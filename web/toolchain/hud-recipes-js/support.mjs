// 为 HUD 配方提供布局、补丁和文字辅助函数；依据 ./python-values.mjs 的模块与调用约定。
// Provide layout, patch and text helpers for HUD recipes using the module and calling contracts in ./python-values.mjs.
import { ordered, put, keys, parsePython, roundEven, float } from "./python-values.mjs";
export function requireValue(condition, code) {
  if (!condition) throw new Error(code);
}
export function applyPatch(base, patch, depth = 0) {
  requireValue(depth <= 128, "HUD_PATCH_DEPTH");
  if (patch == null) return base;
  if (Object.hasOwn(patch, "value")) return patch.value;
  if (Object.hasOwn(patch, "dict")) {
    requireValue(
      base && typeof base === "object" && !Array.isArray(base) && Array.isArray(patch.order),
      "HUD_PATCH_DICT",
    );
    return ordered(
      patch.order.map((key) => [
        key,
        Object.hasOwn(base, key)
          ? applyPatch(base[key], patch.dict[key], depth + 1)
          : patch.dict[key].value,
      ]),
    );
  }
  if (Object.hasOwn(patch, "list")) {
    requireValue(Array.isArray(base) && Array.isArray(patch.list), "HUD_PATCH_LIST");
    return patch.list.map((item) => {
      if (!Object.hasOwn(item, "base")) return item.value;
      requireValue(
        Number.isInteger(item.base) && item.base >= 0 && item.base < base.length,
        "HUD_PATCH_INDEX",
      );
      return applyPatch(base[item.base], item.patch, depth + 1);
    });
  }
  throw new Error("HUD_PATCH_UNKNOWN");
}
export function layoutText(text) {
  return parsePython(
    text
      .replace(/^\uFEFF/, "")
      .replace(/\/\/[^\n]*/g, "")
      .replace(/,(\s*[}\]])/g, "$1"),
  );
}
const FMT = /%[-+ #0-9.]*(?:ll|l|h|z)?[a-zA-Z@%]/g;
const LIMITS = [
  ["dmRiftSay(?!Prefix).*", 31],
  ["dmRiftPtsGot", 62],
  ["dmRiftPtsHeld", 33],
  ["dmRift(Range|Best|Str|Lvl|Time)", 13],
  ["dmRiftGam(Uni|Set)", 8],
  ["dmRiftGamAfterEn", 17],
  ["dmRift(SelT|InfT|GamT|TitleRun|TitleDone|TitleLate)", 16],
  ["dmRift(GuardDown|NextOpen|Saved|RetryOpen|NotCounted)", 16],
  ["dmRiftCap.*", 20],
  ["dmRiftBtn.*", 12],
  ["dmRiftMsItem\\d+", 15],
  ["dmRiftMsSub\\d+", 19],
  ["dmRiftMsTier\\d+", 4],
  ["dmRiftMs(Got|Next|No)", 7],
  ["dmRiftMsT", 12],
  ["dmRiftMsNEn", 38],
  ["dmRiftHud(Title)", 4],
  ["dmRiftHud(Done|Late|Guard)", 18],
  ["dmRiftHud(Next|Same|Kill|Goal)", 26],
  ["dmKbRiftUnlock", 9],
  ["dmKbRift(Best|Total)", 16],
].map(([pattern, limit]) => [new RegExp("^(?:" + pattern + ")$"), limit]);
function tsv(text) {
  const rows = [];
  let row = [],
    field = "",
    quoted = false,
    start = true;
  for (let i = 0; i < text.length; i++) {
    const char = text[i];
    if (quoted) {
      if (char === '"') {
        if (text[i + 1] === '"') {
          field += '"';
          i++;
        } else quoted = false;
      } else field += char;
    } else if (char === '"' && start) {
      quoted = true;
      start = false;
    } else if (char === "\t") {
      row.push(field);
      field = "";
      start = true;
    } else if (char === "\n" || char === "\r") {
      if (char === "\r" && text[i + 1] === "\n") i++;
      row.push(field);
      if (row.length !== 1 || row[0] !== "") rows.push(row);
      row = [];
      field = "";
      start = true;
    } else {
      field += char;
      start = false;
    }
  }
  requireValue(!quoted, "HUD_I18N_QUOTE");
  if (field || row.length) {
    row.push(field);
    rows.push(row);
  }
  return rows;
}
export function vitalCharacters(text, includeKorean = true) {
  requireValue(includeKorean === true, "OVERLAY_RECIPE_REVISION");
  const all = tsv(text),
    header = all.shift();
  const names = ["键", "中文", "English", "Español", "Español MX", "Русский", "한국어"];
  requireValue(header && names.every((name) => header.includes(name)), "HUD_I18N_HEADER");
  const indices = names.map((name) => header.indexOf(name)),
    seen = new Set(),
    values = new Map();
  for (const row of all) {
    const [key, ...columns] = indices.map((i) => row[i]?.replaceAll("\\n", "\n"));
    requireValue(
      typeof key === "string" &&
        /^dm[A-Za-z0-9]+$/.test(key) &&
        columns.every((v) => typeof v === "string") &&
        columns[0] &&
        columns[1] &&
        columns[2] &&
        columns[4] &&
        columns[5],
      "HUD_I18N_ROW",
    );
    requireValue(!seen.has(key), "HUD_I18N_DUPLICATE");
    seen.add(key);
    columns[3] ||= columns[2];
    const [zh, en] = columns,
      expectedFormats = JSON.stringify(zh.match(FMT) || []);
    const limit = LIMITS.find(([pattern]) => pattern.test(key))?.[1];
    for (const value of columns.slice(1)) {
      if (!["dmCustomLangZh", "dmCustomLangHintZh"].includes(key))
        requireValue(
          !/[\u3000-\u9fff\uff00-\uffef]/.test(
            value.replace(/老机型补充包|D2R Mobile Resources|D2R Mobile/g, ""),
          ),
          "HUD_I18N_CJK",
        );
      requireValue(
        JSON.stringify(value.match(FMT) || []) === expectedFormats,
        "HUD_I18N_PLACEHOLDER",
      );
      requireValue(value.split("\n").length === en.split("\n").length, "HUD_I18N_NEWLINES");
      if (limit !== undefined)
        requireValue(
          Array.from(value.replace(FMT, "000").split("\n")[0]).length <= limit,
          "HUD_I18N_LENGTH",
        );
    }
    values.set(key, columns);
  }
  let characters = "生命法力:/ 0123456789";
  const current = new Set(characters),
    added = new Set();
  for (const key of ["dmHudLife", "dmHudMana"]) {
    requireValue(values.has(key), "HUD_I18N_VITAL");
    for (const char of values.get(key).join("").replace(FMT, ""))
      if (!current.has(char)) added.add(char);
  }
  characters += [...added].sort((a, b) => a.codePointAt(0) - b.codePointAt(0)).join("");
  return characters;
}
export function generateLayout(text, patchText, g, bounds) {
  const layout = applyPatch(layoutText(text), parsePython(patchText));
  const X = (x) => (x - g.W / 2) / (g.H / 2160),
    Y = (y) => (y - g.H) / (g.H / 2160);
  const removed = ["TouchBeltFrame", "BeltPopupArrow", "TouchHealthBallRing", "TouchManaBallRing"];
  put(
    layout,
    "children",
    layout.children.filter((child) => !removed.includes(child.name)),
  );
  const by = Object.fromEntries(layout.children.map((child) => [child.name, child]));
  for (const name of [
    "Overlay",
    "HealthBall",
    "ManaBall",
    "LevelUpButton",
    "LevelUpFlourish",
    "Belt",
    "ExpBar",
    ...Array.from({ length: 6 }, (_, i) => "SkillButton" + (i + 1)),
  ])
    requireValue(by[name]?.fields, "HUD_LAYOUT_WIDGET");
  function rect(name, values) {
    const f = by[name].fields;
    if (!Object.hasOwn(f, "rect")) put(f, "rect", ordered());
    for (const key of keys(values)) put(f.rect, key, float(roundEven(values[key], 1)));
    put(f, "anchor", { x: float(0.5), y: 1 });
  }
  put(by.Overlay.fields, "filename", "PANEL/HUD_02/BugBTN");
  delete by.Overlay.fields.transparency;
  rect("Overlay", { x: X(bounds[0]), y: Y(bounds[1]) });
  delete by.Overlay.fields.rect.scale;
  const sb = roundEven((2 * g.BR) / (348 * (g.H / 2160)), 1);
  for (const [name, center] of [
    ["HealthBall", g.LBC],
    ["ManaBall", g.RBC],
  ]) {
    rect(name, { x: X(center[0]) - 174 * sb, y: Y(center[1]) - 175 * sb, scale: sb });
    delete by[name].fields.rect.width;
    put(by[name].fields.rect, "width", 348);
    put(by[name].fields.rect, "height", 350);
    const value = by[name].children.find((item) => item.name === "ValueDisplay");
    requireValue(value?.fields, "HUD_LAYOUT_VALUE");
    put(value.fields, "rect", { x: -6000, y: 0 });
  }
  const s6 = g.slots[5],
    sl = roundEven(0.9, 1),
    lx = X((s6[0] + s6[2]) / 2) - 62 * sl,
    ly = Y((s6[1] + s6[3]) / 2) - 60 * sl;
  rect("LevelUpButton", { x: lx, y: ly, scale: sl });
  rect("LevelUpFlourish", {
    x: lx + (1179 - 1411) * sl,
    y: ly + (-1 - 265) * sl,
    scale: roundEven(1.1 * sl, 1),
  });
  const s1 = g.slots[1];
  rect("Belt", { x: X(s1[0]) + 6.5, y: Y(s1[1]) + 5, scale: roundEven(1.6, 1) });
  put(by.Belt.fields, "cellPadding", { x: 25, y: 68 });
  delete by.Belt.fields.cellOffsetInRow;
  delete by.Belt.fields.itemOffsetInCell;
  const hotkey = by.Belt.children.find((item) => item.name === "HotkeyLabelTemplate");
  requireValue(hotkey?.fields, "HUD_LAYOUT_HOTKEY");
  put(hotkey.fields, "rect", { x: -6000, y: 98 });
  rect("SkillButton6", {
    x: X((s6[0] + s6[2]) / 2) - 63,
    y: Y((s6[1] + s6[3]) / 2) - 63,
    scale: 1,
  });
  const kids = layout.children;
  const beforeOverlay = (child) => {
    kids.splice(kids.indexOf(child), 1);
    kids.splice(kids.indexOf(by.Overlay), 0, child);
  };
  beforeOverlay(by.SkillButton6);
  rect("ExpBar", { x: X(g.EX0) - 7, y: Y(g.EY) - 15, scale: roundEven(1.5, 1) });
  beforeOverlay(by.ExpBar);
  put(by.ExpBar.fields.rect, "y", float(roundEven(Y(g.slots[1][1] + 30), 1)));
  put(by.ExpBar.fields.rect, "x", -6000);
  for (let i = 1; i <= 5; i++) {
    const child = by["SkillButton" + i];
    beforeOverlay(child);
    put(
      child.fields,
      "rect",
      ordered(
        keys(by.SkillButton6.fields.rect).map((key) => [key, by.SkillButton6.fields.rect[key]]),
      ),
    );
    put(child.fields, "anchor", { x: float(0.5), y: 1 });
  }
  return layout;
}
