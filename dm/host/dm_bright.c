// 3D 画面默认保持原版亮度（k = 1），不再按洞穴 / 夜晚自动增加曝光。
// 诊断开关 DM_BRIGHT_K=<k> 可显式固定 1–4 倍曝光；无效值回退原版亮度。图形层 nvn_mtl.m bright_lut 每帧取 k，k = 1 时不改调色表。
// DM_TEST_DAYTIME=<角度>@<拍>：第 <拍> 拍把服务端各幕与客户端的环境时间写成该太阳角（看正午 / 黄昏 / 午夜；
// 要挂拍入口观测门，带 DM_C2S_LOG 等任一）。只写客户端那份会被服务端同步回去。
// 保留诊断所用的客户端环境入口 main+0x2d20bd0：当前幕 +8 指向环境。
// Keep the original 3D brightness by default (k = 1), without automatic exposure boosts in caves or at night.
// Diagnostic switch DM_BRIGHT_K=<k> explicitly fixes exposure at 1–4; invalid values fall back to the original brightness.
// The graphics layer's nvn_mtl.m bright_lut reads k every frame and leaves the color lookup table untouched at k = 1.
// DM_TEST_DAYTIME=<angle>@<tick> writes the environment time of every server act and the client at that tick (to view noon / dusk / midnight;
// attach the tick entry observation gate with any one of DM_C2S_LOG etc.). Writing only the client's copy gets synced back by the server.
// Retain the diagnostic client environment entry at main+0x2d20bd0: current act +8 points to its environment.
#include "dm.h"
#include <errno.h>
#include <math.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

static int rd(uint64_t a, void *buf, size_t n) { vm_size_t got = 0; return a && !vm_read_overwrite(mach_task_self(), a, n, (vm_address_t)buf, &got) && got == n; }
static uint64_t main_base(void) { static uint64_t b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint64_t)g_mod[i].base; return b; }
static uint64_t env_ptr(void) { uint64_t act = 0, env = 0, mb = main_base(); return mb && rd(mb + 0x2d20bd0, &act, 8) && rd(act + 8, &env, 8) ? env : 0; }

float dm_bright_k(void) {   // 图形层每帧调（单线程：出图 / 回放线程） | called every frame by the graphics layer (single thread: the present / replay thread)
    static int initialized; static float k = 1.0f;
    if (!initialized) {
        initialized = 1;
        const char *e = getenv("DM_BRIGHT_K");
        if (e) {
            char *end; errno = 0; double value = strtod(e, &end);
            if (end != e && !*end && errno != ERANGE && isfinite(value) && value >= 1.0 && value <= 4.0) {
                k = (float)value;
                LOG("DM_BRIGHT_K=%g：3D 画面曝光由测试开关固定", k);
            } else LOG("DM_BRIGHT_K 无效（须为 1–4）：保持原版亮度");
        }
    }
    return k;
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
