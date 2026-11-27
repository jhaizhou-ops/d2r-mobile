// 自适应出图的整数分档与线程CPU统计；不改变 nvn_pace.h 的 Apple「至少60」策略。
// Integer-divisor adaptive tiers and thread-CPU sampling; leaves nvn_pace.h's Apple "at least 60" policy unchanged.
#pragma once
#include <stdint.h>

static inline int nvn_adaptive_min_divisor(double hz) {
    double ratio = hz / 60.0;
    int k = (int)ratio;
    if (k < ratio) ++k;   // ceil：90Hz最高45帧，不能取floor而升到90。| ceil: at 90 Hz cap at 45 fps, never floor to 90.
    return k > 0 ? k : 1;
}
static inline int nvn_adaptive_max_divisor(double hz) {
    int k = (int)(hz / 20.0 + 0.02), lo = nvn_adaptive_min_divisor(hz);
    return k < lo ? lo : k;
}
static inline int nvn_adaptive_divisor(double hz, int current, double old_hz) {
    int lo = nvn_adaptive_min_divisor(hz), hi = nvn_adaptive_max_divisor(hz);
    int k = current > 0 ? current : lo;
    if (current > 0 && old_hz > 0 && hz != old_hz) k = (int)(current * hz / old_hz + 0.5);
    return k < lo ? lo : k > hi ? hi : k;
}

// 节拍参考刷新率：60 与 120 之间、不是 60 整数倍的刷新率（鸿蒙触控提频的 90 Hz）按 60 算。
// 否则每次触屏都在 60 / 45 两档间来回跳，动态 3D 比例也会按 90 帧周期把每帧都判成掉帧。60 帧靠期望上屏时刻放出，在 90 Hz 上是 2-1 节奏。
// Pacing reference refresh: a rate between 60 and 120 that is not a multiple of 60 (HarmonyOS touch boost to 90 Hz) counts as 60.
// Otherwise every touch flips the tier between 60 and 45, and dynamic 3D scale judges every frame dropped against a 90 fps period. 60 fps rides on desired present times (2-1 cadence at 90 Hz).
static inline double nvn_pace_reference_hz(double hz) {
    if (hz <= 61 || hz >= 119) return hz;
    double r = hz / 60.0;
    return r - (int)(r + 0.02) > 0.04 ? 60 : hz;
}

// 出图档阶梯（鸿蒙 DM_PACE_LADDER）：屏幕刷新率是这一档帧率的整数倍（±3%），每帧才停整数个刷新周期、不抖。45 档：45 / 90 / 135 Hz；40 档：40 / 80 / 120 Hz。
// Present tier ladder (HarmonyOS DM_PACE_LADDER): the screen refresh must be a whole multiple of the tier's frame rate (±3%) for every frame to stay an
// integer number of refresh cycles, without judder. 45 tier: 45 / 90 / 135 Hz; 40 tier: 40 / 80 / 120 Hz.
static inline int nvn_ladder_hz_ok(double fps, double hz) {
    if (fps <= 0 || hz <= 0) return 0;
    double r = hz / fps, d = r - (double)(long)(r + 0.5); if (d < 0) d = -d;
    return r >= 0.97 && d < 0.03;
}
// 从 t 往 dir（-1 更快、+1 更慢）找下一个可用档；bad[i] 非 0 = 这一档本机暂不可用。没有返回 -1
// From t towards dir (-1 faster, +1 slower) the next usable tier; bad[i] != 0 = tier i is unavailable on this device for now. -1 when none
static inline int nvn_ladder_next(int t, int dir, int n, const int *bad) {
    for (int i = t + dir; i >= 0 && i < n; i += dir) if (!bad[i]) return i;
    return -1;
}

// 输入必须是同一个线程的累计CPU纳秒；60帧窗与原图形负载统计一致，等待不会推进此时钟。
// Input is cumulative CPU nanoseconds of the same thread. The 60-frame window matches existing graphics load statistics; waits do not advance this clock.
typedef struct { uint64_t previous, sum; unsigned count; } nvn_frame_cpu_window;
static inline int nvn_frame_cpu_sample(nvn_frame_cpu_window *s, uint64_t cpu_ns, double *ms) {
    if (!s->previous || cpu_ns < s->previous) { s->previous = cpu_ns; s->sum = 0; s->count = 0; return 0; }
    s->sum += cpu_ns - s->previous; s->previous = cpu_ns;
    if (++s->count < 60) return 0;
    *ms = s->sum / 60000000.0; s->sum = 0; s->count = 0; return 1;
}
