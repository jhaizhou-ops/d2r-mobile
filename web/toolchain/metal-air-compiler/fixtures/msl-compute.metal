#include <metal_stdlib>
using namespace metal;
kernel void main0(constant int *input [[buffer(7)]], device int *output [[buffer(2)]]) {
  int sum = 0;
  for (int i = 0; i < input[0]; ++i) {
    const int v = input[i + 1];
    if ((v & 1) == 0) sum += v * FACTOR;
    else sum -= v;
  }
  output[0] = sum;
}
