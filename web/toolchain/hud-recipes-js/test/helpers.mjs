// 验证 helpers 的约定与边界；依据 ../python-values.mjs、../support.mjs 及本文件的测试断言。
// Verify helpers contracts and boundaries against ../python-values.mjs、../support.mjs and this file's assertions.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { ordered, put, roundEven, float, parsePython, stringifyPython } from "../python-values.mjs";
import { vitalCharacters, applyPatch, layoutText } from "../support.mjs";
import { generateHud } from "../recipe.mjs";
const values = [
  -4.5, -3.5, -2.5, -1.5, -0.45, -0.35, -0.25, -0.15, -0.05, 0, 0.05, 0.15, 0.25, 0.35, 0.45, 1.5,
  2.5, 3.5, 4.5, 151.2, 1049.5, -304.40909090909093,
];
const oracle = JSON.parse(
  execFileSync(
    "python",
    [
      "-c",
      "import json,sys;v=json.loads(sys.argv[1]);print(json.dumps([[round(x),round(x,1)] for x in v]))",
      JSON.stringify(values),
    ],
    { encoding: "utf8" },
  ),
);
for (let i = 0; i < values.length; i++)
  for (let d = 0; d < 2; d++) assert.equal(roundEven(values[i], d), oracle[i][d]);
const inputs = [
  '{"x": 1.0, "n": -0.0, "large": 99999999999999999999, "2": 2, "1": 1}',
  '{"nested":[1e-5,1e-4,1e15,1e16,0.00000123,1.2345678901234567e-8,"生命"]}',
];
for (const input of inputs) {
  const expected = execFileSync(
    "python",
    [
      "-c",
      "import json,sys;print(json.dumps(json.loads(sys.argv[1]),ensure_ascii=False,indent=1),end='')",
      input,
    ],
    { encoding: "utf8" },
  );
  assert.equal(stringifyPython(parsePython(input)), expected);
}
const object = ordered([
  ["first", 1],
  ["width", 2],
  ["last", 3],
]);
delete object.width;
put(object, "width", 4);
assert.equal(stringifyPython(object), '{\n "first": 1,\n "last": 3,\n "width": 4\n}');
assert.equal(
  stringifyPython({ adv: float(16), center: float(1434) }),
  '{\n "adv": 16.0,\n "center": 1434.0\n}',
);
assert.deepEqual(
  applyPatch({ a: 1, b: 2 }, { dict: { a: { value: 3 } }, order: ["b", "a"] }),
  Object.assign(Object.create(null), { b: 2, a: 3 }),
);
assert.equal(layoutText('\uFEFF{"x": 1, // comment\n}').x, 1);
const metadata = JSON.parse(
  readFileSync(
    new URL("../../asset-generation-spike/public/generated/hud-metadata.json", import.meta.url),
  ),
);
const chars = vitalCharacters(metadata.i18n);
assert.ok(chars.startsWith("生命法力:/ 0123456789"));
assert.ok(chars.includes("Ж") || chars.includes("ж"));
assert.throws(() => vitalCharacters("bad\ttsv"), /HUD_I18N_HEADER/);
await assert.rejects(generateHud(new Uint8Array(), {}, {}), /HUD_MODE_REQUIRED/);
await assert.rejects(
  generateHud(
    new TextEncoder().encode("{}"),
    { mode: "switch-upscaled" },
    { codec: {}, kernels: {}, font: {} },
  ),
  /HUD_METADATA_SET/,
);
console.log(
  JSON.stringify({
    roundCases: values.length * 2,
    jsonCases: inputs.length,
    metadataAndRejections: "passed",
  }),
);
