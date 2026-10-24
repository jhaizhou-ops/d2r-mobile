// 仅用于开发者 CI，不导入原生参考结果，也不以 Node 运行时替代浏览器。
// Developer CI only. Never imports the native oracle or substitutes a Node runtime.
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { createReadStream } from "node:fs";
import { readdir, realpath, stat, mkdir, writeFile } from "node:fs/promises";
import { resolve, join, extname, sep } from "node:path";
import { chromium } from "playwright";

const [publishDirectory, outputDirectory] = process.argv.slice(2);
assert.ok(
  publishDirectory && outputDirectory,
  "Usage: capture-browser.mjs <publish-directory> <output-directory>",
);
const output = resolve(outputDirectory);
await mkdir(output, { recursive: true });
const candidates = [];
const inventory = [];
async function discover(directory, depth = 0) {
  if (depth > 6) return;
  const entries = await readdir(directory, { withFileTypes: true });
  if (inventory.length < 32)
    inventory.push({ directory, entries: entries.slice(0, 64).map((entry) => entry.name) });
  if (entries.some((entry) => entry.name === "index.html" && entry.isFile())) {
    try {
      assert.ok((await stat(join(directory, "_framework/dotnet.js"))).isFile());
      assert.ok((await stat(join(directory, "worker.mjs"))).isFile());
      assert.ok((await stat(join(directory, "synthetic-fixture.mjs"))).isFile());
      candidates.push(await realpath(directory));
    } catch {
      /* 这不是可部署的完整资源包。 */
      /* 不是完整的可部署包。 */
      /* Not a complete deployable bundle. */
    }
  }
  for (const entry of entries) {
    if (entry.isDirectory() && entry.name !== "_framework")
      await discover(join(directory, entry.name), depth + 1);
  }
}
await discover(resolve(publishDirectory));
await writeFile(
  join(output, "bundle-diagnostics.json"),
  JSON.stringify({ candidates, inventory }, null, 2),
);
console.log("Published bundle candidates:", JSON.stringify(candidates));
assert.equal(
  candidates.length,
  1,
  `Expected exactly one complete published bundle, found ${candidates.length}`,
);
const root = candidates[0];
const mime = {
  ".html": "text/html",
  ".js": "text/javascript",
  ".mjs": "text/javascript",
  ".json": "application/json",
  ".wasm": "application/wasm",
};
const server = createServer(async (request, response) => {
  try {
    if (!["GET", "HEAD"].includes(request.method)) {
      response.writeHead(405).end();
      return;
    }
    const pathname = decodeURIComponent(new URL(request.url, "http://localhost").pathname);
    const path = await realpath(join(root, pathname === "/" ? "index.html" : pathname));
    if (!path.startsWith(root + sep) || !(await stat(path)).isFile()) {
      response.writeHead(404).end();
      return;
    }
    response.writeHead(200, {
      "Content-Type": mime[extname(path)] ?? "application/octet-stream",
      "Cache-Control": "no-store",
    });
    if (request.method === "HEAD") response.end();
    else
      createReadStream(path)
        .on("error", () => response.destroy())
        .pipe(response);
  } catch {
    response.writeHead(404).end();
  }
});
await new Promise((resolve, reject) => {
  server.once("error", reject);
  server.listen(0, "127.0.0.1", resolve);
});
const origin = `http://127.0.0.1:${server.address().port}`;
const diagnostics = {
  browser: null,
  bundle: root,
  workerSeen: false,
  events: [],
  status: null,
  elapsedMs: null,
  resultBytes: null,
  success: false,
};
function record(type, message) {
  if (diagnostics.events.length < 100)
    diagnostics.events.push({ type, message: String(message).slice(0, 1024) });
}
let browser;
let page;
const started = Date.now();
try {
  browser = await chromium.launch({ headless: true, timeout: 30000 });
  diagnostics.browser = browser.version();
  const context = await browser.newContext({ acceptDownloads: true, serviceWorkers: "block" });
  await context.route("**/*", (route) =>
    new URL(route.request().url()).origin === origin ? route.continue() : route.abort(),
  );
  page = await context.newPage();
  page.on("worker", (worker) => {
    if (worker.url() === `${origin}/worker.mjs`) diagnostics.workerSeen = true;
  });
  page.on("pageerror", (error) => record("pageerror", error.message));
  page.on("console", (message) => {
    if (message.type() === "error") record("console", message.text());
  });
  page.on("requestfailed", (request) =>
    record("requestfailed", `${request.url()} ${request.failure()?.errorText}`),
  );
  page.on("response", (response) => {
    if (response.status() >= 400) record("http", `${response.status()} ${response.url()}`);
  });
  const navigation = await page.goto(origin, { waitUntil: "load", timeout: 30000 });
  assert.equal(navigation.status(), 200);
  const [download] = await Promise.all([
    page.waitForEvent("download", { timeout: 75000 }),
    page.getByRole("button", { name: "Run synthetic EXIT fixture" }).click(),
  ]);
  assert.equal(download.suggestedFilename(), "browser-result.json");
  assert.equal(await download.failure(), null);
  const stream = await download.createReadStream();
  const chunks = [];
  let size = 0;
  for await (const chunk of stream) {
    size += chunk.length;
    if (size > 256 * 1024) {
      stream.destroy();
      throw new Error("Browser result exceeds 256 KiB synthetic fixture cap");
    }
    chunks.push(chunk);
  }
  const bytes = Buffer.concat(chunks);
  const result = JSON.parse(bytes.toString("utf8"));
  assert.deepEqual(Object.keys(result).sort(), ["resources", "spirvBase64"]);
  assert.equal(typeof result.spirvBase64, "string");
  assert.equal(result.resources?.key, "synthetic-exit");
  assert.ok(diagnostics.workerSeen, "Expected real published module worker");
  await writeFile(join(output, "browser-result.json"), bytes);
  diagnostics.resultBytes = size;
  diagnostics.success = true;
} catch (error) {
  record("failure", error.stack ?? error);
  throw error;
} finally {
  diagnostics.elapsedMs = Date.now() - started;
  if (page && !page.isClosed())
    diagnostics.status = (
      await page
        .locator("#status")
        .textContent({ timeout: 1000 })
        .catch(() => null)
    )?.slice(0, 2048);
  await writeFile(join(output, "browser-diagnostics.json"), JSON.stringify(diagnostics, null, 2));
  if (browser) await browser.close();
  await new Promise((resolve) => server.close(resolve));
}
console.log(
  `Actual Chromium worker translation captured (${diagnostics.resultBytes} bytes); parity and validation still required.`,
);
