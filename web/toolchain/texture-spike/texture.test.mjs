// 验证 texture 的约定与边界；依据 ./dist/texture.mjs、./conformance.mjs 及本文件的测试断言。
// Verify texture contracts and boundaries against ./dist/texture.mjs、./conformance.mjs and this file's assertions.
import test from "node:test";
import { readFile } from "node:fs/promises";
import createModule from "./dist/texture.mjs";
import { conformance } from "./conformance.mjs";
test("bounded NVN native/WASM synthetic differential corpus", async () => {
  const fixtures = JSON.parse(
    await readFile(new URL("./.build/fixtures.json", import.meta.url), "utf8"),
  );
  const wasmBinary = await readFile(new URL("./dist/texture.wasm", import.meta.url));
  await conformance(createModule, fixtures, { wasmBinary });
});
