#include <metal_stdlib>
using namespace metal;
struct FragmentResult {
  float4 color [[color(0)]];
};
fragment FragmentResult main0(constant float4 *colors [[buffer(5)]]) {
  FragmentResult result;
  float4 chosen = colors[0];
  if (chosen.x > THRESHOLD) chosen = colors[1];
  result.color = chosen;
  return result;
}
