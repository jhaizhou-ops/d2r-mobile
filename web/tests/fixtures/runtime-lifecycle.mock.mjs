// Each selected scenario runs in its own child process; module mocks and globals never cross scenarios.
import process from "node:process";
const scenario = process.env.D2R_TEST_SCENARIO;
if (
  ![
    "runtime-cache-cancellation",
    "runtime-optional-bootstrap",
    "runtime-shell-build-inputs",
    "runtime-stream-cancellation",
    "runtime-worker-lifecycle",
  ].includes(scenario)
)
  throw new Error("Unknown isolated mock scenario: " + scenario);

// runtime-cache-cancellation
if (scenario === "runtime-cache-cancellation") {
  // 使用延迟的合成缓存与哈希验证取消边界，不读取私有输入或执行真实工作线程。
  // Check cancellation boundaries with delayed synthetic cache/hash results, without private inputs or actual workers.
  const { default: test, mock: mock } = await import("node:test");
  const { default: assert } = await import("node:assert/strict");
  const { createHash: createHash } = await import("node:crypto");

  const encode = (text) => new TextEncoder().encode(text);
  const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const shell = encode("synthetic pinned shell");
  const profile = {
    id: "synthetic-profile",
    archivePath: "profile/shell.zip",
    evidence: { archiveBytes: shell.length, sha256: sha(shell) },
  };
  mock.module("../../shell-profiles.mjs", {
    namedExports: { SHELL_PROFILE_IDS: [profile.id], resolveShellProfile: () => profile },
  });
  globalThis.isSecureContext = true;
  globalThis.location = { href: "https://example.test/", origin: "https://example.test" };
  Object.defineProperty(globalThis, "navigator", { value: {}, configurable: true });
  let serial = 0;

  async function setup(
    t,
    { hits = false, delayMatch = false, delayHash = false, delayPut = false } = {},
  ) {
    const controller = new AbortController();
    const data = new Map([
      [profile.archivePath, shell],
      ["sealed-workers/program-worker.mjs", encode("synthetic public worker")],
      ["toolchain/program-rewrite-spike/dist/capstone-lz4.wasm", encode("synthetic public wasm")],
    ]);
    const assets = [...data].map(([path, bytes]) => ({
      path,
      bytes: bytes.length,
      sha256: sha(bytes),
    }));
    const manifest = encode(
      JSON.stringify({
        version: 1,
        assets,
        totalBytes: assets.reduce((sum, item) => sum + item.bytes, 0),
      }),
    );
    const byKey = new Map(
      assets.map((asset) => [
        `https://example.test/runtime/__public-runtime-cache__/${asset.sha256}-${asset.bytes}`,
        data.get(asset.path),
      ]),
    );
    const state = { fetches: [], puts: [], deletes: [], progress: [], workers: 0, finishedPuts: 0 };
    const ready = Promise.withResolvers(),
      release = Promise.withResolvers();
    const pending = [];
    let waiting = 0;
    async function delay() {
      if (++waiting === assets.length) ready.resolve();
      await release.promise;
    }
    function delayedResult(operation) {
      const result = delay().then(operation);
      pending.push(result);
      return result;
    }
    let clock = 0;
    t.mock.method(performance, "now", () => (clock += 101));
    globalThis.Worker = class {
      constructor() {
        state.workers++;
        throw new Error("Unexpected worker after cancellation");
      }
    };
    globalThis.fetch = async (url) => {
      const path = new URL(url).pathname.slice("/runtime/".length);
      state.fetches.push(path);
      return new Response(path === "runtime-manifest.json" ? manifest : data.get(path));
    };
    globalThis.caches = {
      async open() {
        return {
          async match(key) {
            if (delayMatch)
              return delayedResult(() => (hits ? new Response(byKey.get(key)) : undefined));
            return hits ? new Response(byKey.get(key)) : undefined;
          },
          async put(key, response) {
            state.puts.push(key);
            assert.equal(
              sha(new Uint8Array(await response.arrayBuffer())),
              key.split("/").at(-1).split("-")[0],
            );
            if (delayPut) await delayedResult(() => undefined);
            state.finishedPuts++;
          },
          async delete(key) {
            state.deletes.push(key);
          },
          async keys() {
            return [];
          },
        };
      },
    };
    if (delayHash) {
      const digest = crypto.subtle.digest.bind(crypto.subtle);
      let calls = 0;
      t.mock.method(crypto.subtle, "digest", async (...args) => {
        if (++calls > 1) return delayedResult(() => digest(...args));
        return digest(...args);
      });
    }
    const { prepareSealedRuntime } = await import(
      `../../sealed-runtime/clients.mjs?cache-cancel=${serial++}`
    );
    const running = prepareSealedRuntime({
      baseUrl: "https://example.test/runtime/",
      manifestSha256: sha(manifest),
      components: ["program"],
      signal: controller.signal,
      onProgress: (progress) => state.progress.push(progress),
    });
    const cancelled = assert.rejects(running, /synthetic cancellation/);
    await ready.promise;
    controller.abort(new Error("synthetic cancellation"));
    const progressAtAbort = state.progress.length;
    release.resolve();
    await cancelled;
    await Promise.allSettled(pending);
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(state.progress.length, progressAtAbort);
    assert.equal(state.workers, 0);
    assert.deepEqual(state.deletes, []);
    return state;
  }

  test("delayed cached responses cannot retain bytes or report progress after cancellation", async (t) => {
    const state = await setup(t, { hits: true, delayMatch: true });
    assert.deepEqual(state.fetches, ["runtime-manifest.json"]);
    assert.deepEqual(state.puts, []);
  });
  test("delayed cached hashes do not delete valid entries or report cancelled progress", async (t) => {
    const state = await setup(t, { hits: true, delayHash: true });
    assert.deepEqual(state.fetches, ["runtime-manifest.json"]);
    assert.deepEqual(state.puts, []);
  });
  test("delayed downloaded hashes cannot start cache writes after cancellation", async (t) => {
    const state = await setup(t, { delayHash: true });
    assert.equal(state.fetches.length, 4);
    assert.deepEqual(state.puts, []);
  });
  test("already-started verified cache writes may finish without rollback or progress", async (t) => {
    const state = await setup(t, { delayPut: true });
    assert.equal(state.puts.length, 3);
    assert.equal(state.finishedPuts, 3);
  });
}

// runtime-optional-bootstrap
if (scenario === "runtime-optional-bootstrap") {
  // 用合成公开字节、外壳注册表和工作线程验证引导顺序；不读取私有输入或执行真实编码器。
  // Verify bootstrap ordering with synthetic public bytes, shell registry and workers; no private inputs or actual codecs.
  const { default: test, mock: mock } = await import("node:test");
  const { default: assert } = await import("node:assert/strict");
  const { createHash: createHash } = await import("node:crypto");
  const { OPTIONAL_RUNTIME_ASSET_GROUPS: OPTIONAL_RUNTIME_ASSET_GROUPS } =
    await import("../../runtime-asset-groups.mjs");
  const { selectAirCompilerConcurrency: selectAirCompilerConcurrency } =
    await import("../../air-compiler-worker-client.mjs");
  const {
    AIR_COMPILER_RUNTIME: AIR_COMPILER_RUNTIME,
    airCompilerRuntimeComponent: airCompilerRuntimeComponent,
  } = await import("../../air-compiler-runtime-contract.mjs");
  const {
    RUNTIME_DIRECT_ASSETS: RUNTIME_DIRECT_ASSETS,
    RUNTIME_SEALED_WORKERS: RUNTIME_SEALED_WORKERS,
    SOURCE_ONLY_RUNTIME_ASSETS: SOURCE_ONLY_RUNTIME_ASSETS,
  } = await import("../../runtime-build/payload-policy.mjs");
  const encode = (text) => new TextEncoder().encode(text),
    sha = (bytes) => createHash("sha256").update(bytes).digest("hex"),
    shell = encode("synthetic pinned shell");
  const compilerFactory = encode("synthetic compiler factory, never executed");
  const compilerWasm = encode("synthetic compiler WASM, preparation is mocked");
  const compilerReceipt = {
    version: AIR_COMPILER_RUNTIME.version,
    id: AIR_COMPILER_RUNTIME.id,
    apiVersion: AIR_COMPILER_RUNTIME.apiVersion,
    headerBundleVersion: AIR_COMPILER_RUNTIME.headerBundleVersion,
    sourceLanguage: AIR_COMPILER_RUNTIME.sourceLanguage,
    emscripten: "4.0.20",
    headerSha256: sha(encode("synthetic headers")),
    sourceFingerprintSha256: sha(encode("synthetic source fingerprint")),
    sourceManifestSha256: sha(encode("synthetic source manifest")),
    sourceCommit: "a".repeat(40),
    sourceInputs: { "toolchain.mjs": sha(encode("synthetic toolchain")) },
    files: {
      "d2r_air_wasm.mjs": { bytes: compilerFactory.length, sha256: sha(compilerFactory) },
      "d2r_air_wasm.wasm": { bytes: compilerWasm.length, sha256: sha(compilerWasm) },
    },
  };
  const profile = {
    id: "synthetic-profile",
    archivePath: "profile/shell.zip",
    evidence: { archiveBytes: shell.length, sha256: sha(shell) },
  };
  mock.module("../../shell-profiles.mjs", {
    namedExports: { SHELL_PROFILE_IDS: [profile.id], resolveShellProfile: () => profile },
  });
  let state;
  mock.module("../../astc-producer-client.mjs", {
    namedExports: {
      async createAstcProducerClient({ worker, assets, signal }) {
        state.helperCalls++;
        assert.equal(state.fetches.length, state.expectedFetches);
        assert.ok(signal instanceof AbortSignal);
        assert.equal(signal.aborted, state.signal.aborted);
        assert.deepEqual(
          assets.map(([name]) => name),
          ["astc-encoder.mjs", "astc-encoder.wasm", "png-codec.wasm", "sha256.wasm"],
        );
        const buffers = assets.map(([, value]) => value);
        structuredClone(assets, { transfer: buffers });
        assert.ok(buffers.every((value) => value.byteLength === 0));
        state.initializationSignal = signal;
        state.initializationEntered?.resolve();
        await state.initializationPause?.promise;
        if (state.initializationError) throw state.initializationError;
        let clientClosed = false;
        const client = Object.freeze({
          initialized: !state.badProof,
          ready: true,
          produce() {
            state.privateCalls++;
          },
          async close() {
            if (clientClosed) return;
            clientClosed = true;
            state.producerCloses++;
            worker.terminate();
            if (state.cleanupError) throw state.cleanupError;
          },
        });
        state.client = client;
        return client;
      },
    },
  });
  class Worker extends EventTarget {
    constructor(url) {
      super();
      this.url = url;
      this.terminated = false;
      state.workers.push(this);
    }
    postMessage(message, transfers) {
      const cloned = structuredClone(message, { transfer: transfers });
      assert.equal(cloned.type, "initialize");
      if (cloned.factoryBytes instanceof ArrayBuffer) {
        assert.equal(state.fetches.length, state.expectedFetches);
        assert.deepEqual(new Uint8Array(cloned.factoryBytes), compilerFactory);
        assert.deepEqual(cloned.wasmModule, { syntheticCompilerModule: true });
        assert.equal(cloned.factorySha256, sha(compilerFactory));
        assert.equal(cloned.wasmSha256, sha(compilerWasm));
        assert.equal(cloned.headerSha256, compilerReceipt.headerSha256);
        this.assetNames = ["factory", "wasm"];
        state.compilerInitializations++;
        queueMicrotask(() =>
          this.dispatchEvent(
            new MessageEvent("message", {
              data: { type: "initialized", id: cloned.id, heapBytes: 4096 },
            }),
          ),
        );
        return;
      }
      this.assetNames = cloned.assets.map(([name]) => name);
      queueMicrotask(() =>
        this.onmessage({ data: { id: 0, ok: true, result: { initialized: true } } }),
      );
    }
    terminate() {
      this.terminated = true;
    }
  }
  globalThis.isSecureContext = true;
  globalThis.Worker = Worker;
  globalThis.location = { href: "https://example.test/", origin: "https://example.test" };
  Object.defineProperty(globalThis, "navigator", { value: {}, configurable: true });
  globalThis.caches = {
    open() {
      throw new Error("Test cache unavailable");
    },
  };
  let serial = 0;
  async function setup({
    enabled = false,
    absent = false,
    full = false,
    missingAirComponent = false,
    pageUrl = "https://example.test/",
  } = {}) {
    globalThis.location = { href: pageUrl, origin: new URL(pageUrl).origin };
    const runtimeBase = new URL("./runtime/", pageUrl);
    state = {
      fetches: [],
      workers: [],
      helperCalls: 0,
      privateCalls: 0,
      producerCloses: 0,
      compilerInitializations: 0,
      signal: new AbortController().signal,
    };
    const data = new Map([
      [profile.archivePath, shell],
      ["sealed-workers/program-worker.mjs", encode("synthetic public program worker")],
      [
        "toolchain/program-rewrite-spike/dist/capstone-lz4.wasm",
        encode("synthetic public program wasm"),
      ],
      ["toolchain/asset-runtime-js/dist/png-codec.wasm", encode("synthetic public PNG wasm")],
    ]);
    if (full) {
      for (const path of [...RUNTIME_DIRECT_ASSETS, ...RUNTIME_SEALED_WORKERS])
        if (!data.has(path)) data.set(path, encode(`synthetic ${path}`));
      data.set(AIR_COMPILER_RUNTIME.factory, compilerFactory);
      data.set(AIR_COMPILER_RUNTIME.wasm, compilerWasm);
      data.set(AIR_COMPILER_RUNTIME.receipt, encode(JSON.stringify(compilerReceipt)));
      const closure = (directory, names) => {
        const assets = names.map((path) => {
          const bytes = encode(`synthetic ${directory}/${path}`);
          data.set(`${directory}/${path}`, bytes);
          return { path, bytes: bytes.length, sha256: sha(bytes) };
        });
        data.set(`${directory}/manifest.json`, encode(JSON.stringify({ version: 1, assets })));
      };
      closure("toolchain/asset-runtime-js/dist", [
        "worker.mjs",
        "image-kernels.wasm",
        "png-codec.wasm",
        "font-rasterizer.wasm",
      ]);
      closure("toolchain/offline-runtime-spike/dotnet-dist", [
        "worker.mjs",
        "dotnet.boot.js",
        "dotnet.js",
        "dotnet.native.synthetic.js",
        "dotnet.runtime.synthetic.js",
        "runtime.wasm",
      ]);
    }
    if (!absent)
      for (const path of OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"])
        data.set(path, encode(`synthetic ${path}`));
    const assets = [...data].map(([path, bytes]) => ({
      path,
      bytes: bytes.length,
      sha256: sha(bytes),
      ...(OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"].includes(path)
        ? { optionalGroup: "legacy-astc" }
        : {}),
    }));
    const manifest = {
        version: 1,
        ...(full && !missingAirComponent
          ? { components: { airCompiler: airCompilerRuntimeComponent(compilerReceipt) } }
          : {}),
        assets,
        totalBytes: assets.reduce((sum, asset) => sum + asset.bytes, 0),
      },
      raw = encode(JSON.stringify(manifest));
    data.set("runtime-manifest.json", raw);
    state.expectedFetches = 1 + assets.filter((asset) => enabled || !asset.optionalGroup).length;
    globalThis.fetch = async (url) => {
      const requested = new URL(url);
      assert.equal(requested.origin, runtimeBase.origin);
      assert.ok(requested.pathname.startsWith(runtimeBase.pathname));
      const path = requested.pathname.slice(runtimeBase.pathname.length);
      state.fetches.push(path);
      assert.ok(data.has(path), `Unexpected public fetch: ${path}`);
      return new Response(data.get(path));
    };
    const module = await import(`../../sealed-runtime/clients.mjs?bootstrap=${serial++}`);
    return {
      ...module,
      data,
      options: {
        baseUrl: "./runtime/",
        manifestSha256: sha(raw),
        ...(full ? {} : { components: ["program"] }),
        signal: state.signal,
        ...(enabled ? { optionalGroups: ["legacy-astc"] } : {}),
      },
    };
  }
  test("default setup downloads no optional closure and creates no producer", async () => {
    const { prepareSealedRuntime, options, assertSealedRuntime } = await setup();
    const runtime = await prepareSealedRuntime(options);
    assert.equal(assertSealedRuntime(runtime), runtime);
    assert.equal(state.workers.length, 1);
    assert.equal(state.helperCalls, 0);
    assert.equal(state.fetches.length, 5);
    assert.ok(
      state.fetches.every((path) => !OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"].includes(path)),
    );
    assert.throws(() => runtime.createAstcProducerClient(), /unavailable/);
    assert.equal(state.fetches.length, 5);
    await runtime.close();
    assert.throws(() => assertSealedRuntime(runtime), /live verified/);
  });
  test("opt-in verifies all public bytes and initializes before single acquisition", async () => {
    const { prepareSealedRuntime, options } = await setup({ enabled: true });
    const runtime = await prepareSealedRuntime(options);
    assert.equal(state.workers.length, 2);
    assert.equal(state.helperCalls, 1);
    assert.equal(state.privateCalls, 0);
    assert.deepEqual(runtime.initialized.astcProducer, { initialized: true });
    assert.equal(runtime.createAstcProducerClient(), state.client);
    assert.throws(() => runtime.createAstcProducerClient(), /already acquired/);
    const pending = runtime.close();
    assert.equal(runtime.close(), pending);
    await pending;
    assert.equal(state.producerCloses, 1);
    assert.ok(state.workers.every((worker) => worker.terminated));
  });
  test("unknown group rejects before network and unavailable group rejects before workers", async () => {
    const unknown = await setup();
    await assert.rejects(
      unknown.prepareSealedRuntime({ ...unknown.options, optionalGroups: ["unknown"] }),
      /group selection/,
    );
    assert.equal(state.fetches.length, 0);
    const absent = await setup({ enabled: true, absent: true });
    await assert.rejects(absent.prepareSealedRuntime(absent.options), /unavailable/);
    assert.deepEqual(state.fetches, ["runtime-manifest.json"]);
    assert.equal(state.workers.length, 0);
  });
  test("modified optional codec bytes fail before any worker or private operation", async () => {
    const { prepareSealedRuntime, data, options } = await setup({ enabled: true });
    data.set("toolchain/sha256-stream-wasm/dist/sha256.wasm", encode("tampered"));
    await assert.rejects(prepareSealedRuntime(options), /integrity mismatch/);
    assert.equal(state.workers.length, 0);
    assert.equal(state.privateCalls, 0);
  });
  test("failed initialization proof closes all workers and cannot create a session", async () => {
    const { prepareSealedRuntime, options } = await setup({ enabled: true });
    state.badProof = true;
    state.cleanupError = new Error("synthetic cleanup failure");
    await assert.rejects(prepareSealedRuntime(options), /initialization\/transfer failed/);
    assert.equal(state.producerCloses, 1);
    assert.ok(state.workers.every((worker) => worker.terminated));
  });
  test("cleanup failure remains awaitable while runtime identity is revoked", async () => {
    const { prepareSealedRuntime, options, assertSealedRuntime } = await setup({ enabled: true });
    const runtime = await prepareSealedRuntime(options);
    state.cleanupError = new Error("synthetic cleanup failure");
    const closing = runtime.close();
    assert.throws(() => assertSealedRuntime(runtime), /live verified/);
    assert.throws(() => runtime.createAstcProducerClient(), /unavailable/);
    await assert.rejects(closing, /synthetic cleanup failure/);
  });
  test("incomplete AIR component metadata rejects before worker creation", async () => {
    const { prepareSealedRuntime, options } = await setup({
      full: true,
      missingAirComponent: true,
    });
    await assert.rejects(
      prepareSealedRuntime(options),
      /Verified AIR compiler component is missing/,
    );
    assert.deepEqual(state.fetches, ["runtime-manifest.json"]);
    assert.equal(state.workers.length, 0);
    assert.equal(state.privateCalls, 0);
  });
  test("GitHub Pages project paths keep runtime fetches and shader-worker URLs under the project", async (t) => {
    t.mock.method(WebAssembly, "compile", async () => ({ syntheticCompilerModule: true }));
    const { prepareSealedRuntime, options } = await setup({
      full: true,
      enabled: true,
      pageUrl: "https://n3crosis.github.io/d2r-mobile/",
    });
    const runtime = await prepareSealedRuntime(options);
    assert.equal(state.fetches.length, state.expectedFetches);
    assert.equal(Object.keys(runtime.workerUrls).length, 2);
    for (const url of Object.values(runtime.workerUrls))
      assert.ok(url.startsWith("https://n3crosis.github.io/d2r-mobile/runtime/"));
    assert.ok(state.workers.every((worker) => worker.url.startsWith("blob:")));
    await runtime.close();
    assert.ok(state.workers.every((worker) => worker.terminated));
  });

  test("every production plan initializes from retained assembly paths without raw client sources", async (t) => {
    let compilerPreparations = 0;
    t.mock.method(WebAssembly, "compile", async (bytes) => {
      assert.deepEqual(bytes, compilerWasm);
      compilerPreparations++;
      return { syntheticCompilerModule: true };
    });
    const { prepareSealedRuntime, options, data } = await setup({ enabled: true, full: true });
    assert.ok(SOURCE_ONLY_RUNTIME_ASSETS.every((path) => !data.has(path)));
    const runtime = await prepareSealedRuntime(options);
    assert.deepEqual(Object.keys(runtime.initialized), [
      "assets",
      "translator",
      "texture",
      "extraction",
      "spirvCross",
      "program",
      "airCompiler",
      "astcProducer",
    ]);
    const compilerWorkers = selectAirCompilerConcurrency();
    assert.equal(
      state.workers.length,
      Object.keys(runtime.initialized).length - 1 + compilerWorkers,
    );
    assert.equal(compilerPreparations, 1);
    assert.equal(state.compilerInitializations, compilerWorkers);
    assert.equal(state.privateCalls, 0);
    assert.equal(state.fetches.length, state.expectedFetches);
    assert.equal(state.helperCalls, 1);
    assert.deepEqual(
      state.workers.slice(0, -1).map((worker) => worker.assetNames),
      [
        ["image-kernels.wasm", "png-codec.wasm", "font-rasterizer.wasm"],
        [
          "dotnet.boot.js",
          "dotnet.js",
          "dotnet.native.synthetic.js",
          "dotnet.runtime.synthetic.js",
          "runtime.wasm",
        ],
        [
          "texture.mjs",
          "texture.wasm",
          "bc3.wasm",
          "browser-texture.mjs",
          "browser-texture-encoder.mjs",
        ],
        ["hactool-boundary.wasm"],
        ["spirv-cross.wasm"],
        ["capstone-lz4.wasm"],
        ...Array.from({ length: compilerWorkers }, () => ["factory", "wasm"]),
      ],
    );
    for (const path of SOURCE_ONLY_RUNTIME_ASSETS)
      assert.throws(() => runtime.readPublicAsset(path), /Missing verified runtime asset/);
    for (const path of [...RUNTIME_DIRECT_ASSETS, ...RUNTIME_SEALED_WORKERS])
      assert.deepEqual(runtime.readPublicAsset(path), data.get(path));
    await runtime.close();
    assert.ok(state.workers.every((worker) => worker.terminated));
  });

  test("preloaded ASTC stays dormant until requested, then initializes without any fetch", async () => {
    const { prepareSealedRuntime, options } = await setup({ enabled: true });
    const runtime = await prepareSealedRuntime({ ...options, deferOptionalInitialization: true });
    assert.equal(state.helperCalls, 0);
    assert.equal(runtime.initialized.astcProducer, undefined);
    const before = [...state.fetches];
    await runtime.prepareAstcProducer({ signal: state.signal });
    assert.equal(state.helperCalls, 1);
    assert.deepEqual(state.fetches, before);
    await runtime.prepareAstcProducer({ signal: state.signal });
    assert.equal(state.helperCalls, 1);
    assert.equal(runtime.createAstcProducerClient(), state.client);
    await runtime.close();
    assert.ok(state.workers.every((worker) => worker.terminated));
  });

  for (const action of ["cancel", "close"])
    test(`${action} during deferred ASTC startup retires a late client without reads or refetch`, async () => {
      const { prepareSealedRuntime, options, assertSealedRuntime } = await setup({ enabled: true });
      const runtime = await prepareSealedRuntime({ ...options, deferOptionalInitialization: true });
      const before = [...state.fetches];
      state.initializationEntered = Promise.withResolvers();
      state.initializationPause = Promise.withResolvers();
      const controller = new AbortController();
      const initializing = runtime.prepareAstcProducer({ signal: controller.signal });
      await state.initializationEntered.promise;
      if (action === "close") await runtime.close();
      else controller.abort();
      assert.equal(state.initializationSignal.aborted, true);
      // 模拟初始化器不顾取消而延迟返回。
      // Simulate an initializer returning late despite cancellation.
      state.initializationPause.resolve();
      await assert.rejects(initializing, { name: "AbortError" });
      await runtime.close();
      assert.equal(state.producerCloses, 1);
      assert.equal(state.privateCalls, 0);
      assert.ok(state.workers.every((worker) => worker.terminated));
      assert.deepEqual(state.fetches, before);
      assert.throws(() => assertSealedRuntime(runtime), /live verified/);
      assert.throws(() => runtime.createAstcProducerClient(), /unavailable/);
    });
}

// runtime-shell-build-inputs
if (scenario === "runtime-shell-build-inputs") {
  // 只导入打包配置，以可变注册表模拟逐项检查独立下载固定值；禁止真实网络、构建及二进制执行。
  // Import packaging configuration only, varying a mocked registry to check each independent download pin, without real network, builds or binary execution.
  const { default: test, mock: mock } = await import("node:test");
  const { default: assert } = await import("node:assert/strict");
  const { resolveShellProfile: resolveShellProfile } = await import("../../shell-profiles.mjs");

  const currentProfiles = new Map(
    ["v2.0.0-ios26", "v2.0.0-ios18"].map((id) => [id, resolveShellProfile(id)]),
  );
  let drift;
  let networkCalls = 0;
  mock.method(globalThis, "fetch", () => {
    networkCalls++;
    throw new Error("UNEXPECTED_NETWORK");
  });
  mock.module("../../shell-profiles.mjs", {
    namedExports: {
      resolveShellProfile(id) {
        const profile = structuredClone(currentProfiles.get(id));
        if (drift?.id === id) {
          if (drift.field === "archivePath") profile.archivePath = "profile/legacy-shell.zip";
          else if (drift.field === "archiveBytes") profile.evidence.archiveBytes++;
          else if (drift.field === "sourceUrl") profile.evidence.sourceUrl += "?untrusted";
          else profile.evidence.sha256 = "0".repeat(64);
        }
        return profile;
      },
    },
  });

  for (const id of currentProfiles.keys()) {
    for (const field of ["archivePath", "archiveBytes", "sourceUrl", "sha256"]) {
      test(`${id} ${field} drift fails before any download`, async () => {
        drift = { id, field };
        await assert.rejects(
          import(`../../runtime-build/package.mjs?drift=${id}-${field}`),
          /Pinned public shell registry drift/,
        );
        assert.equal(networkCalls, 0);
      });
    }
  }

  test("unchanged registry remains importable without downloading or assembling", async () => {
    drift = undefined;
    const { SHELL_BUILD_INPUTS } = await import("../../runtime-build/package.mjs?drift=none");
    assert.deepEqual(
      SHELL_BUILD_INPUTS.map((input) => input.profileId),
      [...currentProfiles.keys()],
    );
    assert.equal(networkCalls, 0);
  });
}

// runtime-stream-cancellation
if (scenario === "runtime-stream-cancellation") {
  // 用合成公开载荷及原生 Web Streams 验证读取清理和取消，不宣称浏览器内存或真实编译器证明。
  // Verify read cleanup/cancellation with synthetic public payloads and native Web Streams, not browser RAM or actual compiler proof.
  const { default: test, mock: mock } = await import("node:test");
  const { default: assert } = await import("node:assert/strict");
  const { createHash: createHash } = await import("node:crypto");
  const { getEventListeners: getEventListeners } = await import("node:events");
  const { gzipSync: gzipSync } = await import("node:zlib");
  const encode = (text) => new TextEncoder().encode(text);
  const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const shell = encode("synthetic pinned shell");
  const profile = {
    id: "synthetic-profile",
    archivePath: "profile/shell.zip",
    evidence: { archiveBytes: shell.length, sha256: sha(shell) },
  };
  mock.module("../../shell-profiles.mjs", {
    namedExports: { SHELL_PROFILE_IDS: [profile.id], resolveShellProfile: () => profile },
  });
  globalThis.isSecureContext = true;
  globalThis.location = { href: "https://example.test/", origin: "https://example.test" };
  Object.defineProperty(globalThis, "navigator", { value: {}, configurable: true });
  const ResponseClass = Response;
  let serial = 0;
  async function setup(t, { cacheMode, gzip = false, manifestResponse } = {}) {
    const controller = new AbortController();
    const state = {
      timers: new Map(),
      responses: [],
      streams: [],
      cacheRequests: [],
      cancels: 0,
      deletes: 0,
      puts: 0,
      hashes: 0,
      fetches: [],
      workers: [],
      progress: [],
      ready: Promise.withResolvers(),
    };
    let timerId = 0;
    t.mock.method(globalThis, "setTimeout", (callback, delay) => {
      const id = ++timerId;
      state.timers.set(id, { callback, delay });
      return id;
    });
    t.mock.method(globalThis, "clearTimeout", (id) => state.timers.delete(id));
    const digest = crypto.subtle.digest.bind(crypto.subtle);
    t.mock.method(crypto.subtle, "digest", (...args) => {
      state.hashes++;
      return digest(...args);
    });
    const data = new Map([
      [profile.archivePath, shell],
      ["sealed-workers/program-worker.mjs", encode("synthetic public worker")],
      [
        "toolchain/program-rewrite-spike/dist/capstone-lz4.wasm",
        gzip ? new Uint8Array(4 * 1024 * 1024).fill(42) : encode("synthetic public WASM"),
      ],
    ]);
    const assets = [...data].map(([path, bytes]) => ({
      path,
      bytes: bytes.length,
      sha256: sha(bytes),
    }));
    const inventory = {
      version: 1,
      assets,
      totalBytes: assets.reduce((sum, a) => sum + a.bytes, 0),
    };
    if (gzip) {
      const decoded = Buffer.concat([...data.values()]);
      const wire = gzipSync(decoded);
      const file = "payloads/synthetic.gz";
      let offset = 0;
      inventory.transport = {
        version: 1,
        totalBytes: wire.length,
        files: [
          {
            path: file,
            bytes: wire.length,
            sha256: sha(wire),
            encoding: "gzip",
            decodedBytes: decoded.length,
          },
        ],
        entries: assets.map((asset) => {
          const entry = { sha256: asset.sha256, bytes: asset.bytes, file, offset };
          offset += asset.bytes;
          return entry;
        }),
      };
      data.set(file, wire);
    }
    const manifest = encode(JSON.stringify(inventory));
    const cacheData = new Map(
      assets.map((a) => [
        `https://example.test/runtime/__public-runtime-cache__/${a.sha256}-${a.bytes}`,
        data.get(a.path),
      ]),
    );
    const makeCachedResponse = (key) => {
      const response = new ResponseClass(
        new ReadableStream({
          start(control) {
            state.streams.push({ control, bytes: cacheData.get(key) });
          },
          cancel() {
            state.cancels++;
          },
        }),
      );
      state.responses.push(response);
      return response;
    };
    globalThis.caches = {
      async open() {
        if (!cacheMode) throw new Error("Synthetic cache unavailable");
        return {
          async match(key) {
            const pending = Promise.withResolvers();
            state.cacheRequests.push({ key, pending });
            if (state.cacheRequests.length === assets.length) state.ready.resolve();
            return cacheMode === "late-match" ? pending.promise : makeCachedResponse(key);
          },
          async put() {
            state.puts++;
          },
          async delete() {
            state.deletes++;
          },
          async keys() {
            return [];
          },
        };
      },
    };
    t.mock.method(globalThis, "fetch", async (url) => {
      const path = new URL(url).pathname.slice("/runtime/".length);
      state.fetches.push(path);
      if (path === "runtime-manifest.json") return manifestResponse ?? new ResponseClass(manifest);
      assert.ok(data.has(path));
      return new ResponseClass(data.get(path));
    });
    globalThis.Worker = class {
      constructor() {
        state.workers.push(this);
      }
      postMessage(message, transfers) {
        structuredClone(message, { transfer: transfers });
        queueMicrotask(() =>
          this.onmessage({ data: { id: 0, ok: true, result: { initialized: true } } }),
        );
      }
      terminate() {
        this.terminated = true;
      }
    };
    const { prepareSealedRuntime } = await import(
      `../../sealed-runtime/clients.mjs?stream=${serial++}`
    );
    let runtime;
    t.after(async () => {
      await runtime?.close();
    });
    return {
      state,
      controller,
      async start() {
        runtime = await prepareSealedRuntime({
          baseUrl: "https://example.test/runtime/",
          manifestSha256: sha(manifest),
          components: ["program"],
          signal: controller.signal,
          onProgress: (event) => state.progress.push(event),
        });
        return runtime;
      },
      expireCacheReads() {
        const timeouts = [...state.timers].filter(([, timer]) => timer.delay === 3000);
        assert.equal(timeouts.length, 3);
        for (const [id, timer] of timeouts) {
          state.timers.delete(id);
          timer.callback();
        }
      },
      releaseLateMatches() {
        for (const { key, pending } of state.cacheRequests)
          pending.resolve(makeCachedResponse(key));
      },
      releaseBodies() {
        for (const { control, bytes } of state.streams) {
          try {
            control.enqueue(bytes);
            control.close();
          } catch {
            /* 已取消的流保持关闭。 | Cancelled streams remain closed. */
          }
        }
      },
    };
  }
  const flush = async () => {
    await new Promise(setImmediate);
  };

  test("cache read timeout cancels stalled bodies before successful network fallback", async (t) => {
    const { state, start, expireCacheReads, releaseBodies } = await setup(t, { cacheMode: "body" });
    const pending = start();
    await state.ready.promise;
    await flush();
    expireCacheReads();
    const runtime = await pending;
    try {
      assert.equal(state.cancels, 3);
      assert.ok(state.responses.every((response) => !response.body.locked));
      assert.equal(state.fetches.length, 4);
      assert.equal(state.puts, 0);
      assert.equal(state.deletes, 0);
      const hashes = state.hashes;
      await runtime.close();
      releaseBodies();
      await flush();
      assert.equal(state.hashes, hashes, "abandoned cache bodies never start late hashes");
      assert.equal(state.timers.size, 0);
    } finally {
      releaseBodies();
    }
  });

  test("responses arriving after cache timeout are cancelled without hashing or eviction", async (t) => {
    const { state, start, expireCacheReads, releaseLateMatches, releaseBodies } = await setup(t, {
      cacheMode: "late-match",
    });
    const pending = start();
    await state.ready.promise;
    expireCacheReads();
    const runtime = await pending;
    const hashes = state.hashes;
    await runtime.close();
    releaseLateMatches();
    await flush();
    try {
      assert.equal(state.cancels, 3);
      assert.ok(state.responses.every((response) => !response.body.locked));
      assert.equal(state.hashes, hashes);
      assert.equal(state.deletes, 0);
    } finally {
      releaseBodies();
    }
  });

  test("outer cancellation stops cached bodies immediately without waiting for storage timeout", async (t) => {
    const { state, start, controller, releaseBodies } = await setup(t, { cacheMode: "body" });
    const pending = start();
    const checked = assert.rejects(pending, /synthetic cancellation/);
    await state.ready.promise;
    await flush();
    const progress = state.progress.length;
    controller.abort(new Error("synthetic cancellation"));
    try {
      await checked;
      assert.equal(state.cancels, 3);
      assert.ok(state.responses.every((response) => !response.body.locked));
      assert.deepEqual(state.fetches, ["runtime-manifest.json"]);
      assert.equal(state.hashes, 1);
      assert.equal(state.deletes, 0);
      assert.equal(state.progress.length, progress);
      assert.equal(state.timers.size, 0);
      assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    } finally {
      releaseBodies();
    }
  });

  for (const cleanup of ["reject", "pending"])
    test(`manifest cap error survives ${cleanup} stream cancellation without waiting`, async (t) => {
      let cancels = 0;
      const stream = new ReadableStream({
        start(control) {
          control.enqueue(new Uint8Array(2 * 1024 * 1024 + 1));
        },
        cancel() {
          cancels++;
          return cleanup === "reject"
            ? Promise.reject(new Error("cleanup failed"))
            : new Promise(() => {});
        },
      });
      const { state, start, controller } = await setup(t, {
        manifestResponse: new ResponseClass(stream),
      });
      await assert.rejects(start(), /^Error: Public runtime byte cap$/);
      assert.equal(cancels, 1);
      assert.equal(stream.locked, false);
      assert.equal(state.hashes, 0);
      assert.equal(state.timers.size, 0);
      assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    });

  test("synchronous cancellation and release errors cannot replace the primary read error", async (t) => {
    const primary = new Error("synthetic read failure");
    const { start } = await setup(t, {
      manifestResponse: {
        ok: true,
        body: {
          getReader: () => ({
            read: () => Promise.reject(primary),
            cancel() {
              throw new Error("synthetic cancel failure");
            },
            releaseLock() {
              throw new Error("synthetic release failure");
            },
          }),
        },
      },
    });
    await assert.rejects(start(), (error) => error === primary);
  });

  test("gzip cancellation stops decoded reads before logical hashes or worker initialization", async (t) => {
    const { state, start, controller } = await setup(t, { gzip: true });
    let decodedReads = 0,
      cancels = 0,
      body;
    t.mock.method(
      globalThis,
      "Response",
      class extends ResponseClass {
        constructor(stream) {
          super(stream);
          body = this.body;
          const getReader = body.getReader.bind(body);
          body.getReader = () => {
            const reader = getReader();
            const read = reader.read.bind(reader),
              cancel = reader.cancel.bind(reader);
            reader.read = async () => {
              const result = await read();
              if (!result.done) {
                decodedReads++;
                controller.abort(new Error("synthetic gzip cancellation"));
              }
              return result;
            };
            reader.cancel = () => {
              cancels++;
              return cancel();
            };
            return reader;
          };
        }
      },
    );
    await assert.rejects(start(), /synthetic gzip cancellation/);
    assert.equal(decodedReads, 1);
    assert.equal(cancels, 1);
    assert.equal(body.locked, false);
    assert.equal(state.hashes, 2, "only manifest and compressed transport were hashed");
    assert.equal(state.workers.length, 0);
    assert.equal(state.timers.size, 0);
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
  });

  test("normal gzip decoding retains size and integrity validation", async (t) => {
    const { state, start } = await setup(t, { gzip: true });
    const runtime = await start();
    assert.equal(runtime.readPublicAsset(profile.archivePath).length, shell.length);
    assert.equal(state.hashes, 5);
    assert.equal(state.workers.length, 1);
    await runtime.close();
    assert.equal(state.timers.size, 0);
  });

  for (const cleanup of ["resolve", "reject", "pending", "throw"])
    test(`non-OK public response cancels its unread body with ${cleanup} cleanup`, async (t) => {
      let cancels = 0,
        pulls = 0,
        readers = 0;
      const stream = new ReadableStream(
        {
          pull() {
            pulls++;
          },
          cancel() {
            cancels++;
            if (cleanup === "reject") return Promise.reject(new Error("synthetic cleanup failure"));
            if (cleanup === "pending") return new Promise(() => {});
          },
        },
        { highWaterMark: 0 },
      );
      if (cleanup === "throw")
        t.mock.method(stream, "cancel", () => {
          cancels++;
          throw new Error("synthetic synchronous cleanup failure");
        });
      t.mock.method(stream, "getReader", () => {
        readers++;
        throw new Error("Rejected response must not be read");
      });
      const { state, start, controller } = await setup(t, {
        manifestResponse: new ResponseClass(stream, { status: 503 }),
      });
      await assert.rejects(start(), /^Error: Missing public runtime asset$/);
      assert.equal(cancels, 1);
      assert.equal(pulls, 0);
      assert.equal(readers, 0);
      assert.equal(stream.locked, false);
      assert.equal(state.hashes, 0);
      assert.equal(state.workers.length, 0);
      assert.equal(state.timers.size, 0);
      assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    });

  test("missing public response body preserves the existing rejection", async (t) => {
    const { state, start } = await setup(t, {
      manifestResponse: new ResponseClass(null, { status: 503 }),
    });
    await assert.rejects(start(), /^Error: Missing public runtime asset$/);
    assert.equal(state.hashes, 0);
    assert.equal(state.timers.size, 0);
  });
}

// runtime-worker-lifecycle
if (scenario === "runtime-worker-lifecycle") {
  // 使用合成公开清单、工作线程及手动时钟验证密封 RPC 生命周期；不执行真实 WASM 或私有任务。
  // Verify sealed RPC lifetimes with synthetic public inventory, workers and a manual clock, not actual WASM or private jobs.
  const { default: test, mock: mock } = await import("node:test");
  const { default: assert } = await import("node:assert/strict");
  const { createHash: createHash } = await import("node:crypto");
  const { getEventListeners: getEventListeners } = await import("node:events");
  const { createExtractionWorkerClient: createExtractionWorkerClient } =
    await import("../../extraction-worker-client.mjs");
  const { OPTIONAL_RUNTIME_ASSET_GROUPS: OPTIONAL_RUNTIME_ASSET_GROUPS } =
    await import("../../runtime-asset-groups.mjs");

  const encode = (value) => new TextEncoder().encode(value);
  const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const shell = encode("synthetic pinned shell");
  const profile = {
    id: "synthetic-profile",
    archivePath: "profile/shell.zip",
    evidence: { archiveBytes: shell.length, sha256: sha(shell) },
  };
  mock.module("../../shell-profiles.mjs", {
    namedExports: { SHELL_PROFILE_IDS: [profile.id], resolveShellProfile: () => profile },
  });
  globalThis.isSecureContext = true;
  globalThis.location = { href: "https://example.test/", origin: "https://example.test" };
  Object.defineProperty(globalThis, "navigator", { value: {}, configurable: true });
  let serial = 0;
  async function setup(
    t,
    {
      components = ["program"],
      initialize = true,
      sendError,
      onProgress,
      producer = false,
      signing = false,
    } = {},
  ) {
    const controller = new AbortController();
    const state = {
      workers: [],
      revokedUrls: [],
      timers: new Map(),
      initialize,
      sendError,
      firstInitialize: Promise.withResolvers(),
      producerInitialize: Promise.withResolvers(),
    };
    const revokeObjectURL = URL.revokeObjectURL.bind(URL);
    t.mock.method(URL, "revokeObjectURL", (url) => {
      state.revokedUrls.push(url);
      revokeObjectURL(url);
    });
    let timerId = 0;
    t.mock.method(globalThis, "setTimeout", (callback, delay) => {
      const id = ++timerId;
      state.timers.set(id, { callback, delay });
      return id;
    });
    t.mock.method(globalThis, "clearTimeout", (id) => state.timers.delete(id));
    const data = new Map(
      [
        "sealed-runtime/dotnet-worker.mjs",
        "toolchain/offline-runtime-spike/dotnet-dist/main.mjs",
        "sealed-workers/cross-worker.mjs",
        "toolchain/spirv-cross-spike/wwwroot/spirv-cross.wasm",
        "sealed-workers/program-worker.mjs",
        "toolchain/program-rewrite-spike/dist/capstone-lz4.wasm",
        "sealed-runtime/texture-worker.mjs",
        "toolchain/texture-spike/dist/texture.mjs",
        "toolchain/texture-spike/dist/texture.wasm",
        "toolchain/texture-spike/dist/bc3.wasm",
        "toolchain/texture-spike/browser-texture.mjs",
        "toolchain/texture-spike/browser-texture-encoder.mjs",
        "sealed-workers/nsp-worker.mjs",
        "toolchain/nsp-extract-spike/dist/hactool-boundary.wasm",
      ].map((path) => [path, encode(`synthetic ${path}`)]),
    );
    data.set(profile.archivePath, shell);
    if (components.includes("translator")) {
      const directory = "toolchain/offline-runtime-spike/dotnet-dist";
      const value = data.get(`${directory}/main.mjs`);
      data.set(
        `${directory}/manifest.json`,
        encode(
          JSON.stringify({
            version: 1,
            assets: [{ path: "main.mjs", bytes: value.length, sha256: sha(value) }],
          }),
        ),
      );
    }
    if (components.includes("assets")) {
      const directory = "toolchain/asset-runtime-js/dist";
      const records = [
        "worker.mjs",
        "image-kernels.wasm",
        "png-codec.wasm",
        "font-rasterizer.wasm",
      ].map((path) => {
        const value = encode(`synthetic ${path}`);
        data.set(`${directory}/${path}`, value);
        return { path, bytes: value.length, sha256: sha(value) };
      });
      data.set(
        `${directory}/manifest.json`,
        encode(JSON.stringify({ version: 1, assets: records })),
      );
    }
    if (producer) {
      data.set("toolchain/asset-runtime-js/dist/png-codec.wasm", encode("synthetic PNG wasm"));
      for (const path of OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"])
        data.set(path, encode(`synthetic ${path}`));
    }
    if (signing)
      for (const path of OPTIONAL_RUNTIME_ASSET_GROUPS["ipa-signing"])
        data.set(path, encode(`synthetic ${path}`));
    const assets = [...data].map(([path, bytes]) => ({
      path,
      bytes: bytes.length,
      sha256: sha(bytes),
      ...(OPTIONAL_RUNTIME_ASSET_GROUPS["ipa-signing"].includes(path)
        ? { optionalGroup: "ipa-signing" }
        : {}),
      ...(OPTIONAL_RUNTIME_ASSET_GROUPS["legacy-astc"].includes(path)
        ? { optionalGroup: "legacy-astc" }
        : {}),
    }));
    const manifest = encode(
      JSON.stringify({
        version: 1,
        assets,
        totalBytes: assets.reduce((sum, a) => sum + a.bytes, 0),
      }),
    );
    data.set("runtime-manifest.json", manifest);
    t.mock.method(globalThis, "fetch", async (url) => {
      const value = data.get(new URL(url).pathname.slice("/runtime/".length));
      assert.ok(value, "only verified public inventory is fetched");
      return new Response(value);
    });
    globalThis.caches = { open: () => Promise.reject(new Error("Synthetic cache unavailable")) };
    globalThis.Worker = class extends EventTarget {
      requests = [];
      terminations = 0;
      constructor(url) {
        super();
        this.url = url;
        state.workers.push(this);
      }
      dispatchEvent(event) {
        this[`on${event.type}`]?.(event);
        return super.dispatchEvent(event);
      }
      reply(data) {
        this.dispatchEvent(new MessageEvent("message", { data }));
      }
      postMessage(message, transfers = []) {
        if (state.sendError) throw state.sendError;
        const copied = structuredClone(message, { transfer: transfers });
        this.requests.push(copied);
        if (message.type === "initialize" && message.id === undefined) {
          state.producerInitialize.resolve(this);
        } else if (message.type === "initialize") {
          state.firstInitialize.resolve(this);
          if (state.initialize)
            queueMicrotask(() => this.reply({ id: 0, ok: true, result: { initialized: true } }));
        }
      }
      terminate() {
        this.terminations++;
      }
    };
    const module = await import(`../../sealed-runtime/clients.mjs?lifecycle=${serial++}`);
    let runtime;
    t.after(async () => {
      await runtime?.close();
    });
    return {
      ...module,
      state,
      controller,
      async start() {
        runtime = await module.prepareSealedRuntime({
          baseUrl: "https://example.test/runtime/",
          manifestSha256: sha(manifest),
          components,
          onProgress,
          optionalGroups: [
            ...(producer ? ["legacy-astc"] : []),
            ...(signing ? ["ipa-signing"] : []),
          ],
          signal: controller.signal,
        });
        return runtime;
      },
      expire(delay) {
        const matches = [...state.timers].filter(([, timer]) => timer.delay === delay);
        assert.equal(matches.length, 1);
        const [id, timer] = matches[0];
        state.timers.delete(id);
        timer.callback();
      },
    };
  }
  const runProgram = (client) =>
    client.run({ bytes: new Uint8Array([1, 2]), moduleName: "synthetic", releaseId: "synthetic" });
  const runTexture = (client, signal) =>
    client.packLinearBC3(
      { templateHeader: new Uint8Array(40), linearBC3: new Uint8Array(16), width: 1, height: 1 },
      { signal },
    );
  function stopped(state, controller) {
    assert.ok(state.workers.every((worker) => worker.terminations >= 1));
    assert.equal(state.timers.size, 0, "all request timers are cleared");
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
  }

  test("matching initialization replies require a boolean success discriminator", async (t) => {
    const setupState = await setup(t, { initialize: false });
    const pending = setupState.start();
    const worker = await setupState.state.firstInitialize.promise;
    worker.reply({ id: 0, ok: "yes", result: { initialized: true } });
    await assert.rejects(pending, /Runtime worker protocol failed/);
    stopped(setupState.state, setupState.controller);
  });

  test("malformed matching operation replies close every worker, not resolve a result", async (t) => {
    const { start, state, controller, assertSealedRuntime } = await setup(t, {
      components: ["program", "texture"],
    });
    const runtime = await start();
    const pending = runProgram(runtime.createProgramWorker());
    state.workers[1].reply({ id: 1, ok: 1, files: {} });
    await assert.rejects(pending, /Runtime worker protocol failed/);
    assert.throws(() => assertSealedRuntime(runtime), /live verified/);
    stopped(state, controller);
  });

  test("successful replies preserve nested and top-level worker shapes and clear abort listeners", async (t) => {
    const { start, state } = await setup(t, { components: ["program", "texture"] });
    const runtime = await start();
    const controller = new AbortController();
    const texture = runtime.createTextureClient();
    const pending = runTexture(texture, controller.signal);
    assert.equal(getEventListeners(controller.signal, "abort").length, 1);
    state.workers[0].reply({ id: 1, ok: true, result: { sprite: new Uint8Array([7]) } });
    assert.deepEqual(await pending, { sprite: new Uint8Array([7]) });
    assert.equal(getEventListeners(controller.signal, "abort").length, 0);
    assert.equal(state.timers.size, 0);
    controller.abort();
    assert.ok(state.workers.every((worker) => worker.terminations === 0));
    const program = runProgram(runtime.createProgramWorker());
    const reply = { id: 1, ok: true, files: {}, metadata: "synthetic" };
    state.workers[1].reply(reply);
    assert.deepEqual(await program, reply);
  });

  test("busy rejection leaves the original request active and operation errors remain bounded", async (t) => {
    const { start, state } = await setup(t);
    const runtime = await start();
    const client = runtime.createProgramWorker();
    const first = runProgram(client);
    await assert.rejects(runProgram(client), /busy/);
    await assert.rejects(runProgram(runtime.createProgramWorker()), /busy/);
    assert.equal(state.workers[0].requests.length, 2);
    state.workers[0].reply({ id: 1, ok: false, error: "PRIVATE-CANARY" });
    await assert.rejects(first, /^Error: Runtime operation failed$/);
    assert.equal(state.timers.size, 0);
    const next = runProgram(client);
    state.workers[0].reply({ id: 2, ok: true, files: {} });
    assert.deepEqual(await next, { id: 2, ok: true, files: {} });
  });

  test("closing an active lease rejects pending work and permanently revokes the runtime", async (t) => {
    const { start, state, controller, assertSealedRuntime } = await setup(t);
    const runtime = await start();
    const client = runtime.createProgramWorker();
    const pending = runProgram(client);
    client.close();
    await assert.rejects(pending, /Sealed runtime closed/);
    const closing = runtime.close();
    assert.equal(runtime.close(), closing);
    await closing;
    await assert.rejects(runProgram(client), /closed/);
    assert.throws(() => assertSealedRuntime(runtime), /live verified/);
    state.workers[0].reply({ id: 1, ok: true, files: {} });
    stopped(state, controller);
  });

  test("abort during initialization rejects setup and clears its deadline and listener", async (t) => {
    const { start, state, controller } = await setup(t, { initialize: false });
    const pending = start();
    const worker = await state.firstInitialize.promise;
    controller.abort("PRIVATE-CANARY");
    await assert.rejects(pending, /Runtime cancelled/);
    worker.reply({ id: 0, ok: true, result: { initialized: true } });
    stopped(state, controller);
  });

  test("abort during an operation rejects once and clears listeners before late replies", async (t) => {
    const { start, state, controller } = await setup(t, { components: ["texture"] });
    const runtime = await start();
    const operation = new AbortController();
    const pending = runTexture(runtime.createTextureClient(), operation.signal);
    operation.abort("PRIVATE-CANARY");
    await assert.rejects(pending, { name: "AbortError", message: "Runtime cancelled" });
    assert.equal(getEventListeners(operation.signal, "abort").length, 0);
    state.workers[0].reply({ id: 1, ok: true, result: {} });
    stopped(state, controller);
  });

  test("initialization stages and unrelated replies cannot extend the initialization deadline", async (t) => {
    const { start, state, controller, expire } = await setup(t, { initialize: false });
    const pending = start();
    const worker = await state.firstInitialize.promise;
    const [timer] = state.timers.keys();
    worker.reply({ type: "initialization-stage", stage: "starting" });
    worker.reply({ id: 41, ok: true, result: { initialized: true } });
    assert.deepEqual([...state.timers.keys()], [timer]);
    expire(60000);
    await assert.rejects(pending, /Runtime deadline exceeded/);
    stopped(state, controller);
  });

  test("operation deadline closes all workers and rejects the active request", async (t) => {
    const { start, state, controller, expire } = await setup(t, {
      components: ["program", "texture"],
    });
    const runtime = await start();
    const pending = runProgram(runtime.createProgramWorker());
    expire(120000);
    await assert.rejects(pending, /Runtime deadline exceeded/);
    stopped(state, controller);
  });

  test("postMessage failure during initialization clears pending setup without a timeout", async (t) => {
    const { start, state, controller } = await setup(t, {
      sendError: new Error("synthetic clone"),
    });
    await assert.rejects(start(), /synthetic clone/);
    stopped(state, controller);
  });

  test("postMessage failure during an operation rejects and closes every component", async (t) => {
    const { start, state, controller } = await setup(t, {
      components: ["program", "texture"],
    });
    const runtime = await start();
    state.sendError = new Error("synthetic clone");
    await assert.rejects(runProgram(runtime.createProgramWorker()), /synthetic clone/);
    stopped(state, controller);
  });

  test("worker errors and deserialization errors terminate pending requests", async (t) => {
    const { start, state, controller } = await setup(t);
    const runtime = await start();
    const pending = runProgram(runtime.createProgramWorker());
    state.workers[0].dispatchEvent(new Event("messageerror"));
    await assert.rejects(pending, /Sealed runtime closed/);
    stopped(state, controller);
  });

  test("runtime close rejects acquired extraction transport requests and removes its listeners", async (t) => {
    const { start, state, controller } = await setup(t, { components: ["extraction"] });
    const runtime = await start();
    const client = createExtractionWorkerClient({ createWorker: runtime.createExtractionWorker });
    const pending = client.readRomfs({ token: "synthetic", offset: 0n, count: 1 });
    await runtime.close();
    await assert.rejects(pending, /EXTRACTION_WORKER_FAILED/);
    for (const event of ["message", "error", "messageerror"])
      assert.equal(getEventListeners(state.workers[0], event).length, 0);
    stopped(state, controller);
  });

  test("an earlier worker failure cancels a pending producer initializer without waiting for its deadline", async (t) => {
    const { start, state, controller, expire } = await setup(t, { producer: true });
    let settled = false;
    const pending = start().finally(() => {
      settled = true;
    });
    const checked = assert.rejects(pending, { name: "AbortError" });
    const producer = await state.producerInitialize.promise;
    state.workers[0].dispatchEvent(new Event("error"));
    await new Promise(setImmediate);
    // 旧实现需靠超时才结束；让失败测试也能清理，而不挂起测试进程。
    // The old implementation needs its timeout; release that failure path instead of hanging the test runner.
    const settledBeforeDeadline = settled;
    if (!settled) expire(60000);
    await checked;
    assert.equal(settledBeforeDeadline, true);
    for (const event of ["message", "error", "messageerror"])
      assert.equal(getEventListeners(producer, event).length, 0);
    stopped(state, controller);
  });

  test("external cancellation still reaches a pending producer initializer", async (t) => {
    const { start, state, controller } = await setup(t, { producer: true });
    const pending = start();
    const producer = await state.producerInitialize.promise;
    controller.abort("PRIVATE-CANARY");
    await assert.rejects(pending, { name: "AbortError" });
    for (const event of ["message", "error", "messageerror"])
      assert.equal(getEventListeners(producer, event).length, 0);
    stopped(state, controller);
  });

  for (const event of ["error", "messageerror"])
    test(`producer ${event} during later signer initialization cannot return a ready runtime`, async (t) => {
      const { start, state, controller } = await setup(t, { producer: true, signing: true });
      const pending = start();
      const producer = await state.producerInitialize.promise;
      state.initialize = false;
      producer.reply({ type: "initialized", version: 1, astc: true, png: true, sha: true });
      await new Promise(setImmediate);
      const signer = state.workers.at(-1);
      assert.notEqual(signer, producer);
      assert.equal(signer.requests[0].assets[0][0], "zsign.wasm");
      producer.dispatchEvent(new Event(event));
      signer.reply({ id: 0, ok: true, result: { initialized: true } });
      await assert.rejects(pending, /ASTC producer unavailable/);
      stopped(state, controller);
    });

  test("an idle producer failure cannot be acquired as a live private-input capability", async (t) => {
    const { start, state, controller } = await setup(t, { producer: true });
    const pending = start();
    const producer = await state.producerInitialize.promise;
    producer.reply({ type: "initialized", version: 1, astc: true, png: true, sha: true });
    const runtime = await pending;
    producer.dispatchEvent(new Event("error"));
    assert.throws(() => runtime.createAstcProducerClient(), /unavailable/);
    assert.equal(producer.requests.length, 1, "no private operation is sent");
    await runtime.close();
    stopped(state, controller);
  });

  test("producer proof resolution followed by close before parent continuation still disposes the client", async (t) => {
    const { start, state, controller } = await setup(t, { producer: true });
    const pending = start();
    const checked = assert.rejects(pending, { name: "AbortError" });
    const producer = await state.producerInitialize.promise;
    producer.reply({ type: "initialized", version: 1, astc: true, png: true, sha: true });
    // 证明回复先排入辅助函数恢复；此微任务随后关闭会话，并先于父级 await 的赋值执行。
    // The proof queues the helper continuation first; this microtask closes before the parent's await assignment.
    await new Promise((resolve) => {
      queueMicrotask(() => {
        assert.equal(state.timers.size, 0, "proof already settled the helper request");
        assert.equal(getEventListeners(producer, "message").length, 1);
        state.workers[0].dispatchEvent(new Event("error"));
        resolve();
      });
    });
    await checked;
    for (const event of ["message", "error", "messageerror"])
      assert.equal(getEventListeners(producer, event).length, 0);
    stopped(state, controller);
  });

  test("claimed preloaded IPA signer survives conversion runtime cleanup and closes independently", async (t) => {
    const setupState = await setup(t, { signing: true });
    const runtime = await setupState.start();
    assert.equal(runtime.initialized.ipaSigning.initialized, true);
    const signer = runtime.takeIpaSigningFactory();
    assert.throws(() => runtime.takeIpaSigningFactory(), /already acquired/);
    const worker = setupState.state.workers.at(-1);
    assert.ok(setupState.state.revokedUrls.includes(worker.url));
    await runtime.close();
    assert.equal(worker.terminations, 0);
    assert.ok(setupState.state.workers[0].terminations > 0);
    const attempt = await signer.prepare();
    const signing = attempt.sign({
      ipa: new Blob([new Uint8Array(22)]),
      credentials: {
        kind: "p12",
        identity: new ArrayBuffer(2),
        profile: new ArrayBuffer(2),
        password: "",
      },
    });
    const result = new Blob([new Uint8Array(23)]);
    worker.reply({ id: 1, ok: true, result: { blob: result } });
    assert.equal((await signing).blob, result);
    assert.equal(worker.terminations, 1);
    const retry = await signer.prepare();
    const retryWorker = setupState.state.workers.at(-1);
    assert.notEqual(retryWorker, worker);
    assert.equal(retryWorker.terminations, 0);
    assert.ok(setupState.state.revokedUrls.includes(retryWorker.url));
    retry.close();
    assert.equal(retryWorker.terminations, 1);
    signer.close();
    assert.equal(worker.terminations, 1);
  });

  test("unclaimed preloaded IPA signer closes with its runtime", async (t) => {
    const setupState = await setup(t, { signing: true });
    const runtime = await setupState.start();
    const worker = setupState.state.workers.at(-1);
    await runtime.close();
    assert.ok(worker.terminations > 0);
    assert.throws(() => runtime.takeIpaSigningFactory(), /unavailable/);
  });

  test("default runtime has no signing worker or signable lease", async (t) => {
    const setupState = await setup(t);
    const runtime = await setupState.start();
    assert.equal(runtime.initialized.ipaSigning, undefined);
    assert.equal(setupState.state.workers.length, 1);
    assert.throws(() => runtime.takeIpaSigningFactory(), /unavailable/);
  });

  test("asset recycling reuses its entry URL and leases ignore retired worker events", async (t) => {
    const { start, state, assertSealedRuntime } = await setup(t, { components: ["assets"] });
    const runtime = await start();
    const client = runtime.createAssetWorker();
    await runtime.recycleAssetWorker();
    await runtime.recycleAssetWorker();
    const current = state.workers.at(-1);
    assert.ok(state.workers.every((worker) => worker.url === current.url));
    const pending = client.run("overlay-recipe", new Uint8Array([1]));
    for (const retired of state.workers.slice(0, -1)) {
      assert.equal(retired.terminations, 1);
      retired.reply({ id: 1, ok: false });
      retired.dispatchEvent(new Event("error"));
      retired.dispatchEvent(new Event("messageerror"));
    }
    assert.equal(assertSealedRuntime(runtime), runtime);
    assert.equal(current.terminations, 0);
    current.reply({ id: 1, ok: true, result: { files: {} } });
    assert.deepEqual(await pending, { files: {} });
    await runtime.close();
    assert.equal(state.revokedUrls.filter((url) => url === current.url).length, 1);
  });

  test("cancelling an asset restart rejects and clears all workers and request listeners", async (t) => {
    const { start, state, controller } = await setup(t, { components: ["assets"] });
    const runtime = await start();
    state.initialize = false;
    const restart = new AbortController();
    const pending = runtime.recycleAssetWorker({ signal: restart.signal });
    restart.abort();
    await assert.rejects(pending, { name: "AbortError" });
    assert.equal(getEventListeners(restart.signal, "abort").length, 0);
    stopped(state, controller);
  });

  for (const action of ["close", "abort"])
    test(`asset restart proof followed by ${action} rejects before continuation`, async (t) => {
      const { start, state, controller, assertSealedRuntime } = await setup(t, {
        components: ["assets"],
      });
      const runtime = await start();
      state.initialize = false;
      const restart = new AbortController();
      const pending = runtime.recycleAssetWorker({ signal: restart.signal });
      state.workers.at(-1).reply({ id: 0, ok: true, result: { initialized: true } });
      if (action === "close") runtime.close();
      else restart.abort();
      await assert.rejects(pending, { name: "AbortError" });
      assert.throws(() => assertSealedRuntime(runtime), /live verified/);
      stopped(state, controller);
    });

  test("asset restart rejects concurrent restarts and lease operations without disrupting initialization", async (t) => {
    const { start, state } = await setup(t, { components: ["assets"] });
    const runtime = await start();
    const client = runtime.createAssetWorker();
    state.initialize = false;
    const pending = runtime.recycleAssetWorker();
    await assert.rejects(runtime.recycleAssetWorker(), /busy/);
    await assert.rejects(client.run("overlay-recipe", new Uint8Array([1])), /busy/);
    const current = state.workers.at(-1);
    assert.equal(current.requests.length, 1);
    current.reply({ id: 0, ok: true, result: { initialized: true } });
    await pending;
    const operation = client.run("overlay-recipe", new Uint8Array([1]));
    await assert.rejects(runtime.recycleAssetWorker(), /busy/);
    current.reply({ id: 1, ok: true, result: {} });
    await operation;
  });

  // 原生 structuredClone 模拟真实转移，断言不触碰借用输入及失败快照清理。
  // Native structuredClone models real transfer, checking borrowed inputs and failed snapshot cleanup.
  test("shader facade transfers owned inputs immediately and snapshots metadata", async (t) => {
    const { start, state } = await setup(t, { components: ["translator", "spirvCross"] });
    const runtime = await start();
    const worker = runtime.createTranslatorWorker();
    const code = new Uint8Array([1, 2, 3]);
    const job = { stage: "vertex" };
    const reply = new Promise((resolve) => {
      worker.onmessage = resolve;
    });
    worker.postMessage({ requestId: 7, code, job }, [code.buffer]);
    assert.equal(code.byteLength, 0);
    job.stage = "fragment";
    await Promise.resolve();
    const request = state.workers[0].requests.at(-1);
    assert.deepEqual([...new Uint8Array(request.payload)], [1, 2, 3]);
    assert.deepEqual(request.job, { stage: "vertex" });
    state.workers[0].reply({ id: request.id, ok: true, result: { valid: true } });
    assert.equal((await reply).data.requestId, 7);

    const cross = runtime.createShaderWorkerFactory()(runtime.workerUrls.spirvCross);
    const spirv = new Uint8Array([4, 5]).buffer;
    const context = { stage: "vertex" };
    const crossReply = new Promise((resolve) => {
      cross.onmessage = resolve;
    });
    cross.postMessage({ id: 8, type: "convert", spirv, replayContext: context }, [spirv]);
    assert.equal(spirv.byteLength, 0);
    context.stage = "fragment";
    await Promise.resolve();
    const crossRequest = state.workers[1].requests.at(-1);
    assert.deepEqual([...new Uint8Array(crossRequest.spirv)], [4, 5]);
    assert.equal(crossRequest.replayContext.stage, "vertex");
    state.workers[1].reply({ id: crossRequest.id, ok: true, result: {} });
    assert.equal((await crossReply).data.id, 8);
  });

  test("shader facade preserves subarray bounds and public borrowed inputs", async (t) => {
    const { start, state } = await setup(t, { components: ["translator"] });
    const runtime = await start();
    const worker = runtime.createTranslatorWorker();
    const backing = new Uint8Array([9, 1, 2, 9]);
    const reply = new Promise((resolve) => {
      worker.onmessage = resolve;
    });
    worker.postMessage({ requestId: 1, code: backing.subarray(1, 3), job: {} }, [backing.buffer]);
    assert.equal(backing.byteLength, 0);
    await Promise.resolve();
    const request = state.workers[0].requests.at(-1);
    assert.deepEqual([...new Uint8Array(request.payload)], [1, 2]);
    state.workers[0].reply({ id: request.id, ok: true, result: {} });
    await reply;
    const borrowed = new Uint8Array([6, 7]);
    const pending = runtime.createTranslator().translate({ job: {}, code: borrowed });
    assert.deepEqual([...borrowed], [6, 7]);
    const next = state.workers[0].requests.at(-1);
    state.workers[0].reply({ id: next.id, ok: true, result: {} });
    await pending;
    assert.deepEqual([...borrowed], [6, 7]);
  });

  test("shader facade wipes queued cancellation and busy rejection snapshots", async (t) => {
    const { start, state } = await setup(t, { components: ["translator"] });
    const runtime = await start();
    const clone = globalThis.structuredClone;
    const snapshots = [];
    t.mock.method(globalThis, "structuredClone", (value, options) => {
      const result = clone(value, options);
      if (result?.requestId) snapshots.push(result);
      return result;
    });
    const cancelled = runtime.createTranslatorWorker();
    const code = new Uint8Array([1, 2]);
    cancelled.postMessage({ requestId: 1, code, job: {} }, [code.buffer]);
    cancelled.terminate();
    await Promise.resolve();
    assert.equal(code.byteLength, 0);
    assert.deepEqual([...snapshots[0].code], [0, 0]);
    assert.equal(state.workers[0].requests.length, 1);

    const worker = runtime.createTranslatorWorker();
    worker.postMessage({ requestId: 2, code: new Uint8Array([3]), job: {} });
    await Promise.resolve();
    const first = state.workers[0].requests.at(-1);
    const borrowed = new Uint8Array([4, 5]);
    const failure = new Promise((resolve) => {
      worker.onmessage = resolve;
    });
    worker.postMessage({ requestId: 3, code: borrowed, job: {} });
    assert.equal((await failure).data.ok, false);
    assert.deepEqual([...borrowed], [4, 5]);
    assert.deepEqual([...snapshots[2].code], [0, 0]);
    assert.equal(state.workers[0].requests.length, 2);
    state.workers[0].reply({ id: first.id, ok: true, result: {} });
  });

  test("shader facade forwards the owned buffer itself and wipes failed handoffs", async (t) => {
    const { start, state } = await setup(t, { components: ["translator"] });
    const runtime = await start();
    const clone = globalThis.structuredClone;
    let snapshot;
    t.mock.method(globalThis, "structuredClone", (value, options) => {
      const result = clone(value, options);
      if (result?.requestId === 9) snapshot = result;
      return result;
    });
    let handedOff;
    t.mock.method(state.workers[0], "postMessage", (message) => {
      handedOff = message.payload;
      throw new Error("Synthetic send failure");
    });
    const worker = runtime.createTranslatorWorker();
    const code = new Uint8Array([7, 8]);
    const failure = new Promise((resolve) => {
      worker.onmessage = resolve;
    });
    worker.postMessage({ requestId: 9, job: {}, code }, [code.buffer]);
    assert.equal(code.byteLength, 0);
    assert.equal((await failure).data.ok, false);
    assert.equal(
      handedOff,
      snapshot.code.buffer,
      "full-span owned buffer is forwarded without another copy",
    );
    assert.deepEqual([...new Uint8Array(handedOff)], [0, 0]);
  });

  // 在任何结构化复制之前拒绝超限输入，且借用子视图不会放大为底层分配。
  // Reject oversized inputs before any structured clone; borrowed subviews never amplify to their backing allocation.
  test("shader facade caps inputs and metadata before cloning", async (t) => {
    const { start, state } = await setup(t, { components: ["translator", "spirvCross"] });
    const runtime = await start();
    let clones = 0;
    const clone = globalThis.structuredClone;
    t.mock.method(globalThis, "structuredClone", (...args) => {
      clones++;
      return clone(...args);
    });
    const translator = runtime.createTranslatorWorker();
    const cross = runtime.createShaderWorkerFactory()(runtime.workerUrls.spirvCross);
    for (const [worker, data] of [
      [translator, { requestId: 1, code: new Uint8Array(1024 * 1024 + 1), job: {} }],
      [
        cross,
        { id: 2, type: "convert", spirv: new ArrayBuffer(16 * 1024 * 1024 + 1), replayContext: {} },
      ],
      [translator, { requestId: 3, code: new Uint8Array([1]), job: { large: "x".repeat(65537) } }],
      [
        cross,
        {
          id: 4,
          type: "convert",
          spirv: new ArrayBuffer(4),
          replayContext: { large: "x".repeat(65537) },
        },
      ],
    ]) {
      const reply = new Promise((resolve) => {
        worker.onmessage = resolve;
      });
      assert.doesNotThrow(() => worker.postMessage(data));
      assert.equal((await reply).data.ok, false);
    }
    assert.equal(clones, 0);
    assert.ok(state.workers.every((worker) => worker.requests.length === 1));
  });

  test("borrowed shader subviews clone only their range and ignore unused fields", async (t) => {
    const { start, state } = await setup(t, { components: ["translator"] });
    const runtime = await start();
    const clone = globalThis.structuredClone;
    let sourceBytes;
    t.mock.method(globalThis, "structuredClone", (value, options) => {
      if (value?.requestId === 10) {
        sourceBytes = value.code.buffer.byteLength;
        assert.equal(Object.hasOwn(value, "ignored"), false);
      }
      return clone(value, options);
    });
    const backing = new Uint8Array(2 * 1024 * 1024);
    backing.set([3, 4], 99);
    const extraTransfer = new ArrayBuffer(1);
    const worker = runtime.createTranslatorWorker();
    const reply = new Promise((resolve) => {
      worker.onmessage = resolve;
    });
    worker.postMessage(
      { requestId: 10, code: backing.subarray(99, 101), job: {}, ignored: backing },
      [extraTransfer],
    );
    assert.equal(sourceBytes, 2);
    assert.equal(extraTransfer.byteLength, 0);
    assert.equal(backing.byteLength, 2 * 1024 * 1024);
    backing[99] = 8;
    await Promise.resolve();
    const request = state.workers[0].requests.at(-1);
    assert.deepEqual([...new Uint8Array(request.payload)], [3, 4]);
    state.workers[0].reply({ id: request.id, ok: true, result: {} });
    await reply;
    assert.deepEqual([...backing.subarray(99, 101)], [8, 4]);
  });

  test("invalid shader transfer lists wipe private subview copies without touching borrowed bytes", async (t) => {
    const { start } = await setup(t, { components: ["translator"] });
    const runtime = await start();
    const clone = globalThis.structuredClone;
    let privateCode;
    t.mock.method(globalThis, "structuredClone", (value, options) => {
      if (value?.requestId === 11) privateCode = value.code;
      return clone(value, options);
    });
    const backing = new Uint8Array([9, 3, 4, 9]);
    const extra = new ArrayBuffer(1);
    const worker = runtime.createTranslatorWorker();
    assert.throws(
      () =>
        worker.postMessage({ requestId: 11, code: backing.subarray(1, 3), job: {} }, [
          extra,
          extra,
        ]),
      { name: "DataCloneError" },
    );
    assert.deepEqual([...privateCode], [0, 0]);
    assert.deepEqual([...backing], [9, 3, 4, 9]);
  });

  test("shader facade rejects object and oversized correlation IDs without cloning or echoing them", async (t) => {
    const { start } = await setup(t, { components: ["translator", "spirvCross"] });
    const runtime = await start();
    const clone = globalThis.structuredClone;
    let clones = 0;
    t.mock.method(globalThis, "structuredClone", (...args) => {
      clones++;
      return clone(...args);
    });
    const translator = runtime.createTranslatorWorker();
    const cross = runtime.createShaderWorkerFactory()(runtime.workerUrls.spirvCross);
    for (const id of [{ nested: "id" }, [], new ArrayBuffer(1024 * 1024), "x".repeat(257)]) {
      for (const [worker, data] of [
        [translator, { requestId: id, code: new Uint8Array([1]), job: {} }],
        [cross, { id, type: "convert", spirv: new ArrayBuffer(4), replayContext: {} }],
      ]) {
        const reply = new Promise((resolve) => {
          worker.onmessage = resolve;
        });
        worker.postMessage(data);
        assert.deepEqual((await reply).data, {
          ok: false,
          error: "Sealed operation failed",
          shaderDiagnostic: {
            substep: worker === translator ? "translation" : "spirv-cross",
            errorType: "Error",
            message: "Invalid shader correlation ID",
          },
        });
      }
    }
    for (const id of [undefined, null, true, 1, "x".repeat(256)]) {
      const reply = new Promise((resolve) => {
        translator.onmessage = resolve;
      });
      translator.postMessage({ requestId: id, code: null, job: {} });
      assert.deepEqual((await reply).data, {
        requestId: id,
        ok: false,
        error: "Sealed operation failed",
        shaderDiagnostic: {
          substep: "translation",
          errorType: "Error",
          message: "Runtime input cap",
        },
      });
    }
    assert.equal(clones, 0);
  });

  test("shader diagnostics survive the sealed channel and facade without unrelated payloads", async (t) => {
    const { start, state } = await setup(t, {
      components: ["translator", "spirvCross", "program"],
    });
    const runtime = await start();
    for (const [worker, native, data, substep] of [
      [
        runtime.createTranslatorWorker(),
        state.workers[0],
        { requestId: 19, code: new Uint8Array([1]), job: {} },
        "translation",
      ],
      [
        runtime.createCrossWorker(),
        state.workers[1],
        { id: 20, type: "convert", spirv: new ArrayBuffer(4), replayContext: {} },
        "spirv-cross",
      ],
    ]) {
      const pending = new Promise((resolve) => {
        worker.onmessage = resolve;
      });
      worker.postMessage(data);
      await Promise.resolve();
      const request = native.requests.at(-1);
      native.reply({
        id: request.id,
        ok: false,
        error: "PRIVATE-CANARY",
        shaderDiagnostic: {
          substep,
          errorType: "NotImplementedException",
          message: "Translator does not implement this operation",
          diagnostics: ["error: unsupported synthetic intrinsic", "PRIVATE-CANARY"],
          stack: "PRIVATE-CANARY",
          source: "PRIVATE-CANARY",
          job: { secret: "PRIVATE-CANARY" },
        },
      });
      const reply = (await pending).data;
      assert.equal(reply.ok, false);
      assert.equal(reply.error, "Sealed operation failed");
      assert.deepEqual(reply.shaderDiagnostic, {
        substep,
        errorType: "NotImplementedException",
        message: "Translator does not implement this operation",
        diagnostics: ["error: unsupported synthetic intrinsic"],
      });
      assert.doesNotMatch(JSON.stringify(reply), /PRIVATE-CANARY/);
    }
    const pending = runProgram(runtime.createProgramWorker());
    state.workers[2].reply({
      id: 1,
      ok: false,
      error: "PRIVATE-CANARY",
      shaderDiagnostic: { substep: "translation", message: "Missing real translator export" },
    });
    await assert.rejects(pending, (error) => {
      assert.equal(error.message, "Runtime operation failed");
      assert.equal(Object.hasOwn(error, "shaderDiagnostic"), false);
      return true;
    });
  });

  test("unknown shader errors remain generic, while direct translator preserves structured causes", async (t) => {
    const { start, state } = await setup(t, { components: ["translator"] });
    const runtime = await start();
    const direct = runtime.createTranslator();
    const pending = direct.translate({ job: {}, code: new Uint8Array([1]) });
    state.workers[0].reply({
      id: 1,
      ok: false,
      error: "PRIVATE-CANARY",
      shaderDiagnostic: { substep: "translation", message: "Missing embedded shader helper" },
    });
    await assert.rejects(pending, (error) => {
      assert.equal(error.shaderDiagnostic.message, "Missing embedded shader helper");
      assert.equal(error.shaderDiagnostic.substep, "translation");
      assert.doesNotMatch(error.message, /PRIVATE-CANARY/);
      return true;
    });
    const worker = runtime.createTranslatorWorker();
    const reply = new Promise((resolve) => {
      worker.onmessage = resolve;
    });
    worker.postMessage({ requestId: 21, code: new Uint8Array([1]), job: {} });
    await Promise.resolve();
    state.workers[0].reply({ id: 2, ok: false, error: "PRIVATE-CANARY" });
    const failure = (await reply).data;
    assert.deepEqual(failure.shaderDiagnostic, {
      substep: "translation",
      errorType: "Error",
      message: "Runtime operation failed",
    });
    assert.doesNotMatch(JSON.stringify(failure), /PRIVATE-CANARY/);
  });

  test("translator startup wrapper preserves structured export failures", async (t) => {
    const setupState = await setup(t, { components: ["translator"], initialize: false });
    const pending = setupState.start();
    const worker = await setupState.state.firstInitialize.promise;
    worker.reply({
      id: 0,
      ok: false,
      error: "Offline .NET initialization failed",
      shaderDiagnostic: {
        substep: "pipeline-initialization",
        errorType: "Error",
        message: "Missing real translator export",
      },
    });
    await assert.rejects(pending, (error) => {
      assert.deepEqual(error.shaderDiagnostic, {
        substep: "pipeline-initialization",
        errorType: "Error",
        message: "Missing real translator export",
      });
      return true;
    });
    stopped(setupState.state, setupState.controller);
  });

  // 观察者异常与工作线程初始化结果隔离，进度状态仍继续更新。
  // Observer failures are isolated from worker initialization while progress state keeps advancing.
  for (const mode of ["throw", "reject", "thenable"]) {
    test(`startup progress observer ${mode} does not interrupt initialization`, async (t) => {
      const progress = [];
      let failObserver = false;
      const { start, state } = await setup(t, {
        initialize: false,
        onProgress(value) {
          progress.push(value);
          if (!failObserver) return;
          if (mode === "throw") throw new Error("Synthetic observer throw");
          if (mode === "reject") return Promise.reject(new Error("Synthetic observer rejection"));
          return {
            get then() {
              throw new Error("Synthetic thenable getter");
            },
          };
        },
      });
      const pending = start();
      const worker = await state.firstInitialize.promise;
      failObserver = true;
      assert.doesNotThrow(() =>
        worker.reply({ type: "initialization-stage", stage: "config-module" }),
      );
      assert.match(progress.at(-1).label, /reading configuration/);
      assert.doesNotThrow(() =>
        worker.reply({ type: "initialization-stage", stage: "probe-after" }),
      );
      assert.match(progress.at(-1).label, /finishing startup/);
      worker.reply({ id: 0, ok: true, result: { initialized: true } });
      const runtime = await pending;
      assert.equal(runtime.initialized.program.initialized, true);
      assert.equal(worker.terminations, 0);
      assert.equal(state.timers.size, 0);
      assert.ok(progress.length >= 3);
      assert.notEqual(progress.at(-1), progress.at(-2));
    });
  }

  test("program retirement releases only the completed component and is idempotent", async (t) => {
    const { start, state, assertSealedRuntime } = await setup(t, {
      components: ["program", "texture"],
    });
    const runtime = await start();
    const client = runtime.createProgramWorker();
    const pending = runProgram(client);
    assert.equal(runtime.retireProgramWorker(), false);
    state.workers[1].reply({ id: 1, ok: true, files: {} });
    await pending;
    assert.equal(runtime.retireProgramWorker(), false);
    client.close();
    client.close();
    assert.ok(state.workers.every((worker) => worker.terminations === 0));
    assert.equal(runtime.retireProgramWorker(), true);
    assert.equal(runtime.retireProgramWorker(), true);
    assert.equal(state.workers[1].terminations, 1);
    assert.equal(state.workers[0].terminations, 0);
    assert.doesNotThrow(() => assertSealedRuntime(runtime));
    assert.throws(() => runtime.createProgramWorker(), /Program worker retired/);
    const texture = runTexture(runtime.createTextureClient());
    state.workers[0].reply({ id: 1, ok: true, result: { sprite: new Uint8Array([7]) } });
    assert.deepEqual(await texture, { sprite: new Uint8Array([7]) });
    await runtime.close();
    assert.equal(state.workers[1].terminations, 1);
    assert.equal(state.workers[0].terminations, 1);
  });

  test("program retirement preserves idle siblings and their future requests", async (t) => {
    const { start, state } = await setup(t);
    const runtime = await start();
    const first = runtime.createProgramWorker();
    const sibling = runtime.createProgramWorker();
    first.close();
    assert.equal(runtime.retireProgramWorker(), false);
    assert.equal(state.workers[0].terminations, 0);
    const pending = runProgram(sibling);
    state.workers[0].reply({ id: 1, ok: true, files: {} });
    await pending;
    sibling.close();
    assert.equal(runtime.retireProgramWorker(), true);
  });

  test("program retirement after cancelled active work is a harmless no-op", async (t) => {
    const { start, state } = await setup(t);
    const runtime = await start();
    const client = runtime.createProgramWorker();
    const pending = runProgram(client);
    client.close();
    await assert.rejects(pending, /Sealed runtime closed/);
    assert.equal(runtime.retireProgramWorker(), false);
    assert.equal(state.workers[0].terminations, 1);
  });

  test("completed asset and shader groups retire without touching extraction", async (t) => {
    const { start, state, assertSealedRuntime } = await setup(t, {
      components: ["assets", "texture", "translator", "spirvCross", "extraction"],
    });
    const runtime = await start();
    const asset = runtime.createAssetWorker();
    const texture = runtime.createTextureEncoder();
    assert.equal(runtime.retireAssetWorkers(), false);
    asset.close();
    assert.equal(runtime.retireAssetWorkers(), false);
    assert.equal(state.workers.filter((worker) => worker.terminations).length, 0);
    texture.close();
    assert.equal(runtime.retireAssetWorkers(), true);
    assert.equal(runtime.retireAssetWorkers(), true);
    assert.equal(state.workers.filter((worker) => worker.terminations).length, 2);
    assert.throws(() => runtime.createAssetWorker(), /retired/);
    assert.throws(() => runtime.createTextureClient(), /retired/);
    await assert.rejects(runtime.recycleAssetWorker(), /unavailable/);
    assert.equal(state.workers.length, 5);
    const translator = runtime.createTranslator();
    const cross = runtime.createCrossWorker();
    const pending = translator.translate({ job: {}, code: new Uint8Array([1]) });
    assert.equal(runtime.retireShaderWorkers(), false);
    const worker = state.workers.find((item) => item.requests.at(-1).type === "run");
    worker.reply({ id: 1, ok: true, result: { translated: true } });
    assert.deepEqual(await pending, { translated: true });
    translator.close();
    assert.equal(runtime.retireShaderWorkers(), false);
    cross.terminate();
    cross.terminate();
    assert.equal(runtime.retireShaderWorkers(), true);
    assert.equal(runtime.retireShaderWorkers(), true);
    assert.equal(state.workers.filter((item) => item.terminations).length, 4);
    assert.throws(() => runtime.createTranslator(), /retired/);
    assert.throws(() => runtime.createCrossWorker(), /retired/);
    assert.equal(runtime.createExtractionWorker().terminations, 0);
    assert.doesNotThrow(() => assertSealedRuntime(runtime));
  });

  test("asset retirement skips an in-progress restart and can retire its replacement", async (t) => {
    const { start, state } = await setup(t, { components: ["assets", "texture"] });
    const runtime = await start();
    state.initialize = false;
    const pending = runtime.recycleAssetWorker();
    const replacement = state.workers.at(-1);
    assert.equal(runtime.retireAssetWorkers(), false);
    assert.equal(replacement.terminations, 0);
    replacement.reply({ id: 0, ok: true, result: { initialized: true } });
    await pending;
    assert.equal(runtime.retireAssetWorkers(), true);
    assert.equal(replacement.terminations, 1);
  });

  test("stage retirement after active cancellation does not stop workers twice", async (t) => {
    const { start, state } = await setup(t, {
      components: ["assets", "texture", "translator", "spirvCross"],
    });
    const runtime = await start();
    const asset = runtime.createAssetWorker();
    const pending = asset.run("overlay-recipe", new Uint8Array([1]));
    assert.equal(runtime.retireAssetWorkers(), false);
    asset.close();
    await assert.rejects(pending, /closed/);
    assert.equal(runtime.retireAssetWorkers(), false);
    assert.equal(runtime.retireShaderWorkers(), false);
    assert.ok(state.workers.every((worker) => worker.terminations === 1));
  });
}
