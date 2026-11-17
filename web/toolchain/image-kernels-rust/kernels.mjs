// 仅处理像素的有界 WASM API；不使用浏览器图像解码器或 Canvas。
// Pixel-only, bounded WASM API. No browser image decoder or Canvas participates.
export const MAX_PIXELS = 4_194_304;
const MAX_SIDE = 8192;
function integer(n, name, low = 0, high = MAX_SIDE) {
  if (!Number.isInteger(n) || n < low || n > high) throw new RangeError(`Invalid ${name}`);
  return n;
}
function image(value) {
  if (!value || ![1, 3, 4].includes(value.channels))
    throw new TypeError("Expected L/RGB/RGBA image");
  integer(value.width, "width", 1);
  integer(value.height, "height", 1);
  if (value.width * value.height > MAX_PIXELS) throw new RangeError("Image pixel limit");
  if (
    !(value.data instanceof Uint8Array) ||
    value.data.length !== value.width * value.height * value.channels
  )
    throw new TypeError("Image byte length mismatch");
  return value;
}
export async function createImageKernels(bytes) {
  const module = bytes instanceof WebAssembly.Module ? bytes : await WebAssembly.compile(bytes);
  if (WebAssembly.Module.imports(module).length)
    throw new Error("Image kernel must not import host functions");
  const { exports: wasm } = await WebAssembly.instantiate(module);
  const write = (at, bytes) => new Uint8Array(wasm.memory.buffer, at, bytes.length).set(bytes);
  function put(value, slot = "image_input") {
    image(value);
    write(wasm[slot](), value.data);
  }
  function result(count, width, height, channels) {
    if (count < 0) throw new RangeError("Image kernel rejected operation geometry/options");
    if (count !== width * height * channels) throw new Error("Image kernel result length mismatch");
    return {
      width,
      height,
      channels,
      data: new Uint8Array(wasm.memory.buffer, wasm.image_output(), count).slice(),
    };
  }
  function coordinates(x, y) {
    integer(x, "x", -2147483648, 2147483647);
    integer(y, "y", -2147483648, 2147483647);
  }
  function outputSize(w, h) {
    integer(w, "output width", 1);
    integer(h, "output height", 1);
    if (w * h > MAX_PIXELS) throw new RangeError("Image pixel limit");
  }
  return Object.freeze({
    crop(src, x, y, width, height) {
      coordinates(x, y);
      outputSize(width, height);
      put(src);
      return result(
        wasm.image_crop(src.width, src.height, src.channels, x, y, width, height),
        width,
        height,
        src.channels,
      );
    },
    flip(src, axis) {
      integer(axis, "axis", 0, 1);
      put(src);
      return result(
        wasm.image_flip(src.width, src.height, src.channels, axis),
        src.width,
        src.height,
        src.channels,
      );
    },
    paste(dst, src, x, y, mask = null) {
      coordinates(x, y);
      image(dst);
      image(src);
      if (src.channels !== dst.channels) throw new TypeError("Paste requires matching modes");
      let kind = 0;
      if (mask === "source-alpha") {
        if (src.channels !== 4) throw new TypeError("Source alpha requires RGBA");
        kind = 2;
      } else if (mask !== null) {
        image(mask);
        if (mask.channels !== 1 || mask.width !== src.width || mask.height !== src.height)
          throw new TypeError("Paste mask must be source-sized L");
        write(wasm.image_mask(), mask.data);
        kind = 1;
      }
      put(src);
      put(dst, "image_destination");
      return result(
        wasm.image_paste(src.width, src.height, dst.width, dst.height, src.channels, x, y, kind),
        dst.width,
        dst.height,
        dst.channels,
      );
    },
    alphaComposite(dst, src, x = 0, y = 0) {
      coordinates(x, y);
      image(dst);
      image(src);
      if (src.channels !== 4 || dst.channels !== 4)
        throw new TypeError("Alpha composite requires RGBA");
      put(src);
      put(dst, "image_destination");
      return result(
        wasm.image_alpha_composite(src.width, src.height, dst.width, dst.height, x, y),
        dst.width,
        dst.height,
        4,
      );
    },
    convert(src, channels) {
      if (![1, 3, 4].includes(channels)) throw new TypeError("Invalid output mode");
      put(src);
      return result(
        wasm.image_convert(src.width, src.height, src.channels, channels),
        src.width,
        src.height,
        channels,
      );
    },
    channel(src, channel) {
      image(src);
      integer(channel, "channel", 0, src.channels - 1);
      put(src);
      return result(
        wasm.image_channel(src.width, src.height, src.channels, channel),
        src.width,
        src.height,
        1,
      );
    },
    point(src, lut) {
      image(src);
      if (src.channels !== 1 || !(lut instanceof Uint8Array) || lut.length !== 256)
        throw new TypeError("Point requires L and 256-byte LUT");
      put(src);
      write(wasm.image_mask(), lut);
      return result(wasm.image_channel(src.width, src.height, 1, 4), src.width, src.height, 1);
    },
    putAlpha(src, mask) {
      image(src);
      image(mask);
      if (
        src.channels !== 4 ||
        mask.channels !== 1 ||
        mask.width !== src.width ||
        mask.height !== src.height
      )
        throw new TypeError("PutAlpha requires RGBA and matching L mask");
      put(src);
      write(wasm.image_mask(), mask.data);
      return result(wasm.image_put_alpha(src.width, src.height), src.width, src.height, 4);
    },
    bbox(src, alphaOnly = true) {
      if (typeof alphaOnly !== "boolean") throw new TypeError("Invalid alphaOnly");
      put(src);
      const status = wasm.image_bbox(src.width, src.height, src.channels, Number(alphaOnly));
      if (status < 0) throw new RangeError("BBox rejected image");
      return status
        ? Array.from(new Uint32Array(wasm.memory.buffer, wasm.image_bbox_result(), 4))
        : null;
    },
    maxFilter(src, size = 5) {
      image(src);
      if (src.channels !== 1 || ![3, 5].includes(size))
        throw new TypeError("MaxFilter supports L, size 3 or 5");
      put(src);
      return result(wasm.image_max_filter(src.width, src.height, size), src.width, src.height, 1);
    },
    resizeLanczos(src, width, height) {
      outputSize(width, height);
      image(src);
      if (width * src.height > MAX_PIXELS) throw new RangeError("Resize intermediate pixel limit");
      put(src);
      return result(
        wasm.image_resize_lanczos(src.width, src.height, src.channels, width, height),
        width,
        height,
        src.channels,
      );
    },
  });
}
