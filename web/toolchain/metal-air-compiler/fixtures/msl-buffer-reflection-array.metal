// SPDX-License-Identifier: MIT
// Independently authored array-only alignment regression.
#include <metal_stdlib>
using namespace metal;

struct ArrayLayout {
  float4 values[9];
};

kernel void reflection_array(
    constant ArrayLayout &source [[buffer(0)]],
    device float4 *output [[buffer(1)]]) {
  output[0] = source.values[7];
}
