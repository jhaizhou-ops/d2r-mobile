// 依据 dm/ios/tools/gen_map_goal_art.py 配方生成地图目标资源。
// Generate map-goal assets based on the dm/ios/tools/gen_map_goal_art.py recipe.
/**
 * 使用当前名称的完整地图目标资源。不使用操作系统字体、私有旧名称或推断的图集布局。
 * Complete current-name map-goal assets. No OS fonts, private old names, or inferred atlas layout.
 */
import { RELEASE_ART_REVISION, requireArtRevision } from "./toolchain/art-recipes-js/revision.mjs";
import { runCachedAssetOperation } from "./asset-output-cache.mjs";
export const MAP_GOAL_IMAGE_PATHS = Object.freeze({
  symbols: "hd/global/ui/PANEL/Quest_Log/TalRashaSymbol.sprite",
  units: "hd/global/ui/AUTOMAP/Units.sprite",
});
export const MAP_GOAL_TABLES = Object.freeze([
  "levels",
  "monsters",
  "objects",
  "item-names",
  "npcs",
]);
export const MAP_GOAL_FONT = "hd/ui/fonts/BlizzardGlobal-V5_81.ttf";
const outputNames = () => [
  "mapgoal.json",
  "cross.png",
  ...Array.from({ length: 7 }, (_, i) => `sym_${i}.png`),
  ...Array.from({ length: 31 }, (_, i) =>
    ["", "_en", "_es", "_mx", "_ru", "_ko"].map((s) => `label_${i}${s}.png`),
  ).flat(),
];
export const MAP_GOAL_OUTPUTS = Object.freeze(outputNames());
export const UPSTREAM_MAP_GOAL_OUTPUTS = MAP_GOAL_OUTPUTS;
const fail = (code) => {
  throw new Error(code);
};
const encode = (bytes) => {
  let value = "";
  for (let i = 0; i < bytes.length; i += 8192)
    value += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(value);
};
/**
 * decodeAtlas 必须返回已验证的完整图集，以及实际 frameWidth 和 frames。
 * decodeAtlas must return the validated full atlas plus actual frameWidth and frames.
 */
export async function generateMapGoal({
  logicalAssets,
  assetWorker,
  decodeAtlas,
  signal,
  recipeRevision = RELEASE_ART_REVISION,
  outputCache,
  cacheContext,
}) {
  requireArtRevision(recipeRevision);
  const expected = MAP_GOAL_OUTPUTS;
  const owned = new Set(),
    outputOwned = [];
  let success = false;
  let total = 0,
    pixels = 0;
  const check = () => {
    if (signal?.aborted) fail("MAP_GOAL_CANCELLED");
  };
  const release = (bytes) => {
    bytes.fill(0);
    owned.delete(bytes);
  };
  const read = async (path, limit) => {
    check();
    const bytes = await logicalAssets.read(path, { maxBytes: limit, signal });
    if (!(bytes instanceof Uint8Array)) fail("MAP_GOAL_READ_TYPE");
    owned.add(bytes);
    if (!bytes.length || bytes.length > limit || (total += bytes.length) > 24 * 1024 * 1024)
      fail("MAP_GOAL_READ_LIMIT");
    check();
    return bytes;
  };
  try {
    const images = {},
      strings = {};
    for (const [name, path] of Object.entries(MAP_GOAL_IMAGE_PATHS)) {
      const source = await read(path, 8 * 1024 * 1024);
      const decoded = await decodeAtlas(source, { signal });
      if (decoded.pixels instanceof Uint8Array) owned.add(decoded.pixels);
      else if (decoded.pixels instanceof ArrayBuffer) owned.add(new Uint8Array(decoded.pixels));
      check();
      const { width, height, frameWidth, frames } = decoded;
      if (
        ![width, height, frameWidth, frames].every((v) => Number.isSafeInteger(v) && v > 0) ||
        frames !== (name === "symbols" ? 7 : 8) ||
        width % frames !== 0 ||
        frameWidth > width / frames ||
        (pixels += width * height) > 2097152 ||
        decoded.pixelFormat !== 70 ||
        decoded.bytesPerPixel !== 4 ||
        !(decoded.pixels instanceof Uint8Array) ||
        decoded.pixels.length !== width * height * 4
      )
        fail("MAP_GOAL_FRAME_LAYOUT");
      images[name] = { size: [width, height], frameWidth, frames, rgba: encode(decoded.pixels) };
      // 字符串快照已独立；不将原始输入与解码图集保留到工作线程阶段。
      // String snapshots are independent; do not retain raw inputs and atlases through worker execution.
      release(source);
      release(decoded.pixels);
    }
    const fontBytes = await read(MAP_GOAL_FONT, 9 * 1024 * 1024);
    const font = encode(fontBytes);
    release(fontBytes);
    for (const name of MAP_GOAL_TABLES) {
      const bytes = await read(`local/lng/strings/${name}.json`, 2 * 1024 * 1024);
      strings[name] = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
      release(bytes);
    }
    const payload = new TextEncoder().encode(JSON.stringify({ images, font, strings }));
    owned.add(payload);
    if (payload.length > 20 * 1024 * 1024) fail("MAP_GOAL_INPUT_LIMIT");
    check();
    let result;
    try {
      const options = {
        names: "current",
        revision: recipeRevision,
      };
      result = await runCachedAssetOperation({
        outputCache,
        cacheContext,
        operation: "map-goal",
        payload,
        options,
        signal,
        checkCancellation: check,
        maxTotalBytes: 16 * 1024 * 1024,
        run: () => assetWorker.run("map-goal", payload, options),
        validate(candidate) {
          check();
          if (
            Object.keys(candidate.files ?? {})
              .sort()
              .join() !== [...expected].sort().join()
          )
            fail("MAP_GOAL_OUTPUT_SET");
          let output = 0;
          for (const value of Object.values(candidate.files)) {
            if (
              !(value instanceof Uint8Array || value instanceof ArrayBuffer) ||
              !value.byteLength ||
              (output += value.byteLength) > 16 * 1024 * 1024
            )
              fail("MAP_GOAL_OUTPUT_LIMIT");
          }
        },
      });
    } finally {
      release(payload);
    }
    // 先接管返回缓冲区，再检查取消或结果格式；成功时将输出所有权交给调用方。
    // Own returned buffers before cancellation/shape checks; successful outputs pass to the caller.
    for (const value of Object.values(result?.files ?? {})) {
      if (value instanceof Uint8Array) outputOwned.push(value);
      else if (value instanceof ArrayBuffer) outputOwned.push(new Uint8Array(value));
    }
    check();
    success = true;
    return result;
  } finally {
    if (!success) for (const bytes of outputOwned) bytes.fill(0);
    for (const bytes of owned) bytes.fill(0);
  }
}
