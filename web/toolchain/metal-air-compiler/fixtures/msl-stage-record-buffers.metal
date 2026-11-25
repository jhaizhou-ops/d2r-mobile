// SPDX-License-Identifier: MIT
// Independently authored storage/interface regression, not an application shader.
#include <metal_stdlib>
using namespace metal;

struct BufferedStageRecord {
  float4 value [[attribute(0)]];
};

kernel void stage_record_buffers(
    BufferedStageRecord input [[stage_in]],
    constant BufferedStageRecord *source [[buffer(0)]],
    device BufferedStageRecord *output [[buffer(1)]]) {
  output[0].value = input.value + source[0].value;
}
