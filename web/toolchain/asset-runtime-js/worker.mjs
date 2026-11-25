// 保留初始化屏障、串行队列与回执；构建时内嵌经验证 WASM 清单。 依据 contract.mjs 的操作限制与 runtime.mjs 的初始化约定。
// Preserve initialization barrier, serial queue and receipts; embed verified WASM inventory at build time. Based on the operation limits in contract.mjs and initialization contract in runtime.mjs.
import { validateInitialization, validateRequest, transferableResult } from "./contract.mjs";
import { createAssetRuntime } from "./runtime.mjs";
const expected = globalThis.__ASSET_RUNTIME_WASM_INVENTORY__;
let starting = false,
  runtime = null,
  phase = "validate",
  queue = Promise.resolve();
const stage = (value) => {
  phase = value;
  self.postMessage({ type: "initialization-stage", stage: value });
};
async function initialize(data) {
  if (starting || runtime) throw new Error("Already initialized");
  starting = true;
  const assets = validateInitialization(data);
  try {
    runtime = await createAssetRuntime(assets, expected, stage);
  } finally {
    assets.clear();
  }
  phase = "ready";
  return runtime.receipt;
}
self.onmessage = ({ data }) => {
  // 初始化完成前不排队接收任何私有工作输入。
  // Never enqueue private job inputs before initialization finishes.
  if (data?.type !== "initialize" && !runtime) {
    self.postMessage({ id: data?.id, ok: false, error: "Runtime not initialized" });
    return;
  }
  queue = queue.then(async () => {
    try {
      if (data?.type === "initialize") {
        self.postMessage({ id: 0, ok: true, result: await initialize(data) });
      } else {
        const { operation, payload, options } = validateRequest(data);
        const result = await runtime.execute(operation, payload, options);
        const { message, transfers } = transferableResult(data.id, result);
        self.postMessage(message, transfers);
      }
    } catch (error) {
      // 仅公开初始化诊断含详细信息；私有作业错误保持脱敏。
      // Detailed diagnostics are public initialization only; private job failures stay sanitized.
      const detail =
        !runtime && data?.type === "initialize"
          ? `: ${String(error?.message ?? error).slice(0, 4000)}`
          : "";
      self.postMessage({
        id: data?.id ?? 0,
        ok: false,
        error: `Asset runtime operation failed (${phase})${detail}`,
      });
    }
  });
};
