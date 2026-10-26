// 验证真实 Windows 性能缺省与显式覆盖；不启动游戏或修改调用进程的环境。
// Verify actual Windows performance defaults and explicit overrides without launching the game or changing the caller's environment.
#include "../performance.h"
#include <assert.h>

int main(void) {
    const char *enabled[] = {"DM_GFX_NATIVE_BC", "DM_GFX_LDC_FOLD", "DM_GFX_FORWARD_TEMPS", "DM_GFX_PIPE_MEMO"};
    const char *unchanged[] = {"DM_GFX_DEFER_EVENTS", "DM_GFX_READ_PREFLIGHT", "DM_GFX_XVIEW_GUARD", "DM_3D_SCALE", "DM_GFX_VCACHE", "DM_GFX_TEX_DIRTY",
        "DM_GFX_NOLOAD", "DM_PRESENT_HZ", "DM_PRESENT_INTERVAL"};
    assert(!dm_windows_unsetenv("DM_WINDOWS_GPU_BATCHING"));
    for (size_t i = 0; i < sizeof enabled / sizeof enabled[0]; i++) assert(!dm_windows_unsetenv(enabled[i]));
    for (size_t i = 0; i < sizeof unchanged / sizeof unchanged[0]; i++) assert(!dm_windows_unsetenv(unchanged[i]));
    assert(!dm_windows_unsetenv("DM_GFX_PERF_SAMPLE"));
    assert(!dm_windows_performance_defaults());
    for (size_t i = 0; i < sizeof enabled / sizeof enabled[0]; i++) assert(!strcmp(getenv(enabled[i]), "1"));
    assert(!strcmp(getenv("DM_GFX_PERF_SAMPLE"), "0"));
    for (size_t i = 0; i < sizeof unchanged / sizeof unchanged[0]; i++) assert(!getenv(unchanged[i]));

    for (size_t i = 0; i < sizeof enabled / sizeof enabled[0]; i++) assert(!dm_windows_setenv(enabled[i], "0", 1));
    assert(!dm_windows_setenv("DM_GFX_PERF_SAMPLE", "32", 1));
    assert(!dm_windows_setenv("DM_3D_SCALE", "1.0", 1));
    assert(!dm_windows_setenv("DM_GFX_READ_PREFLIGHT", "1", 1));
    assert(!dm_windows_performance_defaults());
    for (size_t i = 0; i < sizeof enabled / sizeof enabled[0]; i++) assert(!strcmp(getenv(enabled[i]), "0"));
    assert(!strcmp(getenv("DM_GFX_PERF_SAMPLE"), "32"));
    assert(!strcmp(getenv("DM_3D_SCALE"), "1.0"));
    assert(!strcmp(getenv("DM_GFX_READ_PREFLIGHT"), "1"));
    assert(!dm_windows_setenv("DM_WINDOWS_GPU_BATCHING", "1", 1));
    assert(!dm_windows_unsetenv("DM_GFX_READ_PREFLIGHT"));
    assert(!dm_windows_performance_defaults());
    assert(!strcmp(getenv("DM_GFX_DEFER_EVENTS"), "2"));
    assert(!strcmp(getenv("DM_GFX_READ_PREFLIGHT"), "0"));
    assert(!strcmp(getenv("DM_GFX_XVIEW_GUARD"), "1"));
    assert(!strcmp(getenv("DM_3D_SCALE"), "1.0"));
    assert(!getenv("DM_GFX_NOLOAD"));
    assert(!getenv("DM_PRESENT_HZ") && !getenv("DM_PRESENT_INTERVAL"));
    assert(!dm_windows_setenv("DM_GFX_DEFER_EVENTS", "0", 1));
    assert(!dm_windows_setenv("DM_GFX_READ_PREFLIGHT", "1", 1));
    assert(!dm_windows_setenv("DM_GFX_XVIEW_GUARD", "0", 1));
    assert(!dm_windows_performance_defaults());
    assert(!strcmp(getenv("DM_GFX_DEFER_EVENTS"), "0"));
    assert(!strcmp(getenv("DM_GFX_READ_PREFLIGHT"), "1"));
    assert(!strcmp(getenv("DM_GFX_XVIEW_GUARD"), "0"));
    assert(!dm_windows_setenv("DM_WINDOWS_GPU_BATCHING", "0", 1));
    assert(!dm_windows_unsetenv("DM_GFX_DEFER_EVENTS"));
    assert(!dm_windows_performance_defaults());
    assert(!getenv("DM_GFX_DEFER_EVENTS"));
    const char *invalid[] = {"2", "-1", "01", "true"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        assert(!dm_windows_setenv("DM_WINDOWS_GPU_BATCHING", invalid[i], 1));
        assert(dm_windows_performance_defaults() == -1);
    }
    puts("Windows performance defaults, explicit overrides and unchanged synchronization/quality passed");
    return 0;
}
