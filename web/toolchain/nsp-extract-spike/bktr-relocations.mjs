// 依据旧版 hactool BKTR 表布局解析重定位并规划有界读取。
// Parse relocations and plan bounded reads based on the legacy hactool BKTR table layout.

// 旧版 hactool BKTR 重定位表边界，并非解密入口。
// 在调用方验证合并后的 IVFC 层级之前，表字节不可信。
// Legacy hactool BKTR relocation-table boundary, not a decryption entrypoint.
// Table bytes are untrusted until the caller verifies the merged IVFC hierarchy.
const NODE = 0x4000;
const MAX_TABLE = 16 * 1024 * 1024;
const MAX_READ = 65536;
const invalid = () => {
  throw new Error("Unsupported or malformed BKTR relocation table");
};
export function parseRelocations(bytes, { entryCount, baseSize, patchDataSize }) {
  if (
    !(bytes instanceof Uint8Array) ||
    bytes.length < NODE * 2 ||
    bytes.length > MAX_TABLE ||
    bytes.length % NODE ||
    !Number.isSafeInteger(entryCount) ||
    entryCount < 1 ||
    typeof baseSize !== "bigint" ||
    baseSize < 0n ||
    typeof patchDataSize !== "bigint" ||
    patchDataSize < 0n
  )
    invalid();
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const buckets = v.getUint32(4, true),
    size = v.getBigUint64(8, true);
  if (!size || !buckets || buckets > 0x3ff0 / 8 || bytes.length !== (buckets + 1) * NODE) invalid();
  const entries = [];
  for (let bucket = 0; bucket < buckets; bucket++) {
    const start = v.getBigUint64(16 + bucket * 8, true);
    const end = bucket + 1 < buckets ? v.getBigUint64(24 + bucket * 8, true) : size;
    const at = (bucket + 1) * NODE,
      count = v.getUint32(at + 4, true);
    if (
      !count ||
      count > Math.floor(0x3ff0 / 20) ||
      start >= end ||
      end > size ||
      (bucket === 0 && start !== 0n) ||
      v.getBigUint64(at + 8, true) !== end
    )
      invalid();
    for (let i = 0; i < count; i++) {
      const pos = at + 16 + i * 20;
      const offset = v.getBigUint64(pos, true),
        physical = v.getBigUint64(pos + 8, true);
      const source = v.getUint32(pos + 16, true);
      const next = i + 1 < count ? v.getBigUint64(pos + 20, true) : end;
      if (
        (i === 0 && offset !== start) ||
        offset < start ||
        offset >= next ||
        next > end ||
        source > 1
      )
        invalid();
      const limit = source ? patchDataSize : baseSize;
      if (physical > limit || next - offset > limit - physical) invalid();
      entries.push(
        Object.freeze({ offset, physical, size: next - offset, source: source ? "patch" : "base" }),
      );
      if (entries.length > entryCount) invalid();
    }
  }
  if (entries.length !== entryCount) invalid();
  return Object.freeze({ size, entries: Object.freeze(entries) });
}

// 仅规划有界读取。偏移相对于分区，而非最终 RomFS 层级。
// 不暴露原始输出，也不声称已验证真实性。
// Pure bounded read planning. Offsets are section-relative, NOT relative to the
// final RomFS level. No raw output is exposed and no authenticity is asserted.
export function planRelocationRead(table, offset, count) {
  if (
    typeof offset !== "bigint" ||
    offset < 0n ||
    offset > table.size ||
    !Number.isInteger(count) ||
    count < 0 ||
    count > MAX_READ ||
    BigInt(count) > table.size - offset
  )
    invalid();
  const plans = [];
  let remaining = BigInt(count);
  let low = 0,
    high = table.entries.length;
  while (low < high) {
    const middle = Math.floor((low + high) / 2);
    if (table.entries[middle].offset <= offset) low = middle + 1;
    else high = middle;
  }
  for (let i = Math.max(0, low - 1); remaining; i++) {
    const entry = table.entries[i];
    if (!entry || offset < entry.offset || offset >= entry.offset + entry.size) invalid();
    const available = entry.size - (offset - entry.offset);
    const take = available < remaining ? available : remaining;
    plans.push(
      Object.freeze({
        source: entry.source,
        offset: entry.physical + offset - entry.offset,
        count: Number(take),
      }),
    );
    offset += take;
    remaining -= take;
  }
  return Object.freeze(plans);
}
