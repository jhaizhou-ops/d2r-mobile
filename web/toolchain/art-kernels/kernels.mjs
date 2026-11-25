// 固定来源美术配方的数值移植；Pillow 负责栅格输入输出及绘制。
// Numeric ports of the source-pinned art recipes. Pillow owns raster I/O and drawing.
const MAX_PIXELS = 16 * 1024 * 1024;
function image(bytes, width, height, channels = 4) {
  if (
    !Number.isSafeInteger(width) ||
    !Number.isSafeInteger(height) ||
    width < 1 ||
    height < 1 ||
    width * height > MAX_PIXELS ||
    !(bytes instanceof Uint8Array) ||
    bytes.length !== width * height * channels
  )
    throw new Error("Invalid bounded art image");
}
function u8(value) {
  return Math.trunc(Math.min(255, Math.max(0, value))) || 0;
}
function even(value) {
  const n = Math.floor(value),
    f = value - n;
  return f < 0.5 ? n : f > 0.5 ? n + 1 : n + (n % 2);
}
export function defaultState(bytes, width, height) {
  image(bytes, width, height);
  const out = bytes.slice();
  for (let y = even(height * 0.12); y < even(height * 0.88); y++)
    for (let x = even(width * 0.12); x < even(width * 0.88); x++) {
      const p = (y * width + x) * 4,
        r = bytes[p],
        g = bytes[p + 1],
        b = bytes[p + 2];
      if (r > 14 && ((r > g * 1.3 && g - b < 0.17 * r) || r > g * 1.7)) {
        const l = r * 0.5 + g * 0.3 + b * 0.2;
        for (const [c, k] of [
          [0, 0.42],
          [1, 0.4],
          [2, 0.38],
        ])
          out[p + c] = u8(even(l * k + 6));
      }
    }
  return out;
}
export function clearCircles(bytes, width, height, circles) {
  image(bytes, width, height);
  if (
    !Array.isArray(circles) ||
    circles.length > 32 ||
    circles.some((c) => c.length !== 3 || c.some((v) => !Number.isFinite(v)))
  )
    throw new Error("Invalid circles");
  const out = bytes.slice();
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++)
      if (circles.some(([cx, cy, r]) => (x - cx) ** 2 + (y - cy) ** 2 < r ** 2))
        out[(y * width + x) * 4 + 3] = 0;
  return out;
}
export function silhouette(bytes, width, height, frameWidth, pitch) {
  image(bytes, width, height);
  if (
    !Number.isSafeInteger(pitch) ||
    pitch < 1 ||
    !Number.isFinite(frameWidth) ||
    frameWidth <= 0 ||
    frameWidth > width
  )
    throw new Error("Invalid silhouette frame");
  const out = new Uint8Array(bytes.length);
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++) {
      const p = (y * width + x) * 4;
      const r = bytes[p] / 255,
        g = bytes[p + 1] / 255,
        b = bytes[p + 2] / 255;
      const mx = Math.max(r, g, b),
        mn = Math.min(r, g, b),
        sat = (mx - mn) / Math.max(mx, 1e-6);
      const clip = (v) => Math.min(1, Math.max(0, v));
      let a = clip((mx - 0.42) / 0.18) * clip((0.32 - sat) / 0.12) * (bytes[p + 3] / 255);
      if (!(Math.hypot((x % pitch) - frameWidth / 2, y - height / 2) < frameWidth * 0.46)) a = 0;
      out[p] = u8(r * 255);
      out[p + 1] = u8(g * 255);
      out[p + 2] = u8(b * 255);
      out[p + 3] = u8(a * 255);
    }
  return out;
}
export function metal(mask, outline, width, height) {
  image(mask, width, height, 1);
  image(outline, width, height, 1);
  let y0 = height,
    y1 = -1;
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++)
      if (mask[y * width + x]) {
        y0 = Math.min(y0, y);
        y1 = y;
      }
  if (y1 < 0) throw new Error("Empty metal glyph");
  const out = new Uint8Array(width * height * 4),
    top = [236, 206, 140],
    bot = [120, 84, 36],
    light = [255, 240, 200],
    edge = [28, 18, 8];
  const mod = (a, b) => ((a % b) + b) % b;
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++) {
      const p = y * width + x,
        a = mask[p] / 255,
        t = Math.min(1, Math.max(0, (y - y0) / Math.max(1, y1 - y0)));
      const hi = mask[mod(y + 2, height) * width + mod(x + 2, width)] / 255;
      const sh = mask[mod(y - 3, height) * width + mod(x - 3, width)] / 255;
      for (let c = 0; c < 3; c++) {
        let v = (top[c] * (1 - t) + bot[c] * t) * a;
        v += Math.min(1, Math.max(0, a - hi)) * light[c] * 0.6;
        v *= 1 - 0.45 * Math.min(1, Math.max(0, a - sh));
        out[p * 4 + c] = u8(v + (1 - a) * edge[c]);
      }
      out[p * 4 + 3] = u8(Math.max(a, outline[p] / 255) * 255);
    }
  return out;
}
export function feather(width, height, n = 2) {
  if (
    !Number.isSafeInteger(width) ||
    !Number.isSafeInteger(height) ||
    width < 1 ||
    height < 1 ||
    width * height > MAX_PIXELS
  )
    throw new Error("Invalid feather dimensions");
  const out = new Uint8Array(width * height);
  image(out, width, height, 1);
  if (!Number.isSafeInteger(n) || n < 0 || n > Math.min(width, height))
    throw new Error("Invalid feather width");
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++)
      out[y * width + x] = u8(
        Math.min(1, (Math.min(x, y, width - 1 - x, height - 1 - y) + 1) / (n + 1)) * 255,
      );
  return out;
}
export function rgbStats(bytes) {
  if (
    !(bytes instanceof Uint8Array) ||
    bytes.length < 3 ||
    bytes.length % 3 ||
    bytes.length / 3 > MAX_PIXELS
  )
    throw new Error("Invalid RGB statistics");
  const mean = [0, 0, 0],
    variance = [0, 0, 0],
    n = bytes.length / 3;
  for (let i = 0; i < bytes.length; i++) mean[i % 3] += bytes[i];
  for (let c = 0; c < 3; c++) mean[c] /= n;
  for (let i = 0; i < bytes.length; i++) variance[i % 3] += (bytes[i] - mean[i % 3]) ** 2;
  return [mean, variance.map((v) => Math.sqrt(v / n))];
}
export function stone(bytes, width, height, targetMean, targetStd) {
  image(bytes, width, height, 3);
  if ([targetMean, targetStd].some((a) => a.length !== 3 || a.some((v) => !Number.isFinite(v))))
    throw new Error("Invalid stone statistics");
  const [mean, std] = rgbStats(bytes),
    out = new Uint8Array(width * height * 4);
  for (let p = 0; p < width * height; p++) {
    for (let c = 0; c < 3; c++)
      out[p * 4 + c] = u8(((bytes[p * 3 + c] - mean[c]) / std[c]) * targetStd[c] + targetMean[c]);
    out[p * 4 + 3] = 255;
  }
  return out;
}
export function vaultCover(bytes, width, height) {
  image(bytes, width, height);
  if (width < 625 || height < 644) throw new Error("Vault background is too small");
  const brightness = (x0, x1) => {
    let sum = 0;
    for (let x = x0; x < x1; x++) {
      const p = (608 * width + x) * 4;
      sum += bytes[p] + bytes[p + 1] + bytes[p + 2];
    }
    return Math.fround(sum / (x1 - x0));
  };
  if (!(brightness(342, 504) < 75 && brightness(262, 312) > 85))
    throw new Error("Vault gold box differs from pinned source geometry");
  const out = bytes.slice(),
    f = Math.fround;
  for (let y = 576; y < 644; y++)
    for (let x = 322; x < 524; x++) {
      const sx = x < 423 ? 643 - x : 1047 - x;
      for (let c = 0; c < 4; c++) out[(y * width + x) * 4 + c] = bytes[(y * width + sx) * 4 + c];
    }
  for (let k = 0; k < 16; k++) {
    const t = (k + 0.5) / 16,
      x = 415 + k;
    for (let y = 576; y < 644; y++)
      for (let c = 0; c < 4; c++)
        out[(y * width + x) * 4 + c] = u8(
          f(
            f((1 - t) * bytes[(y * width + 643 - x) * 4 + c]) +
              f(t * bytes[(y * width + 1047 - x) * 4 + c]),
          ),
        );
  }
  return out;
}
