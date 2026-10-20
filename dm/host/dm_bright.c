// 3D 画面加亮的动态曝光（雪山山顶正午加亮后过曝，暗处才要加亮）。图形层 nvn_mtl.m bright_lut 每帧调 dm_bright_k() 取曝光 k（1–4）。
// 规则（按序）：
//   ① 局外（dm_uiscreen_ingame = 0，且不在读取画面：前端各页）→ k = 1；读取画面期间 k 保持不动（进出局、走进洞口都不先掉回 1）；
//   ② 迷宫类关卡（Levels.txt DrlgType = 1：洞穴、墓穴、地穴、下水道……）→ k = 4；
//   ③ 原版画面不随昼夜的 biome（hd/env/vis 白天版与夜晚版内容相同），且原版没给固定环境亮度 → k = 4；
//   ④ 其余按原版自己算的环境亮度 I（0–255）：k = 4^((255 − I) / 191)，限在 1–4（正午 I = 255 → 1，午夜 I = 64 → 4）。
// 「原版给固定环境亮度」照原版 0x55a140 的分支判：第四幕（幕号 3，按表 128 / 64 / 56 / 48，其余 16）、区域 120（亚瑞特之巅）/ 137 钉 200、环境 +0x34 置位（收敛到 32）。
// 原版环境结构：客户端当前幕 = [main + 0x2d20bd0]，环境 = [幕 + 8]；+0x8 float 累加时间、+0xc int 环境亮度、+0x2c int 流速（太阳角 = 时间 ÷ 流速，0–360°）、+0x34 int 标志。
// 关卡表由 dm/tools/gen_bright_levels.py 从原版数据生成（dm_bright_levels.inc）。k 在对数空间按时间常数 0.33 秒平滑（约 1 秒到位），切地图 / 进出局不跳变。
// 测试开关：DM_BRIGHT_K=<k> 固定曝光（1 = 原版亮度，画面闸门用）；DM_TEST_DAYTIME=<角度>@<拍>：第 <拍> 拍把服务端各幕与客户端的环境时间写成该太阳角
// （看正午 / 黄昏 / 午夜；要挂拍入口观测门，带 DM_C2S_LOG 等任一）。只写客户端那份会被服务端同步回去。
// Dynamic exposure for the 3D scene brightening (the snowy mountain summit at noon is overexposed after brightening; only dark places need brightening). The graphics layer's nvn_mtl.m bright_lut calls dm_bright_k() every frame to get the exposure k (1–4).
// Rules (in order):
//   (1) Outside a run (dm_uiscreen_ingame = 0, and not on the loading screen: the front-end pages) → k = 1; during the loading screen k stays unchanged (entering/leaving a run and walking into a cave entrance do not drop back to 1 first);
//   (2) Maze-type levels (Levels.txt DrlgType = 1: caves, tombs, crypts, sewers...) → k = 4;
//   (3) Biomes whose original visuals do not follow day and night (the day and night versions in hd/env/vis have identical content), where the original game gives no fixed ambient brightness → k = 4;
//   (4) The rest follow the ambient brightness I (0–255) the original game computes itself: k = 4^((255 − I) / 191), clamped to 1–4 (noon I = 255 → 1, midnight I = 64 → 4).
// "The original game gives a fixed ambient brightness" is judged by the branches of the original game's 0x55a140: Act 4 (act index 3, 128 / 64 / 56 / 48 per the table, 16 for the rest), areas 120 (Arreat Summit) / 137 pinned at 200, environment +0x34 set (converges to 32).
// Original-game environment structure: client current act = [main + 0x2d20bd0], environment = [act + 8]; +0x8 float accumulated time, +0xc int ambient brightness, +0x2c int flow rate (sun angle = time ÷ flow rate, 0–360°), +0x34 int flags.
// The level table is generated from the original game's data by dm/tools/gen_bright_levels.py (dm_bright_levels.inc). k is smoothed in log space with a time constant of 0.33 seconds (settles in about 1 second), with no jump on map changes / entering or leaving a run.
// Test switches: DM_BRIGHT_K=<k> fixed exposure (1 = original brightness, used by the screenshot gate); DM_TEST_DAYTIME=<angle>@<tick>: at tick <tick>, write the environment time of every act on the server and of the client to that sun angle
// (to view noon / dusk / midnight; the tick entry observation gate must be attached, with any one of DM_C2S_LOG etc.). Writing only the client's copy gets synced back by the server.
#include "dm.h"
#include "hle.h"
#include "dm_hud.h"
#include "dm_uiscreen.h"
#include <math.h>
#ifdef __ANDROID__
#include <string.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <mach/mach.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

static const struct { uint16_t lv; uint8_t act, drlg; int8_t varies; } k_lv[] = {
#include "dm_bright_levels.inc"
};

#ifdef __ANDROID__
/* 用 process_vm_readv 读自己进程：内核校验地址，坏指针只返回错误。直接解引用会 SEGV——
 * 首测踩过：act=0 时读 act+8（地址 0x8）当场崩。 | Read our own process via process_vm_readv:
 * the kernel validates the address, so a bad pointer merely returns an error. Direct dereferencing
 * crashes -- the first test read act+8 (address 0x8) with act = 0 and died. */
static int rd(uint64_t a, void *buf, size_t n) {
    if (!a || n > 128) return 0;
    struct iovec local = {buf, n}, remote = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}
/* 安卓早期缺做局内 / 区域 / 读取画面判定的宿主设施，所以这里给弱定义。现在三样在安卓侧都有强定义了：区域来自
 * dm_hud.c + hud_read_android.c（hud_tick 每拍发布），局内来自 dm_uiscreen.c，读取画面来自 dm_cursor.c
 * （dm_ui_loadscreen_up）+ ui_state_android.c。弱定义只剩图形单测 / 不链宿主的构建用。
 * Android used to lack the host facilities for the in-run / area / loading-screen decisions, hence these weak definitions. All three
 * now have strong definitions on Android: the area comes from dm_hud.c + hud_read_android.c (hud_tick publishes it every tick), in-run
 * from dm_uiscreen.c, and the loading screen from dm_cursor.c (dm_ui_loadscreen_up) + ui_state_android.c. The weak definitions remain
 * only for graphics unit tests / builds that do not link the host. */
__attribute__((weak)) int dm_uiscreen_ingame(void) { return 1; }
__attribute__((weak)) void dm_hud_get(dm_hud_info *out) { memset(out, 0, sizeof *out); out->area = -1; }
__attribute__((weak)) int dm_loadscreen_up(void) { return 0; }
#else
static int rd(uint64_t a, void *buf, size_t n) { vm_size_t got = 0; return a && !vm_read_overwrite(mach_task_self(), a, n, (vm_address_t)buf, &got) && got == n; }
#endif
static uint64_t main_base(void) { static uint64_t b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; return b; }
static uint64_t env_ptr(void) { uint64_t act = 0, env = 0, mb = main_base(); return mb && rd(mb + 0x2d20bd0, &act, 8) && rd(act + 8, &env, 8) ? env : 0; }

// 目标 k 的对数（log k）；*why 写规则号，*I / *ang 写读到的原版值（读不到为 −1）
// Logarithm of the target k (log k); *why receives the rule number, *I / *ang receive the original values read (-1 if unreadable)
static double target_logk(int *why, int *area, int *I, double *ang) {
    *I = -1; *ang = -1; *area = -1;
    if (!dm_uiscreen_ingame()) { *why = 1; return 0; }
    dm_hud_info h; dm_hud_get(&h); *area = h.valid ? h.area : -1;
    int act = -1, drlg = -1, varies = -1;
    for (unsigned i = 0; i < sizeof k_lv / sizeof *k_lv; i++) if (k_lv[i].lv == *area) { act = k_lv[i].act; drlg = k_lv[i].drlg; varies = k_lv[i].varies; break; }
    uint64_t env = env_ptr(); int32_t iv = -1, flag = 0, rate = 0; float t = 0;
    if (env && rd(env + 0xc, &iv, 4) && rd(env + 0x34, &flag, 4) && rd(env + 0x2c, &rate, 4) && rd(env + 0x8, &t, 4)) { *I = iv; if (rate > 0) *ang = t / rate; }
    if (drlg == 1) { *why = 2; return log(4.0); }
    int pinned = act == 3 || *area == 120 || *area == 137 || flag != 0;
    if (varies == 0 && !pinned) { *why = 3; return log(4.0); }
    *why = 4;
    if (*I < 0) return log(4.0);   // 环境读不到（刚进局几帧）：按加满 | the environment cannot be read (the first few frames of a run): treat as full boost
    double e = (255.0 - (*I > 255 ? 255 : *I)) / 191.0; if (e < 0) e = 0; if (e > 1) e = 1;
    return e * log(4.0);
}

float dm_bright_k(void) {   // 图形层每帧调（单线程：出图 / 回放线程） | called every frame by the graphics layer (single thread: the present / replay thread)
    static int fixed = -2; static float kf;
    if (fixed == -2) { const char *e = getenv("DM_BRIGHT_K"); fixed = e != NULL; if (e) { kf = (float)atof(e); LOG("DM_BRIGHT_K=%g：3D 画面曝光由测试开关固定", kf); } }
    if (fixed) return kf;
    static double logk = 0; static uint64_t last; static int lwhy = -1, larea = -2; static uint64_t llog; static double ltgt = -1;
    uint64_t t = now_ns();
    { int dm_loadscreen_up(void); if (dm_loadscreen_up()) { last = t; static int tr = -1; static unsigned n; if (tr < 0) tr = getenv("DM_BRIGHT_TRACE") != NULL; if (tr && n++ % 6 == 0) LOG("3D 画面加亮：k %.3f（读取画面中，保持）", exp(logk)); return (float)exp(logk); } }   // 读取画面（进出局、走进洞口）：保持当前 k，撤掉后从原值平滑走到新目标，不先掉回 1 | loading screen (entering/leaving a run, walking into a cave entrance): keep the current k, and after it is taken down move smoothly from the old value to the new target without dropping back to 1 first
    int why, area, I; double ang; double tgt = target_logk(&why, &area, &I, &ang);
    double dt = last ? (t - last) / 1e9 : 0; last = t; if (dt > 0.034) dt = 0.034;   // 读取画面期间不调用（不派发调色表计算）：每次最多按一帧（30 帧档的帧长）推进，撤屏后从原来的 k 起步，不一步跳 | not called during the loading screen (no palette table computation is dispatched): advances by at most one frame each time (the frame length of the 30 fps tier), and after the screen is taken down it starts from the previous k instead of jumping in one step
    logk += (tgt - logk) * (1 - exp(-dt / 0.33));
    if (why != lwhy || area != larea || fabs(tgt - ltgt) > 0.05 || t - llog > 10000000000ull) {   // 规则 / 区域 / 目标变了，或每 10 秒：打一行原版值与 k | when the rule / area / target changed, or every 10 seconds: print one line with the original values and k
        LOG("3D 画面加亮：目标 k %.2f（规则 %s，区域 %d，原版环境亮度 %d，太阳角 %.0f°），当前 k %.2f", exp(tgt),
            why == 1 ? "①局外" : why == 2 ? "②迷宫类" : why == 3 ? "③原版画面不随昼夜" : "④按原版环境亮度", area, I, ang, exp(logk));
        lwhy = why; larea = area; llog = t; ltgt = tgt; }
    { static int tr = -1; static unsigned n; if (tr < 0) tr = getenv("DM_BRIGHT_TRACE") != NULL; if (tr && n++ % 6 == 0) LOG("3D 画面加亮：k %.3f（目标 %.3f，规则 %d）", exp(logk), exp(tgt), why); }   // 诊断：每 6 帧一行 | diagnostics: one line every 6 frames
    return (float)exp(logk);
}

void dm_bright_server_tick(uint64_t G) {   // 服务端每拍入口（diag.c，拍入口观测门挂着时）：测试开关 DM_TEST_DAYTIME | server per-tick entry (diag.c, when the tick entry observation gate is attached): test switch DM_TEST_DAYTIME
    static int ta = -2; static unsigned at;
    if (ta == -2) { const char *e = getenv("DM_TEST_DAYTIME"); ta = -1; if (e) { int a; unsigned t; if (sscanf(e, "%d@%u", &a, &t) == 2) { ta = a; at = t; } } }
    uint32_t tick = 0; if (ta < 0 || !rd(G + 0x170, &tick, 4) || tick < at) return;
    uint64_t acts = 0; int n = 0; rd(G + 0x190, &acts, 8);
    // 原版时段推进（0x55a9d0）每次只推一个时段，时间越过下一时段起点就拉回那个起点：要同时写对时段号（+0）与时段类型（+4）。
    // 时段表取原版普通那张（main + 0x1dab370，6 项 ×（起始角度、类型、光色））：起点 ≤ 目标角度的最后一项
    // The original game's time-period advance (0x55a9d0) advances only one period at a time, and once time passes the start of the next period it is pulled back to that start: both the period index (+0) and the period type (+4) must be written correctly.
    // The period table used is the original game's normal one (main + 0x1dab370, 6 entries × (start angle, type, light color)): the last entry whose start ≤ the target angle
    int idx = -1, kind = 0; uint64_t mb = main_base(); int32_t tbl[18];
    if (mb && rd(mb + 0x1dab370, tbl, sizeof tbl)) { int best = -1; for (int i = 0; i < 6; i++) if (tbl[3 * i] <= ta && tbl[3 * i] > best) { best = tbl[3 * i]; idx = i; kind = tbl[3 * i + 1]; } }
    uint64_t envs[6]; int ne = 0;
    for (int i = 0; acts && i < 5; i++) { uint64_t act = 0, env = 0; if (rd(acts + 8 * i, &act, 8) && rd(act + 8, &env, 8)) envs[ne++] = env; }
    uint64_t ce = env_ptr(); if (ce) envs[ne++] = ce;
    for (int i = 0; i < ne; i++) { int32_t rate = 0, fl = 0; if (!rd(envs[i] + 0x2c, &rate, 4) || rate <= 0 || !rd(envs[i] + 0x34, &fl, 4) || fl) continue;   // 置了特殊标志的不动 | ones with the special flag set are left alone
        if (idx >= 0) { *(int32_t *)(uintptr_t)envs[i] = idx; *(int32_t *)(uintptr_t)(envs[i] + 4) = kind; }
        *(float *)(uintptr_t)(envs[i] + 8) = (float)(ta * rate); n++; }
    LOG("DM_TEST_DAYTIME：拍 %u 把 %d 份环境（服务端各幕 + 客户端%s）写成太阳角 %d°、时段 %d / 类型 %d", tick, n, ce ? "" : "，客户端没取到", ta, idx, kind);
    ta = -1;
}
