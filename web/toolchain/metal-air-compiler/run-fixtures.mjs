// 运行 AIR 编译器的固定输入用例；依据 web/toolchain/metal-air-compiler 的模块与调用约定。
// Run fixed-input cases for the AIR compiler using the module and calling contracts in web/toolchain/metal-air-compiler.
import process from "node:process";
// 仅为原生引导检查，不是浏览器运行时或 iOS 验收。
// Native bootstrap checks only. This is not the browser runtime or iOS acceptance.
import { mkdirSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { join, dirname } from "node:path";
import assert from "node:assert/strict";
const root = dirname(fileURLToPath(import.meta.url));
assert(process.argv[2], "An explicit externally built fork clang path is required");
const clang = process.argv[2];
const out = process.env.AIR_TEST_OUTPUT_DIR || join(root, "fixture-results");
mkdirSync(out, { recursive: true });
// 此运行器独用的绑定用例；按原文件名写出完全相同的源码字节。
// Binding cases used only by this runner; materialize identical source bytes under original filenames.
const bindingFixtures = {
  "buffer-bindings.metal": {
    sha256: "c0a21c1ebb3a50f393c7526e5c78af790f45aecb2c08e7322540ae64321f6b1f",
    source: String.raw`// Compiler fixture, not a complete MSL compatibility test.
// The fork's existing stage/address-space spellings isolate the new binding path.
// Expected: AIR location_index 7 for input, 2 for output, not declaration 0/1.
[[compute_kernel, kernel_dim(3)]]
void sparse(__attribute__((constant_as)) int *input [[buffer(7)]],
            __attribute__((global_as)) int *output [[buffer(2)]]) {
  int total = 0;
  for (int i = 0; i < input[0]; ++i)
    total += input[i + 1] * 3;
  output[0] = total > 9 ? total : -total;
}
`,
  },
  "buffer-binding-errors.metal": {
    sha256: "1288783f25b2de5160b1045e0a6cc11d6e5891d24d5fdf91161a635972d87b7e",
    source: String.raw`#ifdef NEGATIVE
[[compute_kernel, kernel_dim(3)]] void bad(__attribute__((global_as)) int *p [[buffer(-1)]]) { p[0] = 1; }
#endif
#ifdef RANGE
[[compute_kernel, kernel_dim(3)]] void bad(__attribute__((global_as)) int *p [[buffer(31)]]) { p[0] = 1; }
#endif
#ifdef TYPE
[[compute_kernel, kernel_dim(3)]] void bad(int p [[buffer(0)]]) {}
#endif
#ifdef DUPLICATE_ATTRIBUTE
[[compute_kernel, kernel_dim(3)]] void bad(__attribute__((global_as)) int *p [[buffer(0), buffer(1)]]) { p[0] = 1; }
#endif
#ifdef DUPLICATE_INDEX
[[compute_kernel, kernel_dim(3)]] void bad(__attribute__((global_as)) int *p [[buffer(3)]], __attribute__((global_as)) int *q [[buffer(3)]]) { p[0] = q[0]; }
#endif
#ifdef MIXED
[[compute_kernel, kernel_dim(3)]] void bad(__attribute__((global_as)) int *p [[buffer(3)]], __attribute__((global_as)) int *q) { p[0] = q[0]; }
#endif
#ifdef DYNAMIC
int index();
[[compute_kernel, kernel_dim(3)]] void bad(__attribute__((global_as)) int *p [[buffer(index())]]) { p[0] = 1; }
#endif
#ifdef PRIVATE
[[compute_kernel, kernel_dim(3)]] void bad(int *p [[buffer(0)]]) { p[0] = 1; }
#endif
`,
  },
};
for (const [name, { source, sha256 }] of Object.entries(bindingFixtures)) {
  assert.equal(
    createHash("sha256").update(source).digest("hex"),
    sha256,
    `${name}: source identity`,
  );
  mkdirSync(join(out, "fixtures"), { recursive: true });
  writeFileSync(join(out, "fixtures", name), source);
}
const base = [
  "-cc1",
  "-triple",
  "air64-apple-ios18.0.0",
  "-x",
  "metal",
  "-std=metal3.2",
  "-Werror=unknown-attributes",
  "-emit-llvm",
  "-o",
  "-",
];
function compile(file, extra = []) {
  return spawnSync(clang, [...base, ...extra, file], {
    encoding: "utf8",
    maxBuffer: 16 * 1024 * 1024,
  });
}
const positive = compile(join(out, "fixtures/buffer-bindings.metal"));
writeFileSync(join(out, "positive.stderr"), positive.stderr || String(positive.error || ""));
assert.equal(positive.status, 0, "compiler rejected positive fixture");
writeFileSync(join(out, "positive.ll"), positive.stdout);
// 检查每个参数的精确元数据，而非只确认索引在某处存在。
// Check exact per-parameter metadata, not merely that indices exist somewhere.
const definitions = positive.stdout.split("\n").filter((s) => /^!\d+ = !\{/.test(s));
for (const [name, index] of [
  ["input", 7],
  ["output", 2],
]) {
  const matching = definitions.filter((s) => s.includes(`!"air.arg_name", !"${name}"`));
  assert.equal(matching.length, 1, `one metadata record for ${name}`);
  assert(
    matching[0].includes(`!"air.location_index", i32 ${index},`),
    `${name} must keep explicit binding ${index}`,
  );
}
const errors = [
  "NEGATIVE",
  "RANGE",
  "TYPE",
  "DUPLICATE_ATTRIBUTE",
  "DUPLICATE_INDEX",
  "MIXED",
  "DYNAMIC",
  "PRIVATE",
];
for (const define of errors) {
  const result = compile(join(out, "fixtures/buffer-binding-errors.metal"), [`-D${define}`]);
  writeFileSync(join(out, `${define}.stderr`), result.stderr || String(result.error || ""));
  assert.notEqual(result.status, 0, `${define} should reject`);
  assert.match(result.stderr, /error:/, `${define} should diagnose rather than crash`);
  assert.doesNotMatch(
    result.stderr,
    /PLEASE submit a bug report|Stack dump:/,
    `${define} must not crash`,
  );
}
console.log(
  "Native compiler fixtures passed: explicit sparse buffer locations and eight rejecting cases. No WASM or Metal acceptance claimed.",
);
