// 约束 AIR 编译请求、诊断与编译器身份；依据 ./shader-diagnostics.mjs 的模块与调用约定。
// Constrain AIR compile requests, diagnostics and compiler identity using the module and calling contracts in ./shader-diagnostics.mjs.
import { shaderFailureDiagnostic } from "./shader-diagnostics.mjs";
// 固定生产编译配置和有界消息协议；结构验证不等于 Apple GPU 验收。
// Fixed compilation profile and bounded protocol; structural validation is not Apple GPU acceptance.
export const AIR_COMPILER_SOURCE_LANGUAGE = "floor-metal-3.2";
export const AIR_COMPILER_LIMITS = Object.freeze({
  sourceBytes: 4 * 1024 * 1024,
  outputBytes: 16 * 1024 * 1024,
  factoryBytes: 1024 * 1024,
  wasmBytes: 128 * 1024 * 1024,
  batchCompilations: 128,
  batchSourceBytes: 32 * 1024 * 1024,
  recycleHeapBytes: 256 * 1024 * 1024,
  // 调用后的防护，不声称拦截同步 WASM 内存增长；经认证的构建另外强制执行 2 GiB 线性内存上限。
  // A post-call guard, not a claim to intercept synchronous WASM memory growth.
  // The authenticated build separately enforces its 2 GiB linear-memory maximum.
  maxObservedHeapBytes: 512 * 1024 * 1024,
  initializationMs: 180000,
  compilationMs: 120000,
});
export const AIR_COMPILER_OPTIONS = Object.freeze({
  platform: "ios",
  minimumOS: Object.freeze([18, 0]),
  languageVersion: Object.freeze([3, 2]),
  fastMath: true,
});
export const AIR_COMPILER_PHASES = Object.freeze([
  "verifying",
  "preparing",
  "initializing",
  "compiling",
  "validating",
  "complete",
]);
const codes = new Set([
  "AIR_BAD_INPUT",
  "AIR_BAD_OPTIONS",
  "AIR_FRONTEND_FAILED",
  "AIR_INVALID_IR",
  "AIR_UNRESOLVED_SYMBOL",
  "AIR_NO_ENTRY",
  "AIR_OUTPUT_LIMIT",
  "AIR_HEADER_IDENTITY_INVALID",
  "AIR_HEADER_IDENTITY_MISMATCH",
  "AIR_HEADER_BUNDLE_VERSION_MISMATCH",
  "AIR_EXPORT_MISSING",
  "AIR_HEAP_INVALID",
  "AIR_POINTER_INVALID",
  "AIR_SIZE_INVALID",
  "AIR_RUNTIME_FAILED",
  "AIR_ALLOCATION_FAILED",
  "AIR_STATUS_INVALID",
  "AIR_DIAGNOSTIC_LIMIT",
  "AIR_EMPTY_OUTPUT",
  "AIR_CLEANUP_FAILED",
  "AIR_ABORTED",
  "AIR_ADAPTER_DISPOSED",
  "AIR_COMPILER_BUSY",
  "AIR_MODULE_ALREADY_USED",
  "AIR_MODULE_ALREADY_WRAPPED",
  "AIR_BATCH_LIMIT",
  "AIR_ASSET_INVALID",
  "AIR_ASSET_MISMATCH",
  "AIR_PROTOCOL_FAILED",
  "AIR_WORKER_FAILED",
  "AIR_WORKER_UNAVAILABLE",
  "AIR_COMPILER_DISPOSED",
  "AIR_INITIALIZATION_TIMEOUT",
  "AIR_COMPILATION_TIMEOUT",
  "AIR_MEMORY_LIMIT",
  "AIR_OUTPUT_INVALID",
  "AIR_NETWORK_FORBIDDEN",
]);
// 公开固定代码与经过过滤的本地诊断，不转发原始异常或源码。
// Expose fixed codes and filtered local diagnostics, never raw exceptions or source.
export function isAirCompilerDiagnosticCode(value) {
  return codes.has(value);
}
export function airCompilerFailureCode(value) {
  return codes.has(value) ? value : "AIR_RUNTIME_FAILED";
}
export function airCompilerFailure(code, cause, context) {
  const safe = airCompilerFailureCode(code);
  const error = new Error(`Shader compiler failed [${safe}].`);
  error.code = safe;
  const diagnostic = shaderFailureDiagnostic(cause, context);
  if (diagnostic) error.shaderDiagnostic = diagnostic;
  if (safe === "AIR_ABORTED") error.name = "AbortError";
  return error;
}
export function isAirCompilerHash(value) {
  return typeof value === "string" && /^[a-f0-9]{64}$/.test(value);
}
export function validateAirCompilerRequest(source, entry, stage) {
  if (
    typeof source !== "string" ||
    !source.length ||
    source.length > AIR_COMPILER_LIMITS.sourceBytes ||
    source.includes("\0") ||
    typeof entry !== "string" ||
    !/^[a-zA-Z_][a-zA-Z0-9_]{0,255}$/.test(entry) ||
    !["vertex", "fragment", "kernel"].includes(stage)
  )
    throw airCompilerFailure("AIR_BAD_INPUT");
  const bytes = new TextEncoder().encode(source.replaceAll("\r\n", "\n"));
  const size = bytes.length;
  bytes.fill(0);
  if (size > AIR_COMPILER_LIMITS.sourceBytes) throw airCompilerFailure("AIR_BAD_INPUT");
  return size;
}
export function airCompilerIdentity({ factorySha256, wasmSha256, headerSha256 }) {
  if (![factorySha256, wasmSha256, headerSha256].every(isAirCompilerHash))
    throw airCompilerFailure("AIR_ASSET_INVALID");
  return Object.freeze({
    id: "d2r-floor-metal-air-v2",
    assetSha256: wasmSha256,
    factorySha256,
    headerSha256,
    apiVersion: 2,
    headerBundleVersion: 2,
    sourceLanguage: AIR_COMPILER_SOURCE_LANGUAGE,
    mathProfile: "fast-f32",
  });
}
