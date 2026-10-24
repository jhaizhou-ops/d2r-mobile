// 管理纹理编码工作线程的请求生命周期；依据 ./browser-texture-encoder.mjs 的模块与调用约定。
// Manage request lifetimes for the texture-encoding worker using the module and calling contracts in ./browser-texture-encoder.mjs.
import { validateEncodeRequest } from "./browser-texture-encoder.mjs";
/** 同时仅处理一个请求；取消会终止两个编解码运行时，后续调用重新启动它们。 */
/**
 * 最多一个运行中请求；取消会终止两个编解码运行时，后续调用重新启动。
 * One in-flight request. Cancellation terminates both codec runtimes; later calls restart. */
export class TextureEncodeClient {
  #worker;
  #pending;
  #nextId = 0;
  constructor(
    workerFactory = () =>
      new Worker(new URL("./texture-encode-worker.mjs", import.meta.url), { type: "module" }),
  ) {
    this.workerFactory = workerFactory;
  }
  encodeAndPack(request, { signal } = {}) {
    return this.#request("encodeAndPack", request, signal);
  }
  packLinearBC3(request, { signal } = {}) {
    return this.#request("packLinearBC3", request, signal);
  }
  #request(operation, request, signal) {
    if (signal?.aborted)
      return Promise.reject(new DOMException("Encoding cancelled", "AbortError"));
    if (this.#pending) return Promise.reject(new Error("Texture encoder is busy"));
    try {
      validateEncodeRequest(request.templateHeader, request.width, request.height);
    } catch (error) {
      return Promise.reject(error);
    }
    const input = operation === "encodeAndPack" ? request.rgba : request.linearBC3;
    const expected =
      operation === "encodeAndPack"
        ? request.width * request.height * 4
        : Math.ceil(request.width / 4) * Math.ceil(request.height / 4) * 16;
    if (
      !(input instanceof Uint8Array) ||
      input.byteLength !== expected ||
      !(input.buffer instanceof ArrayBuffer)
    )
      return Promise.reject(new Error("Invalid encoder input bytes"));
    // 使用自有副本，确保传输、取消或任务失败时用户输入保持不变。
    // Own copies keep user input intact on transfer, cancellation or failed jobs.
    const payload = {
      width: request.width,
      height: request.height,
      templateHeader: request.templateHeader.slice(),
      [operation === "encodeAndPack" ? "rgba" : "linearBC3"]: input.slice(),
    };
    const id = ++this.#nextId;
    return new Promise((resolve, reject) => {
      const cleanup = () => {
        signal?.removeEventListener("abort", cancel);
        this.#pending = undefined;
      };
      const fail = (error) => {
        if (this.#pending?.id !== id) return;
        this.#worker?.terminate();
        this.#worker = undefined;
        cleanup();
        reject(error);
      };
      const cancel = () => fail(new DOMException("Encoding cancelled", "AbortError"));
      this.#pending = { fail, id };
      signal?.addEventListener("abort", cancel, { once: true });
      try {
        const worker = (this.#worker ??= this.workerFactory());
        worker.onmessage = ({ data }) => {
          if (data.id !== id || this.#pending?.id !== id) return;
          cleanup();
          if (data.ok) resolve(data.result);
          else {
            const error = new Error(data.error?.message ?? "Encoding failed");
            error.code = data.error?.code;
            reject(error);
          }
        };
        worker.onerror = () => fail(new Error("Texture encoding worker failed"));
        worker.postMessage({ id, operation, ...payload }, [
          payload.templateHeader.buffer,
          payload[operation === "encodeAndPack" ? "rgba" : "linearBC3"].buffer,
        ]);
      } catch (error) {
        fail(error);
      }
    });
  }
  close() {
    if (this.#pending) this.#pending.fail(new DOMException("Encoder closed", "AbortError"));
    else {
      this.#worker?.terminate();
      this.#worker = undefined;
    }
  }
}
