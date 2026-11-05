// SPDX-License-Identifier: MIT
// Independently authored padded-vector element-stride regression.
#include <metal_stdlib>
using namespace metal;

struct PaddedVectorLayout {
  uint state;
  float3 direction;
  float3 path[2];
  uint tail;
};

kernel void reflection_float3(
    constant PaddedVectorLayout &source [[buffer(0)]],
    device float4 *output [[buffer(1)]]) {
  output[0] = float4(source.direction + source.path[1],
                     float(source.state + source.tail));
}
