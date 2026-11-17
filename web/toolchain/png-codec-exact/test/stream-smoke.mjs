// 使用固定 zlib 1.2.13 原生字节样本，检查真实 WASM 的有界流式输出。
// Check bounded streaming output in actual WASM against pinned native zlib 1.2.13 byte fixtures.
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { createPngCodec } from "../codec.mjs";

const codec = await createPngCodec(
  await readFile(new URL("../dist/png-codec.wasm", import.meta.url)),
);
const fixture = {
  schema: 1,
  genericZlib: "1.2.13",
  pattern: "byte[index] = index % 251",
  oracle:
    "Existing pinned native codec zlib_begin(9), zlib_push(length,1), no Python zlib fallback",
  nativeOracleSha256: "cb50236c635d7b63f5f81152bdf77f5c22982f0de42420017035febfb654af69",
  generic: [
    {
      kind: "pattern",
      length: 0,
      compressed: "eNoDAAAAAAE=",
    },
    {
      kind: "pattern",
      length: 1,
      compressed: "eNpjAAAAAQAB",
    },
    {
      kind: "pattern",
      length: 32768,
      compressed:
        "eNrtz9ciEAAAQFEULUSKStk7u6FNUqkQkuwQGtolWRXaNGgQ2hlRkVS2lhGKttUUokTZnnryF/f8wREQFBo2XFhkxMhRo8eIiomPlZAcJzV+grTMxEmTZadMlZNXUFRSVlFVU9fQ1JqmraOrp29gOH3GzFlGs+fMnTd/wUJjk0Wmi82WLF1mvnzFSgtLq1XWNrar7dbYr3VwdHJ2cXVb5+7hud7L22fDxk2bfbds3bZ9x85du/f47fXfFxAYFByy/8DB0LDwQ4ePHD12/ERE5MlTp89ERZ89d/5CTOzFuPiES5evXL12/cbNxKTklFupabfv3E3PuJd5P+vBw0fZObl5+QWFRY+fPH32vLiktOxFeUXly1dV1a/fvH33/sPHmtq6+oZPn798/fa98UdTc8vP1rZfv9v/dHT+/dfV3dPb1z8wKECdOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KlTp06dOnXq1KkP1f8DB5ZlLg==",
    },
    {
      kind: "pattern",
      length: 65536,
      compressed:
        "eNrtz0OCEAAAAMDNtrnZtm3btm3XZtu2bdu2bVunjn1i5gcTECx4iJChQocJGy58hIiRIkeJGi16jJixYseJGy9+goSJEgcmSZoseYqUqVKnSZsufYaMmTJnyZote46cuXLnyZsvf4GChQoXKVqseImSpUqXKVuufIWKlSpXqVqteo2atWrXqVuvfoOGjRo3adqseYuWrVq3aduufYeOnTp36dqte4+evXr36duv/4CBgwYPGTosaPiIkaNGjxk7bvyEiZMmT5k6bfqMmbNmz5k7b/6ChYsWL1m6bPmKlatWr1m7bv2GjZs2b9m6bfuOnbt279m7b/+Bg4cOHzl67PiJk6dOnzl77vyFi5cuX7l67fqNm7du37l77/6Dh48eP3n67PmLl69ev3n77v2Hj58+f/n67fuPn79+//kboK6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urv6//g9zMvw8",
    },
  ],
};
const cases = fixture.generic.filter(
  (item) => item.kind === "pattern" && [0, 1, 32768, 65536].includes(item.length),
);
assert.equal(cases.length, 4);
assert.equal(fixture.genericZlib, "1.2.13");
assert.equal(codec.genericZlibVersion, "1.2.13");
for (const item of cases) {
  const raw = Uint8Array.from({ length: item.length }, (_, index) => index % 251);
  const expected = new Uint8Array(Buffer.from(item.compressed, "base64"));
  assert.equal(raw.length, item.length);
  for (const step of [137, 65536]) {
    const input = {
      async *[Symbol.asyncIterator]() {
        for (let offset = 0; offset < raw.length; offset += step)
          yield raw.subarray(offset, offset + step);
      },
    };
    const chunks = [];
    let size = 0;
    for await (const chunk of codec.compressZlibStream(input, {
      maxInputBytes: raw.length,
      maxOutputBytes: expected.length,
    })) {
      assert.ok(chunk.length <= 1024 * 1024);
      size += chunk.length;
      assert.ok(size <= expected.length);
      chunks.push(chunk);
    }
    assert.deepEqual(new Uint8Array(Buffer.concat(chunks)), expected);
  }
}
console.log("Pinned zlib WASM stream smoke: 4 fixtures, 8 fragmented streams; exact bytes.");
