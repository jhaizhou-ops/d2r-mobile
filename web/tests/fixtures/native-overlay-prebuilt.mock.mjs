// 注入仅合成依赖以验证组装；不虚构公开认证开关。
// Inject synthetic dependencies to test assembly; no public authentication bypass flag.
import assert from "node:assert/strict";
import { mock, test } from "node:test";
const calls = [];
let badArchive = false;
const manifest = new Uint8Array();
mock.module("../../extraction-job-adapter.mjs", {
  namedExports: {
    readAuthenticatedAssets() {
      return {
        romfs: {
          getFile() {
            return { size: 0n, readAt: async () => manifest };
          },
        },
        logicalAssets: {},
      };
    },
  },
});
const files = () => ({
  "data/.build.config": new Uint8Array([1, 2]),
  "data/00-00000002.data": new Uint8Array([3, 4]),
});
mock.module("../../casc-overlay-writer.mjs", {
  namedExports: {
    async prepareCascOverlayManifest() {
      return Object.freeze({});
    },
    async writeCascOverlay({ replacements }) {
      calls.push(["bc", replacements.map((r) => Array.from(r.bytes))]);
      return { files: files() };
    },
    async writeCascPrebuiltOverlay({ replacements, external }) {
      calls.push(["astc", replacements.map((r) => Array.from(r.bytes))]);
      const out = files();
      if (badArchive) {
        out["data/00-00000009.data"] = out["data/00-00000002.data"];
        delete out["data/00-00000002.data"];
      }
      return { files: out, external: { count: external.records.length, pack: external.pack } };
    },
  },
});
const { generateNativeOverlay, NATIVE_OVERLAY_GENERATED } =
  await import("../../native-overlay-generator.mjs");
const { RELEASE_OVERLAY_REVISION, UPSTREAM_OVERLAY_REVISION } =
  await import("../../toolchain/overlay-recipe-js/revision.mjs");
const { createOverlayManifest } = await import("../../overlay-contract.mjs");
function options(upstream = true, revision = RELEASE_OVERLAY_REVISION) {
  return {
    ...(upstream ? { recipeRevision: revision } : {}),
    generated: Object.fromEntries(
      NATIVE_OVERLAY_GENERATED.map((name) => [name, new Uint8Array([8])]),
    ),
    worker: {
      close() {},
      async run(_operation, _payload, operationOptions) {
        assert.deepEqual(operationOptions, upstream ? { revision } : {});
        const files = {},
          variants = ["base", "item_notes"].map((name, i) => {
            const id = `${i}-replacement`;
            files[id] = new Uint8Array([10 + i]);
            let names = i
              ? ["ctrl_touch", "dm_textbox_plain.tsv", "features.txt", "touch_strings.tsv"]
              : ["ctrl_touch", "touch_strings.tsv"];
            if (upstream)
              names = [
                ...names,
                ...(revision === UPSTREAM_OVERLAY_REVISION ? ["ps_glyphs"] : []),
                "xbox_glyphs",
              ].sort();
            return {
              name,
              replacements: [{ path: "data/a.bin", add: false, file: id }],
              sidecars: names.map((path, j) => {
                const file = `${i}-${j}`;
                files[file] = new Uint8Array([20 + j]);
                return { path, file };
              }),
            };
          });
        return { files, variants };
      },
    },
  };
}
const pack = { name: "00-00000008.data", size: 20, sha256: "a".repeat(64) };
const external = () => ({
  pack: { ...pack },
  records: [
    {
      path: "data/z.bin",
      oldEkey: "1".repeat(32),
      newEkey: "2".repeat(32),
      encodedSize: 20,
      decodedSize: 30,
    },
  ],
});
test("b811 release seals Xbox-only variants without later PlayStation activation", async () => {
  const out = await generateNativeOverlay({
    ...options(true, RELEASE_OVERLAY_REVISION),
    prebuiltPack: external(),
  });
  for (const root of [
    "romfs_overlay",
    "romfs_overlay_variants/item_notes",
    "romfs_overlay_astc",
    "romfs_overlay_astc_variants/item_notes",
  ]) {
    assert.ok(out.files[`${root}/xbox_glyphs`]);
    assert.equal(out.files[`${root}/ps_glyphs`], undefined);
    assert.match(
      new TextDecoder().decode(out.files[`${root}/overlay.manifest`]),
      /^DM_OVERLAY_V1 243ab3eec668192415561f806ebbf2e670ea5a8a83aee8b72f92021a23989763\n/,
    );
  }
  assert.equal(Object.keys(out.files).length, 29);
});
test("latest-source branch seals every variant after markers and ASTC metadata", async () => {
  const out = await generateNativeOverlay({
    ...options(true, UPSTREAM_OVERLAY_REVISION),
    prebuiltPack: external(),
  });
  for (const root of [
    "romfs_overlay",
    "romfs_overlay_variants/item_notes",
    "romfs_overlay_astc",
    "romfs_overlay_astc_variants/item_notes",
  ]) {
    const inventory = Object.fromEntries(
      Object.entries(out.files)
        .filter(([path]) => path.startsWith(root + "/") && path !== `${root}/overlay.manifest`)
        .map(([path, data]) => [path.slice(root.length + 1), data]),
    );
    assert.ok(inventory.ps_glyphs);
    assert.ok(inventory.xbox_glyphs);
    assert.deepEqual(out.files[`${root}/overlay.manifest`], await createOverlayManifest(inventory));
  }
  assert.equal(Object.keys(out.files).length, 33);
  await assert.rejects(
    generateNativeOverlay({ ...options(), recipeRevision: "v2.0.0" }),
    /REVISION/,
  );
});
test("optional ASTC preserves all default bytes, recipes and exact15 output", async () => {
  calls.length = 0;
  const base = await generateNativeOverlay(options());
  assert.equal(Object.hasOwn(base, "astc"), false);
  assert.deepEqual(
    calls.map((c) => c[0]),
    ["bc", "bc"],
  );
  calls.length = 0;
  const out = await generateNativeOverlay({ ...options(), prebuiltPack: external() });
  for (const [name, value] of Object.entries(base.files)) assert.deepEqual(out.files[name], value);
  assert.deepEqual(
    calls.map((c) => c[0]),
    ["bc", "astc", "bc", "astc"],
  );
  assert.deepEqual(calls[0][1], calls[1][1]);
  assert.deepEqual(calls[2][1], calls[3][1]);
  assert.equal(Object.keys(out.files).filter((p) => p.startsWith("romfs_overlay_astc")).length, 15);
  assert.deepEqual(out.astc, {
    count: 1,
    pack,
    overlayArchives: { base: "00-00000002.data", item_notes: "00-00000002.data" },
  });
  assert.deepEqual(
    JSON.parse(new TextDecoder().decode(out.files["romfs_overlay_astc/pack.json"])),
    pack,
  );
  assert.equal(Object.hasOwn(out.files, "romfs_overlay_astc_variants/item_notes/pack.json"), false);
  for (const side of ["ctrl_touch", "touch_strings.tsv"])
    assert.deepEqual(out.files[`romfs_overlay/${side}`], out.files[`romfs_overlay_astc/${side}`]);
});
test("archive mismatch and payload-bearing input fail closed", async () => {
  badArchive = true;
  await assert.rejects(
    generateNativeOverlay({ ...options(), prebuiltPack: external() }),
    /OVERLAY_PREBUILT_BINDING/,
  );
  badArchive = false;
  await assert.rejects(
    generateNativeOverlay({ ...options(), prebuiltPack: { ...external(), source: {} } }),
    /OVERLAY_PREBUILT_METADATA/,
  );
});

test("recycle failure wipes recipe buffers before releasing them", async () => {
  const opts = options();
  const run = opts.worker.run;
  let returned;
  opts.worker.run = async (...args) => {
    returned = await run(...args);
    const id = Object.keys(returned.files)[0];
    returned.files[id] = returned.files[id].buffer;
    return returned;
  };
  await assert.rejects(
    generateNativeOverlay({
      ...opts,
      recycleWorker: async () => {
        throw new Error("restart failure");
      },
    }),
    /OVERLAY_RECIPE_FAILED/,
  );
  for (const value of Object.values(returned.files)) {
    const bytes = value instanceof ArrayBuffer ? new Uint8Array(value) : value;
    assert.ok(bytes.every((byte) => byte === 0));
  }
  for (const value of Object.values(opts.generated)) assert.deepEqual(value, new Uint8Array([8]));
});
