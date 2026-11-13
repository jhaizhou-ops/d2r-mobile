// SPDX-License-Identifier: MIT
// Independent fixed CI input, not an SDK or application shader.
#include <metal_stdlib>
using namespace metal;
struct AtomicFragmentResult {
  float4 color [[color(0)]];
};
fragment AtomicFragmentResult main0(device atomic_uint *counters [[buffer(0)]],
                                    constant uint *input [[buffer(1)]]) {
  uint previous = atomic_fetch_add_explicit(&counters[input[0]], input[1], memory_order_relaxed);
  return AtomicFragmentResult{float4(previous == input[2] ? 1.0f : 0.0f)};
}
