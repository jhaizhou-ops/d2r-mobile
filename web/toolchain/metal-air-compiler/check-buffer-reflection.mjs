// 检查真实编译器输出的 AIR LLVM 文本，不合成 AIR。
// Inspect real compiler-emitted AIR LLVM text. This does not synthesize AIR.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import process from "node:process";
import { pathToFileURL } from "node:url";

export function readReflection(ir, fixture) {
  assert.match(ir, /!air\.kernel\s*=/, "real AIR kernel metadata is required");
  assert(ir.includes(`@${fixture.entry}(`), "expected entry must exist in emitted IR");
  const nodes = new Map();
  for (const match of ir.matchAll(/^!(\d+)\s*=\s*!\{(.*)\}\s*$/gm)) {
    const tokens = [];
    for (const token of match[2].matchAll(/!"((?:[^"\\]|\\.)*)"|!(\d+)|i\d+\s+(-?\d+)/g)) {
      if (token[1] !== undefined) tokens.push(token[1]);
      else if (token[2] !== undefined) tokens.push({ ref: token[2] });
      else tokens.push(Number(token[3]));
    }
    nodes.set(match[1], tokens);
  }
  const value = (tokens, key) => {
    const index = tokens.indexOf(key);
    assert(index >= 0, `missing ${key}`);
    return tokens[index + 1];
  };
  const sourceNodes = [...nodes.values()].filter(
    (tokens) =>
      tokens.includes("air.buffer") &&
      tokens.includes("air.arg_name") &&
      value(tokens, "air.arg_name") === fixture.buffer.name,
  );
  assert.equal(sourceNodes.length, 1, "source buffer descriptor must be unique");
  const source = sourceNodes[0];
  const readFields = (reference) => {
    assert.equal(typeof reference?.ref, "string", "struct metadata reference required");
    const tokens = nodes.get(reference.ref);
    assert(tokens, `missing metadata node ${reference.ref}`);
    const fields = [];
    for (let i = 0; i < tokens.length;) {
      let children;
      if (tokens[i] === "air.struct_type_info") {
        children = readFields(tokens[i + 1]);
        i += 2;
      }
      const [offset, size, count, type, name] = tokens.slice(i, i + 5);
      assert([offset, size, count].every(Number.isInteger), "ordinary field descriptor required");
      assert.equal(typeof type, "string");
      assert.equal(typeof name, "string");
      fields.push({ name, offset, size, count, ...(children ? { children } : {}) });
      i += 5;
    }
    return fields;
  };
  return {
    name: value(source, "air.arg_name"),
    binding: value(source, "air.location_index"),
    addressSpace: value(source, "air.address_space"),
    size: value(source, "air.arg_type_size"),
    bufferSize: value(source, "air.buffer_size"),
    align: value(source, "air.arg_type_align_size"),
    fields: readFields(value(source, "air.struct_type_info")),
  };
}

export function checkReflection(ir, fixture) {
  assert.deepEqual(readReflection(ir, fixture), {
    ...fixture.buffer,
    bufferSize: fixture.buffer.size,
  });
}

// 刻意只覆盖自编 PaddedVectorLayout 探针，不是通用 LLVM 解析器。检查输出存储类型和实际源 GEP，不仅信任反射或 dereferenceable(80)：后两者原本已为 80 字节，而旧输出类型在最终 DataLayout 下占 112 字节。
// Deliberately bounded to the authored PaddedVectorLayout probe, not a general
// LLVM parser. Check emitted storage types and the actual source GEPs, rather
// than trusting reflection or dereferenceable(80), which were already 80 bytes
// while the old emitted type occupied 112 bytes under its final DataLayout.
export function checkFloat3Storage(ir) {
  const dataLayout = ir.match(/^target datalayout = "([^"]+)"$/m)?.[1];
  assert(dataLayout, "emitted storage DataLayout is required");
  assert(
    !dataLayout.split("-").some((part) => /^a(?:\d+)?:/.test(part)),
    "aggregate alignment overrides are outside the emitted AIR probe profile",
  );
  const alignment = (key) => {
    const clauses = dataLayout.split("-").filter((part) => part.startsWith(`${key}:`));
    assert.equal(clauses.length, 1, `one ${key} ABI alignment is required`);
    const bits = Number(clauses[0].split(":")[1]);
    assert(Number.isSafeInteger(bits) && bits > 0 && bits % 8 === 0);
    return bits / 8;
  };
  const round = (size, align) => Math.ceil(size / align) * align;
  const layout = (type) => {
    const scalar = { i8: [1, "i8"], i32: [4, "i32"], float: [4, "f32"] }[type];
    if (scalar) {
      const align = alignment(scalar[1]);
      return { type, align, size: round(scalar[0], align) };
    }
    if (type === "<3 x float>") {
      const align = alignment("v96");
      return { type, align, size: round(12, align) };
    }
    const array = type.match(/^\[(\d+) x (.+)\]$/);
    assert(array, `unsupported public float3 probe type: ${type}`);
    const count = Number(array[1]);
    assert(Number.isSafeInteger(count) && count > 0 && count <= 128);
    const element = layout(array[2]);
    return { type, align: element.align, size: count * element.size, count, element };
  };
  const records = [...ir.matchAll(/^%struct\.PaddedVectorLayout = type \{ ([^\n]+) \}$/gm)];
  assert.equal(records.length, 1, "one ordinary public probe storage type is required");
  const fields = records[0][1].split(", ").map(layout);
  let size = 0;
  let align = 1;
  for (const field of fields) {
    align = Math.max(align, field.align);
    field.offset = round(size, field.align);
    size = field.offset + field.size;
  }
  assert.equal(align, 16, "actual storage alignment must be 16 bytes");
  assert.equal(round(size, align), 80, "actual storage allocation must be 80 bytes");
  const path = fields.filter((field) => field.type === "[2 x <3 x float>]");
  assert.equal(path.length, 1);
  assert.equal(path[0].offset, 32, "path array must start at byte 32");
  assert.equal(path[0].element.size, 16, "actual float3 array stride must be 16 bytes");
  const entry = ir.match(/^define void @reflection_float3\(([^\n]+)\) [^\n]*\{\n([\s\S]*?)^\}/m);
  assert(entry, "expected real float3 entry body is required");
  assert.match(entry[1], /%struct\.PaddedVectorLayout addrspace\(2\)\*[^,]* %source,/);
  const geps = [
    ...entry[2].matchAll(
      /^\s*(%[-a-zA-Z$._0-9]+) = getelementptr inbounds %struct\.PaddedVectorLayout, %struct\.PaddedVectorLayout addrspace\(2\)\* %source, ([^\n]+)/gm,
    ),
  ];
  const escape = (value) => value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const uses = (value) =>
    [...entry[2].matchAll(new RegExp(`${escape(value)}(?![-a-zA-Z$._0-9])`, "g"))].length;
  assert.equal(uses("%source"), geps.length, "source pointer may only feed the checked GEPs");
  const accesses = geps.map((gep) => {
    const indices = gep[2].split(", ").map((token) => {
      const index = token.match(/^i(?:32|64) (\d+)$/);
      assert(index, "expected constant public probe GEP index");
      return Number(index[1]);
    });
    assert.equal(indices[0], 0, "source GEP must stay within its first record");
    assert(indices.length === 2 || indices.length === 3);
    let field = fields[indices[1]];
    assert(field, "source GEP field must exist");
    let offset = field.offset;
    if (indices.length === 3) {
      assert(field.element && indices[2] < field.count, "source array GEP must be in bounds");
      offset += indices[2] * field.element.size;
      field = field.element;
    }
    assert.equal(uses(gep[1]), 2, "each checked address has one definition and one load use");
    assert.match(
      entry[2],
      new RegExp(
        `\\bload ${escape(field.type)}, ${escape(field.type)} addrspace\\(2\\)\\* ${escape(gep[1])}, align \\d+`,
      ),
      "the expected typed load must use the checked source address",
    );
    return { offset, type: field.type };
  });
  assert.deepEqual(
    accesses.sort((a, b) => a.offset - b.offset),
    [
      { offset: 0, type: "i32" },
      { offset: 16, type: "<3 x float>" },
      { offset: 48, type: "<3 x float>" },
      { offset: 64, type: "i32" },
    ],
    "emitted source GEPs must select state, direction, path[1] and tail at their ABI offsets",
  );
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [entry, irPath] = process.argv.slice(2);
  const fixtures = JSON.parse(
    readFileSync(new URL("fixtures/buffer-reflection.json", import.meta.url), "utf8"),
  );
  const fixture = fixtures.find((candidate) => candidate.entry === entry);
  assert(
    fixture && irPath,
    "usage: node check-buffer-reflection.mjs ENTRY /path/to/real-compiler-output.ll",
  );
  checkReflection(readFileSync(irPath, "utf8"), fixture);
  console.log(`${entry}: expected ordinary buffer reflection passed`);
}
