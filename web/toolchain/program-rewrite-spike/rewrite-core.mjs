// 保持相同的准入接口；固定源版本的重写现使用 JS 与 Capstone/LZ4 WASM。
// Same admission interface; source-pinned rewriting now uses JS + Capstone/LZ4 WASM.
export { createJavaScriptProgramRewriter as createProgramRewriter } from "./js/rewrite.mjs";
