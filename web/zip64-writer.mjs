// 流式写入显式启用的单磁盘 STORE ZIP64。
// 依据 PKWARE APPNOTE 6.3.10 的 ZIP64 记录布局。
// Streams explicitly enabled single-disk STORE ZIP64.
// Based on ZIP64 record layout in PKWARE APPNOTE 6.3.10.

import { indexFiles, ZIP_PATH_LIMITS } from "./preflight.mjs";

/**
 * 选择启用、仅 STORE、单磁盘的 ZIP64。可由封装协调器显式选择。
 * 大小与输出计数使用 BigInt。maxTotalBytes 必填，包含
 * 所有归档字节。接收器写入 Promise 提供背压；接收器在解析后
 * 不得保留缓冲区。生产者在下一次拉取前不得修改已产出的缓冲区。
 * 此模块不保留完整的输入/输出缓冲区。
 * 取消在读写前后协作检查，不会中断待完成的用户代码。
 * 开始 close() 是提交边界；此后的取消无法撤销它。
 * close 失败仍会尽力 abort，但不保证回滚。
 * 依据 PKWARE APPNOTE 6.3.10 §§4.3.7、4.3.9、4.3.12、4.3.14–16、4.5.3。
 * Opt-in, STORE-only, single-disk ZIP64. Explicitly selectable by the packaging coordinator.
 * Sizes and output accounting use BigInt. maxTotalBytes is REQUIRED and includes
 * every archive byte. Sink write promises provide backpressure; sinks must not
 * retain buffers after resolving. Producers must not mutate a yielded buffer
 * until the next pull. No full input/output buffers are retained by this module.
 * Cancellation is cooperative around reads/writes, not an interrupt of pending
 * user code. Starting close() is the commit boundary; later cancellation cannot
 * undo it. A failed close still triggers best-effort abort, not guaranteed rollback.
 * PKWARE APPNOTE 6.3.10 §§4.3.7, 4.3.9, 4.3.12, 4.3.14–16, 4.5.3.
 */
export const ZIP64_LIMITS = Object.freeze({
  ...ZIP_PATH_LIMITS,
  maxEntries: 20_000,
  maxChunkBytes: 4 * 1024 * 1024,
  maxPathBytes: 4096,
  maxMetadataBytes: 8 * 1024 * 1024,
});
const U64_MAX = (1n << 64n) - 1n;
const encoder = new TextEncoder();
const table = Uint32Array.from({ length: 256 }, (_, n) => {
  for (let i = 0; i < 8; i++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
function integer(value, label) {
  if (typeof value === "number" && Number.isSafeInteger(value)) value = BigInt(value);
  if (typeof value !== "bigint" || value < 0n || value > U64_MAX)
    throw new Error(`Invalid ZIP64 ${label}: expected unsigned 64-bit BigInt or safe integer`);
  return value;
}
function record(length) {
  const bytes = new Uint8Array(length);
  const view = new DataView(bytes.buffer);
  return {
    bytes,
    u16: (p, n) => view.setUint16(p, n, true),
    u32: (p, n) => view.setUint32(p, n, true),
    u64: (p, n) => view.setBigUint64(p, integer(n, "record value"), true),
  };
}
function compareNames(a, b) {
  for (let i = 0; i < Math.min(a.name.length, b.name.length); i++) {
    if (a.name[i] !== b.name[i]) return a.name[i] - b.name[i];
  }
  return a.name.length - b.name.length;
}
function prepare(entries, maxTotalBytes) {
  const budget = integer(maxTotalBytes, "output budget");
  if (!Array.isArray(entries) || entries.length > ZIP64_LIMITS.maxEntries)
    throw new Error("ZIP64 entry count limit exceeded");
  let metadata = 98;
  const files = entries.map((entry) => {
    if (!entry) throw new Error("Invalid ZIP64 entry");
    const size = integer(entry.size, "file size");
    if (
      typeof entry.path !== "string" ||
      entry.path.length > ZIP64_LIMITS.maxPathBytes ||
      /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/u.test(entry.path)
    )
      throw new Error("Invalid ZIP64 path encoding or length");
    const name = encoder.encode(entry.path);
    if (name.length > ZIP64_LIMITS.maxPathBytes)
      throw new Error("ZIP64 path length limit exceeded");
    const mode = entry.mode ?? 0o644;
    if (mode !== 0o644 && mode !== 0o755) throw new Error("Unsupported ZIP64 Unix mode");
    if (typeof entry.chunks !== "function") throw new Error("Expected ZIP64 chunk producer");
    metadata += 50 + 74 + 24 + 2 * name.length;
    if (metadata > ZIP64_LIMITS.maxMetadataBytes) throw new Error("ZIP64 metadata limit exceeded");
    return { path: entry.path, name, mode, size, chunks: entry.chunks };
  });
  // 复用路径/冲突策略，不将 BigInt 载荷大小缩窄为 Number。
  // Reuse path/conflict policy without narrowing BigInt payload sizes to Number.
  indexFiles(
    files.map(({ path }) => ({ path, size: 0 })),
    ZIP64_LIMITS,
  );
  files.sort(compareNames);
  let offset = 0n;
  for (const file of files) {
    file.offset = offset;
    offset += BigInt(50 + file.name.length) + file.size + 24n;
  }
  const centralOffset = offset;
  const centralSize = files.reduce((n, file) => n + BigInt(74 + file.name.length), 0n);
  const bytesWritten = centralOffset + centralSize + 98n;
  if (bytesWritten > budget || bytesWritten > U64_MAX)
    throw new Error("ZIP64 output budget or 64-bit limit exceeded");
  return { files, centralOffset, centralSize, bytesWritten };
}

/**
 * 纯声明大小规划；不拉取源，也不声称其字节/CRC 有效。
 * Pure declared-size plan; does not pull sources or claim their bytes/CRC valid.
 */
export function planZip64(entries, { maxTotalBytes } = {}) {
  const plan = prepare(entries, maxTotalBytes);
  return {
    entries: plan.files.map(({ path, size, offset }) => ({ path, size, offset })),
    centralOffset: plan.centralOffset,
    centralSize: plan.centralSize,
    bytesWritten: plan.bytesWritten,
  };
}

/**
 * 纯元数据编码器，也可在不使用载荷的情况下检查 >4GiB 偏移。
 * 结果本身不是完整归档（空目录除外）。
 * Pure metadata encoder, also usable to inspect >4GiB offsets without payloads.
 * The result is NOT a complete archive by itself (except with an empty directory).
 */
export function zip64EndRecords({ entries, centralOffset, centralSize }) {
  const count = integer(entries, "entry count");
  centralOffset = integer(centralOffset, "central offset");
  centralSize = integer(centralSize, "central size");
  integer(centralOffset + centralSize + 98n, "archive size");
  const end = record(98);
  end.u32(0, 0x06064b50);
  end.u64(4, 44n);
  end.u16(12, 0x32d);
  end.u16(14, 45);
  end.u64(24, count);
  end.u64(32, count);
  end.u64(40, centralSize);
  end.u64(48, centralOffset);
  end.u32(56, 0x07064b50);
  end.u64(64, centralOffset + centralSize);
  end.u32(72, 1);
  end.u32(76, 0x06054b50);
  end.u16(84, 0xffff);
  end.u16(86, 0xffff);
  end.u32(88, 0xffffffff);
  end.u32(92, 0xffffffff);
  return end.bytes;
}

export async function writeZip64(entries, sink, { maxTotalBytes, signal } = {}) {
  if (!sink || ["write", "close", "abort"].some((key) => typeof sink[key] !== "function"))
    throw new Error("Expected ZIP64 sink with write, close and abort");
  const check = () => signal?.throwIfAborted();
  let offset = 0n;
  try {
    check();
    const plan = prepare(entries, maxTotalBytes);
    const write = async (bytes) => {
      check();
      const next = offset + BigInt(bytes.byteLength);
      if (next > plan.bytesWritten) throw new Error("Excess ZIP64 output bytes");
      await sink.write(bytes);
      offset = next;
      check();
    };
    for (const file of plan.files) {
      const header = record(50 + file.name.length);
      header.u32(0, 0x04034b50);
      header.u16(4, 45);
      header.u16(6, 0x808);
      header.u16(12, 0x21);
      header.u32(18, 0xffffffff);
      header.u32(22, 0xffffffff);
      header.u16(26, file.name.length);
      header.u16(28, 20);
      header.bytes.set(file.name, 30);
      const extra = 30 + file.name.length;
      header.u16(extra, 1);
      header.u16(extra + 2, 16);
      // 描述符提供 CRC 和实际大小；本地 ZIP64 大小槽位为零。
      // Descriptor supplies CRC and actual sizes; local ZIP64 size slots are zero.
      await write(header.bytes);
      let size = 0n,
        crc = 0xffffffff;
      for await (const bytes of file.chunks()) {
        check();
        if (!(bytes instanceof Uint8Array) || bytes.byteLength > ZIP64_LIMITS.maxChunkBytes)
          throw new Error("Invalid or oversized ZIP64 input chunk");
        const length = BigInt(bytes.byteLength);
        if (length > file.size - size) throw new Error("Excess ZIP64 input bytes");
        size += length;
        for (let i = 0; i < bytes.byteLength; i++)
          crc = table[(crc ^ bytes[i]) & 255] ^ (crc >>> 8);
        if (length) await write(bytes);
      }
      check();
      if (size !== file.size) throw new Error("Short ZIP64 input");
      file.crc = (crc ^ 0xffffffff) >>> 0;
      const descriptor = record(24);
      descriptor.u32(0, 0x08074b50);
      descriptor.u32(4, file.crc);
      descriptor.u64(8, size);
      descriptor.u64(16, size);
      await write(descriptor.bytes);
    }
    if (offset !== plan.centralOffset) throw new Error("ZIP64 local layout mismatch");
    for (const file of plan.files) {
      const header = record(74 + file.name.length);
      header.u32(0, 0x02014b50);
      header.u16(4, 0x32d);
      header.u16(6, 45);
      header.u16(8, 0x808);
      header.u16(14, 0x21);
      header.u32(16, file.crc);
      header.u32(20, 0xffffffff);
      header.u32(24, 0xffffffff);
      header.u16(28, file.name.length);
      header.u16(30, 28);
      header.u32(38, (0o100000 | file.mode) * 65536);
      header.u32(42, 0xffffffff);
      header.bytes.set(file.name, 46);
      const extra = 46 + file.name.length;
      header.u16(extra, 1);
      header.u16(extra + 2, 24);
      header.u64(extra + 4, file.size);
      header.u64(extra + 12, file.size);
      header.u64(extra + 20, file.offset);
      await write(header.bytes);
    }
    await write(zip64EndRecords({ entries: plan.files.length, ...plan }));
    if (offset !== plan.bytesWritten) throw new Error("ZIP64 final layout mismatch");
    check();
    await sink.close();
    return { entries: plan.files.length, bytesWritten: offset };
  } catch (error) {
    try {
      await sink.abort(error);
    } catch {
      /**
       * 保留原始失败。
       * Preserve original failure.
       */
    }
    throw error;
  }
}
