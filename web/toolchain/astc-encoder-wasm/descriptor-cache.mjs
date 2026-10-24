// 依据 dm/tools/astc_pack.py 的 desc_cache，转换选定纹理的描述与缩略图。
// Transform selected texture descriptors and thumbnails from dm/tools/astc_pack.py desc_cache.
import { transcodeBC3Batches } from "./bc3-astc-batches.mjs";

export const DESCRIPTOR_CACHE_LIMITS = Object.freeze({
  inputBytes: 16 * 1024 * 1024,
  outputBytes: 32 * 1024 * 1024,
  thumbnailBytes: 4 * 1024 * 1024,
  entries: 100000,
  mips: 64,
  nodes: 500000,
  depth: 64,
});
const invalid = () => {
  throw new Error("ASTC_DESCRIPTOR_INVALID");
};
const abort = (signal) => {
  if (signal?.aborted) throw new DOMException("Cancelled", "AbortError");
};
const ownedView = (bytes) =>
  bytes instanceof Uint8Array && bytes.buffer instanceof ArrayBuffer && !bytes.buffer.resizable;
function metadata(value, depth = 0, budget = { nodes: 0 }) {
  if (++budget.nodes > DESCRIPTOR_CACHE_LIMITS.nodes || depth > DESCRIPTOR_CACHE_LIMITS.depth)
    invalid();
  if (
    typeof value === "number" &&
    (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value)))
  )
    invalid();
  if (value && typeof value === "object")
    for (const child of Object.values(value)) metadata(child, depth + 1, budget);
}
function decodeThumbnail(value) {
  if (
    typeof value !== "string" ||
    value.length > 4 * Math.ceil(DESCRIPTOR_CACHE_LIMITS.thumbnailBytes / 3) ||
    value.length % 4 !== 0 ||
    /[^A-Za-z0-9+/=]/.test(value)
  )
    invalid();
  const raw = atob(value);
  if (raw.length > DESCRIPTOR_CACHE_LIMITS.thumbnailBytes || btoa(raw) !== value) invalid();
  return Uint8Array.from(raw, (c) => c.charCodeAt(0));
}
function encodeThumbnail(bytes) {
  const chunks = [];
  for (let offset = 0; offset < bytes.length; offset += 32768)
    chunks.push(String.fromCharCode(...bytes.subarray(offset, offset + 32768)));
  return btoa(chunks.join(""));
}
function regions(bytes) {
  if (bytes.length < 8) invalid();
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const count = view.getUint32(0, true),
    table = 4 + view.getInt32(4, true),
    end = table + count * 8;
  if (!count || count > DESCRIPTOR_CACHE_LIMITS.mips || table < 8 || end > bytes.length) invalid();
  const result = [];
  for (let i = 0; i < count; i++) {
    const row = table + i * 8,
      size = view.getUint32(row, true),
      field = row + 4;
    const start = field + view.getInt32(field, true);
    if (!size || size % 16 || start < end || size > bytes.length - start) invalid();
    result.push({ start, size });
  }
  // 源脚本不检查重叠；这里拒绝别名，防止同一范围被不同级重复覆盖。
  // Native does not check overlap; reject aliases so different mips cannot overwrite the same range.
  const ordered = [...result].sort((a, b) => a.start - b.start);
  for (let i = 1; i < ordered.length; i++)
    if (ordered[i - 1].start + ordered[i - 1].size > ordered[i].start) invalid();
  return result;
}

/**
 * converted 是小写逻辑名称到布尔 sRGB 标志的 Map，不是路径读取或身份认证。
 * 只返回完整成功的有界 JSON；保留未选记录与缩略图非块字节，不保持 JSON 空白。
 * converted is a Map of lowercase logical names to boolean sRGB flags, not file I/O or authentication.
 * Return bounded JSON only on complete success; preserve unselected records and non-block thumbnail bytes, not JSON whitespace.
 */
export async function transformTextureDescriptorCache(
  input,
  converted,
  { signal, batchBlocks, encode } = {},
) {
  try {
    if (signal != null && !(signal instanceof AbortSignal)) invalid();
    abort(signal);
    if (
      !ownedView(input) ||
      !input.length ||
      input.length > DESCRIPTOR_CACHE_LIMITS.inputBytes ||
      !(converted instanceof Map) ||
      converted.size > DESCRIPTOR_CACHE_LIMITS.entries
    )
      invalid();
    const selection = new Map();
    for (const [name, srgb] of converted) {
      if (
        typeof name !== "string" ||
        !name.length ||
        name.length > 1024 ||
        !/^[\x20-\x7e]+$/.test(name) ||
        name !== name.toLowerCase() ||
        typeof srgb !== "boolean"
      )
        invalid();
      selection.set(name, srgb);
    }
    const records = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(input));
    if (!Array.isArray(records) || records.length > DESCRIPTOR_CACHE_LIMITS.entries) invalid();
    metadata(records);
    const jobs = [];
    // 先验证全部描述，再调用编码器；不同时保存多张解码后的缩略图。
    // Validate every descriptor before codec calls; never retain multiple decoded thumbnails.
    for (const record of records) {
      abort(signal);
      if (
        !record ||
        typeof record !== "object" ||
        Array.isArray(record) ||
        typeof record.filename !== "string" ||
        record.filename.length > 1024
      )
        invalid();
      const name = record.filename.toLowerCase();
      if (selection.has(name) && !Object.hasOwn(record, "format")) invalid();
      if (!selection.has(name) || ![61, 62].includes(record.format)) continue;
      const srgb = selection.get(name);
      if ((record.format === 62) !== srgb) invalid();
      const thumbnail = record.thumbnailBase64;
      if (thumbnail !== undefined && thumbnail !== null && typeof thumbnail !== "string") invalid();
      if (thumbnail) regions(decodeThumbnail(thumbnail));
      jobs.push({ record, srgb });
    }
    let thumbnailCount = 0;
    for (const { record, srgb } of jobs) {
      abort(signal);
      if (record.thumbnailBase64) {
        const bytes = decodeThumbnail(record.thumbnailBase64);
        for (const { start, size } of regions(bytes)) {
          const source = {
            size,
            readAt: (offset, length) => bytes.subarray(start + offset, start + offset + length),
          };
          try {
            for await (const batch of transcodeBC3Batches(source, {
              profile: srgb ? "srgb" : "linear",
              signal,
              batchBlocks,
              encode,
            })) {
              abort(signal);
              bytes.set(batch.blocks, start + batch.blockOffset * 16);
            }
          } catch {
            abort(signal);
            throw new Error("ASTC_DESCRIPTOR_TRANSCODE");
          }
        }
        record.thumbnailBase64 = encodeThumbnail(bytes);
        thumbnailCount++;
      }
      record.format = srgb ? 81 : 95;
    }
    abort(signal);
    const bytes = new TextEncoder().encode(JSON.stringify(records));
    if (bytes.length > DESCRIPTOR_CACHE_LIMITS.outputBytes) invalid();
    return { bytes, convertedEntries: jobs.length, thumbnailCount };
  } catch (error) {
    abort(signal instanceof AbortSignal ? signal : undefined);
    if (error?.message === "ASTC_DESCRIPTOR_TRANSCODE")
      throw new Error("ASTC_DESCRIPTOR_TRANSCODE");
    throw new Error("ASTC_DESCRIPTOR_INVALID");
  }
}
