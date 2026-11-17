// 仅在本地解码有界、不透明 PNG，生成普通图标文件；不会修改编译后的 Assets.car。
// Decode bounded opaque PNGs locally into loose icon files; never modify compiled Assets.car.
import { createDeflateBoundary } from "./deflate-boundary.mjs";
export const CUSTOM_ICON_LIMITS = Object.freeze({
  bytes: 4 * 1024 * 1024,
  minSide: 256,
  maxSide: 1024,
});
const fail = () => {
  throw new Error("APP_CUSTOMIZATION_ICON_INVALID");
};
const requireValue = (ok) => {
  if (!ok) fail();
};
const signature = new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]);
const table = Uint32Array.from({ length: 256 }, (_, n) => {
  for (let i = 0; i < 8; i++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
const crc = (bytes) => {
  let n = 0xffffffff;
  for (const b of bytes) n = table[(n ^ b) & 255] ^ (n >>> 8);
  return (n ^ 0xffffffff) >>> 0;
};
const u32 = (bytes, at) => new DataView(bytes.buffer, bytes.byteOffset, bytes.length).getUint32(at);
const put = (bytes, at, value) =>
  new DataView(bytes.buffer, bytes.byteOffset, bytes.length).setUint32(at, value);
function concat(parts) {
  const out = new Uint8Array(parts.reduce((n, b) => n + b.length, 0));
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}
function inspect(bytes) {
  requireValue(
    bytes.length <= CUSTOM_ICON_LIMITS.bytes &&
      bytes.length >= 57 &&
      signature.every((n, i) => bytes[i] === n),
  );
  let at = 8,
    side,
    channels,
    done = false,
    started = false,
    ended = false,
    count = 0;
  const compressed = [];
  while (at < bytes.length) {
    requireValue(!done && ++count <= 4096 && at + 12 <= bytes.length);
    const size = u32(bytes, at),
      type = String.fromCharCode(...bytes.subarray(at + 4, at + 8));
    requireValue(
      size <= bytes.length - at - 12 &&
        /^[A-Za-z]{4}$/.test(type) &&
        crc(bytes.subarray(at + 4, at + 8 + size)) === u32(bytes, at + 8 + size),
    );
    const data = bytes.subarray(at + 8, at + 8 + size);
    if (type === "IHDR") {
      requireValue(at === 8 && size === 13);
      side = u32(data, 0);
      channels = data[9] === 2 ? 3 : data[9] === 6 ? 4 : 0;
      requireValue(
        side >= CUSTOM_ICON_LIMITS.minSide &&
          side <= CUSTOM_ICON_LIMITS.maxSide &&
          side === u32(data, 4) &&
          data[8] === 8 &&
          channels &&
          !data[10] &&
          !data[11] &&
          !data[12],
      );
    } else {
      requireValue(side !== undefined);
      if (type === "IDAT") {
        requireValue(!ended);
        started = true;
        compressed.push(data);
      } else {
        if (started) ended = true;
        if (type === "IEND") {
          requireValue(started && size === 0);
          done = true;
        } else
          requireValue(
            [
              "cHRM",
              "gAMA",
              "sRGB",
              "pHYs",
              "tEXt",
              "zTXt",
              "iTXt",
              "eXIf",
              "iCCP",
              "sBIT",
              "bKGD",
              "tIME",
            ].includes(type),
          );
      }
    }
    at += size + 12;
  }
  requireValue(done);
  return { side, channels, compressed: concat(compressed) };
}
async function inflate(encoded, size, signal) {
  requireValue(
    encoded.length >= 6 &&
      (encoded[0] & 15) === 8 &&
      encoded[0] >> 4 <= 7 &&
      ((encoded[0] << 8) | encoded[1]) % 31 === 0 &&
      !(encoded[1] & 32),
  );
  const boundary = createDeflateBoundary(size);
  for (let at = 2; at < encoded.length - 4; at += 1024) {
    signal?.throwIfAborted();
    boundary.push(encoded.subarray(at, Math.min(at + 1024, encoded.length - 4)));
  }
  boundary.finish();
  const stream = new DecompressionStream("deflate"),
    reader = stream.readable.getReader(),
    writer = stream.writable.getWriter(),
    output = new Uint8Array(size);
  let failure, disposal;
  const stop = (error) => {
    if (!failure) {
      failure = error;
      disposal = Promise.allSettled([reader.cancel(error), writer.abort(error)]);
    }
  };
  const abort = () => stop(signal.reason);
  const check = () => {
    signal?.throwIfAborted();
    if (failure) throw failure;
  };
  signal?.addEventListener("abort", abort, { once: true });
  const producer = (async () => {
    try {
      for (let at = 0; at < encoded.length; at += 1024) {
        check();
        await writer.write(encoded.subarray(at, at + 1024));
      }
      await writer.close();
    } catch (error) {
      stop(error);
      throw error;
    }
  })();
  const consumer = (async () => {
    let at = 0;
    try {
      while (true) {
        check();
        const { done, value } = await reader.read();
        check();
        if (done) break;
        requireValue(value.length <= size - at);
        output.set(value, at);
        at += value.length;
      }
      requireValue(at === size);
    } catch (error) {
      stop(error);
      throw error;
    }
  })();
  try {
    await Promise.all([producer, consumer]);
    check();
    return output;
  } finally {
    await Promise.allSettled([producer, consumer]);
    await disposal;
    signal?.removeEventListener("abort", abort);
    reader.releaseLock();
    writer.releaseLock();
  }
}
const paeth = (a, b, c) => {
  const p = a + b - c,
    x = Math.abs(p - a),
    y = Math.abs(p - b),
    z = Math.abs(p - c);
  return x <= y && x <= z ? a : y <= z ? b : c;
};
function unfilter(bytes, side, channels, signal) {
  const stride = side * channels,
    pixels = new Uint8Array(stride * side);
  for (let y = 0; y < side; y++) {
    signal?.throwIfAborted();
    const filter = bytes[y * (stride + 1)];
    requireValue(filter <= 4);
    for (let x = 0; x < stride; x++) {
      const at = y * stride + x,
        a = x >= channels ? pixels[at - channels] : 0,
        b = y ? pixels[at - stride] : 0,
        c = y && x >= channels ? pixels[at - stride - channels] : 0;
      pixels[at] =
        bytes[y * (stride + 1) + x + 1] + [0, a, b, (a + b) >> 1, paeth(a, b, c)][filter];
      if (channels === 4 && x % 4 === 3) requireValue(pixels[at] === 255);
    }
  }
  return pixels;
}
function chunk(type, data) {
  const out = new Uint8Array(data.length + 12);
  put(out, 0, data.length);
  out.set(new TextEncoder().encode(type), 4);
  out.set(data, 8);
  put(out, data.length + 8, crc(out.subarray(4, data.length + 8)));
  return out;
}
function pngRgb(pixels, side) {
  const raw = new Uint8Array(side * (side * 3 + 1));
  for (let y = 0; y < side; y++)
    raw.set(pixels.subarray(y * side * 3, (y + 1) * side * 3), y * (side * 3 + 1) + 1);
  const parts = [new Uint8Array([0x78, 1])];
  for (let at = 0; at < raw.length; at += 65535) {
    const size = Math.min(65535, raw.length - at);
    parts.push(
      new Uint8Array([
        at + size === raw.length ? 1 : 0,
        size & 255,
        size >> 8,
        ~size & 255,
        (~size >> 8) & 255,
      ]),
      raw.subarray(at, at + size),
    );
  }
  let a = 1,
    b = 0;
  for (const n of raw) {
    a = (a + n) % 65521;
    b = (b + a) % 65521;
  }
  const adler = new Uint8Array(4);
  put(adler, 0, ((b << 16) | a) >>> 0);
  parts.push(adler);
  const header = new Uint8Array(13);
  put(header, 0, side);
  put(header, 4, side);
  header[8] = 8;
  header[9] = 2;
  return new Blob(
    [
      signature,
      chunk("IHDR", header),
      chunk("IDAT", concat(parts)),
      chunk("IEND", new Uint8Array()),
    ],
    { type: "image/png" },
  );
}
// 双线性缩放；输出去掉元数据及透明通道，不调用 Canvas、浏览器解码器或远程服务。
// Bilinear resizing drops metadata and alpha; no Canvas, browser image decoder or remote service.
function resize(pixels, side, channels, size, signal) {
  const out = new Uint8Array(size * size * 3);
  for (let y = 0; y < size; y++) {
    signal?.throwIfAborted();
    const fy = Math.max(0, ((y + 0.5) * side) / size - 0.5),
      y0 = Math.floor(fy),
      y1 = Math.min(side - 1, y0 + 1),
      ty = fy - y0;
    for (let x = 0; x < size; x++) {
      const fx = Math.max(0, ((x + 0.5) * side) / size - 0.5),
        x0 = Math.floor(fx),
        x1 = Math.min(side - 1, x0 + 1),
        tx = fx - x0;
      for (let c = 0; c < 3; c++) {
        const top =
            pixels[(y0 * side + x0) * channels + c] * (1 - tx) +
            pixels[(y0 * side + x1) * channels + c] * tx,
          bottom =
            pixels[(y1 * side + x0) * channels + c] * (1 - tx) +
            pixels[(y1 * side + x1) * channels + c] * tx;
        out[(y * size + x) * 3 + c] = Math.round(top * (1 - ty) + bottom * ty);
      }
    }
  }
  return pngRgb(out, size);
}
export const CUSTOM_ICON_FILES = Object.freeze(
  [
    ["D2RCustomIcon20@2x.png", 40, "phone"],
    ["D2RCustomIcon20@3x.png", 60, "phone"],
    ["D2RCustomIcon29@2x.png", 58, "phone"],
    ["D2RCustomIcon29@3x.png", 87, "phone"],
    ["D2RCustomIcon40@2x.png", 80, "phone"],
    ["D2RCustomIcon40@3x.png", 120, "phone"],
    ["D2RCustomIcon60@2x.png", 120, "phone"],
    ["D2RCustomIcon60@3x.png", 180, "phone"],
    ["D2RCustomIcon20~ipad.png", 20, "pad"],
    ["D2RCustomIcon29~ipad.png", 29, "pad"],
    ["D2RCustomIcon40~ipad.png", 40, "pad"],
    ["D2RCustomIcon76~ipad.png", 76, "pad"],
    ["D2RCustomIcon76@2x~ipad.png", 152, "pad"],
    ["D2RCustomIcon83.5@2x~ipad.png", 167, "pad"],
  ].map(Object.freeze),
);
export async function prepareCustomizationIcons(blob, { signal } = {}) {
  try {
    signal?.throwIfAborted();
    requireValue(blob instanceof Blob && blob.size > 0 && blob.size <= CUSTOM_ICON_LIMITS.bytes);
    const { side, channels, compressed } = inspect(new Uint8Array(await blob.arrayBuffer()));
    signal?.throwIfAborted();
    const filtered = await inflate(compressed, side * (side * channels + 1), signal);
    const pixels = unfilter(filtered, side, channels, signal);
    return Object.freeze(
      CUSTOM_ICON_FILES.map(([path, size]) =>
        Object.freeze({ path, blob: resize(pixels, side, channels, size, signal) }),
      ),
    );
  } catch {
    signal?.throwIfAborted();
    fail();
  }
}
