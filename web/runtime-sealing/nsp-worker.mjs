// 封装 NSP 提取工作线程的运行时边界；依据 ../extraction-errors.mjs、../toolchain/nsp-extract-spike/dist/hactool-boundary.mjs 的模块与调用约定。
// Wrap the NSP extraction worker runtime boundary using the module and calling contracts in ../extraction-errors.mjs、../toolchain/nsp-extract-spike/dist/hactool-boundary.mjs.
import { extractionWorkerErrorCode } from "../extraction-errors.mjs";
import factory from "../toolchain/nsp-extract-spike/dist/hactool-boundary.mjs";
import { byteFactory, initializationWasm } from "./wasm-factory.mjs";
import { createBoundary } from "../toolchain/nsp-extract-spike/browser-boundary.mjs";
import { createExtractionEngine } from "../toolchain/nsp-extract-spike/extraction-engine.mjs";
// 不接受调用方提供的模块 URL、源码、原生名称或文件系统路径。
// No caller-supplied module URLs, source code, native names or filesystem paths.
let ready;
let initializing = false;
let initialized = false;
let busy = false,
  closed = false,
  lastId = 0;
const reject = (id, error) =>
  self.postMessage({ id, ok: false, error: extractionWorkerErrorCode(error) });
self.onmessage = async ({ data }) => {
  if (data?.type === "initialize") {
    if (
      initializing ||
      initialized ||
      closed ||
      data.id !== 0 ||
      Object.keys(data).some((key) => !["id", "type", "assets"].includes(key))
    ) {
      self.postMessage({ id: 0, ok: false, error: "SEALED_INITIALIZATION_FAILED" });
      return;
    }
    initializing = true;
    try {
      ready = createBoundary(
        byteFactory(factory, initializationWasm(data.assets, "hactool-boundary.wasm")),
      ).then((boundary) =>
        createExtractionEngine(boundary, {
          onProgress: (progress) => self.postMessage({ id: lastId, progress }),
        }),
      );
      await ready;
      initialized = true;
      self.postMessage({ id: 0, ok: true, result: { initialized: true } });
    } catch {
      closed = true;
      self.postMessage({ id: 0, ok: false, error: "SEALED_INITIALIZATION_FAILED" });
      self.close();
    }
    return;
  }
  const id = data?.id;
  if (!initialized) {
    reject(Number.isSafeInteger(id) ? id : 0);
    return;
  }
  if (
    closed ||
    busy ||
    !Number.isSafeInteger(id) ||
    id <= lastId ||
    !data ||
    Object.keys(data).some((key) => !["id", "operation", "payload"].includes(key)) ||
    typeof data.operation !== "string"
  ) {
    reject(Number.isSafeInteger(id) ? id : 0);
    return;
  }
  busy = true;
  lastId = id;
  try {
    const engine = await ready;
    const result = await engine.execute(data.operation, data.payload);
    if (result instanceof Uint8Array) self.postMessage({ id, ok: true, result }, [result.buffer]);
    else self.postMessage({ id, ok: true, result });
    if (data.operation === "dispose") {
      closed = true;
      self.close();
    }
  } catch (error) {
    // 错误会终止处理；绝不序列化解析器或原生错误文本，以及含密钥的状态。
    // An error is terminal. Never serialize parser/native text or key-bearing state.
    try {
      await (await ready).execute("dispose");
    } catch {
      /* 无论如何都终止。 */
      /* terminate regardless */
    }
    reject(id, error);
    closed = true;
    self.close();
  } finally {
    busy = false;
  }
};
