// 父级持有 ASTC 作业目录租约；仅清理仍与原句柄相同的目录，失败可观察且可重试。
// Parent-owned ASTC job-directory lease; remove only the original handle identity, with observable retryable failures.
export const ASTC_JOB_DIRECTORY_TIMEOUT_MS = 1000;
const failure = (code, cause) => new Error(`ASTC_JOB_DIRECTORY_${code}`, { cause });
function bounded(pending, milliseconds, signal) {
  let timer, abort;
  const limits = [
    pending,
    new Promise((_, reject) => {
      timer = setTimeout(() => reject(failure("TIMEOUT")), milliseconds);
    }),
  ];
  if (signal)
    limits.push(
      new Promise((_, reject) => {
        abort = () => reject(signal.reason ?? new DOMException("Cancelled", "AbortError"));
        signal.addEventListener("abort", abort, { once: true });
        if (signal.aborted) abort();
      }),
    );
  return Promise.race(limits).finally(() => {
    clearTimeout(timer);
    if (abort) signal.removeEventListener("abort", abort);
  });
}

/**
 * stopAndDrain 是受信任父级的停机动作，不是认证或“写入已结束”标志。
 * OPFS 无独占创建或按句柄删除；同源代码必须可信，身份检查不是对恶意替换的原子防御。
 * stopAndDrain is a trusted parent's shutdown action, not authentication or a "writes ended" flag.
 * OPFS has no exclusive create or handle-based delete; same-origin code must be trusted, since identity checks are not atomic protection against hostile replacement.
 */
export async function createAstcJobDirectory({
  root,
  signal,
  stopAndDrain,
  cleanupTimeoutMs = ASTC_JOB_DIRECTORY_TIMEOUT_MS,
  randomUUID = () => globalThis.crypto.randomUUID(),
} = {}) {
  if (
    root?.kind !== "directory" ||
    typeof root.getDirectoryHandle !== "function" ||
    typeof root.removeEntry !== "function"
  )
    throw failure("ROOT_REQUIRED");
  if (
    (signal !== undefined && !(signal instanceof AbortSignal)) ||
    (stopAndDrain !== undefined && typeof stopAndDrain !== "function") ||
    !Number.isSafeInteger(cleanupTimeoutMs) ||
    cleanupTimeoutMs < 1 ||
    cleanupTimeoutMs > 30000 ||
    typeof randomUUID !== "function"
  )
    throw failure("OPTIONS");
  signal?.throwIfAborted();
  const id = randomUUID();
  if (
    typeof id !== "string" ||
    !/^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/i.test(id)
  )
    throw failure("ID");
  const name = `d2rm-astc-job-${id}`;
  let child, creation, cleanup, stop, lastError;
  let phase = "opening",
    disposed = false,
    complete = false,
    removed = false;
  const onAbort = () => {
    void dispose().catch(() => {});
  };
  function dispose() {
    disposed = true;
    signal?.removeEventListener("abort", onAbort);
    if (complete) return Promise.resolve();
    if (!cleanup) {
      phase = "disposing";
      const task = (async () => {
        // 先等待创建结果；超时不代表未执行，不再发起第二次创建或删除。
        // Drain acquisition first; timeout does not mean unexecuted, so never start a second creation or removal.
        try {
          await creation;
        } catch {
          /* 创建错误由创建调用者接收。 | Acquisition errors belong to the creating caller. */
        }
        if (stopAndDrain) {
          if (!stop)
            stop = Promise.resolve()
              .then(stopAndDrain)
              .catch((error) => {
                stop = undefined;
                throw error;
              });
          await stop;
        }
        if (child) {
          let current;
          try {
            current = await root.getDirectoryHandle(name);
          } catch (error) {
            if (error?.name !== "NotFoundError") throw error;
            removed = true;
          }
          if (current) {
            if (typeof child.isSameEntry !== "function" || !(await child.isSameEntry(current)))
              throw failure("IDENTITY_CHANGED");
            try {
              await root.removeEntry(name, { recursive: true });
            } catch (error) {
              if (error?.name !== "NotFoundError") throw error;
            }
            removed = true;
          }
        }
        complete = true;
        phase = "disposed";
        lastError = undefined;
        child = undefined;
      })();
      cleanup = task;
      task.then(
        () => {
          if (cleanup === task) cleanup = undefined;
        },
        (error) => {
          if (cleanup === task) cleanup = undefined;
          phase = "cleanup-failed";
          lastError = error;
        },
      );
    }
    return bounded(cleanup, cleanupTimeoutMs).catch((error) => {
      // 原操作继续受跟踪；返回有界错误，调用方可重试等待同一操作。
      // Keep tracking the original operation; return a bounded error so callers can retry waiting for it.
      lastError = error;
      throw failure("CLEANUP_FAILED", error);
    });
  }
  const lease = Object.freeze({
    get root() {
      if (disposed || phase !== "ready" || !child) throw failure("NOT_READY");
      return child;
    },
    get status() {
      return Object.freeze({ phase, removed, cleanupPending: !!cleanup, lastError });
    },
    dispose,
  });
  creation = (async () => {
    try {
      await root.getDirectoryHandle(name);
      throw failure("COLLISION");
    } catch (error) {
      if (error?.name !== "NotFoundError") throw error;
    }
    signal?.throwIfAborted();
    if (disposed) throw failure("DISPOSED");
    child = await root.getDirectoryHandle(name, { create: true });
    if (child?.kind !== "directory" || typeof child.isSameEntry !== "function")
      throw failure("IDENTITY_UNAVAILABLE");
    signal?.throwIfAborted();
    if (disposed) throw failure("DISPOSED");
    phase = "ready";
  })();
  signal?.addEventListener("abort", onAbort, { once: true });
  try {
    await bounded(creation, cleanupTimeoutMs, signal);
    signal?.throwIfAborted();
    if (disposed) throw failure("DISPOSED");
    return lease;
  } catch (cause) {
    const error = failure("ACQUIRE_FAILED", cause);
    // 即使创建迟到，错误仍携带租约，父级可观察或重试清理。
    // Even with late acquisition, the error retains its lease for parent-observable cleanup/retry.
    Object.defineProperty(error, "lease", { value: lease, enumerable: true });
    void dispose().catch(() => {});
    throw error;
  }
}
