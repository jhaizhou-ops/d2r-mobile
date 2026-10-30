// 佣兵、百宝囊、宽仓库与掉落光的纯文本移植；依据已固定的四份原生 Python 源码。
// Pure mercenary, pouch, wide-stash and drop-light transforms, based on four pinned native Python sources.
import data from "./inventory-data.json" with { type: "json" };
import { troveSlots } from "../remaining-art-js/recipes.mjs";
import {
  parsePythonJson,
  PythonFloat,
  pythonInteger,
  utf8,
} from "../art-recipes-js/native-art-util.mjs";

export const INVENTORY_SOURCE_SHA256 = Object.freeze(data.sourceSha256);
export const INVENTORY_PATHS = Object.freeze({
  mercenary: "controller/hirelinginventorypanelhd.json",
  player: "controller/playerinventoryoriginallayouthd.json",
  mercenaryBackground:
    "data/hd/global/ui/controller/panel/hirelinginventory/v2/hirelinginventorybg.sprite",
  pouch: "controller/console/switch/playerinventoryexpansionlayouthd.json",
  bank: "controller/bankexpansionlayouthd.json",
  overlay: "controller/controlleroverlayhd.json",
});
export const STASH_SPRITES = Object.freeze({
  StashPanelBG: "controller/panel/stash/v2/stashpanelbg",
  StashPanelBGPersonal: "controller/panel/stash/v2/stashpanelbgpersonal",
  StashPanelBGShared: "controller/panel/stash/v2/stashpanelbgshared",
  AdvancedStash_Gems_Console:
    "controller/panel/stash/v2/additionalstash/advancedstash_gems_console",
  AdvancedStash_Consumables_Console:
    "controller/panel/stash/v2/additionalstash/advancedstash_consumables_console",
  AdvancedStash_Runes_Console:
    "controller/panel/stash/v2/additionalstash/advancedstash_runes_console",
});
export const DROP_TIERS = Object.freeze({
  1: "data/hd/vfx/particles/overlays/object/horadric_light/fx_horadric_light.particles",
  2: "data/hd/vfx/particles/missiles/diablo_light_shaft/fx_diablo_light_shaft.particles",
});
const fail = (message) => {
  throw new Error(message);
};
const requireValue = (value, message) => {
  if (!value) fail(message);
};
const escapeRe = (text) => text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const SPACE =
  "[\\u0009-\\u000d\\u001c-\\u0020\\u0085\\u00a0\\u1680\\u2000-\\u200a\\u2028\\u2029\\u202f\\u205f\\u3000]";
const re = (pattern, flags = "") =>
  new RegExp(
    pattern
      .replaceAll("\\s", SPACE)
      .replaceAll("\\d", "\\p{Nd}")
      .replaceAll("\\w", "[\\p{L}\\p{N}_]"),
    flags + "u",
  );
const rstrip = (text) => text.replace(re("\\s+$"), "");
const integer = (text) => pythonInteger(text);
const safeInteger = (n) =>
  n >= BigInt(Number.MIN_SAFE_INTEGER) && n <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(n) : n;
const count = (text, token) => text.split(token).length - 1;
const index = (text, token, start = 0) => {
  const at = text.indexOf(token, start);
  requireValue(at >= 0, "substring not found");
  return at;
};

// 字符串与注释感知的原生扫描器；括号位置不改写。
// Native string/comment-aware scanner; preserves exact brace positions.
export function inventoryBlockEnd(text, start) {
  let depth = 0,
    quoted = false;
  for (let i = start; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === "\\") i++;
      else if (c === '"') quoted = false;
    } else if (c === '"') quoted = true;
    else if (text.slice(i, i + 2) === "//") {
      i = text.indexOf("\n", i);
      if (i < 0) break;
    } else if (c === "{") depth++;
    else if (c === "}" && --depth === 0) return i;
  }
  fail("touch_layout：括号不配对");
}
const ROWS = {
  902: 1064,
  903: 1065,
  905: 1067,
  994: 1132,
  993: 1131,
  995: 1133,
  1088: 1200,
  1089: 1201,
  1090: 1202,
  1183: 1268,
  1185: 1270,
  1279: 1336,
};
const BAND = [
  "CharacterName",
  "HireTypeText",
  "XpProgress",
  "NextLevelXP",
  "CurrentXP",
  "Skill0",
  "Skill1",
  "Skill2",
];
const STATS = [
  "HPTitle",
  "HPStat",
  "StrengthTitle",
  "StrengthStat",
  "DexTitle",
  "DexStat",
  "DamageTitle",
  "Damage",
  "ArmorClassTitle",
  "ArmorClass",
  "FireResistanceTitle",
  "FireText",
  "ColdResistanceTitle",
  "ColdText",
  "LightningResistanceTitle",
  "LightningText",
  "PoisonResistanceTitle",
  "PoisonText",
];
const NEW = ["neck", "gloves", "right_hand", "belt", "left_hand", "feet"];
const ORIGINAL = ["head", "torso", "right_arm", "left_arm"];
const NAV = {
  head: { left: "right_arm", right: "neck", down: "torso" },
  neck: { left: "head", right: "left_arm", down: "left_hand" },
  torso: { left: "right_arm", right: "left_arm", up: "head", down: "belt" },
  right_arm: { right: "torso", down: "gloves" },
  left_arm: { left: "neck", up: "head", down: "feet" },
  gloves: { up: "right_arm", right: "right_hand" },
  right_hand: { left: "gloves", right: "belt", up: "torso" },
  belt: { left: "right_hand", right: "left_hand", up: "torso" },
  left_hand: { left: "belt", right: "feet", up: "neck" },
  feet: { left: "left_hand", up: "left_arm" },
};
const nav = (slot) => {
  const rows = Object.entries(NAV[slot]).map(
    ([direction, target]) => `"${direction}": { "name": "slot_${target}" }`,
  );
  if (slot === "left_arm" || slot === "feet") rows.push('"right": { "panelBoundary": true }');
  return '"navigation": { ' + rows.join(", ") + " }";
};
function block(text, name, blockEnd) {
  const match = re(
    '\\{\\s*"type"\\s*:\\s*"\\w+"\\s*,\\s*"name"\\s*:\\s*"' + escapeRe(name) + '"',
  ).exec(text);
  requireValue(match, "merc_slots：布局里没有控件 " + name);
  return [match.index, blockEnd(text, match.index) + 1];
}
function setY(text, name, fn, blockEnd) {
  const [a, b] = block(text, name, blockEnd),
    part = text.slice(a, b);
  const match = re('("rect"\\s*:\\s*\\{\\s*"x"\\s*:\\s*-?\\d+\\s*,\\s*"y"\\s*:\\s*)(-?\\d+)').exec(
    part,
  );
  requireValue(match, `merc_slots：控件 ${name} 没有 rect 的 y`);
  const pos = match.index + match[1].length;
  return (
    text.slice(0, a) +
    part.slice(0, pos) +
    fn(integer(match[2])) +
    part.slice(pos + match[2].length) +
    text.slice(b)
  );
}
export function mercenaryRects(playerText, { blockEnd = inventoryBlockEnd } = {}) {
  const result = {};
  for (const slot of [...ORIGINAL, ...NEW]) {
    const [a, b] = block(playerText, "slot_" + slot, blockEnd);
    const match = re(
      '"rect"\\s*:\\s*\\{\\s*"x"\\s*:\\s*(\\d+)\\s*,\\s*"y"\\s*:\\s*(\\d+)\\s*,\\s*"width"\\s*:\\s*(\\d+)\\s*,\\s*"height"\\s*:\\s*(\\d+)\\s*\\}',
    ).exec(playerText.slice(a, b));
    requireValue(match, `merc_slots：玩家物品栏布局里 slot_${slot} 没有 rect`);
    const [x, y, w, h] = match.slice(1).map(integer);
    result[slot] = [safeInteger(x - 70n), safeInteger(y - 21n), safeInteger(w), safeInteger(h)];
  }
  return result;
}
export function mercenaryTransform(text, playerText, { blockEnd = inventoryBlockEnd } = {}) {
  for (const name of BAND) text = setY(text, name, (y) => y + 216n, blockEnd);
  for (const [name, y] of [
    ["CharacterName", 884],
    ["HireTypeText", 984],
  ]) {
    const [a, b] = block(text, name, blockEnd);
    let part = text.slice(a, b),
      n1 = 0,
      n2 = 0;
    part = part.replace(
      re(
        '("rect"\\s*:\\s*\\{\\s*"x"\\s*:\\s*-?\\d+\\s*,\\s*"y"\\s*:\\s*)-?\\d+(\\s*,\\s*"width"\\s*:\\s*\\d+\\s*,\\s*"height"\\s*:\\s*)\\d+',
      ),
      (_m, p, q) => {
        n1++;
        return `${p}${y}${q}60`;
      },
    );
    part = part.replace(re('"v"\\s*:\\s*"bottom"'), () => {
      n2++;
      return '"v": "center"';
    });
    requireValue(
      n1 === 1 && n2 === 1,
      `merc_slots：${name} 的 rect / 竖向对齐和原版布局对不上（原版布局变了？）`,
    );
    text = text.slice(0, a) + part + text.slice(b);
  }
  for (const name of STATS)
    text = setY(
      text,
      name,
      (y) => {
        requireValue(
          Object.hasOwn(ROWS, y),
          `merc_slots：属性表控件 ${name} 的 y = ${y} 不在行表里（原版布局变了？）`,
        );
        return ROWS[y];
      },
      blockEnd,
    );
  const rects = mercenaryRects(playerText, { blockEnd });
  for (const slot of ORIGINAL) {
    const [a, b] = block(text, "slot_" + slot, blockEnd);
    let part = text.slice(a, b);
    const i = index(part, '"navigation"'),
      j = blockEnd(part, index(part, "{", i)) + 1;
    part = part.slice(0, i) + nav(slot) + part.slice(j);
    const [x, y, w, h] = rects[slot];
    let n = 0;
    part = part.replace(re('"rect"\\s*:\\s*\\{[^}]*\\}'), () => {
      n++;
      return `"rect": { "x": ${x}, "y": ${y}, "width": ${w}, "height": ${h} }`;
    });
    requireValue(n === 1, `merc_slots：佣兵面板 slot_${slot} 没有 rect`);
    text = text.slice(0, a) + part + text.slice(b);
  }
  let add = "";
  for (const slot of NEW) {
    const [a, b] = block(playerText, "slot_" + slot, blockEnd),
      [x, y, w, h] = rects[slot];
    const match = re('"backgroundFilename"\\s*:\\s*"([^"]+)"').exec(playerText.slice(a, b));
    requireValue(match, "missing backgroundFilename");
    add += `,\n        { "type": "InventorySlotWidget", "name": "slot_${slot}", "fields": { "rect": { "x": ${x}, "y": ${y}, "width": ${w}, "height": ${h} }, "backgroundFilename": "${match[1]}", ${nav(slot)} } }`;
  }
  for (const slot of NEW) {
    const [, y, w, h] = rects[slot];
    add += `,\n        { "type": "RectangleWidget", "name": "TouchMercLock_${slot}", "fields": { "rect": { "x": 99999, "y": ${y}, "width": ${w}, "height": ${h} }, "color": [ 0.187, 0.019, 0.022, 0.78 ] } }`;
  }
  const [, b] = block(text, "slot_left_arm", blockEnd);
  return text.slice(0, b) + add + text.slice(b);
}

export const pouchWidgets = () => data.pouchWidgets;
export function invLayout(text) {
  requireValue(!text.includes('"TouchPouchBox"'), "pouch_layout：物品栏布局里已经有 TouchPouchBox");
  const stripped = rstrip(text),
    i = stripped.lastIndexOf("]");
  requireValue(stripped.endsWith("}") && i > 0, "");
  return text.slice(0, i).replace(/ +$/, "") + pouchWidgets() + "    " + text.slice(i);
}
function shift(text, name, dx, start = 0) {
  const match = re('"name"\\s*:\\s*"' + escapeRe(name) + '"').exec(text.slice(start));
  requireValue(match, "wide_stash：储藏箱布局里没有控件 " + name);
  const i = start + match.index + match[0].length;
  const rect = re('"rect"\\s*:\\s*\\{\\s*"x"\\s*:\\s*(-?\\d+)').exec(text.slice(i));
  requireValue(
    rect && !text.slice(i, i + rect.index).includes('"name"'),
    `wide_stash：控件 ${name} 的 rect 不在它自己的字段里`,
  );
  const at = i + rect.index + rect[0].length - rect[1].length;
  return text.slice(0, at) + (integer(rect[1]) + BigInt(dx)) + text.slice(at + rect[1].length);
}
function containerX(text, name, dx) {
  const match = re('"name"\\s*:\\s*"' + escapeRe(name) + '"').exec(text);
  requireValue(match, "missing container " + name);
  const i = match.index + match[0].length,
    next = text.indexOf('"children"', i),
    fields = text.indexOf('"fields"', i);
  if (fields !== -1 && fields < next) return shift(text, name, dx);
  return (
    text.slice(0, i) + `,\n            "fields": { "rect": { "x": ${dx}, "y": 0 } }` + text.slice(i)
  );
}
export function troveSlotsLayout(text) {
  const pattern = re(
    '\\{\\s*"type"\\s*:\\s*"AdvancedStashSlotWidget"\\s*,\\s*"name"\\s*:\\s*"(\\w+)"\\s*,\\s*"fields"\\s*:\\s*\\{\\s*"rect"\\s*:\\s*\\{[^}]*\\}\\s*,\\s*"itemCode"\\s*:\\s*"(\\w+)"\\s*,?\\s*\\}\\s*,?\\s*\\}\\s*,?',
    "g",
  );
  const matches = [...text.matchAll(pattern)],
    codes = matches.map((m) => m[2]),
    want = troveSlots();
  requireValue(
    JSON.stringify(codes.toSorted()) === JSON.stringify(Object.keys(want).sort()) &&
      matches.every((m) => m[1] === m[2]),
    `wide_stash：高级仓库槽控件不是原版那 91 个（找到 ${codes.length} 个；原版布局变了？）`,
  );
  text = text.replace(pattern, "");
  const i =
    index(text, "[", index(text, '"children"', index(text, '"name": "advancedstash_gems"'))) + 1;
  const rows = Object.keys(want)
    .sort()
    .map((code) => {
      const [x, y, w, h] = want[code];
      return `                { "type": "AdvancedStashSlotWidget", "name": "${code}", "fields": { "rect": { "width": ${w}, "height": ${h}, "x": ${x}, "y": ${y} }, "itemCode": "${code}" } }`;
    });
  return text.slice(0, i) + "\n" + rows.join(",\n") + "\n            " + text.slice(i);
}
export function bankLayout(text) {
  const original = '"cellCount": { "x": 10, "y": 10 }',
    n = count(text, original);
  requireValue(
    n === 1,
    `wide_stash：储藏箱格子 cellCount 10×10 应为 1 处，找到 ${n} 处（原版布局变了？）`,
  );
  text = text.replace(original, '"cellCount": { "x": 14, "y": 10 }');
  const touchOnly = new Set(["TouchStashLeft", "TouchStashRight", "close"]);
  for (const [names, dx] of [
    [["gold_amount", "gold_withdraw", "StashNavigation", "TouchStashLeft", "TouchStashRight"], 196],
    [["ShowPreviousLadderSeasonToggle", "close"], 392],
  ]) {
    for (const name of names) {
      if (touchOnly.has(name) && !re('"name"\\s*:\\s*"' + escapeRe(name) + '"').test(text))
        continue;
      text = shift(text, name, dx);
    }
  }
  text = troveSlotsLayout(text);
  for (const name of [
    "advancedstash_gems",
    "advancedstash_materials",
    "advancedstash_runes",
    "advancedstash_horadriccube",
  ])
    text = containerX(text, name, 196);
  text = shift(text, "advancedstash_horadriccube", -72);
  return vaultTab(text);
}
export function vaultTab(text) {
  for (const [a, b] of [
    ['"tabCount": 5,', '"tabCount": 6,'],
    ['"inactiveFrames": [ 0, 0, 0, 0, 0 ],', '"inactiveFrames": [ 0, 0, 0, 0, 0, 0 ],'],
    ['"activeFrames": [ 1, 1, 1, 1, 1 ],', '"activeFrames": [ 1, 1, 1, 1, 1, 1 ],'],
    ['"disabledFrames": [-1, 0, 0, 0, 0 ],', '"disabledFrames": [-1, 0, 0, 0, 0, -1 ],'],
    [
      '"textStrings": [ "@personal", "@shared", "@gems", "@materials", "@runes" ],',
      '"textStrings": [ "@personal", "@shared", "@dmTroveTab", "@materials", "@runes", "@dmStashVaultTab" ],',
    ],
  ]) {
    requireValue(
      count(text, a) === 1,
      `wide_stash：储藏箱布局 ${a.trim()} 不是原样（原版布局变了？）`,
    );
    text = text.replace(a, b);
  }
  const i = index(text, '"name": "BankTabs"'),
    key = '"tabRightIndicatorPosition": "$HalfPanelRightTabButtonPromptPosition",',
    j = text.indexOf(key, i);
  // 先检查 UTF-16 范围，避免为超长布局创建切片及逐字符数组。
  // Check the UTF-16 span before slicing or expanding an oversized layout.
  let withinLimit = j >= 0 && j - i <= 5000;
  if (withinLimit) {
    let count = 0;
    for (const _char of text.slice(i, j))
      if (++count > 2500) {
        withinLimit = false;
        break;
      }
  }
  requireValue(withinLimit, "wide_stash：BankTabs 的右端键帽位置不是原样（原版布局变了？）");
  text =
    text.slice(0, j) +
    '"tabRightIndicatorPosition": { "x": 920, "y": -2 },' +
    text.slice(j + key.length);
  const end = rstrip(rstrip(text).replace(/}+$/, ""));
  requireValue(end.endsWith("]"), "");
  const at = end.length - 1;
  return (
    rstrip(text.slice(0, at)).replace(/,+$/, "") +
    ",\n        " +
    data.pageWidget +
    ",\n        " +
    data.vaultWidgets +
    "\n    " +
    text.slice(at)
  );
}
export function overlayLayout(text) {
  const i = text.indexOf('"name": "Background"');
  let j = text.indexOf('"TabBarWidget", "name": "Tabs"');
  requireValue(i >= 0 && j >= 0, "wide_stash：外框布局里没有 Background / Tabs（原版布局变了？）");
  let k = text.slice(0, j).lastIndexOf("{");
  const add =
    '{\n                    "type": "ImageWidget", "name": "WideBackground",\n                    "fields": {\n                        "rect": { "x": 99999, "y": 0 },\n                        "filename": "Controller/Panel/zswide/MainPanelBG_Wide",\n                    },\n                },\n                ';
  text = text.slice(0, k) + add + text.slice(k);
  j = text.indexOf('"TabBarWidget", "name": "Tabs"');
  k = text.slice(0, j).lastIndexOf("{");
  let depth = 0,
    end = k;
  for (; end < text.length; end++) {
    if (text[end] === "{") depth++;
    else if (text[end] === "}" && --depth === 0) break;
  }
  requireValue(depth === 0, "wide_stash：外框布局里 Tabs 控件的括号没配上（原版布局变了？）");
  return text.slice(0, end + 1) + ",\n                " + data.sideTitle + text.slice(end + 1);
}
export function patchInventory(bytes, path) {
  requireValue(bytes instanceof Uint8Array, "inventory bytes required");
  const out = bytes.slice(),
    view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  for (const row of [10, 25]) {
    const offset = 4 + row * 240 + 16;
    requireValue(
      out[offset] === 10 && out[offset + 1] === 10,
      `wide_stash：${path} 第 ${row} 行不是 10×10（原版表变了？）`,
    );
    out[offset] = 14;
  }
  for (const [row, dx, dy] of [
    [11, -320, 0],
    [26, -240, 60],
  ]) {
    const base = 4 + row * 240 + 0x28;
    requireValue(
      [0, 4, 8, 12].every((offset) => view.getInt32(base + 4 * 0x14 + offset, true) === -1),
      `wide_stash：${path} 第 ${row} 行的项链格不是空的（原版表变了？）`,
    );
    for (let k = 4; k < 10; k++) {
      const values = Array.from({ length: 5 }, (_, i) =>
        view.getInt32(4 + 0x28 + k * 0x14 + i * 4, true),
      );
      for (let i = 0; i < 5; i++) {
        const value = values[i] + (i < 2 ? dx : i < 4 ? dy : 0);
        requireValue(
          value >= -2147483648 && value <= 2147483647,
          "'i' format requires -2147483648 <= number <= 2147483647",
        );
        view.setInt32(base + k * 0x14 + i * 4, value, true);
      }
    }
  }
  return out;
}

// Python 的 JSON 浮点、转义与字典顺序；仅数据解析，不执行表达式或源代码。
// Python JSON floats, escaping and dictionary order; parses data only, never expressions or source code.
const jsonOrder = new WeakMap();
function jsonString(value, ascii) {
  let out = '"';
  const escaped = { 8: "\\b", 9: "\\t", 10: "\\n", 12: "\\f", 13: "\\r", 34: '\\\"', 92: "\\\\" };
  for (let i = 0; i < value.length; i++) {
    const n = value.charCodeAt(i);
    out +=
      escaped[n] ??
      (n < 32 || (ascii && n >= 127) ? "\\u" + n.toString(16).padStart(4, "0") : value[i]);
  }
  return out + '"';
}
function floatString(value) {
  if (Number.isNaN(value)) return "NaN";
  if (!Number.isFinite(value)) return value < 0 ? "-Infinity" : "Infinity";
  if (Object.is(value, -0)) return "-0.0";
  const negative = value < 0 ? "-" : "",
    magnitude = Math.abs(value);
  if (!magnitude) return "0.0";
  const [mantissa, exponent] = magnitude.toExponential().split("e"),
    power = Number(exponent);
  if (power < -4 || power >= 16)
    return (
      negative + mantissa + "e" + (power < 0 ? "-" : "+") + String(Math.abs(power)).padStart(2, "0")
    );
  const text = String(value);
  return text.includes(".") ? text : text + ".0";
}
export function pythonJsonDumps(
  value,
  { indent = null, ensureAscii = true, separators = null } = {},
) {
  const unit =
    indent === null ? null : typeof indent === "string" ? indent : " ".repeat(Math.max(0, indent));
  const [itemSep, keySep] = separators ?? [indent === null ? ", " : ",", ": "];
  const ancestors = new Set();
  const dump = (item, depth) => {
    if (item === null) return "null";
    if (typeof item === "string") return jsonString(item, ensureAscii);
    if (typeof item === "boolean") return item ? "true" : "false";
    if (typeof item === "bigint") return String(item);
    if (item instanceof PythonFloat) return floatString(item.valueOf());
    if (typeof item === "number")
      return Number.isInteger(item) && Number.isFinite(item)
        ? BigInt(item).toString()
        : floatString(item);
    requireValue(item && typeof item === "object", "Object is not JSON serializable");
    requireValue(!ancestors.has(item), "Circular reference detected");
    ancestors.add(item);
    const array = Array.isArray(item),
      known = jsonOrder.get(item) ?? [];
    const keys = [
      ...known.filter((key) => Object.hasOwn(item, key)),
      ...Object.keys(item).filter((key) => !known.includes(key)),
    ];
    const entries = array ? item : keys.map((key) => [key, item[key]]);
    const rows = entries.map((entry) =>
      array
        ? dump(entry, depth + 1)
        : jsonString(entry[0], ensureAscii) + keySep + dump(entry[1], depth + 1),
    );
    ancestors.delete(item);
    const [left, right] = array ? ["[", "]"] : ["{", "}"];
    if (!rows.length) return left + right;
    return unit === null
      ? left + rows.join(itemSep) + right
      : left +
          "\n" +
          unit.repeat(depth + 1) +
          rows.join(itemSep + "\n" + unit.repeat(depth + 1)) +
          "\n" +
          unit.repeat(depth) +
          right;
  };
  return dump(value, 0);
}
export function parseInventoryJson(text) {
  const root = parsePythonJson(text);
  let at = 0;
  const whitespace = () => {
    while (/[\x20\t\r\n]/.test(text[at] ?? "!")) at++;
  };
  const string = () => {
    const start = at++;
    while (at < text.length) {
      const c = text[at++];
      if (c === '"') return JSON.parse(text.slice(start, at));
      if (c === "\\") at++;
    }
    fail("Invalid JSON string");
  };
  const scan = () => {
    whitespace();
    if (text[at] === "{") {
      const fields = new Map();
      at++;
      whitespace();
      while (text[at] !== "}") {
        const key = string();
        whitespace();
        at++;
        fields.set(key, scan());
        whitespace();
        if (text[at] !== ",") break;
        at++;
        whitespace();
      }
      at++;
      return fields;
    }
    if (text[at] === "[") {
      at++;
      whitespace();
      const values = [];
      while (text[at] !== "]") {
        values.push(scan());
        whitespace();
        if (text[at] !== ",") break;
        at++;
        whitespace();
      }
      at++;
      return values;
    }
    if (text[at] === '"') string();
    else {
      while (at < text.length && !/[\x20\t\r\n,\]}]/.test(text[at])) at++;
    }
    return null;
  };
  const install = (value, shape) => {
    if (shape instanceof Map) {
      jsonOrder.set(value, [...shape.keys()]);
      for (const [key, child] of shape) install(value[key], child);
    } else if (Array.isArray(shape)) shape.forEach((child, i) => install(value[i], child));
  };
  install(root, scan());
  return root;
}
export function variantPath(root, asset, tier) {
  return (
    `data/hd/items/${root}/zswg/${tier}__` + asset.toLowerCase().replaceAll("/", "__") + ".json"
  );
}
export function addLight(text, fx, name, { md5 } = {}) {
  const object = parseInventoryJson(text);
  requireValue(object?.type === "UnitDefinition", name);
  const dependencies = object.dependencies;
  requireValue(
    dependencies !== null && typeof dependencies === "object" && !Array.isArray(dependencies),
    "dependencies must be an object",
  );
  if (!Object.hasOwn(dependencies, "particles")) {
    dependencies.particles = [];
    jsonOrder.get(dependencies)?.push("particles");
  }
  const particles = dependencies.particles;
  if (!particles.some((p) => (Object.hasOwn(p, "path") ? p.path : "").toLowerCase() === fx))
    particles.push({ path: fx });
  requireValue(typeof md5 === "function", "DROP_LIGHT_MD5_REQUIRED");
  requireValue(typeof name === "string", "drop light name must be a string");
  const hash = md5(utf8("droplight:" + name));
  requireValue(typeof hash === "string" && /^[0-9a-f]{32}$/i.test(hash), "DROP_LIGHT_MD5_RESULT");
  const id =
    (Number.parseInt(hash.slice(0, 2), 16) |
      (Number.parseInt(hash.slice(2, 4), 16) << 8) |
      (Number.parseInt(hash.slice(4, 6), 16) << 16) |
      (Number.parseInt(hash.slice(6, 8), 16) << 24) |
      1) >>>
    0;
  const f = (n) => new PythonFloat(n);
  object.entities.push({
    type: "Entity",
    name: "droplight",
    id,
    components: [
      {
        type: "TransformDefinitionComponent",
        name: "component_transform_droplight",
        position: { x: f(0), y: f(0), z: f(0) },
        orientation: { x: f(0), y: f(0), z: f(0), w: f(1) },
        scale: { x: f(1), y: f(1), z: f(1) },
        inheritOnlyPosition: false,
      },
      {
        type: "VfxDefinitionComponent",
        name: "component_vfx_droplight",
        filename: fx,
        hardKillOnDestroy: false,
      },
    ],
  });
  return pythonJsonDumps(object, { separators: [",", ":"] });
}
export function selectDropModels(manifestText, tables) {
  const manifest = new Set();
  for (const line of manifestText.replace(/\r\n|\r/g, "\n").split("\n")) {
    const fields = line.split("|");
    if (fields.length >= 11 && !line.startsWith("#"))
      manifest.add(fields[9].slice(fields[9].indexOf(":") + 1).toLowerCase());
  }
  const assets = new Set();
  for (const table of tables) {
    for (const row of typeof table === "string" ? parseInventoryJson(table) : table) {
      requireValue(Object.keys(row).length === 1, "item row must contain exactly one entry");
      const value = Object.values(row)[0];
      for (const asset of Object.hasOwn(value, "asset")
        ? [value.asset]
        : [value.normal, value.uber, value.ultra])
        assets.add(asset.toLowerCase());
    }
  }
  const models = [];
  let skipped = 0;
  for (const asset of [...assets].sort((a, b) => {
    const aa = Array.from(a, (c) => c.codePointAt(0)),
      bb = Array.from(b, (c) => c.codePointAt(0));
    for (let i = 0; i < Math.min(aa.length, bb.length); i++)
      if (aa[i] !== bb[i]) return aa[i] - bb[i];
    return aa.length - bb.length;
  })) {
    const root = ["weapon", "armor", "misc"].find((root) =>
      manifest.has(`data/hd/items/${root}/${asset}.json`),
    );
    if (root === undefined) {
      skipped++;
      continue;
    }
    if (
      root === "misc" &&
      !["rune/", "ring/", "amulet/", "charm/", "key/", "quest/"].some((prefix) =>
        asset.startsWith(prefix),
      )
    )
      continue;
    models.push({ root, asset, source: `data/hd/items/${root}/${asset}.json` });
  }
  return {
    models,
    skipped,
    statistics: `掉落光变体：${models.length} 个模型 × 2 档（原表指向不存在模型、跳过 ${skipped} 个）`,
  };
}
