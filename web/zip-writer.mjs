// 将受限条目流式写入仅 STORE 的 ZIP32。
// 依据 ZIP32 STORE 记录布局与预检路径约定。
// Streams bounded entries into STORE-only ZIP32.
// Based on ZIP32 STORE record layout and preflight path contracts.

import { indexFiles, ZIP_PATH_LIMITS } from "./preflight.mjs";

/**
 * 仅 STORE 的 ZIP32 原型。不支持压缩、ZIP64、目录或符号链接。
 * entries：[{path, size, mode?: 0o644 | 0o755, chunks: () => AsyncIterable<Uint8Array>}]
 * sink：{write(bytes): Promise<void>, close(): Promise<void>, abort(error): Promise<void>}
 * 调用方拥有目标，并且必须丢弃已中止的部分归档。
 * 固定的 DOS 时间戳为 1980-01-01 00:00:00；路径按 UTF-8 字节排序。
 * 依次等待写入。输入块不得超过 MAX_CHUNK_BYTES；
 * 生产者必须让已产出的缓冲区保持不变，直到下一次拉取。
 * 在读写前后检查取消（无法中断不配合的接收器）。
 * 开始 sink.close() 是提交边界：此后的取消无法撤销
 * 已提交的目标，也不由此写入器处理。
 * STORE-only ZIP32 prototype. No compression, ZIP64, directories or symlinks.
 * entries: [{path, size, mode?: 0o644 | 0o755, chunks: () => AsyncIterable<Uint8Array>}]
 * sink: {write(bytes): Promise<void>, close(): Promise<void>, abort(error): Promise<void>}
 * The caller owns the destination and must discard an aborted partial archive.
 * Fixed 1980-01-01 00:00:00 DOS timestamp; UTF-8 bytewise pathname order.
 * Writes are awaited sequentially. Input chunks must not exceed MAX_CHUNK_BYTES;
 * producers must keep yielded buffers unchanged until the next pull. Cancellation
 * is checked around reads/writes (it cannot interrupt an uncooperative sink).
 * Starting sink.close() is the commit boundary: cancellation after that point
 * cannot undo a committed destination and is not handled by this writer.
 */
export const ZIP_LIMITS = Object.freeze({
  ...ZIP_PATH_LIMITS,
  maxTotalBytes: 512 * 1024 * 1024,
  maxFileBytes: 256 * 1024 * 1024,
  maxEntries: 20_000,
  maxChunkBytes: 4 * 1024 * 1024,
  maxPathBytes: 4096,
  maxMetadataBytes: 8 * 1024 * 1024,
});
const encoder = new TextEncoder();
const flags = 0x808; // UTF-8 和数据描述符。 | UTF-8 and data descriptor.
const table = Uint32Array.from({ length: 256 }, (_, n) => {
  for (let i = 0; i < 8; i++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
function crcUpdate(crc, bytes) {
  for (const byte of bytes) crc = table[(crc ^ byte) & 255] ^ (crc >>> 8);
  return crc >>> 0;
}
function record(length) {
  const bytes = new Uint8Array(length);
  const view = new DataView(bytes.buffer);
  return {
    bytes,
    u16: (p, n) => view.setUint16(p, n, true),
    u32: (p, n) => view.setUint32(p, n, true),
  };
}
function compareNames(a, b) {
  for (let i = 0; i < Math.min(a.name.length, b.name.length); i++) {
    if (a.name[i] !== b.name[i]) return a.name[i] - b.name[i];
  }
  return a.name.length - b.name.length;
}
function prepare(entries) {
  if (!Array.isArray(entries) || entries.length > ZIP_LIMITS.maxEntries)
    throw new Error("ZIP entry count limit exceeded");
  let total = 0,
    metadata = 22;
  const prepared = entries.map((entry) => {
    if (
      !entry ||
      !Number.isSafeInteger(entry.size) ||
      entry.size < 0 ||
      entry.size > ZIP_LIMITS.maxFileBytes
    )
      throw new Error("ZIP file size limit exceeded");
    if (
      typeof entry.path !== "string" ||
      entry.path.length > ZIP_LIMITS.maxPathBytes ||
      /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/u.test(entry.path)
    )
      throw new Error("Invalid ZIP path encoding or length");
    const name = encoder.encode(entry.path);
    if (name.length > ZIP_LIMITS.maxPathBytes) throw new Error("ZIP path length limit exceeded");
    const mode = entry.mode ?? 0o644;
    if (mode !== 0o644 && mode !== 0o755) throw new Error("Unsupported ZIP Unix mode");
    if (typeof entry.chunks !== "function") throw new Error("Expected ZIP chunk producer");
    total += entry.size;
    metadata += 30 + 46 + 16 + 2 * name.length;
    if (total > ZIP_LIMITS.maxTotalBytes || metadata > ZIP_LIMITS.maxMetadataBytes)
      throw new Error("ZIP total or metadata limit exceeded");
    return { path: entry.path, size: entry.size, mode, name, chunks: entry.chunks };
  });
  indexFiles(prepared, ZIP_LIMITS); // 路径安全、大小写冲突、重复项与祖先冲突。 | Path safety, case collisions, duplicate and ancestor conflicts.
  return prepared.sort(compareNames);
}

export async function writeZip(entries, sink, { signal } = {}) {
  if (!sink || ["write", "close", "abort"].some((key) => typeof sink[key] !== "function"))
    throw new Error("Expected ZIP sink with write, close and abort");
  let offset = 0;
  const check = () => signal?.throwIfAborted();
  const write = async (bytes) => {
    check();
    await sink.write(bytes);
    offset += bytes.length;
    check();
  };
  try {
    check();
    const files = prepare(entries); // 任何写入/拉取之前验证所有声明的预算。 | All declared budgets validated before any writes/pulls.
    for (const file of files) {
      file.offset = offset;
      const header = record(30 + file.name.length);
      header.u32(0, 0x04034b50);
      header.u16(4, 20);
      header.u16(6, flags);
      header.u16(12, 0x21);
      header.u16(26, file.name.length);
      header.bytes.set(file.name, 30);
      await write(header.bytes);
      let size = 0,
        crc = 0xffffffff;
      for await (const bytes of file.chunks()) {
        check();
        if (!(bytes instanceof Uint8Array) || bytes.length > ZIP_LIMITS.maxChunkBytes)
          throw new Error("Invalid or oversized ZIP input chunk");
        if (bytes.length > file.size - size) throw new Error("Excess ZIP input bytes");
        size += bytes.length;
        crc = crcUpdate(crc, bytes);
        if (bytes.length) await write(bytes);
      }
      check();
      if (size !== file.size) throw new Error("Short ZIP input");
      file.crc = (crc ^ 0xffffffff) >>> 0;
      const descriptor = record(16);
      descriptor.u32(0, 0x08074b50);
      descriptor.u32(4, file.crc);
      descriptor.u32(8, size);
      descriptor.u32(12, size);
      await write(descriptor.bytes);
    }
    const centralOffset = offset;
    for (const file of files) {
      const header = record(46 + file.name.length);
      header.u32(0, 0x02014b50);
      header.u16(4, 0x314);
      header.u16(6, 20);
      header.u16(8, flags);
      header.u16(14, 0x21);
      header.u32(16, file.crc);
      header.u32(20, file.size);
      header.u32(24, file.size);
      header.u16(28, file.name.length);
      header.u32(38, (0o100000 | file.mode) * 65536);
      header.u32(42, file.offset);
      header.bytes.set(file.name, 46);
      await write(header.bytes);
    }
    const end = record(22);
    end.u32(0, 0x06054b50);
    end.u16(8, files.length);
    end.u16(10, files.length);
    end.u32(12, offset - centralOffset);
    end.u32(16, centralOffset);
    await write(end.bytes);
    check();
    await sink.close();
    return { entries: files.length, bytesWritten: offset };
  } catch (error) {
    // 即使目标清理也失败，仍保留原始失败。
    // Preserve the original failure even if destination cleanup also fails.
    try {
      await sink.abort(error);
    } catch {
      /**
       * 原始错误优先。
       * Original error takes priority.
       */
    }
    throw error;
  }
}
