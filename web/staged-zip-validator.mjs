// 独立验证暂存 STORE ZIP32/ZIP64 的完整性和布局。
// 依据本应用的规范 STORE ZIP 布局及已验证的清单。
// Independently validates staged STORE ZIP32/ZIP64 integrity and layout.
// Based on this application’s canonical STORE ZIP layout and validated manifests.

import { indexFiles, readRange, ZIP_PATH_LIMITS } from "./preflight.mjs";

/**
 * 严格验证本应用的规范 STORE ZIP32/ZIP64 方言。
 * 不导入写入器，也不接受其偏移、大小、CRC 或有效标志。
 * 每个归档字节都属于已检查的头、流式载荷或已检查的尾部。
 * 这是完整性/布局验证，而非真实性或可执行文件验证。
 * Strict validator for this application's canonical STORE ZIP32/ZIP64 dialect.
 * Does not import the writers or accept their offsets, sizes, CRCs or valid flags.
 * Every archive byte belongs to a checked header, streamed payload or checked trailer.
 * This is integrity/layout validation, not authenticity or executable validation.
 */
export const STAGED_ZIP_LIMITS = Object.freeze({
  ...ZIP_PATH_LIMITS,
  maxEntries: 20_000,
  maxPathBytes: 4096,
  maxMetadataBytes: 8 * 1024 * 1024,
  maxChunkBytes: 4 * 1024 * 1024,
});
const fail = () => {
  throw new Error("STAGED_ZIP_INVALID");
};
const requireValue = (value) => {
  if (!value) fail();
};
const encoder = new TextEncoder();
const crcTable = Uint32Array.from({ length: 256 }, (_, value) => {
  for (let bit = 0; bit < 8; bit++) value = value & 1 ? (value >>> 1) ^ 0xedb88320 : value >>> 1;
  return value >>> 0;
});
function safe(value) {
  requireValue(
    typeof value === "bigint" && value >= 0n && value <= BigInt(Number.MAX_SAFE_INTEGER),
  );
  return Number(value);
}
function byteOrder(a, b) {
  for (let i = 0; i < Math.min(a.name.length, b.name.length); i++) {
    if (a.name[i] !== b.name[i]) return a.name[i] - b.name[i];
  }
  return a.name.length - b.name.length;
}

/**
 * 在磁盘写入前，仅从已验证的清单推导精确字节预算。
 * Exact byte budget derived solely from a validated manifest, before disk writes.
 */
export function planStagedZip(manifest, { archiveFormat, maxOutputBytes } = {}) {
  requireValue(["zip32", "zip64"].includes(archiveFormat));
  requireValue(Number.isSafeInteger(maxOutputBytes) && maxOutputBytes > 0);
  requireValue(Array.isArray(manifest) && manifest.length <= STAGED_ZIP_LIMITS.maxEntries);
  const zip64 = archiveFormat === "zip64";
  let metadataBytes = zip64 ? 98 : 22;
  let offset = 0n;
  const entries = manifest
    .map(({ path, size, mode }) => {
      requireValue(typeof path === "string" && path.length <= STAGED_ZIP_LIMITS.maxPathBytes);
      const name = encoder.encode(path);
      requireValue(new TextDecoder("utf-8", { fatal: true }).decode(name) === path);
      requireValue(name.length <= STAGED_ZIP_LIMITS.maxPathBytes);
      requireValue([0o644, 0o755, 0o100644, 0o100755].includes(mode));
      requireValue(Number.isSafeInteger(size) && size >= 0 && (zip64 || size < 0xffffffff));
      metadataBytes += (zip64 ? 148 : 92) + name.length * 2;
      requireValue(metadataBytes <= STAGED_ZIP_LIMITS.maxMetadataBytes);
      return { path, name, size, mode: mode & 0o777 };
    })
    .sort(byteOrder);
  indexFiles(entries, STAGED_ZIP_LIMITS);
  for (const entry of entries) {
    entry.offset = safe(offset);
    offset +=
      BigInt((zip64 ? 50 : 30) + entry.name.length) + BigInt(entry.size) + BigInt(zip64 ? 24 : 16);
  }
  const centralOffset = safe(offset);
  const centralSize = entries.reduce(
    (sum, entry) => sum + (zip64 ? 74 : 46) + entry.name.length,
    0,
  );
  const totalBytes = safe(offset + BigInt(centralSize) + BigInt(zip64 ? 98 : 22));
  requireValue(totalBytes <= maxOutputBytes && (zip64 || totalBytes < 0xffffffff));
  return { entries, archiveFormat, centralOffset, centralSize, totalBytes, metadataBytes };
}

/**
 * 只读有界随机访问源：{ size, read(offset,length) }。
 * Read-only bounded random access source: { size, read(offset,length) }.
 */
export async function validateStagedZip(source, manifest, options) {
  const plan = planStagedZip(manifest, options);
  const { signal } = options;
  const zip64 = plan.archiveFormat === "zip64";
  requireValue(source.size === plan.totalBytes);
  const record = async (offset, length) => {
    const bytes = await readRange(source, offset, length, signal);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return {
      bytes,
      u16: (p) => view.getUint16(p, true),
      u32: (p) => view.getUint32(p, true),
      u64: (p) => safe(view.getBigUint64(p, true)),
      zero: (start, end) => bytes.subarray(start, end).every((byte) => byte === 0),
      name: (start, expected) => expected.every((byte, i) => bytes[start + i] === byte),
    };
  };
  let central = plan.centralOffset;
  for (const entry of plan.entries) {
    signal?.throwIfAborted();
    const localSize = (zip64 ? 50 : 30) + entry.name.length;
    const local = await record(entry.offset, localSize);
    requireValue(local.u32(0) === 0x04034b50 && local.u16(4) === (zip64 ? 45 : 20));
    requireValue(
      local.u16(6) === 0x808 && local.zero(8, 12) && local.u16(12) === 0x21 && local.zero(14, 18),
    );
    requireValue(
      local.u32(18) === (zip64 ? 0xffffffff : 0) && local.u32(22) === (zip64 ? 0xffffffff : 0),
    );
    requireValue(
      local.u16(26) === entry.name.length &&
        local.u16(28) === (zip64 ? 20 : 0) &&
        local.name(30, entry.name),
    );
    if (zip64) {
      const extra = 30 + entry.name.length;
      requireValue(
        local.u16(extra) === 1 && local.u16(extra + 2) === 16 && local.zero(extra + 4, extra + 20),
      );
    }
    let crc = 0xffffffff;
    for (let consumed = 0; consumed < entry.size;) {
      const length = Math.min(STAGED_ZIP_LIMITS.maxChunkBytes, entry.size - consumed);
      const bytes = await readRange(source, entry.offset + localSize + consumed, length, signal);
      for (let i = 0; i < bytes.length; i++) crc = (crc >>> 8) ^ crcTable[(crc ^ bytes[i]) & 255];
      consumed += length;
    }
    crc = (crc ^ 0xffffffff) >>> 0;
    const descriptor = await record(entry.offset + localSize + entry.size, zip64 ? 24 : 16);
    requireValue(descriptor.u32(0) === 0x08074b50 && descriptor.u32(4) === crc);
    requireValue((zip64 ? descriptor.u64(8) : descriptor.u32(8)) === entry.size);
    requireValue((zip64 ? descriptor.u64(16) : descriptor.u32(12)) === entry.size);
    const centralLength = (zip64 ? 74 : 46) + entry.name.length;
    const cd = await record(central, centralLength);
    requireValue(cd.u32(0) === 0x02014b50 && cd.u16(4) === (zip64 ? 0x32d : 0x314));
    requireValue(
      cd.u16(6) === (zip64 ? 45 : 20) &&
        cd.u16(8) === 0x808 &&
        cd.zero(10, 14) &&
        cd.u16(14) === 0x21,
    );
    requireValue(
      cd.u32(16) === crc &&
        cd.u32(20) === (zip64 ? 0xffffffff : entry.size) &&
        cd.u32(24) === (zip64 ? 0xffffffff : entry.size),
    );
    requireValue(
      cd.u16(28) === entry.name.length && cd.u16(30) === (zip64 ? 28 : 0) && cd.zero(32, 38),
    );
    requireValue(
      cd.u32(38) === (0o100000 | entry.mode) * 65536 &&
        cd.u32(42) === (zip64 ? 0xffffffff : entry.offset) &&
        cd.name(46, entry.name),
    );
    if (zip64) {
      const extra = 46 + entry.name.length;
      requireValue(
        cd.u16(extra) === 1 &&
          cd.u16(extra + 2) === 24 &&
          cd.u64(extra + 4) === entry.size &&
          cd.u64(extra + 12) === entry.size &&
          cd.u64(extra + 20) === entry.offset,
      );
    }
    central += centralLength;
  }
  const end = await record(central, zip64 ? 98 : 22);
  let eocd = 0;
  if (zip64) {
    requireValue(
      end.u32(0) === 0x06064b50 &&
        end.u64(4) === 44 &&
        end.u16(12) === 0x32d &&
        end.u16(14) === 45 &&
        end.zero(16, 24),
    );
    requireValue(
      end.u64(24) === plan.entries.length &&
        end.u64(32) === plan.entries.length &&
        end.u64(40) === plan.centralSize &&
        end.u64(48) === plan.centralOffset,
    );
    requireValue(
      end.u32(56) === 0x07064b50 &&
        end.zero(60, 64) &&
        end.u64(64) === central &&
        end.u32(72) === 1,
    );
    eocd = 76;
  }
  requireValue(end.u32(eocd) === 0x06054b50 && end.zero(eocd + 4, eocd + 8));
  requireValue(
    end.u16(eocd + 8) === (zip64 ? 0xffff : plan.entries.length) &&
      end.u16(eocd + 10) === (zip64 ? 0xffff : plan.entries.length),
  );
  requireValue(
    end.u32(eocd + 12) === (zip64 ? 0xffffffff : plan.centralSize) &&
      end.u32(eocd + 16) === (zip64 ? 0xffffffff : plan.centralOffset) &&
      end.u16(eocd + 20) === 0,
  );
  signal?.throwIfAborted();
  return Object.freeze({
    valid: true,
    entries: plan.entries.length,
    bytesValidated: plan.totalBytes,
    archiveFormat: plan.archiveFormat,
  });
}
