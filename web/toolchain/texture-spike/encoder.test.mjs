// 验证 encoder 的约定与边界；依据 ./dist/texture.mjs、./browser-texture-encoder.mjs 及本文件的测试断言。
// Verify encoder contracts and boundaries against ./dist/texture.mjs、./browser-texture-encoder.mjs and this file's assertions.
import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import createModule from "./dist/texture.mjs";
import { createTextureEncoder, validateEncodeRequest } from "./browser-texture-encoder.mjs";
const bytes = (s) => new Uint8Array(Buffer.from(s, "base64"));
test("original native vs WASM BC3 packing and bounded guards", async () => {
  const fixtures = JSON.parse(
    await readFile(new URL("./.build/encoder-fixtures.json", import.meta.url), "utf8"),
  );
  const wasmBinary = await readFile(new URL("./dist/texture.wasm", import.meta.url));
  const encoder = await createTextureEncoder(
    createModule,
    () => {
      throw new Error("Encoding is a separate source-pinned dependency");
    },
    { wasmBinary },
  );
  for (const f of fixtures) {
    assert.deepEqual(
      encoder.packLinearBC3(bytes(f.templateHeader), bytes(f.linearBC3), f.width, f.height).sprite,
      bytes(f.sprite),
    );
  }
  const f = fixtures[0],
    header = bytes(f.templateHeader);
  for (const [w, h] of [
    [0, 1],
    [-1, 1],
    [8193, 1],
    [2049, 2049],
    [1.5, 1],
    [NaN, 1],
  ])
    assert.throws(() => validateEncodeRequest(header, w, h));
  assert.throws(() => encoder.packLinearBC3(header, new Uint8Array(15), 1, 1));
  assert.throws(() => encoder.packLinearBC3(header.subarray(0, 39), new Uint8Array(16), 1, 1));
  const multi = header.slice();
  new DataView(multi.buffer).setUint16(20, 2, true);
  assert.throws(() => validateEncodeRequest(multi, 1, 1), /single-frame/);
  for (const offset of [0, 4, 6]) {
    const invalid = header.slice();
    invalid[offset] = 0;
    assert.throws(() => validateEncodeRequest(invalid, 1, 1));
  }
  const bad = header.slice();
  bad[23] = 255;
  assert.throws(() => validateEncodeRequest(bad, 1, 1));
});
