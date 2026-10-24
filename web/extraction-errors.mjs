// 提取诊断依据固定公开代码与浏览器专用文字表，绝不暴露原生异常或密钥及文件内容。
// Extraction diagnostics use fixed public codes and the browser-owned UI table, never native exceptions or key/file contents.
import { t } from "./i18n.mjs";
const workerCodes = new Set([
  "EXTRACTION_FAILED",
  "PROD_KEYS_MISSING",
  "PROD_KEYS_UNREADABLE",
  "PROD_KEYS_TOO_LARGE",
  "PROD_KEYS_EMPTY",
  "PROD_KEYS_MALFORMED",
  "TITLE_KEY_REQUIRED",
  "TITLE_KEYS_MISSING",
  "TITLE_KEYS_UNREADABLE",
  "TITLE_KEYS_TOO_LARGE",
  "TITLE_KEYS_MALFORMED",
  "KEYS_RUNTIME_FAILED",
  "PACKAGE_INDEX_INVALID",
  "TICKET_UNSUPPORTED",
  "TICKET_INVALID",
  "TICKET_RIGHTS_ID_MISMATCH",
  "TICKET_KEY_CONFLICT",
  "TICKET_LIMIT_EXCEEDED",
  "CONTENT_READ_FAILED",
  "CONTENT_AUTHENTICATION_FAILED",
  "CONTENT_METADATA_INVALID",
]);
const clientCodes = new Set([
  "EXTRACTED_INPUT_INVALID",
  "EXTRACTED_DLC_UNSUPPORTED",
  "EXTRACTED_RESOURCE_LAYOUT",
  "PROGRAM_UNSUPPORTED_VERSION",
  "PROGRAM_MODULE_RELEASE_MISMATCH",
  ...workerCodes,
  "EXTRACTION_WORKER_UNAVAILABLE",
  "EXTRACTION_TRANSPORT_FAILED",
  "EXTRACTION_PROTOCOL_FAILED",
  "EXTRACTION_WORKER_FAILED",
  "EXTRACTION_TIMEOUT",
]);
export const isExtractionWorkerErrorCode = (code) => workerCodes.has(code);
export const isExtractionDiagnosticCode = (code) => clientCodes.has(code);
export const extractionWorkerErrorCode = (error) =>
  isExtractionWorkerErrorCode(error?.code) ? error.code : "EXTRACTION_FAILED";
export function extractionDiagnosticError(code) {
  const safe = isExtractionWorkerErrorCode(code) ? code : "EXTRACTION_FAILED";
  const error = new Error(safe);
  error.code = safe;
  return error;
}

const progressLabels = Object.freeze({
  "prod-keys-read": "dmWebProgresslabelsProdKeysRead",
  "title-keys-read": "dmWebProgresslabelsTitleKeysRead",
  "keys-load": "dmWebProgresslabelsKeysLoad",
  "package-index": "dmWebProgresslabelsPackageIndex",
  "ticket-found": "dmWebProgresslabelsTicketFound",
  "ticket-key-added": "dmWebProgresslabelsTicketKeyAdded",
  "ticket-key-reused": "dmWebProgresslabelsTicketKeyReused",
  "content-hash": "dmWebProgresslabelsContentHash",
  "content-header": "dmWebProgresslabelsContentHeader",
  "content-metadata": "dmWebProgresslabelsContentMetadata",
  "exefs-open": "dmWebProgresslabelsExefsOpen",
  "exefs-read": "dmWebProgresslabelsExefsRead",
  "romfs-open": "dmWebProgresslabelsRomfsOpen",
  "romfs-index": "dmWebProgresslabelsRomfsIndex",
  "logical-index": "dmWebProgresslabelsLogicalIndex",
  "romfs-read": "dmWebProgresslabelsRomfsRead",
  "romfs-base": "dmWebProgresslabelsRomfsBase",
  "romfs-merged": "dmWebProgresslabelsRomfsMerged",
});
export function isExtractionProgress(value) {
  return Boolean(
    value &&
    typeof value === "object" &&
    Object.keys(value).sort().join(",") === "bytes,completed,stage,total" &&
    Object.hasOwn(progressLabels, value.stage) &&
    [value.bytes, value.completed, value.total].every((n) => Number.isSafeInteger(n) && n >= 0) &&
    value.completed <= value.total &&
    value.total <= 4096 &&
    (!value.stage.startsWith("romfs-") ||
      (value.stage === "romfs-read"
        ? value.completed === 0 && value.total === 0
        : value.total === 100)),
  );
}
export function extractionProgressLabel(value) {
  if (!isExtractionProgress(value)) return null;
  if (value.stage === "romfs-base" || value.stage === "romfs-merged")
    return t(
      "dmWebRomfsVerified",
      t(progressLabels[value.stage]),
      value.completed,
      (value.bytes / 1_000_000).toFixed(1),
    );
  const count = value.stage.startsWith("ticket-")
    ? t("dmWebProgressCount", value.completed)
    : value.total > 0
      ? t("dmWebProgressFraction", value.completed, value.total)
      : "";
  const size = value.bytes > 0 ? t("dmWebProgressSize", (value.bytes / 1_000_000).toFixed(1)) : "";
  return `${t(progressLabels[value.stage])}${count}${size}`;
}
