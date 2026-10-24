// 签名拥有独立工作线程；取消同步原生计算时直接终止它。
// Signing owns a separate worker; cancellation terminates synchronous native computation.
import {
  IPA_SIGNING_LIMITS,
  signingFields,
  signingError,
  validSigningBlob,
  validateSigningCredentials,
  validateSigningProgress,
  clearSigningBuffers,
} from "./ipa-signing-contract.mjs";
const aborted = () => new DOMException("Signing cancelled", "AbortError");

/**
 * 仅受信任、校验摘要的引导程序提供工作线程和公开 WASM；此客户端不下载代码。
 * 所有权在此转移，包括初始化失败。sign 消耗凭据缓冲区，原始 Blob 仍属于调用方。
 * JavaScript 无法清零密码字符串，界面也必须清除其引用。
 * Only the trusted, hash-verifying bootstrap supplies this worker and public WASM.
 * This client never fetches code. Ownership transfers here, including initialization failure.
 * Credential buffers are consumed by sign(), even on invalid input; caller owns its original Blob.
 * Password strings cannot be zeroized by JavaScript and must also be cleared by the UI.
 */
export async function prepareIpaSigningClient({
  worker,
  assets,
  signal,
  initializeTimeoutMs = 120000,
  signingTimeoutMs = 600000,
}) {
  let closed = false,
    used = false,
    pending;
  const failedWorker = () => {
    pending?.reject(signingError("SIGNING_WORKER_FAILED"));
    close();
  };
  function close() {
    if (closed) return;
    closed = true;
    for (const type of ["error", "messageerror"]) {
      try {
        worker?.removeEventListener?.(type, failedWorker);
      } catch {
        // 监听器清理失败不得阻止终止，也不能替换已脱敏的错误。
        // Listener cleanup must not prevent termination or replace sanitized errors.
      }
    }
    try {
      worker.terminate();
    } catch {
      // 清理异常不能泄漏原始信息。 | Cleanup exceptions cannot leak raw information.
    }
    pending?.reject(signingError("SIGNING_CLOSED"));
  }
  function exchange(message, transfers, operationSignal, onProgress) {
    return new Promise((resolve, reject) => {
      let settled = false,
        timer;
      const finish = (error, result) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        worker.removeEventListener("message", receive);
        worker.removeEventListener("error", fail);
        worker.removeEventListener("messageerror", fail);
        operationSignal?.removeEventListener("abort", cancel);
        pending = undefined;
        if (error) reject(error);
        else resolve(result);
      };
      const fail = () => finish(signingError("SIGNING_WORKER_FAILED"));
      const cancel = () => {
        finish(aborted());
        close();
      };
      const receive = ({ data }) => {
        if (data?.id !== message.id) return;
        if (message.id === 1 && data?.type === "progress") {
          if (!validateSigningProgress(data)) {
            finish(signingError("SIGNING_PROTOCOL_INVALID"));
            return;
          }
          try {
            Promise.resolve(
              onProgress?.(
                Object.freeze({
                  stage: data.stage,
                  completed: data.completed,
                  total: data.total,
                }),
              ),
            ).catch(() => {});
          } catch {
            // 观察者不会改变签名结果。 | Observers cannot change signing outcomes.
          }
          return;
        }
        if (signingFields(data, ["id", "ok", "error"]) && data.ok === false) {
          finish(signingError(data.error?.code));
          return;
        }
        if (!signingFields(data, ["id", "ok", "result"]) || data.ok !== true) {
          finish(signingError("SIGNING_PROTOCOL_INVALID"));
          return;
        }
        finish(null, data.result);
      };
      pending = { reject: (error) => finish(error) };
      worker.addEventListener("message", receive);
      worker.addEventListener("error", fail);
      worker.addEventListener("messageerror", fail);
      operationSignal?.addEventListener("abort", cancel, { once: true });
      if (operationSignal?.aborted) {
        cancel();
        return;
      }
      if (closed) {
        finish(signingError("SIGNING_CLOSED"));
        return;
      }
      timer = setTimeout(
        () => {
          finish(signingError("SIGNING_TIMEOUT"));
          close();
        },
        message.id === 0 ? initializeTimeoutMs : signingTimeoutMs,
      );
      try {
        worker.postMessage(message, transfers);
      } catch {
        finish(signingError("SIGNING_WORKER_FAILED"));
      }
    });
  }
  try {
    // 可信调用方可缩短测试时限，但不能关闭或延长生产上限。
    // Trusted callers can shorten deadlines for tests, not disable or exceed production caps.
    if (
      !Number.isSafeInteger(initializeTimeoutMs) ||
      initializeTimeoutMs < 1 ||
      initializeTimeoutMs > 120000 ||
      !Number.isSafeInteger(signingTimeoutMs) ||
      signingTimeoutMs < 1 ||
      signingTimeoutMs > 600000
    )
      throw signingError("SIGNING_INITIALIZATION_FAILED");
    if (
      !Array.isArray(assets) ||
      assets.length !== 1 ||
      !Array.isArray(assets[0]) ||
      assets[0].length !== 2 ||
      assets[0][0] !== "zsign.wasm" ||
      !(assets[0][1] instanceof ArrayBuffer) ||
      assets[0][1].resizable ||
      assets[0][1].byteLength < 8 ||
      assets[0][1].byteLength > IPA_SIGNING_LIMITS.wasmBytes
    )
      throw signingError("SIGNING_INITIALIZATION_FAILED");
    const buffer = assets[0][1];
    const initialized = await exchange({ id: 0, type: "initialize", assets }, [buffer], signal);
    if (!signingFields(initialized, ["initialized"]) || initialized.initialized !== true)
      throw signingError("SIGNING_INITIALIZATION_FAILED");
    if (signal?.aborted) throw aborted();
    // 转换期间预热线程仍可能失败；保存失败状态，不能等到读取凭据后才发现失效。
    // Warm workers can fail during conversion; remember failure before credentials are read.
    worker.addEventListener("error", failedWorker);
    worker.addEventListener("messageerror", failedWorker);
  } catch (error) {
    close();
    if (error?.name === "AbortError") throw aborted();
    if (error?.message === "SIGNING_TIMEOUT") throw signingError("SIGNING_TIMEOUT");
    throw signingError("SIGNING_INITIALIZATION_FAILED");
  }
  return Object.freeze({
    close,
    get ready() {
      return !closed && !used;
    },
    async sign({ ipa, credentials, signal: operationSignal, onProgress } = {}) {
      // 并发调用不接触第一个调用拥有的凭据或工作线程。
      // Concurrent callers must not affect credentials or worker owned by the first call.
      if (closed || used) throw signingError("SIGNING_CLOSED");
      used = true;
      const buffers = [credentials?.identity, credentials?.profile];
      try {
        if (!validSigningBlob(ipa) || !validateSigningCredentials(credentials))
          throw signingError("SIGNING_INPUT_INVALID");
        const result = await exchange(
          { id: 1, type: "sign", ipa, credentials },
          buffers,
          operationSignal,
          onProgress,
        );
        if (operationSignal?.aborted) throw aborted();
        if (!signingFields(result, ["blob"]) || !validSigningBlob(result.blob))
          throw signingError("SIGNING_OUTPUT_INVALID");
        return Object.freeze({ blob: result.blob, fileName: "D2RMobile-signed.ipa" });
      } finally {
        clearSigningBuffers(buffers);
        close();
      }
    },
  });
}
