// 依据 NSP 输入校验、CNMT 选择与受信原生边界协调已认证内容读取。
// Coordinate authenticated content reads based on NSP input validation, CNMT selection and a trusted native boundary.
import { inspectKeyBytes, normalizeProdKeyBytes, NSP_INPUT_LIMITS } from "../../nsp-input.mjs";
import { inspectGamePackage } from "../../package-input.mjs";
import { extractionDiagnosticError } from "../../extraction-errors.mjs";
import { parseCnmt } from "../../content-selection.mjs";
import { D2R_WARLOCK_ID, resolveD2rExpansionSelection } from "../../d2r-expansion-policy.mjs";
const CHUNK = 65536,
  META_LIMIT = 16 * 1024 * 1024,
  MAX_CONTENTS = 4096;
const MAX_TICKETS = 4096;
const TICKET_BODY_SIZE = 0x180;
const HEX = "0123456789abcdef";
const TICKET_BODY_OFFSETS = new Map([
  [0x10000, 0x240],
  [0x10001, 0x140],
  [0x10002, 0x80],
  [0x10003, 0x240],
  [0x10004, 0x140],
  [0x10005, 0x80],
  [0x10006, 0x40],
]);

const TICKET_ERRORS = new Set([
  "TICKET_UNSUPPORTED",
  "TICKET_INVALID",
  "TICKET_RIGHTS_ID_MISMATCH",
  "TICKET_KEY_CONFLICT",
  "TICKET_LIMIT_EXCEEDED",
]);
function hexId(bytes) {
  let id = "";
  for (const byte of bytes) id += HEX[byte >>> 4] + HEX[byte & 15];
  return id;
}
// 文本已通过 inspectKeyBytes；只索引标识符，绝不索引密钥值。
// The text has already passed inspectKeyBytes. Index IDs, never key values.
function indexTitleIds(bytes) {
  const ids = new Set();
  for (let start = 0; start < bytes.length;) {
    let end = bytes.indexOf(10, start);
    if (end < 0) end = bytes.length;
    let at = start;
    while (at < end && (bytes[at] === 32 || bytes[at] === 9)) at++;
    if (at < end && bytes[at] !== 35 && bytes[at] !== 13) {
      let id = "";
      for (let i = 0; i < 32; i++) id += String.fromCharCode(bytes[at + i]);
      ids.add(id.toLowerCase());
    }
    start = end + 1;
  }
  return ids;
}
function ticketKeyLine(bytes, expectedId, hasExplicitKey) {
  if (bytes.length < 4) diagnostic("TICKET_INVALID");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const signature = view.getUint32(0, true);
  const body = TICKET_BODY_OFFSETS.get(signature);
  if (body === undefined) diagnostic("TICKET_UNSUPPORTED");
  if (bytes.length < body + TICKET_BODY_SIZE || bytes[body + 0x140] !== 2)
    diagnostic("TICKET_INVALID");
  const type = bytes[body + 0x141];
  if (type > 1) diagnostic("TICKET_INVALID");
  const id = bytes.subarray(body + 0x160, body + 0x170);
  if (id.every((byte) => byte === 0)) diagnostic("TICKET_INVALID");
  if (hexId(id) !== expectedId) diagnostic("TICKET_RIGHTS_ID_MISMATCH");
  if (signature !== 0x10004 || type !== 0) {
    if (hasExplicitKey) return null;
    diagnostic("TICKET_UNSUPPORTED");
  }
  const key = bytes.subarray(body + 0x40, body + 0x50);
  if (key.every((byte) => byte === 0)) diagnostic("TICKET_INVALID");
  if (hasExplicitKey) return null;
  const line = new Uint8Array(68);
  const writeHex = (source, at) => {
    for (const byte of source) {
      line[at++] = HEX.charCodeAt(byte >>> 4);
      line[at++] = HEX.charCodeAt(byte & 15);
    }
  };
  writeHex(id, 0);
  line.set([32, 61, 32], 32);
  writeHex(key, 35);
  line[67] = 10;
  return line;
}
function mergeTicketKeys(title, tickets) {
  const additional = [...tickets.values()];
  if (!additional.length) return title;
  const separator = title.length && title[title.length - 1] !== 10 ? 1 : 0;
  const size = title.length + separator + additional.length * 68;
  if (size > NSP_INPUT_LIMITS.keyBytes) diagnostic("TITLE_KEYS_TOO_LARGE");
  const merged = new Uint8Array(size);
  merged.set(title);
  if (separator) merged[title.length] = 10;
  let at = title.length + separator;
  for (const line of additional) {
    merged.set(line, at);
    at += line.length;
  }
  title.fill(0);
  return merged;
}
const fail = () => {
  throw new Error("EXTRACTION_REQUEST_REJECTED");
};
const diagnostic = (code) => {
  throw extractionDiagnosticError(code);
};
const checked = (code, operation) => {
  try {
    return operation();
  } catch (error) {
    diagnostic(error?.code === "TITLE_KEY_REQUIRED" ? "TITLE_KEY_REQUIRED" : code);
  }
};
async function readKeys(file, kind) {
  if (kind === "title" && file == null) return new Uint8Array();
  const prefix = kind === "prod" ? "PROD_KEYS" : "TITLE_KEYS";
  if (!(file instanceof Blob)) diagnostic(`${prefix}_MISSING`);
  if (file.size > NSP_INPUT_LIMITS.keyBytes) diagnostic(`${prefix}_TOO_LARGE`);
  let bytes;
  try {
    bytes = new Uint8Array(await file.arrayBuffer());
    if (bytes.length !== file.size) diagnostic(`${prefix}_UNREADABLE`);
  } catch {
    bytes?.fill(0);
    diagnostic(`${prefix}_UNREADABLE`);
  }
  try {
    if (kind === "prod") bytes = normalizeProdKeyBytes(bytes);
    else inspectKeyBytes(bytes, kind);
    return bytes;
  } catch (error) {
    bytes.fill(0);
    diagnostic(error?.code === "KEY_EMPTY" ? "PROD_KEYS_EMPTY" : `${prefix}_MALFORMED`);
  }
}
const bounded = (offset, count, size) => {
  if (
    typeof offset !== "bigint" ||
    offset < 0n ||
    offset > size ||
    !Number.isInteger(count) ||
    count < 0 ||
    count > CHUNK ||
    BigInt(count) > size - offset
  )
    fail();
};
function fields(value, allowed) {
  if (
    !value ||
    typeof value !== "object" ||
    Array.isArray(value) ||
    Object.keys(value).some((key) => !allowed.includes(key))
  )
    fail();
}
/** 专用 Worker 引擎。原生边界是受信能力，绝不是 RPC 输入。
 * 此对象仅向外提供读取能力；真实内容的覆盖情况仍未证实。
 * Dedicated-Worker engine. Native boundary is a trusted capability, never RPC input.
 * Only read capabilities leave this object. Real-content coverage remains unproven. */
export function createExtractionEngine(
  boundary,
  { newToken = () => crypto.randomUUID(), onProgress = () => {} } = {},
) {
  const progress = (stage, completed = 0, total = 0, bytes = 0) => {
    try {
      // 观察异步拒绝，但不等待诊断或阻塞同步原生验证。
      // Observe async rejection without awaiting diagnostics or blocking synchronous native verification.
      Promise.resolve(onProgress(Object.freeze({ stage, completed, total, bytes }))).catch(
        () => {},
      );
    } catch {
      // 诊断不能改变提取结果。
      // Diagnostics cannot change the extraction result.
    }
  };
  let state = "new",
    token,
    contents = new Map(),
    warlockHashes = new Map(),
    records = [],
    inventory = [],
    selection;
  let entries = [],
    romfs,
    baseIndex,
    programIndex;
  const revoke = () => {
    token = newToken();
    return token;
  };
  const requireState = (...states) => {
    if (!states.includes(state)) fail();
  };
  const checkToken = (value) => {
    if (typeof value !== "string" || value !== token) fail();
  };
  const member = async (index) => {
    const info = boundary.entry(index);
    if (info.size > BigInt(META_LIMIT)) fail();
    const bytes = new Uint8Array(Number(info.size));
    try {
      for (let offset = 0; offset < bytes.length; offset += CHUNK) {
        const part = boundary.read(index, BigInt(offset), Math.min(CHUNK, bytes.length - offset));
        try {
          bytes.set(part, offset);
        } finally {
          part.fill(0);
        }
      }
      return parseCnmt(bytes);
    } finally {
      bytes.fill(0);
    }
  };
  return {
    async execute(operation, payload = {}) {
      fields(
        payload,
        operation === "initialize"
          ? ["nsps", "prodKeys", "titleKeys"]
          : operation === "select"
            ? ["options"]
            : operation === "openRomfs"
              ? ["token", "verifyRomfs"]
              : operation === "readExefs"
                ? ["token", "index", "offset", "count"]
                : operation === "readRomfs"
                  ? ["token", "offset", "count"]
                  : [],
      );
      if (operation === "initialize") {
        requireState("new");
        state = "initializing";
        if (
          !Array.isArray(payload.nsps) ||
          !payload.nsps.length ||
          payload.nsps.length > NSP_INPUT_LIMITS.packages ||
          payload.nsps.some((file) => !(file instanceof Blob))
        )
          fail();
        const blobs = [],
          ids = [],
          tickets = new Map();
        let prod, title, explicitIds;
        let ticketCount = 0,
          addedCount = 0,
          reusedCount = 0;
        try {
          progress("prod-keys-read");
          prod = await readKeys(payload.prodKeys, "prod");
          progress("title-keys-read");
          title = await readKeys(payload.titleKeys, "title");
          explicitIds = indexTitleIds(title);
          for (const [index, file] of payload.nsps.entries()) {
            progress("package-index", index, payload.nsps.length);
            let catalog;
            try {
              catalog = await inspectGamePackage(file);
            } catch {
              diagnostic("PACKAGE_INDEX_INVALID");
            }
            for (const entry of catalog.entries) {
              const match = /^([0-9a-f]{32})(?:\.cnmt)?\.nca$/iu.exec(entry.name);
              if (!match) {
                const ticket = /^([0-9a-f]{32})\.tik$/iu.exec(entry.name);
                if (ticket) {
                  if (++ticketCount > MAX_TICKETS) diagnostic("TICKET_LIMIT_EXCEEDED");
                  progress("ticket-found", ticketCount, ticketCount);
                  if (entry.size < 4n || entry.size > 4096n) diagnostic("TICKET_INVALID");
                  let bytes, line;
                  try {
                    bytes = new Uint8Array(
                      await file
                        .slice(Number(entry.offset), Number(entry.offset + entry.size))
                        .arrayBuffer(),
                    );
                    if (BigInt(bytes.length) !== entry.size) diagnostic("TICKET_INVALID");
                    const id = ticket[1].toLowerCase();
                    line = ticketKeyLine(bytes, id, explicitIds.has(id));
                    if (line === null) {
                      progress("ticket-key-reused", ++reusedCount, reusedCount);
                      continue;
                    }
                    const old = tickets.get(id);
                    if (old && old.some((byte, i) => byte !== line[i]))
                      diagnostic("TICKET_KEY_CONFLICT");
                    if (!old) {
                      const separator = title.length && title[title.length - 1] !== 10 ? 1 : 0;
                      if (
                        title.length + separator + (tickets.size + 1) * 68 >
                        NSP_INPUT_LIMITS.keyBytes
                      )
                        diagnostic("TICKET_LIMIT_EXCEEDED");
                      tickets.set(id, line);
                      line = undefined;
                      progress("ticket-key-added", ++addedCount, addedCount);
                    } else progress("ticket-key-reused", ++reusedCount, reusedCount);
                  } catch (error) {
                    diagnostic(TICKET_ERRORS.has(error?.code) ? error.code : "TICKET_INVALID");
                  } finally {
                    bytes?.fill(0);
                    line?.fill(0);
                  }
                } else if (/\.tik$/iu.test(entry.name)) diagnostic("TICKET_INVALID");
                else if (/\.(?:nca|ncz)$/iu.test(entry.name)) diagnostic("PACKAGE_INDEX_INVALID");
                continue;
              }
              if (blobs.length >= MAX_CONTENTS) diagnostic("PACKAGE_INDEX_INVALID");
              blobs.push(file.slice(Number(entry.offset), Number(entry.offset + entry.size)));
              ids.push(match[1].toLowerCase());
            }
          }
          title = mergeTicketKeys(title, tickets);
          progress("keys-load");
          checked("KEYS_RUNTIME_FAILED", () => boundary.loadKeys(prod, title));
        } finally {
          prod?.fill(0);
          title?.fill(0);
          for (const line of tickets.values()) line.fill(0);
          tickets.clear();
          explicitIds?.clear();
        }
        if (!blobs.length) diagnostic("PACKAGE_INDEX_INVALID");
        checked("CONTENT_READ_FAILED", () => boundary.mount(blobs));
        for (let i = 0; i < blobs.length; i++) {
          let sha256 = null;
          progress("content-header", i, blobs.length);
          const header = checked("CONTENT_AUTHENTICATION_FAILED", () => boundary.inspect(i));
          const warlock = header.applicationId === D2R_WARLOCK_ID.toString(16).padStart(16, "0");
          if (
            header.size !== BigInt(blobs[i].size) ||
            (!header.fixedSignatureValid && (!warlock || ![1, 5].includes(header.contentType)))
          )
            diagnostic("CONTENT_AUTHENTICATION_FAILED");
          // 这个仅用于解锁的小型附加内容可能带有重新打包的 NCA 头。
          // 即使在快速模式下，也始终计算两个文件的摘要并将其绑定到各自的内容 ID；
          // 基础游戏与更新的可执行内容头仍必须具有有效签名。
          // The small, unlock-only add-on can have repacked NCA headers. Always
          // measure both files and bind each digest to its content ID, even in
          // fast mode; executable base/update headers still require signatures.
          if (warlock) {
            if (blobs[i].size > META_LIMIT || ![1, 5].includes(header.contentType))
              diagnostic("CONTENT_AUTHENTICATION_FAILED");
            progress("content-hash", i, blobs.length, blobs[i].size);
            sha256 = checked("CONTENT_READ_FAILED", () => boundary.hashInput(i));
            if (sha256.slice(0, 32) !== ids[i]) diagnostic("CONTENT_AUTHENTICATION_FAILED");
            warlockHashes.set(ids[i], sha256);
          }
          const record = {
            contentId: ids[i],
            sha256,
            size: header.size,
          };
          const old = contents.get(ids[i]);
          if (old) diagnostic("CONTENT_METADATA_INVALID");
          contents.set(ids[i], { ...record, index: i, ncaType: header.contentType });
          inventory.push(record);
          if (header.contentType === 1) {
            progress("content-metadata", i, blobs.length);
            const count = checked("CONTENT_AUTHENTICATION_FAILED", () =>
              boundary.open(i, !header.fixedSignatureValid ? "warlock-meta" : "meta"),
            );
            try {
              const candidates = [];
              for (let j = 0; j < count; j++)
                if (boundary.entry(j).name.endsWith(".cnmt")) candidates.push(j);
              if (candidates.length !== 1) fail();
              const record = await member(candidates[0]);
              if (
                !header.fixedSignatureValid &&
                (record.type !== "AddOnContent" || record.titleId !== D2R_WARLOCK_ID)
              )
                diagnostic("CONTENT_METADATA_INVALID");
              records.push(record);
            } catch {
              diagnostic("CONTENT_METADATA_INVALID");
            }
          }
        }
        if (!records.length) diagnostic("CONTENT_METADATA_INVALID");
        state = "inspected";
        return { records, inventory };
      }
      if (operation === "select") {
        requireState("inspected");
        fields(payload.options ?? {}, [
          "baseVersion",
          "patchVersion",
          "dlcVersions",
          "contentMode",
        ]);
        selection = resolveD2rExpansionSelection(records, inventory, payload.options);
        // 要求已知附加内容的完整实测 NCA 证据与 Data 头部。
        // 这是包输入校验，并非授权或所有权检查。
        // 仅用于解锁的内容绝不会替代 Program RomFS。
        // Require the known add-on's full measured NCA evidence and Data
        // header. This is package-input validation, not entitlement/ownership.
        // Its unlock-only content is never substituted for Program RomFS.
        for (const addOn of selection.dlc)
          for (const content of addOn.contents)
            if (
              contents.get(content.contentId)?.ncaType !== 5 ||
              warlockHashes.get(content.contentId) !== content.sha256
            )
              diagnostic("CONTENT_AUTHENTICATION_FAILED");
        const program = (row) => {
          const found = row.contents.filter((c) => c.type === 1);
          if (found.length !== 1 || found[0].idOffset !== 0) fail();
          return contents.get(found[0].contentId).index;
        };
        baseIndex = program(selection.base);
        programIndex = selection.patch ? program(selection.patch) : baseIndex;
        progress("exefs-open");
        const count = boundary.open(programIndex, "program-exefs");
        entries = Array.from({ length: count }, (_, index) => ({
          index,
          ...boundary.entry(index),
        }));
        state = "exefs";
        return { selection, token: revoke(), entries };
      }
      if (operation === "readExefs") {
        requireState("exefs");
        checkToken(payload.token);
        if (
          !Number.isInteger(payload.index) ||
          payload.index < 0 ||
          payload.index >= entries.length
        )
          fail();
        bounded(payload.offset, payload.count, entries[payload.index].size);
        return boundary.read(payload.index, payload.offset, payload.count);
      }
      if (operation === "openRomfs") {
        requireState("exefs");
        checkToken(payload.token);
        if (typeof payload.verifyRomfs !== "boolean") fail();
        // 浏览器不执行完整内容摘要扫描；独立调用者仍可要求 RomFS 校验。
        // The browser omits whole-content hashing; independent callers may still verify RomFS.
        revoke();
        const reportVerification = ({ phase, completed, total }) => {
          if (phase === "read") {
            progress("romfs-read", 0, 0, completed);
            return;
          }
          if ((phase === "base" || phase === "merged") && total > 0)
            progress(
              phase === "base" ? "romfs-base" : "romfs-merged",
              Math.min(100, Math.floor((completed / total) * 100)),
              100,
              completed,
            );
        };
        romfs = selection.patch
          ? boundary.openPatchedRomfs(
              baseIndex,
              programIndex,
              reportVerification,
              payload.verifyRomfs,
            )
          : boundary.openRomfs(baseIndex, undefined, reportVerification, payload.verifyRomfs);
        state = "romfs";
        entries = [];
        return { token, size: romfs.size };
      }
      if (operation === "readRomfs") {
        requireState("romfs");
        checkToken(payload.token);
        bounded(payload.offset, payload.count, romfs.size);
        return romfs.readAt(payload.offset, payload.count);
      }
      if (operation === "dispose") {
        if (state !== "disposed") boundary.dispose();
        state = "disposed";
        revoke();
        contents.clear();
        warlockHashes.clear();
        records = [];
        inventory = [];
        entries = [];
        romfs = null;
        selection = null;
        return { disposed: true };
      }
      fail();
    },
  };
}
