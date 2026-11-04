// 安卓前端（front.h 的安卓实现，对应 Mac 的 front_mac.m、iOS 的 front_ios.m）。
// 两种形态：
//   命令行（adb shell 直接跑 dmhost，开发验证用）：没有窗口，front_run 在当前线程直接跑客体，front_metal_layer 为 NULL（图形层走无窗口路径）。
//   App（Java 外壳经 JNI 交来 ANativeWindow，见 jni_android.c）：客体在独立线程跑，front_metal_layer 返回 ANativeWindow*（图形层出图的目标面）。
// 真全屏（同 front_ios.m、front_ohos.c）：屏幕比 16:9 窄（折叠屏内屏、4:3 平板）时局内按屏幕比例出图、3D 铺满整屏，界面由宿主按 16:9
// 摆好并贴底（dm_uiscreen.c），图形层把界面那一趟绘制下移（xbg_set_ui_rect_fn）；前端各页与读取画面仍是 16:9 居中留边。
// 出图尺寸分两段：启动那一次给「窗口纹理上限」（游戏只接受不超过它的新值），之后给当前值；窗口尺寸变了（展开 / 折叠）重算并推 31 让游戏重读。
// Android front end (the Android implementation of front.h, counterpart of Mac's front_mac.m and iOS's front_ios.m).
// Two forms:
//   Command line (dmhost run directly from adb shell, for development checks): no window, front_run runs the guest directly on the current thread, front_metal_layer is NULL (the graphics layer takes the no-window path).
//   App (the Java shell hands over an ANativeWindow through JNI, see jni_android.c): the guest runs on its own thread, front_metal_layer returns the ANativeWindow* (the surface the graphics layer presents to).
// True fullscreen (as front_ios.m and front_ohos.c): on screens narrower than 16:9 (foldable inner panels, 4:3 tablets) the picture in a run is output at
// the screen's aspect with the 3D filling the screen, and the host lays the UI out at 16:9 anchored to the bottom (dm_uiscreen.c), with the graphics layer
// shifting the UI draw pass down (xbg_set_ui_rect_fn); front-end pages and the loading screen stay 16:9 centered with margins.
// The output size comes in two stages: the first read reports the "window texture cap" (the game only accepts new values at or below it), later reads report
// the current value; when the window changes size (unfold / fold) it is recomputed and notification 31 tells the game to reread.
#include "../host/dm.h"
#include "../host/front.h"
#include "../host/dm_input.h"
#include "../host/dm_uiscreen.h"
#include "touch_android.h"
#include "front_android.h"
#include "../ios/Sources/dm_touch_game_state.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>

static void *_Atomic g_window;      // ANativeWindow*；命令行形态为 NULL | ANativeWindow*; NULL in the command-line form
static _Atomic int g_screen_w, g_screen_h;

// ---- 真全屏 ----
// g_full = 1：出图跟屏幕比例、界面按 16:9 贴底；g_startup = 第一次报给游戏的主机档尺寸（= 游戏窗口纹理上限）；
// g_res_now = 当前该给的出图尺寸。三个量都是原子的：游戏线程读分辨率、UI 线程每帧问视图矩形、窗口尺寸变化三者并发。
// g_full = 1: the picture follows the screen aspect and the UI is anchored at 16:9 to the bottom; g_startup = the docked-mode size first reported to the
// game (= the game's window texture cap); g_res_now = the output size to report now. All three are atomic: the game thread reads the resolution, the UI
// thread asks for the view rectangle every frame, and window size changes all race.
static _Atomic int g_full, g_decided;
static _Atomic uint64_t g_startup, g_res_now;   // {u32 宽, u32 高}
static pthread_mutex_t g_cap_mutex = PTHREAD_MUTEX_INITIALIZER;
void dm_oe_res_changed(void);                   // 让游戏重读分辨率参数位（hle_oe.c，性能模式变化通知 31） | make the game reread the resolution slot (hle_oe.c, notification 31)
void xbg_set_ui_rect_fn(void (*fn)(double *y, double *h));   // 图形层：界面那一趟绘制下移（xbg_game.cpp，同 Metal 的 mtl_set_ui_rect_fn）

static double landscape_aspect(void) {
    int w = atomic_load(&g_screen_w), h = atomic_load(&g_screen_h);
    if (w <= 0 || h <= 0) return 0;
    return w > h ? (double)w / h : (double)h / w;   // 横屏比例（窗口首次报尺寸时可能还是竖屏）| landscape aspect (the first reported size may still be portrait)
}
int front_android_full(void) { return atomic_load(&g_full); }
int front_android_window_size(int *w, int *h) {
    if (!w || !h) return 0;
    *w = atomic_load(&g_screen_w); *h = atomic_load(&g_screen_h);
    return *w > 0 && *h > 0;
}
// 界面矩形（占画面高的比例），图形层每帧问一次 | UI rectangle as fractions of the picture height, asked by the graphics layer every frame
static void ui_rect_frac(double *fy, double *fh) {
    int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h);
    if (dm_screen_real(&rw, &rh) && rh > 0) { *fy = (double)y / rh; *fh = (double)h / rh; }
}
// 真全屏开 / 关：开时把界面矩形登记给图形层；关时 dm_uiscreen 把游戏的「屏幕宽高」写回（同 iOS） | Turn true fullscreen on / off: on registers the UI rectangle with the graphics layer; off writes the game's screen width/height back (as iOS)
static void enable_full(int on) {
    if (atomic_exchange(&g_full, on ? 1 : 0) == (on ? 1 : 0)) return;
    if (on) { dm_uiscreen_enable(DM_UISCREEN_BOTTOM); xbg_set_ui_rect_fn(ui_rect_frac); }
    else dm_uiscreen_enable(DM_UISCREEN_OFF);
    LOG("画面：真全屏 %s（屏幕 %dx%d，比例 %.3f）%s", on ? "开" : "关", atomic_load(&g_screen_w), atomic_load(&g_screen_h), landscape_aspect(),
        on ? "：局内按屏幕比例铺满、界面 16:9 贴底，前端页 16:9 居中留边" : "：按画面比例出图（宽屏原规则）");
}
static void update_res(int notify);   // 前向 | forward

// 真全屏的出图短边：屏幕原生像素太高（X Fold3 内屏 2200）——游戏的 3D 比例 = 3D 占出图的比例，下限 0.5（hle_oe.c），
// 原生出图会把 3D 从 540p 抬到 1100p（同一份 GPU 预算的 3 倍）。所以缺省短边取 1080：3D 比例 0.648 -> 3D 约 700p（原 720p×0.75 = 540p），
// 界面比 720p 清晰 2.25 倍。DM_ANDROID_RES=<短边> 覆盖，DM_NATIVE_RES=1 用屏幕原生像素（跑得动再开，见 README.md）。
// Output short side in true fullscreen: the native pixel count is too high (the X Fold3 inner panel is 2200) -- the game's 3D scale is a fraction of the
// output and floors at 0.5 (hle_oe.c), so native output would lift the 3D from 540p to 1100p (3x the same GPU budget). The default short side is therefore
// 1080: 3D scale 0.648 -> about 700p of 3D (720p x 0.75 = 540p before) while the UI is 2.25x sharper than 720p. DM_ANDROID_RES=<short side> overrides it,
// DM_NATIVE_RES=1 uses the screen's native pixels (turn it on only if the device keeps up, see docs/ANDROID.md).
static unsigned full_short_side(int screen_short) {
    unsigned sh = (unsigned)(screen_short > 0 ? screen_short : 720), want = 1080;
    const char *e = getenv("DM_ANDROID_RES");
    if (e && *e) { int v = atoi(e); if (v >= 480 && v <= 4320) want = (unsigned)v; }
    else { const char *n = getenv("DM_NATIVE_RES"); if (n && *n == '1') want = sh; }
    return sh < want ? sh : want;
}
// 3D 比例缺省（只补缺省，玩家 / 测试自己设过就不动）：3D 约 700 高，且不低于 0.5、不高于 0.75（同鸿蒙 front_ohos.c）
// Default 3D scale (fills in a default only; a value the player or a test already set is left alone): about 700 lines of 3D, not below 0.5 and not above 0.75 (as front_ohos.c)
static void default_3d_scale(unsigned short_side) {
    if (getenv("DM_3D_SCALE")) return;
    double sc = 700.0 / short_side; char v[16];
    snprintf(v, sizeof v, "%.2f", sc < 0.5 ? 0.5 : sc > 0.75 ? 0.75 : sc);
    setenv("DM_3D_SCALE_DEFAULT", v, 1);   // 写「缺省档」不写 DM_3D_SCALE：后者压过自定义页的 3D 清晰度三档（hle_oe.c）| set the default level, not DM_3D_SCALE: that one overrides the Custom page's three 3D resolution levels (hle_oe.c)
    LOG("画面：出图短边 %u，3D 比例缺省 %s（自定义页「均衡」档；DM_3D_SCALE 覆盖）", short_side, v);
}
// 真全屏的启动值（也是游戏窗口纹理上限）：屏幕比例、短边见 full_short_side。
// The startup value under true fullscreen (also the game's window texture cap): the screen aspect with a full_short_side short side.
static uint64_t full_cap_value(void) {
    int w = atomic_load(&g_screen_w), h = atomic_load(&g_screen_h);
    double a = landscape_aspect();
    if (w <= 0 || h <= 0 || a <= 0) return 0;
    unsigned short_side = full_short_side(w > h ? h : w);
    unsigned bw = (unsigned)(llround((double)short_side * a / 8) * 8); if (bw > 4096) bw = 4096;
    return (uint64_t)short_side << 32 | bw;
}
// 启动上限：真全屏时取屏幕比例下的出图尺寸，同时把它记成游戏窗口纹理的上限（游戏只接受不超过它的新值，见 dm_uiscreen.c）。
// Startup cap: with true fullscreen the output size at the screen's aspect; it is also recorded as the game's window texture cap (the game only
// accepts new values at or below it, see dm_uiscreen.c).
static void cap_init(void) {
    pthread_mutex_lock(&g_cap_mutex);
    if (!atomic_load(&g_startup)) {
        uint64_t v = full_cap_value();
        if (v) {
            uint64_t exp = 0;
            atomic_compare_exchange_strong(&g_startup, &exp, v);
            default_3d_scale((unsigned)(v >> 32));
            LOG("画面：窗口纹理上限 %ux%u（屏幕 %dx%d，比例 %.3f）", (unsigned)v, (unsigned)(v >> 32), atomic_load(&g_screen_w), atomic_load(&g_screen_h), landscape_aspect());
        }
    }
    pthread_mutex_unlock(&g_cap_mutex);
    update_res(0);
}
// 当前出图尺寸：真全屏 = 屏幕比例（短边 full_short_side）；不开真全屏 = 原规则（主机档 900 高，宽屏按屏幕比例放宽）。
// 不超过启动上限（游戏只接受不超过它的新值）：展开 / 折叠换了更大的屏幕时等比缩进上限里，仍然铺满。
// Current output size: with true fullscreen the screen aspect (short side from full_short_side); without it the old rule (900 tall in docked mode, widened
// to the screen aspect on wide screens). Never above the startup cap (the game only accepts values at or below it): when a larger screen appears after
// unfolding, scale down into the cap -- the picture still fills.
static void update_res(int notify) {
    int w = atomic_load(&g_screen_w), h = atomic_load(&g_screen_h);
    double a = landscape_aspect();
    if (w <= 0 || h <= 0 || a <= 0) return;
    unsigned rw, rh;
    if (atomic_load(&g_full)) { rh = full_short_side(w > h ? h : w); rw = (unsigned)(llround((double)rh * a / 8) * 8); }
    else if (a > 16.0 / 9 + 0.01) { rh = 900; rw = (unsigned)(llround((double)rh * a / 8) * 8); }
    else { rw = 1600; rh = 900; }   // 窄屏且真全屏关着：原版 16:9 留边 | narrow screen with true fullscreen off: the original 16:9 with margins
    if (rw > 4096) { rw = 4096; rh = (unsigned)(llround(4096 / a / 2) * 2); }
    uint64_t cap = atomic_load(&g_startup);
    if (cap) {
        unsigned cw = (unsigned)cap, ch = (unsigned)(cap >> 32);
        if (rw > cw || rh > ch) { double f = fmin((double)cw / rw, (double)ch / rh); rw = (unsigned)(rw * f) & ~1u; rh = (unsigned)(rh * f) & ~1u; }
    }
    if (rw < 2 || rh < 2) return;
    uint64_t v = (uint64_t)rh << 32 | rw, old = atomic_exchange(&g_res_now, v);
    if (old == v) return;
    if (notify && dm_uiscreen_res_live()) { LOG("画面：出图尺寸 %ux%u → %ux%u（窗口 %dx%d），通知游戏重读", (unsigned)old, (unsigned)(old >> 32), rw, rh, w, h); dm_oe_res_changed(); }
    else LOG("画面：出图尺寸 %ux%u（窗口 %dx%d，真全屏 %s）", rw, rh, w, h, atomic_load(&g_full) ? "开" : "关");
}
// 真全屏开不开：按屏幕比例定（同 iOS / 鸿蒙），第一次拿到窗口尺寸时就定，之后展开 / 折叠换了屏幕再重判（见 front_android_set_window）
// Whether true fullscreen is on: decided from the screen aspect (as iOS / HarmonyOS), as soon as the window size is first known, and re-decided when
// unfolding / folding changes the screen (see front_android_set_window)
static void decide_full(void) {
    int exp = 0;
    double a = landscape_aspect();
    if (a <= 0 || !atomic_compare_exchange_strong(&g_decided, &exp, 1)) return;
    const char *e = getenv("DM_ANDROID_FULL");   // DM_ANDROID_FULL=0 关掉（退回全程 16:9 留边，同 iOS 的 DM_IPAD_FULL=0）
    enable_full((!e || *e != '0') && a < 16.0 / 9 - 0.01);
    if (atomic_load(&g_full)) cap_init();
}
void front_android_set_window(void *win, int w, int h) {
    int ow = atomic_load(&g_screen_w), oh = atomic_load(&g_screen_h);
    atomic_store(&g_window, win); atomic_store(&g_screen_w, w); atomic_store(&g_screen_h, h);
    if (w <= 0 || h <= 0) return;                       // 收回画面：保留上次尺寸，等新面 | surface taken back: keep the last size, wait for the new surface
    if (!atomic_load(&g_decided)) { decide_full(); if (!atomic_load(&g_decided)) return; }
    if (w == ow && h == oh) return;                     // 尺寸没变：普通启动 / 同一块屏 | same size: ordinary start / same panel
    // 展开 / 折叠换了屏幕：真全屏开关跟着比例翻转，出图尺寸重算并推 31 让游戏重读
    // Unfolding / folding changed the screen: the fullscreen switch follows the aspect, and the output size is recomputed and pushed to the game with 31
    double a = landscape_aspect();
    const char *e = getenv("DM_ANDROID_FULL");
    enable_full((!e || *e != '0') && a < 16.0 / 9 - 0.01);
    if (atomic_load(&g_full)) cap_init();
    update_res(1);
}

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

// 游戏视图矩形（px，窗口坐标系）：真全屏局内 = 贴底的界面矩形（dm_ui_rect，界面等比 16:9），局外（前端页、读取画面）= 窗口里居中的 16:9；
// 没开真全屏 = 与画面同比例的那块（比例差 < 1% 就是整个窗口）。与 front_ios.m 的 letterbox_rect、front_ohos_view_rect 同一条规则。
// Game view rectangle (px, window coordinates): true fullscreen in a run = the bottom-anchored UI rectangle (dm_ui_rect, the UI is 16:9 there), out of a
// run (front-end pages, loading screen) = the centered 16:9 block; without true fullscreen = the block matching the picture aspect (a difference below 1%
// is the whole window). Same rule as front_ios.m's letterbox_rect and front_ohos_view_rect.
void front_android_view_rect(int win_w, int win_h, int out[4]) {
    out[0] = out[1] = 0; out[2] = win_w; out[3] = win_h;
    if (win_w <= 0 || win_h <= 0) return;
    if (front_android_full()) {
        int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h);
        int vh = (int)floor(win_w * 9.0 / 16), vy = (win_h - vh) / 2;
        if (dm_screen_real(&rw, &rh) && h > 0 && h < rh) { vy = (int)lround((double)win_h * y / rh); vh = (int)lround((double)win_h * h / rh);
            /* 真全屏：游戏按 16:9 设计排版，HUD 中线固定在「界面矩形左起 1920 布局单位」= 1173.6 效果图像素处。界面矩形比 16:9 窄
               （界面高被游戏的 720 下限抬过就是这种）时这条中线比矩形中线偏右，覆盖层（自绘 HUD 与按钮）整体右移同一差值——
               点按跟着视图原点一起平移，世界换算（touch_android.c world_rect）用 −vr_x 又加回来，所以点世界不受影响。
               16:9 及以上算出来是 0，行为不变。 | True fullscreen: the game lays its HUD out for 16:9, so the HUD centreline always
               sits "1920 layout units from the UI rectangle's left edge" = 1173.6 mockup pixels. On a rectangle narrower than 16:9
               (what the game's 720-pixel UI-height floor produces) that centreline sits right of the rectangle's centreline, so the
               whole overlay (self-drawn HUD and buttons) moves right by the same difference: touches follow the view origin, and
               world_rect adds -vr_x back, leaving world taps untouched. On 16:9 and wider it works out to 0 and nothing changes. */
            float hu = (float)vh / 1320.0f; int shift = (int)lroundf(1173.6f * hu - (float)win_w * 0.5f);
            if (shift > 0) out[0] = shift; }
        else if (rw > 0 && rh > 0 && (double)rw * 9 > (double)rh * 16) { vh = (int)lround((double)win_w * rh / rw); vy = (win_h - vh) / 2; }
        out[1] = vy; out[3] = vh; return;
    }
    uint64_t r = front_game_res(1); double gw = (double)(uint32_t)r, gh = (double)(r >> 32);
    if (gw <= 0 || gh <= 0) return;
    double ga = gw / gh, wa = (double)win_w / win_h; if (fabs(wa / ga - 1) < 0.01) return;
    int w = win_w, h = win_h;
    if (wa > ga) w = (int)floor(win_h * ga); else h = (int)floor(win_w / ga);
    out[0] = (win_w - w) / 2; out[1] = (win_h - h) / 2; out[2] = w; out[3] = h;
}

// 游戏输出分辨率：DM_GAME_RES 钉住；真全屏时 = 屏幕比例（启动那一次给上限，之后给当前值，见文件头）；否则原规则：有屏幕尺寸时按同高度
// 放宽到屏幕比例（与 Mac 真全屏、iOS 同一规则，宽 ≤ 4096、8 对齐），否则原版 16:9。
// Game output resolution: pinned by DM_GAME_RES; with true fullscreen the screen aspect (the cap at startup, the current value afterwards, see the file
// header); otherwise the old rule: with a known screen size, widen at the same height to the screen aspect (the same rule as Mac true full screen and iOS,
// width <= 4096, aligned to 8), otherwise the original 16:9.
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    decide_full();
    double a = landscape_aspect();
    if (a <= 0) return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;   // 还没有窗口尺寸：原版 16:9 | no window size yet: the original 16:9
    if (docked) {
        // 第一次报出去的值就是游戏窗口纹理上限（它只接受不超过启动尺寸的新值）：所以启动时哪怕没开真全屏也要记下来，
        // 之后展开 / 折叠换了屏幕（比例翻转、屏幕变大）才有一套上限可以按比例缩进去，画面仍然铺满。
        // The first value reported is the game's window texture cap (it only accepts new values at or below the startup size): record it even
        // when true fullscreen is off at startup, so that unfolding / folding later (aspect flips, screen grows) has a cap to scale into and
        // the picture still fills.
        uint64_t v;
        if (atomic_load(&g_full)) {
            v = dm_uiscreen_res_live() ? atomic_load(&g_res_now) : 0;
            if (!v) v = full_cap_value();
        } else if (a <= 16.0 / 9 + 0.01) v = 900ull << 32 | 1600;
        else { unsigned bw = (unsigned)(llround(900.0 * a / 8) * 8); if (bw > 4096) bw = 4096; v = 900ull << 32 | bw; }
        if (v) { uint64_t exp = 0; atomic_compare_exchange_strong(&g_startup, &exp, v); }
        // 一次总结日志：判定发生在画面就绪时（日志通路还没起来，那几行丢了），这里补一行，玩家 logcat 里能核「真全屏开没开、出图多少」。
        // One summary line: the decision happens when the surface is ready (before the log path exists, so those lines are lost); this line lets a
        // player's logcat show whether true fullscreen is on and what the output size is.
        static int seen;
        if (!seen) {
            seen = 1;
            uint64_t cap = atomic_load(&g_startup);
            LOG("画面：真全屏 %s（屏幕 %dx%d，比例 %.3f）；出图 %ux%u；窗口纹理上限 %ux%u；3D 比例 %s",
                atomic_load(&g_full) ? "开" : "关", atomic_load(&g_screen_w), atomic_load(&g_screen_h), landscape_aspect(),
                (unsigned)(v & 0xffffffffu), (unsigned)(v >> 32), (unsigned)(cap & 0xffffffffu), (unsigned)(cap >> 32),
                getenv("DM_3D_SCALE") ? getenv("DM_3D_SCALE") : "默认");
        }
        return v;
    }
    // 掌机档 720：真全屏按屏幕比例，宽屏同高度放宽，其余原版 16:9
    // Handheld profile 720: the screen aspect under true fullscreen, widened at the same height on wide screens, otherwise the original 16:9
    if (a <= 16.0 / 9 + 0.01 && !atomic_load(&g_full)) return 720ull << 32 | 1280;
    unsigned bw = (unsigned)(llround(720.0 * a / 8) * 8); if (bw > 4096) bw = 4096;
    return 720ull << 32 | bw;
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
