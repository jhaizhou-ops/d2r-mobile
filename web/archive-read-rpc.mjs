// 独占 MessagePort 上的有界归档读取；能力仅绑定父线程持有的来源闭包，不接受路径或键。
// Bounded archive reads on an owned MessagePort; capabilities bind parent-held source closures, never paths or keys.
export const ARCHIVE_RPC_READ_BYTES = 65536;
const MAX = (1n << 64n) - 1n;
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const error = (code) => new Error(`ARCHIVE_RPC_${code}`);
const aborted = () => new DOMException("Cancelled", "AbortError");
const integer = (v, max) => Number.isSafeInteger(v) && v >= 0 && v <= max;
const decimal = (v) =>
  typeof v === "string" && /^(0|[1-9][0-9]{0,19})$/.test(v) && BigInt(v) <= MAX;
const fields = (v, names) =>
  v &&
  typeof v === "object" &&
  !Array.isArray(v) &&
  Object.keys(v).sort().join() === [...names].sort().join();
function config(port, signal, maxPending) {
  if (
    !port ||
    ["postMessage", "addEventListener", "removeEventListener", "start", "close"].some(
      (k) => typeof port[k] !== "function",
    ) ||
    (signal !== undefined && !(signal instanceof AbortSignal)) ||
    !integer(maxPending, 16) ||
    maxPending < 1
  )
    throw error("OPTIONS");
  if (signal?.aborted) throw aborted();
}
function listen(port, signal, receive, stop) {
  const message = ({ data }) => {
    try {
      receive(data);
    } catch {
      stop(error("PROTOCOL"));
    }
  };
  const bad = () => stop(error("PROTOCOL"));
  const cancel = () => stop(aborted());
  port.addEventListener("message", message);
  port.addEventListener("messageerror", bad);
  signal?.addEventListener("abort", cancel, { once: true });
  port.start();
  return () => {
    port.removeEventListener("message", message);
    port.removeEventListener("messageerror", bad);
    signal?.removeEventListener("abort", cancel);
    port.close();
  };
}
/**
 * 来源由可信调用方注册；一次撤销关闭整个作业通道，不遗留在途读取权限。
 * Trusted callers register sources; revoking one capability closes the whole job channel, retaining no in-flight authority.
 */
export function createArchiveReadRpcHost(port, { signal, maxPending = 1 } = {}) {
  config(port, signal, maxPending);
  const sources = new Map(),
    controller = new AbortController();
  let closed = false,
    last = 0,
    active = 0,
    detach = () => {};
  function close() {
    if (closed) return;
    closed = true;
    sources.clear();
    controller.abort();
    try {
      port.postMessage({ type: "closed" });
    } catch {
      /* 不暴露外部错误。 | Never expose external errors. */
    }
    detach();
  }
  function send(value, transfers = []) {
    if (!closed) {
      try {
        port.postMessage(value, transfers);
      } catch {
        close();
      }
    }
  }
  function receive(data) {
    if (closed) return;
    if (fields(data, ["type"]) && data.type === "closed") {
      close();
      return;
    }
    if (
      !fields(data, ["type", "id", "cap", "offset", "count"]) ||
      data.type !== "read" ||
      !integer(data.id, Number.MAX_SAFE_INTEGER) ||
      data.id !== last + 1 ||
      typeof data.cap !== "string" ||
      !sources.has(data.cap) ||
      !decimal(data.offset) ||
      !integer(data.count, ARCHIVE_RPC_READ_BYTES) ||
      active >= maxPending
    )
      throw error("PROTOCOL");
    const source = sources.get(data.cap),
      offset = BigInt(data.offset);
    if (offset > source.size || BigInt(data.count) > source.size - offset) throw error("PROTOCOL");
    last = data.id;
    active++;
    // 宿主 I/O 可能忽略取消；关闭后禁止迟到结果再次发布。
    // Host I/O may ignore cancellation; never publish late results after closure.
    void Promise.resolve()
      .then(() => {
        if (closed) throw aborted();
        return source.readAt(offset, data.count, { signal: controller.signal });
      })
      .then((bytes) => {
        if (closed) return;
        if (
          !(bytes instanceof Uint8Array) ||
          !(bytes.buffer instanceof ArrayBuffer) ||
          bytes.buffer.resizable ||
          bytes.length !== data.count
        )
          throw error("READ");
        // Buffer.slice() 可能共享来源；只传输请求范围的独立普通字节副本。
        // Buffer.slice() may alias the source; transfer only an owned plain copy of the requested range.
        const buffer = new Uint8Array(bytes).buffer;
        send({ type: "result", id: data.id, bytes: buffer }, [buffer]);
      })
      .catch(() => send({ type: "failure", id: data.id, code: "READ" }))
      .finally(() => {
        active--;
      });
  }
  detach = listen(port, signal, receive, close);
  return Object.freeze({
    register(source) {
      if (closed) throw error("CLOSED");
      if (
        !source ||
        typeof source.size !== "bigint" ||
        source.size < 0n ||
        source.size > MAX ||
        typeof source.readAt !== "function" ||
        sources.size >= 32768
      )
        throw error("SOURCE");
      const id = crypto.randomUUID();
      if (!uuid.test(id) || sources.has(id)) throw error("CAPABILITY");
      sources.set(id, Object.freeze({ size: source.size, readAt: source.readAt.bind(source) }));
      return Object.freeze({ id, size: source.size.toString() });
    },
    revoke(id) {
      if (!sources.has(id)) throw error("CAPABILITY");
      close();
    },
    close,
  });
}
/**
 * 不排队无限请求；达到并发上限立即拒绝，调用方必须等待当前读取。
 * Never queue unbounded requests; reject at the concurrency cap so callers await their current read.
 */
export function createArchiveReadRpcClient(
  port,
  { signal, maxPending = 1, timeoutMs = 30000 } = {},
) {
  config(port, signal, maxPending);
  if (!integer(timeoutMs, 120000) || timeoutMs < 1) throw error("OPTIONS");
  const pending = new Map(),
    caps = new Map();
  let closed = false,
    next = 0,
    detach = () => {};
  function close(cause = error("CLOSED")) {
    if (closed) return;
    closed = true;
    for (const p of pending.values()) {
      p.release();
      p.reject(cause);
    }
    pending.clear();
    caps.clear();
    try {
      port.postMessage({ type: "closed" });
    } catch {
      /* 不暴露外部错误。 | Never expose external errors. */
    }
    detach();
  }
  function receive(data) {
    if (closed) return;
    if (fields(data, ["type"]) && data.type === "closed") {
      close();
      return;
    }
    const p = pending.get(data?.id);
    if (!p) throw error("PROTOCOL");
    const success =
      fields(data, ["type", "id", "bytes"]) &&
      data.type === "result" &&
      data.bytes instanceof ArrayBuffer &&
      !data.bytes.resizable &&
      data.bytes.byteLength === p.count;
    const failure =
      fields(data, ["type", "id", "code"]) && data.type === "failure" && data.code === "READ";
    if (!success && !failure) throw error("PROTOCOL");
    pending.delete(data.id);
    p.release();
    if (success) p.resolve(new Uint8Array(data.bytes));
    else p.reject(error("READ"));
  }
  detach = listen(port, signal, receive, close);
  return Object.freeze({
    open(descriptor) {
      if (closed) throw error("CLOSED");
      if (
        !fields(descriptor, ["id", "size"]) ||
        typeof descriptor.id !== "string" ||
        !uuid.test(descriptor.id) ||
        !decimal(descriptor.size) ||
        caps.has(descriptor.id) ||
        caps.size >= 32768
      )
        throw error("CAPABILITY");
      const cap = descriptor.id,
        size = BigInt(descriptor.size);
      caps.set(cap, size);
      return Object.freeze({
        size,
        async readAt(offset, count, options = {}) {
          if (closed) throw error("CLOSED");
          const readSignal = options.signal;
          if (readSignal !== undefined && !(readSignal instanceof AbortSignal))
            throw error("OPTIONS");
          if (readSignal?.aborted) {
            close(aborted());
            throw aborted();
          }
          if (
            typeof offset !== "bigint" ||
            offset < 0n ||
            offset > size ||
            !integer(count, ARCHIVE_RPC_READ_BYTES) ||
            BigInt(count) > size - offset
          )
            throw error("BOUNDS");
          if (pending.size >= maxPending) throw error("BUSY");
          if (next === Number.MAX_SAFE_INTEGER) {
            close();
            throw error("CLOSED");
          }
          const id = ++next;
          return new Promise((resolve, reject) => {
            const cancel = () => close(aborted());
            const timer = setTimeout(() => close(error("TIMEOUT")), timeoutMs);
            const release = () => {
              clearTimeout(timer);
              readSignal?.removeEventListener("abort", cancel);
            };
            pending.set(id, { count, resolve, reject, release });
            readSignal?.addEventListener("abort", cancel, { once: true });
            if (readSignal?.aborted) {
              cancel();
              return;
            }
            try {
              port.postMessage({ type: "read", id, cap, offset: offset.toString(), count });
            } catch {
              close(error("TRANSPORT"));
            }
          });
        },
      });
    },
    close: () => close(),
  });
}
