// 仅用于构建打包；不重写运行时源码，也不进行全局网络拦截。
// Build-only packaging. No runtime source rewriting or global network interception.
import { Buffer } from "node:buffer";
import { readFile, writeFile, mkdir, readdir, copyFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import path from "node:path";
import process from "node:process";
const here = new URL("./", import.meta.url);
const out = new URL("./dotnet-dist/", here);
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const publish = path.resolve(process.argv[2]);
const roots = [];
async function find(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    if (entry.isDirectory()) {
      if (entry.name === "_framework") roots.push(path.join(dir, entry.name));
      else await find(path.join(dir, entry.name));
    }
  }
}
await find(publish);
if (roots.length !== 1) throw new Error("Expected exactly one published framework directory");
await mkdir(out, { recursive: true });
const inventory = (await readdir(roots[0])).sort();
if (inventory.length > 1024) throw new Error("Runtime file-count cap exceeded");
console.log(JSON.stringify({ frameworkInventory: inventory }));
const assets = [];
for (const entry of (await readdir(roots[0], { withFileTypes: true })).sort((a, b) =>
  a.name.localeCompare(b.name),
)) {
  if (entry.isDirectory()) throw new Error("Nested framework resources require explicit review");
  if (/\.(br|gz|map)$/.test(entry.name)) continue;
  if (!/^[a-zA-Z0-9][a-zA-Z0-9_.+-]*\.(js|json|wasm|dat|dll|pdb|symbols)$/.test(entry.name))
    throw new Error(`Unreviewed runtime file: ${entry.name}`);
  await add(entry.name, await readFile(path.join(roots[0], entry.name)));
}
if (!assets.some((a) => a.path === "dotnet.js") || !assets.some((a) => a.path === "dotnet.boot.js"))
  throw new Error("Expected .NET10 JS bootstrap/config");
await add("worker.mjs", await readFile(new URL("dotnet-worker.mjs", here)));
async function add(name, bytes) {
  if (!bytes.length || bytes.length > 64 * 1024 * 1024) throw new Error("Asset size cap");
  await writeFile(new URL(name, out), bytes);
  assets.push({ path: name, bytes: bytes.length, sha256: hash(bytes) });
}
if (assets.reduce((n, a) => n + a.bytes, 0) > 256 * 1024 * 1024) throw new Error("Aggregate cap");
const manifest = JSON.stringify({ version: 1, assets });
if (Buffer.byteLength(manifest) > 131072) throw new Error("Manifest cap");
await writeFile(new URL("manifest.json", out), manifest);
let bootstrap = await readFile(new URL("dotnet-bootstrap.mjs", here), "utf8");
for (const [token, value] of [
  ["__MANIFEST_SHA256__", hash(manifest)],
  ['["__EXPECTED_ASSETS__"]', JSON.stringify(assets.map((a) => a.path))],
]) {
  if (bootstrap.split(token).length !== 2) throw new Error("Template drift");
  bootstrap = bootstrap.replace(token, value);
}
await writeFile(new URL("bootstrap.mjs", out), bootstrap);
await writeFile(
  new URL("index.html", out),
  `<!doctype html><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; connect-src 'self'; script-src 'self' blob: 'wasm-unsafe-eval'; worker-src blob:; img-src data:; object-src 'none'; base-uri 'none'; form-action 'none'"><link rel="icon" href="data:,"><title>Sealed .NET synthetic proof</title><script type="module" src="./bootstrap.mjs"></script>`,
);
await mkdir(new URL("licenses/", out), { recursive: true });
await copyFile(
  new URL("../../../dm/third_party/ryujinx/LICENSE.txt", here),
  new URL("licenses/RYUJINX_LICENSE.txt", out),
);
console.log(JSON.stringify({ assets, manifestSha256: hash(manifest) }));
