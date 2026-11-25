// 原生 CASC 二进制改写的 JavaScript 端口；压缩和 MD5 由固定来源内核注入。 | Native CASC binary rewrite port with source-pinned compression and MD5 injected.
const MAX_INPUT = 64 * 1024 * 1024;
const MAX_OUTPUT = 64 * 1024 * 1024;
const MAX_TABLE = 16 * 1024 * 1024;
const MAX_REPLACEMENT = 16 * 1024 * 1024;
const MAX_JOBS = 4096;
// 补充包元数据具有独立预算，不扩大现有覆盖层任务或载荷限制。
// Supplemental-pack metadata has separate budgets; existing overlay job and payload caps remain unchanged.
export const CASC_PACK_METADATA_LIMITS = Object.freeze({
  entries: 32768,
  pathBytes: 8 * 1024 * 1024,
});
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const fail = () => {
  throw new Error("CASC_OVERLAY_INVALID");
};
function requireValue(value) {
  if (!value) fail();
}
const isObject = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
const u32 = (bytes, pos) => {
  requireValue(pos >= 0 && pos + 4 <= bytes.length);
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(pos, false);
};
const put32 = (bytes, pos, value) => {
  requireValue(
    Number.isSafeInteger(value) &&
      value >= 0 &&
      value <= 0xffffffff &&
      pos >= 0 &&
      pos + 4 <= bytes.length,
  );
  new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).setUint32(pos, value, false);
};
const word = (value) => {
  const bytes = new Uint8Array(4);
  put32(bytes, 0, value);
  return bytes;
};
const concat = (parts, cap = MAX_OUTPUT) => {
  const length = parts.reduce((sum, part) => sum + part.length, 0);
  requireValue(length <= cap);
  const output = new Uint8Array(length);
  let at = 0;
  for (const part of parts) {
    output.set(part, at);
    at += part.length;
  }
  return output;
};
const equal = (a, b) => a.length === b.length && a.every((value, i) => value === b[i]);
const hex = (bytes) => Array.from(bytes, (n) => n.toString(16).padStart(2, "0")).join("");
const unhex = (text) => {
  requireValue(typeof text === "string" && text.length === 32 && /^[0-9a-fA-F]{32}$/.test(text));
  return Uint8Array.from(text.match(/../g), (value) => Number.parseInt(value, 16));
};
const byteKey = (bytes) => {
  let result = "";
  for (let i = 0; i < bytes.length; i += 8192)
    result += String.fromCharCode(...bytes.subarray(i, i + 8192));
  return result;
};
const byteValue = (key) => Uint8Array.from(key, (c) => c.charCodeAt(0));
const lowerBytes = (key) => key.replace(/[A-Z]/g, (c) => c.toLowerCase());
const compare = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const compareUnicode = (a, b) => {
  const x = Array.from(a, (c) => c.codePointAt(0)),
    y = Array.from(b, (c) => c.codePointAt(0));
  for (let i = 0; i < Math.min(x.length, y.length); i++) if (x[i] !== y[i]) return x[i] - y[i];
  return x.length - y.length;
};
function utf8(value) {
  requireValue(typeof value === "string");
  // 配置可达 MiB 级别；直接验证码点，不创建逐字符数组。
  // Configs can reach MiB scale; validate code points without a character array.
  for (const char of value) {
    const point = char.codePointAt(0);
    requireValue(point < 0xd800 || point > 0xdfff);
  }
  return encoder.encode(value);
}
function checkedPath(path) {
  requireValue(typeof path === "string" && path.length <= 4096 && utf8(path).length <= 4096);
  // Python strip 所用空白码点；不能把 BOM 当空白。 | Python strip whitespace, excluding BOM.
  requireValue(Array.from(path).every((c) => c.codePointAt(0) >= 32 && c !== ":" && c !== "\\"));
  requireValue(
    path === path.toLowerCase() &&
      !path.startsWith("/") &&
      path.split("/").every((part) => part && part !== "." && part !== ".."),
  );
  const whitespace = new Set([
    32,
    133,
    160,
    5760,
    8232,
    8233,
    8239,
    8287,
    12288,
    ...Array.from({ length: 11 }, (_, i) => 8192 + i),
  ]);
  requireValue(
    !whitespace.has(path.codePointAt(0)) && !whitespace.has(path.codePointAt(path.length - 1)),
  );
  return path;
}
function table(raw) {
  requireValue(
    raw.length >= 46 &&
      raw.length <= MAX_TABLE &&
      byteKey(raw.subarray(0, 4)) === "TVFS" &&
      raw[4] === 1 &&
      raw[5] >= 46 &&
      raw[5] <= raw.length &&
      raw[6] === 16 &&
      (raw[7] === 0 || raw[7] === 16) &&
      u32(raw, 8) === 10,
  );
  const path = u32(raw, 12),
    pathSize = u32(raw, 16),
    vfs = u32(raw, 20),
    vfsSize = u32(raw, 24),
    cft = u32(raw, 28),
    cftSize = u32(raw, 32),
    est = u32(raw, 38),
    estSize = u32(raw, 42);
  const spans = [
    [path, pathSize],
    [vfs, vfsSize],
    [cft, cftSize],
    [est, estSize],
  ];
  for (const [start, size] of spans) requireValue(start >= 46 && start + size <= raw.length);
  requireValue(estSize > 0 && estSize <= 255 && cftSize % 25 === 0);
  const nonempty = spans.filter(([, size]) => size > 0).sort((a, b) => a[0] - b[0]);
  for (let i = 1; i < nonempty.length; i++)
    requireValue(nonempty[i - 1][0] + nonempty[i - 1][1] <= nonempty[i][0]);
  const values = byteKey(raw.subarray(est, est + estSize)).split("\0");
  const specs = [];
  if (raw[7] === 0) {
    // 原始合成夹具使用序号索引。
    // The original synthetic fixture uses ordinal indexes.
    values.forEach((value, index) => {
      specs[index] = value;
    });
  } else {
    // 零售版 TVFS 的 CFT 记录使用 EST 字符串表内的字节偏移。
    // Retail TVFS CFT records use byte offsets into the EST string table.
    let specOffset = 0;
    for (const value of values) {
      specs[specOffset] = value;
      specOffset += value.length + 1;
    }
  }
  for (let pos = cft; pos < cft + cftSize; pos += 25)
    requireValue(["n", "z", "b:64K*=n", "b:256K*=z"].includes(specs[raw[pos + 20]]));
  return { path, pathSize, vfs, vfsSize, cft, cftSize, end: cft + cftSize, est, estSize, specs };
}
function pathEntry(raw, at) {
  let pos = at,
    pre = 0,
    post = 0,
    name = "",
    value = null;
  const next = () => {
    requireValue(pos < raw.length);
    return raw[pos];
  };
  if (next() === 0) {
    pre = 1;
    pos++;
  }
  if (next() !== 255) {
    const size = raw[pos++];
    requireValue(pos + size <= raw.length);
    name = byteKey(raw.subarray(pos, pos + size));
    pos += size;
  }
  if (next() === 0) {
    post = 1;
    pos++;
  }
  if (next() === 255) {
    value = u32(raw, pos + 1);
    pos += 5;
  } else post = 2;
  return { pos, pre, name, post, value };
}
function validatePathTree(raw) {
  const start = u32(raw, 12),
    size = u32(raw, 16),
    stack = [[start, start + size, 0]];
  let count = 0;
  while (stack.length) {
    let [pos, end, depth] = stack.pop();
    requireValue(depth <= 128);
    while (pos < end) {
      const old = pos,
        entry = pathEntry(raw, pos);
      pos = entry.pos;
      count++;
      requireValue(pos > old && pos <= end && count <= 1000000);
      if (entry.value !== null && entry.value & 0x80000000) {
        const stop = pos + (entry.value & 0x7fffffff) - 4;
        requireValue(stop >= pos && stop <= end);
        stack.push([pos, stop, depth + 1]);
        pos = stop;
      }
    }
  }
}
function* walkPaths(raw, start, end, prefix = "", parents = []) {
  let pos = start;
  const base = prefix;
  while (pos < end) {
    const at = pos,
      entry = pathEntry(raw, pos);
    pos = entry.pos;
    prefix += (entry.pre ? "/" : "") + entry.name + (entry.post ? "/" : "");
    if (entry.value === null) continue;
    if (entry.value & 0x80000000) {
      const stop = pos + (entry.value & 0x7fffffff) - 4;
      yield { at, path: prefix, value: entry.value, folder: [pos, stop], parents };
      yield* walkPaths(raw, pos, stop, prefix, [...parents, at]);
      pos = stop;
    } else yield { at, path: prefix, value: entry.value, folder: null, parents };
    prefix = base;
  }
}
function trieBytes(names) {
  const groups = new Map(),
    out = [];
  let depth = 0;
  for (const name of [...names.keys()].sort(compare)) {
    requireValue(name.length <= 254);
    const first = name.slice(0, 1);
    if (!groups.has(first)) groups.set(first, []);
    groups.get(first).push(name);
  }
  for (const group of groups.values()) {
    if (group.length === 1) {
      const name = group[0];
      out.push(
        new Uint8Array([name.length]),
        byteValue(name),
        new Uint8Array([255]),
        word(names.get(name)),
      );
      continue;
    }
    let common = group[0];
    for (const name of group.slice(1))
      while (!name.startsWith(common)) common = common.slice(0, -1);
    requireValue(!group.includes(common));
    const sub = trieBytes(
      new Map(group.map((name) => [name.slice(common.length), names.get(name)])),
    );
    out.push(
      new Uint8Array([common.length]),
      byteValue(common),
      new Uint8Array([255]),
      word(0x80000000 + sub.bytes.length + 4),
      sub.bytes,
    );
    depth = Math.max(depth, sub.depth + 1);
  }
  return { bytes: concat(out, MAX_TABLE), depth };
}
const offsetFieldSize = (size) => (size <= 255 ? 1 : size <= 65535 ? 2 : size <= 16777215 ? 3 : 4);
function sizedWord(value, size) {
  requireValue(Number.isSafeInteger(value) && value >= 0 && value < 2 ** (size * 8));
  const result = new Uint8Array(size);
  for (let i = size - 1; i >= 0; i--) {
    result[i] = value % 256;
    value = Math.floor(value / 256);
  }
  return result;
}
function addPaths(raw, additions) {
  const {
    path: pathOff,
    pathSize,
    vfs: vfsOff,
    vfsSize,
    cft: cftOff,
    cftSize,
    est: estOff,
    estSize,
  } = table(raw);
  requireValue(
    pathOff + pathSize === estOff &&
      estOff + estSize === cftOff &&
      cftOff + cftSize === vfsOff &&
      vfsOff + vfsSize === raw.length,
  );
  const before = new Map(),
    nodes = new Map();
  for (const node of walkPaths(raw, pathOff, pathOff + pathSize)) {
    if (node.folder) nodes.set(node.path, node);
    else before.set(node.path, node.value);
  }
  const field = offsetFieldSize(cftSize);
  requireValue(offsetFieldSize(cftSize + 25 * additions.size) === field);
  const entrySize = 9 + field,
    newCft = [],
    newVfs = [],
    groups = new Map();
  let number = 0;
  for (const [logical, item] of [...additions].sort((a, b) => compareUnicode(a[0], b[0]))) {
    const bytes = byteKey(utf8(logical)),
      slash = bytes.lastIndexOf("/"),
      second = bytes.lastIndexOf("/", slash - 1);
    requireValue(slash >= 0 && second >= 0);
    const parent = bytes.slice(0, second + 1),
      folder = bytes.slice(second + 1, slash),
      name = bytes.slice(slash + 1),
      groupKey = parent + "\0" + folder;
    if (!groups.has(groupKey)) groups.set(groupKey, { parent, folder, names: new Map() });
    groups.get(groupKey).names.set(name, vfsSize + number * entrySize);
    newCft.push(
      item.ekey,
      word(item.encoded),
      new Uint8Array([item.specIndex]),
      word(item.decoded),
    );
    newVfs.push(
      new Uint8Array([1]),
      word(0),
      word(item.decoded),
      sizedWord(cftSize + number * 25, field),
    );
    number++;
  }
  const path = raw.slice(pathOff, pathOff + pathSize),
    grown = new Map(),
    inserts = [];
  let maxDepth = new DataView(raw.buffer, raw.byteOffset, raw.byteLength).getUint16(36, false);
  for (const { parent, folder, names } of [...groups.values()].sort(
    (a, b) => compare(a.parent, b.parent) || compare(a.folder, b.folder),
  )) {
    const target = [...nodes].find(([key]) => lowerBytes(key) === lowerBytes(parent))?.[1];
    requireValue(target);
    requireValue(
      ![...before.keys(), ...nodes.keys()].some((key) =>
        lowerBytes(key).startsWith(lowerBytes(parent + folder)),
      ),
    );
    const sub = trieBytes(names);
    requireValue(folder.length <= 255);
    const block = concat(
      [
        new Uint8Array([folder.length]),
        byteValue(folder),
        new Uint8Array([0, 255]),
        word(0x80000000 + sub.bytes.length + 4),
        sub.bytes,
      ],
      MAX_TABLE,
    );
    const [start, end] = target.folder;
    let at = end,
      pos = start,
      continuation = false;
    while (pos < end) {
      const child = pos,
        entry = pathEntry(raw, pos);
      pos = entry.pos;
      const first = entry.pre ? "/" : entry.name.slice(0, 1),
        wasContinuation = continuation;
      continuation = entry.value === null;
      if (wasContinuation) {
        if (entry.value !== null && entry.value & 0x80000000) pos += (entry.value & 0x7fffffff) - 4;
        continue;
      }
      requireValue(first !== folder.slice(0, 1));
      if (first > folder.slice(0, 1) && at === end) at = child;
      if (entry.value !== null && entry.value & 0x80000000) pos += (entry.value & 0x7fffffff) - 4;
    }
    inserts.push({ at: at - pathOff, block });
    for (const ancestor of [...target.parents, target.at])
      grown.set(ancestor - pathOff, (grown.get(ancestor - pathOff) ?? 0) + block.length);
    maxDepth = Math.max(maxDepth, target.parents.length + 2 + sub.depth);
  }
  for (const [relative, delta] of grown) {
    const at = pathEntry(path, relative).pos - 4;
    const size = (u32(path, at) & 0x7fffffff) + delta;
    requireValue(size <= 0x7fffffff);
    put32(path, at, 0x80000000 + size);
  }
  // 与原生逆序插入等价，单次拼接避免重复复制整棵树。 | One concatenation is equivalent to native reverse insertion, without repeatedly copying the tree.
  const pieces = [];
  let cursor = 0;
  for (const item of inserts.sort(
    (a, b) => a.at - b.at || compare(byteKey(a.block), byteKey(b.block)),
  )) {
    pieces.push(path.subarray(cursor, item.at), item.block);
    cursor = item.at;
  }
  pieces.push(path.subarray(cursor));
  const tree = concat(pieces, MAX_TABLE),
    head = raw.slice(0, pathOff),
    est = raw.subarray(estOff, estOff + estSize),
    cft = concat([raw.subarray(cftOff, cftOff + cftSize), ...newCft], MAX_TABLE),
    vfs = concat([raw.subarray(vfsOff, vfsOff + vfsSize), ...newVfs], MAX_TABLE);
  const nextEst = pathOff + tree.length,
    nextCft = nextEst + est.length,
    nextVfs = nextCft + cft.length;
  [pathOff, tree.length, nextVfs, vfs.length, nextCft, cft.length].forEach((value, i) =>
    put32(head, 12 + i * 4, value),
  );
  requireValue(maxDepth <= 65535);
  new DataView(head.buffer, head.byteOffset, head.byteLength).setUint16(36, maxDepth, false);
  put32(head, 38, nextEst);
  const result = concat([head, tree, est, cft, vfs], MAX_TABLE),
    after = new Map();
  validatePathTree(result);
  for (const node of walkPaths(result, pathOff, pathOff + tree.length))
    if (!node.folder) after.set(node.path, node.value);
  for (const [key, value] of before) requireValue(after.get(key) === value);
  const added = [...after.keys()]
    .filter((path) => !before.has(path))
    .map((path) => decoder.decode(byteValue(path)).toLowerCase())
    .sort(compareUnicode);
  const expected = [...additions.keys()].sort(compareUnicode);
  requireValue(
    added.length === expected.length && added.every((value, i) => value === expected[i]),
  );
  return result;
}
function setCftSizes(raw, pos, encoded, decoded) {
  const width = offsetFieldSize(u32(raw, 42));
  put32(raw, pos + 16, encoded);
  put32(raw, pos + 20 + width, decoded);
}
function cftPosition(raw, start, end, key, search = byteKey(raw.subarray(start, end))) {
  const table = search,
    needle = byteKey(key);
  const first = table.indexOf(needle);
  requireValue(first >= 0 && table.indexOf(needle, first + 1) === -1);
  return start + first;
}
// 只保留对齐记录的紧凑索引，但逐字节扫描所有候选位置以保留非对齐重复检测。
// Index aligned records compactly, but scan every byte position to retain unaligned duplicate detection.
function compactCftPosition(raw, start, end) {
  const bytes = raw.subarray(start, end),
    count = bytes.length / 25,
    hashes = new Uint32Array(count),
    order = new Uint32Array(count),
    occurrences = new Uint8Array(count);
  let multiplier = 1;
  for (let i = 0; i < 15; i++) multiplier = Math.imul(multiplier, 31);
  const fingerprint = (source, at = 0) => {
    let hash = 0;
    for (let i = 0; i < 16; i++) hash = (Math.imul(hash, 31) + source[at + i]) >>> 0;
    return hash;
  };
  const compare = (position, source, at) => {
    for (let i = 0; i < 16; i++) {
      const difference = bytes[position + i] - source[at + i];
      if (difference) return difference;
    }
    return 0;
  };
  for (let i = 0; i < count; i++) {
    order[i] = i;
    hashes[i] = fingerprint(bytes, i * 25);
  }
  // 指纹碰撞也按完整键排序和二分，不扫描碰撞桶或重复键组。
  // Break fingerprint ties with full keys and binary search, never scan collision buckets or duplicate groups.
  order.sort((a, b) => hashes[a] - hashes[b] || compare(a * 25, bytes, b * 25));
  const find = (hash, source, at) => {
    let low = 0,
      high = count;
    while (low < high) {
      const middle = low + Math.floor((high - low) / 2),
        index = order[middle],
        difference = hashes[index] - hash || compare(index * 25, source, at);
      if (difference < 0) low = middle + 1;
      else high = middle;
    }
    const index = order[low];
    return low < count && hashes[index] === hash && compare(index * 25, source, at) === 0
      ? index
      : -1;
  };
  let hash = fingerprint(bytes);
  for (let at = 0; at + 16 <= bytes.length; at++) {
    const index = find(hash, bytes, at);
    if (index >= 0 && occurrences[index] < 2) occurrences[index]++;
    if (at + 16 < bytes.length)
      hash =
        (Math.imul((hash - Math.imul(bytes[at], multiplier)) >>> 0, 31) + bytes[at + 16]) >>> 0;
  }
  return (key) => {
    const index = find(fingerprint(key), key, 0);
    requireValue(index >= 0 && occurrences[index] === 1);
    return start + index * 25;
  };
}
// 对原始 CFT 统一解析请求，防止写入的新键影响后续旧键查找。
// Resolve all requests against the original CFT so newly written keys cannot alter later old-key lookup.
function resolveCascPackRecords(vfs, entries, prepared) {
  requireValue(
    vfs instanceof Uint8Array &&
      vfs.buffer instanceof ArrayBuffer &&
      !vfs.buffer.resizable &&
      Array.isArray(entries) &&
      entries.length <= CASC_PACK_METADATA_LIMITS.entries,
  );
  const layout = prepared?.layout ?? table(vfs),
    search = prepared?.position
      ? undefined
      : (prepared?.search ?? byteKey(vfs.subarray(layout.cft, layout.end)));
  const paths = new Set(),
    keys = new Set();
  let pathBytes = 0;
  return entries.map((entry) => {
    requireValue(isObject(entry) && !Object.hasOwn(entry, "add"));
    const path = checkedPath(entry.path),
      key = unhex(entry.oldEkey),
      oldEkey = hex(key);
    requireValue(!path.includes(String.fromCharCode(127)));
    pathBytes += utf8(path).length;
    requireValue(
      pathBytes <= CASC_PACK_METADATA_LIMITS.pathBytes && !paths.has(path) && !keys.has(oldEkey),
    );
    paths.add(path);
    keys.add(oldEkey);
    const position = prepared?.position
      ? prepared.position(key)
      : cftPosition(vfs, layout.cft, layout.end, key, search);
    requireValue((position - layout.cft) % 25 === 0 && position + 25 <= layout.end);
    const specIndex = vfs[position + 20];
    return {
      path,
      oldEkey,
      position,
      specIndex,
      spec: layout.specs[specIndex],
      encodedSize: u32(vfs, position + 16),
      decodedSize: u32(vfs, position + 21),
    };
  });
}

/**
 * 检查现有路径请求对应的原始存储规格；不认证路径到旧键的关联或外部包字节。
 * Inspect original storage specs for existing-path requests; does not authenticate path-to-key association or external pack bytes.
 */
// 对单次资源流水线仅快照一次；闭包不暴露可变表或重复建立整张 CFT 字符串。
// Snapshot once per asset pipeline; expose neither mutable tables nor repeated whole-CFT string construction.
// 查询提示仅用于选择算法；小批量与未指定提示保持原字符串路径。
// The query hint only selects the algorithm; small batches and omitted hints keep the original string path.
export function createCascPackStorageLookup(vfs, { expectedQueries = 0 } = {}) {
  requireValue(
    vfs instanceof Uint8Array &&
      vfs.buffer instanceof ArrayBuffer &&
      !vfs.buffer.resizable &&
      vfs.length <= MAX_TABLE &&
      Number.isSafeInteger(expectedQueries) &&
      expectedQueries >= 0 &&
      expectedQueries <= CASC_PACK_METADATA_LIMITS.entries,
  );
  const snapshot = new Uint8Array(vfs),
    layout = table(snapshot),
    prepared =
      expectedQueries >= 2048 && layout.cftSize / 25 >= 2048
        ? { layout, position: compactCftPosition(snapshot, layout.cft, layout.end) }
        : { layout, search: byteKey(snapshot.subarray(layout.cft, layout.end)) };
  return Object.freeze((entries) =>
    Object.freeze(
      resolveCascPackRecords(snapshot, entries, prepared).map(
        ({ position: _position, ...record }) => Object.freeze(record),
      ),
    ),
  );
}

export function inspectCascPackStorageSpecs(vfs, entries) {
  return Object.freeze(
    resolveCascPackRecords(vfs, entries).map(({ position: _position, ...record }) =>
      Object.freeze(record),
    ),
  );
}

/**
 * 仅复制一次 TVFS 并改写现有引用及长度，保留原编码规格序号；不添加路径或读取载荷。
 * Copy TVFS once and rewrite existing references and lengths, retaining original spec indices; no added paths or payload reads.
 */
export function patchCascPrebuiltReferences(vfs, records) {
  const resolved = resolveCascPackRecords(vfs, records);
  const changes = resolved.map((entry, index) => {
    const record = records[index],
      key = unhex(record.newEkey);
    for (const value of [record.encodedSize, record.decodedSize])
      requireValue(Number.isSafeInteger(value) && value >= 0 && value <= 0xffffffff);
    return {
      position: entry.position,
      key,
      encodedSize: record.encodedSize,
      decodedSize: record.decodedSize,
    };
  });
  const output = new Uint8Array(vfs);
  for (const { position, key, encodedSize, decodedSize } of changes) {
    output.set(key, position);
    setCftSizes(output, position, encodedSize, decodedSize);
  }
  return output;
}

/* eslint-disable no-control-regex -- 精确 Python 换行集合。 | Exact Python line-ending set. */
function configLines(text) {
  const expression =
    /[^\r\n\v\f\x1c-\x1e\x85\u2028\u2029]*(?:\r\n|[\r\n\v\f\x1c-\x1e\x85\u2028\u2029]|$)/gu;
  return text.match(expression).filter((line) => line.length);
}
/* eslint-enable no-control-regex */
const replaceConfig = (lines, key, value) =>
  lines.map((line) => (line.startsWith(key + " = ") ? key + " = " + value + "\n" : line));
export function createCascOverlayTransform(dependencies = {}) {
  return createOverlayTransform(dependencies, false);
}
// 外部引用仅由独立入口接收，默认覆盖层契约不变。
// External references enter only through this dedicated factory; the default contract is unchanged.
export function createCascPrebuiltOverlayTransform(dependencies = {}) {
  return createOverlayTransform(dependencies, true);
}
function createOverlayTransform({ md5, compress } = {}, prebuiltMode) {
  requireValue(typeof md5 === "function" && typeof compress === "function");
  const digest = (bytes) => {
    const result = md5(bytes);
    requireValue(result instanceof Uint8Array && result.length === 16);
    return result;
  };
  const zip = async (bytes) => {
    const result = await compress(bytes);
    requireValue(result instanceof Uint8Array && result.length <= MAX_OUTPUT);
    return result;
  };
  async function blte(content, spec) {
    const rawBlocks = spec === "b:64K*=n";
    const blockSize = rawBlocks ? 64 * 1024 : 256 * 1024;
    const blocks = [];
    for (let at = 0; at < content.length; at += blockSize) {
      const raw = content.subarray(at, at + blockSize);
      blocks.push({
        encoded: concat([new Uint8Array([rawBlocks ? 78 : 90]), rawBlocks ? raw : await zip(raw)]),
        decoded: raw.length,
      });
    }
    if (!blocks.length) blocks.push({ encoded: new Uint8Array([78]), decoded: 0 });
    const headerSize = 12 + 24 * blocks.length,
      header = new Uint8Array(headerSize);
    header.set([66, 76, 84, 69]);
    put32(header, 4, headerSize);
    header[8] = 15;
    header.set(sizedWord(blocks.length, 3), 9);
    blocks.forEach((block, i) => {
      const at = 12 + i * 24;
      put32(header, at, block.encoded.length);
      put32(header, at + 4, block.decoded);
      header.set(digest(block.encoded), at + 8);
    });
    return concat([header, ...blocks.map((block) => block.encoded)]);
  }
  async function encode(content, spec) {
    requireValue(["n", "z", "b:64K*=n", "b:256K*=z"].includes(spec));
    return spec === "n" ? content.slice() : spec === "z" ? zip(content) : blte(content, spec);
  }
  function ekey(encoded, location, offset) {
    const size = byteKey(encoded.subarray(0, 4)) === "BLTE" ? u32(encoded, 4) : encoded.length;
    return concat(
      [
        digest(encoded.subarray(0, size)).subarray(0, 9),
        new Uint8Array(location),
        sizedWord(offset, 5),
      ],
      16,
    );
  }
  return async function generate(value, options) {
    requireValue(value instanceof Uint8Array || value instanceof ArrayBuffer);
    const payload = value instanceof Uint8Array ? value : new Uint8Array(value);
    requireValue(payload.length <= MAX_INPUT && isObject(options));
    const { parts, jobs, location } = options;
    requireValue(
      Array.isArray(parts) &&
        Array.isArray(jobs) &&
        jobs.length >= (prebuiltMode ? 0 : 1) &&
        jobs.length <= MAX_JOBS &&
        parts.length === (prebuiltMode ? 4 : 3) + jobs.length,
    );
    requireValue(
      Array.isArray(location) &&
        location.length === 2 &&
        location.every((n) => Number.isInteger(n) && n >= 0 && n <= 255) &&
        location[1] !== 0,
    );
    const slices = [];
    let cursor = 0;
    for (let i = 0; i < parts.length; i++) {
      const length = parts[i],
        cap = i === 0 ? 1024 * 1024 : i < 3 ? MAX_TABLE : MAX_REPLACEMENT;
      requireValue(
        Number.isInteger(length) &&
          length >= 0 &&
          length <= cap &&
          cursor + length <= payload.length,
      );
      slices.push(payload.subarray(cursor, cursor + length));
      cursor += length;
    }
    requireValue(cursor === payload.length);
    let lines = configLines(decoder.decode(slices[0]));
    const values = new Map();
    for (const line of lines) {
      if (line.includes(" = ") && !line.startsWith("#")) {
        const trimmed = line.replace(/\n+$/, ""),
          at = trimmed.indexOf(" = "),
          key = trimmed.slice(0, at),
          value = trimmed.slice(at + 3);
        requireValue(!values.has(key));
        values.set(key, value);
      }
    }
    for (const [name, raw] of [
      ["vfs-1", slices[1]],
      ["vfs-root", slices[2]],
    ]) {
      const keyText = values.get(name) ?? "",
        sizeText = values.get(name + "-size") ?? "";
      requireValue(keyText.length === 65 && /^[0-9a-fA-F]{32} [0-9a-fA-F]{32}$/.test(keyText));
      requireValue(/^[0-9]+ [0-9]+$/.test(sizeText) && !/[^0-9 ]/.test(sizeText));
      requireValue(
        hex(digest(raw)) === keyText.split(" ")[0].toLowerCase() &&
          BigInt(raw.length) === BigInt(sizeText.split(" ")[0]),
      );
    }
    let vfs = slices[1].slice();
    const root = slices[2].slice(),
      vfsTable = table(vfs),
      rootTable = table(root),
      chunks = [],
      inventory = [],
      seen = new Set(),
      additions = new Map();
    // 在任何写入前解析原始位置；不支持普通替换与外部引用重叠。
    // Resolve original positions before any mutation; ordinary/external overlap is unsupported.
    let external, externalResolved, ordinaryResolved;
    if (prebuiltMode) {
      try {
        external = JSON.parse(decoder.decode(slices.at(-1)));
      } catch {
        fail();
      }
      requireValue(
        isObject(external) && isObject(external.pack) && Array.isArray(external.records),
      );
      const { pack, records } = external;
      requireValue(records.length > 0 && records.length <= CASC_PACK_METADATA_LIMITS.entries);
      requireValue(typeof pack.name === "string" && /^[0-9]{2,3}-[0-9]{8}\.data$/.test(pack.name));
      const match = pack.name.match(/^([0-9]+)-([0-9]+)\.data$/),
        part = Number(match[1]),
        archive = Number(match[2]);
      requireValue(
        part <= 255 &&
          archive <= 255 &&
          pack.name ===
            `${String(part).padStart(2, "0")}-${String(archive).padStart(8, "0")}.data` &&
          (part !== location[0] || archive !== location[1]) &&
          Number.isSafeInteger(pack.size) &&
          pack.size > 0 &&
          pack.size <= 2 ** 40 &&
          typeof pack.sha256 === "string" &&
          /^[0-9a-f]{64}$/.test(pack.sha256),
      );
      externalResolved = resolveCascPackRecords(vfs, records).map((entry, index) => {
        const record = records[index],
          key = unhex(record.newEkey);
        let offset = 0;
        for (const byte of key.subarray(11)) offset = offset * 256 + byte;
        requireValue(
          key[9] === part &&
            key[10] === archive &&
            Number.isSafeInteger(record.encodedSize) &&
            record.encodedSize >= 0 &&
            record.encodedSize <= 0xffffffff &&
            Number.isSafeInteger(record.decodedSize) &&
            record.decodedSize >= 0 &&
            record.decodedSize <= 0xffffffff &&
            offset < 2 ** 40 &&
            offset + record.encodedSize <= pack.size,
        );
        return { ...entry, key, encodedSize: record.encodedSize, decodedSize: record.decodedSize };
      });
      const paths = new Set(externalResolved.map((r) => r.path)),
        positions = new Set(externalResolved.map((r) => r.position)),
        ordinaryPositions = new Set();
      ordinaryResolved = jobs.map((job) => {
        requireValue(isObject(job) && (!Object.hasOwn(job, "add") || typeof job.add === "boolean"));
        requireValue(!paths.has(checkedPath(job.path)));
        if (job.add) return null;
        const pos = cftPosition(vfs, vfsTable.cft, vfsTable.end, unhex(job.oldEkey));
        requireValue(
          (pos - vfsTable.cft) % 25 === 0 &&
            pos + 25 <= vfsTable.end &&
            !positions.has(pos) &&
            !ordinaryPositions.has(pos),
        );
        ordinaryPositions.add(pos);
        return pos;
      });
    }
    let outputLength = 0;
    const append = async (content, spec = "b:256K*=z") => {
      const encoded = await encode(content, spec);
      requireValue(outputLength + encoded.length <= MAX_OUTPUT);
      const key = ekey(encoded, location, outputLength);
      chunks.push(encoded);
      outputLength += encoded.length;
      return { ekey: key, decoded: content.length, encoded: encoded.length };
    };
    for (const wantAdd of [false, true]) {
      if (wantAdd && prebuiltMode) {
        for (const record of externalResolved) {
          vfs.set(record.key, record.position);
          setCftSizes(vfs, record.position, record.encodedSize, record.decodedSize);
        }
      }
      for (let i = 0; i < jobs.length; i++) {
        const job = jobs[i];
        requireValue(isObject(job) && (!Object.hasOwn(job, "add") || typeof job.add === "boolean"));
        if ((job.add ?? false) !== wantAdd) continue;
        const path = checkedPath(job.path);
        requireValue(!seen.has(path));
        seen.add(path);
        let pos, specIndex;
        if (wantAdd) {
          requireValue(job.oldEkey === undefined || job.oldEkey === null);
          const found = vfsTable.specs.indexOf("z");
          specIndex = found < 0 ? 0 : found;
        } else {
          pos = prebuiltMode
            ? ordinaryResolved[i]
            : cftPosition(vfs, vfsTable.cft, vfsTable.end, unhex(job.oldEkey));
          requireValue((pos - vfsTable.cft) % 25 === 0 && pos + 25 <= vfsTable.end);
          specIndex = vfs[pos + 20];
        }
        const result = await append(slices[i + 3], vfsTable.specs[specIndex]);
        if (wantAdd) additions.set(path, { ...result, specIndex });
        else {
          vfs.set(result.ekey, pos);
          setCftSizes(vfs, pos, result.encoded, result.decoded);
        }
        inventory.push({
          path,
          add: wantAdd,
          ekey: hex(result.ekey),
          encodedSize: result.encoded,
          decodedSize: result.decoded,
        });
      }
    }
    if (additions.size) {
      validatePathTree(vfs);
      vfs = addPaths(vfs, additions);
      table(vfs);
    }
    const vfsResult = await append(vfs),
      old = unhex(values.get("vfs-1").split(" ")[1]),
      hits = [];
    for (let pos = rootTable.cft; pos < rootTable.end; pos += 25)
      if (equal(root.subarray(pos, pos + 16), old)) hits.push(pos);
    requireValue(hits.length === 1);
    root.set(vfsResult.ekey, hits[0]);
    setCftSizes(root, hits[0], vfsResult.encoded, vfsResult.decoded);
    const rootResult = await append(root);
    lines = replaceConfig(lines, "vfs-1", hex(digest(vfs)) + " " + hex(vfsResult.ekey));
    lines = replaceConfig(lines, "vfs-1-size", `${vfsResult.decoded} ${vfsResult.encoded}`);
    const rootValue = hex(digest(root)) + " " + hex(rootResult.ekey);
    for (const name of ["vfs-root", "vfs-2"]) {
      if (values.get(name) === values.get("vfs-root")) {
        requireValue(values.has(name + "-size"));
        lines = replaceConfig(lines, name, rootValue);
        lines = replaceConfig(lines, name + "-size", `${rootResult.decoded} ${rootResult.encoded}`);
      }
    }
    const config = utf8(lines.join(""));
    requireValue(config.length + outputLength <= MAX_OUTPUT);
    return {
      files: {
        "data/.build.config": config,
        [`data/${String(location[0]).padStart(2, "0")}-${String(location[1]).padStart(8, "0")}.data`]:
          concat(chunks),
      },
      inventory,
      ...(prebuiltMode
        ? {
            external: {
              count: externalResolved.length,
              pack: {
                name: external.pack.name,
                size: external.pack.size,
                sha256: external.pack.sha256,
              },
            },
          }
        : {}),
    };
  };
}
// 固定浏览器实现组合，禁止跨 zlib 版本静默替代。 | Bind the pinned browser implementations; reject silent zlib version substitutions.
export function createPinnedCascOverlayTransform(dependencies = {}) {
  return pinnedOverlayTransform(dependencies, createCascOverlayTransform);
}
export function createPinnedCascPrebuiltOverlayTransform(dependencies = {}) {
  return pinnedOverlayTransform(dependencies, createCascPrebuiltOverlayTransform);
}
function pinnedOverlayTransform({ codec, md5Hasher } = {}, factory) {
  requireValue(
    codec?.genericZlibVersion === "1.2.13" &&
      typeof codec.compressZlib === "function" &&
      typeof md5Hasher?.init === "function" &&
      typeof md5Hasher.update === "function" &&
      typeof md5Hasher.digest === "function",
  );
  return factory({
    md5(bytes) {
      return md5Hasher.init().update(bytes).digest("binary");
    },
    compress: (bytes) => codec.compressZlib(bytes, { level: 9 }),
  });
}
