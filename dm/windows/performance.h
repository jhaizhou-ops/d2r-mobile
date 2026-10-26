// Windows 保守缺省复用 Vulkan 等价优化；批量提交须显式选择，沿用 Android 的事件 / 回读策略。
// Conservative Windows defaults reuse equivalent Vulkan optimizations; opt-in batching uses Android's event/readback policy.
#pragma once
#include "dm_windows.h"

static inline int dm_windows_performance_defaults(void) {
    const char *option = getenv("DM_WINDOWS_GPU_BATCHING");
    if (option && strcmp(option, "0") && strcmp(option, "1")) {
        fputs("[dm] DM_WINDOWS_GPU_BATCHING must be 0 or 1\n", stderr);
        return -1;
    }
    int batching = option && !strcmp(option, "1");
    static const struct { const char *name, *value; int experimental; } defaults[] = {
        {"DM_GFX_NATIVE_BC", "1", 0},
        {"DM_GFX_LDC_FOLD", "1", 0},
        {"DM_GFX_FORWARD_TEMPS", "1", 0},
        {"DM_GFX_PIPE_MEMO", "1", 0},
        {"DM_GFX_PERF_SAMPLE", "0", 0},
        {"DM_GFX_DEFER_EVENTS", "2", 1},
        {"DM_GFX_READ_PREFLIGHT", "0", 1},
        {"DM_GFX_XVIEW_GUARD", "1", 1},
    };
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
        if (defaults[i].experimental && !batching) continue;
        if (dm_windows_setenv(defaults[i].name, defaults[i].value, 0)) {
            fprintf(stderr, "[dm] Cannot set %s: %s\n", defaults[i].name, strerror(errno));
            return -1;
        }
    }
    if (batching) fputs("[dm] Experimental Windows GPU batching requested; explicit graphics overrides still take priority\n", stderr);
    return 0;
}
