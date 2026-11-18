// tvOS 只读贴图缓存策略：CPU 单测检查回收边界，GPU 生命周期由后端 fence 管理。
// tvOS read-only texture cache policy. CPU tests cover eligibility; the backend manages GPU fences.
#pragma once
#include <stdint.h>

static inline int nvn_pkg_cache_pressure(int was_pressure, uint64_t available) {
    // 160 MiB 进入、224 MiB 退出，避免在同一阈值附近反复开关。
    // Enter at 160 MiB, leave at 224 MiB, avoiding oscillation around a single threshold.
    return available < ((uint64_t)(was_pressure ? 224 : 160) << 20);
}
static inline int nvn_pkg_cache_critical(uint64_t available) { return available < (96ull << 20); }
static inline int nvn_pkg_cache_cold(uint64_t frame, uint64_t last_use, int gpu_written, int critical) {
    return !gpu_written && frame >= last_use && frame - last_use >= (critical ? 120u : 600u);
}
static inline uint64_t nvn_pkg_cache_target(uint64_t total, uint64_t available, uint64_t budget, int pressure) {
    if (!budget) return 0;
    uint64_t target = total > budget ? total - budget : 0;
    int critical = nvn_pkg_cache_critical(available);
    uint64_t floor = (uint64_t)(critical ? 64 : 16) << 20;
    uint64_t cap = (uint64_t)(critical ? 64 : 32) << 20;
    if ((pressure || critical) && target < floor) target = floor;
    return target > cap ? cap : target;
}
