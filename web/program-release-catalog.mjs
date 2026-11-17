// 依据仓库发布记录和完整文件摘要描述受信任程序输入。
// Describe trusted program inputs using repository release records and full-file digests.
/**
 * 以仓库为依据的输入，不是浏览器输出已在设备运行的证据。
 * Repository-backed inputs, NOT evidence that browser output ran on a device.
 */
export const PROGRAM_REWRITER_SOURCE =
  "f23802979ceeeab8267b724b2d7f1a568b900777eb8718a0471a41317dc744ee";
export const REGISTERED_PROGRAM_RELEASE = Object.freeze({
  id: "switch-3.3.94012-touch-run-v4",
  gameVersion: "3.3.94012",
  titleId: "0100726014352000",
  ruleset: "touch-run-v4",
  sourceSha256: PROGRAM_REWRITER_SOURCE,
  realGameVerified: false,
  provenance: Object.freeze({
    inputHashes: "dm/tools/extract_game.sh:SUMS",
    moduleOrderAndRuleset: "dm/tools/d2rm_pack.py:MODS and patch",
    mainBuildId: "dm/tools/patch_nso.py:RULE_BUILD, CUTS, OBS, PARAMS",
  }),
  modules: Object.freeze([
    Object.freeze({
      name: "main",
      buildId: "db0507f36b84200ac05f75d76ed5026f",
      inputSha256: "55fd909bb7230b002357e5b0d8499c11f06888f345c49e69eb4734df44a27d0e",
    }),
    // 不虚构 SDK 构建 ID：仓库提供其完整文件哈希。
    // No SDK build ID is invented: the repository supplies its complete file hash.
    Object.freeze({
      name: "sdk",
      buildId: null,
      inputSha256: "1c9f198a91c0bcfee5cf8f5b53fb941895e91334dfb766ce79fc68576797f017",
    }),
  ]),
});
export const REGISTERED_PROGRAM_CATALOG = Object.freeze({
  schemaVersion: 2,
  scope: "repository-registered",
  sourceSha256: PROGRAM_REWRITER_SOURCE,
  versions: Object.freeze([REGISTERED_PROGRAM_RELEASE]),
});
// 这里只登记仓库审查过的完整模块组合；摘要匹配不是提取来源或运行验证。
// Register only repository-reviewed complete module tuples; digest matching is not extraction provenance or runtime proof.
export const REGISTERED_PROGRAM_MODULE_NAMES = Object.freeze(["main", "sdk"]);
const registeredById = new Map();
const registeredInputs = new Set();
for (const release of REGISTERED_PROGRAM_CATALOG.versions) {
  const inputKey = release.modules.map((module) => module.inputSha256).join(":");
  if (
    !/^[a-z0-9][a-z0-9.-]{1,127}$/.test(release.id) ||
    registeredById.has(release.id) ||
    registeredInputs.has(inputKey) ||
    release.sourceSha256 !== PROGRAM_REWRITER_SOURCE ||
    release.modules.map((module) => module.name).join(",") !==
      REGISTERED_PROGRAM_MODULE_NAMES.join(",") ||
    release.modules.some((module) => !/^[0-9a-f]{64}$/.test(module.inputSha256))
  )
    throw new Error("PROGRAM_CATALOG_BINDING");
  registeredById.set(release.id, release);
  registeredInputs.add(inputKey);
}

/**
 * 不接受调用方的目录；未知标识不回退到默认版本。
 * Accept no caller-owned catalog; unknown IDs never fall back to the default release.
 */
export function findRegisteredProgramRelease(releaseId) {
  return typeof releaseId === "string" ? (registeredById.get(releaseId) ?? null) : null;
}

/**
 * 只比较已计算的摘要，不读取输入、不授权重写；调用方仍须验证来源与实际字节。
 * Compare computed digests only, without reading inputs or authorizing rewrites; callers must still verify provenance and actual bytes.
 */
export function diagnoseRegisteredProgramDigests(digests) {
  if (
    !digests ||
    typeof digests !== "object" ||
    Object.keys(digests).sort().join(",") !==
      [...REGISTERED_PROGRAM_MODULE_NAMES].sort().join(",") ||
    REGISTERED_PROGRAM_MODULE_NAMES.some(
      (name) => typeof digests[name] !== "string" || !/^[0-9a-f]{64}$/.test(digests[name]),
    )
  )
    throw new Error("PROGRAM_INVALID_MODULE_SET");
  const release = REGISTERED_PROGRAM_CATALOG.versions.find((candidate) =>
    candidate.modules.every((module) => module.inputSha256 === digests[module.name]),
  );
  const partial = REGISTERED_PROGRAM_CATALOG.versions.some((candidate) =>
    candidate.modules.some((module) => module.inputSha256 === digests[module.name]),
  );
  return Object.freeze({
    status: release ? "registered" : partial ? "incompatible-modules" : "unsupported-version",
    releaseId: release?.id ?? null,
    gameVersion: release?.gameVersion ?? null,
  });
}

export const PROGRAM_REWRITE_LIMITS = Object.freeze({
  moduleInputBytes: 32 * 1024 * 1024,
  totalInputBytes: 64 * 1024 * 1024,
  segmentDecodedBytes: 64 * 1024 * 1024,
  imageBytes: 96 * 1024 * 1024,
  totalOutputBytes: 128 * 1024 * 1024,
});
