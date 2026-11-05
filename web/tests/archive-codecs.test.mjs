// Feature suite: archive-codecs. Original test sections retain independent lexical scopes.
import * as strictModule from "node:assert/strict";
import * as nodeCryptoModule from "node:crypto";
import * as nodeChild_processModule from "node:child_process";
import * as nodeZlibModule from "node:zlib";
import * as nodeUrlModule from "node:url";
import * as nodeTestModule from "node:test";
import * as archiveReaderModule from "../archive-reader.mjs";
import * as archiveStreamReaderModule from "../archive-stream-reader.mjs";
import * as archiveReadRpcModule from "../archive-read-rpc.mjs";
import * as zipReaderModule from "../zip-reader.mjs";
import * as zipWriterModule from "../zip-writer.mjs";
import * as zip64WriterModule from "../zip64-writer.mjs";
import * as promisesModule from "node:fs/promises";
import * as nodeOsModule from "node:os";
import * as nodePathModule from "node:path";
import * as stagedZipValidatorModule from "../staged-zip-validator.mjs";
import * as deflateBoundaryModule from "../deflate-boundary.mjs";
import * as nodeFsModule from "node:fs";
import * as codecModule from "../toolchain/png-codec-exact/codec.mjs";
import * as sourceDownloadModule from "../toolchain/png-codec-exact/source-download.mjs";
import { createMD5 } from "hash-wasm";
import * as checksumVerifierModule from "../checksum-verifier.mjs";
import * as preflightModule from "../preflight.mjs";

// archive-reader
{
  // 验证 archive-reader 的约定与边界；依据 ../archive-reader.mjs 及本文件的测试断言。
  // Verify archive-reader contracts and boundaries against ../archive-reader.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { execFileSync } = nodeChild_processModule;
  const { deflateSync } = nodeZlibModule;
  const { fileURLToPath } = nodeUrlModule;
  const { default: test } = nodeTestModule;
  const { archiveLocator, ARCHIVE_LIMITS, decodeArchiveRecord } = archiveReaderModule;

  // Python 标准库构造样例，然后由真实原生 dmx.py 用内存归档独立解码；不以复制的 JS 解码器作为参考。
  // Python stdlib constructs fixtures, then actual native dmx.py independently decodes
  // them using an in-memory archive. No copied JS decoder serves as the oracle.
  const fixtures = JSON.parse(
    execFileSync(
      "python3",
      [
        "-c",
        String.raw`
import hashlib, importlib.util, io, json, pathlib, struct, sys, zlib
spec = importlib.util.spec_from_file_location('dmx', pathlib.Path(sys.argv[1]) / 'dm/tools/dmx.py')
dmx = importlib.util.module_from_spec(spec); spec.loader.exec_module(dmx)
content = bytes(range(256)) * 32 + b'bounded archive fixture'
parts = [content[:97], content[97:]]
blocks = [b'N' + parts[0], b'Z' + zlib.compress(parts[1])]
header = b'BLTE' + struct.pack('>I', 12 + 24 * len(blocks)) + b'\x0f\x00\x00\x02'
for block, part in zip(blocks, parts):
    header += struct.pack('>II', len(block), len(part)) + hashlib.md5(block).digest()
result = []
for encoding, raw in [('n', content), ('z', zlib.compress(content)), ('b:synthetic', header + b''.join(blocks))]:
    ckey = hashlib.md5(content).hexdigest()
    row = ['0','0','0','0',ckey,str(len(content)),'00'*16,str(len(raw)),encoding,'fixture','0']
    dmx._FH[(0,0)] = io.BytesIO(raw)
    decoded = dmx.read_row(row)
    result.append(dict(raw=raw.hex(), decoded=decoded.hex(), record=dict(offset=0, encodedSize=len(raw), decodedSize=len(content), encoding=encoding, ckey=ckey)))
print(json.dumps(result))
`,
        fileURLToPath(new URL("../../", import.meta.url)),
      ],
      { encoding: "utf8" },
    ),
  );
  const md5 = (bytes) => createHash("md5").update(bytes).digest("hex");
  const verifyMd5 = async (bytes, expected) => md5(bytes) === expected;
  function source(bytes, base = 0) {
    const reads = [];
    return {
      size: base + bytes.length,
      reads,
      async read(offset, length) {
        reads.push([offset, length]);
        assert.ok(offset >= base);
        assert.ok(length <= ARCHIVE_LIMITS.encodedBlock);
        return bytes.slice(offset - base, offset - base + length);
      },
    };
  }
  const blte = fixtures[2];
  const decode = (bytes, record = blte.record, options = {}) =>
    decodeArchiveRecord(source(bytes), record, { verifyMd5, ...options });
  for (const fixture of fixtures) {
    test(`Python/native parity: ${fixture.record.encoding}`, async () => {
      const bytes = Buffer.from(fixture.raw, "hex");
      const input = source(bytes);
      assert.equal(
        Buffer.from(await decodeArchiveRecord(input, fixture.record, { verifyMd5 })).toString(
          "hex",
        ),
        fixture.decoded,
      );
      assert.ok(input.reads.length >= 2);
    });
  }
  test("40-bit locator and sparse >4 GiB reads preserve exact offsets", async () => {
    const base = 0x123456789a;
    assert.deepEqual(archiveLocator("0000000000000000000203123456789a"), {
      chunk: 2,
      archive: 3,
      offset: base,
      filename: "02-00000003.data",
    });
    const input = source(Buffer.from(blte.raw, "hex"), base);
    const result = await decodeArchiveRecord(
      input,
      { ...blte.record, offset: base },
      { verifyMd5 },
    );
    assert.equal(Buffer.from(result).toString("hex"), blte.decoded);
    assert.ok(input.reads.every(([offset]) => offset >= base));
    assert.throws(() => archiveLocator("bad"), /EKey/);
  });
  test("large raw records are read in bounded chunks", async () => {
    const bytes = Buffer.alloc(4 * 1024 * 1024 + 257, 0x5a);
    const input = source(bytes);
    const decoded = await decodeArchiveRecord(
      input,
      {
        offset: 0,
        encodedSize: bytes.length,
        decodedSize: bytes.length,
        encoding: "n",
        ckey: md5(bytes),
      },
      { verifyMd5 },
    );
    assert.equal(Buffer.compare(Buffer.from(decoded), bytes), 0);
    assert.ok(input.reads.length >= 3);
    assert.ok(input.reads.every(([, length]) => length <= 4 * 1024 * 1024));
  });
  test("single compressed records may expand beyond the old 16 MiB block limit", async () => {
    const decoded = Buffer.alloc(17 * 1024 * 1024, 0x5a);
    const encoded = deflateSync(decoded);
    const result = await decodeArchiveRecord(
      source(encoded),
      {
        offset: 0,
        encodedSize: encoded.length,
        decodedSize: decoded.length,
        encoding: "z",
        ckey: md5(decoded),
      },
      { verifyMd5 },
    );
    assert.equal(Buffer.compare(Buffer.from(result), decoded), 0);
  });
  test("MD5 is mandatory, includes BLTE mode byte, and checks final CKey", async () => {
    const bytes = Buffer.from(blte.raw, "hex");
    await assert.rejects(decodeArchiveRecord(source(bytes), blte.record), /verifier is required/);
    const observed = [];
    await decode(bytes, blte.record, {
      verifyMd5: async (data, expected, context) => {
        observed.push([context.kind, data[0]]);
        return verifyMd5(data, expected);
      },
    });
    assert.deepEqual(observed, [
      ["encoded block", 78],
      ["encoded block", 90],
      ["decoded content", 0],
    ]);
    await assert.rejects(
      decode(bytes, { ...blte.record, ckey: "0".repeat(32) }),
      /MD5 mismatch: decoded/,
    );
    bytes[20] ^= 1;
    await assert.rejects(decode(bytes), /MD5 mismatch: encoded/);
    await assert.rejects(
      decode(Buffer.from(blte.raw, "hex"), blte.record, { verifyMd5: () => undefined }),
      /MD5 mismatch/,
    );
  });
  test("reject malformed metadata before payload reads or oversized allocations", async () => {
    const original = Buffer.from(blte.raw, "hex");
    for (const [field, value] of [
      ["decodedSize", ARCHIVE_LIMITS.decodedRecord + 1],
      ["encodedSize", ARCHIVE_LIMITS.encodedRecord + 1],
      ["offset", Number.MAX_SAFE_INTEGER],
      ["offset", -1],
      ["decodedSize", NaN],
    ]) {
      const input = source(original);
      await assert.rejects(
        decodeArchiveRecord(input, { ...blte.record, [field]: value }, { verifyMd5 }),
      );
      assert.equal(input.reads.length, 0);
    }
    for (const mutate of [
      (b) => b.writeUInt32BE(0, 4),
      (b) => {
        b[8] = 0;
      },
      (b) => {
        b[9] = 255;
      },
      (b) => b.writeUInt32BE(ARCHIVE_LIMITS.encodedBlock + 1, 12),
      (b) => b.writeUInt32BE(ARCHIVE_LIMITS.decodedBlock + 1, 16),
      (b) => b.writeUInt32BE(0, 12),
      (b) => b.writeUInt32BE(1, 16),
    ]) {
      const bytes = Buffer.from(original);
      mutate(bytes);
      const input = source(bytes);
      await assert.rejects(decodeArchiveRecord(input, blte.record, { verifyMd5 }));
      assert.ok(input.reads.every(([offset]) => offset === 0));
    }
  });
  test("truncation, short adapter reads and trailing container bytes fail closed", async () => {
    const bytes = Buffer.from(blte.raw, "hex");
    await assert.rejects(
      decode(bytes.subarray(0, 10), { ...blte.record, encodedSize: 10 }),
      /Truncated/,
    );
    await assert.rejects(decode(bytes.subarray(0, bytes.length - 1)), /outside source/);
    await assert.rejects(
      decode(Buffer.concat([bytes, Buffer.from([0])]), {
        ...blte.record,
        encodedSize: bytes.length + 1,
      }),
      /totals mismatch/,
    );
    await assert.rejects(
      decodeArchiveRecord({ size: bytes.length, read: async () => new Uint8Array() }, blte.record, {
        verifyMd5,
      }),
      /Short/,
    );
  });
  test("unsupported mode is rejected even with valid encoded MD5", async () => {
    const bytes = Buffer.from(blte.raw, "hex");
    bytes[60] = 69;
    Buffer.from(md5(bytes.subarray(60, 158)), "hex").copy(bytes, 20);
    await assert.rejects(decode(bytes), /Unsupported BLTE block mode/);
  });
  test("zlib output is bounded by declared size and malformed streams fail", async () => {
    const fixture = fixtures[1],
      bytes = Buffer.from(fixture.raw, "hex");
    await assert.rejects(decode(bytes, { ...fixture.record, decodedSize: 1 }), /exceeds declared/);
    await assert.rejects(
      decode(bytes, { ...fixture.record, decodedSize: fixture.record.decodedSize + 1 }),
      /length mismatch/,
    );
    const corrupt = Buffer.from(bytes);
    corrupt[corrupt.length - 1] ^= 255;
    await assert.rejects(decode(corrupt, fixture.record));
    await assert.rejects(
      decode(bytes.subarray(0, bytes.length - 2), {
        ...fixture.record,
        encodedSize: bytes.length - 2,
      }),
    );
  });
  test("cancellation before reads and during awaited checksum returns no output", async () => {
    const bytes = Buffer.from(blte.raw, "hex");
    const controller = new AbortController();
    controller.abort();
    const input = source(bytes);
    await assert.rejects(
      decodeArchiveRecord(input, blte.record, { verifyMd5, signal: controller.signal }),
      { name: "AbortError" },
    );
    assert.equal(input.reads.length, 0);
    const next = new AbortController();
    await assert.rejects(
      decode(bytes, blte.record, {
        signal: next.signal,
        verifyMd5: async () => {
          next.abort();
          return true;
        },
      }),
      { name: "AbortError" },
    );
  });
  test("empty native raw record is valid; unsupported encodings and raw sizes are not", async () => {
    const record = {
      offset: 0,
      encodedSize: 0,
      decodedSize: 0,
      encoding: "n",
      ckey: md5(new Uint8Array()),
    };
    assert.equal((await decode(new Uint8Array(), record)).length, 0);
    await assert.rejects(decode(new Uint8Array(), { ...record, encoding: "e" }), /Unsupported/);
    await assert.rejects(decode(new Uint8Array(), { ...record, decodedSize: 1 }), /Raw length/);
  });
  test("abort during decompression cancels stream and never returns partial bytes", async () => {
    const original = globalThis.DecompressionStream;
    const controller = new AbortController();
    let transformed = false;
    // 确定性的流调度接缝；真实 zlib 正确性已在上文覆盖。
    // Deterministic stream scheduling seam; actual zlib correctness is covered above.
    globalThis.DecompressionStream = class {
      constructor(format) {
        assert.equal(format, "deflate");
        return new TransformStream({
          transform(_chunk, stream) {
            transformed = true;
            stream.enqueue(new Uint8Array([1]));
            controller.abort();
          },
        });
      }
    };
    try {
      const fixture = fixtures[1];
      await assert.rejects(
        decode(Buffer.from(fixture.raw, "hex"), fixture.record, { signal: controller.signal }),
        { name: "AbortError" },
      );
      assert.ok(transformed);
    } finally {
      globalThis.DecompressionStream = original;
    }
  });
  test("compressed input uses small sequential writes, not a whole-block enqueue", async () => {
    const original = globalThis.DecompressionStream;
    const sizes = [];
    let active = 0,
      peak = 0;
    globalThis.DecompressionStream = class {
      constructor() {
        return new TransformStream({
          async transform(chunk, controller) {
            active++;
            peak = Math.max(peak, active);
            sizes.push(chunk.length);
            await Promise.resolve();
            controller.enqueue(chunk);
            active--;
          },
        });
      }
    };
    try {
      const bytes = new Uint8Array(ARCHIVE_LIMITS.compressedSlice * 4 + 17).fill(7);
      const result = await decode(bytes, {
        offset: 0,
        encodedSize: bytes.length,
        decodedSize: bytes.length,
        encoding: "z",
        ckey: md5(bytes),
      });
      assert.deepEqual(result, bytes);
      assert.deepEqual(sizes, [1024, 1024, 1024, 1024, 17]);
      assert.equal(peak, 1);
    } finally {
      globalThis.DecompressionStream = original;
    }
  });
  test("output overflow stops compressed producer rather than submitting remaining slices", async () => {
    const original = globalThis.DecompressionStream;
    let writes = 0;
    globalThis.DecompressionStream = class {
      constructor() {
        return new TransformStream({
          transform(chunk, controller) {
            writes++;
            assert.ok(chunk.length <= ARCHIVE_LIMITS.compressedSlice);
            controller.enqueue(new Uint8Array(2));
          },
        });
      }
    };
    try {
      const bytes = new Uint8Array(ARCHIVE_LIMITS.compressedSlice * 20);
      await assert.rejects(
        decode(bytes, {
          offset: 0,
          encodedSize: bytes.length,
          decodedSize: 1,
          encoding: "z",
          ckey: "0".repeat(32),
        }),
        /exceeds declared/,
      );
      assert.equal(writes, 1);
    } finally {
      globalThis.DecompressionStream = original;
    }
  });
  test("abort while a write is pending cancels reader, aborts writer and settles producer", async () => {
    const original = globalThis.DecompressionStream;
    const aborter = new AbortController();
    let cancelled = false,
      aborted = false,
      writeSettled = false,
      writes = 0;
    let release;
    globalThis.DecompressionStream = class {
      constructor() {
        this.readable = new ReadableStream({
          cancel() {
            cancelled = true;
            release();
          },
        });
        this.writable = new WritableStream({
          async write() {
            writes++;
            await new Promise((resolve) => {
              release = resolve;
              queueMicrotask(() => aborter.abort());
            });
            writeSettled = true;
          },
          abort() {
            aborted = true;
          },
        });
      }
    };
    try {
      const bytes = new Uint8Array(ARCHIVE_LIMITS.compressedSlice * 3);
      await assert.rejects(
        decode(
          bytes,
          {
            offset: 0,
            encodedSize: bytes.length,
            decodedSize: 1,
            encoding: "z",
            ckey: "0".repeat(32),
          },
          { signal: aborter.signal },
        ),
        { name: "AbortError" },
      );
      assert.ok(cancelled && aborted && writeSettled);
      assert.equal(writes, 1);
    } finally {
      globalThis.DecompressionStream = original;
    }
  });
  test("high-expansion zlib record is rejected after bounded input submission", async () => {
    // 生成 128 MiB 逻辑输出而不整体分配：Python 仅保留一个 64 KiB 合成块和约 128 KiB 压缩样例，解码器只声明 1 字节。
    // Generate 128 MiB logical output without allocating it: Python keeps one 64 KiB
    // synthetic chunk and the ~128 KiB compressed fixture. Decoder declares only 1 byte.
    const bytes = execFileSync("python3", [
      "-c",
      "import sys,zlib; c=zlib.compressobj(); chunk=bytes(65536); [sys.stdout.buffer.write(c.compress(chunk)) for _ in range(2048)]; sys.stdout.buffer.write(c.flush())",
    ]);
    assert.ok(bytes.length < ARCHIVE_LIMITS.encodedBlock);
    await assert.rejects(
      decode(bytes, {
        offset: 0,
        encodedSize: bytes.length,
        decodedSize: 1,
        encoding: "z",
        ckey: "0".repeat(32),
      }),
      /exceeds declared/,
    );
  });

  test("rejected or cancelled final checksum wipes decoded output but preserves borrowed input", async () => {
    for (const failure of ["mismatch", "throw", "cancel"]) {
      const input = new Uint8Array([1, 2, 3, 4]),
        controller = new AbortController();
      let decoded;
      await assert.rejects(
        decodeArchiveRecord(
          { size: input.length, read: async (at, count) => input.subarray(at, at + count) },
          {
            offset: 0,
            encodedSize: input.length,
            decodedSize: input.length,
            encoding: "n",
            ckey: md5(input),
          },
          {
            signal: controller.signal,
            async verifyMd5(bytes) {
              decoded = bytes;
              if (failure === "cancel") controller.abort();
              if (failure === "throw") throw new Error("verifier failure");
              return failure !== "mismatch";
            },
          },
        ),
        failure === "cancel" ? { name: "AbortError" } : /MD5 mismatch|verifier failure/,
      );
      assert.deepEqual(decoded, new Uint8Array(input.length));
      assert.deepEqual(input, new Uint8Array([1, 2, 3, 4]));
    }
  });
  test("successful decoding preserves returned bytes and borrowed source views", async () => {
    const input = new Uint8Array([1, 2, 3, 4]);
    const result = await decodeArchiveRecord(
      { size: input.length, read: async (at, count) => input.subarray(at, at + count) },
      {
        offset: 0,
        encodedSize: input.length,
        decodedSize: input.length,
        encoding: "n",
        ckey: md5(input),
      },
      { verifyMd5 },
    );
    assert.notEqual(result.buffer, input.buffer);
    assert.deepEqual(result, new Uint8Array([1, 2, 3, 4]));
    assert.deepEqual(input, result);
  });
}

// archive-stream-reader
{
  // 依据现有解码器与 Node 校验内核，用自造归档验证流式兼容、损坏拒绝和取消清理。
  // Synthetic archive compatibility, corruption and cancellation checks against the existing decoder and Node checksum kernels.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { deflateSync } = nodeZlibModule;
  const { stageArchiveRecord } = archiveStreamReaderModule;
  const { decodeArchiveRecord } = archiveReaderModule;
  const md5 = (bytes) => createHash("md5").update(bytes).digest("hex");
  const digest = (bytes) => Buffer.from(md5(bytes), "hex");
  const BASE = 0x123456789an;
  function fixture(
    encoding,
    content = Buffer.from(Array.from({ length: 150003 }, (_, i) => i % 251)),
  ) {
    let raw, hashBytes;
    if (encoding.startsWith("b:")) {
      const parts = [content.subarray(0, 70000), content.subarray(70000)];
      const payload = [
        Buffer.concat([Buffer.from("N"), parts[0]]),
        Buffer.concat([Buffer.from("Z"), deflateSync(parts[1])]),
      ];
      const header = Buffer.alloc(60);
      header.write("BLTE");
      header.writeUInt32BE(60, 4);
      header[8] = 15;
      header[11] = 2;
      payload.forEach((p, i) => {
        header.writeUInt32BE(p.length, 12 + i * 24);
        header.writeUInt32BE(parts[i].length, 16 + i * 24);
        digest(p).copy(header, 20 + i * 24);
      });
      raw = Buffer.concat([header, ...payload]);
      hashBytes = header;
    } else {
      raw = encoding === "n" ? content : deflateSync(content);
      hashBytes = raw;
    }
    const record = {
      offset: BASE,
      encodedSize: raw.length,
      decodedSize: content.length,
      encoding,
      ckey: md5(content),
      ekey: md5(hashBytes).slice(0, 18) + "0203" + BASE.toString(16).padStart(10, "0"),
    };
    return { raw, record, content };
  }
  function source(raw, base = BASE) {
    const reads = [];
    return {
      size: base + BigInt(raw.length),
      reads,
      readAt(at, length) {
        assert.equal(typeof at, "bigint");
        assert.ok(at >= base);
        assert.ok(length <= 65536);
        reads.push([at, length]);
        const offset = Number(at - base);
        return new Uint8Array(raw.subarray(offset, offset + length));
      },
    };
  }
  function stageFactory({ retain = true } = {}) {
    const state = { appended: 0, finalized: 0, disposed: 0, chunks: [] };
    const createStage = async () => ({
      append(bytes) {
        assert.ok(bytes.buffer.byteLength <= 65536);
        state.appended += bytes.length;
        if (retain) state.chunks.push(bytes.slice());
      },
      finalize() {
        state.finalized++;
        const bytes = retain ? Buffer.concat(state.chunks) : null;
        return {
          size: state.appended,
          readAt: (at, count) =>
            bytes ? new Uint8Array(bytes.subarray(at, at + count)) : new Uint8Array(count),
        };
      },
      dispose() {
        state.disposed++;
        state.chunks = [];
      },
    });
    return { state, createStage };
  }
  const options = (createStage) => ({
    maxEncodedBytes: 32 * 1024 * 1024,
    maxDecodedBytes: 32 * 1024 * 1024,
    createStage,
  });
  for (const encoding of ["n", "z", "b:synthetic"])
    test(`stream matches existing decoder: ${encoding}`, async () => {
      const { raw, record, content } = fixture(encoding),
        staging = stageFactory();
      const input = source(raw);
      const output = await stageArchiveRecord(input, record, options(staging.createStage));
      assert.equal(output.size, content.length);
      assert.equal(staging.state.finalized, 1);
      assert.deepEqual(Buffer.from(await output.readAt(0, output.size)), content);
      const old = await decodeArchiveRecord(
        { size: raw.length, read: (at, count) => raw.subarray(at, at + count) },
        { ...record, offset: 0 },
        { verifyMd5: async (bytes, expected) => md5(bytes) === expected },
      );
      assert.deepEqual(Buffer.from(old), content);
      await output.dispose();
      await assert.rejects(output.readAt(0, 1), /DISPOSED/);
    });
  test("BLTE detection overrides declared n, and empty n/z records pass checksums", async () => {
    const f = fixture("b:synthetic");
    f.record.encoding = "n";
    for (const value of [f, fixture("n", Buffer.alloc(0)), fixture("z", Buffer.alloc(0))]) {
      const staging = stageFactory();
      const result = await stageArchiveRecord(
        source(value.raw),
        value.record,
        options(staging.createStage),
      );
      assert.equal(result.size, value.content.length);
      await result.dispose();
    }
  });
  test("reject checksum, header, mode, extent, budget and short-read corruption without publishing a stage", async () => {
    const cases = [
      (f) => {
        f.record.ckey = "0".repeat(32);
      },
      (f) => {
        f.record.ekey = "0".repeat(18) + f.record.ekey.slice(18);
      },
      (f) => {
        f.record.ekey = f.record.ekey.slice(0, 22) + "0000000000";
      },
      (f) => {
        f.raw[8] = 0;
      },
      (f) => {
        f.raw[20] ^= 1;
        f.record.ekey = md5(f.raw.subarray(0, 60)).slice(0, 18) + f.record.ekey.slice(18);
      },
      (f) => {
        f.raw[60] = 110;
      },
      (f) => {
        f.raw[f.raw.length - 1] ^= 1;
      },
      (f) => {
        f.record.decodedSize--;
      },
      (f) => {
        f.record.encodedSize++;
      },
    ];
    for (const change of cases) {
      const f = fixture("b:synthetic");
      change(f);
      const staging = stageFactory();
      await assert.rejects(
        stageArchiveRecord(source(f.raw), f.record, options(staging.createStage)),
        /ARCHIVE_STREAM_/,
      );
      assert.equal(staging.state.finalized, 0);
      if (staging.state.appended) assert.equal(staging.state.disposed, 1);
    }
    const f = fixture("n");
    await assert.rejects(
      stageArchiveRecord(source(f.raw), f.record, {
        ...options(stageFactory().createStage),
        maxDecodedBytes: 3,
      }),
      /RECORD/,
    );
    await assert.rejects(
      stageArchiveRecord(
        { size: BASE + BigInt(f.raw.length), readAt: () => new Uint8Array(0) },
        f.record,
        options(stageFactory().createStage),
      ),
      /SHORT_READ/,
    );
  });
  test("streaming supports >16MiB BLTE without decoded retention; whole-buffer reader accepts expanded budget", async () => {
    const part = Buffer.alloc(1024 * 1024, 7),
      block = Buffer.concat([Buffer.from("Z"), deflateSync(part)]),
      count = 17;
    const header = Buffer.alloc(12 + 24 * count);
    header.write("BLTE");
    header.writeUInt32BE(header.length, 4);
    header[8] = 15;
    header[11] = count;
    const contentHash = createHash("md5");
    for (let i = 0; i < count; i++) {
      header.writeUInt32BE(block.length, 12 + i * 24);
      header.writeUInt32BE(part.length, 16 + i * 24);
      digest(block).copy(header, 20 + i * 24);
      contentHash.update(part);
    }
    const raw = Buffer.concat([header, ...Array(count).fill(block)]);
    const record = {
      offset: BASE,
      encodedSize: raw.length,
      decodedSize: part.length * count,
      encoding: "b:256K*=z",
      ckey: contentHash.digest("hex"),
      ekey: md5(header).slice(0, 18) + "0203" + BASE.toString(16),
    };
    const staging = stageFactory({ retain: false });
    const result = await stageArchiveRecord(source(raw), record, options(staging.createStage));
    assert.equal(staging.state.appended, 17 * 1024 * 1024);
    assert.equal(staging.state.chunks.length, 0);
    await result.dispose();
    const decoded = await decodeArchiveRecord(
      { size: raw.length, read: (at, n) => raw.subarray(at, at + n) },
      { ...record, offset: 0 },
      { verifyMd5: (bytes, expected) => md5(bytes) === expected },
    );
    assert.equal(decoded.length, 17 * 1024 * 1024);
    assert.ok(decoded.every((byte) => byte === 7));
  });
  test("abort pending archive reads and stage acquisition; late owned stage is disposed", async () => {
    const f = fixture("n");
    const controller = new AbortController();
    let mark;
    const ready = new Promise((resolve) => {
      mark = resolve;
    });
    const result = stageArchiveRecord(
      {
        size: BASE + BigInt(f.raw.length),
        readAt() {
          mark();
          return new Promise(() => {});
        },
      },
      f.record,
      { ...options(stageFactory().createStage), signal: controller.signal },
    );
    await ready;
    controller.abort();
    await assert.rejects(result, { name: "AbortError" });
    const other = new AbortController();
    let acquired, release;
    const acquiring = new Promise((resolve) => {
      acquired = resolve;
    });
    const stage = stageFactory();
    const job = stageArchiveRecord(source(f.raw), f.record, {
      ...options(() => {
        acquired();
        return new Promise((resolve) => {
          release = resolve;
        });
      }),
      signal: other.signal,
    });
    await acquiring;
    other.abort();
    release(await stage.createStage());
    await assert.rejects(job, { name: "AbortError" });
    assert.equal(stage.state.disposed, 1);
  });
  test("abort during compressed output and sanitize external source failures", async () => {
    const f = fixture("z");
    const controller = new AbortController(),
      staging = stageFactory();
    await assert.rejects(
      stageArchiveRecord(source(f.raw), f.record, {
        ...options(async () => {
          const stage = await staging.createStage();
          return {
            ...stage,
            append(bytes) {
              stage.append(bytes);
              controller.abort();
            },
          };
        }),
        signal: controller.signal,
      }),
      { name: "AbortError" },
    );
    assert.equal(staging.state.finalized, 0);
    assert.equal(staging.state.disposed, 1);
    await assert.rejects(
      stageArchiveRecord(
        {
          size: BASE + BigInt(f.raw.length),
          readAt() {
            throw new Error("/private/example");
          },
        },
        f.record,
        options(staging.createStage),
      ),
      (error) =>
        error.message === "ARCHIVE_STREAM_FAILED" && !error.stack.includes("/private/example"),
    );
  });
  // 清理超时保留可观察的所有权 Promise，既不假报已清理，也不遗失迟到资源。
  // A cleanup timeout retains an observable ownership promise; never claim cleanup or lose late resources.
  test("pending cleanup remains observable and late disposal completes", async () => {
    const f = fixture("n");
    let release;
    const cleanup = new Promise((resolve) => {
      release = resolve;
    });
    let failure;
    try {
      await stageArchiveRecord(
        source(f.raw),
        f.record,
        options(async () => ({
          append() {
            throw new Error("write failed");
          },
          finalize() {
            assert.fail("Must not finalize");
          },
          dispose() {
            return cleanup;
          },
        })),
      );
    } catch (error) {
      failure = error;
    }
    assert.equal(failure.message, "ARCHIVE_STREAM_CLEANUP_PENDING");
    assert.ok(failure.cleanup instanceof Promise);
    release();
    await failure.cleanup;
  });
  // 在校验和正确时也拒绝非法模式，并在输出超过声明大小时停止解压。
  // Reject unsupported modes even with valid checksums, and stop inflation at the declared output size.
  test("verified unsupported BLTE mode and valid compressed stream overflow fail closed", async () => {
    const f = fixture("b:synthetic");
    f.raw[60] = 110;
    const blockSize = f.raw.readUInt32BE(12);
    digest(f.raw.subarray(60, 60 + blockSize)).copy(f.raw, 20);
    f.record.ekey = md5(f.raw.subarray(0, 60)).slice(0, 18) + f.record.ekey.slice(18);
    await assert.rejects(
      stageArchiveRecord(source(f.raw), f.record, options(stageFactory().createStage)),
      /BLOCK_MODE/,
    );
    const z = fixture("z");
    z.record.decodedSize = 1;
    const staging = stageFactory();
    await assert.rejects(
      stageArchiveRecord(source(z.raw), z.record, options(staging.createStage)),
      /DECODED_SIZE/,
    );
    assert.equal(staging.state.finalized, 0);
    assert.equal(staging.state.disposed, 1);
  });
  // 普通小块只从认证来源读取一遍负载；大块用两遍有界读取而不扩大缓存。
  // Ordinary small blocks read payload once from the authenticated source; large blocks use two bounded passes without growing the cache.
  test("small encoded payloads are cached once, large raw records use bounded two-pass reads", async () => {
    const f = fixture("b:synthetic"),
      input = source(f.raw),
      staging = stageFactory();
    const result = await stageArchiveRecord(input, f.record, options(staging.createStage));
    const payloadReads = input.reads.filter(([at]) => at >= BASE + 60n);
    assert.equal(
      payloadReads.reduce((total, [, count]) => total + count, 0),
      f.raw.length - 60,
    );
    await result.dispose();
    const large = fixture("n", Buffer.alloc(1024 * 1024 + 16, 11)),
      largeSource = source(large.raw);
    const largeStage = stageFactory({ retain: false });
    const value = await stageArchiveRecord(
      largeSource,
      large.record,
      options(largeStage.createStage),
    );
    assert.equal(
      largeSource.reads.reduce((total, [, count]) => total + count, 0),
      12 + large.raw.length * 2,
    );
    await value.dispose();
  });

  // 取消后迟到的获取错误不能被吞掉；普通主错误和正常取消不重复包装。
  // Retain late acquisition failures after cancellation without duplicating primary failures or normal aborts.
  function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => {
      resolve = yes;
      reject = no;
    });
    return { promise, resolve, reject };
  }
  async function cancelledAcquisition() {
    const f = fixture("n"),
      controller = new AbortController();
    const entered = deferred(),
      acquiring = deferred();
    const job = stageArchiveRecord(source(f.raw), f.record, {
      ...options(() => {
        entered.resolve();
        return acquiring.promise;
      }),
      signal: controller.signal,
    });
    const outcome = job.catch((error) => error);
    await entered.promise;
    controller.abort();
    await new Promise((resolve) => setImmediate(resolve));
    return { acquiring, outcome };
  }
  test("distinct late acquisition rejection remains observable and sanitized", async () => {
    const { acquiring, outcome } = await cancelledAcquisition();
    const failure = new AggregateError(
      [new Error("private cleanup detail")],
      "private acquisition detail",
    );
    acquiring.reject(failure);
    const error = await outcome;
    assert.equal(error.message, "ARCHIVE_STREAM_CLEANUP_FAILED");
    assert.equal(error.cause.name, "AbortError");
    assert.ok(!error.stack.includes("private"));
    await assert.rejects(error.cleanup, (value) => value === failure);
  });
  test("ordinary acquisition failure and late normal abort are not duplicate cleanup failures", async () => {
    const f = fixture("n");
    await assert.rejects(
      stageArchiveRecord(
        source(f.raw),
        f.record,
        options(() => {
          throw new Error("private primary");
        }),
      ),
      (error) => error.message === "ARCHIVE_STREAM_FAILED" && error.cleanup === undefined,
    );
    const { acquiring, outcome } = await cancelledAcquisition();
    acquiring.reject(new DOMException("Cancelled", "AbortError"));
    assert.equal((await outcome).name, "AbortError");
  });
  test("acquisition deadline preserves later rejection on cleanup", async () => {
    const { acquiring, outcome } = await cancelledAcquisition();
    const error = await outcome;
    assert.equal(error.message, "ARCHIVE_STREAM_CLEANUP_PENDING");
    assert.equal(error.cause.name, "AbortError");
    const failure = new AggregateError([new Error("late cleanup")], "late acquisition");
    acquiring.reject(failure);
    await assert.rejects(error.cleanup, (value) => value === failure);
  });
  test("acquisition deadline preserves late stage disposal failure", async () => {
    const { acquiring, outcome } = await cancelledAcquisition();
    const error = await outcome;
    assert.equal(error.message, "ARCHIVE_STREAM_CLEANUP_PENDING");
    let disposals = 0;
    const failure = new Error("late disposal");
    acquiring.resolve({
      dispose() {
        disposals++;
        throw failure;
      },
    });
    await assert.rejects(error.cleanup, (value) => value === failure);
    assert.equal(disposals, 1);
  });
  // 第二遍输入变化必须拒绝发布；压缩头变化即使解码内容相同也必须被发现。
  // Reject changed second-pass input before publication, including compressed header changes preserving decoded content.
  for (const encoding of ["n", "z"])
    test(`large ${encoding} second-pass mutation is detected before finalization`, async () => {
      const f = fixture("n", Buffer.alloc(1024 * 1024 + 16, 11));
      if (encoding === "z") {
        f.raw = deflateSync(f.content, { level: 0 });
        f.record.encoding = "z";
        f.record.encodedSize = f.raw.length;
        f.record.ekey = md5(f.raw).slice(0, 18) + f.record.ekey.slice(18);
      }
      const input = source(f.raw),
        original = input.readAt;
      let starts = 0;
      input.readAt = (at, count) => {
        if (at === BASE && ++starts === 3) {
          if (encoding === "z") {
            f.raw[0] = 0x78;
            f.raw[1] = 0x9c;
          } else f.raw[0] ^= 1;
        }
        return original(at, count);
      };
      const staging = stageFactory({ retain: false });
      await assert.rejects(
        stageArchiveRecord(input, f.record, options(staging.createStage)),
        /SOURCE_CHANGED/,
      );
      assert.equal(staging.state.finalized, 0);
      assert.equal(staging.state.disposed, 1);
    });

  // 只统计合成来源调用，不代表浏览器或磁盘耗时；压缩馈送上限保持 1 KiB。
  // Count synthetic source calls only, not browser or disk timing; compressed feed remains capped at 1 KiB.
  test("large compressed second pass reads serial 64 KiB windows with exact decoded bytes", async () => {
    const content = Buffer.alloc(2 * 1024 * 1024 + 17, 42);
    const f = fixture("z", content);
    f.raw = deflateSync(content, { level: 0 });
    f.record.encodedSize = f.raw.length;
    f.record.ekey = md5(f.raw).slice(0, 18) + f.record.ekey.slice(18);
    const input = source(f.raw),
      staging = stageFactory();
    const original = input.readAt;
    let active = 0;
    input.readAt = async (...args) => {
      assert.equal(active++, 0);
      await Promise.resolve();
      const result = original(...args);
      active--;
      return result;
    };
    const result = await stageArchiveRecord(input, f.record, options(staging.createStage));
    assert.deepEqual(Buffer.concat(staging.state.chunks), content);
    const windows = Math.ceil(f.raw.length / 65536);
    assert.equal(input.reads.length, 1 + 2 * windows);
    assert.equal(input.reads.length, 67);
    assert.ok(input.reads.every(([, count]) => count <= 65536));
    await result.dispose();
  });

  test("abort while draining a large compressed window never finalizes or reads after cancellation", async () => {
    const f = fixture("z", Buffer.alloc(2 * 1024 * 1024 + 17, 42));
    f.raw = deflateSync(f.content, { level: 0 });
    f.record.encodedSize = f.raw.length;
    f.record.ekey = md5(f.raw).slice(0, 18) + f.record.ekey.slice(18);
    const controller = new AbortController(),
      input = source(f.raw),
      staging = stageFactory();
    const original = input.readAt;
    input.readAt = (...args) => {
      assert.equal(controller.signal.aborted, false);
      return original(...args);
    };
    await assert.rejects(
      stageArchiveRecord(input, f.record, {
        ...options(async () => {
          const stage = await staging.createStage();
          return {
            ...stage,
            append(bytes) {
              stage.append(bytes);
              controller.abort();
            },
          };
        }),
        signal: controller.signal,
      }),
      { name: "AbortError" },
    );
    assert.ok(staging.state.appended > 0);
    assert.equal(staging.state.finalized, 0);
    assert.equal(staging.state.disposed, 1);
  });

  test("staged read disposal blocks queued dispatch and late completion without mutating borrowed bytes", async () => {
    for (const timing of ["before", "during"]) {
      const f = fixture("n", Buffer.from([9])),
        staging = stageFactory();
      const returned = new Uint8Array([9]);
      let reads = 0,
        start,
        finish;
      const started = new Promise((resolve) => {
        start = resolve;
      });
      const delayed = new Promise((resolve) => {
        finish = resolve;
      });
      const output = await stageArchiveRecord(
        source(f.raw),
        f.record,
        options(async () => {
          const stage = await staging.createStage();
          return {
            ...stage,
            async finalize() {
              const file = await stage.finalize();
              return {
                ...file,
                readAt() {
                  reads++;
                  start();
                  return delayed;
                },
              };
            },
          };
        }),
      );
      const pending = output.readAt(0, 1);
      const rejected = assert.rejects(pending, /ARCHIVE_STREAM_DISPOSED/);
      if (timing === "during") await started;
      await output.dispose();
      finish(returned);
      await rejected;
      assert.equal(reads, timing === "before" ? 0 : 1);
      assert.equal(staging.state.disposed, 1);
      assert.deepEqual(returned, new Uint8Array([9]));
    }
  });

  // 缓存副本按 64 KiB 合并，但解压器仍逐个接收至多 1 KiB。
  // Coalesce cached snapshots at 64 KiB while keeping decoder writes capped at 1 KiB.
  test("cached compressed snapshots are bounded windows without enlarging decoder feeds", async (t) => {
    const content = Buffer.alloc(1024 * 1024 - 1024, 42),
      f = fixture("z", content);
    f.raw = deflateSync(content, { level: 0 });
    f.record.encodedSize = f.raw.length;
    f.record.ekey = md5(f.raw).slice(0, 18) + f.record.ekey.slice(18);
    assert.ok(f.raw.length < 1024 * 1024 && f.raw.length > 65536);
    const snapshots = [],
      feeds = [],
      originalSlice = Uint8Array.prototype.slice,
      NativeDecompressionStream = globalThis.DecompressionStream;
    t.mock.method(Uint8Array.prototype, "slice", function (...args) {
      const result = originalSlice.apply(this, args);
      if (this.length === f.raw.length) snapshots.push(result.length);
      return result;
    });
    t.mock.method(globalThis, "DecompressionStream", function (format) {
      const stream = new NativeDecompressionStream(format);
      return {
        readable: stream.readable,
        writable: {
          getWriter() {
            const writer = stream.writable.getWriter();
            return {
              write(bytes) {
                feeds.push(bytes.length);
                assert.ok(bytes.buffer.byteLength <= 65536);
                return writer.write(bytes);
              },
              close: () => writer.close(),
              abort: (error) => writer.abort(error),
              releaseLock: () => writer.releaseLock(),
            };
          },
        },
      };
    });
    const input = source(f.raw),
      staging = stageFactory();
    const output = await stageArchiveRecord(input, f.record, options(staging.createStage));
    assert.deepEqual(Buffer.concat(staging.state.chunks), content);
    assert.equal(snapshots.length, Math.ceil(f.raw.length / 65536));
    assert.equal(snapshots.length, 16);
    assert.ok(snapshots.every((count) => count <= 65536));
    assert.equal(feeds.length, Math.ceil(f.raw.length / 1024));
    assert.equal(feeds.length, 1024);
    assert.ok(feeds.every((count) => count <= 1024));
    assert.equal(input.reads.length, 1 + Math.ceil(f.raw.length / 65536));
    await output.dispose();
  });

  // Buffer.slice 会保留来源别名；异步暂存期间仍须保持独立的读取快照。
  // Buffer.slice aliases its source; read snapshots must remain independent across asynchronous staging.
  for (const bufferSubclass of [false, true])
    test(`stream snapshots isolate reusable ${bufferSubclass ? "Buffer" : "Uint8Array"} source storage`, async () => {
      const f = fixture("n", Buffer.alloc(1024 * 1024 + 17, 42)),
        scratch = bufferSubclass ? Buffer.alloc(65536) : new Uint8Array(65536),
        staging = stageFactory();
      const input = {
        size: BASE + BigInt(f.raw.length),
        readAt(at, count) {
          const offset = Number(at - BASE);
          scratch.set(f.raw.subarray(offset, offset + count));
          return scratch.subarray(0, count);
        },
      };
      const output = await stageArchiveRecord(
        input,
        f.record,
        options(async () => {
          const stage = await staging.createStage();
          return {
            ...stage,
            async append(bytes) {
              await Promise.resolve();
              scratch.fill(0);
              stage.append(bytes);
            },
          };
        }),
      );
      assert.deepEqual(Buffer.concat(staging.state.chunks), f.content);
      await output.dispose();
    });
}

// archive-read-rpc
{
  // 自造消息端口验证来源绑定、精确偏移、有界并发及失败关闭，不使用私有归档。
  // Synthetic ports verify source binding, exact offsets, bounded concurrency and fail-closed cleanup without private archives.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createArchiveReadRpcHost, createArchiveReadRpcClient } = archiveReadRpcModule;
  function pair() {
    const make = () => {
      const listeners = new Map();
      return {
        listeners,
        sent: [],
        closed: false,
        start() {},
        close() {
          this.closed = true;
        },
        addEventListener(k, f) {
          if (!listeners.has(k)) listeners.set(k, new Set());
          listeners.get(k).add(f);
        },
        removeEventListener(k, f) {
          listeners.get(k)?.delete(f);
        },
        deliver(data) {
          for (const f of [...(listeners.get("message") ?? [])]) f({ data });
        },
        postMessage(data, transfers = []) {
          const copy = structuredClone(data, { transfer: transfers });
          this.sent.push(copy);
          queueMicrotask(() => this.peer.deliver(copy));
        },
      };
    };
    const a = make(),
      b = make();
    a.peer = b;
    b.peer = a;
    return [a, b];
  }
  function setup(source, options = {}) {
    const [a, b] = pair(),
      host = createArchiveReadRpcHost(a, options),
      client = createArchiveReadRpcClient(b, options);
    const descriptor = host.register(source),
      reader = client.open(descriptor);
    return { a, b, host, client, descriptor, reader };
  }
  const tick = () => new Promise((resolve) => setImmediate(resolve));
  const deferred = () => {
    let resolve;
    const promise = new Promise((r) => {
      resolve = r;
    });
    return { promise, resolve };
  };
  function detached(port) {
    assert.ok([...port.listeners.values()].every((set) => set.size === 0));
  }
  test("opaque source binding preserves BigInt offsets and transfers owned copies", async () => {
    const size = (1n << 60n) + 100n,
      backing = new Uint8Array(100000),
      calls = [];
    backing.fill(23);
    const s = setup({
      size,
      readAt(at, count) {
        calls.push([at, count]);
        return backing.subarray(7, 7 + count);
      },
    });
    assert.deepEqual(Object.keys(s.descriptor).sort(), ["id", "size"]);
    assert.equal(s.reader.size, size);
    const bytes = await s.reader.readAt(size - 8n, 8);
    assert.equal(bytes.buffer.byteLength, 8);
    assert.deepEqual([...bytes], Array(8).fill(23));
    assert.deepEqual(calls, [[size - 8n, 8]]);
    assert.equal(backing.byteLength, 100000);
    assert.equal(s.b.sent[0].offset, (size - 8n).toString());
    s.client.close();
    await tick();
    detached(s.a);
    detached(s.b);
  });
  test("Buffer subviews transfer only the requested bytes without detaching or mutating the source", async () => {
    const backing = Buffer.alloc(32, 99);
    backing.set([1, 2, 3, 4], 9);
    const before = new Uint8Array(backing);
    const s = setup({ size: 4n, readAt: () => backing.subarray(9, 13) });
    try {
      const bytes = await s.reader.readAt(0n, 4);
      assert.deepEqual([...bytes], [1, 2, 3, 4]);
      assert.equal(bytes.buffer.byteLength, 4);
      assert.equal(backing.byteLength, 32);
      assert.deepEqual(new Uint8Array(backing), before);
      bytes.fill(0);
      assert.deepEqual(new Uint8Array(backing), before);
      assert.deepEqual([...(await s.reader.readAt(0n, 4))], [1, 2, 3, 4]);
    } finally {
      s.client.close();
      s.host.close();
    }
  });
  test("bounds reject locally, concurrency has no queue, identifiers are monotonic", async () => {
    const wait = deferred();
    const s = setup({ size: 100n, readAt: () => wait.promise });
    await assert.rejects(s.reader.readAt(0, 1), /BOUNDS/);
    await assert.rejects(s.reader.readAt(0n, 65537), /BOUNDS/);
    const pending = s.reader.readAt(0n, 1);
    await assert.rejects(s.reader.readAt(0n, 1), /BUSY/);
    wait.resolve(new Uint8Array(1));
    await pending;
    await s.reader.readAt(0n, 1);
    assert.deepEqual(
      s.b.sent.filter((x) => x.type === "read").map((x) => x.id),
      [1, 2],
    );
    s.client.close();
  });
  test("source errors, short reads and shared buffers never disclose source details", async () => {
    for (const readAt of [
      () => {
        throw new Error("private/name");
      },
      () => new Uint8Array(0),
      () => new Uint8Array(new SharedArrayBuffer(1)),
    ]) {
      const s = setup({ size: 1n, readAt });
      await assert.rejects(
        s.reader.readAt(0n, 1),
        (error) => error.message === "ARCHIVE_RPC_READ" && !error.stack.includes("private"),
      );
      s.client.close();
    }
  });
  test("abort closes both endpoints, aborts source and suppresses late results", async () => {
    const wait = deferred(),
      entered = deferred();
    let sourceSignal;
    const s = setup({
      size: 1n,
      readAt(_at, _count, { signal }) {
        sourceSignal = signal;
        entered.resolve();
        return wait.promise;
      },
    });
    const controller = new AbortController();
    const pending = s.reader.readAt(0n, 1, { signal: controller.signal });
    await entered.promise;
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    await tick();
    assert.ok(sourceSignal.aborted);
    wait.resolve(new Uint8Array(1));
    await tick();
    assert.equal(s.a.sent.filter((x) => x.type === "result").length, 0);
    detached(s.a);
    detached(s.b);
  });
  test("revocation and timeout reject pending reads and release listeners", async () => {
    for (const mode of ["revoke", "timeout"]) {
      const s = setup({ size: 1n, readAt: () => new Promise(() => {}) }, { timeoutMs: 10 });
      const pending = s.reader.readAt(0n, 1);
      if (mode === "revoke") s.host.revoke(s.descriptor.id);
      await assert.rejects(pending, mode === "revoke" ? /CLOSED/ : /TIMEOUT/);
      await tick();
      detached(s.a);
      detached(s.b);
    }
  });
  test("host rejects forged capabilities, noncanonical offsets, duplicated IDs and oversized reads", async () => {
    for (const patch of [
      { cap: "forged" },
      { offset: "00" },
      { offset: "18446744073709551616" },
      { count: 65537 },
      { id: 2 },
      { path: "private" },
    ]) {
      const s = setup({ size: 100000n, readAt: () => new Uint8Array(1) });
      s.a.deliver({ type: "read", id: 1, cap: s.descriptor.id, offset: "0", count: 1, ...patch });
      await tick();
      assert.ok(s.a.closed);
      detached(s.a);
      detached(s.b);
    }
    const s = setup({ size: 1n, readAt: () => new Uint8Array(1) });
    await s.reader.readAt(0n, 1);
    s.a.deliver(s.b.sent[0]);
    await tick();
    assert.ok(s.a.closed);
  });
  test("client rejects malformed, duplicate and unsolicited replies", async () => {
    for (const kind of ["malformed", "unsolicited", "duplicate"]) {
      const wait = deferred();
      const s = setup({ size: 1n, readAt: () => wait.promise });
      if (kind === "duplicate") {
        wait.resolve(new Uint8Array(1));
        await s.reader.readAt(0n, 1);
        s.b.deliver({ type: "result", id: 1, bytes: new ArrayBuffer(1) });
      } else {
        const pending = s.reader.readAt(0n, 1);
        s.b.deliver({
          type: "result",
          id: kind === "unsolicited" ? 7 : 1,
          bytes: new ArrayBuffer(2),
        });
        await assert.rejects(pending, /PROTOCOL/);
      }
      await tick();
      assert.ok(s.b.closed);
      detached(s.a);
      detached(s.b);
    }
  });

  // 真实 MessageChannel 验证可转移缓冲与关闭流程；不代表浏览器工作线程验证。
  // A real MessageChannel verifies transfer and closure, not browser-worker integration.
  test("real MessageChannel round trip uses bounded buffers", async () => {
    const { port1, port2 } = new MessageChannel();
    const host = createArchiveReadRpcHost(port1),
      client = createArchiveReadRpcClient(port2);
    try {
      const reader = client.open(
        host.register({ size: 65536n, readAt: (_at, count) => new Uint8Array(count).fill(42) }),
      );
      const result = await reader.readAt(0n, 65536);
      assert.equal(result.byteLength, 65536);
      assert.equal(result[65535], 42);
    } finally {
      client.close();
      host.close();
    }
  });
  test("host enforces pending cap independently of client and global abort releases listeners", async () => {
    const s = setup({ size: 1n, readAt: () => new Promise(() => {}) });
    s.a.deliver({ type: "read", id: 1, cap: s.descriptor.id, offset: "0", count: 1 });
    s.a.deliver({ type: "read", id: 2, cap: s.descriptor.id, offset: "0", count: 1 });
    await tick();
    assert.ok(s.a.closed);
    const controller = new AbortController();
    const t = setup(
      { size: 1n, readAt: () => new Promise(() => {}) },
      { signal: controller.signal },
    );
    const pending = t.reader.readAt(0n, 1);
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    detached(t.a);
    detached(t.b);
  });
}

// zip-reader
{
  // 验证 zip-reader 的约定与边界；依据 ../zip-reader.mjs 及本文件的测试断言。
  // Verify zip-reader contracts and boundaries against ../zip-reader.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { execFileSync } = nodeChild_processModule;
  const { default: test } = nodeTestModule;
  const { createHash } = nodeCryptoModule;
  const { openShellZip, ZIP_LIMITS } = zipReaderModule;

  // 独立 Python zipfile 写入器，仅使用合成公开文本，不下载模板。
  // Independent Python zipfile writer: synthetic public text only, no downloaded shell.
  const fixtures = JSON.parse(
    execFileSync(
      "python3",
      [
        "-c",
        String.raw`
import io, json, zipfile, random, hashlib
class Forward(io.BytesIO):
    def seekable(self): return False
    def seek(self, *args): raise io.UnsupportedOperation()
def make(names, compression=0, forward=False, symlink=False, content=None):
    b = Forward() if forward else io.BytesIO()
    with zipfile.ZipFile(b, 'w', compression=compression) as z:
        for name in names:
            info = zipfile.ZipInfo(name)
            info.compress_type = compression
            info.external_attr = ((0o120777 if symlink else (0o40755 if name.endswith('/') else 0o100644)) << 16)
            z.writestr(info, b'' if name.endswith('/') else (content if content is not None else b'public synthetic shell\n' * 400))
    return b.getvalue().hex()
large = random.Random(7).randbytes(65536)
print(json.dumps({
 'large': make(['public.bin'], zipfile.ZIP_DEFLATED, content=large),
 'largeHash': hashlib.sha256(large).hexdigest(),
 'store': make(['Payload/', 'Payload/Demo.app/', 'Payload/Demo.app/Info.plist']),
 'deflate': make(['Payload/Demo.app/main'], zipfile.ZIP_DEFLATED),
 'descriptor': make(['Payload/Demo.app/main'], zipfile.ZIP_DEFLATED, True),
 'utf8': make(['Caf\u00e9/file.txt']),
 'traversal': make(['../escape']),
 'absolute': make(['/escape']),
 'backslash': make(['a\\b']),
 'duplicate': make(['a', 'a']),
 'case': make(['A/a', 'a/b']),
 'ancestor': make(['a/b', 'a']),
 'symlink': make(['a'], symlink=True),
 'empty': make([]),
}))
`,
      ],
      { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] },
    ),
  );
  const fixture = (key) => Uint8Array.from(Buffer.from(fixtures[key], "hex"));
  function source(bytes, hook) {
    const reads = [];
    return {
      size: bytes.length,
      reads,
      async read(offset, length) {
        reads.push([offset, length]);
        if (hook) await hook(offset, length);
        return bytes.slice(offset, offset + length);
      },
    };
  }
  const dv = (b) => new DataView(b.buffer, b.byteOffset, b.byteLength);
  function central(b) {
    return dv(b).getUint32(b.length - 6, true);
  }
  function put16(b, p, v) {
    dv(b).setUint16(p, v, true);
  }
  function put32(b, p, v) {
    dv(b).setUint32(p, v, true);
  }

  test("Python STORE inventory is detached, frozen and contract-shaped", async () => {
    const zip = await openShellZip(source(fixture("store")));
    assert.equal(zip.entries.length, 3);
    assert.ok(Object.isFrozen(zip.entries));
    assert.ok(Object.isFrozen(zip.entries[0]));
    assert.equal(zip.entries[0].mode, 0o40755);
    assert.equal(zip.entries[2].mode, 0o100644);
    assert.equal((await zip.readEntry("Payload/")).length, 0);
    assert.equal(
      new TextDecoder().decode(await zip.readEntry(zip.entries[2].path)),
      "public synthetic shell\n".repeat(400),
    );
    await assert.rejects(zip.readEntry("missing"), /Missing ZIP entry/);
  });
  for (const key of ["deflate", "descriptor", "utf8"]) {
    test(`Python ${key} round-trip with bounded compressed reads`, async () => {
      const input = source(fixture(key));
      const zip = await openShellZip(input);
      input.reads.length = 0;
      assert.equal(
        new TextDecoder().decode(await zip.readEntry(zip.entries[0].path)),
        "public synthetic shell\n".repeat(400),
      );
      if (key !== "utf8")
        assert.ok(input.reads.every(([, length]) => length <= ZIP_LIMITS.compressedSlice));
    });
  }
  for (const key of [
    "traversal",
    "absolute",
    "backslash",
    "duplicate",
    "case",
    "ancestor",
    "symlink",
    "empty",
  ]) {
    test(`rejects Python ${key} metadata`, async () => {
      await assert.rejects(openShellZip(source(fixture(key))));
    });
  }
  for (const [label, mutate] of [
    [
      "encryption",
      (b, c) => {
        put16(b, c + 8, 1);
      },
    ],
    [
      "unsupported compression",
      (b, c) => {
        put16(b, c + 10, 12);
      },
    ],
    [
      "ZIP64 sentinel",
      (b, c) => {
        put32(b, c + 24, 0xffffffff);
      },
    ],
    [
      "oversized decoded metadata",
      (b, c) => {
        put32(b, c + 24, ZIP_LIMITS.entryBytes + 1);
      },
    ],
    [
      "expansion ratio",
      (b, c) => {
        put32(b, c + 24, 1000000);
      },
    ],
    [
      "local name disagreement",
      (b) => {
        b[30] = 88;
      },
    ],
    [
      "local CRC disagreement",
      (b) => {
        put32(b, 14, 1);
      },
    ],
    [
      "overlap",
      (b, c) => {
        put32(b, c + 42, 1);
      },
    ],
    [
      "multi-disk",
      (b) => {
        put16(b, b.length - 18, 1);
      },
    ],
    [
      "wrong central bounds",
      (b) => {
        put32(b, b.length - 10, 1);
      },
    ],
    [
      "invalid UTF8",
      (b, c) => {
        put16(b, c + 8, 0x800);
        b[c + 46] = 0xff;
      },
    ],
  ]) {
    test(`rejects ${label} before decompression`, async () => {
      const b = fixture("deflate");
      mutate(b, central(b));
      await assert.rejects(openShellZip(source(b)));
    });
  }

  test("rejects payload corruption by CRC, never returns partial data", async () => {
    const b = fixture("utf8");
    const zip = await openShellZip(source(b));
    b[30 + dv(b).getUint16(26, true)] ^= 1;
    await assert.rejects(zip.readEntry(zip.entries[0].path), /CRC mismatch/);
  });

  test("validates data descriptor and accepts signatureless ZIP32 descriptor", async () => {
    const b = fixture("descriptor"),
      c = central(b),
      compressed = dv(b).getUint32(c + 20, true);
    const descriptor = 30 + dv(b).getUint16(26, true) + compressed;
    const bad = b.slice();
    put32(bad, descriptor + 4, 1);
    await assert.rejects(openShellZip(source(bad)), /descriptor/);
    const unsigned = new Uint8Array(b.length - 4);
    unsigned.set(b.subarray(0, descriptor));
    unsigned.set(b.subarray(descriptor + 4), descriptor);
    put32(unsigned, unsigned.length - 6, c - 4);
    const zip = await openShellZip(source(unsigned));
    assert.equal((await zip.readEntry(zip.entries[0].path)).length, 9200);
  });

  test("cancellation and malformed source errors do not expose raw source failures", async () => {
    const controller = new AbortController();
    controller.abort("private detail");
    await assert.rejects(
      openShellZip(source(fixture("store")), { signal: controller.signal }),
      /cancelled/,
    );
    await assert.rejects(
      openShellZip({
        size: 100,
        read() {
          throw new Error("private detail");
        },
      }),
      (e) => !e.message.includes("private detail"),
    );
    await assert.rejects(
      openShellZip({
        size: 100,
        async read() {
          return new Uint8Array(0);
        },
      }),
      /truncated/,
    );
    const zip = await openShellZip(source(fixture("deflate")));
    await assert.rejects(
      zip.readEntry(zip.entries[0].path, { signal: controller.signal }),
      /cancelled/,
    );
  });

  test("output overshoot and truncated deflate reject with no entry result", async () => {
    for (const wrongSize of [9199, 9201]) {
      const b = fixture("deflate"),
        c = central(b);
      put32(b, 22, wrongSize);
      put32(b, c + 24, wrongSize);
      const zip = await openShellZip(source(b));
      await assert.rejects(zip.readEntry(zip.entries[0].path));
    }
    const b = fixture("deflate");
    const zip = await openShellZip(source(b));
    b.fill(0xff, 30 + dv(b).getUint16(26, true), central(b));
    await assert.rejects(zip.readEntry(zip.entries[0].path));
  });

  test("one entry read at a time caps simultaneous output allocation", async () => {
    const zip = await openShellZip(source(fixture("deflate")));
    const first = zip.readEntry(zip.entries[0].path);
    await assert.rejects(zip.readEntry(zip.entries[0].path), /Concurrent/);
    await first;
  });

  test("large deflate is fetched in small slices, never one full encoded entry", async () => {
    const input = source(fixture("large"));
    const zip = await openShellZip(input);
    input.reads.length = 0;
    const output = await zip.readEntry("public.bin");
    assert.equal(createHash("sha256").update(output).digest("hex"), fixtures.largeHash);
    assert.ok(input.reads.length > 60);
    assert.ok(input.reads.every(([, n]) => n <= ZIP_LIMITS.compressedSlice));
  });

  test("mid-stream cancellation settles both directions and permits another read", async () => {
    const controller = new AbortController();
    let decoding = false,
      chunks = 0;
    const input = source(fixture("large"), () => {
      if (decoding && ++chunks === 3) controller.abort("private reason");
    });
    const zip = await openShellZip(input);
    decoding = true;
    await assert.rejects(
      zip.readEntry("public.bin", { signal: controller.signal }),
      (e) => !e.message.includes("private reason"),
    );
    decoding = false;
    assert.equal((await zip.readEntry("public.bin")).length, 65536);
  });

  test("trailing compressed junk is not silently accepted", async () => {
    const original = fixture("deflate"),
      c = central(original);
    const b = new Uint8Array(original.length + 1);
    b.set(original.subarray(0, c));
    b[c] = 0xff;
    b.set(original.subarray(c), c + 1);
    const size = dv(original).getUint32(c + 20, true) + 1;
    put32(b, 18, size);
    put32(b, c + 1 + 20, size);
    put32(b, b.length - 6, c + 1);
    const zip = await openShellZip(source(b));
    await assert.rejects(zip.readEntry(zip.entries[0].path));
  });

  test("trailing junk rejection does not depend on native DEFLATE strictness", async () => {
    const original = fixture("deflate"),
      c = central(original);
    const b = new Uint8Array(original.length + 1);
    b.set(original.subarray(0, c));
    b[c] = 0xff;
    b.set(original.subarray(c), c + 1);
    const size = dv(original).getUint32(c + 20, true) + 1;
    put32(b, 18, size);
    put32(b, c + 1 + 20, size);
    put32(b, b.length - 6, c + 1);
    const Native = globalThis.DecompressionStream;
    globalThis.DecompressionStream = class {
      constructor() {
        // 刻意宽松的运行时忽略全部编码字节，即使存在压缩垃圾，也返回匹配的解码字节/CRC。
        // Deliberately permissive runtime: ignores every encoded byte and would
        // return matching decoded bytes/CRC even when compressed junk is present.
        return new TransformStream({
          transform() {},
          flush(controller) {
            controller.enqueue(new TextEncoder().encode("public synthetic shell\n".repeat(400)));
          },
        });
      }
    };
    try {
      const zip = await openShellZip(source(b));
      await assert.rejects(zip.readEntry(zip.entries[0].path));
    } finally {
      globalThis.DecompressionStream = Native;
    }
  });
}

// zip-writer
{
  // 验证 zip-writer 的约定与边界；依据 ../zip-writer.mjs 及本文件的测试断言。
  // Verify zip-writer contracts and boundaries against ../zip-writer.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const { writeZip, ZIP_LIMITS } = zipWriterModule;

  function entry(path, text = "", mode) {
    const bytes = new TextEncoder().encode(text);
    return {
      path,
      size: bytes.length,
      mode,
      chunks: async function* () {
        for (let i = 0; i < bytes.length; i += 3) yield bytes.subarray(i, i + 3);
      },
    };
  }
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
  async function fails(entries, pattern, output = sink(), options) {
    await assert.rejects(writeZip(entries, output, options), pattern);
    assert.equal(output.closed, 0);
    assert.equal(output.aborted, 1);
    return output;
  }
  test("deterministic STORE ZIP is readable by Python with CRC, Unicode, modes and fixed timestamp", async () => {
    const files = [
      entry("Payload/Demo.app/é.txt", "héllo 🌍"),
      entry("Payload/Demo.app/Run", "123456789", 0o755),
      entry("empty"),
    ];
    const a = sink(),
      b = sink();
    const result = await writeZip(files, a);
    await writeZip([...files].reverse(), b);
    const archive = Buffer.concat(a.parts);
    assert.deepEqual(archive, Buffer.concat(b.parts));
    assert.equal(result.bytesWritten, archive.length);
    assert.equal(result.entries, 3);
    assert.equal(a.closed, 1);
    assert.equal(a.aborted, 0);
    const python = spawnSync(
      "python3",
      [
        "-c",
        `
import io, sys, zipfile
z = zipfile.ZipFile(io.BytesIO(sys.stdin.buffer.read()))
assert z.testzip() is None
assert z.namelist() == ['Payload/Demo.app/Run', 'Payload/Demo.app/é.txt', 'empty']
assert z.read('Payload/Demo.app/Run') == b'123456789'
assert z.read('Payload/Demo.app/é.txt').decode() == 'héllo 🌍'
assert z.read('empty') == b''
for f in z.infolist():
 assert f.date_time == (1980, 1, 1, 0, 0, 0)
 assert f.compress_type == zipfile.ZIP_STORED
 assert f.flag_bits == 0x808
 assert f.create_system == 3
 assert f.external_attr >> 16 == (0o100755 if f.filename.endswith('/Run') else 0o100644)
assert z.getinfo('Payload/Demo.app/Run').CRC == 0xcbf43926
`,
      ],
      { input: archive },
    );
    assert.equal(python.status, 0, python.stderr?.toString());
  });
  test("empty archive is valid", async () => {
    const out = sink();
    await writeZip([], out);
    assert.equal(Buffer.concat(out.parts).length, 22);
    assert.equal(out.closed, 1);
  });
  test("preflight rejects unsafe paths, duplicates, case collisions and ancestor conflicts before writes", async () => {
    for (const paths of [
      ["../a"],
      ["/a"],
      ["a\\b"],
      ["NUL.txt"],
      ["e\u0301"],
      ["a", "a"],
      ["a", "A"],
      ["a", "a/b"],
      ["A/b", "a"],
      ["Resources/a", "resources/b"],
      ["Resources/nested/a", "Resources/Nested/b"],
      ["\ud800"],
    ]) {
      const out = await fails(
        paths.map((p) => entry(p)),
        /path|conflict|encoding/i,
      );
      assert.equal(out.parts.length, 0);
    }
  });
  test("budgets and modes are validated without reading or writing content", async () => {
    const fake = (path, size) => ({
      path,
      size,
      chunks() {
        assert.fail("must not pull");
      },
    });
    const cases = [
      [fake("a", -1)],
      [fake("a", NaN)],
      [fake("a", Infinity)],
      [fake("a", 1.1)],
      [fake("a", ZIP_LIMITS.maxFileBytes + 1)],
      [fake("a", ZIP_LIMITS.maxFileBytes), fake("b", ZIP_LIMITS.maxFileBytes), fake("c", 1)],
      Array.from({ length: ZIP_LIMITS.maxEntries + 1 }, (_, i) => fake("f" + i, 0)),
      Array.from({ length: 1100 }, (_, i) => fake("x".repeat(4000) + i, 0)),
      [fake("x".repeat(4097), 0)],
      [fake("é".repeat(3000), 0)],
      [fake("x/".repeat(32) + "file", 0)],
      Array.from({ length: 16667 }, (_, i) => fake(`d${i}/nested/file`, 0)),
      [{ ...entry("a"), mode: 0o777 }],
      [{ path: "a", size: 0 }],
    ];
    for (const files of cases) {
      const out = await fails(files, /limit|mode|producer|length/i);
      assert.equal(out.parts.length, 0);
    }
  });
  test("short, excess, malformed and oversized input abort without closing", async () => {
    await fails([{ ...entry("a", "abc"), size: 4 }], /Short/);
    await fails([{ ...entry("a", "abc"), size: 2 }], /Excess/);
    for (const bytes of ["invalid", new Uint8Array(ZIP_LIMITS.maxChunkBytes + 1)]) {
      await fails(
        [
          {
            path: "a",
            size: 0,
            chunks: async function* () {
              yield bytes;
            },
          },
        ],
        /chunk/,
      );
    }
  });
  test("sink backpressure prevents parallel writes and pulling ahead", async () => {
    let busy = false,
      pulls = 0;
    const out = sink();
    out.write = async function (bytes) {
      assert.equal(busy, false);
      busy = true;
      const before = pulls;
      await new Promise((resolve) => setTimeout(resolve, 1));
      assert.equal(pulls, before);
      this.parts.push(bytes.slice());
      busy = false;
    };
    await writeZip(
      [
        {
          path: "a",
          size: 2,
          chunks: async function* () {
            for (let i = 0; i < 2; i++) {
              assert.equal(busy, false);
              pulls++;
              yield Uint8Array.of(i);
            }
          },
        },
      ],
      out,
    );
    assert.equal(pulls, 2);
    assert.equal(out.closed, 1);
  });
  test("cancellation before starting and during writes aborts without closing", async () => {
    const before = new AbortController();
    before.abort();
    const out = await fails([entry("a")], /abort/i, sink(), { signal: before.signal });
    assert.equal(out.parts.length, 0);
    const during = new AbortController();
    const output = sink();
    output.write = async () => {
      during.abort();
    };
    await fails([entry("a", "hello")], /abort/i, output, { signal: during.signal });
  });
  test("write and producer failures clean up iterator and preserve original error", async () => {
    let finalized = false,
      writes = 0;
    const out = sink();
    out.write = async () => {
      if (++writes === 2) throw new Error("write failure");
    };
    out.abort = async function () {
      this.aborted++;
      throw new Error("abort failure");
    };
    await fails(
      [
        {
          path: "a",
          size: 1,
          chunks: async function* () {
            try {
              yield Uint8Array.of(1);
            } finally {
              finalized = true;
            }
          },
        },
      ],
      /write failure/,
      out,
    );
    assert.equal(finalized, true);
    await fails(
      [
        {
          path: "a",
          size: 0,
          chunks: async function* () {
            yield* [];
            throw new Error("producer failure");
          },
        },
      ],
      /producer failure/,
    );
  });
  test("close failure aborts and does not resolve successfully", async () => {
    const out = sink();
    out.close = async () => {
      throw new Error("close failure");
    };
    await fails([], /close failure/, out);
  });

  test("failures at every record write abort before close", async () => {
    // 一个空文件：本地头、描述符、中央记录及 EOCD。
    // One empty file: local header, descriptor, central record, EOCD.
    for (let failureAt = 1; failureAt <= 4; failureAt++) {
      const out = sink();
      let writes = 0;
      out.write = async () => {
        if (++writes === failureAt) throw new Error("record failure");
      };
      await fails([entry("a")], /record failure/, out);
      assert.equal(writes, failureAt);
    }
  });
  test("cancellation during input and after final record prevents commit", async () => {
    const controller = new AbortController();
    let finalized = false;
    await fails(
      [
        {
          path: "a",
          size: 1,
          chunks: async function* () {
            try {
              controller.abort();
              yield Uint8Array.of(1);
            } finally {
              finalized = true;
            }
          },
        },
      ],
      /abort/i,
      sink(),
      { signal: controller.signal },
    );
    assert.equal(finalized, true);
    const atEnd = new AbortController(),
      out = sink();
    out.write = async () => {
      atEnd.abort();
    }; // 空 ZIP 只有最终记录。 | Empty ZIP has just its final record.
    await fails([], /abort/i, out, { signal: atEnd.signal });
  });
}

// zip64-writer
{
  // 验证 zip64-writer 的约定与边界；依据 ../zip64-writer.mjs 及本文件的测试断言。
  // Verify zip64-writer contracts and boundaries against ../zip64-writer.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { spawnSync } = nodeChild_processModule;
  const { planZip64, writeZip64, zip64EndRecords, ZIP64_LIMITS } = zip64WriterModule;
  const options = { maxTotalBytes: 1024n * 1024n };
  function entry(path, text = "", mode) {
    const data = new TextEncoder().encode(text);
    return {
      path,
      size: BigInt(data.length),
      mode,
      chunks: async function* () {
        for (let i = 0; i < data.length; i += 3) yield data.subarray(i, i + 3);
      },
    };
  }
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
      async abort(error) {
        this.aborted++;
        this.error = error;
      },
    };
  }
  async function fails(files, pattern, opts = options, out = sink()) {
    await assert.rejects(writeZip64(files, out, opts), pattern);
    assert.equal(out.closed, 0);
    assert.equal(out.aborted, 1);
    return out;
  }
  const view = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  test("real small ZIP64: deterministic bytes and independent Python CRC/content/modes", async () => {
    const files = [entry("é.txt", "héllo 🌍"), entry("Run", "123456789", 0o755), entry("empty")];
    const a = sink(),
      b = sink();
    const result = await writeZip64(files, a, options);
    await writeZip64([...files].reverse(), b, options);
    const archive = Buffer.concat(a.parts);
    assert.deepEqual(archive, Buffer.concat(b.parts));
    assert.equal(result.bytesWritten, BigInt(archive.length));
    assert.equal(planZip64(files, options).bytesWritten, result.bytesWritten);
    assert.equal(a.closed, 1);
    assert.equal(a.aborted, 0);
    const python = spawnSync(
      "python3",
      [
        "-c",
        `
import io, sys, zipfile
z = zipfile.ZipFile(io.BytesIO(sys.stdin.buffer.read()))
assert z.testzip() is None
assert z.namelist() == ['Run', 'empty', 'é.txt']
assert z.read('Run') == b'123456789'
assert z.read('é.txt').decode() == 'héllo 🌍'
assert z.read('empty') == b''
for f in z.infolist():
 assert f.date_time == (1980, 1, 1, 0, 0, 0)
 assert f.compress_type == zipfile.ZIP_STORED
 assert f.flag_bits == 0x808
 assert f.extract_version == 45
 assert f.create_system == 3
 assert f.external_attr >> 16 == (0o100755 if f.filename == 'Run' else 0o100644)
assert z.getinfo('Run').CRC == 0xcbf43926
`,
      ],
      { input: archive },
    );
    assert.equal(python.status, 0, python.stderr?.toString());
    const local = view(a.parts[0]);
    assert.equal(local.getUint32(18, true), 0xffffffff);
    assert.equal(local.getUint16(28, true), 20);
    assert.equal(local.getUint16(33, true), 1);
    const descriptor = a.parts.find(
      (p) => p.length === 24 && view(p).getUint32(0, true) === 0x08074b50,
    );
    assert.equal(view(descriptor).getBigUint64(8, true), 9n);
    assert.equal(view(descriptor).getBigUint64(16, true), 9n);
  });
  test("empty ZIP64 end records", async () => {
    const out = sink();
    await writeZip64([], out, { maxTotalBytes: 98n });
    assert.equal(out.parts.length, 1);
    assert.equal(out.parts[0].length, 98);
    const v = view(out.parts[0]);
    assert.equal(v.getBigUint64(4, true), 44n);
    assert.equal(v.getBigUint64(64, true), 0n);
    assert.equal(v.getUint32(72, true), 1);
    assert.equal(v.getUint32(76, true), 0x06054b50);
  });
  test("declared budgets and paths fail before writes or pulls", async () => {
    let pulls = 0;
    const file = {
      ...entry("a"),
      chunks() {
        pulls++;
        throw new Error("unexpected pull");
      },
    };
    const cases = [
      [[file], {}],
      [[file], { maxTotalBytes: Infinity }],
      [[file], { maxTotalBytes: Number.MAX_SAFE_INTEGER + 1 }],
      [[file], { maxTotalBytes: -1n }],
      [[file], { maxTotalBytes: 98n }],
      [[{ ...file, size: 1n << 64n }], options],
      [[{ ...file, size: -1n }], options],
      [[{ ...file, size: Number.MAX_SAFE_INTEGER + 1 }], options],
      [[{ ...file, mode: 0o777 }], options],
      [[{ ...file, chunks: null }], options],
      [[{ ...file, path: "\ud800" }], options],
      [[{ ...file, path: "x".repeat(4097) }], options],
      [[{ ...file, path: "é".repeat(2049) }], options],
      [[{ ...file, path: "x/".repeat(32) + "file" }], options],
      [
        Array.from({ length: 16667 }, (_, i) => ({ ...file, path: `d${i}/nested/file` })),
        { maxTotalBytes: 8 * 1024 * 1024 },
      ],
      [Array(ZIP64_LIMITS.maxEntries + 1).fill(file), options],
      [
        Array.from({ length: 1200 }, (_, i) => ({ ...file, path: `${i}${"x".repeat(4000)}` })),
        { maxTotalBytes: 1n << 40n },
      ],
    ];
    for (const paths of [
      ["../a"],
      ["/a"],
      ["a\\b"],
      ["a", "A"],
      ["a", "a/b"],
      ["a/b", "a"],
      ["a", "a"],
      ["Resources/a", "resources/b"],
      ["Resources/nested/a", "Resources/Nested/b"],
      ["e\u0301"],
    ])
      cases.push([paths.map((path) => ({ ...file, path })), options]);
    for (const [files, opts] of cases)
      assert.equal((await fails(files, /ZIP64|path|conflict/i, opts)).parts.length, 0);
    assert.equal(pulls, 0);
  });
  test("actual byte counts, chunk bounds and producer errors abort", async () => {
    await fails([{ ...entry("a", "abc"), size: 2n }], /Excess/);
    await fails([{ ...entry("a", "abc"), size: 4n }], /Short/);
    for (const bad of ["abc", new Uint8Array(ZIP64_LIMITS.maxChunkBytes + 1)])
      await fails(
        [
          {
            ...entry("a"),
            chunks: async function* () {
              yield bad;
            },
          },
        ],
        /chunk/,
      );
    await fails(
      [
        {
          ...entry("a"),
          chunks() {
            throw new Error("producer");
          },
        },
      ],
      /producer/,
    );
  });
  test("backpressure, cancellation and iterator cleanup", async () => {
    const controller = new AbortController();
    let pulled = 0,
      returned = 0,
      pending = false;
    const file = {
      path: "a",
      size: 3n,
      chunks: async function* () {
        try {
          for (let i = 0; i < 3; i++) {
            assert.equal(pending, false);
            pulled++;
            yield Uint8Array.of(i);
          }
        } finally {
          returned++;
        }
      },
    };
    const out = sink();
    out.write = async (bytes) => {
      assert.equal(pending, false);
      pending = true;
      await new Promise((resolve) => setImmediate(resolve));
      pending = false;
      if (bytes.length === 1) controller.abort(new Error("cancelled"));
    };
    await fails([file], /cancelled/, { ...options, signal: controller.signal }, out);
    assert.equal(pulled, 1);
    assert.equal(returned, 1);
  });
  test("pre-abort, write/close/cleanup failure and close commit boundary", async () => {
    const controller = new AbortController();
    controller.abort(new Error("pre-cancel"));
    assert.equal(
      (await fails([], /pre-cancel/, { ...options, signal: controller.signal })).parts.length,
      0,
    );
    for (const method of ["write", "close"]) {
      const output = sink();
      output[method] = async () => {
        throw new Error(method);
      };
      output.abort = async () => {
        output.aborted++;
        throw new Error("cleanup");
      };
      await fails([], new RegExp(method), options, output);
    }
    const late = new AbortController(),
      committed = sink();
    committed.close = async () => {
      committed.closed++;
      late.abort();
    };
    await writeZip64([], committed, { ...options, signal: late.signal });
    assert.equal(committed.closed, 1);
    assert.equal(committed.aborted, 0);
  });
  test("metadata-only virtual >4GiB and >2^53 counters: NOT a payload/CRC archive test", () => {
    let sourcePulls = 0;
    for (const size of [0xffffffffn, 0x100000000n, (1n << 53n) + 123n]) {
      const files = ["a", "b"].map((path) => ({
        path,
        size,
        chunks() {
          sourcePulls++;
          throw new Error("metadata only");
        },
      }));
      const plan = planZip64(files, { maxTotalBytes: 1n << 63n });
      assert.equal(plan.entries[1].offset, size + 75n);
      let virtualSinkCounter = 0n;
      for (const file of plan.entries) virtualSinkCounter += 51n + file.size + 24n;
      assert.equal(virtualSinkCounter, plan.centralOffset);
      const end = zip64EndRecords({ ...plan, entries: 2n });
      const v = view(end);
      assert.equal(v.getBigUint64(48, true), virtualSinkCounter);
      assert.equal(v.getBigUint64(64, true), plan.centralOffset + plan.centralSize);
      assert.equal(v.getBigUint64(24, true), 2n);
      assert.equal(v.getBigUint64(40, true), 150n);
      assert.equal(v.getUint16(84, true), 0xffff);
      assert.equal(v.getUint32(92, true), 0xffffffff);
      assert.equal(virtualSinkCounter + plan.centralSize + BigInt(end.length), plan.bytesWritten);
    }
    assert.equal(sourcePulls, 0);
    assert.throws(
      () => zip64EndRecords({ entries: 0, centralOffset: (1n << 64n) - 1n, centralSize: 0 }),
      /archive size/,
    );
  });
  test(
    "opt-in real >4GiB stream with bounded repeated source and discard/count sink",
    { skip: process.env.ZIP64_LARGE_STREAM_TEST !== "1" },
    async () => {
      // 所有字节经过生产 CRC 循环，不伪造块长度或 CRC。
      // All bytes pass through the production CRC loop. No fake chunk lengths/CRC.
      const chunk = new Uint8Array(ZIP64_LIMITS.maxChunkBytes),
        repeats = 1025;
      const size = BigInt(chunk.length) * BigInt(repeats);
      let pulls = 0,
        written = 0n,
        descriptor,
        central,
        secondCentral;
      const file = {
        path: "large",
        size,
        chunks: async function* () {
          for (let i = 0; i < repeats; i++) {
            pulls++;
            yield chunk;
          }
        },
      };
      const out = sink();
      out.write = async (bytes) => {
        written += BigInt(bytes.length);
        if (bytes.length === 24 && !descriptor) descriptor = bytes.slice();
        if (bytes.length === 79) central = bytes.slice();
        if (bytes.length === 75) secondCentral = bytes.slice();
      };
      const result = await writeZip64([file, entry("z")], out, { maxTotalBytes: size + 1024n });
      assert.equal(pulls, repeats);
      assert.equal(written, result.bytesWritten);
      assert.equal(view(descriptor).getBigUint64(8, true), size);
      assert.equal(view(central).getBigUint64(55, true), size);
      assert.equal(view(secondCentral).getBigUint64(67, true), size + 79n);
      const python = spawnSync("python3", [
        "-c",
        `import zlib\nc=0\nb=bytes(${chunk.length})\nfor _ in range(${repeats}): c=zlib.crc32(b,c)\nprint(c)`,
      ]);
      assert.equal(python.status, 0, python.stderr?.toString());
      assert.equal(view(descriptor).getUint32(4, true), Number(python.stdout.toString().trim()));
      assert.equal(out.closed, 1);
    },
  );
}

// staged-zip-validator
{
  // 验证 staged-zip-validator 的约定与边界；依据 ../zip-writer.mjs、../zip64-writer.mjs 及本文件的测试断言。
  // Verify staged-zip-validator contracts and boundaries against ../zip-writer.mjs、../zip64-writer.mjs and this file's assertions.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { mkdtemp, writeFile, rm } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { execFileSync } = nodeChild_processModule;
  const { writeZip } = zipWriterModule;
  const { writeZip64 } = zip64WriterModule;
  const { planStagedZip, validateStagedZip } = stagedZipValidatorModule;
  const manifest = [
    { path: "Payload/Test.app/zéro", size: 5, mode: 0o100755 },
    { path: "Payload/Test.app/a", size: 0, mode: 0o100644 },
  ];
  const payload = Uint8Array.of(0, 1, 127, 128, 255);
  const options = (archiveFormat) => ({ archiveFormat, maxOutputBytes: 1024 * 1024 });
  async function fixture(format, files = manifest) {
    const chunks = [];
    await (format === "zip64" ? writeZip64 : writeZip)(
      files.map((entry) => ({
        ...entry,
        mode: entry.mode & 0o777,
        async *chunks() {
          if (entry.size) yield payload;
        },
      })),
      {
        async write(bytes) {
          chunks.push(bytes.slice());
        },
        async close() {},
        async abort() {},
      },
      { maxTotalBytes: 1024 * 1024 },
    );
    return new Uint8Array(Buffer.concat(chunks));
  }
  const source = (bytes) => ({
    size: bytes.length,
    async read(offset, length) {
      return bytes.slice(offset, offset + length);
    },
  });
  for (const format of ["zip32", "zip64"]) {
    test(`${format}: exact plan, byte validation and independent Python ZIP oracle`, async () => {
      const bytes = await fixture(format);
      assert.equal(bytes.length, planStagedZip(manifest, options(format)).totalBytes);
      assert.equal((await validateStagedZip(source(bytes), manifest, options(format))).valid, true);
      const dir = await mkdtemp(join(tmpdir(), "staged-zip-oracle-"));
      try {
        const file = join(dir, "fixture.zip");
        await writeFile(file, bytes);
        const result = JSON.parse(
          execFileSync(
            "python3",
            [
              "-c",
              `import json,sys,zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
 assert z.testzip() is None
 print(json.dumps([{'name':i.filename,'size':i.file_size,'mode':i.external_attr >> 16,'bytes':list(z.read(i))} for i in z.infolist()]))`,
              file,
            ],
            { encoding: "utf8" },
          ),
        );
        assert.deepEqual(result, [
          { name: "Payload/Test.app/a", size: 0, mode: 0o100644, bytes: [] },
          { name: "Payload/Test.app/zéro", size: 5, mode: 0o100755, bytes: [...payload] },
        ]);
      } finally {
        await rm(dir, { recursive: true, force: true });
      }
    });
    test(`${format}: every single-byte corruption is rejected`, async () => {
      const bytes = await fixture(format);
      for (let i = 0; i < bytes.length; i++) {
        const changed = bytes.slice();
        changed[i] ^= 1;
        await assert.rejects(
          validateStagedZip(source(changed), manifest, options(format)),
          undefined,
          `byte ${i}`,
        );
      }
    });
    test(`${format}: truncation, trailing bytes, changed manifest, cancellation`, async () => {
      const bytes = await fixture(format);
      await assert.rejects(validateStagedZip(source(bytes.slice(1)), manifest, options(format)));
      await assert.rejects(
        validateStagedZip(source(Uint8Array.from([...bytes, 0])), manifest, options(format)),
      );
      await assert.rejects(
        validateStagedZip(
          source(bytes),
          manifest.map((entry) => ({ ...entry, mode: 0o644 })),
          options(format),
        ),
      );
      await assert.rejects(
        validateStagedZip(source(bytes), manifest, {
          ...options(format),
          signal: AbortSignal.abort(),
        }),
      );
      assert.equal(
        (await validateStagedZip(source(await fixture(format, [])), [], options(format))).entries,
        0,
      );
    });
    test(`${format}: staged plans bound implicit directory depth and node count`, () => {
      const entry = (path) => ({ path, size: 0, mode: 0o644 });
      const budget = { archiveFormat: format, maxOutputBytes: 8 * 1024 * 1024 };
      assert.throws(() => planStagedZip([entry("x/".repeat(32) + "file")], budget), /depth limit/);
      assert.throws(
        () =>
          planStagedZip(
            Array.from({ length: 16667 }, (_, i) => entry(`d${i}/nested/file`)),
            budget,
          ),
        /node limit/,
      );
      assert.equal(planStagedZip([entry("x/".repeat(31) + "file")], budget).entries.length, 1);
    });
  }
  test("ZIP64 >4GiB exact metadata without allocating large payloads", () => {
    const big = [{ path: "Payload/Test.app/large", mode: 0o644, size: 2 ** 32 + 1 }];
    const opts = { archiveFormat: "zip64", maxOutputBytes: 2 ** 33 };
    const plan = planStagedZip(big, opts);
    assert.ok(plan.centralOffset > 2 ** 32 && plan.totalBytes > 2 ** 32);
    assert.throws(() => planStagedZip(big, { ...opts, maxOutputBytes: plan.totalBytes - 1 }));
    assert.throws(() => planStagedZip(big, { ...opts, archiveFormat: "zip32" }));
    assert.throws(() =>
      planStagedZip([{ ...big[0], size: Number.MAX_SAFE_INTEGER }], {
        ...opts,
        maxOutputBytes: Number.MAX_SAFE_INTEGER,
      }),
    );
    for (const path of ["../bad", "x\ud800"])
      assert.throws(() => planStagedZip([{ ...big[0], path }], opts));
  });
}

// deflate-boundary
{
  // 验证 deflate-boundary 的约定与边界；依据 ../deflate-boundary.mjs 及本文件的测试断言。
  // Verify deflate-boundary contracts and boundaries against ../deflate-boundary.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { execFileSync } = nodeChild_processModule;
  const { default: test } = nodeTestModule;
  const { createDeflateBoundary } = deflateBoundaryModule;
  const fixtures = JSON.parse(
    execFileSync(
      "python3",
      [
        "-c",
        String.raw`
import json, random, zlib
rng = random.Random(19)
content = bytes(rng.choices(range(40), weights=range(1,41), k=65536))
result = []
for name, data, level, strategy in [('stored',content,0,zlib.Z_DEFAULT_STRATEGY), ('dynamic',content,6,zlib.Z_DEFAULT_STRATEGY), ('fixed', b'public synthetic bytes' * 1000, 6,zlib.Z_FIXED), ('empty',b'',6,zlib.Z_DEFAULT_STRATEGY)]:
 c = zlib.compressobj(level,zlib.DEFLATED,-15,8,strategy)
 encoded = c.compress(data) + c.flush()
 if name == 'dynamic': assert (encoded[0] >> 1) & 3 == 2
 assert zlib.decompress(encoded,-15) == data
 result.append({'name':name,'size':len(data),'encoded':encoded.hex()})
print(json.dumps(result))
`,
      ],
      { encoding: "utf8" },
    ),
  );
  for (const f of fixtures) {
    for (const chunkSize of [1, 7, 1024]) {
      test(`independent zlib ${f.name} with ${chunkSize}-byte boundaries`, () => {
        const scanner = createDeflateBoundary(f.size),
          encoded = Buffer.from(f.encoded, "hex");
        for (let p = 0; p < encoded.length; p += chunkSize)
          scanner.push(encoded.subarray(p, p + chunkSize));
        scanner.finish();
      });
    }
    test(`rejects ${f.name} trailing input independent of native decompressor`, () => {
      const scanner = createDeflateBoundary(f.size),
        encoded = Buffer.from(f.encoded, "hex");
      for (let p = 0; p < encoded.length; p += 1024) scanner.push(encoded.subarray(p, p + 1024));
      scanner.finish();
      assert.throws(() => scanner.push(Uint8Array.of(0)), /structure/);
    });
    test(`rejects ${f.name} truncation`, () => {
      const scanner = createDeflateBoundary(f.size),
        encoded = Buffer.from(f.encoded, "hex");
      for (let p = 0; p < encoded.length - 1; p += 1024)
        scanner.push(encoded.subarray(p, Math.min(p + 1024, encoded.length - 1)));
      assert.throws(() => scanner.finish(), /structure/);
    });
  }
  test("rejects reserved block and mismatched stored complement", () => {
    assert.throws(() => createDeflateBoundary(0).push(Uint8Array.of(7)), /structure/);
    assert.throws(
      () => createDeflateBoundary(1).push(Uint8Array.of(1, 1, 0, 0, 0, 65)),
      /structure/,
    );
  });

  test("public scanner bounds expected size before parsing or allocation", () => {
    for (const size of [
      -1,
      1.5,
      NaN,
      Infinity,
      undefined,
      "1",
      64 * 1024 * 1024 + 1,
      Number.MAX_SAFE_INTEGER,
    ]) {
      assert.throws(() => createDeflateBoundary(size), /expected size/);
    }
    assert.doesNotThrow(() => createDeflateBoundary(0));
    assert.doesNotThrow(() => createDeflateBoundary(64 * 1024 * 1024));
  });

  test("public scanner rejects wrong input type and oversized slice before copying", () => {
    const scanner = createDeflateBoundary(0);
    const oversized = new Uint8Array(1025);
    oversized.subarray = () => assert.fail("must reject before copying input");
    for (const bytes of [
      null,
      undefined,
      [3, 0],
      new ArrayBuffer(2),
      new Uint16Array(2),
      oversized,
    ]) {
      assert.throws(() => scanner.push(bytes), /input slice/);
    }
    scanner.push(Uint8Array.of(3, 0));
    scanner.finish();
  });
}

// zlib-stream
{
  // 合成 ABI 生命周期检查与本地固定原生 zlib 字节检查分开报告；不将模拟当作 WASM 证明。
  // Separate synthetic ABI lifecycle checks from pinned local native zlib byte checks; mocks are not WASM proof.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { existsSync } = nodeFsModule;
  const { fileURLToPath } = nodeUrlModule;
  const { spawnSync } = nodeChild_processModule;
  const { createZlibStream, ZLIB_STREAM_CHUNK_BYTES } = codecModule;
  const options = {
    maxInputBytes: 4 * ZLIB_STREAM_CHUNK_BYTES,
    maxOutputBytes: 5 * ZLIB_STREAM_CHUNK_BYTES,
  };
  const collect = async (stream) => {
    const chunks = [];
    for await (const chunk of stream) chunks.push(chunk);
    return Buffer.concat(chunks);
  };
  async function* source(...parts) {
    yield* parts;
  }
  function fixture() {
    let active = false;
    const calls = [];
    const stream = createZlibStream({
      begin() {
        if (active) throw new Error("busy");
        active = true;
        calls.push("begin");
      },
      push(bytes, finish) {
        calls.push([bytes.length, finish]);
        return finish ? new Uint8Array([9]) : bytes.slice();
      },
      abort() {
        active = false;
        calls.push("abort");
      },
    });
    return {
      stream,
      calls,
      get active() {
        return active;
      },
    };
  }

  test("mock ABI: lazy start, backpressure, empty chunks and one final finish", async () => {
    const f = fixture();
    let pulls = 0;
    const input = {
      async *[Symbol.asyncIterator]() {
        pulls++;
        yield new Uint8Array([1, 2]);
        pulls++;
        yield new Uint8Array(0);
        pulls++;
        yield new Uint8Array([3]);
      },
    };
    const stream = f.stream(input, options);
    assert.equal(f.active, false);
    assert.deepEqual((await stream.next()).value, new Uint8Array([1, 2]));
    assert.equal(pulls, 1);
    assert.equal(f.active, true);
    assert.deepEqual(await collect(stream), Buffer.from([3, 9]));
    assert.deepEqual(f.calls, ["begin", [2, false], [1, false], [0, true], "abort"]);
    assert.equal(f.active, false);
    assert.deepEqual(await collect(f.stream(source(), options)), Buffer.from([9]));
  });

  test("mock ABI: return, throw, upstream failure and budget errors release the instance", async () => {
    for (const method of ["return", "throw"]) {
      const f = fixture();
      let returned = false;
      const input = {
        async *[Symbol.asyncIterator]() {
          try {
            yield new Uint8Array([1]);
            yield new Uint8Array([2]);
          } finally {
            returned = true;
          }
        },
      };
      const stream = f.stream(input, options);
      await stream.next();
      if (method === "throw") await assert.rejects(stream.throw(new Error("consumer")), /consumer/);
      else await stream.return();
      assert.equal(returned, true);
      assert.equal(f.active, false);
      assert.equal(f.calls.at(-1), "abort");
      assert.deepEqual(await collect(f.stream(source(), options)), Buffer.from([9]));
    }
    for (const [input, limits, pattern] of [
      [source(new Uint8Array(ZLIB_STREAM_CHUNK_BYTES + 1)), options, /bounded/],
      [source(new Uint8Array(2)), { ...options, maxInputBytes: 1 }, /input limit/],
      [source(new Uint8Array(2)), { ...options, maxOutputBytes: 1 }, /output limit/],
      [source(), { ...options, maxOutputBytes: 0 }, /output limit/],
      [
        {
          [Symbol.asyncIterator]() {
            return {
              async next() {
                throw new Error("upstream");
              },
            };
          },
        },
        options,
        /upstream/,
      ],
    ]) {
      const f = fixture();
      await assert.rejects(collect(f.stream(input, limits)), pattern);
      assert.equal(f.active, false);
      assert.equal(f.calls.at(-1), "abort");
    }
  });

  test("mock ABI: cancellation while upstream never resolves releases without waiting", async () => {
    const f = fixture(),
      controller = new AbortController();
    let requested = false,
      closed = false;
    const input = {
      [Symbol.asyncIterator]() {
        return {
          next() {
            requested = true;
            return new Promise(() => {});
          },
          return() {
            closed = true;
            return new Promise(() => {});
          },
        };
      },
    };
    const pending = f.stream(input, { ...options, signal: controller.signal }).next();
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(requested, true);
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    await Promise.resolve();
    assert.equal(closed, true);
    assert.equal(f.active, false);
    assert.deepEqual(await collect(f.stream(source(), options)), Buffer.from([9]));
  });

  test("mock ABI: another stream cannot abort the current owner's state", async () => {
    const f = fixture(),
      first = f.stream(source(new Uint8Array([1])), options);
    await first.next();
    await assert.rejects(f.stream(source(), options).next(), /busy/);
    assert.equal(f.active, true);
    assert.equal(f.calls.filter((v) => v === "abort").length, 0);
    await first.return();
    assert.equal(f.active, false);
    for (const limits of [{}, { ...options, level: 6 }, { ...options, maxInputBytes: Infinity }])
      await assert.rejects(collect(f.stream(source(), limits)));
  });

  test("mock ABI: cancellation while yielded releases immediately without aborting a later stream", async () => {
    const f = fixture(),
      controller = new AbortController();
    const first = f.stream(source(new Uint8Array([1])), { ...options, signal: controller.signal });
    await first.next();
    controller.abort();
    assert.equal(f.active, false);
    const second = f.stream(source(new Uint8Array([2])), options);
    await second.next();
    assert.equal(f.active, true);
    await assert.rejects(first.next(), { name: "AbortError" });
    assert.equal(f.active, true);
    await second.return();
    assert.equal(f.active, false);
  });

  test("mock ABI: bounded output slices preserve backpressure and failure cleanup", async () => {
    let pulls = 0,
      released = 0;
    const stream = createZlibStream({
      begin() {},
      push(bytes, finish) {
        if (finish) return new Uint8Array(0);
        return new Uint8Array(ZLIB_STREAM_CHUNK_BYTES + 7).fill(bytes[0]);
      },
      abort() {
        released++;
      },
    });
    const input = {
      async *[Symbol.asyncIterator]() {
        pulls++;
        yield new Uint8Array([4]);
        pulls++;
        yield new Uint8Array([5]);
      },
    };
    const iterator = stream(input, options);
    const first = await iterator.next();
    assert.equal(first.value.length, ZLIB_STREAM_CHUNK_BYTES);
    assert.ok(first.value.buffer.byteLength <= ZLIB_STREAM_CHUNK_BYTES);
    const second = await iterator.next();
    assert.equal(second.value.length, 7);
    assert.ok(second.value.buffer.byteLength <= ZLIB_STREAM_CHUNK_BYTES);
    assert.equal(pulls, 1);
    assert.ok(second.value.every((value) => value === 4));
    await iterator.return();
    assert.equal(released, 1);
    const broken = createZlibStream({
      begin() {},
      push() {
        throw new Error("ABI rejected");
      },
      abort() {
        released++;
      },
    });
    await assert.rejects(collect(broken(source(new Uint8Array([1])), options)), /ABI rejected/);
    assert.equal(released, 2);
  });

  // 最终输出也必须拥有有界底层缓冲区，避免合法压缩输出被暂存器拒绝。
  // Final output must also own bounded backing buffers so staging accepts valid compressed bytes.
  test("mock ABI: final output chunks own bounded buffers even for Buffer results", async () => {
    for (const subclass of [false, true]) {
      const tail = subclass
        ? Buffer.alloc(ZLIB_STREAM_CHUNK_BYTES + 7, 9)
        : new Uint8Array(ZLIB_STREAM_CHUNK_BYTES + 7).fill(9);
      const stream = createZlibStream({
        begin() {},
        push(_bytes, finish) {
          assert.equal(finish, true);
          return tail;
        },
        abort() {},
      });
      const chunks = [];
      for await (const part of stream(source(), options)) {
        assert.ok(part.buffer.byteLength <= ZLIB_STREAM_CHUNK_BYTES);
        chunks.push(part);
      }
      assert.deepEqual(
        chunks.map((part) => part.length),
        [ZLIB_STREAM_CHUNK_BYTES, 7],
      );
      assert.deepEqual(Buffer.concat(chunks), Buffer.from(tail));
    }
  });

  test("mock ABI: paused abort closes upstream once even if consumer abandons the iterator", async () => {
    const f = fixture(),
      controller = new AbortController();
    let closes = 0;
    const input = {
      [Symbol.asyncIterator]() {
        return {
          async next() {
            return { value: new Uint8Array([1]), done: false };
          },
          async return() {
            closes++;
            return { done: true };
          },
        };
      },
    };
    const first = f.stream(input, { ...options, signal: controller.signal });
    await first.next();
    controller.abort();
    await Promise.resolve();
    assert.equal(closes, 1);
    assert.equal(f.active, false);
    const second = f.stream(source(new Uint8Array([2])), options);
    await second.next();
    await first.return();
    assert.equal(closes, 1);
    assert.equal(f.active, true);
    await second.return();
  });

  test("mock ABI: hung or rejecting upstream cleanup cannot hide primary failures", async () => {
    for (const cleanup of ["hung", "reject", "throw"]) {
      for (const failure of ["input", "output", "abi", "upstream"]) {
        let closes = 0,
          releases = 0;
        const stream = createZlibStream({
          begin() {},
          push() {
            if (failure === "abi") throw new Error("primary ABI");
            return new Uint8Array([1]);
          },
          abort() {
            releases++;
          },
        });
        const input = {
          [Symbol.asyncIterator]() {
            return {
              async next() {
                if (failure === "upstream") throw new Error("primary upstream");
                return { done: false, value: new Uint8Array([1]) };
              },
              return() {
                closes++;
                if (cleanup === "throw") throw new Error("cleanup");
                return cleanup === "hung"
                  ? new Promise(() => {})
                  : Promise.reject(new Error("cleanup"));
              },
            };
          },
        };
        const limits = {
          ...options,
          maxInputBytes: failure === "input" ? 0 : options.maxInputBytes,
          maxOutputBytes: failure === "output" ? 0 : options.maxOutputBytes,
        };
        let timer;
        const timeout = new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error("blocked on cleanup")), 1000);
        });
        try {
          await assert.rejects(
            Promise.race([stream(input, limits).next(), timeout]),
            failure === "input"
              ? /input limit/
              : failure === "output"
                ? /output limit/
                : failure === "abi"
                  ? /primary ABI/
                  : /primary upstream/,
          );
        } finally {
          clearTimeout(timer);
        }
        assert.equal(closes, 1);
        assert.equal(releases, 1);
      }
    }
  });

  test("mock ABI: normal consumer return still awaits cooperative upstream closure", async () => {
    const f = fixture();
    let completeClose,
      closes = 0;
    const input = {
      [Symbol.asyncIterator]() {
        return {
          async next() {
            return { done: false, value: new Uint8Array([1]) };
          },
          return() {
            closes++;
            return new Promise((resolve) => {
              completeClose = resolve;
            });
          },
        };
      },
    };
    const stream = f.stream(input, options);
    await stream.next();
    let settled = false;
    const closing = stream.return().then(() => {
      settled = true;
    });
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(f.active, false);
    assert.equal(closes, 1);
    assert.equal(settled, false);
    completeClose({ done: true });
    await closing;
    assert.equal(settled, true);
  });

  test("mock ABI: streaming rejects oversized backing buffers but accepts bounded subviews", async () => {
    const f = fixture();
    const oversized = new Uint8Array(new ArrayBuffer(ZLIB_STREAM_CHUNK_BYTES + 1), 0, 1);
    await assert.rejects(collect(f.stream(source(oversized), options)), /bounded zlib input chunk/);
    assert.deepEqual(f.calls, ["begin", "abort"]);
    const backing = new Uint8Array(ZLIB_STREAM_CHUNK_BYTES);
    backing.set([7, 8], 3);
    assert.deepEqual(
      await collect(f.stream(source(backing.subarray(3, 5)), options)),
      Buffer.from([7, 8, 9]),
    );
  });

  const native = fileURLToPath(
    new URL("../toolchain/png-codec-exact/dist/png_codec_native.so", import.meta.url),
  );
  test(
    "real local native ABI: fragmented input + separate empty finish preserves exact zlib bytes",
    { skip: !existsSync(native) },
    () => {
      const result = spawnSync(
        "python3",
        [
          "-c",
          String.raw`
import ctypes,sys,zlib,random
lib=ctypes.CDLL(sys.argv[1])
lib.png_input.restype=ctypes.c_void_p
lib.png_output.restype=ctypes.c_void_p
lib.generic_zlib_version.restype=ctypes.c_char_p
assert lib.generic_zlib_version()==b'1.2.13'
def encode(raw,step,separate):
    assert lib.zlib_begin(9)==0
    out=[]
    try:
        for off in range(0,len(raw),step):
            part=raw[off:off+step]
            ctypes.memmove(lib.png_input(),part,len(part))
            n=lib.zlib_push(len(part),int(not separate and off+step>=len(raw)))
            assert n>=0
            out.append(ctypes.string_at(lib.png_output(),n))
        if separate or not raw:
            n=lib.zlib_push(0,1)
            assert n>=0
            out.append(ctypes.string_at(lib.png_output(),n))
        return b''.join(out)
    finally: lib.zlib_abort()
rng=random.Random(1234)
for raw in [b'',b'a',bytes(range(256))*4097,rng.randbytes(2*1024*1024+17)]:
    expected=encode(raw,1024*1024,False)
    assert zlib.decompress(expected)==raw
    for step in [1 if len(raw)<2 else 8191,65536,1024*1024]:
        assert encode(raw,step,True)==expected
print('Pinned native zlib1.2.13: four deterministic inputs, three fragment sizes, exact byte equality')
`,
          native,
        ],
        { encoding: "utf8", timeout: 30000 },
      );
      assert.equal(result.status, 0, result.stderr);
      assert.match(result.stdout, /exact byte equality/);
    },
  );
}

// zlib-source-download
{
  // 合成网络响应验证公开源码下载；不访问网络或改动已有缓存。
  // Synthetic responses test public-source downloads without network access or cache mutation.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { mkdtemp, readFile, writeFile, readdir, rm } = promisesModule;
  const { tmpdir } = nodeOsModule;
  const { join } = nodePathModule;
  const { downloadPinnedZlib, readPinnedZlibSource } = sourceDownloadModule;
  const raw = new Uint8Array([1, 2, 3, 4]);
  const pin = {
    url: "https://example.invalid/pinned.tar.gz",
    bytes: raw.length,
    sha256: createHash("sha256").update(raw).digest("hex"),
  };

  function run(responses) {
    const calls = [],
      sleeps = [];
    return {
      calls,
      sleeps,
      promise: downloadPinnedZlib(pin, {
        fetch: async (url, options) => {
          calls.push({ url, options });
          const next = responses.shift();
          if (next instanceof Error) throw next;
          return next;
        },
        sleep: async (ms) => {
          sleeps.push(ms);
        },
      }),
    };
  }

  test("connection timeout and transient HTTP retry the same pinned URL", async () => {
    let cancelled = false;
    const busy = new Response(
      new ReadableStream({
        cancel() {
          cancelled = true;
        },
      }),
      { status: 503 },
    );
    const f = run([
      new TypeError("fetch failed", { cause: { code: "UND_ERR_CONNECT_TIMEOUT" } }),
      busy,
      new Response(raw),
    ]);
    assert.deepEqual(await f.promise, raw);
    assert.deepEqual(f.sleeps, [1000, 2000]);
    assert.equal(cancelled, true);
    assert.equal(f.calls.length, 3);
    for (const call of f.calls) {
      assert.equal(call.url, pin.url);
      assert.ok(call.options.signal instanceof AbortSignal);
    }
  });

  test("partial transport failure discards bytes before a fresh verified attempt", async () => {
    let reads = 0;
    const broken = new Response(
      new ReadableStream({
        pull(controller) {
          if (reads++ === 0) controller.enqueue(new Uint8Array([99]));
          else controller.error(new TypeError("terminated"));
        },
      }),
    );
    const f = run([broken, new Response(raw)]);
    assert.deepEqual(await f.promise, raw);
    assert.equal(f.calls.length, 2);
  });

  test("retries stop after three transient failures", async () => {
    const f = run(Array.from({ length: 3 }, () => new DOMException("timeout", "TimeoutError")));
    await assert.rejects(f.promise, { name: "TimeoutError" });
    assert.equal(f.calls.length, 3);
    assert.deepEqual(f.sleeps, [1000, 2000]);
  });

  test("HTTP denial and size or hash mismatch never retry", async () => {
    for (const response of [
      new Response(null, { status: 404 }),
      new Response(raw.slice(1)),
      new Response(new Uint8Array([1, 2, 3, 5])),
      new Response(new Uint8Array(5)),
    ]) {
      const f = run([response]);
      await assert.rejects(f.promise, /zlib source fetch 404|Pinned zlib source mismatch/);
      assert.equal(f.calls.length, 1);
      assert.deepEqual(f.sleeps, []);
    }
  });

  test("mismatch diagnostics identify received bytes without retaining the response body", async () => {
    for (const body of [new Uint8Array([60, 62]), new Uint8Array([1, 2, 3, 5])]) {
      const response = new Response(body, {
        headers: { "content-type": "text/html", "content-encoding": "identity" },
      });
      const f = run([response]);
      await assert.rejects(f.promise, (error) => {
        const details = JSON.parse(error.message.split("Pinned zlib source mismatch: ")[1]);
        assert.deepEqual(details, {
          url: pin.url,
          status: 200,
          contentType: "text/html",
          contentEncoding: "identity",
          expectedBytes: pin.bytes,
          receivedBytes: body.length,
          complete: true,
          expectedSha256: pin.sha256,
          receivedSha256: createHash("sha256").update(body).digest("hex"),
        });
        return true;
      });
      assert.equal(f.calls.length, 1);
      assert.deepEqual(f.sleeps, []);
      assert.equal(response.body.locked, false);
    }
  });

  test("mismatch response headers are bounded diagnostic hints, not integrity evidence", async () => {
    const f = run([
      new Response(raw.slice(1), {
        headers: { "content-type": "x".repeat(1024), "content-encoding": "y".repeat(1024) },
      }),
    ]);
    await assert.rejects(f.promise, (error) => {
      const details = JSON.parse(error.message.split("Pinned zlib source mismatch: ")[1]);
      assert.equal(details.contentType.length, 128);
      assert.equal(details.contentEncoding.length, 128);
      return true;
    });
    assert.equal(f.calls.length, 1);
  });

  test("oversized streamed response is cancelled without reading the remaining body", async () => {
    let cancelled = false;
    const response = new Response(
      new ReadableStream({
        start(controller) {
          controller.enqueue(new Uint8Array(5));
        },
        cancel() {
          cancelled = true;
        },
      }),
    );
    await assert.rejects(run([response]).promise, (error) => {
      const details = JSON.parse(error.message.split("Pinned zlib source mismatch: ")[1]);
      assert.equal(details.receivedBytes, 5);
      assert.equal(details.complete, false);
      assert.equal(details.receivedSha256, null);
      return true;
    });
    assert.equal(cancelled, true);
  });

  test("official release transport preserves both existing archive identities", async () => {
    for (const [name, version, bytes, sha256] of [
      [
        "zlib-source.json",
        "1.3.1",
        1512791,
        "9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23",
      ],
      [
        "zlib-legacy-source.json",
        "1.2.13",
        1497445,
        "b3a24de97a8fdbc835b9833169501030b8977031bcb54b3b3ac13740f846ab30",
      ],
    ]) {
      const actual = JSON.parse(
        await readFile(new URL(`../toolchain/png-codec-exact/${name}`, import.meta.url)),
      );
      assert.deepEqual(actual, {
        version,
        url: `https://github.com/madler/zlib/releases/download/v${version}/zlib-${version}.tar.gz`,
        bytes,
        sha256,
        root: `zlib-${version}`,
      });
    }
  });

  test("partial and wrong-digest source caches recover, then reuse verified bytes", async () => {
    const directory = await mkdtemp(join(tmpdir(), "zlib-cache-"));
    const path = join(directory, "source.tar.gz");
    let calls = 0;
    const options = {
      fetch: async () => {
        calls++;
        return new Response(raw);
      },
    };
    try {
      for (const bad of [raw.slice(1), new Uint8Array([1, 2, 3, 5])]) {
        await writeFile(path, bad);
        assert.deepEqual(await readPinnedZlibSource(path, pin, options), raw);
        assert.deepEqual(new Uint8Array(await readFile(path)), raw);
      }
      await readPinnedZlibSource(path, pin, options);
      assert.equal(calls, 2);
      await assert.rejects(readPinnedZlibSource(directory, pin, options), { code: "EISDIR" });
      assert.equal(calls, 2);
      assert.deepEqual(await readdir(directory), ["source.tar.gz"]);
      await writeFile(path, raw.slice(1));
      await assert.rejects(
        readPinnedZlibSource(path, pin, { fetch: async () => new Response(raw.slice(1)) }),
        /Pinned zlib source mismatch/,
      );
      assert.deepEqual(new Uint8Array(await readFile(path)), raw.slice(1));
      assert.deepEqual(await readdir(directory), ["source.tar.gz"]);
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });
}

// checksum-verifier
{
  // 验证 checksum-verifier 的约定与边界；依据 hash-wasm、../checksum-verifier.mjs 及本文件的测试断言。
  // Verify checksum-verifier contracts and boundaries against hash-wasm、../checksum-verifier.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { default: test } = nodeTestModule;
  const { readFileSync } = nodeFsModule;
  const { MD5_CHUNK_BYTES, verifyMd5 } = checksumVerifierModule;
  const { fileSource } = preflightModule;
  const { decodeArchiveRecord } = archiveReaderModule;

  const digest = (bytes) => createHash("md5").update(bytes).digest("hex");
  const encode = (text) => new TextEncoder().encode(text);

  // RFC 1321 第 A.5 节测试集，使用公开的合成算法测试向量。
  // RFC 1321 section A.5 test suite. Synthetic, public algorithm test vectors.
  const vectors = [
    ["", "d41d8cd98f00b204e9800998ecf8427e"],
    ["a", "0cc175b9c0f1b6a831c399e269772661"],
    ["abc", "900150983cd24fb0d6963f7d28e17f72"],
    ["message digest", "f96b697d7cb7938d525a2f31aaf161d0"],
    ["abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"],
    [
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
      "d174ab98d277d9f5a5611c2c9f419d9f",
    ],
    [
      "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
      "57edf4a22be3c955ac49da2e2107b67a",
    ],
  ];

  for (const [input, expected] of vectors) {
    test(`RFC 1321 vector: ${JSON.stringify(input)}`, async () => {
      assert.equal(await verifyMd5(encode(input), expected), true);
      assert.equal(await verifyMd5(encode(input), expected.toUpperCase()), true);
    });
  }

  test("exact nonzero-offset views match independent Node crypto across block/chunk boundaries", async () => {
    for (const length of [
      0,
      1,
      55,
      56,
      63,
      64,
      65,
      MD5_CHUNK_BYTES - 1,
      MD5_CHUNK_BYTES,
      MD5_CHUNK_BYTES + 1,
      MD5_CHUNK_BYTES * 3 + 63,
    ]) {
      for (const offset of [1, 7, 65]) {
        const backing = Uint8Array.from(
          { length: offset + length + 91 },
          (_, i) => (i * 37 + 19) % 256,
        );
        const view = backing.subarray(offset, offset + length);
        assert.equal(await verifyMd5(view, digest(view)), true, `${offset}/${length}`);
        assert.equal(
          await verifyMd5(view, digest(backing)),
          false,
          `${offset}/${length} excludes padding`,
        );
      }
    }
  });

  test("rejects malformed digests and non-Uint8Array input", async () => {
    for (const expected of [
      null,
      undefined,
      1,
      "",
      "0".repeat(31),
      "0".repeat(33),
      "g".repeat(32),
      `${"0".repeat(32)}\n`,
    ]) {
      await assert.rejects(verifyMd5(new Uint8Array(), expected), /32-character MD5/);
    }
    for (const bytes of [null, [], new ArrayBuffer(1), new DataView(new ArrayBuffer(1))]) {
      await assert.rejects(verifyMd5(bytes, vectors[0][1]), /Uint8Array/);
    }
  });

  test("digest mismatch returns exactly false", async () => {
    assert.equal(await verifyMd5(encode("abc"), vectors[0][1]), false);
  });

  test("already-aborted signals reject with their original reason, including empty input", async () => {
    const reason = new Error("cancel checksum");
    const signal = AbortSignal.abort(reason);
    await assert.rejects(
      verifyMd5(new Uint8Array(), vectors[0][1], { signal }),
      (error) => error === reason,
    );
  });

  test("task-level cancellation interrupts incremental hashing", async () => {
    const controller = new AbortController();
    const reason = new Error("stop between chunks");
    const bytes = new Uint8Array(MD5_CHUNK_BYTES * 4);
    const verification = verifyMd5(bytes, digest(bytes), { signal: controller.signal });
    setTimeout(() => controller.abort(reason), 0);
    await assert.rejects(verification, (error) => error === reason);
    assert.equal(await verifyMd5(encode("abc"), vectors[2][1]), true);
  });

  test("concurrent calls do not share hash state", async () => {
    const inputs = [encode("abc"), new Uint8Array(MD5_CHUNK_BYTES + 7).fill(17)];
    assert.deepEqual(await Promise.all(inputs.map((bytes) => verifyMd5(bytes, digest(bytes)))), [
      true,
      true,
    ]);
  });

  test("archive decoder accepts adapter and still rejects mismatching CKey", async () => {
    const bytes = encode("synthetic archive checksum fixture");
    const source = fileSource(new Blob([bytes]));
    const record = {
      offset: 0,
      encodedSize: bytes.length,
      decodedSize: bytes.length,
      encoding: "n",
      ckey: digest(bytes),
    };
    assert.deepEqual(await decodeArchiveRecord(source, record, { verifyMd5 }), bytes);
    await assert.rejects(
      decodeArchiveRecord(source, { ...record, ckey: vectors[0][1] }, { verifyMd5 }),
      /MD5 mismatch/,
    );
  });

  test("incremental calls never exceed the bounded chunk size or widen the input view", async () => {
    const chunks = [];
    class ObservedBytes extends Uint8Array {
      subarray(start, end) {
        const view = super.subarray(start, end);
        assert.ok(view.byteLength <= MD5_CHUNK_BYTES);
        assert.equal(
          view.byteOffset,
          this.byteOffset + chunks.reduce((sum, size) => sum + size, 0),
        );
        chunks.push(view.byteLength);
        return new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
      }
    }
    const backing = new Uint8Array(MD5_CHUNK_BYTES * 3 + 100).fill(29);
    const bytes = new ObservedBytes(backing.buffer, 13, backing.length - 32);
    assert.equal(await verifyMd5(bytes, digest(bytes)), true);
    assert.equal(
      chunks.reduce((sum, size) => sum + size, 0),
      bytes.byteLength,
    );
    assert.equal(chunks.length, Math.ceil(bytes.byteLength / MD5_CHUNK_BYTES));
  });

  test("hash-wasm release and MIT license are pinned", () => {
    const metadata = JSON.parse(
      readFileSync(new URL("../node_modules/hash-wasm/package.json", import.meta.url)),
    );
    const manifest = JSON.parse(readFileSync(new URL("../package.json", import.meta.url)));
    assert.equal(metadata.version, "4.12.0");
    assert.equal(manifest.dependencies["hash-wasm"], metadata.version);
    assert.equal(metadata.license, "MIT");
    assert.match(
      readFileSync(new URL("../node_modules/hash-wasm/LICENSE", import.meta.url), "utf8"),
      /Copyright \(c\) 2020 Dani Biró/,
    );
  });

  test("large incremental hashes preserve independent binary and hex states without network", async () => {
    const originalFetch = globalThis.fetch;
    globalThis.fetch = () => {
      throw new Error("Hashing must not fetch WASM");
    };
    try {
      const [first, second] = await Promise.all([createMD5(), createMD5()]);
      const oracle = createHash("md5");
      const chunk = new Uint8Array(MD5_CHUNK_BYTES + 13).fill(71);
      for (let index = 0; index < 2048; index++) {
        first.update(chunk);
        oracle.update(chunk);
        second.update(new Uint8Array([index % 256]));
      }
      assert.equal(first.digest(), oracle.digest("hex"));
      const small = Uint8Array.from({ length: 2048 }, (_, index) => index % 256);
      assert.deepEqual(
        second.digest("binary"),
        new Uint8Array(createHash("md5").update(small).digest()),
      );
      assert.equal(first.init().digest(), vectors[0][1]);
    } finally {
      globalThis.fetch = originalFetch;
    }
  });
}
