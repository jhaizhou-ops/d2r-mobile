// 开发者浏览器冒烟测试：使用真实编译器字节，不接入生产 IPA 流程。
// Developer browser smoke worker: uses real compiler bytes, not the production IPA pipeline.
// 调用方须锁定构建产物指纹；完成、失败或取消后必须终止此单次 Worker。
// The caller must pin build fingerprints and terminate this one-shot Worker after completion, failure or cancellation.
import { createAirCompilerAdapter } from "../../air-compiler-adapter.mjs";
import { inspectCompiledMetalLibrary } from "../../compiled-shader-boundary.mjs";

let started = false;
const hex = (bytes) => Array.from(bytes, (x) => x.toString(16).padStart(2, "0")).join("");
async function verify(bytes, expected) {
  if (
    !(bytes instanceof ArrayBuffer) ||
    bytes.byteLength === 0 ||
    typeof expected !== "string" ||
    !/^[a-f0-9]{64}$/.test(expected)
  )
    throw new Error("AIR_SMOKE_ASSET_INVALID");
  const actual = hex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));
  if (actual !== expected) throw new Error("AIR_SMOKE_ASSET_MISMATCH");
}

self.addEventListener("message", async ({ data }) => {
  if (started) return;
  started = true;
  let url, adapter, output;
  try {
    await verify(data?.factoryBytes, data?.factorySha256);
    await verify(data?.wasmBytes, data?.wasmSha256);
    if (!["vertex", "fragment", "kernel"].includes(data.stage))
      throw new Error("AIR_SMOKE_STAGE_INVALID");
    const start = performance.now();
    const compiled = await WebAssembly.compile(data.wasmBytes);
    url = URL.createObjectURL(new Blob([data.factoryBytes], { type: "text/javascript" }));
    const { default: factory } = await import(url);
    let instantiations = 0;
    const module = await factory({
      wasmBinary: new Uint8Array(data.wasmBytes),
      instantiateWasm(imports, receive) {
        if (++instantiations !== 1) throw new Error("AIR_SMOKE_REPEATED_INSTANTIATION");
        const instance = new WebAssembly.Instance(compiled, imports);
        receive(instance, compiled);
        return instance.exports;
      },
      locateFile() {
        throw new Error("AIR_SMOKE_UNEXPECTED_ASSET_REQUEST");
      },
      print() {},
      printErr() {},
    });
    if (instantiations !== 1) throw new Error("AIR_SMOKE_UNVERIFIED_INSTANTIATION");
    adapter = createAirCompilerAdapter(module, {
      expectedHeaderSha256: data.headerSha256,
      apiVersion: data.apiVersion,
    });
    const ready = performance.now();
    output = adapter.compile(data.source);
    const finished = performance.now();
    const inspection = await inspectCompiledMetalLibrary(output.bytes, {
      entry: data.entry,
      stage: data.stage,
      minimumOS: [18, 0],
      languageVersion: [3, 2],
      metadataLengthIncludesPrefix: false,
    });
    self.postMessage({
      ok: true,
      inspection,
      initializationMs: ready - start,
      compilationMs: finished - ready,
      heapBytes: module.HEAPU8.byteLength,
      instantiations,
      // 结构检查不等于 Apple 运行时验收或完整游戏兼容。
      // Structural inspection is not Apple runtime acceptance or full-game compatibility.
      validation: "browser-compilation-and-container-integrity-only",
    });
  } catch (error) {
    // 不将原始编译器异常、源码或标识符传回 UI。
    // Never return raw compiler exceptions, source or identifiers to the UI.
    const code =
      typeof error?.code === "string" && /^AIR_[A-Z_]+$/.test(error.code)
        ? error.code
        : "AIR_SMOKE_FAILED";
    self.postMessage({ ok: false, code });
  } finally {
    output?.bytes.fill(0);
    adapter?.dispose();
    if (url) URL.revokeObjectURL(url);
    self.close();
  }
});
