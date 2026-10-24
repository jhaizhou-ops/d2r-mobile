// 对旧式 NSO 段进行有界验证，不认证游戏构建。
// 依据 NSO 段头及原始 LZ4 块布局。
// Performs bounded legacy NSO segment validation without authenticating a game build.
// Based on NSO segment headers and raw LZ4 block layout.

/**
 * 有界的旧式 NSO 段验证。绝不认证游戏构建。
 * Bounded legacy NSO segment validation. Never authenticates a game build.
 */
import { inspectNso, readRange, MAX_READ_BYTES, NSO_HEADER_BYTES } from "./preflight.mjs";

export const NSO_VALIDATION_LIMITS = Object.freeze({
  maxSegmentBytes: 64 * 1024 * 1024,
  maxTotalDecodedBytes: 128 * 1024 * 1024,
  maxWorkingBytes: 256 * 1024 * 1024,
  maxImageBytes: 256 * 1024 * 1024,
  chunkBytes: 64 * 1024,
});
const HARD_LIMIT = 256 * 1024 * 1024;
const yieldTask = () => new Promise((resolve) => setTimeout(resolve, 0));

function limitsFor(options) {
  const limits = { ...NSO_VALIDATION_LIMITS, ...options };
  for (const [key, value] of Object.entries(limits)) {
    if (
      !Object.hasOwn(NSO_VALIDATION_LIMITS, key) ||
      !Number.isSafeInteger(value) ||
      value <= 0 ||
      value > HARD_LIMIT
    )
      throw new Error(`Invalid validation limit: ${key}`);
  }
  if (limits.chunkBytes > MAX_READ_BYTES) throw new Error("Chunk exceeds read range limit");
  return limits;
}
async function checkpoint(signal) {
  signal?.throwIfAborted();
  await yieldTask();
  signal?.throwIfAborted();
}

/**
 * 原始独立 LZ4 块，不是帧或带大小前缀的 Python 块。
 * Raw independent LZ4 block, not a frame or size-prefixed Python block.
 */
export async function decodeLz4Block(input, decodedSize, { signal, limits: options } = {}) {
  const limits = limitsFor(options);
  signal?.throwIfAborted();
  if (
    !(input instanceof Uint8Array) ||
    !Number.isSafeInteger(decodedSize) ||
    decodedSize < 0 ||
    decodedSize > limits.maxSegmentBytes ||
    decodedSize > limits.maxTotalDecodedBytes ||
    input.length > limits.maxSegmentBytes ||
    decodedSize + input.length > limits.maxWorkingBytes
  )
    throw new Error("LZ4 allocation budget exceeded or invalid input");
  const output = new Uint8Array(decodedSize);
  let ip = 0,
    op = 0,
    lastMatchStart = -1,
    workSinceYield = 0;
  const pauseIfNeeded = () => {
    signal?.throwIfAborted();
    if (workSinceYield < limits.chunkBytes) return undefined;
    workSinceYield = 0;
    return checkpoint(signal);
  };
  const byte = () => {
    if (ip >= input.length) throw new Error("Truncated LZ4 block");
    workSinceYield++;
    return input[ip++];
  };
  async function length(base) {
    let value = base;
    if (base === 15) {
      let extra;
      do {
        extra = byte();
        value += extra;
        if (value > decodedSize) throw new Error("LZ4 length exceeds decoded size");
        const pause = pauseIfNeeded();
        if (pause) await pause;
      } while (extra === 255);
    }
    return value;
  }
  while (ip < input.length) {
    signal?.throwIfAborted();
    const token = byte();
    const literals = await length(token >>> 4);
    if (literals > input.length - ip || literals > output.length - op)
      throw new Error("LZ4 literal range exceeded");
    for (let n = 0; n < literals;) {
      const count = Math.min(limits.chunkBytes, literals - n);
      output.set(input.subarray(ip, ip + count), op);
      ip += count;
      op += count;
      n += count;
      workSinceYield += count * 2; // 消耗的字面量输入与写出的输出。 | Literal input consumed and output written.
      const pause = pauseIfNeeded();
      if (pause) await pause;
    }
    if (ip === input.length) {
      if (op !== decodedSize) throw new Error("LZ4 decoded size mismatch");
      if (lastMatchStart >= 0 && (literals < 5 || decodedSize - lastMatchStart < 12))
        throw new Error("Invalid LZ4 terminal sequence");
      signal?.throwIfAborted();
      return output;
    }
    const offset = byte() | (byte() << 8);
    if (!offset || offset > op) throw new Error("Invalid LZ4 match offset");
    const matchLength = (await length(token & 15)) + 4;
    if (matchLength > output.length - op) throw new Error("LZ4 match range exceeded");
    lastMatchStart = op;
    for (let n = 0; n < matchLength;) {
      const count = Math.min(limits.chunkBytes, matchLength - n);
      // 必须逐字节向前复制：匹配段可能与其源重叠。
      // Forward byte copying is essential: matches may overlap their source.
      for (let j = 0; j < count; j++) {
        output[op] = output[op - offset];
        op++;
      }
      n += count;
      workSinceYield += count;
      const pause = pauseIfNeeded();
      if (pause) await pause;
    }
  }
  throw new Error("Missing LZ4 terminal sequence");
}

async function digest(bytes, signal) {
  signal?.throwIfAborted();
  if (!globalThis.crypto?.subtle) throw new Error("WebCrypto SHA-256 is unavailable");
  // WebCrypto 无法取消内部摘要。及时拒绝并丢弃结果。
  // WebCrypto cannot cancel its internal digest. Reject promptly and discard it.
  let onAbort;
  const aborted = new Promise((_, reject) => {
    onAbort = () => reject(signal.reason);
    signal?.addEventListener("abort", onAbort, { once: true });
  });
  try {
    const result = await Promise.race([globalThis.crypto.subtle.digest("SHA-256", bytes), aborted]);
    signal?.throwIfAborted();
    return new Uint8Array(result);
  } finally {
    signal?.removeEventListener("abort", onAbort);
  }
}

/**
 * 返回独立的已解码段，而非可执行映像或受支持的构建。
 * Returns separate decoded segments, not an executable image or supported build.
 */
export async function validateNsoSegments(source, { signal, limits: options } = {}) {
  const limits = limitsFor(options);
  const header = (await readRange(source, 0, NSO_HEADER_BYTES, signal)).slice();
  const info = await inspectNso({ size: source.size, read: async () => header }, signal);
  const total = info.segments.reduce((sum, s) => sum + s.decodedSize, 0);
  const maxStored = Math.max(...info.segments.map((s) => s.storedSize));
  const maxDecoded = Math.max(...info.segments.map((s) => s.decodedSize));
  const data = info.segments[2];
  const bssStart = data.memoryOffset + data.decodedSize;
  const imageEnd = Math.max(
    bssStart + info.bssSize,
    ...info.segments.map((s) => s.memoryOffset + s.decodedSize),
  );
  if (
    maxStored > limits.maxSegmentBytes ||
    maxDecoded > limits.maxSegmentBytes ||
    total > limits.maxTotalDecodedBytes ||
    total + maxStored + maxDecoded + limits.chunkBytes + NSO_HEADER_BYTES >
      limits.maxWorkingBytes ||
    imageEnd > limits.maxImageBytes
  )
    throw new Error("NSO allocation budget exceeded");
  for (const segment of info.segments.slice(0, 2)) {
    if (
      info.bssSize &&
      segment.decodedSize &&
      segment.memoryOffset < bssStart + info.bssSize &&
      bssStart < segment.memoryOffset + segment.decodedSize
    )
      throw new Error("BSS overlaps segment");
  }
  const segments = [];
  for (const [i, segment] of info.segments.entries()) {
    signal?.throwIfAborted();
    const stored = new Uint8Array(segment.storedSize);
    for (let offset = 0; offset < stored.length; offset += limits.chunkBytes) {
      const count = Math.min(limits.chunkBytes, stored.length - offset);
      stored.set(await readRange(source, segment.fileOffset + offset, count, signal), offset);
      await checkpoint(signal);
    }
    const bytes = segment.compressed
      ? await decodeLz4Block(stored, segment.decodedSize, { signal, limits })
      : stored;
    const hashRequired = Boolean(info.flags & (1 << (i + 3)));
    if (hashRequired) {
      const actual = await digest(bytes, signal);
      const expected = header.subarray(0xa0 + i * 32, 0xc0 + i * 32);
      if (!actual.every((value, index) => value === expected[index]))
        throw new Error(`NSO ${segment.name} SHA-256 mismatch`);
    }
    segments.push({
      ...segment,
      bytes,
      hashRequired,
      integrity: hashRequired ? "verified" : "not-requested",
    });
  }
  signal?.throwIfAborted();
  return {
    ...info,
    segments,
    structuralValidation: "legacy-segments-only",
    requiredHashesVerified: true,
    allSegmentsHashed: segments.every((s) => s.hashRequired),
    supportedGameBuild: false,
    versionVerified: false,
  };
}
