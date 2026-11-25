// 解析静态资产路径并提供本地预览服务；依据 web/scripts 的模块与调用约定。
// Resolve static asset paths and serve the local preview using the module and calling contracts in web/scripts.
import { resolve, sep, extname } from "node:path";
import { stat } from "node:fs/promises";
import { watch } from "node:fs";
import { fileURLToPath } from "node:url";

const outputDirectory = fileURLToPath(new URL("../dist/", import.meta.url));
const contentTypes = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".txt": "text/plain; charset=utf-8",
};

export function assetPath(pathname) {
  let decoded;
  try {
    decoded = decodeURIComponent(pathname);
  } catch {
    return null;
  }
  if (
    decoded.includes("\\") ||
    decoded.includes("\0") ||
    decoded.split("/").some((part) => part.startsWith("."))
  )
    return null;
  const relative = decoded === "/" ? "index.html" : decoded.replace(/^\/+/, "");
  const path = resolve(outputDirectory, relative);
  return path.startsWith(resolve(outputDirectory) + sep) ? path : null;
}

export function serve() {
  return Bun.serve({
    hostname: "127.0.0.1",
    port: 4173,
    async fetch(request) {
      if (request.method !== "GET" && request.method !== "HEAD")
        return new Response("Method not allowed", { status: 405 });
      const path = assetPath(new URL(request.url).pathname);
      if (!path) return new Response("Not found", { status: 404 });
      try {
        if (!(await stat(path)).isFile()) return new Response("Not found", { status: 404 });
        const headers = {
          "Content-Type": contentTypes[extname(path)] ?? "application/octet-stream",
          "Cache-Control": "no-store",
          "X-Content-Type-Options": "nosniff",
        };
        return new Response(request.method === "HEAD" ? null : Bun.file(path), { headers });
      } catch {
        return new Response("Not found", { status: 404 });
      }
    },
  });
}

// 开发模式仅在明确启用监视时加载构建工具。
// Load build tools only when watch mode is explicitly selected.
async function watchPreview() {
  const { build, root } = await import("./build.mjs");
  await build();
  const server = serve();
  console.log(`Development preview: ${server.url} (refresh after rebuild)`);
  let running = false;
  let pending = false;
  let timer;
  async function rebuild() {
    pending = true;
    if (running) return;
    running = true;
    while (pending) {
      pending = false;
      try {
        await build();
      } catch (error) {
        console.error(error);
      }
    }
    running = false;
  }
  function scheduleRebuild() {
    clearTimeout(timer);
    timer = setTimeout(rebuild, 100);
  }
  // 不递归监视下载的工具链、node_modules 或构建输出。
  // Do not recursively watch downloaded toolchains, node_modules or build output.
  const watchers = [
    watch(`${root}/src`, { recursive: true }, scheduleRebuild),
    watch(root, (_event, filename) => {
      if (/^(?:index\.html|[^/]+\.mjs)$/.test(String(filename ?? ""))) scheduleRebuild();
    }),
  ];
  for (const signal of ["SIGINT", "SIGTERM"]) {
    process.once(signal, () => {
      clearTimeout(timer);
      for (const watcher of watchers) watcher.close();
      server.stop();
    });
  }
}

if (import.meta.main) {
  if (process.argv.includes("--watch")) await watchPreview();
  else {
    const server = serve();
    console.log(`Static preview: ${server.url}`);
  }
}
