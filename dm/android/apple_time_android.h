/* Android 的 Apple 专有写法垫片（用 -include 强制编进宿主 .c）：
 *   · clock_gettime_nsec_np(CLOCK_UPTIME_RAW)：macOS 的单调钟；安卓对应 CLOCK_MONOTONIC（不含睡眠的 CLOCK_BOOTTIME 不需要）。
 *   · CLOCK_UPTIME_RAW / struct stat 的 st_mtimespec：这两个名字在 Linux 上不存在（安卓是 st_mtim）。
 * 只补名字与等价实现，不改任何调用点的语义。
 * Android shim for Apple-only spellings (force-included into the host sources):
 *   · clock_gettime_nsec_np(CLOCK_UPTIME_RAW): the macOS monotonic clock; on Android that is CLOCK_MONOTONIC
 *     (CLOCK_BOOTTIME, which also counts sleep, is not needed).
 *   · CLOCK_UPTIME_RAW and struct stat's st_mtimespec do not exist on Linux (Android has st_mtim).
 * Names and equivalent implementations only; no call-site semantics change. */
#ifndef DM_ANDROID_APPLE_TIME_SHIM_H
#define DM_ANDROID_APPLE_TIME_SHIM_H
#include <time.h>
#include <stdint.h>

#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif

static inline uint64_t clock_gettime_nsec_np(int clk) {
    struct timespec ts;
    if (clock_gettime(clk, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifndef st_mtimespec
#define st_mtimespec st_mtim
#endif

#endif