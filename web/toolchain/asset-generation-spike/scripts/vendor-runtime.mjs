// 仅用于构建/CI；浏览器绝不访问 CDN。npm 锁文件固定运行时字节；上游 pyodide-lock.json 使用 SHA-256 固定依赖 wheel。
// Build/CI only. Browser never contacts the CDN. npm lock pins the runtime bytes;
// upstream pyodide-lock.json pins dependency wheels with SHA-256.
import { mkdir, readFile, copyFile, writeFile, readdir } from "node:fs/promises";
import { createHash } from "node:crypto";
import { resolve } from "node:path";
const version = "0.27.7";
const source = resolve("node_modules/pyodide");
const dest = resolve("public/vendor/pyodide");
await mkdir(dest, { recursive: true });
const pkg = JSON.parse(await readFile(`${source}/package.json`));
if (pkg.version !== version) throw new Error("Pyodide version drift");
const lock = JSON.parse(await readFile(`${source}/pyodide-lock.json`));
const hashes = {};
for (const file of [
  "pyodide.mjs",
  "pyodide.asm.js",
  "pyodide.asm.wasm",
  "python_stdlib.zip",
  "pyodide-lock.json",
]) {
  await copyFile(`${source}/${file}`, `${dest}/${file}`);
  hashes[file] = createHash("sha256")
    .update(await readFile(`${dest}/${file}`))
    .digest("hex");
}
// 保留运行时包分发的许可证和声明文件。wheel 归档保留自身的 dist-info 许可证；下方也会保存包元数据。
// Preserve the runtime package's distributed license/notice files. Wheel archives
// retain their own dist-info licenses; package metadata is saved below as well.
for (const file of await readdir(source)) {
  if (/^(license|copying|notice)/i.test(file))
    await copyFile(`${source}/${file}`, `${dest}/${file}`);
}
const seen = new Set();
async function vendor(name) {
  if (seen.has(name)) return;
  seen.add(name);
  const info = lock.packages[name];
  if (!info || !/^[a-zA-Z0-9_.+-]+$/.test(info.file_name))
    throw new Error(`Invalid package ${name}`);
  for (const dep of info.depends ?? []) await vendor(dep);
  let bytes;
  try {
    const cached = await readFile(`${dest}/${info.file_name}`);
    if (createHash("sha256").update(cached).digest("hex") === info.sha256) bytes = cached;
  } catch (error) {
    if (error.code !== "ENOENT") throw error;
  }
  if (!bytes) {
    const response = await fetch(
      `https://cdn.jsdelivr.net/pyodide/v${version}/full/${info.file_name}`,
    );
    if (!response.ok) throw new Error(`Package fetch failed: ${name} ${response.status}`);
    bytes = Buffer.from(await response.arrayBuffer());
  }
  const hash = createHash("sha256").update(bytes).digest("hex");
  if (hash !== info.sha256) throw new Error(`Integrity mismatch: ${name}`);
  await writeFile(`${dest}/${info.file_name}`, bytes);
  hashes[info.file_name] = hash;
}
await vendor("pillow");
await writeFile(
  `${dest}/vendor-manifest.json`,
  JSON.stringify(
    {
      version,
      packages: Object.fromEntries([...seen].map((name) => [name, lock.packages[name]])),
      sha256: hashes,
    },
    null,
    2,
  ),
);
console.log(`Self-hosted Pyodide ${version}; packages: ${[...seen].join(", ")}`);
