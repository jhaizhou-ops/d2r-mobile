/* Android HUD 抬数（P3）：OBS_TICK（服务端拍入口，G = x0）每拍读受控玩家 → dm_hud_publish。
 * 移植 dm/host/diag.c hud_tick 的必需面：等级 / 本级经验‰ / 腰带四列件数 / 回城次数 / 生命法力 /
 * 未分配点数 / 死亡 / 坐标 / 区域 / 当前人数档。地址同 94012（与 iOS 一致）。
 * Android HUD reads (P3): per server tick (OBS_TICK gate, G = x0) read the controlled player and
 * publish into dm_hud. Ported from diag.c's hud_tick (same 94012 addresses as iOS). */
#include "../host/hle.h"
#include "../host/dm.h"
#include "../host/dm_hud.h"
#include "dm_hud_exp.h"   /* k_exp[100]（build/generated 生成） */
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum { CLS_TBK = 533, CLS_TSC = 544 };   /* 回城书 / 回城卷轴（原版 misc.txt 行序） | Tome / Scroll of Town Portal */

static int rd(uint64_t a, void *out, size_t n) {
    if (!a || a > UINT64_MAX - n) return 0;
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}
static module_t *main_mod(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return &g_mod[i]; return NULL; }
static int stat_read(uint64_t unit, uint32_t want, uint32_t *value) {
    uint64_t sl = 0, sb = 0; uint32_t sc = 0;
    if (!rd(unit + 0x88, &sl, 8) || !sl || !rd(sl + 0x30, &sb, 8) || !rd(sl + 0x38, &sc, 4)) return 0;
    if (sc > 512) sc = 512;
    uint32_t pair[2];
    for (uint32_t i = 0; i < sc; i++) if (rd(sb + 8 * i, pair, sizeof pair) && pair[0] == want << 16) { *value = pair[1]; return 1; }
    return 0;
}
static uint64_t players_obj(void) { module_t *m = main_mod(); return m ? ((uint64_t (*)(void))(m->base + 0x3102c0))() : 0; }
static uint32_t local_player_id(void) {
    uint32_t slot = 8, lid = 0xffffffffu; module_t *mm = main_mod();
    if (mm && rd((uint64_t)mm->base + 0x2d20e4c, &slot, 4) && slot < 8) rd((uint64_t)mm->base + 0x2d60380 + 4 * slot, &lid, 4);
    return lid;
}

static void hud_tick(uint64_t G) {
    dm_hud_info h = {0};
    uint64_t player = 0, inv = 0, u = 0, next = 0;
    uint32_t lid = local_player_id();
    for (int b = 0; b < 128 && !player; b++) {
        if (!rd(G + 0x2230 + 8 * b, &u, 8)) continue;
        for (int k = 0; u && k < 4096; k++) {
            uint32_t mode = 0, id = 0; uint64_t pp = 0; uint16_t c[4] = {0};
            if (rd(u + 0xC, &mode, 4) && mode && rd(u + 0x38, &pp, 8) && pp && rd(pp, c, 8) && (c[1] || c[3]) &&
                (lid == 0xffffffffu || (rd(u + 8, &id, 4) && id == lid))) { player = u; break; }
            if (!rd(u + 0x158, &u, 8)) break;
        }
    }
    uint32_t sig = 0;
    if (player && rd(player + 0x90, &inv, 8) && rd(inv, &sig, 4) && sig == 0x01020304) {
        uint32_t lv = 0, xp = 0;
        stat_read(player, 12, &lv);
        stat_read(player, 13, &xp);
        module_t *mm = main_mod();
        if (mm) { int (*get)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(mm->base + 0x5b9290);
            h.hp = get(player, 6, 0) >> 8; h.hp_max = get(player, 7, 0) >> 8; h.mp = get(player, 8, 0) >> 8; h.mp_max = get(player, 9, 0) >> 8; }
        uint32_t sp = 0, kp = 0, pm = 0;
        stat_read(player, 4, &sp); stat_read(player, 5, &kp); h.points = (int)(sp + kp);
        if (rd(player + 0xC, &pm, 4)) h.dead = dm_hud_player_mode_dead(pm);
        h.level = (int)lv;
        { uint64_t pp = 0, r1 = 0, r2 = 0, lvl = 0; uint16_t c[4] = {0}; uint32_t no = 0;
          if (rd(player + 0x38, &pp, 8) && rd(pp, c, 8)) { h.x = c[1]; h.y = c[3]; }
          if (pp && rd(pp + 0x20, &r1, 8) && rd(r1 + 0x18, &r2, 8) && rd(r2 + 0x90, &lvl, 8) && rd(lvl + 0x1f8, &no, 4) && no < 200) h.area = (int)no; }
        if (lv >= 99) h.exp_permille = 1000;
        else if (lv >= 1) {
            uint32_t lo = k_exp[lv - 1], hi = k_exp[lv];
            h.exp_permille = hi > lo && xp > lo ? (int)((uint64_t)(xp - lo) * 1000 / (hi - lo)) : 0;
            if (h.exp_permille > 1000) h.exp_permille = 1000;
        }
        if (rd(inv + 0x10, &u, 8))
            for (unsigned k = 0; u && k < 1024; k++) {
                uint32_t id = 0, cls = 0, mode = 0, x = 0; uint64_t d = 0, pp = 0;
                int ok = rd(u + 8, &id, 4) && rd(u + 4, &cls, 4) && rd(u + 0xC, &mode, 4) && rd(u + 0x10, &d, 8) && d;
                if (!ok) break;
                if (rd(u + 0x38, &pp, 8) && pp) rd(pp + 0x10, &x, 4);
                if (mode == 2) h.belt[x % 4]++;
                if (cls == CLS_TSC) h.portal++;
                if (cls == CLS_TBK) { uint32_t q = 0; if (stat_read(u, 70, &q)) h.portal += (int)q; }
                next = 0; rd(d + 0xB0, &next, 8); u = next;
            }
        h.valid = 1;
    }
    { uint64_t obj = players_obj(); uint32_t pl = 0; if (obj && rd(obj + 0xc, &pl, 4)) h.players = (int)pl; }
    { module_t *m = main_mod(); uint32_t qu = 0; if (m && rd((uint64_t)m->base + 0x2d5ff58, &qu, 4)) h.quest_unread = qu; }
    dm_hud_publish(&h);
}
static void tick_obs(const uint64_t *regs) {
    hud_tick(regs[0]);
    static unsigned long n;
    if (++n % 120 == 0) { dm_hud_info h; dm_hud_get(&h);
        LOG("[hud] 拍%lu valid=%d hp=%d/%d mp=%d/%d lv=%d exp=%d‰ belt=%d,%d,%d,%d players=%d portal=%d pts=%d dead=%d",
            n, h.valid, h.hp, h.hp_max, h.mp, h.mp_max, h.level, h.exp_permille,
            h.belt[0], h.belt[1], h.belt[2], h.belt[3], h.players, h.portal, h.points, h.dead); }
}
void dm_android_hud_install(void) {
    LOG("[hud] 安装服务端拍门：门在=%d", gate_obs_present(OBS_TICK));
    gate_obs_set(OBS_TICK, tick_obs);
}
