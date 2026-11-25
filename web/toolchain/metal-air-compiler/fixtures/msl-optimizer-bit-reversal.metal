// 原创回归输入：只用位移和掩码，触发优化器的位反转识别。
// Authored regression: shifts and masks trigger optimizer bit-reversal recognition.
#include <metal_stdlib>
using namespace metal;

kernel void main0(device uint *values [[buffer(0)]]) {
  uint bits = values[0];
  bits = (bits >> 16) | (bits << 16);
  bits = ((bits >> 8) & 0x00ff00ffu) | ((bits << 8) & 0xff00ff00u);
  bits = ((bits >> 4) & 0x0f0f0f0fu) | ((bits << 4) & 0xf0f0f0f0u);
  bits = ((bits >> 2) & 0x33333333u) | ((bits << 2) & 0xccccccccu);
  bits = ((bits >> 1) & 0x55555555u) | ((bits << 1) & 0xaaaaaaaau);
  values[0] = bits;
}
