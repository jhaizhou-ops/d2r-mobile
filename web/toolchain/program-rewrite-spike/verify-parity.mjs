// 比较程序改写实现的输出一致性；依据 ./verify-adapter.mjs、playwright 的模块与调用约定。
// Compare output parity between program-rewrite implementations using the module and calling contracts in ./verify-adapter.mjs、playwright.
import { verifyGuestAdapter } from "./verify-adapter.mjs";
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { readFile, readdir, mkdir, writeFile } from "node:fs/promises";
import { resolve, extname, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";
const root = fileURLToPath(new URL("./dist/", import.meta.url));
const manifest = JSON.parse(
  await readFile(new URL("./fixtures/manifest.json", import.meta.url), "utf8"),
);
const server = createServer(async (req, res) => {
  try {
    const pathname = new URL(req.url, "http://localhost").pathname;
    if (pathname === "/") {
      res.setHeader("Content-Type", "text/html");
      res.end("<!doctype html><title>Synthetic program rewrite parity</title>");
      return;
    }
    const path = resolve(root, "." + decodeURIComponent(pathname));
    if (!path.startsWith(root.endsWith(sep) ? root : root + sep)) {
      res.writeHead(403).end();
      return;
    }
    res.setHeader(
      "Content-Type",
      {
        ".mjs": "text/javascript",
        ".js": "text/javascript",
        ".wasm": "application/wasm",
        ".json": "application/json",
      }[extname(path)] ?? "application/octet-stream",
    );
    res.end(await readFile(path));
  } catch {
    res.writeHead(404).end();
  }
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const browser = await chromium.launch({ headless: true });
try {
  const page = await browser.newPage();
  // 运行时完全同源；绝不上传任何夹具或输出。
  // Runtime is entirely same-origin. No fixture or output is ever uploaded.
  await page.route("**/*", (route) =>
    route
      .request()
      .url()
      .startsWith(origin + "/")
      ? route.continue()
      : route.abort(),
  );
  await page.goto(origin);
  const results = await page.evaluate(
    async (cases) => {
      const worker = new Worker("/worker.mjs", { type: "module" });
      const run = (data) =>
        new Promise((resolve, reject) => {
          const timer = setTimeout(() => reject(new Error("Worker timed out")), 120000);
          worker.onmessage = ({ data: result }) => {
            clearTimeout(timer);
            resolve(result);
          };
          worker.onerror = (error) => {
            clearTimeout(timer);
            reject(new Error(error.message));
          };
          worker.postMessage(data);
        });
      const results = [];
      try {
        for (const entry of cases) {
          const bytes = new Uint8Array(
            await (await fetch(`/fixtures/${entry.name}.nso`)).arrayBuffer(),
          );
          const result = await run({ id: entry.name, caseName: entry.name, bytes });
          if (result.files)
            result.files = Object.fromEntries(
              Object.entries(result.files).map(([name, bytes]) => [name, Array.from(bytes)]),
            );
          results.push(result);
        }
        const bytes = new Uint8Array(await (await fetch("/fixtures/gates.nso")).arrayBuffer());
        bytes[0x50] ^= 1;
        results.push(await run({ id: "unsupported", caseName: "gates", bytes }));
        const { REGISTERED_PROGRAM_RELEASE } = await import("/program-release-catalog.mjs");
        results.push(
          await run({
            id: "registered-rejected",
            releaseId: REGISTERED_PROGRAM_RELEASE.id,
            moduleName: "main",
            bytes,
          }),
        );
        return results;
      } finally {
        worker.terminate();
      }
    },
    manifest.cases.filter((entry) => !entry.multiOnly),
  );
  for (const entry of manifest.cases.filter((entry) => !entry.multiOnly)) {
    const result = results.find((r) => r.id === entry.name);
    assert.equal(result.ok, entry.success, `${entry.name}: ${result.error}`);
    if (!entry.success) {
      assert.match(
        result.error,
        /AssertionError/,
        `${entry.name} must reach the unchanged upstream assertion`,
      );
      assert.ok(
        result.error.includes(entry.errorIncludes),
        "Specific original preimage/build check must fire",
      );
      continue;
    }
    const oracle = new URL(`./oracle/${entry.name}/`, import.meta.url);
    assert.deepEqual(Object.keys(result.files).sort(), (await readdir(oracle)).sort());
    for (const [name, data] of Object.entries(result.files)) {
      assert.deepEqual(
        Buffer.from(data),
        await readFile(new URL(name, oracle)),
        `${entry.name}/${name}`,
      );
    }
    assert.ok(result.files["main.patched.bin"].length > 65536, "Actual trampoline bytes emitted");
    if (entry.name === "gates")
      assert.ok(
        Buffer.from(result.files["main.gates.tsv"]).toString().includes("svc"),
        "Actual SVC gate emitted",
      );
  }
  assert.match(results.find((r) => r.id === "unsupported").error, /Unsupported input/);
  assert.match(
    results.find((r) => r.id === "registered-rejected").error,
    /Unsupported registered program input/,
  );
  const adapterProof = await verifyGuestAdapter(page);
  await mkdir(new URL("./browser-results/", import.meta.url), { recursive: true });
  await writeFile(
    new URL("./browser-results/parity.json", import.meta.url),
    JSON.stringify(
      {
        syntheticOnly: true,
        adapterProof,
        cases: manifest.cases.map(({ name, success }) => ({
          name,
          expectedSuccess: success,
          matchedNative: true,
        })),
        unsupportedInputRejected: true,
      },
      null,
      2,
    ),
  );
  console.log(
    "Synthetic program rewrite parity: all image bytes, fixups, gates, sites and manifests match original Python; negative gates passed. Real build remains blocked.",
  );
} finally {
  await browser.close();
  await new Promise((resolve) => server.close(resolve));
}
