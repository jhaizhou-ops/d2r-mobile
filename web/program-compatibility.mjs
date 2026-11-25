// 从封闭目录导出版本与外壳诊断；不把目录就绪等同于新版本或设备验证。
// Derive version and shell diagnostics from closed catalogs, never equating catalog readiness with new-version or device verification.
import { REGISTERED_PROGRAM_CATALOG } from "./program-release-catalog.mjs";
import { BUNDLED_SHELL_PROFILE_IDS, resolveShellProfile } from "./shell-profiles.mjs";

export const PROGRAM_COMPATIBILITY = Object.freeze({
  catalogScope: "repository-registered",
  arbitraryGameVersions: false,
  arbitraryShellImports: false,
  browserNativeSourceRebuild: false,
  realGameVerified: false,
  releases: Object.freeze(
    REGISTERED_PROGRAM_CATALOG.versions.map((release) =>
      Object.freeze({ id: release.id, gameVersion: release.gameVersion, titleId: release.titleId }),
    ),
  ),
  shellProfiles: Object.freeze(
    BUNDLED_SHELL_PROFILE_IDS.map((id) => {
      const profile = resolveShellProfile(id);
      return Object.freeze({
        id,
        releaseId: profile.releaseId,
        minimumVersion: profile.guestTarget.minimumVersion,
        programReleaseIds: profile.programReleaseIds,
      });
    }),
  ),
});

const diagnosticCodes = new Set([
  "PROGRAM_UNSUPPORTED_VERSION",
  "PROGRAM_MODULE_RELEASE_MISMATCH",
  "PROGRAM_SHELL_RELEASE_MISMATCH",
  "PROGRAM_INVALID_MODULE_SET",
  "PROGRAM_INPUT_DIGEST_MISMATCH",
  "PROGRAM_BUILD_ID_MISMATCH",
]);

// 只转发固定代码；不暴露文件路径、输入摘要、构建 ID 或原始错误消息。
// Forward only fixed codes, never file paths, input hashes, build IDs or raw error messages.
export function isProgramCompatibilityDiagnosticCode(code) {
  return typeof code === "string" && diagnosticCodes.has(code);
}
