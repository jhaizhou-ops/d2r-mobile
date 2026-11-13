// 依据 gen_widestash_art.py 等原生图像生成器与 nvn_deswizzle.c 组合剩余图像。
// Compose remaining art from native image generators including gen_widestash_art.py and nvn_deswizzle.c.
/**
 * 仅操作已认证资源的原生 stash/trove/vault/pouch 构建顺序。原始图像语句由 package-assets.py remaining-art 打包。MainPanel 直接复制原始 BC3 块而不重新编码，与 gen_widestash_art.py 一致。
 * Native stash/trove/vault/pouch build order, operating only on authenticated assets.
 * Original image statements are packaged by package-assets.py remaining-art. MainPanel
 * copies original BC3 blocks without re-encoding, matching gen_widestash_art.py.
 */
import { resolveLogicalAssetSuffixes } from "./logical-asset-suffixes.mjs";
const BG = {
  StashPanelBG: "stash/v2/stashpanelbg",
  StashPanelBGPersonal: "stash/v2/stashpanelbgpersonal",
  StashPanelBGShared: "stash/v2/stashpanelbgshared",
};
const ADV = {
  AdvancedStash_Gems_Console: "stash/v2/additionalstash/advancedstash_gems_console",
  AdvancedStash_Consumables_Console: "stash/v2/additionalstash/advancedstash_consumables_console",
  AdvancedStash_Runes_Console: "stash/v2/additionalstash/advancedstash_runes_console",
};
const fail = (code) => {
  throw new Error(code);
};
const asBytes = (value) =>
  value instanceof Uint8Array ? value : value instanceof ArrayBuffer ? new Uint8Array(value) : null;
function base64(bytes) {
  let text = "";
  for (let i = 0; i < bytes.length; i += 8192)
    text += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return btoa(text);
}
/**
 * 与 nvn_deswizzle.c 源码完全一致的二维 BC3 偏移；仅接受固定的主面板几何尺寸。这不会改变通用解码器或编码器限制。
 * Source-exact 2D BC3 offset from nvn_deswizzle.c; only the pinned main-panel
 * geometry is accepted. This does not change generic decoder/encoder limits.
 */
export function widenMainPanelBlocks(source) {
  if (!(source instanceof Uint8Array) || source.length < 40 || source.length > 8 * 1024 * 1024)
    fail("STASH_MAIN_BYTES");
  const h = new DataView(source.buffer, source.byteOffset, source.length);
  if (
    h.getUint32(0, true) !== 0x31417053 ||
    h.getUint16(4, true) !== 61 ||
    h.getUint16(6, true) !== 1440 ||
    h.getUint32(8, true) !== 2888 ||
    h.getUint32(12, true) !== 881 ||
    h.getUint16(20, true) !== 2 ||
    source[23] > 5
  )
    fail("STASH_MAIN_GEOMETRY");
  const rowBytes = (2888 / 4) * 16,
    rows = Math.ceil(881 / 4);
  let shift = source[23];
  while (shift && rows <= 8 * 2 ** (shift - 1)) shift--;
  const blockHeight = 2 ** shift,
    gobsX = Math.ceil(rowBytes / 64),
    blockBytes = 512 * blockHeight;
  const storage = gobsX * Math.ceil(rows / (8 * blockHeight)) * blockBytes;
  if (source.length !== storage + 40 || h.getUint32(32, true) !== storage)
    fail("STASH_MAIN_STORAGE");
  const output = new Uint8Array((1640 / 4) * 16 * rows);
  for (let y = 0; y < rows; y++) {
    for (let block = 0; block < 410; block++) {
      const originalBlock = block < 87 ? block : block - 49;
      const x = (361 + originalBlock) * 16;
      const gob =
        Math.floor((x % 64) / 32) * 256 +
        Math.floor((y % 8) / 2) * 64 +
        Math.floor((x % 32) / 16) * 32 +
        (y % 2) * 16;
      const offset =
        Math.floor(y / (8 * blockHeight)) * gobsX * blockBytes +
        Math.floor(x / 64) * blockBytes +
        Math.floor((y % (8 * blockHeight)) / 8) * 512 +
        gob;
      output.set(source.subarray(40 + offset, 40 + offset + 16), (y * 410 + block) * 16);
    }
  }
  // 编码器模板元数据归一化为一个物理帧。以 width1640 打包后，最终显示
  // 宽度恢复为原生 fw+EX=1636。
  // Encoder template metadata is normalized to one physical frame. Final display
  // width is restored to native fw+EX=1636 after packing width1640.
  const templateHeader = source.slice(0, 40),
    t = new DataView(templateHeader.buffer);
  t.setUint16(6, 1444, true);
  t.setUint32(8, 1444, true);
  t.setUint16(20, 1, true);
  return { templateHeader, linearBC3: output, width: 1640, height: 881, frameWidth: 1636 };
}
export async function generateRemainingArt({
  logicalAssets,
  assetWorker,
  decodeAtlas,
  textureEncoder,
  signal,
}) {
  const owned = createRemainingArtOwnedBytes(),
    files = {};
  let inputBytes = 0,
    outputBytes = 0,
    success = false;
  const check = () => {
    if (signal?.aborted) fail("REMAINING_ART_CANCELLED");
  };
  const own = (value) => {
    const bytes = asBytes(value);
    if (!bytes) fail("REMAINING_ART_BYTES");
    owned.add(bytes);
    return bytes;
  };
  const keep = (path, value) => {
    const bytes = own(value);
    if (
      !bytes.length ||
      bytes.length > 8 * 1024 * 1024 ||
      (outputBytes += bytes.length) > 32 * 1024 * 1024 ||
      Object.hasOwn(files, path)
    )
      fail("REMAINING_ART_OUTPUT_LIMIT");
    files[path] = bytes.slice();
  };
  let sourcePaths;
  async function source(rel) {
    check();
    const path = sourcePaths.get(rel);
    const raw = own(await logicalAssets.read(path, { maxBytes: 8 * 1024 * 1024, signal }));
    check();
    if (
      raw.length < 40 ||
      raw.length > 8 * 1024 * 1024 ||
      (inputBytes += raw.length) > 32 * 1024 * 1024
    )
      fail("REMAINING_ART_INPUT_LIMIT");
    const h = new DataView(raw.buffer, raw.byteOffset, raw.length);
    if (h.getUint32(0, true) !== 0x31417053 || h.getUint16(4, true) !== 61)
      fail("REMAINING_ART_HEADER");
    return raw;
  }
  async function decode(raw) {
    const result = await decodeAtlas(raw, { signal });
    const rgba = own(result.pixels);
    check();
    if (
      result.pixelFormat !== 70 ||
      result.bytesPerPixel !== 4 ||
      !Number.isSafeInteger(result.width) ||
      !Number.isSafeInteger(result.height) ||
      result.width * result.height > 4 * 1024 * 1024 ||
      rgba.length !== result.width * result.height * 4
    )
      fail("REMAINING_ART_DECODE");
    return { width: result.width, height: result.height, rgba };
  }
  async function transform(kind, name, images) {
    check();
    const input = Object.fromEntries(
      Object.entries(images).map(([key, im]) => [
        key,
        { width: im.width, height: im.height, rgba: base64(im.rgba) },
      ]),
    );
    const payload = own(new TextEncoder().encode(JSON.stringify(input)));
    if (payload.length > 20 * 1024 * 1024) fail("REMAINING_ART_PAYLOAD_LIMIT");
    let result;
    try {
      result = await assetWorker.run("remaining-art", payload, { kind, name });
    } finally {
      owned.release(payload);
    }
    // 先接管有效返回缓冲区；缺失或损坏的 RGBA 也必须清理其余文件。
    // Own valid returned buffers first so missing/invalid RGBA still cleans up other files.
    for (const value of Object.values(result?.files ?? {})) {
      const data = asBytes(value);
      if (data) owned.add(data);
    }
    const rgba = own(result.files?.["image.rgba"]);
    for (const bytes of Object.values(result.files ?? {})) own(bytes);
    check();
    if (
      !Number.isSafeInteger(result.width) ||
      !Number.isSafeInteger(result.height) ||
      result.width < 1 ||
      result.width > 1640 ||
      result.height < 1 ||
      result.height > 1024 ||
      rgba.length !== result.width * result.height * 4
    )
      fail("REMAINING_ART_RESULT");
    const names = Object.keys(result.files).sort().join();
    if (names !== (kind === "vault" ? "VaultBottom.png,image.rgba" : "image.rgba"))
      fail("REMAINING_ART_OUTPUT_SET");
    return { ...result, rgba };
  }
  async function encode(template, image) {
    check();
    const encoded = await textureEncoder.encodeAndPack(
      {
        templateHeader: template.slice(0, 40),
        rgba: image.rgba,
        width: image.width,
        height: image.height,
      },
      { signal },
    );
    for (const key of ["sprite", "linearBC3"]) if (encoded[key]) own(encoded[key]);
    check();
    return own(encoded.sprite);
  }
  try {
    const names = [
      "mainpanelbg",
      ...Object.values(BG),
      ...Object.values(ADV),
      "charactersheet/v2/charactersheet_bg",
      "inventorypanel/v2/inventorybg_classic",
    ];
    const paths = resolveLogicalAssetSuffixes(
      logicalAssets,
      names.map((name) => `/ui/controller/panel/${name}.sprite`),
      check,
    );
    sourcePaths = new Map(names.map((name, index) => [name, paths[index]]));
    {
      const before = owned.snapshot();
      const main = own(await source("mainpanelbg"));
      const wide = widenMainPanelBlocks(main);
      own(wide.templateHeader);
      own(wide.linearBC3);
      const packed = await textureEncoder.packLinearBC3(wide, { signal });
      const wideSprite = own(packed.sprite);
      check();
      new DataView(wideSprite.buffer, wideSprite.byteOffset, wideSprite.length).setUint16(
        6,
        wide.frameWidth,
        true,
      );
      keep("NativeHUD/widestash/MainPanelBG_Wide.sprite", wideSprite);
      // keep 已复制输出，主面板来源及中间结果不再使用。
      // keep copied the output; main-panel sources and intermediates have no remaining consumers.
      owned.releaseSince(before);
    }
    const sources = {},
      images = {};
    for (const [name, rel] of Object.entries({ ...BG, ...ADV })) {
      sources[name] = await source(rel);
      images[name] = await decode(sources[name]);
      const h = new DataView(sources[name].buffer, sources[name].byteOffset, sources[name].length);
      if (h.getUint16(6, true) !== 652 || images[name].width !== 652 || h.getUint16(20, true) !== 1)
        fail("REMAINING_ART_STASH_SOURCE");
    }
    const character = await decode(await source("charactersheet/v2/charactersheet_bg"));
    let personal;
    for (const name of Object.keys({ ...BG, ...ADV })) {
      const kind = Object.hasOwn(BG, name)
        ? "stash"
        : name === "AdvancedStash_Gems_Console"
          ? "trove"
          : "advanced";
      const inputs =
        kind === "stash"
          ? { image: images[name], personal: images.StashPanelBGPersonal }
          : kind === "trove"
            ? {
                gems: images.AdvancedStash_Gems_Console,
                runes: images.AdvancedStash_Runes_Console,
                cons: images.AdvancedStash_Consumables_Console,
                character,
              }
            : { image: images[name] };
      const before = owned.snapshot();
      const result = await transform(kind, name, inputs);
      const sprite = await encode(sources[name], result);
      keep(`NativeHUD/widestash/${name}_wide.sprite`, sprite);
      if (name === "StashPanelBGPersonal") personal = sprite;
      // 保留个人仓库的有损精灵供封面往返，其他新中间缓冲区已使用完毕。
      // Preserve the lossy personal-stash sprite for the cover round trip; other new intermediates are consumed.
      owned.releaseSince(before, personal ? [personal] : []);
    }
    // 原生封面来自有损编码后的 widestash 结果，而非
    // 编码前的 RGBA。保留这次往返及其原始金框保护检查。
    // Native cover is derived from the lossy encoded widestash result, not the
    // pre-encoding RGBA. Keep this round trip and its original gold-box guard.
    {
      const before = owned.snapshot();
      const vault = await transform("vault", "VaultBottom", { image: await decode(personal) });
      keep("NativeHUD/widestash/VaultBottom.sprite", await encode(personal, vault));
      keep("NativeHUD/widestash/VaultBottom.png", vault.files["VaultBottom.png"]);
      owned.releaseSince(before);
      owned.release(personal);
      personal = undefined;
    }
    {
      const before = owned.snapshot();
      const inventory = await source("inventorypanel/v2/inventorybg_classic");
      const pouch = await transform("pouch", "PouchBox", { image: await decode(inventory) });
      keep("NativeHUD/pouch/PouchBox.sprite", await encode(inventory, pouch));
      owned.releaseSince(before);
    }
    check();
    success = true;
    return { files };
  } finally {
    owned.dispose();
    if (!success) for (const bytes of Object.values(files)) bytes.fill(0);
  }
}

// 用底层缓冲区识别别名；阶段清理不触碰先前仍被共享的模板/图像。
// Identify aliases by backing buffer; stage cleanup never touches earlier shared templates/images.
export function createRemainingArtOwnedBytes() {
  const values = new Set();
  const release = (buffer) => {
    for (const value of values)
      if (value.buffer === buffer) {
        value.fill(0);
        values.delete(value);
      }
  };
  return Object.freeze({
    add(value) {
      values.add(value);
    },
    snapshot() {
      return new Set([...values].map((value) => value.buffer));
    },
    release(value) {
      release(value.buffer);
    },
    releaseSince(before, preserve = []) {
      const protectedBuffers = new Set([...before, ...preserve.map((value) => value.buffer)]);
      for (const value of values) if (!protectedBuffers.has(value.buffer)) release(value.buffer);
    },
    dispose() {
      for (const value of values) value.fill(0);
      values.clear();
    },
  });
}
