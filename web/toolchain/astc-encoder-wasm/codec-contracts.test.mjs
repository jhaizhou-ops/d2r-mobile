// 按功能集中合约测试；保留每个原始测试及其夹具作用域。
// Feature contract suite; preserves every original test and its fixture scope.
import test from "node:test";
import assert from "node:assert/strict";
import { validateImage, encodeWithModule } from "./contract.mjs";
import { encodeAstc4x4 } from "./client.mjs";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { decodeBC3Blocks, decodeBC3BlockGrid, MAX_BC3_BLOCKS } from "./bc3-decoder.mjs";
import { transcodeBC3Batches } from "./bc3-astc-batches.mjs";
import { transcodeTextureStream, TEXTURE_HEADER_CHUNK_BYTES } from "./texture-stream.mjs";
import { transformTextureDescriptorCache, DESCRIPTOR_CACHE_LIMITS } from "./descriptor-cache.mjs";

// synthetic: preserve the original fixture scope and every test case.
{
  // 仅验证合成输入契约；不是编码保真或游戏对等测试。
  // Synthetic contracts only; these are not codec fidelity or game parity tests.
  // 基于公开基元契约和 Node 内置测试断言。
  // Based on the public primitive contract and Node built-in test assertions.

  const image = (width = 4, height = 4) => ({
    width,
    height,
    rgba: new ArrayBuffer(width * height * 4),
  });
  test("size, profile, edge dimensions, and bounded allocation", () => {
    assert.equal(validateImage(image()), 16);
    for (const preset of ["thorough", "fast"])
      assert.equal(validateImage({ ...image(), preset }), 16);
    for (const preset of [null, "", "fastest", "FAST", 0, 1, {}, NaN])
      assert.throws(() => validateImage({ ...image(), preset }), /preset/);
    assert.equal(validateImage(image(5, 1)), 32);
    assert.equal(validateImage(image(1024, 1024)), 1048576);
    for (const width of [0, -1, NaN, Infinity, 1.5, 4097])
      assert.throws(() => validateImage({ ...image(), width }));
    assert.throws(() => validateImage({ ...image(), width: 2048, height: 2048 }));
    assert.throws(() => validateImage({ ...image(), profile: "hdr" }));
    assert.throws(() => validateImage({ ...image(), rgba: new Uint8Array(64) }));
    assert.throws(() => validateImage({ ...image(), rgba: new ArrayBuffer(63) }));
    if (typeof SharedArrayBuffer !== "undefined")
      assert.throws(() => validateImage({ ...image(), rgba: new SharedArrayBuffer(64) }));
  });
  test("module allocations are freed after success, rejection, and partial allocation failure", () => {
    for (const outcome of ["ok", "codec", "allocation"]) {
      let allocations = 0;
      const freed = [];
      const module = {
        _astc_encoder_abi_version: () => 2,
        HEAPU8: new Uint8Array(1024),
        _malloc: () => (++allocations === 1 ? 64 : outcome === "allocation" ? 0 : 128),
        _free: (ptr) => freed.push(ptr),
        _astc_encode_rgba: (...args) => {
          assert.deepEqual(args, [64, 64, 4, 4, 1, 128, 16, 0]);
          module.HEAPU8.fill(73, 128, 144);
          return outcome === "codec" ? 101 : 0;
        },
      };
      const encode = () => encodeWithModule(module, { ...image(), profile: "srgb" });
      if (outcome === "ok") assert.deepEqual(new Uint8Array(encode()), new Uint8Array(16).fill(73));
      else assert.throws(encode);
      assert.deepEqual(freed, outcome === "allocation" ? [64] : [128, 64]);
    }
  });
  test("worker ownership, success, abort, busy guard, malformed response, timeout and startup failure", async () => {
    const original = globalThis.Worker;
    let latest;
    class FakeWorker {
      constructor() {
        latest = this;
      }
      postMessage(data, transfer) {
        this.data = structuredClone(data, { transfer });
      }
      terminate() {
        this.terminated = true;
      }
    }
    globalThis.Worker = FakeWorker;
    try {
      for (const signal of [{}, { aborted: false }, "invalid", 1]) {
        const untouched = image();
        assert.throws(() => encodeAstc4x4(untouched, { signal }), /AbortSignal/);
        assert.equal(untouched.rgba.byteLength, 64);
      }
      const input = image();
      let result = encodeAstc4x4(input);
      assert.equal(input.rgba.byteLength, 0);
      assert.equal(latest.data.preset, "thorough");
      await assert.rejects(encodeAstc4x4(image()), /Only one/);
      latest.onmessage({ data: { ok: true, blocks: new ArrayBuffer(16) } });
      assert.equal((await result).byteLength, 16);
      assert.equal(latest.terminated, true);
      result = encodeAstc4x4({ ...image(), preset: "fast" });
      assert.equal(latest.data.preset, "fast");
      latest.onmessage({ data: { ok: true, blocks: new ArrayBuffer(16) } });
      await result;
      const controller = new AbortController();
      result = encodeAstc4x4(image(), { signal: controller.signal });
      controller.abort();
      await assert.rejects(result, { name: "AbortError" });
      assert.equal(latest.terminated, true);
      const untouched = image();
      await assert.rejects(encodeAstc4x4(untouched, { signal: controller.signal }), {
        name: "AbortError",
      });
      assert.equal(untouched.rgba.byteLength, 64);
      result = encodeAstc4x4(image());
      latest.onmessage({ data: { ok: true, blocks: new ArrayBuffer(15) } });
      await assert.rejects(result, /length/);
      await assert.rejects(encodeAstc4x4(image(), { timeoutMs: 1 }), /deadline/);
      assert.equal(latest.terminated, true);
      globalThis.Worker = class {
        constructor() {
          throw new Error("startup");
        }
      };
      await assert.rejects(encodeAstc4x4(image()), /startup/);
      globalThis.Worker = FakeWorker;
      result = encodeAstc4x4(image());
      latest.onerror();
      await assert.rejects(result, /initialization/);
    } finally {
      globalThis.Worker = original;
    }
  });

  test("preset mapping preserves omitted thorough and selects only bounded ABI values", () => {
    const calls = [];
    const module = {
      _astc_encoder_abi_version: () => 2,
      HEAPU8: new Uint8Array(1024),
      _malloc: () => 64,
      _free: () => {},
      _astc_encode_rgba: (...args) => {
        calls.push(args);
        return 0;
      },
    };
    for (const profile of ["linear", "srgb"]) {
      for (const preset of [undefined, "thorough", "fast"])
        encodeWithModule(module, { ...image(), profile, preset });
      const [omitted, thorough, fast] = calls.splice(0);
      assert.deepEqual(omitted, thorough);
      assert.equal(thorough[7], 0);
      assert.equal(fast[7], 1);
      assert.deepEqual(fast.slice(0, 7), thorough.slice(0, 7));
    }
    assert.throws(() => encodeWithModule(module, { ...image(), preset: "invalid" }), /preset/);
    assert.equal(calls.length, 0);
  });

  test("stale WASM ABI fails closed before allocations", () => {
    for (const version of [undefined, 1, 3])
      assert.throws(
        () =>
          encodeWithModule(
            {
              _astc_encoder_abi_version: version === undefined ? undefined : () => version,
              _malloc: () => {
                assert.fail("must not allocate");
              },
            },
            { ...image(), preset: "fast" },
          ),
        /ABI mismatch/,
      );
  });
}

// bc3-decoder: preserve the original fixture scope and every test case.
{
  // 根据原生 NumPy 纯函数与确定性自造块，验证字节、取整和拼接顺序；不含游戏资源。
  // Verify bytes, rounding and grid order against native NumPy pure functions with deterministic synthetic blocks; no game assets.

  function fixture() {
    const input = new Uint8Array(1031 * 16);
    let seed = 0x19283746;
    for (let i = 0; i < input.length; ++i) {
      seed ^= seed << 13;
      seed ^= seed >>> 17;
      seed ^= seed << 5;
      input[i] = seed & 255;
    }
    for (let block = 0; block < 12; ++block) {
      const start = block * 16;
      input[start] = [0, 255, 42][block % 3];
      input[start + 1] = [255, 0, 42][block % 3];
      let indices = 0n;
      for (let pixel = 0; pixel < 16; ++pixel) indices |= BigInt(pixel % 8) << BigInt(pixel * 3);
      for (let byte = 0; byte < 6; ++byte)
        input[start + 2 + byte] = Number((indices >> BigInt(byte * 8)) & 255n);
      input[start + 8] = 0;
      input[start + 9] = 0;
      input[start + 10] = 255;
      input[start + 11] = 255;
      input.fill(0xe4, start + 12, start + 16);
    }
    return input;
  }
  test("all synthetic bytes match actual native NumPy decoder and native block-grid stitching", (t) => {
    const numpy = spawnSync("python3", ["-c", "import numpy"], { encoding: "utf8" });
    if (numpy.error || numpy.status !== 0)
      return t.skip("Existing Python/NumPy unavailable; no install attempted");
    const input = fixture();
    const result = spawnSync(
      "python3",
      [fileURLToPath(new URL("./bc3-native-reference.py", import.meta.url))],
      {
        input: JSON.stringify({ bc3: Buffer.from(input).toString("base64") }),
        encoding: "utf8",
        maxBuffer: 1024 * 1024,
      },
    );
    assert.equal(result.status, 0, result.stderr);
    const expected = JSON.parse(result.stdout);
    assert.deepEqual(decodeBC3Blocks(input), new Uint8Array(Buffer.from(expected.rgba, "base64")));
    const grid = decodeBC3BlockGrid(input);
    assert.equal(grid.width, expected.width);
    assert.equal(grid.height, expected.height);
    assert.equal(grid.paddedBlocks, 1017);
    assert.deepEqual(
      new Uint8Array(grid.rgba),
      new Uint8Array(Buffer.from(expected.grid, "base64")),
    );
  });
  test("unconditional four-color mode, alpha special indices, views and no input mutation", () => {
    const raw = fixture().subarray(0, 16),
      copy = raw.slice();
    const rgba = decodeBC3Blocks(raw);
    assert.deepEqual([...rgba.subarray(8, 11)], [85, 85, 85]);
    assert.deepEqual([...rgba.subarray(12, 15)], [170, 170, 170]);
    assert.equal(rgba[6 * 4 + 3], 0);
    assert.equal(rgba[7 * 4 + 3], 255);
    assert.deepEqual(raw, copy);
    const padded = new Uint8Array(48);
    padded.set(raw, 16);
    assert.deepEqual(decodeBC3Blocks(padded.subarray(16, 32)), rgba);
    assert.deepEqual(new Uint8Array(decodeBC3BlockGrid(raw, { rowBlocks: 1 }).rgba), rgba);
  });
  test("reject malformed input and padded image overflow; largest primitive stays bounded", () => {
    for (const raw of [
      new Uint8Array(0),
      new Uint8Array(15),
      new Uint8Array(MAX_BC3_BLOCKS * 16 + 16),
      new ArrayBuffer(16),
    ])
      assert.throws(() => decodeBC3Blocks(raw));
    if (typeof SharedArrayBuffer !== "undefined")
      assert.throws(() => decodeBC3Blocks(new Uint8Array(new SharedArrayBuffer(16))));
    for (const rowBlocks of [0, -1, 1.5, NaN, Infinity, 1025])
      assert.throws(() => decodeBC3BlockGrid(new Uint8Array(16), { rowBlocks }));
    const max = new Uint8Array(MAX_BC3_BLOCKS * 16);
    assert.equal(decodeBC3Blocks(max).byteLength, 4 * 1024 * 1024);
    const grid = decodeBC3BlockGrid(max);
    assert.equal(grid.rgba.byteLength, 4 * 1024 * 1024);
    assert.equal(grid.paddedBlocks, 0);
    assert.throws(() => decodeBC3BlockGrid(max, { rowBlocks: 1023 }));
    assert.throws(() => decodeBC3BlockGrid(max, { rowBlocks: 1 }));
  });
}

// bc3-astc-batches: preserve the original fixture scope and every test case.
{
  // 仅用注入的模拟编码器验证批处理契约，不验证真实 WASM 转码或保真度。
  // Test batch orchestration using an injected fake encoder only; not real WASM transcoding or fidelity.

  const readSource = (size) => ({
    size,
    async readAt(offset, length) {
      return new Uint8Array(length);
    },
  });
  const fakeCodec = async ({ width, height }) => new ArrayBuffer(width * height);
  async function collect(iterator) {
    const output = [];
    for await (const chunk of iterator) output.push(chunk);
    return output;
  }
  test("fake codec: serial lazy batches, ordered raw blocks, exact tail trimming and monotonic counts", async () => {
    const reads = [],
      calls = [];
    let running = 0,
      batch = 0;
    const source = {
      size: (65536 + 3) * 16,
      async readAt(offset, length) {
        reads.push([offset, length]);
        return new Uint8Array(length);
      },
    };
    const encode = async ({ width, height, profile, rgba }) => {
      assert.equal(++running, 1);
      assert.equal(profile, "srgb");
      assert.equal(rgba.byteLength, width * height * 4);
      calls.push([width, height]);
      await Promise.resolve();
      const bytes = new Uint8Array(width * height);
      for (let block = 0; block < bytes.length / 16; ++block)
        bytes.fill((batch * 11 + block) % 251, block * 16, block * 16 + 16);
      batch++;
      running--;
      return bytes.buffer;
    };
    const iterator = transcodeBC3Batches(source, { profile: "srgb", encode });
    assert.equal(reads.length, 0);
    const first = (await iterator.next()).value;
    assert.equal(reads.length, 1);
    assert.equal(first.blocks.byteLength, 1048576);
    assert.equal(first.blockOffset, 0);
    assert.equal(first.completedBlocks, 65536);
    assert.equal(first.blocks[16], 1);
    await Promise.resolve();
    assert.equal(reads.length, 1);
    const second = (await iterator.next()).value;
    assert.equal(second.blocks.byteLength, 48);
    assert.equal(second.blocks.buffer.byteLength, 48);
    assert.deepEqual(
      [...second.blocks],
      [
        ...new Uint8Array(16).fill(11),
        ...new Uint8Array(16).fill(12),
        ...new Uint8Array(16).fill(13),
      ],
    );
    assert.equal(second.blockOffset, 65536);
    assert.equal(second.completedBlocks, 65539);
    assert.equal(second.totalBlocks, 65539);
    assert.equal((await iterator.next()).done, true);
    assert.deepEqual(reads, [
      [0, 1048576],
      [1048576, 48],
    ]);
    assert.deepEqual(calls, [
      [4096, 256],
      [4096, 4],
    ]);
  });
  test("fake codec: invalid sizes, short reads, explicit profile, encoded output, and empty source", async () => {
    for (const size of [-16, 1, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1])
      await assert.rejects(
        collect(transcodeBC3Batches(readSource(size), { profile: "linear", encode: fakeCodec })),
      );
    for (const profile of [undefined, "hdr", null])
      await assert.rejects(
        collect(transcodeBC3Batches(readSource(16), { profile, encode: fakeCodec })),
      );
    for (const batchBlocks of [0, 65537, 1.5])
      await assert.rejects(
        collect(
          transcodeBC3Batches(readSource(16), {
            profile: "linear",
            batchBlocks,
            encode: fakeCodec,
          }),
        ),
      );
    await assert.rejects(
      collect(
        transcodeBC3Batches(
          { size: 16, readAt: () => new Uint8Array(15) },
          { profile: "linear", encode: () => assert.fail("Must not encode short read") },
        ),
      ),
      /short/,
    );
    await assert.rejects(
      collect(
        transcodeBC3Batches(readSource(16), {
          profile: "linear",
          encode: () => new ArrayBuffer(16),
        }),
      ),
      /padded/,
    );
    assert.deepEqual(
      await collect(
        transcodeBC3Batches(
          { size: 0, readAt: () => assert.fail("No empty read") },
          { profile: "linear", encode: fakeCodec },
        ),
      ),
      [],
    );
  });
  test("fake codec: abort before read, while read hangs, during codec, and no reads after consumer stop", async () => {
    const before = new AbortController();
    before.abort();
    await assert.rejects(
      collect(
        transcodeBC3Batches(readSource(16), {
          profile: "linear",
          signal: before.signal,
          encode: fakeCodec,
        }),
      ),
      { name: "AbortError" },
    );
    for (const phase of ["read", "encode"]) {
      const controller = new AbortController();
      let started;
      const ready = new Promise((resolve) => {
        started = resolve;
      });
      const hang = (_a, _b, options) => {
        assert.equal(options?.signal ?? _b.signal, controller.signal);
        started();
        return new Promise(() => {});
      };
      const source = phase === "read" ? { size: 16, readAt: hang } : readSource(16);
      const result = collect(
        transcodeBC3Batches(source, {
          profile: "linear",
          signal: controller.signal,
          encode: phase === "encode" ? hang : fakeCodec,
        }),
      );
      await ready;
      controller.abort();
      await assert.rejects(result, { name: "AbortError" });
    }
    let reads = 0;
    const source = {
      size: 48,
      readAt: (_offset, length) => {
        reads++;
        return new Uint8Array(length);
      },
    };
    for await (const chunk of transcodeBC3Batches(source, {
      profile: "linear",
      batchBlocks: 1,
      encode: fakeCodec,
    })) {
      assert.equal(chunk.blockCount, 1);
      break;
    }
    assert.equal(reads, 1);
  });
  // 将输入端点映射为模拟输出标记，验证跨批读取偏移与实际解码图像顺序。
  // Map input endpoints to fake output markers to verify cross-batch offsets and actual decoded image order.
  test("fake codec: source block identity survives decoding and multiple batches", async () => {
    const input = new Uint8Array(7 * 16);
    for (let block = 0; block < 7; ++block) {
      input[block * 16] = 255;
      input[block * 16 + 9] = (block + 1) << 3;
    }
    const source = {
      size: input.length,
      readAt: (offset, length) => input.subarray(offset, offset + length),
    };
    const encode = async ({ rgba, width, height }) => {
      const pixels = new Uint8Array(rgba),
        output = new Uint8Array(width * height);
      for (let block = 0; block < output.length / 16; ++block) {
        const x = block % (width / 4),
          y = Math.floor(block / (width / 4));
        output.fill(pixels[(y * 4 * width + x * 4) * 4], block * 16, block * 16 + 16);
      }
      return output.buffer;
    };
    const markers = [];
    for await (const { blocks } of transcodeBC3Batches(source, {
      profile: "linear",
      batchBlocks: 3,
      encode,
    }))
      for (let byte = 0; byte < blocks.length; byte += 16) markers.push(blocks[byte]);
    assert.deepEqual(markers, [8, 16, 24, 33, 41, 49, 57]);
  });
}

// texture-stream: preserve the original fixture scope and every test case.
{
  // 使用合成容器和模拟编码器测试结构语义；不验证真实游戏或真实 ASTC 转码。
  // Test structural semantics with synthetic containers and a fake encoder; not real game or ASTC transcode validation.

  function fixture(format = 0x3d, start = 56, blocks = 4) {
    const bytes = new Uint8Array(start + blocks * 16);
    bytes.fill(37, 0, start);
    bytes[4] = format;
    new DataView(bytes.buffer).setUint32(40, start - 40, true);
    return bytes;
  }
  const sourceOf = (bytes) => ({
    size: bytes.length,
    readAt: (offset, length) => bytes.subarray(offset, offset + length),
  });
  const fake = async ({ width, height }) => new Uint8Array(width * height).fill(0xa5).buffer;
  async function collect(stream) {
    const items = [];
    for await (const item of stream) items.push(item);
    return items;
  }
  test("fake codec: both format mappings, exact header preservation, tail positions, and completion", async () => {
    for (const [format, target, profile] of [
      [0x3d, 0x5f, "linear"],
      [0x3e, 0x51, "srgb"],
    ]) {
      const input = fixture(format),
        original = input.slice();
      const events = await collect(
        transcodeTextureStream(sourceOf(input), {
          batchBlocks: 3,
          encode: (image) => {
            assert.equal(image.profile, profile);
            return fake(image);
          },
        }),
      );
      const output = new Uint8Array(input.length);
      let cursor = 0;
      for (const event of events)
        if (event.bytes) {
          assert.equal(event.offset, cursor);
          output.set(event.bytes, event.offset);
          cursor += event.bytes.length;
        }
      assert.equal(cursor, input.length);
      const expectedHeader = input.slice(0, 56);
      expectedHeader[4] = target;
      assert.deepEqual(output.slice(0, 56), expectedHeader);
      assert.deepEqual(output.slice(56), new Uint8Array(64).fill(0xa5));
      assert.deepEqual(input, original);
      assert.deepEqual(
        events.filter((x) => x.kind === "blocks").map((x) => x.completedBlocks),
        [3, 4],
      );
      assert.equal(events.at(-1).kind, "complete");
      assert.equal(events.at(-1).profile, profile);
    }
  });
  test("bounded header chunks and lazy early stop avoid reading the texture tail", async () => {
    const input = fixture(0x3d, TEXTURE_HEADER_CHUNK_BYTES + 73),
      reads = [];
    const source = {
      size: input.length,
      readAt: (offset, length) => {
        reads.push([offset, length]);
        return input.subarray(offset, offset + length);
      },
    };
    const stream = transcodeTextureStream(source, { encode: fake });
    assert.equal(reads.length, 0);
    const first = (await stream.next()).value;
    assert.equal(first.bytes.length, 48);
    assert.deepEqual(reads, [[0, 48]]);
    await stream.return();
    assert.equal(reads.length, 1);
    reads.length = 0;
    const events = await collect(transcodeTextureStream(source, { encode: fake }));
    assert.deepEqual(
      events.filter((x) => x.kind === "header").map((x) => x.bytes.length),
      [48, 65536, 25],
    );
    assert.ok(
      reads
        .filter(([offset]) => offset < 65609)
        .every(([, length]) => length <= TEXTURE_HEADER_CHUNK_BYTES),
    );
  });
  test("explicit unsupported/short skips; malformed offsets and short reads fail safely", async () => {
    assert.deepEqual(
      await collect(
        transcodeTextureStream({ size: 47, readAt: () => assert.fail("No short input read") }),
      ),
      [{ kind: "skip", reason: "short-input" }],
    );
    const unsupported = fixture(7);
    assert.deepEqual(await collect(transcodeTextureStream(sourceOf(unsupported))), [
      { kind: "skip", reason: "unsupported-format", format: 7 },
    ]);
    for (const start of [40, 47, 57, 120, 0xffffffff + 40]) {
      const input = fixture();
      new DataView(input.buffer).setUint32(40, start - 40, true);
      await assert.rejects(
        collect(transcodeTextureStream(sourceOf(input), { encode: fake })),
        /bounds/,
      );
    }
    await assert.rejects(
      collect(
        transcodeTextureStream({ size: 64, readAt: () => new Uint8Array(47) }, { encode: fake }),
      ),
      /short texture read/,
    );
    for (const size of [-1, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1])
      await assert.rejects(collect(transcodeTextureStream({ size, readAt() {} })), /safe-sized/);
    const stream = transcodeTextureStream(sourceOf(fixture()), { batchBlocks: 0, encode: fake });
    await assert.rejects(stream.next(), /batch/);
  });
  test("abort pending header read and abort after header prevent completion and further reads", async () => {
    const controller = new AbortController();
    let started;
    const ready = new Promise((resolve) => {
      started = resolve;
    });
    const source = {
      size: 64,
      readAt: (_offset, _length, { signal }) => {
        assert.equal(signal, controller.signal);
        started();
        return new Promise(() => {});
      },
    };
    const result = collect(
      transcodeTextureStream(source, { signal: controller.signal, encode: fake }),
    );
    await ready;
    controller.abort();
    await assert.rejects(result, { name: "AbortError" });
    const nextController = new AbortController();
    let reads = 0;
    const bytes = fixture();
    const stream = transcodeTextureStream(
      {
        size: bytes.length,
        readAt: (offset, length) => {
          reads++;
          return bytes.subarray(offset, offset + length);
        },
      },
      { signal: nextController.signal, encode: fake },
    );
    assert.equal((await stream.next()).value.kind, "header");
    nextController.abort();
    await assert.rejects(stream.next(), { name: "AbortError" });
    assert.equal(reads, 1);
  });
  // 外部读取和编码错误仅报告固定类别，不泄漏调用者的私有路径或文件名。
  // External read and codec failures expose fixed categories, not caller-private paths or filenames.
  test("source and injected codec failure messages are sanitized", async () => {
    const secret = "/private/user/example.texture";
    await assert.rejects(
      collect(
        transcodeTextureStream({
          size: 64,
          readAt() {
            throw new Error(secret);
          },
        }),
      ),
      (error) => error.message === "Texture read failed" && !error.stack.includes(secret),
    );
    await assert.rejects(
      collect(
        transcodeTextureStream(sourceOf(fixture()), {
          encode() {
            throw new Error(secret);
          },
        }),
      ),
      (error) =>
        error.message === "Texture block conversion failed" && !error.stack.includes(secret),
    );
  });
}

// descriptor-cache: preserve the original fixture scope and every test case.
{
  // 用手写缩略图与注入编码器检查原生描述布局；不读取游戏或运行真实编码器。
  // Check native descriptor layout with authored thumbnails and an injected codec; no game or real encoder.

  const json = (value) => new TextEncoder().encode(JSON.stringify(value));
  const read = (result) => JSON.parse(new TextDecoder().decode(result.bytes));
  const base64 = (bytes) => Buffer.from(bytes).toString("base64");
  const from64 = (value) => new Uint8Array(Buffer.from(value, "base64"));
  function thumbnail() {
    const b = new Uint8Array(112).fill(177),
      v = new DataView(b.buffer);
    v.setUint32(0, 2, true);
    v.setInt32(4, 4, true);
    v.setUint32(8, 16, true);
    v.setInt32(12, 48 - 12, true);
    v.setUint32(16, 16, true);
    v.setInt32(20, 80 - 20, true);
    b.fill(0, 48, 64);
    b.fill(0, 80, 96);
    return b;
  }
  const entry = (filename = "one.texture", format = 61, b = thumbnail()) => ({
    filename,
    format,
    thumbnailBase64: base64(b),
    other: { width: 7, note: "保留", values: [null, true, 2.5] },
  });
  const fake = async ({ width, height }) => new ArrayBuffer(width * height);
  test("selected formats and metadata; serial mips preserve header, gaps, trailer and caller input", async () => {
    const original = thumbnail(),
      rows = [
        entry("ONE.TEXTURE"),
        entry("two.texture", 62),
        entry("skip.texture", 61),
        { filename: "unsupported.texture", format: 60, thumbnailBase64: "unparsed" },
      ];
    const input = json(rows),
      before = input.slice(),
      calls = [];
    let running = 0;
    const result = await transformTextureDescriptorCache(
      input,
      new Map([
        ["one.texture", false],
        ["two.texture", true],
        ["unsupported.texture", false],
      ]),
      {
        encode: async ({ width, height, profile }) => {
          assert.equal(++running, 1);
          calls.push(profile);
          await Promise.resolve();
          running--;
          return new Uint8Array(width * height).fill(calls.length).buffer;
        },
        batchBlocks: 1,
      },
    );
    assert.deepEqual(calls, ["linear", "linear", "srgb", "srgb"]);
    assert.deepEqual(input, before);
    const output = read(result);
    assert.equal(result.convertedEntries, 2);
    assert.equal(result.thumbnailCount, 2);
    assert.equal(output[0].format, 95);
    assert.equal(output[1].format, 81);
    assert.deepEqual(output[0].other, rows[0].other);
    assert.deepEqual(output.slice(2), rows.slice(2));
    for (let j = 0; j < 2; j++) {
      const b = from64(output[j].thumbnailBase64);
      for (let i = 0; i < b.length; i++)
        assert.equal(
          b[i],
          i >= 48 && i < 64 ? j * 2 + 1 : i >= 80 && i < 96 ? j * 2 + 2 : original[i],
        );
    }
  });
  test("empty, absent and null thumbnails retain shape; unselected malformed thumbnail stays untouched", async () => {
    const rows = [
      { filename: "a", format: 61 },
      { filename: "b", format: 62, thumbnailBase64: "" },
      { filename: "c", format: 61, thumbnailBase64: null },
      { filename: "d", format: 62, thumbnailBase64: "bad" },
    ];
    const r = await transformTextureDescriptorCache(
      json(rows),
      new Map([
        ["a", false],
        ["b", true],
        ["c", false],
      ]),
      { encode: () => assert.fail("No codec") },
    );
    assert.deepEqual(
      read(r),
      rows.map((e, i) => (i < 3 ? { ...e, format: i === 1 ? 81 : 95 } : e)),
    );
    assert.equal(r.thumbnailCount, 0);
  });
  test("all thumbnail metadata is checked before first codec call, including profile mismatch", async () => {
    let calls = 0;
    for (const bad of [entry("b", 62), { ...entry("b"), thumbnailBase64: "bad" }])
      await assert.rejects(
        transformTextureDescriptorCache(
          json([entry("a"), bad]),
          new Map([
            ["a", false],
            ["b", false],
          ]),
          {
            encode: () => {
              calls++;
              return fake({ width: 4096, height: 4 });
            },
          },
        ),
        /ASTC_DESCRIPTOR_INVALID/,
      );
    assert.equal(calls, 0);
  });
  test("signed pointers, counts, lengths, table bounds and overlapping payload ranges reject", async () => {
    const mutations = [
      (v) => v.setUint32(0, 0, true),
      (v) => v.setUint32(0, 65, true),
      (v) => v.setInt32(4, -8, true),
      (v) => v.setInt32(4, 100, true),
      (v) => v.setInt32(12, -40, true),
      (v) => v.setInt32(12, 0x7fffffff, true),
      (v) => v.setUint32(8, 0, true),
      (v) => v.setUint32(8, 15, true),
      (v) => v.setUint32(8, 128, true),
      (v) => v.setInt32(20, 48 - 20, true),
      (v) => v.setInt32(12, 8 - 12, true),
    ];
    for (const mutate of mutations) {
      const b = thumbnail();
      mutate(new DataView(b.buffer));
      await assert.rejects(
        transformTextureDescriptorCache(json([entry("a", 61, b)]), new Map([["a", false]]), {
          encode: () => assert.fail("Invalid layout"),
        }),
        /ASTC_DESCRIPTOR_INVALID/,
      );
    }
  });
  test("table order, not physical order, determines codec sequence; gaps remain legal", async () => {
    const b = thumbnail(),
      v = new DataView(b.buffer);
    v.setInt32(12, 80 - 12, true);
    v.setInt32(20, 48 - 20, true);
    let n = 0;
    const r = await transformTextureDescriptorCache(
      json([entry("a", 61, b)]),
      new Map([["a", false]]),
      { encode: async ({ width, height }) => new Uint8Array(width * height).fill(++n).buffer },
    );
    const out = from64(read(r)[0].thumbnailBase64);
    assert.equal(out[80], 1);
    assert.equal(out[48], 2);
    assert.equal(out[64], 177);
  });
  test("input bounds, canonical base64, map shape and unsafe metadata reject without private errors", async () => {
    for (const input of [
      new Uint8Array([255]),
      json({}),
      json([{}]),
      json([{ filename: "a", format: 61, n: 1e30 }]),
      new Uint8Array(DESCRIPTOR_CACHE_LIMITS.inputBytes + 1),
    ])
      await assert.rejects(
        transformTextureDescriptorCache(input, new Map()),
        /ASTC_DESCRIPTOR_INVALID/,
      );
    for (const map of [{}, new Map([["A", false]]), new Map([["a", "linear"]])])
      await assert.rejects(
        transformTextureDescriptorCache(json([]), map),
        /ASTC_DESCRIPTOR_INVALID/,
      );
    for (const value of [
      " " + base64(thumbnail()),
      base64(thumbnail()).replace(/=$/, ""),
      "AB==",
      9,
    ])
      await assert.rejects(
        transformTextureDescriptorCache(
          json([{ filename: "secret.texture", format: 61, thumbnailBase64: value }]),
          new Map([["secret.texture", false]]),
        ),
        { message: "ASTC_DESCRIPTOR_INVALID" },
      );
  });
  test("codec errors are sanitized and abort interrupts a hung codec without exposing filenames", async () => {
    const input = json([entry("private.texture")]),
      map = new Map([["private.texture", false]]);
    await assert.rejects(
      transformTextureDescriptorCache(input, map, {
        encode: () => {
          throw Error("private.texture /secrets");
        },
      }),
      { message: "ASTC_DESCRIPTOR_TRANSCODE" },
    );
    const c = new AbortController();
    let start;
    const started = new Promise((r) => {
      start = r;
    });
    const pending = transformTextureDescriptorCache(input, map, {
      signal: c.signal,
      encode: () => {
        start();
        return new Promise(() => {});
      },
    });
    await started;
    c.abort();
    await assert.rejects(pending, { name: "AbortError" });
    await assert.rejects(transformTextureDescriptorCache(input, map, { signal: c.signal }), {
      name: "AbortError",
    });
  });
}
