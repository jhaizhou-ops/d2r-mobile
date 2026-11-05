// 发布资产闭包的精确白名单；拒绝误带旧 Python 运行时。 依据资产运行时所需模块与三个 WASM 后端的发布文件集合。
// Exact release asset closure allowlist; reject accidental legacy Python runtime payloads. Based on the release file set required by the asset runtime modules and three WASM backends.
export const ASSET_RUNTIME_DIRECTORY = "toolchain/asset-runtime-js/dist";
export const ASSET_RUNTIME_FILES = Object.freeze([
  "worker.mjs",
  "image-kernels.wasm",
  "png-codec.wasm",
  "font-rasterizer.wasm",
]);
export function assertNoPythonRuntimeAsset(path) {
  const lower = path.toLowerCase(),
    name = lower.split("/").at(-1);
  if (
    lower.includes("pyodide") ||
    /\.(?:py|pyc|pyo|whl)$/.test(name) ||
    name.startsWith("libpython") ||
    name === "python_stdlib.zip"
  )
    throw new Error("Python runtime asset is not allowed in the release payload");
}
export function validateAssetRuntimeInventory(manifest) {
  if (
    manifest?.version !== 1 ||
    !Array.isArray(manifest.assets) ||
    manifest.assets.length !== ASSET_RUNTIME_FILES.length
  )
    throw new Error("Incomplete asset runtime closure");
  const found = new Set();
  let total = 0;
  for (const entry of manifest.assets) {
    if (
      !ASSET_RUNTIME_FILES.includes(entry.path) ||
      found.has(entry.path) ||
      !Number.isSafeInteger(entry.bytes) ||
      entry.bytes < 1 ||
      entry.bytes > 32 * 1024 * 1024 ||
      !/^[a-f0-9]{64}$/.test(entry.sha256)
    )
      throw new Error("Unexpected asset runtime payload");
    found.add(entry.path);
    total += entry.bytes;
  }
  if (total > 96 * 1024 * 1024) throw new Error("Asset runtime byte cap");
  return manifest.assets;
}
