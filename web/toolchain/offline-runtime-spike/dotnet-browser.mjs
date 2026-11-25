// 连接浏览器与隔离的 .NET 运行时工作线程；依据 ../shader-spike/wwwroot/synthetic-fixture.mjs 的模块与调用约定。
// Connect the browser to an isolated .NET runtime worker using the module and calling contracts in ../shader-spike/wwwroot/synthetic-fixture.mjs.
import { Buffer } from "node:buffer";
import assert from "node:assert/strict";
import http from "node:http";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { fileURLToPath, pathToFileURL } from "node:url";
import path from "node:path";
import process from "node:process";
import { code, job } from "../shader-spike/wwwroot/synthetic-fixture.mjs";
const root = fileURLToPath(new URL("./dotnet-dist/", import.meta.url));
const publicManifest = JSON.parse(await readFile(path.join(root, "manifest.json"), "utf8"));
const runtimeNames = publicManifest.assets.map((asset) => asset.path);
assert.ok(
  runtimeNames.every((name) => /^[a-zA-Z0-9][a-zA-Z0-9_.+-]*$/.test(name)),
  "Prepared runtime names must be flat safe filenames",
);
const publicNames = new Set(["index.html", "bootstrap.mjs", "manifest.json", ...runtimeNames]);
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
    if (!publicNames.has(name)) {
      res.writeHead(404);
      res.end();
      return;
    }
    if (mode === "redirect" && name === "manifest.json") {
      res.writeHead(302, { Location: "/probe/redirect" });
      res.end();
      return;
    }
    if (
      mode === "missing" &&
      name === runtimeNames.find((n) => /^dotnet\.native.*\.wasm$/.test(n))
    ) {
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
      if (
        mode === "oversized" &&
        name === runtimeNames.find((n) => /^dotnet\.native.*\.wasm$/.test(n))
      )
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
  browser = await chromium.launch({
    headless: true,
    ...(process.env.CHROMIUM_EXECUTABLE ? { executablePath: process.env.CHROMIUM_EXECUTABLE } : {}),
  });
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
    let initializing = true;
    const diagnostics = [];
    const record = (text) => {
      if (initializing && diagnostics.length < 20) diagnostics.push(String(text).slice(0, 1000));
    };
    page.on("console", (message) => {
      if (message.type() === "error" || message.type() === "warning") record(message.text());
    });
    page.on("pageerror", (error) => record(error.message));
    page.on("worker", () => workers++);
    await page.exposeFunction("offlineProofSealed", () => {
      sealed = true;
    });
    await page.goto(origin);
    await page.waitForFunction(() => typeof globalThis.startOfflineDotnet === "function");
    const outcome = await page.evaluate(async (urls) => {
      try {
        globalThis.dotnetProof = await globalThis.startOfflineDotnet(urls);
        return { result: globalThis.dotnetProof.stats };
      } catch (error) {
        return { error: error.message };
      }
    }, probes);
    initializing = false;
    if (scenario === "valid" && outcome.error)
      console.log(
        JSON.stringify({
          initializationFailure: outcome.error,
          diagnostics,
          postSealReceipts: receipts.filter((entry) => entry.sealed).slice(0, 30),
        }),
      );
    if (scenario !== "valid") {
      assert.ok(outcome.error, `${scenario} must fail closed`);
      assert.equal(sealed, false, `${scenario} must fail before sealing`);
      assert.equal(workers, 0, `${scenario} must not execute any runtime Worker`);
      assert.equal(
        receipts.some((entry) => entry.pathname.startsWith("/probe/")),
        false,
      );
    } else {
      assert.equal(outcome.result?.initialized, true, outcome.error);
      assert.ok(outcome.result.declared > 0);
      assert.ok(outcome.result.reads > 0);
      assert.match(outcome.result.mainAssembly, /^Dmsh\.Browser\.Spike(?:\.dll)?$/);
      assert.equal(outcome.result.blocked, 8);
      assert.equal(outcome.result.documentBlocked, 4);
      assert.equal(outcome.result.transferred, true);
      assert.equal(workers, 1);
      const translated = await page.evaluate(
        async ({ job, bytes }) => globalThis.dotnetProof.run(job, new Uint8Array(bytes)),
        { job, bytes: [...code] },
      );
      const destination = path.resolve(process.argv[2]);
      await mkdir(destination, { recursive: true });
      await writeFile(path.join(destination, "browser-result.json"), JSON.stringify(translated));
      // 独立的原生对比在后续 CI 步骤中执行。
      // Independent native comparison executes in the following CI step.
      assert.equal(
        await page.evaluate(async () => {
          try {
            await globalThis.dotnetProof.run({ stage: "unsupported" }, new Uint8Array(16));
            return false;
          } catch {
            return true;
          }
        }),
        true,
      );
      const repeated = await page.evaluate(
        async ({ job, bytes }) => globalThis.dotnetProof.run(job, new Uint8Array(bytes)),
        { job, bytes: [...code] },
      );
      assert.deepEqual(repeated, translated, "Valid translation recovers after rejected input");
      await page.waitForTimeout(250);
      assert.deepEqual(
        receipts.filter((entry) => entry.sealed),
        [],
        "No HTTP receipt after seal, including package loading and conversions",
      );
      const manifest = JSON.parse(await readFile(path.join(root, "manifest.json"), "utf8"));
      assert.deepEqual(
        receipts.map((entry) => entry.pathname).sort(),
        [
          "/",
          "/bootstrap.mjs",
          "/manifest.json",
          ...manifest.assets.map((asset) => `/${asset.path}`),
        ].sort(),
        "Exact public preload closure",
      );
      await page.evaluate(() => globalThis.dotnetProof.close());
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
