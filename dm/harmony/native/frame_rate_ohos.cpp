// Native XComponent frame-rate request. This is a system request, not an FPS guarantee.
// Official API 11+: SetExpectedFrameRateRange + Register/UnregisterOnFrameCallback.
#include "frame_rate_ohos.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

extern "C" int dm_fps_setting(void);
#define RATE_LOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0xD2E0, "dm-rate", __VA_ARGS__)
static OH_NativeXComponent *g_component;
static int g_focused = 1, g_running, g_registered, g_attempted, g_target, g_key;
static uint64_t g_next_poll;
// 出图档阶梯（hle_nvn.c ladder_tick，游戏线程）要的档：帧率 / 刷新率；0 = 没开阶梯，照旧按目标请求 | the tier the present ladder (hle_nvn.c ladder_tick, game thread) wants: fps / refresh; 0 = ladder off, request by target as before
static std::atomic<int> g_want_fps{0}, g_want_hz{0};
extern "C" void front_gtx_rate(int fps, int hz);
extern "C" void dm_ohos_rate_want(double fps, int hz) {
    int f = (int)(fps + 0.5); if (f == g_want_fps.load() && hz == g_want_hz.load()) return;
    g_want_fps.store(f); g_want_hz.store(hz); front_gtx_rate(f, hz);
}
static int want_key(int target) { return target * 1000000 + g_want_fps.load() * 1000 + g_want_hz.load(); }

static int request_target(int target) {
    // Match the game's existing normal/eco target. Do not keep 120/144 Hz requested in the background.
    OH_NativeXComponent_ExpectedRateRange range = {target, target, target};
    const char *profile = getenv("DM_RATE_RANGE");
    if (target == 60 && profile && (*profile == '1' || *profile == '2')) {
        range.min = *profile == '1' ? 30 : 0; range.max = 120;
    }
    // DM_REFRESH_120=1：请屏幕跑 120 Hz，出图档就有 60 / 40 / 30（60 Hz 屏只有 60 / 30 / 20）——营地撑不住 60 时停在均匀的 40 而不是 30。
    // DM_REFRESH_120=1: ask for a 120 Hz screen so the present tiers become 60 / 40 / 30 (a 60 Hz screen only has 60 / 30 / 20) -- when camp cannot hold 60 it
    // settles at an even 40 instead of 30.
    const char *r120 = getenv("DM_REFRESH_120");
    if (target == 60 && r120 && *r120 == '1') { range.min = 60; range.max = 120; range.expected = 120; }
    // 出图档阶梯的 45 / 40 档：请屏幕换到这一档能整除的刷新率。DM_LADDER_XC：hz（缺省，{hz,hz,hz}）、fps（{fps,fps,fps}）、span（{fps,hz,hz}）、0（不改请求）
    // Ladder tiers 45 / 40: ask for a refresh the tier divides. DM_LADDER_XC: hz (default, {hz,hz,hz}), fps ({fps,fps,fps}), span ({fps,hz,hz}), 0 (request unchanged)
    int wf = g_want_fps.load(), wh = g_want_hz.load();
    if (target == 60 && wf > 0 && wh > 60) {
        const char *m = getenv("DM_LADDER_XC"); m = m && *m ? m : "hz";
        if (!strcmp(m, "hz")) { range.min = wh; range.max = wh; range.expected = wh; }
        else if (!strcmp(m, "fps")) { range.min = wf; range.max = wf; range.expected = wf; }
        else if (!strcmp(m, "span")) { range.min = wf; range.max = wh; range.expected = wh; }
    }
    g_target = target; g_key = want_key(target); // A failed request is not retried every frame; retry on a new target or foreground session.
    int rc = OH_NativeXComponent_SetExpectedFrameRateRange(g_component, &range);
    RATE_LOG("XComponent rate request min=%{public}d max=%{public}d expected=%{public}d rc=%{public}d (request only, not actual FPS)",
             range.min, range.max, range.expected, rc);
    fprintf(stderr, "[dm] XComponent 帧率请求 min=%d max=%d expected=%d rc=%d（阶梯 %d 档 / %d Hz；只是请求）\n", range.min, range.max, range.expected, rc, wf, wh);
    return rc;
}

static void on_frame(OH_NativeXComponent *component, uint64_t timestamp, uint64_t target_timestamp) {
    (void)target_timestamp;
    if (component != g_component || !g_running || !g_focused || !g_registered) return;
    // This callback runs on ArkUI's thread. No rendering, blocking, or per-frame system calls here.
    // 阶梯换档立即跟（两个原子读）；目标帧率设置每秒查一次 | follow a ladder tier change at once (two atomic loads); the target setting is polled once a second
    if (g_target && want_key(g_target) != g_key) { request_target(g_target); return; }
    if (timestamp < g_next_poll) return;
    g_next_poll = timestamp + UINT64_C(1000000000);
    int target = dm_fps_setting() == 30 ? 30 : 60;
    if (target != g_target) request_target(target);
}

static void stop_request(void) {
    if (g_registered && g_component) {
        int rc = OH_NativeXComponent_UnregisterOnFrameCallback(g_component);
        RATE_LOG("XComponent rate callback unregister rc=%{public}d", rc);
        if (rc == OH_NATIVEXCOMPONENT_RESULT_SUCCESS) g_registered = 0;
    }
    g_attempted = 0; g_target = 0; g_next_poll = 0;
}

static void reconcile(void) {
    if (!g_component || !g_running || !g_focused) { stop_request(); return; }
    if (g_registered || g_attempted) return;
    g_attempted = 1;
    int target = dm_fps_setting() == 30 ? 30 : 60;
    if (request_target(target) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) return;
    int rc = OH_NativeXComponent_RegisterOnFrameCallback(g_component, on_frame);
    RATE_LOG("XComponent rate callback register rc=%{public}d", rc);
    if (rc == OH_NATIVEXCOMPONENT_RESULT_SUCCESS) g_registered = 1;
}

void dm_ohos_frame_rate_surface(OH_NativeXComponent *component, int present) {
    if (!present) {
        if (component != g_component) return;
        stop_request();
        // The platform owns and destroys this XComponent. Never retain its pointer after surface destruction.
        g_component = 0; g_registered = 0;
        return;
    }
    if (component != g_component) {
        stop_request(); g_component = component; g_registered = 0;
    }
    reconcile();
}
void dm_ohos_frame_rate_focus(int foreground) {
    g_focused = foreground != 0; reconcile();
}
void dm_ohos_frame_rate_running(int running) {
    g_running = running != 0; reconcile();
}
