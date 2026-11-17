// 实验性 floor-Metal C API 的内存边界；不加载资产、不联网，也不声称兼容 Apple MSL。
// Memory boundary for the experimental floor-Metal C API; no asset loading, network, or Apple MSL compatibility claim.
import {
  sanitizeAirFrontendDiagnostics,
  sanitizeCompilerDiagnostics,
  shaderFailureDiagnostic,
} from "./shader-diagnostics.mjs";
import { AIR_COMPILER_SOURCE_LANGUAGE } from "./air-compiler-contract.mjs";
const LIMITS = Object.freeze({
  source: 4 * 1024 * 1024,
  output: 64 * 1024 * 1024,
  diagnostics: 64 * 1024,
});
const STATUS = Object.freeze([
  "AIR_OK",
  "AIR_BAD_INPUT",
  "AIR_BAD_OPTIONS",
  "AIR_FRONTEND_FAILED",
  "AIR_INVALID_IR",
  "AIR_UNRESOLVED_SYMBOL",
  "AIR_NO_ENTRY",
  "AIR_OUTPUT_LIMIT",
]);
const EXPORTS = [
  "malloc",
  "free",
  "air_result_status",
  "air_result_data",
  "air_result_size",
  "air_result_diagnostics",
  "air_result_diagnostics_size",
  "air_result_free",
  "air_compiler_header_sha256",
];
const encoder = new TextEncoder();
const errors = new WeakSet();
const wrappedModules = new WeakSet();
function failure(code, diagnosticBytes = 0, shaderDiagnostic) {
  const error = new Error(`Experimental shader compiler failed [${code}].`);
  error.code = code;
  error.diagnosticBytes = diagnosticBytes;
  if (shaderDiagnostic) error.shaderDiagnostic = shaderDiagnostic;
  error.requiresWorkerTermination = true;
  errors.add(error);
  return error;
}
function checkNumber(value, maximum, code) {
  if (!Number.isSafeInteger(value) || value < 0 || value > maximum) throw failure(code);
  return value;
}

// 调用方须先验证编译器 WASM 资产，传入已实例化模块和受信任头文件指纹。
// The caller must authenticate compiler WASM assets first and pass the instantiated module and trusted header fingerprint.
// 默认单次使用；v2 可显式选择有限批次，错误后必须丢弃模块。
// Single-use by default; v2 may explicitly opt into bounded batches, discarding the module after any failure.
// 同步 WASM 执行无法协作取消；真正中断须由外部终止专用 Worker。
// API v2 和 fast-f32 均须显式选择；v1 保持保守模式 ABI。
// API v2 and fast-f32 are explicit opt-ins; v1 retains its conservative ABI.
// Synchronous WASM cannot cooperatively cancel; the owner must terminate the dedicated Worker to interrupt it.
export function createAirCompilerAdapter(
  module,
  { expectedHeaderSha256, apiVersion = 1, maxCompilations = 1 } = {},
) {
  if (
    !Number.isSafeInteger(maxCompilations) ||
    maxCompilations < 1 ||
    maxCompilations > 256 ||
    (apiVersion !== 2 && maxCompilations !== 1)
  )
    throw failure("AIR_BAD_OPTIONS");
  if (apiVersion !== 1 && apiVersion !== 2) throw failure("AIR_BAD_OPTIONS");
  if (typeof expectedHeaderSha256 !== "string" || !/^[a-f0-9]{64}$/.test(expectedHeaderSha256))
    throw failure("AIR_HEADER_IDENTITY_INVALID");
  if (wrappedModules.has(module)) throw failure("AIR_MODULE_ALREADY_WRAPPED");
  const api = Object.create(null);
  const compileExport = `air_compile_v${apiVersion}`;
  const requiredExports = [...EXPORTS, compileExport];
  if (apiVersion === 2) requiredExports.push("air_compiler_header_bundle_version");
  for (const name of requiredExports) {
    if (typeof module?.[`_${name}`] !== "function") throw failure("AIR_EXPORT_MISSING");
    api[name] = module[`_${name}`].bind(module);
  }
  const heap = () => {
    const bytes = module.HEAPU8;
    if (
      !(bytes instanceof Uint8Array) ||
      !(bytes.buffer instanceof ArrayBuffer) ||
      bytes.byteOffset !== 0 ||
      bytes.byteLength !== bytes.buffer.byteLength
    )
      throw failure("AIR_HEAP_INVALID");
    return bytes;
  };
  const region = (pointer, size, allowEmpty = false) => {
    checkNumber(pointer, 0xffffffff, "AIR_POINTER_INVALID");
    checkNumber(size, 0xffffffff, "AIR_SIZE_INVALID");
    const bytes = heap();
    if (
      (!allowEmpty && pointer === 0) ||
      pointer > bytes.length ||
      size > bytes.length - pointer ||
      (size > 0 && pointer === 0)
    )
      throw failure("AIR_POINTER_INVALID");
    return bytes.subarray(pointer, pointer + size);
  };
  let headerSha256;
  try {
    if (apiVersion === 2 && api.air_compiler_header_bundle_version() !== 2)
      throw failure("AIR_HEADER_BUNDLE_VERSION_MISMATCH");
    const bytes = region(api.air_compiler_header_sha256(), 65);
    if (bytes[64] !== 0) throw failure("AIR_HEADER_IDENTITY_INVALID");
    headerSha256 = String.fromCharCode(...bytes.subarray(0, 64));
    if (headerSha256 !== expectedHeaderSha256) throw failure("AIR_HEADER_IDENTITY_MISMATCH");
  } catch (error) {
    if (errors.has(error)) throw error;
    throw failure("AIR_RUNTIME_FAILED", 0, shaderFailureDiagnostic(error));
  }
  wrappedModules.add(module);
  let compilationCount = 0,
    active = false,
    disposed = false;
  const check = (signal) => {
    if (disposed) throw failure("AIR_ADAPTER_DISPOSED");
    if (signal?.aborted) throw failure("AIR_ABORTED");
  };
  return Object.freeze({
    status: `experimental-${AIR_COMPILER_SOURCE_LANGUAGE}-not-apple-msl-compatible`,
    limits: LIMITS,
    apiVersion,
    get headerSha256() {
      return headerSha256;
    },
    get used() {
      return compilationCount > 0;
    },
    get compilationCount() {
      return compilationCount;
    },
    // dispose 只阻止新请求，不会终止已经运行的同步 WASM。
    // dispose only prevents new requests; it cannot terminate already-running synchronous WASM.
    dispose() {
      disposed = true;
    },
    compile(
      source,
      {
        iosMajor = 18,
        output = "metallib",
        sourceLanguage = AIR_COMPILER_SOURCE_LANGUAGE,
        mathProfile = "conservative",
        signal,
      } = {},
    ) {
      check(signal);
      if (active) throw failure("AIR_COMPILER_BUSY");
      if (compilationCount >= maxCompilations)
        throw failure(maxCompilations === 1 ? "AIR_MODULE_ALREADY_USED" : "AIR_BATCH_LIMIT");
      if (
        !["conservative", "fast-f32"].includes(mathProfile) ||
        (apiVersion !== 2 && mathProfile !== "conservative") ||
        sourceLanguage !== AIR_COMPILER_SOURCE_LANGUAGE ||
        ![18, 26].includes(iosMajor) ||
        !["metallib", "llvm-text"].includes(output)
      )
        throw failure("AIR_BAD_OPTIONS");
      if (
        typeof source !== "string" ||
        !source.length ||
        source.length > LIMITS.source ||
        source.includes("\0")
      )
        throw failure("AIR_BAD_INPUT");
      const input = encoder.encode(source.replaceAll("\r\n", "\n"));
      if (input.length > LIMITS.source) {
        input.fill(0);
        throw failure("AIR_BAD_INPUT");
      }
      compilationCount++;
      active = true;
      let inputPointer = 0,
        resultPointer = 0,
        copied,
        problem;
      try {
        const allocated = api.malloc(input.length);
        if (allocated === 0) throw failure("AIR_ALLOCATION_FAILED");
        const target = region(allocated, input.length);
        inputPointer = allocated;
        check(signal);
        target.set(input);
        const result = api[compileExport](
          inputPointer,
          input.length,
          iosMajor,
          output === "metallib" ? 0 : 1,
          ...(apiVersion === 2 ? [mathProfile === "fast-f32" ? 1 : 0] : []),
        );
        if (result === 0) throw failure("AIR_ALLOCATION_FAILED");
        region(result, 1);
        resultPointer = result;
        check(signal);
        const status = checkNumber(
          api.air_result_status(resultPointer),
          STATUS.length - 1,
          "AIR_STATUS_INVALID",
        );
        const diagnosticSize = checkNumber(
          api.air_result_diagnostics_size(resultPointer),
          LIMITS.diagnostics,
          "AIR_DIAGNOSTIC_LIMIT",
        );
        const diagnosticPointer = api.air_result_diagnostics(resultPointer);
        const diagnosticRegion = region(diagnosticPointer, diagnosticSize, diagnosticSize === 0);
        check(signal);
        // 只读取有界诊断正文；过滤源码行、路径及密钥后传给本地报告。
        // Read bounded diagnostic prose only; remove source lines, paths and keys before local reporting.
        if (status !== 0)
          throw failure(STATUS[status], diagnosticSize, {
            diagnostics: (STATUS[status] === "AIR_FRONTEND_FAILED"
              ? sanitizeAirFrontendDiagnostics
              : sanitizeCompilerDiagnostics)(new TextDecoder().decode(diagnosticRegion)),
            compilerPhase: "compiling",
          });
        const size = checkNumber(
          api.air_result_size(resultPointer),
          LIMITS.output,
          "AIR_OUTPUT_LIMIT",
        );
        if (!size) throw failure("AIR_EMPTY_OUTPUT");
        const pointer = api.air_result_data(resultPointer);
        check(signal);
        copied = new Uint8Array(region(pointer, size));
        check(signal);
      } catch (error) {
        problem = errors.has(error)
          ? error
          : failure("AIR_RUNTIME_FAILED", 0, shaderFailureDiagnostic(error));
      } finally {
        // getter/编译/释放均可能扩展内存；清理时重新获取 HEAP，不保存旧视图。
        // Getters, compilation and cleanup may grow memory; reacquire HEAP for cleanup, never reuse stale views.
        if (resultPointer) {
          try {
            api.air_result_free(resultPointer);
          } catch {
            problem ??= failure("AIR_CLEANUP_FAILED");
          }
        }
        if (inputPointer) {
          try {
            region(inputPointer, input.length).fill(0);
          } catch {
            problem ??= failure("AIR_CLEANUP_FAILED");
          }
          try {
            api.free(inputPointer);
          } catch {
            problem ??= failure("AIR_CLEANUP_FAILED");
          }
        }
        input.fill(0);
        active = false;
      }
      if (!problem) {
        try {
          check(signal);
        } catch (error) {
          problem = error;
        }
      }
      if (problem) {
        disposed = true;
        copied?.fill(0);
        throw problem;
      }
      return Object.freeze({
        bytes: copied,
        output,
        iosMajor,
        headerSha256,
        apiVersion,
        mathProfile,
        ...(apiVersion === 2 ? { headerBundleVersion: 2 } : {}),
        status: "experimental-output-unvalidated",
        requiresWorkerTermination: compilationCount >= maxCompilations,
      });
    },
  });
}
