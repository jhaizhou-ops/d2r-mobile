// 基于原生块顺序与有界解码/编码契约，逐批处理 readAt 来源，不保留整张纹理。
// Serial readAt batches based on native block order and bounded decode/encode contracts; never retain a whole texture.
import { decodeBC3BlockGrid, MAX_BC3_BLOCKS } from "./bc3-decoder.mjs";
import { encodeAstc4x4 } from "./client.mjs";
function aborted(signal) {
  if (signal?.aborted) throw new DOMException("Cancelled", "AbortError");
}
// 取消停止等待；底层读取的资源释放仍取决于来源是否遵守传入的 signal。
// Cancellation stops waiting; release of underlying read resources still depends on the source honoring signal.
async function cancellable(operation, signal) {
  aborted(signal);
  if (!signal) return operation();
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      signal.removeEventListener("abort", abort);
      if (error) reject(error);
      else resolve(value);
    };
    const abort = () => finish(new DOMException("Cancelled", "AbortError"));
    signal.addEventListener("abort", abort, { once: true });
    if (signal.aborted) return abort();
    Promise.resolve()
      .then(() => {
        aborted(signal);
        return operation();
      })
      .then(
        (value) => finish(null, value),
        (error) => finish(error),
      );
  });
}
export async function* transcodeBC3Batches(
  source,
  { profile, signal, batchBlocks = MAX_BC3_BLOCKS, encode = encodeAstc4x4 } = {},
) {
  if (
    !source ||
    !Number.isSafeInteger(source.size) ||
    source.size < 0 ||
    source.size % 16 ||
    typeof source.readAt !== "function"
  )
    throw new TypeError("Expected aligned safe-sized readAt source");
  if (profile !== "linear" && profile !== "srgb")
    throw new TypeError("Explicit linear or srgb profile required");
  if (signal != null && !(signal instanceof AbortSignal))
    throw new TypeError("Expected an AbortSignal");
  if (!Number.isSafeInteger(batchBlocks) || batchBlocks < 1 || batchBlocks > MAX_BC3_BLOCKS)
    throw new RangeError("Batch block limit must be 1..65536");
  if (typeof encode !== "function") throw new TypeError("Expected encoder function");
  const totalBytes = source.size,
    totalBlocks = totalBytes / 16;
  aborted(signal);
  for (let offset = 0; offset < totalBytes;) {
    aborted(signal);
    const length = Math.min(batchBlocks * 16, totalBytes - offset),
      count = length / 16;
    let raw = await cancellable(() => source.readAt(offset, length, { signal }), signal);
    aborted(signal);
    if (
      !(raw instanceof Uint8Array) ||
      !(raw.buffer instanceof ArrayBuffer) ||
      raw.buffer.resizable ||
      raw.byteLength !== length
    )
      throw new Error("Invalid or short BC3 read");
    let image = decodeBC3BlockGrid(raw);
    raw = null;
    let encoded = await cancellable(
      () =>
        encode({ rgba: image.rgba, width: image.width, height: image.height, profile }, { signal }),
      signal,
    );
    aborted(signal);
    const paddedLength = image.width * image.height;
    image = null;
    if (
      !(encoded instanceof ArrayBuffer) ||
      encoded.resizable ||
      encoded.byteLength !== paddedLength
    )
      throw new Error("Invalid padded ASTC result length");
    // ASTC 每块也是 16 字节；仅移除拼图最后的填充块，不重排真实块。
    // ASTC also uses 16 bytes per block; remove only the final grid padding, never reorder real blocks.
    const blocks = new Uint8Array(
      encoded.byteLength === length ? encoded : encoded.slice(0, length),
    );
    encoded = null;
    const blockOffset = offset / 16;
    offset += length;
    yield {
      blocks,
      blockOffset,
      blockCount: count,
      completedBlocks: offset / 16,
      totalBlocks,
      profile,
    };
  }
}
