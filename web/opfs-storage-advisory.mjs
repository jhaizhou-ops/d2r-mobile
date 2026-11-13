// 仅查询浏览器源存储估算；不申请持久化或权限，不读取文件，不预留磁盘。
// Query browser-origin storage estimates only; no persistence/permission requests, file reads, or disk reservation.
export const OPFS_STORAGE_ADVISORY_TIMEOUT_MS = 1000;
const validBytes = (value) => Number.isSafeInteger(value) && value >= 0;

/**
 * requiredBytes 只能来自已知暂存需求，不能由 NSP 大小或包容量上限推断。
 * 结果仅为浏览器源的建议值，不能代表外部选定目录的可用空间。
 * requiredBytes must come from a known staging requirement, never NSP size or a pack capacity cap.
 * Results are advisory for browser-origin storage, never free space in an external selected directory.
 * @param {{ storage?: Pick<StorageManager, "estimate"> | null, requiredBytes?: number, timeoutMs?: number, signal?: AbortSignal }} [options]
 */
export async function readOpfsStorageAdvisory({
  storage,
  requiredBytes,
  timeoutMs = OPFS_STORAGE_ADVISORY_TIMEOUT_MS,
  signal,
} = {}) {
  if (
    (requiredBytes !== undefined && !validBytes(requiredBytes)) ||
    !Number.isSafeInteger(timeoutMs) ||
    timeoutMs < 1 ||
    timeoutMs > 30000 ||
    (signal !== undefined && !(signal instanceof AbortSignal))
  )
    throw new TypeError("Invalid OPFS storage advisory options");
  const result = (status, quotaBytes = null, usageBytes = null) => {
    const availableBytes = status === "estimated" ? Math.max(0, quotaBytes - usageBytes) : null;
    return Object.freeze({
      status,
      quotaBytes,
      usageBytes,
      availableBytes,
      requiredBytes: requiredBytes ?? null,
      sufficiency:
        availableBytes === null || requiredBytes === undefined
          ? "unknown"
          : availableBytes >= requiredBytes
            ? "estimated-sufficient"
            : "estimated-insufficient",
      advisoryOnly: true,
    });
  };
  if (signal?.aborted) return result("cancelled");
  let estimate;
  try {
    if (storage === undefined) storage = globalThis.navigator?.storage;
    estimate = storage?.estimate;
  } catch {
    return result("unavailable");
  }
  if (typeof estimate !== "function") return result("unavailable");
  let timer, onAbort;
  const pending = Promise.resolve().then(async () => {
    if (signal?.aborted) return result("cancelled");
    let value;
    try {
      value = await estimate.call(storage);
    } catch {
      return result("unavailable");
    }
    if (signal?.aborted) return result("cancelled");
    try {
      const quota = value?.quota,
        usage = value?.usage;
      if (!validBytes(quota) || !validBytes(usage)) return result("invalid");
      return result("estimated", quota, usage);
    } catch {
      return result("invalid");
    }
  });
  const candidates = [
    pending,
    new Promise((resolve) => {
      timer = setTimeout(() => resolve(result("timeout")), timeoutMs);
    }),
  ];
  if (signal)
    candidates.push(
      new Promise((resolve) => {
        onAbort = () => resolve(result("cancelled"));
        signal.addEventListener("abort", onAbort, { once: true });
        if (signal.aborted) onAbort();
      }),
    );
  try {
    // 超时或取消无法终止平台估算；迟到的结果/拒绝仍被观察，但不会改变已返回结果。
    // Timeout/cancel cannot stop the platform estimate; late results/rejections stay observed without changing the returned result.
    return await Promise.race(candidates);
  } finally {
    clearTimeout(timer);
    if (onAbort) signal.removeEventListener("abort", onAbort);
  }
}
