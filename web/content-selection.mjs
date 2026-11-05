// 根据元数据选择内容，不解密或应用补丁。
// 依据已解密 CNMT 记录及独立测量的 NCA 清单。
// Selects content from metadata without decryption or patch application.
// Based on decrypted CNMT records and independently measured NCA inventory.

/**
 * 仅按元数据选择。不处理密钥、NCA 解密或补丁应用，也不声称支持游戏。
 * Metadata-only selection. No keys, NCA decryption, patch application, or game support claim.
 */
export class ContentSelectionError extends Error {
  constructor(code, message) {
    super(message);
    this.name = "ContentSelectionError";
    this.code = code;
  }
}
const fail = (code, message) => {
  throw new ContentSelectionError(code, message);
};
const TYPES = { 128: "Application", 129: "Patch", 130: "AddOnContent" };
const MAX_BYTES = 16 * 1024 * 1024;
const MAX_RECORDS = 4096;
const hex = (bytes) => Array.from(bytes, (n) => n.toString(16).padStart(2, "0")).join("");
function uint(value, bits, field) {
  if (typeof value !== "bigint" && !(typeof value === "number" && Number.isSafeInteger(value)))
    fail("INVALID_INTEGER", `${field} must be an exact integer.`);
  const n = BigInt(value);
  if (n < 0n || n >= 1n << BigInt(bits))
    fail("INVALID_INTEGER", `${field} is outside uint${bits}.`);
  return n;
}
function digest(value, length, field) {
  if (typeof value !== "string" || !new RegExp(`^[0-9a-fA-F]{${length}}$`).test(value))
    fail("INVALID_IDENTITY", `${field} must be ${length} hexadecimal characters.`);
  return value.toLowerCase();
}
function content(c) {
  if (!c || typeof c !== "object") fail("INVALID_CONTENT", "Content record required.");
  const type = Number(uint(c.type, 8, "content type"));
  if (type < 1 || type > 5)
    fail(
      "UNSUPPORTED_CONTENT_TYPE",
      "Meta, delta fragments, and unknown content types are not selectable.",
    );
  return {
    contentId: digest(c.contentId, 32, "content ID"),
    sha256: digest(c.sha256, 64, "SHA-256"),
    size: uint(c.size, 40, "size"),
    type,
    idOffset: Number(uint(c.idOffset, 8, "ID offset")),
    attributes: Number(uint(c.attributes ?? 0, 8, "attributes")),
  };
}
function record(r) {
  if (!r || !Object.values(TYPES).includes(r.type))
    fail(
      "UNSUPPORTED_META_TYPE",
      "Only application, full patch, and add-on metadata are supported.",
    );
  if (!Array.isArray(r.contents) || !r.contents.length || r.contents.length > MAX_RECORDS)
    fail("INVALID_CONTENT", "A bounded nonempty content table is required.");
  if ((r.metaDependencies?.length ?? 0) !== 0)
    fail("UNSUPPORTED_DEPENDENCIES", "Content-meta dependencies are not resolved by this module.");
  const out = {
    type: r.type,
    titleId: uint(r.titleId, 64, "title ID"),
    version: uint(r.version, 32, "version"),
    applicationId: uint(r.applicationId, 64, "application ID"),
    requiredApplicationVersion: uint(
      r.requiredApplicationVersion ?? 0,
      32,
      "required application version",
    ),
    requiredSystemVersion: uint(r.requiredSystemVersion ?? 0, 32, "required system version"),
    contents: r.contents.map(content).sort((a, b) => a.contentId.localeCompare(b.contentId)),
  };
  if (r.cnmtDigest !== undefined) out.cnmtDigest = digest(r.cnmtDigest, 64, "CNMT digest");
  if (uint(r.contentAccessibilities ?? 0, 8, "content accessibilities") !== 0n)
    fail("UNSUPPORTED_DLC_ACCESS", "Non-default DLC accessibility is unsupported.");
  if (!out.titleId || !out.applicationId)
    fail("INVALID_IDENTITY", "Zero title/application IDs are not supported.");
  if (r.type === "Application") {
    if (out.titleId !== out.applicationId)
      fail("INVALID_ASSOCIATION", "Application must identify itself.");
    out.patchId = uint(r.patchId, 64, "patch ID");
  }
  if (r.type === "AddOnContent" && uint(r.dataPatchId ?? 0, 64, "data patch ID") !== 0n)
    fail("UNSUPPORTED_DATA_PATCH", "Add-on data patches are unsupported.");
  const ids = new Set();
  const slots = new Set();
  for (const c of out.contents) {
    const slot = `${c.type}:${c.idOffset}`;
    if (ids.has(c.contentId) || slots.has(slot))
      fail("DUPLICATE_CONTENT", "Duplicate content ID or type/ID-offset slot.");
    ids.add(c.contentId);
    slots.add(slot);
  }
  return out;
}

/**
 * 解析已解密、已封装的 CNMT 字节，而非 Meta NCA 或已安装数据库记录。
 * Parse decrypted, packaged CNMT bytes, not a Meta NCA or installed database record.
 */
export function parseCnmt(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength < 32 || bytes.byteLength > MAX_BYTES)
    fail("INVALID_CNMT_SIZE", "CNMT must be 32 bytes to 16 MiB.");
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const type = TYPES[v.getUint8(12)];
  if (!type) fail("UNSUPPORTED_META_TYPE", "Unsupported CNMT type.");
  if (v.getUint8(13) !== 0 || v.getUint8(22) !== 0)
    fail("UNSUPPORTED_PLATFORM", "Unsupported platform or partial install type.");
  const ext = v.getUint16(14, true),
    count = v.getUint16(16, true),
    metaCount = v.getUint16(18, true);
  const allowed = type === "Patch" ? [24] : type === "Application" ? [16] : [16, 24];
  if (!allowed.includes(ext) || 32 + ext > bytes.length)
    fail("INVALID_EXTENDED_HEADER", "Unexpected or truncated extended header.");
  if (!count || count > MAX_RECORDS || metaCount > MAX_RECORDS)
    fail("INVALID_CONTENT", "CNMT count exceeds supported bounds or is empty.");
  const extra = type === "Patch" ? v.getUint32(44, true) : 0;
  const end = 32 + ext + count * 56 + metaCount * 16 + extra;
  if (end + 32 !== bytes.length)
    fail("INVALID_CNMT_SIZE", "CNMT tables, extended data and digest do not exactly fit input.");
  if (metaCount) fail("UNSUPPORTED_DEPENDENCIES", "Content-meta dependencies are unsupported.");
  const titleId = v.getBigUint64(0, true);
  const contents = [],
    contentIds = new Set();
  let deltaFragmentCount = 0;
  for (let i = 0; i < count; i++) {
    const p = 32 + ext + i * 56;
    const entry = {
      sha256: hex(bytes.subarray(p, p + 32)),
      contentId: hex(bytes.subarray(p + 32, p + 48)),
      size: BigInt(v.getUint32(p + 48, true)) | (BigInt(v.getUint8(p + 52)) << 32n),
      attributes: v.getUint8(p + 53),
      type: v.getUint8(p + 54),
      idOffset: v.getUint8(p + 55),
    };
    if (contentIds.has(entry.contentId)) fail("DUPLICATE_CONTENT", "Duplicate content ID in CNMT.");
    contentIds.add(entry.contentId);
    if (entry.type === 6) {
      // 完整补丁内容存在时无需增量片段。
      // 它们可能共用 ID 偏移，因此排除在可选内容之外。
      // Delta fragments are not needed when the full patch content is present.
      // They may share an ID offset, so keep them out of selectable contents.
      if (type !== "Patch" || entry.attributes !== 0 || entry.idOffset !== 0)
        fail("UNSUPPORTED_CONTENT_TYPE", "Unsupported delta fragment metadata.");
      deltaFragmentCount++;
    } else contents.push(entry);
  }
  const parsed = record({
    type,
    titleId,
    version: BigInt(v.getUint32(8, true)),
    applicationId: type === "Application" ? titleId : v.getBigUint64(32, true),
    patchId: type === "Application" ? v.getBigUint64(32, true) : undefined,
    requiredApplicationVersion:
      type === "Application"
        ? v.getUint32(44, true)
        : type === "AddOnContent"
          ? v.getUint32(40, true)
          : 0,
    dataPatchId: type === "AddOnContent" && ext === 24 ? v.getBigUint64(48, true) : 0n,
    contentAccessibilities: type === "AddOnContent" && ext === 24 ? v.getUint8(44) : 0,
    requiredSystemVersion: type === "AddOnContent" ? 0 : v.getUint32(40, true),
    contents,
  });
  return {
    ...parsed,
    cnmtDigest: hex(bytes.subarray(end)),
    digestVerified: false,
    extendedDataSize: extra,
    deltaFragmentCount,
  };
}
// 显式 DLC 版本要求必须完整满足；先验证并快照，不能静默忽略未知标题或错误键名。
// Explicit DLC version requests must all be satisfied; validate and snapshot before selection, never silently ignore unknown titles or malformed keys.
function dlcVersionRequests(value) {
  const requested = new Map();
  if (value === undefined) return requested;
  if (
    !value ||
    typeof value !== "object" ||
    ![Object.prototype, null].includes(Object.getPrototypeOf(value))
  )
    fail("INVALID_INPUT", "DLC versions must be a plain keyed object.");
  const keys = Reflect.ownKeys(value);
  if (keys.length > MAX_RECORDS)
    fail("INVALID_INPUT", "DLC version request count exceeds supported bounds.");
  for (const id of keys) {
    if (typeof id !== "string" || !/^[0-9a-f]{16}$/.test(id))
      fail("INVALID_IDENTITY", "DLC version keys must be lowercase 16-character hexadecimal IDs.");
    const property = Object.getOwnPropertyDescriptor(value, id);
    if (!Object.hasOwn(property, "value"))
      fail("INVALID_INPUT", "DLC versions must be data properties.");
    requested.set(id, uint(property.value, 32, "DLC version"));
  }
  return requested;
}
const fingerprint = (value) =>
  JSON.stringify(value, (_, v) => (typeof v === "bigint" ? v.toString() : v));
function choose(candidates, requested, label) {
  const sorted = [...candidates].sort((a, b) =>
    a.version < b.version ? -1 : a.version > b.version ? 1 : 0,
  );
  if (requested !== undefined) {
    const version = uint(requested, 32, `${label} version`);
    const chosen = sorted.find((r) => r.version === version);
    if (!chosen) fail("MISSING_VERSION", `${label} version ${version} is absent.`);
    return chosen;
  }
  if (sorted.length > 1)
    fail(
      "AMBIGUOUS_VERSION",
      `${label} requires an explicit version: ${sorted.map((r) => r.version).join(", ")}. Newest does not mean supported.`,
    );
  return sorted[0];
}

/**
 * inventory 条目必须包含独立计算的完整 NCA SHA-256 和大小。
 * 返回的 DLC 保持独立；此函数绝不叠加 DLC 或应用补丁。
 * inventory entries must carry independently computed full-NCA SHA-256 and size.
 * Returned DLC stays separate; this function never overlays DLC or applies patches.
 */
export function resolveContentSelection(records, inventory, options = {}) {
  if (
    !Array.isArray(records) ||
    !records.length ||
    records.length > MAX_RECORDS ||
    !Array.isArray(inventory) ||
    inventory.length > MAX_RECORDS
  )
    fail("INVALID_INPUT", "Bounded metadata and inventory arrays are required.");
  if (!options || typeof options !== "object" || Array.isArray(options))
    fail("INVALID_INPUT", "Options must be an object.");
  const requestedDlcVersions = dlcVersionRequests(options.dlcVersions);
  const unique = new Map();
  for (const raw of records) {
    const r = record(raw),
      key = `${r.type}:${r.titleId}:${r.version}`;
    if (unique.has(key) && fingerprint(unique.get(key)) !== fingerprint(r))
      fail("CONFLICTING_METADATA", "Same title/type/version has conflicting metadata.");
    unique.set(key, r);
  }
  const all = [...unique.values()];
  const availableDlcIds = new Set(
    all
      .filter((r) => r.type === "AddOnContent")
      .map((r) => r.titleId.toString(16).padStart(16, "0")),
  );
  for (const id of requestedDlcVersions.keys())
    if (!availableDlcIds.has(id))
      fail("MISSING_VERSION", "An explicitly requested DLC title is absent.");
  const bases = all.filter((r) => r.type === "Application");
  if (!bases.length) fail("MISSING_BASE", "An application base is required.");
  if (new Set(bases.map((r) => r.titleId)).size !== 1)
    fail("AMBIGUOUS_APPLICATION", "Multiple application targets are present.");
  const base = choose(bases, options.baseVersion, "Base");
  for (const r of all) {
    if (r.applicationId !== base.titleId)
      fail("ORPHAN_CONTENT", "Patch or DLC belongs to another application.");
    if (r.type === "Patch" && r.titleId !== base.patchId)
      fail("INVALID_ASSOCIATION", "Patch ID differs from the application extended header.");
  }
  const patch =
    choose(
      all.filter((r) => r.type === "Patch"),
      options.patchVersion,
      "Patch",
    ) ?? null;
  const effectiveVersion = patch?.version ?? base.version;
  if (
    effectiveVersion < base.requiredApplicationVersion ||
    (patch && patch.version <= base.version)
  )
    fail("INCOMPATIBLE_VERSION", "Selected patch/base version is incompatible.");
  const dlcGroups = new Map();
  for (const r of all.filter((r) => r.type === "AddOnContent")) {
    const key = r.titleId.toString(16).padStart(16, "0");
    if (!dlcGroups.has(key)) dlcGroups.set(key, []);
    dlcGroups.get(key).push(r);
  }
  const dlc = [...dlcGroups]
    .sort(([a], [b]) => a.localeCompare(b))
    .map(([id, rows]) => choose(rows, requestedDlcVersions.get(id), `DLC ${id}`));
  for (const r of dlc)
    if (r.requiredApplicationVersion > effectiveVersion)
      fail("INCOMPATIBLE_DLC", "DLC requires a newer application version.");
  const available = new Map();
  for (const raw of inventory) {
    if (!raw || typeof raw !== "object")
      fail("INVALID_INPUT", "Inventory entry must be an object.");
    const c = {
      contentId: digest(raw.contentId, 32, "inventory content ID"),
      size: uint(raw.size, 40, "inventory size"),
    };
    if (available.has(c.contentId)) fail("CONFLICTING_CONTENT", "Duplicate inventory content ID.");
    available.set(c.contentId, c);
  }
  const required = new Map();
  for (const r of [base, patch, ...dlc].filter(Boolean))
    for (const c of r.contents) {
      const found = available.get(c.contentId);
      if (!found) fail("MISSING_CONTENT", `Required content ${c.contentId} is absent.`);
      if (found.size !== c.size)
        fail(
          "CONTENT_IDENTITY_MISMATCH",
          `Required content ${c.contentId} does not match CNMT size.`,
        );
      required.set(c.contentId, { contentId: c.contentId, sha256: c.sha256, size: c.size });
    }
  return {
    applicationId: base.titleId,
    base,
    patch,
    dlc,
    effectiveVersion,
    requiredContents: [...required.values()].sort((a, b) => a.contentId.localeCompare(b.contentId)),
    gameCompatibility: "unknown",
    requiresExecutableAllowlist: true,
    patchApplied: false,
  };
}
