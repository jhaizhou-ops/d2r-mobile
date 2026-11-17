#include <metal_stdlib>
using namespace metal;
struct VertexResult {
  float4 position [[position]];
};
vertex VertexResult main0(constant float4 *positions [[buffer(3)]]) {
  VertexResult result;
  result.position = positions[0];
  result.position.x = positions[0].x * SCALE;
  result.position.y = positions[0].y + positions[1].y;
  return result;
}
