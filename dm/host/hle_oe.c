// nn::oe（应用运行环境）：真机上经 am/oe 服务查询主机状态；宿主直接给固定状态。
// oe::Initialize 在 hle_sdkinit.c，不在这里。
// 环境开关：DM_OPMODE=0 掌机 / 1 主机（默认 1，对应主机模式的渲染档）；DM_LANG=语言码（默认 zh-Hans）。
// nn::oe (application operating environment): on the real device the console state is queried through the am/oe services; the host gives fixed state directly.
// oe::Initialize is in hle_sdkinit.c, not here.
// Environment switches: DM_OPMODE=0 handheld / 1 console (default 1, corresponding to the console-mode render profile); DM_LANG=language code (default zh-Hans).
#include "dm_cursor.h"
#include "hle.h"
#include "dm_uiscreen.h"
#include <TargetConditionals.h>

// 运行模式是运行时状态（前端可改：iOS 过热降级切掌机）：唯一来源在此，启动时从 DM_OPMODE 取一次，之后不再读环境变量
// The operation mode is runtime state (the front end can change it: iOS overheat downgrade switches to handheld): the single source is here, taken once from DM_OPMODE at startup, the environment variable is not read again afterwards
#include <stdatomic.h>
static _Atomic int g_opmode = -1;
int dm_opmode(void) {
    int m = g_opmode;
    if (m < 0) { const char *e = getenv("DM_OPMODE"); int v = e ? atoi(e) : 1; int exp = -1; atomic_compare_exchange_strong(&g_opmode, &exp, v); m = g_opmode; }
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
void dm_oe_ui_opmode(int m) { g_ui_opmode = m < 0 ? -1 : m; atomic_store(&g_ui_armed, 1); if (g_ntf_opmode) mq_push(MSG_OPMODE); }
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
HLE_FORCE("_ZN2nn2oe15EnableRecordingEv", "录像开关；宿主无系统录像", void, h_oe_EnableRec, void) {}
HLE_FORCE("_ZN2nn2oe16DisableRecordingEv", "录像开关；宿主无系统录像", void, h_oe_DisableRec, void) {}
HLE_FORCE("_ZN2nn2oe16EnableScreenShotEv", "截图开关；宿主无系统截图", void, h_oe_EnableShot, void) {}
HLE_FORCE("_ZN2nn2oe17DisableScreenShotEv", "截图开关；宿主无系统截图", void, h_oe_DisableShot, void) {}

// OperationMode：0 Handheld、1 Console；PerformanceMode：0 Normal、1 Boost（与运行模式一一对应）
// OperationMode: 0 Handheld, 1 Console; PerformanceMode: 0 Normal, 1 Boost (one-to-one with the operation mode)
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "运行模式查询；按 DM_OPMODE 给固定值", int, h_oe_GetOpMode, void) { int u = g_ui_opmode; if (atomic_exchange(&g_ui_armed, 0)) atomic_fetch_add(&g_ui_reads, 1); return u >= 0 ? u : opmode(); }
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "性能模式查询；与运行模式一致", int, h_oe_GetPerfMode, void) { return opmode(); }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "设 CPU/GPU 频率档；宿主不调频", void, h_oe_SetPerfCfg, int mode, int cfg) {
    LOG("oe::SetPerformanceConfiguration mode=%d cfg=%#x（忽略）", mode, cfg);
}

// 脚本化状态变化（无窗口验证生命周期 / 过热降级）：DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30"（第 N 秒）
// 时刻从游戏第一次取通知算起；「@ingame+N」= 读取画面第 K 次撤下（K = 前面 @ingame 项数 + 1，首个即进局）后 N 秒（按事件，不受负载影响）。与前端调用并存。
// Scripted state changes (verify lifecycle / overheat downgrade without a window): DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30" (second N)
// Time counts from the game's first notification fetch; "@ingame+N" = N seconds after the loading screen is taken down for the K-th time (K = number of preceding @ingame items + 1, the first one is run entry) (event-based, not affected by load). Coexists with front end calls.
#include <unistd.h>
static _Atomic int g_ls_unloads;   // 读取画面撤下次数（diag.c 的 UIMSG 处理调 dm_note_loadscreen_unload） | number of times the loading screen was taken down (the UIMSG handler in diag.c calls dm_note_loadscreen_unload)
void dm_note_loadscreen_unload(void) { atomic_fetch_add(&g_ls_unloads, 1); }
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

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "前后台状态；由前端 dm_focus_set 驱动", int, h_oe_GetFocus, void) { return g_focus; }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "前后台切换策略；游戏设 1（收焦点通知），宿主按通知语义实现", void, h_oe_SetFocusMode, int m) { LOG("oe::SetFocusHandlingMode %d", m); }

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
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "系统通知队列；前端状态变化入队", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
    scale3d_tick();
    pthread_once(&g_oe_once, oe_script_start);
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
    if (ok) for (int i = 0; i < 2; i++) if (g_keyres[i]) pthread_setspecific(g_keyres[i], (void *)dm_game_res(i));
    if (ok && *msg == MSG_PERFMODE) dm_uiscreen_res_reread();
    if (ok) LOG("oe 通知被游戏取走：%u", *msg);   // 状态变化很少，逐条打不刷屏 | state changes are rare, printing each one does not flood the log
    if (ok && (*msg == MSG_OPMODE || *msg == MSG_PERFMODE)) dm_ui_gen_bump(*msg == MSG_OPMODE ? DM_UIGEN_OPMODE : DM_UIGEN_PERFMODE);   // 界面代号（dm_cursor.h） | UI generation (dm_cursor.h)
    return ok;
}
// bool 参数：Switch 调用方不扩展，声明成 unsigned 只看低 8 位
// bool arguments: Switch callers do not extend them, declared as unsigned and only the low 8 bits are looked at
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "恢复通知开关；宿主的恢复以焦点通知 15 表达", void, h_oe_SetResumeNtf, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "运行模式变化通知开关", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "性能模式变化通知开关", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// nn::settings::LanguageCode = struct { char string[8]; }，8 字节按值返回在 x0
// nn::settings::LanguageCode = struct { char string[8]; }, 8 bytes returned by value in x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "系统语言查询；按 DM_LANG 给值", uint64_t, h_oe_GetLang, void) {
    const char *e = getenv("DM_LANG");
    char code[8] = {0};
#if TARGET_OS_IPHONE
    const char *fallback = "zh-Hans";
#else
    const char *fallback = "es";
#endif
    strncpy(code, e && *e ? e : fallback, sizeof code - 1);
    uint64_t v; memcpy(&v, code, 8);
    return v;
}

// nn::util::Uuid 16 字节：本机固定值（只用于本地区分设备，联网已剪）
// nn::util::Uuid 16 bytes: a fixed local value (only used to tell devices apart locally, networking is cut)
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", "伪设备号；宿主固定值", void, h_oe_GetDevId, uint8_t *uuid) {
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    memcpy(uuid, k, 16);
}
