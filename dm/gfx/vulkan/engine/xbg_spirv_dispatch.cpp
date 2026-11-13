// 计算着色器入口的派发范围检查（#256）。Adreno 830（OnePlus 13，驱动 0x80320050）上，局部尺寸恰为 8×4×1 的计算派发在 y 方向
// 工作组数为奇数时多跑一行工作组，其余形状都准确，Adreno 730 也准确。游戏建每块光源列表的着色器（b54d192f7c620ded）正是 8×4×1，按
// 32×20 块派发 (4,5,1)：多出来的调用当成第 20–23 行写进去，写到的正是第 0–3 行第二层的列表，于是画面右上那几行块拿到别处的
// 光源，每帧不同，表现为闪烁的橙色方块。这里在入口最前面让范围之外的调用直接返回，范围之内的调用结果逐字节不变。
// 比较的是「工作组号 × 局部尺寸 + 组内号」与「工作组数 × 局部尺寸」：在这个着色器里多出来的调用工作组号在范围内、组内号越界，
// 而 830 的编译器认定组内号总小于局部尺寸，把「组内号 >= 字面量尺寸」的比较删掉了；位置的比较今天的驱动保留。风险：以后的驱动
// 若把位置比较也推理掉，闪烁会无声地回来。上限从缓冲区读的检查删不掉，但需要给每次派发传入尺寸（推送常量），改动管线布局，
// 留待需要时再做。
// The dispatch-range check at the entry of compute shaders (#256). On the Adreno 830 (OnePlus 13, driver 0x80320050), a
// compute dispatch whose local size is exactly 8x4x1 runs one extra row of work groups when its y group count is odd; every
// other shape is exact, and so is the Adreno 730. The game's per-tile light-list shader (b54d192f7c620ded) is 8x4x1, dispatched
// (4,5,1) over 32x20 tiles: the extra invocations write as tile rows 20-23, which are the second-layer lists of rows 0-3, so the
// top-right tiles of the picture get other tiles' lights, different every frame: the flickering orange rectangles. Here an
// invocation outside the range returns first thing; one inside computes exactly the same bytes.
// The check compares "work group id x local size + local id" with "number of work groups x local size". In this shader the
// extra invocations have an in-range work group id and an out-of-range local id, and the 830's compiler takes the local id to
// be below the local size and removes a "local id >= literal size" comparison; the position comparison stays with today's
// driver. The risk: a later driver that reasons the position comparison away too would bring the flicker back without a
// sound. A check whose limit is read from a buffer cannot be removed, but it needs the size passed to every dispatch (a push
// constant), which changes the pipeline layouts; that is left until it is needed.
#include "xbg_spirv_dispatch.h"

namespace {
enum : uint32_t {
    OpEntryPoint = 15, OpExecutionMode = 16, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeVector = 23,
    OpTypePointer = 32, OpConstant = 43, OpConstantComposite = 44, OpFunction = 54, OpVariable = 59, OpLoad = 61,
    OpDecorate = 71, OpIAdd = 128, OpIMul = 132, OpAny = 154, OpLogicalOr = 166, OpUGreaterThanEqual = 174, OpSelectionMerge = 247, OpLabel = 248,
    OpBranchConditional = 250, OpReturn = 253, OpExecutionModeId = 331,
    ModelGLCompute = 5, ModeLocalSize = 17, ModeLocalSizeId = 38, DecBuiltIn = 11, StorageInput = 1,
    BuiltInNumWorkgroups = 24, BuiltInWorkgroupSize = 25, BuiltInWorkgroupId = 26, BuiltInLocalInvocationId = 27,
};
inline uint32_t inst(uint32_t op, uint32_t words) { return (words << 16) | op; }
// 类型、常量、全局变量、未定义值、前向指针类型：模块里“声明区”的指令。
// Types, constants, global variables, undefined values and forward pointer types: the instructions of the declaration section.
bool is_global_decl(uint32_t op) { return (op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == OpVariable || op == 1 || op == 322; }
}

size_t xbg_spirv_dispatch_guard(const uint32_t *in, size_t n, std::vector<uint32_t> &out) {
    out.clear();
    if (!in || n < 5 || in[0] != 0x07230203u) return 0;
    uint32_t bound = in[3];
    // 三个内建输入：工作组数、工作组号、组内调用号。
    // The three built-in inputs: the number of work groups, the work group id and the local invocation id.
    static const uint32_t builtin[3] = { BuiltInNumWorkgroups, BuiltInWorkgroupId, BuiltInLocalInvocationId };
    uint32_t var[3] = { 0, 0, 0 }, var_type[3] = { 0, 0, 0 }, var_storage[3] = { 0, 0, 0 };
    size_t entry_at = 0, first_decl = 0, first_func = 0;
    uint32_t entry = 0, entries = 0, local[3] = { 0, 0, 0 };
    bool have_local = false;
    uint32_t t_void = 0, t_bool = 0, t_uint = 0, t_v3uint = 0, t_v3bool = 0;
    // 指向 uvec3 的 Input 指针类型可以重复声明，全部记下。
    // Pointer types may be declared more than once: every Input pointer to uvec3 is kept.
    std::vector<uint32_t> in_ptr;
    // glslang 的模块另有一个带 WorkgroupSize 修饰的常量，它优先于 LocalSize：记下各 uint 常量的值，供读它的三个分量。
    // A glslang module also has a constant decorated WorkgroupSize, which takes precedence over LocalSize: the values of the
    // uint constants are kept so its three components can be read.
    uint32_t size_constant = 0;
    bool size_constant_read = false;
    std::vector<uint32_t> constant_value(bound, 0);
    std::vector<uint8_t> is_constant(bound, 0);
    for (size_t i = 5; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        if (op == OpFunction) { first_func = i; break; }
        if (!first_decl && is_global_decl(op)) first_decl = i;
        if (op == OpEntryPoint && wc >= 4) {
            ++entries;
            if (in[i + 1] == ModelGLCompute) { entry = in[i + 2]; entry_at = i; }
        } else if (op == OpExecutionMode && wc >= 3 && in[i + 1] == entry && entry) {
            if (in[i + 2] == ModeLocalSize && wc == 6) { local[0] = in[i + 3]; local[1] = in[i + 4]; local[2] = in[i + 5]; have_local = true; }
        } else if (op == OpExecutionModeId && wc >= 3 && in[i + 2] == ModeLocalSizeId) {
            return 0;
        } else if (op == OpDecorate && wc == 4 && in[i + 2] == DecBuiltIn) {
            if (in[i + 3] == BuiltInWorkgroupSize) { if (size_constant) return 0; size_constant = in[i + 1]; }
            for (int k = 0; k < 3; k++) if (in[i + 3] == builtin[k]) { if (var[k]) return 0; var[k] = in[i + 1]; }
        } else if (op == OpTypeVoid && wc == 2) t_void = in[i + 1];
        else if (op == OpTypeBool && wc == 2) t_bool = in[i + 1];
        else if (op == OpTypeInt && wc == 4 && in[i + 2] == 32 && in[i + 3] == 0) t_uint = in[i + 1];
        else if (op == OpTypeVector && wc == 4 && in[i + 3] == 3) {
            if (t_uint && in[i + 2] == t_uint) t_v3uint = in[i + 1];
            else if (t_bool && in[i + 2] == t_bool) t_v3bool = in[i + 1];
        } else if (op == OpTypePointer && wc == 4 && in[i + 2] == StorageInput && t_v3uint && in[i + 3] == t_v3uint) in_ptr.push_back(in[i + 1]);
        else if (op == OpVariable && wc >= 4) {
            for (int k = 0; k < 3; k++) if (var[k] && in[i + 2] == var[k]) { var_type[k] = in[i + 1]; var_storage[k] = in[i + 3]; }
        } else if (op == OpConstant && wc == 4 && t_uint && in[i + 1] == t_uint && in[i + 2] < bound) {
            constant_value[in[i + 2]] = in[i + 3];
            is_constant[in[i + 2]] = 1;
        } else if (size_constant && op >= 41 && op <= 52 && wc >= 3 && in[i + 2] == size_constant) {
            // 只认三个普通 uint 常量组成的 OpConstantComposite；特化常量等一律不改。
            // Only an OpConstantComposite of three plain uint constants is read; a specialization constant leaves the module alone.
            if (op != OpConstantComposite || wc != 6 || !t_v3uint || in[i + 1] != t_v3uint) return 0;
            for (int k = 0; k < 3; k++) {
                const uint32_t c = in[i + 3 + k];
                if (c >= bound || !is_constant[c]) return 0;
                local[k] = constant_value[c];
            }
            size_constant_read = true;
        }
        i += wc;
    }
    if (size_constant && !size_constant_read) return 0;
    if (entries != 1 || !entry || !(have_local || size_constant_read) || !local[0] || !local[1] || !local[2]) return 0;
    if (!first_decl || !first_func || !(entry_at < first_decl && first_decl < first_func) || !t_void) return 0;
    // 已有的内建变量必须是 uvec3 的 Input 变量。
    // A built-in variable the module already has must be an Input variable of type uvec3.
    for (int k = 0; k < 3; k++) {
        if (!var[k]) continue;
        bool ok = var_storage[k] == StorageInput;
        if (ok) { ok = false; for (uint32_t p : in_ptr) if (p == var_type[k]) ok = true; }
        if (!ok) return 0;
    }
    // 入口函数：返回 void，第一条是 OpLabel，随后是它的 Function 变量。
    // The entry function: returns void, starts with its OpLabel, and its Function variables follow.
    size_t func_at = 0;
    for (size_t i = first_func; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (!wc || i + wc > n) return 0;
        if (op == OpFunction && wc == 5 && in[i + 2] == entry) { func_at = i; break; }
        i += wc;
    }
    if (!func_at || in[func_at + 1] != t_void) return 0;
    const size_t label_at = func_at + 5;
    if (label_at + 2 > n || in[label_at] != inst(OpLabel, 2)) return 0;
    const uint32_t entry_label = in[label_at + 1];
    size_t vars_end = label_at + 2;
    while (vars_end < n && (in[vars_end] & 0xffff) == OpVariable) {
        const uint32_t wc = in[vars_end] >> 16;
        if (!wc || vars_end + wc > n) return 0;
        vars_end += wc;
    }
    // 新的 id。
    // New ids.
    auto fresh = [&]() { return bound++; };
    const bool need_uint = !t_uint, need_bool = !t_bool;
    if (need_uint) t_uint = fresh();
    if (need_bool) t_bool = fresh();
    const bool need_v3uint = !t_v3uint, need_v3bool = !t_v3bool;
    if (need_v3uint) t_v3uint = fresh();
    if (need_v3bool) t_v3bool = fresh();
    const bool need_ptr = in_ptr.empty();
    const uint32_t t_ptr = need_ptr ? fresh() : in_ptr[0];
    bool created[3] = { false, false, false };
    uint32_t added = 0;
    for (int k = 0; k < 3; k++) if (!var[k]) { var[k] = fresh(); created[k] = true; ++added; }
    const uint32_t c_x = fresh(), c_y = fresh(), c_z = fresh(), c_local = fresh();
    const uint32_t v_num = fresh(), v_group = fresh(), v_local = fresh(), b_group = fresh(), b_local = fresh(), b_or = fresh(), b_any = fresh();
    const uint32_t v_base = fresh(), v_pos = fresh(), v_limit = fresh(), b_pos = fresh(), b_or2 = fresh();
    const uint32_t new_label = fresh(), return_label = fresh();
    const uint32_t entry_wc = in[entry_at] >> 16;
    if (entry_wc + added > 0xffff) return 0;

    out.reserve(n + 96);
    out.insert(out.end(), in, in + entry_at);
    // 入口点的接口表加上新建的内建变量。
    // The entry point's interface gains the built-in variables created here.
    out.push_back(inst(OpEntryPoint, entry_wc + added));
    out.insert(out.end(), in + entry_at + 1, in + entry_at + entry_wc);
    for (int k = 0; k < 3; k++) if (created[k]) out.push_back(var[k]);
    out.insert(out.end(), in + entry_at + entry_wc, in + first_decl);
    for (int k = 0; k < 3; k++) if (created[k]) { const uint32_t d[4] = { inst(OpDecorate, 4), var[k], DecBuiltIn, builtin[k] }; out.insert(out.end(), d, d + 4); }
    out.insert(out.end(), in + first_decl, in + first_func);
    auto emit = [&](std::initializer_list<uint32_t> w) { out.insert(out.end(), w); };
    if (need_uint) emit({ inst(OpTypeInt, 4), t_uint, 32, 0 });
    if (need_bool) emit({ inst(OpTypeBool, 2), t_bool });
    if (need_v3uint) emit({ inst(OpTypeVector, 4), t_v3uint, t_uint, 3 });
    if (need_v3bool) emit({ inst(OpTypeVector, 4), t_v3bool, t_bool, 3 });
    if (need_ptr) emit({ inst(OpTypePointer, 4), t_ptr, StorageInput, t_v3uint });
    for (int k = 0; k < 3; k++) if (created[k]) emit({ inst(OpVariable, 4), t_ptr, var[k], StorageInput });
    emit({ inst(OpConstant, 4), t_uint, c_x, local[0] });
    emit({ inst(OpConstant, 4), t_uint, c_y, local[1] });
    emit({ inst(OpConstant, 4), t_uint, c_z, local[2] });
    emit({ inst(OpConstantComposite, 6), t_v3uint, c_local, c_x, c_y, c_z });
    out.insert(out.end(), in + first_func, in + label_at);
    // 新的第一块：原来的 Function 变量、检查、条件返回；原来的第一块保留自己的标号，成为合并块。
    // The new first block: the original Function variables, the check and the conditional return. The original first block
    // keeps its label and becomes the merge block.
    emit({ inst(OpLabel, 2), new_label });
    out.insert(out.end(), in + label_at + 2, in + vars_end);
    emit({ inst(OpLoad, 4), t_v3uint, v_num, var[0] });
    emit({ inst(OpLoad, 4), t_v3uint, v_group, var[1] });
    emit({ inst(OpLoad, 4), t_v3uint, v_local, var[2] });
    emit({ inst(OpUGreaterThanEqual, 5), t_v3bool, b_group, v_group, v_num });
    emit({ inst(OpUGreaterThanEqual, 5), t_v3bool, b_local, v_local, c_local });
    emit({ inst(OpLogicalOr, 5), t_v3bool, b_or, b_group, b_local });
    // 位置 = 工作组号 × 局部尺寸 + 组内号，与 工作组数 × 局部尺寸 比较。只比较组内号与字面量尺寸时，驱动的编译器认定组内号总小于
    // 局部尺寸，把比较删掉（Adreno 830 上多出来的调用正是组内号越界的那些）；位置的比较它删不掉。
    // Position = work group id × local size + local id, against number of work groups × local size. Compared with the
    // literal size alone, the local id check is removed by the driver's compiler, which takes the local id to be below the
    // local size (and on the Adreno 830 the extra invocations are exactly ones whose local id is out of range); the
    // position check stays.
    emit({ inst(OpIMul, 5), t_v3uint, v_base, v_group, c_local });
    emit({ inst(OpIAdd, 5), t_v3uint, v_pos, v_base, v_local });
    emit({ inst(OpIMul, 5), t_v3uint, v_limit, v_num, c_local });
    emit({ inst(OpUGreaterThanEqual, 5), t_v3bool, b_pos, v_pos, v_limit });
    emit({ inst(OpLogicalOr, 5), t_v3bool, b_or2, b_or, b_pos });
    emit({ inst(OpAny, 4), t_bool, b_any, b_or2 });
    emit({ inst(OpSelectionMerge, 3), entry_label, 0 });
    emit({ inst(OpBranchConditional, 4), b_any, return_label, entry_label });
    emit({ inst(OpLabel, 2), return_label });
    emit({ inst(OpReturn, 1) });
    emit({ inst(OpLabel, 2), entry_label });
    out.insert(out.end(), in + vars_end, in + n);
    out[3] = bound;
    return 1;
}
