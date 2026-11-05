// 验证 equivalence 的约定与边界；依据 ../kernels.mjs 及本文件的测试断言。
// Verify equivalence contracts and boundaries against ../kernels.mjs and this file's assertions.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createImageKernels } from "../kernels.mjs";
const api = await createImageKernels(
  readFileSync(new URL("../dist/image-kernels.wasm", import.meta.url)),
);
const fixtures = JSON.parse(readFileSync(new URL("fixtures.json", import.meta.url)));
const decode = (im) => ({ ...im, data: new Uint8Array(Buffer.from(im.data, "base64")) });
let count = 0;
for (const fixture of fixtures) {
  const src = decode(fixture.src);
  let result;
  if (["paste", "alphaComposite"].includes(fixture.op)) {
    const args = [decode(fixture.dst), src, ...fixture.args];
    if (fixture.op === "paste")
      args.push(
        fixture.mask && typeof fixture.mask === "object" ? decode(fixture.mask) : fixture.mask,
      );
    result = api[fixture.op](...args);
  } else if (fixture.op === "point") result = api.point(src, new Uint8Array(fixture.lut));
  else if (fixture.op === "putAlpha") result = api.putAlpha(src, decode(fixture.mask));
  else result = api[fixture.op](src, ...fixture.args);
  assert.deepEqual(
    result,
    fixture.op === "bbox" ? fixture.expected : decode(fixture.expected),
    `case ${count}: ${fixture.op} ${src.width}x${src.height} c${src.channels} ${fixture.args}`,
  );
  count++;
}
const im = { width: 1, height: 1, channels: 4, data: new Uint8Array([12, 34, 56, 0]) };
assert.throws(() => api.crop(im, 0, 0, 8192, 8192), /limit/);
assert.throws(() => api.crop(im, NaN, 0, 1, 1), /Invalid/);
assert.throws(() => api.crop(im, 0.5, 0, 1, 1), /Invalid/);
assert.throws(() => api.flip(im, 2), /Invalid/);
assert.throws(() => api.channel(im, 4), /Invalid/);
assert.throws(
  () => api.paste(im, im, 0, 0, { width: 1, height: 1, channels: 1, data: new Uint8Array(2) }),
  /length/,
);
assert.throws(() => api.convert(im, 2), /mode/);
assert.throws(() => api.maxFilter(im), /supports/);
assert.throws(() => api.point(im, new Uint8Array(256)), /requires/);
console.log(
  `Exact Pillow 10.2.0 pixels/geometry: ${count} cases, including all 65,536 alpha pairs. Bounds rejected.`,
);
