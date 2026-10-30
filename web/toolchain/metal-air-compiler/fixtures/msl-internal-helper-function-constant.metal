// 原创回归输入：保留内部辅助函数并生成函数常量谓词初始化器。
// Authored regression: retain an internal helper and generate a function-constant predicate initializer.
#include <metal_stdlib>
using namespace metal;

constant bool enable_color [[function_constant(17)]];

static __attribute__((noinline)) float add_bias(float value) {
  return value + 0.25f;
}

struct FragmentResult {
  float4 color [[color(0), function_constant(enable_color)]];
};

fragment FragmentResult main0(constant float *values [[buffer(0)]]) {
  FragmentResult result;
  result.color = float4(add_bias(values[0]));
  return result;
}
