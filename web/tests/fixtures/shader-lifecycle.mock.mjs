// Each selected scenario runs in its own child process; module mocks and globals never cross scenarios.
import process from "node:process";
const scenario = process.env.D2R_TEST_SCENARIO;
if (
  !["shader-output-cache", "shader-retirement-assets", "production-shader-concurrency"].includes(
    scenario,
  )
)
  throw new Error("Unknown isolated mock scenario: " + scenario);

// shader-output-cache
if (scenario === "shader-output-cache") {
  // 仅供测试的合成生产循环依赖，不是真实源码/编译器证明。
  // Test-only synthetic production-loop dependencies, not real source/compiler proof.
  const { default: assert } = await import("node:assert/strict");
  const { mock: mock } = await import("node:test");
  const keys = Array.from({ length: 6927 }, (_, i) => i.toString(16).padStart(16, "0"));
  const cache = new Map();
  let full = false,
    proofFails = false,
    forceHit = false,
    badHit = false,
    controller,
    source = "synthetic",
    compiled = 0,
    translated = 0,
    proofs = 0,
    writes = 0,
    accepted = [],
    abortWrite = false;
  const identity = {
    id: "synthetic-compiler",
    assetSha256: "1".repeat(64),
    factorySha256: "2".repeat(64),
    headerSha256: "3".repeat(64),
    apiVersion: 2,
    headerBundleVersion: 2,
    sourceLanguage: "floor-metal-3.2",
    mathProfile: "fast-f32",
  };
  const options = { platform: "ios", minimumOS: [18, 0], languageVersion: [3, 2], fastMath: true };
  mock.module("../../sealed-runtime/clients.mjs", { namedExports: { assertSealedRuntime() {} } });
  mock.module("../../shader-catalog-adapter.mjs", { namedExports: { assertShaderCatalog() {} } });
  mock.module("../../extraction-job-adapter.mjs", {
    namedExports: {
      readAuthenticatedAssets() {
        return {
          logicalAssets: {
            async read() {
              return new Uint8Array([1]);
            },
          },
        };
      },
      readAuthenticatedProgramModules() {
        return {};
      },
    },
  });
  mock.module("../../logical-asset-suffixes.mjs", {
    namedExports: {
      resolveLogicalAssetSuffixes() {
        return ["data:data/hd/shaders/synthetic"];
      },
    },
  });
  mock.module("../../browser-task-yield.mjs", {
    namedExports: {
      async yieldBrowserTask({ signal }) {
        signal?.throwIfAborted();
      },
    },
  });
  mock.module("../../shader-pipeline.mjs", {
    namedExports: {
      createShaderPipeline() {
        return {
          async translate() {
            translated++;
            return {
              msl: source,
              replay: { entry: "main0", msl_entry_kind: "kernel" },
              resources: { notes: [] },
            };
          },
          async verifyCompleteSourceSet() {
            proofs++;
            if (proofFails) throw new Error("synthetic proof failed");
            return { sourceDigest: "be59c864f087b377" };
          },
          dispose() {},
        };
      },
    },
  });
  mock.module("../../compiled-shader-boundary.mjs", {
    namedExports: {
      async acceptCompiledShaderOutput({ binary, compiler, signal }) {
        signal?.throwIfAborted();
        assert.equal(binary[0], 7, "synthetic output rejected");
        const bytes = binary.slice();
        accepted.push(bytes);
        return { bytes, receipt: { compiler }, receiptSha256: "synthetic" };
      },
    },
  });
  const { createProductionShaderAdapter } = await import("../../production-shaders.mjs");
  const runtime = {
    workerUrls: {},
    createShaderWorkerFactory() {},
    createAirCompilerClient() {
      return {
        compiler: { ...identity },
        options: structuredClone(options),
        limits: { outputBytes: 16 * 1024 * 1024 },
        async compile() {
          compiled++;
          return { bytes: new Uint8Array([7]), compiler: { ...identity }, options };
        },
        dispose() {},
      };
    },
  };
  const catalog = {
    keys,
    bfWant: [],
    describe(key) {
      return { name: "synthetic", index: parseInt(key, 16), embedded: false };
    },
    createOwnedPackSource() {
      return {
        async prepare(key) {
          if (!full && parseInt(key, 16) >= 2) throw new Error("synthetic stop");
          return { job: { stage: "cs" }, code: new Uint8Array([1]) };
        },
        dispose() {},
      };
    },
  };
  const outputCache = {
    async read({ key }) {
      return badHit
        ? new Uint8Array([0])
        : forceHit
          ? new Uint8Array([7])
          : (cache.get(key)?.slice() ?? null);
    },
    async write({ key, bytes }) {
      writes++;
      cache.set(key, bytes.slice());
      if (abortWrite) controller.abort();
      return true;
    },
  };
  async function run(context = { runtimeManifestSha256: "4".repeat(64) }) {
    controller = new AbortController();
    const adapter = createProductionShaderAdapter({
      runtime,
      catalog,
      outputCache,
      cacheContext: context,
      trustedShell: {
        trustedRecipe: {
          shaderKeys: keys,
          shaderDigest: "be59c864f087b377",
          chain: "123456789abc",
        },
      },
    });
    try {
      if (abortWrite)
        await assert.rejects(adapter.generateShaders({ signal: controller.signal }), {
          name: "AbortError",
        });
      else if (!full)
        await assert.rejects(
          adapter.generateShaders({ signal: controller.signal }),
          /synthetic stop/,
        );
      else if (proofFails)
        await assert.rejects(
          adapter.generateShaders({ signal: controller.signal }),
          /synthetic proof failed/,
        );
      else assert.ok((await adapter.generateShaders({ signal: controller.signal })).length > 6927);
    } finally {
      adapter.dispose();
    }
  }
  await run();
  assert.equal(compiled, 2);
  assert.equal(writes, 2);
  await run();
  assert.equal(compiled, 2);
  assert.equal(translated, 4);
  // 键绑定编译器、头文件、工厂、ABI、配置、选项及规范化源码/运行时。
  // Key binds compiler/header/factory/ABI/profile/options and normalized source/runtime.
  for (const field of ["assetSha256", "factorySha256", "headerSha256"]) {
    identity[field] = "9".repeat(64);
    const before = compiled;
    await run();
    assert.equal(compiled, before + 2);
  }
  for (const field of ["apiVersion", "headerBundleVersion"]) {
    identity[field]++;
    const before = compiled;
    await run();
    assert.equal(compiled, before + 2);
  }
  identity.mathProfile = "synthetic-other";
  let before = compiled;
  await run();
  assert.equal(compiled, before + 2);
  options.fastMath = false;
  before = compiled;
  await run();
  assert.equal(compiled, before + 2);
  options.minimumOS = [26, 0];
  before = compiled;
  await run();
  assert.equal(compiled, before + 2);
  source = "different source";
  before = compiled;
  await run();
  assert.equal(compiled, before + 2);
  before = compiled;
  await run({ runtimeManifestSha256: "5".repeat(64) });
  assert.equal(compiled, before + 2);
  badHit = true;
  before = compiled;
  await run();
  assert.equal(compiled, before + 2);
  badHit = false;
  // 缺少可信上下文时绕过缓存，而非弱化缓存键。
  // Missing trusted context bypasses caching rather than weakening its key.
  before = compiled;
  const oldWrites = writes;
  await run({});
  assert.equal(compiled, before + 2);
  assert.equal(writes, oldWrites);
  forceHit = true;
  full = true;
  before = compiled;
  const oldTranslated = translated;
  await run();
  assert.equal(compiled, before);
  assert.equal(translated - oldTranslated, 6927);
  assert.equal(proofs, 1);
  proofFails = true;
  await run();
  assert.equal(compiled, before);
  assert.equal(proofs, 2);
  // 持久化步骤被取消时，擦除已接受缓冲区，不能发布输出。
  // A cancelled persistence step erases accepted buffers and cannot publish output.
  full = false;
  forceHit = false;
  proofFails = false;
  abortWrite = true;
  source = "cancel fresh source";
  accepted = [];
  await run();
  assert.equal(accepted.length, 1);
  assert.deepEqual(accepted[0], new Uint8Array(1));
}

// shader-retirement-assets
if (scenario === "shader-retirement-assets") {
  // 用真实清单认证与合成线程验证着色器退休时的公开字节所有权；绝不编译或执行 WASM。
  // Verify public-byte ownership at shader retirement with real manifest authentication and inert workers; never compile or execute WASM.
  const { default: test, mock: mock } = await import("node:test");
  const { default: assert } = await import("node:assert/strict");
  const { createHash: createHash } = await import("node:crypto");
  const { gzipSync: gzipSync } = await import("node:zlib");
  const { setImmediate: nextTurn } = await import("node:timers/promises");
  const { AIR_COMPILER_RUNTIME: AIR, airCompilerRuntimeComponent: airCompilerRuntimeComponent } =
    await import("../../air-compiler-runtime-contract.mjs");

  const encode = (text) => new TextEncoder().encode(text);
  const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const CROSS = "toolchain/spirv-cross-spike/wwwroot/spirv-cross.wasm";
  const DOTNET = "toolchain/offline-runtime-spike/dotnet-dist";
  const shell = encode("synthetic shell");
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

  async function setup(t, { transport = "direct", alias = false, components } = {}) {
    const files = new Map([
      [profile.archivePath, shell],
      ["profile/metadata.json", encode("synthetic profile metadata")],
      ["catalog/shader_map.tsv", encode("synthetic shader catalog")],
      ["sealed-runtime/dotnet-worker.mjs", encode("inert translator worker")],
      [`${DOTNET}/runtime.wasm`, encode("synthetic translator WASM")],
      ["sealed-workers/cross-worker.mjs", encode("inert Cross worker")],
      [CROSS, encode("synthetic Cross WASM")],
      [AIR.entry, encode("inert AIR worker")],
      [AIR.factory, encode("inert AIR factory")],
      [AIR.wasm, encode("synthetic AIR WASM")],
      ["sealed-workers/program-worker.mjs", encode("inert program worker")],
      ["toolchain/program-rewrite-spike/dist/capstone-lz4.wasm", encode("synthetic program WASM")],
    ]);
    const record = (path) => ({
      path,
      bytes: files.get(path).length,
      sha256: sha(files.get(path)),
    });
    files.set(
      `${DOTNET}/manifest.json`,
      encode(
        JSON.stringify({
          version: 1,
          assets: [{ ...record(`${DOTNET}/runtime.wasm`), path: "runtime.wasm" }],
        }),
      ),
    );
    const receipt = {
      version: AIR.version,
      id: AIR.id,
      apiVersion: AIR.apiVersion,
      headerBundleVersion: AIR.headerBundleVersion,
      sourceLanguage: AIR.sourceLanguage,
      emscripten: "4.0.20",
      headerSha256: sha(encode("synthetic headers")),
      sourceFingerprintSha256: sha(encode("synthetic source")),
      sourceManifestSha256: sha(encode("synthetic source manifest")),
      sourceCommit: "a".repeat(40),
      sourceInputs: { "toolchain.mjs": sha(encode("synthetic toolchain")) },
      files: {
        "d2r_air_wasm.mjs": {
          bytes: files.get(AIR.factory).length,
          sha256: sha(files.get(AIR.factory)),
        },
        "d2r_air_wasm.wasm": {
          bytes: files.get(AIR.wasm).length,
          sha256: sha(files.get(AIR.wasm)),
        },
      },
    };
    files.set(AIR.receipt, encode(JSON.stringify(receipt)));
    if (alias) {
      if (alias === true || alias === AIR.wasm)
        files.set("profile/air-alias.bin", files.get(AIR.wasm));
      if (alias === true || alias === CROSS) files.set("profile/cross-alias.bin", files.get(CROSS));
    }
    const assets = [...files.keys()].map(record);
    const manifest = {
      version: 1,
      assets,
      totalBytes: assets.reduce((total, asset) => total + asset.bytes, 0),
      components: { airCompiler: airCompilerRuntimeComponent(receipt) },
    };
    const downloads = new Map(files);
    if (transport !== "direct") {
      const unique = [...new Map(assets.map((asset) => [asset.sha256, asset])).values()];
      const groups = transport === "shared" ? [unique] : unique.map((asset) => [asset]);
      const payloads = [],
        entries = [];
      for (const [index, group] of groups.entries()) {
        const path = `payload/${index}.bin`;
        const decoded = new Uint8Array(group.reduce((sum, item) => sum + item.bytes, 0));
        let offset = 0;
        for (const asset of group) {
          decoded.set(files.get(asset.path), offset);
          entries.push({ file: path, offset, bytes: asset.bytes, sha256: asset.sha256 });
          offset += asset.bytes;
        }
        const encoded = transport === "gzip" ? new Uint8Array(gzipSync(decoded)) : decoded;
        downloads.set(path, encoded);
        payloads.push({
          path,
          bytes: encoded.length,
          sha256: sha(encoded),
          encoding: transport === "gzip" ? "gzip" : "identity",
          decodedBytes: decoded.length,
        });
      }
      manifest.transport = {
        version: 1,
        files: payloads,
        entries,
        totalBytes: payloads.reduce((sum, file) => sum + file.bytes, 0),
      };
    }
    const raw = encode(JSON.stringify(manifest));
    downloads.set("runtime-manifest.json", raw);
    const state = { workers: [], fetches: 0, preparations: 0, originals: new Map(), revoked: [] };
    const digest = crypto.subtle.digest.bind(crypto.subtle);
    t.mock.method(crypto.subtle, "digest", (algorithm, bytes) => {
      for (const path of [AIR.wasm, CROSS]) {
        if (!state.originals.has(path) && sha(bytes) === sha(files.get(path)))
          state.originals.set(path, bytes);
      }
      return digest(algorithm, bytes);
    });
    t.mock.method(WebAssembly, "compile", async (bytes) => {
      state.preparations++;
      assert.deepEqual(bytes, files.get(AIR.wasm));
      assert.notEqual(bytes.buffer, state.originals.get(AIR.wasm).buffer);
      return { inertModule: true };
    });
    const revoke = URL.revokeObjectURL.bind(URL);
    t.mock.method(URL, "revokeObjectURL", (url) => {
      state.revoked.push(url);
      revoke(url);
    });
    t.mock.method(globalThis, "fetch", async (url) => {
      state.fetches++;
      const bytes = downloads.get(new URL(url).pathname.slice("/runtime/".length));
      assert.ok(bytes, "fetch only authenticated synthetic public inventory");
      return new Response(bytes);
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
      reply(data) {
        const event = new MessageEvent("message", { data });
        this.onmessage?.(event);
        this.dispatchEvent(event);
      }
      postMessage(message, transfers = []) {
        const copy = structuredClone(message, { transfer: transfers });
        this.requests.push(copy);
        if (copy.type !== "initialize") return;
        this.air = copy.factoryBytes instanceof ArrayBuffer;
        queueMicrotask(() =>
          this.reply(
            this.air
              ? { type: "initialized", id: copy.id, heapBytes: 4096 }
              : { id: copy.id, ok: true, result: { initialized: true } },
          ),
        );
      }
      terminate() {
        this.terminations++;
      }
    };
    const { prepareSealedRuntime, assertSealedRuntime } = await import(
      `../../sealed-runtime/clients.mjs?retirement=${++serial}`
    );
    const runtime = await prepareSealedRuntime({
      baseUrl: "https://example.test/runtime/",
      manifestSha256: sha(raw),
      components: components ?? ["translator", "spirvCross", "program"],
    });
    t.after(() => runtime.close());
    return { runtime, state, files, assertSealedRuntime };
  }

  const release = { releasePublicWasm: true };
  function assertReadable(runtime, files, paths = [AIR.wasm, CROSS]) {
    for (const path of paths) assert.deepEqual(runtime.readPublicAsset(path), files.get(path));
  }
  function assertOriginalsIntact(state, files) {
    for (const path of [AIR.wasm, CROSS])
      assert.deepEqual(state.originals.get(path), files.get(path));
  }
  function compile(client) {
    return client.compile("kernel void main0() {}", { entry: "main0", stage: "kernel" });
  }
  function compiled(worker, { recycle = false } = {}) {
    const output = new Uint8Array(88).fill(7);
    worker.reply({
      type: "compiled",
      id: worker.requests.at(-1).id,
      bytes: output.buffer,
      recycle,
      heapBytes: 4096,
      compilationCount: 1,
      inspection: { validation: "container-and-integrity-only", outputSize: output.length },
    });
    return output;
  }

  for (const transport of ["direct", "identity", "gzip"])
    test(`default retirement preserves all public reads (${transport})`, async (t) => {
      const { runtime, state, files } = await setup(t, { transport });
      assert.equal(runtime.retireShaderWorkers(), true);
      assert.equal(runtime.retireShaderWorkers({ releasePublicWasm: false }), true);
      assertReadable(runtime, files, [...files.keys()]);
      assertOriginalsIntact(state, files);
      assert.equal(state.revoked.length, 0);
    });

  for (const transport of ["direct", "identity", "gzip"])
    test(`production opt-in drops only dedicated shader WASM references (${transport})`, async (t) => {
      const { runtime, state, files, assertSealedRuntime } = await setup(t, { transport });
      const copies = [runtime.readPublicAsset(AIR.wasm), runtime.readPublicAsset(CROSS)];
      const fetches = state.fetches;
      assert.equal(runtime.retireShaderWorkers(release), true);
      assert.equal(runtime.retireShaderWorkers(release), true);
      for (const path of [AIR.wasm, CROSS])
        assert.throws(() => runtime.readPublicAsset(path), /Missing verified runtime asset/);
      assertReadable(
        runtime,
        files,
        [...files.keys()].filter((path) => ![AIR.wasm, CROSS].includes(path)),
      );
      assert.deepEqual(copies, [files.get(AIR.wasm), files.get(CROSS)]);
      assertOriginalsIntact(state, files);
      assert.equal(state.fetches, fetches);
      assert.equal(state.revoked.length, 0);
      assert.equal(assertSealedRuntime(runtime), runtime);
      assert.equal(
        state.workers.filter((worker) => worker.terminations === 1).length,
        2 + state.workers.filter((worker) => worker.air).length,
      );
      assert.throws(() => runtime.createAirCompilerClient(), /unavailable/);
      assert.throws(() => runtime.createTranslator(), /retired/);
      assert.throws(() => runtime.createCrossWorker(), /retired/);
      const program = runtime.createProgramWorker();
      const pending = program.run({
        bytes: new Uint8Array([1]),
        moduleName: "synthetic",
        releaseId: "synthetic",
      });
      const worker = state.workers.find(
        (item) => item.requests[0].assets?.[0]?.[0] === "capstone-lz4.wasm",
      );
      worker.reply({ id: worker.requests.at(-1).id, ok: true, files: {} });
      await pending;
      program.close();
      await runtime.close();
      assertOriginalsIntact(state, files);
      assert.throws(() => runtime.readPublicAsset(profile.archivePath), /closed/);
    });

  for (const options of [{ transport: "shared" }, { transport: "identity", alias: true }])
    test(`opt-in preserves shared backing and whole-buffer aliases (${JSON.stringify(options)})`, async (t) => {
      const { runtime, state, files } = await setup(t, options);
      if (options.transport === "shared")
        assert.equal(state.originals.get(AIR.wasm).buffer, state.originals.get(CROSS).buffer);
      else
        for (const original of state.originals.values())
          assert.equal(original.byteLength, original.buffer.byteLength);
      assert.equal(runtime.retireShaderWorkers(release), true);
      assert.equal(runtime.retireShaderWorkers(release), true);
      assertReadable(runtime, files, [...files.keys()]);
      assertOriginalsIntact(state, files);
    });

  for (const alias of [AIR.wasm, CROSS])
    test(`one aliased candidate does not block the other dedicated release (${alias})`, async (t) => {
      const { runtime, state, files } = await setup(t, { transport: "identity", alias });
      assert.equal(runtime.retireShaderWorkers(release), true);
      assertReadable(runtime, files, [alias]);
      const released = alias === AIR.wasm ? CROSS : AIR.wasm;
      assert.throws(() => runtime.readPublicAsset(released), /Missing verified runtime asset/);
      assertOriginalsIntact(state, files);
    });

  test("idle sibling leases and active work block all retirement cleanup", async (t) => {
    const { runtime, state, files } = await setup(t);
    const translator = runtime.createTranslator();
    const cross = runtime.createCrossWorker();
    const compiler = runtime.createAirCompilerClient();
    const sibling = runtime.createAirCompilerClient();
    for (const close of [
      () => translator.close(),
      () => cross.terminate(),
      () => compiler.dispose(),
    ]) {
      assert.equal(runtime.retireShaderWorkers(release), false);
      assertReadable(runtime, files);
      assert.ok(state.workers.every((worker) => worker.terminations === 0));
      close();
    }
    const pending = compile(sibling);
    await nextTurn();
    assert.equal(runtime.retireShaderWorkers(release), false);
    assertReadable(runtime, files);
    const output = compiled(state.workers.find((worker) => worker.air));
    assert.equal((await pending).bytes.buffer, output.buffer);
    assert.equal(runtime.retireShaderWorkers(release), false);
    sibling.dispose();
    assert.equal(runtime.retireShaderWorkers(release), true);
    assert.deepEqual(output, new Uint8Array(88).fill(7));
    assertOriginalsIntact(state, files);
  });

  test("compiler failure stops its pool but keeps public originals until retirement", async (t) => {
    const { runtime, state, files } = await setup(t);
    const compiler = runtime.createAirCompilerClient();
    const first = compile(compiler);
    await nextTurn();
    const initial = state.workers.find((worker) => worker.air);
    initial.reply({ type: "error", id: initial.requests.at(-1).id, code: "AIR_FRONTEND_FAILED" });
    await assert.rejects(first, { code: "AIR_FRONTEND_FAILED" });
    assert.equal(compiler.ready, false);
    assert.ok(
      state.workers.filter((worker) => worker.air).every((worker) => worker.terminations === 1),
    );
    await assert.rejects(compile(compiler), { code: "AIR_COMPILER_DISPOSED" });
    assert.equal(runtime.retireShaderWorkers(release), false);
    assertReadable(runtime, files);
    compiler.dispose();
    assert.equal(runtime.retireShaderWorkers(release), true);
    assertOriginalsIntact(state, files);
  });

  test("compiler recycling reuses prepared code and keeps originals until successful retirement", async (t) => {
    const { runtime, state, files } = await setup(t);
    const compiler = runtime.createAirCompilerClient();
    const initialCount = state.workers.filter((worker) => worker.air).length;
    const first = compile(compiler);
    await nextTurn();
    const recycledOutput = compiled(
      state.workers.find((worker) => worker.air),
      { recycle: true },
    );
    assert.equal((await first).bytes.buffer, recycledOutput.buffer);
    assertReadable(runtime, files);
    const second = compile(compiler);
    await nextTurn();
    const finalOutput = compiled(state.workers.at(-1));
    await second;
    assert.equal(state.preparations, 1, "recycling reuses the prepared inert module");
    assert.equal(state.workers.filter((worker) => worker.air).length, initialCount + 1);
    compiler.dispose();
    assert.equal(runtime.retireShaderWorkers(release), true);
    assertOriginalsIntact(state, files);
    for (const output of [recycledOutput, finalOutput])
      assert.deepEqual(output, new Uint8Array(88).fill(7));
  });

  test("missing shader components cannot release public WASM", async (t) => {
    const { runtime, state, files } = await setup(t, { components: ["airCompiler"] });
    assert.equal(runtime.retireShaderWorkers(release), false);
    assertReadable(runtime, files);
    assertOriginalsIntact(state, files);
    assert.ok(state.workers.every((worker) => worker.terminations === 0));
  });

  test("invalid opt-in fails before retirement and explicit later cleanup is idempotent", async (t) => {
    const { runtime, state, files } = await setup(t);
    for (const releasePublicWasm of [null, 1, "true", {}, []]) {
      assert.throws(
        () => runtime.retireShaderWorkers({ releasePublicWasm }),
        /Invalid shader retirement option/,
      );
      assertReadable(runtime, files);
      assert.ok(state.workers.every((worker) => worker.terminations === 0));
    }
    assert.equal(runtime.retireShaderWorkers(), true);
    assertReadable(runtime, files);
    assert.equal(runtime.retireShaderWorkers(release), true);
    assert.equal(runtime.retireShaderWorkers(release), true);
    assert.throws(() => runtime.readPublicAsset(AIR.wasm), /Missing verified runtime asset/);
    assertOriginalsIntact(state, files);
  });
}

// production-shader-concurrency
if (scenario === "production-shader-concurrency") {
  // 合成目录与可控编译替身检查生产并发，不执行真实编译器、WASM 或游戏输入。
  // Synthetic catalogs and controlled compiler substitutes exercise production concurrency, not real compilers, WASM or game inputs.
  const { default: test, mock: mock } = await import("node:test");
  const { default: assert } = await import("node:assert/strict");
  const { createHash: createHash } = await import("node:crypto");
  const { setImmediate: nextTurn } = await import("node:timers/promises");
  const keys = Array.from({ length: 6927 }, (_, index) => index.toString(16).padStart(16, "0"));
  let state;
  mock.module("../../sealed-runtime/clients.mjs", { namedExports: { assertSealedRuntime() {} } });
  mock.module("../../shader-catalog-adapter.mjs", { namedExports: { assertShaderCatalog() {} } });
  mock.module("../../extraction-job-adapter.mjs", {
    namedExports: {
      readAuthenticatedAssets: () => ({ logicalAssets: { read: async () => new Uint8Array([1]) } }),
      readAuthenticatedProgramModules: () => ({}),
    },
  });
  mock.module("../../logical-asset-suffixes.mjs", {
    namedExports: { resolveLogicalAssetSuffixes: () => ["data:data/hd/shaders/synthetic"] },
  });
  mock.module("../../browser-task-yield.mjs", {
    namedExports: {
      async yieldBrowserTask({ signal }) {
        signal?.throwIfAborted();
      },
    },
  });
  mock.module("../../shader-native-artifacts.mjs", {
    namedExports: {
      c1NeedsFullBuffer: (source) => Number(source.slice(9)) % 2 === 0,
      readableTileVariant: (source) => ({ msl: `${source}_fetch`, metadata: { synthetic: true } }),
    },
  });
  mock.module("../../shader-pipeline.mjs", {
    namedExports: {
      createShaderPipeline() {
        return {
          async translate({ job }) {
            state.translated.push(job.key);
            return {
              msl: `synthetic${parseInt(job.key, 16)}`,
              replay: {
                entry: "main0",
                msl_entry_kind: job.stage === "ps" ? "fragment" : "kernel",
                bindings: [],
              },
              resources: { notes: [] },
            };
          },
          async verifyCompleteSourceSet() {
            assert.equal(state.active, 0, "proof waits for every compile");
            assert.equal(state.translated.length, 6927);
            state.proofs++;
            if (state.proofFails) throw new Error("synthetic proof failure");
            return { sourceDigest: "be59c864f087b377" };
          },
          dispose() {},
        };
      },
    },
  });
  mock.module("../../compiled-shader-boundary.mjs", {
    namedExports: {
      async acceptCompiledShaderOutput({ binary, expected, signal }) {
        signal?.throwIfAborted();
        const bytes = binary.slice();
        state.accepted.push(bytes);
        return {
          bytes,
          receipt: { compiler: { id: "synthetic" }, ...expected },
          receiptSha256: `synthetic-${expected.key}-${expected.variant}`,
        };
      },
    },
  });
  const { createProductionShaderAdapter } = await import("../../production-shaders.mjs");
  function setup(concurrency, { automatic = false, cache = false, proofFails = false } = {}) {
    state = {
      active: 0,
      peak: 0,
      calls: [],
      translated: [],
      proofs: 0,
      accepted: [],
      raw: [],
      progress: [],
      writeActive: 0,
      writes: 0,
      proofFails,
    };
    const current = state;
    const controller = new AbortController();
    const adapter = createProductionShaderAdapter({
      runtime: {
        workerUrls: {},
        createShaderWorkerFactory() {},
        createAirCompilerClient() {
          return {
            concurrency,
            compiler: { id: "synthetic" },
            options: {},
            limits: { outputBytes: 1024 },
            async compile(source, { signal }) {
              signal.throwIfAborted();
              current.active++;
              current.peak = Math.max(current.peak, current.active);
              assert.ok(current.active <= concurrency);
              const job = Promise.withResolvers();
              const abort = () => job.reject(signal.reason);
              signal.addEventListener("abort", abort, { once: true });
              const index = Number(source.match(/\d+/)[0]);
              const complete = () => {
                const bytes = new Uint8Array([index % 251, source.endsWith("_fetch") ? 1 : 0]);
                current.raw.push(bytes);
                job.resolve({ bytes, compiler: { id: "synthetic" }, options: {} });
              };
              current.calls.push({ source, complete, reject: job.reject });
              if (automatic) queueMicrotask(complete);
              try {
                return await job.promise;
              } finally {
                current.active--;
                signal.removeEventListener("abort", abort);
              }
            },
            dispose() {},
          };
        },
      },
      catalog: {
        keys,
        bfWant: [],
        describe(key) {
          return { name: "synthetic", index: parseInt(key, 16), embedded: false };
        },
        createOwnedPackSource() {
          return {
            async prepare(key) {
              return {
                job: { key, stage: parseInt(key, 16) % 2 ? "ps" : "cs" },
                code: new Uint8Array([1]),
              };
            },
            dispose() {},
          };
        },
      },
      outputCache: cache
        ? {
            read: async () => null,
            async write({ signal }) {
              signal.throwIfAborted();
              assert.equal(current.writeActive, 0, "cache commits are serialized");
              current.writeActive++;
              await nextTurn();
              current.writeActive--;
              current.writes++;
              return true;
            },
          }
        : undefined,
      cacheContext: { runtimeManifestSha256: "1".repeat(64) },
      trustedShell: {
        trustedRecipe: {
          shaderKeys: keys,
          shaderDigest: "be59c864f087b377",
          chain: "123456789abc",
        },
      },
    });
    const result = adapter.generateShaders({
      signal: controller.signal,
      onProgress: (event) => current.progress.push(event),
    });
    result.catch(() => {});
    return { state: current, adapter, controller, result };
  }
  async function until(predicate) {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      if (predicate()) return;
      await nextTurn();
    }
    assert.fail("synthetic condition did not settle");
  }

  test("translation overlaps four compiles without preparing a fifth record; cancellation drains all", async () => {
    const run = setup(4);
    await until(() => run.state.active === 4);
    assert.equal(run.state.translated.length, 4);
    await nextTurn();
    assert.equal(run.state.translated.length, 4);
    run.state.calls.find((call) => call.source === "synthetic2").complete();
    await until(() => run.state.translated.length === 5);
    assert.equal(run.state.proofs, 0);
    run.controller.abort();
    await assert.rejects(run.result, { name: "AbortError" });
    assert.equal(run.state.active, 0);
    assert.ok(run.state.accepted.every((bytes) => bytes.every((x) => x === 0)));
    assert.ok(run.state.raw.every((bytes) => bytes.every((x) => x === 0)));
    assert.ok(run.state.progress.every((event) => event.compilerWorkers === 4));
    run.adapter.dispose();
  });

  test("the first compile failure aborts siblings and retains its own record diagnostic", async () => {
    const run = setup(4);
    await until(() => run.state.active === 4);
    const failure = new Error("synthetic compiler failure");
    failure.code = "AIR_FRONTEND_FAILED";
    run.state.calls.find((call) => call.source === "synthetic2").reject(failure);
    await assert.rejects(run.result, (error) => {
      assert.equal(error.code, "AIR_FRONTEND_FAILED");
      assert.equal(error.shaderDiagnostic.record, 3);
      assert.equal(error.shaderDiagnostic.substep, "compilation");
      return true;
    });
    assert.equal(run.state.active, 0);
    assert.equal(run.state.proofs, 0);
    assert.equal(run.state.translated.length, 4);
    run.adapter.dispose();
  });

  async function outputDigest(concurrency, cache) {
    const run = setup(concurrency, { automatic: true, cache });
    const entries = await run.result;
    const hash = createHash("sha256");
    for (const entry of entries) {
      hash.update(entry.path);
      for await (let bytes of entry.chunks()) {
        if (entry.path === "dm/c1_full.tsv")
          bytes = new TextEncoder().encode(new TextDecoder().decode(bytes).replace(/^#.*\n/, ""));
        hash.update(bytes);
      }
    }
    assert.equal(run.state.proofs, 1);
    assert.equal(run.state.calls.length, 6927 + Math.floor(6927 / 2));
    assert.ok(run.state.peak <= concurrency);
    if (cache) assert.equal(run.state.writes, run.state.calls.length);
    run.adapter.dispose();
    assert.ok(run.state.accepted.every((bytes) => bytes.every((x) => x === 0)));
    return hash.digest("hex");
  }
  test("sequential and parallel output order/base-fetch receipts match and every concurrent cache result persists", async () => {
    assert.equal(await outputDigest(4, true), await outputDigest(1, false));
  });

  test("all parallel outputs are withheld and wiped when final source proof fails", async () => {
    const run = setup(4, { automatic: true, proofFails: true });
    await assert.rejects(run.result, /synthetic proof failure/);
    assert.equal(run.state.proofs, 1);
    assert.equal(run.state.active, 0);
    assert.ok(run.state.accepted.every((bytes) => bytes.every((x) => x === 0)));
    run.adapter.dispose();
  });
}
