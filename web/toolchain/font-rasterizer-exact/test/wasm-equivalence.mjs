// 验证 wasm-equivalence 的约定与边界；依据 ../font.mjs 及本文件的测试断言。
// Verify wasm-equivalence contracts and boundaries against ../font.mjs and this file's assertions.
import { Buffer } from "node:buffer";
// 仅在实际产物用固定 emsdk 构建完成后运行。
// Run only after the actual artifact is built with the pinned emsdk.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { createFontRasterizer } from "../font.mjs";
const fixture = JSON.parse(readFileSync(new URL("fixtures.json", import.meta.url)));
const kernel = await createFontRasterizer(
  readFileSync(new URL("../dist/font-rasterizer.wasm", import.meta.url)),
);
assert.deepEqual(fixture.backend, {
  pillow: "10.2.0",
  freetype: "2.6.0",
  layout: "BASIC",
  raqm: false,
  pyodide: "0.27.7",
});
let opened = "";
for (const [index, item] of fixture.cases.entries()) {
  const key = `${item.font}/${item.size}`;
  if (key !== opened) {
    const font = fixture.fonts[item.font],
      bytes = readFileSync(font.path);
    assert.equal(createHash("sha256").update(bytes).digest("hex"), font.sha256);
    kernel.open(bytes, item.size);
    opened = key;
  }
  const options = { anchor: item.anchor, stroke: item.stroke };
  if (item.op === "mask") {
    const measured = kernel.measure(item.text, options);
    assert.deepEqual(measured.bbox, item.bbox, `bbox ${index}`);
    assert.equal(measured.length, item.length, `length ${index}`);
    assert.deepEqual(measured.metrics, item.metrics, `metrics ${index}`);
    const result = kernel.mask(item.text, { ...options, start: item.start });
    assert.deepEqual([result.width, result.height], item.maskSize, `mask size ${index}`);
    assert.deepEqual(result.offset, item.offset, `mask offset ${index}`);
    assert.deepEqual(
      Buffer.from(result.pixels),
      Buffer.from(item.mask, "base64"),
      `mask pixels ${index}`,
    );
  } else {
    const result = kernel.draw(
      Buffer.from(item.before, "base64"),
      item.width,
      item.height,
      item.text,
      {
        ...options,
        x: item.pos[0],
        y: item.pos[1],
        fill: item.fill,
        strokeFill: item.strokeFill,
        channels: item.channels,
      },
    );
    assert.deepEqual(
      Buffer.from(result),
      Buffer.from(item.pixels, "base64"),
      `draw pixels ${index}`,
    );
  }
}
kernel.close();
console.log(
  JSON.stringify({
    cases: fixture.cases.length,
    status: "exact",
    comparison: "WASM port vs bundled WASM Pillow10.2/FreeType2.6/BASIC",
    gameFontsVerified: false,
  }),
);
