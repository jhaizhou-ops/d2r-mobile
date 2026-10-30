// 商人买卖、属性 / 技能加点的宿主入口。全部复用游戏手柄版自己的路径（地址为 3.3.94012 原映像偏移）：
//   买 / 卖：main+0x433e50(玩家, 物品, 0 买 / 1 卖, 0, 1 免确认, 0)——商人面板手柄键（0x40adb4 → 0x40acf4）与背包格手柄卖出（0x3cbea0）调的同一个；
//            它自己做「正在和 NPC 交互」「距上次交易 ≥500 ms」等检查，再发 0x32 / 0x33 包；钱不够 / 背包满由服务端拒。
//   价格：main+0x572ec0(玩家, 物品, 难度 u8[main+0x2d20b88], [main+0x2d5ec18], NPC 类别号 (i16)[NPC+4], 0 买 / 1 卖)。
//   当前商人：u32[main+0x2d5ebfc] = NPC 单位 id，u32[main+0x2d5ec00] ≠0 = 正在和 NPC 交互。
//   光标选中的货：界面根 [main+0x2f05458] → [+0xd0] → [+0x190] 焦点 / [+0x178] 悬停控件，格子类（虚表 main+0x2089ab8）且属主是 NPC（[控件+0x5c8]==1）→ main+0x3cc87c(控件, 0)。
//   属性加点：界面消息 CharacterStatsPanelMessage:UseSkillPoint，载荷 {i32 序号（0 力 1 敏 2 体 3 精）, i32 模式（2 = 1 点、3 = 5 点、1 = 全部）}（属性加号控件 0xc4f704 发的同一条，0xc42858 处理 → 包 0x3a）。
//   技能加点：技能树按钮控件里存着点击消息——[+0x558] 类别 SkillsTreePanelMessage、[+0x568] → i32 技能号；发 SkillsTreePanelMessage:UseSkillPoint {技能号}（0xc362ac 处理 → 包 0x3b）；
//            能不能加 = main+0xc368b0(技能号)。
// Host entry points for vendor buying/selling and stat / skill point allocation. All reuse the game's own controller-version paths (addresses are offsets in the original 3.3.94012 image):
//   Buy / sell: main+0x433e50(player, item, 0 buy / 1 sell, 0, 1 no confirmation, 0) -- the same one called by the vendor panel controller button (0x40adb4 → 0x40acf4) and the inventory cell controller sell (0x3cbea0);
//            it does the "interacting with an NPC", "≥500 ms since the last trade" and similar checks itself, then sends packet 0x32 / 0x33; not enough gold / inventory full is rejected by the server.
//   Price: main+0x572ec0(player, item, difficulty u8[main+0x2d20b88], [main+0x2d5ec18], NPC class id (i16)[NPC+4], 0 buy / 1 sell).
//   Current vendor: u32[main+0x2d5ebfc] = NPC unit id, u32[main+0x2d5ec00] ≠0 = interacting with an NPC.
//   Goods selected by the cursor: UI root [main+0x2f05458] → [+0xd0] → [+0x190] focus / [+0x178] hovered widget, of the grid class (vtable main+0x2089ab8) and owned by the NPC ([widget+0x5c8]==1) → main+0x3cc87c(widget, 0).
//   Stat point allocation: UI message CharacterStatsPanelMessage:UseSkillPoint, payload {i32 index (0 strength 1 dexterity 2 vitality 3 energy), i32 mode (2 = 1 point, 3 = 5 points, 1 = all)} (the same message sent by the stat plus widget 0xc4f704, handled by 0xc42858 → packet 0x3a).
//   Skill point allocation: the skill tree button widget stores the click message -- [+0x558] category SkillsTreePanelMessage, [+0x568] → i32 skill id; send SkillsTreePanelMessage:UseSkillPoint {skill id} (handled by 0xc362ac → packet 0x3b);
//            whether a point can be added = main+0xc368b0(skill id).
#include "dm_prof.h"
#include "dm_actions.h"
#include "dm_cursor_item.h"
#include "dm_elysium.h"
#include "dm_custom.h"
#include "dm_input.h"
#include "dm_hud.h"
#include <TargetConditionals.h>
#include <unistd.h>
#include "dm_uiscreen.h"
#include "hle.h"
#include "dm_rift.h"
#include "dm_i18n.h"
int dm_skill_cap(void);
#include "dm.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>

#define H_STATS  0x105f47414eb99892ull   // "CharacterStatsPanelMessage"
#define H_TREE   0xc50d69aa61850d12ull   // "SkillsTreePanelMessage"
#define H_USEPT  0x290e587494892995ull   // "UseSkillPoint"
static uint8_t *ab(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static dm_shop_info g_shop; static dm_points_info g_pts = { .sel_skill = -1 }; static _Atomic int g_open;
static _Atomic int g_buy, g_sell, g_stat = -1, g_skill;
void dm_shop_get(dm_shop_info *o) { pthread_mutex_lock(&g_mu); *o = g_shop; pthread_mutex_unlock(&g_mu); }
void dm_points_get(dm_points_info *o) { pthread_mutex_lock(&g_mu); *o = g_pts; pthread_mutex_unlock(&g_mu); }
int dm_shop_open(void) { return atomic_load(&g_open); }
void dm_shop_buy(void) { atomic_store(&g_buy, 1); }
void dm_shop_sell_cursor(void) { atomic_store(&g_sell, 1); }
void dm_stat_add(int stat, int n) { if (stat >= 0 && stat <= 3 && (n == 1 || n == 5 || n == -1)) atomic_store(&g_stat, stat << 8 | (n == 1 ? 2 : n == 5 ? 3 : 1)); }
void dm_skill_add_selected(void) { atomic_store(&g_skill, 1); }
static _Atomic uint32_t g_belt; static _Atomic int g_test_hold, g_test_sell;
void dm_belt_put(uint32_t item_id) { if (item_id) atomic_store(&g_belt, item_id); }
void dm_test_hold_first_sellable(void) { atomic_store(&g_test_hold, 1); }
static _Atomic int g_stow, g_stow_res = -1;
static _Atomic int g_test_swap;
void dm_test_swap_held(void) { atomic_store(&g_test_swap, 1); }   // 测试动作 A75：手上那件与背包里另一件交换（造「交换后留在手上」的现场） | test action A75: swap the held item with another item in the inventory (sets up the "stays in hand after the swap" scenario)
void dm_held_stow(void) { atomic_store(&g_stow_res, -1); atomic_store(&g_stow, 1); }
int dm_held_stow_result(void) { return atomic_load(&g_stow_res); }
void dm_test_sell_first(void) { atomic_store(&g_test_sell, 1); }
static _Atomic int g_beltx = -1, g_belt_state;   // 待执行的展开 / 收起（-1 无）；快照：低 8 位排数、位 8 展开 | pending expand / collapse (-1 none); snapshot: low 8 bits row count, bit 8 expanded
void dm_belt_expand(int on) { atomic_store(&g_beltx, on ? 1 : 0); }
static _Atomic int g_belt_use;
void dm_belt_use(int col, int row) { if (col >= 0 && col < 4 && row >= 0 && row < 4) atomic_store(&g_belt_use, 1 + row * 4 + col); }
void dm_belt_use_merc(int col, int row) { if (col >= 0 && col < 4 && row >= 0 && row < 4) atomic_store(&g_belt_use, 0x100 | (1 + row * 4 + col)); }   // 腰带双击展开后上面几排：手柄没有这几格的键，走下面 g_use 的原版喂雇佣兵分支 | the upper rows after the belt is expanded by double-tap: the controller has no button for these cells, so go through the original game's feed-mercenary branch of g_use below
static _Atomic int g_use_merc;   // 这一次 g_use 指定喂雇佣兵（腰带双击）：雇佣兵不在 / 已死 / 这瓶不能喂 = 不喂也不自己喝 | this time g_use is designated to feed the mercenary (belt double-tap): mercenary absent / dead / this bottle cannot be fed = neither feed nor drink it oneself
int dm_belt_rows(int *expanded) { int v = atomic_load(&g_belt_state); if (expanded) *expanded = v >> 8 & 1; return v & 0xff; }
static _Atomic int g_sort = -1, g_rname;
static _Atomic int g_held_equip = -1;
void dm_held_equip(int bodyloc) { if ((bodyloc & 0xff) > 0 && (bodyloc & 0xff) < 16) atomic_store(&g_held_equip, bodyloc); }   // 低 8 位 = 身体栏位；0x100 = 落点是佣兵面板里的格 | Low 8 bits = body location; 0x100 = the drop point is a cell in the mercenary panel
void dm_inv_sort(int page) { if (page == 0 || page == 3 || page == 4) atomic_store(&g_sort, page); }
void dm_name_random(void) { atomic_store(&g_rname, 12); }
int dm_name_taken(const char *name) {   // 一个角色一个目录（存档工作目录 / 角色名） | one directory per character (save working directory / character name)
    const char *dm_save_workdir(void); const char *wd = dm_save_workdir(); char pth[1200];
    if (!wd || !name || !*name || strchr(name, '/')) return 0;
    snprintf(pth, sizeof pth, "%s/%s", wd, name); return access(pth, F_OK) == 0;
}   // 值 = 还能试几次：取到的名字与已有角色重名就再取一次（见 dm_actions_pump） | value = how many more tries are left: if the name drawn duplicates an existing character's, draw again (see dm_actions_pump)
static _Atomic uint32_t g_equip, g_unequip; static _Atomic int g_test_eq;
void dm_item_equip(uint32_t id) { if (id) atomic_store(&g_equip, id); }
void dm_item_unequip(uint32_t id) { if (id) atomic_store(&g_unequip, id); }
void dm_test_equip(int what) { atomic_store(&g_test_eq, what); }   // 测试：1 = 装备背包里第一件能穿的，2 = 卸下身上第一件，3 = 对背包里第一件穿不了的调装备（应只播错误音） | test: 1 = equip the first wearable item in the inventory, 2 = unequip the first item worn, 3 = call equip on the first unwearable item in the inventory (should only play the error sound)
static _Atomic uint32_t g_ident; static _Atomic int g_tcancel, g_test_ident;
void dm_item_identify(uint32_t id) { if (id) atomic_store(&g_ident, id); }
// 免费鉴定（未鉴定物品双击）：走游戏服务端自己的鉴定函数（地址待读码），不自己改标志位
// Free identify (double-tap on an unidentified item): goes through the game server's own identify function (address to be read from the code); does not modify the flag bits itself
static _Atomic uint32_t g_ident_free;
void dm_item_identify_free(uint32_t id) { if (id) atomic_store(&g_ident_free, id); }
uint32_t dm_item_identify_free_take(void) { return atomic_exchange(&g_ident_free, 0); }   // 服务端拍取走（diag.c） | taken by the server tick (diag.c)
void dm_item_target_cancel(void) { atomic_store(&g_tcancel, 1); }
static _Atomic uint32_t g_repair;
static _Atomic int g_bfill;
void dm_belt_fill(void) { atomic_store(&g_bfill, 1); }
// 物品栏面板里可见的原版腰带控件（消息处理 main+0x3911d8）：补满的每一轮现找，不留指针——补满要跑好几轮（每 6 帧一次），
// 中间切手柄 / 触屏布局、切语言会把整套界面重建、旧控件全部释放，留着的旧指针再拿来读虚表、写进焦点就是野指针（Mac 的崩溃，宿主复现 2/2）
// The visible original belt widget in the inventory panel (message handler main+0x3911d8): looked up fresh every fill round, no pointer kept; a fill runs several rounds (one every 6 frames),
// and switching controller / touch layout or language in between rebuilds the entire UI and frees all old widgets; reusing a kept old pointer to read the vtable or write focus is a dangling pointer (the Mac crash, reproduced 2/2 on the host)
static uint64_t belt_find(uint64_t w, uint64_t h, int dep) {
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    if (*(uint64_t *)(*(uint64_t *)w + 0x28) == h) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = belt_find(*(uint64_t *)(ch + 8 * i), h, dep + 1); if (r) return r; }
    return 0;
}
static uint64_t belt_now(uint8_t *b, uint64_t root) {
    uint64_t ch = root ? *(uint64_t *)(root + 0x58) : 0, n = root ? *(uint64_t *)(root + 0x60) : 0;
    for (uint64_t i = 0; ch && n <= 256 && i < n; i++) { uint64_t p = *(uint64_t *)(ch + 8 * i); const char *nm = p ? *(const char **)(p + 8) : NULL;
        if (nm && !strcmp(nm, "PlayerInventoryExpansionLayout")) return belt_find(p, (uint64_t)(uintptr_t)(b + 0x3911d8), 0); }
    return 0;
}
void dm_item_repair(uint32_t id) { if (id) atomic_store(&g_repair, id); }
void dm_test_identify(int what) { atomic_store(&g_test_ident, what); }   // 测试：1 = 对第一件未鉴定的物品鉴定；2 = 退出等目标状态 | test: 1 = identify the first unidentified item; 2 = leave the waiting-for-target state
static _Atomic uint64_t g_take; static _Atomic int g_test_take;
void dm_stash_take_id(uint32_t id, int n) { if (id && n > 0) atomic_store(&g_take, (uint64_t)id << 32 | (uint32_t)n); }
static _Atomic int g_take_cur;
void dm_stash_take(int n) { if (n > 0) atomic_store(&g_take_cur, n); }
void dm_test_stash_take(int n) { atomic_store(&g_test_take, n); }   // 测试：对高级仓库里叠数最多的那件取 n 件 | test: take n pieces from the item with the largest stack count in the advanced stash
static _Atomic int g_cmp;
void dm_item_compare(int on) { atomic_store(&g_cmp, on ? 1 : 0); }
static _Atomic uint32_t g_use; static _Atomic int g_test_use;
void dm_item_use(uint32_t item_id) { if (item_id) atomic_store(&g_use, item_id); }
void dm_test_use_first(int kind) { atomic_store(&g_test_use, kind); }   // 测试：用背包里第一件 1 药水 / 2 卷轴 / 3 书 / 4 方块；5 = 腰带里第一件；6 = 鉴定书 / 鉴定卷轴 | test: use the first 1 potion / 2 scroll / 3 tome / 4 cube in the inventory; 5 = the first item in the belt; 6 = identify tome / identify scroll
// 立即刷新悬停：main+0x2aa480(x, y) 写手柄光标的全局坐标（main+0x2d128f8 / fc），main+0xabd6bc(焦点管理器, 0) 按这对坐标重算悬停 [+0x178]、焦点 [+0x190] 与格控件的选中格
// （游戏自己开关面板、处理鼠标键时调的同一个；第二参数 1 会播光标音效，这里传 0）。它开头自己查「手柄光标模式」两个字节（main+0x2d128f0 ≠ 0 且 0x2d128ec == 0），不满足直接返回
// Refresh the hover immediately: main+0x2aa480(x, y) writes the controller cursor's global coordinates (main+0x2d128f8 / fc), main+0xabd6bc(focus manager, 0) recomputes the hover [+0x178], the focus [+0x190] and the grid widget's selected cell from this coordinate pair
// (the same one the game itself calls when opening/closing panels and handling mouse buttons; a second argument of 1 plays the cursor sound, 0 is passed here). At its start it checks the two "controller cursor mode" bytes itself (main+0x2d128f0 ≠ 0 and 0x2d128ec == 0) and returns right away if they are not satisfied
int dm_ui_hover_now(int x, int y) {
    uint8_t *b = ab(); if (!b) return 0; uint64_t root = *(uint64_t *)(b + 0x2f05458), fm = root ? *(uint64_t *)(root + 0xd0) : 0;
    if (!fm || !*(uint8_t *)(b + 0x2d128f0) || *(uint8_t *)(b + 0x2d128ec)) return 0;
    ((void (*)(int, int))(b + 0x2aa480))(x, y); ((void (*)(uint64_t, int))(b + 0xabd6bc))(fm, 0); return 1;
}
// 测试：在画面归一化坐标处调 dm_ui_hover_now，并当场（同一次调用内）读出悬停控件与它上面的物品——验「不等下一帧，悬停已经换到这里」
// Test: call dm_ui_hover_now at normalized screen coordinates and read out, on the spot (within the same call), the hovered widget and the item on it -- verifies "without waiting for the next frame, the hover has already moved here"
void dm_test_hover_at(float nx, float ny) {
    uint8_t *b = ab(); if (!b) return; uint64_t root = *(uint64_t *)(b + 0x2f05458), fm = root ? *(uint64_t *)(root + 0xd0) : 0; if (!fm) return;
    int W = *(int32_t *)(b + 0x211da38), H = *(int32_t *)(b + 0x211da3c), x = (int)(nx * W), y = (int)(ny * H);   // 入参已是界面矩形里的归一化坐标（调用方换算，见 hle_hid.c 的 DM_UI_TAPS） | the arguments are already normalized coordinates within the UI rectangle (converted by the caller, see DM_UI_TAPS in hle_hid.c)
    uint64_t f0 = *(uint64_t *)(fm + 0x190); int ok = dm_ui_hover_now(x, y); uint64_t h1 = *(uint64_t *)(fm + 0x178), f1 = *(uint64_t *)(fm + 0x190);
    uint64_t it = f1 && *(uint64_t *)f1 - (uint64_t)(uintptr_t)b == 0x2089ab8 ? ((uint64_t (*)(uint64_t, void *))(b + 0x3cc87c))(f1, NULL) : 0; uint32_t code = it ? ((uint32_t (*)(uint64_t))(b + 0x570260))(it) : 0;
    LOG("测试：hover_now (%d,%d) → %d；焦点 %s → %s，悬停 %s，选中格 %d,%d，物品 id %u（%.3s）", x, y, ok, f0 && *(const char **)(f0 + 8) ? *(const char **)(f0 + 8) : "-", f1 && *(const char **)(f1 + 8) ? *(const char **)(f1 + 8) : "-",
        h1 && *(const char **)(h1 + 8) ? *(const char **)(h1 + 8) : "-", f1 ? *(int32_t *)(f1 + 0x544) : -1, f1 ? *(int32_t *)(f1 + 0x548) : -1, it ? *(uint32_t *)(it + 8) : 0, (const char *)&code);
}
static _Atomic int g_test_belt;
void dm_test_belt_first_potion(void) { atomic_store(&g_test_belt, 1); }

static uint64_t a_find(uint64_t w, const char *name, int dep) {   // 按名字找控件：名字 +8、子控件数组 +0x58、个数 +0x60 | find a widget by name: name +8, child widget array +0x58, count +0x60
    if (!w || dep > 14) return 0; const char *nm = *(const char **)(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = a_find(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
// 按名字找面板：只看界面根（PanelManager）的直接子控件（同 dm_cursor.c ui_panel）。每次从界面根现读，不留控件指针过帧
// Find a panel by name: only looks at the direct child widgets of the UI root (PanelManager) (same as dm_cursor.c ui_panel). Reads from the UI root fresh every time; no widget pointer is kept across frames
static uint64_t a_panel(uint64_t root, const char *name) {
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t p = *(uint64_t *)(ch + 8 * i); const char *nm = p ? *(const char **)(p + 8) : NULL; if (nm && !strcmp(nm, name)) return p; }
    return 0;
}
// 玩家某技能的基础等级：技能链表 [玩家+0x100] → [0] 首节点；节点 [0] → i16 技能号、+0x44 == -1（不是装备给的）、+0x38 等级、+8 下一个（同 0xc36954–0xc369c4）
// Base level of one of the player's skills: skill linked list [player+0x100] → [0] first node; node [0] → i16 skill id, +0x44 == -1 (not granted by equipment), +0x38 level, +8 next (same as 0xc36954–0xc369c4)
static int a_skill_base(uint64_t pl, int skill) {
    uint64_t l = *(uint64_t *)(pl + 0x100), n = l ? *(uint64_t *)l : 0;
    for (int k = 0; n && k < 512; k++, n = *(uint64_t *)(n + 8)) { uint64_t r = *(uint64_t *)n; if (r && *(int16_t *)r == skill && *(int32_t *)(n + 0x44) == -1) { int v = *(int32_t *)(n + 0x38); return v < 0 ? 0 : v; } }
    return 0;
}
// 原版技能单独洗点 main+0x7c0f90(game, player)：遍历职业技能、返还硬点、发客户端技能包，清理召唤与被动。
// 已核对 3.3.94012 的调用链：7c0f90 → 7c11b0 → 5af180；不走属性洗点，不耗道具或任务次数。
// Native skill-only reset main+0x7c0f90(game, player): iterates class skills, refunds hard points, sends client packets and refreshes summons/passives.
// Verified 3.3.94012 call chain: 7c0f90 -> 7c11b0 -> 5af180; no attribute reset, item cost or quest use.
#include "dm_skill_reset.h"
static _Atomic int g_reset_event, g_reset_result;
static _Atomic uint64_t g_reset_session, g_reset_request;
static dm_skill_reset_flow g_reset_flow;
void dm_skill_reset_on_uimsg(const char *cat, const char *act, const char *arg) {
    if (strcmp(cat, "PanelManager")) return;
    // 关闭 / 读档立即撤销确认，避免两个渲染帧之间关页再开页沿用旧确认。
    // Revoke confirmation on close/load, even if the page closes and reopens between rendered frames.
    if ((!strcmp(act, "ClosePanel") && (!strcmp(arg, "UISwitcher") || !strcmp(arg, "SkillsTreePanel"))) ||
        (!strcmp(act, "OpenPanel") && !strcmp(arg, "LoadScreenPanel"))) {
        g_reset_flow = (dm_skill_reset_flow){0}; atomic_store(&g_reset_event, 0);
        if (!strcmp(arg, "LoadScreenPanel")) { atomic_store(&g_reset_request, 0); atomic_store(&g_reset_session, 0); }
        return;
    }
    if (strcmp(act, "ClosePanel")) return;
    int e = !strcmp(arg, "TouchSkillReset") ? 1 : !strcmp(arg, "TouchSkillResetYes") ? 2 : !strcmp(arg, "TouchSkillResetNo") ? 3 : 0;
    if (e) atomic_store(&g_reset_event, e);
}
static int reset_total(uint8_t *b, uint64_t pl) {
    unsigned cls = *(uint32_t *)(pl + 4), ver = *(uint8_t *)(pl + 0x1bd);
    if (cls > 7 || ver < 1 || ver > 3) return -1;
    uint64_t t = *(uint64_t *)(b + 0x2d64a18 + ver * 16);
    if (!t) return -1;
    uint64_t counts = *(uint64_t *)(t + 0x1208), ids = *(uint64_t *)(t + 0x1228), limits = *(uint64_t *)(t + 0x1500);
    int stride = *(int32_t *)(t + 0x1220);
    if (!counts || !ids || !limits) return -1;
    int n = ((int32_t *)counts)[cls], cap = *(int32_t *)limits, levels[64];
    if (n < 1 || n > 64 || stride < n || stride > 64 || cap < 1) return -1;
    for (int i = 0; i < n; i++) {
        int skill = ((int16_t *)ids)[cls * stride + i];
        if (skill < 0 || (uint64_t)skill >= *(uint64_t *)(t + 0x11f0)) return -1;
        levels[i] = a_skill_base(pl, skill);
        if (levels[i] > cap) return -1;
    }
    int points = ((int (*)(uint64_t, int, int))(b + 0x5b9110))(pl, 5, 0);
    return dm_skill_reset_total(points, levels, (size_t)n);
}
void dm_skill_reset_srv_tick(uint8_t *b, uint64_t game, uint64_t player) {
    static uint64_t old_game, old_player; static uint32_t epoch;
    if (game != old_game || player != old_player) { old_game = game; old_player = player; if (!++epoch) ++epoch; }
    uint64_t token = player && game ? ((uint64_t)(epoch ? epoch : 1) << 32) | *(uint32_t *)(player + 8) : 0;
    atomic_store(&g_reset_session, token);
    uint64_t req = atomic_exchange(&g_reset_request, 0);
    if (!req) return;
    if (!token || req != token || *(uint32_t *)(b + 0x2d20b8c) != 0 || *(uint32_t *)(player + 0xc) == 0 || *(uint32_t *)(player + 0xc) == 17) {
        atomic_store(&g_reset_result, 3); return;
    }
    int total = reset_total(b, player);
    if (total < 0) { atomic_store(&g_reset_result, 2); return; }
    int before = ((int (*)(uint64_t, int, int))(b + 0x5b9110))(player, 5, 0);
    if (total == before) { atomic_store(&g_reset_result, 4); return; }
    ((void (*)(uint64_t, uint64_t))(b + 0x7c0f90))(game, player);
    int after = ((int (*)(uint64_t, int, int))(b + 0x5b9110))(player, 5, 0);
    LOG("Skill reset: unspent %d -> %d (expected %d)", before, after, total);
    atomic_store(&g_reset_result, after == total ? 1 : 3);
}
static void ui_show_ours(uint64_t w, int on) {
    const char *nm = w ? *(const char **)(w + 8) : NULL;
    if (!nm || strncmp(nm, "TouchSkillReset", 15)) return;
    *(uint8_t *)(w + 0x50) = (uint8_t)on; *(uint8_t *)(w + 0x51) = (uint8_t)on;
}
static void skill_reset_ui(uint8_t *b, uint64_t root, uint64_t pl) {
    uint64_t tree = a_panel(root, "SkillsTreePanel");
    int open = tree && *(uint8_t *)(tree + 0x50), allowed = pl && *(uint32_t *)(b + 0x2d20b8c) == 0;
    uint64_t token = atomic_load(&g_reset_session);
    allowed = allowed && token && (uint32_t)token == *(uint32_t *)(pl + 8);
    int event = atomic_exchange(&g_reset_event, 0);
    if (dm_skill_reset_event(&g_reset_flow, event, allowed ? token : 0, open, allowed)) {
        atomic_store(&g_reset_request, token);
        // 关技能树后原版才恢复服务端拍；请求绑定当前游戏 / 玩家，不带到下一局。
        // Closing the tree resumes server ticks; the request is bound to this game/player, never the next run.
        void dm_ui_send(const char *, const char *, const char *);
        dm_ui_send("PanelManager", "ClosePanel", "UISwitcher");
        return;
    }
    ui_show_ours(a_find(tree, "TouchSkillReset", 0), open && allowed && !g_reset_flow.armed);
    ui_show_ours(a_find(tree, "TouchSkillResetYes", 0), open && g_reset_flow.armed);
    ui_show_ours(a_find(tree, "TouchSkillResetNo", 0), open && g_reset_flow.armed);
    ui_show_ours(a_find(tree, "TouchSkillResetHint", 0), open && g_reset_flow.armed);
    int result = atomic_exchange(&g_reset_result, 0);
    if (result) {
        void dm_game_notice(uint8_t *, const char *);
        const char *key = result == 1 ? "dmSkillResetDone" : result == 2 ? "dmSkillResetFull" : result == 4 ? "dmSkillResetEmpty" : "dmSkillResetUnavailable";
        dm_game_notice(b, dm_str(b, key));
    }
}

// 还能不能给技能加点、不能的原因。游戏的 main+0xc368b0 只查「有技能点、公式、没到上限」，不查等级和前置技能（等级不够时它返回 1，点了由服务端拒，实测）；
// 这两项按技能表补：记录（步长 0x2ec，表在 [版本表+0x11e8]）+0x214 = 等级要求、+0x21e / +0x220 / +0x222 = 前置技能号（-1 无）（运行时对 Vigor 读出 18 / 109 / 104，与 skills.txt 一致）；
// 加第 N 点要求角色等级 ≥ 等级要求 + 已有基础等级（暗黑 2 规则；界面「需要等级」显示的就是它）。返回 0 可加，1 没有技能点，2 等级不够，3 前置技能没学，4 其它（已到上限等）
// Whether a point can still be added to a skill, and the reason if not. The game's main+0xc368b0 only checks "has skill points, formula, not at the cap"; it does not check level or prerequisite skills (when the level is too low it returns 1 and the click is rejected by the server, measured);
// these two are filled in from the skill table: record (stride 0x2ec, table at [version table+0x11e8]) +0x214 = level requirement, +0x21e / +0x220 / +0x222 = prerequisite skill ids (-1 none) (at runtime Vigor reads 18 / 109 / 104, matching skills.txt);
// adding the Nth point requires character level ≥ level requirement + existing base level (Diablo 2 rule; this is what the UI's 「需要等级」(Required Level) shows). Returns 0 can add, 1 no skill points, 2 level too low, 3 prerequisite skill not learned, 4 other (already at the cap, etc.)
static int a_skill_block(uint8_t *b, uint64_t pl, int skill, int points) {
    if (points < 1) return 1;
    uint8_t ver = *(uint8_t *)(pl + 0x1bd); if (ver < 1 || ver > 3 || skill < 0) return 4;
    uint64_t t = *(uint64_t *)(b + 0x2d64a18 + (uint64_t)ver * 16); if ((uint64_t)skill >= *(uint64_t *)(t + 0x11f0)) return 4;
    uint64_t rec = *(uint64_t *)(t + 0x11e8) + (uint64_t)skill * 0x2ec;
    if (((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 12, 0) < *(int16_t *)(rec + 0x214) + a_skill_base(pl, skill)) return 2;
    for (int i = 0; i < 3; i++) { int q = *(int16_t *)(rec + 0x21e + 2 * i); if (q >= 0 && a_skill_base(pl, q) < 1) return 3; }
    if (((int (*)(int))(b + 0xc368b0))(skill) & 1) return 0;
    // 5：卡在「没打通过秘境、已到原版上限 20」——这一格本来是 20、解锁后会放到 40 的技能，基础等级已到 20
    // 5: stuck at "no Greater Rift cleared, already at the original cap of 20": this slot holds a skill whose cap was 20 and becomes 40 once unlocked, and its base level is already 20
    if (dm_skill_cap() > 20 && !dm_rift_cleared_any() && a_skill_base(pl, skill) >= 20 && a_skill_base(pl, skill) < dm_skill_cap() && *(int16_t *)(uintptr_t)(rec + 0x184) == 20) return 5;
    return 4;
}
// 技能硬点上限 20 → 40（「硬点放到 40 级上限吧」，秘境里人物能力要跟上怪物成长）。
// 上限在哪判（读码）：客户端「能不能加」main+0xc368b0（0xc36a64–0xc36a90）与服务端收加点包 0x3b main+0x6db600（0x6db770–0x6db798）读的都是
// 技能表记录 +0x184（int16，skills.txt 的 maxlvl 列）；两处写死的 20（mov #0x14）只是这一格 < 1 时的缺省。所以改表就够，不用改代码：
// 数据表装好后把每个版本表里 maxlvl 正好是 20 的技能改成 40（客户端与服务端同进程、同一份表）。别的上限值（1 级技能、不可加点的 0）不动。
// 基础等级读数另有一个全局钳值 *[数据表 + 0x1500]（两处都先 min 一下），值在首次改表时记进日志。
// 上限是可调的数（「太强了就压到 30 级硬顶。这个咱们都可调」）：缺省 SKILL_CAP，环境变量 DM_SKILL_CAP=<21–99> 覆盖（=20 等于不改表，照原版）
// Hard skill point cap 20 → 40 ("let's raise hard points to a cap of level 40"; in the Rift the character's power has to keep up with monster growth).
// Where the cap is checked (from reading the code): both the client's "can a point be added" check at main+0xc368b0 (0xc36a64–0xc36a90) and the server's
// handler for the add-point packet 0x3b at main+0x6db600 (0x6db770–0x6db798) read
// skill table record +0x184 (int16, the maxlvl column of skills.txt); the hard-coded 20 at both sites (mov #0x14) is only the default when this field is
// < 1. So changing the table is enough, with no code change:
// once the data tables are loaded, every skill whose maxlvl is exactly 20 in each version's table is changed to 40 (client and server share one process
// and one copy of the table). Other cap values (level 1 skills, 0 for skills that cannot take points) are left alone.
// The base level reading also has a global clamp *[data table + 0x1500] (both sites apply min with it first); its value is logged the first time the
// table is changed.
// The cap is a tunable number ("if it's too strong, clamp it to a hard cap of level 30. All of this is adjustable"): default SKILL_CAP, overridden by
// the environment variable DM_SKILL_CAP=<21–99> (=20 means the table is not changed, same as the original game)
#define SKILL_CAP 40
int dm_skill_cap(void) { static int c; if (!c) { const char *e = getenv("DM_SKILL_CAP"); c = e ? atoi(e) : SKILL_CAP; if (c < 20 || c > 99) c = SKILL_CAP; } return c; }
// 解锁条件：「做成首次打通任意一次秘境以后开启技能硬点上限解锁。之前已有存档和状态不用管。」——这个角色限时内打通过任意一层（dm_rift_cleared_any，
// 就是秘境记录里的最高通关层 ≥ 1，随角色档同一次提交）才把表改成 40；没打通过是原版的 20。表是全局的、客户端与服务端共用，所以每帧按「当前角色」对一次，换角色跟着换。
// 不迁移不回收：没解锁的老角色已经点过 20 的技能点数原样留着（表只管「还能不能再加」），只是不能再往上加。
// Unlock condition: "Once any Greater Rift has been cleared for the first time, unlock the hard skill point cap. Existing saves and state need no handling." Only when this character has cleared any level within the time limit (dm_rift_cleared_any,
// i.e. highest cleared level in the Greater Rift record ≥ 1, committed together with the character save) is the table changed to 40; otherwise it stays at the original 20. The table is global and shared by client and server, so it is synced to "the current character" every frame and follows character switches.
// No migration, no refund: skill points that an old character without the unlock has already put in up to 20 stay as they are (the table only governs "can more be added"); they just cannot go higher.
static void skill_cap_pump(uint8_t *b) {
    static uint64_t seen[4]; static int applied[4]; static uint8_t ours[4][512]; int cap = dm_skill_cap();   // ours：这个版本表里原本正好是 20、归我们改的那些技能（位图） | ours: the skills whose entry in this version's table was originally exactly 20 and that we change (bitmap)
    if (cap == 20) return;
    int want = dm_rift_cleared_any() ? cap : 20;
    for (int v = 1; v <= 3; v++) { uint64_t t = *(uint64_t *)(b + 0x2d64a18 + (uint64_t)v * 16); if (!t || (seen[v] == t && applied[v] == want)) continue;
        uint64_t recs = *(uint64_t *)(t + 0x11e8), n = *(uint64_t *)(t + 0x11f0), gp = *(uint64_t *)(t + 0x1500); if (!recs || !n || n > 4096) continue;
        if (seen[v] != t) { int ch = 0, other = 0; memset(ours[v], 0, sizeof ours[v]);
            for (uint64_t i = 0; i < n; i++) { int16_t m = *(int16_t *)(uintptr_t)(recs + i * 0x2ec + 0x184); if (m == 20) { ours[v][i >> 3] |= (uint8_t)(1 << (i & 7)); ch++; } else if (m > 0) other++; }
            seen[v] = t; applied[v] = 20; LOG("技能上限：数据表版本 %d 共 %llu 个技能，%d 个的硬点上限是 20（解锁后改成 %d；另有 %d 个上限不是 20 的不动）；基础等级读数的全局钳值 %d", v, (unsigned long long)n, ch, cap, other, gp ? *(int32_t *)(uintptr_t)gp : -1); }
        if (applied[v] != want) { int ch = 0; for (uint64_t i = 0; i < n; i++) if (ours[v][i >> 3] & (1 << (i & 7))) { *(int16_t *)(uintptr_t)(recs + i * 0x2ec + 0x184) = (int16_t)want; ch++; }
            applied[v] = want; LOG("技能上限：数据表版本 %d 的 %d 个技能硬点上限 → %d（这个角色%s）", v, ch, want, want == 20 ? "还没打通过秘境" : "打通过秘境"); } }
}
static void a_send(uint8_t *b, uint64_t root, uint64_t cat, uint64_t act, int32_t p0, int32_t p1) {   // 给界面根发消息（虚表 +0x28） | send a message to the UI root (vtable +0x28)
    static int32_t payload[4]; static uint64_t msg[8]; payload[0] = p0; payload[1] = p1; memset(msg, 0, sizeof msg); msg[0] = cat; msg[1] = act; msg[2] = (uint64_t)(uintptr_t)payload;
    (void)b; ((void (*)(uint64_t, void *))*(uint64_t *)(*(uint64_t *)root + 0x28))(root, msg);
}
// 自动喝药：生命按两档（见下面 ap_life_pick：低于 50% 红药、低于 30% 紫药，没有紫药再喝一瓶红药），法力 < 上限 30% 喝蓝药（mp1–mp5）——先背包（页 0）、背包没有再腰带，城里也喝。
// 生命 / 法力读游戏自己的取属性函数 main+0x5b9290(单位, 属性号, 0)（含装备 / 护身符加成，角色页同口径），6 / 7 生命、8 / 9 法力，定点 ×256。
// 喝走 g_use 那条原版路径（腰带格分支 0x2cecf0 / 背包 0x3c0098，都自己组 0x26 包），不模拟点击。
// 不连喝：以游戏自己的「药效还在」为准，药效没了才再判——
//   法力：属性 26 manarecovery（misc.txt mp* 的 stat1），喝下后下一拍起非 0，回满或到期归 0（本机 拍 31 喝 → 拍 32 起 = 100 → 拍 52 回满归 0）；
//   生命：状态 100 healthpot（misc.txt hp* 的 state）。不用它的属性 74 hpregen：装备的「生命回复」也记在 74 上，戴着就一直非 0。
//   状态位：[单位+0x88] 属性表（+0x1c 最高位 = 扩展表）+0xb30 起按 状态号 / 32 一个字的位图（游戏自己读法见 main+0x6240f4，置位见 0x5bd510）。
// 发出喝药后等药效出现，1 秒（25 拍）没出现算这次没喝成，再按数值重判。两种同时低时先喝红。
// 开关：「自定义」设置 auto_mana（dm_custom.c，键名沿用只管蓝药时的旧名，现管红蓝两种），缺省开
// Auto potion: life uses two tiers (see ap_life_pick below: red potion below 50%, purple potion below 30%, one more red potion if there is no purple), mana < 30% of the maximum drinks a blue potion (mp1–mp5) -- inventory first (page 0), and the belt if the inventory has none; also drinks in town.
// Life / mana are read with the game's own stat getter function main+0x5b9290(unit, stat id, 0) (including equipment / charm bonuses, same definition as the character page), 6 / 7 life, 8 / 9 mana, fixed point ×256.
// Drinking goes through the original g_use path (belt-cell branch 0x2cecf0 / inventory 0x3c0098, both build the 0x26 packet themselves), no click simulation.
// No chain drinking: the game's own "potion effect still active" is authoritative, and it is judged again only after the effect is gone --
//   mana: stat 26 manarecovery (stat1 of mp* in misc.txt), non-zero from the tick after drinking, back to 0 when full or expired (on this machine: drink at tick 31 → = 100 from tick 32 → full and back to 0 at tick 52);
//   life: state 100 healthpot (state of hp* in misc.txt). Its stat 74 hpregen is not used: the "life regeneration" of equipment is also recorded on 74, so it stays non-zero as long as such gear is worn.
//   State bits: [unit+0x88] stat table (top bit of +0x1c = extended table), a bitmap from +0xb30 with one word per state id / 32 (for how the game itself reads it see main+0x6240f4, for setting see 0x5bd510).
// After a drink is sent, wait for the effect to appear; if it has not appeared within 1 second (25 ticks) this drink counts as failed, and it is judged again by the values. When both are low, red is drunk first.
// Switch: the 「自定义」(Custom) setting auto_mana (dm_custom.c; the key name keeps the old name from when it only handled blue potions and now covers both red and blue), on by default
uint32_t dm_cur_tick(void);
static int a_has_state(uint64_t u, int st) {
    uint64_t sl = *(uint64_t *)(u + 0x88); if (!sl || st < 0 || st > 0x1fe || !(*(uint32_t *)(sl + 0x1c) >> 31)) return 0;
    return (*(uint32_t *)(sl + 0xb30 + 4 * (uint64_t)(st >> 5)) >> (st & 31)) & 1;
}
// 生命两档（人物与佣兵同一套）：低于 50% 喝红药（药效在就不喝，不连灌）；低于 30% 喝紫药（瞬回，没有药效状态：喝完 1 秒仍低于 30% 才再喝），
// 没有紫药就再喝一瓶红药——这一瓶不受「药效还在」挡，但每次掉到 30% 以下只放行一瓶，之后回到「药效没了才喝」，回到 30% 以上重新计（挨打时不把红药灌光）。
// 两个阈值可调：DM_AUTOPOT_RED（缺省 50）/ DM_AUTOPOT_REJUV（缺省 30），百分数。法力仍是低于 30% 喝蓝药。
// The two life tiers (the same set for the character and the mercenary): below 50% drink a red potion (not while its effect is active, no chain drinking); below 30% drink a purple potion (instant, with no effect state: drink again only if still below 30% 1 second after drinking),
// and with no purple potion drink one more red potion -- this one is not blocked by "effect still active", but only one is let through per drop below 30%; after that it goes back to "drink only once the effect is gone", and the count restarts once life is back above 30% (so the red potions are not all poured down while taking hits).
// Both thresholds are tunable: DM_AUTOPOT_RED (default 50) / DM_AUTOPOT_REJUV (default 30), in percent. Mana is still: below 30% drink a blue potion.
static int ap_pct(int which) { static int v[2] = { -1, -1 }; static const char *const nm[2] = { "DM_AUTOPOT_RED", "DM_AUTOPOT_REJUV" }; static const int def[2] = { 50, 30 };
    if (v[which] < 0) { const char *e = getenv(nm[which]); v[which] = e && atoi(e) > 0 && atoi(e) <= 100 ? atoi(e) : def[which]; } return v[which]; }
// 在玩家物品栏里找一瓶药：kind 0 红（hp1–hp5）/ 1 蓝（mp1–mp5）/ 2 紫（rvs / rvl）。先背包（页 0）、再腰带最下面一排（原版按腰带键喝的就是这一排）。返回物品 id，没有 = 0
// Find a potion among the player's items: kind 0 red (hp1–hp5) / 1 blue (mp1–mp5) / 2 purple (rvs / rvl). Inventory first (page 0), then the bottom row of the belt (the row the original game drinks from when a belt key is pressed). Returns the item id, none = 0
static uint32_t ap_find(uint8_t *b, uint64_t pl, int kind, uint32_t *ocode, int *in_bag) {
    uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; uint32_t bid = 0, iid = 0, brow = 99, bc = 0, ic = 0;
    for (int k = 0; it && k < 512; k++) { uint64_t d = *(uint64_t *)(it + 0x10), pp = *(uint64_t *)(it + 0x38); if (!d) break;
        uint32_t code = ((uint32_t (*)(uint64_t))(b + 0x570260))(it), cont = *(uint32_t *)(it + 0xc), c3 = code & 0xffffff; int c2 = code >> 16 & 0xff;
        int ok = kind == 2 ? (c3 == 0x737672 || c3 == 0x6c7672) : ((code & 0xffff) == (kind ? 0x706du : 0x7068u) && c2 >= '1' && c2 <= '5');   // "rvs" / "rvl"；"hp" / "mp" + 1–5
        if (ok) { if (cont == 2 && pp && *(uint32_t *)(pp + 0x14) < brow) { brow = *(uint32_t *)(pp + 0x14); bid = *(uint32_t *)(it + 8); bc = code; }
                  else if (cont == 0 && *(uint8_t *)(d + 0x55) == 0 && !iid) { iid = *(uint32_t *)(it + 8); ic = code; } }
        it = *(uint64_t *)(d + 0xb0); }
    *ocode = iid ? ic : bc; *in_bag = iid != 0; return iid ? iid : bid;
}
typedef struct { uint32_t last; int warned, extra; } ap_life;   // last：上次发出的拍；extra：这次掉到 30% 以下已经放行过那一瓶不看药效的红药 | last: the tick of the last send; extra: on this drop below 30%, the one red potion that ignores the potion effect has already been let through
// 按生命两档挑药。v / mx 生命与上限，healing = 红药药效还在。返回要用的物品 id（0 = 这一拍不喝），*why 给日志
// Pick a potion by the two life tiers. v / mx are life and its maximum, healing = the red potion effect is still active. Returns the id of the item to use (0 = no drink on this tick); *why is for the log
static uint32_t ap_life_pick(uint8_t *b, uint64_t pl, ap_life *s, int v, int mx, int healing, uint32_t t, uint32_t *code, int *in_bag, const char **why) {
    if (mx <= 0 || v <= 0) return 0;
    int low = (long long)v * 100 < (long long)mx * ap_pct(1), mid = (long long)v * 100 < (long long)mx * ap_pct(0);
    if (!low) s->extra = 0;
    if (!mid) { s->warned = 0; if (healing) s->last = 0; return 0; }
    if (s->last && t >= s->last && t - s->last < 25) return 0;   // 刚发过一瓶：等 1 秒（红药等药效出现、紫药等生命跳上去） | one was just sent: wait 1 second (for a red potion, until the effect appears; for a purple potion, until life jumps up)
    if (low) { uint32_t id = ap_find(b, pl, 2, code, in_bag); if (id) { *why = "紫药"; return id; }
        if (healing && s->extra) return 0;   // 没有紫药、那一瓶额外的红已经喝过：等药效没了 | no purple potion and the one extra red has already been drunk: wait for the potion effect to end
        id = ap_find(b, pl, 0, code, in_bag); if (id) { if (healing) s->extra = 1; *why = healing ? "没有紫药，再喝一瓶红药" : "没有紫药，喝红药"; return id; }
        if (!s->warned) { s->warned = 1; *why = "没药"; } return 0; }
    if (healing) return 0;   // 50% 档：药效还在不再喝 | 50% tier: do not drink again while the potion effect is still active
    { uint32_t id = ap_find(b, pl, 0, code, in_bag); if (id) { *why = "红药"; return id; } if (!s->warned) { s->warned = 1; *why = "没药"; } return 0; }
}
static int auto_pot_life(uint8_t *b, uint64_t pl, uint32_t t) {   // 人物生命；返回 1 = 这一拍发了喝药 | character life; returns 1 = a drink was sent on this tick
    static ap_life s; int (*gs)(uint64_t, int, int) = (void *)(b + 0x5b9290); int v = gs(pl, 6, 0) >> 8, mx = gs(pl, 7, 0) >> 8, bag = 0; uint32_t code = 0; const char *why = NULL;
    uint32_t id = ap_life_pick(b, pl, &s, v, mx, a_has_state(pl, 100), t, &code, &bag, &why);
    if (!id) { if (why) LOG("自动喝药：生命 %d/%d 低于 %d%%，背包和腰带里都没有可喝的生命药水", v, mx, ap_pct(0)); return 0; }
    atomic_store(&g_use, id); s.last = t; s.warned = 0;
    LOG("自动喝药：拍 %u 生命 %d/%d（%d%%）→ %s：喝%s的 %.3s（id %u）", t, v, mx, (int)((long long)v * 100 / mx), why, bag ? "背包" : "腰带", (const char *)&code, id);
    return 1;
}
static int auto_pot_mana(uint8_t *b, uint64_t pl, uint32_t t) {   // 法力：低于 30% 喝蓝药，药效（属性 26）在就不喝；返回 1 = 这一拍发了喝药 | mana: below 30% drink a blue potion, not while the potion effect (stat 26) is present; returns 1 = a drink was sent on this tick
    static uint32_t last; static int warned; int (*gs)(uint64_t, int, int) = (void *)(b + 0x5b9290);
    if (gs(pl, 26, 0)) { last = 0; return 0; }
    if (last && t >= last && t - last < 25) return 0;
    int v = gs(pl, 8, 0) >> 8, mx = gs(pl, 9, 0) >> 8, bag = 0; uint32_t code = 0;
    if (mx <= 0 || v * 10 >= mx * 3) { warned = 0; return 0; }
    uint32_t id = ap_find(b, pl, 1, &code, &bag);
    if (!id) { if (!warned) { warned = 1; LOG("自动喝药：法力 %d/%d 低于 30%%，背包和腰带里都没有法力药水", v, mx); } return 0; }
    atomic_store(&g_use, id); last = t; warned = 0;
    LOG("自动喝药：拍 %u 法力 %d/%d 低于 30%% → 喝%s的 %.3s（id %u）", t, v, mx, bag ? "背包" : "腰带", (const char *)&code, id);
    return 1;
}
// 佣兵自动喂药：同上面的生命两档；玩家自己生命低于 30% 那一档时先不喂（药先紧着玩家）。
// 走现成的喂佣兵路径（g_use + g_use_merc，同腰带双击：原版自己判这瓶能不能喂、目标 = 佣兵单位）。开关：「自定义」设置 merc_pot，缺省关（会多耗玩家的药）。
// 佣兵单位 id = [0x2d33528] 链表里类型 7、属主 = 玩家、[+0x20] == 0 的那条（同下面喂佣兵处）；生命与药效读服务端那只的真值（diag.c dm_diag_srv_merc）——
// 客户端那只的生命是服务端发来的比例，进局头两百多拍没发过、是没初始化的值，不能拿来决定消耗玩家的药
// Mercenary auto-feed potions: the same two life tiers as above; while the player's own life is in the below-30% tier, do not feed yet (potions go to the player first).
// Goes through the existing feed-mercenary path (g_use + g_use_merc, same as the belt double-tap: the original game itself judges whether this potion can be fed, target = the mercenary unit). Switch: the 「自定义」(Custom) setting merc_pot, off by default (it uses up more of the player's potions).
// Mercenary unit id = the entry in the [0x2d33528] linked list with type 7, owner = the player, and [+0x20] == 0 (same as the feed-mercenary code below); life and potion effect are read from the true values of the server-side one (diag.c dm_diag_srv_merc) --
// the client-side one's life is a ratio sent by the server, which has not been sent during the first two-hundred-odd ticks after joining a game and is an uninitialized value, so it must not be used to decide to consume the player's potions
static int auto_merc_pot(uint8_t *b, uint64_t pl, uint32_t t) {
    static int env = -2; if (env == -2) { const char *e = getenv("DM_MERC_POT"); env = e ? e[0] != '0' : -1; }   // 测试强制：DM_MERC_POT=0 关 / 其余开，优先于自定义页 | Test override: DM_MERC_POT=0 off / anything else on, takes priority over the Custom page
    if (!(env >= 0 ? env : dm_custom_get("merc_pot"))) return 0;
    static ap_life s; int (*gs)(uint64_t, int, int) = (void *)(b + 0x5b9290);
    { int v = gs(pl, 6, 0), mx = gs(pl, 7, 0); if (mx <= 0 || (long long)v * 100 < (long long)mx * ap_pct(1)) return 0; }   // 玩家自己缺血：药先紧着玩家 | The player is low on life: potions go to the player first
    uint32_t mid = 0xffffffffu; for (uint64_t e = *(uint64_t *)(b + 0x2d33528); e; e = *(uint64_t *)(e + 0x30)) if (*(uint32_t *)(e + 4) == 7 && *(uint32_t *)(e + 0xc) == *(uint32_t *)(pl + 8) && !*(uint32_t *)(e + 0x20)) { mid = *(uint32_t *)(e + 8); break; }
    int dm_diag_srv_merc(uint32_t id, int *hp, int *mx, int *healing); int v = 0, mx = 0, healing = 0, bag = 0; uint32_t code = 0; const char *why = NULL;
    if (mid == 0xffffffffu || !dm_diag_srv_merc(mid, &v, &mx, &healing)) return 0;   // 没有佣兵 / 死了 / 服务端没有这只 | No mercenary / dead / the server has no such unit
    uint32_t id = ap_life_pick(b, pl, &s, v, mx, healing, t, &code, &bag, &why);
    if (!id) { if (why) LOG("佣兵自动喂药：佣兵生命 %d%% 低于 %d%%，背包和腰带里都没有可喂的生命药水", (int)((long long)v * 100 / mx), ap_pct(0)); return 0; }
    atomic_store(&g_use_merc, 1); atomic_store(&g_use, id); s.last = t; s.warned = 0;
    LOG("佣兵自动喂药：拍 %u 佣兵（id %u）生命 %d/%d（%d%%）→ %s：喂%s的 %.3s（id %u）", t, mid, v, mx, (int)((long long)v * 100 / mx), why, bag ? "背包" : "腰带", (const char *)&code, id);
    return 1;
}
static void auto_pot_pump(uint8_t *b, uint64_t pl) {
    if (!pl) return;
    uint32_t t = dm_cur_tick(); if (!t) return;
    uint32_t mode = *(uint32_t *)(pl + 0xc); if (mode == 0 || mode == 17 || mode == 0x11) return;   // 死亡中 / 已死 / 玩家状态不允许用物品（同 g_use 的挡法） | Dying / dead / the player state does not allow using items (same blocking as g_use)
    if (((uint64_t (*)(void))(b + 0x2aa7c0))() || atomic_load(&g_use)) return;   // 手上拿着东西 / 已有一件在等着用 | Holding something in hand / one item is already waiting to be used
    if (dm_custom_get("auto_mana") && (auto_pot_life(b, pl, t) || auto_pot_mana(b, pl, t))) return;   // 一拍只发一件（g_use 一次一件），先红后蓝 | Only one is sent per tick (g_use handles one at a time), red first, then blue
    auto_merc_pot(b, pl, t);   // 玩家这一拍没喝药才轮到佣兵 | The mercenary only gets a turn if the player did not drink a potion this tick
}
// ---------- 副手组技能一键放 ----------
// 两组武器共用一张原版绑定表（main+0x2d5f0c0，每格 28 字节：+0 技能号、+4 物品 id，-1 = 自己学的；「切换武器时更换技能键」选项关着时只用前 12 格）。
// 在 II 组绑的充能技能（物品给的），切回 I 组后那件物品还穿在身上（换手位），技能链表里却没有这个节点，原版按了不放。
// 这里把「切过去 → 放 → 切回」合成一步，每一步都走原版：
//   换武器 = 调游戏自己的 main+0x41db90(当前组)（物品栏 I / II 页签、手柄肩键最后都调它；它自己组 0x50 包，包尾带「当前组」，服务端 0x6de790 拿它和自己记的组号比，对不上就拒）；
//   当前组 = 服务端玩家 [[玩家+0x10]+0xf0]+0x5a0（客户端那份没有，diag.c hud_tick 每拍发布）；
//   技能能放了 = 客户端玩家技能链表 [玩家+0x100] 里出现「技能号、物品 id 都相同」的节点（原版选技能 main+0x43f170 只认这两个都对上的）；
//   放 = 原样把玩家按的那个手柄键交给游戏（游戏自己组 0x3c / 0x0c，扣充能、扣法力、法力不够不放都是原版的事）；
//   切回 = 松手、施法动作结束（[[玩家+0x100]+0x18] == 0，服务端换武器也拒绝动作中的请求）后再调一次。
// 键在哪一刻压住：取手柄状态这一刻（dm_swapskill_filter，hle_hid.c），游戏读到之前；切好之前这个键不给游戏，切好后放行，按住就一直给（连放）。
// 次数 = 物品自己的「充能技能」属性 204（层 = 技能号 << 6 | 等级，值 = 上限 << 8 | 剩余），读游戏的属性表，不自己记。
// ---------- One-press cast of off-hand set skills ----------
// The two weapon sets share one original binding table (main+0x2d5f0c0, 28 bytes per cell: +0 skill id, +4 item id, -1 = self-learned; when the option 「切换武器时更换技能键」(Weapon Swap Separate Skill Buttons) is off only the first 12 cells are used).
// For a charged skill (granted by an item) bound in set II, after switching back to set I the item is still worn (in the weapon-swap slots), but the skill linked list has no such node, and in the original game pressing it casts nothing.
// Here "switch over → cast → switch back" is merged into one step, and every step goes through the original game:
//   weapon swap = call the game's own main+0x41db90(current set) (the inventory panel I / II tabs and the controller shoulder buttons all end up calling it; it builds the 0x50 packet itself, with the "current set" at the end, and the server 0x6de790 compares it with the set number it has recorded and rejects on a mismatch);
//   current set = server-side player [[player+0x10]+0xf0]+0x5a0 (the client copy does not have it; published every tick by diag.c hud_tick);
//   skill can be cast = a node with "the same skill id and item id" appears in the client player's skill linked list [player+0x100] (the original skill selection main+0x43f170 only accepts one where both match);
//   cast = hand the controller button the player pressed to the game as is (the game builds 0x3c / 0x0c itself; deducting charges, deducting mana, and not casting without enough mana are all the original game's business);
//   switch back = called once more after release and after the cast action ends ([[player+0x100]+0x18] == 0; the server also rejects weapon swap requests during an action).
// When the button is held back: at the moment the controller state is fetched (dm_swapskill_filter, hle_hid.c), before the game reads it; the button is not given to the game until the swap is done, is let through afterwards, and keeps being given while held (repeated casts).
// Charge count = the item's own "charged skill" stat 204 (layer = skill id << 6 | level, value = max << 8 | remaining), read from the game's stat table, not tracked by us.
enum { SS_IDLE, SS_REQ, SS_SWAPPING, SS_READY, SS_RELEASED, SS_BACK, SS_REBIND };
static _Atomic int g_ss_phase, g_ss_srv_wset = -1, g_ss_kind[12], g_ss_charges[12], g_ss_alt[12];
static _Atomic uint64_t g_ss_bit;          // 这一次压住 / 放行的那个键位 | The button bit held back / let through this time
static _Atomic int g_ss_slot = -1, g_ss_early, g_ss_dead_town, g_ss_cast, g_ss_zl;   // g_ss_cast：放行后看见施法了（次数减了 / 进了动作）；g_ss_zl：按下那一刻 ZL 按着（第二套技能），补按时一并带上   // 死了 / 在城里：按下不换武器（城里原版本来就放不了技能，切过去只会白换两次）   // 玩家在切好之前就松手了 | g_ss_cast: a cast was seen after letting through (count went down / entered an action); g_ss_zl: ZL was held at the moment of the press (second skill set), carried along in supplementary presses   // dead / in town: a press does not swap weapons (in town the original game cannot cast skills anyway, switching over would only swap twice for nothing)   // the player released before the swap was done
static _Atomic uint64_t g_ss_map[6];       // 6 个面：手柄位 | 普通格 << 32 | 替代格（按住 ZL）<< 40 | 6 face buttons: controller bit | normal cell << 32 | alternate cell (hold ZL) << 40
static _Atomic uint32_t g_ss_equipped[16]; static _Atomic int g_ss_neq;   // 身上穿着的物品 id（两组都算） | Ids of the items being worn (both sets counted)
void dm_swapskill_srv_wset(int w) { atomic_store(&g_ss_srv_wset, w); }
int dm_swapskill_wset(void) { return atomic_load(&g_ss_srv_wset); }
int dm_swapskill_busy(void) { return atomic_load(&g_ss_phase) != SS_IDLE; }   // 副手组技能的连招在进行（增益自动补 dm_autobuff.c 这时不自己换武器） | an off-hand-set skill combo is in progress (buff auto-recast dm_autobuff.c does not swap weapons itself meanwhile)
static _Atomic uint32_t g_ss_itemsig; uint32_t dm_swapskill_itemsig(void) { return atomic_load(&g_ss_itemsig); }   // 客户端技能链表里物品给的技能（技能号、物品 id）的指纹 | Fingerprint of the item-granted skills (skill id, item id) in the client skill linked list
int dm_swapskill_slot(int slot, int *charges) {   // 1 = 技能在另一组（画「II」），2 = 充能技能（画次数），4 = 当前组也有同号技能（按下用当前组那个，不换武器）；可叠加 | 1 = the skill is in the other set (draw "II"), 2 = charged skill (draw the count), 4 = the current set also has a skill with the same id (a press uses the one in the current set, no weapon swap); can be combined
    if (slot < 0 || slot >= 12) return 0; int k = atomic_load(&g_ss_kind[slot]); if (charges) *charges = atomic_load(&g_ss_charges[slot]); return k; }
int dm_swapskill_equipped(int32_t item) {   // 这件物品身上穿着（含换手位那组）：技能钮照常显示、不当空位 | This item is being worn (including the set in the weapon-swap slots): the skill button is shown as usual, not treated as an empty slot
    int n = atomic_load(&g_ss_neq); for (int i = 0; i < n && i < 16; i++) if ((int32_t)atomic_load(&g_ss_equipped[i]) == item) return 1; return 0; }
static int ss_node(uint64_t pl, int id, int item, int *charges) {   // 技能链表里「技能号 + 物品 id」都对上的节点：在 = 1 | The node in the skill linked list where both "skill id + item id" match: present = 1
    uint64_t l = *(uint64_t *)(pl + 0x100), n = l ? *(uint64_t *)l : 0;
    for (int k = 0; n && k < 1024; k++, n = *(uint64_t *)(n + 8)) { uint64_t r = *(uint64_t *)n; if (r && *(int16_t *)r == id && *(int32_t *)(n + 0x44) == item) { if (charges) *charges = *(int32_t *)(n + 0x48); return 1; } }
    return 0;
}
static int ss_node_any(uint64_t pl, int id) {   // 当前组里同技能号的节点（任意来源、等级 > 0）：返回它的物品 id（-1 = 自己学的），没有 = -2 | The node with the same skill id in the current set (any source, level > 0): returns its item id (-1 = self-learned), none = -2
    uint64_t l = *(uint64_t *)(pl + 0x100), n = l ? *(uint64_t *)l : 0;
    for (int k = 0; n && k < 1024; k++, n = *(uint64_t *)(n + 8)) { uint64_t r = *(uint64_t *)n; if (r && *(int16_t *)r == id && *(int32_t *)(n + 0x38) > 0) return *(int32_t *)(n + 0x44); }
    return -2;
}
static int ss_item_charges(uint64_t it, int skill) {   // 物品属性 204 里这个技能的剩余次数；不是充能 = -1（属性表：[单位+0x88]，扩展表取 +0xe8 否则 +0x30 的 {数组, 个数}，每项 {u32 属性号 << 16 | 层, i32 值}，同取属性函数 0x5b9290） | Remaining charges of this skill in item stat 204; not a charged skill = -1 (stat table: [unit+0x88], the {array, count} at +0xe8 for an extended table, otherwise at +0x30, each entry {u32 stat id << 16 | layer, i32 value}, same as the stat getter function 0x5b9290)
    uint64_t sl = *(uint64_t *)(it + 0x88); if (!sl) return -1;
    uint64_t a = (*(int32_t *)(sl + 0x1c) < 0) ? sl + 0xe8 : sl + 0x30, arr = *(uint64_t *)a, cnt = *(uint64_t *)(a + 8);
    for (uint64_t i = 0; arr && i < cnt && i < 512; i++) { uint32_t key = *(uint32_t *)(arr + 8 * i); if ((key >> 16) == 204 && (int)((key & 0xffff) >> 6) == skill) return *(int32_t *)(arr + 8 * i + 4) & 0xff; }
    return -1;
}
static void swapskill_pump(uint8_t *b, uint64_t pl) {
    if (!pl) return;
    // 面 → 格（main+0x2d13cbc，6 × {逻辑键, 普通格, 替代格}；逻辑键同 dm_touch_game_state.c sdk()）
    // Face button → cell (main+0x2d13cbc, 6 × {logical button, normal cell, alternate cell}; logical buttons as in dm_touch_game_state.c sdk())
    for (int i = 0; i < 6; i++) { uint32_t lg = *(uint32_t *)(b + 0x2d13cbc + 12 * i), nm = *(uint32_t *)(b + 0x2d13cbc + 12 * i + 4), al = *(uint32_t *)(b + 0x2d13cbc + 12 * i + 8);
        uint64_t bit = lg == 0x1000 ? DM_BTN_B : lg == 0x2000 ? DM_BTN_A : lg == 0x4000 ? DM_BTN_Y : lg == 0x8000 ? DM_BTN_X : lg == 0x200 ? DM_BTN_R : lg == 0x800 ? DM_BTN_ZR : 0;
        atomic_store(&g_ss_map[i], bit && nm < 12 && al < 12 ? bit | (uint64_t)nm << 32 | (uint64_t)al << 40 : 0); }
    // 身上穿着的物品（两组）：客户端物品链表里状态 1 = 穿着
    // Items being worn (both sets): state 1 in the client item linked list = worn
    uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; int neq = 0; uint64_t eqit[16];
    for (int k = 0; it && k < 512; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; if (*(uint32_t *)(it + 0xc) == 1 && neq < 16) { eqit[neq] = it; atomic_store(&g_ss_equipped[neq], *(uint32_t *)(it + 8)); neq++; } it = *(uint64_t *)(d + 0xb0); }
    atomic_store(&g_ss_neq, neq);
    { uint32_t h = 2166136261u; uint64_t l = *(uint64_t *)(pl + 0x100), n = l ? *(uint64_t *)l : 0;
      for (int k = 0; n && k < 1024; k++, n = *(uint64_t *)(n + 8)) { uint64_t r = *(uint64_t *)n; int32_t itm = *(int32_t *)(n + 0x44); if (r && itm != -1) { h = (h ^ (uint16_t)*(int16_t *)r) * 16777619u; h = (h ^ (uint32_t)itm) * 16777619u; } }
      atomic_store(&g_ss_itemsig, h); }
    for (int s = 0; s < 12; s++) { int id = *(int32_t *)(b + 0x2d5f0c0 + 28 * s), item = *(int32_t *)(b + 0x2d5f0c0 + 28 * s + 4), kind = 0, ch = -1;
        if (id >= 0 && item != -1) { uint64_t owner = 0; for (int e = 0; e < neq; e++) if (*(int32_t *)(eqit[e] + 8) == item) owner = eqit[e];
            if (owner) { ch = ss_item_charges(owner, id); if (ch >= 0) kind |= 2;
                if (!ss_node(pl, id, item, NULL)) { int alt = ss_node_any(pl, id);
                    if (alt != -2) { kind = 4; ch = -1; atomic_store(&g_ss_alt[s], alt);   // 用当前组那个来源：次数也显示它的（自己学的 / 非充能 = 不显示） | Use the source in the current set: the charge count shown is its too (self-learned / not charged = not shown)
                        for (int e = 0; e < neq && alt != -1; e++) if (*(int32_t *)(eqit[e] + 8) == alt) { ch = ss_item_charges(eqit[e], id); if (ch >= 0) kind |= 2; } }
                    else kind |= 1; } } }
        atomic_store(&g_ss_kind[s], kind); atomic_store(&g_ss_charges[s], ch < 0 ? 0 : ch); }
    // 测试开关 DM_TEST_SS_ALT=1（造现场）：把「在另一组」的那格绑定抄到同一个面的第二套（按住 ZL 那套）格里，验第二套技能上的副手组技能。
    // 每拍对齐一次：进局头几拍绑定里的物品 id 还是存档里的旧号，游戏随后才改成本局的号（先 14 后 13），只抄一次会抄到旧号
    // Test switch DM_TEST_SS_ALT=1 (test setup): copy the binding of the cell that is "in the other set" into the second-set (the hold-ZL set) cell of the same face button, to verify off-hand set skills on the second skill set.
    // Aligned once every tick: in the first few ticks of a run the item id in the binding is still the old number from the save, and the game only later changes it to this run's number (14 first, then 13), so copying only once would copy the old number
    { static int alt_face = -1; if (getenv("DM_TEST_SS_ALT")) for (int i = 0; i < 6; i++) { uint64_t m = atomic_load(&g_ss_map[i]); int nm = (int)(m >> 32 & 0xff), al = (int)(m >> 40 & 0xff);
          if (!m || nm >= 12 || al >= 12 || !(alt_face == i || (alt_face < 0 && (atomic_load(&g_ss_kind[nm]) & 1)))) continue;
          if (memcmp(b + 0x2d5f0c0 + 28 * al, b + 0x2d5f0c0 + 28 * nm, 28)) { memcpy(b + 0x2d5f0c0 + 28 * al, b + 0x2d5f0c0 + 28 * nm, 28); LOG("DM_TEST_SS_ALT：面 %d 普通格 %d（技能 %d 物品 %d）的绑定抄到第二套格 %d", i, nm, *(int32_t *)(b + 0x2d5f0c0 + 28 * nm), *(int32_t *)(b + 0x2d5f0c0 + 28 * nm + 4), al); }
          alt_face = i; break; } }
    { dm_hud_info hi; dm_hud_get(&hi); uint32_t md = *(uint32_t *)(pl + 0xc); atomic_store(&g_ss_dead_town, md == 0 || md == 17 || (hi.valid && dm_hud_area_is_town(hi.area))); }
    // 连招
    // Combo
    static uint32_t t0, tback, tidle, treq, ch_ready; static int busy_ready; static int id, item, from, tries, rb_slot = -1, rb_item; uint32_t t = dm_cur_tick();
    if (rb_slot >= 0 && atomic_load(&g_ss_phase) == SS_IDLE) { *(int32_t *)(b + 0x2d5f0c0 + 28 * rb_slot + 4) = rb_item; rb_slot = -1; }   // 松手：绑定改回原来的物品 | Release: change the binding back to the original item
    uint32_t mode = *(uint32_t *)(pl + 0xc); int dead = mode == 0 || mode == 17;
    int ph = atomic_load(&g_ss_phase), w = atomic_load(&g_ss_srv_wset);
    uint64_t sl = *(uint64_t *)(pl + 0x100); int busy = sl && *(uint64_t *)(sl + 0x18);
    void (*swap0)(int) = (void (*)(int))(b + 0x41db90);
    // 测试开关 DM_TEST_SS_REJECT=<次数>：连招里前这么多次「换过去」不发（模拟服务端拒绝 / 包丢了），验重试
    // Test switch DM_TEST_SS_REJECT=<count>: in the combo, the first this-many "swap over" attempts are not sent (simulating server rejection / packet loss), to verify the retry
    static int rej_left = -1; if (rej_left < 0) { const char *e = getenv("DM_TEST_SS_REJECT"); rej_left = e ? atoi(e) : 0; }
    // 技能树里的 I / II 页签（dm_cursor.c swaptab_*）：点了 = 换武器；关技能树时切回打开时那组（走下面「切回」同一段，带重试）
    // The I / II tabs in the skill tree (dm_cursor.c swaptab_*): tapping = weapon swap; when the skill tree is closed, switch back to the set it was opened with (through the same "switch back" code below, with retries)
    { static int tree_from = -1; int dm_ui_swaptab_take(void), dm_ui_skilltree_open(void);
      if (dm_ui_swaptab_take()) { if (ph == SS_IDLE && w >= 0 && !busy && !dead) { if (tree_from < 0) tree_from = w; swap0(w); LOG("副手组技能：拍 %u 技能树里点 I / II 页签 → 换武器（当前组 %d）", t, w); } else LOG("副手组技能：技能树页签这一下没换（%s）", ph != SS_IDLE ? "连招进行中" : busy ? "动作中" : "读不到当前武器组"); }
      if (tree_from >= 0 && !dm_ui_skilltree_open() && ph == SS_IDLE) { if (w >= 0 && w != tree_from) { from = tree_from; tback = t; tries = 1; swap0(w); atomic_store(&g_ss_phase, SS_BACK); LOG("副手组技能：拍 %u 关技能树 → 切回打开时的第 %d 组", t, from); } tree_from = -1; } }
    switch (ph) {
    case SS_REQ: { int s = atomic_load(&g_ss_slot); id = *(int32_t *)(b + 0x2d5f0c0 + 28 * s); item = *(int32_t *)(b + 0x2d5f0c0 + 28 * s + 4);
        if (atomic_load(&g_ss_kind[s]) & 4) {   // 两组都有：这一下把绑定的物品 id 临时指到当前组那个来源（原版选技能要「技能号 + 物品 id」都对上），松手后改回 | Both sets have it: for this press, temporarily point the binding's item id at the source in the current set (the original skill selection needs both "skill id + item id" to match), and change it back after release
            rb_slot = s; rb_item = item; *(int32_t *)(b + 0x2d5f0c0 + 28 * s + 4) = atomic_load(&g_ss_alt[s]); atomic_store(&g_ss_phase, SS_REBIND);
            LOG("副手组技能：拍 %u 槽 %d 技能 %d 两组都有 → 用当前组的（来源物品 %d，不换武器）", t, s, id, atomic_load(&g_ss_alt[s])); break; }
        if (dead || w < 0 || dm_uiscreen_panel()) { LOG("副手组技能：槽 %d 不触发（%s）", s, dead ? "死亡中" : w < 0 ? "读不到当前武器组" : "面板开着"); atomic_store(&g_ss_phase, SS_IDLE); break; }
        // 人物在动作中（施法 / 攻击 / 受击）：服务端会拒绝这时的换武器，先等动作结束（最多 50 拍）
        // The character is in an action (casting / attacking / being hit): the server rejects a weapon swap at this time, so wait for the action to end first (at most 50 ticks)
        if (busy) { if (!treq) { treq = t; LOG("副手组技能：拍 %u 槽 %d 按下时人物在动作中 → 等动作结束再换武器", t, s); }
            if (t - treq < 50) break; LOG("【红】副手组技能：等了 50 拍动作还没结束，放弃这次"); treq = 0; atomic_store(&g_ss_phase, SS_IDLE); break; }
        treq = 0; from = w; t0 = t; tries = 1; if (rej_left > 0) { rej_left--; LOG("DM_TEST_SS_REJECT：这次换过去不发"); } else swap0(w); atomic_store(&g_ss_phase, SS_SWAPPING);
        LOG("副手组技能：拍 %u 槽 %d 技能 %d（物品 %d）在另一组 → 换武器（当前组 %d）", t, s, id, item, w); break; }
    case SS_SWAPPING: { int ch = 0;
        if (dead) { LOG("副手组技能：换武器途中死亡，放弃（不切回，武器组由游戏管）"); atomic_store(&g_ss_phase, SS_IDLE); break; }
        if (ss_node(pl, id, item, &ch)) { LOG("副手组技能：拍 %u 技能可放了（换武器 → 可放 %u 拍，剩余 %d 次）→ 放行按键", t, t - t0, ch); ch_ready = (uint32_t)ch; busy_ready = busy; atomic_store(&g_ss_cast, 0); atomic_store(&g_ss_phase, SS_READY); break; }
        // 服务端记的组号还是原来那组 = 这次换武器没被接受（动作中被拒 / 包丢了）：6 拍后动作不在就再发，最多 5 次（换过去正常 1 拍内就变）
        // The set number recorded by the server is still the original set = this weapon swap was not accepted (rejected during an action / packet lost): send again after 6 ticks if no action is in progress, at most 5 times (a successful swap normally shows within 1 tick)
        if (t - t0 >= 6 && tries < 5 && atomic_load(&g_ss_srv_wset) == from && !busy) { tries++; t0 = t; if (rej_left > 0) { rej_left--; LOG("DM_TEST_SS_REJECT：这次换过去不发"); } else swap0(from); LOG("副手组技能：拍 %u 换过去没被接受（当前组仍是 %d）→ 第 %d 次再发", t, from, tries); break; }
        if (t - t0 >= 25) {   // 最后一次发出 25 拍后技能还没出现：放弃（列出这个技能现有的节点，看是物品号对不上还是没换过去） | The skill still has not appeared 25 ticks after the last send: give up (list the existing nodes of this skill, to see whether the item id does not match or the swap did not happen)
            { char nb[160] = ""; uint64_t l = *(uint64_t *)(pl + 0x100), n = l ? *(uint64_t *)l : 0; for (int k = 0; n && k < 1024; k++, n = *(uint64_t *)(n + 8)) { uint64_t r = *(uint64_t *)n; if (r && *(int16_t *)r == id) { size_t q = strlen(nb); snprintf(nb + q, sizeof nb - q, " 物品%d/等级%d", *(int32_t *)(n + 0x44), *(int32_t *)(n + 0x38)); } }
                LOG("【红】副手组技能：换武器后 25 拍技能还没出现，放弃这次（当前组 %d；技能 %d 现有节点：%s）", atomic_load(&g_ss_srv_wset), id, nb[0] ? nb : " 无"); atomic_store(&g_ss_phase, atomic_load(&g_ss_srv_wset) != from ? SS_RELEASED : SS_IDLE); } }
        break; }
    case SS_READY: { int ch = -1;   // 放行中：看见施法了（充能次数减了，或从不在动作变成在动作）就告诉取键处，早松手的补按到此为止 | Letting through: once a cast is seen (the charge count went down, or it went from not in an action to in an action), tell the button-fetch site, and the supplementary presses for an early release end here
        if (!atomic_load(&g_ss_cast) && ((ss_node(pl, id, item, &ch) && ch >= 0 && (uint32_t)ch < ch_ready) || (busy && !busy_ready))) { atomic_store(&g_ss_cast, 1); if (atomic_load(&g_ss_early)) LOG("副手组技能：拍 %u 看见施法（剩余 %d 次）", t, ch); }
        if (!busy) busy_ready = 0;
        break; }
    case SS_RELEASED:
        if (dead) { atomic_store(&g_ss_phase, SS_IDLE); break; }
        if (busy) { tidle = 0; break; }   // 施法动作还没完：服务端会拒绝动作中的换武器 | Cast action not finished yet: the server rejects a weapon swap during an action
        if (!tidle) tidle = t; if (t - tidle < 2) break;   // 客户端动作刚结束时服务端那边可能还差一拍（本机 结束当拍发的换武器被拒过一次） | When the client action has just ended the server side may still be one tick behind (on this machine a weapon swap sent on the very tick it ended was rejected once)
        if (w != from) { tback = t; tries = 1; swap0(w); atomic_store(&g_ss_phase, SS_BACK); LOG("副手组技能：拍 %u 松手、动作结束 → 切回（当前组 %d）", t, w); }
        else atomic_store(&g_ss_phase, SS_IDLE);
        break;
    case SS_BACK:
        if (w == from || dead) { LOG("副手组技能：拍 %u 已切回第 %d 组（用了 %u 拍）", t, from, t - tback); atomic_store(&g_ss_phase, SS_IDLE); break; }
        if (t - tback >= 8) { if (tries < 3) { tries++; tback = t; if (!busy) swap0(w); LOG("副手组技能：切回 25 拍没成，再试一次"); }
            else { LOG("【红】副手组技能：切回三次都没成，停在第 %d 组（玩家可在物品栏自己切）", w); atomic_store(&g_ss_phase, SS_IDLE); } }
        break;
    }
}
// 取手柄状态这一刻（hle_hid.c，游戏读之前）：压住要先换武器的那个键，切好后放行。raw = 这一刻要交给游戏的键
// At the moment the controller state is fetched (hle_hid.c, before the game reads it): hold back the button that needs a weapon swap first, and let it through once the swap is done. raw = the buttons about to be handed to the game at this moment
uint64_t dm_swapskill_filter(uint64_t raw) {
    static uint64_t prev; static int inj; uint64_t out = raw; int ph = atomic_load(&g_ss_phase); uint64_t bit = atomic_load(&g_ss_bit);
    if (ph == SS_IDLE && !dm_uiscreen_panel() && !atomic_load(&g_ss_dead_town)) {   // 面板开着时这些键是界面操作（返回 / 绑定…），不拦 | When a panel is open these buttons are UI operations (back / bind…), not intercepted
        uint64_t rise = raw & ~prev;
        for (int i = 0; i < 6 && rise; i++) { uint64_t m = atomic_load(&g_ss_map[i]), fb = m & 0xffffffffu; if (!fb || !(rise & fb)) continue;
            int slot = (int)((raw & DM_BTN_ZL) ? (m >> 40) & 0xff : (m >> 32) & 0xff), k = slot < 12 ? atomic_load(&g_ss_kind[slot]) : 0;
            if ((k & 4) || ((k & 1) && (!(k & 2) || atomic_load(&g_ss_charges[slot]) > 0))) {   // 在另一组、次数没用完（不是充能的 = 不看次数） | In the other set, charges not used up (not a charged skill = charges are not checked)
                atomic_store(&g_ss_slot, slot); atomic_store(&g_ss_bit, fb); atomic_store(&g_ss_early, 0); atomic_store(&g_ss_zl, (raw & DM_BTN_ZL) != 0); inj = 0; atomic_store(&g_ss_phase, SS_REQ); out &= ~fb; break; } }
    } else if (ph == SS_REBIND) { if (!(raw & bit)) atomic_store(&g_ss_phase, SS_IDLE); }   // 绑定已指到当前组：照常放行，松手结束 | The binding already points to the current set: let it through as usual, ends on release
    else if (ph == SS_REQ || ph == SS_SWAPPING) { out &= ~bit; if (!(raw & bit)) atomic_store(&g_ss_early, 1); if (atomic_load(&g_ss_zl)) out |= DM_BTN_ZL; }   // 第二套技能：ZL 一直按着，补按时它早已领先 | Second skill set: ZL is held the whole time, and by the time of the supplementary press it is already ahead
    else if (ph == SS_READY) {
        // 键已松开、还没看见施法（早松手，或刚放行一两帧就松了）：一直补按到看见施法（单击 = 一定放一次），上限 40 次取样（约 0.7 秒），按着 = 照常连放
        // Button already released and no cast seen yet (released early, or released just a frame or two after being let through): keep pressing for the player until a cast is seen (single tap = guaranteed to cast once), capped at 40 samples (about 0.7 seconds); held = keeps casting as usual
        if (!(raw & bit) && !atomic_load(&g_ss_cast) && inj < 40) { out |= bit; inj++; if (atomic_load(&g_ss_zl)) out |= DM_BTN_ZL; }
        else if (!(raw & bit)) { if (inj >= 40) LOG("【红】副手组技能：补按 40 次取样还没看见施法，放手"); atomic_store(&g_ss_phase, SS_RELEASED); } }
    prev = raw; return out;
}
// 测试开关 DM_TEST_BINDTBL（造现场，产品不设）：原版手柄技能绑定表 main+0x2d5f0c0（32 格 × 28 字节，前 4 字节 = 技能号）。
//   log：进局后每变一次打一行；clear:<格>[,<格>…]：进局 3 秒后把这几格的技能号写成空格的值（取表里第一个现成的空格值），造「这个键没有技能」
// Test switch DM_TEST_BINDTBL (scene setup, not set in the product): the original gamepad skill binding table main+0x2d5f0c0 (32 slots × 28 bytes, first 4 bytes = skill ID).
//   log: after entering the game, prints a line on every change; clear:<slot>[,<slot>...]: 3 seconds after entering the game, writes the skill ID of these slots as the empty slot value (taken from the first existing empty slot value in the table), producing "this key has no skill"
static void test_bindtbl(uint8_t *b, uint64_t pl) {
    static int mode = -1; static int clr[8], nclr; if (mode < 0) { const char *e = getenv("DM_TEST_BINDTBL"); mode = 0;
        if (e && !strcmp(e, "log")) mode = 1; else if (e && !strncmp(e, "clear:", 6)) { mode = 2; for (const char *c = e + 6; c && *c && nclr < 8; c = strchr(c, ',') ? strchr(c, ',') + 1 : NULL) clr[nclr++] = atoi(c); } }
    if (!mode || !pl) return;
    int32_t cur[32]; for (int i = 0; i < 32; i++) memcpy(&cur[i], b + 0x2d5f0c0 + (uint64_t)i * 28, 4);
    static int32_t prev[32]; static int have; static uint64_t t0; uint64_t now_ns(void); if (!t0) t0 = now_ns();
    if (mode == 2 && nclr && now_ns() - t0 > 3000000000ull) { int32_t emp = -1; for (int i = 0; i < 32 && emp == -1; i++) { int k = 0; for (int j = 0; j < nclr; j++) k |= clr[j] == i; if (!k && (cur[i] < 0 || cur[i] == 0xffff)) emp = cur[i]; }
        for (int j = 0; j < nclr; j++) if (clr[j] >= 0 && clr[j] < 32) { memcpy(b + 0x2d5f0c0 + (uint64_t)clr[j] * 28, &emp, 4); cur[clr[j]] = emp; }
        LOG("测试 DM_TEST_BINDTBL：清了 %d 格（写成空格值 %d）", nclr, emp); nclr = 0; }
    if (have && !memcmp(cur, prev, sizeof cur)) return; memcpy(prev, cur, sizeof cur); have = 1;
    char t[400]; int k = 0; for (int i = 0; i < 32; i++) k += snprintf(t + k, sizeof t - k, "%s%d", i == 16 ? " ‖ " : i ? "," : "", cur[i]);
    LOG("测试 DM_TEST_BINDTBL：绑定表 [%s]", t);
}
void dm_actions_pump(void) {
    uint8_t *b = ab(); if (!b) return; uint64_t root = *(uint64_t *)(b + 0x2f05458); if (!root) return;
    uint64_t pl = ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c));
    // 手上物品的唯一真值（dm_hud.c）：客户端玩家物品栏的光标物品（物品栏 +0x40），它是物品单位（类型 4）且按 id 在客户端单位表里查得回同一个对象才算拿着；否则当作手上空
    // The single source of truth for the held item (dm_hud.c): the cursor item of the client player's inventory (inventory +0x40); it counts as held only if it is an item unit (type 4) and looking it up by id in the client unit table returns the same object; otherwise the hand is treated as empty
    { uint64_t inv = pl ? *(uint64_t *)(pl + 0x90) : 0, held = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x40) : 0; uint32_t id = 0;
      if (held && *(uint32_t *)held == 4) { uint32_t hid = *(uint32_t *)(held + 8);
          if (((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), hid & 0x7f, hid, 4) == held) id = hid; }
      static uint64_t bad; if (held && !id && held != bad) { bad = held; LOG("【红】手上物品：客户端光标物品字段指着的不是有效物品（类型 %u）→ 当作手上空", *(uint32_t *)held); }
      static uint32_t last; if (id != last) { last = id; LOG("手上物品（客户端）：%s（物品 id %u）", id ? "拿着" : "空了", id); }
      dm_hud_set_held(id); }
    test_bindtbl(b, pl);
    DM_PROF(0, "a.elysium", dm_elysium_pump());
    { void dm_runeword_pump(void); DM_PROF(0, "a.runeword", dm_runeword_pump()); }   // 符文之语助手（dm_runeword.c） | Runeword helper (dm_runeword.c)
    dm_uiscreen_pump();
    // 随机名：给 CharacterCreateNamePanel 发它自己认的那条消息——InputMessage:ControllerActionBegin，载荷 +0x10 键序号 2（布局图例 option2「随机名字」；面板消息处理 main+0xc33378 的 0xc33594 分支：
    // 按当前职业从建角面板的名字表里随机取一条，main+0xac2058 写进名字框）。界面根 +0xb8（本帧已处理标志）非 0 时面板会忽略，下一帧再发
    // 游戏每次启动后取的第一个随机名是固定的（巫师是 Freya），存档里已有同名角色时点「创建」会失败、弹提示。
    // 取到的名字若已有同名角色（存档工作目录下有这个名字的目录，一个角色一个目录）就再按一次「随机名字」，一帧一次，至多 12 次——等于替玩家多按几下
    // Random name: send CharacterCreateNamePanel the message it itself recognizes -- InputMessage:ControllerActionBegin, payload +0x10 button index 2 (layout legend option2 「随机名字」(Random Name); the 0xc33594 branch of the panel message handler main+0xc33378:
    // it picks a random entry from the character creation panel's name table by the current class, and main+0xac2058 writes it into the name box). The panel ignores it when UI root +0xb8 (the handled-this-frame flag) is non-zero, so it is sent again on the next frame
    // The first random name the game takes after each launch is fixed (for the Sorceress it is Freya), and when the save already has a character of that name, tapping 「创建」(Create) fails and shows a message.
    // If the name obtained already belongs to a character (the save working directory has a directory of that name, one directory per character), press 「随机名字」(Random Name) once more, once per frame, at most 12 times -- equivalent to pressing it a few more times for the player
    if (atomic_load(&g_rname) && !*(uint8_t *)(root + 0xb8)) { int left = atomic_load(&g_rname) - 1; atomic_store(&g_rname, 0); uint64_t np = a_find(root, "CharacterCreateNamePanel", 0);
        if (np && *(uint8_t *)(np + 0x50)) { static uint8_t pay[0x40]; static uint64_t msg[8]; memset(pay, 0, sizeof pay); *(int32_t *)(pay + 0x10) = 2; memset(msg, 0, sizeof msg); msg[0] = 0x4ae067c6248b6042ull; msg[1] = 0x1e439768f7dd4594ull; msg[2] = (uint64_t)(uintptr_t)pay;
            ((void (*)(uint64_t, void *))*(uint64_t *)(*(uint64_t *)np + 0x28))(np, msg);
            uint64_t in = *(uint64_t *)(np + 0x168); const char *tx = in ? *(const char **)(in + 0x558) : NULL; uint64_t ln = in ? *(uint64_t *)(in + 0x560) : 0;
            int dup = 0; if (tx && ln && ln < 64) { char nm[64]; snprintf(nm, sizeof nm, "%.*s", (int)ln, tx); dup = dm_name_taken(nm); }
            LOG("起名：随机名 → 名字框「%.*s」%s", tx && ln < 64 ? (int)ln : 0, tx ? tx : "", !dup ? "" : left > 0 ? "（已有同名角色，再取一个）" : "【红】（已有同名角色，试满 12 次仍重名）");
            if (dup && left > 0) atomic_store(&g_rname, left); }
        else LOG("起名：名字面板没开，随机名没发"); }
    skill_reset_ui(b, root, pl);
    dm_shop_info s = {0}; dm_points_info p = { .sel_skill = -1 }; uint64_t sel = 0, held = 0;
    if (pl) {
        int (*stat)(uint64_t, int, int) = (void *)(b + 0x5b9290);
        s.gold = stat(pl, 14, 0); s.stash_gold = stat(pl, 15, 0); p.stat_points = stat(pl, 4, 0); p.skill_points = stat(pl, 5, 0);
        uint64_t fm = *(uint64_t *)(root + 0xd0), vp = a_panel(root, "VendorPanelLayout"); uint32_t npc_id = *(uint32_t *)(b + 0x2d5ebfc);
        uint64_t npc = *(uint32_t *)(b + 0x2d5ec00) ? ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d607a0), npc_id & 0x7f, npc_id, 1) : 0;
        s.open = npc && vp && *(uint8_t *)(vp + 0x50);
        if (s.open) {
            int (*price)(uint64_t, uint64_t, int, uint64_t, int, int) = (void *)(b + 0x572ec0); int diff = *(uint8_t *)(b + 0x2d20b88), cls = *(int16_t *)(npc + 4); uint64_t q = *(uint64_t *)(b + 0x2d5ec18);
            const uint64_t cand[2] = { fm ? *(uint64_t *)(fm + 0x190) : 0, fm ? *(uint64_t *)(fm + 0x178) : 0 };
            for (int i = 0; i < 2 && !sel; i++) { uint64_t w = cand[i]; if (!w || *(uint64_t *)w - (uint64_t)(uintptr_t)b != 0x2089ab8 || *(uint32_t *)(w + 0x5c8) != 1) continue;
                sel = ((uint64_t (*)(uint64_t, void *))(b + 0x3cc87c))(w, NULL); if (sel && *(uint32_t *)sel != 4) sel = 0; }
            if (sel) { s.sel_valid = 1; s.sel_id = *(uint32_t *)(sel + 8); uint32_t code = ((uint32_t (*)(uint64_t))(b + 0x570260))(sel); for (int i = 0; i < 3; i++) { char c = (char)(code >> (8 * i)); s.sel_code[i] = c == ' ' ? 0 : c; }
                // 货架格：[物品+0x38] → +0x10 / +0x14（购买包里「货架原格」读的同一处，0x4356b8–0x4356e4）；占格：物品表记录（步长 0x1c0，[版本表+0x15c0]，下标 [物品+4]）+0x11e 宽、+0x11f 高（自动找位 0x55f6c8 / 0x55f6d0 读的同一处）
                // Shelf cell: [item+0x38] → +0x10 / +0x14 (the same place the purchase packet reads for "original shelf cell", 0x4356b8–0x4356e4); occupied cells: item table record (stride 0x1c0, [version table+0x15c0], index [item+4]) +0x11e width, +0x11f height (the same place auto-placement 0x55f6c8 / 0x55f6d0 reads)
                { uint64_t pp = *(uint64_t *)(sel + 0x38); uint8_t ver = *(uint8_t *)(sel + 0x1bd); uint64_t t = ver >= 1 && ver <= 3 ? *(uint64_t *)(b + 0x2d64a18 + (uint64_t)ver * 16) : 0; uint32_t ci = *(uint32_t *)(sel + 4);
                  if (pp) { s.sel_gx = *(int32_t *)(pp + 0x10); s.sel_gy = *(int32_t *)(pp + 0x14); }
                  if (t && ci < *(uint32_t *)(t + 0x15c8)) { uint64_t rec = *(uint64_t *)(t + 0x15c0) + (uint64_t)ci * 0x1c0; s.sel_gw = *(uint8_t *)(rec + 0x11e); s.sel_gh = *(uint8_t *)(rec + 0x11f); } }
                s.buy_price = price(pl, sel, diff, q, cls, 0); s.can_buy = s.buy_price <= s.gold + s.stash_gold; }
            uint64_t inv = *(uint64_t *)(pl + 0x90); held = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x40) : 0;
            if (held) { s.held_valid = 1; s.held_id = *(uint32_t *)(held + 8); s.sell_price = ((int (*)(uint64_t))(b + 0x576c60))(held) ? price(pl, held, diff, q, cls, 1) : 0; }
        }
        uint64_t tp = a_panel(root, "SkillsTreePanel"); p.tree_open = tp && *(uint8_t *)(tp + 0x50);
        { uint64_t cp = a_panel(root, "CharacterStatsPanel"), ap = a_panel(root, "AdvancedStatsPanel"); p.char_open = cp && *(uint8_t *)(cp + 0x50) && !(ap && *(uint8_t *)(ap + 0x50)); }
        uint64_t fw = fm ? *(uint64_t *)(fm + 0x190) : 0;
        if (p.tree_open && fw && *(uint64_t *)(fw + 0x558) == H_TREE && *(uint64_t *)(fw + 0x568)) { p.sel_skill = **(int32_t **)(fw + 0x568); p.sel_block = a_skill_block(b, pl, p.sel_skill, p.skill_points); p.sel_can_add = !p.sel_block; }
    }
    // 装备对比：物品格控件（虚表 main+0x2089ab8）+0x632 = 「对比键按着」。原版只在比较键（手柄动作键序号 2）的按下 / 松开消息里写它（0x3b2e40），得焦时按键是否按着重算（0x3b6d50）、
    // 失焦清 0（0x3b6f00）；说明框生成（0x3b1cb4 末段 0x3b29d8）读它决定加不加「身上那件」的框。这里在开着时每帧把焦点 / 悬停的物品格这一位保持为 1；
    // 关掉那一下写回 0。+0x631（这一页允不允许对比，赌博页是 0）不碰
    // Equipment comparison: item cell widget (vtable main+0x2089ab8) +0x632 = "compare button held". The original game writes it only in the press / release messages of the compare button (controller action button index 2) (0x3b2e40), recomputes it from whether the button is held on gaining focus (0x3b6d50),
    // and clears it to 0 on losing focus (0x3b6f00); tooltip generation (0x3b1cb4, final part 0x3b29d8) reads it to decide whether to add the box for "the one being worn". Here, while enabled, this bit of the focused / hovered item cell is kept at 1 every frame;
    // it is written back to 0 at the moment of turning off. +0x631 (whether comparison is allowed on this page, 0 on the gambling page) is not touched
    { static int was; int on = atomic_load(&g_cmp); uint64_t fm = *(uint64_t *)(root + 0xd0);
      if (fm && (on || was)) for (int i = 0; i < 2; i++) { uint64_t w = *(uint64_t *)(fm + (i ? 0x178 : 0x190)); if (w && *(uint64_t *)w - (uint64_t)(uintptr_t)b == 0x2089ab8) *(uint8_t *)(w + 0x632) = (uint8_t)on; }
      was = on; }
    // 整理：main+0x3c2320(属主单位号, 容器页)——物品格「按住排序」（手柄动作键序号 4 + 长按，0x3b2e2c–0x3b2ea0）的终点：客户端算好每件的新位置，发一个 0x4e 包由服务端挪。
    // 先过原版的可用判断 main+0x3c213c(属主单位, 页)（手上没拿物品等），不过就不调。已经排好时它自己不发包
    // Sort: main+0x3c2320(owner unit id, container page) -- the end point of the item cell "hold to sort" (controller action button index 4 + long press, 0x3b2e2c–0x3b2ea0): the client computes each item's new position and sends one 0x4e packet for the server to move them.
    // First pass the original availability check main+0x3c213c(owner unit, page) (nothing held in hand, etc.); if it fails, do not call. When already sorted it sends no packet by itself
    { int sp = atomic_exchange(&g_sort, -1); if (sp >= 0 && pl) { int ok = ((int (*)(uint64_t, int))(b + 0x3c213c))(pl, sp) & 1; if (ok) ((void (*)(uint32_t, int))(b + 0x3c2320))(*(uint32_t *)(pl + 8), sp);
        LOG("整理：页 %d %s", sp, ok ? "已调原版整理" : "原版判定此刻不能整理（手上拿着物品等），没调"); } }
    // 装备 / 卸下。装备：main+0x3cf394(_, 物品)——物品格手柄「按住确认键」分流里的自动装备（0x3cfc34 → 0x3cf394），它用 0x583b60 选栏位、调 0x3bb24c 发一个直达包（空位 0x56、与已装备的交换 0x59 等）。
    // 先照原版过两步判断：0x583b60(玩家, 物品, &栏位, 1, 0) 与 0x565950(玩家, 栏位, 物品, 1, 0)，不过就只播错误音（0x35f120(3,…)，原版同）。雇佣兵面板开着且他能穿（0x3be960）时改给雇佣兵（0x3bed60），同原版顺序。
    // 卸下：main+0x3bb24c(0, 栏位, 1, -1, 0)——装备栏控件「按住确认键」的终点（0x3d6b80），发 0x58 包；栏位号 = 物品数据 +0x54。原版外层的共同前提：手上没拿物品、物品没有待确认的操作（0x474990）、交易中（界面开关 0x2d5ff5e）不做
    // Equip / unequip. Equip: main+0x3cf394(_, item) -- the auto-equip in the item cell controller "hold confirm" dispatch (0x3cfc34 → 0x3cf394); it picks the slot with 0x583b60 and calls 0x3bb24c to send a direct packet (0x56 for an empty slot, 0x59 for swapping with the equipped one, etc.).
    // First pass the same two checks as the original game: 0x583b60(player, item, &slot, 1, 0) and 0x565950(player, slot, item, 1, 0); if they fail only the error sound is played (0x35f120(3,…), same as the original game). When the mercenary panel is open and he can wear it (0x3be960), it goes to the mercenary instead (0x3bed60), in the same order as the original game.
    // Unequip: main+0x3bb24c(0, slot, 1, -1, 0) -- the end point of the equipment slot widget's "hold confirm" (0x3d6b80), sends the 0x58 packet; slot number = item data +0x54. Common preconditions of the original outer layer: nothing held in hand, the item has no pending-confirmation operation (0x474990), and nothing is done while trading (UI switch 0x2d5ff5e)
    if (pl) { int tq = atomic_exchange(&g_test_eq, 0); uint64_t inv = *(uint64_t *)(pl + 0x90), it0 = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0;
        if (tq) { uint32_t fid = 0; for (uint64_t it = tq < 3 ? it0 : 0; it && !fid; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; uint32_t md = *(uint32_t *)(it + 0xc); int sl = 0;
                if (tq == 1 ? (md == 0 && *(uint8_t *)(d + 0x55) == 0 && ((int (*)(uint64_t, uint64_t, int *, int, int))(b + 0x583b60))(pl, it, &sl, 1, 0)) : md == 1) fid = *(uint32_t *)(it + 8); it = *(uint64_t *)(d + 0xb0); }
            if (tq == 3) for (uint64_t it = it0; it && !fid; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; int sl = 0; if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && !((int (*)(uint64_t, uint64_t, int *, int, int))(b + 0x583b60))(pl, it, &sl, 1, 0)) fid = *(uint32_t *)(it + 8); it = *(uint64_t *)(d + 0xb0); }
            if (fid) atomic_store(tq == 2 ? &g_unequip : &g_equip, fid); else LOG("测试：%s", tq == 1 ? "背包里没有能穿的物品" : "身上没有穿着的物品"); }
        uint32_t eq = atomic_exchange(&g_equip, 0), un = atomic_exchange(&g_unequip, 0), id = eq ? eq : un;
        if (id) { uint64_t it = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), id & 0x7f, id, 4), d = it ? *(uint64_t *)(it + 0x10) : 0;
            int busy = !it || !d || ((uint64_t (*)(void))(b + 0x2aa7c0))() != 0 || (inv && *(uint64_t *)(inv + 0x40)) || (((int (*)(uint64_t))(b + 0x474990))(it) & 0xff) || *(uint8_t *)(b + 0x2d5ff5e);
            uint32_t code = it ? ((uint32_t (*)(uint64_t))(b + 0x570260))(it) : 0; const char *res;
            if (busy) res = !it ? "没找到" : "此刻不能做（手上拿着东西 / 物品有待确认的操作 / 交易中），没调";
            else if (eq) { int sl = 0;
                if (*(uint32_t *)(it + 0xc) != 0) res = "不在容器里，没调";
                else if (!((int (*)(uint64_t, uint64_t, int *, int, int))(b + 0x583b60))(pl, it, &sl, 1, 0) || !((int (*)(uint64_t, int, uint64_t, int, int))(b + 0x565950))(pl, sl, it, 1, 0)) { ((void (*)(int, uint64_t, int, int, int))(b + 0x35f120))(3, 0, 0, 0, 0); res = "原版判定穿不了，只播错误音"; }
                else if (((int (*)(uint64_t, int))(b + 0x3be960))(it, 0) & 1) { struct { uint32_t a; uint8_t c; } t0 = {0, 0}; ((void (*)(uint64_t, int, void *))(b + 0x3bed60))(it, 1, &t0); res = "雇佣兵面板开着：给雇佣兵装备（已调）"; }
                else { ((void (*)(uint64_t, uint64_t))(b + 0x3cf394))(0, it); res = "已调原版装备"; } }
            else { int mine = 0; for (uint64_t q = it0, k = 0; q && k < 512 && !mine; k++) { uint64_t qd = *(uint64_t *)(q + 0x10); if (!qd) break; mine = q == it; q = *(uint64_t *)(qd + 0xb0); }   /* 这件在不在玩家自己的物品栏链上 | Whether this item is on the player's own inventory chain */
                if (*(uint32_t *)(it + 0xc) != 1) res = "不是穿在身上的，没调"; else if (s.open) res = "商人页开着，不卸（原版那条路会变成卖掉）";
                else if (!mine) res = "穿在佣兵身上的，不卸（下面那条是卸玩家同一栏位的入口，会把玩家身上那件卸掉；佣兵的装备拖出来）";
                else { ((void (*)(uint64_t, int, int, uint32_t, int))(b + 0x3bb24c))(0, *(uint8_t *)(d + 0x54), 1, 0xffffffffu, 0); res = "已调原版卸下"; } }
            LOG("%s：物品 id %u（%.3s）%s", eq ? "装备" : "卸下", id, (const char *)&code, res); } }
    // 鉴定（「用了物品等目标」状态）：光标状态 [0x2d12928] == 5、待用物品 [0x2d12920]（main+0x471510 进入）。
    // 完成：物品格点击处理 0x3b3a64 在该状态下的终点（0x3b3f38 起）= 0x4747b0(目标) + 0x2cecf0(标志, 待用物品 id, 它的模式, 它的页, 它的格, 目标 id, 目标类型 4, 目标所在页, 目标的格, 三段腰带补位信息)；
    // 装备栏点击处理 0x3d7418 起是同一段，只是「目标所在页」给 0xff。标志的算法、腰带里的卷轴先调 0x47135c 取补位信息，都和上面「使用腰带物品」同。交易页（页 1 / 2）的目标原版不认。
    // 退出：原版点到空处 / 无效目标时内联的那段（0x3b3c54–0x3b3c88）：清待用物品、状态置 7、[0x2d12938] 清零、0x2b47dc(0x2acecc(), 玩家号, 7, -1)、0x2aa5f0()
    // Identify (the "item used, waiting for a target" state): cursor state [0x2d12928] == 5, pending item [0x2d12920] (entered through main+0x471510).
    // Completion: the end point of the item cell click handler 0x3b3a64 in that state (from 0x3b3f38) = 0x4747b0(target) + 0x2cecf0(flags, pending item id, its mode, its page, its cell, target id, target type 4, target's page, target's cell, three pieces of belt refill info);
    // the equipment slot click handler from 0x3d7418 is the same code, except that "target's page" is 0xff. How the flags are computed, and that a scroll in the belt first calls 0x47135c for the refill info, are the same as "use belt item" above. The original game does not accept targets on the trade pages (page 1 / 2).
    // Exit: the code the original game inlines when clicking on empty space / an invalid target (0x3b3c54–0x3b3c88): clear the pending item, set the state to 7, zero [0x2d12938], 0x2b47dc(0x2acecc(), player id, 7, -1), 0x2aa5f0()
    if (pl) { int ti = atomic_exchange(&g_test_ident, 0); uint64_t inv = *(uint64_t *)(pl + 0x90);
        if (ti == 1) { uint32_t fid = 0, any = 0; int n = 0, un = 0; for (uint64_t it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; n++; if (!(*(uint32_t *)(d + 0x18) & 0x10)) { un++; if (!fid) fid = *(uint32_t *)(it + 8); } else if (!any && it != *(uint64_t *)(b + 0x2d12920) && *(uint32_t *)(it + 0xc) == 1) any = *(uint32_t *)(it + 8); it = *(uint64_t *)(d + 0xb0); }
            if (!fid) fid = any; LOG("测试：物品 %d 件，未鉴定 %d 件，取 id %u%s", n, un, fid, un ? "" : "（没有未鉴定的，拿身上一件已鉴定的试发包）"); if (fid) atomic_store(&g_ident, fid); }
        else if (ti == 3) {   // 测试：把背包里第一件护身符在服务端和客户端都标成未鉴定（物品数据 +0x18 标志位 0x10），造鉴定用的现场 | Test: mark the first charm in the inventory as unidentified on both server and client (item data +0x18 flag bit 0x10), to set up a test case for identifying
            for (uint64_t it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
                if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && (((uint32_t (*)(uint64_t))(b + 0x570260))(it) & 0xffff) == 0x6d63) { uint64_t dm_diag_srv_item(uint32_t); uint32_t iid = *(uint32_t *)(it + 8); uint64_t su = dm_diag_srv_item(iid), sd = su ? *(uint64_t *)(su + 0x10) : 0;
                    if (sd) { *(uint32_t *)(sd + 0x18) &= ~0x10u; *(uint32_t *)(d + 0x18) &= ~0x10u; } LOG("测试：物品 id %u 标成未鉴定：服务端单位 %s", iid, sd ? "找到，两端已改" : "没找到，没改"); break; }
                it = *(uint64_t *)(d + 0xb0); } }
        else if (ti == 2) atomic_store(&g_tcancel, 1);
        // 单件修理（商人页修理态下点物品）：原版物品格点击处理 0x3b3f80–0x3b440c 与装备栏 0x3bb898–0x3bb8c4 在该模式下的共同终点
        // 0x40acf4(面板(函数体不读), 属主, 物品, 1 = 自己的, 0, 0) → 0x433e50 按模式走修理分支（判可修 0x435b00、算费用、成交）。不在修理态不许调：0x433e50 那时走的是卖出分支
        // 腰带一键补满（物品栏里腰带空格双击）：原版手柄「焦点在腰带上按住 R」那段在腰带控件的消息处理 0x3911d8 里，不是独立函数——
        // 条件 0x391484、算格 0x563380、发包（0x5d）、音效都在里面。这里照原样递一条「动作键 4（R）开始、带按住标记」的消息给它：
        // 它要求界面根本帧未处理（+0xb8 = 0）且焦点控件（[根+0xd0]+0x190）是腰带自己，调用期间替它摆好、调完放回
        // 原版一次只装一瓶（手柄是按住 R 连着触发）：一次补满 = 每 6 帧再递一次，直到原版判定装不了（腰带满 / 没药了），最多 16 次（只递一次时：背包 3 瓶、空 2 列，只补了 1 瓶）
        // 腰带控件每一轮现找（belt_now），不跨帧留指针；界面正在重建 / 刚重建（dm_ui_unstable：切布局事务中，或界面代号 2 帧内变过）这一轮先不做，稳了再接着补
        // Single-item repair (tap an item while the vendor page is in repair mode): the common endpoint, in that mode, of the original inventory-grid click handler 0x3b3f80–0x3b440c and the equipment slots 0x3bb898–0x3bb8c4
        // 0x40acf4(panel (body does not read it), owner, item, 1 = own, 0, 0) → 0x433e50 takes the repair branch by mode (repairable check 0x435b00, compute cost, complete the deal). Must not be called outside repair mode: 0x433e50 then takes the sell branch
        // One-tap belt fill (double-tap an empty belt slot in the inventory): the original controller "hold R with focus on the belt" logic lives in the belt widget's message handler 0x3911d8, not in a separate function;
        // the condition 0x391484, slot computation 0x563380, packet send (0x5d) and sound effect are all inside it. Here we hand it, unchanged, a message "action key 4 (R) start, with hold flag":
        // it requires the UI root frame to be unprocessed (+0xb8 = 0) and the focused widget ([root+0xd0]+0x190) to be the belt itself; we set these up for the call and restore them afterwards
        // The original inserts only one potion per call (on controller, holding R triggers it repeatedly): one fill = hand the message again every 6 frames until the original decides nothing more fits (belt full / out of potions), at most 16 times (with a single call: 3 potions in the backpack, 2 empty columns, only 1 potion filled)
        // The belt widget is looked up fresh each round (belt_now), with no pointer kept across frames; while the UI is rebuilding / just rebuilt (dm_ui_unstable: in a layout-switch transaction, or the UI generation changed within 2 frames) this round is skipped, and filling resumes once stable
        { static uint64_t bf_next; static int bf_left, bf_done; int dm_ui_unstable(void); int req = atomic_exchange(&g_bfill, 0); uint64_t fr = dm_guest_frame();
          if (req) { bf_left = 16; bf_done = 0; bf_next = 0;
              // 测试开关 DM_TEST_BELTFILL_SWITCH=1：补满一开始就造「补满途中界面重建」的现场，走真实输入的同一入口：
              // iOS 请求切到手柄版（同实体手柄碰一下，dm_padmode_request）；Mac App 恒为手柄版，会重建的是换键帽（同键盘 / 手柄交替，dm_padmode_request_kbd）
              // Test switch DM_TEST_BELTFILL_SWITCH=1: as soon as a fill starts, create the "UI rebuilt mid-fill" situation through the same entry point as real input:
              // on iOS, request a switch to the controller layout (same as touching a physical controller, dm_padmode_request); the Mac App is always in controller layout, so what rebuilds there is the key-cap swap (same as alternating keyboard / controller, dm_padmode_request_kbd)
              if (getenv("DM_TEST_BELTFILL_SWITCH")) {
#if TARGET_OS_OSX
                  int dm_padmode_kbd(void); dm_padmode_request_glyph(dm_padmode_kbd() ? DM_GLYPH_PAD : DM_GLYPH_KBD); LOG("测试 DM_TEST_BELTFILL_SWITCH：补满开始，请求换键帽");
#else
                  void dm_padmode_request(int); dm_padmode_request(1); LOG("测试 DM_TEST_BELTFILL_SWITCH：补满开始，请求切到手柄版");
#endif
              } }
          int due = bf_left && fr >= bf_next && !dm_ui_unstable(); uint64_t root = *(uint64_t *)(b + 0x2f05458), fh = root ? *(uint64_t *)(root + 0xd0) : 0, bw = due ? belt_now(b, root) : 0;
          if (due) { const char *res = NULL;
              if (!fh || !bw) res = "物品栏没开着 / 找不到腰带控件，没调";
              else if (!(((int (*)(uint64_t))(b + 0x391484))(bw) & 1)) {
                  // 原版自动装入只管腰带焦点那一列；别的空列：背包里下一瓶「原版判定此刻能进腰带」的药水（0x3c1b28），用背包格「移到腰带」那个函数放（同 dm_belt_put，原版自己挑列）
                  // The original auto-fill only handles the belt column that has focus; for the other empty columns: the next potion in the inventory that "the original game judges can go into the belt right now" (0x3c1b28), placed with the inventory cell "move to belt" function (same as dm_belt_put, the original game picks the column itself)
                  uint64_t inv = pl ? *(uint64_t *)(pl + 0x90) : 0, it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0, put = 0;
                  for (int k = 0; it && k < 512 && !put; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
                      if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && ((int (*)(uint64_t, int))(b + 0x574bc0))(it, 9) && (((int (*)(uint64_t))(b + 0x3c1b28))(it) & 1)) put = it; it = *(uint64_t *)(d + 0xb0); }   // 只放药水（类型 9）：卷轴原版也能进腰带，但补满是补药（第 2 列先被回城卷轴占了） | Only potions (type 9) are put in: in the original game scrolls can go into the belt too, but refill means refilling potions (column 2 would be taken by a Town Portal scroll first)
                  if (put) { struct { uint32_t slot; uint8_t fixed; } tgt = {0, 0}; ((void (*)(uint64_t, uint64_t, uint8_t, int, void *))(b + 0x3c1cd0))(put, pl, 0, 1, &tgt); bf_done++; bf_left--; bf_next = fr + 6; }
                  else res = bf_done ? "腰带满了 / 背包里没有能进腰带的药了" : "原版判定现在不能装（腰带满 / 手上有东西 / 没有能进腰带的），没调"; }
              else { uint8_t pay[0x20] = {0}; *(uint32_t *)(pay + 0x10) = 4; pay[0x19] = 1; uint64_t msg[3] = { 0x4ae067c6248b6042ull, 0x1e439768f7dd4594ull, (uint64_t)(uintptr_t)pay };
                  uint64_t of = *(uint64_t *)(fh + 0x190); uint8_t oh = *(uint8_t *)(root + 0xb8); *(uint64_t *)(fh + 0x190) = bw; *(uint8_t *)(root + 0xb8) = 0;
                  ((void (*)(uint64_t, uint64_t *))(b + 0x3911d8))(bw, msg); *(uint64_t *)(fh + 0x190) = of; *(uint8_t *)(root + 0xb8) = oh; bf_done++; bf_left--; bf_next = fr + 6; }
              if (res || !bf_left) { LOG("腰带补满：调了原版自动装入 %d 次，停（%s）", bf_done, res ? res : "到 16 次上限"); bf_left = 0; } } }
        { uint32_t rid = atomic_exchange(&g_repair, 0);
          if (rid) { uint64_t it = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), rid & 0x7f, rid, 4); const char *res;
              if (!it) res = "没找到物品"; else if (!dm_vendor_repair_mode()) res = "不在修理态，没调";
              else if (inv && *(uint64_t *)(inv + 0x40)) res = "手上拿着东西，没调"; else if (((int (*)(uint64_t))(b + 0x474990))(it) & 0xff) res = "物品有待确认的操作，没调";
              else if (!(((int (*)(uint64_t))(b + 0x435b00))(it) & 1)) res = "原版判定这件不用修 / 修不了，没调";
              else { ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int))(b + 0x40acf4))(0, pl, it, 1, 0, 0); res = "已调原版修理"; }
              LOG("修理：物品 id %u %s", rid, res); } }
        uint32_t id = atomic_exchange(&g_ident, 0); int cancel = atomic_exchange(&g_tcancel, 0); int on = *(uint32_t *)(b + 0x2d12928) == 5; uint64_t sc = on ? *(uint64_t *)(b + 0x2d12920) : 0;
        if (id) { uint64_t it = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), id & 0x7f, id, 4), d = it ? *(uint64_t *)(it + 0x10) : 0, pp = it ? *(uint64_t *)(it + 0x38) : 0;
            uint32_t code = it ? ((uint32_t (*)(uint64_t))(b + 0x570260))(it) : 0, md = it ? *(uint32_t *)(it + 0xc) : 9; const char *res;
            if (!sc) res = "不在等目标状态，没调"; else if (!it || !d || !pp) res = "没找到目标"; else if (md > 1) res = "目标不在背包 / 箱子 / 方块 / 身上，没调";
            else if (md == 0 && (*(uint8_t *)(d + 0x55) == 1 || *(uint8_t *)(d + 0x55) == 2)) res = "目标在交易页，原版不认，没调";
            else if (((int (*)(uint64_t))(b + 0x474990))(it) & 0xff) res = "目标有待确认的操作，没调";
            else { uint8_t col[0x30]; memset(col, 0, sizeof col); uint64_t sd = *(uint64_t *)(sc + 0x10), sp = *(uint64_t *)(sc + 0x38); uint32_t sm = *(uint32_t *)(sc + 0xc); int belt = sm == 2;
                ((void (*)(uint64_t))(b + 0x4747b0))(it);
                if (belt) ((void (*)(void *, uint64_t, uint64_t))(b + 0x47135c))(col, inv, sc);
                uint64_t rec = ((int (*)(uint64_t, int))(b + 0x574bc0))(sc, 0x12) ? 0 : ((uint64_t (*)(uint8_t, uint64_t))(b + 0x4d8580))(*(uint8_t *)(sc + 0x1bd), sc); int flag = rec && !*(uint8_t *)(rec + 8);
                uint32_t sxy = sp ? (*(uint32_t *)(sp + 0x10) & 0xffff) | *(uint32_t *)(sp + 0x14) << 16 : 0, txy = (*(uint32_t *)(pp + 0x10) & 0xffff) | *(uint32_t *)(pp + 0x14) << 16;
                ((void (*)(int, uint32_t, int, int, uint32_t, uint32_t, int, int, uint64_t, void *, void *, void *))(b + 0x2cecf0))(flag, *(uint32_t *)(sc + 8), (int)sm, sd ? *(uint8_t *)(sd + 0x55) : 0xff, sxy, id, 4, md == 0 ? *(uint8_t *)(d + 0x55) : 0xff, txy, belt ? col : NULL, belt ? col + 0xc : NULL, belt ? col + 0x18 : NULL);
                res = "已调原版「对物品使用」"; }
            LOG("鉴定：目标 id %u（%.3s）%s", id, (const char *)&code, res); }
        if (cancel && on) { uint64_t old = *(uint64_t *)(b + 0x2d12920); *(uint64_t *)(b + 0x2d12920) = 0; *(uint32_t *)(b + 0x2d12928) = 7; *(uint32_t *)(b + 0x2d12938) = 0;
            if (old) ((void (*)(uint64_t, uint32_t, int, int))(b + 0x2b47dc))(((uint64_t (*)(void))(b + 0x2acecc))(), *(uint32_t *)(b + 0x2d20e4c), 7, -1);
            ((void (*)(void))(b + 0x2aa5f0))(); LOG("鉴定：退出等目标状态"); } }
    // 高级仓库取件到背包（命令 0x63；包里 +0xd 是去向：1 = 背包，2 = 魔盒）：
    //   取 1 件：格控件的方法 main+0x39d824(格控件, _, 模式 0) → 子操作 1、去向 1。
    //   取 n 件：main+0x39d9e0(格控件, _, 模式 0, 件数) → 子操作 2、去向 1、带件数。
    // 两个函数都只读控件 +0x608 的物品指针，这里给一块只填了这一格的内存。件数超过叠数时按叠数取。
    // 模式 3 是「去向 2 = 移进魔盒」（原版长按确认键那条，0x3ca9ec，前提背包里有魔盒且箱子停在高级页 0x39b424）；没有魔盒时服务端不理—— 先按它做，三局都没反应，才查到去向这一格。
    // Mac 宿主实测（术士档，符文页一叠 12 个一号符文）：取 1 → 叠数 11、背包 +1；取 3 → 9、+3；取 10 → 2、+10
    // Withdraw from the advanced stash into the inventory (command 0x63; +0xd in the packet is the destination: 1 = inventory, 2 = Horadric Cube):
    //   Take 1: the cell widget's method main+0x39d824(cell widget, _, mode 0) → sub-operation 1, destination 1.
    //   Take n: main+0x39d9e0(cell widget, _, mode 0, count) → sub-operation 2, destination 1, with the count.
    // Both functions only read the item pointer at widget +0x608, so a block of memory with only that field filled in is passed here. If the count exceeds the stack size, the stack size is taken.
    // Mode 3 is "destination 2 = move into the Horadric Cube" (the original long-press confirm path, 0x3ca9ec, requiring a Horadric Cube in the inventory and the stash on the advanced page 0x39b424); without a Horadric Cube the server ignores it -- it was done that way first, three runs got no response, and only then was the destination field found.
    // Measured on the Mac host (Warlock save, a stack of 12 rune #1 on the rune page): take 1 → stack 11, inventory +1; take 3 → 9, +3; take 10 → 2, +10
    if (pl) { int tn = atomic_exchange(&g_test_take, 0);
        if (tn) { uint32_t fid = 0; int best = 0; uint64_t cu = ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)), us[3] = {pl, cu ? ((uint64_t (*)(uint64_t))(b + 0x652650))(cu) : 0, cu ? ((uint64_t (*)(uint64_t))(b + 0x652780))(cu) : 0};
            for (int k = 0; k < 3; k++) { uint64_t inv = us[k] ? *(uint64_t *)(us[k] + 0x90) : 0; if (!inv || *(uint32_t *)inv != 0x01020304) continue;
                int cnt = 0, stk = 0; for (uint64_t it = *(uint64_t *)(inv + 0x10); it; it = *(uint64_t *)(*(uint64_t *)(it + 0x10) + 0xb0)) { if (!*(uint64_t *)(it + 0x10)) break; cnt++; stk += *(uint8_t *)(*(uint64_t *)(it + 0x10) + 0x9c) != 0; } if (getenv("DM_TEST_TAKE_LOG")) LOG("测试：单位 %d（%s，同玩家 %d）物品 %d 件，其中成叠 %d", k, k == 0 ? "玩家" : k == 1 ? "0x652650" : "0x652780", us[k] == pl, cnt, stk);
                for (uint64_t it = *(uint64_t *)(inv + 0x10); it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; if (*(uint8_t *)(d + 0x9c) > best) { best = *(uint8_t *)(d + 0x9c); fid = *(uint32_t *)(it + 8); } it = *(uint64_t *)(d + 0xb0); } }
            if (fid) { LOG("测试：高级仓库里叠数最多的是物品 id %u（%d 件），取 %d 件", fid, best, tn); atomic_store(&g_take, (uint64_t)fid << 32 | (uint32_t)tn); } else LOG("测试：没有成叠的物品"); }
        { int nc = atomic_exchange(&g_take_cur, 0); if (nc) { dm_cursor_item_info ci; if (dm_cursor_item(&ci)) atomic_store(&g_take, (uint64_t)ci.id << 32 | (uint32_t)nc); else LOG("取多件：光标下没有物品，不动"); } }
        // 取件一件一件发（取 1 件的原版入口），每件之前用游戏自己的找位函数问背包还放不放得下这一件：main+0x55f620(物品栏, 物品, 格区号, &x, &y, 页 0)，
        // 物品栏 = 玩家 +0x90、格区号 = 表 main+0x1dae684[玩家类 × 8 + 4]（原版「放回背包」0x3bdb10 在 0x3be078–0x3be18c 的同一组参数）。放不下就停、不发包；
        // 上一件要等服务端收下（那一叠的叠数减了）且游戏拍号变了才取下一件——原版发包函数 main+0x2ceab0 会丢掉「同一拍里与上一个一模一样的包」（0x2ceb04–0x2ceb20，拍号 = [main+0x2d20b80]+0xc），连取的包字节完全相同。
        //原先长按一次发「取 3 件」，背包只剩一两格时服务端整个不收，一件都取不出
        // Withdrawals are sent one at a time (the original entry for taking 1), and before each one the game's own position-finding function is asked whether the inventory can still fit this one: main+0x55f620(item store, item, grid region number, &x, &y, page 0),
        // item store = player +0x90, grid region number = table main+0x1dae684[player class × 8 + 4] (the same set of arguments the original "put back into inventory" 0x3bdb10 uses at 0x3be078–0x3be18c). If it does not fit, stop and send no packet;
        // the next one is taken only after the server has accepted the previous one (that stack's count went down) and the game tick number has changed -- the original packet send function main+0x2ceab0 drops "a packet identical to the previous one within the same tick" (0x2ceb04–0x2ceb20, tick number = [main+0x2d20b80]+0xc), and consecutive withdrawal packets are byte-identical.
        // It used to send "take 3" for one long press, and with only one or two cells left in the inventory the server rejected the whole thing and not a single one came out
        static struct { uint32_t id, tick; int left, stack, wait; } seq;
        uint64_t tk = atomic_exchange(&g_take, 0);
        if (tk) { uint32_t id = (uint32_t)(tk >> 32); int n = (int)(uint32_t)tk; uint64_t it = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), id & 0x7f, id, 4), d = it ? *(uint64_t *)(it + 0x10) : 0;
            int st = d ? *(uint8_t *)(d + 0x9c) : 0; uint32_t code = it ? ((uint32_t (*)(uint64_t))(b + 0x570260))(it) : 0; const char *res;
            if (!it || !d) res = "没找到"; else if (!*(uint8_t *)(b + 0x2d5ff60)) res = "箱子没开，没调"; else if (!st) res = "不是成叠物品，没调";
            else { if (n > st) n = st; seq.id = id; seq.left = n; seq.stack = -1; seq.wait = 0; res = "逐件取"; }
            LOG("取多件：物品 id %u（%.3s）叠数 %d 取 %d：%s", id, (const char *)&code, st, n, res); }
        if (seq.left > 0) { uint64_t it = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), seq.id & 0x7f, seq.id, 4), d = it ? *(uint64_t *)(it + 0x10) : 0, inv = *(uint64_t *)(pl + 0x90);
            int st = d ? *(uint8_t *)(d + 0x9c) : 0; uint32_t cls = *(uint32_t *)(pl + 4); uint64_t gt = *(uint64_t *)(b + 0x2d20b80);
            if (!it || !st || !*(uint8_t *)(b + 0x2d5ff60) || !inv || cls > 7) seq.left = 0;   /* 那一叠取空了 / 箱子关了 | That stack has been emptied / the stash was closed */
            else if (seq.stack >= 0 && (st >= seq.stack || !gt || *(uint32_t *)(gt + 0xc) == seq.tick)) { if (++seq.wait > 60) { LOG("取多件：上一件 1 秒内没被收下（叠数仍 %d），剩 %d 件不取", st, seq.left); seq.left = 0; } }
            else { uint32_t fx = 0, fy = 0; static uint64_t fake[0x620 / 8];
                if (!((int (*)(uint64_t, uint64_t, int, uint32_t *, uint32_t *, int))(b + 0x55f620))(inv, it, *(int32_t *)(b + 0x1dae684 + cls * 8 + 4), &fx, &fy, 0)) { LOG("取多件：背包放不下了，剩 %d 件不取（叠数 %d）", seq.left, st); seq.left = 0; }
                else { fake[0x608 / 8] = it; ((void (*)(void *, uint64_t, int))(b + 0x39d824))(fake, 0, 0); seq.stack = st; seq.tick = gt ? *(uint32_t *)(gt + 0xc) : 0; seq.wait = 0; seq.left--; LOG("取多件：取 1 件到背包格 %u,%u（叠数 %d，之后还要取 %d）", fx, fy, st, seq.left); } } } }
    // 腰带展开：面板号 0x1a 的开关（状态字节 main+0x2d5ff48 + 0x1a）。键鼠版「显示腰带」动作（0x368be0）切换的就是它；腰带控件每帧（0x38f2f8）按它重算自己的展开标志。
    // 打开 main+0x453670(0x1a, 0)（自己过面板互斥），关闭 main+0x454f40(0x1a, 0)。排数 = main+0x3bb068(玩家) 给的格数 / 4
    // Belt expansion: the switch of panel number 0x1a (state byte main+0x2d5ff48 + 0x1a). The keyboard-and-mouse 「显示腰带」(Show Belt) action (0x368be0) toggles exactly this; the belt widget recomputes its own expanded flag from it every frame (0x38f2f8).
    // Open with main+0x453670(0x1a, 0) (it handles panel mutual exclusion itself), close with main+0x454f40(0x1a, 0). Row count = the cell count given by main+0x3bb068(player) / 4
    if (pl) { int bx = atomic_exchange(&g_beltx, -1); if (bx == 1) ((void (*)(int, int))(b + 0x453670))(0x1a, 0); else if (bx == 0) ((void (*)(int, int))(b + 0x454f40))(0x1a, 0);
        int cells = ((int (*)(uint64_t))(b + 0x3bb068))(pl), ex = *(uint8_t *)(b + 0x2d5ff48 + 0x1a) != 0; atomic_store(&g_belt_state, ((cells + 3) / 4 & 0xff) | ex << 8);
        if (bx >= 0) LOG("腰带：%s → 面板 0x1a 状态 %d，%d 排", bx ? "展开" : "收起", ex, (cells + 3) / 4); }
    pthread_mutex_lock(&g_mu); g_shop = s; g_pts = p; pthread_mutex_unlock(&g_mu); atomic_store(&g_open, s.open);
    int (*trade)(uint64_t, uint64_t, int, int, int, int) = (void *)(b + 0x433e50);
    // 钱不够 / 背包放不下都不在这里拦：照样走游戏的购买路径，由服务端拒并弹原版提示（「你没有足够的金币那么做。」「你没有足够的空间那么做。」，带「继续」），
    // 实测货不扣、钱不扣、货架那格还在（先前在这里先拦，玩家拖买后屏上没有任何反馈）。can_buy 只作读数
    // Neither not enough gold nor no room in the inventory is blocked here: it still goes through the game's purchase path, and the server rejects it and shows the original message (「你没有足够的金币那么做。」(You do not have enough gold for that.) / 「你没有足够的空间那么做。」(You do not have enough free space to do that.), with 「继续」(Continue)),
    // measured: no goods deducted, no gold deducted, the shelf cell still there (blocking here first used to leave the player with no on-screen feedback at all after a drag-to-buy). can_buy is only a reading
    if (atomic_exchange(&g_buy, 0)) { int r = !sel ? -1 : trade(pl, sel, 0, 0, 1, 0); LOG("商人：买选中的货 id %u（%s，货架格 %d,%d 占 %dx%d，价 %d，身上 %d + 箱内 %d）→ %d（1 已发、-1 没选中）", s.sel_id, s.sel_code, s.sel_gx, s.sel_gy, s.sel_gw, s.sel_gh, s.buy_price, s.gold, s.stash_gold, r); }
    if (atomic_exchange(&g_sell, 0)) { int r = held && s.sell_price > 0 ? trade(pl, held, 1, 0, 1, 0) : -1; LOG("商人：卖手上的物品 id %u（卖价 %d，身上 %d）→ %d", s.held_id, s.sell_price, s.gold, r); }
    // 放进腰带：main+0x3c1cd0(物品, 玩家, 来源页, 1 = 从背包格直接移, {槽, 已定槽 = 0})——手柄版背包格上「移到腰带」调的同一个（0x3cbff0–0x3cc014），
    // 它自己找腰带空位、发 0x53 包；前置同原版：main+0x3c1b28(物品) = 类型可放腰带且此刻有位，不满足就不动（腰带满时物品原地不动）
    // 测试动作：对背包（页 0）里第一瓶药水调 dm_belt_put（物品 id 每局不同，脚本里写不死）
    // Put into the belt: main+0x3c1cd0(item, player, source page, 1 = move directly from an inventory cell, {slot, slot decided = 0}) -- the same one the controller version calls for "move to belt" on an inventory cell (0x3cbff0–0x3cc014),
    // it finds a free belt position itself and sends the 0x53 packet; same precondition as the original game: main+0x3c1b28(item) = the type can go into the belt and there is room right now, otherwise nothing happens (when the belt is full the item stays where it is)
    // Test action: call dm_belt_put on the first potion in the inventory (page 0) (item ids differ every run and cannot be hard-coded in a script)
    if (atomic_exchange(&g_test_belt, 0) && pl) { uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; uint32_t pid = 0;
        for (int k = 0; it && k < 512 && !pid; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && ((int (*)(uint64_t, int))(b + 0x574bc0))(it, 9)) pid = *(uint32_t *)(it + 8); it = *(uint64_t *)(d + 0xb0); }
        if (pid) atomic_store(&g_belt, pid); else LOG("测试：背包里没有药水"); }
    // 使用物品：main+0x3c0098(物品, 掩码 3)——手柄版背包格「按住确认键」（0x3cfc34 → 0x3cfdb4）与键鼠右键点物品（0x3b55c0 → 0x3b574c）的共同终点，自己组 0x26 包发出；
    // 这里不经长按判定所以没有那约 0.4 秒。外层原版多挡的三条照挡：玩家在、玩家状态 [+0xc] ≠ 0x11、手上没拿物品（main+0x2aa7c0() == 0）；能不能用由原版 0x3bff70(物品, 3) 判
    // 喝腰带某一格：腰带里的物品容器号 2（[+0xc]），格位在位置记录（+0x38）的 +0x10：格号 = 列 + 4 × 排
    // Use item: main+0x3c0098(item, mask 3) -- the common end point of the controller version's inventory cell "hold confirm" (0x3cfc34 → 0x3cfdb4) and the keyboard-and-mouse right-click on an item (0x3b55c0 → 0x3b574c); it builds and sends the 0x26 packet itself;
    // this does not go through the long-press detection, so there is no delay of about 0.4 seconds. The three extra conditions the original outer layer blocks on are blocked here too: the player exists, player state [+0xc] ≠ 0x11, nothing held in hand (main+0x2aa7c0() == 0); whether it can be used is decided by the original 0x3bff70(item, 3)
    // Drinking from a belt cell: an item in the belt has container number 2 ([+0xc]), and its cell is at +0x10 of the position record (+0x38): cell number = column + 4 × row
    { int bu = atomic_exchange(&g_belt_use, 0), bm = bu >> 8 & 1; bu &= 0xff; if (bu && pl) { uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; uint32_t uid = 0;
        for (int k = 0; it && k < 512 && !uid; k++) { uint64_t d = *(uint64_t *)(it + 0x10), pp = *(uint64_t *)(it + 0x38); if (!d) break;
            if (*(uint32_t *)(it + 0xc) == 2 && pp && (int)(*(uint32_t *)(pp + 0x10) + 4 * *(uint32_t *)(pp + 0x14)) == bu - 1) uid = *(uint32_t *)(it + 8); it = *(uint64_t *)(d + 0xb0); }
        LOG("腰带：%s第 %d 排第 %d 列 → %s（物品 id %u）", bm ? "喂雇佣兵 " : "喝", (bu - 1) / 4, (bu - 1) % 4, uid ? "有药" : "空格，不动", uid);
        if (uid) { atomic_store(&g_use_merc, bm); atomic_store(&g_use, uid); } } }
    { int tk = atomic_exchange(&g_test_use, 0); if (tk && pl) { uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; uint32_t uid = 0;
        for (int k = 0; it && k < 512 && !uid; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; int (*ty)(uint64_t, int) = (void *)(b + 0x574bc0); uint32_t code = ((uint32_t (*)(uint64_t))(b + 0x570260))(it);
            if (tk == 5 ? *(uint32_t *)(it + 0xc) == 2 : (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && (tk == 1 ? ty(it, 9) : tk == 2 ? ty(it, 0x16) : tk == 3 ? ty(it, 0x12) : tk == 6 ? (code == 0x206b6269 || code == 0x20637369) : code == 0x20786f62))) uid = *(uint32_t *)(it + 8); it = *(uint64_t *)(d + 0xb0); }
        if (uid) atomic_store(&g_use, uid); else LOG("测试：背包里没有第 %d 类物品（1 药水 2 卷轴 3 书 4 方块）", tk); } }
    // 手上物品放到装备栏位：装备槽控件的点击处理（main+0x3d710c）在手上有物品时调的就是 0x3bb24c(手上物品, 栏位, 0, -1, 0)，
    // 它内部由 0x565950 判能不能放 / 换（空槽发 0x1a、有物品发 0x1d，需求不够等不发包）。走按键那条路要求界面焦点正在这个槽上——
    // 戒指位的上沿被物品栏里的腰带控件的命中区压着，焦点落在腰带上时确认键被它收走（戒指拖到戒指位没反应）
    // Put the item in hand into an equipment slot: the click handler of the equipment slot widget (main+0x3d710c) calls exactly 0x3bb24c(item in hand, slot, 0, -1, 0) when there is an item in hand,
    // and inside it 0x565950 decides whether it can be placed / swapped (empty slot sends 0x1a, occupied sends 0x1d, unmet requirements etc. send no packet). Going through the button path requires the UI focus to be on this slot --
    // the top edge of the ring slot is overlapped by the hit area of the belt widget in the inventory panel, and when the focus lands on the belt the confirm button is consumed by it (dragging a ring to the ring slot did nothing)
    { int loc = atomic_exchange(&g_held_equip, -1); uint64_t h = pl && loc > 0 ? ((uint64_t (*)(void))(b + 0x2aa7c0))() : 0;
      if (h && (loc & 0x100)) {   // 落在佣兵格上：原版「给雇佣兵装备」0x3bed60(物品, 0 = 物品在手上, {栏位, 指定了栏位})，它自己查能不能穿、发 0x51（左右戒指各是各的栏位，所以带上落点那一格） | Dropped on a mercenary cell: the original game's "equip the mercenary" 0x3bed60(item, 0 = item is in hand, {slot, slot specified}); it checks by itself whether the item can be worn and sends 0x51 (the left and right rings each have their own slot, so the cell at the drop point is passed along)
          struct { uint32_t a; uint8_t c; } t0 = { (uint32_t)(loc & 0xff), 1 }; ((void (*)(uint64_t, int, void *))(b + 0x3bed60))(h, 0, &t0);
          LOG("装备：手上物品 id %u → 佣兵栏位 %d，已调原版给雇佣兵装备", *(uint32_t *)(h + 8), loc & 0xff); }
      else if (h) { int r = ((int (*)(uint64_t, int, uint64_t, int, int))(b + 0x565950))(pl, loc, h, 0, 0); ((void (*)(uint64_t, int, int, int, int))(b + 0x3bb24c))(h, loc, 0, -1, 0);
          LOG("装备：手上物品 id %u → 栏位 %d（原版判定 %d：0 不能放、1 放入、5 交换）已调原版放下", *(uint32_t *)(h + 8), loc, r); }
      else if (loc > 0) LOG("装备：手上没有物品，不动"); }
    DM_PROF(0, "a.skill_cap", skill_cap_pump(b));
    {   // 测试开关 DM_TEST_SKILL_ADD=<帧>:<技能号>:<点数>[,<技能号>:<点数>…]（造现场 / 验上限）：到帧起每 6 帧替玩家发一个原版加技能点包（0x3b：技能号、0 = 加 1 点）， | test switch DM_TEST_SKILL_ADD=<frame>:<skill id>:<points>[,<skill id>:<points>...] (for setting up a test case / verifying the cap): starting at that frame, every 6 frames send one of the original game's add-skill-point packets on the player's behalf (0x3b: skill id, 0 = add 1 point),
        // 按写的顺序一个技能发够再发下一个（有前置的技能把前置写在前面）；进局后列一次已学技能，每个技能发完记一行它的基础等级
        // Skills are processed in the order written, one skill receiving all its points before the next (for a skill with prerequisites, list the prerequisites
        // first); after joining a game the learned skills are listed once, and each skill logs one line with its base level once its points have been sent
        static int at = -1, n, cur, sent, listed; static struct { int sk, cnt; } q[12]; static uint64_t nx;
        if (at < 0) { const char *e = getenv("DM_TEST_SKILL_ADD"); at = 0; if (e) { at = atoi(e); const char *c = strchr(e, ':'); while (c && n < 12 && sscanf(c + 1, "%d:%d", &q[n].sk, &q[n].cnt) == 2) { n++; c = strchr(c + 1, ','); } } }
        uint64_t f = dm_guest_frame(); int (*gs)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(b + 0x5b9290);
        if (at > 0 && pl) {
            if (!listed && f > 300) { listed = 1; char o[400]; int k2 = 0; o[0] = 0; uint64_t l = *(uint64_t *)(pl + 0x100), nd = l ? *(uint64_t *)l : 0;
                for (int k = 0; nd && k < 512; k++, nd = *(uint64_t *)(nd + 8)) { uint64_t r = *(uint64_t *)nd; if (r && *(int32_t *)(nd + 0x44) == -1 && *(int32_t *)(nd + 0x38) > 0 && k2 < 380) k2 += snprintf(o + k2, sizeof o - k2, " %d=%d", *(int16_t *)r, *(int32_t *)(nd + 0x38)); }
                LOG("测试 DM_TEST_SKILL_ADD：职业 %u 等级 %d 剩余技能点 %d，已学技能（号=基础等级）%s", *(uint32_t *)(pl + 4), gs(pl, 12, 0), gs(pl, 5, 0), o); }
            if (f >= (uint64_t)at && f >= nx && cur < n) {
                if (sent < q[cur].cnt) { uint8_t pk[5] = { 0x3b, (uint8_t)q[cur].sk, (uint8_t)(q[cur].sk >> 8), 0, 0 }; ((void (*)(const uint8_t *, int))(b + 0x2ceab0))(pk, 5); sent++; nx = f + 6; }
                else if (f >= nx + 12) { LOG("测试 DM_TEST_SKILL_ADD：技能 %d 发了 %d 个加点包 → 基础等级 %d（剩余技能点 %d）", q[cur].sk, sent, a_skill_base(pl, q[cur].sk), gs(pl, 5, 0)); cur++; sent = 0; } } } }
    {   // 测试开关 DM_TEST_C2S="<帧>:<十六进制包>[;…]"（造现场）：到帧替玩家原样发一个客户端包（原版发包函数）。例：选左手技能 = 3c + 技能号两字节 + 00 80 ffffffff | Test switch DM_TEST_C2S="<frame>:<hex packet>[;...]" (for test setup): at that frame, send one client packet verbatim on the player's behalf (the original game's packet send function). Example: select left-hand skill = 3c + two bytes of skill id + 00 80 ffffffff
        static int n = -1; static struct { uint64_t at; uint8_t pk[32]; int len; } q[8];
        if (n < 0) { n = 0; const char *e = getenv("DM_TEST_C2S"); while (e && *e && n < 8) { q[n].at = strtoull(e, NULL, 10); const char *c = strchr(e, ':'); if (!c) break; c++; int l = 0; unsigned v;
                while (l < 32 && sscanf(c, "%2x", &v) == 1 && c[0] != ';' && c[1] != ';' && c[1]) { q[n].pk[l++] = (uint8_t)v; c += 2; } q[n].len = l; n++; e = strchr(c, ';'); if (e) e++; } }
        uint64_t f = dm_guest_frame(); for (int i = 0; pl && i < n; i++) if (q[i].at && f >= q[i].at) { q[i].at = 0; ((void (*)(const uint8_t *, int))(b + 0x2ceab0))(q[i].pk, q[i].len); LOG("测试 DM_TEST_C2S：帧 %llu 发包 %02x（长 %d）", (unsigned long long)f, q[i].pk[0], q[i].len); } }
    { void dm_freetp_client_step(uint8_t *, uint64_t); DM_PROF(0, "a.freetp_client", dm_freetp_client_step(b, pl)); }   // 免费回城：客户端玩家挂回城技能、读 ZR 那格空不空 | Free town portal: attach the town portal skill to the client player and read whether the ZR slot is empty
    dm_custom_ui_pump(root); DM_PROF(0, "a.auto_pot", auto_pot_pump(b, pl)); DM_PROF(0, "a.swapskill", swapskill_pump(b, pl)); { void dm_kills_ui_pump(void); DM_PROF(0, "a.kills_ui", dm_kills_ui_pump()); } { void dm_rift_ui_pump(void); DM_PROF(0, "a.rift_ui", dm_rift_ui_pump()); } { void dm_vault_ui_pump(void); DM_PROF(0, "a.vault_ui", dm_vault_ui_pump()); }
    uint32_t use = atomic_exchange(&g_use, 0); int force_merc = use ? atomic_exchange(&g_use_merc, 0) : 0;
    if (use && pl) { uint64_t it = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), use & 0x7f, use, 4);
        int busy = *(uint32_t *)(pl + 0xc) == 0x11 || ((uint64_t (*)(void))(b + 0x2aa7c0))() != 0, can = it && !busy && (((int (*)(uint64_t, int))(b + 0x3bff70))(it, 3) & 1);
        int belt = it && *(uint32_t *)(it + 0xc) == 2;
        uint32_t code = it ? ((uint32_t (*)(uint64_t))(b + 0x570260))(it) : 0;
        // 雇佣兵页开着（[0x2d5ff66]）且原版判这件能喂给雇佣兵（0x3bf794：生命 / 解毒 / 解冻药类，且玩家有雇佣兵单位）：喂给雇佣兵，不自己喝。
        // 原版物品格「按住确认键」分流 0x3cfc34 的次序：待确认操作（0x474990）→ 给雇佣兵穿 → 自己穿 → 这一步（0x3cfd58–0x3cfd74）→ 自己用（0x3bff70 / 0x3c0098）；所以它不以「自己能用」为前提
        // The mercenary page is open ([0x2d5ff66]) and the original game judges this item can be fed to the mercenary (0x3bf794: healing / antidote / thawing potion types, and the player has a mercenary unit): feed it to the mercenary, do not drink it oneself.
        // Order of the original item cell "hold confirm" dispatch 0x3cfc34: pending-confirmation operation (0x474990) → equip on mercenary → equip on self → this step (0x3cfd58–0x3cfd74) → use on self (0x3bff70 / 0x3c0098); so it does not require "usable by oneself"
        int merc = it && !busy && (*(uint8_t *)(b + 0x2d5ff66) || force_merc) && !(((int (*)(uint64_t))(b + 0x474990))(it) & 0xff) && (((int (*)(uint64_t))(b + 0x3bf794))(it) & 1);
        uint32_t tgt = *(uint32_t *)(pl + 8);   // 腰带分支的目标：自己；喂雇佣兵时 = 雇佣兵单位 id（[0x2d33528] 链表里类型 7、属主 = 玩家、[+0x20] == 0 的那条的 +8；0x392dd8–0x392e10 同查） | Target of the belt branch: self; when feeding the mercenary = the mercenary unit id (the +8 of the entry in the [0x2d33528] linked list with type 7, owner = player, [+0x20] == 0; the same lookup as 0x392dd8–0x392e10)
        if (merc && belt) { tgt = 0xffffffffu; for (uint64_t e = *(uint64_t *)(b + 0x2d33528); e; e = *(uint64_t *)(e + 0x30)) if (*(uint32_t *)(e + 4) == 7 && *(uint32_t *)(e + 0xc) == *(uint32_t *)(pl + 8) && !*(uint32_t *)(e + 0x20)) { tgt = *(uint32_t *)(e + 8); break; }
            if (tgt == 0xffffffffu) merc = 0, tgt = *(uint32_t *)(pl + 8); }
        if (force_merc && !merc) can = 0;   // 指定喂雇佣兵而喂不了：不改成自己喝（同底排「ZL + 腰带键」的原版结果） | Asked to feed the mercenary but cannot: do not switch to drinking it oneself (same as the original game's result for "ZL + belt button" on the bottom row)
        // 腰带里的物品不走 0x3c0098：那条路按「背包物品」组包（容器字段 0），服务端不认（实测：包发了药没喝）。照腰带格自己的使用分支（main+0x392cd0–0x392dd0）组：
        // 0x47135c 算出这一列里要往下补位的药水 → 0x2cecf0(标志, 物品 id, 容器 2 = 腰带, 0xff, 格 x | y<<16, 目标 = 玩家 id, 目标类型 0, 0xff, 0, 三段补位信息) → 0x4747b0(物品)
        // Items in the belt do not go through 0x3c0098: that path builds the packet as an "inventory item" (container field 0), which the server does not accept (measured: the packet was sent but the potion was not drunk). Build it like the belt cell's own use branch (main+0x392cd0–0x392dd0):
        // 0x47135c computes the potions in this column that will shift down to fill the gap → 0x2cecf0(flags, item id, container 2 = belt, 0xff, cell x | y<<16, target = player id, target type 0, 0xff, 0, three pieces of refill info) → 0x4747b0(item)
        if ((can || merc) && belt) { uint8_t col[0x30]; memset(col, 0, sizeof col); uint64_t pp = *(uint64_t *)(it + 0x38); ((void (*)(void *, uint64_t, uint64_t))(b + 0x47135c))(col, *(uint64_t *)(pl + 0x90), it);
            uint8_t ver = *(uint8_t *)(it + 0x1bd); uint64_t rec = ((int (*)(uint64_t, int))(b + 0x574bc0))(it, 0x12) ? 0 : ((uint64_t (*)(uint8_t, uint64_t))(b + 0x4d8580))(ver, it); int flag = rec && !*(uint8_t *)(rec + 8);
            uint32_t xy = pp ? (*(uint32_t *)(pp + 0x10) & 0xffff) | *(uint32_t *)(pp + 0x14) << 16 : 0;
            ((void (*)(int, uint32_t, int, int, uint32_t, uint32_t, int, int, uint64_t, void *, void *, void *))(b + 0x2cecf0))(flag, use, 2, 0xff, xy, tgt, merc, 0xff, 0, col, col + 0xc, col + 0x18);
            ((void (*)(uint64_t))(b + 0x4747b0))(it);
            if (merc) ((void (*)(uint32_t, int, int))(b + 0x4bd710))(tgt, 1, 0x5b); }   // 腰带格分支喂雇佣兵时原版紧跟的一步（0x392e4c–0x392e58） | The step the original game does right after feeding the mercenary in the belt-cell branch (0x392e4c–0x392e58)
        else if (merc) ((void (*)(uint64_t))(b + 0x3bf860))(it);   // 背包里的：原版同一个函数（自己发 0x26，目标 = 雇佣兵、目标类型 1） | In the inventory: the same original function (it sends 0x26 itself, target = mercenary, target type 1)
        else if (can) ((void (*)(uint64_t, int))(b + 0x3c0098))(it, 3);
        LOG("使用：物品 id %u（%.3s）%s", use, (const char *)&code, !it ? "没找到" : busy ? "此刻不能用（手上拿着东西 / 玩家状态不允许），没发" : merc ? (force_merc ? "腰带双击：喂给雇佣兵（已调）" : "雇佣兵页开着：喂给雇佣兵（已调）") : force_merc ? "腰带双击：雇佣兵不在 / 这瓶不能喂，不喂也不自己喝" : can ? "已调原版使用" : "原版判定不可使用，没发"); }
    uint32_t bid = atomic_exchange(&g_belt, 0);
    if (bid && pl) { uint64_t it = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), bid & 0x7f, bid, 4), d = it ? *(uint64_t *)(it + 0x10) : 0;
        int ok = it && d && *(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && (((int (*)(uint64_t))(b + 0x3c1b28))(it) & 1);   // 只认背包里的（模式 0、页 0） | Only items in the inventory are accepted (mode 0, page 0)
        if (ok) { struct { uint32_t slot; uint8_t fixed; } tgt = {0, 0}; ((void (*)(uint64_t, uint64_t, uint8_t, int, void *))(b + 0x3c1cd0))(it, pl, *(uint8_t *)(d + 0x55), 1, &tgt); }
        LOG("腰带：物品 id %u %s", bid, !it ? "没找到" : ok ? "放进腰带（已发）" : "不能放（不在背包里 / 类型不对 / 腰带满），不动"); }
    // 手上物品收进背包（不关面板）：原版「给手上物品在容器里找位置并放入」是 main+0x3bdb10(物品, 目标单位, 页, 物品所在页或 0xff, 标志, {0})——按页选格区
    // （0 = 背包，3 = 魔盒，4 = 大箱子：0x3bdc50 的跳表），用 main+0x55f620 找能放下的位置，发 0x18（放入容器）并把物品从光标上摘掉，返回 1 放成 / 0 没放成（此时不发包、物品留在手上）。
    // 参数照原版放回背包的调用点 0x3a0b7c–0x3a0ba8：页 0、标志 0；前置同原版（0x3cf4f4）：main+0x474990(物品) 为 0。关面板不在这段里。
    // 目标单位 = 玩家自己（函数按目标的类取背包格区、在目标的物品栏里找位置）。原先照 0x3cf4d0 抄的 main+0x652650(玩家) 不是「玩家」：它在玩家物品栏的一条链表里找某类条目再换成单位，
    // 术士档碰巧有（所以最初自测通过），88 级法师档返回 0（「取不到目标单位」，重试 91 帧都一样）
    // 测试动作：手上有物品时，对背包（页 0）里第一件别的物品发手柄版「放下并换出」的 0x1f 包（21 字节：1f [手上 id][目标 id][u32 0][u16 x, y][u16 x, y]，照一局真实记录的包），换下来的那件由服务端回执放到手上
    // Stow the item in hand into the inventory (without closing the panel): the original game's "find a place in a container for the item in hand and put it in" is main+0x3bdb10(item, target unit, page, the item's page or 0xff, flags, {0}) -- it picks the grid region by page
    // (0 = inventory, 3 = Horadric Cube, 4 = stash: the jump table at 0x3bdc50), finds a position that fits with main+0x55f620, sends 0x18 (put into container) and takes the item off the cursor, returning 1 placed / 0 not placed (in which case no packet is sent and the item stays in hand).
    // The arguments follow the original put-back-into-inventory call site 0x3a0b7c–0x3a0ba8: page 0, flags 0; same precondition as the original game (0x3cf4f4): main+0x474990(item) is 0. Closing the panel is not part of this.
    // Target unit = the player itself (the function picks the inventory grid region by the target's class and looks for a position in the target's item store). The main+0x652650(player) originally copied from 0x3cf4d0 is not "the player": it looks for an entry of a certain kind in a linked list of the player's item store and converts it to a unit,
    // which the Warlock save happened to have (so the first self-test passed), while the level-88 Sorceress save returns 0 (「取不到目标单位」(cannot get target unit), the same on all 91 retry frames)
    // Test action: with an item in hand, send for the first other item in the inventory (page 0) the controller version's "put down and swap out" 0x1f packet (21 bytes: 1f [in-hand id][target id][u32 0][u16 x, y][u16 x, y], following a packet recorded in a real run); the item swapped out is put into hand by the server's acknowledgment
    if (atomic_exchange(&g_test_swap, 0) && pl) { uint64_t inv = *(uint64_t *)(pl + 0x90), ok = inv && *(uint32_t *)inv == 0x01020304, held = ok ? *(uint64_t *)(inv + 0x40) : 0, it = ok ? *(uint64_t *)(inv + 0x10) : 0; int done = 0;
        for (int k = 0; held && it && k < 512 && !done; k++) { uint64_t d = *(uint64_t *)(it + 0x10), pp = *(uint64_t *)(it + 0x38); if (!d) break;
            if (it != held && *(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && pp) { uint8_t pk[21] = {0x1f}; uint32_t hid = *(uint32_t *)(held + 8), id = *(uint32_t *)(it + 8), x = *(uint32_t *)(pp + 0x10), y = *(uint32_t *)(pp + 0x14); uint16_t xy[4] = { (uint16_t)x, (uint16_t)y, (uint16_t)x, (uint16_t)y };
                memcpy(pk + 1, &hid, 4); memcpy(pk + 5, &id, 4); memcpy(pk + 13, xy, 8); ((void (*)(void *, int))(b + 0x2ceab0))(pk, 21); LOG("测试：手上 id %u 与背包里 id %u（格 %u,%u）交换", hid, id, x, y); done = 1; }
            it = *(uint64_t *)(d + 0xb0); }
        if (!done) LOG("测试：没有可交换的（手上没物品 / 背包里没有别的物品）"); }
    // 刚交换到手上的物品要等服务器回执后才放得进去（发 0x1f 后立刻调，没放成；约 0.8 秒后原版按返回同一件就收进去了），
    // 所以没放成先不报结果，逐帧重试到 STOW_TRY 帧（约 1.5 秒）为止；手上本来就没物品则立刻报 0。
    // An item just swapped into hand can be put in only after the server's acknowledgment (calling right after sending 0x1f did not place it; about 0.8 seconds later the original game's Back press stowed the same item),
    // so a failed placement does not report a result yet, and it is retried every frame for up to STOW_TRY frames (about 1.5 seconds); if there was no item in hand to begin with, 0 is reported immediately.
    enum { STOW_TRY = 90 };
    if (atomic_load(&g_stow)) { static int tries, why0; int r = 0, why = 0; uint64_t inv = pl ? *(uint64_t *)(pl + 0x90) : 0, held = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x40) : 0, tgt = 0;
        if (!held || *(uint32_t *)held != 4) why = 1;
        else if (((int (*)(uint64_t))(b + 0x474990))(held) & 1) why = 2;
        else if (!(tgt = pl)) why = 3;
        else { uint64_t d = *(uint64_t *)(held + 0x10); struct { uint64_t pos; uint8_t fixed; } o = {0, 0};
            r = ((int (*)(uint64_t, uint64_t, int, int, int, void *))(b + 0x3bdb10))(held, tgt, 0, d ? *(uint8_t *)(d + 0x55) : 0xff, 0, &o) & 1; if (!r) why = 4; }
        if (!tries) why0 = why;
        if (r || why == 1 || ++tries >= STOW_TRY) { static const char *const wn[] = { "", "手上没有物品", "物品暂时不可放（main+0x474990）", "取不到目标单位", "背包里找不到位置" };
            LOG("手上物品收进背包：%s（试了 %d 帧；第一帧：%s，最后：%s）", r ? "已放入（已发）" : why == 1 && !tries ? "手上没有物品" : "没放成，物品留在手上", tries + 1, r && !tries ? "直接放成" : wn[why0], r ? "放成" : wn[why]);
            tries = 0; atomic_store(&g_stow, 0); atomic_store(&g_stow_res, r); } }
    // 测试动作（面板没开时拿到手上的物品游戏会自己丢到脚边地上——实测紧跟着发 0x17，可用来造「地上有物品」的现场）：把背包（页 0）或大箱子（页 4）里第一件能卖的物品拿到手上——拼手柄版「拿起」发的 0x19 包（17 字节：19 [id][x][y][页]，服务端 0x6d5ec0），再调 0x4747b0(物品)（同 0x3b5878）
    // Test action (when no panel is open the game itself drops an item picked up into hand onto the ground at the player's feet -- measured: a 0x17 is sent right after -- which can be used to set up an "item on the ground" test case): pick up into hand the first sellable item in the inventory (page 0) or the stash (page 4) -- build the 0x19 packet the controller version sends for "pick up" (17 bytes: 19 [id][x][y][page], server 0x6d5ec0), then call 0x4747b0(item) (same as 0x3b5878)
    if (atomic_exchange(&g_test_hold, 0) && pl) { uint64_t inv = *(uint64_t *)(pl + 0x90), it0 = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0, it; int done = 0;
        for (int want = 0; want <= 4 && !done; want += 4) { it = it0;   // 先找背包（页 0），没有再找大箱子（页 4；人不在箱子旁时服务端不认） | Look in the inventory first (page 0), and if none then in the stash (page 4; the server does not accept it when the player is not next to the stash)
        for (int k = 0; it && k < 512 && !done; k++) { uint64_t d = *(uint64_t *)(it + 0x10), pp = *(uint64_t *)(it + 0x38); if (!d) break;
            if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == want && pp && ((int (*)(uint64_t))(b + 0x576c60))(it)) { uint8_t pk[17] = {0x19}; uint32_t id = *(uint32_t *)(it + 8), x = *(uint32_t *)(pp + 0x10), y = *(uint32_t *)(pp + 0x14), pg = *(uint8_t *)(d + 0x55);
                memcpy(pk + 1, &id, 4); memcpy(pk + 5, &x, 4); memcpy(pk + 9, &y, 4); memcpy(pk + 13, &pg, 4); ((void (*)(void *, int))(b + 0x2ceab0))(pk, 17); ((void (*)(uint64_t))(b + 0x4747b0))(it); LOG("测试：拿起物品 id %u（页 %u 格 %u,%u）", id, pg, x, y); done = 1; }
            it = *(uint64_t *)(d + 0xb0); } }
        if (!done) LOG("测试：背包里没有能卖的物品可拿"); }
    // 测试动作：不经「拿到手上」，直接把玩家背包里或穿着的第一件能卖的物品卖给当前商人——与 dm_shop_sell_cursor 调的是同一个交易函数、同样的参数，只是物品来源不同
    // Test action: without going through "pick up into hand", directly sell the first sellable item in the player's inventory or worn to the current vendor -- it calls the same trade function with the same arguments as dm_shop_sell_cursor, only the item source differs
    if (atomic_exchange(&g_test_sell, 0) && pl) { uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; int done = 0;
        for (int k = 0; it && k < 512 && !done; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
            if (s.open && (*(uint32_t *)(it + 0xc) == 1 || (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0)) && ((int (*)(uint64_t))(b + 0x576c60))(it)) { uint64_t npc = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d607a0), *(uint32_t *)(b + 0x2d5ebfc) & 0x7f, *(uint32_t *)(b + 0x2d5ebfc), 1);
                int pr = npc ? ((int (*)(uint64_t, uint64_t, int, uint64_t, int, int))(b + 0x572ec0))(pl, it, *(uint8_t *)(b + 0x2d20b88), *(uint64_t *)(b + 0x2d5ec18), *(int16_t *)(npc + 4), 1) : 0;
                int r = trade(pl, it, 1, 0, 1, 0); LOG("测试：直接卖物品 id %u（模式 %u，卖价 %d，身上 %d）→ %d", *(uint32_t *)(it + 8), *(uint32_t *)(it + 0xc), pr, s.gold, r); done = 1; }
            it = *(uint64_t *)(d + 0xb0); }
        if (!done) LOG("测试：没有能卖的物品 / 商人页没开"); }
    int st = atomic_exchange(&g_stat, -1);
    if (st >= 0 && pl) { a_send(b, root, H_STATS, H_USEPT, st >> 8, st & 0xff); LOG("加点：属性序号 %d 模式 %d（2 = 1 点、3 = 5 点、1 = 全部），发令前剩余属性点 %d", st >> 8, st & 0xff, p.stat_points); }
    if (atomic_exchange(&g_skill, 0)) { int go = p.sel_skill >= 0 && p.sel_can_add; if (go) a_send(b, root, H_TREE, H_USEPT, p.sel_skill, 0); LOG("加点：技能 %d（不能加的原因 %d：0 可加 1 没点数 2 等级不够 3 前置没学 4 其它 5 没打通过秘境、已到 20；剩余技能点 %d）%s", p.sel_skill, p.sel_block, p.skill_points, go ? "已发" : p.sel_skill >= 0 ? "不能加，没发" : "没有选中的技能，忽略");
        if (!go && p.sel_skill >= 0 && p.sel_block == 5) { static uint64_t last; uint64_t f = dm_guest_frame(); if (!last || f - last >= 120) { last = f; void dm_game_notice(uint8_t *, const char *); dm_game_notice(b, dm_str(b, "dmSkillCapLocked")); LOG("加点：提示「%s」", dm_str(b, "dmSkillCapLocked")); } } }
}
