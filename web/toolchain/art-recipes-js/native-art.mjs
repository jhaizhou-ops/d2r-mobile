// 原生图集与 C 索引的有界 JavaScript 移植；依据原始 native_art/generate.py 与 asset-generation-spike/src/native_art.py。
// Bounded JavaScript port of native atlases and the C index, based on original native_art/generate.py and asset-generation-spike/src/native_art.py.
import {
  decimal,
  decodeBase64,
  exactKeys,
  isPythonInteger,
  parsePythonJson,
  pythonInteger,
  pythonNumber,
  pythonStrip,
  requiredEntry,
  requireValue,
  tsvDictRows,
  utf8,
  utf8Decoder,
} from "./native-art-util.mjs";
import { acceptsNativeArtPngDigest } from "../../native-art-png-digests.mjs";

const METADATA_KEYS = ["manifest", "geometry", "binding", "implementation"];
const CLASS_CODES = new Map(
  ["ama", "sor", "nec", "pal", "bar", "dru", "ass", "war", ""].map((code, index) => [code, index]),
);
const COMMON_ICON = new Map([
  [357n, 20n],
  [358n, 22n],
  [360n, 24n],
  [361n, 30n],
  [362n, 32n],
  [363n, 34n],
  [364n, 36n],
  [370n, 38n],
]);
const TABLE_NAMES = ["skills.txt", "skilldesc.txt"];
const MIB = 1024 * 1024;
const prefixPath = (path) =>
  path.startsWith("data:data/") ? path.slice("data:data/".length) : path;
const originalPath = (path) => {
  const offset = path.indexOf("data:data/");
  return offset < 0 ? path : path.slice(offset + "data:data/".length);
};
// 在码点计数前限制 UTF-16 长度，避免为无效名称分配逐字符数组。
// Bound UTF-16 length before counting code points, without character arrays for invalid names.
const validNameLength = (name) => {
  if (name.length > 254) return false;
  let count = 0;
  for (const _char of name) if (++count >= 128) return false;
  return true;
};
const unique = (values) => new Set(values).size === values.length;

export async function generateNativeArt(payload, options, { codec, sha256, md5 }) {
  requireValue(
    payload instanceof Uint8Array && payload.length <= 20 * MIB,
    "NATIVE_ART_INPUT_LIMIT",
  );
  const bundle = parsePythonJson(utf8Decoder.decode(payload));
  const metadata = bundle.metadata;
  requireValue(exactKeys(metadata, METADATA_KEYS), "NATIVE_ART_METADATA_SET");
  const pins = options?.metadataSha256 ?? {};
  requireValue(exactKeys(pins, METADATA_KEYS), "NATIVE_ART_TRUST_REQUIRED");
  const metadataHashes = {};
  for (const [name, text] of Object.entries(metadata)) {
    requireValue(typeof text === "string", "NATIVE_ART_METADATA_LIMIT");
    const bytes = utf8(text);
    requireValue(bytes.length <= 2 * MIB, "NATIVE_ART_METADATA_LIMIT");
    const hash = await sha256(bytes);
    requireValue(hash === pins[name], "NATIVE_ART_METADATA_HASH");
    metadataHashes[name] = hash;
  }
  const manifest = parsePythonJson(metadata.manifest);
  const assets = manifest.assets;
  requireValue(
    Array.isArray(assets) && assets.length >= 9 && assets.length <= 64,
    "NATIVE_ART_ASSET_COUNT",
  );
  const names = assets.map((asset) => asset.asset);
  requireValue(
    unique(names) &&
      names.every(
        (name) =>
          typeof name === "string" &&
          name.startsWith("dm_native_") &&
          name.endsWith(".png") &&
          !name.includes("/") &&
          !name.includes("\\") &&
          validNameLength(name),
      ),
    "NATIVE_ART_OUTPUT_NAME",
  );
  const officialTables = manifest.official_tables;
  requireValue(
    Array.isArray(officialTables) &&
      officialTables.length === 2 &&
      TABLE_NAMES.every((name) => officialTables.some((table) => table.file === name)),
    "NATIVE_ART_TABLE_SET",
  );
  const paths = assets.map((asset) => prefixPath(asset.official_path));
  paths.push(...officialTables.map((table) => "global/excel/" + table.file));
  requireValue(unique(paths) && exactKeys(bundle.raw, paths), "NATIVE_ART_INPUT_SET");
  requireValue(exactKeys(bundle.rgba, names), "NATIVE_ART_RGBA_SET");
  const raw = new Map();
  let rawBytes = 0;
  for (const [path, encoded] of Object.entries(bundle.raw)) {
    const bytes = decodeBase64(encoded);
    rawBytes += bytes.length;
    raw.set(path, bytes);
  }
  requireValue(rawBytes <= 8 * MIB, "NATIVE_ART_RAW_LIMIT");
  const images = new Map();
  let pixels = 0n;
  for (let i = 0; i < assets.length; i++) {
    const asset = assets[i];
    requireValue(
      Array.isArray(asset.atlas_px) && asset.atlas_px.length === 2,
      "NATIVE_ART_DIMENSIONS",
    );
    const [width, height] = asset.atlas_px;
    requireValue(
      isPythonInteger(width) && isPythonInteger(height) && width > 0 && height > 0,
      "NATIVE_ART_DIMENSIONS",
    );
    pixels += BigInt(width) * BigInt(height);
    requireValue(pixels <= 4_194_304n, "NATIVE_ART_PIXEL_LIMIT");
    const rgba = decodeBase64(bundle.rgba[asset.asset]);
    requireValue(
      rgba.length === Number(BigInt(width) * BigInt(height) * 4n),
      "NATIVE_ART_RGBA_SIZE",
    );
    images.set(paths[i], { rgba, width: Number(width), height: Number(height) });
  }

  const tables = new Map();
  for (const table of officialTables) {
    const bytes = raw.get("global/excel/" + table.file);
    requireValue(
      (await sha256(bytes)) === table.sha256,
      `native_art：游戏文件里的 ${table.file} 和冻结的不一样（游戏版本不对？要 3.3.94012）`,
    );
    tables.set(table.file, tsvDictRows(utf8Decoder.decode(bytes)));
  }
  const atlases = new Map();
  const files = {};
  let outputBytes = 0;
  for (const asset of assets) {
    const path = originalPath(asset.official_path);
    requireValue(raw.has(path) && images.has(path), "NATIVE_ART_INPUT_SET");
    const bytes = raw.get(path);
    requireValue(
      (await sha256(bytes)) === asset.official_sprite_sha256 &&
        (await md5(bytes)) === asset.official_sprite_md5,
      `native_art：${asset.official_path} 的原件和冻结的不一样（游戏版本不对？）`,
    );
    const { rgba, width, height } = images.get(path);
    requireValue(
      (await sha256(rgba)) === asset.decoded_rgba_sha256,
      `native_art：${asset.asset} 解出的 RGBA 像素与冻结的不一致`,
    );
    const png = await codec.encodeRgba(rgba, width, height);
    requireValue(
      png instanceof Uint8Array && acceptsNativeArtPngDigest(asset, await sha256(png)),
      `native_art：${asset.asset} 解出的 PNG 与冻结的不一致（Pillow 版本？）`,
    );
    files[asset.asset] = png.slice();
    outputBytes += png.length;
    atlases.set(asset.asset, asset);
  }
  const atl = Array.from({ length: CLASS_CODES.size }, (_, index) => {
    const name = `dm_native_skill_${index}.png`;
    requireValue(atlases.has(name), "NATIVE_ART_CLASS_ATLAS");
    return atlases.get(name);
  });
  const descriptions = new Map(
    tables.get("skilldesc.txt").map((row) => [requiredEntry(row, "skilldesc"), row]),
  );
  const rows = [];
  for (const row of tables.get("skills.txt")) {
    const id = pythonInteger(requiredEntry(row, "*Id"));
    let char = CLASS_CODES.get(requiredEntry(row, "charclass")) ?? -1;
    const description = descriptions.get(requiredEntry(row, "skilldesc"));
    const cel = description?.has("IconCel") ? description.get("IconCel") : "";
    requireValue(typeof cel === "string", "NATIVE_ART_INTEGER");
    let frame = pythonStrip(cel) ? pythonInteger(cel) : -1n;
    if (char < 0 || frame < 0n || frame + 1n >= pythonNumber(atl[char].frame_count)) {
      char = -1;
      frame = -1n;
    }
    if (COMMON_ICON.has(id)) {
      frame = COMMON_ICON.get(id);
      requireValue(
        char >= 0 && frame + 1n < pythonNumber(atl[char].frame_count),
        "NATIVE_ART_COMMON_ICON",
      );
    }
    rows.push([id, char, frame]);
  }
  requireValue(unique(rows.map((row) => row[0])), "NATIVE_ART_DUPLICATE_ID");
  const canonical = "[" + rows.map((row) => "[" + row.join(",") + "]").join(",") + "]";
  requireValue(
    (await sha256(utf8(canonical))) === manifest.descriptor_index_sha256,
    "native_art：技能索引和冻结的不一致",
  );
  const out = [
    "/* 从冻结原版表生成，不手改。 | Generated from frozen official tables; do not hand-edit. */",
    '#include "dm_touch_native_art.h"',
    '#include "../../host/dm_input.h"',
    "#include <stddef.h>",
    "const dm_ta_atlas dm_ta_atlases[DM_TA_ATLASES]={",
  ];
  for (const asset of atl)
    out.push(
      ` {"${asset.asset.slice(0, -4)}",${[
        ...asset.atlas_px,
        asset.frame_width,
        asset.frame_pitch,
        asset.frame_count,
      ]
        .map(decimal)
        .join(",")}},`,
    );
  out.push(
    "};",
    `const char dm_ta_manifest_sha[]="${metadataHashes.manifest}";`,
    `const char dm_ta_geometry_sha[]="${metadataHashes.geometry}";`,
    `const char dm_ta_binding_sha[]="${metadataHashes.binding}";`,
    "static const struct { uint32_t id; int atlas,frame; } rows[]={",
  );
  for (const row of rows) out.push(` {${row.join(",")}},`);
  out.push("};", metadata.implementation.replace(/\r\n?/g, "\n"));
  files["dm_touch_native_art.c"] = utf8(out.join("\n") + "\n");
  outputBytes += files["dm_touch_native_art.c"].length;
  requireValue(outputBytes <= 20 * MIB, "NATIVE_ART_OUTPUT_LIMIT");
  return { files, assetCount: assets.length };
}
