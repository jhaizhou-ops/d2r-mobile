// 封装程序改写工作线程的运行时边界；依据 ../toolchain/program-rewrite-spike/dist/capstone-lz4.mjs、../toolchain/program-rewrite-spike/rewrite-core.mjs 的模块与调用约定。
// Wrap the program-rewrite worker runtime boundary using the module and calling contracts in ../toolchain/program-rewrite-spike/dist/capstone-lz4.mjs、../toolchain/program-rewrite-spike/rewrite-core.mjs.
import createNative from "../toolchain/program-rewrite-spike/dist/capstone-lz4.mjs";
import { createProgramRewriter } from "../toolchain/program-rewrite-spike/rewrite-core.mjs";
import { PROGRAM_REWRITER_SOURCE } from "../program-release-catalog.mjs";
import { byteFactory } from "./wasm-factory.mjs";
const NAMES = ["capstone-lz4.wasm"];
let started = false;
let context;
let queue = Promise.resolve();
const rewrite = createProgramRewriter(() => {
  if (!context) throw new Error("PROGRAM_NOT_INITIALIZED");
  return context;
});
async function initialize(data) {
  if (
    started ||
    data.id !== 0 ||
    Object.keys(data).some((key) => !["id", "type", "assets"].includes(key))
  )
    throw new Error("PROGRAM_INITIALIZATION");
  started = true;
  if (!Array.isArray(data.assets) || data.assets.length !== NAMES.length)
    throw new Error("PROGRAM_ASSET_INVENTORY");
  const assets = new Map();
  let total = 0;
  for (const entry of data.assets) {
    if (!Array.isArray(entry) || entry.length !== 2) throw new Error("PROGRAM_ASSET_ENTRY");
    const [name, buffer] = entry;
    if (
      !NAMES.includes(name) ||
      assets.has(name) ||
      !(buffer instanceof ArrayBuffer) ||
      !buffer.byteLength ||
      buffer.byteLength > 32 * 1024 * 1024
    )
      throw new Error("PROGRAM_ASSET_ENTRY");
    total += buffer.byteLength;
    assets.set(name, new Uint8Array(buffer));
  }
  if (total > 96 * 1024 * 1024) throw new Error("PROGRAM_ASSET_CAP");
  const native = await byteFactory(createNative, assets.get("capstone-lz4.wasm").buffer)();
  // 捆绑的 JS 与规则依赖闭包在构建时固定源码摘要，并由准入工厂再次检查。
  // 不交付 Python 加载器或夹具入口。
  // The bundled JS/rule closure is source-pinned at build time and checked again
  // by the admission factory. No Python loader or fixture route is shipped.
  context = { native, manifest: { sourceSha256: PROGRAM_REWRITER_SOURCE, cases: [] } };
}
self.addEventListener("message", ({ data }) => {
  if (data?.type === "initialize") {
    initialize(data)
      .then(() => self.postMessage({ id: 0, ok: true, result: { initialized: true } }))
      .catch(() => {
        self.postMessage({ id: 0, ok: false, error: "PROGRAM_INITIALIZATION_FAILED" });
        self.close();
      });
    return;
  }
  if (!context || data?.releaseId === undefined || data?.caseName !== undefined) {
    self.postMessage({ id: data?.id, ok: false, error: "PROGRAM_REQUEST_REJECTED" });
    return;
  }
  queue = queue.then(async () => {
    try {
      const result = await rewrite(data);
      self.postMessage(
        { id: data.id, ok: true, ...result },
        Object.values(result.files).map((bytes) => bytes.buffer),
      );
    } catch {
      self.postMessage({ id: data?.id, ok: false, error: "PROGRAM_REWRITE_FAILED" });
    }
  });
});
