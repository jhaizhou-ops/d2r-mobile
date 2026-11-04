// 鸿蒙前端（front.h 的鸿蒙实现）：由安卓 front_android.c 拷来，按 iOS front_ios.m 改了三处——
//   ① 真全屏：屏幕比 16:9 窄（Pura X 等折叠屏）时同 iOS（dm_uiscreen BOTTOM）：局内画面按屏幕原生比例出、3D 铺满，界面按 16:9 贴底；前端各页 16:9 居中留边；
//      界面矩形的纵向位置交给 Vulkan 图形层（xbg_set_ui_rect_fn，对应 Metal 的 mtl_set_ui_rect_fn）。DM_IPAD_FULL=0 退回全程 16:9 留黑边。
//   ② 触控同 iOS：界面消息进 touch_ohos.c（front_ios.m touch_ui_message 的搬运），按键由 UI 线程直接 dm_input_set，不挂 dm_pad_hook。
//   ③ 游戏视图矩形（letterbox_rect 的搬运）给 ArkTS 摆触控层用。
// HarmonyOS front end (the HarmonyOS implementation of front.h): copied from Android's front_android.c and changed in three places after iOS front_ios.m --
//   (1) True fullscreen: on screens narrower than 16:9 (foldables such as the Pura X), as iOS (dm_uiscreen BOTTOM): the in-run picture is output at the screen's
//       native aspect ratio with 3D filling it and the UI laid out at 16:9 anchored to the bottom; front-end pages are 16:9 centered with margins; the vertical
//       position of the UI rectangle goes to the Vulkan graphics layer (xbg_set_ui_rect_fn, counterpart of Metal's mtl_set_ui_rect_fn). DM_IPAD_FULL=0 falls back to 16:9 with black bars throughout.
//   (2) Touch as iOS: UI messages go to touch_ohos.c (a port of front_ios.m touch_ui_message); keys are published by the UI thread with dm_input_set, no dm_pad_hook.
//   (3) The game view rectangle (a port of letterbox_rect) is given to ArkTS to place the touch layer.
#include "../../host/dm.h"
#include "../../host/front.h"
#include "../../host/dm_input.h"
#include "../../host/dm_uiscreen.h"
#include "../../ios/Sources/dm_touch_game_state.h"
#include "touch_ohos.h"
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <sched.h>
#include <qos/qos.h>
#include <window_manager/oh_display_manager.h>
#include <dlfcn.h>
#include <graphics_game_sdk/opengtx_base.h>
#include <unistd.h>

void front_pin_fast(const char *who);
void front_gtx_register(int who);
static void *_Atomic g_window;
static _Atomic int g_screen_w, g_screen_h;
void front_android_set_window(void *win, int w, int h) { atomic_store(&g_window, win); if (w > 0 && h > 0) { atomic_store(&g_screen_w, w); atomic_store(&g_screen_h, h); } }

static _Atomic int g_refresh_millihz;
void front_android_set_refresh(float hz) {
    atomic_store(&g_refresh_millihz, isfinite(hz) && hz > 0 && hz < 1000 ? (int)lroundf(hz * 1000) : 0);
}
// 显示服务报告的刷新率会变化；查询失败时保留 ArkTS 启动快照或上次有效值。
// The display-service refresh rate can change; on query failure retain the ArkTS startup snapshot or last valid value.
static uint32_t g_refresh_listener_index;
static bool g_refresh_listener_registered;
static bool g_refresh_register_warned, g_refresh_unregister_warned;
static _Atomic bool g_refresh_read_warned;
static _Atomic unsigned g_refresh_change_logs;

static void front_refresh_update(void) {
    uint32_t hz = 0;
    int r = OH_NativeDisplayManager_GetDefaultDisplayRefreshRate(&hz);
    if (r != DISPLAY_MANAGER_OK || hz == 0 || hz >= 1000) {
        if (!atomic_exchange(&g_refresh_read_warned, true))
            LOG("Display refresh query failed: rc=%d hz=%u; retaining cached refresh", r, hz);
        return;
    }
    int previous = atomic_load(&g_refresh_millihz);
    front_android_set_refresh((float)hz);
    if (previous != (int)hz * 1000 && atomic_fetch_add(&g_refresh_change_logs, 1) < 8)
        LOG("Display refresh updated: %.3f -> %u Hz", previous / 1000.0, hz);
}

static void front_refresh_changed(uint64_t display_id) {
    (void)display_id;
    front_refresh_update();
}

static void front_refresh_start(void) {
    if (!g_refresh_listener_registered) {
        int r = OH_NativeDisplayManager_RegisterDisplayChangeListener(
            front_refresh_changed, &g_refresh_listener_index);
        if (r == DISPLAY_MANAGER_OK) g_refresh_listener_registered = true;
        else if (!g_refresh_register_warned) {
            g_refresh_register_warned = true;
            LOG("Display refresh listener registration failed: rc=%d; using snapshot", r);
        }
    }
    front_refresh_update();
}

static void front_refresh_stop(void) {
    if (!g_refresh_listener_registered) return;
    int r = OH_NativeDisplayManager_UnregisterDisplayChangeListener(g_refresh_listener_index);
    if (r == DISPLAY_MANAGER_OK) g_refresh_listener_registered = false;
    else if (!g_refresh_unregister_warned) {
        g_refresh_unregister_warned = true;
        LOG("Display refresh listener removal failed: rc=%d", r);
    }
}

double front_display_refresh(char *name, size_t n) {
    if (name && n) *name = 0;
    return atomic_load(&g_refresh_millihz) / 1000.0;
}

void *front_metal_layer(void) { return atomic_load(&g_window); }

// 界面矩形在真实画面里的纵向位置与高（画面高的比例），同 front_ios.m ui_rect_frac | Vertical position and height of the UI rectangle as fractions of the picture height, as front_ios.m ui_rect_frac
static void ui_rect_frac(double *fy, double *fh) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); if (dm_screen_real(&rw, &rh) && rh > 0) { *fy = (double)y / rh; *fh = (double)h / rh; } }
void xbg_set_ui_rect_fn(void (*fn)(double *y, double *h));

void front_run(void (*guest)(void)) {
    if (front_metal_layer()) {
        void uimsg_subscribe(void (*)(const char *, const char *, const char *));
        front_refresh_start();
        uimsg_subscribe(dm_ohos_touch_uimsg);
        dm_touch_game_live_check(1);
        front_pin_fast("游戏主线程");
        { void dm_gameperf_start(void); dm_gameperf_start(); }   // Game Service 温控 / GPU 负载监测 | Game Service thermal / GPU load monitoring
        front_gtx_register(0);
        if (!dm_touch_game_attach()) LOG("【红】技能绑定读取没挂上（产物没有观测门 OBS_BINDING_GETTER，或 main 模块不符）：技能键图标 / 回城键绑定不工作");
    }
    guest();
    front_refresh_stop();
}

// 把调用线程绑到最高主频的那组核（Pura X：cpu10–11 2.5 GHz；调度器把翻译线程放在 2.15 GHz 的中核、游戏线程常在 1.6 GHz 的小核）。
// DM_PIN_FAST=0 关；=十六进制掩码 指定核
// Pin the calling thread to the cores with the highest max frequency (Pura X: cpu at 2.5 GHz; the scheduler kept the translation thread on 2.15 GHz mid cores
// and the game thread often on 1.6 GHz little cores). DM_PIN_FAST=0 disables; =hex mask picks the cores
void front_pin_keep(unsigned mask);
void front_pin_fast(const char *who) {
    // 先走系统 QoS（鸿蒙的正路：系统按 QoS 把线程排到快核；直接改可用核会被系统每帧改回中核，实测上千次）
    // System QoS first (the HarmonyOS way: the system places threads on fast cores by QoS; setting affinity directly is reverted to mid cores by the system every frame, measured thousands of times)
    { const char *q = getenv("DM_QOS"); if (!(q && *q == '0')) { int r = OH_QoS_SetThreadQoS(QOS_USER_INTERACTIVE); LOG("QoS：%s（线程 %d）→ USER_INTERACTIVE，结果 %d", who, (int)gettid(), r); } }
    const char *e = getenv("DM_PIN_FAST"); unsigned mask = 0;
    if (!e || !*e || (*e == '0' && !e[1])) return;   // 硬绑核只在显式给掩码时做 | hard affinity only with an explicit mask
    mask = (unsigned)strtoul(e, NULL, 16);
    if (!mask) { long best = 0, all = 0;
        for (int c = 0; c < 32; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c); FILE *f = fopen(p, "r"); if (!f) continue;
            long v = 0; if (fscanf(f, "%ld", &v) == 1 && v > 0) { all |= 1L << c; if (v > best) { best = v; mask = 0; } if (v == best) mask |= 1u << c; } fclose(f); }
        if ((long)mask == all) mask = 0; }
    if (!mask) return;
    cpu_set_t set; CPU_ZERO(&set); for (int c = 0; c < 32; c++) if (mask >> c & 1) CPU_SET(c, &set);
    int r = sched_setaffinity(0, sizeof set, &set);
    LOG("绑核：%s（线程 %d）→ 掩码 %#x，结果 %d", who, (int)gettid(), mask, r);
    static __thread unsigned kept; kept = r ? 0 : mask;
    front_pin_keep(kept);
}
// 系统（QoS / 资源调度）会改回线程的可用核：调用线程每帧核对一次，被改了就再绑（同 K50 上 MIUI 的情况）
// The system (QoS / resource scheduling) rewrites the thread's allowed cores: the calling thread checks once per frame and re-pins if changed (as MIUI did on the K50)
static __thread unsigned g_pin_mask; static __thread unsigned g_pin_drifts;
void front_pin_keep(unsigned mask) { g_pin_mask = mask; }
void front_pin_check(void) {
    if (!g_pin_mask) return;
    cpu_set_t cur; CPU_ZERO(&cur); if (sched_getaffinity(0, sizeof cur, &cur)) return;
    unsigned have = 0; for (int c = 0; c < 32; c++) if (CPU_ISSET(c, &cur)) have |= 1u << c;
    if (have == g_pin_mask) return;
    cpu_set_t set; CPU_ZERO(&set); for (int c = 0; c < 32; c++) if (g_pin_mask >> c & 1) CPU_SET(c, &set);
    int r = sched_setaffinity(0, sizeof set, &set);
    if (++g_pin_drifts <= 5 || !(g_pin_drifts & 1023)) LOG("绑核：线程 %d 的可用核被改成 %#x（第 %u 次），重绑 %#x 结果 %d", (int)gettid(), have, g_pin_drifts, g_pin_mask, r);
}

// ---- OpenGTX（Graphics Accelerate Kit，游戏渲染加速服务）：把游戏主线程、渲染线程（= 翻译线程）、关键线程登记给系统，
// 声明目标帧率与游戏类型，场景切换时下发场景信息；系统据此调整 SoC / DDR 频率与调度（GPU Turbo X 的开放入口）。
// 运行时 dlopen：设备不支持时整段跳过，不影响启动。DM_OPENGTX=0 关。
// ---- OpenGTX (Graphics Accelerate Kit, game rendering acceleration): register the game main thread, render thread (= the translation thread) and key threads
// with the system, declare the target frame rate and game type, and send scene info on scene changes; the system adjusts SoC / DDR frequencies and scheduling
// (the open entry of GPU Turbo X). Loaded at run time with dlopen: skipped entirely on devices without it, never blocking startup. DM_OPENGTX=0 disables.
static struct {
    void *lib; OpenGTX_Context *ctx; int main_tid, render_tid, key_tids[5], nkeys, active, scene;
    OpenGTX_Context *(*create)(OpenGTX_DeviceInfoCallback);
    OpenGTX_ErrorCode (*config)(OpenGTX_Context *, const OpenGTX_ConfigDescription *);
    OpenGTX_ErrorCode (*activate)(OpenGTX_Context *);
    OpenGTX_ErrorCode (*scene_info)(OpenGTX_Context *, const OpenGTX_GameSceneInfo *);
    OpenGTX_ErrorCode (*frame_info)(OpenGTX_Context *, const OpenGTX_FrameRenderInfo *);
} g_gtx;
static pthread_mutex_t g_gtx_mu = PTHREAD_MUTEX_INITIALIZER;
static void gtx_temp(OpenGTX_TempLevel level) { LOG("OpenGTX：设备温度档位 %d", (int)level); }
static void gtx_try_activate(void) {   // 主线程和渲染线程都登记了才建上下文 | create the context only once both the main and render threads are registered
    if (g_gtx.active || !g_gtx.main_tid || !g_gtx.render_tid) return;
    const char *e = getenv("DM_OPENGTX"); if (e && *e == '0') return;
    g_gtx.active = -1;
    g_gtx.lib = dlopen("libopengtx.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_gtx.lib) { LOG("OpenGTX：库加载不到（%s），跳过", dlerror()); return; }
    g_gtx.create = (OpenGTX_Context *(*)(OpenGTX_DeviceInfoCallback))dlsym(g_gtx.lib, "HMS_OpenGTX_CreateContext");
    g_gtx.config = (OpenGTX_ErrorCode (*)(OpenGTX_Context *, const OpenGTX_ConfigDescription *))dlsym(g_gtx.lib, "HMS_OpenGTX_SetConfiguration");
    g_gtx.activate = (OpenGTX_ErrorCode (*)(OpenGTX_Context *))dlsym(g_gtx.lib, "HMS_OpenGTX_Activate");
    g_gtx.scene_info = (OpenGTX_ErrorCode (*)(OpenGTX_Context *, const OpenGTX_GameSceneInfo *))dlsym(g_gtx.lib, "HMS_OpenGTX_DispatchGameSceneInfo");
    g_gtx.frame_info = (OpenGTX_ErrorCode (*)(OpenGTX_Context *, const OpenGTX_FrameRenderInfo *))dlsym(g_gtx.lib, "HMS_OpenGTX_DispatchFrameRenderInfo");
    if (!g_gtx.create || !g_gtx.config || !g_gtx.activate) { LOG("OpenGTX：接口不全，跳过"); return; }
    g_gtx.ctx = g_gtx.create(gtx_temp);
    if (!g_gtx.ctx) { LOG("OpenGTX：设备不支持（CreateContext 返回空），跳过"); return; }
    OpenGTX_ConfigDescription c; memset(&c, 0, sizeof c);
    c.mode = SCENE_MODE; c.targetFPS = 60;
    c.packageName = (char *)"com.xiao.boluo"; c.appVersion = (char *)"0.14.0";
    c.engineType = OTHERS_ENGINE; c.engineVersion = (char *)"dm"; c.gameType = RPG; c.pictureQualityMaxLevel = FHD;
    int dm = atomic_load(&g_screen_w), sh = atomic_load(&g_screen_h);
    c.resolutionMaxValue.height = sh > dm ? dm : sh; c.resolutionMaxValue.width = sh > dm ? sh : dm;
    if (c.resolutionMaxValue.height < 360) { c.resolutionMaxValue.height = 1320; c.resolutionMaxValue.width = 2120; }
    c.gameMainThreadId = g_gtx.main_tid; c.gameRenderThreadId = g_gtx.render_tid;
    for (int i = 0; i < 5; i++) c.gameKeyThreadIds[i] = i < g_gtx.nkeys ? g_gtx.key_tids[i] : 0;
    c.vulkanSupport = true;
    OpenGTX_ErrorCode r1 = g_gtx.config(g_gtx.ctx, &c), r2 = r1 == OPENGTX_SUCCESS ? g_gtx.activate(g_gtx.ctx) : r1;
    LOG("OpenGTX：主线程 %d 渲染线程 %d 关键线程 %d 个，目标 60 FPS，分辨率 %dx%d → 配置 %d 激活 %d", g_gtx.main_tid, g_gtx.render_tid, g_gtx.nkeys,
        c.resolutionMaxValue.width, c.resolutionMaxValue.height, (int)r1, (int)r2);
    if (r2 == OPENGTX_SUCCESS) g_gtx.active = 1;
}
// ---- FAST Kit 调度优化（PerfHint，API 26）：装载期向系统声明场景并点名线程（游戏主线程、翻译线程、关键线程），由系统给这些线程加调度 / 频率。
// 启动：渲染线程登记后起 APP_LAUNCH（LONG，最长 60 s），主菜单出来（MainMenuButtonRibbon）结束；每次进局 / 切图的读取画面：PAGE_LOAD（MEDIUM，最长 10 s），撤下结束。
// 运行时 dlopen：设备 / 系统不支持时跳过；频率限制（间隔不足等）只记日志。DM_PERFHINT=0 关。
// ---- FAST Kit scheduling optimization (PerfHint, API 26): during loads, declare the scene to the system and name the threads (game main, translation, key threads) so it
// boosts their scheduling / clocks. Start-up: APP_LAUNCH (LONG, at most 60 s) once the render thread registers, ended when the main menu appears (MainMenuButtonRibbon); each
// run / area loading screen: PAGE_LOAD (MEDIUM, at most 10 s), ended when it is taken down. Loaded at run time with dlopen: skipped when unsupported; rate limits (interval too
// short etc.) are only logged. DM_PERFHINT=0 disables.
#include <FASTKit/scheduling_optimization.h>
static struct {
    int tried, launch_on, load_on;
    HMS_FAST_SchedulingOptimization_ErrorCode (*create)(HMS_FAST_PerfHintConfigBuilder **);
    void (*destroy)(HMS_FAST_PerfHintConfigBuilder *);
    HMS_FAST_SchedulingOptimization_ErrorCode (*type)(HMS_FAST_PerfHintConfigBuilder *, HMS_FAST_SchedulingOptimization_SceneType);
    HMS_FAST_SchedulingOptimization_ErrorCode (*state)(HMS_FAST_PerfHintConfigBuilder *, HMS_FAST_SchedulingOptimization_SceneState);
    HMS_FAST_SchedulingOptimization_ErrorCode (*dur)(HMS_FAST_PerfHintConfigBuilder *, HMS_FAST_SchedulingOptimization_DurationType);
    HMS_FAST_SchedulingOptimization_ErrorCode (*tids)(HMS_FAST_PerfHintConfigBuilder *, int *, uint32_t);
    HMS_FAST_SchedulingOptimization_ErrorCode (*build)(HMS_FAST_PerfHintConfigBuilder *, HMS_FAST_PerfHintConfig **);
    void (*free_cfg)(HMS_FAST_PerfHintConfig *);
    HMS_FAST_SchedulingOptimization_ErrorCode (*hint)(const HMS_FAST_PerfHintConfig *);
} g_ph;
static int ph_ready(void) {
    if (!g_ph.tried) { g_ph.tried = 1; const char *e = getenv("DM_PERFHINT"); if (e && *e == '0') return 0;
        void *l = dlopen("libscheduling_optimization.z.so", RTLD_NOW | RTLD_LOCAL); if (!l) { LOG("PerfHint：库加载不到（%s），跳过", dlerror()); return 0; }
        *(void **)&g_ph.create = dlsym(l, "HMS_FAST_PerfHintConfigBuilder_Create"); *(void **)&g_ph.destroy = dlsym(l, "HMS_FAST_PerfHintConfigBuilder_Destroy");
        *(void **)&g_ph.type = dlsym(l, "HMS_FAST_PerfHintConfigBuilder_SetSceneType"); *(void **)&g_ph.state = dlsym(l, "HMS_FAST_PerfHintConfigBuilder_SetSceneState");
        *(void **)&g_ph.dur = dlsym(l, "HMS_FAST_PerfHintConfigBuilder_SetDurationType"); *(void **)&g_ph.tids = dlsym(l, "HMS_FAST_PerfHintConfigBuilder_SetTids");
        *(void **)&g_ph.build = dlsym(l, "HMS_FAST_PerfHintConfigBuilder_Build"); *(void **)&g_ph.free_cfg = dlsym(l, "HMS_FAST_PerfHintConfig_Destroy");
        *(void **)&g_ph.hint = dlsym(l, "HMS_FAST_SchedulingOptimization_PerfHint");
        if (!g_ph.create || !g_ph.type || !g_ph.state || !g_ph.dur || !g_ph.tids || !g_ph.build || !g_ph.hint) { g_ph.hint = NULL; LOG("PerfHint：接口不全，跳过"); } }
    return g_ph.hint != NULL;
}
static void ph_send(HMS_FAST_SchedulingOptimization_SceneType type, int begin, HMS_FAST_SchedulingOptimization_DurationType dur, const char *what) {
    if (!ph_ready()) return;
    int tids[8], n = 0; if (g_gtx.main_tid) tids[n++] = g_gtx.main_tid; if (g_gtx.render_tid) tids[n++] = g_gtx.render_tid;
    for (int i = 0; i < g_gtx.nkeys && n < 8; i++) tids[n++] = g_gtx.key_tids[i];
    HMS_FAST_PerfHintConfigBuilder *b = NULL; HMS_FAST_PerfHintConfig *c = NULL; int r = g_ph.create(&b);
    if (!r) r = g_ph.type(b, type); if (!r) r = g_ph.state(b, begin ? HMS_FAST_BEGIN : HMS_FAST_END); if (!r) r = g_ph.dur(b, dur);
    if (!r && n) r = g_ph.tids(b, tids, (uint32_t)n); if (!r) r = g_ph.build(b, &c); if (!r) r = g_ph.hint(c);
    if (c && g_ph.free_cfg) g_ph.free_cfg(c); if (b && g_ph.destroy) g_ph.destroy(b);
    LOG("PerfHint：%s %s（%d 个线程）→ %d", what, begin ? "开始" : "结束", n, r);
}
void dm_platform_menu_ready(void) { pthread_mutex_lock(&g_gtx_mu); if (g_ph.launch_on) { g_ph.launch_on = 0; ph_send(HMS_FAST_APP_LAUNCH, 0, HMS_FAST_LONG, "启动"); } pthread_mutex_unlock(&g_gtx_mu); }
// who：0 游戏主线程 1 渲染（翻译）线程 2 其他关键线程（出图 / 音频）| who: 0 game main thread, 1 render (translation) thread, 2 other key threads (present / audio)
void front_gtx_register(int who) {
    pthread_mutex_lock(&g_gtx_mu);
    int tid = (int)gettid();
    if (who == 0) g_gtx.main_tid = tid; else if (who == 1) g_gtx.render_tid = tid; else if (g_gtx.nkeys < 5) g_gtx.key_tids[g_gtx.nkeys++] = tid;
    gtx_try_activate();
    if (who == 1 && g_gtx.main_tid && !g_ph.tried) { g_ph.launch_on = 1; ph_send(HMS_FAST_APP_LAUNCH, 1, HMS_FAST_LONG, "启动"); }
    pthread_mutex_unlock(&g_gtx_mu);
}
// 场景：1 局内 2 读取 3 前端页 | Scene: 1 in a run 2 loading 3 front-end pages
void front_gtx_scene(int scene) {
    { static int last; if (scene != last) { pthread_mutex_lock(&g_gtx_mu);   // 读取画面进出：PerfHint PAGE_LOAD | loading screen in / out: PerfHint PAGE_LOAD
        if (scene == 2 && !g_ph.load_on) { if (g_ph.launch_on) { g_ph.launch_on = 0; ph_send(HMS_FAST_APP_LAUNCH, 0, HMS_FAST_LONG, "启动"); } g_ph.load_on = 1; ph_send(HMS_FAST_PAGE_LOAD, 1, HMS_FAST_MEDIUM, "读取画面"); }
        else if (scene != 2 && g_ph.load_on) { g_ph.load_on = 0; ph_send(HMS_FAST_PAGE_LOAD, 0, HMS_FAST_MEDIUM, "读取画面"); }
        last = scene; pthread_mutex_unlock(&g_gtx_mu); } }
    if (g_gtx.active != 1 || !g_gtx.scene_info || scene == g_gtx.scene) return;
    pthread_mutex_lock(&g_gtx_mu);
    if (scene != g_gtx.scene) {
        g_gtx.scene = scene;
        OpenGTX_GameSceneInfo si; memset(&si, 0, sizeof si);
        si.sceneID = scene == 1 ? PLAYING : scene == 2 ? LOADING : GAME_INTERFACE;
        si.description = (char *)(scene == 1 ? "in run" : scene == 2 ? "loading" : "menus");
        si.recommendFPS = 60; si.minFPS = 30; si.maxFPS = 60;
        int dm = atomic_load(&g_screen_w), sh = atomic_load(&g_screen_h);
        si.resolutionCurValue.height = sh > dm ? dm : sh; si.resolutionCurValue.width = sh > dm ? sh : dm;
        OpenGTX_ErrorCode r = g_gtx.scene_info(g_gtx.ctx, &si);
        LOG("OpenGTX：场景 → %s，结果 %d", si.description, (int)r);
    }
    pthread_mutex_unlock(&g_gtx_mu);
}
// 每帧（出图前）下发一次渲染信息（我们拿不到游戏相机，填 0；接口要求每帧调）| once per frame before present (the game camera is not available to us, so zeros; the API asks for a call every frame)
void front_gtx_frame(void) {
    if (g_gtx.active != 1 || !g_gtx.frame_info) return;
    OpenGTX_FrameRenderInfo fi; memset(&fi, 0, sizeof fi); g_gtx.frame_info(g_gtx.ctx, &fi);
}

// 真全屏开没开（同 iOS g_full）：第一次问分辨率时按屏幕比例定 | Whether true fullscreen is on (as iOS g_full): decided from the screen aspect the first time the resolution is asked
static int g_full;
int front_ohos_full(void) { return g_full; }

// 游戏输出分辨率：同 iOS 规则（屏比 16:9 窄 → 真全屏，按屏幕比例出）；高度沿用安卓的 900 / 720 档（像素不跟屏幕原生走，省 GPU）。DM_GAME_RES 钉住
// Game output resolution: same rule as iOS (screen narrower than 16:9 → true fullscreen at the screen aspect); the height keeps Android's 900 / 720 profiles (not native pixels, to save GPU). DM_GAME_RES pins it
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    int dm = atomic_load(&g_screen_w), sh = atomic_load(&g_screen_h);
    unsigned bh = docked ? 900 : 720;
    if (dm > 0 && sh > 0) {
        double aspect = dm > sh ? (double)dm / sh : (double)sh / dm;   // 横屏比例（窗口首次报尺寸时可能还是竖屏）| landscape aspect (the first window size may still be portrait)
        static int decided;
        if (!decided) { decided = 1;
            if (aspect < 16.0 / 9 - 0.01) { const char *fu = getenv("DM_IPAD_FULL");
                if (!fu || atoi(fu)) { g_full = 1; dm_uiscreen_enable(DM_UISCREEN_BOTTOM); xbg_set_ui_rect_fn(ui_rect_frac);
                    LOG("画面：屏幕 %dx%d（比例 %.3f）比 16:9 窄 → 真全屏：局内铺满、界面按 16:9 贴底，前端 16:9 居中留边", dm, sh, aspect); } } }
        // 主机档 = 屏幕原生像素（同 iOS：界面文字原生清晰，3D 由游戏的渲染比例另缩）；掌机档 720 × 屏幕比例。宽 ≤ 4096、8 对齐
        // Docked = native screen pixels (as iOS: UI text sharp at native resolution, 3D scaled separately by the game's render scale); handheld = 720 × screen aspect. Width ≤ 4096, aligned to 8
        if (g_full || aspect > 16.0 / 9 + 0.01) {
            unsigned nh = (unsigned)(dm < sh ? dm : sh);
            // 原生像素出图（缺省开，DM_NATIVE_RES=0 退回 900 高）：3D 比例 = 700 / 屏高（夹在 0.5–0.75，同 d2r-harmonyos），3D 像素量与 900 高 × 0.75 相当，
            // 界面文字清晰；Pura X 实测约少 2–3 FPS（GPU 24 → 27 ms）。用户在 dm/env 里给了 DM_3D_SCALE 就不动
            // Native-pixel output (on by default, DM_NATIVE_RES=0 falls back to 900 tall): 3D scale = 700 / screen height (clamped to 0.5-0.75, as d2r-harmonyos), so the 3D pixel count matches
            // 900 tall x 0.75 while UI text is sharp; measured about 2-3 FPS less on the Pura X (GPU 24 -> 27 ms). A DM_3D_SCALE given in dm/env is left alone
            const char *nr = getenv("DM_NATIVE_RES");
            if ((!nr || *nr != '0') && docked && nh >= 720) { unsigned nw = (unsigned)(llround(nh * aspect / 8) * 8); if (nw > 4096) { nw = 4096; nh = (unsigned)llround(4096 / aspect); }
                if (!getenv("DM_3D_SCALE")) { double sc = 700.0 / nh; char v[16]; snprintf(v, sizeof v, "%.2f", sc < 0.5 ? 0.5 : sc > 0.75 ? 0.75 : sc); setenv("DM_3D_SCALE_DEFAULT", v, 0); setenv("DM_3D_SCALE_FLOOR", v, 0);
                    LOG("画面：原生像素出图 %ux%u，3D 比例 %s", nw, nh, v); }
                return (uint64_t)nh << 32 | nw; }
            if (docked && !getenv("DM_3D_SCALE")) { setenv("DM_3D_SCALE_DEFAULT", "0.75", 0); setenv("DM_3D_SCALE_FLOOR", "0.75", 0); }
            unsigned bw = (unsigned)(llround((double)bh * aspect / 8) * 8); if (bw > 4096) bw = 4096;
            return (uint64_t)bh << 32 | bw;
        }
    }
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}

// 游戏视图矩形（触控层 / 自绘底栏 / 技能钮都在它里面），同 front_ios.m letterbox_rect；输入输出单位与 ArkTS 一致（vp）
// Game view rectangle (the touch layer / custom bottom bar / skill buttons all live in it), as front_ios.m letterbox_rect; units as ArkTS (vp)
void front_ohos_view_rect(float fw, float fh, float out[4]) {
    out[0] = 0; out[1] = 0; out[2] = fw; out[3] = fh;
    if (fw <= 0 || fh <= 0) return;
    if (g_full) {
        int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h);
        float vh = floorf(fw * 9 / 16), vy = floorf((fh - vh) / 2);
        if (dm_screen_real(&rw, &rh) && h > 0 && h < rh) { vy = fh * y / rh; vh = fh * h / rh; }
        else if (rw > 0 && rh > 0 && (double)rw * 9 > (double)rh * 16) { vh = fw * rh / rw; vy = (fh - vh) / 2; }
        out[1] = vy; out[3] = vh; return;
    }
    uint64_t r = front_game_res(1); double gw = (double)(uint32_t)r, gh = (double)(r >> 32);
    if (gw <= 0 || gh <= 0) return;
    double ga = gw / gh, wa = (double)fw / fh; if (fabs(wa / ga - 1) < 0.01) return;
    float w = fw, h = fh;
    if (wa > ga) w = floorf(fh * (float)ga); else h = floorf(fw / (float)ga);
    out[0] = floorf((fw - w) / 2); out[1] = floorf((fh - h) / 2); out[2] = w; out[3] = h;
}

__attribute__((weak)) int front_android_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap; return -1;
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!front_metal_layer()) return -1;
    return front_android_text_input(title, sub, guide, initial, max_chars, out, out_cap);
}
