// 固定来源的表格与路径变换；不访问网络或文件系统。 | Source-pinned table/path transforms; no network or filesystem access.
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const MAX_INPUT = 20 * 1024 * 1024;
const SOURCE_NAMES = ["dm_hud_exp.h", "dm_vault_tab.h", "dm_touch_native_art.c"];
const TAGS = ["EXP", "VTPART", "VTROW", "VTSET", "TAROW"];
const OPERATIONS = new Set(["experience-table", "overlay-paths", "derived-tables", "vault-table"]);
const LAY = "data/global/ui/layouts/";
const UI = "data/hd/global/ui/";
const TDIR = [
  ["controller/console/switch/", "0touch_console_switch_lay/"],
  ["controller/console/", "ztouch_console_lay/"],
  ["controller/", "xtouch_lay/"],
];
const PARTS = [
  "头盔",
  "盔甲",
  "盾牌与手套",
  "鞋子与腰带",
  "首饰",
  "剑与匕首",
  "斧与爪",
  "锤与棍",
  "长柄与矛",
  "弓弩",
  "法杖与法器",
];
const PART_OF = [
  ["helm", 0],
  ["head", 0],
  ["tors", 1],
  ["grim", 10],
  ["shld", 2],
  ["glov", 2],
  ["boot", 3],
  ["belt", 3],
  ["ring", 4],
  ["amul", 4],
  ["jewl", 4],
  ["char", 4],
  ["swor", 5],
  ["knif", 5],
  ["axe", 6],
  ["h2h", 6],
  ["scep", 7],
  ["staf", 10],
  ["wand", 10],
  ["orb", 10],
  ["blun", 7],
  ["pole", 8],
  ["spea", 8],
  ["jave", 8],
  ["miss", 9],
];
export const VAULT_PATHS = Object.freeze([
  ...["ItemTypes", "armor", "weapons", "misc", "UniqueItems", "Sets", "SetItems"].map(
    (name) => `data/global/excel/${name}.txt`,
  ),
  ...["uniques", "sets", "items"].map((name) => `data/hd/items/${name}.json`),
]);
function requireValue(value, code) {
  if (!value) throw new Error(code);
}
const isObject = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
const exactKeys = (value, names) =>
  isObject(value) &&
  Object.keys(value).length === names.length &&
  names.every((name) => Object.hasOwn(value, name));
function utf8(text) {
  // 逐个码点验证，不为大型清单创建逐字符数组。
  // Validate code points without allocating a character array for large manifests.
  for (const c of text) {
    const code = c.codePointAt(0);
    requireValue(code < 0xd800 || code > 0xdfff, "ASSET_UTF8");
  }
  return encoder.encode(text);
}
const hash = async (value) =>
  Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", value)), (n) =>
    n.toString(16).padStart(2, "0"),
  ).join("");
const pyLines = (text) => {
  // eslint-disable-next-line no-control-regex -- 精确 Python 换行集合。 | Exact Python splitlines set.
  const lines = text.split(/\r\n|[\n\r\v\f\x1c-\x1e\x85\u2028\u2029]/u);
  if (lines.at(-1) === "") lines.pop();
  return lines;
};
const pyStrip = (text) =>
  text.replace(
    // eslint-disable-next-line no-control-regex -- 精确 Python 空白集合。 | Exact Python whitespace set.
    /^[\u0009-\u000d\u001c-\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+|[\u0009-\u000d\u001c-\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+$/gu,
    "",
  );
// Python 整数无精度损失；接受 Unicode 十进制数字。 | Preserve Python integer precision and Unicode decimal digits.
function integer(value) {
  requireValue(typeof value === "string", "ASSET_INTEGER");
  // int() 不接受 str.strip() 允许的 ASCII 分隔控制符。 | int() excludes the ASCII separator controls accepted by str.strip().
  let text = value.replace(
    // eslint-disable-next-line no-control-regex -- 精确 Python 整数空白集合。 | Exact Python integer whitespace set.
    /^[\u0009-\u000d\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+|[\u0009-\u000d\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+$/gu,
    "",
  );
  text = text.replace(/\p{Decimal_Number}/gu, (digit) => {
    const cp = digit.codePointAt(0);
    let first = cp;
    while (first > 0 && /\p{Decimal_Number}/u.test(String.fromCodePoint(first - 1))) first--;
    return String((cp - first) % 10);
  });
  requireValue(/^[+-]?[0-9](?:_?[0-9])*$/.test(text), "ASSET_INTEGER");
  text = text.replaceAll("_", "");
  requireValue(text.replace(/^[+-]/, "").length <= 4300, "ASSET_INTEGER_LIMIT");
  return BigInt(text);
}
const inRange = (value, low, high) => value >= BigInt(low) && value <= BigInt(high);
// CSV 的双引号/跨行字段与 DictReader 空行规则。 | CSV quoting/multiline fields and DictReader blank-record behavior.
function tsvRecords(text) {
  const rows = [];
  let row = [],
    field = "",
    fieldLength = 0,
    quoted = false,
    start = true,
    touched = false;
  const append = (character) => {
    field += character;
    requireValue(++fieldLength <= 131072, "ASSET_CSV_FIELD_LIMIT");
  };
  const finish = () => {
    row.push(field);
    rows.push(row);
    row = [];
    field = "";
    fieldLength = 0;
    start = true;
    touched = false;
  };
  for (let i = 0; i < text.length; i++) {
    let c = text[i];
    // 按 Unicode 码点计数，与 Python csv.field_size_limit 一致。 | Count Unicode code points, as Python's csv.field_size_limit does.
    if (c.charCodeAt(0) >= 0xd800 && c.charCodeAt(0) <= 0xdbff && i + 1 < text.length)
      c += text[++i];
    if (quoted) {
      if (c === '"') {
        if (text[i + 1] === '"') {
          append('"');
          i++;
        } else quoted = false;
      } else append(c);
      touched = true;
    } else if (c === '"' && start) {
      quoted = true;
      start = false;
      touched = true;
    } else if (c === "\t") {
      row.push(field);
      field = "";
      fieldLength = 0;
      start = true;
      touched = true;
    } else if (c === "\n" || c === "\r") {
      if (c === "\r") {
        while (text[i + 1] === "\r") i++;
        requireValue(text[i + 1] === "\n" || i + 1 === text.length, "ASSET_CSV_NEWLINE");
        if (text[i + 1] === "\n") i++;
      }
      if (touched || row.length || field) finish();
      else rows.push([]);
    } else {
      append(c);
      start = false;
      touched = true;
    }
  }
  if (touched || row.length || field) finish();
  return rows;
}
function experience(payload) {
  const [head = [], ...rows] = tsvRecords(decoder.decode(payload));
  const values = [];
  for (const row of rows) {
    if (!row.length) continue;
    const record = Object.fromEntries(head.map((name, i) => [name, row[i] ?? null]));
    const level = Object.hasOwn(record, "Level") ? record.Level : "";
    requireValue(typeof level === "string", "ASSET_CSV_LEVEL");
    // 非十进制 isdigit 字符仍会使原生 Amazon 转换执行；数据表等级使用十进制。 | Tables use decimal levels; preserve the additional Python isdigit characters below.
    if (
      typeof level === "string" &&
      /^[\p{Decimal_Number}\u00b2\u00b3\u00b9\u1369-\u1371\u19da\u2070\u2074-\u2079\u2080-\u2089\u2460-\u2468\u2474-\u247c\u2488-\u2490\u24ea\u24f5-\u24fd\u24ff\u2776-\u277e\u2780-\u2788\u278a-\u2792\u{10a40}-\u{10a43}\u{10e60}-\u{10e68}\u{11052}-\u{1105a}\u{1f100}-\u{1f10a}]+$/u.test(
        level,
      )
    )
      values.push(integer(record.Amazon).toString());
  }
  const accepted = values.slice(0, 100);
  requireValue(accepted.length === 100, `gen_hud_exp：experience.txt 只有 ${accepted.length} 级`);
  const out = [
    "// 由原版 data/global/excel/experience.txt 生成（3.3.94012，各职业同值，取 Amazon 列）：k_exp[L] = 升到 L+1 级所需总经验；L = 0..99",
    "#pragma once",
    "#include <stdint.h>",
    "static const uint32_t k_exp[100] = {",
  ];
  for (let i = 0; i < 100; i += 10)
    out.push("    " + accepted.slice(i, i + 10).join(", ") + (i < 90 ? "," : ""));
  out.push("};");
  return { files: { "dm_hud_exp.h": encoder.encode(out.join("\n") + "\n") } };
}
export function touchPath(path) {
  const buttons = UI + "controller/controllerbuttons/";
  if (path.startsWith(buttons)) {
    const rest = path.slice(buttons.length);
    return rest.split("/").length === 2
      ? UI + "ztouch_controller_buttons_ui/" + rest.replaceAll("/", "_")
      : null;
  }
  const cursor = UI + "controller/cursor/";
  if (path.startsWith(cursor) && !path.slice(cursor.length).includes("/"))
    return UI + "xtouch_cursor_lay/" + path.slice(cursor.length);
  if (!path.startsWith(LAY) || path.includes("/_profile")) return null;
  const relative = path.slice(LAY.length);
  for (const [source, target] of TDIR) {
    if (relative.startsWith(source)) {
      const rest = relative.slice(source.length);
      return rest.includes("/") ? null : LAY + target + rest;
    }
  }
  return null;
}
// 先限制 UTF-16 单元数，再计数码点，不展开大型无效路径。
// Bound UTF-16 units before counting code points, without expanding large invalid paths.
function validPathLength(path) {
  if (path.length > 2048) return false;
  let count = 0;
  for (const _char of path) if (++count > 1024) return false;
  return true;
}
function overlayPaths(payload) {
  const paths = JSON.parse(decoder.decode(payload));
  requireValue(Array.isArray(paths) && paths.length <= 4096, "Expected at most 4096 logical paths");
  const result = paths.map((path) => {
    requireValue(
      typeof path === "string" &&
        validPathLength(path) &&
        !path.includes("\\") &&
        !path.includes(":") &&
        path.split("/").every((p) => p && p !== "." && p !== ".."),
      "Unsafe logical path",
    );
    path = path.toLowerCase();
    return { source: path, target: touchPath(path) };
  });
  // Python json.dumps 默认 ASCII 转义，包含代理对。 | Match json.dumps default ASCII escaping, including surrogate pairs.
  const text = JSON.stringify(result).replace(
    /[\u007f-\uffff]/g,
    (c) => "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"),
  );
  return { files: { "overlay-paths.json": encoder.encode(text) } };
}
// 生成器的 C 字符串还原，不执行源代码。 | Decode generated C strings without executing source text.
function unquote(text) {
  if (!text.includes("\\")) return text;
  const input = encoder.encode(text),
    out = [];
  const escapes = {
    97: 7,
    98: 8,
    102: 12,
    110: 10,
    114: 13,
    116: 9,
    118: 11,
    92: 92,
    39: 39,
    34: 34,
  };
  for (let i = 0; i < input.length; i++) {
    let c = input[i];
    if (c !== 92) {
      out.push(c);
      continue;
    }
    requireValue(++i < input.length, "DERIVED_STRING_ESCAPE");
    c = input[i];
    if (c === 10) continue;
    if (Object.hasOwn(escapes, c)) out.push(escapes[c]);
    else if (c >= 48 && c <= 55) {
      let octal = String.fromCharCode(c);
      while (octal.length < 3 && input[i + 1] >= 48 && input[i + 1] <= 55)
        octal += String.fromCharCode(input[++i]);
      const value = Number.parseInt(octal, 8);
      requireValue(value <= 255, "DERIVED_STRING_ESCAPE");
      out.push(value);
    } else if (c === 120 || c === 117 || c === 85) {
      const count = c === 120 ? 2 : c === 117 ? 4 : 8;
      const hex = String.fromCharCode(...input.subarray(i + 1, i + count + 1));
      requireValue(hex.length === count && /^[a-fA-F0-9]+$/.test(hex), "DERIVED_STRING_ESCAPE");
      const value = Number.parseInt(hex, 16);
      requireValue(value <= 255, "DERIVED_STRING_ESCAPE");
      out.push(value);
      i += count;
    } else {
      // 原生生成器不产生 Unicode 名称转义；拒绝而非猜测。 | Native generators never emit Unicode named escapes; reject rather than guess.
      requireValue(c !== 78, "DERIVED_STRING_ESCAPE");
      out.push(92, c);
    }
  }
  return decoder.decode(new Uint8Array(out));
}
const CSTR = '"((?:[^"\\\\]|\\\\.)*)"';
function match(text, pattern) {
  const result = pattern.exec(text);
  requireValue(result !== null, "DERIVED_SOURCE_FORMAT");
  return result;
}
function stringList(text) {
  return Array.from(text.matchAll(new RegExp(CSTR, "g")), (m) => unquote(m[1]));
}
function parseDerived(sources) {
  const hud = sources[SOURCE_NAMES[0]],
    vault = sources[SOURCE_NAMES[1]],
    art = sources[SOURCE_NAMES[2]];
  const exp = match(hud, /k_exp\[100\] = \{(.*?)\};/s)[1]
    .replaceAll("\n", " ")
    .split(",")
    .filter((x) => pyStrip(x))
    .map(integer);
  requireValue(exp.length === 100, "DERIVED_SOURCE_FORMAT");
  const parts = stringList(match(vault, /vt_part_name\[VT_NPART\] = \{(.*?)\};/)[1]);
  const rowPattern = new RegExp(
    "^\\s*\\{ (\\d+), (\\d+), (\\d+), (\\d+), (\\d+), (-?\\d+), " + CSTR + ", " + CSTR + " \\},$",
    "gm",
  );
  const rows = Array.from(vault.matchAll(rowPattern), (r) => [
    ...r.slice(1, 7).map(integer),
    unquote(r[7]),
    unquote(r[8]),
  ]);
  const sets = stringList(match(vault, /vt_set_key\[\] = \{(.*?)\};/)[1]);
  requireValue(
    rows.length &&
      sets.length &&
      BigInt(parts.length) === integer(match(vault, /#define VT_NPART (\d+)/)[1]),
    "DERIVED_SOURCE_FORMAT",
  );
  requireValue(vault.split("\n    { ").length - 1 === rows.length, "DERIVED_SOURCE_FORMAT");
  const body = match(
    art,
    /static const struct \{ uint32_t id; int atlas,frame; \} rows\[\]=\{\n(.*?)\n\};/s,
  )[1];
  const trows = Array.from(body.matchAll(/^ \{(\d+),(-?\d+),(-?\d+)\},$/gm), (r) =>
    r.slice(1).map(integer),
  );
  requireValue(trows.length === body.split("\n").length, "DERIVED_SOURCE_FORMAT");
  return [exp, parts, rows, sets, trows];
}
async function derived(payload, options) {
  requireValue(payload.length <= 8 * 1024 * 1024, "DERIVED_INPUT_LIMIT");
  const sources = JSON.parse(decoder.decode(payload));
  requireValue(exactKeys(sources, SOURCE_NAMES), "DERIVED_SOURCE_SET");
  const generated = options.mode === "generated";
  if (generated)
    requireValue(exactKeys(options, ["mode", "expectedSourceSha256"]), "DERIVED_OPTIONS");
  const expected = options.expectedCounts,
    hashes = options.expectedSourceSha256;
  if (!generated) requireValue(exactKeys(expected, TAGS), "DERIVED_COUNTS_REQUIRED");
  requireValue(exactKeys(hashes, SOURCE_NAMES), "DERIVED_HASHES_REQUIRED");
  const validCounts = (counts) =>
    Object.values(counts).every((n) => Number.isInteger(n) && n > 0 && n <= 100000) &&
    counts.EXP === 100 &&
    counts.VTPART <= 256 &&
    counts.VTSET <= 32768;
  if (!generated) requireValue(validCounts(expected), "DERIVED_COUNTS_RANGE");
  for (const name of SOURCE_NAMES) {
    requireValue(typeof sources[name] === "string", "DERIVED_SOURCE_TYPE");
    const encoded = utf8(sources[name]);
    requireValue(encoded.length <= 2 * 1024 * 1024, "DERIVED_SOURCE_LIMIT");
    requireValue((await hash(encoded)) === hashes[name], "DERIVED_SOURCE_HASH");
  }
  const [exp, parts, rows, sets, trows] = parseDerived(sources);
  const counts = Object.fromEntries(
    TAGS.map((tag, i) => [tag, [exp, parts, rows, sets, trows][i].length]),
  );
  requireValue(validCounts(counts), "DERIVED_COUNTS_RANGE");
  if (!generated)
    requireValue(
      TAGS.every((tag) => counts[tag] === expected[tag]),
      "DERIVED_COUNT_MISMATCH",
    );
  requireValue(
    exp.every((n) => inRange(n, 0, 0xffffffff)),
    "DERIVED_EXP_RANGE",
  );
  requireValue(
    rows.every(
      (r) =>
        inRange(r[0], 0, 1) &&
        inRange(r[1], 0, parts.length - 1) &&
        inRange(r[2], 0, 2) &&
        inRange(r[3], 0, 65535) &&
        inRange(r[4], 0, 65535) &&
        inRange(r[5], -1, sets.length - 1),
    ),
    "DERIVED_VAULT_RANGE",
  );
  requireValue(
    trows.every(
      (r) =>
        inRange(r[0], 0, 0xffffffff) &&
        inRange(r[1], -1, 0x7fffffff) &&
        inRange(r[2], -1, 0x7fffffff),
    ),
    "DERIVED_SKILL_RANGE",
  );
  requireValue(new Set(trows.map((r) => r[0])).size === trows.length, "DERIVED_DUPLICATE_SKILL");
  requireValue(
    [...parts, ...sets, ...rows.flatMap((r) => r.slice(6))].every((text) => {
      for (const char of text) {
        const point = char.codePointAt(0);
        if (point < 32 || point === 127) return false;
      }
      return true;
    }),
    "DERIVED_CONTROL_CHARACTER",
  );
  const lines = [];
  for (let i = 0; i < 100; i += 10) lines.push("EXP\t" + exp.slice(i, i + 10).join("\t"));
  for (const part of parts) lines.push("VTPART\t" + part);
  for (const row of rows) lines.push("VTROW\t" + row.join("\t"));
  for (const set of sets) lines.push("VTSET\t" + set);
  for (const row of trows) lines.push("TAROW\t" + row.join("\t"));
  let length = 0;
  const chunks = lines.map((line) => {
    requireValue(line.split("\t").length < 17 && !line.includes("\n"), "DERIVED_SOURCE_FORMAT");
    const encoded = encoder.encode(line);
    requireValue(encoded.length <= 8190, "DERIVED_RUNTIME_LINE_LIMIT");
    length += encoded.length + 1;
    requireValue(length <= 8 * 1024 * 1024, "DERIVED_OUTPUT_LIMIT");
    return encoded;
  });
  const result = new Uint8Array(length);
  let cursor = 0;
  for (const chunk of chunks) {
    result.set(chunk, cursor);
    cursor += chunk.length;
    result[cursor++] = 10;
  }
  return {
    files: { "derived.tsv": result },
    counts,
    sourceSha256: { ...hashes },
    evidence: generated ? "generated-source-receipt" : "expected-source-and-count-match",
  };
}
function base64(value) {
  requireValue(
    typeof value === "string" && value.length % 4 === 0 && !/[^A-Za-z0-9+/=]/.test(value),
    "VAULT_BASE64",
  );
  const padding = value.indexOf("=");
  requireValue(
    padding < 0 ||
      (padding >= value.length - 2 && (value.length - padding === 1 || value.endsWith("=="))),
    "VAULT_BASE64",
  );
  const decoded = atob(value);
  // 直接填充有界输出，避免字符串迭代器展开为逐字符临时列表。
  // Fill the bounded output directly, without expanding a string iterator into a character list.
  const bytes = new Uint8Array(decoded.length);
  for (let i = 0; i < decoded.length; i++) bytes[i] = decoded.charCodeAt(i);
  return bytes;
}
async function vault(payload, options) {
  requireValue(exactKeys(options, []), "VAULT_INPUT_LIMIT");
  const bundle = JSON.parse(decoder.decode(payload));
  requireValue(exactKeys(bundle, ["assets", "manifest"]), "VAULT_INPUT_SET");
  requireValue(exactKeys(bundle.assets, VAULT_PATHS), "VAULT_ASSET_SET");
  const { manifest } = bundle;
  requireValue(
    typeof manifest === "string" && utf8(manifest).length <= 4 * 1024 * 1024,
    "VAULT_MANIFEST_LIMIT",
  );
  const assets = new Map(),
    sourceSha256 = {};
  let total = 0;
  for (const name of VAULT_PATHS) {
    const raw = base64(bundle.assets[name]);
    requireValue(raw.length > 0 && raw.length <= 2 * 1024 * 1024, "VAULT_ASSET_LIMIT");
    if (name.endsWith(".txt")) {
      let lines = 0;
      for (let i = 0; i < raw.length; i++) {
        if (raw[i] === 10 || raw[i] === 13) {
          lines++;
          if (raw[i] === 13 && raw[i + 1] === 10) i++;
        }
      }
      if (raw.at(-1) !== 10 && raw.at(-1) !== 13) lines++;
      requireValue(lines <= 100001, "VAULT_ROW_LIMIT");
    } else {
      const rows = JSON.parse(decoder.decode(raw));
      requireValue(Array.isArray(rows) && rows.length <= 100000, "VAULT_JSON_ROWS");
    }
    assets.set(name, raw);
    total += raw.length;
    sourceSha256[name] = await hash(raw);
  }
  requireValue(total <= 12 * 1024 * 1024, "VAULT_TOTAL_LIMIT");
  const sprites = new Set();
  for (const line of pyLines(manifest)) {
    const fields = line.split("|");
    requireValue(
      fields.length === 11 &&
        fields[9].startsWith("data:data/hd/global/ui/items/") &&
        fields[9].endsWith(".sprite"),
      "VAULT_MANIFEST_ROW",
    );
    sprites.add(fields[9].toLowerCase().slice("data:data/hd/global/ui/".length, -".sprite".length));
  }
  const txt = (name) => {
    const lines = pyLines(
      new TextDecoder("utf-8", { ignoreBOM: true }).decode(
        assets.get(`data/global/excel/${name}.txt`),
      ),
    );
    requireValue(lines.length > 0, "VAULT_SOURCE_FORMAT");
    const head = lines[0].split("\t");
    return lines
      .slice(1)
      .filter((line) => pyStrip(line))
      .map((line) =>
        Object.fromEntries(
          line
            .split("\t")
            .slice(0, head.length)
            .map((value, i) => [head[i], value]),
        ),
      );
  };
  const js = (name) => {
    const result = Object.create(null);
    for (const row of JSON.parse(decoder.decode(assets.get(`data/hd/items/${name}.json`)))) {
      requireValue(isObject(row), "VAULT_SOURCE_FORMAT");
      for (const [key, value] of Object.entries(row)) result[key] = value;
    }
    return result;
  };
  const types = new Map();
  for (const r of txt("ItemTypes"))
    if (r.Code) {
      requireValue(Object.hasOwn(r, "Equiv1") && Object.hasOwn(r, "Equiv2"), "VAULT_SOURCE_FORMAT");
      types.set(r.Code, [r.Equiv1, r.Equiv2]);
    }
  const bases = new Map();
  for (const [cat, file] of [
    ["armor", "armor"],
    ["weapon", "weapons"],
    ["misc", "misc"],
  ]) {
    for (const row of txt(file)) {
      const code = row.code ?? "";
      if (!code) continue;
      const tier = code === row.ultracode ? 2 : code === row.ubercode ? 1 : 0;
      bases.set(code, [cat, row.type ?? "", tier, integer(row.level || "0")]);
    }
  }
  const part = (code) => {
    const seen = new Set(),
      todo = [bases.get(code)[1]];
    for (let i = 0; i < todo.length; i++) {
      const type = todo[i];
      if (!type || seen.has(type)) continue;
      seen.add(type);
      todo.push(...(types.get(type) ?? ["", ""]));
    }
    const found = PART_OF.find(([type]) => seen.has(type));
    requireValue(found, "VAULT_NATIVE_REJECTED");
    return found[1];
  };
  const uj = js("uniques"),
    sj = js("sets"),
    ij = js("items");
  const icon = (key, code, json) => {
    requireValue(typeof key === "string", "VAULT_SOURCE_FORMAT");
    const cat = bases.get(code)[0];
    const name = key
      .toLowerCase()
      .replaceAll("'", "")
      .replace(/[^a-z0-9]+/g, "_")
      .replace(/^_+|_+$/g, "");
    for (const asset of [json[name]?.normal, ij[code]?.asset]) {
      if (asset) {
        requireValue(typeof asset === "string", "VAULT_SOURCE_FORMAT");
        if (sprites.has(`items/${cat}/${asset.toLowerCase()}`)) return `Items/${cat}/${asset}`;
      }
    }
    throw new Error("VAULT_NATIVE_REJECTED");
  };
  const ok = (r) => r.spawnable !== "0" && !r.disabled;
  const rows = [];
  let rowStringBytes = 0;
  const appendRow = (row) => {
    rowStringBytes += utf8(row[5]).length + utf8(row[6]).length;
    requireValue(rowStringBytes <= 2 * 1024 * 1024, "DERIVED_OUTPUT_LIMIT");
    rows.push(row);
  };
  for (const r of txt("UniqueItems")) {
    const code = r.code ?? "";
    if (!ok(r) || !bases.has(code)) continue;
    appendRow([
      1,
      part(code),
      bases.get(code)[2],
      integer(r["*ID"]),
      integer(r.lvl || "0"),
      r.index,
      icon(r.index, code, uj),
      -1,
    ]);
  }
  const sets = txt("Sets")
    .filter((r) => r.index)
    .map((r) => r.index);
  for (const r of txt("SetItems")) {
    const code = r.item ?? "";
    if (!ok(r) || !bases.has(code) || !sets.includes(r.set)) continue;
    appendRow([
      0,
      part(code),
      bases.get(code)[2],
      integer(r["*ID"]),
      integer(r.lvl || "0"),
      r.index,
      icon(r.index, code, sj),
      sets.indexOf(r.set),
    ]);
  }
  const sortKey = (r) => (r[0] === 1 ? [r[0], r[1], r[2], r[4], r[3]] : [r[0], r[7], r[3]]);
  rows.sort((a, b) => {
    const x = sortKey(a),
      y = sortKey(b);
    for (let i = 0; i < Math.min(x.length, y.length); i++) {
      if (x[i] < y[i]) return -1;
      if (x[i] > y[i]) return 1;
    }
    return x.length - y.length;
  });
  const used = sets.filter((_, i) => rows.some((r) => r[0] === 0 && r[7] === i));
  const q = (text) => '"' + text.replaceAll("\\", "\\\\").replaceAll('"', '\\"') + '"';
  const chunks = [];
  let outputLength = 0;
  const append = (line) => {
    const chunk = utf8(line + "\n");
    outputLength += chunk.length;
    requireValue(outputLength <= 2 * 1024 * 1024, "DERIVED_OUTPUT_LIMIT");
    chunks.push(chunk);
  };
  append(
    "// 生成文件，勿手改：dm/ios/tools/gen_vault_table.py <原版 RomFS>（编年史仓库固定表，见该脚本头注释）",
  );
  append(
    "// 部位：" +
      PARTS.map(
        (p, i) => `${i} ${p} ${rows.filter((r) => r[0] === 1 && r[1] === i).length} 件`,
      ).join("、"),
  );
  append(
    `// 暗金 ${rows.filter((r) => r[0] === 1).length} 件、套装 ${used.length} 套 ${rows.filter((r) => r[0] === 0).length} 件`,
  );
  append(`#define VT_NPART ${PARTS.length}`);
  append(`static const char *const vt_part_name[VT_NPART] = { ${PARTS.map(q).join(", ")} };`);
  append(
    "typedef struct { uint8_t kind, part, tier; uint16_t row, lvl; int16_t set; const char *key, *icon; } vt_row;   // kind 1 暗金 / 0 套装件；row = 表行号（物品数据 +0x34）；set = 套装序号（暗金 −1）",
  );
  append("static const vt_row vt_rows[] = {");
  for (const r of rows)
    append(
      `    { ${r[0]}, ${r[1]}, ${r[2]}, ${r[3]}, ${r[4]}, ${r[0] === 0 ? used.indexOf(sets[r[7]]) : -1}, ${q(r[5])}, ${q(r[6])} },`,
    );
  append("};");
  append("#define VT_NROW (int)(sizeof vt_rows / sizeof vt_rows[0])");
  append(
    `static const char *const vt_set_key[] = { ${used.map(q).join(", ")} };   // 套装名的字符串键（Sets.txt index），次序 = 原版`,
  );
  append("#define VT_NSET (int)(sizeof vt_set_key / sizeof vt_set_key[0])");
  const result = new Uint8Array(outputLength);
  let offset = 0;
  for (const chunk of chunks) {
    result.set(chunk, offset);
    offset += chunk.length;
  }
  return {
    files: { "dm_vault_tab.h": result },
    sourceSha256,
    manifestSha256: await hash(utf8(manifest)),
  };
}
export const hasAssetTransform = (operation) => OPERATIONS.has(operation);
export async function executeAssetTransform(operation, value, options = {}) {
  requireValue(
    value instanceof ArrayBuffer || value instanceof Uint8Array,
    "Invalid bounded asset request",
  );
  const payload = value instanceof Uint8Array ? value : new Uint8Array(value);
  requireValue(
    payload.length <= MAX_INPUT && isObject(options) && JSON.stringify(options).length <= 4096,
    "Invalid bounded asset request",
  );
  switch (operation) {
    case "experience-table":
      return experience(payload);
    case "overlay-paths":
      return overlayPaths(payload);
    case "derived-tables":
      return derived(payload, options);
    case "vault-table":
      return vault(payload, options);
    default:
      throw new Error("Unsupported asset operation");
  }
}
