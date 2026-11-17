// Feature suite: asset-generation. Original test sections retain independent lexical scopes.
import * as nodeTestModule from "node:test";
import * as strictModule from "node:assert/strict";
import * as transformsModule from "../toolchain/asset-transforms-js/transforms.mjs";
import * as recipeModule from "../toolchain/overlay-recipe-js/recipe.mjs";
import * as recipesModule from "../toolchain/remaining-art-js/recipes.mjs";
import * as assetGeneratorAdapterModule from "../asset-generator-adapter.mjs";
import * as nodeChild_processModule from "node:child_process";
import * as nodeCryptoModule from "node:crypto";
import * as assetPipelineAdapterModule from "../asset-pipeline-adapter.mjs";
import * as extractionAdapterFixtureModule from "./helpers/extraction-adapter-fixture.mjs";
import * as mapGoalGeneratorModule from "../map-goal-generator.mjs";
import * as cascOverlayFixtureModule from "./helpers/casc-overlay-fixture.mjs";
import * as utilModule from "../toolchain/art-recipes-js/util.mjs";
import * as fontModule from "../toolchain/font-rasterizer-exact/font.mjs";
import * as hudGeneratorModule from "../hud-generator.mjs";
import * as revisionModule from "../toolchain/art-recipes-js/revision.mjs";
import * as logicalAssetSuffixesModule from "../logical-asset-suffixes.mjs";
import * as remainingArtGeneratorModule from "../remaining-art-generator.mjs";
import * as mercenaryArtGeneratorModule from "../mercenary-art-generator.mjs";
import * as mapGoalModule from "../toolchain/art-recipes-js/map-goal.mjs";
import * as nativeArtGeneratorModule from "../native-art-generator.mjs";
import * as nativeArtPngDigestsModule from "../native-art-png-digests.mjs";
import * as nativeArtTextureModule from "../native-art-texture.mjs";
import * as browserTextureEncoderModule from "../toolchain/texture-spike/browser-texture-encoder.mjs";
import * as v61AssetWorkerModule from "../v61-asset-worker.mjs";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as nodeProcessModule from "node:process";
import * as nodeUrlModule from "node:url";
import * as nodeZlibModule from "node:zlib";
import * as inventoryModule from "../toolchain/overlay-recipe-js/inventory.mjs";
import * as annotationsModule from "../toolchain/overlay-recipe-js/annotations.mjs";
import * as inventoryParityModule from "../toolchain/overlay-recipe-js/test/inventory-parity.mjs";
import * as annotationsParityModule from "../toolchain/overlay-recipe-js/test/annotations-parity.mjs";
import * as overlayModule from "../toolchain/casc-overlay-js/overlay.mjs";
import * as annotationDataJsonModule from "../toolchain/overlay-recipe-js/annotation-data.json" with { type: "json" };
import * as releaseV2DataJsonModule from "../toolchain/overlay-recipe-js/release-v2-data.json" with { type: "json" };
import * as releaseV2Module from "../toolchain/overlay-recipe-js/release-v2.mjs";
import * as touchLayoutModule from "../toolchain/overlay-recipe-js/touch-layout.mjs";
import * as overlayRevisionModule from "../toolchain/overlay-recipe-js/revision.mjs";
import * as overlayContractModule from "../overlay-contract.mjs";
import * as nodeFsModule from "node:fs";
import * as nativeArtModule from "../toolchain/art-recipes-js/native-art.mjs";
import * as nativeCasesModule from "../toolchain/art-recipes-js/test/native-cases.mjs";
import * as supportModule from "../toolchain/hud-recipes-js/support.mjs";

// 原有独立作用域测试区段共用的套件内夹具。
// Suite-local fixtures shared by the original independently scoped sections.
const assetExtractionFixtureModule = (() => {
  const { createHash } = nodeCryptoModule;
  const { fixture, open, selection } = extractionAdapterFixtureModule;
  const { fixture: casc } = cascOverlayFixtureModule;
  const { MAP_GOAL_IMAGE_PATHS, MAP_GOAL_TABLES, MAP_GOAL_FONT } = mapGoalGeneratorModule;
  // 自编扁平 data/ RomFS 映像，通过真实提取适配器的有界线程协议提供；没有生产解析器覆盖或注册接缝。
  // Authored flat data/ RomFS image, served through the real extraction adapter's
  // bounded worker protocol. No production resolver override or registration seam.
  function romfs(files) {
    const items = Object.entries(files).map(([path, bytes]) => ({
      name: new TextEncoder().encode(path.slice(5)),
      bytes,
    }));
    let fileSize = 0,
      dataSize = 0;
    for (const item of items) {
      item.meta = fileSize;
      item.data = dataSize;
      fileSize += 32 + Math.ceil(item.name.length / 4) * 4;
      dataSize += item.bytes.length;
    }
    const dirStart = 84,
      dirSize = 52,
      hashStart = dirStart + dirSize,
      fileStart = hashStart + 4,
      dataStart = fileStart + fileSize;
    const out = new Uint8Array(dataStart + dataSize),
      v = new DataView(out.buffer);
    const u32 = (p, x) => v.setUint32(p, x, true),
      u64 = (p, x) => v.setBigUint64(p, BigInt(x), true);
    [80, 80, 4, dirStart, dirSize, hashStart, 4, fileStart, fileSize, dataStart].forEach((x, i) =>
      u64(i * 8, x),
    );
    [0, 0xffffffff, 24, 0xffffffff, 0xffffffff, 0].forEach((x, i) => u32(dirStart + i * 4, x));
    [0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 4].forEach((x, i) => u32(dirStart + 24 + i * 4, x));
    out.set(new TextEncoder().encode("data"), dirStart + 48);
    items.forEach((item, i) => {
      const p = fileStart + item.meta;
      u32(p, 24);
      u32(p + 4, items[i + 1]?.meta ?? 0xffffffff);
      u64(p + 8, item.data);
      u64(p + 16, item.bytes.length);
      u32(p + 24, 0xffffffff);
      u32(p + 28, item.name.length);
      out.set(item.name, p + 32);
      out.set(item.bytes, dataStart + item.data);
    });
    return out;
  }
  async function extractedFixture(template) {
    const files = Object.fromEntries(
      Object.entries(casc.files).map(([p, b]) => [p, Buffer.from(b, "base64")]),
    );
    const assets = {
      ...Object.fromEntries(
        [...Object.values(MAP_GOAL_IMAGE_PATHS), MAP_GOAL_FONT].map((p) => [
          p,
          Buffer.from("fixture"),
        ]),
      ),
      ...Object.fromEntries(
        MAP_GOAL_TABLES.map((p) => [`local/lng/strings/${p}.json`, Buffer.from("[]")]),
      ),
      "template.sprite": template,
    };
    let offset = 0;
    const rows = [],
      chunks = [];
    for (const [name, bytes] of Object.entries(assets)) {
      const ekey = "0".repeat(18) + "0003" + offset.toString(16).padStart(10, "0");
      rows.push(
        [
          "0",
          "0",
          "0",
          "3",
          createHash("md5").update(bytes).digest("hex"),
          bytes.length,
          ekey,
          bytes.length,
          "n",
          `data:data/${name.toLowerCase()}`,
          "0",
        ].join("|"),
      );
      chunks.push(bytes);
      offset += bytes.length;
    }
    files["data/build_manifest.psv"] = Buffer.concat([
      files["data/build_manifest.psv"],
      Buffer.from("\n" + rows.join("\n") + "\n"),
    ]);
    files["data/00-00000003.data"] = Buffer.concat(chunks);
    const f = fixture({ romfs: romfs(files) }),
      session = await open(f);
    return {
      extracted: await f.adapter.extractSelected({ session, selection }),
      selection,
      cleanup: f.adapter.cleanup,
    };
  }
  return { extractedFixture };
})();
const memoryOutputCacheModule = (() => {
  // 仅为合成模拟：模拟缓存字节所有权约定，而非文件系统或账本真实性。
  // Synthetic only: emulate the cache byte ownership contract, not filesystem/ledger authenticity.
  function memoryOutputCache() {
    const files = new Map();
    return {
      async read({ kind, key, maxBytes }) {
        const bytes = files.get(`${kind}/${key}`);
        return bytes && bytes.length <= maxBytes ? bytes.slice() : null;
      },
      async write({ kind, key, bytes }) {
        files.set(`${kind}/${key}`, bytes.slice());
        return true;
      },
    };
  }
  const assetCacheContext = Object.freeze({
    runtimeManifestSha256: "a".repeat(64),
    profileId: "synthetic-profile",
    artRevision: "synthetic-recipe",
  });
  return { memoryOutputCache, assetCacheContext };
})();

// asset-base64-allocation
{
  // 合成公共入口回归：大载荷解码保持字节和严格语法，不测量浏览器内存。
  // Synthetic public-entry regressions: large decodes preserve bytes and strict grammar, not browser RAM measurements.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { executeAssetTransform, VAULT_PATHS } = transformsModule;
  const { createOverlayRecipe, GENERATED } = recipeModule;
  const { createRemainingArtRecipes } = recipesModule;
  const encode = (value) => new TextEncoder().encode(JSON.stringify(value));
  const encoded = (bytes) => Buffer.from(bytes).toString("base64");
  const vaultPayload = (first) =>
    encode({
      manifest: "",
      assets: Object.fromEntries(VAULT_PATHS.map((name, i) => [name, i ? "" : first])),
    });
  const remaining = (inspect = () => {}) =>
    createRemainingArtRecipes({
      pixels: Object.fromEntries(
        ["crop", "paste", "flip", "convert", "resizeLanczos"].map((name) => [
          name,
          (image) => image,
        ]),
      ),
      png: { encodeRgba: () => new Uint8Array([1]) },
      numeric: {
        rgbStats() {},
        stone() {},
        feather() {},
        vaultCover(bytes) {
          inspect(bytes);
          return bytes;
        },
      },
    });
  const overlay = (
    decodeRgba = () => {
      throw new Error("DECODED");
    },
  ) => createOverlayRecipe({ png: { decodeRgba }, md5() {}, sha1() {} });
  function noStringIterable(t) {
    const from = Uint8Array.from;
    t.mock.method(Uint8Array, "from", function (value, ...args) {
      assert.notEqual(
        typeof value,
        "string",
        "large binary strings must not expand through an iterable",
      );
      return from.call(this, value, ...args);
    });
  }

  test("vault public transform decodes a 2 MiB asset without regex stack or string iterable expansion", async (t) => {
    noStringIterable(t);
    const bytes = new Uint8Array(2 * 1024 * 1024).fill(65);
    const digest = crypto.subtle.digest.bind(crypto.subtle);
    let observed = false;
    t.mock.method(crypto.subtle, "digest", (algorithm, value) => {
      assert.deepEqual(value, bytes);
      observed = true;
      return digest(algorithm, value);
    });
    await assert.rejects(
      executeAssetTransform("vault-table", vaultPayload(encoded(bytes))),
      /VAULT_ASSET_LIMIT/,
    );
    assert.equal(
      observed,
      true,
      "first decoded asset reached its byte digest before the empty second asset was rejected",
    );
  });

  test("overlay public recipe decodes a 16 MiB generated image byte-exactly without string iterable expansion", async (t) => {
    noStringIterable(t);
    const bytes = new Uint8Array(16 * 1024 * 1024);
    for (let i = 0; i < bytes.length; i++) bytes[i] = i % 256;
    let observed = false;
    const recipe = overlay((value) => {
      assert.deepEqual(value, bytes);
      observed = true;
      throw new Error("DECODED");
    });
    const generated = Object.fromEntries(
      GENERATED.map((name) => [name, name === "NativeHUD/v4/base.png" ? encoded(bytes) : "AA=="]),
    );
    await assert.rejects(
      recipe.execute("overlay-recipe", encode({ manifest: "", assets: {}, generated })),
      /DECODED/,
    );
    assert.equal(observed, true);
  });

  test("remaining-art public recipe decodes multi-MiB RGBA byte-exactly without string iterable expansion", (t) => {
    noStringIterable(t);
    const bytes = new Uint8Array(848 * 1024 * 4);
    for (let i = 0; i < bytes.length; i++) bytes[i] = i % 256;
    let observed = false;
    const recipe = remaining((value) => {
      assert.deepEqual(value, bytes);
      observed = true;
    });
    const result = recipe.execute(
      "remaining-art",
      encode({ image: { width: 848, height: 1024, rgba: encoded(bytes) } }),
      { kind: "vault", name: "VaultBottom" },
    );
    assert.equal(observed, true);
    assert.deepEqual(result.files["image.rgba"], bytes);
  });

  for (const value of [
    "A",
    "AA",
    "AAA",
    "A===",
    "====",
    "AA=A",
    "=AAA",
    "AAAA=",
    "AAAA===",
    "AA==AA==",
    "AA-_",
    "AA==\n",
    " AA==",
    "AA\t==",
    "AA==\r\n",
    "AA==\u2028",
  ])
    test(`asset base64 decoders reject malformed padding/alphabet/whitespace ${JSON.stringify(value)}`, async () => {
      await assert.rejects(
        executeAssetTransform("vault-table", vaultPayload(value)),
        /VAULT_BASE64/,
      );
      await assert.rejects(
        overlay().execute(
          "overlay-recipe",
          encode({ manifest: "", assets: { "data/a": value }, generated: {} }),
        ),
        /OVERLAY_BASE64/,
      );
      assert.throws(
        () =>
          remaining().execute(
            "remaining-art",
            encode({ image: { width: 1, height: 1, rgba: value } }),
            { kind: "advanced", name: "unused" },
          ),
        /REMAINING_ART_RGBA/,
      );
    });

  for (const value of ["", "AA==", "AB==", "AAA=", "AAB=", "AAAA", "AA+/", "AAAAAA=="])
    test(`vault and overlay preserve accepted base64 grammar ${JSON.stringify(value)}`, async () => {
      await assert.rejects(
        executeAssetTransform("vault-table", vaultPayload(value)),
        /VAULT_ASSET_LIMIT/,
      );
      await assert.rejects(
        overlay().execute(
          "overlay-recipe",
          encode({ manifest: "", assets: { "data/a": value }, generated: {} }),
        ),
        /OVERLAY_GENERATED_ART_REQUIRED/,
      );
    });
}

// asset-generator-adapter
{
  // 验证 asset-generator-adapter 的约定与边界；依据 ../asset-generator-adapter.mjs 及本文件的测试断言。
  // Verify asset-generator-adapter contracts and boundaries against ../asset-generator-adapter.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createAssetGeneratorAdapter, ASSET_ADAPTER_CAPABILITIES, ASSET_ADAPTER_LIMITS } =
    assetGeneratorAdapterModule;

  const recipe = (outputs = { "frame.png": "NativeHUD/test.png" }) => ({
    operation: "image-default-frame",
    input: "data/Synthetic/test.sprite",
    frame: 1,
    outputs,
  });
  function fixture(options = {}) {
    const calls = [];
    let closed = 0;
    const output = new Uint8Array([1, 2, 3, 4]);
    const worker = {
      run: async (...args) => {
        calls.push(["worker", ...args]);
        return {
          files: { "frame.png": output.buffer, "frame.sprite": new Uint8Array([5]).buffer },
        };
      },
      close: () => closed++,
      ...options.worker,
    };
    const reader = {
      read: async (...args) => {
        calls.push(["reader", ...args]);
        return new Uint8Array([83, 112, 65, 49, 31, 0]);
      },
      ...options.reader,
    };
    const adapter = createAssetGeneratorAdapter({
      recipes: options.recipes ?? [recipe()],
      createWorker: () => worker,
    });
    return {
      adapter,
      calls,
      output,
      reader,
      get closed() {
        return closed;
      },
    };
  }
  async function collect(entry) {
    const result = [];
    for await (const bytes of entry.chunks()) result.push(...bytes);
    return result;
  }
  test("bounded logical read and existing worker become app-relative streamed entries", async () => {
    const f = fixture();
    const signal = new AbortController().signal;
    const entries = await f.adapter.generateAssets({
      extracted: { logicalAssets: f.reader },
      signal,
    });
    assert.equal(entries[0].path, "NativeHUD/test.png");
    assert.equal(entries[0].mode, 0o100644);
    assert.equal(entries[0].size, 4);
    assert.equal(f.calls[0][1], "data/synthetic/test.sprite");
    assert.deepEqual(f.calls[0][2], { maxBytes: ASSET_ADAPTER_LIMITS.inputBytes, signal });
    assert.equal(f.calls[1][1], "image-default-frame");
    assert.deepEqual(f.calls[1][3], { frame: 1 });
    f.output.fill(99);
    assert.deepEqual(await collect(entries[0]), [1, 2, 3, 4]);
    const first = await entries[0].chunks().next();
    first.value.fill(77);
    assert.deepEqual(await collect(entries[0]), [1, 2, 3, 4]);
    assert.equal(f.closed, 1);
    await f.adapter.dispose();
    assert.equal(f.closed, 1);
    await assert.rejects(collect(entries[0]), { code: "ASSET_DISPOSED" });
  });
  test("incomplete capability is explicit; table and overlay plans cannot pretend to be app assets", () => {
    assert.equal(ASSET_ADAPTER_CAPABILITIES.completeAssetPipeline, false);
    assert.equal(ASSET_ADAPTER_CAPABILITIES.imageDefaultFrameV61, false);
    for (const operation of ["experience-table", "overlay-paths", "casc-pack"]) {
      assert.throws(() => fixture({ recipes: [{ ...recipe(), operation }] }), {
        code: "ASSET_UNSUPPORTED_RECIPE",
      });
    }
  });
  test("unsafe and duplicate destinations fail before any worker or read", () => {
    for (const path of ["../x.png", "Payload/a.app/x.png", "x\\y.png", "a//b.png", "a/./b.png"]) {
      assert.throws(() => fixture({ recipes: [recipe({ "frame.png": path })] }));
    }
    assert.throws(() => fixture({ recipes: [recipe(), recipe()] }), {
      code: "ASSET_DUPLICATE_OUTPUT",
    });
    assert.throws(() => fixture({ recipes: [recipe({ "frame.rgba": "raw.png" })] }), {
      code: "ASSET_UNSUPPORTED_OUTPUT",
    });
  });
  test("v61 is not enabled before the separate texture adapter is verified and integrated", async () => {
    const f = fixture({ reader: { read: async () => new Uint8Array([83, 112, 65, 49, 61, 0]) } });
    await assert.rejects(f.adapter.generateAssets({ extracted: { logicalAssets: f.reader } }), {
      code: "ASSET_V61_NOT_CONNECTED",
    });
    assert.equal(f.calls.length, 0);
    assert.equal(f.closed, 1);
  });
  test("pre-abort and in-flight abort reject promptly without leaking abort reasons", async () => {
    const before = new AbortController();
    before.abort(new Error("private key-like diagnostic"));
    const f = fixture();
    await assert.rejects(f.adapter.generateAssets({ signal: before.signal }), {
      code: "ASSET_CANCELLED",
    });
    assert.equal(f.closed, 0);
    const during = new AbortController();
    let entered;
    const ready = new Promise((resolve) => {
      entered = resolve;
    });
    const g = fixture({
      worker: {
        run: () => {
          entered();
          return new Promise(() => {});
        },
      },
    });
    const pending = g.adapter.generateAssets({
      extracted: { logicalAssets: g.reader },
      signal: during.signal,
    });
    await ready;
    during.abort(new Error("private path"));
    await assert.rejects(pending, { code: "ASSET_CANCELLED" });
    assert.equal(g.closed, 1);
  });
  test("invalid read/output and worker errors fail closed with redacted errors", async () => {
    for (const [options, code] of [
      [{ reader: { read: async () => new ArrayBuffer(8) } }, "ASSET_INPUT_LIMIT"],
      [
        { worker: { run: async () => ({ files: { "frame.png": new Uint8Array([1]) } }) } },
        "ASSET_INVALID_RESULT",
      ],
      [{ worker: { run: async () => ({ files: {} }) } }, "ASSET_MISSING_OUTPUT"],
      [
        {
          worker: {
            run: async () => {
              throw new Error("secret data");
            },
          },
        },
        "ASSET_GENERATION_FAILED",
      ],
    ]) {
      const f = fixture(options);
      await assert.rejects(f.adapter.generateAssets({ extracted: { logicalAssets: f.reader } }), {
        message: code,
        code,
      });
      assert.equal(f.closed, 1);
    }
  });
  test("single-use adapter and fixed-size chunks bound lifecycle and output reads", async () => {
    const bytes = new Uint8Array(ASSET_ADAPTER_LIMITS.chunkBytes + 3);
    const f = fixture({ worker: { run: async () => ({ files: { "frame.png": bytes.buffer } }) } });
    const entries = await f.adapter.generateAssets({ extracted: { logicalAssets: f.reader } });
    const lengths = [];
    for await (const chunk of entries[0].chunks()) lengths.push(chunk.length);
    assert.deepEqual(lengths, [ASSET_ADAPTER_LIMITS.chunkBytes, 3]);
    await assert.rejects(f.adapter.generateAssets({ extracted: { logicalAssets: f.reader } }), {
      code: "ASSET_ADAPTER_SINGLE_USE",
    });
    await f.adapter.dispose();
  });

  test("hard input and aggregate output budgets reject rather than truncating", async () => {
    const oversized = fixture({
      reader: { read: async () => new Uint8Array(ASSET_ADAPTER_LIMITS.inputBytes + 1) },
    });
    await assert.rejects(
      oversized.adapter.generateAssets({ extracted: { logicalAssets: oversized.reader } }),
      { code: "ASSET_INPUT_LIMIT" },
    );
    const outputBytes = new ArrayBuffer(ASSET_ADAPTER_LIMITS.retainedOutputBytes / 2 + 1);
    const aggregate = fixture({
      recipes: [recipe(), recipe({ "frame.png": "NativeHUD/second.png" })],
      worker: { run: async () => ({ files: { "frame.png": outputBytes } }) },
    });
    await assert.rejects(
      aggregate.adapter.generateAssets({ extracted: { logicalAssets: aggregate.reader } }),
      { code: "ASSET_OUTPUT_LIMIT" },
    );
    assert.equal(aggregate.closed, 1);
    assert.throws(
      () =>
        fixture({
          recipes: Array.from({ length: ASSET_ADAPTER_LIMITS.recipes + 1 }, () => recipe()),
        }),
      { code: "ASSET_INVALID_RECIPES" },
    );
  });

  test("cleanup failure preserves a primary generation failure without unsafe finally throws", async () => {
    const f = fixture({
      worker: {
        run: async () => {
          throw new Error("private failure");
        },
        close: () => {
          throw new Error("private cleanup");
        },
      },
    });
    await assert.rejects(f.adapter.generateAssets({ extracted: { logicalAssets: f.reader } }), {
      code: "ASSET_GENERATION_FAILED",
      cleanupCode: "ASSET_WORKER_CLEANUP_FAILED",
    });
  });
}

// asset-generator-output-lifetime
{
  // 独立进程验证输出清零及引用释放，不依赖真实资源或主测试进程的 GC 状态。
  // An isolated process verifies output wiping and reference release without real assets or parent GC state.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;

  const adapterUrl = new URL("../asset-generator-adapter.mjs", import.meta.url).href;
  test("disposed asset entries and suspended iterators release owned output buffers", () => {
    const result = spawnSync(
      process.execPath,
      [
        "--expose-gc",
        "--input-type=module",
        "-e",
        `
      import assert from "node:assert/strict";
      import { createAssetGeneratorAdapter } from ${JSON.stringify(adapterUrl)};
      const borrowed = new Uint8Array(65537).fill(7);
      const input = new Uint8Array([83, 112, 65, 49, 31, 0]);
      let owned;
      const slice = ArrayBuffer.prototype.slice;
      ArrayBuffer.prototype.slice = function (...args) {
        const copy = slice.apply(this, args);
        if (this === borrowed.buffer) owned = new WeakRef(copy);
        return copy;
      };
      const adapter = createAssetGeneratorAdapter({
        recipes: [{ operation: "image-default-frame", input: "data/test.sprite",
          outputs: { "frame.png": "NativeHUD/test.png" } }],
        createWorker: () => ({
          run: async () => ({ files: { "frame.png": borrowed.buffer } }),
          close() {},
        }),
      });
      let entries;
      try {
        entries = await adapter.generateAssets({ extracted: {
          logicalAssets: { read: async () => input },
        } });
      } finally {
        ArrayBuffer.prototype.slice = slice;
      }
      assert.ok(owned?.deref());
      const iterator = entries[0].chunks();
      const first = await iterator.next();
      assert.equal(first.value.length, 65536);
      assert.ok(first.value.every(byte => byte === 7));
      await adapter.dispose();
      assert.ok(new Uint8Array(owned.deref()).every(byte => byte === 0));
      assert.ok(borrowed.every(byte => byte === 7));
      assert.deepEqual([...input], [83, 112, 65, 49, 31, 0]);
      assert.ok(first.value.every(byte => byte === 7));
      for (let attempt = 0; attempt < 20; attempt++) {
        await new Promise(resolve => setImmediate(resolve));
        globalThis.gc();
      }
      assert.equal(owned.deref(), undefined, "retained entry/iterator keeps disposed output alive");
      await assert.rejects(iterator.next(), error => error.code === "ASSET_DISPOSED");
      await assert.rejects(entries[0].chunks().next(), error => error.code === "ASSET_DISPOSED");
      assert.equal(entries[0].size, borrowed.length);
      await adapter.dispose();
      `,
      ],
      { encoding: "utf8", timeout: 15000 },
    );
    assert.equal(result.status, 0, result.stderr || result.error?.message);
  });
}

// asset-pipeline-adapter
{
  // 依据合成提取夹具检查资源流水线的验证、生命周期和清理契约。
  // Check asset pipeline validation, lifetime and cleanup contracts using synthetic extraction fixtures.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { createAssetPipelineAdapter } = assetPipelineAdapterModule;
  const { extractedFixture } = assetExtractionFixtureModule;
  const { MAP_GOAL_OUTPUTS } = mapGoalGeneratorModule;
  const { source, worker: overlayWorker, fixture } = cascOverlayFixtureModule;

  async function setup() {
    const calls = [],
      owned = [];
    let closed = 0,
      frames = 7;
    const template = new Uint8Array(48),
      header = new DataView(template.buffer);
    header.setUint32(0, 0x31417053, true);
    header.setUint16(4, 61, true);
    header.setUint16(6, 4, true);
    header.setUint32(8, 4, true);
    header.setUint32(12, 4, true);
    header.setUint16(20, 1, true);
    const sha256 = createHash("sha256").update(template).digest("hex");
    const py = overlayWorker();
    const options = {
      mapGoal: true,
      createAssetWorker: () => ({
        close() {
          closed++;
        },
        async run(op, payload, args) {
          calls.push(op);
          if (op === "map-goal")
            return {
              files: Object.fromEntries(MAP_GOAL_OUTPUTS.map((n) => [n, new Uint8Array([1, 2])])),
            };
          return py.run(op, payload, args);
        },
      }),
      createTextureClient: () => ({
        close() {
          closed++;
        },
        async decodeAtlas(bytes) {
          calls.push("decode");
          const count = bytes[0] === 0x53 ? 1 : frames++;
          const pixels = new Uint8Array(count * 4 * 4 * 4);
          owned.push(pixels);
          return {
            width: count * 4,
            height: 4,
            frameWidth: 4,
            frameCount: count,
            pixels,
            pixelFormat: 70,
            bytesPerPixel: 4,
          };
        },
      }),
      createTextureEncoder: () => ({
        close() {
          closed++;
        },
        async encodeAndPack({ width, height, templateHeader, rgba }) {
          calls.push("encode");
          assert.equal(templateHeader.length, 40);
          assert.equal(rgba.length, width * height * 4);
          return { width, height, sprite: template.slice(), linearBC3: new Uint8Array(16) };
        },
      }),
      textureReplacements: [
        {
          input: "data/template.sprite",
          replacement: fixture.replacements[0].path,
          add: false,
          sha256,
          transform: ({ pixels, width, height }) => {
            calls.push("transform");
            return { rgba: pixels.slice(), width, height };
          },
        },
      ],
    };
    return { options, calls, owned, closed: () => closed, auth: await extractedFixture(template) };
  }
  async function bytes(entry) {
    const parts = [];
    for await (const b of entry.chunks()) parts.push(b);
    return Buffer.concat(parts);
  }

  test("map-goal and BC3 replacement compose through real bounded CASC writer into native destinations", async () => {
    const f = await setup(),
      adapter = createAssetPipelineAdapter(f.options);
    const entries = await adapter.generateAssets(f.auth);
    assert.equal(entries.filter((e) => e.path.startsWith("mapgoal/")).length, 195);
    assert.deepEqual(
      entries.filter((e) => e.path.startsWith("romfs_overlay/")).map((e) => e.path),
      ["romfs_overlay/data/.build.config", "romfs_overlay/data/00-00000002.data"],
    );
    assert.equal(f.calls[0], "decode");
    assert.ok(f.calls.indexOf("map-goal") < f.calls.indexOf("transform"));
    assert.ok(f.calls.indexOf("transform") < f.calls.indexOf("encode"));
    assert.ok(f.calls.indexOf("encode") < f.calls.indexOf("casc-overlay"));
    assert.equal(f.closed(), 3);
    for (const b of f.owned) assert.ok(b.every((v) => v === 0));
    for (const entry of entries) assert.equal((await bytes(entry)).length, entry.size);
    // 暂存和校验前可重复读取；清理只失效生产器，不修改条目元数据或已返回副本。
    // Reads remain repeatable before staging/validation; cleanup invalidates producers,
    // not entry metadata or previously yielded copies.
    const metadata = entries.map(({ path, size, mode }) => ({ path, size, mode }));
    const first = await bytes(entries[0]);
    assert.deepEqual(await bytes(entries[0]), first);
    const iterator = entries[0].chunks();
    const yielded = (await iterator.next()).value;
    const saved = yielded.slice();
    await iterator.return();
    assert.equal(adapter.capabilities.completeAssetPipeline, false);
    await adapter.dispose();
    assert.deepEqual(yielded, saved);
    assert.deepEqual(
      entries.map(({ path, size, mode }) => ({ path, size, mode })),
      metadata,
    );
    for (const entry of entries) await assert.rejects(bytes(entry), /CANCELLED/);
    await adapter.dispose();
    await assert.rejects(adapter.generateAssets(f.auth), /SINGLE_USE/);
  });
  test("default resolver rejects forged extracted capabilities before workers or reads", async () => {
    const f = await setup();
    let created = false;
    f.options.createAssetWorker = () => {
      created = true;
    };
    await assert.rejects(
      createAssetPipelineAdapter(f.options).generateAssets({
        extracted: { romfs: source(), authenticated: true },
        selection: {},
      }),
      /^Error: ASSET_PIPELINE_FAILED$/,
    );
    assert.equal(created, false);
    assert.deepEqual(f.calls, []);
  });
  test("source hash mismatch stops before decode, transform, encode or overlay", async () => {
    const f = await setup();
    f.options.mapGoal = false;
    f.options.textureReplacements[0].sha256 = "0".repeat(64);
    await assert.rejects(
      createAssetPipelineAdapter(f.options).generateAssets(f.auth),
      /SOURCE_HASH/,
    );
    assert.ok(!f.calls.includes("decode"));
    assert.ok(!f.calls.includes("encode"));
    assert.equal(f.closed(), 3);
    for (const b of f.owned) assert.ok(b.every((v) => v === 0));
  });
  test("recipe validation and snapshot precede input access", async () => {
    const f = await setup();
    for (const replacement of ["../escape", "Payload/App.app/x", "data/../x"])
      assert.throws(
        () =>
          createAssetPipelineAdapter({
            ...f.options,
            textureReplacements: [{ ...f.options.textureReplacements[0], replacement }],
          }),
        /RECIPE/,
      );
    const adapter = createAssetPipelineAdapter(f.options);
    f.options.textureReplacements[0].sha256 = "0".repeat(64);
    const entries = await adapter.generateAssets(f.auth);
    assert.equal(entries.length, 197);
    adapter.dispose();
  });
  test("abort stops a pending trusted transform promptly and wipes its late output", async () => {
    const f = await setup();
    f.options.mapGoal = false;
    let ready, finish;
    const started = new Promise((r) => {
      ready = r;
    });
    f.options.textureReplacements[0].transform = () => {
      ready();
      return new Promise((r) => {
        finish = r;
      });
    };
    const adapter = createAssetPipelineAdapter(f.options),
      controller = new AbortController();
    const pending = adapter.generateAssets({ ...f.auth, signal: controller.signal });
    await started;
    controller.abort("PRIVATE-CANARY");
    await assert.rejects(pending, /^Error: ASSET_PIPELINE_CANCELLED$/);
    assert.equal(f.closed(), 3);
    assert.ok(!f.calls.includes("encode"));
    const late = new Uint8Array(64).fill(4);
    finish({ rgba: late, width: 4, height: 4 });
    await new Promise((r) => setImmediate(r));
    assert.ok(late.every((v) => v === 0));
  });
  test("pre-abort never authenticates or constructs clients; invalid dimensions stop encoding", async () => {
    const f = await setup();
    await assert.rejects(
      createAssetPipelineAdapter(f.options).generateAssets({
        ...f.auth,
        signal: AbortSignal.abort(),
      }),
      /CANCELLED/,
    );
    assert.deepEqual(f.calls, []);
    f.options.mapGoal = false;
    f.options.textureReplacements[0].transform = () => ({
      rgba: new Uint8Array(1),
      width: 9000,
      height: 1,
    });
    await assert.rejects(
      createAssetPipelineAdapter(f.options).generateAssets(f.auth),
      /^Error: ASSET_PIPELINE_FAILED$/,
    );
    assert.ok(!f.calls.includes("encode"));
  });

  test("custom worker Buffer map-goal outputs are snapshotted before scratch cleanup", async () => {
    const f = await setup();
    const createWorker = f.options.createAssetWorker;
    const returned = [];
    f.options.createAssetWorker = () => {
      const worker = createWorker();
      const run = worker.run;
      worker.run = async (...args) => {
        const result = await run(...args);
        if (args[0] === "map-goal")
          for (const name of Object.keys(result.files)) {
            const bytes = Buffer.from(result.files[name]);
            returned.push(bytes);
            result.files[name] = bytes;
          }
        return result;
      };
      return worker;
    };
    const adapter = createAssetPipelineAdapter(f.options);
    const entries = await adapter.generateAssets(f.auth);
    assert.equal(returned.length, MAP_GOAL_OUTPUTS.length);
    for (const buffer of returned) assert.ok(buffer.every((value) => value === 0));
    const mapEntries = entries.filter((entry) => entry.path.startsWith("mapgoal/"));
    for (const entry of mapEntries) assert.deepEqual([...(await bytes(entry))], [1, 2]);
    const { value: chunk } = await mapEntries[0].chunks().next();
    adapter.dispose();
    assert.deepEqual([...chunk], [1, 2]);
  });
}

// asset-text-validation
{
  // 验证 asset-text-validation 的约定与边界；依据 ../toolchain/art-recipes-js/util.mjs、../toolchain/asset-transforms-js/transforms.mjs 及本文件的测试断言。
  // Verify asset-text-validation contracts and boundaries against ../toolchain/art-recipes-js/util.mjs、../toolchain/asset-transforms-js/transforms.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { utf8 } = utilModule;
  const { executeAssetTransform, VAULT_PATHS } = transformsModule;

  // 监测逐字符数组扩张，不测量 RSS 或改变字符串编码语义。
  // Detect character-array expansion without measuring RSS or changing encoding semantics.
  test("art and table UTF-8 checks avoid character arrays and preserve scalar validation", async () => {
    const original = Array.from;
    Array.from = function (value, ...args) {
      assert.notEqual(typeof value, "string", "must not expand strings into character arrays");
      return original.call(this, value, ...args);
    };
    try {
      for (const text of ["", "plain", "中文 😀\t\n", "x".repeat(2 * 1024 * 1024)])
        assert.deepEqual(utf8(text), new TextEncoder().encode(text));
      for (const text of ["\ud800", "\udfff", "x\ud800y"])
        assert.throws(() => utf8(text), /ART_UTF8/);
      assert.throws(() => utf8(null), /ART_UTF8/);
      const assets = Object.fromEntries(VAULT_PATHS.map((path) => [path, ""]));
      for (const [manifest, expected] of [
        ["x".repeat(4 * 1024 * 1024 + 1), /VAULT_MANIFEST_LIMIT/],
        ["\ud800", /ASSET_UTF8/],
      ]) {
        const payload = new TextEncoder().encode(JSON.stringify({ assets, manifest }));
        await assert.rejects(executeAssetTransform("vault-table", payload), expected);
      }
    } finally {
      Array.from = original;
    }
  });

  // 在下一验证阶段停止，以隔离长度检查；不需要图像后端或真实资源。
  // Stop at the next validation stage to isolate length checks without image backends or real assets.
  test("recipe label and path limits count Unicode without expanding strings", async () => {
    const { generateMapGoal, LABELS } = await import("../toolchain/art-recipes-js/map-goal.mjs");
    const { generateNativeArt } = await import("../toolchain/art-recipes-js/native-art.mjs");
    const encode = (value) => new TextEncoder().encode(JSON.stringify(value));
    const original = Array.from;
    Array.from = function (value, ...args) {
      assert.notEqual(typeof value, "string", "must not expand strings into character arrays");
      return original.call(this, value, ...args);
    };
    try {
      for (const value of ["a".repeat(256), "😀".repeat(256), "a😀".repeat(128), "\ud800"])
        checkLabel(value, /MAP_GOAL_DIMENSIONS/);
      for (const value of ["", "a".repeat(257), "😀".repeat(257), "x\n", "a".repeat(1024 * 1024)])
        checkLabel(value, /MAP_GOAL_LABEL_LIMIT/);
      function checkLabel(value, expected) {
        const tables = {};
        for (const [table, Key] of LABELS)
          (tables[table] ??= []).push({
            Key,
            zhCN: "a",
            enUS: "a",
            esES: "a",
            esMX: "a",
            ruRU: "a",
            koKR: "가",
          });
        // 只替换首个标签，保持超长用例仍在表格输入预算之内。
        // Replace only the first label to keep oversized cases within the table input budget.
        tables[LABELS[0][0]][0].zhCN = value;
        const strings = Object.fromEntries(
          Object.entries(tables).map(([name, rows]) => [name, JSON.stringify(rows)]),
        );
        assert.throws(
          () =>
            generateMapGoal(
              encode({ images: { symbols: {}, units: {} }, font: "", strings }),
              { names: "current" },
              {},
            ),
          expected,
        );
      }
      for (const value of ["a".repeat(1024), "😀".repeat(1024), "a😀".repeat(512), "\ud800"])
        await executeAssetTransform("overlay-paths", encode([value]));
      for (const value of ["a".repeat(1025), "😀".repeat(1025), "a".repeat(1024 * 1024)])
        await assert.rejects(
          executeAssetTransform("overlay-paths", encode([value])),
          /Unsafe logical path/,
        );
      for (const [count, expected] of [
        [113, /NATIVE_ART_TABLE_SET/],
        [114, /NATIVE_ART_OUTPUT_NAME/],
        [1024 * 1024, /NATIVE_ART_OUTPUT_NAME/],
      ]) {
        const name = "dm_native_" + "😀".repeat(count) + ".png";
        // The large-name fixture uses ASCII so its manifest remains below the 2 MiB cap.
        // 超长名称用 ASCII，确保清单仍低于 2 MiB 上限。
        const assets = [
          { asset: count > 1000 ? "dm_native_" + "a".repeat(count) + ".png" : name },
          ...Array.from({ length: 8 }, (_, i) => ({ asset: `dm_native_${i}.png` })),
        ];
        const metadata = {
          manifest: JSON.stringify({ assets }),
          geometry: "",
          binding: "",
          implementation: "",
        };
        await assert.rejects(
          generateNativeArt(
            encode({ metadata }),
            {
              metadataSha256: Object.fromEntries(
                Object.keys(metadata).map((key) => [key, "synthetic"]),
              ),
            },
            { sha256: async () => "synthetic" },
          ),
          expected,
        );
      }
    } finally {
      Array.from = original;
    }
  });

  test("derived strings retain control-character rejection without character arrays", async () => {
    const { createHash } = await import("node:crypto");
    const sourcesFor = (name) => ({
      "dm_hud_exp.h":
        "static const uint32_t k_exp[100] = {" +
        Array.from({ length: 100 }, (_, i) => i).join(",") +
        "};\n",
      "dm_vault_tab.h":
        '#define VT_NPART 1\nstatic const char *const vt_part_name[VT_NPART] = { "' +
        name +
        '" };\nstatic const vt_row vt_rows[] = {\n    { 1, 0, 2, 5, 17, -1, "Name", "Path" },\n};\nstatic const char *const vt_set_key[] = { "Set" };\n',
      "dm_touch_native_art.c":
        "static const struct { uint32_t id; int atlas,frame; } rows[]={\n {12,1,2},\n};\n",
    });
    const original = Array.from;
    Array.from = function (value, ...args) {
      assert.notEqual(typeof value, "string", "must not expand strings into character arrays");
      return original.call(this, value, ...args);
    };
    try {
      for (const [name, error] of [
        ["中文 😀", null],
        ["bad\\tname", /DERIVED_CONTROL_CHARACTER/],
        ["bad\\177name", /DERIVED_CONTROL_CHARACTER/],
        ["x".repeat(100000), /DERIVED_RUNTIME_LINE_LIMIT/],
      ]) {
        const sources = sourcesFor(name);
        const result = executeAssetTransform(
          "derived-tables",
          new TextEncoder().encode(JSON.stringify(sources)),
          {
            mode: "generated",
            expectedSourceSha256: Object.fromEntries(
              Object.entries(sources).map(([key, text]) => [
                key,
                createHash("sha256").update(text).digest("hex"),
              ]),
            ),
          },
        );
        if (error) await assert.rejects(result, error);
        else assert.ok((await result).files["derived.tsv"].length > 0);
      }
    } finally {
      Array.from = original;
    }
  });
}

// font-rasterizer-input
{
  // 模拟字体 ABI 仅验证 JS 输入边界；不代表真实字体或 WASM 光栅化验证。
  // Mock font ABI tests cover JS input boundaries, not real fonts or WASM rasterization.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createFontRasterizer } = fontModule;

  async function fixture(t) {
    const memory = { buffer: new ArrayBuffer(67108864) },
      counts = [];
    const module = new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
    t.mock.method(WebAssembly, "instantiate", async () => ({
      exports: {
        memory,
        font_version: () => 20600,
        font_close() {},
        font_input: () => 8192,
        font_open: () => 0,
        font_text: () => 0,
        font_info: () => 16384,
        font_measure(count) {
          counts.push(count);
          return 0;
        },
        font_ft_error: () => 0,
      },
    }));
    const font = await createFontRasterizer(module);
    font.open(new Uint8Array([1]), 48);
    return { font, counts, memory };
  }

  test("oversized font text is rejected before codepoint-array allocation", async (t) => {
    const { font, counts } = await fixture(t);
    const text = "a".repeat(2 * 1024 * 1024);
    const original = Array.from;
    let expanded = false;
    t.mock.method(Array, "from", function (value, ...args) {
      if (value === text) {
        expanded = true;
        throw Error("oversized allocation attempted");
      }
      return original.call(this, value, ...args);
    });
    assert.throws(() => font.measure(text), /Single-line Unicode text limit/);
    assert.equal(expanded, false);
    assert.deepEqual(counts, []);
  });

  test("font limit counts Unicode codepoints, not UTF-16 code units", async (t) => {
    const { font, counts, memory } = await fixture(t);
    font.measure("😀".repeat(2048));
    assert.deepEqual(counts, [2048]);
    assert.ok(new Uint32Array(memory.buffer, 0, 2048).every((point) => point === 0x1f600));
    for (const text of ["a".repeat(2049), "😀".repeat(2049), "bad\nline", "\ud800"]) {
      assert.throws(() => font.measure(text), /Single-line Unicode text limit/);
    }
    assert.deepEqual(counts, [2048]);
    font.close();
    assert.throws(() => font.measure("a"), /Open a font/);
  });
}

// hud-generator
{
  // 验证 hud-generator 的约定与边界；依据 ../hud-generator.mjs、本测试套件内的内存输出缓存夹具 及本文件的测试断言。
  // Verify hud-generator contracts and boundaries against ../hud-generator.mjs、the suite-local memory output cache fixture and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { generateHud, HUD_LOGICAL_INPUTS } = hudGeneratorModule;
  const { memoryOutputCache, assetCacheContext } = memoryOutputCacheModule;
  const { RELEASE_ART_REVISION, UPSTREAM_ART_REVISION } = revisionModule;
  const sha = (b) => createHash("sha256").update(b).digest("hex");
  function fixture() {
    const png = new Uint8Array([1, 2, 3]);
    const assets = Array.from({ length: 9 }, (_, i) => ({
      asset: `dm_native_skill_${i}.png`,
      atlas_px: [1, 1],
      decoded_png_sha256: sha(png),
    }));
    const metadata = { manifest: JSON.stringify({ assets }), i18n: "authored", patch: "{}" };
    const reads = [];
    return {
      reads,
      mode: "switch-upscaled",
      metadata,
      metadataSha256: Object.fromEntries(Object.entries(metadata).map(([k, v]) => [k, sha(v)])),
      nativeArtFiles: Object.fromEntries(assets.map((a) => [a.asset, png.slice()])),
      logicalAssets: {
        async read(path, options) {
          reads.push({ path, options });
          return new TextEncoder().encode(path.endsWith(".json") ? "{}" : "authored bytes");
        },
      },
      decodeAtlas: async () => ({
        width: 1,
        height: 1,
        pixelFormat: 70,
        bytesPerPixel: 4,
        pixels: new Uint8Array([1, 2, 3, 255]),
      }),
      assetWorker: {
        async run(op, payload, options) {
          assert.equal(op, "hud-v4");
          assert.equal(options.mode, "switch-upscaled");
          assert.equal(options.revision, overlayRevisionModule.RELEASE_OVERLAY_REVISION);
          const bundle = JSON.parse(new TextDecoder().decode(payload));
          assert.deepEqual(Object.keys(bundle.fonts).sort(), [
            "blizzardglobal-v5_81.ttf",
            "exocetblizzardot-medium.otf",
          ]);
          return {
            files: {
              "base.png": new ArrayBuffer(1),
              "hud.json": new ArrayBuffer(1),
              "hud_v4_ui/ui.json": new ArrayBuffer(1),
            },
          };
        },
      },
    };
  }
  test("complete HUD obtains both fonts from exact logical archive paths", async () => {
    const f = fixture();
    await generateHud(f);
    assert.deepEqual(
      f.reads.map((r) => r.path),
      HUD_LOGICAL_INPUTS,
    );
    assert.ok(f.reads.every((r) => r.options.maxBytes <= 16 * 1024 * 1024));
    assert.deepEqual(f.nativeArtFiles["dm_native_skill_0.png"], new Uint8Array([1, 2, 3]));
  });
  test("HUD rejects undeclared PC mode, wrong metadata and prior-stage atlas corruption", async () => {
    let f = fixture();
    f.mode = "pc";
    await assert.rejects(generateHud(f), /MODE_REQUIRED/);
    assert.equal(f.reads.length, 0);
    f = fixture();
    f.metadata.patch = "changed";
    await assert.rejects(generateHud(f), /METADATA_HASH/);
    f = fixture();
    f.nativeArtFiles["dm_native_skill_3.png"][0] = 5;
    await assert.rejects(generateHud(f), /ATLAS_HASH/);
  });
  test("HUD propagates missing archive font instead of substituting an OS font", async () => {
    const f = fixture();
    const read = f.logicalAssets.read;
    f.logicalAssets.read = async (path, options) => {
      if (path.endsWith(".otf")) throw new Error("font absent");
      return read(path, options);
    };
    await assert.rejects(generateHud(f), /font absent/);
  });

  test("HUD forwards only a recognized explicit internal source revision", async () => {
    for (const revision of [RELEASE_ART_REVISION, UPSTREAM_ART_REVISION]) {
      const f = fixture();
      f.recipeRevision = revision;
      const run = f.assetWorker.run;
      f.assetWorker.run = async (operation, payload, options) => {
        assert.equal(options.revision, revision);
        return run(operation, payload, { ...options, revision: RELEASE_ART_REVISION });
      };
      await generateHud(f);
    }
    const invalid = fixture();
    invalid.recipeRevision = "v2";
    await assert.rejects(generateHud(invalid), /RECIPE_REVISION/);
    assert.equal(invalid.reads.length, 0);
  });

  test("HUD reuses encoded rendering, but changed font bytes require fresh rendering", async () => {
    const f = fixture();
    let calls = 0;
    const run = f.assetWorker.run;
    f.assetWorker.run = (...args) => {
      calls++;
      return run(...args);
    };
    const options = { ...f, outputCache: memoryOutputCache(), cacheContext: assetCacheContext };
    await generateHud(options);
    await generateHud(options);
    assert.equal(calls, 1);
    assert.equal(f.reads.length, HUD_LOGICAL_INPUTS.length * 2);
    const read = f.logicalAssets.read;
    f.logicalAssets.read = (path, request) =>
      path.endsWith(".otf")
        ? new TextEncoder().encode("changed synthetic font")
        : read(path, request);
    await generateHud(options);
    assert.equal(calls, 2);
  });
}

// logical-asset-suffixes
{
  // 合成操作计数不是浏览器耗时或实际游戏测试。
  // Synthetic operation counts are not browser timing or real-game tests.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const {
    findUniqueLogicalPathSuffixes: batch,
    resolveLogicalAssetSuffixes: resolve,
    LOGICAL_SUFFIX_BATCH_LIMITS: LIMITS,
  } = logicalAssetSuffixesModule;
  const { generateRemainingArt } = remainingArtGeneratorModule;
  const { generateMercenaryArt } = mercenaryArtGeneratorModule;
  function logical(paths, duplicates = []) {
    const rows = paths.map((path) => Object.freeze([...Array(9), path]));
    const duplicate = new Set(duplicates);
    return {
      entries: () => rows.values(),
      hasDuplicatePath: (path) => duplicate.has(path.toLowerCase()),
    };
  }
  function singular(source, suffix) {
    let match;
    for (const row of source.entries()) {
      if (!row[9].toLowerCase().endsWith(suffix)) continue;
      if (match !== undefined || source.hasDuplicatePath(row[9]))
        throw Error("EXTRACTION_ASSET_AMBIGUOUS");
      match = row[9];
    }
    if (match === undefined) throw Error("EXTRACTION_ASSET_MISSING");
    return match;
  }
  test("batch preserves original mixed-case paths, suffix order and repeated requests", () => {
    const source = logical([
      "data:DATA/a/shared.sprite",
      "data:data/b/shared.sprite",
      "data:data/A/Other.sprite",
    ]);
    const wanted = ["/b/shared.sprite", "/a/other.sprite", "/a/shared.sprite", "/b/shared.sprite"];
    const result = batch(source, wanted);
    assert.deepEqual(
      result,
      wanted.map((suffix) => singular(source, suffix)),
    );
    assert.equal(result[2], "data:DATA/a/shared.sprite");
    assert.ok(Object.isFrozen(result));
  });
  test("missing and ambiguity follow request order, including normalized duplicate rows", () => {
    for (const source of [
      logical(["data:data/x/a.sprite", "data:data/y/a.sprite"]),
      logical(["data:DATA/x/a.sprite"], ["data:data/x/a.sprite"]),
    ]) {
      for (const wanted of [
        ["/missing.sprite", "/a.sprite"],
        ["/a.sprite", "/missing.sprite"],
        ["/a.sprite", "/a.sprite"],
      ]) {
        let expected;
        try {
          wanted.map((suffix) => singular(source, suffix));
        } catch (error) {
          expected = error.message;
        }
        assert.throws(() => batch(source, wanted), { message: expected });
      }
    }
  });
  test("request count and ASCII byte bounds reject before scanning; empty batch does not scan", () => {
    const source = {
      entries() {
        throw Error("must not scan");
      },
    };
    assert.deepEqual(batch(source, []), []);
    for (const wanted of [
      new Array(LIMITS.requests + 1).fill("/a"),
      new Array(2049).fill("/" + "a".repeat(511)),
    ])
      assert.throws(() => batch(source, wanted), /SUFFIX_LIMIT/);
    for (const wanted of [null, ["/../a"], ["/A"], ["/a\\b"], ["/" + "a".repeat(512)]])
      assert.throws(() => batch(source, wanted), /SUFFIX/);
  });
  test("cancellation checked on each scanned row and every call starts a new scan", () => {
    const abort = new Error("cancelled");
    let visited = 0,
      closed = false;
    const source = {
      entries: function* () {
        try {
          for (let i = 0; i < 10; i++) {
            visited++;
            yield [...Array(9), `data:data/${i}.sprite`];
          }
        } finally {
          closed = true;
        }
      },
      hasDuplicatePath: () => false,
    };
    assert.throws(
      () =>
        batch(source, ["/9.sprite"], () => {
          if (visited === 3) throw abort;
        }),
      (error) => error === abort,
    );
    assert.equal(visited, 3);
    assert.equal(closed, true);
    visited = 0;
    batch(source, ["/9.sprite"]);
    batch(source, ["/9.sprite"]);
    assert.equal(visited, 20);
  });
  test("singular-only test adapters remain supported in request order", () => {
    const calls = [];
    const adapter = {
      findUniquePathSuffix: (suffix) => {
        calls.push(suffix);
        return `original${suffix}`;
      },
    };
    assert.deepEqual(resolve(adapter, ["/b", "/a", "/b"]), [
      "original/b",
      "original/a",
      "original/b",
    ]);
    assert.deepEqual(calls, ["/b", "/a", "/b"]);
  });
  test("200k-row synthetic batch cuts 64 scans/lowercase passes to one", () => {
    const count = 200000,
      wanted = Array.from({ length: 64 }, (_, i) => `/hd/shaders/pack${i}.bin`);
    const paths = Array.from({ length: count }, (_, i) =>
      i < 64 ? `data:data/hd/shaders/pack${i}.bin` : `data:data/other/item${i}.sprite`,
    );
    const source = logical(paths);
    source.hasDuplicatePath = () => false;
    const lower = String.prototype.toLowerCase,
      ends = String.prototype.endsWith;
    let lowerCalls = 0,
      comparisons = 0;
    // 同步临时计数后立即还原，不跨异步边界或改变字符串结果。
    // Restore synchronous instrumentation immediately; never cross async boundaries or change string results.
    try {
      String.prototype.toLowerCase = function () {
        lowerCalls++;
        return lower.call(this);
      };
      String.prototype.endsWith = function (...args) {
        comparisons++;
        return ends.apply(this, args);
      };
      const expected = wanted.map((suffix) => singular(source, suffix));
      assert.equal(lowerCalls, count * 64);
      assert.equal(comparisons, count * 64);
      lowerCalls = comparisons = 0;
      const actual = batch(source, wanted);
      assert.equal(lowerCalls, count);
      assert.equal(comparisons, 64);
      assert.deepEqual(actual, expected);
    } finally {
      String.prototype.toLowerCase = lower;
      String.prototype.endsWith = ends;
    }
  });
  for (const [name, generate, expectedCount] of [
    ["remaining", generateRemainingArt, 9],
    ["mercenary", generateMercenaryArt, 3],
  ])
    test(`${name} art batches known queries before reads and preserves original lookup result`, async () => {
      let calls = 0;
      const sentinel = Error("stop before decoding");
      const logicalAssets = {
        findUniquePathSuffixes(suffixes) {
          calls++;
          assert.equal(suffixes.length, expectedCount);
          return suffixes.map((suffix) => `Original${suffix}`);
        },
        async read(path) {
          assert.ok(path.startsWith("Original/"));
          throw sentinel;
        },
      };
      await assert.rejects(generate({ logicalAssets }), (error) => error === sentinel);
      assert.equal(calls, 1);
    });
}

// map-goal-generator
{
  // 验证 map-goal-generator 的约定与边界；依据 ../map-goal-generator.mjs、../toolchain/art-recipes-js/revision.mjs 及本文件的测试断言。
  // Verify map-goal-generator contracts and boundaries against ../map-goal-generator.mjs、../toolchain/art-recipes-js/revision.mjs and this file's assertions.
  const { test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { generateMapGoal, MAP_GOAL_OUTPUTS, UPSTREAM_MAP_GOAL_OUTPUTS, MAP_GOAL_FONT } =
    mapGoalGeneratorModule;
  const { UPSTREAM_ART_REVISION } = revisionModule;
  function fixture() {
    const reads = [],
      buffers = [];
    const logicalAssets = {
      async read(path, { maxBytes }) {
        reads.push([path, maxBytes]);
        const bytes = new TextEncoder().encode(path.endsWith(".json") ? "[]" : "fixture");
        buffers.push(bytes);
        return bytes;
      },
    };
    let n = 0;
    const decodeAtlas = async () => {
      const frames = 7 + n++;
      const pixels = new Uint8Array(frames * 4 * 3 * 4);
      buffers.push(pixels);
      return {
        width: frames * 4,
        height: 3,
        frameWidth: 3,
        frames,
        pixels,
        pixelFormat: 70,
        bytesPerPixel: 4,
      };
    };
    const assetWorker = {
      async run(op, payload, options) {
        assert.equal(op, "map-goal");
        assert.deepEqual(options, {
          names: "current",
          revision: overlayRevisionModule.RELEASE_OVERLAY_REVISION,
        });
        const bundle = JSON.parse(new TextDecoder().decode(payload));
        assert.equal(Object.keys(bundle.strings).length, 5);
        return { files: Object.fromEntries(MAP_GOAL_OUTPUTS.map((n) => [n, new Uint8Array([1])])) };
      },
    };
    return { logicalAssets, decodeAtlas, assetWorker, reads, buffers };
  }
  test("Map-goal maps exactly eight archive reads and all 195 current outputs", async () => {
    const f = fixture();
    const result = await generateMapGoal(f);
    assert.equal(Object.keys(result.files).length, 195);
    assert.equal(f.reads.length, 8);
    assert.ok(f.reads.some(([p]) => p === MAP_GOAL_FONT));
    for (const b of f.buffers) assert.ok(b.every((x) => x === 0));
  });
  test("Map-goal does not infer invalid frame layout", async () => {
    const f = fixture();
    f.decodeAtlas = async () => ({
      width: 10,
      height: 1,
      frames: 7,
      frameWidth: 1,
      pixels: new Uint8Array(40),
      pixelFormat: 70,
      bytesPerPixel: 4,
    });
    await assert.rejects(generateMapGoal(f), /MAP_GOAL_FRAME_LAYOUT/);
  });
  test("Map-goal rejects incomplete generator output", async () => {
    const f = fixture();
    f.assetWorker.run = async () => ({ files: { "mapgoal.json": new Uint8Array([1]) } });
    await assert.rejects(generateMapGoal(f), /MAP_GOAL_OUTPUT_SET/);
  });
  test("Map-goal missing archive font remains an error, never OS fallback", async () => {
    const f = fixture();
    const read = f.logicalAssets.read;
    f.logicalAssets.read = async (p, o) => {
      if (p === MAP_GOAL_FONT) throw Error("font missing");
      return read(p, o);
    };
    await assert.rejects(generateMapGoal(f), /font missing/);
    for (const b of f.buffers) assert.ok(b.every((x) => x === 0));
  });
  test("Map-goal cancellation prevents any read", async () => {
    const f = fixture();
    await assert.rejects(
      generateMapGoal({ ...f, signal: AbortSignal.abort() }),
      /MAP_GOAL_CANCELLED/,
    );
    assert.equal(f.reads.length, 0);
  });

  test("Map-goal owns late decoded pixels before cancellation", async () => {
    for (const arrayBuffer of [false, true]) {
      const f = fixture(),
        controller = new AbortController();
      const pixels = new Uint8Array([9]);
      f.decodeAtlas = async () => {
        controller.abort();
        return { pixels: arrayBuffer ? pixels.buffer : pixels };
      };
      await assert.rejects(
        generateMapGoal({ ...f, signal: controller.signal }),
        /MAP_GOAL_CANCELLED/,
      );
      assert.deepEqual(pixels, new Uint8Array(1));
      for (const bytes of f.buffers) assert.ok(bytes.every((byte) => byte === 0));
    }
  });
  test("Map-goal wipes all returned files on cancellation or rejected output", async () => {
    for (const failure of ["cancel", "missing", "invalid", "limit"]) {
      const f = fixture(),
        controller = new AbortController();
      const data = new Uint8Array([7]),
        other = new Uint8Array([8]);
      const files = Object.fromEntries(MAP_GOAL_OUTPUTS.map((name) => [name, new Uint8Array([1])]));
      files[MAP_GOAL_OUTPUTS[0]] = data;
      files[MAP_GOAL_OUTPUTS.at(-1)] = other.buffer;
      if (failure === "missing") delete files[MAP_GOAL_OUTPUTS[1]];
      if (failure === "invalid") files[MAP_GOAL_OUTPUTS[1]] = "not bytes";
      if (failure === "limit")
        files[MAP_GOAL_OUTPUTS[1]] = new Uint8Array(16 * 1024 * 1024 + 1).fill(2);
      f.assetWorker.run = async () => {
        if (failure === "cancel") controller.abort();
        return { files };
      };
      await assert.rejects(
        generateMapGoal({ ...f, signal: controller.signal }),
        /MAP_GOAL_(CANCELLED|OUTPUT_SET|OUTPUT_LIMIT)/,
      );
      for (const value of Object.values(files)) {
        const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) : value;
        if (bytes instanceof Uint8Array) assert.ok(bytes.every((byte) => byte === 0));
      }
      assert.deepEqual(data, new Uint8Array(1));
      assert.deepEqual(other, new Uint8Array(1));
    }
  });
  test("Map-goal leaves successful worker output ownership with the caller", async () => {
    const f = fixture();
    const files = Object.fromEntries(MAP_GOAL_OUTPUTS.map((name) => [name, new Uint8Array([7])]));
    files[MAP_GOAL_OUTPUTS[0]] = new Uint8Array([8]).buffer;
    f.assetWorker.run = async () => ({ files });
    const result = await generateMapGoal(f);
    assert.equal(result.files, files);
    for (const value of Object.values(files)) {
      const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) : value;
      assert.ok(bytes.every((byte) => byte !== 0));
    }
  });

  test("Map-goal releases input snapshots before worker execution without wiping borrowed originals", async () => {
    const f = fixture(),
      original = new Uint8Array([4, 5, 6]);
    const read = f.logicalAssets.read;
    f.logicalAssets.read = async (path, options) => {
      for (const bytes of f.buffers) assert.ok(bytes.every((byte) => byte === 0));
      if (path === MAP_GOAL_FONT) {
        const copy = original.slice();
        f.buffers.push(copy);
        return copy;
      }
      return read(path, options);
    };
    let request;
    const run = f.assetWorker.run;
    f.assetWorker.run = async (operation, payload, options) => {
      request = payload;
      for (const bytes of f.buffers) assert.ok(bytes.every((byte) => byte === 0));
      assert.ok(payload.some((byte) => byte !== 0));
      await Promise.resolve();
      assert.ok(payload.some((byte) => byte !== 0));
      return run(operation, payload, options);
    };
    await generateMapGoal(f);
    assert.ok(request.every((byte) => byte === 0));
    assert.deepEqual(original, new Uint8Array([4, 5, 6]));
  });

  test("Map-goal enables the 195-file inventory only with an explicit internal revision", async () => {
    const f = fixture();
    f.recipeRevision = UPSTREAM_ART_REVISION;
    f.assetWorker.run = async (_op, _payload, options) => {
      assert.deepEqual(options, { names: "current", revision: UPSTREAM_ART_REVISION });
      return {
        files: Object.fromEntries(
          UPSTREAM_MAP_GOAL_OUTPUTS.map((name) => [name, new Uint8Array([1])]),
        ),
      };
    };
    assert.equal(Object.keys((await generateMapGoal(f)).files).length, 195);
    const invalid = fixture();
    invalid.recipeRevision = "v2";
    await assert.rejects(generateMapGoal(invalid), /RECIPE_REVISION/);
    assert.equal(invalid.reads.length, 0);
  });
}

// map-goal-recipe-budget
{
  // 合成后端检查聚合预算与输出顺序，不使用游戏资源或真实字体。
  // Synthetic backends check aggregate budgets and output order, without game assets or real fonts.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { generateMapGoal, LABELS } = mapGoalModule;
  const { UPSTREAM_MAP_GOAL_OUTPUTS } = mapGoalGeneratorModule;
  const { RELEASE_ART_REVISION, UPSTREAM_ART_REVISION } = revisionModule;

  function fixture(outputBytes, mutateStrings = () => {}, revision = UPSTREAM_ART_REVISION) {
    const strings = {};
    for (const [table, key] of LABELS) {
      (strings[table] ??= []).push({
        Key: key,
        zhCN: "a",
        enUS: "a",
        esES: "a",
        esMX: "a",
        ruRU: "a",
        koKR: "가",
      });
    }
    mutateStrings(strings);
    const image = (frames) => ({
      size: [frames, 1],
      frameWidth: 1,
      frames,
      rgba: btoa("\0".repeat(frames * 4)),
    });
    const payload = new TextEncoder().encode(
      JSON.stringify({
        images: { symbols: image(7), units: image(8) },
        font: btoa("synthetic"),
        strings: Object.fromEntries(
          Object.entries(strings).map(([key, rows]) => [key, JSON.stringify(rows)]),
        ),
      }),
    );
    let encoded = 0,
      closed = 0;
    const dependencies = {
      codec: {
        encodeRgba() {
          ++encoded;
          return typeof outputBytes === "function" ? outputBytes(encoded) : outputBytes;
        },
      },
      kernels: {
        crop: () => ({ width: 1, height: 1, data: new Uint8Array(4) }),
        channel: (image) => image,
        point: (image) => image,
        putAlpha: (image) => image,
      },
      font: {
        open() {},
        close() {
          ++closed;
        },
        measure: () => ({ length: 1 }),
        draw: (bytes) => bytes,
      },
    };
    return {
      run: () =>
        generateMapGoal(
          payload,
          { names: "current", ...(revision ? { revision } : {}) },
          dependencies,
        ),
      get encoded() {
        return encoded;
      },
      get closed() {
        return closed;
      },
    };
  }

  test("map-goal stops encoding at the first aggregate output overflow", () => {
    const bytes = new Uint8Array(1024 * 1024).fill(9);
    const f = fixture(bytes);
    assert.throws(f.run, /MAP_GOAL_OUTPUT_LIMIT/);
    assert.equal(f.encoded, 17);
    assert.ok(bytes.every((value) => value === 0));
    assert.equal(f.closed, 1);
  });

  test("map-goal rejects one oversized image before encoding another", () => {
    const bytes = new Uint8Array(16 * 1024 * 1024 + 1).fill(9);
    const f = fixture(bytes);
    assert.throws(f.run, /MAP_GOAL_OUTPUT_LIMIT/);
    assert.equal(f.encoded, 1);
    assert.ok(bytes.every((value) => value === 0));
    assert.equal(f.closed, 1);
  });

  test("in-budget map-goal keeps the complete output set and recipe ordering", () => {
    const f = fixture(new Uint8Array([1]));
    const result = f.run();
    const names = Object.keys(result.files);
    assert.deepEqual([...names].sort(), [...UPSTREAM_MAP_GOAL_OUTPUTS].sort());
    assert.deepEqual(names.slice(0, 8), [
      ...Array.from({ length: 7 }, (_, i) => `sym_${i}.png`),
      "cross.png",
    ]);
    assert.deepEqual(names.slice(8, 14), [
      "label_0.png",
      "label_0_en.png",
      "label_0_es.png",
      "label_0_mx.png",
      "label_0_ru.png",
      "label_0_ko.png",
    ]);
    assert.equal(names.at(-1), "mapgoal.json");
    assert.equal(result.files["cross.png"][0], 1);
    const manifest = JSON.parse(new TextDecoder().decode(result.files["mapgoal.json"]));
    assert.equal(manifest.labels.length, 31);
    assert.ok(manifest.labels.every((label) => label.koKR === "가"));
    assert.equal(f.encoded, 194);
    assert.equal(f.closed, 1);
  });

  test("map-goal requires nonempty bounded Korean labels before rendering", () => {
    for (const value of [undefined, "", "가".repeat(257), "bad\nlabel"]) {
      const f = fixture(new Uint8Array([1]), (strings) => {
        strings.levels[0].koKR = value;
      });
      assert.throws(f.run, /MAP_GOAL_LABEL_LIMIT/);
      assert.equal(f.encoded, 0);
    }
  });

  test("map-goal includes the final JSON manifest in its aggregate budget", () => {
    const first = new Uint8Array(16 * 1024 * 1024 - 193).fill(9);
    const rest = new Uint8Array([9]);
    const f = fixture((index) => (index === 1 ? first : rest));
    assert.throws(f.run, /MAP_GOAL_OUTPUT_LIMIT/);
    assert.equal(f.encoded, 194);
    assert.ok(first.every((value) => value === 0));
    assert.equal(rest[0], 0);
    assert.equal(f.closed, 1);
  });

  test("map-goal defaults use the current release inventory and reject retired revisions", () => {
    const f = fixture(new Uint8Array([1]), () => {}, null);
    const result = f.run();
    assert.deepEqual(result, fixture(new Uint8Array([1]), () => {}, RELEASE_ART_REVISION).run());
    assert.equal(Object.keys(result.files).length, 195);
    assert.equal(f.encoded, 194);
    const manifest = JSON.parse(new TextDecoder().decode(result.files["mapgoal.json"]));
    assert.ok(manifest.labels.every((label) => Object.hasOwn(label, "koKR")));
    assert.throws(() => fixture(new Uint8Array([1]), () => {}, "v1.0.18").run(), /REVISION/);
  });

  test("reviewed b811 map-goal branch matches upstream synthetic output", () => {
    const release = fixture(new Uint8Array([1]), () => {}, RELEASE_ART_REVISION).run();
    const upstream = fixture(new Uint8Array([1])).run();
    assert.deepEqual(release, upstream);
    assert.throws(
      () => generateMapGoal(new Uint8Array(), { names: "current", revision: null }, {}),
      /RECIPE_REVISION/,
    );
  });
}

// mercenary-art-lifetime
{
  // 合成生命周期测试，不构成真实图像或浏览器内存证明。
  // Synthetic lifecycle tests are not real-image or browser-memory proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { generateMercenaryArt, MERCENARY_INPUTS, createMercenaryOwnedBytes } =
    mercenaryArtGeneratorModule;

  test("owned alias release is idempotent and leaves independent copies intact", () => {
    const owned = createMercenaryOwnedBytes(),
      buffer = new Uint8Array([1, 2, 3, 4]);
    const alias = new Uint8Array(buffer.buffer),
      copy = buffer.slice();
    owned.add(buffer);
    owned.add(alias);
    owned.add(copy);
    owned.release(alias);
    assert.equal(owned.size, 1);
    assert.deepEqual(buffer, new Uint8Array(4));
    assert.deepEqual(copy, new Uint8Array([1, 2, 3, 4]));
    owned.release(buffer);
    assert.equal(owned.size, 1);
    owned.dispose();
    owned.dispose();
    assert.deepEqual(copy, new Uint8Array(4));
  });
  function fixture(failWorker = false) {
    const raw = [],
      decoded = [],
      headers = [];
    let payload;
    const names = Object.keys(MERCENARY_INPUTS);
    const logicalAssets = {
      findUniquePathSuffix(suffix) {
        return `data:data${suffix}`;
      },
      async read(path) {
        for (const value of [...raw, ...decoded]) assert.ok(value.every((byte) => byte === 0));
        const name = names.find((name) => path.endsWith(`/${name}.sprite`));
        const [width, height] = MERCENARY_INPUTS[name];
        const source = new Uint8Array(40),
          view = new DataView(source.buffer);
        view.setUint32(0, 0x31417053, true);
        view.setUint16(4, 61, true);
        view.setUint16(6, 652, true);
        view.setUint32(8, width, true);
        view.setUint32(12, height, true);
        raw.push(source);
        headers.push(source.slice());
        return source;
      },
    };
    return {
      raw,
      decoded,
      headers,
      get payload() {
        return payload;
      },
      options: {
        logicalAssets,
        async decodeAtlas(source) {
          const view = new DataView(source.buffer);
          const width = view.getUint32(8, true),
            height = view.getUint32(12, true);
          const pixels = new Uint8Array(width * height * 4).fill(7);
          decoded.push(pixels);
          return { width, height, pixelFormat: 70, bytesPerPixel: 4, pixels };
        },
        assetWorker: {
          async run(_op, bytes) {
            payload = bytes;
            assert.ok(bytes.some((byte) => byte !== 0));
            const images = JSON.parse(new TextDecoder().decode(bytes));
            assert.equal(Object.keys(images).length, 3);
            if (failWorker) throw new Error("worker failure");
            return {
              width: 652,
              height: 704,
              files: {
                "mercenary.rgba": new Uint8Array(652 * 704 * 4).fill(9),
                "HirelingInventoryBG.png": new Uint8Array([1, 2, 3]),
              },
            };
          },
        },
        textureEncoder: {
          async encodeAndPack({ templateHeader, rgba }) {
            assert.ok(payload.every((byte) => byte === 0));
            assert.deepEqual(templateHeader, headers[0]);
            assert.equal(rgba[0], 9);
            return {
              width: 652,
              height: 704,
              sprite: new Uint8Array(40).fill(3),
              linearBC3: new Uint8Array([5, 6]),
            };
          },
        },
      },
    };
  }
  test("sources release after snapshots, payload after consumption, template and returned copies survive", async () => {
    const f = fixture();
    const result = await generateMercenaryArt(f.options);
    assert.deepEqual(result.files["HirelingInventoryBG.png"], new Uint8Array([1, 2, 3]));
    assert.ok(result.files["HirelingInventoryBG.sprite"].every((byte) => byte === 3));
    for (const value of [...f.raw, ...f.decoded, f.payload])
      assert.ok(value.every((byte) => byte === 0));
  });
  test("worker rejection still releases request and sources", async () => {
    const f = fixture(true);
    await assert.rejects(generateMercenaryArt(f.options), /worker failure/);
    assert.ok(f.payload.every((byte) => byte === 0));
    for (const value of [...f.raw, ...f.decoded]) assert.ok(value.every((byte) => byte === 0));
  });

  test("malformed mercenary worker results wipe every returned byte buffer", async () => {
    for (const invalid of [false, true]) {
      const f = fixture(),
        first = new Uint8Array([4]),
        last = new Uint8Array([5]);
      f.options.assetWorker.run = async () => ({
        files: invalid
          ? { "mercenary.rgba": null, "HirelingInventoryBG.png": last.buffer }
          : { unexpected: first, extra: last.buffer },
      });
      await assert.rejects(generateMercenaryArt(f.options), /MERCENARY_(OUTPUT_SET|BYTES)/);
      assert.deepEqual(last, new Uint8Array(1));
      if (!invalid) assert.deepEqual(first, new Uint8Array(1));
      for (const value of [...f.raw, ...f.decoded]) assert.ok(value.every((byte) => byte === 0));
    }
  });
}

// native-art-generator
{
  // 验证 native-art-generator 的约定与边界；依据 ../native-art-generator.mjs、本测试套件内的内存输出缓存夹具 及本文件的测试断言。
  // Verify native-art-generator contracts and boundaries against ../native-art-generator.mjs、the suite-local memory output cache fixture and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { generateNativeArt } = nativeArtGeneratorModule;
  const { memoryOutputCache, assetCacheContext } = memoryOutputCacheModule;
  const sha = (b) => createHash("sha256").update(b).digest("hex");
  function fixture() {
    const bytes = new Uint8Array([1, 2, 3]);
    const manifest = {
      official_tables: ["skills.txt", "skilldesc.txt"].map((file) => ({
        file,
        sha256: sha(bytes),
      })),
      assets: Array.from({ length: 9 }, (_, i) => ({
        asset: `dm_native_skill_${i}.png`,
        official_path: `data:data/hd/authored/${i}.sprite`,
        official_sprite_sha256: sha(bytes),
        atlas_px: [1, 1],
      })),
    };
    const metadata = {
      manifest: JSON.stringify(manifest),
      geometry: "{}",
      binding: "{}",
      implementation: "/* authored */",
    };
    const metadataSha256 = Object.fromEntries(
      Object.entries(metadata).map(([k, v]) => [k, sha(v)]),
    );
    const reads = [];
    const files = Object.fromEntries(
      [...manifest.assets.map((a) => a.asset), "dm_touch_native_art.c"].map((n) => [
        n,
        new ArrayBuffer(1),
      ]),
    );
    return {
      metadata,
      metadataSha256,
      reads,
      files,
      logicalAssets: {
        async read(path, options) {
          reads.push({ path, ...options });
          return bytes.slice();
        },
      },
      decodeAtlas: async () => ({
        width: 1,
        height: 1,
        pixels: new Uint8Array([1, 2, 3, 4]),
        pixelFormat: 70,
        bytesPerPixel: 4,
      }),
      assetWorker: {
        async run(op, payload, options) {
          assert.equal(op, "native-art");
          const bundle = JSON.parse(new TextDecoder().decode(payload));
          assert.equal(Object.keys(bundle.raw).length, 11);
          assert.equal(Object.keys(bundle.rgba).length, 9);
          assert.deepEqual(options, { metadataSha256 });
          return { files };
        },
      },
    };
  }
  test("native art reads the entire named inventory and emits no partial set", async () => {
    const f = fixture();
    assert.equal((await generateNativeArt(f)).files, f.files);
    assert.equal(f.reads.length, 11);
    assert.ok(f.reads.every((r) => r.maxBytes <= 8 * 1024 * 1024));
  });
  test("native art rejects untrusted metadata before logical reads", async () => {
    const f = fixture();
    f.metadata.binding = "changed";
    await assert.rejects(generateNativeArt(f), /METADATA_HASH/);
    assert.equal(f.reads.length, 0);
  });
  test("native art rejects source hash, output omissions, bad decoded dimensions and cancellation", async () => {
    let f = fixture();
    f.logicalAssets.read = async () => new Uint8Array([0]);
    await assert.rejects(generateNativeArt(f), /SOURCE_HASH/);
    f = fixture();
    delete f.files["dm_native_skill_8.png"];
    await assert.rejects(generateNativeArt(f), /OUTPUT_SET/);
    f = fixture();
    f.decodeAtlas = async () => ({ width: 2, height: 1, pixels: new Uint8Array(8) });
    await assert.rejects(generateNativeArt(f), /DECODE_GEOMETRY/);
    f = fixture();
    const controller = new AbortController();
    controller.abort();
    f.signal = controller.signal;
    await assert.rejects(generateNativeArt(f), /CANCELLED/);
    assert.equal(f.reads.length, 0);
  });

  test("native-art cache hits still authenticate fresh input and invalidate changed decoded content", async () => {
    const f = fixture();
    const run = f.assetWorker.run;
    let calls = 0;
    f.assetWorker.run = (...args) => {
      calls++;
      return run(...args);
    };
    const options = { ...f, outputCache: memoryOutputCache(), cacheContext: assetCacheContext };
    await generateNativeArt(options);
    await generateNativeArt(options);
    assert.equal(calls, 1);
    assert.equal(f.reads.length, 22);
    const decode = options.decodeAtlas;
    options.decodeAtlas = async (...args) => ({
      ...(await decode(...args)),
      pixels: new Uint8Array([4, 3, 2, 1]),
    });
    await generateNativeArt(options);
    assert.equal(calls, 2);
    options.logicalAssets = { read: async () => new Uint8Array([0]) };
    await assert.rejects(generateNativeArt(options), /SOURCE_HASH/);
    assert.equal(calls, 2);
  });
}

// native-art-png-digests
{
  // 验证 native-art-png-digests 的约定与边界；依据 ../native-art-png-digests.mjs 及本文件的测试断言。
  // Verify native-art-png-digests contracts and boundaries against ../native-art-png-digests.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { acceptsNativeArtPngDigest } = nativeArtPngDigestsModule;

  test("browser PNG digest is accepted only with its matching frozen atlas pin", () => {
    const asset = {
      asset: "dm_native_skill_0.png",
      decoded_png_sha256: "15f751a2499a798509ab4746780414527b3405c8afc78b654e34cdadaf868617",
    };
    const browser = "a9e5a63e6eee6cf44cd96e77ad59f6067dc09dcf6d2bcd2bf77200327e6928e9";
    assert.equal(acceptsNativeArtPngDigest(asset, asset.decoded_png_sha256), true);
    assert.equal(acceptsNativeArtPngDigest(asset, browser), true);
    assert.equal(
      acceptsNativeArtPngDigest({ ...asset, decoded_png_sha256: "0".repeat(64) }, browser),
      false,
    );
    assert.equal(acceptsNativeArtPngDigest({ ...asset, asset: "other.png" }, browser), false);
    assert.equal(acceptsNativeArtPngDigest(asset, "0".repeat(64)), false);
  });
}

// native-art-texture
{
  // 验证 native-art-texture 的约定与边界；依据 ../native-art-texture.mjs 及本文件的测试断言。
  // Verify native-art-texture contracts and boundaries against ../native-art-texture.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { nativeArtAtlasDescriptor, NativeArtTextureClient } = nativeArtTextureModule;
  function sprite(width = 136, frameWidth = 65, frames = 2) {
    const bytes = new Uint8Array(56),
      h = new DataView(bytes.buffer);
    h.setUint32(0, 0x31417053, true);
    h.setUint16(4, 61, true);
    h.setUint16(6, frameWidth, true);
    h.setUint32(8, width, true);
    h.setUint32(12, 65, true);
    h.setUint16(20, frames, true);
    return bytes;
  }
  test("whole-atlas retains padded dimensions and accepts a final unpadded frame", () => {
    const a = nativeArtAtlasDescriptor(sprite());
    assert.equal(a.descriptor.width, 136);
    assert.equal(a.frameWidth, 65);
    assert.equal(a.framePitch, 68);
    assert.equal(nativeArtAtlasDescriptor(sprite(155, 155, 1)).descriptor.width, 155);
    const packed = nativeArtAtlasDescriptor(sprite(124, 62, 2));
    assert.equal(packed.descriptor.width, 124);
    assert.equal(packed.framePitch, 62);
    assert.throws(() => nativeArtAtlasDescriptor(sprite(130)), /GEOMETRY/);
    assert.throws(() => nativeArtAtlasDescriptor(sprite(137)), /GEOMETRY/);
    assert.throws(() => nativeArtAtlasDescriptor(sprite(136, 65, 0)), /GEOMETRY/);
  });
  test("whole-atlas worker uses explicit decode descriptor and abort closes pending work", async () => {
    let sent,
      terminated = false;
    const worker = {
      postMessage(message) {
        sent = message;
      },
      terminate() {
        terminated = true;
      },
    };
    const client = new NativeArtTextureClient(worker);
    const controller = new AbortController();
    const pending = client.decodeAtlas(sprite(), { signal: controller.signal });
    assert.equal(sent.operation, "decodePillowBc3");
    assert.equal(sent.descriptor.width, 136);
    assert.equal(sent.source.length, 16);
    controller.abort();
    await assert.rejects(pending, /CANCELLED/);
    assert.equal(terminated, true);
  });
}

// remaining-art-lifetime
{
  // 合成生命周期及别名测试；不测量浏览器内存或真实图像兼容性。
  // Synthetic lifetime and alias tests; no browser memory or real-image compatibility measurement.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { generateRemainingArt, createRemainingArtOwnedBytes } = remainingArtGeneratorModule;

  test("stage release preserves earlier shared buffers and explicit aliases", () => {
    const owned = createRemainingArtOwnedBytes(),
      shared = new Uint8Array([1]),
      temp = new Uint8Array([2]),
      personal = new Uint8Array([3]);
    owned.add(shared);
    const before = owned.snapshot();
    owned.add(new Uint8Array(shared.buffer));
    owned.add(temp);
    owned.add(new Uint8Array(temp.buffer));
    owned.add(personal);
    owned.releaseSince(before, [new Uint8Array(personal.buffer)]);
    assert.equal(shared[0], 1);
    assert.equal(temp[0], 0);
    assert.equal(personal[0], 3);
    owned.release(personal);
    owned.release(personal);
    assert.equal(personal[0], 0);
    owned.dispose();
    assert.equal(shared[0], 0);
  });
  test("actual generator releases intermediates while preserving shared inputs, vault round trip and returned copies", async () => {
    let main, wide, personal, previousRgba, previousLinear;
    const shared = [],
      resultBuffers = [];
    const empty = (bytes) => assert.ok(bytes.every((value) => value === 0));
    const source = (name) => {
      const isMain = name.endsWith("/mainpanelbg.sprite");
      const storage = 181 * 28 * 512;
      const bytes = new Uint8Array(isMain ? storage + 40 : 40).fill(7);
      const view = new DataView(bytes.buffer);
      view.setUint32(0, 0x31417053, true);
      view.setUint16(4, 61, true);
      view.setUint16(6, isMain ? 1440 : 652, true);
      view.setUint32(8, isMain ? 2888 : 652, true);
      view.setUint32(12, isMain ? 881 : 1, true);
      view.setUint16(20, isMain ? 2 : 1, true);
      if (isMain) {
        bytes[23] = 0;
        view.setUint32(32, storage, true);
        main = bytes;
      } else shared.push(bytes);
      return bytes;
    };
    const result = await generateRemainingArt({
      logicalAssets: {
        findUniquePathSuffix: (suffix) => `data:data${suffix}`,
        async read(path) {
          if (main) empty(main);
          return source(path);
        },
      },
      async decodeAtlas(raw) {
        assert.ok(raw.some((value) => value !== 0));
        if (raw.buffer === personal?.buffer) assert.equal(personal[0], 9);
        const width =
          raw.buffer === personal?.buffer ? 652 : new DataView(raw.buffer).getUint32(8, true);
        const pixels = new Uint8Array(width * 4).fill(6);
        shared.push(pixels);
        return { width, height: 1, pixelFormat: 70, bytesPerPixel: 4, pixels };
      },
      assetWorker: {
        async run(_op, payload, { kind, name }) {
          if (previousRgba) empty(previousRgba);
          if (previousLinear) empty(previousLinear);
          if (wide) empty(wide);
          if (kind === "vault") assert.equal(personal[0], 9);
          const inputs = JSON.parse(new TextDecoder().decode(payload));
          assert.ok(Object.keys(inputs).length);
          if (name === "StashPanelBGShared")
            assert.ok(shared.every((bytes) => bytes.some((value) => value !== 0)));
          const rgba = new Uint8Array(4).fill(8);
          previousRgba = rgba;
          resultBuffers.push(rgba);
          return {
            width: 1,
            height: 1,
            files: {
              "image.rgba": rgba.buffer,
              ...(kind === "vault" ? { "VaultBottom.png": new Uint8Array([4, 5]).buffer } : {}),
            },
          };
        },
      },
      textureEncoder: {
        async packLinearBC3(value) {
          wide = value.linearBC3;
          return { sprite: new Uint8Array(40).fill(3) };
        },
        async encodeAndPack({ templateHeader, rgba }) {
          assert.ok(templateHeader.some((value) => value !== 0));
          assert.equal(rgba[0], 8);
          const sprite = new Uint8Array(40).fill(9),
            linearBC3 = new Uint8Array([2]);
          previousLinear = linearBC3;
          if (resultBuffers.length === 2) personal = sprite;
          return { sprite: sprite.buffer, linearBC3: linearBC3.buffer };
        },
      },
    });
    assert.equal(Object.keys(result.files).length, 10);
    for (const bytes of Object.values(result.files)) assert.ok(bytes.some((value) => value !== 0));
    empty(main);
    empty(wide);
    empty(personal);
    empty(previousRgba);
    empty(previousLinear);
    for (const bytes of shared) empty(bytes);
  });

  test("missing or malformed remaining-art RGBA wipes all other worker files", async () => {
    for (const invalid of [false, true]) {
      const first = new Uint8Array([7]),
        last = new Uint8Array([8]);
      const sources = [];
      const options = {
        logicalAssets: {
          findUniquePathSuffix: (suffix) => suffix,
          async read(path) {
            const main = path.endsWith("/mainpanelbg.sprite"),
              storage = 181 * 28 * 512;
            const data = new Uint8Array(main ? storage + 40 : 40),
              view = new DataView(data.buffer);
            view.setUint32(0, 0x31417053, true);
            view.setUint16(4, 61, true);
            view.setUint16(6, main ? 1440 : 652, true);
            view.setUint32(8, main ? 2888 : 652, true);
            view.setUint32(12, main ? 881 : 1, true);
            view.setUint16(20, main ? 2 : 1, true);
            if (main) view.setUint32(32, storage, true);
            sources.push(data);
            return data;
          },
        },
        async decodeAtlas() {
          const pixels = new Uint8Array(652 * 4).fill(6);
          sources.push(pixels);
          return { width: 652, height: 1, pixelFormat: 70, bytesPerPixel: 4, pixels };
        },
        textureEncoder: {
          async packLinearBC3() {
            return { sprite: new Uint8Array(40) };
          },
        },
        assetWorker: {
          async run() {
            return {
              files: {
                "first.png": first,
                ...(invalid ? { "image.rgba": "not bytes" } : {}),
                "VaultBottom.png": last.buffer,
              },
            };
          },
        },
      };
      await assert.rejects(generateRemainingArt(options), /REMAINING_ART_BYTES/);
      assert.deepEqual(first, new Uint8Array(1));
      assert.deepEqual(last, new Uint8Array(1));
      for (const source of sources) assert.ok(source.every((byte) => byte === 0));
    }
  });
}

// texture-encoder-lifetime
{
  // 合成模块只验证所有权与失败清理，不构成原生编码或浏览器证明。
  // Synthetic module checks ownership and failure cleanup, not native codec or browser proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createTextureEncoder } = browserTextureEncoderModule;
  function fixture(failure) {
    const heap = new ArrayBuffer(4096);
    let next = 64,
      allocations = 0;
    const module = {
      HEAPU8: new Uint8Array(heap),
      HEAPU32: new Uint32Array(heap),
      _malloc(size) {
        if (++allocations === 3 && failure === "allocation") return 0;
        const pointer = next;
        next += Math.ceil(size / 8) * 8;
        return pointer;
      },
      _free() {},
      _texture_info(_pointer, _level, _layer, info) {
        if (failure === "layout") return -4;
        module.HEAPU32.set([16, 16, 4, 4, 70, 4, 16, 1], info / 4);
        return 0;
      },
      _texture_pack_bc3(_descriptor, _input, _length, output, size) {
        module.HEAPU8.fill(7, output, output + size);
        return failure === "pack" ? -4 : 0;
      },
    };
    const header = new Uint8Array(40),
      view = new DataView(header.buffer);
    view.setUint32(0, 0x31417053, true);
    view.setUint16(4, 61, true);
    view.setUint16(6, 4, true);
    view.setUint32(8, 4, true);
    view.setUint32(12, 4, true);
    view.setUint16(20, 1, true);
    return { module, header, rgba: new Uint8Array(64).fill(3) };
  }
  test("encoding wipes independent BC3 output when layout, allocation, length or packing fails", async () => {
    for (const failure of ["layout", "allocation", "length", "pack", "type"]) {
      const f = fixture(failure),
        savedHeader = f.header.slice(),
        savedRgba = f.rgba.slice();
      const encoded = new Uint8Array(failure === "length" ? 15 : 16).fill(9);
      const encoder = await createTextureEncoder(
        async () => f.module,
        async () => (failure === "type" ? encoded.buffer : encoded),
      );
      await assert.rejects(encoder.encodeAndPack(f.header, f.rgba, 4, 4));
      assert.ok(encoded.every((byte) => byte === 0));
      assert.deepEqual(f.header, savedHeader);
      assert.deepEqual(f.rgba, savedRgba);
    }
  });
  test("packing failure preserves injected encoder aliases to caller inputs", async () => {
    for (const source of ["header", "rgba"]) {
      const f = fixture("pack"),
        savedHeader = f.header.slice(),
        savedRgba = f.rgba.slice();
      const encoder = await createTextureEncoder(
        async () => f.module,
        async () => f[source].subarray(0, 16),
      );
      await assert.rejects(encoder.encodeAndPack(f.header, f.rgba, 4, 4));
      assert.deepEqual(f.header, savedHeader);
      assert.deepEqual(f.rgba, savedRgba);
    }
  });
  test("successful encoding preserves returned BC3 and sprite and caller inputs", async () => {
    const f = fixture(),
      savedHeader = f.header.slice(),
      savedRgba = f.rgba.slice();
    const encoded = new Uint8Array(16).fill(9);
    const encoder = await createTextureEncoder(
      async () => f.module,
      async () => encoded,
    );
    const result = await encoder.encodeAndPack(f.header, f.rgba, 4, 4);
    assert.equal(result.linearBC3, encoded);
    assert.ok(encoded.every((byte) => byte === 9));
    assert.equal(result.sprite.length, 56);
    assert.ok(result.sprite.subarray(40).every((byte) => byte === 7));
    assert.deepEqual(f.header, savedHeader);
    assert.deepEqual(f.rgba, savedRgba);
  });
}

// v61-asset-worker
{
  // 验证 v61-asset-worker 的约定与边界；依据 ../v61-asset-worker.mjs、../asset-generator-adapter.mjs 及本文件的测试断言。
  // Verify v61-asset-worker contracts and boundaries against ../v61-asset-worker.mjs、../asset-generator-adapter.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createV61AssetWorker, decodedV61ToInternalRgba, TextureWorkerClient } =
    v61AssetWorkerModule;
  const { createAssetGeneratorAdapter } = assetGeneratorAdapterModule;
  function fixture() {
    const input = new Uint8Array(56);
    input.set([83, 112, 65, 49, 61, 0, 1, 0]);
    const view = new DataView(input.buffer);
    view.setUint32(8, 2, true);
    view.setUint32(12, 1, true);
    const decoded = {
      width: 2,
      height: 1,
      frameWidth: 1,
      pixelFormat: 70,
      bytesPerPixel: 4,
      pixels: new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]),
    };
    return { input, decoded };
  }
  test("internal RGBA bridge preserves dimensions/frame pixels without claiming v61 encoding", () => {
    const { input, decoded } = fixture();
    const bytes = decodedV61ToInternalRgba(input, decoded);
    const view = new DataView(bytes.buffer);
    assert.equal(view.getUint16(4, true), 31);
    assert.equal(view.getUint16(6, true), 1);
    assert.equal(view.getUint32(20, true), 2);
    assert.deepEqual(bytes.subarray(40), decoded.pixels);
    assert.throws(() => decodedV61ToInternalRgba(input, { ...decoded, pixelFormat: 1 }));
    assert.throws(() => decodedV61ToInternalRgba(input, { ...decoded, pixels: new Uint8Array(4) }));
  });
  test("composed worker emits PNG only for v61 and erases temporary RGBA", async () => {
    const { input, decoded } = fixture();
    let intermediate,
      closed = 0;
    const worker = createV61AssetWorker({
      textureWorker: { decodeV61: async () => decoded, close: () => closed++ },
      assetWorker: {
        run: async (op, bytes, options) => {
          assert.equal(op, "image-default-frame");
          assert.equal(options.frame, 1);
          intermediate = bytes;
          return {
            files: {
              "frame.png": new ArrayBuffer(1),
              "frame.sprite": new ArrayBuffer(48),
              "frame.rgba": new ArrayBuffer(8),
            },
          };
        },
        close: () => closed++,
      },
    });
    const result = await worker.run("image-default-frame", input, { frame: 1 });
    assert.deepEqual(Object.keys(result.files), ["frame.png"]);
    assert.equal(result.sourceVersion, 61);
    assert.ok(intermediate.every((x) => x === 0));
    assert.ok(decoded.pixels.every((x) => x === 0));
    assert.equal(input[4], 61);
    worker.close();
    worker.close();
    assert.equal(closed, 2);
  });
  test("texture client uses real worker wire contract and does not detach input", async () => {
    const { input, decoded } = fixture();
    let stopped = false;
    const transport = {
      postMessage(message, transfer) {
        assert.equal(message.operation, "decodeV61");
        assert.notEqual(message.source.buffer, input.buffer);
        assert.equal(transfer[0], message.source.buffer);
        queueMicrotask(() =>
          transport.onmessage({ data: { id: message.id, ok: true, result: decoded } }),
        );
      },
      terminate() {
        stopped = true;
      },
    };
    const client = new TextureWorkerClient(transport);
    assert.equal(await client.decodeV61(input), decoded);
    client.close();
    assert.equal(stopped, true);
  });
  test("v61 packaging requires explicit opt-in and forbids sprite output even when enabled", async () => {
    const { input } = fixture();
    const make = (enableV61Png, output) =>
      createAssetGeneratorAdapter({
        enableV61Png,
        createWorker: () => ({
          capabilities: { v61Png: true },
          run: async () => ({ files: { "frame.png": new ArrayBuffer(1) } }),
          close() {},
        }),
        recipes: [{ operation: "image-default-frame", input: "data/test.sprite", outputs: output }],
      });
    const context = { extracted: { logicalAssets: { read: async () => input } } };
    await assert.rejects(make(false, { "frame.png": "test.png" }).generateAssets(context), {
      code: "ASSET_V61_NOT_CONNECTED",
    });
    await assert.rejects(make(true, { "frame.sprite": "test.sprite" }).generateAssets(context), {
      code: "ASSET_V61_SPRITE_UNSUPPORTED",
    });
    const adapter = make(true, { "frame.png": "test.png" });
    assert.equal((await adapter.generateAssets(context))[0].size, 1);
    await adapter.dispose();
  });

  test("v61 bridge wipes late decoded pixels after close without starting asset conversion", async () => {
    const { input, decoded } = fixture();
    const before = input.slice();
    let finishDecode,
      runs = 0;
    const worker = createV61AssetWorker({
      textureWorker: {
        decodeV61: () =>
          new Promise((resolve) => {
            finishDecode = resolve;
          }),
        close() {},
      },
      assetWorker: {
        run() {
          runs++;
        },
        close() {},
      },
    });
    const pending = worker.run("image-default-frame", input);
    worker.close();
    finishDecode(decoded);
    await assert.rejects(pending, { message: "V61_ASSET_BRIDGE_FAILED" });
    assert.equal(runs, 0);
    assert.ok(decoded.pixels.every((value) => value === 0));
    assert.deepEqual(input, before);
  });

  test("v61 bridge wipes decoded pixels when geometry validation rejects before conversion", async () => {
    for (const mismatch of [{ width: 3 }, { pixelFormat: 1 }, { pixels: new Uint8Array([9]) }]) {
      const { input, decoded } = fixture();
      Object.assign(decoded, mismatch);
      const before = input.slice();
      let runs = 0;
      const worker = createV61AssetWorker({
        textureWorker: { decodeV61: async () => decoded, close() {} },
        assetWorker: {
          run() {
            runs++;
          },
          close() {},
        },
      });
      await assert.rejects(worker.run("image-default-frame", input), {
        message: "V61_ASSET_BRIDGE_FAILED",
      });
      assert.equal(runs, 0);
      assert.ok(decoded.pixels.every((value) => value === 0));
      assert.deepEqual(input, before);
      worker.close();
    }
  });

  test("v61 bridge preserves safe rejection for absent or nonbyte decoded pixels", async () => {
    for (const decoded of [null, {}, { pixels: {} }]) {
      const { input } = fixture();
      const worker = createV61AssetWorker({
        textureWorker: { decodeV61: async () => decoded, close() {} },
        assetWorker: {
          run() {
            assert.fail("invalid decoding must not convert");
          },
          close() {},
        },
      });
      await assert.rejects(worker.run("image-default-frame", input), {
        message: "V61_ASSET_BRIDGE_FAILED",
      });
      worker.close();
    }
  });

  test("v61 bridge wipes owned rejected worker outputs but preserves caller aliases", async () => {
    for (const invalid of [false, true]) {
      const { input, decoded } = fixture();
      const before = input.slice();
      const png = new Uint8Array([8, 9]);
      const sprite = new Uint8Array([10, 11]);
      let finish, started;
      const running = new Promise((resolve) => {
        started = resolve;
      });
      const worker = createV61AssetWorker({
        textureWorker: { decodeV61: async () => decoded, close() {} },
        assetWorker: {
          run() {
            started();
            return new Promise((resolve) => {
              finish = resolve;
            });
          },
          close() {},
        },
      });
      const pending = worker.run("image-default-frame", input);
      await running;
      if (!invalid) worker.close();
      finish({
        files: {
          "frame.png": invalid ? png : png.buffer,
          "frame.sprite": sprite.buffer,
          "caller-alias": input.buffer,
        },
      });
      await assert.rejects(pending, { message: "V61_ASSET_BRIDGE_FAILED" });
      assert.ok(png.every((value) => value === 0));
      assert.ok(sprite.every((value) => value === 0));
      assert.deepEqual(input, before);
      worker.close();
    }
  });

  test("accepted v61 PNG output remains intact after bridge cleanup", async () => {
    const { input, decoded } = fixture();
    const png = new Uint8Array([1, 2, 3]);
    const worker = createV61AssetWorker({
      textureWorker: { decodeV61: async () => decoded, close() {} },
      assetWorker: {
        run: async () => ({ files: { "frame.png": png.buffer } }),
        close() {},
      },
    });
    const result = await worker.run("image-default-frame", input);
    assert.equal(result.files["frame.png"], png.buffer);
    assert.deepEqual(png, new Uint8Array([1, 2, 3]));
    worker.close();
    assert.deepEqual(png, new Uint8Array([1, 2, 3]));
  });
}

// native-overlay-prebuilt
{
  // 普通测试入口始终执行隔离模块模拟覆盖，不要求调用方记住实验标志。
  // The ordinary test entry always executes isolated module-mock coverage without caller flags.
  const { assertIsolatedRun } = assertIsolatedRunModule;
  const { default: test } = nodeTestModule;
  const { spawnSync } = nodeChild_processModule;
  const { default: process } = nodeProcessModule;
  test("native overlay optional prebuilt assembly (isolated module mocks)", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = spawnSync(
      process.execPath,
      [
        "--experimental-test-module-mocks",
        "--test",
        "--test-reporter=tap",
        new URL("./fixtures/native-overlay-prebuilt.mock.mjs", import.meta.url).pathname,
      ],
      { env, encoding: "utf8", timeout: 30000, maxBuffer: 1024 * 1024 },
    );
    assertIsolatedRun(result);
  });
}

// overlay-recipe-provenance
{
  // 验证 overlay-recipe-provenance 的约定与边界；依据 web/tests 及本文件的测试断言。
  // Verify overlay-recipe-provenance contracts and boundaries against web/tests and this file's assertions.
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const { fileURLToPath } = nodeUrlModule;
  const { default: test } = nodeTestModule;

  test("overlay source and derived-data pins stay synchronized after shared UI text changes", () => {
    const result = spawnSync(
      process.execPath,
      [fileURLToPath(new URL("../toolchain/verify-asset-sources.mjs", import.meta.url)), "overlay"],
      { encoding: "utf8" },
    );
    assert.equal(result.error, undefined);
    assert.equal(result.status, 0, result.stderr || result.stdout);
  });
}

// overlay-unicode-allocation
{
  // 合成文本检查避免字符数组扩张，同时保留 Unicode 行为；不使用游戏输入。
  // Synthetic text checks avoid character-array expansion while preserving Unicode behavior; no game inputs.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { spawnSync } = nodeChild_processModule;
  const { deflateSync } = nodeZlibModule;
  const { logical } = recipeModule;
  const { vaultTab } = inventoryModule;
  const { createAnnotations } = annotationsModule;
  const { inventoryFixtures } = inventoryParityModule;
  const { buildAnnotationFixture } = annotationsParityModule;
  const { createCascOverlayTransform } = overlayModule;
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  test("logical paths preserve codepoint boundaries and reject huge paths before iteration", () => {
    for (const value of ["a".repeat(4091), "😀".repeat(4091), "\ud800"])
      assert.equal(logical("DATA\\" + value), "data/" + value);
    for (const value of ["a".repeat(4092), "😀".repeat(4092), "bad\n", "../bad"])
      assert.throws(() => logical("data/" + value), /OVERLAY_PATH/);
    const original = String.prototype[Symbol.iterator];
    String.prototype[Symbol.iterator] = function () {
      assert.ok(this.length <= 8192, "must not iterate an oversized path");
      return original.call(this);
    };
    try {
      assert.throws(() => logical("data/" + "a".repeat(1024 * 1024)), /OVERLAY_PATH/);
      assert.equal(logical("x".repeat(1024 * 1024) + ":data/good"), "data/good");
    } finally {
      String.prototype[Symbol.iterator] = original;
    }
  });

  test("vault layout span counts astral codepoints without a full character array", () => {
    const { bank } = inventoryFixtures();
    const rewritten = vaultTab(bank);
    const start = rewritten.indexOf('"name": "BankTabs"');
    const key = '"tabRightIndicatorPosition":';
    const span = rewritten.indexOf(key) - start;
    const original = Array.from;
    Array.from = function (value, ...args) {
      assert.notEqual(typeof value, "string", "must not expand a layout span");
      return original.call(this, value, ...args);
    };
    try {
      for (const char of ["a", "😀"])
        for (const extra of [0, 1]) {
          const text = bank.replace(key, char.repeat(2500 - span + extra) + key);
          if (extra) assert.throws(() => vaultTab(text), /BankTabs/);
          else assert.ok(vaultTab(text).includes('"x": 920'));
        }
      assert.throws(() => vaultTab(bank.replace(key, "x".repeat(1024 * 1024) + key)), /BankTabs/);
    } finally {
      Array.from = original;
    }
  });

  test("annotations avoid expanding discarded long skills and preserve first-codepoint uppercase", async () => {
    const fixture = buildAnnotationFixture();
    const longSkill = "Skill" + "x".repeat(10000);
    const name = "ß" + "x".repeat(10000);
    for (const [file, change] of [
      [
        "skills.json",
        (rows) => {
          rows.find((row) => row.Key === "skillname1").esES = longSkill;
        },
      ],
      [
        "item-names.json",
        (rows) => {
          rows.find((row) => row.Key === "isc").esES = "[ms]Pergamino de " + name;
        },
      ],
    ]) {
      const path = "data/local/lng/strings/" + file;
      const rows = JSON.parse(decoder.decode(fixture.assets.get(path)));
      change(rows);
      fixture.assets.set(path, encoder.encode(JSON.stringify(rows)));
    }
    const original = Array.from;
    Array.from = function (value, ...args) {
      assert.ok(
        value !== longSkill && value !== name,
        "must not expand the checked skill or stripped name",
      );
      return original.call(this, value, ...args);
    };
    try {
      const result = await createAnnotations({
        readAsset: (path) => fixture.assets.get(path),
        sha1: fixture.sha1,
      }).buildItemNotes();
      const rows = JSON.parse(
        decoder
          .decode(result.replacements.get("data/local/lng/strings/item-names.json"))
          .replace(/^\ufeff/, ""),
      );
      assert.equal(rows.find((row) => row.Key === "isc").esES, "[ms]SS" + "x".repeat(10000));
    } finally {
      Array.from = original;
    }
  });

  test("CASC config encoding validates large text without a character array", async () => {
    const fixture = spawnSync(
      "python3",
      [new URL("../toolchain/casc-overlay-js/test/fixture.py", import.meta.url).pathname],
      { encoding: "utf8", maxBuffer: 8 * 1024 * 1024 },
    );
    assert.equal(fixture.status, 0, fixture.stderr);
    const request = JSON.parse(fixture.stdout);
    const payload = Buffer.from(request.payload, "base64");
    const comment = "#" + "x".repeat(100000) + "\n";
    const config = Buffer.concat([
      payload.subarray(0, request.options.parts[0]),
      encoder.encode(comment),
    ]);
    const input = Buffer.concat([config, payload.subarray(request.options.parts[0])]);
    request.options.parts[0] = config.length;
    const generate = createCascOverlayTransform({
      md5: (bytes) => new Uint8Array(createHash("md5").update(bytes).digest()),
      compress: (bytes) => new Uint8Array(deflateSync(bytes, { level: 9 })),
    });
    const original = Array.from;
    Array.from = function (value, ...args) {
      assert.ok(
        typeof value !== "string" || value.length < 5000,
        "must not expand the large config",
      );
      return original.call(this, value, ...args);
    };
    try {
      const result = await generate(input, request.options);
      assert.ok(decoder.decode(result.files["data/.build.config"]).includes(comment));
    } finally {
      Array.from = original;
    }
  });
}

// overlay-v2-contract
{
  // 仅合成字节和仓库静态数据的 v2 覆盖层测试；不运行原生生成器或 WASM。
  // V2 overlay tests use only synthetic bytes and repository static data, never native generators or WASM.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { default: test } = nodeTestModule;
  const { default: data } = annotationDataJsonModule;
  const { default: release } = releaseV2DataJsonModule;
  const { releaseI18n } = releaseV2Module;
  const { createAnnotations } = annotationsModule;
  const { padCustomPage } = touchLayoutModule;
  const { controllerGlyphs } = recipeModule;
  const { RELEASE_OVERLAY_REVISION, UPSTREAM_OVERLAY_REVISION, requireOverlayRevision } =
    overlayRevisionModule;
  const { createOverlayManifest, OVERLAY_SOURCE_FINGERPRINT } = overlayContractModule;

  const enc = new TextEncoder(),
    dec = new TextDecoder();
  const sha256 = (value) => createHash("sha256").update(value).digest("hex");
  const stringPath = "data/local/lng/strings/ui.json";
  test("retired overlay revisions and layout branches reject before generating output", () => {
    assert.throws(() => requireOverlayRevision("v1.0.18"), /REVISION/);
    assert.throws(
      () => createAnnotations({ readAsset() {}, sha1() {}, upstream: false }),
      /REVISION/,
    );
    assert.throws(
      () => padCustomPage('"name": "VideoOptionsPanel" "name": "OptionsTable"', false),
      /REVISION/,
    );
  });
  test("supported Korean i18n retains exact upstream rows and layout", async () => {
    for (const upstream of [true]) {
      const pipeline = createAnnotations({
        readAsset: () => enc.encode('[{"Key":"original"}]'),
        sha1: () => "0".repeat(40),
        upstream,
      });
      const result = await pipeline.buildI18n();
      const rows = JSON.parse(dec.decode(result.replacements.get(stringPath))).slice(1);
      const expected = data.i18n;
      assert.equal(result.count, expected.rows.length);
      assert.deepEqual(
        rows.map((row) => row.Key),
        expected.rows.map((row) => row[0]),
      );
      assert.ok(rows.every((row) => !row.Key.startsWith("dmWeb")));
      for (let i = 0; i < rows.length; i++) {
        assert.equal(rows[i].id, expected.id0 + i);
        assert.equal(rows[i].koKR, expected.rows[i][6]);
        assert.equal(rows[i].esMX, expected.rows[i][4] || expected.rows[i][3]);
      }
      const page = padCustomPage('"name": "VideoOptionsPanel" "name": "OptionsTable"', upstream);
      assert.equal(page.includes("TouchCustom_lang_ko"), upstream);
      assert.equal(page.includes("TouchCustomLangHintKo"), upstream);
      assert.ok(!page.includes("TouchCustom_voice"));
    }
    assert.throws(() => requireOverlayRevision("v1.0.18"), /REVISION/);
    assert.equal(requireOverlayRevision(UPSTREAM_OVERLAY_REVISION), true);
    assert.equal(requireOverlayRevision(RELEASE_OVERLAY_REVISION), true);
    assert.throws(() => requireOverlayRevision("v2.0.0"), /REVISION/);
  });
  test("b811 release preserves its exact row IDs and excludes later voice additions", async () => {
    assert.equal(sha256(JSON.stringify(releaseI18n)), release.i18nSha256);
    const result = await createAnnotations({
      readAsset: () => enc.encode('[{"Key":"original"}]'),
      sha1: () => "0".repeat(40),
      release: true,
    }).buildI18n();
    const rows = JSON.parse(dec.decode(result.replacements.get(stringPath))).slice(1);
    assert.equal(rows.length, 449);
    assert.deepEqual(
      rows.map((row) => row.Key),
      releaseI18n.rows.map((row) => row[0]),
    );
    assert.ok(rows.some((row) => row.Key === "dmCustomLangKoOpt"));
    assert.ok(rows.every((row) => !row.Key.startsWith("dmCustomVoice")));
  });

  test("Korean touch strings retain source checks and do not affect legacy sidecars", async () => {
    const records = new Map();
    for (const [lang, tables] of Object.entries(data.touch.tables))
      for (const [name, items] of Object.entries(tables)) {
        if (!records.has(name)) records.set(name, new Map());
        for (const [key, [hash]] of Object.entries(items)) {
          if (!records.get(name).has(key)) records.get(name).set(key, { Key: key });
          records.get(name).get(key)[lang] = `HASH:${hash}`;
        }
      }
    const assets = new Map(
      [...records].map(([name, rows]) => [
        "data/local/lng/strings/" + name,
        enc.encode(JSON.stringify([...rows.values()])),
      ]),
    );
    for (const upstream of [true]) {
      const pipeline = createAnnotations({
        readAsset: (path) => assets.get(path),
        upstream,
        sha1: (raw) => dec.decode(raw).slice(5) + "0".repeat(32),
      });
      const sidecar = dec.decode((await pipeline.buildTouchStrings()).touchStrings);
      assert.equal(sidecar.includes("두 번 탭하여 사용"), upstream);
      assert.ok(sidecar.includes("MoviePlaybackHoldToSkip"));
    }
    await assert.rejects(
      createAnnotations({
        readAsset: (path) => assets.get(path),
        sha1: () => "0".repeat(40),
      }).buildTouchStrings(),
      /TOUCH_STRING_SOURCE/,
    );
  });

  test("controller glyphs implement face swaps, rectangle fallback and Switch fallback", () => {
    const prefix = "data/hd/global/ui/controller/controllerbuttons/",
      names = [
        "ButtonRight.sprite",
        "ButtonBottom.sprite",
        "ButtonTop.sprite",
        "ButtonLeft.sprite",
        "ButtonLeftRectangle.sprite",
        "DPad.sprite",
      ];
    const assets = new Map();
    const rows = [];
    const add = (path) => {
      assets.set(path.toLowerCase(), enc.encode(path));
      const row = Array(11).fill("");
      row[9] = "asset:" + path;
      rows.push(row);
    };
    for (const name of names)
      for (const platform of ["switch", "xbox"]) add(prefix + platform + "/" + name);
    for (const name of names.slice(0, 4)) add(prefix + "ps5/" + name);
    const out = controllerGlyphs(rows, (path) => {
      const bytes = assets.get(path.toLowerCase());
      assert.ok(bytes, path);
      return bytes;
    });
    const dest = "data/hd/global/ui/ztouch_controller_buttons_ui/";
    for (const [name, source] of [
      ["switchxbuttonright.sprite", "xbox/ButtonBottom.sprite"],
      ["switchxbuttonbottom.sprite", "xbox/ButtonRight.sprite"],
      ["switchxbuttontop.sprite", "xbox/ButtonTop.sprite"],
      ["switchpbuttonright.sprite", "ps5/ButtonBottom.sprite"],
      ["switchpbuttontop.sprite", "ps5/ButtonLeft.sprite"],
      ["switchpbuttonleftrectangle.sprite", "ps5/ButtonLeft.sprite"],
      ["switchpdpad.sprite", "switch/DPad.sprite"],
    ])
      assert.equal(dec.decode(out.get(dest + name)), prefix + source);
    assert.equal(out.size, names.length * 2);
    const releaseGlyphs = controllerGlyphs(rows, (path) => assets.get(path.toLowerCase()), {
      playstation: false,
    });
    assert.equal(releaseGlyphs.size, names.length);
    assert.ok([...releaseGlyphs.keys()].every((name) => name.startsWith(dest + "switchx")));
  });

  function overlayFiles() {
    return {
      "data/a.data": enc.encode("archive"),
      "data/.build.config": enc.encode("config\r\n"),
      ctrl_touch: enc.encode("marker\n"),
      "touch_strings.tsv": enc.encode("text\n"),
      ps_glyphs: new Uint8Array(),
      "pack.json": enc.encode('{"pack":1}\n'),
    };
  }
  test("manifest uses exact payload bytes, portable sorted paths and all sidecars", async () => {
    const files = overlayFiles();
    const expected =
      `DM_OVERLAY_V1 ${OVERLAY_SOURCE_FINGERPRINT}\n` +
      Object.keys(files)
        .sort()
        .map((path) => `${sha256(files[path])} ${path}\n`)
        .join("");
    assert.equal(dec.decode(await createOverlayManifest(files)), expected);
    assert.notEqual(sha256(files["data/.build.config"]), sha256(enc.encode("config\n")));
    assert.equal(
      dec.decode(await createOverlayManifest(Object.fromEntries(Object.entries(files).reverse()))),
      expected,
    );
  });
  test("manifest rejects incomplete, ambiguous, unsafe and cancelled payloads", async () => {
    for (const name of ["ctrl_touch", "touch_strings.tsv", "data/.build.config", "data/a.data"]) {
      const files = overlayFiles();
      delete files[name];
      await assert.rejects(createOverlayManifest(files), /REQUIRED/);
      files[name] = new Uint8Array();
      await assert.rejects(createOverlayManifest(files), /EMPTY/);
    }
    for (const name of [
      "../outside",
      "data/CON.bin",
      "data/x.",
      "data/é",
      "CTRL_TOUCH",
      "ctrl_touch/nested",
      "overlay.manifest",
    ])
      await assert.rejects(
        createOverlayManifest({ ...overlayFiles(), [name]: enc.encode("bad") }),
        /PATH/,
      );
    const controller = new AbortController();
    controller.abort(new Error("cancelled"));
    await assert.rejects(
      createOverlayManifest(overlayFiles(), { signal: controller.signal }),
      /cancelled/,
    );
  });
}

// upstream-v2-art-contract
{
  // 只覆盖惰性合成约定，不使用原生代码、WASM、浏览器、字体或私有输入。
  // Inert synthetic contract coverage only. No native, WASM, browser, font or private input.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFileSync } = nodeFsModule;
  const { createHash } = nodeCryptoModule;
  const { generateNativeArt } = nativeArtModule;
  const { createNativeArtCases, nativeHashDependencies } = nativeCasesModule;
  const { vitalCharacters } = supportModule;

  const codec = {
    encodeRgba(rgba, width, height) {
      // 确定性的替身，不是 PNG 编码器或像素一致性参考。
      // A deterministic stand-in, not a PNG encoder or pixel-parity oracle.
      return new Uint8Array([...new TextEncoder().encode(`${width}x${height}:`), ...rgba]);
    },
  };

  test("v2 native-art preserves all bounded synthetic success/rejection cases", async () => {
    const cases = await createNativeArtCases(codec);
    for (const entry of cases) {
      const run = () =>
        generateNativeArt(entry.payload, entry.options, { codec, ...nativeHashDependencies });
      if (entry.rejects) await assert.rejects(run, undefined, entry.label);
      else {
        const result = await run();
        assert.equal(result.assetCount, 10, entry.label);
        assert.equal(Object.keys(result.files).length, 11, entry.label);
        const source = new TextDecoder().decode(result.files["dm_touch_native_art.c"]);
        assert.ok(
          source.startsWith(
            "/* 从冻结原版表生成，不手改。 | Generated from frozen official tables; do not hand-edit. */\n",
          ),
        );
        assert.ok(
          source.includes(
            `const char dm_ta_manifest_sha[]="${entry.options.metadataSha256.manifest}";`,
          ),
        );
        assert.ok(source.includes('#include "../../host/dm_input.h"'));
      }
    }
  });

  test("v2 native-art requires RGBA pins even when encoded bytes still match", async () => {
    const [entry] = await createNativeArtCases(codec);
    const bundle = JSON.parse(new TextDecoder().decode(entry.payload));
    const manifest = JSON.parse(bundle.metadata.manifest);
    delete manifest.assets[0].decoded_rgba_sha256;
    bundle.metadata.manifest = JSON.stringify(manifest);
    const bytes = new TextEncoder().encode(bundle.metadata.manifest);
    const options = {
      metadataSha256: {
        ...entry.options.metadataSha256,
        manifest: nativeHashDependencies.sha256(bytes),
      },
    };
    await assert.rejects(
      generateNativeArt(new TextEncoder().encode(JSON.stringify(bundle)), options, {
        codec,
        ...nativeHashDependencies,
      }),
      /RGBA/,
    );
  });

  const header = "键\t中文\tEnglish\tEspañol\tEspañol MX\tРусский\t한국어\t用在哪";
  const i18n = (koLife = "생명력", koMana = "마나") =>
    `${header}\ndmHudLife\t生命\tLife\tVida\t\tЖизнь\t${koLife}\tHUD\ndmHudMana\t法力\tMana\tMana\t\tМана\t${koMana}\tHUD\n`;

  test("v2 HUD includes Korean vital glyphs and validates the extra language", () => {
    const characters = vitalCharacters(i18n(), true);
    assert.equal(vitalCharacters(i18n()), characters);
    assert.throws(() => vitalCharacters(i18n(), false), /REVISION/);
    for (const char of "생명력마나") assert.ok(characters.includes(char));
    assert.throws(() => vitalCharacters(i18n(""), true), /HUD_I18N_ROW/);
    assert.throws(() => vitalCharacters(i18n("生命"), true), /HUD_I18N_CJK/);
    assert.throws(() => vitalCharacters(i18n("%d"), true), /HUD_I18N_PLACEHOLDER/);
    assert.throws(() => vitalCharacters(i18n("생명력\\n"), true), /HUD_I18N_NEWLINES/);
    assert.throws(
      () => vitalCharacters(i18n().replace("한국어", "missing"), true),
      /HUD_I18N_HEADER/,
    );
  });

  test("v2 HUD reads the actual shared source table, including Korean", () => {
    const table = readFileSync(new URL("../../dm/i18n/ui_text.tsv", import.meta.url), "utf8");
    const characters = vitalCharacters(table, true);
    for (const char of "생명마나") assert.ok(characters.includes(char));
  });

  test("retired HUD revisions reject before loading assets", () => {
    assert.throws(() => overlayRevisionModule.requireOverlayRevision("v1.0.18"), /REVISION/);
  });

  test("b811/e375 art source comparison stays pinned to the reviewed merge inputs", () => {
    const record = JSON.parse(
      readFileSync(
        new URL("../toolchain/art-recipes-js/release-source-compatibility.json", import.meta.url),
        "utf8",
      ),
    );
    const sha = (bytes) => createHash("sha256").update(bytes).digest("hex");
    for (const [path, hash] of Object.entries(record.sharedSourceSha256)) {
      const expected = record.mergedSourceOverrides[path]?.sha256 ?? hash;
      assert.equal(sha(readFileSync(new URL(`../../${path}`, import.meta.url))), expected, path);
    }
    const table = readFileSync(new URL(`../../${record.i18n.path}`, import.meta.url), "utf8");
    assert.equal(sha(table), record.i18n.mergedSha256);
    for (const [key, columns] of Object.entries(record.i18n.unchangedHudVitalRows)) {
      const row = table.split("\n").find((line) => line.startsWith(`${key}\t`));
      assert.deepEqual(row.split("\t").slice(1, 7), columns);
    }
  });
}

// Asset source gates share one command without running unrelated gates on import.
{
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const { fileURLToPath } = nodeUrlModule;
  const gateUrl = new URL("../toolchain/verify-asset-sources.mjs", import.meta.url);
  const gates = [
    [
      "asset-transforms",
      "asset-transforms-js/source-provenance.json",
      "Asset transform native source drift",
    ],
    ["casc-overlay", "casc-overlay-js/source-provenance.json", "CASC overlay source drift"],
    ["art-recipes", "art-recipes-js/source-provenance.json", "Art recipe source drift"],
    ["remaining-art", "remaining-art-js/source-provenance.json", "Remaining-art source drift"],
    ["hud", "hud-recipes-js/provenance.json", "HUD source drift"],
    ["overlay", "overlay-recipe-js/source-provenance.json", "Overlay recipe source drift"],
  ];
  test("each consolidated asset source selector verifies its original source contract", () => {
    for (const [selector] of gates) {
      const result = spawnSync(process.execPath, [fileURLToPath(gateUrl), selector], {
        encoding: "utf8",
      });
      assert.equal(result.status, 0, `${selector}: ${result.stderr}`);
    }
    for (const args of [[], ["unknown"], ["toString"]]) {
      const result = spawnSync(process.execPath, [fileURLToPath(gateUrl), ...args], {
        encoding: "utf8",
      });
      assert.notEqual(result.status, 0);
    }
  });
  test("consolidated gate imports read no manifests and selected gates reject source tampering", async () => {
    const { mkdtemp, mkdir, readFile, writeFile, rm } = await import("node:fs/promises");
    const { tmpdir } = await import("node:os");
    const { join, dirname } = await import("node:path");
    const { pathToFileURL } = await import("node:url");
    const directory = await mkdtemp(join(tmpdir(), "asset-source-gates-"));
    try {
      const script = join(directory, "web/toolchain/verify-asset-sources.mjs");
      await mkdir(dirname(script), { recursive: true });
      await writeFile(script, await readFile(gateUrl));
      const imported = spawnSync(
        process.execPath,
        [
          "--input-type=module",
          "-e",
          `await import(${JSON.stringify(pathToFileURL(script).href)})`,
        ],
        { encoding: "utf8" },
      );
      assert.equal(imported.status, 0, imported.stderr);
      assert.equal(imported.stdout, "");
      for (const [selector, manifestPath, diagnostic] of gates) {
        const bytes = await readFile(new URL(`../toolchain/${manifestPath}`, import.meta.url));
        const manifest = JSON.parse(bytes);
        const firstSource = Object.keys(manifest.sources ?? manifest)[0];
        const target = join(directory, "web/toolchain", manifestPath);
        await mkdir(dirname(target), { recursive: true });
        await writeFile(target, bytes);
        const source = join(directory, firstSource);
        await mkdir(dirname(source), { recursive: true });
        await writeFile(source, "deliberately changed source bytes");
        const result = spawnSync(process.execPath, [script, selector], { encoding: "utf8" });
        assert.notEqual(result.status, 0, selector);
        assert.ok(result.stderr.includes(diagnostic), result.stderr);
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });
}
