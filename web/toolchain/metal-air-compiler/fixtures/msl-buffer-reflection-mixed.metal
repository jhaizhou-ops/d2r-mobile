// SPDX-License-Identifier: MIT
// Independently authored mixed-layout regression, not an application shader.
#include <metal_stdlib>
using namespace metal;

struct MixedLayout {
  uint token;
  uint4 table[3];
  float4 origin;
  float4 scale;
  int mode;
  float4 weights[5];
  int4 ranges;
  int end;
};

kernel void reflection_mixed(
    constant MixedLayout &source [[buffer(0)]],
    device float4 *output [[buffer(1)]]) {
  output[0] = source.origin + source.scale + source.weights[1] +
      float4(source.table[2]) + float4(source.ranges) +
      float4(float(source.token + uint(source.mode + source.end)));
}
