// 三个有界配方的合成逐字节比较；不启动浏览器、不读取游戏文件。 | Synthetic exact-byte comparison for three bounded recipes; no browser or game files.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import process from "node:process";
import { createImageKernels } from "../../image-kernels-rust/kernels.mjs";
import { createPngCodec } from "../../png-codec-exact/codec.mjs";
import { createFontRasterizer } from "../../font-rasterizer-exact/font.mjs";
import { defaultState } from "../../art-kernels/kernels.mjs";
import { generateNativeArt } from "../native-art.mjs";
import { createNativeArtCases, nativeHashDependencies } from "./native-cases.mjs";
import { createArtRecipes } from "../recipes.mjs";
import { generateDefaultFrame } from "../default-frame.mjs";
import { generateMapGoal, LABELS } from "../map-goal.mjs";
import { RELEASE_ART_REVISION, UPSTREAM_ART_REVISION } from "../revision.mjs";
import { createOracle } from "./oracle.mjs";
import { verifyArtRecipeSources } from "../../verify-asset-sources.mjs";
await verifyArtRecipeSources();
const oracle = await createOracle();
try {
  const wasm = process.argv.includes("--wasm");
  const kernels = await createImageKernels(
    await readFile(new URL("../../image-kernels-rust/dist/image-kernels.wasm", import.meta.url)),
  );
  const codec = wasm
    ? await createPngCodec(
        await readFile(new URL("../../png-codec-exact/dist/png-codec.wasm", import.meta.url)),
      )
    : oracle.codec;
  const font = wasm
    ? await createFontRasterizer(
        await readFile(
          new URL("../../font-rasterizer-exact/dist/font-rasterizer.wasm", import.meta.url),
        ),
      )
    : oracle.font;
  const dependencies = { kernels, codec, font, defaultState, ...nativeHashDependencies };
  const enc = (obj) => new TextEncoder().encode(JSON.stringify(obj));
  const b64 = (bytes) => Buffer.from(bytes).toString("base64");
  const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const pattern = (length, seed = 0) =>
    Uint8Array.from({ length }, (_, i) => (i * 43 + Math.floor(i / 7) * 97 + seed) % 256);
  let count = 0;
  async function compare(operation, generate, payload, options, rejects = false) {
    if (rejects) {
      assert.throws(
        () => oracle.generate(operation, payload, options),
        `${operation} oracle should reject`,
      );
      await assert.rejects(
        async () => generate(payload, options, dependencies),
        `${operation} port should reject`,
      );
    } else {
      const actual = await generate(payload, options, dependencies);
      const expected = oracle.generate(operation, payload, options);
      assert.deepEqual(Object.keys(actual.files).sort(), Object.keys(expected.files).sort());
      for (const name of Object.keys(expected.files))
        assert.equal(
          Buffer.compare(actual.files[name], expected.files[name]),
          0,
          `${operation}: ${name}`,
        );
      const metadata = (value) =>
        Object.fromEntries(Object.entries(value).filter(([name]) => name !== "files"));
      assert.deepEqual(metadata(actual), metadata(expected), operation);
    }
    count++;
  }
  function sprite(fw, frames, height, seed) {
    const width = fw * frames,
      raw = pattern(width * height * 4, seed),
      payload = new Uint8Array(40 + raw.length),
      view = new DataView(payload.buffer);
    payload.set([83, 112, 65, 49]);
    view.setUint16(4, 31, true);
    view.setUint16(6, fw, true);
    view.setUint32(8, width, true);
    view.setUint32(12, height, true);
    view.setUint32(20, 1, true);
    view.setUint32(32, raw.length, true);
    view.setUint32(36, 4, true);
    payload.set(raw, 40);
    return payload;
  }
  for (const [fw, frames, height, seed, index] of [
    [1, 1, 1, 0, 0],
    [23, 3, 19, 7, 1],
    [19, 4, 31, 255, 3],
  ])
    await compare("image-default-frame", generateDefaultFrame, sprite(fw, frames, height, seed), {
      frame: index,
    });
  const small = sprite(3, 2, 5, 4);
  for (const options of [{ frame: -1 }, { frame: 2 }, { frame: 0.5 }, { frame: true }])
    await compare("image-default-frame", generateDefaultFrame, small, options, true);
  await compare("image-default-frame", generateDefaultFrame, small.subarray(1), {}, true);
  const broken = small.slice();
  new DataView(broken.buffer).setUint32(36, 3, true);
  await compare("image-default-frame", generateDefaultFrame, broken, {}, true);
  const fontPath =
    process.argv.find((arg) => arg.startsWith("--font="))?.slice(7) ||
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
  const fontBytes = await readFile(fontPath);
  const strings = {};
  for (const [table, key] of LABELS) {
    strings[table] ||= [];
    strings[table].push({
      Key: key,
      zhCN: "名字",
      enUS: "AV jg",
      esES: "Áéñ",
      esMX: "café",
      ruRU: "Жизнь",
      koKR: "생명력",
    });
  }
  const bundle = {
    images: {
      symbols: { size: [7 * 9, 11], frameWidth: 7, frames: 7, rgba: b64(pattern(7 * 9 * 11 * 4)) },
      units: {
        size: [8 * 7, 13],
        frameWidth: 6,
        frames: 8,
        rgba: b64(pattern(8 * 7 * 13 * 4, 23)),
      },
    },
    font: b64(fontBytes),
    strings: Object.fromEntries(
      Object.entries(strings).map(([name, rows]) => [
        name,
        (name === "levels" ? "\ufeff" : "") + JSON.stringify(rows),
      ]),
    ),
  };
  await compare("map-goal", generateMapGoal, enc(bundle), { names: "current" });
  for (const revision of [RELEASE_ART_REVISION, UPSTREAM_ART_REVISION])
    await compare("map-goal", generateMapGoal, enc(bundle), { names: "current", revision });
  for (const options of [{}, { names: "old" }, { names: "current", extra: 0 }])
    await compare("map-goal", generateMapGoal, enc(bundle), options, true);
  const modified = (f) => {
    const clone = structuredClone(bundle);
    f(clone);
    return enc(clone);
  };
  await compare(
    "map-goal",
    generateMapGoal,
    modified((b) => {
      b.images.units.frames = 7;
    }),
    { names: "current" },
    true,
  );
  await compare(
    "map-goal",
    generateMapGoal,
    modified((b) => {
      b.images.symbols.rgba = "AAAA!";
    }),
    { names: "current" },
    true,
  );
  await compare(
    "map-goal",
    generateMapGoal,
    modified((b) => {
      b.strings.npcs = JSON.stringify([{ Key: "Drehya" }, { Key: "Drehya" }]);
    }),
    { names: "current" },
    true,
  );
  await compare(
    "map-goal",
    generateMapGoal,
    modified((b) => {
      b.strings.levels = "\ufeff" + b.strings.levels;
    }),
    { names: "current" },
    true,
  );
  const floatDimensions = new TextEncoder().encode(
    JSON.stringify(bundle).replace('"frameWidth":7', '"frameWidth":7.0'),
  );
  await compare("map-goal", generateMapGoal, floatDimensions, { names: "current" }, true);
  const withLong = modified((b) => {
    const rows = JSON.parse(b.strings.npcs);
    rows[0].zhCN = "x".repeat(257);
    b.strings.npcs = JSON.stringify(rows);
  });
  await compare("map-goal", generateMapGoal, withLong, { names: "current" }, true);
  for (const { payload, options, rejects } of await createNativeArtCases(codec))
    await compare("native-art", generateNativeArt, payload, options, rejects);
  assert.throws(
    () => createArtRecipes({ ...dependencies, codec: { ...codec, zlibVersion: "1.3.2" } }),
    /PNG_BACKEND/,
  );
  const execute = createArtRecipes(dependencies);
  const noFont = createArtRecipes({ ...dependencies, font: null });
  assert.deepEqual(
    await execute("image-default-frame", small),
    await noFont("image-default-frame", small),
  );
  await assert.rejects(() => noFont("map-goal", enc(bundle), { names: "current" }), /FONT_BACKEND/);
  await assert.rejects(() => execute("unsupported", new Uint8Array()), /Unsupported/);
  console.log(
    JSON.stringify({
      cases: count,
      exactBytes: true,
      kernels: "real WASM",
      codecAndFont: wasm ? "real WASM" : "bundled Pillow oracle (orchestration isolation)",
      testFontSha256: hash(fontBytes),
    }),
  );
} finally {
  await oracle.close();
}
