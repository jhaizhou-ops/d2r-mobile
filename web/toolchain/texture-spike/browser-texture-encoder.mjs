// 验证纹理编码请求并包装编码器；依据 ./browser-texture.mjs 的模块与调用约定。
// Validate texture-encoding requests and wrap the encoder using the module and calling contracts in ./browser-texture.mjs.
import { createTextureDecoder, TextureError } from "./browser-texture.mjs";
export function validateEncodeRequest(header, width, height) {
  if (
    !(header instanceof Uint8Array) ||
    header.byteLength !== 40 ||
    !(header.buffer instanceof ArrayBuffer)
  )
    throw new TextureError(-5, "Expected exact 40-byte source header");
  if (
    ![width, height].every((v) => Number.isSafeInteger(v) && v > 0 && v <= 8192) ||
    width * height > 4 * 1024 * 1024
  )
    throw new TextureError(-4, "Encoder dimensions exceed 4M-pixel limit");
  const view = new DataView(header.buffer, header.byteOffset, 40);
  if (view.getUint32(0, true) !== 0x31417053 || view.getUint16(4, true) !== 61)
    throw new TextureError(-2, "Expected original SpA1 v61 header");
  const fw = view.getUint16(6, true),
    originalWidth = view.getUint32(8, true),
    originalHeight = view.getUint32(12, true);
  if (
    view.getUint16(20, true) !== 1 ||
    !fw ||
    originalWidth < fw ||
    originalWidth > Math.ceil(fw / 4) * 4 ||
    originalWidth > 8192 ||
    !originalHeight ||
    originalHeight > 8192 ||
    originalWidth * originalHeight > 4 * 1024 * 1024 ||
    header[23] > 5
  )
    throw new TextureError(-3, "Only bounded single-frame v61 templates are supported");
  return {
    target: 1,
    format: 0x44,
    flags: 0,
    samples: 0,
    width,
    height,
    depth: 1,
    levels: 1,
    stride: 0,
    pkg_block_h: header[23] + 1,
  };
}
/**
 * encodeRGBA 实现固定源码的 BC3 算法，生产使用有界 Rust/WASM。
 * encodeRGBA implements the source-pinned BC3 algorithm (production: bounded Rust/WASM). */
export async function createTextureEncoder(createModule, encodeRGBA, options = {}) {
  const module = await createModule(options);
  const decoder = await createTextureDecoder(async () => module);
  function packLinearBC3(templateHeader, linearBC3, width, height) {
    const desc = validateEncodeRequest(templateHeader, width, height);
    const layout = decoder.layout(desc);
    if (
      !(linearBC3 instanceof Uint8Array) ||
      !(linearBC3.buffer instanceof ArrayBuffer) ||
      linearBC3.byteLength !== layout.compressedBytes
    )
      throw new TextureError(-5);
    const allocations = [];
    function alloc(size) {
      const p = module._malloc(size);
      if (!p) throw new TextureError(-4);
      allocations.push(p);
      return p;
    }
    try {
      const descriptor = alloc(40),
        input = alloc(linearBC3.length),
        output = alloc(layout.storageBytes);
      module.HEAPU32.set(Object.values(desc), descriptor / 4);
      module.HEAPU8.set(linearBC3, input);
      const status = module._texture_pack_bc3(
        descriptor,
        input,
        linearBC3.length,
        output,
        layout.storageBytes,
      );
      if (status) throw new TextureError(status);
      const sprite = new Uint8Array(40 + layout.storageBytes);
      sprite.set(templateHeader);
      const header = new DataView(sprite.buffer);
      header.setUint16(6, width, true);
      header.setUint32(8, width, true);
      header.setUint32(12, height, true);
      header.setUint16(20, 1, true);
      header.setUint32(32, layout.storageBytes, true);
      sprite.set(module.HEAPU8.subarray(output, output + layout.storageBytes), 40);
      return { sprite, width, height };
    } finally {
      for (const p of allocations) module._free(p);
    }
  }
  async function encodeAndPack(templateHeader, rgba, width, height) {
    validateEncodeRequest(templateHeader, width, height);
    if (
      !(rgba instanceof Uint8Array) ||
      !(rgba.buffer instanceof ArrayBuffer) ||
      rgba.byteLength !== width * height * 4
    )
      throw new TextureError(-5);
    const linearBC3 = await encodeRGBA(rgba, width, height);
    try {
      return { ...packLinearBC3(templateHeader, linearBC3, width, height), linearBC3 };
    } catch (error) {
      // 打包失败时擦除独立编码结果，但不修改注入编码器借用的调用方输入视图。
      // Wipe independent encoded output on packing failure, never aliases of caller inputs.
      const bytes =
        linearBC3 instanceof Uint8Array
          ? linearBC3
          : linearBC3 instanceof ArrayBuffer
            ? new Uint8Array(linearBC3)
            : null;
      if (
        bytes?.byteLength &&
        bytes.buffer !== rgba.buffer &&
        bytes.buffer !== templateHeader.buffer
      )
        bytes.fill(0);
      throw error;
    }
  }
  return Object.freeze({ encodeAndPack, packLinearBC3 });
}
