// 在工作线程中执行纹理冒烟用例；依据 ./dist/texture.mjs、./conformance.mjs 的模块与调用约定。
// Execute texture smoke cases inside a worker using the module and calling contracts in ./dist/texture.mjs、./conformance.mjs.
import createModule from "./dist/texture.mjs";
import { conformance } from "./conformance.mjs";
try {
  const fixtures = await (await fetch("./.build/fixtures.json")).json();
  const result = await conformance(createModule, fixtures);
  self.postMessage({ ok: true, ...result });
} catch (error) {
  self.postMessage({ ok: false, message: error.message });
}
