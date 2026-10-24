// 在 CASC 和所有旁路文件完成后生成原生 v1 完整清单；不访问文件系统。
// Finalize the native v1 complete manifest after CASC and sidecars, without filesystem access.
import source from "./toolchain/overlay-recipe-js/contract-source.json" with { type: "json" };
import releaseSource from "./toolchain/overlay-recipe-js/release-contract-source.json" with { type: "json" };
import {
  RELEASE_OVERLAY_REVISION,
  UPSTREAM_OVERLAY_REVISION,
} from "./toolchain/overlay-recipe-js/revision.mjs";

export const OVERLAY_SOURCE_FINGERPRINT = source.sourceFingerprint;
const encoder = new TextEncoder();
const required = ["ctrl_touch", "touch_strings.tsv", "data/.build.config"];
const isData = (path) => /^data\/[^/]+\.data$/.test(path);
const fail = (code) => {
  throw new Error(code);
};
const validPath = (path) =>
  typeof path === "string" &&
  path.length > 0 &&
  path.length <= 1024 &&
  path.split("/").length <= 32 &&
  path
    .split("/")
    .every(
      (part) =>
        /^[A-Za-z0-9_.+-]+$/.test(part) &&
        ![".", ".."].includes(part) &&
        !part.endsWith(".") &&
        !/^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$/i.test(part.split(".")[0]),
    );

export async function createOverlayManifest(
  files,
  { signal, revision = UPSTREAM_OVERLAY_REVISION } = {},
) {
  if (![RELEASE_OVERLAY_REVISION, UPSTREAM_OVERLAY_REVISION].includes(revision))
    fail("OVERLAY_CONTRACT_REVISION");
  const sourceFingerprint =
    revision === RELEASE_OVERLAY_REVISION
      ? releaseSource.sourceFingerprint
      : OVERLAY_SOURCE_FINGERPRINT;
  const names = Object.keys(files).sort(),
    folds = new Set();
  if (names.length > 4096 || !/^[0-9a-f]{64}$/.test(OVERLAY_SOURCE_FINGERPRINT))
    fail("OVERLAY_CONTRACT_LIMIT");
  for (const name of names) {
    if (
      !validPath(name) ||
      name.toLowerCase() === "overlay.manifest" ||
      folds.has(name.toLowerCase())
    )
      fail("OVERLAY_CONTRACT_PATH");
    folds.add(name.toLowerCase());
  }
  for (const name of names) {
    const parts = name.toLowerCase().split("/");
    for (let end = 1; end < parts.length; end++)
      if (folds.has(parts.slice(0, end).join("/"))) fail("OVERLAY_CONTRACT_PATH");
  }
  if (required.some((name) => !Object.hasOwn(files, name)) || !names.some(isData))
    fail("OVERLAY_CONTRACT_REQUIRED");
  let total = 0;
  const lines = [`DM_OVERLAY_V1 ${sourceFingerprint}\n`];
  for (const name of names) {
    signal?.throwIfAborted();
    const data = files[name];
    if (!(data instanceof Uint8Array) || data.length > 8 * 1024 ** 3) fail("OVERLAY_CONTRACT_FILE");
    if (data.length === 0 && (required.includes(name) || isData(name)))
      fail("OVERLAY_CONTRACT_EMPTY");
    total += data.length;
    if (total > 16 * 1024 ** 3) fail("OVERLAY_CONTRACT_LIMIT");
    const digest = new Uint8Array(await globalThis.crypto.subtle.digest("SHA-256", data));
    lines.push(
      Array.from(digest, (byte) => byte.toString(16).padStart(2, "0")).join("") + ` ${name}\n`,
    );
  }
  signal?.throwIfAborted();
  const manifest = encoder.encode(lines.join(""));
  if (manifest.length > 4 * 1024 * 1024) fail("OVERLAY_CONTRACT_LIMIT");
  return manifest;
}
