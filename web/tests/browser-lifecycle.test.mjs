// Feature suite: browser-lifecycle. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as promisesModule from "node:fs/promises";
import * as typescriptModule from "typescript";
import * as browserPackagerModule from "../browser-packager.mjs";
import * as packagingJobModule from "../packaging-job.mjs";
import * as packagingDiagnosticsModule from "../packaging-diagnostics.mjs";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as nodeChild_processModule from "node:child_process";
import * as browserStagedOutputModule from "../browser-staged-output.mjs";
import * as zipWriterModule from "../zip-writer.mjs";
import * as zip64WriterModule from "../zip64-writer.mjs";
import * as stagedZipValidatorModule from "../staged-zip-validator.mjs";
import * as nodeEventsModule from "node:events";
import * as browserTaskYieldModule from "../browser-task-yield.mjs";
import * as assetOutputCacheModule from "../asset-output-cache.mjs";
import * as downloadPreparationModule from "../download-preparation.mjs";
import * as localOutputCacheModule from "../local-output-cache.mjs";
import * as opfsStorageAdvisoryModule from "../opfs-storage-advisory.mjs";

// browser-capabilities
{
  // 合成 API 存在性检查，不访问浏览器权限或更改共享全局对象。
  // Synthetic API presence checks do not access browser permissions or alter shared globals.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { default: ts } = typescriptModule;
  const { assertBrowserPackagerCapabilities } = browserPackagerModule;

  function supported() {
    const unused = () => assert.fail("presence check must not invoke APIs");
    return {
      isSecureContext: true,
      navigator: {
        locks: { request: unused },
        storage: { getDirectory: unused, estimate: unused },
      },
      Worker: unused,
      DecompressionStream: class {
        constructor(format) {
          assert.equal(format, "gzip");
        }
      },
      crypto: { subtle: {}, randomUUID: unused },
      AbortSignal: { any: unused, prototype: { throwIfAborted: unused } },
    };
  }

  test("complete capability surface passes without requesting permissions or reading input", () => {
    assert.doesNotThrow(() => assertBrowserPackagerCapabilities(supported()));
  });

  test("storage estimates are optional and never read by the capability check", () => {
    const environment = supported();
    delete environment.navigator.storage.estimate;
    assert.doesNotThrow(() => assertBrowserPackagerCapabilities(environment));
    Object.defineProperty(environment.navigator.storage, "estimate", {
      get() {
        throw new Error("storage estimates must not gate browser support");
      },
    });
    assert.doesNotThrow(() => assertBrowserPackagerCapabilities(environment));
  });

  test("missing or non-callable required methods use the existing unsupported code", () => {
    for (const path of [
      ["navigator", "storage", "getDirectory"],
      ["navigator", "locks", "request"],
      ["Worker"],
      ["DecompressionStream"],
      ["crypto", "randomUUID"],
      ["AbortSignal", "any"],
      ["AbortSignal", "prototype", "throwIfAborted"],
    ]) {
      for (const value of [undefined, null, true, {}]) {
        const environment = supported();
        let owner = environment;
        for (const key of path.slice(0, -1)) owner = owner[key];
        owner[path.at(-1)] = value;
        assert.throws(() => assertBrowserPackagerCapabilities(environment), {
          message: "BROWSER_UNSUPPORTED",
        });
      }
    }
  });

  test("missing parent objects and insecure contexts reject without ReferenceError", () => {
    for (const override of [
      { isSecureContext: false },
      { navigator: undefined },
      { navigator: {} },
      { crypto: undefined },
      { crypto: { randomUUID() {} } },
      { AbortSignal: undefined },
      { AbortSignal: { any() {} } },
    ]) {
      assert.throws(() => assertBrowserPackagerCapabilities({ ...supported(), ...override }), {
        message: "BROWSER_UNSUPPORTED",
      });
    }
  });

  function assertCapabilityOrder(source) {
    const file = ts.createSourceFile(
      "packager.mjs",
      source,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.JS,
    );
    assert.equal(file.parseDiagnostics.length, 0);
    const entry = file.statements.find(
      (node) => ts.isFunctionDeclaration(node) && node.name?.text === "prepareBrowserPackager",
    );
    assert.ok(entry?.body);
    for (const kind of [ts.SyntaxKind.ExportKeyword, ts.SyntaxKind.AsyncKeyword])
      assert.ok(entry.modifiers?.some((modifier) => modifier.kind === kind));
    const nodes = [];
    const visit = (node) => {
      if (ts.isFunctionLike(node)) {
        // 跳过未调用的定义，但立即调用的函数体仍参与前置等待检查。
        // Skip inert definitions, but include immediately invoked bodies in the pre-check await rule.
        let callee = node;
        while (ts.isParenthesizedExpression(callee.parent)) callee = callee.parent;
        if (!ts.isCallExpression(callee.parent) || callee.parent.expression !== callee) return;
      }
      nodes.push(node);
      ts.forEachChild(node, visit);
    };
    visit(entry.body);
    const call = (name) => {
      const matches = nodes.filter(
        (node) =>
          ts.isCallExpression(node) &&
          ts.isIdentifier(node.expression) &&
          node.expression.text === name,
      );
      assert.equal(matches.length, 1, `Expected one ${name} call`);
      return matches[0];
    };
    const check = call("assertBrowserPackagerCapabilities"),
      setup = call("prepareSealedRuntime"),
      profile = call("loadProductionProfile");
    assert.ok(
      entry.body.statements.some(
        (node) => ts.isExpressionStatement(node) && node.expression === check,
      ),
    );
    assert.ok(ts.isAwaitExpression(setup.parent));
    assert.ok(ts.isAwaitExpression(profile.parent));
    assert.ok(check.pos < setup.pos && setup.pos < profile.pos);
    assert.equal(
      nodes.some((node) => ts.isAwaitExpression(node) && node.pos < check.pos),
      false,
    );
  }

  test("capability ordering ignores harmless comments and formatting, not missing or reordered checks", () => {
    for (const source of [
      `export async function prepareBrowserPackager() {
      // 验证前不等待。 | Do not await setup before the check.
      assertBrowserPackagerCapabilities();
      const runtime = await prepareSealedRuntime();
      await loadProductionProfile(runtime);
    }`,
      `export async function prepareBrowserPackager () {
      const unused = async () => await unrelatedWork();
      assertBrowserPackagerCapabilities /* 校验 | Check */ ( );
      const runtime = await prepareSealedRuntime\n( );
      await loadProductionProfile\n(runtime);
    }`,
    ])
      assert.doesNotThrow(() => assertCapabilityOrder(source));

    for (const body of [
      "/* assertBrowserPackagerCapabilities(); */ const runtime = await prepareSealedRuntime(); await loadProductionProfile(runtime);",
      "const runtime = await prepareSealedRuntime(); assertBrowserPackagerCapabilities(); await loadProductionProfile(runtime);",
      "await otherWork(); assertBrowserPackagerCapabilities(); const runtime = await prepareSealedRuntime(); await loadProductionProfile(runtime);",
      "(async () => { await otherWork(); })(); assertBrowserPackagerCapabilities(); const runtime = await prepareSealedRuntime(); await loadProductionProfile(runtime);",
      "((async function () { await otherWork(); }))(); assertBrowserPackagerCapabilities(); const runtime = await prepareSealedRuntime(); await loadProductionProfile(runtime);",
      "function unused() { assertBrowserPackagerCapabilities(); } const runtime = await prepareSealedRuntime(); await loadProductionProfile(runtime);",
      "assertBrowserPackagerCapabilities(); const runtime = prepareSealedRuntime(); await loadProductionProfile(runtime);",
      "assertBrowserPackagerCapabilities(); await loadProductionProfile(); const runtime = await prepareSealedRuntime();",
    ])
      assert.throws(() =>
        assertCapabilityOrder(`export async function prepareBrowserPackager() { ${body} }`),
      );
  });

  test("production bootstrap checks capabilities before runtime setup and private input methods", async () => {
    const source = await readFile(new URL("../browser-packager.mjs", import.meta.url), "utf8");
    assertCapabilityOrder(source);
  });

  test("gzip constructor rejection uses the existing unsupported code", () => {
    for (const DecompressionStream of [
      () => undefined,
      class {
        constructor() {
          throw new TypeError("unsupported gzip");
        }
      },
    ]) {
      assert.throws(
        () => assertBrowserPackagerCapabilities({ ...supported(), DecompressionStream }),
        { message: "BROWSER_UNSUPPORTED" },
      );
    }
  });
}

// browser-cleanup-errors
{
  // 合成清理错误检查：不读取游戏文件，不输出原始错误文本。
  // Synthetic cleanup failure checks: no game files and no raw error text in UI summaries.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { cleanupPackagingAdapters } = browserPackagerModule;
  const { PackagingJobError } = packagingJobModule;
  const { packagingFailureSummary } = packagingDiagnosticsModule;

  test("successful cleanup retains order and awaits both operations", async () => {
    const calls = [];
    assert.equal(
      await cleanupPackagingAdapters(
        async () => {
          await Promise.resolve();
          calls.push("extraction");
        },
        async () => calls.push("adapters"),
      ),
      undefined,
    );
    assert.deepEqual(calls, ["extraction", "adapters"]);
  });
  test("single extraction failure retains exact identity and still closes adapters", async () => {
    const primary = new Error("first");
    let closed = false;
    await assert.rejects(
      cleanupPackagingAdapters(
        () => {
          throw primary;
        },
        async () => {
          closed = true;
        },
      ),
      (error) => error === primary,
    );
    assert.equal(closed, true);
  });
  test("single adapter-close failure retains exact identity", async () => {
    const cleanup = new Error("second");
    await assert.rejects(
      cleanupPackagingAdapters(
        async () => {},
        async () => {
          throw cleanup;
        },
      ),
      (error) => error === cleanup,
    );
  });
  test("dual failure preserves original cause and both errors without exposing raw details", async () => {
    const first = new Error("private source path"),
      second = new Error("private cleanup detail");
    await assert.rejects(
      cleanupPackagingAdapters(
        async () => {
          throw first;
        },
        () => {
          throw second;
        },
      ),
      (error) => {
        assert.ok(error instanceof AggregateError);
        assert.equal(error.cause, first);
        assert.deepEqual(error.errors, [first, second]);
        const wrapped = new PackagingJobError(error, {
          stage: "cleanup",
          code: "STAGE_FAILED",
          cleanupCodes: ["RESOURCE_CLEANUP_FAILED"],
        });
        assert.equal(wrapped.cause.cause, first);
        const summary = packagingFailureSummary(wrapped.report);
        assert.ok(!summary.includes("private"));
        return true;
      },
    );
  });
}

// browser-legacy-astc
{
  // 合成生命周期测试不构成浏览器、真实游戏或原生兼容性证明。
  // Synthetic lifecycle tests are not browser, real-game or native compatibility proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { withLegacyAstcProducerLease, prepareBrowserPackager, LEGACY_ASTC_PRODUCTION_LIMITS } =
    browserPackagerModule;
  const { PackagingJobError } = packagingJobModule;

  test("default operation needs no producer and preserves result", async () => {
    const result = { committed: true };
    assert.equal(await withLegacyAstcProducerLease(undefined, async () => result), result);
  });
  test("lease closes only after export and job cleanup settle", async () => {
    const events = [];
    await withLegacyAstcProducerLease({ close: async () => events.push("lease") }, async () => {
      events.push("export");
      await Promise.resolve();
      events.push("job-cleanup");
    });
    assert.deepEqual(events, ["export", "job-cleanup", "lease"]);
  });
  test("primary error identity survives successful cleanup", async () => {
    const primary = new Error("cancelled");
    let closed = false;
    await assert.rejects(
      withLegacyAstcProducerLease(
        {
          close: async () => {
            closed = true;
          },
        },
        async () => {
          throw primary;
        },
      ),
      (error) => error === primary,
    );
    assert.equal(closed, true);
  });
  test("cleanup failure preserves primary cause and observable cleanup", async () => {
    const primary = new Error("quota");
    const cleanup = new Error("lease");
    const failure = new PackagingJobError(primary, { cleanupCodes: [], committed: false });
    await assert.rejects(
      withLegacyAstcProducerLease(
        {
          close: async () => {
            throw cleanup;
          },
        },
        async () => {
          throw failure;
        },
      ),
      (error) => {
        assert.equal(error.cause, primary);
        assert.deepEqual(error.cleanupErrors, [cleanup]);
        assert.deepEqual(error.report.cleanupCodes, ["LEGACY_ASTC_CLEANUP_FAILED"]);
        return true;
      },
    );
  });
  test("cleanup after IPA commit reports committed outcome instead of silent success", async () => {
    await assert.rejects(
      withLegacyAstcProducerLease(
        {
          close: async () => {
            throw new Error("lease");
          },
        },
        async () => ({ committed: true }),
      ),
      (error) => {
        assert.equal(error.report.committed, true);
        assert.equal(error.report.stage, "cleanup");
        return true;
      },
    );
  });
  test("option fails closed and policy limits are explicit", async () => {
    await assert.rejects(prepareBrowserPackager({ legacyAstc: "yes" }), /LEGACY_ASTC_OPTION/);
    for (const astcPreset of ["other", "FAST", null, 1])
      await assert.rejects(prepareBrowserPackager({ astcPreset }), /ASTC_PRESET_OPTION/);
    assert.equal(LEGACY_ASTC_PRODUCTION_LIMITS.maxPackBytes, 8 * 1024 ** 3);
    assert.ok(Object.isFrozen(LEGACY_ASTC_PRODUCTION_LIMITS));
  });

  test("conversion diagnostics expose only safe multi-pass counts and ignore observer failures", async () => {
    const { createLegacyAstcDiagnosticObserver } = await import("../browser-packager.mjs");
    const events = [];
    const emit = createLegacyAstcDiagnosticObserver((event) => events.push(event));
    emit({
      phase: "converting",
      processedBytes: 12,
      completedBlocks: 3,
      completedRecords: 1,
      path: "private",
      key: "secret",
      totalBytes: 100,
    });
    assert.deepEqual(events, [
      {
        stage: "assets",
        state: "running",
        diagnostic: {
          kind: "legacy-astc",
          phase: "converting",
          processedBytes: 12,
          completedBlocks: 3,
          completedRecords: 1,
        },
      },
    ]);
    assert.ok(Object.isFrozen(events[0].diagnostic));
    for (const invalid of [
      { phase: "unknown", processedBytes: 0, completedBlocks: 0 },
      { phase: "converting", processedBytes: -1, completedBlocks: 0 },
      { phase: "converting", processedBytes: 0, completedBlocks: Infinity },
    ])
      emit(invalid);
    assert.equal(events.length, 1);
    const valid = { phase: "complete", processedBytes: 12, completedBlocks: 3 };
    createLegacyAstcDiagnosticObserver(() => {
      throw new Error("observer");
    })(valid);
    createLegacyAstcDiagnosticObserver(async () => {
      throw new Error("observer");
    })(valid);
    const controller = new AbortController();
    controller.abort();
    createLegacyAstcDiagnosticObserver(
      () => assert.fail("aborted observer"),
      controller.signal,
    )(valid);
    await new Promise((resolve) => setImmediate(resolve));
  });

  test("only exact producer detail codes survive failure summary without raw text", async () => {
    const { legacyAstcFailureDetailCode } = await import("../browser-packager.mjs");
    const { packagingFailureSummary } = await import("../packaging-diagnostics.mjs");
    for (const detailCode of ["STORAGE_QUOTA", "SIZE_LIMIT", "INTEGRITY_FAILED", "CODEC_FAILED"]) {
      const cause = Object.assign(new Error("private filename key"), { detailCode });
      const error = new PackagingJobError(cause, { stage: "assets", code: "STAGE_FAILED" });
      assert.equal(legacyAstcFailureDetailCode(error), detailCode);
      const summary = packagingFailureSummary({
        ...error.report,
        detailCode: legacyAstcFailureDetailCode(error),
      });
      assert.ok(summary.includes(`[${detailCode}]`));
      assert.ok(!summary.includes("private filename key"));
    }
    const cause = Object.assign(new Error("private"), { detailCode: "PRIVATE_PATH" });
    assert.equal(legacyAstcFailureDetailCode(new PackagingJobError(cause, {})), undefined);
    assert.ok(!packagingFailureSummary({ detailCode: "PRIVATE_PATH" }).includes("PRIVATE_PATH"));
  });
}

// browser-resource-skip
{
  // 合成策略测试；跳过只影响原始资源导出，不证明已有目录有效。
  // Synthetic policy tests; skipping affects only original resource export, not validity of existing folders.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { runOptionalResourceExport } = browserPackagerModule;

  test("default and explicit true export original resources once and preserve errors", async () => {
    let calls = 0;
    const result = { status: "complete", directoryName: "generated" };
    const exporter = async () => {
      calls++;
      return result;
    };
    assert.equal(await runOptionalResourceExport(undefined, exporter), result);
    assert.equal(await runOptionalResourceExport(true, exporter), result);
    assert.equal(calls, 2);
    const primary = new Error("quota");
    await assert.rejects(
      runOptionalResourceExport(true, async () => {
        throw primary;
      }),
      (error) => error === primary,
    );
  });
  test("explicit false never calls exporter or claims completion or verification", async () => {
    const result = await runOptionalResourceExport(false, () => assert.fail("must not export"));
    assert.deepEqual(result, { status: "skipped", reason: "user-requested" });
    assert.ok(Object.isFrozen(result));
    assert.equal(result.directoryName, undefined);
  });
  test("ambiguous skip values reject before side effects", async () => {
    for (const value of [null, 0, 1, "false", {}, []])
      await assert.rejects(
        runOptionalResourceExport(value, () => assert.fail("must not export")),
        /RESOURCE_EXPORT_OPTION/,
      );
  });
  test("wiring keeps authenticated reads and mandatory supplemental export outside skip callback", async () => {
    const source = await readFile(new URL("../browser-packager.mjs", import.meta.url), "utf8");
    const start = source.indexOf("async createOutput(context)");
    const auth = source.indexOf("readAuthenticatedAssets(authenticated, selected", start);
    const optional = source.indexOf("runOptionalResourceExport(exportResources", start);
    const outcome = source.indexOf("resourceOutcome = exportResources", optional);
    const supplemental = source.indexOf("exportLegacyDevicePack({", outcome);
    const output = source.indexOf("output = createBrowserBlobOutputAdapters", supplemental);
    assert.ok(
      start < auth &&
        auth < optional &&
        optional < outcome &&
        outcome < supplemental &&
        supplemental < output,
    );
    assert.match(
      source,
      /if \(!exportResources\) resources = resourceOutcome = RESOURCE_EXPORT_SKIPPED/,
    );
  });
}

// browser-signing-lifetime
{
  // 将模块模拟与所有真实实现测试隔离。
  // Isolate module mocks from all actual implementation tests.
  const { default: test } = nodeTestModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;
  const { spawnSync } = nodeChild_processModule;
  test("packager signing lifetime and unsigned fallback (isolated mocks)", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/browser-signing-lifetime.mock.mjs", import.meta.url).pathname,
      ],
      { env, encoding: "utf8", timeout: 30000, maxBuffer: 1024 * 1024 },
    );
    assertIsolatedRun(result);
  });
}

// browser-staged-output
{
  // 根据 docs/packaging-export.md，使用内存文件系统替身验证暂存输出的提交、取消和清理契约。
  // Verify staged-output commit, cancellation, and cleanup contracts using an in-memory filesystem double, based on docs/packaging-export.md.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createBrowserStagedOutputAdapters } = browserStagedOutputModule;
  const { writeZip } = zipWriterModule;
  const { writeZip64 } = zip64WriterModule;
  const { planStagedZip } = stagedZipValidatorModule;
  const uuid = "12345678-1234-4123-8123-123456789abc",
    name = `d2rm-output-${uuid}.tmp`;
  const manifest = [{ path: "Payload/Test.app/a", size: 3, mode: 0o100755 }];
  const entries = [
    {
      ...manifest[0],
      mode: 0o755,
      async *chunks() {
        yield Uint8Array.of(1, 2, 3);
      },
    },
  ];
  function environment(faults = {}) {
    const files = new Map([["unrelated", Uint8Array.of(9)]]),
      events = [];
    let committed = Uint8Array.of(7),
      stageBytes,
      destinationOpens = 0;
    const handle = (key, destination = false) => ({
      kind: "file",
      async queryPermission() {
        return faults.permission ?? "granted";
      },
      async getFile() {
        return new Blob([files.get(key)]);
      },
      async createWritable() {
        if (destination) destinationOpens++;
        else faults.onStageOpen?.();
        const chunks = [];
        return {
          async write(bytes) {
            events.push(destination ? "destination-write" : "stage-write");
            if (faults[destination ? "destinationWrite" : "stageWrite"])
              throw new Error("disk full");
            chunks.push(bytes.slice());
            if (destination) faults.onDestinationWrite?.();
          },
          async close() {
            events.push(destination ? "destination-close" : "stage-close");
            if (faults[destination ? "destinationClose" : "stageClose"])
              throw new Error("close failed");
            const data = new Uint8Array(Buffer.concat(chunks));
            if (destination) {
              committed = data;
              faults.onDestinationClose?.();
            } else {
              files.set(key, data);
              stageBytes = data;
            }
          },
          async abort() {
            events.push(destination ? "destination-abort" : "stage-abort");
            await faults[destination ? "onDestinationAbort" : "onStageAbort"]?.();
            if (faults[destination ? "destinationAbort" : "stageAbort"])
              throw new Error("abort failed");
          },
        };
      },
    });
    const storage = {
      async estimate() {
        return { usage: 0, quota: faults.quota ?? 10 ** 9 };
      },
      async getDirectory() {
        return {
          async getFileHandle(key, options) {
            if (!files.has(key) && !options?.create)
              throw new DOMException("missing", "NotFoundError");
            if (!files.has(key)) files.set(key, new Uint8Array());
            return handle(key);
          },
          async removeEntry(key) {
            events.push(`remove:${key}`);
            if (faults.remove) throw new Error("remove failed");
            files.delete(key);
          },
        };
      },
    };
    return {
      files,
      events,
      storage,
      destinationHandle: handle("destination", true),
      get committed() {
        return committed;
      },
      get stageBytes() {
        return stageBytes;
      },
      get destinationOpens() {
        return destinationOpens;
      },
    };
  }
  function adapters(env, format = "zip64", overrides = {}) {
    return createBrowserStagedOutputAdapters({
      destinationHandle: env.destinationHandle,
      storage: env.storage,
      secureContext: true,
      randomUUID: () => uuid,
      archiveFormat: format,
      maxOutputBytes: 1024 * 1024,
      ...overrides,
    });
  }
  async function writeStage(adapter, format = "zip64", signal) {
    const output = await adapter.createOutput({ manifest, signal });
    await (format === "zip64" ? writeZip64 : writeZip)(
      entries,
      { write: output.write, async close() {}, abort: output.abort },
      { maxTotalBytes: 1024 * 1024, signal },
    );
    return output;
  }
  for (const format of ["zip32", "zip64"])
    test(`${format}: validation precedes destination opening; cleanup precedes commit`, async () => {
      const env = environment(),
        adapter = adapters(env, format),
        output = await writeStage(adapter, format);
      await assert.rejects(output.close(), /OUTPUT_NOT_VALIDATED/);
      assert.equal((await adapter.validateArchive({ output, manifest })).valid, true);
      assert.equal(env.destinationOpens, 0);
      await assert.rejects(output.write(Uint8Array.of(0)), /OUTPUT_NOT_WRITABLE/);
      await output.close();
      await output.abort();
      assert.equal(
        env.committed.length,
        planStagedZip(manifest, { archiveFormat: format, maxOutputBytes: 1024 * 1024 }).totalBytes,
      );
      assert.deepEqual([...env.files.keys()], ["unrelated"]);
      assert.ok(env.events.indexOf(`remove:${name}`) < env.events.indexOf("destination-close"));
    });
  test("missing, failed, invalid and low quota estimates do not gate a valid archive", async () => {
    for (const estimate of [
      undefined,
      async () => {
        throw new Error("estimate unavailable");
      },
      async () => ({ quota: NaN, usage: 0 }),
      async () => ({ quota: 0, usage: 0 }),
    ]) {
      const env = environment();
      env.storage.estimate = estimate;
      const adapter = adapters(env),
        output = await writeStage(adapter);
      await adapter.validateArchive({ output, manifest });
      await output.close();
      assert.deepEqual(env.committed, env.stageBytes);
      assert.equal(env.destinationOpens, 1);
      assert.deepEqual([...env.files.keys()], ["unrelated"]);
    }
  });
  test("permission, capability and collisions fail without collateral cleanup", async () => {
    const denied = environment({ permission: "prompt" });
    await assert.rejects(
      adapters(denied).createOutput({ manifest }),
      /OUTPUT_DESTINATION_PERMISSION_REQUIRED/,
    );
    assert.deepEqual([...denied.files.keys()], ["unrelated"]);
    assert.equal(denied.destinationOpens, 0);
    const env = environment();
    env.files.set(name, Uint8Array.of(42));
    await assert.rejects(adapters(env).createOutput({ manifest }), /OUTPUT_TEMP_COLLISION/);
    assert.deepEqual(env.files.get(name), Uint8Array.of(42));
    await assert.rejects(
      adapters(env, "zip64", { secureContext: false }).createOutput({ manifest }),
      /OUTPUT_OPFS_UNAVAILABLE/,
    );
  });
  test("short/excess archives and alien output/manifest fail closed", async () => {
    const env = environment(),
      adapter = adapters(env),
      output = await adapter.createOutput({ manifest });
    await assert.rejects(adapter.validateArchive({ output, manifest }), /OUTPUT_SHORT_ARCHIVE/);
    await assert.rejects(output.write(new Uint8Array(1024)), /OUTPUT_BYTE_LIMIT/);
    await assert.rejects(adapter.validateArchive({ output: {}, manifest }), /OUTPUT_NOT_OWNED/);
    await assert.rejects(
      adapter.validateArchive({ output, manifest: [] }),
      /OUTPUT_MANIFEST_MISMATCH/,
    );
    await output.abort();
    assert.equal(env.destinationOpens, 0);
    assert.deepEqual([...env.files.keys()], ["unrelated"]);
  });
  test("corrupted stage bytes and post-validation mutations cannot commit", async () => {
    for (const after of [false, true]) {
      const env = environment(),
        adapter = adapters(env);
      const output = after ? await writeStage(adapter) : await adapter.createOutput({ manifest });
      if (after) {
        await adapter.validateArchive({ output, manifest });
        env.stageBytes[0] ^= 1;
        await assert.rejects(output.close());
        assert.ok(!env.events.includes("destination-close"));
      } else {
        await output.write(
          new Uint8Array(
            planStagedZip(manifest, {
              archiveFormat: "zip64",
              maxOutputBytes: 1024 * 1024,
            }).totalBytes,
          ),
        );
        await assert.rejects(adapter.validateArchive({ output, manifest }));
      }
      await output.abort();
      assert.deepEqual(env.committed, Uint8Array.of(7));
      assert.deepEqual([...env.files.keys()], ["unrelated"]);
    }
  });
  test("transfer/cleanup failures, cancellation and late cancellation", async () => {
    for (const kind of ["destinationWrite", "destinationClose", "remove", "cancel", "lateCancel"]) {
      const controller = new AbortController(),
        faults = { [kind]: true, quota: 0 };
      if (kind === "cancel") faults.onDestinationWrite = () => controller.abort();
      if (kind === "lateCancel") faults.onDestinationClose = () => controller.abort();
      const env = environment(faults),
        adapter = adapters(env),
        output = await writeStage(adapter, "zip64", controller.signal);
      await adapter.validateArchive({ output, manifest, signal: controller.signal });
      if (kind === "lateCancel") await output.close();
      else {
        await assert.rejects(output.close());
        assert.deepEqual(env.committed, Uint8Array.of(7));
        assert.ok(env.events.includes("destination-abort"));
      }
      faults.remove = false;
      await output.abort();
      assert.deepEqual([...env.files.keys()], ["unrelated"]);
    }
  });
  test("actual stage write/close failures still fail safely with a low quota estimate", async () => {
    for (const kind of ["stageWrite", "stageClose"]) {
      const env = environment({ [kind]: true, quota: 0 }),
        adapter = adapters(env);
      if (kind === "stageWrite") await assert.rejects(writeStage(adapter), /disk full/);
      else {
        const output = await writeStage(adapter);
        await assert.rejects(adapter.validateArchive({ output, manifest }), /close failed/);
        await output.abort();
      }
      assert.equal(env.destinationOpens, 0);
      assert.deepEqual([...env.files.keys()], ["unrelated"]);
    }
    const env = environment();
    await assert.rejects(adapters(env).createOutput({ manifest, signal: AbortSignal.abort() }));
    assert.deepEqual([...env.files.keys()], ["unrelated"]);
  });

  test("explicit abort during pending transfer cancels before commit", async () => {
    const faults = {},
      env = environment(faults),
      adapter = adapters(env);
    const output = await writeStage(adapter);
    await adapter.validateArchive({ output, manifest });
    let abortPromise;
    faults.onDestinationWrite = () => {
      abortPromise = output.abort();
    };
    await assert.rejects(output.close());
    await abortPromise;
    assert.deepEqual(env.committed, Uint8Array.of(7));
    assert.deepEqual([...env.files.keys()], ["unrelated"]);
    assert.ok(!env.events.includes("destination-close"));
  });

  test("oversized source chunks are rejected before staging write", async () => {
    const env = environment(),
      adapter = adapters(env),
      output = await adapter.createOutput({ manifest });
    await assert.rejects(output.write(new Uint8Array(4 * 1024 * 1024 + 1)), /OUTPUT_INVALID_CHUNK/);
    assert.ok(!env.events.includes("stage-write"));
    await output.abort();
  });

  test("failed writer aborts retain the stream for a later cleanup retry", async () => {
    for (const destination of [false, true]) {
      const kind = destination ? "destination" : "stage",
        faults = { [`${kind}Abort`]: true },
        env = environment(faults),
        adapter = adapters(env);
      const output = destination
        ? await writeStage(adapter)
        : await adapter.createOutput({ manifest });
      if (destination) {
        await adapter.validateArchive({ output, manifest });
        faults.destinationWrite = true;
        await assert.rejects(output.close(), /disk full/);
      }
      await assert.rejects(output.abort(), /OUTPUT_CLEANUP_FAILED/);
      if (!destination) {
        assert.equal(env.files.has(name), true, "An unterminated stage writer keeps its target");
        assert.equal(env.events.includes(`remove:${name}`), false);
      }
      const attempts = env.events.filter((event) => event === `${kind}-abort`).length;
      faults[`${kind}Abort`] = false;
      await output.abort();
      assert.equal(env.events.filter((event) => event === `${kind}-abort`).length, attempts + 1);
      await output.abort();
      assert.equal(env.events.filter((event) => event === `${kind}-abort`).length, attempts + 1);
      assert.deepEqual(env.committed, Uint8Array.of(7));
      assert.deepEqual([...env.files.keys()], ["unrelated"]);
    }
  });

  test("concurrent abort calls share cleanup instead of disposing a writer twice", async () => {
    let resume;
    const gate = new Promise((resolve) => {
      resume = resolve;
    });
    const env = environment({ onStageAbort: () => gate }),
      adapter = adapters(env),
      output = await adapter.createOutput({ manifest });
    const first = output.abort(),
      second = output.abort();
    resume();
    await Promise.all([first, second]);
    assert.equal(env.events.filter((event) => event === "stage-abort").length, 1);
    assert.equal(env.events.filter((event) => event === `remove:${name}`).length, 1);
    assert.deepEqual(env.committed, Uint8Array.of(7));
    assert.deepEqual([...env.files.keys()], ["unrelated"]);
  });

  test("setup cancellation preserves cleanup failure and does not delete a live stage", async () => {
    const controller = new AbortController();
    const faults = { stageAbort: true, onStageOpen: () => controller.abort() };
    const env = environment(faults);
    await assert.rejects(
      adapters(env).createOutput({ manifest, signal: controller.signal }),
      (error) => {
        assert.equal(error.message, "OUTPUT_SETUP_CLEANUP_FAILED");
        assert.equal(error.cause, controller.signal.reason);
        assert.equal(error.errors[0], controller.signal.reason);
        assert.equal(error.errors[1].message, "OUTPUT_CLEANUP_FAILED");
        return true;
      },
    );
    assert.equal(env.files.has(name), true);
    assert.equal(env.events.includes(`remove:${name}`), false);
    assert.equal(env.destinationOpens, 0);
  });

  // 只读查询不拥有文件；取消后不必等浏览器返回，但必须观察迟到的结果或拒绝。
  // Read-only queries own no files; cancellation stops waiting while late results/rejections remain observed.
  for (const phase of ["permission", "directory", "probe"])
    test(`cancellation during a pending ${phase} query does not wait or create files`, async () => {
      for (const lateFailure of [false, true]) {
        const controller = new AbortController(),
          reason = new Error("synthetic setup cancellation"),
          env = environment(),
          root = await env.storage.getDirectory();
        let started,
          finish,
          createCalls = 0,
          laterQueries = 0;
        const entered = new Promise((resolve) => {
          started = resolve;
        });
        const pending = new Promise((resolve, reject) => {
          finish = () =>
            lateFailure
              ? reject(new Error("late query failure"))
              : resolve(
                  {
                    permission: "granted",
                    directory: root,
                    probe: { kind: "file" },
                  }[phase],
                );
        });
        let waiting = false;
        const query =
          (name, read) =>
          (...args) => {
            if (waiting) laterQueries++;
            if (name !== phase) return read(...args);
            waiting = true;
            started();
            return pending;
          };
        env.destinationHandle.queryPermission = query("permission", () => "granted");
        env.storage.getDirectory = query("directory", () => root);
        const getFile = root.getFileHandle.bind(root),
          probe = query("probe", getFile);
        root.getFileHandle = (name, options) => {
          if (!options?.create) return probe(name, options);
          createCalls++;
          return getFile(name, options);
        };
        const opening = adapters(env).createOutput({ manifest, signal: controller.signal });
        const outcome = opening.then(
          () => "opened",
          (error) => error,
        );
        await entered;
        controller.abort(reason);
        await new Promise((resolve) => setImmediate(resolve));
        const sentinel = Symbol("waiting"),
          result = await Promise.race([outcome, Promise.resolve(sentinel)]);
        finish();
        await outcome;
        await new Promise((resolve) => setImmediate(resolve));
        assert.equal(result, reason);
        assert.equal(laterQueries, 0);
        assert.equal(createCalls, 0);
        assert.equal(env.destinationOpens, 0);
        assert.deepEqual([...env.files.keys()], ["unrelated"]);
      }
    });

  // 创建文件或写入器会获取资源所有权；取消必须先排空操作，再释放返回的资源。
  // Creating a file or writer acquires ownership; cancellation must drain first, then release returned resources.
  for (const phase of ["file", "writer"])
    test(`setup cancellation drains pending ${phase} creation before cleanup`, async () => {
      const env = environment(),
        controller = new AbortController(),
        root = await env.storage.getDirectory();
      let started, finish;
      const entered = new Promise((resolve) => {
          started = resolve;
        }),
        gate = new Promise((resolve) => {
          finish = resolve;
        });
      const getFile = root.getFileHandle.bind(root);
      root.getFileHandle = async (name, options) => {
        if (options?.create && phase === "file") {
          started();
          await gate;
        }
        const handle = await getFile(name, options);
        if (options?.create && phase === "writer") {
          const create = handle.createWritable.bind(handle);
          handle.createWritable = async (...args) => {
            started();
            await gate;
            return create(...args);
          };
        }
        return handle;
      };
      env.storage.getDirectory = async () => root;
      const outcome = adapters(env)
        .createOutput({ manifest, signal: controller.signal })
        .then(
          () => "opened",
          (error) => error,
        );
      await entered;
      controller.abort();
      await new Promise((resolve) => setImmediate(resolve));
      const waiting = Symbol("waiting");
      assert.equal(await Promise.race([outcome, Promise.resolve(waiting)]), waiting);
      assert.equal(env.events.includes(`remove:${name}`), false);
      finish();
      assert.equal(await outcome, controller.signal.reason);
      assert.equal(
        env.events.filter((event) => event === "stage-abort").length,
        phase === "writer" ? 1 : 0,
      );
      assert.equal(env.events.filter((event) => event === `remove:${name}`).length, 1);
      assert.equal(env.destinationOpens, 0);
      assert.deepEqual([...env.files.keys()], ["unrelated"]);
    });
}

// browser-task-yield
{
  // 使用受控调度器、端口和真实 Node 消息通道验证生命周期；不保证浏览器时序。
  // Controlled schedulers/ports and a real Node channel test lifetimes, not browser timing.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { getEventListeners } = nodeEventsModule;
  const { yieldBrowserTask } = browserTaskYieldModule;

  function replaceGlobal(t, name, value) {
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
    Object.defineProperty(globalThis, name, { configurable: true, writable: true, value });
    t.after(() => {
      if (descriptor) Object.defineProperty(globalThis, name, descriptor);
      else delete globalThis[name];
    });
  }
  function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => {
      resolve = yes;
      reject = no;
    });
    return { promise, resolve, reject };
  }
  function fakeChannels(t, { post, close, construct } = {}) {
    const channels = [];
    replaceGlobal(
      t,
      "MessageChannel",
      class {
        constructor() {
          const port = () => ({
            onmessage: null,
            onmessageerror: null,
            closes: 0,
            close() {
              this.closes++;
              close?.(this);
            },
            postMessage(value) {
              post?.(value);
            },
          });
          this.port1 = port();
          this.port2 = port();
          channels.push(this);
          construct?.();
        }
      },
    );
    return channels;
  }
  function assertClean(channel, signal) {
    assert.equal(getEventListeners(signal, "abort").length, 0);
    for (const port of [channel.port1, channel.port2]) {
      assert.equal(port.closes, 1);
      assert.equal(port.onmessage, null);
      assert.equal(port.onmessageerror, null);
    }
  }

  test("already aborted calls allocate no channel and never invoke the scheduler", async (t) => {
    const controller = new AbortController(),
      reason = new Error("cancelled first");
    controller.abort(reason);
    replaceGlobal(t, "scheduler", {
      yield() {
        assert.fail("scheduler must not run");
      },
    });
    const channels = fakeChannels(t);
    await assert.rejects(
      yieldBrowserTask({ signal: controller.signal }),
      (error) => error === reason,
    );
    assert.equal(channels.length, 0);
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
  });

  test("callable scheduler is preferred with its receiver and removes the abort listener", async (t) => {
    const ready = deferred(),
      controller = new AbortController();
    let calls = 0,
      finished = false;
    const scheduler = {
      yield() {
        assert.equal(this, scheduler);
        calls++;
        return ready.promise;
      },
    };
    replaceGlobal(t, "scheduler", scheduler);
    const channels = fakeChannels(t);
    const pending = yieldBrowserTask({ signal: controller.signal }).then(() => {
      finished = true;
    });
    await Promise.resolve();
    assert.equal(finished, false);
    assert.equal(calls, 1);
    assert.equal(channels.length, 0);
    assert.equal(getEventListeners(controller.signal, "abort").length, 1);
    ready.resolve();
    await pending;
    assert.equal(finished, true);
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
  });

  test("abort does not wait for the scheduler and its late rejection stays observed", async (t) => {
    const ready = deferred(),
      controller = new AbortController(),
      reason = new Error("cancelled");
    replaceGlobal(t, "scheduler", { yield: () => ready.promise });
    const pending = yieldBrowserTask({ signal: controller.signal });
    controller.abort(reason);
    await assert.rejects(pending, (error) => error === reason);
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    ready.reject(new Error("late scheduler rejection"));
    await Promise.resolve();
  });

  test("scheduler failures preserve their cause without a fallback or retained listener", async (t) => {
    const channels = fakeChannels(t),
      controller = new AbortController(),
      reason = new Error("scheduler failed");
    replaceGlobal(t, "scheduler", undefined);
    for (const yieldCall of [
      () => {
        throw reason;
      },
      () => Promise.reject(reason),
    ]) {
      globalThis.scheduler = { yield: yieldCall };
      await assert.rejects(
        yieldBrowserTask({ signal: controller.signal }),
        (error) => error === reason,
      );
      assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    }
    assert.equal(channels.length, 0);
  });

  test("abort after scheduler fulfillment but before async continuation still rejects", async (t) => {
    const ready = deferred(),
      controller = new AbortController(),
      reason = new Error("late abort");
    replaceGlobal(t, "scheduler", { yield: () => ready.promise });
    const pending = yieldBrowserTask({ signal: controller.signal });
    const aborting = ready.promise.then(() => controller.abort(reason));
    ready.resolve();
    await assert.rejects(pending, (error) => error === reason);
    await aborting;
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
  });

  test("missing or noncallable scheduler uses a fresh one-shot channel and closes both ports", async (t) => {
    const channels = fakeChannels(t),
      controller = new AbortController();
    replaceGlobal(t, "scheduler", undefined);
    for (const scheduler of [undefined, { yield: false }]) {
      globalThis.scheduler = scheduler;
      let done = false;
      const pending = yieldBrowserTask({ signal: controller.signal }).then(() => {
        done = true;
      });
      const channel = channels.at(-1),
        receive = channel.port1.onmessage;
      await Promise.resolve();
      assert.equal(done, false);
      receive({ data: null });
      await pending;
      assert.equal(done, true);
      assertClean(channel, controller.signal);
      receive({ data: null });
      assertClean(channel, controller.signal);
    }
    assert.equal(channels.length, 2);
  });

  test("channel cancellation rejects promptly and ignores already queued delivery", async (t) => {
    replaceGlobal(t, "scheduler", undefined);
    const channels = fakeChannels(t),
      controller = new AbortController();
    const pending = yieldBrowserTask({ signal: controller.signal }),
      channel = channels[0],
      receive = channel.port1.onmessage;
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    assertClean(channel, controller.signal);
    receive({ data: null });
    assertClean(channel, controller.signal);
  });

  test("channel post failure closes both ports even when one close throws", async (t) => {
    replaceGlobal(t, "scheduler", undefined);
    const reason = new Error("post failed"),
      controller = new AbortController();
    const channels = fakeChannels(t, {
      post() {
        throw reason;
      },
      close() {
        throw new Error("close failed");
      },
    });
    await assert.rejects(
      yieldBrowserTask({ signal: controller.signal }),
      (error) => error === reason,
    );
    assertClean(channels[0], controller.signal);
  });

  test("channel message errors release ports and listeners", async (t) => {
    replaceGlobal(t, "scheduler", undefined);
    const channels = fakeChannels(t),
      controller = new AbortController();
    const pending = yieldBrowserTask({ signal: controller.signal });
    channels[0].port1.onmessageerror();
    await assert.rejects(pending, /Task yield message failed/);
    assertClean(channels[0], controller.signal);
  });

  test("abort during channel construction cleans up the late acquired ports", async (t) => {
    replaceGlobal(t, "scheduler", undefined);
    const controller = new AbortController(),
      channels = fakeChannels(t, {
        construct() {
          controller.abort();
        },
        post() {
          assert.fail("must not post after abort");
        },
      });
    await assert.rejects(yieldBrowserTask({ signal: controller.signal }), { name: "AbortError" });
    assertClean(channels[0], controller.signal);
  });

  test("real Node MessageChannel yields without a timer or persistent channel", async (t) => {
    replaceGlobal(t, "scheduler", undefined);
    replaceGlobal(t, "setTimeout", () => {
      assert.fail("timer fallback is not allowed");
    });
    let resumed = false;
    const pending = yieldBrowserTask().then(() => {
      resumed = true;
    });
    await Promise.resolve();
    assert.equal(resumed, false);
    await pending;
    assert.equal(resumed, true);
  });
}

// asset-output-cache
{
  // 验证 asset-output-cache 的约定与边界；依据 ../asset-output-cache.mjs 及本文件的测试断言。
  // Verify asset-output-cache contracts and boundaries against ../asset-output-cache.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { runCachedAssetOperation } = assetOutputCacheModule;

  const context = Object.freeze({
    runtimeManifestSha256: "a".repeat(64),
    profileId: "synthetic-ios",
    artRevision: "synthetic-art-1",
    overlayRevision: "synthetic-overlay-1",
  });
  const encode = (value) => new TextEncoder().encode(value);
  function fixture() {
    const entries = new Map(),
      writes = [];
    let runs = 0,
      validations = 0;
    const cache = {
      async read({ kind, key, maxBytes }) {
        const bytes = entries.get(`${kind}/${key}`);
        return bytes && bytes.length <= maxBytes ? bytes.slice() : null;
      },
      async write({ kind, key, bytes }) {
        writes.push(kind);
        entries.set(`${kind}/${key}`, bytes.slice());
        return true;
      },
    };
    const options = {
      outputCache: cache,
      cacheContext: context,
      operation: "native-art",
      payload: encode("synthetic pixels and source bytes"),
      options: { metadataSha256: { manifest: "b".repeat(64) } },
      async run() {
        runs++;
        return { files: { "art.png": new Uint8Array([1, 2, 3]) }, assetCount: 1 };
      },
      validate(result) {
        validations++;
        assert.deepEqual(Object.keys(result.files), ["art.png"]);
        assert.deepEqual([...new Uint8Array(result.files["art.png"])], [1, 2, 3]);
      },
    };
    return { options, cache, entries, writes, runs: () => runs, validations: () => validations };
  }

  test("encoded asset hits preserve outputs and repeat the same validation", async () => {
    const f = fixture();
    const first = await runCachedAssetOperation(f.options);
    assert.deepEqual(f.writes, ["asset-file", "asset-index"]);
    first.files["art.png"].fill(0);
    const second = await runCachedAssetOperation(f.options);
    assert.deepEqual([...second.files["art.png"]], [1, 2, 3]);
    assert.equal(second.assetCount, 1);
    assert.equal(f.runs(), 1);
    assert.equal(f.validations(), 2);
    assert.equal(f.entries.size, 2);
    assert.ok(
      [...f.entries.values()].every(
        (bytes) => !new TextDecoder().decode(bytes).includes("synthetic pixels"),
      ),
    );
  });

  test("every source payload, option, profile, recipe and runtime identity changes the key", async () => {
    for (const changed of [
      { payload: encode("different source bytes") },
      { options: { metadataSha256: { manifest: "c".repeat(64) } } },
      { cacheContext: { ...context, profileId: "other-profile" } },
      { cacheContext: { ...context, artRevision: "other-art" } },
      { cacheContext: { ...context, overlayRevision: "other-overlay" } },
      { cacheContext: { ...context, runtimeManifestSha256: "d".repeat(64) } },
    ]) {
      const f = fixture();
      await runCachedAssetOperation(f.options);
      await runCachedAssetOperation({ ...f.options, ...changed });
      assert.equal(f.runs(), 2);
    }
  });

  test("unavailable, denied or incomplete caches fall back to normal generation", async () => {
    for (const setup of [
      (f) => {
        f.options.outputCache = undefined;
      },
      (f) => {
        f.options.cacheContext = undefined;
      },
      (f) => {
        f.cache.read = async () => {
          throw new Error("denied");
        };
      },
      (f) => {
        f.cache.write = async () => {
          throw new Error("quota");
        };
      },
    ]) {
      const f = fixture();
      setup(f);
      await runCachedAssetOperation(f.options);
      assert.equal(f.runs(), 1);
    }
    const f = fixture();
    await runCachedAssetOperation(f.options);
    for (const key of f.entries.keys()) if (key.startsWith("asset-file/")) f.entries.delete(key);
    await runCachedAssetOperation(f.options);
    assert.equal(f.runs(), 2);
  });

  test("malformed metadata or cached outputs rejected by the generator are misses", async () => {
    for (const kind of ["asset-index", "asset-file"]) {
      const f = fixture();
      await runCachedAssetOperation(f.options);
      for (const [key, bytes] of f.entries) if (key.startsWith(`${kind}/`)) bytes[0] = 42;
      await runCachedAssetOperation(f.options);
      assert.equal(f.runs(), 2);
    }
  });

  test("rejected fresh output never publishes a reusable cache index", async () => {
    const f = fixture();
    f.options.validate = () => {
      throw new Error("rejected output");
    };
    await assert.rejects(runCachedAssetOperation(f.options), /rejected output/);
    assert.deepEqual(f.writes, []);
  });

  test("a partial file write does not publish the complete result", async () => {
    const f = fixture();
    f.options.run = async () => ({
      files: { "art.png": new Uint8Array([1, 2, 3]), "extra.png": new Uint8Array([4]) },
    });
    f.options.validate = () => {};
    let attempted = 0;
    const write = f.cache.write;
    f.cache.write = async (request) => (++attempted === 2 ? false : write(request));
    await runCachedAssetOperation(f.options);
    assert.deepEqual(f.writes, ["asset-file"]);
    assert.ok([...f.entries.keys()].every((key) => !key.startsWith("asset-index/")));
  });

  test("a failed refresh cannot replace files referenced by the earlier complete index", async () => {
    const f = fixture();
    let value = 1;
    f.options.run = async () => ({
      files: {
        "one.png": new Uint8Array([value]),
        "two.png": new Uint8Array([value]),
      },
    });
    f.options.validate = () => {};
    await runCachedAssetOperation(f.options);
    const read = f.cache.read,
      write = f.cache.write;
    let writes = 0;
    value = 2;
    f.cache.read = async () => null;
    f.cache.write = async (request) => (++writes === 2 ? false : write(request));
    await runCachedAssetOperation(f.options);
    f.cache.read = read;
    f.cache.write = write;
    const retained = await runCachedAssetOperation(f.options);
    assert.deepEqual([...retained.files["one.png"]], [1]);
    assert.deepEqual([...retained.files["two.png"]], [1]);
  });

  test("cache read cancellation wipes returned snapshots without falling through to compute", async () => {
    const f = fixture();
    await runCachedAssetOperation(f.options);
    const controller = new AbortController(),
      returned = [];
    const read = f.cache.read;
    f.cache.read = async (request) => {
      const bytes = await read(request);
      returned.push(bytes);
      if (request.kind === "asset-file") controller.abort();
      return bytes;
    };
    await assert.rejects(runCachedAssetOperation({ ...f.options, signal: controller.signal }), {
      name: "AbortError",
    });
    assert.equal(f.runs(), 1);
    assert.ok(returned.every((bytes) => bytes.every((byte) => byte === 0)));
  });

  test("cancellation after fresh generation wipes results and prevents cache writes", async () => {
    const f = fixture(),
      controller = new AbortController();
    const bytes = new Uint8Array([1, 2, 3]);
    f.options.run = async () => {
      controller.abort();
      return { files: { "art.png": bytes } };
    };
    await assert.rejects(runCachedAssetOperation({ ...f.options, signal: controller.signal }), {
      name: "AbortError",
    });
    assert.deepEqual([...bytes], [0, 0, 0]);
    assert.deepEqual(f.writes, []);
  });

  test("per-file and aggregate cache budgets never enlarge normal output acceptance", async () => {
    for (const limits of [{ maxFileBytes: 2 }, { maxTotalBytes: 2 }]) {
      const f = fixture();
      await runCachedAssetOperation({ ...f.options, ...limits });
      assert.equal(f.runs(), 1);
      assert.deepEqual(f.writes, []);
    }
  });
}

// committed-download-recovery
{
  // 合成产物证明提交后清理失败可恢复下载，不构成真实浏览器证明。
  // Synthetic artifacts prove post-commit cleanup recovery, not real-browser behavior.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { withLegacyAstcProducerLease, takeCommittedDownload } = browserPackagerModule;
  const { PackagingJobError } = packagingJobModule;
  const { createDownloadPreparation } = downloadPreparationModule;
  const { committedCleanupWarning } = packagingDiagnosticsModule;

  test("committed Blob survives cleanup error privately and URL retry never regenerates", async () => {
    const blob = new Blob(["synthetic IPA"]);
    const cleanup = new Error("private cleanup details");
    let conversions = 0,
      attempts = 0;
    let failure;
    try {
      await withLegacyAstcProducerLease(
        {
          close() {
            throw cleanup;
          },
        },
        async () => {
          conversions++;
          return { committed: true };
        },
        () => ({ blob, fileName: "unsigned.ipa" }),
      );
    } catch (error) {
      failure = error;
    }
    assert.ok(failure instanceof PackagingJobError);
    assert.equal(failure.cause, cleanup);
    assert.deepEqual(failure.cleanupErrors, [cleanup]);
    assert.equal(failure.report.committed, true);
    assert.doesNotMatch(JSON.stringify(failure), /blob|unsigned\.ipa|synthetic IPA/);
    const recovery = takeCommittedDownload(failure);
    assert.equal(recovery.blob, blob);
    assert.equal(takeCommittedDownload(failure), undefined);
    const preparation = createDownloadPreparation({
      createObjectURL(value) {
        assert.equal(value, blob);
        if (++attempts === 1) throw new Error("URL failed");
        return "blob:ready";
      },
      revokeObjectURL() {},
    });
    preparation.retain(recovery);
    assert.equal(preparation.prepare().status, "retryable");
    assert.equal(preparation.prepare().status, "ready");
    assert.equal(conversions, 1);
    assert.match(committedCleanupWarning(), /cleanup failed.*Save the IPA, then reload/);
    assert.doesNotMatch(committedCleanupWarning(), /No completed IPA/);
  });

  test("forged reports and precommit failures never authorize retrieval", async () => {
    const forged = new PackagingJobError(new Error("validation"), { committed: true });
    let reads = 0;
    assert.equal(takeCommittedDownload(forged), undefined);
    for (const operation of [
      async () => {
        throw forged;
      },
      async () => ({ committed: false }),
    ]) {
      await assert.rejects(
        withLegacyAstcProducerLease(
          {
            close() {
              throw new Error("cleanup");
            },
          },
          operation,
          () => {
            reads++;
            return { blob: new Blob(), fileName: "x" };
          },
        ),
        (error) => {
          assert.equal(takeCommittedDownload(error), undefined);
          return true;
        },
      );
    }
    assert.equal(reads, 0);
  });

  test("unavailable committed output preserves original cleanup cause without recovery", async () => {
    const cleanup = new Error("cleanup");
    await assert.rejects(
      withLegacyAstcProducerLease(
        {
          close() {
            throw cleanup;
          },
        },
        async () => ({ committed: true }),
        () => {
          throw new Error("not available");
        },
      ),
      (error) => {
        assert.equal(error.cause, cleanup);
        assert.equal(takeCommittedDownload(error), undefined);
        return true;
      },
    );
  });

  test("UI uses opaque recovery before generic failure wording and preserves export outcomes", async () => {
    const source = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const recovery = source.slice(
      source.indexOf("const recovered = takeCommittedDownload(error)"),
      source.indexOf("appendActivity(packagingFailureSummary(report)"),
    );
    assert.match(recovery, /retain\(recovered\)/);
    assert.match(recovery, /committedCleanupWarning\(\)/);
    assert.match(recovery, /dmWebResourcesSaved/);
    assert.match(recovery, /prepareDownload\(!originalIdentity && !jobOptions.signing\)/);
    assert.doesNotMatch(recovery, /\.generate\(/);
  });

  test("packager binds its own sink and transfers recovery when preserving export reports", async () => {
    const source = await readFile(new URL("../browser-packager.mjs", import.meta.url), "utf8");
    assert.match(source, /\(\) => output\.getDownload\(\)/);
    assert.match(
      source,
      /const recovery = takeCommittedDownload\(error\);\s*if \(recovery\) committedDownloads\.set\(reported, recovery\);\s*throw reported;/,
    );
    const { catalog } = await import("../i18n-catalog.mjs");
    assert.match(catalog.dmWebCommittedCleanupWarning[1], /清理失败.*先保存 IPA.*重新载入/);
  });
}

// local-output-cache
{
  // 仅使用合成存储，不执行浏览器、私有输入、编译器或 WASM。
  // Synthetic storage only: no browser, private input, compiler, or WASM execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { cacheKeyFor, createLocalOutputCache } = localOutputCacheModule;
  const missing = () => new DOMException("Missing", "NotFoundError");
  const tick = () => new Promise((resolve) => setImmediate(resolve));
  function deferred() {
    let resolve;
    const promise = new Promise((done) => {
      resolve = done;
    });
    return { promise, resolve };
  }
  class Directory {
    constructor(state) {
      this.state = state;
      this.entries = new Map();
      this.kind = "directory";
    }
    async getDirectoryHandle(name, { create = false } = {}) {
      if (!this.entries.has(name)) {
        if (!create) throw missing();
        this.entries.set(name, new Directory(this.state));
      }
      const result = this.entries.get(name);
      if (result.kind !== "directory") throw new DOMException("Wrong kind", "TypeMismatchError");
      return result;
    }
    async getFileHandle(name, { create = false } = {}) {
      if (!this.entries.has(name)) {
        if (!create) throw missing();
        this.entries.set(name, new LocalFile(this.state, name));
      }
      const result = this.entries.get(name);
      if (this.state.getHandle) await this.state.getHandle({ name, create, handle: result });
      return result;
    }
    async removeEntry(name) {
      if (!this.entries.delete(name)) throw missing();
    }
  }
  class LocalFile {
    constructor(state, name) {
      this.state = state;
      this.name = name;
      this.bytes = new Uint8Array();
      this.kind = "file";
    }
    async isSameEntry(value) {
      return this === value;
    }
    async getFile() {
      const owner = this;
      return {
        size: owner.advertisedSize ?? owner.bytes.length,
        async text() {
          return new TextDecoder().decode(owner.bytes);
        },
        async arrayBuffer() {
          owner.state.reads++;
          const copy = owner.bytes.slice().buffer;
          if (owner.state.readBuffer) await owner.state.readBuffer(copy);
          return copy;
        },
      };
    }
    async createWritable() {
      const owner = this,
        chunks = [];
      let aborted = false;
      return {
        async write(bytes) {
          if (owner.state.writeFailure) throw new Error("QuotaExceeded");
          chunks.push(new Uint8Array(bytes));
        },
        async close() {
          if (owner.state.closeWait) await owner.state.closeWait();
          if (aborted) throw new Error("Aborted");
          owner.bytes = new Uint8Array(chunks.reduce((n, b) => n + b.length, 0));
          let at = 0;
          for (const bytes of chunks) {
            owner.bytes.set(bytes, at);
            at += bytes.length;
          }
        },
        async abort() {
          aborted = true;
          owner.state.aborts++;
        },
      };
    }
  }
  function fixture() {
    const state = { reads: 0, aborts: 0 },
      root = new Directory(state),
      caches = new Map();
    let locked = false;
    const environment = {
      location: { href: "https://packager.test/app/" },
      crypto,
      navigator: {
        locks: {
          async request(_name, options, callback) {
            assert.equal(options.ifAvailable, true);
            if (locked) return callback(null);
            locked = true;
            try {
              return await callback({});
            } finally {
              locked = false;
            }
          },
        },
      },
      caches: {
        async open(name) {
          if (!caches.has(name)) {
            const entries = new Map();
            caches.set(name, {
              entries,
              async keys() {
                return [...entries.keys()].map((url) => ({ url }));
              },
              async put(url, response) {
                if (state.putFailure) throw new Error("Storage failed");
                if (state.putWait) await state.putWait();
                entries.set(url, response);
              },
              async delete(url) {
                return entries.delete(url);
              },
            });
          }
          return caches.get(name);
        },
      },
    };
    const options = { directory: root, environment };
    const shaderFolder = () =>
      root.entries.get("D2R-cache").entries.get("v1").entries.get("shader");
    return { state, root, caches, environment, options, shaderFolder };
  }
  const key = "a".repeat(64);
  const value = new Uint8Array([1, 2, 3, 4]);
  const read = (cache, extra = {}) => cache.read({ kind: "shader", key, maxBytes: 16, ...extra });
  const write = (cache, extra = {}) => cache.write({ kind: "shader", key, bytes: value, ...extra });

  test("visible cache starts with a fresh ledger and leaves older hidden caches untouched", async () => {
    const f = fixture();
    const previous = await createLocalOutputCache(f.options);
    assert.equal(await write(previous), true);
    previous.close();
    await tick();
    const hidden = f.root.entries.get("D2R-cache");
    const oldMarker = hidden.entries.get("v1").entries.get("cache.json").bytes.slice();
    const oldLedger = [...f.caches.values()][0];
    const oldEntries = [...oldLedger.entries];
    f.root.entries.set(".d2r-cache", hidden);
    f.root.entries.delete("D2R-cache");
    const getDirectory = f.root.getDirectoryHandle.bind(f.root);
    f.root.getDirectoryHandle = (name, options) => {
      assert.notEqual(name, ".d2r-cache", "never probe or migrate the old hidden folder");
      return getDirectory(name, options);
    };
    const cache = await createLocalOutputCache(f.options);
    assert.equal(await read(cache), null);
    assert.equal(f.caches.size, 2);
    assert.notEqual(f.root.entries.get("D2R-cache"), hidden);
    assert.equal(await write(cache), true);
    assert.deepEqual(await read(cache), value);
    assert.equal(f.root.entries.get(".d2r-cache"), hidden);
    assert.deepEqual(hidden.entries.get("v1").entries.get("cache.json").bytes, oldMarker);
    assert.deepEqual([...oldLedger.entries], oldEntries);
    cache.close();
  });

  test("canonical key order, complete inputs, and bounded metadata", async () => {
    assert.equal(
      await cacheKeyFor({ b: [1, true], a: "source" }),
      await cacheKeyFor({ a: "source", b: [1, true] }),
    );
    assert.notEqual(await cacheKeyFor({ source: "a" }), await cacheKeyFor({ source: "b" }));
    await assert.rejects(cacheKeyFor({ value: undefined }));
    await assert.rejects(cacheKeyFor(new Array(2)));
    await assert.rejects(cacheKeyFor(Array(8).fill("x".repeat(1024 * 1024))), /LIMIT/);
    await assert.rejects(cacheKeyFor({ ["k".repeat(1024 * 1024 + 1)]: true }), /LIMIT/);
  });
  test("cold miss, accepted write, warm independently checked owned snapshot", async () => {
    const f = fixture();
    let cache = await createLocalOutputCache(f.options);
    assert.equal(await read(cache), null);
    assert.equal(await write(cache), true);
    const first = await read(cache);
    assert.deepEqual(first, value);
    first.fill(0);
    cache.close();
    await tick();
    cache = await createLocalOutputCache(f.options);
    assert.deepEqual(await read(cache), value);
    assert.equal([...f.caches.values()][0].entries.size, 1);
    cache.close();
  });
  test("folder bytes and neighboring receipts cannot enroll themselves in missing ledger", async () => {
    const f = fixture();
    let cache = await createLocalOutputCache(f.options);
    await write(cache);
    cache.close();
    await tick();
    f.caches.clear();
    const forged = await f.shaderFolder().getFileHandle("receipt.json", { create: true });
    forged.bytes = new TextEncoder().encode(JSON.stringify({ outputSha256: "b".repeat(64) }));
    cache = await createLocalOutputCache(f.options);
    assert.equal(await read(cache), null);
    cache.close();
  });
  test("tampering and oversized advertised file are misses, never unbounded reads", async () => {
    const f = fixture(),
      cache = await createLocalOutputCache(f.options);
    await write(cache);
    const file = [...f.shaderFolder().entries.values()][0];
    file.bytes = new Uint8Array([9, 9, 9, 9]);
    assert.equal(await read(cache), null);
    const before = f.state.reads;
    file.advertisedSize = 10 ** 9;
    assert.equal(await read(cache), null);
    assert.equal(f.state.reads, before);
    cache.close();
  });
  test("ignore bypasses reuse, fresh outputs can refresh, mutated old leaf preserved", async () => {
    const f = fixture();
    let cache = await createLocalOutputCache(f.options);
    await write(cache);
    cache.close();
    await tick();
    const old = [...f.shaderFolder().entries.values()][0];
    old.bytes = new Uint8Array([8, 8, 8, 8]);
    cache = await createLocalOutputCache({ ...f.options, ignoreCache: true });
    const reads = f.state.reads;
    assert.equal(await read(cache), null);
    assert.equal(f.state.reads, reads);
    assert.equal(await write(cache, { bytes: new Uint8Array([5]) }), true);
    assert.ok([...f.shaderFolder().entries.values()].includes(old));
    cache.close();
    await tick();
    cache = await createLocalOutputCache(f.options);
    assert.deepEqual(await read(cache), new Uint8Array([5]));
    cache.close();
  });
  test("readwrite failure warns once, removes owned incomplete file, and stays optional", async () => {
    const f = fixture(),
      warnings = [];
    const cache = await createLocalOutputCache({
      ...f.options,
      onWarning: (event) => warnings.push(event),
    });
    f.state.writeFailure = true;
    assert.equal(await write(cache), false);
    assert.equal(await write(cache), false);
    assert.equal(await read(cache), null);
    assert.equal(f.shaderFolder().entries.size, 0);
    assert.deepEqual(warnings, [{ phase: "cache", status: "unavailable" }]);
    assert.equal([...f.caches.values()][0].entries.size, 0);
    cache.close();
  });
  test("ledger completion failure cannot authorize or leave the incomplete output", async () => {
    const f = fixture(),
      cache = await createLocalOutputCache(f.options);
    f.state.putFailure = true;
    assert.equal(await write(cache), false);
    assert.equal(f.shaderFolder().entries.size, 0);
    assert.equal([...f.caches.values()][0].entries.size, 0);
    cache.close();
  });
  test("origin lock conflict does not wait or mutate another generation's cache", async () => {
    const f = fixture(),
      first = await createLocalOutputCache(f.options);
    const second = await createLocalOutputCache(f.options);
    assert.equal(await write(second), false);
    assert.equal(await write(first), true);
    second.close();
    first.close();
    await tick();
    const third = await createLocalOutputCache(f.options);
    assert.deepEqual(await read(third), value);
    third.close();
  });
  test("one instance never runs two cache writes concurrently", async () => {
    const f = fixture(),
      cache = await createLocalOutputCache(f.options);
    const results = await Promise.all([write(cache), write(cache, { key: "b".repeat(64) })]);
    assert.deepEqual(results, [true, false]);
    assert.equal([...f.caches.values()][0].entries.size, 1);
    cache.close();
  });
  test("cancelled late cache read promptly rejects and wipes its late snapshot", async () => {
    const f = fixture(),
      cache = await createLocalOutputCache(f.options);
    await write(cache);
    const wait = deferred(),
      entered = deferred(),
      controller = new AbortController();
    let late;
    f.state.readBuffer = async (bytes) => {
      late = bytes;
      entered.resolve();
      await wait.promise;
    };
    const pending = read(cache, { signal: controller.signal });
    await entered.promise;
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    wait.resolve();
    await tick();
    assert.deepEqual(new Uint8Array(late), new Uint8Array(value.length));
    cache.close();
  });
  test("late created output handles are cleaned after cancellation, never committed", async () => {
    const f = fixture(),
      cache = await createLocalOutputCache(f.options);
    const wait = deferred(),
      entered = deferred(),
      controller = new AbortController();
    f.state.getHandle = async ({ name, create }) => {
      if (create && name.endsWith(".bin")) {
        entered.resolve();
        await wait.promise;
      }
    };
    const pending = write(cache, { signal: controller.signal });
    await entered.promise;
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    wait.resolve();
    await tick();
    await tick();
    assert.equal(f.shaderFolder().entries.size, 0);
    assert.equal([...f.caches.values()][0].entries.size, 0);
    cache.close();
  });
  test("late empty marker creation is cleaned and a subsequent attempt remains usable", async () => {
    const f = fixture(),
      wait = deferred(),
      entered = deferred(),
      controller = new AbortController();
    f.state.getHandle = async ({ name, create }) => {
      if (create && name === "cache.json") {
        entered.resolve();
        await wait.promise;
      }
    };
    const pending = createLocalOutputCache({ ...f.options, signal: controller.signal });
    await entered.promise;
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    wait.resolve();
    await tick();
    await tick();
    f.state.getHandle = undefined;
    const cache = await createLocalOutputCache(f.options);
    assert.equal(await write(cache), true);
    cache.close();
  });
  test("hung optional storage times out and abandoned completion is not reused", async () => {
    const f = fixture(),
      cache = await createLocalOutputCache({ ...f.options, timeoutMs: 100 });
    const wait = deferred();
    f.state.putWait = () => wait.promise;
    assert.equal(await write(cache), false);
    assert.equal(f.shaderFolder().entries.size, 0);
    wait.resolve();
    await tick();
    await tick();
    assert.equal([...f.caches.values()][0].entries.size, 0);
    assert.equal(await read(cache), null);
    cache.close();
  });
  test("unavailable browser storage and malformed foreign marker never block or get deleted", async () => {
    const f = fixture();
    const root = await f.root.getDirectoryHandle("D2R-cache", { create: true });
    const folder = await root.getDirectoryHandle("v1", { create: true });
    const marker = await folder.getFileHandle("cache.json", { create: true });
    marker.bytes = new TextEncoder().encode("not our marker");
    const cache = await createLocalOutputCache(f.options);
    assert.equal(await write(cache), false);
    assert.equal(new TextDecoder().decode(marker.bytes), "not our marker");
    cache.close();
  });

  test("close cancels a pending ledger commit and holds lock through its late outcome", async () => {
    const f = fixture(),
      cache = await createLocalOutputCache(f.options);
    const wait = deferred(),
      entered = deferred();
    f.state.putWait = () => {
      entered.resolve();
      return wait.promise;
    };
    const pending = write(cache);
    await entered.promise;
    cache.close();
    assert.equal(await pending, false);
    const second = await createLocalOutputCache(f.options);
    assert.equal(await write(second), false);
    second.close();
    wait.resolve();
    await tick();
    await tick();
    f.state.putWait = undefined;
    const third = await createLocalOutputCache(f.options);
    assert.equal(await read(third), null);
    assert.equal(await write(third), true);
    third.close();
  });
  test("closed cache cannot start later leaf creation after a suspended directory lookup", async () => {
    const f = fixture(),
      cache = await createLocalOutputCache(f.options);
    const folder = f.root.entries.get("D2R-cache").entries.get("v1");
    const original = folder.getDirectoryHandle.bind(folder),
      wait = deferred(),
      entered = deferred();
    folder.getDirectoryHandle = async (...args) => {
      entered.resolve();
      await wait.promise;
      return original(...args);
    };
    const pending = write(cache);
    await entered.promise;
    cache.close();
    assert.equal(await pending, false);
    wait.resolve();
    await tick();
    await tick();
    assert.equal(f.shaderFolder().entries.size, 0);
  });
}

// opfs-storage-advisory
{
  // 只使用存储估算替身；不访问真实存储或改变权限。
  // Storage-estimate doubles only; no real storage access or permission changes.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { readOpfsStorageAdvisory } = opfsStorageAdvisoryModule;
  const storage = (value) => ({ estimate: async () => value });
  test("reports origin estimate and compares only explicit known staging requirements", async () => {
    const options = { storage: storage({ quota: 1000, usage: 300 }) };
    const unknown = await readOpfsStorageAdvisory(options);
    assert.deepEqual(unknown, {
      status: "estimated",
      quotaBytes: 1000,
      usageBytes: 300,
      availableBytes: 700,
      requiredBytes: null,
      sufficiency: "unknown",
      advisoryOnly: true,
    });
    assert.ok(Object.isFrozen(unknown));
    assert.equal(
      (await readOpfsStorageAdvisory({ ...options, requiredBytes: 700 })).sufficiency,
      "estimated-sufficient",
    );
    assert.equal(
      (await readOpfsStorageAdvisory({ ...options, requiredBytes: 701 })).sufficiency,
      "estimated-insufficient",
    );
    assert.equal(
      (await readOpfsStorageAdvisory({ ...options, requiredBytes: 0 })).sufficiency,
      "estimated-sufficient",
    );
    const over = await readOpfsStorageAdvisory({
      storage: storage({ quota: 10, usage: 12 }),
      requiredBytes: 1,
    });
    assert.equal(over.availableBytes, 0);
    assert.equal(over.sufficiency, "estimated-insufficient");
  });
  test("missing, throwing or rejected APIs are advisory unavailable, never a normal-mode gate", async () => {
    for (const candidate of [
      null,
      {},
      { estimate: 1 },
      {
        get estimate() {
          throw new Error("unavailable");
        },
      },
      {
        estimate() {
          throw new Error("unavailable");
        },
      },
      {
        estimate() {
          return Promise.reject(new Error("unavailable"));
        },
      },
    ]) {
      const result = await readOpfsStorageAdvisory({ storage: candidate, requiredBytes: 100 });
      assert.equal(result.status, "unavailable");
      assert.equal(result.availableBytes, null);
      assert.equal(result.sufficiency, "unknown");
    }
  });
  test("missing and unsafe numeric estimates stay unknown without coercion", async () => {
    for (const value of [
      null,
      {},
      { quota: 100 },
      { quota: 100, usage: undefined },
      ...[-1, NaN, Infinity, 1.5, Number.MAX_SAFE_INTEGER + 1, "100", 100n].flatMap((n) => [
        { quota: n, usage: 0 },
        { quota: 100, usage: n },
      ]),
      {
        get quota() {
          throw new Error("bad getter");
        },
        usage: 0,
      },
    ]) {
      const result = await readOpfsStorageAdvisory({ storage: storage(value) });
      assert.equal(result.status, "invalid");
      assert.equal(result.availableBytes, null);
      assert.equal(result.sufficiency, "unknown");
    }
  });
  test("timeout returns promptly and late rejected estimates remain observed", async () => {
    let reject;
    const result = await readOpfsStorageAdvisory({
      storage: {
        estimate: () =>
          new Promise((_, r) => {
            reject = r;
          }),
      },
      timeoutMs: 5,
    });
    assert.equal(result.status, "timeout");
    assert.equal(result.availableBytes, null);
    reject(new Error("late failure"));
    await new Promise((resolve) => setTimeout(resolve, 0));
  });
  test("pre-cancel avoids estimation; pending cancellation returns unknown without hanging", async () => {
    const controller = new AbortController();
    controller.abort();
    let calls = 0;
    assert.equal(
      (
        await readOpfsStorageAdvisory({
          storage: {
            estimate() {
              calls++;
            },
          },
          signal: controller.signal,
        })
      ).status,
      "cancelled",
    );
    assert.equal(calls, 0);
    const active = new AbortController(),
      pending = readOpfsStorageAdvisory({
        storage: { estimate: () => new Promise(() => {}) },
        signal: active.signal,
      });
    await Promise.resolve();
    active.abort();
    assert.equal((await pending).status, "cancelled");
  });
  test("only estimate is invoked and option limits reject configuration mistakes", async () => {
    const candidate = {
      estimate() {
        assert.equal(this, candidate);
        return { quota: 100, usage: 0 };
      },
      persist() {
        assert.fail("must not request persistence");
      },
      getDirectory() {
        assert.fail("must not inspect files");
      },
    };
    await readOpfsStorageAdvisory({ storage: candidate });
    for (const options of [
      { requiredBytes: -1 },
      { requiredBytes: Infinity },
      { requiredBytes: 1.1 },
      { timeoutMs: 0 },
      { timeoutMs: 30001 },
      { signal: {} },
    ])
      await assert.rejects(readOpfsStorageAdvisory({ storage: candidate, ...options }), /options/);
  });
}

// shader-output-cache
{
  // 只使用模块模拟及合成字节，不执行编译器或私有输入。
  // Only module mocks and synthetic bytes. No compiler or private inputs execute.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;
  test("production shader reuse preserves identity, validation, source proof and cancellation", () => {
    const result = spawnSync(
      process.execPath,
      ["--experimental-test-module-mocks", "tests/fixtures/shader-lifecycle.mock.mjs"],
      {
        env: { ...process.env, D2R_TEST_SCENARIO: "shader-output-cache" },
        cwd: new URL("../", import.meta.url),
        encoding: "utf8",
        timeout: 60000,
      },
    );
    assert.equal(result.status, 0, result.stderr || result.stdout);
  });

  test("bounded production shader concurrency preserves ownership, cache and deterministic proof-first output", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/shader-lifecycle.mock.mjs", import.meta.url).pathname,
      ],
      {
        env: { ...env, D2R_TEST_SCENARIO: "production-shader-concurrency" },
        encoding: "utf8",
        timeout: 30000,
        maxBuffer: 1024 * 1024,
      },
    );
    assertIsolatedRun(result);
  });
}
