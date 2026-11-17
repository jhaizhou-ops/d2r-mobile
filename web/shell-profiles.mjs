// 封闭的公开外壳配置，依据发布摘要及独立的宿主二进制检查。
// Closed public shell profiles, based on release digests and independent host inspection.
import {
  REGISTERED_PROGRAM_RELEASE,
  findRegisteredProgramRelease,
} from "./program-release-catalog.mjs";

import {
  V2_SHELL_PROFILE_IDS,
  V2_SHELL_CAPABILITIES,
  resolveV2ShellEvidence,
} from "./shell-v2-evidence.mjs";

export const DEFAULT_SHELL_PROFILE_ID = "v2.0.0-ios26";
export const IOS18_SHELL_PROFILE_ID = "v2.0.0-ios18";
export const BUNDLED_SHELL_PROFILE_IDS = V2_SHELL_PROFILE_IDS;
const releaseRecipe = {
  artRevision: V2_SHELL_CAPABILITIES.artRecipeRevision,
  overlayRevision: V2_SHELL_CAPABILITIES.overlayRecipeRevision,
  overlaySourceFingerprint: V2_SHELL_CAPABILITIES.overlayNativeSourceFingerprint,
};
function freeze(value) {
  if (value && typeof value === "object") {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}
const profiles = freeze({
  ...Object.fromEntries(
    V2_SHELL_PROFILE_IDS.map((id) => [
      id,
      {
        ...resolveV2ShellEvidence(id),
        programReleaseIds: [REGISTERED_PROGRAM_RELEASE.id],
        archivePath:
          id === DEFAULT_SHELL_PROFILE_ID ? "profile/shell.zip" : "profile/shell-ios18.zip",
        assetRecipe: releaseRecipe,
      },
    ]),
  ),
});
export const SHELL_PROFILE_IDS = Object.freeze(Object.keys(profiles));
for (const profile of Object.values(profiles)) {
  const executable = profile.evidence.entries.find(
    (entry) => entry.path === profile.capacities.evidence.executablePath,
  );
  if (
    !profile.programReleaseIds.length ||
    new Set(profile.programReleaseIds).size !== profile.programReleaseIds.length ||
    profile.programReleaseIds.some((id) => !findRegisteredProgramRelease(id)) ||
    profile.capacities.shellSha256 !== profile.evidence.sha256 ||
    !executable ||
    executable.sha256 !== profile.capacities.evidence.executableSha256 ||
    executable.sha256 !== profile.contentPin.originalSha256 ||
    executable.size !== profile.contentPin.size ||
    profile.capacities.evidence.executablePath !==
      `Payload/DmIOS.app/${profile.contentPin.executablePath}` ||
    profile.evidence.machOBuildVersions.length !== 1 ||
    profile.evidence.machOBuildVersions[0].platformId !== 2 ||
    profile.evidence.machOBuildVersions[0].minimumVersion !== profile.guestTarget.minimumVersion
  )
    throw new Error("SHELL_PROFILE_BINDING");
}

/**
 * 只接受内置配置标识；调用方不能提供摘要、偏移或目标版本。
 * Accept only built-in profile IDs; callers cannot supply hashes, offsets or target versions.
 */
export function resolveShellProfile(profileId = DEFAULT_SHELL_PROFILE_ID) {
  if (typeof profileId !== "string" || !Object.hasOwn(profiles, profileId))
    throw new Error("SHELL_PROFILE_UNKNOWN");
  return profiles[profileId];
}
