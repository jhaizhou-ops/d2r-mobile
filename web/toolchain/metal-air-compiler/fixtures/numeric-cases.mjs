// 仅供原生数值回归的自编输入；保留原始文件名、源码字节及摘要。
// Authored native numeric regressions only; preserve original filenames, source bytes and digests.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";

export const numericFixtures = Object.freeze({
  "math-scalar": {
    sha256: "768f21ae692332c1cafe2ff9a8eb5ad8fc5471257bb63f701a82a2da61d985a5",
    source: String.raw`#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;
kernel void scalar_math(device float *out [[buffer(0)]], constant float *in [[buffer(1)]]) {
 float x=in[0], y=in[1], z=in[2];
 out[0]=fma(x,y,z); out[1]=sqrt(x); out[2]=rsqrt(x);
 out[3]=exp2(x); out[4]=log2(x); out[5]=floor(x);
 out[6]=trunc(x); out[7]=rint(x); out[8]=abs(x);
 out[9]=fast::min(x,y); out[10]=fast::max(x,y); out[11]=fast::clamp(x,y,z);
 out[12]=select(x,y,isnan(z));
 out[13]=sin(x); out[14]=cos(y); out[15]=ceil(z);
}
`,
  },
  "math-vector": {
    sha256: "07197d64e28f2e4cc77edc864814e82bbd73eff732203a07a8176a9aeace74e4",
    source: String.raw`#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;
kernel void vector_math(device float4 *out [[buffer(0)]], constant float4 *in [[buffer(1)]]) {
 simd::float4 x=in[0], y=in[1], z=in[2];
 out[0]=fma(x,y,z); out[1]=fast::sqrt(x); out[2]=fast::rsqrt(x);
 out[3]=fast::exp2(x); out[4]=fast::log2(x); out[5]=fast::floor(x);
 out[6]=fast::trunc(x); out[7]=fast::rint(x); out[8]=fast::abs(x);
 out[9]=fast::min(x,y); out[10]=fast::max(x,y); out[11]=fast::clamp(x,y,z);
}
`,
  },
  "integer-preservation": {
    sha256: "01335a6f6c36735866a42898ccf93252beb7569f18bde090d42cb3ceb50cb9c3",
    source: String.raw`#include <metal_stdlib>
using namespace metal;
kernel void integer_ops(device uint *out [[buffer(0)]], constant uint *in [[buffer(1)]]) {
 out[0]=min(in[0],in[1]); out[1]=max(in[0],in[1]);
 out[2]=clamp(in[0],in[1],in[2]);
 out[3]=as_type<uint>(as_type<float>(in[0]));
 out[4]=as_type<uint>(abs(as_type<int>(in[0])));
 out[5]=reverse_bits(in[0]);
 out[6]=as_type<uint>(reverse_bits(as_type<int>(in[1])));
}
`,
  },
  "nan-select": {
    sha256: "af310a0c46ec6a18d27ff253bc4c1e9e0a867561c687f3e27aeca99dd21de707",
    source: String.raw`#include <metal_stdlib>
using namespace metal;
kernel void classify(device uint *out [[buffer(0)]], constant float *in [[buffer(1)]]) {
 out[0]=isnan(in[0]);
}
kernel void sanitize(device float2 *out [[buffer(0)]], constant float2 *in [[buffer(1)]]) {
 out[0]=select(in[0],float2(0.0f),isnan(in[0]));
}
`,
  },
});

export function materializeNumericFixture(name, outputDirectory) {
  assert(Object.hasOwn(numericFixtures, name), "Unknown numeric fixture");
  const { source, sha256 } = numericFixtures[name];
  assert.equal(
    createHash("sha256").update(source).digest("hex"),
    sha256,
    `${name}: source identity`,
  );
  const path = join(outputDirectory, "fixtures", `${name}.metal`);
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, source);
  return path;
}
