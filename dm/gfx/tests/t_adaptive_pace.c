// 自适应节拍边界测试：期望档位手算；CPU累计时钟在等待期间不动，旧墙钟输入必须被负对照识别。
// Adaptive pacing boundary tests: hand-calculated tiers; CPU clocks stop during waits, and a wall-clock negative control must be detected.
#include "../nvn/nvn_adaptive_pace.h"
#include "../nvn/nvn_pace.h"

static const char *failure;
#define CHECK(c, name) do { if (!(c)) { failure = name; return 1; } } while (0)

int adaptive_pace_test(void) {
    failure = 0;
    struct { double hz; int want; } tiers[] = {{60,1},{90,2},{120,2},{59.94,1},{119.88,2},{50,1},{144,3}};
    for (unsigned i = 0; i < sizeof(tiers)/sizeof(tiers[0]); ++i) {
        int k = nvn_adaptive_divisor(tiers[i].hz, 0, 0);
        CHECK(k == tiers[i].want, "initial tier");
        CHECK(tiers[i].hz / k <= 60, "initial 60 fps ceiling");
    }
    CHECK(nvn_adaptive_divisor(90, 1, 90) == 2, "clamp stale 90 fps tier");
    CHECK(nvn_adaptive_divisor(90, 4, 120) == 3, "120 to 90 retains 30 fps");
    CHECK(nvn_adaptive_divisor(60, 3, 90) == 2, "90 to 60 retains 30 fps");
    CHECK(nvn_adaptive_divisor(120, 2, 60) == 4, "60 to 120 retains 30 fps");
    CHECK(nvn_adaptive_divisor(90, 2, 120) == 2, "120 to 90 caps faster tier");
    CHECK(nvn_adaptive_divisor(60, 6, 120) == 3, "20 fps floor survives refresh change");
    CHECK(nvn_pace_hz(90, 60) == 90, "legacy Apple floor remains unchanged");
    struct { double hz, want; } refs[] = {{60,60},{59.94,59.94},{90,60},{75,60},{100,60},{120,120},{119.88,119.88},{144,144},{50,50},{30,30},{0,0}};
    for (unsigned i = 0; i < sizeof(refs)/sizeof(refs[0]); ++i) CHECK(nvn_pace_reference_hz(refs[i].hz) == refs[i].want, "pacing reference refresh");
    CHECK(nvn_adaptive_divisor(nvn_pace_reference_hz(90), 0, 0) == 1, "touch-boost 90 Hz keeps the 60 fps tier");
    CHECK(nvn_adaptive_divisor(nvn_pace_reference_hz(90), 1, 60) == 1, "60 to boosted 90 Hz does not drop to 45");
    CHECK(nvn_adaptive_divisor(nvn_pace_reference_hz(90), 2, 60) == 2, "60 to boosted 90 Hz keeps the 30 fps tier");
    CHECK(nvn_pace_hz(nvn_pace_reference_hz(90), 60) == 60, "dynamic 3D scale period stays 60 at boosted 90 Hz");
    CHECK(nvn_adaptive_divisor(90, 1, 60) == 2, "negative control: raw 90 Hz drops the 60 tier to 45");
    CHECK(90.0 / (int)(90.0 / 60.0 + 0.02) > 60, "negative control detects old floor bug");

    nvn_frame_cpu_window cpu = {0}, wall = {0}; double cm = -1, wm = -1;
    nvn_frame_cpu_sample(&cpu, 1000000, &cm); nvn_frame_cpu_sample(&wall, 1000000, &wm);
    for (unsigned i = 1; i <= 60; ++i) {
        // 每帧实际工作4ms、等待30ms：CPU平均必须4，墙钟负对照34，不因等待误降档。
        // Each frame does 4 ms CPU work and waits 30 ms: CPU must average 4, wall-clock control 34.
        int ready = nvn_frame_cpu_sample(&cpu, 1000000ull + i * 4000000ull, &cm);
        nvn_frame_cpu_sample(&wall, 1000000ull + i * 34000000ull, &wm);
        CHECK(ready == (i == 60), "publish only after complete 60 frame window");
    }
    CHECK(cm == 4 && wm == 34, "waits excluded from CPU; wall negative control differs");
    CHECK(cm < 1000.0/30 && wm > 1000.0/30, "wall input wrongly exceeds sustainable 30 fps budget");
    for (unsigned i = 1; i <= 60; ++i) nvn_frame_cpu_sample(&cpu, 241000000ull + i * 7000000ull, &cm);
    CHECK(cm == 7, "next window does not retain previous CPU load");
    CHECK(!nvn_frame_cpu_sample(&cpu, 1000, &cm), "counter reset starts a fresh window");
    CHECK(cpu.count == 0 && cpu.sum == 0, "counter reset clears partial load");
    return 0;
}
const char *adaptive_pace_failure(void) { return failure; }

// 桌面DLL驱动可把真实 thread_time_ns 采样送进生产统计函数；不代替鸿蒙设备验证。
// A desktop DLL driver can feed real thread_time_ns samples into the production sampler; it does not replace HarmonyOS device verification.
static nvn_frame_cpu_window observed;
double adaptive_pace_observe(uint64_t cpu_ns, int reset) {
    double ms = -1;
    if (reset) { observed.previous = observed.sum = 0; observed.count = 0; }
    nvn_frame_cpu_sample(&observed, cpu_ns, &ms);
    return ms;
}

#ifndef DM_PACE_TEST_DLL
#include <stdio.h>
#include <time.h>
#include <errno.h>
static uint64_t time_ns(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int real_cpu_clock_test(void) {
    nvn_frame_cpu_window sample = {0}; double ms = -1;
    uint64_t wall0 = time_ns(CLOCK_MONOTONIC), cpu0 = time_ns(CLOCK_THREAD_CPUTIME_ID);
    CHECK(wall0 && cpu0, "real clocks available");
    volatile unsigned work = 1;
    while (time_ns(CLOCK_MONOTONIC) - wall0 < 20000000ull) work = work * 1664525u + 1013904223u;
    (void)work;
    struct timespec delay = {0, 150000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) { }
    uint64_t cpu1 = time_ns(CLOCK_THREAD_CPUTIME_ID), wall1 = time_ns(CLOCK_MONOTONIC);
    CHECK(cpu1 >= cpu0 && wall1 > wall0, "real clocks monotonic");
    nvn_frame_cpu_sample(&sample, cpu0, &ms);
    for (unsigned i = 0; i < 60; ++i) nvn_frame_cpu_sample(&sample, cpu1, &ms);
    double wall_ms = (wall1 - wall0) / 1e6, cpu_ms = ms * 60;
    printf("real clock: wall=%.3f ms, thread CPU=%.3f ms (includes work, excludes 150 ms sleep)\n", wall_ms, cpu_ms);
    CHECK(wall_ms >= 150, "real wait occurred");
    CHECK(cpu_ms < wall_ms * 0.5, "sleep is excluded; wall-clock negative control would fail");
    return 0;
}
int main(void) {
    int r = adaptive_pace_test();
    if (!r) r = real_cpu_clock_test();
    printf("adaptive pace: %s%s\n", r ? "FAIL " : "PASS", r ? failure : "");
    return r;
}
#endif
