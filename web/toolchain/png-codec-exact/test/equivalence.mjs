// 验证 equivalence 的约定与边界；依据 ../codec.mjs 及本文件的测试断言。
// Verify equivalence contracts and boundaries against ../codec.mjs and this file's assertions.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createPngCodec } from "../codec.mjs";
const codec = await createPngCodec(
  readFileSync(new URL("../dist/png-codec.wasm", import.meta.url)),
);
const fixture = JSON.parse(readFileSync(new URL("fixtures.json", import.meta.url)));
assert.equal(codec.zlibVersion, "1.3.1");
assert.equal(codec.genericZlibVersion, "1.2.13");
const bytes = (b64) => new Uint8Array(Buffer.from(b64, "base64"));
for (const test of fixture.cases) {
  const raw = bytes(test.rgba),
    png = bytes(test.png);
  assert.deepEqual(
    codec.encodeRgba(raw, test.width, test.height),
    png,
    `PNG ${test.width}x${test.height} ${test.kind}`,
  );
  assert.deepEqual(codec.decodeRgba(png), {
    width: test.width,
    height: test.height,
    channels: 4,
    data: raw,
  });
}
const close = codec.decodeRgba(
  readFileSync(new URL("../../../../dm/ios/NativeHUD/touch/close.png", import.meta.url)),
);
assert.deepEqual([close.width, close.height, close.channels], [24, 33, 4]);
for (const test of fixture.generic) {
  let raw;
  if (test.kind === "pattern") {
    raw = new Uint8Array(test.length);
    for (let i = 0; i < raw.length; i++) raw[i] = i % 251;
  } else raw = bytes(test.raw);
  assert.deepEqual(
    codec.compressZlib(raw, { level: 9 }),
    bytes(test.compressed),
    `CPython zlib ${test.length}`,
  );
  // 真实 WASM 流与同一固定 CPython 字节基准比较；不采用模拟 ABI。
  // Compare the real WASM stream against the same pinned CPython bytes, without ABI mocks.
  const chunks = [];
  const input = {
    async *[Symbol.asyncIterator]() {
      for (let offset = 0; offset < raw.length; offset += 65536)
        yield raw.slice(offset, offset + 65536);
    },
  };
  for await (const chunk of codec.compressZlibStream(input, {
    maxInputBytes: raw.length,
    maxOutputBytes: raw.length + 1048576,
  }))
    chunks.push(chunk);
  assert.deepEqual(
    new Uint8Array(Buffer.concat(chunks)),
    bytes(test.compressed),
    `Streaming CPython zlib ${test.length}`,
  );
}
const good = bytes(fixture.cases[2].png);
const bad = good.slice();
bad[45] ^= 1;
const trailing = new Uint8Array(good.length + 1);
trailing.set(good);
for (const data of [new Uint8Array([1]), bad, trailing, good.slice(0, -1)])
  assert.throws(() => codec.decodeRgba(data), /rejected/);
assert.throws(() => codec.encodeRgba(new Uint8Array(4), 0, 1), /limit/);
assert.throws(() => codec.encodeRgba(new Uint8Array(4), 8192, 8192), /limit/);
assert.throws(() => codec.compressZlib(new Uint8Array(0), { level: 6 }), /only level 9/);
assert.throws(
  () => codec.compressZlib(new Uint8Array(0), { level: 9, memLevel: 8 }),
  /only level 9/,
);
console.log(
  `WASM exactness: ${fixture.cases.length} PNG byte/pixel cases; ${fixture.generic.length} CPython zlib streams; fixed close button; invalid input rejected.`,
);
