// 本地着色器失败投影：固定应用原因、有界编译诊断；不保存源码、路径或密钥。
// Local shader failure projection: fixed app causes and bounded compiler prose, never source, paths or keys.
export const SHADER_DIAGNOSTIC_SUBSTEPS = Object.freeze([
  "authenticated-input",
  "pipeline-initialization",
  "catalog-lookup",
  "source-read",
  "record-preparation",
  "translation",
  "spirv-cross",
  "reflection-validation",
  "source-hashing",
  "compilation",
  "output-validation",
  "output-budget",
  "source-proof",
  "finalization",
]);
const phases = ["verifying", "preparing", "initializing", "compiling", "validating", "complete"];
const errorTypes = [
  "Error",
  "TypeError",
  "RangeError",
  "SyntaxError",
  "AbortError",
  "TimeoutError",
  "CompileError",
  "LinkError",
  "RuntimeError",
  "DataCloneError",
  "InvalidStateError",
  "OperationError",
  "NotSupportedError",
  "QuotaExceededError",
  "JSException",
  "ArgumentException",
  "ArgumentOutOfRangeException",
  "InvalidOperationException",
  "NotImplementedException",
  "NotSupportedException",
  "MissingMethodException",
  "TypeLoadException",
  "FileNotFoundException",
  "OutOfMemoryException",
];
// 只允许源码中明确编写的固定消息；不能把原始异常当作编译器诊断。
// Only explicitly authored fixed messages are eligible; raw exceptions are not compiler diagnostics.
const messages = new Set([
  "Shader translation failed",
  "SPIRV-Cross conversion failed",
  "Invalid conversion request",
  "CROSS_NOT_INITIALIZED",
  "SEALED_INITIALIZATION_FAILED",
  "SPIR-V must be Uint8Array",
  "SPIR-V must contain 5–16777216 whole words",
  "Invalid little-endian SPIR-V magic",
  "Invalid SPIR-V header",
  "Invalid SPIR-V instruction size",
  "Translator resource facts must include sbufs",
  "Unrecognized sampler name (native reflection would reject it)",
  "Translator does not implement this operation",
  "Translator dependency is missing",
  "AIR compiler lease closed or busy",
  "Authenticated main NSO missing",
  "BSS overlaps segment",
  "Catalog record absent from pack",
  "Chunk exceeds read range limit",
  "Compiled SPIR-V/MSL stage does not match declared job",
  "Compiled shader rejected: AIR or language version mismatch",
  "Compiled shader rejected: bitcode digest mismatch",
  "Compiled shader rejected: duplicate function name",
  "Compiled shader rejected: duplicate tag",
  "Compiled shader rejected: embedded debug source is unsupported",
  "Compiled shader rejected: embedded function-name terminator",
  "Compiled shader rejected: entry or stage mismatch",
  "Compiled shader rejected: file size mismatch",
  "Compiled shader rejected: function list size mismatch",
  "Compiled shader rejected: invalid AIRR reflection envelope",
  "Compiled shader rejected: invalid LLVM bitcode envelope",
  "Compiled shader rejected: invalid SHA256",
  "Compiled shader rejected: invalid bitcode payload offset",
  "Compiled shader rejected: invalid bitcode wrapper",
  "Compiled shader rejected: invalid compiler identity",
  "Compiled shader rejected: invalid compiler options",
  "Compiled shader rejected: invalid dynamic header",
  "Compiled shader rejected: invalid dynamic library name",
  "Compiled shader rejected: invalid expected shader identity",
  "Compiled shader rejected: invalid function count",
  "Compiled shader rejected: invalid function group",
  "Compiled shader rejected: invalid function name",
  "Compiled shader rejected: invalid metadata group length",
  "Compiled shader rejected: invalid output bytes",
  "Compiled shader rejected: invalid reflection alignment padding",
  "Compiled shader rejected: invalid reflection field",
  "Compiled shader rejected: invalid reflection group",
  "Compiled shader rejected: invalid reflection root",
  "Compiled shader rejected: invalid reflection root table",
  "Compiled shader rejected: invalid reflection table reference",
  "Compiled shader rejected: invalid reflection terminator",
  "Compiled shader rejected: invalid reflection vector",
  "Compiled shader rejected: invalid reflection vtable",
  "Compiled shader rejected: invalid source",
  "Compiled shader rejected: invalid source-set digest",
  "Compiled shader rejected: invalid structural expectations",
  "Compiled shader rejected: invalid version",
  "Compiled shader rejected: missing LLVM bitcode",
  "Compiled shader rejected: missing section",
  "Compiled shader rejected: missing tag terminator",
  "Compiled shader rejected: not a compiled library",
  "Compiled shader rejected: not an iOS executable library",
  "Compiled shader rejected: overlapping sections",
  "Compiled shader rejected: oversized offset",
  "Compiled shader rejected: reflection count mismatch",
  "Compiled shader rejected: reflection offset mismatch",
  "Compiled shader rejected: reflection range out of bounds",
  "Compiled shader rejected: reflection reference without declared section",
  "Compiled shader rejected: section out of bounds",
  "Compiled shader rejected: source byte limit exceeded",
  "Compiled shader rejected: source digest mismatch",
  "Compiled shader rejected: target OS mismatch",
  "Compiled shader rejected: trailing tag data",
  "Compiled shader rejected: unaccounted library tail",
  "Compiled shader rejected: unaccounted or overlapping declared tail",
  "Compiled shader rejected: unaccounted reflection root fields",
  "Compiled shader rejected: unaccounted reflection tail",
  "Compiled shader rejected: unsupported container version",
  "Compiled shader rejected: unsupported declared library tail",
  "Compiled shader rejected: unsupported reflection version",
  "Compiled shader rejected: unsupported target OS patch version",
  "Compiled shader runtime is unavailable; rebuild application assets",
  "Control hash does not match catalog key",
  "Declared graphics stage does not match SPH stage bits 10..13",
  "Declared graphics stage does not match SPH stage bits 10..13.",
  "Domain MSL must preserve the original triangle patch contract",
  "EXTRACTION_ADAPTER_OPTIONS",
  "EXTRACTION_ASSET_AMBIGUOUS",
  "EXTRACTION_ASSET_MISSING",
  "EXTRACTION_ASSET_SUFFIX",
  "EXTRACTION_ASSET_SUFFIX_LIMIT",
  "EXTRACTION_CANCELLED",
  "EXTRACTION_DISPOSED",
  "EXTRACTION_EXEFS_INVALID",
  "EXTRACTION_ITEM_MANIFEST_LIMIT",
  "EXTRACTION_PROGRAM_BUDGET",
  "EXTRACTION_PROGRAM_MISSING",
  "EXTRACTION_PROVENANCE_REJECTED",
  "EXTRACTION_READ_BOUNDS",
  "EXTRACTION_SELECTION_MISMATCH",
  "EXTRACTION_SELECTION_REJECTED",
  "EXTRACTION_SINGLE_USE",
  "Embedded code marker missing",
  "Expected job object.",
  "Explicit debug/reduced options required",
  "Explicit native debug/reduced translation options required",
  "Full pinned shader catalog required",
  "Full shader digest/completion proof failed",
  "Full shader source/replay byte budget exceeded",
  "Graphics input requires a 0x50-byte SPH plus instructions",
  "Graphics input requires a 0x50-byte SPH plus instructions.",
  "Graphics jobs require explicit attr and fout type tables",
  "Graphics jobs require explicit attr and fout type tables.",
  "Hull SPH requires nonzero threads per input primitive",
  "Hull SPH requires nonzero threads per input primitive.",
  "Incomplete pinned catalog",
  "Incomplete shader digest input",
  "Invalid Cross request",
  "Invalid LZ4 match offset",
  "Invalid LZ4 terminal sequence",
  "Invalid bounded shader input",
  "Invalid c1 range",
  "Invalid catalog row/facts",
  "Invalid compiled shader runtime",
  "Invalid conflict override",
  "Invalid control key",
  "Invalid embedded control location",
  "Invalid embedded shader count",
  "Invalid graphics attribute/output component type or slot",
  "Invalid graphics attribute/output component type or slot.",
  "Invalid or oversized LF Metal source",
  "Invalid or oversized translator SPIR-V base64",
  "Invalid owned shader pack backing",
  "Invalid shader code bounds",
  "Invalid shader correlation ID",
  "Invalid shader digest filename encoding",
  "Invalid shader digest input count",
  "Invalid shader finalization work",
  "Invalid shader output budget",
  "Invalid shader progress total",
  "Invalid shader timeout",
  "Invalid sorted shader digest record",
  "Invalid/duplicate fact key",
  "Job metadata must be at most 64 KiB.",
  "LZ4 allocation budget exceeded or invalid input",
  "LZ4 decoded size mismatch",
  "LZ4 length exceeds decoded size",
  "LZ4 literal range exceeded",
  "LZ4 match range exceeded",
  "Main image exceeds shader bounds",
  "Main segment exceeds native memory image",
  "Malformed storage-buffer reflection",
  "Missing LZ4 terminal sequence",
  "Missing embedded shader helper",
  "Missing explicit facts",
  "Missing real translator export",
  "Missing translator result",
  "Missing/truncated shader control segment",
  "NSO allocation budget exceeded",
  "Noncanonical translator SPIR-V base64",
  "Only one shader conversion may run at a time",
  "Oversized shader metadata",
  "Oversized translator SPIR-V",
  "Pinned bf_want source digest mismatch",
  "Production shader adapter is disposed or already used",
  "Runtime deadline exceeded",
  "Runtime input cap",
  "Runtime not initialized",
  "Runtime operation failed",
  "Runtime unavailable, busy or cancelled",
  "Runtime worker failed",
  "Runtime worker protocol failed",
  "SPH stage disagrees with catalog",
  "SPIRV-Cross provenance mismatch",
  "SPIRV-Cross replay fields mismatch",
  "Sealed operation failed",
  "Shader adapter disposed",
  "Shader already converted in this pipeline",
  "Shader code must be 16 bytes to 1 MiB, 8-byte aligned",
  "Shader conversion cancelled",
  "Shader conversion is still running",
  "Shader conversion timed out",
  "Shader digest input exceeds bounds",
  "Shader key does not belong to pack source",
  "Shader key is outside trusted complete catalog",
  "Shader must be 16 bytes to 1 MiB and 8-byte aligned.",
  "Shader pack did not return bytes",
  "Shader pack source is disposed",
  "Shader pipeline is disposed",
  "Shader result cap",
  "Shader-source memory budget exceeded",
  "Tessellation requires explicit unverified-source-port opt-in",
  "Tessellation requires explicit unverified-source-port opt-in.",
  "Translator resource sidecar does not match declared job stage",
  "Truncated LZ4 block",
  "Trusted complete shader-key catalog required",
  "Trusted native shell chain required",
  "Trusted release shader catalog differs from pinned full catalog",
  "Unauthenticated shader catalog",
  "Unexpected embedded shader completion",
  "Unexpected fact field",
  "Unexpected logical shader pack path",
  "Unknown catalog key",
  "Unknown shader pack source",
  "Unsafe catalog source name",
  "Unsupported production shader source digest",
  "Unsupported shader stage: geometry remains unsupported",
  "Unsupported shader stage; geometry is not integrated.",
  "Unsupported shell SPIRV_CROSS: this compiler is pinned to 1.4.363.0; a matched source/toolchain is required",
  "WebCrypto SHA-256 is unavailable",
  "Worker assets must be self-hosted same-origin module URLs",
]);
const compilerProse =
  /^(?:(?:fatal error|error|warning|note|frontend diagnostic):|use of (?:undeclared|undefined)|unknown (?:type|attribute|argument|register)|no (?:matching|member|type named|viable|template named)|undeclared identifier|invalid |unsupported |cannot |incompatible |expected |too (?:many|few)|candidate |called object |call to |implicit |redefinition |conflicting |non-constant |reference to |assigning to |initializing |passing |variable |function |argument |return (?:type|value)|static assertion failed|expression |["'][^"']{1,160}["'] (?:is |was |does |has |cannot |requires |file not found)|Unresolved external (?:function|global):|assembly and assembly symbol labels are unsupported|Source must contain between|Compiled output exceeded|Metal library serialization failed|Frontend did not produce|No AIR entry point|Unsupported shader stage for a control intrinsic)/i;
const secret =
  /(?:prod\.keys|title\.keys|password|passwd|credential|bearer|(?:api|access|refresh|session)[_ -]?(?:key|token)|master_key|titlekey|private[_ -]?key|BEGIN .*PRIVATE KEY)/i;
// 只有 AIR 前端的 FormatDiagnostic 输出可使用此入口；不是任意异常消息。
// Only AIR frontend FormatDiagnostic output may use this entry point, not arbitrary exceptions.
export function sanitizeAirFrontendDiagnostics(value) {
  return typeof value === "string" ? sanitizeDiagnosticLines(value, true) : Object.freeze([]);
}
export function sanitizeCompilerDiagnostics(value) {
  return sanitizeDiagnosticLines(value, false);
}
function sanitizeDiagnosticLines(value, frontendStream) {
  if (typeof value !== "string" && !Array.isArray(value)) return Object.freeze([]);
  const inputs = typeof value === "string" ? [value] : value.slice(0, 8);
  const output = [];
  let remaining = 2048;
  for (const input of inputs) {
    if (typeof input !== "string") continue;
    for (let line of input.slice(0, 65536).split(/\r?\n/)) {
      if (output.length >= 8 || remaining < 1) break;
      line = line
        .replaceAll("\u001b", "")
        .replace(/\[[0-9;]*m/g, "")
        .trim();
      if (!line || secret.test(line) || /^(?:\d+\s*\||[|^~]|at\s)/.test(line)) continue;
      // 丢弃实际文件名，只保留编译器诊断的位置数字。
      // Discard actual filenames, retaining only diagnostic line/column numbers.
      const located =
        /^(?:.+?):(\d{1,7})(?::(\d{1,7}))?:\s*((?:fatal error|error|warning|note|frontend diagnostic):.*)$/.exec(
          line,
        );
      const location = located ? `source:${located[1]}${located[2] ? ":" + located[2] : ""}: ` : "";
      if (located) line = located[3];
      const recognized = compilerProse.test(line);
      // 前端诊断没有级别前缀；保留未知诊断句子，但不把裸标识符当作正文。
      // Frontend diagnostics have no severity prefix; retain unfamiliar sentences, not bare identifiers.
      const frontendProse = frontendStream && !recognized && /\s/.test(line);
      if (!recognized && !frontendProse) continue;
      if (frontendProse && /^(?:\/\/|\/\*)/.test(line)) continue;
      // 路径、长令牌及代码片段不是诊断正文；绝不转发源码插入符或 IR 行。
      // Paths, long tokens and code snippets are not diagnostic prose; never forward source carets or IR lines.
      line = line
        .replace(/["'][^"'\n]*(?:[\\/])[^"'\n]*["']/g, "'[path]'")
        .replace(/(?:[a-z]+:\/\/|[A-Za-z]:[\\/]|\/)[^\s"'<>]+/gi, "[path]")
        .replace(/(?:[\w.-]+[\\/])+[\w./-]+/g, "[path]")
        .replace(/\b[a-f0-9]{24,}\b/gi, "[redacted]")
        .replace(/[A-Za-z0-9+_/-]{48,}={0,2}/g, "[redacted]");
      line = Array.from(line)
        .filter((character) => {
          const code = character.charCodeAt(0);
          return (
            code >= 32 &&
            !(code >= 127 && code <= 159) &&
            !(code >= 0x202a && code <= 0x202e) &&
            !(code >= 0x2066 && code <= 0x2069)
          );
        })
        .join("");
      const prose = line.replace(/["'][{};=]["']/g, "[punctuation]");
      if (/[{}]|(?:^|:\s)(?:define|declare)\s|(?:^|\s)%\w+\s*=|\s=\s/.test(prose)) continue;
      if (frontendProse && /;\s*$/.test(prose)) continue;
      // 此标签仅说明来源，不把 note 或 warning 误标为 error。
      // This label identifies the producer without mislabeling a note or warning as an error.
      const safe = (location + (frontendProse ? "frontend diagnostic: " : "") + line).slice(
        0,
        Math.min(512, remaining),
      );
      if (safe && !output.includes(safe)) {
        output.push(safe);
        remaining -= safe.length;
      }
    }
  }
  return Object.freeze(output);
}
function safeMessage(value) {
  if (typeof value !== "string" || value.length > 512) return undefined;
  if (messages.has(value)) return value;
  if (
    /^SHADER_SRC mismatch: generated [a-f0-9]{16}, trusted shell [a-f0-9]{16}$/.test(value) ||
    /^Incomplete shader source set: \d{1,5} expected shaders missing$/.test(value) ||
    /^NSO (?:text|ro|data) SHA-256 mismatch$/.test(value) ||
    /^Unsupported catalog stage (?:cs|vs|ps|hs|ds|gs); record retained, full catalog incomplete$/.test(
      value,
    )
  )
    return value;
  return undefined;
}
export function sanitizeShaderDiagnostic(value) {
  if (!value || typeof value !== "object") return undefined;
  const output = {};
  for (const [key, allowed] of [
    ["substep", SHADER_DIAGNOSTIC_SUBSTEPS],
    ["compilerPhase", phases],
    ["errorType", errorTypes],
    ["variant", ["b", "bf"]],
    ["shaderStage", ["cs", "vs", "ps", "hs", "ds", "vertex", "fragment", "kernel"]],
  ])
    if (allowed.includes(value[key])) output[key] = value[key];
  const message = safeMessage(value.message);
  if (message) output.message = message;
  const diagnostics = sanitizeCompilerDiagnostics(value.diagnostics);
  if (diagnostics.length) output.diagnostics = diagnostics;
  if (Number.isSafeInteger(value.record) && value.record >= 1 && value.record <= 6927)
    output.record = value.record;
  if (typeof value.sourceSha256 === "string" && /^[a-f0-9]{64}$/.test(value.sourceSha256))
    output.sourceSha256 = value.sourceSha256;
  if (typeof value.sourceDigest === "string" && /^[a-f0-9]{16}$/.test(value.sourceDigest))
    output.sourceDigest = value.sourceDigest;
  return Object.keys(output).length ? Object.freeze(output) : undefined;
}
export function shaderFailureDiagnostic(error, context = {}) {
  const inherited = sanitizeShaderDiagnostic(error?.shaderDiagnostic);
  const projected = sanitizeShaderDiagnostic({ message: error?.message, errorType: error?.name });
  return sanitizeShaderDiagnostic({ ...context, ...projected, ...inherited });
}
export function withShaderFailureContext(error, context = {}) {
  const result = new Error("Shader operation failed");
  result.name = errorTypes.includes(error?.name) ? error.name : "Error";
  // 固定 AIR 代码由调用边界再次验证；不把任意代码加入消息。
  // Fixed AIR codes are revalidated at the consuming boundary; arbitrary codes never enter messages.
  if (typeof error?.code === "string" && /^AIR_[A-Z_]{1,64}$/.test(error.code))
    result.code = error.code;
  result.shaderDiagnostic = shaderFailureDiagnostic(error, context);
  return result;
}
