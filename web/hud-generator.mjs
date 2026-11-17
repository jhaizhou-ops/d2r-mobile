// 依据原生 HUD 生成配方和已验证的前序图像阶段生成 HUD 资源。
// Generate HUD assets from native HUD recipes and verified preceding art stages.
/**
 * 完整的 Switch 放大原生 HUD。字体通过逻辑归档读取，绝非操作系统文件。
 * Complete Switch-upscaled native HUD. Fonts are logical archive reads, never OS files.
 */
import { acceptsNativeArtPngDigest } from "./native-art-png-digests.mjs";
import { RELEASE_ART_REVISION, requireArtRevision } from "./toolchain/art-recipes-js/revision.mjs";
import { runCachedAssetOperation } from "./asset-output-cache.mjs";
const IMAGE_PATHS = Object.freeze({
  front_panel: "hd/global/ui/PANEL/HUD_02/front_panel.sprite",
  levelupbutton: "hd/global/ui/Controller/HUD/LevelUpButton.sprite",
  greater_healing_potion: "hd/global/ui/items/misc/potion/greater_healing_potion.sprite",
});
const FONT_NAMES = Object.freeze(["blizzardglobal-v5_81.ttf", "exocetblizzardot-medium.otf"]);
const fail = (code) => {
  throw new Error(code);
};
const encode = (bytes) => {
  let value = "";
  for (let i = 0; i < bytes.length; i += 8192)
    value += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(value);
};
async function hash(bytes) {
  return Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (b) =>
    b.toString(16).padStart(2, "0"),
  ).join("");
}
export const HUD_LOGICAL_INPUTS = Object.freeze([
  ...Object.values(IMAGE_PATHS),
  ...FONT_NAMES.map((n) => `hd/ui/fonts/${n}`),
  "global/ui/Layouts/Controller/HUDPanelHD.json",
]);
/**
 * nativeArtFiles 是之前已验证的原生图像阶段输出，而非用户选择的文件。
 * nativeArtFiles are the preceding verified native-art stage outputs, not user-selected files.
 */
export async function generateHud({
  logicalAssets,
  assetWorker,
  decodeAtlas,
  nativeArtFiles,
  metadata,
  metadataSha256,
  mode,
  signal,
  recipeRevision = RELEASE_ART_REVISION,
  outputCache,
  cacheContext,
}) {
  requireArtRevision(recipeRevision);
  const check = () => {
    if (signal?.aborted) fail("HUD_CANCELLED");
  };
  check();
  if (mode !== "switch-upscaled") fail("HUD_MODE_REQUIRED");
  if (
    Object.keys(metadata ?? {})
      .sort()
      .join() !== "i18n,manifest,patch"
  )
    fail("HUD_METADATA_SET");
  for (const key of ["i18n", "manifest", "patch"]) {
    if (typeof metadata[key] !== "string" || metadata[key].length > 2 * 1024 * 1024)
      fail("HUD_METADATA_LIMIT");
    if ((await hash(new TextEncoder().encode(metadata[key]))) !== metadataSha256?.[key])
      fail("HUD_METADATA_HASH");
  }
  const manifest = JSON.parse(metadata.manifest),
    assets = manifest.assets.filter((a) => a.asset.startsWith("dm_native_skill_"));
  if (
    assets.length !== 9 ||
    new Set(assets.map((a) => a.asset)).size !== 9 ||
    assets.some((a) => !/^dm_native_skill_[0-8]\.png$/.test(a.asset))
  )
    fail("HUD_ATLAS_SET");
  let total = 0,
    pixels = 0;
  const owned = new Set(),
    images = {},
    fonts = {},
    atlases = {};
  const release = (bytes) => {
    bytes.fill(0);
    owned.delete(bytes);
  };
  const read = async (path) => {
    check();
    const bytes = await logicalAssets.read(path, {
      maxBytes: Math.min(16 * 1024 * 1024, 40 * 1024 * 1024 - total),
      signal,
    });
    if (
      !(bytes instanceof Uint8Array) ||
      bytes.length > 16 * 1024 * 1024 ||
      (total += bytes.length) > 40 * 1024 * 1024
    )
      fail("HUD_READ_LIMIT");
    owned.add(bytes);
    check();
    return bytes;
  };
  try {
    for (const [name, path] of Object.entries(IMAGE_PATHS)) {
      const source = await read(path);
      const decoded = await decodeAtlas(source, { signal });
      if (decoded.pixels instanceof Uint8Array) owned.add(decoded.pixels);
      check();
      const { width, height } = decoded;
      if (
        !Number.isSafeInteger(width) ||
        !Number.isSafeInteger(height) ||
        width < 1 ||
        height < 1 ||
        (pixels += width * height) > 4194304 ||
        decoded.pixelFormat !== 70 ||
        decoded.bytesPerPixel !== 4 ||
        !(decoded.pixels instanceof Uint8Array) ||
        decoded.pixels.length !== width * height * 4
      )
        fail("HUD_DECODE_GEOMETRY");
      images[name] = { size: [width, height], rgba: encode(decoded.pixels) };
      // 像素编码完成后再一起擦除源数据和像素，兼容重叠视图。
      // Wipe source and pixels together after pixel encoding, allowing overlapping views.
      release(source);
      release(decoded.pixels);
    }
    for (const name of FONT_NAMES) {
      const bytes = await read(`hd/ui/fonts/${name}`);
      fonts[name] = encode(bytes);
      release(bytes);
    }
    let layout;
    {
      const bytes = await read("global/ui/Layouts/Controller/HUDPanelHD.json");
      layout = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
      release(bytes);
    }
    if (new TextEncoder().encode(layout).length > 2 * 1024 * 1024) fail("HUD_LAYOUT_LIMIT");
    for (const asset of assets) {
      const input = nativeArtFiles?.[asset.asset];
      const bytes = input instanceof ArrayBuffer ? new Uint8Array(input) : input;
      if (
        !(bytes instanceof Uint8Array) ||
        bytes.length > 16 * 1024 * 1024 ||
        !acceptsNativeArtPngDigest(asset, await hash(bytes))
      )
        fail("HUD_ATLAS_HASH");
      const [w, h] = asset.atlas_px;
      if (
        !Number.isSafeInteger(w) ||
        !Number.isSafeInteger(h) ||
        w < 1 ||
        h < 1 ||
        (pixels += w * h) > 4194304
      )
        fail("HUD_PIXEL_LIMIT");
      atlases[asset.asset] = encode(bytes); // 绝不修改借用的前序阶段输出。 | Borrowed prior-stage outputs are never mutated.
    }
    const payload = new TextEncoder().encode(
      JSON.stringify({ metadata, images, fonts, layout, atlases }),
    );
    owned.add(payload);
    // 负载已独立编码；仅清空本地映射，保留借用的图集和元数据。
    // The payload is independently encoded; clear only local maps, preserving borrowed atlases and metadata.
    for (const map of [images, fonts, atlases]) for (const key of Object.keys(map)) delete map[key];
    if (payload.length > 64 * 1024 * 1024) fail("HUD_INPUT_LIMIT");
    check();
    const options = {
      mode,
      metadataSha256,
      revision: recipeRevision,
    };
    return await runCachedAssetOperation({
      outputCache,
      cacheContext,
      operation: "hud-v4",
      payload,
      options,
      signal,
      checkCancellation: check,
      run: () => assetWorker.run("hud-v4", payload, options),
      validate(result) {
        check();
        if (
          !result.files?.["base.png"] ||
          !result.files?.["hud.json"] ||
          !result.files?.["hud_v4_ui/ui.json"]
        )
          fail("HUD_OUTPUT_SET");
      },
    });
  } finally {
    for (const bytes of owned) bytes.fill(0);
  }
}
