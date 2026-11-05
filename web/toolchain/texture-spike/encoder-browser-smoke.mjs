// 运行浏览器纹理编码冒烟验证；依据 web/toolchain/texture-spike 的模块与调用约定。
// Run browser texture-encoding smoke checks using the module and calling contracts in web/toolchain/texture-spike.
import assert from "node:assert/strict";
import process from "node:process";
import http from "node:http";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_ENTRY).href);
const server = http.createServer(async (req, res) => {
  const pathname = new URL(req.url, "http://localhost").pathname;
  if (pathname === "/favicon.ico") {
    res.writeHead(204);
    res.end();
    return;
  }
  if (pathname === "/") {
    res.setHeader("Content-Type", "text/html");
    res.end("<!doctype html><title>Synthetic original BC3 encoder</title>");
    return;
  }
  const target = path.resolve(root, `.${pathname}`);
  if (!target.startsWith(root + path.sep)) {
    res.writeHead(403);
    res.end();
    return;
  }
  try {
    res.setHeader(
      "Content-Type",
      target.endsWith(".wasm")
        ? "application/wasm"
        : /\.(mjs|js)$/.test(target)
          ? "text/javascript"
          : "application/octet-stream",
    );
    res.end(await readFile(target));
  } catch {
    res.writeHead(404);
    res.end();
  }
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
let browser;
try {
  browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  const external = [],
    errors = [];
  const origin = `http://127.0.0.1:${server.address().port}`;
  page.on("request", (r) => {
    if (!r.url().startsWith(origin + "/")) external.push(r.url());
  });
  page.on("pageerror", (e) => errors.push(e.message));
  await page.goto(origin);
  const result = await page.evaluate(async () => {
    const { TextureEncodeClient } = await import("/texture-spike/texture-encode-client.mjs");
    const fixtures = await (await fetch("/texture-spike/.build/encoder-fixtures.json")).json();
    const from64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
    const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
    const client = new TextureEncodeClient();
    try {
      for (const f of fixtures) {
        const request = {
          templateHeader: from64(f.templateHeader),
          rgba: from64(f.rgba),
          width: f.width,
          height: f.height,
        };
        const result = await client.encodeAndPack(request);
        if (!same(result.linearBC3, from64(f.linearBC3)))
          throw new Error(`Original linear BC3 mismatch ${f.width}x${f.height} ${f.pattern}`);
        if (!same(result.sprite, from64(f.sprite)))
          throw new Error(`Original packed bytes mismatch ${f.width}x${f.height} ${f.pattern}`);
        if (request.rgba.byteLength !== f.width * f.height * 4)
          throw new Error("Client detached caller input");
      }
      const templateHeader = from64(fixtures[0].templateHeader);
      const controller = new AbortController();
      const pending = client.encodeAndPack(
        { templateHeader, rgba: new Uint8Array(1024 * 1024 * 4), width: 1024, height: 1024 },
        { signal: controller.signal },
      );
      setTimeout(() => controller.abort(), 5);
      let aborted = false;
      try {
        await pending;
      } catch (e) {
        aborted = e.name === "AbortError";
      }
      if (!aborted) throw new Error("In-flight worker cancellation did not reject");
      const f = fixtures[0];
      const recovered = await client.packLinearBC3({
        templateHeader,
        linearBC3: from64(f.linearBC3),
        width: f.width,
        height: f.height,
      });
      if (!same(recovered.sprite, from64(f.sprite)))
        throw new Error("Cancelled worker did not restart cleanly");
      const pre = new AbortController();
      pre.abort();
      try {
        await client.encodeAndPack(
          { templateHeader, rgba: new Uint8Array(4), width: 1, height: 1 },
          { signal: pre.signal },
        );
        throw new Error("Pre-aborted call accepted");
      } catch (e) {
        if (e.name !== "AbortError") throw e;
      }
      for (const request of [
        { templateHeader, rgba: new Uint8Array(4), width: 8193, height: 1 },
        { templateHeader, rgba: new Uint8Array(3), width: 1, height: 1 },
        {
          templateHeader: templateHeader.subarray(0, 39),
          rgba: new Uint8Array(4),
          width: 1,
          height: 1,
        },
      ]) {
        let failed = false;
        try {
          await client.encodeAndPack(request);
        } catch {
          failed = true;
        }
        if (!failed) throw new Error("Malformed request accepted");
      }
      return {
        fixtures: fixtures.length,
        exactLinear: true,
        exactSprite: true,
        cancellation: true,
      };
    } finally {
      client.close();
    }
  });
  assert.deepEqual(external, [], "No external browser traffic");
  assert.deepEqual(errors, [], "No page errors");
  console.log(JSON.stringify({ ok: true, ...result }));
} finally {
  if (browser) await browser.close();
  await new Promise((resolve) => server.close(resolve));
}
