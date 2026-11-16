// 片元四元组邻居的保守导数改写，基于PR14 a90826a及SPIR-V规范；不把未知流当浮点数。
// Conservative fragment quad-neighbour derivative rewrite, based on PR14 a90826a and SPIR-V; unknown flows are never presumed numeric.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

enum class xbg_quad_skip : uint32_t {
    malformed, module_contract, non_float, lane_index, control_flow,
    bit_pattern, integer_flow, unknown_source, unknown_use, memory_flow, count
};
// 辅助函数调用形式（Ryujinx 把 SHFL 译成辅助函数，见 xbg_spirv_quad.cpp）的跳过原因。
// Skip reasons of the helper-call form (the translator's SHFL helper functions; see xbg_spirv_quad.cpp).
enum class xbg_quad_call_skip : uint32_t { module_contract, helper, arguments, control_flow, source, use, count };
struct xbg_spirv_quad_stats {
    size_t candidates=0, rewritten=0, guarded_nonfinite=0;
    size_t skipped[static_cast<size_t>(xbg_quad_skip::count)]={};
    size_t calls=0, calls_rewritten=0;
    size_t call_skipped[static_cast<size_t>(xbg_quad_call_skip::count)]={};
    // 放宽模式下才改写的调用：按浮点改写的（结果另有按位用途，或值的直接来源是浮点运算 / 读取），和值来自浮点转整数的
    // （转回浮点、取邻居、再转整数）。
    // Calls rewritten only in relaxed mode: those rewritten as floats (the result also has bit uses, or the value's immediate
    // producers are float arithmetic or loads) and those whose value comes from a float-to-integer conversion (converted back
    // to float, neighbour taken, converted to integer again).
    size_t calls_relaxed_float=0, calls_integer=0;
};
const char *xbg_spirv_quad_skip_name(xbg_quad_skip reason);
const char *xbg_spirv_quad_call_skip_name(xbg_quad_call_skip reason);

// 返回改写数（shuffle 与辅助函数调用之和）；0时out为空，调用方使用原模块。stats每次调用重置，供每shader记录。
// 数值重建存在有限浮点舍入；非有限输入/中间值回退原shuffle，不声称位精确或真机改善。
// Return rewrite count (shuffles plus helper calls); when zero, out is empty and callers retain the original module. Reset stats per call for per-shader reporting.
// Numerical reconstruction can round finite values; nonfinite inputs/intermediates fall back to the original shuffle, with no bit-exact or device-benefit claim.
size_t xbg_spirv_quad_derivatives(const uint32_t *words,size_t count,std::vector<uint32_t> &out,
                                  xbg_spirv_quad_stats *stats);
size_t xbg_spirv_quad_derivatives(const uint32_t *words,size_t count,std::vector<uint32_t> &out);
// 放宽模式，给片元阶段不支持子组操作的驱动（Adreno 650 旧驱动、Maleoon 910：supportedStages 只有计算阶段）。否则这些着色器
// 根本不能用，所以：结果另有按位用途、或值的直接来源是浮点运算 / 浮点读取的调用照样按浮点改写；值来自浮点转整数
// （OpConvertFToS / OpConvertFToU）的调用转回浮点取邻居再转回整数，|v| < 2^24 时精确；主函数里直接的 shuffle 不留回退。
// 调用仍须在统一控制流里，其他来源仍然跳过。
// Relaxed mode, for drivers that do not support subgroup operations in the fragment stage (old Adreno 650 drivers, Maleoon
// 910: supportedStages holds only compute). The shaders cannot be used there otherwise, so: a call whose result also has bit
// uses, or whose value's immediate producers are float arithmetic or float loads, is still rewritten as floats; a call whose
// value comes from a float-to-integer conversion (OpConvertFToS /
// OpConvertFToU) is converted back to float, the neighbour taken and converted to integer again, exact for |v| < 2^24; a
// shuffle directly in the entry function keeps no fallback. Calls must still be in uniform control flow, and any other
// source is still skipped.
size_t xbg_spirv_quad_derivatives(const uint32_t *words,size_t count,std::vector<uint32_t> &out,
                                  xbg_spirv_quad_stats *stats,bool relaxed);
// 改写之后去掉片元着色器里不再用到的子组部分：入口点调不到的函数（改写后没人调用的 shuffle 辅助函数）、没有用处的子组内置
// 输入变量（连同其注解和入口点接口里的条目），以及在没有任何子组指令和子组内置变量时的 GroupNonUniform* 能力声明。
// 返回去掉的项数；0 时 out 为空。
// After the rewrite, remove the subgroup parts a fragment shader no longer uses: functions the entry point cannot reach (the
// shuffle helper nobody calls after the rewrite), subgroup built-in input variables without uses (with their annotations and
// their entry in the entry point's interface), and the GroupNonUniform* capability declarations once no subgroup instruction
// or subgroup built-in variable remains. Returns the number of items removed; when zero, out is empty.
size_t xbg_spirv_strip_subgroup(const uint32_t *words,size_t count,std::vector<uint32_t> &out);
