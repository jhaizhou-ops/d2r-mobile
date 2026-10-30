// SPDX-License-Identifier: MIT
// Independently authored nested-record array-stride regression.
#include <metal_stdlib>
using namespace metal;

struct LayoutLeaf {
  float4 payload;
  uint code;
};

struct NestedLayout {
  uint tag;
  LayoutLeaf items[2];
  uint footer;
};

kernel void reflection_nested(
    constant NestedLayout &source [[buffer(0)]],
    device float4 *output [[buffer(1)]]) {
  output[0] = source.items[1].payload +
      float4(float(source.tag + source.items[0].code + source.footer));
}
