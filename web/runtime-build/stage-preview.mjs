// 暂存发布预览所需的前端与已校验运行时；依据 ./inventory.mjs、../verification/preview-manifest.mjs 的模块与调用约定。
// Stage frontend files and verified runtimes for the release preview using the module and calling contracts in ./inventory.mjs、../verification/preview-manifest.mjs.
import process from "node:process";
import { mkdir, copyFile, rm, readFile, writeFile } from "node:fs/promises";
import { verifyRuntimeInventory } from "./inventory.mjs";
import { createPreviewManifest } from "../verification/preview-manifest.mjs";

const source = new URL("./out/", import.meta.url);
const preview = new URL("../dist/", import.meta.url);
const destination = new URL("runtime/", preview);
const expected = process.env.EXPECTED_RUNTIME_MANIFEST_SHA256;
const inventory = await verifyRuntimeInventory(source, expected, process.env.GITHUB_SHA || null);
// 固定目标目录仅用于 Bun 生成的输出；不得复制任意目录树，
// 仅暂存经过独立验证的清单条目。
// The fixed destination belongs only to the generated Bun output. Do not copy
// arbitrary trees; only independently verified manifest entries are staged.
await rm(destination, { recursive: true, force: true });
await mkdir(destination, { recursive: true });
for (const entry of inventory.files) {
  const target = new URL(entry.path, destination);
  await mkdir(new URL("./", target), { recursive: true });
  await copyFile(new URL(entry.path, source), target);
}
const htmlUrl = new URL("index.html", preview);
const html = await readFile(htmlUrl, "utf8");
const marker = "__RUNTIME_MANIFEST_SHA256__";
if (html.split(marker).length !== 2)
  throw new Error("Missing or duplicate runtime trust-anchor placeholder");
await writeFile(htmlUrl, html.replace(marker, expected));
await createPreviewManifest(preview, process.env.GITHUB_SHA || null, expected);
console.log(
  `Staged ${inventory.files.length} verified public runtime files; staging does not establish browser or device acceptance.`,
);
