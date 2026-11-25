// 检查真实 API 输出的 LLVM 文本；合成测试只验证本校验器。此样例专用读取器不是通用 LLVM 解析器或活跃性证明。
// Checks actual API-emitted LLVM text. Synthetic tests validate only this checker.
// This fixture-specific reader is not a general LLVM parser or liveness proof.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import process from "node:process";
import { pathToFileURL } from "node:url";

const entryName = "fragment_input_liveness";
function splitOperands(text) {
  const result = [];
  let start = 0,
    depth = 0,
    quoted = false;
  for (let i = 0; i < text.length; ++i) {
    const c = text[i];
    if (quoted && c === "\\") {
      ++i;
      continue;
    }
    if (c === '"') quoted = !quoted;
    if (quoted) continue;
    if ("([{<".includes(c)) ++depth;
    else if (")]}>".includes(c)) --depth;
    else if (c === "," && depth === 0) {
      result.push(text.slice(start, i).trim());
      start = i + 1;
    }
    assert(depth >= 0, "balanced metadata operands required");
  }
  assert(!quoted && depth === 0, "complete metadata operands required");
  if (text.slice(start).trim()) result.push(text.slice(start).trim());
  return result;
}
function decodeOperand(token) {
  if (/^!\d+$/.test(token)) return { ref: token.slice(1) };
  if (/^!"[^"\\]*"$/.test(token)) return token.slice(2, -1);
  if (/^i32 -?\d+$/.test(token)) return Number(token.slice(4));
  return { raw: token };
}
function getValue(descriptor, key) {
  const index = descriptor.indexOf(key);
  assert(index >= 0, `missing ${key}`);
  assert.equal(descriptor.lastIndexOf(key), index, `duplicate ${key}`);
  return descriptor[index + 1];
}

export function checkFragmentInputLiveness(
  ir,
  { keepAll = false, keepVarying = keepAll, keepPosition = keepAll } = {},
) {
  assert.equal(typeof keepAll, "boolean");
  assert.equal(typeof keepVarying, "boolean");
  assert.equal(typeof keepPosition, "boolean");
  const nodes = new Map();
  for (const match of ir.matchAll(/^!(\d+) = (?:distinct )?!\{(.*)\}\s*$/gm)) {
    assert(!nodes.has(match[1]), "duplicate metadata node");
    nodes.set(match[1], splitOperands(match[2]).map(decodeOperand));
  }
  const resolve = (reference) => {
    assert.equal(typeof reference?.ref, "string", "metadata reference required");
    const node = nodes.get(reference.ref);
    assert(node, `missing metadata node ${reference.ref}`);
    return node;
  };
  const roots = [...ir.matchAll(/^!air\.fragment = !\{(.*)\}\s*$/gm)];
  assert.equal(roots.length, 1, "exactly one fragment metadata root required");
  const entries = splitOperands(roots[0][1]).map(decodeOperand);
  assert.equal(entries.length, 1, "fixture has exactly one fragment entry");
  const entry = resolve(entries[0]);
  assert.equal(entry.length, 3, "fixture entry metadata protocol");
  assert.match(
    entry[0]?.raw ?? "",
    new RegExp(`@${entryName}$`),
    "metadata must name fixture entry",
  );
  const definitions = [
    ...ir.matchAll(
      new RegExp(`^define [^\\n]*@${entryName}\\(([^\\n]*)\\)[^\\n]*\\{\\n([\\s\\S]*?)^\\}`, "gm"),
    ),
  ];
  assert.equal(definitions.length, 1, "fixture entry definition required");
  const parameters = splitOperands(definitions[0][1]);
  assert.equal(parameters.length, 5, "retained arguments must not be removed");
  for (let i = 0; i < 3; ++i)
    assert.match(parameters[i], /^<4 x float>\s/, `stage argument ${i} type`);
  for (let i = 3; i < 5; ++i)
    assert.match(
      parameters[i],
      /^<4 x float> addrspace\(2\)\*\s/,
      `buffer argument ${i} type/address space`,
    );

  // 受限样例检查：以非调试函数体内的局部值标记判断各保留参数是否有文本使用。生产终结器使用 LLVM Argument::use_empty()，不是此解析器。
  // Constrained fixture check: local-value tokens in this non-debug function
  // body establish whether each preserved parameter has any textual use.
  // The production finalizer uses LLVM Argument::use_empty(), not this parser.
  const body = definitions[0][2].replace(/;[^\n]*/g, "");
  const uses = new Set(body.match(/%"(?:[^"\\]|\\.)*"|%[-a-zA-Z$._0-9]+/g) ?? []);
  const argumentUsed = parameters.map((parameter) => {
    const name = parameter.match(/(%"(?:[^"\\]|\\.)*"|%[-a-zA-Z$._0-9]+)$/)?.[1];
    assert(name, "named fixture parameter required");
    return uses.has(name);
  });
  assert.deepEqual(
    argumentUsed,
    [true, keepVarying, keepPosition, true, false],
    "actual final fixture parameter uses",
  );
  const outputs = resolve(entry[1]).map(resolve);
  assert(
    outputs.length > 0 && outputs.every((output) => !output.includes("air.arg_unused")),
    "output descriptors must not receive input liveness markers",
  );

  const descriptors = resolve(entry[2]).map(resolve);
  assert.equal(descriptors.length, 5, "all input/resource descriptors retained");
  assert.deepEqual(
    descriptors.map((input) => input[0]),
    [0, 1, 2, 3, 4],
    "preserved argument indices/order",
  );
  const expected = [
    [
      0,
      "air.fragment_input",
      "user(locn7)",
      "air.flat",
      "air.arg_type_name",
      "float4",
      "air.arg_name",
      "kept",
    ],
    [
      1,
      "air.fragment_input",
      "user(locn11)",
      "air.flat",
      "air.arg_type_name",
      "float4",
      "air.arg_name",
      "optionalValue",
    ],
    [
      2,
      "air.position",
      "air.center",
      "air.no_perspective",
      "air.arg_type_name",
      "float4",
      "air.arg_name",
      "position",
    ],
  ];
  if (!keepVarying) expected[1].push("air.arg_unused");
  if (!keepPosition) expected[2].push("air.arg_unused");
  assert.deepEqual(
    descriptors.slice(0, 3),
    expected,
    "exact stage-input descriptors and terminal unused flags",
  );
  for (const [index, binding, name] of [
    [3, 9, "bias"],
    [4, 13, "untouched"],
  ]) {
    const descriptor = descriptors[index];
    assert.equal(descriptor[1], "air.buffer");
    assert.equal(getValue(descriptor, "air.location_index"), binding);
    assert.equal(getValue(descriptor, "air.arg_name"), name);
    assert.equal(getValue(descriptor, "air.address_space"), 2);
    assert(!descriptor.includes("air.arg_unused"), "resources are outside this finalizer's scope");
  }
  return { entry: entryName, keepAll, keepVarying, keepPosition, argumentUsed, descriptors };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [mode, path] = process.argv.slice(2);
  const variants = {
    live: { keepVarying: true, keepPosition: true },
    dead: { keepVarying: false, keepPosition: false },
    "varying-dead": { keepVarying: false, keepPosition: true },
    "position-dead": { keepVarying: true, keepPosition: false },
  };
  assert(
    Object.hasOwn(variants, mode) && path,
    "usage: node check-fragment-input-liveness.mjs live|dead|varying-dead|position-dead /path/to/API-output.ll",
  );
  checkFragmentInputLiveness(readFileSync(path, "utf8"), variants[mode]);
  console.log(`${mode}: exact fragment-input liveness metadata passed`);
}
