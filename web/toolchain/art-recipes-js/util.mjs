// 有界配方的字节与元数据检查；基于现有 Python 适配器。 | Bounded recipe byte/metadata checks, based on the existing Python adapters.
import { decodeBase64 } from "./native-art-util.mjs";
export const encoder = new TextEncoder();
export const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
export function requireValue(value, code) {
  if (!value) throw new Error(code);
}
export const object = (value) =>
  value !== null && typeof value === "object" && !Array.isArray(value);
export const exactKeys = (value, keys) =>
  object(value) &&
  Object.keys(value).length === keys.length &&
  keys.every((key) => Object.hasOwn(value, key));
export const positiveInteger = (n) => Number.isSafeInteger(n) && n > 0;
export function utf8(text) {
  requireValue(typeof text === "string", "ART_UTF8");
  // 逐个码点验证，不为大型清单创建逐字符数组。
  // Validate code points without allocating a character array for large manifests.
  for (const char of text) {
    const n = char.codePointAt(0);
    requireValue(n < 0xd800 || n > 0xdfff, "ART_UTF8");
  }
  return encoder.encode(text);
}
export function payloadBytes(payload, limit, code) {
  requireValue(payload instanceof Uint8Array && payload.length <= limit, code);
  return payload;
}
export function base64(text, code) {
  try {
    return decodeBase64(text);
  } catch {
    throw new Error(code);
  }
}
export function png(codec, im) {
  const result = codec.encodeRgba(im.data, im.width, im.height);
  requireValue(result instanceof Uint8Array, "ART_PNG_BYTES");
  return result;
}
export const backend = Object.freeze({ pillow: "10.2.0", freetype: "2.6.0", raqm: false });
