// 有界 Info.plist 读写；仅支持标准 plist 类型，不接受实体声明、重复键或循环。
// Bounded Info.plist codec: standard types only, no entity declarations, duplicate keys or cycles.
export const CUSTOM_PLIST_BYTES = 1024 * 1024;
const encoder = new TextEncoder();
const scalar = Symbol("plist scalar");
const fail = () => {
  throw new Error("APP_CUSTOMIZATION_PLIST_INVALID");
};
const requireValue = (ok) => {
  if (!ok) fail();
};
const typed = (kind, value) => ({ [scalar]: kind, value });
const dict = () => Object.create(null);
const validText = (value) =>
  // eslint-disable-next-line no-control-regex -- XML forbids these code points.
  !/[\u0000-\u0008\u000b\u000c\u000e-\u001f\ufffe\uffff]/u.test(value) && value.isWellFormed();
const escape = (value) => {
  requireValue(value.length <= CUSTOM_PLIST_BYTES && validText(value));
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&apos;")
    .replace(/\r/g, "&#13;");
};
function budget() {
  let nodes = 0;
  return (depth) => requireValue(depth <= 32 && ++nodes <= 4096);
}
function binary(bytes) {
  requireValue(bytes.length >= 40);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length),
    trailer = bytes.length - 32;
  const uint = (offset, width) => {
    requireValue([1, 2, 4, 8].includes(width) && offset >= 0 && offset + width <= bytes.length);
    let value = 0;
    for (let i = 0; i < width; i++) value = value * 256 + bytes[offset + i];
    requireValue(Number.isSafeInteger(value));
    return value;
  };
  const offsetSize = bytes[trailer + 6],
    refSize = bytes[trailer + 7];
  requireValue([1, 2, 4, 8].includes(offsetSize) && [1, 2, 4, 8].includes(refSize));
  const count = uint(trailer + 8, 8),
    root = uint(trailer + 16, 8),
    table = uint(trailer + 24, 8);
  requireValue(
    count > 0 &&
      count <= 4096 &&
      root < count &&
      table >= 8 &&
      table + count * offsetSize === trailer,
  );
  const offsets = Array.from({ length: count }, (_, i) => uint(table + i * offsetSize, offsetSize));
  requireValue(new Set(offsets).size === count && offsets.every((n) => n >= 8 && n < table));
  const sorted = [...offsets].sort((a, b) => a - b),
    ends = new Map(sorted.map((n, i) => [n, sorted[i + 1] ?? table]));
  const active = new Set(),
    check = budget();
  let textBytes = 0;
  const reserve = (size) => {
    textBytes += size;
    requireValue(Number.isSafeInteger(textBytes) && textBytes <= CUSTOM_PLIST_BYTES);
  };
  function read(index, depth = 0) {
    check(depth);
    requireValue(index < count && !active.has(index));
    active.add(index);
    try {
      let at = offsets[index];
      const end = ends.get(at),
        tag = bytes[at++],
        kind = tag >> 4;
      let length = tag & 15;
      if ([4, 5, 6, 7, 10, 13].includes(kind) && length === 15) {
        requireValue(at < end && bytes[at] >> 4 === 1 && (bytes[at] & 15) <= 3);
        const width = 2 ** (bytes[at++] & 15);
        requireValue(at + width <= end);
        length = uint(at, width);
        at += width;
      }
      const span = (n) => {
        requireValue(Number.isSafeInteger(n) && n >= 0 && at + n <= end);
        return bytes.subarray(at, at + n);
      };
      if (kind === 0) {
        requireValue(tag === 8 || tag === 9);
        return tag === 9;
      }
      if (kind === 1) {
        const width = 2 ** length;
        span(width);
        requireValue(width <= 8);
        let value = 0n;
        for (const b of span(width)) value = value * 256n + BigInt(b);
        if (width === 8 && bytes[at] & 128) value -= 1n << 64n;
        return typed("integer", value.toString());
      }
      if (kind === 2 || kind === 3) {
        const width = 2 ** length;
        span(width);
        requireValue(kind === 3 ? tag === 0x33 : width === 4 || width === 8);
        const value = width === 4 ? view.getFloat32(at) : view.getFloat64(at);
        requireValue(Number.isFinite(value));
        if (kind === 2) return typed("real", Object.is(value, -0) ? "-0" : String(value));
        requireValue(Number.isInteger(value));
        const date = new Date(978307200000 + value * 1000);
        requireValue(Number.isFinite(date.getTime()));
        const iso = date.toISOString().replace(".000Z", "Z");
        requireValue(/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(iso));
        return typed("date", iso);
      }
      if (kind === 4) {
        reserve(Math.ceil(length / 3) * 4);
        let value = "";
        for (const b of span(length)) value += String.fromCharCode(b);
        return typed("data", btoa(value));
      }
      if ([5, 6, 7].includes(kind)) {
        reserve(length * (kind === 6 ? 2 : 1));
        const data = span(length * (kind === 6 ? 2 : 1));
        requireValue(kind !== 5 || data.every((b) => b < 128));
        const value = new TextDecoder(kind === 6 ? "utf-16be" : "utf-8", {
          fatal: true,
          ignoreBOM: true,
        }).decode(data);
        requireValue(validText(value));
        return value;
      }
      if (kind === 10 || kind === 13) {
        requireValue(length <= 4096);
        span(length * refSize * (kind === 13 ? 2 : 1));
        const ref = (i) => uint(at + i * refSize, refSize);
        if (kind === 10) return Array.from({ length }, (_, i) => read(ref(i), depth + 1));
        const value = dict();
        for (let i = 0; i < length; i++) {
          const key = read(ref(i), depth + 1);
          requireValue(typeof key === "string" && !Object.hasOwn(value, key));
          value[key] = read(ref(length + i), depth + 1);
        }
        return value;
      }
      fail();
    } finally {
      active.delete(index);
    }
  }
  return read(root);
}
function xml(bytes) {
  let text = new TextDecoder("utf-8", { fatal: true }).decode(bytes).replace(/\r\n?/g, "\n").trim();
  text = text.replace(/^<\?xml version=["']1\.0["'](?: encoding=["']UTF-8["'])?\s*\?>\s*/i, "");
  text = text.replace(
    /^<!DOCTYPE plist PUBLIC "-\/\/Apple\/\/DTD PLIST 1\.0\/\/EN" "http:\/\/www\.apple\.com\/DTDs\/PropertyList-1\.0\.dtd">\s*/,
    "",
  );
  requireValue(!text.includes("<!") && !text.includes("<?"));
  const open = /^<plist version=["']1\.0["']>/.exec(text);
  requireValue(open);
  let at = open[0].length;
  const check = budget();
  const whitespace = () => {
    while (/\s/.test(text[at] ?? "") && at < text.length) at++;
  };
  const take = (token) => {
    whitespace();
    requireValue(text.startsWith(token, at));
    at += token.length;
  };
  const unescape = (value) => {
    // 先拒绝裸 &，避免不匹配分号的重复扫描产生二次复杂度。
    // Reject bare ampersands before substitution to avoid quadratic missing-semicolon scans.
    requireValue(
      !value.includes("<") && !/&(?!(?:amp|lt|gt|quot|apos|#\d+|#x[\da-fA-F]+);)/.test(value),
    );
    const output = value.replace(/&([^;]*);/g, (_all, entity) => {
      const named = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'" };
      if (Object.hasOwn(named, entity)) return named[entity];
      requireValue(/^#(?:[0-9]+|x[0-9a-fA-F]+)$/.test(entity));
      const n = entity[1] === "x" ? parseInt(entity.slice(2), 16) : Number(entity.slice(1));
      requireValue(
        Number.isSafeInteger(n) && n >= 0 && n <= 0x10ffff && !(n >= 0xd800 && n <= 0xdfff),
      );
      return String.fromCodePoint(n);
    });
    requireValue(validText(output));
    return output;
  };
  function read(depth = 0, keyOnly = false) {
    check(depth);
    whitespace();
    const match = /^<(dict|array|string|key|integer|real|data|date|true|false)\s*(\/?)>/.exec(
      text.slice(at),
    );
    requireValue(match);
    at += match[0].length;
    const kind = match[1],
      empty = match[2] === "/";
    requireValue(keyOnly === (kind === "key"));
    if (["true", "false"].includes(kind)) {
      requireValue(empty);
      return kind === "true";
    }
    if (kind === "dict" || kind === "array") {
      const out = kind === "dict" ? dict() : [];
      if (empty) return out;
      while (true) {
        whitespace();
        if (text.startsWith(`</${kind}>`, at)) break;
        if (kind === "array") out.push(read(depth + 1));
        else {
          const key = read(depth + 1, true);
          requireValue(!Object.hasOwn(out, key));
          out[key] = read(depth + 1);
        }
      }
      take(`</${kind}>`);
      return out;
    }
    let value = "";
    if (!empty) {
      const end = text.indexOf(`</${kind}>`, at);
      requireValue(end >= at);
      value = text.slice(at, end);
      at = end + kind.length + 3;
    }
    if (kind === "string" || kind === "key") return unescape(value);
    value = value.trim();
    if (kind === "integer") requireValue(/^-?\d{1,20}$/.test(value));
    else if (kind === "real")
      requireValue(
        /^-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$/.test(value) &&
          Number.isFinite(Number(value)),
      );
    else if (kind === "date")
      requireValue(
        /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(value) &&
          new Date(value).toISOString().replace(".000Z", "Z") === value,
      );
    else if (kind === "data") {
      value = value.replace(/\s/g, "");
      requireValue(/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value));
    }
    return typed(kind, value);
  }
  const value = read();
  take("</plist>");
  whitespace();
  requireValue(at === text.length);
  return value;
}
export function readCustomizationPlist(bytes) {
  try {
    requireValue(
      bytes instanceof Uint8Array && bytes.length > 0 && bytes.length <= CUSTOM_PLIST_BYTES,
    );
    const value =
      new TextDecoder().decode(bytes.subarray(0, 8)) === "bplist00" ? binary(bytes) : xml(bytes);
    requireValue(value && Object.getPrototypeOf(value) === null);
    return value;
  } catch {
    fail();
  }
}
export function writeCustomizationPlist(value) {
  const check = budget(),
    parts = [];
  let length = 0;
  const emit = (text) => {
    length += text.length;
    requireValue(length <= CUSTOM_PLIST_BYTES);
    parts.push(text);
  };
  function write(node, depth = 0) {
    check(depth);
    if (typeof node === "string") return emit(`<string>${escape(node)}</string>`);
    if (typeof node === "boolean") return emit(node ? "<true/>" : "<false/>");
    if (node?.[scalar]) return emit(`<${node[scalar]}>${escape(node.value)}</${node[scalar]}>`);
    if (Array.isArray(node)) {
      emit("<array>");
      for (const entry of node) write(entry, depth + 1);
      emit("</array>");
      return;
    }
    requireValue(node && Object.getPrototypeOf(node) === null);
    emit("<dict>");
    for (const [key, entry] of Object.entries(node)) {
      emit(`<key>${escape(key)}</key>`);
      write(entry, depth + 1);
    }
    emit("</dict>");
  }
  emit(
    '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">',
  );
  write(value);
  emit("</plist>\n");
  const bytes = encoder.encode(parts.join(""));
  requireValue(bytes.length <= CUSTOM_PLIST_BYTES);
  return bytes;
}
