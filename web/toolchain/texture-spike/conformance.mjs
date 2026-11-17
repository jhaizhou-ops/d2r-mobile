// 检查纹理实现与参考输入的一致性；依据 ./browser-texture.mjs 的模块与调用约定。
// Check texture implementation conformance against reference inputs using the module and calling contracts in ./browser-texture.mjs.
import { createTextureDecoder } from "./browser-texture.mjs";
function expect(condition, message) {
  if (!condition) throw new Error(message);
}
function bytes(text) {
  return Uint8Array.from(atob(text), (char) => char.charCodeAt(0));
}
function same(a, b) {
  return a.length === b.length && a.every((value, index) => value === b[index]);
}
export async function conformance(factory, fixtures, moduleOptions) {
  const decoder = await createTextureDecoder(factory, moduleOptions);
  let checks = 0;
  for (const fixture of fixtures) {
    const info = decoder.layout(fixture.descriptor, { level: fixture.level });
    expect(same(Object.values(info), fixture.info), "Layout parity mismatch");
    const result = decoder.decode(fixture.descriptor, bytes(fixture.source), {
      level: fixture.level,
    });
    expect(same(result.pixels, bytes(fixture.pixels)), "Native/WASM decoded pixel mismatch");
    checks++;
  }
  const d = {
    target: 1,
    format: 0x44,
    flags: 0,
    samples: 0,
    width: 16,
    height: 16,
    depth: 1,
    levels: 1,
    stride: 0,
    pkg_block_h: 0,
  };
  function rejects(fn, code) {
    let error;
    try {
      fn();
    } catch (caught) {
      error = caught;
    }
    expect(error?.code === code, `Expected rejection ${code}, got ${error?.code}`);
    checks++;
  }
  for (const [change, code] of [
    [{ format: 0x79 }, -2],
    [{ target: 2 }, -3],
    [{ depth: 2 }, -3],
    [{ samples: 2 }, -3],
    [{ width: 0 }, -1],
    [{ height: -1 }, -1],
    [{ width: 8193 }, -1],
    [{ width: 1.5 }, -1],
    [{ levels: 32 }, -1],
    [{ levels: 6 }, -1],
    [{ pkg_block_h: 7 }, -1],
    [{ flags: 0x10, levels: 2 }, -3],
    [{ flags: 0x10, stride: 1 }, -1],
    [{ width: 8192, height: 8192 }, -4],
    [{ unknown: 1 }, -1],
  ])
    rejects(() => decoder.layout({ ...d, ...change }), code);
  rejects(() => decoder.layout(d, { level: 1 }), -1);
  rejects(() => decoder.layout(d, { level: NaN }), -1);
  rejects(() => decoder.layout(d, { layer: 1 }), -1);
  rejects(() => decoder.decode(d, new Uint8Array(1)), -5);
  rejects(() => decoder.decode(d, new Uint16Array(512)), -5);
  const v61fixture = fixtures.find(
    (f) => f.descriptor.format === 0x44 && f.descriptor.width === 1 && f.level === 0,
  );
  const source = bytes(v61fixture.source);
  const sprite = new Uint8Array(40 + source.length);
  const header = new DataView(sprite.buffer);
  header.setUint32(0, 0x31417053, true);
  header.setUint16(4, 61, true);
  header.setUint16(6, 1, true);
  header.setUint32(8, 1, true);
  header.setUint32(12, 1, true);
  sprite[23] = 0;
  sprite.set(source, 40);
  expect(
    same(decoder.decodeV61(sprite).pixels, bytes(v61fixture.pixels)),
    "Synthetic v61 pixel mismatch",
  );
  rejects(() => decoder.decodeV61(sprite.subarray(0, 39)), -5);
  sprite[23] = 255;
  rejects(() => decoder.decodeV61(sprite), -1);
  sprite[23] = 0;
  header.setUint16(4, 31, true);
  rejects(() => decoder.decodeV61(sprite), -2);
  // 强制堆增长超过初始 16 MiB，并使用独立的全零 BC3 参考结果。
  // Force growth beyond the initial 16 MiB heap, with an independent zero-BC3 oracle.
  const large = { ...d, flags: 0x10, width: 2049, height: 2049 };
  const zeros = new Uint8Array(decoder.layout(large).storageBytes);
  const grown = decoder.decode(large, zeros);
  expect(
    grown.pixels.length === 2049 * 2049 * 4 && grown.pixels.every((v) => v === 0),
    "Heap growth / zero-BC3 oracle failed",
  );
  const redDesc = { ...d, format: 0x41, flags: 0x10, width: 4, height: 4 };
  const red = new Uint8Array(decoder.layout(redDesc).storageBytes);
  red.set([0, 248, 0, 0, 0, 0, 0, 0]);
  expect(
    decoder.decode(redDesc, red).pixels.every((v, i) => v === [255, 0, 0, 255][i % 4]),
    "BC1 known-red oracle failed",
  );
  // 每次畸形请求和堆增长后，有效输入仍须正常工作。
  // Valid input still works after every malformed request and heap growth.
  expect(decoder.layout(d).width === 16, "Decoder recovery failed");
  return { checks: checks + 4, fixtures: fixtures.length };
}
