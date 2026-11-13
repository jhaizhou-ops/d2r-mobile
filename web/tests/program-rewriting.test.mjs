// Feature suite: program-rewriting. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as nodeCryptoModule from "node:crypto";
import * as guestFrameworkModule from "../guest-framework.mjs";
import * as programRewriterAdapterModule from "../program-rewriter-adapter.mjs";
import * as promisesModule from "node:fs/promises";
import * as typescriptModule from "typescript";
import * as programReleaseCatalogModule from "../program-release-catalog.mjs";
import * as programCompatibilityModule from "../program-compatibility.mjs";
import * as shellProfilesModule from "../shell-profiles.mjs";
import * as rewriteModule from "../toolchain/program-rewrite-spike/js/rewrite.mjs";
import * as nodeChild_processModule from "node:child_process";
import * as nodeUrlModule from "node:url";
import * as ruleDataModule from "../toolchain/program-rewrite-spike/js/rule-data.mjs";
import * as outputsModule from "../toolchain/program-rewrite-spike/js/outputs.mjs";
import * as arm64Module from "../toolchain/program-rewrite-spike/js/arm64.mjs";
import * as relocationsModule from "../toolchain/program-rewrite-spike/js/relocations.mjs";

// program-buffer-lifetime
{
  // 合成缓冲区生命周期测试；不测量浏览器内存或断言真实游戏兼容性。
  // Synthetic buffer lifetime tests; no browser-memory measurement or real-game compatibility claim.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { buildGuestFramework, guestInfoPlist } = guestFrameworkModule;
  const { createProgramRewriterAdapter, PROGRAM_REWRITER_SOURCE } = programRewriterAdapterModule;

  const encode = (text) => new TextEncoder().encode(text);
  const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const empty = (bytes) => assert.ok(bytes.every((value) => value === 0));
  const buildId = "534e5448455449430000000000000000";
  async function fixture(t, targetHeader = "") {
    const snapshots = new Map(),
      copiedFiles = new Map(),
      inputs = {},
      outputs = {},
      modules = [],
      images = [];
    for (const [index, name] of ["main", "sdk"].entries()) {
      const input = new Uint8Array(128).fill(index + 1);
      input.set(Buffer.from(buildId, "hex"), 0x40);
      inputs[name] = input;
      const image = new Uint8Array(65540).fill(index + 3);
      const files = {
        "build_src.sha256": encode(`${PROGRAM_REWRITER_SOURCE}  patch_nso.py\n`),
        [`${name}.patched.bin`]: image,
        [`${name}.fixups.tsv`]: encode(
          targetHeader +
            "# TEXT_END\t4\n# PATCH_OFF\t65536\n# KEY_PAGE_OFF\t49152\n# KEY_PAGE_SIZE\t16384\n# RO_OFF\t16384\n# DATA_OFF\t32768\n",
        ),
      };
      for (const suffix of ["gates", "sites", "cuts", "params", "obs"])
        files[`${name}.${suffix}.tsv`] = encode(`${name} ${suffix}\n`);
      outputs[name] = files;
      modules.push({
        name,
        caseName: name,
        buildId,
        ruleset: "native",
        inputSha256: sha(input),
        files: Object.fromEntries(Object.entries(files).map(([file, bytes]) => [file, sha(bytes)])),
      });
      images.push({ name, image, textEnd: 4, patchOff: 65536 });
    }
    const guest = await buildGuestFramework(images);
    const plist = encode(guestInfoPlist());
    const catalog = {
      schemaVersion: 2,
      scope: "synthetic-only",
      sourceSha256: PROGRAM_REWRITER_SOURCE,
      versions: [{ id: "lifetime", modules, guestSha256: sha(guest), plistSha256: sha(plist) }],
    };
    // 在摘要边界观察独占字节，不依赖可重写的 slice 方法。
    // Observe owned bytes at the digest boundary, without relying on overridable slice methods.
    const pending = new Map();
    const observe = (bytes, name, copies) => {
      const key = sha(bytes);
      if (!pending.has(key)) pending.set(key, []);
      pending.get(key).push({ name, copies });
    };
    for (const [name, bytes] of Object.entries(inputs)) observe(bytes, name, snapshots);
    for (const [name, files] of Object.entries(outputs))
      for (const file of Object.keys(files).sort())
        observe(files[file], `${name}/${file}`, copiedFiles);
    const digest = globalThis.crypto.subtle.digest.bind(globalThis.crypto.subtle);
    t.mock.method(globalThis.crypto.subtle, "digest", (algorithm, bytes) => {
      const match = pending.get(sha(bytes))?.shift();
      if (match) match.copies.set(match.name, bytes);
      return digest(algorithm, bytes);
    });
    return { inputs, outputs, snapshots, copiedFiles, guest, plist, catalog };
  }
  function adapterFor(f, run, close = () => {}, workerOutputOwnership = "borrowed") {
    return createProgramRewriterAdapter({
      trustedCatalog: f.catalog,
      workerOutputOwnership,
      resolveInput: () => ({ versionId: "lifetime", programModules: f.inputs }),
      createWorker: () => ({
        run:
          run ??
          (({ moduleName }) => ({
            ok: true,
            status: "synthetic-only",
            files: f.outputs[moduleName],
          })),
        close,
      }),
    });
  }
  async function collect(entry) {
    const chunks = [];
    for await (const chunk of entry.chunks()) chunks.push(chunk);
    return Buffer.concat(chunks);
  }

  test("program generation releases scratch only after Guest consumes it and preserves exact streamed bytes", async (t) => {
    const f = await fixture(t);
    const originalInputs = Object.fromEntries(
      Object.entries(f.inputs).map(([name, bytes]) => [name, sha(bytes)]),
    );
    const originalOutputs = Object.fromEntries(
      Object.entries(f.outputs).map(([name, files]) => [
        name,
        Object.fromEntries(Object.entries(files).map(([file, bytes]) => [file, sha(bytes)])),
      ]),
    );
    let guestBuffer,
      closed = 0;
    const digest = globalThis.crypto.subtle.digest.bind(globalThis.crypto.subtle);
    t.mock.method(globalThis.crypto.subtle, "digest", (algorithm, bytes) => {
      if (bytes.byteLength === f.guest.length) {
        guestBuffer = bytes;
        // 全部快照和镜像在最终输出哈希检查之前仍保持完整。
        // All snapshots and images remain intact through the final output digest check.
        for (const [name, snapshot] of f.snapshots)
          assert.equal(sha(snapshot), originalInputs[name]);
        for (const name of ["main", "sdk"])
          assert.equal(
            sha(f.copiedFiles.get(`${name}/${name}.patched.bin`)),
            originalOutputs[name][`${name}.patched.bin`],
          );
      }
      return digest(algorithm, bytes);
    });
    const adapter = adapterFor(f, undefined, () => {
      closed++;
    });
    const entries = await adapter.generateProgram();
    assert.equal(closed, 1);
    assert.equal(f.snapshots.size, 2);
    assert.equal(f.copiedFiles.size, 16);
    assert.ok(guestBuffer);
    for (const bytes of f.snapshots.values()) empty(bytes);
    const retained = new Set([
      "main/main.fixups.tsv",
      "main/main.gates.tsv",
      "main/main.cuts.tsv",
      "sdk/sdk.fixups.tsv",
      "sdk/sdk.gates.tsv",
    ]);
    for (const [name, bytes] of f.copiedFiles) {
      if (retained.has(name)) assert.ok(bytes.some((value) => value !== 0));
      else empty(bytes);
    }
    assert.equal(entries.length, 7);
    const consumed = [];
    for (const entry of entries) {
      const bytes = await collect(entry);
      const expected = entry.path.endsWith("/Guest")
        ? f.guest
        : entry.path.endsWith("Info.plist")
          ? f.plist
          : f.outputs[entry.path.slice(3).split(".")[0]][entry.path.slice(3)];
      assert.deepEqual(bytes, Buffer.from(expected));
      consumed.push({ bytes, digest: sha(bytes) });
    }
    adapter.dispose();
    adapter.dispose();
    empty(guestBuffer);
    for (const bytes of f.copiedFiles.values()) empty(bytes);
    for (const entry of entries) await assert.rejects(collect(entry), { code: "PROGRAM_DISPOSED" });
    for (const { bytes, digest: expected } of consumed) assert.equal(sha(bytes), expected);
    for (const [name, bytes] of Object.entries(f.inputs))
      assert.equal(sha(bytes), originalInputs[name]);
    for (const [name, files] of Object.entries(f.outputs))
      for (const [file, bytes] of Object.entries(files))
        assert.equal(sha(bytes), originalOutputs[name][file]);
  });

  test("failed Guest verification wipes staged inputs and all owned worker copies", async (t) => {
    const f = await fixture(t);
    f.catalog.versions[0].guestSha256 = "0".repeat(64);
    const adapter = adapterFor(f);
    await assert.rejects(adapter.generateProgram(), { code: "PROGRAM_GUEST_DIGEST_MISMATCH" });
    assert.equal(f.snapshots.size, 2);
    assert.equal(f.copiedFiles.size, 16);
    for (const bytes of f.snapshots.values()) empty(bytes);
    for (const bytes of f.copiedFiles.values()) empty(bytes);
    for (const bytes of Object.values(f.inputs)) assert.ok(bytes.some((value) => value !== 0));
    adapter.dispose();
  });

  test("cancellation during rewrite still wipes owned snapshots without mutating borrowed input", async (t) => {
    const f = await fixture(t);
    const controller = new AbortController();
    const adapter = adapterFor(f, () => {
      controller.abort();
      return new Promise(() => {});
    });
    await assert.rejects(adapter.generateProgram({ signal: controller.signal }), {
      code: "PROGRAM_CANCELLED",
    });
    for (const bytes of f.snapshots.values()) empty(bytes);
    for (const bytes of Object.values(f.inputs)) assert.ok(bytes.some((value) => value !== 0));
    adapter.dispose();
  });

  test("dispose during rewrite rejects generation and wipes snapshots", async (t) => {
    const f = await fixture(t);
    let started;
    const running = new Promise((resolve) => {
      started = resolve;
    });
    const adapter = adapterFor(f, () => {
      started();
      return new Promise(() => {});
    });
    const generation = adapter.generateProgram();
    await running;
    adapter.dispose();
    await assert.rejects(generation, { code: "PROGRAM_DISPOSED" });
    for (const bytes of f.snapshots.values()) empty(bytes);
    for (const bytes of Object.values(f.inputs)) assert.ok(bytes.some((value) => value !== 0));
  });

  test("Buffer inputs and worker outputs stay borrowed and emitted chunks survive disposal", async (t) => {
    const f = await fixture(t);
    for (const name of Object.keys(f.inputs)) f.inputs[name] = Buffer.from(f.inputs[name]);
    for (const files of Object.values(f.outputs))
      for (const name of Object.keys(files)) files[name] = Buffer.from(files[name]);
    const borrowed = [
      ...Object.values(f.inputs),
      ...Object.values(f.outputs).flatMap(Object.values),
    ];
    const expected = borrowed.map(sha);
    const adapter = adapterFor(f);
    const entries = await adapter.generateProgram();
    assert.deepEqual(borrowed.map(sha), expected);
    assert.equal(f.snapshots.size, 2);
    assert.equal(f.copiedFiles.size, 16);
    const chunks = [];
    for (const entry of entries)
      for await (const bytes of entry.chunks()) chunks.push({ bytes, digest: sha(bytes) });
    adapter.dispose();
    assert.deepEqual(borrowed.map(sha), expected);
    for (const { bytes, digest } of chunks) assert.equal(sha(bytes), digest);
    for (const bytes of [...f.snapshots.values(), ...f.copiedFiles.values()]) empty(bytes);
  });

  test("owned worker outputs are adopted by identity and retained only until consumed", async (t) => {
    const f = await fixture(t);
    const originalInputs = Object.values(f.inputs).map(sha);
    const adapter = adapterFor(f, undefined, undefined, "owned");
    const entries = await adapter.generateProgram();
    for (const [module, files] of Object.entries(f.outputs))
      for (const [name, bytes] of Object.entries(files)) {
        assert.equal(f.copiedFiles.get(`${module}/${name}`), bytes);
        const retained =
          name.endsWith("fixups.tsv") || name.endsWith("gates.tsv") || name === "main.cuts.tsv";
        if (retained) assert.ok(bytes.some((value) => value !== 0));
        else empty(bytes);
      }
    const chunks = [];
    for (const entry of entries)
      for await (const bytes of entry.chunks()) chunks.push({ bytes, digest: sha(bytes) });
    adapter.dispose();
    for (const files of Object.values(f.outputs))
      for (const bytes of Object.values(files)) empty(bytes);
    for (const { bytes, digest } of chunks) assert.equal(sha(bytes), digest);
    assert.deepEqual(Object.values(f.inputs).map(sha), originalInputs);
  });

  test("owned digest failure wipes even response files not yet hashed", async (t) => {
    const f = await fixture(t);
    f.catalog.versions[0].modules[0].files["build_src.sha256"] = "0".repeat(64);
    const adapter = adapterFor(f, undefined, undefined, "owned");
    await assert.rejects(adapter.generateProgram(), { code: "PROGRAM_OUTPUT_DIGEST_MISMATCH" });
    for (const bytes of Object.values(f.outputs.main)) empty(bytes);
    for (const bytes of Object.values(f.outputs.sdk)) assert.ok(bytes.some((value) => value !== 0));
  });

  test("owned malformed response shape still wipes transferred eligible files", async (t) => {
    const f = await fixture(t);
    const adapter = adapterFor(
      f,
      () => ({ ok: true, status: "invalid", files: f.outputs.main }),
      undefined,
      "owned",
    );
    await assert.rejects(adapter.generateProgram(), { code: "PROGRAM_REWRITE_REJECTED" });
    for (const bytes of Object.values(f.outputs.main)) empty(bytes);
  });

  for (const kind of ["buffer", "subview", "shared", "resizable", "alias"])
    test(`owned output rejects ${kind} without wiping ineligible storage`, async (t) => {
      const f = await fixture(t);
      const original = f.outputs.main["main.gates.tsv"];
      let invalid;
      if (kind === "buffer") invalid = Buffer.from(original);
      if (kind === "subview") {
        invalid = new Uint8Array(original.length + 1).fill(7).subarray(1);
      }
      if (kind === "shared")
        invalid = new Uint8Array(new SharedArrayBuffer(original.length)).fill(7);
      if (kind === "resizable")
        invalid = new Uint8Array(
          new ArrayBuffer(original.length, { maxByteLength: original.length + 1 }),
        ).fill(7);
      if (kind === "alias") invalid = f.outputs.main["main.cuts.tsv"];
      const expected = sha(invalid);
      f.outputs.main["main.gates.tsv"] = invalid;
      const adapter = adapterFor(f, undefined, undefined, "owned");
      await assert.rejects(adapter.generateProgram(), { code: "PROGRAM_INVALID_OUTPUT" });
      if (kind !== "alias") assert.equal(sha(invalid), expected);
      for (const bytes of Object.values(f.outputs.main))
        if (bytes !== invalid || kind === "alias") empty(bytes);
    });

  for (const workerOutputOwnership of ["borrowed", "owned"])
    for (const interruption of ["abort", "dispose"])
      test(`${workerOutputOwnership} late successful response after ${interruption} respects ownership`, async (t) => {
        const f = await fixture(t);
        let resolveResult, started;
        const running = new Promise((resolve) => {
          started = resolve;
        });
        const controller = new AbortController();
        const adapter = adapterFor(
          f,
          () => {
            started();
            return new Promise((resolve) => {
              resolveResult = resolve;
            });
          },
          undefined,
          workerOutputOwnership,
        );
        const generation = adapter.generateProgram({ signal: controller.signal });
        await running;
        if (interruption === "abort") controller.abort();
        else adapter.dispose();
        await assert.rejects(generation, {
          code: interruption === "abort" ? "PROGRAM_CANCELLED" : "PROGRAM_DISPOSED",
        });
        const before = Object.values(f.outputs.main).map(sha);
        resolveResult({ ok: true, status: "synthetic-only", files: f.outputs.main });
        await new Promise((resolve) => setImmediate(resolve));
        if (workerOutputOwnership === "owned")
          for (const bytes of Object.values(f.outputs.main)) empty(bytes);
        else assert.deepEqual(Object.values(f.outputs.main).map(sha), before);
        for (const bytes of Object.values(f.inputs)) assert.ok(bytes.some((value) => value !== 0));
      });

  test("owned Guest verification failure wipes both modules", async (t) => {
    const f = await fixture(t);
    f.catalog.versions[0].guestSha256 = "0".repeat(64);
    const adapter = adapterFor(f, undefined, undefined, "owned");
    await assert.rejects(adapter.generateProgram(), { code: "PROGRAM_GUEST_DIGEST_MISMATCH" });
    for (const files of Object.values(f.outputs))
      for (const bytes of Object.values(files)) empty(bytes);
    for (const bytes of f.snapshots.values()) empty(bytes);
  });

  test("owned unexpected file is also wiped on file-set rejection", async (t) => {
    const f = await fixture(t);
    f.outputs.main["unexpected.bin"] = new Uint8Array([7, 8]);
    const adapter = adapterFor(f, undefined, undefined, "owned");
    await assert.rejects(adapter.generateProgram(), { code: "PROGRAM_REWRITE_REJECTED" });
    for (const bytes of Object.values(f.outputs.main)) empty(bytes);
  });

  test("owned detached output cannot interrupt cleanup of later files", async (t) => {
    const f = await fixture(t);
    const detached = f.outputs.main["build_src.sha256"];
    structuredClone(detached, { transfer: [detached.buffer] });
    const adapter = adapterFor(f, undefined, undefined, "owned");
    await assert.rejects(adapter.generateProgram(), { code: "PROGRAM_INVALID_OUTPUT" });
    for (const bytes of Object.values(f.outputs.main)) if (bytes !== detached) empty(bytes);
  });

  test("owned genuine empty sidecars remain valid", async (t) => {
    const f = await fixture(t);
    f.outputs.main["main.sites.tsv"] = new Uint8Array();
    f.catalog.versions[0].modules[0].files["main.sites.tsv"] = sha(new Uint8Array());
    const adapter = adapterFor(f, undefined, undefined, "owned");
    await adapter.generateProgram();
    adapter.dispose();
  });

  for (const target of ["0", "0x0"]) {
    test(`explicit Apple TARGET ${target} preserves synthetic Guest assembly`, async (t) => {
      const f = await fixture(t, `# TARGET\t${target}\n`);
      const adapter = adapterFor(f);
      try {
        const entries = await adapter.generateProgram();
        assert.deepEqual(
          await collect(entries.find((entry) => entry.path.endsWith("/Guest"))),
          Buffer.from(f.guest),
        );
      } finally {
        adapter.dispose();
      }
    });
  }
  for (const targetHeader of [
    "# TARGET\t1\n",
    "# TARGET\t0x2\n",
    "# TARGET\t3\n",
    "# TARGET\t0\n# TARGET\t0\n",
    "# TARGET\t-1\n",
  ]) {
    test(`non-Apple or invalid TARGET rejects before Guest assembly: ${JSON.stringify(targetHeader)}`, async (t) => {
      const f = await fixture(t, targetHeader);
      const adapter = adapterFor(f);
      try {
        await assert.rejects(adapter.generateProgram(), { code: "PROGRAM_INVALID_FIXUPS" });
        for (const bytes of f.snapshots.values()) empty(bytes);
        for (const bytes of f.copiedFiles.values()) empty(bytes);
        for (const bytes of Object.values(f.inputs)) assert.ok(bytes.some((value) => value !== 0));
      } finally {
        adapter.dispose();
      }
    });
  }
}

// program-release-catalog
{
  // 只用已登记的公开摘要与合成字节测试版本选择及拒绝边界；不读取游戏内容。
  // Test release selection and rejection boundaries with registered public digests and synthetic bytes, without reading game content.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { readFile } = promisesModule;
  const { default: ts } = typescriptModule;
  const {
    REGISTERED_PROGRAM_CATALOG,
    REGISTERED_PROGRAM_RELEASE,
    findRegisteredProgramRelease,
    diagnoseRegisteredProgramDigests,
  } = programReleaseCatalogModule;
  const { PROGRAM_COMPATIBILITY, isProgramCompatibilityDiagnosticCode } =
    programCompatibilityModule;
  const { createProgramRewriterAdapter } = programRewriterAdapterModule;
  const { resolveShellProfile, SHELL_PROFILE_IDS, BUNDLED_SHELL_PROFILE_IDS } = shellProfilesModule;
  const { createJavaScriptProgramRewriter } = rewriteModule;

  const knownDigests = Object.fromEntries(
    REGISTERED_PROGRAM_RELEASE.modules.map((module) => [module.name, module.inputSha256]),
  );

  test("registered release selection requires the complete exact module digest tuple", () => {
    const result = diagnoseRegisteredProgramDigests({ ...knownDigests });
    assert.deepEqual(result, {
      status: "registered",
      releaseId: REGISTERED_PROGRAM_RELEASE.id,
      gameVersion: "3.3.94012",
    });
    assert.equal(findRegisteredProgramRelease(result.releaseId), REGISTERED_PROGRAM_RELEASE);
    assert.ok(Object.isFrozen(result));
    for (const name of ["main", "sdk"]) {
      assert.equal(
        diagnoseRegisteredProgramDigests({ ...knownDigests, [name]: "0".repeat(64) }).status,
        "incompatible-modules",
      );
    }
    assert.deepEqual(
      diagnoseRegisteredProgramDigests({ main: "0".repeat(64), sdk: "1".repeat(64) }),
      {
        status: "unsupported-version",
        releaseId: null,
        gameVersion: null,
      },
    );
    assert.equal(
      diagnoseRegisteredProgramDigests({ main: knownDigests.sdk, sdk: knownDigests.main }).status,
      "unsupported-version",
    );
  });

  test("malformed claims and unknown release IDs never select a default", () => {
    for (const invalid of [
      null,
      {},
      { main: knownDigests.main },
      { ...knownDigests, rtld: "0".repeat(64) },
      { ...knownDigests, main: knownDigests.main.toUpperCase() },
      { ...knownDigests, sdk: "x".repeat(64) },
    ])
      assert.throws(() => diagnoseRegisteredProgramDigests(invalid), /PROGRAM_INVALID_MODULE_SET/);
    for (const id of [undefined, null, {}, "", "__proto__", "constructor", "3.3.94012", "latest"])
      assert.equal(findRegisteredProgramRelease(id), null);
    assert.throws(() => REGISTERED_PROGRAM_CATALOG.versions.push({ id: "custom" }), TypeError);
  });

  test("compatibility catalog stays honest, frozen and explicitly binds each shell to registered programs", () => {
    assert.equal(PROGRAM_COMPATIBILITY.arbitraryGameVersions, false);
    assert.equal(PROGRAM_COMPATIBILITY.arbitraryShellImports, false);
    assert.equal(PROGRAM_COMPATIBILITY.browserNativeSourceRebuild, false);
    assert.equal(PROGRAM_COMPATIBILITY.realGameVerified, false);
    assert.deepEqual(
      PROGRAM_COMPATIBILITY.releases.map((release) => release.gameVersion),
      ["3.3.94012"],
    );
    assert.deepEqual(
      PROGRAM_COMPATIBILITY.shellProfiles.map((profile) => profile.id),
      BUNDLED_SHELL_PROFILE_IDS,
    );
    for (const summary of PROGRAM_COMPATIBILITY.shellProfiles) {
      const profile = resolveShellProfile(summary.id);
      assert.equal(summary.minimumVersion, profile.guestTarget.minimumVersion);
      assert.deepEqual(summary.programReleaseIds, [REGISTERED_PROGRAM_RELEASE.id]);
      assert.throws(() => summary.programReleaseIds.push("caller-release"), TypeError);
    }
    assert.throws(() => PROGRAM_COMPATIBILITY.releases.push({}), TypeError);
  });

  test("only fixed program diagnostic codes can reach a public report", () => {
    for (const code of [
      "PROGRAM_UNSUPPORTED_VERSION",
      "PROGRAM_MODULE_RELEASE_MISMATCH",
      "PROGRAM_SHELL_RELEASE_MISMATCH",
      "PROGRAM_INVALID_MODULE_SET",
      "PROGRAM_INPUT_DIGEST_MISMATCH",
      "PROGRAM_BUILD_ID_MISMATCH",
    ])
      assert.equal(isProgramCompatibilityDiagnosticCode(code), true);
    for (const code of [null, {}, "PROGRAM_UNSUPPORTED_VERSION /private/file", knownDigests.main])
      assert.equal(isProgramCompatibilityDiagnosticCode(code), false);
  });

  function syntheticInput() {
    const main = new Uint8Array(256).fill(1),
      sdk = new Uint8Array(256).fill(2);
    main.set(Buffer.from(REGISTERED_PROGRAM_RELEASE.modules[0].buildId, "hex"), 0x40);
    return { main, sdk };
  }
  function adapter(
    input,
    profileId,
    createWorker = () => assert.fail("Unexpected worker creation"),
  ) {
    return createProgramRewriterAdapter({
      profileId,
      verifyProvenance: () => input,
      createWorker,
    });
  }

  test("unknown registered bytes reject before worker creation and leave borrowed input intact", async () => {
    const input = syntheticInput();
    const originals = Object.fromEntries(
      Object.entries(input).map(([name, bytes]) => [name, bytes.slice()]),
    );
    await assert.rejects(adapter(input).generateProgram(), { code: "PROGRAM_UNSUPPORTED_VERSION" });
    for (const name of ["main", "sdk"]) assert.deepEqual(input[name], originals[name]);
    await assert.rejects(adapter({ main: input.main }).generateProgram(), {
      code: "PROGRAM_INVALID_MODULE_SET",
    });
  });

  // 仅在测试中替换摘要结果以抵达注册路线；不是对真实游戏输入的验证。
  // Substitute digest results only in this test to reach the registered route, never as evidence about real game input.
  function mockMeasuredDigests(t, input, partial = false) {
    const digest = globalThis.crypto.subtle.digest.bind(globalThis.crypto.subtle);
    const originals = Object.fromEntries(
      Object.entries(input).map(([name, bytes]) => [
        name,
        createHash("sha256").update(bytes).digest("hex"),
      ]),
    );
    t.mock.method(globalThis.crypto.subtle, "digest", (algorithm, bytes) => {
      const hash = createHash("sha256").update(bytes).digest("hex");
      const name = Object.keys(originals).find((key) => originals[key] === hash);
      if (name && (!partial || name === "main"))
        return Promise.resolve(Uint8Array.from(Buffer.from(knownDigests[name], "hex")).buffer);
      return digest(algorithm, bytes);
    });
  }

  test("known main with a mismatched SDK produces the fixed mixed-release diagnostic", async (t) => {
    const input = syntheticInput();
    mockMeasuredDigests(t, input, true);
    await assert.rejects(adapter(input).generateProgram(), {
      code: "PROGRAM_MODULE_RELEASE_MISMATCH",
    });
  });

  test("measured release identity routes to both pinned shells and the worker, never a caller version label", async (t) => {
    const input = syntheticInput();
    mockMeasuredDigests(t, input);
    for (const profileId of SHELL_PROFILE_IDS) {
      let calls = 0;
      const expected = new Error("Synthetic worker stop");
      const instance = adapter(input, profileId, () => ({
        run(request) {
          calls++;
          assert.equal(request.releaseId, REGISTERED_PROGRAM_RELEASE.id);
          assert.equal(request.caseName, undefined);
          assert.equal(request.moduleName, "main");
          throw expected;
        },
        close() {},
      }));
      await assert.rejects(instance.generateProgram(), { code: "PROGRAM_GENERATION_FAILED" });
      assert.equal(calls, 1);
    }
  });

  test("a matching digest cannot bypass the independent build-ID check", async (t) => {
    const input = syntheticInput();
    input.main.fill(0, 0x40, 0x50);
    mockMeasuredDigests(t, input);
    await assert.rejects(adapter(input).generateProgram(), { code: "PROGRAM_BUILD_ID_MISMATCH" });
  });

  test("worker rejects an unknown release or mismatched bytes before native initialization", async () => {
    const rewrite = createJavaScriptProgramRewriter(() =>
      assert.fail("Unexpected native initialization"),
    );
    for (const releaseId of ["caller-release", REGISTERED_PROGRAM_RELEASE.id]) {
      await assert.rejects(
        rewrite({ bytes: new Uint8Array(256), releaseId, moduleName: "main" }),
        /Unsupported registered program input/,
      );
    }
  });

  // 仅在隔离的测试模块中加入第二份合成登记，验证无需新增生产注入接口的目录扩展。
  // Add a second synthetic registration only inside isolated test modules, proving extension without introducing a production injection API.
  async function extendedCatalogFixture() {
    const inputs = syntheticInput();
    const release = {
      ...REGISTERED_PROGRAM_RELEASE,
      id: "second-repository-fixture",
      gameVersion: "synthetic-only",
      modules: REGISTERED_PROGRAM_RELEASE.modules.map((module) => ({
        ...module,
        inputSha256: createHash("sha256").update(inputs[module.name]).digest("hex"),
      })),
    };
    const asModule = (source) =>
      `data:text/javascript;base64,${Buffer.from(source).toString("base64")}`;
    const catalogSource = await readFile(
      new URL("../program-release-catalog.mjs", import.meta.url),
      "utf8",
    );
    const marker = "versions: Object.freeze([REGISTERED_PROGRAM_RELEASE]),";
    assert.equal(catalogSource.split(marker).length, 2);
    const catalogUrl = asModule(
      catalogSource.replace(
        marker,
        `versions: Object.freeze([REGISTERED_PROGRAM_RELEASE, Object.freeze(${JSON.stringify(release)})]),`,
      ),
    );
    async function sourceWithImports(path, overrides, transform = (source) => source) {
      const sourceUrl = new URL(path, import.meta.url);
      let source = transform(await readFile(sourceUrl, "utf8"));
      const file = ts.createSourceFile(
        "isolated.mjs",
        source,
        ts.ScriptTarget.Latest,
        true,
        ts.ScriptKind.JS,
      );
      assert.equal(file.parseDiagnostics.length, 0);
      for (const statement of [...file.statements].reverse()) {
        if (!ts.isImportDeclaration(statement) && !ts.isExportDeclaration(statement)) continue;
        if (!statement.moduleSpecifier) continue;
        const specifier = statement.moduleSpecifier;
        assert.ok(ts.isStringLiteral(specifier));
        source =
          source.slice(0, specifier.getStart(file)) +
          JSON.stringify(overrides[specifier.text] ?? new URL(specifier.text, sourceUrl).href) +
          source.slice(specifier.end);
      }
      return asModule(source);
    }
    const profileUrl = await sourceWithImports(
      "../shell-profiles.mjs",
      { "./program-release-catalog.mjs": catalogUrl },
      (source) =>
        source.replace(
          "programReleaseIds: [REGISTERED_PROGRAM_RELEASE.id],",
          `programReleaseIds: id === "v2.0.0-ios26" ? [REGISTERED_PROGRAM_RELEASE.id, ${JSON.stringify(release.id)}] : [REGISTERED_PROGRAM_RELEASE.id],`,
        ),
    );
    const adapterUrl = await sourceWithImports("../program-rewriter-adapter.mjs", {
      "./program-release-catalog.mjs": catalogUrl,
      "./shell-profiles.mjs": profileUrl,
    });
    return {
      inputs,
      release,
      catalog: await import(catalogUrl),
      createAdapter: (await import(adapterUrl)).createProgramRewriterAdapter,
    };
  }

  test("a second repository registration is measured, selected and bound to its explicit shell", async () => {
    const { inputs, release, catalog, createAdapter } = await extendedCatalogFixture();
    const measured = Object.fromEntries(
      release.modules.map((module) => [module.name, module.inputSha256]),
    );
    assert.equal(catalog.diagnoseRegisteredProgramDigests(measured).releaseId, release.id);
    assert.equal(
      catalog.diagnoseRegisteredProgramDigests({ ...measured, main: knownDigests.main }).status,
      "incompatible-modules",
    );
    let workerRelease;
    await assert.rejects(
      createAdapter({
        verifyProvenance: () => inputs,
        createWorker: () => ({
          run(request) {
            workerRelease = request.releaseId;
            throw new Error("Synthetic worker stop");
          },
          close() {},
        }),
      }).generateProgram(),
      { code: "PROGRAM_GENERATION_FAILED" },
    );
    assert.equal(workerRelease, release.id);
    await assert.rejects(
      createAdapter({
        profileId: "v2.0.0-ios18",
        verifyProvenance: () => inputs,
        createWorker: () => assert.fail("Unbound release reached worker"),
      }).generateProgram(),
      { code: "PROGRAM_SHELL_RELEASE_MISMATCH" },
    );
    assert.equal(findRegisteredProgramRelease(release.id), null);
    assert.equal(REGISTERED_PROGRAM_CATALOG.versions.length, 1);
  });
}

// program-rewrite-apple-source
{
  // 只解析源码和自编合成字节记录，不执行原生代码或 WASM。
  // Source parsing and authored synthetic byte records only; no native/WASM execution.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { spawnSync } = nodeChild_processModule;
  const { readFile } = promisesModule;
  const { fileURLToPath } = nodeUrlModule;
  const { PROGRAM_REWRITER_SOURCE } = programReleaseCatalogModule;
  const { SOURCE_SHA256 } = ruleDataModule;
  const { buildOutputs } = outputsModule;
  const root = new URL("../toolchain/program-rewrite-spike/", import.meta.url);

  test("v2 source, catalog, rule data and explicit Apple proof share the exact source digest", async () => {
    const source = await readFile(new URL("../../dm/tools/patch_nso.py", import.meta.url));
    const digest = createHash("sha256").update(source).digest("hex");
    const baseline = JSON.parse(
      await readFile(new URL("apple-source-baseline.json", root), "utf8"),
    );
    assert.equal(digest, PROGRAM_REWRITER_SOURCE);
    assert.equal(digest, SOURCE_SHA256);
    assert.equal(digest, baseline.sourceSha256);
    assert.equal(
      (await readFile(new URL("upstream-source.sha256", root), "utf8")).trim(),
      `${digest}  dm/tools/patch_nso.py`,
    );
    assert.equal(baseline.outputFormatAddition, "# TARGET\t0x0\n");
  });

  test("the full Apple-specialized source AST matches the prior script and rejects semantic mutations", () => {
    const result = spawnSync(
      "python3",
      [
        "-B",
        "-c",
        `
import ast,importlib.util,json,sys
from pathlib import Path
p=Path(sys.argv[1]); spec=importlib.util.spec_from_file_location('apple_audit', p/'audit-apple-source.py')
audit=importlib.util.module_from_spec(spec); spec.loader.exec_module(audit)
contract=audit.verify(); source=audit.SOURCE.read_text(encoding='utf-8')
mutations = [
 ('else: emit(MRS_TPIDRRO(t))', 'else: emit(MRS_TPIDR(t))'),
 ("os.environ.get('DM_TARGET', 'apple')", "os.environ.get('DM_TARGET', 'android')"),
 ('"apple": 0, "android": 1', '"apple": 1, "android": 1'),
 ('emit(STRX(S, T))', 'emit(STRX(T, S))'),
 ('range(0, 16)', 'range(0, 17)'),
 ('else STP(S, T, 31, -16)', 'else STP(S, T, 31, -32)'),
]
for old,new in mutations:
 assert old in source
 changed=source.replace(old,new)
 try:
  normalized,reviewed=audit.specialize(ast.parse(changed))
  assert reviewed != contract['reviewedTargetNodes'] or audit.digest(normalized) != contract['baselineAstSha256'], old
 except ValueError:
  pass
print(json.dumps({'mutationsRejected':len(mutations)}))
`,
        fileURLToPath(root),
      ],
      { encoding: "utf8" },
    );
    assert.equal(result.status, 0, result.stderr);
    assert.equal(JSON.parse(result.stdout).mutationsRejected, 6);
  });

  test("synthetic output declares Apple0 without changing image bytes or layout records", async () => {
    const patch = Uint8Array.of(1, 2, 3, 4);
    const files = await buildOutputs(
      {
        image: new Uint8Array(65536),
        patchOff: 65536,
        keyPageOff: 49152,
        textEnd: 4,
        roOff: 16384,
        dataOff: 32768,
        moduleName: "main",
        kinds: new Map(),
        done: new Set(),
        windows: [],
        jumps: new Map(),
        relocator: { patchBytes: () => patch, gates: [] },
      },
      { cuts: [], params: [], obs: [], buildId: "synthetic" },
      { K18: 0 },
    );
    assert.deepEqual(files["main.patched.bin"].subarray(65536), patch);
    assert.equal(
      new TextDecoder().decode(files["main.fixups.tsv"]),
      "# TARGET\t0x0\n# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0xc000\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x4\n# RO_OFF\t0x4000\n# DATA_OFF\t0x8000\n# KEY_K18_IDX\t0\n",
    );
  });
}

// program-rewrite-arithmetic
{
  // 以自行编写的指令夹具检查成对访存偏移和栈恢复边界；不读取游戏输入。
  // Check pair-memory offsets and stack restore bounds with authored instruction fixtures, not game inputs.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const { fileURLToPath } = nodeUrlModule;
  const { STP, LDP, STP_PRE, LDP_POST } = arm64Module;
  const { createRelocations } = relocationsModule;

  const encoders = { STP, LDP, STP_PRE, LDP_POST };
  const source = fileURLToPath(new URL("../../dm/tools/patch_nso.py", import.meta.url));
  const pairOffset = (word) => (((word >>> 15) & 127) << 25) >> 22;

  test("pair encoders preserve every representable offset against the pinned Python functions", () => {
    const reference = spawnSync(
      "python3",
      [
        "-B",
        "-c",
        `import ast,json,sys
names = ['STP', 'LDP', 'STP_PRE', 'LDP_POST']
tree = ast.parse(open(sys.argv[1], encoding='utf-8').read())
functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in names]
scope = {}
exec(compile(ast.Module(body=functions, type_ignores=[]), sys.argv[1], 'exec'), scope)
print(json.dumps({name: [scope[name](0, 1, 31, off) for off in range(-512, 505, 8)] for name in names}))`,
        source,
      ],
      { encoding: "utf8" },
    );
    assert.equal(reference.status, 0, reference.stderr);
    const expected = JSON.parse(reference.stdout);
    for (const [name, encode] of Object.entries(encoders)) {
      const actual = [];
      for (let offset = -512; offset <= 504; offset += 8) {
        const word = encode(0, 1, 31, offset);
        assert.equal(pairOffset(word), offset);
        actual.push(word);
      }
      assert.deepEqual(actual, expected[name]);
    }
  });

  test("pair encoders reject out-of-range, unaligned and noninteger offsets", () => {
    const invalid = [-520, 512, -608, -513, 505, -511, -1, 1, 503, 0.5, NaN, Infinity];
    for (const encode of Object.values(encoders)) {
      for (const offset of invalid)
        assert.throws(() => encode(0, 1, 31, offset), /ARM64 pair offset out of range/);
    }
  });

  function relocator(decodeMaybe) {
    return createRelocations(
      { patchOff: 0x10000, keyPageOff: 0x4000 },
      { decodeMaybe },
      { K18: 0 },
    );
  }

  test("large stack post-index rejects before any patch bytes or fixups are emitted", () => {
    const ins = {
      word: 0xa8df4ff2,
      address: 0x40,
      mnemonic: "ldp",
      op_str: "x18, x19, [sp], #0x1f0",
      reads: ["sp"],
      writes: ["x18", "x19", "sp"],
    };
    const r = relocator(() => assert.fail("Unsupported restore must reject before substitution"));
    assert.throws(() => r.sitePlain(ins), /ARM64 pair offset out of range/);
    assert.equal(r.here(), 0x10000);
    assert.equal(r.patchBytes().length, 0);
    assert.deepEqual(r.fix, []);
    assert.deepEqual(r.gates, []);
  });

  test("supported stack restores keep their existing instruction bytes exactly", () => {
    const cases = [
      [
        0x91000652,
        0x91000400,
        "add",
        "x18, x18, #1",
        "e0073fa961d03bd5a0ffff90000040f92100008b200040f900040091200000f9e0077fa908c0ff17",
      ],
      [
        0xa9ba4ff2,
        0xa9ba4fe0,
        "stp",
        "x18, x19, [sp, #-0x60]!",
        "e00739a961d03bd5a0ffff90000040f92100008b200040f9e04fbaa9200000f9e0077fa908c0ff17",
      ],
      [
        0xa8c14ff2,
        0xa8c14fe0,
        "ldp",
        "x18, x19, [sp], #0x10",
        "e00739a961d03bd5a0ffff90000040f92100008b200040f9e04fc1a8200000f9e00778a908c0ff17",
      ],
      [
        0xa8d94ff2,
        0xa8d94fe0,
        "ldp",
        "x18, x19, [sp], #0x190",
        "e00739a961d03bd5a0ffff90000040f92100008b200040f9e04fd9a8200000f9e00760a908c0ff17",
      ],
    ];
    for (const [word, substituted, mnemonic, op_str, expected] of cases) {
      const ins = { word, address: 0x40, mnemonic, op_str, reads: [], writes: [] };
      // 只接受夹具列出的替换字，不以模拟解码器声称 Capstone 或设备验证。
      // Accept only the fixture's substituted word; this is not Capstone or device verification.
      const r = relocator((candidate) =>
        candidate === substituted ? { ...ins, op_str: op_str.replaceAll("x18", "x0") } : null,
      );
      assert.equal(r.sitePlain(ins), 0x10000);
      assert.equal(Buffer.from(r.patchBytes()).toString("hex"), expected);
    }
  });
}
