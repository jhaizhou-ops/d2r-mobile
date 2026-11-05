// 依据 dm/tools/dmx.py 清单语义和已认证 RomFS 索引读取逻辑资源。
// Read logical assets using dm/tools/dmx.py manifest semantics and an authenticated RomFS index.
/**
 * 单个已认证且不可变的 RomFS 索引上的惰性逻辑资源。
 * Lazy logical assets over one authenticated, immutable RomFS index.
 */
import { archiveLocator, ARCHIVE_LIMITS, decodeArchiveRecord } from "./archive-reader.mjs";
import { verifyMd5 } from "./checksum-verifier.mjs";
import { ROMFS_LIMITS } from "./romfs-tree.mjs";

export const CASC_LIMITS = Object.freeze({
  manifestBytes: 128 * 1024 * 1024,
  lineBytes: 16 * 1024,
  rows: 1000000,
  pathBytes: 4096,
});
const utf8 = new TextEncoder();
const fail = (message) => {
  throw new Error(`Invalid CASC: ${message}`);
};
function checkPath(path, request = false) {
  if (
    typeof path !== "string" ||
    !path ||
    path.length > CASC_LIMITS.pathBytes ||
    utf8.encode(path).length > CASC_LIMITS.pathBytes
  )
    fail("logical path");
  const normalized = request ? path.replaceAll("\\", "/") : path;
  const tail =
    normalized.startsWith("data:") || normalized.startsWith("DATA:")
      ? normalized.slice(5)
      : normalized.replace(/^data:/i, "");
  if (
    // eslint-disable-next-line no-control-regex
    /[\\:\u0000-\u001f\u007f]/u.test(tail) ||
    tail.split("/").some((p) => !p || p === "." || p === "..")
  )
    fail("unsafe logical path");
  return normalized.toLowerCase();
}
function uint(text, name) {
  if (!/^\d+$/.test(text)) fail(name);
  const value = BigInt(text);
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) fail(name);
  return Number(value);
}
function reader(file, name) {
  if (!file || typeof file.size !== "bigint" || file.size < 0n || typeof file.readAt !== "function")
    fail(`missing or invalid ${name}`);
  return { size: file.size, readAt: file.readAt.bind(file) };
}
async function exactRead(file, offset, count, signal) {
  signal?.throwIfAborted();
  if (offset < 0n || offset > file.size || BigInt(count) > file.size - offset) fail("file extent");
  const bytes = await file.readAt(offset, count, { signal });
  signal?.throwIfAborted();
  if (!(bytes instanceof Uint8Array) || bytes.length !== count) fail("short read");
  return bytes.slice();
}

async function scanManifest(getFile, accept, signal, nativeAstc = false) {
  const manifest = reader(getFile("data/build_manifest.psv"), "build manifest");
  if (manifest.size > BigInt(CASC_LIMITS.manifestBytes)) fail("manifest byte budget");
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let pending = "",
    first = true,
    rowCount = 0;
  function line(text) {
    if (text.endsWith("\r")) text = text.slice(0, -1);
    if (text.length > CASC_LIMITS.lineBytes || utf8.encode(text).length > CASC_LIMITS.lineBytes)
      fail("manifest line budget");
    if (first) {
      first = false;
      // dmx 跳过首行；原生 ASTC 将所有行交给路径筛选，不虚构头部版本。
      // dmx skips the first line; native ASTC path-filters every row without inventing a header version.
      if (!nativeAstc) return;
    }
    if (!nativeAstc && text.startsWith("#")) return;
    const fields = text.split("|");
    // 原生 dmx 跳过短行及空行。保留此行为。
    // Native dmx skips short/blank rows. Preserve that behavior.
    if (fields.length < 11) return;
    if (++rowCount > CASC_LIMITS.rows) fail("manifest row budget");
    accept(fields.slice(0, 11));
  }
  for (let offset = 0n; offset < manifest.size;) {
    signal?.throwIfAborted();
    const count = Number(
      manifest.size - offset > BigInt(ROMFS_LIMITS.readBytes)
        ? BigInt(ROMFS_LIMITS.readBytes)
        : manifest.size - offset,
    );
    pending += decoder.decode(await exactRead(manifest, offset, count, signal), { stream: true });
    offset += BigInt(count);
    const lines = pending.split("\n");
    pending = lines.pop();
    for (const text of lines) line(text);
    if (pending.length > CASC_LIMITS.lineBytes) fail("manifest line budget");
  }
  pending += decoder.decode();
  if (pending) line(pending);
  if (first) fail("empty manifest");
  signal?.throwIfAborted();
}

/**
 * romfs 是所选已认证镜像上 indexRomfs 的 { getFile(path) }。认证及版本选择属于提取阶段，绝非调用方布尔值。仅清单会立即建立索引；不缓存归档或解码内容。重复逻辑路径采用最后一行，与 dm/tools/dmx.py 完全一致。
 * romfs is indexRomfs's { getFile(path) } over the selected authenticated image.
 * Authentication/version selection belongs to extraction, never a caller boolean.
 * Only the manifest is indexed eagerly. No archive or decoded content is cached.
 * Duplicate logical paths use the last row, exactly like dm/tools/dmx.py.
 */
export async function createCascLogicalAssets(romfs, { signal } = {}) {
  signal?.throwIfAborted();
  if (!romfs || typeof romfs.getFile !== "function") fail("RomFS index required");
  const getFile = romfs.getFile.bind(romfs);
  const index = new Map();
  const duplicatePaths = new Set();
  await scanManifest(
    getFile,
    (fields) => {
      // 无名清单记录不可通过逻辑路径请求；保留其余已验证行。
      // Unnamed manifest records cannot be requested by logical path.
      if (!fields[9]) return;
      const key = checkPath(fields[9]);
      if (index.has(key)) duplicatePaths.add(key);
      index.set(key, Object.freeze(fields));
    },
    signal,
  );
  function lookup(path) {
    signal?.throwIfAborted();
    const key = checkPath(path, true);
    for (const candidate of [key, `data:${key}`, `data:data/${key}`]) {
      const row = index.get(candidate);
      if (row) return row;
    }
    throw new Error(`CASC logical asset not found: ${path}`);
  }
  return Object.freeze({
    count: index.size,
    lookup,
    hasDuplicatePath: (path) => duplicatePaths.has(checkPath(path, true)),
    // 迭代器避免复制庞大路径列表；行本身不可变。
    // Iterator avoids duplicating a large path list; rows themselves are immutable.
    entries: () => index.values(),
    async read(path, { maxBytes = ARCHIVE_LIMITS.decodedRecord, signal: readSignal } = {}) {
      signal?.throwIfAborted();
      readSignal?.throwIfAborted();
      const combined =
        signal && readSignal ? AbortSignal.any([signal, readSignal]) : (signal ?? readSignal);
      if (!Number.isSafeInteger(maxBytes) || maxBytes < 0) fail("maxBytes");
      const row = lookup(path);
      if (row[1] !== "0") fail("unsupported key layout");
      const locator = archiveLocator(row[6]);
      if (uint(row[2], "chunk") !== locator.chunk || uint(row[3], "archive") !== locator.archive)
        fail("EKey chunk/archive mismatch");
      const encodedSize = uint(row[7], "encoded size"),
        decodedSize = uint(row[5], "decoded size");
      if (
        decodedSize > maxBytes ||
        decodedSize > ARCHIVE_LIMITS.decodedRecord ||
        encodedSize > ARCHIVE_LIMITS.encodedRecord
      )
        fail("record byte budget");
      const file = reader(getFile(`data/${locator.filename}`), "archive");
      const start = BigInt(`0x${row[6].slice(22)}`);
      if (start > file.size || BigInt(encodedSize) > file.size - start) fail("record extent");
      // 现有解码器使用 Number 偏移。仅向其提供记录内局部窗口；
      // 所有绝对 RomFS 偏移及范围均保持 BigInt，包括超大镜像。
      // Existing decoder uses Number offsets. Give it only a record-local window;
      // every absolute RomFS offset and extent stays BigInt, including huge images.
      const source = {
        size: encodedSize,
        async read(offset, count) {
          const bytes = new Uint8Array(count);
          for (let position = 0; position < count; position += ROMFS_LIMITS.readBytes) {
            const length = Math.min(ROMFS_LIMITS.readBytes, count - position);
            bytes.set(
              await exactRead(file, start + BigInt(offset + position), length, combined),
              position,
            );
          }
          return bytes;
        },
      };
      return decodeArchiveRecord(
        source,
        {
          offset: 0,
          encodedSize,
          decodedSize,
          encoding: row[8],
          ckey: row[4],
        },
        { verifyMd5, signal: combined },
      );
    },
  });
}

// ASTC 仅索引首个规范路径匹配；此底层解析器本身不授予认证能力。
// ASTC indexes only the first normalized path match; this low-level parser grants no authentication.
export const ASTC_MANIFEST_LIMITS = Object.freeze({
  records: 32768,
  metadataBytes: 8 * 1024 * 1024,
});
export async function createCascAstcRecords(romfs, { signal } = {}) {
  signal?.throwIfAborted();
  if (!romfs || typeof romfs.getFile !== "function") fail("RomFS index required");
  const getFile = romfs.getFile.bind(romfs);
  const rows = new Map();
  let metadataBytes = 0;
  await scanManifest(
    getFile,
    (fields) => {
      const original = fields[9];
      const normalized = original.slice(original.indexOf(":") + 1).toLowerCase();
      if (!normalized.endsWith(".texture") && normalized !== "data/hd/texture_desc_cache.json")
        return;
      const path = checkPath(normalized);
      if (rows.has(path)) return;
      metadataBytes += utf8.encode(fields.join("|")).length;
      if (
        rows.size >= ASTC_MANIFEST_LIMITS.records ||
        metadataBytes > ASTC_MANIFEST_LIMITS.metadataBytes
      )
        fail("ASTC metadata budget");
      rows.set(path, Object.freeze(fields));
    },
    signal,
    true,
  );
  return Object.freeze({
    entries: () => rows.keys(),
    open(path, { signal: readSignal } = {}) {
      signal?.throwIfAborted();
      readSignal?.throwIfAborted();
      const combined =
        signal && readSignal ? AbortSignal.any([signal, readSignal]) : (signal ?? readSignal);
      const row = rows.get(path);
      if (!row) fail("ASTC record missing");
      if (row[1] !== "0") fail("unsupported key layout");
      const locator = archiveLocator(row[6]);
      if (uint(row[2], "chunk") !== locator.chunk || uint(row[3], "archive") !== locator.archive)
        fail("EKey chunk/archive mismatch");
      const encodedSize = uint(row[7], "encoded size"),
        decodedSize = uint(row[5], "decoded size");
      if (encodedSize > 0xffffffff || decodedSize > 0xffffffff) fail("ASTC record size");
      if (!/^[0-9a-f]{32}$/i.test(row[4])) fail("CKey");
      const file = reader(getFile(`data/${locator.filename}`), "archive");
      const offset = BigInt(locator.offset);
      if (offset > file.size || BigInt(encodedSize) > file.size - offset) fail("record extent");
      return Object.freeze({
        record: Object.freeze({
          offset,
          encodedSize,
          decodedSize,
          encoding: row[8],
          ckey: row[4],
          ekey: row[6],
        }),
        source: Object.freeze({
          size: file.size,
          async readAt(position, count, { signal: operationSignal } = {}) {
            const active =
              combined && operationSignal
                ? AbortSignal.any([combined, operationSignal])
                : (combined ?? operationSignal);
            active?.throwIfAborted();
            if (
              typeof position !== "bigint" ||
              !Number.isSafeInteger(count) ||
              count < 0 ||
              count > ROMFS_LIMITS.readBytes ||
              position < offset ||
              position > offset + BigInt(encodedSize) ||
              BigInt(count) > offset + BigInt(encodedSize) - position
            )
              fail("ASTC record read extent");
            return exactRead(file, position, count, active);
          },
        }),
      });
    },
  });
}
