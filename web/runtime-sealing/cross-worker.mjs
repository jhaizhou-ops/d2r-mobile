// 封装跨工作线程的运行时请求；依据 ../toolchain/spirv-cross-spike/wwwroot/spirv-cross.mjs、../toolchain/spirv-cross-spike/wwwroot/convert.mjs 的模块与调用约定。
// Wrap cross-worker runtime requests using the module and calling contracts in ../toolchain/spirv-cross-spike/wwwroot/spirv-cross.mjs、../toolchain/spirv-cross-spike/wwwroot/convert.mjs.
import factory from "../toolchain/spirv-cross-spike/wwwroot/spirv-cross.mjs";
import { convertSpirv } from "../toolchain/spirv-cross-spike/wwwroot/convert.mjs";
import { byteFactory, initializationWasm } from "./wasm-factory.mjs";
import { shaderFailureDiagnostic, sanitizeShaderDiagnostic } from "../shader-diagnostics.mjs";
let sealedFactory;
let started = false;
let queue = Promise.resolve();
function conversionDiagnostic(error) {
  // 只有已知转换器的 stderr 信封可提供编译诊断；净化器继续拒绝源码、堆栈和私有路径。
  // Only the converter's known stderr envelope supplies compiler diagnostics; sanitization still rejects source, stacks and private paths.
  const detail = typeof error?.message === "string" ? error.message.slice(0, 65536) : "";
  const compiler = /^SPIRV-Cross exited -?\d{1,6}: ([\s\S]*)$/.exec(detail);
  const diagnostic = shaderFailureDiagnostic(error, {
    substep: "spirv-cross",
    message: "SPIRV-Cross conversion failed",
  });
  return sanitizeShaderDiagnostic({
    ...diagnostic,
    ...(compiler ? { diagnostics: compiler[1] } : {}),
  });
}
self.addEventListener("message", ({ data }) => {
  if (data?.type === "initialize") {
    if (
      started ||
      data.id !== 0 ||
      Object.keys(data).some((key) => !["id", "type", "assets"].includes(key))
    ) {
      self.postMessage({
        id: 0,
        ok: false,
        error: "SEALED_INITIALIZATION_FAILED",
        shaderDiagnostic: shaderFailureDiagnostic(new Error("SEALED_INITIALIZATION_FAILED"), {
          substep: "pipeline-initialization",
        }),
      });
      return;
    }
    started = true;
    try {
      sealedFactory = byteFactory(factory, initializationWasm(data.assets, "spirv-cross.wasm"));
      self.postMessage({ id: 0, ok: true, result: { initialized: true } });
    } catch (error) {
      self.postMessage({
        id: 0,
        ok: false,
        error: "SEALED_INITIALIZATION_FAILED",
        shaderDiagnostic: shaderFailureDiagnostic(error, {
          substep: "pipeline-initialization",
          message: "SEALED_INITIALIZATION_FAILED",
        }),
      });
      self.close();
    }
    return;
  }
  if (!sealedFactory) {
    self.postMessage({
      id: data?.id,
      ok: false,
      error: "CROSS_NOT_INITIALIZED",
      shaderDiagnostic: shaderFailureDiagnostic(new Error("CROSS_NOT_INITIALIZED"), {
        substep: "pipeline-initialization",
      }),
    });
    return;
  }
  queue = queue.then(async () => {
    try {
      if (data?.type !== "convert" || !(data.spirv instanceof ArrayBuffer))
        throw new Error("Invalid conversion request");
      const result = await convertSpirv(new Uint8Array(data.spirv), {
        factory: sealedFactory,
        replayContext: data.replayContext,
      });
      self.postMessage({ id: data.id, ok: true, result });
    } catch (error) {
      self.postMessage({
        id: data?.id,
        ok: false,
        error: "CROSS_CONVERSION_FAILED",
        shaderDiagnostic: conversionDiagnostic(error),
      });
    }
  });
});
