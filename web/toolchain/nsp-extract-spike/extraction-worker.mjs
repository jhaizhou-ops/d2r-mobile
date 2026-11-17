// 依据固定 WASM 模块与提取引擎实现串行、受限的 Worker 消息入口。
// Implement a serialized, restricted Worker message entrypoint based on the fixed WASM module and extraction engine.
import { extractionWorkerErrorCode } from "../../extraction-errors.mjs";
import factory from "./dist/hactool-boundary.mjs";
import { createBoundary } from "./browser-boundary.mjs";
import { createExtractionEngine } from "./extraction-engine.mjs";
// 不接受调用方提供的模块 URL、源代码、原生名称或文件系统路径。
// No caller-supplied module URLs, source code, native names or filesystem paths.
const ready = createBoundary(factory).then((boundary) =>
  createExtractionEngine(boundary, {
    onProgress: (progress) => self.postMessage({ id: lastId, progress }),
  }),
);
let busy = false,
  closed = false,
  lastId = 0;
const reject = (id, error) =>
  self.postMessage({ id, ok: false, error: extractionWorkerErrorCode(error) });
self.onmessage = async ({ data }) => {
  const id = data?.id;
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
    // 错误意味着终止；绝不序列化解析器或原生文本，也不序列化含密钥的状态。
    // An error is terminal. Never serialize parser/native text or key-bearing state.
    try {
      await (await ready).execute("dispose");
    } catch {
      /* 无论如何都终止
       * terminate regardless */
    }
    reject(id, error);
    closed = true;
    self.close();
  } finally {
    busy = false;
  }
};
