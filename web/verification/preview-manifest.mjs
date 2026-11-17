// 生成预览构建的文件与步骤清单；依据 ../runtime-build/inventory.mjs、../scripts/frontend-notices.mjs 的模块与调用约定。
// Generate file and step manifests for preview builds using the module and calling contracts in ../runtime-build/inventory.mjs、../scripts/frontend-notices.mjs.
import process from "node:process";
import { readdir, readFile, writeFile, lstat } from "node:fs/promises";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { verifyRuntimeInventory } from "../runtime-build/inventory.mjs";
import { validateStaticBuild, FRONTEND_NOTICES_FILE } from "../scripts/frontend-notices.mjs";

export async function createPreviewManifest(directory, commit = null, runtimeDigest = null) {
  const files = [];
  const assets = new Map();
  let html;
  let runtime = null;
  for (const name of await readdir(directory)) {
    const url = new URL(name, directory);
    const info = await lstat(url);
    if (name === "runtime") {
      runtime = await verifyRuntimeInventory(new URL("runtime/", directory), runtimeDigest, commit);
      continue;
    }
    if (!info.isFile() || info.isSymbolicLink())
      throw new Error("Preview must contain regular files only");
    if (name === "build-manifest.json") continue;
    if (
      name !== "index.html" &&
      name !== FRONTEND_NOTICES_FILE &&
      !/^[A-Za-z0-9_-]+\.(js|css)$/.test(name)
    ) {
      throw new Error("Unexpected file outside the static bundle allowlist");
    }
    if (info.size > 16 * 1024 * 1024) throw new Error("Unexpected oversized preview file");
    const bytes = await readFile(url);
    files.push({
      path: name,
      bytes: bytes.length,
      sha256: createHash("sha256").update(bytes).digest("hex"),
    });
    if (name === "index.html") html = bytes.toString("utf8");
    else if (name !== FRONTEND_NOTICES_FILE) assets.set(name, bytes.toString("utf8"));
  }
  if (!html || files.length > 200) throw new Error("Invalid preview inventory");
  validateStaticBuild(html, assets);
  const references = new Set(
    [...html.matchAll(/\b(?:src|href)="\.\/([^"]+)"/g)].map((match) => match[1]),
  );
  if (references.size !== assets.size || [...assets.keys()].some((name) => !references.has(name))) {
    throw new Error("Unreferenced file in preview bundle");
  }
  const manifest = {
    schema: 1,
    commit: /^[0-9a-f]{40}$/.test(commit || "") ? commit : null,
    scope: "Static preview only; not a working game IPA",
    runtimeManifestSha256: runtime?.sha256 ?? null,
    files: [
      ...files,
      ...(runtime?.files ?? []).map((file) => ({ ...file, path: `runtime/${file.path}` })),
    ].sort((a, b) => a.path.localeCompare(b.path)),
  };
  await writeFile(
    new URL("build-manifest.json", directory),
    JSON.stringify(manifest, null, 2) + "\n",
  );
  return manifest;
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await createPreviewManifest(
    new URL("../dist/", import.meta.url),
    process.env.GITHUB_SHA,
    process.env.EXPECTED_RUNTIME_MANIFEST_SHA256,
  );
}
