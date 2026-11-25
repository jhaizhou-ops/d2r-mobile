// 指引只描述已提交输出，不描述请求的选项或部分导出路径。
// Instructions describe committed outputs only, never requested options or partial export paths.
const completeDirectory = (outcome) =>
  outcome?.status === "complete" &&
  typeof outcome.directoryName === "string" &&
  outcome.directoryName.trim() &&
  !/[\\/\0]/.test(outcome.directoryName)
    ? outcome.directoryName
    : null;

/**
 * @param {{ resources?: { status?: string, directoryName?: string }, legacyPack?: { status?: string, directoryName?: string } } | null | undefined} result
 * @param {string} appName The actual identity of this output, including original-identity recovery.
 */
export function createPostGenerationGuide(result, appName) {
  if (typeof appName !== "string" || !appName.trim()) return null;
  const resourcesDirectory = completeDirectory(result?.resources);
  const legacyDirectory = completeDirectory(result?.legacyPack);
  return Object.freeze({
    appName,
    resourcesDirectory,
    resourcesStatus: resourcesDirectory
      ? "complete"
      : result?.resources?.status === "skipped"
        ? "skipped"
        : "incomplete",
    legacyDirectory,
    legacyIncomplete:
      !legacyDirectory && ["complete", "incomplete"].includes(result?.legacyPack?.status ?? ""),
  });
}
