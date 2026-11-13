// 有界读取 ZIP32 外壳输入并检查 CRC。
// 依据 ZIP32 中央目录、本地记录和 CRC 约定。
// Reads bounded ZIP32 shell input and checks CRC.
// Based on ZIP32 central-directory, local-record, and CRC conventions.

/**
 * 有界 ZIP32 外壳输入。CRC 检测损坏，而非密码学真实性。
 * Bounded ZIP32 shell input. CRC detects corruption, not cryptographic authenticity.
 */
import { createDeflateBoundary } from "./deflate-boundary.mjs";
import { MAX_READ_BYTES, readRange, safeRelativePath, ZIP_PATH_LIMITS } from "./preflight.mjs";

export const ZIP_LIMITS = Object.freeze({
  archiveBytes: 256 * 1024 * 1024,
  totalBytes: 256 * 1024 * 1024,
  entryBytes: 64 * 1024 * 1024,
  entries: 20000,
  pathNodes: ZIP_PATH_LIMITS.maxPathNodes,
  pathDepth: ZIP_PATH_LIMITS.maxPathDepth,
  centralBytes: 8 * 1024 * 1024,
  nameBytes: 2048,
  pathCharacters: 512,
  extraBytes: 8192,
  expansionRatio: 1000,
  compressedSlice: 1024,
});
const fail = (message) => {
  throw new Error(message);
};
const check = (signal) => {
  if (signal?.aborted) fail("ZIP operation cancelled");
};
const view = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
const u16 = (b, p) => view(b).getUint16(p, true);
const u32 = (b, p) => view(b).getUint32(p, true);
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
async function range(source, offset, length, signal) {
  check(signal);
  let bytes;
  try {
    bytes = await readRange(source, offset, length);
  } catch {
    check(signal);
    fail("Invalid, missing or truncated ZIP range");
  }
  check(signal);
  return bytes;
}
function extra(bytes) {
  for (let p = 0; p < bytes.length;) {
    if (bytes.length - p < 4) fail("Truncated ZIP extra field");
    const id = u16(bytes, p),
      length = u16(bytes, p + 2);
    if (length > bytes.length - p - 4) fail("Truncated ZIP extra field");
    // 拒绝替代路径解释、ZIP64 和加密扩展。
    // Reject alternate path interpretations, ZIP64 and encryption extensions.
    if ([1, 0x7075, 0x6375, 0x9901, 0x0017].includes(id)) fail("Unsupported ZIP extra field");
    p += 4 + length;
  }
}
function name(bytes, flags) {
  if (!(flags & 0x800) && bytes.some((b) => b > 127))
    fail("ZIP names must be ASCII or flagged UTF-8");
  let result;
  try {
    result = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(bytes);
  } catch {
    fail("Invalid ZIP UTF-8 name");
  }
  if (
    result.length > ZIP_LIMITS.pathCharacters ||
    /[<>"|?*\u0080-\u009f\u200e\u200f\u202a-\u202e\u2066-\u2069\ufeff]/u.test(result)
  )
    fail("Unsafe ZIP path");
  try {
    safeRelativePath(result.replace(/\/$/, ""));
  } catch {
    fail("Unsafe ZIP path");
  }
  return result;
}
const crcTable = Uint32Array.from({ length: 256 }, (_, n) => {
  for (let i = 0; i < 8; i++) n = (n >>> 1) ^ (n & 1 ? 0xedb88320 : 0);
  return n >>> 0;
});
function crcStep(crc, bytes) {
  for (const b of bytes) crc = crcTable[(crc ^ b) & 255] ^ (crc >>> 8);
  return crc;
}

/**
 * source 是 preflight 的 { size, read(offset, length): Promise<Uint8Array> }。
 * source is preflight's { size, read(offset, length): Promise<Uint8Array> }.
 */
export async function openShellZip(source, { signal } = {}) {
  check(signal);
  if (
    !source ||
    !Number.isSafeInteger(source.size) ||
    source.size < 22 ||
    source.size > ZIP_LIMITS.archiveBytes ||
    typeof source.read !== "function"
  )
    fail("Invalid or oversized ZIP source");
  const tailStart = Math.max(0, source.size - 65557);
  const tail = await range(source, tailStart, source.size - tailStart, signal);
  let end = -1;
  for (let p = tail.length - 22; p >= 0; p--) {
    if (u32(tail, p) === 0x06054b50 && p + 22 + u16(tail, p + 20) === tail.length) {
      if (end !== -1) fail("Ambiguous ZIP end record");
      end = p;
    }
  }
  if (end < 0) fail("Missing ZIP end record");
  const count = u16(tail, end + 10),
    centralSize = u32(tail, end + 12),
    centralStart = u32(tail, end + 16);
  if (u16(tail, end + 4) || u16(tail, end + 6) || u16(tail, end + 8) !== count)
    fail("Multi-disk ZIP unsupported");
  if (count === 0xffff || centralSize === 0xffffffff || centralStart === 0xffffffff)
    fail("ZIP64 input unsupported");
  if (
    !count ||
    count > ZIP_LIMITS.entries ||
    centralSize > ZIP_LIMITS.centralBytes ||
    centralStart + centralSize !== tailStart + end
  )
    fail("Invalid ZIP central directory bounds");
  const internal = [],
    nodes = new Map(),
    explicit = new Set();
  let position = centralStart,
    total = 0;
  for (let i = 0; i < count; i++) {
    if (centralStart + centralSize - position < 46) fail("Truncated ZIP central directory");
    const h = await range(source, position, 46, signal);
    if (u32(h, 0) !== 0x02014b50) fail("Invalid ZIP central header");
    const flags = u16(h, 8),
      method = u16(h, 10),
      crc32 = u32(h, 16),
      compressedSize = u32(h, 20),
      size = u32(h, 24);
    const nameLength = u16(h, 28),
      extraLength = u16(h, 30),
      commentLength = u16(h, 32),
      offset = u32(h, 42);
    if (u16(h, 6) > 20 || u16(h, 34) || [size, compressedSize, offset].includes(0xffffffff))
      fail("Unsupported ZIP version, disk or ZIP64 entry");
    if (![0, 8].includes(method) || flags & ~(0x800 | 8 | (method === 8 ? 6 : 0)))
      fail("Encrypted or unsupported ZIP compression/flags");
    if (
      !nameLength ||
      nameLength > ZIP_LIMITS.nameBytes ||
      extraLength > ZIP_LIMITS.extraBytes ||
      46 + nameLength + extraLength + commentLength > centralStart + centralSize - position
    )
      fail("Invalid ZIP name or extra bounds");
    total += size;
    if (
      size > ZIP_LIMITS.entryBytes ||
      compressedSize > ZIP_LIMITS.entryBytes ||
      total > ZIP_LIMITS.totalBytes ||
      (size > 0 && (!compressedSize || size > compressedSize * ZIP_LIMITS.expansionRatio)) ||
      (method === 0 && size !== compressedSize)
    )
      fail("ZIP size or expansion limit");
    const bytes = await range(source, position + 46, nameLength + extraLength, signal);
    const nameBytes = bytes.slice(0, nameLength),
      path = name(nameBytes, flags),
      directory = path.endsWith("/");
    extra(bytes.subarray(nameLength));
    const attributes = u32(h, 38),
      rawMode = attributes >>> 16,
      type = rawMode & 0xf000;
    if (
      (type && type !== (directory ? 0x4000 : 0x8000)) ||
      rawMode & 0o7000 ||
      (attributes & 0x10 && !directory) ||
      (directory && (size || crc32))
    )
      fail("Unsafe ZIP file type or directory");
    const mode = rawMode || (directory ? 0o40755 : 0o100644);
    const parts = path.replace(/\/$/, "").split("/");
    if (parts.length > ZIP_LIMITS.pathDepth) fail("ZIP path depth limit");
    for (let n = 1; n <= parts.length; n++) {
      const original = parts.slice(0, n).join("/"),
        key = original.toLowerCase(),
        isDir = n < parts.length || directory;
      const previous = nodes.get(key);
      if (previous && (previous.original !== original || previous.directory !== isDir))
        fail("ZIP case or ancestor conflict");
      if (n === parts.length) {
        if (explicit.has(key)) fail("Duplicate ZIP path");
        explicit.add(key);
      }
      nodes.set(key, { original, directory: isDir });
      if (nodes.size > ZIP_LIMITS.pathNodes) fail("ZIP path node limit");
    }
    internal.push({
      path,
      directory,
      mode,
      size,
      compressedSize,
      crc32,
      method,
      flags,
      offset,
      nameBytes,
      version: u16(h, 6),
      time: u16(h, 12),
      date: u16(h, 14),
    });
    position += 46 + nameLength + extraLength + commentLength;
  }
  if (position !== centralStart + centralSize) fail("ZIP central directory size mismatch");
  // 在允许分配任何条目输出前验证每个本地记录。
  // Validate every local record before permitting any entry-output allocation.
  const physical = [...internal].sort((a, b) => a.offset - b.offset);
  let expectedOffset = 0;
  for (const entry of physical) {
    if (entry.offset !== expectedOffset || centralStart - entry.offset < 30)
      fail("Overlapping, gapped or invalid ZIP local records");
    const h = await range(source, entry.offset, 30, signal);
    const nl = u16(h, 26),
      el = u16(h, 28);
    if (
      u32(h, 0) !== 0x04034b50 ||
      u16(h, 4) !== entry.version ||
      u16(h, 10) !== entry.time ||
      u16(h, 12) !== entry.date ||
      u16(h, 6) !== entry.flags ||
      u16(h, 8) !== entry.method ||
      nl !== entry.nameBytes.length ||
      el > ZIP_LIMITS.extraBytes
    )
      fail("ZIP local/central header mismatch");
    const dataOffset = entry.offset + 30 + nl + el,
      dataEnd = dataOffset + entry.compressedSize;
    if (dataEnd > centralStart) fail("ZIP entry exceeds local bounds");
    const variable = await range(source, entry.offset + 30, nl + el, signal);
    if (!same(variable.subarray(0, nl), entry.nameBytes)) fail("ZIP local/central name mismatch");
    extra(variable.subarray(nl));
    const descriptor = Boolean(entry.flags & 8);
    for (const [p, expected] of [
      [14, entry.crc32],
      [18, entry.compressedSize],
      [22, entry.size],
    ]) {
      const actual = u32(h, p);
      if (actual !== expected && !(descriptor && actual === 0))
        fail("ZIP local/central size or CRC mismatch");
    }
    expectedOffset = dataEnd;
    if (descriptor) {
      if (centralStart - dataEnd < 12) fail("Truncated ZIP data descriptor");
      const first = await range(source, dataEnd, 12, signal);
      // 使用中央 CRC 及其后的值解决签名冲突。
      // Signature collision is resolved using the central CRC and following values.
      if (
        u32(first, 0) === entry.crc32 &&
        u32(first, 4) === entry.compressedSize &&
        u32(first, 8) === entry.size
      ) {
        expectedOffset += 12;
      } else {
        if (u32(first, 0) !== 0x08074b50 || centralStart - dataEnd < 16)
          fail("Invalid ZIP data descriptor");
        const last = await range(source, dataEnd + 12, 4, signal);
        if (
          u32(first, 4) !== entry.crc32 ||
          u32(first, 8) !== entry.compressedSize ||
          u32(last, 0) !== entry.size
        )
          fail("ZIP descriptor/central mismatch");
        expectedOffset += 16;
      }
    }
    entry.dataOffset = dataOffset;
  }
  if (expectedOffset !== centralStart) fail("Unexpected bytes before ZIP central directory");
  const byPath = new Map(internal.map((entry) => [entry.path, entry]));
  const entries = Object.freeze(
    internal.map(({ path, size, mode, directory, compressedSize, crc32, method }) =>
      Object.freeze({ path, size, mode, directory, compressedSize, crc32, method }),
    ),
  );
  let reading = false;
  return Object.freeze({
    entries,
    async readEntry(path, { signal: readSignal = signal } = {}) {
      check(readSignal);
      const entry = byPath.get(path);
      if (!entry) fail("Missing ZIP entry");
      if (reading) fail("Concurrent ZIP entry reads unsupported");
      reading = true;
      try {
        return await decodeEntry(source, entry, readSignal);
      } finally {
        reading = false;
      }
    },
  });
}

async function decodeEntry(source, entry, signal) {
  const output = new Uint8Array(entry.size);
  let written = 0,
    crc = 0xffffffff;
  const consume = (bytes) => {
    check(signal);
    if (bytes.length > output.length - written) fail("ZIP decoded output exceeds declared size");
    output.set(bytes, written);
    written += bytes.length;
    crc = crcStep(crc, bytes);
  };
  if (entry.method === 0) {
    for (let p = 0; p < entry.compressedSize; p += MAX_READ_BYTES)
      consume(
        await range(
          source,
          entry.dataOffset + p,
          Math.min(MAX_READ_BYTES, entry.compressedSize - p),
          signal,
        ),
      );
  } else {
    const boundary = createDeflateBoundary(entry.size);
    let stream;
    try {
      stream = new DecompressionStream("deflate-raw");
    } catch {
      fail("Raw DEFLATE decompression unavailable");
    }
    const reader = stream.readable.getReader(),
      writer = stream.writable.getWriter();
    let stopped = false,
      cleanup;
    const stop = () => {
      if (!stopped) {
        stopped = true;
        cleanup = Promise.allSettled([reader.cancel(), writer.abort()]);
      }
    };
    const active = () => {
      check(signal);
      if (stopped) fail("Invalid ZIP DEFLATE stream");
    };
    signal?.addEventListener("abort", stop, { once: true });
    const producer = (async () => {
      try {
        for (let p = 0; p < entry.compressedSize; p += ZIP_LIMITS.compressedSlice) {
          active();
          const bytes = await range(
            source,
            entry.dataOffset + p,
            Math.min(ZIP_LIMITS.compressedSlice, entry.compressedSize - p),
            signal,
          );
          active();
          // Chromium 会在 JS 能检查输出前展开整个输入块。
          // 保持输入微小，等待背压，同时读取有上限的输出。
          // Chromium expands an entire input chunk before JS can inspect output.
          // Keep input tiny, await backpressure and drain capped output concurrently.
          boundary.push(bytes);
          await writer.write(bytes);
        }
        active();
        boundary.finish();
        await writer.close();
      } catch {
        stop();
        fail("Invalid, truncated or cancelled ZIP DEFLATE input");
      }
    })();
    const consumer = (async () => {
      try {
        while (true) {
          active();
          const { value, done } = await reader.read();
          active();
          if (done) break;
          consume(value);
        }
      } catch {
        stop();
        fail("Invalid, oversized or cancelled ZIP DEFLATE output");
      }
    })();
    try {
      await Promise.all([producer, consumer]);
      check(signal);
    } finally {
      await Promise.allSettled([producer, consumer]);
      if (cleanup) await cleanup;
      signal?.removeEventListener("abort", stop);
      reader.releaseLock();
      writer.releaseLock();
    }
  }
  check(signal);
  if (written !== entry.size || (crc ^ 0xffffffff) >>> 0 !== entry.crc32)
    fail("ZIP decoded length or CRC mismatch");
  return output;
}
