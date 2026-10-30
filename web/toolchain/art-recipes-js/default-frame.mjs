// 原版 v31 默认帧配方；复用已迁移的默认态像素运算和精确 PNG 编码。 | Original v31 default-frame recipe using the migrated default-state kernel and exact PNG encoder.
import { payloadBytes, requireValue, png } from "./util.mjs";
export function encodeV31(im) {
  requireValue(im.width <= 65535, "Frame width exceeds v31 uint16");
  const out = new Uint8Array(40 + im.data.length);
  const view = new DataView(out.buffer);
  out.set([83, 112, 65, 49]);
  view.setUint16(4, 31, true);
  view.setUint16(6, im.width, true);
  view.setUint32(8, im.width, true);
  view.setUint32(12, im.height, true);
  view.setUint32(20, 1, true);
  view.setUint32(32, im.width * im.height * 4, true);
  view.setUint32(36, 4, true);
  out.set(im.data, 40);
  return out;
}
export function generateDefaultFrame(payload, options, { codec, kernels, defaultState }) {
  payloadBytes(payload, 20 * 1024 * 1024, "Input exceeds operation byte limit");
  requireValue(
    payload.length >= 40 &&
      payload[0] === 83 &&
      payload[1] === 112 &&
      payload[2] === 65 &&
      payload[3] === 49,
    "Invalid SpA1 header",
  );
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const version = view.getUint16(4, true),
    frameWidth = view.getUint16(6, true),
    width = view.getUint32(8, true),
    height = view.getUint32(12, true);
  requireValue(version !== 61, "UNSUPPORTED_V61: NVN deswizzle/BC3 bridge is not implemented");
  requireValue(version === 31, "Unsupported sprite version");
  requireValue(
    width > 0 && height > 0 && width * height <= 4_194_304,
    "Invalid or oversized dimensions",
  );
  requireValue(frameWidth > 0 && width % frameWidth === 0, "Invalid frame width");
  requireValue(payload.length === 40 + width * height * 4, "Truncated sprite or trailing payload");
  requireValue(
    view.getUint32(32, true) === width * height * 4 && view.getUint32(36, true) === 4,
    "Invalid RGBA byte count or stride",
  );
  const index = Object.hasOwn(options, "frame") ? options.frame : 0;
  requireValue(
    Number.isInteger(index) && index >= 0 && index < width / frameWidth,
    "Frame index out of range",
  );
  const crop = kernels.crop(
    { width, height, channels: 4, data: payload.subarray(40) },
    index * frameWidth,
    0,
    frameWidth,
    height,
  );
  const im = { ...crop, data: defaultState(crop.data, crop.width, crop.height) };
  return {
    width: im.width,
    height: im.height,
    files: { "frame.rgba": im.data, "frame.png": png(codec, im), "frame.sprite": encodeV31(im) },
    versions: { pillow: "10.2.0" },
  };
}
