// 将实际 ZIP 字节连接到仅元数据的外壳约定。
// 依据 ZIP 读取器、可信发布注册表与外壳约定接口。
// Bridges actual ZIP bytes to the metadata-only shell contract.
// Based on the ZIP reader, trusted release registry, and shell-contract interfaces.

/**
 * 从实际 ZIP 字节到仅元数据外壳约定的显式桥接。
 * Explicit bridge from real ZIP bytes to the metadata-only shell contract.
 */
import { openShellZip, ZIP_LIMITS } from "./zip-reader.mjs";
import { validateShellContract, SHELL_LIMITS } from "./shell-contract.mjs";

export const SHELL_OUTPUT_CHUNK_BYTES = 64 * 1024;
const fail = (code) => {
  throw new Error(code);
};
const check = (signal) => {
  if (signal?.aborted) fail("SHELL_CANCELLED");
};
const compare = (a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
function freezeRecipe(value) {
  if (
    !value ||
    !value.targetOS ||
    !Array.isArray(value.requiredPaths) ||
    value.requiredPaths.length > SHELL_LIMITS.requiredPaths ||
    (value.extraFiles !== undefined &&
      (!Array.isArray(value.extraFiles) || value.extraFiles.length > SHELL_LIMITS.requiredPaths))
  )
    fail("SHELL_RELEASE_INVALID");
  const r = {};
  for (const key of ["schemaVersion", "product", "chain", "kind", "shaderDigest"])
    r[key] = value[key];
  r.targetOS = Object.freeze({
    platform: value.targetOS.platform,
    minimumVersion: value.targetOS.minimumVersion,
  });
  r.requiredPaths = Object.freeze([...value.requiredPaths]);
  r.extraFiles = Object.freeze([...(value.extraFiles ?? [])]);
  return Object.freeze(r);
}
function releaseSnapshot(value, releaseId) {
  // 此布尔值是明确的可信宿主断言，而非签名验证器。
  // 绝不能从不可信 ZIP 内的字段提供此对象。
  // This boolean is an explicit trusted-host assertion, not a signature verifier.
  // Never provide this object from a field inside the untrusted ZIP.
  if (
    !value ||
    value.verified !== true ||
    value.releaseId !== releaseId ||
    !Array.isArray(value.files) ||
    !value.files.length ||
    value.files.length > ZIP_LIMITS.entries
  )
    fail("SHELL_RELEASE_UNVERIFIED");
  const trustedRecipe = freezeRecipe(value.trustedRecipe);
  const files = value.files
    .map((entry) => {
      if (
        !entry ||
        typeof entry.path !== "string" ||
        entry.path.length > SHELL_LIMITS.path ||
        !Number.isSafeInteger(entry.size) ||
        entry.size < 0 ||
        entry.size > ZIP_LIMITS.entryBytes ||
        !Number.isInteger(entry.mode) ||
        entry.mode < 0 ||
        entry.mode > 0xffff ||
        typeof entry.sha256 !== "string" ||
        !/^[0-9a-f]{64}$/.test(entry.sha256)
      )
        fail("SHELL_RELEASE_INVALID");
      return Object.freeze({
        path: entry.path,
        size: entry.size,
        mode: entry.mode,
        sha256: entry.sha256,
      });
    })
    .sort(compare);
  return Object.freeze({ trustedRecipe, files: Object.freeze(files) });
}
function bundleBase(entries) {
  const apps = new Set();
  for (const { path } of entries) {
    if (path === "Payload/") continue;
    const match = /^Payload\/([^/]+\.app)\//.exec(path);
    if (!match) fail("SHELL_BUNDLE_INVALID");
    apps.add(match[1]);
  }
  if (apps.size !== 1) fail("SHELL_BUNDLE_INVALID");
  return `Payload/${[...apps][0]}/`;
}
function text(bytes) {
  if (bytes.some((b) => (b < 32 && b !== 10 && b !== 13) || b > 126)) fail("SHELL_TEXT_INVALID");
  return new TextDecoder("ascii").decode(bytes);
}

/**
 * 返回 packaging-job 的 loadShell({signal}) 适配器，不启用生成。
 * resolveTrustedRelease(releaseId,{signal}) 必须独立认证发布注册表，
 * 并返回 {verified:true,releaseId,trustedRecipe,files:[{path,size,mode,sha256}]}。
 * 此模块不提供注册表、签名者信任根或已下载外壳发现功能。
 * inspectTargetOS({base,entries,readEntry,signal}) 必须独立解析实际已验证的
 * Mach-O/plist 字节，返回 {platform,minimumVersion}；它不接收配方。
 * 随机访问源必须不可变。所有读取都经过 CRC 和 SHA-256 检查。
 * Returns packaging-job's loadShell({signal}) adapter, without enabling generation.
 * resolveTrustedRelease(releaseId,{signal}) MUST independently authenticate a release
 * registry and return {verified:true,releaseId,trustedRecipe,files:[{path,size,mode,sha256}]}.
 * This module supplies no registry, signer trust roots or downloaded shell discovery.
 * inspectTargetOS({base,entries,readEntry,signal}) MUST parse actual verified Mach-O/plist
 * bytes independently and return {platform,minimumVersion}; it receives no recipe.
 * The random-access source must be immutable. All reads are CRC + SHA-256 checked.
 */
export function createShellLoader({
  source,
  releaseId,
  resolveTrustedRelease,
  inspectTargetOS,
} = {}) {
  if (
    !source ||
    typeof source.read !== "function" ||
    typeof releaseId !== "string" ||
    !/^[A-Za-z0-9_.-]{1,128}$/.test(releaseId) ||
    typeof resolveTrustedRelease !== "function" ||
    typeof inspectTargetOS !== "function"
  )
    fail("SHELL_CAPABILITY_UNAVAILABLE");
  return async function loadShell({ signal } = {}) {
    check(signal);
    if (!globalThis.crypto?.subtle) fail("SHELL_SHA256_UNAVAILABLE");
    let resolved;
    try {
      resolved = await resolveTrustedRelease(releaseId, { signal });
    } catch {
      check(signal);
      fail("SHELL_RELEASE_UNAVAILABLE");
    }
    check(signal);
    const release = releaseSnapshot(resolved, releaseId);
    const zip = await openShellZip(source, { signal });
    const actual = [...zip.entries].sort(compare);
    if (
      actual.length !== release.files.length ||
      actual.some((entry, i) =>
        ["path", "size", "mode"].some((key) => entry[key] !== release.files[i][key]),
      )
    )
      fail("SHELL_RELEASE_INVENTORY_MISMATCH");
    const expected = new Map(release.files.map((entry) => [entry.path, entry]));
    const base = bundleBase(actual);
    let reading = false,
      streaming = false;
    async function readVerified(path, fromStream = false) {
      check(signal);
      if (!expected.has(path)) fail("SHELL_ENTRY_MISSING");
      if (reading || (streaming && !fromStream)) fail("SHELL_CONCURRENT_READ_UNSUPPORTED");
      reading = true;
      try {
        const bytes = await zip.readEntry(path, { signal });
        check(signal);
        let digest;
        try {
          digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
        } catch {
          check(signal);
          fail("SHELL_SHA256_FAILED");
        }
        check(signal);
        const hex = Array.from(digest, (b) => b.toString(16).padStart(2, "0")).join("");
        if (hex !== expected.get(path).sha256) fail("SHELL_ENTRY_DIGEST_MISMATCH");
        return bytes;
      } finally {
        reading = false;
      }
    }
    async function readText(path) {
      const entry = expected.get(path);
      if (!entry || entry.size > SHELL_LIMITS.text) fail("SHELL_TEXT_MISSING_OR_OVERSIZED");
      return text(await readVerified(path));
    }
    const shellInfoText = await readText(base + "dm/shell_info.txt");
    const shellManifestText = await readText(base + "dm/shell_manifest.txt");
    let observed;
    try {
      observed = await inspectTargetOS(
        Object.freeze({
          base,
          entries: zip.entries,
          readEntry: (path) => readVerified(path),
          signal,
        }),
      );
    } catch {
      check(signal);
      fail("SHELL_TARGET_INSPECTION_FAILED");
    }
    check(signal);
    const observedTargetOS = Object.freeze({
      platform: observed?.platform,
      minimumVersion: observed?.minimumVersion,
    });
    const fullContract = {
      entries: zip.entries,
      shellInfoText,
      shellManifestText,
      trustedRecipe: release.trustedRecipe,
      observedTargetOS,
    };
    validateShellContract(fullContract);
    // 当前协调器的 ZIP32 写入器不支持显式目录。
    // 验证所有目录记录，然后仅省略冗余的目录元数据。
    // The current coordinator's ZIP32 writer has no explicit-directory support.
    // Validate all directory records, then omit only redundant directory metadata.
    const entries = Object.freeze(
      actual
        .filter((entry) => !entry.directory)
        .map(({ path, size, mode }) => {
          if (![0o644, 0o755].includes(mode & 0o777)) fail("SHELL_OUTPUT_MODE_UNSUPPORTED");
          return Object.freeze({
            path,
            size,
            mode,
            async *chunks() {
              check(signal);
              if (streaming || reading) fail("SHELL_CONCURRENT_READ_UNSUPPORTED");
              streaming = true;
              try {
                const bytes = await readVerified(path, true);
                for (let p = 0; p < bytes.length; p += SHELL_OUTPUT_CHUNK_BYTES) {
                  check(signal);
                  yield bytes.subarray(p, p + SHELL_OUTPUT_CHUNK_BYTES);
                }
                check(signal);
              } finally {
                streaming = false;
              }
            },
          });
        }),
    );
    const contract = Object.freeze({
      ...fullContract,
      entries: Object.freeze(
        entries.map(({ path, size, mode }) => Object.freeze({ path, size, mode })),
      ),
    });
    validateShellContract(contract);
    return Object.freeze({
      contract,
      entries,
      releaseId,
      contentVerified: false,
      bytesVerifiedOnRead: true,
      installable: false,
    });
  };
}
