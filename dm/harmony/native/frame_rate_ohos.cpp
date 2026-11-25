// Native XComponent frame-rate request. This is a system request, not an FPS guarantee.
// Official API 11+: SetExpectedFrameRateRange + Register/UnregisterOnFrameCallback.
#include "frame_rate_ohos.h"
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" int dm_fps_setting(void);
#define RATE_LOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0xD2E0, "dm-rate", __VA_ARGS__)
static OH_NativeXComponent *g_component;
static int g_focused = 1, g_running, g_registered, g_attempted, g_target;
static uint64_t g_next_poll;

static int request_target(int target) {
    // Match the game's existing normal/eco target. Do not keep 120/144 Hz requested in the background.
    OH_NativeXComponent_ExpectedRateRange range = {target, target, target};
    const char *profile = getenv("DM_RATE_RANGE");
    if (target == 60 && profile && (*profile == '1' || *profile == '2')) {
        range.min = *profile == '1' ? 30 : 0; range.max = 120;
    }
    g_target = target; // A failed request is not retried every frame; retry on a new target or foreground session.
    int rc = OH_NativeXComponent_SetExpectedFrameRateRange(g_component, &range);
    RATE_LOG("XComponent rate request min=%{public}d max=%{public}d expected=%{public}d rc=%{public}d (request only, not actual FPS)",
             range.min, range.max, range.expected, rc);
    return rc;
}

static void on_frame(OH_NativeXComponent *component, uint64_t timestamp, uint64_t target_timestamp) {
    (void)target_timestamp;
    if (component != g_component || !g_running || !g_focused || !g_registered) return;
    // This callback runs on ArkUI's thread. No rendering, blocking, or per-frame system calls here.
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
