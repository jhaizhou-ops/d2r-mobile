// Feature suite: astc-production. Original test sections retain independent lexical scopes.
import * as strictModule from "node:assert/strict";
import * as nodeTestModule from "node:test";
import * as astcJobDirectoryModule from "../astc-job-directory.mjs";
import * as nodeCryptoModule from "node:crypto";
import * as astcOutputValidatorModule from "../astc-output-validator.mjs";
import * as revisionModule from "../toolchain/overlay-recipe-js/revision.mjs";
import * as extractionAdapterFixtureModule from "./helpers/extraction-adapter-fixture.mjs";
import * as astcPackJobsModule from "../astc-pack-jobs.mjs";
import * as cascOverlayWriterModule from "../casc-overlay-writer.mjs";
import * as astcProducerClientModule from "../astc-producer-client.mjs";
import * as astcProducerWorkerModule from "../sealed-runtime/astc-producer-worker.mjs";
import * as archiveReadRpcModule from "../archive-read-rpc.mjs";
import * as cascPackWriterModule from "../casc-pack-writer.mjs";
import * as cascLogicalReaderModule from "../casc-logical-reader.mjs";
import * as extractionJobAdapterModule from "../extraction-job-adapter.mjs";
import * as assertIsolatedRunModule from "./helpers/assert-isolated-run.mjs";
import * as nodeChild_processModule from "node:child_process";
import * as nodeProcessModule from "node:process";
import * as productionProfileModule from "../production-profile.mjs";
import * as browserPackagerModule from "../browser-packager.mjs";
import * as promisesModule from "node:fs/promises";
import * as packageModule from "../runtime-build/package.mjs";

// astc-job-directory
{
  // 合成 OPFS 句柄测试所有权和异步故障；不声称真实浏览器锁或磁盘行为。
  // Synthetic OPFS handles test ownership and asynchronous failures, not real browser locking or disk behavior.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { createAstcJobDirectory } = astcJobDirectoryModule;
  const id = "12345678-1234-4123-8123-123456789abc";
  const missing = () => new DOMException("missing", "NotFoundError");
  function deferred() {
    let resolve, reject;
    const promise = new Promise((a, b) => {
      resolve = a;
      reject = b;
    });
    return { promise, resolve, reject };
  }
  function fixture() {
    const entries = new Map(),
      log = [];
    let createHook, removeHook;
    const child = () => {
      const identity = {};
      return {
        kind: "directory",
        identity,
        async isSameEntry(other) {
          return other.identity === identity;
        },
      };
    };
    const root = {
      kind: "directory",
      async getDirectoryHandle(name, options) {
        log.push(options?.create ? "create" : "get");
        if (options?.create) {
          if (createHook) await createHook();
          const value = child();
          entries.set(name, value);
          return value;
        }
        if (!entries.has(name)) throw missing();
        return entries.get(name);
      },
      async removeEntry(name, options) {
        log.push("remove");
        assert.equal(options.recursive, true);
        if (removeHook) await removeHook();
        if (!entries.delete(name)) throw missing();
      },
    };
    return {
      root,
      entries,
      log,
      child,
      set createHook(value) {
        createHook = value;
      },
      set removeHook(value) {
        removeHook = value;
      },
    };
  }
  const options = (f, extra = {}) => ({
    root: f.root,
    randomUUID: () => id,
    cleanupTimeoutMs: 30,
    ...extra,
  });
  test("fresh lease retained until explicit disposal, stop before identity checked removal", async () => {
    const f = fixture(),
      lease = await createAstcJobDirectory(
        options(f, {
          stopAndDrain: () => {
            f.log.push("stop");
          },
        }),
      );
    assert.equal(lease.root, [...f.entries.values()][0]);
    assert.equal(lease.status.phase, "ready");
    assert.equal(f.entries.size, 1);
    await lease.dispose();
    assert.equal(lease.status.phase, "disposed");
    assert.equal(lease.status.removed, true);
    assert.ok(f.log.indexOf("stop") < f.log.indexOf("remove"));
    assert.throws(() => lease.root, /NOT_READY/);
    await lease.dispose();
    assert.equal(f.log.filter((v) => v === "remove").length, 1);
  });
  test("preexisting or replaced entries are never removed", async () => {
    const f = fixture(),
      name = `d2rm-astc-job-${id}`,
      original = f.child();
    f.entries.set(name, original);
    let failure;
    try {
      await createAstcJobDirectory(options(f));
    } catch (error) {
      failure = error;
    }
    assert.match(failure.cause.message, /COLLISION/);
    await failure.lease.dispose();
    assert.equal(f.entries.get(name), original);
    assert.ok(!f.log.includes("remove"));
    f.entries.delete(name);
    const lease = await createAstcJobDirectory(options(f)),
      replacement = f.child();
    f.entries.set(name, replacement);
    await assert.rejects(lease.dispose(), (error) => /IDENTITY_CHANGED/.test(error.cause.message));
    assert.equal(f.entries.get(name), replacement);
    assert.equal(lease.status.phase, "cleanup-failed");
    assert.ok(!f.log.includes("remove"));
  });
  test("failed removal is observable and retryable without losing ownership", async () => {
    const f = fixture(),
      lease = await createAstcJobDirectory(options(f));
    f.removeHook = () => {
      throw new DOMException("locked", "InvalidStateError");
    };
    await assert.rejects(lease.dispose(), /CLEANUP_FAILED/);
    assert.equal(lease.status.removed, false);
    assert.equal(f.entries.size, 1);
    f.removeHook = undefined;
    await lease.dispose();
    assert.equal(f.entries.size, 0);
    assert.equal(lease.status.removed, true);
  });
  test("uncertain removal timeout remains single-flight and can be awaited again", async () => {
    const f = fixture(),
      gate = deferred(),
      lease = await createAstcJobDirectory(options(f));
    f.removeHook = () => gate.promise;
    await assert.rejects(lease.dispose(), (error) => /TIMEOUT/.test(error.cause.message));
    assert.equal(lease.status.cleanupPending, true);
    assert.equal(lease.status.removed, false);
    const retry = lease.dispose();
    assert.equal(f.log.filter((v) => v === "remove").length, 1);
    gate.resolve();
    await retry;
    assert.equal(lease.status.removed, true);
  });
  test("late creation after timeout exposes lease and cleans only acquired directory", async () => {
    const f = fixture(),
      gate = deferred();
    f.createHook = () => gate.promise;
    let failure;
    try {
      await createAstcJobDirectory(options(f));
    } catch (error) {
      failure = error;
    }
    assert.match(failure.cause.message, /TIMEOUT/);
    assert.ok(failure.lease);
    assert.equal(failure.lease.status.cleanupPending, true);
    gate.resolve();
    await failure.lease.dispose();
    assert.equal(f.entries.size, 0);
    assert.equal(failure.lease.status.removed, true);
    assert.equal(f.log.filter((v) => v === "create").length, 1);
  });
  test("abort during creation and stop failure keep cleanup observable", async () => {
    const f = fixture(),
      gate = deferred(),
      controller = new AbortController();
    f.createHook = () => gate.promise;
    const pending = createAstcJobDirectory(options(f, { signal: controller.signal }));
    await new Promise((resolve) => setTimeout(resolve, 0));
    controller.abort();
    let failure;
    try {
      await pending;
    } catch (error) {
      failure = error;
    }
    assert.ok(failure.lease);
    gate.resolve();
    await failure.lease.dispose();
    assert.equal(f.entries.size, 0);
    const g = fixture();
    let calls = 0;
    const lease = await createAstcJobDirectory(
      options(g, {
        stopAndDrain: () => {
          if (++calls === 1) throw new Error("stop failed");
        },
      }),
    );
    await assert.rejects(lease.dispose(), /CLEANUP_FAILED/);
    assert.ok(!g.log.includes("remove"));
    await lease.dispose();
    assert.equal(calls, 2);
    assert.equal(lease.status.removed, true);
  });

  test("pending shutdown prevents removal and retry does not issue shutdown twice", async () => {
    const f = fixture(),
      gate = deferred();
    let calls = 0;
    const lease = await createAstcJobDirectory(
      options(f, {
        stopAndDrain: () => {
          calls++;
          return gate.promise;
        },
      }),
    );
    await assert.rejects(lease.dispose(), /CLEANUP_FAILED/);
    assert.equal(calls, 1);
    assert.ok(!f.log.includes("remove"));
    const retry = lease.dispose();
    gate.resolve();
    await retry;
    assert.equal(calls, 1);
    assert.equal(lease.status.removed, true);
  });
  test("pre-aborted creation performs no filesystem access and missing owned directory is released", async () => {
    const f = fixture(),
      controller = new AbortController();
    controller.abort();
    await assert.rejects(
      createAstcJobDirectory(options(f, { signal: controller.signal })),
      /abort/i,
    );
    assert.equal(f.log.length, 0);
    const lease = await createAstcJobDirectory(options(f));
    f.entries.clear();
    await lease.dispose();
    assert.equal(lease.status.removed, true);
    assert.ok(!f.log.includes("remove"));
  });
}

// astc-output-validator
{
  // 只验证合成 ASTC 输出清单和元数据；不读取游戏或宣称内容认证。
  // Validate synthetic ASTC inventories and metadata only, without game reads or content-authentication claims.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const {
    validateAstcOutputs,
    validateLegacyPackMetadata,
    validateCascArchiveName,
    astcOutputPaths,
    ASTC_OUTPUT_LIMITS,
  } = astcOutputValidatorModule;
  const { RELEASE_OVERLAY_REVISION, UPSTREAM_OVERLAY_REVISION } = revisionModule;
  const encode = (s) => new TextEncoder().encode(s);
  const pack = { name: "00-00000008.data", size: 3, sha256: "a".repeat(64) };
  const overlayArchives = { base: "00-00000009.data", item_notes: "00-00000010.data" };
  function rawFixture() {
    const files = {};
    for (const [prefix, name, sidecars] of [
      [
        "romfs_overlay_astc",
        overlayArchives.base,
        ["ctrl_touch", "touch_strings.tsv", "pack.json"],
      ],
      [
        "romfs_overlay_astc_variants/item_notes",
        overlayArchives.item_notes,
        ["ctrl_touch", "touch_strings.tsv", "dm_textbox_plain.tsv", "features.txt"],
      ],
    ]) {
      files[`${prefix}/data/.build.config`] = encode("config");
      files[`${prefix}/data/${name}`] = Uint8Array.of(1);
      for (const side of sidecars)
        files[`${prefix}/${side}`] = encode(
          side === "pack.json"
            ? JSON.stringify(pack, null, 1)
            : side === "features.txt"
              ? "item_notes\ndata/example\n"
              : "text",
        );
    }
    return { files, pack, overlayArchives };
  }
  function releaseFixture() {
    const result = { ...rawFixture(), recipeRevision: RELEASE_OVERLAY_REVISION };
    for (const root of ["romfs_overlay_astc", "romfs_overlay_astc_variants/item_notes"]) {
      result.files[`${root}/xbox_glyphs`] = encode(
        "ztouch_controller_buttons_ui/switchx*.sprite\n",
      );
      result.files[`${root}/overlay.manifest`] = encode(
        "DM_OVERLAY_V1 243ab3eec668192415561f806ebbf2e670ea5a8a83aee8b72f92021a23989763\n" +
          Object.keys(result.files)
            .filter((path) => path.startsWith(root + "/"))
            .sort()
            .map(
              (path) =>
                `${createHash("sha256").update(result.files[path]).digest("hex")} ${path.slice(root.length + 1)}\n`,
            )
            .join(""),
      );
    }
    return result;
  }
  const fixture = releaseFixture;
  test("accepts exactly native ASTC base plus item_notes while leaving caller bytes unchanged", () => {
    const options = fixture(),
      before = structuredClone(options);
    const result = validateAstcOutputs(options);
    assert.equal(result.paths.length, 15);
    assert.equal(result.recipeRevision, RELEASE_OVERLAY_REVISION);
    assert.deepEqual(result.pack, pack);
    assert.ok(Object.isFrozen(result) && Object.isFrozen(result.paths));
    assert.deepEqual(options, before);
  });
  test("b811 accepts only its exact fifteen-file Xbox closure and source-bound manifests", () => {
    const options = releaseFixture(),
      before = structuredClone(options);
    const result = validateAstcOutputs(options);
    assert.equal(result.paths.length, 15);
    assert.equal(result.recipeRevision, RELEASE_OVERLAY_REVISION);
    assert.deepEqual(result.paths, astcOutputPaths(options));
    assert.deepEqual(options, before);
    assert.ok(result.paths.every((path) => !path.endsWith("/ps_glyphs")));
    assert.throws(() => validateAstcOutputs({ ...options, recipeRevision: "v1.0.18" }));
    assert.throws(() =>
      validateAstcOutputs({ ...rawFixture(), recipeRevision: RELEASE_OVERLAY_REVISION }),
    );
    for (const revision of [null, "v2.0.0", UPSTREAM_OVERLAY_REVISION, {}, "__proto__"])
      assert.throws(() => validateAstcOutputs({ ...options, recipeRevision: revision }));
    for (const path of Object.keys(options.files)) {
      const incomplete = releaseFixture();
      delete incomplete.files[path];
      assert.throws(() => validateAstcOutputs(incomplete));
    }
    const extra = releaseFixture();
    extra.files["romfs_overlay_astc/ps_glyphs"] = encode("not supported");
    assert.throws(() => validateAstcOutputs(extra));
  });
  test("b811 marker, manifest prefix, digest fields and exact sorted members reject drift", () => {
    for (const root of ["romfs_overlay_astc", "romfs_overlay_astc_variants/item_notes"]) {
      for (const marker of [
        "",
        "xbox\n",
        "ztouch_controller_buttons_ui/switchx*.sprite",
        "\uFEFFztouch_controller_buttons_ui/switchx*.sprite\n",
        "x".repeat(65),
      ]) {
        const options = releaseFixture();
        options.files[`${root}/xbox_glyphs`] = encode(marker);
        assert.throws(() => validateAstcOutputs(options));
      }
      for (const mutate of [
        (text) => text.replace("DM_OVERLAY_V1", "DM_OVERLAY_V2"),
        (text) => text.replace("243ab3ee", "043ab3ee"),
        (text) => text.replace(/\n[a-f0-9]/, "\ng"),
        (text) => text.replace(" ctrl_touch\n", " ps_glyphs\n"),
        (text) => text.replace(" ctrl_touch\n", " ../ctrl_touch\n"),
        (text) =>
          text
            .split("\n")
            .filter((line) => !line.endsWith(" ctrl_touch"))
            .join("\n"),
        (text) => text + text.split("\n")[1] + "\n",
        (text) => "\uFEFF" + text,
        (text) => text.replaceAll("\n", "\r\n"),
        (text) => text.slice(0, -1),
        () => "x".repeat(ASTC_OUTPUT_LIMITS.manifestBytes + 1),
      ]) {
        const options = releaseFixture(),
          path = `${root}/overlay.manifest`;
        options.files[path] = encode(mutate(new TextDecoder().decode(options.files[path])));
        assert.throws(() => validateAstcOutputs(options));
      }
    }
  });
  test("missing/extra/default BC/external data and index paths reject", () => {
    for (const extra of [
      "pack_index.tsv",
      `data/${pack.name}`,
      `romfs_overlay_astc/data/${pack.name}`,
      "romfs_overlay/data/.build.config",
      "romfs_overlay_astc_variants/other/features.txt",
      "romfs_overlay_astc/../escape",
    ]) {
      const options = fixture();
      options.files[extra] = Uint8Array.of(1);
      assert.throws(() => validateAstcOutputs(options), /ASTC_OUTPUT_INVALID/);
    }
    for (const path of Object.keys(fixture().files)) {
      const options = fixture();
      delete options.files[path];
      assert.throws(() => validateAstcOutputs(options), /ASTC_OUTPUT_INVALID/);
    }
  });
  test("pack metadata is exactly bound and ambiguous JSON is rejected", () => {
    const jsons = [
      JSON.stringify({ ...pack, size: 4 }),
      JSON.stringify({ ...pack, sha256: "b".repeat(64) }),
      JSON.stringify({ ...pack, extra: 1 }),
      JSON.stringify(pack).replace('"size":3', '"size":3,"size":3'),
      JSON.stringify(pack).replace('"size":3', '"size":3,"\\u0073ize":3'),
      " ".repeat(513),
      "not json",
    ];
    for (const json of jsons) {
      const options = fixture();
      options.files["romfs_overlay_astc/pack.json"] = encode(json);
      assert.throws(() => validateAstcOutputs(options));
    }
  });
  test("names, size limits, external archive alias and variant feature identity fail closed", () => {
    for (const name of [
      "../a",
      "0-00000008.data",
      "00-00000256.data",
      "256-00000008.data",
      "00-00000008.DATA",
      "000-00000008.data",
    ])
      assert.throws(() => validateCascArchiveName(name));
    assert.equal(validateCascArchiveName("255-00000255.data"), "255-00000255.data");
    for (const size of [-1, 0.1, NaN, Infinity, 2 ** 40 + 1])
      assert.throws(() => validateLegacyPackMetadata({ ...pack, size }));
    const options = fixture();
    options.overlayArchives = { ...overlayArchives, base: pack.name };
    assert.throws(() => validateAstcOutputs(options));
    const other = fixture();
    other.files["romfs_overlay_astc_variants/item_notes/features.txt"] = encode("other\n");
    assert.throws(() => validateAstcOutputs(other));
  });
  test("empty archive and shared/resizable byte buffers reject", () => {
    for (const bytes of [
      new Uint8Array(),
      new Uint8Array(new SharedArrayBuffer(1)),
      new Uint8Array(new ArrayBuffer(1, { maxByteLength: 2 })),
    ]) {
      const options = fixture();
      options.files[`romfs_overlay_astc/data/${overlayArchives.base}`] = bytes;
      assert.throws(() => validateAstcOutputs(options));
    }
  });
}

// astc-pack-jobs
{
  // 合成内核输入及编码器替身；不宣称它们具有提取认证来源。
  // Synthetic core inputs and encoder doubles; they do not claim authenticated extraction provenance.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { fixture: extractionFixture, open, selection } = extractionAdapterFixtureModule;
  const { createHash } = nodeCryptoModule;
  const { createAstcPackJobsCore, createAuthenticatedAstcPackJobs } = astcPackJobsModule;
  const { readAuthenticatedCascVfs } = cascOverlayWriterModule;
  const md5 = (b) => createHash("md5").update(b).digest("hex");
  function texture(format) {
    const b = new Uint8Array(64);
    b[4] = format;
    new DataView(b.buffer).setUint32(40, 8, true);
    return b;
  }
  function fixture({
    signal,
    encode = async ({ width, height }) => new Uint8Array(width * height).buffer,
    dependencies = {},
  } = {}) {
    const payloads = [
      texture(0x3d),
      texture(2),
      texture(0x3e),
      new TextEncoder().encode(
        JSON.stringify([
          { filename: "data/a.texture", format: 61 },
          { filename: "data/c.texture", format: 62 },
        ]),
      ),
    ];
    const paths = [
      "data/a.texture",
      "data/b.texture",
      "data/c.texture",
      "data/hd/texture_desc_cache.json",
    ];
    const tokens = paths.map((path) => ({ path }));
    const records = payloads.map((b, i) => ({
      offset: BigInt(i * 1000),
      encodedSize: b.length,
      decodedSize: b.length,
      encoding: "n",
      ckey: md5(b),
      ekey: md5(b).slice(0, 18) + "0000" + (i * 1000).toString(16).padStart(10, "0"),
    }));
    const vfs = new Uint8Array(46 + 25 * 4 + 2),
      v = new DataView(vfs.buffer);
    vfs.set([84, 86, 70, 83, 1, 46, 16, 0]);
    v.setUint32(8, 10);
    v.setUint32(12, 46);
    v.setUint32(20, 46);
    v.setUint32(28, 46);
    v.setUint32(32, 100);
    v.setUint32(38, 146);
    v.setUint32(42, 2);
    vfs[146] = 110;
    records.forEach((r, i) => {
      const p = 46 + i * 25;
      vfs.set(Buffer.from(r.ekey, "hex"), p);
      v.setUint32(p + 16, r.encodedSize);
      v.setUint32(p + 21, r.decodedSize);
    });
    const state = { created: 0, disposed: 0, active: 0, peak: 0, opens: [] };
    const createStage = async () => {
      state.created++;
      state.active++;
      state.peak = Math.max(state.peak, state.active);
      let parts = [],
        data,
        disposed = false;
      return {
        async append(b) {
          parts.push(b.slice());
        },
        async finalize() {
          data = Buffer.concat(parts);
          parts = [];
          return {
            file: new Blob([data]),
            size: data.length,
            async readAt(at, count) {
              return new Uint8Array(data.subarray(at, at + count));
            },
          };
        },
        async dispose() {
          if (!disposed) {
            disposed = true;
            state.disposed++;
            state.active--;
            data?.fill(0);
            parts = [];
          }
        },
      };
    };
    const header = Buffer.alloc(36),
      block = Buffer.concat([Buffer.from("N"), vfs]);
    header.write("BLTE");
    header.writeUInt32BE(36, 4);
    header[8] = 15;
    header[11] = 1;
    header.writeUInt32BE(block.length, 12);
    header.writeUInt32BE(vfs.length, 16);
    Buffer.from(md5(block), "hex").copy(header, 20);
    const encoded = Buffer.concat([header, block]),
      archive = Buffer.alloc(4000 + encoded.length);
    payloads.forEach((b, i) => archive.set(b, i * 1000));
    archive.set(encoded, 4000);
    const vfsKey = md5(header).slice(0, 18) + "0000" + (4000).toString(16).padStart(10, "0");
    const config = `vfs-1 = ${md5(vfs)} ${vfsKey}\nvfs-1-size = ${vfs.length} ${encoded.length}\n`;
    const manifest =
      "header\n" +
      records
        .map((r, i) =>
          [
            "0",
            "0",
            "0",
            "0",
            r.ckey,
            r.decodedSize,
            r.ekey,
            r.encodedSize,
            "n",
            `data:${paths[i]}`,
            "0",
          ].join("|"),
        )
        .join("\n") +
      "\n";
    const files = {
      "build_manifest.psv": Buffer.from(manifest),
      ".build.config": Buffer.from(config),
      "00-00000000.data": archive,
    };
    const core = createAstcPackJobsCore(
      {
        selected: { textures: tokens.slice(0, 3), descriptor: tokens[3] },
        vfs,
        openRecord(token) {
          const i = tokens.indexOf(token);
          state.opens.push(i);
          return {
            record: records[i],
            source: {
              size: records[i].offset + BigInt(payloads[i].length),
              async readAt(at, count) {
                return payloads[i].slice(
                  Number(at - records[i].offset),
                  Number(at - records[i].offset) + count,
                );
              },
            },
          };
        },
        root: {},
        maxEncodedBytes: 10000,
        maxDecodedBytes: 10000,
        batchBlocks: 1,
        encode,
        signal,
        createStage,
      },
      dependencies,
    );
    return { core, state, files, createStage, encode };
  }
  async function consume(source) {
    const parts = [];
    for await (const bytes of source) parts.push(bytes.slice());
    return Buffer.concat(parts);
  }
  test("one verified stage at a time, skip once, descriptor last with converted paths", async () => {
    const { core, state } = fixture(),
      jobs = [];
    for await (const job of core.jobs) {
      jobs.push({ path: job.path, bytes: await consume(job.source) });
      assert.equal(state.active, 0);
    }
    assert.equal(core.maxEntries, 4);
    assert.equal(jobs.length, 3);
    assert.deepEqual(state.opens, [0, 1, 2, 3]);
    assert.equal(state.created, 4);
    assert.equal(state.disposed, 4);
    assert.equal(state.peak, 1);
    assert.equal(jobs[0].bytes[4], 95);
    assert.equal(jobs[1].bytes[4], 81);
    assert.deepEqual(
      JSON.parse(jobs[2].bytes).map((v) => v.format),
      [95, 81],
    );
  });
  test("outer return, unconsumed advance, and paused abort dispose current stage", async () => {
    for (const mode of ["return", "advance", "abort"]) {
      const controller = new AbortController(),
        { core, state } = fixture({ signal: controller.signal }),
        it = core.jobs[Symbol.asyncIterator]();
      await it.next();
      assert.equal(state.active, 1);
      if (mode === "return") await it.return();
      if (mode === "advance") await assert.rejects(it.next(), /SOURCE_NOT_CONSUMED/);
      if (mode === "abort") {
        controller.abort();
        await new Promise((resolve) => setTimeout(resolve, 0));
        assert.equal(state.active, 0);
        await it.return();
      }
      assert.equal(state.disposed, 1);
    }
  });
  test("conversion failure retains primary error and cleans staged bytes", async () => {
    const { core, state } = fixture({
        encode: async () => {
          throw new Error("synthetic encoder failure");
        },
      }),
      it = core.jobs[Symbol.asyncIterator](),
      job = (await it.next()).value;
    await assert.rejects(consume(job.source), /conversion failed/);
    await it.return();
    assert.equal(state.active, 0);
  });
  test("unstarted disposal and fake extraction identities cannot open production facade", async () => {
    const { core, state } = fixture();
    await core.dispose();
    await assert.rejects(core.jobs[Symbol.asyncIterator]().next(), /abort/i);
    assert.equal(state.created, 0);
    await assert.rejects(
      createAuthenticatedAstcPackJobs(
        {},
        {},
        {
          root: {},
          maxEncodedBytes: 100,
          maxDecodedBytes: 100,
          batchBlocks: 1,
          encode: async () => {},
        },
      ),
      /PROVENANCE/,
    );
    await assert.rejects(readAuthenticatedCascVfs({}, {}), /PROVENANCE/);
  });

  function romfsImage(files) {
    const entries = Object.entries(files),
      names = entries.map(([name]) => Buffer.from(name)),
      lengths = names.map((b) => 32 + Math.ceil(b.length / 4) * 4);
    const tableSize = lengths.reduce((a, b) => a + b, 0),
      dataStart = 140 + tableSize;
    const bytes = new Uint8Array(dataStart + entries.reduce((n, [, b]) => n + b.length, 0)),
      view = new DataView(bytes.buffer);
    const u32 = (p, n) => view.setUint32(p, n, true),
      u64 = (p, n) => view.setBigUint64(p, BigInt(n), true);
    [80, 80, 4, 84, 52, 136, 4, 140, tableSize, dataStart].forEach((n, i) => u64(i * 8, n));
    [0, 0xffffffff, 24, 0xffffffff, 0xffffffff, 0].forEach((n, i) => u32(84 + i * 4, n));
    [0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 4].forEach((n, i) => u32(108 + i * 4, n));
    bytes.set(Buffer.from("data"), 132);
    let table = 0,
      data = 0;
    entries.forEach(([, body], i) => {
      const p = 140 + table,
        next = i === entries.length - 1 ? 0xffffffff : table + lengths[i];
      u32(p, 24);
      u32(p + 4, next);
      u64(p + 8, data);
      u64(p + 16, body.length);
      u32(p + 24, next);
      u32(p + 28, names[i].length);
      bytes.set(names[i], p + 32);
      bytes.set(body, dataStart + data);
      table += lengths[i];
      data += body.length;
    });
    return bytes;
  }
  test("authenticated facade obtains config/TVFS itself and emits the same synthetic jobs", async () => {
    const f = fixture();
    await f.core.dispose();
    const extraction = extractionFixture({ romfs: romfsImage(f.files) }),
      session = await open(extraction);
    const extracted = await extraction.adapter.extractSelected({ session, selection });
    const producer = await createAuthenticatedAstcPackJobs(extracted, selection, {
      root: {},
      maxEncodedBytes: 10000,
      maxDecodedBytes: 10000,
      batchBlocks: 1,
      encode: f.encode,
      createStage: f.createStage,
    });
    const paths = [];
    for await (const job of producer.jobs) {
      await consume(job.source);
      paths.push(job.path);
    }
    assert.deepEqual(paths, [
      "data/a.texture",
      "data/c.texture",
      "data/hd/texture_desc_cache.json",
    ]);
    assert.equal(f.state.peak, 1);
    assert.equal(f.state.disposed, 4);
    await producer.dispose();
    await extraction.adapter.cleanup();
  });

  // 兼容重导出必须指向同一内核，而非包装或复制父级认证实现。
  // Compatibility reexports must identify the same core, not wrap or duplicate parent authentication.
  test("facade reexports the standalone core and cleanup limit unchanged", async () => {
    const facade = await import("../astc-pack-jobs.mjs");
    const core = await import("../astc-pack-jobs-core.mjs");
    assert.equal(facade.createAstcPackJobsCore, core.createAstcPackJobsCore);
    assert.equal(facade.ASTC_JOB_CLEANUP_WAIT_MS, core.ASTC_JOB_CLEANUP_WAIT_MS);
  });
}

// astc-producer
{
  // 合成工作线程及目录验证生命周期；不把编码器替身冒充真实 WASM 或浏览器证据。
  // Synthetic workers/directories verify lifecycle; codec doubles are not real WASM or browser evidence.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { createAstcProducerClient, bindAstcRecordSource, validateAstcProducerReceipt } =
    astcProducerClientModule;
  const { createAstcProducerWorkerHandler, astcProducerFailureCode } = astcProducerWorkerModule;
  const { createArchiveReadRpcClient } = archiveReadRpcModule;
  const { writeCascPack } = cascPackWriterModule;
  const { fixture: extractionFixture, open, selection } = extractionAdapterFixtureModule;
  const path = "data/hd/texture_desc_cache.json",
    packName = "00-00000002.data";
  const md5 = (b) => createHash("md5").update(b).digest("hex");
  const assets = () =>
    ["astc-encoder.mjs", "astc-encoder.wasm", "png-codec.wasm", "sha256.wasm"].map((name) => [
      name,
      new ArrayBuffer(1),
    ]);
  const tick = () => new Promise((resolve) => setImmediate(resolve));
  class WorkerDouble {
    listeners = new Map();
    messages = [];
    terminated = false;
    constructor(run) {
      this.run = run;
    }
    addEventListener(k, f) {
      if (!this.listeners.has(k)) this.listeners.set(k, new Set());
      this.listeners.get(k).add(f);
    }
    removeEventListener(k, f) {
      this.listeners.get(k)?.delete(f);
    }
    terminate() {
      this.terminated = true;
    }
    emit(data) {
      for (const f of this.listeners.get("message") ?? []) f({ data });
    }
    postMessage(data, transfers) {
      // 普通对象目录仅作测试替身；真实浏览器通过结构化克隆传递 FileSystemDirectoryHandle。
      // Plain directory objects are test doubles; real browsers structured-clone FileSystemDirectoryHandle.
      const root = data.root;
      const copy = structuredClone({ ...data, root: undefined }, { transfer: transfers });
      copy.root = root;
      this.messages.push(copy);
      queueMicrotask(() => this.run(copy, this));
    }
  }
  const proof = { type: "initialized", version: 1, astc: true, png: true, sha: true };
  function receipt(bytes = Uint8Array.of(91, 93)) {
    const key = md5(bytes).slice(0, 18) + "0002" + "0".repeat(10);
    return {
      type: "complete",
      preset: "thorough",
      file: new File([bytes], "pack"),
      pack: {
        name: packName,
        size: bytes.length,
        sha256: createHash("sha256").update(bytes).digest("hex"),
      },
      records: [{ path, newEkey: key, encodedSize: bytes.length, decodedSize: bytes.length }],
      count: 1,
      index: `# pack\t${packName}\n${path}\t${key}\t${bytes.length}\t${bytes.length}\n`,
    };
  }
  function directory() {
    const entries = new Map();
    let removes = 0;
    return {
      entries,
      get removes() {
        return removes;
      },
      root: {
        kind: "directory",
        async getDirectoryHandle(name, options) {
          if (options?.create) {
            const child = { kind: "directory", isSameEntry: (other) => other === child };
            entries.set(name, child);
            return child;
          }
          if (!entries.has(name)) throw new DOMException("missing", "NotFoundError");
          return entries.get(name);
        },
        async removeEntry(name) {
          removes++;
          entries.delete(name);
        },
      },
    };
  }
  function romfsImage(files) {
    const entries = Object.entries(files),
      names = entries.map(([n]) => Buffer.from(n)),
      lengths = names.map((n) => 32 + Math.ceil(n.length / 4) * 4);
    const tableSize = lengths.reduce((a, b) => a + b, 0),
      start = 140 + tableSize,
      bytes = new Uint8Array(start + entries.reduce((n, [, b]) => n + b.length, 0)),
      v = new DataView(bytes.buffer);
    const u32 = (p, n) => v.setUint32(p, n, true),
      u64 = (p, n) => v.setBigUint64(p, BigInt(n), true);
    [80, 80, 4, 84, 52, 136, 4, 140, tableSize, start].forEach((n, i) => u64(i * 8, n));
    [0, 0xffffffff, 24, 0xffffffff, 0xffffffff, 0].forEach((n, i) => u32(84 + i * 4, n));
    [0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 4].forEach((n, i) => u32(108 + i * 4, n));
    bytes.set(Buffer.from("data"), 132);
    let table = 0,
      data = 0;
    entries.forEach(([, body], i) => {
      const p = 140 + table,
        next = i === entries.length - 1 ? 0xffffffff : table + lengths[i];
      u32(p, 24);
      u32(p + 4, next);
      u64(p + 8, data);
      u64(p + 16, body.length);
      u32(p + 24, next);
      u32(p + 28, names[i].length);
      bytes.set(names[i], p + 32);
      bytes.set(body, start + data);
      table += lengths[i];
      data += body.length;
    });
    return bytes;
  }
  async function authenticated() {
    const body = Buffer.from("[]"),
      vfs = Buffer.from("synthetic-vfs"),
      block = Buffer.concat([Buffer.from("N"), vfs]),
      header = Buffer.alloc(36);
    header.write("BLTE");
    header.writeUInt32BE(36, 4);
    header[8] = 15;
    header[11] = 1;
    header.writeUInt32BE(block.length, 12);
    header.writeUInt32BE(vfs.length, 16);
    Buffer.from(md5(block), "hex").copy(header, 20);
    const encoded = Buffer.concat([header, block]),
      archive = Buffer.alloc(100 + encoded.length);
    archive.set(body);
    archive.set(encoded, 100);
    const ekey = md5(body).slice(0, 18) + "0".repeat(14),
      vkey = md5(header).slice(0, 18) + "0000" + (100).toString(16).padStart(10, "0");
    const row = ["0", "0", "0", "0", md5(body), 2, ekey, 2, "n", `data:${path}`, "0"].join("|");
    const config = `vfs-1 = ${md5(vfs)} ${vkey}\nvfs-1-size = ${vfs.length} ${encoded.length}\n`;
    const fixture = extractionFixture({
      romfs: romfsImage({
        "build_manifest.psv": Buffer.from(`header\n${row}\n`),
        ".build.config": Buffer.from(config),
        "00-00000000.data": archive,
      }),
    });
    const session = await open(fixture),
      extracted = await fixture.adapter.extractSelected({ session, selection });
    return { fixture, extracted };
  }
  const options = (root) => ({
    root,
    maxEncodedBytes: 10000,
    maxDecodedBytes: 10000,
    maxPackBytes: 10000,
    batchBlocks: 1,
  });
  test("verified initialization transfers four assets and rejects forged extraction before private work", async () => {
    const bytes = assets(),
      worker = new WorkerDouble((data, w) => {
        assert.equal(data.type, "initialize");
        w.emit(proof);
      });
    const client = await createAstcProducerClient({ worker, assets: bytes });
    assert.equal(client.initialized, true);
    assert.ok(bytes.every(([, b]) => b.byteLength === 0));
    await assert.rejects(
      client.produce({}, selection, options(directory().root)),
      /ASTC_PRODUCER_FAILED/,
    );
    assert.equal(worker.messages.length, 1);
    assert.ok(worker.terminated);
    await client.close();
  });
  test("range wrapper blocks reads outside authenticated record despite larger archive", async () => {
    let reads = 0;
    const bound = bindAstcRecordSource({
      source: {
        size: 1000n,
        readAt: (_at, n) => {
          reads++;
          return new Uint8Array(n);
        },
      },
      record: { offset: 100n, encodedSize: 10 },
    });
    assert.equal(bound.size, 1000n);
    await assert.rejects(bound.readAt(99n, 1), /READ_BOUNDS/);
    await assert.rejects(bound.readAt(109n, 2), /READ_BOUNDS/);
    await bound.readAt(100n, 10);
    assert.equal(reads, 1);
  });
  test("authenticated producer binds RPC and retains output lease until disposal", async () => {
    const auth = await authenticated(),
      dir = directory();
    let rpc;
    const worker = new WorkerDouble(async (data, w) => {
      if (data.type === "initialize") {
        w.emit(proof);
        return;
      }
      assert.equal(data.packName, packName);
      assert.equal(data.records.length, 1);
      assert.deepEqual(new TextDecoder().decode(data.vfs), "synthetic-vfs");
      rpc = createArchiveReadRpcClient(data.port);
      const source = rpc.open(data.records[0].source);
      const bytes = await source.readAt(0n, 2);
      assert.deepEqual([...bytes], [91, 93]);
      w.emit(receipt(bytes));
    });
    const client = await createAstcProducerClient({ worker, assets: assets() });
    const result = await client.produce(auth.extracted, selection, options(dir.root));
    assert.ok(worker.terminated);
    assert.equal(dir.entries.size, 1);
    assert.equal(result.count, 1);
    assert.deepEqual(result.encoding, { format: "astc", preset: "thorough" });
    assert.ok(Object.isFrozen(result.encoding));
    assert.deepEqual([...(await result.source.readAt(0, 2))], [91, 93]);
    await result.dispose();
    assert.equal(dir.removes, 1);
    await assert.rejects(result.source.readAt(0, 1), /DISPOSED/);
    await client.close();
    assert.equal(dir.removes, 1);
    rpc.close();
    await auth.fixture.adapter.cleanup();
  });
  test("real pack writer index header and metadata pass receipt validation", async () => {
    let chunks = [],
      size = 0;
    const output = await writeCascPack({
      jobs: (async function* () {
        yield {
          path,
          spec: "n",
          decodedSize: 2,
          source: (async function* () {
            yield Uint8Array.of(91, 93);
          })(),
        };
      })(),
      location: [0, 2],
      locationAvailable: () => true,
      root: {},
      maxPackBytes: 1000,
      expectedEntries: 1,
      codec: {
        genericZlibVersion: "1.2.13",
        compressZlibStream: () => {
          assert.fail();
        },
      },
      hashFile: async (file) => ({
        sha256: createHash("sha256")
          .update(new Uint8Array(await file.arrayBuffer()))
          .digest("hex"),
        bytesHashed: file.size,
      }),
      createStage: async () => ({
        append(bytes) {
          chunks.push(bytes);
          size += bytes.length;
        },
        async finalize() {
          const file = new File(chunks, "pack");
          return { file, size, readAt() {} };
        },
        dispose() {
          chunks = [];
        },
      }),
    });
    const value = {
      type: "complete",
      preset: "thorough",
      file: output.source.file,
      pack: output.pack,
      records: output.records,
      index: output.index,
      count: output.count,
    };
    assert.equal(
      validateAstcProducerReceipt(value, { paths: new Set([path]), packName, maxPackBytes: 1000 })
        .count,
      1,
    );
    for (const bad of [
      { ...value, index: value.index.replace(/^.*\n/, "") },
      { ...value, pack: { ...value.pack, name: "00-00000003.data" } },
      { ...value, count: 2 },
    ])
      assert.throws(
        () =>
          validateAstcProducerReceipt(bad, {
            paths: new Set([path]),
            packName,
            maxPackBytes: 1000,
          }),
        /RECEIPT/,
      );
  });
  test("worker requires initialization and rejects concurrent state transitions", async () => {
    const messages = [];
    let finish;
    const handler = createAstcProducerWorkerHandler({
      send: (data) => messages.push(data),
      initialize: () =>
        new Promise((resolve) => {
          finish = resolve;
        }),
    });
    const initializing = handler({ type: "initialize", assets: [] });
    await tick();
    await handler({ type: "produce" });
    finish({});
    await initializing;
    assert.ok(messages.every((message) => message.type === "failure"));
  });
  test("malformed initialization and abort terminate without accepting private input", async () => {
    const worker = new WorkerDouble((_data, w) => w.emit({ ...proof, png: false }));
    await assert.rejects(createAstcProducerClient({ worker, assets: assets() }), /INITIALIZE/);
    assert.ok(worker.terminated);
    const controller = new AbortController(),
      waiting = new WorkerDouble(() => {});
    const pending = createAstcProducerClient({
      worker: waiting,
      assets: assets(),
      signal: controller.signal,
    });
    controller.abort();
    await assert.rejects(pending, { name: "AbortError" });
    assert.ok(waiting.terminated);
  });

  // 只有严格递增的真实工作收据续期；静默、重复进度和显式总期限均会停止作业。
  // Only strictly advancing work receipts renew inactivity; silence, duplicate progress and explicit overall deadlines stop jobs.
  test("validated progress renews inactivity without a silent overall deadline", async () => {
    const auth = await authenticated(),
      dir = directory();
    const timers = [],
      progress = [];
    const worker = new WorkerDouble((data, w) => {
      if (data.type === "initialize") {
        w.emit(proof);
        return;
      }
      for (let i = 1; i <= 6; i++)
        timers.push(
          setTimeout(() => w.emit({ type: "progress", sequence: i, bytes: i, blocks: 0 }), i * 20),
        );
      timers.push(setTimeout(() => w.emit(receipt()), 145));
    });
    const client = await createAstcProducerClient({ worker, assets: assets() });
    try {
      const result = await client.produce(auth.extracted, selection, {
        ...options(dir.root),
        inactivityTimeoutMs: 45,
        onProgress(value) {
          progress.push({ value, frozen: Object.isFrozen(value), at: performance.now() });
          if (progress.length === 1) throw new Error("observer");
          return Promise.reject(new Error("observer async"));
        },
      });
      assert.equal(result.count, 1);
      assert.ok(
        progress.every(
          (event) =>
            event.frozen &&
            !Object.hasOwn(event.value, "path") &&
            !Object.hasOwn(event.value, "ekey"),
        ),
      );
      assert.equal(progress[0].value.phase, "preparing");
      assert.equal(progress.at(-1).value.phase, "complete");
      const updates = progress.filter((event) => event.value.phase === "converting");
      assert.ok(updates.length >= 1);
      for (let i = 1; i < updates.length; i++) assert.ok(updates[i].at - updates[i - 1].at >= 99);
      await result.dispose();
    } finally {
      for (const timer of timers) clearTimeout(timer);
      await client.close();
      await auth.fixture.adapter.cleanup();
    }
  });
  test("stale progress, inactivity and explicit overall deadline terminate and clean", async () => {
    for (const mode of ["stale", "idle", "overall"]) {
      const auth = await authenticated(),
        dir = directory(),
        timers = [];
      const worker = new WorkerDouble((data, w) => {
        if (data.type === "initialize") {
          w.emit(proof);
          return;
        }
        if (mode === "stale") {
          w.emit({ type: "progress", sequence: 1, bytes: 1, blocks: 0 });
          w.emit({ type: "progress", sequence: 2, bytes: 1, blocks: 0 });
        } else if (mode === "overall")
          for (let i = 1; i <= 5; i++)
            timers.push(
              setTimeout(
                () => w.emit({ type: "progress", sequence: i, bytes: i, blocks: 0 }),
                i * 10,
              ),
            );
      });
      const client = await createAstcProducerClient({ worker, assets: assets() });
      try {
        await assert.rejects(
          client.produce(auth.extracted, selection, {
            ...options(dir.root),
            inactivityTimeoutMs: 40,
            ...(mode === "overall" ? { overallTimeoutMs: 25 } : {}),
          }),
          /ASTC_PRODUCER_FAILED/,
        );
        assert.ok(worker.terminated);
        assert.equal(dir.entries.size, 0);
      } finally {
        for (const timer of timers) clearTimeout(timer);
        await client.close();
        await auth.fixture.adapter.cleanup();
      }
    }
  });
  test("failed directory removal retains retryable lease and safe failure codes", async () => {
    const auth = await authenticated(),
      dir = directory(),
      remove = dir.root.removeEntry;
    let deny = true;
    dir.root.removeEntry = async (name) => {
      if (deny) throw new Error("private storage name");
      return remove(name);
    };
    const worker = new WorkerDouble((data, w) =>
      w.emit(data.type === "initialize" ? proof : { type: "failure", code: "CODEC_FAILED" }),
    );
    const client = await createAstcProducerClient({ worker, assets: assets() });
    let failure;
    try {
      await client.produce(auth.extracted, selection, options(dir.root));
    } catch (error) {
      failure = error;
    }
    assert.match(failure.message, /ASTC_PRODUCER_CLEANUP/);
    assert.ok(failure.lease);
    assert.ok(!failure.message.includes("private"));
    assert.equal(dir.entries.size, 1);
    deny = false;
    await client.close();
    assert.equal(dir.entries.size, 0);
    await auth.fixture.adapter.cleanup();
  });

  for (const preset of ["thorough", "fast"])
    test(`worker shares ${preset} across texture and thumbnail encoding and echoes receipt`, async () => {
      const messages = [],
        channel = new MessageChannel();
      const handler = createAstcProducerWorkerHandler({
        send: (data) => messages.push(data),
        initialize: async () => ({
          encode: (input) => {
            assert.equal(input.preset, preset);
            return new ArrayBuffer(16);
          },
          codec: {},
          hashFile: async (file) => ({ sha256: "a".repeat(64), bytesHashed: file.size }),
        }),
        core: (options) => ({
          maxEntries: 1,
          dispose: async () => {},
          jobs: (async function* () {
            await options.encode({
              width: 4,
              height: 4,
              rgba: new ArrayBuffer(64),
              profile: "linear",
            });
            await options.encode({
              width: 4,
              height: 4,
              rgba: new ArrayBuffer(64),
              profile: "srgb",
            });
            yield 1;
          })(),
        }),
        writePack: async (options) => {
          for await (const ignored of options.jobs) assert.equal(ignored, 1);
          const value = receipt();
          await options.hashFile(value.file, {});
          return { source: { file: value.file }, ...value };
        },
      });
      await handler({ type: "initialize", assets: [] });
      await handler({
        type: "produce",
        preset,
        port: channel.port1,
        root: { kind: "directory" },
        vfs: new ArrayBuffer(1),
        records: [
          {
            path,
            record: { offset: 0n },
            source: { id: "12345678-1234-4123-8123-123456789abc", size: "2" },
          },
        ],
      });
      assert.equal(messages[0].type, "initialized");
      assert.deepEqual(messages[1], { type: "progress", sequence: 1, bytes: 0, blocks: 1 });
      assert.equal(messages.at(-1).type, "complete");
      assert.ok(messages.at(-1).file instanceof File);
      assert.equal(messages.at(-1).preset, preset);
      channel.port2.close();
    });

  // 安全诊断只允许固定代码，未知错误中的私有文本不会被返回。
  // Safe diagnostics allow only fixed codes; private text in unknown errors is never returned.
  test("safe failure diagnostics whitelist quota, limits and integrity", () => {
    assert.equal(
      astcProducerFailureCode(new DOMException("private/path", "QuotaExceededError")),
      "STORAGE_QUOTA",
    );
    assert.equal(astcProducerFailureCode(new Error("CASC_PACK_PACK_SIZE")), "SIZE_LIMIT");
    assert.equal(astcProducerFailureCode(new Error("ARCHIVE_STREAM_CKEY")), "INTEGRITY_FAILED");
    assert.equal(astcProducerFailureCode(new Error("private/key")), "CODEC_FAILED");
  });

  // 预设只是内部编码选择，不改变外部数据认证边界。
  // Presets select internal encoding only; they do not alter external authentication boundaries.
  test("receipt rejects missing, wrong or unknown presets", () => {
    const value = receipt();
    const limits = { paths: new Set([path]), packName, maxPackBytes: 1000, preset: "fast" };
    assert.deepEqual(validateAstcProducerReceipt({ ...value, preset: "fast" }, limits).encoding, {
      format: "astc",
      preset: "fast",
    });
    for (const preset of [undefined, "thorough", "other", 1])
      assert.throws(() => validateAstcProducerReceipt({ ...value, preset }, limits), /RECEIPT/);
  });
  test("producer rejects unknown preset before authenticating or reading input", async () => {
    const worker = new WorkerDouble((_data, w) => w.emit(proof));
    const client = await createAstcProducerClient({ worker, assets: assets() });
    await assert.rejects(
      client.produce(null, null, { ...options(directory().root), preset: "other" }),
      /OPTIONS/,
    );
    assert.equal(worker.messages.length, 1);
    assert.ok(worker.terminated);
  });
  test("fast producer request and receipt stay bound", async () => {
    const auth = await authenticated(),
      dir = directory();
    const worker = new WorkerDouble((data, w) => {
      if (data.type === "initialize") return w.emit(proof);
      assert.equal(data.preset, "fast");
      w.emit({ ...receipt(), preset: "fast" });
    });
    const client = await createAstcProducerClient({ worker, assets: assets() });
    const result = await client.produce(auth.extracted, selection, {
      ...options(dir.root),
      preset: "fast",
    });
    assert.deepEqual(result.encoding, { format: "astc", preset: "fast" });
    await result.dispose();
    await auth.fixture.adapter.cleanup();
  });

  test("worker rejects unknown preset before opening record RPC or invoking codec", async () => {
    const messages = [];
    let invoked = false;
    const handler = createAstcProducerWorkerHandler({
      send: (value) => messages.push(value),
      initialize: async () => ({
        encode: () => {
          invoked = true;
        },
      }),
      core: () => {
        invoked = true;
      },
    });
    await handler({ type: "initialize", assets: [] });
    await handler({ type: "produce", preset: "other" });
    assert.equal(invoked, false);
    assert.equal(messages.at(-1).type, "failure");
  });

  for (const mode of ["cancel", "dispose", "short"])
    test(`producer wipes an owned File read rejected after fulfillment: ${mode}`, async () => {
      const auth = await authenticated(),
        dir = directory(),
        controller = new AbortController(),
        reason = new Error("synthetic read cancellation");
      const worker = new WorkerDouble((data, w) =>
        w.emit(data.type === "initialize" ? proof : receipt()),
      );
      const client = await createAstcProducerClient({ worker, assets: assets() });
      let captured, closing;
      try {
        const result = await client.produce(auth.extracted, selection, options(dir.root));
        result.source.file.slice = () => ({
          async arrayBuffer() {
            captured = mode === "short" ? Uint8Array.of(41) : Uint8Array.of(41, 42);
            if (mode === "cancel") controller.abort(reason);
            if (mode === "dispose") closing = result.dispose();
            return captured.buffer;
          },
        });
        await assert.rejects(result.source.readAt(0, 2, { signal: controller.signal }), (error) => {
          if (mode === "cancel") assert.equal(error, reason);
          else
            assert.equal(
              error.message,
              `ASTC_PRODUCER_${mode === "dispose" ? "DISPOSED" : "SHORT_READ"}`,
            );
          return true;
        });
        await closing;
        assert.ok(captured.every((byte) => byte === 0));
      } finally {
        await client.close();
        await auth.fixture.adapter.cleanup();
      }
    });

  for (const mode of ["cancel", "timeout"])
    for (const settle of ["resolve", "reject"])
      test(`producer observes a late File ${settle} after ${mode}`, async (t) => {
        const auth = await authenticated(),
          dir = directory(),
          controller = new AbortController();
        const worker = new WorkerDouble((data, w) =>
          w.emit(data.type === "initialize" ? proof : receipt()),
        );
        const client = await createAstcProducerClient({ worker, assets: assets() });
        let resolve, reject;
        const pending = new Promise((yes, no) => {
          resolve = yes;
          reject = no;
        });
        try {
          const result = await client.produce(auth.extracted, selection, options(dir.root));
          result.source.file.slice = () => ({ arrayBuffer: () => pending });
          if (mode === "timeout") t.mock.timers.enable({ apis: ["setTimeout"] });
          const reading = result.source.readAt(0, 2, { signal: controller.signal });
          if (mode === "cancel") controller.abort();
          else t.mock.timers.tick(30000);
          await assert.rejects(
            reading,
            mode === "cancel" ? { name: "AbortError" } : /ASTC_PRODUCER_READ_FAILED/,
          );
          const bytes = Uint8Array.of(41, 42);
          if (settle === "resolve") resolve(bytes.buffer);
          else reject(new Error("synthetic late File failure"));
          await tick();
          if (settle === "resolve") assert.ok(bytes.every((byte) => byte === 0));
        } finally {
          t.mock.timers.reset();
          await client.close();
          await auth.fixture.adapter.cleanup();
        }
      });

  test("producer transfers successful File reads without wiping caller-owned bytes", async () => {
    const auth = await authenticated(),
      dir = directory();
    const worker = new WorkerDouble((data, w) =>
      w.emit(data.type === "initialize" ? proof : receipt()),
    );
    const client = await createAstcProducerClient({ worker, assets: assets() });
    try {
      const result = await client.produce(auth.extracted, selection, options(dir.root));
      const captured = Uint8Array.of(41, 42);
      result.source.file.slice = () => ({ arrayBuffer: async () => captured.buffer });
      const bytes = await result.source.readAt(0, 2);
      assert.equal(bytes.buffer, captured.buffer);
      await result.dispose();
      assert.deepEqual(bytes, Uint8Array.of(41, 42));
    } finally {
      await client.close();
      await auth.fixture.adapter.cleanup();
    }
  });
}

// astc-record-selection
{
  // 纯合成清单与 RomFS；不含游戏内容或真实密钥。
  // Synthetic manifests and RomFS only; no game content or real keys.
  const { default: assert } = strictModule;
  const { default: test } = nodeTestModule;
  const { createCascAstcRecords, createCascLogicalAssets, ASTC_MANIFEST_LIMITS } =
    cascLogicalReaderModule;
  const { enumerateAuthenticatedAstcRecords, openAuthenticatedAstcRecord } =
    extractionJobAdapterModule;
  const { fixture, open, selection } = extractionAdapterFixtureModule;
  const encoder = new TextEncoder();
  const row = (path, offset = 0) =>
    [
      "0",
      "0",
      "0",
      "0",
      "a".repeat(32),
      "1",
      "0".repeat(22) + offset.toString(16).padStart(10, "0"),
      "1",
      "n",
      path,
      "0",
    ].join("|");
  const manifest = (rows) => encoder.encode(`header\n${rows.join("\n")}\n`);
  function source(bytes) {
    const reads = [];
    return {
      reads,
      getFile(path) {
        const data = path === "data/build_manifest.psv" ? bytes : Uint8Array.of(11, 22);
        return {
          size: BigInt(data.length),
          readAt(offset, count) {
            reads.push({ path, count });
            return data.slice(Number(offset), Number(offset) + count);
          },
        };
      },
    };
  }
  function image(bytes) {
    const names = ["build_manifest.psv", "00-00000000.data"].map((v) => encoder.encode(v));
    const lengths = names.map((v) => 32 + Math.ceil(v.length / 4) * 4);
    const fileStart = 140,
      dataStart = fileStart + lengths[0] + lengths[1];
    const result = new Uint8Array(dataStart + bytes.length + 2),
      view = new DataView(result.buffer);
    const u32 = (p, v) => view.setUint32(p, v, true),
      u64 = (p, v) => view.setBigUint64(p, BigInt(v), true);
    [80, 80, 4, 84, 52, 136, 4, 140, lengths[0] + lengths[1], dataStart].forEach((v, i) =>
      u64(i * 8, v),
    );
    [0, 0xffffffff, 24, 0xffffffff, 0xffffffff, 0].forEach((v, i) => u32(84 + i * 4, v));
    [0, 0xffffffff, 0xffffffff, 0, 0xffffffff, 4].forEach((v, i) => u32(108 + i * 4, v));
    result.set(encoder.encode("data"), 132);
    let position = fileStart;
    for (let i = 0; i < 2; i++) {
      u32(position, 24);
      u32(position + 4, i === 0 ? lengths[0] : 0xffffffff);
      u64(position + 8, i === 0 ? 0 : bytes.length);
      u64(position + 16, i === 0 ? bytes.length : 2);
      u32(position + 24, i === 0 ? lengths[0] : 0xffffffff);
      u32(position + 28, names[i].length);
      result.set(names[i], position + 32);
      position += lengths[i];
    }
    result.set(bytes, dataStart);
    result.set([11, 22], dataStart + bytes.length);
    return result;
  }
  test("ASTC first normalized row and descriptor differ from unchanged generic last row", async () => {
    const bytes = manifest([
      row("data:DATA/A.texture"),
      row("data:data/a.texture", 1),
      row("DATA:data/hd/texture_desc_cache.json"),
      row("data:data/hd/texture_desc_cache.json", 1),
    ]);
    const romfs = source(bytes),
      astc = await createCascAstcRecords(romfs),
      generic = await createCascLogicalAssets(romfs);
    assert.deepEqual([...astc.entries()], ["data/a.texture", "data/hd/texture_desc_cache.json"]);
    assert.equal(astc.open("data/a.texture").record.offset, 0n);
    assert.equal(astc.open("data/hd/texture_desc_cache.json").record.offset, 0n);
    assert.ok(generic.lookup("data/a.texture")[6].endsWith("01"));
    const opened = astc.open("data/a.texture");
    assert.deepEqual(await opened.source.readAt(0n, 1), Uint8Array.of(11));
    await assert.rejects(opened.source.readAt(0n, 65537), /extent/);
    await assert.rejects(opened.source.readAt(1n, 1), /extent/);
    assert.ok(romfs.reads.every(({ count }) => count <= 65536));
  });
  test("ASTC scan bounds reads, count, retained metadata, paths and cancellation", async () => {
    const romfs = source(
      manifest(Array.from({ length: 1000 }, (_, i) => row(`data:data/${i}.texture`))),
    );
    assert.equal([...(await createCascAstcRecords(romfs)).entries()].length, 1000);
    assert.ok(romfs.reads.length > 1 && romfs.reads.every(({ count }) => count <= 65536));
    await assert.rejects(
      createCascAstcRecords(
        source(
          manifest(
            Array.from({ length: ASTC_MANIFEST_LIMITS.records + 1 }, (_, i) =>
              row(`data:data/${i}.texture`),
            ),
          ),
        ),
      ),
      /metadata budget/,
    );
    await assert.rejects(
      createCascAstcRecords(
        source(
          manifest(
            Array.from({ length: 2200 }, (_, i) =>
              row(`data:data/${"x".repeat(3900)}${i}.texture`),
            ),
          ),
        ),
      ),
      /metadata budget/,
    );
    await assert.rejects(
      createCascAstcRecords(source(manifest([row("data:data/../a.texture")]))),
      /unsafe/,
    );
    const controller = new AbortController();
    controller.abort();
    await assert.rejects(createCascAstcRecords(romfs, { signal: controller.signal }), /abort/i);
  });
  test("authenticated tokens reject copies, wrong selection, and revoked sessions", async () => {
    const f = fixture({
      romfs: image(
        manifest([
          row("data:data/a.texture"),
          row("data:data/a.texture", 1),
          row("data:data/hd/texture_desc_cache.json"),
        ]),
      ),
    });
    const session = await open(f),
      extracted = await f.adapter.extractSelected({ session, selection });
    await assert.rejects(
      enumerateAuthenticatedAstcRecords({ ...extracted }, selection),
      /PROVENANCE/,
    );
    await assert.rejects(
      enumerateAuthenticatedAstcRecords(extracted, { ...selection, effectiveVersion: 999 }),
      /PROVENANCE/,
    );
    const records = await enumerateAuthenticatedAstcRecords(extracted, selection);
    assert.equal(records.textures.length, 1);
    assert.ok(records.descriptor);
    const token = records.textures[0];
    assert.throws(() => openAuthenticatedAstcRecord({ ...token }), /PROVENANCE/);
    assert.throws(
      () => openAuthenticatedAstcRecord({ path: token.path, authenticated: true }),
      /PROVENANCE/,
    );
    const opened = openAuthenticatedAstcRecord(token);
    assert.deepEqual(await opened.source.readAt(0n, 1), Uint8Array.of(11));
    await session.dispose();
    assert.throws(() => openAuthenticatedAstcRecord(token), /DISPOSED/);
    await assert.rejects(opened.source.readAt(0n, 1), /DISPOSED/);
    await f.adapter.cleanup();
  });
  test("enumeration cancellation revokes retained tokens even without a read", async () => {
    const f = fixture({ romfs: image(manifest([row("data:data/a.texture")])) }),
      session = await open(f);
    const extracted = await f.adapter.extractSelected({ session, selection }),
      controller = new AbortController();
    const { textures } = await enumerateAuthenticatedAstcRecords(extracted, selection, {
      signal: controller.signal,
    });
    const opened = openAuthenticatedAstcRecord(textures[0]);
    controller.abort();
    assert.throws(() => openAuthenticatedAstcRecord(textures[0]), /abort/i);
    await assert.rejects(opened.source.readAt(0n, 1), /abort/i);
    await f.adapter.cleanup();
  });

  test("native ASTC considers the first physical row and rejects malformed locator metadata", async () => {
    const astc = await createCascAstcRecords(source(encoder.encode(row("data:data/a.texture"))));
    assert.deepEqual([...astc.entries()], ["data/a.texture"]);
    for (const [field, value, error] of [
      [1, "1", /key layout/],
      [2, "1", /mismatch/],
      [5, "4294967296", /record size/],
      [7, "3", /extent/],
      [4, "bad", /CKey/],
    ]) {
      const fields = row("data:data/a.texture").split("|");
      fields[field] = value;
      const records = await createCascAstcRecords(source(manifest([fields.join("|")])));
      assert.throws(() => records.open("data/a.texture"), error);
    }
  });

  test("canceling a pending trusted record read closes the extraction session", async () => {
    const f = fixture({ romfs: image(manifest([row("data:data/a.texture")])) }),
      session = await open(f);
    const extracted = await f.adapter.extractSelected({ session, selection });
    const { textures } = await enumerateAuthenticatedAstcRecords(extracted, selection);
    const opened = openAuthenticatedAstcRecord(textures[0]),
      controller = new AbortController();
    f.worker.handler = undefined;
    const pending = opened.source.readAt(0n, 1, { signal: controller.signal });
    controller.abort();
    await assert.rejects(pending, /DISPOSED/);
    assert.equal(f.worker.terminated, true);
    assert.throws(() => openAuthenticatedAstcRecord(textures[0]), /DISPOSED/);
    await f.adapter.cleanup();
  });
}

// production-assets-astc
{
  // 普通测试命令通过独立子进程执行模块替身，不需要更改 CI 标志。
  // The normal test command runs module doubles in an isolated subprocess without changing CI flags.
  const { assertIsolatedRun } = assertIsolatedRunModule;
  const { default: test } = nodeTestModule;
  const { spawnSync } = nodeChild_processModule;
  const { default: process } = nodeProcessModule;
  test("optional ASTC adapter metadata and lifetime contracts", () => {
    const env = { ...process.env };
    delete env.NODE_TEST_CONTEXT;
    const result = ["asset-pipeline-astc", "production-assets-astc"].map((selectedScenario) =>
      spawnSync(
        process.execPath,
        [
          "--experimental-test-module-mocks",
          "--test",
          "--test-reporter=tap",
          new URL("./fixtures/astc-pipeline.mock.mjs", import.meta.url).pathname,
        ],
        {
          env: { ...env, D2R_TEST_SCENARIO: selectedScenario },
          encoding: "utf8",
          timeout: 30000,
          maxBuffer: 1024 * 1024,
        },
      ),
    );
    for (const isolatedResult of result) assertIsolatedRun(isolatedResult);
  });
}

// production-astc-binding
{
  // 用合成条目验证可选 ASTC 契约绑定；不启用界面、不读取游戏或外壳二进制。
  // Verify optional ASTC contract binding with synthetic entries, without enabling UI or reading game/shell binaries.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { createHash } = nodeCryptoModule;
  const { bindProductionAstcEntries, inspectProductionAstcEntries } = productionProfileModule;
  const { selectProductionAstcEntries } = browserPackagerModule;
  const { ASTC_OUTPUT_LIMITS } = astcOutputValidatorModule;
  const { RELEASE_OVERLAY_REVISION, UPSTREAM_OVERLAY_REVISION } = revisionModule;
  const encode = (s) => new TextEncoder().encode(s);
  const binding = {
    pack: { name: "00-00000008.data", size: 123, sha256: "a".repeat(64) },
    overlayArchives: { base: "00-00000009.data", item_notes: "00-00000009.data" },
  };
  function fixture(recipeRevision = RELEASE_OVERLAY_REVISION) {
    const files = {};
    for (const [prefix, archive, sides] of [
      [
        "romfs_overlay_astc",
        binding.overlayArchives.base,
        ["ctrl_touch", "touch_strings.tsv", "pack.json"],
      ],
      [
        "romfs_overlay_astc_variants/item_notes",
        binding.overlayArchives.item_notes,
        ["ctrl_touch", "touch_strings.tsv", "dm_textbox_plain.tsv", "features.txt"],
      ],
    ]) {
      files[`${prefix}/data/.build.config`] = encode("config");
      files[`${prefix}/data/${archive}`] = new Uint8Array(70000);
      for (const side of sides)
        files[`${prefix}/${side}`] = encode(
          side === "pack.json"
            ? JSON.stringify(binding.pack, null, 1)
            : side === "features.txt"
              ? "item_notes\ndata/example\n"
              : "content",
        );
    }
    if (recipeRevision === RELEASE_OVERLAY_REVISION) {
      for (const root of ["romfs_overlay_astc", "romfs_overlay_astc_variants/item_notes"]) {
        files[`${root}/xbox_glyphs`] = encode("ztouch_controller_buttons_ui/switchx*.sprite\n");
        files[`${root}/overlay.manifest`] = encode(
          "DM_OVERLAY_V1 243ab3eec668192415561f806ebbf2e670ea5a8a83aee8b72f92021a23989763\n" +
            Object.keys(files)
              .filter((path) => path.startsWith(root + "/"))
              .sort()
              .map(
                (path) =>
                  `${createHash("sha256").update(files[path]).digest("hex")} ${path.slice(root.length + 1)}\n`,
              )
              .join(""),
        );
      }
    }
    return Object.entries(files).map(([path, bytes]) => ({
      path,
      size: bytes.length,
      mode: 0o100644,
      async *chunks() {
        for (let i = 0; i < bytes.length; i += 65536) yield bytes.slice(i, i + 65536);
      },
    }));
  }
  function profileFixture(profileId) {
    const targetOS = { platform: "ios", minimumVersion: "26.0.0" };
    const recipe = {
      schemaVersion: 1,
      product: "synthetic",
      chain: "test",
      kind: "ios",
      shaderDigest: "0123456789abcdef",
      targetOS,
      requiredPaths: ["romfs_overlay/", "romfs_overlay_variants/"],
      extraFiles: ["dm/pack_info.txt", "dm/content_mode.json"],
    };
    const entries = ["Info.plist", "DmIOS", "dm/shell_info.txt", "dm/shell_manifest.txt"].map(
      (p) => ({ path: `Payload/DmIOS.app/${p}`, size: 1, mode: 0o100644 }),
    );
    return {
      ...(profileId === undefined ? {} : { profileId }),
      trustedShell: { trustedRecipe: recipe },
      shell: {
        entries,
        contract: {
          entries,
          trustedRecipe: recipe,
          shellInfoText:
            "PRODUCT=synthetic\nCHAIN=test\nKIND=ios\nSHADER_SRC=0123456789abcdef\nSPIRV_CROSS=synthetic\n",
          shellManifestText: recipe.requiredPaths.join("\n"),
          observedTargetOS: targetOS,
        },
      },
      close() {},
    };
  }
  test("default selection preserves BC entries and requires no binding", () => {
    const entries = [
      { path: "romfs_overlay/data/00-00000007.data" },
      { path: "romfs_overlay_variants/item_notes/features.txt" },
    ];
    assert.deepEqual(selectProductionAstcEntries(entries), []);
    assert.equal(entries.length, 2);
  });
  test("b811 binding preserves exact closure and recipe identity through final selection", async () => {
    const entries = fixture(RELEASE_OVERLAY_REVISION),
      releaseBinding = { ...binding, recipeRevision: RELEASE_OVERLAY_REVISION },
      original = profileFixture("v2.0.0-ios26");
    assert.equal(selectProductionAstcEntries(entries, releaseBinding).length, 15);
    const result = await bindProductionAstcEntries(original, entries, releaseBinding);
    assert.equal(result.astcOutput.recipeRevision, RELEASE_OVERLAY_REVISION);
    assert.equal(result.astcOutput.paths.length, 15);
    assert.equal(selectProductionAstcEntries(entries, result.astcOutput).length, 15);
    assert.equal(
      result.astcOutput.totalBytes,
      entries.reduce((total, entry) => total + entry.size, 0),
    );
    assert.deepEqual(result.trustedShell.trustedRecipe.extraFiles, [
      ...original.trustedShell.trustedRecipe.extraFiles,
      ...entries.map((entry) => entry.path).sort(),
    ]);
    assert.equal(original.astcOutput, undefined);
    assert.equal(original.trustedShell.trustedRecipe.extraFiles.length, 2);
  });
  test("closed profile and binding revisions cannot cross or opt into later-source ASTC", async () => {
    const releaseBinding = { ...binding, recipeRevision: RELEASE_OVERLAY_REVISION };
    for (const profileId of ["v1.0.18-ios26", "v1.0.18-ios18"])
      await assert.rejects(
        bindProductionAstcEntries(
          profileFixture(profileId),
          fixture(RELEASE_OVERLAY_REVISION),
          releaseBinding,
        ),
        /SHELL_PROFILE_UNKNOWN/,
      );
    for (const profileId of [undefined, "v2.0.0-ios26", "v2.0.0-ios18"])
      await assert.rejects(
        bindProductionAstcEntries(profileFixture(profileId), fixture(), {
          ...binding,
          recipeRevision: "v1.0.18",
        }),
        /RECIPE_BINDING/,
      );
    for (const recipeRevision of [null, "v2", UPSTREAM_OVERLAY_REVISION, {}]) {
      const bad = { ...binding, recipeRevision };
      await assert.rejects(inspectProductionAstcEntries(fixture(RELEASE_OVERLAY_REVISION), bad));
      assert.throws(() => selectProductionAstcEntries(fixture(RELEASE_OVERLAY_REVISION), bad));
    }
    const legacy = await bindProductionAstcEntries(profileFixture(undefined), fixture(), binding);
    assert.equal(legacy.astcOutput.recipeRevision, RELEASE_OVERLAY_REVISION);
  });
  test("b811 selection and streamed metadata reject missing, duplicate, unsupported and oversized members", async () => {
    const releaseBinding = { ...binding, recipeRevision: RELEASE_OVERLAY_REVISION };
    assert.throws(() =>
      selectProductionAstcEntries(fixture(RELEASE_OVERLAY_REVISION), {
        ...binding,
        recipeRevision: "v1.0.18",
      }),
    );
    assert.throws(() => selectProductionAstcEntries(fixture("v1.0.18"), releaseBinding));
    for (const changedPath of ["romfs_overlay_astc/ps_glyphs", "romfs_overlay_astc/xbox_glyphs"]) {
      const entries = fixture(RELEASE_OVERLAY_REVISION);
      entries[0] = { ...entries[0], path: changedPath };
      assert.throws(() => selectProductionAstcEntries(entries, releaseBinding));
      await assert.rejects(inspectProductionAstcEntries(entries, releaseBinding));
    }
    for (const [suffix, size] of [
      ["/xbox_glyphs", ASTC_OUTPUT_LIMITS.markerBytes + 1],
      ["/overlay.manifest", ASTC_OUTPUT_LIMITS.manifestBytes + 1],
    ]) {
      const entries = fixture(RELEASE_OVERLAY_REVISION),
        index = entries.findIndex((entry) => entry.path.endsWith(suffix));
      entries[index] = {
        ...entries[index],
        size,
        chunks() {
          assert.fail("oversized metadata must reject before reading");
        },
      };
      await assert.rejects(inspectProductionAstcEntries(entries, releaseBinding), /METADATA/);
    }
    for (const suffix of ["/xbox_glyphs", "/overlay.manifest"]) {
      const entries = fixture(RELEASE_OVERLAY_REVISION),
        index = entries.findIndex((entry) => entry.path.endsWith(suffix)),
        data = encode("wrong\n");
      entries[index] = {
        ...entries[index],
        size: data.length,
        async *chunks() {
          yield data;
        },
      };
      await assert.rejects(inspectProductionAstcEntries(entries, releaseBinding));
    }
  });
  test("b811 streams still enforce modes, complete sizes, late errors and cancellation", async () => {
    const releaseBinding = { ...binding, recipeRevision: RELEASE_OVERLAY_REVISION };
    for (const change of [
      (entry) => ({ ...entry, mode: 0o100755 }),
      (entry) => ({ ...entry, size: entry.size + 1 }),
      (entry) => ({
        ...entry,
        async *chunks() {
          yield new Uint8Array(entry.size);
          throw new Error("LATE_PRODUCER");
        },
      }),
    ]) {
      const entries = fixture(RELEASE_OVERLAY_REVISION);
      entries[1] = change(entries[1]);
      await assert.rejects(inspectProductionAstcEntries(entries, releaseBinding));
    }
    const entries = fixture(RELEASE_OVERLAY_REVISION),
      controller = new AbortController(),
      last = entries.at(-1),
      chunks = last.chunks;
    entries[entries.length - 1] = {
      ...last,
      async *chunks() {
        yield* chunks();
        controller.abort();
      },
    };
    await assert.rejects(
      inspectProductionAstcEntries(entries, releaseBinding, { signal: controller.signal }),
      { name: "AbortError" },
    );
  });
  test("binding extends only exact fifteen extras and does not mutate default profile", async () => {
    const entries = fixture(),
      original = profileFixture();
    const result = await bindProductionAstcEntries(original, entries, binding);
    assert.equal(result.astcOutput.paths.length, 15);
    assert.equal(
      result.astcOutput.totalBytes,
      entries.reduce((total, entry) => total + entry.size, 0),
    );
    assert.deepEqual(result.trustedShell.trustedRecipe.extraFiles, [
      ...original.trustedShell.trustedRecipe.extraFiles,
      ...entries.map((entry) => entry.path).sort(),
    ]);
    assert.equal(original.trustedShell.trustedRecipe.extraFiles.length, 2);
    assert.equal(original.astcOutput, undefined);
    assert.equal(result.shell.entries, original.shell.entries);
    assert.equal(result.close, original.close);
    assert.equal(result.shell.contract.trustedRecipe, result.trustedShell.trustedRecipe);
    assert.ok(result.astcOutput.paths.every((path) => !path.endsWith("/")));
    await assert.rejects(bindProductionAstcEntries(result, entries, binding), /ALREADY_BOUND/);
  });
  test("unbound, partial, external archive/index and unexpected ASTC paths reject", async () => {
    assert.throws(() => selectProductionAstcEntries(fixture()), /UNBOUND/);
    assert.throws(() => selectProductionAstcEntries(fixture().slice(1), binding), /INVENTORY/);
    for (const path of [
      "pack_index.tsv",
      "x/pack_index.tsv",
      `data/${binding.pack.name}`,
      `romfs_overlay/data/${binding.pack.name}`,
      "Legacy Device Pack/data/00-00000008.data",
    ])
      assert.throws(
        () => selectProductionAstcEntries([...fixture(), { path }], binding),
        /EXTERNAL_PACK/,
      );
    const entries = fixture();
    entries[0] = { ...entries[0], path: "romfs_overlay_astc/unexpected" };
    await assert.rejects(inspectProductionAstcEntries(entries, binding), /ASTC_OUTPUT_INVALID/);
  });
  test("chunks are fully drained including later producer errors, with exact sizes and modes", async () => {
    for (const change of [
      (entry) => ({ ...entry, size: entry.size + 1 }),
      (entry) => ({ ...entry, mode: 0o100755 }),
      (entry) => ({ ...entry, mode: 0o120644 }),
      (entry) => ({
        ...entry,
        async *chunks() {
          yield new Uint8Array(entry.size + 1);
        },
      }),
      (entry) => ({
        ...entry,
        async *chunks() {
          yield new Uint8Array(entry.size);
          throw new Error("LATE_PRODUCER");
        },
      }),
      (entry) => ({
        ...entry,
        async *chunks() {
          yield new Uint8Array(new ArrayBuffer(1024 * 1024 + 1), 0, entry.size);
        },
      }),
    ]) {
      const entries = fixture();
      entries[0] = change(entries[0]);
      await assert.rejects(inspectProductionAstcEntries(entries, binding));
    }
  });
  test("duplicate entries and mismatched pack JSON cannot bind", async () => {
    const duplicate = fixture();
    duplicate[1] = duplicate[0];
    await assert.rejects(inspectProductionAstcEntries(duplicate, binding), /ENTRY/);
    await assert.rejects(
      inspectProductionAstcEntries(fixture(), { ...binding, pack: { ...binding.pack, size: 124 } }),
      /ASTC_OUTPUT_INVALID/,
    );
  });
  test("pre-abort prevents reading and cancellation after final chunk rejects", async () => {
    const pre = new AbortController();
    pre.abort();
    const untouched = fixture().map((entry) => ({
      ...entry,
      chunks() {
        assert.fail("unexpected read");
      },
    }));
    await assert.rejects(inspectProductionAstcEntries(untouched, binding, { signal: pre.signal }), {
      name: "AbortError",
    });
    const controller = new AbortController(),
      entries = fixture();
    entries[0] = {
      ...entries[0],
      async *chunks() {
        yield encode("config");
        controller.abort();
      },
    };
    await assert.rejects(
      inspectProductionAstcEntries(entries, binding, { signal: controller.signal }),
      { name: "AbortError" },
    );
  });
  test("large opaque files are drained without full-file reconstruction", async () => {
    const entries = fixture();
    const index = entries.findIndex((entry) => entry.path.endsWith(binding.overlayArchives.base));
    let count = 0;
    entries[index] = {
      ...entries[index],
      size: 2 * 1024 * 1024,
      async *chunks() {
        for (let i = 0; i < 32; i++) {
          count++;
          yield new Uint8Array(65536);
        }
      },
    };
    const result = await inspectProductionAstcEntries(entries, binding);
    assert.equal(count, 32);
    assert.equal(
      result.totalBytes,
      entries.reduce((sum, entry) => sum + entry.size, 0),
    );
  });
}

// optional-codec-receipts
{
  // 通过合成构建凭据验证可选模块的固定编译器、来源与内存契约。
  // Verify pinned compiler, source and memory contracts through synthetic optional-module receipts.
  const { default: test } = nodeTestModule;
  const { default: assert } = strictModule;
  const { readFile } = promisesModule;
  const { validateOptionalCodecReceipts } = packageModule;
  const source = JSON.parse(
    await readFile(
      new URL("../toolchain/sha256-stream-wasm/source-pin.json", import.meta.url),
      "utf8",
    ),
  );
  function fixture() {
    return {
      astcCommit: "2ff200e08c98a61775351687ea2b5d38e455ad8b\n",
      astcCompiler: "emcc (Emscripten gcc/clang-like replacement) 4.0.20\n",
      shaSourcePin: structuredClone(source),
      shaReport: {
        schemaVersion: 1,
        source: structuredClone(source),
        emsdk: "4.0.20",
        compilerVersion: "emcc (Emscripten gcc/clang-like replacement) 4.0.20",
        memoryBytes: 262144,
        stackBytes: 16384,
        chunkBytes: 65536,
        hostImports: 0,
        memoryGrowth: false,
        bytes: 1024,
        sha256: "a".repeat(64),
      },
    };
  }
  test("optional codecs retain exact pinned source/compiler and SHA memory receipts", () => {
    assert.deepEqual(validateOptionalCodecReceipts(fixture()), {
      bytes: 1024,
      sha256: "a".repeat(64),
    });
  });
  test("changed source, compiler, SHA ABI or artifact receipt rejects", () => {
    for (const mutate of [
      (value) => (value.astcCommit = "0".repeat(40)),
      (value) => (value.astcCompiler = value.astcCompiler.replace("4.0.20", "4.0.21")),
      (value) => (value.shaReport.source.commit = "0".repeat(40)),
      (value) => (value.shaReport.source.files["mbedtls/LICENSE"].sha256 = "0".repeat(64)),
      (value) => (value.shaReport.emsdk = "4.0.21"),
      (value) => (value.shaReport.compilerVersion = "emcc 4.0.21"),
      (value) => value.shaReport.memoryBytes++,
      (value) => value.shaReport.stackBytes++,
      (value) => value.shaReport.chunkBytes++,
      (value) => (value.shaReport.hostImports = 1),
      (value) => (value.shaReport.memoryGrowth = true),
      (value) => (value.shaReport.bytes = 0),
      (value) => (value.shaReport.bytes = 64 * 1024 * 1024 + 1),
      (value) => (value.shaReport.sha256 = "bad"),
    ]) {
      const value = fixture();
      mutate(value);
      assert.throws(() => validateOptionalCodecReceipts(value), /codec build receipt/);
    }
  });
}
