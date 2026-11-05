// 精确的 v2 外壳证据；配置注册表单独绑定已审查的资源配方。
// Exact v2 shell evidence. The profile registry binds the separately reviewed asset recipe.
import defaultEvidence from "./toolchain/shell-release-evidence/v2.0.0.json" with { type: "json" };
import legacyEvidence from "./toolchain/shell-release-evidence/v2.0.0-ios18.json" with { type: "json" };
import defaultCapacities from "./toolchain/shell-release-evidence/v2.0.0-derived-capacities.json" with { type: "json" };
import legacyCapacities from "./toolchain/shell-release-evidence/v2.0.0-ios18-derived-capacities.json" with { type: "json" };

export const V2_SHELL_RELEASE_SOURCE = "b811b90172f4eb40ec1bc89d903308e0c5ab88d0";

function freeze(value) {
  if (value && typeof value === "object") {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}

// 此模块只保存证据；配置、界面和下载集合由封闭注册表控制。
// This module stores evidence only; the closed registry controls profiles, UI and downloads.
const candidates = freeze({
  "v2.0.0-ios26": {
    id: "v2.0.0-ios26",
    releaseId: "v2.0.0",
    evidence: defaultEvidence,
    capacities: defaultCapacities,
    guestTarget: { platform: "ios", minimumVersion: "26.0.0", sdkVersion: "26.0.0" },
    contentPin: {
      release: "v2.0.0",
      executablePath: "DmIOS",
      size: 2119120,
      originalSha256: "ee65b6c36bfb362af4fcf5661429e34b6097f14af5b73d8cd7e72016f12c68ce",
      noDlcSha256: "f25e60982514ce34d0603ba8d1e7e6412947baf27ef553f11bcb08f2ae4f615a",
      symbol: "_aoc_owned.v",
      address: 0x1001a71e8n,
      fileOffset: 0x1a71e8,
    },
  },
  "v2.0.0-ios18": {
    id: "v2.0.0-ios18",
    releaseId: "v2.0.0",
    evidence: legacyEvidence,
    capacities: legacyCapacities,
    // 保留手写 Guest 格式的 SDK 字段；宿主实测 SDK 27.1 单独记录在证据中。
    // Preserve the handwritten Guest SDK field; observed host SDK 27.1 is separate evidence.
    guestTarget: { platform: "ios", minimumVersion: "18.0.0", sdkVersion: "26.0.0" },
    contentPin: {
      release: "v2.0.0",
      executablePath: "DmIOS",
      size: 2100168,
      originalSha256: "1ab0bcacc8cde2a691ec17d70f7ca6da7e45bee563c10cf0c7ee50c67fac32c4",
      noDlcSha256: "6056f821d90b2553a00bb0148220aabede4f082891a17a53deaebb86b5ae38f4",
      symbol: "_aoc_owned.v",
      address: 0x1001a3058n,
      fileOffset: 0x1a3058,
    },
  },
});

export const V2_SHELL_PROFILE_IDS = Object.freeze(Object.keys(candidates));
export const V2_SHELL_CAPABILITIES = freeze({
  status: "source-reviewed-not-device-validated",
  releaseSourceCommit: V2_SHELL_RELEASE_SOURCE,
  overlayRecipeRevision: V2_SHELL_RELEASE_SOURCE,
  artRecipeRevision: V2_SHELL_RELEASE_SOURCE,
  overlayNativeSourceFingerprint:
    "243ab3eec668192415561f806ebbf2e670ea5a8a83aee8b72f92021a23989763",
  matchingReleaseAssetRecipeRequired: true,
  independentCapacityReviewRequired: true,
  completeShaderSourceDigestRequired: true,
  reflectionAndCompiledLibraryValidationRequired: true,
  // e3756f8 的可选功能不能从 b811 发布壳推断。
  // Optional later-source capabilities cannot be inferred from the b811 release shells.
  laterPlayStationGlyphsSupported: false,
  laterIosVp8Supported: false,
  deviceExecutionVerified: false,
});

for (const candidate of Object.values(candidates)) {
  const { evidence, capacities, contentPin, guestTarget } = candidate;
  const executable = evidence.entries.find(
    (entry) => entry.path === capacities.evidence.executablePath,
  );
  const contentEvidence = evidence.inspection.contentMode;
  if (
    evidence.releaseTarget !== V2_SHELL_RELEASE_SOURCE ||
    capacities.evidence.sourceCommit !== V2_SHELL_RELEASE_SOURCE ||
    capacities.shellSha256 !== evidence.sha256 ||
    !executable ||
    executable.sha256 !== capacities.evidence.executableSha256 ||
    executable.sha256 !== contentPin.originalSha256 ||
    executable.size !== contentPin.size ||
    capacities.evidence.executablePath !== `Payload/DmIOS.app/${contentPin.executablePath}` ||
    evidence.machOBuildVersions.length !== 1 ||
    evidence.machOBuildVersions[0].platformId !== 2 ||
    evidence.machOBuildVersions[0].minimumVersion !== guestTarget.minimumVersion ||
    contentEvidence.sourceCommit !== V2_SHELL_RELEASE_SOURCE ||
    contentEvidence.symbol !== contentPin.symbol ||
    BigInt(contentEvidence.address) !== contentPin.address ||
    contentEvidence.fileOffset !== contentPin.fileOffset ||
    contentEvidence.originalFourBytes !== "ffffffff" ||
    contentEvidence.replacementFourBytes !== "00000000" ||
    contentEvidence.derivedSha256 !== contentPin.noDlcSha256
  )
    throw new Error("SHELL_V2_EVIDENCE_BINDING");
}

/**
 * 仅访问信息证据，不会将候选项加入任何生产路径。
 * Evidence access only. This does not opt a candidate into any production path. */
export function resolveV2ShellEvidence(id) {
  if (typeof id !== "string" || !Object.hasOwn(candidates, id))
    throw new Error("SHELL_V2_EVIDENCE_UNKNOWN");
  return candidates[id];
}
