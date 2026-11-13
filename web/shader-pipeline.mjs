// 依据 SPIRV-Cross 契约、反射回放及外壳配方连接浏览器着色器翻译。
// Wire browser shader translation using SPIRV-Cross contracts, reflection replay and shell recipes.
/**
 * 浏览器计算、顶点及片段翻译器到 SPIRV-Cross 的适配器。不包含包写入器。
 * Browser compute/vertex/fragment translator -> SPIRV-Cross adapter. No package writer.
 */
import { shaderFailureDiagnostic } from "./shader-diagnostics.mjs";
import { validateShellContract } from "./shell-contract.mjs";
import {
  CROSS_COMMIT,
  MSL_ARGUMENTS,
  validateSpirv,
} from "./toolchain/spirv-cross-spike/wwwroot/spirv-contract.mjs";
import { replayFields } from "./toolchain/spirv-cross-spike/wwwroot/replay-reflection.mjs";
export const SHADER_PIPELINE_LIMITS = Object.freeze({
  code: 1024 * 1024,
  metadata: 65536,
  spirv: 16 * 1024 * 1024,
  msl: 16 * 1024 * 1024,
  totalMsl: 512 * 1024 * 1024,
  shaders: 16384,
  timeoutMs: 120000,
});
const encoder = new TextEncoder();
const hex = (bytes) => [...bytes].map((x) => x.toString(16).padStart(2, "0")).join("");
const digest = async (bytes) => new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
const hash = async (bytes) => hex(await digest(bytes));
function error(message, name = "Error") {
  const e = new Error(message);
  e.name = name;
  return e;
}
function assetUrl(value, base) {
  const origin = new URL(base),
    url = new URL(value, origin);
  if (
    !["http:", "https:"].includes(url.protocol) ||
    url.origin !== origin.origin ||
    url.username ||
    url.password ||
    url.hash ||
    !url.pathname.endsWith(".mjs")
  )
    throw error("Worker assets must be self-hosted same-origin module URLs");
  return url.href;
}
function decodeBase64(value) {
  if (typeof value !== "string" || value.length > Math.ceil(SHADER_PIPELINE_LIMITS.spirv / 3) * 4)
    throw error("Invalid or oversized translator SPIR-V base64");
  let binary;
  try {
    binary = atob(value);
  } catch {
    throw error("Invalid or oversized translator SPIR-V base64");
  }
  if (binary.length > SHADER_PIPELINE_LIMITS.spirv) throw error("Oversized translator SPIR-V");
  // 精确往返拒绝空白、缺失填充及非零填充位，无需在大输入上反复分组匹配。
  // Exact round-trip rejects whitespace, missing padding and nonzero pad bits without repeated-group matching on large inputs.
  if (btoa(binary) !== value) throw error("Noncanonical translator SPIR-V base64");
  // 直接填充有界字节数组，避免 from(string) 先展开整个字符迭代列表。
  // Fill bounded bytes directly, avoiding from(string)'s intermediate full character iterable list.
  const decoded = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) decoded[i] = binary.charCodeAt(i);
  try {
    return validateSpirv(decoded);
  } finally {
    // 验证器返回独立副本；无论成功与否都清理临时解码缓冲区。
    // Validation returns a separate copy; always wipe the temporary decoded backing.
    decoded.fill(0);
  }
}
function metadata(value) {
  const json = JSON.stringify(value);
  if (typeof json !== "string" || encoder.encode(json).length > SHADER_PIPELINE_LIMITS.metadata)
    throw error("Oversized shader metadata");
  return JSON.parse(json);
}
function resourceFacts(value, key, stage) {
  const r = metadata(value);
  if (
    !r ||
    r.key !== key ||
    r.stage !== stage ||
    !Array.isArray(r.notes) ||
    r.notes.some((x) => typeof x !== "string") ||
    !["sbufs", "cbufs", "textures", "images"].every((k) => Array.isArray(r[k]))
  )
    throw error("Translator resource sidecar does not match declared job stage");
  if (r.notes.length) throw error(`Unsupported translator query/defaults: ${r.notes.join("; ")}`);
  for (const b of r.sbufs)
    if (
      !b ||
      !["binding", "slot", "cb_slot", "cb_offset", "flags"].every(
        (k) => Number.isSafeInteger(b[k]) && b[k] >= 0,
      )
    )
      throw error("Malformed storage-buffer reflection");
  return r;
}
function rpc({ workerFactory, url, request, transfer, signal, timeoutMs, decode }) {
  if (signal.aborted) return Promise.reject(error("Shader conversion cancelled", "AbortError"));
  return new Promise((resolve, reject) => {
    let worker, timer;
    const finish = (failure, value) => {
      clearTimeout(timer);
      signal.removeEventListener("abort", abort);
      if (worker) {
        worker.removeEventListener("message", message);
        worker.removeEventListener("error", failed);
        worker.removeEventListener("messageerror", malformed);
        worker.terminate();
        worker = null;
      }
      if (failure) reject(failure);
      else resolve(value);
    };
    const abort = () => finish(error("Shader conversion cancelled", "AbortError"));
    const failed = (e) =>
      finish(error(`Shader worker failed: ${e.message ?? "startup/runtime error"}`));
    const malformed = () => finish(error("Shader worker returned an unreadable message"));
    const message = ({ data }) => {
      try {
        const reply = decode(data);
        if (reply.matched) finish(null, reply.result);
      } catch (e) {
        finish(e);
      }
    };
    try {
      worker = workerFactory(url, { type: "module" });
      worker.addEventListener("message", message);
      worker.addEventListener("error", failed);
      worker.addEventListener("messageerror", malformed);
      signal.addEventListener("abort", abort, { once: true });
      timer = setTimeout(
        () => finish(error("Shader conversion timed out", "TimeoutError")),
        timeoutMs,
      );
      if (signal.aborted) return abort();
      worker.postMessage(request, transfer);
    } catch (e) {
      finish(e);
    }
  });
}
/**
 * trustedShell 必须来自应用已认证的发布目录，而非仅来自用户编辑的 shell_info。它以完整 shaderKeys 列表扩展 trustedRecipe。现有验证仅针对元数据，不是文件认证。
 * trustedShell must come from the application's authenticated release catalog,
 * not from user-edited shell_info alone. It extends trustedRecipe with the full
 * shaderKeys list. Existing validation is metadata-only, not file authentication.
 */
export function createShaderPipeline({
  trustedShell,
  translatorWorkerUrl,
  crossWorkerUrl,
  baseUrl = globalThis.location?.href,
  workerFactory = (url, options) => new Worker(url, options),
  timeoutMs = SHADER_PIPELINE_LIMITS.timeoutMs,
}) {
  validateShellContract(trustedShell);
  const recipe = trustedShell.trustedRecipe;
  const shellCross = /^SPIRV_CROSS=([^\r\n]+)$/m.exec(trustedShell.shellInfoText)?.[1];
  if (recipe.spirvCrossVersion !== "1.4.363.0" || shellCross !== recipe.spirvCrossVersion)
    throw error(
      "Unsupported shell SPIRV_CROSS: this compiler is pinned to 1.4.363.0; a matched source/toolchain is required",
    );
  const keys = recipe.shaderKeys;
  if (
    !Array.isArray(keys) ||
    !keys.length ||
    keys.length > SHADER_PIPELINE_LIMITS.shaders ||
    keys.some((k) => typeof k !== "string" || !/^[0-9a-f]{16}$/.test(k)) ||
    new Set(keys).size !== keys.length
  )
    throw error("Trusted complete shader-key catalog required");
  if (typeof recipe.chain !== "string" || !/^[0-9a-f]{12}$/.test(recipe.chain))
    throw error("Trusted native shell chain required");
  if (
    !Number.isSafeInteger(timeoutMs) ||
    timeoutMs < 1 ||
    timeoutMs > SHADER_PIPELINE_LIMITS.timeoutMs
  )
    throw error("Invalid shader timeout");
  const contract = Object.freeze({
    chain: recipe.chain,
    shaderDigest: recipe.shaderDigest,
    keys: Object.freeze([...keys].sort()),
  });
  const allowed = new Set(contract.keys),
    completed = new Map();
  const translatorUrl = assetUrl(translatorWorkerUrl, baseUrl),
    crossUrl = assetUrl(crossWorkerUrl, baseUrl);
  let disposed = false,
    active = null,
    sequence = 0,
    totalMsl = 0;
  const ensureOpen = () => {
    if (disposed) throw error("Shader pipeline is disposed");
  };
  return Object.freeze({
    async translate({ job, code, signal } = {}) {
      ensureOpen();
      if (active) throw error("Only one shader conversion may run at a time");
      const requestJob = metadata(job);
      if (!requestJob || !["cs", "vs", "ps", "hs", "ds"].includes(requestJob.stage))
        throw error("Unsupported shader stage: geometry remains unsupported");
      const tessellation = ["hs", "ds"].includes(requestJob.stage);
      if (tessellation && requestJob.allowUnverifiedTessellation !== true)
        throw error("Tessellation requires explicit unverified-source-port opt-in");
      if (!allowed.has(requestJob.key))
        throw error("Shader key is outside trusted complete catalog");
      if (completed.has(requestJob.key)) throw error("Shader already converted in this pipeline");
      if (typeof requestJob.debug !== "boolean" || typeof requestJob.reduced !== "boolean")
        throw error("Explicit native debug/reduced translation options required");
      if (
        !(code instanceof Uint8Array) ||
        code.length < 16 ||
        code.length > SHADER_PIPELINE_LIMITS.code ||
        code.length % 8
      )
        throw error("Shader code must be 16 bytes to 1 MiB, 8-byte aligned");
      let hullOutputVertices = null;
      if (requestJob.stage !== "cs") {
        if (code.length < 0x60)
          throw error("Graphics input requires a 0x50-byte SPH plus instructions");
        const headerStage =
          (new DataView(code.buffer, code.byteOffset, code.byteLength).getUint32(0, true) >>> 10) &
          15;
        if (headerStage !== { vs: 1, hs: 2, ds: 3, ps: 5 }[requestJob.stage])
          throw error("Declared graphics stage does not match SPH stage bits 10..13");
        if (requestJob.stage === "hs")
          hullOutputVertices =
            new DataView(code.buffer, code.byteOffset, code.byteLength).getUint32(8, true) >>> 24;
        if (requestJob.stage === "hs" && hullOutputVertices === 0)
          throw error("Hull SPH requires nonzero threads per input primitive");
        for (const [name, limit] of [
          ["attr", 32],
          ["fout", 8],
        ]) {
          const types = requestJob[name];
          if (!types || typeof types !== "object" || Array.isArray(types))
            throw error("Graphics jobs require explicit attr and fout type tables");
          for (const [slot, type] of Object.entries(types))
            if (
              !/^(0|[1-9][0-9]*)$/.test(slot) ||
              Number(slot) >= limit ||
              !["float", "sint", "uint"].includes(type)
            )
              throw error("Invalid graphics attribute/output component type or slot");
        }
      }
      if (signal?.aborted) throw error("Shader conversion cancelled", "AbortError");
      const controller = new AbortController();
      active = controller;
      const abort = () => controller.abort();
      signal?.addEventListener("abort", abort, { once: true });
      const deadline = Date.now() + timeoutMs,
        requestId = ++sequence;
      const remaining = () => Math.max(1, deadline - Date.now());
      let bytes, jobBytes, spirv, mslBytes;
      let substep = "translation";
      try {
        // Buffer.slice() 与输入共享内存；始终分配独立的普通字节副本。
        // Buffer.slice() aliases its input; always allocate a plain owned byte copy.
        bytes = new Uint8Array(code);
        const codeSha256 = await hash(bytes);
        jobBytes = encoder.encode(JSON.stringify(requestJob));
        const jobSha256 = await hash(jobBytes);
        const translated = await rpc({
          workerFactory,
          url: translatorUrl,
          signal: controller.signal,
          timeoutMs: remaining(),
          request: { requestId, job: requestJob, code: bytes },
          transfer: [bytes.buffer],
          decode(data) {
            if (data?.requestId !== requestId) return { matched: false };
            if (data.error) {
              const failure = error("Shader translation failed");
              failure.shaderDiagnostic = shaderFailureDiagnostic(data, { substep: "translation" });
              throw failure;
            }
            if (!data.result) throw error("Missing translator result");
            return { matched: true, result: data.result };
          },
        });
        const resources = resourceFacts(translated.resources, requestJob.key, requestJob.stage);
        spirv = decodeBase64(translated.spirvBase64);
        const spirvSha256 = await hash(spirv);
        substep = "spirv-cross";
        const converted = await rpc({
          workerFactory,
          url: crossUrl,
          signal: controller.signal,
          timeoutMs: remaining(),
          request: {
            id: requestId,
            type: "convert",
            spirv: spirv.buffer,
            replayContext: { stage: requestJob.stage, resources },
          },
          transfer: [spirv.buffer],
          decode(data) {
            if (data?.id !== requestId) return { matched: false };
            if (data.ok !== true) {
              const failure = error("SPIRV-Cross conversion failed");
              failure.shaderDiagnostic = shaderFailureDiagnostic(data, { substep: "spirv-cross" });
              throw failure;
            }
            return { matched: true, result: data.result };
          },
        });
        substep = "reflection-validation";
        if (
          !converted ||
          typeof converted.msl !== "string" ||
          converted.msl.length > SHADER_PIPELINE_LIMITS.msl ||
          converted.msl.includes("\r")
        )
          throw error("Invalid or oversized LF Metal source");
        mslBytes = encoder.encode(converted.msl);
        if (
          mslBytes.length > SHADER_PIPELINE_LIMITS.msl ||
          totalMsl + mslBytes.length > SHADER_PIPELINE_LIMITS.totalMsl
        )
          throw error("Shader-source memory budget exceeded");
        const sourceHash = await digest(mslBytes),
          provenance = converted.provenance;
        if (
          converted.schema !== "spirv-cross-browser-stage-v1" ||
          provenance?.spirvCrossCommit !== CROSS_COMMIT ||
          JSON.stringify(provenance.mslArguments) !== JSON.stringify(MSL_ARGUMENTS) ||
          provenance.inputSha256 !== spirvSha256 ||
          provenance.mslSha256 !== hex(sourceHash) ||
          provenance.nativeChainClaimed !== false ||
          provenance.metalCompiled !== false
        )
          throw error("SPIRV-Cross provenance mismatch");
        const replay = replayFields(requestJob.stage, converted.msl, resources);
        const mode = { cs: "comp", vs: "vert", ps: "frag", hs: "tesc", ds: "tese" }[
          requestJob.stage
        ];
        if (
          converted.reflection?.entryPoints?.length !== 1 ||
          converted.reflection.entryPoints[0].mode !== mode ||
          replay.msl_entry_kind !==
            { cs: "kernel", vs: "vertex", ps: "fragment", hs: "kernel", ds: "vertex" }[
              requestJob.stage
            ]
        )
          throw error("Compiled SPIR-V/MSL stage does not match declared job");
        if (
          requestJob.stage === "ds" &&
          !/\[\[\s*patch\(\s*triangle(?:\s*,\s*\d+)?\s*\)\s*\]\]/.test(converted.msl)
        )
          throw error("Domain MSL must preserve the original triangle patch contract");
        if (JSON.stringify(replay) !== JSON.stringify(converted.replay))
          throw error("SPIRV-Cross replay fields mismatch");
        if (controller.signal.aborted || disposed)
          throw error("Shader conversion cancelled", "AbortError");
        if (Date.now() > deadline) throw error("Shader conversion timed out", "TimeoutError");
        completed.set(requestJob.key, sourceHash);
        totalMsl += mslBytes.length;
        return {
          status: "shader-source-candidate",
          key: requestJob.key,
          stage: requestJob.stage,
          stageSupport: tessellation ? "source-ported-unverified" : "synthetic-parity-verified",
          tessellation: tessellation
            ? {
                nativeParityVerified: false,
                outputVertices: hullOutputVertices,
                domain: requestJob.stage === "ds" ? "triangles" : null,
                spacing: requestJob.stage === "ds" ? "equal" : null,
                stageLinkageVerified: false,
              }
            : null,
          threadgroup_size: requestJob.stage === "cs" ? (requestJob.local ?? null) : null,
          sourceName: `${requestJob.key}_b.metal`,
          msl: converted.msl,
          resources,
          replay,
          spirvReflection: converted.reflection,
          provenance: { ...provenance, codeSha256, jobSha256 },
          shellChain: contract.chain,
          shaderSourceSetVerified: false,
          installable: false,
          metalCompiled: false,
        };
      } catch (failure) {
        failure.shaderDiagnostic = shaderFailureDiagnostic(failure, { substep });
        throw failure;
      } finally {
        // 清理仍由本地持有的快照；真实 Worker 已转移的缓冲区不可再写入。
        // Wipe locally owned snapshots; real-worker transfers may have detached their buffers.
        for (const owned of [bytes, jobBytes, spirv, mslBytes])
          if (owned?.byteLength) owned.fill(0);
        signal?.removeEventListener("abort", abort);
        active = null;
      }
    },
    async verifyCompleteSourceSet() {
      ensureOpen();
      if (active) throw error("Shader conversion is still running");
      const missing = contract.keys.filter((k) => !completed.has(k));
      if (missing.length)
        throw error(`Incomplete shader source set: ${missing.length} expected shaders missing`);
      // 精确的 d2rm_pack.py src_digest：排序后文件名的 UTF-8 加上原始 SHA256(source)，
      // 然后取该拼接结果 SHA256 的前 16 个十六进制字符。
      // Exact d2rm_pack.py src_digest: sorted filename UTF-8 + raw SHA256(source),
      // then first 16 hex digits of SHA256 over that concatenation.
      const input = buildShaderSourceDigestInput(contract.keys, completed);
      const sourceDigest = (await hash(input)).slice(0, 16);
      ensureOpen();
      if (sourceDigest !== contract.shaderDigest)
        throw error(
          `SHADER_SRC mismatch: generated ${sourceDigest}, trusted shell ${contract.shaderDigest}`,
        );
      return {
        status: "shader-source-set-equivalent",
        sourceDigest,
        shellChain: contract.chain,
        shaderCount: completed.size,
        installable: false,
        metalCompiled: false,
        chainRewritten: false,
      };
    },
    dispose() {
      disposed = true;
      active?.abort();
      completed.clear();
      totalMsl = 0;
    },
  });
}

// 固定 ASCII 文件名和原始摘要直接写入单个缓冲区，不建立逐记录副本。
// Write fixed ASCII filenames and raw digests directly into one buffer, without per-record copies.
// 6927 项只分配一个 387912 字节输入；这是显式分配计数，不是浏览器 RAM 测量。
// 6927 records allocate one 387912-byte input; this is explicit allocation accounting, not browser RAM measurement.
export function buildShaderSourceDigestInput(keys, completed) {
  const recordBytes = 56,
    nameBytes = 24;
  if (!Array.isArray(keys) || keys.length < 1 || keys.length > SHADER_PIPELINE_LIMITS.shaders)
    throw error("Invalid shader digest input count");
  const size = keys.length * recordBytes;
  if (!Number.isSafeInteger(size) || size > SHADER_PIPELINE_LIMITS.shaders * recordBytes)
    throw error("Shader digest input exceeds bounds");
  const input = new Uint8Array(size);
  let offset = 0,
    previous;
  for (const key of keys) {
    const sourceHash = completed.get(key);
    if (
      typeof key !== "string" ||
      !/^[0-9a-f]{16}$/.test(key) ||
      (previous !== undefined && previous >= key) ||
      !(sourceHash instanceof Uint8Array) ||
      sourceHash.length !== 32 ||
      offset > size - recordBytes
    )
      throw error("Invalid sorted shader digest record");
    const written = encoder.encodeInto(
      `${key}_b.metal`,
      input.subarray(offset, offset + nameBytes),
    );
    if (written.read !== nameBytes || written.written !== nameBytes)
      throw error("Invalid shader digest filename encoding");
    input.set(sourceHash, offset + nameBytes);
    offset += recordBytes;
    previous = key;
  }
  if (offset !== size) throw error("Incomplete shader digest input");
  return input;
}
