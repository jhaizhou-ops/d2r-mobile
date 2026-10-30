// 合成输入的真实工作线程集成，使用独立的原始原生命令行参考结果。
// Synthetic real-worker integration, independent original native CLI oracle.
import assert from "node:assert/strict";
import { readFile, writeFile, mkdir, readdir, stat } from "node:fs/promises";
import { createServer } from "node:http";
import { resolve, join, extname, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";
import { fixtures } from "./graphics-fixtures.mjs";
const root = fileURLToPath(new URL("../../../", import.meta.url));
const webRoot = join(root, "web");
const [command, first, second, third] = process.argv.slice(2);
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const expectedSource = (
  await readFile(new URL("../shader-spike/upstream-source.sha256", import.meta.url), "utf8")
).split(/\s/)[0];
assert.equal(
  hash(await readFile(join(root, "dm/gfx/tools/dmsh_xlate/Program.cs"))),
  expectedSource,
  "Translator native source changed: review bridge first",
);
if (command === "prepare") {
  assert.ok(first);
  const directory = resolve(first);
  await mkdir(directory, { recursive: true });
  const jobs = [];
  for (const fixture of fixtures) {
    const codePath = join(directory, `${fixture.name}.bin`);
    await writeFile(codePath, fixture.code);
    jobs.push({ ...fixture.job, code: codePath });
  }
  await writeFile(join(directory, "jobs.json"), JSON.stringify(jobs, null, 2));
} else if (command === "capture") {
  assert.ok(
    first && second && third,
    "capture <translator-publish-directory> <native-directory> <output-directory>",
  );
  const output = resolve(third);
  await mkdir(output, { recursive: true });
  const candidates = [];
  async function discover(directory, depth = 0) {
    if (depth > 6) return;
    const entries = await readdir(directory, { withFileTypes: true });
    if (entries.some((e) => e.name === "worker.mjs" && e.isFile())) {
      try {
        if ((await stat(join(directory, "_framework/dotnet.js"))).isFile())
          candidates.push(directory);
      } catch {
        /* 不是发布的运行时。Not published runtime. */
      }
    }
    for (const e of entries)
      if (e.isDirectory() && e.name !== "_framework")
        await discover(join(directory, e.name), depth + 1);
  }
  await discover(resolve(first));
  assert.equal(candidates.length, 1, "Expected exactly one complete translator publish");
  const translatorRoot = candidates[0],
    native = resolve(second),
    oracles = new Map(),
    aggregate = createHash("sha256");
  for (const fixture of [...fixtures].sort((a, b) => a.job.key.localeCompare(b.job.key))) {
    const key = fixture.job.key;
    const msl = (await readFile(join(native, `${key}_b.metal`), "utf8")).replace(/\r\n/g, "\n");
    const spirv = await readFile(join(native, `${key}.spv`));
    const resources = JSON.parse(await readFile(join(native, `${key}.res.json`), "utf8"));
    const reflection = JSON.parse(await readFile(join(native, `${key}.cross.json`), "utf8"));
    aggregate.update(`${key}_b.metal`).update(createHash("sha256").update(msl).digest());
    oracles.set(key, { msl, spirv, resources, reflection });
  }
  const sourceDigest = aggregate.digest("hex").slice(0, 16);
  // 合成预期配方来自独立的原生夹具输出，而非浏览器输出；它并不构成对真实发布版壳的信任断言。
  // Synthetic expected recipe originates from independent native fixture output,
  // never from browser output. It is not a real release-shell trust assertion.
  const chain = "000000000001",
    targetOS = { platform: "ios", minimumVersion: "16.0" },
    requiredPaths = [`msl/b-${chain}/`];
  const shell = {
    entries: ["Info.plist", "App", "dm/shell_info.txt", "dm/shell_manifest.txt"].map((p) => ({
      path: `Payload/App.app/${p}`,
      size: 10,
      mode: 0o100644,
    })),
    shellInfoText: `PRODUCT=synthetic\nCHAIN=${chain}\nKIND=ios\nSHADER_SRC=${sourceDigest}\nSPIRV_CROSS=1.4.363.0\n`,
    shellManifestText: requiredPaths.join("\n"),
    observedTargetOS: targetOS,
    trustedRecipe: {
      schemaVersion: 1,
      product: "synthetic",
      chain,
      kind: "ios",
      shaderDigest: sourceDigest,
      targetOS,
      requiredPaths,
      shaderKeys: fixtures.map((f) => f.job.key),
      spirvCrossVersion: "1.4.363.0",
    },
  };
  const mime = {
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".json": "application/json",
    ".wasm": "application/wasm",
  };
  const server = createServer(async (request, response) => {
    try {
      const pathname = new URL(request.url, "http://localhost").pathname;
      if (pathname === "/") {
        response.setHeader("Content-Type", "text/html");
        response.end("<!doctype html><title>Synthetic linked shader pipeline</title>");
        return;
      }
      const [base, relative] = pathname.startsWith("/translator/")
        ? [translatorRoot, pathname.slice("/translator/".length)]
        : pathname.startsWith("/web/")
          ? [webRoot, pathname.slice("/web/".length)]
          : [null, null];
      assert.ok(base);
      const path = resolve(base, relative);
      assert.ok(path.startsWith(base + sep));
      response.setHeader("Content-Type", mime[extname(path)] ?? "application/octet-stream");
      response.end(await readFile(path));
    } catch {
      response.writeHead(404);
      response.end();
    }
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const require = createRequire(new URL("../shader-spike/package.json", import.meta.url));
  const { chromium } = require("playwright");
  let browser;
  try {
    browser = await chromium.launch({ headless: true });
    const page = await browser.newPage();
    const pageErrors = [];
    page.on("pageerror", (e) => pageErrors.push(e.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    const captured = await page.evaluate(
      async ({ shell, fixtures }) => {
        const { createShaderPipeline } = await import("/web/shader-pipeline.mjs");
        const options = {
          trustedShell: shell,
          translatorWorkerUrl: "/translator/worker.mjs",
          crossWorkerUrl: "/web/toolchain/spirv-cross-spike/wwwroot/worker.mjs",
        };
        // 同时测试实际 C# 桥接，不仅测试适配器侧拒绝。
        // Exercise the actual C# bridge too, not only adapter-side rejection.
        const rawWorker = new Worker(options.translatorWorkerUrl, { type: "module" });
        let rawId = 0;
        async function mustReject(job, code, expected) {
          const requestId = ++rawId;
          const reply = await new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
              rawWorker.removeEventListener("message", listener);
              reject(new Error("Raw bridge rejection timed out"));
            }, 60000);
            const listener = ({ data }) => {
              if (data.requestId !== requestId) return;
              clearTimeout(timer);
              rawWorker.removeEventListener("message", listener);
              resolve(data);
            };
            rawWorker.addEventListener("message", listener);
            rawWorker.postMessage({ requestId, job, code });
          });
          if (typeof reply.error !== "string" || !reply.error.includes(expected))
            throw new Error(
              `Actual bridge did not reject invalid graphics input: ${JSON.stringify(reply)}`,
            );
        }
        try {
          const vertex = fixtures.find((f) => f.job.stage === "vs");
          await mustReject(vertex.job, new Uint8Array(16), "SPH");
          await mustReject(vertex.job, new Uint8Array(vertex.code.length), "SPH stage");
          await mustReject(
            { ...vertex.job, attr: { 32: "float" } },
            new Uint8Array(vertex.code),
            "Invalid graphics",
          );
          await mustReject(
            { ...vertex.job, stage: "gs" },
            new Uint8Array(vertex.code),
            "unsupported",
          );
        } finally {
          rawWorker.terminate();
        }
        const pipeline = createShaderPipeline(options);
        try {
          let incompleteRejected = false;
          try {
            await pipeline.verifyCompleteSourceSet();
          } catch {
            incompleteRejected = true;
          }
          const results = [];
          for (const fixture of fixtures)
            results.push(
              await pipeline.translate({ job: fixture.job, code: new Uint8Array(fixture.code) }),
            );
          const verified = await pipeline.verifyCompleteSourceSet();
          return {
            results,
            verified,
            incompleteRejected,
            invalidGraphicsRejected: 3,
            unsupportedStageRejected: true,
          };
        } finally {
          pipeline.dispose();
        }
      },
      { shell, fixtures: fixtures.map((f) => ({ ...f, code: [...f.code] })) },
    );
    assert.deepEqual(pageErrors, []);
    assert.equal(captured.incompleteRejected, true);
    assert.equal(captured.results.length, fixtures.length);
    assert.equal(captured.invalidGraphicsRejected, 3);
    assert.equal(captured.unsupportedStageRejected, true);
    const evidence = [];
    for (const fixture of fixtures) {
      const key = fixture.job.key,
        result = captured.results.find((r) => r.key === key),
        expected = oracles.get(key);
      assert.equal(result.stage, fixture.job.stage);
      assert.equal(result.msl, expected.msl, `${fixture.name}: linked browser MSL bytes differ`);
      assert.deepEqual(
        result.resources,
        expected.resources,
        `${fixture.name}: translator resource sidecar differs`,
      );
      assert.equal(
        result.provenance.inputSha256,
        hash(expected.spirv),
        `${fixture.name}: translator SPIR-V SHA differs`,
      );
      assert.deepEqual(
        result.spirvReflection,
        expected.reflection,
        `${fixture.name}: Cross reflection differs`,
      );
      const oracle = spawnSync(
        "python3",
        [join(webRoot, "toolchain/spirv-cross-spike/native-oracle.py")],
        {
          input: JSON.stringify([
            { stage: fixture.job.stage, msl: expected.msl, resources: expected.resources },
          ]),
          encoding: "utf8",
        },
      );
      assert.equal(oracle.status, 0, oracle.stderr);
      assert.deepEqual(result.replay, JSON.parse(oracle.stdout)[0]);
      assert.equal(
        result.replay.msl_entry_kind,
        { cs: "kernel", vs: "vertex", ps: "fragment" }[fixture.job.stage],
      );
      if (fixture.job.stage === "vs") {
        assert.ok(
          result.replay.vertex_inputs.some((x) => x.location === 0),
          "Authored vertex input0 disappeared",
        );
        assert.match(result.msl, /\[\[position\]\]/, "Authored vertex position output disappeared");
      }
      if (fixture.job.stage === "ps") {
        assert.deepEqual(result.replay.raster.colors, [0], "Authored color0 output disappeared");
        assert.equal(result.replay.raster.discard, 0);
      }
      await writeFile(join(output, `${key}.browser.metal`), result.msl);
      evidence.push({
        name: fixture.name,
        stage: fixture.job.stage,
        codeSha256: hash(fixture.code),
        spirvSha256: hash(expected.spirv),
        mslSha256: hash(expected.msl),
        replay: result.replay,
      });
    }
    assert.equal(captured.verified.sourceDigest, sourceDigest);
    assert.equal(captured.verified.shaderCount, fixtures.length);
    assert.equal(captured.verified.installable, false);
    assert.equal(captured.verified.chainRewritten, false);
    await writeFile(join(output, "linked-browser-result.json"), JSON.stringify(captured, null, 2));
    await writeFile(
      join(output, "report.json"),
      JSON.stringify(
        {
          commit: process.env.GITHUB_SHA ?? null,
          browser: browser.version(),
          fixtures: evidence,
          sourceDigest,
          byteAndReflectionParity: true,
          realLinkedModuleWorkers: true,
          invalidGraphicsRejected: captured.invalidGraphicsRejected,
          unsupportedStageRejected: captured.unsupportedStageRejected,
          installable: false,
          scope:
            "Authored compute/vertex/fragment only; no real release shell, game coverage, readable-tile variants or Metal compilation",
        },
        null,
        2,
      ),
    );
    console.log(
      "PASS: real browser compute/vertex/fragment translator -> SPIRV-Cross adapter matches native MSL/resources/SPIR-V hash/reflection and complete synthetic SHADER_SRC gate.",
    );
  } finally {
    await browser?.close();
    await new Promise((resolve) => server.close(resolve));
  }
} else throw new Error("Expected prepare or capture command");
