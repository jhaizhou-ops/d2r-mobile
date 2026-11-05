// Each selected scenario runs in its own child process; module mocks and globals never cross scenarios.
import process from "node:process";
const scenario = process.env.D2R_TEST_SCENARIO;
if (!["asset-pipeline-astc", "production-assets-astc"].includes(scenario))
  throw new Error("Unknown isolated mock scenario: " + scenario);

// asset-pipeline-astc
if (scenario === "asset-pipeline-astc") {
  // 隔离模块替身仅验证可选元数据、15项输出、所有权和预算，不声明游戏或浏览器证明。
  // Isolated module doubles test optional metadata, eleven outputs, ownership and budgets, not game or browser proof.
  const { default: assert } = await import("node:assert/strict");
  const { createOverlayManifest: createOverlayManifest } =
    await import("../../overlay-contract.mjs");
  const { RELEASE_OVERLAY_REVISION: RELEASE_OVERLAY_REVISION } =
    await import("../../toolchain/overlay-recipe-js/revision.mjs");
  const { mock: mock, test: test } = await import("node:test");
  mock.module("../../extraction-job-adapter.mjs", {
    namedExports: { readAuthenticatedAssets: () => ({ logicalAssets: {}, romfs: {} }) },
  });
  const pack = { name: "00-00000008.data", size: 20, sha256: "a".repeat(64) };
  const metadata = () => ({
    pack: { ...pack },
    records: [
      {
        path: "data/a",
        oldEkey: "1".repeat(32),
        newEkey: "2".repeat(32),
        encodedSize: 20,
        decodedSize: 30,
      },
    ],
  });
  let received, output, mode, unblock, overlayRevision, mapRevision;
  const generate = async ({ prebuiltPack, recipeRevision }) => {
    overlayRevision = recipeRevision;
    received = prebuiltPack;
    const files = {
      "romfs_overlay/default":
        mode === "oversize" ? new Uint8Array(80 * 1024 * 1024 + 1) : new Uint8Array([8]),
    };
    let astc;
    if (prebuiltPack || mode === "unbound") {
      for (const [prefix, names] of [
        [
          "romfs_overlay_astc",
          [
            "data/.build.config",
            "data/00-00000002.data",
            "ctrl_touch",
            "touch_strings.tsv",
            "pack.json",
          ],
        ],
        [
          "romfs_overlay_astc_variants/item_notes",
          [
            "data/.build.config",
            "data/00-00000002.data",
            "ctrl_touch",
            "touch_strings.tsv",
            "dm_textbox_plain.tsv",
            "features.txt",
          ],
        ],
      ])
        for (const name of names)
          files[`${prefix}/${name}`] = new TextEncoder().encode(
            name === "pack.json"
              ? JSON.stringify(pack)
              : name === "features.txt"
                ? "item_notes\n"
                : "x",
          );
      astc = {
        count: 1,
        pack: { ...pack },
        overlayArchives: { base: "00-00000002.data", item_notes: "00-00000002.data" },
      };
      if (recipeRevision === RELEASE_OVERLAY_REVISION) {
        for (const root of ["romfs_overlay_astc", "romfs_overlay_astc_variants/item_notes"]) {
          files[`${root}/xbox_glyphs`] = new TextEncoder().encode(
            "ztouch_controller_buttons_ui/switchx*.sprite\n",
          );
          const members = Object.fromEntries(
            Object.entries(files)
              .filter(([p]) => p.startsWith(root + "/"))
              .map(([p, b]) => [p.slice(root.length + 1), b]),
          );
          files[`${root}/overlay.manifest`] = await createOverlayManifest(members, {
            revision: recipeRevision,
          });
        }
      }
      if (mode === "missing") delete files["romfs_overlay_astc/ctrl_touch"];
    }
    if (mode === "late-oversize") {
      files["romfs_overlay/too-large"] = new Uint8Array(80 * 1024 * 1024).fill(3);
    }
    if (mode === "alias") files["romfs_overlay/alias"] = files["romfs_overlay/default"];
    if (mode === "subview") files["romfs_overlay/default"] = new Uint8Array([7, 8]).subarray(1);
    output = files;
    const result = { files, ...(astc ? { astc } : {}) };
    if (mode === "pending")
      return new Promise((resolve) => {
        unblock = () => resolve(result);
      });
    return result;
  };
  mock.module("../../native-overlay-generator.mjs", {
    namedExports: {
      NATIVE_OVERLAY_GENERATED: ["NativeHUD/v4/base.png"],
      generateNativeOverlay: generate,
    },
  });
  mock.module("../../map-goal-generator.mjs", {
    namedExports: {
      generateMapGoal: async ({ recipeRevision }) => {
        mapRevision = recipeRevision;
        return { files: { "synthetic.png": new Uint8Array([1]) } };
      },
    },
  });
  const { createAssetPipelineAdapter, ASSET_PIPELINE_LIMITS, snapshotPrebuiltPackMetadata } =
    await import("../../asset-pipeline-adapter.mjs");
  const options = (prebuiltPack) => ({
    nativeOverlayGenerated: { "NativeHUD/v4/base.png": new Uint8Array([1]) },
    prebuiltPack,
    createAssetWorker: () => ({ run() {}, close() {} }),
    createTextureClient: () => ({ decodeAtlas() {}, close() {} }),
  });
  test("optional binding appears only after complete successful generation and clears on dispose", async () => {
    mode = "pending";
    const input = metadata(),
      adapter = createAssetPipelineAdapter(options(input));
    assert.equal(adapter.getAstcOutputBinding(), undefined);
    const work = adapter.generateAssets({});
    while (!unblock) await new Promise((resolve) => setImmediate(resolve));
    assert.equal(adapter.getAstcOutputBinding(), undefined);
    assert.notEqual(received, input);
    assert.deepEqual(received, input);
    assert.ok(Object.isFrozen(received.records[0]));
    unblock();
    const entries = await work;
    unblock = undefined;
    assert.equal(entries.length, 16);
    const binding = adapter.getAstcOutputBinding();
    assert.deepEqual(Object.keys(binding).sort(), [
      "count",
      "overlayArchives",
      "pack",
      "recipeRevision",
    ]);
    assert.ok(Object.isFrozen(binding));
    assert.ok(Object.isFrozen(binding.pack));
    assert.ok(Object.isFrozen(binding.overlayArchives));
    assert.equal(binding.count, 1);
    assert.equal(binding.recipeRevision, RELEASE_OVERLAY_REVISION);
    assert.equal(binding.pack.name, pack.name);
    for (const bytes of Object.values(output)) assert.ok(bytes.some((n) => n !== 0));
    for (const entry of entries.filter((item) => item.path.startsWith("romfs_overlay_astc")))
      for await (const bytes of entry.chunks()) {
        assert.ok(bytes.length <= 64 * 1024);
        assert.ok(bytes.buffer.byteLength <= 64 * 1024);
      }
    const part = await entries[0].chunks().next();
    assert.ok(part.value.some((n) => n !== 0));
    const snapshot = part.value.slice();
    adapter.dispose();
    for (const bytes of Object.values(output)) assert.ok(bytes.every((n) => n === 0));
    assert.deepEqual(part.value, snapshot);
    assert.equal(adapter.getAstcOutputBinding(), undefined);
  });
  test("default pipeline has no ASTC binding and rejects unbound optional files", async () => {
    mode = undefined;
    const adapter = createAssetPipelineAdapter(options());
    assert.equal((await adapter.generateAssets({})).length, 1);
    assert.equal(adapter.getAstcOutputBinding(), undefined);
    adapter.dispose();
    mode = "unbound";
    const bad = createAssetPipelineAdapter(options());
    await assert.rejects(bad.generateAssets({}));
    assert.equal(bad.getAstcOutputBinding(), undefined);
  });
  test("exact15 validation and unchanged80MiB aggregate limit fail without binding", async () => {
    assert.equal(ASSET_PIPELINE_LIMITS.outputBytes, 80 * 1024 * 1024);
    for (const value of ["missing", "oversize"]) {
      mode = value;
      const adapter = createAssetPipelineAdapter(options(metadata()));
      await assert.rejects(
        adapter.generateAssets({}),
        value === "oversize" ? /OUTPUT_LIMIT/ : /FAILED/,
      );
      assert.equal(adapter.getAstcOutputBinding(), undefined);
      for (const bytes of Object.values(output)) assert.ok(bytes.every((n) => n === 0));
    }
  });
  test("metadata rejects payloads and out-of-bound counts before any generator work", () => {
    for (const value of [
      { ...metadata(), source: {} },
      { ...metadata(), bytes: new Uint8Array(1) },
      { ...metadata(), records: Array(32769).fill(metadata().records[0]) },
      { ...metadata(), records: [{ ...metadata().records[0], bytes: new Uint8Array(1) }] },
    ])
      assert.throws(() => snapshotPrebuiltPackMetadata(value));
    const input = metadata(),
      snap = snapshotPrebuiltPackMetadata(input);
    input.pack.name = "changed";
    assert.equal(snap.pack.name, pack.name);
  });

  test("native overlay output is adopted without a payload copy and chunks stay detached", async (t) => {
    mode = undefined;
    const slice = Uint8Array.prototype.slice;
    const observed = [];
    t.mock.method(Uint8Array.prototype, "slice", function (...args) {
      if (Object.values(output ?? {}).includes(this)) observed.push(this);
      return slice.apply(this, args);
    });
    const adapter = createAssetPipelineAdapter(options());
    const entries = await adapter.generateAssets({});
    assert.deepEqual(observed, []);
    const source = output["romfs_overlay/default"];
    const { value: chunk } = await entries[0].chunks().next();
    assert.deepEqual(observed, [source]);
    assert.notEqual(chunk.buffer, source.buffer);
    assert.deepEqual([...chunk], [8]);
    adapter.dispose();
    assert.deepEqual([...source], [0]);
    assert.deepEqual([...chunk], [8]);
  });

  for (const invalidMode of ["late-oversize", "alias", "subview"])
    test(`native overlay ${invalidMode} failure wipes adopted and scratch outputs`, async () => {
      mode = invalidMode;
      const adapter = createAssetPipelineAdapter(options());
      await assert.rejects(
        adapter.generateAssets({}),
        invalidMode === "late-oversize" ? /OUTPUT_LIMIT/ : /BYTES/,
      );
      for (const bytes of Object.values(output)) assert.ok(bytes.every((n) => n === 0));
      adapter.dispose();
    });

  test("pipeline forwards b811 to map-goal and overlay and retains exact ASTC revision", async () => {
    mode = undefined;
    const adapter = createAssetPipelineAdapter({
      ...options(metadata()),
      mapGoal: true,
      artRecipeRevision: RELEASE_OVERLAY_REVISION,
      overlayRecipeRevision: RELEASE_OVERLAY_REVISION,
    });
    const entries = await adapter.generateAssets({});
    assert.equal(mapRevision, RELEASE_OVERLAY_REVISION);
    assert.equal(overlayRevision, RELEASE_OVERLAY_REVISION);
    assert.equal(entries.filter((e) => e.path.startsWith("romfs_overlay_astc")).length, 15);
    assert.equal(adapter.getAstcOutputBinding().recipeRevision, RELEASE_OVERLAY_REVISION);
    adapter.dispose();
  });
}

// production-assets-astc
if (scenario === "production-assets-astc") {
  // 生产组合层使用隔离测试依赖验证发布时机；不增加生产注入或认证绕过接口。
  // Test publication timing in production composition with isolated dependencies, without production injection or authentication bypass APIs.
  const { default: assert } = await import("node:assert/strict");
  const { SHELL_PROFILE_IDS: SHELL_PROFILE_IDS, resolveShellProfile: resolveShellProfile } =
    await import("../../shell-profiles.mjs");
  const { mock: mock, test: test } = await import("node:test");
  const pack = { name: "00-00000008.data", size: 20, sha256: "a".repeat(64) };
  const prebuilt = {
    pack,
    records: [
      {
        path: "data/a",
        oldEkey: "1".repeat(32),
        newEkey: "2".repeat(32),
        encodedSize: 20,
        decodedSize: 30,
      },
    ],
  };
  const binding = Object.freeze({
    count: 1,
    pack: Object.freeze({ ...pack }),
    overlayArchives: Object.freeze({ base: "00-00000002.data", item_notes: "00-00000002.data" }),
  });
  const pipelineEntry = Object.freeze({
    path: "romfs_overlay_astc/mock",
    size: 1,
    async *chunks() {
      yield new Uint8Array([1]);
    },
  });
  let forwarded, failure, pipelineDisposed, hudRevision, pipelineRevisions, lifetimeCase;
  const output = (name, value, mayUseBuffer = false) => {
    const bytes = new Uint8Array([value]);
    lifetimeCase?.outputs.set(name, bytes);
    return mayUseBuffer && lifetimeCase?.arrayBuffers ? bytes.buffer : bytes;
  };
  const fileBatch = (stage, files) =>
    lifetimeCase?.invalidBatch === stage ? { invalid: "wrong type", ...files } : files;
  mock.module("../../extraction-job-adapter.mjs", {
    namedExports: {
      readAuthenticatedAssets: () => ({ logicalAssets: { read: async () => new Uint8Array(40) } }),
    },
  });
  mock.module("../../native-art-generator.mjs", {
    namedExports: {
      generateNativeArt: async () => ({
        files: fileBatch("native", {
          "dm_touch_native_art.c": output("art-c", 11, true),
          "native/icon.png": output("art-icon", 12, true),
        }),
      }),
    },
  });
  mock.module("../../hud-generator.mjs", {
    namedExports: {
      generateHud: async ({ recipeRevision }) => {
        hudRevision = recipeRevision;
        await lifetimeCase?.onStage?.("hud");
        const result = {
          files: fileBatch("hud", {
            "base.png": output("hud-base", 21, true),
            "hud.json": output("hud-json", 22, true),
            "hud_v4_ui/ui.json": output("hud-ui", 23, true),
          }),
        };
        await lifetimeCase?.onHudResult?.();
        return result;
      },
    },
  });
  mock.module("../../production-derived.mjs", {
    namedExports: {
      generateProductionDerived: async () => {
        await lifetimeCase?.onStage?.("derived");
        return {
          files: fileBatch("derived", { "dm/derived.tsv": output("derived", 31) }),
          counts: {},
        };
      },
    },
  });
  mock.module("../../frontend-panel-transform.mjs", {
    namedExports: {
      FRONTEND_PANEL_SOURCE: { input: "data/a" },
      transformFrontendPanel: () => ({ rgba: new Uint8Array(4), width: 1, height: 1 }),
    },
  });
  mock.module("../../asset-pipeline-adapter.mjs", {
    namedExports: {
      snapshotPrebuiltPackMetadata: (value) => value,
      createAssetPipelineAdapter: ({ prebuiltPack, artRecipeRevision, overlayRecipeRevision }) => {
        pipelineRevisions = {
          artRevision: artRecipeRevision,
          overlayRevision: overlayRecipeRevision,
        };
        forwarded = prebuiltPack;
        pipelineDisposed = false;
        return {
          async generateAssets() {
            await lifetimeCase?.onStage?.("pipeline");
            if (failure instanceof Error) throw failure;
            if (failure === "pipeline") throw new Error("pipeline failure");
            return [pipelineEntry];
          },
          getAstcOutputBinding: () => (prebuiltPack ? binding : undefined),
          dispose() {
            pipelineDisposed = true;
          },
        };
      },
    },
  });
  const { createProductionAssetAdapter } = await import("../../production-assets.mjs");
  function adapter(enabled = true, options = {}) {
    const factory = (name, client) => {
      options.created?.push(name);
      if (options.failFactory === name) throw new Error(`factory ${name} failed`);
      return {
        ...client,
        close() {
          options.closed?.push(name);
          options.onClose?.(name);
          if (failure === "close" || options.failClose === name) throw new Error("close failed");
        },
      };
    };
    const runtime = {
      createAssetWorker: () => factory("worker", {}),
      createTextureClient: () =>
        factory("texture", {
          decodeAtlas: async () => ({ pixels: new Uint8Array(4) }),
        }),
      createTextureEncoder: () =>
        factory("encoder", {
          encodeAndPack: async () => ({
            sprite: output("panel-sprite", 41),
            linearBC3: new Uint8Array(1),
          }),
        }),
    };
    return createProductionAssetAdapter({
      runtime,
      profile: {
        profileId: options.profileId ?? "v2.0.0-ios26",
        nativeArt: { manifest: "{}" },
        derivedCounts: options.derivedCounts ?? {},
      },
      ...(enabled ? { prebuiltPack: prebuilt } : {}),
    });
  }
  test("production adapter forwards only metadata and publishes binding after success", async () => {
    failure = undefined;
    const assets = adapter();
    assert.equal(assets.getAstcOutputBinding(), undefined);
    const entries = await assets.generateAssets({});
    assert.equal(entries[0], pipelineEntry);
    assert.equal(forwarded, prebuilt);
    assert.equal(assets.getAstcOutputBinding(), binding);
    assets.dispose();
    assert.equal(assets.getAstcOutputBinding(), undefined);
    assert.equal(pipelineDisposed, true);
  });
  test("production default, pipeline failure and final closure failure never publish ASTC binding", async () => {
    failure = undefined;
    const standard = adapter(false);
    await standard.generateAssets({});
    assert.equal(standard.getAstcOutputBinding(), undefined);
    standard.dispose();
    for (const mode of ["pipeline", "close"]) {
      failure = mode;
      const assets = adapter();
      await assert.rejects(assets.generateAssets({}));
      assert.equal(assets.getAstcOutputBinding(), undefined);
      failure = undefined;
      assets.dispose();
    }
  });

  test("production cleanup closes all created clients despite factory, generation or close failures", async () => {
    failure = undefined;
    for (const failFactory of ["worker", "texture", "encoder"]) {
      const created = [],
        closed = [];
      const assets = adapter(true, { created, closed, failFactory, failClose: "worker" });
      await assert.rejects(assets.generateAssets({}), { message: `factory ${failFactory} failed` });
      assert.deepEqual(closed, created.slice(0, -1));
      assert.equal(assets.getAstcOutputBinding(), undefined);
      assets.dispose();
    }
    for (const failClose of ["worker", "texture", "encoder"]) {
      const closed = [];
      const assets = adapter(true, { closed, failClose });
      await assert.rejects(assets.generateAssets({}), { message: "PRODUCTION_ASSET_CLEANUP" });
      assert.deepEqual(closed, ["worker", "texture", "encoder"]);
      assert.equal(assets.getAstcOutputBinding(), undefined);
      assets.dispose();
    }
    failure = "pipeline";
    const closed = [];
    const assets = adapter(true, { closed, failClose: "worker" });
    await assert.rejects(assets.generateAssets({}), {
      message: "pipeline failure",
      cleanupCode: "PRODUCTION_ASSET_CLEANUP",
    });
    assert.deepEqual(closed, ["worker", "texture", "encoder"]);
    assert.equal(assets.getAstcOutputBinding(), undefined);
    failure = undefined;
    assets.dispose();
  });

  test("cleanup annotation never replaces an error with a readonly cleanupCode", async () => {
    const primary = new Error("primary failure");
    Object.defineProperty(primary, "cleanupCode", { value: "existing cleanup code" });
    assert.equal(Object.isExtensible(primary), true);
    failure = primary;
    const closed = [];
    const assets = adapter(true, { closed, failClose: "worker" });
    await assert.rejects(assets.generateAssets({}), (error) => error === primary);
    assert.equal(primary.cleanupCode, "existing cleanup code");
    assert.deepEqual(closed, ["worker", "texture", "encoder"]);
    failure = undefined;
    assets.dispose();
  });

  test("production forwards only closed v2 recipes, never caller revisions", async () => {
    failure = undefined;
    for (const profileId of SHELL_PROFILE_IDS) {
      const assets = adapter(false, { profileId });
      await assets.generateAssets({});
      const expected = resolveShellProfile(profileId).assetRecipe;
      assert.equal(hudRevision, expected.artRevision);
      assert.deepEqual(pipelineRevisions, {
        artRevision: expected.artRevision,
        overlayRevision: expected.overlayRevision,
      });
      assets.dispose();
    }
    assert.throws(
      () => adapter(false, { profileId: "e3756f8942b40eba6c851e881828a5b1bda17fd6" }),
      /SHELL_PROFILE_UNKNOWN/,
    );
    assert.throws(
      () => createProductionAssetAdapter({ runtime: {}, profile: {} }),
      /PRODUCTION_ASSET_PROFILE/,
    );
  });

  const trackOutputs = (t, options = {}) => {
    failure = undefined;
    lifetimeCase = { outputs: new Map(), ...options };
    t.after(() => {
      lifetimeCase = undefined;
      failure = undefined;
    });
    return lifetimeCase;
  };
  const assertWiped = (outputs) => {
    assert.ok(outputs.size > 0);
    for (const [name, bytes] of outputs) assert.deepEqual([...bytes], [0], name);
  };

  test("production wipes intermediate outputs on HUD, derived and pipeline failure", async (t) => {
    for (const stage of ["hud", "derived", "pipeline"]) {
      for (const arrayBuffers of [false, true]) {
        const primary = new Error(`${stage} stopped`);
        const tracked = trackOutputs(t, {
          arrayBuffers,
          onStage(current) {
            if (current === stage) throw primary;
          },
        });
        const assets = adapter();
        await assert.rejects(assets.generateAssets({}), (error) => error === primary);
        assertWiped(tracked.outputs);
        assets.dispose();
        assertWiped(tracked.outputs);
      }
    }
  });

  test("production wipes returned derived bytes before rejecting capacity mismatch", async (t) => {
    const tracked = trackOutputs(t);
    const assets = adapter(true, { derivedCounts: { expected: 1 } });
    await assert.rejects(assets.generateAssets({}), { message: "SHELL_DERIVED_CAPACITY_MISMATCH" });
    assert.equal(tracked.outputs.has("derived"), true);
    assertWiped(tracked.outputs);
    assets.dispose();
  });

  test("production wipes valid siblings after an invalid-first generated batch", async (t) => {
    for (const invalidBatch of ["native", "hud", "derived"]) {
      const tracked = trackOutputs(t, { invalidBatch, arrayBuffers: true });
      const assets = adapter();
      await assert.rejects(assets.generateAssets({}), { message: "PRODUCTION_ASSET_OUTPUT_TYPE" });
      assertWiped(tracked.outputs);
      assets.dispose();
      assertWiped(tracked.outputs);
    }
  });

  test("production wipes all generated output on pipeline cancellation or final close failure", async (t) => {
    for (const mode of ["pipeline-abort", "close-abort", "close-failure"]) {
      const controller = new AbortController();
      const primary = new Error("cancelled generation");
      const tracked = trackOutputs(t, {
        onStage(stage) {
          if (stage === "pipeline" && mode === "pipeline-abort") controller.abort(primary);
        },
      });
      const assets = adapter(true, {
        failClose: mode === "close-failure" ? "worker" : undefined,
        onClose(name) {
          if (name === "worker" && mode === "close-abort") controller.abort(primary);
        },
      });
      await assert.rejects(
        assets.generateAssets({ signal: controller.signal }),
        mode === "close-failure"
          ? { message: "PRODUCTION_ASSET_CLEANUP" }
          : (error) => error === primary,
      );
      assert.equal(tracked.outputs.has("panel-sprite"), true);
      assertWiped(tracked.outputs);
      assert.equal(assets.getAstcOutputBinding(), undefined);
      assets.dispose();
    }
  });

  test("production disposal wipes prior outputs and later returned HUD bytes", async (t) => {
    let releaseHud, hudReady;
    const ready = new Promise((resolve) => (hudReady = resolve));
    const gate = new Promise((resolve) => (releaseHud = resolve));
    const tracked = trackOutputs(t, {
      async onHudResult() {
        hudReady();
        await gate;
      },
    });
    const assets = adapter();
    const pending = assets.generateAssets({});
    await ready;
    assets.dispose();
    assert.deepEqual([...tracked.outputs.get("art-icon")], [0]);
    assert.deepEqual([...tracked.outputs.get("hud-ui")], [23]);
    releaseHud();
    await assert.rejects(pending, { message: "PRODUCTION_ASSET_DISPOSED" });
    assertWiped(tracked.outputs);
  });

  test("production retains success entry bytes until dispose without wiping shared HUD UI early", async (t) => {
    const tracked = trackOutputs(t, {
      arrayBuffers: true,
      onStage() {
        for (const [name, bytes] of tracked.outputs) assert.notEqual(bytes[0], 0, name);
      },
    });
    const assets = adapter();
    const entries = await assets.generateAssets({});
    for (const name of ["art-c", "hud-base", "hud-json", "panel-sprite"])
      assert.deepEqual([...tracked.outputs.get(name)], [0], name);
    for (const [path, name, value] of [
      ["native/icon.png", "art-icon", 12],
      ["hud_v4_ui/ui.json", "hud-ui", 23],
      ["dm/derived.tsv", "derived", 31],
    ]) {
      const item = entries.find((entry) => entry.path === path);
      for (let repeat = 0; repeat < 2; repeat++) {
        const chunks = [];
        for await (const bytes of item.chunks()) chunks.push(bytes);
        assert.equal(chunks[0].buffer, tracked.outputs.get(name).buffer);
        assert.deepEqual([...chunks[0]], [value], name);
      }
    }
    assert.equal(assets.getAstcOutputBinding(), binding);
    assets.dispose();
    assertWiped(tracked.outputs);
    const item = entries.find((entry) => entry.path === "native/icon.png");
    await assert.rejects(item.chunks().next(), { message: "PRODUCTION_ASSET_DISPOSED" });
  });
}
