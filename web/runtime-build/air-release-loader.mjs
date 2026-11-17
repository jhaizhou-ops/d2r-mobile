// 新增的构建时校验器，本模块不安装或执行编译器。
// Additive, build-time verifier. This module never installs or executes a compiler.
import { lstat, mkdir, mkdtemp, writeFile, rename, rm } from "node:fs/promises";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { gunzipSync } from "node:zlib";
import {
  AIR_RELEASE_OUTPUTS,
  AIR_RELEASE_NOTICE_PATHS,
  verifyFreshAirNotices,
  airReleaseSha256,
  airReleaseFileMaximum,
  safeAirReleasePath,
  parseAirReleaseJson,
  validateFreshAirReleasePin,
  verifyFreshAirManifest,
  verifyFreshAirAsset,
  freshAirAssetUrl,
  verifyFreshAirBuildInputs,
  verifyFreshAirReceipt,
  verifyFreshAirSourceLock,
} from "./air-release-contract.mjs";
import {
  boundedReleaseRead,
  downloadAirReleaseBytes,
  downloadAirForkSourceBytes,
} from "./air-release-transport.mjs";

const pinFile = fileURLToPath(
  new URL("../toolchain/metal-air-compiler/release-pin.json", import.meta.url),
);
const cacheRoot = fileURLToPath(new URL(".build/air-compiler-release/", import.meta.url));
export async function readFreshAirReleasePin() {
  try {
    return validateFreshAirReleasePin(
      parseAirReleaseJson(await boundedReleaseRead(pinFile, 4096), 4096),
    );
  } catch (error) {
    if (error.code !== "ENOENT") throw error;
    throw new Error(
      "Fresh AIR release is not pinned; no placeholder, historical redistribution or compiler-build fallback is allowed",
      { cause: error },
    );
  }
}
async function ensureCache(directory) {
  await mkdir(directory, { recursive: true });
  const info = await lstat(directory);
  if (!info.isDirectory() || info.isSymbolicLink())
    throw new Error("AIR release cache must be a regular directory");
}
async function cachedBytes(path, maximum, verify, url, download) {
  try {
    const bytes = await boundedReleaseRead(path, maximum);
    verify(bytes);
    return bytes;
  } catch (error) {
    if (error.code !== "ENOENT" || !download) throw error;
  }
  const bytes = await downloadAirReleaseBytes(url, maximum);
  verify(bytes);
  const temporary = await mkdtemp(join(dirname(path), ".download-"));
  try {
    await writeFile(join(temporary, "asset"), bytes, { flag: "wx" });
    await rename(join(temporary, "asset"), path);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
  return bytes;
}
export async function readFreshAirReleaseMetadata({ download = false } = {}) {
  const pin = await readFreshAirReleasePin(),
    directory = join(cacheRoot, pin.manifestSha256);
  if (download) await ensureCache(directory);
  const manifestBytes = await cachedBytes(
    join(directory, "manifest.json"),
    1024 ** 2,
    (bytes) => verifyFreshAirManifest(bytes, pin),
    `https://github.com/${pin.repository}/releases/download/${pin.version}/air-wasm-${pin.version}-manifest.json`,
    download,
  );
  const release = verifyFreshAirManifest(manifestBytes, pin);
  const metadata = { pin, release, directory, manifestBytes };
  const { bytes: buildInputsBytes } = await readFreshAirReleaseAsset(metadata, "buildInputs", {
    download,
  });
  return {
    ...metadata,
    buildInputsBytes,
    buildInputs: verifyFreshAirBuildInputs(buildInputsBytes, release),
  };
}
export async function readFreshAirReleaseAsset(metadata, kind, { download = false } = {}) {
  const { release, directory } = metadata,
    asset = release.assets[kind];
  if (!asset) throw new Error("Unknown AIR release asset");
  const path = join(directory, asset.name);
  const bytes = await cachedBytes(
    path,
    asset.bytes,
    (value) => verifyFreshAirAsset(value, release, kind),
    freshAirAssetUrl(release, kind),
    download,
  );
  return { path, bytes };
}
export function unpackFreshAirRuntime(bytes, release, buildInputs) {
  verifyFreshAirAsset(bytes, release, "runtime");
  const raw = gunzipSync(bytes, { maxOutputLength: 192 * 1024 ** 2 }),
    files = new Map();
  const text = (block) =>
    new TextDecoder("utf-8", { fatal: true }).decode(block).replace(/\0.*$/s, "");
  const octal = (block) => {
    const value = text(block).trim();
    if (!/^[0-7]+$/.test(value)) throw new Error("Invalid AIR runtime tar integer");
    return Number.parseInt(value, 8);
  };
  let offset = 0;
  for (; offset + 512 <= raw.length;) {
    const block = raw.subarray(offset, offset + 512);
    if (block.every((byte) => byte === 0)) break;
    const name = text(block.subarray(0, 100)),
      size = octal(block.subarray(124, 136));
    let checksum = 0;
    for (let i = 0; i < 512; i++) checksum += i >= 148 && i < 156 ? 32 : block[i];
    const expected = release.runtimeFiles?.[name];
    if (
      checksum !== octal(block.subarray(148, 156)) ||
      text(block.subarray(257, 263)) !== "ustar" ||
      text(block.subarray(345, 500)) ||
      !safeAirReleasePath(name) ||
      files.has(name) ||
      files.size >= 64 ||
      ![0, 48].includes(block[156]) ||
      text(block.subarray(157, 257)) ||
      !Number.isSafeInteger(size) ||
      size < 1 ||
      size > airReleaseFileMaximum(name) ||
      offset + 512 + size > raw.length ||
      !expected ||
      expected.bytes !== size
    )
      throw new Error("Unsafe, unlisted or oversized AIR runtime archive member");
    const data = raw.subarray(offset + 512, offset + 512 + size);
    if (airReleaseSha256(data) !== expected.sha256)
      throw new Error(`AIR runtime member digest mismatch: ${name}`);
    const paddedEnd = offset + 512 + Math.ceil(size / 512) * 512;
    if (raw.subarray(offset + 512 + size, paddedEnd).some((byte) => byte !== 0))
      throw new Error("Nonzero AIR runtime member padding");
    files.set(name, data);
    offset = paddedEnd;
  }
  if (offset + 1024 > raw.length || raw.subarray(offset).some((byte) => byte !== 0))
    throw new Error("AIR runtime archive has invalid trailing data");
  if (
    JSON.stringify([...files.keys()].sort()) !==
    JSON.stringify(Object.keys(release.runtimeFiles).sort())
  )
    throw new Error("AIR runtime inventory closure mismatch");
  return verifyFreshAirRuntimeFiles(files, release, buildInputs);
}
export function verifyFreshAirRuntimeFiles(files, release, buildInputs) {
  if (
    JSON.stringify([...files.keys()].sort()) !==
    JSON.stringify(Object.keys(release.runtimeFiles).sort())
  )
    throw Error("AIR runtime inventory closure mismatch");
  for (const [name, bytes] of files) {
    const record = release.runtimeFiles[name];
    if (
      bytes.length !== record.bytes ||
      bytes.length > airReleaseFileMaximum(name) ||
      airReleaseSha256(bytes) !== record.sha256
    )
      throw Error(`AIR runtime member digest mismatch: ${name}`);
  }
  const receipt = verifyFreshAirReceipt(files.get("build-report.json"), release, buildInputs);
  const sourceLock = verifyFreshAirSourceLock(files.get("source-lock.json"), release, buildInputs);
  const headerNames = Object.keys(sourceLock.sdkFiles)
    .filter((path) => path.startsWith("api/include/"))
    .map((path) => path.slice(4));
  const requiredNames = [
    ...AIR_RELEASE_OUTPUTS,
    "build-report.json",
    "source-lock.json",
    "air_compiler.h",
    "README.txt",
    "CONTENTS.sha256",
    ...AIR_RELEASE_NOTICE_PATHS,
    ...headerNames,
  ].sort();
  if (JSON.stringify([...files.keys()].sort()) !== JSON.stringify(requiredNames))
    throw new Error("AIR runtime SDK closure mismatch");
  for (const name of ["air_compiler.h", ...headerNames])
    if (airReleaseSha256(files.get(name)) !== sourceLock.sdkFiles[`api/${name}`])
      throw new Error(`AIR SDK header differs from built source: ${name}`);
  const contents = [...files]
    .filter(([name]) => name !== "CONTENTS.sha256")
    .sort(([a], [b]) => (a < b ? -1 : 1))
    .map(([name, data]) => `${airReleaseSha256(data)}  ${name}\n`)
    .join("");
  if (files.get("CONTENTS.sha256").toString("utf8") !== contents)
    throw new Error("AIR runtime contents digest mismatch");
  const noticeOrigins = verifyFreshAirNotices(files, release);
  return { files, receipt, sourceLock, noticeOrigins };
}
export async function loadFreshAirRelease({ download = false } = {}) {
  const metadata = await readFreshAirReleaseMetadata({ download });
  // 即使直接调用导入器，完整且经过认证的源码也是前提。
  // Complete authenticated source is a prerequisite even for direct importer callers.
  const source = await readFreshAirReleaseAsset(metadata, "correspondingSource", { download });
  const runtime = await readFreshAirReleaseAsset(metadata, "runtime", { download });
  return {
    ...metadata,
    source,
    runtime,
    ...unpackFreshAirRuntime(runtime.bytes, metadata.release, metadata.buildInputs),
  };
}
// 源码不变量检查实际构建的精确输入，不检查补丁文本或未固定的分支。
// Source invariants inspect exact built inputs, never patch text or an unpinned branch.
export async function readFreshAirForkSource(path) {
  const metadata = await readFreshAirReleaseMetadata({ download: true });
  const digest =
    path === "wasm/source-lock.json"
      ? metadata.release.sourceLockSha256
      : (metadata.buildInputs.sourceInputs[path] ?? metadata.buildInputs.tooling[path]);
  if (!safeAirReleasePath(path) || !/^[a-f0-9]{64}$/.test(digest ?? ""))
    throw Error("AIR source path is not a pinned build input");
  const target = join(metadata.directory, `source-${digest}`),
    maximum = 8 * 1024 ** 2;
  let bytes;
  try {
    bytes = await boundedReleaseRead(target, maximum);
  } catch (error) {
    if (error.code !== "ENOENT") throw error;
    bytes = await downloadAirForkSourceBytes(
      `https://raw.githubusercontent.com/${metadata.release.repository}/${metadata.release.commit}/${path}`,
      maximum,
    );
    if (airReleaseSha256(bytes) !== digest) throw Error("Pinned AIR fork source digest mismatch");
    const staging = await mkdtemp(join(metadata.directory, ".source-"));
    try {
      await writeFile(join(staging, "input"), bytes, { flag: "wx" });
      await rename(join(staging, "input"), target);
    } finally {
      await rm(staging, { recursive: true, force: true });
    }
  }
  if (airReleaseSha256(bytes) !== digest) throw Error("Pinned AIR fork source digest mismatch");
  return bytes;
}
