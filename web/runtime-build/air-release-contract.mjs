// 仅接受新的 fork 构建，绝不重贴或重写历史收据。
// Fresh fork builds only. Historical receipts are never relabelled or rewritten.
import { createHash } from "node:crypto";
import { isDeepStrictEqual } from "node:util";
import { AIR_COMPILER_LIMITS } from "../air-compiler-contract.mjs";

export const AIR_RELEASE_CAPS = Object.freeze({
  runtime: 128 * 1024 ** 2,
  correspondingSource: 256 * 1024 ** 2,
  buildInputs: 1024 ** 2,
});
export const AIR_RELEASE_OUTPUTS = Object.freeze(["air_wasm.mjs", "air_wasm.wasm"]);
const repositoryNotices = Object.freeze([
  "llvm/LICENSE.TXT",
  "clang/LICENSE.TXT",
  "llvm/include/llvm/Support/LICENSE.TXT",
  "llvm/include/llvm/Transforms/LibFloor/cfg/LICENSE",
  "llvm/lib/Transforms/LibFloor/cfg/LICENSE",
  "llvm/lib/Bitcode/MetalLib/bzip2/LICENSE",
  "llvm/lib/Bitcode/MetalLib/tar/LICENSE",
]);
const sdkNotices = Object.freeze([
  "LICENSE",
  "AUTHORS",
  "system/lib/compiler-rt/LICENSE.TXT",
  "system/lib/libc/musl/COPYRIGHT",
  "system/lib/libcxx/LICENSE.TXT",
  "system/lib/libcxxabi/LICENSE.TXT",
  "system/lib/libunwind/LICENSE.TXT",
]);
const gplNotice = Object.freeze({
  sourceUrl:
    "https://gcc.gnu.org/git/?p=gcc.git;a=blob_plain;f=COPYING;hb=62c888a8f13ddc7184a741dc849fd62e5a2b5962",
  bytes: 18002,
  sha256: "231f7edcc7352d7734a96eef0b8030f77982678c516876fcb81e25b32d68564c",
});
export const AIR_RELEASE_NOTICE_PATHS = Object.freeze([
  ...repositoryNotices.map((path) => `licenses/${path}`),
  ...sdkNotices.map((path) => `licenses/emscripten/${path}`),
  "licenses/GPL-2.0.txt",
  "licenses/ORIGINS.json",
]);
const SHA = (v) => typeof v === "string" && /^[a-f0-9]{64}$/.test(v);
const GIT = (v) => typeof v === "string" && /^[a-f0-9]{40}$/.test(v);
const REPO = (v) =>
  typeof v === "string" && /^[A-Za-z0-9][A-Za-z0-9_.-]*\/[A-Za-z0-9][A-Za-z0-9_.-]*$/.test(v);
const VERSION = (v) => typeof v === "string" && /^v\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$/.test(v);
const object = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const exact = (v, keys) =>
  object(v) && Object.keys(v).sort().join("\0") === [...keys].sort().join("\0");
const requireValue = (ok, message) => {
  if (!ok) throw new Error(message);
};
export const airReleaseSha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");
export const safeAirReleasePath = (path) =>
  typeof path === "string" &&
  path.length <= 100 &&
  /^[A-Za-z0-9_.+/-]+$/.test(path) &&
  !path.split("/").some((part) => !part || part === "." || part === "..");
export function airReleaseFileMaximum(name) {
  if (name === "air_wasm.wasm") return AIR_COMPILER_LIMITS.wasmBytes;
  if (name === "air_wasm.mjs") return AIR_COMPILER_LIMITS.factoryBytes;
  return 1024 ** 2;
}
function fileRecord(record, cap) {
  return (
    exact(record, ["bytes", "sha256"]) &&
    Number.isSafeInteger(record.bytes) &&
    record.bytes > 0 &&
    record.bytes <= cap &&
    SHA(record.sha256)
  );
}
export function parseAirReleaseJson(bytes, maximum = 1024 ** 2) {
  requireValue(bytes.byteLength > 0 && bytes.byteLength <= maximum, "AIR release JSON exceeds cap");
  const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  const result = JSON.parse(text);
  // JSON.parse 本身会静默接受重复键，因此拒绝含糊的已签名元数据。
  // JSON.parse alone silently accepts duplicate keys. Reject ambiguous signed metadata.
  let at = 0;
  const space = () => {
    while (/\s/.test(text[at] ?? "") && at < text.length) at++;
  };
  function string() {
    const start = at++;
    while (at < text.length) {
      if (text[at++] === "\\") at++;
      else if (text[at - 1] === '"') return JSON.parse(text.slice(start, at));
    }
    throw new Error("Unterminated AIR release JSON string");
  }
  function value(depth = 0) {
    requireValue(depth < 64, "AIR release JSON nesting exceeds cap");
    space();
    if (text[at] === '"') {
      string();
      return;
    }
    if (text[at] === "{") {
      at++;
      space();
      const keys = new Set();
      if (text[at] === "}") {
        at++;
        return;
      }
      for (;;) {
        space();
        const key = string();
        requireValue(!keys.has(key), "Duplicate AIR release JSON key");
        keys.add(key);
        space();
        at++;
        value(depth + 1);
        space();
        if (text[at++] === "}") return;
      }
    }
    if (text[at] === "[") {
      at++;
      space();
      if (text[at] === "]") {
        at++;
        return;
      }
      for (;;) {
        value(depth + 1);
        space();
        if (text[at++] === "]") return;
      }
    }
    while (at < text.length && !/[\s,}\]]/.test(text[at])) at++;
  }
  value();
  return result;
}
export function validateFreshAirReleasePin(pin) {
  requireValue(
    exact(pin, ["schemaVersion", "repository", "commit", "version", "manifestSha256"]) &&
      pin.schemaVersion === 1 &&
      REPO(pin.repository) &&
      GIT(pin.commit) &&
      VERSION(pin.version) &&
      SHA(pin.manifestSha256),
    "Exact AIR release repository/commit/version/manifest digest required",
  );
  return pin;
}
function sourceIdentity(report, release) {
  requireValue(
    REPO(report.repository) && GIT(report.commit) && GIT(report.tree),
    "Invalid AIR fork source identity",
  );
  for (const key of ["repository", "commit", "tree", "sourceLockSha256", "sourceFingerprintSha256"])
    if (release) requireValue(report[key] === release[key], `AIR release source mismatch: ${key}`);
  requireValue(
    SHA(report.sourceLockSha256) && SHA(report.sourceFingerprintSha256),
    "Missing AIR source fingerprints",
  );
}
function apiIdentity(report, release) {
  requireValue(
    report.apiVersion === 2 &&
      report.headerBundleVersion === 2 &&
      report.sourceLanguage === "floor-metal-3.2" &&
      SHA(report.headerSha256),
    "Unsupported AIR compiler API/header identity",
  );
  if (release)
    requireValue(report.headerSha256 === release.headerSha256, "AIR release header mismatch");
}
function outputRecords(files) {
  requireValue(
    exact(files, AIR_RELEASE_OUTPUTS) &&
      AIR_RELEASE_OUTPUTS.every((name) => fileRecord(files[name], airReleaseFileMaximum(name))),
    "Invalid AIR release compiler output records",
  );
}
export function verifyFreshAirManifest(bytes, pin) {
  validateFreshAirReleasePin(pin);
  requireValue(
    airReleaseSha256(bytes) === pin.manifestSha256,
    "AIR release manifest digest mismatch",
  );
  const report = parseAirReleaseJson(bytes);
  requireValue(
    exact(report, [
      "schemaVersion",
      "status",
      "kind",
      "binaryBuiltFromNewFork",
      "repository",
      "commit",
      "tree",
      "version",
      "sourceLockSha256",
      "sourceFingerprintSha256",
      "apiVersion",
      "headerBundleVersion",
      "sourceLanguage",
      "headerSha256",
      "buildReceiptSha256",
      "files",
      "runtimeFiles",
      "assets",
    ]) &&
      report.schemaVersion === 1 &&
      report.status === "final" &&
      report.kind === "fork-built-compiler" &&
      report.binaryBuiltFromNewFork === true,
    "Unsupported or unfinished AIR fork release",
  );
  for (const key of ["repository", "commit", "version"])
    requireValue(report[key] === pin[key], `AIR release pin mismatch: ${key}`);
  sourceIdentity(report);
  apiIdentity(report);
  outputRecords(report.files);
  requireValue(SHA(report.buildReceiptSha256), "Missing AIR build receipt digest");
  requireValue(
    exact(report.assets, Object.keys(AIR_RELEASE_CAPS)),
    "Incomplete AIR binary/source/build-input closure",
  );
  const names = new Set();
  for (const [kind, asset] of Object.entries(report.assets)) {
    requireValue(
      exact(asset, ["name", "bytes", "sha256"]) &&
        /^[A-Za-z0-9][A-Za-z0-9_.-]*$/.test(asset.name ?? "") &&
        !names.has(asset.name) &&
        fileRecord({ bytes: asset.bytes, sha256: asset.sha256 }, AIR_RELEASE_CAPS[kind]),
      `Invalid bounded AIR asset: ${kind}`,
    );
    names.add(asset.name);
  }
  requireValue(
    object(report.runtimeFiles) &&
      Object.keys(report.runtimeFiles).length <= 64 &&
      Object.keys(report.runtimeFiles).length >= 8,
    "Invalid AIR runtime inventory",
  );
  for (const [path, record] of Object.entries(report.runtimeFiles))
    requireValue(
      safeAirReleasePath(path) && fileRecord(record, airReleaseFileMaximum(path)),
      "Invalid AIR runtime member record",
    );
  for (const name of [
    ...AIR_RELEASE_OUTPUTS,
    "build-report.json",
    "source-lock.json",
    "air_compiler.h",
    "README.txt",
    "CONTENTS.sha256",
    ...AIR_RELEASE_NOTICE_PATHS,
  ])
    requireValue(report.runtimeFiles[name], `Missing AIR runtime member: ${name}`);
  for (const name of AIR_RELEASE_OUTPUTS)
    requireValue(
      isDeepStrictEqual(report.runtimeFiles[name], report.files[name]),
      "AIR runtime output inventory differs",
    );
  requireValue(
    report.runtimeFiles["build-report.json"].sha256 === report.buildReceiptSha256 &&
      report.runtimeFiles["source-lock.json"].sha256 === report.sourceLockSha256,
    "AIR runtime provenance inventory differs",
  );
  return report;
}
export function freshAirAssetUrl(release, kind) {
  requireValue(AIR_RELEASE_CAPS[kind] && release.assets?.[kind], "Unknown AIR release asset");
  return `https://github.com/${release.repository}/releases/download/${release.version}/${release.assets[kind].name}`;
}
export function verifyFreshAirAsset(bytes, release, kind) {
  const entry = release.assets?.[kind];
  requireValue(
    AIR_RELEASE_CAPS[kind] &&
      entry &&
      bytes.byteLength === entry.bytes &&
      bytes.byteLength <= AIR_RELEASE_CAPS[kind] &&
      airReleaseSha256(bytes) === entry.sha256,
    `AIR release asset mismatch: ${kind}`,
  );
}
function validateInputMap(inputs) {
  requireValue(
    object(inputs) &&
      Object.keys(inputs).length === 50 &&
      Object.entries(inputs).every(
        ([path, digest]) =>
          safeAirReleasePath(path) && /^(?:clang|llvm|wasm)\//.test(path) && SHA(digest),
      ),
    "Incomplete AIR source input closure",
  );
}
export function freshAirSourceFingerprint(report) {
  requireValue(
    REPO(report.repository) &&
      GIT(report.commit) &&
      GIT(report.tree) &&
      SHA(report.sourceLockSha256),
    "Invalid AIR source fingerprint inputs",
  );
  validateInputMap(report.sourceInputs);
  return airReleaseSha256(
    `floor-air-wasm-source-v1\n${report.repository}\n${report.commit}\n${report.tree}\n${report.sourceLockSha256}\n` +
      Object.entries(report.sourceInputs)
        .sort(([a], [b]) => (a < b ? -1 : 1))
        .map(([path, digest]) => `${path}\0${digest}\n`)
        .join(""),
  );
}
function buildTool(tool) {
  requireValue(
    exact(tool, [
      "emscriptenVersion",
      "emsdkCommit",
      "nodeMajor",
      "platform",
      "versions",
      "jobs",
      "byteIdenticalRebuildClaimed",
      "runner",
    ]) &&
      tool.emscriptenVersion === "4.0.20" &&
      tool.emsdkCommit === "e4fe26ef59168ff44f4c23c466e497bf60b3411e" &&
      tool.nodeMajor === 24 &&
      tool.platform === "Linux" &&
      tool.byteIdenticalRebuildClaimed === false &&
      Number.isSafeInteger(tool.jobs) &&
      tool.jobs >= 1 &&
      tool.jobs <= 4,
    "Unsupported AIR build toolchain",
  );
  for (const [key, names] of [
    ["versions", ["node", "emcc", "cmake", "ninja", "nativeCompiler", "python", "ccache"]],
    ["runner", ["imageOS", "imageVersion", "os", "arch"]],
  ])
    requireValue(
      exact(tool[key], names) &&
        names.every(
          (name) =>
            typeof tool[key][name] === "string" &&
            tool[key][name].length > 0 &&
            tool[key][name].length <= 4096,
        ),
      "Incomplete AIR build environment disclosure",
    );
  requireValue(
    /^v24\./.test(tool.versions.node) &&
      /^emcc .* 4\.0\.20(?:\s|$)/.test(tool.versions.emcc) &&
      tool.runner.os === "Linux",
    "AIR disclosed tool versions/platform contradict policy",
  );
}
export function verifyFreshAirBuildInputs(bytes, release) {
  verifyFreshAirAsset(bytes, release, "buildInputs");
  const report = parseAirReleaseJson(bytes);
  requireValue(
    exact(report, [
      "schemaVersion",
      "repository",
      "commit",
      "tree",
      "version",
      "sourceLockSha256",
      "sourceInputs",
      "sourceFingerprintSha256",
      "sourceTreeClean",
      "buildTool",
      "commands",
      "tooling",
      "sourceDateEpoch",
      "sourceArchivePrefix",
      "sdkSource",
    ]) &&
      report.schemaVersion === 1 &&
      report.version === release.version &&
      report.sourceTreeClean === true &&
      Number.isSafeInteger(report.sourceDateEpoch) &&
      report.sourceDateEpoch > 0 &&
      report.sourceArchivePrefix === `air-wasm-${release.version}-source/` &&
      exact(report.sdkSource, ["repository", "commit", "manifestSha256", "runtimeSources"]) &&
      report.sdkSource.repository === "https://github.com/emscripten-core/emsdk" &&
      report.sdkSource.commit === "e4fe26ef59168ff44f4c23c466e497bf60b3411e" &&
      SHA(report.sdkSource.manifestSha256) &&
      exact(report.sdkSource.runtimeSources, [
        "archivePrefix",
        "fileCount",
        "bytes",
        "inventorySha256",
      ]) &&
      report.sdkSource.runtimeSources.archivePrefix ===
        `air-wasm-${release.version}-sdk-runtime/` &&
      Number.isSafeInteger(report.sdkSource.runtimeSources.fileCount) &&
      report.sdkSource.runtimeSources.fileCount > 0 &&
      report.sdkSource.runtimeSources.fileCount <= 10000 &&
      Number.isSafeInteger(report.sdkSource.runtimeSources.bytes) &&
      report.sdkSource.runtimeSources.bytes > 0 &&
      report.sdkSource.runtimeSources.bytes <= 128 * 1024 ** 2 &&
      SHA(report.sdkSource.runtimeSources.inventorySha256),
    "Unsupported AIR build inputs",
  );
  sourceIdentity(report, release);
  validateInputMap(report.sourceInputs);
  buildTool(report.buildTool);
  requireValue(
    freshAirSourceFingerprint(report) === report.sourceFingerprintSha256,
    "AIR source input fingerprint mismatch",
  );
  requireValue(
    Array.isArray(report.commands) &&
      report.commands.length > 0 &&
      report.commands.length <= 32 &&
      report.commands.every(
        (entry) =>
          Array.isArray(entry) &&
          entry.length > 0 &&
          entry.length <= 32 &&
          entry.every((arg) => typeof arg === "string" && arg.length > 0 && arg.length <= 8192),
      ),
    "Missing AIR build commands",
  );
  requireValue(
    exact(report.tooling, [
      ".github/workflows/release-air-wasm.yml",
      "wasm/release/github_release.py",
      "wasm/release/probe.mjs",
      "wasm/release/release-config.json",
      "wasm/release/release.py",
    ]) &&
      Object.entries(report.tooling).every(
        ([path, digest]) => safeAirReleasePath(path) && SHA(digest),
      ),
    "Missing AIR release tooling inputs",
  );
  return report;
}
export function verifyFreshAirReceipt(bytes, release, inputs) {
  requireValue(
    airReleaseSha256(bytes) === release.buildReceiptSha256,
    "AIR build receipt digest mismatch",
  );
  const receipt = parseAirReleaseJson(bytes);
  requireValue(
    exact(receipt, [
      "schemaVersion",
      "kind",
      "repository",
      "commit",
      "tree",
      "version",
      "sourceLockSha256",
      "sourceInputs",
      "sourceFingerprintSha256",
      "sourceTreeClean",
      "apiVersion",
      "headerBundleVersion",
      "sourceLanguage",
      "headerSha256",
      "buildTool",
      "files",
      "verification",
    ]) &&
      receipt.schemaVersion === 1 &&
      receipt.kind === "floor-air-wasm-build" &&
      receipt.version === release.version &&
      receipt.sourceTreeClean === true,
    "Unsupported AIR fresh build receipt",
  );
  sourceIdentity(receipt, release);
  apiIdentity(receipt, release);
  outputRecords(receipt.files);
  validateInputMap(receipt.sourceInputs);
  buildTool(receipt.buildTool);
  requireValue(
    freshAirSourceFingerprint(receipt) === receipt.sourceFingerprintSha256,
    "AIR receipt source fingerprint mismatch",
  );
  for (const name of AIR_RELEASE_OUTPUTS)
    requireValue(
      receipt.files[name].bytes === release.files[name].bytes &&
        receipt.files[name].sha256 === release.files[name].sha256,
      "AIR receipt output mismatch",
    );
  const probe = receipt.verification;
  requireValue(
    exact(probe, [
      "schemaVersion",
      "exportsVerified",
      "headerVerified",
      "abiVersion",
      "headerBundleVersion",
      "headerSha256",
      "smoke",
      "browserExecutionVerified",
      "deviceExecutionVerified",
      "gpuExecutionVerified",
      "fullApplicationVerified",
    ]) &&
      probe.schemaVersion === 1 &&
      probe.exportsVerified === true &&
      probe.headerVerified === true &&
      probe.abiVersion === 2 &&
      probe.headerBundleVersion === 2 &&
      probe.headerSha256 === receipt.headerSha256 &&
      exact(probe.smoke, ["passed", "cases"]) &&
      probe.smoke.passed === true &&
      Array.isArray(probe.smoke.cases) &&
      probe.smoke.cases.length === 3 &&
      [
        ["kernel-metallib", 0],
        ["invalid-source", 3],
        ["bad-options", 2],
      ].every(([name, status], index) => {
        const result = probe.smoke.cases[index];
        return (
          exact(result, ["name", "passed", "status", "outputBytes", "diagnosticBytes"]) &&
          result.name === name &&
          result.status === status &&
          result.passed === true &&
          Number.isSafeInteger(result.outputBytes) &&
          Number.isSafeInteger(result.diagnosticBytes) &&
          result.diagnosticBytes >= 0 &&
          result.diagnosticBytes <= 64 * 1024 &&
          (status === 0
            ? result.outputBytes > 4 && result.outputBytes <= 64 * 1024 ** 2
            : result.outputBytes === 0 && result.diagnosticBytes > 0)
        );
      }) &&
      [
        "browserExecutionVerified",
        "deviceExecutionVerified",
        "gpuExecutionVerified",
        "fullApplicationVerified",
      ].every((key) => probe[key] === false),
    "Incomplete or overstated AIR build verification scope",
  );
  if (inputs) {
    requireValue(
      isDeepStrictEqual(receipt.sourceInputs, inputs.sourceInputs),
      "AIR receipt/build-input source mismatch",
    );
    requireValue(
      isDeepStrictEqual(receipt.buildTool, inputs.buildTool),
      "AIR receipt/build-input tool mismatch",
    );
  }
  return receipt;
}
export function verifyFreshAirSourceLock(bytes, release, inputs) {
  requireValue(
    airReleaseSha256(bytes) === release.sourceLockSha256,
    "AIR source lock digest mismatch",
  );
  const lock = parseAirReleaseJson(bytes);
  requireValue(
    exact(lock, ["schemaVersion", "upstream", "buildTool", "compilerFiles", "sdkFiles"]) &&
      lock.schemaVersion === 1 &&
      exact(lock.upstream, ["repository", "commit", "archiveSha256"]) &&
      /^https:\/\/github\.com\/[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(
        lock.upstream.repository ?? "",
      ) &&
      GIT(lock.upstream.commit) &&
      SHA(lock.upstream.archiveSha256) &&
      exact(lock.buildTool, [
        "emscriptenVersion",
        "emsdkCommit",
        "nodeMajor",
        "platform",
        "byteIdenticalRebuildClaimed",
      ]) &&
      Object.entries(lock.buildTool).every(([key, value]) => value === inputs.buildTool[key]) &&
      object(lock.compilerFiles) &&
      Object.keys(lock.compilerFiles).length === 31 &&
      object(lock.sdkFiles) &&
      Object.keys(lock.sdkFiles).length === 19,
    "Incomplete AIR source lock",
  );
  const merged = {
    ...lock.compilerFiles,
    ...Object.fromEntries(
      Object.entries(lock.sdkFiles).map(([path, digest]) => [`wasm/${path}`, digest]),
    ),
  };
  validateInputMap(merged);
  requireValue(
    Object.keys(lock.compilerFiles).every((path) => /^(?:clang|llvm)\//.test(path)) &&
      isDeepStrictEqual(merged, inputs.sourceInputs),
    "AIR source lock/input closure mismatch",
  );
  const headers = Object.entries(lock.sdkFiles)
    .filter(([path]) => path.startsWith("api/include/"))
    .sort(([a], [b]) => (a < b ? -1 : 1));
  requireValue(
    headers.length === 10 &&
      airReleaseSha256(
        "floor-air-header-bundle-v2\n" +
          headers.map(([path, digest]) => `${path.slice(12)}:${digest}\n`).join(""),
      ) === release.headerSha256,
    "AIR source lock header identity mismatch",
  );
  return lock;
}

export function verifyFreshAirNotices(files, release) {
  const origins = parseAirReleaseJson(files.get("licenses/ORIGINS.json"));
  requireValue(
    exact(origins, ["schemaVersion", "repository", "commit", "tree", "files"]) &&
      origins.schemaVersion === 1 &&
      ["repository", "commit", "tree"].every((key) => origins[key] === release[key]) &&
      exact(
        origins.files,
        AIR_RELEASE_NOTICE_PATHS.filter((path) => path !== "licenses/ORIGINS.json"),
      ),
    "Invalid AIR license origin closure",
  );
  for (const path of AIR_RELEASE_NOTICE_PATHS) {
    const bytes = files.get(path),
      record = release.runtimeFiles[path];
    requireValue(
      bytes &&
        record &&
        bytes.byteLength === record.bytes &&
        airReleaseSha256(bytes) === record.sha256,
      "AIR license bytes differ from pinned inventory",
    );
    if (path === "licenses/ORIGINS.json") continue;
    const origin = origins.files[path];
    requireValue(
      origin.bytes === record.bytes && origin.sha256 === record.sha256,
      "AIR license origin digest mismatch",
    );
    if (path === "licenses/GPL-2.0.txt")
      requireValue(
        isDeepStrictEqual(origin, gplNotice),
        "AIR external GPL text differs from pinned official source",
      );
    else if (path.startsWith("licenses/emscripten/"))
      requireValue(
        exact(origin, ["sdkPath", "emsdkCommit", "emscriptenVersion", "bytes", "sha256"]) &&
          sdkNotices.includes(origin.sdkPath) &&
          path === `licenses/emscripten/${origin.sdkPath}` &&
          origin.emsdkCommit === "e4fe26ef59168ff44f4c23c466e497bf60b3411e" &&
          origin.emscriptenVersion === "4.0.20",
        "AIR SDK license provenance mismatch",
      );
    else
      requireValue(
        exact(origin, ["repositoryPath", "bytes", "sha256"]) &&
          origin.repositoryPath === path.slice(9) &&
          repositoryNotices.includes(origin.repositoryPath),
        "AIR license repository path mismatch",
      );
  }
  return origins;
}
