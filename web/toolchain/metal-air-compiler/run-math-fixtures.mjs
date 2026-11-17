// 运行 AIR 编译器的数学运算用例；依据 web/toolchain/metal-air-compiler 的模块与调用约定。
// Run mathematical-operation cases for the AIR compiler using the module and calling contracts in web/toolchain/metal-air-compiler.
import assert from "node:assert/strict";
import { materializeNumericFixture } from "./fixtures/numeric-cases.mjs";
import process from "node:process";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join, resolve } from "node:path";
assert(
  process.argv[2] && process.argv[3],
  "Explicit external fork clang and header-directory paths are required",
);
const root = dirname(fileURLToPath(import.meta.url));
const out = process.env.AIR_TEST_OUTPUT_DIR || join(root, "fixture-results");
const clang = resolve(process.argv[2] || "");
assert(
  process.argv[2],
  "Usage: node run-math-fixtures.mjs /path/to/clang /path/to/fork/headers [authored-api-test-build-directory]",
);
const headers = resolve(process.argv[3]);
const base = [
  "-cc1",
  "-triple",
  "air64-apple-ios18.0.0",
  "-x",
  "metal",
  "-std=metal3.2",
  "-O2",
  "-flegacy-pass-manager",
  "-Werror=unknown-attributes",
  "-I",
  headers,
  "-emit-llvm",
  "-o",
  "-",
];
for (const name of ["math-scalar", "math-vector", "integer-preservation"]) {
  const r = spawnSync(clang, [...base, "-DAIR_FAST_MATH=1", materializeNumericFixture(name, out)], {
    encoding: "utf8",
    maxBuffer: 8 * 1024 * 1024,
  });
  assert.equal(r.status, 0, r.stderr);
  if (name.startsWith("math-"))
    for (const symbol of [
      "fma",
      "fast_sqrt",
      "fast_rsqrt",
      "fast_exp2",
      "fast_log2",
      "fast_floor",
      "fast_trunc",
      "fast_rint",
      "fast_fabs",
      "fast_fmin",
      "fast_fmax",
      "fast_clamp",
    ])
      assert(r.stdout.includes("@air." + symbol + "."), symbol);
  if (name === "math-scalar")
    for (const symbol of ["fast_sin", "fast_cos", "fast_ceil"])
      assert(r.stdout.includes("@air." + symbol + ".f32"), symbol);
  if (name === "integer-preservation") {
    assert(
      !/uitofp|sitofp|@air\.convert\.f\./.test(r.stdout),
      "Integer values must not pass through float",
    );
    for (const symbol of ["abs.s.i32", "reverse_bits.i32"])
      assert(r.stdout.includes("@air." + symbol), symbol);
  }
  assert(!/@llvm\.(sqrt|exp2|sin|cos|ceil|bitreverse)/.test(r.stdout));
}
const prefix = "#include <metal_stdlib>\nusing namespace metal; ";
for (const source of [
  "kernel void k(device float *o [[buffer(0)]]) {o[0]=sqrt(o[0]);}",
  "kernel void k(device int2 *o [[buffer(0)]]) {o[0]=abs(o[0]);}",
  "kernel void k(device uint *o [[buffer(0)]]) {o[0]=as_type<uint>((ulong)1);}",
  "bool4 unsupported;",
  "kernel void k(device float *o [[buffer(0)]]) {o[0]=fast::sqrt(42);}",
  "kernel void k(device float *o [[buffer(0)]]) {half a=1; o[0]=fma(a,a,a);}",
]) {
  const r = spawnSync(clang, [...base, "-"], { input: prefix + source, encoding: "utf8" });
  assert.equal(r.status, 1, r.stderr);
  assert(r.stderr.includes("error:"));
  assert(!/Assertion|PLEASE submit a bug report/.test(r.stderr));
}
if (process.argv[4])
  for (const name of ["d2r_air_api_smoke", "d2r_air_header_smoke"]) {
    const r = spawnSync(join(resolve(process.argv[4]), name), [], { encoding: "utf8" });
    assert.equal(r.status, 0, r.stdout + "\n" + r.stderr);
  }
console.log(
  "Nine authored math fixtures passed" +
    (process.argv[4] ? ", with API baseline and header/profile regression suites." : "."),
);
