// 完整 JS/WASM 资产闭包：仅接受固定清单的字节，不下载依赖、不回退 Python。 依据资产清单、固定模块工厂与各转换器的操作约定。
// Complete JS/WASM asset closure: accepts pinned inventory bytes only, without dependency downloads or Python fallback. Based on the asset inventory, pinned module factories and transformer operation contracts.
import { verifyWasmModule } from "./wasm-contract.mjs";
import { OPERATION_LIMITS } from "./contract.mjs";
import { hasAssetTransform, executeAssetTransform } from "../asset-transforms-js/transforms.mjs";
import {
  createPinnedCascOverlayTransform,
  createPinnedCascPrebuiltOverlayTransform,
} from "../casc-overlay-js/overlay.mjs";
import { createArtRecipes, isArtRecipeOperation } from "../art-recipes-js/recipes.mjs";
import {
  createRemainingArtRecipes,
  supportsRemainingArtOperation,
} from "../remaining-art-js/recipes.mjs";
import { generateHud } from "../hud-recipes-js/recipe.mjs";
import { createOverlayRecipe } from "../overlay-recipe-js/recipe.mjs";
import { createImageKernels } from "../image-kernels-rust/kernels.mjs";
import { createPngCodec } from "../png-codec-exact/codec.mjs";
import { createFontRasterizer } from "../font-rasterizer-exact/font.mjs";
import * as numeric from "../art-kernels/kernels.mjs";
import { createMD5 } from "hash-wasm";
const WASM_NAMES = ["image-kernels.wasm", "png-codec.wasm", "font-rasterizer.wasm"];
const digest = async (algorithm, bytes) =>
  Array.from(new Uint8Array(await crypto.subtle.digest(algorithm, bytes)), (byte) =>
    byte.toString(16).padStart(2, "0"),
  ).join("");
export async function createAssetRuntime(assets, expected, onStage = () => {}) {
  if (
    !(assets instanceof Map) ||
    assets.size !== WASM_NAMES.length ||
    !Array.isArray(expected) ||
    expected.length !== WASM_NAMES.length ||
    new Set(expected.map((asset) => asset.path)).size !== WASM_NAMES.length
  )
    throw new Error("Incomplete asset WASM closure");
  onStage("verify-asset-wasm");
  const modules = new Map();
  for (const name of WASM_NAMES) {
    const bytes = assets.get(name),
      record = expected.find((asset) => asset.path === name);
    if (
      !(bytes instanceof Uint8Array) ||
      !record ||
      !Number.isSafeInteger(record.bytes) ||
      record.bytes < 1 ||
      bytes.length !== record.bytes ||
      !/^[a-f0-9]{64}$/.test(record.sha256) ||
      (await digest("SHA-256", bytes)) !== record.sha256
    )
      throw new Error(`Asset WASM integrity mismatch: ${name}`);
    const module = await WebAssembly.compile(bytes);
    verifyWasmModule(module, name);
    modules.set(name, module);
  }
  onStage("initialize-pixel-kernels");
  const kernels = await createImageKernels(modules.get("image-kernels.wasm"));
  onStage("initialize-png-zlib");
  const codec = await createPngCodec(modules.get("png-codec.wasm"));
  onStage("initialize-font-rasterizer");
  const font = await createFontRasterizer(modules.get("font-rasterizer.wasm"));
  onStage("bind-asset-recipes");
  // 在异步初始化后同步复用实例；每次调用完整结束，不跨 await 共享进行中的状态。
  // Reuse only complete synchronous hashes after async initialization; never share in-flight state across await.
  const md5Hasher = await createMD5();
  const md5 = (bytes) => md5Hasher.init().update(bytes).digest();
  const sha256 = (bytes) => digest("SHA-256", bytes),
    sha1 = (bytes) => digest("SHA-1", bytes);
  const art = createArtRecipes({
    codec,
    kernels,
    defaultState: numeric.defaultState,
    font,
    md5,
    sha256,
  });
  const remaining = createRemainingArtRecipes({ pixels: kernels, png: codec, numeric });
  const casc = createPinnedCascOverlayTransform({ codec, md5Hasher });
  const cascPrebuilt = createPinnedCascPrebuiltOverlayTransform({ codec, md5Hasher });
  const overlay = createOverlayRecipe({ png: codec, md5, sha1 });
  if (typeof overlay?.execute !== "function")
    throw new Error("Incomplete overlay recipe implementation");
  const routes = new Map();
  for (const operation of Object.keys(OPERATION_LIMITS)) {
    const candidates = [
      hasAssetTransform(operation) &&
        ((payload, options) => executeAssetTransform(operation, payload, options)),
      isArtRecipeOperation(operation) && ((payload, options) => art(operation, payload, options)),
      supportsRemainingArtOperation(operation) &&
        ((payload, options) => remaining.execute(operation, payload, options)),
      operation === "casc-overlay" && casc,
      operation === "casc-prebuilt-overlay" && cascPrebuilt,
      operation === "hud-v4" &&
        ((payload, options) => generateHud(payload, options, { codec, kernels, font })),
      operation === "overlay-recipe" &&
        ((payload, options) => overlay.execute(operation, payload, options)),
    ].filter(Boolean);
    if (candidates.length !== 1)
      throw new Error(`Asset recipe routing is incomplete/ambiguous: ${operation}`);
    routes.set(operation, candidates[0]);
  }
  return Object.freeze({
    receipt: Object.freeze({
      initialized: true,
      blocked: 0,
      reads: WASM_NAMES.length,
      version: "asset-runtime-js/1",
      packages: { pillow: false },
      operations: [...routes.keys()],
      backends: {
        pngZlib: codec.zlibVersion,
        cascZlib: codec.genericZlibVersion,
        freetype: "2.6.0",
      },
    }),
    async execute(operation, payload, options) {
      if (!routes.has(operation)) throw new Error("Unsupported asset operation");
      return routes.get(operation)(payload, options);
    },
  });
}
