// 对调用方提供的不可变、已认证 IVFC 数据层建立有界 RomFS 索引。
// 依据 RomFS 元数据布局与调用方认证的 IVFC 数据边界。
// Builds bounded RomFS indexes over caller-supplied immutable, authenticated IVFC data.
// Based on RomFS metadata layout and the caller-authenticated IVFC data boundary.

/**
 * 有界 RomFS 索引。调用方必须提供不可变且已认证的 IVFC 数据层。
 * Bounded RomFS indexing. The caller must supply an immutable, authenticated IVFC data level.
 */
export const ROMFS_LIMITS = Object.freeze({
  readBytes: 64 * 1024,
  metadataBytes: 16 * 1024 * 1024,
  members: 100000,
  depth: 64,
  nameBytes: 255,
  pathBytes: 4096,
  totalPathBytes: 16 * 1024 * 1024,
});
const EMPTY = 0xffffffff;
const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const viewOf = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
function fail(message) {
  throw new Error(`Invalid RomFS: ${message}`);
}
function extent(offset, size, end, label) {
  if (offset < 0n || size < 0n || offset > end || size > end - offset)
    fail(`${label} out of bounds`);
}
function disjoint(ranges, label) {
  const sorted = ranges
    .filter((r) => r[1] > r[0])
    .sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  for (let i = 1; i < sorted.length; i++)
    if (sorted[i][0] < sorted[i - 1][1]) fail(`${label} overlap`);
}
function complete(ranges, size) {
  ranges.sort((a, b) => a[0] - b[0]);
  let end = 0;
  for (const [start, next] of ranges) {
    if (start !== end) fail("metadata overlap or unreachable record");
    end = next;
  }
  if (BigInt(end) !== size) fail("unreachable metadata");
}
function component(bytes) {
  let name;
  try {
    name = decoder.decode(bytes);
  } catch {
    fail("invalid UTF-8 name");
  }
  if (
    !name ||
    name === "." ||
    name === ".." ||
    name !== name.normalize("NFC") ||
    [...name].some(
      (char) =>
        char.codePointAt(0) < 32 || (char.codePointAt(0) >= 127 && char.codePointAt(0) <= 159),
    ) ||
    /[/\\<>:"|?*]/u.test(name) ||
    /[. ]$/u.test(name) ||
    /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/iu.test(name)
  )
    fail("unsafe name");
  return name;
}

/**
 * source：{ size: bigint, readAt(offset: bigint, count: number): Uint8Array | Promise<Uint8Array> }。
 * readAt 在此索引的整个生命周期内必须引用同一已认证的不可变映像。
 * 此解析器不认证或解密字节，也不信任调用方提供的“verified”标志。
 * source: { size: bigint, readAt(offset: bigint, count: number): Uint8Array | Promise<Uint8Array> }.
 * readAt must refer to the SAME authenticated immutable image for this index's lifetime.
 * This parser does not authenticate/decrypt bytes, nor trust a caller-supplied "verified" flag.
 */
export async function indexRomfs(source, { signal } = {}) {
  if (
    !source ||
    typeof source.size !== "bigint" ||
    source.size < 80n ||
    typeof source.readAt !== "function"
  )
    fail("reader contract");
  const imageSize = source.size;
  const readAt = source.readAt.bind(source);
  const check = (extraSignal) => {
    signal?.throwIfAborted();
    extraSignal?.throwIfAborted();
  };
  async function read(offset, count, extraSignal) {
    check(extraSignal);
    if (
      typeof offset !== "bigint" ||
      !Number.isSafeInteger(count) ||
      count < 0 ||
      count > ROMFS_LIMITS.readBytes
    )
      fail("read limit");
    extent(offset, BigInt(count), imageSize, "read");
    if (count === 0) return new Uint8Array();
    const bytes = await readAt(offset, count);
    check(extraSignal);
    if (!(bytes instanceof Uint8Array) || bytes.byteLength !== count) fail("short read");
    // 将元数据和返回的载荷与读取器可复用的暂存缓冲区隔离。
    // Isolate metadata and returned payload from a reader's reusable scratch buffer.
    // 使用基类构造器复制，避免 Buffer 等子类的 slice 保留来源别名。
    // Copy through the base constructor; subclass slice methods such as Buffer.slice may alias.
    return new Uint8Array(bytes);
  }
  let metadataReadBytes = 0;
  async function readMetadata(offset, count) {
    metadataReadBytes += count;
    if (metadataReadBytes > ROMFS_LIMITS.metadataBytes) fail("metadata read budget");
    return read(offset, count);
  }
  const header = viewOf(await readMetadata(0n, 80));
  const u64 = (offset) => header.getBigUint64(offset, true);
  if (u64(0) !== 80n) fail("header size");
  const tables = [8, 24, 40, 56].map((p) => ({ offset: u64(p), size: u64(p + 8) }));
  const dataOffset = u64(72);
  if (dataOffset < 80n || dataOffset > imageSize || dataOffset % 4n) fail("data offset");
  let metadataBytes = 80n;
  for (const table of tables) {
    if (table.offset < 80n || table.offset % 4n || table.size % 4n) fail("table alignment");
    extent(table.offset, table.size, imageSize, "metadata table");
    metadataBytes += table.size;
  }
  if (metadataBytes > BigInt(ROMFS_LIMITS.metadataBytes)) fail("metadata budget");
  disjoint(
    tables.map((t) => [t.offset, t.offset + t.size]),
    "metadata tables",
  );
  const dirs = tables[1],
    files = tables[3];
  if (dirs.size < 24n) fail("missing root");
  const visitedDirs = new Set(),
    visitedFiles = new Set(),
    paths = new Set();
  const dirRanges = [],
    fileRanges = [],
    dataRanges = [];
  const entries = [],
    directoryPaths = [];
  let members = 0,
    pathBytes = 0;
  function register(path, bytes) {
    if (++members > ROMFS_LIMITS.members) fail("member count");
    if (bytes > ROMFS_LIMITS.pathBytes || (pathBytes += bytes) > ROMFS_LIMITS.totalPathBytes)
      fail("path budget");
    // 为下游不区分大小写的文件系统拒绝大小写折叠后的冲突。
    // Case-folded collisions are rejected for downstream case-insensitive filesystems.
    const key = path.toLowerCase();
    if (paths.has(key)) fail("duplicate path");
    paths.add(key);
  }
  async function record(table, offset, fixed, visited, ranges) {
    check();
    if (offset === EMPTY || offset % 4 || visited.has(offset)) fail("invalid link or cycle");
    if (members >= ROMFS_LIMITS.members) fail("member count");
    extent(BigInt(offset), BigInt(fixed), table.size, "record");
    visited.add(offset);
    const bytes = await readMetadata(table.offset + BigInt(offset), fixed);
    const view = viewOf(bytes);
    const nameSize = view.getUint32(fixed - 4, true);
    if (nameSize > ROMFS_LIMITS.nameBytes) fail("name limit");
    const length = fixed + Math.ceil(nameSize / 4) * 4;
    extent(BigInt(offset), BigInt(length), table.size, "record name");
    ranges.push([offset, offset + length]);
    const name = nameSize
      ? component(await readMetadata(table.offset + BigInt(offset + fixed), nameSize))
      : "";
    return { view, name, nameSize };
  }
  // 显式栈避免递归，并跟踪预期父节点和深度。
  // Explicit stack avoids recursion and tracks both expected parent and depth.
  const stack = [{ offset: 0, parent: 0, path: "", bytes: 0, depth: 0 }];
  while (stack.length) {
    check();
    const node = stack.pop();
    if (node.depth > ROMFS_LIMITS.depth) fail("depth limit");
    const dir = await record(dirs, node.offset, 24, visitedDirs, dirRanges);
    const d = dir.view;
    if (d.getUint32(0, true) !== node.parent) fail("directory parent");
    const root = node.offset === 0;
    if (root ? dir.nameSize !== 0 || d.getUint32(4, true) !== EMPTY : dir.nameSize === 0)
      fail("root or directory name");
    const path = root ? "" : node.path ? `${node.path}/${dir.name}` : dir.name;
    const bytes = node.bytes + (node.path ? 1 : 0) + dir.nameSize;
    register(path, bytes);
    directoryPaths.push(path);
    let fileOffset = d.getUint32(12, true);
    while (fileOffset !== EMPTY) {
      const file = await record(files, fileOffset, 32, visitedFiles, fileRanges);
      const f = file.view;
      if (f.getUint32(0, true) !== node.offset || !file.nameSize) fail("file parent or name");
      const filePath = path ? `${path}/${file.name}` : file.name;
      register(filePath, bytes + (path ? 1 : 0) + file.nameSize);
      const offset = dataOffset + f.getBigUint64(8, true),
        size = f.getBigUint64(16, true);
      extent(offset, size, imageSize, "file data");
      if (size) dataRanges.push([offset, offset + size]);
      entries.push(
        Object.freeze({
          path: filePath,
          size,
          async readAt(relativeOffset, count, { signal: readSignal } = {}) {
            check(readSignal);
            if (
              typeof relativeOffset !== "bigint" ||
              !Number.isSafeInteger(count) ||
              count < 0 ||
              count > ROMFS_LIMITS.readBytes
            )
              fail("file read limit");
            extent(relativeOffset, BigInt(count), size, "file read");
            return read(offset + relativeOffset, count, readSignal);
          },
        }),
      );
      fileOffset = f.getUint32(4, true);
    }
    const sibling = d.getUint32(4, true),
      child = d.getUint32(8, true);
    if (sibling !== EMPTY) stack.push({ ...node, offset: sibling });
    if (child !== EMPTY)
      stack.push({ offset: child, parent: node.offset, path, bytes, depth: node.depth + 1 });
  }
  check();
  complete(dirRanges, dirs.size);
  complete(fileRanges, files.size);
  disjoint(dataRanges, "file data");
  // 文件范围和表范围已分别检查；仅检查四个表与文件的交集，避免再次排序和复制文件列表。
  // Files and tables are separately disjoint; check only their intersections, avoiding another file-list copy and sort.
  for (const table of tables) {
    if (!table.size) continue;
    const end = table.offset + table.size;
    for (const [start, next] of dataRanges)
      if (start < end && table.offset < next) fail("file data and metadata overlap");
  }
  const byPath = new Map(entries.map((entry) => [entry.path, entry]));
  return Object.freeze({
    size: imageSize,
    files: Object.freeze(entries),
    directories: Object.freeze(directoryPaths),
    getFile: (path) => byPath.get(path),
  });
}
