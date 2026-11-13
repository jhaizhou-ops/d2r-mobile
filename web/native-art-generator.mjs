// 依据已验证原生图像元数据及资源工作线程契约生成图像清单。
// Generate the art inventory from verified native-art metadata and the asset worker contract.
/**
 * 完整原生图像清单，不是通用裁剪配方或完整 IPA 流水线。
 * Complete native-art inventory, not a generic crop recipe or complete IPA pipeline.
 */
import { runCachedAssetOperation } from "./asset-output-cache.mjs";
const MAX_INPUT = 8 * 1024 * 1024;
const MAX_PAYLOAD = 20 * 1024 * 1024;
const fail = (code) => {
  throw new Error(code);
};
function base64(bytes) {
  let text = "";
  for (let i = 0; i < bytes.length; i += 8192)
    text += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(text);
}
async function digest(bytes) {
  return Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)), (b) =>
    b.toString(16).padStart(2, "0"),
  ).join("");
}
/**
 * metadata 和 metadataSha256 必须来自已验证的应用发布资源。
 * metadata and metadataSha256 must come from verified application release resources.
 */
export async function generateNativeArt({
  logicalAssets,
  assetWorker,
  decodeAtlas,
  metadata,
  metadataSha256,
  signal,
  outputCache,
  cacheContext,
}) {
  const check = () => {
    if (signal?.aborted) fail("NATIVE_ART_CANCELLED");
  };
  check();
  const keys = ["manifest", "geometry", "binding", "implementation"];
  if (
    Object.keys(metadata ?? {})
      .sort()
      .join() !== [...keys].sort().join()
  )
    fail("NATIVE_ART_METADATA_SET");
  for (const key of keys) {
    if (typeof metadata[key] !== "string" || metadata[key].length > 2 * 1024 * 1024)
      fail("NATIVE_ART_METADATA_LIMIT");
    if ((await digest(new TextEncoder().encode(metadata[key]))) !== metadataSha256?.[key])
      fail("NATIVE_ART_METADATA_HASH");
  }
  const manifest = JSON.parse(metadata.manifest);
  if (!Array.isArray(manifest.assets) || manifest.assets.length < 9 || manifest.assets.length > 64)
    fail("NATIVE_ART_ASSET_COUNT");
  if (
    manifest.official_tables?.length !== 2 ||
    manifest.official_tables
      .map((t) => t.file)
      .sort()
      .join() !== "skilldesc.txt,skills.txt"
  )
    fail("NATIVE_ART_TABLE_SET");
  const raw = {},
    rgba = {};
  const owned = new Set();
  const release = (bytes) => {
    bytes.fill(0);
    owned.delete(bytes);
  };
  let total = 0,
    pixels = 0;
  try {
    const read = async (path, hash) => {
      check();
      if (
        typeof path !== "string" ||
        path.length > 1024 ||
        path.includes("\\") ||
        path.includes(":") ||
        path.split("/").some((p) => !p || p === "." || p === "..") ||
        Object.hasOwn(raw, path)
      )
        fail("NATIVE_ART_INPUT_PATH");
      const bytes = await logicalAssets.read(path, { maxBytes: MAX_INPUT - total, signal });
      check();
      if (!(bytes instanceof Uint8Array) || bytes.length > MAX_INPUT - total)
        fail("NATIVE_ART_RAW_LIMIT");
      owned.add(bytes);
      total += bytes.length;
      if ((await digest(bytes)) !== hash) fail("NATIVE_ART_SOURCE_HASH");
      raw[path] = base64(bytes);
      return bytes;
    };
    for (const table of manifest.official_tables)
      release(await read(`global/excel/${table.file}`, table.sha256));
    for (const asset of manifest.assets) {
      const [width, height] = asset.atlas_px ?? [];
      if (
        !Number.isSafeInteger(width) ||
        !Number.isSafeInteger(height) ||
        width < 1 ||
        height < 1 ||
        (pixels += width * height) > 4194304
      )
        fail("NATIVE_ART_DIMENSIONS");
      if (!/^dm_native_[A-Za-z0-9_]+\.png$/.test(asset.asset) || Object.hasOwn(rgba, asset.asset))
        fail("NATIVE_ART_OUTPUT_NAME");
      if (!asset.official_path?.startsWith("data:data/")) fail("NATIVE_ART_INPUT_PATH");
      const bytes = await read(asset.official_path.slice(10), asset.official_sprite_sha256);
      const decoded = await decodeAtlas(bytes, { signal });
      if (decoded.pixels instanceof Uint8Array) owned.add(decoded.pixels);
      check();
      if (
        decoded.width !== width ||
        decoded.height !== height ||
        !(decoded.pixels instanceof Uint8Array) ||
        decoded.pixels.length !== width * height * 4 ||
        decoded.pixelFormat !== 70 ||
        decoded.bytesPerPixel !== 4
      )
        fail("NATIVE_ART_DECODE_GEOMETRY");
      rgba[asset.asset] = base64(decoded.pixels);
      // 原始数据及像素均已编码后再擦除，兼容解码器返回重叠视图。
      // Wipe only after encoding both source and pixels, allowing overlapping decoder views.
      release(bytes);
      release(decoded.pixels);
    }
    const payload = new TextEncoder().encode(JSON.stringify({ metadata, raw, rgba }));
    owned.add(payload);
    // 负载已独立编码；等待工作线程前释放本地 base64 字符串。
    // The payload is independently encoded; release local base64 strings before awaiting the worker.
    for (const map of [raw, rgba]) for (const key of Object.keys(map)) delete map[key];
    if (payload.length > MAX_PAYLOAD) fail("NATIVE_ART_INPUT_LIMIT");
    check();
    const expected = [...manifest.assets.map((a) => a.asset), "dm_touch_native_art.c"].sort();
    const options = { metadataSha256 };
    return await runCachedAssetOperation({
      outputCache,
      cacheContext,
      operation: "native-art",
      payload,
      options,
      signal,
      checkCancellation: check,
      run: () => assetWorker.run("native-art", payload, options),
      validate(result) {
        check();
        if (
          Object.keys(result.files ?? {})
            .sort()
            .join() !== expected.join()
        )
          fail("NATIVE_ART_OUTPUT_SET");
      },
    });
  } finally {
    for (const bytes of owned) bytes.fill(0);
  }
}
