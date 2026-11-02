// 安卓前端（front.h 的安卓实现，对应 Mac 的 front_mac.m、iOS 的 front_ios.m）。
// 两种形态：
//   命令行（adb shell 直接跑 dmhost，开发验证用）：没有窗口，front_run 在当前线程直接跑客体，front_metal_layer 为 NULL（图形层走无窗口路径）。
//   App（Java 外壳经 JNI 交来 ANativeWindow，见 jni_android.c）：客体在独立线程跑，front_metal_layer 返回 ANativeWindow*（图形层出图的目标面）。
// Android front end (the Android implementation of front.h, counterpart of Mac's front_mac.m and iOS's front_ios.m).
// Two forms:
//   Command line (dmhost run directly from adb shell, for development checks): no window, front_run runs the guest directly on the current thread, front_metal_layer is NULL (the graphics layer takes the no-window path).
//   App (the Java shell hands over an ANativeWindow through JNI, see jni_android.c): the guest runs on its own thread, front_metal_layer returns the ANativeWindow* (the surface the graphics layer presents to).
#include "../host/dm.h"
#include "../host/front.h"
#include "../host/dm_input.h"
#include "touch_android.h"
#include "../ios/Sources/dm_touch_game_state.h"
#include <math.h>
#include <stdatomic.h>

static void *_Atomic g_window;      // ANativeWindow*；命令行形态为 NULL | ANativeWindow*; NULL in the command-line form
static _Atomic int g_screen_w, g_screen_h;
void front_android_set_window(void *win, int w, int h) { atomic_store(&g_window, win); atomic_store(&g_screen_w, w); atomic_store(&g_screen_h, h); }

// UI 线程从当前 Display 读实际刷新率；0 表示未知，不冒充实测 60 Hz。
// The UI thread reads the current Display refresh rate; 0 means unknown, not a measured 60 Hz.
static _Atomic int g_refresh_millihz;
void front_android_set_refresh(float hz) {
    atomic_store(&g_refresh_millihz, isfinite(hz) && hz > 0 && hz < 1000 ? (int)lroundf(hz * 1000) : 0);
}
double front_display_refresh(char *name, size_t n) {
    if (name && n) *name = 0;
    return atomic_load(&g_refresh_millihz) / 1000.0;
}

void *front_metal_layer(void) { return atomic_load(&g_window); }

void front_run(void (*guest)(void)) {
    // 观测门在装载后才可订阅；这里沿用 iOS 在 front_run 接入的时机。
    // Observation gates exist only after loading; subscribe at front_run as iOS does.
    if (front_metal_layer()) {
        void uimsg_subscribe(void (*)(const char *, const char *, const char *));
        uimsg_subscribe(dm_android_touch_uimsg);
        dm_pad_hook = dm_android_touch_step;
        dm_touch_game_attach();
        dm_touch_game_live_check(1);
    }
    guest();   // 两种形态都在调用线程上跑：App 形态由 JNI 侧先起好客体线程再进 dm_main | Both forms run on the calling thread: in the App form the JNI side starts the guest thread first and then enters dm_main
}

// 游戏输出分辨率：DM_GAME_RES 钉住；有屏幕尺寸时按同高度放宽到屏幕比例（与 Mac 真全屏、iOS 同一规则，宽 ≤ 4096、8 对齐），否则原版 16:9
// Game output resolution: pinned by DM_GAME_RES; with a known screen size, widen at the same height to the screen aspect (the same rule as Mac true full screen and iOS, width ≤ 4096, aligned to 8), otherwise the original 16:9
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    int dm = atomic_load(&g_screen_w), sh = atomic_load(&g_screen_h);
    unsigned bh = docked ? 900 : 720;
    // 比例与 16:9 不同就按同高取屏幕比例：更宽的屏放宽（手机），更窄的屏收窄（4:3 平板，收窄后 dm_uiscreen 把界面落回 16:9、3D 铺满，与 iPad 同一规则）；
    // 此前窄屏落到 16:9 默认值，整帧被拉伸铺满窄屏，画面瘦长
    // When the aspect differs from 16:9, take the screen aspect at the same height: wider screens widen (phones), narrower screens narrow
    // (a 4:3 tablet; after narrowing, dm_uiscreen lays the UI back into 16:9 and lets the 3D fill, the same rule as iPad).
    // Narrower screens used to fall to the 16:9 default and the whole frame was stretched to fill them, elongating the picture
    if (dm > 0 && sh > 0 && dm * 9 != sh * 16) {
        unsigned bw = (unsigned)(llround((double)bh * dm / sh / 8) * 8); if (bw > 4096) bw = 4096;
        return (uint64_t)bh << 32 | bw;
    }
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}

// 文本输入框：App 用 text_input_jni.c 的强实现弹系统输入框；命令行无窗口，返回 -1。
// Text input: the App overrides this with text_input_jni.c; windowless command-line runs return -1.
__attribute__((weak)) int front_android_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1;
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!front_metal_layer()) return -1;
    return front_android_text_input(title, sub, guide, initial, max_chars, out, out_cap);
}
