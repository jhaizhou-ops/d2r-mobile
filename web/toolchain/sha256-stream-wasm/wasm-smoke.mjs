// 对实际构建的零导入 WASM 做小型字节与生命周期检查；不运行多 GiB 输入。
// Check actual zero-import WASM bytes and lifecycle with small inputs; no multi-GiB input.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { createSha256FileHasher } from "./adapter.mjs";
const bytes = readFileSync(new URL("dist/sha256.wasm", import.meta.url));
const report = JSON.parse(readFileSync(new URL("dist/build-report.json", import.meta.url), "utf8"));
assert.equal(createHash("sha256").update(bytes).digest("hex"), report.sha256);
assert.equal(bytes.length, report.bytes);
const hasher = await createSha256FileHasher(bytes);
for (const length of [0, 1, 55, 56, 63, 64, 65, 65535, 65536, 65537, 131089]) {
  const raw = Uint8Array.from({ length }, (_, index) => (index * 73 + 19) % 256);
  const actual = await hasher.hashFile(new Blob([raw]));
  assert.deepEqual(actual, {
    sha256: createHash("sha256").update(raw).digest("hex"),
    bytesHashed: length,
  });
}
const controller = new AbortController();
controller.abort();
await assert.rejects(hasher.hashFile(new Blob(), { signal: controller.signal }), {
  name: "AbortError",
});
console.log(JSON.stringify({ wasmVerified: true, cases: 11, greaterThan4GiBCarryVerified: false }));
