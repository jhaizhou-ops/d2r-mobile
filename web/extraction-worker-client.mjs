// 依据 extraction-protocol 的消息及时限契约管理隔离提取传输。
// Manage isolated extraction transport using the extraction-protocol message and deadline contracts.
import { isExtractionWorkerErrorCode, isExtractionProgress } from "./extraction-errors.mjs";
/**
 * 隔离提取 Worker 的严格单请求传输。createWorker 属于受信任的应用接线；这里不接受 URL、源码或 RPC。
 * Strict, single-flight transport for the isolated extraction Worker.
 * createWorker is trusted application wiring; no URL/source/RPC is accepted here.
 */
export class ExtractionClientError extends Error {
  constructor(code) {
    super(code);
    this.name = "ExtractionClientError";
    this.code = code;
  }
}
const error = (code) => new ExtractionClientError(code);
const keys = (value, expected) =>
  value &&
  typeof value === "object" &&
  Object.keys(value).sort().join(",") === [...expected].sort().join(",");
const romfsProgress = (stage) => ["romfs-read", "romfs-base", "romfs-merged"].includes(stage);
/**
 * 截止时间覆盖排队及执行总时长。长时间验证也不会获得无限期租约或基于心跳的续期；真实游戏的吞吐量尚未测量。
 * Total queue-plus-execution deadlines. Long verification never gets an
 * unbounded lease or heartbeat-based renewal; real-title throughput is unmeasured.
 */
export const EXTRACTION_OPERATION_TIMEOUTS = Object.freeze({
  initialize: Object.freeze({ defaultMs: 2 * 60 * 60 * 1000, maxMs: 6 * 60 * 60 * 1000 }),
  select: Object.freeze({ defaultMs: 120000, maxMs: 600000 }),
  readExefs: Object.freeze({ defaultMs: 30000, maxMs: 120000 }),
  openRomfs: Object.freeze({ defaultMs: 2 * 60 * 60 * 1000, maxMs: 6 * 60 * 60 * 1000 }),
  readRomfs: Object.freeze({ defaultMs: 30000, maxMs: 120000 }),
  dispose: Object.freeze({ defaultMs: 30000, maxMs: 120000 }),
});
export function createExtractionWorkerClient({
  createWorker,
  timeoutMs,
  operationTimeoutMs = {},
  maxQueue = 8,
} = {}) {
  if (
    typeof createWorker !== "function" ||
    (timeoutMs !== undefined &&
      (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 600000)) ||
    !operationTimeoutMs ||
    typeof operationTimeoutMs !== "object" ||
    Array.isArray(operationTimeoutMs) ||
    Object.keys(operationTimeoutMs).some(
      (operation) => !Object.hasOwn(EXTRACTION_OPERATION_TIMEOUTS, operation),
    ) ||
    !Number.isInteger(maxQueue) ||
    maxQueue < 1 ||
    maxQueue > 32
  )
    throw error("EXTRACTION_CLIENT_OPTIONS");
  const deadlines = Object.create(null);
  for (const [operation, limits] of Object.entries(EXTRACTION_OPERATION_TIMEOUTS)) {
    const override = Object.hasOwn(operationTimeoutMs, operation);
    // 旧的全局选项可以缩短长操作，但绝不会扩大
    // 新的短操作时限。优先使用显式的逐操作覆盖值。
    // The legacy global option can shorten long operations but never expands the
    // new short-operation ceilings. Explicit per-operation overrides are preferred.
    const value = override
      ? operationTimeoutMs[operation]
      : timeoutMs === undefined
        ? limits.defaultMs
        : Math.min(timeoutMs, limits.maxMs);
    if (!Number.isSafeInteger(value) || value < 1 || value > limits.maxMs)
      throw error("EXTRACTION_CLIENT_OPTIONS");
    deadlines[operation] = value;
  }
  let worker;
  try {
    worker = createWorker();
  } catch {
    throw error("EXTRACTION_WORKER_UNAVAILABLE");
  }
  if (
    !worker ||
    ["postMessage", "terminate", "addEventListener", "removeEventListener"].some(
      (k) => typeof worker[k] !== "function",
    )
  ) {
    try {
      worker?.terminate?.();
    } catch {
      /* 绝不暴露工厂内部细节。 | Never expose factory details. */
    }
    throw error("EXTRACTION_WORKER_UNAVAILABLE");
  }
  let closed = false,
    nextId = 0,
    active;
  const queue = [];
  function settle(task, failure, result) {
    clearTimeout(task.timer);
    task.signal?.removeEventListener("abort", task.abort);
    task.payload = undefined;
    if (failure) task.reject(failure);
    else task.resolve(result);
  }
  function close(code = "EXTRACTION_DISPOSED") {
    if (closed) return;
    closed = true;
    worker.removeEventListener("message", message);
    worker.removeEventListener("error", crashed);
    worker.removeEventListener("messageerror", crashed);
    try {
      worker.terminate();
    } catch {
      /* 本地引用仍会被撤销。 | Local references still revoked. */
    }
    if (active) settle(active, error(code));
    active = undefined;
    for (const task of queue.splice(0)) settle(task, error(code));
    worker = undefined;
  }
  function pump() {
    if (closed || active || !queue.length) return;
    active = queue.shift();
    try {
      worker.postMessage({ id: active.id, operation: active.operation, payload: active.payload });
    } catch {
      close("EXTRACTION_TRANSPORT_FAILED");
    }
  }
  function message(event) {
    const data = event.data;
    if (
      (active?.operation === "initialize" ||
        active?.operation === "select" ||
        active?.operation === "openRomfs") &&
      data?.id === active.id &&
      keys(data, ["id", "progress"]) &&
      isExtractionProgress(data.progress) &&
      (active.operation === "openRomfs") === romfsProgress(data.progress.stage) &&
      (active.operation !== "select" || data.progress.stage === "exefs-open")
    ) {
      try {
        // 异步观察者拒绝也必须被观察，但绝不等待或续期请求。
        // Observe async observer rejection without awaiting it or renewing the request.
        Promise.resolve(active.onProgress?.(Object.freeze({ ...data.progress }))).catch(() => {});
      } catch {
        // 观察者不会更改请求或延长截止时间。
        // Observers do not alter the request or renew its deadline.
      }
      return;
    }
    if (
      !active ||
      !data ||
      data.id !== active.id ||
      !(data.ok === true
        ? keys(data, ["id", "ok", "result"])
        : data.ok === false &&
          keys(data, ["id", "ok", "error"]) &&
          isExtractionWorkerErrorCode(data.error))
    ) {
      close("EXTRACTION_PROTOCOL_FAILED");
      return;
    }
    if (!data.ok) {
      close(data.error);
      return;
    }
    const task = active;
    let valid = false;
    try {
      valid = task.validate(data.result);
    } catch {
      /* 结构化克隆数据格式错误。 | Malformed structured-clone data. */
    }
    if (!valid) {
      close("EXTRACTION_PROTOCOL_FAILED");
      return;
    }
    active = undefined;
    settle(task, null, data.result);
    if (task.operation === "dispose") close();
    else pump();
  }
  function crashed(event) {
    event.preventDefault?.();
    close("EXTRACTION_WORKER_FAILED");
  }
  worker.addEventListener("message", message);
  worker.addEventListener("error", crashed);
  worker.addEventListener("messageerror", crashed);
  function request(operation, payload, validate, { signal, onProgress } = {}) {
    if (closed) return Promise.reject(error("EXTRACTION_DISPOSED"));
    if (signal?.aborted) {
      close("EXTRACTION_CANCELLED");
      return Promise.reject(error("EXTRACTION_CANCELLED"));
    }
    if (queue.length >= maxQueue) return Promise.reject(error("EXTRACTION_QUEUE_LIMIT"));
    return new Promise((resolve, reject) => {
      const task = {
        id: ++nextId,
        operation,
        payload,
        validate,
        resolve,
        reject,
        signal,
        onProgress,
      };
      task.abort = () => close("EXTRACTION_CANCELLED");
      // 包括排队时间，以约束请求的总生命周期。
      // Includes time spent queued, bounding total request lifetime.
      task.timer = setTimeout(() => close("EXTRACTION_TIMEOUT"), deadlines[operation]);
      signal?.addEventListener("abort", task.abort, { once: true });
      queue.push(task);
      pump();
    });
  }
  const read = (operation, payload, options) => {
    if (
      typeof payload.offset !== "bigint" ||
      payload.offset < 0n ||
      !Number.isInteger(payload.count) ||
      payload.count < 0 ||
      payload.count > 65536 ||
      typeof payload.token !== "string" ||
      !payload.token ||
      payload.token.length > 256 ||
      (operation === "readExefs" && (!Number.isInteger(payload.index) || payload.index < 0))
    )
      return Promise.reject(error("EXTRACTION_READ_BOUNDS"));
    return request(
      operation,
      payload,
      (result) =>
        result instanceof Uint8Array &&
        result.byteLength === payload.count &&
        result.byteOffset === 0 &&
        result.buffer instanceof ArrayBuffer &&
        result.buffer.byteLength === payload.count,
      options,
    );
  };
  return Object.freeze({
    initialize: ({ nsps, prodKeys, titleKeys }, options) =>
      request(
        "initialize",
        { nsps, prodKeys, titleKeys },
        (result) =>
          keys(result, ["records", "inventory"]) &&
          Array.isArray(result.records) &&
          result.records.length > 0 &&
          result.records.length <= 4096 &&
          Array.isArray(result.inventory) &&
          result.inventory.length <= 4096,
        options,
      ),
    select: (options, requestOptions) =>
      request(
        "select",
        { options },
        (result) =>
          keys(result, ["selection", "token", "entries"]) &&
          typeof result.token === "string" &&
          result.token.length > 0 &&
          result.token.length <= 256 &&
          Array.isArray(result.entries) &&
          result.entries.length <= 65536,
        requestOptions,
      ),
    readExefs: ({ token, index, offset, count }, options) =>
      read("readExefs", { token, index, offset, count }, options),
    openRomfs: (token, { verifyRomfs = true, ...options } = {}) =>
      request(
        "openRomfs",
        { token, verifyRomfs },
        (result) =>
          keys(result, ["token", "size"]) &&
          typeof result.token === "string" &&
          result.token.length > 0 &&
          result.token.length <= 256 &&
          result.token !== token &&
          typeof result.size === "bigint" &&
          result.size >= 80n,
        options,
      ),
    readRomfs: ({ token, offset, count }, options) =>
      read("readRomfs", { token, offset, count }, options),
    dispose: () =>
      closed
        ? Promise.resolve()
        : request(
            "dispose",
            {},
            (result) => keys(result, ["disposed"]) && result.disposed === true,
          ),
    close: () => close(),
  });
}
