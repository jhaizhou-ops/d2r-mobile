// 依据固定入口打包封闭公开工作线程；运行时依赖只由已验证字节提供。
// Bundle closed public workers from fixed entries; runtime dependencies arrive only as verified bytes.
import { mkdir, rm } from "node:fs/promises";
import { fileURLToPath } from "node:url";
const outdir = fileURLToPath(new URL("../runtime-build/.build/sealed-workers/", import.meta.url));
await rm(outdir, { recursive: true, force: true });
await mkdir(outdir, { recursive: true });
for (const name of [
  "nsp-worker",
  "cross-worker",
  "program-worker",
  "astc-producer-worker",
  "signing-worker",
  "air-compiler-worker",
]) {
  const source = ["astc-producer-worker", "air-compiler-worker"].includes(name)
    ? `../sealed-runtime/${name}.mjs`
    : `${name}.mjs`;
  const result = await globalThis.Bun.build({
    entrypoints: [fileURLToPath(new URL(source, import.meta.url))],
    // ASTC 工作者不能意外携带认证门面或 UI 图；在加载阶段拒绝，而非用 PURE 注解隐藏副作用。
    // Reject auth facade/UI graph loads in the ASTC worker rather than hiding side effects with PURE annotations.
    plugins:
      name === "astc-producer-worker"
        ? [
            {
              name: "astc-core-boundary",
              setup(build) {
                build.onLoad(
                  {
                    filter:
                      /\/(?:astc-pack-jobs|extraction-job-adapter|extraction-worker-client|extraction-errors|i18n|i18n-catalog|casc-overlay-writer)\.mjs$/,
                  },
                  () => {
                    throw new Error(
                      "ASTC worker must not load parent authentication or UI modules",
                    );
                  },
                );
              },
            },
          ]
        : [],
    outdir,
    naming: `${name}.mjs`,
    target: "browser",
    format: "esm",
    minify: true,
    splitting: false,
    sourcemap: "none",
    env: "disable",
    define: { process: "undefined" },
    // Emscripten 生成的休眠 Node 分支不能在浏览器工作线程执行；不打包 Node 垫片。
    // Dormant generated Emscripten Node branches cannot run in browser workers; no Node shim is bundled.
    external: ["node:*", "module", "fs", "path", "url", "crypto"],
  });
  if (!result.success) throw new AggregateError(result.logs, `Failed to bundle ${name}`);
  if (result.outputs.length !== 1) throw new Error(`Unexpected split worker output: ${name}`);
  const code = await result.outputs[0].text();
  // 构建必须闭合所有字面量导入；剩余动态工厂导入只使用初始化时的已验证内存 Blob。
  // Close every literal import at build time; remaining dynamic factory imports use verified initialization Blobs only.
  const imports = new globalThis.Bun.Transpiler({ loader: "js" }).scanImports(code);
  if (imports.length)
    throw new Error(`Unclosed worker import graph: ${name}: ${JSON.stringify(imports)}`);
}
