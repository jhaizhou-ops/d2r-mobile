// SPDX-License-Identifier: MIT
// Independently authored storage/interface regression, not an application shader.
#include <metal_stdlib>
using namespace metal;

struct SharedStageRecord {
  float4 value [[attribute(0)]];
};

kernel void stage_record_threadgroup(
    SharedStageRecord input [[stage_in]],
    threadgroup SharedStageRecord *scratch [[threadgroup(0)]],
    device float4 *output [[buffer(0)]],
    uint lane [[thread_index_in_threadgroup]]) {
  scratch[lane].value = input.value;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  output[lane] = scratch[lane].value;
}
