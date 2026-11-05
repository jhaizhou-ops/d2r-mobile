// 实验 API：消耗缓冲区；转移后取消也不返还输入。
// Experimental API: consumes the buffer, including on cancellation after transfer.
// 基于 contract.mjs 契约与浏览器 Worker / AbortSignal API。
// Based on contract.mjs and browser Worker / AbortSignal APIs.
import { validateImage } from "./contract.mjs";
let active = false;
export function encodeAstc4x4(input, { signal, timeoutMs = 120000 } = {}) {
  const expected = validateImage(input);
  if (signal != null && !(signal instanceof AbortSignal))
    throw new TypeError("Expected an AbortSignal");
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 120000)
    throw new RangeError("Timeout must be 1..120000 ms");
  if (signal?.aborted) return Promise.reject(new DOMException("Cancelled", "AbortError"));
  if (active) return Promise.reject(new Error("Only one ASTC job is allowed per module realm"));
  active = true;
  return new Promise((resolve, reject) => {
    let worker,
      timer,
      finished = false;
    const finish = (error, value) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
      worker?.terminate();
      active = false;
      if (error) reject(error);
      else resolve(value);
    };
    const abort = () => finish(new DOMException("Cancelled", "AbortError"));
    try {
      worker = new Worker(new URL("./worker.mjs", import.meta.url), { type: "module" });
      worker.onmessage = ({ data }) => {
        if (!data?.ok) return finish(new Error(data?.error || "ASTC worker failed"));
        if (!(data.blocks instanceof ArrayBuffer) || data.blocks.byteLength !== expected)
          return finish(new Error("Invalid ASTC output length"));
        finish(null, data.blocks);
      };
      worker.onerror = () => finish(new Error("ASTC worker initialization or encoding failed"));
      worker.onmessageerror = () => finish(new Error("ASTC worker message failed"));
      signal?.addEventListener("abort", abort, { once: true });
      if (signal?.aborted) return abort();
      timer = setTimeout(() => finish(new Error("ASTC encoding deadline exceeded")), timeoutMs);
      worker.postMessage(
        {
          rgba: input.rgba,
          width: input.width,
          height: input.height,
          profile: input.profile ?? "linear",
          preset: input.preset ?? "thorough",
        },
        [input.rgba],
      );
    } catch (error) {
      finish(error);
    }
  });
}
