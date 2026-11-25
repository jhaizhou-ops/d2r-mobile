// 本地、有界的全区 XCI HEAD/HFS0 检查；不解密、不联网、不验证卡带签名。
// Local, bounded global-XCI HEAD/HFS0 inspection; no decryption, network, or cartridge signatures.
// Authored JavaScript. Format references only (no hactool implementation is included):
// https://github.com/SciresM/hactool/blob/master/xci.h (HEAD field positions)
// https://github.com/SciresM/hactool/blob/master/hfs0.h (HFS0 records and relative offsets)
// https://github.com/SciresM/hactool/blob/master/xci.c (global unsuffixed root SHA-256)
// https://github.com/SciresM/hactool/blob/master/hfs0.c (declared member prefix SHA-256)

export const XCI_INPUT_LIMITS = Object.freeze({
  readBytes: 64 * 1024,
  partitions: 4,
  entries: 65536,
  indexBytes: 16 * 1024 * 1024,
  hashPrefixBytes: 16 * 1024 * 1024,
  totalHashBytes: 64 * 1024 * 1024,
  filenameBytes: 255,
});

export class XciInputError extends Error {
  constructor(code, message) {
    super(message);
    this.name = "XciInputError";
    this.code = code;
  }
}

const fail = (code, message) => {
  throw new XciInputError(code, message);
};
const partitionNames = new Set(["update", "normal", "secure", "logo"]);

function safeName(name) {
  return (
    name.length > 0 &&
    name.length <= XCI_INPUT_LIMITS.filenameBytes &&
    name !== "." &&
    name !== ".." &&
    !/[\\/:<>"|?*\p{C}]/u.test(name) &&
    !/[ .]$/u.test(name) &&
    name.normalize("NFC") === name &&
    !/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/iu.test(name)
  );
}

function fileSize(file) {
  if (
    !file ||
    !Number.isSafeInteger(file.size) ||
    file.size < 0 ||
    typeof file.slice !== "function"
  ) {
    fail("FILE_SIZE", "XCI file size is not safely representable.");
  }
  return BigInt(file.size);
}

// 单次 Blob 读取不超过 64 KiB；上层分配同样有硬性上限。
// Every Blob read is at most 64 KiB; callers also cap aggregate buffer allocations.
async function readBytes(state, offset, length) {
  if (
    !Number.isSafeInteger(length) ||
    length < 0 ||
    length > Math.max(XCI_INPUT_LIMITS.indexBytes, XCI_INPUT_LIMITS.hashPrefixBytes) ||
    offset < 0n ||
    offset > state.size ||
    BigInt(length) > state.size - offset
  ) {
    fail("READ_RANGE", "Requested XCI file range exceeds supported bounds.");
  }
  const result = new Uint8Array(length);
  try {
    for (let position = 0; position < length; position += XCI_INPUT_LIMITS.readBytes) {
      const count = Math.min(length - position, XCI_INPUT_LIMITS.readBytes);
      const start = offset + BigInt(position);
      const bytes = new Uint8Array(
        await state.file.slice(Number(start), Number(start + BigInt(count))).arrayBuffer(),
      );
      if (bytes.length !== count) fail("SHORT_READ", "XCI file data is truncated.");
      result.set(bytes, position);
    }
    return result;
  } catch (error) {
    result.fill(0);
    if (error instanceof XciInputError) throw error;
    fail("READ_FAILED", "Could not read the selected local XCI file.");
  }
}

function reserveHash(state, length) {
  if (length > XCI_INPUT_LIMITS.hashPrefixBytes) {
    fail("HFS0_HASH_LIMIT", "An XCI hash prefix exceeds the bounded verifier limit.");
  }
  state.hashBytes += length;
  if (state.hashBytes > XCI_INPUT_LIMITS.totalHashBytes) {
    fail("HFS0_HASH_LIMIT", "XCI hash prefixes exceed the total verification budget.");
  }
}

async function verifyHash(state, offset, length, expected, code) {
  const bytes = await readBytes(state, offset, length);
  try {
    if (!globalThis.crypto?.subtle) {
      fail("XCI_CRYPTO", "SHA-256 verification is unavailable in this browser context.");
    }
    let actual;
    try {
      actual = new Uint8Array(await globalThis.crypto.subtle.digest("SHA-256", bytes));
    } catch {
      fail("XCI_CRYPTO", "Could not verify the XCI SHA-256 hash.");
    }
    if (actual.length !== expected.length || actual.some((value, i) => value !== expected[i])) {
      fail(code, "XCI SHA-256 prefix mismatch or unsupported regional hash variant.");
    }
  } finally {
    bytes.fill(0);
  }
}

async function inspectHfs(state, base, extent, root = false) {
  if (extent < 16n || base < 0n || base > state.size || extent > state.size - base) {
    fail("HFS0_RANGE", "XCI HFS0 partition extent is invalid or truncated.");
  }
  const head = await readBytes(state, base, 16);
  const header = new DataView(head.buffer, head.byteOffset, head.byteLength);
  if (header.getUint32(0, true) !== 0x30534648) {
    fail("HFS0_MAGIC", "XCI partition does not have an HFS0 header.");
  }
  if (header.getUint32(12, true) !== 0) {
    fail("HFS0_RESERVED", "Unsupported HFS0 reserved header value.");
  }
  const count = header.getUint32(4, true);
  const stringsSize = header.getUint32(8, true);
  const indexSize = 16n + BigInt(count) * 64n + BigInt(stringsSize);
  state.entries += count;
  state.indexBytes += indexSize;
  if (
    (root && (!count || count > XCI_INPUT_LIMITS.partitions)) ||
    state.entries > XCI_INPUT_LIMITS.entries ||
    state.indexBytes > BigInt(XCI_INPUT_LIMITS.indexBytes)
  ) {
    fail("HFS0_LIMIT", "XCI HFS0 indexes exceed supported count or size bounds.");
  }
  if (indexSize > extent) fail("HFS0_TRUNCATED", "XCI HFS0 index is truncated.");
  const index = await readBytes(state, base + 16n, Number(indexSize - 16n));
  const view = new DataView(index.buffer, index.byteOffset, index.byteLength);
  const stringsStart = count * 64;
  const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
  const seen = new Set();
  const entries = [];
  for (let i = 0; i < count; i++) {
    const position = i * 64;
    const relativeOffset = view.getBigUint64(position, true);
    const size = view.getBigUint64(position + 8, true);
    const nameOffset = view.getUint32(position + 16, true);
    const hashSize = view.getUint32(position + 20, true);
    if (view.getBigUint64(position + 24, true) !== 0n) {
      fail("HFS0_RESERVED", "Unsupported HFS0 reserved entry value.");
    }
    if (
      nameOffset >= stringsSize ||
      (nameOffset > 0 && index[stringsStart + nameOffset - 1] !== 0)
    ) {
      fail("HFS0_NAME", "XCI HFS0 filename offset is invalid.");
    }
    const nameStart = stringsStart + nameOffset;
    const nameLimit = Math.min(index.length, nameStart + XCI_INPUT_LIMITS.filenameBytes + 1);
    let nameEnd = nameStart;
    while (nameEnd < nameLimit && index[nameEnd] !== 0) nameEnd++;
    if (nameEnd === nameStart || nameEnd === nameLimit) {
      fail("HFS0_NAME", "XCI filename is empty, too long, or unterminated.");
    }
    let name;
    try {
      name = decoder.decode(index.subarray(nameStart, nameEnd));
    } catch {
      fail("HFS0_NAME", "XCI filename is not valid UTF-8.");
    }
    if (!safeName(name)) fail("UNSAFE_NAME", "XCI contains an unsafe entry name.");
    const folded = name.toLowerCase();
    if (seen.has(folded)) fail("DUPLICATE_NAME", "XCI contains ambiguous duplicate names.");
    seen.add(folded);
    const relativeStart = indexSize + relativeOffset;
    if (relativeStart > extent || size > extent - relativeStart) {
      fail("HFS0_RANGE", "XCI HFS0 member extends beyond its containing partition.");
    }
    if (BigInt(hashSize) > size || (size > 0n && hashSize === 0)) {
      fail("HFS0_HASH_RANGE", "XCI HFS0 member has an invalid or missing hash prefix.");
    }
    reserveHash(state, hashSize);
    entries.push({
      name,
      offset: base + relativeStart,
      size,
      hashSize,
      hash: index.slice(position + 32, position + 64),
    });
  }
  const occupied = entries
    .filter((entry) => entry.size > 0n)
    .sort((a, b) => (a.offset < b.offset ? -1 : a.offset > b.offset ? 1 : 0));
  for (let i = 1; i < occupied.length; i++) {
    if (occupied[i].offset < occupied[i - 1].offset + occupied[i - 1].size) {
      fail("HFS0_OVERLAP", "XCI HFS0 member extents overlap.");
    }
  }
  return { indexSize, entries };
}

/**
 * 返回 secure 分区中 NCA/票据的绝对 BigInt 范围；调用方必须使用同一个不可变 Blob。
 * Return absolute BigInt NCA/ticket ranges from secure; consume the same immutable Blob.
 * Hashes establish container consistency, NOT authenticity or full-member integrity.
 * Existing NCA/CNMT identity/integrity checks remain mandatory after this inspection.
 * Only global (unsuffixed SHA-256) HEAD/HFS0 is supported. No XCI header-key use,
 * RSA/certificate verification, regional suffix fallback, split, or compressed input.
 */
export async function inspectXci(file) {
  const size = fileSize(file);
  if (size < 0x200n) fail("XCI_HEADER", "XCI HEAD header is truncated.");
  const state = { file, size, hashBytes: 0, indexBytes: 0n, entries: 0 };
  const head = await readBytes(state, 0n, 0x200);
  const header = new DataView(head.buffer, head.byteOffset, head.byteLength);
  if (header.getUint32(0x100, true) !== 0x44414548) {
    fail("XCI_MAGIC", "XCI does not have a HEAD header at offset 0x100.");
  }
  const rootOffset = header.getBigUint64(0x130, true);
  const rootHeaderSize = header.getBigUint64(0x138, true);
  if (
    rootOffset < 0x200n ||
    rootOffset % 0x200n !== 0n ||
    rootOffset > size ||
    rootHeaderSize < 16n ||
    rootHeaderSize > size - rootOffset
  ) {
    fail("XCI_ROOT_RANGE", "XCI root HFS0 offset or declared header size is invalid.");
  }
  if (rootHeaderSize > BigInt(XCI_INPUT_LIMITS.hashPrefixBytes)) {
    fail("HFS0_HASH_LIMIT", "XCI root header exceeds the bounded verifier limit.");
  }
  reserveHash(state, Number(rootHeaderSize));
  await verifyHash(
    state,
    rootOffset,
    Number(rootHeaderSize),
    head.subarray(0x140, 0x160),
    "XCI_ROOT_HASH",
  );
  const root = await inspectHfs(state, rootOffset, size - rootOffset, true);
  if (root.indexSize !== rootHeaderSize) {
    fail("XCI_ROOT_SIZE", "XCI declared root header size does not match its HFS0 index.");
  }
  if (
    root.entries.some((entry) => !partitionNames.has(entry.name)) ||
    !root.entries.some((entry) => entry.name === "secure")
  ) {
    fail("XCI_PARTITIONS", "XCI requires a secure partition and only known partition names.");
  }
  const selected = [];
  for (const partition of root.entries) {
    await verifyHash(
      state,
      partition.offset,
      partition.hashSize,
      partition.hash,
      "HFS0_PARTITION_HASH",
    );
    const members = await inspectHfs(state, partition.offset, partition.size);
    for (const member of members.entries) {
      if (partition.name === "secure" && /\.ncz$/iu.test(member.name)) {
        fail("XCI_COMPRESSED", "Compressed NCZ members are unsupported; use an uncompressed XCI.");
      }
      await verifyHash(state, member.offset, member.hashSize, member.hash, "HFS0_MEMBER_HASH");
      if (partition.name === "secure" && /\.(?:nca|tik)$/iu.test(member.name)) {
        if (member.size === 0n) fail("XCI_EMPTY_MEMBER", "XCI contains an empty NCA or ticket.");
        selected.push(
          Object.freeze({ name: member.name, offset: member.offset, size: member.size }),
        );
      }
    }
  }
  if (!selected.some((entry) => /\.nca$/iu.test(entry.name))) {
    fail("XCI_CONTENT", "XCI secure partition contains no NCA files.");
  }
  return Object.freeze({ format: "XCI", size, entries: Object.freeze(selected) });
}
