// 生成并验证运行时传输清单与负载；依据 ../runtime-asset-groups.mjs、../air-compiler-runtime-contract.mjs 的模块与调用约定。
// Create and validate runtime transport manifests and payloads using the module and calling contracts in ../runtime-asset-groups.mjs、../air-compiler-runtime-contract.mjs.
import { Buffer } from "node:buffer";
import { createHash } from "node:crypto";
import { gzipSync, gunzipSync } from "node:zlib";
import {
  validateRuntimeAssetGroups,
  validateRuntimeTransportGroups,
} from "../runtime-asset-groups.mjs";
import {
  AIR_COMPILER_RUNTIME,
  runtimeAssetMaximumBytes,
  runtimeTransportMaximumBytes,
} from "../air-compiler-runtime-contract.mjs";

const maximumFile = 64 * 1024 * 1024;
const maximumTotal = 512 * 1024 * 1024;
const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
const contentKey = (record) => `${record.sha256}:${record.bytes}`;
const validSize = (value, cap = maximumFile) =>
  Number.isSafeInteger(value) && value > 0 && value <= cap;

// 传输与逻辑路径清单分离：工作线程清单和相对路径保持不变，
// 相同的公开字节只传输一次。
// Transport is separate from the logical path inventory: worker manifests and
// relative paths stay unchanged, while identical public bytes travel only once.
export function createRuntimeTransport(contents, { assets } = {}) {
  const ownership = assets === undefined ? undefined : validateRuntimeAssetGroups(assets);
  const compiler = assets?.find((asset) => asset.path === AIR_COMPILER_RUNTIME.wasm);
  if (
    ownership &&
    (ownership.size !== contents.size ||
      [...contents.values()].some((record) => !ownership.has(contentKey(record))))
  )
    throw new Error("Runtime transport grouping inventory mismatch");
  const groupOf = (record) => ownership?.get(contentKey(record)) ?? "";
  const files = [],
    entries = [],
    payloads = new Map();
  let pending = [],
    pendingBytes = 0,
    pendingGroup = "";
  function flush(group) {
    if (!group.length) return;
    const decoded = Buffer.concat(group.map((record) => record.data));
    const compressed = gzipSync(decoded, { level: 9 });
    const encoding = compressed.length < decoded.length ? "gzip" : "identity";
    const bytes = encoding === "gzip" ? compressed : decoded;
    const digest = sha(bytes);
    const path = `payloads/${digest}.${encoding === "gzip" ? "gz" : "bin"}`;
    if (payloads.has(path)) throw new Error("Duplicate transport payload");
    payloads.set(path, bytes);
    files.push({
      path,
      bytes: bytes.length,
      sha256: digest,
      encoding,
      decodedBytes: decoded.length,
    });
    let offset = 0;
    for (const record of group) {
      entries.push({ sha256: record.sha256, bytes: record.bytes, file: path, offset });
      offset += record.bytes;
    }
  }
  for (const record of [...contents.values()].sort(
    (a, b) => groupOf(a).localeCompare(groupOf(b)) || a.sha256.localeCompare(b.sha256),
  )) {
    if (
      !validSize(
        record.bytes,
        compiler && contentKey(record) === contentKey(compiler)
          ? runtimeAssetMaximumBytes(compiler.path)
          : maximumFile,
      ) ||
      record.data.length !== record.bytes ||
      sha(record.data) !== record.sha256
    )
      throw new Error("Invalid transport source bytes");
    const group = groupOf(record);
    if (group !== pendingGroup) {
      flush(pending);
      pending = [];
      pendingBytes = 0;
      pendingGroup = group;
    }
    if (record.bytes > 256 * 1024) {
      flush([record]);
    } else {
      if (pendingBytes + record.bytes > 2 * 1024 * 1024) {
        flush(pending);
        pending = [];
        pendingBytes = 0;
      }
      pending.push(record);
      pendingBytes += record.bytes;
    }
  }
  flush(pending);
  files.sort((a, b) => a.path.localeCompare(b.path));
  entries.sort((a, b) => a.sha256.localeCompare(b.sha256));
  const transport = {
    version: 1,
    totalBytes: files.reduce((total, file) => total + file.bytes, 0),
    files,
    entries,
  };
  return { transport, payloads };
}

export function validateRuntimeTransport(manifest) {
  validateRuntimeAssetGroups(manifest.assets);
  const transport = manifest.transport;
  if (
    transport?.version !== 1 ||
    !Array.isArray(transport.files) ||
    !transport.files.length ||
    transport.files.length > 4096 ||
    !Array.isArray(transport.entries) ||
    !transport.entries.length ||
    transport.entries.length > 4096 ||
    !validSize(transport.totalBytes, maximumTotal)
  )
    throw new Error("Invalid runtime transport contract");
  const files = new Map(),
    expected = new Map(manifest.assets.map((entry) => [contentKey(entry), entry]));
  let total = 0,
    decodedTotal = 0;
  for (const file of transport.files) {
    const maximum = runtimeTransportMaximumBytes(file, manifest);
    if (
      !/^[a-f0-9]{64}$/.test(file.sha256) ||
      !["identity", "gzip"].includes(file.encoding) ||
      file.path !== `payloads/${file.sha256}.${file.encoding === "gzip" ? "gz" : "bin"}` ||
      files.has(file.path) ||
      !validSize(file.bytes, maximum) ||
      !validSize(file.decodedBytes, maximum) ||
      (file.encoding === "identity" && file.bytes !== file.decodedBytes)
    )
      throw new Error("Invalid runtime transport file");
    total += file.bytes;
    decodedTotal += file.decodedBytes;
    files.set(file.path, { ...file, entries: [] });
  }
  if (total !== transport.totalBytes || decodedTotal > maximumTotal)
    throw new Error("Runtime transport total mismatch");
  const seen = new Set();
  for (const entry of transport.entries) {
    const key = contentKey(entry),
      file = files.get(entry.file);
    if (
      !validSize(entry.bytes, runtimeAssetMaximumBytes(expected.get(key)?.path)) ||
      !/^[a-f0-9]{64}$/.test(entry.sha256) ||
      !expected.has(key) ||
      seen.has(key) ||
      !file ||
      !Number.isSafeInteger(entry.offset) ||
      entry.offset < 0 ||
      entry.offset + entry.bytes > file.decodedBytes
    )
      throw new Error("Invalid runtime transport entry");
    seen.add(key);
    file.entries.push(entry);
  }
  if (seen.size !== expected.size) throw new Error("Incomplete logical runtime transport");
  validateRuntimeTransportGroups(manifest.assets, transport.entries);
  for (const file of files.values()) {
    let offset = 0;
    for (const entry of file.entries.sort((a, b) => a.offset - b.offset)) {
      if (entry.offset !== offset) throw new Error("Overlapping or incomplete runtime pack");
      offset += entry.bytes;
    }
    if (offset !== file.decodedBytes) throw new Error("Unused runtime transport bytes");
  }
  return files;
}

export function verifyTransportPayload(file, bytes) {
  if (bytes.length !== file.bytes || sha(bytes) !== file.sha256)
    throw new Error("Runtime transport digest mismatch");
  const decoded =
    file.encoding === "gzip" ? gunzipSync(bytes, { maxOutputLength: file.decodedBytes }) : bytes;
  if (decoded.length !== file.decodedBytes)
    throw new Error("Runtime transport decoded size mismatch");
  for (const entry of file.entries) {
    if (sha(decoded.subarray(entry.offset, entry.offset + entry.bytes)) !== entry.sha256)
      throw new Error("Runtime logical asset digest mismatch");
  }
}
