/** 固定源版本的佣兵、仓库、宝藏、金库与袋子图像组合，不使用 Python 或 I/O。
 * 显式指定后端：固定的图像内核、PNG 编解码器及现有数值图像内核。
 * Source-pinned mercenary/stash/trove/vault/pouch composition, no Python or I/O.
 * Backends are explicit: frozen image-kernels, PNG codec and existing numeric art kernels.
 */
const MAX_PAYLOAD = 20 * 1024 * 1024;
const OPERATIONS = new Set(["remaining-art", "mercenary-art"]);
const SPEC = Object.freeze({
  StashPanelBG: { cut: 133, yb: 525, mid: [214, 438] },
  StashPanelBGPersonal: { cut: 129, yb: 515, mid: [216, 428] },
  StashPanelBGShared: { cut: 129, yb: 515, mid: [216, 428] },
});
const MERCENARY_INPUTS = Object.freeze({
  "hirelinginventory/v2/hirelinginventorybg": [652, 704],
  "inventorypanel/v2/inventorybg_classic": [507, 700],
  mainpanelbg: [2888, 881],
});
const EXPECTED = Object.freeze({
  stash: ["image", "personal"],
  trove: ["gems", "runes", "cons", "character"],
  advanced: ["image"],
  vault: ["image"],
  pouch: ["image"],
});
const requireValue = (value, code) => {
  if (!value) throw new Error(code);
};
const object = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
const keys = (value, names) =>
  object(value) &&
  Object.keys(value).length === names.length &&
  names.every((key) => Object.hasOwn(value, key));
const even = (value) => {
  const n = Math.floor(value),
    f = value - n;
  return f < 0.5 ? n : f > 0.5 ? n + 1 : n + (n % 2);
};
const newImage = (width, height, data = new Uint8Array(width * height * 4), channels = 4) => ({
  width,
  height,
  channels,
  data,
});
function base64(value, code) {
  requireValue(
    typeof value === "string" && value.length % 4 === 0 && !/[^A-Za-z0-9+/=]/.test(value),
    code,
  );
  const padding = value.indexOf("=");
  requireValue(
    padding < 0 ||
      (padding >= value.length - 2 && (value.length - padding === 1 || value.endsWith("=="))),
    code,
  );
  let raw;
  try {
    raw = atob(value);
  } catch {
    throw new Error(code);
  }
  // 直接填充有界输出，避免字符串迭代器展开为逐字符临时列表。
  // Fill the bounded output directly, without expanding a string iterator into a character list.
  const bytes = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
  return bytes;
}
function parse(payload, code) {
  requireValue(payload instanceof Uint8Array && payload.length <= MAX_PAYLOAD, code);
  return JSON.parse(new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(payload));
}
export function supportsRemainingArtOperation(operation) {
  return OPERATIONS.has(operation);
}

/** 匹配 gen_trove_art.geometry 的插入顺序与 Python 浮点表达式。
 * Matches gen_trove_art.geometry insertion order and Python's float expressions. */
export function troveGeometry() {
  const W = 732,
    FR = 20,
    FRR = 26,
    SCALE = 0.85,
    PED_X = 205 - 58 + 98 - 36;
  const dm = 45 * SCALE,
    tw = 13 * 43 + dm + 18,
    x0 = FR + (W - FR - FRR - tw) / 2;
  const result = {},
    rows = ["gc", "gf", "gs", "gl", "gp"],
    colors = ["w", "g", "r", "y", "v", "b"],
    skulls = ["skc", "skf", "sku", "skl", "skz"];
  for (let r = 0; r < 5; r++)
    for (let k = 0; k < 14; k++) {
      const cx = x0 + k * 43 + dm / 2 + (k >= 7 ? 18 : 0),
        cy = 28 + r * 43 + dm / 2;
      if (k < 6) {
        let code = rows[r] + colors[k];
        if (code === "glv") code = "gzv";
        result[code] = [cx, cy, 1];
      } else if (k === 6) result[skulls[r]] = [cx, cy, 1];
      else if (r < 4) result[`r${String(r * 7 + k - 6).padStart(2, "0")}`] = [cx, cy, 1];
      else if (k - 7 < 5) result[`r${String(29 + k - 7).padStart(2, "0")}`] = [cx + 43, cy, 1];
    }
  function group(codes, counts, gx) {
    let i = 0;
    for (let row = 0; row < counts.length; row++)
      for (let k = 0; k < counts[row]; k++) {
        const cx = gx + k * 44 + dm / 2;
        const cy =
          row === 0 ? 278 + (95 * SCALE) / 2 : 278 + 95 * SCALE + 6 + (row - 1) * 44 + dm / 2;
        result[codes[i++]] = [cx, cy, row === 0 ? 2 : 1];
      }
  }
  const pl = PED_X,
    pr = PED_X + 234;
  group(
    ["pk1", "pk2", "pk3", "dhn", "bey", "mbr", "rvs", "rvl"],
    [3, 3, 2],
    FR + (pl - FR - (2 * 44 + dm)) / 2,
  );
  group(
    [
      "ua1",
      "ua2",
      "ua3",
      "ua4",
      "ua5",
      "xa1",
      "xa2",
      "xa3",
      "xa4",
      "xa5",
      "toa",
      "tes",
      "ceh",
      "bet",
      "fed",
    ],
    [5, 5, 5],
    pr + (W - FRR - pr - (4 * 44 + dm)) / 2,
  );
  return result;
}
export function troveSlots() {
  const result = {};
  for (const [code, [cx, cy, height]] of Object.entries(troveGeometry())) {
    const w = even(98 * 0.85),
      h = height === 1 ? w : even(196 * 0.85),
      wp = 49 * 0.85,
      hp = 49 * 0.85 * height;
    result[code] = [even((cx - wp / 2 - (98 - 58)) * 2), even((cy - hp / 2) * 2) + 102, w, h];
  }
  return result;
}

export function createRemainingArtRecipes({ pixels, png, numeric } = {}) {
  for (const name of ["crop", "paste", "flip", "convert", "resizeLanczos"])
    requireValue(typeof pixels?.[name] === "function", "ART_PIXEL_BACKEND_REQUIRED");
  requireValue(typeof png?.encodeRgba === "function", "ART_PNG_BACKEND_REQUIRED");
  for (const name of ["rgbStats", "stone", "feather", "vaultCover"])
    requireValue(typeof numeric?.[name] === "function", "ART_NUMERIC_BACKEND_REQUIRED");
  const crop = (src, [x0, y0, x1, y1]) => pixels.crop(src, x0, y0, x1 - x0, y1 - y0);
  const paste = (dst, src, x, y, mask = null) => pixels.paste(dst, src, x, y, mask);
  const feather = (width, height, n = 2) =>
    newImage(width, height, numeric.feather(width, height, n), 1);
  function stats(src, boxes) {
    const parts = boxes.map((box) => pixels.convert(crop(src, box), 3).data);
    const all = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
    let offset = 0;
    for (const part of parts) {
      all.set(part, offset);
      offset += part.length;
    }
    return numeric.rgbStats(all);
  }
  function stone(src, width, height, target, ox = 90, oy = 190) {
    const rgba = pixels.convert(crop(src, [ox, oy, ox + width, oy + height]), 3);
    return newImage(width, height, numeric.stone(rgba.data, width, height, target[0], target[1]));
  }
  function widenStash(src, personal, spec) {
    const w = src.width,
      h = src.height,
      {
        cut,
        yb,
        mid: [left, right],
      } = spec;
    let out = newImage(w + 196, h);
    const top = crop(src, [0, 0, w, yb]),
      bottom = crop(src, [0, yb, w, h]),
      bh = h - yb;
    out = paste(out, crop(top, [0, 0, cut + 196, yb]), 0, 0);
    out = paste(out, crop(top, [cut, 0, w, yb]), cut + 196, 0);
    out = paste(out, crop(bottom, [0, 0, left, bh]), 0, yb);
    out = paste(out, crop(bottom, [right, 0, w, bh]), right + 196, yb);
    out = paste(out, crop(bottom, [left, 0, right, bh]), left + 98, yb);
    const source = crop(personal, [248, 533, 346, 577]);
    let col = newImage(98, bh),
      k = 0;
    for (let y = 0; y < bh; y += source.height, k++)
      col = paste(col, k % 2 ? pixels.flip(source, 1) : source, 0, y);
    out = paste(out, col, left, yb);
    return paste(out, col, right + 98, yb);
  }
  function pouch(src) {
    const frame = crop(src, [152, 628, 352, 692]),
      h = frame.height;
    let out = newImage(140, h);
    out = paste(out, crop(frame, [0, 0, 24, h]), 0, 0);
    out = paste(out, crop(frame, [frame.width - 24, 0, frame.width, h]), 116, 0);
    return paste(
      out,
      pixels.resizeLanczos(crop(frame, [24, 0, frame.width - 24, h]), 92, h),
      24,
      0,
    );
  }
  function trove(gems, runes, cons, character) {
    requireValue(
      [gems, runes, cons].every((im) => im.width === 652 && im.height === 649) &&
        character.width === 1369 &&
        character.height === 706,
      "REMAINING_ART_TROVE_GEOMETRY",
    );
    const W = 732,
      H = 649,
      FR = 20,
      FRR = 26,
      YS = 462,
      YB = 507,
      PED_X = 209;
    let out = newImage(W, H);
    const g = crop(gems, [58, 0, 594, H]),
      c = crop(cons, [58, 0, 594, H]),
      gw = g.width;
    const target = stats(gems, [
      [165, 31, 178, 276],
      [226, 31, 239, 276],
      [287, 31, 300, 276],
    ]);
    out = paste(out, crop(g, [0, 0, FR, H]), 0, 0);
    out = paste(out, crop(g, [gw - FRR, 0, gw, H]), W - FRR, 0);
    const st = stone(character, W - 2 * FR + 6, H, target);
    out = paste(out, crop(st, [0, 0, st.width, YS - 14]), FR - 3, 14);
    out = paste(out, crop(g, [0, 0, FR - 3, YS]), 0, 0);
    out = paste(out, crop(g, [gw - FRR + 3, 0, gw, YS]), W - FRR + 3, 0);
    function mirrorFill(strip, x, y, width) {
      let k = 0;
      for (let px = x; px < x + width; k++) {
        let part = k % 2 ? pixels.flip(strip, 0) : strip;
        part = crop(part, [0, 0, Math.min(part.width, x + width - px), part.height]);
        out = paste(out, part, px, y);
        px += part.width;
      }
    }
    mirrorFill(crop(g, [FR, 0, gw - FRR, 15]), FR, 0, W - FR - FRR);
    function widen(y0, y1, xl, xr, mode) {
      const band = crop(c, [0, y0, gw, y1]),
        h = y1 - y0,
        nx = xl + 98 - 36;
      const left = crop(band, [0, 0, xl, h]),
        mid = crop(band, [xl, 0, xr, h]),
        right = crop(band, [xr, 0, gw, h]);
      function fill(x, width, edge, flip) {
        if (width <= 0) return;
        let block;
        if (mode === "tile") {
          const src = crop(edge, flip ? [6, 0, 70, h] : [edge.width - 70, 0, edge.width - 6, h]);
          block = newImage(width, h);
          for (let px = 0; px < width; px += src.width)
            block = paste(block, src, flip ? width - px - src.width : px, 0);
        } else {
          block = stone(
            character,
            width,
            h,
            stats(band, [[30, h - 16, gw - 30, h - 4]]),
            300 + x,
            420,
          );
          // 精确匹配宽度为 1 的 ImageDraw.line((0,y),(width,y),fill=RGBA)：
          // 包含端点，再裁剪到图块；不使用抗锯齿或混合。
          // Exact ImageDraw.line((0,y),(width,y),fill=RGBA) at width1: endpoint
          // is inclusive, then clipped to the block. No antialiasing/blending.
          const rgb = pixels.convert(edge, 3),
            col = flip ? 2 : edge.width - 3;
          for (let y = 0; y < h; y++) {
            const p = (y * edge.width + col) * 3,
              r = rgb.data[p],
              green = rgb.data[p + 1],
              b = rgb.data[p + 2];
            if (r - b > 22 && r > 60)
              for (let xx = 0; xx < width; xx++)
                block.data.set([r, green, b, 255], (y * width + xx) * 4);
          }
        }
        out = paste(out, block, x, y0);
      }
      fill(xl, nx - xl, left, false);
      fill(nx + (xr - xl), W - (gw - xr) - nx - (xr - xl), right, true);
      out = paste(out, left, 0, y0);
      out = paste(out, mid, nx, y0);
      out = paste(out, right, W - (gw - xr), y0);
    }
    const ped = crop(cons, [205, 264, 439, YS + 3]);
    out = paste(out, ped, PED_X, 264, feather(ped.width, ped.height, 1));
    widen(YS, YB, 147, 381, "tile");
    widen(YB, H, 168, 363, "dark");
    function slot(src, x, y, width = 45, height = 45) {
      const tile = crop(src, [x - 4, y - 4, x + width + 4, y + height + 4]);
      return pixels.resizeLanczos(tile, even(tile.width * 0.85), even(tile.height * 0.85));
    }
    const gs = slot(gems, 117, 31),
      rs = slot(runes, 93, 51),
      tall = slot(cons, 87, 49, 45, 95),
      one = slot(cons, 87, 154);
    for (const [code, [cx, cy, height]] of Object.entries(troveGeometry())) {
      const tile =
        height === 2 ? tall : "gs".includes(code[0]) ? gs : /^r\d+$/.test(code) ? rs : one;
      out = paste(
        out,
        tile,
        even(cx - tile.width / 2),
        even(cy - tile.height / 2),
        feather(tile.width, tile.height),
      );
    }
    return out;
  }
  function remaining(payload, options) {
    requireValue(object(options), "REMAINING_ART_INPUT_LIMIT");
    requireValue(keys(options, ["kind", "name"]), "REMAINING_ART_OPTIONS");
    const { kind, name } = options;
    requireValue(Object.hasOwn(EXPECTED, kind), "REMAINING_ART_KIND");
    const raw = parse(payload, "REMAINING_ART_INPUT_LIMIT");
    requireValue(keys(raw, EXPECTED[kind]), "REMAINING_ART_INPUT_SET");
    const images = {};
    for (const [key, entry] of Object.entries(raw)) {
      requireValue(keys(entry, ["width", "height", "rgba"]), "REMAINING_ART_FIELDS");
      const { width, height } = entry;
      requireValue(
        Number.isInteger(width) &&
          Number.isInteger(height) &&
          width >= 1 &&
          width <= 1640 &&
          height >= 1 &&
          height <= 1024,
        "REMAINING_ART_GEOMETRY",
      );
      const data = base64(entry.rgba, "REMAINING_ART_RGBA");
      requireValue(data.length === width * height * 4, "REMAINING_ART_RGBA");
      images[key] = newImage(width, height, data);
    }
    const im = images.image;
    let result;
    if (kind === "stash") {
      requireValue(
        Object.hasOwn(SPEC, name) && im.width === 652 && images.personal.width === 652,
        "REMAINING_ART_STASH_GEOMETRY",
      );
      requireValue(im.height >= 644 && images.personal.height >= 644, "REMAINING_ART_STASH_HEIGHT");
      result = widenStash(im, images.personal, SPEC[name]);
    } else if (kind === "trove") {
      requireValue(name === "AdvancedStash_Gems_Console", "REMAINING_ART_TROVE_NAME");
      result = paste(
        newImage(848, images.gems.height),
        trove(images.gems, images.runes, images.cons, images.character),
        58,
        0,
      );
    } else if (kind === "advanced") {
      requireValue(
        ["AdvancedStash_Consumables_Console", "AdvancedStash_Runes_Console"].includes(name) &&
          im.width === 652 &&
          im.height === 649,
        "REMAINING_ART_ADVANCED_GEOMETRY",
      );
      result = paste(newImage(848, im.height), im, 98, 0);
    } else if (kind === "vault") {
      requireValue(
        name === "VaultBottom" && im.width === 848 && im.height >= 644,
        "REMAINING_ART_VAULT_GEOMETRY",
      );
      const full = newImage(im.width, im.height, numeric.vaultCover(im.data, im.width, im.height));
      let eh = im.height - 516;
      eh += (4 - (eh % 4)) % 4;
      result = crop(full, [0, 516, im.width, 516 + eh]);
    } else {
      requireValue(
        name === "PouchBox" && [507, 508].includes(im.width) && im.height === 700,
        "REMAINING_ART_POUCH_GEOMETRY",
      );
      result = pouch(im);
    }
    const files = { "image.rgba": result.data };
    if (kind === "vault")
      files["VaultBottom.png"] = png.encodeRgba(result.data, result.width, result.height);
    return { width: result.width, height: result.height, files };
  }
  function mercenary(payload, options) {
    requireValue(keys(options, []), "MERCENARY_INPUT_LIMIT");
    const raw = parse(payload, "MERCENARY_INPUT_LIMIT");
    requireValue(keys(raw, Object.keys(MERCENARY_INPUTS)), "MERCENARY_INPUT_SET");
    const images = {};
    for (const [name, size] of Object.entries(MERCENARY_INPUTS)) {
      const entry = raw[name];
      requireValue(keys(entry, ["header", "rgba"]), "MERCENARY_INPUT_FIELDS");
      const header = base64(entry.header, "MERCENARY_HEADER"),
        data = base64(entry.rgba, "MERCENARY_RGBA");
      requireValue(
        header.length === 40 && new TextDecoder().decode(header.subarray(0, 4)) === "SpA1",
        "MERCENARY_HEADER",
      );
      const view = new DataView(header.buffer, header.byteOffset, header.byteLength);
      requireValue(
        view.getUint16(4, true) === 61 &&
          view.getUint32(8, true) === size[0] &&
          view.getUint32(12, true) === size[1] &&
          (name !== "hirelinginventory/v2/hirelinginventorybg" ||
            view.getUint16(6, true) === 652) &&
          data.length === size[0] * size[1] * 4,
        "MERCENARY_DIMENSIONS",
      );
      images[name] = newImage(...size, data);
    }
    const src = images["hirelinginventory/v2/hirelinginventorybg"],
      inv = images["inventorypanel/v2/inventorybg_classic"];
    const mp = crop(images.mainpanelbg, [1444, 0, 2888, 881]);
    let out = newImage(src.width, src.height, src.data.slice());
    out = paste(out, crop(mp, [759, 92, 1395, 504]), 8, 8);
    for (const [x0, x1] of [
      [0, 8],
      [644, 652],
    ])
      out = paste(out, crop(src, [x0, 100, x1, 208]), x0, 312);
    for (const box of [
      [0, 0, inv.width, 345],
      [0, 345, 122, 392],
      [388, 345, inv.width, 392],
    ]) {
      const frame = crop(inv, box);
      out = paste(out, frame, 63 + box[0], 11 + box[1], "source-alpha");
    }
    out = paste(out, crop(src, [0, 312, 652, 422]), 0, 420);
    let y = 530;
    for (const top of [448, 495, 543, 590, 638]) {
      out = paste(out, crop(src, [0, top, 652, top + 34]), 0, y);
      y += 34;
    }
    if (y < 704) out = paste(out, crop(src, [0, y, 652, 704]), 0, y);
    return {
      width: 652,
      height: 704,
      files: {
        "mercenary.rgba": out.data,
        "HirelingInventoryBG.png": png.encodeRgba(out.data, 652, 704),
      },
    };
  }
  return Object.freeze({
    execute(operation, payload, options = {}) {
      requireValue(OPERATIONS.has(operation), "ART_RECIPE_OPERATION");
      return operation === "remaining-art"
        ? remaining(payload, options)
        : mercenary(payload, options);
    },
  });
}
