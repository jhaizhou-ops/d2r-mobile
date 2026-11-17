// 新运行时必须携带真实编译器闭包；旧清单仍可读取，但没有编译服务。
// New runtimes carry a real compiler closure; legacy inventories remain readable without a compiler service.
import { AIR_COMPILER_SOURCE_LANGUAGE, AIR_COMPILER_LIMITS } from "./air-compiler-contract.mjs";
export const AIR_COMPILER_RUNTIME = Object.freeze({
  version: 2,
  id: "d2r-floor-metal-air-v2",
  apiVersion: 2,
  headerBundleVersion: 2,
  sourceLanguage: AIR_COMPILER_SOURCE_LANGUAGE,
  entry: "sealed-workers/air-compiler-worker.mjs",
  factory: "toolchain/metal-air-compiler/dist/d2r_air_wasm.mjs",
  wasm: "toolchain/metal-air-compiler/dist/d2r_air_wasm.wasm",
  receipt: "toolchain/metal-air-compiler/dist/build-report.json",
});
export const AIR_COMPILER_OUTPUT_NAMES = Object.freeze(["d2r_air_wasm.mjs", "d2r_air_wasm.wasm"]);
const maximumOrdinaryAsset = 64 * 1024 * 1024;
export function airCompilerOutputMaximumBytes(name) {
  if (name === "d2r_air_wasm.wasm") return AIR_COMPILER_LIMITS.wasmBytes;
  if (name === "d2r_air_wasm.mjs") return AIR_COMPILER_LIMITS.factoryBytes;
  throw new Error("Unknown AIR compiler output");
}
export function runtimeAssetMaximumBytes(path) {
  return path === AIR_COMPILER_RUNTIME.wasm ? AIR_COMPILER_LIMITS.wasmBytes : maximumOrdinaryAsset;
}
// 只有独占载荷中的精确编译器 WASM 切片可超过普通资产上限。
// Only a sole, exact compiler WASM slice may exceed the ordinary asset cap.
export function runtimeTransportMaximumBytes(file, manifest) {
  const wasm = manifest.assets.find((asset) => asset.path === AIR_COMPILER_RUNTIME.wasm);
  const entries = manifest.transport.entries.filter((entry) => entry.file === file.path);
  if (
    wasm &&
    entries.length === 1 &&
    entries[0].sha256 === wasm.sha256 &&
    entries[0].bytes === wasm.bytes &&
    entries[0].offset === 0 &&
    file.decodedBytes === wasm.bytes
  )
    return AIR_COMPILER_LIMITS.wasmBytes;
  return maximumOrdinaryAsset;
}
const sha256 = (value) => typeof value === "string" && /^[a-f0-9]{64}$/.test(value);
const boundedFile = (value, maximum) =>
  Number.isSafeInteger(value?.bytes) &&
  value.bytes > 0 &&
  value.bytes <= maximum &&
  sha256(value.sha256);

const exactKeys = (value, names) =>
  value &&
  typeof value === "object" &&
  !Array.isArray(value) &&
  Object.keys(value).sort().join("\0") === [...names].sort().join("\0");
const gitIdentity = (value) => typeof value === "string" && /^[a-f0-9]{40}$/.test(value);
export function isFreshAirCompilerReceipt(report) {
  return report?.schemaVersion === 1 && report.kind === "floor-air-wasm-build";
}
function freshReceiptStructure(report) {
  const tool = report.buildTool,
    probe = report.verification;
  if (
    !exactKeys(tool, [
      "emscriptenVersion",
      "emsdkCommit",
      "nodeMajor",
      "platform",
      "versions",
      "runner",
      "jobs",
      "byteIdenticalRebuildClaimed",
    ]) ||
    !exactKeys(tool.versions, [
      "node",
      "emcc",
      "cmake",
      "ninja",
      "nativeCompiler",
      "python",
      "ccache",
    ]) ||
    Object.values(tool.versions).some(
      (value) => typeof value !== "string" || !value || value.length > 32768,
    ) ||
    !/^v24\./.test(tool.versions.node) ||
    !/^emcc .* 4\.0\.20(?:\s|$)/.test(tool.versions.emcc) ||
    !exactKeys(tool.runner, ["imageOS", "imageVersion", "os", "arch"]) ||
    Object.values(tool.runner).some(
      (value) => typeof value !== "string" || !value || value.length > 256,
    ) ||
    tool.runner.os !== "Linux" ||
    !Number.isInteger(tool.jobs) ||
    tool.jobs < 1 ||
    tool.jobs > 4
  )
    return false;
  if (
    !exactKeys(probe, [
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
    ]) ||
    !exactKeys(probe.smoke, ["passed", "cases"]) ||
    !Array.isArray(probe.smoke.cases) ||
    probe.smoke.cases.length !== 3
  )
    return false;
  const names = ["kernel-metallib", "invalid-source", "bad-options"],
    statuses = [0, 3, 2];
  return (
    probe.smoke.cases.every(
      (item, index) =>
        exactKeys(item, ["name", "passed", "status", "outputBytes", "diagnosticBytes"]) &&
        item.name === names[index] &&
        item.passed === true &&
        item.status === statuses[index] &&
        Number.isSafeInteger(item.outputBytes) &&
        (index === 0
          ? item.outputBytes > 4 && item.outputBytes <= 64 * 1024 ** 2
          : item.outputBytes === 0) &&
        Number.isSafeInteger(item.diagnosticBytes) &&
        item.diagnosticBytes >= (index === 0 ? 0 : 1) &&
        item.diagnosticBytes <= 64 * 1024,
    ) && Object.values(report.files ?? {}).every((record) => exactKeys(record, ["bytes", "sha256"]))
  );
}
function validateFreshReceipt(report) {
  if (
    !exactKeys(report, [
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
    ]) ||
    !/^[A-Za-z0-9][A-Za-z0-9_.-]*\/[A-Za-z0-9][A-Za-z0-9_.-]*$/.test(report.repository ?? "") ||
    !gitIdentity(report.commit) ||
    !gitIdentity(report.tree) ||
    !/^v\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$/.test(report.version ?? "") ||
    report.sourceTreeClean !== true ||
    report.apiVersion !== 2 ||
    report.headerBundleVersion !== 2 ||
    report.sourceLanguage !== AIR_COMPILER_SOURCE_LANGUAGE ||
    !sha256(report.headerSha256) ||
    !sha256(report.sourceFingerprintSha256) ||
    !sha256(report.sourceLockSha256) ||
    !report.sourceInputs ||
    Object.keys(report.sourceInputs).length !== 50 ||
    Object.entries(report.sourceInputs).some(
      ([path, digest]) =>
        !/^(?:clang|llvm|wasm)\/[A-Za-z0-9_.+/-]+$/.test(path) ||
        path.split("/").some((part) => !part || part === "." || part === "..") ||
        !sha256(digest),
    ) ||
    !freshReceiptStructure(report) ||
    report.buildTool?.emscriptenVersion !== "4.0.20" ||
    report.buildTool.emsdkCommit !== "e4fe26ef59168ff44f4c23c466e497bf60b3411e" ||
    report.buildTool.nodeMajor !== 24 ||
    report.buildTool.platform !== "Linux" ||
    report.buildTool.byteIdenticalRebuildClaimed !== false ||
    !exactKeys(report.files, ["air_wasm.mjs", "air_wasm.wasm"]) ||
    !boundedFile(report.files["air_wasm.mjs"], AIR_COMPILER_LIMITS.factoryBytes) ||
    !boundedFile(report.files["air_wasm.wasm"], AIR_COMPILER_LIMITS.wasmBytes) ||
    report.verification?.schemaVersion !== 1 ||
    report.verification.exportsVerified !== true ||
    report.verification.headerVerified !== true ||
    report.verification.abiVersion !== 2 ||
    report.verification.headerBundleVersion !== 2 ||
    report.verification.headerSha256 !== report.headerSha256 ||
    report.verification.smoke?.passed !== true ||
    [
      "browserExecutionVerified",
      "deviceExecutionVerified",
      "gpuExecutionVerified",
      "fullApplicationVerified",
    ].some((key) => report.verification[key] !== false)
  )
    throw new Error("Invalid fresh AIR compiler build receipt");
  return report;
}
// 仅允许这两个保持字节不变的别名，绝不重写构建收据。
// Only these two byte-preserving aliases are permitted. Never rewrite a build receipt.
export function airCompilerReceiptOutput(report, localName) {
  if (!AIR_COMPILER_OUTPUT_NAMES.includes(localName))
    throw new Error("Unknown AIR compiler output alias");
  return report.files[
    isFreshAirCompilerReceipt(report) ? localName.replace(/^d2r_/, "") : localName
  ];
}
export function airCompilerReceiptSourceRecord(report) {
  return isFreshAirCompilerReceipt(report)
    ? {
        path: "toolchain/metal-air-compiler/dist/source-lock.json",
        sha256: report.sourceLockSha256,
      }
    : { path: "toolchain/metal-air-compiler/manifest.json", sha256: report.sourceManifestSha256 };
}
export function validateAirCompilerBuildReceipt(report) {
  if (isFreshAirCompilerReceipt(report)) return validateFreshReceipt(report);
  if (
    report?.version !== AIR_COMPILER_RUNTIME.version ||
    report.id !== AIR_COMPILER_RUNTIME.id ||
    report.apiVersion !== 2 ||
    report.headerBundleVersion !== 2 ||
    report.sourceLanguage !== AIR_COMPILER_SOURCE_LANGUAGE ||
    report.emscripten !== "4.0.20" ||
    !sha256(report.headerSha256) ||
    !sha256(report.sourceFingerprintSha256) ||
    !sha256(report.sourceManifestSha256) ||
    !/^[a-f0-9]{40}$/.test(report.sourceCommit ?? "") ||
    !report.sourceInputs ||
    Object.keys(report.sourceInputs).length > 128 ||
    !Object.keys(report.sourceInputs).length ||
    Object.entries(report.sourceInputs).some(
      ([name, digest]) =>
        (name !== "toolchain.mjs" && !/^(?:api|patches)\/[A-Za-z0-9_.+/-]+$/.test(name)) ||
        name.split("/").some((part) => !part || part === "." || part === "..") ||
        !sha256(digest),
    ) ||
    !report.files ||
    Object.keys(report.files).length !== AIR_COMPILER_OUTPUT_NAMES.length ||
    AIR_COMPILER_OUTPUT_NAMES.some(
      (name) => !boundedFile(report.files[name], airCompilerOutputMaximumBytes(name)),
    )
  )
    throw new Error("Invalid AIR compiler build receipt");
  return report;
}

export function airCompilerRuntimeComponent(report) {
  validateAirCompilerBuildReceipt(report);
  return Object.freeze({
    ...AIR_COMPILER_RUNTIME,
    sealedEntry: AIR_COMPILER_RUNTIME.entry,
    sealedJobAdapter: true,
    headerSha256: report.headerSha256,
    sourceFingerprintSha256: report.sourceFingerprintSha256,
    factorySha256: airCompilerReceiptOutput(report, "d2r_air_wasm.mjs").sha256,
    wasmSha256: airCompilerReceiptOutput(report, "d2r_air_wasm.wasm").sha256,
  });
}

export function validateAirCompilerRuntimeComponent(manifest, { required = false } = {}) {
  const component = manifest?.components?.airCompiler;
  const records = new Map((manifest?.assets ?? []).map((asset) => [asset.path, asset]));
  const paths = ["entry", "factory", "wasm", "receipt"];
  if (component === undefined) {
    if (required || paths.some((name) => records.has(AIR_COMPILER_RUNTIME[name])))
      throw new Error("Verified AIR compiler component is missing; rebuild the public runtime");
    return undefined;
  }
  if (
    Object.entries(AIR_COMPILER_RUNTIME).some(([key, value]) => component[key] !== value) ||
    component.sealedEntry !== AIR_COMPILER_RUNTIME.entry ||
    component.sealedJobAdapter !== true ||
    !sha256(component.headerSha256) ||
    !sha256(component.sourceFingerprintSha256) ||
    !sha256(component.factorySha256) ||
    !sha256(component.wasmSha256) ||
    paths.some((name) => !records.has(component[name])) ||
    records.get(component.factory).sha256 !== component.factorySha256 ||
    records.get(component.wasm).sha256 !== component.wasmSha256
  )
    throw new Error("Invalid verified AIR compiler component");
  return component;
}

export function assertAirCompilerReceiptMatchesComponent(report, component) {
  const expected = airCompilerRuntimeComponent(report);
  if (!component || Object.entries(expected).some(([key, value]) => component[key] !== value))
    throw new Error("AIR compiler component does not match its authenticated build receipt");
  return report;
}
