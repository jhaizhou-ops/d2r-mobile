// nn::oe（应用运行环境）：真机上经 am/oe 服务查询主机状态；宿主直接给固定状态。
// oe::Initialize 在 hle_sdkinit.c，不在这里。
// 环境开关：DM_OPMODE=0 掌机 / 1 主机（默认 1，对应主机模式的渲染档）；DM_LANG=语言码（默认 zh-Hans）。
// nn::oe (application operating environment): on the real device the console state is queried through the am/oe services; the host gives fixed state directly.
// oe::Initialize is in hle_sdkinit.c, not here.
// Environment switches: DM_OPMODE=0 handheld / 1 console (default 1, corresponding to the console-mode render profile); DM_LANG=language code (default zh-Hans).
#include "dm_cursor.h"
#include "dm_custom.h"
#include "hle.h"
#ifdef __ANDROID__
#include "../android/services_platform.h"
#include <errno.h>
#include <math.h>
#else
#include "dm_uiscreen.h"
#endif

// 运行模式是运行时状态（前端可改：iOS 过热降级切掌机）：唯一来源在此，启动时从 DM_OPMODE 取一次，之后不再读环境变量
// The operation mode is runtime state (the front end can change it: iOS overheat downgrade switches to handheld): the single source is here, taken once from DM_OPMODE at startup, the environment variable is not read again afterwards
#include <stdatomic.h>
static _Atomic int g_opmode = -1;
int dm_opmode(void) {
    int m = g_opmode;
    if (m < 0) {
#ifdef __ANDROID__
        int v = 0; // Android touch host uses the original handheld execution profile.
#else
        const char *e = getenv("DM_OPMODE"); int v = e ? atoi(e) : 1;
#endif
        int exp = -1; atomic_compare_exchange_strong(&g_opmode, &exp, v); m = g_opmode;
    }
    return m;
}
// ---------- 系统通知队列（AppletMessage）：前端状态变化 → 游戏每帧 TryPopNotificationMessage 取 ----------
// 消息号（游戏 main 0xe10d30 实际处理的）：15 焦点变化、30 运行模式变化、31 性能模式变化
// ---------- System notification queue (AppletMessage): front end state changes → fetched by the game every frame with TryPopNotificationMessage ----------
// Message numbers (those actually handled by the game at main 0xe10d30): 15 focus change, 30 operation mode change, 31 performance mode change
enum { MSG_FOCUS = 15, MSG_OPMODE = 30, MSG_PERFMODE = 31 };
#define MQ 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_mq[MQ]; static int g_mqh, g_mqn;
static _Atomic int g_ntf_opmode, g_ntf_perfmode;          // 游戏用 Set*NotificationEnabled 打开后才推 | only pushed after the game turns it on with Set*NotificationEnabled
static void mq_push(uint32_t m) {
    pthread_mutex_lock(&g_mq_lock);
    // 15/30/31 是「查询当前状态」的失效通知，不是携带历史值的事件。
    // 同类型尚未取走时保留首次排位：D2R 消费循环只读最新状态，重复通知的写回相同，尺寸相同不再 resize。
    // 只合并这三种状态通知；未来的逐次事件（如 Resume/Exit）不能套这个规则。
    // 合法生产者只发这三种，因此待取最多三项，任意多次失焦/恢复不再撞 32 槽上限；弹出后新变化照常入队。
    // 15/30/31 are invalidation notifications meaning "query the current state", not events carrying historical values.
    // While one of the same type has not been taken yet, the first queue position is kept: the D2R consumer loop only reads the latest state, repeated notifications write back the same thing, and with the same size there is no further resize.
    // Only these three state notifications are merged; future per-occurrence events (such as Resume/Exit) must not use this rule.
    // Legitimate producers only send these three, so at most three items are pending, and any number of focus losses/restores no longer hits the 32-slot limit; after a pop, new changes are enqueued as usual.
    if (m == MSG_FOCUS || m == MSG_OPMODE || m == MSG_PERFMODE)
        for (int i = 0; i < g_mqn; i++)
            if (g_mq[(g_mqh + i) % MQ] == m) { pthread_mutex_unlock(&g_mq_lock); return; }
    if (g_mqn == MQ) die("oe 通知队列溢出（游戏没在取？）");
    g_mq[(g_mqh + g_mqn++) % MQ] = m;
    pthread_mutex_unlock(&g_mq_lock);
}
void dm_oe_res_changed(void) { if (getenv("DM_TEST_OE_FOCUS_FIRST")) mq_push(MSG_FOCUS);   /* 测试：让 31 排在一条焦点通知后面（iOS 包启动时的顺序） | Test: queue 31 behind a focus notification (the order at iOS app startup) */
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE); else LOG("【红】分辨率要切，但游戏没打开性能模式变化通知"); }   // 出图分辨率该变了（dm_uiscreen.c）：让游戏重读参数位 | the render resolution should change (dm_uiscreen.c): make the game re-read the parameter bits
// 只对 GetOperationMode 报的运行模式（手柄 / 触屏布局切换让游戏重载布局用，dm_padmode.c）：m ≥ 0 报 m 并推一次运行模式通知，m < 0 报回真值再推一次。
// 手柄样式（hle_hid.c connected_id）与性能模式照真值，所以不弹「重新连接控制器」、出图分辨率不变
// Operation mode reported to GetOperationMode only (used to make the game reload layouts when switching between controller / touch layouts, dm_padmode.c):
// m ≥ 0 reports m and pushes one operation-mode notification; m < 0 reports the true value again and pushes one more.
// The controller style (hle_hid.c connected_id) and performance mode keep their true values, so no "Reconnect controller" prompt appears and the render
// resolution does not change
static _Atomic int g_ui_opmode = -1;
static _Atomic int g_ui_armed; static _Atomic uint32_t g_ui_reads;   // 推过通知后游戏第一次来读运行模式记一次：切换事务拿它回读「游戏收到了」 | Counts once when the game first comes to read the operation mode after a notification has been pushed: the switch transaction uses it to read back "the game received it"
uint32_t dm_oe_ui_opmode_reads(void) { return atomic_load(&g_ui_reads); }
void dm_oe_ui_opmode(int m) {
#ifdef __ANDROID__
    if (m >= 0) die("Android UI operation-mode override needs a validated game build");
#endif
    g_ui_opmode = m < 0 ? -1 : m; atomic_store(&g_ui_armed, 1); if (g_ntf_opmode) mq_push(MSG_OPMODE);
}
void dm_set_opmode(int m) {   // 前端调（过热降级等）：改状态并按游戏打开的开关推送 30 / 31 | called by the front end (overheat downgrade etc.): change the state and push 30 / 31 according to the switches the game has turned on
    int old = dm_opmode(); if (old == m) return;
    g_opmode = m;
    LOG("oe 运行模式 %d → %d", old, m);
    if (g_ntf_opmode) mq_push(MSG_OPMODE);
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE);
}
// FocusState：1 InFocus、2 OutOfFocus、3 Background。前端调 dm_focus_set（iOS 生命周期 / Mac 窗口失焦）
// FocusState: 1 InFocus, 2 OutOfFocus, 3 Background. The front end calls dm_focus_set (iOS lifecycle / Mac window losing focus)
static _Atomic int g_focus = 1; static _Atomic uint64_t g_focus_in_ns;   // 最近一次进入 InFocus 的时刻 | time of the most recent entry into InFocus
int dm_focus_now(void) { return atomic_load(&g_focus); }
uint64_t dm_focus_in_since(void) { return atomic_load(&g_focus_in_ns); }   // 图形层显示节拍看门狗：只在激活时算停顿，从激活起算（nvn_mtl.m mtl_present_wait） | graphics layer display link watchdog: stalls are only computed while active, counted from activation (nvn_mtl.m mtl_present_wait)
void dm_focus_set(int st) {
    if (st < 1 || st > 3) die("dm_focus_set 非法状态 %d", st);
    if (st == 1) atomic_store(&g_focus_in_ns, now_ns());
    if (atomic_exchange(&g_focus, st) == st) return;
    LOG("oe 焦点状态 → %d", st);
    mq_push(MSG_FOCUS);
}
static int opmode(void) { return dm_opmode(); }

// 截图 / 录像开关：宿主无系统截图服务，空操作
// Screenshot / video recording switches: the host has no system screenshot service, no-op
HLE_FORCE("_ZN2nn2oe15EnableRecordingEv", "录像许可策略", void, h_oe_EnableRec, void) {
#ifdef __ANDROID__
    dm_android_platform_setting("recording_allowed", 1);
#endif
}
HLE_FORCE("_ZN2nn2oe16DisableRecordingEv", "录像许可策略", void, h_oe_DisableRec, void) {
#ifdef __ANDROID__
    dm_android_platform_setting("recording_allowed", 0);
#endif
}
HLE_FORCE("_ZN2nn2oe16EnableScreenShotEv", "截图许可策略", void, h_oe_EnableShot, void) {
#ifdef __ANDROID__
    dm_android_platform_setting("screenshots_allowed", 1);
#endif
}
HLE_FORCE("_ZN2nn2oe17DisableScreenShotEv", "截图许可策略", void, h_oe_DisableShot, void) {
#ifdef __ANDROID__
    dm_android_platform_setting("screenshots_allowed", 0);
#endif
}

// OperationMode：0 Handheld、1 Console；PerformanceMode：0 Normal、1 Boost（与运行模式一一对应）
// OperationMode: 0 Handheld, 1 Console; PerformanceMode: 0 Normal, 1 Boost (one-to-one with the operation mode)
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "运行模式查询；按 DM_OPMODE 给固定值", int, h_oe_GetOpMode, void) { int u = g_ui_opmode; if (atomic_exchange(&g_ui_armed, 0)) atomic_fetch_add(&g_ui_reads, 1); return u >= 0 ? u : opmode(); }
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "性能模式查询；与运行模式一致", int, h_oe_GetPerfMode, void) { return opmode(); }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "设 CPU/GPU 频率档；宿主不调频", void, h_oe_SetPerfCfg, int mode, int cfg) {
#ifdef __ANDROID__
    if (mode < 0 || mode > 1) die("Invalid Android guest performance profile");
    dm_android_platform_setting(mode ? "guest_console_performance_request" : "guest_handheld_performance_request", cfg);
    LOG("Android guest performance request recorded; device clocks remain controlled by Android");
#else
    LOG("oe::SetPerformanceConfiguration mode=%d cfg=%#x（忽略）", mode, cfg);
#endif
}

// 脚本化状态变化（无窗口验证生命周期 / 过热降级）：DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30"（第 N 秒）
// 时刻从游戏第一次取通知算起；「@ingame+N」= 读取画面第 K 次撤下（K = 前面 @ingame 项数 + 1，首个即进局）后 N 秒（按事件，不受负载影响）。与前端调用并存。
// Scripted state changes (verify lifecycle / overheat downgrade without a window): DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30" (second N)
// Time counts from the game's first notification fetch; "@ingame+N" = N seconds after the loading screen is taken down for the K-th time (K = number of preceding @ingame items + 1, the first one is run entry) (event-based, not affected by load). Coexists with front end calls.
#include <unistd.h>
static _Atomic int g_ls_unloads;   // 读取画面撤下次数（diag.c 的 UIMSG 处理调 dm_note_loadscreen_unload） | number of times the loading screen was taken down (the UIMSG handler in diag.c calls dm_note_loadscreen_unload)
void dm_note_loadscreen_unload(void) { atomic_fetch_add(&g_ls_unloads, 1); }
#ifndef __ANDROID__
static void *oe_script(void *arg) {
    char *buf = arg; uint64_t t0 = now_ns();
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char key[16]; int v; double at;
        if (sscanf(tok, "%15[a-z]=%d@ingame+%lf", key, &v, &at) == 3) {
            static int need;
            need++; while (atomic_load(&g_ls_unloads) < need) usleep(20000);
            usleep((useconds_t)(at * 1e6));
        } else {
            if (sscanf(tok, "%15[a-z]=%d@%lf", key, &v, &at) != 3) die("DM_OE_SCRIPT 项「%s」格式应为 键=值@秒 或 键=值@ingame+秒", tok);
            uint64_t due = t0 + (uint64_t)(at * 1e9), now = now_ns();
            if (due > now) usleep((useconds_t)((due - now) / 1000));
        }
        if (!strcmp(key, "focus")) dm_focus_set(v);
        else if (!strcmp(key, "opmode")) dm_set_opmode(v);
        else die("DM_OE_SCRIPT 未知键「%s」（focus / opmode）", key);
    }
    return NULL;
}
static void oe_script_start(void) {
    const char *e = getenv("DM_OE_SCRIPT"); if (!e) return;
    pthread_t th; if (pthread_create(&th, NULL, oe_script, strdup(e))) die("DM_OE_SCRIPT 线程");
    pthread_detach(th);
}
static pthread_once_t g_oe_once = PTHREAD_ONCE_INIT;
#endif

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "前后台状态；由前端 dm_focus_set 驱动", int, h_oe_GetFocus, void) { return g_focus; }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "前后台切换策略；游戏设 1（收焦点通知），宿主按通知语义实现", void, h_oe_SetFocusMode, int m) {
#ifdef __ANDROID__
    if (m < 0 || m > 2) die("Unsupported Android guest focus handling mode %d", m);
    dm_android_platform_setting("focus_handling_mode", m);
#endif
    LOG("oe::SetFocusHandlingMode %d", m);
}

// 通知：返回 bool（w0），宿主没有系统通知
// ---------- 3D 内部渲染比例 ----------
// 游戏自带一个静态的 3D 渲染比例：渲染器对象（全局指针 [main+0x31e0848]）成员 +0x66a4（float，原版恒 1.0）。
// 改分辨率函数 main+0x8582f0(输出宽, 输出高) → main+0x1627ef0 按「比例 × 输出宽高」重建 3D 目标（+0xee78 / +0xee7c），
// 界面层与 Final Result 仍按输出尺寸（+0xee88 / +0xee8c）建，末端由游戏自己的放大着色器 ps_upsample_p 放回输出尺寸。
// 启动流程不调 0x8582f0，所以要主动调一次；调用点必须是游戏主线程取系统通知处——游戏自己处理模式切换的同一上下文
// （在出图 HLE 里调会自锁：渲染锁已被出图路径持有）。
// 缺省：iOS 0.75，Mac 宿主 1.0（画面闸门对 Switch 真值比）；DM_3D_SCALE=0.5–1.0 覆盖。
// 视觉尺子真值阶段（DM_NVN_RULER=1 的 A / B）强制 1.0，切到被测模式后才生效。
// 动态比例（系统降频看的是占用率，温控档位只是粗信号 → 按实际掉帧调）：控制器吃出图线程报来的实际上屏间隔
// （与 perf.txt 同源），三级 配置值(0.75) → 0.72 → 0.70；下限 0.70（实看 0.65 有差别）。规则见 scale3d_ctl_step。
// 开关：DM_3D_SCALE_DYN=0 关；DM_THERMAL_NOFALLBACK=1（功耗 / 帧时间测量）也关——测量时比例必须钉住。
// DM_3D_SCALE_DYN_INJECT=帧起-帧止（按 dm_nvn_frame_count 计）：诊断，这段帧里把喂给控制器的间隔一律当两个周期（掉帧），段外当一个周期
// （Mac 上没有上屏时刻，用它验降级 / 回升的整条链）。只在局内生效（不在局内不判），满 10 秒（5 窗）降第一档。
// Notification: returns bool (w0); the host has no system notifications
// ---------- 3D internal render scale ----------
// The game has a built-in static 3D render scale: renderer object (global pointer [main+0x31e0848]) member +0x66a4 (float, always 1.0 in the original game).
// The resolution-change function main+0x8582f0(output width, output height) → main+0x1627ef0 rebuilds the 3D targets at "scale × output size" (+0xee78 / +0xee7c),
// while the UI layer and Final Result are still built at the output size (+0xee88 / +0xee8c), and at the end the game's own upscale shader ps_upsample_p scales back to the output size.
// The startup flow does not call 0x8582f0, so it must be called once proactively; the call site must be where the game main thread fetches system notifications, the same context in which the game itself handles mode switches
// (calling it inside the present HLE self-deadlocks: the render lock is already held by the present path).
// Default: iOS 0.75, Mac host 1.0 (the picture gate compares against Switch ground truth); DM_3D_SCALE=0.5–1.0 overrides.
// During the visual ruler ground-truth phase (A / B of DM_NVN_RULER=1) it is forced to 1.0 and takes effect only after switching to the mode under test.
// Dynamic scale (system throttling looks at utilization and the thermal level is only a coarse signal → adjust by actual dropped frames): the controller consumes the actual present intervals reported by the present thread
// (same source as perf.txt), three levels: configured value (0.75) → 0.72 → 0.70; floor 0.70 (0.65 is visibly different). Rules: see scale3d_ctl_step.
// Switches: DM_3D_SCALE_DYN=0 disables it; DM_THERMAL_NOFALLBACK=1 (power / frame-time measurement) also disables it, since the scale must be pinned during measurement.
// DM_3D_SCALE_DYN_INJECT=start frame-end frame (counted by dm_nvn_frame_count): diagnostics; within that frame range every interval fed to the controller is treated as two periods (dropped), outside it as one period
// (the Mac has no present timestamps, so this verifies the whole downgrade / recovery chain). Active only in-game (no judging outside a game); the first level drops after a full 10 s (5 windows).
#ifndef __ANDROID__
#include <TargetConditionals.h>
// 3D 比例的档位表：0 档 = 配置值（iPhone 0.75），往下每档一个比例；下限由 DM_3D_SCALE_FLOOR（缺省见 S3_FLOOR_DEF）截断。只动 3D 渲染比例，界面和文字不降
// 3D scale level table: level 0 = configured value (iPhone 0.75), one scale per level going down; the floor is clamped by DM_3D_SCALE_FLOOR (default: see S3_FLOOR_DEF). Only the 3D render scale changes; UI and text are not lowered
static const float g_s3_levels[] = {0.70f, 0.65f, 0.60f, 0.55f, 0.50f};   // 1 档起 | Starting from level 1
#define S3_NLV ((int)(sizeof g_s3_levels / sizeof g_s3_levels[0]))
#define S3_FLOOR_DEF 0.60f   // 待真机扫档（热机稳态最后 30 秒掉帧 ≤ 1%）定 | To be set by a device level sweep (dropped frames ≤ 1% in the last 30 s of warm steady state)
// 纯函数控制器（单测 t_oe.c 直接喂序列）：level 0 = 配置值，越大越低。判据只认实际上屏：
//   掉帧 = 上屏间隔 > 1.5 × 目标周期（60 帧时 25 ms），或被系统丢弃（−1）；每 2 秒一个窗，记窗内帧数与掉帧数，留最近 30 窗（60 秒）。
//   降：最近 5 窗（10 秒）掉帧率 > 5%（快：重场景一来就降），或最近 30 窗（60 秒）掉帧率 > 1.5%（慢：长期小掉帧也降）→ 降一档，停判 3 秒、窗记录清空。
//   升：连续 15 窗（30 秒）一帧不掉 → 升一档；升上去 15 秒内又被打回，下一次升要连续 45 窗（90 秒）。
//   阈值按实玩记录定：实玩第二段（帧率 59+、偶发卡顿）最坏 10 秒 3.7%、每分钟 0.97% / 0.47% / 0.39%，两条都不触发；v1.0.15 热机重场景 12.6% 触发。
// 旧规则（窗帧率 < 目标 × 0.92 才降、只有 0.72 / 0.70 两档）在重场景降到 0.70 就到底了，热机稳态仍掉 12.6%（Hc 局）。
// Pure-function controller (the unit test t_oe.c feeds it sequences directly): level 0 = configured value, higher = lower. The criterion counts only actual on-screen presentation:
//   Dropped frame = present interval > 1.5 × target period (25 ms at 60 fps), or discarded by the system (−1); one window every 2 s, recording the frames and dropped frames in it, keeping the last 30 windows (60 s).
//   Lower: dropped-frame rate over the last 5 windows (10 s) > 5% (fast: lower as soon as a heavy scene arrives), or over the last 30 windows (60 s) > 1.5% (slow: also lower on long-term minor drops) → down one level, pause judging for 3 s, clear window records.
//   Raise: 15 consecutive windows (30 s) without a dropped frame → up one level; if knocked back within 15 s of being raised, the next raise needs 45 consecutive windows (90 s).
//   Thresholds set from real-play recordings: real-play segment 2 (59+ fps, occasional stutter) worst 10 s 3.7%, per minute 0.97% / 0.47% / 0.39%, triggering neither rule; v1.0.15 warm-device heavy scene 12.6% triggers.
// Old rule (lower only when the window frame rate < target × 0.92, only two levels 0.72 / 0.70) bottomed out at 0.70 in heavy scenes, and warm steady state still dropped 12.6% (Hc run).
typedef struct { int level, max_level; double period_ms; uint64_t hold_until, last_up; double win_ms; unsigned win_n, win_d;
                 uint16_t wn[30], wd[30]; int nw; int good_run, need_good; double last_r10, last_r60; } scale3d_ctl;
#define S3_SEC 1000000000ull
static void scale3d_ctl_hold(scale3d_ctl *c, uint64_t now, uint64_t ns) {   // 停判一段时间（进局 / 回前台 / 换级之后）：窗与历史清零 | Pause judging for a while (after entering a game / returning to foreground / changing level): clear windows and history
    if (now + ns > c->hold_until) c->hold_until = now + ns;
    c->win_ms = 0; c->win_n = c->win_d = 0; c->nw = 0; c->good_run = 0;
}
static double s3_rate(const scale3d_ctl *c, int k) {   // 最近 k 窗的掉帧率（窗不足 k 个按已有的算；没有窗返回 0） | Dropped-frame rate over the last k windows (fewer than k windows: compute over those present; no windows returns 0)
    unsigned n = 0, d = 0; for (int i = 0; i < k && i < c->nw; i++) { n += c->wn[i]; d += c->wd[i]; } return n ? (double)d / n : 0;
}
static int scale3d_ctl_step(scale3d_ctl *c, double iv_ms, uint64_t now) {   // 返回当前级 | returns the current level
    if (now < c->hold_until || iv_ms == 0) return c->level;   // 停判中 / 无读数（0） | Judging paused / no reading (0)
    double per = c->period_ms > 0 ? c->period_ms : 1000.0 / 60;
    c->win_n++; if (iv_ms < 0 || iv_ms > per * 1.5) c->win_d++;   // 被丢弃（−1）也算掉帧 | Discarded (−1) also counts as a dropped frame
    c->win_ms += iv_ms > 0 ? iv_ms : per;
    if (c->win_ms < 2000) return c->level;
    memmove(c->wn + 1, c->wn, sizeof c->wn - sizeof c->wn[0]); memmove(c->wd + 1, c->wd, sizeof c->wd - sizeof c->wd[0]);
    c->wn[0] = (uint16_t)c->win_n; c->wd[0] = (uint16_t)c->win_d; if (c->nw < 30) c->nw++;
    int clean = c->win_d == 0; c->win_ms = 0; c->win_n = c->win_d = 0;
    c->last_r10 = s3_rate(c, 5); c->last_r60 = c->nw >= 30 ? s3_rate(c, 30) : 0;
    if (c->level < c->max_level && ((c->nw >= 5 && c->last_r10 > 0.05) || (c->nw >= 30 && c->last_r60 > 0.015))) {
        if (c->last_up && now - c->last_up < 15 * S3_SEC) c->need_good = 45;   // 刚升上去 15 秒内又掉：下次升要 90 秒 | Dropped again within 15 s of being raised: the next raise needs 90 s
        c->level++; scale3d_ctl_hold(c, now, 3 * S3_SEC); return c->level;
    }
    if (clean) { if (++c->good_run >= (c->need_good ? c->need_good : 15) && c->level > 0) { c->level--; c->last_up = now; c->need_good = 0; scale3d_ctl_hold(c, now, 3 * S3_SEC); } }
    else c->good_run = 0;
    return c->level;
}
// 画质设置（做界面，这里给接口）：0 = 流畅优先（缺省，掉帧就自动降 3D 比例）；1 = 画质优先（不自动降，保持配置值）。档位名与缺省值等
// Quality setting (UI to be built; this provides the interface): 0 = smoothness first (default; automatically lowers the 3D scale on frame drops); 1 = quality first (no automatic lowering, keeps the configured value). Level names, defaults, etc.
static _Atomic int g_3d_quality_mode;
void dm_3d_quality_mode_set(int mode) { atomic_store(&g_3d_quality_mode, mode == 1 ? 1 : 0); }
int dm_3d_quality_mode_get(void) { return atomic_load(&g_3d_quality_mode); }
static _Atomic int g_scale3d_level;
static _Atomic int g_scale3d_resumed;
void dm_3d_scale_resumed(void) { atomic_store(&g_scale3d_resumed, 1); }   // 前端：刚回前台。回来头几秒的慢帧是恢复（表面重建、缓存变冷），不是 GPU 吃紧：停判 3 秒 | front end: just returned to foreground. Slow frames in the first few seconds back are recovery (surface rebuild, caches gone cold), not GPU pressure: suspend judging for 3 seconds
void dm_3d_scale_feed(double iv_ms) {   // 出图线程每帧调（diag.c dm_perf_present 转来）：实际上屏间隔 ms，−1 = 被丢弃，0 = 无读数 | called by the present thread every frame (forwarded from diag.c dm_perf_present): actual present interval in ms, −1 = dropped, 0 = no reading
    static scale3d_ctl c; static int on = -1; static long inj0 = -1, inj1 = -1;
    if (on < 0) { const char *e = getenv("DM_3D_SCALE_DYN"), *j = getenv("DM_3D_SCALE_DYN_INJECT"); on = !(e && !strcmp(e, "0")) && !getenv("DM_THERMAL_NOFALLBACK");
        if (j) { inj0 = atol(j); inj1 = strchr(j, '-') ? atol(strchr(j, '-') + 1) : inj0; }
        const char *fe = getenv("DM_3D_SCALE_FLOOR"); float fl = fe ? strtof(fe, NULL) : S3_FLOOR_DEF;   // 下限：档位表里不低于它的最深一档 | Floor: the deepest level in the level table that is not below it
        for (int k = 0; k < S3_NLV; k++) if (g_s3_levels[k] >= fl - 1e-4f) c.max_level = k + 1;
        LOG("3D 比例控制：%s，下限 %.2f（第 %d 档）", on ? "开" : "关", c.max_level ? g_s3_levels[c.max_level - 1] : 0.0f, c.max_level); }
    if (!on) return;
    if (atomic_load(&g_3d_quality_mode) == 1) { if (c.level) { c.level = 0; LOG("3D 比例控制：画质优先，回到配置值"); } atomic_store(&g_scale3d_level, 0); return; }
    uint64_t now = now_ns();
    if (atomic_exchange(&g_scale3d_resumed, 0)) scale3d_ctl_hold(&c, now, 3 * S3_SEC);
    { static int was; int in = dm_ingame_now();   // 不在局内（菜单 / 读取）不判；刚进局的 20 秒也不判（进局头十几秒的慢帧是装载，不是 GPU 吃紧） | not judged outside a run (menu / loading); not judged in the first 20 seconds of a run either (slow frames in the first ten-odd seconds of a run are loading, not GPU pressure)
      if (!in) { was = 0; scale3d_ctl_hold(&c, now, S3_SEC); return; }
      if (!was) { was = 1; if (inj0 < 0) scale3d_ctl_hold(&c, now, 20 * S3_SEC); } }
    { int mtl_fps_target(void); double mtl_display_refresh(void), dm_pace_hz(double, double), r = mtl_display_refresh(), hz = dm_pace_hz(r, 60);
      double p = mtl_fps_target() == 30 ? 1000.0 / dm_pace_hz(r, 30) : hz != 60 ? 1000.0 / hz : 0; if (p != c.period_ms) { c.period_ms = p; scale3d_ctl_hold(&c, now, 2 * S3_SEC); } }   // 目标周期跟着出图帧率换（省电 30 帧、非 60 倍数的屏取整除档，如 144 Hz 屏 72），切换后停判 2 秒 | The target period follows the present frame rate (power saving 30 fps; screens not a multiple of 60 take the evenly dividing tier, e.g. 72 on a 144 Hz screen); checking pauses for 2 seconds after a switch
    if (inj0 >= 0) { long f = (long)dm_nvn_frame_count(); double per = c.period_ms > 0 ? c.period_ms : 1000.0 / 60; iv_ms = f >= inj0 && f <= inj1 ? 2 * per : per; }   // 注入：段内每帧当掉一帧（两个周期） | Injection: every frame within the segment counts as a dropped frame (two periods)
    int was = c.level, lv = scale3d_ctl_step(&c, iv_ms, now);
    if (lv != was) LOG("3D 比例控制：最近 10 秒掉帧 %.1f%%、60 秒 %.1f%% → %s到第 %d 档（3D 比例 %.2f）%s", c.last_r10 * 100, c.last_r60 * 100, lv > was ? "降" : "升", lv,
                       lv ? g_s3_levels[lv - 1] : -1.0f, c.need_good ? "（刚升又掉：下次升档要 90 秒）" : "");
    atomic_store(&g_scale3d_level, lv);
}
void dm_3d_scale_hot(int hot) { (void)hot; }   // 过热不再直接降档（改按实际掉帧）；保留符号给前端 | overheating no longer lowers the level directly (now by actual dropped frames); the symbol is kept for the front end
static _Atomic float g_scale3d_cur = 1.0f;
float dm_3d_scale_cur(void) { return atomic_load(&g_scale3d_cur); }
static void scale3d_tick(void) {
    static float want_cfg = -1, cur = 1.0f; static module_t *main;
    if (want_cfg < 0) {
        const char *e = getenv("DM_3D_SCALE"); want_cfg = TARGET_OS_IPHONE ? 0.75f : 1.0f;
        if (e) { char *end; float v = strtof(e, &end); if (end == e || *end || !(v >= 0.5f && v <= 1.0f)) die("DM_3D_SCALE 非法：%s（要求 0.5–1.0）", e); want_cfg = v; }
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) main = &g_mod[i];
    }
    int lv = atomic_load(&g_scale3d_level);   // 动态档：0 = 配置值，k = g_s3_levels[k − 1]（都不高于配置值） | Dynamic level: 0 = configured value, k = g_s3_levels[k − 1] (none higher than the configured value)
    float dyn = lv <= 0 ? want_cfg : g_s3_levels[(lv > S3_NLV ? S3_NLV : lv) - 1]; if (dyn > want_cfg) dyn = want_cfg;
    float want = dm_nvn_ruler_truth() ? 1.0f : dyn;
    if (want == cur || !main || dm_nvn_frame_count() < 30) return;
    uintptr_t renderer = *(uintptr_t *)(main->base + 0x31e0848); if (!renderer) return;
    uint32_t ow = *(uint32_t *)(renderer + 0xee88), oh = *(uint32_t *)(renderer + 0xee8c); if (!ow || !oh) return;
    { extern void mtl_rt_force_full(int) __attribute__((weak)); if (mtl_rt_force_full) mtl_rt_force_full(4); }   // 运行时外推：换比例前后几帧一律全画（主通道换尺寸那帧不能外推） | runtime extrapolation: the few frames before and after a scale change are all drawn in full (the frame where the main pass changes size cannot be extrapolated)
    float *scale = (float *)(renderer + 0x66a4); dm_guest_write_prepare(scale, sizeof *scale); *scale = want;
    ((void (*)(uint32_t, uint32_t))(main->base + 0x8582f0))(ow, oh);
    cur = want; atomic_store(&g_scale3d_cur, want);
    LOG("3D 渲染比例 %.2f：3D 目标 %ux%u，输出 %ux%u（帧 %llu）", want, *(uint32_t *)(renderer + 0xee78), *(uint32_t *)(renderer + 0xee7c),
        *(uint32_t *)(renderer + 0xee88), *(uint32_t *)(renderer + 0xee8c), (unsigned long long)dm_nvn_frame_count());
}
#else
/* Build91738 mapping proved against original NSO SHA0ffc74e2... / BuildID
 * 2c7f979610e9911979982dc07bfc5702. These text anchors also match the verified
 * offline rewritten main SHA2894c9b0...: no NSO header exists in that flat image.
 * Full evidence: analysis/d2r-mobile-port/render-scale-map-20261014/evidence.json.
 * Keep this Android experiment separate from the build94012 Apple addresses. */
/* Core-only host/input probes omit the graphics layer. Its real presented-frame
 * counter is mandatory only when the explicit scaling experiment is enabled. */
extern uint64_t dm_nvn_frame_count(void) __attribute__((weak));
static module_t *g_android_scale_main;
static float g_android_scale_want=1.0f;   // 配置值（DM_3D_SCALE，缺省 1.0）——也是动态档的 0 档 | configured value (DM_3D_SCALE, default 1.0) -- also level 0 of the dynamic table
static _Atomic float g_android_scale_cur=1.0f;
static uint32_t g_android_scale_ref_w, g_android_scale_ref_h;   // DM_3D_SCALE_REF：只在大时把 3D 域的输出抬到该尺寸（对齐 iOS 的 0.75×原生世界密度） | DM_3D_SCALE_REF: lift the 3D domain's output to this size only when larger
static int g_android_scale_armed;   // 1 = 实验已武装（DM_3D_SCALE 或 DM_3D_SCALE_DYN 任一） | experiment armed (either DM_3D_SCALE or DM_3D_SCALE_DYN)
static int g_android_scale_dyn;     // 1 = 动态档位生效 | dynamic levels active
static pthread_once_t g_android_scale_once=PTHREAD_ONCE_INIT;
static int g_android_scale_94012;   // 1 = 用 Apple 侧 build94012 地址映射（已对本镜像做锚点校验） | 1 = Apple-side build94012 address map (anchor-verified against this image)

static void android_scale_build_guard(module_t *main) {
    if (main && main->text_end==0x1b4f5d0 && main->ro_off==0x1b50000 && main->data_off==0x2028000 && main->size>=0x31b2538) {
        /* Anchor the notification caller, wrapper/global, scale arithmetic, and
         * target dimension sources. None of these bytes contain relocations or
         * overlap the Android instruction rewrite sites. */
        static const struct { size_t address; unsigned count; uint32_t words[30]; } anchors[]={
            {0xdf7b90,10,{0xd10283ff,0xa9046ffc,0xa90567fa,0xa9065ff8,0xa90757f6,0xa9084ff4,0xa9097bfd,0x910243fd,0x910033e0,0x94355aff}},
            {0x841540,10,{0xd10343ff,0xa9095ff8,0xa90a57f6,0xa90b4ff4,0xa90c7bfd,0x910303fd,0x90014b88,0xf9403d08,0xb4000d28,0x90014b98}},
            {0x160d210,24,{0x1e230021,0x528cd488,0xbc686800,0x1e210801,0x529dcf0a,0x1e250028,0x8b0a0017,0x1e230041,0x1e210800,0x1e250009,0xb94002ea,0xaa0003f3,0x6b08015f,0x54000081,0xb94006ea,0x6b09015f,0x540000e0,0x7100011f,0x7a401924,0x29020ae1,0x290026e8,0x290126e8,0x540000e1,0xa9447bfd}},
            {0x84168c,30,{0xb0014b97,0xf94296e9,0x528cbfaa,0x382a6928,0x90014b88,0x91020108,0xa9402500,0xf802e3ff,0xa9027fe9,0xf94252a9,0xa9017fe9,0x2947a909,0x290727ea,0x79407109,0x5280038a,0x390113ff,0x79006be9,0xb90043ea,0x29054ff4,0xf9400901,0xf9400008,0xf9419508,0x910043e2,0xd63f0100,0x08dffec8,0x36000a68,0xf94296e0,0x2a1403e1,0x2a1303e2,0x94372ebe}},
            {0x15d93dc,7,{0xd2a7f00a,0xf80342ac,0xb20007ec,0xf2c0006a,0xf803c2ac,0xd28000ac,0xf80b42aa}},
            {0x160e694,11,{0x529dcf08,0xaa0003f3,0xf0005739,0x8b08001a,0x529e6408,0x8b080000,0xf9569268,0xf9400901,0xf000573b,0xf944ab39,0xf9400342}},
            {0x160f188,6,{0x529f9808,0x8b080260,0xf9569268,0xf9400901,0xf9400b42,0x90000018}},
            {0x160f250,5,{0x529fae08,0x8b080260,0xf9569268,0xf9400901,0xf9400b42}},
            /* Reject the old whole-output resolution experiment as well. */
            {0xdf7bbc,2,{0xd280a016,0xd280c818}},
            {0xdf7bd4,1,{0xf2c05a16}},
            {0xdf7be0,1,{0xf2c07098}},
            {0xdf9c7c,2,{0xd280a008,0xf2c05a08}},
        };
        for (size_t i=0;i<sizeof anchors/sizeof *anchors;i++)
            if (memcmp(main->base+anchors[i].address,anchors[i].words,anchors[i].count*4))
                die("ANDROID_3D_SCALE build91738 anchor mismatch at %#zx",anchors[i].address);
        g_android_scale_94012=0;
        return;
    }
    /* 第二套映射：Apple 侧 build94012 地址（本树 iOS 分支用的那套）。离线已对本镜像
     * （设备实际加载的 main.patched.bin）核对：改分辨率函数序言与 24 词比例算术块逐字吻合。
     * 渲染器全局量无法离线锚定（BSS，运行时写入），故 tick 里另加：指针对齐/可读、输出必须
     * 恰好 1280x720、当前比例必须是 0.70..1.0 的有限值，且改完后逐项校验尺寸。
     * Second map: the Apple-side build94012 addresses (the ones the iOS branch in this tree
     * uses). Verified offline against this very image: the resize-function prologue and the
     * 24-word scale-arithmetic block match exactly. The renderer global cannot be anchored
     * offline (BSS, runtime-filled), so the tick additionally requires an aligned readable
     * pointer, the exact 1280x720 output size, a finite in-range current scale, and it
     * verifies every size after the rebuild. */
    if (main && main->size>=0x31e0860) {
        static const struct { size_t address; unsigned count; uint32_t words[24]; } anchors[]={
            {0x8582f0,12,{0xd10343ff,0xa9095ff8,0xa90a57f6,0xa90b4ff4,0xa90c7bfd,0x910303fd,0xf0014c28,0xf941cd08,0xb4000d28,0xf0014c38,0x910ea318,0x2a0003f4}},
            {0x1627f08,24,{0x1e230021,0x528cd488,0xbc686800,0x1e210801,0x529dcf0a,0x1e250028,0x8b0a0017,0x1e230041,0x1e210800,0x1e250009,0xb94002ea,0xaa0003f3,0x6b08015f,0x54000081,0xb94006ea,0x6b09015f,0x540000e0,0x7100011f,0x7a401924,0x29020ae1,0x290026e8,0x290126e8,0x540000e1,0xa9447bfd}},
        };
        for (size_t i=0;i<sizeof anchors/sizeof *anchors;i++)
            if (memcmp(main->base+anchors[i].address,anchors[i].words,anchors[i].count*4))
                die("ANDROID_3D_SCALE build94012 anchor mismatch at %#zx",anchors[i].address);
        g_android_scale_94012=1;
        return;
    }
    die("ANDROID_3D_SCALE unsupported main layout (neither verified 91738 nor 94012)");
}
/* ── 动态 3D 比例（Android）：档位表与状态（控制器函数在文件尾；规则与 Apple 分支逐条相同）──
 * Dynamic 3D scale (Android): level table and state (controller functions at the end of the file;
 * rules line-for-line the same as the Apple branch). */
#define S3_SEC_A 1000000000ull
#define S3_NLV_A ((int)(sizeof g_s3_levels_a/sizeof g_s3_levels_a[0]))
#define S3_FLOOR_DEF_A 0.70f   // 待真机扫档（热机稳态最后 30 秒掉帧 ≤ 1%）定 | To be set by a device sweep (dropped frames ≤ 1% in the last 30 s of warm steady state)
static const float g_s3_levels_a[]={0.85f,0.80f,0.75f,0.70f,0.65f};   // 1 档起 | starting from level 1
typedef struct { int level,max_level; double period_ms; uint64_t hold_until,last_up; double win_ms; unsigned win_n,win_d;
                 uint16_t wn[30],wd[30]; int nw; int good_run,need_good; double last_r10,last_r60; } scale3d_ctl_a;
static _Atomic int g_scale3d_level;            // 动态档（0 = 配置值，k = g_s3_levels_a[k−1]） | dynamic level (0 = configured value, k = g_s3_levels_a[k-1])
static _Atomic int g_scale3d_resumed;
static _Atomic int g_3d_quality_mode;          // 0 = 流畅优先（缺省），1 = 画质优先 | 0 = smoothness first (default), 1 = quality first
static void android_scale_configure(void) {
    const char *e=getenv("DM_3D_SCALE"),*d=getenv("DM_3D_SCALE_DYN");
    int dyn=d && strcmp(d,"0") && !getenv("DM_THERMAL_NOFALLBACK");
    /* 2026-10-16 起 Android 默认开世界档：3D 比例 0.75（= iOS 档）；DM_3D_SCALE=1.0 可关（纯原生）。
       档位可在 diagnostics/quality.txt 或 flag/env 里改（app_jni.c）。 */
    float base=0.75f;
#if defined(__ANDROID__)
    /* 分辨率档（自定义页 q_res）决定世界预算：low 1.0 MP / standard 1.25 MP / high 0.55×出图。
       出图像素由 front_game_res 同一套预算算出（含 DM_GAME_RES 覆盖）。派生值自己夹在 0.70..1.0，
       不把超出范围的数交给下面那句 die（那是给显式 DM_3D_SCALE 用的校验）。
       Resolution tier (Custom page q_res) decides the world budget: low 1.0 MP / standard 1.25 MP / high 0.55x output.
       The output pixels come from the same budgets in front_game_res (including the DM_GAME_RES override). The derived value is
       clamped to 0.70..1.0 here instead of being handed to the die below (that check is for an explicit DM_3D_SCALE). */
    { extern unsigned dm_android_quality_out_px(void);
      unsigned out = dm_android_quality_out_px();
      int tier = dm_custom_res_tier();
      /* 世界（3D）像素预算，三档统一用**绝对 MP**（原来高清写成 0.55×出图，与低/标准的绝对值
         不同量纲，调参时容易搞混）。数值按真机实测折算：高清档 0.55×3.20 MP ≈ 1.76 MP。
         World (3D) pixel budget: all three tiers now use absolute megapixels. The high tier used to be
         0.55 x output (a relative amount, inconsistent with the absolute low/standard budgets);
         1.75 MP matches what 0.55 x 3.20 MP produced on the reference device. */
      double wb = tier == 0 ? 0.85e6 : tier == 2 ? 1.75e6 : 1.10e6;   /* 800p 做成真省电档：1.0 → 0.85 MP */   /* 1080p 档 1.25 → 1.10 MP：世界通道约 −12% */
      if (out && wb > 0.0) { double s = sqrt(wb / (double)out);
          if (s < 0.70) { static int once; if (!once++) LOG("【黄】世界预算需 3D 比例 %.3f < 0.70，按 0.70 截断", s); s = 0.70; }
          if (s > 1.0) s = 1.0;
          base = (float)s; } }
#endif
    if (e) { char *end; errno=0; float want=strtof(e,&end);
        if (end==e || *end || errno==ERANGE || !isfinite(want) || want<0.70f || want>1.0f)
            die("ANDROID_3D_SCALE invalid value %s; expected finite 0.70..1.0",e);
        base=want; }
    if (!dm_nvn_frame_count)
        die("ANDROID_3D_SCALE requires graphics presented-frame counter; unavailable in core-only probes");
    { const char *rf=getenv("DM_3D_SCALE_REF");
      if (rf) { unsigned w=0,h=0; char tail=0;
        if (sscanf(rf,"%ux%u%c",&w,&h,&tail)==2 && w>=640 && w<=3840 && h>=360 && h<=2160) { g_android_scale_ref_w=w; g_android_scale_ref_h=h; }
        else die("ANDROID_3D_SCALE_REF invalid value %s; expected WxH",rf); } }
    for (int i=0;i<g_nmod;i++) if (!strcmp(g_mod[i].name,"main")) g_android_scale_main=&g_mod[i];
    android_scale_build_guard(g_android_scale_main);
    g_android_scale_want=base; g_android_scale_dyn=dyn?1:0; g_android_scale_armed=1;
    LOG("ANDROID_3D_SCALE_CONFIG build=%s base=%.3f dyn=%d ref=%ux%u",g_android_scale_94012?"94012":"91738",(double)base,g_android_scale_dyn,g_android_scale_ref_w,g_android_scale_ref_h);
}
static int android_scale_readable(uintptr_t address,size_t length) {
    if (address>UINTPTR_MAX-length) return 0;
    FILE *f=fopen("/proc/self/maps","r"); if (!f) return 0;
    char line[512], permissions[5]; unsigned long long lo,hi; int found=0;
    while (fgets(line,sizeof line,f))
        if (sscanf(line,"%llx-%llx %4s",&lo,&hi,permissions)==3 && permissions[0]=='r' && address>=lo && address+length<=hi) { found=1; break; }
    fclose(f); return found;
}
static void android_scale3d_tick(void *return_address) {
    static _Thread_local int rebuilding;
    pthread_once(&g_android_scale_once,android_scale_configure);
    if (!g_android_scale_armed || rebuilding || dm_nvn_frame_count()<30) return;
    module_t *main=g_android_scale_main;
    uintptr_t obj_off=g_android_scale_94012?0x31e0848:0x31b2528, fn_off=g_android_scale_94012?0x8582f0:0x841540;
    if (!g_android_scale_94012 && return_address!=main->base+0xdf7bb8 && return_address!=main->base+0xdf7c10)
        die("ANDROID_3D_SCALE unexpected notification caller %p",return_address);
    if (g_android_scale_94012 &&
        ((const uint8_t *)return_address<(const uint8_t *)main->base || (const uint8_t *)return_address>=(const uint8_t *)main->base+main->size))
        die("ANDROID_3D_SCALE unexpected caller %p outside main (94012 map)",return_address);
    uintptr_t renderer=*(uintptr_t *)(main->base+obj_off); if (!renderer) return;
    /* readable() 会完整扫一遍 /proc/self/maps（毫秒级）——绝不能每次 tick 都做。缓存校验结果：
     * 指针没变就不再扫（变更时或每 ~5 秒复核一次）。这个 tick 每次系统通知轮询都会进来，
     * 之前的"每次扫两遍"把启用动态比例的会话拖慢近一倍。 | readable() scans all of
     * /proc/self/maps (milliseconds) -- never do it per tick. Cache the validation: re-scan only
     * when the pointer changes (or once every ~5 s as a safety net). This tick runs on every
     * notification poll; scanning twice per call nearly halved throughput in dynamic-scale runs. */
    { static uintptr_t checked; static uint64_t checked_at; uint64_t now_t=now_ns();
      if (renderer!=checked || now_t-checked_at>5ull*1000000000ull) {
        if ((renderer&15) || renderer>UINTPTR_MAX-0xee90 || !android_scale_readable(renderer+0x66a4,4) || !android_scale_readable(renderer+0xee78,24))
            die("ANDROID_3D_SCALE invalid renderer pointer %p",(void *)renderer);
        checked=renderer; checked_at=now_t; } }
    uint32_t *sizes=(uint32_t *)(renderer+0xee78);
    uint32_t ow=sizes[4],oh=sizes[5];
    if (!g_android_scale_94012) { if (ow!=1280 || oh!=720) return; }   // 91738 实验固定按 1280x720 输出 | the 91738 experiment assumed the fixed 1280x720 output
    else if (ow<640 || ow>3840 || oh<360 || oh>2160) return;          // 94012：接受宿主配置的输出分辨率 | 94012: accept the host-configured output resolution
    /* 参考输出（DM_3D_SCALE_REF）：游戏默认停在 720p 输出（iOS 侧是真原生 2556x1179）。只在参考值更大
     * 时把输出抬到参考值——用于把 3D 世界密度对齐 iOS 的"0.75×原生"。校验仍按实际字段做；应用失败
     * 会回退并放弃（不重试）。 | Reference output: the game's default output can sit at 720p (iOS reports
     * its true native). Lift it to the reference only when larger, to match iOS's world density
     * (0.75x native). Verification reads the real fields; a failure reverts and gives up. */
    if (g_android_scale_ref_w>ow || g_android_scale_ref_h>oh) { ow=g_android_scale_ref_w; oh=g_android_scale_ref_h; }
    /* 目标比例：固定档 = 配置值；动态档 = 档位表（0 档 = 配置值，clamp 到不高于它）。
       与当前值一致就什么都不做（固定档因此天然只应用一次）。 | Target scale: fixed mode =
       configured value; dynamic mode = level table (level 0 = configured value, clamped not to
       exceed it). Equal to the current value: nothing to do (which makes fixed mode apply once). */
    float want=g_android_scale_want;
    if (g_android_scale_dyn) { int lv=atomic_load(&g_scale3d_level); if (lv>0) { int k=lv>S3_NLV_A?S3_NLV_A:lv; want=g_s3_levels_a[k-1]; if (want>g_android_scale_want) want=g_android_scale_want; } }
    float *scale=(float *)(renderer+0x66a4),before=*scale;
    if (!isfinite(before) || before<0.50f || before>1.0f)
        die("ANDROID_3D_SCALE unexpected current scale %.9g",(double)before);
    if (fabsf(before-want)<1e-4f) { atomic_store(&g_android_scale_cur,before); return; }
    uint32_t expected_w=(uint32_t)lroundf((float)ow*want),expected_h=(uint32_t)lroundf((float)oh*want);
    LOG("ANDROID_3D_SCALE_BEGIN before=%.3f want=%.3f internal=%ux%u output=%ux%u frame=%llu caller=%#zx",(double)before,(double)want,sizes[0],sizes[1],ow,oh,(unsigned long long)dm_nvn_frame_count(),(size_t)((uint8_t *)return_address-main->base));
    rebuilding=1;
    uint64_t start=now_ns();
    dm_guest_write_prepare(scale,sizeof *scale); *scale=want;
    ((void (*)(uint32_t,uint32_t))(main->base+fn_off))(ow,oh);
    uint64_t elapsed=now_ns()-start;
    LOG("ANDROID_3D_SCALE_END before=%.3f after=%.3f internal=%ux%u duplicate=%ux%u output=%ux%u rebuild_ms=%.3f frame=%llu",(double)before,(double)*scale,sizes[0],sizes[1],sizes[2],sizes[3],sizes[4],sizes[5],(double)elapsed/1e6,(unsigned long long)dm_nvn_frame_count());
    if (*(uintptr_t *)(main->base+obj_off)!=renderer || *scale!=want || sizes[0]!=expected_w || sizes[1]!=expected_h || sizes[2]!=expected_w || sizes[3]!=expected_h || sizes[4]!=ow || sizes[5]!=oh) {
        if (!g_android_scale_94012)
            die("ANDROID_3D_SCALE resize verification failed: expected internal=%ux%u output=%ux%u",expected_w,expected_h,ow,oh);
        /* 94012 映射：校验失败必须能回退到原状——比例写回原值并重跑一次改分辨率函数，恢复了才继续；
           恢复不了（尺寸仍不对）才 die。回退成功后放弃动态调整（固定在回退值），游戏继续。 | 94012 map:
           a verification failure must revert: write the old scale back and run the resize function
           again; only die if the revert itself fails. After a successful revert, dynamic levels are
           given up (the reverted value is kept) and the game continues. */
        LOG("【红】ANDROID_3D_SCALE (94012) resize verification failed; reverting to %.3f (got internal=%ux%u duplicate=%ux%u output=%ux%u)",(double)before,sizes[0],sizes[1],sizes[2],sizes[3],sizes[4],sizes[5]);
        dm_guest_write_prepare(scale,sizeof *scale); *scale=before;
        ((void (*)(uint32_t,uint32_t))(main->base+fn_off))(ow,oh);
        uint32_t revert_w=(uint32_t)lroundf((float)ow*before),revert_h=(uint32_t)lroundf((float)oh*before);
        if (sizes[0]!=revert_w || sizes[1]!=revert_h || sizes[4]!=ow || sizes[5]!=oh)
            die("ANDROID_3D_SCALE (94012) revert failed: internal=%ux%u expected=%ux%u",sizes[0],sizes[1],revert_w,revert_h);
        g_android_scale_dyn=0;   // 放弃动态调整，不再重试 | give dynamic adjustments up, do not retry
        atomic_store(&g_android_scale_cur,before);
        rebuilding=0; return;
    }
    atomic_store(&g_android_scale_cur,want); rebuilding=0;
}
float dm_3d_scale_cur(void) { return atomic_load(&g_android_scale_cur); }
/* ── 动态 3D 比例（Android 侧）──────────────────────────────────────────────────────────────
 * 规则与上面 Apple 分支逐条相同（判据、窗口、阈值、升降停判）：掉帧率 >5% / 10 秒或 >1.5% / 60 秒
 * 降一档；30 秒一帧不掉升一档；刚升 15 秒内又掉，下次升要 90 秒。改阈值时两处一起改。
 * 差别只有两处：①判据输入 = Android 出图点喂来的实际上屏间隔（diagnostics.c → dm_3d_scale_feed），
 * 拿不到系统丢弃标志；②周期取本平台节拍（inc 的 dm_android_nvn_period_ms）。
 * 档位表按本平台定：原生输出下从 0.85 起降，缺省下限 0.70（DM_3D_SCALE_FLOOR 可改）。
 * 默认关：DM_3D_SCALE_DYN=1 才生效（倒档同 Apple：=0 或 DM_THERMAL_NOFALLBACK=1 关）。
 * Rules line-for-line the same as the Apple branch above (criterion, windows, thresholds, hold
 * after a change): dropped-frame rate >5% over 10 s or >1.5% over 60 s lowers one level; 15 clean
 * windows (30 s) raise one level; knocked back within 15 s of a raise means the next raise needs
 * 90 s. Tune both sides together. Only two differences: (1) the input is the actual present
 * interval fed by the Android present point (no system discard flag); (2) the period comes from
 * this platform's pacer (dm_android_nvn_period_ms in the inc). Level table tuned here: start at
 * 0.85 under native output, default floor 0.70 (DM_3D_SCALE_FLOOR overrides). Off by default:
 * DM_3D_SCALE_DYN=1 enables it (=0 or DM_THERMAL_NOFALLBACK=1 disables, like Apple). */
/* 档位表 / 结构体已上移到 android_scale_configure 之前（tick 要用） | level table and struct moved
 * above android_scale_configure (used by the tick). */
static void scale3d_ctl_hold_a(scale3d_ctl_a *c,uint64_t now,uint64_t ns) {   // 停判一段时间：窗与历史清零 | pause judging: clear windows and history
    if (now+ns>c->hold_until) c->hold_until=now+ns;
    c->win_ms=0; c->win_n=c->win_d=0; c->nw=0; c->good_run=0;
}
static double s3_rate_a(const scale3d_ctl_a *c,int k) {   // 最近 k 窗的掉帧率 | dropped-frame rate over the last k windows
    unsigned n=0,d=0; for (int i=0;i<k && i<c->nw;i++) { n+=c->wn[i]; d+=c->wd[i]; } return n?(double)d/n:0;
}
static int scale3d_ctl_step_a(scale3d_ctl_a *c,double iv_ms,uint64_t now) {   // 返回当前级 | returns the current level
    if (now<c->hold_until || iv_ms==0) return c->level;   // 停判中 / 无读数 | judging paused / no reading
    double per=c->period_ms>0?c->period_ms:1000.0/60;
    c->win_n++; if (iv_ms<0 || iv_ms>per*1.5) c->win_d++;   // 超过 1.5 个周期算掉帧 | >1.5 periods counts as a dropped frame
    c->win_ms+=iv_ms>0?iv_ms:per;
    if (c->win_ms<2000) return c->level;
    memmove(c->wn+1,c->wn,sizeof c->wn-sizeof c->wn[0]); memmove(c->wd+1,c->wd,sizeof c->wd-sizeof c->wd[0]);
    c->wn[0]=(uint16_t)c->win_n; c->wd[0]=(uint16_t)c->win_d; if (c->nw<30) c->nw++;
    int clean=c->win_d==0; c->win_ms=0; c->win_n=c->win_d=0;
    c->last_r10=s3_rate_a(c,5); c->last_r60=c->nw>=30?s3_rate_a(c,30):0;
    if (c->level<c->max_level && ((c->nw>=5 && c->last_r10>0.05) || (c->nw>=30 && c->last_r60>0.015))) {
        if (c->last_up && now-c->last_up<15*S3_SEC_A) c->need_good=45;   // 刚升又掉：下次升档要 90 秒 | raised then knocked back: the next raise needs 90 s
        c->level++; scale3d_ctl_hold_a(c,now,3*S3_SEC_A); return c->level;
    }
    if (clean) { if (++c->good_run>=(c->need_good?c->need_good:15) && c->level>0) { c->level--; c->last_up=now; c->need_good=0; scale3d_ctl_hold_a(c,now,3*S3_SEC_A); } }
    else c->good_run=0;
    return c->level;
}
/* 状态原子量已上移（见 android_scale_configure 之前） | state atomics moved above */
void dm_3d_quality_mode_set(int mode) { atomic_store(&g_3d_quality_mode,mode==1?1:0); }
int dm_3d_quality_mode_get(void) { return atomic_load(&g_3d_quality_mode); }
void dm_3d_scale_resumed(void) { atomic_store(&g_scale3d_resumed,1); }   // 前端：刚回前台，回头的慢帧是恢复不是 GPU 吃紧 → 停判 3 秒 | front end: just returned to foreground; the slow frames after a return are recovery, not GPU pressure -> pause 3 s
void dm_3d_scale_feed(double iv_ms) {   // 出图线程每帧调（diagnostics.c：实际上屏间隔 ms；0 = 无读数） | called by the present thread every frame (diagnostics.c: actual present interval in ms; 0 = no reading)
    static scale3d_ctl_a c; static int on=-1; static long inj0=-1,inj1=-1;
    if (on<0) { const char *e=getenv("DM_3D_SCALE_DYN"),*j=getenv("DM_3D_SCALE_DYN_INJECT"); on=!!(e && strcmp(e,"0")) && !getenv("DM_THERMAL_NOFALLBACK");
        if (j) { inj0=atol(j); inj1=strchr(j,'-')?atol(strchr(j,'-')+1):inj0; }
        const char *fe=getenv("DM_3D_SCALE_FLOOR"); float fl=fe?strtof(fe,NULL):S3_FLOOR_DEF_A;
        for (int k=0;k<S3_NLV_A;k++) if (g_s3_levels_a[k]>=fl-1e-4f) c.max_level=k+1;
        LOG("3D 比例控制（Android）：%s，0.85 起降、下限 %.2f（第 %d 档）",on?"开":"关",c.max_level?g_s3_levels_a[c.max_level-1]:0.0f,c.max_level); }
    if (!on) return;
    // 进程内交替测量（owner 方法，消除跨会话热差）：flag-3d-quality 文件在 → 画质优先（钉住
    // 配置值）；撤掉 → 动态档恢复。约每 2 秒查一次盘；切换点记帧号，便于按窗口分段统计。
    // In-process alternating measurement (the owner's method, removes cross-session thermal
    // drift): while flag-3d-quality exists, quality-first pins the configured value; removing it
    // restores the dynamic levels. Polled about every 2 s; the switch point is logged with the
    // frame number so windows can be attributed to a phase.
    { static unsigned abtick; if ((abtick++ & 127u) == 0) {
        char f[1200] = ""; const char *dir = getenv("DM_FRAME_DIR"); int wq = 0;
        if (dir && *dir && snprintf(f, sizeof f, "%s/flag-3d-quality", dir) < (int)sizeof f) wq = access(f, F_OK) == 0;
        if (wq != atomic_load(&g_3d_quality_mode)) { atomic_store(&g_3d_quality_mode, wq);
            LOG("3D 比例 A/B：%s（帧 %llu）", wq ? "画质优先（钉住配置值）" : "动态档恢复", (unsigned long long)(dm_nvn_frame_count ? dm_nvn_frame_count() : 0)); } } }
    if (atomic_load(&g_3d_quality_mode)==1) { if (c.level) { c.level=0; LOG("3D 比例控制：画质优先，回到配置值"); } atomic_store(&g_scale3d_level,0); return; }
    uint64_t now=now_ns();
    if (atomic_exchange(&g_scale3d_resumed,0)) scale3d_ctl_hold_a(&c,now,3*S3_SEC_A);
    { static int seen; if (!seen) { seen=1; scale3d_ctl_hold_a(&c,now,20*S3_SEC_A); } }   // 首帧起停判 20 秒（装载 / 进局慢帧不算） | hold 20 s from the first frame (loading / entry slowness does not count)
    // Android 还没接"局内"信号：菜单 / 读取画面的长帧也会进窗，降档日志带窗数据，回查用 | No in-game
    // signal is wired on Android yet: menu / loading long frames also enter the windows; the level-
    // change log line carries the window rates for review.
    { extern double dm_android_nvn_period_ms(void) __attribute__((weak)); double p=dm_android_nvn_period_ms?dm_android_nvn_period_ms():0;
      if (!(p>0)) p=1000.0/60; if (p!=c.period_ms) { c.period_ms=p; scale3d_ctl_hold_a(&c,now,2*S3_SEC_A); } }
    if (inj0>=0) { long f=dm_nvn_frame_count?(long)dm_nvn_frame_count():0; double per=c.period_ms>0?c.period_ms:1000.0/60; iv_ms=f>=inj0&&f<=inj1?2*per:per; }   // 注入诊断：段内每帧当掉一帧 | injection diagnostic: every frame within the segment counts as dropped
    int was=c.level,lv=scale3d_ctl_step_a(&c,iv_ms,now);
    if (lv!=was) LOG("3D 比例控制（Android）：最近 10 秒掉帧 %.1f%%、60 秒 %.1f%% → %s到第 %d 档（3D 比例 %.2f）%s",c.last_r10*100,c.last_r60*100,lv>was?"降":"升",lv,
                     lv?g_s3_levels_a[lv-1]:-1.0f,c.need_good?"（刚升又掉：下次升档要 90 秒）":"");
    atomic_store(&g_scale3d_level,lv);
}
void dm_3d_scale_hot(int hot) { dm_android_platform_setting("thermal_warning", hot != 0); }
#endif
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "系统通知队列；前端状态变化入队", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
#ifndef __ANDROID__
    scale3d_tick();
    pthread_once(&g_oe_once, oe_script_start);
#else
    android_scale3d_tick(__builtin_extract_return_addr(__builtin_return_address(0)));
#endif
    pthread_mutex_lock(&g_mq_lock);
    int ok = g_mqn > 0;
    if (ok) { *msg = g_mq[g_mqh]; g_mqh = (g_mqh + 1) % MQ; g_mqn--; }
    pthread_mutex_unlock(&g_mq_lock);
    // 分辨率参数位在线程槽里，游戏在取通知的线程上读它们来改出图尺寸（收到 31「性能模式变化」时，main+0xe10ddc 起；30 只更新一个标志）。
    // 读的时机：处理函数每次进来、第一条通知取到之后读一次（0xe10d5c / 0xe10d60），后面循环里再取到的通知用的还是这次读的值。所以每取走一条都把此刻该给的值写进当前线程的槽——
    // 只在取走 31 时写不够：31 排在别的通知（如焦点 15）后面时，参数位在取 15 之后就读过了（iOS 包：前端没切成 16:9，Mac 宿主那局 31 正好排第一）。
    // The resolution parameter bits live in the thread slot, and the game reads them on the thread that fetches notifications to change the render size (on receiving 31 "performance mode changed", from main+0xe10ddc; 30 only updates a flag).
    // When they are read: each time the handler is entered, once after the first notification is fetched (0xe10d5c / 0xe10d60), and notifications fetched later in the loop still use the values from that read. So every time one is taken, the value due at this moment is written into the current thread's slot --
    // writing only when 31 is taken is not enough: when 31 is queued behind another notification (e.g. focus 15), the parameter bits have already been read after fetching 15 (iOS app: the front end did not switch to 16:9, in the Mac host run 31 happened to be queued first).
#ifndef __ANDROID__
    if (ok) for (int i = 0; i < 2; i++) if (g_keyres[i]) pthread_setspecific(g_keyres[i], (void *)dm_game_res(i));
    if (ok && *msg == MSG_PERFMODE) dm_uiscreen_res_reread();
#endif
    if (ok) LOG("oe 通知被游戏取走：%u", *msg);   // 状态变化很少，逐条打不刷屏 | state changes are rare, printing each one does not flood the log
#ifndef __ANDROID__
    if (ok && (*msg == MSG_OPMODE || *msg == MSG_PERFMODE)) dm_ui_gen_bump(*msg == MSG_OPMODE ? DM_UIGEN_OPMODE : DM_UIGEN_PERFMODE);   // 界面代号（dm_cursor.h） | UI generation (dm_cursor.h)
#endif
    return ok;
}
// bool 参数：Switch 调用方不扩展，声明成 unsigned 只看低 8 位
// bool arguments: Switch callers do not extend them, declared as unsigned and only the low 8 bits are looked at
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "恢复通知开关；宿主的恢复以焦点通知 15 表达", void, h_oe_SetResumeNtf, unsigned on) {
#ifdef __ANDROID__
    dm_android_platform_setting("resume_notifications", (on & 0xffu) != 0);
#else
    (void)on;
#endif
}
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "运行模式变化通知开关", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "性能模式变化通知开关", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// nn::settings::LanguageCode = struct { char string[8]; }，8 字节按值返回在 x0
// nn::settings::LanguageCode = struct { char string[8]; }, 8 bytes returned by value in x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "系统语言查询；按 DM_LANG 给值", uint64_t, h_oe_GetLang, void) {
#ifdef __ANDROID__
    return dm_android_platform_language_code();
#else
    const char *e = getenv("DM_LANG");
    char code[8] = {0};
    strncpy(code, e ? e : "zh-Hans", sizeof code - 1);
    uint64_t v; memcpy(&v, code, 8);
    return v;
#endif
}

// nn::util::Uuid 16 字节：本机固定值（只用于本地区分设备，联网已剪）
// nn::util::Uuid 16 bytes: a fixed local value (only used to tell devices apart locally, networking is cut)
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", "伪设备号；宿主固定值", void, h_oe_GetDevId, uint8_t *uuid) {
#ifdef __ANDROID__
    dm_android_platform_device_uuid(uuid);
#else
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    memcpy(uuid, k, 16);
#endif
}
