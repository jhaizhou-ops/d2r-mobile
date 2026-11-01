// 用有界原版调用桩验证生产提示条管理与 main+0xaa9f88 追加门；按八个内嵌文字控件的容量建模。
// Exercise the production legend manager and append gate at main+0xaa9f88 with bounded native-call stubs, modeling eight inline text widgets.
#include "../dm_padmode.c"

module_t g_mod[MAX_MOD];
int g_nmod;
static uint64_t frame;
static uint32_t generation = 1;
uint32_t dm_ui_gen(void) { return generation; }
// 每批合成控件全部替换；旧代号不保留任何存活控件。
// Each synthetic generation replaces all widgets; no old widget survives.
uint64_t dm_wref_get(dm_wref *r) { return r->gen == generation ? r->w : 0; }
uint64_t dm_guest_frame(void) { return frame; }
void dm_hud_get(dm_hud_info *h) { memset(h, 0, sizeof *h); }
void dm_hud_request_players(int n) { (void)n; }
static int checks, fails, appends, removals, overruns;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

typedef struct {
    uint64_t widget[0x1818 / 8];
    uint64_t entries[16][8];
    char text[16][64];
} test_legend;
static test_legend a, other;
static uint64_t root[16], overlay[0x448 / 8], children[1];
static uint64_t addr(const void *p) { return (uint64_t)(uintptr_t)p; }
static uint64_t count(test_legend *lg) { return lg->widget[0x90 / 8]; }
static void write_entry(test_legend *lg, unsigned i, uint32_t glyph, const char *text) {
    memmove(lg->text[i], text, strlen(text) + 1);
    memset(lg->entries[i], 0, sizeof lg->entries[i]);
    lg->entries[i][0] = glyph; lg->entries[i][1] = 1;
    lg->entries[i][2] = addr(lg->text[i]); lg->entries[i][3] = strlen(text);
}
static void make(void *out, int act, int unused, void *view) {
    (void)unused;
    struct { const char *p; uint64_t n; } *s = view;
    uint64_t *e = out; memset(e, 0, 64);
    e[0] = (uint32_t)act; e[1] = 1; e[2] = addr(s->p); e[3] = s->n;
    e[4] = 1ull << 63;   // 文字由测试后端持有，无需释放客体分配。 | The test backend owns its text; no guest allocation needs freeing.
}
static void append(uint64_t vector, uint64_t entry) {
    test_legend *lg = (test_legend *)(uintptr_t)(vector - 0x88);
    uint64_t *e = (uint64_t *)(uintptr_t)entry; unsigned n = (unsigned)count(lg);
    if (n >= LG_CAP) overruns++;
    if (n >= 16) abort();
    write_entry(lg, n, (uint32_t)e[0], (const char *)(uintptr_t)e[2]);
    lg->widget[0x90 / 8]++; appends++;
}
static void add(uint64_t widget, void *entry) {
    test_legend *lg = (test_legend *)(uintptr_t)widget; uint64_t *e = entry;
    for (unsigned i = 0; i < count(lg); i++) if (lg->entries[i][0] == e[0]) {
        write_entry(lg, i, (uint32_t)e[0], (const char *)(uintptr_t)e[2]); return;
    }
    uint64_t regs[31] = { 0 }; regs[0] = widget + 0x88; regs[1] = addr(entry); regs[19] = widget;
    svc_legend_append(regs);
}
static void remove_entry(uint64_t widget, int act) {
    test_legend *lg = (test_legend *)(uintptr_t)widget;
    for (unsigned i = 0; i < count(lg); i++) if (lg->entries[i][0] == (uint32_t)act) {
        for (unsigned j = i; j + 1 < count(lg); j++)
            write_entry(lg, j, (uint32_t)lg->entries[j + 1][0], lg->text[j + 1]);
        lg->widget[0x90 / 8]--; removals++; return;
    }
}
static void native_add(test_legend *lg, int act, const char *text) {
    uint64_t e[8]; struct { const char *p; uint64_t n; } sv = { text, strlen(text) };
    make(e, act, 0, &sv); add(addr(lg), e);
}
static void init_legend(test_legend *lg) {
    memset(lg, 0, sizeof *lg); lg->widget[0x88 / 8] = addr(lg->entries);
}
static void reset(void) {
    init_legend(&a); init_legend(&other);
    memset(root, 0, sizeof root); memset(overlay, 0, sizeof overlay);
    root[1] = addr("PanelManager"); root[0x58 / 8] = addr(children); root[0x60 / 8] = 1;
    children[0] = addr(overlay); overlay[1] = addr("ControllerOverlay");
    ((uint8_t *)overlay)[0x50] = 1; overlay[0x440 / 8] = addr(&a);
    *(uint64_t *)(g_b + 0x2f05458) = addr(root);
    g_lg_n = g_lg_on_n = g_lh_n = 0; dm_wref_set(&g_lg_widget, 0); g_lg_frame = 0;
    frame = 100; g_cur = 1; atomic_store(&g_freeze, 0);
    appends = removals = overruns = 0;
}
static void declare_pages(void) {
    dm_padmode_legend(19, "Previous"); dm_padmode_legend(20, "Next"); legend_pump(g_b, frame);
}
static void fill(test_legend *lg, int n) { for (int i = 0; i < n; i++) native_add(lg, 100 + i, "Native"); }
int main(void) {
    unsetenv("DM_TEST_LEGEND"); unsetenv("DM_TEST_LEGEND_LOG");
    g_b = calloc(1, 0x3211708); if (!g_b) return 2;
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = g_b;
    g_lg_api.base = g_b; g_lg_api.make = make; g_lg_api.add = add;
    g_lg_api.remove = remove_entry; g_lg_api.append = append;

    // 复现原先未受保护的追加，确认夹具能够检出第九项访问。
    // Reproduce the original unguarded append and verify that this fixture detects the ninth access.
    reset(); fill(&a, 8);
    uint64_t ninth[8]; struct { const char *p; uint64_t n; } sv = { "Compare", 7 };
    make(ninth, 2, 0, &sv); append(addr(&a) + 0x88, addr(ninth));
    CHECK(count(&a) == 9 && overruns == 1, "unguarded native append reproduces the ninth-entry overflow");

    // 原先的崩溃：自定义翻页提示占满提示条后，原版焦点又追加第九条提示。
    // The original crash: native focus adds a ninth hint after custom paging hints filled the legend.
    reset(); fill(&a, 6); declare_pages();
    CHECK(count(&a) == 8 && g_lg_on_n == 2, "native and custom hints share an eight-entry capacity");
    native_add(&a, 2, "Compare");
    CHECK(count(&a) == 8 && !overruns, "native Compare never appends a ninth entry");
    CHECK(legend_find(g_b, addr(&a), 2) != 0, "native Compare takes priority over optional paging");
    CHECK(g_lg_on_n == 1 && removals == 1, "evicted custom hint loses ownership");
    frame++; declare_pages();
    CHECK(count(&a) == 8 && !overruns && g_lg_on_n == 1, "next frame does not add the evicted optional hint at capacity");

    reset(); fill(&a, 8); int before = appends;
    native_add(&a, 500, "Another native action");
    CHECK(count(&a) == 8 && appends == before && !overruns, "a full native-only legend defers additional actions safely");
    native_add(&a, 100, "Changed");
    CHECK(count(&a) == 8 && !strcmp(a.text[0], "Changed"), "same-key native updates still work at capacity");
    declare_pages();
    CHECK(count(&a) == 8 && !g_lg_on_n, "deferred custom hints are not recorded as installed");

    reset(); native_add(&a, 19, "Native previous"); fill(&a, 7); declare_pages();
    CHECK(count(&a) == 8 && g_lg_on_n == 1 && legend_ours(legend_find(g_b, addr(&a), 19), "Previous"),
          "same-key custom text replacement is allowed without an extra slot");

    reset(); fill(&a, 2); declare_pages(); ((uint8_t *)overlay)[0x50] = 0; frame++;
    legend_pump(g_b, frame);
    CHECK(count(&a) == 2 && removals == 2 && !g_lg_on_n && !g_lg_n, "hidden overlay removes its custom hints before forgetting them");
    ((uint8_t *)overlay)[0x50] = 1; frame++; legend_pump(g_b, frame);
    CHECK(count(&a) == 2, "reopening the same widget does not resurrect stale declarations");

    reset(); declare_pages(); frame++;
    dm_padmode_on_uimsg("PanelManager", "ClosePanel", "BankExpansionLayout");
    CHECK(!count(&a) && !g_lg_on_n && !g_lg_n, "closing the bank synchronously retracts paging hints");
    legend_pump(g_b, frame);
    CHECK(!count(&a), "two-frame declaration grace cannot restore hints after bank close");

    reset(); declare_pages(); native_add(&a, 19, "Original panel now owns this key");
    legend_clear(g_b);
    CHECK(count(&a) == 1 && legend_find(g_b, addr(&a), 19) != 0, "cleanup preserves a same-key hint replaced by the game");
    CHECK(!g_lg_on_n, "cleanup relinquishes replaced entries");

    reset(); declare_pages(); dm_wref_set(&g_lg_widget, 1); overlay[0x440 / 8] = addr(&other); frame += 3;
    legend_pump(g_b, frame);
    CHECK(!removals && !g_lg_on_n && dm_wref_get(&g_lg_widget) == addr(&other), "rebuild never dereferences the destroyed widget address");

    reset(); declare_pages(); generation++; frame += 3;
    legend_pump(g_b, frame);
    CHECK(!removals && !g_lg_on_n, "a new generation at the same address does not inherit custom ownership");

    reset(); declare_pages(); fill(&other, 8); before = removals;
    native_add(&other, 200, "Other panel");
    CHECK(count(&other) == 8 && count(&a) == 2 && removals == before, "another legend cannot evict custom hints from the current widget");

    reset(); declare_pages(); atomic_store(&g_freeze, 1); legend_pump(g_b, frame);
    CHECK(!count(&a) && !g_lg_on_n && !g_lg_n, "mode rebuild freeze clears custom hints before old widgets disappear");
    reset(); declare_pages(); g_cur = 0; legend_pump(g_b, frame);
    CHECK(!count(&a) && !g_lg_on_n, "leaving controller mode cleans the still-existing legend");

    for (int cycle = 0; cycle < 40; cycle++) {
        reset(); fill(&a, 6); declare_pages(); native_add(&a, 2, "Compare");
        dm_padmode_on_uimsg("PanelManager", "ClosePanel", "BankExpansionLayout");
        frame++; native_add(&a, 0, "Pick up"); legend_pump(g_b, frame);
        CHECK(count(&a) <= 8 && !g_lg_on_n && !overruns, "repeated bank/inventory/mercenary transitions stay within capacity");
    }
    free(g_b); printf("legend: %d checks, %d failures\n", checks, fails); return fails != 0;
}
