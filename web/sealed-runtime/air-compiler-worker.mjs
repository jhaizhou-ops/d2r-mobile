// 已验证父级传入编译后的 WASM；本工作线程从不获取编译器、源码或辅助文件。
// The verified parent supplies compiled WASM; this worker never fetches code, source or helper files.
import { shaderFailureDiagnostic } from "../shader-diagnostics.mjs";
import { createAirCompilerAdapter } from "../air-compiler-adapter.mjs";
import { inspectCompiledMetalLibrary } from "../compiled-shader-boundary.mjs";
import {
  AIR_COMPILER_LIMITS as limits,
  AIR_COMPILER_OPTIONS,
  AIR_COMPILER_SOURCE_LANGUAGE,
  airCompilerFailure,
  airCompilerFailureCode,
  airCompilerIdentity,
  validateAirCompilerRequest,
} from "../air-compiler-contract.mjs";

const scope = globalThis;
let adapter,
  module,
  initializing = false,
  active = false,
  retired = false,
  sourceBytes = 0,
  compilerPhase = "initializing";
const fail = (code) => {
  throw airCompilerFailure(code);
};
const progress = (id, phase) => {
  compilerPhase = phase;
  scope.postMessage({ type: "progress", id, phase });
};
const hex = (bytes) => Array.from(bytes, (x) => x.toString(16).padStart(2, "0")).join("");

function disableNetwork() {
  const blocked = () => fail("AIR_NETWORK_FORBIDDEN");
  for (const name of [
    "fetch",
    "XMLHttpRequest",
    "WebSocket",
    "EventSource",
    "importScripts",
    "Worker",
    "SharedWorker",
  ])
    Object.defineProperty(scope, name, { value: blocked, configurable: false, writable: false });
}
function heapBytes() {
  // 每次编译器调用或内存增长后，重新获取当前公开视图。
  // Reacquire the current public view after every compiler call or memory growth.
  const bytes = module?.HEAPU8;
  if (!(bytes instanceof Uint8Array) || bytes.byteLength > limits.maxObservedHeapBytes)
    fail("AIR_MEMORY_LIMIT");
  return bytes.byteLength;
}
async function initialize(data) {
  if (adapter || initializing || retired) fail("AIR_PROTOCOL_FAILED");
  initializing = true;
  const identity = airCompilerIdentity(data);
  let factoryUrl;
  try {
    if (
      !(data.factoryBytes instanceof ArrayBuffer) ||
      data.factoryBytes.resizable ||
      !data.factoryBytes.byteLength ||
      data.factoryBytes.byteLength > limits.factoryBytes ||
      !(data.wasmModule instanceof WebAssembly.Module)
    )
      fail("AIR_ASSET_INVALID");
    // 模块来源是父线程对已验证不可变字节的编译，而不是用户模块。
    // Module provenance is the parent's verified immutable-byte compilation, not a user module.
    const actual = hex(new Uint8Array(await crypto.subtle.digest("SHA-256", data.factoryBytes)));
    if (actual !== identity.factorySha256) fail("AIR_ASSET_MISMATCH");
    disableNetwork();
    factoryUrl = URL.createObjectURL(new Blob([data.factoryBytes], { type: "text/javascript" }));
    const { default: factory } = await import(factoryUrl);
    if (typeof factory !== "function") fail("AIR_ASSET_INVALID");
    let instantiations = 0;
    module = await factory({
      instantiateWasm(imports, receive) {
        if (++instantiations !== 1) fail("AIR_PROTOCOL_FAILED");
        const instance = new WebAssembly.Instance(data.wasmModule, imports);
        receive(instance, data.wasmModule);
        return instance.exports;
      },
      locateFile() {
        fail("AIR_NETWORK_FORBIDDEN");
      },
      print() {},
      printErr() {},
      onAbort() {
        fail("AIR_RUNTIME_FAILED");
      },
    });
    if (instantiations !== 1) fail("AIR_PROTOCOL_FAILED");
    adapter = createAirCompilerAdapter(module, {
      expectedHeaderSha256: identity.headerSha256,
      apiVersion: 2,
      maxCompilations: limits.batchCompilations,
    });
    scope.postMessage({ type: "initialized", id: data.id, heapBytes: heapBytes() });
  } finally {
    if (data.factoryBytes instanceof ArrayBuffer) new Uint8Array(data.factoryBytes).fill(0);
    data.factoryBytes = undefined;
    data.wasmModule = undefined;
    if (factoryUrl) URL.revokeObjectURL(factoryUrl);
    initializing = false;
  }
}
async function compile(data) {
  if (!adapter || initializing || retired) fail("AIR_PROTOCOL_FAILED");
  let output;
  try {
    const size = validateAirCompilerRequest(data.source, data.entry, data.stage);
    if (size > limits.batchSourceBytes - sourceBytes) fail("AIR_BATCH_LIMIT");
    progress(data.id, "compiling");
    output = adapter.compile(data.source, {
      iosMajor: 18,
      output: "metallib",
      sourceLanguage: AIR_COMPILER_SOURCE_LANGUAGE,
      mathProfile: "fast-f32",
    });
    data.source = undefined;
    if (output.bytes.length > limits.outputBytes) fail("AIR_OUTPUT_LIMIT");
    const heap = heapBytes();
    sourceBytes += size;
    progress(data.id, "validating");
    let inspection;
    try {
      inspection = await inspectCompiledMetalLibrary(output.bytes, {
        entry: data.entry,
        stage: data.stage,
        ...AIR_COMPILER_OPTIONS,
        metadataLengthIncludesPrefix: false,
      });
    } catch (error) {
      throw airCompilerFailure("AIR_OUTPUT_INVALID", error, { substep: "output-validation" });
    }
    retired =
      output.requiresWorkerTermination ||
      sourceBytes >= limits.batchSourceBytes - limits.sourceBytes ||
      heap >= limits.recycleHeapBytes;
    scope.postMessage(
      {
        type: "compiled",
        id: data.id,
        bytes: output.bytes.buffer,
        inspection,
        heapBytes: heap,
        compilationCount: adapter.compilationCount,
        recycle: retired,
      },
      [output.bytes.buffer],
    );
    output = undefined;
    if (retired) {
      adapter.dispose();
      scope.close();
    }
  } finally {
    data.source = undefined;
    output?.bytes.fill(0);
  }
}

scope.addEventListener("message", async ({ data }) => {
  if (retired) return;
  if (active) {
    scope.postMessage({ type: "error", id: data?.id, code: "AIR_COMPILER_BUSY" });
    adapter?.dispose();
    retired = true;
    scope.close();
    return;
  }
  active = true;
  try {
    if (!Number.isSafeInteger(data?.id) || data.id < 1) fail("AIR_PROTOCOL_FAILED");
    if (data.type === "initialize") await initialize(data);
    else if (data.type === "compile") await compile(data);
    else fail("AIR_PROTOCOL_FAILED");
  } catch (error) {
    // 只传递有界且已过滤的诊断，不传源码、路径或原始异常。
    // Transfer only bounded filtered diagnostics, never source, paths or raw exceptions.
    scope.postMessage({
      type: "error",
      id: data?.id,
      code: airCompilerFailureCode(error?.code),
      shaderDiagnostic: shaderFailureDiagnostic(error, { compilerPhase }),
    });
    adapter?.dispose();
    retired = true;
    scope.close();
  } finally {
    active = false;
  }
});
