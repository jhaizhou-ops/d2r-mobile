// SPDX-License-Identifier: MIT
// Independently authored public regression. No game source or reference bytes.
#include <metal_stdlib>
#include <metal_tessellation>
using namespace metal;

#ifndef PATCH_ENTRY_NAME
#define PATCH_ENTRY_NAME patch_linkage_probe
#endif
#ifndef PATCH_CONTROL_POINTS
#define PATCH_CONTROL_POINTS 3
#endif

struct LinkageControlPoint {
  float4 position [[attribute(0)]];
};
struct LinkagePatchInput {
  patch_control_point<LinkageControlPoint> points;
};
struct LinkagePatchOutput {
  float4 position [[position]];
};

// Helpers still have C++ linkage: exempting every Metal function from mangling
// would make these overloads conflict. Inline them to isolate entry naming from
// unrelated helper-linkage lowering.
__attribute__((always_inline)) float entry_bias(float value) {
  return value + 0.25f;
}
__attribute__((always_inline)) float entry_bias(uint value) {
  return float(value) + 0.5f;
}

[[patch(triangle, PATCH_CONTROL_POINTS)]] vertex LinkagePatchOutput PATCH_ENTRY_NAME(
    LinkagePatchInput input [[stage_in]], float3 coordinate [[position_in_patch]]) {
  LinkagePatchOutput output;
  output.position = input.points[0].position;
  output.position.x += entry_bias(coordinate.x);
  output.position.y += entry_bias(input.points.size());
  return output;
}
