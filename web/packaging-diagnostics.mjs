// 浏览器打包诊断：依据固定错误代码与浏览器专用文字表，不显示私密输入。
// Browser packaging diagnostics based on fixed error codes and the browser-owned UI table, without private inputs.
import { t } from "./i18n.mjs";
import { APP_CUSTOMIZATION_ERROR_CODES } from "./app-customization.mjs";
import { isProgramCompatibilityDiagnosticCode } from "./program-compatibility.mjs";
import { sanitizeShaderDiagnostic } from "./shader-diagnostics.mjs";
import { isAirCompilerDiagnosticCode } from "./air-compiler-contract.mjs";
import { extractionProgressLabel, isExtractionDiagnosticCode } from "./extraction-errors.mjs";
const reasons = Object.freeze({
  EXTRACTED_INPUT_INVALID: "dmWebExtractedInputInvalid",
  EXTRACTED_DLC_UNSUPPORTED: "dmWebExtractedDlcUnsupported",
  EXTRACTED_RESOURCE_LAYOUT: "dmWebExtractedResourceLayout",
  ...Object.fromEntries(APP_CUSTOMIZATION_ERROR_CODES.map((code) => [code, "dmWebCustomInvalid"])),
  PROGRAM_UNSUPPORTED_VERSION: "dmWebProgramUnsupportedVersion",
  PROGRAM_MODULE_RELEASE_MISMATCH: "dmWebProgramModuleMismatch",
  PROGRAM_SHELL_RELEASE_MISMATCH: "dmWebProgramShellMismatch",
  PROGRAM_INVALID_MODULE_SET: "dmWebProgramInvalidModules",
  PROGRAM_INPUT_DIGEST_MISMATCH: "dmWebProgramDigestMismatch",
  PROGRAM_BUILD_ID_MISMATCH: "dmWebProgramBuildIdMismatch",
  PROD_KEYS_MISSING: "dmWebReasonsProdKeysMissing",
  PROD_KEYS_UNREADABLE: "dmWebReasonsProdKeysUnreadable",
  PROD_KEYS_TOO_LARGE: "dmWebReasonsProdKeysTooLarge",
  PROD_KEYS_EMPTY: "dmWebReasonsProdKeysEmpty",
  PROD_KEYS_MALFORMED: "dmWebReasonsProdKeysMalformed",
  TITLE_KEY_REQUIRED: "dmWebReasonsTitleKeyRequired",
  TITLE_KEYS_MISSING: "dmWebReasonsTitleKeysMissing",
  TITLE_KEYS_UNREADABLE: "dmWebReasonsTitleKeysUnreadable",
  TITLE_KEYS_TOO_LARGE: "dmWebReasonsTitleKeysTooLarge",
  TITLE_KEYS_MALFORMED: "dmWebReasonsTitleKeysMalformed",
  KEYS_RUNTIME_FAILED: "dmWebReasonsKeysRuntimeFailed",
  PACKAGE_INDEX_INVALID: "dmWebReasonsPackageIndexInvalid",
  TICKET_UNSUPPORTED: "dmWebReasonsTicketUnsupported",
  TICKET_INVALID: "dmWebReasonsTicketInvalid",
  TICKET_RIGHTS_ID_MISMATCH: "dmWebReasonsTicketRightsIdMismatch",
  TICKET_KEY_CONFLICT: "dmWebReasonsTicketKeyConflict",
  TICKET_LIMIT_EXCEEDED: "dmWebReasonsTicketLimitExceeded",
  CONTENT_READ_FAILED: "dmWebReasonsContentReadFailed",
  CONTENT_AUTHENTICATION_FAILED: "dmWebReasonsContentAuthenticationFailed",
  CONTENT_METADATA_INVALID: "dmWebReasonsContentMetadataInvalid",
  EXTRACTION_WORKER_UNAVAILABLE: "dmWebReasonsExtractionWorkerUnavailable",
  EXTRACTION_TRANSPORT_FAILED: "dmWebReasonsExtractionTransportFailed",
  EXTRACTION_PROTOCOL_FAILED: "dmWebReasonsExtractionProtocolFailed",
  EXTRACTION_WORKER_FAILED: "dmWebReasonsExtractionWorkerFailed",
  EXTRACTION_TIMEOUT: "dmWebReasonsExtractionTimeout",
  EXTRACTION_FAILED: "dmWebReasonsExtractionFailed",
});
const stages = Object.freeze({
  capabilities: "dmWebStagesCapabilities",
  discovery: "dmWebStagesDiscovery",
  secrets: "dmWebStagesSecrets",
  inspection: "dmWebStagesInspection",
  selection: "dmWebStagesSelection",
  extraction: "dmWebStagesExtraction",
  disposal: "dmWebStagesDisposal",
  program: "dmWebStagesProgram",
  customization: "dmWebStagesCustomization",
  assets: "dmWebStagesAssets",
  shaders: "dmWebStagesShaders",
  shell: "dmWebStagesShell",
  validation: "dmWebStagesValidation",
  assembly: "dmWebStagesAssembly",
  output: "dmWebStagesOutput",
  resources: "dmWebStagesResources",
  archive: "dmWebStagesArchive",
  cleanup: "dmWebStagesCleanup",
  commit: "dmWebStagesCommit",
});
// 只投影有界计数器；不转发导出器的目录名、路径或其他字段。
// Project bounded counters only; never forward exporter directory names, paths or other fields.
export function sanitizeResourceProgress(value) {
  if (!value || typeof value !== "object") return undefined;
  const bytes = (count) =>
    typeof count === "bigint" && count >= 0n && count <= BigInt(Number.MAX_SAFE_INTEGER)
      ? Number(count)
      : count;
  const progress = {
    filesWritten: value.filesWritten,
    totalFiles: value.totalFiles,
    bytesWritten: bytes(value.bytesWritten),
    totalBytes: bytes(value.totalBytes),
  };
  if (
    !Object.values(progress).every((count) => Number.isSafeInteger(count) && count >= 0) ||
    progress.filesWritten > progress.totalFiles ||
    progress.bytesWritten > progress.totalBytes
  )
    return undefined;
  return Object.freeze({
    ...progress,
    ...(value.operation === "verify" ? { operation: "verify" } : {}),
  });
}
// 高频资源计数仅显示在现有状态行；活动日志保留阶段事件。
// Frequent resource counters belong only in the existing status line; activity logs keep stage events.
export function resourceProgressLabel(event) {
  if (event?.stage !== "resources" || event.state !== "running") return null;
  const progress = sanitizeResourceProgress(event.resourceProgress);
  if (!progress) return null;
  return t(
    progress.operation === "verify" ? "dmWebResourceVerifyProgress" : "dmWebResourceProgress",
    progress.filesWritten,
    progress.totalFiles,
    (progress.bytesWritten / 1_000_000).toFixed(1),
    (progress.totalBytes / 1_000_000).toFixed(1),
  );
}
// 仅固定标签与有界数值进度进入活动日志。
// Only fixed labels and bounded numeric progress enter the activity log.
export function packagingActivity(event) {
  if (event?.phase === "cache")
    return event.status === "unavailable" ? t("dmWebCacheUnavailable") : null;
  const diagnostic = extractionProgressLabel(event?.diagnostic);
  if (diagnostic) return diagnostic;
  if (!Object.hasOwn(stages, event?.stage)) return null;
  const state = ["running", "complete", "failed"].includes(event.state) ? event.state : "running";
  return t(
    "dmWebStageState",
    t(stages[event.stage]),
    t({ running: "dmWebRunning", complete: "dmWebComplete", failed: "dmWebFailed" }[state]),
  );
}
// 只显示四个固定的补充包失败代码，绝不显示工作线程原始文本。
// Show only four fixed supplemental-pack failure codes, never raw worker text.
export function isLegacyAstcDiagnosticCode(code) {
  return ["STORAGE_QUOTA", "SIZE_LIMIT", "INTEGRITY_FAILED", "CODEC_FAILED"].includes(code);
}
export function packagingFailureSummary(report) {
  if (report?.code === "CANCELLED") return t("dmWebCancelled");
  // 先限定程序代码的阶段，避免通用提取代码集合放宽其范围。
  // Scope program codes before the generic extraction set can broaden their stages.
  const detailCode = report?.detailCode;
  const code = isProgramCompatibilityDiagnosticCode(detailCode)
    ? report?.stage === "program" ||
      (report?.stage === "secrets" &&
        ["PROGRAM_UNSUPPORTED_VERSION", "PROGRAM_MODULE_RELEASE_MISMATCH"].includes(detailCode))
      ? detailCode
      : null
    : isExtractionDiagnosticCode(detailCode) ||
        (report?.stage === "customization" && APP_CUSTOMIZATION_ERROR_CODES.includes(detailCode)) ||
        isLegacyAstcDiagnosticCode(detailCode) ||
        (report?.stage === "shaders" && isAirCompilerDiagnosticCode(detailCode))
      ? detailCode
      : null;
  const stage = Object.hasOwn(stages, report?.stage)
    ? t(stages[report.stage])
    : t("dmWebLocalPackaging");
  return t(
    "dmWebFailureSummary",
    stage,
    code ? ` [${code}]` : "",
    code && Object.hasOwn(reasons, code) ? `${t(reasons[code])} ` : "",
  );
}
// 仅使用生成的目标名称；不复制源文件名或任意嵌套报告。
// Generated destinations only; never copy source filenames or arbitrary nested reports.
export function sanitizeResourceOutcome(outcome) {
  if (!outcome || !["complete", "incomplete"].includes(outcome.status)) return undefined;
  const directoryName = outcome.directoryName;
  if (
    directoryName !== null &&
    (typeof directoryName !== "string" ||
      (directoryName !== "data" &&
        !/^D2R-resources-[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/i.test(
          directoryName,
        )))
  )
    return undefined;
  if (outcome.status === "complete")
    return directoryName ? Object.freeze({ status: "complete", directoryName }) : undefined;
  if (
    typeof outcome.cleanupComplete !== "boolean" ||
    typeof outcome.incompleteMarkerPresent !== "boolean"
  )
    return undefined;
  return Object.freeze({
    status: "incomplete",
    directoryName,
    cleanupComplete: outcome.cleanupComplete,
    incompleteMarkerPresent: outcome.incompleteMarkerPresent,
    ...(outcome.existingDataPreserved === true && directoryName === "data"
      ? { existingDataPreserved: true }
      : {}),
    ...([
      "RESOURCE_EXISTING_DATA_UNVERIFIED",
      "RESOURCE_EXPORT_BUSY",
      "RESOURCE_EXPORT_LOCK_UNAVAILABLE",
    ].includes(outcome.reason)
      ? { reason: outcome.reason }
      : {}),
  });
}
export function packagingFailureMessage(report) {
  const resources = sanitizeResourceOutcome(report?.resources);
  const resourcesSkipped =
    report?.resources?.status === "skipped" && report.resources.reason === "user-requested";
  // IPA 输出开始不表示已导出资源；保留明确跳过的独立结果。
  // Starting IPA output does not imply resource export; preserve the explicit skipped outcome.
  let output =
    report?.outputStarted === true && !resourcesSkipped
      ? t("dmWebOutputPossible")
      : t("dmWebOutputNone");
  if (resources?.existingDataPreserved) output = t("dmWebExistingDataUnverified");
  else if (resources?.reason === "RESOURCE_EXPORT_BUSY") output = t("dmWebResourceExportBusy");
  else if (resources?.reason === "RESOURCE_EXPORT_LOCK_UNAVAILABLE")
    output = t("dmWebResourceExportLockUnavailable");
  else if (resources?.status === "complete")
    output = t("dmWebOutputComplete", resources.directoryName);
  else if (resources?.cleanupComplete === true) output = t("dmWebOutputRemoved");
  else if (resources?.directoryName)
    output = t(
      "dmWebOutputIncomplete",
      resources.directoryName,
      t(resources.incompleteMarkerPresent ? "dmWebMarkerPresent" : "dmWebMarkerUnknown"),
    );
  return `${packagingFailureSummary(report)}${output}${t("dmWebFailureRetry")}`;
}

// 仅在已取回真实提交的下载后使用；不要根据报告标志推断下载可用。
// Use only after recovering an actual committed download, never from a report flag.
export function committedCleanupWarning() {
  return t("dmWebCommittedCleanupWarning");
}

// 着色器计数不含名称或路径，且不声明阶段完成。
// Shader counters contain no names or paths and never declare stage completion.
export function sanitizeShaderRecordProgress(value) {
  if (
    !value ||
    !Number.isSafeInteger(value.totalRecords) ||
    value.totalRecords < 1 ||
    value.totalRecords > 6927 ||
    !Number.isSafeInteger(value.completedRecords) ||
    value.completedRecords < 1 ||
    value.completedRecords > value.totalRecords
  )
    return undefined;
  return Object.freeze({
    completedRecords: value.completedRecords,
    totalRecords: value.totalRecords,
    ...(Number.isSafeInteger(value.compilerWorkers) &&
    value.compilerWorkers >= 1 &&
    value.compilerWorkers <= 4
      ? { compilerWorkers: value.compilerWorkers }
      : {}),
  });
}
export function compilerWorkerLabel(count) {
  return Number.isSafeInteger(count) && count >= 1 && count <= 4
    ? t("dmWebCompilerWorkers", count)
    : "";
}
export function shaderProgressLabel(event) {
  if (
    event?.stage !== "shaders" ||
    event.state !== "running" ||
    event.diagnostic?.kind !== "shader-records"
  )
    return null;
  const progress = sanitizeShaderRecordProgress(event.diagnostic);
  return progress
    ? t("dmWebStagesShaders") +
        t("dmWebProgressFraction", progress.completedRecords, progress.totalRecords) +
        compilerWorkerLabel(progress.compilerWorkers)
    : null;
}

// 失败后显示经过重新校验的阶段和编译器诊断，不复制原始错误对象。
// Display revalidated context and compiler diagnostics after failure, never raw error objects.
export function shaderFailureDetails(report) {
  if (report?.stage !== "shaders" || report?.code === "CANCELLED") return [];
  const detail = sanitizeShaderDiagnostic(report.shaderDiagnostic);
  if (!detail) return [];
  const context = [
    detail.substep,
    detail.compilerPhase,
    detail.errorType,
    detail.record ? `#${detail.record}` : undefined,
    detail.variant,
    detail.shaderStage,
  ]
    .filter(Boolean)
    .join(" / ");
  const lines = context ? [t("dmWebShaderFailureContext", context)] : [];
  if (detail.message) lines.push(detail.message);
  lines.push(...(detail.diagnostics ?? []));
  if (detail.sourceSha256) lines.push(`SHA-256: ${detail.sourceSha256}`);
  if (detail.sourceDigest) lines.push(t("dmWebShaderSourceDigest", detail.sourceDigest));
  return [...new Set(lines)];
}

// 文件夹错误只映射固定代码，不复制本地路径或解析器原文。
// Folder errors map fixed codes only, without local paths or raw parser messages.
export function gameFolderFailureMessage(code) {
  const keys = {
    MISSING_INPUT: "dmWebMissingInput",
    EXTRACTED_MISSING_INPUT: "dmWebExtractedMissingInput",
    EXTRACTED_DLC_UNSUPPORTED: "dmWebExtractedDlcUnsupported",
    EXTRACTED_NAME: "dmWebExtractedInvalidName",
    EXTRACTED_LIMIT: "dmWebExtractedLimit",
  };
  return t(
    typeof code === "string" && Object.hasOwn(keys, code) ? keys[code] : "dmWebFolderUnreadable",
  );
}

// 自定义失败不透露图标文件名、证书、输入值或内部错误文本。
// Customization failures expose no icon filenames, certificates, values or internal error text.
export function appCustomizationFailureMessage(error) {
  const keys = {
    APP_CUSTOMIZATION_NAME_INVALID: "dmWebCustomNameInvalid",
    APP_CUSTOMIZATION_BUNDLE_INVALID: "dmWebCustomBundleInvalid",
    APP_CUSTOMIZATION_VERSION_INVALID: "dmWebCustomVersionInvalid",
    APP_CUSTOMIZATION_BUILD_INVALID: "dmWebCustomBuildInvalid",
    APP_CUSTOMIZATION_ICON_INVALID: "dmWebCustomIconInvalid",
  };
  const code = error instanceof Error ? error.message : undefined;
  return t(
    typeof code === "string" && Object.hasOwn(keys, code) ? keys[code] : "dmWebCustomInvalid",
  );
}
