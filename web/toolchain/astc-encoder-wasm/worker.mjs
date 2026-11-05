// 每个可丢弃工作线程处理一张图像。终止线程即取消边界。
// One image per disposable worker. Termination is the cancellation boundary.
// 基于固定版本 Arm WASM 模块与 contract.mjs 的编码契约。
// Based on the pinned Arm WASM module and contract.mjs encoding contract.
import { validateImage, encodeWithModule } from "./contract.mjs";
let started = false;
self.onmessage = async ({ data }) => {
  if (started) return;
  started = true;
  try {
    validateImage(data);
    const { default: createModule } = await import("./dist/astc-encoder.mjs");
    const module = await createModule();
    const blocks = encodeWithModule(module, data);
    self.postMessage({ ok: true, blocks }, [blocks]);
  } catch (error) {
    self.postMessage({ ok: false, error: String(error?.message || error) });
  } finally {
    self.close();
  }
};
