// 依据 dm/ios/tools/gen_merc_art.py 的原始配方生成雇佣兵图像。
// Generate mercenary art from the original dm/ios/tools/gen_merc_art.py recipe.
/**
 * 精确的原始雇佣兵图像合成，不虚构发布哈希。
 * Exact original mercenary image composition, with no invented release hashes.
 */
import { resolveLogicalAssetSuffixes } from "./logical-asset-suffixes.mjs";
export const MERCENARY_INPUTS = Object.freeze({
  "hirelinginventory/v2/hirelinginventorybg": Object.freeze([652, 704]),
  "inventorypanel/v2/inventorybg_classic": Object.freeze([507, 700]),
  mainpanelbg: Object.freeze([2888, 881]),
});
const fail = (code) => {
  throw new Error(code);
};
const bytes = (v) =>
  v instanceof Uint8Array ? v : v instanceof ArrayBuffer ? new Uint8Array(v) : null;
const base64 = (value) => {
  let text = "";
  for (let i = 0; i < value.length; i += 8192)
    text += String.fromCharCode(...value.subarray(i, i + 8192));
  return btoa(text);
};
/**
 * logicalAssets 必须是已认证提取能力。编码器及资源工作线程必须已由其所属任务控制器加载并密封。
 * logicalAssets must be the authenticated extraction capability. Encoder and
 * asset worker must already be loaded/sealed by their owning job controller.
 */
export async function generateMercenaryArt({
  logicalAssets,
  assetWorker,
  decodeAtlas,
  textureEncoder,
  signal,
}) {
  const owned = createMercenaryOwnedBytes(),
    output = [];
  const check = () => {
    if (signal?.aborted) fail("MERCENARY_CANCELLED");
  };
  const own = (value) => {
    const b = bytes(value);
    if (!b) fail("MERCENARY_BYTES");
    owned.add(b);
    return b;
  };
  try {
    const images = {};
    let template,
      total = 0;
    const names = Object.keys(MERCENARY_INPUTS);
    const paths = resolveLogicalAssetSuffixes(
      logicalAssets,
      names.map((name) => `/ui/controller/panel/${name}.sprite`),
      check,
    );
    for (const [index, name] of names.entries()) {
      const [width, height] = MERCENARY_INPUTS[name];
      check();
      const path = paths[index];
      const source = own(await logicalAssets.read(path, { maxBytes: 8 * 1024 * 1024, signal }));
      check();
      if (
        source.length < 40 ||
        source.length > 8 * 1024 * 1024 ||
        (total += source.length) > 12 * 1024 * 1024
      )
        fail("MERCENARY_INPUT_LIMIT");
      const header = own(source.slice(0, 40));
      const h = new DataView(header.buffer, header.byteOffset, 40);
      if (
        h.getUint32(0, true) !== 0x31417053 ||
        h.getUint16(4, true) !== 61 ||
        h.getUint32(8, true) !== width ||
        h.getUint32(12, true) !== height ||
        (name.startsWith("hireling") && h.getUint16(6, true) !== 652)
      )
        fail("MERCENARY_SOURCE_GEOMETRY");
      const decoded = await decodeAtlas(source, { signal });
      const rgba = own(decoded.pixels);
      check();
      if (
        decoded.width !== width ||
        decoded.height !== height ||
        decoded.pixelFormat !== 70 ||
        decoded.bytesPerPixel !== 4 ||
        rgba.length !== width * height * 4
      )
        fail("MERCENARY_ATLAS_GEOMETRY");
      images[name] = { header: base64(header), rgba: base64(rgba) };
      if (name.startsWith("hireling")) template = header;
      // 字符串快照已独立；只保留之后编码所需的 40 字节模板。
      // String snapshots are independent; retain only the 40-byte template needed by later encoding.
      owned.release(source);
      owned.release(rgba);
      if (header !== template) owned.release(header);
    }
    const payload = own(new TextEncoder().encode(JSON.stringify(images)));
    // 序列化后解除字符串引用，不声称擦除字符串或控制垃圾回收时间。
    // Drop string references after serialization; this neither zeroes strings nor controls collection timing.
    for (const name of Object.keys(images)) delete images[name];
    if (payload.length > 20 * 1024 * 1024) fail("MERCENARY_PAYLOAD_LIMIT");
    check();
    let result;
    try {
      result = await assetWorker.run("mercenary-art", payload, {});
    } finally {
      // 等待工作线程使用结束之后才释放请求字节，包括失败路径。
      // Release request bytes only after worker consumption settles, including failure paths.
      owned.release(payload);
    }
    // 先接管所有有效返回缓冲区，避免格式错误跳过清理。
    // Own all returned byte buffers before malformed shapes can bypass cleanup.
    for (const value of Object.values(result?.files ?? {})) {
      const data = bytes(value);
      if (data) owned.add(data);
    }
    if (
      Object.keys(result.files ?? {})
        .sort()
        .join() !== "HirelingInventoryBG.png,mercenary.rgba"
    )
      fail("MERCENARY_OUTPUT_SET");
    const rgba = own(result.files["mercenary.rgba"]),
      png = own(result.files["HirelingInventoryBG.png"]);
    check();
    if (
      result.width !== 652 ||
      result.height !== 704 ||
      rgba.length !== 652 * 704 * 4 ||
      !png.length ||
      png.length > 4 * 1024 * 1024
    )
      fail("MERCENARY_OUTPUT_LIMIT");
    const encoded = await textureEncoder.encodeAndPack(
      { templateHeader: template, rgba, width: 652, height: 704 },
      { signal },
    );
    const sprite = own(encoded.sprite);
    if (encoded.linearBC3) own(encoded.linearBC3);
    check();
    if (
      encoded.width !== 652 ||
      encoded.height !== 704 ||
      sprite.length < 40 ||
      sprite.length > 4 * 1024 * 1024
    )
      fail("MERCENARY_ENCODE_RESULT");
    const files = {
      "HirelingInventoryBG.png": png.slice(),
      "HirelingInventoryBG.sprite": sprite.slice(),
    };
    output.push(...Object.values(files));
    check();
    return { width: 652, height: 704, files };
  } catch (error) {
    for (const value of output) value.fill(0);
    throw error;
  } finally {
    owned.dispose();
  }
}

// 同一底层缓冲区的多个自有视图一并释放，重复释放无操作。
// Release all owned aliases of a backing buffer together; repeated release is a no-op.
export function createMercenaryOwnedBytes() {
  const values = new Set();
  return Object.freeze({
    add(value) {
      values.add(value);
    },
    release(value) {
      for (const owned of values) {
        if (owned.buffer !== value.buffer) continue;
        owned.fill(0);
        values.delete(owned);
      }
    },
    dispose() {
      for (const value of values) value.fill(0);
      values.clear();
    },
    get size() {
      return values.size;
    },
  });
}
