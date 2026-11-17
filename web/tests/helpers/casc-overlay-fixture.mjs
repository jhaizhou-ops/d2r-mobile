// 验证 casc-overlay-fixture 的约定与边界；依据 web/tests/helpers 及本文件的测试断言。
// Verify casc-overlay-fixture contracts and boundaries against web/tests/helpers and this file's assertions.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
const base = new URL("../../toolchain/asset-generation-spike/", import.meta.url);
const script = new URL("test/casc-overlay-oracle.py", base);
execFileSync("python3", [new URL("scripts/package-assets.py", base).pathname, "casc-overlay"]);
export const fixture = JSON.parse(
  execFileSync("python3", [script.pathname], { maxBuffer: 4 * 1024 * 1024 }),
);
export function source(changes = {}) {
  const reads = [];
  const files = Object.entries({ ...fixture.files, ...changes }).map(([path, encoded]) => {
    const bytes = Buffer.from(encoded, "base64");
    return {
      path,
      size: BigInt(bytes.length),
      async readAt(start, count) {
        reads.push({ path, start, count });
        return new Uint8Array(bytes.subarray(Number(start), Number(start) + count));
      },
    };
  });
  return { files, reads, getFile: (path) => files.find((f) => f.path === path) };
}
export function worker() {
  return {
    close() {},
    async run(operation, payload, options) {
      assert.equal(operation, "casc-overlay");
      const result = JSON.parse(
        execFileSync("python3", [script.pathname, "serialize"], {
          input: JSON.stringify({ payload: Buffer.from(payload).toString("base64"), options }),
          maxBuffer: 4 * 1024 * 1024,
        }),
      );
      result.files = Object.fromEntries(
        Object.entries(result.files).map(([path, b64]) => [
          path,
          Uint8Array.from(Buffer.from(b64, "base64")).buffer,
        ]),
      );
      return result;
    },
  };
}
export const jobs = () =>
  fixture.replacements.map((j) => ({
    ...j,
    bytes: Uint8Array.from(Buffer.from(j.bytes, "base64")),
  }));
