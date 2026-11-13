// 完整的 Switch 模式 HUD 组合移植，由生产资源运行时使用。
// Complete Switch-mode HUD composition port used by the production asset runtime.
import { defaultState, clearCircles, silhouette, metal } from "../art-kernels/kernels.mjs";
import { ordered, put, roundEven as round, float, stringifyPython } from "./python-values.mjs";
import { requireValue as require, vitalCharacters, generateLayout } from "./support.mjs";
import { RELEASE_ART_REVISION, requireArtRevision } from "../art-recipes-js/revision.mjs";
import { acceptsNativeArtPngDigest } from "../../native-art-png-digests.mjs";
const FONTS = ["blizzardglobal-v5_81.ttf", "exocetblizzardot-medium.otf"];
const IMAGE_NAMES = ["front_panel", "levelupbutton", "greater_healing_potion"];
const SIZES = {
  front_panel: [2822, 415],
  levelupbutton: [248, 121],
  greater_healing_potion: [98, 98],
};
const MAX_PIXELS = 4_194_304,
  encoder = new TextEncoder();
const sameKeys = (value, names) =>
  value &&
  typeof value === "object" &&
  !Array.isArray(value) &&
  Object.keys(value).length === names.length &&
  names.every((name) => Object.hasOwn(value, name));
function utf8(text, cap, code) {
  require(typeof text === "string", code);
  for (const c of text) {
    const n = c.codePointAt(0);
    require(n < 0xd800 || n > 0xdfff, code);
  }
  const raw = encoder.encode(text);
  require(raw.length <= cap, code);
  return raw;
}
async function sha256(bytes) {
  return [...new Uint8Array(await globalThis.crypto.subtle.digest("SHA-256", bytes))]
    .map((n) => n.toString(16).padStart(2, "0"))
    .join("");
}
function base64(value, cap, code) {
  require(typeof value === "string" &&
    value.length <= Math.ceil(cap / 3) * 4 &&
    value.length % 4 === 0 &&
    /^[A-Za-z0-9+/]*={0,2}$/.test(value), code);
  const raw = globalThis.atob(value);
  require(raw.length <= cap, code);
  const result = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) result[i] = raw.charCodeAt(i);
  return result;
}
function newImage(width, height, channels = 4) {
  require(Number.isInteger(width) &&
    Number.isInteger(height) &&
    width > 0 &&
    height > 0 &&
    width <= 8192 &&
    height <= 8192 &&
    width * height <= MAX_PIXELS, "HUD_CANVAS_LIMIT");
  return { width, height, channels, data: new Uint8Array(width * height * channels) };
}
const withData = (image, data) => ({ ...image, data });
const clone = (image) => withData(image, image.data.slice());
function clearRectangle(image, x0, y0, x1, y1) {
  for (let y = Math.max(0, y0); y < Math.min(image.height, y1); y++)
    for (let x = Math.max(0, x0); x < Math.min(image.width, x1); x++)
      image.data[(y * image.width + x) * 4 + 3] = 0;
}
function border(image) {
  // Pillow 包含端点的矩形 (0,0,w-1,h-1)，线宽为 3，使用替换式 RGBA 颜色。
  // Pillow inclusive rectangle (0,0,w-1,h-1), width=3, replacement RGBA ink.
  for (let y = 0; y < image.height; y++)
    for (let x = 0; x < image.width; x++)
      if (x < 3 || y < 3 || x >= image.width - 3 || y >= image.height - 3)
        image.data.set([40, 33, 24, 255], (y * image.width + x) * 4);
  return image;
}
export async function generateHud(payload, options, { codec, kernels: k, font }) {
  require(payload instanceof Uint8Array && payload.length <= 64 * 1024 * 1024, "HUD_INPUT_LIMIT");
  require(options?.mode === "switch-upscaled", "HUD_MODE_REQUIRED");
  requireArtRevision(Object.hasOwn(options, "revision") ? options.revision : RELEASE_ART_REVISION);
  require(codec && k && font, "HUD_DEPENDENCIES_REQUIRED");
  const bundle = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(payload)),
    metadata = bundle.metadata;
  require(sameKeys(metadata, ["patch", "i18n", "manifest"]), "HUD_METADATA_SET");
  require(sameKeys(options.metadataSha256, Object.keys(metadata)), "HUD_TRUST_REQUIRED");
  for (const [name, text] of Object.entries(metadata)) {
    const raw = utf8(text, 2 * 1024 * 1024, "HUD_METADATA_LIMIT");
    require((await sha256(raw)) === options.metadataSha256[name], "HUD_METADATA_HASH");
  }
  require(sameKeys(bundle.images, IMAGE_NAMES), "HUD_IMAGE_SET");
  require(sameKeys(bundle.fonts, FONTS), "HUD_FONT_SET");
  const manifest = JSON.parse(metadata.manifest);
  require(Array.isArray(manifest.assets), "HUD_ATLAS_SET");
  const atlases = manifest.assets.filter(
    (a) => typeof a.asset === "string" && a.asset.startsWith("dm_native_skill_"),
  );
  const atlasNames = Array.from({ length: 9 }, (_, i) => `dm_native_skill_${i}.png`);
  require(atlases.length === 9 &&
    new Set(atlases.map((a) => a.asset)).size === 9 &&
    atlases.every((a) => atlasNames.includes(a.asset)), "HUD_ATLAS_SET");
  require(sameKeys(bundle.atlases, atlasNames), "HUD_ATLAS_SET");
  utf8(bundle.layout, 2 * 1024 * 1024, "HUD_LAYOUT_LIMIT");
  const images = {},
    fonts = {},
    decodedAtlases = {};
  let pixels = 0;
  for (const name of IMAGE_NAMES) {
    const value = bundle.images[name];
    require(Array.isArray(value.size) && value.size.length === 2, "HUD_DIMENSIONS");
    const [width, height] = value.size;
    require(Number.isInteger(width) &&
      Number.isInteger(height) &&
      width > 0 &&
      height > 0 &&
      width <= 8192 &&
      height <= 8192, "HUD_DIMENSIONS");
    pixels += width * height;
    require(pixels <= MAX_PIXELS, "HUD_PIXEL_LIMIT");
    const data = base64(value.rgba, MAX_PIXELS * 4, "HUD_RGBA_SIZE");
    require(data.length === width * height * 4, "HUD_RGBA_SIZE");
    images[name] = { width, height, channels: 4, data };
  }
  for (const name of FONTS) {
    fonts[name] = base64(bundle.fonts[name], 16 * 1024 * 1024, "HUD_FONT_LIMIT");
    require(fonts[name].length > 0, "HUD_FONT_LIMIT");
  }
  for (const a of atlases) {
    const raw = base64(bundle.atlases[a.asset], MAX_PIXELS * 4 + 131072, "HUD_ATLAS_SIZE");
    require(acceptsNativeArtPngDigest(a, await sha256(raw)), "HUD_ATLAS_HASH");
    const image = codec.decodeRgba(raw);
    require(Array.isArray(a.atlas_px) &&
      a.atlas_px.length === 2 &&
      image.width === a.atlas_px[0] &&
      image.height === a.atlas_px[1], "HUD_ATLAS_DIMENSIONS");
    pixels += image.width * image.height;
    require(pixels <= MAX_PIXELS, "HUD_PIXEL_LIMIT");
    decodedAtlases[a.asset] = image;
  }
  const vital = vitalCharacters(metadata.i18n, true),
    files = Object.create(null);
  let outputBytes = 0,
    activeFont = "";
  const crop = (image, box) => k.crop(image, box[0], box[1], box[2] - box[0], box[3] - box[1]);
  const resize = (image, w, h) => k.resizeLanczos(image, w, h);
  const source = (name) => resize(images[name], ...SIZES[name]);
  const prepare = (image) => {
    const alpha = k.channel(image, 3),
      lut = Uint8Array.from({ length: 256 }, (_, i) => (i > 24 ? 255 : 0)),
      box = k.bbox(k.point(alpha, lut));
    require(box, "HUD_EMPTY_ART_ALPHA");
    return crop(image, box);
  };
  const useFont = (name, size) => {
    const key = `${name}/${size}`;
    if (activeFont !== key) {
      font.open(fonts[name], size);
      activeFont = key;
    }
  };
  const draw = (image, text, opts) =>
    withData(
      image,
      font.draw(image.data, image.width, image.height, text, { ...opts, channels: image.channels }),
    );
  const add = (name, raw) => {
    require(!Object.hasOwn(files, name) && raw instanceof Uint8Array, "HUD_OUTPUT_DUPLICATE");
    outputBytes += raw.length;
    require(Object.keys(files).length < 512 && outputBytes <= 32 * 1024 * 1024, "HUD_OUTPUT_LIMIT");
    files[name] = raw;
  };
  const png = (name, image) => add(name, codec.encodeRgba(image.data, image.width, image.height));
  try {
    const fp = source("front_panel"),
      levelSheet = source("levelupbutton"),
      lvl = crop(levelSheet, [0, 0, 124, 121]);
    const W = 2868,
      H = 1320,
      K = 156 / 348,
      P = 120,
      DM = 114,
      SH = round((DM * (391 - 277)) / (1792 - 1673)),
      B = 12,
      EXT = 60,
      BH = 156,
      XC = 1390,
      N = 6;
    const slotsWidth = N * DM + (N - 1) * (P - DM),
      SX0 = XC - Math.floor(slotsWidth / 2),
      SY0 = H - BH + Math.floor((BH - SH) / 2),
      BAR_X0 = SX0 - B,
      BAR_X1 = SX0 + slotsWidth + B;
    const [IX0, IX1, IY0, IY1] = [2, 107, 7, 101],
      IW = IX1 - IX0 + 1,
      IH = IY1 - IY0 + 1,
      BOX = round(Math.min(IW, IH) * (1 - 2 * 0.09));
    const slots = Array.from({ length: N }, (_, i) => [
      SX0 + i * P,
      SY0,
      SX0 + i * P + DM,
      SY0 + SH,
    ]);
    const tile = crop(fp, [690, 330, 1170, 388]),
      tileFlipped = k.flip(tile, 0),
      edge = crop(fp, [690, 276, 1170, 288]),
      edgeFlipped = k.flip(edge, 1);
    let bar = newImage(BAR_X1 - BAR_X0 + 2 * EXT, BH);
    for (let y = 0; y < BH; y += tile.height)
      for (let x = 0; x < bar.width; x += tile.width)
        bar = k.alphaComposite(
          bar,
          Math.floor(x / tile.width) % 2 === 0 ? tile : tileFlipped,
          x,
          y,
        );
    for (let x = 0; x < bar.width; x += edge.width) {
      bar = k.alphaComposite(bar, edge, x, 0);
      bar = k.alphaComposite(bar, edgeFlipped, x, BH - edge.height);
    }
    border(bar);
    const slot = resize(crop(fp, [1673, 277, 1792, 391]), DM, SH),
      reference = prepare(source("greater_healing_potion")),
      referenceScale = BOX / Math.max(reference.width, reference.height);
    const referenceG = Math.sqrt(
      reference.width * referenceScale * (reference.height * referenceScale),
    );
    const L0 = crop(fp, [0, 0, 610, 415]),
      R0 = crop(fp, [2210, 0, 2822, 415]),
      L = resize(L0, round(L0.width * K), round(L0.height * K)),
      R = resize(R0, round(R0.width * K), round(R0.height * K));
    const BR = round(174 * K),
      LX = BAR_X0 + 6 - round(608 * K),
      RX = BAR_X1 - 6 - round(24 * K),
      LBC = [LX + round(434 * K), H - L.height + round(239 * K)],
      RBC = [RX + round(198 * K), H - R.height + round(239 * K)];
    const EX0 = slots[0][0],
      EX1 = slots.at(-1)[2],
      EY = H - BH - 12;
    useFont(FONTS[0], 30);
    const labelBoxes = [
      [LBC, "生命: 55 / 55"],
      [RBC, "法力: 20 / 20"],
    ].map(([center, text]) => [center, font.measure(text, { anchor: "mb", stroke: 1 }).bbox]);
    function hitsStatue(box) {
      for (const [image, ox] of [
        [L, LX],
        [R, RX],
      ]) {
        const oy = H - image.height;
        for (let y = Math.max(box[1], oy); y < Math.min(box[3], H); y++)
          for (let x = Math.max(box[0], ox); x < Math.min(box[2], ox + image.width); x++)
            if (image.data[((y - oy) * image.width + x - ox) * 4 + 3] > 24) return true;
      }
      return false;
    }
    let YB = LBC[1] - BR - 4;
    for (; ; YB--) {
      require(YB >= -8192, "HUD_BASELINE_LIMIT");
      const ok = labelBoxes.every(([center, b]) => {
        const box = [b[0] + center[0], b[1] + YB, b[2] + center[0] + 2, b[3] + YB + 2];
        return !hitsStatue(box) && !(box[2] > EX0 - 6 && box[0] < EX1 + 6 && box[3] > EY - 6);
      });
      if (ok) break;
    }
    const g = { W, H, BR, LBC, RBC, EX0, EX1, EY, YB, slots };
    let canvas = newImage(W, H);
    canvas = k.alphaComposite(canvas, bar, BAR_X0 - EXT, H - BH);
    for (const [x, y] of slots) canvas = k.alphaComposite(canvas, slot, x, y);
    let cross = prepare(withData(lvl, defaultState(lvl.data, lvl.width, lvl.height)));
    const fitScale = Math.min(
      BOX / Math.max(cross.width, cross.height),
      referenceG / Math.sqrt(cross.width * cross.height),
    );
    cross = resize(
      cross,
      Math.max(1, round(cross.width * fitScale)),
      Math.max(1, round(cross.height * fitScale)),
    );
    let layer = newImage(W, H);
    layer = k.alphaComposite(
      layer,
      cross,
      round(slots[5][0] + IX0 + IW / 2 - cross.width / 2),
      round(slots[5][1] + IY0 + IH / 2 - cross.height / 2),
    );
    canvas = k.alphaComposite(canvas, layer);
    const circles = [LBC, RBC].map(([x, y]) => [x, y, BR - 2]);
    let glass = withData(canvas, clearCircles(canvas.data, W, H, circles));
    for (const [image, x] of [
      [L, LX],
      [R, RX],
    ]) {
      canvas = k.alphaComposite(canvas, image, x, H - image.height);
      glass = k.alphaComposite(glass, image, x, H - image.height);
    }
    canvas = withData(canvas, clearCircles(canvas.data, W, H, circles));
    const bounds = k.bbox(canvas);
    require(bounds, "HUD_EMPTY_BASE");
    let sprite = crop(canvas, bounds);
    const full = clone(glass);
    for (const i of [1, 2, 3, 4]) {
      const [x, y] = slots[i];
      clearRectangle(full, x + IX0, y + IY0, x + IX1 + 1, y + IY1 + 1);
    }
    clearRectangle(full, ...slots[5]);
    const fullCropped = crop(full, bounds),
      U = H / 2160;
    sprite = resize(sprite, round(sprite.width / (2 * U)), round(sprite.height / (2 * U)));
    png("base.png", sprite);
    const layout = generateLayout(bundle.layout, metadata.patch, g, bounds);
    add("hud.json", encoder.encode(stringifyPython(layout)));
    const uiPath = "hud_v4_ui/";
    function glyphs(tag, fill, size, characters = "0123456789Lv·% ") {
      useFont(FONTS[0], size);
      const result = ordered();
      for (const character of characters) {
        const measured = font.measure(character),
          [asc, desc] = measured.metrics,
          w = Math.trunc(measured.length) + 20;
        let image = newImage(w, asc + desc + 20);
        const origin = [8, 8 + asc];
        image = draw(image, character, {
          x: origin[0] + 2,
          y: origin[1] + 2,
          fill: [0, 0, 0, 170],
          anchor: "ls",
        });
        image = draw(image, character, {
          x: origin[0],
          y: origin[1],
          fill,
          anchor: "ls",
          stroke: 1,
          strokeFill: [10, 8, 6, 255],
        });
        const box = k.bbox(image),
          name = `g_${tag}_${character.codePointAt(0).toString(16).padStart(4, "0")}.png`;
        if (box) png(uiPath + name, crop(image, box));
        put(result, character, {
          file: box ? name : null,
          dx: box ? box[0] - origin[0] : 0,
          dy: box ? box[1] - origin[1] : 0,
          adv: float(measured.length),
        });
      }
      return result;
    }
    const ui = {
      ref: [W, H],
      center_x: float(W / 2),
      slots,
      inner: [IX0, IY0, IX1, IY1],
      exp: {
        x0: EX0,
        x1: EX1,
        y: EY,
        h: 6,
        track: [22, 18, 14],
        edge: [70, 58, 40],
        fill: [196, 160, 70],
        ticks: 20,
      },
      lv: {
        x: float((EX0 + EX1) / 2),
        baseline: EY - 6,
        glyphs: glyphs("lv", [205, 185, 140, 255], 26),
      },
      badge: { glyphs: glyphs("b", [235, 235, 235, 255], 26), dx: -3, dy: -1 },
      pick: { center_x: 1390, pitch: 120, y: float(1051.5) },
      vital: { left_x: LBC[0], right_x: RBC[0], baseline: YB },
    };
    ui.vital.glyphs = glyphs("v", [235, 235, 235, 255], 30, vital);
    png(uiPath + "slot.png", slot);
    png(uiPath + "base_full.png", fullCropped);
    ui.base = { x: bounds[0], y: bounds[1] };
    for (let i = 0; i < 2; i++) {
      const frame = resize(crop(levelSheet, [i * 124, 0, i * 124 + 124, 121]), DM, SH);
      png(uiPath + `lvl_${i}.png`, frame);
      png(
        uiPath + `lvl_d${i}.png`,
        withData(frame, defaultState(frame.data, frame.width, frame.height)),
      );
    }
    useFont(FONTS[1], 220);
    for (let n = 1; n <= 8; n++) {
      let mask = newImage(330, 440, 1);
      mask = draw(mask, String(n), { x: 55, y: 55, fill: 255 });
      const outline = k.maxFilter(mask, 5);
      let image = prepare({
        width: mask.width,
        height: mask.height,
        channels: 4,
        data: metal(mask.data, outline.data, mask.width, mask.height),
      });
      const scale = (BOX * 0.85) / image.height;
      image = resize(image, Math.max(1, round(image.width * scale)), round(image.height * scale));
      png(uiPath + `diff_${n}.png`, image);
    }
    for (const atlas of atlases) {
      const image = decodedAtlases[atlas.asset];
      png(
        uiPath + "sil_" + atlas.asset,
        withData(
          image,
          silhouette(image.data, image.width, image.height, atlas.frame_width, atlas.frame_pitch),
        ),
      );
    }
    useFont(FONTS[0], 34);
    for (const [name, text] of [
      ["main", "攻击"],
      ...Array.from({ length: 8 }, (_, i) => [`f${i + 1}`, `F${i + 1}`]),
    ]) {
      let image = newImage(Math.trunc(font.measure(text).length) + 16, 60);
      image = draw(image, text, {
        x: 10,
        y: 8,
        fill: [0, 0, 0, 190],
        stroke: 2,
        strokeFill: [0, 0, 0, 190],
      });
      image = draw(image, text, {
        x: 8,
        y: 6,
        fill: [226, 206, 160, 255],
        stroke: 1,
        strokeFill: [30, 22, 12, 255],
      });
      const box = k.bbox(image);
      png(uiPath + `lbl_${name}.png`, box ? crop(image, box) : image);
    }
    add(uiPath + "ui.json", encoder.encode(stringifyPython(ui)));
    const expected = new Set([
      "base.png",
      "hud.json",
      uiPath + "ui.json",
      uiPath + "slot.png",
      uiPath + "base_full.png",
    ]);
    for (let i = 0; i < 2; i++) {
      expected.add(uiPath + `lvl_${i}.png`);
      expected.add(uiPath + `lvl_d${i}.png`);
    }
    for (let i = 1; i <= 8; i++) expected.add(uiPath + `diff_${i}.png`);
    for (const a of atlases) expected.add(uiPath + "sil_" + a.asset);
    for (const name of ["main", ...Array.from({ length: 8 }, (_, i) => `f${i + 1}`)])
      expected.add(uiPath + `lbl_${name}.png`);
    for (const key of ["lv", "badge", "vital"])
      for (const glyph of Object.values(ui[key].glyphs))
        if (glyph.file) expected.add(uiPath + glyph.file);
    require(Object.keys(files).length === expected.size &&
      Object.keys(files).every((name) => expected.has(name)), "HUD_OUTPUT_SET");
    return {
      files,
      mode: "switch-upscaled",
      backend: { pillow: "10.2.0", freetype: "2.6.0", raqm: false },
    };
  } finally {
    font.close();
    for (const bytes of Object.values(fonts)) bytes.fill(0);
  }
}
