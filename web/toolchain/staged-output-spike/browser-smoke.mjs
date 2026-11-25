/** 小型真实 Chromium OPFS 验证，并非系统保存选择器或多 GB 耐久测试。 */
/**
 * 小型真实 Chromium OPFS 验证，不是操作系统保存选择器或多 GB 耐久测试。
 * Tiny real Chromium OPFS proof, NOT an OS save-picker or multi-GB endurance test. */
import assert from "node:assert/strict";
import http from "node:http";
import { readFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
const { chromium } = await import(pathToFileURL(process.env.PLAYWRIGHT_ENTRY).href);
const allowed = new Set([
  "browser-staged-output.mjs",
  "staged-zip-validator.mjs",
  "preflight.mjs",
  "zip-writer.mjs",
  "zip64-writer.mjs",
]);
const requests = [];
const server = http.createServer(async (req, res) => {
  requests.push(req.url);
  if (req.url === "/") {
    res.setHeader("Content-Type", "text/html");
    res.end("<!doctype html><title>Local staged output proof</title>");
    return;
  }
  const name = req.url.slice(1);
  if (!allowed.has(name)) {
    res.writeHead(404);
    res.end();
    return;
  }
  res.setHeader("Content-Type", "text/javascript");
  res.end(await readFile(new URL(`../../${name}`, import.meta.url)));
});
let browser;
try {
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ serviceWorkers: "block" });
  const page = await context.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}`);
  const result = await page.evaluate(async () => {
    const { createBrowserStagedOutputAdapters } = await import("/browser-staged-output.mjs");
    const { validateStagedZip } = await import("/staged-zip-validator.mjs");
    const { fileSource } = await import("/preflight.mjs");
    const { writeZip } = await import("/zip-writer.mjs");
    const { writeZip64 } = await import("/zip64-writer.mjs");
    const requireValue = (value) => {
      if (!value) throw new Error("BROWSER_STAGING_ASSERTION");
    };
    const root = await globalThis.navigator.storage.getDirectory();
    const directoryName = `staging-proof-${globalThis.crypto.randomUUID()}`;
    const directory = await root.getDirectoryHandle(directoryName, { create: true });
    const storage = {
      getDirectory: async () => directory,
      estimate: () => globalThis.navigator.storage.estimate(),
    };
    const results = [];
    const names = async () => {
      const keys = [];
      for await (const key of directory.keys()) keys.push(key);
      return keys.sort();
    };
    const sentinel = await directory.getFileHandle("sentinel", { create: true });
    const sentinelWriter = await sentinel.createWritable();
    await sentinelWriter.write("untouched");
    await sentinelWriter.close();
    const manifest = [{ path: "Payload/Test.app/a", size: 65537, mode: 0o100755 }];
    const entries = [
      {
        ...manifest[0],
        mode: 0o755,
        async *chunks() {
          yield new Uint8Array(65536).fill(17);
          yield Uint8Array.of(99);
        },
      },
    ];
    try {
      for (const format of ["zip32", "zip64"]) {
        const destination = await directory.getFileHandle("result", { create: true });
        const adapter = createBrowserStagedOutputAdapters({
          destinationHandle: destination,
          archiveFormat: format,
          maxOutputBytes: 1024 * 1024,
          storage,
        });
        const output = await adapter.createOutput({ manifest });
        await (format === "zip64" ? writeZip64 : writeZip)(
          entries,
          { write: output.write, abort: output.abort, async close() {} },
          { maxTotalBytes: 1024 * 1024 },
        );
        requireValue((await destination.getFile()).size === 0);
        await adapter.validateArchive({ output, manifest });
        await output.close();
        const checked = await validateStagedZip(fileSource(await destination.getFile()), manifest, {
          archiveFormat: format,
          maxOutputBytes: 1024 * 1024,
        });
        requireValue(checked.valid && (await sentinel.getFile()).size === 9);
        requireValue(JSON.stringify(await names()) === JSON.stringify(["result", "sentinel"]));
        await directory.removeEntry("result");
        results.push(`${format}: committed and independently reread`);
      }
      const destination = await directory.getFileHandle("result", { create: true });
      let original = await destination.createWritable();
      await original.write("previous");
      await original.close();
      original = undefined;
      const controller = new AbortController();
      const cancellingDestination = {
        kind: "file",
        queryPermission: (options) => destination.queryPermission(options),
        async createWritable(options) {
          const writer = await destination.createWritable(options);
          return {
            async write(bytes) {
              await writer.write(bytes);
              controller.abort();
            },
            close: () => writer.close(),
            abort: () => writer.abort(),
          };
        },
      };
      const adapter = createBrowserStagedOutputAdapters({
        destinationHandle: cancellingDestination,
        archiveFormat: "zip64",
        maxOutputBytes: 1024 * 1024,
        storage,
      });
      const output = await adapter.createOutput({ manifest, signal: controller.signal });
      await writeZip64(
        entries,
        { write: output.write, abort: output.abort, async close() {} },
        { maxTotalBytes: 1024 * 1024 },
      );
      await adapter.validateArchive({ output, manifest });
      let cancelled = false;
      try {
        await output.close();
      } catch {
        cancelled = true;
      }
      await output.abort();
      requireValue(cancelled && (await destination.getFile()).size === 8);
      requireValue((await (await destination.getFile()).text()) === "previous");
      requireValue(JSON.stringify(await names()) === JSON.stringify(["result", "sentinel"]));
      results.push(
        "cancel during actual destination write: aborted, original preserved, own stage removed",
      );
      const lowQuota = createBrowserStagedOutputAdapters({
        destinationHandle: destination,
        archiveFormat: "zip64",
        maxOutputBytes: 1024 * 1024,
        storage: { ...storage, estimate: async () => ({ quota: 1, usage: 0 }) },
      });
      const lowQuotaOutput = await lowQuota.createOutput({ manifest });
      requireValue((await names()).length === 3);
      await lowQuotaOutput.abort();
      requireValue((await names()).length === 2);
      requireValue((await (await destination.getFile()).text()) === "previous");
      requireValue((await (await sentinel.getFile()).text()) === "untouched");
      results.push(
        "low estimated quota: staging allowed; abort cleans up and preserves existing output",
      );
      return results;
    } finally {
      // 此目录由本隔离测试创建，并非用户存储。
      // This directory was created by this isolated test, not user storage.
      await root.removeEntry(directoryName, { recursive: true });
    }
  });
  assert.equal(result.length, 4);
  assert.ok(
    requests.every((url) => url === "/" || url === "/favicon.ico" || allowed.has(url.slice(1))),
  );
  console.log(
    JSON.stringify(
      {
        status: "passed",
        browser: await browser.version(),
        scenarios: result,
        scope: "tiny real Chromium OPFS, not OS picker or multi-GB",
      },
      null,
      2,
    ),
  );
} finally {
  await browser?.close();
  await new Promise((resolve) => server.close(resolve));
}
