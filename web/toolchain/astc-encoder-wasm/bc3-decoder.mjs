// 基于 dm/tools/astc_pack.py 的 bc3_decode 与 blocks_to_image，实现有界、原样通道的解码。
// Bounded identity-channel decoding based on bc3_decode and blocks_to_image in dm/tools/astc_pack.py.
import { MAX_PIXELS, MAX_DIMENSION } from "./contract.mjs";
export const MAX_BC3_BLOCKS = MAX_PIXELS / 16;
function blockCount(input) {
  if (
    !(input instanceof Uint8Array) ||
    !(input.buffer instanceof ArrayBuffer) ||
    input.buffer.resizable
  )
    throw new TypeError("Expected a fixed, non-shared Uint8Array");
  if (!input.byteLength || input.byteLength % 16 || input.byteLength / 16 > MAX_BC3_BLOCKS)
    throw new RangeError("Expected 1..65536 complete BC3 blocks");
  return input.byteLength / 16;
}
function decode(input, count, output, rowBlocks) {
  const alpha = new Uint8Array(8),
    colors = new Uint8Array(12);
  for (let block = 0; block < count; ++block) {
    const src = block * 16,
      a0 = input[src],
      a1 = input[src + 1];
    alpha[0] = a0;
    alpha[1] = a1;
    for (let k = 2; k < 8; ++k)
      alpha[k] =
        a0 > a1
          ? Math.floor(((8 - k) * a0 + (k - 1) * a1 + 3) / 7)
          : k === 6
            ? 0
            : k === 7
              ? 255
              : Math.floor(((6 - k) * a0 + (k - 1) * a1 + 2) / 5);
    for (let endpoint = 0; endpoint < 2; ++endpoint) {
      const c = input[src + 8 + endpoint * 2] | (input[src + 9 + endpoint * 2] << 8);
      const r = (c >>> 11) & 31,
        g = (c >>> 5) & 63,
        b = c & 31;
      colors[endpoint * 3] = (r << 3) | (r >>> 2);
      colors[endpoint * 3 + 1] = (g << 2) | (g >>> 4);
      colors[endpoint * 3 + 2] = (b << 3) | (b >>> 2);
    }
    // 即使端点顺序反转，也始终使用 BC3 四色模式；不采用 BC1 透明模式。
    // Always use BC3 four-color mode, even for reversed endpoints; never BC1 transparency mode.
    for (let channel = 0; channel < 3; ++channel) {
      colors[6 + channel] = Math.floor((2 * colors[channel] + colors[3 + channel] + 1) / 3);
      colors[9 + channel] = Math.floor((colors[channel] + 2 * colors[3 + channel] + 1) / 3);
    }
    const alphaLow = input[src + 2] | (input[src + 3] << 8) | (input[src + 4] << 16);
    const alphaHigh = input[src + 5] | (input[src + 6] << 8) | (input[src + 7] << 16);
    const colorBits =
      input[src + 12] | (input[src + 13] << 8) | (input[src + 14] << 16) | (input[src + 15] << 24);
    for (let pixel = 0; pixel < 16; ++pixel) {
      const a = ((pixel < 8 ? alphaLow : alphaHigh) >>> ((pixel % 8) * 3)) & 7;
      const c = ((colorBits >>> (pixel * 2)) & 3) * 3;
      const dst =
        rowBlocks === 0
          ? block * 64 + pixel * 4
          : ((Math.floor(block / rowBlocks) * 4 + Math.floor(pixel / 4)) * rowBlocks * 4 +
              (block % rowBlocks) * 4 +
              (pixel % 4)) *
            4;
      output[dst] = colors[c];
      output[dst + 1] = colors[c + 1];
      output[dst + 2] = colors[c + 2];
      output[dst + 3] = alpha[a];
    }
  }
}
export function decodeBC3Blocks(input) {
  const count = blockCount(input);
  const rgba = new Uint8Array(count * 64);
  decode(input, count, rgba, 0);
  return rgba;
}
// 直接写入拼接图像，不再生成完整的块序 RGBA 中间副本；末行空块填零。
// Write the stitched image directly without a full block-order RGBA intermediate; zero-pad the last row.
export function decodeBC3BlockGrid(input, { rowBlocks = 1024 } = {}) {
  const count = blockCount(input);
  if (!Number.isSafeInteger(rowBlocks) || rowBlocks < 1 || rowBlocks > MAX_DIMENSION / 4)
    throw new RangeError("Invalid BC3 grid row width");
  const rows = Math.ceil(count / rowBlocks),
    width = rowBlocks * 4,
    height = rows * 4;
  if (height > MAX_DIMENSION || width * height > MAX_PIXELS)
    throw new RangeError("Padded BC3 grid exceeds primitive image bounds");
  const rgba = new Uint8Array(width * height * 4);
  decode(input, count, rgba, rowBlocks);
  return {
    rgba: rgba.buffer,
    width,
    height,
    blockCount: count,
    rowBlocks,
    paddedBlocks: rows * rowBlocks - count,
  };
}
