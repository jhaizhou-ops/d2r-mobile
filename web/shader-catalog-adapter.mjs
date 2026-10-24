// 依据固定的目录源摘要和原生 loc/pc_of 规则映射着色器记录。
// Map shader records using pinned catalog-source digests and native loc/pc_of rules.
const authenticatedCatalogs = new WeakSet();
export function assertShaderCatalog(value) {
  if (!authenticatedCatalogs.has(value)) throw new Error("Unauthenticated shader catalog");
}
/**
 * 仅映射源码固定的目录。不进行获取、翻译、发布证明或打包。
 * Source-pinned catalog mapping only. No fetch, translation, release attestation or packaging.
 */
export const SHADER_CATALOG_SOURCES = Object.freeze({
  map: "419dae7adddbaba455045b1d121a01671fcb3bc82fc5d7d193593255df23c628",
  facts: "5f50b1d9934b2f4e85ebddbd62114c6f5acb94ae42b8509fcfc3486e7953f490",
  conflicts: "5e554c11cf2732d90539eb334e84a744ef88f527bc280b9256f3d72185f25501",
});
const HEX = /^[a-f0-9]{16}$/;
const control = new Uint8Array([0x34, 0x12, 0x76, 0x98]);
const codeMarker = new Uint8Array([0x78, 0x56, 0x34, 0x12]);
const recordMarker = new TextEncoder().encode("BTCDBTCD");
const sha = async (bytes) =>
  [...new Uint8Array(await crypto.subtle.digest("SHA-256", bytes))]
    .map((x) => x.toString(16).padStart(2, "0"))
    .join("");
function bytes(value, max, name) {
  if (!(value instanceof Uint8Array) || !value.length || value.length > max)
    throw new Error(`Invalid bounded ${name} bytes`);
  return value;
}
function match(data, at, marker) {
  return marker.every((v, i) => data[at + i] === v);
}
function find(data, marker, start = 0, end = data.length) {
  if (!marker.length) return start <= end ? start : -1;
  // 原生字节搜索跳过不可能的起点；视图限制搜索范围且不复制内容。
  // Native byte search skips impossible starts; a view bounds the search without copying content.
  const search = end === data.length ? data : data.subarray(0, end);
  for (
    let i = search.indexOf(marker[0], start);
    i >= 0 && i <= end - marker.length;
    i = search.indexOf(marker[0], i + 1)
  )
    if (match(data, i, marker)) return i;
  return -1;
}
function supported(stage, optIn = false) {
  if (!["cs", "vs", "ps"].includes(stage) && !(optIn === true && ["hs", "ds"].includes(stage)))
    throw new Error(`Unsupported catalog stage ${stage}; record retained, full catalog incomplete`);
}
function options({ debug, reduced }) {
  if (typeof debug !== "boolean" || typeof reduced !== "boolean")
    throw new Error("Explicit debug/reduced options required");
}
/**
 * 可独立调用的记录解码器；描述符不是发布配方。
 * Independently callable record decoder; descriptor is NOT a release recipe.
 */
export async function decodeShaderRecord(descriptor, input, flags) {
  supported(descriptor.stage, flags.allowUnverifiedTessellation);
  options(flags);
  if (!HEX.test(descriptor.key)) throw new Error("Invalid control key");
  // 哈希等待前复制到独立的普通字节数组；Buffer.slice() 仅借用原存储。
  // Copy into an owned plain byte array before hashing; Buffer.slice() only borrows storage.
  const rec = new Uint8Array(bytes(input, 2 * 1024 * 1024, "shader record"));
  let code,
    returned = false;
  try {
    const ci = find(rec, control);
    if (ci < 0x38 || ci + 0x880 > rec.length)
      throw new Error("Missing/truncated shader control segment");
    if ((await sha(rec.subarray(ci, ci + 0x880))).slice(0, 16) !== descriptor.key)
      throw new Error("Control hash does not match catalog key");
    const view = new DataView(rec.buffer);
    const c1o = view.getUint32(ci + 0x700, true),
      c1t = view.getUint32(ci + 0x704, true);
    if (c1o > c1t || c1t > ci - 0x38 || ![0, 256, 512, 768].includes(c1t - c1o))
      throw new Error("Invalid c1 range");
    const start = 0x38 + (descriptor.stage === "cs" ? 0 : 0x30);
    code = rec.slice(start, ci);
    if (
      code.length < (descriptor.stage === "cs" ? 16 : 0x60) ||
      code.length > 1024 * 1024 ||
      code.length % 8
    )
      throw new Error("Invalid shader code bounds");
    if (descriptor.stage !== "cs") {
      const stage = (new DataView(code.buffer).getUint32(0, true) >>> 10) & 15;
      if (stage !== { vs: 1, hs: 2, ds: 3, ps: 5 }[descriptor.stage])
        throw new Error("SPH stage disagrees with catalog");
    }
    if (descriptor.stage === "hs" && new DataView(code.buffer).getUint32(8, true) >>> 24 === 0)
      throw new Error("Hull SPH requires nonzero threads per input primitive");
    const facts = structuredClone(descriptor.facts);
    if (
      !facts ||
      ["attr", "fout", "img", "tex"].some(
        (k) => !facts[k] || typeof facts[k] !== "object" || Array.isArray(facts[k]),
      )
    )
      throw new Error("Missing explicit facts");
    if (
      Object.keys(facts).some((k) => !["attr", "fout", "img", "tex", "local", "shared"].includes(k))
    )
      throw new Error("Unexpected fact field");
    const prepared = {
      job: {
        key: descriptor.key,
        stage: descriptor.stage,
        tex: {},
        img: {},
        attr: {},
        fout: {},
        c1: [...rec.subarray(0x38 + c1o, 0x38 + c1t)]
          .map((x) => x.toString(16).padStart(2, "0"))
          .join(""),
        debug: flags.debug,
        reduced: flags.reduced,
        ...(["hs", "ds"].includes(descriptor.stage) ? { allowUnverifiedTessellation: true } : {}),
        ...facts,
      },
      code,
      source: {
        name: descriptor.name,
        index: descriptor.index,
        pcPaired: descriptor.pcPaired,
        controlVerified: true,
      },
      installable: false,
    };
    returned = true;
    return prepared;
  } finally {
    // 返回的代码拥有独立缓冲区；失败时清理尚未交给调用方的代码。
    // Returned code owns a separate backing; wipe unreturned code on failure.
    rec.fill(0);
    if (!returned) code?.fill(0);
  }
}
/**
 * 转移整个独立缓冲区以消除调用方别名；仅缓存固定目录需要的记录边界，不缓存解码结果。
 * Transfer the complete owned backing to detach caller aliases; cache only catalog-bounded record offsets, never decoded results.
 */
function createOwnedPackSource(name, input, entries, maximumIndex) {
  if (maximumIndex === undefined) throw new Error("Unknown shader pack source");
  const original = bytes(input, 256 * 1024 * 1024, "shader pack");
  if (
    !(original.buffer instanceof ArrayBuffer) ||
    original.buffer.resizable === true ||
    original.byteOffset !== 0 ||
    original.byteLength !== original.buffer.byteLength
  )
    throw new Error("Invalid owned shader pack backing");
  // 先分配小型索引，再转移私有包；分配失败不得剥夺调用方的所有权。
  // Allocate the small index before transferring the private pack; allocation failure must leave caller ownership intact.
  let offsets = new Uint32Array(maximumIndex + 2),
    pack;
  let count = 0,
    start = 0,
    eof = false,
    disposed = false;
  const dispose = () => {
    disposed = true;
    pack?.fill(0);
    offsets?.fill(0);
    pack = offsets = undefined;
  };
  try {
    pack = structuredClone(original, { transfer: [original.buffer] });
  } catch (error) {
    dispose();
    throw error;
  }
  const check = (signal) => {
    signal?.throwIfAborted();
    if (disposed) throw new Error("Shader pack source is disposed");
  };
  return Object.freeze({
    async prepare(key, flags = {}) {
      let prepared;
      try {
        check(flags.signal);
        const descriptor = entries.get(key);
        if (!descriptor || descriptor.embedded || descriptor.name !== name)
          throw new Error("Shader key does not belong to pack source");
        supported(descriptor.stage, flags.allowUnverifiedTessellation);
        options(flags);
        // 与原生 re.finditer 一样跳过完整标记；末尾只补一次 EOF，不收集目录之外的标记。
        // Advance past full markers like native re.finditer; memoize EOF and never collect markers beyond the catalog bound.
        while (count <= descriptor.index + 1 && !eof) {
          check(flags.signal);
          const found = find(pack, recordMarker, start);
          if (found < 0) {
            eof = true;
            break;
          }
          offsets[count++] = found;
          start = found + recordMarker.length;
        }
        check(flags.signal);
        if (count <= descriptor.index) throw new Error("Catalog record absent from pack");
        const end = count > descriptor.index + 1 ? offsets[descriptor.index + 1] : pack.length;
        prepared = await decodeShaderRecord(
          descriptor,
          pack.subarray(offsets[descriptor.index], end),
          flags,
        );
        check(flags.signal);
        return prepared;
      } catch (error) {
        // 哈希等待期间取消或释放时，不能泄漏随后返回的独立代码；失败使此所有者终止。
        // Cancellation/disposal during hashing must not leak subsequently returned code; a failed owner is terminal.
        prepared?.code.fill(0);
        dispose();
        throw error;
      }
    },
    dispose,
  });
}
export async function loadShaderCatalog(input) {
  const texts = {};
  for (const [name, expected] of Object.entries(SHADER_CATALOG_SOURCES)) {
    // Buffer 输入也必须持有独立快照，保证解析字节与摘要检查一致。
    // Buffer inputs also require owned snapshots so parsed bytes match the checked digest.
    const data = new Uint8Array(bytes(input[name], 8 * 1024 * 1024, name));
    if ((await sha(data)) !== expected) throw new Error(`Pinned ${name} source digest mismatch`);
    texts[name] = new TextDecoder("utf-8", { fatal: true }).decode(data);
  }
  const facts = new Map();
  for (const line of texts.facts.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const [key, json] = line.split("\t");
    if (!HEX.test(key) || facts.has(key)) throw new Error("Invalid/duplicate fact key");
    facts.set(key, JSON.parse(json));
  }
  const entries = new Map();
  const stages = { 计算: "cs", 顶点: "vs", 像素: "ps", 域: "ds", 外壳: "hs" };
  for (const line of texts.map.split("\n")) {
    if (!line || line.startsWith("#") || line.startsWith("package\t")) continue;
    const [name, indexText, label, key, pcPaired] = line.split("\t");
    const embedded = /^main\.rodata@[a-f0-9]+$/.test(name);
    if (!embedded && !/^[a-zA-Z0-9_]+\.bin$/.test(name))
      throw new Error("Unsafe catalog source name");
    const index = embedded ? Number.parseInt(name.split("@")[1], 16) : Number(indexText);
    const stage = embedded ? stages[label] : name.slice(0, 2);
    if (
      !Number.isSafeInteger(index) ||
      index < 0 ||
      !HEX.test(key) ||
      !HEX.test(pcPaired) ||
      stage !== stages[label] ||
      !facts.has(key)
    )
      throw new Error("Invalid catalog row/facts");
    // 原始 loc/pc_of 使用 setdefault：第一个物理源优先。
    // Original loc/pc_of use setdefault: first physical source wins.
    if (!entries.has(key))
      entries.set(key, { key, name, index, stage, pcPaired, embedded, facts: facts.get(key) });
  }
  for (const line of texts.conflicts.split("\n")) {
    if (!line || line.startsWith("#") || line.startsWith("dm_")) continue;
    const [key, pc] = line.split("\t");
    if (!HEX.test(pc) || !entries.has(key)) throw new Error("Invalid conflict override");
    entries.get(key).pcPaired = pc;
  }
  if (entries.size !== 6927 || facts.size !== entries.size)
    throw new Error("Incomplete pinned catalog");
  // 当前提交的 bf_want 策略是生成的空列表。可选运行时
  // 字节必须匹配同一精确来源，绝不能使用调用方添加内容。
  // Current committed bf_want policy is an empty generated list. Optional runtime
  // bytes must match the same exact source, never caller-provided additions.
  const bfWant = input.bfWant ?? new TextEncoder().encode("# generated\n");
  if (
    (await sha(bytes(bfWant, 1024 * 1024, "bf_want"))) !==
    "8e91845e3b2e8793a97b7a0e7c33c58ad92761e7d2ed7ffe60f8fb0e196649cf"
  )
    throw new Error("Pinned bf_want source digest mismatch");
  const packMaximumIndexes = new Map();
  for (const entry of entries.values())
    if (!entry.embedded)
      packMaximumIndexes.set(
        entry.name,
        Math.max(packMaximumIndexes.get(entry.name) ?? -1, entry.index),
      );
  const catalog = Object.freeze({
    bfWant: Object.freeze([]),
    keys: Object.freeze([...entries.keys()]),
    unsupported: Object.freeze(
      [...entries.values()]
        .filter((e) => !["cs", "vs", "ps"].includes(e.stage))
        .map((e) => Object.freeze({ key: e.key, stage: e.stage, name: e.name })),
    ),
    describe(key) {
      const e = entries.get(key);
      if (!e) throw new Error("Unknown catalog key");
      return structuredClone(e);
    },
    createOwnedPackSource(name, packBytes) {
      return createOwnedPackSource(name, packBytes, entries, packMaximumIndexes.get(name));
    },
    async prepare(
      key,
      { packBytes, mainImage, debug, reduced, allowUnverifiedTessellation = false } = {},
    ) {
      const e = entries.get(key);
      if (!e) throw new Error("Unknown catalog key");
      supported(e.stage, allowUnverifiedTessellation);
      options({ debug, reduced });
      let rec;
      if (e.embedded) {
        // mainImage 必须已经是原生 nsoimg.load 内存镜像布局，而非原始 NSO。
        // mainImage must already be the native nsoimg.load memory-image layout, NOT raw NSO.
        const image = bytes(mainImage, 512 * 1024 * 1024, "decompressed main image");
        const o = e.index;
        if (o < 0x1000 || o + 0x880 > image.length || !match(image, o, control))
          throw new Error("Invalid embedded control location");
        let g = -1;
        for (
          let i = find(image, codeMarker, o - 0x1000, o);
          i >= 0;
          i = find(image, codeMarker, i + 1, o)
        )
          g = i;
        if (g < 0) throw new Error("Embedded code marker missing");
        rec = new Uint8Array(0x38 + o + 0x880 - g);
        rec.set(image.subarray(g, o + 0x880), 0x38);
      } else {
        const pack = bytes(packBytes, 256 * 1024 * 1024, "shader pack");
        let count = 0,
          begin = -1,
          end = pack.length;
        // Python re.finditer 使用不重叠匹配。 | Python re.finditer uses non-overlapping matches.
        for (
          let i = find(pack, recordMarker);
          i >= 0;
          i = find(pack, recordMarker, i + recordMarker.length)
        ) {
          if (count === e.index) begin = i;
          if (count === e.index + 1) {
            end = i;
            break;
          }
          count++;
        }
        if (begin < 0) throw new Error("Catalog record absent from pack");
        rec = pack.subarray(begin, end);
      }
      return decodeShaderRecord(e, rec, { debug, reduced, allowUnverifiedTessellation });
    },
    productionEquivalent: false,
  });
  authenticatedCatalogs.add(catalog);
  return catalog;
}
