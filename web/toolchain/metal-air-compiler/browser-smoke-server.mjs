// 本地开发服务器只暴露明确列出的编译器和自编测试资产，不读取游戏文件。
// Local developer server exposes only explicitly listed compiler and authored test assets, never game files.
import { createServer } from "node:http";
import process from "node:process";
import { readFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { verifyAirCompilerAssets } from "../../runtime-build/air-compiler-assets.mjs";
import { airCompilerReceiptOutput } from "../../air-compiler-runtime-contract.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
if (args.length !== 4 || args[0] !== "--build" || args[2] !== "--sha256")
  throw Error(
    "Usage: browser-smoke-server.mjs --build /authenticated/release/dist --sha256 RECEIPT_SHA256",
  );
const build = resolve(args[1]);
const report = await verifyAirCompilerAssets(build, args[3]);
const routes = new Map();
async function add(url, path, type) {
  const data = await readFile(path);
  routes.set(url, { data, type });
  return data;
}
const factory = await add("/compiler.mjs", resolve(build, "d2r_air_wasm.mjs"), "text/javascript");
const wasm = await add("/compiler.wasm", resolve(build, "d2r_air_wasm.wasm"), "application/wasm");
const hash = (data) => createHash("sha256").update(data).digest("hex");
if (
  hash(factory) !== airCompilerReceiptOutput(report, "d2r_air_wasm.mjs").sha256 ||
  hash(wasm) !== airCompilerReceiptOutput(report, "d2r_air_wasm.wasm").sha256
)
  throw Error("Compiler bytes changed after authentication");
const { apiVersion, headerSha256 } = report;
routes.set("/manifest.json", {
  type: "application/json",
  data: JSON.stringify({
    factorySha256: hash(factory),
    wasmSha256: hash(wasm),
    headerSha256,
    apiVersion,
  }),
});
for (const name of ["browser-smoke-worker.mjs", "browser-smoke-client.mjs"])
  await add(`/web/toolchain/metal-air-compiler/${name}`, resolve(here, name), "text/javascript");
for (const name of [
  "air-compiler-adapter.mjs",
  "air-compiler-contract.mjs",
  "shader-diagnostics.mjs",
  "compiled-shader-boundary.mjs",
])
  await add(`/web/${name}`, resolve(here, "../..", name), "text/javascript");
for (const name of ["compute", "vertex", "fragment"])
  await add(`/fixture-${name}.metal`, resolve(here, `fixtures/msl-${name}.metal`), "text/plain");
routes.set("/", {
  type: "text/html",
  data:
    '<!doctype html><meta charset="utf-8"><title>AIR compiler browser smoke</title>' +
    '<button id="cancel">Cancel</button><pre id="status">Loading compiler files…</pre>' +
    '<script type="module" src="/web/toolchain/metal-air-compiler/browser-smoke-client.mjs"></script>',
});
const server = createServer((request, response) => {
  const route = routes.get(request.url);
  if (request.method !== "GET" || !route) {
    response.writeHead(404).end();
    return;
  }
  response.writeHead(200, {
    "Content-Type": route.type,
    "Cache-Control": "no-store",
    "X-Content-Type-Options": "nosniff",
  });
  response.end(route.data);
});
server.listen(0, "127.0.0.1", () => {
  console.log(`Open http://127.0.0.1:${server.address().port}/ in desktop Chromium.`);
  console.log("Developer smoke only; no full-game or Apple runtime acceptance claim.");
});
