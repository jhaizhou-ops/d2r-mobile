// 现行译名的小地图终点配方；固定 gen_map_goal_art.py 的顺序、布局和 PNG 字节。 | Current-name minimap goal recipe preserving pinned gen_map_goal_art.py order, layout and PNG bytes.
import {
  backend,
  base64,
  decoder,
  exactKeys,
  object,
  payloadBytes,
  png,
  positiveInteger,
  requireValue,
  utf8,
} from "./util.mjs";
import { parsePythonJson } from "./native-art-util.mjs";
import { RELEASE_ART_REVISION, requireArtRevision } from "./revision.mjs";
const NPC = [212, 196, 145],
  OBJ = [255, 255, 255];
export const LABELS = Object.freeze(
  [
    ["levels", "Tal Rasha's Tomb", OBJ],
    ["monsters", "Bloodraven", NPC],
    ["objects", "StoneAlpha", OBJ],
    ["objects", "Inifuss", OBJ],
    ["monsters", "The Countess", NPC],
    ["item-names", "hdm", OBJ],
    ["monsters", "Radament", NPC],
    ["item-names", "box", OBJ],
    ["item-names", "Staff of Kings", OBJ],
    ["objects", "taintedsunaltar", OBJ],
    ["monsters", "The Summoner", NPC],
    ["item-names", "qey", OBJ],
    ["item-names", "qbr", OBJ],
    ["item-names", "qhr", OBJ],
    ["objects", "compellingorb", OBJ],
    ["item-names", "bbb", OBJ],
    ["levels", "Ruined Temple", OBJ],
    ["monsters", "Izual", NPC],
    ["objects", "Hellforge", OBJ],
    ["item-names", "Seal", OBJ],
    ["monsters", "Siege Boss", NPC],
    ["objects", "cagedwussie1", OBJ],
    ["npcs", "Drehya", NPC],
    ["objects", "orifice", OBJ],
    ["objects", "gidbinn altar", OBJ],
    ["monsters", "Andariel", NPC],
    ["monsters", "Duriel", NPC],
    ["monsters", "Mephisto", NPC],
    ["monsters", "Baal", NPC],
    ["objects", "ancientsaltar", OBJ],
    ["npcs", "Nihlathak", NPC],
  ].map((label) => Object.freeze([...label.slice(0, 2), Object.freeze([...label[2]])])),
);
const LANGUAGES = ["zhCN", "enUS", "esES", "esMX", "ruRU", "koKR"];
const SUFFIXES = ["", "_en", "_es", "_mx", "_ru", "_ko"];
const TABLES = [...new Set(LABELS.map(([name]) => name))];
export function generateMapGoal(payload, options, { codec, kernels, font }) {
  payloadBytes(payload, 20 * 1024 * 1024, "MAP_GOAL_INPUT_LIMIT");
  requireValue(
    (exactKeys(options, ["names"]) || exactKeys(options, ["names", "revision"])) &&
      options.names === "current",
    "MAP_GOAL_CURRENT_NAMES_REQUIRED",
  );
  requireArtRevision(Object.hasOwn(options, "revision") ? options.revision : RELEASE_ART_REVISION);
  const languages = LANGUAGES;
  const suffixes = SUFFIXES;
  const bundle = parsePythonJson(decoder.decode(payload));
  requireValue(exactKeys(bundle, ["images", "font", "strings"]), "MAP_GOAL_INPUT_SET");
  requireValue(exactKeys(bundle.images, ["symbols", "units"]), "MAP_GOAL_IMAGE_SET");
  requireValue(exactKeys(bundle.strings, TABLES), "MAP_GOAL_STRING_SET");
  const strings = new Map();
  for (const [name, text] of Object.entries(bundle.strings)) {
    requireValue(
      typeof text === "string" && utf8(text).length <= 2 * 1024 * 1024,
      "MAP_GOAL_STRING_LIMIT",
    );
    const rows = JSON.parse(text.replace(/^\uFEFF+/u, ""));
    requireValue(Array.isArray(rows) && rows.length <= 20000, "MAP_GOAL_STRING_ROWS");
    const indexed = new Map();
    for (const row of rows) {
      requireValue(object(row) && typeof row.Key === "string", "MAP_GOAL_STRING_ROW");
      requireValue(!indexed.has(row.Key), "MAP_GOAL_DUPLICATE_KEY");
      indexed.set(row.Key, row);
    }
    for (const [table, key] of LABELS)
      if (table === name) {
        requireValue(indexed.has(key), "MAP_GOAL_MISSING_KEY");
        for (const lang of languages) {
          const value = indexed.get(key)[lang];
          // 每个码点最多占两个 UTF-16 单元，先拒绝超长输入，再进行有界遍历。
          // Each code point uses at most two UTF-16 units; reject oversized input before bounded iteration.
          requireValue(
            typeof value === "string" && value.length > 0 && value.length <= 512,
            "MAP_GOAL_LABEL_LIMIT",
          );
          let count = 0;
          for (const char of value)
            requireValue(++count <= 256 && char.codePointAt(0) >= 32, "MAP_GOAL_LABEL_LIMIT");
        }
      }
    // 后续原配方只剥除一个 UTF-8 BOM，不能把多个 BOM 悄悄接受。 | The original later strips only one UTF-8 BOM, so do not silently accept multiple BOMs.
    const renderRows = JSON.parse(text.startsWith("\ufeff") ? text.slice(1) : text);
    strings.set(name, new Map(renderRows.map((row) => [row.Key, row])));
  }
  let pixels = 0;
  const images = {};
  for (const [name, value] of Object.entries(bundle.images)) {
    requireValue(
      object(value) && Array.isArray(value.size) && value.size.length === 2,
      "MAP_GOAL_DIMENSIONS",
    );
    const [width, height] = value.size,
      fw = value.frameWidth,
      frames = value.frames;
    requireValue([width, height, fw, frames].every(positiveInteger), "MAP_GOAL_DIMENSIONS");
    requireValue(
      frames === (name === "symbols" ? 7 : 8) && width % frames === 0 && fw <= width / frames,
      "MAP_GOAL_FRAME_LAYOUT",
    );
    pixels += width * height;
    requireValue(pixels <= 2097152, "MAP_GOAL_PIXEL_LIMIT");
    const data = base64(value.rgba, "MAP_GOAL_RGBA_ENCODING");
    requireValue(data.length === width * height * 4, "MAP_GOAL_RGBA_SIZE");
    images[name] = { width, height, channels: 4, data, fw, pitch: width / frames };
  }
  const fontBytes = base64(bundle.font, "MAP_GOAL_FONT_ENCODING");
  requireValue(fontBytes.length > 0 && fontBytes.length <= 9 * 1024 * 1024, "MAP_GOAL_FONT_LIMIT");
  font.open(fontBytes, 48);
  const files = {};
  let outputBytes = 0,
    success = false;
  const add = (name, bytes) => {
    // 先接管新输出，再检查累计预算；失败时包括触发超限的缓冲区也须擦除。
    // Own each output before checking the aggregate budget, including the buffer that exceeds it.
    files[name] = bytes;
    outputBytes += bytes.length;
    requireValue(outputBytes <= 16 * 1024 * 1024, "MAP_GOAL_OUTPUT_LIMIT");
  };
  try {
    // 原适配器也检查别表中同名的键。 | The original adapter also checks matching keys in other tables.
    for (const indexed of strings.values())
      for (const [, key] of LABELS)
        if (indexed.has(key))
          for (const lang of languages) {
            const width = font.measure(indexed.get(key)[lang]).length;
            requireValue(width >= 0 && width <= 8172, "MAP_GOAL_RENDER_WIDTH");
          }
    for (let i = 0; i < 7; i++)
      add(
        `sym_${i}.png`,
        png(
          codec,
          kernels.crop(
            images.symbols,
            i * images.symbols.pitch,
            0,
            images.symbols.fw,
            images.symbols.height,
          ),
        ),
      );
    let cross = kernels.crop(
      images.units,
      6 * images.units.pitch,
      0,
      images.units.fw,
      images.units.height,
    );
    const alpha = kernels.point(
      kernels.channel(cross, 3),
      Uint8Array.from({ length: 256 }, (_, n) => Math.floor(n / 2)),
    );
    cross = kernels.putAlpha(cross, alpha);
    add("cross.png", png(codec, cross));
    const manifest = [];
    for (const [i, [table, key, color]] of LABELS.entries()) {
      const row = strings.get(table).get(key);
      for (const [j, lang] of languages.entries()) {
        const text = row[lang],
          width = Math.trunc(font.measure(text).length) + 20,
          height = 60;
        const data = new Uint8Array(width * height * 4);
        for (let p = 3; p < data.length; p += 4) data[p] = 150;
        const drawn = font.draw(data, width, height, text, {
          x: width / 2,
          y: height / 2,
          fill: [...color, 255],
          anchor: "mm",
        });
        add(`label_${i}${suffixes[j]}.png`, png(codec, { width, height, data: drawn }));
      }
      manifest.push({
        id: i,
        file: table,
        key,
        zhCN: row.zhCN,
        old: row.zhCN,
        enUS: row.enUS,
        esES: row.esES,
        esMX: row.esMX,
        ruRU: row.ruRU,
        koKR: row.koKR,
        color: [...color],
      });
    }
    add(
      "mapgoal.json",
      utf8(JSON.stringify({ text_px: 48, symbol_frames: 7, labels: manifest }, null, 1)),
    );
    requireValue(Object.keys(files).length === 195, "MAP_GOAL_OUTPUT_SET");
    success = true;
    return { files, backend: { ...backend } };
  } finally {
    if (!success) for (const bytes of Object.values(files)) bytes.fill(0);
    font.close();
  }
}
