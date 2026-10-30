// 使用真实 Chromium Worker 的测试工具，绝不将原生输出导入浏览器。
// Real Chromium worker harness; never imports native output into the browser.
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { resolve, extname, sep } from "node:path";
import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";
const require = createRequire(new URL("../shader-spike/package.json", import.meta.url));
const { chromium } = require("playwright");
const root = resolve("wwwroot"),
  output = resolve(process.argv[2] ?? "results");
await mkdir(output, { recursive: true });
const mime = {
  ".mjs": "text/javascript",
  ".wasm": "application/wasm",
  ".html": "text/html",
  ".spv": "application/octet-stream",
};
const server = createServer(async (request, response) => {
  try {
    const path = resolve(root, "." + new URL(request.url, "http://localhost").pathname);
    if (!path.startsWith(root + sep)) throw new Error("Outside bundle");
    response.setHeader("Content-Type", mime[extname(path)] ?? "application/octet-stream");
    response.end(await readFile(path));
  } catch {
    response.writeHead(404);
    response.end();
  }
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
let browser;
try {
  browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${server.address().port}/index.html`);
  const outputs = await page.evaluate(async () => {
    const worker = new Worker("./worker.mjs", { type: "module" });
    const pending = new Map();
    let id = 0;
    worker.onmessage = ({ data }) => {
      pending.get(data.id)?.(data);
      pending.delete(data.id);
    };
    function convert(spirv, stage, resources = { sbufs: [] }) {
      const key = ++id;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error("Worker timed out")), 90000);
        pending.set(key, (data) => {
          clearTimeout(timer);
          resolve(data);
        });
        worker.postMessage(
          { id: key, type: "convert", spirv, replayContext: { stage, resources } },
          [spirv],
        );
      });
    }
    try {
      const invalid = await convert(new ArrayBuffer(20), "cs");
      if (invalid.ok) throw new Error("Worker accepted malformed SPIR-V");
      const results = [];
      for (const [name, stage] of [
        ["fixture", "cs"],
        ["fragment", "ps"],
        ["storage", "cs"],
        ["fixture", "cs"],
      ]) {
        const bytes = await (await fetch(`./${name}.spv`)).arrayBuffer();
        const resources = {
          sbufs:
            name === "storage" ? [{ binding: 0, cb_slot: 0, cb_offset: 0x310 / 4, flags: 2 }] : [],
        };
        const result = await convert(bytes, stage, resources);
        if (!result.ok) throw new Error(result.error);
        results.push({ name, stage, resources, ...result.result });
      }
      return { invalid, results };
    } finally {
      worker.terminate();
    }
  });
  assert.deepEqual(errors, []);
  assert.equal(outputs.results[0].msl, outputs.results[3].msl, "Repeat calls must be stable");
  for (const result of outputs.results.slice(0, 3)) {
    const nativeMsl = (
      await readFile(resolve(output, `${result.name}.native.metal`), "utf8")
    ).replace(/\r\n/g, "\n");
    const nativeReflection = JSON.parse(
      await readFile(resolve(output, `${result.name}.native.json`), "utf8"),
    );
    assert.equal(result.msl, nativeMsl, `${result.name}: Metal source differs from original CLI`);
    assert.deepEqual(
      result.reflection,
      nativeReflection,
      `${result.name}: SPIR-V reflection differs`,
    );
    const oracle = spawnSync("python3", ["native-oracle.py"], {
      input: JSON.stringify([{ stage: result.stage, msl: nativeMsl, resources: result.resources }]),
      encoding: "utf8",
    });
    assert.equal(oracle.status, 0, oracle.stderr);
    assert.deepEqual(
      result.replay,
      JSON.parse(oracle.stdout)[0],
      `${result.name}: replay fields differ`,
    );
    await writeFile(resolve(output, `${result.name}.browser.metal`), result.msl);
    await writeFile(
      resolve(output, `${result.name}.browser.json`),
      JSON.stringify(result, null, 2),
    );
  }
  await writeFile(
    resolve(output, "browser-report.json"),
    JSON.stringify(
      {
        browser: browser.version(),
        malformedRejected: true,
        repeatedStable: true,
        fixtures: outputs.results
          .slice(0, 3)
          .map((x) => ({ name: x.name, provenance: x.provenance })),
        limitations: [
          "Synthetic only",
          "No Apple Metal compiler",
          "No production chain or package output",
        ],
      },
      null,
      2,
    ),
  );
  console.log(
    "PASS: real Chromium worker matches native MSL bytes, upstream reflection and replay fields (compute + fragment + storage).",
  );
} finally {
  await browser?.close();
  await new Promise((resolve) => server.close(resolve));
}
