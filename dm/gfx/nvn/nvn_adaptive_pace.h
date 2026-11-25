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

// 输入必须是同一个线程的累计CPU纳秒；60帧窗与原图形负载统计一致，等待不会推进此时钟。
// Input is cumulative CPU nanoseconds of the same thread. The 60-frame window matches existing graphics load statistics; waits do not advance this clock.
typedef struct { uint64_t previous, sum; unsigned count; } nvn_frame_cpu_window;
static inline int nvn_frame_cpu_sample(nvn_frame_cpu_window *s, uint64_t cpu_ns, double *ms) {
    if (!s->previous || cpu_ns < s->previous) { s->previous = cpu_ns; s->sum = 0; s->count = 0; return 0; }
    s->sum += cpu_ns - s->previous; s->previous = cpu_ns;
    if (++s->count < 60) return 0;
    *ms = s->sum / 60000000.0; s->sum = 0; s->count = 0; return 1;
}
