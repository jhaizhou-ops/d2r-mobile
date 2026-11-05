// 密封加载器拥有工作线程代码的认证；本客户端验证公开编译器副本并复用有限批次。
// The sealed loader authenticates worker code; this client verifies public compiler snapshots and reuses bounded batches.
import {
  AIR_COMPILER_LIMITS as limits,
  AIR_COMPILER_OPTIONS,
  AIR_COMPILER_PHASES,
  airCompilerFailure,
  airCompilerIdentity,
  validateAirCompilerRequest,
} from "./air-compiler-contract.mjs";

const hash = async (bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (x) =>
    x.toString(16).padStart(2, "0"),
  ).join("");
const fail = (code) => {
  throw airCompilerFailure(code);
};
function report(callback, phase) {
  try {
    Promise.resolve(callback?.(Object.freeze({ phase }))).catch(() => {});
  } catch {
    /* 同步抛错、拒绝的 Promise 和 thenable 都不能破坏所有权约定。 */
    /* Synchronous throws, rejected promises and thenables cannot break ownership. */
  }
}
function snapshot(value, maximum) {
  if (value instanceof ArrayBuffer) value = new Uint8Array(value);
  if (
    !(value instanceof Uint8Array) ||
    !(value.buffer instanceof ArrayBuffer) ||
    value.buffer.resizable ||
    !value.length ||
    value.length > maximum
  )
    fail("AIR_ASSET_INVALID");
  return new Uint8Array(value);
}
function check(signal) {
  if (signal?.aborted) fail("AIR_ABORTED");
}
async function waitFor(promise, signal, timeout, code) {
  let timer, cancel;
  try {
    check(signal);
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(airCompilerFailure(code)), timeout);
        cancel = () => reject(airCompilerFailure("AIR_ABORTED"));
        signal?.addEventListener("abort", cancel, { once: true });
        if (signal?.aborted) cancel();
      }),
    ]);
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener("abort", cancel);
  }
}

// 独立 WASM 堆不需要共享内存；仅按保守设备提示选择最多四个工作线程。
// Independent WASM heaps need no shared memory; conservative device hints cap the pool at four workers.
export function selectAirCompilerConcurrency(
  { hardwareConcurrency, deviceMemory } = globalThis.navigator ?? {},
) {
  const cores =
    Number.isFinite(hardwareConcurrency) && hardwareConcurrency >= 1
      ? Math.floor(hardwareConcurrency)
      : 2;
  const memoryCap =
    Number.isFinite(deviceMemory) && deviceMemory > 0
      ? deviceMemory >= 8
        ? 4
        : deviceMemory >= 4
          ? 2
          : 1
      : 2;
  return Math.max(1, Math.min(4, cores, memoryCap));
}

export function createAirCompilerWorkerClient(options = {}) {
  return createCompilerClients(options, 1, false);
}

export function createAirCompilerWorkerPool(options = {}) {
  return createCompilerClients(options, selectAirCompilerConcurrency(options.capabilities), true);
}

// 所有工作线程在返回前完成初始化，共享已验证公开模块但不共享编译器实例或私有源码队列。
// Every worker initializes before return, sharing verified public code but neither compiler instances nor private source queues.
async function createCompilerClients(
  { createWorker, assets, signal, onProgress } = {},
  concurrency,
  failTogether,
) {
  check(signal);
  if (typeof createWorker !== "function" || !assets) fail("AIR_ASSET_INVALID");
  const compiler = airCompilerIdentity(assets);
  let factoryBytes,
    wasmBytes,
    wasmModule,
    disposed = false,
    poolReady = false,
    startupFailure,
    compilerPhase = "verifying";
  const lanes = [];
  function dispose() {
    if (disposed) return;
    disposed = true;
    for (const lane of lanes) lane.stop(airCompilerFailure("AIR_COMPILER_DISPOSED"));
    factoryBytes?.fill(0);
    factoryBytes = undefined;
    wasmModule = undefined;
  }
  function ensureLive(requestSignal) {
    if (startupFailure) throw startupFailure;
    if (disposed) fail("AIR_COMPILER_DISPOSED");
    check(requestSignal);
  }
  function createLane() {
    let current,
      active = false,
      nextId = 0,
      compilerPhase = "initializing",
      observedHeap = 0;
    function stop(reason) {
      const state = current;
      current = undefined;
      if (!state) return;
      state.failure = reason;
      state.worker.removeEventListener("message", state.message);
      state.worker.removeEventListener("error", state.error);
      state.worker.removeEventListener("messageerror", state.error);
      state.worker.terminate();
      state.pending?.reject(reason ?? airCompilerFailure("AIR_WORKER_FAILED"));
      state.pending = undefined;
      if (reason && failTogether && !disposed && (poolReady || state.initialized)) {
        if (!poolReady) startupFailure = reason;
        dispose();
      }
    }
    function connect() {
      let worker;
      try {
        worker = createWorker();
      } catch {
        fail("AIR_WORKER_UNAVAILABLE");
      }
      if (
        !worker ||
        ["addEventListener", "removeEventListener", "postMessage", "terminate"].some(
          (method) => typeof worker[method] !== "function",
        )
      ) {
        worker?.terminate?.();
        fail("AIR_WORKER_UNAVAILABLE");
      }
      const state = { worker, pending: undefined, initialized: false };
      state.error = (event) => {
        event?.preventDefault?.();
        if (current === state) stop(airCompilerFailure("AIR_WORKER_FAILED"));
      };
      state.message = ({ data }) => {
        const pending = state.pending;
        if (current !== state || !pending || data?.id !== pending.id) {
          if (data?.bytes instanceof ArrayBuffer) new Uint8Array(data.bytes).fill(0);
          if (current === state) stop(airCompilerFailure("AIR_PROTOCOL_FAILED"));
          return;
        }
        if (data.type === "progress") {
          if (!AIR_COMPILER_PHASES.includes(data.phase)) {
            stop(airCompilerFailure("AIR_PROTOCOL_FAILED"));
            return;
          }
          compilerPhase = data.phase;
          report(pending.onProgress, data.phase);
        } else if (data.type === "error") {
          stop(airCompilerFailure(data.code, data, { compilerPhase }));
        } else {
          state.pending = undefined;
          pending.resolve(data);
        }
      };
      worker.addEventListener("message", state.message);
      worker.addEventListener("error", state.error);
      worker.addEventListener("messageerror", state.error);
      current = state;
      return state;
    }
    async function request(
      state,
      message,
      transfers,
      requestSignal,
      callback,
      initialization = false,
    ) {
      ensureLive(requestSignal);
      if (current !== state || state.pending) fail("AIR_COMPILER_BUSY");
      let timer, abort;
      try {
        return await new Promise((resolve, reject) => {
          const id = ++nextId;
          state.pending = { id, resolve, reject, onProgress: callback };
          abort = () => stop(airCompilerFailure("AIR_ABORTED"));
          requestSignal?.addEventListener("abort", abort, { once: true });
          timer = setTimeout(
            () =>
              stop(
                airCompilerFailure(
                  initialization ? "AIR_INITIALIZATION_TIMEOUT" : "AIR_COMPILATION_TIMEOUT",
                ),
              ),
            initialization ? limits.initializationMs : limits.compilationMs,
          );
          if (requestSignal?.aborted) {
            abort();
            return;
          }
          try {
            state.worker.postMessage({ ...message, id }, transfers);
          } catch {
            stop(airCompilerFailure("AIR_PROTOCOL_FAILED"));
          }
        });
      } finally {
        clearTimeout(timer);
        requestSignal?.removeEventListener("abort", abort);
      }
    }
    async function start(requestSignal, callback) {
      ensureLive(requestSignal);
      if (current?.initialized) return current;
      compilerPhase = "initializing";
      report(callback, "initializing");
      ensureLive(requestSignal);
      const state = connect();
      const copy = factoryBytes.slice();
      try {
        const result = await request(
          state,
          {
            type: "initialize",
            factoryBytes: copy.buffer,
            wasmModule,
            factorySha256: compiler.factorySha256,
            wasmSha256: compiler.assetSha256,
            headerSha256: compiler.headerSha256,
          },
          [copy.buffer],
          requestSignal,
          callback,
          true,
        );
        ensureLive(requestSignal);
        if (current !== state) throw state.failure ?? airCompilerFailure("AIR_WORKER_FAILED");
        if (
          result?.type !== "initialized" ||
          !Number.isSafeInteger(result.heapBytes) ||
          result.heapBytes < 1 ||
          result.heapBytes > limits.maxObservedHeapBytes
        )
          fail("AIR_PROTOCOL_FAILED");
        observedHeap = result.heapBytes;
        state.initialized = true;
        return state;
      } catch (error) {
        stop(error);
        throw error;
      } finally {
        if (copy.byteLength) copy.fill(0);
      }
    }
    return Object.freeze({
      get active() {
        return active;
      },
      get heapBytes() {
        return current ? observedHeap : 0;
      },
      start,
      stop,
      async compile(source, { entry, stage, signal: requestSignal, onProgress: callback } = {}) {
        ensureLive(requestSignal);
        if (active) fail("AIR_COMPILER_BUSY");
        validateAirCompilerRequest(source, entry, stage);
        active = true;
        let bytes;
        try {
          const state = await start(requestSignal, callback);
          compilerPhase = "compiling";
          const result = await request(
            state,
            { type: "compile", source, entry, stage },
            [],
            requestSignal,
            callback,
          );
          source = undefined;
          if (result?.bytes instanceof ArrayBuffer) bytes = new Uint8Array(result.bytes);
          ensureLive(requestSignal);
          if (
            result?.type !== "compiled" ||
            !bytes ||
            bytes.buffer.resizable ||
            bytes.length < 88 ||
            bytes.length > limits.outputBytes ||
            typeof result.recycle !== "boolean" ||
            !Number.isSafeInteger(result.compilationCount) ||
            result.compilationCount < 1 ||
            result.compilationCount > limits.batchCompilations ||
            !Number.isSafeInteger(result.heapBytes) ||
            result.heapBytes < 1 ||
            result.heapBytes > limits.maxObservedHeapBytes ||
            result.inspection?.validation !== "container-and-integrity-only" ||
            result.inspection.outputSize !== bytes.length
          )
            fail("AIR_PROTOCOL_FAILED");
          observedHeap = result.heapBytes;
          if (
            failTogether &&
            lanes.reduce((total, lane) => total + lane.heapBytes, 0) > 1024 * 1024 * 1024
          )
            fail("AIR_MEMORY_LIMIT");
          if (result.recycle) stop();
          report(callback, "complete");
          ensureLive(requestSignal);
          const output = Object.freeze({
            bytes,
            compiler,
            options: AIR_COMPILER_OPTIONS,
            inspection: result.inspection,
          });
          bytes = undefined;
          return output;
        } catch (error) {
          stop(error);
          if (failTogether) dispose();
          throw airCompilerFailure(error?.code, error, { compilerPhase });
        } finally {
          bytes?.fill(0);
          source = undefined;
          active = false;
        }
      },
    });
  }
  try {
    factoryBytes = snapshot(assets.factoryBytes, limits.factoryBytes);
    wasmBytes = snapshot(assets.wasmBytes, limits.wasmBytes);
    report(onProgress, "verifying");
    // 准备阶段不含私有输入；取消会放弃结果，但 WebAssembly.compile 本身不可取消。
    // Preparation has no private input. Abort abandons its result; WebAssembly.compile itself is not cancellable.
    const preparation = (async () => {
      if (
        (await hash(factoryBytes)) !== compiler.factorySha256 ||
        (await hash(wasmBytes)) !== compiler.assetSha256
      )
        fail("AIR_ASSET_MISMATCH");
      check(signal);
      compilerPhase = "preparing";
      report(onProgress, "preparing");
      return await WebAssembly.compile(wasmBytes);
    })();
    // 保留 WASM 字节，直到异步编译确实结束。
    // Keep WASM bytes intact until its asynchronous compilation has actually settled.
    const inputBytes = wasmBytes;
    preparation.finally(() => inputBytes.fill(0)).catch(() => {});
    wasmModule = await waitFor(
      preparation,
      signal,
      limits.initializationMs,
      "AIR_INITIALIZATION_TIMEOUT",
    );
    wasmBytes = undefined;
    for (let index = 0; index < concurrency; index++) {
      ensureLive(signal);
      const lane = createLane();
      lanes.push(lane);
      try {
        await lane.start(signal, onProgress);
      } catch (error) {
        lane.stop(error);
        if (
          index > 0 &&
          ["AIR_WORKER_UNAVAILABLE", "AIR_ALLOCATION_FAILED", "AIR_MEMORY_LIMIT"].includes(
            error?.code,
          )
        ) {
          lanes.pop();
          break;
        }
        throw error;
      }
      // 保留至少一半池预算给后续编译增长；这不是浏览器总内存测量。
      // Reserve at least half the pool budget for later heap growth; this is not total browser memory measurement.
      if (lanes.reduce((total, item) => total + item.heapBytes, 0) >= 512 * 1024 * 1024) break;
    }
    ensureLive(signal);
  } catch (error) {
    dispose();
    const cause = startupFailure ?? error;
    throw airCompilerFailure(cause?.code, cause, { compilerPhase });
  }
  poolReady = true;
  return Object.freeze({
    initialized: true,
    get ready() {
      return !disposed && !!wasmModule;
    },
    concurrency: lanes.length,
    compiler,
    options: AIR_COMPILER_OPTIONS,
    limits,
    dispose,
    close: dispose,
    async compile(source, options = {}) {
      ensureLive(options.signal);
      const lane = lanes.find((item) => !item.active);
      if (!lane) fail("AIR_COMPILER_BUSY");
      return lane.compile(source, options);
    },
  });
}
