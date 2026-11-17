// 共享边界；在转移所有权前及工作线程内再次验证。
// Shared bounds; validate before ownership transfer and again in the worker.
// 基于 bridge.cpp 的 RGBA8 契约与分配上限。
// Based on the bridge.cpp RGBA8 contract and allocation limits.
export const MAX_PIXELS = 1048576;
export const MAX_DIMENSION = 4096;
export function validateImage({ rgba, width, height, profile = "linear", preset = "thorough" }) {
  if (
    !Number.isSafeInteger(width) ||
    !Number.isSafeInteger(height) ||
    width < 1 ||
    height < 1 ||
    width > MAX_DIMENSION ||
    height > MAX_DIMENSION ||
    width * height > MAX_PIXELS
  )
    throw new RangeError("Image exceeds ASTC bounds");
  if (!(rgba instanceof ArrayBuffer) || rgba.resizable || rgba.byteLength !== width * height * 4)
    throw new TypeError("Expected one exact, fixed-length owned RGBA8 ArrayBuffer");
  if (profile !== "linear" && profile !== "srgb") throw new TypeError("Unknown ASTC profile");
  if (preset !== "thorough" && preset !== "fast") throw new TypeError("Unknown ASTC preset");
  return Math.ceil(width / 4) * Math.ceil(height / 4) * 16;
}

// 一次复制到 WASM，一次紧凑输出复制；不使用 PNG 或虚拟文件系统。
// One WASM copy in, one compact output copy out; no PNG or virtual filesystem.
export function encodeWithModule(module, input) {
  const size = validateImage(input);
  if (module._astc_encoder_abi_version?.() !== 2) throw new Error("ASTC encoder ABI mismatch");
  let src = 0,
    dst = 0;
  try {
    src = module._malloc(input.rgba.byteLength);
    if (!src) throw new Error("ASTC input allocation failed");
    dst = module._malloc(size);
    if (!dst) throw new Error("ASTC output allocation failed");
    module.HEAPU8.set(new Uint8Array(input.rgba), src);
    const result = module._astc_encode_rgba(
      src,
      input.rgba.byteLength,
      input.width,
      input.height,
      input.profile === "srgb" ? 1 : 0,
      dst,
      size,
      input.preset === "fast" ? 1 : 0,
    );
    if (result !== 0) throw new Error(`ASTC encoder failed (${result})`);
    return module.HEAPU8.slice(dst, dst + size).buffer;
  } finally {
    if (dst) module._free(dst);
    if (src) module._free(src);
  }
}
