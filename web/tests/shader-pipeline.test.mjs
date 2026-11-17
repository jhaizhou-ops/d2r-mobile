// Feature suite: shader-pipeline. Original test sections retain independent lexical scopes.
import * as strictModule from "node:assert/strict";
import * as nodeCryptoModule from "node:crypto";
import * as nodeTestModule from "node:test";
import * as compiledShaderBoundaryModule from "../compiled-shader-boundary.mjs";
import * as nodeChild_processModule from "node:child_process";
import * as productionShadersModule from "../production-shaders.mjs";
import * as nodeFsModule from "node:fs";
import * as shaderCatalogAdapterModule from "../shader-catalog-adapter.mjs";
import * as promisesModule from "node:fs/promises";
import * as nodeOsModule from "node:os";
import * as nodePathModule from "node:path";
import * as nodeUrlModule from "node:url";
import * as inventoryShaderCorpusModule from "../scripts/inventory-shader-corpus.mjs";
import * as shaderDiagnosticsModule from "../shader-diagnostics.mjs";
import * as shaderPipelineModule from "../shader-pipeline.mjs";
import * as shaderFeatureInventoryModule from "../shader-feature-inventory.mjs";
import * as syntheticFixtureModule from "../toolchain/shader-spike/wwwroot/synthetic-fixture.mjs";
import * as convertModule from "../toolchain/spirv-cross-spike/wwwroot/convert.mjs";
import * as graphicsFixturesModule from "../toolchain/shader-pipeline-spike/graphics-fixtures.mjs";
import * as replayReflectionModule from "../toolchain/spirv-cross-spike/wwwroot/replay-reflection.mjs";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as shaderSourcelinkDiagnosticModule from "../runtime-build/shader-sourcelink-diagnostic.mjs";

// compiled-shader-boundary
{
  // 验证 compiled-shader-boundary 的约定与边界；依据 ../compiled-shader-boundary.mjs 及本文件的测试断言。
  // Verify compiled-shader-boundary contracts and boundaries against ../compiled-shader-boundary.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { default: test } = nodeTestModule;
  const { acceptCompiledShaderOutput, inspectCompiledMetalLibrary } = compiledShaderBoundaryModule;
  const enc = new TextEncoder();
  const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
  function u16(n) {
    const b = new Uint8Array(2);
    new DataView(b.buffer).setUint16(0, n, true);
    return b;
  }
  function u32(n) {
    const b = new Uint8Array(4);
    new DataView(b.buffer).setUint32(0, n, true);
    return b;
  }
  function u64(n) {
    const b = new Uint8Array(8);
    new DataView(b.buffer).setBigUint64(0, BigInt(n), true);
    return b;
  }
  function concat(...parts) {
    const b = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) {
      b.set(p, o);
      o += p.length;
    }
    return b;
  }
  const tag = (name, bytes) => concat(enc.encode(name), u16(bytes.length), bytes);
  // 合成容器仅测试边界，载荷不是可执行着色器，也不证明真实编译成功。
  // Synthetic containers test the boundary only; their payload is not an executable shader or compilation proof.
  function fixture({ wrapped = false } = {}) {
    let bitcode = new Uint8Array([0x42, 0x43, 0xc0, 0xde, 0, 0, 0, 0]);
    if (wrapped)
      bitcode = concat(u32(0x0b17c0de), u32(0), u32(20), u32(bitcode.length), u32(0), bitcode);
    const body = concat(
      tag("NAME", enc.encode("main0\0")),
      tag("TYPE", new Uint8Array([1])),
      tag("HASH", new Uint8Array(createHash("sha256").update(bitcode).digest())),
      tag("MDSZ", u64(bitcode.length)),
      tag("OFFT", concat(u64(0), u64(0), u64(0))),
      tag("VERS", concat(u16(2), u16(6), u16(2), u16(3))),
      enc.encode("ENDT"),
    );
    const functions = concat(u32(1), u32(body.length + 4), body);
    const metadata = concat(u32(4), enc.encode("ENDT"));
    const header = new Uint8Array(88),
      view = new DataView(header.buffer);
    header.set(enc.encode("MTLB"));
    view.setUint16(4, 1, true);
    view.setUint16(6, 2, true);
    view.setUint16(8, 7, true);
    header[11] = 0x82;
    view.setUint16(12, 18, true);
    let start = 88;
    for (const [i, part] of [functions, metadata, metadata, bitcode].entries()) {
      header.set(u64(start), 24 + i * 16);
      header.set(u64(part.length - (i === 0 ? 4 : 0)), 32 + i * 16);
      start += part.length;
    }
    header.set(u64(start), 16);
    return concat(header, functions, metadata, metadata, bitcode);
  }
  function input(binary = fixture()) {
    return {
      source: "fragment float4 main0() { return 1; }\n",
      binary,
      compiler: { id: "test-only", assetSha256: "1".repeat(64) },
      options: { platform: "ios", minimumOS: [18, 0], languageVersion: [2, 3], fastMath: false },
      expected: {
        key: "0000000000000001",
        variant: "b",
        entry: "main0",
        stage: "fragment",
        sourceSetDigest: "be59c864f087b377",
        sourceSha256: hash("fragment float4 main0() { return 1; }\n"),
      },
    };
  }
  // 自编 a2flo 段/根向量封装，不是真实编译器输出或反射语义。
  // Authored a2flo section/root-vector envelope, not real compiler output or reflection semantics.
  function declaredTailFixture() {
    const original = fixture(),
      view = new DataView(original.buffer);
    const oldPublic = Number(view.getBigUint64(40, true));
    const reference = tag("RFLT", u64(4)),
      extensionSize = 70;
    const shift = reference.length + extensionSize;
    const dynamic = concat(tag("NAME", enc.encode("shader.metallib\0")), enc.encode("ENDT"));
    const dynamicStart = original.length + shift,
      reflectionStart = dynamicStart + dynamic.length;
    // AIRR 根包含版本 0.4.0 及两个元素大小为四字节的空向量。
    // AIRR root: version 0.4.0 and two empty four-byte-element vectors.
    const airr = concat(
      u32(20),
      enc.encode("AIRR"),
      u16(0),
      u16(10),
      u16(24),
      u16(4),
      u16(16),
      u16(20),
      u32(10),
      u32(0),
      u32(4),
      u32(0),
      u32(8),
      u32(8),
      u32(0),
      u32(0),
    );
    const groupStart = reflectionStart + 4,
      padding = (16 - ((groupStart + 12) % 16)) % 16;
    const reflection = concat(
      u32(1),
      u32(16 + padding + airr.length),
      enc.encode("RBUF"),
      u32(padding + airr.length),
      new Uint8Array(padding),
      airr,
      enc.encode("ENDT"),
    );
    const extension = concat(
      tag("HDYN", concat(u64(dynamicStart), u64(dynamic.length))),
      tag("RLST", concat(u64(reflectionStart), u64(reflection.length))),
      tag("UUID", new Uint8Array(16)),
      enc.encode("ENDT"),
    );
    assert.equal(extension.length, extensionSize);
    const binary = concat(
      original.subarray(0, oldPublic - 4),
      reference,
      original.subarray(oldPublic - 4, oldPublic),
      extension,
      original.subarray(oldPublic),
      dynamic,
      reflection,
    );
    binary.set(u64(binary.length), 16);
    binary.set(u64(Number(view.getBigUint64(32, true)) + reference.length), 32);
    binary.set(u32(view.getUint32(92, true) + reference.length), 92);
    for (const field of [40, 56, 72])
      binary.set(u64(Number(view.getBigUint64(field, true)) + shift), field);
    return {
      binary,
      extension: oldPublic + reference.length,
      reference: oldPublic + 2,
      dynamic: dynamicStart,
      reflection: reflectionStart,
      group: groupStart,
      padding,
      airr: groupStart + 12 + padding,
      bitcode: original.length - 8 + shift,
    };
  }

  test("accepts only declared non-debug HDYN/RLST envelopes and binds the entire output", async () => {
    const f = declaredTailFixture(),
      request = input(f.binary);
    const options = {
      entry: "main0",
      stage: "fragment",
      minimumOS: [18, 0],
      languageVersion: [2, 3],
      metadataLengthIncludesPrefix: false,
    };
    const inspected = await inspectCompiledMetalLibrary(f.binary, options);
    const legacy = await inspectCompiledMetalLibrary(fixture(), options);
    const first = await acceptCompiledShaderOutput(request);
    assert.equal(inspected.validation, "container-and-integrity-only");
    assert.equal(inspected.functions[0].bitcodeSha256, legacy.functions[0].bitcodeSha256);
    assert.equal(first.receipt.outputSha256, hash(f.binary));
    assert.equal(first.receipt.sourceSha256, request.expected.sourceSha256);
    // 有效的仅尾部修改保留 bitcode 哈希，但会改变整个输出的收据。
    // A valid tail-only change retains the bitcode hash but changes the complete output receipt.
    f.binary[f.dynamic + 6] = "S".charCodeAt(0);
    const second = await acceptCompiledShaderOutput(input(f.binary));
    assert.notEqual(first.receipt.outputSha256, second.receipt.outputSha256);
    assert.notEqual(first.receiptSha256, second.receiptSha256);
    assert.equal(
      (await inspectCompiledMetalLibrary(f.binary, options)).functions[0].bitcodeSha256,
      inspected.functions[0].bitcodeSha256,
    );
  });

  for (const [name, mutate, error] of [
    [
      "undeclared suffix",
      (f) => {
        f.binary = concat(f.binary, u32(0));
        f.binary.set(u64(f.binary.length), 16);
      },
      /unaccounted or overlapping declared tail/,
    ],
    [
      "overlapping dynamic section",
      (f) => f.binary.set(u64(f.dynamic - 1), f.extension + 6),
      /unaccounted or overlapping declared tail/,
    ],
    [
      "out-of-bounds reflection section",
      (f) => f.binary.set(u64(f.binary.length), f.extension + 36),
      /section out of bounds/,
    ],
    [
      "debug source declaration",
      (f) => f.binary.set(enc.encode("HSRD"), f.extension + 44),
      /embedded debug source/,
    ],
    [
      "unknown declaration",
      (f) => f.binary.set(enc.encode("FAKE"), f.extension + 44),
      /unsupported declared library tail/,
    ],
    [
      "reflection reference mismatch",
      (f) => f.binary.set(u64(5), f.reference),
      /reflection offset mismatch/,
    ],
    [
      "reflection count mismatch",
      (f) => f.binary.set(u32(2), f.reflection),
      /reflection count mismatch/,
    ],
    [
      "RBUF payload size mismatch",
      (f) => f.binary.set(u32(8), f.group + 8),
      /invalid reflection group/,
    ],
    [
      "reflection alignment padding",
      (f) => {
        assert.ok(f.padding > 0);
        f.binary[f.group + 12] = 1;
      },
      /invalid reflection alignment padding/,
    ],
    [
      "reflection terminator",
      (f) => f.binary.set(enc.encode("SARC"), f.binary.length - 4),
      /invalid reflection terminator/,
    ],
    [
      "AIRR identifier",
      (f) => f.binary.set(enc.encode("MSLS"), f.airr + 4),
      /invalid AIRR reflection envelope/,
    ],
    [
      "reflection root bounds",
      (f) => f.binary.set(u32(0xfffffffc), f.airr),
      /reflection range out of bounds/,
    ],
    [
      "reflection vector bounds",
      (f) => f.binary.set(u32(0xffffffff), f.airr + 44),
      /reflection range out of bounds/,
    ],
    [
      "bitcode digest with valid tail",
      (f) => {
        f.binary[f.bitcode + 7] ^= 1;
      },
      /bitcode digest mismatch/,
    ],
  ])
    test(`rejects declared-tail ${name}`, async () => {
      const f = declaredTailFixture();
      mutate(f);
      await assert.rejects(acceptCompiledShaderOutput(input(f.binary)), error);
    });

  test("continues rejecting legacy libraries with undeclared tails", async () => {
    const binary = concat(fixture(), enc.encode("fragment source text"));
    binary.set(u64(binary.length), 16);
    await assert.rejects(acceptCompiledShaderOutput(input(binary)), /unaccounted library tail/);
  });
  for (const wrapped of [false, true])
    test(`binds source/compiler/options/output for ${wrapped ? "wrapped" : "raw"} bitcode envelope`, async () => {
      const request = input(fixture({ wrapped }));
      const result = await acceptCompiledShaderOutput(request);
      assert.notEqual(result.bytes, request.binary);
      assert.deepEqual(result.bytes, request.binary);
      assert.equal(result.receipt.validation, "container-and-integrity-only");
      assert.equal(result.receipt.outputSha256, hash(request.binary));
      assert.equal(result.receiptSha256, hash(JSON.stringify(result.receipt)));
      assert.equal(result.receipt.sourceSetDigest, request.expected.sourceSetDigest);
      assert.equal(result.receipt.metalCompiled, undefined);
      assert.equal(Object.isFrozen(result.receipt.options.minimumOS), true);
    });
  test("normalizes only CRLF just like the existing source-digest contract", async () => {
    const request = input();
    request.source = request.source.replaceAll("\n", "\r\n");
    assert.equal(
      (await acceptCompiledShaderOutput(request)).receipt.sourceSha256,
      request.expected.sourceSha256,
    );
    request.source += "\r";
    await assert.rejects(acceptCompiledShaderOutput(request), /source digest mismatch/);
  });
  test("snapshots output, identity and options before yielding", async () => {
    const request = input(),
      original = request.binary.slice();
    const pending = acceptCompiledShaderOutput(request);
    request.binary.fill(0);
    request.expected.entry = "wrong";
    request.options.minimumOS[0] = 99;
    request.compiler.id = "wrong";
    const result = await pending;
    assert.deepEqual(result.bytes, original);
    assert.equal(result.receipt.entry, "main0");
    assert.equal(result.receipt.compiler.id, "test-only");
    assert.deepEqual(result.receipt.options.minimumOS, [18, 0]);
  });
  for (const [name, mutate, error] of [
    [
      "source masquerading as metallib",
      (r) => r.binary.set(enc.encode("source")),
      /not a compiled library/,
    ],
    ["macOS target", (r) => (r.binary[11] = 0x81), /not an iOS/],
    ["simulator target", (r) => (r.binary[11] = 0x87), /not an iOS/],
    ["dynamic library", (r) => (r.binary[10] = 2), /not an iOS/],
    ["OS mismatch", (r) => (r.options.minimumOS[0] = 17), /target OS mismatch/],
    ["language mismatch", (r) => (r.options.languageVersion[1] = 4), /language version mismatch/],
    ["wrong entry", (r) => (r.expected.entry = "other"), /entry or stage mismatch/],
    ["wrong stage", (r) => (r.expected.stage = "vertex"), /entry or stage mismatch/],
    [
      "incorrect source digest",
      (r) => (r.expected.sourceSha256 = "2".repeat(64)),
      /source digest mismatch/,
    ],
    ["size mismatch", (r) => (r.binary[16] ^= 1), /file size mismatch/],
    ["unsafe offset", (r) => r.binary.fill(255, 24, 32), /oversized offset/],
    ["section overlap", (r) => r.binary.set(u64(88), 40), /overlapping sections/],
    ["empty function list", (r) => r.binary.set(u32(0), 88), /invalid function count/],
    ["bad group length", (r) => r.binary.set(u32(0xffffffff), 92), /section out of bounds/],
    ["corrupted bitcode", (r) => (r.binary[r.binary.length - 1] ^= 1), /bitcode digest mismatch/],
    ["invalid bitcode magic", (r) => (r.binary[r.binary.length - 8] = 0), /bitcode envelope/],
    [
      "empty public metadata",
      (r) => {
        const v = new DataView(r.binary.buffer);
        r.binary.set(u32(0), Number(v.getBigUint64(40, true)));
      },
      /invalid metadata group length/,
    ],
  ])
    test(`rejects ${name}`, async () => {
      const request = input();
      mutate(request);
      await assert.rejects(acceptCompiledShaderOutput(request), error);
    });
  test("rejects truncated bytes, detached-like empty output, and shared memory", async () => {
    for (const binary of [
      fixture().subarray(0, 87),
      new Uint8Array(),
      new Uint8Array(new SharedArrayBuffer(100)),
    ])
      await assert.rejects(acceptCompiledShaderOutput(input(binary)), /invalid output bytes/);
  });
  test("aborts both before work and while hashes are pending without modifying caller bytes", async () => {
    const controller = new AbortController(),
      request = input();
    request.signal = controller.signal;
    const before = request.binary.slice(),
      pending = acceptCompiledShaderOutput(request);
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    assert.deepEqual(request.binary, before);
    await assert.rejects(acceptCompiledShaderOutput(request), { name: "AbortError" });
  });
  test("changing compiler or fast-math options changes the receipt identity", async () => {
    const a = input(),
      b = input(),
      c = input();
    b.compiler.assetSha256 = "3".repeat(64);
    c.options.fastMath = true;
    const results = await Promise.all([a, b, c].map(acceptCompiledShaderOutput));
    assert.equal(new Set(results.map((r) => r.receiptSha256)).size, 3);
    assert.equal(new Set(results.map((r) => r.receipt.outputSha256)).size, 1);
  });

  test("does not silently omit unknown compiler options from the receipt", async () => {
    const request = input();
    request.options.defines = { CHANGE_SHADER: 1 };
    await assert.rejects(acceptCompiledShaderOutput(request), /invalid compiler options/);
  });

  for (const sectionOffset of [40, 56]) {
    for (const length of [8, 9, 0xffffffff])
      test(`rejects incorrect metadata group length ${length} at ${sectionOffset}`, async () => {
        const request = input();
        const view = new DataView(request.binary.buffer);
        request.binary.set(u32(length), Number(view.getBigUint64(sectionOffset, true)));
        await assert.rejects(
          acceptCompiledShaderOutput(request),
          /metadata group length|section out of bounds/,
        );
      });
  }
  test("target OS has separate uint8 minor and patch fields", async () => {
    const request = input();
    request.binary[14] = 3;
    request.options.minimumOS = [18, 3];
    assert.deepEqual(
      (await acceptCompiledShaderOutput(request)).receipt.options.minimumOS,
      [18, 3],
    );
    request.binary[15] = 1;
    await assert.rejects(
      acceptCompiledShaderOutput(request),
      /unsupported target OS patch version/,
    );
  });

  test("accepts the selected writer's UUID header extension and rejects malformed tags", async () => {
    const original = fixture(),
      view = new DataView(original.buffer);
    const offset = Number(view.getBigUint64(40, true));
    const extension = concat(tag("UUID", new Uint8Array(16)), enc.encode("ENDT"));
    const binary = concat(original.subarray(0, offset), extension, original.subarray(offset));
    binary.set(u64(binary.length), 16);
    for (const field of [40, 56, 72])
      binary.set(u64(Number(view.getBigUint64(field, true)) + extension.length), field);
    await acceptCompiledShaderOutput(input(binary));
    binary.set(u16(0xffff), offset + 4);
    await assert.rejects(acceptCompiledShaderOutput(input(binary)), /section out of bounds/);
  });

  test("production accepts only Xcode metadata lengths; legacy floor layout is diagnostic-only", async () => {
    const binary = fixture(),
      view = new DataView(binary.buffer);
    const options = {
      entry: "main0",
      stage: "fragment",
      minimumOS: [18, 0],
      languageVersion: [2, 3],
      metadataLengthIncludesPrefix: false,
    };
    assert.equal(
      (await inspectCompiledMetalLibrary(binary, options)).validation,
      "container-and-integrity-only",
    );
    await acceptCompiledShaderOutput(input(binary));
    await assert.rejects(
      inspectCompiledMetalLibrary(binary, { ...options, metadataLengthIncludesPrefix: true }),
      /invalid metadata group length/,
    );
    for (const field of [40, 56]) binary.set(u32(8), Number(view.getBigUint64(field, true)));
    await assert.rejects(inspectCompiledMetalLibrary(binary, options), /section out of bounds/);
    await assert.rejects(acceptCompiledShaderOutput(input(binary)), /section out of bounds/);
    const result = await inspectCompiledMetalLibrary(binary, {
      ...options,
      metadataLengthIncludesPrefix: true,
    });
    assert.equal(result.compiler, undefined);
    assert.deepEqual(result.functions[0].airVersion, [2, 6]);
    assert.equal(result.outputSha256, hash(binary));
    await assert.rejects(
      inspectCompiledMetalLibrary(binary, { ...options, metadataLengthIncludesPrefix: undefined }),
      /invalid structural expectations/,
    );
  });
}

// production-shader-memory
{
  // 仅使用合成字节测试预算、分块和生命周期；不构成游戏或浏览器内存测量。
  // Synthetic bytes test budgets, chunking and lifetimes, not game validation or browser memory measurement.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const {
    createShaderOutputBudget,
    createShaderRecordProgress,
    createShaderFinalizationCheckpoint,
    shaderOutputChunks,
    createShaderSourceCache,
  } = productionShadersModule;
  const encode = (value) => new TextEncoder().encode(value);
  const packOwner = (bytes) =>
    Object.freeze({
      dispose() {
        bytes.fill(0);
      },
    });

  test("actual post-chain formatted replay rejects a compact-estimate budget bypass", () => {
    const replay = { bindings: [{ name: "value", slot: 1 }], notes: ["synthetic"] };
    const compact = encode(JSON.stringify(replay));
    const actual = encode(JSON.stringify({ ...replay, chain: "trusted-chain" }, null, 1));
    const base = 20,
      fetch = 10;
    assert.ok(actual.length > compact.length);
    const budget = createShaderOutputBudget(base + fetch + compact.length);
    budget.replace(0, base + fetch + compact.length);
    assert.throws(() => budget.replace(compact.length, actual.length), /budget exceeded/);
    assert.equal(budget.used, base + fetch + compact.length);
  });
  test("base, fetch, final JSON and index count exactly once", () => {
    const budget = createShaderOutputBudget(100);
    budget.replace(0, 20 + 10 + 5);
    budget.replace(5, 30);
    budget.replace(0, 40);
    assert.equal(budget.used, 100);
    assert.throws(() => budget.replace(0, 1), /budget exceeded/);
  });
  test("output chunks own <=64KiB backings and preserve exact bytes across replays", async () => {
    const original = Uint8Array.from({ length: 150000 }, (_, i) => i % 251);
    for (let run = 0; run < 2; run++) {
      let offset = 0,
        checks = 0;
      for await (const chunk of shaderOutputChunks(original, () => checks++)) {
        assert.ok(chunk.buffer.byteLength <= 65536);
        assert.notEqual(chunk.buffer, original.buffer);
        assert.deepEqual(chunk, original.subarray(offset, offset + chunk.length));
        offset += chunk.length;
        chunk.fill(0);
      }
      assert.equal(offset, original.length);
      assert.equal(checks, 3);
    }
  });
  test("cancellation between chunks prevents further allocation/delivery", async () => {
    const controller = new AbortController();
    const iterator = shaderOutputChunks(new Uint8Array(100000), () =>
      controller.signal.throwIfAborted(),
    );
    assert.equal((await iterator.next()).value.length, 65536);
    controller.abort();
    await assert.rejects(iterator.next(), { name: "AbortError" });
  });
  test("noncontiguous embedded uses retain image until final completion and invalidate pack identity", () => {
    const cache = createShaderSourceCache(2),
      image = new Uint8Array([7, 8]);
    const first = new Uint8Array([1, 2]);
    cache.setPack("a", packOwner(first));
    cache.enter({ embedded: true, name: "embedded-a" });
    assert.deepEqual(first, new Uint8Array(2));
    assert.equal(cache.packName, null);
    cache.setImage(image);
    cache.completeEmbedded();
    assert.equal(cache.image, image);
    cache.enter({ embedded: false, name: "a" });
    assert.equal(cache.pack, undefined);
    const second = new Uint8Array([3, 4]);
    cache.setPack("a", packOwner(second));
    cache.enter({ embedded: true, name: "embedded-b" });
    assert.deepEqual(second, new Uint8Array(2));
    assert.equal(cache.image, image);
    cache.completeEmbedded();
    assert.equal(cache.image, undefined);
    assert.deepEqual(image, new Uint8Array(2));
    cache.enter({ embedded: false, name: "a" });
    assert.equal(cache.packName, null);
  });
  test("source switches and failure cleanup wipe only retained owned buffers", () => {
    const cache = createShaderSourceCache(1),
      pack = new Uint8Array([5]),
      image = new Uint8Array([6]);
    cache.setPack("a", packOwner(pack));
    cache.setImage(image);
    cache.enter({ embedded: false, name: "b" });
    assert.equal(pack[0], 0);
    assert.equal(image[0], 6);
    cache.dispose();
    cache.dispose();
    assert.equal(image[0], 0);
    assert.equal(cache.image, undefined);
  });
  test("a pack owner survives same-source entries and is released exactly once on replacement", () => {
    const cache = createShaderSourceCache(0);
    let first = 0,
      second = 0;
    const source = Object.freeze({ dispose: () => first++ });
    cache.setPack("a", source);
    cache.enter({ embedded: false, name: "a" });
    assert.equal(cache.pack, source);
    assert.equal(first, 0);
    cache.setPack("b", Object.freeze({ dispose: () => second++ }));
    assert.equal(first, 1);
    cache.dispose();
    cache.dispose();
    assert.equal(first, 1);
    assert.equal(second, 1);
    assert.equal(cache.pack, undefined);
    assert.equal(cache.packName, null);
  });

  // 执行真实生产循环，验证每个物理包只交接一次，并在下一来源失败时释放两个所有者。
  // Execute the real production loop: hand off each physical pack once and release both owners when the next source fails.
  test("production reuses one owner per pack and releases owners and code on preparation failure", () => {
    const script = `
    import assert from "node:assert/strict";
    import { mock } from "node:test";
    const reads = [], created = [], disposed = [], code = [], compiled = [], accepted = [];
    let translated = 0, compilerDisposed = 0;
    mock.module("./sealed-runtime/clients.mjs", { namedExports: { assertSealedRuntime() {} } });
    mock.module("./shader-catalog-adapter.mjs", { namedExports: { assertShaderCatalog() {} } });
    mock.module("./extraction-job-adapter.mjs", { namedExports: {
      readAuthenticatedAssets() { return { logicalAssets: { async read(path) {
        reads.push(path);
        return new Uint8Array([7, 8]);
      } } }; },
      readAuthenticatedProgramModules() { return {}; }
    } });
    mock.module("./logical-asset-suffixes.mjs", { namedExports: {
      resolveLogicalAssetSuffixes() { return ["data:data/hd/shaders/a", "data:data/hd/shaders/b"]; }
    } });
    mock.module("./shader-pipeline.mjs", { namedExports: { createShaderPipeline() {
      return { async translate() {
        translated++;
        return { msl: "synthetic", replay: { entry: "synthetic_main", msl_entry_kind: "kernel" }, resources: { notes: [] } };
      }, dispose() {} };
    } } });
    // 只测试所有权，此模拟不验证或伪造真实已编译库。
    // Only ownership is exercised; this mock does not validate or fabricate a real compiled library.
    mock.module("./compiled-shader-boundary.mjs", { namedExports: {
      async acceptCompiledShaderOutput({ binary, compiler, expected, signal }) {
        assert.ok(signal instanceof AbortSignal);
        assert.equal(expected.entry, "synthetic_main");
        assert.equal(expected.stage, "kernel");
        assert.match(expected.sourceSha256, /^[a-f0-9]{64}$/);
        const bytes = binary.slice();
        accepted.push(bytes);
        return { bytes, receipt: { compiler }, receiptSha256: "synthetic-cleanup-only" };
      }
    } });
    const { createProductionShaderAdapter } = await import("./production-shaders.mjs");
    const keys = Array.from({ length: 6927 }, (_, i) => i.toString(16).padStart(16, "0"));
    const adapter = createProductionShaderAdapter({
      runtime: {
        workerUrls: {}, createShaderWorkerFactory() {},
        createAirCompilerClient() {
          return {
            async compile(source, { entry, stage, signal }) {
              assert.equal(source, "synthetic");
              assert.equal(entry, "synthetic_main");
              assert.equal(stage, "kernel");
              assert.ok(signal instanceof AbortSignal);
              signal.throwIfAborted();
              const bytes = new Uint8Array([3, 4]);
              compiled.push(bytes);
              return { bytes, compiler: { id: "synthetic-cleanup-only" }, options: {} };
            },
            dispose() { compilerDisposed++; }
          };
        }
      },
      catalog: { keys, bfWant: [], describe(key) {
        const index = parseInt(key, 16);
        return { name: index < 2 ? "a" : "b", index, embedded: false };
      },
      prepare() { throw new Error("Pack must use its owned source"); },
      createOwnedPackSource(name, input) {
        created.push(name);
        const owned = structuredClone(input, { transfer: [input.buffer] });
        assert.equal(input.byteLength, 0);
        return {
          async prepare(key, flags) {
            assert.ok(flags.signal instanceof AbortSignal);
            if (name === "b") throw new Error("synthetic preparation failure");
            const bytes = new Uint8Array([1, 2]);
            code.push(bytes);
            return { job: { stage: "cs" }, code: bytes };
          },
          dispose() { owned.fill(0); disposed.push(name); }
        };
      } },
      trustedShell: { trustedRecipe: { shaderKeys: keys, shaderDigest: "be59c864f087b377" } }
    });
    await assert.rejects(adapter.generateShaders(), (error) => {
      assert.match(error.message, /synthetic preparation failure/);
      assert.deepEqual(error.shaderDiagnostic, { substep: "record-preparation", errorType: "Error", record: 3 });
      assert.doesNotMatch(JSON.stringify(error.shaderDiagnostic), /synthetic preparation failure|data:data/);
      return true;
    });
    assert.deepEqual(reads, ["data:data/hd/shaders/a", "data:data/hd/shaders/b"]);
    assert.deepEqual(created, ["a", "b"]);
    assert.deepEqual(disposed, ["a", "b"]);
    assert.equal(translated, 2);
    assert.equal(compiled.length, 2);
    assert.equal(accepted.length, 2);
    assert.equal(compilerDisposed, 1);
    for (const bytes of code) assert.deepEqual(bytes, new Uint8Array(2));
    for (const bytes of [...compiled, ...accepted]) assert.deepEqual(bytes, new Uint8Array(2));
    adapter.dispose();
    assert.deepEqual(disposed, ["a", "b"]);
    assert.equal(compilerDisposed, 1);
  `;
    const result = spawnSync(
      process.execPath,
      ["--experimental-test-module-mocks", "--input-type=module", "-e", script],
      { cwd: new URL("../", import.meta.url), encoding: "utf8", timeout: 30000 },
    );
    assert.equal(result.status, 0, result.stderr || result.stdout);
  });

  // 模拟异步准备结束时的取消，直接执行生产适配器而非复制其清理逻辑。
  // Simulate cancellation as async preparation settles, executing the production adapter rather than copied cleanup logic.
  test("cancellation after preparation wipes owned code without starting translation", () => {
    const script = `
    import assert from "node:assert/strict";
    import { mock } from "node:test";
    const controller = new AbortController();
    const code = new Uint8Array([9, 8, 7, 6]);
    const independent = code.slice();
    let translated = 0, disposed = 0, prepared = 0, sourceDisposed = 0, compilerDisposed = 0;
    mock.module("./sealed-runtime/clients.mjs", { namedExports: { assertSealedRuntime() {} } });
    mock.module("./shader-catalog-adapter.mjs", { namedExports: { assertShaderCatalog() {} } });
    mock.module("./extraction-job-adapter.mjs", { namedExports: {
      readAuthenticatedAssets() { return { logicalAssets: { async read() { return new Uint8Array([1]); } } }; },
      readAuthenticatedProgramModules() { return {}; }
    } });
    mock.module("./logical-asset-suffixes.mjs", { namedExports: {
      resolveLogicalAssetSuffixes() { return ["data:data/hd/shaders/synthetic"]; }
    } });
    mock.module("./shader-pipeline.mjs", { namedExports: { createShaderPipeline() {
      return { async translate() { translated++; }, dispose() { disposed++; } };
    } } });
    const { createProductionShaderAdapter } = await import("./production-shaders.mjs");
    const keys = Array.from({ length: 6927 }, (_, i) => i.toString(16).padStart(16, "0"));
    const adapter = createProductionShaderAdapter({
      runtime: {
        workerUrls: {}, createShaderWorkerFactory() {},
        createAirCompilerClient() {
          return {
            async compile() { assert.fail("Cancelled preparation must never start compilation"); },
            dispose() { compilerDisposed++; }
          };
        }
      },
      catalog: { keys, describe() { return { name: "synthetic", index: 0, embedded: false }; },
        prepare() { throw new Error("Pack must use its owned source"); },
        createOwnedPackSource() {
          return {
            async prepare() {
              prepared++;
              await Promise.resolve();
              controller.abort();
              return { job: {}, code };
            },
            dispose() { sourceDisposed++; }
          };
        }
      },
      trustedShell: { trustedRecipe: { shaderKeys: keys, shaderDigest: "be59c864f087b377" } }
    });
    await assert.rejects(adapter.generateShaders({ signal: controller.signal }), { name: "AbortError" });
    assert.equal(prepared, 1);
    assert.equal(translated, 0);
    assert.equal(sourceDisposed, 1);
    assert.ok(disposed > 0);
    assert.equal(compilerDisposed, 1);
    assert.deepEqual(code, new Uint8Array(4));
    assert.deepEqual(independent, new Uint8Array([9, 8, 7, 6]));
    adapter.dispose();
    assert.equal(compilerDisposed, 1);
  `;
    const result = spawnSync(
      process.execPath,
      ["--experimental-test-module-mocks", "--input-type=module", "-e", script],
      {
        cwd: new URL("../", import.meta.url),
        encoding: "utf8",
        timeout: 30000,
      },
    );
    assert.equal(result.status, 0, result.stderr || result.stdout);
  });

  // 合成时钟验证节流与最终记录计数，不把计数当作阶段完成。
  // A synthetic clock verifies throttling and the last record count, not stage completion.
  test("shader counters are monotonic, throttled, bounded and observer-isolated", async () => {
    let time = 0;
    const controller = new AbortController(),
      events = [];
    const report = createShaderRecordProgress(
      6,
      (value) => events.push(value),
      controller.signal,
      () => time,
    );
    report(1);
    report(2);
    report(2);
    report(0);
    report(7);
    report(NaN);
    time = 249;
    report(3);
    time = 250;
    report(4);
    report(3);
    report(5);
    report(6);
    report(6);
    assert.deepEqual(
      events,
      [1, 4, 6].map((completedRecords) => ({ completedRecords, totalRecords: 6 })),
    );
    assert.ok(events.every(Object.isFrozen));
    assert.ok(events.every((value) => !Object.hasOwn(value, "state")));
    controller.abort();
    assert.doesNotThrow(() => report(6));
    const cancelled = createShaderRecordProgress(
      1,
      () => assert.fail("cancelled observer"),
      controller.signal,
    );
    cancelled(1);
    createShaderRecordProgress(1, () => {
      throw new Error("private");
    })(1);
    createShaderRecordProgress(1, async () => {
      throw new Error("private async");
    })(1);
    await new Promise((resolve) => setImmediate(resolve));
    for (const total of [0, 6928, Infinity, 1.5])
      assert.throws(() => createShaderRecordProgress(total));
  });

  test("finalization yields on byte or monotonic work budget without per-row tasks", async () => {
    let time = 0,
      tasks = 0,
      checks = 0;
    const checkpoint = createShaderFinalizationCheckpoint(
      () => checks++,
      undefined,
      async () => {
        tasks++;
      },
      () => time,
    );
    for (let i = 0; i < 3; i++) assert.equal(checkpoint(1024 * 1024), undefined);
    assert.equal(tasks, 0);
    await checkpoint(1024 * 1024);
    assert.equal(tasks, 1);
    time = 7;
    assert.equal(checkpoint(0), undefined);
    time = 8;
    await checkpoint(0);
    assert.equal(tasks, 2);
    assert.ok(checks >= 8);
    assert.throws(() => checkpoint(-1));
  });

  test("cancellation delivered by a finalization task rejects before continuing", async () => {
    const controller = new AbortController();
    const checkpoint = createShaderFinalizationCheckpoint(
      () => controller.signal.throwIfAborted(),
      controller.signal,
      async ({ signal }) => {
        assert.equal(signal, controller.signal);
        controller.abort();
      },
      () => 0,
    );
    await assert.rejects(checkpoint(4 * 1024 * 1024), { name: "AbortError" });
    assert.throws(() => checkpoint(1), { name: "AbortError" });
  });

  // 检查让出位置仍位于完整证明之后；结果发布前必须完成最后取消检查。
  // Check yields remain after complete proof, with a final cancellation check before publishing results.
  test("production finalization preserves proof-first ordering and signal-aware record yields", async () => {
    const { readFile } = await import("node:fs/promises");
    const source = await readFile(new URL("../production-shaders.mjs", import.meta.url), "utf8");
    assert.ok(
      source.indexOf("await pipeline.verifyCompleteSourceSet()") <
        source.indexOf("const checkpoint = createShaderFinalizationCheckpoint"),
    );
    assert.match(source, /await yieldBrowserTask\(\{ signal: controller.signal \}\)/);
    assert.doesNotMatch(source, /setTimeout\(resolve, 0\)/);
    assert.match(
      source,
      /entries.push\(entry\("dm\/c1_full.tsv", index\)\);\s*check\(\);\s*return entries/,
    );
    assert.match(source, /const pause = checkpoint\(json.length\);\s*if \(pause\) await pause/);
  });

  // 直接约束所有权登记顺序，使预算拒绝同样擦除刚分配的元数据。
  // Constrain ownership registration order so budget rejection also wipes newly allocated metadata.
  test("finalization owns replay and index bytes before a budget check can reject them", async () => {
    const { readFile } = await import("node:fs/promises");
    const source = await readFile(new URL("../production-shaders.mjs", import.meta.url), "utf8");
    assert.match(
      source,
      /const json = encoder\.encode\(JSON\.stringify\(row\.replay, null, 1\)\);\s*retained\.push\(json\);\s*budget\.replace\(row\.replayReservation, json\.length\)/,
    );
    assert.match(source, /retained\.push\(index\);\s*budget\.replace\(0, index\.length\)/);
  });
}

// shader-catalog-adapter
{
  // 依据固定着色器目录和合成标记数据验证目录解析与记录边界。
  // Verify catalog parsing and record boundaries using pinned shader metadata and synthetic markers.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFileSync } = nodeFsModule;
  const { createHash } = nodeCryptoModule;
  const { spawnSync } = nodeChild_processModule;
  const { loadShaderCatalog, decodeShaderRecord } = shaderCatalogAdapterModule;
  const root = new URL("../../", import.meta.url);
  const input = () =>
    Object.fromEntries(
      [
        ["map", "shader_map.tsv"],
        ["facts", "shader_facts.tsv"],
        ["conflicts", "shader_map.tsv.conflicts.tsv"],
      ].map(([k, f]) => [k, new Uint8Array(readFileSync(new URL(`dm/gfx/nvn/${f}`, root)))]),
    );
  test("pinned full catalog matches original first-source and conflict selection, retaining unsupported stages", async () => {
    const catalog = await loadShaderCatalog(input());
    const script = `import ast,json,sys\np=sys.argv[1]\nt=ast.parse(open(p).read())\nstart=next(i for i,n in enumerate(t.body) if isinstance(n,ast.Assign) and any(isinstance(x,ast.Tuple) and any(isinstance(y,ast.Name) and y.id=='pc_of' for y in x.elts) for x in n.targets))\nend=next(i for i,n in enumerate(t.body) if isinstance(n,ast.FunctionDef) and n.name=='main_rec')\ng={'MAP':sys.argv[2]}\nexec(compile(ast.Module(body=t.body[start:end],type_ignores=[]),p,'exec'),g)\nprint(json.dumps([{'key':k,'name':n,'index':i,'pcPaired':g['pc_of'][k],'stage':g['stage_of'].get(k,n[:2])} for k,(n,i) in g['loc'].items()]))`;
    const native = spawnSync(
      "python3",
      [
        "-c",
        script,
        new URL("dm/gfx/tools/dmsh_build.py", root).pathname,
        new URL("dm/gfx/nvn/shader_map.tsv", root).pathname,
      ],
      { encoding: "utf8", maxBuffer: 8 * 1024 * 1024 },
    );
    assert.equal(native.status, 0, native.stderr);
    assert.deepEqual(
      catalog.keys.map((key) => {
        const { name, index, pcPaired, stage } = catalog.describe(key);
        return { key, name, index, pcPaired, stage };
      }),
      JSON.parse(native.stdout),
    );
    assert.equal(catalog.keys.length, 6927);
    assert.equal(catalog.keys.filter((k) => catalog.describe(k).embedded).length, 4);
    assert.deepEqual(
      catalog.unsupported.map((x) => x.key),
      ["14b6f7ad94a21328", "5221043a88afa381"],
    );
    for (const e of catalog.unsupported)
      await assert.rejects(
        catalog.prepare(e.key, { debug: true, reduced: true }),
        /Unsupported catalog stage/,
      );
    assert.equal(catalog.productionEquivalent, false);
    const copy = catalog.describe(catalog.keys[0]);
    copy.facts.tex.injected = "bad";
    assert.equal(catalog.describe(copy.key).facts.tex.injected, undefined);
  });
  test("tampered source and missing pack cannot become a job", async () => {
    for (const name of ["map", "facts", "conflicts"]) {
      const data = input();
      data[name][0] ^= 1;
      await assert.rejects(loadShaderCatalog(data), /source digest mismatch/);
    }
    const catalog = await loadShaderCatalog(input());
    await assert.rejects(
      catalog.prepare(catalog.keys[0], {
        packBytes: new Uint8Array(32),
        debug: true,
        reduced: true,
      }),
      /record absent/,
    );
    await assert.rejects(
      catalog.prepare(catalog.keys[0], { packBytes: new Uint8Array(32) }),
      /Explicit/,
    );
  });
  test("pinned catalog sources retain their checked bytes when Buffer inputs change during hashing", async (t) => {
    const expected = await loadShaderCatalog(input());
    const expectedEntries = expected.keys.map((key) => expected.describe(key));
    for (const [name, from, to] of [
      ["map", "cs_assao_clear_load_counter_c.bin", "cs_xssao_clear_load_counter_c.bin"],
      ["facts", '"2d"', '"3d"'],
      ["conflicts", "67c858efc3fcf781", "07c858efc3fcf781"],
    ])
      await t.test(name, async (t) => {
        const data = input();
        const backing = Buffer.alloc(data[name].length + 2, 0xa5);
        const borrowed = backing.subarray(1, -1);
        borrowed.set(data[name]);
        data[name] = borrowed;
        const at = borrowed.indexOf(from);
        assert.ok(at >= 0);
        const nativeDigest = crypto.subtle.digest.bind(crypto.subtle);
        let mutated = false;
        t.mock.method(crypto.subtle, "digest", async (algorithm, bytes) => {
          const digest = await nativeDigest(algorithm, bytes);
          if (bytes.byteLength === borrowed.byteLength) {
            borrowed.write(to, at);
            mutated = true;
          }
          return digest;
        });
        const actual = await loadShaderCatalog(data);
        assert.equal(mutated, true);
        assert.deepEqual(
          actual.keys.map((key) => actual.describe(key)),
          expectedEntries,
        );
        assert.equal(backing[0], 0xa5);
        assert.equal(backing.at(-1), 0xa5);
      });
  });
  function fixture(stage, c1Length) {
    const length = (stage === "cs" ? 16 : 0x90) + c1Length;
    const rec = new Uint8Array(0x38 + length + 0x880);
    const ci = 0x38 + length;
    rec.set(new TextEncoder().encode("BTCDBTCD"));
    if (stage !== "cs")
      new DataView(rec.buffer).setUint32(0x68, (stage === "vs" ? 1 : 5) << 10, true);
    rec.set([0x34, 0x12, 0x76, 0x98], ci);
    const v = new DataView(rec.buffer);
    v.setUint32(ci + 0x700, length - c1Length, true);
    v.setUint32(ci + 0x704, length, true);
    const key = createHash("sha256")
      .update(rec.subarray(ci, ci + 0x880))
      .digest("hex")
      .slice(0, 16);
    return {
      rec,
      descriptor: {
        key,
        stage,
        name: `${stage}_synthetic.bin`,
        index: 0,
        pcPaired: "0000000000000000",
        facts: { attr: {}, fout: {}, tex: {}, img: {} },
      },
    };
  }
  test("authored cs/vs/ps records and c1 match original native job_for exactly", async () => {
    for (const stage of ["cs", "vs", "ps"])
      for (const size of [0, 256, 512, 768]) {
        const { rec, descriptor } = fixture(stage, size);
        const actual = await decodeShaderRecord(descriptor, rec, { debug: true, reduced: false });
        const script = `import ast,base64,json,os,struct,sys,tempfile\na=json.load(sys.stdin);p=sys.argv[1];t=ast.parse(open(p).read());f=next(n for n in t.body if isinstance(n,ast.FunctionDef) and n.name=='job_for');g={'struct':struct,'os':os,'stage_of':{},'FACTS':{a['key']:a['facts']}};os.environ['XLATE_DEBUG']='1';os.environ['XLATE_REDUCED']='0';exec(compile(ast.Module(body=[f],type_ignores=[]),p,'exec'),g)\nwith tempfile.TemporaryDirectory() as d:\n j,c=g['job_for'](a['key'],a['name'],base64.b64decode(a['rec']),d);code=open(j.pop('code'),'rb').read();print(json.dumps({'job':j,'code':base64.b64encode(code).decode()}))`;
        const native = spawnSync(
          "python3",
          ["-c", script, new URL("dm/gfx/tools/dmsh_build.py", root).pathname],
          {
            input: JSON.stringify({ ...descriptor, rec: Buffer.from(rec).toString("base64") }),
            encoding: "utf8",
          },
        );
        assert.equal(native.status, 0, native.stderr);
        const expected = JSON.parse(native.stdout);
        assert.deepEqual(actual.job, expected.job);
        assert.equal(Buffer.from(actual.code).toString("base64"), expected.code);
        rec[rec.length - 1] ^= 1;
        await assert.rejects(
          decodeShaderRecord(descriptor, rec, { debug: true, reduced: false }),
          /Control hash/,
        );
      }
  });
  test("record decoding owns exact copies of Buffer and Uint8Array subviews", async (t) => {
    for (const stage of ["cs", "vs", "ps"])
      for (const kind of ["uint8-subview", "buffer", "buffer-subview"])
        await t.test(`${stage} ${kind}`, async () => {
          const { rec, descriptor } = fixture(stage, 256);
          rec[stage === "cs" ? 0x38 : 0x70] = 7;
          rec[rec.length - 0x880 - 256] = 9;
          const offset = kind === "buffer" ? 0 : 13;
          const backing = new Uint8Array(rec.length + offset * 2).fill(0xa5);
          backing.set(rec, offset);
          const borrowed =
            kind === "uint8-subview"
              ? backing.subarray(offset, offset + rec.length)
              : Buffer.from(backing.buffer, offset, rec.length);
          const before = backing.slice();
          const result = await decodeShaderRecord(descriptor, borrowed, {
            debug: true,
            reduced: false,
          });
          const start = stage === "cs" ? 0x38 : 0x68;
          assert.deepEqual(result.code, rec.subarray(start, rec.length - 0x880));
          assert.equal(result.job.c1, "09" + "00".repeat(255));
          assert.equal(result.code.byteOffset, 0);
          assert.equal(result.code.byteLength, result.code.buffer.byteLength);
          assert.notEqual(result.code.buffer, borrowed.buffer);
          assert.deepEqual(backing, before);
          borrowed.fill(0);
          assert.deepEqual(result.code, rec.subarray(start, rec.length - 0x880));
        });
  });
  test("record Buffer mutation during hashing cannot change the checked snapshot", async (t) => {
    const { rec, descriptor } = fixture("vs", 256);
    rec[0x70] = 7;
    const backing = Buffer.alloc(rec.length + 2, 0xa5);
    const borrowed = backing.subarray(1, -1);
    borrowed.set(rec);
    const nativeDigest = crypto.subtle.digest.bind(crypto.subtle);
    t.mock.method(crypto.subtle, "digest", async (algorithm, bytes) => {
      const digest = await nativeDigest(algorithm, bytes);
      borrowed.fill(0x55);
      return digest;
    });
    const result = await decodeShaderRecord(descriptor, borrowed, { debug: true, reduced: false });
    assert.deepEqual(result.code, rec.subarray(0x68, rec.length - 0x880));
    assert.ok(borrowed.every((byte) => byte === 0x55));
    assert.equal(backing[0], 0xa5);
    assert.equal(backing.at(-1), 0xa5);
  });
  test("failed record decoding does not wipe borrowed Buffer storage", async () => {
    const { rec, descriptor } = fixture("cs", 0);
    const backing = Buffer.alloc(rec.length + 2, 0xa5);
    const borrowed = backing.subarray(1, -1);
    borrowed.set(rec);
    const before = Buffer.from(backing);
    descriptor.key = "0".repeat(16);
    await assert.rejects(
      decodeShaderRecord(descriptor, borrowed, { debug: true, reduced: false }),
      /Control hash/,
    );
    assert.deepEqual(backing, before);
  });
  test("record snapshots always wipe without altering borrowed input or successful code", async (t) => {
    const originalSlice = Uint8Array.prototype.slice;
    const nativeDigest = crypto.subtle.digest.bind(crypto.subtle);
    for (const outcome of ["success", "control", "stage", "facts"]) {
      const { rec, descriptor } = fixture("vs", 0);
      rec[0x70] = 7;
      if (outcome === "control") descriptor.key = "0".repeat(16);
      if (outcome === "stage") new DataView(rec.buffer).setUint32(0x68, 0, true);
      if (outcome === "facts") descriptor.facts = null;
      const before = rec.slice();
      let snapshot, code;
      const digest = t.mock.method(crypto.subtle, "digest", (algorithm, data) => {
        snapshot = new Uint8Array(data.buffer);
        return nativeDigest(algorithm, data);
      });
      Uint8Array.prototype.slice = function (...args) {
        const result = originalSlice.apply(this, args);
        if (this.buffer === snapshot?.buffer) code = result;
        return result;
      };
      try {
        const pending = decodeShaderRecord(descriptor, rec, { debug: true, reduced: false });
        if (outcome === "success") {
          const result = await pending;
          assert.equal(result.code, code);
          assert.notEqual(result.code.buffer, snapshot.buffer);
          assert.deepEqual(result.code, before.subarray(0x68, before.length - 0x880));
          assert.equal(result.code[8], 7);
        } else {
          await assert.rejects(
            pending,
            outcome === "control"
              ? /Control hash/
              : outcome === "stage"
                ? /SPH stage/
                : /Missing explicit facts/,
          );
          if (code) assert.ok(code.every((byte) => byte === 0));
        }
        assert.ok(snapshot.every((byte) => byte === 0));
        assert.deepEqual(rec, before);
      } finally {
        Uint8Array.prototype.slice = originalSlice;
        digest.mock.restore();
      }
    }
  });

  test("invalid c1 and SPH fail closed even with a matching control hash", async () => {
    for (const kind of ["c1", "sph"]) {
      const { rec, descriptor } = fixture("vs", 0);
      if (kind === "c1") {
        const ci = rec.length - 0x880;
        new DataView(rec.buffer).setUint32(ci + 0x704, 0xffffffff, true);
        descriptor.key = createHash("sha256").update(rec.subarray(ci)).digest("hex").slice(0, 16);
      } else new DataView(rec.buffer).setUint32(0x68, 0, true);
      await assert.rejects(
        decodeShaderRecord(descriptor, rec, { debug: true, reduced: true }),
        kind === "c1" ? /c1 range/ : /SPH stage/,
      );
    }
  });
  test("control search skips partial candidates without skipping the first full marker", async () => {
    const { rec, descriptor } = fixture("cs", 0);
    rec.set([0x34, 0x34, 0x12, 0x76], 0x38);
    const actual = await decodeShaderRecord(descriptor, rec, { debug: true, reduced: false });
    assert.deepEqual(actual.code, rec.slice(0x38, rec.length - 0x880));
    rec.set([0x34, 0x12, 0x76, 0x98], 0x38);
    await assert.rejects(
      decodeShaderRecord(descriptor, rec, { debug: true, reduced: false }),
      /Control hash/,
    );
  });
  test("control search rejects a partial marker at the end of the record", async () => {
    const { rec, descriptor } = fixture("cs", 0);
    rec[rec.length - 0x880] = 0;
    rec.set([0x34, 0x12, 0x76], rec.length - 3);
    await assert.rejects(
      decodeShaderRecord(descriptor, rec, { debug: true, reduced: false }),
      /Missing\/truncated shader control segment/,
    );
  });
  test("pack record search keeps native non-overlapping marker boundaries", async () => {
    const catalog = await loadShaderCatalog(input());
    const key = catalog.keys.find((candidate) => {
      const descriptor = catalog.describe(candidate);
      return !descriptor.embedded && descriptor.index === 1 && descriptor.stage === "cs";
    });
    assert.ok(key);
    // 仅有一个不重叠标记；若错误地逐字节推进，偏移 4 会被当作第二条记录。
    // Only one non-overlapping marker exists; advancing bytewise would count offset 4 as a second record.
    const packText = "BTCDBTCDBTCD";
    assert.equal(packText.indexOf("BTCDBTCD", 1), 4);
    assert.equal(packText.indexOf("BTCDBTCD", 8), -1);
    const packBytes = new TextEncoder().encode(packText);
    await assert.rejects(
      catalog.prepare(key, { packBytes, debug: true, reduced: false }),
      /Catalog record absent from pack/,
    );
  });

  const ownedFlags = Object.freeze({ debug: true, reduced: false });
  function ownedFixture(catalog, name = "cs_lut_combine_c.bin") {
    const descriptors = catalog.keys
      .map((key) => catalog.describe(key))
      .filter((entry) => entry.name === name)
      .sort((a, b) => a.index - b.index);
    assert.ok(descriptors.length);
    const records = descriptors.map((descriptor) => {
      const { rec } = fixture(descriptor.stage, 0);
      // 每个合成控制段均有独立内容，不伪造真实目录或扩大摘要模拟范围。
      // Distinguish synthetic controls without replacing the pinned catalog or broadening the digest mock.
      new DataView(rec.buffer).setUint32(rec.length - 0x880 + 8, descriptor.index + 1, true);
      if (descriptor.stage === "cs") rec[0x38] = descriptor.index + 1;
      return rec;
    });
    return { descriptors, records };
  }
  function joinedBytes(parts) {
    const result = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
    let at = 0;
    for (const part of parts) {
      result.set(part, at);
      at += part.length;
    }
    return result;
  }
  function mockSyntheticControls(t, { descriptors, records }, beforeReturn = async () => {}) {
    const nativeDigest = crypto.subtle.digest.bind(crypto.subtle);
    const controls = new Map(
      records.map((record, i) => [
        Buffer.from(record.subarray(record.length - 0x880)).toString("hex"),
        descriptors[i].key,
      ]),
    );
    let calls = 0;
    t.mock.method(crypto.subtle, "digest", async (algorithm, data) => {
      const key =
        algorithm === "SHA-256" && data.byteLength === 0x880
          ? controls.get(Buffer.from(data).toString("hex"))
          : undefined;
      if (!key) return nativeDigest(algorithm, data);
      calls++;
      await beforeReturn();
      return Uint8Array.from(Buffer.from(key.padEnd(64, "0"), "hex")).buffer;
    });
    return {
      get calls() {
        return calls;
      },
    };
  }
  function observeOwnedStorage(t) {
    const nativeClone = globalThis.structuredClone;
    const NativeOffsets = Uint32Array;
    const transferred = [];
    const offsets = [];
    t.mock.method(globalThis, "structuredClone", (value, options) => {
      const result = nativeClone(value, options);
      if (options?.transfer?.length) {
        const buffer = result instanceof ArrayBuffer ? result : result.buffer;
        assert.ok(buffer instanceof ArrayBuffer);
        transferred.push(new Uint8Array(buffer));
      }
      return result;
    });
    t.mock.method(globalThis, "Uint32Array", function (...args) {
      const result = new NativeOffsets(...args);
      offsets.push(result);
      return result;
    });
    return { transferred, offsets };
  }
  test("public catalog prepare keeps borrowed subviews attached", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const generated = ownedFixture(catalog);
    mockSyntheticControls(t, generated);
    const pack = joinedBytes(generated.records);
    const backing = new Uint8Array(pack.length + 2);
    backing.set(pack, 1);
    const borrowed = backing.subarray(1, -1);
    const result = await catalog.prepare(generated.descriptors[1].key, {
      ...ownedFlags,
      packBytes: borrowed,
    });
    assert.deepEqual(borrowed, pack);
    assert.equal(backing.byteLength, pack.length + 2);
    assert.equal(result.code[0], 2);
  });
  test("owned pack takes exclusive transferred storage and exposes only a frozen source", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const generated = ownedFixture(catalog);
    const digest = mockSyntheticControls(t, generated);
    const storage = observeOwnedStorage(t);
    const pack = joinedBytes(generated.records);
    const alias = new Uint8Array(pack.buffer);
    const view = new DataView(pack.buffer);
    const independent = pack.slice();
    const source = catalog.createOwnedPackSource(generated.descriptors[0].name, pack);
    try {
      assert.equal(pack.byteLength, 0);
      assert.equal(alias.byteLength, 0);
      assert.throws(() => view.getUint8(0), TypeError);
      assert.ok(Object.isFrozen(source));
      assert.deepEqual(Reflect.ownKeys(source).sort(), ["dispose", "prepare"]);
      assert.equal(storage.transferred.length, 1);
      assert.deepEqual(storage.transferred[0], independent);
      assert.equal(storage.offsets.length, 1);
      assert.equal(storage.offsets[0].length, 3);
      assert.ok(
        storage.offsets[0].every((value) => value === 0),
        "offsets are populated lazily",
      );
      const descriptor = generated.descriptors[0];
      const prepared = await source.prepare(descriptor.key, {
        ...ownedFlags,
        packBytes: new Uint8Array(1),
      });
      assert.equal(prepared.job.key, descriptor.key);
      assert.equal(prepared.job.stage, descriptor.stage);
      assert.equal(prepared.source.name, descriptor.name);
      assert.equal(prepared.source.index, descriptor.index);
      assert.equal(prepared.source.pcPaired, descriptor.pcPaired);
      assert.equal(prepared.source.controlVerified, true);
      assert.equal(prepared.installable, false);
      assert.equal(digest.calls, 1);
    } finally {
      source.dispose();
    }
    source.dispose();
    assert.ok(storage.transferred[0].every((value) => value === 0));
    assert.ok(storage.offsets.length > 0);
    assert.ok(storage.offsets.every((table) => table.every((value) => value === 0)));
    assert.ok(independent.some((value) => value !== 0));
    await assert.rejects(source.prepare(generated.descriptors[0].key, ownedFlags), /disposed/i);
    assert.equal(digest.calls, 1);
  });
  test("owned pack rejects unknown or embedded names and non-exclusive backing stores without detaching", async () => {
    const catalog = await loadShaderCatalog(input());
    const name = catalog.describe(catalog.keys[0]).name;
    const embedded = catalog.keys
      .map((key) => catalog.describe(key))
      .find((entry) => entry.embedded);
    for (const invalidName of ["missing.bin", embedded.name, "../cs_lut_combine_c.bin", null]) {
      const pack = new Uint8Array([1, 2, 3]);
      assert.throws(() => catalog.createOwnedPackSource(invalidName, pack));
      assert.deepEqual(pack, new Uint8Array([1, 2, 3]));
    }
    const full = new Uint8Array([1, 2, 3, 4]);
    const shared = new Uint8Array(new SharedArrayBuffer(4));
    const resizable = new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 }));
    for (const pack of [
      new Uint8Array(),
      new Uint16Array(2),
      [1, 2],
      full.subarray(1),
      full.subarray(0, 3),
      shared,
      resizable,
    ]) {
      assert.throws(() => catalog.createOwnedPackSource(name, pack));
    }
    assert.deepEqual(full, new Uint8Array([1, 2, 3, 4]));
    assert.equal(shared.byteLength, 4);
    assert.equal(resizable.byteLength, 4);
    const detached = new Uint8Array(4);
    structuredClone(detached.buffer, { transfer: [detached.buffer] });
    assert.throws(() => catalog.createOwnedPackSource(name, detached));
    const oversized = new Uint8Array(256 * 1024 * 1024 + 1);
    assert.throws(() => catalog.createOwnedPackSource(name, oversized));
    assert.equal(oversized.byteLength, 256 * 1024 * 1024 + 1);
  });
  test("owned pack transfer failures have no copy fallback", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const name = catalog.describe(catalog.keys[0]).name;
    const pack = new Uint8Array([1, 2, 3]);
    const failure = new Error("synthetic transfer unavailable");
    const clone = t.mock.method(globalThis, "structuredClone", () => {
      throw failure;
    });
    assert.throws(() => catalog.createOwnedPackSource(name, pack));
    assert.equal(clone.mock.callCount(), 1);
    assert.deepEqual(pack, new Uint8Array([1, 2, 3]));
  });
  test("owned pack index allocation failure leaves caller ownership intact", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const name = catalog.describe(catalog.keys[0]).name;
    const pack = new Uint8Array([1, 2, 3]);
    const failure = new Error("synthetic offset allocation failure");
    const clone = t.mock.method(globalThis, "structuredClone");
    t.mock.method(globalThis, "Uint32Array", function () {
      throw failure;
    });
    assert.throws(() => catalog.createOwnedPackSource(name, pack), failure);
    assert.equal(clone.mock.callCount(), 0);
    assert.deepEqual(pack, new Uint8Array([1, 2, 3]));
  });
  test("owned pack binds its name and catalog key while preserving explicit decoder flags", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const generated = ownedFixture(catalog);
    const digest = mockSyntheticControls(t, generated);
    const name = generated.descriptors[0].name;
    const storage = observeOwnedStorage(t);
    const embedded = catalog.keys
      .map((key) => catalog.describe(key))
      .find((entry) => entry.embedded);
    const other = catalog.keys
      .map((key) => catalog.describe(key))
      .find((entry) => !entry.embedded && entry.name !== name);
    for (const [key, flags, expected] of [
      ["0000000000000000", ownedFlags, /pack source/i],
      [embedded.key, ownedFlags, /pack source/i],
      [other.key, ownedFlags, /pack source/i],
      [generated.descriptors[0].key, undefined, /Explicit/],
    ]) {
      const source = catalog.createOwnedPackSource(name, joinedBytes(generated.records));
      try {
        await assert.rejects(source.prepare(key, flags), expected);
        assert.ok(storage.transferred.at(-1).every((value) => value === 0));
        await assert.rejects(source.prepare(generated.descriptors[0].key, ownedFlags), /disposed/i);
      } finally {
        source.dispose();
      }
    }
    assert.equal(digest.calls, 0);
    const source = catalog.createOwnedPackSource(name, joinedBytes(generated.records));
    t.after(() => source.dispose());
    const result = await source.prepare(generated.descriptors[0].key, {
      debug: false,
      reduced: true,
    });
    assert.equal(result.job.debug, false);
    assert.equal(result.job.reduced, true);
  });
  test("owned pack indexes lazily within the selected catalog bound and decodes repeated out-of-order requests", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const generated = ownedFixture(catalog);
    assert.deepEqual(
      generated.descriptors.map((entry) => entry.index),
      [0, 1],
    );
    const digest = mockSyntheticControls(t, generated);
    const storage = observeOwnedStorage(t);
    const suffix = new TextEncoder().encode("BTCDBTCD".repeat(20000));
    const source = catalog.createOwnedPackSource(
      generated.descriptors[0].name,
      joinedBytes([...generated.records, suffix]),
    );
    t.after(() => source.dispose());
    assert.equal(storage.offsets.length, 1);
    assert.ok(storage.offsets[0].every((value) => value === 0));
    const nativeIndexOf = Uint8Array.prototype.indexOf;
    let packSearches = 0;
    t.mock.method(Uint8Array.prototype, "indexOf", function (...args) {
      if (this.buffer === storage.transferred[0].buffer) packSearches++;
      return nativeIndexOf.apply(this, args);
    });
    const last = await source.prepare(generated.descriptors[1].key, ownedFlags);
    assert.equal(last.code[0], 2);
    assert.ok(storage.offsets.length > 0);
    assert.ok(storage.offsets.every((table) => table.length <= 3));
    assert.ok(packSearches <= 3, "unused pack markers must not extend the catalog-bounded scan");
    const searches = packSearches;
    const tables = storage.offsets.length;
    last.code.fill(0);
    const first = await source.prepare(generated.descriptors[0].key, ownedFlags);
    const repeated = await source.prepare(generated.descriptors[1].key, ownedFlags);
    assert.equal(first.code[0], 1);
    assert.equal(repeated.code[0], 2);
    assert.notEqual(repeated.code.buffer, last.code.buffer);
    assert.equal(digest.calls, 3, "every prepare must re-run record integrity checks");
    assert.equal(packSearches, searches, "indexed requests must not rescan the pack");
    assert.equal(storage.offsets.length, tables);
  });
  test("owned pack preserves non-overlapping record markers and the final EOF boundary", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const generated = ownedFixture(catalog);
    mockSyntheticControls(t, generated);
    const name = generated.descriptors[0].name;
    for (const prefix of [generated.records[0], new TextEncoder().encode("BTCDBTCDBTCD\0")]) {
      const source = catalog.createOwnedPackSource(
        name,
        joinedBytes([prefix, generated.records[1]]),
      );
      try {
        const result = await source.prepare(generated.descriptors[1].key, ownedFlags);
        assert.deepEqual(result.code, generated.records[1].slice(0x38, -0x880));
      } finally {
        source.dispose();
      }
    }
    const absent = catalog.createOwnedPackSource(name, new TextEncoder().encode("BTCDBTCDBTCD"));
    try {
      await assert.rejects(
        absent.prepare(generated.descriptors[1].key, ownedFlags),
        /record absent/,
      );
    } finally {
      absent.dispose();
    }
  });
  test("owned pack retains real control-hash rejection and permits failure cleanup", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const generated = ownedFixture(catalog);
    const storage = observeOwnedStorage(t);
    const source = catalog.createOwnedPackSource(
      generated.descriptors[0].name,
      joinedBytes(generated.records),
    );
    try {
      // 此处使用真实摘要，合成控制段绝不能被认作固定目录中的游戏记录。
      // Real hashing here must reject synthetic controls as pinned catalog records.
      await assert.rejects(
        source.prepare(generated.descriptors[0].key, ownedFlags),
        /Control hash/,
      );
      assert.ok(storage.transferred[0].every((value) => value === 0));
      assert.ok(storage.offsets.every((table) => table.every((value) => value === 0)));
    } finally {
      source.dispose();
    }
    assert.ok(storage.transferred[0].every((value) => value === 0));
    assert.ok(storage.offsets.every((table) => table.every((value) => value === 0)));
    await assert.rejects(source.prepare(generated.descriptors[0].key, ownedFlags), /disposed/i);
  });
  test("owned pack retains stage and c1 guards after a synthetic matching control hash", async (t) => {
    const catalog = await loadShaderCatalog(input());
    for (const kind of ["c1", "stage"]) {
      await t.test(kind, async (t) => {
        const generated = ownedFixture(catalog, "ps_alpha_threshold_p.bin");
        const record = generated.records[0];
        const view = new DataView(record.buffer);
        if (kind === "c1") view.setUint32(record.length - 0x880 + 0x704, 0xffffffff, true);
        else view.setUint32(0x68, 0, true);
        mockSyntheticControls(t, generated);
        const source = catalog.createOwnedPackSource(generated.descriptors[0].name, record);
        t.after(() => source.dispose());
        await assert.rejects(
          source.prepare(generated.descriptors[0].key, ownedFlags),
          kind === "c1" ? /c1 range/ : /SPH stage/,
        );
      });
    }
  });
  test("owned pack rejects already-aborted requests before decoding", async (t) => {
    const catalog = await loadShaderCatalog(input());
    const generated = ownedFixture(catalog);
    const digest = mockSyntheticControls(t, generated);
    const storage = observeOwnedStorage(t);
    const source = catalog.createOwnedPackSource(
      generated.descriptors[0].name,
      joinedBytes(generated.records),
    );
    t.after(() => source.dispose());
    const controller = new AbortController();
    controller.abort();
    await assert.rejects(
      source.prepare(generated.descriptors[0].key, { ...ownedFlags, signal: controller.signal }),
      { name: "AbortError" },
    );
    assert.equal(digest.calls, 0);
    assert.ok(storage.transferred[0].every((value) => value === 0));
    assert.ok(storage.offsets.every((table) => table.every((value) => value === 0)));
    await assert.rejects(source.prepare(generated.descriptors[0].key, ownedFlags), /disposed/i);
  });
  test("owned pack wipes late successful code when disposal or cancellation races decoding", async (t) => {
    const catalog = await loadShaderCatalog(input());
    for (const mode of ["dispose", "abort"]) {
      await t.test(mode, async (t) => {
        const generated = ownedFixture(catalog);
        const reached = Promise.withResolvers();
        const gate = Promise.withResolvers();
        mockSyntheticControls(t, generated, async () => {
          reached.resolve();
          await gate.promise;
        });
        const storage = observeOwnedStorage(t);
        const codeCopies = [];
        const nativeSlice = Uint8Array.prototype.slice;
        t.mock.method(Uint8Array.prototype, "slice", function (...args) {
          const result = nativeSlice.apply(this, args);
          if (args[0] === 0x38 && result.length === 16) codeCopies.push(result);
          return result;
        });
        const controller = new AbortController();
        const source = catalog.createOwnedPackSource(
          generated.descriptors[0].name,
          joinedBytes(generated.records),
        );
        t.after(() => source.dispose());
        const pending = source.prepare(generated.descriptors[0].key, {
          ...ownedFlags,
          signal: controller.signal,
        });
        await reached.promise;
        if (mode === "dispose") source.dispose();
        else controller.abort();
        gate.resolve();
        await assert.rejects(pending, mode === "dispose" ? /disposed/i : { name: "AbortError" });
        assert.equal(codeCopies.length, 1, "decoder completed after the pending hash settled");
        assert.ok(codeCopies[0].every((value) => value === 0));
        source.dispose();
        assert.ok(storage.transferred[0].every((value) => value === 0));
        assert.ok(storage.offsets.every((table) => table.every((value) => value === 0)));
      });
    }
  });
}

// shader-corpus-inventory
{
  // 验证 shader-corpus-inventory 的约定与边界；依据 ../scripts/inventory-shader-corpus.mjs 及本文件的测试断言。
  // Verify shader-corpus-inventory contracts and boundaries against ../scripts/inventory-shader-corpus.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { mkdtemp, writeFile, mkdir, symlink, truncate, rm } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { createHash } = nodeCryptoModule;
  const { spawnSync } = nodeChild_processModule;
  const { fileURLToPath } = nodeUrlModule;
  const { inventoryShaderCorpus } = inventoryShaderCorpusModule;
  const keyA = "0000000000000001",
    keyB = "0000000000000002";
  async function directory(t) {
    const folder = await mkdtemp(join(tmpdir(), "private-shader-corpus-"));
    t.after(() => rm(folder, { recursive: true, force: true }));
    return folder;
  }
  async function base(
    folder,
    key = keyA,
    source = "fragment float4 privateName() { return float4(1); }\n",
  ) {
    await writeFile(join(folder, `${key}_b.metallib`), source);
    await writeFile(
      join(folder, `${key}_b.json`),
      JSON.stringify({ key: `${key}_b`, stage: "ps" }),
    );
  }

  test("matches exact filename/raw-hash source digest and counts deduplicated base/fetch sources", async (t) => {
    const folder = await directory(t);
    const normalized = "fragment float4 privateName() { return float4(1); }\n";
    await base(folder, keyB, normalized.replaceAll("\n", "\r\n"));
    await base(folder, keyA, normalized);
    await writeFile(join(folder, `${keyA}_bf.metallib`), normalized);
    const digest = createHash("sha256");
    for (const key of [keyA, keyB]) {
      digest.update(`${key}_b.metal`);
      digest.update(createHash("sha256").update(normalized).digest());
    }
    const expectedDigest = digest.digest("hex").slice(0, 16);
    const result = await inventoryShaderCorpus(folder, { expectedDigest, expectedCount: 2 });
    assert.equal(result.sourceDigest, expectedDigest);
    assert.equal(result.baseCount, 2);
    assert.equal(result.fetchCount, 1);
    assert.equal(result.uniqueNormalizedSourceHashCount, 1);
    assert.equal(result.expectation, "matches");
    assert.equal(result.authenticated, false);
    assert.equal(result.inventory.stages.ps, 3);
    assert.doesNotMatch(JSON.stringify(result), /privateName|\.metallib|private-shader-corpus/);
    assert.equal((await inventoryShaderCorpus(folder)).expectation, "not-checked");
    assert.equal(
      (await inventoryShaderCorpus(folder, { expectedDigest, expectedCount: 3 })).expectation,
      "mismatch",
    );
  });

  test("rejects compiled MTLB bytes and malformed source encoding", async (t) => {
    const folder = await directory(t);
    await base(folder, keyA, Buffer.from([77, 84, 76, 66, 0, 0, 0, 0]));
    await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_COMPILED_BINARY" });
    await base(folder, keyA, Buffer.from([255, 255]));
    await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_INVALID_UTF8" });
    await base(folder, keyA, "private\0source");
    await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_INVALID_SOURCE" });
  });

  test("rejects missing or malformed metadata and mismatched shader identity", async (t) => {
    const folder = await directory(t);
    await writeFile(join(folder, `${keyA}_b.metallib`), "fragment void f() {}");
    await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_MISSING_COMPANION" });
    for (const content of ["private broken json", JSON.stringify({ stage: "secretStage" })]) {
      await writeFile(join(folder, `${keyA}_b.json`), content);
      await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_INVALID_METADATA" });
    }
    await writeFile(
      join(folder, `${keyA}_b.json`),
      JSON.stringify({ key: `${keyB}_b`, stage: "ps" }),
    );
    await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_METADATA_KEY_MISMATCH" });
  });

  test("does not skip unexpected shader files or traverse mixed chain folders", async (t) => {
    const folder = await directory(t);
    await base(folder);
    await writeFile(join(folder, "private_unknown_b.metallib"), "private");
    await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_UNEXPECTED_ENTRY" });
    await rm(join(folder, "private_unknown_b.metallib"));
    await mkdir(join(folder, "b-other-chain"));
    await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_NONREGULAR_ENTRY" });
  });

  test("rejects symlink inputs, selected symlink directories and oversized regular sources", async (t) => {
    const folder = await directory(t);
    const outside = await directory(t);
    await base(outside);
    await symlink(join(outside, `${keyA}_b.metallib`), join(folder, `${keyA}_b.metallib`));
    await assert.rejects(inventoryShaderCorpus(folder), { code: "CORPUS_NONREGULAR_ENTRY" });
    const link = join(folder, "selected-link");
    await symlink(outside, link, "dir");
    await assert.rejects(inventoryShaderCorpus(link), { code: "CORPUS_INVALID_DIRECTORY" });
    await truncate(join(outside, `${keyA}_b.metallib`), 8 * 1024 * 1024 + 1);
    await assert.rejects(inventoryShaderCorpus(outside), { code: "CORPUS_FILE_SIZE" });
  });

  test("CLI emits only safe codes for private paths and metadata and supports explicit expectations", async (t) => {
    const folder = await directory(t);
    const script = fileURLToPath(
      new URL("../scripts/inventory-shader-corpus.mjs", import.meta.url),
    );
    await base(folder);
    const good = spawnSync(process.execPath, [script, folder], { encoding: "utf8" });
    assert.equal(good.status, 0, good.stderr);
    assert.equal(JSON.parse(good.stdout).baseCount, 1);
    const mismatch = spawnSync(
      process.execPath,
      [script, folder, "--expected-digest", "0000000000000000", "--expected-count", "1"],
      { encoding: "utf8" },
    );
    assert.equal(mismatch.status, 2);
    assert.equal(JSON.parse(mismatch.stdout).expectation, "mismatch");
    await writeFile(join(folder, `${keyA}_b.json`), "private secret broken JSON");
    const bad = spawnSync(process.execPath, [script, folder], { encoding: "utf8" });
    assert.equal(bad.status, 1);
    assert.equal(bad.stderr, "CORPUS_INVALID_METADATA\n");
    assert.equal(bad.stdout, "");
  });
}

// shader-diagnostics
{
  // 合成诊断过滤测试，不加载 WASM 或私有输入。
  // Synthetic diagnostic filtering tests, without WASM or private input.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { sanitizeCompilerDiagnostics, sanitizeShaderDiagnostic, shaderFailureDiagnostic } =
    shaderDiagnosticsModule;

  test("compiler prose is bounded, strips source excerpts, paths, secrets and IR", () => {
    const diagnostics = sanitizeCompilerDiagnostics([
      "unknown type name 'texture2d_ms'\nno matching function for call to 'sample'\n" +
        "1 | kernel void PRIVATE_SOURCE() { int value = 42; }\n^~~~~\n" +
        "error: title.keys = SECRET_CANARY\nerror: api_token: SECRET_CANARY\n" +
        "error: /home/Private Name/game/file.metal missing\n" +
        "error: C:\\Users\\Private\\game.metal missing\n" +
        "error: key 0123456789abcdef0123456789abcdef\n" +
        "define void @private() {\n%private = call void @thing()\n}",
    ]);
    assert.match(diagnostics.join("\n"), /texture2d_ms|sample/);
    assert.doesNotMatch(
      diagnostics.join("\n"),
      /PRIVATE_SOURCE|SECRET_CANARY|home|Users|0123456789abcdef|define|@thing/,
    );
    const bounded = sanitizeCompilerDiagnostics(
      Array.from({ length: 30 }, (_, i) => `error: ${i} ${"x".repeat(600)}`),
    );
    assert.ok(bounded.length <= 8);
    assert.ok(bounded.join("").length <= 2048);
    assert.ok(Object.isFrozen(diagnostics));
  });

  test("app causes are exact authored messages, never arbitrary exception strings or objects", () => {
    assert.deepEqual(
      shaderFailureDiagnostic(new Error("Control hash does not match catalog key"), {
        substep: "record-preparation",
        record: 1,
      }),
      {
        substep: "record-preparation",
        errorType: "Error",
        message: "Control hash does not match catalog key",
        record: 1,
      },
    );
    const value = sanitizeShaderDiagnostic({
      substep: "PRIVATE_SUBSTEP",
      message: "PRIVATE_MESSAGE",
      errorType: "PRIVATE_TYPE",
      record: 9000,
      diagnostics: ["PRIVATE_DIAGNOSTIC"],
      sourceSha256: "PRIVATE_HASH",
      path: "/private",
    });
    assert.equal(value, undefined);
    assert.deepEqual(
      shaderFailureDiagnostic(new TypeError("PRIVATE_RAW"), { substep: "source-read" }),
      { substep: "source-read", errorType: "TypeError" },
    );
  });

  test("already filtered diagnostics survive repeated worker and UI projections", () => {
    const once = sanitizeShaderDiagnostic({
      substep: "compilation",
      compilerPhase: "compiling",
      diagnostics: ["/input/shader.metal:1:4: error: unknown type name 'missing'"],
      record: 3,
      sourceSha256: "f".repeat(64),
    });
    assert.deepEqual(sanitizeShaderDiagnostic(once), once);
    assert.equal(once.diagnostics[0], "source:1:4: error: unknown type name 'missing'");
  });

  test("expected punctuation remains useful without accepting shader excerpt blocks", () => {
    assert.deepEqual(
      sanitizeCompilerDiagnostics(
        "expected '}'\nerror: expected '{' after function declarator\nerror: kernel void f() { do_work(); }",
      ),
      ["expected '}'", "error: expected '{' after function declarator"],
    );
  });

  test("runtime staging keeps diagnostics while generic compiler identity excludes D2R helpers", async () => {
    const { readFile } = await import("node:fs/promises");
    const policy = await readFile(
      new URL("../runtime-build/payload-policy.mjs", import.meta.url),
      "utf8",
    );
    assert.match(policy, /"shader-diagnostics\.mjs"/);
    const adapter = await readFile(new URL("../air-compiler-adapter.mjs", import.meta.url), "utf8");
    assert.match(adapter, /shader-diagnostics\.mjs/);
    const { readReviewedSourceLock } = await import("./fixtures/air/fixture.mjs");
    const lock = await readReviewedSourceLock();
    assert(Object.keys(lock.compilerFiles).every((path) => !path.includes("shader-diagnostics")));
    assert(Object.keys(lock.sdkFiles).every((path) => !path.includes("shader-diagnostics")));
  });

  test("native frontend prose keeps privacy filtering, bounds and idempotence without a phrase whitelist", async () => {
    const { sanitizeAirFrontendDiagnostics } = await import("../shader-diagnostics.mjs");
    const value = [
      "address space is not permitted on this declaration",
      "parameter is declared here",
      "type conversion changes address space",
      "non-const binding is invalid",
      "an unfamiliar diagnostic should still be reported",
      "1 | kernel void PRIVATE_SOURCE() { private_function(); }",
      "    ^~~~",
      "parameter cannot use prod.keys PRIVATE_KEY_CANARY",
      "parameter from '/Users/PRIVATE_PATH/game.metal' is invalid",
      "type C:\\Users\\PRIVATE_PATH\\game.metal cannot be used",
      "private_shader_identifier",
      "float4 PRIVATE_SOURCE;",
      "return private_function();",
      "// PRIVATE_SOURCE comment",
      "kernel void PRIVATE_SOURCE() { private_function(); }",
      "%private = call void @private_function()",
    ].join("\n");
    const actual = sanitizeAirFrontendDiagnostics(value);
    assert.deepEqual(
      actual.slice(0, 5),
      value
        .split("\n")
        .slice(0, 5)
        .map((line) => `frontend diagnostic: ${line}`),
    );
    assert.doesNotMatch(
      actual.join("\n"),
      /PRIVATE|private_shader_identifier|private_function|Users/,
    );
    assert.deepEqual(sanitizeCompilerDiagnostics(actual), actual);
    assert.deepEqual(sanitizeAirFrontendDiagnostics(actual.join("\n")), actual);
    assert.deepEqual(sanitizeShaderDiagnostic({ diagnostics: actual }).diagnostics, actual);
    const bounded = sanitizeAirFrontendDiagnostics(
      Array.from({ length: 30 }, (_, i) => `parameter ${i} ${"detail ".repeat(200)}`).join("\n"),
    );
    assert.ok(bounded.length <= 8);
    assert.ok(bounded.every((line) => line.length <= 512));
    assert.ok(bounded.join("").length <= 2048);
    assert.deepEqual(sanitizeCompilerDiagnostics(bounded), bounded);
    assert.deepEqual(sanitizeCompilerDiagnostics("parameter may not be qualified"), []);
    assert.deepEqual(shaderFailureDiagnostic(new Error("parameter may not be qualified")), {
      errorType: "Error",
    });
  });
}

// shader-digest-input
{
  // 合成差分检查保留原生文件名/原始哈希顺序，不使用游戏数据。
  // Synthetic differential checks preserve native filename/raw-hash ordering without game data.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { default: test } = nodeTestModule;
  const { buildShaderSourceDigestInput } = shaderPipelineModule;
  const key = (n) => n.toString(16).padStart(16, "0");
  const digest = (value) => createHash("sha256").update(value).digest();

  test("single-buffer layout equals legacy concatenation and sequential native-style hashing", () => {
    for (const count of [1, 2, 6927, 16384]) {
      const keys = Array.from({ length: count }, (_, i) => key(i));
      const completed = new Map(keys.map((value) => [value, digest(`source ${value}\n`)]));
      const original = Buffer.concat(
        keys.map((value) => Buffer.concat([Buffer.from(`${value}_b.metal`), completed.get(value)])),
      );
      const actual = buildShaderSourceDigestInput(keys, completed);
      assert.equal(actual.byteLength, 56 * count);
      assert.equal(actual.buffer.byteLength, actual.byteLength);
      assert.deepEqual(Buffer.from(actual), original);
      const native = createHash("sha256");
      for (const value of keys) native.update(`${value}_b.metal`).update(completed.get(value));
      assert.equal(digest(actual).toString("hex"), native.digest("hex"));
      assert.deepEqual(completed.get(keys[0]), digest(`source ${keys[0]}\n`));
    }
  });

  test("digest input rejects excess counts, malformed hashes and noncanonical ordering", () => {
    const a = key(1),
      b = key(2),
      hashes = new Map([
        [a, new Uint8Array(32)],
        [b, new Uint8Array(32)],
      ]);
    for (const keys of [[], Array(16385).fill(a), [b, a], [a, a], ["INVALID"], [a, key(3)]])
      assert.throws(() => buildShaderSourceDigestInput(keys, hashes));
    for (const hash of [new Uint8Array(31), new Uint8Array(33), Array(32).fill(0), null])
      assert.throws(() => buildShaderSourceDigestInput([a], new Map([[a, hash]])));
  });
}

// shader-feature-inventory
{
  // 验证 shader-feature-inventory 的约定与边界；依据 ../shader-feature-inventory.mjs 及本文件的测试断言。
  // Verify shader-feature-inventory contracts and boundaries against ../shader-feature-inventory.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { createShaderFeatureInventory } = shaderFeatureInventoryModule;

  test("inventories generated ABI features and variants without retaining identifiers", () => {
    const inventory = createShaderFeatureInventory();
    inventory.add(
      `
    struct PrivateArguments {
      texture2d<float> secretTexture [[id(5)]];
      sampler secretSampler [[id(9)]];
    };
    constant bool secretFlag [[function_constant(32)]];
    struct PrivateOutput { float4 secretColor [[color(0), function_constant(secretFlag)]]; };
    fragment PrivateOutput secretEntry(constant PrivateArguments& args [[buffer(0)]],
      float4 p [[position]]) {
      return {args.secretTexture.sample(args.secretSampler, p.xy) + dfdx(p)};
    }
  `,
      { stage: "ps", variant: "readableTile" },
    );
    const result = inventory.snapshot();
    assert.equal(result.records, 1);
    assert.equal(result.stages.ps, 1);
    assert.equal(result.variants.readableTile, 1);
    assert.equal(result.attributes.id, 2);
    assert.equal(result.attributes.buffer, 1);
    assert.equal(result.textureTypeOccurrences.texture2d, 1);
    assert.deepEqual(result.functionConstants, {
      numericAttributeOccurrences: 1,
      symbolicAttributeOccurrences: 1,
    });
    assert.equal(result.featureShaderCounts.textureSampling, 1);
    assert.equal(result.featureShaderCounts.derivatives, 1);
    assert.doesNotMatch(JSON.stringify(result), /secret|Private/);
  });

  test("comments, escaped literals and raw literals do not spoof features", () => {
    const inventory = createShaderFeatureInventory();
    inventory.add(
      `// texture2d<float> [[buffer(42)]]\n
    /* threadgroup atomic_uint */
    "discard_fragment() \\" [[id(4)]]";
    R"tag(texture3d<float> [[id(7)]] ")tag";
    kernel void actual() { threadgroup_barrier(mem_flags::mem_threadgroup); }
  `,
      { stage: "cs" },
    );
    const result = inventory.snapshot();
    assert.equal(result.featureShaderCounts.barriers, 1);
    assert.equal(result.featureShaderCounts.threadgroupMemory, 0);
    assert.equal(result.featureShaderCounts.discard, 0);
    assert.equal(result.attributes.id, 0);
    assert.equal(result.textureTypeOccurrences.texture2d, 0);
    assert.equal(result.textureTypeOccurrences.texture3d, 0);
  });

  test("line splicing is processed before comments and identifiers", () => {
    const inventory = createShaderFeatureInventory();
    inventory.add("// ignored \\\ntexture2d<float>\nthread\\\ngroup float value;", { stage: "cs" });
    assert.equal(inventory.snapshot().textureTypeOccurrences.texture2d, 0);
    assert.equal(inventory.snapshot().featureShaderCounts.threadgroupMemory, 1);
  });

  test("reports preprocessor and unknown attributes without publishing their names", () => {
    const inventory = createShaderFeatureInventory();
    inventory.add("#if 0\nfloat x [[private_attribute(3)]];\n#endif", { stage: "vs" });
    const result = inventory.snapshot();
    assert.equal(result.featureShaderCounts.preprocessor, 1);
    assert.equal(result.unknownAttributeOccurrences, 1);
    assert.doesNotMatch(JSON.stringify(result), /private_attribute/);
  });

  test("snapshots are detached and invalid records never update totals", () => {
    const inventory = createShaderFeatureInventory();
    inventory.add("kernel void f() {}", { stage: "cs" });
    const snapshot = inventory.snapshot();
    snapshot.stages.cs = 100;
    for (const source of ['"secret', "/* secret", 'R"tag(secret']) {
      assert.throws(() => inventory.add(source, { stage: "cs" }), /MSL_INVENTORY_/);
    }
    assert.throws(() => inventory.add("", { stage: "secret" }), /INVALID_INPUT/);
    assert.throws(() => inventory.add("x".repeat(8 * 1024 * 1024 + 1), { stage: "cs" }), /LIMIT/);
    assert.equal(inventory.snapshot().records, 1);
    assert.equal(inventory.snapshot().stages.cs, 1);
  });

  test("preserves directive line boundaries and nested attribute argument commas", () => {
    const inventory = createShaderFeatureInventory();
    inventory.add(
      "float x; /* comment\n */ #if 0\nfloat y [[private_call(f(1, 2), 3), position]];",
      {
        stage: "vs",
      },
    );
    const result = inventory.snapshot();
    assert.equal(result.featureShaderCounts.preprocessor, 1);
    assert.equal(result.unknownAttributeOccurrences, 1);
    assert.equal(result.attributes.position, 1);
    assert.throws(
      () => inventory.add("float y [[color((0)]];", { stage: "ps" }),
      /INVALID_ATTRIBUTE/,
    );
    assert.equal(inventory.snapshot().records, 1);
  });
}

// shader-fixture-encoding
{
  // 验证 shader-fixture-encoding 的约定与边界；依据 ../toolchain/shader-spike/wwwroot/synthetic-fixture.mjs 及本文件的测试断言。
  // Verify shader-fixture-encoding contracts and boundaries against ../toolchain/shader-spike/wwwroot/synthetic-fixture.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { code, job } = syntheticFixtureModule;
  test("authored shader fixture keeps exact encoding and options", () => {
    assert.equal(code.byteLength, 16);
    const view = new DataView(code.buffer, code.byteOffset, code.byteLength);
    assert.equal(view.getBigUint64(0, true), 0n);
    assert.equal(view.getBigUint64(8, true), 0xe30000000007000fn);
    assert.equal(job.stage, "cs");
    assert.deepEqual(job.local, [1, 1, 1]);
    assert.equal(job.shared, 0);
    assert.equal(job.debug, true);
    assert.equal(job.reduced, false);
  });
}

// shader-pipeline
{
  // 验证 shader-pipeline 的约定与边界；依据 ../shader-pipeline.mjs、../toolchain/spirv-cross-spike/wwwroot/convert.mjs 及本文件的测试断言。
  // Verify shader-pipeline contracts and boundaries against ../shader-pipeline.mjs、../toolchain/spirv-cross-spike/wwwroot/convert.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { spawnSync } = nodeChild_processModule;
  const { mkdtempSync, writeFileSync, rmSync } = nodeFsModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { default: test } = nodeTestModule;
  const { createShaderPipeline, SHADER_PIPELINE_LIMITS } = shaderPipelineModule;
  const { CROSS_COMMIT, MSL_ARGUMENTS, validateSpirv } = convertModule;
  const { fixtures: graphicsFixtures } = graphicsFixturesModule;
  const { replayFields } = replayReflectionModule;
  const key = "0000000000000001",
    chain = "abcdef012345",
    msl = "kernel void main0() {}\n";
  const spv = new Uint8Array(new Uint32Array([0x07230203, 0x10000, 0, 1, 0]).buffer);
  const sha = (x) => createHash("sha256").update(x).digest("hex");
  const sourceDigest = (sources) => {
    const h = createHash("sha256");
    for (const [k, v] of [...sources].sort(([a], [b]) => a.localeCompare(b)))
      h.update(`${k}_b.metal`).update(
        createHash("sha256").update(v.replace(/\r\n/g, "\n")).digest(),
      );
    return h.digest("hex").slice(0, 16);
  };
  function shell(keys = [key], source = sourceDigest([[key, msl]])) {
    const targetOS = { platform: "ios", minimumVersion: "16.0" },
      requiredPaths = [`msl/b-${chain}/`];
    return {
      entries: ["Info.plist", "App", "dm/shell_info.txt", "dm/shell_manifest.txt"].map((x) => ({
        path: `Payload/App.app/${x}`,
        size: 10,
        mode: 0o100644,
      })),
      shellInfoText: `PRODUCT=synthetic\nCHAIN=${chain}\nKIND=ios\nSHADER_SRC=${source}\nSPIRV_CROSS=1.4.363.0\n`,
      shellManifestText: requiredPaths.join("\n"),
      observedTargetOS: targetOS,
      trustedRecipe: {
        schemaVersion: 1,
        product: "synthetic",
        chain,
        kind: "ios",
        shaderDigest: source,
        targetOS,
        requiredPaths,
        shaderKeys: keys,
        spirvCrossVersion: "1.4.363.0",
      },
    };
  }
  const job = { key, stage: "cs", local: [1, 1, 1], debug: true, reduced: false };
  function setup({
    trustedShell = shell(),
    mutateTranslate,
    mutateCross,
    hang,
    crash,
    timeoutMs = 500,
    wrongId = false,
    transferInputs = true,
  } = {}) {
    const workers = [];
    class FakeWorker {
      constructor(url) {
        this.url = url;
        this.listeners = new Map();
        this.terminated = false;
        workers.push(this);
      }
      addEventListener(type, cb) {
        if (!this.listeners.has(type)) this.listeners.set(type, new Set());
        this.listeners.get(type).add(cb);
      }
      removeEventListener(type, cb) {
        this.listeners.get(type)?.delete(cb);
      }
      terminate() {
        this.terminated = true;
      }
      emit(type, value) {
        for (const fn of this.listeners.get(type) ?? []) fn(value);
      }
      postMessage(raw, transfer) {
        const data = structuredClone(raw, transferInputs ? { transfer } : undefined);
        this.request = data;
        queueMicrotask(() => {
          if (hang) return;
          if (crash) return this.emit("error", { message: "synthetic runtime failure" });
          if (this.url.includes("translator")) {
            let result = {
              spirvBase64: Buffer.from(spv).toString("base64"),
              resources: {
                key: data.job.key,
                stage: data.job.stage,
                notes: [],
                sbufs: [],
                cbufs: [],
                textures: [],
                images: [],
              },
            };
            result = mutateTranslate?.(result) ?? result;
            if (wrongId) this.emit("message", { data: { requestId: -1, error: "unrelated" } });
            this.emit("message", { data: { requestId: data.requestId, result } });
          } else {
            const stage = data.replayContext.stage;
            const stageMsl = {
              cs: msl,
              vs: "vertex void main0() {}\n",
              ps: "fragment void main0() {}\n",
            }[stage];
            const result = {
              schema: "spirv-cross-browser-stage-v1",
              msl: stageMsl,
              reflection: {
                entryPoints: [{ mode: { cs: "comp", vs: "vert", ps: "frag" }[stage] }],
              },
              replay: replayFields(stage, stageMsl, data.replayContext.resources),
              provenance: {
                spirvCrossCommit: CROSS_COMMIT,
                mslArguments: [...MSL_ARGUMENTS],
                inputSha256: sha(new Uint8Array(data.spirv)),
                mslSha256: sha(stageMsl),
                nativeChainClaimed: false,
                metalCompiled: false,
              },
            };
            this.emit("message", {
              data: { id: data.id, ok: true, result: mutateCross?.(result) ?? result },
            });
          }
        });
      }
    }
    const options = {
      trustedShell,
      baseUrl: "https://app.example/tools/index.html",
      translatorWorkerUrl: "./translator.mjs",
      crossWorkerUrl: "./cross.mjs",
      workerFactory: (url, opts) => {
        assert.deepEqual(opts, { type: "module" });
        return new FakeWorker(url);
      },
      timeoutMs,
    };
    return { pipeline: createShaderPipeline(options), workers, options };
  }
  test("actual worker protocols connect, resource facts pass unchanged, transfer preserves caller input", async () => {
    const { pipeline, workers } = setup({ wrongId: true }),
      code = new Uint8Array(16);
    const result = await pipeline.translate({ job, code });
    assert.equal(code.byteLength, 16);
    assert.equal(result.msl, msl);
    assert.equal(result.installable, false);
    assert.equal(result.shaderSourceSetVerified, false);
    assert.equal(result.metalCompiled, false);
    assert.equal(workers.length, 2);
    assert.ok(workers.every((x) => x.terminated));
    assert.deepEqual(workers[1].request.replayContext.resources, result.resources);
    assert.deepEqual(new Uint8Array(workers[1].request.spirv), spv);
    assert.match(result.provenance.codeSha256, /^[a-f0-9]{64}$/);
    assert.deepEqual(await pipeline.verifyCompleteSourceSet(), {
      status: "shader-source-set-equivalent",
      sourceDigest: sourceDigest([[key, msl]]),
      shellChain: chain,
      shaderCount: 1,
      installable: false,
      metalCompiled: false,
      chainRewritten: false,
    });
    await assert.rejects(pipeline.translate({ job, code }), /already converted/);
    pipeline.dispose();
  });
  test("SHADER_SRC aggregate matches original Python function with filename ordering", async () => {
    const key2 = "0000000000000002",
      expected = sourceDigest([
        [key, msl],
        [key2, msl],
      ]),
      { pipeline } = setup({ trustedShell: shell([key2, key], expected) });
    await pipeline.translate({ job: { ...job, key: key2 }, code: new Uint8Array(16) });
    await assert.rejects(pipeline.verifyCompleteSourceSet(), /Incomplete/);
    await pipeline.translate({ job, code: new Uint8Array(16) });
    const dir = mkdtempSync(join(tmpdir(), "shader-src-"));
    try {
      writeFileSync(join(dir, `${key}_b.metal`), msl);
      writeFileSync(join(dir, `${key2}_b.metal`), msl);
      const script =
        'import ast,hashlib,os,sys\np=sys.argv[1]\nt=ast.parse(open(p).read())\nf=next(n for n in t.body if isinstance(n,ast.FunctionDef) and n.name=="src_digest")\nexec(compile(ast.Module(body=[f],type_ignores=[]),p,"exec"))\nprint(src_digest(sys.argv[2]))';
      const native = spawnSync(
        "python3",
        ["-c", script, new URL("../../dm/tools/d2rm_pack.py", import.meta.url).pathname, dir],
        { encoding: "utf8" },
      );
      assert.equal(native.status, 0, native.stderr);
      assert.equal((await pipeline.verifyCompleteSourceSet()).sourceDigest, native.stdout.trim());
    } finally {
      rmSync(dir, { recursive: true, force: true });
      pipeline.dispose();
    }
  });
  test("trusted shell mismatch, missing catalog and off-origin assets fail before worker start", () => {
    const { options } = setup();
    const invalid = shell();
    invalid.shellInfoText = invalid.shellInfoText.replace(chain, "bad");
    assert.throws(
      () => createShaderPipeline({ ...options, trustedShell: invalid }),
      /CHAIN mismatch/,
    );
    // 过旧的编译器和未经验证的未来编译器都应安全拒绝，即使模板与应用配方相互一致。
    // Both an obsolete compiler and an unverified future compiler fail closed,
    // even when the shell and application recipe agree with one another.
    for (const version of ["1.4.357.0", "1.4.999.0"]) {
      const incompatible = shell();
      incompatible.shellInfoText = incompatible.shellInfoText.replace(
        "SPIRV_CROSS=1.4.363.0",
        `SPIRV_CROSS=${version}`,
      );
      incompatible.trustedRecipe.spirvCrossVersion = version;
      assert.throws(
        () => createShaderPipeline({ ...options, trustedShell: incompatible }),
        /Unsupported shell SPIRV_CROSS/,
      );
    }
    const disagree = shell();
    disagree.trustedRecipe.spirvCrossVersion = "1.4.357.0";
    assert.throws(
      () => createShaderPipeline({ ...options, trustedShell: disagree }),
      /Unsupported shell SPIRV_CROSS/,
    );
    const missing = shell();
    delete missing.trustedRecipe.shaderKeys;
    assert.throws(() => createShaderPipeline({ ...options, trustedShell: missing }), /catalog/);
    for (const url of [
      "https://elsewhere.example/a.mjs",
      "data:text/javascript,x",
      "file:///tmp/a.mjs",
      "https://u:p@app.example/a.mjs",
    ])
      assert.throws(() => createShaderPipeline({ ...options, crossWorkerUrl: url }), /self-hosted/);
  });
  test("geometry/tessellation, unknown keys and implicit options remain unsupported", async () => {
    const { pipeline, workers } = setup();
    await assert.rejects(
      pipeline.translate({ job: { ...job, stage: "gs" }, code: new Uint8Array(16) }),
      /Unsupported shader stage/,
    );
    await assert.rejects(
      pipeline.translate({ job: { ...job, key: "ffffffffffffffff" }, code: new Uint8Array(16) }),
      /outside trusted/,
    );
    await assert.rejects(
      pipeline.translate({ job: { ...job, reduced: undefined }, code: new Uint8Array(16) }),
      /Explicit native/,
    );
    assert.equal(workers.length, 0);
    pipeline.dispose();
  });
  test("translator defaults, malformed SPIR-V and wrong resource identity fail closed", async () => {
    for (const mutateTranslate of [
      (r) => ({ ...r, spirvBase64: "bad" }),
      (r) => ({ ...r, resources: { ...r.resources, key: "bad" } }),
      (r) => ({ ...r, resources: { ...r.resources, notes: ["missing texture type"] } }),
    ]) {
      const { pipeline, workers } = setup({ mutateTranslate });
      await assert.rejects(pipeline.translate({ job, code: new Uint8Array(16) }));
      assert.equal(workers.length, 1);
      assert.ok(workers[0].terminated);
      pipeline.dispose();
    }
  });
  test("large bounded SPIR-V decodes without recursive regex or iterable expansion", async (t) => {
    const bytes = new Uint8Array(4 * 1024 * 1024);
    bytes.set(spv);
    const view = new DataView(bytes.buffer);
    for (let offset = spv.length; offset < bytes.length; offset += 4)
      view.setUint32(offset, 0x10000, true);
    const nativeFrom = Uint8Array.from;
    t.mock.method(Uint8Array, "from", function (value, ...args) {
      assert.notEqual(typeof value, "string", "do not expand binary strings into iterable lists");
      return nativeFrom.call(this, value, ...args);
    });
    const { pipeline, workers } = setup({
      timeoutMs: 10000,
      mutateTranslate: (result) => ({
        ...result,
        spirvBase64: Buffer.from(bytes).toString("base64"),
      }),
    });
    try {
      assert.equal((await pipeline.translate({ job, code: new Uint8Array(16) })).msl, msl);
      assert.equal(workers.length, 2);
      assert.deepEqual(new Uint8Array(workers[1].request.spirv), bytes);
      assert.equal((await pipeline.verifyCompleteSourceSet()).shaderCount, 1);
    } finally {
      pipeline.dispose();
    }
  });
  test("SPIR-V base64 requires canonical alphabet, padding and pad bits", async () => {
    const canonical = Buffer.from(spv).toString("base64");
    for (const encoded of [
      null,
      7,
      "",
      " " + canonical,
      canonical + "\n",
      canonical.slice(0, 4) + "\t" + canonical.slice(4),
      canonical.slice(0, -1),
      canonical + "=",
      canonical.slice(0, -2) + "B=",
      "=" + canonical.slice(1),
      "-" + canonical.slice(1),
      "_" + canonical.slice(1),
      "$" + canonical.slice(1),
      "A".repeat(Math.ceil(SHADER_PIPELINE_LIMITS.spirv / 3) * 4),
      "A".repeat(Math.ceil(SHADER_PIPELINE_LIMITS.spirv / 3) * 4 + 4),
    ]) {
      const { pipeline, workers } = setup({
        mutateTranslate: (result) => ({ ...result, spirvBase64: encoded }),
      });
      try {
        await assert.rejects(pipeline.translate({ job, code: new Uint8Array(16) }));
        assert.equal(workers.length, 1);
        assert.ok(workers[0].terminated);
        await assert.rejects(pipeline.verifyCompleteSourceSet(), /Incomplete/);
      } finally {
        pipeline.dispose();
      }
    }
    for (const size of [20, 24, 28]) {
      const bytes = new Uint8Array(size);
      bytes.set(spv);
      const view = new DataView(bytes.buffer);
      for (let offset = spv.length; offset < size; offset += 4)
        view.setUint32(offset, 0x10000, true);
      const { pipeline } = setup({
        mutateTranslate: (result) => ({
          ...result,
          spirvBase64: Buffer.from(bytes).toString("base64"),
        }),
      });
      try {
        assert.equal((await pipeline.translate({ job, code: new Uint8Array(16) })).msl, msl);
      } finally {
        pipeline.dispose();
      }
    }
  });
  test("forged compiler provenance, output hash and replay are rejected", async () => {
    for (const mutateCross of [
      (r) => ({ ...r, provenance: { ...r.provenance, spirvCrossCommit: "wrong" } }),
      (r) => ({ ...r, msl: r.msl + "corrupt" }),
      (r) => ({ ...r, replay: {} }),
      (r) => ({ ...r, provenance: { ...r.provenance, nativeChainClaimed: true } }),
    ]) {
      const { pipeline, workers } = setup({ mutateCross });
      await assert.rejects(pipeline.translate({ job, code: new Uint8Array(16) }), /mismatch/);
      assert.ok(workers.every((x) => x.terminated));
      await assert.rejects(pipeline.verifyCompleteSourceSet(), /Incomplete/);
      pipeline.dispose();
    }
  });
  test("complete but mismatched source set is never accepted or relabeled", async () => {
    const { pipeline } = setup({ trustedShell: shell([key], "0000000000000000") });
    await pipeline.translate({ job, code: new Uint8Array(16) });
    await assert.rejects(pipeline.verifyCompleteSourceSet(), /SHADER_SRC mismatch/);
    pipeline.dispose();
  });
  test("abort, disposal, worker crash and deadline terminate workers and release busy state", async () => {
    for (const action of ["abort", "dispose", "timeout", "crash"]) {
      const { pipeline, workers } = setup({
          hang: action !== "crash",
          crash: action === "crash",
          timeoutMs: 20,
        }),
        controller = new AbortController();
      const pending = pipeline.translate({
        job,
        code: new Uint8Array(16),
        signal: controller.signal,
      });
      const rejected = assert.rejects(pending, /cancelled|timed out|worker failed/);
      if (action !== "crash")
        await assert.rejects(pipeline.translate({ job, code: new Uint8Array(16) }), /one shader/);
      await new Promise((resolve) => setTimeout(resolve, 5));
      if (action === "abort") controller.abort();
      if (action === "dispose") pipeline.dispose();
      await rejected;
      assert.ok(workers.every((x) => x.terminated));
      pipeline.dispose();
      await assert.rejects(pipeline.verifyCompleteSourceSet(), /disposed/);
    }
  });

  test("owned hash snapshots are wiped on cancellation, rejection and successful facade replies", async () => {
    const originalDigest = crypto.subtle.digest;
    for (const outcome of ["abort-code", "abort-spirv", "bad-provenance", "success"]) {
      const captured = [],
        controller = new AbortController();
      crypto.subtle.digest = function (algorithm, bytes) {
        captured.push(bytes);
        if (
          (outcome === "abort-code" && captured.length === 1) ||
          (outcome === "abort-spirv" && captured.length === 3)
        )
          controller.abort();
        return originalDigest.call(this, algorithm, bytes);
      };
      const { pipeline, workers } = setup({
        transferInputs: false,
        mutateCross:
          outcome === "bad-provenance" ? (result) => ({ ...result, replay: {} }) : undefined,
      });
      const caller = new Uint8Array(16).fill(7);
      try {
        const pending = pipeline.translate({ job, code: caller, signal: controller.signal });
        if (outcome === "success") {
          assert.equal((await pending).msl, msl);
        } else {
          await assert.rejects(pending, outcome.startsWith("abort") ? /cancelled/ : /mismatch/);
        }
        assert.equal(
          captured.length,
          outcome === "abort-code" ? 2 : outcome === "abort-spirv" ? 3 : 4,
        );
        for (const owned of captured) {
          assert.notEqual(owned, caller);
          assert.ok(owned.every((byte) => byte === 0));
        }
        assert.deepEqual(caller, new Uint8Array(16).fill(7));
        assert.ok(workers.every((worker) => worker.terminated));
        crypto.subtle.digest = originalDigest;
        if (outcome === "success")
          assert.equal((await pipeline.verifyCompleteSourceSet()).shaderCount, 1);
        else await assert.rejects(pipeline.verifyCompleteSourceSet(), /Incomplete/);
      } finally {
        crypto.subtle.digest = originalDigest;
        pipeline.dispose();
      }
    }
  });

  test("temporary decoded SPIR-V is wiped after validation success or rejection", async () => {
    const originalFill = Uint8Array.prototype.fill;
    for (const valid of [true, false]) {
      let decoded;
      Uint8Array.prototype.fill = function (...args) {
        if (this.length === spv.length) decoded = this;
        return originalFill.apply(this, args);
      };
      const { pipeline } = setup({
        mutateTranslate: valid
          ? undefined
          : (result) => ({
              ...result,
              spirvBase64: Buffer.alloc(20, 7).toString("base64"),
            }),
      });
      try {
        const pending = pipeline.translate({ job, code: new Uint8Array(16) });
        if (valid) assert.equal((await pending).msl, msl);
        else await assert.rejects(pending, /SPIR-V magic/);
        assert.equal(decoded.length, 20);
        assert.ok(decoded.every((byte) => byte === 0));
      } finally {
        Uint8Array.prototype.fill = originalFill;
        pipeline.dispose();
      }
    }
  });

  test("graphics validates full SPH, explicit metadata and stage-specific wire/replay fields", async () => {
    for (const f of graphicsFixtures.filter((f) => f.job.stage !== "cs")) {
      const expectedMsl =
        f.job.stage === "vs" ? "vertex void main0() {}\n" : "fragment void main0() {}\n";
      const { pipeline, workers } = setup({
        trustedShell: shell([f.job.key], sourceDigest([[f.job.key, expectedMsl]])),
      });
      const result = await pipeline.translate({ job: f.job, code: f.code });
      assert.equal(result.stage, f.job.stage);
      assert.equal(result.threadgroup_size, null);
      assert.equal(workers[1].request.replayContext.stage, f.job.stage);
      assert.equal(result.replay.msl_entry_kind, f.job.stage === "vs" ? "vertex" : "fragment");
      await pipeline.verifyCompleteSourceSet();
      pipeline.dispose();
      for (const [badJob, badCode, pattern] of [
        [f.job, new Uint8Array(16), /SPH/],
        [f.job, new Uint8Array(f.code.length), /SPH stage/],
        [{ ...f.job, attr: undefined }, f.code, /explicit attr/],
        [{ ...f.job, attr: { 32: "float" } }, f.code, /Invalid graphics/],
        [{ ...f.job, fout: { 0: "double" } }, f.code, /Invalid graphics/],
      ]) {
        const { pipeline, workers } = setup({ trustedShell: shell([f.job.key]) });
        await assert.rejects(pipeline.translate({ job: badJob, code: badCode }), pattern);
        assert.equal(workers.length, 0);
        pipeline.dispose();
      }
    }
  });
  test("compiled stage mismatch is rejected even if hashes and replay are otherwise valid", async () => {
    const { pipeline } = setup({
      mutateCross: (r) => ({ ...r, reflection: { entryPoints: [{ mode: "frag" }] } }),
    });
    await assert.rejects(
      pipeline.translate({ job, code: new Uint8Array(16) }),
      /stage does not match/,
    );
    pipeline.dispose();
  });

  test("SPIR-V validation owns an exact copy of Buffer subviews", () => {
    const backing = Buffer.alloc(spv.length + 8, 7),
      input = backing.subarray(4, -4);
    input.set(spv);
    const before = Buffer.from(backing),
      validated = validateSpirv(input);
    assert.equal(Object.getPrototypeOf(validated), Uint8Array.prototype);
    assert.notEqual(validated.buffer, input.buffer);
    assert.equal(validated.buffer.byteLength, input.byteLength);
    assert.deepEqual(validated, spv);
    validated.fill(0);
    assert.deepEqual(backing, before);
  });

  test("Buffer caller bytes survive shader transfers and all cleanup outcomes", async () => {
    for (const transferInputs of [true, false]) {
      for (const outcome of ["success", "abort", "crash"]) {
        const backing = Buffer.alloc(24, 7),
          code = backing.subarray(4, 20),
          before = Buffer.from(backing),
          controller = new AbortController(),
          { pipeline, workers } = setup({ transferInputs, crash: outcome === "crash" });
        try {
          const pending = pipeline.translate({ job, code, signal: controller.signal });
          if (outcome === "abort") controller.abort();
          if (outcome === "success") assert.equal((await pending).msl, msl);
          else await assert.rejects(pending, outcome === "abort" ? /cancelled/ : /worker failed/);
          assert.deepEqual(backing, before);
          assert.equal(code.byteLength, 16);
          assert.ok(workers.every((worker) => worker.terminated));
        } finally {
          pipeline.dispose();
        }
      }
    }
  });
}

// shader-retirement-assets
{
  // 隔离公开 WASM 退休所有权测试；所有线程及编译均为无执行替身。
  // Isolate public WASM retirement ownership tests; all workers and compilation are inert stand-ins.
  const { default: test } = nodeTestModule;
  const { spawnSync } = nodeChild_processModule;
  const { assertIsolatedRun } = assertIsolatedRunModule;

  test("shader retirement public-asset ownership (isolated module mocks)", () => {
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
        env: { ...env, D2R_TEST_SCENARIO: "shader-retirement-assets" },
        encoding: "utf8",
        timeout: 30000,
        maxBuffer: 1024 * 1024,
      },
    );
    assertIsolatedRun(result);
  });
}

// shader-sourcelink-diagnostic
{
  // 用临时合成输出验证只读诊断边界；不运行 Git、SDK 或真实编译器。
  // Verify read-only diagnostic bounds with temporary synthetic outputs, without Git, SDKs or actual compilers.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { mkdtemp, mkdir, readFile, rm, symlink, writeFile } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join, dirname } = nodePathModule;
  const { inspectShaderSourceLink } = shaderSourcelinkDiagnosticModule;

  const revision = "a".repeat(40);
  const schema = "shader-sourcelink-diagnostic-v1";
  const direct = "obj/Release/net10.0/ShaderSpike.sourcelink.json";
  const nested = "obj/Release/net10.0/browser-wasm/ShaderSpike.sourcelink.json";
  const data = (current = true) =>
    JSON.stringify({
      documents: {
        "/private-build-path/*": `https://raw.githubusercontent.com/owner/repository/${current ? revision : "b".repeat(40)}/*`,
      },
    });
  async function fixture(t) {
    const projectDirectory = await mkdtemp(join(tmpdir(), "d2r-sourcelink-"));
    t.after(() => rm(projectDirectory, { recursive: true, force: true }));
    return {
      projectDirectory,
      inspect: (sourceRevision = revision) =>
        inspectShaderSourceLink({ projectDirectory, sourceRevision }),
      async write(path, bytes) {
        const target = join(projectDirectory, path);
        await mkdir(dirname(target), { recursive: true });
        await writeFile(target, bytes);
      },
    };
  }

  test("known candidates emit only bounded hashes and revision-presence metadata", async (t) => {
    for (const path of [direct, nested]) {
      const f = await fixture(t),
        raw = data();
      await f.write(path, raw);
      assert.deepEqual(await f.inspect(), {
        schema,
        status: "present",
        bytes: Buffer.byteLength(raw),
        sha256: createHash("sha256").update(raw).digest("hex"),
        documentMappings: 1,
        containsCurrentAppRevision: true,
      });
      await f.write(path, data(false));
      const result = await f.inspect();
      assert.equal(result.containsCurrentAppRevision, false);
      assert.doesNotMatch(
        JSON.stringify(result),
        /private-build-path|githubusercontent|owner|repository/,
      );
    }
  });

  test("absent, ambiguous and malformed inputs produce fixed nonfatal statuses", async (t) => {
    const f = await fixture(t);
    assert.deepEqual(await f.inspect(), { schema, status: "absent" });
    await f.write("unrelated/secret.sourcelink.json", data());
    assert.deepEqual(await f.inspect(), { schema, status: "absent" });
    for (const raw of [
      "not-json",
      JSON.stringify({ documents: [] }),
      JSON.stringify({ documents: {}, other: "secret" }),
      JSON.stringify({ documents: { x: "https://secret:password@example.test/*" } }),
      JSON.stringify({
        documents: Object.fromEntries(
          Array.from({ length: 129 }, (_, index) => [String(index), "https://example.test/*"]),
        ),
      }),
      " ".repeat(65537),
      Buffer.from([0xff]),
    ]) {
      await f.write(direct, raw);
      assert.deepEqual(await f.inspect(), { schema, status: "invalid" });
    }
    await f.write(direct, data());
    assert.deepEqual(await f.inspect("invalid revision"), { schema, status: "invalid" });
    await f.write(nested, data());
    assert.deepEqual(await f.inspect(), { schema, status: "ambiguous" });
  });

  test("candidate files and directory ancestry cannot be symlinks", async (t) => {
    for (const part of [
      "obj",
      "obj/Release",
      "obj/Release/net10.0",
      "obj/Release/net10.0/browser-wasm",
      direct,
    ]) {
      const f = await fixture(t);
      await f.write("outside/ShaderSpike.sourcelink.json", data());
      await mkdir(dirname(join(f.projectDirectory, part)), { recursive: true });
      await symlink(
        join(
          f.projectDirectory,
          part === direct ? "outside/ShaderSpike.sourcelink.json" : "outside",
        ),
        join(f.projectDirectory, part),
      );
      assert.deepEqual(await f.inspect(), { schema, status: "invalid" });
    }
  });

  test("revision matches only URL path segments, never source paths or substrings", async (t) => {
    const f = await fixture(t);
    await f.write(
      direct,
      JSON.stringify({
        documents: {
          [`/${revision}/*`]: `https://example.test/prefix${revision}suffix/*`,
        },
      }),
    );
    assert.equal((await f.inspect()).containsCurrentAppRevision, false);
  });

  test("diagnostic runs after publish and reuses outer manifest revision without payload inclusion", async () => {
    const build = await readFile(new URL("../runtime-build/build.mjs", import.meta.url), "utf8");
    assert.equal(
      [...build.matchAll(/run\("git", \["rev-parse", "HEAD"\], root, true\)/g)].length,
      1,
    );
    assert.match(
      build,
      /run\(command, args, resolve\(root, directory\)\);\s+if \(name === "shader browser publish"\)/,
    );
    assert.match(build, /build: \{[\s\S]*?crossCommit,\s+sourceRevision,/);
    const assembly = await readFile(
      new URL("../runtime-build/package.mjs", import.meta.url),
      "utf8",
    );
    assert.doesNotMatch(assembly, /shader-sourcelink-diagnostic|\.sourcelink\.json/);
  });
}
