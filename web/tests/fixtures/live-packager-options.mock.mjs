// 合成运行时与适配器验证实时选项；不运行编译器、WASM、浏览器或私有输入。
// Synthetic runtimes/adapters verify live options; no compiler, WASM, browser or private inputs.
import test, { mock } from "node:test";
import assert from "node:assert/strict";
import { BUNDLED_SHELL_PROFILE_IDS } from "../../shell-profiles.mjs";
let state;
const stop = new Error("Synthetic job stopped before real input");
const runtime = {
  takeIpaSigningFactory: () => ({ prepare: async () => ({ close() {} }), close() {} }),
  prepareAstcProducer: async () => {
    state.producerPrepares++;
  },
  createAstcProducerClient: () => ({
    async produce(_input, _selection, options) {
      state.preset = options.preset;
      return { pack: {}, records: [] };
    },
    close() {},
  }),
  retireAssetWorkers() {},
  close() {
    state.runtimeCloses++;
  },
};
mock.module("../../sealed-runtime/clients.mjs", {
  namedExports: {
    async prepareSealedRuntime(options) {
      state.bootstrap = options;
      return runtime;
    },
  },
});
mock.module("../../production-profile.mjs", {
  namedExports: {
    async loadProductionProfile(_runtime, { profileId }) {
      state.loadedProfiles.push(profileId);
      if (state.profileReplacementPause) {
        state.profileReplacementEntered.resolve();
        await state.profileReplacementPause.promise;
      }
      return {
        profileId,
        contentMode: "synthetic",
        shell: { contract: {} },
        trustedShell: { trustedRecipe: { requiredPaths: [], product: "test" } },
        close() {
          state.closedProfiles.push(profileId);
        },
      };
    },
    bindProductionAstcEntries() {
      throw new Error("Not used");
    },
    inspectProductionAstcEntries() {
      throw new Error("Not used");
    },
  },
});
mock.module("../../extraction-job-adapter.mjs", {
  namedExports: {
    createExtractionJobAdapter(options) {
      state.extraction = options;
      return { extractSelected: async () => ({}), cleanup() {} };
    },
    readAuthenticatedProgramModules() {},
    readAuthenticatedAssets() {
      throw new Error("No private reads");
    },
    readInputContentPolicy() {
      throw new Error("No private reads");
    },
  },
});
mock.module("../../program-rewriter-adapter.mjs", {
  namedExports: {
    createProgramRewriterAdapter(options) {
      state.program = options;
      return { dispose() {} };
    },
  },
});
mock.module("../../production-assets.mjs", {
  namedExports: {
    createProductionAssetAdapter() {
      return { generateAssets: async () => [], dispose() {} };
    },
  },
});
mock.module("../../production-shaders.mjs", {
  namedExports: {
    createProductionShaderAdapter() {
      return { dispose() {} };
    },
  },
});
mock.module("../../local-output-cache.mjs", {
  namedExports: {
    async createLocalOutputCache(options) {
      state.cacheOptions = options;
      if (state.cachePause) {
        state.cacheEntered.resolve();
        await state.cachePause.promise;
      }
      return {
        close() {
          state.cacheCloses++;
        },
      };
    },
  },
});
mock.module("../../packaging-job.mjs", {
  namedExports: {
    PackagingJobError: class extends Error {},
    async runPackagingJob(options) {
      state.job = options;
      await options.adapters.extractSelected({ selection: {} });
      await options.adapters.generateAssets({ signal: options.signal });
      throw stop;
    },
  },
});
globalThis.isSecureContext = true;
globalThis.Worker = class {};
Object.defineProperty(globalThis, "navigator", {
  value: { locks: { request() {} }, storage: { getDirectory: async () => ({ synthetic: true }) } },
  configurable: true,
});
globalThis.fetch = () => {
  throw new Error("Network forbidden after bootstrap");
};
const { prepareBrowserPackager } = await import("../../browser-packager.mjs");
function reset() {
  state = {
    loadedProfiles: [],
    closedProfiles: [],
    producerPrepares: 0,
    runtimeCloses: 0,
    cacheCloses: 0,
  };
}
const input = { kind: "directory", queryPermission: async () => "granted" };

test("editable bootstrap retains all public capabilities without allocating optional workers", async () => {
  reset();
  const packager = await prepareBrowserPackager({ editableOptions: true });
  assert.deepEqual(state.bootstrap.preloadProfileIds, BUNDLED_SHELL_PROFILE_IDS);
  assert.deepEqual(state.bootstrap.optionalGroups, ["legacy-astc", "ipa-signing"]);
  assert.equal(state.bootstrap.deferOptionalInitialization, true);
  assert.equal(state.producerPrepares, 0);
  assert.equal(state.extraction, undefined);
  assert.equal(packager.signingReady, true);
  await packager.close();
});

test("generation uses changed target, legacy choice and preset from one snapshot", async () => {
  for (const profileId of BUNDLED_SHELL_PROFILE_IDS)
    for (const legacyAstc of [false, true])
      for (const astcPreset of ["thorough", "fast"]) {
        reset();
        const packager = await prepareBrowserPackager({ editableOptions: true });
        const controller = new AbortController();
        const options = {
          input,
          profileId,
          legacyAstc,
          astcPreset,
          exportResources: false,
          signal: controller.signal,
        };
        const generating = packager.generate(options);
        // 调用方修改不能改变正在运行的解构任务快照。
        // Caller changes cannot alter an in-flight destructured job snapshot.
        options.legacyAstc = !legacyAstc;
        options.astcPreset = astcPreset === "fast" ? "thorough" : "fast";
        await assert.rejects(generating, (error) => error === stop);
        assert.equal(state.program.profileId, profileId);
        assert.equal(state.loadedProfiles.at(-1), profileId);
        assert.equal(state.extraction.skipContentHashes, undefined);
        assert.equal(state.job.selectionOptions.skipContentHashes, undefined);
        assert.equal(state.producerPrepares, legacyAstc ? 1 : 0);
        assert.equal(state.preset, legacyAstc ? astcPreset : undefined);
        await assert.rejects(packager.generate({ input }), /RELOAD_REQUIRED/);
        await packager.close();
      }
});

test("unprepared capabilities reject instead of silently ignoring options or fetching", async () => {
  reset();
  const packager = await prepareBrowserPackager();
  await assert.rejects(packager.generate({ input, legacyAstc: true }), /LEGACY_ASTC_NOT_PRELOADED/);
  await assert.rejects(
    packager.generate({ input, profileId: BUNDLED_SHELL_PROFILE_IDS[1] }),
    /PROFILE_NOT_PRELOADED/,
  );
  assert.equal(state.job, undefined);
  assert.equal(state.extraction, undefined);
  await packager.close();
});

for (const action of ["cancel", "close"])
  test(`${action} during profile replacement releases late public bytes before private work`, async () => {
    reset();
    const packager = await prepareBrowserPackager({ editableOptions: true });
    const initialProfileId = state.loadedProfiles[0];
    const profileId = BUNDLED_SHELL_PROFILE_IDS.find((id) => id !== initialProfileId);
    assert.ok(profileId);
    state.profileReplacementEntered = Promise.withResolvers();
    state.profileReplacementPause = Promise.withResolvers();
    const controller = new AbortController();
    const generating = packager.generate({ input, profileId, signal: controller.signal });
    await state.profileReplacementEntered.promise;
    if (action === "close") await packager.close();
    else controller.abort();
    // 模拟经过验证的内存加载器在操作结束后才返回。
    // Simulate a verified in-memory loader returning after the operation ended.
    state.profileReplacementPause.resolve();
    await assert.rejects(
      generating,
      action === "close" ? /RELOAD_REQUIRED/ : { name: "AbortError" },
    );
    assert.equal(state.closedProfiles.at(-1), profileId);
    assert.equal(state.closedProfiles.filter((id) => id === profileId).length, 1);
    assert.equal(state.extraction, undefined);
    assert.equal(state.program, undefined);
    assert.equal(state.job, undefined);
    assert.equal(state.producerPrepares, 0);
    assert.equal(state.runtimeCloses, 1);
    await packager.close();
  });

for (const action of ["cancel", "close"])
  test(`${action} during cache acquisition closes the late cache before private work`, async () => {
    reset();
    const packager = await prepareBrowserPackager();
    state.cacheEntered = Promise.withResolvers();
    state.cachePause = Promise.withResolvers();
    const controller = new AbortController();
    const generating = packager.generate({ input, ignoreCache: true, signal: controller.signal });
    await state.cacheEntered.promise;
    assert.equal(state.cacheOptions.ignoreCache, true);
    if (action === "close") await packager.close();
    else controller.abort();
    state.cachePause.resolve();
    await assert.rejects(
      generating,
      action === "close" ? /RELOAD_REQUIRED/ : { name: "AbortError" },
    );
    assert.equal(state.cacheCloses, 1);
    assert.equal(state.extraction, undefined);
    assert.equal(state.job, undefined);
    await packager.close();
    assert.equal(state.cacheCloses, 1);
  });
