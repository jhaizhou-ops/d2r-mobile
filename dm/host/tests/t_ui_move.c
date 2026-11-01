// 用合成控件验证生产位置守卫；按物品栏原位保存函数 main+0x3db974 建模，不运行游戏映像。
// Exercise the production move guard with synthetic widgets, modeling inventory origin saving at main+0x3db974. No game image is run.
#include "../dm_cursor.c"

module_t g_mod[MAX_MOD];
int g_nmod;
static int frozen, checks, fails, relayouts, inventory_saves;
int dm_padmode_busy(void) { return frozen; }
uint64_t dm_guest_frame(void) { return 100; }

#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

typedef struct { uint64_t data[64]; } test_widget;
static uint64_t vt[16], children[4], overlay_children[2], bank_children[1];
static test_widget root, old_widget, inventory, bank, overlay, wide_background, bank_leaf;
static uint64_t address(test_widget *w) { return (uint64_t)(uintptr_t)w; }
static int32_t *rect(test_widget *w) { return (int32_t *)((uint8_t *)w + 0x70); }
static int32_t *saved_position(test_widget *w) { return (int32_t *)((uint8_t *)w + 0x18c); }
static void relayout(uint64_t w) {
    relayouts++;
    // PlayerInventoryPanel 的 +0x10 方法保存当前原位，不只是重排子控件；建模此行为以暴露打开下个面板时复用坐标被污染的问题。
    // PlayerInventoryPanel's +0x10 method saves its current origin; it does
    // not merely reflow children. Model that behavior to expose pollution
    // of the coordinates reused when the game opens the next panel.
    if (!strcmp((const char *)*(uint64_t *)(w + 8), "PlayerInventoryExpansionLayout")) {
        memcpy((void *)(w + 0x18c), (const void *)(w + 0x70), 8);
        inventory_saves++;
    }
}
static void init_widget(test_widget *w, const char *name, int x) {
    memset(w, 0, sizeof *w);
    w->data[0] = (uint64_t)(uintptr_t)vt;
    w->data[1] = (uint64_t)(uintptr_t)name;
    *((uint8_t *)w + 0x50) = *((uint8_t *)w + 0x51) = 1;
    *(float *)((uint8_t *)w + 0x80) = 1.0f;
    rect(w)[0] = x;
    rect(w)[1] = 200;
    memcpy(saved_position(w), rect(w), 8);
}
static void reset(void) {
    frozen = relayouts = inventory_saves = g_nmv = 0;
    memset(g_mv, 0, sizeof g_mv);
    memset(g_mv_want, 0, sizeof g_mv_want);
    g_mv_base_on = 0;
    init_widget(&root, "PanelManager", 0);
    init_widget(&old_widget, "BankExpansionLayout", -1000);
    init_widget(&inventory, "PlayerInventoryExpansionLayout", 1000);
    init_widget(&bank, "BankExpansionLayout", -1000);
    root.data[0x58 / 8] = (uint64_t)(uintptr_t)children;
    root.data[0x60 / 8] = 2;
    children[0] = address(&bank); children[1] = address(&inventory);
    *(uint64_t *)(g_mod[0].base + 0x2f05458) = address(&root);
}
static void wide_tree(void) {
    reset();
    init_widget(&overlay, "ControllerOverlay", 0);
    init_widget(&wide_background, "WideBackground", 99999);
    init_widget(&bank_leaf, "BankItems", 0);
    children[2] = address(&overlay); root.data[0x60 / 8] = 3;
    overlay_children[0] = address(&wide_background);
    overlay.data[0x58 / 8] = (uint64_t)(uintptr_t)overlay_children;
    overlay.data[0x60 / 8] = 1;
    bank_children[0] = address(&bank_leaf);
    bank.data[0x58 / 8] = (uint64_t)(uintptr_t)bank_children;
    bank.data[0x60 / 8] = 1;
}
static void native_inventory_open(int dx) {
    rect(&inventory)[0] = saved_position(&inventory)[0] + dx;
    rect(&inventory)[1] = saved_position(&inventory)[1];
}
int main(void) {
    g_nmod = 1; strcpy(g_mod[0].name, "main");
    g_mod[0].base = calloc(1, 0x2f05458 + 8);
    if (!g_mod[0].base) return 2;
    vt[0x10 / 8] = (uint64_t)(uintptr_t)relayout;

    // 重建会移除新物品栏条目前面的条目；上次写入值必须记在压缩后的槽位，否则随后每帧都把上次偏移当作新原位。
    // A rebuild removes entries ahead of the newly inserted inventory entry.
    // Its last-written value must be stored in the compacted slot, or every
    // subsequent frame treats our previous offset as a new original position.
    reset();
    children[0] = address(&old_widget);
    mv_offset(MV_WIDE, address(&old_widget), 0, -196, 1);
    mv_offset(MV_WIDE, address(&old_widget), 2, 392, 1);
    children[0] = address(&bank);
    mv_offset(MV_WIDE, address(&inventory), 0, 196, 1);
    CHECK(g_nmv == 1, "destroyed widgets are pruned");
    CHECK(g_mv[0].r.w == address(&inventory) && g_mv[0].orig == 1000 && g_mv[0].val == 1196,
          "pruning records the original and last-written x in the new slot");
    for (int i = 0; i < 20; i++) mv_offset(MV_WIDE, address(&inventory), 0, 196, 1);
    CHECK(rect(&inventory)[0] == 1196, "inventory offset is applied once after a rebuild");
    mv_restore(address(&root), MV_WIDE);
    CHECK(rect(&inventory)[0] == 1000 && g_nmv == 0, "inventory restores its unshifted position");

    // 压缩表还必须保留不相关的存活条目。
    // Compaction must also preserve an unrelated live entry.
    reset(); children[0] = address(&old_widget);
    mv_offset(MV_WIDE, address(&old_widget), 0, -196, 0);
    mv_offset(MV_WIDE, address(&inventory), 0, 196, 0);
    children[0] = address(&bank);
    mv_offset(MV_WIDE, address(&bank), 0, -196, 1);
    CHECK(g_nmv == 2, "compaction keeps the live inventory entry");
    mv_offset(MV_WIDE, address(&inventory), 0, 196, 0);
    mv_offset(MV_WIDE, address(&bank), 0, -196, 1);
    CHECK(rect(&inventory)[0] == 1196 && rect(&bank)[0] == -1196,
          "both panels retain a single offset after partial pruning");
    mv_restore(address(&root), MV_WIDE);
    CHECK(rect(&inventory)[0] == 1000 && rect(&bank)[0] == -1000 && !g_nmv,
          "both panels restore after partial pruning");

    // 两次重建期间都不改原版控件的矩形。
    // Keep original widget geometry untouched during either rebuild pass.
    reset(); frozen = 1;
    mv_offset(MV_WIDE, address(&inventory), 0, 196, 1);
    CHECK(rect(&inventory)[0] == 1000 && !g_nmv && !relayouts,
          "rebuild freezes new position adjustments and relayouts");
    frozen = 0;
    mv_offset(MV_WIDE, address(&inventory), 0, 196, 1);
    frozen = 1;
    mv_restore(address(&root), MV_WIDE);
    CHECK(rect(&inventory)[0] == 1000 && !g_nmv,
          "switch cleanup can still restore existing adjustments while frozen");

    reset();
    for (int mode = 0; mode < 12; mode++) {
        mv_offset(MV_WIDE, address(&inventory), 0, 196, 1);
        CHECK(rect(&inventory)[0] == 1196, "mode starts with a single inventory offset");
        frozen = 1; mv_restore(address(&root), MV_WIDE);
        for (int pass = 0; pass < 2; pass++) {
            for (int frame = 0; frame < 6; frame++) {
                mv_offset(MV_WIDE, address(&inventory), 0, 196, 1);
                CHECK(rect(&inventory)[0] == 1000 && !g_nmv, "both rebuild passes keep baseline geometry");
            }
        }
        frozen = 0;
    }
    // 验证实际储藏箱更新函数，不另抄偏移；加宽与还原都必须保留原版原位备份。
    // Exercise the actual stash pump rather than duplicating its offsets.
    // Native origin backups must survive both widening and restoration.
    wide_tree();
    ui_widestash_pump(address(&root));
    CHECK(rect(&inventory)[0] == 1196, "stash pump moves inventory by one widening offset");
    CHECK(saved_position(&inventory)[0] == 1000 && saved_position(&inventory)[1] == 200,
          "stash widening does not save its temporary x/y as the native origin");
    CHECK(inventory_saves == 0, "stash pump never calls the inventory origin-saving method");
    for (int frame = 0; frame < 20; frame++) ui_widestash_pump(address(&root));
    CHECK(rect(&inventory)[0] == 1196 && saved_position(&inventory)[0] == 1000,
          "stable stash frames preserve one offset and the native origin");
    *((uint8_t *)&bank + 0x50) = 0;
    // 商店可能在下一次清理前就已摆好物品栏。
    // A vendor can position the inventory before our next cleanup pass.
    native_inventory_open(300);
    ui_widestash_pump(address(&root));
    CHECK(rect(&inventory)[0] == 1300 && mv_count(MV_WIDE) == 1,
          "vendor geometry keeps the unmodified native origin and upstream pending restore");
    CHECK(saved_position(&inventory)[0] == 1000 && inventory_saves == 0,
          "cleanup preserves native origin when the game already moved inventory");

    wide_tree();
    for (int cycle = 0; cycle < 12; cycle++) {
        *((uint8_t *)&bank + 0x50) = 1;
        native_inventory_open(0);
        ui_widestash_pump(address(&root));
        CHECK(rect(&inventory)[0] == 1196 && saved_position(&inventory)[0] == 1000,
              "reopening stash after native origin restore does not accumulate x");
        *((uint8_t *)&bank + 0x50) = 0;
        ui_widestash_pump(address(&root));
        CHECK(rect(&inventory)[0] == 1000 && saved_position(&inventory)[0] == 1000 && !mv_count(MV_WIDE),
              "closing stash restores rect and preserves native origin each cycle");
    }
    CHECK(inventory_saves == 0, "repeated stash open/close never rewrites native origin");

    // 魔盒并排用同一守卫路径，在 y 方向将物品栏藏到屏外。
    // Cube side-by-side hides inventory on y, using the same guarded path.
    reset();
    mv_offset(MV_CUBE, address(&inventory), 1, 6000, 1);
    CHECK(rect(&inventory)[1] == 6200, "cube moves inventory below the visible area");
    CHECK(saved_position(&inventory)[1] == 200 && inventory_saves == 0,
          "cube hiding does not poison inventory's saved y");
    mv_restore(address(&root), MV_CUBE);
    CHECK(rect(&inventory)[1] == 200 && saved_position(&inventory)[1] == 200 && inventory_saves == 0,
          "cube cleanup restores y without invoking the origin-saving method");
    native_inventory_open(0);
    CHECK(rect(&inventory)[0] == 1000 && rect(&inventory)[1] == 200,
          "native inventory restore remains on screen after cube closes");

    // 面板内容隐藏后，面板自己的可见位可能仍然保留。
    // A panel can keep its own visible bit while its content is hidden.
    wide_tree();
    ui_widestash_pump(address(&root));
    *((uint8_t *)&bank_leaf + 0x51) = 0;
    ui_widestash_pump(address(&root));
    CHECK(rect(&inventory)[0] == 1000 && !mv_count(MV_WIDE) && !g_mv_want[MV_WIDE],
          "stash with no visible leaf releases the widening offsets");

    // 外框重建或布局没有 WideBackground 时，即使储藏箱仍报开启，也必须清理存活控件上的偏移。
    // Overlay rebuilding or a layout without WideBackground must release
    // offsets on surviving widgets, even if the stash still reports open.
    wide_tree();
    ui_widestash_pump(address(&root));
    root.data[0x60 / 8] = 2;
    ui_widestash_pump(address(&root));
    CHECK(rect(&inventory)[0] == 1000 && rect(&bank)[0] == -1000 && !mv_count(MV_WIDE) && !g_mv_want[MV_WIDE],
          "missing overlay restores surviving panels and clears widening intent");

    wide_tree();
    ui_widestash_pump(address(&root));
    overlay.data[0x60 / 8] = 0;
    ui_widestash_pump(address(&root));
    CHECK(rect(&inventory)[0] == 1000 && rect(&bank)[0] == -1000 && !mv_count(MV_WIDE) && !g_mv_want[MV_WIDE],
          "missing wide frame restores surviving panels and clears widening intent");

    free(g_mod[0].base);
    printf("t_ui_move: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
