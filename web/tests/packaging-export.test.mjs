// Feature suite: packaging-export. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as typescriptModule from "typescript";
import * as packagingDiagnosticsModule from "../packaging-diagnostics.mjs";
import * as packagingJobModule from "../packaging-job.mjs";
import * as contentSelectionModule from "../content-selection.mjs";
import * as d2rExpansionPolicyModule from "../d2r-expansion-policy.mjs";
import * as promisesModule from "node:fs/promises";
import * as downloadPreparationModule from "../download-preparation.mjs";
import * as resourceFolderExportModule from "../resource-folder-export.mjs";
import * as legacyDevicePackExportModule from "../legacy-device-pack-export.mjs";
import * as resourceWriteBufferModule from "../resource-write-buffer.mjs";
import * as nodeCryptoModule from "node:crypto";
import * as syntheticExportModule from "../synthetic-export.mjs";
import * as guestFrameworkModule from "../guest-framework.mjs";
import * as rawFileStageModule from "../raw-file-stage.mjs";
import * as resourceDataReuseModule from "../resource-data-reuse.mjs";
import * as resourceOutputLockModule from "../resource-output-lock.mjs";
import * as nodeChild_processModule from "node:child_process";
import * as nodeFsModule from "node:fs";
import * as nodeOsModule from "node:os";
import * as nodePathModule from "node:path";
import * as nodeUrlModule from "node:url";
import * as guestTargetStreamModule from "../guest-target-stream.mjs";
import * as shellProfilesModule from "../shell-profiles.mjs";
import * as previewManifestModule from "../verification/preview-manifest.mjs";

const resourceOutputLocksModule = (() => {
  function resourceOutputLocks() {
    let active = false;
    return {
      async request(_name, _options, callback) {
        if (active) return callback(null);
        active = true;
        try {
          return await callback({});
        } finally {
          active = false;
        }
      },
    };
  }
  return { resourceOutputLocks };
})();

// packaging-diagnostics
{
  // 浏览器诊断契约：验证固定标签、隐私边界及有界资源计数。
  // Browser diagnostic contracts: verify fixed labels, privacy boundaries and bounded resource counts.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { default: ts } = typescriptModule;
  const {
    packagingActivity,
    shaderProgressLabel,
    sanitizeShaderRecordProgress,
    packagingFailureMessage,
    packagingFailureSummary,
    shaderFailureDetails,
    sanitizeResourceOutcome,
    sanitizeResourceProgress,
    resourceProgressLabel,
  } = packagingDiagnosticsModule;

  test("resource status projects safe counters without flooding activity logs", () => {
    const resourceProgress = sanitizeResourceProgress({
      filesWritten: 2,
      totalFiles: 3,
      bytesWritten: 4500000000n,
      totalBytes: 9000000000n,
      directoryName: "PRIVATE-PATH",
      private: "PRIVATE-KEY",
    });
    assert.deepEqual(resourceProgress, {
      filesWritten: 2,
      totalFiles: 3,
      bytesWritten: 4500000000,
      totalBytes: 9000000000,
    });
    assert.equal(Object.isFrozen(resourceProgress), true);
    assert.doesNotMatch(JSON.stringify(resourceProgress), /PRIVATE/);
    const event = { stage: "resources", state: "running", resourceProgress };
    assert.equal(
      resourceProgressLabel(event),
      "Saving game resources: 2/3 files; 4500.0/9000.0 MB",
    );
    assert.equal(
      packagingActivity(event),
      packagingActivity({ ...event, resourceProgress: undefined }),
    );
    assert.equal(resourceProgressLabel({ ...event, state: "complete" }), null);
    assert.equal(resourceProgressLabel({ ...event, state: "failed" }), null);
    assert.equal(resourceProgressLabel({ ...event, stage: "archive" }), null);
    assert.equal(resourceProgressLabel({ ...event, resourceProgress: undefined }), null);
    assert.notEqual(packagingActivity({ ...event, state: "complete" }), packagingActivity(event));
  });

  test("resource counters reject unsafe values and inconsistent totals", () => {
    const valid = { filesWritten: 0, totalFiles: 1, bytesWritten: 0, totalBytes: 10 };
    for (const field of Object.keys(valid)) {
      for (const count of [-1, 0.5, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1, "1", null]) {
        assert.equal(sanitizeResourceProgress({ ...valid, [field]: count }), undefined);
      }
    }
    for (const bytesWritten of [-1n, BigInt(Number.MAX_SAFE_INTEGER) + 1n]) {
      assert.equal(sanitizeResourceProgress({ ...valid, bytesWritten }), undefined);
    }
    assert.equal(sanitizeResourceProgress({ ...valid, filesWritten: 2 }), undefined);
    assert.equal(sanitizeResourceProgress({ ...valid, bytesWritten: 11 }), undefined);
    assert.equal(sanitizeResourceProgress({ ...valid, totalFiles: 1n }), undefined);
    assert.equal(sanitizeResourceProgress(null), undefined);
    assert.equal(sanitizeResourceProgress({}), undefined);
    assert.deepEqual(
      sanitizeResourceProgress({
        ...valid,
        bytesWritten: BigInt(Number.MAX_SAFE_INTEGER),
        totalBytes: BigInt(Number.MAX_SAFE_INTEGER),
      }),
      { ...valid, bytesWritten: Number.MAX_SAFE_INTEGER, totalBytes: Number.MAX_SAFE_INTEGER },
    );
    assert.equal(
      resourceProgressLabel({
        stage: "resources",
        state: "running",
        resourceProgress: { filesWritten: 0, totalFiles: 0, bytesWritten: 0, totalBytes: 0 },
      }),
      "Saving game resources: 0/0 files; 0.0/0.0 MB",
    );
  });

  test("activity log accepts only fixed stages, bounded counts and public codes", () => {
    assert.equal(packagingActivity({ stage: "PRIVATE-PATH", label: "PRIVATE-KEY" }), null);
    assert.equal(
      packagingActivity({ stage: "secrets", label: "PRIVATE-KEY" }),
      "Verifying local keys and packages: running",
    );
    assert.equal(
      packagingActivity({
        stage: "secrets",
        diagnostic: { stage: "content-hash", completed: 1, total: 3, bytes: 2000000 },
      }),
      "Hashing package content (1/3); 2.0 MB",
    );
    assert.equal(
      packagingActivity({
        diagnostic: { stage: "content-hash", completed: 1, total: 3, bytes: 2, private: "CANARY" },
      }),
      null,
    );
    assert.doesNotMatch(
      packagingFailureSummary({ stage: "PRIVATE-PATH", detailCode: "PRIVATE-KEY" }),
      /PRIVATE/,
    );
    assert.equal(
      packagingActivity({
        stage: "extraction",
        diagnostic: { stage: "romfs-merged", completed: 50, total: 100, bytes: 4500000000 },
      }),
      "Verifying merged update RomFS: 50% (4500.0 MB verified)",
    );
  });

  test("resource recovery details stay out of activity logs and copied failure summaries", () => {
    const directoryName = "D2R-resources-12345678-1234-4123-8123-123456789abc";
    const resources = sanitizeResourceOutcome({
      status: "complete",
      directoryName,
      private: "PRIVATE-KEY",
    });
    assert.deepEqual(resources, { status: "complete", directoryName });
    assert.equal(Object.isFrozen(resources), true);
    const report = { stage: "archive", outputStarted: true, resources };
    assert.match(
      packagingFailureMessage(report),
      /Complete game resources remain in D2R-resources-/,
    );
    assert.doesNotMatch(packagingFailureSummary(report), /D2R-resources|PRIVATE/);
    assert.doesNotMatch(
      packagingActivity({ stage: "archive", resources }),
      /D2R-resources|PRIVATE/,
    );
    assert.doesNotMatch(
      packagingFailureMessage({
        ...report,
        resources: { status: "complete", directoryName: "PRIVATE-PATH" },
      }),
      /PRIVATE/,
    );
    const incomplete = {
      status: "incomplete",
      directoryName,
      cleanupComplete: false,
      incompleteMarkerPresent: true,
    };
    assert.match(
      packagingFailureMessage({ ...report, resources: incomplete }),
      /Do not use this as a finished resource set/,
    );
    assert.match(
      packagingFailureMessage({ ...report, resources: incomplete }),
      /warning marker remains/,
    );
    assert.match(
      packagingFailureMessage({
        ...report,
        resources: { ...incomplete, cleanupComplete: true, incompleteMarkerPresent: false },
      }),
      /no resource folder from this attempt remains/,
    );
    assert.equal(sanitizeResourceOutcome({ ...incomplete, cleanupComplete: "true" }), undefined);
  });
  test("early failure never suggests resource exports; key code is actionable", () => {
    const message = packagingFailureMessage({
      stage: "secrets",
      detailCode: "PROD_KEYS_UNREADABLE",
      outputStarted: false,
    });
    assert.match(message, /PROD_KEYS_UNREADABLE/);
    assert.match(message, /No resources were exported/);
    assert.doesNotMatch(message, /may have been saved/);
    assert.match(
      packagingFailureMessage({ stage: "output", outputStarted: true }),
      /may have been saved/,
    );
  });

  test("ticket diagnostics are specific and counters never expose input details", () => {
    for (const [stage, label] of [
      ["ticket-found", "Embedded tickets found"],
      ["ticket-key-added", "Title keys added from tickets"],
      ["ticket-key-reused", "Existing title keys reused"],
    ]) {
      assert.equal(
        packagingActivity({ diagnostic: { stage, completed: 2, total: 2, bytes: 0 } }),
        `${label}: 2`,
      );
      assert.equal(
        packagingActivity({
          diagnostic: { stage, completed: 2, total: 2, bytes: 0, filename: "PRIVATE" },
        }),
        null,
      );
    }
    for (const detailCode of [
      "TICKET_UNSUPPORTED",
      "TICKET_INVALID",
      "TICKET_RIGHTS_ID_MISMATCH",
      "TICKET_KEY_CONFLICT",
      "TICKET_LIMIT_EXCEEDED",
    ]) {
      const message = packagingFailureSummary({ stage: "secrets", detailCode });
      assert.match(message, new RegExp(`\\[${detailCode}\\]`));
      assert.doesNotMatch(message, /undefined/);
    }
    assert.doesNotMatch(
      packagingFailureSummary({ stage: "secrets", detailCode: "TITLE_KEY_REQUIRED" }),
      /does not import NSP tickets/,
    );
  });

  test("shader record labels remain bounded running counters without private fields", async () => {
    const { createShaderDiagnosticObserver } = await import("../browser-packager.mjs");
    const events = [];
    const observer = createShaderDiagnosticObserver((event) => events.push(event));
    observer({ completedRecords: 6927, totalRecords: 6927, name: "PRIVATE" });
    assert.equal(events.length, 1);
    assert.equal(events[0].state, "running");
    assert.equal(shaderProgressLabel(events[0]), "Generating shaders (6927/6927)");
    assert.doesNotMatch(JSON.stringify(events), /PRIVATE|name/);
    assert.equal(shaderProgressLabel({ ...events[0], state: "complete" }), null);
    assert.equal(shaderProgressLabel({ ...events[0], stage: "assets" }), null);
    for (const value of [
      { completedRecords: 2, totalRecords: 1 },
      { completedRecords: 0, totalRecords: 1 },
      { completedRecords: 1, totalRecords: 6928 },
      { completedRecords: NaN, totalRecords: 1 },
    ]) {
      assert.equal(sanitizeShaderRecordProgress(value), undefined);
      observer(value);
    }
    assert.equal(events.length, 1);
    const controller = new AbortController();
    controller.abort();
    createShaderDiagnosticObserver(
      () => assert.fail("cancelled"),
      controller.signal,
    )({ completedRecords: 1, totalRecords: 1 });
    createShaderDiagnosticObserver(() => {
      throw new Error("private");
    })({ completedRecords: 1, totalRecords: 1 });
    createShaderDiagnosticObserver(async () => {
      throw new Error("private");
    })({ completedRecords: 1, totalRecords: 1 });
    await new Promise((resolve) => setImmediate(resolve));
  });

  // 按语法选取进度声明及其条件分支，不依赖后续注释的文本或语言。
  // Select the progress declaration and conditional by syntax, not a following comment's text or language.
  function shaderStatusBranch(text) {
    const file = ts.createSourceFile(
      "App.tsx",
      text,
      ts.ScriptTarget.Latest,
      true,
      ts.ScriptKind.TSX,
    );
    const matches = [];
    function visit(node) {
      if (ts.isBlock(node) || ts.isSourceFile(node)) {
        for (let i = 1; i < node.statements.length; i++) {
          const previous = node.statements[i - 1];
          const branch = node.statements[i];
          if (
            !ts.isVariableStatement(previous) ||
            !ts.isIfStatement(branch) ||
            !ts.isIdentifier(branch.expression) ||
            branch.expression.text !== "shaderLabel" ||
            !ts.isBlock(branch.thenStatement)
          )
            continue;
          const declaration = previous.declarationList.declarations;
          if (declaration.length !== 1) continue;
          const { name, initializer } = declaration[0];
          if (
            !ts.isIdentifier(name) ||
            name.text !== "shaderLabel" ||
            !initializer ||
            !ts.isCallExpression(initializer) ||
            !ts.isIdentifier(initializer.expression) ||
            initializer.expression.text !== "shaderProgressLabel" ||
            initializer.arguments.length !== 1 ||
            !ts.isIdentifier(initializer.arguments[0]) ||
            initializer.arguments[0].text !== "event"
          )
            continue;
          matches.push(text.slice(previous.getStart(file), branch.thenStatement.end));
        }
      }
      ts.forEachChild(node, visit);
    }
    visit(file);
    assert.equal(matches.length, 1, "Expected one shader progress status branch");
    return matches[0];
  }

  test("shader status source selection is independent of following comments", () => {
    const branch =
      "const shaderLabel = shaderProgressLabel(event); if (shaderLabel) { pendingGenerationStatus.current = shaderLabel; appendActivity(null); return; }";
    for (const comment of [
      "// 补充包导出单独显示",
      "// Supplemental export status",
      "/* renamed */",
      "",
    ]) {
      const source = `function progress(event) { ${branch}\n${comment}\n unrelated(); }`;
      assert.equal(shaderStatusBranch(source), branch);
    }
    assert.throws(
      () =>
        shaderStatusBranch(
          "// const shaderLabel = shaderProgressLabel(event); if (shaderLabel) {}",
        ),
      /Expected one/,
    );
    assert.throws(
      () => shaderStatusBranch(`function progress(event) { ${branch} ${branch} }`),
      /Expected one/,
    );
  });

  test("production shader counters use coalesced UI status without record log spam", async () => {
    const { readFile } = await import("node:fs/promises");
    const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const branch = shaderStatusBranch(app);
    assert.match(branch, /pendingGenerationStatus.current = shaderLabel/);
    assert.match(branch, /appendActivity\(null\)/);
    assert.match(branch, /return;/);
    assert.match(
      app,
      /cancellation.current\?\.abort\(\);\s*pendingGenerationStatus.current = null/,
    );
    const packager = await readFile(new URL("../browser-packager.mjs", import.meta.url), "utf8");
    assert.match(
      packager,
      /onProgress: createShaderDiagnosticObserver\(onProgress, context.signal\)/,
    );
    const shaders = await readFile(new URL("../production-shaders.mjs", import.meta.url), "utf8");
    assert.ok(
      shaders.indexOf("reportRecords(pending.length)") >
        shaders.indexOf("budget.replace(0, base.length"),
    );
    assert.ok(
      shaders.indexOf("reportRecords(pending.length)") <
        shaders.indexOf("await pipeline.verifyCompleteSourceSet()"),
    );
  });

  test("IPA output failure preserves an explicitly skipped resource export", () => {
    for (const code of ["STAGE_FAILED", "CANCELLED"]) {
      const message = packagingFailureMessage({
        code,
        stage: "output",
        outputStarted: true,
        resources: { status: "skipped", reason: "user-requested" },
      });
      assert.match(message, /No resources were exported by this attempt/);
      assert.doesNotMatch(message, /Resources may have been saved/);
    }
    for (const resources of [
      undefined,
      { status: "skipped" },
      { status: "skipped", reason: "other" },
    ])
      assert.match(
        packagingFailureMessage({
          code: "STAGE_FAILED",
          stage: "output",
          outputStarted: true,
          resources,
        }),
        /Resources may have been saved/,
      );
  });

  test("shader reports display allowlisted AIR codes without raw compiler text", () => {
    const report = {
      stage: "shaders",
      code: "STAGE_FAILED",
      detailCode: "AIR_FRONTEND_FAILED",
      message: "PRIVATE-SOURCE-CANARY",
    };
    assert.match(packagingFailureSummary(report), /\[AIR_FRONTEND_FAILED\]/);
    assert.doesNotMatch(packagingFailureSummary(report), /PRIVATE/);
    assert.doesNotMatch(
      packagingFailureSummary({ ...report, stage: "assets" }),
      /AIR_FRONTEND_FAILED/,
    );
    assert.doesNotMatch(
      packagingFailureSummary({ ...report, detailCode: "AIR_PRIVATE-SOURCE-CANARY" }),
      /PRIVATE/,
    );
  });

  test("shader failure details show actionable local diagnostics without raw reports", () => {
    const report = {
      stage: "shaders",
      code: "STAGE_FAILED",
      message: "PRIVATE-RAW-REPORT",
      stack: "PRIVATE-STACK",
      shaderDiagnostic: {
        substep: "compilation",
        compilerPhase: "frontend",
        errorType: "Error",
        record: 1,
        variant: "b",
        shaderStage: "vertex",
        diagnostics: ["error: use of undeclared identifier 'reverse_bits'"],
        sourceSha256: "a".repeat(64),
        sourceDigest: "b".repeat(16),
        private: "PRIVATE-KEY",
        path: "/PRIVATE-PATH",
      },
    };
    const lines = shaderFailureDetails(report).join("\n");
    assert.match(lines, /compilation/);
    assert.match(lines, /#1/);
    assert.match(lines, /reverse_bits/);
    assert.match(lines, /SHA-256: a{64}/);
    assert.doesNotMatch(lines, /PRIVATE/);
    assert.deepEqual(shaderFailureDetails({ ...report, stage: "secrets" }), []);
    assert.deepEqual(shaderFailureDetails({ ...report, code: "CANCELLED" }), []);
    assert.deepEqual(shaderFailureDetails({ stage: "shaders" }), []);
  });

  test("UI appends sanitized shader detail lines after the failure summary", async () => {
    const { readFile } = await import("node:fs/promises");
    const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    assert.match(
      app,
      /appendActivity\(packagingFailureSummary\(report\), true\);\s*for \(const detail of shaderFailureDetails\(report\)\) appendActivity\(detail, true\)/,
    );
  });

  test("shader progress exposes only a bounded actual compiler worker count", () => {
    for (const count of [1, 2, 4]) {
      const progress = sanitizeShaderRecordProgress({
        completedRecords: 2,
        totalRecords: 6927,
        compilerWorkers: count,
      });
      assert.equal(progress.compilerWorkers, count);
      assert.equal(
        shaderProgressLabel({
          stage: "shaders",
          state: "running",
          diagnostic: { kind: "shader-records", ...progress },
        }),
        `Generating shaders (2/6927) · compiler workers: ${count}`,
      );
    }
    for (const count of [0, 5, NaN, "4", "PRIVATE"]) {
      const progress = sanitizeShaderRecordProgress({
        completedRecords: 2,
        totalRecords: 6927,
        compilerWorkers: count,
      });
      assert.equal(progress.compilerWorkers, undefined);
    }
  });
  test("direct data reuse has checking progress and non-destructive collision instructions", () => {
    const progress = sanitizeResourceProgress({
      operation: "verify",
      filesWritten: 1,
      totalFiles: 2,
      bytesWritten: 3,
      totalBytes: 4,
    });
    assert.match(
      resourceProgressLabel({ stage: "resources", state: "running", resourceProgress: progress }),
      /^Checking existing resources:/,
    );
    assert.deepEqual(sanitizeResourceOutcome({ status: "complete", directoryName: "data" }), {
      status: "complete",
      directoryName: "data",
    });
    const message = packagingFailureMessage({
      stage: "output",
      outputStarted: true,
      resources: {
        status: "incomplete",
        directoryName: "data",
        cleanupComplete: true,
        incompleteMarkerPresent: false,
        existingDataPreserved: true,
        reason: "RESOURCE_EXISTING_DATA_UNVERIFIED",
      },
    });
    assert.match(message, /existing data folder could not be verified/);
    assert.match(message, /files were not changed/);
    assert.match(message, /Skip resource export/);
    assert.doesNotMatch(message, /export was removed|Complete game resources remain/);
    assert.doesNotMatch(
      JSON.stringify(
        sanitizeResourceOutcome({
          status: "incomplete",
          directoryName: "data",
          cleanupComplete: true,
          incompleteMarkerPresent: false,
          reason: "PRIVATE-PATH",
        }),
      ),
      /PRIVATE/,
    );
  });
}

// packaging-job
{
  // 验证 packaging-job 的约定与边界；依据 ../packaging-job.mjs 及本文件的测试断言。
  // Verify packaging-job contracts and boundaries against ../packaging-job.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { runPackagingJob, REQUIRED_PACKAGING_ADAPTERS, PackagingJobError } = packagingJobModule;

  // 仅为合成编排样例，不证明解密、转换、签名或 IPA 兼容性。
  // Synthetic orchestration fixtures ONLY. Not decryption, conversion, signing or IPA compatibility proof.
  function fixture() {
    const calls = [],
      progress = [];
    const input = ["game.nsp", "prod.keys", "title.keys"].map((name) => ({
      name,
      webkitRelativePath: `folder/${name}`,
      size: 0,
      slice() {
        throw new Error("Keys must only be opened by secret adapter");
      },
    }));
    const targetOS = { platform: "ios", minimumVersion: "16.0" };
    const entry = (path, mode = 0o100644) => ({
      path,
      mode,
      size: 1,
      async *chunks() {
        yield new Uint8Array([1]);
      },
    });
    const shellEntries = ["Info.plist", "Game", "dm/shell_info.txt", "dm/shell_manifest.txt"].map(
      (p) => entry(`Payload/Game.app/${p}`),
    );
    const requiredPaths = ["Frameworks/Guest.framework/", "dm/assets", "msl/shader"];
    const contract = {
      entries: shellEntries,
      shellInfoText: "PRODUCT=test\nCHAIN=test\nKIND=ios\nSHADER_SRC=0123456789abcdef",
      shellManifestText: requiredPaths.join("\n"),
      trustedRecipe: {
        schemaVersion: 1,
        product: "test",
        chain: "test",
        kind: "ios",
        shaderDigest: "0123456789abcdef",
        targetOS,
        requiredPaths,
      },
      observedTargetOS: targetOS,
    };
    const content = {
      contentId: "1".repeat(32),
      sha256: "2".repeat(64),
      size: 1n,
      type: 1,
      idOffset: 0,
    };
    const app = 0xf123456789abc000n;
    const base = {
      type: "Application",
      titleId: app,
      applicationId: app,
      patchId: app + 0x800n,
      version: 0n,
      contents: [content],
    };
    const adapters = {
      async openSecretSession() {
        calls.push("open");
        return {
          async dispose() {
            calls.push("dispose");
          },
        };
      },
      async inspectContents() {
        calls.push("inspect");
        return { records: [base], inventory: [{ ...content, hashVerified: true }] };
      },
      async extractSelected() {
        calls.push("extract");
        return {};
      },
      async generateProgram(context) {
        assert.equal(context.inputs, undefined);
        assert.equal(context.session, undefined);
        calls.push("program");
        return [
          {
            ...entry("Frameworks/Guest.framework/Guest", 0o100755),
            signature: { kind: "ad-hoc", offset: 0, size: 1 },
          },
        ];
      },
      async generateAssets() {
        calls.push("assets");
        return [entry("dm/assets")];
      },
      async generateShaders() {
        calls.push("shaders");
        return [entry("msl/shader")];
      },
      async loadShell() {
        calls.push("shell");
        return { contract, entries: shellEntries };
      },
      async validateGenerated() {
        calls.push("validate");
        return { valid: true, observedOutputTargetOS: targetOS };
      },
      async createOutput() {
        calls.push("create");
        return {
          async write() {
            calls.push("write");
          },
          async close() {
            calls.push("close");
          },
          async abort() {
            calls.push("abort");
          },
        };
      },
      async validateArchive() {
        calls.push("archive");
        return { valid: true };
      },
      async cleanup() {
        calls.push("cleanup");
      },
    };
    return { input, adapters, calls, progress, onProgress: (p) => progress.push(p) };
  }

  test("missing capabilities fail before touching input or opening output", async () => {
    for (const missing of REQUIRED_PACKAGING_ADAPTERS) {
      const f = fixture();
      delete f.adapters[missing];
      f.input = {
        get kind() {
          throw new Error("Input touched");
        },
      };
      await assert.rejects(runPackagingJob(f), (e) => e.report.code === "CAPABILITY_UNAVAILABLE");
      assert.deepEqual(f.calls, []);
    }
  });
  test("complete ordered lifecycle and deterministic detached manifest", async () => {
    const f = fixture();
    const result = await runPackagingJob(f);
    assert.equal(result.status, "complete");
    assert.equal(result.installable, false);
    assert.equal(result.committed, true);
    assert.deepEqual(f.calls.slice(0, 4), ["open", "inspect", "extract", "dispose"]);
    assert.ok(f.calls.indexOf("validate") < f.calls.indexOf("create"));
    assert.deepEqual(f.calls.slice(-3), ["archive", "cleanup", "close"]);
    assert.deepEqual(
      result.manifest.map((e) => e.path),
      result.manifest.map((e) => e.path).sort(),
    );
    assert.ok(Object.isFrozen(result.manifest[0]));
    assert.equal(f.calls.includes("abort"), false);
  });
  test("selected NSP names exclude an optional package before opening secrets", async () => {
    const f = fixture();
    f.input.push({
      name: "dlc.nsp",
      webkitRelativePath: "folder/dlc.nsp",
      size: 0,
      slice() {
        throw new Error("Excluded package must not be opened");
      },
    });
    f.adapters.openSecretSession = async ({ inputs }) => {
      assert.deepEqual(
        inputs.nsps.map((file) => file.name),
        ["game.nsp"],
      );
      return { async dispose() {} };
    };
    const result = await runPackagingJob({ ...f, selectedNspNames: ["game.nsp"] });
    assert.equal(result.status, "complete");
  });
  test("verified RomFS progress reaches the packaging observer", async () => {
    const f = fixture();
    f.adapters.extractSelected = async ({ onDiagnostic }) => {
      onDiagnostic({ stage: "romfs-base", completed: 50, total: 100, bytes: 4000000000 });
      return {};
    };
    await runPackagingJob(f);
    assert(
      f.progress.some(
        (event) =>
          event.stage === "extraction" &&
          event.label === "Verifying base RomFS: 50% (4000.0 MB verified)",
      ),
    );
  });
  test("bounded generator concurrency and draining in-flight work before cleanup", async () => {
    const f = fixture();
    let active = 0,
      peak = 0;
    for (const key of ["generateProgram", "generateAssets", "generateShaders"]) {
      const original = f.adapters[key];
      f.adapters[key] = async (context) => {
        active++;
        peak = Math.max(peak, active);
        await new Promise((resolve) => setTimeout(resolve, 5));
        active--;
        return original(context);
      };
    }
    await runPackagingJob({ ...f, concurrency: 2 });
    assert.equal(peak, 2);
    assert.equal(active, 0);
  });
  test("original error preserved, cleanup errors sanitized, no arbitrary messages reported", async () => {
    const f = fixture(),
      original = new Error("SECRET KEY CONTENT");
    f.adapters.inspectContents = async () => {
      throw original;
    };
    f.adapters.openSecretSession = async () => ({
      async dispose() {
        throw new Error("SECRET DISPOSE");
      },
    });
    f.adapters.cleanup = async () => {
      throw new Error("SECRET CLEANUP");
    };
    await assert.rejects(runPackagingJob(f), (e) => {
      assert.ok(e instanceof PackagingJobError);
      assert.equal(e.cause, original);
      assert.deepEqual(e.report.cleanupCodes, [
        "SECRET_DISPOSAL_FAILED",
        "RESOURCE_CLEANUP_FAILED",
      ]);
      assert.equal(JSON.stringify(e).includes("CONTENT"), false);
      assert.equal(e.report.stage, "inspection");
      return true;
    });
    assert.equal(f.calls.includes("create"), false);
  });
  test("failed generator cancels peers, waits for drain, and never creates output", async () => {
    const f = fixture();
    let settled = false;
    f.adapters.generateProgram = async () => {
      throw new Error("failure");
    };
    f.adapters.generateAssets = async () => {
      await new Promise((r) => setTimeout(r, 10));
      settled = true;
      return [];
    };
    f.adapters.cleanup = async () => {
      assert.equal(settled, true);
    };
    await assert.rejects(runPackagingJob(f));
    assert.equal(f.calls.includes("create"), false);
  });
  test("cancellation during extraction disposes session and does not generate", async () => {
    const f = fixture(),
      controller = new AbortController();
    f.adapters.extractSelected = async () => {
      controller.abort("SECRET");
      return {};
    };
    await assert.rejects(
      runPackagingJob({ ...f, signal: controller.signal }),
      (e) => e.report.code === "CANCELLED",
    );
    assert.ok(f.calls.includes("dispose"));
    assert.equal(f.calls.includes("program"), false);
  });
  test("validation and close failures prevent success and abort the staged output", async () => {
    for (const failing of ["validateGenerated", "validateArchive", "close", "cleanup"]) {
      const f = fixture();
      if (failing === "close")
        f.adapters.createOutput = async () => ({
          async write() {},
          async close() {
            throw new Error("close failed");
          },
          async abort() {
            f.calls.push("abort");
          },
        });
      else
        f.adapters[failing] = async () => {
          throw new Error("failure");
        };
      await assert.rejects(runPackagingJob(f), (e) => !e.report.committed);
      assert.equal(f.calls.includes("abort"), failing !== "validateGenerated");
    }
  });
  test("writes await sink backpressure and cancellation after close begins cannot undo commit", async () => {
    const f = fixture(),
      controller = new AbortController();
    let writing = false,
      writes = 0;
    f.adapters.createOutput = async () => ({
      async write() {
        assert.equal(writing, false);
        writing = true;
        await Promise.resolve();
        writes++;
        writing = false;
      },
      async close() {
        controller.abort();
      },
      async abort() {
        assert.fail("Committed output cannot be aborted");
      },
    });
    const result = await runPackagingJob({
      ...f,
      signal: controller.signal,
      onProgress() {
        throw new Error("observer secret");
      },
    });
    assert.equal(result.committed, true);
    assert.ok(writes > 0);
  });
  // 提交前的快照与复制仍可取消；无关关闭错误不能被竞态取消掩盖。
  // Snapshot and copy before commit remain cancellable; racing cancellation must not mask unrelated close errors.
  for (const phase of ["snapshot", "copy", "close-failure"]) {
    test(`pre-commit ${phase} preserves the actual outcome`, async () => {
      const f = fixture(),
        controller = new AbortController();
      const diskFailure = new Error("synthetic close failure");
      let aborts = 0,
        committed = false;
      f.adapters.createOutput = async ({ signal }) => ({
        async write() {},
        async close() {
          // 与实际暂存适配器相同，组合信号保留调用方取消原因的身份。
          // Like the real staged adapter, a composed signal preserves the caller's abort-reason identity.
          const transferSignal = AbortSignal.any([signal, new AbortController().signal]);
          await Promise.resolve();
          if (phase === "copy") await Promise.resolve();
          controller.abort();
          if (phase === "close-failure") throw diskFailure;
          transferSignal.throwIfAborted();
          committed = true;
        },
        async abort() {
          aborts++;
        },
      });
      await assert.rejects(runPackagingJob({ ...f, signal: controller.signal }), (error) => {
        assert.equal(error.report.stage, "commit");
        assert.equal(error.report.commitStarted, true);
        assert.equal(error.report.committed, false);
        assert.equal(error.report.code, phase === "close-failure" ? "STAGE_FAILED" : "CANCELLED");
        if (phase === "close-failure") assert.equal(error.cause, diskFailure);
        return true;
      });
      assert.equal(committed, false);
      assert.equal(aborts, 1);
    });
  }
  test("forged streamed shell inventory is rejected before output creation", async () => {
    const f = fixture(),
      load = f.adapters.loadShell;
    f.adapters.loadShell = async (context) => {
      const shell = await load(context);
      shell.entries = [...shell.entries, { path: "extra", size: 0, mode: 0o644 }];
      return shell;
    };
    await assert.rejects(runPackagingJob(f));
    assert.equal(f.calls.includes("create"), false);
  });

  test("every required generation stage and both explicit validation acknowledgments gate output", async () => {
    for (const key of ["generateProgram", "generateAssets", "generateShaders", "loadShell"]) {
      const f = fixture();
      f.adapters[key] = async () => {
        throw new Error("unsupported capability");
      };
      await assert.rejects(runPackagingJob(f));
      assert.equal(f.calls.includes("create"), false);
      assert.ok(f.calls.includes("dispose"));
    }
    for (const key of ["validateGenerated", "validateArchive"]) {
      const f = fixture();
      f.adapters[key] = async () => ({ valid: false });
      await assert.rejects(runPackagingJob(f));
      assert.equal(f.calls.includes("close"), false);
    }
  });
  test("cancellation during writes aborts once, keeps abort failures sanitized, and never closes", async () => {
    const f = fixture(),
      controller = new AbortController();
    let aborts = 0;
    f.adapters.createOutput = async () => ({
      async write() {
        controller.abort("SECRET REASON");
      },
      async close() {
        assert.fail("Must not close");
      },
      async abort(error) {
        aborts++;
        assert.equal(error.message, "PACKAGING_ABORTED");
        throw new Error("SECRET SINK");
      },
    });
    await assert.rejects(runPackagingJob({ ...f, signal: controller.signal }), (e) => {
      assert.equal(e.report.code, "CANCELLED");
      assert.deepEqual(e.report.cleanupCodes, ["OUTPUT_ABORT_FAILED"]);
      assert.equal(JSON.stringify(e.report).includes("SECRET REASON"), false);
      return true;
    });
    assert.equal(aborts, 1);
  });

  test("ZIP64 opt-in requires a bounded budget before input or secrets", async () => {
    for (const maxOutputBytes of [undefined, 0, -1, Infinity, Number.MAX_SAFE_INTEGER + 1]) {
      const f = fixture();
      await assert.rejects(runPackagingJob({ ...f, archiveFormat: "zip64", maxOutputBytes }));
      assert.deepEqual(f.calls, []);
    }
  });
  test("ZIP64 keeps staged validation and cleanup before commit", async () => {
    const f = fixture();
    const result = await runPackagingJob({
      ...f,
      archiveFormat: "zip64",
      maxOutputBytes: 1024 * 1024,
    });
    assert.equal(result.committed, true);
    assert.equal(typeof result.bytesWritten, "bigint");
    assert.ok(f.calls.indexOf("archive") < f.calls.indexOf("close"));
    assert.ok(f.calls.indexOf("cleanup") < f.calls.indexOf("close"));
  });
  test("ZIP64 rejects an insufficient output budget before destination creation", async () => {
    const f = fixture();
    await assert.rejects(runPackagingJob({ ...f, archiveFormat: "zip64", maxOutputBytes: 1 }));
    assert.equal(f.calls.includes("create"), false);
  });

  test("explicit lazy mode retains isolated session through output reads and disposes before validation", async () => {
    const f = fixture();
    let alive = false;
    f.adapters.openSecretSession = async () => {
      alive = true;
      f.calls.push("open");
      return {
        async dispose() {
          alive = false;
          f.calls.push("dispose");
        },
      };
    };
    f.adapters.generateAssets = async (context) => {
      assert.equal(context.session, undefined);
      assert.equal(context.inputs, undefined);
      return [
        {
          path: "dm/assets",
          mode: 0o100644,
          size: 1,
          async *chunks() {
            assert.equal(alive, true);
            yield new Uint8Array([1]);
          },
        },
      ];
    };
    f.adapters.validateArchive = async () => {
      assert.equal(alive, false);
      f.calls.push("archive");
      return { valid: true };
    };
    await runPackagingJob({ ...f, secretLifetime: "archive" });
    assert.ok(f.calls.indexOf("write") < f.calls.indexOf("dispose"));
    assert.ok(f.calls.indexOf("dispose") < f.calls.indexOf("archive"));
    assert.ok(f.calls.indexOf("archive") < f.calls.indexOf("close"));
  });
  test("lazy-mode disposal failure prevents archive validation and commit", async () => {
    const f = fixture();
    f.adapters.openSecretSession = async () => ({
      async dispose() {
        f.calls.push("dispose");
        throw new Error("private key session cleanup failure");
      },
    });
    await assert.rejects(runPackagingJob({ ...f, secretLifetime: "archive" }), (e) => {
      assert.equal(e.report.stage, "disposal");
      return true;
    });
    assert.equal(f.calls.filter((x) => x === "dispose").length, 1);
    assert.equal(f.calls.includes("archive"), false);
    assert.equal(f.calls.includes("close"), false);
    assert.equal(f.calls.includes("abort"), true);
  });
  test("lazy mode still disposes when generation or writing fails", async () => {
    for (const which of ["generation", "writing"]) {
      const f = fixture();
      if (which === "generation")
        f.adapters.generateAssets = async () => {
          throw new Error("failed");
        };
      else
        f.adapters.createOutput = async () => ({
          write: async () => {
            throw new Error("failed");
          },
          close: async () => assert.fail("close"),
          abort: async () => {},
        });
      await assert.rejects(runPackagingJob({ ...f, secretLifetime: "archive" }));
      assert.equal(f.calls.filter((x) => x === "dispose").length, 1);
    }
  });

  test("safe input diagnoses survive packaging without exposing raw causes or implying output", async () => {
    const f = fixture();
    f.adapters.openSecretSession = async () => {
      const error = new Error("PRIVATE-KEY-PATH-CANARY");
      error.code = "PROD_KEYS_UNREADABLE";
      throw error;
    };
    await assert.rejects(runPackagingJob(f), (error) => {
      assert.equal(error.report.detailCode, "PROD_KEYS_UNREADABLE");
      assert.equal(error.report.outputStarted, false);
      assert.doesNotMatch(JSON.stringify(error.report), /PRIVATE/);
      return true;
    });
  });

  test("diagnostics from finished stages cannot overwrite current job progress", async () => {
    const f = fixture();
    const diagnostic = { stage: "keys-load", completed: 0, total: 0, bytes: 0 };
    let savedObserver;
    const open = f.adapters.openSecretSession;
    f.adapters.openSecretSession = async (context) => {
      savedObserver = context.onDiagnostic;
      savedObserver(diagnostic);
      return open(context);
    };
    const inspect = f.adapters.inspectContents;
    f.adapters.inspectContents = async (context) => {
      savedObserver(diagnostic);
      return inspect(context);
    };
    const events = [];
    await runPackagingJob({ ...f, onProgress: (event) => events.push(event) });
    savedObserver(diagnostic);
    assert.equal(events.filter((event) => event.diagnostic).length, 1);
  });

  // 合成观察者失败必须独立于任务结果，也不能延长任务生命周期。
  // Synthetic observer failures stay independent of the job outcome and cannot extend its lifetime.
  test("rejected async stage and extraction diagnostic observers preserve successful cleanup and commit", async () => {
    const f = fixture();
    let stages = 0,
      diagnostics = 0;
    f.adapters.extractSelected = async ({ onDiagnostic }) => {
      onDiagnostic({ stage: "romfs-base", completed: 50, total: 100, bytes: 64 });
      return {};
    };
    const result = await runPackagingJob({
      ...f,
      onProgress: async (event) => {
        if (event.diagnostic) diagnostics++;
        else stages++;
        throw new Error("synthetic async observer failure");
      },
    });
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(result.committed, true);
    assert.ok(stages > 0);
    assert.equal(diagnostics, 1);
    assert.ok(f.calls.includes("dispose"));
    assert.ok(f.calls.includes("cleanup"));
  });
  test("rejected async observer cannot replace the underlying job failure", async () => {
    const f = fixture(),
      primary = new Error("synthetic extraction failure");
    f.adapters.extractSelected = async ({ onDiagnostic }) => {
      onDiagnostic({ stage: "romfs-base", completed: 50, total: 100, bytes: 64 });
      throw primary;
    };
    await assert.rejects(
      runPackagingJob({
        ...f,
        onProgress: async () => {
          throw new Error("synthetic async observer failure");
        },
      }),
      (error) => error instanceof PackagingJobError && error.cause === primary,
    );
    await new Promise((resolve) => setImmediate(resolve));
    assert.ok(f.calls.includes("dispose"));
    assert.ok(f.calls.includes("cleanup"));
  });
  test("unsettled progress observers are never awaited before successful commit", async () => {
    const f = fixture();
    f.adapters.extractSelected = async ({ onDiagnostic }) => {
      onDiagnostic({ stage: "romfs-base", completed: 50, total: 100, bytes: 64 });
      return {};
    };
    const result = await runPackagingJob({ ...f, onProgress: () => new Promise(() => {}) });
    assert.equal(result.committed, true);
    assert.ok(f.calls.includes("cleanup"));
  });

  test("shader failures preserve only fixed AIR codes at the shader stage", async () => {
    for (const [method, code, expected] of [
      ["generateShaders", "AIR_FRONTEND_FAILED", "AIR_FRONTEND_FAILED"],
      ["generateShaders", "AIR_COMPILATION_TIMEOUT", "AIR_COMPILATION_TIMEOUT"],
      ["generateShaders", "PRIVATE-SOURCE-DIAGNOSTIC", null],
      ["generateAssets", "AIR_FRONTEND_FAILED", null],
    ]) {
      const f = fixture();
      f.adapters[method] = async () => {
        throw Object.assign(new Error("PRIVATE-SOURCE-CANARY"), { code });
      };
      await assert.rejects(runPackagingJob(f), (error) => {
        assert.equal(error.report.detailCode, expected);
        assert.equal(error.report.outputStarted, false);
        assert.doesNotMatch(JSON.stringify(error.report), /PRIVATE/);
        return true;
      });
    }
  });

  test("shader report exposes useful bounded diagnostics without original exception text", async () => {
    const f = fixture();
    f.adapters.generateShaders = async () => {
      throw Object.assign(new Error("/Users/private/prod.keys PRIVATE_RAW_CAUSE"), {
        code: "AIR_FRONTEND_FAILED",
        shaderDiagnostic: {
          substep: "compilation",
          record: 4,
          variant: "bf",
          shaderStage: "fragment",
          compilerPhase: "compiling",
          sourceSha256: "b".repeat(64),
          diagnostics: [
            "use of undeclared identifier 'texture_missing'",
            "error: password = PRIVATE_SECRET",
          ],
          path: "/Users/private/game.nsp",
        },
      });
    };
    await assert.rejects(runPackagingJob(f), (error) => {
      assert.equal(error.report.detailCode, "AIR_FRONTEND_FAILED");
      assert.equal(error.report.shaderDiagnostic.substep, "compilation");
      assert.equal(error.report.shaderDiagnostic.record, 4);
      assert.deepEqual(error.report.shaderDiagnostic.diagnostics, [
        "use of undeclared identifier 'texture_missing'",
      ]);
      assert.equal(error.report.shaderDiagnostic.sourceSha256, "b".repeat(64));
      assert.doesNotMatch(JSON.stringify(error.report), /PRIVATE|Users|prod.keys/);
      return true;
    });
  });

  // 程序阶段允许固定诊断；本地输入打开阶段仅允许版本及模块不匹配诊断。
  // Program generation admits fixed codes; local input opening admits only version/tuple failures.
  test("program compatibility reports enforce stage scope before extraction diagnostics", async () => {
    const known = [
      "PROGRAM_UNSUPPORTED_VERSION",
      "PROGRAM_MODULE_RELEASE_MISMATCH",
      "PROGRAM_SHELL_RELEASE_MISMATCH",
      "PROGRAM_INVALID_MODULE_SET",
      "PROGRAM_INPUT_DIGEST_MISMATCH",
      "PROGRAM_BUILD_ID_MISMATCH",
    ];
    const stages = [
      ["openSecretSession", "secrets"],
      ["inspectContents", "inspection"],
      ["selectContents", "selection"],
      ["extractSelected", "extraction"],
      ["generateProgram", "program"],
      ["generateAssets", "assets"],
      ["generateShaders", "shaders"],
      ["loadShell", "shell"],
      ["validateGenerated", "validation"],
      ["createOutput", "output"],
      ["validateArchive", "archive"],
      ["cleanup", "cleanup"],
    ];
    for (const code of [
      ...known,
      "PRIVATE-KEY",
      "PROGRAM_UNSUPPORTED_VERSION /PRIVATE-PATH",
      null,
    ]) {
      for (const [method, stage] of stages) {
        const f = fixture();
        f.adapters[method] = async () => {
          if (method === "cleanup") f.calls.push("cleanup");
          throw Object.assign(new Error("PRIVATE-PATH PRIVATE-KEY"), {
            code,
            private: "PRIVATE-INPUT",
          });
        };
        await assert.rejects(runPackagingJob(f), (error) => {
          const allowed =
            (stage === "program" && known.includes(code)) ||
            (stage === "secrets" && known.slice(0, 2).includes(code));
          assert.equal(error.report.stage, stage);
          assert.equal(error.report.detailCode, allowed ? code : null, `${stage}: ${code}`);
          assert.doesNotMatch(JSON.stringify(error.report), /PRIVATE/);
          assert.equal(error.report.committed, false);
          if (!["output", "archive", "cleanup"].includes(stage))
            assert.equal(error.report.outputStarted, false);
          return true;
        });
        assert.ok(f.calls.includes("cleanup"));
        if (!["output", "archive", "cleanup"].includes(stage))
          assert.equal(f.calls.includes("create"), false);
      }
    }
  });

  test("public diagnostic retains only the single checked error code value", async () => {
    const f = fixture();
    let reads = 0;
    f.adapters.generateProgram = async () => {
      throw Object.defineProperty(new Error("PRIVATE-CAUSE"), "code", {
        get() {
          return ++reads === 1 ? "PROGRAM_UNSUPPORTED_VERSION" : "PRIVATE-UNCHECKED-CODE";
        },
      });
    };
    await assert.rejects(runPackagingJob(f), (error) => {
      assert.equal(error.report.detailCode, "PROGRAM_UNSUPPORTED_VERSION");
      assert.doesNotMatch(JSON.stringify(error.report), /PRIVATE/);
      return true;
    });
    assert.equal(reads, 1);
  });

  test("mixed package selection includes XCI names without reading excluded packages", async () => {
    const f = fixture();
    f.input.push({
      name: "base.xci",
      webkitRelativePath: "folder/base.xci",
      size: 0,
      slice() {
        throw Error("metadata only");
      },
    });
    const original = f.adapters.openSecretSession;
    f.adapters.openSecretSession = async (context) => {
      assert.deepEqual(
        context.inputs.nsps.map((file) => file.name),
        ["base.xci"],
      );
      return original(context);
    };
    await runPackagingJob({ ...f, selectedNspNames: ["base.xci"] });
  });
  test("extracted orchestration accepts a separate selection capability without fabricated CNMT", async () => {
    const f = fixture();
    const inputs = ["ExeFS/main", "ExeFS/sdk", "RomFS/data/build_manifest.psv"].map((path) =>
      Object.assign(new Blob(["synthetic"]), {
        name: path.split("/").at(-1),
        webkitRelativePath: `root/${path}`,
      }),
    );
    const selection = {
      inputKind: "extracted",
      dlcMetadata: "not-authenticated",
      contentMode: "base-update-only",
      dlc: [],
    };
    f.adapters.inspectContents = async () => ({ inputKind: "extracted" });
    f.adapters.selectContents = async ({ inputs: discovered }) => {
      assert.equal(discovered.inputMode, "extracted");
      assert.equal(Object.hasOwn(discovered, "prodKeys"), false);
      return selection;
    };
    f.adapters.extractSelected = async (context) => {
      assert.equal(context.selection, selection);
      return {};
    };
    await runPackagingJob({ ...f, input: inputs });
    await assert.rejects(
      runPackagingJob({
        ...fixture(),
        input: inputs,
        inputMode: "extracted",
        selectedNspNames: [],
      }),
    );
  });
}

// content-selection
{
  // 验证 content-selection 的约定与边界；依据 ../content-selection.mjs 及本文件的测试断言。
  // Verify content-selection contracts and boundaries against ../content-selection.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { parseCnmt, resolveContentSelection, ContentSelectionError } = contentSelectionModule;
  const app = 0xf123456789abc000n,
    patchId = app + 0x800n;
  const c = (n = 1) => ({
    contentId: n.toString(16).padStart(32, "0"),
    sha256: n.toString(16).padStart(64, "0"),
    size: 100n,
    type: 1,
    idOffset: 0,
  });
  const base = (version = 0n) => ({
    type: "Application",
    titleId: app,
    applicationId: app,
    patchId,
    version,
    contents: [c()],
  });
  const patch = (version = 65536n) => ({
    type: "Patch",
    titleId: patchId,
    applicationId: app,
    version,
    contents: [c(2)],
  });
  const dlc = (version = 0n) => ({
    type: "AddOnContent",
    titleId: app + 0x1001n,
    applicationId: app,
    version,
    requiredApplicationVersion: 65536n,
    contents: [c(3)],
  });
  const inv = (...rows) => [
    ...new Map(
      rows
        .flatMap((r) => r.contents)
        .map((c) => [c.contentId, { contentId: c.contentId, size: c.size }]),
    ).values(),
  ];
  const code = (expected) => (error) =>
    error instanceof ContentSelectionError && error.code === expected;
  function fixture(type = 128, ext = type === 129 ? 24 : 16, extra = 0) {
    const bytes = new Uint8Array(32 + ext + 56 + extra + 32),
      v = new DataView(bytes.buffer);
    v.setBigUint64(0, type === 128 ? app : type === 129 ? patchId : app + 0x1001n, true);
    v.setUint32(8, 65536, true);
    v.setUint8(12, type);
    v.setUint16(14, ext, true);
    v.setUint16(16, 1, true);
    v.setBigUint64(32, type === 128 ? patchId : app, true);
    if (type === 129) v.setUint32(44, extra, true);
    const p = 32 + ext;
    bytes[p + 31] = 1;
    bytes[p + 47] = 1;
    v.setUint32(p + 48, 100, true);
    bytes[p + 54] = 1;
    return bytes;
  }
  test("CNMT reads exact little-endian IDs, version, hash, size, content slot", () => {
    const bytes = fixture(),
      p = 48;
    bytes[p + 52] = 1;
    bytes[p + 53] = 2;
    const r = parseCnmt(bytes);
    assert.equal(r.titleId, app);
    assert.equal(r.patchId, patchId);
    assert.equal(r.version, 65536n);
    assert.equal(r.contents[0].size, 4294967396n);
    assert.equal(r.contents[0].attributes, 2);
    assert.equal(r.digestVerified, false);
    assert.equal(r.contents[0].sha256, c().sha256);
    const padded = new Uint8Array(bytes.length + 20);
    padded.set(bytes, 7);
    assert.deepEqual(parseCnmt(padded.subarray(7, 7 + bytes.length)), r);
  });
  test("patch extended data bounds and both DLC extended headers", () => {
    assert.equal(parseCnmt(fixture(129, 24, 48)).extendedDataSize, 48);
    assert.equal(parseCnmt(fixture(130)).applicationId, app);
    assert.equal(parseCnmt(fixture(130, 24)).type, "AddOnContent");
    const b = fixture(130, 24);
    new DataView(b.buffer).setBigUint64(48, 1n, true);
    assert.throws(() => parseCnmt(b), code("UNSUPPORTED_DATA_PATCH"));
  });
  test("full patch ignores authenticated delta fragment records during selection", () => {
    const original = fixture(129, 24);
    const bytes = new Uint8Array(original.length + 56);
    bytes.set(original.subarray(0, 32 + 24 + 56));
    new DataView(bytes.buffer).setUint16(16, 2, true);
    bytes.set(original.subarray(32 + 24 + 56), 32 + 24 + 112);
    const fragment = 32 + 24 + 56;
    bytes[fragment + 31] = 2;
    bytes[fragment + 47] = 2;
    new DataView(bytes.buffer).setUint32(fragment + 48, 200, true);
    bytes[fragment + 54] = 6;
    const parsed = parseCnmt(bytes);
    assert.equal(parsed.contents.length, 1);
    assert.equal(parsed.deltaFragmentCount, 1);
    assert.equal(parsed.contents[0].type, 1);
    bytes[fragment + 53] = 1;
    assert.throws(() => parseCnmt(bytes), code("UNSUPPORTED_CONTENT_TYPE"));
    bytes[fragment + 53] = 0;
    bytes[fragment + 47] = 1;
    assert.throws(() => parseCnmt(bytes), code("DUPLICATE_CONTENT"));
  });
  test("all truncations and trailing data fail closed", () => {
    const b = fixture();
    for (let i = 0; i < b.length; i++)
      assert.throws(() => parseCnmt(b.subarray(0, i)), ContentSelectionError);
    assert.throws(() => parseCnmt(new Uint8Array(b.length + 1)), ContentSelectionError);
    assert.throws(() => parseCnmt(new Uint8Array(16 * 1024 * 1024 + 1)), code("INVALID_CNMT_SIZE"));
  });
  test("unsupported type, content, dependencies, platform, counts and lengths", () => {
    for (const [offset, value, expected] of [
      [12, 131, "UNSUPPORTED_META_TYPE"],
      [13, 1, "UNSUPPORTED_PLATFORM"],
      [22, 1, "UNSUPPORTED_PLATFORM"],
      [14, 8, "INVALID_EXTENDED_HEADER"],
      [16, 0, "INVALID_CONTENT"],
      [102, 6, "UNSUPPORTED_CONTENT_TYPE"],
    ]) {
      const b = fixture();
      b[offset] = value;
      assert.throws(() => parseCnmt(b), code(expected));
    }
    const b = fixture(129);
    new DataView(b.buffer).setUint32(44, 0xffffffff, true);
    assert.throws(() => parseCnmt(b), code("INVALID_CNMT_SIZE"));
    const deps = new Uint8Array(fixture().length + 16);
    deps.set(fixture());
    deps[18] = 1;
    assert.throws(() => parseCnmt(deps), code("UNSUPPORTED_DEPENDENCIES"));
  });
  test("base + explicit full patch + compatible DLC deterministic across input order", () => {
    const rows = [base(), patch(), dlc()];
    const a = resolveContentSelection(rows, inv(...rows));
    const b = resolveContentSelection([...rows].reverse(), inv(...rows).reverse());
    assert.deepEqual(a, b);
    assert.equal(a.effectiveVersion, 65536n);
    assert.equal(a.dlc.length, 1);
    assert.equal(a.gameCompatibility, "unknown");
    assert.equal(a.requiresExecutableAllowlist, true);
    assert.equal(a.patchApplied, false);
  });
  test("missing base, orphan DLC, ambiguous applications and patch ID mismatch", () => {
    assert.throws(() => resolveContentSelection([patch()], inv(patch())), code("MISSING_BASE"));
    assert.throws(
      () => resolveContentSelection([base(), { ...dlc(), applicationId: app + 1n }], []),
      code("ORPHAN_CONTENT"),
    );
    assert.throws(
      () =>
        resolveContentSelection(
          [base(), { ...base(), titleId: app + 1n, applicationId: app + 1n }],
          [],
        ),
      code("AMBIGUOUS_APPLICATION"),
    );
    assert.throws(
      () => resolveContentSelection([base(), { ...patch(), titleId: patchId + 1n }], []),
      code("INVALID_ASSOCIATION"),
    );
  });
  test("versions are numeric, explicit, range-safe, and never inferred from newest", () => {
    const rows = [base(), patch(10n), patch(2n)];
    assert.throws(
      () => resolveContentSelection(rows, inv(...rows)),
      (e) => e.code === "AMBIGUOUS_VERSION" && e.message.includes("2, 10"),
    );
    assert.equal(
      resolveContentSelection(rows, inv(...rows), { patchVersion: 2n }).patch.version,
      2n,
    );
    assert.throws(
      () => resolveContentSelection(rows, [], { patchVersion: 9n }),
      code("MISSING_VERSION"),
    );
    for (const version of [
      -1n,
      4294967296n,
      NaN,
      Infinity,
      1.5,
      "65536",
      Number.MAX_SAFE_INTEGER + 1,
    ])
      assert.throws(() => resolveContentSelection([base(version)], []), code("INVALID_INTEGER"));
    assert.throws(
      () => resolveContentSelection([{ ...base(), titleId: 1n << 64n }], []),
      code("INVALID_INTEGER"),
    );
  });
  test("DLC compatibility and base/update minimum versions are enforced", () => {
    assert.throws(
      () => resolveContentSelection([base(), dlc()], inv(base(), dlc())),
      code("INCOMPATIBLE_DLC"),
    );
    assert.throws(
      () => resolveContentSelection([base(10n), patch(2n)], []),
      code("INCOMPATIBLE_VERSION"),
    );
    assert.throws(
      () => resolveContentSelection([{ ...base(), requiredApplicationVersion: 1n }], []),
      code("INCOMPATIBLE_VERSION"),
    );
  });
  test("duplicate equal records are idempotent; differing same-version records fail", () => {
    assert.deepEqual(
      resolveContentSelection([base(), base()], inv(base(), base())),
      resolveContentSelection([base()], inv(base())),
    );
    assert.throws(
      () => resolveContentSelection([base(), { ...base(), contents: [c(8)] }], []),
      code("CONFLICTING_METADATA"),
    );
    assert.throws(
      () => resolveContentSelection([{ ...base(), contents: [c(), c()] }], []),
      code("DUPLICATE_CONTENT"),
    );
    assert.throws(
      () => resolveContentSelection([{ ...base(), contents: [c(), c(2)] }], []),
      code("DUPLICATE_CONTENT"),
    );
  });
  test("required content existence, size, and inventory conflicts are gates", () => {
    assert.throws(() => resolveContentSelection([base()], []), code("MISSING_CONTENT"));
    assert.throws(
      () => resolveContentSelection([base()], [{ ...c(), hashVerified: true, size: 99n }]),
      code("CONTENT_IDENTITY_MISMATCH"),
    );
    assert.throws(
      () =>
        resolveContentSelection(
          [base()],
          [...inv(base()), { ...c(), hashVerified: true, size: 99n }],
        ),
      code("CONFLICTING_CONTENT"),
    );
    assert.throws(
      () => resolveContentSelection([{ ...base(), contents: [{ ...c(), contentId: "bad" }] }], []),
      code("INVALID_IDENTITY"),
    );
    assert.throws(
      () => resolveContentSelection([{ ...base(), contents: [{ ...c(), size: 1n << 40n }] }], []),
      code("INVALID_INTEGER"),
    );
  });
  test("selection accepts unique content IDs with matching sizes without hash claims", () => {
    const unverified = { ...c(), sha256: null, hashVerified: false };
    assert.equal(resolveContentSelection([base()], [unverified]).base.titleId, base().titleId);
    assert.throws(
      () => resolveContentSelection([base()], [unverified, unverified]),
      code("CONFLICTING_CONTENT"),
    );
    assert.throws(
      () => resolveContentSelection([base()], [{ ...unverified, size: 99n }], {}),
      code("CONTENT_IDENTITY_MISMATCH"),
    );
  });
  test("multiple DLC versions require explicit choice and DLC is never merged", () => {
    const rows = [base(), patch(), dlc(2n), dlc(10n)],
      id = (app + 0x1001n).toString(16);
    assert.throws(() => resolveContentSelection(rows, inv(...rows)), code("AMBIGUOUS_VERSION"));
    const result = resolveContentSelection(rows, inv(...rows), { dlcVersions: { [id]: 2n } });
    assert.equal(result.dlc[0].version, 2n);
    assert.equal(result.base.contents.length, 1);
  });

  test("conflicting CNMT digests or required-system metadata are not silently deduplicated", () => {
    assert.throws(
      () =>
        resolveContentSelection(
          [
            { ...base(), cnmtDigest: "a".repeat(64) },
            { ...base(), cnmtDigest: "b".repeat(64) },
          ],
          [],
        ),
      code("CONFLICTING_METADATA"),
    );
    assert.throws(
      () => resolveContentSelection([base(), { ...base(), requiredSystemVersion: 1n }], []),
      code("CONFLICTING_METADATA"),
    );
  });
  test("malformed arrays, inventory, non-default DLC accessibility, and oversized count fail", () => {
    for (const rows of [null, [], new Array(4097).fill(base())])
      assert.throws(() => resolveContentSelection(rows, []), code("INVALID_INPUT"));
    assert.throws(() => resolveContentSelection([base()], [null]), code("INVALID_INPUT"));
    assert.throws(() => resolveContentSelection([base()], [], null), code("INVALID_INPUT"));
    assert.throws(
      () => resolveContentSelection([{ ...dlc(), contentAccessibilities: 1 }], []),
      code("UNSUPPORTED_DLC_ACCESS"),
    );
    const b = fixture();
    new DataView(b.buffer).setUint16(16, 4097, true);
    assert.throws(() => parseCnmt(b), code("INVALID_CONTENT"));
    const d = fixture(130, 24);
    d[44] = 1;
    assert.throws(() => parseCnmt(d), code("UNSUPPORTED_DLC_ACCESS"));
  });

  // 显式版本请求不能被静默忽略；这里只验证元数据策略，不授予认证。
  // Explicit version requests must not be silently ignored; this tests metadata policy, not authentication.
  test("DLC version requests reject absent titles without changing optional defaults", () => {
    const rows = [base(), patch()];
    const id = (app + 0x1001n).toString(16);
    for (const options of [{}, { dlcVersions: {} }, { dlcVersions: Object.create(null) }])
      assert.deepEqual(resolveContentSelection(rows, inv(...rows), options).dlc, []);
    assert.throws(
      () => resolveContentSelection(rows, inv(...rows), { dlcVersions: { [id]: 0n } }),
      code("MISSING_VERSION"),
    );
    const withDlc = [...rows, dlc(2n)];
    assert.throws(
      () =>
        resolveContentSelection(withDlc, inv(...withDlc), {
          dlcVersions: { [id]: 2n, ["0000000000000001"]: 0n },
        }),
      code("MISSING_VERSION"),
    );
  });
  test("DLC version map types, canonical keys and exact uint32 values fail closed", () => {
    const id = (app + 0x1001n).toString(16);
    const rows = [base(), patch(), dlc(2n)];
    const run = (dlcVersions) => resolveContentSelection(rows, inv(...rows), { dlcVersions });
    for (const value of [null, [], "not a map", new Map(), new Date(), 2, true])
      assert.throws(() => run(value), code("INVALID_INPUT"));
    for (const key of [id.toUpperCase(), "1", "g".repeat(16), Symbol("id")])
      assert.throws(() => run({ [key]: 2n }), code("INVALID_IDENTITY"));
    for (const value of [undefined, "2", -1, 1.5, 2 ** 32, 1n << 32n, Number.MAX_SAFE_INTEGER + 1])
      assert.throws(() => run({ [id]: value }), code("INVALID_INTEGER"));
    let getterCalled = false;
    assert.throws(
      () =>
        run(
          Object.defineProperty({}, id, {
            get() {
              getterCalled = true;
              return 2n;
            },
          }),
        ),
      code("INVALID_INPUT"),
    );
    assert.equal(getterCalled, false);
    assert.throws(
      () =>
        run(
          Object.fromEntries(
            Array.from({ length: 4097 }, (_, i) => [i.toString(16).padStart(16, "0"), 0]),
          ),
        ),
      code("INVALID_INPUT"),
    );
    for (const version of [2, 2n]) {
      const map = Object.assign(Object.create(null), { [id]: version });
      assert.equal(run(map).dlc[0].version, 2n);
    }
    assert.throws(
      () => resolveContentSelection([{}], [], { dlcVersions: [] }),
      code("INVALID_INPUT"),
    );
  });
}

// d2r-expansion-policy
{
  // 验证 d2r-expansion-policy 的约定与边界；依据 ../d2r-expansion-policy.mjs 及本文件的测试断言。
  // Verify d2r-expansion-policy contracts and boundaries against ../d2r-expansion-policy.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const {
    D2R_APPLICATION_ID: app,
    D2R_PATCH_ID: patchId,
    D2R_WARLOCK_ID: warlock,
    resolveD2rExpansionSelection: resolve,
  } = d2rExpansionPolicyModule;

  function fixture() {
    const content = (n, type = 1) => ({
      contentId: n.toString(16).padStart(32, "0"),
      sha256: n.toString(16).padStart(64, "0"),
      size: 100n,
      type,
      idOffset: 0,
    });
    return [
      {
        type: "Application",
        titleId: app,
        applicationId: app,
        patchId,
        version: 0n,
        contents: [content(1)],
      },
      {
        type: "Patch",
        titleId: patchId,
        applicationId: app,
        version: 65536n,
        contents: [content(2)],
      },
      {
        type: "AddOnContent",
        titleId: warlock,
        applicationId: app,
        version: 0n,
        requiredApplicationVersion: 65536n,
        contents: [content(3, 2)],
      },
    ];
  }
  const inventory = (rows) => [
    ...new Map(
      rows
        .flatMap((r) => r.contents)
        .map((c) => [c.contentId, { contentId: c.contentId, size: c.size }]),
    ).values(),
  ];
  const code = (expected) => (error) => error.code === expected;

  test("known expansion remains separate and fully required, never marked owned or compatible", () => {
    const rows = fixture();
    const result = resolve(rows, inventory(rows));
    assert.equal(result.dlc[0].titleId, warlock);
    assert.equal(result.requiredContents.length, 3);
    assert.equal(result.base.contents.length, 1);
    assert.equal(result.patch.contents.length, 1);
    assert.equal(result.requiresExecutableAllowlist, true);
    assert.equal(result.gameCompatibility, "unknown");
    assert.equal(result.patchApplied, false);
    assert.equal("owned" in result, false);
  });

  test("missing expansion/update and unknown expansion reject", () => {
    const rows = fixture();
    assert.throws(() => resolve(rows.slice(0, 2), inventory(rows)), code("D2R_EXPANSION_REQUIRED"));
    rows[2].requiredApplicationVersion = 0n;
    assert.throws(() => resolve([rows[0], rows[2]], inventory(rows)), code("D2R_UPDATE_REQUIRED"));
    rows[2].titleId++;
    assert.throws(() => resolve(rows, inventory(rows)), code("D2R_UNKNOWN_EXPANSION"));
  });

  test("association and required version checks are retained", () => {
    let rows = fixture();
    rows[2].applicationId++;
    assert.throws(() => resolve(rows, inventory(rows)), code("ORPHAN_CONTENT"));
    rows = fixture();
    rows[2].requiredApplicationVersion = 65537n;
    assert.throws(() => resolve(rows, inventory(rows)), code("INCOMPATIBLE_DLC"));
    rows = fixture();
    rows[1].titleId++;
    assert.throws(() => resolve(rows, inventory(rows)), code("INVALID_ASSOCIATION"));
    rows = fixture();
    for (const r of rows) {
      r.applicationId += 0x2000n;
      r.titleId += 0x2000n;
    }
    rows[0].patchId += 0x2000n;
    assert.throws(() => resolve(rows, inventory(rows)), code("D2R_APPLICATION_REQUIRED"));
  });

  test("missing, mismatched and conflicting inventory reject", () => {
    const rows = fixture();
    const inv = inventory(rows);
    assert.throws(() => resolve(rows, inv.slice(0, 2)), code("MISSING_CONTENT"));
    inv[2].size++;
    assert.throws(() => resolve(rows, inv), code("CONTENT_IDENTITY_MISMATCH"));
    assert.throws(() => resolve(rows, [...inv, inventory(rows)[2]]), code("CONFLICTING_CONTENT"));
  });

  test("only conservative nonempty single Data slot is accepted", () => {
    for (const change of [
      (c) => (c.type = 1),
      (c) => (c.idOffset = 1),
      (c) => (c.attributes = 1),
      (c) => (c.size = 0n),
    ]) {
      const rows = fixture();
      change(rows[2].contents[0]);
      assert.throws(() => resolve(rows, inventory(rows)), code("D2R_EXPANSION_LAYOUT_UNSUPPORTED"));
    }
    const rows = fixture();
    rows[2].contents.push({
      ...rows[2].contents[0],
      contentId: "f".repeat(32),
      idOffset: 1,
    });
    assert.throws(() => resolve(rows, inventory(rows)), code("D2R_EXPANSION_LAYOUT_UNSUPPORTED"));
  });

  test("multiple DLC versions require explicit choice without omitting DLC evidence", () => {
    const rows = fixture();
    rows.push({ ...rows[2], version: 1n });
    assert.throws(() => resolve(rows, inventory(rows)), code("AMBIGUOUS_VERSION"));
    const selected = resolve(rows, inventory(rows), {
      dlcVersions: { [warlock.toString(16).padStart(16, "0")]: 1n },
    });
    assert.equal(selected.dlc[0].version, 1n);
  });
}

// download-preparation
{
  // 使用合成 Blob 和 URL/DOM 替身验证下载失败不损坏已完成产物。
  // Synthetic Blobs and URL/DOM doubles verify download failure cannot invalidate completed artifacts.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { readFile } = promisesModule;
  const { createDownloadPreparation, attemptAutomaticDownload } = downloadPreparationModule;
  test("failed URL preparation retains identical completed Blob for retry, then reuses URL", () => {
    const blob = new Blob(["synthetic IPA"]),
      calls = [],
      revoked = [];
    let fail = true;
    const session = createDownloadPreparation({
      createObjectURL(value) {
        calls.push(value);
        if (fail) throw new Error("private error");
        return "blob:ready";
      },
      revokeObjectURL(value) {
        revoked.push(value);
      },
    });
    session.retain({ blob, fileName: "ready.ipa" });
    assert.deepEqual(session.prepare(), { status: "retryable" });
    assert.equal(revoked.length, 0);
    fail = false;
    assert.deepEqual(session.prepare(), {
      status: "ready",
      url: "blob:ready",
      fileName: "ready.ipa",
    });
    assert.deepEqual(calls, [blob, blob]);
    session.prepare();
    assert.equal(calls.length, 2);
    session.dispose();
    assert.deepEqual(revoked, ["blob:ready"]);
    assert.deepEqual(session.prepare(), { status: "unavailable" });
  });
  test("replacement does not revoke prior URL until new URL creation succeeds", () => {
    let fail = false,
      count = 0;
    const revoked = [];
    const session = createDownloadPreparation({
      createObjectURL() {
        if (fail) throw new Error("no URL");
        return `blob:${++count}`;
      },
      revokeObjectURL(value) {
        revoked.push(value);
      },
    });
    session.retain({ blob: new Blob(["one"]), fileName: "one.ipa" });
    session.prepare();
    session.retain({ blob: new Blob(["two"]), fileName: "two.ipa" });
    fail = true;
    session.prepare();
    assert.deepEqual(revoked, []);
    fail = false;
    assert.equal(session.prepare().url, "blob:2");
    assert.deepEqual(revoked, ["blob:1"]);
    session.dispose();
    assert.deepEqual(revoked, ["blob:1", "blob:2"]);
  });
  test("all automatic download DOM failures are contained and never revoke URL", () => {
    for (const phase of ["create", "append", "click", "remove"]) {
      const node = {
        click() {
          if (phase === "click") throw new Error("blocked");
        },
        remove() {
          if (phase === "remove") throw new Error("blocked");
        },
      };
      const document = {
        createElement() {
          if (phase === "create") throw new Error("blocked");
          return node;
        },
        body: {
          append() {
            if (phase === "append") throw new Error("blocked");
          },
        },
      };
      assert.doesNotThrow(() =>
        attemptAutomaticDownload({ url: "blob:retained", fileName: "ready.ipa" }, document),
      );
    }
  });
  // 只放宽 JSX 空白；仍要求零参数箭头回调，不能直接调用或传递点击事件。
  // Allow JSX whitespace only; still require a zero-argument arrow callback, not immediate invocation or event forwarding.
  const downloadRetryButton =
    /<Button\s+onClick\s*=\s*\{\s*\(\s*\)\s*=>\s*prepareDownload\s*\(\s*\)\s*\}\s*>/;
  test("download retry button check ignores whitespace but rejects event forwarding and immediate calls", () => {
    for (const source of [
      "<Button onClick={() => prepareDownload()}>",
      "<Button\n  onClick = { ( ) => prepareDownload ( ) }\n>",
      "<Button onClick={()=>prepareDownload()} >",
    ])
      assert.match(source, downloadRetryButton);
    for (const source of [
      "<Button onClick={prepareDownload}>",
      "<Button onClick={prepareDownload()}>",
      "<Button onClick={(event) => prepareDownload(event)}>",
      "<Button onClick={() => prepareDownload(event)}>",
      "<Button onClick={() => anotherAction()}>",
    ])
      assert.doesNotMatch(source, downloadRetryButton);
  });
  test("UI retry never reruns conversion and completed outcomes bypass conversion-failure catch", async () => {
    const app = await readFile(new URL("../src/App.tsx", import.meta.url), "utf8");
    const retry = app.slice(
      app.indexOf("  function prepareDownload("),
      app.indexOf("  async function generate()"),
    );
    assert.match(retry, /downloadPreparation.current.prepare\(\)/);
    assert.doesNotMatch(retry, /\.generate\(|setLegacyStatus|setResourceOutcome/);
    assert.match(
      app,
      /generationCompleted = true;[\s\S]*?downloadPreparation.current.retain\(result.download\)/,
    );
    assert.match(
      app,
      /if \(generationCompleted\) \{[\s\S]*?dmWebDownloadPreparationFailed[\s\S]*?return;\s+\}\s+tracker.fail\(controller.signal.aborted\);\s+publishProgress\(tracker\);\s+const report/,
    );
    assert.match(app, /preparedDownload.dispose\(\)/);
    assert.match(app, downloadRetryButton);
  });
}

// export-write-batching
{
  // 合成字节和文件系统调用计数，不代表磁盘速度或浏览器表现。
  // Synthetic bytes and filesystem call counts are not disk-speed or browser evidence.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { exportRomfsResources } = resourceFolderExportModule;
  const { exportLegacyDevicePack } = legacyDevicePackExportModule;
  const { resourceOutputLocks } = resourceOutputLocksModule;
  const { createResourceWriteBuffer, RESOURCE_WRITE_BUFFER_BYTES: BATCH } =
    resourceWriteBufferModule;
  const contents = Uint8Array.from({ length: 2 * 1024 * 1024 + 17 }, (_, i) => i % 251);
  const sourceText = await readFile(
    new URL("../resource-folder-export.mjs", import.meta.url),
    "utf8",
  );
  const native = [...sourceText.matchAll(/\["([\d-]+\.data)", (\d+)n\]/g)].map(
    ([, name, size]) => ({
      path: `data/${name}`,
      size: BigInt(size),
    }),
  );
  function fixture(kind, mode) {
    let reads = 0,
      written = 0;
    const calls = [],
      controller = new AbortController();
    const stop = Error("stop after synthetic first resource");
    function directory() {
      const entries = new Map();
      const handle = {
        kind: "directory",
        isSameEntry: async (other) => other === handle,
        queryPermission: async () => "granted",
        getDirectoryHandle: async (name, options) => get(name, options, true),
        getFileHandle: async (name, options) => get(name, options, false),
        async removeEntry(name, options) {
          assert.equal(options, undefined);
          if (entries.get(name)?.entries?.size) throw Error("not empty");
          entries.delete(name);
        },
        entries,
      };
      function get(name, options, isDir) {
        if (!entries.has(name)) {
          if (!options?.create) throw new DOMException("missing", "NotFoundError");
          entries.set(name, isDir ? directory() : file(name));
        }
        return entries.get(name);
      }
      return handle;
    }
    function file(name) {
      let size = 0;
      const handle = {
        kind: "file",
        isSameEntry: async (other) => other === handle,
        getFile: async () => ({ size }),
        async createWritable() {
          size = 0;
          return {
            async write(bytes) {
              if (name === "0-test.bin" || name === "00-00000008.data") {
                calls.push(bytes.length);
                assert.ok(bytes.length <= BATCH);
                if (mode === "write-fail" && calls.length === 2) throw stop;
                assert.deepEqual(bytes, contents.subarray(written, written + bytes.length));
                written += bytes.length;
                if (mode === "write-cancel") controller.abort();
              }
              size += bytes.length;
            },
            close: async () => {},
            abort: async () => {},
          };
        },
      };
      return handle;
    }
    const root = directory();
    async function readAt(offset, count) {
      reads++;
      assert.ok(count <= 65536);
      const bytes = contents.slice(Number(offset), Number(offset) + count);
      if (mode === "read-cancel" && reads === 2) controller.abort();
      return bytes;
    }
    const run = () =>
      kind === "legacy"
        ? exportLegacyDevicePack({
            locks: resourceOutputLocks(),
            outputDirectoryHandle: root,
            secureContext: true,
            signal: controller.signal,
            finalizedPack: {
              pack: { name: "00-00000008.data", size: contents.length, sha256: "a".repeat(64) },
              source: { size: contents.length, file: { size: contents.length }, readAt },
            },
          })
        : exportRomfsResources({
            locks: resourceOutputLocks(),
            outputDirectoryHandle: root,
            signal: controller.signal,
            romfs: {
              files: [{ path: "data/0-test.bin", size: BigInt(contents.length) }, ...native],
              getFile(path) {
                if (path === "data/0-test.bin") return { size: BigInt(contents.length), readAt };
                const entry = native.find((file) => file.path === path);
                return {
                  size: entry.size,
                  readAt: async () => {
                    throw stop;
                  },
                };
              },
            },
          });
    return {
      run,
      controller,
      calls,
      stop,
      get reads() {
        return reads;
      },
      get written() {
        return written;
      },
    };
  }
  for (const kind of ["legacy", "resources"])
    for (const mode of ["exact", "read-cancel", "write-cancel", "write-fail"])
      test(`${kind}: bounded batch ${mode}`, async () => {
        const old = Object.getOwnPropertyDescriptor(globalThis, "isSecureContext");
        Object.defineProperty(globalThis, "isSecureContext", { value: true, configurable: true });
        try {
          const f = fixture(kind, mode);
          let report;
          try {
            report = await f.run();
          } catch (error) {
            report = error.report;
            if (mode === "exact") assert.equal(error.cause, f.stop);
          }
          assert.ok(report);
          if (mode === "exact") {
            assert.equal(f.reads, 33);
            assert.deepEqual(f.calls, [BATCH, BATCH, 17]);
            assert.equal(f.written, contents.length);
            assert.equal(Number(report.bytesWritten), contents.length);
          } else if (mode === "read-cancel") {
            assert.equal(f.reads, 2);
            assert.deepEqual(f.calls, []);
            assert.equal(Number(report.bytesWritten), 0);
          } else {
            assert.equal(Number(report.bytesWritten), BATCH);
            assert.equal(f.written, BATCH);
            assert.deepEqual(f.calls, mode === "write-fail" ? [BATCH, BATCH] : [BATCH]);
          }
          if (mode !== "exact") assert.equal(report.status, "incomplete");
        } finally {
          if (old) Object.defineProperty(globalThis, "isSecureContext", old);
          else delete globalThis.isSecureContext;
        }
      });
  test("coalescer copies without mutating source aliases and wipes retained buffer after writes", async () => {
    const source = new Uint8Array(65536).fill(7),
      alias = source.subarray(0);
    let sent,
      written = 0;
    const buffer = createResourceWriteBuffer({
      check() {},
      written: (n) => (written += n),
      write: async (bytes) => {
        sent = bytes;
        assert.equal(bytes[0], 7);
      },
    });
    await buffer.append(source);
    assert.equal(written, 0);
    assert.equal(alias[0], 7);
    await buffer.flush();
    assert.equal(written, source.length);
    assert.ok(sent.every((n) => n === 0));
    assert.ok(alias.every((n) => n === 7));
    buffer.dispose();
  });
  test("coalescer enforces serial backpressure and rejects concurrent disposal", async () => {
    let release,
      calls = 0;
    const buffer = createResourceWriteBuffer({
      check() {},
      written() {},
      write: async () => {
        calls++;
        await new Promise((resolve) => {
          release = resolve;
        });
      },
    });
    await buffer.append(Uint8Array.of(9));
    const pending = buffer.flush();
    await assert.rejects(buffer.flush(), /BUSY/);
    await assert.rejects(buffer.append(Uint8Array.of(8)), /BUSY/);
    assert.throws(() => buffer.dispose(), /BUSY/);
    assert.equal(calls, 1);
    release();
    await pending;
    buffer.dispose();
  });

  test("coalescer clears only used tails while all retained backing bytes remain zero", async (t) => {
    const fills = [],
      originalFill = Uint8Array.prototype.fill;
    t.mock.method(Uint8Array.prototype, "fill", function (value, start = 0, end = this.length) {
      if (this.length === BATCH && value === 0) fills.push(end - start);
      return originalFill.call(this, value, start, end);
    });
    let retained;
    const buffer = createResourceWriteBuffer({
      check() {},
      written() {},
      write: async (bytes) => {
        retained = new Uint8Array(bytes.buffer);
      },
    });
    for (const count of [65536, 17, 1]) {
      await buffer.append(new Uint8Array(count).fill(7));
      await buffer.flush();
      assert.ok(retained.every((value) => value === 0));
    }
    assert.deepEqual(fills, [65536, 17, 1]);
    buffer.dispose();
    assert.ok(retained.every((value) => value === 0));
  });
}

// synthetic-export
{
  // 验证 synthetic-export 的约定与边界；依据 ../synthetic-export.mjs、../guest-framework.mjs 及本文件的测试断言。
  // Verify synthetic-export contracts and boundaries against ../synthetic-export.mjs、../guest-framework.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { webcrypto } = nodeCryptoModule;
  const { exportSyntheticArchive, syntheticModules, SYNTHETIC_CHUNK_BYTES, SYNTHETIC_FILENAME } =
    syntheticExportModule;
  const { buildGuestFramework, guestInfoPlist } = guestFrameworkModule;

  function sink() {
    return {
      parts: [],
      closed: 0,
      aborted: 0,
      async write(bytes) {
        this.parts.push(bytes.slice());
      },
      async close() {
        this.closed++;
      },
      async abort() {
        this.aborted++;
      },
    };
  }
  // 解析中央目录及本地载荷，不访问磁盘或用户文件。
  // Parse central directory and local payloads without disk or user file access.
  function unpack(parts) {
    const bytes = Buffer.concat(parts),
      view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const end = bytes.length - 22;
    assert.equal(view.getUint32(end, true), 0x06054b50);
    let at = view.getUint32(end + 16, true);
    const files = new Map();
    for (let i = 0; i < view.getUint16(end + 10, true); i++) {
      assert.equal(view.getUint32(at, true), 0x02014b50);
      const size = view.getUint32(at + 24, true),
        nameLength = view.getUint16(at + 28, true);
      const name = bytes.subarray(at + 46, at + 46 + nameLength).toString();
      const local = view.getUint32(at + 42, true);
      const start =
        local + 30 + view.getUint16(local + 26, true) + view.getUint16(local + 28, true);
      files.set(name, bytes.subarray(start, start + size));
      at += 46 + nameLength + view.getUint16(at + 30, true) + view.getUint16(at + 32, true);
    }
    return files;
  }
  test("synthetic ZIP contains exactly the generated Guest, plist and explicit warning; chunks and progress are bounded", async () => {
    const out = sink(),
      progress = [];
    const result = await exportSyntheticArchive(out, {
      crypto: webcrypto,
      onProgress: (p) => progress.push(p),
    });
    assert.equal(SYNTHETIC_FILENAME, "d2r-packager-synthetic-test.zip");
    assert.equal(out.closed, 1);
    assert.equal(out.aborted, 0);
    assert.ok(out.parts.every((p) => p.length <= SYNTHETIC_CHUNK_BYTES));
    assert.equal(
      result.bytesWritten,
      out.parts.reduce((n, p) => n + p.length, 0),
    );
    assert.equal(result.entries, 3);
    const files = unpack(out.parts);
    assert.deepEqual(
      [...files.keys()],
      ["Guest.framework/Guest", "Guest.framework/Info.plist", "README-NOT-INSTALLABLE.txt"],
    );
    const expected = await buildGuestFramework(syntheticModules(), { crypto: webcrypto });
    assert.deepEqual(new Uint8Array(files.get("Guest.framework/Guest")), expected);
    assert.equal(
      files.get("Guest.framework/Info.plist").toString(),
      guestInfoPlist("local.synthetic.guest"),
    );
    assert.match(files.get("README-NOT-INSTALLABLE.txt").toString(), /^NOT INSTALLABLE/);
    assert.match(files.get("README-NOT-INSTALLABLE.txt").toString(), /NO GAME CONTENT/);
    assert.equal(progress[0].phase, "generating");
    assert.equal(progress[0].bytesWritten, 0);
    assert.equal(progress.at(-1).phase, "finishing");
    assert.equal(progress.at(-1).bytesWritten, result.bytesWritten);
    assert.ok(progress.every((p, i) => !i || p.bytesWritten >= progress[i - 1].bytesWritten));
  });
  test("pre-abort and generation failure abort the sink before ZIP writes begin", async () => {
    const controller = new AbortController();
    controller.abort();
    for (const options of [{ signal: controller.signal, crypto: webcrypto }, { crypto: {} }]) {
      const out = sink();
      await assert.rejects(exportSyntheticArchive(out, options));
      assert.equal(out.parts.length, 0);
      assert.equal(out.aborted, 1);
      assert.equal(out.closed, 0);
    }
  });
  test("cancellation during framework hashing aborts without starting ZIP", async () => {
    const out = sink(),
      controller = new AbortController();
    const crypto = {
      subtle: {
        async digest(...args) {
          const digest = await webcrypto.subtle.digest(...args);
          controller.abort();
          return digest;
        },
      },
    };
    await assert.rejects(exportSyntheticArchive(out, { crypto, signal: controller.signal }), {
      name: "AbortError",
    });
    assert.equal(out.parts.length, 0);
    assert.equal(out.aborted, 1);
    assert.equal(out.closed, 0);
  });
  test("cancellation while writing aborts exactly once and does not close", async () => {
    const out = sink(),
      controller = new AbortController();
    out.write = async () => {
      controller.abort();
    };
    await assert.rejects(
      exportSyntheticArchive(out, { crypto: webcrypto, signal: controller.signal }),
      { name: "AbortError" },
    );
    assert.equal(out.aborted, 1);
    assert.equal(out.closed, 0);
  });
  test("write and close failures preserve original error and abort exactly once", async () => {
    for (const operation of ["write", "close"]) {
      const out = sink(),
        failure = new Error(`${operation} failed`);
      out[operation] = async () => {
        throw failure;
      };
      out.abort = async function () {
        this.aborted++;
        throw new Error("cleanup failed");
      };
      await assert.rejects(
        exportSyntheticArchive(out, { crypto: webcrypto }),
        (e) => e === failure,
      );
      assert.equal(out.aborted, 1);
    }
  });
  test("late cancellation after close starts does not report a committed save as cancelled", async () => {
    const out = sink(),
      controller = new AbortController();
    out.close = async function () {
      controller.abort();
      this.closed++;
    };
    const result = await exportSyntheticArchive(out, {
      crypto: webcrypto,
      signal: controller.signal,
    });
    assert.equal(out.closed, 1);
    assert.equal(out.aborted, 0);
    assert.equal(result.entries, 3);
  });
}

// legacy-device-pack-export
{
  // 用合成暂存源和文件系统替身验证导出、取消及所有权；不构成真实浏览器证明。
  // Verify export, cancellation and ownership with synthetic staged sources and filesystem doubles, not browser proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { resourceOutputLocks } = resourceOutputLocksModule;
  const {
    exportLegacyDevicePack,
    LegacyPackExportError,
    LEGACY_PACK_EXPORT_MARKER: MARKER,
  } = legacyDevicePackExportModule;
  const folderName = "Legacy Device Pack";
  function fixture(faults = {}) {
    const events = [];
    function directory(path = "") {
      const entries = new Map();
      const node = {
        kind: "directory",
        entries,
        isSameEntry: async (other) => other === node,
        queryPermission: async () => faults.permission ?? "granted",
        async getDirectoryHandle(name, options) {
          return get(name, options, "directory");
        },
        async getFileHandle(name, options) {
          return get(name, options, "file");
        },
        async removeEntry(name, options) {
          assert.equal(options, undefined);
          events.push(["remove", `${path}/${name}`]);
          faults.remove?.(`${path}/${name}`);
          const item = entries.get(name);
          if (!item) throw new DOMException("missing", "NotFoundError");
          if (item.kind === "directory" && item.entries.size)
            throw new DOMException("not empty", "InvalidModificationError");
          entries.delete(name);
        },
      };
      async function get(name, options, kind) {
        let item = entries.get(name);
        if (item && item.kind !== kind) throw new DOMException("kind", "TypeMismatchError");
        if (!item && !options?.create) throw new DOMException("missing", "NotFoundError");
        if (!item) {
          item = kind === "directory" ? directory(`${path}/${name}`) : file(`${path}/${name}`);
          entries.set(name, item);
          faults.created?.(`${path}/${name}`, item);
        }
        return item;
      }
      return node;
    }
    function file(path) {
      const node = {
        kind: "file",
        data: new Uint8Array(),
        isSameEntry: async (other) => other === node,
        async getFile() {
          return new Blob([node.data]);
        },
        async createWritable() {
          events.push(["open", path]);
          const pieces = [];
          return {
            async write(bytes) {
              events.push(["write", path, bytes.length]);
              faults.write?.(path, bytes);
              pieces.push(bytes.slice());
            },
            async close() {
              const data = new Uint8Array(Buffer.concat(pieces));
              faults.close?.(path, data);
              node.data = data;
              events.push(["close", path]);
            },
            async abort() {
              faults.abort?.(path);
              events.push(["abort", path]);
            },
          };
        },
      };
      return node;
    }
    const root = directory();
    root.entries.set("unrelated", file("/unrelated"));
    const content = Uint8Array.from({ length: 70003 }, (_, i) => i & 255);
    const pack = { name: "00-00000008.data", size: content.length, sha256: "a".repeat(64) };
    const finalizedPack = {
      pack,
      source: {
        size: content.length,
        file: new Blob([content]),
        async readAt(offset, length) {
          assert.ok(length <= 65536);
          faults.read?.(offset, length);
          return faults.short ? new Uint8Array() : content.slice(offset, offset + length);
        },
      },
    };
    const options = {
      finalizedPack,
      outputDirectoryHandle: root,
      secureContext: true,
      locks: resourceOutputLocks(),
    };
    return { root, events, content, pack, options };
  }
  test("exports native manifest and archive to fresh renamed directory with committed marker", async () => {
    const fs = fixture(),
      progress = [];
    const result = await exportLegacyDevicePack({
      ...fs.options,
      onProgress: (value) => progress.push(value),
    });
    assert.equal(result.status, "complete");
    assert.equal(result.filesWritten, 2);
    assert.equal(result.bytesWritten, fs.content.length);
    const folder = fs.root.entries.get(folderName);
    assert.deepEqual(folder.entries.get("data").entries.get(fs.pack.name).data, fs.content);
    assert.deepEqual(
      JSON.parse(new TextDecoder().decode(folder.entries.get("manifest.json").data)),
      {
        files: [fs.pack],
      },
    );
    assert.equal(
      JSON.parse(new TextDecoder().decode(folder.entries.get(MARKER).data)).status,
      "complete",
    );
    assert.equal(progress.at(-1).state, "complete");
    assert.ok(fs.root.entries.has("unrelated"));
  });
  test("invalid metadata/source/signal and denied permission never overwrite", async () => {
    for (const mode of ["pack", "source", "signal", "permission"]) {
      const fs = fixture(mode === "permission" ? { permission: "denied" } : {});
      if (mode === "pack") fs.options.finalizedPack.pack = { ...fs.pack, name: "../bad" };
      if (mode === "source") fs.options.finalizedPack.source.size++;
      if (mode === "signal") fs.options.signal = {};
      await assert.rejects(
        exportLegacyDevicePack(fs.options),
        (error) => error instanceof LegacyPackExportError && error.report.cleanupComplete,
      );
      assert.equal(
        fs.events.some((e) => e[0] === "write" || e[0] === "remove"),
        false,
      );
      assert.ok(fs.root.entries.has("unrelated"));
    }
  });
  test("simple numbered legacy folders preserve every earlier file and directory", async () => {
    const fs = fixture();
    const first = await exportLegacyDevicePack(fs.options);
    const original = fs.root.entries.get(folderName);
    const collision = { kind: "file", private: "untouched" };
    fs.root.entries.set(`${folderName}-2`, collision);
    const next = await exportLegacyDevicePack(fs.options);
    assert.equal(first.directoryName, folderName);
    assert.equal(next.directoryName, `${folderName}-3`);
    assert.equal(fs.root.entries.get(folderName), original);
    assert.equal(fs.root.entries.get(`${folderName}-2`), collision);
    assert.equal(fs.events.filter(([operation]) => operation === "remove").length, 0);
  });
  test("read/write/close failures remove only owned output and report partial counts", async () => {
    for (const mode of ["read", "write", "close", "short"]) {
      const faults =
        mode === "short"
          ? { short: true }
          : {
              [mode]: (path) => {
                if (mode === "read" || path.endsWith(".data")) throw new Error("disk failure");
              },
            };
      const fs = fixture(faults);
      await assert.rejects(exportLegacyDevicePack(fs.options), (error) => {
        assert.ok(error.report.cleanupComplete);
        assert.equal(error.report.status, "incomplete");
        return true;
      });
      assert.deepEqual([...fs.root.entries.keys()], ["unrelated"]);
    }
  });
  test("abort after acquiring owned file cleans it; final marker close is commit boundary", async () => {
    const controller = new AbortController();
    const fs = fixture({
      created: (path) => {
        if (path.endsWith(".data")) controller.abort();
      },
    });
    await assert.rejects(
      exportLegacyDevicePack({ ...fs.options, signal: controller.signal }),
      (error) => error.report.code === "CANCELLED" && error.report.cleanupComplete,
    );
    const after = new AbortController();
    const done = fixture({
      close: (path, bytes) => {
        if (
          path.endsWith(MARKER) &&
          JSON.parse(new TextDecoder().decode(bytes)).status === "complete"
        )
          after.abort();
      },
    });
    assert.equal(
      (await exportLegacyDevicePack({ ...done.options, signal: after.signal })).status,
      "complete",
    );
  });
  test("cleanup failure preserves incomplete marker and reports remaining output", async () => {
    const fs = fixture({
      write: (path) => {
        if (path.endsWith(".data")) throw new Error("write failed");
      },
      remove: (path) => {
        if (path.endsWith(".data")) throw new Error("remove denied");
      },
    });
    await assert.rejects(exportLegacyDevicePack(fs.options), (error) => {
      assert.equal(error.report.cleanupComplete, false);
      assert.equal(error.report.incompleteMarkerPresent, true);
      assert.ok(error.report.cleanupCodes.includes("LEGACY_PACK_ENTRY_CLEANUP_FAILED"));
      return true;
    });
    assert.equal(
      JSON.parse(new TextDecoder().decode(fs.root.entries.get(folderName).entries.get(MARKER).data))
        .status,
      "incomplete",
    );
  });
  test("unknown additions and replaced handles survive nonrecursive cleanup", async () => {
    for (const replace of [false, true]) {
      let fs;
      fs = fixture({
        write: (path) => {
          if (!path.endsWith(".data")) return;
          const data = fs.root.entries.get(folderName).entries.get("data");
          if (replace)
            data.entries.set(fs.pack.name, { kind: "file", isSameEntry: async () => false });
          else data.entries.set("user-added", { kind: "file" });
          throw new Error("stop");
        },
      });
      await assert.rejects(
        exportLegacyDevicePack(fs.options),
        (error) => !error.report.cleanupComplete && error.report.incompleteMarkerPresent,
      );
      const data = fs.root.entries.get(folderName).entries.get("data");
      assert.ok(data.entries.has(replace ? fs.pack.name : "user-added"));
    }
  });
  test("progress observer errors do not change successful export", async () => {
    const fs = fixture();
    assert.equal(
      (
        await exportLegacyDevicePack({
          ...fs.options,
          onProgress: () => Promise.reject(new Error("observer")),
        })
      ).status,
      "complete",
    );
  });

  test("manifest and final-marker close failures never report completion", async () => {
    for (const target of ["manifest.json", "complete-marker"]) {
      const fs = fixture({
        close: (path, bytes) => {
          if (
            path.endsWith(target) ||
            (target === "complete-marker" &&
              path.endsWith(MARKER) &&
              JSON.parse(new TextDecoder().decode(bytes)).status === "complete")
          )
            throw new Error("close failed");
        },
      });
      await assert.rejects(
        exportLegacyDevicePack(fs.options),
        (error) => error.report.status === "incomplete" && error.report.cleanupComplete,
      );
      assert.deepEqual([...fs.root.entries.keys()], ["unrelated"]);
    }
  });
  test("unsafe source backing buffers reject and secure context is required", async () => {
    const fs = fixture();
    const borrowed = new Uint8Array(65537).fill(23);
    fs.options.finalizedPack.source.readAt = async (_, count) => borrowed.subarray(0, count);
    await assert.rejects(
      exportLegacyDevicePack(fs.options),
      (error) => /READ_RANGE/.test(error.cause.message) && error.report.cleanupComplete,
    );
    assert.ok(borrowed.every((byte) => byte === 23));
    const insecure = fixture();
    await assert.rejects(
      exportLegacyDevicePack({ ...insecure.options, secureContext: false }),
      (error) => /SECURE_CONTEXT/.test(error.cause.message),
    );
    assert.equal(insecure.events.length, 0);
  });
  test("cancelled and short reads wipe bounded owned bytes before export cleanup", async () => {
    for (const mode of ["cancel", "short"]) {
      const fs = fixture(),
        controller = new AbortController();
      const original = fs.content.slice();
      let returned;
      fs.options.finalizedPack.source.readAt = async (_, count) => {
        returned = new Uint8Array(mode === "short" ? count - 1 : count).fill(19);
        if (mode === "cancel") controller.abort();
        return returned;
      };
      await assert.rejects(
        exportLegacyDevicePack({ ...fs.options, signal: controller.signal }),
        (error) => {
          assert.equal(
            error.report.code,
            mode === "cancel" ? "CANCELLED" : "LEGACY_PACK_EXPORT_FAILED",
          );
          assert.equal(error.report.cleanupComplete, true);
          if (mode === "short") assert.match(error.cause.message, /READ_RANGE/);
          return true;
        },
      );
      assert.ok(returned.every((byte) => byte === 0));
      assert.deepEqual(fs.content, original);
      assert.deepEqual([...fs.root.entries.keys()], ["unrelated"]);
    }
  });

  test("abort during streaming reports written bytes and preserves no owned leftovers", async () => {
    const controller = new AbortController();
    const fs = fixture({
      write: (path) => {
        if (path.endsWith(".data")) controller.abort();
      },
    });
    await assert.rejects(
      exportLegacyDevicePack({ ...fs.options, signal: controller.signal }),
      (error) =>
        error.report.code === "CANCELLED" &&
        error.report.bytesWritten === fs.content.length &&
        error.report.cleanupComplete,
    );
    assert.deepEqual([...fs.root.entries.keys()], ["unrelated"]);
  });

  for (const preset of ["thorough", "fast"])
    test(`exports optional ${preset} provenance without changing native pack fields`, async () => {
      const fs = fixture();
      fs.options.finalizedPack.encoding = { format: "astc", preset };
      const result = await exportLegacyDevicePack(fs.options);
      const manifest = JSON.parse(
        new TextDecoder().decode(fs.root.entries.get(folderName).entries.get("manifest.json").data),
      );
      assert.deepEqual(manifest, { files: [fs.pack], encoding: { format: "astc", preset } });
      assert.deepEqual(result.encoding, { format: "astc", preset });
      assert.ok(Object.isFrozen(result.encoding));
    });
  test("invalid encoding provenance rejects before creating export files", async () => {
    for (const encoding of [
      null,
      {},
      { format: "astc", preset: "other" },
      { format: "other", preset: "fast" },
      { format: "astc", preset: "fast", authenticated: true },
    ]) {
      const fs = fixture();
      fs.options.finalizedPack.encoding = encoding;
      await assert.rejects(
        exportLegacyDevicePack(fs.options),
        (error) => error.cause.message === "LEGACY_PACK_INVALID_ENCODING",
      );
      assert.ok(!fs.root.entries.has(folderName));
    }
  });

  // abort 拒绝不证明流已停止；禁止删除可能仍被流使用的目标。
  // A rejected abort does not prove the stream stopped; never remove targets the stream may still use.
  test("failed writer abort preserves owned output instead of deleting ahead of a live stream", async () => {
    const primary = new Error("write failed");
    const fs = fixture({
      write(path) {
        if (path.endsWith(".data")) throw primary;
      },
      abort() {
        throw new Error("writer still active");
      },
    });
    await assert.rejects(exportLegacyDevicePack(fs.options), (error) => {
      assert.equal(error.cause, primary);
      assert.equal(error.report.cleanupComplete, false);
      assert.equal(error.report.incompleteMarkerPresent, true);
      assert.deepEqual(error.report.cleanupCodes, ["LEGACY_PACK_WRITER_ABORT_FAILED"]);
      return true;
    });
    assert.equal(fs.events.filter(([type]) => type === "remove").length, 0);
    assert.ok(fs.root.entries.get(folderName).entries.get("data").entries.has(fs.pack.name));
  });
}

// raw-file-stage
{
  // 只用内存文件系统替身测试原始文件暂存的边界、所有权和错误；不构成真实浏览器证明。
  // Test raw-stage bounds, ownership and failures with an in-memory filesystem double, not real-browser proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createRawFileStage, RAW_FILE_STAGE_CHUNK_BYTES } = rawFileStageModule;
  const uuid = "12345678-1234-4123-8123-123456789abc";
  const name = `d2rm-raw-${uuid}`;
  const notFound = () => new DOMException("missing", "NotFoundError");
  function filesystem(faults = {}) {
    const entries = new Map([["unrelated", { keep: true }]]),
      events = [];
    let bytes = new Uint8Array(),
      closed = false;
    const handle = {
      async createWritable(options) {
        events.push(["open", options]);
        if (faults.open) throw faults.open;
        return {
          async write({ type, position, data }) {
            events.push(["write", position, data.length]);
            assert.equal(type, "write");
            assert.ok(data.length <= RAW_FILE_STAGE_CHUNK_BYTES);
            assert.ok(position <= bytes.length);
            await faults.onWrite?.();
            if (faults.write) throw faults.write;
            const next = new Uint8Array(Math.max(bytes.length, position + data.length));
            next.set(bytes);
            next.set(data, position);
            bytes = next;
          },
          async close() {
            events.push(["close"]);
            await faults.onClose?.();
            if (faults.close) throw faults.close;
            closed = true;
          },
          async abort() {
            events.push(["abort"]);
            if (faults.abort) throw faults.abort;
          },
        };
      },
      async getFile() {
        events.push(["getFile"]);
        assert.equal(closed, true);
        if (faults.file) throw faults.file;
        const blob = new Blob([faults.changedSize ? new Uint8Array(bytes.length + 1) : bytes]);
        if (!faults.read && !faults.shortRead && !faults.onRead && !faults.onReadBuffer)
          return blob;
        return {
          size: blob.size,
          slice(start, end) {
            assert.ok(end - start <= RAW_FILE_STAGE_CHUNK_BYTES);
            return {
              async arrayBuffer() {
                await faults.onRead?.();
                if (faults.read) throw faults.read;
                const buffer = await blob
                  .slice(start, faults.shortRead ? Math.max(start, end - 1) : end)
                  .arrayBuffer();
                faults.onReadBuffer?.(buffer);
                return buffer;
              },
            };
          },
        };
      },
    };
    const root = {
      kind: "directory",
      async getDirectoryHandle(key, options) {
        events.push(["directory", key, options?.create ?? false]);
        if (!options?.create) {
          if (!entries.has(key)) throw notFound();
          return entries.get(key);
        }
        const child = {
          async getFileHandle(fileName, opts) {
            assert.equal(fileName, "data.bin");
            assert.deepEqual(opts, { create: true });
            if (faults.childFile) throw faults.childFile;
            return handle;
          },
        };
        entries.set(key, child);
        faults.onCreate?.();
        return child;
      },
      async removeEntry(key, options) {
        events.push(["remove", key, options]);
        assert.equal(key, name);
        assert.deepEqual(options, { recursive: true });
        if (faults.remove) throw faults.remove;
        if (!entries.delete(key)) throw notFound();
      },
    };
    return {
      root,
      entries,
      events,
      make: (options = {}) =>
        createRawFileStage({
          root,
          maxBytes: 2 * RAW_FILE_STAGE_CHUNK_BYTES,
          randomUUID: () => uuid,
          ...options,
        }),
    };
  }

  test("sequential appends and bounded backpatch produce replayable closed snapshot", async () => {
    const fs = filesystem();
    const stage = await fs.make();
    await stage.append(Uint8Array.of(1, 2, 3));
    await stage.backpatch(1, Uint8Array.of(8));
    await stage.append(Uint8Array.of(4, 5));
    const source = await stage.finalize();
    assert.equal(source.size, 5);
    assert.equal(source.file.size, 5);
    for (let i = 0; i < 2; i++)
      assert.deepEqual(await source.readAt(0, 5), Uint8Array.of(1, 8, 3, 4, 5));
    assert.deepEqual(await source.readAt(5, 0), new Uint8Array());
    assert.ok(
      fs.events.findIndex((event) => event[0] === "close") <
        fs.events.findIndex((event) => event[0] === "getFile"),
    );
    await stage.dispose();
    await stage.dispose();
    assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
    await assert.rejects(source.readAt(0, 1), /DISPOSED/);
    await assert.rejects(stage.append(Uint8Array.of(9)), /DISPOSED/);
  });

  test("limits, root and UUID reject before ownership or file writes", async () => {
    for (const maxBytes of [undefined, -1, 1.5, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1]) {
      const fs = filesystem();
      await assert.rejects(fs.make({ maxBytes }), /INVALID_LIMIT/);
      assert.equal(fs.events.length, 0);
    }
    await assert.rejects(createRawFileStage({ maxBytes: 1 }), /ROOT_REQUIRED/);
    const fs = filesystem();
    await assert.rejects(fs.make({ randomUUID: () => "../bad" }), /INVALID_ID/);
    assert.equal(fs.events.length, 0);
  });

  test("zero budget and exact chunk/budget limits work without sparse gaps", async () => {
    const empty = await filesystem().make({ maxBytes: 0 });
    assert.equal((await empty.finalize()).size, 0);
    await empty.dispose();
    const stage = await filesystem().make({ maxBytes: RAW_FILE_STAGE_CHUNK_BYTES });
    await stage.append(new Uint8Array(RAW_FILE_STAGE_CHUNK_BYTES));
    await stage.backpatch(RAW_FILE_STAGE_CHUNK_BYTES, new Uint8Array());
    const source = await stage.finalize();
    assert.equal(
      (await source.readAt(0, RAW_FILE_STAGE_CHUNK_BYTES)).length,
      RAW_FILE_STAGE_CHUNK_BYTES,
    );
    await stage.dispose();
    for (const operation of [
      (s) => s.append(new Uint8Array(RAW_FILE_STAGE_CHUNK_BYTES + 1)),
      (s) => s.append(new Uint8Array(5)),
      (s) => s.backpatch(1, Uint8Array.of(1)),
      (s) => s.backpatch(-1, new Uint8Array()),
      (s) => s.backpatch(0.5, new Uint8Array()),
      (s) => s.append([1]),
    ]) {
      const fs = filesystem(),
        s = await fs.make({ maxBytes: 4 });
      await assert.rejects(operation(s), /RAW_STAGE_/);
      assert.equal(
        fs.events.some((event) => event[0] === "write"),
        false,
      );
      await s.dispose();
      assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
    }
  });

  test("read ranges reject over-limit, fractional and out-of-bounds reads", async () => {
    for (const [offset, length] of [
      [0, RAW_FILE_STAGE_CHUNK_BYTES + 1],
      [-1, 1],
      [0.5, 1],
      [0, -1],
      [2, 0],
      [0, 2],
    ]) {
      const stage = await filesystem().make();
      await stage.append(Uint8Array.of(1));
      const source = await stage.finalize();
      await assert.rejects(source.readAt(offset, length), /READ_RANGE/);
      await stage.dispose();
    }
  });

  test("existing child is never reused or removed", async () => {
    const fs = filesystem();
    fs.entries.set(name, { existing: true });
    await assert.rejects(fs.make(), /COLLISION/);
    assert.deepEqual(fs.entries.get(name), { existing: true });
    assert.equal(
      fs.events.some((event) => event[0] === "remove"),
      false,
    );
  });

  test("creation, quota, write, close and snapshot failures clean only owned child", async () => {
    for (const field of ["childFile", "open", "write", "close", "file", "changedSize"]) {
      const cause = new DOMException(
        field,
        field === "write" ? "QuotaExceededError" : "OperationError",
      );
      const fs = filesystem({ [field]: field === "changedSize" ? true : cause });
      let stage;
      await assert.rejects(
        async () => {
          stage = await fs.make();
          await stage.append(Uint8Array.of(1));
          await stage.finalize();
        },
        field === "changedSize" ? /SIZE_CHANGED/ : (error) => error === cause,
      );
      await stage?.dispose();
      assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
    }
  });

  test("cleanup failure is observable and dispose retries owned resources", async () => {
    const faults = { remove: new Error("remove failed") };
    const fs = filesystem(faults),
      stage = await fs.make();
    await assert.rejects(stage.dispose(), /CLEANUP_FAILED/);
    assert.ok(fs.entries.has(name));
    delete faults.remove;
    await stage.dispose();
    assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
  });

  test("failed writer abort retains the stage until a successful dispose retry", async () => {
    const faults = { abort: new Error("abort failed") };
    const fs = filesystem(faults),
      stage = await fs.make();
    await stage.append(Uint8Array.of(1, 2, 3));
    await assert.rejects(stage.dispose(), (error) => {
      assert.match(error.message, /CLEANUP_FAILED/);
      assert.deepEqual(error.errors, [faults.abort]);
      return true;
    });
    assert.ok(fs.entries.has(name));
    assert.equal(fs.events.filter((event) => event[0] === "remove").length, 0);
    delete faults.abort;
    await stage.dispose();
    assert.equal(fs.events.filter((event) => event[0] === "abort").length, 2);
    assert.equal(fs.events.filter((event) => event[0] === "remove").length, 1);
    assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
    await stage.dispose();
    assert.equal(fs.events.filter((event) => event[0] === "remove").length, 1);
  });

  test("abort before and during creation or writes cleans safely", async () => {
    const controller = new AbortController();
    controller.abort();
    const pre = filesystem();
    await assert.rejects(pre.make({ signal: controller.signal }), { name: "AbortError" });
    assert.equal(pre.events.length, 0);
    for (const phase of ["onCreate", "onWrite", "onClose"]) {
      const ctl = new AbortController();
      const fs = filesystem({ [phase]: () => ctl.abort() });
      let stage;
      await assert.rejects(
        async () => {
          stage = await fs.make({ signal: ctl.signal });
          await stage.append(Uint8Array.of(1));
          await stage.finalize();
        },
        { name: "AbortError" },
      );
      await stage?.dispose();
      assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
    }
  });

  test("concurrent writes reject and disposal drains the pending browser call", async () => {
    let unblock, started;
    const entered = new Promise((resolve) => {
      started = resolve;
    });
    const wait = new Promise((resolve) => {
      unblock = resolve;
    });
    const fs = filesystem({
      onWrite: () => {
        started();
        return wait;
      },
    });
    const stage = await fs.make();
    const writing = stage.append(Uint8Array.of(1));
    await entered;
    await assert.rejects(stage.append(Uint8Array.of(2)), /CONCURRENT_OPERATION/);
    const disposed = stage.dispose();
    assert.equal(
      fs.events.some((event) => event[0] === "remove"),
      false,
    );
    unblock();
    await assert.rejects(writing, /DISPOSED/);
    await disposed;
    assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
  });

  test("finalize is one-shot and writes after finalization reject", async () => {
    for (const operation of [
      (s) => s.finalize(),
      (s) => s.append(Uint8Array.of(1)),
      (s) => s.backpatch(0, new Uint8Array()),
    ]) {
      const stage = await filesystem().make();
      await stage.finalize();
      await assert.rejects(operation(stage), /NOT_WRITABLE/);
      await stage.dispose();
    }
  });

  test("read errors, short reads and cancellation reject and remove only the owned child", async () => {
    for (const mode of ["read", "shortRead", "abort"]) {
      const controller = new AbortController();
      const reason = new Error("read failed");
      const faults =
        mode === "abort"
          ? { onRead: () => controller.abort() }
          : { [mode]: mode === "read" ? reason : true };
      const fs = filesystem(faults);
      const stage = await fs.make({ signal: controller.signal });
      await stage.append(Uint8Array.of(1, 2));
      const source = await stage.finalize();
      await assert.rejects(
        source.readAt(0, 2),
        mode === "abort"
          ? { name: "AbortError" }
          : mode === "read"
            ? (error) => error === reason
            : /SHORT_READ/,
      );
      await stage.dispose();
      assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
    }
  });

  test("operation and cleanup errors retain both causes and support later cleanup retry", async () => {
    const write = new Error("disk failed"),
      abort = new Error("abort failed"),
      remove = new Error("remove failed");
    const faults = { write, abort, remove };
    const fs = filesystem(faults),
      stage = await fs.make();
    await assert.rejects(stage.append(Uint8Array.of(1)), (error) => {
      assert.equal(error.cause, write);
      assert.equal(error.errors[0], write);
      assert.deepEqual(error.errors[1].errors, [abort]);
      return true;
    });
    assert.equal(
      fs.events.some((event) => event[0] === "remove"),
      false,
    );
    assert.ok(fs.entries.has(name));
    delete faults.abort;
    await assert.rejects(stage.dispose(), (error) => {
      assert.deepEqual(error.errors, [remove]);
      return true;
    });
    assert.ok(fs.entries.has(name));
    delete faults.remove;
    await stage.dispose();
    assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
  });

  test("idle abort triggers cleanup and source reads reject afterward", async () => {
    const controller = new AbortController(),
      fs = filesystem();
    const stage = await fs.make({ signal: controller.signal });
    const source = await stage.finalize();
    controller.abort();
    await stage.dispose();
    await assert.rejects(source.readAt(0, 0), { name: "AbortError" });
    assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
  });

  test("non-AbortSignal inputs reject before filesystem access", async () => {
    for (const signal of [
      null,
      false,
      "signal",
      {},
      new AbortController(),
      {
        throwIfAborted() {},
        addEventListener() {},
        removeEventListener() {},
      },
    ]) {
      const fs = filesystem();
      await assert.rejects(fs.make({ signal }), /INVALID_SIGNAL/);
      assert.equal(fs.events.length, 0);
    }
  });

  test("small views cannot retain oversized, shared or resizable backing buffers", async () => {
    const resizable = new ArrayBuffer(8, { maxByteLength: 16 });
    assert.equal(resizable.resizable, true);
    for (const bytes of [
      new Uint8Array(new ArrayBuffer(RAW_FILE_STAGE_CHUNK_BYTES + 1), 0, 1),
      new Uint8Array(new SharedArrayBuffer(8), 0, 1),
      new Uint8Array(resizable, 0, 1),
    ]) {
      for (const method of ["append", "backpatch"]) {
        const fs = filesystem(),
          stage = await fs.make();
        await stage.append(Uint8Array.of(1));
        const before = fs.events.filter((event) => event[0] === "write").length;
        await assert.rejects(
          method === "append" ? stage.append(bytes) : stage.backpatch(0, bytes),
          /INVALID_CHUNK/,
        );
        assert.equal(fs.events.filter((event) => event[0] === "write").length, before);
        await stage.dispose();
        assert.deepEqual([...fs.entries.keys()], ["unrelated"]);
      }
    }
  });

  // 失败的读取副本尚未转交调用方；成功读取和空读取保持原有所有权。
  // Failed read copies have not been handed to callers; successful and empty reads retain their ownership contract.
  test("raw stage wipes unreturned read buffers on cancellation and short reads", async () => {
    for (const mode of ["abort", "shortRead"]) {
      const controller = new AbortController();
      let captured;
      const fs = filesystem({
        shortRead: mode === "shortRead",
        onReadBuffer(buffer) {
          captured = new Uint8Array(buffer);
          if (mode === "abort") controller.abort();
        },
      });
      const stage = await fs.make({ signal: controller.signal });
      await stage.append(Uint8Array.of(7, 8, 9));
      const source = await stage.finalize();
      await assert.rejects(
        source.readAt(0, 3),
        mode === "abort" ? { name: "AbortError" } : /RAW_STAGE_SHORT_READ/,
      );
      assert.ok(captured.length > 0);
      assert.ok(captured.every((value) => value === 0));
      await stage.dispose();
      assert.equal(fs.entries.has(name), false);
      assert.equal(fs.entries.has("unrelated"), true);
    }
  });

  test("successful raw stage reads remain caller-owned after disposal, including empty reads", async () => {
    const fs = filesystem();
    const stage = await fs.make();
    await stage.append(Uint8Array.of(7, 8));
    const source = await stage.finalize();
    const bytes = await source.readAt(0, 2);
    const empty = await source.readAt(2, 0);
    await stage.dispose();
    assert.deepEqual([...bytes], [7, 8]);
    assert.equal(empty.length, 0);
  });
}

// resource-data-reuse
{
  // 只使用合成字节，证明有界只读复用，而非游戏/设备兼容性。
  // Synthetic bytes only: these tests establish bounded, read-only reuse, not game/device compatibility.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { verifyResourceData } = resourceDataReuseModule;
  const { exportRomfsResources, RESOURCE_EXPORT_MARKER } = resourceFolderExportModule;
  const { withResourceOutputLock } = resourceOutputLockModule;
  const { resourceOutputLocks } = resourceOutputLocksModule;
  const { readFile } = promisesModule;

  function fixture() {
    const owned = [],
      progress = [],
      reads = [],
      operations = [];
    const source = new Map([
      ["archive.data", Uint8Array.from({ length: 70003 }, (_, i) => i % 251)],
      ["nested/manifest.txt", new TextEncoder().encode("synthetic current update")],
    ]);
    function file(bytes) {
      const handle = {
        kind: "file",
        bytes,
        isSameEntry: async (other) => other === handle,
        getFile: async () => new Blob([handle.bytes]),
        createWritable() {
          operations.push("write");
          throw Error("must not write");
        },
      };
      return handle;
    }
    function directory() {
      const children = new Map();
      const handle = {
        kind: "directory",
        children,
        queryPermission: async () => "granted",
        isSameEntry: async (other) => other === handle,
        async *entries() {
          yield* children;
        },
        async getFileHandle(name, options) {
          return get(name, "file", options);
        },
        async getDirectoryHandle(name, options) {
          return get(name, "directory", options);
        },
        removeEntry() {
          operations.push("remove");
          throw Error("must not remove");
        },
      };
      function get(name, kind, options) {
        if (options?.create) {
          operations.push("create");
          throw Error("must not create");
        }
        const value = children.get(name);
        if (!value) throw new DOMException("missing", "NotFoundError");
        if (value.kind !== kind) throw new DOMException("kind", "TypeMismatchError");
        return value;
      }
      return handle;
    }
    const data = directory(),
      nested = directory(),
      root = directory();
    data.children.set("archive.data", file(source.get("archive.data").slice()));
    nested.children.set("manifest.txt", file(source.get("nested/manifest.txt").slice()));
    data.children.set("nested", nested);
    data.children.set(
      RESOURCE_EXPORT_MARKER,
      file(new TextEncoder().encode('{"status":"complete","untrusted":true}')),
    );
    root.children.set("data", data);
    const entries = [...source].map(([path, bytes]) => ({
      path,
      sourcePath: `data/${path}`,
      size: BigInt(bytes.length),
    }));
    const romfs = {
      getFile(path) {
        const bytes = source.get(path.slice("data/".length));
        return (
          bytes && {
            size: BigInt(bytes.length),
            async readAt(offset, count) {
              assert.ok(count <= 65536);
              reads.push([offset, count]);
              const copy = bytes.slice(Number(offset), Number(offset) + count);
              owned.push(copy);
              return copy;
            },
          }
        );
      },
    };
    const options = {
      directory: data,
      entries,
      romfs,
      markerName: RESOURCE_EXPORT_MARKER,
      onProgress: (value) => progress.push(value),
    };
    return {
      root,
      data,
      nested,
      options,
      operations,
      reads,
      owned,
      progress,
      file,
      directory,
      source,
    };
  }

  test("existing data is compared completely in bounded chunks without any writes or trusting its marker", async () => {
    const f = fixture(),
      marker = f.data.children.get(RESOURCE_EXPORT_MARKER);
    const result = await verifyResourceData(f.options);
    assert.deepEqual(result, { filesVerified: 2, bytesVerified: 70027n });
    assert.deepEqual(f.progress.at(-1), result);
    assert.equal(f.reads.length, 3);
    assert.ok(f.owned.every((bytes) => bytes.every((byte) => byte === 0)));
    assert.equal(f.data.children.get(RESOURCE_EXPORT_MARKER), marker);
    assert.deepEqual(f.operations, []);
  });

  test("missing, extra, wrong-size and same-size changed files cannot reuse a claimed-complete folder", async () => {
    for (const mode of ["missing", "extra", "size", "bytes", "empty-dir", "marker-dir"]) {
      const f = fixture();
      if (mode === "missing") f.nested.children.delete("manifest.txt");
      if (mode === "extra") f.data.children.set("foreign.data", f.file(new Uint8Array()));
      if (mode === "size") f.data.children.get("archive.data").bytes = new Uint8Array(3);
      if (mode === "bytes") f.data.children.get("archive.data").bytes[65536] ^= 1;
      if (mode === "empty-dir") f.data.children.set("foreign", f.directory());
      if (mode === "marker-dir") f.data.children.set(RESOURCE_EXPORT_MARKER, f.directory());
      const before = [...f.data.children];
      await assert.rejects(verifyResourceData(f.options), /RESOURCE_EXISTING_DATA_UNVERIFIED/);
      assert.deepEqual([...f.data.children], before);
      assert.deepEqual(f.operations, []);
      assert.ok(f.owned.every((bytes) => bytes.every((byte) => byte === 0)));
    }
  });

  test("cancellation, short source reads and replaced handles cannot finish verification", async () => {
    for (const mode of ["cancel", "short", "replace"]) {
      const f = fixture(),
        controller = new AbortController(),
        original = f.options.romfs.getFile;
      f.options.romfs.getFile = (path) => {
        const source = original(path),
          read = source.readAt;
        source.readAt = async (...args) => {
          const bytes = await read(...args);
          if (mode === "cancel") controller.abort();
          if (mode === "short") return bytes.subarray(1);
          if (mode === "replace")
            f.data.children.set("archive.data", f.file(f.source.get("archive.data").slice()));
          return bytes;
        };
        return source;
      };
      await assert.rejects(verifyResourceData({ ...f.options, signal: controller.signal }));
      assert.deepEqual(f.operations, []);
      if (mode !== "short") assert.ok(f.owned.every((bytes) => bytes.every((byte) => byte === 0)));
    }
  });

  test("exporter preserves mismatching input/data instead of treating it as owned cleanup", async (t) => {
    const prior = Object.getOwnPropertyDescriptor(globalThis, "isSecureContext");
    Object.defineProperty(globalThis, "isSecureContext", { value: true, configurable: true });
    t.after(() => {
      if (prior) Object.defineProperty(globalThis, "isSecureContext", prior);
      else delete globalThis.isSecureContext;
    });
    const f = fixture();
    const text = await readFile(new URL("../resource-folder-export.mjs", import.meta.url), "utf8");
    f.options.romfs.files = [...text.matchAll(/\["([\d-]+\.data)", (\d+)n\]/g)].map(
      ([, name, size]) => ({ path: `data/${name}`, size: BigInt(size) }),
    );
    assert.equal(f.options.romfs.files.length, 18);
    await assert.rejects(
      exportRomfsResources({
        romfs: f.options.romfs,
        outputDirectoryHandle: f.root,
        locks: resourceOutputLocks(),
      }),
      (error) => {
        assert.equal(error.report.directoryName, "data");
        assert.equal(error.report.existingDataPreserved, true);
        assert.equal(error.report.reason, "RESOURCE_EXISTING_DATA_UNVERIFIED");
        return true;
      },
    );
    assert.equal(f.root.children.get("data"), f.data);
    assert.deepEqual(f.operations, []);
  });

  test("same-origin exports are exclusive through completion and lock failures never touch files", async () => {
    const locks = resourceOutputLocks();
    let release,
      entered = false;
    const first = withResourceOutputLock(
      locks,
      undefined,
      () =>
        new Promise((resolve) => {
          entered = true;
          release = resolve;
        }),
    );
    assert.ok(entered);
    await assert.rejects(
      withResourceOutputLock(locks, undefined, () => assert.fail("must not run")),
      /RESOURCE_EXPORT_BUSY/,
    );
    release("complete");
    assert.equal(await first, "complete");
    assert.equal(await withResourceOutputLock(locks, undefined, () => "next"), "next");
    const controller = new AbortController();
    controller.abort();
    await assert.rejects(
      withResourceOutputLock(locks, controller.signal, () => assert.fail("must not run")),
      { name: "AbortError" },
    );
    await assert.rejects(
      exportRomfsResources({ locks: {} }),
      (error) =>
        error.report.reason === "RESOURCE_EXPORT_LOCK_UNAVAILABLE" &&
        error.report.directoryName === null,
    );
  });
}

// resource-folder-cleanup
{
  // 合成目录/读取器只验证清理所有权，不证明原生验收表或真实资源内容。
  // Synthetic directories/readers test cleanup ownership, not native acceptance tables or real resource content.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { exportRomfsResources, RESOURCE_EXPORT_MARKER } = resourceFolderExportModule;
  const { resourceOutputLocks } = resourceOutputLocksModule;
  const moduleSource = await readFile(
    new URL("../resource-folder-export.mjs", import.meta.url),
    "utf8",
  );
  const files = [...moduleSource.matchAll(/\["([\d-]+\.data)", (\d+)n\]/g)].map(
    ([, name, size]) => ({
      path: `data/${name}`,
      size: BigInt(size),
    }),
  );
  assert.equal(files.length, 18);
  function fixture(abortFails) {
    const events = [];
    const primary = Error("synthetic read failure");
    function dir() {
      const entries = new Map();
      const handle = {
        kind: "directory",
        entries,
        queryPermission: async () => "granted",
        isSameEntry: async (other) => other === handle,
        getDirectoryHandle: async (name, options) => get(name, options, true),
        getFileHandle: async (name, options) => get(name, options, false),
        async removeEntry(name, options) {
          assert.equal(options, undefined);
          events.push(name);
          const child = entries.get(name);
          if (child?.entries?.size) throw Error("not empty");
          entries.delete(name);
        },
      };
      function get(name, options, directory) {
        if (!entries.has(name)) {
          if (!options?.create) throw new DOMException("missing", "NotFoundError");
          entries.set(name, directory ? dir() : file());
        }
        return entries.get(name);
      }
      return handle;
    }
    function file() {
      const handle = {
        kind: "file",
        isSameEntry: async (other) => other === handle,
        async createWritable() {
          return {
            write: async () => {},
            close: async () => {},
            abort: async () => {
              if (abortFails) throw Error("still active");
            },
          };
        },
      };
      return handle;
    }
    const root = dir();
    const romfs = {
      files,
      getFile(path) {
        const entry = files.find((file) => file.path === path);
        return {
          size: entry.size,
          readAt: async () => {
            throw primary;
          },
        };
      },
    };
    return { root, romfs, primary, events };
  }
  for (const abortFails of [true, false])
    test(`resource writer abort ${abortFails ? "failure retains" : "success removes"} owned output`, async () => {
      const prior = Object.getOwnPropertyDescriptor(globalThis, "isSecureContext");
      Object.defineProperty(globalThis, "isSecureContext", { value: true, configurable: true });
      const fs = fixture(abortFails);
      try {
        await assert.rejects(
          exportRomfsResources({
            romfs: fs.romfs,
            outputDirectoryHandle: fs.root,
            locks: resourceOutputLocks(),
          }),
          (error) => {
            assert.equal(error.cause, fs.primary);
            assert.equal(error.report.cleanupComplete, !abortFails);
            assert.equal(error.report.incompleteMarkerPresent, abortFails);
            assert.deepEqual(
              error.report.cleanupCodes,
              abortFails ? ["RESOURCE_WRITER_ABORT_FAILED"] : [],
            );
            return true;
          },
        );
        if (abortFails) {
          assert.equal(fs.events.length, 0);
          const folder = [...fs.root.entries.values()][0];
          assert.ok(folder.entries.has(RESOURCE_EXPORT_MARKER));
          assert.equal([...fs.root.entries.keys()][0], "data");
          assert.ok(folder.entries.has("00-00000001.data"));
        } else assert.equal(fs.root.entries.size, 0);
      } finally {
        if (prior) Object.defineProperty(globalThis, "isSecureContext", prior);
        else delete globalThis.isSecureContext;
      }
    });
}

// guest-framework
{
  // 根据仓库 Python 写入器验证合成 Guest 字节、签名和封闭目标配置。
  // Verify synthetic Guest bytes, signatures and closed target profiles against the repository Python writer.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { webcrypto, createHash } = nodeCryptoModule;
  const { spawnSync } = nodeChild_processModule;
  const { mkdtempSync, writeFileSync, readFileSync, rmSync } = nodeFsModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { fileURLToPath } = nodeUrlModule;
  const {
    buildGuestFramework,
    planGuestFramework,
    guestInfoPlist,
    MAX_IMAGE_BYTES,
    MAX_GUEST_BYTES,
    PAGE_SIZE,
  } = guestFrameworkModule;
  const { inspectGuestTarget } = guestTargetStreamModule;
  const { DEFAULT_SHELL_PROFILE_ID } = shellProfilesModule;
  const source = fileURLToPath(new URL("../../dm/tools/guest_dylib.py", import.meta.url));
  const options = { crypto: webcrypto };
  const fixture = (name, size, textEnd, patchOff) => ({
    name,
    image: Uint8Array.from({ length: size }, (_, i) => (i * 13 + 31) % 256),
    textEnd,
    patchOff,
  });

  function pythonBuild(modules, dir) {
    for (const m of modules) {
      writeFileSync(join(dir, `${m.name}.patched.bin`), m.image);
      writeFileSync(
        join(dir, `${m.name}.fixups.tsv`),
        `# TEXT_END ${m.textEnd.toString(16)}\n# PATCH_OFF ${m.patchOff.toString(16)}\n`,
      );
    }
    const r = spawnSync(
      "python3",
      [
        "-B",
        "-c",
        `import importlib.util,sys\ns=importlib.util.spec_from_file_location('guest',sys.argv[1]); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)\nm.write(sys.argv[2],sys.argv[2],sys.argv[3:])`,
        source,
        dir,
        ...modules.map((m) => m.name),
      ],
      { encoding: "utf8" },
    );
    assert.equal(r.status, 0, r.stderr);
    return readFileSync(join(dir, "Guest.framework", "Guest"));
  }

  test("synthetic byte parity with stdlib Python writer, repeated builds and plist", async () => {
    const cases = [
      [fixture("a", 1, 1, PAGE_SIZE)],
      [
        fixture("a", PAGE_SIZE * 3 + 17, PAGE_SIZE - 1, PAGE_SIZE * 2),
        fixture("b", PAGE_SIZE * 2, 0, PAGE_SIZE),
      ],
      [fixture("abcdefghij", PAGE_SIZE, PAGE_SIZE, PAGE_SIZE)],
      Array.from({ length: 48 }, (_, i) => fixture(`m${i}`, 1, 0, 0)),
    ];
    for (const modules of cases) {
      const dir = mkdtempSync(join(tmpdir(), "guest-parity-"));
      try {
        const expected = pythonBuild(modules, dir);
        const actual = await buildGuestFramework(modules, options);
        assert.deepEqual(Buffer.from(actual), expected);
        assert.deepEqual(await buildGuestFramework(modules, options), actual);
        assert.equal(
          guestInfoPlist(),
          readFileSync(join(dir, "Guest.framework", "Info.plist"), "utf8"),
        );
      } finally {
        rmSync(dir, { recursive: true, force: true });
      }
    }
  });

  test("16 KiB layout, UUID and structural ad-hoc signature cover the entire unsigned prefix", async () => {
    const modules = [fixture("foo", PAGE_SIZE * 3 + 71, PAGE_SIZE, PAGE_SIZE * 2)];
    const p = planGuestFramework(modules),
      out = await buildGuestFramework(modules, options),
      v = new DataView(out.buffer);
    let pos = 32,
      uuid,
      textSize;
    for (let i = 0; i < v.getUint32(16, true); i++) {
      const cmd = v.getUint32(pos, true),
        size = v.getUint32(pos + 4, true);
      if (cmd === 0x19) {
        const at = Number(v.getBigUint64(pos + 24, true)),
          len = Number(v.getBigUint64(pos + 32, true));
        assert.equal(at % PAGE_SIZE, 0);
        assert.equal(len % PAGE_SIZE, 0);
        if (pos === 32) textSize = len;
        for (let j = 0; j < v.getUint32(pos + 64, true); j++) {
          const s = pos + 72 + 80 * j;
          assert.equal(v.getUint32(s + 48, true) % PAGE_SIZE, 0);
          assert.equal(Number(v.getBigUint64(s + 40, true)) % PAGE_SIZE, 0);
          assert.equal(v.getUint32(s + 52, true), 14);
        }
      }
      if (cmd === 0x1b) uuid = out.slice(pos + 8, pos + 24);
      if (i === v.getUint32(16, true) - 1) {
        assert.equal(cmd, 0x1d);
        assert.equal(v.getUint32(pos + 8, true), p.signatureOffset);
        assert.equal(v.getUint32(pos + 12, true), p.signatureSize);
      }
      pos += size;
    }
    assert.deepEqual(
      Buffer.from(uuid),
      createHash("sha256").update(out.subarray(PAGE_SIZE, p.bodyEnd)).digest().subarray(0, 16),
    );
    const sig = p.signatureOffset,
      cd = sig + 20;
    assert.equal(v.getUint32(sig), 0xfade0cc0);
    assert.equal(v.getUint32(cd), 0xfade0c02);
    assert.equal(v.getUint32(cd + 12), 0x20002);
    assert.equal(v.getUint32(cd + 32), sig);
    assert.equal(Number(v.getBigUint64(cd + 72)), textSize);
    const count = v.getUint32(cd + 28);
    for (let i = 0; i < count; i++)
      assert.deepEqual(
        Buffer.from(out.subarray(cd + 94 + i * 32, cd + 126 + i * 32)),
        createHash("sha256")
          .update(out.subarray(i * 4096, Math.min((i + 1) * 4096, sig)))
          .digest(),
      );
    assert.ok(out.subarray(sig + v.getUint32(sig + 4)).every((b) => b === 0));
    assert.ok(p.signatureSize - v.getUint32(sig + 4) >= 0x40000);
  });

  test("final Guest size includes header and signature within the downstream file budget", () => {
    const image = new Uint8Array(4 * 1024 * 1024);
    const modules = Array.from({ length: 31 }, (_, i) => ({
      name: `m${i}`,
      image,
      textEnd: 0,
      patchOff: 0,
    }));
    const tail = { name: "tail", textEnd: 0, patchOff: 0 };
    // 重用 4 MiB 后备存储来检查大布局，不分配或哈希大文件。
    // Reuse a 4 MiB backing store to check large layouts without allocating or hashing a large file.
    const lastSize = 2 * 1024 * 1024 + 8 * PAGE_SIZE;
    const allowed = planGuestFramework([
      ...modules,
      { ...tail, image: image.subarray(0, lastSize) },
    ]);
    assert.ok(allowed.outputSize <= MAX_GUEST_BYTES);
    assert.ok(MAX_GUEST_BYTES - allowed.outputSize < PAGE_SIZE);
    assert.throws(
      () =>
        planGuestFramework([
          ...modules,
          { ...tail, image: image.subarray(0, lastSize + PAGE_SIZE) },
        ]),
      /128 MiB output policy/,
    );
    assert.throws(
      () => planGuestFramework([...modules, { ...tail, image }]),
      /128 MiB output policy/,
    );
  });

  test("malformed inputs reject before output allocation or hashing", async () => {
    const valid = fixture("a", PAGE_SIZE, 1, PAGE_SIZE);
    const sharedImage = new Uint8Array(new SharedArrayBuffer(1));
    const repeatedImage = new Uint8Array(4 * 1024 * 1024);
    const bad = [
      null,
      [],
      Array(49).fill(valid),
      [valid, valid],
      ...["", "../x", "abcdefghijk", "é", "a\0b"].map((name) => [{ ...valid, name }]),
      ...[-1, NaN, Infinity, 1.1, Number.MAX_SAFE_INTEGER, MAX_IMAGE_BYTES + 1].flatMap((n) => [
        [{ ...valid, textEnd: n }],
        [{ ...valid, patchOff: n }],
      ]),
      [{ ...valid, image: new ArrayBuffer(1) }],
      [{ ...valid, image: sharedImage }],
      [{ ...valid, image: new Uint8Array() }],
      [{ ...valid, textEnd: PAGE_SIZE + 1 }],
      [{ ...valid, patchOff: 1 }],
      [{ ...valid, patchOff: 0 }],
      [{ ...valid, patchOff: PAGE_SIZE * 2 }],
      Array.from({ length: 32 }, (_, i) => ({
        name: `m${i}`,
        image: repeatedImage,
        textEnd: 0,
        patchOff: 0,
      })),
      Array.from({ length: 33 }, (_, i) => ({
        name: `m${i}`,
        image: repeatedImage,
        textEnd: 0,
        patchOff: 0,
      })),
      [
        ...Array.from({ length: 32 }, (_, i) => ({
          name: `m${i}`,
          image: repeatedImage.subarray(1),
          textEnd: 0,
          patchOff: 0,
        })),
        { name: "tail", image: new Uint8Array(1), textEnd: 0, patchOff: 0 },
      ],
    ];
    const Native = globalThis.Uint8Array;
    let allocations = 0,
      hashes = 0;
    globalThis.Uint8Array = new Proxy(Native, {
      construct(target, args) {
        allocations++;
        return Reflect.construct(target, args);
      },
    });
    try {
      for (const modules of bad)
        await assert.rejects(
          buildGuestFramework(modules, {
            crypto: {
              subtle: {
                digest() {
                  hashes++;
                  throw Error("Unexpected hashing");
                },
              },
            },
          }),
        );
      assert.equal(allocations, 0);
      assert.equal(hashes, 0);
    } finally {
      globalThis.Uint8Array = Native;
    }
    for (const id of ["a&b.c", "", "a", "<evil/>", "a.".repeat(200)])
      assert.throws(() => guestInfoPlist(id));
  });

  test("subarray input views, missing WebCrypto and unchanged inputs", async () => {
    const full = fixture("a", PAGE_SIZE + 4, 1, PAGE_SIZE);
    const image = full.image.subarray(2, PAGE_SIZE + 2),
      original = image.slice();
    const modules = [{ ...full, image }];
    await buildGuestFramework(modules, options);
    assert.deepEqual(image, original);
    await assert.rejects(buildGuestFramework(modules, { crypto: null }), /WebCrypto/);
  });

  test("AbortSignal rejects before validation/allocation and during UUID/page hashing", async () => {
    const controller = new AbortController();
    controller.abort("custom reason");
    const Native = globalThis.Uint8Array;
    let allocations = 0;
    globalThis.Uint8Array = new Proxy(Native, {
      construct(target, args) {
        allocations++;
        return Reflect.construct(target, args);
      },
    });
    try {
      // 无效元数据确保取消检查发生在验证之前。
      // Invalid metadata ensures cancellation is checked even before validation.
      await assert.rejects(buildGuestFramework(null, { ...options, signal: controller.signal }), {
        name: "AbortError",
      });
      assert.equal(allocations, 0);
    } finally {
      globalThis.Uint8Array = Native;
    }
    for (const abortOnCall of [1, 2, 4]) {
      const c = new AbortController();
      let calls = 0;
      const crypto = {
        subtle: {
          async digest(algorithm, bytes) {
            calls++;
            const result = await webcrypto.subtle.digest(algorithm, bytes);
            if (calls === abortOnCall) c.abort();
            return result;
          },
        },
      };
      await assert.rejects(
        buildGuestFramework([fixture("a", PAGE_SIZE, 1, PAGE_SIZE)], { crypto, signal: c.signal }),
        { name: "AbortError" },
      );
      assert.equal(calls, abortOnCall, "No further digests after cancellation");
    }
  });

  test("closed profiles preserve default bytes and bind legacy minimum, SDK, plist and signature", async () => {
    const modules = [fixture("a", PAGE_SIZE, 1, PAGE_SIZE)];
    const baseline = await buildGuestFramework(modules, options);
    assert.deepEqual(
      await buildGuestFramework(modules, { ...options, profileId: DEFAULT_SHELL_PROFILE_ID }),
      baseline,
    );
    assert.equal(
      guestInfoPlist("local.guest", { profileId: DEFAULT_SHELL_PROFILE_ID }),
      guestInfoPlist(),
    );
    for (const [profileId, minimum] of [
      [DEFAULT_SHELL_PROFILE_ID, "26.0"],
      ["v2.0.0-ios18", "18.0"],
    ]) {
      const bytes = await buildGuestFramework(modules, { ...options, profileId });
      const plist = guestInfoPlist("local.guest", { profileId });
      assert.ok(plist.includes(`<key>MinimumOSVersion</key><string>${minimum}</string>`));
      const observed = await inspectGuestTarget(
        {
          size: bytes.length,
          async *chunks() {
            yield bytes;
          },
        },
        {
          expectedSdkVersion: "26.0.0",
          minimumOSVersion: minimum,
        },
      );
      assert.deepEqual(observed, {
        platform: "ios",
        minimumVersion: `${minimum}.0`,
        sdkVersion: "26.0.0",
      });
      const p = planGuestFramework(modules);
      const cd = p.signatureOffset + 20;
      const view = new DataView(bytes.buffer);
      for (let i = 0; i < view.getUint32(cd + 28); i++) {
        assert.deepEqual(
          Buffer.from(bytes.subarray(cd + 94 + i * 32, cd + 126 + i * 32)),
          createHash("sha256")
            .update(bytes.subarray(i * 4096, Math.min((i + 1) * 4096, p.signatureOffset)))
            .digest(),
        );
      }
      if (minimum === "18.0") {
        assert.notDeepEqual(bytes, baseline);
        assert.deepEqual(
          bytes.subarray(PAGE_SIZE, p.signatureOffset),
          baseline.subarray(PAGE_SIZE, p.signatureOffset),
        );
      }
    }
  });

  test("unknown profiles reject before hashing and cannot emit plist targets", async () => {
    for (const profileId of [
      "legacy18",
      "18",
      "ios17",
      "",
      null,
      {},
      { minimumVersion: "18.0.0" },
    ]) {
      await assert.rejects(
        buildGuestFramework([fixture("a", 1, 1, PAGE_SIZE)], {
          profileId,
          crypto: {
            subtle: {
              digest() {
                assert.fail("Unexpected hashing");
              },
            },
          },
        }),
        /SHELL_PROFILE/,
      );
      assert.throws(() => guestInfoPlist("local.guest", { profileId }), /SHELL_PROFILE/);
    }
  });
}

// guest-target-stream
{
  // 仅使用合成 Mach-O 前缀测试分块目标检查；不读取游戏或密钥。
  // Exercise chunked target inspection using synthetic Mach-O prefixes only; no game or key input.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { inspectGuestTarget, inspectGuestPlist } = guestTargetStreamModule;

  const { guestInfoPlist } = guestFrameworkModule;

  const expected = { platform: "ios", minimumVersion: "18.2.3", sdkVersion: "26.0.0" };
  function fixture() {
    const bytes = new Uint8Array(96);
    const view = new DataView(bytes.buffer);
    const put = (offset, value) => view.setUint32(offset, value, true);
    put(0, 0xfeedfacf);
    put(4, 0x0100000c);
    put(12, 6);
    put(16, 2);
    put(20, 48);
    put(32, 0x19);
    put(36, 24);
    put(56, 0x32);
    put(60, 24);
    put(64, 2);
    put(68, 0x120203);
    put(72, 0x1a0000);
    return { bytes, put };
  }
  function entry(bytes, split = bytes.length, size = bytes.length) {
    return {
      size,
      async *chunks() {
        yield new Uint8Array();
        for (let at = 0; at < bytes.length; at += split) yield bytes.subarray(at, at + split);
      },
    };
  }

  test("all small chunk boundaries preserve headers, skipped commands and target", async () => {
    const { bytes } = fixture();
    for (let split = 1; split <= bytes.length; split++)
      assert.deepEqual(await inspectGuestTarget(entry(bytes, split)), expected);
  });

  test("target recognition still drains all payload chunks", async () => {
    const { bytes } = fixture();
    let drained = false;
    const source = {
      size: bytes.length,
      async *chunks() {
        yield bytes.subarray(0, 80);
        yield bytes.subarray(80);
        drained = true;
      },
    };
    assert.deepEqual(await inspectGuestTarget(source), expected);
    assert.equal(drained, true);
  });

  test("short, excess and invalid trailing chunks are rejected after valid target", async () => {
    const { bytes } = fixture();
    await assert.rejects(inspectGuestTarget(entry(bytes, 1, bytes.length + 1)), /GUEST_SIZE/);
    await assert.rejects(inspectGuestTarget(entry(bytes, 1, bytes.length - 1)), /GUEST_SIZE/);
    await assert.rejects(
      inspectGuestTarget({
        size: bytes.length,
        async *chunks() {
          yield bytes;
          yield [0];
        },
      }),
      /GUEST_SIZE/,
    );
  });

  test("malformed headers, command bounds and duplicate targets fail closed", async () => {
    const mutations = [
      (put) => put(0, 0),
      (put) => put(4, 0),
      (put) => put(12, 2),
      (put) => put(16, 4097),
      (put) => put(16, 3),
      (put) => put(16, 1),
      (put) => put(20, 1000),
      (put) => put(20, 49),
      (put) => put(36, 0),
      (put) => put(36, 9),
      (put) => put(36, 64),
      (put) => put(60, 16),
      (put) => put(64, 1),
      (put) => put(56, 0x19),
      (put) => {
        put(32, 0x32);
        put(40, 2);
      },
    ];
    for (const mutate of mutations) {
      const { bytes, put } = fixture();
      mutate(put);
      await assert.rejects(inspectGuestTarget(entry(bytes, 1)), /GUEST_TARGET/);
    }
  });

  test("truncated header and partial command prefix fail closed", async () => {
    await assert.rejects(inspectGuestTarget(entry(new Uint8Array(31), 1)), /GUEST_TARGET/);
    const { bytes, put } = fixture();
    put(16, 1);
    put(20, 7);
    await assert.rejects(inspectGuestTarget(entry(bytes, 1)), /GUEST_TARGET/);
  });

  test("cancellation and producer failures after target detection propagate", async () => {
    const { bytes } = fixture();
    const controller = new AbortController();
    await assert.rejects(
      inspectGuestTarget(
        {
          size: bytes.length,
          async *chunks() {
            yield bytes.subarray(0, 80);
            controller.abort();
            yield bytes.subarray(80);
          },
        },
        { signal: controller.signal },
      ),
      { name: "AbortError" },
    );
    await assert.rejects(
      inspectGuestTarget({
        size: bytes.length,
        async *chunks() {
          yield bytes.subarray(0, 80);
          throw new Error("PRODUCER_FAILED");
        },
      }),
      /PRODUCER_FAILED/,
    );
  });

  test("large skipped command needs no command-sized parser allocation", async () => {
    const { bytes, put } = fixture();
    const commandSize = 8 * 1024 * 1024;
    put(20, commandSize + 24);
    put(36, commandSize);
    const block = new Uint8Array(65536);
    const source = {
      size: 32 + commandSize + 24,
      async *chunks() {
        yield bytes.subarray(0, 40);
        let remaining = commandSize - 8;
        while (remaining) {
          const length = Math.min(remaining, block.length);
          yield block.subarray(0, length);
          remaining -= length;
        }
        yield bytes.subarray(56, 80);
      },
    };
    assert.deepEqual(await inspectGuestTarget(source), expected);
  });

  test("invalid declared entry budgets fail before pulling", async () => {
    for (const size of [-1, 0.5, NaN, 128 * 1024 * 1024 + 1]) {
      await assert.rejects(
        inspectGuestTarget({
          size,
          chunks() {
            throw new Error("MUST_NOT_PULL");
          },
        }),
        /GUEST_MISSING/,
      );
    }
  });

  test("build tool counts exactly match command size and valid tool records stream", async () => {
    for (const count of [1, 2, 0xffffffff]) {
      const { bytes, put } = fixture();
      put(76, count);
      await assert.rejects(inspectGuestTarget(entry(bytes, 1)), /GUEST_TARGET/);
    }
    const { bytes, put } = fixture();
    put(20, 56);
    put(60, 32);
    await assert.rejects(inspectGuestTarget(entry(bytes, 1)), /GUEST_TARGET/);
    put(76, 1);
    put(80, 3);
    put(84, 0x1a0000);
    for (let split = 1; split <= bytes.length; split++)
      assert.deepEqual(await inspectGuestTarget(entry(bytes, split)), expected);
  });

  test("all legacy minimum-version commands reject even alongside a build target", async () => {
    for (const command of [0x24, 0x25, 0x2f, 0x30]) {
      const { bytes, put } = fixture();
      put(32, command);
      await assert.rejects(inspectGuestTarget(entry(bytes, 1)), /GUEST_TARGET/);
      put(32, 0x19);
      put(56, command);
      await assert.rejects(inspectGuestTarget(entry(bytes, 1)), /GUEST_TARGET/);
    }
  });

  test("expected SDK and parsed plist minimum must agree with the executable", async () => {
    const { bytes } = fixture();
    assert.deepEqual(
      await inspectGuestTarget(entry(bytes, 1), {
        expectedSdkVersion: "26.0",
        minimumOSVersion: "18.2.3",
      }),
      expected,
    );
    for (const options of [
      { expectedSdkVersion: "18.0.0" },
      { minimumOSVersion: "26.0" },
      ...[null, 26, "", "26.0.0.0", "65536.0", "26.256", "26.x", " 26.0"].flatMap((value) => [
        { expectedSdkVersion: value },
        { minimumOSVersion: value },
      ]),
    ])
      await assert.rejects(inspectGuestTarget(entry(bytes, 1), options), /GUEST_TARGET/);
  });

  test("canonical generated plist streams exactly and binds to the closed profile", async () => {
    for (const [profileId, minimumVersion] of [
      [undefined, "26.0.0"],
      ["v2.0.0-ios26", "26.0.0"],
      ["v2.0.0-ios18", "18.0.0"],
    ]) {
      const bytes = new TextEncoder().encode(guestInfoPlist(undefined, { profileId }));
      for (const split of [1, 7, 32, bytes.length])
        assert.deepEqual(await inspectGuestPlist(entry(bytes, split), { profileId }), {
          minimumVersion,
        });
      const otherProfile = minimumVersion === "18.0.0" ? "v2.0.0-ios26" : "v2.0.0-ios18";
      await assert.rejects(
        inspectGuestPlist(entry(bytes), { profileId: otherProfile }),
        /GUEST_PLIST/,
      );
      await assert.rejects(
        inspectGuestPlist(entry(bytes, 1, bytes.length + 1), { profileId }),
        /GUEST_PLIST/,
      );
      await assert.rejects(
        inspectGuestPlist(entry(bytes.subarray(0, -1), 1, bytes.length), { profileId }),
        /GUEST_PLIST/,
      );
      const changed = bytes.slice();
      changed[changed.length - 2] ^= 1;
      await assert.rejects(inspectGuestPlist(entry(changed), { profileId }), /GUEST_PLIST/);
    }
  });

  test("plist validation rejects noncanonical XML, invalid size and producer behavior", async () => {
    const encoder = new TextEncoder();
    const canonical = guestInfoPlist();
    for (const text of [
      canonical + " ",
      canonical.replace("</dict>", "<key>MinimumOSVersion</key><string>18.0</string></dict>"),
      canonical.replace("26.0", "&#50;6.0"),
    ])
      await assert.rejects(inspectGuestPlist(entry(encoder.encode(text))), /GUEST_PLIST/);
    for (const size of [-1, NaN, 0.5, 16385])
      await assert.rejects(
        inspectGuestPlist({
          size,
          chunks() {
            assert.fail("Unexpected read");
          },
        }),
        /GUEST_PLIST/,
      );
    const bytes = encoder.encode(canonical);
    await assert.rejects(
      inspectGuestPlist({
        size: bytes.length,
        async *chunks() {
          yield bytes;
          yield [0];
        },
      }),
      /GUEST_PLIST/,
    );
    await assert.rejects(
      inspectGuestPlist({
        size: bytes.length,
        async *chunks() {
          yield bytes;
          yield new Uint8Array([0]);
        },
      }),
      /GUEST_PLIST/,
    );
    await assert.rejects(
      inspectGuestPlist({
        size: bytes.length,
        async *chunks() {
          yield bytes;
          throw new Error("PRODUCER_FAILED");
        },
      }),
      /PRODUCER_FAILED/,
    );
    const controller = new AbortController();
    await assert.rejects(
      inspectGuestPlist(
        {
          size: bytes.length,
          async *chunks() {
            yield bytes;
            controller.abort();
          },
        },
        { signal: controller.signal },
      ),
      { name: "AbortError" },
    );
  });
}

// preview-manifest
{
  // 验证 preview-manifest 的约定与边界；依据 ../verification/preview-manifest.mjs 及本文件的测试断言。
  // Verify preview-manifest contracts and boundaries against ../verification/preview-manifest.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { mkdtemp, writeFile, rm, symlink } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { pathToFileURL } = nodeUrlModule;
  const { createPreviewManifest } = previewManifestModule;

  test("preview manifest accepts only referenced static bundle files", async () => {
    const path = await mkdtemp(join(tmpdir(), "preview-manifest-"));
    const directory = pathToFileURL(`${path}/`);
    try {
      await writeFile(
        join(path, "index.html"),
        '<meta charset="utf-8"><link href="./app.css"><script src="./app.js"></script>',
      );
      await writeFile(join(path, "app.js"), "console.log('preview');");
      await writeFile(join(path, "app.css"), "body{color:black}");
      const manifest = await createPreviewManifest(directory, "a".repeat(40));
      assert.equal(manifest.commit, "a".repeat(40));
      assert.equal(manifest.files.length, 3);
      for (const name of ["prod.keys", "notes.txt", "unused.js"]) {
        await writeFile(join(path, name), "not public");
        await assert.rejects(createPreviewManifest(directory));
        await rm(join(path, name));
      }
      await rm(join(path, "build-manifest.json"));
      await symlink(join(path, "app.js"), join(path, "build-manifest.json"));
      await assert.rejects(createPreviewManifest(directory), /regular files/);
    } finally {
      await rm(path, { recursive: true, force: true });
    }
  });
}
