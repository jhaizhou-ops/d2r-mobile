// 根据实际打包模块保留第三方声明；依据 Bun 构建元数据及原始许可证文件。
// Preserve third-party notices from actual bundled modules using Bun metadata and original licenses.
import { readFile, writeFile, realpath, readdir } from "node:fs/promises";
import { resolve, dirname, relative, sep } from "node:path";
import { fileURLToPath } from "node:url";

export const FRONTEND_NOTICES_FILE = "third-party-notices.txt";
const root = fileURLToPath(new URL("../", import.meta.url));

// 固定包还内嵌 MD5 C 和 TypeScript helper；保留其原始声明。
// The pinned package embeds MD5 C and TypeScript helpers; retain their original notices.
export async function readHashWasmEmbeddedNotices(
  directory = resolve(root, "node_modules/hash-wasm"),
) {
  const md5 = (await readFile(resolve(directory, "src/md5.c"), "utf8")).match(
    /^\/\*[\s\S]*?\*\//,
  )?.[0];
  const helper = (await readFile(resolve(directory, "dist/index.esm.js"), "utf8")).match(
    /\/\*\*+\r?\nCopyright \(c\) Microsoft Corporation\.[\s\S]*?\*\//,
  )?.[0];
  if (!md5?.includes("Alexander Peslyak") || !helper)
    throw new Error("hash-wasm embedded notices changed");
  return `${md5}\n\n${helper}`;
}

// 只读取有输出贡献的包，不执行包代码，也不遍历开发依赖。
// Read only packages contributing output bytes; never execute packages or traverse dev dependencies.
export async function writeFrontendNotices(outputDirectory, metafile, inputDirectory = root) {
  if (!metafile || !metafile.outputs || typeof metafile.outputs !== "object")
    throw new Error("Frontend notices require Bun output metadata");
  const modules = new Set();
  for (const output of Object.values(metafile.outputs)) {
    if (!output.inputs || typeof output.inputs !== "object")
      throw new Error("Frontend output lacks input contribution metadata");
    for (const [path, contribution] of Object.entries(output.inputs)) {
      if (!Number.isFinite(contribution.bytesInOutput) || contribution.bytesInOutput < 0)
        throw new Error("Invalid frontend input contribution");
      if (contribution.bytesInOutput > 0) modules.add(path);
    }
  }
  if (!modules.size) throw new Error("Frontend metadata has no contributing inputs");
  const installed = await realpath(resolve(root, "node_modules"));
  const packages = new Map();
  for (const path of modules) {
    if (!path.replaceAll("\\", "/").includes("node_modules/")) continue;
    const source = await realpath(resolve(inputDirectory, path));
    const inside = relative(installed, source);
    if (inside === ".." || inside.startsWith(`..${sep}`) || inside.startsWith(sep))
      throw new Error("Frontend package input is outside installed dependencies");
    let directory = dirname(source);
    while (directory !== installed) {
      try {
        const metadata = JSON.parse(await readFile(resolve(directory, "package.json"), "utf8"));
        if (typeof metadata.name === "string" && typeof metadata.version === "string") {
          packages.set(directory, metadata);
          break;
        }
      } catch (error) {
        if (error.code !== "ENOENT") throw error;
      }
      directory = dirname(directory);
    }
    if (directory === installed) throw new Error("Frontend package metadata is missing");
  }
  // Tailwind 转换把 CSS 来源折叠为本地入口；其显式样式导入仍随页面分发。
  // Tailwind collapses CSS origins into the local entry; its explicit stylesheet import still ships.
  for (const path of modules) {
    if (resolve(inputDirectory, path) !== resolve(root, "src/index.css")) continue;
    const css = await readFile(resolve(root, "src/index.css"), "utf8");
    if (!/@import\s+["']tailwindcss["']/.test(css))
      throw new Error("Frontend stylesheet provenance changed");
    const directory = resolve(installed, "tailwindcss");
    packages.set(directory, JSON.parse(await readFile(resolve(directory, "package.json"), "utf8")));
  }
  if (!packages.size) throw new Error("Frontend metadata has no runtime packages");
  const sections = ["Frontend third-party notices\n"];
  for (const [directory, metadata] of [...packages].sort(([a], [b]) => a.localeCompare(b))) {
    const files = (await readdir(directory))
      .filter((name) => /^(?:licen[sc]e|notice)(?:[.-].*)?$/i.test(name))
      .sort();
    if (!files.some((name) => /^licen[sc]e(?:[.-].*)?$/i.test(name))) {
      // 部分 Radix 辅助包未携带根许可证；仅同一仓库、明确 MIT 的包共用原文。
      // Some Radix helpers omit the root license; share its original only for the same MIT repository.
      const upstream = "git+https://github.com/radix-ui/primitives.git";
      if (
        !metadata.name.startsWith("@radix-ui/") ||
        metadata.license !== "MIT" ||
        metadata.repository?.url !== upstream
      )
        throw new Error(`Missing frontend license: ${metadata.name}@${metadata.version}`);
      const shared = JSON.parse(
        await readFile(resolve(installed, "radix-ui/package.json"), "utf8"),
      );
      if (shared.license !== "MIT" || shared.repository?.url !== upstream)
        throw new Error("Radix shared license provenance changed");
      sections.push(
        `${metadata.name}@${metadata.version} (shared upstream ${upstream}; radix-ui@${shared.version}/LICENSE)\n\n${await readFile(resolve(installed, "radix-ui/LICENSE"), "utf8")}`,
      );
    }
    if (metadata.name === "hash-wasm")
      sections.push(
        `${metadata.name}@${metadata.version} (embedded MD5 and TypeScript helper notices)\n\n${await readHashWasmEmbeddedNotices(directory)}`,
      );
    for (const file of files)
      sections.push(
        `${metadata.name}@${metadata.version} (${file})\n\n${await readFile(resolve(directory, file), "utf8")}`,
      );
  }
  // 本地复制的组件仍随产物分发；逐字保留许可证。
  // Locally copied components still ship; preserve their license verbatim.
  sections.push(`shadcn/ui\n\n${await readFile(resolve(root, "SHADCN_LICENSE.md"), "utf8")}`);
  await writeFile(resolve(outputDirectory, FRONTEND_NOTICES_FILE), sections.join("\n\n"));
}

// 拒绝静态发布输出中的不合规路径与资产；依据 web/scripts 的模块与调用约定。
// Reject disallowed paths and assets in static build output using the module and calling contracts in web/scripts.
export function validateStaticBuild(html, assets) {
  const references = [...html.matchAll(/\b(?:src|href)="([^"]+)"/g)].map((match) => match[1]);
  if (
    !references.some((name) => name.endsWith(".js")) ||
    !references.some((name) => name.endsWith(".css"))
  ) {
    throw new Error("Missing bundled script or stylesheet");
  }
  for (const reference of references) {
    if (!/^\.\/[A-Za-z0-9_-]+\.(?:js|css)$/.test(reference) || !assets.has(reference.slice(2))) {
      throw new Error("Asset is missing or not a local static bundle");
    }
  }
  for (const [name, content] of assets) {
    if (name.endsWith(".css") && /@(?:apply|theme|source)\b|@import\s/.test(content)) {
      throw new Error("Tailwind directives were not compiled");
    }
    if (
      name.endsWith(".js") &&
      /\bBun\s*\.|(?:from\s*|import\s*\()["'](?:bun|node):/.test(content)
    ) {
      throw new Error("Server runtime API leaked into browser bundle");
    }
  }
}
