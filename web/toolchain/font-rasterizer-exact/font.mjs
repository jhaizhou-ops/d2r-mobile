// 供生产资源运行时使用的隔离、有界字体光栅化封装。
// Isolated, bounded font rasterizer wrapper used by the production asset runtime.
const FONT_LIMIT = 16 * 1024 * 1024,
  PIXELS = 4 * 1024 * 1024,
  SIDE = 8192;
const anchors = { la: 0, ls: 1, mb: 2, mm: 3 };
const integer = (value, min, max, name) => {
  if (!Number.isInteger(value) || value < min || value > max) throw new RangeError(name);
  return value;
};
const bytes = (value, length, name) => {
  if (!(value instanceof Uint8Array) || value.length !== length) throw new TypeError(name);
  return value;
};
const packed = (value, channels) => {
  const rgba = channels === 1 ? [value] : value;
  if (!Array.isArray(rgba) || rgba.length !== channels) throw new TypeError("Ink channels");
  return (
    rgba.reduce((n, channel, i) => n | (integer(channel, 0, 255, "Ink byte") << (i * 8)), 0) >>> 0
  );
};
export async function createFontRasterizer(wasm) {
  const module = wasm instanceof WebAssembly.Module ? wasm : await WebAssembly.compile(wasm);
  if (WebAssembly.Module.imports(module).length) throw new Error("Unexpected font WASM imports");
  const instance = await WebAssembly.instantiate(module, {});
  const e = instance.exports;
  e._initialize?.();
  if (e.font_version() !== 20600 || e.memory.buffer.byteLength !== 67108864)
    throw new Error("Unexpected font backend");
  let opened = false;
  const check = (code) => {
    if (code) throw new Error(`Font rasterizer error ${code} (FreeType ${e.font_ft_error()})`);
  };
  const info = () => Array.from(new Int32Array(e.memory.buffer, e.font_info(), 7));
  const prepare = (text, anchor, stroke) => {
    if (!opened || typeof text !== "string") throw new TypeError("Open a font and supply text");
    if (!Object.hasOwn(anchors, anchor)) throw new RangeError("Supported anchors: la, ls, mb, mm");
    integer(stroke, 0, 2, "Stroke width");
    // 每个 Unicode 码点最多占两个 UTF-16 单元；先拒绝明显超限输入，避免展开大数组。
    // Each Unicode codepoint uses at most two UTF-16 units; reject oversized input before array expansion.
    if (text.length > 4096) throw new RangeError("Single-line Unicode text limit");
    const points = Array.from(text, (char) => char.codePointAt(0));
    if (
      points.length > 2048 ||
      points.some((n) => n === 10 || n === 13 || (n >= 0xd800 && n <= 0xdfff))
    )
      throw new RangeError("Single-line Unicode text limit");
    new Uint32Array(e.memory.buffer, e.font_text(), points.length).set(points);
    return [points.length, anchors[anchor], stroke];
  };
  return {
    open(fontBytes, size) {
      if (
        !(fontBytes instanceof Uint8Array) ||
        !fontBytes.length ||
        fontBytes.length > FONT_LIMIT ||
        !Number.isFinite(size) ||
        size < 1 ||
        size > 256
      )
        throw new RangeError("Font size or byte limit");
      e.font_close();
      opened = false;
      new Uint8Array(e.memory.buffer, e.font_input(), fontBytes.length).set(fontBytes);
      check(e.font_open(fontBytes.length, size));
      opened = true;
    },
    close() {
      e.font_close();
      opened = false;
    },
    measure(text, { anchor = "la", stroke = 0 } = {}) {
      check(e.font_measure(...prepare(text, anchor, stroke)));
      const [width, height, x, y, advance, ascent, descent] = info();
      return {
        width,
        height,
        offset: [x, y],
        bbox: [x, y, x + width, y + height],
        length: advance / 64,
        metrics: [ascent, descent],
      };
    },
    mask(text, { anchor = "la", stroke = 0, start = [0, 0] } = {}) {
      if (
        !Array.isArray(start) ||
        start.length !== 2 ||
        start.some((n) => !Number.isFinite(n) || Math.abs(n) >= 1)
      )
        throw new RangeError("Fractional text start");
      check(e.font_render(...prepare(text, anchor, stroke), ...start));
      const [width, height, x, y] = info();
      return {
        width,
        height,
        offset: [x, y],
        pixels: new Uint8Array(e.memory.buffer, e.font_mask(), width * height).slice(),
      };
    },
    draw(
      pixels,
      width,
      height,
      text,
      { x, y, fill, strokeFill = fill, stroke = 0, anchor = "la", channels = 4 } = {},
    ) {
      integer(width, 0, SIDE, "Canvas width");
      integer(height, 0, SIDE, "Canvas height");
      if (width * height > PIXELS || (channels !== 1 && channels !== 4))
        throw new RangeError("Canvas limit/mode");
      if (![x, y].every((n) => Number.isFinite(n) && Math.abs(n) <= SIDE * 2))
        throw new RangeError("Draw position");
      bytes(pixels, width * height * channels, "Canvas byte length");
      const params = prepare(text, anchor, stroke),
        ink = packed(fill, channels),
        outline = packed(strokeFill, channels);
      new Uint8Array(e.memory.buffer, e.font_canvas(), pixels.length).set(pixels);
      check(e.font_draw(...params, x, y, width, height, channels, ink, outline));
      return new Uint8Array(e.memory.buffer, e.font_canvas(), pixels.length).slice();
    },
  };
}
