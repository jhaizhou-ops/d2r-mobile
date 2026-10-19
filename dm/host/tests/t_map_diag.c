// 小地图诊断回归：实际生产采样函数，自造对象；检查缺失/隐藏/换幕/偏移及稳定状态去重，控件只读。
// Minimap diagnostic regression: real production sampler with authored objects; covers missing/hidden/Act changes/offsets, stable-state deduplication and read-only widgets.
// Minimap diagnostic regression: real production sampler with authored objects; covers missing/hidden/Act changes/offsets, stable-state deduplication and read-only widgets.
#define DM_PC_ITEMS_CONNECTED 0
#include "../dm_cursor.c"
int dm_ui_pc_desktop(void) { return 0; }
module_t g_mod[MAX_MOD]; int g_nmod;
static int area = 1, uy = 308;
void dm_hud_get(dm_hud_info *h) { memset(h, 0, sizeof *h); h->valid = 1; h->area = area; }
int dm_uiscreen_ingame(void) { return 1; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { *x = 0; *y = uy; *w = 2420; *h = 1360; }
int dm_screen_real(int *w, int *h) { *w = 2420; *h = 1668; return 1; }
static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
int main(void) {
    _Alignas(8) unsigned char root[128] = {0}, am[160] = {0}, before[160]; uint64_t child = (uint64_t)(uintptr_t)am;
    unsigned char *base = calloc(1, 0x2f05460); if (!base) return 2; module_t m = {0}; m.base = base;
    CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 1);
    CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 0);
    *(uint64_t *)am = (uint64_t)(uintptr_t)base + 0x20c58a0; *(const char **)(am + 8) = "AutoMap";
    *(uint64_t *)(root + 0x58) = (uint64_t)(uintptr_t)&child; *(uint64_t *)(root + 0x60) = 1;
    *(int *)(am + 0x70) = -1280; *(int *)(am + 0x74) = -411; *(int *)(am + 0x78) = 1280; *(int *)(am + 0x7c) = 719;
    CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 1); CHECK(g_map_diag.visible == 0);
    am[0x50] = 1; memcpy(before, am, sizeof am);
    CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 1); CHECK(!memcmp(before, am, sizeof am));
    for (int i = 0; i < 100; i++) CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 0);
    area = 40; CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 1); CHECK(g_map_diag.area == 40);
    area = 47; am[0x50] = 0; CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 1); CHECK(g_map_diag.area == 47 && g_map_diag.visible == 0);
    uy = 154; CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 1);
    *(int *)(base + 0x2d479f4) = 2; *(int *)(base + 0x2d5c748) = 1;
    CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 1); CHECK(g_map_diag.cfg == 2 && g_map_diag.mini == 1);
    *(uint64_t *)(root + 0x60) = 0; CHECK(ui_map_diag(&m, (uint64_t)(uintptr_t)root) == 1); CHECK(!g_map_diag.object);
    free(base); printf("t_map_diag: %d checks, %d failures\n", checks, fails); return fails != 0;
}
