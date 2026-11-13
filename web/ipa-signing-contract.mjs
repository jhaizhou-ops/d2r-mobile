// 本地签名的有界协议；诊断不包含证书、密码或路径。
// Bounded local signing protocol; diagnostics never contain certificates, passwords or paths.
export const IPA_SIGNING_LIMITS = Object.freeze({
  ipaBytes: 512 * 1024 * 1024,
  memberBytes: 256 * 1024 * 1024,
  identityBytes: 4 * 1024 * 1024,
  profileBytes: 4 * 1024 * 1024,
  passwordCharacters: 4096,
  wasmBytes: 64 * 1024 * 1024,
});
export const IPA_SIGNING_STAGES = Object.freeze(["inspect", "sign", "package", "verify"]);
export const IPA_SIGNING_ERROR_CODES = Object.freeze([
  "SIGNING_INITIALIZATION_FAILED",
  "SIGNING_INPUT_INVALID",
  "SIGNING_IDENTITY_INVALID",
  "SIGNING_PROFILE_INVALID",
  "SIGNING_PROFILE_MISMATCH",
  "SIGNING_APPLICATION_ID_TOO_LONG",
  "SIGNING_SIZE_LIMIT",
  "SIGNING_FAILED",
  "SIGNING_OUTPUT_INVALID",
  "SIGNING_WORKER_FAILED",
  "SIGNING_PROTOCOL_INVALID",
  "SIGNING_CLOSED",
  "SIGNING_TIMEOUT",
]);
export const signingFields = (value, fields) =>
  value !== null &&
  typeof value === "object" &&
  !Array.isArray(value) &&
  Object.keys(value).sort().join() === [...fields].sort().join();
export function signingError(code) {
  return new Error(IPA_SIGNING_ERROR_CODES.includes(code) ? code : "SIGNING_FAILED");
}
export function validSigningBlob(value) {
  return value instanceof Blob && value.size >= 22 && value.size <= IPA_SIGNING_LIMITS.ipaBytes;
}
export function validateSigningCredentials(value) {
  const buffer = (bytes, max) =>
    bytes instanceof ArrayBuffer &&
    !bytes.resizable &&
    bytes.byteLength > 0 &&
    bytes.byteLength <= max;
  return (
    signingFields(value, ["kind", "identity", "profile", "password"]) &&
    value.kind === "p12" &&
    buffer(value.identity, IPA_SIGNING_LIMITS.identityBytes) &&
    buffer(value.profile, IPA_SIGNING_LIMITS.profileBytes) &&
    value.identity !== value.profile &&
    typeof value.password === "string" &&
    !value.password.includes("\0") &&
    value.password.length <= IPA_SIGNING_LIMITS.passwordCharacters
  );
}
export function validateSigningProgress(value) {
  return (
    signingFields(value, ["id", "type", "stage", "completed", "total"]) &&
    value.id === 1 &&
    value.type === "progress" &&
    IPA_SIGNING_STAGES.includes(value.stage) &&
    Number.isSafeInteger(value.total) &&
    value.total >= 0 &&
    value.total <= IPA_SIGNING_LIMITS.ipaBytes &&
    Number.isSafeInteger(value.completed) &&
    value.completed >= 0 &&
    value.completed <= value.total
  );
}
export function clearSigningBuffers(values) {
  for (const value of values) {
    try {
      if (value instanceof ArrayBuffer) new Uint8Array(value).fill(0);
    } catch {
      // 已转移的缓冲区不能再访问。 | Transferred buffers are no longer accessible.
    }
  }
}
