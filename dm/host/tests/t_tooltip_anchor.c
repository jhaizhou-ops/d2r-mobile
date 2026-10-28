// 装备预览锚点回归：运行实际 OBS_TOOLTIP 回调，使用自造面板树与矩形，不需要游戏内容。
// Item-preview anchor regression: runs the real OBS_TOOLTIP callback with an authored widget tree and rectangles, without game content.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD]; int g_nmod;
uint64_t dm_guest_frame(void) { return 1; }
static int padmode, modebusy;
int dm_padmode_current(void) { return padmode; }
int dm_padmode_busy(void) { return modebusy; }
int dm_cursor_item(dm_cursor_item_info *o) { memset(o, 0, sizeof *o); strcpy(o->code, "cm3"); return 1; }
int main(int argc, char **argv) {
    (void)argv; unsetenv("DM_TEST_TIPDOCK"); if (argc > 1) setenv("DM_TEST_TIPDOCK", "0", 1);
    unsigned char root[128] = {0}, inv[128] = {0}; uint64_t child = (uint64_t)(uintptr_t)inv;
    *(const char **)(inv + 8) = "PlayerInventoryExpansionLayout";
    *(uint64_t *)(root + 0x58) = (uint64_t)(uintptr_t)&child; *(uint64_t *)(root + 0x60) = 1;
    unsigned char *base = calloc(1, 0x2f05460); if (!base) return 2;
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = base;
    *(uint64_t *)(base + 0x2f05458) = (uint64_t)(uintptr_t)root;
    *(int *)(base + 0x211da38) = 2420; *(int *)(base + 0x211da3c) = 1360;
    int fails = 0;
    const int xy[4][2] = { { 890, 740 }, { 1500, 740 }, { 890, 1120 }, { 1500, 1120 } };
    for (int i = 0; i < 4; i++) {
        uint64_t regs[9] = { 11, (uint64_t)(uintptr_t)"authored tooltip", (uint32_t)xy[i][0] | (uint64_t)(uint32_t)xy[i][1] << 32, 70ull | 140ull << 32, 44, 55, 66, 77, 88 }, old[9];
        memcpy(old, regs, sizeof old); dm_ui_tooltip_obs(regs);
        int cx = (int32_t)regs[2];
        int want_x = xy[i][0] + 35 < 1210 ? 371 : 2049;
        if (argc > 1) { if (memcmp(regs, old, sizeof old)) fails++; }
        else if (cx != want_x || (regs[2] >> 32) != (old[2] >> 32) || regs[3] != (old[3] & 0xffffffff00000000ull)) { fails++; printf("FAIL corner %d: x=%d, expected %d; vertical anchor must be retained\n",i,cx,want_x); }
        for (int k = 0; k < 9; k++) if (k != 2 && k != 3 && regs[k] != old[k]) fails++;
    }
    const unsigned excluded[] = { DM_PS_BANK, DM_PS_VENDOR, DM_PS_IMBUE, DM_PS_CUBE, DM_PS_VAULT, DM_PS_DEAD };
    for (unsigned i=0;i<sizeof excluded/sizeof *excluded;i++) {
        uint64_t r[9] = { 11, (uint64_t)(uintptr_t)"authored tooltip", 890ull | 740ull << 32, 70ull | 140ull << 32 }, old[9];
        memcpy(old,r,sizeof r); atomic_store(&g_pstate,excluded[i]); dm_ui_tooltip_obs(r); if(memcmp(r,old,sizeof r)) fails++;
    }
    atomic_store(&g_pstate,0);
    for (int i=0;i<2;i++) {
        padmode=i==0; modebusy=i==1;
        uint64_t r[9] = { 11, (uint64_t)(uintptr_t)"authored tooltip", 890ull | 740ull << 32, 70ull | 140ull << 32 }, old[9];
        memcpy(old,r,sizeof r); dm_ui_tooltip_obs(r); if(memcmp(r,old,sizeof r)) fails++;
    }
    free(base); printf("t_tooltip_anchor: %s, 4 corners + 8 exclusion gates, %d failures\n", argc > 1 ? "native control" : "same-side vertical anchor", fails); return fails != 0;
}
