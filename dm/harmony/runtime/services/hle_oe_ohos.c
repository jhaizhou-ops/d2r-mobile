#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// nn::oe（应用运行环境）：真机上经 am/oe 服务查询主机状态；宿主直接给固定状态。
// oe::Initialize 在 hle_sdkinit.c，不在这里。
// 环境开关：SW_OPMODE=0 掌机 / 1 主机（默认 1，对应主机模式的渲染档）；SW_LANG=语言码（默认 zh-Hans）。
#include "hle_ohos_compat.h"


// 运行模式是运行时状态（前端可改：iOS 过热降级切掌机）：唯一来源在此，启动时从 SW_OPMODE 取一次，之后不再读环境变量
#include <stdatomic.h>
static _Atomic int g_opmode = -1;
int sw_opmode(void) {
    int m = g_opmode;
    if (m < 0) { const char *e = getenv("SW_OPMODE"); int v = e ? atoi(e) : 1; int exp = -1; atomic_compare_exchange_strong(&g_opmode, &exp, v); m = g_opmode; }
    return m;
}
// ---------- 系统通知队列（AppletMessage）：前端状态变化 → 游戏每帧 TryPopNotificationMessage 取 ----------
// 消息号（游戏 main 0xe10d30 实际处理的）：15 焦点变化、30 运行模式变化、31 性能模式变化
enum { MSG_FOCUS = 15, MSG_OPMODE = 30, MSG_PERFMODE = 31 };
#define MQ 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_mq[MQ]; static int g_mqh, g_mqn;
static _Atomic int g_ntf_opmode, g_ntf_perfmode;          // 游戏用 Set*NotificationEnabled 打开后才推
static void mq_push(uint32_t m) {
    pthread_mutex_lock(&g_mq_lock);
    // 15/30/31 是「查询当前状态」的失效通知，不是携带历史值的事件。
    // 同类型尚未取走时保留首次排位：D2R 消费循环只读最新状态，重复通知的写回相同，尺寸相同不再 resize。
    // 只合并这三种状态通知；未来的逐次事件（如 Resume/Exit）不能套这个规则。
    // 合法生产者只发这三种，因此待取最多三项，任意多次失焦/恢复不再撞 32 槽上限；弹出后新变化照常入队。
    if (m == MSG_FOCUS || m == MSG_OPMODE || m == MSG_PERFMODE)
        for (int i = 0; i < g_mqn; i++)
            if (g_mq[(g_mqh + i) % MQ] == m) { pthread_mutex_unlock(&g_mq_lock); return; }
    if (g_mqn == MQ) die("oe 通知队列溢出（游戏没在取？）");
    g_mq[(g_mqh + g_mqn++) % MQ] = m;
    pthread_mutex_unlock(&g_mq_lock);
}
void sw_oe_res_changed(void) { if (getenv("SW_TEST_OE_FOCUS_FIRST")) mq_push(MSG_FOCUS);   /* 测试：让 31 排在一条焦点通知后面（iOS 包启动时的顺序） */
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE); else LOG("【红】分辨率要切，但游戏没打开性能模式变化通知"); }   // 出图分辨率该变了（sw_uiscreen.c）：让游戏重读参数位
void sw_set_opmode(int m) {   // 前端调（过热降级等）：改状态并按游戏打开的开关推送 30 / 31
    int old = sw_opmode(); if (old == m) return;
    g_opmode = m;
    LOG("oe 运行模式 %d → %d", old, m);
    if (g_ntf_opmode) mq_push(MSG_OPMODE);
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE);
}
// FocusState：1 InFocus、2 OutOfFocus、3 Background。前端调 sw_focus_set（iOS 生命周期 / Mac 窗口失焦）
static _Atomic int g_focus = 1;
void sw_focus_set(int st) {
    if (st < 1 || st > 3) die("sw_focus_set 非法状态 %d", st);
    if (atomic_exchange(&g_focus, st) == st) return;
    LOG("oe 焦点状态 → %d", st);
    mq_push(MSG_FOCUS);
}
static int opmode(void) { return sw_opmode(); }

// 截图 / 录像开关：宿主无系统截图服务，空操作
HLE_FORCE("_ZN2nn2oe15EnableRecordingEv", "录像开关；宿主无系统录像", void, h_oe_EnableRec, void) {}
HLE_FORCE("_ZN2nn2oe16DisableRecordingEv", "录像开关；宿主无系统录像", void, h_oe_DisableRec, void) {}
HLE_FORCE("_ZN2nn2oe16EnableScreenShotEv", "截图开关；宿主无系统截图", void, h_oe_EnableShot, void) {}
HLE_FORCE("_ZN2nn2oe17DisableScreenShotEv", "截图开关；宿主无系统截图", void, h_oe_DisableShot, void) {}

// OperationMode：0 Handheld、1 Console；PerformanceMode：0 Normal、1 Boost（与运行模式一一对应）
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "运行模式查询；按 SW_OPMODE 给固定值", int, h_oe_GetOpMode, void) { return opmode(); }
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "性能模式查询；与运行模式一致", int, h_oe_GetPerfMode, void) { return opmode(); }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "设 CPU/GPU 频率档；宿主不调频", void, h_oe_SetPerfCfg, int mode, int cfg) {
    LOG("oe::SetPerformanceConfiguration mode=%d cfg=%#x（忽略）", mode, cfg);
}

// 脚本化状态变化（无窗口验证生命周期 / 过热降级）：SW_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30"（第 N 秒）
// 时刻从游戏第一次取通知算起；「@ingame+N」= 读取画面第 K 次撤下（K = 前面 @ingame 项数 + 1，首个即进局）后 N 秒（按事件，不受负载影响）。与前端调用并存。
#include <unistd.h>
static _Atomic int g_ls_unloads;   // 读取画面撤下次数（diag.c 的 UIMSG 处理调 sw_note_loadscreen_unload）
void sw_note_loadscreen_unload(void) { atomic_fetch_add(&g_ls_unloads, 1); }
static void *oe_script(void *arg) {
    char *buf = arg; uint64_t t0 = now_ns();
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char key[16]; int v; double at;
        if (sscanf(tok, "%15[a-z]=%d@ingame+%lf", key, &v, &at) == 3) {
            static int need;
            need++; while (atomic_load(&g_ls_unloads) < need) usleep(20000);
            usleep((useconds_t)(at * 1e6));
        } else {
            if (sscanf(tok, "%15[a-z]=%d@%lf", key, &v, &at) != 3) die("SW_OE_SCRIPT 项「%s」格式应为 键=值@秒 或 键=值@ingame+秒", tok);
            uint64_t due = t0 + (uint64_t)(at * 1e9), now = now_ns();
            if (due > now) usleep((useconds_t)((due - now) / 1000));
        }
        if (!strcmp(key, "focus")) sw_focus_set(v);
        else if (!strcmp(key, "opmode")) sw_set_opmode(v);
        else die("SW_OE_SCRIPT 未知键「%s」（focus / opmode）", key);
    }
    return NULL;
}
static void oe_script_start(void) {
    const char *e = getenv("SW_OE_SCRIPT"); if (!e) return;
    pthread_t th; if (pthread_create(&th, NULL, oe_script, strdup(e))) die("SW_OE_SCRIPT 线程");
    pthread_detach(th);
}
static pthread_once_t g_oe_once = PTHREAD_ONCE_INIT;

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "前后台状态；由前端 sw_focus_set 驱动", int, h_oe_GetFocus, void) { return g_focus; }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "前后台切换策略；游戏设 1（收焦点通知），宿主按通知语义实现", void, h_oe_SetFocusMode, int m) { LOG("oe::SetFocusHandlingMode %d", m); }

// 通知：返回 bool（w0），宿主没有系统通知

// ---------- 原生屏幕比例输出（Claude，移植 iOS 改写器 PARAMS / thread.c 参数位）----------
// 游戏 main 里造分辨率常数的 4 处、宽高比上限 3 处已在 harmony_images.S 改成 adrp+ldr 读下面四个变量（patch_widescreen.py）。
// 前端在加载运行库前 setenv("XBL_SCREEN","宽x高")（横屏像素）；没给 = 原版 1600x900 / 1280x720 / 19:9。
static float xbl_scale3d = 0.75f;   // 3D 渲染比例（原生分辨率输出时按 3D 高约 700 定）
static float xbl_scale3d_quality = 0.75f;   // 画质档（按 3D 高约 700 定的原值）
// 流畅 / 画质两档（前端按钮）：0 = 画质档，其余 = 指定比例（流畅档 0.45）。游戏主线程取通知处生效，可随时切换。
static _Atomic float xbl_scale3d_req = -1.0f;
__attribute__((visibility("default"))) void xblr_set_scale3d(float v) {
    if (v <= 0) v = xbl_scale3d_quality; if (v < 0.35f) v = 0.35f; if (v > 0.75f) v = 0.75f; atomic_store(&xbl_scale3d_req, v);
}
// 技能键图标（同 iOS sw_touch_game_bridge.c 的两张表）：按键→槽映射 main+0x2d13cbc（6 组 × {逻辑键, 普通槽, 替代槽}），
// 技能绑定 main+0x2d5f0c0（32 项 × 28 字节，每项首 4 字节 = 原版技能号；0 / 16 两组）。只读静态数据，前端定时取、自己解析。
__attribute__((visibility("default"))) int xblr_skill_tables(uint8_t *mapping72, uint32_t *ids32) {
    static module_t *m; if (!m) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i]; if (!m) return 0; }
    if (!mapping72 || !ids32 || m->size < 0x2d5f0c0 + 32 * 28) return 0;
    memcpy(mapping72, m->base + 0x2d13cbc, 72);
    for (int i = 0; i < 32; i++) memcpy(&ids32[i], m->base + 0x2d5f0c0 + i * 28, 4);
    return 1;
}
__attribute__((visibility("hidden"),aligned(8))) uint64_t xbl_kres1 = (900ull << 32) | 1600;
__attribute__((visibility("hidden"),aligned(8))) uint64_t xbl_kres0 = (720ull << 32) | 1280;
__attribute__((visibility("hidden"),aligned(4))) uint32_t xbl_kaspect = 0x40071c72;
__attribute__((visibility("hidden"),aligned(4))) uint32_t xbl_kaspect_ui = 0x40071c72;
__attribute__((constructor)) static void xbl_screen_init(void) {
    const char *e = getenv("XBL_SCREEN"); unsigned sw = 0, sh = 0;
    if (!e || sscanf(e, "%ux%u", &sw, &sh) != 2 || !sw || !sh) return;
    if (sh > sw) { unsigned t = sw; sw = sh; sh = t; }
    double aspect = (double)sw / sh;
    // iOS 主机档 = 屏幕原生像素（界面原生清晰），3D 由游戏自带渲染比例另缩（xbl_scale3d）。XBL_OUTPUT=900 退回 900 高。
    const char *oh = getenv("XBL_OUTPUT"); unsigned h1 = oh ? (unsigned)atoi(oh) : sh; if (h1 < 720 || h1 > sh) h1 = sh;
    uint64_t w1 = (uint64_t)(h1 * aspect / 8 + 0.5) * 8, w0 = (uint64_t)(720 * aspect / 8 + 0.5) * 8;
    xbl_kres1 = ((uint64_t)h1 << 32) | w1; xbl_kres0 = (720ull << 32) | w0;
    { float sc = 700.0f / (float)h1; if (sc < 0.5f) sc = 0.5f; if (sc > 0.75f) sc = 0.75f; xbl_scale3d = xbl_scale3d_quality = sc; }
    { const char *m = getenv("XBL_SCALE3D"); float v = m ? strtof(m, NULL) : 0; if (v > 0) { if (v < 0.35f) v = 0.35f; if (v > 0.75f) v = 0.75f; xbl_scale3d = v; } }
    float a = 19.0f / 9.0f; int wide = 0; if ((float)w1 / h1 > a) { a = (float)w1 / h1; wide = 1; }
    float ui = wide ? a + 0.15f : a; memcpy(&xbl_kaspect, &a, 4); memcpy(&xbl_kaspect_ui, &ui, 4);
    LOG("原生比例输出：屏幕 %ux%u → 主机 %llux%u、掌机 %llux720、宽高比上限 %.3f（界面 %.3f）、3D 比例 %.2f", sw, sh, (unsigned long long)w1, h1, (unsigned long long)w0, a, ui, xbl_scale3d);
}
// ---------- 3D 内部渲染比例（Claude 移植 iOS hle_oe.c scale3d_tick，固定 0.75，无动态降档）----------
// 渲染器 [main+0x31e0848]+0x66a4 = 3D 比例；main+0x8582f0(输出宽,高) 按比例重建 3D 目标，界面仍按输出尺寸、末端游戏自带放大。
// 只能在游戏主线程取系统通知处调（同 iOS）。前 300 次取通知不动（装载期）。
#define XBL_SCALE3D xbl_scale3d
static void scale3d_tick(void) {
    static int done, calls; static module_t *main_m;
    if (done) {   // 已生效：前端切档时重建 3D 目标
        float want = atomic_exchange(&xbl_scale3d_req, -1.0f); if (want < 0 || want == xbl_scale3d) return;
        uintptr_t renderer = *(uintptr_t *)(main_m->base + 0x31e0848); if (!renderer) return;
        uint32_t ow = *(uint32_t *)(renderer + 0xee88), oh = *(uint32_t *)(renderer + 0xee8c); if (!ow || !oh) return;
        float *scale = (float *)(renderer + 0x66a4); sw_guest_write_prepare(scale, sizeof *scale); *scale = want; xbl_scale3d = want;
        ((void (*)(uint32_t, uint32_t))(main_m->base + 0x8582f0))(ow, oh);
        LOG("3D 渲染比例切换 %.2f：3D 目标 %ux%u", want, *(uint32_t *)(renderer + 0xee78), *(uint32_t *)(renderer + 0xee7c)); return;
    }
    if (++calls < 300) return;
    if (!main_m) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) main_m = &g_mod[i]; if (!main_m) return; }
    uintptr_t renderer = *(uintptr_t *)(main_m->base + 0x31e0848); if (!renderer) return;
    uint32_t ow = *(uint32_t *)(renderer + 0xee88), oh = *(uint32_t *)(renderer + 0xee8c); if (!ow || !oh) return;
    float *scale = (float *)(renderer + 0x66a4); sw_guest_write_prepare(scale, sizeof *scale); *scale = XBL_SCALE3D;
    ((void (*)(uint32_t, uint32_t))(main_m->base + 0x8582f0))(ow, oh);
    done = 1;
    LOG("3D 渲染比例 %.2f：3D 目标 %ux%u，输出 %ux%u", XBL_SCALE3D, *(uint32_t *)(renderer + 0xee78), *(uint32_t *)(renderer + 0xee7c), ow, oh);
}
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "系统通知队列；前端状态变化入队", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
    scale3d_tick();
    { static unsigned n; static uint64_t e0, b0, t0; if (++n % 600 == 0) { void sw_audio_underruns(uint64_t *, uint64_t *); uint64_t e, b, t = now_ns(); sw_audio_underruns(&e, &b);
        if (t0) LOG("音频欠载：%.1f 秒内 %llu 次、%.1f ms 静音", (t - t0) / 1e9, (unsigned long long)(e - e0), (b - b0) / (48.0 * 4));
        { void sw_audio_stats(uint64_t*,uint64_t*,uint64_t*,uint64_t*,uint64_t*,uint64_t*,uint64_t*); static uint64_t pb, pc, pl, pn, pq, pqn; uint64_t ab, ac, l, ln, lm, q, qn; sw_audio_stats(&ab, &ac, &l, &ln, &lm, &q, &qn);
          if (t0) LOG("音频产出：%.0f%% 实时（%llu 块，块均 %.1f ms），释放→追加 均 %.2f ms 最长 %.1f ms，追加时队列深度均 %.1f，声卡回调最大 %.1f ms", (ab - pb) / (48.0 * 4) / ((t - t0) / 1e6) * 100, (unsigned long long)(ac - pc), ac > pc ? (ab - pb) / (48.0 * 4) / (ac - pc) : 0.0, ln > pn ? (l - pl) / 1e6 / (ln - pn) : 0.0, lm / 1e6, qn > pqn ? (double)(q - pq) / (qn - pqn) : 0.0, ({ uint64_t sw_audio_cb_max(void); sw_audio_cb_max() / (48.0 * 12); }));
          pb = ab; pc = ac; pl = l; pn = ln; pq = q; pqn = qn; }
        { extern _Atomic uint64_t sw_opus_ns, sw_opus_calls; static uint64_t on0, oc0; uint64_t on = atomic_load(&sw_opus_ns), oc = atomic_load(&sw_opus_calls);
          if (t0) LOG("Opus 软件解码：%.1f 秒内 %llu 次、共 %.0f ms（占一个核 %.0f%%）", (t - t0) / 1e9, (unsigned long long)(oc - oc0), (on - on0) / 1e6, (on - on0) / (double)(t - t0) * 100); on0 = on; oc0 = oc; }
        e0 = e; b0 = b; t0 = t; } }
    pthread_once(&g_oe_once, oe_script_start);
    pthread_mutex_lock(&g_mq_lock);
    int ok = g_mqn > 0;
    if (ok) { *msg = g_mq[g_mqh]; g_mqh = (g_mqh + 1) % MQ; g_mqn--; }
    pthread_mutex_unlock(&g_mq_lock);
    // 分辨率参数位在线程槽里，游戏在取通知的线程上读它们来改出图尺寸（收到 31「性能模式变化」时，main+0xe10ddc 起；30 只更新一个标志）。
    // 读的时机：处理函数每次进来、第一条通知取到之后读一次（0xe10d5c / 0xe10d60），后面循环里再取到的通知用的还是这次读的值。所以每取走一条都把此刻该给的值写进当前线程的槽——
    // 只在取走 31 时写不够：31 排在别的通知（如焦点 15）后面时，参数位在取 15 之后就读过了（10-05 iOS 包：前端没切成 16:9，Mac 宿主那局 31 正好排第一）。
    if (ok) LOG("oe 通知被游戏取走：%u", *msg);   // 状态变化很少，逐条打不刷屏
    return ok;
}
// bool 参数：Switch 调用方不扩展，声明成 unsigned 只看低 8 位
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "恢复通知开关；宿主的恢复以焦点通知 15 表达", void, h_oe_SetResumeNtf, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "运行模式变化通知开关", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "性能模式变化通知开关", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// nn::settings::LanguageCode = struct { char string[8]; }，8 字节按值返回在 x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "系统语言查询；按 SW_LANG 给值", uint64_t, h_oe_GetLang, void) {
    const char *e = getenv("SW_LANG");
    char code[8] = {0};
    strncpy(code, e ? e : "zh-Hans", sizeof code - 1);
    uint64_t v; memcpy(&v, code, 8);
    return v;
}

// nn::util::Uuid 16 字节：本机固定值（只用于本地区分设备，联网已剪）
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", "伪设备号；宿主固定值", void, h_oe_GetDevId, uint8_t *uuid) {
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    memcpy(uuid, k, 16);
}
