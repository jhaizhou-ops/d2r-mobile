// fragment_input_liveness 探针的补充专用 AIRR 读取器，不替代容器/哈希校验，不执行 Metal，不证明 LLVM 使用计数，也不代表 Apple 运行时接受。
// Supplemental fixture-specific AIRR reader for the
// fragment_input_liveness canary. This does not replace container/hash checking,
// execute Metal, prove LLVM use counts, or establish Apple runtime acceptance.
const decoder = new TextDecoder("utf-8", { fatal: true });
const MAX_BYTES = 16 * 1024 * 1024;
const MAX_NODES = 4096;
const KINDS = {
  1: "FragmentFunction",
  262144: "Buffer",
  274432: "FragmentInput",
  274434: "Position",
};
function fail(message) {
  throw new Error(`Fragment reflection rejected: ${message}`);
}
function requireValue(condition, message) {
  if (!condition) fail(message);
}

class Reader {
  constructor(bytes) {
    requireValue(bytes instanceof Uint8Array, "Uint8Array bytes required");
    requireValue(bytes.length <= MAX_BYTES, "size limit");
    this.bytes = bytes;
    this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }
  range(offset, size, end = this.bytes.length) {
    requireValue(
      Number.isSafeInteger(offset) &&
        Number.isSafeInteger(size) &&
        Number.isSafeInteger(end) &&
        offset >= 0 &&
        size >= 0 &&
        end <= this.bytes.length &&
        offset <= end &&
        size <= end - offset,
      "range out of bounds",
    );
  }
  u8(p) {
    this.range(p, 1);
    return this.view.getUint8(p);
  }
  u16(p) {
    this.range(p, 2);
    return this.view.getUint16(p, true);
  }
  u32(p) {
    this.range(p, 4);
    return this.view.getUint32(p, true);
  }
  i32(p) {
    this.range(p, 4);
    return this.view.getInt32(p, true);
  }
  u64(p) {
    this.range(p, 8);
    const n = this.view.getBigUint64(p, true);
    requireValue(n <= BigInt(Number.MAX_SAFE_INTEGER), "unsafe 64-bit offset");
    return Number(n);
  }
  tag(p) {
    this.range(p, 4);
    return String.fromCharCode(...this.bytes.subarray(p, p + 4));
  }
  tags(start, end) {
    this.range(start, end - start);
    const result = new Map();
    let p = start;
    while (p < end) {
      this.range(p, 4, end);
      const name = this.tag(p);
      p += 4;
      if (name === "ENDT") {
        requireValue(p === end, "trailing tag data");
        return result;
      }
      this.range(p, 2, end);
      const size = this.u16(p);
      p += 2;
      this.range(p, size, end);
      requireValue(!result.has(name), `duplicate ${name} tag`);
      result.set(name, { offset: p, size });
      p += size;
    }
    fail("missing ENDT");
  }
  field(tags, name, size) {
    const value = tags.get(name);
    requireValue(value && (size === undefined || value.size === size), `missing/invalid ${name}`);
    return value.offset;
  }
  cstring(offset, size) {
    this.range(offset, size);
    requireValue(
      size >= 2 && size <= 1024 && this.u8(offset + size - 1) === 0,
      "invalid string length/terminator",
    );
    const bytes = this.bytes.subarray(offset, offset + size - 1);
    requireValue(!bytes.includes(0), "embedded string terminator");
    return decoder.decode(bytes);
  }
  // 每次 FlatBuffer 读取均限制在本 AIRR 子数组中，不访问整个 MTLB。
  // Every FlatBuffer read is confined to this AIRR subarray, never whole MTLB.
  table(root) {
    requireValue(root >= 8 && root % 4 === 0, "unaligned table root");
    this.range(root, 4);
    const delta = this.i32(root),
      vtable = root - delta;
    requireValue(delta !== 0 && vtable >= 8 && vtable % 2 === 0, "invalid vtable offset");
    this.range(vtable, 4);
    const vsize = this.u16(vtable),
      inlineSize = this.u16(vtable + 2);
    requireValue(
      vsize >= 4 && vsize <= 1024 && vsize % 2 === 0 && inlineSize >= 4,
      "invalid table sizes",
    );
    this.range(vtable, vsize);
    this.range(root, inlineSize);
    requireValue(vtable + vsize <= root || root + inlineSize <= vtable, "overlapping table/vtable");
    const offsets = [];
    for (let p = vtable + 4; p < vtable + vsize; p += 2) {
      const value = this.u16(p);
      requireValue(value === 0 || (value >= 4 && value < inlineSize), "invalid field offset");
      offsets.push(value);
    }
    return { root, vtable, vsize, inlineSize, offsets };
  }
  slot(table, field, width, required = false) {
    const offset = table.offsets[field] ?? 0;
    if (offset === 0) {
      requireValue(!required, `missing field ${field}`);
      return null;
    }
    requireValue(width <= table.inlineSize - offset, "field exceeds inline data");
    const p = table.root + offset;
    this.range(p, width);
    return p;
  }
  scalar(table, field, width, fallback = null) {
    const p = this.slot(table, field, width);
    return p === null ? fallback : width === 1 ? this.u8(p) : this.u32(p);
  }
  target(pointer, width = 4) {
    const relative = this.u32(pointer);
    requireValue(relative >= 4, "non-forward reference");
    const target = pointer + relative;
    this.range(target, width);
    return target;
  }
  vector(table, field, required = true, maxCount = MAX_NODES) {
    const p = this.slot(table, field, 4, required);
    if (p === null) return [];
    const start = this.target(p);
    requireValue(start % 4 === 0, "unaligned vector");
    const count = this.u32(start);
    requireValue(count <= maxCount, "vector count limit");
    this.range(start + 4, count * 4);
    return Array.from({ length: count }, (_, i) => start + 4 + i * 4);
  }
  string(table, field, required = true) {
    const p = this.slot(table, field, 4, required);
    if (p === null) return null;
    const start = this.target(p),
      size = this.u32(start);
    requireValue(size <= 1023, "string size limit");
    return this.cstring(start + 4, size + 1);
  }
  bool(table, field) {
    const p = this.slot(table, field, 1);
    if (p === null) return { present: false, value: null, offset: null };
    const value = this.u8(p);
    requireValue(value === 0 || value === 1, "noncanonical boolean");
    return { present: true, value: value === 1, offset: p };
  }
}

export function inspectFragmentReflection(bytes) {
  const r = new Reader(bytes);
  r.range(0, 88);
  requireValue(r.tag(0) === "MTLB" && r.u64(16) === bytes.length, "invalid MTLB/length");
  const sections = [24, 40, 56, 72].map((p, i) => {
    const start = r.u64(p),
      size = r.u64(p + 8) + (i === 0 ? 4 : 0);
    r.range(start, size);
    requireValue(start >= 88 && size >= 4, "invalid section");
    return { start, size, end: start + size };
  });
  for (let i = 1; i < sections.length; ++i)
    requireValue(sections[i].start >= sections[i - 1].end, "overlapping sections");
  const [functions, publicMetadata, , bitcode] = sections;
  const count = r.u32(functions.start);
  requireValue(count === 1, "one fixture entry required");
  let cursor = functions.start + 4;
  const entries = [];
  for (let i = 0; i < count; ++i) {
    r.range(cursor, 4, functions.end);
    const size = r.u32(cursor);
    requireValue(size >= 8, "invalid function group");
    r.range(cursor, size, functions.end);
    const tags = r.tags(cursor + 4, cursor + size),
      name = tags.get("NAME");
    requireValue(name, "missing entry name");
    entries.push({
      name: r.cstring(name.offset, name.size),
      type: r.u8(r.field(tags, "TYPE", 1)),
      reflectionOffset: tags.has("RFLT") ? r.u64(r.field(tags, "RFLT", 8)) : null,
    });
    cursor += size;
  }
  requireValue(cursor === functions.end, "function list size mismatch");
  const program = r.tags(functions.end, publicMetadata.start);
  if (!program.has("RLST")) {
    requireValue(
      entries.every((x) => x.reflectionOffset === null) && bitcode.end === bytes.length,
      "undeclared reflection tail",
    );
    return { reflectionAvailable: false, entries };
  }
  const rl = r.field(program, "RLST", 16),
    start = r.u64(rl),
    size = r.u64(rl + 8);
  r.range(start, size);
  requireValue(
    size >= 4 && start >= bitcode.end && start + size === bytes.length,
    "invalid RLST range",
  );
  requireValue(r.u32(start) === count, "reflection count mismatch");
  let relative = 4;
  const results = [];
  for (const entry of entries) {
    requireValue(entry.reflectionOffset === relative, "RFLT/RLST offset mismatch");
    r.range(start + relative, 12, start + size);
    const group = start + relative,
      groupSize = r.u32(group),
      payloadSize = r.u32(group + 8);
    requireValue(
      groupSize >= 24 && payloadSize === groupSize - 16 && r.tag(group + 4) === "RBUF",
      "invalid RBUF",
    );
    r.range(group, groupSize, start + size);
    requireValue(r.tag(group + groupSize - 4) === "ENDT", "missing RBUF ENDT");
    const payload = group + 12,
      padding = (16 - (payload % 16)) % 16;
    requireValue(payloadSize >= padding + 8, "short AIRR payload");
    requireValue(
      bytes.subarray(payload, payload + padding).every((x) => x === 0),
      "nonzero padding",
    );
    const airrStart = payload + padding,
      airrSize = payloadSize - padding;
    const a = new Reader(bytes.subarray(airrStart, airrStart + airrSize));
    requireValue(airrSize % 4 === 0 && a.tag(4) === "AIRR", "invalid AIRR");
    const root = a.table(a.u32(0)),
      version = a.slot(root, 0, 12, true);
    requireValue(
      a.u32(version) === 0 && a.u32(version + 4) === 4 && a.u32(version + 8) === 0,
      "unsupported AIRR version",
    );
    const nodeSlots = a.vector(root, 1);
    const nodes = nodeSlots.map((slot, id) => {
      const wrapper = a.table(a.target(slot)),
        kind = a.scalar(wrapper, 0, 4);
      const payload = a.table(a.target(a.slot(wrapper, 1, 4, true)));
      return { id, kind, payload };
    });
    const node = (id) => {
      requireValue(Number.isInteger(id) && id >= 0 && id < nodes.length, "node ID out of range");
      return nodes[id];
    };
    const fragmentIds = a.vector(root, 2, true, 1).map((p) => a.u32(p));
    requireValue(fragmentIds.length === 1, "one fixture fragment function required");
    const fragments = fragmentIds.map((id) => {
      const fn = node(id);
      requireValue(fn.kind === 1, "fragment ID is not FragmentFunction");
      const argumentSlots = a.vector(fn.payload, 2, true, 5);
      requireValue(argumentSlots.length === 5, "all five arguments required");
      const argumentIds = argumentSlots.map((p) => a.u32(p));
      requireValue(new Set(argumentIds).size === 5, "duplicate argument node IDs");
      const args = argumentIds.map(node).map((n) => {
        const t = n.payload,
          result = {
            id: n.id,
            kind: KINDS[n.kind] ?? n.kind,
            tableOffset: t.root,
            vtableOffset: t.vtable,
          };
        if (n.kind === 274432)
          Object.assign(result, {
            name: a.string(t, 6),
            type: a.string(t, 5),
            attribute: a.string(t, 1),
            location: a.scalar(t, 2, 4),
            sampling: a.scalar(t, 3, 1),
            interpolation: a.scalar(t, 4, 1),
            unused: a.bool(t, 7),
          });
        else if (n.kind === 274434)
          Object.assign(result, {
            name: a.string(t, 4),
            type: a.string(t, 3),
            sampling: a.scalar(t, 1, 1, 1),
            interpolation: a.scalar(t, 2, 1, 3),
            unused: a.bool(t, 5),
          });
        else if (n.kind === 262144)
          Object.assign(result, {
            name: a.string(t, 11),
            type: a.string(t, 10),
            binding: a.scalar(t, 2, 4),
            count: a.scalar(t, 3, 4),
            addressSpace: a.scalar(t, 5, 1, 1),
            unused: a.bool(t, 12),
          });
        return result;
      });
      return { id, name: a.string(fn.payload, 0), arguments: args };
    });
    results.push({ ...entry, airrStart, airrSize, nodeCount: nodes.length, fragments });
    relative += groupSize;
  }
  requireValue(relative === size, "unaccounted reflection tail");
  return { reflectionAvailable: true, entries: results };
}

export function checkFragmentInputReflection(
  bytes,
  { keepVarying = false, keepPosition = false } = {},
) {
  requireValue(
    typeof keepVarying === "boolean" && typeof keepPosition === "boolean",
    "boolean options required",
  );
  const result = inspectFragmentReflection(bytes);
  requireValue(
    result.reflectionAvailable && result.entries.length === 1,
    "one reflected entry required",
  );
  const entry = result.entries[0];
  requireValue(
    entry.name === "fragment_input_liveness" && entry.type === 1 && entry.fragments.length === 1,
    "fixture fragment entry required",
  );
  const fn = entry.fragments[0];
  requireValue(fn.name === entry.name && fn.arguments.length === 5, "all five arguments required");
  const args = fn.arguments;
  requireValue(new Set(args.map((x) => x.id)).size === 5, "duplicate argument node IDs");
  const expected = [
    ["FragmentInput", "kept"],
    ["FragmentInput", "optionalValue"],
    ["Position", "position"],
    ["Buffer", "bias"],
    ["Buffer", "untouched"],
  ];
  for (let i = 0; i < 5; ++i)
    requireValue(
      args[i].kind === expected[i][0] &&
        args[i].name === expected[i][1] &&
        args[i].type === "float4",
      `argument ${i} kind/name/type/order changed`,
    );
  for (const [i, location] of [
    [0, 7],
    [1, 11],
  ])
    requireValue(
      args[i].attribute === `user(locn${location})` && args[i].interpolation === 2,
      `argument ${i} attribute/flat qualifier changed`,
    );
  requireValue(
    args[2].sampling === 1 && args[2].interpolation === 3,
    "position qualifiers changed",
  );
  for (const [i, binding] of [
    [3, 9],
    [4, 13],
  ])
    requireValue(
      args[i].binding === binding && args[i].count === 1 && args[i].addressSpace === 2,
      `buffer ${i} binding/count/address space changed`,
    );
  const expectedUnused = [false, !keepVarying, !keepPosition, false, false];
  for (let i = 0; i < 5; ++i) {
    const unused = args[i].unused;
    // 严格遵循写入器约定：活跃或范围外字段应缺席，而非显式 false；死字段必须存在且为 true，忽略 air.arg_unused 不能通过。
    // Strict writer contract: live/out-of-scope fields are absent, not explicit false.
    // For dead fields require presence AND true; ignoring air.arg_unused cannot pass.
    requireValue(
      expectedUnused[i] ? unused.present && unused.value === true : !unused.present,
      `argument ${i} unused presence/value mismatch`,
    );
  }
  return {
    assertion: "fragment-input-binary-reflection",
    unusedInputs: expectedUnused.filter(Boolean).length,
    arguments: args,
    airrStart: entry.airrStart,
    airrSize: entry.airrSize,
  };
}
