// 构建时集中校验资产来源；导入不执行检查，选择器只运行指定门禁。
// Build-only asset source gates; imports have no side effects and selectors run only their selected checks.
import process from "node:process";
import { Buffer } from "node:buffer";
import { readFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { pathToFileURL } from "node:url";
import { resolve } from "node:path";

export async function verifyAssetTransformSources() {
  // 仅构建时核对原始来源；不读取游戏资源。 | Build-only source pin check; reads no game resources.
  const root = new URL("../../", import.meta.url);
  const manifest = JSON.parse(
    await readFile(
      new URL("./asset-transforms-js/source-provenance.json", import.meta.url),
      "utf8",
    ),
  );
  if (manifest.schemaVersion !== 1 || Object.keys(manifest.sources).length !== 7)
    throw new Error("Asset transform source manifest drift");
  for (const [path, expected] of Object.entries(manifest.sources)) {
    const bytes = await readFile(new URL(path, root));
    if (createHash("sha256").update(bytes).digest("hex") !== expected)
      throw new Error(`Asset transform native source drift: ${path}`);
  }
  return manifest;
}

export async function verifyCascOverlaySources() {
  // 固定原生 CASC 来源与 MD5，构建时拒绝漂移。 | Pin native CASC sources and MD5; fail the build on drift.
  const root = new URL("../../", import.meta.url);
  const manifest = JSON.parse(
    await readFile(new URL("./casc-overlay-js/source-provenance.json", import.meta.url), "utf8"),
  );
  if (manifest.schemaVersion !== 1 || Object.keys(manifest.sources).length !== 3)
    throw new Error("CASC overlay source manifest drift");
  for (const [path, expected] of Object.entries(manifest.sources)) {
    const bytes = await readFile(new URL(path, root));
    if (createHash("sha256").update(bytes).digest("hex") !== expected)
      throw new Error(`CASC overlay source drift: ${path}`);
  }
}

export async function verifyArtRecipeSources() {
  // 构建时校验原配方、适配器与已迁移的数值核。 | Verify original recipes, adapters and migrated numeric kernels at build time.
  const root = new URL("../../", import.meta.url);
  const manifest = JSON.parse(
    await readFile(new URL("./art-recipes-js/source-provenance.json", import.meta.url), "utf8"),
  );
  if (manifest.schemaVersion !== 1 || Object.keys(manifest.sources).length !== 11)
    throw new Error("Art recipe source manifest drift");
  for (const [path, digest] of Object.entries(manifest.sources)) {
    if (
      createHash("sha256")
        .update(await readFile(new URL(path, root)))
        .digest("hex") !== digest
    )
      throw new Error(`Art recipe source drift: ${path}`);
  }
}

export async function verifyRemainingArtSources() {
  // 校验剩余美术配方的来源摘要；依据 web/toolchain/remaining-art-js/scripts 的模块与调用约定。
  // Verify source digests for remaining-art recipes using the module and calling contracts in web/toolchain/remaining-art-js/scripts.
  const pins = JSON.parse(
    readFileSync(new URL("./remaining-art-js/source-provenance.json", import.meta.url)),
  );
  const root = new URL("../../", import.meta.url);
  for (const [path, expected] of Object.entries(pins)) {
    const bytes = readFileSync(new URL(path, root));
    if (createHash("sha256").update(bytes).digest("hex") !== expected)
      throw new Error("Remaining-art source drift: " + path);
  }
  console.log(`Verified ${Object.keys(pins).length} remaining-art source pins`);
}

export async function verifyHudRecipeSources() {
  // 仅用于打包门禁：保留全部现有源码与元数据校验。
  // Packaging gate only: preserve every existing source/metadata guard.
  const root = new URL("../../", import.meta.url);
  const pins = JSON.parse(
    readFileSync(new URL("./hud-recipes-js/provenance.json", import.meta.url)),
  );
  for (const [path, expected] of Object.entries(pins)) {
    const actual = createHash("sha256")
      .update(readFileSync(new URL(path, root)))
      .digest("hex");
    if (actual !== expected) throw new Error("HUD source drift: " + path);
  }
  const previous = JSON.parse(
    readFileSync(new URL("./asset-generation-spike/hud-provenance.json", import.meta.url)),
  );
  if (JSON.stringify(pins) !== JSON.stringify(previous))
    throw new Error("HUD provenance contract drift");
  console.log("HUD recipe source and metadata pins verified");
}

export async function verifyOverlayRecipeSources() {
  // 校验覆盖层配方的源文件与静态数据摘要；依据 web/toolchain/overlay-recipe-js/scripts 的模块与调用约定。
  // Verify source and static-data digests for overlay recipes using the module and calling contracts in web/toolchain/overlay-recipe-js/scripts.
  const pins = JSON.parse(
    readFileSync(new URL("./overlay-recipe-js/source-provenance.json", import.meta.url)),
  );
  const root = new URL("../../", import.meta.url);
  for (const [path, expected] of Object.entries(pins)) {
    const raw = readFileSync(new URL(path, root));
    if (createHash("sha256").update(raw).digest("hex") !== expected)
      throw new Error("Overlay recipe source drift: " + path);
  }
  const close = JSON.parse(
    readFileSync(new URL("./overlay-recipe-js/close-image.json", import.meta.url)),
  );
  if (
    createHash("sha256").update(Buffer.from(close.base64, "base64")).digest("hex") !== close.sha256
  )
    throw new Error("Overlay recipe close image drift");
  const dataPins = JSON.parse(
    readFileSync(new URL("./overlay-recipe-js/data-provenance.json", import.meta.url)),
  );
  for (const [path, expected] of Object.entries(dataPins)) {
    if (
      createHash("sha256")
        .update(readFileSync(new URL("./overlay-recipe-js/" + path, import.meta.url)))
        .digest("hex") !== expected
    )
      throw new Error("Overlay recipe static data drift: " + path);
  }
  const contract = JSON.parse(
    readFileSync(new URL("./overlay-recipe-js/contract-source.json", import.meta.url)),
  );
  const fingerprint = createHash("sha256").update("DM_OVERLAY_SOURCE_V1\0");
  for (const [path, expected] of Object.entries(contract.normalizedSources)) {
    const text = new TextDecoder("utf-8", { fatal: true }).decode(
      readFileSync(new URL(path, root)),
    );
    const normalized = Buffer.from(text.replace(/\r\n|\r/g, "\n")),
      length = Buffer.alloc(8);
    length.writeBigUInt64BE(BigInt(normalized.length));
    if (createHash("sha256").update(normalized).digest("hex") !== expected)
      throw new Error("Overlay contract normalized source drift: " + path);
    fingerprint
      .update(path + "\0")
      .update(length)
      .update(normalized);
  }
  if (fingerprint.digest("hex") !== contract.sourceFingerprint)
    throw new Error("Overlay contract source fingerprint drift");
  console.log(
    `Verified ${Object.keys(pins).length} overlay recipe source pins and ${Object.keys(dataPins).length} static data pins`,
  );
}

export const ASSET_SOURCE_GATES = Object.freeze({
  "asset-transforms": verifyAssetTransformSources,
  "casc-overlay": verifyCascOverlaySources,
  "art-recipes": verifyArtRecipeSources,
  "remaining-art": verifyRemainingArtSources,
  hud: verifyHudRecipeSources,
  overlay: verifyOverlayRecipeSources,
});
export async function verifyAssetSources(selector) {
  if (!Object.hasOwn(ASSET_SOURCE_GATES, selector))
    throw new Error(`Unknown asset source gate: ${selector}`);
  return ASSET_SOURCE_GATES[selector]();
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const selectors = process.argv.slice(2);
  if (!selectors.length) throw new Error("Specify an asset source gate or all");
  for (const selector of selectors) {
    if (selector === "all") {
      for (const gate of Object.keys(ASSET_SOURCE_GATES)) await verifyAssetSources(gate);
    } else await verifyAssetSources(selector);
  }
}
