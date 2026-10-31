// 免费回城的 ZR：只有这一格在 I 组（+0）和 II 组（+16）都空着才当回城；绑了技能就交还原版。
// iPhone 实测（手柄、「切换武器时更换技能键」关着）：技能绑在前 12 格（ZR = 249、按着 ZL = 250），宿主发布的当前武器组先是 -1（还没读到）、后是 1，
// 原先按组号非 0 只看 +16 格，六个面全读成空，ZR / ZL+ZR 一直被当成回城，绑的技能放不出来。
// Free town portal on ZR: only when this slot is empty in both set I (+0) and set II (+16) is it taken as town portal; once a skill is bound it goes back to the original game.
// Measured on iPhone (controller, "Weapon Swap Separate Skill Buttons" off): skills are bound in the first 12 slots (ZR = 249, with ZL held = 250), and the current weapon set
// the host publishes is first -1 (not read yet), later 1; previously any non-zero set number read only the +16 slots, all six faces read as empty, and ZR / ZL+ZR were always taken as town portal.
#include "../dm_freetp.c"
static int g_wset;
int dm_swapskill_wset(void) { return g_wset; }
static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
enum { MAP = 0x2d13cbc, BIND = 0x2d5f0c0, STRIDE = 28 };
static uint8_t *mb; static _Alignas(8) uint8_t player[0x200];   // 玩家 +0x1bd 数据版本 0：不挂 411（不调游戏函数） | player +0x1bd data version 0: 411 is not attached (no game function is called)
static void face(int i, uint32_t lg, uint32_t nm, uint32_t al) { uint32_t v[3] = { lg, nm, al }; memcpy(mb + MAP + 12 * i, v, 12); }
static void bind(int slot, uint32_t id) { memcpy(mb + BIND + (uint64_t)slot * STRIDE, &id, 4); }
static void step(void) { dm_freetp_client_step(mb, (uint64_t)(uintptr_t)player); }
int main(void) {
    mb = calloc(1, BIND + 32 * STRIDE); if (!mb) return 2;
    for (int s = 0; s < 32; s++) bind(s, ZR_EMPTY);
    face(0, 0x2000, 0, 6); face(1, 0x1000, 1, 7); face(2, 0x8000, 2, 8); face(3, 0x4000, 3, 9); face(4, 0x200, 4, 10); face(5, 0x800, 5, 11);   // ZR → 格 5，按着 ZL → 格 11 | ZR -> slot 5, with ZL held -> slot 11
    // 什么都没绑：ZR、ZL+ZR 都当回城（组号多少都一样） | Nothing bound: ZR and ZL+ZR are both town portal (whatever the set number)
    for (g_wset = -1; g_wset <= 1; g_wset++) { step(); CHECK(dm_freetp_zr_free(0) && dm_freetp_zr_free(1)); }
    // 实测现场：技能绑在前 12 格，组号 -1 / 0 / 1 都要交还原版 | Measured scene: skills in the first 12 slots; with set number -1 / 0 / 1 they must all go back to the original game
    bind(5, 249); bind(11, 250);
    for (g_wset = -1; g_wset <= 1; g_wset++) { step(); CHECK(!dm_freetp_zr_free(0)); CHECK(!dm_freetp_zr_free(1)); }
    // 只绑了 ZL+ZR：ZR 仍是回城 | Only ZL+ZR bound: ZR is still town portal
    bind(5, ZR_EMPTY); g_wset = 1; step(); CHECK(dm_freetp_zr_free(0)); CHECK(!dm_freetp_zr_free(1));
    // 「切换武器时更换技能键」开着、只在 II 组（+16）绑了：也交还原版 | "Weapon Swap Separate Skill Buttons" on, bound only in set II (+16): also goes back to the original game
    bind(11, ZR_EMPTY); bind(16 + 5, 249); g_wset = 1; step(); CHECK(!dm_freetp_zr_free(0)); CHECK(dm_freetp_zr_free(1));
    // 不在局里：两格都不当回城 | Not in a run: neither is town portal
    dm_freetp_client_step(mb, 0); CHECK(!dm_freetp_zr_free(0) && !dm_freetp_zr_free(1));
    printf("%s t_freetp_zr：%d 项检查，%d 项失败\n", fails ? "✗" : "✓", checks, fails);
    return fails != 0;
}
