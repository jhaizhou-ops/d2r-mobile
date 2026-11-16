// 计算着色器入口的派发范围检查：落在派发范围之外的调用立即返回（见 xbg_spirv_dispatch.cpp）
// A dispatch-range check at the entry of a compute shader: an invocation outside the dispatched range returns at once (see
// xbg_spirv_dispatch.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// 给只有一个 GLCompute 入口的模块在入口函数最前面加上：
//   if (any(gl_WorkGroupID >= gl_NumWorkGroups) || any(gl_LocalInvocationID >= 局部尺寸) ||
//       any(gl_WorkGroupID * 局部尺寸 + gl_LocalInvocationID >= gl_NumWorkGroups * 局部尺寸)) return;
// 局部尺寸取字面量 LocalSize（翻译器的模块），或带 WorkgroupSize 修饰的普通常量（glslang 的模块，它优先于 LocalSize）。
// 范围之内的调用结果不变。返回 1 表示已加（out 为新模块）；返回 0 表示模块原样使用（out 为空），包括不是计算模块、入口不止
// 一个、局部尺寸是 LocalSizeId 或特化常量、入口函数不是 void、或内建变量不是 uvec3 输入。
// For a module with a single GLCompute entry point, puts this first in the entry function:
//   if (any(gl_WorkGroupID >= gl_NumWorkGroups) || any(gl_LocalInvocationID >= local size) ||
//       any(gl_WorkGroupID * local size + gl_LocalInvocationID >= gl_NumWorkGroups * local size)) return;
// The local size is the literal LocalSize (the translator's modules) or a plain constant decorated WorkgroupSize (glslang's
// modules, where it takes precedence over LocalSize). An invocation inside the range computes exactly what it did.
// Returns 1 when the check was added (out holds the new module) and 0 when the module is to be used unchanged (out is
// empty): not a compute module, more than one entry point, a local size given by LocalSizeId or a specialization constant,
// an entry function that does not return void, or a built-in variable that is not a uvec3 input.
size_t xbg_spirv_dispatch_guard(const uint32_t *words, size_t count, std::vector<uint32_t> &out);
