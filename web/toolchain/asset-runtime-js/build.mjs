// 同一 CI 作业内组装已构建的精确后端；不安装工具、不下载运行时、不启动浏览器。 依据各后端构建报告、来源固定记录与资产清单约定。
// Assemble already-built exact backends in the same CI job; no tool installation, runtime downloads or browser launch. Based on backend build reports, source provenance pins and the asset inventory contract.
import process from "node:process";
import { ASSET_SOURCE_GATES, verifyAssetSources } from "../verify-asset-sources.mjs";
import { readHashWasmEmbeddedNotices } from "../../scripts/frontend-notices.mjs";
import { readFile, writeFile, mkdir, mkdtemp, rename, rm } from "node:fs/promises";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { verifyWasmModule } from "./wasm-contract.mjs";
import { validateAssetRuntimeInventory } from "./payload-policy.mjs";
import { OPERATION_LIMITS } from "./contract.mjs";
const here = new URL("./", import.meta.url);
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const dependencies = [
  {
    path: "image-kernels.wasm",
    source: "../image-kernels-rust/dist/image-kernels.wasm",
    report: "../image-kernels-rust/dist/build-report.json",
    factory: "../image-kernels-rust/kernels.mjs",
    create: "createImageKernels",
  },
  {
    path: "png-codec.wasm",
    source: "../png-codec-exact/dist/png-codec.wasm",
    report: "../png-codec-exact/dist/build-report.json",
    factory: "../png-codec-exact/codec.mjs",
    create: "createPngCodec",
  },
  {
    path: "font-rasterizer.wasm",
    source: "../font-rasterizer-exact/dist/font-rasterizer.wasm",
    report: "../font-rasterizer-exact/dist/build-report.json",
    factory: "../font-rasterizer-exact/font.mjs",
    create: "createFontRasterizer",
  },
];
const gates = Object.keys(ASSET_SOURCE_GATES);
const notices = [
  ["asset-runtime-source-provenance.json", "./source-provenance.json"],
  ["asset-transform-source-provenance.json", "../asset-transforms-js/source-provenance.json"],
  ["casc-overlay-source-provenance.json", "../casc-overlay-js/source-provenance.json"],
  ["art-recipe-source-provenance.json", "../art-recipes-js/source-provenance.json"],
  ["remaining-art-source-provenance.json", "../remaining-art-js/source-provenance.json"],
  ["hud-recipe-source-provenance.json", "../hud-recipes-js/provenance.json"],
  ["overlay-recipe-source-provenance.json", "../overlay-recipe-js/source-provenance.json"],
  ["overlay-recipe-data-provenance.json", "../overlay-recipe-js/data-provenance.json"],
  ["numeric-art-source-provenance.json", "../art-kernels/provenance.json"],
  ["hash-wasm-LICENSE.txt", "../../node_modules/hash-wasm/LICENSE"],
  ["hash-wasm-package.json", "../../node_modules/hash-wasm/package.json"],
  ["pixel-kernel-LICENSE-Pillow.txt", "../image-kernels-rust/reference/LICENSE"],
  ["pixel-kernel-source-provenance.json", "../image-kernels-rust/reference/sources.json"],
  ["pixel-kernel-build-report.json", "../image-kernels-rust/dist/build-report.json"],
  ["png-codec-LICENSE-Pillow.txt", "../png-codec-exact/dist/LICENSE-Pillow"],
  ["png-codec-LICENSE-zlib.txt", "../png-codec-exact/dist/LICENSE-zlib"],
  ["png-codec-LICENSE-zlib-legacy.txt", "../png-codec-exact/dist/LICENSE-zlib-legacy"],
  ["png-codec-source-provenance.json", "../png-codec-exact/reference/sources.json"],
  ["png-codec-zlib-source.json", "../png-codec-exact/zlib-source.json"],
  ["png-codec-zlib-legacy-source.json", "../png-codec-exact/zlib-legacy-source.json"],
  ["png-codec-build-report.json", "../png-codec-exact/dist/build-report.json"],
  ["font-source-provenance.json", "../font-rasterizer-exact/reference/sources.json"],
  ["font-freetype-source.json", "../font-rasterizer-exact/freetype-source.json"],
  ["font-build-report.json", "../font-rasterizer-exact/dist/build-report.json"],
  ...[
    "LICENSE",
    "LICENSE-FreeType",
    "LICENSE-FreeType-selection",
    "LICENSE-bdf",
    "LICENSE-pcf",
    "LICENSE-FreeType-zlib",
  ].map((name) => [`font-${name}.txt`, `../font-rasterizer-exact/dist/${name}`]),
];
export const buildPlan = Object.freeze({
  stages: [
    "Build image-kernels-rust, png-codec-exact and font-rasterizer-exact in the existing compiler job",
    "Finish/source-verify all 12 recipe operations, including overlay-recipe",
    "Verify artifact build receipts, zero host imports and actual backend startup",
    "Bundle a closed worker with embedded WASM digests using Bun 1.4.3",
    "Write a four-asset manifest and flat dependency notices directory",
  ],
  command: "bun web/toolchain/asset-runtime-js/build.mjs",
  output: "web/toolchain/asset-runtime-js/dist/",
  sourceGates: gates,
  operations: Object.keys(OPERATION_LIMITS),
  wasm: dependencies.map(({ path, source }) => ({ path, source })),
  productionSwitch:
    "clients.mjs, runtime-build/build.mjs and package.mjs select this exact four-asset closure; same-job CI must build and validate all artifacts before deployment",
});
async function verifySources() {
  const provenance = JSON.parse(await readFile(new URL("source-provenance.json", here), "utf8"));
  if (provenance.schemaVersion !== 1 || Object.keys(provenance.sources).length !== 1)
    throw new Error("Asset protocol provenance drift");
  for (const [path, expected] of Object.entries(provenance.sources)) {
    if (hash(await readFile(new URL(path, new URL("../../../", here)))) !== expected)
      throw new Error(`Asset worker protocol source drift: ${path}`);
  }
  for (const gate of gates) await verifyAssetSources(gate);
}
async function assemble() {
  if (!globalThis.Bun || globalThis.Bun.version !== "1.4.3")
    throw new Error("Run with the existing Bun 1.4.3 build toolchain");
  await verifySources();
  const assets = [],
    contents = new Map();
  for (const dependency of dependencies) {
    const bytes = await readFile(new URL(dependency.source, here));
    const report = JSON.parse(await readFile(new URL(dependency.report, here), "utf8"));
    const sha256 = hash(bytes);
    if (
      !bytes.length ||
      bytes.length > 32 * 1024 * 1024 ||
      report.bytes !== bytes.length ||
      report.sha256 !== sha256 ||
      report.hostImports !== 0
    )
      throw new Error(`Invalid asset WASM build receipt: ${dependency.path}`);
    const module = await WebAssembly.compile(bytes);
    verifyWasmModule(module, dependency.path);
    const factory = await import(new URL(dependency.factory, here));
    const backend = await factory[dependency.create](module);
    if (
      dependency.path === "png-codec.wasm" &&
      (backend.zlibVersion !== "1.3.1" || backend.genericZlibVersion !== "1.2.13")
    )
      throw new Error("Exact zlib versions unavailable");
    assets.push({ path: dependency.path, bytes: bytes.length, sha256 });
    contents.set(dependency.path, bytes);
  }
  const folder = new URL(".build/", here);
  await mkdir(folder, { recursive: true });
  const staging = await mkdtemp(fileURLToPath(new URL("assembly-", folder)));
  try {
    const result = await globalThis.Bun.build({
      entrypoints: [fileURLToPath(new URL("worker.mjs", here))],
      outdir: staging,
      naming: "worker.mjs",
      target: "browser",
      format: "esm",
      minify: true,
      splitting: false,
      sourcemap: "none",
      env: "disable",
      define: { "globalThis.__ASSET_RUNTIME_WASM_INVENTORY__": JSON.stringify(assets) },
    });
    if (!result.success) throw new AggregateError(result.logs, "Asset runtime bundle failed");
    if (result.outputs.length !== 1) throw new Error("Unexpected asset worker output graph");
    const code = await result.outputs[0].text();
    if (
      new globalThis.Bun.Transpiler({ loader: "js" }).scanImports(code).length ||
      code.includes("__ASSET_RUNTIME_WASM_INVENTORY__")
    )
      throw new Error("Asset worker closure is incomplete");
    const worker = new TextEncoder().encode(code);
    if (!worker.length || worker.length > 32 * 1024 * 1024)
      throw new Error("Asset worker size cap");
    assets.push({ path: "worker.mjs", bytes: worker.length, sha256: hash(worker) });
    if (assets.reduce((sum, entry) => sum + entry.bytes, 0) > 96 * 1024 * 1024)
      throw new Error("Asset runtime size cap");
    for (const [name, bytes] of contents) await writeFile(`${staging}/${name}`, bytes);
    validateAssetRuntimeInventory({ version: 1, assets });
    await writeFile(`${staging}/manifest.json`, JSON.stringify({ version: 1, assets }) + "\n");
    await mkdir(`${staging}/licenses`);
    await writeFile(
      `${staging}/licenses/hash-wasm-embedded.txt`,
      await readHashWasmEmbeddedNotices(),
    );
    for (const [name, source] of notices)
      await writeFile(`${staging}/licenses/${name}`, await readFile(new URL(source, here)));
    await writeFile(
      `${staging}/licenses/asset-runtime-build-report.json`,
      JSON.stringify(
        {
          runtime: "asset-runtime-js/1",
          bun: globalThis.Bun.version,
          operations: Object.keys(OPERATION_LIMITS),
          assets,
          sourceGates: gates,
          pythonRuntime: false,
          networkDependencyLoads: false,
        },
        null,
        2,
      ) + "\n",
    );
    await rm(new URL("dist/", here), { recursive: true, force: true });
    await rename(staging, new URL("dist/", here));
    console.log(
      JSON.stringify({
        output: buildPlan.output,
        assets: assets.length,
        bytes: assets.reduce((sum, entry) => sum + entry.bytes, 0),
        pythonRuntime: false,
      }),
    );
  } catch (error) {
    await rm(staging, { recursive: true, force: true });
    throw error;
  }
}
if (process.argv[2] === "--plan") console.log(JSON.stringify(buildPlan, null, 2));
else if (process.argv[2] === "--verify-sources") await verifySources();
else if (process.argv.length === 2) await assemble();
else
  throw new Error("Usage: bun web/toolchain/asset-runtime-js/build.mjs [--plan|--verify-sources]");
