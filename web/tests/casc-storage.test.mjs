// Feature suite: casc-storage. Original test sections retain independent lexical scopes.
import * as strictModule from "node:assert/strict";
import * as nodeChild_processModule from "node:child_process";
import * as nodeUrlModule from "node:url";
import * as nodeTestModule from "node:test";
import * as cascLogicalReaderModule from "../casc-logical-reader.mjs";
import * as cascOverlayWriterModule from "../casc-overlay-writer.mjs";
import * as cascOverlayFixtureModule from "./helpers/casc-overlay-fixture.mjs";
import * as nodeCryptoModule from "node:crypto";
import * as nodeZlibModule from "node:zlib";
import * as overlayModule from "../toolchain/casc-overlay-js/overlay.mjs";
import * as cascPackWriterModule from "../casc-pack-writer.mjs";
import * as archiveReaderModule from "../archive-reader.mjs";
import * as rawFileStageModule from "../raw-file-stage.mjs";
import { createMD5 } from "hash-wasm";
import * as contractModule from "../toolchain/asset-runtime-js/contract.mjs";

// casc-logical-reader
{
  // 验证 casc-logical-reader 的约定与边界；依据 ../casc-logical-reader.mjs 及本文件的测试断言。
  // Verify casc-logical-reader contracts and boundaries against ../casc-logical-reader.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { execFileSync } = nodeChild_processModule;
  const { fileURLToPath } = nodeUrlModule;
  const { default: test } = nodeTestModule;
  const { createCascLogicalAssets, CASC_LIMITS } = cascLogicalReaderModule;

  // 以真实 dmx.py 为查找/解码参考，样例为合成内容，不含游戏字节；稀疏归档测试超过 4 GiB 的原生 40 位定位值。
  // Actual dmx.py is the lookup/decode oracle. Fixtures are synthetic and contain no
  // game bytes. A sparse archive exercises the native 40-bit locator above 4 GiB.
  const fixture = JSON.parse(
    execFileSync(
      "python3",
      [
        "-c",
        String.raw`
import hashlib, importlib.util, json, pathlib, struct, sys, tempfile, zlib
spec=importlib.util.spec_from_file_location('dmx',pathlib.Path(sys.argv[1])/'dm/tools/dmx.py')
dmx=importlib.util.module_from_spec(spec); spec.loader.exec_module(dmx)
content=bytes(range(256))*300+b'logical asset fixture'
base=(1<<35)+7
rows=[]; records=[]; pos=base
for i, encoding in enumerate(['n','z','b:synthetic']):
    if encoding=='n': raw=content
    elif encoding=='z': raw=zlib.compress(content)
    else:
        block=b'Z'+zlib.compress(content)
        raw=b'BLTE'+struct.pack('>I',36)+b'\x0f\x00\x00\x01'+struct.pack('>II',len(block),len(content))+hashlib.md5(block).digest()+block
    ekey=b'\0'*9+bytes([2,3])+pos.to_bytes(5,'big')
    rows.append('|'.join(['0','0','2','3',hashlib.md5(content).hexdigest(),str(len(content)),ekey.hex(),str(len(raw)),encoding,'data:data/Test/File'+str(i),'0']))
    records.append({'offset':str(pos),'hex':raw.hex()}); pos+=len(raw)+31
# Last duplicate wins, including case-only duplicate; native first match is not used.
rows.insert(0,rows[0].replace(hashlib.md5(content).hexdigest(),'f'*32))
manifest='FileId|KeyLayout|Chunk|Archive|CKey|CSize|EKey|ESize|Encoding|Path|Flags\n# comment\nshort|row\n'+'\n'.join(rows)+'\n'
with tempfile.TemporaryDirectory() as root:
    dmx.ROOT=root
    pathlib.Path(root,'build_manifest.psv').write_text(manifest)
    with open(pathlib.Path(root,'02-00000003.data'),'wb') as f:
        for r in records: f.seek(int(r['offset'])); f.write(bytes.fromhex(r['hex']))
    queries=['TEST\\FILE0','data/test/file1','DATA:DATA/TEST/FILE2']
    answers=[dmx.get(q).hex() for q in queries]
print(json.dumps({'manifest':manifest,'records':records,'queries':queries,'answers':answers}))
`,
        fileURLToPath(new URL("../../", import.meta.url)),
      ],
      { maxBuffer: 2 * 1024 * 1024 },
    ).toString(),
  );

  function romfs({ manifest = fixture.manifest, mutate, onRead } = {}) {
    const text = new TextEncoder().encode(manifest);
    const reads = [];
    const records = fixture.records.map((r) => ({
      offset: BigInt(r.offset),
      bytes: Uint8Array.from(Buffer.from(r.hex, "hex")),
    }));
    return {
      reads,
      getFile(path) {
        if (path === "data/build_manifest.psv")
          return {
            size: BigInt(text.length),
            readAt(offset, count) {
              reads.push([path, offset, count]);
              return text.slice(Number(offset), Number(offset) + count);
            },
          };
        if (path !== "data/02-00000003.data") return undefined;
        return {
          size: 1n << 60n,
          readAt(offset, count, { signal } = {}) {
            signal?.throwIfAborted();
            reads.push([path, offset, count]);
            assert.equal(typeof offset, "bigint");
            assert.ok(count <= 65536);
            const record = records.find(
              (r) =>
                offset >= r.offset && offset + BigInt(count) <= r.offset + BigInt(r.bytes.length),
            );
            assert.ok(record, "must read only requested record bytes");
            const bytes = record.bytes.slice(
              Number(offset - record.offset),
              Number(offset - record.offset) + count,
            );
            mutate?.(bytes);
            onRead?.();
            return bytes;
          },
        };
      },
    };
  }

  test("logical reads match original dmx lookup, duplicate resolution and all encodings", async () => {
    const source = romfs();
    const assets = await createCascLogicalAssets(source);
    assert.equal(assets.count, 3);
    assert.ok(
      source.reads.every(([path]) => path.endsWith("build_manifest.psv")),
      "archive remains lazy",
    );
    for (const [i, query] of fixture.queries.entries()) {
      const result = await assets.read(query, { maxBytes: 100000 });
      assert.equal(Buffer.from(result).toString("hex"), fixture.answers[i]);
      result.fill(0); // 调用方修改不能影响后续读取或其他资源。 | Consumer mutation must not affect future reads or other assets.
    }
    assert.equal(
      Buffer.from(await assets.read(fixture.queries[0])).toString("hex"),
      fixture.answers[0],
    );
    assert.equal([...assets.entries()].length, 3);
    assert.ok(Object.isFrozen(assets.lookup(fixture.queries[0])));
  });

  test("unnamed manifest records do not prevent indexed logical reads", async () => {
    const unnamed = ["0", "0", "0", "0", "0", "0", "0", "0", "n", "", "0"].join("|");
    const manifest = fixture.manifest.replace("# comment\n", `# comment\n${unnamed}\n`);
    const assets = await createCascLogicalAssets(romfs({ manifest }));
    assert.equal(assets.count, 3);
    assert.equal(
      Buffer.from(await assets.read(fixture.queries[0])).toString("hex"),
      fixture.answers[0],
    );
  });

  test("budgets and unsupported layout reject before reading archive bytes", async () => {
    const source = romfs();
    const assets = await createCascLogicalAssets(source);
    await assert.rejects(assets.read("test/file0", { maxBytes: 1 }), /byte budget/);
    await assert.rejects(assets.read("test/file0", { maxBytes: -1 }), /maxBytes/);
    assert.ok(source.reads.every(([p]) => p.endsWith("build_manifest.psv")));
    const other = await createCascLogicalAssets(
      romfs({ manifest: fixture.manifest.replaceAll("0|0|2|3|", "0|1|2|3|") }),
    );
    await assert.rejects(other.read("test/file0"), /key layout/);
    const mismatch = await createCascLogicalAssets(
      romfs({ manifest: fixture.manifest.replaceAll("0|0|2|3|", "0|0|1|3|") }),
    );
    await assert.rejects(mismatch.read("test/file0"), /chunk\/archive mismatch/);
  });

  test("integrity errors never publish bytes", async () => {
    const assets = await createCascLogicalAssets(
      romfs({
        mutate: (bytes) => {
          if (bytes.length) bytes[0] ^= 1;
        },
      }),
    );
    await assert.rejects(assets.read("test/file0"), /MD5 mismatch/);
    const short = romfs();
    const get = short.getFile;
    short.getFile = (path) =>
      path.endsWith(".data")
        ? {
            size: 10n,
            readAt() {
              assert.fail("out-of-bounds read");
            },
          }
        : get(path);
    await assert.rejects((await createCascLogicalAssets(short)).read("test/file0"), /extent/);
  });

  test("abort before and during lazy reads, including lifetime cancellation", async () => {
    const controller = new AbortController();
    const source = romfs({ onRead: () => controller.abort(new Error("cancelled read")) });
    const assets = await createCascLogicalAssets(source);
    await assert.rejects(
      assets.read("test/file0", { signal: controller.signal }),
      /cancelled read/,
    );
    const lifetime = new AbortController();
    const indexed = await createCascLogicalAssets(romfs(), { signal: lifetime.signal });
    lifetime.abort(new Error("expired mount"));
    await assert.rejects(indexed.read("test/file0"), /expired mount/);
    await assert.rejects(
      createCascLogicalAssets(romfs(), { signal: lifetime.signal }),
      /expired mount/,
    );
  });

  test("unsafe paths, absent assets and oversized manifest fail closed", async () => {
    const assets = await createCascLogicalAssets(romfs());
    for (const path of [
      "../test/file0",
      "/test/file0",
      "data:data/../test/file0",
      "data:data:test/file0",
      "test//file0",
      "test\0/file0",
    ])
      await assert.rejects(assets.read(path), /path/);
    await assert.rejects(assets.read("missing"), /not found/);
    await assert.rejects(
      createCascLogicalAssets({
        getFile: () => ({
          size: BigInt(CASC_LIMITS.manifestBytes) + 1n,
          readAt() {
            assert.fail("oversized manifest read");
          },
        }),
      }),
      /manifest byte budget/,
    );
    await assert.rejects(
      createCascLogicalAssets(
        romfs({ manifest: fixture.manifest.replace("data:data/Test/File0", "data:data/../File0") }),
      ),
      /unsafe logical path/,
    );
    await assert.rejects(
      createCascLogicalAssets(romfs({ manifest: "x".repeat(CASC_LIMITS.lineBytes + 1) })),
      /line budget/,
    );
  });
}

// casc-overlay-writer
{
  // 验证 casc-overlay-writer 的约定与边界；依据 ../casc-overlay-writer.mjs、./helpers/casc-overlay-fixture.mjs 及本文件的测试断言。
  // Verify casc-overlay-writer contracts and boundaries against ../casc-overlay-writer.mjs、./helpers/casc-overlay-fixture.mjs and this file's assertions.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { prepareCascOverlayManifest, writeCascOverlay, OVERLAY_LIMITS } = cascOverlayWriterModule;
  const { fixture, source, worker, jobs } = cascOverlayFixtureModule;
  test("real config, TVFS and n/z/multiblock BLTE archive equal independent original-native overlay", async () => {
    const romfs = source();
    const result = await writeCascOverlay({ romfs, replacements: jobs(), worker: worker() });
    assert.deepEqual(result.inventory, fixture.inventory);
    for (const [path, b64] of Object.entries(fixture.expected))
      assert.deepEqual(Buffer.from(result.files[path]), Buffer.from(b64, "base64"));
    const archiveReads = romfs.reads.filter((r) => r.path.endsWith(".data"));
    assert.ok(archiveReads.length > 0);
    // 旧资源数据位于两条 TVFS 记录之前，绝不能读取。
    // Old asset data comes before both TVFS records and must never be fetched.
    assert.ok(archiveReads.every((r) => r.start > 0n));
    assert.ok(romfs.reads.every((r) => r.count <= 64 * 1024));
  });
  test("empty overlay returns no changed files without any source reads", async () => {
    const romfs = source();
    assert.deepEqual(await writeCascOverlay({ romfs, replacements: [], worker: worker() }), {
      files: {},
      inventory: [],
    });
    assert.equal(romfs.reads.length, 0);
  });
  test("one authenticated manifest selection serves both overlay variants", async () => {
    const romfs = source();
    const replacements = jobs();
    const manifestSelection = await prepareCascOverlayManifest({
      romfs,
      paths: replacements.map((job) => job.path),
    });
    const manifestReads = () =>
      romfs.reads.filter((read) => read.path === "data/build_manifest.psv").length;
    const scanned = manifestReads();
    assert.ok(scanned > 0);
    for (let index = 0; index < 2; index++) {
      const result = await writeCascOverlay({
        romfs,
        replacements,
        worker: worker(),
        manifestSelection,
      });
      assert.deepEqual(result.inventory, fixture.inventory);
      assert.equal(manifestReads(), scanned);
    }
    await assert.rejects(
      writeCascOverlay({ romfs: source(), replacements, worker: worker(), manifestSelection }),
    );
    await assert.rejects(
      writeCascOverlay({ romfs, replacements, worker: worker(), manifestSelection: {} }),
    );
  });
  test("first manifest duplicate wins, preserving native overlay lookup", async () => {
    const manifest = Buffer.from(fixture.files["data/build_manifest.psv"], "base64").toString();
    const duplicate = manifest
      .split("\n")[1]
      .replace(/\|[0-9a-f]{32}\|/g, "|" + "f".repeat(32) + "|");
    const romfs = source({
      "data/build_manifest.psv": Buffer.from(manifest + duplicate + "\n").toString("base64"),
    });
    const out = await writeCascOverlay({ romfs, replacements: jobs(), worker: worker() });
    assert.deepEqual(out.inventory, fixture.inventory);
  });
  test("rejects short reads and corrupt authenticated-record checksums before worker", async () => {
    const romfs = source();
    romfs.files[0].readAt = async () => new Uint8Array(0);
    await assert.rejects(writeCascOverlay({ romfs, replacements: jobs(), worker: worker() }));
    const config = Buffer.from(fixture.files["data/.build.config"], "base64")
      .toString()
      .replace(/vfs-1 = ./, "vfs-1 = f");
    await assert.rejects(
      writeCascOverlay({
        romfs: source({ "data/.build.config": Buffer.from(config).toString("base64") }),
        replacements: jobs(),
        worker: worker(),
      }),
    );
  });
  test("bounds, duplicate normalized paths and absent source are rejected", async () => {
    for (const replacements of [
      [{ path: "../bad", bytes: new Uint8Array() }],
      [{ path: "data/n.bin", bytes: new Uint8Array(OVERLAY_LIMITS.replacement + 1) }],
      [...jobs(), jobs()[0]],
      [{ path: "data/missing", bytes: new Uint8Array() }],
    ]) {
      await assert.rejects(writeCascOverlay({ romfs: source(), replacements, worker: worker() }));
    }
  });
  test("abort stops serializer worker and does not release partial output", async () => {
    const controller = new AbortController();
    let closed = 0;
    const pending = {
      close() {
        closed++;
      },
      async run() {
        controller.abort();
        return { files: {} };
      },
    };
    await assert.rejects(
      writeCascOverlay({
        romfs: source(),
        replacements: jobs(),
        worker: pending,
        signal: controller.signal,
      }),
    );
    assert.equal(closed, 1);
  });
}

// casc-pack-metadata
{
  // 依据原生 25 字节 CFT 布局验证合成补充包元数据，不读取游戏或外部包。
  // Verify synthetic supplemental-pack metadata against the native 25-byte CFT layout; no game or external pack reads.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { createHash } = nodeCryptoModule;
  const { deflateSync } = nodeZlibModule;
  const {
    createCascOverlayTransform,
    inspectCascPackStorageSpecs,
    createCascPackStorageLookup,
    patchCascPrebuiltReferences,
    CASC_PACK_METADATA_LIMITS,
  } = overlayModule;
  const key = (i) => createHash("md5").update(`synthetic-${i}`).digest("hex");
  function fixture(count = 3) {
    const specs = new TextEncoder().encode("n\0z\0b:256K*=z\0");
    const bytes = new Uint8Array(46 + count * 25 + specs.length),
      view = new DataView(bytes.buffer);
    bytes.set([84, 86, 70, 83, 1, 46, 16, 0]);
    view.setUint32(8, 10);
    view.setUint32(12, 46);
    view.setUint32(20, 46);
    view.setUint32(28, 46);
    view.setUint32(32, count * 25);
    view.setUint32(38, 46 + count * 25);
    view.setUint32(42, specs.length);
    bytes.set(specs, 46 + count * 25);
    const entries = Array.from({ length: count }, (_, i) => ({
      path: `data/textures/${i}.texture`,
      oldEkey: key(i),
    }));
    entries.forEach((entry, i) => {
      const pos = 46 + i * 25;
      bytes.set(Buffer.from(entry.oldEkey, "hex"), pos);
      view.setUint32(pos + 16, 10 + i);
      bytes[pos + 20] = i % 3;
      view.setUint32(pos + 21, 100 + i);
    });
    return { bytes, entries };
  }

  test("inspect preserves original n/z/BLTE specs, lengths, order and immutable metadata", () => {
    const { bytes, entries } = fixture(),
      before = bytes.slice();
    const actual = inspectCascPackStorageSpecs(bytes, [entries[2], entries[0], entries[1]]);
    assert.deepEqual(
      actual.map((v) => [v.spec, v.specIndex, v.encodedSize, v.decodedSize]),
      [
        ["b:256K*=z", 2, 12, 102],
        ["n", 0, 10, 100],
        ["z", 1, 11, 101],
      ],
    );
    assert.ok(Object.isFrozen(actual));
    assert.ok(actual.every(Object.isFrozen));
    assert.deepEqual(bytes, before);
  });

  test("patch preserves all bytes except selected keys and lengths, including original spec bytes", () => {
    const { bytes, entries } = fixture(),
      before = bytes.slice();
    const records = [
      { ...entries[0], newEkey: entries[1].oldEkey, encodedSize: 0xffffffff, decodedSize: 0 },
      { ...entries[1], newEkey: entries[0].oldEkey, encodedSize: 17, decodedSize: 18 },
    ];
    const output = patchCascPrebuiltReferences(bytes, records);
    const expected = bytes.slice(),
      view = new DataView(expected.buffer);
    records.forEach((r, i) => {
      const pos = 46 + i * 25;
      expected.set(Buffer.from(r.newEkey, "hex"), pos);
      view.setUint32(pos + 16, r.encodedSize);
      view.setUint32(pos + 21, r.decodedSize);
    });
    assert.deepEqual(output, expected);
    assert.deepEqual(bytes, before);
    assert.notEqual(output.buffer, bytes.buffer);
    assert.equal(output[66], 0);
    assert.equal(output[91], 1);
    assert.deepEqual(patchCascPrebuiltReferences(bytes, []), bytes);
  });

  test("patch copies Buffer-backed inputs rather than retaining slice aliases", () => {
    const { bytes, entries } = fixture();
    const input = Buffer.from(bytes),
      before = Buffer.from(input);
    const output = patchCascPrebuiltReferences(input, [
      { ...entries[0], newEkey: key(10), encodedSize: 1, decodedSize: 2 },
    ]);
    assert.deepEqual(input, before);
    assert.notEqual(output.buffer, input.buffer);
    assert.equal(output.constructor, Uint8Array);
  });

  test("duplicate requests, missing/unaligned/ambiguous keys and unsafe paths reject", () => {
    const { bytes, entries } = fixture();
    for (const requests of [
      [entries[0], entries[0]],
      [entries[0], { ...entries[1], oldEkey: entries[0].oldEkey.toUpperCase() }],
      [{ ...entries[0], oldEkey: key(900) }],
      [{ ...entries[0], oldEkey: Buffer.from(bytes.subarray(47, 63)).toString("hex") }],
      [{ ...entries[0], path: "data/../bad" }],
      [{ ...entries[0], path: "DATA/test" }],
      [{ ...entries[0], path: "data/file\tbad" }],
      [{ ...entries[0], path: "data/file\u007fbad" }],
      [{ ...entries[0], add: true }],
      [{ ...entries[0], oldEkey: "__proto__" }],
    ])
      assert.throws(() => inspectCascPackStorageSpecs(bytes, requests), /CASC_OVERLAY_INVALID/);
    const duplicate = bytes.slice();
    duplicate.set(duplicate.subarray(46, 62), 71);
    assert.throws(
      () => inspectCascPackStorageSpecs(duplicate, [entries[0]]),
      /CASC_OVERLAY_INVALID/,
    );
  });

  test("patch validates every record before mutation and rejects invalid uint32 lengths", () => {
    const { bytes, entries } = fixture(),
      before = bytes.slice();
    for (const value of [-1, 0x100000000, 0.5, NaN, Infinity, "1", 1n]) {
      for (const field of ["encodedSize", "decodedSize"]) {
        const record = {
          ...entries[0],
          newEkey: key(10),
          encodedSize: 1,
          decodedSize: 2,
          [field]: value,
        };
        assert.throws(() => patchCascPrebuiltReferences(bytes, [record]), /CASC_OVERLAY_INVALID/);
      }
    }
    assert.throws(
      () =>
        patchCascPrebuiltReferences(bytes, [
          { ...entries[0], newEkey: key(10), encodedSize: 1, decodedSize: 2 },
          { ...entries[1], newEkey: "bad", encodedSize: 1, decodedSize: 2 },
        ]),
      /CASC_OVERLAY_INVALID/,
    );
    assert.deepEqual(bytes, before);
  });

  test("independent metadata count supports 27000 entries without raising overlay payload caps", () => {
    const { bytes, entries } = fixture(27000);
    const actual = inspectCascPackStorageSpecs(bytes, entries);
    assert.equal(actual.length, 27000);
    assert.deepEqual(
      createCascPackStorageLookup(bytes, { expectedQueries: 27000 })(entries),
      actual,
    );
    assert.equal(actual.at(-1).oldEkey, entries.at(-1).oldEkey);
    assert.throws(
      () =>
        inspectCascPackStorageSpecs(
          bytes,
          Array(CASC_PACK_METADATA_LIMITS.entries + 1).fill(entries[0]),
        ),
      /CASC_OVERLAY_INVALID/,
    );
    assert.throws(
      () => inspectCascPackStorageSpecs(new Uint8Array(10), []),
      /CASC_OVERLAY_INVALID/,
    );
  });

  // 可复用查询不受原始 Buffer 后续修改影响，且不放宽重复字节规则。
  // Reusable lookup ignores later source Buffer mutation and retains duplicate-byte rules.
  test("reusable lookup snapshots once and preserves exact occurrence rejection", () => {
    const { bytes, entries } = fixture();
    const expected = inspectCascPackStorageSpecs(bytes, entries);
    const source = Buffer.from(bytes),
      lookup = createCascPackStorageLookup(source);
    source.fill(0);
    assert.ok(Object.isFrozen(lookup));
    for (let i = 0; i < entries.length; i++) assert.deepEqual(lookup([entries[i]]), [expected[i]]);
    assert.throws(() => lookup([entries[0], entries[0]]));
    const malformed = fixture();
    // 第二次出现位于下一条记录内部的非对齐位置。
    // The second occurrence is unaligned inside the following record.
    malformed.bytes.set(Buffer.from(malformed.entries[0].oldEkey, "hex"), 46 + 25 + 1);
    const duplicate = createCascPackStorageLookup(malformed.bytes);
    assert.throws(() => duplicate([malformed.entries[0]]));
    assert.throws(() => createCascPackStorageLookup(new Uint8Array(16 * 1024 * 1024 + 1)));
  });

  // 紧凑索引与原有字符串查询必须逐项一致，指纹绝不代替完整键比较。
  // Compact indexing must match the original string lookup; fingerprints never replace full-key comparison.
  test("compact storage lookup matches default lookup and snapshots Buffer inputs", () => {
    const { bytes, entries } = fixture(2048),
      source = Buffer.from(bytes),
      indexed = createCascPackStorageLookup(source, { expectedQueries: 2048 }),
      original = createCascPackStorageLookup(bytes);
    source.fill(0);
    assert.deepEqual(indexed(entries), original(entries));
    assert.ok(Object.isFrozen(indexed(entries.slice(0, 1))[0]));
    for (const lookup of [indexed, original]) {
      assert.throws(() => lookup([{ path: "data/absent", oldEkey: "ff".repeat(16) }]));
      assert.throws(() => lookup([entries[0], entries[0]]));
      assert.throws(() => lookup([{ ...entries[0], add: true }]));
      assert.throws(() => lookup([{ ...entries[0], path: "../unsafe" }]));
      assert.throws(() => lookup([{ ...entries[0], oldEkey: "bad" }]));
    }
  });

  test("compact lookup rejects aligned, unaligned, crossing and all-identical duplicate keys", () => {
    for (const offset of [25, 26, 46, "all"]) {
      const { bytes, entries } = fixture(2048),
        duplicate = bytes.slice(46, 62);
      if (offset === "all") {
        for (let i = 1; i < 2048; i++) bytes.set(duplicate, 46 + i * 25);
      } else bytes.set(duplicate, 46 + offset);
      for (const expectedQueries of [0, 2048]) {
        const lookup = createCascPackStorageLookup(bytes, { expectedQueries });
        assert.throws(() => lookup([entries[0]]), /CASC_OVERLAY_INVALID/);
      }
    }
  });

  test("compact lookup resolves many genuine fingerprint collisions by exact key", () => {
    const { bytes, entries } = fixture(2048);
    // 每一对 (0,31)/(1,0) 的多项式指纹相同；组合成 256 个不同完整键。
    // Each (0,31)/(1,0) pair has the same polynomial fingerprint, yielding 256 distinct full keys.
    for (let i = 0; i < 256; i++) {
      const keyBytes = new Uint8Array(16);
      for (let pair = 0; pair < 8; pair++) {
        keyBytes[pair * 2] = (i >> pair) & 1;
        keyBytes[pair * 2 + 1] = keyBytes[pair * 2] ? 0 : 31;
      }
      bytes.set(keyBytes, 46 + i * 25);
      entries[i].oldEkey = Buffer.from(keyBytes).toString("hex");
    }
    const original = createCascPackStorageLookup(bytes),
      indexed = createCascPackStorageLookup(bytes, { expectedQueries: 2048 });
    let accepted = 0;
    for (const entry of entries.slice(0, 256)) {
      let expected;
      try {
        expected = original([entry]);
      } catch {
        assert.throws(() => indexed([entry]), /CASC_OVERLAY_INVALID/);
        continue;
      }
      assert.deepEqual(indexed([entry]), expected);
      accepted++;
    }
    assert.ok(accepted > 0);
  });

  test("compact lookup requires both thresholds and rejects unbounded query hints", () => {
    for (const count of [2047, 2048]) {
      const { bytes, entries } = fixture(count);
      for (const expectedQueries of [undefined, 2047, 2048]) {
        const originalIndexOf = String.prototype.indexOf;
        let searches = 0;
        String.prototype.indexOf = function (needle, from) {
          if (this.length === count * 25 && needle.length === 16) searches++;
          return originalIndexOf.call(this, needle, from);
        };
        try {
          const lookup = createCascPackStorageLookup(bytes, { expectedQueries });
          lookup([entries[0]]);
        } finally {
          String.prototype.indexOf = originalIndexOf;
        }
        assert.equal(searches, count >= 2048 && expectedQueries >= 2048 ? 0 : 2);
      }
    }
    const { bytes } = fixture();
    for (const expectedQueries of [-1, 1.5, NaN, Infinity, "2048", 32769])
      assert.throws(() => createCascPackStorageLookup(bytes, { expectedQueries }));
  });

  // 零私有输入的零售布局合成检查，不声称与旧 Python 序号索引参考逐字节一致。
  // Synthetic retail-layout checks with no private inputs, not parity with the old ordinal-index Python reference.
  function retailFixture() {
    const original = fixture();
    const specs = Buffer.from("n\0z\0b:64K*=n\0b:256K*=z\0");
    const bytes = new Uint8Array(46 + 75 + specs.length);
    bytes.set(original.bytes.subarray(0, 46 + 75));
    bytes[7] = 16;
    new DataView(bytes.buffer).setUint32(42, specs.length);
    bytes.set(specs, 46 + 75);
    [0, 2, 4].forEach((offset, i) => {
      bytes[46 + i * 25 + 20] = offset;
    });
    return { bytes, entries: original.entries };
  }

  test("retail EST byte offsets preserve specs and reject interior or out-of-table references", () => {
    const { bytes, entries } = retailFixture();
    const before = bytes.slice();
    assert.deepEqual(
      inspectCascPackStorageSpecs(bytes, entries).map(({ spec, specIndex }) => [spec, specIndex]),
      [
        ["n", 0],
        ["z", 2],
        ["b:64K*=n", 4],
      ],
    );
    const compressed = bytes.slice();
    compressed[116] = 13;
    assert.equal(inspectCascPackStorageSpecs(compressed, [entries[2]])[0].spec, "b:256K*=z");
    for (const offset of [1, 3, 5, 22, 255]) {
      const invalid = bytes.slice();
      invalid[116] = offset;
      assert.throws(() => inspectCascPackStorageSpecs(invalid, entries), /CASC_OVERLAY_INVALID/);
    }
    const patched = patchCascPrebuiltReferences(bytes, [
      { ...entries[2], newEkey: key(900), encodedSize: 123, decodedSize: 122 },
    ]);
    assert.equal(patched[116], 4);
    assert.deepEqual(bytes, before);
  });

  test("retail raw BLTE emits exact 64 KiB N blocks, checksums and empty-record framing", async () => {
    const digest = (data) => new Uint8Array(createHash("md5").update(data).digest());
    const hash = (data) => Buffer.from(digest(data)).toString("hex");
    const generate = createCascOverlayTransform({
      md5: digest,
      compress: (data) => new Uint8Array(deflateSync(data, { level: 9 })),
    });
    for (const length of [0, 65536, 65537, 131073]) {
      const { bytes: vfs, entries } = retailFixture();
      const root = fixture(1).bytes;
      const oldVfs = key(900);
      root.set(Buffer.from(oldVfs, "hex"), 46);
      const config = Buffer.from(
        `vfs-1 = ${hash(vfs)} ${oldVfs}\nvfs-1-size = ${vfs.length} ${vfs.length}\nvfs-root = ${hash(root)} ${key(901)}\nvfs-root-size = ${root.length} ${root.length}\n`,
      );
      const content = Uint8Array.from({ length }, (_, i) => i % 251);
      const parts = [config, vfs, root, content];
      const result = await generate(new Uint8Array(Buffer.concat(parts)), {
        parts: parts.map((p) => p.length),
        jobs: [{ path: entries[2].path, oldEkey: entries[2].oldEkey }],
        location: [0, 2],
      });
      const archive = result.files["data/00-00000002.data"];
      const encoded = archive.subarray(0, result.inventory[0].encodedSize);
      const view = new DataView(encoded.buffer, encoded.byteOffset, encoded.length);
      const count = Math.max(1, Math.ceil(length / 65536));
      assert.equal(Buffer.from(encoded.subarray(0, 4)).toString(), "BLTE");
      assert.equal(view.getUint32(4), 12 + count * 24);
      assert.equal(encoded[8], 15);
      assert.equal(encoded[9] * 65536 + encoded[10] * 256 + encoded[11], count);
      let at = 12 + count * 24,
        decoded = 0;
      for (let i = 0; i < count; i++) {
        const size = Math.min(65536, length - decoded);
        assert.equal(view.getUint32(12 + i * 24), size + 1);
        assert.equal(view.getUint32(16 + i * 24), size);
        const block = encoded.subarray(at, at + size + 1);
        assert.equal(block[0], 78);
        assert.deepEqual(block.subarray(1), content.subarray(decoded, decoded + size));
        assert.deepEqual(encoded.subarray(20 + i * 24, 36 + i * 24), digest(block));
        at += size + 1;
        decoded += size;
      }
      assert.equal(at, encoded.length);
      assert.equal(decoded, length);
      assert.equal(result.inventory[0].decodedSize, length);
      assert.equal(
        result.inventory[0].ekey.slice(0, 18),
        hash(encoded.subarray(0, 12 + count * 24)).slice(0, 18),
      );
    }
  });
}

// casc-pack-writer
{
  // 仅用合成字节、Python 原生编码和内存暂存替身测试补充包；不构成浏览器或游戏证明。
  // Test packs with synthetic bytes, native Python encoding and an in-memory stage double; not browser or game proof.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { spawnSync } = nodeChild_processModule;
  const { writeCascPack } = cascPackWriterModule;
  const { decodeArchiveRecord } = archiveReaderModule;
  const { RAW_FILE_STAGE_CHUNK_BYTES } = rawFileStageModule;
  const native = new URL("../../dm/tools/casc_overlay.py", import.meta.url).pathname;
  function python(code, input, args = []) {
    const result = spawnSync("python3", ["-c", code, ...args], {
      input,
      maxBuffer: 4 * 1024 * 1024,
    });
    assert.equal(result.status, 0, result.stderr.toString());
    return new Uint8Array(result.stdout);
  }
  // 版本字段仅是注入替身的契约值，不声称宿主 Python 是固定版本。
  // The version field is only the injected double's contract value, not a claim that host Python is pinned.
  const pinnedLibrary = process.env.CASC_PACK_NATIVE_CODEC;
  const nativeCompression = `
import sys,ctypes
lib=ctypes.CDLL(sys.argv[1])
lib.png_input.restype=ctypes.POINTER(ctypes.c_ubyte)
lib.png_output.restype=ctypes.POINTER(ctypes.c_ubyte)
lib.generic_zlib_version.restype=ctypes.c_char_p
assert lib.generic_zlib_version()==b"1.2.13"
raw=sys.stdin.buffer.read()
assert lib.zlib_begin(9)==0
for at in range(0,max(1,len(raw)),65536):
 part=raw[at:at+65536]
 ctypes.memmove(lib.png_input(),part,len(part))
 size=lib.zlib_push(len(part),int(at+65536>=len(raw)))
 assert size>=0
 sys.stdout.buffer.write(ctypes.string_at(lib.png_output(),size))
`;
  const codec = {
    genericZlibVersion: "1.2.13",
    // 测试桥仅拼接小型合成样本；生产必须注入真正有界的固定版本流式编解码器。
    // The test bridge concatenates small synthetic fixtures only; production must inject the genuinely bounded pinned streaming codec.
    async *compressZlibStream(input, options) {
      const pieces = [];
      for await (const bytes of input) pieces.push(bytes);
      const raw = Buffer.concat(pieces);
      assert.ok(raw.length <= options.maxInputBytes);
      const encoded = python(
        pinnedLibrary
          ? nativeCompression
          : "import sys,zlib; sys.stdout.buffer.write(zlib.compress(sys.stdin.buffer.read(),9))",
        raw,
        pinnedLibrary ? [pinnedLibrary] : [],
      );
      for (let at = 0; at < encoded.length; at += 113) yield encoded.slice(at, at + 113);
    },
  };
  async function* source(bytes, step = 137) {
    for (let at = 0; at < bytes.length; at += step) yield bytes.slice(at, at + step);
  }
  async function* jobs(items) {
    yield* items;
  }
  function memoryStage(faults = {}) {
    let data = new Uint8Array(),
      busy = false;
    const log = [],
      state = { disposed: false, created: false, log };
    const run = async (name, action) => {
      assert.equal(busy, false);
      busy = true;
      try {
        log.push(name);
        await Promise.resolve();
        await faults[name]?.();
        return action();
      } finally {
        busy = false;
      }
    };
    state.createStage = async () => {
      state.created = true;
      return {
        append(bytes) {
          return run("append", () => {
            const next = new Uint8Array(data.length + bytes.length);
            next.set(data);
            next.set(bytes, data.length);
            data = next;
          });
        },
        backpatch(offset, bytes) {
          return run("backpatch", () => data.set(bytes, offset));
        },
        finalize() {
          return run("finalize", () => ({
            size: data.length,
            file: new Blob([data]),
            readAt: async (offset, length) => data.slice(offset, offset + length),
          }));
        },
        async dispose() {
          state.disposed = true;
          await faults.dispose?.();
          data = new Uint8Array();
        },
      };
    };
    return state;
  }
  async function hashFile(file, { chunkBytes, signal }) {
    const hash = createHash("sha256");
    let bytesHashed = 0;
    for (let at = 0; at < file.size; at += chunkBytes) {
      signal?.throwIfAborted();
      const bytes = new Uint8Array(await file.slice(at, at + chunkBytes).arrayBuffer());
      bytesHashed += bytes.length;
      hash.update(bytes);
    }
    return { sha256: hash.digest("hex"), bytesHashed };
  }
  function options(items, stage, extra = {}) {
    return {
      jobs: jobs(items),
      location: [0, 8],
      locationAvailable: async () => true,
      maxPackBytes: 3 * 1024 * 1024,
      expectedEntries: items.length,
      codec,
      hashFile,
      createStage: stage.createStage,
      ...extra,
    };
  }
  const job = (spec, bytes, path = "data/test.bin") => ({
    path,
    spec,
    decodedSize: bytes.length,
    source: source(bytes),
  });
  test("serial n/z/BLTE/empty pack bytes, EKeys, index and metadata match native Python", async () => {
    const raw = Uint8Array.from({ length: 600123 }, (_, i) => ((i * 19) ^ (i >>> 7)) & 255);
    const inputs = [
      ["n", raw],
      ["z", raw],
      ["b:256K*=z", raw],
      ["n", new Uint8Array()],
      ["z", new Uint8Array()],
      ["b:256K*=z", new Uint8Array()],
      ["n", new Uint8Array([66, 76, 84, 69, 0, 0, 0, 10, 9, 8, 7, 6])],
      ["n", new Uint8Array([66, 76, 84, 69, 0, 0, 0, 0])],
    ];
    const items = inputs.map(([spec, bytes], i) => job(spec, bytes, `data/${i}.bin`));
    const stage = memoryStage();
    const result = await writeCascPack(options(items, stage));
    let offset = 0,
      expectedIndex = "# pack\t00-00000008.data\n";
    for (let i = 0; i < inputs.length; i++) {
      const [spec, bytes] = inputs[i];
      const output = python(
        "import sys,importlib.util; s=importlib.util.spec_from_file_location('casc',sys.argv[1]); m=importlib.util.module_from_spec(s); s.loader.exec_module(m); e=m.encode_by_espec(sys.stdin.buffer.read(),sys.argv[2]); sys.stdout.buffer.write(m.ekey_for(e,0,8,int(sys.argv[3]))+e)",
        bytes,
        [native, spec, String(offset)],
      );
      const expectedKey = Buffer.from(output.subarray(0, 16)).toString("hex"),
        encoded = output.subarray(16);
      assert.deepEqual(await result.source.readAt(offset, encoded.length), encoded);
      assert.deepEqual(result.records[i], {
        path: `data/${i}.bin`,
        newEkey: expectedKey,
        encodedSize: encoded.length,
        decodedSize: bytes.length,
      });
      expectedIndex += `data/${i}.bin\t${expectedKey}\t${encoded.length}\t${bytes.length}\n`;
      offset += encoded.length;
    }
    assert.equal(result.source.size, offset);
    assert.equal(result.index, expectedIndex);
    assert.deepEqual(result.pack, {
      name: "00-00000008.data",
      size: offset,
      sha256: (await hashFile(result.source.file, { chunkBytes: 1024 })).sha256,
    });
    assert.equal(stage.disposed, false);
    assert.equal(stage.log.filter((n) => n === "backpatch").length, 2);
    await result.dispose();
    assert.equal(stage.disposed, true);
  });
  test("empty pack finalizes and hashing is exact", async () => {
    const stage = memoryStage();
    const result = await writeCascPack(options([], stage));
    assert.equal(result.pack.size, 0);
    assert.equal(result.pack.sha256, createHash("sha256").digest("hex"));
    await result.dispose();
  });

  // 零长、64 KiB 边界和相邻记录都保留原样 BLTE 编码规格。
  // Empty records, 64 KiB boundaries and neighboring records retain raw BLTE framing.
  test("raw BLTE pack records preserve exact framing, checksums, offsets and decoded bytes", async () => {
    const inputs = [0, 1, 65535, 65536, 65537, 131073].map((length) =>
      Uint8Array.from({ length }, (_, i) => (i * 29 + (i >>> 8)) & 255),
    );
    const before = Uint8Array.of(17, 18, 19),
      after = Uint8Array.of(20, 21);
    const items = [
      job("n", before, "data/before"),
      ...inputs.map((bytes, i) => job("b:64K*=n", bytes, `data/raw-${i}`)),
      job("n", after, "data/after"),
    ];
    const stage = memoryStage();
    const result = await writeCascPack(
      options(items, stage, {
        codec: {
          genericZlibVersion: "1.2.13",
          compressZlibStream() {
            assert.fail("Raw BLTE must not invoke zlib");
          },
        },
      }),
    );
    const md5 = (bytes) => createHash("md5").update(bytes).digest();
    let offset = before.length;
    for (let i = 0; i < inputs.length; i++) {
      const bytes = inputs[i],
        count = Math.max(1, Math.ceil(bytes.length / 65536)),
        header = Buffer.alloc(12 + count * 24),
        blocks = [];
      header.write("BLTE");
      header.writeUInt32BE(header.length, 4);
      header[8] = 15;
      header.writeUIntBE(count, 9, 3);
      for (let index = 0; index < count; index++) {
        const payload = bytes.subarray(index * 65536, (index + 1) * 65536),
          block = Buffer.concat([Buffer.from("N"), payload]);
        header.writeUInt32BE(block.length, 12 + index * 24);
        header.writeUInt32BE(payload.length, 16 + index * 24);
        md5(block).copy(header, 20 + index * 24);
        blocks.push(block);
      }
      const encoded = Buffer.concat([header, ...blocks]),
        record = result.records[i + 1],
        expectedKey =
          md5(header).toString("hex").slice(0, 18) + "0008" + offset.toString(16).padStart(10, "0");
      if (bytes.length === 0)
        assert.deepEqual(
          new Uint8Array(encoded),
          python(
            "import sys,importlib.util; s=importlib.util.spec_from_file_location('casc',sys.argv[1]); m=importlib.util.module_from_spec(s); s.loader.exec_module(m); sys.stdout.buffer.write(m.encode_by_espec(b'', 'b:64K*=n'))",
            new Uint8Array(),
            [native],
          ),
        );
      assert.deepEqual(record, {
        path: `data/raw-${i}`,
        newEkey: expectedKey,
        encodedSize: encoded.length,
        decodedSize: bytes.length,
      });
      assert.deepEqual(await result.source.readAt(offset, encoded.length), new Uint8Array(encoded));
      const decoded = await decodeArchiveRecord(
        { size: result.source.size, read: result.source.readAt },
        {
          offset,
          encodedSize: encoded.length,
          decodedSize: bytes.length,
          encoding: "b:64K*=n",
          ckey: md5(bytes).toString("hex"),
        },
        { verifyMd5: async (value, expected) => md5(value).toString("hex") === expected },
      );
      assert.deepEqual(decoded, bytes);
      offset += encoded.length;
    }
    assert.equal(Number.parseInt(result.records.at(-1).newEkey.slice(22), 16), offset);
    assert.deepEqual(await result.source.readAt(offset, after.length), after);
    assert.equal(result.source.size, offset + after.length);
    assert.equal(stage.log.filter((name) => name === "backpatch").length, inputs.length);
    assert.equal(
      result.index,
      "# pack\t00-00000008.data\n" +
        result.records
          .map(
            (record) =>
              `${record.path}\t${record.newEkey}\t${record.encodedSize}\t${record.decodedSize}\n`,
          )
          .join(""),
    );
    await result.dispose();
  });

  test("large raw BLTE header reservation stays within bounded stage chunks", async () => {
    const writes = [];
    let disposed = false;
    const item = { ...job("b:64K*=n", new Uint8Array()), decodedSize: 0xf0000000 };
    await assert.rejects(
      writeCascPack(
        options(
          [item],
          {},
          {
            maxPackBytes: 2 ** 32,
            createStage: async () => ({
              async append(bytes) {
                assert.ok(bytes.buffer.byteLength <= RAW_FILE_STAGE_CHUNK_BYTES);
                writes.push(bytes.length);
              },
              async dispose() {
                disposed = true;
              },
            }),
          },
        ),
      ),
      /CASC_PACK_DECODED_SIZE/,
    );
    assert.deepEqual(writes, [
      RAW_FILE_STAGE_CHUNK_BYTES,
      12 + 24 * 61440 - RAW_FILE_STAGE_CHUNK_BYTES,
    ]);
    assert.equal(disposed, true);
  });

  test("raw BLTE rejects impossible record and remaining pack sizes before consuming input", async () => {
    for (const [decodedSize, maxPackBytes, prefix, expected] of [
      [0xffffffff, 2 ** 40, false, /CASC_PACK_ENCODED_SIZE/],
      [65536, 65573, true, /CASC_PACK_PACK_SIZE/],
      [0, 36, false, /CASC_PACK_PACK_SIZE/],
    ]) {
      let consumed = false;
      const item = {
        ...job("b:64K*=n", new Uint8Array()),
        decodedSize,
        source: (async function* () {
          consumed = true;
          yield new Uint8Array();
        })(),
      };
      const stage = memoryStage();
      const items = prefix ? [job("n", Uint8Array.of(42), "data/prefix"), item] : [item];
      await assert.rejects(writeCascPack(options(items, stage, { maxPackBytes })), expected);
      assert.equal(consumed, false);
      assert.equal(stage.log.filter((name) => name === "append").length, prefix ? 1 : 0);
      assert.equal(stage.disposed, true);
    }
  });
  test("collision rejects before stage creation and source consumption", async () => {
    const stage = memoryStage();
    await assert.rejects(
      writeCascPack(options([], stage, { locationAvailable: async () => false })),
      /LOCATION_COLLISION/,
    );
    assert.equal(stage.created, false);
  });
  test("source exact counts, backing-buffer bounds, unknown specs and index paths fail closed", async () => {
    const cases = [
      { ...job("n", new Uint8Array(3)), decodedSize: 2 },
      { ...job("z", new Uint8Array(3)), decodedSize: 4 },
      { ...job("b:256K*=z", new Uint8Array(3)), decodedSize: 4 },
      { ...job("n", new Uint8Array()), decodedSize: 2 ** 31 },
      { ...job("n", new Uint8Array()), decodedSize: 2 ** 32 },
      job("b:64K*=z", new Uint8Array()),
      job("n", new Uint8Array(), "+data/a"),
      job("n", new Uint8Array(), "data/a\tb"),
      job("n", new Uint8Array(), "data/../a"),
      {
        ...job("n", new Uint8Array(1)),
        source: (async function* () {
          yield new Uint8Array(1024 * 1024 + 1).subarray(0, 1);
        })(),
      },
      job("n", new Uint8Array([66, 76, 84, 69])),
    ];
    for (const item of cases) {
      const stage = memoryStage();
      await assert.rejects(writeCascPack(options([item], stage)), /CASC_PACK_/);
      assert.equal(stage.disposed, true);
    }
  });
  test("duplicates, entry counts, pack byte budget and hash mismatch dispose partial outputs", async () => {
    const item = () => job("n", new Uint8Array([1, 2, 3]));
    const cases = [
      [[item(), item()], {}],
      [[item()], { expectedEntries: 0 }],
      [[item()], { expectedEntries: 2 }],
      [[item()], { maxPackBytes: 2 }],
      [[item()], { hashFile: async () => ({ sha256: "a".repeat(64), bytesHashed: 2 }) }],
      [[item()], { hashFile: async () => ({ sha256: "invalid", bytesHashed: 3 }) }],
    ];
    for (const [items, extra] of cases) {
      const stage = memoryStage();
      await assert.rejects(writeCascPack(options(items, stage, extra)), /CASC_PACK_/);
      assert.equal(stage.disposed, true);
    }
  });
  test("abort during append closes async source and cleans stage", async () => {
    const controller = new AbortController(),
      reason = new Error("cancelled");
    let closed = false;
    const input = (async function* () {
      try {
        yield new Uint8Array([1]);
        yield new Uint8Array([2]);
      } finally {
        closed = true;
      }
    })();
    const stage = memoryStage({ append: () => controller.abort(reason) });
    await assert.rejects(
      writeCascPack(
        options([{ path: "data/a", spec: "n", decodedSize: 2, source: input }], stage, {
          signal: controller.signal,
        }),
      ),
      reason,
    );
    assert.equal(closed, true);
    assert.equal(stage.disposed, true);
  });
  test("write/backpatch/finalize/hash failures clean up and cleanup failures remain visible", async () => {
    for (const action of ["append", "backpatch", "finalize", "hash"]) {
      const error = new Error(action),
        stage = memoryStage({
          [action]: () => {
            throw error;
          },
        });
      const extra =
        action === "hash"
          ? {
              hashFile: async () => {
                throw error;
              },
            }
          : {};
      await assert.rejects(
        writeCascPack(options([job("b:256K*=z", new Uint8Array([1]))], stage, extra)),
        error,
      );
      assert.equal(stage.disposed, true);
    }
    const stage = memoryStage({
      append: () => {
        throw new Error("write");
      },
      dispose: () => {
        throw new Error("cleanup");
      },
    });
    await assert.rejects(
      writeCascPack(options([job("n", new Uint8Array([1]))], stage)),
      (error) => error instanceof AggregateError && error.errors.length === 2,
    );
  });
  test("location bytes and pack address-space budgets reject truncation", async () => {
    for (const extra of [
      { location: [0, 256] },
      { location: [-1, 8] },
      { maxPackBytes: 2 ** 40 + 1 },
      { expectedEntries: 32769 },
    ]) {
      const stage = memoryStage();
      await assert.rejects(writeCascPack(options([], stage, extra)), /INVALID_OPTIONS/);
      assert.equal(stage.created, false);
    }
  });

  // 即使注入的编解码器提前停止，也不暴露未完成的档。
  // Even an injected codec stopping early must not expose an incomplete archive.
  test("incomplete codec consumption fails for raw zlib and each BLTE block", async () => {
    const broken = {
      genericZlibVersion: "1.2.13",
      async *compressZlibStream() {
        yield new Uint8Array([1]);
      },
    };
    for (const spec of ["z", "b:256K*=z"]) {
      const stage = memoryStage();
      await assert.rejects(
        writeCascPack(options([job(spec, new Uint8Array([1, 2]))], stage, { codec: broken })),
        /SOURCE_NOT_CONSUMED|CODEC_INCOMPLETE/,
      );
      assert.equal(stage.disposed, true);
    }
  });

  // 挂起的宿主回调不会无限阻止取消；清理是否完成单独报告。
  // Hung host callbacks cannot block cancellation forever; cleanup completion is reported separately.
  test("abort races location, jobs.next and final hashing", async () => {
    for (const action of ["location", "next", "hash"]) {
      const controller = new AbortController(),
        reason = new Error("cancel host wait"),
        stage = memoryStage();
      let reached;
      const entered = new Promise((resolve) => {
        reached = resolve;
      });
      const hang = (signal) => {
        assert.equal(signal, controller.signal);
        reached();
        return new Promise(() => {});
      };
      const extra = { signal: controller.signal };
      if (action === "location") extra.locationAvailable = (_, { signal }) => hang(signal);
      if (action === "next")
        extra.jobs = {
          [Symbol.asyncIterator]() {
            return { next: () => hang(controller.signal), return: async () => ({ done: true }) };
          },
        };
      if (action === "hash") extra.hashFile = (_, { signal }) => hang(signal);
      const work = writeCascPack(options([], stage, extra));
      await entered;
      controller.abort(reason);
      await assert.rejects(work, reason);
      assert.equal(stage.disposed, action !== "location");
    }
  });
  test("abort preserves primary cause with never-settling iterator cleanup and stage disposal", async () => {
    const controller = new AbortController(),
      reason = new Error("cancel"),
      stage = memoryStage({ dispose: () => new Promise(() => {}) });
    let reached;
    const entered = new Promise((resolve) => {
      reached = resolve;
    });
    const input = {
      [Symbol.asyncIterator]() {
        return {
          next: () => {
            reached();
            return new Promise(() => {});
          },
          return: () => new Promise(() => {}),
        };
      },
    };
    const work = writeCascPack(options([], stage, { jobs: input, signal: controller.signal }));
    await entered;
    controller.abort(reason);
    await assert.rejects(
      work,
      (error) =>
        error instanceof AggregateError &&
        error.cause === reason &&
        error.cleanup instanceof Promise &&
        error.errors[1].message === "CASC_PACK_CLEANUP_PENDING",
    );
    assert.equal(stage.disposed, true);
  });
  test("late stage acquisition after cancellation is disposed without losing ownership", async () => {
    const controller = new AbortController(),
      reason = new Error("late cancel"),
      stage = memoryStage();
    let acquire, entered;
    const reached = new Promise((resolve) => {
      entered = resolve;
    });
    const createStage = () => {
      entered();
      return new Promise((resolve) => {
        acquire = resolve;
      });
    };
    const work = writeCascPack(options([], stage, { createStage, signal: controller.signal }));
    await reached;
    controller.abort(reason);
    let failure;
    try {
      await work;
    } catch (error) {
      failure = error;
    }
    assert.ok(failure instanceof AggregateError);
    assert.equal(failure.cause, reason);
    assert.equal(stage.disposed, false);
    acquire(await stage.createStage());
    await failure.cleanup;
    assert.equal(stage.disposed, true);
  });
  test("iterator cleanup rejection does not replace a source failure", async () => {
    const primary = new Error("source failed"),
      cleanup = new Error("iterator close failed"),
      stage = memoryStage();
    const input = {
      [Symbol.asyncIterator]() {
        return {
          next: async () => {
            throw primary;
          },
          return: async () => {
            throw cleanup;
          },
        };
      },
    };
    await assert.rejects(
      writeCascPack(options([], stage, { jobs: input })),
      (error) =>
        error instanceof AggregateError &&
        error.cause === primary &&
        error.errors.includes(cleanup),
    );
    assert.equal(stage.disposed, true);
  });

  // 迟到的获取失败可能包含资源清理失败，不能被取消原因掩盖。
  // Late acquisition failures may contain cleanup failures and must not be hidden by cancellation.
  test("late acquisition cleanup failure remains observable after cleanup deadline", async () => {
    const controller = new AbortController(),
      reason = new Error("cancel"),
      cleanupFailure = new Error("RAW_STAGE_CLEANUP_FAILED");
    const lateFailure = new AggregateError(
      [cleanupFailure],
      "RAW_STAGE_OPERATION_AND_CLEANUP_FAILED",
    );
    let rejectAcquire, reached;
    const entered = new Promise((resolve) => {
      reached = resolve;
    });
    const stage = memoryStage();
    const work = writeCascPack(
      options([], stage, {
        signal: controller.signal,
        createStage: () => {
          reached();
          return new Promise((_, reject) => {
            rejectAcquire = reject;
          });
        },
      }),
    );
    await entered;
    controller.abort(reason);
    let failure;
    try {
      await work;
    } catch (error) {
      failure = error;
    }
    assert.ok(failure instanceof AggregateError);
    assert.equal(failure.cause, reason);
    rejectAcquire(lateFailure);
    const results = await failure.cleanup;
    assert.equal(results[0].status, "rejected");
    assert.equal(results[0].reason, lateFailure);
    assert.ok(results[0].reason.errors.includes(cleanupFailure));
  });
  test("ordinary acquisition failure preserves its original error without duplication", async () => {
    const reason = new Error("create failed"),
      stage = memoryStage();
    await assert.rejects(
      writeCascPack(
        options([], stage, {
          createStage: async () => {
            throw reason;
          },
        }),
      ),
      reason,
    );
  });
  test("oversized paths reject before allocating transformations and hash-prefixed TSV rows reject", async () => {
    const original = String.prototype.toLowerCase;
    String.prototype.toLowerCase = function () {
      if (this.length > 4096) throw new Error("unexpected large transformation");
      return original.call(this);
    };
    try {
      for (const path of ["a".repeat(4097), "#data/test.bin"]) {
        const stage = memoryStage();
        await assert.rejects(
          writeCascPack(options([job("n", new Uint8Array(), path)], stage)),
          /CASC_PACK_INVALID_PATH/,
        );
        assert.equal(stage.disposed, true);
      }
    } finally {
      String.prototype.toLowerCase = original;
    }
  });

  // 上限模式允许单次过滤枚举，但不承诺候选集合完整；精确计数模式保持原行为。
  // Maximum-only mode allows one-pass filtered enumeration but does not promise candidate completeness; exact-count mode retains its behavior.
  test("max-only mode accepts fewer jobs and returns actual count without a pre-pass", async () => {
    const stage = memoryStage();
    let enumerations = 0,
      reads = 0;
    const stream = {
      async *[Symbol.asyncIterator]() {
        enumerations++;
        yield {
          path: "data/filtered.bin",
          spec: "n",
          decodedSize: 1,
          source: {
            async *[Symbol.asyncIterator]() {
              reads++;
              yield new Uint8Array([7]);
            },
          },
        };
      },
    };
    const result = await writeCascPack(
      options([], stage, { jobs: stream, expectedEntries: undefined, maxEntries: 3 }),
    );
    assert.equal(result.count, 1);
    assert.equal(result.records.length, 1);
    assert.equal(enumerations, 1);
    assert.equal(reads, 1);
    await result.dispose();
    const emptyStage = memoryStage();
    const empty = await writeCascPack(
      options([], emptyStage, { expectedEntries: undefined, maxEntries: 0 }),
    );
    assert.equal(empty.count, 0);
    await empty.dispose();
  });
  test("at least one valid explicit entry count or bound is required", async () => {
    for (const extra of [
      { expectedEntries: undefined },
      { expectedEntries: undefined, maxEntries: null },
      { expectedEntries: undefined, maxEntries: -1 },
      { expectedEntries: undefined, maxEntries: 1.5 },
      { expectedEntries: undefined, maxEntries: 32769 },
      { expectedEntries: 2, maxEntries: 1 },
      { expectedEntries: null, maxEntries: 1 },
    ]) {
      const stage = memoryStage();
      await assert.rejects(writeCascPack(options([], stage, extra)), /INVALID_OPTIONS/);
      assert.equal(stage.created, false);
    }
  });
  test("max-only overrun closes enumeration and cleans partial output", async () => {
    const stage = memoryStage();
    let closed = false;
    const stream = {
      async *[Symbol.asyncIterator]() {
        try {
          yield job("n", new Uint8Array([1]), "data/a");
          yield job("n", new Uint8Array([2]), "data/b");
        } finally {
          closed = true;
        }
      },
    };
    await assert.rejects(
      writeCascPack(
        options([], stage, { jobs: stream, expectedEntries: undefined, maxEntries: 1 }),
      ),
      /INVALID_JOB/,
    );
    assert.equal(closed, true);
    assert.equal(stage.disposed, true);
    assert.ok(!stage.log.includes("finalize"));
  });
  test("exact counts remain mandatory when supplied alongside a looser maximum", async () => {
    const stage = memoryStage();
    const result = await writeCascPack(
      options([job("n", new Uint8Array([1]))], stage, { expectedEntries: 1, maxEntries: 2 }),
    );
    assert.equal(result.count, 1);
    await result.dispose();
    for (const items of [
      [],
      [job("n", new Uint8Array([1]), "data/a"), job("n", new Uint8Array([2]), "data/b")],
    ]) {
      const failed = memoryStage();
      await assert.rejects(
        writeCascPack(options(items, failed, { expectedEntries: 1, maxEntries: 2 })),
        /ENTRY_COUNT|INVALID_JOB/,
      );
      assert.equal(failed.disposed, true);
    }
  });
  test("max-only enumeration errors are not interpreted as successful early completion", async () => {
    const stage = memoryStage(),
      reason = new Error("candidate enumeration failed");
    const stream = {
      async *[Symbol.asyncIterator]() {
        yield job("n", new Uint8Array([1]));
        throw reason;
      },
    };
    await assert.rejects(
      writeCascPack(
        options([], stage, { jobs: stream, expectedEntries: undefined, maxEntries: 2 }),
      ),
      reason,
    );
    assert.equal(stage.disposed, true);
    assert.ok(!stage.log.includes("finalize"));
  });
}

// casc-pinned-md5-view
{
  // 合成 TVFS 输入验证固定 MD5 适配器只散列视图范围；不加载游戏数据或 codec WASM。
  // Synthetic TVFS input verifies the pinned MD5 adapter hashes only the view; no game data or codec WASM.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { spawnSync } = nodeChild_processModule;
  const { deflateSync } = nodeZlibModule;
  const { fileURLToPath } = nodeUrlModule;
  const { createCascOverlayTransform, createPinnedCascOverlayTransform } = overlayModule;

  const compress = (bytes) => new Uint8Array(deflateSync(bytes, { level: 9 }));

  test("pinned CASC MD5 hashes exact Buffer subviews rather than unrelated backing bytes", async () => {
    const fixture = spawnSync(
      "python3",
      [fileURLToPath(new URL("../toolchain/casc-overlay-js/test/fixture.py", import.meta.url))],
      { encoding: "utf8", maxBuffer: 8 * 1024 * 1024 },
    );
    assert.equal(fixture.status, 0, fixture.stderr);
    const request = JSON.parse(fixture.stdout);
    const payload = Buffer.from(request.payload, "base64");
    const oracle = createCascOverlayTransform({
      md5: (bytes) => new Uint8Array(createHash("md5").update(bytes).digest()),
      compress,
    });
    const expected = await oracle(new Uint8Array(payload), request.options);
    const generate = createPinnedCascOverlayTransform({
      codec: { genericZlibVersion: "1.2.13", compressZlib: compress },
      md5Hasher: await createMD5(),
    });
    for (const padding of [0, 19]) {
      const backing = Buffer.alloc(payload.length + padding * 2, 0xa7);
      backing.set(payload, padding);
      const actual = await generate(
        backing.subarray(padding, padding + payload.length),
        request.options,
      );
      assert.deepEqual(actual.inventory, expected.inventory);
      assert.deepEqual(Object.keys(actual.files).sort(), Object.keys(expected.files).sort());
      for (const name of Object.keys(expected.files))
        assert.deepEqual(
          new Uint8Array(actual.files[name]),
          new Uint8Array(expected.files[name]),
          name,
        );
      assert.ok(backing.subarray(0, padding).every((byte) => byte === 0xa7));
      assert.ok(backing.subarray(padding + payload.length).every((byte) => byte === 0xa7));
    }
  });
}

// casc-prebuilt-routing
{
  // 合成源与注入的 Node 编解码器验证专用外部引用路由；不构成真实 WASM、浏览器或游戏证明。
  // Verify dedicated external-reference routing with synthetic sources and injected Node codecs, not actual WASM, browser or game proof.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { createHash } = nodeCryptoModule;
  const { deflateSync } = nodeZlibModule;
  const { writeCascPrebuiltOverlay, reserveCascPackLocations, OVERLAY_LIMITS } =
    cascOverlayWriterModule;
  const { createCascPrebuiltOverlayTransform } = overlayModule;
  const { OPERATION_LIMITS, validateRequest, transferableResult } = contractModule;
  const { fixture, source, jobs } = cascOverlayFixtureModule;
  const manifest = Buffer.from(fixture.files["data/build_manifest.psv"], "base64")
    .toString()
    .split("\n");
  const oldEkey = manifest.find((line) => line.includes("data:data/n.bin")).split("|")[6];
  function external() {
    const bytes = new Uint8Array([9, 8, 7]);
    return {
      pack: {
        name: "00-00000003.data",
        size: 3,
        sha256: createHash("sha256").update(bytes).digest("hex"),
      },
      records: [
        {
          path: "data/n.bin",
          oldEkey,
          newEkey: createHash("md5").update(bytes).digest("hex").slice(0, 18) + "00030000000000",
          encodedSize: 3,
          decodedSize: 3,
        },
      ],
    };
  }
  const transform = createCascPrebuiltOverlayTransform({
    md5: (bytes) => new Uint8Array(createHash("md5").update(bytes).digest()),
    compress: (bytes) => new Uint8Array(deflateSync(bytes, { level: 9 })),
  });
  function serializer(inspect = () => {}) {
    return {
      close() {},
      async run(operation, payload, options) {
        assert.equal(operation, "casc-prebuilt-overlay");
        assert.equal(options.parts.length, options.jobs.length + 4);
        assert.deepEqual(Object.keys(options).sort(), ["jobs", "location", "parts"]);
        const message = { id: 1, operation, payload: payload.buffer, options };
        const request = validateRequest(message);
        inspect(request);
        const result = await transform(request.payload, request.options);
        return transferableResult(1, result).message.result;
      },
    };
  }
  test("reservation keeps default overlay location first and external archive separate", () => {
    assert.deepEqual(reserveCascPackLocations(source()), {
      overlayLocation: [0, 2],
      packLocation: [0, 3],
      packName: "00-00000003.data",
    });
    assert.deepEqual(
      reserveCascPackLocations(source(), { packLocation: [1, 255] }).packLocation,
      [1, 255],
    );
    for (const packLocation of [
      [0, 1],
      [0, 2],
      [0, 256],
      [-1, 2],
    ])
      assert.throws(() => reserveCascPackLocations(source(), { packLocation }));
  });
  test("dedicated host routes metadata in its own segment without copying external archive bytes", async () => {
    const references = external(),
      romfs = source();
    let checked = false;
    const out = await writeCascPrebuiltOverlay({
      romfs,
      replacements: jobs().slice(1),
      external: references,
      worker: serializer(({ payload, options }) => {
        const offset = options.parts.slice(0, -1).reduce((a, b) => a + b, 0);
        assert.deepEqual(
          JSON.parse(new TextDecoder().decode(payload.subarray(offset))),
          references,
        );
        assert.deepEqual(options.location, [0, 2]);
        checked = true;
      }),
    });
    assert.equal(checked, true);
    assert.equal(out.inventory.length, 3);
    assert.deepEqual(out.external, { count: 1, pack: references.pack });
    assert.deepEqual(Object.keys(out.files).sort(), [
      "data/.build.config",
      "data/00-00000002.data",
    ]);
    assert.ok(!romfs.reads.some((r) => r.path === "data/00-00000003.data"));
  });
  test("external-only overlay is generated instead of default empty-job no-op", async () => {
    const out = await writeCascPrebuiltOverlay({
      romfs: source(),
      external: external(),
      worker: serializer(),
    });
    assert.equal(out.inventory.length, 0);
    assert.equal(out.external.count, 1);
    assert.equal(Object.keys(out.files).length, 2);
  });
  test("declared location, range, duplicates, overlap and original association fail closed", async () => {
    const variants = [
      (e) => {
        e.pack.name = "00-00000001.data";
      },
      (e) => {
        e.pack.name = "00-00000002.data";
      },
      (e) => {
        e.records[0].newEkey = "0".repeat(18) + "00040000000000";
      },
      (e) => {
        e.records[0].encodedSize = 4;
      },
      (e) => {
        e.records[0].newEkey = e.records[0].newEkey.slice(0, 22) + "ffffffffff";
      },
      (e) => {
        e.records[0].oldEkey = "f".repeat(32);
      },
      (e) => {
        e.records.push({ ...e.records[0] });
      },
      (e) => {
        e.records[0].decodedSize = 2 ** 32;
      },
      (e) => {
        e.records[0].path = "a".repeat(4097);
      },
    ];
    for (const mutate of variants) {
      const e = external();
      mutate(e);
      await assert.rejects(
        writeCascPrebuiltOverlay({ romfs: source(), external: e, worker: serializer() }),
      );
    }
    await assert.rejects(
      writeCascPrebuiltOverlay({
        romfs: source(),
        replacements: jobs(),
        external: external(),
        worker: serializer(),
      }),
    );
  });
  test("external metadata has independent entry/path budgets before source reads", async () => {
    for (const records of [
      Array(32769).fill(external().records[0]),
      Array.from({ length: 2049 }, (_, i) => ({
        ...external().records[0],
        path: `data/${String(i).padStart(5, "0")}/` + "a".repeat(4085),
        oldEkey: i.toString(16).padStart(32, "0"),
      })),
    ]) {
      const romfs = source();
      await assert.rejects(
        writeCascPrebuiltOverlay({
          romfs,
          external: { ...external(), records },
          worker: serializer(),
        }),
      );
      assert.equal(romfs.reads.length, 0);
    }
  });
  test("transport accepts over-1MiB external segment but retains existing options/input caps", () => {
    assert.deepEqual(OPERATION_LIMITS["casc-overlay"], [64, 1048576]);
    assert.deepEqual(OPERATION_LIMITS["casc-prebuilt-overlay"], [64, 1048576]);
    assert.equal(OVERLAY_LIMITS.jobs, 4096);
    const count = 1048577,
      message = {
        id: 1,
        operation: "casc-prebuilt-overlay",
        payload: new ArrayBuffer(count),
        options: { parts: [0, 0, 0, count], jobs: [], location: [0, 2] },
      };
    assert.equal(validateRequest(message).payload.length, count);
    for (const options of [
      { ...message.options, external: "x" },
      { ...message.options, parts: [0, 0, 0, 16 * 1024 * 1024 + 1] },
      { ...message.options, jobs: Array(4097).fill({}) },
      { ...message.options, location: [0, 256] },
      { ...message.options, jobs: [{ path: "x".repeat(1048576) }], parts: [0, 0, 0, 0, count] },
    ])
      assert.throws(() => validateRequest({ ...message, options }));
    assert.throws(() =>
      validateRequest({ ...message, payload: new ArrayBuffer(64 * 1024 * 1024 + 1) }),
    );
  });
  test("worker metadata mismatch is rejected without releasing changed files", async () => {
    const delegate = serializer();
    const worker = {
      close() {},
      async run(...args) {
        const result = await delegate.run(...args);
        result.external.pack.name = "00-00000009.data";
        return result;
      },
    };
    await assert.rejects(
      writeCascPrebuiltOverlay({ romfs: source(), external: external(), worker }),
    );
  });
}
