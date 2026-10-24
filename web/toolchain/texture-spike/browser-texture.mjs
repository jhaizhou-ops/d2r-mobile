// 包装纹理解码器及其错误边界；依据 web/toolchain/texture-spike 的模块与调用约定。
// Wrap the texture decoder and its error boundary using the module and calling contracts in web/toolchain/texture-spike.
const CAP = 64 * 1024 * 1024;
const fields = [
  "target",
  "format",
  "flags",
  "samples",
  "width",
  "height",
  "depth",
  "levels",
  "stride",
  "pkg_block_h",
];
const errors = {
  [-1]: "Invalid texture dimensions, mip, stride or packaged layout",
  [-2]: "Unsupported texture format (only NVN BC1–BC7 are supported)",
  [-3]: "Unsupported texture shape, flags, layers or linear mip layout",
  [-4]: "Texture allocation limit exceeded",
  [-5]: "Texture source or destination length mismatch",
};
export class TextureError extends Error {
  constructor(code, message = errors[code]) {
    super(message ?? "Texture decoding failed");
    this.name = "TextureError";
    this.code = code;
  }
}
function check(code) {
  if (code) throw new TextureError(code);
}
function descriptor(value) {
  if (!value || typeof value !== "object") throw new TextureError(-1);
  if (Object.keys(value).some((key) => !fields.includes(key)))
    throw new TextureError(-1, "Unknown descriptor field");
  const words = fields.map((key) => value[key]);
  if (words.some((word) => !Number.isInteger(word) || word < 0 || word > 0x7fffffff))
    throw new TextureError(-1);
  return new Uint32Array(words);
}
/**
 * 每个专用线程实例化一次，不使用文件、密钥、网络或 DOM。
 * Instantiate once per dedicated worker; no files, keys, network or DOM are used. */
export async function createTextureDecoder(createModule, options = {}) {
  const module = await createModule(options);
  function allocation(size) {
    if (!Number.isInteger(size) || size < 1 || size > CAP) throw new TextureError(-4);
    const pointer = module._malloc(size);
    if (!pointer) throw new TextureError(-4);
    return pointer;
  }
  function withInfo(desc, level, layer, callback) {
    const words = descriptor(desc);
    if (
      !Number.isInteger(level) ||
      level < 0 ||
      level > 13 ||
      !Number.isInteger(layer) ||
      layer !== 0
    )
      throw new TextureError(-1);
    const ptr = allocation(40);
    let info = 0;
    try {
      info = allocation(32);
      module.HEAPU32.set(words, ptr / 4);
      check(module._texture_info(ptr, level, layer, info));
      const m = module.HEAPU32.slice(info / 4, info / 4 + 8);
      return callback(ptr, m);
    } finally {
      module._free(info);
      module._free(ptr);
    }
  }
  function layout(desc, { level = 0, layer = 0 } = {}) {
    return withInfo(desc, level, layer, (_, m) => ({
      storageBytes: m[0],
      compressedBytes: m[1],
      width: m[2],
      height: m[3],
      pixelFormat: m[4],
      bytesPerPixel: m[5],
      rowBytes: m[6],
      rows: m[7],
    }));
  }
  function decodeWith(decoder, desc, source, { level = 0, layer = 0 } = {}) {
    if (
      !(source instanceof Uint8Array) ||
      source.byteLength > CAP ||
      !(source.buffer instanceof ArrayBuffer)
    )
      throw new TextureError(-5);
    return withInfo(desc, level, layer, (ptr, m) => {
      if (source.byteLength !== m[0]) throw new TextureError(-5);
      let input = 0,
        output = 0;
      try {
        input = allocation(source.byteLength);
        output = allocation(m[2] * m[3] * m[5]);
        module.HEAPU8.set(source, input);
        check(decoder(ptr, level, layer, input, source.byteLength, output, m[2] * m[3] * m[5]));
        return {
          pixels: module.HEAPU8.slice(output, output + m[2] * m[3] * m[5]),
          width: m[2],
          height: m[3],
          pixelFormat: m[4],
          bytesPerPixel: m[5],
        };
      } finally {
        module._free(output);
        module._free(input);
      }
    });
  }
  const decode = (desc, source, options) =>
    decodeWith(module._texture_decode, desc, source, options);
  const decodePillowBc3 = (desc, source, options) =>
    decodeWith(module._texture_decode_pillow_bc3, desc, source, options);
  function decodeV61(bytes) {
    if (!(bytes instanceof Uint8Array) || bytes.length < 40 || bytes.length > CAP + 40)
      throw new TextureError(-5);
    const header = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (header.getUint32(0, true) !== 0x31417053 || header.getUint16(4, true) !== 61)
      throw new TextureError(-2, "Expected SpA1 sprite version 61");
    const width = header.getUint32(8, true),
      height = header.getUint32(12, true);
    const frameWidth = header.getUint16(6, true);
    if (!frameWidth || frameWidth > width || width % frameWidth !== 0)
      throw new TextureError(-1, "Invalid horizontal sprite frame width");
    const result = decode(
      {
        target: 1,
        format: 0x44,
        flags: 0,
        samples: 0,
        width,
        height,
        depth: 1,
        levels: 1,
        stride: 0,
        pkg_block_h: bytes[23] + 1,
      },
      bytes.subarray(40),
    );
    return { ...result, frameWidth };
  }
  return Object.freeze({ layout, decode, decodePillowBc3, decodeV61 });
}
