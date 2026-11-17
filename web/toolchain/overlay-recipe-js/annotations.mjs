/** 按固定源版本手工移植原生注释与字符串处理流水线。
 * 公开源码行为：不包含可选私有 oldnames.tsv/comments.tsv。
 * readAsset/readReplacement 使用规范化小写 data/... 路径；
 * 调用方在此处提供清单选定的资源字节前须先认证它们。
 * Handwritten port of the source-pinned native annotation/string pipeline.
 * Public-source behavior: optional private oldnames.tsv/comments.tsv are absent.
 * readAsset/readReplacement use canonical lowercase data/... paths. The caller
 * authenticates manifest-selected asset bytes before exposing them here.
 */
import DATA from "./annotation-data.json" with { type: "json" };
import { releaseI18n } from "./release-v2.mjs";

export const annotationSources = Object.freeze({ ...DATA.sources });
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });
const STR = "data/local/lng/strings/";
const EXCEL = "data/global/excel/";
const LANGS = ["zhCN", "esES", "esMX"];
const ITEM_FILES = ["item-names.json", "chinese-overlay.json"];
const D = DATA.drop;
const own = (o, k) => Object.hasOwn(o, k);
const get = (o, k, fallback = "") => (own(o, k) ? o[k] : fallback);
const points = (s) => Array.from(s);
// 每个码点至多占两个 UTF-16 单元，无需为长度判断展开整个字符串。
// Each code point takes at most two UTF-16 units; length checks need no full character array.
const withinCodePoints = (text, limit) => {
  if (text.length > limit * 2) return false;
  let count = 0;
  for (const _char of text) if (++count > limit) return false;
  return true;
};
const escapeRegex = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const newline = (s) => s.replace(/\r\n?/g, "\n");
const validUnicode = (text) => {
  for (const c of text) {
    const value = c.codePointAt(0);
    if (value >= 0xd800 && value <= 0xdfff) fail("ANNOT_INVALID_UNICODE");
  }
  return text;
};
const encode = (s, bom = false) => encoder.encode((bom ? "\ufeff" : "") + validUnicode(s));
const fullMatch = (pattern, value) => {
  const match = new RegExp("^(?:" + pattern + ")$").exec(value);
  return match !== null && match[0].length === value.length;
};
const escTsv = (s) => s.replaceAll("\\", "\\\\").replaceAll("\n", "\\n").replaceAll("\t", "\\t");
const fail = (code, context = "") => {
  throw new Error(`${code}${context ? ": " + context : ""}`);
};
const integer = (s) => {
  if (!/^[+-]?\d+$/.test(String(s).trim())) fail("ANNOT_INTEGER", String(s));
  const n = Number(s);
  if (!Number.isSafeInteger(n)) fail("ANNOT_INTEGER", String(s));
  return n;
};
const pythonSort = (a, b) => {
  const aa = points(a),
    bb = points(b);
  for (let i = 0; i < Math.min(aa.length, bb.length); i++) {
    const d = aa[i].codePointAt(0) - bb[i].codePointAt(0);
    if (d) return d;
  }
  return aa.length - bb.length;
};
// Python json.dumps(..., ensure_ascii=False)，使用默认的逗号与冒号空格。
// Python json.dumps(..., ensure_ascii=False), default comma/colon spaces.
const json = (v) => {
  if (Array.isArray(v)) return "[" + v.map(json).join(", ") + "]";
  if (v && typeof v === "object")
    return (
      "{" +
      Object.entries(v)
        .map(([k, x]) => json(k) + ": " + json(x))
        .join(", ") +
      "}"
    );
  if (typeof v === "string") validUnicode(v);
  return JSON.stringify(v);
};
const format = (fmt, ...args) => {
  let i = 0;
  return fmt.replace(/%[%sd]/g, (m) => (m === "%%" ? "%" : String(args[i++])));
};
const braces = (fmt, values) => fmt.replace(/\{([^}]+)\}/g, (_, k) => values[k]);
function readText(bytes, path) {
  if (!(bytes instanceof Uint8Array)) fail("ANNOT_ASSET_MISSING", path);
  return decoder.decode(bytes);
}

// csv.DictReader(..., delimiter='\t')：保留引号内的制表符、换行和双写引号。
// 忽略空白记录，但不忽略含空字段的记录。
// csv.DictReader(..., delimiter='\t'): preserve quoted tabs/newlines and doubled
// quotes. Blank records are ignored, unlike records containing empty fields.
function table(text) {
  const rows = [];
  let row = [],
    value = "",
    quoted = false,
    start = true,
    active = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === '"') {
        if (text[i + 1] === '"') {
          value += '"';
          i++;
        } else quoted = false;
      } else value += c;
      active = true;
    } else if (c === '"' && start) {
      quoted = true;
      start = false;
      active = true;
    } else if (c === "\t") {
      row.push(value);
      value = "";
      start = true;
      active = true;
    } else if (c === "\n" || c === "\r") {
      if (c === "\r" && text[i + 1] === "\n") i++;
      if (active || row.length || value) {
        row.push(value);
        rows.push(row);
      }
      row = [];
      value = "";
      start = true;
      active = false;
    } else {
      value += c;
      start = false;
      active = true;
    }
  }
  if (active || row.length || value) {
    row.push(value);
    rows.push(row);
  }
  const fields = rows.shift() || [];
  return rows.map((values) =>
    Object.fromEntries(fields.map((field, i) => [field, values[i] ?? null])),
  );
}
const indexBy = (rows, key) => new Map(rows.filter((r) => r[key]).map((r) => [r[key], r]));

class Strings {
  constructor(readAsset, readReplacement) {
    this.readAsset = readAsset;
    this.readReplacement = readReplacement;
    this.text = new Map();
    this.dirty = new Set();
    this.col = "zhCN";
  }
  skip(name) {
    return this.col !== "zhCN" && name === "chinese-overlay.json";
  }
  load(name) {
    if (this.text.has(name)) return;
    const path = STR + name;
    // 原生实现始终先读取并认证原始资源，再读取替换资源。
    // Native always reads/authenticates the original before reading a replacement.
    let text = readText(this.readAsset(path), path);
    const replacement = this.readReplacement(path);
    if (replacement) text = newline(readText(replacement, path));
    this.text.set(name, text);
  }
  split(value) {
    const tag = this.col === "zhCN" ? "" : value.match(/^(?:\[[fm][sp]\])+/)?.[0] || "";
    return [tag, value.slice(tag.length)];
  }
  iter(name, key, lang = this.col, skip = true) {
    if (skip && this.skip(name)) return [];
    this.load(name);
    const escapedKey = skip ? key.replaceAll('"', '\\"') : key;
    const regex = new RegExp(
      '"Key":\\s*"' + escapeRegex(escapedKey) + '",.*?"' + lang + '":\\s*("(?:[^"\\\\]|\\\\.)*")',
      "gs",
    );
    return Array.from(this.text.get(name).matchAll(regex), (match) => ({
      value: match[1],
      end: match.index + match[0].length,
      start: match.index + match[0].length - match[1].length,
    }));
  }
  has(name, key) {
    return this.iter(name, key).length > 0;
  }
  keys(name) {
    this.load(name);
    return Array.from(this.text.get(name).matchAll(/"Key":\s*"((?:[^"\\]|\\.)*)"/g), (m) => m[1]);
  }
  get(name, key) {
    const m = this.iter(name, key)[0];
    return m ? this.split(JSON.parse(m.value))[1] : null;
  }
  getLang(name, key, lang) {
    const m = this.iter(name, key, lang, false)[0];
    return m ? JSON.parse(m.value) : null;
  }
  set(name, key, fn) {
    const matches = this.iter(name, key);
    if (!matches.length) return 0;
    let text = this.text.get(name);
    for (const m of matches.reverse()) {
      const [tag, value] = this.split(JSON.parse(m.value));
      let next = fn(value);
      if (this.col !== "zhCN") next = next.replaceAll("[", "(").replaceAll("]", ")");
      text = text.slice(0, m.start) + json(tag + next) + text.slice(m.end);
    }
    this.text.set(name, text);
    this.dirty.add(name);
    return matches.length;
  }
  write() {
    return new Map(
      Array.from(this.dirty)
        .sort(pythonSort)
        .map((name) => [STR + name, encode(this.text.get(name), true)]),
    );
  }
}

function runeNumbers(s) {
  let n = 0;
  for (let i = 1; i <= 33; i++) {
    const key = "r" + String(i).padStart(2, "0");
    if (s.col !== "zhCN") {
      const name = s.get("item-runes.json", key),
        short = s.get("item-runes.json", key + "L");
      if (
        !name ||
        !short ||
        !fullMatch("\\S+ " + escapeRegex(short), name) ||
        !fullMatch("[A-Za-z']+", short)
      )
        fail("ANNOT_RUNE_SOURCE", `${s.col}/${key}`);
      n += s.set("item-runes.json", key, () => `${name} #${i}`);
      n += s.set("item-runes.json", key + "L", () => `${short}#${i} `);
    } else {
      const zh = s.get("item-runes.json", key + "L"),
        en = s.getLang("item-runes.json", key + "L", "enUS");
      if (!zh || !en || !fullMatch("[A-Za-z']+", en)) fail("ANNOT_RUNE_SOURCE", key);
      n += s.set("item-runes.json", key, () => `${zh} ${en} #${i}`);
      n += s.set("item-runes.json", key + "L", () => `${zh}#${i} `);
    }
  }
  return n;
}
function isShield(code, types) {
  const seen = new Set();
  while (code && !seen.has(code)) {
    if (code === "shld") return true;
    seen.add(code);
    code = types.get(code)?.Equiv1 || "";
  }
  return false;
}
function baseTiers(s, excel) {
  const [[wn, wu, wl], speeds, fmt] = DATA.tiers[s.col];
  const types = indexBy(excel("itemtypes.txt"), "Code");
  let n = 0;
  for (const tab of ["weapons.txt", "armor.txt"])
    for (const r of excel(tab)) {
      const code = r.code;
      if (!code || r.spawnable !== "1" || !["", "0"].includes(r.quest) || r.type === "tpot")
        continue;
      let tier =
        code === r.normcode ? wn : code === r.ubercode ? wu : code === r.ultracode ? wl : null;
      if (!tier) continue;
      if (tab === "armor.txt" && (r.type === "tors" || isShield(r.type, types))) {
        if (!own(speeds, r.speed)) fail("ANNOT_ARMOR_SPEED", code);
        tier += "·" + speeds[r.speed];
      }
      for (const file of ITEM_FILES)
        n += s.set(file, r.namestr || code, (text) => format(fmt, text, tier));
    }
  return n;
}
function ranges(s, excel) {
  const zh = s.col === "zhCN";
  const names = zh ? DATA.ranges.STAT_NAMES : DATA.ranges.STAT_NAMES_ES;
  const maxlen = zh ? 4 : 16,
    generic = zh ? "技能" : "Habilidad",
    sep = zh ? "" : " ";
  const skills = new Map();
  for (const row of excel("skills.txt")) {
    const id = get(row, "*Id").trim(),
      skill = get(row, "skill").trim();
    if (id && skill) {
      const value = s.get("skills.json", "skillname" + id);
      skills.set(skill, value && withinCodePoints(value, maxlen) ? value : generic);
    }
  }
  const props = indexBy(excel("properties.txt"), "code");
  function line(row, runeword, slots) {
    const parts = [];
    for (let i = 1; i <= slots; i++) {
      const code = get(row, (runeword ? "T1Code" : "prop") + i).trim();
      if (!code) continue;
      const low = get(row, (runeword ? "T1Min" : "min") + i).trim();
      const high = get(row, (runeword ? "T1Max" : "max") + i).trim();
      const clean = code.replace(/^\*+/, "");
      if (
        !DATA.ranges.RANDOM_FUNCS.includes(props.get(clean)?.func1) ||
        low === high ||
        !low ||
        !high
      )
        continue;
      const param = get(row, (runeword ? "T1Param" : "par") + i).trim();
      let label = names[code] || names[clean];
      if (DATA.ranges.SKILL_CODES.includes(code) || DATA.ranges.SKILL_CODES.includes(clean))
        label = param ? (skills.get(param) || generic) + "+" : generic;
      label ||= code;
      parts.push(
        `${label}${label.endsWith("+") ? "" : sep}${code === "ac" ? "+" : ""}${low}-${high}${code.includes("%") ? "%" : ""}`,
      );
      if (parts.length === 3) break;
    }
    return parts.join(sep + "/" + sep);
  }
  let n = 0;
  function annotate(row, isRune, count, color) {
    const value = line(row, isRune, count);
    if (!value) return;
    const key = get(row, isRune ? "Name" : "index");
    for (const file of isRune ? ["item-runes.json", "chinese-overlay.json"] : ITEM_FILES)
      n += s.set(file, key, (text) =>
        text.startsWith("ÿc5[") ? text : `ÿc5[${value}]\n${color}${text}`,
      );
  }
  for (const r of excel("uniqueitems.txt"))
    if (r.disabled !== "1" && r.spawnable === "1") annotate(r, false, 12, "ÿc4");
  for (const r of excel("setitems.txt")) annotate(r, false, 8, "ÿc2");
  for (const r of excel("runes.txt")) if (r.complete === "1") annotate(r, true, 7, "ÿc4");
  return n;
}
function levels(s, excel) {
  const fields = ["MonLvlEx", "MonLvlEx(N)", "MonLvlEx(H)"];
  const groups = new Map();
  for (const row of excel("levels.txt"))
    if (row.LevelName) {
      if (!groups.has(row.LevelName)) groups.set(row.LevelName, new Map());
      const values = fields.map((f) => get(row, f));
      groups.get(row.LevelName).set(json(values), values);
    }
  let n = 0;
  for (const [key, group] of groups) {
    if (group.size > 1) continue;
    const values = group
      .values()
      .next()
      .value.map((x) => integer(x || 0));
    if (values.every((x) => x === 0)) continue;
    n += s.set(
      "levels.json",
      key,
      (text) => `${text}${s.col === "zhCN" ? "" : " "}[${values.join("/")}]`,
    );
  }
  return n;
}
function shrines(s, excel) {
  const words = DATA.shrines[s.col];
  let n = 0;
  for (const row of excel("shrines.txt")) {
    const key = row.StringName;
    if (!key || key === "ShrId0" || get(row, "*Effect").includes("(Not Used)")) continue;
    const fmt = words.effect[row.Code];
    const details = [fmt ? format(fmt, get(row, "Arg0"), get(row, "Arg1")) : row["*Effect"]];
    const reset = get(row, "reset time in minutes");
    details.push(reset ? format(words.reset, reset) : words.noreset);
    const frames = integer(get(row, "Duration in frames") || 0);
    if (frames) details.push(format(words.dur, format(words.sec, Math.floor(frames / 25))));
    n += s.set("shrines.json", key, (text) => `ÿc5${details.join(words.sep)}\nÿc0${text}`);
  }
  return n;
}

class Tables {
  constructor(excel) {
    this.misc = indexBy(excel("misc.txt"), "code");
    this.weap = indexBy(excel("weapons.txt"), "code");
    this.armor = indexBy(excel("armor.txt"), "code");
    this.types = indexBy(excel("itemtypes.txt"), "Code");
    this.cube = excel("cubemain.txt");
    this.runes = indexBy(excel("runes.txt"), "*Rune Name");
    this.tc = excel("treasureclassex.txt");
    this.monstats = excel("monstats.txt");
    this.superuniques = excel("superuniques.txt");
    this.automagic = excel("automagic.txt");
    this.prefix = excel("magicprefix.txt");
    this.suffix = excel("magicsuffix.txt");
    this.parents = new Map();
    for (const r of this.tc)
      for (let k = 1; k <= 10; k++) {
        const item = get(r, "Item" + k);
        if (item) {
          if (!this.parents.has(item)) this.parents.set(item, new Set());
          this.parents.get(item).add(r["Treasure Class"]);
        }
      }
  }
  ancestors(type) {
    const out = new Set(),
      stack = [type];
    while (stack.length) {
      const x = stack.pop();
      if (x && !out.has(x)) {
        out.add(x);
        const row = this.types.get(x) || {};
        stack.push(get(row, "Equiv1"), get(row, "Equiv2"));
      }
    }
    return out;
  }
  gemRune(code) {
    for (const r of this.cube) {
      const ins = Array.from(
        { length: 7 },
        (_, i) => (get(r, "input " + (i + 1)) || "").split(",")[0],
      );
      const out = get(r, "output");
      if (ins.includes(code) && /^r\d\d$/.test(out) && ins.some((x) => /^r\d\d$/.test(x)))
        return integer(out.slice(1));
    }
    return null;
  }
  sources(code) {
    const seen = new Set(),
      stack = [code];
    while (stack.length)
      for (const p of this.parents.get(stack.pop()) || [])
        if (!seen.has(p)) {
          seen.add(p);
          stack.push(p);
        }
    const out = [];
    for (const r of this.superuniques)
      if (Object.keys(r).some((c) => c.startsWith("TC") && c.includes("(H)") && seen.has(r[c])))
        out.push(["su", r.Superunique, r.Name]);
    if (out.length) return out;
    for (const r of this.monstats)
      if (
        Object.keys(r).some(
          (c) => c.startsWith("TreasureClass") && c.includes("(H)") && seen.has(r[c]),
        )
      )
        out.push(["mon", r.Id, r.NameStr]);
    return out;
  }
  classMagic(code) {
    const ancestors = this.ancestors(this.weap.get(code).type);
    function best(tab, mods, spawn) {
      let value = 0;
      for (const r of tab) {
        if (spawn && r.spawnable !== "1") continue;
        const its = Array.from({ length: 7 }, (_, i) => get(r, "itype" + (i + 1)));
        const excluded = Array.from({ length: 5 }, (_, i) => get(r, "etype" + (i + 1)));
        if (!its.some((x) => ancestors.has(x)) || excluded.some((x) => ancestors.has(x))) continue;
        for (let k = 1; k <= 3; k++)
          if (mods.includes(r["mod" + k + "code"]))
            value = Math.max(value, integer(r["mod" + k + "max"] || 0));
      }
      return value;
    }
    return [
      best(this.automagic, ["skilltab"], false) + best(this.prefix, ["skilltab"], true),
      best(this.suffix, ["swing1", "swing2", "swing3"], true),
    ];
  }
  craftGlove(code) {
    const r = this.armor.get(code),
      family = [r.normcode, r.ubercode, r.ultracode];
    for (const c of this.cube)
      if (family.includes((get(c, "input 1") || "").split(",")[0]))
        for (let k = 1; k <= 5; k++)
          if (own(D.CRAFT_GLOVE_MODS, c["mod " + k])) return c["mod " + k];
    return null;
  }
}
function stringsGet(s, key) {
  let value = null;
  for (const file of D.NAME_FILES) if (s.has(file, key)) value = s.get(file, key);
  return value;
}
function runeLine(code, name) {
  const m = /^(.+) ([A-Za-z']+) (#\d+)$/.exec(name);
  if (!m || m[0].length !== name.length) return null;
  const i = integer(code.slice(1)),
    text = "ÿc8" + m[1] + " ÿc5" + m[2] + "ÿc2" + m[3];
  if (i < D.HIGH_RUNE) return text;
  const bar = "ÿc8" + "—".repeat(i >= 28 ? 13 : 9),
    gap = "\n".repeat(i >= 27 ? 4 : 2);
  return bar + gap + text + gap + bar;
}
function act(col, n) {
  return col === "zhCN"
    ? ` 第${n}幕`
    : ` Acto ${col === "esES" ? ["", "I", "II", "III", "IV", "V"][integer(n)] : n}`;
}
function miscRule(t, s, code, name) {
  const row = t.misc.get(code),
    type = get(row, "type"),
    words = D.WORDS[s.col];
  if (own(words.short, code)) return words.short[code];
  const match = own(words.strip, code) ? new RegExp(words.strip[code]).exec(name) : null;
  if (match) {
    const result = name.slice(0, match.index) + name.slice(match.index + match[0].length);
    const first = result[Symbol.iterator]().next().value || "";
    return first.toUpperCase() + result.slice(first.length);
  }
  if (["hpot", "mpot"].includes(type) && /\d$/.test(code)) {
    const i = integer(code.at(-1)) - 1;
    if (i < 0 || i >= words.size.length) fail("ANNOT_POTION_SIZE", code);
    const word = braces(words.pot, { s: words.size[i], c: words.pot_color[type] }).trim();
    return i === 4 ? (type === "hpot" ? "ÿc1" : "ÿc3") + word : word;
  }
  if (own(D.GEM_COLOR, type)) {
    const g = (type === "gemz" ? D.SKULL_GRADE_CH : D.GEM_GRADE_CH)[code[type === "gemz" ? 2 : 1]];
    if (g === undefined) return null;
    const word = words.grade[g],
      color = D.GEM_GRADE[g][1];
    const base = word ? color + braces(words.gem, { w: word, c: words.gem_color[type] }) : name;
    const n = g <= 2 ? t.gemRune(code) : null;
    return base + (n ? format(words.gem_rune, n) : "");
  }
  if (type === "rune" && /^r\d\d$/.test(code)) return runeLine(code, name);
  if (own(D.SOURCE_NOTE, code)) {
    const sources = t.sources(code);
    if (!sources.length) fail("ANNOT_DROP_SOURCE", code);
    const who = Array.from(new Set(sources.map((x) => stringsGet(s, x[2]) || x[2]))).join(words.or);
    const actNumber = sources.map((x) => D.BOSS_ACT[x[1]]).find((x) => x !== undefined);
    const color = D.SOURCE_COLOR[code] || "ÿc2";
    const nm = (file, key) => s.get(file, key).split("\n").at(-1).replace(/ÿc./g, "");
    const note =
      s.col === "zhCN"
        ? words.source[code]
        : braces(words.source[code], {
            torch: nm("item-names.json", "Hellfire Torch"),
            token: nm("item-modifiers.json", "toa"),
          });
    return `${color}[${who} · ${note}]\n${color}${name}${actNumber ? act(s.col, actNumber) : ""}`;
  }
  const m = /Act ?(\d)/.exec(get(row, "name"));
  return m && type === "ques" && code.startsWith("ua") ? name + act(s.col, m[1]) : null;
}
function baseRule(t, s, code, name) {
  const parts = [],
    words = D.WORDS[s.col];
  const weapon = t.weap.get(code),
    armor = t.armor.get(code);
  if (D.CLASS_MAGIC.includes(code)) parts.push(format(words.class_magic, ...t.classMagic(code)));
  // Python 空字典为假值，而 JS 空对象不是。
  // Python empty dicts are false; JS empty objects are not.
  const v0 = D.VALUE_BASES[code],
    vb = v0 && Object.keys(v0).length ? v0 : D.WEAPON_VALUE[code];
  if (vb?.value) {
    const k = vb.eth ? 1.5 : 1;
    if (weapon) {
      const lo = integer(weapon.mindam || weapon["2handmindam"]),
        hi = integer(weapon.maxdam || weapon["2handmaxdam"]);
      parts.push(
        format(words.weapon_value, weapon.speed || "0", lo, Math.floor(Math.ceil(hi * k) * 1.15)),
      );
    } else {
      const lo = integer(armor.minac),
        hi = integer(armor.maxac),
        top = Math.floor(Math.floor((hi + 1) * k) * 1.15);
      parts.push(vb.value === "range" ? format(words.def_range, lo, top) : format(words.def, top));
    }
  }
  if (own(D.TOP_RUNEWORDS, code)) {
    const names = [];
    for (const rw of D.TOP_RUNEWORDS[code]) {
      const r = t.runes.get(rw);
      const its = Array.from({ length: 6 }, (_, i) => get(r, "itype" + (i + 1)));
      const ancestors = t.ancestors((weapon || armor).type);
      if (!its.some((x) => ancestors.has(x))) fail("ANNOT_RUNEWORD_BASE", `${code}/${rw}`);
      names.push((stringsGet(s, r.Name) || rw).split("\n").at(-1).replace(/ÿc./g, ""));
    }
    parts.push(words.can_make + names.join(" / "));
  }
  const glove = armor?.type === "glov" ? t.craftGlove(code) : null;
  let out = name;
  if (own(D.VALUE_BASES, code)) out = words.star + out;
  if (glove) out += format(words.craft, words.craft_mods[glove]);
  if (parts.length) out = "[" + parts.join(" · ") + "]\n" + out;
  if (D.FRAMED.includes(code)) out = "—".repeat(7) + "\n" + out + "\n" + "—".repeat(7);
  return out === name ? null : out;
}
function drops(s, excel) {
  const t = new Tables(excel);
  let n = 0;
  for (const [tab, rule] of [
    [t.misc, miscRule],
    [t.weap, baseRule],
    [t.armor, baseRule],
  ])
    for (const [code, row] of tab) {
      const key = row.namestr || code,
        current = [];
      for (const file of D.FILES) if (s.has(file, key)) current.push(s.get(file, key));
      if (!current.length) continue;
      const next = rule(t, s, code, current.at(-1));
      if (next === null || next === current.at(-1)) continue;
      for (const file of D.FILES) n += s.set(file, key, () => next);
    }
  return n;
}
function textboxTable(full, plain) {
  const rt = (s) => s.replaceAll("ÿc", "\ue07e");
  const order = [
    ...D.FILES.filter((name) => full.dirty.has(name)),
    ...Array.from(full.dirty)
      .filter((name) => !D.FILES.includes(name))
      .sort(pythonSort),
  ];
  const tab = new Map(),
    source = new Map();
  for (const col of LANGS) {
    full.col = plain.col = col;
    const effective = new Map();
    for (const name of order)
      for (const key of full.keys(name)) {
        const a = full.get(name, key),
          b = plain.get(name, key);
        if (a !== null && b !== null)
          effective.set(json([D.FILES.includes(name) ? "*" : name, key]), [a, b]);
      }
    for (const [key, [a, b]] of effective) {
      if (!a.includes("ÿc") || a === b) continue;
      const ra = rt(a),
        rb = rt(b);
      if (tab.has(ra) && tab.get(ra) !== rb && source.get(ra)[0] !== col) {
        tab.set(ra, ra.replace(/\ue07e./g, ""));
        source.set(ra, [source.get(ra)[0] + "/" + col, key]);
        continue;
      }
      if (tab.has(ra) && tab.get(ra) !== rb) fail("ANNOT_TEXTBOX_CONFLICT", key);
      tab.set(ra, rb);
      source.set(ra, [col, key]);
    }
  }
  full.col = plain.col = "zhCN";
  return tab;
}

/** 所有方法均为异步（sha1 可为同步或异步）。资源读取仍为同步：
 * 有界工作线程已持有经过认证的源字节。返回的 Map 保留插入顺序。
 * 附属文件使用无 BOM 的 UTF-8；字符串替换恰含一个 BOM，与原生 utf-8-sig 写入一致。
 * All methods are async (sha1 may be synchronous or async). Asset reads remain
 * synchronous: the bounded worker already holds the authenticated source bytes.
 * Returned Maps are insertion ordered. Sidecars are UTF-8 without BOM; string
 * replacements have exactly one BOM, as native utf-8-sig writes do.
 */
export function createAnnotations({
  readAsset,
  readReplacement = () => undefined,
  sha1,
  upstream = true,
  release = false,
}) {
  if (upstream !== true) fail("OVERLAY_RECIPE_REVISION");
  const touchData = DATA.touch;
  if (
    typeof readAsset !== "function" ||
    typeof readReplacement !== "function" ||
    typeof sha1 !== "function"
  )
    fail("ANNOT_DEPENDENCIES");
  const excelCache = new Map();
  const excel = (name) => {
    if (!excelCache.has(name))
      excelCache.set(name, table(readText(readAsset(EXCEL + name), EXCEL + name)));
    return excelCache.get(name);
  };
  async function sourceHash(value, expected, context) {
    const hash = await sha1(encode(value));
    if (typeof hash !== "string" || !/^[0-9a-f]{40}$/.test(hash) || hash.slice(0, 8) !== expected)
      fail("TOUCH_STRING_SOURCE", context);
  }
  async function buildTouchStrings() {
    const replacements = new Map(),
      lines = [],
      seen = new Set();
    let count = 0;
    const strings = new Strings(readAsset, () => undefined);
    for (const [name, items] of Object.entries(touchData.tables.zhCN)) {
      strings.load(name);
      for (const [key, [hash, next]] of Object.entries(items))
        if (touchData.fileOnly.includes(key)) {
          const match = strings.iter(name, key)[0];
          if (!match) fail("TOUCH_STRING_KEY", `${name}/${key}`);
          await sourceHash(JSON.parse(match.value), hash, `${name}/${key}`);
          const text = strings.text.get(name);
          strings.text.set(name, text.slice(0, match.start) + json(next) + text.slice(match.end));
          count++;
        }
      if (Object.keys(items).some((key) => touchData.fileOnly.includes(key)))
        replacements.set(STR + name, encode(strings.text.get(name), true));
    }
    // 原生附属文件校验的是原始资源，而非仅用于文件的替换内容。
    // Native sidecar validates original assets, never the file-only replacements.
    const originals = new Strings(readAsset, () => undefined);
    for (const name of Object.keys(touchData.tables.zhCN))
      for (const [lang, tables] of Object.entries(touchData.tables)) {
        for (const [key, [hash, next]] of Object.entries(tables[name] || {})) {
          if (lang === "zhCN" && touchData.fileOnly.includes(key)) continue;
          const value = originals.getLang(name, key, lang);
          if (value === null) fail("TOUCH_STRING_KEY", `${name}/${key}/${lang}`);
          await sourceHash(value, hash, `${name}/${key}/${lang}`);
          const row = [key, hash, next],
            identity = json(row);
          if (lang === "zhCN" || !seen.has(identity)) {
            lines.push(row);
            seen.add(identity);
          }
        }
      }
    return {
      replacements,
      count,
      touchStrings: encode(lines.map(([k, h, v]) => `${k}\t${h}\t${escTsv(v)}\n`).join("")),
    };
  }
  async function buildI18n() {
    const data = release ? releaseI18n : DATA.i18n,
      strings = new Strings(readAsset, readReplacement);
    strings.load("ui.json");
    let text = strings.text.get("ui.json");
    const have = new Set(strings.keys("ui.json")),
      used = new Set();
    const fmt = new RegExp(data.format, "g"),
      proper = new RegExp(data.proper, "g");
    const add = data.rows.map(([key, zh, en, es, mx, ru, ko], i) => {
      if (!/^dm[A-Za-z0-9]+$/.test(key) || !zh || !en || !es || !ru || !ko || used.has(key))
        fail("I18N_ROW", key);
      used.add(key);
      if (have.has(key)) fail("I18N_KEY_CLASH", key);
      mx ||= es;
      const placeholders = json(zh.match(fmt) || []);
      const limit = data.enLimits.find(([pat]) => new RegExp("^(?:" + pat + ")$").test(key))?.[1];
      for (const t of [en, es, mx, ru, ko]) {
        if (
          !data.enHasZh.includes(key) &&
          /[\u3000-\u9fff\uff00-\uffef]/.test(t.replace(proper, ""))
        )
          fail("I18N_UNEXPECTED_CHINESE", key);
        if (json(t.match(fmt) || []) !== placeholders) fail("I18N_PLACEHOLDERS", key);
        if (t.split("\n").length !== en.split("\n").length) fail("I18N_NEWLINES", key);
        if (limit !== undefined && points(t.replace(fmt, "000").split("\n")[0]).length > limit)
          fail("I18N_LENGTH", key);
      }
      const out = { id: data.id0 + i, Key: key };
      for (const lang of data.langs)
        out[lang] = ["zhCN", "zhTW"].includes(lang)
          ? zh
          : lang === "esES"
            ? es
            : lang === "esMX"
              ? mx
              : lang === "ruRU"
                ? ru
                : lang === "koKR"
                  ? ko
                  : en;
      return out;
    });
    const end = text.trimEnd().lastIndexOf("]");
    if (end < 0) fail("I18N_JSON");
    text =
      text.slice(0, end).trimEnd() +
      ",\n" +
      add.map((row) => "  " + json(row)).join(",\n") +
      "\n" +
      text.slice(end);
    JSON.parse(text);
    return { replacements: new Map([[STR + "ui.json", encode(text, true)]]), count: add.length };
  }
  async function buildItemNotes() {
    const s = new Strings(readAsset, readReplacement),
      stat = {};
    for (const col of LANGS) {
      s.col = col;
      const st = { 旧译名: 0, 可变范围: ranges(s, excel) };
      // 原生 comments() 即使缺少可选表也仍会读取 skills。
      // Native comments() still reads skills even when its optional table is absent.
      if (col === "zhCN")
        for (const key of s.keys("skills.json"))
          if (key.toLowerCase().startsWith("skillname")) s.get("skills.json", key);
      st["点评"] = 0;
      st["符文序号"] = runeNumbers(s);
      st["底材档次"] = baseTiers(s, excel);
      st["区域等级"] = levels(s, excel);
      st["神殿效果"] = shrines(s, excel);
      st["掉落名"] = drops(s, excel);
      if (col === "zhCN") Object.assign(stat, st);
      else {
        delete st["旧译名"];
        delete st["点评"];
        stat[col] = st;
      }
    }
    s.col = "zhCN";
    const plain = new Strings(readAsset, readReplacement);
    for (const col of LANGS) {
      plain.col = col;
      runeNumbers(plain);
      baseTiers(plain, excel);
      levels(plain, excel);
    }
    plain.col = "zhCN";
    const tab = textboxTable(s, plain);
    stat["文字框对照"] = tab.size;
    const text =
      "# 注释版\t文字框版（annot_strings.py 生成，宿主 dm_textbox.c 读）\n" +
      Array.from(tab.keys())
        .sort(pythonSort)
        .map((key) => `${escTsv(key)}\t${escTsv(tab.get(key))}\n`)
        .join("");
    return { replacements: s.write(), stat, textbox: encode(text) };
  }
  return Object.freeze({ buildTouchStrings, buildI18n, buildItemNotes });
}
