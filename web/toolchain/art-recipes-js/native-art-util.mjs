// 原生图集移植的 Python JSON、整数、CSV 和字节语义；依据 native_art/generate.py 与有界包装器。
// Python JSON, integer, CSV, and byte semantics for the native-art port, based on native_art/generate.py and its bounded wrapper.
const encoder = new TextEncoder();
export const utf8Decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });

export function requireValue(condition, code) {
  if (!condition) throw new Error(code);
}

export function utf8(text) {
  requireValue(typeof text === "string", "NATIVE_ART_UTF8");
  for (const character of text) {
    const code = character.codePointAt(0);
    requireValue(code < 0xd800 || code > 0xdfff, "NATIVE_ART_UTF8");
  }
  return encoder.encode(text);
}

export const isObject = (value) =>
  value !== null &&
  typeof value === "object" &&
  !Array.isArray(value) &&
  !(value instanceof Number);

export const exactKeys = (value, keys) =>
  isObject(value) &&
  Object.keys(value).length === keys.length &&
  keys.every((key) => Object.hasOwn(value, key));

// Python 区分 JSON 的 1 与 1.0；安全整数保留 Number，其余整数使用 BigInt。
// Python distinguishes JSON 1 from 1.0; safe integers retain Number, and other integers use BigInt.
export class PythonFloat extends Number {}
export const isPythonInteger = (value) =>
  typeof value === "bigint" || (typeof value === "number" && Number.isSafeInteger(value));

export function parsePythonJson(text) {
  requireValue(typeof text === "string", "NATIVE_ART_JSON");
  let position = 0;
  const whitespace = () => {
    while (position < text.length && /[\x20\t\r\n]/.test(text[position])) position++;
  };
  const string = () => {
    const start = position++;
    while (position < text.length) {
      const character = text[position++];
      if (character === '"') return JSON.parse(text.slice(start, position));
      if (character === "\\") position++;
    }
    throw new Error("NATIVE_ART_JSON");
  };
  const value = () => {
    whitespace();
    const character = text[position];
    if (character === '"') return string();
    if (character === "[") {
      position++;
      whitespace();
      const result = [];
      if (text[position] === "]") {
        position++;
        return result;
      }
      for (;;) {
        result.push(value());
        whitespace();
        if (text[position] === "]") {
          position++;
          return result;
        }
        requireValue(text[position++] === ",", "NATIVE_ART_JSON");
      }
    }
    if (character === "{") {
      position++;
      whitespace();
      const result = {};
      if (text[position] === "}") {
        position++;
        return result;
      }
      for (;;) {
        whitespace();
        requireValue(text[position] === '"', "NATIVE_ART_JSON");
        const key = string();
        whitespace();
        requireValue(text[position++] === ":", "NATIVE_ART_JSON");
        Object.defineProperty(result, key, {
          value: value(),
          enumerable: true,
          writable: true,
          configurable: true,
        });
        whitespace();
        if (text[position] === "}") {
          position++;
          return result;
        }
        requireValue(text[position++] === ",", "NATIVE_ART_JSON");
      }
    }
    for (const [token, result] of [
      ["true", true],
      ["false", false],
      ["null", null],
    ]) {
      if (text.startsWith(token, position)) {
        position += token.length;
        return result;
      }
    }
    for (const token of ["NaN", "Infinity", "-Infinity"]) {
      if (text.startsWith(token, position)) {
        position += token.length;
        return new PythonFloat(Number(token));
      }
    }
    const match = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/.exec(
      text.slice(position),
    );
    requireValue(match, "NATIVE_ART_JSON");
    const token = match[0];
    position += token.length;
    if (/[.eE]/.test(token)) return new PythonFloat(Number(token));
    requireValue(token.replace(/^-/, "").length <= 4300, "NATIVE_ART_INTEGER_LIMIT");
    const integer = BigInt(token);
    return integer >= BigInt(Number.MIN_SAFE_INTEGER) && integer <= BigInt(Number.MAX_SAFE_INTEGER)
      ? Number(integer)
      : integer;
  };
  const result = value();
  whitespace();
  requireValue(position === text.length, "NATIVE_ART_JSON");
  return result;
}

// 严格对应 b64decode(validate=True)，但不额外限制未用填充位。
// Match b64decode(validate=True) without adding a restriction on unused padding bits.
export function decodeBase64(text) {
  requireValue(typeof text === "string" && text.length % 4 === 0, "NATIVE_ART_BASE64");
  const padding = text.endsWith("==") ? 2 : text.endsWith("=") ? 1 : 0;
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const end = text.length - padding;
  for (let i = 0; i < end; i++) requireValue(alphabet.includes(text[i]), "NATIVE_ART_BASE64");
  const out = new Uint8Array((text.length / 4) * 3 - padding);
  let written = 0;
  for (let i = 0; i < text.length; i += 4) {
    const packed =
      (alphabet.indexOf(text[i]) << 18) |
      (alphabet.indexOf(text[i + 1]) << 12) |
      (Math.max(0, alphabet.indexOf(text[i + 2])) << 6) |
      Math.max(0, alphabet.indexOf(text[i + 3]));
    if (written < out.length) out[written++] = packed >>> 16;
    if (written < out.length) out[written++] = packed >>> 8;
    if (written < out.length) out[written++] = packed;
  }
  return out;
}

export const pythonStrip = (text) =>
  text.replace(
    // eslint-disable-next-line no-control-regex -- Python str.strip 的空白集合。 | Python str.strip whitespace set.
    /^[\u0009-\u000d\u001c-\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+|[\u0009-\u000d\u001c-\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+$/gu,
    "",
  );

export function pythonInteger(value) {
  requireValue(typeof value === "string", "NATIVE_ART_INTEGER");
  let text = value.replace(
    // eslint-disable-next-line no-control-regex -- int 与 str.strip 对 ASCII 分隔控制符的规则不同。 | int and str.strip differ on ASCII separator controls.
    /^[\u0009-\u000d\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+|[\u0009-\u000d\u0020\u0085\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000]+$/gu,
    "",
  );
  text = text.replace(/\p{Decimal_Number}/gu, (digit) => {
    const code = digit.codePointAt(0);
    let first = code;
    while (first > 0 && /\p{Decimal_Number}/u.test(String.fromCodePoint(first - 1))) first--;
    return String((code - first) % 10);
  });
  requireValue(/^[+-]?[0-9](?:_?[0-9])*$/.test(text), "NATIVE_ART_INTEGER");
  text = text.replaceAll("_", "");
  requireValue(text.replace(/^[+-]/, "").length <= 4300, "NATIVE_ART_INTEGER_LIMIT");
  return BigInt(text);
}

export function pythonNumber(value) {
  if (value instanceof PythonFloat) return value.valueOf();
  if (typeof value === "boolean") return Number(value);
  requireValue(typeof value === "number" || typeof value === "bigint", "NATIVE_ART_NUMBER");
  return value;
}

export function decimal(value) {
  const number = pythonNumber(value);
  if (typeof number === "bigint") return number.toString();
  requireValue(Number.isFinite(number), "NATIVE_ART_INTEGER");
  return BigInt(Math.trunc(number)).toString();
}

// 对应 csv.DictReader(StringIO(text), delimiter='\t')，包括重复列名、空行和跨行引号。
// Match csv.DictReader(StringIO(text), delimiter='\t'), including duplicate headers, blank rows, and multiline quoting.
export function tsvDictRows(text) {
  const records = [];
  let record = [],
    field = "",
    fieldLength = 0,
    quoted = false,
    start = true,
    touched = false;
  const append = (character) => {
    field += character;
    fieldLength++;
    requireValue(fieldLength <= 131072, "NATIVE_ART_CSV_FIELD_LIMIT");
  };
  const finishField = () => {
    record.push(field);
    field = "";
    fieldLength = 0;
    start = true;
  };
  const finishRecord = () => {
    if (touched || record.length || fieldLength) finishField();
    records.push(record);
    record = [];
    touched = false;
    start = true;
  };
  for (let i = 0; i < text.length; i++) {
    let character = text[i];
    if (
      character.charCodeAt(0) >= 0xd800 &&
      character.charCodeAt(0) <= 0xdbff &&
      i + 1 < text.length
    )
      character += text[++i];
    if (quoted) {
      if (character === '"') {
        if (text[i + 1] === '"') {
          append('"');
          i++;
        } else quoted = false;
      } else append(character);
      touched = true;
    } else if (character === '"' && start) {
      quoted = true;
      start = false;
      touched = true;
    } else if (character === "\t") {
      finishField();
      touched = true;
    } else if (character === "\n" || character === "\r") {
      if (character === "\r") {
        while (text[i + 1] === "\r") i++;
        requireValue(text[i + 1] === "\n" || i + 1 === text.length, "NATIVE_ART_CSV_NEWLINE");
        if (text[i + 1] === "\n") i++;
      }
      finishRecord();
    } else {
      append(character);
      start = false;
      touched = true;
    }
  }
  if (touched || record.length || fieldLength) finishRecord();
  const [headers = [], ...rows] = records;
  return rows
    .filter((row) => row.length)
    .map((row) => {
      const result = new Map(headers.map((name, index) => [name, row[index] ?? null]));
      if (row.length > headers.length) result.set(null, row.slice(headers.length));
      return result;
    });
}

export function requiredEntry(mapping, key) {
  requireValue(mapping.has(key), "NATIVE_ART_CSV_COLUMN");
  return mapping.get(key);
}
