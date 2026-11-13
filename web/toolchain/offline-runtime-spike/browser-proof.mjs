// 驱动离线运行时的浏览器验证；依据 web/toolchain/offline-runtime-spike 的模块与调用约定。
// Drive browser verification of offline runtimes using the module and calling contracts in web/toolchain/offline-runtime-spike.
import { Buffer } from "node:buffer";
import assert from "node:assert/strict";
import http from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath, pathToFileURL } from "node:url";
import path from "node:path";
import process from "node:process";
const root = fileURLToPath(new URL("./dist/", import.meta.url));
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_ENTRY).href);
let sealed = false;
let mode = "valid";
const receipts = [];
const servers = [];
function makeServer(assets) {
  const server = http.createServer(async (req, res) => {
    const pathname = new URL(req.url, "http://localhost").pathname;
    receipts.push({ pathname, sealed, assets });
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Cache-Control", "no-store");
    if (pathname.startsWith("/probe/")) {
      res.setHeader("Content-Type", "text/javascript");
      res.end("export default true;");
      return;
    }
    if (pathname === "/control") {
      res.setHeader("Content-Type", "text/html");
      res.end("<!doctype html><title>Positive network control</title>");
      return;
    }
    if (!assets) {
      res.writeHead(404);
      res.end();
      return;
    }
    const name = pathname === "/" ? "index.html" : pathname.slice(1);
    if (!/^[a-zA-Z0-9.-]+$/.test(name)) {
      res.writeHead(404);
      res.end();
      return;
    }
    if (mode === "redirect" && name === "manifest.json") {
      res.writeHead(302, { Location: "/probe/redirect" });
      res.end();
      return;
    }
    if (mode === "missing" && name === "texture.wasm") {
      res.writeHead(404);
      res.end();
      return;
    }
    try {
      let bytes = await readFile(path.join(root, name));
      if (mode === "manifest" && name === "manifest.json") bytes = Buffer.from("{}");
      if (mode === "corrupt" && name === "worker.mjs") {
        bytes = Buffer.from(bytes);
        bytes[bytes.length - 1] ^= 1;
      }
      if (mode === "oversized" && name === "texture.wasm")
        bytes = Buffer.concat([bytes, Buffer.alloc(1)]);
      res.setHeader(
        "Content-Type",
        name.endsWith(".html")
          ? "text/html"
          : name.endsWith(".wasm")
            ? "application/wasm"
            : name.endsWith(".json")
              ? "application/json"
              : "text/javascript",
      );
      res.end(bytes);
    } catch {
      res.writeHead(404);
      res.end();
    }
  });
  servers.push(server);
  return server;
}
const originServer = makeServer(true);
const foreignServer = makeServer(false);
let browser;
try {
  for (const server of servers)
    await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const origin = `http://127.0.0.1:${originServer.address().port}`;
  const foreign = `http://127.0.0.1:${foreignServer.address().port}`;
  const probes = [`${origin}/probe/same`, `${foreign}/probe/cross`];
  browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ serviceWorkers: "block" });
  // 不安装路由或拦截；先验证两个哨兵端点均可访问。
  // No routing/interception is installed. Prove both sentinels are reachable first.
  const control = await context.newPage();
  await control.goto(`${origin}/control`);
  assert.deepEqual(
    await control.evaluate(
      async (urls) => Promise.all(urls.map(async (url) => (await fetch(url)).ok)),
      probes,
    ),
    [true, true],
  );
  await control.close();
  const results = [];
  for (const scenario of ["manifest", "corrupt", "oversized", "missing", "redirect", "valid"]) {
    mode = scenario;
    sealed = false;
    receipts.length = 0;
    const page = await context.newPage();
    let workers = 0;
    page.on("worker", () => workers++);
    await page.exposeFunction("offlineProofSealed", () => {
      sealed = true;
    });
    await page.goto(origin);
    await page.waitForFunction(() => typeof globalThis.runOfflineProof === "function");
    const outcome = await page.evaluate(async (urls) => {
      try {
        return { result: await globalThis.runOfflineProof(urls) };
      } catch (error) {
        return { error: error.message };
      }
    }, probes);
    if (scenario !== "valid") {
      assert.ok(outcome.error, `${scenario} must fail closed`);
      assert.equal(sealed, false, `${scenario} must fail before sealing`);
      assert.equal(workers, 0, `${scenario} must not execute any runtime Worker`);
      assert.equal(
        receipts.some((entry) => entry.pathname.startsWith("/probe/")),
        false,
      );
    } else {
      assert.equal(outcome.result?.ok, true, outcome.error);
      assert.equal(outcome.result.pixels, 64);
      assert.equal(outcome.result.instantiations, 1);
      assert.equal(outcome.result.blocked, 8);
      assert.equal(outcome.result.documentBlocked, 4);
      assert.equal(outcome.result.transferred, true);
      assert.equal(workers, 1);
      // 等待浏览器异步资源操作完成；服务器收到请求才是决定性证据。
      // Let async browser resource work settle; server receipt is the decisive evidence.
      await page.waitForTimeout(250);
      assert.deepEqual(
        receipts.filter((entry) => entry.sealed),
        [],
        "No HTTP receipt after seal",
      );
      assert.deepEqual(
        receipts.map((entry) => entry.pathname).sort(),
        [
          "/",
          "/bootstrap.mjs",
          "/manifest.json",
          "/texture.mjs",
          "/texture.wasm",
          "/browser-texture.mjs",
          "/worker.mjs",
        ].sort(),
        "Exact public preload closure",
      );
    }
    results.push({ scenario, passed: true, workers });
    await page.close();
  }
  console.log(JSON.stringify({ ok: true, browser: browser.version(), results }));
} finally {
  if (browser) await browser.close();
  for (const server of servers)
    if (server.listening) await new Promise((resolve) => server.close(resolve));
}
