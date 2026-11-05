// 校验运行时清单、资产大小及完整性；依据 ./transport.mjs、../runtime-asset-groups.mjs 的模块与调用约定。
// Verify runtime manifests, asset sizes and integrity using the module and calling contracts in ./transport.mjs、../runtime-asset-groups.mjs.
import { validateRuntimeTransport, verifyTransportPayload } from "./transport.mjs";
import { validateRuntimeAssetGroups } from "../runtime-asset-groups.mjs";
import { Buffer } from "node:buffer";
import { readdir, readFile, lstat } from "node:fs/promises";
import { createHash } from "node:crypto";
import {
  validateAirCompilerRuntimeComponent,
  runtimeAssetMaximumBytes,
} from "../air-compiler-runtime-contract.mjs";

const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
export async function verifyRuntimeInventory(directory, expectedDigest, expectedCommit = null) {
  if (!/^[a-f0-9]{64}$/.test(expectedDigest || ""))
    throw new Error("Trusted runtime manifest digest required");
  const rootInfo = await lstat(directory);
  if (!rootInfo.isDirectory() || rootInfo.isSymbolicLink())
    throw new Error("Runtime directory must not be a symlink");
  const manifestUrl = new URL("runtime-manifest.json", directory);
  const manifestInfo = await lstat(manifestUrl);
  if (
    !manifestInfo.isFile() ||
    manifestInfo.isSymbolicLink() ||
    manifestInfo.size > 4 * 1024 * 1024
  )
    throw new Error("Invalid runtime manifest file");
  const raw = await readFile(manifestUrl);
  if (sha(raw) !== expectedDigest) throw new Error("Runtime manifest digest mismatch");
  const manifest = JSON.parse(raw);
  if (
    manifest.version !== 1 ||
    manifest.productionEnabled !== false ||
    !Array.isArray(manifest.assets) ||
    !manifest.assets.length ||
    manifest.assets.length > 4096 ||
    !Number.isSafeInteger(manifest.totalBytes) ||
    manifest.totalBytes > 512 * 1024 * 1024
  )
    throw new Error("Invalid runtime manifest contract");
  if (expectedCommit && manifest.build?.sourceRevision !== expectedCommit)
    throw new Error("Runtime source revision mismatch");
  const records = new Map(),
    folded = new Set();
  let total = 0;
  for (const entry of manifest.assets) {
    if (
      typeof entry.path !== "string" ||
      entry.path.length > 512 ||
      !/^[A-Za-z0-9_.+/-]+$/.test(entry.path) ||
      entry.path.split("/").some((part) => !part || part === "." || part === "..") ||
      folded.has(entry.path.toLowerCase()) ||
      ["runtime-manifest.json", "runtime-integrity.mjs"].includes(entry.path.toLowerCase()) ||
      !Number.isSafeInteger(entry.bytes) ||
      entry.bytes < 1 ||
      entry.bytes > runtimeAssetMaximumBytes(entry.path) ||
      !/^[a-f0-9]{64}$/.test(entry.sha256) ||
      typeof entry.source !== "string" ||
      entry.source.length > 512 ||
      !/^[A-Za-z0-9_.+/-]+$/.test(entry.source) ||
      entry.source.split("/").some((part) => !part || part === "." || part === "..")
    )
      throw new Error("Invalid runtime inventory record");
    records.set(entry.path, entry);
    folded.add(entry.path.toLowerCase());
    total += entry.bytes;
  }
  if (total !== manifest.totalBytes) throw new Error("Runtime total mismatch");
  validateRuntimeAssetGroups(manifest.assets);
  validateAirCompilerRuntimeComponent(manifest);
  const transportFiles = manifest.transport ? validateRuntimeTransport(manifest) : null;
  if (transportFiles) {
    records.clear();
    for (const [path, record] of transportFiles) records.set(path, record);
  }
  const integrity = `export const runtimeManifestSha256 = ${JSON.stringify(expectedDigest)};\nexport const productionEnabled = false;\n`;
  for (const [path, bytes] of [
    ["runtime-manifest.json", raw],
    ["runtime-integrity.mjs", Buffer.from(integrity)],
  ]) {
    if (records.has(path)) throw new Error("Reserved runtime metadata path");
    records.set(path, { path, bytes: bytes.length, sha256: sha(bytes) });
  }
  const found = [];
  let directories = 0;
  async function walk(url, prefix = "") {
    if (++directories > 8192) throw new Error("Runtime directory count cap");
    for (const name of await readdir(url)) {
      const path = prefix + name,
        child = new URL(name, url),
        info = await lstat(child);
      if (info.isSymbolicLink()) throw new Error("Runtime symlink rejected");
      if (info.isDirectory()) {
        if (![...records.keys()].some((entry) => entry.startsWith(path + "/")))
          throw new Error("Unexpected runtime directory");
        await walk(new URL(name + "/", url), path + "/");
      } else {
        const record = records.get(path);
        if (!info.isFile() || !record || info.size !== record.bytes)
          throw new Error("Unexpected runtime file or size");
        const bytes = await readFile(child);
        if (sha(bytes) !== record.sha256) throw new Error("Runtime payload digest mismatch");
        if (transportFiles?.has(path)) verifyTransportPayload(record, bytes);
        found.push({ path, bytes: bytes.length, sha256: record.sha256 });
      }
    }
  }
  await walk(directory);
  if (found.length !== records.size) throw new Error("Incomplete runtime payload");
  return {
    manifest,
    files: found.sort((a, b) => a.path.localeCompare(b.path)),
    sha256: expectedDigest,
  };
}
