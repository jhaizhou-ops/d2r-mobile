// 依据原生 casc_overlay 清单规则和现有资源工作线程写入 CASC 增量。
// Write CASC deltas using native casc_overlay manifest rules and the existing asset worker.
/**
 * 真实有界 CASC 增量写入器。原始归档保持惰性读取，绝不克隆。这是内部原语，不是生产 UI 或完整资源流水线。
 * Real bounded CASC delta writer. Original archives stay lazy, never cloned.
 * This is an internal primitive, not a production UI or a complete asset pipeline.
 */
import { readAuthenticatedAssets } from "./extraction-job-adapter.mjs";
import { archiveLocator, decodeArchiveRecord, ARCHIVE_LIMITS } from "./archive-reader.mjs";
import { verifyMd5 } from "./checksum-verifier.mjs";
import { createMD5 } from "hash-wasm";
import { ROMFS_LIMITS } from "./romfs-tree.mjs";
import { CASC_LIMITS } from "./casc-logical-reader.mjs";
import { CASC_PACK_METADATA_LIMITS } from "./toolchain/casc-overlay-js/overlay.mjs";

export const OVERLAY_LIMITS = Object.freeze({
  config: 1024 * 1024,
  input: 64 * 1024 * 1024,
  output: 64 * 1024 * 1024,
  replacement: 16 * 1024 * 1024,
  jobs: 4096,
});
const fail = () => {
  throw new Error("CASC_OVERLAY_INVALID");
};
const encoder = new TextEncoder();
function normalize(value) {
  if (typeof value !== "string") fail();
  let path = value.trim().replaceAll("\\", "/");
  while (/^data:/i.test(path)) path = path.slice(5);
  path = path.replace(/^\/+/, "").toLowerCase();
  if (
    !path ||
    encoder.encode(path).length > 4096 ||
    // eslint-disable-next-line no-control-regex
    /[\u0000-\u001f\u007f:]/u.test(path) ||
    path.split("/").some((p) => !p || p === "." || p === "..")
  )
    fail();
  return path;
}
async function exact(file, offset, count, signal) {
  signal?.throwIfAborted();
  if (
    !file ||
    typeof file.size !== "bigint" ||
    typeof file.readAt !== "function" ||
    offset < 0n ||
    offset + BigInt(count) > file.size
  )
    fail();
  const result = new Uint8Array(count);
  for (let p = 0; p < count; p += ROMFS_LIMITS.readBytes) {
    const size = Math.min(ROMFS_LIMITS.readBytes, count - p);
    const bytes = await file.readAt(offset + BigInt(p), size, { signal });
    signal?.throwIfAborted();
    if (!(bytes instanceof Uint8Array) || bytes.length !== size) fail();
    result.set(bytes, p);
  }
  return result;
}
async function smallFile(romfs, path, limit, signal) {
  const file = romfs.getFile(path);
  if (!file || file.size > BigInt(limit)) fail();
  return exact(file, 0n, Number(file.size), signal);
}
function configValues(bytes) {
  const values = new Map();
  for (const line of new TextDecoder("utf-8", { fatal: true }).decode(bytes).split("\n")) {
    const at = line.indexOf(" = ");
    if (at < 0 || line.startsWith("#")) continue;
    const key = line.slice(0, at);
    if (values.has(key)) fail();
    values.set(key, line.slice(at + 3));
  }
  return values;
}
async function tvfs(romfs, values, name, signal) {
  const keys = values.get(name)?.split(" ");
  const sizes = values.get(`${name}-size`)?.split(" ");
  if (
    keys?.length !== 2 ||
    keys.some((k) => !/^[a-f0-9]{32}$/i.test(k)) ||
    sizes?.length !== 2 ||
    sizes.some((n) => !/^\d+$/.test(n))
  )
    fail();
  const [decodedSize, encodedSize] = sizes.map(Number);
  if (
    !Number.isSafeInteger(decodedSize) ||
    !Number.isSafeInteger(encodedSize) ||
    decodedSize > ARCHIVE_LIMITS.decodedRecord ||
    encodedSize > ARCHIVE_LIMITS.encodedRecord
  )
    fail();
  const locator = archiveLocator(keys[1]);
  const file = romfs.getFile(`data/${locator.filename}`);
  const start = BigInt(locator.offset);
  if (!file || start + BigInt(encodedSize) > file.size) fail();
  const read = (offset, count) => exact(file, start + BigInt(offset), count, signal);
  const prefix = await read(0, Math.min(12, encodedSize));
  if (prefix.length < 12 || String.fromCharCode(...prefix.slice(0, 4)) !== "BLTE") fail();
  const headerSize = new DataView(prefix.buffer).getUint32(4);
  if (headerSize < 12 || headerSize > encodedSize || headerSize > 12 + 24 * ARCHIVE_LIMITS.blocks)
    fail();
  let hash = await createMD5();
  let digest;
  try {
    digest = hash.update(await read(0, headerSize)).digest();
  } finally {
    hash = null;
  }
  if (digest.slice(0, 18) !== keys[1].slice(0, 18).toLowerCase()) fail();
  return decodeArchiveRecord(
    { size: encodedSize, read },
    { offset: 0, encodedSize, decodedSize, encoding: "b:256K*=z", ckey: keys[0] },
    { verifyMd5, signal },
  );
}
// 从绑定的认证资源读取并验证原始 TVFS；调用方不能提供替代字节。
// Read and verify original TVFS from bound authenticated assets; callers cannot substitute bytes.
export async function readAuthenticatedCascVfs(extracted, selection, { signal } = {}) {
  const { romfs } = readAuthenticatedAssets(extracted, selection, { signal });
  const config = await smallFile(romfs, "data/.build.config", OVERLAY_LIMITS.config, signal);
  const bytes = await tvfs(romfs, configValues(config), "vfs-1", signal);
  readAuthenticatedAssets(extracted, selection, { signal });
  return bytes;
}

/**
 * 与原生 casc_overlay.read_manifest 完全一致，首个重复项优先，而非 dmx 的规则。
 * First duplicate wins exactly as native casc_overlay.read_manifest, not dmx.
 */
async function selectedManifest(romfs, wanted, signal) {
  const file = romfs.getFile("data/build_manifest.psv");
  if (!file || file.size > BigInt(CASC_LIMITS.manifestBytes)) fail();
  const selected = new Map();
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let pending = "",
    rows = 0;
  const line = (text) => {
    if (encoder.encode(text).length > CASC_LIMITS.lineBytes) fail();
    const fields = text.split("|");
    if (fields.length < 11 || fields[0].startsWith("#")) return;
    if (++rows > CASC_LIMITS.rows) fail();
    // 不拒绝原生代码从不使用的无关路径或记录。
    // Do not reject unrelated paths/records that native never uses.
    let path;
    try {
      path = normalize(fields[9]);
    } catch {
      return;
    }
    if (wanted.has(path) && !selected.has(path)) {
      if (!/^[a-f0-9]{32}$/i.test(fields[6])) fail();
      selected.set(path, fields[6]);
    }
  };
  for (let pos = 0n; pos < file.size;) {
    const length = Number(
      file.size - pos > BigInt(ROMFS_LIMITS.readBytes)
        ? BigInt(ROMFS_LIMITS.readBytes)
        : file.size - pos,
    );
    pending += decoder.decode(await exact(file, pos, length, signal), { stream: true });
    pos += BigInt(length);
    const lines = pending.split("\n");
    pending = lines.pop();
    for (const text of lines) line(text);
    if (pending.length > CASC_LIMITS.lineBytes) fail();
  }
  pending += decoder.decode();
  if (pending) line(pending);
  return selected;
}
const manifestSelections = new WeakMap();

// 只读取一次已认证的源清单，以此解析两种覆盖层变体。
// 不透明的选择结果仍绑定到确切的 RomFS 对象和请求路径。
// Resolve both overlay variants against one read of the authenticated source manifest.
// The opaque selection remains bound to the exact RomFS object and requested paths.
export async function prepareCascOverlayManifest({ romfs, paths, signal } = {}) {
  if (!romfs || typeof romfs.getFile !== "function" || !Array.isArray(paths)) fail();
  const wanted = new Set(paths.map(normalize));
  const records = await selectedManifest(romfs, wanted, signal);
  const selection = Object.freeze({});
  manifestSelections.set(selection, { romfs, wanted, records });
  return selection;
}
const packName = ([chunk, archive]) =>
  `${String(chunk).padStart(2, "0")}-${String(archive).padStart(8, "0")}.data`;
const validLocation = (value) =>
  Array.isArray(value) &&
  value.length === 2 &&
  value.every((n) => Number.isSafeInteger(n) && n >= 0 && n <= 255);

/**
 * 根据真实源清单先预留默认覆盖层位置，再选择补充包；不是并发文件系统锁或内容认证。
 * Reserve the default overlay location from real source inventory before selecting a supplemental pack; not a concurrent filesystem lock or content authentication.
 */
export function reserveCascPackLocations(romfs, { packLocation } = {}) {
  if (!romfs || !Array.isArray(romfs.files) || typeof romfs.getFile !== "function") fail();
  const used = new Set(romfs.files.map((file) => file.path));
  const next = () => {
    for (let chunk = 0; chunk < 256; chunk++)
      for (let archive = 1; archive < 256; archive++) {
        const value = [chunk, archive];
        if (!used.has(`data/${packName(value)}`) && !romfs.getFile(`data/${packName(value)}`))
          return value;
      }
    fail();
  };
  const overlayLocation = next();
  used.add(`data/${packName(overlayLocation)}`);
  const selected = packLocation === undefined ? next() : packLocation;
  if (
    !validLocation(selected) ||
    used.has(`data/${packName(selected)}`) ||
    romfs.getFile(`data/${packName(selected)}`)
  )
    fail();
  return Object.freeze({
    overlayLocation: Object.freeze(overlayLocation),
    packLocation: Object.freeze([...selected]),
    packName: packName(selected),
  });
}

function externalReferences(external) {
  const pack = external?.pack,
    records = external?.records;
  if (
    typeof pack?.name !== "string" ||
    pack.name.length > 17 ||
    !Number.isSafeInteger(pack.size) ||
    pack.size < 1 ||
    pack.size > 2 ** 40 ||
    typeof pack.sha256 !== "string" ||
    !/^[0-9a-f]{64}$/.test(pack.sha256) ||
    !Array.isArray(records) ||
    records.length < 1 ||
    records.length > CASC_PACK_METADATA_LIMITS.entries
  )
    fail();
  const match = /^(\d{2,3})-(\d{8})\.data$/.exec(pack.name);
  const location = match && [Number(match[1]), Number(match[2])];
  if (!validLocation(location) || packName(location) !== pack.name) fail();
  let pathBytes = 0;
  const paths = new Set(),
    keys = new Set();
  const checked = records.map((record) => {
    if (
      typeof record?.path !== "string" ||
      record.path.length > 4096 ||
      record.path.startsWith("+")
    )
      fail();
    const path = normalize(record.path);
    if (
      path !== record.path ||
      paths.has(path) ||
      typeof record.oldEkey !== "string" ||
      !/^[0-9a-f]{32}$/.test(record.oldEkey) ||
      typeof record.newEkey !== "string" ||
      !/^[0-9a-f]{32}$/.test(record.newEkey) ||
      keys.has(record.oldEkey)
    )
      fail();
    pathBytes += encoder.encode(path).length;
    if (pathBytes > CASC_PACK_METADATA_LIMITS.pathBytes) fail();
    paths.add(path);
    keys.add(record.oldEkey);
    for (const size of [record.encodedSize, record.decodedSize])
      if (!Number.isSafeInteger(size) || size < 0 || size > 0xffffffff) fail();
    const chunk = Number.parseInt(record.newEkey.slice(18, 20), 16),
      archive = Number.parseInt(record.newEkey.slice(20, 22), 16),
      offset = Number.parseInt(record.newEkey.slice(22), 16);
    if (chunk !== location[0] || archive !== location[1] || offset + record.encodedSize > pack.size)
      fail();
    return {
      path,
      oldEkey: record.oldEkey,
      newEkey: record.newEkey,
      encodedSize: record.encodedSize,
      decodedSize: record.decodedSize,
    };
  });
  return {
    pack: { name: pack.name, size: pack.size, sha256: pack.sha256 },
    records: checked,
    location,
  };
}

/**
 * 原有默认入口不接受外部引用，保持其空任务行为和字节输出。
 * The original default entry accepts no external references and preserves empty-job behavior and byte output.
 */
export async function writeCascOverlay({
  romfs,
  replacements,
  worker,
  signal,
  manifestSelection,
} = {}) {
  return writeOverlay({ romfs, replacements, worker, signal, manifestSelection });
}

/**
 * 专用内部入口：引用元数据走单独有界载荷段，不扩大原选项、覆盖层或替换内容上限。
 * Dedicated internal entry: reference metadata uses a separately bounded payload segment without raising original options, overlay or replacement limits.
 */
export async function writeCascPrebuiltOverlay({
  romfs,
  replacements = [],
  external,
  worker,
  signal,
  manifestSelection,
} = {}) {
  if (!external) return Promise.reject(new Error("CASC_OVERLAY_INVALID"));
  return writeOverlay({ romfs, replacements, external, worker, signal, manifestSelection });
}

async function writeOverlay({
  romfs,
  replacements,
  external,
  worker,
  signal,
  manifestSelection,
} = {}) {
  signal?.throwIfAborted();
  if (
    !romfs ||
    typeof romfs.getFile !== "function" ||
    !Array.isArray(romfs.files) ||
    !worker ||
    typeof worker.run !== "function" ||
    typeof worker.close !== "function" ||
    !Array.isArray(replacements) ||
    replacements.length > OVERLAY_LIMITS.jobs
  )
    fail();
  const prebuilt = external === undefined ? undefined : externalReferences(external);
  const seen = new Set();
  let total = 0;
  const jobs = replacements.map((job) => {
    const path = normalize(job?.path);
    if (
      seen.has(path) ||
      !(job.bytes instanceof Uint8Array) ||
      job.bytes.length > OVERLAY_LIMITS.replacement ||
      (job.add !== undefined && typeof job.add !== "boolean")
    )
      fail();
    seen.add(path);
    total += job.bytes.length;
    if (total > OVERLAY_LIMITS.input) fail();
    return { path, add: job.add ?? false, bytes: job.bytes };
  });
  if (!jobs.length && !prebuilt)
    return Object.freeze({ files: Object.freeze({}), inventory: Object.freeze([]) });
  let location;
  if (prebuilt) {
    location = reserveCascPackLocations(romfs, { packLocation: prebuilt.location }).overlayLocation;
    for (const record of prebuilt.records) {
      if (seen.has(record.path)) fail();
      seen.add(record.path);
    }
  } else {
    const used = new Set(romfs.files.map((f) => f.path));
    outer: for (let chunk = 0; chunk < 256; chunk++)
      for (let archive = 1; archive < 256; archive++) {
        const name = `data/${String(chunk).padStart(2, "0")}-${String(archive).padStart(8, "0")}.data`;
        if (!used.has(name)) {
          location = [chunk, archive];
          break outer;
        }
      }
    if (!location) fail();
  }
  const config = await smallFile(romfs, "data/.build.config", OVERLAY_LIMITS.config, signal);
  const values = configValues(config);
  const prepared =
    manifestSelection === undefined ? undefined : manifestSelections.get(manifestSelection);
  if (
    manifestSelection !== undefined &&
    (!prepared || prepared.romfs !== romfs || [...seen].some((path) => !prepared.wanted.has(path)))
  )
    fail();
  const manifest = prepared ? prepared.records : await selectedManifest(romfs, seen, signal);
  const descriptors = jobs.map((job) => {
    const oldEkey = manifest.get(job.path);
    if (job.add ? oldEkey !== undefined : oldEkey === undefined) fail();
    return { path: job.path, add: job.add, ...(!job.add ? { oldEkey } : {}) };
  });
  if (prebuilt)
    for (const record of prebuilt.records)
      if (manifest.get(record.path)?.toLowerCase() !== record.oldEkey) fail();
  const vfs = await tvfs(romfs, values, "vfs-1", signal);
  const root = await tvfs(romfs, values, "vfs-root", signal);
  const buffers = [config, vfs, root, ...jobs.map((j) => j.bytes)];
  if (prebuilt) {
    const references = encoder.encode(
      JSON.stringify({ pack: prebuilt.pack, records: prebuilt.records }),
    );
    if (references.length > 16 * 1024 * 1024) fail();
    buffers.push(references);
  }
  const parts = buffers.map((b) => b.length);
  total = parts.reduce((a, b) => a + b, 0);
  if (total > OVERLAY_LIMITS.input) fail();
  const payload = new Uint8Array(total);
  let pos = 0;
  for (const bytes of buffers) {
    payload.set(bytes, pos);
    pos += bytes.length;
  }
  const abort = () => worker.close();
  signal?.addEventListener("abort", abort, { once: true });
  try {
    signal?.throwIfAborted();
    const result = await worker.run(prebuilt ? "casc-prebuilt-overlay" : "casc-overlay", payload, {
      parts,
      jobs: descriptors,
      location,
    });
    signal?.throwIfAborted();
    const archive = `data/${String(location[0]).padStart(2, "0")}-${String(location[1]).padStart(8, "0")}.data`;
    const names = Object.keys(result?.files ?? {}).sort();
    if (
      names.length !== 2 ||
      names[0] !== "data/.build.config" ||
      names[1] !== archive ||
      !Array.isArray(result.inventory) ||
      result.inventory.length !== jobs.length
    )
      fail();
    if (
      prebuilt &&
      (result.external?.count !== prebuilt.records.length ||
        result.external?.pack?.name !== prebuilt.pack.name ||
        result.external.pack.size !== prebuilt.pack.size ||
        result.external.pack.sha256 !== prebuilt.pack.sha256)
    )
      fail();
    total = 0;
    const files = {};
    for (const name of names) {
      const bytes = result.files[name];
      if (!(bytes instanceof ArrayBuffer)) fail();
      total += bytes.byteLength;
      if (total > OVERLAY_LIMITS.output) fail();
      files[name] = new Uint8Array(bytes);
    }
    return Object.freeze({
      files: Object.freeze(files),
      inventory: Object.freeze(result.inventory),
      ...(prebuilt
        ? {
            external: Object.freeze({
              count: prebuilt.records.length,
              pack: Object.freeze({ ...prebuilt.pack }),
            }),
          }
        : {}),
    });
  } finally {
    payload.fill(0);
    vfs.fill(0);
    root.fill(0);
    signal?.removeEventListener("abort", abort);
  }
}
