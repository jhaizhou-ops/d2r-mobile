// 固定版本 HUD 配方所需的有限 Python 值与 JSON 语义。
// Narrow Python value/JSON semantics needed by the pinned HUD recipe.
const ORDER = Symbol("Python insertion order");
export class PyFloat extends Number {}
export const float = (n) => new PyFloat(n);
export function ordered(entries = []) {
  const obj = Object.create(null);
  Object.defineProperty(obj, ORDER, { value: [] });
  for (const [key, value] of entries) put(obj, key, value);
  return obj;
}
export function put(obj, key, value) {
  if (obj[ORDER] && !Object.hasOwn(obj, key)) {
    const previous = obj[ORDER].indexOf(key);
    if (previous >= 0) obj[ORDER].splice(previous, 1);
    obj[ORDER].push(key);
  }
  Object.defineProperty(obj, key, { value, writable: true, enumerable: true, configurable: true });
  return value;
}
export function keys(obj) {
  return obj[ORDER] ? obj[ORDER].filter((key) => Object.hasOwn(obj, key)) : Object.keys(obj);
}
export function roundEven(value, digits = 0) {
  value = Number(value);
  if (!Number.isFinite(value) || !Number.isInteger(digits) || digits < 0 || digits > 1)
    throw new RangeError("Bounded Python round");
  if (!value) return digits ? value : 0;
  // 对精确的 binary64 有理数舍入，而非 value*10（后者会再次舍入）。
  // Round the exact binary64 rational, not value*10 (which rounds a second time).
  const bits = new DataView(new ArrayBuffer(8));
  bits.setFloat64(0, Math.abs(value));
  const raw = bits.getBigUint64(0),
    exponent = Number((raw >> 52n) & 2047n);
  let numerator = (raw & ((1n << 52n) - 1n)) | (exponent ? 1n << 52n : 0n);
  const power = (exponent || 1) - 1023 - 52;
  let denominator = 1n;
  if (power >= 0) numerator <<= BigInt(power);
  else denominator <<= BigInt(-power);
  const scale = 10n ** BigInt(digits);
  numerator *= scale;
  let result = numerator / denominator;
  const remainder = numerator % denominator;
  if (remainder * 2n > denominator || (remainder * 2n === denominator && result & 1n)) result++;
  return result === 0n && digits === 0
    ? 0
    : ((value < 0 ? -1 : 1) * Number(result)) / Number(scale);
}
function floatText(value) {
  const n = Number(value);
  if (!Number.isFinite(n)) throw new RangeError("Nonfinite HUD JSON");
  if (Object.is(n, -0)) return "-0.0";
  if (n === 0) return "0.0";
  const magnitude = Math.abs(n);
  let text = magnitude < 0.0001 || magnitude >= 1e16 ? n.toExponential() : n.toString();
  if (text.includes("e")) {
    const [mantissa, exponent] = text.split("e"),
      e = Number(exponent);
    text = mantissa + "e" + (e < 0 ? "-" : "+") + Math.abs(e).toString().padStart(2, "0");
  } else if (!text.includes(".")) text += ".0";
  return text;
}
export function stringifyPython(value, indent = 1) {
  function visit(v, depth) {
    if (depth > 128) throw new RangeError("HUD JSON nesting");
    if (v instanceof PyFloat) return floatText(v);
    if (typeof v === "number") {
      if (!Number.isSafeInteger(v)) return floatText(v);
      return String(v);
    }
    if (typeof v === "bigint") return String(v);
    if (v === null || typeof v === "boolean" || typeof v === "string") return JSON.stringify(v);
    const array = Array.isArray(v);
    if (!array && (!v || typeof v !== "object")) throw new TypeError("HUD JSON value");
    const parts = array
      ? v.map((item) => visit(item, depth + 1))
      : keys(v).map((key) => JSON.stringify(key) + ": " + visit(v[key], depth + 1));
    if (!parts.length) return array ? "[]" : "{}";
    return (
      (array ? "[" : "{") +
      "\n" +
      " ".repeat((depth + 1) * indent) +
      parts.join(",\n" + " ".repeat((depth + 1) * indent)) +
      "\n" +
      " ".repeat(depth * indent) +
      (array ? "]" : "}")
    );
  }
  return visit(value, 0);
}
// 解析游戏布局与结构补丁时，保留浮点数与整数的类型差异及对象插入顺序。
// JSON.parse 会丢失这两项区别。
// Preserve float-vs-int type and object insertion order when parsing game layout
// and the structural patch. JSON.parse loses both distinctions.
export function parsePython(text) {
  let at = 0;
  function space() {
    while (at < text.length && /[\t\n\r ]/.test(text[at])) at++;
  }
  function parse(depth = 0) {
    if (depth > 128) throw new RangeError("HUD JSON nesting");
    space();
    const start = at,
      token = text[at++];
    if (token === '"') {
      while (at < text.length) {
        const c = text[at++];
        if (c === "\\") at++;
        else if (c === '"') return JSON.parse(text.slice(start, at));
      }
      throw new SyntaxError("HUD JSON string");
    }
    if (token === "{" || token === "[") {
      const arr = token === "[",
        end = arr ? "]" : "}",
        result = arr ? [] : ordered();
      space();
      if (text[at] === end) {
        at++;
        return result;
      }
      for (;;) {
        let key;
        if (!arr) {
          key = parse(depth + 1);
          if (typeof key !== "string") throw new SyntaxError("HUD JSON key");
          space();
          if (text[at++] !== ":") throw new SyntaxError("HUD JSON colon");
        }
        const value = parse(depth + 1);
        if (arr) result.push(value);
        else put(result, key, value);
        space();
        const c = text[at++];
        if (c === end) return result;
        if (c !== ",") throw new SyntaxError("HUD JSON separator");
      }
    }
    at = start;
    for (const [word, value] of [
      ["null", null],
      ["true", true],
      ["false", false],
    ])
      if (text.startsWith(word, at)) {
        at += word.length;
        return value;
      }
    const match = /-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/.exec(text.slice(at));
    if (!match || match.index !== 0) throw new SyntaxError("HUD JSON number");
    at += match[0].length;
    const number = Number(match[0]);
    if (!Number.isFinite(number)) throw new RangeError("HUD JSON number range");
    if (/[.eE]/.test(match[0])) return float(number);
    return Number.isSafeInteger(number) ? number : BigInt(match[0]);
  }
  const value = parse();
  space();
  if (at !== text.length) throw new SyntaxError("HUD JSON trailing input");
  return value;
}
