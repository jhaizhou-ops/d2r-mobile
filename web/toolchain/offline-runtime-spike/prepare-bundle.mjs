// 仅用于 CI/构建；运行时字节由现有固定版本的纹理构建生成。
// CI/build only. Runtime bytes are produced by the existing pinned texture build.
import { readFile, writeFile, mkdir, copyFile, cp } from "node:fs/promises";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
const here = new URL("./", import.meta.url);
const out = new URL("./dist/", here);
const texture = new URL("../texture-spike/", here);
await mkdir(out, { recursive: true });
const entries = [
  ["texture.mjs", new URL("dist/texture.mjs", texture)],
  ["texture.wasm", new URL("dist/texture.wasm", texture)],
  ["browser-texture.mjs", new URL("browser-texture.mjs", texture)],
  ["worker.mjs", new URL("worker.mjs", here)],
];
const assets = [];
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
for (const [path, source] of entries) {
  const bytes = await readFile(source);
  if (!bytes.length || bytes.length > 16 * 1024 * 1024)
    throw new Error("Asset size outside proof cap");
  await copyFile(source, new URL(path, out));
  assets.push({ path, bytes: bytes.length, sha256: hash(bytes) });
}
if (assets.reduce((sum, asset) => sum + asset.bytes, 0) > 32 * 1024 * 1024)
  throw new Error("Proof bundle exceeds total cap");
const manifest = JSON.stringify({ version: 1, assets });
await writeFile(new URL("manifest.json", out), manifest);
const bootstrap = await readFile(new URL("bootstrap.mjs", here), "utf8");
if (bootstrap.split("__MANIFEST_SHA256__").length !== 2)
  throw new Error("Digest placeholder mismatch");
await writeFile(
  new URL("bootstrap.mjs", out),
  bootstrap.replace("__MANIFEST_SHA256__", hash(manifest)),
);
await writeFile(
  new URL("index.html", out),
  `<!doctype html>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; connect-src 'self'; script-src 'self' blob: 'wasm-unsafe-eval'; worker-src blob:; img-src data:; object-src 'none'; base-uri 'none'; form-action 'none'">
<link rel="icon" href="data:,">
<title>Offline texture isolation proof</title>
<script type="module" src="./bootstrap.mjs"></script>
<p>Synthetic developer proof only. No local file input.</p>\n`,
);
await cp(new URL("dist/licenses/", texture), new URL("licenses/", out), { recursive: true });
console.log(
  JSON.stringify({ directory: fileURLToPath(out), assets, manifestSha256: hash(manifest) }),
);
