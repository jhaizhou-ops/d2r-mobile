// 基于 astc_pack.py 的纹理格式码与相对数据偏移，流式保留头部并转换 BC3 尾部。
// Stream-preserve headers and convert BC3 tails using astc_pack.py format codes and relative data offsets.
import { transcodeBC3Batches } from "./bc3-astc-batches.mjs";
export const TEXTURE_HEADER_CHUNK_BYTES = 65536;
function checkAbort(signal) {
  if (signal?.aborted) throw new DOMException("Cancelled", "AbortError");
}
async function readExact(source, offset, length, signal) {
  checkAbort(signal);
  let remove = () => {};
  const pending = Promise.resolve().then(() => {
    checkAbort(signal);
    return source.readAt(offset, length, { signal });
  });
  let raw;
  try {
    raw = signal
      ? await Promise.race([
          pending,
          new Promise((_resolve, reject) => {
            const abort = () => reject(new DOMException("Cancelled", "AbortError"));
            signal.addEventListener("abort", abort, { once: true });
            remove = () => signal.removeEventListener("abort", abort);
            if (signal.aborted) abort();
          }),
        ])
      : await pending;
  } catch (error) {
    if (signal?.aborted || error?.name === "AbortError")
      throw new DOMException("Cancelled", "AbortError");
    throw new Error("Texture read failed");
  } finally {
    remove();
  }
  checkAbort(signal);
  if (
    !(raw instanceof Uint8Array) ||
    !(raw.buffer instanceof ArrayBuffer) ||
    raw.buffer.resizable ||
    raw.byteLength !== length
  )
    throw new Error("Invalid or short texture read");
  return raw;
}
export async function* transcodeTextureStream(source, { signal, batchBlocks, encode } = {}) {
  if (
    !source ||
    !Number.isSafeInteger(source.size) ||
    source.size < 0 ||
    typeof source.readAt !== "function"
  )
    throw new TypeError("Expected a safe-sized texture readAt source");
  if (signal != null && !(signal instanceof AbortSignal))
    throw new TypeError("Expected an AbortSignal");
  checkAbort(signal);
  const size = source.size;
  if (size < 48) {
    yield { kind: "skip", reason: "short-input" };
    return;
  }
  let prefix = await readExact(source, 0, 48, signal);
  const format = prefix[4];
  if (format !== 0x3d && format !== 0x3e) {
    yield { kind: "skip", reason: "unsupported-format", format };
    return;
  }
  const dataStart =
    40 + new DataView(prefix.buffer, prefix.byteOffset, prefix.byteLength).getUint32(40, true);
  if (dataStart < 48 || dataStart >= size || (size - dataStart) % 16)
    throw new Error("Malformed texture data bounds");
  const profile = format === 0x3e ? "srgb" : "linear";
  const newFormat = format === 0x3e ? 0x51 : 0x5f;
  // 在任何输出前验证批处理选项，避免因可预知选项错误留下部分头部。
  // Validate batch options before any output, avoiding partial headers for predictable option errors.
  if (
    batchBlocks !== undefined &&
    (!Number.isSafeInteger(batchBlocks) || batchBlocks < 1 || batchBlocks > 65536)
  )
    throw new RangeError("Invalid texture batch block limit");
  if (encode !== undefined && typeof encode !== "function")
    throw new TypeError("Expected encoder function");
  prefix = prefix.slice();
  prefix[4] = newFormat;
  yield { kind: "header", offset: 0, bytes: prefix, format, newFormat, dataStart, profile };
  prefix = null;
  for (let offset = 48; offset < dataStart;) {
    checkAbort(signal);
    const length = Math.min(TEXTURE_HEADER_CHUNK_BYTES, dataStart - offset);
    const bytes = (await readExact(source, offset, length, signal)).slice();
    yield { kind: "header", offset, bytes };
    offset += length;
  }
  const tail = {
    size: size - dataStart,
    readAt: (offset, length, options) =>
      readExact(source, dataStart + offset, length, options.signal),
  };
  try {
    for await (const batch of transcodeBC3Batches(tail, { profile, signal, batchBlocks, encode })) {
      checkAbort(signal);
      yield {
        kind: "blocks",
        offset: dataStart + batch.blockOffset * 16,
        bytes: batch.blocks,
        blockCount: batch.blockCount,
        completedBlocks: batch.completedBlocks,
        totalBlocks: batch.totalBlocks,
      };
    }
  } catch (error) {
    if (signal?.aborted || error?.name === "AbortError")
      throw new DOMException("Cancelled", "AbortError");
    throw new Error("Texture block conversion failed");
  }
  checkAbort(signal);
  yield {
    kind: "complete",
    size,
    format,
    newFormat,
    profile,
    totalBlocks: (size - dataStart) / 16,
  };
}
