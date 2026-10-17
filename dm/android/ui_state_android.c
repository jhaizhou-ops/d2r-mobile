/* UI 状态机（P1）：订阅 UI 消息门（OBS_UIMSG），驱动：
 *   · scene.game（局内/局外）与面板位图 —— 共享函数 dm_tc_navigation（dm_touch_controls.c）
 *   · 面板开合栈（hud_stack_update 移植自 dm_cursor.c；只开 NpcDialogPanel/ScrollSpeechPanel 记 -1）
 *   · 面板模式 dm_tc_panel_mode（共享）→ 前端/导航态、joy_block
 * 消息解析对照 dm/host/diag.c 的 uimsg_print：x1 = 消息 {u64 类型哈希, u64 动作哈希, char* 面板名, …}，
 * 哈希 = FNV-1a 64；本状态机只关心一张已知名字表（未知名字给合成串，比较永不命中、栈里按串记录）。
 * 线程：门回调在游戏主线程；读数用原子量给触摸层（Java UI 线程）读。
 * Android UI state machine (P1): subscribes to the OBS_UIMSG gate, drives scene/panel mode like front_ios.m. */
#include "../host/hle.h"
#include "../host/dm.h"
#include "../ios/Sources/dm_touch_controls.h"
#include "ui_state_android.h"
#include <stdatomic.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static _Atomic int g_frontend = 1;      /* 启动即局外（对齐 iOS g_touch_frontend=1） | front-end at launch, like iOS */
static _Atomic int g_panel_mode;
static _Atomic unsigned g_rev;
static _Atomic int g_dead;

static int read_live(uint64_t a, void *out, size_t n) {
    if (!a || a > UINT64_MAX - n) return 0;
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)a, n};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

/* ---- 已知名字表（FNV-1a 64 → 名字；未知名合成 "#%016llx" 般的串） ---- */
static const char *const k_names[] = {
    /* 类别 */ "PanelManager", "FrontEndNavigation",
    /* 动作 */ "OpenPanel", "OpenExclusivePanel", "ClosePanel", "UnloadPanel", "TogglePanel", "LoadPanel", "UnloadAll", "HideGameInfo",
    "ToGame", "ToFrontEnd",
    /* dm_tc_navigation 的面板名单 | panel names of dm_tc_navigation */
    "PlayerInventoryExpansionLayout", "SkillsTreePanel", "PauseLayout", "PauseLayoutGarden", "CharacterStatsPanel", "QuestLogPanelExpansion",
    "BankExpansionLayout", "HoradricCubeLayout", "WaypointsPanelExpansion", "NpcDialogPanel", "NpcTradePanel", "ButtonBindingModal", "SkillSelect", "YouDiedModal",
    "LoadScreenPanel", "ScrollSpeechPanel",
    /* 常驻层（不算面板开 | resident layers, not counted as open panels） */
    "EmoteWheelPanel", "HUDLevelNames", "HUDMessagePanel", "HUDMonsterHealth", "HUDPanel", "HUDWarnings", "HireablesPanel",
    "InGameMoviePlayerPanel", "MiniMenuPanel", "PaladinAuraPanel", "PanelBordersPanel", "ShowItemsPanel", "TooltipsPanel",
    "SaveIndicatorPanel", "NotificationPanel", "PartyPanelMessage", "HelpButtonPanel", "ControllerOverlay", "UISwitcher", "AutoMap", "CandidateListPanel",
};
enum { K_N = (int)(sizeof k_names / sizeof *k_names) };
static uint64_t g_hash[K_N];
static void names_init(void) {
    static int done; if (done) return; done = 1;
    for (int i = 0; i < K_N; i++) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (const char *p = k_names[i]; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
        g_hash[i] = h;
    }
}
/* arg/cat/act 的解析缓冲：回调单线程（游戏主线程），静态可用 | single-threaded callback, static buffers are fine */
static const char *name_of(uint64_t h) {
    names_init();
    for (int i = 0; i < K_N; i++) if (g_hash[i] == h) return k_names[i];
    static char synth[4][24]; static int s;
    char *b = synth[s++ & 3];
    snprintf(b, 24, "#%016llx", (unsigned long long)h);
    return b;
}

/* ---- 面板开合栈（移植 dm_cursor.c:2415-2492；P1 精简版：去掉"读取画面撤下时按面板树核对"那一步，等 P2 接 dm_cursor 后收敛） ---- */
static int resident(const char *name) {
    static const char *const k[] = { "EmoteWheelPanel", "HUDLevelNames", "HUDMessagePanel", "HUDMonsterHealth", "HUDPanel", "HUDWarnings", "HireablesPanel",
        "InGameMoviePlayerPanel", "MiniMenuPanel", "PaladinAuraPanel", "PanelBordersPanel", "ShowItemsPanel", "TooltipsPanel",
        "SaveIndicatorPanel", "NotificationPanel", "PartyPanelMessage", "HelpButtonPanel", "ControllerOverlay", "UISwitcher", "AutoMap", "CandidateListPanel" };
    if (!name || !*name) return 0;
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strcmp(name, k[i])) return 1;
    return 0;
}
enum { HUD_STACK_MAX = 64 };
static struct { char name[64]; int open; } g_stack[HUD_STACK_MAX];
static int stack_msg(const char *act, const char *arg) {
    if (!strcmp(act, "UnloadAll")) memset(g_stack, 0, sizeof g_stack);
    else if (!strcmp(act, "UnloadPanel") && !strcmp(arg, "LoadScreenPanel")) memset(g_stack, 0, sizeof g_stack);   /* P1：整栈清零（P2 换成按面板树核对） | P1: clear all (P2 refines with the panel tree) */
    else {
        int tog = !strcmp(act, "TogglePanel");
        int open = !strcmp(act, "OpenPanel") || !strcmp(act, "OpenExclusivePanel"), close = !strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel");
        if (!open && !close && !tog) goto count;
        if (resident(arg)) goto count;
        int slot = -1; for (int i = 0; i < HUD_STACK_MAX; i++) if (!strcmp(g_stack[i].name, arg)) { slot = i; break; }
        if (slot < 0) for (int i = 0; i < HUD_STACK_MAX; i++) if (!g_stack[i].name[0]) { slot = i; strlcpy(g_stack[i].name, arg, sizeof g_stack[i].name); break; }
        if (slot < 0) goto count;
        if (tog) { open = g_stack[slot].open <= 0; close = !open; }
        if (close && g_stack[slot].open <= 0) goto count;      /* 没开着收到关闭 = 空操作 | close while not open = no-op */
        if (open && g_stack[slot].open > 0) goto count;        /* 已开着再开不重复计 | open while open does not double-count */
        g_stack[slot].open = open ? 1 : 0;
        LOG("[hud] 面板栈 %s %s → %d", act, arg, g_stack[slot].open);
    }
count:;
    int n = 0, dlg = 0;
    for (int i = 0; i < HUD_STACK_MAX; i++) { if (!strcmp(g_stack[i].name, "NpcDialogPanel") || !strcmp(g_stack[i].name, "ScrollSpeechPanel")) dlg |= g_stack[i].open > 0; else n += g_stack[i].open; }
    if (!n && dlg) n = -1;
    return n;
}

/* ---- 消息回调（对齐 front_ios.m:795-824 touch_ui_message） ---- */
static dm_tc_scene g_scene;
static void ui_message(const char *cat, const char *act, const char *arg) {
    int blocked = 0;
    if (!strcmp(cat, "PanelManager")) {
        blocked = stack_msg(act, arg);
        if ((!strcmp(act, "OpenPanel") || !strcmp(act, "OpenExclusivePanel")) && !strcmp(arg, "YouDiedModal")) atomic_store(&g_dead, 1);
        if ((!strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel")) && !strcmp(arg, "YouDiedModal")) atomic_store(&g_dead, 0);
        if (!strcmp(act, "UnloadAll")) atomic_store(&g_dead, 0);
        dm_tc_set_panel_state(atomic_load(&g_dead) ? 4 : 0);   /* DM_PS_DEAD（P1：死亡位由消息维护；其余位等 P2 的面板树泵） | DM_PS_DEAD from messages in P1 */
    }
    int nav = dm_tc_navigation(&g_scene, cat, act, arg);
    if (g_scene.game && blocked > 0) nav = 1;                  /* 局内只要还有非常驻面板开着就是面板态 | any non-resident panel open in a run = panel state */
    int nmode = dm_tc_panel_mode(nav, &g_scene, blocked);
    int fe = !g_scene.game;
    int changed = 0;
    if (atomic_exchange(&g_frontend, fe) != fe) { changed = 1; LOG("UI_STATE frontend=%d（%s:%s:%s）", fe, cat, act, arg); }
    if (atomic_exchange(&g_panel_mode, nmode) != nmode) { changed = 1; LOG("UI_STATE panel_mode=%d blocked=%d（%s:%s:%s）", nmode, blocked, cat, act, arg); }
    if (changed) atomic_fetch_add(&g_rev, 1);
}

/* ---- OBS_UIMSG 门回调：解析消息 → 本地总线（diagnostics.c 的 dm_android_uimsg → 订阅者） ---- */
void dm_android_uimsg(const char *category, const char *action, const char *argument);
static void uimsg_obs(const uint64_t *regs) {
    uint64_t msg = regs[1], f[3] = {0, 0, 0};
    if (!msg || !read_live(msg, f, sizeof f)) return;          /* 非消息调用：不输出 | not a message call: ignore */
    const char *cat = name_of(f[0]), *act = name_of(f[1]);
    char argbuf[65] = ""; const char *arg = "";
    if (f[2] && read_live(f[2], argbuf, 64)) {
        argbuf[64] = 0;
        int printable = 1; size_t k = 0;
        for (; k < 64 && argbuf[k]; k++) if ((uint8_t)argbuf[k] < 0x20 || (uint8_t)argbuf[k] >= 0x7f) { printable = 0; break; }
        if (printable && k < 64) arg = argbuf;
    }
    dm_android_uimsg(cat, act, arg);
}

void dm_android_ui_state_install(void) {
    names_init();
    gate_obs_set(OBS_UIMSG, uimsg_obs);
    void uimsg_subscribe(void (*)(const char *, const char *, const char *));
    uimsg_subscribe(ui_message);
    LOG("UI_STATE 已挂 OBS_UIMSG（前端=1 启动值）");
}

int dm_android_ui_frontend(void) { return atomic_load(&g_frontend); }
int dm_android_ui_panel_mode(void) { return atomic_load(&g_panel_mode); }
unsigned dm_android_ui_rev(void) { return atomic_load(&g_rev); }
