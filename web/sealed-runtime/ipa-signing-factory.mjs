// 每次签名使用独立线程；尝试之间仅保留已验证公开字节，不联网或保存凭据。
// Retain only verified public bytes between one-shot signing attempts; never fetch or retain credentials.
import { prepareIpaSigningClient } from "../ipa-signing-client.mjs";
import { IPA_SIGNING_LIMITS, clearSigningBuffers, signingError } from "../ipa-signing-contract.mjs";

export async function prepareVerifiedIpaSigningFactory({
  workerBlob,
  wasmBytes,
  signal,
  warmup = true,
}) {
  if (
    typeof warmup !== "boolean" ||
    !(workerBlob instanceof Blob) ||
    workerBlob.size < 1 ||
    workerBlob.size > IPA_SIGNING_LIMITS.wasmBytes ||
    !(wasmBytes instanceof Uint8Array) ||
    wasmBytes.byteLength < 8 ||
    wasmBytes.byteLength > IPA_SIGNING_LIMITS.wasmBytes
  )
    throw signingError("SIGNING_INITIALIZATION_FAILED");
  let code = workerBlob,
    wasm = wasmBytes.slice(),
    closed = false,
    initializing = false,
    preparing = false;
  workerBlob = wasmBytes = undefined;
  let warmClient, activeAttempt, initialization, startupWorker, startupUrl;
  const revokeStartup = () => {
    if (startupUrl !== undefined) URL.revokeObjectURL(startupUrl);
    startupUrl = undefined;
  };
  function terminateStartup() {
    try {
      startupWorker?.terminate();
    } catch {
      // 清理异常不泄漏原始信息，也不阻止其余公开资源释放。
      // Cleanup exceptions cannot leak raw details or prevent remaining public resource release.
    }
  }
  function close() {
    if (closed) return;
    closed = true;
    initialization?.abort();
    terminateStartup();
    activeAttempt?.close();
    warmClient?.close();
    warmClient = undefined;
    revokeStartup();
    wasm?.fill(0);
    wasm = code = undefined;
  }
  async function initialize(operationSignal) {
    if (closed || initializing) throw signingError("SIGNING_CLOSED");
    operationSignal?.throwIfAborted();
    initializing = true;
    const controller = new AbortController();
    initialization = controller;
    const forwardAbort = () => controller.abort();
    operationSignal?.addEventListener("abort", forwardAbort, { once: true });
    if (operationSignal?.aborted) forwardAbort();
    let client;
    try {
      startupUrl = URL.createObjectURL(code);
      startupWorker = new Worker(startupUrl, { type: "module" });
      const buffer = wasm.slice().buffer;
      client = await prepareIpaSigningClient({
        worker: startupWorker,
        assets: [["zsign.wasm", buffer]],
        signal: controller.signal,
      });
      if (closed) throw signingError("SIGNING_CLOSED");
      operationSignal?.throwIfAborted();
      if (!client.ready || buffer.byteLength !== 0)
        throw signingError("SIGNING_INITIALIZATION_FAILED");
      return client;
    } catch (error) {
      client?.close();
      terminateStartup();
      if (error?.name === "AbortError") throw error;
      throw signingError(error?.message);
    } finally {
      operationSignal?.removeEventListener("abort", forwardAbort);
      revokeStartup();
      startupWorker = initialization = undefined;
      initializing = false;
    }
  }
  async function prepare({ signal: operationSignal } = {}) {
    if (closed || preparing || initializing || activeAttempt) throw signingError("SIGNING_CLOSED");
    operationSignal?.throwIfAborted();
    preparing = true;
    let client;
    try {
      client = warmClient;
      warmClient = undefined;
      // 已失效的预热线程不能授权私有输入；仅用保留的公开字节重新初始化。
      // A failed warm worker cannot authorize private input; restart using retained public bytes only.
      if (client && !client.ready) {
        client.close();
        client = undefined;
      }
      if (!client) client = await initialize(operationSignal);
      if (closed || operationSignal?.aborted) {
        client.close();
        operationSignal?.throwIfAborted();
        throw signingError("SIGNING_CLOSED");
      }
      let released = false,
        used = false;
      const attempt = Object.freeze({
        close() {
          if (released) return;
          released = true;
          client.close();
          client = undefined;
          if (activeAttempt === attempt) activeAttempt = undefined;
        },
        async sign(options) {
          if (released || used || closed) throw signingError("SIGNING_CLOSED");
          used = true;
          try {
            return await client.sign(options);
          } finally {
            clearSigningBuffers([options?.credentials?.identity, options?.credentials?.profile]);
            attempt.close();
          }
        },
      });
      activeAttempt = attempt;
      return attempt;
    } finally {
      preparing = false;
    }
  }
  try {
    // 首次尝试复用已验证的初始线程，避免重复启动；后续尝试不保留旧线程。
    // Keep the verified initial worker for the first attempt, avoiding a second startup.
    if (warmup) warmClient = await initialize(signal);
    signal?.throwIfAborted();
    signal = undefined;
  } catch (error) {
    close();
    throw error;
  }
  return Object.freeze({
    prepare,
    async sign(options) {
      if (!activeAttempt) throw signingError("SIGNING_CLOSED");
      return activeAttempt.sign(options);
    },
    close,
  });
}
