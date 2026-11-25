// 依据 gen_romfs_overlay.py 的 ASTC 基础与 item_notes 文件集合验证独立产物；不修改默认 BC 输出。
// Validate isolated ASTC base and item_notes outputs against gen_romfs_overlay.py without changing default BC outputs.
import { RELEASE_OVERLAY_REVISION } from "./toolchain/overlay-recipe-js/revision.mjs";
import releaseSource from "./toolchain/overlay-recipe-js/release-contract-source.json" with { type: "json" };

export const ASTC_OUTPUT_LIMITS = Object.freeze({
  fileBytes: 64 * 1024 * 1024,
  totalBytes: 128 * 1024 * 1024,
  packJsonBytes: 512,
  markerBytes: 64,
  manifestBytes: 4096,
});
const fail = () => {
  throw new Error("ASTC_OUTPUT_INVALID");
};
export function validateCascArchiveName(name) {
  if (typeof name !== "string" || !/^\d{2,3}-\d{8}\.data$/.test(name)) fail();
  const [part, archive] = name.slice(0, -5).split("-").map(Number);
  if (
    part > 255 ||
    archive > 255 ||
    name !== `${String(part).padStart(2, "0")}-${String(archive).padStart(8, "0")}.data`
  )
    fail();
  return name;
}
// 这只是受信任最终产物的结构快照，不认证调用者提供的哈希、位置或来源。
// This is a structural snapshot of a trusted finalized artifact, not authentication of caller hashes, locations or provenance.
export function validateLegacyPackMetadata(pack) {
  if (
    !pack ||
    typeof pack !== "object" ||
    Array.isArray(pack) ||
    Object.keys(pack).sort().join(",") !== "name,sha256,size"
  )
    fail();
  const { name, size, sha256 } = pack;
  validateCascArchiveName(name);
  if (
    !Number.isSafeInteger(size) ||
    size < 0 ||
    size > 2 ** 40 ||
    typeof sha256 !== "string" ||
    !/^[a-f0-9]{64}$/.test(sha256)
  )
    fail();
  return Object.freeze({ name, size, sha256 });
}
export function astcOutputPaths({
  overlayArchives,
  recipeRevision = RELEASE_OVERLAY_REVISION,
} = {}) {
  if (
    ![RELEASE_OVERLAY_REVISION].includes(recipeRevision) ||
    !overlayArchives ||
    Object.keys(overlayArchives).sort().join(",") !== "base,item_notes"
  )
    fail();
  const baseName = validateCascArchiveName(overlayArchives.base),
    notesName = validateCascArchiveName(overlayArchives.item_notes);
  const base = "romfs_overlay_astc",
    notes = "romfs_overlay_astc_variants/item_notes";
  const expected = [
    `${base}/data/.build.config`,
    `${base}/data/${baseName}`,
    `${base}/ctrl_touch`,
    `${base}/touch_strings.tsv`,
    `${base}/pack.json`,
    `${notes}/data/.build.config`,
    `${notes}/data/${notesName}`,
    `${notes}/ctrl_touch`,
    `${notes}/touch_strings.tsv`,
    `${notes}/dm_textbox_plain.tsv`,
    `${notes}/features.txt`,
    ...[base, notes].flatMap((root) => [`${root}/xbox_glyphs`, `${root}/overlay.manifest`]),
  ].sort();
  return Object.freeze(expected);
}

function releaseManifest(files, root, expected) {
  const bytes = files[`${root}/overlay.manifest`];
  if (bytes.length > ASTC_OUTPUT_LIMITS.manifestBytes) fail();
  let text;
  try {
    text = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(bytes);
  } catch {
    fail();
  }
  const members = expected
    .filter((path) => path.startsWith(root + "/") && path !== `${root}/overlay.manifest`)
    .map((path) => path.slice(root.length + 1));
  const lines = text.split("\n");
  if (
    lines.shift() !== `DM_OVERLAY_V1 ${releaseSource.sourceFingerprint}` ||
    lines.pop() !== "" ||
    lines.length !== members.length
  )
    fail();
  for (const [index, name] of members.entries()) {
    const match = /^([a-f0-9]{64}) (.+)$/.exec(lines[index]);
    if (!match || match[2] !== name) fail();
  }
}

export function validateAstcOutputs({
  files,
  pack,
  overlayArchives,
  recipeRevision = RELEASE_OVERLAY_REVISION,
} = {}) {
  const bound = validateLegacyPackMetadata(pack);
  const expected = astcOutputPaths({ overlayArchives, recipeRevision });
  if (!files || typeof files !== "object" || Array.isArray(files)) fail();
  if (Object.values(overlayArchives).includes(bound.name)) fail();
  const base = "romfs_overlay_astc",
    notes = "romfs_overlay_astc_variants/item_notes";
  if (JSON.stringify(Object.keys(files).sort()) !== JSON.stringify(expected)) fail();
  let totalBytes = 0;
  for (const path of expected) {
    const bytes = files[path];
    if (
      !(bytes instanceof Uint8Array) ||
      !(bytes.buffer instanceof ArrayBuffer) ||
      bytes.buffer.resizable ||
      bytes.buffer.byteLength > ASTC_OUTPUT_LIMITS.fileBytes ||
      bytes.length > ASTC_OUTPUT_LIMITS.fileBytes ||
      (totalBytes += bytes.length) > ASTC_OUTPUT_LIMITS.totalBytes
    )
      fail();
    if ((path.includes("/data/") || path.endsWith("/ctrl_touch")) && bytes.length === 0) fail();
  }
  const jsonBytes = files[`${base}/pack.json`];
  if (jsonBytes.length > ASTC_OUTPUT_LIMITS.packJsonBytes) fail();
  let json, parsed;
  try {
    json = new TextDecoder("utf-8", { fatal: true }).decode(jsonBytes);
    parsed = JSON.parse(json);
  } catch {
    fail();
  }
  // 仅有三个平面键，拒绝重复键与转义键，以免 JSON 解析掩盖歧义。
  // Exactly three flat keys; reject duplicates and escaped keys so JSON parsing cannot hide ambiguity.
  const jsonKeys = [...json.matchAll(/"((?:\\.|[^"\\])*)"\s*:/g)].map((match) => match[1]);
  if (jsonKeys.sort().join(",") !== "name,sha256,size") fail();
  const actual = validateLegacyPackMetadata(parsed);
  if (actual.name !== bound.name || actual.size !== bound.size || actual.sha256 !== bound.sha256)
    fail();
  const features = new TextDecoder("utf-8", { fatal: true }).decode(files[`${notes}/features.txt`]);
  if (!features.startsWith("item_notes\n")) fail();
  if (recipeRevision === RELEASE_OVERLAY_REVISION) {
    for (const root of [base, notes]) {
      const marker = files[`${root}/xbox_glyphs`];
      if (
        marker.length > ASTC_OUTPUT_LIMITS.markerBytes ||
        new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(marker) !==
          "ztouch_controller_buttons_ui/switchx*.sprite\n"
      )
        fail();
      releaseManifest(files, root, expected);
    }
  }
  // 清单检查只验证源码指纹与精确行结构，不认证不透明归档的内容摘要。
  // Manifest checks bind the source fingerprint and exact row structure, not opaque archive content hashes.
  return Object.freeze({ paths: expected, totalBytes, pack: bound, recipeRevision });
}
