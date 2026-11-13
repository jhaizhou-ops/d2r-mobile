// 使用 Bun 构建静态前端并校验发布输出；依据 bun-plugin-tailwind、./frontend-notices.mjs 的模块与调用约定。
// Build the static frontend with Bun and validate release output using the module and calling contracts in bun-plugin-tailwind、./frontend-notices.mjs.
import { rm, mkdir, readFile, readdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { resolve, dirname } from "node:path";
import tailwind from "bun-plugin-tailwind";
import { validateStaticBuild, writeFrontendNotices } from "./frontend-notices.mjs";

export const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
export const outputDirectory = resolve(root, "dist");

export async function build() {
  // 此目录仅包含生成的前端资源；绝不将源码、密钥文件、
  // 工具链夹具或环境变量复制到部署产物中。
  // This directory contains only generated frontend assets. Never copy source,
  // key files, toolchain fixtures, or environment variables into the deployment.
  await rm(outputDirectory, { recursive: true, force: true });
  await mkdir(outputDirectory, { recursive: true });
  const result = await Bun.build({
    entrypoints: [resolve(root, "index.html")],
    outdir: outputDirectory,
    target: "browser",
    format: "esm",
    minify: true,
    metafile: true,
    sourcemap: "none",
    env: "disable",
    define: { "process.env.NODE_ENV": JSON.stringify("production") },
    plugins: [tailwind],
  });
  if (!result.success) throw new AggregateError(result.logs, "Frontend bundling failed");
  await writeFrontendNotices(outputDirectory, result.metafile, process.cwd());
  const htmlPath = resolve(outputDirectory, "index.html");
  const html = await readFile(htmlPath, "utf8");
  const files = await readdir(outputDirectory);
  if (!files.some((name) => name.endsWith(".js")) || !files.some((name) => name.endsWith(".css"))) {
    throw new Error("Missing browser script or Tailwind stylesheet");
  }
  const assets = new Map(
    await Promise.all(
      files
        .filter((name) => name !== "index.html")
        .map(async (name) => [name, await readFile(resolve(outputDirectory, name), "utf8")]),
    ),
  );
  validateStaticBuild(html, assets);
  console.log(`Bun bundled ${files.length} static files into dist/`);
}

if (import.meta.main) await build();
