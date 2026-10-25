#include "dm_elysium.h"
#include "dm_cursor.h"
#include "dm_custom.h"
#include "dm.h"
#include "dm_uiscreen.h"
#include "dm_input.h"
#include "dm_hud.h"
#include "dm_actions.h"
#include "dm_cursor_item.h"
#include "dm_i18n.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <math.h>

static module_t *main_module(void) {
    for (int i = 0; i < g_nmod; ++i)
        if (!strcmp(g_mod[i].name, "main")) return &g_mod[i];
    return NULL;
}

static _Atomic uint64_t g_a_wait;   // 确认键按下沿交给游戏的时刻（0 = 没有在等）：A 松开后收到「手柄动作结束」清零，松开 0.3 秒没等到也放行（面板正好关了之类） | Time at which the confirm button press edge was handed to the game (0 = not waiting): cleared to zero on receiving 'controller action end' after A is released; also let through if it has not arrived 0.3 seconds after release (e.g. the panel happened to close)
int dm_lat_on(void) { static int on = -1; if (on < 0) on = getenv("DM_LAT_LOG") != NULL; return on; }
void dm_cursor_set(int x, int y) {
    module_t *m = main_module();
    if (!m) { fprintf(stderr, "dm_cursor_set：没有 main 模块\n"); return; }
    *(int32_t *)(m->base + 0x2d128f8) = x;
    *(int32_t *)(m->base + 0x2d128fc) = y;
}

static _Atomic uint64_t g_req;   // 位 63 = 有效；低 32 位 = 归一化 x、y（各 16 位，×65535） | Bit 63 = valid; low 32 bits = normalized x, y (16 bits each, ×65535)

void dm_cursor_set_screen(float x, float y, float screen_w, float screen_h) {
    if (screen_w <= 0 || screen_h <= 0) return;
    float nx = x / screen_w, ny = y / screen_h;
    nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    atomic_store(&g_req, (1ull << 63) | ((uint64_t)(uint32_t)(nx * 65535 + .5f) << 16) | (uint32_t)(ny * 65535 + .5f));
}

void dm_cursor_clear(void) { atomic_store(&g_req, 0); }

// 扣住的 A：每次 A 按下沿记下当时的目标位置排队；光标先走到队首位置放出 A（一次采样），再处理下一个 / 追当前目标。
// 拖放（起点拿起 → 移动 → 终点放下）因此不会因为目标在途中变化而丢掉起点那下 A（inv_drag 实测：清掉扣住的 A 只剩移动）
// 到位半径（原版视口像素）：背包格半宽约 18.75（1564×720 下格距 37.5），原先取 18 = 停在格子边上，光标从另一侧过来时落到邻格
// （游戏卡 0.4 s 后光标大步越过目标，停在 752 / 目标 770，物品放进了左邻格）。取 9 = 格心 ±1/4 格
// Held-back A: every A press edge records the target position at that time and is queued; the cursor first moves to the position at the head of the queue and releases the A (one sample), then handles the next one / chases the current target.
// A drag-and-drop (pick up at the start -> move -> put down at the end) therefore does not lose the A at the start because the target changed on the way (measured with inv_drag: clearing the held-back A left only the move)
// In-position radius (original-game viewport pixels): the half-width of an inventory cell is about 18.75 (cell pitch 37.5 at 1564×720); previously 18 was used = stopping on the cell edge, and when the cursor came from the other side it landed in the neighboring cell
// (after the game stalled for 0.4 s the cursor took a big step past the target and stopped at 752 / target 770, and the item was put into the left neighboring cell). Using 9 = cell center ±1/4 cell
enum { DM_CURSOR_NEAR = 4, DM_CURSOR_JUMP = 10, DM_CURSOR_LAND = 6 };   // 到位半径 4：取 9 时从上方过来会停在目标上方 9 像素——目标贴着背包上沿就停在了背包外，「放下」按在空处、物品留在手上（拖放用例 z1 成功 0 / 5 的直接原因） | In-position radius 4: with 9, coming from above it stops 9 pixels above the target -- if the target touches the top edge of the inventory it stops outside the inventory, the 'put down' is pressed on empty space and the item stays in hand (the direct cause of one drag-and-drop regression case succeeding 0 / 5)
enum { DM_CURSOR_SYNC_POLLS = 1 };   // 写完坐标后摇杆回中几次采样再推（游戏每帧取一次手柄；按次数不按墙钟：墙钟 50 ms 在 60 帧下白等一拍多） | Number of samples the stick is centered after writing the coordinates before pushing (the game reads the controller once per frame; by count, not wall clock: a 50 ms wall clock wastes more than one tick at 60 fps)
enum { AQ_MAX = 4 };
typedef struct { int x, y; uint64_t t; } aq_entry;
static aq_entry g_aq[AQ_MAX];
static int g_naq;
static _Atomic int g_naq_pub;   // 对前端发布：还有几下 A 扣着没送到游戏、或 Y 在等光标到位（dm_cursor_pending） | Published to the front end: how many A presses are still held back and not delivered to the game, or Y is waiting for the cursor to be in position (dm_cursor_pending)
int dm_cursor_pending(void) { return atomic_load(&g_naq_pub); }

static void stick_toward(int dx, int dy, int distance, int32_t *lx, int32_t *ly) {
    double strength = distance <= 16 ? 1.0 : fmax(0.25, fmin(1.0, distance / 60.0));   // 落位后的最后几像素全速走（约 3 像素 / 拍，过不了半径 4 的头）；远处才按比例（只有写坐标没写住时才会走到远处） | After landing, cover the last few pixels at full speed (about 3 pixels / tick, cannot overshoot radius 4); scale proportionally only when far away (it only gets far away when the coordinate write did not stick)
    *lx = (int32_t)lround(dx / (double)distance * strength * DM_STICK_MAX);
    *ly = (int32_t)lround(-dy / (double)distance * strength * DM_STICK_MAX);
    if (*lx > DM_STICK_MAX) *lx = DM_STICK_MAX;
    if (*lx < -DM_STICK_MAX) *lx = -DM_STICK_MAX;
    if (*ly > DM_STICK_MAX) *ly = DM_STICK_MAX;
    if (*ly < -DM_STICK_MAX) *ly = -DM_STICK_MAX;
}

// 放不下往里挪：游戏把手上的物品以光标为中心放——3 格高的剑落在背包最上一行就超出背包、游戏不收、物品留在手上
// （z1：下一个手势的「拿起」变成了「放下」，整串错位）。多格区域的「放下」按完 0.3 秒物品还在手上且还是同一件，
// 就往区域里面挪一格再按一次，至多两次（物品最高 4 格）；换成了别的物品（= 和那格的物品交换了）就停。
// Move inward when it cannot be put down: the game places the item in hand with the cursor as the center -- a 3-cell-tall sword landing on the top row of the inventory sticks out of the inventory, the game rejects it, and the item stays in hand
// (in one regression case: the 'pick up' of the next gesture became a 'put down', and the whole sequence went out of step). If the item is still in hand and still the same item 0.3 seconds after a 'put down' in a multi-cell area,
// move one cell into the area and press again, at most twice (items are at most 4 cells tall); stop if it changed to another item (= swapped with the item in that cell).
typedef struct { int valid, x, y, cell_h, r, rows; } drop_cell;
static drop_cell g_dc;                      // 最近一次多格区域落点（slot_center 填） | Most recent landing point in a multi-cell area (filled by slot_center)
typedef struct { int active, pending, tries, id, x, y, cell_h, r, rows, stage; uint64_t t; } drop_retry;   // stage：手上清空收尾走到哪一步（4 = 已按第二次返回；0 还没动 / 1 已往原位放过 / 2 已调收进背包 / 3 已按返回） | stage: how far the empty-the-hand wrap-up has gone (4 = back pressed a second time; 0 not started / 1 already put toward the original position / 2 already called stow into inventory / 3 back pressed)
static drop_retry g_rt;
// 这一页有没有自由的手柄光标：开着的面板里有物品区（背包 / 装备槽 / 箱子 / 货架 / 腰带格）、技能图标或属性「+」才有。
// 没有的页（设置、暂停菜单、任务、小站、对话…）摇杆是「改当前选项 / 移高亮」——往那里推摇杆去追光标目标会改设置：
// 说明框到点把光标停到屏幕左缘，在设置页变成一直向左推，音效从 100 被减到 0 还存了盘。
// 所以光标请求只在有自由光标的页生效，别的页整个不动手柄（不推摇杆、不写坐标、不扣键）。
// Whether this page has a free controller cursor: only when an open panel has an item area (inventory / equipment slots / stash / vendor shelf / belt cells), skill icons or attribute '+'.
// On pages without one (settings, pause menu, quests, waypoint, dialogue...) the stick means 'change the current option / move the highlight' -- pushing the stick there to chase the cursor target changes settings:
// when the tooltip timed out and parked the cursor at the left edge of the screen, on the settings page that became a continuous push to the left, and the sound effects volume was reduced from 100 to 0 and even saved.
// So cursor requests take effect only on pages with a free cursor, and on other pages the controller is not touched at all (no stick push, no coordinate write, no key held back).
static int item_name(const char *n);
static int panel_skip(const char *nm);
static int has_visible_leaf(uint64_t w, int dep);
uint64_t dm_guest_frame(void);
// 界面代号（dm_cursor.h）。加 1 的时机：游戏取走运行模式 / 性能模式通知（hle_oe.c，它据此重载布局）、切语言（diag.c setlang_obs）、
// 以及游戏主线程每帧第一次取代号时发现上一帧在的某个顶层面板不在了（面板卸载、整套重建都会这样；只新开面板不算——只增不释放，
// 若也加 1，自定义页之类按「控件换了 = 页刚出现」重摆的会把玩家没应用的改动冲掉）
// UI generation (dm_cursor.h). Incremented when: the game consumes a run-mode / performance-mode notification (hle_oe.c; it reloads the layout accordingly), the language is switched (diag.c setlang_obs),
// and when the game main thread, on its first generation fetch each frame, finds that some top-level panel present in the previous frame is gone (panel unloads and full rebuilds both do this; newly opened panels alone do not count, since that only adds and frees nothing;
// if it also incremented, pages like the custom page that re-lay out on "widget changed = page just appeared" would wipe out the player's unapplied changes)
static _Atomic uint32_t g_ui_gen = 1; static _Atomic uint64_t g_ui_gen_fr;   // 最近一次加 1 时的游戏帧 | Game frame of the most recent increment
void dm_ui_gen_bump(int why) { uint32_t g = atomic_fetch_add(&g_ui_gen, 1) + 1; atomic_store(&g_ui_gen_fr, dm_guest_frame());
    static unsigned n; if (n++ >= 200) return;
    switch (why) { case DM_UIGEN_OPMODE: LOG("界面代号 → %u（游戏取走运行模式通知，要重载布局）", g); break; case DM_UIGEN_PERFMODE: LOG("界面代号 → %u（游戏取走性能模式通知）", g); break;
        case DM_UIGEN_LANG: LOG("界面代号 → %u（切语言）", g); break; case DM_UIGEN_ROOT: LOG("界面代号 → %u（界面根换了）", g); break; default: LOG("界面代号 → %u（有顶层面板被卸掉）", g); } }
static void ui_gen_scan(void) {   // 游戏主线程：顶层面板集合里少了谁 → 加 1 | Game main thread: a panel missing from the top-level panel set → increment by 1
    static uint64_t fr0 = UINT64_MAX, prev[256], root0; static int np; uint64_t fr = dm_guest_frame(); if (fr == fr0) return; fr0 = fr;
    module_t *m = main_module(); uint64_t root = 0; if (m) memcpy(&root, m->base + 0x2f05458, 8);
    uint64_t ch = root ? *(uint64_t *)(root + 0x58) : 0, n = root ? *(uint64_t *)(root + 0x60) : 0; if (n > 256) n = 0;
    int gone = root != root0 && root0; for (int i = 0; i < np && !gone; i++) { int k = 0; while (k < (int)n && *(uint64_t *)(ch + 8 * k) != prev[i]) k++; gone = k == (int)n; }
    if (gone) dm_ui_gen_bump(root != root0 ? DM_UIGEN_ROOT : DM_UIGEN_PANEL_GONE);
    root0 = root; np = (int)n; for (int i = 0; i < np; i++) prev[i] = *(uint64_t *)(ch + 8 * i);
}
uint32_t dm_ui_gen(void) { ui_gen_scan(); return atomic_load(&g_ui_gen); }   // 只许游戏主线程调（顺带做本帧的顶层面板核对） | Game main thread only (also performs this frame's top-level panel check)
static int ui_has(uint64_t w, uint64_t want, int dep);
static int ui_alive(uint64_t w) { module_t *m = main_module(); uint64_t root = 0; if (m) memcpy(&root, m->base + 0x2f05458, 8); return w && root && ui_has(root, w, 0); }
// 不在树上时只给 0、不清 w：游戏重载布局时会把面板（AutoMap 等）先从根上摘下、再挂回同一个对象，摘下的那几帧不在树上但没被释放；
// 清掉的话挂回来就成了「新控件」——挪位表因此忘了小地图已挪过，又挪一遍（回归 ipadfull top 局小地图被推出屏外）
// When not in the tree, only return 0 without clearing w: when the game reloads the layout it first detaches panels (AutoMap etc.) from the root and then reattaches the same object; during those frames they are not in the tree but not freed;
// clearing would turn the reattached one into a "new widget", so the move table forgot the minimap had already been moved and moved it again (in the ipadfull top regression run the minimap was pushed off screen)
uint64_t dm_wref_get(dm_wref *r) { if (!r->w) return 0; uint32_t g = dm_ui_gen(); if (r->gen == g) return r->w; if (ui_alive(r->w)) { r->gen = g; return r->w; } return 0; }
int dm_ui_gen_revalidate(uint32_t *gen, uint64_t anchor) { uint32_t g = dm_ui_gen(); if (*gen == g) return 1; if (ui_alive(anchor)) { *gen = g; return 1; } return 0; }
int dm_ui_unstable(void) { int dm_padmode_busy(void); (void)dm_ui_gen(); return dm_padmode_busy() || dm_guest_frame() - atomic_load(&g_ui_gen_fr) < 2; }
static int free_walk(uint64_t w, int dep, uint64_t base) {
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    const char *nm = (const char *)*(uint64_t *)(w + 8);
    if (dep == 1 && nm && (!strncmp(nm, "HUD", 3) || panel_skip(nm))) return 0;
    if ((nm && (item_name(nm) || !strncmp(nm, "SkillButton", 11))) || *(uint64_t *)w == base + 0x20c7d68) return 1;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (free_walk(*(uint64_t *)(ch + 8 * i), dep + 1, base)) return 1;
    return 0;
}
// 模态面板：挡住底下面板输入的那一层。按游戏自己的命名认——名字以 Modal 结尾、且有可见内容（数量框 DropGoldModal、确认框 ConfirmationModal、
// 技能绑定框 ButtonBindingModal…）。从上往下找第一个；角标 / 指示类面板（存盘指示 SaveIndicatorPanel、佣兵头像 HireablesPanel、提示条…）
// 不是模态的，不挡。with_bind = 0 时技能绑定框不算（它有自己的点击处理和专用取消键）
// Modal panel: the layer that blocks input to the panels beneath it. Identified by the game's own naming -- the name ends with Modal and it has visible content (quantity box DropGoldModal, confirmation box ConfirmationModal,
// skill binding box ButtonBindingModal...). Search from the top down for the first one; badge / indicator panels (save indicator SaveIndicatorPanel, mercenary portrait HireablesPanel, button legend...)
// are not modal and do not block. With with_bind = 0 the skill binding box does not count (it has its own click handling and a dedicated cancel key)
static uint64_t modal_any(uint64_t root, int with_bind, const char **name) {
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60); if (!ch || n > 256) return 0;
    for (int i = (int)n - 1; i >= 0; i--) { uint64_t p = *(uint64_t *)(ch + 8 * i); if (!p || !*(uint8_t *)(p + 0x50)) continue;
        const char *nm = (const char *)*(uint64_t *)(p + 8); size_t l = nm ? strlen(nm) : 0;
        if (l < 5 || strcmp(nm + l - 5, "Modal") || (!with_bind && !strcmp(nm, "ButtonBindingModal")) || !has_visible_leaf(p, 1)) continue;
        if (name) *name = nm; return p; }
    return 0;
}
static uint64_t ui_find(uint64_t w, const char *name, int dep);
static uint64_t ui_panel(uint64_t root, const char *name);   // 只扫界面根的直接子控件（定义在后面） | Scans only the direct children of the UI root (defined later)
static int free_cursor_page(module_t *m) {
    static uint64_t at = UINT64_MAX; static int yes; static const char *whyp = ""; uint64_t fr = dm_guest_frame(); if (fr == at) return yes;
    // 有自由光标 = 开着的面板里有物品区 / 技能图标 / 属性「+」，并且上面没有盖着模态的一层（数量框、确认框、交易结果提示）：
    // 模态框开着时手柄输入归它——数量框里推摇杆 = 加减数字（数字自己涨）。
    // 原先只看「最上层那个面板」有没有物品区，鉴定 / 买卖后角上的存盘指示（SaveIndicatorPanel）正好在最上层的那一瞬间
    // 被判成没有自由光标，拿起物品的确认键被扣住（真机 2/2 复现）；佣兵头像栏 HireablesPanel 同理。现在只认真正模态的面板
    // Has a free cursor = an open panel has an item area / skill icons / attribute '+', and no modal layer covers it (quantity box, confirmation box, trade result prompt):
    // while a modal is open, controller input belongs to it -- pushing the stick in the quantity box = increasing / decreasing the number (the number goes up by itself).
    // Previously only 'the topmost panel' was checked for an item area; at the instant after identify / buy / sell when the save indicator in the corner (SaveIndicatorPanel) happened to be topmost,
    // it was classified as having no free cursor and the confirm button for picking up an item was held back (reproduced 2/2 on a real device); the same goes for the mercenary portrait bar HireablesPanel. Now only truly modal panels count
    uint64_t root = 0; memcpy(&root, m->base + 0x2f05458, 8); int was = yes; at = fr; yes = 0; const char *mn = "";
    if (root && free_walk(root, 0, (uint64_t)m->base)) { uint64_t dp;
        if (modal_any(root, 1, &mn)) yes = 0;
        else if ((dp = ui_find(root, "NpcDialogPanel", 0)) && *(uint8_t *)(dp + 0x50) && has_visible_leaf(dp, 1)) { yes = 0; mn = "NpcDialogPanel"; }
        // 下拉展开的选项（DropdownListContents，编年史页的「部位」等）：手柄输入归它，推摇杆 = 一行行往下移高亮（点一下就一直往下选）
        // Options of an expanded dropdown (DropdownListContents, e.g. 「部位」(Slot) on the chronicle page): controller input belongs to it, and pushing the stick = moving the highlight down row by row (one tap would keep selecting downward)
        else if ((dp = ui_find(root, "DropdownListContents", 0)) && *(uint8_t *)(dp + 0x50)) { yes = 0; mn = "DropdownListContents"; }
        else yes = 1; }
    if (yes != was || (!yes && mn != whyp && mn[0])) LOG("dm_cursor_apply：这一页%s自由光标%s%s%s", yes ? "有" : "没有", yes ? "" : "，光标请求不执行（不推摇杆）", mn[0] ? "；上面盖着 " : "", mn);
    whyp = mn;
    return yes;
}
static void cursor_apply(uint64_t *buttons, int32_t *lx, int32_t *ly);
static _Atomic uint64_t g_exact_xy = UINT64_MAX;   // 最近一次命中落在高级仓库成叠格自己矩形里的那个点（x | y<<32）：光标就停在这一格，不做「手指粗」吸附 | The point of the most recent hit that landed inside an advanced stash stacked cell's own rectangle (x | y<<32): the cursor stops on this cell, with no 'fat finger' snap
static _Atomic int g_sn_new;   // 新一次按下（dm_ui_item_query 置）：上一次的吸附 / 停靠作废。只按「请求点没变」续用会跨触摸残留——同一落点再按，光标还在停靠点，长按判成空格去整理背包 | A new press (set by dm_ui_item_query): the previous snap / parking is invalidated. Reusing it only on 'the request point has not changed' would leave residue across touches -- pressing the same landing point again, the cursor is still at the parking point, and the long-press is classified as an empty cell and sorts the inventory
static int sn_on; static _Atomic uint64_t g_snap;   // 就近吸附中；g_snap = 吸附记录（请求点 x | y<<16 | 落点 x<<32 | y<<48），长按据此分「贴着物品边」与「落在空格中间」 | Nearest snap in effect; g_snap = snap record (request point x | y<<16 | landing point x<<32 | y<<48); long-press uses it to tell 'next to an item's edge' from 'in the middle of an empty cell'
static _Atomic int g_free_pub;
// 真鼠标悬停复用原版焦点重算；不能套触屏的「胖手指」吸附，否则空格旁也会冒出邻格说明。
// Real mouse hover reuses the game's focus refresh, bypassing touch's fat-finger snap so empty cells do not show a neighbor's tooltip.
int dm_ui_mouse_hover_now(float nx, float ny) {
    module_t *m = main_module();
    uint64_t waiting = atomic_load(&g_a_wait);
    if (!m || dm_hud_held() || dm_cursor_pending() || (waiting && now_ns() - waiting < 300000000ull) || !free_cursor_page(m)) return 0;
    int w = *(int32_t *)(m->base + 0x211da38), h = *(int32_t *)(m->base + 0x211da3c);
    if (w <= 0 || h <= 0) return 0;
    int x, y;
    if (!(nx >= 0 && nx <= 1 && ny >= 0 && ny <= 1)) { x = w * 2 / 100; y = h / 2; }
    else { x = (int)(nx * w); y = (int)(ny * h); if (x >= w) x = w - 1; if (y >= h) y = h - 1; }
    { int dm_vault_ui_blocks(int, int); if ((dm_ui_panel_state() & DM_PS_VAULT) && dm_vault_ui_blocks(x, y)) return 0; }
    int ok = dm_ui_hover_now(x, y);
    if (getenv("DM_TEST_HOVER_LOG")) { dm_cursor_item_info ci = {0}; int found = dm_cursor_item(&ci); LOG("鼠标悬停刷新：(%d,%d) → %d 物品 %s id %u", x, y, ok, found ? ci.code : "-", ci.id); }
    return ok;
}
int dm_ui_free_cursor(void) { return atomic_load(&g_free_pub); }   // 当前页有没有自由光标（物品栏 / 技能树 / 属性页这类） | Whether the current page has a free cursor (pages such as inventory / skill tree / attributes)
void dm_cursor_apply(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    cursor_apply(buttons, lx, ly);
    // 装备对比（单击装备 = 自己的说明 + 身上那件并排，不加手势不加按钮）：原版是「按住来比较」，触摸没有按住的键——
    // 有物品区的面板开着期间一直开着对比位（dm_item_compare，不经手柄键、不翻页签），说明框出来时就并排带上身上同部位那件；面板关了收掉
    // Equipment comparison (single tap on equipment = its own description + the equipped item side by side, with no added gesture and no added button): the original game is 'hold to compare', and touch has no key to hold --
    // while a panel with an item area is open, the compare bit stays on (dm_item_compare, without going through controller keys and without flipping tabs), so when the tooltip appears it brings the equipped item of the same slot side by side; turned off when the panel closes
    { module_t *cm = main_module(); static int cmp = -1; int want = cm ? free_cursor_page(cm) : 0; atomic_store(&g_free_pub, want);
      int dm_padmode_current(void); int c = want && !dm_padmode_current(); if (c != cmp) { cmp = c; dm_item_compare(c); } }   // 手柄模式照原版：按住对比键才对比（符文之语清单也靠这一位认「按住对比键」） | Controller mode follows the original: compare only while the compare button is held (the runeword list also relies on this bit to detect "compare button held")
    if ((*buttons & DM_BTN_A) && atomic_load(&g_a_wait)) atomic_store(&g_a_wait, now_ns());   // A 还按着：兜底的 0.3 秒从松开那一刻起算 | A is still held: the fallback's 0.3 seconds are counted from the moment of release
}
static dm_wref g_down; static _Atomic uint64_t g_origin_xy;   // 按下那次命中的物品区控件；最近一次「拿起」的物品原位中心（位 63 = 有效，x | y << 32 的低 31 位） | Item-area widget hit by that press; center of the original position of the most recently 'picked up' item (bit 63 = valid, low 31 bits of x | y << 32)
// 「拿起」那一刻记下物品原位的中心：放回原处要放回这里。游戏以光标为中心放——放回手指最初按下的那个点的话，
// 按在长条物品的一端起拖，放回时整件偏一格、压到别的物品或出界被拒收，留在手上
// 拖着进不了腰带的物品（装备、护身符…）经过腰带那一排时不把光标挪上去：原版光标一到腰带格，多排腰带就向上展开，
// 盖住戒指位 / 腰带位，随后落在戒指位上的「放下」落到了弹层的药水格里、游戏不收（mini：鉴定过的戒指拖到戒指位 0 次换装；
// 帽子换头盔的路径没经过腰带所以通）。真人手指从背包往上拖到戒指位必经腰带这一排。药水 / 卷轴照常（要靠展开放进上面几排）。
// At the moment of 'pick up', record the center of the item's original position: putting it back means putting it back here. The game places with the cursor as the center -- if it were put back at the point where the finger first went down,
// a drag started by pressing on one end of a long item would put the whole item back one cell off, overlapping another item or out of bounds and rejected, leaving it in hand
// When dragging an item that cannot go into the belt (equipment, charms...) past the belt row, do not move the cursor onto it: as soon as the original game's cursor reaches a belt cell, a multi-row belt expands upward,
// covering the ring slot / belt slot, and the 'put down' that then lands on the ring slot lands in a potion cell of the popup layer and the game rejects it (observed on the second Mac: an identified ring dragged to the ring slot was equipped 0 times;
// the hat-to-helmet swap path does not pass the belt, so it works). A real finger dragging upward from the inventory to the ring slot must cross this belt row. Potions / scrolls behave as usual (they rely on the expansion to be put into the upper rows).
static int g_pick_beltable = 1;   // 最近一次拿起的那件能不能放腰带（拿起时记） | Whether the most recently picked-up item can go into the belt (recorded at pick-up)
static uint64_t ui_find(uint64_t w, const char *name, int deep);
static int belt_row_block(module_t *m, int x, int y) {
    if (g_pick_beltable) return 0; dm_hud_info hi; dm_hud_get(&hi); if (!hi.valid || !hi.holding) return 0;
    uint64_t root = 0; memcpy(&root, m->base + 0x2f05458, 8); uint64_t pn = root ? ui_find(root, "PlayerInventoryExpansionLayout", 1) : 0, b = pn && *(uint8_t *)(pn + 0x50) ? ui_find(pn, "Belt", 1) : 0;
    if (!b || !*(uint8_t *)(b + 0x50)) return 0;
    uint64_t xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32), ch = *(uint64_t *)(pn + 0x58), n = *(uint64_t *)(pn + 0x60);
    // 腰带控件的命中范围把上面几排（展开后的位置）也算在内，和戒指位 / 腰带位重叠（mini：落在戒指位上也命中腰带）——点在某个装备槽里就不算腰带
    // The belt widget's hit range also includes the rows above it (their positions when expanded), overlapping the ring slot / belt slot (observed on the second Mac: landing on the ring slot also hits the belt) -- a tap inside some equipment slot does not count as the belt
    for (uint64_t i = 0; ch && i < n && n <= 256; i++) { uint64_t c = *(uint64_t *)(ch + 8 * i); const char *nm = c ? (const char *)*(uint64_t *)(c + 8) : NULL;
        if (c && nm && !strncmp(nm, "slot_", 5) && *(uint8_t *)(c + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)c + 0x48))(c, xy) & 1)) return 0; }
    return ((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)b + 0x48))(b, xy) & 1;
}
static void origin_note(module_t *m, int x, int y) {
    dm_hud_info hi; dm_hud_get(&hi); if (hi.valid && hi.holding) return;   // 手上已有东西：这下是放下，不是拿起 | Something is already in hand: this press is a put-down, not a pick-up
    dm_cursor_item_info ci; uint64_t w = dm_wref_get(&g_down); atomic_store(&g_origin_xy, 0);
    if (!w || !dm_cursor_item(&ci)) return;
    g_pick_beltable = ci.beltable || ci.container == DM_ITEM_AT_BELT;
    int32_t vw = *(int32_t *)(m->base + 0x211da38), vh = *(int32_t *)(m->base + 0x211da3c);
    uint64_t fn = *(uint64_t *)(*(uint64_t *)w + 0x48); int x0 = x, x1 = x, y0 = y, y1 = y;
#define ORG_HIT(px, py) (((int (*)(uint64_t, uint64_t))fn)(w, (uint64_t)(uint32_t)(px) | ((uint64_t)(uint32_t)(py) << 32)) & 1)
    if (!ORG_HIT(x, y)) return;
    while (x0 > 1 && ORG_HIT(x0 - 2, y)) x0 -= 2; while (x1 < vw - 2 && ORG_HIT(x1 + 2, y)) x1 += 2;
    while (y0 > 1 && ORG_HIT(x, y0 - 2)) y0 -= 2; while (y1 < vh - 2 && ORG_HIT(x, y1 + 2)) y1 += 2;
    int ox = (x0 + x1) / 2, oy = (y0 + y1) / 2;   // 装备位：整个槽的中心 | Equipment slot: center of the whole slot
    if (ci.container == DM_ITEM_AT_BELT) { int cw = (x1 - x0) / 4; if (cw > 0) { int c = (x - x0) / cw, r = (y - y0) / cw; ox = x0 + c * cw + cw / 2; oy = y0 + r * cw + cw / 2; if (oy > y1) oy = (y0 + y1) / 2; } }
    else if (ci.container != DM_ITEM_AT_EQUIP && ci.gw > 0 && ci.gh > 0) { double cell = 112.5 * vh / 2160.0; int cols = (int)lround((x1 - x0) / cell), rows = (int)lround((y1 - y0) / cell);
        if (cols > 0 && rows > 0) { ox = x0 + (int)((ci.gx + ci.gw / 2.0) * (x1 - x0) / cols); oy = y0 + (int)((ci.gy + ci.gh / 2.0) * (y1 - y0) / rows); } }
    atomic_store(&g_origin_xy, (1ull << 63) | (uint64_t)(uint32_t)ox | ((uint64_t)(uint32_t)oy << 32));
}
int dm_ui_origin_point(float *nx, float *ny) {   // 最近一次「拿起」的物品原位中心（归一化）；0 = 没有 | Center of the original position of the most recently 'picked up' item (normalized); 0 = none
    module_t *m = main_module(); if (!m) return 0; int32_t vw = *(int32_t *)(m->base + 0x211da38), vh = *(int32_t *)(m->base + 0x211da3c); uint64_t v = atomic_load(&g_origin_xy);
    if (!(v >> 63) || vw <= 0 || vh <= 0) return 0; *nx = (float)(((v & 0xffffffff) + 0.5) / vw); *ny = (float)((((v >> 32) & 0x7fffffff) + 0.5) / vh); return 1;
}
// 这下 A 是不是落在刚算出的拖放落点上：是且手上拿着物品，就登记「放不下再试」（往里挪一格）。排队放出与直通两条路径都要登记
// 松手必有结局的最后一道（背包宝石拖到箱子里被占的格 = 原版交换，换下来的盾留在手上，之后页签 / 取金币 / 双击全被当成「放下」）：
// 手上拿着东西时按下的每一个 A（= 一次「放下」）都登记；0.3 秒后手上还有东西就收尾——
//   还是同一件、落在多格区域、且这件高过一格：往里挪一格再放（至多两次，见上）；
//   换成了别的物品（交换）或放不下：把手上这件放到被拖物品腾出的原位（g_pick_xy）；
//   还在手上：收进背包（dm_held_stow：原版「给物品在背包里找位置并放入」的同一个函数，不关面板）；
//   背包也满：按一下返回——原版手上有物品时按返回 = 关面板并由游戏自己处理（mini 实测背包有位时发 0x18、手上清空）。
// Whether this A lands on the drag-and-drop landing point just computed: if so and an item is held in hand, register 'retry if it cannot be put down' (move one cell inward). Both the queued-release path and the pass-through path must register
// The last line of defense for 'every release must have an outcome' (an inventory gem dragged onto an occupied cell in the stash = original-game swap, the swapped-out shield stays in hand, and afterwards tabs / taking gold / double taps are all treated as 'put down'):
// every A pressed while something is held in hand (= one 'put down') is registered; if something is still in hand 0.3 seconds later, wrap up --
//   still the same item, landed in a multi-cell area, and the item is taller than one cell: move one cell inward and put down again (at most twice, see above);
//   changed to another item (swap) or cannot be put down: put the item in hand into the original position vacated by the dragged item (g_pick_xy);
//   still in hand: stow it into the inventory (dm_held_stow: the same function as the original game's 'find a place in the inventory for the item and put it in', without closing the panel);
//   inventory also full: press back once -- in the original game pressing back with an item in hand = close the panel and let the game handle it itself (measured on the second Mac: with room in the inventory it sends 0x18 and the hand is emptied).
static int g_fb_used, g_fb_nudge;   // 这次松手走过「直接调原版放下」的兜底；兜底成功后要把光标从腰带控件压着的那一带挪开 | This release went through the 'call the original game's put-down directly' fallback; after the fallback succeeds the cursor must be moved out of the band covered by the belt widget
static _Atomic int g_drop_loc;   // 最近一次松手落在装备槽上时那个槽的身体栏位（控件 +0x5d8）；0 = 落点不是装备槽 | Body location of the equipment slot (widget +0x5d8) the last time a release landed on an equipment slot; 0 = the landing point is not an equipment slot
static _Atomic uint64_t g_pick_xy;   // 最近一次「空手按 A 拿起」那件物品的原位中心（位 63 = 有效）；放下时的 A 不改它 | Center of the original position of the item most recently 'picked up by pressing A with an empty hand' (bit 63 = valid); the A of a put-down does not change it
static _Atomic uint64_t g_set_back_at;
#define DROP_RETRY_ARM() do { dm_hud_info hi; dm_hud_get(&hi); if (g_rt.active && g_rt.stage) { /* 收尾自己按的 A：不重新登记 | The A pressed by the wrap-up itself: do not register again */ } else if (hi.valid && hi.holding) { int grid = g_dc.valid && abs(tx - g_dc.x) <= 5 && abs(ty - g_dc.y) <= 5; \
    g_rt = (drop_retry){ .active = 1, .id = hi.holding, .x = tx, .y = ty, .cell_h = grid ? g_dc.cell_h : 0, .r = grid ? g_dc.r : 0, .rows = grid ? g_dc.rows : 1, .t = now_ns() }; g_dc.valid = 0; } else g_rt.active = 0; } while (0)
// 手上物品守门（手上拿着东西时按合成，服务端校验 0x6e8740 直接丢掉 0x20 包，不出声不提示；触摸上光标看不见，手上那件玩家也看不见）。
// 规矩：触摸界面上不许有「看不见的手上物品」。上面的松手收尾只在我们替玩家按了「放下」的 A 之后才登记，别的路径（交换后留在手上、面板切换、
// 不经我们的 A 拿起…）没人兜。这里补一道总的：前端报「触摸已空闲」（没有手指、没有在发 / 排着的键与光标请求）满 0.6 秒、手上仍有物品，
// 就走同一条收尾——是刚才拿起的那件（g_pick_id）且这一页有自由光标：放回拿起时的原格；否则收进背包（dm_held_stow），背包也满再按返回交给游戏。
// 不管：鉴定 / 打孔等「等选目标」态（手上是那张卷轴）、单件任务槽面板（槽里那件就是手上那件）、上一条收尾还在走、手上那件有待确认操作；同一件收尾失败过就不再反复收。
// 手柄模式也不管：原版手柄界面上手上那件就画在光标上、玩家看得见，守门的前提不成立；而且手柄模式没有手指，「触摸已空闲」恒为真，
// 拿起物品 0.6 秒就会被收走（线上手柄版实跑确认）。切回触屏模式时手上还拿着的，照常由守门收。
// Held-item guard (if Transmute is pressed while an item is held, the server-side check at 0x6e8740 simply drops the 0x20 packet, with no sound and no
// message; on touch the cursor is invisible, so the player cannot see the held item either).
// Rule: the touch UI must never have an "invisible held item". The release cleanup above is registered only after we have pressed the "put down" A on
// the player's behalf; other paths (an item left in hand after a swap, panel switches,
// pickups that do not go through our A, ...) have no safety net. This adds a general one: once the front end has reported "touch idle" (no fingers, no
// keys or cursor requests being sent or queued) for a full 0.6 seconds and an item is still held,
// the same cleanup runs: if it is the item just picked up (g_pick_id) and this page has a free cursor, put it back in the cell it was picked up from;
// otherwise stow it in the inventory (dm_held_stow), and if the inventory is full too, press Back and hand it to the game.
// Not handled: "waiting for a target" states such as identify / socketing (the held item is that scroll), single-item quest slot panels (the item in the
// slot is the held item), a previous cleanup still in progress, a held item with an operation pending confirmation; an item whose cleanup has failed
// once is not retried over and over.
// Controller mode is not handled either: in the original controller UI the held item is drawn on the cursor and the player can see it, so the guard's
// premise does not hold; besides, controller mode has no fingers, so "touch idle" is always true,
// and a picked-up item would be taken away after 0.6 seconds (confirmed by running the released controller build). An item still held when switching back to
// touch mode is handled by the guard as usual.
static _Atomic int g_hand_idle; static uint64_t g_pick_t; static int g_pick_id;
void dm_ui_hand_idle(int idle) { atomic_store(&g_hand_idle, idle != 0); }
static void hand_guard(module_t *m) {
    dm_hud_info hi; dm_hud_get(&hi); int h = hi.valid ? hi.holding : 0; uint64_t now = now_ns();
    static int last, failed, off = -1; static uint64_t since; if (off < 0) { const char *e = getenv("DM_TEST_HAND_GUARD"); off = e && !strcmp(e, "0"); }   // 测试开关 DM_TEST_HAND_GUARD=0：关掉守门（单测合成钮兜底用） | Test switch DM_TEST_HAND_GUARD=0: turn the guard off (used by the unit test for the transmute button fallback)
    if (h && h != last) g_pick_id = now - g_pick_t < 1500000000ull ? h : 0;   // 手上换成这件：是不是刚空手按 A 拿起的 | The hand now holds this item: was it just picked up by pressing A with an empty hand
    last = h; if (!h) failed = 0;
    int dm_padmode_current(void);
    if (off || !h || h == failed || !atomic_load(&g_hand_idle) || dm_padmode_current() || g_rt.active || g_naq || dm_held_pending() || dm_ui_targeting() || (dm_ui_panel_state() & DM_PS_IMBUE)) { since = 0; return; }
    if (!since) { since = now; return; }
    if (now - since < 600000000ull) return;
    since = 0; failed = h;   // 这一轮收完还在手上（换了别的物品会重新算）就不再收同一件 | If it is still in hand after this round of wrap-up (a change to another item recomputes), do not wrap up the same item again
    int back = (atomic_load(&g_pick_xy) >> 63) && g_pick_id == h && m && free_cursor_page(m);
    g_rt = (drop_retry){ .active = 1, .id = h, .rows = 1, .stage = back ? 0 : 1, .t = now - 400000000ull }; atomic_store(&g_drop_loc, 0);
    LOG("手上物品守门：触摸已空闲 0.6 秒、手上还拿着（id %d）→ %s", h, back ? "放回拿起时的原格" : "收进背包");
}
static void cursor_apply(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    hand_guard(main_module());
    static uint64_t prev_a;
    uint64_t a_now = *buttons & DM_BTN_A, a_edge = a_now && !prev_a;
    prev_a = a_now;
    static int y_wait; uint64_t y_now = *buttons & DM_BTN_Y; if (!y_now) y_wait = 0;   // Y（丢弃 / 按住移到另一侧）同样要等光标到位才放行：没到位就按 = 对着别的物品丢弃 | Y (drop / hold to move to the other side) likewise has to wait for the cursor to be in position before it is let through: pressing before it is in position = dropping some other item
    uint64_t r = atomic_load(&g_req);
    // 0.3 秒只是检查间隔：手上那件还有待确认的操作（上一个指令服务端没答完）时不收尾——这时挪格重放 / 放原位会落空或落到别处
    // （用游戏自己的待确认标志，不把「过了一段时间」当成服务端已处理）；最多等 2 秒
    // 单件任务槽面板开着时，手上那件就是「槽里那件」（原版的进槽 = 拿在手上），手上有东西是常态，不是「放下没成」：不收尾。
    // 槽占用时去拖背包里别的物品，这段把槽里的长剑放回了背包原格，绕过了槽控件——槽上留着剑的图，点确认发出的成交包服务端不收
    // 0.3 seconds is only the check interval: while the item in hand still has an operation pending confirmation (the server has not finished answering the previous command), do not wrap up -- moving a cell and putting down again / putting back in the original position would miss or land somewhere else
    // (use the game's own pending-confirmation flag, and do not treat 'some time has passed' as the server having processed it); wait at most 2 seconds
    // While the single-item task slot panel is open, the item in hand is 'the item in the slot' (in the original game, entering the slot = holding it in hand); having something in hand is the normal state, not 'the put-down failed': do not wrap up.
    // When the slot was occupied and another item in the inventory was dragged, this section put the long sword in the slot back into its original inventory cell, bypassing the slot widget -- the sword's image stayed on the slot, and the transaction packet sent by tapping confirm was rejected by the server
    if (g_rt.active && (dm_ui_panel_state() & DM_PS_IMBUE)) g_rt.active = 0;
    // 「按返回」这一步（会关玩家的面板）对同一件手上物品只做一轮：g_back_id = 已经为它按过返回的那件；手上空了（或换了别的）才清。
    // 原先没有上限：手上读数是假的时（卡死：服务端光标挂着已进仓库的魔盒），每拖一次物品就整条走一遍、按两次返回，刚开的面板一开就被关
    // The "press Back" step (which closes the player's panels) runs only one round per held item: g_back_id = the item for which Back has already been pressed; cleared only when the hand empties (or holds something else).
    // Previously there was no limit: when the held-item reading was false (the hang: the server cursor still holding a Horadric Cube already in the stash), every drag ran the whole chain and pressed Back twice, so a freshly opened panel closed immediately
    static int g_back_id; { dm_hud_info h0; dm_hud_get(&h0); if (!h0.holding || (g_back_id && h0.holding != g_back_id)) g_back_id = 0; }
    if (g_rt.active && !g_rt.pending && now_ns() - g_rt.t > 300000000ull && !(dm_held_pending() && now_ns() - g_rt.t < 2000000000ull)) {
        dm_hud_info hi; dm_hud_get(&hi);
        uint64_t pk = atomic_load(&g_pick_xy);
        if (!hi.valid || !hi.holding) { g_rt.active = 0; if (g_fb_used) { g_fb_used = 0; g_fb_nudge = 1; } }   // 手上空了：这次松手有结局了 | Hand is empty: this release has reached an outcome
        else if (g_naq >= AQ_MAX) g_rt.active = 0;
        else if (g_rt.stage == 0 && hi.holding == g_rt.id && g_rt.tries < 2 && g_rt.rows > 1 && ({ int hw_ = 0, hh_ = 0; int dm_held_size(int *, int *); dm_held_size(&hw_, &hh_) && hh_ > 1; })) {   // 只有高过一格的物品才会因为贴着区域下沿放不下；一格高的还在手上 = 这一格不收它（例：未鉴定珠宝拖到带孔装备上服务端不收），再挪一格只会和旁边的物品连环交换 | Only items taller than one cell can fail to be put down because they touch the bottom edge of the area; a one-cell-tall item still in hand = this cell does not accept it (e.g. an unidentified jewel dragged onto socketed equipment is rejected by the server), and moving one more cell would only chain-swap it with the neighboring items
            int nr = g_rt.r + (2 * g_rt.r + 1 < g_rt.rows ? 1 : -1); g_rt.y += (nr - g_rt.r) * g_rt.cell_h; g_rt.r = nr; g_rt.tries++;
            LOG("dm_cursor_apply：放下后物品还在手上，往里挪一格到 (%d,%d) 再放（第 %d 次）", g_rt.x, g_rt.y, g_rt.tries);
            g_rt.pending = 1; g_aq[g_naq++] = (aq_entry){g_rt.x, g_rt.y, now_ns()}; }
        else if (g_rt.stage == 0 && hi.holding == g_rt.id && atomic_load(&g_drop_loc) > 0) {   // 落在装备槽上、按了确认键物品还在手上：焦点可能不在这个槽上（戒指位上沿被腰带控件压着），直接调原版放下函数 | Landed on an equipment slot, and the item is still in hand after the confirm button was pressed: the focus may not be on this slot (the top edge of the ring slot is covered by the belt widget), so call the original game's put-down function directly
            int loc = atomic_exchange(&g_drop_loc, 0); void dm_held_equip(int); dm_held_equip(loc); g_rt.t = now_ns(); g_fb_used = 1;
            LOG("dm_cursor_apply：放在装备槽上没放下（手上 id %d）→ 直接调原版放下，栏位 %d", hi.holding, loc); }
        else if (g_rt.stage == 0 && (pk >> 63)) { g_rt.stage = 1; g_rt.x = (int)(pk & 0x7fffffff); g_rt.y = (int)(pk >> 32 & 0x7fffffff);
            LOG("dm_cursor_apply：放下后手上还有物品（%s，id %d）→ 放到被拖物品腾出的原位 (%d,%d)", hi.holding == g_rt.id ? "没放下" : "换下来的那件", hi.holding, g_rt.x, g_rt.y);
            g_rt.pending = 1; g_aq[g_naq++] = (aq_entry){g_rt.x, g_rt.y, now_ns()}; }
        else if (g_rt.stage < 2) { g_rt.stage = 2; g_rt.t = now_ns(); dm_held_stow();
            LOG("dm_cursor_apply：原位也放不下（手上 id %d）→ 收进背包（不关面板）", hi.holding); }
        else if (g_rt.stage == 2 && dm_held_stow_result() < 0 && now_ns() - g_rt.t < 2000000000ull) { /* dm_held_stow 没放成会逐帧重试约 1.5 秒，这段时间结果是 -1：等它出结果 | If dm_held_stow does not manage to stow, it retries every frame for about 1.5 seconds, and during that time the result is -1: wait for it to produce a result */ }
        else if (g_rt.stage == 2 && g_back_id == hi.holding) { g_rt.active = 0; static int lg; if (lg++ < 20) LOG("【红】dm_cursor_apply：手上这件（id %d）已按过一轮返回仍没放下 → 不再按返回（不关玩家的面板），等手上换了再说", hi.holding); }
        else if (g_rt.stage == 2) { g_rt.stage = 3; g_rt.t = now_ns(); g_back_id = hi.holding; atomic_store(&g_set_back_at, dm_guest_frame() + 1);
            LOG("dm_cursor_apply：背包也收不进（手上 id %d，结果 %d）→ 按返回，交给游戏自己处理", hi.holding, dm_held_stow_result()); }
        else if (g_rt.stage == 3) { g_rt.stage = 4; g_rt.t = now_ns(); atomic_store(&g_set_back_at, dm_guest_frame() + 1);   // 原版按返回分两步：面板开着时第一下只关面板、手上那件留着；面板外再按一下才由游戏自己处理——背包有位放背包（0x18），没位丢在脚下（0x17，玩家看得见、能捡回） | In the original game pressing back takes two steps: while a panel is open the first press only closes the panel and the item in hand is kept; pressing once more outside the panel is then handled by the game itself -- put into the inventory if there is room (0x18), dropped at the feet if not (0x17, visible to the player and can be picked back up)
            LOG("dm_cursor_apply：按返回只关了面板、手上还拿着（id %d）→ 再按一次返回，由游戏放背包或丢在脚下", hi.holding); }
        else { g_rt.active = 0; LOG("【红】dm_cursor_apply：按两次返回后手上仍有物品（id %d），收尾放弃", hi.holding); }
    }
    if (!(r >> 63) && !g_rt.pending) { g_naq = 0; atomic_store(&g_naq_pub, 0); return; }
    module_t *m = main_module();
    static int warned, logged, last_ix = -1, last_iy = -1;
    if (!m) { if (!warned++) LOG("【红】dm_cursor_apply：没有 main 模块，跳过"); return; }
    if (!free_cursor_page(m)) { uint64_t mine = (DM_BTN_A | DM_BTN_Y) & ~dm_ui_auto_buttons();   /* 宿主自己替玩家按的键（对话菜单的确认、设置页的返回）照常放行 | Keys the host presses on the player's behalf (confirm in the dialogue menu, back on the settings page) pass through as usual */
        if (*buttons & mine) { static int lg; if (lg++ < 20) LOG("dm_cursor_apply：这一页没有自由光标，手势层带着光标请求送来的确认 / 丢弃键不放行（防止被模态框吃掉）"); *buttons &= ~mine; }
        g_naq = 0; g_rt.active = g_rt.pending = 0; atomic_store(&g_naq_pub, 0); return; }
    int32_t width = *(int32_t *)(m->base + 0x211da38);
    int32_t height = *(int32_t *)(m->base + 0x211da3c);
    if (width <= 0 || height <= 0) { if (!warned++) LOG("【红】dm_cursor_apply：原版视口 %dx%d 无效，跳过", width, height); return; }
    int ix = (int)(((r >> 16) & 0xffff) / 65535.0 * width), iy = (int)((r & 0xffff) / 65535.0 * height);
    if (!(r >> 63)) { ix = g_rt.x; iy = g_rt.y; }   // 手指已抬起、只剩「往里挪再放」那一下 | The finger is already up; only the 'move inward and put down again' press remains
    static int sn_tx, sn_ty, sn_x, sn_y;   // 就近吸附：请求位置 (sn_tx,sn_ty) 已改落到 (sn_x,sn_y)；请求没变就一直用吸附后的位置（含随后的 A） | Nearest snap: the requested position (sn_tx,sn_ty) has been changed to land at (sn_x,sn_y); as long as the request does not change, the snapped position keeps being used (including the A that follows)
    // 兜底装上之后：光标还停在被腰带控件压着的那一带，焦点在腰带上 → 腰带展开并挂出药水说明框，要等说明框自收才消失。
    // 腰带一旦展开，弹层把整个戒指位都盖住，往槽里挪几个像素没用（试过）；把光标送到屏幕左缘的停靠点（说明框自收用的同一个位置），
    // 腰带收起、不挂说明框；手势层的请求没变就一直停在那
    // After the fallback has equipped it: the cursor still sits in the band covered by the belt widget and the focus is on the belt -> the belt expands and shows the potion tooltip, which only goes away when the tooltip closes by itself.
    // Once the belt expands, the popup layer covers the whole ring slot, and moving a few pixels into the slot does not help (tried); send the cursor to the parking point at the left edge of the screen (the same position used when the tooltip closes by itself),
    // the belt collapses and no tooltip is shown; it stays there as long as the gesture layer's request does not change
    if (atomic_exchange(&g_sn_new, 0)) sn_on = 0;
    if (g_fb_nudge) { g_fb_nudge = 0; sn_on = 1; sn_tx = ix; sn_ty = iy; sn_x = width * 2 / 100; sn_y = height / 2; dm_ui_hover_now(sn_x, sn_y); LOG("dm_cursor_apply：兜底装上后光标送到停靠点 (%d,%d)，离开腰带", sn_x, sn_y); }
    if (sn_on && ix == sn_tx && iy == sn_ty) { ix = sn_x; iy = sn_y; } else sn_on = 0;
    if (ix >= width) ix = width - 1;
    if (iy >= height) iy = height - 1;
    int32_t cx = *(int32_t *)(m->base + 0x2d128f8);
    int32_t cy = *(int32_t *)(m->base + 0x2d128fc);
    int arrived_now = (int)hypot((double)(ix - cx), (double)(iy - cy)) <= DM_CURSOR_NEAR;
    if (a_edge && (g_naq || !arrived_now)) {
        if (g_naq < AQ_MAX) g_aq[g_naq++] = (aq_entry){ix, iy, now_ns()};
        else LOG("【红】dm_cursor_apply：扣住的 A 超过 %d 个，丢弃 (%d,%d)", AQ_MAX, ix, iy);
    }
    while (g_naq && now_ns() - g_aq[0].t > 3000000000ull) {   // 3 秒没到位：报红、丢掉这下 A。计时从它成为队首（光标开始朝它走）起算： | Not in position after 3 seconds: report a failure and drop this A. The timer starts when it becomes the head of the queue (the cursor starts moving toward it):
        // 箱子 ↔ 背包跨半个屏幕，推摇杆走过去要 1.5 秒左右，原先按入队时刻算 1.5 秒，排在后面的「放下」还没轮到就超时了（z3：盾拖不进背包）
        // Stash ↔ inventory spans half the screen, and pushing the stick across takes about 1.5 seconds; previously the 1.5 seconds were counted from enqueue time, so the 'put down' queued behind timed out before its turn came (z3: the shield could not be dragged into the inventory)
        LOG("【红】dm_cursor_apply：目标 (%d,%d) 3 秒未到位，放弃这次 A", g_aq[0].x, g_aq[0].y);
        memmove(g_aq, g_aq + 1, --g_naq * sizeof g_aq[0]); if (g_naq) g_aq[0].t = now_ns();
    }
    int tx = g_naq ? g_aq[0].x : ix, ty = g_naq ? g_aq[0].y : iy;
    static int an_x = -100000, an_y = -100000;   // 最近一次 A 按下沿交出去时的位置 | Position at the time the most recent A press edge was handed over
    int dx = tx - cx, dy = ty - cy, distance = (int)hypot((double)dx, (double)dy);
    // 远处直接落位：推摇杆每秒只走约 200 像素，武器格 ↔ 背包一趟 1 秒——玩家 1 秒一个拖放时「放下」拖到下一个手势里才发（z1：0 / 5）。
    // 离目标超过 DM_CURSOR_JUMP 时把游戏的光标坐标直接写到目标跟前 DM_CURSOR_LAND 像素处，最后一小段照旧推摇杆走：
    // 游戏只在自己挪光标时重算悬停（mini 实测：整段直接写到目标上，光标圈到了、悬停还停在原来那格，A 拿走的是原来那格的药水）。
    // 游戏另有一份内部光标位置（浮点，main+0x2a9ae0 的对象 +0x18 / +0x1c）：摇杆在死区内的那一拍才从这个全局重新取值，摇杆推着时按内部位置积分再写回全局。
    // 所以写完要先让摇杆回中 DM_CURSOR_SYNC_POLLS 次采样等它取走，再推；写的同一拍就推，下一拍光标弹回原处（mini z1r：3 秒到不了位）。
    // 一步到位（dm_ui_hover_now）：光标不在目标上时，直接设坐标并让游戏当场重算悬停 / 焦点 / 选中格——
    // 不用再「写到跟前 → 摇杆回中 → 推一下」等三次采样（约 100 毫秒）。此刻不是手柄光标模式（返回 0）才走下面的老路。
    // 确认键的按下沿交给游戏后，游戏要到它自己处理输入时才派发「手柄动作开始」（实测晚 10 毫秒以上、隔一次取手柄）；这之前光标不能动，否则动作落在新位置（拖放的「拿起」落到空格）。
    // 用确定信号：按下沿置位，收到 ControllerActionEnd 清位，期间光标钉在原地
    // Land directly from far away: pushing the stick moves only about 200 pixels per second, and one trip weapon slot ↔ inventory takes 1 second -- when the player does one drag-and-drop per second the 'put down' is delayed into the next gesture before it is sent (one regression case: 0 / 5).
    // When farther than DM_CURSOR_JUMP from the target, write the game's cursor coordinates directly to DM_CURSOR_LAND pixels short of the target, and cover the last short stretch by pushing the stick as before:
    // the game recomputes hover only when it moves the cursor itself (measured on the second Mac: writing the whole way onto the target, the cursor ring got there but the hover stayed on the original cell, and A took the potion in the original cell).
    // The game also has an internal cursor position (floating point, +0x18 / +0x1c of the object at main+0x2a9ae0): only on a tick where the stick is inside the dead zone does it re-read the value from this global; while the stick is pushed it integrates from the internal position and writes back to the global.
    // So after writing, first center the stick for DM_CURSOR_SYNC_POLLS samples to wait for it to be picked up, then push; pushing on the same tick as the write makes the cursor bounce back to where it was on the next tick (one regression case on the second Mac: not in position after 3 seconds).
    // One-step placement (dm_ui_hover_now): when the cursor is not on the target, set the coordinates directly and have the game recompute hover / focus / selected cell on the spot --
    // no more waiting three samples (about 100 ms) for 'write short of the target -> center the stick -> push once'. The old path below is taken only when this is not controller cursor mode at the moment (returns 0).
    // After the press edge of the confirm button is handed to the game, the game dispatches 'controller action start' only when it processes input itself (measured more than 10 ms later, one controller read apart); until then the cursor must not move, otherwise the action lands at the new position (the 'pick up' of a drag-and-drop lands on an empty cell).
    // Use a definite signal: set on the press edge, cleared on receiving ControllerActionEnd, and in between the cursor is pinned in place
    uint64_t aw = atomic_load(&g_a_wait); int waiting = aw && now_ns() - aw < 300000000ull; if (aw && !waiting) atomic_store(&g_a_wait, 0);
    if (waiting) { tx = cx; ty = cy; dx = dy = 0; distance = 0; }
    else if (belt_row_block(m, tx, ty)) { tx = cx; ty = cy; dx = dy = 0; distance = 0; }   // 拖着进不了腰带的物品路过腰带：光标留在原处，不让腰带展开 | Dragging an item that cannot go into the belt past the belt: the cursor stays where it is so the belt does not expand
    else if (distance > 0 && dm_ui_hover_now(tx, ty)) { DM_LAT("hover_now %d,%d", tx, ty); cx = tx; cy = ty; dx = dy = 0; distance = 0;
        // 手指粗：落点那格是空的、手上又没拿东西（不是在找空格放下）时，在半格内由近到远找有物品的格，光标落到那里。
        // 一步到位是精确到格的；推摇杆的老路在物品边缘外几像素也拿得起（z3：盾下沿外 9 像素处起拖，老路 5/5、新路 0/5）
        // Fat finger: when the cell at the landing point is empty and nothing is held in hand (not looking for an empty cell to put something down), search within half a cell, nearest first, for a cell with an item, and the cursor lands there.
        // The one-step placement is exact to the cell; the old stick-pushing path could also pick up a few pixels outside the item's edge (z3: starting a drag 9 pixels below the shield's bottom edge, old path 5/5, new path 0/5)
        dm_cursor_item_info ci; dm_hud_info hi; dm_hud_get(&hi);
        // 例外：高级仓库的宝石 / 材料 / 符文页每格是固定物品位，手指落在某一格自己的矩形里就认那一格——空格就是空格，
        // 不吸到旁边那一叠上（否则双击空格取走的是邻格的东西）
        // Exception: on the gem / material / rune pages of the advanced stash every cell is a fixed item position; a finger landing inside a cell's own rectangle selects that cell -- an empty cell is an empty cell,
        // and it does not snap to the stack next to it (otherwise double-tapping an empty cell would take the contents of the neighboring cell)
        if (!g_naq && !(hi.valid && hi.holding) && !dm_cursor_item(&ci) && atomic_load(&g_exact_xy) != ((uint64_t)(uint32_t)tx | (uint64_t)(uint32_t)ty << 32)) {
            int st = height * 20 / 750, found = 0; static const int ox[8] = { 0, 0, -1, 1, -1, 1, -1, 1 }, oy[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
            for (int k = 0; k < 8 && !found; k++) { int px = tx + ox[k] * st, py = ty + oy[k] * st; if (px < 0 || py < 0 || px >= width || py >= height) continue;
                if (dm_ui_hover_now(px, py) && dm_cursor_item(&ci)) { found = 1; sn_on = 1; sn_tx = tx; sn_ty = ty; sn_x = px; sn_y = py; atomic_store(&g_snap, (uint64_t)(tx & 0xffff) | (uint64_t)(ty & 0xffff) << 16 | (uint64_t)(px & 0xffff) << 32 | (uint64_t)(py & 0xffff) << 48); cx = px; cy = py; DM_LAT("hover_snap %d,%d", px, py); } }
            if (!found) dm_ui_hover_now(tx, ty); } }
    static int sync_left;
    { static int lt; int tg = tx * 4096 + ty; if (tg != lt) { lt = tg; DM_LAT("target %d,%d d=%d", tx, ty, distance); } }
    if (distance > DM_CURSOR_JUMP && !sync_left) {
        sync_left = DM_CURSOR_SYNC_POLLS + 1; DM_LAT("jump");
        cx = tx - (int)lround(dx * (double)DM_CURSOR_LAND / distance); cy = ty - (int)lround(dy * (double)DM_CURSOR_LAND / distance);
        *(int32_t *)(m->base + 0x2d128f8) = cx; *(int32_t *)(m->base + 0x2d128fc) = cy;
        dx = tx - cx; dy = ty - cy; distance = (int)hypot((double)dx, (double)dy);
    }
    if (sync_left && --sync_left) {   // 等游戏取走新位置：摇杆回中、A / Y 都先压着（这下 A 还在队里或还按着，下面照常处理） | Waiting for the game to pick up the new position: stick centered, A / Y both held back for now (this A is still in the queue or still pressed; handled as usual below)
        *lx = *ly = 0; *buttons &= ~DM_BTN_A; if (y_now) { *buttons &= ~DM_BTN_Y; y_wait = 1; }
        atomic_store(&g_naq_pub, g_naq + y_wait); return;
    }
    if (g_naq) {
        *buttons &= ~DM_BTN_A;
        if (waiting) { *lx = *ly = 0; }   // 上一下 A 游戏还没派发：下一下先扣着 | The game has not dispatched the previous A yet: hold back the next one for now
        else if (distance <= DM_CURSOR_NEAR) { *buttons |= DM_BTN_A; *lx = *ly = 0;
            if (g_rt.pending) { g_rt.pending = 0; g_rt.t = now_ns(); }
            else { DROP_RETRY_ARM(); }
            DM_LAT("a_out %d,%d", cx, cy);
            origin_note(m, tx, ty);
            an_x = tx; an_y = ty; atomic_store(&g_a_wait, now_ns());
            memmove(g_aq, g_aq + 1, --g_naq * sizeof g_aq[0]); if (g_naq) g_aq[0].t = now_ns(); static int lg; if (lg++ < 60) LOG("dm_cursor_apply：到位 (%d,%d)，放出扣住的 A", cx, cy); }
        else stick_toward(dx, dy, distance, lx, ly);
    } else if (distance <= DM_CURSOR_NEAR) {
        *lx = *ly = 0;                       // 到位：A 原样通过（长按 = 按住） | In position: A passes through unchanged (long-press = held)
        if (a_edge) { dm_hud_info ph; dm_hud_get(&ph); int empty = !(ph.valid && ph.holding); origin_note(m, tx, ty); if (empty) { atomic_store(&g_pick_xy, atomic_load(&g_origin_xy)); g_pick_t = now_ns(); } an_x = tx; an_y = ty; atomic_store(&g_a_wait, now_ns()); DROP_RETRY_ARM(); }
        else if (a_now && !waiting && (abs(tx - an_x) > DM_CURSOR_NEAR || abs(ty - an_y) > DM_CURSOR_NEAR)) *buttons &= ~DM_BTN_A;   // 目标已离开锚点：这下 A 到此为止，等下一个按下沿 | The target has left the anchor: this A press ends here; wait for the next press edge
        { static int was; int tgt = tx * 4096 + ty; if (tgt != was) { was = tgt; DM_LAT("arrive %d,%d", tx, ty); } if (a_edge) DM_LAT("a_pass %d,%d", cx, cy); if (y_now && !y_wait) { static uint64_t yl; if (now_ns() - yl > 500000000ull) DM_LAT("y_pass"); yl = now_ns(); } }
    } else {
        *buttons &= ~DM_BTN_A;
        stick_toward(dx, dy, distance, lx, ly);
    }
    if (y_now) { if (g_naq || distance > DM_CURSOR_NEAR) { *buttons &= ~DM_BTN_Y; y_wait = 1; } else y_wait = 0; }
    atomic_store(&g_naq_pub, g_naq + y_wait);
    if ((tx != last_ix || ty != last_iy) && logged < 40) { logged++; last_ix = tx; last_iy = ty; LOG("dm_cursor_apply：目标 (%d,%d)，原生 (%d,%d)，距 %d，扣住 A %d", tx, ty, cx, cy, distance, g_naq); }
}

// ---------- 原生点击：往界面根控件注入鼠标消息（面板触摸路线） ----------
// Switch 版主程序保留了 PC 的鼠标消息处理（各面板 / 通用控件类里与 LeftMouseButtonDown 等哈希比较的代码点几十处）：
// 往界面根控件（main+0x2f05458 指的 PanelManager，消息处理 = 虚表 +0x28）发 InputMessage 的 MouseMove / LeftMouseButtonDown / LeftMouseButtonUp，
// 游戏自己命中、自己执行——不需要自由光标，也不需要我们知道任何面板的几何。消息 = {类别哈希, 动作哈希, 负载指针}，哈希 = 名字的 FNV-1a 64；
// 负载 +4 / +8 = 光标 x / y（原版视口坐标）。前端把触点按下 / 移动 / 抬起交给 dm_ui_touch，这里排队，在游戏取手柄处（主线程）每帧发一条：
// ---------- Native click: inject mouse messages into the UI root widget (panel touch route) ----------
// The Switch main program keeps the PC mouse message handling (dozens of code sites in the panel / generic widget classes that compare against hashes such as LeftMouseButtonDown):
// send InputMessage MouseMove / LeftMouseButtonDown / LeftMouseButtonUp to the UI root widget (the PanelManager pointed to by main+0x2f05458, message handler = vtable +0x28),
// and the game does its own hit testing and its own execution -- no free cursor is needed, and we do not need to know the geometry of any panel. Message = {category hash, action hash, payload pointer}, hash = FNV-1a 64 of the name;
// payload +4 / +8 = cursor x / y (original-game viewport coordinates). The front end hands touch point down / move / up to dm_ui_touch, they are queued here, and one is sent per frame where the game reads the controller (main thread):

uint64_t dm_guest_frame(void);
// 手指状态机（前端线程写、取手柄处读）：按着期间每帧把光标坐标顶住并发一条 Move——游戏每帧会用手柄光标逻辑把坐标改回去，
// 悬停（含职业选择页那种按光标拾取 3D 角色的）下一帧就丢（Hover Paladin 之后 30 ms 就 Unhover）。
// 抬起后：下一帧 Move + Down，再下一帧 Move + Up（按下前已有至少一帧悬停成立）。
// Finger state machine (written by the front-end thread, read where the controller is read): while held, pin the cursor coordinates every frame and send one Move -- every frame the game uses its controller cursor logic to change the coordinates back,
// so a hover (including the kind on the class selection page that picks a 3D character by cursor) is lost on the next frame (Unhover comes just 30 ms after Hover Paladin).
// After release: Move + Down on the next frame, then Move + Up on the frame after (at least one frame of hover has already held before the press).
static _Atomic int g_ut_phase;                      // 0 无 1 按着 2 待发按下 3 待发抬起 | 0 none, 1 held down, 2 press pending, 3 release pending
static _Atomic uint64_t g_ut_xy;                    // 归一化坐标各 16 位 | Normalized coordinates, 16 bits each
// 职业选择页（实玩后重做）。读码（人物控件 HDCharacterSelectorWidget，虚表 main+0x20c6fb8，消息处理 main+0xc2d840）：
//   · 控件状态字 +0x13f4：0 待机 / 1 出列中 / 2 已出列 / 3 归队中；设置状态并起动画的函数 main+0xc2cf24(控件, 状态)。
//   · 左键按下（main+0xc2dc94）：自己是 2 → 确认进模式页；自己不是 0 → 不收；自己是 0 但全局标志 main+0x2f143b0（有人在出列中）为真 → 不收。
//     别人发来 Select（main+0xc2da68）：自己是 2 才归队。所以出列动画那 3–4 秒里既不能改选也不能确认（手柄 B / A 同样被挡）。
//   · 点击矩形与人物站位对不上（术士的矩形与德鲁伊重合、人却站在法师的矩形里）——覆盖层里改成按实际站位分的八条竖条（touch_layout.py）。
//   做法：点击照常交给游戏（出列 / 归队都是原版动画）；只在按下前把「正在出列、且这次点的不是它」的那位用游戏自己的函数设成归队（3），
//   全局标志随之清掉，这次点击就能选中新的。确认不靠再点一次，用布局里的「继续」按钮（发 ToConsoleCharacterOptions，随时成立）。
// Class selection page (redone after actual play). Code reading (character widget HDCharacterSelectorWidget, vtable main+0x20c6fb8, message handler main+0xc2d840):
//   - Widget state word +0x13f4: 0 idle / 1 stepping forward / 2 stepped forward / 3 returning to the line; the function that sets the state and starts the animation is main+0xc2cf24(widget, state).
//   - Left button down (main+0xc2dc94): self is 2 -> confirm and enter the mode page; self is not 0 -> not accepted; self is 0 but the global flag main+0x2f143b0 (someone is stepping forward) is true -> not accepted.
//     Select sent by someone else (main+0xc2da68): returns to the line only if self is 2. So during the 3–4 seconds of the step-forward animation you can neither change the selection nor confirm (controller B / A are blocked the same way).
//   - The click rectangles do not match where the characters stand (the Warlock's rectangle coincides with the Druid's, while the character stands in the Sorceress's rectangle) -- in the overlay they are replaced by eight vertical strips split by actual standing position (touch_layout.py).
//   Method: the click is handed to the game as usual (stepping forward / returning are both original-game animations); only, before the press, the one that 'is stepping forward and is not the one tapped this time' is set to returning (3) with the game's own function,
//   the global flag is cleared along with it, and this click can then select the new one. Confirming does not rely on tapping again; it uses the 「继续」(Continue) button in the layout (sends ToConsoleCharacterOptions, valid at any time).
static _Atomic int g_cc_open;
static _Atomic int g_fe_name_open, g_fe_name_tap;   // 起名页开着；点了输入框（重新弹键盘） | Naming page open; the input box was tapped (bring the keyboard up again)
static _Atomic int g_fe_name_has;   // 起名页开着期间已经填过名字（输入框确认过非空的字，含留空取的随机名） | a name has been filled in while the naming page was open (the input field confirmed non-empty text, including the random name taken when left empty)
static const char *const k_cc_class[8] = { "Amazon", "Assassin", "Necromancer", "Barbarian", "Paladin", "Sorceress", "Warlock", "Druid" };
static _Atomic int g_fe_opt_open, g_fe_confirm_open, g_fe_back;
static _Atomic int g_fe_opt_side;   // 模式页最近一次点的是哪张牌：0 标准（左半屏）1 专家（右半屏） | Which card was tapped most recently on the mode page: 0 Standard (left half of the screen), 1 Hardcore (right half of the screen)
// 「极乐世界」勾选项（布局见 touch_layout.py 的 _BLISS：模式页牌子下面一行，原版勾选框 TouchBliss + 标题 + 说明）。
// 勾选框是原版开关按钮，点一下自己切换；点这一行的文字 = 点勾选框（点击挪到框心）。勾没勾读控件当前用的那组帧号：+0x6e8 = 当前常态帧，
// 等于布局里写的 toggledFrame（4）即已勾（真机 逐字节比对：勾 / 不勾只有 +0x610、+0x6e8–0x6f7 这几处变）。模式页开着时每帧读一次记下来，页关了保留最后的值。
// The 「极乐世界」(Elysium) checkbox (for the layout see _BLISS in touch_layout.py: one row below the plaques on the mode page, the original game's checkbox TouchBliss + title + description).
// The checkbox is an original-game toggle button that toggles itself on a tap; tapping the text of this row = tapping the checkbox (the click is moved to the box center). Whether it is checked is read from the frame number set the widget currently uses: +0x6e8 = current normal-state frame,
// equal to the toggledFrame written in the layout (4) means checked (byte-by-byte comparison on a real device: between checked / unchecked only +0x610 and +0x6e8–0x6f7 change). While the mode page is open it is read once per frame and recorded; after the page closes the last value is kept.
#define BLISS_Y0 1290
#define BLISS_Y1 1470
static _Atomic int g_fe_bliss;
int dm_fe_bliss(void) { return atomic_load(&g_fe_bliss); }
// 删除角色弹窗：原版要「长按『是』」，长按按手柄 A 的按住时长算（鼠标按住 4 秒只在抬起时出一次 Confirm、不删）。手指按在「是」上期间按住 A，松手放开
// Delete-character popup: the original game requires 'long-press 「是」(Yes)', and the long-press is measured by how long controller A is held (holding the mouse for 4 seconds only produces one Confirm on release and does not delete). Hold A while the finger is pressed on 「是」(Yes), and release it when the finger lifts
static _Atomic int g_fe_del_open, g_del_q, g_del_hold;   // 模式页 / 确认框开着；模式页上点了牌子以外 → 返回 | Mode page / confirmation box open; on the mode page a tap landed outside the plaques -> back
int dm_restart_busy(void);   // 一键重开进行中（定义在下面）：界面触点一律丢掉 | One-key restart in progress (defined below): all UI touch points are dropped
void dm_ui_touch(int phase, float nx, float ny) { if (phase != 3 && dm_restart_busy()) return;   // phase：0 按下 1 移动 2 抬起 3 作废（按下后改成了滚动：这次不点）；nx / ny = 出图画面归一化坐标 [0,1] | phase: 0 down, 1 move, 2 up, 3 cancelled (turned into a scroll after the press: no click this time); nx / ny = coordinates normalized to the rendered picture [0,1]
    if (phase == 3) { atomic_store(&g_ut_phase, 0); atomic_store(&g_del_q, 0); atomic_store(&g_del_hold, 0); return; }
    if (phase != 1) DM_LAT("ui_touch %d", phase);
    nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    atomic_store(&g_ut_xy, ((uint64_t)(uint32_t)(nx * 65535 + .5f) << 16) | (uint32_t)(ny * 65535 + .5f));
    // 「选择游戏模式」页只有标准 / 专家两块牌子（布局 y 899–1259 / 2160，覆盖层已挪走天梯行与键位提示）：点牌子那一行以外 = 返回职业选择
    // The 「选择游戏模式」(Select game modes) page has only the two plaques standard / hardcore (layout y 899–1259 / 2160; the overlay has already moved away the ladder row and the key hints): tapping outside the plaque row = back to class selection
    if (phase == 2 && atomic_load(&g_ut_phase) == 1 && atomic_load(&g_fe_opt_open) && !atomic_load(&g_fe_confirm_open) && (ny < 899 / 2160.0f || ny > 1259 / 2160.0f) && !(ny > BLISS_Y0 / 2160.0f && ny < BLISS_Y1 / 2160.0f)) { atomic_store(&g_ut_phase, 0); atomic_store(&g_fe_back, 1); return; }   // 牌子下面那一行是「极乐世界」勾选项，不算返回 | The row below the plaques is the 「极乐世界」(Elysium) checkbox and does not count as back
    if (phase == 2 && atomic_load(&g_fe_opt_open) && !atomic_load(&g_fe_confirm_open)) atomic_store(&g_fe_opt_side, nx >= 0.5f);
    if (phase == 2 && (atomic_exchange(&g_del_hold, 0) | atomic_exchange(&g_del_q, 0))) { atomic_store(&g_ut_phase, 0); return; }
    if (phase == 0 && atomic_load(&g_fe_del_open)) atomic_store(&g_del_q, 1);
    if (phase == 2) { if (atomic_load(&g_ut_phase) == 1) atomic_store(&g_ut_phase, 2); }
    else atomic_store(&g_ut_phase, 1);
}
int dm_ui_touch_active(void) { return atomic_load(&g_ut_phase) != 0; }
// 前端纵向拖能不能当滚动：删除角色确认框（长按「是」，手指一漂就作废长按）、模式页（牌子以外 = 返回）、起名页、确认框开着时不当，照旧是点按
// Whether a vertical drag in the front end can count as a scroll: not while the delete-character confirmation box (long-press 「是」(Yes); the long-press is voided as soon as the finger drifts), the mode page (outside the plaques = back), the naming page, or a confirmation box is open; those stay taps as before
int dm_ui_fe_drag_ok(void) { return !atomic_load(&g_fe_del_open) && !atomic_load(&g_fe_opt_open) && !atomic_load(&g_fe_name_open) && !atomic_load(&g_fe_confirm_open); }
static uint64_t ui_hash(const char *s) { uint64_t h = 0xcbf29ce484222325ull; for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull; return h; }
static void ui_send_mouse_arg(module_t *m, uint64_t root, uint64_t act, int x, int y, int arg) {   // 负载 +4 / +8 = 光标，+0x10 = 滚轮量（120 一格）/ 键码（VK） | Payload +4 / +8 = cursor, +0x10 = wheel amount (120 per notch) / key code (VK)
    static uint64_t payload[16], msg[8];
    memset(payload, 0, sizeof payload); payload[0] = (uint64_t)(uint32_t)x << 32; payload[1] = (uint32_t)y; payload[2] = (uint32_t)arg;
    memset(msg, 0, sizeof msg); msg[0] = ui_hash("InputMessage"); msg[1] = act; msg[2] = (uint64_t)payload;
    uint64_t vt = 0, fn = 0; memcpy(&vt, (void *)root, 8); memcpy(&fn, (void *)(vt + 0x28), 8);
    dm_cursor_set(x, y); (void)m;
    ((void (*)(uint64_t, void *))fn)(root, msg);
}
static void ui_send_mouse(module_t *m, uint64_t root, uint64_t act, int x, int y) { ui_send_mouse_arg(m, root, act, x, y, 0); }
// 给某个控件所在的位置发滚轮（游戏主线程；自定义页手柄选行时让原版滚动区自己滚到位，dm_custom.c）：notches > 0 = 内容往下（露出上面的行）。
// 位置取控件命中区的中心（屏幕上粗扫一遍它的命中函数）；控件不在屏上就不发
// Send a wheel event at the position of a given widget (game main thread; when a row is selected with the controller on the Custom page, this lets the original scroll area scroll into place by itself, dm_custom.c): notches > 0 = content moves down (revealing the rows above).
// The position is the center of the widget's hit area (a coarse scan of the screen with its hit-test function); nothing is sent if the widget is not on screen
void dm_ui_wheel_on(uint64_t w, int notches) {
    module_t *m = main_module(); uint64_t root = 0; if (!m || !w || !notches) return; memcpy(&root, m->base + 0x2f05458, 8); if (!root) return;
    int W = *(int32_t *)(m->base + 0x211da38), H = *(int32_t *)(m->base + 0x211da3c), x0 = W, x1 = -1, y0 = H, y1 = -1; if (W <= 0 || H <= 0) return;
    uint64_t fn = *(uint64_t *)(*(uint64_t *)w + 0x48);
    for (int y = 8; y < H; y += 16) for (int x = 8; x < W; x += 16) if (((int (*)(uint64_t, uint64_t))fn)(w, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32)) & 1) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x1 < 0) return; int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    ui_send_mouse(m, root, ui_hash("MouseMove"), cx, cy); ui_send_mouse_arg(m, root, ui_hash("MouseScroll"), cx, cy, notches * 120);
}
// 通用指针事件（手势层 / 探查用）：悬停保持期间每帧发 MouseMove 顶住光标；其余事件排队，每出图帧发一个（前面先发 Move）。
// 消息名都是主程序里有比较点的（哈希搜指令得到）：Left/RightMouseButtonDown/Up、MouseScroll、KeyDown / KeyUp。
// Generic pointer events (for the gesture layer / probing): while a hover is held, send MouseMove every frame to pin the cursor; the other events are queued, one sent per presented frame (preceded by a Move).
// The message names are all ones with a comparison site in the main program (found by searching instructions for the hash): Left/RightMouseButtonDown/Up, MouseScroll, KeyDown / KeyUp.
static _Atomic int g_tab_unhover;   // 点页签切页：下一次指针泵把光标挪到这次点的页签上（g_iq_xy）并收掉悬停保持，见 dm_ui_tab_go | Tapping a tab to switch pages: the next pointer pump moves the cursor onto the tab just tapped (g_iq_xy) and drops the held hover, see dm_ui_tab_go
static _Atomic int g_up_hold; static _Atomic uint64_t g_up_xy; static struct { int kind, arg; } g_up_q[32]; static _Atomic unsigned g_up_w, g_up_r;
void dm_ui_ptr(int kind, float nx, float ny, int arg) { if (dm_restart_busy()) return;
    nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    if (kind <= DM_UP_MOVE) { atomic_store(&g_up_xy, ((uint64_t)(uint32_t)(nx * 65535 + .5f) << 16) | (uint32_t)(ny * 65535 + .5f)); if (kind == DM_UP_HOVER) atomic_store(&g_up_hold, 1); return; }
    unsigned w = atomic_load(&g_up_w); if (w - atomic_load(&g_up_r) >= 32) return;
    g_up_q[w % 32].kind = kind; g_up_q[w % 32].arg = arg; atomic_store(&g_up_w, w + 1);
}
// 物品区命中：触点是否落在物品格 / 装备位 / 腰带 / 箱子格上（手势层据此分「单击 = 只移光标出说明」与「单击 = 点按钮」）。
// 做法：从界面根控件按名字走控件树（+8 名字、+0x50 可见、+0x58 / +0x60 子控件），对名字是 grid / horadriccube_grid / Belt / slot_* / advancedstash_* / basicstash_container 的可见控件
// 调它自己的命中函数（虚表 +0x48，参数 = 光标 x / y 打包，MouseScroll 处理处 main+0xad4d04 就是这么判「光标在不在我身上」）。
// 前端在触点按下时提问（任意线程），游戏主线程取手柄处作答：控件树只在主线程读。
// Item-area hit: whether the touch point lands on an item cell / equipment slot / belt / stash cell (the gesture layer uses this to tell 'single tap = only move the cursor and show the description' from 'single tap = click a button').
// Method: walk the widget tree by name from the UI root widget (+8 name, +0x50 visible, +0x58 / +0x60 children); for visible widgets named grid / horadriccube_grid / Belt / slot_* / advancedstash_* / basicstash_container
// call the widget's own hit function (vtable +0x48, argument = cursor x / y packed; the MouseScroll handler at main+0xad4d04 decides 'is the cursor on me' exactly this way).
// The front end asks when the touch point goes down (any thread), and the game main thread answers where it reads the controller: the widget tree is read only on the main thread.
static _Atomic uint64_t g_iq_xy; static _Atomic unsigned g_iq_seq, g_iq_done; static _Atomic int g_iq_res;
static _Atomic int g_iq_drop;
static _Atomic int g_drop_last = -1;   // 最近一次答上的拖放落点类 | Class of the most recently answered drag-and-drop landing point
// 「界面命中」行的限频：同一个答案 1 秒内至多打 4 条，答案变了立刻打。原先按进程累计封顶（判类 400 条、落点 60 条），
// 长局（如 2 小时的局）后半段就没有判类行了，读不出触点走了哪条路
// Rate limit for the 「界面命中」(UI hit) lines: at most 4 lines per second for the same answer, printed immediately when the answer changes. Previously capped by a per-process total (400 classification lines, 60 landing-point lines),
// so the second half of a long run (e.g. a 2-hour run) had no classification lines and you could not tell which path a touch point took
static int hit_log_ok(const char *hit) {
    static char last[96]; static uint64_t t0; static int n; uint64_t now = now_ns();
    if (strcmp(last, hit) || now - t0 > 1000000000ull) { strlcpy(last, hit, sizeof last); t0 = now; n = 0; }
    return n++ < 4;
}
static _Atomic int g_iq_out;   // 这次查询的触点在界面矩形之外（见 ui_classify） | The touch point of this query is outside the UI rectangle (see ui_classify)
#define UI_OUT(nx, ny) ((nx) < 0 || (nx) > 1 || (ny) < 0 || (ny) > 1)
unsigned dm_ui_item_query(float nx, float ny) {
    if (!atomic_load(&g_iq_drop)) { atomic_store(&g_drop_last, -1); atomic_store(&g_sn_new, 1); }   // 普通命中查询（按下）= 新手势：上一次拖放的落点、吸附 / 停靠都作废 | Ordinary hit query (press) = new gesture: the previous drag-and-drop landing point and the snap / parking are all invalidated
    DM_LAT("query");
    atomic_store(&g_iq_out, UI_OUT(nx, ny));
    nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    atomic_store(&g_iq_xy, ((uint64_t)(uint32_t)(nx * 65535 + .5f) << 16) | (uint32_t)(ny * 65535 + .5f));
    return atomic_fetch_add(&g_iq_seq, 1) + 1;
}
int dm_ui_item_result(unsigned seq) { return atomic_load(&g_iq_done) == seq ? atomic_load(&g_iq_res) : -1; }   // -1 = 还没答 | -1 = not answered yet
static int item_name(const char *n) { return !strcmp(n, "grid") || !strcmp(n, "horadriccube_grid") || !strcmp(n, "Belt") || !strncmp(n, "slot_", 5) || !strncmp(n, "advancedstash_", 14) || !strcmp(n, "basicstash_container"); }
static uint64_t g_iw_base;   // main 模块基址（item_walk 按虚表认高级仓库的成叠格用；ui_classify 进来时写） | Base address of the main module (used by item_walk to identify advanced stash stacked cells by vtable; written on entry to ui_classify)
static const char *g_modal_name = "";   // 最近一次判成「模态框外」时那个模态框的名字 | Name of the modal the last time a hit was classified as 'outside the modal'
static dm_wref g_slot; static const char *g_slot_panel;   // 最近一次物品区命中的控件、它所在的顶层面板名 | Widget of the most recent item-area hit, and the name of the top-level panel it belongs to
static int item_walk(uint64_t w, uint64_t xy, int dep, char *hit, size_t hn) {
    if (!w || dep > 14) return 0;
    uint64_t nm = *(uint64_t *)(w + 8), ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60);
    if (!*(uint8_t *)(w + 0x50)) return 0;
    if (dep == 1 && nm && !strncmp((const char *)nm, "HUD", 3)) return 0;   // HUD 各层（底栏那个 Belt 等）属于游戏画面：面板开着时不是物品区（设置页点「应用」被判成「物品区 Belt」，光标推摇杆改了别的设置） | The HUD layers (the Belt in the bottom bar, etc.) belong to the game picture: while a panel is open they are not an item area (tapping 「应用」(Apply) on the settings page was classified as 「物品区 Belt」(item area Belt), and the cursor pushing the stick changed another setting)
    // 高级仓库（宝石 / 材料 / 符文页）的成叠格：每格一个控件，名字是物品码（r01、gcw…），认虚表 main+0x20892f8（控件树转储）；它们的容器 advancedstash_* 自己没有矩形、命不中
    // Stacked cells of the advanced stash (gem / material / rune pages): one widget per cell, named by item code (r01, gcw...), identified by vtable main+0x20892f8 (widget tree dump); their containers advancedstash_* have no rectangle of their own and cannot be hit
    if (nm && (item_name((const char *)nm) || (g_iw_base && (*(uint64_t *)w - g_iw_base == 0x20892f8 || *(uint64_t *)w - g_iw_base == 0x20898e8)))) {   /* 0x20898e8 = 单件任务槽（ImbueSocketWidget）：槽里那件就是手上那件，从槽上起手拖到背包格 = 放回背包 | 0x20898e8 = single-item task slot (ImbueSocketWidget): the item in the slot is the item in hand; starting a drag on the slot and dragging to an inventory cell = put it back into the inventory */ uint64_t vt = *(uint64_t *)w, fn = *(uint64_t *)(vt + 0x48);
        if (((int (*)(uint64_t, uint64_t))fn)(w, xy) & 1) { strlcpy(hit, (const char *)nm, hn); dm_wref_set(&g_slot, w); return 1; } }
    if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (item_walk(*(uint64_t *)(ch + 8 * i), xy, dep + 1, hit, hn)) return 1;
    return 0;
}
static _Atomic uint64_t g_taps_base;
// 诊断：DM_UI_DUMP=<控件名> —— DM_UI_TAPS_AT 命中后第 45 帧把该名字的可见控件子树打一遍（名字、可见、虚表、子控件数、+0x40–0xc0 的非零整数），探控件结构用
// Diagnostic: DM_UI_DUMP=<widget name> -- on frame 45 after the DM_UI_TAPS_AT hit, dump the visible widget subtrees with that name (name, visible, vtable, child count, non-zero integers at +0x40–0xc0); used to probe widget structure
static void ui_dump(uint64_t w, int dep, const char *want, int on, module_t *m) {
    if (!w || dep > 14) return; uint64_t nm = *(uint64_t *)(w + 8), ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); const char *name = nm ? (const char *)nm : "";
    if (!strcmp(want, "@top")) { if (dep <= 2 && *(uint8_t *)(w + 0x50)) LOG("UIDUMP %*s%s n=%llu vt=main+%#llx", dep * 2, "", name, (unsigned long long)n, (unsigned long long)(*(uint64_t *)w - (uint64_t)m->base)); if (dep >= 2) return; }
    if (!on && !strcmp(name, want)) on = 1;
    if (on) { char b[700]; int k = 0; for (int o = 0x40; o < 0x1c0 && k < 640; o += 4) { int32_t v = *(int32_t *)(w + o); float f; memcpy(&f, &v, 4); if (v && o != 0x58 && o != 0x5c && o != 0x60) { if (v > -100000 && v < 100000) k += snprintf(b + k, sizeof b - k, " %x:%d", o, v); else if (f > -9000 && f < 9000 && (f > .01f || f < -.01f)) k += snprintf(b + k, sizeof b - k, " %x:%.1f", o, f); } }
        LOG("UIDUMP %*s%s vis=%d n=%llu vt=main+%#llx 消息处理=main+%#llx |%s", dep * 2, "", name, *(uint8_t *)(w + 0x50), (unsigned long long)n, (unsigned long long)(*(uint64_t *)w - (uint64_t)m->base), (unsigned long long)(*(uint64_t *)(*(uint64_t *)w + 0x28) - (uint64_t)m->base), b); }
    if (!ch || n > 256) return;
    for (uint64_t i = 0; i < n; i++) ui_dump(*(uint64_t *)(ch + 8 * i), dep + 1, want, on, m);
}
// 页签：鼠标左键点页签只出焦点框不切页（探查表 s4、布局两种改法都点不动），手柄 ZL / ZR（顶部五页签是 L / R）每按一次切一页。
// 页签条 = 虚表 main+0x20ba3a8 的控件（配置名 TabBarWidget），子控件 ImageK 是第 K 个页签（可见的按 +0x70 的 x 排序）。
// 当前选中第几个页签由页签条自己记在 +0xdc4（32 位整数，= ImageK 的 K；没有选中 / 没在用时是 −1）——游戏画高亮、响应翻页键读的就是它。
// 小站、箱子、顶部功能页同一个偏移（切页前后整段差分，随页号变的整数只有这一个）。+0x558 是配置的页签总数，不是可见数。
// 原先从外观反推当前页（先是「贴图帧号与众不同」、后是「文字最亮」）：小站页签条的图和字是分开的子控件，页签图上那三个数不是字色，
// 当前页恒认成同一个——只解锁两幕时 iPad 上从 II 切不回 I、iPhone 比例上从 I 切不到 II。两套反推都删了。
// 点到页签条上某个页签 → 算出与当前页差几页，宿主自己按够次数（dm_ui_tab_go）。
// Tabs: a left mouse click on a tab only shows the focus frame and does not switch pages (probe table s4; neither of two layout changes made it work), while controller ZL / ZR (L / R for the five top tabs) switch one page per press.
// Tab bar = the widget with vtable main+0x20ba3a8 (config name TabBarWidget); child widget ImageK is the K-th tab (visible ones sorted by x at +0x70).
// Which tab is currently selected is recorded by the tab bar itself at +0xdc4 (32-bit integer, = the K of ImageK; −1 when nothing is selected / not in use) - this is what the game reads to draw the highlight and respond to page-flip keys.
// Waypoint, stash and the top function page share the same offset (whole-block diff before and after a page switch: this is the only integer that follows the page number). +0x558 is the configured total number of tabs, not the visible count.
// The current page used to be inferred from appearance (first "the texture frame number that differs from the rest", later "the brightest text"): the waypoint tab bar's images and texts are separate child widgets, and those three numbers on the tab image are not the text color,
// so the current page was always recognized as the same one - with only two acts unlocked, on iPad II could not switch back to I, and at the iPhone aspect ratio I could not switch to II. Both inferences have been deleted.
// A tap on some tab of the tab bar → compute how many pages it differs from the current page, and the host presses enough times itself (dm_ui_tab_go).
#define TAB_SEL(w) (*(int32_t *)((w) + 0xdc4))
// 切页（从第一页点到最后一页，原先按「下一页」键连发 N 次，中间每页都画一遍）：直接调页签条自己的「选中第 K 个」
// main+0xad9b80(页签条, K, 出声)——跳过禁用的页签（+0xe88 + K）、写 +0xdc4、按需放切页音效（0x35f120）、经界面根发页签条的切页消息（+0xb30）
// 与第 K 个页签自己的消息（+0xb50 + K × 32）；面板收到切页消息直接换到那一页，中间页不出现（读码：写 +0xdc4 的三处之一）。
// 点到页签时 tab_walk 记下页签条、名字与 K；dm_ui_tab_go 改成直接切（执行前按指针核它还在界面树里、可见、是页签条类）。技能绑定流程里按差值走的那条（bd_tab_step）照旧按键
// Page switching (tapping from the first page to the last used to fire the "next page" key N times, drawing every page in between): call the tab bar's own "select the K-th" directly,
// main+0xad9b80(tab bar, K, play sound) - it skips disabled tabs (+0xe88 + K), writes +0xdc4, plays the page-switch sound as needed (0x35f120), and sends via the UI root the tab bar's page-switch message (+0xb30)
// and the K-th tab's own message (+0xb50 + K × 32); on receiving the page-switch message the panel goes straight to that page and the pages in between never appear (from reading the code: one of the three places that write +0xdc4).
// When a tab is tapped tab_walk records the tab bar, the name and K; dm_ui_tab_go now switches directly (before executing, it verifies by pointer that it is still in the UI tree, visible, and of the tab bar class). The path in the skill binding flow that steps by delta (bd_tab_step) still presses keys as before
static dm_wref g_tab_ref; static _Atomic int g_tab_have; static int g_tab_k; static char g_tab_name[48]; static _Atomic int g_tab_direct;   // 命中的页签条：g_tab_ref 只在游戏主线程读写（带界面代号）；g_tab_have 给前端线程 dm_ui_tab_go 原子读（先写 K 与名字、最后发布） | Tab strip that was hit: g_tab_ref is read/written only on the game main thread (with UI generation); g_tab_have is read atomically by the front-end thread in dm_ui_tab_go (K and the name are written first, published last)
static int ui_has(uint64_t w, uint64_t want, int dep) {   // 界面树里还挂着这个控件（按指针找；名字会重，如小站的页签条就叫「Tabs」） | This widget is still attached to the UI tree (searched by pointer; names can repeat, e.g. the waypoint tab bar is just called "Tabs")
    if (!w || dep > 14) return 0; if (w == want) return 1; uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (ui_has(*(uint64_t *)(ch + 8 * i), want, dep + 1)) return 1;
    return 0; }
static int tab_walk(uint64_t w, uint64_t xy, int dep, uint64_t vt_tab, int *delta, int *top) {
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    uint64_t nm = *(uint64_t *)(w + 8), ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60);
    if (!ch || n > 256) return 0;
    if (*(uint64_t *)w == vt_tab) {
        int hit = -1, act = -1, hx = 0, ax = 0, hk = -1, sel = TAB_SEL(w); struct { int x, k; } tabs[32]; int nt = 0;
        for (uint64_t i = 0; i < n; i++) { uint64_t c = *(uint64_t *)(ch + 8 * i); if (!c || !*(uint8_t *)(c + 0x50)) continue; const char *cn = (const char *)*(uint64_t *)(c + 8);
            if (!cn || strncmp(cn, "Image", 5) || cn[5] < '0' || cn[5] > '9' || nt >= 32) continue; int x = *(int32_t *)(c + 0x70), kk = atoi(cn + 5); tabs[nt].x = x; tabs[nt++].k = kk;
            if (kk == sel) { act = 1; ax = x; }
            uint64_t cvt = *(uint64_t *)c, fn = *(uint64_t *)(cvt + 0x48); if (((int (*)(uint64_t, uint64_t))fn)(c, xy) & 1) { hit = 1; hx = x; hk = kk; } }
        if (hit > 0) { static int lg; if (lg++ < 200) LOG("界面页签：页签条「%s」可见 %d 个，当前选中第 %d 个%s，点中第 %d 个", nm ? (const char *)nm : "?", nt, sel, act > 0 ? "" : "（不在可见页签里，不切）", hk); }
        if (hit > 0 && act > 0) { int d = 0; for (int i = 0; i < nt; i++) { if (tabs[i].x > ax && tabs[i].x <= hx) d++; if (tabs[i].x < ax && tabs[i].x >= hx) d--; }
            *delta = d; *top = nm && !strcmp((const char *)nm, "UISwitcherTabs"); g_tab_k = hk; strlcpy(g_tab_name, nm ? (const char *)nm : "", sizeof g_tab_name); dm_wref_set(&g_tab_ref, w); atomic_store(&g_tab_have, 1); return 1; }
        return 0;
    }
    for (uint64_t i = 0; i < n; i++) if (tab_walk(*(uint64_t *)(ch + 8 * i), xy, dep + 1, vt_tab, delta, top)) return 1;
    return 0;
}
// 面板内 / 面板外：界面鼠标点击只在触点落在某个打开的面板的控件上时才发。点击没有控件接住时会落到「点击接收器」（ClickCatcher，虚表 main+0x20c1328；
// 面板里的 ClickCatcher 与 HUD 的 BGClickCatcher），它把点击转给游戏世界 = 键鼠版的点地走路（小站页点面板外角色走开）。
// 除摇杆外任何触点不许触发移动。判法：在非 HUD 的顶层面板里找被点中的叶子控件，点击接收器不算；一个都没有 = 面板外。
// Inside / outside a panel: a UI mouse click is sent only when the touch point lands on a widget of some open panel. A click no widget catches falls to the "click catcher" (ClickCatcher, vtable main+0x20c1328;
// the ClickCatcher in panels and the HUD's BGClickCatcher), which forwards the click to the game world = the keyboard-mouse click-to-walk (tapping outside the panel on the waypoint page makes the character walk away).
// No touch point other than the stick may trigger movement. How it is judged: look for a hit leaf widget in the non-HUD top-level panels, click catchers excluded; none at all = outside the panel.
static const char *g_inside_name = "";
static _Atomic int g_cube_side_req, g_cube_side_close, g_cube_side_on;   // 魔盒并排：请求并排打开 / 只关魔盒（面板状态那段每帧落地） | cube side by side: request to open side by side / close only the cube (carried out every frame by the panel state section)
void dm_cube_side_mode_leave(void) { atomic_store(&g_cube_side_close, 1); }   // 登记表（dm_mode_items.h）：切模式前把并排着的魔盒关掉（手柄模式保持原版：开魔盒会关储藏箱） | registry (dm_mode_items.h): before switching modes, close the cube that is side by side (controller mode keeps the original behavior: opening the cube closes the stash)
// 技能绑定用：本次命中（吸附后）是不是落在技能树的技能图标上（控件名 SkillButton…，整条父链可见）。是 = 记下吸附后的归一化坐标，前端据此发「光标到点 + 按 Y」。
// For skill binding: whether this hit (after snapping) lands on a skill icon in the skill tree (widget name SkillButton..., whole parent chain visible). Yes = record the snapped normalized coordinates, which the front end uses to send "cursor to point + press Y".
static uint64_t ui_find(uint64_t w, const char *name, int dep);
static _Atomic uint64_t g_bd_hit;   // 最高位 = 有效；[31:16] x、[15:0] y（0..65535 归一化） | Top bit = valid; [31:16] x, [15:0] y (normalized 0..65535)
static dm_wref g_bd_hit_ref;
static int bd_skill_walk(uint64_t w, uint64_t xy, int dep) {
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    uint64_t nm = *(uint64_t *)(w + 8), ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60);
    if (nm && !strncmp((const char *)nm, "SkillButton", 11) && strncmp((const char *)nm, "SkillButtonC", 12)) { uint64_t fn = *(uint64_t *)(*(uint64_t *)w + 0x48); int h = ((int (*)(uint64_t, uint64_t))fn)(w, xy) & 1; if (h) dm_wref_set(&g_bd_hit_ref, w); return h; }
    if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (bd_skill_walk(*(uint64_t *)(ch + 8 * i), xy, dep + 1)) return 1;
    return 0;
}
// 属性页的四个「+」（CharacterStatsPanel > LevelUpInfo > Strength / Dex / Vitality / EnergyPointButton）：点一下游戏当场加 1 点、不可撤销。
// 「先选中、再确认」：技能图标与这四个「+」的点按只移光标（选中、出说明），加点由手机层的确认按钮触发。第 62 位 = 命中的是属性「+」。
// The four "+" on the attributes page (CharacterStatsPanel > LevelUpInfo > Strength / Dex / Vitality / EnergyPointButton): one tap and the game adds 1 point on the spot, irreversibly.
// "Select first, then confirm": taps on skill icons and these four "+" only move the cursor (select, show the tooltip), and spending the point is triggered by the phone layer's confirm button. Bit 62 = the hit is an attribute "+".
static _Atomic uint32_t g_bd_hit_id;   // 最近一次命中的加点类按钮是哪一个（g_bd_hit_ref = 它的控件，主线程写） | Which point-allocation button was hit most recently (g_bd_hit_ref = its widget, written by the main thread)
uint32_t dm_ui_hit_id(void) { return atomic_load(&g_bd_hit_id); }
static int bd_stat_walk(uint64_t w, uint64_t xy, int dep) {
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    uint64_t nm = *(uint64_t *)(w + 8), ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); size_t L = nm ? strlen((const char *)nm) : 0;
    if (L > 11 && !strcmp((const char *)nm + L - 11, "PointButton")) { uint64_t fn = *(uint64_t *)(*(uint64_t *)w + 0x48); int h = ((int (*)(uint64_t, uint64_t))fn)(w, xy) & 1; if (h) dm_wref_set(&g_bd_hit_ref, w); return h; }
    if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (bd_stat_walk(*(uint64_t *)(ch + 8 * i), xy, dep + 1)) return 1;
    return 0;
}
static void bd_note_skill(module_t *m, uint64_t root, int x, int y) {
    int32_t w = *(int32_t *)(m->base + 0x211da38), h = *(int32_t *)(m->base + 0x211da3c); if (w <= 0 || h <= 0) return;
    // 上面盖着模态框（绑定框、确认框…）时不看底下的技能树：绑定框的格子压在技能图标上，点格子会被认成「点了底下那个技能」，
    // 前端只移光标、格子收不到点击（逐格读出第 2 排第 3 列不生效；格子算作可点控件后才走得到这里）
    // When a modal (binding box, confirmation box...) is on top, do not look at the skill tree underneath: the binding box's cells sit over skill icons, and tapping a cell would be read as "tapped that skill underneath",
    // so the front end only moved the cursor and the cell got no click (reading cell by cell showed row 2 column 3 not taking effect; this is only reached once cells count as clickable widgets)
    if (modal_any(root, 1, NULL)) { atomic_store(&g_bd_hit_id, 0); return; }
    uint64_t xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32), tree = ui_find(root, "SkillsTreePanel", 0), cs = ui_find(root, "CharacterStatsPanel", 0);
    dm_wref_set(&g_bd_hit_ref, 0); uint64_t kind = tree && bd_skill_walk(tree, xy, 0) ? (1ull << 63) : cs && bd_stat_walk(cs, xy, 0) ? (1ull << 63) | (1ull << 62) : 0; if (!kind) { atomic_store(&g_bd_hit_id, 0); return; }
    { const char *hn = dm_wref_get(&g_bd_hit_ref) ? (const char *)*(uint64_t *)(dm_wref_get(&g_bd_hit_ref) + 8) : "";   // 属性「+」：1..4 = 力量 / 敏捷 / 体力 / 精力（按名字首字母）；技能图标：控件地址低 31 位 | 最高位 | Attribute "+": 1..4 = Strength / Dexterity / Vitality / Energy (by first letter of the name); skill icon: low 31 bits of the widget address | top bit
      uint32_t id = (kind >> 62) & 1 ? (hn[0] == 'S' ? 1 : hn[0] == 'D' ? 2 : hn[0] == 'V' ? 3 : hn[0] == 'E' ? 4 : 0) : (uint32_t)dm_wref_get(&g_bd_hit_ref) | 0x80000000u; atomic_store(&g_bd_hit_id, id); }
    atomic_store(&g_bd_hit, kind | ((uint64_t)(x * 65535ll / w) << 16) | (uint64_t)(y * 65535ll / h));
}
int dm_ui_bind_skill_hit(float *nx, float *ny) { uint64_t v = atomic_load(&g_bd_hit); if (!(v >> 63)) return 0;   /* 返回 1 = 技能图标，2 = 属性「+」 | Returns 1 = skill icon, 2 = attribute "+" */ *nx = ((v >> 16) & 0xffff) / 65535.0f; *ny = (v & 0xffff) / 65535.0f; return (v >> 62) & 1 ? 2 : 1; }
static int inside_walk(uint64_t w, uint64_t xy, int dep, uint64_t vt_catch) {
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60), vt = *(uint64_t *)w;
    if (vt == vt_catch) return 0;
    { const char *nm = (const char *)*(uint64_t *)(w + 8);   // 不画东西、只占位的辅助控件不算（名字来自实测：整屏命中、与面板无关） | Helper widgets that draw nothing and only occupy space do not count (names from measurement: full-screen hit, unrelated to panels)
      static const char *skip[] = { "PanelBordersPanel", "ControllerCursorBounds", "ShowItemsPanel", "MiniMenuPanel", "TooltipsPanel", "EmoteWheelPanel", "TransparentBG", "SpriteColoringHelper", "LanguageFontRemapper", "ShowItemsParameters", "ControllerCursorParameters", "MoviePlayer", "AutoMap", "TooltipTextBox" };   // TooltipTextBox：说明框伸到面板外时不算面板（点它 = 点面板外 = 关；mini 技能树页 2/5 被它挡住）   // AutoMap：小地图面板整屏占位（进局后常开） | TooltipTextBox: when the tooltip box extends outside the panel it does not count as a panel (tapping it = tapping outside the panel = close; on the second Mac it blocked 2/5 on the skill tree page)   // AutoMap: the minimap panel is a full-screen placeholder (always open after entering a run)
      if (nm) for (unsigned i = 0; i < sizeof skip / sizeof *skip; i++) if (!strcmp(nm, skip[i])) return 0; }
    { uint64_t rel = vt - (vt_catch - 0x20c1328);   // 叶子控件，或带子控件的底板类（九宫格底板 0x20b9580、对话框边框 0x20bc048：设置页的 Background 就是带子控件的底板； | Leaf widgets, or backing classes with children (nine-slice backing 0x20b9580, dialog frame 0x20bc048: the Background of the settings page is a backing with children;
      // 带子控件的图片 0x20b8650：编年史「查看奖励」页的整页底板 RewardsBg 就是它，原先不算，卡片之间的空处判成「奖励页没盖住」，
      // 点下去穿到底下的编年史列表 / 分类 / 页签上，列表容器拿到焦点、金框画在奖励页上面（iPhone 15 Pro 截图）。按它自己的矩形算，同叶子图片；
      // 先查它的子控件、都没中才算它自己——名字要报到最里层（关闭钮按 TouchClose / close 的名字认）
      // Image widget with children 0x20b8650: the full-page backing RewardsBg of the Chronicle "View Rewards" page is one. It used to be ignored, so empty space between cards was judged "not covered by the rewards page",
      // and a tap fell through to the Chronicle list / categories / tabs underneath; the list container got focus and its gold frame was drawn over the rewards page (iPhone 15 Pro screenshot). It now counts with its own rect, like a leaf image;
      // its children are checked first and only if none is hit does it count itself, so the name is reported down to the innermost widget (the close button is recognized by the names TouchClose / close)
      if (rel == 0x20b8650 && n && ch && n <= 256) { for (uint64_t i = 0; i < n; i++) if (inside_walk(*(uint64_t *)(ch + 8 * i), xy, dep + 1, vt_catch)) return 1;
          uint64_t fn = *(uint64_t *)(vt + 0x48); if (((int (*)(uint64_t, uint64_t))fn)(w, xy) & 1) { g_inside_name = *(uint64_t *)(w + 8) ? (const char *)*(uint64_t *)(w + 8) : "?"; return 1; } return 0; }
      if (!n || !ch || rel == 0x20b9580 || rel == 0x20bc048) { uint64_t fn = *(uint64_t *)(vt + 0x48); int h = ((int (*)(uint64_t, uint64_t))fn)(w, xy) & 1; if (h) { g_inside_name = *(uint64_t *)(w + 8) ? (const char *)*(uint64_t *)(w + 8) : "?"; return 1; } if (!n || !ch) return 0; } }
    if (n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (inside_walk(*(uint64_t *)(ch + 8 * i), xy, dep + 1, vt_catch)) return 1;
    return 0;
}
static const char *g_inside_owner = "";   // 最近一次「面板内」命中的叶子属于哪个顶层面板（空 = 这次不是走「面板内空白」判出来的） | Which top-level panel the leaf of the most recent "inside panel" hit belongs to (empty = this time was not decided via "blank inside panel")
static int free_walk(uint64_t w, int dep, uint64_t base);
// 这一点在不在某个「带物品区的面板」自己的矩形里（面板根控件有边界：点在里面命中、屏幕角不命中）。
// 两侧黑网格、外框边线都不在里面（mini 探查）。
// Whether this point is inside the own rect of some "panel with an item area" (the panel root widget has bounds: hit inside, not hit at the screen corner).
// The black grid on both sides and the outer frame lines are not inside (probed on the second Mac).
static int in_item_panel_rect(module_t *m, uint64_t root, uint64_t xy) {
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60); if (!ch || n > 256) return 0;
    // 面板的命中矩形比画出来的石板窄（物品栏矩形 1408 单位宽，石板约 1770；石板和黑网格是同一张底图，没有控件给石板边界）：
    // 左右各外扩 180 单位再判，石板外圈算「放回」不算「丢」——误丢比放回更伤
    // The panel's hit rect is narrower than the drawn slab (the inventory panel rect is 1408 units wide, the slab about 1770; the slab and the black grid are one background image, and no widget gives the slab bounds):
    // extend 180 units on each side before judging, so the slab's outer ring counts as "put back" rather than "drop" - a mistaken drop hurts more than a put-back
    int32_t vh = *(int32_t *)(m->base + 0x211da3c); int x = (int)(uint32_t)xy; uint64_t yy = xy >> 32 << 32;
    for (uint64_t i = 0; i < n; i++) { uint64_t p = *(uint64_t *)(ch + 8 * i); if (!p || !*(uint8_t *)(p + 0x50)) continue;
        uint64_t fn = *(uint64_t *)(*(uint64_t *)p + 0x48); int dx = (int)(180 * *(float *)(p + 0x80) * vh / 2160.0), xl = x > dx ? x - dx : 0;
        if (((((int (*)(uint64_t, uint64_t))fn)(p, xy) | ((int (*)(uint64_t, uint64_t))fn)(p, yy | (uint32_t)xl) | ((int (*)(uint64_t, uint64_t))fn)(p, yy | (uint32_t)(x + dx))) & 1)
            && !(((int (*)(uint64_t, uint64_t))fn)(p, (uint64_t)3 | ((uint64_t)3 << 32)) & 1) && free_walk(p, 1, (uint64_t)m->base)) return 1; }
    return 0;
}
static int ui_inside_panel(module_t *m, uint64_t root, uint64_t xy) {   // 从上层往下找：记下最上面那个接住这一点的面板 | Search from the top layer down: record the topmost panel that catches this point
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60); if (!ch || n > 256) return 0;
    for (int i = (int)n - 1; i >= 0; i--) { uint64_t p = *(uint64_t *)(ch + 8 * i); if (!p || !*(uint8_t *)(p + 0x50)) continue; const char *nm = (const char *)*(uint64_t *)(p + 8);
        if (nm && !strncmp(nm, "HUD", 3)) continue;   // HUD 各层属于游戏画面，不算面板 | HUD layers belong to the game screen and do not count as panels
        if (nm && !strcmp(nm, "NotificationPanel")) continue;   // 系统提示框不接点击（同 panel_skip） | system notifications do not take taps (same as panel_skip)
        if (nm && !strcmp(nm, "FocusManager")) continue;   // 焦点金框只是画在被选中控件上的框，不是可点的东西（同 panel_skip）：原先点到金框画着的地方命中「FocusIndicator@FocusManager」，那一下落空 | The focus gold frame is only a frame drawn on the selected widget, not something you can tap (same as panel_skip): before, tapping where the gold frame was drawn hit "FocusIndicator@FocusManager" and the tap went nowhere
        if (inside_walk(p, xy, 1, (uint64_t)m->base + 0x20c1328)) { g_inside_owner = nm ? nm : "?"; return 1; } }
    return 0;
}
// 就近吸附（手指热区；审计结果：可点控件普遍 4–5 毫米高，246 个里只有 12 个够 7 毫米）：
// 触点不在可点控件上时，在它周围 3.5 毫米（90 布局单位）内由近到远找可点控件（九圈 × 八个方向取样，用控件自己的命中函数），找到就把点击挪到那个取样点上。
// 可点控件按类型（虚表）认：按钮、开关按钮、属性「+」、任务图标；页签单独处理（上下各放宽 90 单位）。
// Nearest-snap (finger hot zone; audit result: clickable widgets are generally 4–5 mm high, only 12 of 246 reach 7 mm):
// when the touch point is not on a clickable widget, look for clickable widgets within 3.5 mm (90 layout units) around it from near to far (sampling nine rings × eight directions with the widgets' own hit-test functions), and on a find move the click to that sample point.
// Clickable widgets are recognized by type (vtable): buttons, toggle buttons, attribute "+", quest icons; tabs are handled separately (widened 90 units above and below).
static dm_wref g_click;   // 最近一次命中的可点控件 | The most recently hit clickable widget
static const char *g_click_name = ""; static int g_click_exact;   // g_click_exact：这次是判触点本身（不是吸附取样）——滑块只认直接点中，不吸附（点到滑槽值会直接跳过去） | g_click_exact: this time the touch point itself is being judged (not a snap sample) - sliders are direct hits only, no snapping (a tap on the track makes the value jump straight there)
static int click_walk(uint64_t w, uint64_t xy, int dep, uint64_t base) {
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    uint64_t vt = *(uint64_t *)w - base, ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60);
    // 0x20baff8 = 下拉（DropdownListWidget）：原先不算可点，编年史页的部位下拉紧挨页签条，页签条上下放宽的 90 单位把点它的那一下抢成切页签（真机上点不动，宿主复现点成切到第 3 页签）
    // 0x20b91c0 / 0x20b8718 = 数字框 / 文字框（存取金币的数量框、起名框）：画出来的框（数量框的 BackgroundBox，布局 x 244–986）比输入控件本身（323–953）宽一圈，
    // 原先不算可点控件、也就不吸附：点在框的左头（数字显示的位置）落到弹窗底板上，输入框没开，点「好」= 按缺省最大值全存 / 全取。
    // 算可点之后现成的 3.5 毫米吸附（90 单位）盖住框四边的余量（左 79、上 19、下 24）
    // 0x20baff8 = dropdown (DropdownListWidget): it used not to count as clickable; the slot dropdown on the chronicle page sits right next to the tab bar, and the tab bar's 90-unit vertical widening turned taps on it into tab switches (cannot be tapped on a real device; the host reproduced it as switching to tab 3)
    // 0x20b91c0 / 0x20b8718 = number box / text box (the gold deposit/withdraw quantity box, the naming box): the drawn box (the quantity box's BackgroundBox, layout x 244–986) is one ring wider than the input widget itself (323–953);
    // it used not to count as a clickable widget and so was not snapped to: a tap on the left end of the box (where the number is shown) landed on the popup backing, the input box did not open, and tapping 「好」(OK) = deposit / withdraw everything at the default maximum.
    // Once counted as clickable, the existing 3.5 mm snap (90 units) covers the margins on all four sides of the box (left 79, top 19, bottom 24)
    if (vt == 0x20b7b00 || vt == 0x208a5f8 || vt == 0x20bc110 || vt == 0x20c7d68 || vt == 0x20c80f0 || vt == 0x20c6fb8 || vt == 0x20b84d0 || vt == 0x20b8cd0 || vt == 0x20b97f8 || vt == 0x20b91c0 || vt == 0x20b8718 || vt == 0x20baff8 || (vt == 0x20bad70 && g_click_exact)) {   /* 按钮 / 技能树「综合技能」页的技能图标（普通技能、物品技能，0x208a5f8）/ 开关按钮 / 属性「+」/ 任务图标 / 职业人物 / 列表行（0x20b8cd0 = 键鼠版选角页的角色行）/ 滚动条箭头 / 滑块（含两端箭头，只认直接点中） | Buttons / skill icons on the skill tree's 「综合技能」(General Skills) page (normal skills, item skills, 0x208a5f8) / toggle buttons / attribute "+" / quest icons / class characters / list rows (0x20b8cd0 = character rows on the keyboard-mouse character select page) / scrollbar arrows / sliders (including the arrows at both ends, direct hits only) */ uint64_t fn = *(uint64_t *)(*(uint64_t *)w + 0x48); if (((int (*)(uint64_t, uint64_t))fn)(w, xy) & 1) { dm_wref_set(&g_click, w); g_click_name = *(uint64_t *)(w + 8) ? (const char *)*(uint64_t *)(w + 8) : "?"; return 1; } }
    if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (click_walk(*(uint64_t *)(ch + 8 * i), xy, dep + 1, base)) return 1;
    return 0;
}
// 面板有上下层（根控件的子控件数组里越靠后越在上面：暂停菜单 → 设置页框 → 设置页内容，按打开顺序）。上层面板盖住的地方，下层的按钮虽然还「可见」，
// 游戏照样会把没人接的点击交给它——Mac 的 iOS 包：设置页里点偏滑块箭头十来个像素，点击落到了被盖住的暂停菜单「消息记录」上，设置页被关。
// 所以：先找盖住这一点的最上层面板 T；「在可点控件上 / 就近吸附」只认 T 及其以上的控件（被盖住的下层按钮不吸附过去）。
// 上层面板里我们认不出类型的控件（滑块两端的箭头由滑块自己处理、没有单独的命中区）照常把点击交给游戏，不拦；
// 设置页盖着暂停菜单这一对另行处理：设置页开着时把暂停菜单整层设为不可见（dm_ui_mouse_pump），它的按钮就收不到点击。
// Panels are layered (later in the root widget's child array is higher: pause menu → settings page frame → settings page content, in opening order). Where an upper panel covers, the lower panel's buttons are still "visible" and
// the game will still hand unclaimed clicks to them - Mac iOS app: on the settings page, missing a slider arrow by ten-odd pixels put the click on the covered pause menu's 「消息记录」(Message Log), and the settings page closed.
// So: first find the topmost panel T covering this point; "on a clickable widget / nearest-snap" only recognizes widgets of T and above (covered lower buttons are not snapped to).
// Widgets in upper panels whose type we cannot identify (the arrows at both ends of a slider are handled by the slider itself and have no separate hit area) still pass the click to the game as usual, not blocked;
// the settings-page-over-pause-menu pair is handled separately: while the settings page is open the whole pause menu layer is set invisible (dm_ui_mouse_pump), so its buttons cannot receive clicks.
static int panel_skip(const char *nm) { static const char *sk[] = { "AutoMap", "PanelBordersPanel", "ShowItemsPanel", "MiniMenuPanel", "TooltipsPanel", "EmoteWheelPanel", "FocusManager", "InGameMoviePlayerPanel",
        "NotificationPanel" };   // 系统提示框只是告知，不挡下面的面板：它在屏上的几秒里压着的格子 / 按钮照常点 | a system notification only informs and does not block the panels beneath it: during the few seconds it is on screen, the cells / buttons it covers can be tapped as usual
    if (!nm) return 0; if (!strncmp(nm, "HUD", 3)) return 2; for (unsigned i = 0; i < sizeof sk / sizeof *sk; i++) if (!strcmp(nm, sk[i])) return 1; return 0; }
// 有边界矩形的面板（根控件在屏幕角不命中）：点不在它的矩形里，它里面的东西都不算——滚动列表滚出去的行仍「可见」、命中函数仍为真
// （设置页内容面板：「应用」按钮的位置上压着滚到下面去的「高级音量」文字）。返回 1 = 这一点落在面板范围外，跳过该面板。
// A panel with a bounding rect (root widget not hit at the screen corner): if the point is not inside its rect, nothing in it counts - rows scrolled out of a scrolling list are still "visible" and their hit-test function is still true
// (settings page content panel: the 「高级音量」(Advanced Volume) text scrolled below sits at the position of the 「应用」(Apply) button). Returns 1 = this point is outside the panel's extent, skip the panel.
static int panel_clipped(uint64_t p, uint64_t xy) { uint64_t fn = *(uint64_t *)(*(uint64_t *)p + 0x48);
    return !(((int (*)(uint64_t, uint64_t))fn)(p, (uint64_t)3 | ((uint64_t)3 << 32)) & 1) && !(((int (*)(uint64_t, uint64_t))fn)(p, xy) & 1) && *(int32_t *)(p + 0x78) > 0 && *(int32_t *)(p + 0x7c) > 0; }
// 盖住这一点的最上层面板在根的子数组里的下标（-1 = 没有）：HUD 各层与整屏占位的不算。越靠后越在上层。
// 判类（物品区 / 页签 / 可点控件）都只看它和它上面的面板——模态弹窗开着时，它底下的一切都不该命中
// （金币框的按钮被判成底下箱子的格子，11 次点击没反应）。
// 面板里有没有任何「看得见的叶子控件」（整条父链两个可见位都开着）。只剩一个矩形、里面什么都不画的面板不算盖层：
// NPC 对话面板在交易页底下一直开着，「你没有足够的金币」的提示框关掉后它还留着提示框那块矩形（探查：根命中 1 / 角命中 0 / 叶命中 0），
// 被当成盖在货架上——再按那件货不算物品区、变成鼠标点击，游戏弹出自己的购买确认框
// Index in the root's child array of the topmost panel covering this point (-1 = none): HUD layers and full-screen placeholders excluded. Later entries are on upper layers.
// Classification (item area / tab / clickable widget) only looks at it and the panels above it - while a modal popup is open, nothing underneath it should be hit
// (the gold box's buttons were judged as cells of the stash underneath, and 11 taps did nothing).
// Whether the panel contains any "visible leaf widget" (both visible flags on along the whole parent chain). A panel left with only a rect and drawing nothing inside does not count as a covering layer:
// the NPC dialog panel stays open underneath the trade page, and after the 「你没有足够的金币」(You do not have enough gold) prompt box closes it still keeps that prompt box's rect (probe: root hit 1 / corner hit 0 / leaf hit 0),
// so it was treated as covering the shelf - pressing that item again was no longer an item area but became a mouse click, and the game popped up its own purchase confirmation box
static int has_visible_leaf(uint64_t w, int dep) {
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50) || !*(uint8_t *)(w + 0x51)) return 0;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || !n) return dep > 1;
    if (n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (has_visible_leaf(*(uint64_t *)(ch + 8 * i), dep + 1)) return 1;
    return 0;
}
static int ui_top_cover(module_t *m, uint64_t root, int x, int y) {
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60), base = (uint64_t)m->base; if (!ch || n > 256) return -1;
#define SNAP_XY(px, py) ((uint64_t)(uint32_t)(px) | ((uint64_t)(uint32_t)(py) << 32))
    int top = -1;   // 盖住这一点的最上层面板（HUD 各层与整屏占位的不算） | The topmost panel covering this point (HUD layers and full-screen placeholders excluded)
    for (int i = (int)n - 1; i >= 0 && top < 0; i--) { uint64_t p = *(uint64_t *)(ch + 8 * i); if (!p || !*(uint8_t *)(p + 0x50) || panel_skip((const char *)*(uint64_t *)(p + 8))) continue;
        uint64_t pfn = *(uint64_t *)(*(uint64_t *)p + 0x48);   // 盖住 = 面板自己有边界矩形且点在里面（根控件命中、屏幕角不命中：设置页内容面板），或有叶子控件被点中 | Covering = the panel has its own bounding rect and the point is inside (root widget hit, screen corner not hit: the settings page content panel), or a leaf widget is hit
        if (panel_clipped(p, SNAP_XY(x, y))) continue;
        if ((((int (*)(uint64_t, uint64_t))pfn)(p, SNAP_XY(x, y)) & 1 && !(((int (*)(uint64_t, uint64_t))pfn)(p, SNAP_XY(3, 3)) & 1) && has_visible_leaf(p, 1)) || inside_walk(p, SNAP_XY(x, y), 1, base + 0x20c1328)) top = i; }
    return top;
}
// 滑块两端的箭头（每点走 1）：贴图只有 2.8 毫米、没有单独的命中区（由滑块自己按点击位置处理），就近吸附认不到它。
// 触点落在某个滑块的滑槽左 / 右外侧 7 毫米内（上下各 3.5 毫米）= 点那一端的箭头：把点击挪到箭头中心（滑槽端头外 44 单位，实测音效滑块滑槽 917–1148、箭头 898 / 1165）。
// The arrows at both ends of a slider (1 per tap): the texture is only 2.8 mm with no separate hit area (the slider itself handles it by click position), so nearest-snap cannot find it.
// A touch point within 7 mm outside the left / right of some slider's track (3.5 mm above and below) = a tap on that end's arrow: move the click to the arrow center (44 units beyond the track end; measured for the sound-effects slider: track 917–1148, arrows 898 / 1165).
static int slider_hit(uint64_t w, int x, int y) { uint64_t fn = *(uint64_t *)(*(uint64_t *)w + 0x48); return ((int (*)(uint64_t, uint64_t))fn)(w, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32)) & 1; }
static void slider_collect(uint64_t w, uint64_t vt, int dep, uint64_t *out, int *n) {
    if (!w || dep > 16 || *n >= 48 || !*(uint8_t *)(w + 0x50)) return;
    if (*(uint64_t *)w == vt) { out[(*n)++] = w; return; }
    uint64_t ch = *(uint64_t *)(w + 0x58), k = *(uint64_t *)(w + 0x60); if (!ch || k > 256) return;
    for (uint64_t i = 0; i < k; i++) slider_collect(*(uint64_t *)(ch + 8 * i), vt, dep + 1, out, n);
}
static int slider_arrow(module_t *m, uint64_t root, int *x, int *y, int W, int H) {
    static uint64_t sl[48]; int n = 0; slider_collect(root, (uint64_t)m->base + 0x20bad70, 0, sl, &n); if (!n) return 0;
    float u = H / 2160.0f; int vt = (int)(106 * u), reach = (int)(212 * u), st = (int)(12 * u) > 1 ? (int)(12 * u) : 1, off = (int)(44 * u);
    for (int d = 0; d <= vt; d += st) for (int sg = 0; sg < (d ? 2 : 1); sg++) { int yy = *y + (sg ? d : -d); if (yy < 0 || yy >= H) continue;   // 由近到远找行 | Find the row from near to far
        for (int e = 0; e <= reach + st; e += st) for (int dir = -1; dir <= 1; dir += 2) { int xx = *x + dir * e; if (xx < 0 || xx >= W || (!e && dir > 0)) continue;
            for (int i = 0; i < n; i++) if (slider_hit(sl[i], xx, yy)) { uint64_t w = sl[i]; int a, b, l, r, top, bot;
                for (a = 0, b = xx; a < b;) { int mid = (a + b) / 2; if (slider_hit(w, mid, yy)) b = mid; else a = mid + 1; } l = a;
                for (a = xx, b = W - 1; a < b;) { int mid = (a + b + 1) / 2; if (slider_hit(w, mid, yy)) a = mid; else b = mid - 1; } r = a;
                for (a = 0, b = yy; a < b;) { int mid = (a + b) / 2; if (slider_hit(w, xx, mid)) b = mid; else a = mid + 1; } top = a;
                for (a = yy, b = H - 1; a < b;) { int mid = (a + b + 1) / 2; if (slider_hit(w, xx, mid)) a = mid; else b = mid - 1; } bot = a;
                int in = (int)(64 * u), side = dir;   // 触点压在滑槽上：只有离端头 2.5 毫米内才算箭头（右端外离面板边只有 5 毫米，向内补足 7 毫米）；再往里是滑槽自己的「跳到这里」 | Touch point on the slider track: only within 2.5 mm of the end counts as the arrow (outside the right end there is only 5 mm to the panel edge, so the inside makes it up to 7 mm); further in is the track's own "jump here"
                if (*x >= l && *x <= r) { if (*x - l <= in && *x - l <= r - *x) side = 1; else if (r - *x <= in) side = -1; else return 0; }
                int ax = side > 0 ? l - off : r + off, ay = (top + bot) / 2;   // side>0：触点在左端 | side>0: the touch point is at the left end
                LOG("界面吸附：(%d,%d) 在滑块 %s 的%s端（滑槽 x %d–%d，行 y=%d）→ 点箭头 (%d,%d)", *x, *y, *(uint64_t *)(w + 8) ? (const char *)*(uint64_t *)(w + 8) : "?", side > 0 ? "左" : "右", l, r, ay, ax, ay);
                *x = ax; *y = ay; return 1; } } }
    return 0;
}
static void slot_center(uint64_t w, int *x, int *y, int vw, int vh, int cells);
static int ui_snap(module_t *m, uint64_t root, int *x, int *y) {   // 0 周围没有可点控件 / 1 本来就在可点控件上 / 2 已挪到最近的可点控件上 / 6 压着被上层面板盖住的可点控件：不点 | 0 no clickable widget nearby / 1 already on a clickable widget / 2 moved to the nearest clickable widget / 6 on a clickable widget covered by an upper panel: do not click
    int32_t w = *(int32_t *)(m->base + 0x211da38), h = *(int32_t *)(m->base + 0x211da3c); if (w <= 0 || h <= 0) return 0;
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60), base = (uint64_t)m->base; if (!ch || n > 256) return 0;
    int top = ui_top_cover(m, root, *x, *y);
    if (getenv("DM_DBG_COVER")) { char b[900]; int k = 0; for (int i = (int)n - 1; i >= 0 && k < 820; i--) { uint64_t p = *(uint64_t *)(ch + 8 * i); if (!p || !*(uint8_t *)(p + 0x50)) continue; const char *nm = (const char *)*(uint64_t *)(p + 8);
            int rh = ((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)p + 0x48))(p, SNAP_XY(*x, *y)) & 1, r0 = ((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)p + 0x48))(p, SNAP_XY(3, 3)) & 1;
            int lf = inside_walk(p, SNAP_XY(*x, *y), 1, base + 0x20c1328); if (!rh && !lf) continue;
            k += snprintf(b + k, sizeof b - k, " [%d]%s:根%d角%d叶=%s", i, nm ? nm : "?", rh, r0, lf ? g_inside_name : "-"); } LOG("盖层探查 (%d,%d)：%s", *x, *y, b); }
    int lo = top < 0 ? 0 : top;
#define SNAP_AT(px, py, from, to) ({ int hit_ = 0; for (int i_ = (to); i_ >= (from) && !hit_; i_--) { uint64_t p_ = *(uint64_t *)(ch + 8 * i_); if (p_ && *(uint8_t *)(p_ + 0x50) && (top < 0 || panel_skip((const char *)*(uint64_t *)(p_ + 8)) != 1) && !panel_clipped(p_, SNAP_XY(px, py))) hit_ = click_walk(p_, SNAP_XY(px, py), 1, base); } hit_; })
    if (SNAP_AT(*x, *y, lo, (int)n - 1)) return 1;   // 正压在别的可点控件上：不让滑块箭头的放大区抢 | Directly on another clickable widget: do not let the slider arrow's enlarged area take it
    if (slider_arrow(m, root, x, y, w, h)) return 2;
    g_click_exact = 1; int on = SNAP_AT(*x, *y, lo, (int)n - 1); g_click_exact = 0; if (on) return 1;
    static const int dx[8] = { 0, 0, -1, 1, -1, 1, -1, 1 }, dy[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };   // 先上下、再左右、再斜向：按钮多是横长条，上下偏的最多 | Up/down first, then left/right, then diagonals: buttons are mostly wide horizontal bars, so vertical misses are the most common
    float R = 90.0f * h / 2160.0f;
    for (int ring = 1; ring <= 9; ring++) for (int d = 0; d < 8; d++) { float r = R * ring / 9 * (d < 4 ? 1.0f : 0.7071f);   // 九圈（每圈约 0.4 毫米）：两个按钮之间的缝里落点归更近的那个 | Nine rings (about 0.4 mm each): a point in the gap between two buttons goes to the nearer one
        int px = *x + (int)(dx[d] * r), py = *y + (int)(dy[d] * r); if (px < 0 || py < 0 || px >= w || py >= h) continue;
        if (SNAP_AT(px, py, lo, (int)n - 1)) {   // 吸附到的点在控件边上：点击发到控件中心（沿这一点向四个方向探边）；中心若落在别的控件上（套着的子按钮）就留在取样点 | The snapped point is on the widget's edge: send the click to the widget center (probe edges in four directions from this point); if the center falls on another widget (a nested child button) stay at the sample point
            uint64_t cw = dm_wref_get(&g_click); int qx = px, qy = py; slot_center(cw, &qx, &qy, w, h, 0);
            if (SNAP_AT(qx, qy, lo, (int)n - 1) && dm_wref_get(&g_click) == cw) { px = qx; py = qy; } else { SNAP_AT(px, py, lo, (int)n - 1); }
            *x = px; *y = py; return 2; } }
    return 0;
}
static uint64_t ui_find(uint64_t w, const char *name, int dep);
// 吸附不许吸到的按钮：点错了后果重（丢改动、花金币、退局、删角色）。手指落在它们旁边 3.5 毫米内不再算点中，只认本体；
// 本体不足 44 点的（iPhone 档：屏高 375 点）外扩到 44 点见方，免得小按钮变得难点。Confirm 这个名字各确认框通用，只在删除角色确认框开着时算
// Buttons the snap must not snap to: a wrong tap has heavy consequences (losing changes, spending gold, leaving the game, deleting a character). A finger landing within 3.5 mm beside them no longer counts as a hit; only the body itself counts;
// bodies smaller than 44 points (iPhone profile: screen height 375 points) are extended to 44 points square, so that small buttons do not become hard to tap. The name Confirm is shared by all confirmation dialogs, and counts only while the delete-character confirmation dialog is open
static int snap_guarded(uint64_t root, const char *nm) {
    static const char *const k[] = { "CancelButton", "DefaultButton", "convert", "button_repair_all", "button_repair", "button_refresh", "button_blank", "Exit", "TouchRestart" };
    // 商人页的 button_repair 在我们的布局里就是「刷新货架」（借单件修理那颗控件，touch_layout.py），与 button_blank / button_refresh 同一颗钮
    // On the vendor page button_repair is 「刷新货架」(Refresh Stock) in our layout (borrowing the repair-single-item widget, touch_layout.py), the same button as button_blank / button_refresh
    if (!nm) return 0; for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strcmp(nm, k[i])) return 1;
    if (!strcmp(nm, "Confirm")) { uint64_t dm = ui_find(root, "DeleteCharacterModal", 0); return dm && *(uint8_t *)(dm + 0x50); }
    return 0;
}
static int snap_guard_near(uint64_t cw, const char *nm, int x, int y, int sx, int sy, int vw, int vh) {   // 触点 (x,y) 在本体外扩到 44 点见方之内？(sx,sy) = 吸附到的本体上一点 | is the touch point (x,y) within the widget body extended to 44 points square? (sx,sy) = the point on the body it was snapped to
    if (!cw || vh <= 0) return 0; uint64_t fn = *(uint64_t *)(*(uint64_t *)cw + 0x48); int x0 = sx, x1 = sx, y0 = sy, y1 = sy;
#define SG_HIT(px, py) (((int (*)(uint64_t, uint64_t))fn)(cw, (uint64_t)(uint32_t)(px) | ((uint64_t)(uint32_t)(py) << 32)) & 1)
    while (x0 > 0 && SG_HIT(x0 - 1, sy)) x0--; while (x1 < vw - 1 && SG_HIT(x1 + 1, sy)) x1++;
    while (y0 > 0 && SG_HIT(sx, y0 - 1)) y0--; while (y1 < vh - 1 && SG_HIT(sx, y1 + 1)) y1++;
#undef SG_HIT
    float pt = vh / 375.0f, wp = (x1 - x0 + 1) / pt, hp = (y1 - y0 + 1) / pt, ex = wp < 44 ? (44 - wp) / 2 * pt : 0, ey = hp < 44 ? (44 - hp) / 2 * pt : 0;
    int up = strcmp(nm, "CancelButton") && strcmp(nm, "DefaultButton");   // 设置页底下这两颗的上方紧贴选项列表（滚到框外的行离按钮上沿只有 4 点）：不向上扩 | these two at the bottom of the settings page sit right under the option list (a row scrolled out of the box is only 4 points from the buttons' top edge): do not extend upward
    int ok = x >= x0 - ex && x <= x1 + ex && y >= y0 - (up ? ey : 0) && y <= y1 + ey;
    LOG("吸附排除：%s 本体 %.1f×%.1f 点，触点离本体 横 %.1f 纵 %.1f 点，外扩 横 %.1f 纵 %.1f 点 → %s", nm, wp, hp, (x < x0 ? x0 - x : x > x1 ? x - x1 : 0) / pt, (y < y0 ? y0 - y : y > y1 ? y - y1 : 0) / pt, ex / pt, ey / pt, ok ? "算点中" : "不点");
    return ok;
}
static _Atomic uint64_t g_speech_end, g_speech_back_at, g_close_down;   // 任务语音停下的那一帧；到这一帧替玩家按返回关任务面板；最近一次按下落在右上角 X 上的那一帧 | The frame the quest voice stopped; on reaching this frame press back for the player to close the quest panel; the frame of the most recent press that landed on the top-right X
static _Atomic uint64_t g_set_back_at;   // 到这一帧替玩家按一下返回键（设置页点了应用 / 放弃） | On reaching this frame press the back key once for the player (apply / discard was tapped on the settings page)
static _Atomic int g_tab_delta, g_tab_top, g_tab_left; static _Atomic uint64_t g_tab_mask; static _Atomic uint64_t g_tab_then;
// NPC 对话菜单（交谈 / 交易或修复 / 灌魔 / 取消）：选项是 NpcDialogPanel 下 Container 里的 ListItemK（行高 72 单位，手机上约 3 毫米），不认鼠标点击
// （改布局的 isHoverList 也无效），手柄方向键上下移高亮、A 确认；推摇杆会一拨到底，不能用。高亮项 = 文字颜色与众不同的那行（+0xbc）；先按「蓝大于红」认，认不出再按与众不同。
// 点到哪一行（上下放宽到最近的一行）→ 方向键 × 差值，再按 A。
// NPC dialog menu (Talk / Trade or Repair / Imbue / Cancel): the options are ListItemK in the Container under NpcDialogPanel (row height 72 units, about 3 mm on a phone) and do not respond to mouse clicks
// (changing isHoverList in the layout has no effect either); D-pad up/down moves the highlight and A confirms; pushing the stick jumps all the way to the end and cannot be used. Highlighted item = the row whose text color differs from the rest (+0xbc); recognize by "blue greater than red" first, and by difference only if that fails.
// Whichever row is tapped (widened up and down to the nearest row) → D-pad × delta, then press A.
static int npc_menu_rows(uint64_t root, uint64_t pn, int x, int y, int vh, int *delta);
// 菜单自己记的当前选中行：面板 +0x248 = 它的列表控件（ListWidget，虚表 main+0x20b8df0），列表 +0xd0 = 那个 Container，+0x140 = 选中下标（0 起，-1 = 没有选中）。
// 方向键上 / 下改的就是它（列表消息处理 main+0xacbab4–0xacbb04），确认键按它定执行哪一项（面板 main+0x3daf4c 起）；各行的文字颜色只是它每帧的投影（main+0xac0da8）。
// 返回 1 = 读到了（*cur 可为 -1）；0 = 结构对不上（列表指针 / 虚表 / Container 不符）
// The currently selected row as recorded by the menu itself: panel +0x248 = its list widget (ListWidget, vtable main+0x20b8df0), list +0xd0 = that Container, +0x140 = selected index (0-based, -1 = no selection).
// This is what D-pad up / down changes (list message handling main+0xacbab4–0xacbb04), and the confirm key decides which item to execute from it (panel, from main+0x3daf4c); each row's text color is only its per-frame projection (main+0xac0da8).
// Returns 1 = read (*cur may be -1); 0 = the structure does not match (list pointer / vtable / Container mismatch)
static _Atomic int g_menu_want = -1;   // 这次点的是第几行：挪完方向键、按确认键之前拿它和菜单自己记的选中行核一遍 | Which row this tap is on: after moving with the D-pad and before pressing confirm, verify it against the selected row the menu itself records
// 商人页上的交易结果提示（「你没有足够的金币那么做。／继续」「…空间…」）：收它用返回键，不用确认键（本机复现）。
// 点修理 / 全部修理之后，手柄焦点留在那个修理按钮上，按确认键 = 再修一次 → 同一个提示又弹出来，点哪都退不出去；
// 返回键是原版收这个提示的键：游戏发 VendorPanelMessage ResetToIdle，提示收掉、商人页照常（本机：之后点货架、点 X 都正常）
// Transaction result prompt on the vendor page (「你没有足够的金币那么做。／继续」(You do not have enough gold for that. / Continue), 「…空间…」(...space...)): dismiss it with the back key, not the confirm key (reproduced on the local machine).
// After tapping repair / repair all, controller focus stays on that repair button, so pressing confirm = repair again → the same prompt pops up again and no tap anywhere gets out;
// the back key is the original game's key for dismissing this prompt: the game sends VendorPanelMessage ResetToIdle, the prompt is dismissed and the vendor page carries on (local machine: tapping the shelf and tapping X afterwards both work)
static int g_menu_dismiss;
static int npc_menu_sel(uint64_t pn, uint64_t ct, int *cur) {
    module_t *m = main_module(); uint64_t ls = pn ? *(uint64_t *)(pn + 0x248) : 0; if (!m || !ls || *(uint64_t *)ls != (uint64_t)m->base + 0x20b8df0 || *(uint64_t *)(ls + 0xd0) != ct) return 0;
    int32_t c = *(int32_t *)(ls + 0x140); uint64_t n = *(uint64_t *)(ct + 0x60); *cur = c >= 0 && (uint64_t)c < n ? c : -1; return 1;
}
static int npc_menu(uint64_t root, int x, int y, int vh, int *delta) {
    atomic_store(&g_menu_want, -1);   // 只有「点中了某一行」才记目标行；单项提示框 / 商人页上的结果框是「点哪都算点当前项」，不核对 | The target row is recorded only when "a row was hit"; single-item prompt boxes / the result box on the vendor page are "a tap anywhere counts as the current item" and are not verified
    uint64_t pn = ui_find(root, "NpcDialogPanel", 0); if (!pn || !*(uint8_t *)(pn + 0x50)) return 0;
    int shop = dm_shop_open(), shown = has_visible_leaf(pn, 1);
    if (shop && !shown) return 0;   // 商人页开着时对话面板一直在（空壳，没有可见内容，里面还留着上一个菜单的行）：不是提示框，别认 | While the vendor page is open the dialog panel is always there (an empty shell with no visible content, still holding the previous menu's rows): it is not a prompt box, do not recognize it
    g_menu_dismiss = 0;
    if (npc_menu_rows(root, pn, x, y, vh, delta)) { if (shop && g_menu_dismiss) *delta = 0; return 1; }
    // 商人页上盖着的对话框 = 交易结果提示，是模态的：没点在行上、或它的结构认不出行（背包满「你没有足够的空间那么做。」这个框
    // 就认不出——mini：点框外落到了底下的货架上），都算「继续」= 按返回键收掉（见 g_menu_dismiss）
    // A dialog covering the vendor page = the transaction result prompt, which is modal: not tapping on a row, or its structure not yielding rows (the inventory-full box 「你没有足够的空间那么做。」(You do not have enough free space to do that.)
    // is not recognized - observed on the second Mac: a tap outside the box landed on the shelf underneath), all count as 「继续」(Continue) = press the back key to dismiss it (see g_menu_dismiss)
    if (shop) { *delta = 0; g_menu_dismiss = 1; return 1; }
    return 0;
}
static int npc_menu_rows(uint64_t root, uint64_t pn, int x, int y, int vh, int *delta) {
    (void)root;
    uint64_t ae = ui_find(pn, "ActiveElements", 0), ct = ae ? ui_find(ae, "Container", 0) : 0; if (!ct) return 0;
    uint64_t ch = *(uint64_t *)(ct + 0x58), n = *(uint64_t *)(ct + 0x60); if (!ch || !n || n > 32) return 0;
    int hit = -1, cur = -1; uint32_t col[32];
    for (uint64_t i = 0; i < n; i++) { uint64_t it = *(uint64_t *)(ch + 8 * i), tb = it && *(uint64_t *)(it + 0x60) ? *(uint64_t *)*(uint64_t *)(it + 0x58) : 0; col[i] = tb ? *(uint32_t *)(tb + 0xbc) : 0; }
    for (uint64_t i = 0; i < n; i++) { int same = 0; for (uint64_t j = 0; j < n; j++) same += col[j] == col[i]; if (same == 1 && n >= 3) cur = (int)i; }
    // 只有两行的菜单（新角色找卡夏 / 瓦瑞夫：「交谈 / 取消」；阿卡拉重设：「好 / 取消」）：两行颜色各不相同，「与众不同」比不出来——cur 算不出就落到鼠标点击那条路，
    // 这类行不认鼠标，点哪行都没反应。+0xbc 起四个数是文字色的红 / 绿 / 蓝 / 透明度（mini 探查）：高亮行蓝紫（6e,6e,ff），
    // 普通行白或浅灰（f0,f0,f0 或 ff,ff,ff——两种都量到过，不稳定；首版拿 0xf0 当常量比，玩家路径上是 0xff，4/4 仍点不动）。
    // 所以按语义认、不比具体数值：蓝大于红的那行就是高亮项（行数不限）；一行都不是时保留上面「与众不同」的结果
    // Menus with only two rows (a new character at Kashya / Warriv: 「交谈 / 取消」(Talk / Cancel); Akara's reset: 「好 / 取消」(OK / Cancel)): the two rows each have a different color, so "differs from the rest" gives nothing - when cur cannot be computed it falls to the mouse-click path,
    // such rows ignore the mouse, and tapping either row does nothing. The four numbers from +0xbc are the text color's red / green / blue / alpha (probed on the second Mac): the highlighted row is blue-violet (6e,6e,ff),
    // normal rows are white or light gray (f0,f0,f0 or ff,ff,ff - both have been measured, not stable; the first version compared against 0xf0 as a constant, it is 0xff on the player path, and 4/4 still could not be tapped).
    // So recognize by meaning rather than exact values: the row with blue greater than red is the highlighted item (any number of rows); when no row qualifies, keep the "differs from the rest" result above
    { int blue = -1, nb = 0; for (uint64_t i = 0; i < n; i++) { uint64_t it = *(uint64_t *)(ch + 8 * i), tb = it && *(uint64_t *)(it + 0x60) ? *(uint64_t *)*(uint64_t *)(it + 0x58) : 0;
          if (tb && *(uint32_t *)(tb + 0xc4) > *(uint32_t *)(tb + 0xbc)) { blue = (int)i; nb++; } }
      if (nb == 1) cur = blue; }
    // 以上按文字颜色认只是兜底：菜单自己记着选中行，读得到就用它（同一个固定触点 3 局里 2 局落到下一行，查到最后是菜单位置逐局不同，
    // 但颜色这条路本身是脆的——两种白、两行 / 一行菜单都要特判）
    // Recognizing by text color above is only a fallback: the menu itself records the selected row, so use it when readable (the same fixed touch point landed on the next row in 2 of 3 runs, which in the end turned out to be the menu position differing run to run,
    // but the color path itself is fragile - two whites, and two-row / one-row menus all need special cases)
    int col_cur = cur, sel = -1, have = npc_menu_sel(pn, ct, &sel); if (have) cur = sel;
    // 左右也放宽（各 7 毫米，由近到远）：菜单行只有文字那么宽（约 50 点），手指稍偏就落到菜单外 = 关对话（审计时误关 1 次）
    // Also widened left and right (7 mm each, from near to far): a menu row is only as wide as its text (about 50 pt), and a slightly off finger lands outside the menu = closes the dialog (closed by mistake once during the audit)
    static const int xoff[7] = { 0, 70, -70, 140, -140, 210, -210 };
    // 上下由近到远、每 6 单位一档、同一档先上后下：触点落在两行之间的缝里时归最近的那一行（原先 0 / +18 / -18 … 固定先往下找，缝里的触点一律判给下一行）
    // Up and down from near to far, one step per 6 units, up before down within a step: a touch point in the gap between two rows goes to the nearest row (previously 0 / +18 / -18 ... always searched downward first, so touch points in the gap always went to the next row)
    int used = 0;
    for (int q = 0; q < 7 && hit < 0; q++) for (int k = 0; k < 21 && hit < 0; k++) { int dy = (k + 1) / 2 * 6 * (k & 1 ? -1 : 1), yy = y + dy * vh / 2160, xx = x + xoff[q] * vh / 2160; if (xx < 0 || yy < 0) continue; uint64_t xy = (uint64_t)(uint32_t)xx | ((uint64_t)(uint32_t)yy << 32); used = dy;
        for (uint64_t i = 0; i < n && hit < 0; i++) { uint64_t it = *(uint64_t *)(ch + 8 * i); if (it && *(uint8_t *)(it + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)it + 0x48))(it, xy) & 1)) hit = (int)i; } }
    // 只有一项的对话框（交易结果「你没有足够的金币那么做。／继续」）：没有「颜色与众不同的那行」可比，那一项就是高亮项；整个框都算点它
    // （原先 cur 算不出 → 落到鼠标点击那条路，这类行不认鼠标，「继续」点不掉）
    // A dialog with only one item (transaction result 「你没有足够的金币那么做。／继续」(You do not have enough gold for that. / Continue)): there is no "row whose color differs from the rest" to compare, so that one item is the highlighted item; the whole box counts as tapping it
    // (previously cur could not be computed → it fell to the mouse-click path, such rows ignore the mouse, and 「继续」(Continue) could not be dismissed)
    if (n == 1) { *delta = 0; g_menu_dismiss = 1; return 1; }   // 点哪里都算点它（它是模态的提示：框外点一下也该关掉——mini 弹窗还开着时去拖底下的货，命中落在背景上、拖不起来，看着像卡死） | A tap anywhere counts as tapping it (it is a modal prompt: a tap outside the box should close it too - on the second Mac, dragging an item underneath while the popup was still open, the hit landed on the background, the drag would not start, and it looked frozen)
    if (hit < 0 || (cur < 0 && !have)) return 0;
    atomic_store(&g_menu_want, hit);
    { uint64_t hw = *(uint64_t *)(ch + 8 * hit); const char *hnm = hw && *(uint64_t *)(hw + 8) ? (const char *)*(uint64_t *)(hw + 8) : "?";
    LOG("界面对话菜单：%llu 行，点中第 %d 行（%s）%s，选中行 %d（%s%s）", (unsigned long long)n, hit, hnm, used ? "（触点在行外，就近）" : "", cur, have ? "菜单自己记的" : "【红】列表结构对不上，按文字颜色认的", have && col_cur != cur ? "；按颜色认会是另一行" : ""); }
    *delta = hit - cur; return 1;
}

// 模态框在最上层时，框外的触摸不进底下的面板（提取框开着时去拖背包物品，松手那下确认键被数量框吃掉 = 点了「好」，
// 箱子里 60457 金币被全取走；mini 上复现）。最上层那个面板名字以 Modal 结尾且有可见内容 = 模态框；
// 点不在它里面 → 9（模态框外）。技能绑定框（ButtonBindingModal）有自己的处理和专用取消键，不算在内。模态框怎么认见 modal_any。
// When a modal is on top, touches outside it do not reach the panels underneath (with the withdraw box open, dragging an inventory item: the confirm key on release was swallowed by the quantity box = tapped 「好」(OK),
// and all 60457 gold in the stash was withdrawn; reproduced on the second Mac). The topmost panel whose name ends in Modal and has visible content = a modal;
// a point not inside it → 9 (outside a modal). The skill binding box (ButtonBindingModal) has its own handling and a dedicated cancel key and is not included. See modal_any for how modals are recognized.
static uint64_t modal_top(module_t *m, uint64_t root, const char **name) { (void)m; return modal_any(root, 0, name); }
static int ui_classify0(module_t *m, uint64_t root, int x, int y, char *hit, size_t hn);
static int swaptab_hit(uint64_t root, int x, int y); int dm_swapskill_wset(void); uint32_t dm_swapskill_itemsig(void); static _Atomic int g_swaptab_tap; static void fe_send(const char *cat, const char *act, const char *arg);
// 触点落在界面矩形之外（iPad 真全屏：界面贴底，上方露着世界；触点的归一化坐标 < 0 或 > 1）：一律算面板外——模态框开着时算模态框外。
// 原先各入口把坐标夹回 [0,1] 再判，点面板上方的世界就成了点面板最顶一排（页签、右上角关闭），Mac「iPad」包上点一下切了页签。
// 入口记在这个量里（异步查询经 g_iq_out 带到主线程），只在紧接着的那一次判类生效。
// A touch point outside the UI rect (iPad true fullscreen: the UI is bottom-aligned with the world showing above; the touch point's normalized coordinate is < 0 or > 1): always counts as outside the panel - or outside the modal when a modal is open.
// Previously each entry clamped the coordinates back to [0,1] before judging, so tapping the world above the panel became tapping the panel's top row (tabs, the top-right close), and on the Mac "iPad" app one tap switched tabs.
// The entry records it in this variable (asynchronous queries carry it to the main thread via g_iq_out), and it applies only to the classification that immediately follows.
static int g_cls_out;
// 符文之语清单（dm/host/dm_runeword.c）：长按带孔底材 → 打开清单（1 = 是匹配的底材、已打开）；清单开着时点清单外 → 关
// Runeword list (dm/host/dm_runeword.c): long press on a socketed base → open the list (1 = it is a matching base and the list opened); while the list is open, a tap outside it → close
int dm_runeword_long_press(const dm_cursor_item_info *ci);
int dm_runeword_list_open(void);   // 清单开着 = 1 | List open = 1
void dm_runeword_list_close(void);
void dm_runeword_button(const char *name);   // 清单里点中的控件（名字前缀 TouchRw）：宿主按名字决定做什么（按钮执行、名字开详情、底板 / 文字不动） | The widget hit in the list (name prefix TouchRw): the host decides what to do by name (buttons execute, names open details, backing / text do nothing)
// 符文之语清单（控件都在物品栏面板下的 TouchRwBox 容器里，名字前缀 TouchRw）：点中哪个可见控件。返回 0 = 不在清单上，1 = 在清单上，*btn = 点中的最上层那个控件名
// （同层后画的在上：按钮 / 详情框压在底板上，两个都中时取后面那个）
// Runeword list (its widgets are all in the TouchRwBox container under the inventory panel, name prefix TouchRw): which visible widget is hit. Returns 0 = not on the list, 1 = on the list, *btn = name of the topmost widget hit
// (within a layer later-drawn is on top: buttons / the detail box sit on the backing, and when both are hit the later one is taken)
static int rw_hit(uint64_t w, uint64_t xy, int dep, const char **btn) {
    if (!w || dep > 5 || !*(uint8_t *)(w + 0x50)) return 0; const char *nm = *(uint64_t *)(w + 8) ? (const char *)*(uint64_t *)(w + 8) : "";
    int r = 0; if (!strncmp(nm, "TouchRw", 7) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x48))(w, xy) & 1)) { *btn = nm; r = 1; }
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return r;
    for (uint64_t i = 0; i < n; i++) if (rw_hit(*(uint64_t *)(ch + 8 * i), xy, dep + 1, btn)) r = 1;
    return r;
}
// 我们自己的控件（名字以 Touch 开头）改可见：+0x50 可见、+0x51 收点击，两个都写。不是我们的控件就报红、不写（规则：不写原版控件；静态扫 tests/native_write_scan.py 只许在这类函数里写这两个字节）
// Change visibility of our own widgets (names starting with Touch): +0x50 visible, +0x51 receives clicks, both written. If it is not our widget, report a failure and do not write (rule: no writes to the original game's widgets; the static scan tests/native_write_scan.py only allows writing these two bytes in functions of this kind)
static void ui_show_ours(uint64_t w, int on) {
    const char *nm = w ? (const char *)*(uint64_t *)(w + 8) : NULL; if (!w) return;
    if (!nm || strncmp(nm, "Touch", 5)) { static int lg; if (lg++ < 8) LOG("【红】界面：要改可见的控件「%s」不是我们的（Touch 开头），不写", nm ? nm : "?"); return; }
    if (*(uint8_t *)(w + 0x50) != (uint8_t)on || *(uint8_t *)(w + 0x51) != (uint8_t)on) { *(uint8_t *)(w + 0x50) = (uint8_t)on; *(uint8_t *)(w + 0x51) = (uint8_t)on; }
}
static void ui_move_ours(uint64_t w, int x) {   // 我们自己的控件（Touch 开头）挪 x（矩形 +0x70，改完虚表 +0x10 重排）；原版控件只许走位置守卫 mv_offset | Move x of our own widgets (names starting with Touch) (rect +0x70, vtable +0x10 re-layout after changing); the original game's widgets may only go through the position guard mv_offset
    const char *nm = w ? (const char *)*(uint64_t *)(w + 8) : NULL; if (!w) return;
    if (!nm || strncmp(nm, "Touch", 5)) { static int lg; if (lg++ < 8) LOG("【红】界面：要挪的控件「%s」不是我们的，不写", nm ? nm : "?"); return; }
    int32_t *r = (int32_t *)(w + 0x70); if (r[0] != x) { r[0] = x; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x10))(w); }
}
#define PG_REACH 112   // 个人仓库翻页箭头的外圈（布局单位，自箭头命中框边缘起） | outer ring of the personal stash page-flip arrows (layout units, from the edge of the arrow's hit box)
static int pg_personal(uint64_t root) {   // 储藏箱开着且停在个人页（页签 0） | the stash is open and on the personal page (tab 0)
    uint64_t bk = ui_panel(root, "BankExpansionLayout"), tabs = bk && *(uint8_t *)(bk + 0x50) ? ui_find(bk, "BankTabs", 0) : 0; return tabs && TAB_SEL(tabs) == 0;
}
static int pg_near(uint64_t root, int x, int y, int vh) {   // 这一点在哪个翻页箭头的外圈里：-1 左 / +1 右 / 0 都不在。由近到远八个方向取样箭头自己的命中函数（同 ui_snap 的取法） | which page-flip arrow's outer ring this point is in: -1 left / +1 right / 0 neither. Samples the arrow's own hit-test function in eight directions from near to far (same method as ui_snap)
    uint64_t bk = ui_panel(root, "BankExpansionLayout"); if (!bk || vh <= 0) return 0;
    static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, dy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    for (int r = 16; r <= PG_REACH; r += 16) for (int k = 0; k < 2; k++) { uint64_t t = ui_find(bk, k ? "TouchStashRight" : "TouchStashLeft", 0); if (!t || !*(uint8_t *)(t + 0x50)) continue;
        uint64_t fn = *(uint64_t *)(*(uint64_t *)t + 0x48);
        for (int d = 0; d < 8; d++) { int xx = x + dx[d] * r * vh / 2160, yy = y + dy[d] * r * vh / 2160; if (xx < 0 || yy < 0) continue;
            if (((int (*)(uint64_t, uint64_t))fn)(t, (uint64_t)(uint32_t)xx | ((uint64_t)(uint32_t)yy << 32)) & 1) return k ? 1 : -1; } }
    return 0;
}
static int ui_classify(module_t *m, uint64_t root, int x, int y, char *hit, size_t hn) {   // 1 物品区 / 2 页签 / 0 面板内其它控件 / 3 面板外 / 9 模态框外 | 1 item area / 2 tab / 0 other widget inside the panel / 3 outside the panel / 9 outside a modal
    if (g_cls_out) { g_cls_out = 0; const char *on = "";
        if (modal_top(m, root, &on)) { snprintf(hit, hn, "模态框外 %s", on); g_modal_name = on; return 9; }
        g_inside_owner = ""; atomic_store(&g_bd_hit, 0); atomic_store(&g_close_down, 0); snprintf(hit, hn, "（界面矩形外）"); return 3; }
    // 编年史页（储藏箱第六页）上的点一律算「面板内其它控件」（0）：这一页下面是原版嵌入魔盒容器（游戏在第六页显示它），不许判成物品区去拿魔盒里的东西；
    // 点下去交给 dm_vault_ui_tap（格子 / 自己的控件 / 其余吃掉），拖动交给 dm_vault_ui_drag（仓库页按独立页面处理）。下拉展开的选项面板开着时 dm_vault_ui_over 答 0，照常判
    // Taps on the chronicle page (stash page six) always count as "other widget inside the panel" (0): under this page is the original game's embedded cube container (the game shows it on page six), and it must not be judged as an item area to pick up things in the cube;
    // a tap goes to dm_vault_ui_tap (cells / our own widgets / the rest swallowed), a drag goes to dm_vault_ui_drag (the vault page is handled as an independent page). While a dropdown's expanded option panel is open dm_vault_ui_over answers 0 and judging proceeds as usual
    if (dm_ui_panel_state() & DM_PS_VAULT) { int dm_vault_ui_over(int, int); if (dm_vault_ui_over(x, y)) { g_inside_owner = ""; atomic_store(&g_bd_hit, 0); snprintf(hit, hn, "编年史页"); return 0; } }
    // 设置页盖在暂停菜单上，暂停菜单的按钮却还收点击（游戏的分发不看谁在上面）：点在暂停菜单按钮上、又不在设置页的可点控件上 = 被盖住，不点（同「压着被上层面板盖住的按钮」）。
    // 原先设置页开着时把暂停菜单整层设成不可见、关了写回——那是写原版控件，违反不写原版控件的规则，改成这里只在命中时挡
    // The settings page covers the pause menu, yet the pause menu's buttons still receive clicks (the game's dispatch does not look at who is on top): on a pause menu button and not on a clickable widget of the settings page = covered, do not click (same as "on a button covered by an upper panel").
    // Previously, while the settings page was open the whole pause menu layer was set invisible and written back on close - that is writing to the original game's widgets, violating the no-writes-to-original-widgets rule; changed to blocking here only on a hit
    { uint64_t st = ui_panel(root, "SettingsPanel"), xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32);
      uint64_t cp = ui_panel(root, "VideoOptionsPanel");   // 自定义页（借的视频页）与设置页并列、不是它的子控件：这一页上的勾选框也算「设置页自己可点的控件」（语言行的「中文」那格正落在暂停菜单按钮的位置上） | The Custom page (the borrowed video page) sits beside the settings page and is not its child: checkboxes on this page also count as "controls of the settings page itself that can be tapped" (the 「中文」 box of the language row lands exactly where the pause menu button is)
      if (st && *(uint8_t *)(st + 0x50) && !click_walk(st, xy, 1, (uint64_t)m->base) && !(cp && *(uint8_t *)(cp + 0x50) && click_walk(cp, xy, 1, (uint64_t)m->base))) {   // 设置页自己可点的控件照常；点在它的底板 / 列表空处时，游戏会把这一下交给底下的暂停菜单按钮 | The settings page's own clickable widgets work as usual; when the tap is on its backing / empty list space, the game would hand this tap to the pause menu button underneath
          static const char *const pl[] = { "PauseLayoutGarden", "PauseLayout" };
          for (int i = 0; i < 2; i++) { uint64_t pw = ui_panel(root, pl[i]); if (pw && *(uint8_t *)(pw + 0x50) && click_walk(pw, xy, 1, (uint64_t)m->base)) { snprintf(hit, hn, "设置页盖着的暂停菜单按钮（不点，可滚）"); return 11; }   /* 11：这一下不点下去，但从这里起手上下拖照常滚设置页的列表（自定义页各行的文字不是可点控件，玩家多半就在文字上滑） | 11: this tap is not pressed down, but a vertical drag starting here still scrolls the settings page list as usual (the text of the rows on the Custom page is not a clickable widget, and players mostly swipe right on the text) */ } } }
    const char *mn = ""; uint64_t mp = modal_top(m, root, &mn);
    if (mp) { uint64_t xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32), fn = *(uint64_t *)(*(uint64_t *)mp + 0x48);
        // 「在框里」：框自己有边界且点在里面，或点中它的某个可见叶子 / 可点控件（确认框的「应用 / 放弃」按钮只有后一种认得到——
        // 只用叶子判时点「应用」被当成框外，确认框按不动）
        // "Inside the box": the box has its own bounds and the point is inside, or one of its visible leaves / clickable widgets is hit (the confirmation box's 「应用 / 放弃」(Apply / Discard) buttons are only caught by the latter -
        // judging by leaves alone, a tap on 「应用」(Apply) was treated as outside the box and the confirmation box could not be pressed)
        int in = ((((int (*)(uint64_t, uint64_t))fn)(mp, xy) & 1) && !(((int (*)(uint64_t, uint64_t))fn)(mp, (uint64_t)3 | ((uint64_t)3 << 32)) & 1))
              || inside_walk(mp, xy, 1, (uint64_t)m->base + 0x20c1328) || click_walk(mp, xy, 1, (uint64_t)m->base);
        if (!in) { snprintf(hit, hn, "模态框外 %s", mn); g_modal_name = mn; return 9; } }
    // 单件任务槽面板（灌魔 / 打孔 / 个性化 / 古墓底座）的关闭钮画在外框右上角凹槽里，但它属于最先打开的 ImbueItemsPanel，凹槽那块被后开的背包面板与外框盖在上层：
    // 按层级它不算可点控件，游戏自己的鼠标分发也到不了它（点了有鼠标按下抬起、没有关面板消息）。点中它 = 走「点面板外 = 关面板」那条现成的路（原版的返回键）
    // The close button of the single-item quest slot panel (imbue / socketing / personalize / tomb pedestal) is drawn in the notch at the top right of the outer frame, but it belongs to the ImbueItemsPanel opened first, and that notch area is covered on upper layers by the later-opened inventory panel and the outer frame:
    // by layering it does not count as a clickable widget, and the game's own mouse dispatch cannot reach it either (tapping gives mouse down/up but no close-panel message). Hitting it = take the existing "tap outside the panel = close the panel" path (the original game's back key)
    { uint64_t ip = ui_find(root, "ImbueItemsPanel", 0), tc = ip && *(uint8_t *)(ip + 0x50) ? ui_find(ip, "TouchClose", 1) : 0;
      if (tc && *(uint8_t *)(tc + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)tc + 0x48))(tc, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32)) & 1)) {
          g_inside_owner = ""; atomic_store(&g_bd_hit, 0); atomic_store(&g_close_down, 0); snprintf(hit, hn, "单件任务槽面板的关闭钮（按面板外算）"); return 3; } }
    if (dm_runeword_list_open()) {   // 符文之语清单开着（dm_runeword.c）：点清单上任何地方都吃掉、交给宿主按控件名处理；点清单外关清单并吃掉 | Runeword list open (dm_runeword.c): a tap anywhere on the list is swallowed and handed to the host to handle by widget name; a tap outside the list closes the list and is swallowed
        // 一律不发鼠标点击（同技能树 I / II 页签）：清单压在物品格上，发鼠标点击的话游戏自己的分发先给了底下的格子
        // （第 2 页的左箭头落在腰带格上，鼠标点击发了、箭头没收到，页没翻）——点清单绝不能落成点物品
        // Never send a mouse click (same as the skill tree I / II tabs): the list sits on top of item cells, and with a mouse click the game's own dispatch gives it to the cell underneath first
        // (the left arrow of page 2 lands on a belt cell: the mouse click was sent, the arrow did not get it, and the page did not flip) - a tap on the list must never turn into a tap on an item
        const char *bn = NULL; int rw = rw_hit(ui_find(root, "PlayerInventoryExpansionLayout", 0), (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32), 1, &bn);
        if (rw && bn) { dm_runeword_button(bn); snprintf(hit, hn, "符文之语清单 %s（宿主处理，不发鼠标点击）", bn); return 6; }
        if (!rw) { dm_runeword_list_close(); snprintf(hit, hn, "符文之语清单外 → 关清单"); return 6; } }
    if (swaptab_hit(root, x, y)) { atomic_store(&g_swaptab_tap, 1); snprintf(hit, hn, "技能树 I / II 页签 → 换武器"); return 6; }   // 吃掉这一下，不发鼠标点击 | Swallow this tap, no mouse click sent
    { uint64_t bk = ui_panel(root, "BankExpansionLayout"), tabs = bk && *(uint8_t *)(bk + 0x50) ? ui_find(bk, "BankTabs", 0) : 0;   // 个人页（页签 0）上的左右箭头 = 翻个人仓库页（dm_pages.c）；共享页照旧交给游戏；第六页「编年史」不用这两个原版箭头（被盖板压着，翻页条是它自己的控件，点按在 dm_vault_ui_tap 里收） | The left / right arrows on the personal page (tab 0) = turn personal stash pages (dm_pages.c); shared pages are left to the game as before; the sixth page 「编年史」(Chronicle) does not use these two original arrows (they are under the cover; the page bar is its own widget, and taps are handled in dm_vault_ui_tap)
      if (tabs && TAB_SEL(tabs) == 0) for (int k = 0; k < 2; k++) { uint64_t t = ui_find(bk, k ? "TouchStashRight" : "TouchStashLeft", 0);
          if (t && *(uint8_t *)(t + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)t + 0x48))(t, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32)) & 1)) {
              void dm_pages_request_flip(int); dm_pages_request_flip(k ? 1 : -1); snprintf(hit, hn, "个人仓库%s一页", k ? "下" : "上"); return 6; } } }
    return ui_classify0(m, root, x, y, hit, hn);
}
static int ui_classify0(module_t *m, uint64_t root, int x, int y, char *hit, size_t hn) {
    uint64_t xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32); atomic_store(&g_bd_hit, 0); g_inside_owner = "";
    uint64_t rch = *(uint64_t *)(root + 0x58), rn = *(uint64_t *)(root + 0x60); if (!rch || rn > 256) return 3;
    g_iw_base = (uint64_t)m->base;
    int lo = ui_top_cover(m, root, x, y); if (lo < 0) lo = 0;   // 只看盖住这一点的最上层面板和它上面的 | Only look at the topmost panel covering this point and those above it
    // 商人页上的交易结果提示是模态的，先于物品区判：它不盖住的地方（货架、背包格）点下去也算点「继续」，不许落到底下的格子上
    // The transaction result prompt on the vendor page is modal and is judged before the item area: tapping where it does not cover (shelf, inventory cells) also counts as tapping 「继续」(Continue), and must not fall onto the cells underneath
    if (dm_shop_open()) { int nd = 0; if (npc_menu(root, x, y, *(int32_t *)(m->base + 0x211da3c), &nd)) {
        if (g_menu_dismiss) { atomic_store(&g_tab_top, 4); snprintf(hit, hn, "商人页结果提示（按返回收）"); return 2; }
        atomic_store(&g_tab_delta, nd); atomic_store(&g_tab_top, 3); snprintf(hit, hn, "对话菜单 %+d 行", nd); return 2; } }
    for (int i = (int)rn - 1; i >= lo; i--) { uint64_t pw = *(uint64_t *)(rch + 8 * i); if (item_walk(pw, xy, 1, hit, hn)) { g_slot_panel = pw ? (const char *)*(uint64_t *)(pw + 8) : NULL; return 1; } }
    { int nd = 0; if (npc_menu(root, x, y, *(int32_t *)(m->base + 0x211da3c), &nd)) { atomic_store(&g_tab_delta, nd); atomic_store(&g_tab_top, 3); snprintf(hit, hn, "对话菜单 %+d 行", nd); return 2; } }
    int d = 0, top = 0, th = 0; { int32_t vh = *(int32_t *)(m->base + 0x211da3c); static const int off[5] = { 0, 45, -45, 90, -90 };   // 页签条只有 3.5–4.7 毫米高：上下各放宽到 90 单位 | The tab bar is only 3.5–4.7 mm high: widened to 90 units above and below
        // 放宽的那几档（i ≥ 1）只在触点本身没压着别的可点控件时才用：小站页每幕第一行紧贴幕页签，
        // 行的上半截被向下放宽的页签抢走（点第一行目的地成了切幕）。触点正压在页签上（i = 0）不受影响
        // The widened steps (i ≥ 1) are used only when the touch point itself is not on another clickable widget: on the waypoint page the first row of each act sits right against the act tabs,
        // and the upper half of the row was taken by the downward-widened tab (tapping the first-row destination became an act switch). A touch point directly on a tab (i = 0) is unaffected
        int on_ctl = -1;
        for (int i = 0; i < 5 && !th; i++) { if (i == 1) { int qx = x, qy = y; if (on_ctl < 0) on_ctl = ui_snap(m, root, &qx, &qy) == 1; if (on_ctl) break; }
            int yy = y + off[i] * vh / 2160; if (yy < 0 || yy >= vh) continue; int tl = ui_top_cover(m, root, x, yy); if (tl < 0) tl = 0;
            for (int k = (int)rn - 1; k >= tl && !th; k--) th = tab_walk(*(uint64_t *)(rch + 8 * k), (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)yy << 32), 1, (uint64_t)m->base + 0x20ba3a8, &d, &top); } }
    if (th) { atomic_store(&g_tab_delta, d); atomic_store(&g_tab_top, top); snprintf(hit, hn, "页签 %+d%s", d, top ? "（顶部）" : ""); return 2; }
    // 手柄版技能树的页内分页（防御光环 / 攻击光环…）不是页签条控件，是四个文字框 TextTabK，只认 ZL / ZR：点在第 K 个文字框上 = 按差值走。
    // 当前页 = TabK 里 Background 可见的那个（「综合技能」那页 Tab3 没有 Background：前三个都不可见就是它）。只在手柄版布局认（触屏版布局的技能树页签是可点的）
    // The in-page pagination of the gamepad-layout skill tree (Defensive Auras / Offensive Auras ...) is not a tab-bar widget but four text boxes TextTabK that only respond to ZL / ZR: a click on the K-th text box = step by the difference.
    // The current page = the TabK whose Background is visible (on the "general skills" page Tab3 has no Background: if none of the first three is visible, it is that one). Recognised only in the gamepad layout (in the touch layout the skill tree tabs are clickable)
    { int dm_padmode_current(void); uint64_t sp = dm_padmode_current() ? ui_find(root, "SkillsTreePanel", 0) : 0;
      if (sp && *(uint8_t *)(sp + 0x50) && *(uint8_t *)(sp + 0x51)) { int tgt = -1, cur = 3; char nm[16];
          for (int k = 0; k < 4; k++) { snprintf(nm, sizeof nm, "TextTab%d", k); uint64_t tw = ui_find(sp, nm, 0);
              if (tw && *(uint8_t *)(tw + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)tw + 0x48))(tw, xy) & 1)) tgt = k;
              snprintf(nm, sizeof nm, "Tab%d", k); uint64_t tb = k < 3 ? ui_find(sp, nm, 0) : 0, bg = tb ? ui_find(tb, "Background", 0) : 0; if (bg && *(uint8_t *)(bg + 0x50)) cur = k; }
          if (tgt >= 0) { dm_wref_set(&g_tab_ref, 0); atomic_store(&g_tab_have, 0); atomic_store(&g_tab_delta, tgt - cur); atomic_store(&g_tab_top, 0); snprintf(hit, hn, "技能树分页 %+d", tgt - cur); return 2; } } }
    { int sx = x, sy = y, sn = ui_snap(m, root, &sx, &sy); if (sn == 6) { snprintf(hit, hn, "盖:%s 不点", g_inside_name); return 6; }
      if (sn == 1) { if (snap_guarded(root, g_click_name)) snap_guard_near(dm_wref_get(&g_click), g_click_name, x, y, sx, sy, *(int32_t *)(m->base + 0x211da38), *(int32_t *)(m->base + 0x211da3c));   // 只为留一行本体点数读数 | only to leave one line of readout of the widget body's size in points
          snprintf(hit, hn, " 可点控件 %s", g_click_name); bd_note_skill(m, root, sx, sy); return 0; }
      // 个人仓库的翻页箭头（命中框只有 64 单位见方，手机上约 13 点）：点偏了被吸附到箭头上时，同样发翻页请求——翻个人仓库页是宿主做的（dm_pages.c），
      // 原先只有正点在命中框里才发；吸附过去的那一下只发了原版点击，原版在个人页不认，表现为「点了没反应」（的翻页偶发点不动）
      // The page-flip arrows of the personal stash (the hit box is only 64 units square, about 13 points on a phone): when an off-target tap is snapped onto an arrow, the page-flip request is sent as well -- flipping personal stash pages is done by the host (dm_pages.c),
      // and previously it was sent only for a tap squarely inside the hit box; a tap snapped over sent only the original game's click, which the original game ignores on the personal page, showing up as "tapped and nothing happened" (page flipping that occasionally would not respond)
      if (sn && (!strcmp(g_click_name, "TouchStashLeft") || !strcmp(g_click_name, "TouchStashRight")) && pg_personal(root)) {
          void dm_pages_request_flip(int); int dir = g_click_name[10] == 'R' ? 1 : -1; dm_pages_request_flip(dir); snprintf(hit, hn, "个人仓库%s一页（点偏，吸附）", dir > 0 ? "下" : "上"); return 6; }
      if (sn && snap_guarded(root, g_click_name) && !snap_guard_near(dm_wref_get(&g_click), g_click_name, x, y, sx, sy, *(int32_t *)(m->base + 0x211da38), *(int32_t *)(m->base + 0x211da3c))) { snprintf(hit, hn, "吸附到 %s：后果重的按钮只认本体（不足 44 点的外扩到 44 点见方），不点", g_click_name); return 6; }
      if (sn) { snprintf(hit, hn, " 吸附 %s (%d,%d)", g_click_name, sx, sy); bd_note_skill(m, root, sx, sy); return 0; }   // 在可点控件上或它 3.5 毫米内（含面板边上的按钮外沿） | On a clickable widget or within 3.5 mm of it (including the outer rim of buttons at the panel edge)
      // 再外一圈（到 PG_REACH 单位）：周围没有别的可点控件抢这一下（上面的吸附没找到）、又不在物品格上（更前面已判）时也算点箭头——
      // 箭头连同这一圈的有效范围 = 64 + 2 × 112 = 288 单位见方，375 点高的屏上正好 50 点，更高的屏更大
      // One more ring out (up to PG_REACH units): when no other clickable widget around competes for this tap (the snap above found nothing) and it is not on an item cell (already checked further up), it also counts as tapping the arrow --
      // the effective range of the arrow together with this ring = 64 + 2 × 112 = 288 units square, exactly 50 points on a screen 375 points high, and larger on taller screens
      { int dir = pg_personal(root) ? pg_near(root, x, y, *(int32_t *)(m->base + 0x211da3c)) : 0;
        if (dir) { void dm_pages_request_flip(int); dm_pages_request_flip(dir); snprintf(hit, hn, "个人仓库%s一页（点偏，外圈）", dir > 0 ? "下" : "上"); return 6; } } }
    if (ui_inside_panel(m, root, xy)) { snprintf(hit, hn, " %s@%s", g_inside_name, g_inside_owner); atomic_store(&g_close_down, !strcmp(g_inside_name, "TouchClose") ? dm_guest_frame() : 0);
        if ((!strcmp(g_inside_name, "TouchClose") || !strcmp(g_inside_name, "close")) && atomic_load(&g_cube_side_on)) {   // 魔盒并排着：右上角关闭钮只关魔盒（这一下不点下去——点下去是把储藏箱一起关掉） | cube is side by side: the top-right close button closes only the cube (this tap is not pressed down -- pressing it would close the stash along with it)
            static uint64_t lf; uint64_t f = dm_guest_frame(); if (f - lf > 30) { lf = f; atomic_store(&g_cube_side_close, 1); } snprintf(hit, hn, "并排的魔盒：关闭钮只关魔盒"); return 6; }
        return 0; }
    return 3;   // 面板外（不发鼠标点击） | Outside the panel (no mouse click sent)
}
void dm_ui_tab_go(void) {   // 前端在「点按落在页签上」（最近一次命中结果 = 2）时调：按 |差| 次 ZR / ZL（顶部五页签 R / L） | Called by the front end when "the tap landed on a tab" (most recent hit result = 2): press ZR / ZL |delta| times (R / L for the five top tabs)
    DM_LAT("tab_go");
    int d = atomic_load(&g_tab_delta), top = atomic_load(&g_tab_top);
    if (top == 4) { atomic_store(&g_set_back_at, dm_guest_frame() + 2); LOG("界面对话菜单：商人页结果提示 → 按返回键收掉"); return; }
    if (top == 3) { atomic_store(&g_tab_then, DM_BTN_A); atomic_store(&g_tab_mask, d > 0 ? DM_BTN_DOWN : DM_BTN_UP); atomic_store(&g_tab_left, d > 0 ? d : -d); LOG("界面对话菜单：方向键 %+d 行后按 A", d); return; }
    if (!d) return;
    // 切页前把游戏的光标挪到这次点的页签上（发一次 MouseMove）：光标原先停在上一页最后碰过的地方（比如拖放的落点），新页的控件正好出现在那里就会弹它的悬停说明——
    // 物品栏里拖完东西再点「佣兵」页签，佣兵页的技能图标说明框盖住半个面板（固定回归 f 局截图）。页签上没有悬停说明，停在那里最干净
    // 只置一个原子标志、由指针泵（游戏主线程）去发：这里有两个调用方（前端点页签、绑定技能框里切页在游戏主线程），不能往单写者的 g_up_q 里排
    // Before switching pages, move the game's cursor onto the tab just tapped (send one MouseMove): the cursor was resting where the previous page was last touched (for example a drag-and-drop drop point), and if a widget of the new page happens to appear there its hover tooltip pops up --
    // after dragging something in the inventory and then tapping the 「佣兵」(Mercenary) tab, the tooltip of a skill icon on the mercenary page covers half the panel (screenshot from fixed regression run f). Tabs have no hover tooltip, so resting there is cleanest
    // Only one atomic flag is set and the pointer pump (game main thread) does the sending: there are two callers here (the front end tapping a tab, and page switching inside the skill binding box on the game main thread), so it must not be queued into the single-writer g_up_q
    atomic_store(&g_tab_unhover, 1);
    if (atomic_load(&g_tab_have)) { atomic_store(&g_tab_left, 0); atomic_store(&g_tab_direct, 1); LOG("界面页签：切 %+d 页 → 直接选中第 %d 个（%s）", d, g_tab_k, g_tab_name); return; }
    atomic_store(&g_tab_mask, d > 0 ? (top ? DM_BTN_R : DM_BTN_ZR) : (top ? DM_BTN_L : DM_BTN_ZL)); atomic_store(&g_tab_left, d > 0 ? d : -d);
    LOG("界面页签：切 %+d 页（%s）", d, top ? "L / R" : "ZL / ZR");
}
// 单件任务槽面板开着、槽里有东西（= 手上拿着）、而这次按下的不是槽本身：槽里那件之外的物品这时不许动。只许游戏主线程调，在 ui_classify 答出 1 之后用（g_slot = 这次命中的物品区控件）
// The single-item quest socket panel is open and the socket holds something (= held in hand), and this press is not on the socket itself: items other than the one in the socket must not move now. Game main thread only; use after ui_classify answers 1 (g_slot = the item-area widget hit this time)
static int imbue_locked(module_t *m, uint64_t root) {
    uint64_t ip = m && root ? ui_find(root, "ImbueItemsPanel", 0) : 0; if (!ip || !*(uint8_t *)(ip + 0x50) || !has_visible_leaf(ip, 1)) return 0;
    dm_hud_info hi; dm_hud_get(&hi); if (!hi.valid || !hi.holding) return 0;
    return !(dm_wref_get(&g_slot) && *(uint64_t *)dm_wref_get(&g_slot) == (uint64_t)m->base + 0x20898e8);
}
int dm_ui_item_at_now(float nx, float ny) {   // 只许游戏主线程调（Mac 宿主触点脚本在取手柄处跑） | Callable only from the game main thread (the Mac host touch point script runs where the controller is polled)
    module_t *m = main_module(); if (!m) return 0; uint64_t root = 0; memcpy(&root, m->base + 0x2f05458, 8);
    int32_t w = *(int32_t *)(m->base + 0x211da38), h = *(int32_t *)(m->base + 0x211da3c); if (!root || w <= 0 || h <= 0) return 0;
    { int dm_vault_ui_tip_gate(int, int); if ((dm_ui_panel_state() & DM_PS_VAULT) && dm_vault_ui_tip_gate((int)(nx * w), (int)(ny * h))) return 6; }
    { int dm_pouch_tip_gate(int, int); if (dm_pouch_tip_gate((int)(nx * w), (int)(ny * h))) return 6; }   // 百宝囊清单开着：点框外只收清单（dm_pouch.c）   // 同 ui_item_pump：编年史说明框外只收框 | Pouch list open: a tap outside the box only dismisses the list (dm_pouch.c)   // same as ui_item_pump: outside the chronicle tooltip only dismisses the box
    char hit[40]; g_cls_out = UI_OUT(nx, ny); int r = ui_classify(m, root, (int)(nx * w), (int)(ny * h), hit, sizeof hit);
    if (r == 9) return 6;
    if (r == 1 && imbue_locked(m, root)) return 10;
    return r == 1 && g_slot_panel && !strcmp(g_slot_panel, "VendorPanelLayout") && dm_shop_open() ? 7 : r;
}
// 拖放落点怎么处理（物品画在手指上方约 40 点处，玩家有的对着物品放、有的对着手指放——手指对准武器槽松手，抬起点落在槽外的背景上，被当成拖出面板丢到了地上）：
//   1 = 抬起点在物品区，放在那里；4 = 抬起点不在、手指点在物品区，放在手指点；3 = 手指点在面板外，丢弃；0 = 都不是（面板里的空白处），放回原处
// 装备槽的落点一律取槽心：贴着槽边放（手指按在槽心时，抬在上方的落点正好在槽上沿）游戏不收、槽变红。
// 槽心用命中函数向四个方向探边得到（没有现成的屏幕矩形）。
// How the drag-drop drop point is handled (the item is drawn about 40 pt above the finger; some players aim with the item and some with the finger - releasing with the finger on the weapon slot, the lifted point fell on the background outside the slot and was treated as dragging out of the panel, dropping it on the ground):
//   1 = the lifted point is in an item area, put it there; 4 = the lifted point is not but the finger point is in an item area, put it at the finger point; 3 = the finger point is outside the panel, drop; 0 = neither (blank space inside the panel), put back
// The drop point for an equipment slot is always the slot center: placing at the slot edge (with the finger on the slot center, the lifted drop point is right at the slot's top edge) is rejected by the game and the slot turns red.
// The slot center is found by probing the edges in four directions with the hit-test function (there is no ready-made screen rect).
static _Atomic uint64_t g_drop_xy;   // 上一次落点判定给出的放下位置（视口像素 x | y << 32） | Put-down position given by the previous drop-point verdict (viewport pixels x | y << 32)
static void slot_center(uint64_t w, int *x, int *y, int vw, int vh, int cells) {
    uint64_t fn = *(uint64_t *)(*(uint64_t *)w + 0x48); int x0 = *x, x1 = *x, y0 = *y, y1 = *y;
#define SLOT_HIT(px, py) (((int (*)(uint64_t, uint64_t))fn)(w, (uint64_t)(uint32_t)(px) | ((uint64_t)(uint32_t)(py) << 32)) & 1)
    while (x0 > 2 && SLOT_HIT(x0 - 3, *y)) x0 -= 3; while (x1 < vw - 3 && SLOT_HIT(x1 + 3, *y)) x1 += 3;
    while (y0 > 2 && SLOT_HIT(*x, y0 - 3)) y0 -= 3; while (y1 < vh - 3 && SLOT_HIT(*x, y1 + 3)) y1 += 3;
    // 多格的区域（背包 / 箱子 / 魔盒）：取落点所在那一格的格心（格距 112.5 单位）；单格的（装备槽、腰带格）就是整块的中心
    // Multi-cell areas (inventory / stash / cube): take the center of the cell containing the drop point (cell pitch 112.5 units); single-cell ones (equipment slots, belt cells) use the center of the whole block
    double cell = 112.5 * vh / 2160.0; int cols = (int)lround((x1 - x0) / cell), rows = (int)lround((y1 - y0) / cell);
    if (!cells) cols = rows = 1;
    // 多格区域：落点就用物品画着的那个点（游戏以光标为中心放），只收进区域边界内 3 像素。光标现在能精确落位：
    // 取格心时 2×2 的物品正好压在四格交点的一侧，往哪边取整不由玩家看到的位置决定（盾放回箱子压到旁边的头盔上、成了交换）
    // Multi-cell areas: the drop point is simply the point where the item is drawn (the game places it centered on the cursor), only pulled 3 pixels inside the area bounds. The cursor can now land precisely:
    // when taking the cell center, a 2×2 item sits exactly to one side of a four-cell junction, and which way it rounds is not determined by the position the player sees (a shield put back in the stash landed on the helmet next to it and became a swap)
    if (cols > 1) { if (*x < x0 + 3) *x = x0 + 3; if (*x > x1 - 3) *x = x1 - 3; } else *x = (x0 + x1) / 2;
    if (rows > 1) { int r = (int)((*y - y0) / cell); if (r >= rows) r = rows - 1; if (r < 0) r = 0; if (*y < y0 + 3) *y = y0 + 3; if (*y > y1 - 3) *y = y1 - 3;
        g_dc = (drop_cell){ 1, *x, *y, (y1 - y0) / rows, r, rows }; } else { *y = (y0 + y1) / 2; g_dc.valid = 0; }
}
static int vt_hit_walk(uint64_t w, uint64_t xy, int dep, uint64_t vt) {   // 可见控件树里有没有这个类（虚表）的控件被点中 | Whether a widget of this class (vtable) in the visible widget tree is hit
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    if (*(uint64_t *)w == vt) return ((int (*)(uint64_t, uint64_t))*(uint64_t *)(vt + 0x48))(w, xy) & 1;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) if (vt_hit_walk(*(uint64_t *)(ch + 8 * i), xy, dep + 1, vt)) return 1;
    return 0;
}
// 最近一次落点判定为 5 时，落点是不是玩家背包格（物品栏面板的 grid）：货架拖买、成叠格拖出只在这里成交（原先松在装备槽、腰带格、那一叠自己身上也照买 / 照取）
// For the most recent drop-point verdict of 5, whether the drop point is the player's inventory grid (the grid of the inventory panel): shelf drag-to-buy and stack-cell drag-out only complete here (previously a release on an equipment slot, a belt cell, or the stack itself also bought / took)
static _Atomic int g_drop_bag;
int dm_ui_drop_bag(void) { return atomic_load(&g_drop_bag); }
static int g_press_stack;   // 最近一次按下命中的是高级仓库的成叠格（只在游戏主线程读写） | The most recent press hit a stack cell of the advanced stash (read and written only on the game main thread)
static _Atomic int g_vault_drop;   // 最后一次落点查询落在编年史页上时手上那件的 id（否则 0） | id of the held item when the last drop-point query landed on the chronicle page (otherwise 0)
int dm_ui_vault_drop(void) { return atomic_load(&g_vault_drop); }
// 视口像素点在物品栏背包格（PlayerInventoryExpansionLayout 的 grid）上：给出它在格里的位置（格为单位 × 100）与格子的列数、行数（编年史拖回背包用）。
// 边界用格子控件自己的命中函数二分探（同 slot_center），格距 112.5 单位（按 2160 高折算）
// A viewport pixel point on the inventory grid of the inventory panel (the grid of PlayerInventoryExpansionLayout): gives its position within the grid (in cells × 100) and the grid's column and row counts (used for dragging from the chronicle back to the inventory).
// The bounds are binary-searched with the grid widget's own hit-test function (same as slot_center); cell pitch 112.5 units (scaled for a 2160 height)
int dm_ui_bag_point(int x, int y, int *fx100, int *fy100, int *cols, int *rows) {
    module_t *m = main_module(); if (!m) return 0; uint64_t root = 0; memcpy(&root, m->base + 0x2f05458, 8);
    int32_t vw = *(int32_t *)(m->base + 0x211da38), vh = *(int32_t *)(m->base + 0x211da3c); uint64_t pn = root ? ui_panel(root, "PlayerInventoryExpansionLayout") : 0, g = pn ? ui_find(pn, "grid", 0) : 0;
    if (!g || !*(uint8_t *)(g + 0x50) || vw <= 0 || vh <= 0 || x < 0 || y < 0 || x >= vw || y >= vh) return 0;
    uint64_t fn = *(uint64_t *)(*(uint64_t *)g + 0x48);
#define BAG_HIT(px, py) (((int (*)(uint64_t, uint64_t))fn)(g, (uint64_t)(uint32_t)(px) | ((uint64_t)(uint32_t)(py) << 32)) & 1)
    if (!BAG_HIT(x, y)) return 0;
    int a, b, x0, x1, y0, y1;
    for (a = 0, b = x; a < b;) { int mid = (a + b) / 2; if (BAG_HIT(mid, y)) b = mid; else a = mid + 1; } x0 = a;
    for (a = x, b = vw - 1; a < b;) { int mid = (a + b + 1) / 2; if (BAG_HIT(mid, y)) a = mid; else b = mid - 1; } x1 = a;
    for (a = 0, b = y; a < b;) { int mid = (a + b) / 2; if (BAG_HIT(x, mid)) b = mid; else a = mid + 1; } y0 = a;
    for (a = y, b = vh - 1; a < b;) { int mid = (a + b + 1) / 2; if (BAG_HIT(x, mid)) a = mid; else b = mid - 1; } y1 = a;
#undef BAG_HIT
    double cell = 112.5 * vh / 2160.0; *cols = (int)lround((x1 - x0 + 1) / cell); *rows = (int)lround((y1 - y0 + 1) / cell);
    *fx100 = (int)lround((x - x0) * 100.0 / cell); *fy100 = (int)lround((y - y0) * 100.0 / cell); return *cols > 0 && *rows > 0;
}
// 佣兵面板里还没解锁的六个新装备格（项链 2、戒指 6 / 7、腰带 8、鞋 9、手套 10；秘境最高通关不到 10 层）：点中 / 拖到就弹一句提示（dm_rift.c，半秒内不重复）。
// 认命中结果（g_slot = 这次命中的物品区控件）、不认界面焦点：空格旁边有物品时光标会吸到那件物品所在的格上（点腰带格，焦点落在身体格——实测）。佣兵格 = 属主 +0x5c8 == 1
// The six not-yet-unlocked new equipment slots in the mercenary panel (amulet 2, rings 6 / 7, belt 8, boots 9, gloves 10; highest Rift cleared below level 10): tapping / dragging onto one pops a hint (dm_rift.c, no repeat within half a second).
// Matches the hit result (g_slot = the item-area widget hit this time), not UI focus: when an empty slot has an item next to it, the cursor snaps to the slot holding that item (tapping the belt slot puts focus on the body slot, measured). Mercenary slot = owner +0x5c8 == 1
static int merc_lock_hit(const char *hit) {   // 返回 1 = 这次命中的是锁着的格（已弹提示） | Returns 1 = this hit is a locked cell (hint already shown)
    if (!dm_wref_get(&g_slot) || !hit || strncmp(hit, "slot_", 5) || *(uint32_t *)(dm_wref_get(&g_slot) + 0x5c8) != 1) return 0; int l_ = *(int32_t *)(dm_wref_get(&g_slot) + 0x5d8); int dm_rift_merc_unlocked(void); void dm_rift_merc_locked_hint(void);
    if ((l_ == 2 || (l_ >= 6 && l_ <= 10)) && !dm_rift_merc_unlocked()) { dm_rift_merc_locked_hint(); return 1; }
    return 0;
}
static int ui_drop_kind(module_t *m, uint64_t root, int x, int yl, int yf) {
    // 松手时落点一律按物品图的位置（手指上方那个抬起点 yl）判，不看手指点——背包格、装备位、腰带格、箱子、货架、黑网格、面板外都同一口径
    // On release the drop point is always judged by the item image position (the lifted point yl above the finger), not the finger point - inventory cells, equipment slots, belt cells, stash, shelf, black grid and outside the panel all use the same convention
    (void)yf; atomic_store(&g_drop_bag, 0); int32_t vw = *(int32_t *)(m->base + 0x211da38), vh = *(int32_t *)(m->base + 0x211da3c);
    char hit[40]; int px = x, py = yl;
    int c = ui_classify(m, root, x, yl, hit, sizeof hit);
    if (c == 9) return 0;   // 模态框开着：落点一律算放回原处 | Modal open: every drop point counts as put back
    // 编年史页（第六页）开着、松在编年史那块上：放回原处，并记下手上那件——手空了之后由 dm_vault_ui.c 存进编年史仓库（拖进来 = 存入，设计第二节）。
    // 这个函数拖动中也会反复被问，只记「最后一次问的落点在不在编年史上」，松手前最后一问说了算
    // With the chronicle page (page six) open and the release on the chronicle block: put back, and record the held item - once the hand is empty dm_vault_ui.c stores it into the chronicle vault (drag in = deposit, design section 2).
    // This function is also asked repeatedly during the drag, so it only records "whether the drop point of the last query was on the chronicle"; the last query before release decides
    if (dm_ui_panel_state() & DM_PS_VAULT) { int dm_vault_ui_over(int, int); uint32_t dm_vault_ui_held(void); uint32_t id = dm_vault_ui_held(); int v = id && dm_vault_ui_over(x, yl);
        atomic_store(&g_vault_drop, v ? (int)id : 0); if (v) return 0; } else atomic_store(&g_vault_drop, 0);
    // 单件任务槽（灌魔 / 打孔 / 个性化 / 古墓底座共用的 ImbueItemsPanel 里那个 ImbueSocketWidget，按控件类认）：
    // 原版里「物品拿在手上」就等于「在槽里」（拿起后槽里直接显示它），不存在「在槽上按确认放下」——那样槽变红、不发包（宿主读数）。
    // 松手落在槽上答 8：前端按原版走法重做（放回原格，再对它按住 Y = 原版的「放进槽」）；之前这里被判成面板外，手上那件直接丢到了面板后面的地上
    // Single-item quest slot (the ImbueSocketWidget inside the ImbueItemsPanel shared by imbue / socketing / personalize / tomb pedestal, recognized by widget class):
    // in the original game "item in hand" is the same as "in the slot" (after pick-up the slot shows it directly), and there is no "press confirm on the slot to put down" - doing that turns the slot red and sends no packet (host readout).
    // A release landing on the slot answers 8: the front end redoes it the original game's way (put back to the original cell, then hold Y on it = the original game's "put into slot"); before, this was judged as outside the panel and the held item was dropped straight onto the ground behind the panel
    { uint64_t ip = ui_find(root, "ImbueItemsPanel", 0);
      if (ip && *(uint8_t *)(ip + 0x50)) {
          // 槽控件自己的矩形（312×425 单位）比画出来的金框窄一圈：物品图中心落在框内侧边上会漏判。左右上下各再探 70 单位（按 2160 高折算），覆盖到画出来的框
          // The slot widget's own rect (312×425 units) is one ring narrower than the drawn gold frame: an item image centered on the inner edge of the frame would be missed. Probe a further 70 units in each of left/right/up/down (scaled for a 2160 height) to cover the drawn frame
          static const int ox[5] = { 0, -70, 70, 0, 0 }, oy[5] = { 0, 0, 0, -70, 70 };
          for (int k = 0; k < 5 && vh > 0; k++) { int xx = x + ox[k] * vh / 2160, yy = yl + oy[k] * vh / 2160; if (xx < 0 || yy < 0) continue;
              if (vt_hit_walk(ip, (uint64_t)(uint32_t)xx | ((uint64_t)(uint32_t)yy << 32), 0, (uint64_t)m->base + 0x20898e8)) return 8; }
          // 落在这个面板自己的底板上、槽以外：放回原格。不按「面板外 = 丢」——丢下去的东西被面板挡着，玩家看不见（真全屏：偏到槽边 7 点就丢了）
          // Landed on this panel's own backing, outside the slot: put back to the original cell. Not treated as "outside the panel = drop" - the dropped item would be hidden by the panel and invisible to the player (true fullscreen: 7 pt off the slot edge and it was dropped)
          uint64_t bg = ui_find(ip, "Background", 0);
          if (bg && *(uint8_t *)(bg + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)bg + 0x48))(bg, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)yl << 32)) & 1)) return 0; } }
    // 丢弃区：落在面板两侧的黑网格 = 丢。判据：面板框内的空白、但不在任何带物品区的面板自己的矩形里；
    // 物品栏自己那块石板的空白（装备位之间）仍放回原处——那里离装备位太近，容易误丢
    // 商人页开着：松手落在商人面板自己的范围里（货架格以外的底板、页签条、修理钮一带）也算卖——原版是拖到商店面板任意位置都卖出。
    // 货架格本身在下面按物品区答 7；能不能卖仍由游戏判（卖不掉的由前端随后放回原格）
    // Drop zone: landing on the black grid on either side of the panel = drop. Criterion: blank space inside the panel frame, but not inside the own rect of any panel that has an item area;
    // blank space on the inventory panel's own slab (between equipment slots) still puts back - it is too close to the equipment slots and easy to drop by mistake
    // Vendor page open: a release landing within the vendor panel's own extent (the backing outside shelf cells, the tab bar, around the repair button) also counts as sell - in the original game dragging to anywhere on the shop panel sells.
    // The shelf cells themselves answer 7 below as an item area; whether it can be sold is still judged by the game (what cannot be sold is put back to its original cell by the front end afterwards)
    if (c != 1 && dm_shop_open()) { uint64_t vp = ui_find(root, "VendorPanelLayout", 0), xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)yl << 32);
        if (vp && *(uint8_t *)(vp + 0x50)) { static const char *part[3] = { "background", "background_repair", "tabs" };
            for (int k = 0; k < 3; k++) { uint64_t w = ui_find(vp, part[k], 0); if (w && *(uint8_t *)(w + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x48))(w, xy) & 1)) return 7; } } }
    if (c != 1) return c == 3 || (c == 0 && g_inside_owner[0] && !in_item_panel_rect(m, root, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)yl << 32))) ? 3 : 0;
    if (belt_row_block(m, x, yl)) return 0;   // 进不了腰带的物品松手在腰带格上：放回原处 | An item that cannot go into the belt released on a belt cell: put back
    if (g_slot_panel && !strcmp(g_slot_panel, "VendorPanelLayout") && dm_shop_open()) return 7;   // 7 = 落在商人的货架上：松手时卖掉手上那件（前端调 dm_shop_sell_cursor；卖不掉的放回原处） | 7 = landed on the vendor's shelf: on release sell the held item (the front end calls dm_shop_sell_cursor; what cannot be sold is put back)
    if (merc_lock_hit(hit)) { atomic_store(&g_drop_loc, 0); return 0; }   // 松手在锁着的格上：放回原处（同上面腰带那行），不记栏位——不让松手收尾的兜底去给佣兵穿（服务端那道门本来也会拒，这里省掉那一趟） | Released on a locked cell: put back where it was (same as the belt line above), without recording a slot -- so that the release-finish fallback does not go and equip the mercenary (the server-side gate would reject it anyway; this saves that round trip)
    atomic_store(&g_drop_loc, dm_wref_get(&g_slot) && !strncmp(hit, "slot_", 5) ? (*(int32_t *)(dm_wref_get(&g_slot) + 0x5d8) | (*(uint32_t *)(dm_wref_get(&g_slot) + 0x5c8) == 1 ? 0x100 : 0)) : 0);   // 佣兵面板里的同名格（属主 +0x5c8 == 1）带上 0x100：兜底时走「给佣兵穿」，不能按玩家的栏位放（那会把东西穿到玩家身上） | A same-named cell in the mercenary panel (owner +0x5c8 == 1) carries 0x100: the fallback then goes through "equip the mercenary" and must not place by the player's slot (that would put the item on the player)
    if (dm_wref_get(&g_slot)) slot_center(dm_wref_get(&g_slot), &px, &py, vw, vh, strncmp(hit, "slot_", 5) != 0);   // 装备槽取槽心；背包类用图的位置（收进边界内） | Equipment slots take the slot center; inventory-type areas use the image position (pulled inside the bounds)
    atomic_store(&g_drop_xy, (uint64_t)(uint32_t)px | ((uint64_t)(uint32_t)py << 32));
    atomic_store(&g_drop_bag, !strcmp(hit, "grid") && g_slot_panel && !strcmp(g_slot_panel, "PlayerInventoryExpansionLayout"));
    return 5;   // 5 = 放在 g_drop_xy（dm_ui_drop_point 取） | 5 = put down at g_drop_xy (fetched via dm_ui_drop_point)
}
// 松手那一刻这次的落点查询还没答上时用：真机上松手常紧跟在最后一次移动之后（几毫秒），查询要等游戏下一帧才答。
// 上一次移动的落点离松手点只有几个像素，用它；一次都没答过返回 -1（前端按「放回原处」处理）。
// 原先没答上就当「物品区」在落点按放下——落点不在任何格子上时什么都不发生，物品留在手上（箭袋挂在面板右边缘）
// Used when this release's drop-point query has not been answered yet at the moment of release: on a real device the release often follows the last move immediately (a few ms), and the query is not answered until the game's next frame.
// The drop point of the previous move is only a few pixels from the release point, so use it; returns -1 if none was ever answered (the front end handles it as "put back").
// Previously an unanswered query was treated as "item area" and a put-down was pressed at the drop point - when the drop point is on no cell nothing happens and the item stays in hand (the quiver hung at the right edge of the panel)
int dm_ui_drop_last(void) { return atomic_load(&g_drop_last); }
int dm_ui_drop_point(float *nx, float *ny) {   // 最近一次落点判定为 5 时的放下位置（归一化） | Put-down position (normalized) from the most recent drop-point verdict of 5
    module_t *m = main_module(); if (!m) return 0; int32_t vw = *(int32_t *)(m->base + 0x211da38), vh = *(int32_t *)(m->base + 0x211da3c); if (vw <= 0 || vh <= 0) return 0;
    uint64_t v = atomic_load(&g_drop_xy); *nx = (float)((v & 0xffffffff) + 0.5) / vw; *ny = (float)((v >> 32) + 0.5) / vh; return 1;
}
static _Atomic uint32_t g_iq_y2;
unsigned dm_ui_drop_query(float nx, float ny_lifted, float ny_finger) {
    ny_finger = ny_finger < 0 ? 0 : ny_finger > 1 ? 1 : ny_finger; atomic_store(&g_iq_y2, (uint32_t)(ny_finger * 65535 + .5f)); atomic_store(&g_iq_drop, 1);
    return dm_ui_item_query(nx, ny_lifted);
}
int dm_ui_drop_at_now(float nx, float ny_lifted, float ny_finger) {   // 同步版，只许游戏主线程调 | Synchronous version, callable only from the game main thread
    module_t *m = main_module(); if (!m) return 1; uint64_t root = 0; memcpy(&root, m->base + 0x2f05458, 8);
    int32_t w = *(int32_t *)(m->base + 0x211da38), h = *(int32_t *)(m->base + 0x211da3c); if (!root || w <= 0 || h <= 0) return 1;
    g_cls_out = UI_OUT(nx, ny_lifted); return ui_drop_kind(m, root, (int)(nx * w), (int)(ny_lifted * h), (int)(ny_finger * h));
}
static _Atomic uint64_t g_buy_at;   // 位 63 = 有请求；x、y 各 16 位归一化 | Bit 63 = request pending; x and y each 16-bit normalized
void dm_ui_shop_buy_at(float nx, float ny) {
    nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    atomic_store(&g_buy_at, (1ull << 63) | ((uint64_t)(uint32_t)(nx * 65535 + .5f) << 16) | (uint32_t)(ny * 65535 + .5f));
}
// 高级仓库里的一叠拖到背包松手 = 取 1 件：与货架同一条路（起拖时不拿起、光标挪开；松手把光标放回那一叠再取）。位 62 = 这次是取件不是买
// A stack in the advanced stash dragged to the inventory and released = take 1: same path as the shelf (no pick-up at drag start, the cursor moves away; on release the cursor is put back on that stack and then it is taken). Bit 62 = this one is a take, not a buy
void dm_ui_stack_take_at(float nx, float ny) {
    nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    atomic_store(&g_buy_at, (3ull << 62) | ((uint64_t)(uint32_t)(nx * 65535 + .5f) << 16) | (uint32_t)(ny * 65535 + .5f));
}
// 高级仓库里的一叠拖到这一页下方的魔盒格上松手 = 1 件进魔盒：光标放回那一叠，替玩家短按原版的 X（成叠格上「移动到赫拉迪姆魔盒」；能不能进由原版判）。位 61 = 这次是进魔盒
// A stack in the advanced stash dragged onto a cube cell at the bottom of this page and released = 1 item into the cube: put the cursor back on that stack and short-press the original game's X for the player (「移动到赫拉迪姆魔盒」(Move to Horadric Cube) on a stack cell; whether it can go in is judged by the original game). Bit 61 = this one is into the cube
void dm_ui_stack_cube_at(float nx, float ny) {
    nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny;
    atomic_store(&g_buy_at, (7ull << 61) | ((uint64_t)(uint32_t)(nx * 65535 + .5f) << 16) | (uint32_t)(ny * 65535 + .5f));
}
static void ui_buy_pump(int width, int height) {   // 主线程：光标放回货上并让游戏当场重算选中（dm_ui_hover_now），再买选中的那件 | Main thread: put the cursor back on the item and have the game recompute the selection on the spot (dm_ui_hover_now), then buy the selected item
    uint64_t v = atomic_exchange(&g_buy_at, 0); if (!(v >> 63)) return;
    int x = (int)(((v >> 16) & 0xffff) / 65535.0 * width), y = (int)((v & 0xffff) / 65535.0 * height);
    // 光标放不回去、或放回去后选中的不是货架上的货 / 储藏箱里那一叠，就不买不取（原先只打红字照买，买的是当前选中的别的东西）
    // If the cursor cannot be put back, or after putting it back the selection is not a shelf item / that stack in the stash, do not buy or take (it used to just print a failure line and buy anyway, buying whatever else was currently selected)
    int take = v >> 62 & 1; dm_cursor_item_info ci;
    if (!dm_ui_hover_now(x, y)) { LOG("【红】界面拖%s：此刻不是手柄光标模式，光标放不回 (%d,%d) → 不%s", take ? "取" : "买", x, y, take ? "取" : "买"); return; }
    if (!dm_cursor_item(&ci) || ci.container != (take ? DM_ITEM_AT_STASH : DM_ITEM_AT_SHOP)) { LOG("【红】界面拖%s：光标放回 (%d,%d) 后选中的不是%s → 不%s", take ? "取" : "买", x, y, take ? "储藏箱里的一叠" : "货架上的货", take ? "取" : "买"); return; }
    if (v >> 61 & 1) { dm_ui_key_pulse(DM_BTN_X, 6); LOG("界面拖放：一叠松在魔盒格上 → 光标回那一叠（%d,%d），按 X", x, y); }   // 魔盒请求也带「取」位：上面已核过选中的是储藏箱里那一叠 | The cube request also carries the "take" bit: the check above already verified the selection is that stack in the stash
    else if (take) { void dm_stash_take(int); dm_stash_take(1); } else dm_shop_buy();
}
// 鉴定：双击鉴定卷轴 / 书进入「等选目标」状态后，单击任何一件物品 = 鉴定它（一下，不再双击 / 长按）；
// 点空处、点别的地方、点到鉴不了的（已鉴定的）、关面板 = 退出该状态，不耗卷轴。入口都是接的原版路径（dm_item_identify / dm_item_target_cancel）
// Identify: after a double tap on an identify scroll / tome enters the "waiting for target" state, a single tap on any item = identify it (one tap, no more double tap / long press);
// tapping empty space, tapping elsewhere, tapping something that cannot be identified (already identified), or closing the panel = leave that state without consuming a scroll. All entries hook the original game's paths (dm_item_identify / dm_item_target_cancel)
static _Atomic int g_tap_req, g_targeting_pub;
void dm_ui_item_tap(void) { atomic_store(&g_tap_req, 1); }
int dm_ui_targeting(void) { return atomic_load(&g_targeting_pub); }
static _Atomic int g_tap_box;
int dm_ui_tap_box(void) { return atomic_load(&g_tap_box); }
// 仓库开着时，光标此刻指着的若是背包里的物品 = 它的单位 id，否则 0（指着仓库一侧 / 空格 / 没指着）。前端据此决定双击先发 X 再看它还在不在。
// 「能不能进堆叠栏」不在这里判：物品数据 +0x9c 对背包里的散件恒为 0（它是仓库里一叠的数量，本机探针），游戏自己的判定出处没读到——
// 交给游戏：对不能堆叠的物品按 X 原版什么都不做（提示条上也没有 X 这一项），所以发完 X 看物品还在不在就知道
// While the stash is open, if the cursor is currently on an item in the inventory = its unit id, otherwise 0 (on the stash side / an empty cell / on nothing). The front end uses this to decide to send X first on a double tap and then see whether it is still there.
// "Whether it can go into the stacking tab" is not judged here: item data +0x9c is always 0 for loose items in the inventory (it is the count of a stack in the stash, local probe), and where the game's own check lives was not found -
// leave it to the game: pressing X on a non-stackable item does nothing in the original game (the button legend has no X entry either), so after sending X, checking whether the item is still there tells
static _Atomic uint32_t g_cur_stack_id;
// 未鉴定物品双击 = 免费鉴定（只有未鉴定的时候双击是鉴定，各页都先判这一条，已鉴定照原双击规则）。
// 光标此刻指着的是我方（背包 / 箱子 / 方块 / 身上）一件未鉴定物品 → 发布它的 id；触摸层双击时先看它
// Double tap on an unidentified item = free identify (double tap means identify only while unidentified; every page checks this rule first, and identified items follow the usual double-tap rules).
// The cursor is currently on one of our own (inventory / stash / cube / equipped) unidentified items → publish its id; the touch layer checks it first on a double tap
static _Atomic uint32_t g_hover_unid;
uint32_t dm_ui_hover_unid(void) { return atomic_load(&g_hover_unid); }
static _Atomic uint32_t g_ident_req;
void dm_ui_identify_hovered(void) { atomic_store(&g_ident_req, atomic_load(&g_hover_unid)); }
uint32_t dm_ui_cur_stack_id(void) { return atomic_load(&g_cur_stack_id); }
// 这次按下的成叠格里那一叠有几件：按下命中成叠格后，手柄光标落到那一格上时读一次（光标物品在储藏箱里且叠数 > 0）。
// 拖出跟手图据此决定原格压不压暗——还有剩余就不压暗（剩下的那叠看得见），只有 1 件才压暗。-1 = 还没读到（按下就甩走，光标没来得及落上去）
// How many items are in the stack of the stack cell this press hit: read once when, after the press hit a stack cell, the controller cursor lands on that cell (the cursor item is in the stash and stack count > 0).
// The drag-out follow image uses it to decide whether to dim the source cell - if some remain it is not dimmed (the remaining stack is visible), dimmed only with exactly 1. -1 = not read yet (flicked away right after pressing, before the cursor could land)
static _Atomic int g_stack_left = -1, g_stack_want;
int dm_ui_stack_left(void) { return atomic_load(&g_stack_left); }
static void ui_target_pump(module_t *m, uint64_t fr) {   // 主线程，每帧 | Main thread, every frame
    if (atomic_load(&g_stack_want)) { dm_cursor_item_info si; if (dm_cursor_item(&si) && si.container == DM_ITEM_AT_STASH && si.stack > 0) { atomic_store(&g_stack_left, si.stack); atomic_store(&g_stack_want, 0); } }
    int tg = dm_item_targeting() != 0; atomic_store(&g_targeting_pub, tg);
    static uint64_t check_at; int tap = atomic_exchange(&g_tap_req, 0);
    if (tap) { dm_cursor_item_info bi; atomic_store(&g_tap_box, dm_cursor_item(&bi) && !strcmp(bi.code, "box")); }   // 刚单击的这件是不是魔盒（开着箱子时双击魔盒 = 打开，前端据此分流） | Whether the item just tapped is the cube (with the stash open, double tap on the cube = open; the front end branches on this)
    { dm_cursor_item_info ui; int c = dm_cursor_item(&ui) ? ui.container : 0;
      atomic_store(&g_hover_unid, c && ui.unid && (c == DM_ITEM_AT_INVENTORY || c == DM_ITEM_AT_STASH || c == DM_ITEM_AT_CUBE || c == DM_ITEM_AT_EQUIP) ? ui.id : 0);
      // 手柄模式：光标停在未鉴定物品上按住 A 0.5 秒 = 免费鉴定（触屏是双击）。原版 A = 短按捡起、按住使用，未鉴定的装备没有「使用」；
      // 这时接管 A：短按照旧还给原版（替玩家按一下），按住才鉴定。手上拿着东西时不接管（A 是放下 / 交换）。
      // 提示：原版对未鉴定装备的提示条只有「A 捡起」，把它取的那条串换成「捡起（按住鉴定）」（dm_padmode.c 的换串小表，离开这件物品就换回）
      // Controller mode: with the cursor resting on an unidentified item, holding A for 0.5 seconds = free identify (on touch it is a double tap).
      // In the original game A = short press to pick up, hold to use, and unidentified equipment has no "use";
      // here A is taken over: a short press is handed back to the original game as before (pressed once for the player), and only a hold identifies.
      // No takeover while something is held on the cursor (A is put down / swap).
      // Hint: the original button legend for unidentified equipment only has "A Pick Up"; the string it fetches is swapped for
      // "Pick Up (hold to identify)" (the small string swap table in dm_padmode.c; swapped back on leaving this item)
      { int dm_padmode_current(void); uint64_t dm_padmode_page_keys(uint64_t), dm_padmode_page_held(void); void dm_padmode_page_pulse(uint64_t), dm_padmode_ident_hint(int);
        static uint64_t t0; static int fired; uint32_t hu = atomic_load(&g_hover_unid); dm_hud_info hh; dm_hud_get(&hh);
        if (dm_padmode_current() && hu && !(hh.valid && hh.holding)) { uint64_t ev = dm_padmode_page_keys(DM_BTN_A), now = now_ns(); int held = (dm_padmode_page_held() & DM_BTN_A) != 0; dm_padmode_ident_hint(1);
            if (ev & DM_BTN_A) { t0 = now; fired = 0; }
            if (held && t0 && !fired && now - t0 >= 500000000ull) { fired = 1; atomic_store(&g_ident_req, hu); LOG("免费鉴定：手柄按住 A 0.5 秒，光标下未鉴定物品 id %u → 鉴定", hu); }
            if (!held && t0) { if (!fired) { dm_padmode_page_pulse(DM_BTN_A); LOG("免费鉴定：手柄短按 A → 还给原版（捡起）"); } t0 = 0; } }
        else t0 = 0; }
      uint32_t rq = atomic_exchange(&g_ident_req, 0); if (rq) { void dm_item_identify_free(uint32_t); dm_item_identify_free(rq); } }
    if (dm_ui_panel_state() & DM_PS_BANK) { dm_cursor_item_info ci; atomic_store(&g_cur_stack_id, dm_cursor_item(&ci) && ci.container == DM_ITEM_AT_INVENTORY ? ci.id : 0); } else atomic_store(&g_cur_stack_id, 0);   // 刚单击的这件是不是魔盒（开着箱子时双击魔盒 = 打开，前端据此分流） | Whether the item just tapped is the cube (with the stash open, double tap on the cube = open; the front end branches on this)
    if (tap && !tg && dm_vendor_repair_mode()) { dm_cursor_item_info ri; if (dm_cursor_item(&ri)) { dm_item_repair(ri.id); LOG("界面修理：修理态下单击 %s（id %u）→ 修理", ri.code, ri.id); } }   // 商人页修理态（点了修理钮）：单击物品 = 修它；退出仍是再点修理钮（原版） | Vendor page repair mode (the repair button was tapped): single tap on an item = repair it; exiting is still tapping the repair button again (as in the original game)
    if (!tg) { check_at = 0; return; }
    if (!free_cursor_page(m)) { dm_item_target_cancel(); LOG("界面鉴定：面板关了，退出鉴定态"); return; }
    if (check_at && fr >= check_at) { check_at = 0; dm_item_target_cancel(); LOG("界面鉴定：点的那件鉴不了（状态没变），退出鉴定态"); return; }
    if (!tap) return;
    dm_cursor_item_info ci;
    if (dm_cursor_item(&ci)) { dm_item_identify(ci.id); check_at = fr + 24; LOG("界面鉴定：单击 %s（id %u）→ 鉴定", ci.code, ci.id); }
    else { dm_item_target_cancel(); LOG("界面鉴定：点在空格上，退出鉴定态"); }
}
static _Atomic int g_use_req;
void dm_ui_use_item(void) { atomic_store(&g_use_req, 1); }
// 手指下有没有物品（长按 / 双击共用）：光标位置的物品，减去「手指粗」吸附拉过去的（往吸附方向只探四分之一格还够不着的算空格）
// Whether there is an item under the finger (shared by long press / double tap): the item at the cursor position, minus those pulled in by the "finger thickness" snap (if probing just a quarter cell in the snap direction does not reach it, it counts as an empty cell)
static int finger_item(dm_cursor_item_info *ci, const char *who) {
    int has = dm_cursor_item(ci);
    // 手指落在空格上时光标可能被「手指粗」吸附拉到了旁边的物品上（半格内有物品就吸）：箱子快满时每个空格都挨着物品，空格长按永远成了「物品上长按」。
    // 长按时再分一次：往吸附方向只探四分之一格——还够得着那件物品 = 手指贴着它的边（按物品算）；够不着 = 手指在空格中间（按空格算）。探完把光标放回吸附点
    // When the finger lands on an empty cell the cursor may have been pulled onto a neighboring item by the "finger thickness" snap (snaps if an item is within half a cell): when the stash is nearly full every empty cell is next to an item, and a long press on an empty cell always became "long press on an item".
    // Classify once more on long press: probe only a quarter cell in the snap direction - still reaching that item = the finger is against its edge (counts as the item); not reaching = the finger is in the middle of the empty cell (counts as empty). After probing, put the cursor back at the snap point
    if (has && sn_on) { uint64_t g = atomic_load(&g_snap); int tx = g & 0xffff, ty = g >> 16 & 0xffff, px = g >> 32 & 0xffff, py = g >> 48 & 0xffff; dm_cursor_item_info c2;
        int near = dm_ui_hover_now(tx + (px - tx) / 2, ty + (py - ty) / 2) && dm_cursor_item(&c2) && c2.id == ci->id; dm_ui_hover_now(px, py);
        if (!near) has = 0; LOG("界面%s：光标是吸附到 %s 上的（请求 %d,%d → 落点 %d,%d）；四分之一格内%s", who, ci->code, tx, ty, px, py, near ? "够得着 → 按物品算" : "够不着 → 按空格算"); }
    return has;
}
static int belt_widget(uint64_t w) { return w && g_iw_base && *(uint64_t *)(*(uint64_t *)w + 0x28) == g_iw_base + 0x3911d8; }   // 物品栏里的原版腰带控件（消息处理 main+0x3911d8） | The original game's belt widget in the inventory panel (message handler main+0x3911d8)
static void ui_use_pump(void) {   // 主线程 | Main thread
    if (!atomic_exchange(&g_use_req, 0)) return;
    dm_cursor_item_info ci;
    // 物品栏里腰带的空格上双击 = 一键补满（原先是长按，改成双击）；有药的格照旧走下面
    // Double tap on an empty belt cell in the inventory panel = one-tap refill (used to be long press, changed to double tap); cells holding a potion go through the code below as before
    if (belt_widget(dm_wref_get(&g_slot)) && !finger_item(&ci, "双击")) { dm_belt_fill(); LOG("界面双击：腰带空格 → 一键补满"); return; }
    if (!dm_cursor_item(&ci)) { LOG("界面双击：光标下没有物品，不动"); return; }
    if ((dm_ui_panel_state() & DM_PS_VAULT) && ci.container == DM_ITEM_AT_INVENTORY) { void dm_vault_request_deposit(uint32_t); dm_vault_request_deposit(ci.id); LOG("界面双击：编年史页开着，背包的 %s（id %u）→ 存进编年史仓库", ci.code, ci.id); return; }
    if (ci.container == DM_ITEM_AT_SHOP) { dm_shop_buy(); LOG("界面双击：货架上的 %s → 买", ci.code); }
    else if (ci.container == DM_ITEM_AT_EQUIP) { dm_item_unequip(ci.id); LOG("界面双击：身上的 %s（id %u）→ 卸到背包", ci.code, ci.id); }   // 走卸装的直接入口：不等「按住确认键」的 0.6 秒 | Take the direct unequip entry: no waiting for the 0.6 s of "hold the confirm key"
    else if (ci.equippable) { dm_item_equip(ci.id); LOG("界面双击：%s（id %u）能装备 → 装上", ci.code, ci.id); }
    else if (!strcmp(ci.code, "box") && (dm_ui_panel_state() & DM_PS_BANK)) { atomic_store(&g_cube_side_req, 1); LOG("界面双击：储藏箱开着，魔盒（id %u）→ 并排打开（不走原版使用：那条路会先关储藏箱）", ci.id); }
    else { dm_item_use(ci.id); LOG("界面双击：%s（id %u）→ 使用", ci.code, ci.id); }
    DM_LAT("use");
}
static _Atomic int g_long_req;
void dm_ui_long_press(void) { atomic_store(&g_long_req, 1); }
static void ui_long_pump(void) {   // 主线程：按游戏自己的悬停对象取物品（dm_cursor_item），不按坐标反算 | Main thread: get the item from the game's own hover object (dm_cursor_item), not back-computed from coordinates
    if (!atomic_exchange(&g_long_req, 0)) return;
    // 物品栏里的腰带上长按：不动（补满改成腰带空格双击）。在这里先返回，不落到「空格长按 = 整理」
    // Long press on the belt in the inventory panel: nothing happens (refill was changed to a double tap on an empty belt cell). Return here first so it does not fall into "long press on empty cell = sort"
    if (belt_widget(dm_wref_get(&g_slot))) { LOG("界面长按：腰带 → 不动（补满改成空格双击）"); return; }
    dm_cursor_item_info ci; int has = finger_item(&ci, "长按");
    if (!has) {   // 空格上长按 = 整理这一页（原版手柄「按住排序」的同一个终点 dm_inv_sort）。认页：从命中的格子控件沿父链（+0x30）看名字——储藏箱页的箱子格与背包格都叫 grid， | Long press on an empty cell = sort this page (the same endpoint as the original controller's 「按住排序」(Hold to Sort), dm_inv_sort). Identifying the page: from the hit cell widget walk up the parent chain (+0x30) and look at names - the stash cells and inventory cells on the stash page are both called grid,
        // 只能靠上层名字分：带 stash / bank 的 = 储藏箱，带 cube 的 = 方块，其余 = 背包；商人货架、装备位、腰带不整理
        // It can only be told apart by the ancestor's name: containing stash / bank = stash, containing cube = cube, the rest = inventory; the vendor shelf, equipment slots and belt are not sorted
        const char *nm = dm_wref_get(&g_slot) ? (const char *)*(uint64_t *)(dm_wref_get(&g_slot) + 8) : NULL, *pn = g_slot_panel; char chain[160] = ""; int page = -1, grid = nm && (!strcmp(nm, "grid") || !strcmp(nm, "horadriccube_grid") || !strncmp(nm, "advancedstash_", 14) || !strcmp(nm, "basicstash_container"));
        if (grid && !(pn && !strcmp(pn, "VendorPanelLayout"))) { page = 0;
            for (uint64_t w = dm_wref_get(&g_slot); w && strlen(chain) < 120; w = *(uint64_t *)(w + 0x30)) { const char *n = *(uint64_t *)(w + 8) ? (const char *)*(uint64_t *)(w + 8) : "?"; strlcat(chain, n, sizeof chain); strlcat(chain, "<", sizeof chain);
                if (page == 0 && (strcasestr(n, "stash") || strcasestr(n, "bank")) && strcmp(n, "BankExpansionLayout") && strcmp(n, "BankOriginalLayout")) page = 4;
                if (page == 0 && strcasestr(n, "cube")) page = 3; } }
        if (page >= 0) { void dm_inv_sort(int); dm_inv_sort(page); }
        LOG("界面长按：空格（控件链 %s，面板 %s）→ %s", chain[0] ? chain : (nm ? nm : "?"), pn ? pn : "?", page < 0 ? "不动" : page == 0 ? "整理背包" : page == 3 ? "整理方块" : "整理储藏箱");
        return; }
    if (ci.container == DM_ITEM_AT_STASH && ci.stack > 0) { void dm_stash_take(int); dm_stash_take(3); LOG("界面长按：高级仓库里的一叠 %s（%d 件）→ 取 3 件到背包", ci.code, ci.stack); return; }   // 原版手柄「按住 = 移动 3 个」，件数不变 | The original controller's "hold = move 3"; the count is unchanged
    if (!strcmp(ci.code, "box") && ci.container == DM_ITEM_AT_INVENTORY && (dm_ui_panel_state() & DM_PS_BANK)) { atomic_store(&g_cube_side_req, 1); LOG("界面长按：储藏箱开着，魔盒（id %u）→ 并排打开", ci.id); return; }   // 与双击同一个入口（魔盒并排） | the same entry as the double-tap (cube side by side)
    if (ci.container == DM_ITEM_AT_INVENTORY && ci.belt_room) { dm_belt_put(ci.id); LOG("界面长按：背包里的 %s（id %u）→ 腰带", ci.code, ci.id); }
    else if (dm_runeword_long_press(&ci)) { g_fb_nudge = 1; LOG("界面长按：%s → 符文之语清单（光标送停靠点，不挂说明框）", ci.code); }   // 匹配的带孔底材（dm_runeword.c 判）。光标还停在底材上会挂出原版说明框、压住清单第二行：同兜底装上后那条路，下一帧送到屏幕左缘停靠点 | A matching socketed base (judged by dm_runeword.c). With the cursor still on the base the original game's tooltip box would show and cover the second row of the list: same path as after a fallback equip, send it to the parking point at the left edge of the screen on the next frame
    else LOG("界面长按：%s（id %u，位置 %d，可进腰带 %d，腰带有位 %d）不动", ci.code, ci.id, ci.container, ci.beltable, ci.belt_room);
}
static _Atomic uint64_t g_shelf_box, g_shelf_pt, g_shelf_vp; static _Atomic unsigned g_shelf_seq;
// 商人货架上被按住的那件货的屏幕矩形（归一化 x0,y0,x1,y1，整件货占的格子的外接矩形）。seq = dm_ui_item_query 的序号。任意线程可读。
// 返回 0：这次命中不是货架上的货 / 还没答 / 手柄光标还没落到那件货上（选中读数还是上一件的：触点不在算出的矩形里）——下一帧再问。
// Screen rect of the item being pressed on the vendor shelf (normalized x0,y0,x1,y1, the bounding rect of the cells the whole item occupies). seq = the sequence number of dm_ui_item_query. Readable from any thread.
// Returns 0: this hit is not a shelf item / not answered yet / the controller cursor has not yet landed on that item (the selection readout is still the previous item's: the touch point is not inside the computed rect) - ask again next frame.
int dm_ui_item_rect(unsigned seq, float r[4]) {
    if (atomic_load(&g_shelf_seq) != seq) return 0;   // 只认「这次按下命中了货架」时记下的序号：起拖后每次移动都会发落点查询，不能再拿「最近一次查询」来比 | Only the sequence number recorded when "this press hit the shelf" is accepted: after the drag starts every move issues a drop-point query, so comparing against "the most recent query" no longer works
    uint64_t b = atomic_load(&g_shelf_box), p = atomic_load(&g_shelf_pt), v = atomic_load(&g_shelf_vp);
    int x0 = b & 0xffff, y0 = (b >> 16) & 0xffff, x1 = (b >> 32) & 0xffff, y1 = (b >> 48) & 0xffff, px = (int)(uint32_t)p, py = (int)(p >> 32), vw = (int)(uint32_t)v, vh = (int)(v >> 32);
    if (vw <= 0 || vh <= 0 || x1 <= x0 || y1 <= y0) return 0;
    dm_shop_info si; dm_shop_get(&si); if (!si.open || !si.sel_valid || si.sel_gw <= 0 || si.sel_gh <= 0) return 0;
    double cell = 112.5 * vh / 2160.0; int cols = (int)lround((x1 - x0) / cell), rows = (int)lround((y1 - y0) / cell); if (cols < 1 || rows < 1) return 0;
    double cw = (double)(x1 - x0) / cols, chh = (double)(y1 - y0) / rows;
    double rx0 = x0 + si.sel_gx * cw, ry0 = y0 + si.sel_gy * chh, rx1 = rx0 + si.sel_gw * cw, ry1 = ry0 + si.sel_gh * chh;
    if (px < rx0 - 2 || px > rx1 + 2 || py < ry0 - 2 || py > ry1 + 2) return 0;
    r[0] = (float)(rx0 / vw); r[1] = (float)(ry0 / vh); r[2] = (float)(rx1 / vw); r[3] = (float)(ry1 / vh); return 1;
}
// 货架格网（归一化：左上角 x,y 与一格的宽、高）；最近一次按下命中货架时探到的。拖货跟手图在回放层按格网认货用（不等选中态）。
// Shelf grid (normalized: top-left x,y and the width and height of one cell); probed at the most recent press that hit the shelf. Used by the replay layer to identify the item by grid for the dragged-item follow image (without waiting for the selected state).
int dm_ui_shelf_grid(float g[4]) {
    uint64_t b = atomic_load(&g_shelf_box), v = atomic_load(&g_shelf_vp);
    int x0 = b & 0xffff, y0 = (b >> 16) & 0xffff, x1 = (b >> 32) & 0xffff, y1 = (b >> 48) & 0xffff, vw = (int)(uint32_t)v, vh = (int)(v >> 32);
    if (vw <= 0 || vh <= 0 || x1 <= x0 || y1 <= y0) return 0;
    double cell = 112.5 * vh / 2160.0; int cols = (int)lround((x1 - x0) / cell), rows = (int)lround((y1 - y0) / cell); if (cols < 1 || rows < 1) return 0;
    g[0] = (float)x0 / vw; g[1] = (float)y0 / vh; g[2] = (float)(x1 - x0) / cols / vw; g[3] = (float)(y1 - y0) / rows / vh; return 1;
}
static void ui_item_pump(uint64_t root, int width, int height) {
    if (dm_lat_on()) { static unsigned ls; unsigned ds = atomic_load(&g_iq_done); float rr[4]; if (ds != ls && dm_ui_item_rect(ds, rr)) { ls = ds; DM_LAT("shelf_rect %d,%d-%d,%d", (int)(rr[0] * width), (int)(rr[1] * height), (int)(rr[2] * width), (int)(rr[3] * height)); } }
    unsigned seq = atomic_load(&g_iq_seq); if (seq == atomic_load(&g_iq_done)) return;
    uint64_t v = atomic_load(&g_iq_xy); int x = (int)(((v >> 16) & 0xffff) / 65535.0 * width), y = (int)((v & 0xffff) / 65535.0 * height);
    char hit[40] = ""; int r; g_cls_out = atomic_load(&g_iq_out);
    if (atomic_exchange(&g_iq_drop, 0)) { r = ui_drop_kind(main_module(), root, x, y, (int)(atomic_load(&g_iq_y2) / 65535.0 * height)); 
        if (r == 5 && g_press_stack && dm_wref_get(&g_slot) && *(uint64_t *)(dm_wref_get(&g_slot) + 8) && !strcmp((const char *)*(uint64_t *)(dm_wref_get(&g_slot) + 8), "horadriccube_grid")) r = 11;   // 11 = 从成叠格拖出、物品图落在这一页下方的魔盒格上（前端：1 件进魔盒，不是取到背包）。g_slot = 刚才落点分类命中的物品区控件 | 11 = dragged out of a stacked slot with the item image landing on the Horadric Cube slot below this page (front end: 1 item goes into the Cube, not taken to the backpack). g_slot = the item-area widget hit by the drop-point classification just now
        snprintf(hit, sizeof hit, "拖放落点类 %d", r); atomic_store(&g_iq_res, r); atomic_store(&g_drop_last, r); atomic_store(&g_iq_done, seq);
        if (hit_log_ok(hit)) LOG("界面命中：(%d,%d) → %s", x, y, hit); return; }
    { int dm_pouch_tip_gate(int, int); if (dm_pouch_tip_gate(x, y)) { r = 6; atomic_store(&g_iq_res, r); atomic_store(&g_iq_done, seq); return; } }   // 百宝囊清单开着：点框外只收清单（dm_pouch.c） | Pouch list open: a tap outside the box only dismisses the list (dm_pouch.c)
    { int dm_vault_ui_tip_gate(int, int); if ((dm_ui_panel_state() & DM_PS_VAULT) && dm_vault_ui_tip_gate(x, y)) { r = 6; atomic_store(&g_iq_res, r); atomic_store(&g_iq_done, seq); return; } }   // 编年史说明框开着：点框外 = 只收框（dm_vault_ui.c），同点模态框外 | Chronicle tooltip open: a tap outside the box = only dismiss the box (dm_vault_ui.c), same as tapping outside a modal
    r = ui_classify(main_module(), root, x, y, hit, sizeof hit);
    if (r == 9) {   // 点模态框外 = 只取消这一层。数量框（提取 / 存入 / 丢金币）按返回就是「取消」；确认框的返回键是「放弃改动」，不替玩家选，只挡住这一下 | A tap outside a modal = cancel only this layer. For quantity boxes (withdraw / deposit / drop gold) pressing back is simply 「取消」(Cancel); for a confirmation box the back key is 「放弃改动」(Discard Changes), so no choice is made for the player and this tap is just blocked
        if (!strcmp(g_modal_name, "DropGoldModal")) { atomic_store(&g_set_back_at, dm_guest_frame() + 2); LOG("界面命中：(%d,%d) → 模态框外，按返回取消 %s", x, y, g_modal_name); }
        else LOG("界面命中：(%d,%d) → 模态框外（%s），这一下不处理", x, y, g_modal_name);
        r = 6; atomic_store(&g_iq_res, r); atomic_store(&g_iq_done, seq); return; }
    if (r != 1 && r != 7 && dm_item_targeting()) { dm_item_target_cancel(); LOG("界面鉴定：点在物品区之外，退出鉴定态"); }
    if (r == 1) dm_wref_set(&g_down, dm_wref_get(&g_slot));   // 这次按下落在哪个物品区控件上（「拿起」那下算物品原位用） | Which item-area widget this press landed on (used to compute the item's original position on the "pick up" press)
    { dm_hud_info hh; dm_hud_get(&hh); if (r == 0 && hh.valid && hh.holding && g_inside_owner[0] && !in_item_panel_rect(main_module(), root, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32))) r = 3; }   // 手上还拿着物品时点两侧黑网格：按面板外算（手势层 = 丢） | Tapping the black grid on either side while still holding an item: treated as outside the panel (gesture layer = drop)
    if (r == 1 && dm_wref_get(&g_slot) && g_iw_base && *(uint64_t *)dm_wref_get(&g_slot) - g_iw_base == 0x20892f8) { r = 8; atomic_store(&g_exact_xy, (uint64_t)(uint32_t)x | (uint64_t)(uint32_t)y << 32); atomic_store(&g_stack_left, -1); atomic_store(&g_stack_want, 1); }   // 8 = 高级仓库里的成叠格（手势层照物品区处理，只有双击不同：取 1 件，不按住 Y——按住是原版的「移动 3 个」） | 8 = a stack cell in the advanced stash (the gesture layer handles it like the item area, only the double tap differs: take 1, without holding Y - holding is the original game's 「移动 3 个」(Move 3))
    g_press_stack = r == 8;
    if (r == 1 && imbue_locked(main_module(), root)) r = 10;   // 10 = 单件任务槽里有东西时，背包 / 身上的别的物品：手势层照物品区，但不拿起、双击不动（原版此时这些物品上不出「放置 / 丢弃」提示） | 10 = other items in the inventory / on the body while the single-item quest slot holds something: the gesture layer treats it like the item area, but no pick-up and double tap does nothing (the original game shows no 「放置 / 丢弃」(Place / Drop) hint on these items at this time)
    if (r == 1 && g_slot_panel && !strcmp(g_slot_panel, "VendorPanelLayout") && dm_shop_open()) r = 7;   // 7 = 商人货架上的货（手势层照物品区处理，只有起拖不同：拖到背包 = 买） | 7 = an item on the vendor shelf (the gesture layer handles it like the item area, only the drag start differs: dragging to the inventory = buy)
    if ((r == 7 || r == 8) && dm_wref_get(&g_slot)) {   // 货架控件的边界（命中函数向四个方向探），给 dm_ui_item_rect 换算那件货的屏幕矩形；成叠格同样探（探到的就是那一格自己），拖出跟手图按它认图标 | Bounds of the shelf widget (hit-test function probed in four directions), for dm_ui_item_rect to convert into the screen rect of that item; stack cells are probed the same way (what is found is that one cell itself), and the drag-out follow image identifies the icon by it
        uint64_t fn = *(uint64_t *)(*(uint64_t *)dm_wref_get(&g_slot) + 0x48); int x0 = x, x1 = x, y0 = y, y1 = y;
#define SHELF_HIT(px, py) (((int (*)(uint64_t, uint64_t))fn)(dm_wref_get(&g_slot), (uint64_t)(uint32_t)(px) | ((uint64_t)(uint32_t)(py) << 32)) & 1)
        while (x0 > 1 && SHELF_HIT(x0 - 2, y)) x0 -= 2; while (x1 < width - 2 && SHELF_HIT(x1 + 2, y)) x1 += 2;
        while (y0 > 1 && SHELF_HIT(x, y0 - 2)) y0 -= 2; while (y1 < height - 2 && SHELF_HIT(x, y1 + 2)) y1 += 2;
        atomic_store(&g_shelf_box, (uint64_t)(uint16_t)x0 | ((uint64_t)(uint16_t)y0 << 16) | ((uint64_t)(uint16_t)x1 << 32) | ((uint64_t)(uint16_t)y1 << 48));
        atomic_store(&g_shelf_pt, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32) ); atomic_store(&g_shelf_vp, (uint64_t)(uint32_t)width | ((uint64_t)(uint32_t)height << 32)); atomic_store(&g_shelf_seq, seq); }
    // 点佣兵面板里的空格 / 锁着的格（手上没拿东西）：这一下不移光标——原版手柄光标落在空格上会被吸到旁边有物品的格、弹出那件物品的说明框盖住面板。
    // 锁着的格照常弹解锁提示；手上拿着物品时不拦（放东西走松手落点那条）。结果 6 = 手势层不点
    // Tapping an empty cell / a locked cell in the mercenary panel (nothing held in hand): this tap does not move the cursor -- the original game's controller cursor, landing on an empty cell, is snapped to a neighboring cell that has an item and pops up that item's tooltip, covering the panel.
    // Locked cells show the unlock hint as usual; with an item held in hand it is not blocked (placing goes through the release drop point path). Result 6 = the gesture layer does not tap
    if (r == 1 && dm_wref_get(&g_slot) && !strncmp(hit, "slot_", 5) && *(uint32_t *)(dm_wref_get(&g_slot) + 0x5c8) == 1) { dm_hud_info hh; dm_hud_get(&hh); int dm_rift_merc_slot_empty(int);
        if (!(hh.valid && hh.holding) && (merc_lock_hit(hit) || dm_rift_merc_slot_empty(*(int32_t *)(dm_wref_get(&g_slot) + 0x5d8)))) { r = 6; snprintf(hit + strlen(hit), sizeof hit - strlen(hit), "（佣兵空格）"); } }
    atomic_store(&g_iq_res, r); atomic_store(&g_iq_done, seq);
    if (r == 1) merc_lock_hit(hit);
    if (hit_log_ok(hit)) { LOG("界面命中：(%d,%d) → %s%s", x, y, r == 1 ? "物品区 " : r == 10 ? "物品区（单件槽占用，不动）" : r == 8 ? "成叠格 " : r == 7 ? "货架 " : r == 2 ? "" : r == 3 ? "面板外" : r == 6 ? "" : "面板内控件", hit); }
}
static void ui_ptr_pump(module_t *m, uint64_t root, int width, int height) {
    unsigned r = atomic_load(&g_up_r); int pend = r != atomic_load(&g_up_w), unhover = atomic_exchange(&g_tab_unhover, 0);
    if (unhover) { atomic_store(&g_up_xy, atomic_load(&g_iq_xy)); atomic_store(&g_up_hold, 0); }
    if (!atomic_load(&g_up_hold) && !pend && !unhover) return;
    static uint64_t hs[10]; if (!hs[0]) { static const char *nm[10] = { "MouseMove", "", "", "LeftMouseButtonDown", "LeftMouseButtonUp", "RightMouseButtonDown", "RightMouseButtonUp", "MouseScroll", "KeyDown", "KeyUp" }; for (int i = 0; i < 10; i++) hs[i] = ui_hash(nm[i]); }
    uint64_t v = atomic_load(&g_up_xy); int x = (int)(((v >> 16) & 0xffff) / 65535.0 * width), y = (int)((v & 0xffff) / 65535.0 * height); if (x >= width) x = width - 1; if (y >= height) y = height - 1;
    int dm_vault_ui_blocks(int, int), vb = (dm_ui_panel_state() & DM_PS_VAULT) && dm_vault_ui_blocks(x, y);   // 编年史页上（我们的控件除外）：悬停与按键都不发，见 dm_vault_ui.c | On the chronicle page (except our own widgets): neither hover nor key presses are sent, see dm_vault_ui.c
    if (!vb) ui_send_mouse(m, root, hs[0], x, y);
    if (!pend) return;
    int k = g_up_q[r % 32].kind, a = g_up_q[r % 32].arg; atomic_store(&g_up_r, r + 1);
    if (k == DM_UP_END) atomic_store(&g_up_hold, 0); else if (k >= DM_UP_LDOWN && k <= DM_UP_KEYUP && !vb) { ui_send_mouse_arg(m, root, hs[k], x, y, a); LOG("界面指针：事件 %d 参数 %d @(%d,%d)", k, a, x, y); }
}
// ---------- 界面连续手势：上下拖 = 滚动、按住滑块横拖 = 值跟手 ----------
// 读码 + 实测（Mac 宿主，选项页）：
//   滚动：列表认键鼠版滚轮——InputMessage:MouseScroll，光标落在可滚区域上才生效（滚动控件自己的命中函数判，main+0xad4d04），滚轮量整除 120（main+0xad4d20），
//         一格在 900 线高的画面上约 32 像素（三分之二行）。所以不用知道任何面板的几何：在起手位置发滚轮，不可滚的地方游戏自己不理。
//   滑块：不认「按住拖」（按下 → 移动 → 抬起，值不变）；认「滑槽上按下」——LeftMouseButtonDown 那一刻值直接跳到按下的位置。两端箭头每点走 1。
//         所以值跟手 = 手指移动时在「夹在滑槽范围内的手指 x、滑块那一行的 y」重复发 按下 / 抬起。只有起手落在滑块上才做（否则横扫会点到一路上的开关 / 箭头）。
//         滑块控件 = 虚表 main+0x20bad70；起手命中与滑槽两端都用控件自己的命中函数（虚表 +0x48）探，不读矩形字段；命中区 = 滑槽（不含两端箭头）。
// 前端线程只写下面几个原子量；控件树与消息只在主线程（ui_drag_pump，每出图帧一次）碰。
// ---------- Continuous UI gestures: vertical drag = scroll, hold a slider and drag horizontally = value follows the finger ----------
// From reading the code + measurement (Mac host, options page):
//   Scrolling: lists respond to the keyboard-mouse wheel - InputMessage:MouseScroll, effective only when the cursor is over a scrollable area (judged by the scroll widget's own hit-test function, main+0xad4d04), with the wheel amount integer-divided by 120 (main+0xad4d20);
//         one notch is about 32 pixels on a 900-line-high screen (two thirds of a row). So no panel geometry needs to be known: send the wheel at the starting position, and the game itself ignores it where nothing scrolls.
//   Sliders: "hold and drag" is not recognized (press → move → release, the value does not change); "press on the track" is - at the moment of LeftMouseButtonDown the value jumps straight to the pressed position. The arrows at both ends move 1 per tap.
//         So value-follows-finger = as the finger moves, repeatedly send press / release at "the finger x clamped to the track range, the y of the slider's row". Done only when the gesture starts on a slider (otherwise a horizontal swipe would tap the toggles / arrows along the way).
//         Slider widget = vtable main+0x20bad70; both the starting hit and the two ends of the track are probed with the widget's own hit-test function (vtable +0x48), without reading rect fields; hit area = the track (excluding the arrows at both ends).
// The front-end thread only writes the few atomics below; the widget tree and messages are touched only on the main thread (ui_drag_pump, once per presented frame).
#define UI_SCROLL_STEP(h) ((h) * 32 / 900 > 4 ? (h) * 32 / 900 : 4)   // 一格滚轮对应手指走多少像素 | How many pixels of finger travel correspond to one wheel notch
static _Atomic int g_dg_kind; static _Atomic uint64_t g_dg_o, g_dg_c; static _Atomic unsigned g_dg_seq; static _Atomic uint32_t g_dg_tol;
static uint64_t dg_pack(float nx, float ny) { nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny; return ((uint64_t)(uint32_t)(nx * 65535 + .5f) << 16) | (uint32_t)(ny * 65535 + .5f); }
void dm_ui_drag_begin(int kind, float ox, float oy, float tol_ny) {
    uint64_t o = dg_pack(ox, oy); atomic_store(&g_dg_o, o); atomic_store(&g_dg_c, o); atomic_store(&g_dg_tol, (uint32_t)((tol_ny < 0 ? 0 : tol_ny > 0.5f ? 0.5f : tol_ny) * 65535));
    atomic_store(&g_dg_kind, kind == 2 ? 2 : 1); atomic_fetch_add(&g_dg_seq, 1);
}
void dm_ui_drag_move(float x, float y) { if (atomic_load(&g_dg_kind)) atomic_store(&g_dg_c, dg_pack(x, y)); }
void dm_ui_drag_end(void) { atomic_store(&g_dg_kind, 0); }
static _Atomic uint32_t g_dg_lift;   // 前端报的物品图抬起量（屏高的比例 × 65535）：iOS 40 点 × 控件缩放、Mac 触点脚本 40 / 1320，同各自拖放落点 | The item image lift reported by the front end (fraction of screen height × 65535): iOS 40 pt × widget scale, Mac touch point script 40 / 1320, same as each one's drag-drop drop point
void dm_ui_drag_lift(float ny) { atomic_store(&g_dg_lift, (uint32_t)((ny < 0 ? 0 : ny > 0.5f ? 0.5f : ny) * 65535)); }
float dm_ui_drag_lift_get(void) { return atomic_load(&g_dg_lift) / 65535.0f; }
static int dg_hit(uint64_t w, int x, int y) { uint64_t fn = *(uint64_t *)(*(uint64_t *)w + 0x48); return ((int (*)(uint64_t, uint64_t))fn)(w, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32)) & 1; }
static void dg_sliders(uint64_t w, uint64_t vt, int dep, uint64_t *out, int *n) {   // 收集可见的滑块控件（整条父链都可见） | Collect visible slider widgets (the whole parent chain visible)
    if (!w || dep > 16 || *n >= 48 || !*(uint8_t *)(w + 0x50)) return;
    if (*(uint64_t *)w == vt) { out[(*n)++] = w; return; }
    uint64_t ch = *(uint64_t *)(w + 0x58), k = *(uint64_t *)(w + 0x60); if (!ch || k > 256) return;
    for (uint64_t i = 0; i < k; i++) dg_sliders(*(uint64_t *)(ch + 8 * i), vt, dep + 1, out, n);
}
static void ui_drag_pump(module_t *m, uint64_t root, int W, int H) {
    static unsigned seen; static int kind, sent, s_ok, s_y, s_l, s_r, s_last, s_down; static uint64_t h_mv, h_dn, h_up, h_sc;
    if (!h_mv) { h_mv = ui_hash("MouseMove"); h_dn = ui_hash("LeftMouseButtonDown"); h_up = ui_hash("LeftMouseButtonUp"); h_sc = ui_hash("MouseScroll"); }
    unsigned seq = atomic_load(&g_dg_seq); int live = atomic_load(&g_dg_kind);
    uint64_t o = atomic_load(&g_dg_o), c = atomic_load(&g_dg_c);
    int ox = (int)(((o >> 16) & 0xffff) / 65535.0 * (W - 1)), oy = (int)((o & 0xffff) / 65535.0 * (H - 1)), cx = (int)(((c >> 16) & 0xffff) / 65535.0 * (W - 1)), cy = (int)((c & 0xffff) / 65535.0 * (H - 1));
    if (kind == 2 && s_down) { ui_send_mouse(m, root, h_mv, s_last, s_y); ui_send_mouse(m, root, h_up, s_last, s_y); s_down = 0; return; }   // 上一帧按下的，这一帧抬起 | Pressed on the previous frame, released on this frame
    if (seq != seen) {   // 新的一次拖动 | A new drag
        seen = seq; kind = live; sent = 0; s_ok = 0; s_down = 0; s_last = -100000;
        { int dm_vault_ui_drag(int, int, int), dm_vault_ui_over(int, int); if ((dm_ui_panel_state() & DM_PS_VAULT) && dm_vault_ui_drag(0, ox, oy)) { kind = 3; return; }   // 编年史格子上起拖 = 往背包取（dm_vault_ui.c） | A drag started on a chronicle cell = take into the inventory (dm_vault_ui.c)
          if ((dm_ui_panel_state() & DM_PS_VAULT) && dm_vault_ui_over(ox, oy)) { kind = 0; return; } }   // 编年史页上别处起拖：什么都不发（不发滚轮 / 滑块点击给底下的原版魔盒容器，独立页面） | Drag started elsewhere on the chronicle page: send nothing (no wheel / slider clicks to the original game's cube container underneath; independent page)
        if (kind == 2) {
            static uint64_t sl[48]; int n = 0; dg_sliders(root, (uint64_t)m->base + 0x20bad70, 0, sl, &n);
            int tol = (int)(atomic_load(&g_dg_tol) / 65535.0 * H), q = H / 180 > 1 ? H / 180 : 1; uint64_t w = 0; int hy = oy, hx = ox;
            for (int d = 0; d <= tol && !w; d += q) for (int sg = 0; sg < (d ? 2 : 1) && !w; sg++) { int y = oy + (sg ? d : -d); if (y < 0 || y >= H) continue;   // 由近到远：先起手那一行，再上下各放宽（手指粗细） | From near to far: the starting row first, then widened up and down (finger thickness)
                for (int e = 0; e < 5 && !w; e++) { int x = ox + (e == 1 ? -tol / 2 : e == 2 ? tol / 2 : e == 3 ? -tol : e == 4 ? tol : 0); if (x < 0 || x >= W) continue;   // 左右也放宽：起手压在滑槽端头外一点也算 | Also widened left and right: starting slightly outside the end of the slider track still counts
                    for (int i = 0; i < n; i++) if (dg_hit(sl[i], x, y)) { w = sl[i]; hy = y; hx = x; break; } } }
            if (w) {   // 沿命中的那一行 / 那一列二分探出控件的左右、上下边 | Binary-search along the hit row / column to find the widget's left/right and top/bottom edges
                int a, b, top, bot;
                for (a = 0, b = hx; a < b;) { int mid = (a + b) / 2; if (dg_hit(w, mid, hy)) b = mid; else a = mid + 1; } s_l = a;
                for (a = hx, b = W - 1; a < b;) { int mid = (a + b + 1) / 2; if (dg_hit(w, mid, hy)) a = mid; else b = mid - 1; } s_r = a;
                for (a = 0, b = hy; a < b;) { int mid = (a + b) / 2; if (dg_hit(w, hx, mid)) b = mid; else a = mid + 1; } top = a;
                for (a = hy, b = H - 1; a < b;) { int mid = (a + b + 1) / 2; if (dg_hit(w, hx, mid)) a = mid; else b = mid - 1; } bot = a;
                int hh = bot - top + 1; s_y = (top + bot) / 2; s_ok = s_r - s_l > 16;   // 命中区就是滑槽本身（两端箭头是它外面的子控件，实测音效滑块命中区 917–1148、箭头在 898 / 1165） | The hit area is the slider track itself (the arrows at both ends are child widgets outside it; measured for the sound-effects slider: hit area 917–1148, arrows at 898 / 1165)
                LOG("界面拖动：起手 (%d,%d) 命中滑块 %s，行 y=%d 高 %d，滑槽 x %d–%d%s", ox, oy, *(uint64_t *)(w + 8) ? (const char *)*(uint64_t *)(w + 8) : "?", s_y, hh, s_l, s_r, s_ok ? "" : "（太窄，不做）");
            } else LOG("界面拖动：横向拖的起手 (%d,%d) 不在滑块上（可见滑块 %d 个，上下放宽 %d 像素），不发任何输入", ox, oy, n, tol);
        } else if (kind == 1) { static int once; if (!once++) LOG("界面拖动：首例纵向拖 → 在起手位置 (%d,%d) 发滚轮，每 %d 像素一格", ox, oy, UI_SCROLL_STEP(H)); }
    }
    if (!kind) return;
    if (kind == 3) { int dm_vault_ui_drag(int, int, int); if (live) dm_vault_ui_drag(1, cx, cy); else { dm_vault_ui_drag(2, cx, cy); kind = 0; } return; }
    if (kind == 1) {
        if (!live) { kind = 0; return; }   // 松手即停 | Stops as soon as the finger lifts
        int want = (cy - oy) / UI_SCROLL_STEP(H), d = want - sent; if (!d) return;   // 手指往下 = 内容跟着往下 = 滚轮正 | Finger moves down = content follows down = positive wheel
        if (d > 3) d = 3; if (d < -3) d = -3;
        ui_send_mouse(m, root, h_mv, ox, oy); ui_send_mouse_arg(m, root, h_sc, ox, oy, d * 120); sent += d; return;
    }
    if (!s_ok) { if (!live) kind = 0; return; }
    int x = cx < s_l ? s_l : cx > s_r ? s_r : cx;
    if (x != s_last && (x - s_last >= 2 || s_last - x >= 2 || !live || x == s_l || x == s_r)) { ui_send_mouse(m, root, h_mv, x, s_y); ui_send_mouse(m, root, h_dn, x, s_y); s_last = x; s_down = 1; return; }   // 按下：值跳到手指下面；下一帧抬起 | Press: the value jumps under the finger; release on the next frame
    if (!live) kind = 0;   // 最后的位置已发过 | The last position has already been sent
}
static void ui_auto_key(uint64_t mask);
static uint64_t ui_find(uint64_t w, const char *name, int dep);
// ---------- 技能绑定的触摸流程 ----------
// 原版手柄流程：技能树（SkillsTreePanel）里光标停在一个技能上按 Y → 弹「重新绑定技能」框（ButtonBindingModal，两排六格 A B Y X R ZR，第二排 = 按住 ZL 的那组）；
// 在框里「直接按要绑的那个键」就绑上并关框（不是移光标选格：按方向键没反应，按 A 即绑到 A 位）。
// 触摸流程把顺序反过来——先定位（点空技能位的十字 / 长按技能钮），再选技能：
//   1 开技能树：按 +（功能页）后看 SkillsTreePanel 开没开，没开就按 R 翻页，直到它开（最多 8 次）；
//   2 等玩家点技能：前端把这期间的点按改成「手柄光标走到触点 + 按 Y」；看到 ButtonBindingModal 开了进下一步；玩家自己关了技能树 = 取消；
//   3 框开稳后按目标键（第二组带 ZL），等框关；再按 B 关功能页回到游戏。
// 面板开没开看控件树（名字 + 可见位），每 6 帧看一次；每步有帧数上限，超时放弃并打日志。
// ---------- Touch flow for skill binding ----------
// The original game's controller flow: in the skill tree (SkillsTreePanel), with the cursor on a skill, press Y → the 「重新绑定技能」(Press button to rebind skill) box pops up (ButtonBindingModal, two rows of six cells A B Y X R ZR, second row = the set with ZL held);
// in the box, "directly pressing the key to bind" binds it and closes the box (it is not moving a cursor to pick a cell: the D-pad does nothing, and pressing A binds to the A slot).
// The touch flow reverses the order - pick the slot first (tap the cross on an empty skill slot / long-press a skill button), then pick the skill:
//   1 Open the skill tree: after pressing + (function page), check whether SkillsTreePanel is open; if not, press R to flip pages until it is (at most 8 times);
//   2 Wait for the player to tap a skill: the front end turns taps during this time into "move the controller cursor to the touch point + press Y"; once ButtonBindingModal is seen open, go to the next step; the player closing the skill tree themselves = cancel;
//   3 Once the box has opened and settled, press the target key (with ZL for the second set) and wait for the box to close; then press B to close the function page and return to the game.
// Whether a panel is open is read from the widget tree (name + visible flag), checked every 6 frames; each step has a frame limit, and on timeout it gives up and logs.
static _Atomic uint64_t g_bd_key; static _Atomic int g_bd_state;   // 0 无 1 开技能树 5 找页 2 等点技能 3 框里按键 4 关面板 | 0 none 1 open skill tree 5 find page 2 wait for skill tap 3 press key in box 4 close panel
static _Atomic int g_bd_stay;
void dm_ui_bind_begin(uint64_t key_mask) { if (!key_mask) return; atomic_store(&g_bd_key, key_mask); atomic_store(&g_bd_stay, 0); atomic_store(&g_bd_state, 1); }
int dm_ui_bind_active(void) { int s = atomic_load(&g_bd_state); return s == 0 ? 0 : (s == 1 || s == 5) ? 1 : s == 2 ? 2 : 3; }
void dm_ui_bind_cancel(void) { atomic_store(&g_bd_state, 0); }
// 目标位序号 1..8（技能钮 B / X / Y / R = 1..4，第二组 +4；与游戏技能钮上的 F1–F8 一致），不在绑定态 = 0。手机层画提示字用
// Target slot index 1..8 (skill buttons B / X / Y / R = 1..4, second set +4; matches F1–F8 on the game's skill buttons); not in the binding state = 0. Used by the phone layer to draw the hint text
int dm_ui_bind_slot(void) { if (!atomic_load(&g_bd_state)) return 0; uint64_t k = atomic_load(&g_bd_key); int s = (k & DM_BTN_B) ? 1 : (k & DM_BTN_X) ? 2 : (k & DM_BTN_Y) ? 3 : (k & DM_BTN_R) ? 4 : 0; return s ? s + ((k & DM_BTN_ZL) ? 4 : 0) : 0; }
// 开技能树那一步要松开的键：第二组技能在游戏态一直按着 ZL（手势层合成的修饰键），按着它时 + 不开功能页
// Keys to release at the open-skill-tree step: the second skill set keeps ZL held in the game state (a modifier synthesized by the gesture layer), and while it is held + does not open the function page
uint64_t dm_ui_auto_release(void) { int s = atomic_load(&g_bd_state); return s == 1 || s == 5 || s == 4 ? DM_BTN_ZL : 0; }   // 4 = 绑完关功能页：第二组时玩家那边 ZL 一直按着，带着 ZL 的返回键游戏不认，要等 30 帧后的重按才关（真机 0.58 秒） | 4 = close the function page after binding: with the second set ZL is held on the player's side, the game does not accept the back key with ZL held, and it only closes on the re-press 30 frames later (0.58 s on a real device)
static int bd_open(uint64_t root, const char *name) { uint64_t w = ui_find(root, name, 0); return w && *(uint8_t *)(w + 0x50); }
// 进技能树时停在上次看的分类页；新角色只有一两个技能，开在空页玩家会以为没东西可选。
// 进来时切到「第一个有已学技能的分类页」，都没有就去最后一页（综合技能：普通攻击等，人人都有）。
// 读法（控件树实测）：SkillsTreePanel 的子控件 Tab0..3 各带 SkillButtonContainer > SkillButton… > Counter（等级数字，文本在 +0xa0，没学 = 空）；
// TextTab0..3 是页签文字，当前页的字色 +0xbc = 240、其余 125。切页用游戏自己的 ZL / ZR（同 dm_ui_tab_go）。
// 没显示过的页等级数字是空的（控件到显示时才填），所以只能看当前页：先回第 0 页，当前页没有已学技能就往后翻一页再看（状态 5）。
// Entering the skill tree lands on the category page last viewed; a new character has only one or two skills, and opening on an empty page makes the player think there is nothing to choose.
// On entry, switch to "the first category page with a learned skill"; if none, go to the last page (general skills: normal attack etc., which everyone has).
// How it is read (measured from the widget tree): SkillsTreePanel's child widgets Tab0..3 each have SkillButtonContainer > SkillButton... > Counter (the level number, text at +0xa0, not learned = empty);
// TextTab0..3 are the tab texts; the current page's text color +0xbc = 240, the others 125. Pages are switched with the game's own ZL / ZR (same as dm_ui_tab_go).
// Pages never shown have empty level numbers (the widgets are filled only when shown), so only the current page can be checked: go back to page 0 first, and if the current page has no learned skill, flip forward one page and check again (state 5).
static uint64_t bd_child(uint64_t w, const char *name) {
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t c = *(uint64_t *)(ch + 8 * i); const char *cn = c ? (const char *)*(uint64_t *)(c + 8) : 0; if (cn && !strcmp(cn, name)) return c; }
    return 0;
}
static int bd_tab_learned(uint64_t tab) {
    uint64_t box = bd_child(tab, "SkillButtonContainer"); if (!box) return 0;
    uint64_t ch = *(uint64_t *)(box + 0x58), n = *(uint64_t *)(box + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t c = *(uint64_t *)(ch + 8 * i), cnt = c ? bd_child(c, "Counter") : 0; uint8_t t = cnt ? *(uint8_t *)(cnt + 0xa0) : 0; if (t >= '1' && t <= '9') return 1; }
    return 0;
}
static int bd_tab_cur(uint64_t tree) { char nm[16]; int32_t col[4]; int have[4] = {0}, n = 0;
    for (int k = 0; k < 4; k++) { snprintf(nm, sizeof nm, "TextTab%d", k); uint64_t t = bd_child(tree, nm); if (t) { have[k] = 1; col[k] = *(int32_t *)(t + 0xbc); n++; if (col[k] == 240) return k; } }
    // 当前页的文字色原先写死认 240；对话菜单上量到同一类颜色值在不同局里会是 240 或 255，不能当常量——认不出时退到「与众不同的那个」（技能树页签至少三个）
    // The current page's text color used to be hard-coded as 240; on the dialog menu the same kind of color value was measured as 240 or 255 in different runs, so it cannot be a constant - when unrecognizable, fall back to "the one that differs from the rest" (the skill tree has at least three tabs)
    for (int k = 0; k < 4 && n >= 3; k++) { if (!have[k]) continue; int same = 0; for (int j = 0; j < 4; j++) same += have[j] && col[j] == col[k]; if (same == 1) return k; }
    return -1; }
// 副手组技能：技能树「综合技能」页、「物品技能」标题栏右端插了原版物品栏的 I / II 页签（touch_layout.py：TouchSwapTab0 / 1 底图两帧、TouchSwapI / II 字）。
// 只在综合技能页显示；底图按当前武器组换帧。点它 = 真换武器（dm_actions.c 调游戏自己的换武器函数），列表由游戏自己刷成那一组的物品技能；关技能树时切回打开前那组
// Off-hand set skills: on the skill tree's 「综合技能」(General Skills) page, the original game's inventory I / II tabs are inserted at the right end of the 「物品技能」(Item Skills) title bar (touch_layout.py: TouchSwapTab0 / 1 two-frame background, TouchSwapI / II text).
// Shown only on the general skills page; the background frame follows the current weapon set. Tapping it = a real weapon swap (dm_actions.c calls the game's own weapon swap function), and the game itself refreshes the list to that set's item skills; when the skill tree closes it switches back to the set from before opening
static _Atomic int g_swaptab_open;
int dm_ui_swaptab_take(void) { return atomic_exchange(&g_swaptab_tap, 0); }
int dm_ui_skilltree_open(void) { return atomic_load(&g_swaptab_open); }
static uint64_t swaptab_w(uint64_t root, int k) { static const char *nm[4] = { "TouchSwapTab0", "TouchSwapTab1", "TouchSwapI", "TouchSwapII" }; uint64_t tw = ui_panel(root, "SkillsTreePanel"); return tw ? ui_find(tw, nm[k], 0) : 0; }
static void swaptab_pump(uint64_t root) {
    uint64_t tw = ui_panel(root, "SkillsTreePanel"); int open = tw && *(uint8_t *)(tw + 0x50), show = open && bd_tab_cur(tw) == 3, w = dm_swapskill_wset();
    atomic_store(&g_swaptab_open, open);
    // 物品给的技能变了（换了武器组，客户端技能链表收到新节点；比服务端组号晚一两拍，按组号变化刷会刷成旧的，本机实测）：原版只在切到这一页时建物品技能列表，
    // 原版只在技能树这一功能页打开时建物品技能列表（页内发「切到第 3 / 4 页」都不重建，本机 实测），所以把功能页切走再切回、再选回综合技能页
    // The skills granted by items changed (weapon set swapped, the client skill linked list received new nodes; this is a tick or two later than the server-side set number, and refreshing on the set-number change refreshes to the old ones, measured on the local machine): the original game only builds the item skill list when switching to this page,
    // the original game only builds the item skill list when the skill tree function page opens (sending "switch to page 3 / 4" within the page does not rebuild either, measured on the local machine), so switch the function page away and back, then reselect the general skills page
    static uint32_t lastsig; static int had; uint32_t sig = dm_swapskill_itemsig(); if (show && had && sig != lastsig) { module_t *sm = main_module(); uint64_t us = ui_find(root, "UISwitcher", 0);
        if (sm && us && *(uint8_t *)(us + 0x50) && *(uint64_t *)(*(uint64_t *)us + 0x28) == (uint64_t)sm->base + 0x404fd8) { int cur = *(int32_t *)(us + 0x1b8);
            ((void (*)(uint64_t, int, int))(sm->base + 0x405898))(us, cur - 1, 0); ((void (*)(uint64_t, int, int))(sm->base + 0x405898))(us, cur, 1);   // 功能页切走再切回（同点页签直切那条，main+0x405898），技能树重开时按新武器组建列表 | Switch the function page away and back (same path as the direct switch on tapping a tab, main+0x405898); when the skill tree reopens it builds the list for the new weapon set
            fe_send("SkillsTreePanelMessage", "ActivateTab", "3"); LOG("副手组技能：武器组的物品技能变了 → 技能树重开到综合技能页"); } }
    lastsig = sig; had = show; (void)w;
    for (int k = 0; k < 4 && tw; k++) { uint64_t x = swaptab_w(root, k); if (!x) continue; int v = show && (k >= 2 || (w < 0 ? k == 0 : k == w)); ui_show_ours(x, v); }
}
static int swaptab_hit(uint64_t root, int x, int y) {
    for (int k = 0; k < 2; k++) { uint64_t t = swaptab_w(root, k); if (t && *(uint8_t *)(t + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)t + 0x48))(t, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32)) & 1)) return 1; }
    return 0;
}
static void bd_tab_step(int d) { atomic_store(&g_tab_delta, d); atomic_store(&g_tab_top, 0); dm_wref_set(&g_tab_ref, 0); atomic_store(&g_tab_have, 0); dm_ui_tab_go(); }   // 绑定流程按差值走：不用直接切（g_tab_w 清掉） | The binding flow steps by delta: no direct switch (g_tab_w cleared)
// 技能树里长按技能 → 游戏自己的「重新绑定技能」框（ButtonBindingModal）：两排六格，格子只认手柄键、不认点击。
// 触摸下点格子 = 替玩家按那一格的键：按画面上从左到右的列号认，第 1..5 列 = 手柄 A / B / Y / X / R（A = 主技能 / 交互大钮；第二排先按住 ZL）。
// 第 3 列是 Y、第 4 列是 X，不是字母顺序：mini 看绑定框前后截图量的——按 X 技能落进第 4 列、按 Y 落进第 3 列（两排都是）；
// 原先按 A / B / X / Y / R 排，点第 3 列技能落到第 4 列。格子名字里的序号（SkillButton1..5）就是画面列号，不带键的信息。
// 第 6 列（ZR = 回城快捷键）布局里已挪走。返回 列号 + (第二排 ? 5 : 0)，即 1..10，没点中格子 0。
// Long press on a skill in the skill tree → the game's own 「重新绑定技能」(Press button to rebind skill) box (ButtonBindingModal): two rows of six cells; the cells only respond to controller keys, not taps.
// Under touch, tapping a cell = pressing that cell's key for the player: recognized by on-screen column number from left to right, columns 1..5 = controller A / B / Y / X / R (A = the big main skill / interact button; for the second row hold ZL first).
// Column 3 is Y and column 4 is X, not alphabetical order: measured on the second Mac from screenshots before and after the binding box - pressing X puts the skill in column 4, pressing Y in column 3 (both rows);
// it used to be ordered A / B / X / Y / R, and tapping column 3 put the skill in column 4. The index in a cell's name (SkillButton1..5) is just the on-screen column number and carries no key information.
// Column 6 (ZR = the town portal shortcut) has already been moved away in the layout. Returns column number + (second row ? 5 : 0), i.e. 1..10; 0 if no cell was hit.
static int bd_cell_row(const char *nm) { return !nm ? 0 : !strncmp(nm, "AltSkillButton", 14) ? 2 : !strncmp(nm, "SkillButton", 11) ? 1 : 0; }
static int bd_modal_walk(uint64_t w, uint64_t xy, int dep) {
    if (!w || dep > 8 || !*(uint8_t *)(w + 0x50)) return 0;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t c = *(uint64_t *)(ch + 8 * i); if (!c || !*(uint8_t *)(c + 0x50)) continue; int row = bd_cell_row((const char *)*(uint64_t *)(c + 8));
        if (!row) { int r = bd_modal_walk(c, xy, dep + 1); if (r) return r; continue; }
        if (!(((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)c + 0x48))(c, xy) & 1)) continue;
        int col = 1, cx = *(int32_t *)(c + 0x70);   // 列号 = 同排里 x 比它小的格子数 + 1 | Column number = number of cells in the same row with a smaller x + 1
        for (uint64_t k = 0; k < n; k++) { uint64_t o = *(uint64_t *)(ch + 8 * k); if (o && o != c && bd_cell_row((const char *)*(uint64_t *)(o + 8)) == row && *(int32_t *)(o + 0x70) < cx) col++; }
        return col >= 1 && col <= 5 ? col + (row == 2 ? 5 : 0) : 0; }
    return 0;
}
// g_bd_stay = 1：这次绑定是从玩家自己开的技能树里来的，绑完不关功能页
// g_bd_stay = 1: this binding came from a skill tree the player opened themselves, so the function page is not closed after binding
static int ui_bind_modal_tap(uint64_t root, int x, int y) {   // 界面点击落下前调：点中绑定框的格子 = 吃掉这次点击、替玩家按键；返回 1 = 已处理 | Called before a UI click lands: hitting a cell of the binding box = swallow this tap and press the key for the player; returns 1 = handled
    if (atomic_load(&g_bd_state)) return 0;
    uint64_t md = ui_find(root, "ButtonBindingModal", 0); if (!md || !*(uint8_t *)(md + 0x50)) return 0;
    int slot = bd_modal_walk(md, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32), 0);
    // 格子只有 130 单位见方（手机上约 4 毫米），两排之间隔 64 单位、同排左右紧挨着。命中区往没有邻居的方向外扩（不改画面）：
    // 没点中格子时，上下各探 32 / 45 单位、左右各探 45 单位（只对最左 / 最右一列有用），由近到远取第一个探到的格子
    // A cell is only 130 units square (about 4 mm on a phone), with 64 units between the two rows and cells in the same row touching side by side. The hit area is extended toward directions with no neighbor (no visual change):
    // when no cell is hit, probe 32 / 45 units up and down and 45 units left and right (only useful for the leftmost / rightmost column), taking the first cell found from near to far
    if (!slot) { module_t *m = main_module(); int vh = m ? *(int32_t *)(m->base + 0x211da3c) : 0; static const int oy[8] = { -32, 32, -45, 45, 0, 0, -32, 32 }, ox[8] = { 0, 0, 0, 0, -45, 45, -45, 45 };
        for (int k = 0; k < 8 && !slot && vh > 0; k++) { int xx = x + ox[k] * vh / 2160, yy = y + oy[k] * vh / 2160; if (xx < 0 || yy < 0) continue; slot = bd_modal_walk(md, (uint64_t)(uint32_t)xx | ((uint64_t)(uint32_t)yy << 32), 0); } }
    if (!slot) {   // 点在框外 = 取消，留在技能树。取消键是 MINUS（mini 逐个量：MINUS / PLUS / ZR 都只关这个框、不绑定、技能树还在；B 不是取消——B 是第 2 列的目标键，按了就绑上去）。框内空白处不动 | A tap outside the box = cancel, staying in the skill tree. The cancel key is MINUS (measured one by one on the second Mac: MINUS / PLUS / ZR all just close this box, with no binding and the skill tree still open; B is not cancel - B is the target key of column 2, and pressing it binds there). Blank areas inside the box do nothing
        uint64_t bg = ui_find(md, "Background", 0); uint64_t xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32);
        if (bg && !(((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)bg + 0x48))(bg, xy) & 1)) {
            atomic_store(&g_bd_key, DM_BTN_MINUS); atomic_store(&g_bd_stay, 1); atomic_store(&g_bd_state, 3); LOG("技能绑定：点在绑定框外 → 取消（MINUS）"); return 1; }
        return bg ? 1 : 0; }   // 框内空白：吃掉这次点击（落到底下的技能树上会选中别的技能） | Blank area inside the box: swallow this tap (landing on the skill tree underneath would select another skill)
    static const uint64_t k[5] = { DM_BTN_A, DM_BTN_B, DM_BTN_Y, DM_BTN_X, DM_BTN_R };   // 第 1 列 = 手柄 A = 主技能 / 交互那个大钮；第 3 列 Y、第 4 列 X | Column 1 = controller A = the big main skill / interact button; column 3 is Y, column 4 is X
    atomic_store(&g_bd_key, k[(slot - 1) % 5] | (slot > 5 ? DM_BTN_ZL : 0)); atomic_store(&g_bd_stay, 1); atomic_store(&g_bd_state, 3);
    LOG("技能绑定：绑定框里点了第 %d 排第 %d 列", slot > 5 ? 2 : 1, (slot - 1) % 5 + 1); return 1;
}
// 顶部功能页条当前在哪一页（页签序号，见 TAB_SEL）；读不到返回 -1。开技能树时用它当「上一下翻页键已经生效」的信号
// Which page the top function page bar is currently on (tab index, see TAB_SEL); returns -1 if unreadable. Used when opening the skill tree as the signal that "the previous page-flip key has taken effect"
static int bd_top_tab(uint64_t root) {
    uint64_t tabs = ui_find(root, "UISwitcherTabs", 0); if (!tabs || !*(uint8_t *)(tabs + 0x50)) return -1;
    return TAB_SEL(tabs);
}
// ---------- 位置守卫：我们改原版控件矩形只有四处，一律走 mv_offset ----------
// MV_BIND 技能绑定框挪屏外、MV_LIFT 真全屏贴顶上提、MV_WIDE 储藏箱加宽、MV_SAFE 安全区内移。只许挪各自名单里的控件（mv_allowed，不在名单报红不写）；
// 挪的是「原值 + 偏移」：第一次挪记原值，之后发现现值不是我们上次写的（游戏重建 / 重排过）就把现值当新原值；偏移为 0 = 写回原值（现值还是我们写的才写，不盖游戏自己的值）并忘掉这一条。
// 条件没了的那位要一条不剩（mv_restore，只写还挂在界面树上的控件）。自检 mv_audit：关面板 / 切页签 / 切后台 / 退出到前端后 12 帧，条件已不成立的那位还剩条目就报红。
// 静态扫描（tests/native_write_scan.py）：本文件里写控件矩形 +0x70–+0x7c 只许在 mv_offset 与 ui_move_ours 里
// ---------- Position guard: there are only four places where we change the rects of the original game's widgets, and all go through mv_offset ----------
// MV_BIND moves the skill binding box off screen, MV_LIFT is the true-fullscreen top-lift, MV_WIDE is the stash widening, MV_SAFE is the safe-area inward move. Each may only move widgets on its own list (mv_allowed; not on the list = report a failure and do not write);
// what is moved is "original value + offset": the first move records the original value, and afterwards if the current value is found not to be what we last wrote (the game rebuilt / re-laid it out) the current value becomes the new original; offset 0 = write back the original value (only if the current value is still ours, so the game's own value is not overwritten) and forget the entry.
// A bit whose condition is gone must have no entries left (mv_restore, which only writes widgets still attached to the UI tree). Self-check mv_audit: 12 frames after closing a panel / switching tabs / going to background / exiting to the front end, a bit whose condition no longer holds but still has entries reports a failure.
// Static scan (tests/native_write_scan.py): in this file, writes to widget rects +0x70–+0x7c are only allowed inside mv_offset and ui_move_ours
enum { MV_BIND, MV_LIFT, MV_WIDE, MV_SAFE, MV_CUBE, MV_TROVE, MV_N };
static const char *const mv_name[MV_N] = { "技能绑定框挪屏外", "真全屏贴顶上提", "储藏箱加宽", "安全区内移", "魔盒并排右移", "珍宝匣页签" };
typedef struct { dm_wref r; int own, f, relayout; int32_t orig, val; } mvent;   // r：挪过的控件。认条目按 r.w 比身份、写之前用 ui_has 核在不在树上（重载布局时面板会摘下再挂回同一个对象，不能按代号作废） | r: a moved widget. Entries are matched by identity on r.w, and ui_has checks that it is still in the tree before writing (when the layout reloads, a panel is detached and reattached as the same object, so it cannot be invalidated by generation)
static mvent g_mv[96]; static int g_nmv, g_mv_want[MV_N];
static int mv_allowed(int own, uint64_t w) {
    const char *nm = (const char *)*(uint64_t *)(w + 8), *pn = *(uint64_t *)(w + 0x30) ? (const char *)*(uint64_t *)(*(uint64_t *)(w + 0x30) + 8) : NULL; if (!nm) return 0;
    switch (own) {
    case MV_BIND: return !strcmp(nm, "ButtonBindingModal");
    case MV_LIFT: return *(float *)(w + 0x4c) == 0 && *(int32_t *)(w + 0x84) <= -100;   // 界面根下贴顶的 HUD 层 | Top-attached HUD layers under the UI root
    case MV_WIDE: { static const char *const k[] = { "WideBackground", "Text0", "Text1", "TabsEndcapLeft", "TabsEndcapRight", "ControllerCursorBounds", "BankExpansionLayout", "PlayerInventoryExpansionLayout" };
        for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strcmp(nm, k[i])) return 1; return 0; }
    case MV_CUBE: return !strcmp(nm, "HoradricCubeLayout") || !strcmp(nm, "PlayerInventoryExpansionLayout") || !strcmp(nm, "Text0") || !strcmp(nm, "Text1");
    case MV_TROVE: return pn && !strcmp(pn, "BankTabs");   // 储藏箱页签条的子控件（各页签的图） | child widgets of the stash tab bar (the image of each tab)
    case MV_SAFE: return !strcmp(nm, "AutoMap") || !strcmp(nm, "HUDWarnings") || (pn && !strcmp(pn, "HireablesPanel"));
    }
    return 0;
}
static int mv_count(int own);
static void mv_prune(void) {   // 不在界面树上的条目直接忘掉（不写回：对象已被游戏销毁）；只在要新加条目时调（界面重建时才有，少） | Entries not on the UI tree are simply forgotten (no write-back: the object has been destroyed by the game); only called when a new entry is about to be added (only happens on a UI rebuild, rare)
    module_t *m = main_module(); uint64_t root = 0; if (!m) return; memcpy(&root, m->base + 0x2f05458, 8); if (!root) return; int n0 = g_nmv;
    for (int i = g_nmv - 1; i >= 0; i--) if (!ui_has(root, g_mv[i].r.w, 0)) g_mv[i] = g_mv[--g_nmv];
    if (g_nmv != n0) { static int lg; if (lg++ < 20) LOG("位置守卫：%d 条的控件已不在界面树上（界面重建过），忘掉（剩 %d）", n0 - g_nmv, g_nmv); }
}
static int32_t g_mv_base; static int g_mv_base_on;   // 调用方知道这个控件的原位时（如小地图面板贴边 = -宽）在调用前给出：按它算，不拿控件此刻的值当原值（界面重建出来的新控件可能带着我们上一遍挪过的值） | When the caller knows this widget's original position (e.g. the minimap panel flush against the edge = -width), it supplies it before the call: compute from that instead of taking the widget's current value as the original (a new widget produced by a UI rebuild may carry the value we shifted on the previous pass)
static void mv_offset(int own, uint64_t w, int f, int32_t delta, int relayout) {   // f：0 x、1 y、2 宽 | f: 0 x, 1 y, 2 width
    if (!w) return; int32_t *v = (int32_t *)(w + 0x70 + 4 * f); int i = 0; while (i < g_nmv && !(g_mv[i].r.w == w && g_mv[i].f == f)) i++;   // 按原指针认条目（只比身份）：w 是调用方此刻从界面树上取的活控件；同址的新控件由下面「现值≠上次写的值 → 现值当原值」接住 | Match entries by original pointer (identity comparison only): w is the live widget the caller just took from the UI tree; a new widget at the same address is caught by the "current value ≠ last written value → treat current value as the original" rule below
    if (i == g_nmv) { if (!delta) return; if (!mv_allowed(own, w)) { static int lg; if (lg++ < 8) LOG("【红】位置守卫：%s 要挪的控件「%s」不在名单里，不写", mv_name[own], (const char *)*(uint64_t *)(w + 8)); return; }
        mv_prune();   // 游戏重建界面（如切语言）后旧控件对象没了、新对象另起一条：先把不在界面树上的条目忘掉（不写，内存可能已释放），否则条目越积越多、96 条满了新控件写不进（英文版 ×8 红） | After the game rebuilds the UI (e.g. a language switch) the old widget objects are gone and the new objects start new entries: first forget entries not on the UI tree (no write, the memory may already be freed), otherwise entries keep piling up and once the 96 entries are full new widgets cannot be written (English version: ×8 red)
        if (g_nmv >= 96) { static int lg; if (lg++ < 8) LOG("【红】位置守卫：%s 条目满了（96 条都还挂在界面树上），「%s」不写", mv_name[own], (const char *)*(uint64_t *)(w + 8)); return; }
        if (!mv_count(own)) { static int lg[MV_N]; if (lg[own]++ < 40) LOG("位置守卫：%s 开始挪（%s）", mv_name[own], (const char *)*(uint64_t *)(w + 8)); }   // 每次「开挪」留一条证据（绑定框只挪 0.2 秒，自检碰不上） | Leave one line of evidence at every "start moving" (the binding box is only moved for 0.2 s, which the self-check never catches)
        i = g_nmv;   // prune 后新条目的下标可能已变；后续必须更新实际插入的条目 | Pruning can move the insertion slot; update the entry actually inserted | Pruning can move the insertion slot; update the entry actually inserted
        g_mv[g_nmv] = (mvent){ { 0, 0 }, own, f, relayout, *v, *v }; dm_wref_set(&g_mv[g_nmv++].r, w); }
    mvent *e = &g_mv[i];
    // 写回时控件 x 不是我们上次写的值 = 原版暂时把它挪走了（＋ 关储藏箱：原版同一帧单开物品栏，把我们挪过的 x 记成自己的原位，单开的关掉时写回它）。
    // 条目留着、原值不动，等它回到我们的值再写回原位；丢掉的话我们的位移被原版当原位，下次再叠一次（tests/t_widestash_restore.c）
    // On restore the widget value is not the one we last wrote = the original game has moved it away for now (+ closing the stash: in the same frame the original game opens the inventory alone, records our shifted x as its own original position and writes it back when that inventory closes).
    // Keep the entry and its original value, and restore once the widget holds our value again; dropping it would let the original game adopt our shift as its original position and stack another one next time (tests/t_widestash_restore.c)
    if (!delta && !g_mv_base_on && *v != e->val) return;
    if (g_mv_base_on) e->orig = g_mv_base; else if (*v != e->val) e->orig = *v;   // 游戏重建 / 重排过：现值当原值（调用方给了原位就用给的） | The game has rebuilt / re-laid out the UI: take the current value as the original value (if the caller supplied an original position, use that)
    if (!delta) { if (*v == e->val && *v != e->orig) { *v = e->orig; if (e->relayout) ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x10))(w); } g_mv[i] = g_mv[--g_nmv]; return; }
    int32_t want = e->orig + delta; if (*v != want) { *v = want; if (relayout) ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x10))(w); } e->val = want;
}
static void mv_restore(uint64_t root, int own) {   // 这一位的条目全部写回原值（只写还挂在界面树上的，不在了就忘掉） | Write all entries of this bit back to their original values (only those still attached to the UI tree; forget the ones that are gone)
    int n0 = mv_count(own); if (!n0) return;
    for (int i = g_nmv - 1; i >= 0; i--) if (g_mv[i].own == own) { if (root && ui_has(root, g_mv[i].r.w, 0)) mv_offset(own, g_mv[i].r.w, g_mv[i].f, 0, g_mv[i].relayout); else g_mv[i] = g_mv[--g_nmv]; }
    int n1 = mv_count(own); if (n1 == n0) return;   // 只剩等原版写回的条目、这一帧没变：不记（每帧都会进来） | Only entries waiting for the original game to write back, nothing changed this frame: do not log (this runs every frame)
    static int lg[MV_N]; if (lg[own]++ < 40) LOG("位置守卫：%s 条件没了，%d 处全部写回（剩 %d）", mv_name[own], n0, n1);
}
static int mv_count(int own) { int n = 0; for (int i = 0; i < g_nmv; i++) n += g_mv[i].own == own; return n; }
static uint64_t g_mv_audit_at; static const char *g_mv_audit_path = "";
static void mv_audit_arm(const char *path) {   // 前一条还没到点就合并路名：切后台时客体时钟冻结，自检要到回前台后才跑，原先被「回前台」覆盖、整局没有「切后台」那行 | If the previous one has not come due yet, merge the path names: on going to background the guest clock freezes and the self-check only runs after returning to foreground; it used to be overwritten by 「回前台」(back to foreground), so the whole run had no 「切后台」(to background) line
    static char both[96]; if (g_mv_audit_at && strcmp(g_mv_audit_path, path)) { snprintf(both, sizeof both, "%s → %s", g_mv_audit_path, path); g_mv_audit_path = both; } else g_mv_audit_path = path;
    g_mv_audit_at = dm_guest_frame() + 12; }
void dm_ui_lifecycle(int background) { mv_audit_arm(background ? "切后台" : "回前台"); }   // iOS 前端进 / 出后台时调（front_ios.m） | Called by the iOS front end on entering / leaving the background (front_ios.m)
static void mv_audit_pump(uint64_t root, uint64_t fr) {
    if (!g_mv_audit_at || fr < g_mv_audit_at) return; g_mv_audit_at = 0; int bad = 0; char line[256]; int k = 0;
    // 只数控件此刻还是我们写的值的条目：等原版写回的条目（原版暂时挪走了）不算没写回 | Only count entries whose widget still holds our value: entries waiting for the original game to write back (it has moved the widget away for now) are not unrestored
    for (int o = 0; o < MV_N; o++) { int live = 0; for (int i = 0; i < g_nmv; i++) live += g_mv[i].own == o && ui_has(root, g_mv[i].r.w, 0) && *(int32_t *)(g_mv[i].r.w + 0x70 + 4 * g_mv[i].f) == g_mv[i].val;
        if (!g_mv_want[o] && live) { bad++; k += snprintf(line + k, sizeof line - k, "%s 还剩 %d 处没写回；", mv_name[o], live); }
        else if (k < 200) k += snprintf(line + k, sizeof line - k, "%s %s；", mv_name[o], g_mv_want[o] ? "条件成立、挪着" : "已全部写回"); }
    static const char *lp; static uint64_t lfr; if (!bad && lp == g_mv_audit_path && fr - lfr < 600) return;   // 都对：同一条路 10 秒内只记一次（关面板消息很密）；红的一律记 | All correct: the same path is logged only once per 10 s (close-panel messages are dense); failure lines are always logged
    lp = g_mv_audit_path; lfr = fr; LOG("%s位置守卫自检（%s）：%s", bad ? "【红】" : "", g_mv_audit_path, line);
}
static void ui_bind_pump(uint64_t root, uint64_t fr) {
    static int last, tries, zl, page; static uint64_t t0, next, pressed;
    int st = atomic_load(&g_bd_state);
    // 点十字 / 从位出发的绑定：游戏自己的「重新绑定技能」框只是我们替玩家按键的中间步骤，不让它闪出来（原则：只换操作不换逻辑——框照开、键照按，只是看不见）。
    // 做法是每帧把框挪到屏幕外（矩形 x 加 30000）。不能关它的可见位：关了它就收不到键，B 会落到技能树上把功能页关掉。
    // 从技能出发（技能树里长按 → 玩家自己点格子）的那条要给玩家看，不挪。
    // Binding started by tapping the cross / from a slot: the game's own 「重新绑定技能」(Press button to rebind skill) box is only an intermediate step of us pressing keys for the player, so it is not allowed to flash up (principle: change only the operation, not the logic - the box still opens and the keys are still pressed, it just cannot be seen).
    // The method is to move the box off screen every frame (add 30000 to the rect x). Its visible flag must not be turned off: with it off the box cannot receive keys, and B would land on the skill tree and close the function page.
    // The path that starts from a skill (long press in the skill tree → the player taps a cell themselves) must be shown to the player, so it is not moved.
    { uint64_t md = ui_panel(root, "ButtonBindingModal"); int away = md && (st == 2 || st == 3) && !atomic_load(&g_bd_stay); g_mv_want[MV_BIND] = away;
      if (md && away) mv_offset(MV_BIND, md, 0, 30000, 0); else mv_restore(root, MV_BIND); }   // 框还在、不再挪也走 mv_restore：写回留一条证据（g 局：绑定状态先于关框结束，写回无日志） | Box still there but no longer being moved also goes through mv_restore: the write-back leaves a line of evidence (run g: the binding state ended before the box closed, and the write-back had no log)
    if (!st) { last = 0; return; }
    if (st != last) { last = st; t0 = fr; next = fr; tries = 0; zl = 0; pressed = 0; page = -1; }
    if (fr < next) return; next = fr + (st == 2 || st == 5 ? 6 : 1);   // 开页、按目标键、关页三段每帧看反馈（原先固定 6 帧一看、按完键固定等 24–30 帧） | The three stages - open the page, press the target key, close the page - check feedback every frame (previously checked at a fixed 6-frame interval, with a fixed 24–30 frame wait after a key press)
    int tree = bd_open(root, "SkillsTreePanel"), modal = bd_open(root, "ButtonBindingModal");
    uint64_t key = atomic_load(&g_bd_key);
    switch (st) {
    case 1: {   // 开技能树：按 + 开功能页，再按 R 翻到技能树。每一下按完看反馈再按下一下（按页面反馈推进，不逐页固定等 24 帧）： | Open the skill tree: press + to open the function page, then press R to flip to the skill tree. After each press, check the feedback before pressing the next (advance on page feedback rather than a fixed 24-frame wait per page):
        // 功能页条的当前页变了 = 上一下 R 生效了，隔够松键的帧数就按下一下；一直没反应才等满 24 帧重试。功能页还没开时只重试 +——R 在游戏画面里是技能键（PR 指出）
        // The current page of the function page bar changed = the previous R took effect; press the next one once the key-release frames have passed; only wait the full 24 frames and retry when there is no response at all. While the function page is not open yet, only retry + - on the game screen R is a skill button (pointed out in a PR)
        if (tree) { LOG("技能绑定：技能树已开（开页用了 %llu 帧、按键 %d 次），等玩家点一个技能（目标键 %#llx）", (unsigned long long)(fr - t0), tries, (unsigned long long)key); atomic_store(&g_bd_state, 5); break; }
        if (tries >= 9) { LOG("【红】技能绑定：按了 %d 次键技能树还没开，放弃", tries); atomic_store(&g_bd_state, 0); break; }
        if (fr - t0 < ((key & DM_BTN_ZL) ? 12 : 2)) break;   // 第二组技能时 ZL 一直按着：先让它松开稳住——松 ZL 与按 + 同一帧到，游戏不开功能页；第一组不用等 | With the second skill set ZL is held the whole time: let it release and settle first - if releasing ZL and pressing + arrive on the same frame the game does not open the function page; the first set needs no wait
        int dm = bd_open(root, "UISwitcher"), cur = dm ? bd_top_tab(root) : -1;
        if (tries) { uint64_t dt = fr - pressed; int moved = dm && cur >= 0 && cur != page;   // 上一下按完之后：页变了且已松键 3 帧 → 接着按；否则等满 24 帧 | After the previous press: page changed and the key has been released for 3 frames → press the next one; otherwise wait the full 24 frames
            if (!((moved && dt >= 7) || dt >= 24)) break; }
        page = cur; pressed = fr; dm_ui_key_pulse(dm ? DM_BTN_R : DM_BTN_PLUS, 4); tries++; break; }
    case 5: {   // 找页：tries = 已看过几页 | Find the page: tries = number of pages looked at so far
        uint64_t tw = ui_panel(root, "SkillsTreePanel"); int cur = tw ? bd_tab_cur(tw) : -1; char nm[16];
        if (!tree || cur < 0 || tries > 6) { atomic_store(&g_bd_state, 2); break; }
        if (atomic_load(&g_tab_left)) break;   // 上一次切页的键还没按完 | The key press of the previous page switch has not finished yet
        next = fr + 12;
        if (!tries) { tries = 1; if (cur) { bd_tab_step(-cur); break; } }
        snprintf(nm, sizeof nm, "Tab%d", cur); uint64_t tb = bd_child(tw, nm);
        if (cur == 3 || (tb && bd_tab_learned(tb))) { LOG("技能绑定：停在第 %d 页（第一个有已学技能的分类页）", cur); atomic_store(&g_bd_state, 2); break; }
        tries++; bd_tab_step(1); break; }
    case 2:
        if (modal) { atomic_store(&g_bd_state, 3); break; }
        if (!tree && fr - t0 > 30) { LOG("技能绑定：玩家关了技能树，取消"); atomic_store(&g_bd_state, 0); }
        break;
    case 3:
        if (!modal) { if (tries) { int stay = atomic_load(&g_bd_stay); LOG("技能绑定：%s %#llx，%s", key == DM_BTN_MINUS ? "已取消，键" : "已绑到键", (unsigned long long)key, stay ? "留在技能树" : "关功能页"); atomic_store(&g_bd_state, stay ? 0 : 4); } else if (fr - t0 > 120) atomic_store(&g_bd_state, 0); break; }
        if (fr - t0 < 12) break;   // 框开稳再按 | Press once the box has opened and settled
        if (pressed && fr - pressed < 30) break;   // 按过了：每帧看框关没关（上面），30 帧没关才重按 | Already pressed: check every frame whether the box has closed (above); re-press only if it has not closed after 30 frames
        if (tries >= 3) { LOG("【红】技能绑定：在绑定框里按了 3 次目标键 %#llx 框都没关，放弃", (unsigned long long)key); atomic_store(&g_bd_state, 0); break; }
        // 第二排（ZL 组）：先单按住 ZL 10 帧再加目标键。两个键同一帧按下游戏只认目标键，绑到第一排
        // Second row (ZL set): hold ZL alone for 10 frames first, then add the target key. If both keys go down on the same frame the game only sees the target key and binds to the first row
        if ((key & DM_BTN_ZL) && !zl) { zl = 1; dm_ui_key_pulse(DM_BTN_ZL, 30); next = fr + 10; break; }
        zl = 0; dm_ui_key_pulse(key, 6); tries++; pressed = fr; break;
    case 4:
        if (!tree) { atomic_store(&g_bd_state, 0); break; }
        if (pressed && fr - pressed < 30) break;   // 每帧看技能树关没关，30 帧没关才重按 | Check every frame whether the skill tree has closed; re-press only if it has not closed after 30 frames
        if (tries >= 3) { atomic_store(&g_bd_state, 0); break; }
        if (fr - t0 < ((key & DM_BTN_ZL) ? 10 : 3)) break;   // 目标键刚松开：隔几帧再按返回。第二组：这一步宿主替玩家松开 ZL（dm_ui_auto_release），要松稳了返回键才认——mini：等 3 帧不认（靠 30 帧后的重按，0.58 秒）、8 帧认（0.17 秒），取 10 帧留余量 | The target key was just released: wait a few frames before pressing back. Second set: at this step the host releases ZL for the player (dm_ui_auto_release), and the back key is only accepted once the release has settled - observed on the second Mac: not accepted after waiting 3 frames (relies on the re-press 30 frames later, 0.58 s), accepted at 8 frames (0.17 s); use 10 frames for margin
        dm_ui_key_pulse(DM_BTN_B, 4); tries++; pressed = fr; break;
    }
}
// ---------- 全屏地图 ↔ 右上角小地图：运行时切，不落盘（读反汇编） ----------
// 「透视地图尺寸」是配置变量 AutoMapMode（对象 main+0x2d479e8，值在 +0xC：0 全屏 / 1 微缩放左 / 2 微缩放右；注册处 main+0x30d2a8），注册时带变更回调 main+0x40f780(int 档)：
//   [main+0x2d5e8f0] = 档 == 1（放左）；[main+0x2d5c748] = 档 != 0（是否微缩）；变了就调 main+0x411540 重排地图、把卷动量 [0x2d5e910 / 914] 清零。
// 只改配置值不调回调，画面不变。这里直接调回调：切全屏传 0，切回传配置值（玩家在选项页选的放左 / 放右）；配置对象本身不动，所以不落盘、下一局照玩家的设置来。
// 触屏模式记住玩家要不要全屏地图（g_map_want）：每局开头与玩家在选项页改档时取「配置是不是全屏档」，点小地图 / 点空处跟着改；
// 开面板时让成小地图（g_map_hold，前端报面板态），关掉面板、换区域后游戏按配置重建了地图，都按 g_map_want 再切回去。
// 配置是全屏档（0）时小地图按「微缩放右」（2）显示，配置值不改写——玩家选的全屏照存照用（线上：选全屏无效、开装备栏 / 换图就回小地图）。
// 手柄上没有切这一档的键（地图键 − 只管开 / 关）。
// ---------- Fullscreen map ↔ top-right minimap: switched at runtime, not persisted (from reading the disassembly) ----------
// 「透视地图尺寸」(Automap Size) is the config variable AutoMapMode (object main+0x2d479e8, value at +0xC: 0 fullscreen / 1 mini on the left / 2 mini on the right; registered at main+0x30d2a8), registered with a change callback main+0x40f780(int mode):
//   [main+0x2d5e8f0] = mode == 1 (on the left); [main+0x2d5c748] = mode != 0 (mini or not); on a change it calls main+0x411540 to re-layout the map and zeroes the scroll values [0x2d5e910 / 914].
// Changing only the config value without calling the callback leaves the picture unchanged. Here the callback is called directly: pass 0 to switch to fullscreen, pass the config value to switch back (left / right as the player chose on the options page); the config object itself is untouched, so nothing is persisted and the next run follows the player's setting.
// In touch mode, remember whether the player wants the full-screen map (g_map_want): at the start of each run and whenever the player changes the setting on the options page it follows
// "is the config the full-screen setting", and tapping the minimap / tapping empty space changes it; while a panel is open the map steps aside to the minimap (g_map_hold, the front end reports panel state),
// and after the panel closes, or after an area change in which the game rebuilt the map from the config, it is switched back to g_map_want.
// With the config on full screen (0) the minimap is shown as "minimap right" (2) and the config value is not rewritten: the player's full-screen choice is kept and used (player report: choosing full screen had no effect, and opening the inventory / changing area went back to the minimap).
// The controller has no key for switching this mode (the map key − only opens / closes).
static _Atomic int g_map_req = -1, g_map_full_now, g_map_hold;
static int g_map_want = -1, g_map_cfg_seen = -1;   // 主线程（ui_map_pump）独用 | main thread (ui_map_pump) only
void dm_ui_map_full(int on) { atomic_store(&g_map_req, on != 0); }
void dm_ui_map_hold(int on) { atomic_store(&g_map_hold, on != 0); }
int dm_ui_map_is_full(void) { return atomic_load(&g_map_full_now); }
// 真全屏局内，全屏地图的线条要盖在世界上：游戏把地图中心放在「屏幕高的一半」（它读到的是我们写的 16:9 界面高），画出来在 界面矩形 y + 界面高 ÷ 2；
// 世界的中心在真实画面高的一半，两者差 界面矩形 y −（真实高 − 界面高）÷ 2（贴底时 = 留边高度的一半，地图标记比人低约 128 点）。
// 地图有一对卷动量 [main+0x2d5e910 / 914]（单位 = 地图格；中心 = 玩家所在格 + 卷动量，main+0x413354 / 0x4133d8；游戏只在切档等处清零，触摸 / 手柄下没有别的写入方），
// 纵向写成「上移这段距离」对应的格数。每格的屏幕像素照游戏自己的算法（0x413280–0x413298）：max(⌈面板缩放 × [面板+0x15a8] × 8⌉ ÷ 8, 1/8) × [main+0x2d5e91c]，
// 面板缩放调游戏的 main+0x390b38(面板)。只在全屏地图（非微缩）且界面矩形下移时写；其余情况把我们写的值清掉。右上角小地图走贴顶上提，不用它。
// In a true-fullscreen run, the fullscreen map's lines must overlay the world: the game puts the map center at "half the screen height" (what it reads is the 16:9 UI height we wrote), so it is drawn at UI rect y + UI height ÷ 2;
// the world's center is at half the real screen height, and the two differ by UI rect y − (real height − UI height) ÷ 2 (when bottom-aligned = half the margin height; map markers are about 128 pt lower than the character).
// The map has a pair of scroll values [main+0x2d5e910 / 914] (unit = map cells; center = the player's cell + scroll, main+0x413354 / 0x4133d8; the game only zeroes them on mode switches and the like, and there is no other writer under touch / controller),
// and the vertical one is written as the number of cells corresponding to "move up by this distance". Screen pixels per cell follow the game's own algorithm (0x413280–0x413298): max(⌈panel scale × [panel+0x15a8] × 8⌉ ÷ 8, 1/8) × [main+0x2d5e91c],
// with panel scale obtained by calling the game's main+0x390b38(panel). Written only for the fullscreen map (not mini) when the UI rect is shifted down; in all other cases the value we wrote is cleared. The top-right minimap uses the top-lift and does not need this.
static void ui_map_center(module_t *m, uint64_t root, int mini) {
    static int ours; int32_t *sc = (int32_t *)(m->base + 0x2d5e914); int want = 0;
    int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); uint64_t am = !mini && dm_uiscreen_ingame() && dm_screen_real(&rw, &rh) && h > 0 && h < rh ? ui_panel(root, "AutoMap") : 0;
    if (am && *(uint64_t *)am == (uint64_t)m->base + 0x20c58a0) { float q = ceilf(((float (*)(uint64_t))(m->base + 0x390b38))(am) * *(float *)(am + 0x15a8) * 8) / 8; if (q < 0.125f) q = 0.125f;
        float cell = q * *(float *)(m->base + 0x2d5e91c); if (cell > 0.1f) want = (int)lroundf((y - (rh - h) / 2.0f) / cell); }
    if (*sc == want || (*sc != ours && *sc != 0)) { if (*sc == want) ours = want; return; }   // 已是目标值；或是别人写的非零值（不是我们上次写的）：不动 | Already the target value; or a non-zero value written by someone else (not what we last wrote): leave it alone
    { static int lg; if (lg++ < 8) LOG("全屏地图中心：纵向卷动量 %d → %d 格", *sc, want); } *sc = want; ours = want;
}
static void ui_map_pump(module_t *m) {
#if defined(__OHOS__)
    // 鸿蒙的 UI 线程可能在选角面板加载时先请求收起地图；此时原版 AutoMap 尚未创建，回调会解引用空对象。
    // 局外不调用地图回调，也不把旧请求带进下一局；游戏内的切换仍走原路径。
    // The HarmonyOS UI thread can request a map reset while character select is loading, before the guest AutoMap exists.
    // Discard out-of-game requests instead of calling the map callback or carrying them into the next game.
    if (!dm_uiscreen_ingame()) {
        int pending = atomic_exchange(&g_map_req, -1);
        atomic_store(&g_map_full_now, 0);
        if (pending >= 0) { static int n; if (n++ < 4) LOG("鸿蒙：局外地图切换请求已忽略（地图尚未就绪）"); }
        return;
    }
#endif
    int cfg = *(volatile int32_t *)(m->base + 0x2d479f4), mini = *(volatile int32_t *)(m->base + 0x2d5c748);
    // 测试开关 DM_TEST_AUTOMAP=<帧>:<档>：到帧把配置改成这一档并调回调（当作玩家在选项页选了它）
    // Test switch DM_TEST_AUTOMAP=<frame>:<setting>: at that frame, change the config to this setting and call the callback (as if the player had picked it on the options page)
    { static long at = -1; static int tv; uint64_t fr = dm_guest_frame(); if (at < 0) { const char *e = getenv("DM_TEST_AUTOMAP"); at = e ? atol(e) : 0; const char *c = e ? strchr(e, ':') : NULL; tv = c ? atoi(c + 1) : 0; }
      if (at > 0 && fr >= (uint64_t)at) { at = 0; *(volatile int32_t *)(m->base + 0x2d479f4) = tv; ((void (*)(int))(m->base + 0x40f780))(tv); cfg = tv; mini = *(volatile int32_t *)(m->base + 0x2d5c748); LOG("测试 DM_TEST_AUTOMAP：帧 %llu 把「透视地图尺寸」改成 %d", (unsigned long long)fr, tv); } }
    int dm_padmode_current(void); dm_hud_info hi; dm_hud_get(&hi);
    int r = atomic_exchange(&g_map_req, -1);
    if (dm_padmode_current()) {   // 手柄模式是原版界面：三档照原版，只有微缩档能临时切全屏 | controller mode is the original UI: the three settings work as in the original game, and only the minimap settings can switch to full screen temporarily
        if (r >= 0) {
            if (cfg != 1 && cfg != 2) { static int n; if (n++ < 4) LOG("地图切换：玩家的「透视地图尺寸」是 %d（不是微缩档），不切", cfg); }
            else if ((mini == 0) != r) { ((void (*)(int))(m->base + 0x40f780))(r ? 0 : cfg); mini = *(volatile int32_t *)(m->base + 0x2d5c748); LOG("地图切换：→ %s（配置值 %d 不动，不落盘）", r ? "全屏" : cfg == 1 ? "微缩放左" : "微缩放右", cfg); }
        }
        atomic_store(&g_map_full_now, mini == 0 && (cfg == 1 || cfg == 2)); return;
    }
    if (cfg != g_map_cfg_seen) {   // 每局第一次读到配置、或玩家在选项页改了档：要不要全屏跟配置走 | first config read in a run, or the player changed the setting on the options page: whether to show the full-screen map follows the config
        g_map_want = cfg == 0; { static int n; if (n++ < 8) LOG("地图：「透视地图尺寸」读到 %d → 触屏下%s", cfg, cfg == 0 ? "缺省全屏地图（点空处回小地图）" : "缺省小地图"); } g_map_cfg_seen = cfg; }
    if (r >= 0 && g_map_want != r) { g_map_want = r; LOG("地图：玩家点了 → 记住%s", r ? "全屏地图" : "小地图"); }
    int small = cfg == 1 ? 1 : 2, hold = atomic_load(&g_map_hold), want = g_map_want == 1 && !hold;   // 全屏档时小地图按微缩放右显示 | with the full-screen setting, the minimap is shown as minimap right
    if (hi.valid && (mini == 0) != want) {   // 局内才切（局外原版地图面板还没建）；换区域后游戏按配置重建、开关面板，都在这里对回去 | switch only in a run (outside one the original map panel does not exist yet); after an area change the game rebuilds from the config, and panels open / close: both are put right here
        ((void (*)(int))(m->base + 0x40f780))(want ? 0 : small); mini = *(volatile int32_t *)(m->base + 0x2d5c748);
        static int n; if (n++ < 40) LOG("地图：→ %s（%s；配置值 %d 不动）", want ? "全屏" : small == 1 ? "微缩放左" : "微缩放右", hold ? "面板开着先让开" : g_map_want ? "玩家要全屏" : "玩家要小地图", cfg); }
    atomic_store(&g_map_full_now, mini == 0);
}
// ---------- 右上角小地图的位置：触摸层把它当点击区（点一下 = 地图开关键），矩形取游戏自己的 AutoMap 面板，不写死 ----------
// AutoMap 控件（虚表 main+0x20c58a0）：+0x70 / +0x74 = x / y、+0x78 / +0x7c = 宽 / 高（布局单位，2160 = 画面高；微缩地图放右时 x = −宽，即贴着画面右边），+0x80 缩放。
// 微缩地图放右 = (−1279, 78, 1279, 720)，即画面右侧宽 0.59 个屏高、从上往下 3.6% – 36.9% 的那一块。面板关着 / 没加载时沿用最近一次读到的。
// ---------- Position of the top-right minimap: the touch layer treats it as a tap area (one tap = the map toggle key); the rect is taken from the game's own AutoMap panel, not hard-coded ----------
// AutoMap widget (vtable main+0x20c58a0): +0x70 / +0x74 = x / y, +0x78 / +0x7c = width / height (layout units, 2160 = screen height; with the mini map on the right x = −width, i.e. flush with the right edge of the screen), +0x80 scale.
// Mini map on the right = (−1279, 78, 1279, 720), i.e. the block on the right side of the screen 0.59 screen heights wide, from 3.6% to 36.9% down from the top. When the panel is closed / not loaded, the last value read is reused.
static _Atomic uint64_t g_mm_rect; static _Atomic int g_mm_have;
int dm_ui_minimap_rect(float r[4]) { if (!atomic_load(&g_mm_have)) return 0; uint64_t v = atomic_load(&g_mm_rect); for (int i = 0; i < 4; i++) r[i] = (float)(int16_t)(uint16_t)(v >> (16 * (3 - i))) / 8192.0f; return 1; }
// 贴界面矩形顶边的面板回真实画面顶部（iPad 真全屏贴底 / 居中：界面矩形在画面里下移了 y0，见 dm_uiscreen.c）：面板矩形 y（+0x74，布局单位 2160 = 界面高）减去 y0 换成的单位数，
// 画到界面坐标的负 y 上（图形层的界面下移是投影偏移，视口顶边仍在画面顶，负 y 画得出来）。
// 认法（一条规则，没有名单）：局内（dm_uiscreen_ingame）PanelManager 的直接子面板里，层号（+0x84，int）≤ -100 且纵向锚点（+0x4c，float）= 0 的。
// 层号 ≤ -100 是垫在最底下的常显信息层（转储：AutoMap -110，HUDWarnings / HUDMonsterHealth / HUDLevelNames / HireablesPanel -100；物品名 -10、底栏 -3、
// 面板 0–10、暂停菜单是正的）；其中纵向锚点 0 = 贴顶边（进区域的地名横幅 HUDLevelNames 锚在 0.2，不中）。只按锚点认会把暂停菜单（0.5 / 0）也算进去，试过。
// 不上提的一个贴顶面板：ScrollSpeechPanel（NPC 说话时顶部正中的滚动字幕，锚点 0.5 / 0、层号 101）。按名字补进来过：框上去了、字没了（滚动文字有自己的剪裁区，
// 落到界面矩形上方就被裁光；iOS 包、Mac 宿主截图都是空框），撤掉。它留在界面矩形顶边，NPC 在画面中上部时会压到 NPC。
// HUDMonsterHealth（顶部首领血条，层 -100、锚点 0.5 / 0）照规则上提。它带贴图的进度段靠「设剪裁区」画出比例，上提到负 y 后游戏给图形层的剪裁区是空的
// （图形入口实读：x 1 y 1 宽 0 高 0），进度段整段不画——名字、条框、百分比都在、条是黑的。所以触屏布局把进度段换成键鼠版原版不带贴图的画法
// （touch_layout.py _boss_bar_top：游戏直接按比例画矩形，不经剪裁区），上提后照常。NPC 滚动字幕是同一个原因，它没有不经剪裁区的画法，仍不上提
// 只在局内做：前端各页是整页布局，标题、人物点击区、底部提示在同一个面板里，整体上提会把它们一起带走。
// 原值记下，游戏自己重排过（值不是我们上次写的）就把现值当原值；y0 回 0 / 出局时写回原值。每 6 帧对一次。
// 局内 / 局外信号里「读取画面在不在」以面板消息为准，并在消息到达的那一刻就翻转、顺手把贴顶上提对一遍：游戏在同一帧里先处理消息再排界面，
// 等下一帧每帧那一处看面板树才翻的话，读取画面撤掉的那一帧界面还按居中排（那一帧血球在居中位置、地名没上提）。-1 = 还没收到过消息，按面板树看。
// Panels attached to the top edge of the UI rectangle go back to the real top of the screen (iPad true fullscreen bottom-aligned / centered: the UI rectangle is shifted down by y0 within the screen, see dm_uiscreen.c): the panel rectangle y (+0x74, layout units, 2160 = UI height) has y0 converted to units subtracted from it,
// and is drawn at a negative y in UI coordinates (the graphics layer's UI downshift is a projection offset, the viewport top edge is still at the screen top, so negative y can be drawn).
// How they are recognized (one rule, no name list): in a run (dm_uiscreen_ingame), among the direct child panels of PanelManager, those with layer number (+0x84, int) ≤ -100 and vertical anchor (+0x4c, float) = 0.
// Layer number ≤ -100 is the always-shown information layer at the very bottom (dump: AutoMap -110, HUDWarnings / HUDMonsterHealth / HUDLevelNames / HireablesPanel -100; item names -10, bottom bar -3,
// panels 0–10, the pause menu is positive); among them vertical anchor 0 = attached to the top edge (the area-entry name banner HUDLevelNames is anchored at 0.2 and does not match). Recognizing by anchor alone would also include the pause menu (0.5 / 0); tried.
// One top-attached panel that is not lifted: ScrollSpeechPanel (the scrolling subtitles at top center while an NPC speaks, anchor 0.5 / 0, layer 101). It was once added by name: the frame went up and the text vanished (scrolling text has its own scissor rectangle,
// and is clipped away entirely once above the UI rectangle; both the iOS app and Mac host screenshots show an empty frame), so it was reverted. It stays at the top edge of the UI rectangle and covers the NPC when the NPC is in the upper middle of the screen.
// HUDMonsterHealth (the boss health bar at the top, layer -100, anchor 0.5 / 0) is lifted per the rule. Its textured progress segment draws the ratio by "setting a scissor rectangle", and after lifting to negative y the scissor rectangle the game gives the graphics layer is empty
// (actual read at the graphics entry: x 1 y 1 width 0 height 0), so the whole progress segment is not drawn -- the name, bar frame and percentage are all there, and the bar is black. So the touch layout replaces the progress segment with the keyboard-and-mouse version's original untextured drawing
// (touch_layout.py _boss_bar_top: the game draws a rectangle directly by ratio, without a scissor rectangle), which works as usual after lifting. The NPC scrolling subtitles have the same cause, but have no drawing path that avoids the scissor rectangle, so they are still not lifted
// Done only in a run: front-end pages are whole-page layouts, with the title, character tap areas and bottom hints in the same panel, and lifting the whole thing would take them along.
// The original value is recorded; if the game has re-laid it out itself (the value is not what we last wrote) the current value is taken as the original; when y0 returns to 0 / on leaving the run the original value is written back. Reconciled once every 6 frames.
// In the in-run / out-of-run signal, "whether the loading screen is present" goes by the panel message, flips at the very moment the message arrives, and reconciles the top lift on the spot: within one frame the game processes messages before laying out the UI,
// and if the flip waited for the per-frame check of the panel tree in the next frame, the UI would still be laid out centered in the frame the loading screen is removed (in that frame the health globe is at the centered position and the area name is not lifted). -1 = no message received yet, go by the panel tree.
static _Atomic int g_load_up = -1;
static uint64_t g_lift_next;
// 系统提示框（NotificationPanel）的停靠高度分两态。布局里写的是没开面板时的高度（gen_romfs_overlay.py：上提到让开底栏上方的生命 / 法力数值）；
// 开着大面板时那行数值被面板盖住，上提后的框反而压在背包格子上，所以面板态改停到 NOTE_PANEL_Y（布局单位，同 json 的 y）：大面板底边框那一带，在金币 / 存放那排下面。
// 大面板 = 下面名单里的界面根直接子可见（NPC 讲话框、对话菜单这类小框不算：那时底栏还在，框要留在上提位）。名单漏了的面板照无面板处理（= 上一版行为）
// 改的是对象 +0x19c「停靠目标 y」：原版重排（main+0x3d7e1c）按布局把矩形 y 抄进去，每帧更新（main+0x3d7fbc）滑入滑出时按 (1-t)·目标 插值写 +0x74、
// 自己从不写 +0x19c——所以改它不跟动画抢坐标（用位置守卫改 +0x74 就是被动画每帧盖回）。单位换算：矩形高 +0x7c 对应布局高 244。
// 写回由我们确定性做（把写 +0x19c 登记成例外的条件；原版控件规则见 tests/native_write_scan.py NATIVE_FIELD_OK）：
// 我们记下原值，停靠条件一结束——面板关、读取画面开（切场景）、保存并退出、回到前端——当场写回，不靠游戏重排；重排只是第二道保险
// （现值不是我们上次写的 = 游戏重排过（换模式 / 改分辨率），当新原值）。消息路（dm_ui_on_uimsg）在消息到达那一刻写，每帧路兜着。
// 保存并退出 / 回到前端之后到下一次进局之前不再停靠（暂停菜单在退出那几帧还挂着，不压住的话下一帧又停回面板态）。
// 测试开关 DM_TEST_NOTE_PANEL_Y=<单位> 试别的高度
// The docking height of the system notice box (NotificationPanel) has two states. The layout holds the height with no panel open (gen_romfs_overlay.py: raised out of the way of the life / mana numbers above the bottom bar);
// with a large panel open that row of numbers is covered by the panel, and the raised box would sit on the inventory cells instead, so in panel state it docks at NOTE_PANEL_Y (layout units, like y in the json): the band of the large panel's bottom frame, below the gold / Store row.
// Large panel = a root of the list below being a visible direct child (small boxes such as the NPC speech box and dialogue menu do not count: the bottom bar is still there then, and the box stays in the raised position). A panel missing from the list is treated as no panel (= the previous behaviour)
// What is changed is the object's +0x19c "docking target y": the original relayout (main+0x3d7e1c) copies the rectangle y into it from the layout, and the per-frame update (main+0x3d7fbc) writes +0x74 interpolated as (1-t)*target during slide-in / slide-out,
// never writing +0x19c itself - so changing it does not fight the animation for the position (changing +0x74 with a position guard just gets overwritten by the animation every frame). Unit conversion: rectangle height +0x7c corresponds to layout height 244.
// Writing back is done deterministically by us (the condition for registering writes to +0x19c as an exception; the rule for original widgets is in tests/native_write_scan.py NATIVE_FIELD_OK):
// we record the original value, and as soon as the docking condition ends - panel closed, loading screen open (scene change), save and exit, back to the front end - it is written back on the spot, without relying on the game's relayout; the relayout is only a second safety net
// (current value is not what we last wrote = the game relaid it out (mode change / resolution change), taken as the new original). The message path (dm_ui_on_uimsg) writes the moment the message arrives; the per-frame path covers the rest.
// After save and exit / back to the front end, no docking until the next time a game is entered (the pause menu is still up for those few frames of exiting; without holding it, the next frame would dock to the panel state again).
// Test switch DM_TEST_NOTE_PANEL_Y=<units> tries another height
#define NOTE_PANEL_Y -310
// 停靠目标切换的那一刻框已停稳（+0x198 显示中、矩形 y +0x74 正等于旧目标）：原版停稳时不再写坐标，只改 +0x19c 它会一直留在旧位置——
// 开面板时压着背包格、关面板时压着底栏数值（两个方向都被退回过）。所以两个方向都顺手把 +0x74 一次写成新目标（登记的第二处例外，
// 只许 note_dock_snap 写）。滑入滑出期间 +0x74 不等于旧目标、不写；滑出刚开始那一帧即使相等，之后的插值用的也是已改的 +0x19c，同向不打架。
// 写完下一帧核：+0x74 还是新目标 = 没在动画期写；被改了记红（g_nd_ychk）
// At the moment the docking target switches, the box has already settled (+0x198 showing, rectangle y +0x74 exactly equal to the old target): once settled the original no longer writes the position, and changing only +0x19c leaves it at the old place -
// over the inventory cells when a panel opens, over the bottom-bar numbers when it closes (both directions were sent back). So both directions also write +0x74 to the new target in one go (the second registered exception,
// written only by note_dock_snap). During slide-in / slide-out +0x74 is not equal to the old target, so nothing is written; even if equal on the first frame of slide-out, the interpolation after that uses the already changed +0x19c, so they move the same way.
// Check on the next frame after writing: +0x74 still the new target = not written during animation; if it was changed, log red (g_nd_ychk)
static _Atomic uint64_t g_tip_fr; static int32_t g_tip_box[4];   // 说明框：最近一次提交的出图帧号、估算矩形 x0 y0 x1 y1（界面坐标；tip_note 写、ui_note_dock_pump 读，都在游戏主线程） | Description box: present frame number of the last submission and estimated rectangle x0 y0 x1 y1 (UI coordinates; written by tip_note, read by ui_note_dock_pump, both on the game main thread)
static dm_wref g_nd; static int32_t g_nd_orig, g_nd_mine; static int g_nd_on = -1, g_nd_hold;   // g_nd_hold：1 = 退出中（等出局），2 = 已出局（等进局） | g_nd_hold: 1 = leaving (waiting to be out of the game), 2 = out of the game (waiting to enter)
static int g_nd_ychk; static int32_t g_nd_ywant; static unsigned g_nd_ywr, g_nd_ybad;   // 写过 +0x74 待下一帧核（核它还是 g_nd_ywant）；累计写入次数 / 动画期写入（被改）次数 | +0x74 was written, check it next frame (that it is still g_nd_ywant); total writes / writes during animation (overwritten)
static int note_dock_snap(uint64_t w, int32_t from, int32_t to) {   // 框已停稳在 from：矩形 y 一次写成 to。返回 1 = 写了 | the box has settled at from: write the rectangle y as to in one go. Returns 1 = written
    int32_t *ry = (int32_t *)(w + 0x74); if (!*(uint8_t *)(w + 0x198) || *ry != from || from == to) return 0;
    *ry = to; g_nd_ychk = 1; g_nd_ywant = to; g_nd_ywr++; return 1;
}
static void note_dock_restore(uint64_t root, const char *why) {   // 只许游戏主线程 | game main thread only
    uint64_t w = root ? ui_panel(root, "NotificationPanel") : 0; if (!w || w != dm_wref_get(&g_nd)) return;   // 不在树里 / 不是记下的那个对象：不碰 | not in the tree / not the object we recorded: do not touch
    int32_t *t = (int32_t *)(w + 0x19c);
    if (*t == g_nd_mine && g_nd_mine != g_nd_orig) {
        *t = g_nd_orig; int still = note_dock_snap(w, g_nd_mine, g_nd_orig);
        static int lg; if (lg++ < 60) LOG("提示框停靠：%s → 写回原值 y %d%s", why, g_nd_orig, still ? "（框已停稳，矩形 y 一并写回）" : ""); }
    g_nd_mine = *t; g_nd_on = 0;
}
static void ui_note_dock_pump(uint64_t root) {
    static const char *const big[] = { "PlayerInventoryExpansionLayout", "SkillsTreePanel", "CharacterStatsPanel", "QuestLogPanelExpansion", "BankExpansionLayout", "HoradricCubeLayout",
        "WaypointsPanelExpansion", "VendorPanelLayout", "HirelingInventoryPanel", "ImbueItemsPanel", "ChroniclePanel", "MessageLogPanel", "PauseLayout", "PauseLayoutGarden",
        "SettingsPanel", "SoundOptionsPanel", "VideoOptionsPanel", "GameplayOptionsPanel" };
    static int py = 1;
    if (py > 0) { const char *e = getenv("DM_TEST_NOTE_PANEL_Y"); py = e ? atoi(e) : NOTE_PANEL_Y; }
    // 退出到前端后的挂起状态先推进、再找提示框：前端界面树里没有提示框控件，放在「找不到就返回」后面的话「已出局」那一步永远推进不到，
    // 再进局时还卡在「退出中」、停靠整局失效（查出：Mac App 启动时游戏自己发一次回到前端，停靠在 Mac 上从没生效；iOS 保存并退出再进局后同样失效）
    // Advance the pending state after exiting to the front end first, then look for the notice box: the front-end UI tree has no notice box widget, so placing this after "return if not found" means the "left game" step never advances,
    // and on re-entering the game it is still stuck in "exiting", breaking docking for the whole run (found in testing: the game itself returns to the front end once when the Mac App starts, so docking never worked on Mac; on iOS it also broke after save and exit then re-entering the game)
    int ing = dm_uiscreen_ingame(); if (g_nd_hold == 1 && !ing) g_nd_hold = 2; else if (g_nd_hold == 2 && ing) g_nd_hold = 0;
    uint64_t w = ui_panel(root, "NotificationPanel");   // 顶层面板：只扫界面根的直接子（原先全树深搜，每帧约 73k 条指令） | Top-level panels: only the direct children of the UI root are scanned (previously a full-tree depth-first search, about 73k instructions per frame)
    if (!w) { dm_wref_set(&g_nd, 0); return; }
    int32_t *t = (int32_t *)(w + 0x19c), h = *(int32_t *)(w + 0x7c);
    if (g_nd_ychk) { g_nd_ychk = 0; int32_t y = *(int32_t *)(w + 0x74); static int lg;
        if (w != dm_wref_get(&g_nd)) { if (lg++ < 60) LOG("提示框停靠：写回矩形 y 后提示框对象已换（出局 / 重建），不核"); }
        else if (y != g_nd_ywant) { g_nd_ybad++; LOG("【红】提示框停靠：矩形 y 写成 %d 后下一帧被改成 %d（动画期写入，第 %u 次）", g_nd_ywant, y, g_nd_ybad); }
        else if (lg++ < 60) LOG("提示框停靠：停稳的框下一帧在 y %d（%s；矩形 y 写入第 %u 次，动画期 %u 次）", y, y == g_nd_orig ? "原位" : "面板态位", g_nd_ywr, g_nd_ybad); }
    if (w != dm_wref_get(&g_nd) || *t != g_nd_mine) { dm_wref_set(&g_nd, w); g_nd_orig = *t; g_nd_mine = *t; }
    int panel = 0; uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60);   // 名单里的面板在屏上（可见且有可见内容）= 面板态；两种模式通用 | a panel on the list is on screen (visible and with visible content) = panel state; same for both modes
    for (uint64_t j = 0; ing && !g_nd_hold && j < n && !panel; j++) { uint64_t c = *(uint64_t *)(ch + 8 * j); const char *nm = c ? (const char *)*(uint64_t *)(c + 8) : NULL;
        if (!nm || !*(uint8_t *)(c + 0x50)) continue;
        for (unsigned i = 0; i < sizeof big / sizeof *big && !panel; i++) panel = !strcmp(nm, big[i]) && has_visible_leaf(c, 1); }
    int32_t pv = h > 0 ? (int32_t)lroundf(py * h / 244.0f) : g_nd_orig;
    // 说明框开着时提示框不压它（iOS / Mac 同一条）：面板态下这一帧或上一帧提交过说明、估算矩形与提示框相交，就把提示框挪到说明框正上 / 正下
    // （离原位近、又不出屏的那侧），说明框没了回原位。提示框中心 y = 界面高 + 目标 × 界面高 / 2160、半高 = 矩形高 × 界面高 / 4320、半宽 = 矩形宽 × 界面高 / 4320（布局单位，界面高对应 2160）
    // While the description box is open the notice box does not cover it (same for iOS / Mac): in panel state, if a description was submitted this frame or the previous one and the estimated rectangle intersects the notice box, move the notice box directly above / below the description box
    // (the side closer to the original position that stays on screen); return to the original position when the description box is gone. Notice box center y = UI height + target × UI height / 2160, half height = rect height × UI height / 4320, half width = rect width × UI height / 4320 (layout units, UI height corresponds to 2160)
    { module_t *mm = main_module(); int UW = mm ? *(int32_t *)(mm->base + 0x211da38) : 0, UH = mm ? *(int32_t *)(mm->base + 0x211da3c) : 0; uint64_t fr = dm_guest_frame(), tf = atomic_load(&g_tip_fr);
      static int moved; int32_t rw = *(int32_t *)(w + 0x78);
      if (panel && UH > 0 && h > 0 && rw > 0 && tf && fr - tf <= 2) {
          int cy = UH + (int)lroundf((float)pv * UH / 2160.0f), hh = (int)lroundf((float)h * UH / 4320.0f), hw = (int)lroundf((float)rw * UH / 4320.0f), gap = UH / 90;
          const int32_t *b = g_tip_box; int hit = UW / 2 - hw < b[2] && UW / 2 + hw > b[0] && cy - hh < b[3] + gap && cy + hh > b[1] - gap;
          if (hit) { int up = b[1] - gap - hh, dn = b[3] + gap + hh, ok_up = up - hh >= 0, ok_dn = dn + hh <= UH;
              int to = ok_up && ok_dn ? (abs(up - cy) <= abs(dn - cy) ? up : dn) : ok_up ? up : ok_dn ? dn : cy;
              if (to != cy) { pv = (int32_t)lroundf((float)(to - UH) * 2160.0f / UH); if (!moved) { moved = 1; LOG("提示框停靠：说明框占 (%d,%d)–(%d,%d) 压到提示框（中心 y %d）→ 挪到说明框%s，中心 y %d", b[0], b[1], b[2], b[3], cy, to < cy ? "上方" : "下方", to); } } }
          else moved = 0; }
      else if (moved) { moved = 0; LOG("提示框停靠：说明框没了 → 回面板态位"); } }
    if (!(panel && pv > g_nd_orig)) { if (g_nd_on) note_dock_restore(root, ing ? "面板关了" : "不在局内"); }   // 只往下挪：布局本来就不低于面板态高度时不动 | only move it down: if the layout is already no higher than the panel-state height, leave it
    else if (*t != pv) { int32_t was = *t; *t = pv; g_nd_mine = pv;
        if (note_dock_snap(w, was, pv)) { static int lg; if (lg++ < 60) LOG("提示框停靠：目标换成 %d 时框已停稳在 y %d，矩形 y 一并写成新目标", pv, was); } }
    if (panel != g_nd_on) { g_nd_on = panel; static int lg; if (lg++ < 40) LOG("提示框停靠：%s，目标 y %d（布局原值 %d，矩形高 %d）", panel ? "面板态" : "无面板", *t, g_nd_orig, h); }
}
static void ui_lift_pump(uint64_t root, uint64_t fr) {
    // 普通轮询等控件树稳定，避免下一轮布局继承已上提的坐标；加载消息（fr=0）仍当场更新。两条路都避开模式切换事务
    // Polling waits for the widget tree to settle so the next layout cannot inherit lifted coordinates; loading messages (fr=0) still update immediately. Both paths avoid mode-switch transactions
    // Polling waits for the widget tree to settle so the next layout cannot inherit lifted coordinates; loading messages (fr=0) still update immediately. Both paths avoid mode-switch transactions
    int dm_padmode_busy(void); if (dm_ui_unstable() && (fr || dm_padmode_busy())) return;
    if (fr < g_lift_next) return; g_lift_next = fr + 6;
    int x, y, w, h; dm_ui_rect(&x, &y, &w, &h); int lift = h > 0 && dm_uiscreen_ingame() ? (int)((long)y * 2160 / h) : 0;
    g_mv_want[MV_LIFT] = lift != 0; if (!lift) { mv_restore(root, MV_LIFT); return; }   // 不上提（不在局内 / 不是真全屏）：全部写回原值 | No lift (not in-game / not true fullscreen): write everything back to the original values
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60);
    for (uint64_t k = 0; ch && k < n && k < 256; k++) { uint64_t p = *(uint64_t *)(ch + 8 * k); if (!p) continue; const char *nm = (const char *)*(uint64_t *)(p + 8);
        float ay = *(float *)(p + 0x4c); int layer = *(int32_t *)(p + 0x84); if (!(ay == 0 && layer <= -100)) continue;
        int32_t y0 = *(int32_t *)(p + 0x74); mv_offset(MV_LIFT, p, 1, -lift, 0);
        if (*(int32_t *)(p + 0x74) != y0) { static int lg; if (lg++ < 16) LOG("贴顶上提：%s（层 %d）y %d → %d", nm ? nm : "?", layer, y0, *(int32_t *)(p + 0x74)); } }
}
// 珍宝匣：宝石 / 材料 / 符文三页的格位合到了宝石页那一页（布局 wide_stash.py trove_slots，底图 gen_trove_art.py），页签条上只留「个人 / 共享 / 珍宝匣 / 编年史」。
// 原版页签序号不动（2 宝石 = 珍宝匣、3 材料、4 符文、5 编年史；游戏按序号认页，改不得）：材料、符文两个页签的图挪出屏，编年史那个左移两个页签宽，都走位置守卫。
// 手柄 LB / RB 逐个序号走会落到 3、4（空页）：落上去就照来的方向替玩家再选一次（从珍宝匣一侧来 → 编年史，从编年史来 → 珍宝匣），用原版自己的选页签函数 main+0xad9b80
// Trove: the slots of the gems / materials / runes pages are merged into the gems page (layout wide_stash.py trove_slots, background gen_trove_art.py), and the tab bar keeps only "Personal / Shared / Trove / Chronicle".
// The original tab indices are untouched (2 gems = Trove, 3 materials, 4 runes, 5 Chronicle; the game identifies pages by index, so they cannot change): the materials and runes tab images are moved off screen, the Chronicle one shifts left by two tab widths, all through the position guard.
// Controller LB / RB steps index by index and would land on 3 or 4 (empty pages): when it does, select once more for the player in the direction of travel (from the Trove side → Chronicle, from Chronicle → Trove), using the game's own tab-select function main+0xad9b80
static void ui_trove_tabs_pump(module_t *m, uint64_t root, uint64_t fr) {
    static uint64_t next; if (fr < next) return; next = fr + 2;
    uint64_t bk = ui_panel(root, "BankExpansionLayout"), tabs = bk && *(uint8_t *)(bk + 0x50) ? ui_find(bk, "BankTabs", 0) : 0;
    // 只在珍宝匣布局下做（认法：符文格 r01 挂在宝石容器里 = wide_stash.py trove_slots 生效）。没有这层布局时（Mac 宿主跑的是原版五页签、三页各自还在）
    // 藏掉材料 / 符文页签就再也进不去那两页
    // Only done under the Trove layout (detected by: rune slot r01 is parented to the gem container = wide_stash.py trove_slots is in effect). Without that layout (the Mac host runs the original five tabs, with the three pages still separate)
    // hiding the materials / runes tabs would leave those two pages unreachable
    { uint64_t gm = tabs ? ui_find(bk, "advancedstash_gems", 0) : 0; if (!gm || !ui_find(gm, "r01", 0)) tabs = 0; }
    g_mv_want[MV_TROVE] = tabs != 0; if (!tabs || *(uint64_t *)tabs != (uint64_t)m->base + 0x20ba3a8) { mv_restore(root, MV_TROVE); return; }
    uint64_t ch = *(uint64_t *)(tabs + 0x58), n = *(uint64_t *)(tabs + 0x60); int x0 = 0, x1 = 0, have = 0;
    for (uint64_t i = 0; ch && i < n && i < 64; i++) { uint64_t c = *(uint64_t *)(ch + 8 * i); const char *cn = c ? (const char *)*(uint64_t *)(c + 8) : NULL; if (!cn) continue;
        if (!strcmp(cn, "Image0")) { x0 = *(int32_t *)(c + 0x70); have |= 1; } if (!strcmp(cn, "Image1")) { x1 = *(int32_t *)(c + 0x70); have |= 2; } }
    { static int lg; if (!lg++) { char b[700]; int k = 0; for (uint64_t i = 0; ch && i < n && i < 64 && k < 640; i++) { uint64_t c = *(uint64_t *)(ch + 8 * i); const char *cn = c ? (const char *)*(uint64_t *)(c + 8) : NULL; if (cn) k += snprintf(b + k, sizeof b - k, " %s(%d,%d)", cn, *(int32_t *)(c + 0x70), *(int32_t *)(c + 0x74)); }
          LOG("珍宝匣页签：页签条子控件 %llu 个：%s", (unsigned long long)n, b); } }
    if (have != 3 || x1 <= x0) return; int tw = x1 - x0;
    for (uint64_t i = 0; ch && i < n && i < 64; i++) { uint64_t c = *(uint64_t *)(ch + 8 * i); const char *cn = c ? (const char *)*(uint64_t *)(c + 8) : NULL; if (!cn) continue;
        size_t l = strlen(cn); if (!l || cn[l - 1] < '0' || cn[l - 1] > '9' || (l > 1 && cn[l - 2] >= '0' && cn[l - 2] <= '9')) continue; int k = cn[l - 1] - '0';
        if (k == 3 || k == 4) mv_offset(MV_TROVE, c, 1, 6000, 0); else if (k == 5) mv_offset(MV_TROVE, c, 0, -2 * tw, 0); }
    static int last = -1; int sel = TAB_SEL(tabs);
    // 翻页箭头（我们插回的 TouchStashLeft / Right）只有个人页、共享页用得着：珍宝匣和编年史页上挪出屏（原先一直画着，落在珍宝匣底带的石面上）
    // The paging arrows (TouchStashLeft / Right, which we put back) are only needed on the personal and shared tabs: on the Trove and Chronicle tabs they are moved off screen (they used to be drawn always, landing on the stone face of the Trove bottom band)
    for (int k = 0; k < 2; k++) { uint64_t t = ui_find(bk, k ? "TouchStashRight" : "TouchStashLeft", 0); static int32_t ox[2] = { INT32_MIN, INT32_MIN }; if (!t) continue;
        int32_t x = *(int32_t *)(t + 0x70); if (x < 50000) ox[k] = x; if (ox[k] != INT32_MIN) ui_move_ours(t, sel >= 2 ? 99999 : ox[k]); }
    if (sel == 3 || sel == 4) { int to = last >= 5 ? 2 : 5; ((void (*)(uint64_t, int, int))(m->base + 0xad9b80))(tabs, to, 0); LOG("珍宝匣页签：落在第 %d 个（空页，上一个是第 %d 个）→ 改选第 %d 个，现为 %d", sel, last, to, TAB_SEL(tabs)); last = TAB_SEL(tabs); }
    else last = sel;
}
// NPC 对话菜单（交谈 / 交易…，NpcDialogPanel）跟着 NPC 摆：位置是经典坐标（800×600 基准，面板 +0x220 / +0x224，y 再减 +0x228），游戏每帧由 main+0x3dab64 经 main+0x45b260
// 按「屏幕宽高」全局量换成界面像素、夹在屏幕内，写进面板矩形（所以直接改面板矩形留不住，试过）。真全屏下全局量的高是 16:9 的界面高 h，3D 却按真实高 H 画，
// 菜单相对 NPC 就低了一截（16:9 下在 NPC 上方，真全屏下盖在身上）。应有的位置：x' = x·H/600 +（W − 800·H/600）/2，y' = y·H/600 − 界面矩形 y；
// 游戏算的是 x·h/600 +（W − 800·h/600）/2 与 y·h/600。把经典坐标换成「经游戏这套算法后正好落在应有位置」的值：x → 400 +（x − 400）·H/h，y →（y·H/600 − 界面矩形 y）·600/h。
// 经典坐标只在菜单打开时由游戏写一次（0x3d9e08 / 0x3d9e14）；认「游戏刚写过」= 值不是我们上次写的。应有位置在界面矩形上方（负 y）时游戏会把它夹回 0，即贴着界面矩形顶边。
// The NPC dialog menu (Talk / Trade..., NpcDialogPanel) is placed following the NPC: its position is in classic coordinates (800×600 base, panel +0x220 / +0x224, with +0x228 further subtracted from y); every frame the game, from main+0x3dab64 via main+0x45b260,
// converts it to UI pixels using the "screen width/height" globals, clamps it inside the screen, and writes it into the panel rect (so changing the panel rect directly does not stick; tried). In true fullscreen the height in the globals is the 16:9 UI height h, yet 3D is drawn with the real height H,
// so the menu ends up lower relative to the NPC (above the NPC at 16:9, on top of its body in true fullscreen). Where it should be: x' = x·H/600 + (W − 800·H/600)/2, y' = y·H/600 − UI rect y;
// what the game computes is x·h/600 + (W − 800·h/600)/2 and y·h/600. Replace the classic coordinates with values that "land exactly at the intended position after going through the game's algorithm": x → 400 + (x − 400)·H/h, y → (y·H/600 − UI rect y)·600/h.
// The classic coordinates are written by the game only once when the menu opens (0x3d9e08 / 0x3d9e14); "the game just wrote it" is recognized as = the value is not what we last wrote. When the intended position is above the UI rect (negative y) the game clamps it back to 0, i.e. flush with the top edge of the UI rect.
static void ui_npcdlg_pump(uint64_t root) {
    static dm_wref pw; static int ox, oy; int rx, ry, rw, rh, W = 0, H = 0; dm_ui_rect(&rx, &ry, &rw, &rh);
    if (!dm_uiscreen_ingame() || !dm_screen_real(&W, &H) || rh <= 0 || rh >= H) { dm_wref_set(&pw, 0); return; }
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60), p = 0;
    for (uint64_t k = 0; ch && k < n && k < 256 && !p; k++) { uint64_t c = *(uint64_t *)(ch + 8 * k); const char *nm = c ? (const char *)*(uint64_t *)(c + 8) : 0; if (nm && !strcmp(nm, "NpcDialogPanel")) p = c; }
    if (!p || *(uint8_t *)(p + 0x238)) { dm_wref_set(&pw, 0); return; }   // +0x238 = 给的已是屏幕坐标，不经这套换算 | +0x238 = the given values are already screen coordinates and do not go through this conversion
    int32_t *f = (int32_t *)(p + 0x220), off = *(int32_t *)(p + 0x228); if ((p == dm_wref_get(&pw) && f[0] == ox && f[1] == oy) || (!f[0] && !f[1])) return;   // 已换过；或还没给坐标（菜单没开） | Already converted; or no coordinates given yet (menu not open)
    double lx = f[0], ly = f[1] - off; int nx = (int)lround(400 + (lx - 400) * H / rh), ny = (int)lround((ly * H / 600.0 - ry) * 600.0 / rh) + off;
    LOG("NPC 对话菜单：经典坐标 (%d,%d) → (%d,%d)（应有位置 真实画面 y %.0f）", f[0], f[1], nx, ny, ly * H / 600.0); f[0] = ox = nx; f[1] = oy = ny; dm_wref_set(&pw, p); *(uint8_t *)(p + 0x239) = 1;
}
// 游戏态点世界的入口（前端每次点世界都走这里）：先看这一下是不是点在左上角佣兵头像上——是 = 开佣兵页（新入口），这一下不再交给世界点选；
// 开法 = 替玩家按 + 开功能页、再按 R 翻到佣兵页签（ui_mercpg_pump）。不调原版键鼠的「切换佣兵页」0x36a190：它开出来的是键鼠版独立面板，没有页签条、点面板外不关（本机实测）
// 不是 = 原样转给 dm_world_tap。命中照原版右键点头像那段（main+0x3a3744–0x3a3768）：头像没被隐藏（[0x2d58cb0] != 2）、头像控件 [HireablesPanel+0x1c8] 的命中函数认这个点。
// u = 界面坐标（游戏视图归一化），各 16 位有符号、单位 1/8192（范围 ±4）：iPad 真全屏下头像随贴顶上提（ui_lift_pump）挪到游戏视图上方，u 的 y 是负的，
// 夹到 [0,1] 就点不中（iPad 全屏下实测）；w = 世界点选用的出图归一化坐标，各 15 位
// Entry point for tapping the world in the game state (every world tap from the front end goes through here): first check whether this tap is on the mercenary portrait at the top left - if yes = open the mercenary page (new entry) and this tap is not passed on to world selection;
// how it opens = press + for the player to open the function page, then press R to flip to the mercenary tab (ui_mercpg_pump). The original game's keyboard-mouse "toggle mercenary page" 0x36a190 is not called: what it opens is the keyboard-mouse standalone panel, with no tab bar and not closing on a tap outside the panel (measured on the local machine)
// if no = forward unchanged to dm_world_tap. Hit testing follows the original game's right-click-on-portrait code (main+0x3a3744–0x3a3768): the portrait is not hidden ([0x2d58cb0] != 2) and the hit-test function of the portrait widget [HireablesPanel+0x1c8] accepts this point.
// u = UI coordinates (normalized to the game view), each 16-bit signed, in units of 1/8192 (range ±4): in iPad true fullscreen the portrait moves above the game view with the top-lift (ui_lift_pump), so u's y is negative,
// and clamping to [0,1] makes the tap miss (measured in iPad fullscreen); w = normalized render coordinates used for world selection, 15 bits each
static _Atomic uint64_t g_wtap;
static _Atomic int g_mercpg;
void dm_ui_world_tap(float ux, float uy, float wx, float wy) { if (dm_restart_busy()) return;
    uint64_t q[4]; float f[4] = { ux, uy, wx, wy };
    for (int i = 0; i < 2; i++) { float c = f[i] < -3.99f ? -3.99f : f[i] > 3.99f ? 3.99f : f[i]; q[i] = (uint64_t)(uint16_t)(int16_t)lroundf(c * 8192); }
    for (int i = 2; i < 4; i++) { float c = f[i] < 0 ? 0 : f[i] > 1 ? 1 : f[i]; q[i] = (uint64_t)(c * 32767 + .5f); }
    atomic_store(&g_wtap, 1ull << 63 | q[0] << 46 | q[1] << 30 | q[2] << 15 | q[3]);
}
static void ui_wtap_pump(module_t *m, uint64_t root, int width, int height) {   // 主线程 | Main thread
    uint64_t v = atomic_exchange(&g_wtap, 0); if (!(v >> 63)) return;
    int x = (int)lround((int16_t)(v >> 46 & 0xffff) / 8192.0 * width), y = (int)lround((int16_t)(v >> 30 & 0xffff) / 8192.0 * height);
    uint64_t p = ui_find(root, "HireablesPanel", 0), hd = p && *(uint8_t *)(p + 0x50) ? *(uint64_t *)(p + 0x1c8) : 0;
    if (hd && *(int32_t *)(m->base + 0x2d58cb0) != 2 && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)hd + 0x48))(hd, (uint64_t)(uint32_t)x | (uint64_t)(uint32_t)y << 32) & 1)) {
        atomic_store(&g_mercpg, 1); LOG("界面命中：(%d,%d) → 佣兵头像，开佣兵页", x, y); return; }
    // 讲话文字框 ScrollSpeechPanel 单独开着（物件 / 任务触发的讲话，如远古人祭坛：没有 NpcDialogPanel，前端不进面板态，手指点它原先穿到世界、文字要自己滚完半分多钟）：
    // 点在框上（边框 DialogBorder 或文字 ScrollingText）= 照面板态点它的同一条路送一次界面鼠标点击——原版自己收到点击后发 ScrollingTextEnd 关框（NPC 开场白那种场合一直是这么关的）。框外的点照常交给世界
    // The speech text box ScrollSpeechPanel open on its own (speech triggered by an object / quest, e.g. the Ancients' altar: no NpcDialogPanel, the front end does not enter the panel state, a finger tap on it used to fall through to the world, and the text had to scroll by itself for over half a minute):
    // a tap on the box (the border DialogBorder or the text ScrollingText) = send one UI mouse click through the same path used for tapping it in the panel state - the original game, on receiving the click, sends ScrollingTextEnd itself and closes the box (this is how it has always been closed for NPC opening lines). Taps outside the box go to the world as usual
    { uint64_t sp = ui_find(root, "ScrollSpeechPanel", 0); static const char *part[2] = { "DialogBorder", "ScrollingText" };
      for (int k = 0; sp && *(uint8_t *)(sp + 0x50) && k < 2; k++) { uint64_t c = ui_find(sp, part[k], 0);
          if (c && *(uint8_t *)(c + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)c + 0x48))(c, (uint64_t)(uint32_t)x | (uint64_t)(uint32_t)y << 32) & 1)) {
              float ux = (v >> 45 & 0x7fff) / 32767.0f, uy = (v >> 30 & 0x7fff) / 32767.0f; dm_ui_touch(0, ux, uy); dm_ui_touch(2, ux, uy);
              LOG("界面命中：(%d,%d) → 讲话文字框 %s@ScrollSpeechPanel（单独开着），送界面点击关它", x, y, part[k]); return; } } }
    unsigned dm_world_tap(float, float); dm_world_tap((v >> 15 & 0x7fff) / 32767.0f, (v & 0x7fff) / 32767.0f);
}
// 开佣兵页：按 + 开功能页，再按 R 翻到佣兵页签（开关位 main+0x2d5ff66 = 1 为到位）。每一下按完看反馈再按下一下，同技能绑定「开技能树」那段
// Open the mercenary page: press + to open the function page, then press R to flip to the mercenary tab (toggle flag main+0x2d5ff66 = 1 means arrived). After each press, check the feedback before pressing the next, same as the "open skill tree" part of skill binding
static void ui_mercpg_pump(module_t *m, uint64_t root, uint64_t fr) {
    static int on, tries, page; static uint64_t pressed;
    if (atomic_exchange(&g_mercpg, 0) && !on) { on = 1; tries = 0; page = -1; pressed = 0; }
    if (!on) return;
    if (*(uint8_t *)(m->base + 0x2d5ff66)) { LOG("佣兵页：已开（按键 %d 次）", tries); on = 0; return; }
    if (tries >= 9) { LOG("【红】佣兵页：按了 %d 次键还没翻到，放弃", tries); on = 0; return; }
    int dm = bd_open(root, "UISwitcher"), cur = dm ? bd_top_tab(root) : -1;
    if (tries) { uint64_t dt = fr - pressed; int moved = dm && cur >= 0 && cur != page; if (!((moved && dt >= 7) || dt >= 24)) return; }
    page = cur; pressed = fr; dm_ui_key_pulse(dm ? DM_BTN_R : DM_BTN_PLUS, 4); tries++;
}
// 个人仓库页码（个人仓库 2–5 页，dm_pages.c）：写在自己的文字控件 TouchPgPage_Text 里（布局 wide_stash.py），文字照原版共享页页码的写法：
// 原版写共享页页码（0x3963e4 内 0x397098–0x39714c）= 格式串 0x7f5630({"SharedStashPage", 15}) → 0x399130(缓冲, 格式, 第几页, 共几页) → 0x136e5c(文字控件 +0x88, 缓冲, 长) → 0xadc434(文字控件) 重排；
// 原先借原版共享页那块（SharedStashTabContainer）并强制它可见、改它的字——违反「我们的功能不改原版控件状态」，已改
// Personal stash page number (personal stash pages 2–5, dm_pages.c): written in our own text widget TouchPgPage_Text (layout wide_stash.py); the text is written the way the original game writes the shared-page page number:
// the original game writes the shared-page page number (0x397098–0x39714c inside 0x3963e4) = format string 0x7f5630({"SharedStashPage", 15}) → 0x399130(buffer, format, page index, page count) → 0x136e5c(text widget +0x88, buffer, length) → 0xadc434(text widget) re-layout;
// it used to borrow the original game's shared-page block (SharedStashTabContainer), force it visible and change its text - a violation of "our features do not change the state of the original game's widgets"; changed
static void pages_text(uint8_t *b, uint64_t tb, int page, int total) {
    struct { const char *s; uint64_t n; } key = { "SharedStashPage", 15 }; uint64_t fmt = ((uint64_t (*)(void *))(b + 0x7f5630))(&key);
    char buf[256]; memset(buf, 0, sizeof buf); ((void (*)(void *, uint64_t, int64_t, int64_t))(b + 0x399130))(buf, fmt, page, total);
    ((void (*)(uint64_t, const char *, uint64_t))(b + 0x136e5c))(tb + 0x88, buf, buf[0] ? strlen(buf) : 0); ((void (*)(uint64_t))(b + 0xadc434))(tb);
}
static int g_bank_tab_keep = -1; static uint64_t g_bank_tab_until;   // 切换前储藏箱停在哪个页签（-1 = 没记）；过了这一帧就不再恢复 | Which tab the stash was on before the switch (-1 = not recorded); no restore once past this frame
static int ui_touch_or_pad_ready(void) { int dm_padmode_busy(void); return !dm_padmode_busy(); }
static void ui_pages_pump(module_t *m, uint64_t root) {   // 个人页上把自己的页码控件 TouchPgPage 挪进来写「第 n/5 页」，别的页签挪回屏外；原版共享页那块页码一律不碰（独立页面原则） | On the personal page, move our own page-number widget TouchPgPage in and write 「第 n/5 页」(Page n/5); on other tabs move it back off screen; the original game's shared-page page number is never touched (independent-page principle)
    static int lt = -1, lp = -1; uint64_t bk = ui_panel(root, "BankExpansionLayout");
    if (!bk || !*(uint8_t *)(bk + 0x50)) { lt = lp = -1; return; }
    uint64_t tabs = ui_find(bk, "BankTabs", 0), pg = ui_find(bk, "TouchPgPage", 0), tb = pg ? ui_find(pg, "TouchPgPage_Text", 0) : 0;
    // 手柄 / 触屏切换时储藏箱开着：界面重建后原版只把页签恢复到它自己记的那几页，停在第六页「编年史」的会跳回个人页。切换前记下页签（dm_ui_cache_mode_leave），
    // 重建完成后不一样就用原版自己的选页签函数选回去（不出声）。120 帧内储藏箱没再出现就作废
    // Stash open during a controller / touch switch: after the UI rebuild the original game restores the tab only to the pages it remembers itself, so one sitting on the sixth page 「编年史」(Chronicle) jumps back to the personal page. The tab is recorded before the switch (dm_ui_cache_mode_leave),
    // and after the rebuild completes, if it differs, it is selected again with the original game's own tab selection function (silently). Voided if the stash has not reappeared within 120 frames
    if (g_bank_tab_keep >= 0 && tabs && ui_touch_or_pad_ready()) { int keep = g_bank_tab_keep; g_bank_tab_keep = -1;
        int was = TAB_SEL(tabs); if (dm_guest_frame() <= g_bank_tab_until && was != keep) { ((void (*)(uint64_t, int, int))(m->base + 0xad9b80))(tabs, keep, 0); LOG("储藏箱：切换布局后页签从第 %d 个选回第 %d 个（现为 %d）", was, keep, TAB_SEL(tabs)); } }
    if (!tabs || !pg || !tb) return;
    int tab = TAB_SEL(tabs), cur; int dm_pages_cur(void), dm_pages_count(void); cur = dm_pages_cur(); uint8_t *b = (uint8_t *)m->base;
    ui_move_ours(pg, tab == 0 ? 0 : 99999);   // 矩形 +0x70，改完虚表 +0x10 重排 | Rect +0x70; after changing, vtable +0x10 re-layout
    if (tab == 0 && (lt != 0 || lp != cur)) { pages_text(b, tb, cur + 1, dm_pages_count()); lp = cur; LOG("个人仓库：页码写成第 %d/%d 页", cur + 1, dm_pages_count()); }
    if (tab != 0) lp = -1;
    lt = tab;
    // 手柄模式：翻个人仓库页原先只有触屏的左右箭头（TouchStashLeft / Right，手柄版布局里没有），第 2–5 页的东西手柄玩家够不着。
    // 个人页上接管 L / R = 上一页 / 下一页（同编年史仓库页 dm_vault_ui.c 的键位与提示条写法；ZL / ZR 仍是原版的换页签）
    // Controller mode: paging through the personal stash pages used to be possible only with the touch left / right arrows
    // (TouchStashLeft / Right, absent from the controller layout), so controller players could not reach what is on pages 2 to 5.
    // On the personal page take over L / R = previous page / next page (same key mapping and button legend approach as the Chronicle vault page
    // in dm_vault_ui.c; ZL / ZR remain the original tab switching)
    { int dm_padmode_current(void); void dm_padmode_legend(int, const char *), dm_pages_request_flip(int); uint64_t dm_padmode_page_keys(uint64_t);
      if (tab == 0 && dm_padmode_current()) { uint64_t ev = dm_padmode_page_keys(DM_BTN_L | DM_BTN_R);
          dm_padmode_legend(19, dm_str(b, "dmPadLegPrev")); dm_padmode_legend(20, dm_str(b, "dmPadLegNext"));
          if (ev & DM_BTN_L) { dm_pages_request_flip(-1); LOG("个人仓库：手柄 L → 上一页"); } else if (ev & DM_BTN_R) { dm_pages_request_flip(1); LOG("个人仓库：手柄 R → 下一页"); } } }
}
// 储藏箱加宽（编年史仓库第 1 步）：储藏箱 10 → 14 列，面板宽 392 单位。外框 ControllerOverlay 的 Background 是 14 个面板共用的一张图，
// 所以宽框是布局里另加的 WideBackground（比原框宽 392，排在 Background 之后）。游戏不认布局里的 visible，宽框平时放在屏幕外（x 99999）。
// 储藏箱开着时：宽框挪到 -196（左右各出 196，整体仍居中）、旧框不画，储藏箱面板左移 196 并加宽 392，物品栏面板右移 196，标题两个字与光标边界跟着改；关掉还原。
// 矩形 +0x70 x、+0x78 宽，改完调虚表 +0x10 重排（同安全区挪头像那处）。值不是我们上次写的 = 面板重建或游戏重排过，现值当原值
// Stash widening (chronicle vault step 1): stash 10 → 14 columns, panel 392 units wider. The outer frame ControllerOverlay's Background is a single image shared by 14 panels,
// so the wide frame is a WideBackground added separately in the layout (392 wider than the original frame, placed after Background). The game ignores visible in the layout, so the wide frame normally sits off screen (x 99999).
// While the stash is open: the wide frame moves to -196 (196 out on each side, still centered overall), the old frame is not drawn, the stash panel moves left 196 and widens by 392, the inventory panel moves right 196, and the two title texts and the cursor bounds follow; closing restores it.
// Rect +0x70 x, +0x78 width; after changing, call vtable +0x10 to re-layout (same as the safe-area portrait move). A value that is not what we last wrote = the panel was rebuilt or the game re-laid it out, so the current value is taken as the original
static void ui_widestash_pump(uint64_t root) {
    uint64_t bank = ui_panel(root, "BankExpansionLayout"); int on = bank && *(uint8_t *)(bank + 0x50); static int was = -1; g_mv_want[MV_WIDE] = on;
    if (!on && !was && !mv_count(MV_WIDE)) return;   // 关着且已还原：每帧只扫根下一层 | Closed and already restored: only scan one level under the root each frame
    uint64_t ov = ui_panel(root, "ControllerOverlay"); if (!ov || !ui_find(ov, "WideBackground", 0)) { was = 0; return; }   // 资源里没有宽框：不动 | No wide frame in the assets: leave it alone
    if (on != was) { LOG("储藏箱加宽：%s", on ? "开（宽外框、物品栏右移）" : "关（还原）"); was = on; }
    // 标题条 Tabs 两等分，改它的宽不会重分（实测）；直接改它的两个字控件：Text0 原 x 0 宽 1440 → 左半 -196..1636，Text1 原 x 1440 → 右半 1636..3076
    // The title bar Tabs splits in two equal halves, and changing its width does not re-split (measured); change its two text widgets directly: Text0 originally x 0 width 1440 → left half -196..1636, Text1 originally x 1440 → right half 1636..3076
    uint64_t tb = ui_find(ov, "Tabs", 0);
    // 标题条两端的端帽挂在居中 2880 宽的容器上，各往外挪 196
    // The end caps at both ends of the title bar hang on a centered 2880-wide container; each moves outward by 196
    static const struct { const char *nm; int in, dx, dw; } t[] = { { "WideBackground", 1, -196 - 99999, 0 }, { "Text0", 2, -196, 392 }, { "Text1", 2, 196, 0 },
        { "TabsEndcapLeft", 1, -196, 0 }, { "TabsEndcapRight", 1, 196, 0 }, { "ControllerCursorBounds", 1, -196, 392 }, { "BankExpansionLayout", 0, -196, 392 }, { "PlayerInventoryExpansionLayout", 0, 196, 0 } };
    if (!on) { mv_restore(root, MV_WIDE); return; }
    for (int i = 0; i < 8; i++) { uint64_t w = t[i].in == 2 ? (tb ? ui_find(tb, t[i].nm, 0) : 0) : t[i].in ? ui_find(ov, t[i].nm, 0) : ui_panel(root, t[i].nm); if (!w) continue;
        float k = t[i].in ? 1.0f : *(float *)(w + 0x80);   // 面板矩形挪 1 单位，画面只挪 1 / 面板缩放（+0x80，iOS 档 1.2）个外框单位：照外框单位写只挪到 5/6（叉偏右 33、面板边对不上框） | Moving the panel rect by 1 unit moves the picture by only 1 / panel scale (+0x80, 1.2 in the iOS mode) outer-frame units: writing in outer-frame units only moves it 5/6 of the way (the X is off to the right by 33 and the panel edge does not line up with the frame)
        mv_offset(MV_WIDE, w, 0, (int)lroundf(t[i].dx * k), 1); mv_offset(MV_WIDE, w, 2, (int)lroundf(t[i].dw * k), 1); }
}
// 右上角内移到安全线内（跟运行时安全区，不写死机型）：前端把右安全区换算成布局单位（2160 高）传进来。
// 小地图：直接改 AutoMap 面板矩形 x（+0x70）。面板对象里的 miniOffsetRight（+0x15cc，登记处 main+0xc0ce18）运行时改了不生效：
// 它只在面板载入时用一次，之后切档重排也不再读（写了字段、切全屏再切回，矩形仍是 -1278）；
// 信息栏（区域名）：HUDWarnings 面板矩形 x（+0x70，原值记下）= 原值 - 单位数。每 6 帧对一次（面板重建 / 切档后重新写）。
// Move the top-right corner inside the safe line (follows the runtime safe area, no hard-coded device models): the front end converts the right safe area into layout units (2160 high) and passes it in.
// Minimap: change the AutoMap panel rect x (+0x70) directly. miniOffsetRight in the panel object (+0x15cc, registered at main+0xc0ce18) has no effect when changed at runtime:
// it is used only once when the panel loads, and is not read again when re-laid out on a mode switch (after writing the field and switching to fullscreen and back, the rect is still -1278);
// Info bar (area name): HUDWarnings panel rect x (+0x70, original value recorded) = original value - units. Checked every 6 frames (rewritten after a panel rebuild / mode switch).
static _Atomic int g_safe_r;
static _Atomic int g_safe_l;   // 左安全区：左上角的佣兵 / 队友头像栏往右让 | Left safe area: the mercenary / party member portrait bar at the top left gives way to the right
void dm_ui_safe_left(int units) { atomic_store(&g_safe_l, units < 0 ? 0 : units > 800 ? 800 : units); }
void dm_ui_safe_right(int units) { atomic_store(&g_safe_r, units < 0 ? 0 : units > 800 ? 800 : units); }
static void ui_safe_pump(module_t *m, uint64_t root, uint64_t fr) {
    static uint64_t next; if (fr < next) return; next = fr + 6; int u = atomic_load(&g_safe_r);   // 切全屏再切回时游戏把矩形写回贴边，6 帧内补回 | After switching to fullscreen and back the game writes the rect back to the edge; restored within 6 frames
    int ul0 = atomic_load(&g_safe_l); g_mv_want[MV_SAFE] = dm_uiscreen_ingame() && (u || ul0);   // 条件：在局内且有安全区。出局（退出到前端）全部写回 | Condition: in-game and there is a safe area. On leaving the run (exit to the front end) everything is written back
    if (!g_mv_want[MV_SAFE]) { mv_restore(root, MV_SAFE); return; }
    uint64_t w = ui_panel(root, "AutoMap");
    if (w && *(uint64_t *)w == (uint64_t)m->base + 0x20c58a0) {   // 微缩放右那一档：面板矩形贴画面右边（x = -宽）；内移 = 再往左 u | The mini-on-the-right mode: the panel rect hugs the right edge of the screen (x = -width); moving inward = a further u to the left
        int cfg = *(volatile int32_t *)(m->base + 0x2d479f4), mini = *(volatile int32_t *)(m->base + 0x2d5c748); int32_t *rx = (int32_t *)(w + 0x70), ww = *(int32_t *)(w + 0x78);
        int on = cfg == 2 && mini && ww > 0 && ww < 4000; int32_t x0 = *rx;
        // 内移量从「贴边」（x = -宽）算，不从控件此刻的值算：手柄 / 触屏切换让游戏把界面重建两遍，第二遍建出来的面板矩形带着我们上一遍挪过的值（实跑：-1564 → -2002 之后又 -2002 → -2440），
        // 照「现值当原值」再挪一次，小地图就往左多挪了一个安全区、切回触屏也不回来。所以把原位（贴边）交给 mv_offset，由它按原位 + 内移量写
        // The inward shift is computed from "flush against the edge" (x = -width), not from the widget's current value: switching between
        // controller and touch makes the game rebuild the UI twice, and the panel rectangle built the second time carries the value we shifted
        // on the previous pass (real run: -1564 -> -2002, and then again -2002 -> -2440).
        // Shifting once more with "current value taken as the original value" moved the minimap one extra safe area to the left, and it did not
        // come back when switching to touch again. So the original position (flush against the edge) is handed to mv_offset, which writes
        // original position + inward shift.
        g_mv_base = -ww; g_mv_base_on = on;   // 原位 = 贴边（经带守卫的 mv_offset 写，不直接写控件） | Original position = flush against the edge (written through the guarded mv_offset, not directly to the widget)
        mv_offset(MV_SAFE, w, 0, on ? -u : 0, 0); g_mv_base_on = 0; if (*rx != x0) LOG("安全区：小地图矩形 x %d → %d", x0, *rx);
    }
    // 头像栏 HireablesPanel：每个头像是建面板时从 Template 克隆出来的子控件（main+0x3a2c50 起的循环），位置在建的那一刻写进子控件矩形（0x3a2d98）、
    // 紧接着调一次子控件虚表 +0x10（0x3a2d94–0x3a2da4）让它按新矩形重算；之后不再排版。所以运行时光改面板矩形 / 子控件矩形画面不动，
    // 改完子控件矩形要照原版补那一次虚表 +0x10。原值记下，x = 原值 + 左安全区单位数
    // Portrait bar HireablesPanel: each portrait is a child widget cloned from Template when the panel is built (the loop starting at main+0x3a2c50); its position is written into the child widget's rect at build time (0x3a2d98),
    // immediately followed by one call to the child widget's vtable +0x10 (0x3a2d94–0x3a2da4) so it recomputes for the new rect; there is no further layout after that. So at runtime, changing only the panel rect / child widget rect does not move the picture;
    // after changing the child widget rect, that one vtable +0x10 call must be made as the original game does. The original value is recorded; x = original value + left safe-area units
    { uint64_t p = ui_panel(root, "HireablesPanel"); int ul = atomic_load(&g_safe_l);
      uint64_t ch = p ? *(uint64_t *)(p + 0x58) : 0, n = p ? *(uint64_t *)(p + 0x60) : 0;
      for (uint64_t k = 0; ch && k < n && k < 40; k++) { uint64_t c = *(uint64_t *)(ch + 8 * k); if (!c) continue; int32_t x0 = *(int32_t *)(c + 0x70);
          mv_offset(MV_SAFE, c, 0, ul, 1); if (*(int32_t *)(c + 0x70) != x0) { static int lg; if (lg++ < 6) LOG("安全区：头像 %d x %d → %d", (int)k, x0, *(int32_t *)(c + 0x70)); } } }
    uint64_t h = ui_panel(root, "HUDWarnings");
    if (h) { int32_t x0 = *(int32_t *)(h + 0x70); mv_offset(MV_SAFE, h, 0, -u, 0); if (*(int32_t *)(h + 0x70) != x0) LOG("安全区：信息栏 x %d → %d", x0, *(int32_t *)(h + 0x70)); }

}
// 小地图任务终点（dm_world.c dm_map_goals）用的地图几何：地图开没开、全屏还是微缩、每格像素。每格像素照游戏自己的算法（同 ui_map_center：0x413280–0x413298），
// 单位是真实画面像素；地图上的世界格 (x, y) 画在 玩家所在点 + ((Δx−Δy)×1.6, (Δx+Δy)×0.8) × 每格像素（宿主峡谷 4 组墓口拟合，误差 ≤3%）。
// 开没开看 AutoMap 控件可见位（+0x50）；全屏 / 微缩看 [main+0x2d5c748]（0 = 全屏，同 ui_map_pump）；微缩地图的玩家点在小地图框中心、缩放用另一档（+0x15ac）。主线程每帧写一次
// Map geometry used for the minimap quest goals (dm_world.c dm_map_goals): whether the map is open, fullscreen or mini, and pixels per cell. Pixels per cell follows the game's own algorithm (same as ui_map_center: 0x413280–0x413298),
// in real screen pixels; world cell (x, y) on the map is drawn at the player's point + ((Δx−Δy)×1.6, (Δx+Δy)×0.8) × pixels per cell (fitted on the host from 4 sets of tomb entrances in the canyon, error ≤3%).
// Open or not is read from the AutoMap widget's visible flag (+0x50); fullscreen / mini from [main+0x2d5c748] (0 = fullscreen, same as ui_map_pump); the mini map's player point is at the center of the minimap frame and its zoom uses the other level (+0x15ac). Written once per frame by the main thread
static _Atomic uint64_t g_mapgeom;   // 高 32 位 = 模式（0 没开 / 1 微缩 / 2 全屏），低 32 位 = 每格像素（float 的位） | High 32 bits = mode (0 not open / 1 mini / 2 fullscreen), low 32 bits = pixels per cell (bits of a float)
static void ui_map_geom_pump(module_t *m, uint64_t root) {
    uint64_t am = ui_panel(root, "AutoMap"); int mode = 0; float cell = 0;
    if (am && *(uint64_t *)am == (uint64_t)m->base + 0x20c58a0 && *(uint8_t *)(am + 0x50)) {
        mode = *(volatile int32_t *)(m->base + 0x2d5c748) == 0 ? 2 : 1;
        float q = ceilf(((float (*)(uint64_t))(m->base + 0x390b38))(am) * *(float *)(am + (mode == 2 ? 0x15a8 : 0x15ac)) * 8) / 8; if (q < 0.125f) q = 0.125f;   // 全屏 / 微缩各一档缩放（0xc0df58 / 0xc0de54；实测 1.0 / 0.5） | One zoom level each for fullscreen / mini (0xc0df58 / 0xc0de54; measured 1.0 / 0.5)
        cell = q * *(float *)(m->base + 0x2d5e918); }
    uint32_t cb; memcpy(&cb, &cell, 4); atomic_store(&g_mapgeom, (uint64_t)mode << 32 | cb);
}
int dm_ui_map_geom(float *cell) { uint64_t v = atomic_load(&g_mapgeom); uint32_t cb = (uint32_t)v; memcpy(cell, &cb, 4); return (int)(v >> 32); }
// 顶部首领血条的百分比（接在血条右边同一行，取整）。布局覆盖层把手柄版挂在目标头顶的血条挪到了顶部，并在条右边加了空字的 TouchBossPct
// （dm/ios/tools/touch_layout.py _boss_bar_top）；这里每帧把进度条控件（HealthBarWidget，虚表 main+0x2089650）自己的比例 +0x154 写成「NN%」——
// 与条同一个数（游戏已按整百分比截断：客户端生命 126/128 = 98.4% → 0.98），不另算。面板没开 / 控件不在（Mac 宿主跑原版布局）什么都不做。
// Percentage for the top boss health bar (on the same line to the right of the bar, rounded to an integer). The layout overlay moved the controller version's health bar, which hangs above the target's head, to the top, and added an empty-text TouchBossPct to the right of the bar
// (dm/ios/tools/touch_layout.py _boss_bar_top); here, every frame, the progress bar widget's (HealthBarWidget, vtable main+0x2089650) own ratio at +0x154 is written out as "NN%" -
// the same number as the bar (the game has already truncated to whole percent: client life 126/128 = 98.4% → 0.98), not computed separately. If the panel is not open / the widget is absent (the Mac host runs the original game's layout), do nothing.
static void ui_boss_pct_pump(module_t *m, uint64_t root) {
    uint64_t panel = ui_panel(root, "HUDMonsterHealth"); if (!panel || !*(uint8_t *)(panel + 0x50)) return;
    uint64_t hp = ui_find(panel, "Health", 1), tx = hp ? ui_find(hp, "TouchBossPct", 1) : 0;
    if (!tx || *(uint64_t *)hp != (uint64_t)m->base + 0x2089650 || *(uint64_t *)tx != (uint64_t)m->base + 0x20ba4c8) return;
    float v = *(float *)(hp + 0x154); if (!(v >= 0 && v <= 1)) return;
    char t[8]; int n = snprintf(t, sizeof t, "%d%%", (int)lroundf(v * 100));
    if (*(uint64_t *)(tx + 0x90) == (uint64_t)n && !memcmp(*(const char **)(tx + 0x88), t, (size_t)n)) return;   // 字没变不写（同 dm_update.c set_text） | Do not write if the text is unchanged (same as dm_update.c set_text)
    ((void *(*)(uint64_t, const char *, uint64_t))(m->base + 0x136e5c))(tx + 0x88, t, (uint64_t)n); ((void (*)(uint64_t))(m->base + 0xadc434))(tx);
}
// 来自 hubeiqiao 的 （公开仓库 诊断）
// 只在半秒采样中状态变化时记录小地图；不写控件或可见位，缺对象也要留下证据。
// From hubeiqiao (public repo diagnostics)
// Records the minimap only when its state changes in the half-second sampling; never writes widgets or visibility bits, and leaves evidence even when objects are missing.
typedef struct { uint64_t object; int valid, area, ingame, load, cfg, mini, visible, x, y, w, h, ux, uy, uw, uh, rw, rh; } map_diag_t;
static map_diag_t g_map_diag; static int g_map_diag_have;
static int ui_map_diag(module_t *m, uint64_t root) {
    map_diag_t d; memset(&d, 0, sizeof d); dm_hud_info hi; dm_hud_get(&hi); d.valid = hi.valid; d.area = hi.area;
    d.ingame = dm_uiscreen_ingame(); d.load = atomic_load(&g_load_up);
    d.cfg = *(int32_t *)(m->base + 0x2d479f4); d.mini = *(int32_t *)(m->base + 0x2d5c748);
    dm_ui_rect(&d.ux, &d.uy, &d.uw, &d.uh); dm_screen_real(&d.rw, &d.rh);
    uint64_t am = ui_panel(root, "AutoMap");
    if (am && *(uint64_t *)am == (uint64_t)m->base + 0x20c58a0) { d.object = am; d.visible = *(uint8_t *)(am + 0x50);
        d.x = *(int32_t *)(am + 0x70); d.y = *(int32_t *)(am + 0x74); d.w = *(int32_t *)(am + 0x78); d.h = *(int32_t *)(am + 0x7c); }
    if (g_map_diag_have && !memcmp(&d, &g_map_diag, sizeof d)) return 0;
    int rebuilt = !g_map_diag_have || d.object != g_map_diag.object; memcpy(&g_map_diag, &d, sizeof d); g_map_diag_have = 1;
    LOG("小地图诊断：区域 %d 玩家有效 %d 局内 %d 读取 %d 对象 %s 重建 %d 可见 %d 配置 %d 微缩 %d 矩形 %d,%d,%d,%d 界面 %d,%d,%d,%d 真实 %dx%d", d.area, d.valid, d.ingame, d.load, d.object ? "有" : "无", rebuilt, d.visible, d.cfg, d.mini, d.x, d.y, d.w, d.h, d.ux, d.uy, d.uw, d.uh, d.rw, d.rh);
    return 1;
}
static void ui_minimap_pump(module_t *m, uint64_t root, int W, int H, uint64_t fr) {
    static uint64_t next; if (fr < next) return; next = fr + 30;
    ui_map_diag(m, root);
    uint64_t w = ui_panel(root, "AutoMap"); if (!w || *(uint64_t *)w != (uint64_t)m->base + 0x20c58a0) return;
    int32_t x = *(int32_t *)(w + 0x70), y = *(int32_t *)(w + 0x74), ww = *(int32_t *)(w + 0x78), hh = *(int32_t *)(w + 0x7c); float sc = *(float *)(w + 0x80);
    if (ww <= 0 || hh <= 0 || ww > 4000 || hh > 2160 || !(sc > 0.1f && sc < 10)) return;
    double u = H / 2160.0, x0 = (x < 0 ? W + x * u : x * u), y0 = y * u, x1 = x0 + ww * sc * u, y1 = y0 + hh * sc * u;   // x 为负 = 从画面右边往左量 | Negative x = measured leftward from the right edge of the screen
    if (x0 < 0) x0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H; if (x1 - x0 < 8 || y1 - y0 < 8) return;
    // 四个量各 16 位有符号、单位 1/8192 个界面矩形（范围 ±4）：贴顶上提后小地图在界面矩形上方，y 是负的；原先按无符号 ×65535 打包，负值存不进，热区只剩界面矩形里露出的那一条（iOS 包）
    // Four values, each 16-bit signed, in units of 1/8192 of the UI rect (range ±4): after the top-lift the minimap is above the UI rect, so y is negative; it used to be packed as unsigned ×65535, negative values could not be stored, and the hot zone was only the strip showing inside the UI rect (iOS app)
    #define MM_Q(val) ((uint64_t)(uint16_t)(int16_t)lround((val) * 8192.0))
    uint64_t v = MM_Q(x0 / W) << 48 | MM_Q(y0 / H) << 32 | MM_Q(x1 / W) << 16 | MM_Q(y1 / H);
    #undef MM_Q
    int chg = !atomic_load(&g_mm_have) || atomic_load(&g_mm_rect) != v; atomic_store(&g_mm_rect, v); atomic_store(&g_mm_have, 1);
    if (chg) { float r[4]; dm_ui_minimap_rect(r); LOG("小地图矩形：控件 (%d,%d,%d,%d) 缩放 %.2f → 画面 %dx%d 里 x %.0f–%.0f、y %.0f–%.0f；前端读到 x %.0f–%.0f、y %.0f–%.0f", x, y, ww, hh, sc, W, H, x0, x1, y0, y1, r[0] * W, r[2] * W, r[1] * H, r[3] * H); }
}
// 测试量具（触点脚本 tap_ui <控件名>[@<面板名>]）：盯住一个界面控件，主线程每帧把它的中心（游戏视图归一化）发布出来，脚本线程读了去落真实触点。
// 贴屏幕边的控件（选角页右列等）在不同宽高比的屏上位置不同，裸坐标脚本换一台设备就点偏（DM_TOUCH_REF 的换算只对居中的界面成立）；按名字找就同一份脚本到处能用。
// 位置用控件自己的命中函数在画面上取样求得（128×72 个点里命中的那些取重心，再取离重心最近的命中点——控件没有现成的矩形可读）；只认可见的控件。
// Test instrument (touch point script tap_ui <widget name>[@<panel name>]): watch one UI widget; the main thread publishes its center (normalized to the game view) every frame, and the script thread reads it to land a real touch point.
// Widgets hugging the screen edge (the right column of the character select page, etc.) sit at different positions on screens of different aspect ratios, so a raw-coordinate script taps off target on another device (the DM_TOUCH_REF conversion only holds for centered UI); finding by name lets the same script work everywhere.
// The position is obtained by sampling the screen with the widget's own hit-test function (centroid of the hits among 128×72 points, then the hit point nearest the centroid - the widget has no ready-made rect to read); only visible widgets are recognized.
static char g_wn[2][64]; static _Atomic int g_wn_on; static _Atomic uint64_t g_wn_pos;
void dm_ui_watch(const char *spec) {
    atomic_store(&g_wn_on, 0); atomic_store(&g_wn_pos, 0); if (!spec) return;
    const char *at = strchr(spec, '@'); snprintf(g_wn[0], sizeof g_wn[0], "%.*s", at ? (int)(at - spec) : 63, spec); snprintf(g_wn[1], sizeof g_wn[1], "%s", at ? at + 1 : "");
    atomic_store(&g_wn_on, 1);
}
int dm_ui_watch_pos(float *nx, float *ny) { uint64_t v = atomic_load(&g_wn_pos); if (!v) return 0; *nx = ((v >> 16) & 0xffff) / 65535.0f; *ny = (v & 0xffff) / 65535.0f; return 1; }
static uint64_t vis_find(uint64_t w, const char *name, int dep) {   // 按名字找可见控件，后画的（上层）优先 | Find a visible widget by name; later-drawn (upper-layer) ones take priority
    if (!w || dep > 14 || !*(uint8_t *)(w + 0x50)) return 0;
    const char *nm = (const char *)*(uint64_t *)(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (int i = (int)n - 1; i >= 0; i--) { uint64_t r = vis_find(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
// 控件在画面上的中心（游戏视图归一化）：控件没有现成的矩形可读，用它自己的命中函数在 128×72 个点上取样，命中点取重心，再取离重心最近的命中点。0 = 一个点都没命中
// Center of a widget on screen (normalized to the game view): the widget has no ready-made rect to read, so sample 128×72 points with its own hit-test function, take the centroid of the hit points, then take the hit point nearest the centroid. 0 = not a single point hit
static int ui_widget_center(uint64_t t, int w, int h, float *nx, float *ny) {
    int (*hit)(uint64_t, uint64_t) = (int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)t + 0x48);
    enum { GX = 128, GY = 72 }; static uint8_t on[GY][GX]; double sx = 0, sy = 0; int n = 0;
    for (int j = 0; j < GY; j++) for (int i = 0; i < GX; i++) { int x = (int)((i + 0.5) * w / GX), y = (int)((j + 0.5) * h / GY); on[j][i] = hit(t, SNAP_XY(x, y)) & 1; if (on[j][i]) { sx += x; sy += y; n++; } }
    if (!n) return 0;
    sx /= n; sy /= n; double best = 1e18; int bx = 0, by = 0;
    for (int j = 0; j < GY; j++) for (int i = 0; i < GX; i++) if (on[j][i]) { double x = (i + 0.5) * w / GX, y = (j + 0.5) * h / GY, d = (x - sx) * (x - sx) + (y - sy) * (y - sy); if (d < best) { best = d; bx = (int)x; by = (int)y; } }
    *nx = (float)((bx + 0.5) / w); *ny = (float)((by + 0.5) / h); return 1;
}
static void ui_watch_pump(void) {   // 主线程，每帧；只在盯着的时候干活 | Main thread, every frame; only does work while watching something
    if (!atomic_load(&g_wn_on)) return;
    module_t *m = main_module(); if (!m) return;
    int32_t w = *(int32_t *)(m->base + 0x211da38), h = *(int32_t *)(m->base + 0x211da3c); uint64_t root = 0; memcpy(&root, m->base + 0x2f05458, 8);
    if (w <= 0 || h <= 0 || !root) return;
    uint64_t p = g_wn[1][0] ? vis_find(root, g_wn[1], 0) : root, t = p ? vis_find(p, g_wn[0], 0) : 0;
    if (!t) { atomic_store(&g_wn_pos, 0); return; }
    float nx, ny; if (!ui_widget_center(t, w, h, &nx, &ny)) { atomic_store(&g_wn_pos, 0); return; }
    uint64_t v = ((uint64_t)(uint32_t)(nx * 65535) << 16) | (uint32_t)(ny * 65535);
    atomic_store(&g_wn_pos, v ? v : 1);
}
// 面板状态安全阀。前端的「有没有面板开着」来自 PanelManager 的开关消息；根修在前端（按开 / 没开记，见 front_ios.m
// hud_stack_message）。这里只兜一种状态：消息上还有面板开着，而这些面板在游戏面板树里一个都不可见，连续 1.5 秒——屏上已经没有面板、
// 前端却按面板态（卡死的样子）。这时给订阅者把它们补发 ClosePanel 并打【红】行（它一出手就该去查那段开关序列，不许静默抹平）。
// 不逐个面板判：面板被自己的子页盖住时本来就不可见、过后要回来（暂停菜单被编年史 / 设置页盖住，对话菜单被商人页 / 雇佣列表盖住），
// 常驻层消息上开着也可以没有可见内容（圣骑光环栏、佣兵头像栏）——这两种第一、二版都误报过。只要还有任何一个开着的面板在屏上就不动。
// 守的是一条整体不变式：前端处于面板态 ⇒ 游戏面板树里此刻至少有一个开着的非常驻面板可见。只在局内核
// 局内常驻层 = 营地无操作时 OpenPanel 后一直不关的 13 个（实测 UI 消息流）+ 存档提示 / 通知 / 组队消息 / 帮助按钮 / 手柄提示层 / 自动地图。
// 前端的面板栈和这里的安全阀都不把它们算作「有面板开着」
// Panel state safety valve. The front end's "is any panel open" comes from PanelManager's open/close messages; the root fix is in the front end (tracked as open / not open, see front_ios.m
// hud_stack_message). This only backstops one state: the messages still say panels are open, yet none of those panels is visible in the game's panel tree, for 1.5 s straight - there is no panel on screen any more but
// the front end is still in the panel state (it looks frozen). In that case re-send ClosePanel for them to subscribers and print a 【红】(red) line (whenever it fires, go check that open/close sequence; silently smoothing it over is not allowed).
// It does not judge panel by panel: a panel covered by its own sub-page is legitimately invisible and comes back later (the pause menu covered by the chronicle / settings page, the dialog menu covered by the vendor page / hire list),
// and a persistent layer that is open per the messages may also have no visible content (the paladin aura bar, the mercenary portrait bar) - the first and second versions both false-alarmed on these two. As long as any open panel is still on screen, do nothing.
// What it guards is one overall invariant: the front end is in the panel state ⇒ at least one open non-persistent panel is visible in the game's panel tree right now. Checked only in-game
// In-game persistent layers = the 13 that stay open after OpenPanel when idle in camp (measured UI message stream) + save prompt / notifications / party messages / help button / controller hint layer / automap.
// Neither the front end's panel stack nor the safety valve here counts them as "a panel is open"
int dm_ui_panel_resident(const char *name) {
    static const char *k[] = { "EmoteWheelPanel", "HUDLevelNames", "HUDMessagePanel", "HUDMonsterHealth", "HUDPanel", "HUDWarnings", "HireablesPanel",
        "InGameMoviePlayerPanel", "MiniMenuPanel", "PaladinAuraPanel", "PanelBordersPanel", "ShowItemsPanel", "TooltipsPanel",
        "SaveIndicatorPanel", "NotificationPanel", "PartyPanelMessage", "HelpButtonPanel", "ControllerOverlay", "UISwitcher", "AutoMap",
        // 输入法候选列表层：第一次有文字输入框（编年史的搜索框）时随之 OpenPanel，之后游戏从不关它（33 份日志里开 61 次、关 / 卸 0 次），
        // 面板树里它一直可见（框与滚动箭头可见、没有候选词），面板树也判不出「其实没画东西」。原先进过一次编年史，前端就一直按面板态：
        // 退回游戏后摇杆 / 技能钮 / 底栏全没、推摇杆不动，安全阀也因它「可见」而不收
        // IME candidate list layer: it gets an OpenPanel the first time a text input box appears (the chronicle's search box), and the game never closes it afterwards (61 opens, 0 closes / unloads across 33 logs);
        // it stays visible in the panel tree (the box and scroll arrows are visible, with no candidates), and the panel tree cannot tell that "nothing is actually drawn" either. Previously, once the chronicle had been entered, the front end stayed in the panel state forever:
        // after returning to the game the stick / skill buttons / bottom bar were all gone and pushing the stick did nothing, and the safety valve did not clear it either because it was "visible"
        "CandidateListPanel" };
    if (!name) return 0; for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strcmp(name, k[i])) return 1; return 0;
}
// 面板开合栈（前端「面板模式」的依据，原在 front_ios.m hud_stack_message；搬来，iOS 前端与 Mac touch_mac.c 在各自的界面消息回调里调同一份）。
// 底部 UI v4 的 UIKit 底栏只在「局内常态」画：维护面板开合栈，常驻 HUD 层（dm_ui_panel_resident）之外任何面板开着就不画、前端进面板态。
// 返回栈读数：开着的非常驻面板数；只开着 NPC 对话菜单 / 说话框时 -1。日志字样「[hud] 面板栈 …」保持原样（回归脚本按它认）
// Panel open/close stack (the basis of the front end's "panel mode"; formerly front_ios.m hud_stack_message, moved here so the iOS front end and Mac touch_mac.c call the same one from their UI message callbacks).
// The UIKit bottom bar of bottom UI v4 is only drawn in the normal in-game state: maintain the panel stack; whenever any panel other than the resident HUD layers (dm_ui_panel_resident) is open, it is not drawn and the front end enters panel mode.
// Returns the stack reading: number of open non-resident panels; -1 when only the NPC dialog menu / speech box is open. The log text "[hud] 面板栈 ..." is kept as is (regression scripts match on it)
enum { HUD_STACK_MAX = 64 };
static struct { char name[64]; int open; } g_hud_stack[HUD_STACK_MAX];
__attribute__((weak)) void dm_platform_menu_ready(void) {}   // 空的弱定义（不是弱声明）：苹果链接器要求弱引用链接时有定义；Vulkan 后端 / 鸿蒙平台层给强定义 | empty weak definitions (not declarations): Apple's linker needs a weak reference to resolve; the Vulkan backend / HarmonyOS platform give the strong ones
static void hud_stack_update(const char *act, const char *arg) {
    // 读取画面撤下：清掉此刻在游戏面板树里已经看不见的记录（防只开不关的漏配），屏上还在的照记——
    // 通关页是读取画面撤下前 0.5 秒开的（OpenExclusivePanel EndScreenPanelExpansion → UnloadPanel LoadScreenPanel），原先整栈清零把它也清了：
    // 底栏和技能钮盖在通关页上、点「好」变成点世界
    // Loading screen removed: clear the records no longer visible in the game's panel tree at this moment (guards against open-without-close mismatches), and keep those still on screen --
    // the game-completion page is opened 0.5 seconds before the loading screen is removed (OpenExclusivePanel EndScreenPanelExpansion → UnloadPanel LoadScreenPanel); previously the whole stack was zeroed, clearing it too:
    // the bottom bar and skill buttons covered the game-completion page, and tapping 「好」(OK) became a world tap
    if(!strcmp(act,"UnloadPanel") && !strcmp(arg,"LoadScreenPanel")) {
        for(int i=0;i<HUD_STACK_MAX;i++) {
            if(g_hud_stack[i].open>0 && strcmp(g_hud_stack[i].name,"LoadScreenPanel") && dm_ui_panel_shown(g_hud_stack[i].name)) fprintf(stderr,"[hud] 面板栈 读取画面撤下：%s 屏上还在，照记\n",g_hud_stack[i].name);
            else memset(&g_hud_stack[i],0,sizeof g_hud_stack[i]);
        }
    }
    else if(!strcmp(act,"UnloadAll")) memset(g_hud_stack,0,sizeof g_hud_stack);
    else {
        // PanelManager 的面板动作全集（读码：它的消息分发 main+0xb1dc60 只认这 8 个，与全部日志里出现过的一致）：
        //   OpenPanel / OpenExclusivePanel = 开；ClosePanel / UnloadPanel = 关（读取画面那条见上）；TogglePanel = 按当前记录翻转（高级属性页走它）；
        //   UnloadAll = 全清（见上）；LoadPanel = 预载、不上屏，不算开关；HideGameInfo = 隐藏游戏信息层，与面板开关无关
        // Full set of PanelManager panel actions (from reading the code: its message dispatch main+0xb1dc60 only recognizes these 8, matching what has appeared across all logs):
        //   OpenPanel / OpenExclusivePanel = open; ClosePanel / UnloadPanel = close (for the loading screen one see above); TogglePanel = flip according to the current record (the advanced stats page uses it);
        //   UnloadAll = clear all (see above); LoadPanel = preload without showing, not counted as open or close; HideGameInfo = hide the game info layer, unrelated to panel open / close
        int tog=!strcmp(act,"TogglePanel");
        int open=!strcmp(act,"OpenPanel")||!strcmp(act,"OpenExclusivePanel"), close=!strcmp(act,"ClosePanel")||!strcmp(act,"UnloadPanel");
        if(!open && !close && !tog) return;
        if(dm_ui_panel_resident(arg)) return;
        int slot=-1; for(int i=0;i<HUD_STACK_MAX;i++) if(!strcmp(g_hud_stack[i].name,arg)) { slot=i; break; }
        if(slot<0) for(int i=0;i<HUD_STACK_MAX;i++) if(!g_hud_stack[i].name[0]) { slot=i; strlcpy(g_hud_stack[i].name,arg,sizeof g_hud_stack[i].name); break; }
        if(slot<0) return;
        if(tog) { open=g_hud_stack[slot].open<=0; close=!open; }
        // 没开着就收到关闭 = 空操作，不是漏配：游戏开功能页 / 切页签时对一批互斥面板（仓库、方块、商店、小站、对话…）统一发 ClosePanel，不管开没开；
        // 启动时选角页也是先关再开（读界面消息流水；之前记成红，玩家路径每次启动 14 条假红）。真漏接了打开的后果是底栏不让开，由下面 60 秒巡检兜
        // A close received while not open = no-op, not a missing mapping: when opening a function page / switching tabs the game sends ClosePanel to a batch of mutually exclusive panels (storage, cube, shop, waypoint, dialog…) regardless of whether they are open;
        // at launch the character select page is also closed first and then opened (from reading the UI message log; previously logged as a failure, 14 false failures per launch on the player path). The consequence of really missing an open is the bottom bar not yielding, which the 60-second patrol below covers
        if(close && g_hud_stack[slot].open<=0) return;
        // 按「开 / 没开」记，不按次数累加：游戏对已经开着的面板再发一次 OpenPanel 是常态——雇佣列表上的替换确认框每被关掉一次，
        // 就重发一次 OpenPanel HireMenuPanel（中间没有关闭），而整段对话结束时只发固定次数的关闭。按次数加的话多出确认框两回再雇成，计数留在 1：
        // 屏上已没有面板，前端却一直按面板态（没摇杆、没技能钮、点哪里都是「面板外」）。809 份日志里计数到过 2 以上的只有 4 个面板
        // （主菜单链接、选角页、起名候选栏、雇佣列表），没有一个是真叠两层要关两次的。dm_tc_navigation 那一路本来就是幂等位图，两路口径一致了
        // Recorded as "open / not open", not accumulated by count: the game sending OpenPanel again for an already open panel is normal -- each time the replacement confirmation box on the hire list is dismissed,
        // OpenPanel HireMenuPanel is sent again (with no close in between), while at the end of the whole dialog only a fixed number of closes is sent. Adding by count, when the confirmation box comes up two extra times and the hire then succeeds, leaves the count at 1:
        // no panel left on screen, yet the front end stays in panel mode (no stick, no skill buttons, every tap is "outside the panel"). Across 809 logs only 4 panels ever reached a count of 2 or more
        // (main menu links, character select page, naming candidate bar, hire list), and none of them was a real two-layer stack needing two closes. The dm_tc_navigation path was already an idempotent bitmap, so the two paths now agree
        if(open && g_hud_stack[slot].open>0) { fprintf(stderr,"[hud] 面板栈 %s %s：已经开着，不重复计\n",act,arg); return; }
        g_hud_stack[slot].open=open?1:0; if(open && !strcmp(arg,"MainMenuButtonRibbon")) dm_platform_menu_ready();   // 主菜单出来：平台结束启动加速 | main menu shown: the platform ends its start-up boost
        fprintf(stderr,"[hud] 面板栈 %s %s → %d\n",act,arg,g_hud_stack[slot].open);
    }
}
int dm_hud_stack_msg(const char *act, const char *arg) {
    hud_stack_update(act, arg);
    // NPC 对话菜单（交谈 / 交易…那一小列）不挡底栏：只开着它时底栏照常显示（否则 UIKit 层让开、露出游戏层那份底栏，难度格和回城书不见、腰带样子也变）。
    // 编码进同一个数：对话菜单开着且没有别的面板 → -1（底栏不让开、且免除导航态的隐藏）
    // The NPC dialog menu (the small column of Talk / Trade…) does not block the bottom bar: with only it open the bottom bar shows as usual (otherwise the UIKit layer yields and exposes the game layer's bottom bar; the difficulty cell and Tome of Town Portal disappear and the belt looks different too).
    // Encoded into the same number: dialog menu open with no other panel → -1 (the bottom bar does not yield, and navigation-mode hiding is waived)
    int n=0, dlg=0; for(int i=0;i<HUD_STACK_MAX;i++) { if(!strcmp(g_hud_stack[i].name,"NpcDialogPanel")||!strcmp(g_hud_stack[i].name,"ScrollSpeechPanel")) dlg|=g_hud_stack[i].open>0; else n+=g_hud_stack[i].open; }   // ScrollSpeechPanel = NPC 说话的滚动文字框（屏幕上方一小块），同样不挡底栏 | ScrollSpeechPanel = the scrolling text box of NPC speech (a small block at the top of the screen); it likewise does not block the bottom bar
    if(!n && dlg) n=-1;
    return n;
}
void dm_hud_stack_dump(void) { for (int i = 0; i < HUD_STACK_MAX; i++) if (g_hud_stack[i].open) fprintf(stderr, "%s×%d ", g_hud_stack[i].name, g_hud_stack[i].open); }   // 开着的记录（巡检日志用） | Records currently open (for the patrol log)
static struct { char name[48]; int open; } g_pm[48];   // 界面消息上开着的非常驻面板 | Non-persistent panels that are open per the UI messages
static void ui_panel_note(const char *act, const char *arg) {   // 界面消息回调里调（游戏主线程） | Called from the UI message callback (game main thread)
    if (!strcmp(act, "UnloadAll")) { memset(g_pm, 0, sizeof g_pm); return; }
    if (!strcmp(act, "UnloadPanel") && arg && !strcmp(arg, "LoadScreenPanel")) {   // 读取画面撤下：与前端 hud_stack_message 同口径——屏上已看不见的记录清掉，还在的（通关页）照记 | Loading screen taken down: same convention as the front end's hud_stack_message - records no longer visible on screen are cleared, those still there (the game-completion page) stay recorded
        for (int i = 0; i < 48; i++) if (!g_pm[i].open || !strcmp(g_pm[i].name, "LoadScreenPanel") || !dm_ui_panel_shown(g_pm[i].name)) memset(&g_pm[i], 0, sizeof g_pm[i]);
        return; }
    // 动作全集与前端 hud_stack_message 同一张表（PanelManager 分发 main+0xb1dc60 只认 8 个）：TogglePanel 按当前记录翻转；LoadPanel / HideGameInfo 不改开关
    // The full action set is the same table as the front end's hud_stack_message (the PanelManager dispatch main+0xb1dc60 only recognizes 8): TogglePanel flips the current record; LoadPanel / HideGameInfo do not change open/closed
    int tg = !strcmp(act, "TogglePanel"), op = !strcmp(act, "OpenPanel") || !strcmp(act, "OpenExclusivePanel"), cl = !strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel");
    if ((!op && !cl && !tg) || !arg || !arg[0] || panel_skip(arg) || dm_ui_panel_resident(arg)) return;
    int k = -1; for (int i = 0; i < 48; i++) if (!strcmp(g_pm[i].name, arg)) { k = i; break; }
    if (tg) { op = k < 0 || !g_pm[k].open; cl = !op; }
    if (k < 0 && op) for (int i = 0; i < 48; i++) if (!g_pm[i].name[0]) { k = i; strlcpy(g_pm[i].name, arg, sizeof g_pm[i].name); break; }
    if (k < 0) return;
    g_pm[k].open = op;
}
// 安全阀的另一半：游戏面板树里看得见一个非常驻面板，界面消息上却没记它开着，连续 3 次巡检（约 1.5 秒）→ 给订阅者补发 OpenPanel，前端回面板态。
// 守的不变式：屏上有非常驻面板 ⇒ 前端处于面板态。游戏常态下界面根的可见直接子全是常驻层（DM_UI_DUMP=@top：HUD 五层、AutoMap、佣兵头像栏、
// 光环栏、ShowItems / PanelBorders / MiniMenu / EmoteWheel / FocusManager / Tooltips / InGameMoviePlayer），名单外的可见面板就是真开着的。
// 一出手打【红】行：有一条打开没被记上（或被清掉），去查那段消息序列，不许静默抹平
// The other half of the safety valve: a non-persistent panel is visible in the game's panel tree but the UI messages have no record of it being open, for 3 consecutive checks (about 1.5 s) → re-send OpenPanel to subscribers so the front end returns to the panel state.
// Invariant guarded: a non-persistent panel is on screen ⇒ the front end is in the panel state. In the game's normal state the visible direct children of the UI root are all persistent layers (DM_UI_DUMP=@top: the five HUD layers, AutoMap, the mercenary portrait bar,
// the aura bar, ShowItems / PanelBorders / MiniMenu / EmoteWheel / FocusManager / Tooltips / InGameMoviePlayer), so a visible panel outside that list is really open.
// Whenever it fires it prints a 【红】(red) line: some open was not recorded (or was cleared); go check that message sequence, silently smoothing it over is not allowed
static void ui_panel_heal_open(uint64_t ch, uint64_t n) {
    void uimsg_emit(const char *, const char *, const char *);
    static struct { char name[48]; int miss; } seen[8]; int keep[8] = {0};
    for (uint64_t j = 0; j < n; j++) { uint64_t p = *(uint64_t *)(ch + 8 * j); const char *nm = p ? (const char *)*(uint64_t *)(p + 8) : NULL;
        if (!nm || !nm[0] || strlen(nm) >= 48 || panel_skip(nm) || dm_ui_panel_resident(nm) || !*(uint8_t *)(p + 0x50) || !has_visible_leaf(p, 1)) continue;
        int on = 0; for (int i = 0; i < 48; i++) if (g_pm[i].open && !strcmp(g_pm[i].name, nm)) { on = 1; break; }
        if (on) continue;
        int k = -1; for (int i = 0; i < 8; i++) if (!strcmp(seen[i].name, nm)) { k = i; break; }
        if (k < 0) for (int i = 0; i < 8; i++) if (!seen[i].name[0]) { k = i; strlcpy(seen[i].name, nm, sizeof seen[i].name); seen[i].miss = 0; break; }
        if (k < 0) continue;
        keep[k] = 1;
        if (++seen[k].miss < 3) continue;
        LOG("【红】界面安全阀：%s 在屏上开着 1.5 秒，界面消息上却没记它开着 → 补发 OpenPanel（有一条打开没记上或被清掉了，去查这段消息序列）", nm);
        ui_panel_note("OpenPanel", nm); uimsg_emit("PanelManager", "OpenPanel", nm); keep[k] = 0; }
    for (int i = 0; i < 8; i++) if (!keep[i]) seen[i].name[0] = 0;   // 不再可见 / 已记上 / 已补发：计数从头来 | No longer visible / already recorded / already re-sent: the count starts over
}
// 编年史的子页（三个原版列表页、我们的积分牌 / 仓库页）：切页签时游戏发 ChroniclePanelMessage:ToggleChildPanel:<页> 并替它发 OpenPanel <页>，关编年史时却只关
// ChroniclePanel 自己、不替子页发关——子页在界面消息上一直「开着」，前端 1.5 秒里仍当面板态，最后靠安全阀报红清掉（手柄模式在积分牌页按 B 关编年史实跑）。
// 记下见过的子页名；编年史关了，下一帧替还开着的子页补发 ClosePanel（在消息回调之外发）。
// Child pages of the Chronicle (the three original list pages, our kill board / vault pages): when switching tabs the game sends
// ChroniclePanelMessage:ToggleChildPanel:<page> and sends OpenPanel <page> on its behalf, but when the Chronicle is closed it only closes
// ChroniclePanel itself and sends no close for the child page -- in terms of UI messages the child page stays "open", the front end still
// treats it as panel state for 1.5 seconds, and in the end the safety valve reports a failure and clears it (real run: in controller mode, pressing B
// on the kill board page to close the Chronicle).
// Record the child page names seen; once the Chronicle is closed, send the missing ClosePanel on the next frame for child pages still open
// (sent outside the message callback).
static char g_chr_child[8][48]; static _Atomic int g_chr_closed;
static void ui_chronicle_children(void) {   // 游戏主线程，每帧 | game main thread, every frame
    if (!atomic_exchange(&g_chr_closed, 0)) return; void uimsg_emit(const char *, const char *, const char *);
    for (int c = 0; c < 8; c++) { if (!g_chr_child[c][0]) continue;
        for (int i = 0; i < 48; i++) if (g_pm[i].open && !strcmp(g_pm[i].name, g_chr_child[c])) { g_pm[i].open = 0; uimsg_emit("PanelManager", "ClosePanel", g_chr_child[c]); LOG("界面：编年史关了 → 替子页 %s 补发 ClosePanel", g_chr_child[c]); } }
}
static void ui_panel_heal(uint64_t root, uint64_t fr) {   // 游戏主线程，每帧 | game main thread, every frame
    ui_chronicle_children();
    static uint64_t next; static int miss; if (fr < next) return; next = fr + 30;
    void uimsg_emit(const char *, const char *, const char *); dm_hud_info hi; dm_hud_get(&hi); if (!hi.valid) { miss = 0; return; }
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60); if (!ch || n > 256) return;
    int stale = 0, alive = 0;   // stale：消息上开着、面板树里不可见的个数；alive：有开着的面板此刻在屏上，或状态不明 | stale: number open per the messages but not visible in the panel tree; alive: some open panel is on screen right now, or the state is unknown
    for (int i = 0; i < 48; i++) { if (!g_pm[i].name[0] || !g_pm[i].open) continue;
        int shown = 0;
        for (uint64_t j = 0; j < n; j++) { uint64_t p = *(uint64_t *)(ch + 8 * j); const char *nm = p ? (const char *)*(uint64_t *)(p + 8) : NULL;
            if (nm && !strcmp(nm, g_pm[i].name)) { shown = *(uint8_t *)(p + 0x50) && has_visible_leaf(p, 1); break; } }
        if (shown) alive = 1; else stale++; }   // 面板树里找不到它（还没建 / 已卸）也算不可见 | Not found in the panel tree (not built yet / already unloaded) also counts as not visible
    ui_panel_heal_open(ch, n);
    if (alive || !stale) { miss = 0; return; }
    if (++miss < 3) return;
    miss = 0;
    for (int i = 0; i < 48; i++) { if (!g_pm[i].name[0] || !g_pm[i].open) continue;   // 整个清空：前端回游戏态 | Clear everything: the front end returns to the game state
        char nm[48]; strlcpy(nm, g_pm[i].name, sizeof nm);
        LOG("【红】界面安全阀：%s 在界面消息上还开着，但屏上已经没有任何面板 1.5 秒 → 补发 ClosePanel（有一条没见过的只开不关，去查这段消息序列）", nm);
        uimsg_emit("PanelManager", "ClosePanel", nm); g_pm[i].open = 0; }
}
// 给手势层的面板状态位：手势层原先靠 PanelManager 的开关消息记「储藏箱开着」等标志，消息只开不关时标志一直为真
// （储藏箱那个留着为真的话，物品栏里双击会变成「按住丢弃键」而不是使用）。改成每帧读游戏面板树，变了就发一条合成消息 DM:PanelState 让前端重读
// Panel state bits for the gesture layer: the gesture layer used to track flags such as "stash is open" from PanelManager's open/close messages, and when a message only opened and never closed the flag stayed true forever
// (if the stash flag stays true, a double tap in the inventory panel becomes "hold the drop key" instead of use). Changed to reading the game's panel tree every frame and, on a change, sending a synthetic message DM:PanelState so the front end re-reads
static _Atomic int g_pstate;
static _Atomic int g_dead_reopen; void dm_dead_modal_mode_enter(void) { atomic_store(&g_dead_reopen, 1); }   // 登记表（dm_mode_items.h）：每次切换完成调一次 | Registry (dm_mode_items.h): called once each time a switch completes
int dm_ui_panel_state(void) { return atomic_load(&g_pstate); }
static int root_panel_shown(uint64_t root, const char *name) {
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t j = 0; j < n; j++) { uint64_t p = *(uint64_t *)(ch + 8 * j); const char *nm = p ? (const char *)*(uint64_t *)(p + 8) : NULL;
        if (nm && !strcmp(nm, name)) return *(uint8_t *)(p + 0x50) && has_visible_leaf(p, 1); }
    return 0;
}
int dm_ui_panel_shown(const char *name) {   // 游戏主线程（界面消息回调里也可调）：这个面板此刻在游戏面板树里可见（界面根的直接子、有看得见的叶子） | Game main thread (also callable from UI message callbacks): this panel is currently visible in the game's panel tree (a direct child of the UI root with a visible leaf)
    module_t *m = main_module(); uint64_t root = 0; if (m) memcpy(&root, m->base + 0x2f05458, 8);
    return root && name && name[0] && root_panel_shown(root, name);
}
// 按名字找面板：只看界面根（PanelManager）的直接子控件。面板都是界面根的直接子控件（LoadPanel 挂上、UnloadPanel / UnloadAll 摘掉），
// 每帧都要问的「某面板在不在 / 开没开」走这里，不走 ui_find 的整树查找（面板不在时 ui_find 要把整棵树比完）。
// 每次都从界面根现读，不留控件指针过帧；只许游戏主线程调
// Find a panel by name: only looks at the direct child widgets of the UI root (PanelManager). Panels are all direct child widgets of the UI root (attached by LoadPanel, detached by UnloadPanel / UnloadAll),
// so the per-frame question "is a given panel present / open" goes through here rather than ui_find's whole-tree search (when the panel is absent ui_find has to compare the entire tree).
// Reads from the UI root fresh every time and keeps no widget pointer across frames; may only be called from the game main thread
static uint64_t ui_panel(uint64_t root, const char *name) {
    uint64_t ch = *(uint64_t *)(root + 0x58), n = *(uint64_t *)(root + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t j = 0; j < n; j++) { uint64_t p = *(uint64_t *)(ch + 8 * j); const char *nm = p ? (const char *)*(uint64_t *)(p + 8) : NULL; if (nm && !strcmp(nm, name)) return p; }
    return 0;
}
static void ui_panel_state_pump(uint64_t root) {   // 游戏主线程，每帧 | game main thread, every frame
    void uimsg_emit(const char *, const char *, const char *);
    int st = (root_panel_shown(root, "BankExpansionLayout") ? DM_PS_BANK : 0) | (dm_shop_open() ? DM_PS_VENDOR : 0)   // 商人页：游戏自己的商店状态（dm_actions.c 每帧读） | vendor page: the game's own shop state (read every frame by dm_actions.c)
           | (root_panel_shown(root, "YouDiedModal") ? DM_PS_DEAD : 0) | (root_panel_shown(root, "ImbueItemsPanel") ? DM_PS_IMBUE : 0) | (root_panel_shown(root, "HoradricCubeLayout") ? DM_PS_CUBE : 0);
    if (st & DM_PS_BANK) { uint64_t bk = ui_panel(root, "BankExpansionLayout"), tabs = bk ? ui_find(bk, "BankTabs", 0) : 0; if (tabs && TAB_SEL(tabs) == 5) st |= DM_PS_VAULT; }   // 第六页「编年史」：双击存入走 ui_use_pump，不发原版「按住 Y」（那一页游戏当魔盒页，会送进魔盒） | the sixth page 「编年史」(Chronicle): double-tap deposit goes through ui_use_pump and does not send the original 「按住 Y」(Hold Y) (the game treats that page as the cube page and would send the item into the cube)
    // 模式切换完成时死亡弹窗开着：这个弹窗不随界面重建换布局（开的时候按哪种模式建的就一直是哪份——手柄版有「按 ＋ 继续」那行，触屏版没有），
    // 让原版面板管理器关掉再开一次，按当前模式的布局重建。只走 PanelManager 的开关，不发「继续」（那才是复活）
    // The death popup is open when a mode switch completes: this popup does not change layout with the UI rebuild (it stays whichever copy it was built as when opened -- the controller version has the 「按 ＋ 继续」(Press + to continue) line, the touch version does not),
    // so have the original game's panel manager close it and open it again, rebuilding it with the current mode's layout. Only PanelManager open / close is used; 「继续」(Continue) is not sent (that would be the revive)
    if (atomic_exchange(&g_dead_reopen, 0) && (st & DM_PS_DEAD)) { fe_send("PanelManager", "ClosePanel", "YouDiedModal"); fe_send("PanelManager", "OpenPanel", "YouDiedModal"); LOG("手柄 / 触屏布局：死亡弹窗开着切了模式 → 让原版按当前布局重开一次"); }
    {   // 魔盒并排：储藏箱与魔盒面板同时开着时，魔盒面板从原版的左边（会压住储藏箱）挪到右边、物品栏面板下移出屏；条件没了写回（位置守卫 MV_CUBE，只动这两个面板的矩形）。 | Cube side by side: while the stash and the cube panel are both open, the cube panel is moved from the original game's left side (where it would cover the stash) to the right, and the inventory panel is moved down off screen; written back when the condition no longer holds (position guard MV_CUBE, only the rects of these two panels are changed).
        // 右移量按物品栏面板算：魔盒面板的画面左沿对到物品栏石板内侧（面板矩形 x = 物品栏 x − 155 单位，1334×750 截图上实量）。
        // 物品栏面板为什么挪走而不是关掉：压在魔盒下面时原版光标和拖放仍会落到它的格子 / 装备位上（实测：拖进魔盒的物品被穿到了手上）；
        //   而原版里关物品栏、关魔盒都会连带把储藏箱关掉（面板是成对关的，实测）——所以「只关魔盒」也不能真关：把魔盒面板收起（下移出屏）、物品栏挪回来，
        //   魔盒面板在原版那边仍算开着，等储藏箱关的时候一起关。收起期间再对魔盒「使用」= 重新并排。y 归这一位，x 上物品栏归「储藏箱加宽」那一位，互不相干
        // The right shift is computed from the inventory panel: the left edge of the cube panel's picture is aligned to the inner side of the inventory's stone slab (panel rect x = inventory x − 155 units, measured on a 1334×750 screenshot).
        // Why the inventory panel is moved away rather than closed: when it lies under the cube, the original game's cursor and drag-and-drop still land on its cells / equipment slots (measured: an item dragged into the cube got equipped in a hand slot instead);
        //   and in the original game closing the inventory or closing the cube both close the stash along with it (panels are closed in pairs, measured) -- so "close only the cube" cannot really close it either: the cube panel is stowed (moved down off screen) and the inventory is moved back,
        //   while on the original game's side the cube panel still counts as open and is closed together with the stash when the stash closes. Using 「使用」(Use) on the cube again while it is stowed = side by side again. y belongs to this bit, the inventory's x belongs to the "widened stash" bit, and the two do not interfere
        uint64_t cube = ui_panel(root, "HoradricCubeLayout"), inv = ui_panel(root, "PlayerInventoryExpansionLayout"); int both = (st & DM_PS_BANK) && (st & DM_PS_CUBE) && cube && inv; static int folded;
        g_mv_want[MV_CUBE] = both; if (!both) folded = 0;
        if (both && atomic_exchange(&g_cube_side_close, 0) && !folded) { folded = 1; LOG("魔盒并排：收起魔盒面板，回到储藏箱 + 物品栏"); }
        if (both && atomic_exchange(&g_cube_side_req, 0) && folded) { folded = 0; LOG("魔盒并排：魔盒面板重新并排"); }
        if (both) { int i_ = 0; while (i_ < g_nmv && !(g_mv[i_].r.w == cube && g_mv[i_].f == 0)) i_++;
            int32_t cx0 = i_ < g_nmv && *(int32_t *)(cube + 0x70) == g_mv[i_].val ? g_mv[i_].orig : *(int32_t *)(cube + 0x70);
            mv_offset(MV_CUBE, cube, 0, folded ? 0 : *(int32_t *)(inv + 0x70) - 155 - cx0, 1); mv_offset(MV_CUBE, cube, 1, folded ? 6000 : 0, 1); mv_offset(MV_CUBE, inv, 1, folded ? 0 : 6000, 1); }
        else if (mv_count(MV_CUBE)) mv_restore(root, MV_CUBE);
        // 标题：原版标题条这时写的是「赫拉迪克方块 | 物品栏」。左边那个字一直挪出屏、右边那个并排时挪出屏（都只动 y），换上我们自己的「储藏箱」「赫拉迪克方块」（TouchSideTitle，在外框布局里，wide_stash.py overlay_layout）
        // Titles: the original title bar at this point reads 「赫拉迪克方块 | 物品栏」(Horadric Cube | Inventory). The left title text is always moved off screen, and the right one is moved off screen while side by side (only y is changed for both), replaced with our own 「储藏箱」(Stash) and 「赫拉迪克方块」(Horadric Cube) (TouchSideTitle, in the outer frame layout, wide_stash.py overlay_layout)
        { uint64_t ov = ui_panel(root, "ControllerOverlay"), t0 = ov ? ui_find(ov, "Text0", 0) : 0, t1 = ov ? ui_find(ov, "Text1", 0) : 0, tt = ov ? ui_find(ov, "TouchSideTitle", 0) : 0;
          if (both) { mv_offset(MV_CUBE, t0, 1, 6000, 1); mv_offset(MV_CUBE, t1, 1, folded ? 0 : 6000, 1); }
          if (tt) { ui_move_ours(tt, both ? -196 : 99999); ui_show_ours(ui_find(tt, "TouchSideTitle_R", 0), both && !folded); } }
        atomic_store(&g_cube_side_on, both && !folded);
        if (!both) atomic_store(&g_cube_side_close, 0);
        if (atomic_exchange(&g_cube_side_req, 0) && (st & DM_PS_BANK) && !(st & DM_PS_CUBE)) { fe_send("PanelManager", "OpenPanel", "HoradricCubeLayout"); LOG("魔盒并排：储藏箱开着 → 魔盒面板并排打开"); } }
    if (atomic_exchange(&g_pstate, st) != st) { LOG("界面状态：面板状态位 → %#x（1 储藏箱 / 2 商人页 / 4 死亡弹窗）", st); uimsg_emit("DM", "PanelState", ""); }
    // 面板冻结（diag.c freeze_tick，缺省关）：「算冻结的面板」有没有哪个在屏上。读面板树的可见位，不靠开关消息——消息会只开不关、会对没开的补发关。
    // 名单：物品栏 / 角色 / 高级属性 / 技能树 / 任务 / 佣兵 / 编年史 / 商人（含修理、赌博）/ 雇佣名单 / 储藏箱 / 单件任务槽 / 魔盒算；NPC 对话菜单、小站页、确认框不算（确认框叠在算的面板上时底下那个仍在屏上，照冻）；
    // 死亡弹窗、读取画面在屏上时一律不冻（换图要服务端走拍）；暂停菜单、设置页不在名单里，沿用原版暂停。
    // 下面这张表全是面板管理器开关的顶层面板（界面根的直接子）：只扫根下一层。原先每个名字从根全树深搜，面板没开时每帧十几遍扫完整棵树（城里每帧约 723k 条指令，宿主每帧总量的一半，玩法线 DM_PUMP_PROF 量）
    // Panel freeze (diag.c freeze_tick, off by default): whether any of the "panels that count for freezing" is on screen. Reads the visible bits of the panel tree instead of relying on open / close messages -- messages can open without a matching close, and can send a close for a panel that was never opened.
    // The list: inventory / character / advanced stats / skill tree / quests / mercenary / Chronicle / merchant (including repair and gambling) / hire list / stash / single-item quest socket / Horadric Cube count; the NPC dialogue menu, the waypoint page and confirmation boxes do not (when a confirmation box sits on top of a panel that counts, the one underneath is still on screen and the game stays frozen);
    // never freeze while the death popup or the loading screen is on screen (changing levels needs the server to tick); the pause menu and the settings page are not on the list and keep the original pause.
    // Every panel in the table below is a top-level panel opened and closed by the panel manager (a direct child of the UI root), so only the level under the root is scanned. Previously each name did a full-tree depth-first search from the root, and with the panels closed the whole tree was walked a dozen-plus times per frame (about 723k instructions per frame in town, half of the host's per-frame total, measured with DM_PUMP_PROF)
    { static const char *fz[] = {"PlayerInventoryExpansionLayout", "CharacterStatsPanel", "AdvancedStatsPanel", "SkillsTreePanel", "QuestLogPanelExpansion", "HirelingInventoryPanel", "VendorPanelLayout", "HireMenuPanel", "BankExpansionLayout", "BankOriginalLayout", "ImbueItemsPanel", "HoradricCubeLayout", "ChroniclePanel"};
      int want = 0, which = -1; for (unsigned i = 0; i < sizeof fz / sizeof fz[0] && !want; i++) { uint64_t w = ui_panel(root, fz[i]); if (w && *(uint8_t *)(w + 0x50) && has_visible_leaf(w, 1)) { want = 1; which = (int)i; } }
      if (want && ((st & DM_PS_DEAD) || root_panel_shown(root, "LoadScreenPanel"))) want = 0;
      extern _Atomic int g_dm_frz_want; if (atomic_exchange(&g_dm_frz_want, want) != want && getenv("DM_PANEL_FREEZE")) LOG("界面状态：算冻结的面板 %s%s", want ? "在屏上：" : "都不在屏上", want ? fz[which] : "");
      void dm_freeze_frame(int want); dm_freeze_frame(want); }
}
// 每帧泵按模式分两类（下面 dm_ui_mouse_pump 里那一长行；tests/mode_scan.py 核：那一行里每个泵要么带 if (tm)、要么在这张「两种模式都跑」的单子上，新加的泵两边都不在就红）。
//   触屏专属（带 if (tm)）：请求全部来自触摸手势——点世界 / 佣兵头像、长按取物、双击使用、点货买、拖放、点技能绑定、副手组页签、开佣兵页。
//     手柄模式与切换事务进行中不跑；在途的请求由切换清理作废（dm_ui_cache_mode_leave）。
//   两种模式都跑：ui_target_pump（鉴定目标，含手柄按住 A）、ui_item_pump（命中查询，宿主自己也问）、ui_ptr_pump（指针事件，宿主合成的点击也走）、
//     ui_safe_pump / ui_lift_pump / ui_note_dock_pump / ui_trove_tabs_pump / ui_minimap_pump / ui_map_pump / ui_map_geom_pump / ui_map_center（屏幕几何，与输入方式无关）、ui_boss_pct_pump、ui_npcdlg_pump、
//     ui_widestash_pump、mv_audit_pump、ui_pages_pump（含手柄 L / R 翻页）、ui_watch_pump、ui_panel_heal、ui_panel_state_pump、rs_pump（一键重开，在前端泵里调）
// 物品说明框停靠（观测门 OBS_TOOLTIP，挂在说明框面板的「提交一条说明」函数入口 main+0xb0d560(面板, 文字, 目标矩形左上 x2, 目标矩形宽高 x3, 样式, 文字 2)）。
// 说明框是即时式的：悬停着的控件每帧重新提交、画完就清，宿主在自己的时机读面板里的记录数一直是 0，所以只能在提交那一刻改输入。
// 原版把框摆在目标矩形（被点的那一格）正下 / 正上方：长说明（暗金、符文之语，十几行）盖住腰带、戒指、背包中间几列和「存放」钮，要点被盖住的格子得先点别处。
// 触屏、只开着物品栏一个面板、光标下是物品时：把目标矩形换成对侧那块暗格空板（物品栏外框里、中间区域左右各一块）顶上的一个点——
// 物品在面板右半停左空板、在左半停右空板，框从上往下排；量字、摆放、推回屏内仍是游戏自己做（离屏幕边的距离走布局里的 screenEdgePadding）。
// 空板位置按屏高定（面板按屏高缩放）：空板中线离屏幕中线 0.617 × 屏高、顶边 0.117 × 屏高（iPhone 15 Pro 档 2556×1179 截图量：
// 667 点宽视图里空板 108–180 / 487–558、屏高 307；拿 16:9 截图验算中线 102 落在那张图的空板 60–145 里）。
// 原先按「屏幕边到面板内框的带子正中」算，只在 16:9 碰巧落在空板里，2.17:1 的 iPhone 上落到面板外框外、贴着屏幕边（iPhone 15 Pro 视频）。
// 其余一律原样放行：手柄 / 键鼠模式；并排两个面板的页（储藏箱 / 商人 / 魔盒 / 单件槽 / 佣兵页——没有空带）；技能、属性、按钮的说明（光标下不是物品）；世界里的名牌。
// 只许游戏主线程（门回调就在）。
// 测试开关 DM_TEST_TIPDOCK=1：不看模式（Mac 宿主是手柄版，用它看效果）；=0：整个关掉
// The per-frame pumps fall into two classes by mode (the long line in dm_ui_mouse_pump below; tests/mode_scan.py checks that every pump on that line either has if (tm) or is on this "runs in both modes" list; a newly added pump that is in neither is red).
//   Touch only (with if (tm)): requests all come from touch gestures: tapping the world / mercenary portrait, long-press to pick up, double-tap to use, tap to buy, drag and drop, tapping to bind a skill, the off-hand set tab, opening the mercenary page.
//     Not run in gamepad mode or while a switch transaction is in progress; requests in flight are cancelled by the switch cleanup (dm_ui_cache_mode_leave).
//   Run in both modes: ui_target_pump (identify target, including holding A on the gamepad), ui_item_pump (hit queries, the host asks too), ui_ptr_pump (pointer events, host-synthesized clicks go through it as well),
//     ui_safe_pump / ui_lift_pump / ui_note_dock_pump / ui_trove_tabs_pump / ui_minimap_pump / ui_map_pump / ui_map_geom_pump / ui_map_center (screen geometry, independent of input method), ui_boss_pct_pump, ui_npcdlg_pump,
//     ui_widestash_pump, mv_audit_pump, ui_pages_pump (including L / R page turning on the gamepad), ui_watch_pump, ui_panel_heal, ui_panel_state_pump, rs_pump (one-tap restart, called from the front end pump)
// Item description box docking (observation gate OBS_TOOLTIP, on the entry of the description panel's "submit one description" function main+0xb0d560(panel, text, target rect top-left x2, target rect size x3, style, text 2)).
// The description box is immediate-mode: the hovered widget resubmits it every frame and it is cleared after drawing, so when the host reads the panel at its own time the record count is always 0; the input can only be changed at the moment of submission.
// The original places the box directly below / above the target rect (the slot that was tapped): long descriptions (uniques, runewords, a dozen lines) cover the belt, rings, the middle columns of the inventory and the Stash button, and you have to tap elsewhere before you can tap a covered slot.
// On touch, with only the inventory panel open and an item under the cursor: replace the target rect with a point at the top of the dark empty side panel on the opposite side (inside the inventory frame, one on each side of the middle area):
// an item in the right half of the panel docks on the left panel and vice versa, and the box is laid out downward; measuring the text, placing it and pushing it back on screen are still done by the game (the distance from the screen edge uses screenEdgePadding from the layout).
// The side panels are positioned by screen height (the panel scales with screen height): panel center line 0.617 x screen height from the screen center line, top edge 0.117 x screen height (measured on an iPhone 15 Pro 2556x1179 screenshot:
// in a 667-point-wide view the panels span 108–180 / 487–558 with screen height 307; cross-checked on a 16:9 screenshot, center line 102 falls inside that image's panel at 60–145).
// This used to be computed as "the middle of the band between the screen edge and the panel's inner frame", which only happened to land in the side panel on 16:9; on 2.17:1 iPhones it landed outside the panel frame against the screen edge (iPhone 15 Pro video).
// Everything else passes through unchanged: gamepad / keyboard and mouse mode; pages with two panels side by side (stash / vendor / Horadric Cube / single-item slot / mercenary page, which have no empty band); descriptions of skills, attributes and buttons (no item under the cursor); name plates in the world.
// Game main thread only (the gate callback runs there).
// Test switch DM_TEST_TIPDOCK=1: ignore the mode (the Mac host is the gamepad version; use it to see the effect); =0: turn it off entirely
static int ui_touch_mode(void);
// 说明框占的地方（给提示框让位用，见 ui_note_dock_pump）：提交时只有目标矩形和文字，框的大小游戏在提交函数里面量（main+0xb0f998），入口拿不到，
// 按文字估：行数 = 换行数 + 1，每行 0.038 × 屏高，上下边 0.03 × 屏高（Mac 16:10 截图标定：15 行长说明在 900 高界面里占 515）；
// 横向以目标中线为中、半宽 0.25 × 屏高（比实测最宽的一行还宽，宁可多让）；纵向照原版：目标正下方放得下就在下、否则在上。界面坐标（与提交参数同一套）
// Area taken by the description box (for making room for the notice box, see ui_note_dock_pump): at submission there is only the target rectangle and text; the game measures the box size inside the submit function (main+0xb0f998), unreachable from the entry,
// so it is estimated from the text: lines = newlines + 1, 0.038 × screen height per line, top and bottom margins 0.03 × screen height (calibrated on a Mac 16:10 screenshot: a 15-line description takes 515 in a 900-high UI);
// horizontally centered on the target center line with half width 0.25 × screen height (wider than the widest measured line; better to give more room); vertically as in the original game: below the target if it fits, otherwise above. UI coordinates (same system as the submit arguments)
static void tip_note(const uint64_t *regs) {
    module_t *m = main_module(); if (!m) return; int H = *(int32_t *)(m->base + 0x211da3c); const char *txt = (const char *)(uintptr_t)regs[1]; if (H <= 0 || !txt) return;
    int n = 1; for (const char *c = txt; *c && n < 64; c++) n += *c == '\n';
    int tx = (int32_t)regs[2], ty = (int32_t)(regs[2] >> 32), tw = (int32_t)regs[3], th = (int32_t)(regs[3] >> 32), bh = (int)lroundf((0.038f * n + 0.03f) * H), hw = H / 4, cx = tx + tw / 2;
    int below = ty + th + bh <= H, y0 = below ? ty + th : ty - bh;
    g_tip_box[0] = cx - hw; g_tip_box[1] = y0; g_tip_box[2] = cx + hw; g_tip_box[3] = y0 + bh; atomic_store(&g_tip_fr, dm_guest_frame());
}
static void tip_dock(const uint64_t *regs);
void dm_ui_tooltip_obs(const uint64_t *regs) { tip_dock(regs); tip_note(regs); }   // 先按触屏规则改停靠目标，再按最终目标记占位 | First change the dock target by touch rules, then record the occupied area by the final target
static void tip_dock(const uint64_t *regs) {
    static int force = -2; if (force == -2) { const char *e = getenv("DM_TEST_TIPDOCK"); force = e ? atoi(e) : -1; }
    if (force == 0 || (force < 0 && !ui_touch_mode())) return;
    module_t *m = main_module(); uint64_t root = 0; if (!m) return; memcpy(&root, m->base + 0x2f05458, 8); if (!root) return;
    if (atomic_load(&g_pstate) & (DM_PS_BANK | DM_PS_VENDOR | DM_PS_IMBUE | DM_PS_CUBE | DM_PS_VAULT | DM_PS_DEAD)) return;
    if (!ui_panel(root, "PlayerInventoryExpansionLayout") || ui_panel(root, "HirelingInventoryPanel")) return;
    dm_cursor_item_info ci; if (!dm_cursor_item(&ci)) return;
    int W = *(int32_t *)(m->base + 0x211da38), H = *(int32_t *)(m->base + 0x211da3c); if (W <= 0 || H <= 0) return;
    int tx = (int32_t)regs[2], tw = (int32_t)regs[3], off = (int)lroundf(0.617f * H); if (W / 2 - off < H / 10) return;   // 空板贴到屏幕边（比 16:9 还窄的屏）就不挪 | If the side panel would touch the screen edge (screens narrower than 16:9), do not move the box
    int left = tx + tw / 2 >= W / 2, cx = left ? W / 2 - off : W / 2 + off, y = (int)(0.117f * H);
    uint64_t *r = (uint64_t *)(uintptr_t)regs;   // 门只回写 x0–x8：改 x2 / x3 | the gate writes back only x0-x8: change x2 / x3
    r[2] = (uint64_t)(uint32_t)cx | (uint64_t)(uint32_t)y << 32; r[3] = 0;
    static int lg; static int ll = -1; if (ll != left && lg < 40) { ll = left; lg++; LOG("说明框停靠：物品 %s 在面板%s半 → 说明框停到%s侧空板（中线 x %d，顶 y %d；画面 %dx%d）", ci.code, left ? "右" : "左", left ? "左" : "右", cx, y, W, H); }
}
static int ui_touch_mode(void) { int dm_padmode_current(void), dm_padmode_busy(void); return !dm_padmode_current() && !dm_padmode_busy(); }
// Mac App：界面是手柄版（上面答「不是触屏模式」），但真鼠标走的是触屏那套物品手势（touch_mac.c 把鼠标事件喂给触屏状态机）。
// 只把物品手势的四个执行泵（长按 / 双击使用 / 买 / 拖放）在真鼠标模式下补跑一遍；绑定框、技能页签对调、佣兵页这些改触屏布局的泵不跑。没有请求时这四个什么都不做，手柄玩家不受影响
// Mac App: the interface is the gamepad layout (the answer above is "not touch mode"), but the real mouse uses the touch layout's item gestures (touch_mac.c feeds mouse events to the touch state machine).
// Only the four execution pumps of the item gestures (long press / double-click use / buy / drag and drop) are additionally run in real-mouse mode; the pumps that change the touch layout - binding box, skill tab swap, mercenary page - are not run. With no request these four do nothing, so gamepad players are unaffected
#if TARGET_OS_OSX
#define UI_MOUSE_ITEM_PUMPS(pm, proot, pw, phh) do { int dm_touch_host_is_live(void), dm_padmode_busy(void); if (!tm && dm_touch_host_is_live() && !dm_padmode_busy()) { ui_long_pump(); ui_use_pump(); ui_buy_pump(pw, phh); ui_drag_pump(pm, proot, pw, phh); } } while (0)
#else
#define UI_MOUSE_ITEM_PUMPS(pm, proot, pw, phh) do { } while (0)
#endif
void dm_ui_mouse_pump(void) {   // 游戏取手柄处调（hle_hid fill，主线程）；每个出图帧做一次 | called where the game fetches the controller (hle_hid fill, main thread); done once per presented frame
    static uint64_t last_frame = UINT64_MAX; uint64_t fr = dm_guest_frame();
    if (fr == last_frame) return;
    (void)dm_ui_gen();   // 每帧核一次顶层面板（界面代号）：卸掉又在同一地址建出新面板的那种，隔帧才核会漏 | Check the top-level panels (UI generation) once per frame: a panel unloaded and rebuilt at the same address would be missed by checking every other frame
    ui_watch_pump();
    { module_t *pm0 = main_module(); uint64_t pr = 0; if (pm0) memcpy(&pr, pm0->base + 0x2f05458, 8); if (pr) { ui_panel_heal(pr, fr); ui_panel_state_pump(pr); } }
    // 设置页盖在功能页上面：点顶部页签要先按返回键退出设置页，再切页签（否则底下的页在换、设置页一直盖着）。
    // 有未应用的改动时返回键会弹「是否应用」的确认框——0.6 秒后设置页还在，就不切了，留给玩家答确认框
    // The settings page covers the function pages: tapping a top tab must first press back to exit the settings page and then switch tabs (otherwise the page underneath changes while the settings page stays on top).
    // With unapplied changes the back button pops up the 「是否应用」(Apply changes?) confirmation box -- if the settings page is still there after 0.6 seconds, do not switch and leave the confirmation box for the player to answer
    { static int st; static uint64_t at; uint64_t sb = atomic_load(&g_set_back_at); module_t *sm = main_module(); uint64_t hroot = 0; if (sm) memcpy(&hroot, sm->base + 0x2f05458, 8);
      if (sb && fr >= sb) { atomic_store(&g_set_back_at, 0); ui_auto_key(DM_BTN_B); }
      { uint64_t qb = atomic_load(&g_speech_back_at); if (qb && fr >= qb) { atomic_store(&g_speech_back_at, 0); uint64_t qp = hroot ? ui_find(hroot, "QuestLogPanelExpansion", 1) : 0; if (qp && *(uint8_t *)(qp + 0x50)) ui_auto_key(DM_BTN_B); } }
      if ((atomic_load(&g_tab_left) > 0 || atomic_load(&g_tab_direct)) && atomic_load(&g_tab_top) == 1) { uint64_t sp = hroot ? ui_find(hroot, "SettingsPanel", 1) : 0; int open = sp && *(uint8_t *)(sp + 0x50);
          if (st == 0 && open) { ui_auto_key(DM_BTN_B); st = 1; at = fr + 36; LOG("界面页签：设置页开着，先按返回键退出"); return; }
          if (st == 1) { if (open && fr < at) return; st = 0; if (open) { atomic_store(&g_tab_left, 0); atomic_store(&g_tab_direct, 0); LOG("界面页签：设置页没关（在等确认框），这次不切页签"); return; } } }
      else if (!atomic_load(&g_tab_left) && !atomic_load(&g_tab_direct)) st = 0;
      if (atomic_exchange(&g_tab_direct, 0) && sm && hroot) {   // 直接切：页签条还在界面树里、可见、还是页签条类，才调 | direct switch: called only if the tab bar is still in the UI tree, visible, and still of the tab bar class
          uint64_t gw = dm_wref_get(&g_tab_ref), tw = gw && ui_has(hroot, gw, 0) ? gw : 0, us = ui_find(hroot, "UISwitcher", 0); int d = atomic_load(&g_tab_delta);
          if (!strcmp(g_tab_name, "UISwitcherTabs") && us && *(uint8_t *)(us + 0x50) && *(uint64_t *)(*(uint64_t *)us + 0x28) == (uint64_t)sm->base + 0x404fd8) {
              // 顶部功能页（任务日志 … 选项）：页面是功能页面板 UISwitcher 自己换的，页签条的切页消息它不接。它接 L / R（消息处理 main+0x404fd8 的 0x4052a8 / 0x4052d4 两支）
              // 都是算出「当前页 ± 1」再调 main+0x405898(面板, 目标页, 方向)；这里直接把目标页给它，一步到位。当前页 +0x1b8、页数 +0x188
              // Top function pages (quest log ... options): the pages are switched by the function page panel UISwitcher itself, and it does not take the tab bar's page-switch message. It takes L / R (the two branches 0x4052a8 / 0x4052d4 of message handler main+0x404fd8),
              // both of which compute "current page ± 1" and then call main+0x405898(panel, target page, direction); here the target page is given to it directly, in one step. Current page +0x1b8, page count +0x188
              int cur = *(int32_t *)(us + 0x1b8), n = *(int32_t *)(us + 0x188), to = g_tab_k; if (to < 0) to = 0; if (n > 0 && to >= n) to = n - 1;   // 页号 = 页签的第 K 个（隐藏的佣兵页也占一页），不能用可见页签数推：没佣兵时从选项往回点物品栏会多走一页 | page number = the Kth tab (the hidden mercenary page also takes up a page); it cannot be derived from the visible tab count: without a mercenary, tapping back from options to the inventory panel would go one page too far
              ((void (*)(uint64_t, int, int))(sm->base + 0x405898))(us, to, d > 0); LOG("界面页签：功能页第 %d → 第 %d 页（直接切，现为 %d）", cur, to, *(int32_t *)(us + 0x1b8)); }
          else if (tw && *(uint64_t *)tw == (uint64_t)sm->base + 0x20ba3a8 && *(uint8_t *)(tw + 0x50)) { int was = TAB_SEL(tw);
              ((void (*)(uint64_t, int, int))(sm->base + 0xad9b80))(tw, g_tab_k, 1); LOG("界面页签：「%s」第 %d → 第 %d 个（直接选中，现为 %d）", g_tab_name, was, g_tab_k, TAB_SEL(tw)); }
          else LOG("【红】界面页签：页签条「%s」已不在 / 不可见，这次不切", g_tab_name);
          dm_wref_set(&g_tab_ref, 0); atomic_store(&g_tab_have, 0); } }
    { static uint64_t tnext; if (fr >= tnext) { if (atomic_load(&g_tab_left) > 0) { atomic_fetch_sub(&g_tab_left, 1); ui_auto_key(atomic_load(&g_tab_mask)); tnext = fr + 8; }
        else { uint64_t th = atomic_exchange(&g_tab_then, 0);
            // 对话菜单：按确认键之前核一遍菜单自己记的选中行是不是点的那一行；不是就补挪（至多两轮），还不对就不按——宁可这一下没反应，也不替玩家选错项
            // Dialog menu: before pressing confirm, check whether the selected row the menu itself records is the row that was tapped; if not, move again to correct it (at most two rounds), and if still wrong do not press -- better for this tap to do nothing than to pick the wrong entry for the player
            if (th == DM_BTN_A && atomic_load(&g_tab_top) == 3) { static int fix; int want = atomic_load(&g_menu_want), sel = -1; module_t *mm = main_module(); uint64_t rt = 0; if (mm) memcpy(&rt, mm->base + 0x2f05458, 8);
                uint64_t pn = rt ? ui_find(rt, "NpcDialogPanel", 0) : 0, ae = pn ? ui_find(pn, "ActiveElements", 0) : 0, ct = ae ? ui_find(ae, "Container", 0) : 0;
                if (want >= 0 && ct && npc_menu_sel(pn, ct, &sel) && sel != want) {
                    if (fix < 2) { fix++; int d = want - sel; atomic_store(&g_tab_then, DM_BTN_A); atomic_store(&g_tab_mask, d > 0 ? DM_BTN_DOWN : DM_BTN_UP); atomic_store(&g_tab_left, d > 0 ? d : -d); LOG("界面对话菜单：按确认前选中行是 %d、要的是 %d，补挪 %+d 行", sel, want, d); th = 0; }
                    else { LOG("【红】界面对话菜单：补挪两轮后选中行仍是 %d（要 %d），这一下不按确认", sel, want); th = 0; fix = 0; } }
                else fix = 0; }
            if (th) { ui_auto_key(th); tnext = fr + 8; } } } }
    { module_t *pm = main_module(); uint64_t proot = 0; if (pm) { memcpy(&proot, pm->base + 0x2f05458, 8); int32_t pw = *(int32_t *)(pm->base + 0x211da38), phh = *(int32_t *)(pm->base + 0x211da3c);
        static uint64_t pf = UINT64_MAX; if (proot && pw > 0 && phh > 0 && pf != fr) { pf = fr; { static int dd; const char *dn; if (!dd && (dn = getenv("DM_UI_DUMP")) && atomic_load(&g_taps_base) && fr >= atomic_load(&g_taps_base) + (getenv("DM_UI_DUMP_AT") ? atoi(getenv("DM_UI_DUMP_AT")) : 45)) { dd = 1; ui_dump(proot, 0, dn, 0, pm); } } int tm = ui_touch_mode(); if (tm) ui_wtap_pump(pm, proot, pw, phh); if (tm) ui_long_pump(); ui_target_pump(pm, fr); if (tm) ui_use_pump(); if (tm) ui_buy_pump(pw, phh); ui_item_pump(proot, pw, phh); ui_ptr_pump(pm, proot, pw, phh); if (tm) ui_drag_pump(pm, proot, pw, phh); UI_MOUSE_ITEM_PUMPS(pm, proot, pw, phh); { int lu = atomic_load(&g_load_up); dm_uiscreen_set_ingame(ui_panel(proot, "HUDPanel") && !(lu < 0 ? ui_find(proot, "LoadScreenPanel", 0) != 0 : lu)); } ui_safe_pump(pm, proot, fr); ui_lift_pump(proot, fr); ui_note_dock_pump(proot); ui_trove_tabs_pump(pm, proot, fr); ui_minimap_pump(pm, proot, pw, phh, fr); ui_map_pump(pm); ui_map_geom_pump(pm, proot); ui_boss_pct_pump(pm, proot); ui_npcdlg_pump(proot); ui_map_center(pm, proot, *(volatile int32_t *)(pm->base + 0x2d5c748)); if (tm) ui_bind_pump(proot, fr); if (tm) swaptab_pump(proot); if (tm) ui_mercpg_pump(pm, proot, fr); ui_widestash_pump(proot); mv_audit_pump(proot, fr); ui_pages_pump(pm, proot); } } }
    if (atomic_load(&g_fe_name_open) && atomic_load(&g_ut_phase) == 2) {   // 起名页：点输入框（底板 / 文本框）= 重新弹系统键盘，不发鼠标点击 | naming page: tapping the input box (backplate / text box) = pop up the system keyboard again, no mouse click is sent
        module_t *nm = main_module(); uint64_t nroot = 0; if (nm) memcpy(&nroot, nm->base + 0x2f05458, 8); uint64_t pn = nroot ? ui_find(nroot, "CharacterCreateNamePanel", 0) : 0;
        if (pn && nm) { int32_t w = *(int32_t *)(nm->base + 0x211da38), h = *(int32_t *)(nm->base + 0x211da3c); uint64_t v = atomic_load(&g_ut_xy);
            uint64_t xy = (uint64_t)(uint32_t)(int)(((v >> 16) & 0xffff) / 65535.0 * w) | ((uint64_t)(uint32_t)(int)((v & 0xffff) / 65535.0 * h) << 32);
            static const char *box[] = { "InputBackplate", "InputBackground", "InputText" };
            for (int i = 0; i < 3; i++) { uint64_t b = ui_find(pn, box[i], 0); if (b && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)b + 0x48))(b, xy) & 1)) { atomic_store(&g_ut_phase, 0); atomic_store(&g_fe_name_tap, 1); break; } } } }
    if (atomic_load(&g_del_q) && !atomic_load(&g_del_hold)) { module_t *dm = main_module(); uint64_t droot = 0; if (dm) memcpy(&droot, dm->base + 0x2f05458, 8);
        uint64_t modal = droot ? ui_find(droot, "DeleteCharacterModal", 0) : 0, yes = modal ? ui_find(modal, "Confirm", 0) : 0;
        if (yes && dm) { int32_t w = *(int32_t *)(dm->base + 0x211da38), h = *(int32_t *)(dm->base + 0x211da3c); uint64_t v = atomic_load(&g_ut_xy);
            uint64_t xy = (uint64_t)(uint32_t)(int)(((v >> 16) & 0xffff) / 65535.0 * w) | ((uint64_t)(uint32_t)(int)((v & 0xffff) / 65535.0 * h) << 32);
            uint64_t fn = *(uint64_t *)(*(uint64_t *)yes + 0x48);
            if (((int (*)(uint64_t, uint64_t))fn)(yes, xy) & 1) { atomic_store(&g_del_hold, 1); atomic_store(&g_ut_phase, 0); LOG("前端路径：删除角色弹窗按住「是」→ 按住 A 到松手"); }
            else atomic_store(&g_del_q, 0); }
        else atomic_store(&g_del_q, 0); }
    int ph = atomic_load(&g_ut_phase); if (!ph) return;
    last_frame = fr;
    module_t *m = main_module(); if (!m) return;
    int32_t width = *(int32_t *)(m->base + 0x211da38), height = *(int32_t *)(m->base + 0x211da3c);
    uint64_t root = 0; memcpy(&root, m->base + 0x2f05458, 8);
    if (width <= 0 || height <= 0 || !root) return;
    uint64_t r = atomic_load(&g_ut_xy);
    int x = (int)(((r >> 16) & 0xffff) / 65535.0 * width), y = (int)((r & 0xffff) / 65535.0 * height); if (x >= width) x = width - 1; if (y >= height) y = height - 1;
    if (atomic_load(&g_fe_opt_open)) { uint64_t bw = ui_find(root, "TouchBliss", 0);
        if (bw) { int on = *(int32_t *)(bw + 0x6e8) == 4; if (atomic_exchange(&g_fe_bliss, on) != on) { LOG("前端路径：极乐世界 %s", on ? "勾上" : "取消"); dm_elysium_arm(on); }
            int32_t vw = *(int32_t *)(m->base + 0x211da38), vh = *(int32_t *)(m->base + 0x211da3c);
            if (ph && vh > 0 && !atomic_load(&g_fe_confirm_open) && y > BLISS_Y0 * vh / 2160 && y < BLISS_Y1 * vh / 2160) { x = vw / 2 + (-1551 + 35) * vh / 2160; y = (1330 + 33) * vh / 2160; } } }   // 这一行里点哪都算点勾选框（悬停那几拍也要挪：控件没被悬停过不收点击） | a tap anywhere in this row counts as tapping the checkbox (the hover ticks must be moved too: a widget that has never been hovered does not accept clicks)
    static uint64_t h_mv, h_dn, h_up; if (!h_mv) { h_mv = ui_hash("MouseMove"); h_dn = ui_hash("LeftMouseButtonDown"); h_up = ui_hash("LeftMouseButtonUp"); }
    static int logged;
    { int dm_vault_ui_blocks(int, int); if (!((dm_ui_panel_state() & DM_PS_VAULT) && dm_vault_ui_blocks(x, y))) ui_send_mouse(m, root, h_mv, x, y); }   // 编年史页上不悬停（会冒出底下魔盒钮的原版说明，dm_vault_ui.c） | no hover on the chronicle page (it would bring up the original description of the cube button underneath, dm_vault_ui.c)
    // 按下之前至少先悬停 2 帧：游戏下一帧才算出「光标在哪个控件上」，同一帧 Move + Down 会被当成点在空白处
    // （Mac 的 iOS 包：暂停菜单点「选项」→ 整个面板被关。之前没暴露是因为手柄光标先走到了那里）
    // Hover for at least 2 frames before pressing: the game only works out "which widget the cursor is on" on the next frame, and Move + Down in the same frame is treated as a click on empty space
    // (iOS app on the Mac: tapping 「选项」(Options) in the pause menu → the whole panel got closed. It was not exposed before because the controller cursor had gone there first)
    static uint64_t snapped;   // 这次点按已吸附过的位置（吸附后 g_ut_xy 改成可点控件上的点，再悬停、按下） | the position this tap has already been snapped at (after snapping, g_ut_xy is changed to a point on the clickable widget, then hover, then press)
    if (ph == 2 && r != snapped && (dm_ui_panel_state() & DM_PS_VAULT) && ({ int dm_vault_ui_blocks(int, int); dm_vault_ui_blocks(x, y); })) snapped = r;   // 编年史页自己收的点（格子、部位页签、空处）不吸附：原先点部位页签被吸到旁边的搜索框上 | taps the chronicle page handles itself (cells, slot tabs, empty space) are not snapped: previously a tap on a slot tab got snapped onto the search box next to it
    if (ph == 2 && r != snapped) { int sx = x, sy = y, sn = ui_snap(m, root, &sx, &sy);
        if (sn == 6) { atomic_store(&g_ut_phase, 0); static int l6; if (l6++ < 40) LOG("界面鼠标：(%d,%d) 压着被上层面板盖住的按钮，不点", x, y); return; }
        if (sn == 2) { static int lg; if (lg++ < 40) LOG("界面鼠标：(%d,%d) 就近吸附到 (%d,%d)", x, y, sx, sy);
            r = ((uint64_t)(uint32_t)((sx + 0.5) / width * 65535) << 16) | (uint32_t)((sy + 0.5) / height * 65535); atomic_store(&g_ut_xy, r); x = sx; y = sy; }
        snapped = r; }
    static int hov; static uint64_t hov_xy; if (r != hov_xy) { hov_xy = r; hov = 0; }
    if (ph == 1) hov++;
    if (ph == 2 && hov < 2) { hov++; return; }
    if (ph == 2 && atomic_load(&g_cc_open) && !atomic_load(&g_fe_opt_open) && !atomic_load(&g_fe_name_open) && !atomic_load(&g_fe_confirm_open)) {   // 职业选择页（上面没盖着模式页 / 起名页 / 弹窗）：这次点的若是别的职业，先让正在出列的那位归队（见上） | class selection page (not covered by the mode page / naming page / a popup): if this tap is on a different class, first have the one currently stepping forward return to the lineup (see above)
        uint64_t pn = ui_find(root, "CharacterCreatePanel", 0), xy = (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32), ws[8]; int hit = -1;
        for (int i = 0; pn && i < 8; i++) { ws[i] = ui_find(pn, k_cc_class[i], 0); if (ws[i] && hit < 0 && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)ws[i] + 0x48))(ws[i], xy) & 1)) hit = i; }
        for (int i = 0; pn && hit >= 0 && i < 8; i++) if (i != hit && ws[i] && *(int32_t *)(ws[i] + 0x13f4) == 1) {
            ((void (*)(uint64_t, int))(m->base + 0xc2cf24))(ws[i], 3); static int lg; if (lg++ < 12) LOG("前端路径：职业选择页改选 %s → 让出列中的 %s 归队", k_cc_class[hit], k_cc_class[i]); }
        if (pn && hit >= 0 && ws[hit] && *(int32_t *)(ws[hit] + 0x13f4) == 3) ((void (*)(uint64_t, int))(m->base + 0xc2cf24))(ws[hit], 0);   // 被点的这位还在归队途中：先回到待机，游戏才收这次点击 | the one tapped is still on its way back to the lineup: return it to idle first, and only then does the game accept this click
    }
    if (ph == 2 && ui_bind_modal_tap(root, x, y)) { int e = 2; atomic_compare_exchange_strong(&g_ut_phase, &e, 0); return; }
    if (ph == 2 && (dm_ui_panel_state() & DM_PS_VAULT)) { int dm_vault_ui_tap(int, int); if (dm_vault_ui_tap(x, y)) { int e = 2; atomic_compare_exchange_strong(&g_ut_phase, &e, 0); return; } }   // 编年史页的格子：单击说明 / 双击取出（dm_vault_ui.c），不发鼠标 | cells on the chronicle page: single tap for description / double-tap to take out (dm_vault_ui.c), no mouse event is sent
    if (ph == 2) {   // 合成钮（魔盒页 / 储藏箱魔盒格的 convert）：手上有物品时原版服务端直接丢掉这次合成（0x6e8740），先收进背包再按（兜底，守门之外再保一道） | transmute button (convert on the cube page / the stash cube cell): with an item in hand the original server simply drops this transmute (0x6e8740), so stow it into the inventory first and then press (a fallback, one more safeguard beyond the gatekeeping)
        int px = x, py = y; static uint64_t st0; dm_hud_info hh; dm_hud_get(&hh);
        if (hh.valid && hh.holding && ui_snap(m, root, &px, &py) && !strcmp(g_click_name, "convert")) {
            if (!st0) { st0 = now_ns(); dm_held_stow(); LOG("合成钮：手上还拿着物品（id %d），原版会丢掉这次合成 → 先收进背包再按", hh.holding); return; }
            int res = dm_held_stow_result();
            if (res != 0 && now_ns() - st0 < 1600000000ull) return;   // 还在收（-1）或已发包等回执（1）：等手上空 | still stowing (-1) or packet sent and awaiting acknowledgement (1): wait for the hand to be empty
            // 收不进背包（背包满 / 放不下）：手上有东西时按合成，服务端一定静默丢掉这次合成。不按；这次点击作废，
            // 交给松手收尾链从「收进背包没成」那一步接着走：按返回先关面板、再按一次由游戏自己处理——背包有位放背包，没位丢在脚下。
            // 玩家看到的是面板关了、东西在脚下（或回了背包），手上空了重开魔盒再按合成就成
            // Cannot be stowed into the inventory (inventory full / no room): pressing transmute with something in hand always makes the server silently drop this transmute. Do not press; this click is voided,
            // and it is handed to the release wrap-up chain to continue from the "stowing into the inventory failed" step: press back to close the panel first, then press once more for the game to handle itself -- into the inventory if there is room, dropped at the feet if not.
            // What the player sees is the panel closed and the item at their feet (or back in the inventory); with the hand empty, reopening the cube and pressing transmute then works
            LOG("合成钮：手上物品收不进背包（id %d，结果 %d）→ 不按合成（按了原版会静默丢掉），交收尾链：按返回关面板、由游戏放背包或丢在脚下", hh.holding, res);
            g_rt = (drop_retry){ .active = 1, .id = hh.holding, .rows = 1, .stage = 2, .t = now_ns() - 400000000ull }; atomic_store(&g_drop_loc, 0);
            st0 = 0; int e = 2; atomic_compare_exchange_strong(&g_ut_phase, &e, 0); return; }
        else if (st0) LOG("合成钮：手上已空，按下");
        else if (!(hh.valid && hh.holding) && ui_snap(m, root, &px, &py) && !strcmp(g_click_name, "convert")) LOG("合成钮：按下（HUD 读数 %s，手上 %d）——之后应有一行「合成读数：服务端收到合成请求」，没有就是请求没进游戏服务端", hh.valid ? "有效" : "无效", hh.holding);   // #22：空手这条路原来不记 | #22: the empty-hand path was not logged before
        st0 = 0; }
    if (ph == 2) { ui_send_mouse(m, root, h_dn, x, y); int e = 2; atomic_compare_exchange_strong(&g_ut_phase, &e, 3); }
    else if (ph == 3) { ui_send_mouse(m, root, h_up, x, y); int e = 3; atomic_compare_exchange_strong(&g_ut_phase, &e, 0);
        DM_LAT("click %d,%d", x, y);
        if (logged < 400) { logged++; LOG("界面鼠标：点击 (%d,%d)", x, y); }
        // 设置页的「应用 / 放弃」：这页是手柄版逻辑，退出只认返回键（原版底部提示「B 关闭菜单」），这两个键鼠版按钮点了游戏会处理但不关页。
        // 圆钮隐藏后这页就只剩点面板外能出去。点完替玩家按一下返回：应用 = 保存后退出，放弃 = 不保存退出
        // The settings page's 「应用 / 放弃」(Apply / Discard): this page uses controller-version logic and exits only on the back button (the original bottom hint is 「B 关闭菜单」(B Close Menu)); when these two keyboard-and-mouse-version buttons are tapped the game handles them but does not close the page.
        // With the round buttons hidden, the only way out of this page is tapping outside the panel. After the tap, press back once for the player: apply = save then exit, discard = exit without saving
        { uint64_t sp = ui_find(root, "SettingsPanel", 1); char nm[40]; int px = x, py = y;
          if (sp && *(uint8_t *)(sp + 0x50) && ui_snap(m, root, &px, &py) == 1 && (strlcpy(nm, g_click_name, sizeof nm), !strcmp(nm, "AcceptButton") || !strcmp(nm, "CancelButton") || !strcmp(nm, "TouchClose"))) {   // TouchClose = 这页右上角补的 X（它自己不发消息） | TouchClose = the X added at the top right of this page (it sends no message itself)
              atomic_store(&g_set_back_at, dm_guest_frame() + 12); LOG("界面鼠标：设置页点了 %s → 随后按返回键退出设置页", nm);
              // 「自定义」页的勾选：这两个按钮的 AcceptChanges / CancelChanges 消息观测门看不到（实测只见返回键关页），在这里按点中的按钮转给 dm_custom
              // Checkmarks on the 「自定义」(Custom) page: the AcceptChanges / CancelChanges messages of these two buttons are not visible to the observation gate (in measurement only the back button closing the page was seen), so here they are forwarded to dm_custom according to the button tapped
              dm_custom_on_uimsg("OptionsPanelMessage", nm[0] == 'A' ? "AcceptChanges" : "CancelChanges", ""); } }
        // 任务语音播着时点右上角 X：按下那一下被语音面板吃掉、只停语音（游戏发 DialogueFinished），X 没收到点击，要再点一次才关。
        // 这次点击落在 X 上、且语音是这半秒内刚停的：替玩家按一下返回 = 一下就停语音并关面板（到点时任务面板已经关了就不按）
        // Tapping the top-right X while quest speech is playing: that press is eaten by the speech panel and only stops the speech (the game sends DialogueFinished), the X does not receive the click, and another tap is needed to close.
        // If this click landed on the X and the speech stopped within this last half second: press back once for the player = one tap both stops the speech and closes the panel (not pressed if the quest panel is already closed by then)
        { uint64_t se = atomic_load(&g_speech_end), f = dm_guest_frame();
          if (se && f - se < 30 && atomic_load(&g_close_down) && f - atomic_load(&g_close_down) < 60) {   /* 按下判类时记的「这一下落在 X 上」（点击这一拍「可点控件」查询认不出 X：mini 实测返回 0） | "this press landed on the X" recorded when classifying the press (on the click tick the "clickable widget" query does not recognize the X: measured on the second Mac, it returns 0) */ atomic_store(&g_speech_back_at, f + 12); LOG("界面鼠标：任务语音播着时点了 X → 随后按返回键关面板"); } } }
}

// ---------- 前端路径理顺（进 App 直接离线；没有存档直接到新建角色；有存档选角页当首页） ----------
// 界面消息观测回调里只记「要做什么」，真正的动作在游戏取手柄处（主线程、消息分发之外）做：
//   · 教学提示框 TutorialTooltip（首次启动逐项弹，内容是手柄键位说明）：自动按 B = 跳过全部；
//   · 主菜单按钮条出现：存档目录里没有角色 → 直接发 FrontEndNavigation:ToCharacterCreate（负载 "1" = 离线，抓「选择游戏模式 → 离线」那一下得到）；
//     有角色 → 自动按 X 打开选角页；
//   · 建角的模式页 CharacterOptionsPanel：自动接受缺省；
//   · 「选择游戏模式」OnlinePlayModeModal 若仍被打开（主菜单点了新角色）：同样直接走离线建角。
// ---------- Front-end path streamlining (entering the App goes straight to offline; with no saves go straight to new character creation; with saves the character select page is the home page) ----------
// The UI message observation callback only records "what to do"; the actual actions are done where the game fetches the controller (main thread, outside message dispatch):
//   - Tutorial tooltip box TutorialTooltip (popped up item by item on first launch, its content being controller button explanations): automatically press B = skip all;
//   - Main menu button bar appears: no characters in the save directory → send FrontEndNavigation:ToCharacterCreate directly (payload "1" = offline, obtained by capturing the 「选择游戏模式 → 离线」(Select game modes → Offline) step);
//     characters exist → automatically press X to open the character select page;
//   - Character creation mode page CharacterOptionsPanel: automatically accept the defaults;
//   - If the 「选择游戏模式」(Select game modes) OnlinePlayModeModal still gets opened (new character tapped on the main menu): likewise go straight to offline character creation.
#include <dirent.h>
#include <sys/stat.h>
static _Atomic int g_fe_ribbon, g_fe_list, g_fe_name_key; static _Atomic int g_fe_skip_tut, g_fe_menu, g_fe_mode_modal, g_fe_options; static _Atomic int g_fe_name, g_fe_name_done; static _Atomic uint64_t g_fe_key_until, g_fe_key_mask;
static int has_character(const char *dir, int depth) {
    DIR *d = opendir(dir); if (!d) return 0; struct dirent *e; int found = 0;
    while (!found && (e = readdir(d))) { if (e->d_name[0] == '.') continue; char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".d2s")) found = 1;
        else if (depth < 3) { struct stat st; if (!stat(p, &st) && S_ISDIR(st.st_mode)) found = has_character(p, depth + 1); } }
    closedir(d); return found;
}
static _Atomic uint64_t g_taps_base;   // DM_UI_TAPS_AT="动作:面板"（子串）：DM_UI_TAPS 的帧号从这条界面消息第一次出现起算 | DM_UI_TAPS_AT="action:panel" (substring): the frame numbers of DM_UI_TAPS count from the first occurrence of this UI message
uint64_t dm_ui_taps_base(void) { return atomic_load(&g_taps_base); }
static void rs_on_uimsg(const char *cat, const char *act, const char *arg);   // 一键重开（定义在 fe_send 之后） | one-tap restart (defined after fe_send)
// 选角列表里确认选人（原版 CharacterSelect:SelectCharacter，只收起列表）之后，原版靠「给被选角色打上现在的时间戳 → 列表按时间戳从新到旧重排（main+0x7fc1e0）
// → 取第 0 行（main+0x80efb4）」落到被选的角色上，重排时只把下标夹进范围、不按角色找回。它依赖时钟单调：系统时间早于某个存档（存档经 iCloud 从时钟更快的设备来、
// 调过系统时间、测试用的固定时钟）时被选者排不到第 0 行，进的就是别人。这里在确认那一刻记下高亮角色的名字，重排后的那次 MainMenuHD:UpdateCharacterInfo 上按名字核对，
// 不等就用原版「设当前角色」main+0x7fe32c(下标) 设回并再发一次刷新。列表 / 选中槽按 [main+0x2d7badc] 成对（同下面一键重开那段）；角色名在记录 +0x10
// After confirming a character in the character select list (the original game's CharacterSelect:SelectCharacter, which only collapses the list), the original game lands on the selected character by "stamping the selected character with the current timestamp → re-sorting the list by timestamp from newest to oldest (main+0x7fc1e0)
// → taking row 0 (main+0x80efb4)", and on re-sorting it only clamps the index into range without finding the character again. It depends on a monotonic clock: when the system time is earlier than some save (a save that came through iCloud from a device with a faster clock,
// a changed system time, a fixed clock used by tests), the selected character does not sort to row 0 and someone else is entered. Here the name of the highlighted character is recorded at the moment of confirming, checked by name on the MainMenuHD:UpdateCharacterInfo that follows the re-sort,
// and if it differs, set back with the original game's "set current character" main+0x7fe32c(index) and the refresh sent once more. The list / selected slot are paired by [main+0x2d7badc] (same as the one-tap restart section below); the character name is at record +0x10
static char g_sel_name[17]; static uint64_t g_sel_fr;
static const char *sel_cur(uint8_t *b, const char *want, int64_t *at) {   // 当前选中角色的名字；want 非空时顺带在列表里找它的下标 | Name of the currently selected character; when want is non-empty, also find its index in the list
    int mode = *(int32_t *)(b + 0x2d7badc) != 1; uint8_t *lv = b + (mode ? 0x2d710f0 : 0x2d71108), *slot = b + (mode ? 0x2d7b950 : 0x2d7b960);
    uint64_t arr = *(uint64_t *)lv, n = *(uint64_t *)(lv + 8), cur = *(uint64_t *)slot; const char *nm = NULL; if (at) *at = -1;
    for (uint64_t i = 0; arr && i < n && i < 256; i++) { uint64_t e = *(uint64_t *)(arr + 8 * i); if (!e) continue;
        if (i == cur && slot[8]) nm = (const char *)(e + 0x10); if (want && at && *at < 0 && !strncmp((const char *)(e + 0x10), want, 16)) *at = (int64_t)i; }
    return nm;
}
static void sel_keep(const char *cat, const char *act) {
    module_t *m = main_module(); if (!m) return; uint8_t *b = (uint8_t *)m->base; uint64_t fr = dm_guest_frame();
    if (!strcmp(cat, "CharacterSelect") && !strcmp(act, "SelectCharacter")) { const char *nm = sel_cur(b, NULL, NULL); g_sel_name[0] = 0; if (nm) { strlcpy(g_sel_name, nm, sizeof g_sel_name); g_sel_fr = fr; } return; }
    if (!g_sel_name[0] || strcmp(act, "UpdateCharacterInfo")) return;
    if (fr - g_sel_fr > 120) { g_sel_name[0] = 0; return; }   // 只管确认后紧跟的那次重排；之后玩家再移高亮是另一回事 | Only handles the one re-sort right after the confirm; the player moving the highlight afterwards is a different matter
    int64_t at; const char *nm = sel_cur(b, g_sel_name, &at); char want[17]; strlcpy(want, g_sel_name, sizeof want); g_sel_name[0] = 0;
    if (nm && !strncmp(nm, want, 16)) return;
    if (at < 0) { LOG("【红】选角：确认的角色「%s」不在重排后的列表里，当前是「%s」，不动", want, nm ? nm : ""); return; }
    ((void (*)(int))(b + 0x7fe32c))((int)at); fe_send("MainMenuHD", "UpdateCharacterInfo", "");
    LOG("选角：确认的是「%s」，原版重排后当前成了「%s」（被选者没排到第一行）→ 已按名字设回第 %lld 行", want, nm ? nm : "", (long long)at);
}
void dm_ui_on_uimsg(const char *cat, const char *act, const char *arg) {
    sel_keep(cat, act);
    // 位置守卫自检的四条路：关面板 / 切页签 / 退出到前端（切后台由前端 dm_ui_lifecycle 报）
    // The four paths of the position guard self-check: closing a panel / switching tabs / exiting to the front end (going to the background is reported by the front end's dm_ui_lifecycle)
    if (!strcmp(cat, "PanelManager") && !strcmp(act, "ClosePanel") && arg && strncmp(arg, "Touch", 5)) mv_audit_arm("关面板");
    else if (!strcmp(act, "SelectTab") || !strcmp(act, "ActivateTab")) mv_audit_arm("切页签");
    else if (!strcmp(cat, "FrontEndNavigation") && !strcmp(act, "ToFrontEnd")) mv_audit_arm("退出到前端");
    if (!strcmp(cat, "PanelManager") && arg && !strcmp(arg, "LoadScreenPanel") && (!strcmp(act, "OpenPanel") || !strcmp(act, "UnloadPanel"))) { int up = act[0] == 'O'; atomic_store(&g_load_up, up);
        module_t *pm = main_module(); uint64_t root = 0; if (pm) memcpy(&root, pm->base + 0x2f05458, 8);
        if (root) { dm_uiscreen_set_ingame(!up && ui_find(root, "HUDPanel", 0)); g_lift_next = 0; ui_lift_pump(root, 0); } }
    { int ex = (!strcmp(cat, "PausePanelMessage") && !strcmp(act, "ExitGame")) || (!strcmp(cat, "FrontEndNavigation") && !strcmp(act, "ToFrontEnd")),
          ld = !strcmp(cat, "PanelManager") && arg && !strcmp(arg, "LoadScreenPanel") && !strcmp(act, "OpenPanel");
      if (ex || ld) { module_t *pm = main_module(); uint64_t root = 0; if (pm) memcpy(&root, pm->base + 0x2f05458, 8); if (ex) g_nd_hold = 1;   // 提示框停靠：停靠条件结束，当场写回原值 | notice docking: the docking condition ended, write the original value back on the spot
          note_dock_restore(root, ex ? (act[0] == 'E' ? "保存并退出" : "回到前端") : "读取画面（切场景）"); } }
    if (!strcmp(cat, "QuestLogPanelMessage") && !strcmp(act, "DialogueFinished")) atomic_store(&g_speech_end, dm_guest_frame());
    if (!strcmp(cat, "PanelManager")) ui_panel_note(act, arg ? arg : "");
    if (!strcmp(cat, "ChroniclePanelMessage") && !strcmp(act, "ToggleChildPanel") && arg && arg[0]) { int c = 0; while (c < 8 && g_chr_child[c][0] && strcmp(g_chr_child[c], arg)) c++; if (c < 8 && !g_chr_child[c][0]) strlcpy(g_chr_child[c], arg, sizeof g_chr_child[c]); }
    if (!strcmp(cat, "PanelManager") && arg && !strcmp(arg, "ChroniclePanel") && (!strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel"))) atomic_store(&g_chr_closed, 1);
    dm_custom_on_uimsg(cat, act, arg ? arg : "");
    if (act && !strcmp(act, "ControllerActionEnd")) atomic_store(&g_a_wait, 0);   // 等到「手柄动作结束」：拿起是在确认键松开被游戏处理时才发的（只等到「开始」就挪光标，背包 → 箱子的拿起包没发） | wait until "controller action end": the pick-up is only sent when the release of the confirm button is processed by the game (if the cursor is moved after waiting only for "start", the pick-up packet for inventory → stash is not sent)
    if (strcmp(cat, "InputMessage") && strcmp(cat, "MonsterHealth") && strcmp(cat, "PartyPanelMessage")) DM_LAT("ui %s %s %s", cat, act, arg ? arg : "");
    { static const char *at; static int got; if (!got) { got = 1; at = getenv("DM_UI_TAPS_AT"); }
      if (at && !atomic_load(&g_taps_base)) { char k[160]; snprintf(k, sizeof k, "%s:%s", act, arg); if (strstr(k, at)) atomic_store(&g_taps_base, dm_guest_frame() | 1); } }
    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "CharacterCreateNamePanel")) { int op = !strcmp(act, "OpenPanel"); if (op || !strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel")) { atomic_store(&g_fe_name_open, op); if (op) atomic_store(&g_fe_name_has, 0); if (op) atomic_store(&g_fe_name, 1); } }
    if (!strcmp(cat, "PanelManager") && (!strcmp(arg, "CharacterOptionsPanel") || !strcmp(arg, "ConfirmationModal"))) { int op = !strcmp(act, "OpenPanel"); if (op || !strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel")) atomic_store(arg[1] == 'h' ? &g_fe_opt_open : &g_fe_confirm_open, op); }
    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "DeleteCharacterModal")) { if (!strcmp(act, "OpenPanel")) atomic_store(&g_fe_del_open, 1); else if (!strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel")) { atomic_store(&g_fe_del_open, 0); atomic_store(&g_del_hold, 0); } }
    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "CharacterCreatePanel")) { if (!strcmp(act, "OpenExclusivePanel") || !strcmp(act, "OpenPanel")) atomic_store(&g_cc_open, 1); else if (!strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel")) atomic_store(&g_cc_open, 0); }
    if (!strcmp(cat, "PanelManager") && !strcmp(act, "UnloadAll")) atomic_store(&g_cc_open, 0);
    // 起名页：这页原版只认手柄键。布局里插的「创建 / 返回」按钮发 PanelManager:ClosePanel:TouchKey<键>（关一个不存在的面板；自定义类别的消息不进消息流，验过）→ 这里换成手柄键（＋ = 接受，B = 返回）
    // Naming page: in the original game this page only recognizes controller buttons. The 「创建 / 返回」(Create / Back) buttons inserted into the layout send PanelManager:ClosePanel:TouchKey<key> (closing a nonexistent panel; messages with a custom category do not enter the message stream, verified) → converted here into controller buttons (+ = accept, B = back)
    if (!strcmp(cat, "PanelManager") && !strcmp(act, "ClosePanel") && !strncmp(arg, "TouchKey", 8)) { if (!strcmp(arg + 8, "PLUS")) atomic_store(&g_fe_name_key, 1); else if (!strcmp(arg + 8, "B")) atomic_store(&g_fe_name_key, 2); }
    if (!strcmp(cat, "ConsoleCharacterCreate") && !strcmp(act, "ToggleCharacterOptions") && !strcmp(arg, "1")) atomic_store(&g_fe_options, 1);
    rs_on_uimsg(cat, act, arg);   // 一键重开（定义在下面） | one-tap restart (defined below)
    // 主菜单按钮条在不在、选角列表开没开：有存档时首页必须是选角页，列表被收起（点了已选中的那一行 = 原版的「选定并返回主菜单」）就重新打开
    // Whether the main menu button bar is present and whether the character select list is open: when there are saves the home page must be the character select page, and if the list is collapsed (the already selected row was tapped = the original game's "select and return to the main menu") it is reopened
    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "MainMenuButtonRibbon") && !strcmp(act, "OpenPanel")) atomic_store(&g_fe_ribbon, 1);
    if ((!strcmp(cat, "PanelManager") && (!strcmp(act, "UnloadAll") || (!strcmp(arg, "MainMenuPanel") && !strcmp(act, "UnloadPanel")))) || (!strcmp(cat, "FrontEndNavigation") && (!strcmp(act, "ToGame") || !strcmp(act, "ToCharacterCreate")))) atomic_store(&g_fe_ribbon, 0);
    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "CharacterSelectPanel") && !strcmp(act, "OpenPanel") && atomic_exchange(&g_fe_bliss, 0)) { LOG("前端路径：回到选角页，极乐世界勾选清掉"); dm_elysium_arm(0); }   // 取消建角：这次的勾不带到下一次 | character creation cancelled: this time's checkmark is not carried over to the next time
    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "CharacterSelectPanel")) { if (!strcmp(act, "OpenPanel")) atomic_store(&g_fe_list, 1); else if (!strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel")) atomic_store(&g_fe_list, 0); }
    if (strcmp(cat, "PanelManager") || strcmp(act, "OpenPanel")) return;
    if (!strcmp(arg, "TutorialTooltip")) atomic_store(&g_fe_skip_tut, 1);
    else if (!strcmp(arg, "MainMenuButtonRibbon")) atomic_store(&g_fe_menu, 1);
    else if (!strcmp(arg, "OnlinePlayModeModal")) atomic_store(&g_fe_mode_modal, 1);
}
static uint64_t ui_find(uint64_t w, const char *name, int dep) {   // 按名字找控件（不看可见位）；只许游戏主线程调 | find a widget by name (ignoring the visible bit); may only be called from the game main thread
    if (!w || dep > 6) return 0; uint64_t nm = *(uint64_t *)(w + 8), ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60);
    if (nm && !strcmp((const char *)nm, name)) return w;
    if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = ui_find(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
static uint64_t fe_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static char g_fe_name_text[64]; static _Atomic uint64_t g_fe_name_ms;   // 键盘交回的名字（写完后才置 g_fe_name_done）；主线程第一次看到交回的时刻 | the name handed back by the keyboard (g_fe_name_done is set only after it is written); the moment the main thread first sees the hand-back
_Atomic int g_fe_name_no_accept;   // 前端置 1：这次返回的是替玩家抽的随机名（front_ios.m），只填进名字框、不自动「接受」——同原版「随机名字」键，玩家看到名字再自己点创建 | set to 1 by the front end: what is returned this time is a random name drawn for the player (front_ios.m), which is only filled into the name box without an automatic 「接受」(Accept) -- same as the original game's 「随机名字」(Random Name) button, the player sees the name and then taps create themselves
int dm_ui_fe_name_wants_input(void) { return atomic_load(&g_fe_name_open) && !atomic_load(&g_fe_name_has); }   // 起名页开着、名字还没填：Mac 键盘的回车这时当「输入名称」（A）而不是「接受」（＋），见 touch_mac.c | naming page open and the name not yet filled in: Return on a Mac keyboard then acts as "Enter Name" (A) instead of "Accept" (+), see touch_mac.c
void dm_ui_text_entered(const char *text) { if (text && *text && atomic_load(&g_fe_name_open)) atomic_store(&g_fe_name_has, 1); if (atomic_exchange(&g_fe_name_no_accept, 0)) return; if (text && *text && atomic_load(&g_fe_name_open)) { strlcpy(g_fe_name_text, text, sizeof g_fe_name_text); atomic_store(&g_fe_name_done, 1); } }   // 系统键盘返回（hle_swkbd.c）：起名页输完名字 → 自动「接受」 | system keyboard returns (hle_swkbd.c): name entered on the naming page → automatic 「接受」(Accept)
// 前端要宿主替玩家点一下某个手柄键（任意线程调）：按住 frames 个客体帧后自动松开。目前只有一处用：进局后点一次「显示物品」键（front_ios.m item_labels_on）
// The front end asks the host to press a controller button once for the player (callable from any thread): held for frames guest frames and then released automatically. Currently used in only one place: pressing the 「显示物品」(Show Items) button once after entering a run (front_ios.m item_labels_on)
static _Atomic uint64_t g_pulse_mask, g_pulse_until;
void dm_ui_key_pulse(uint64_t mask, int frames) { atomic_store(&g_pulse_mask, mask); atomic_store(&g_pulse_until, dm_guest_frame() + (uint64_t)frames); }
// 独占的一下（进局后替玩家点「显示物品」键 L 用）：L 在局内常态是「显示物品」，面板开着时是「上一页签」。只在请求那一刻看「没有面板」不够——
// 同一次取手柄里玩家正好按下开背包的键，游戏先开面板、再把 L 当成换页签（iPad 档佣兵局 f：物品栏刚开就被切到角色页；真玩家进局头 1.5 秒开背包同样会撞）。
// 所以放到游戏取手柄那一刻（hle_hid.c fill，游戏主线程）裁决：局内、界面消息上没有开着的面板、这一次要交给游戏的别的键全是 0，才开始；
// 开始后的 frames 帧里只交这一个键，别的键压住（新按下的留在按下沿锁存里，之后照常给出，不丢）。条件不成立就一直等。
// An exclusive press (used to press the 「显示物品」(Show Items) button L for the player after entering a run): L is normally 「显示物品」(Show Items) in a run, and 「上一页签」(Previous Tab) while a panel is open. Checking "no panel" only at the moment of the request is not enough --
// if within the same controller fetch the player happens to press the open-inventory button, the game opens the panel first and then treats L as a tab switch (iPad-tier mercenary run f: the inventory panel had just opened and was switched to the character page; a real player opening the inventory within the first 1.5 seconds of a run hits the same thing).
// So the decision is made at the moment the game fetches the controller (hle_hid.c fill, game main thread): it starts only when in a run, no panel is open according to the UI messages, and all other buttons to be handed to the game this time are 0;
// for the frames frames after it starts only this one button is handed over and the other buttons are held back (newly pressed ones stay in the press-edge latch and are delivered as usual afterwards, not lost). If the conditions do not hold it keeps waiting.
static _Atomic uint64_t g_alone_mask, g_alone_until;
void dm_ui_key_pulse_alone(uint64_t mask, int frames) { atomic_store(&g_alone_until, (uint64_t)frames); atomic_store(&g_alone_mask, mask); }   // 任意线程：登记请求（until 先存帧数，开始时换成截止帧） | any thread: register the request (until first stores the frame count and is replaced with the deadline frame at start)
uint64_t dm_ui_alone_take(uint64_t others) {   // fill 里调：返回这一次应独占交给游戏的键，0 = 照常 | called in fill: returns the button that should be handed to the game exclusively this time, 0 = as usual
    static uint64_t cur, until; uint64_t fr = dm_guest_frame();
    if (cur) { if (fr < until) return cur; cur = 0; }
    uint64_t m = atomic_load(&g_alone_mask); if (!m || others || !dm_uiscreen_ingame()) return 0;
    { int dm_padmode_busy(void); if (dm_padmode_busy()) return 0; }   // 手柄 / 触屏切换事务进行中输入是冻结的，这时发出去会被整下丢掉（前端只登记一次、不会再补）：同别的条件一样等着，切完再发 | While a controller / touch switch transaction is in progress input is frozen, and anything sent now would be dropped whole (the front end registers it only once and will not resend): wait like the other conditions do and send after the switch completes
    for (int i = 0; i < 48; i++) if (g_pm[i].open) return 0;
    atomic_store(&g_alone_mask, 0); cur = m; until = fr + atomic_load(&g_alone_until);
    LOG("前端：进局常态，替玩家点一次「显示物品」键（掉落物名字常显；这 %d 帧只交这一个键 %#llx）", (int)atomic_load(&g_alone_until), (unsigned long long)m);
    return cur;
}
uint64_t dm_ui_auto_buttons(void) { return (dm_guest_frame() < atomic_load(&g_fe_key_until) ? atomic_load(&g_fe_key_mask) : 0) | (atomic_load(&g_del_hold) ? DM_BTN_A : 0)
                                           | (dm_guest_frame() < atomic_load(&g_pulse_until) ? atomic_load(&g_pulse_mask) : 0); }
static void fe_key(uint64_t mask) { atomic_store(&g_fe_key_mask, mask); atomic_store(&g_fe_key_until, dm_guest_frame() + 3); }
static void ui_auto_key(uint64_t mask) { fe_key(mask); }
static void fe_send(const char *cat, const char *act, const char *arg) {
    module_t *m = main_module(); uint64_t root = 0; if (!m) return; memcpy(&root, m->base + 0x2f05458, 8); if (!root) return;
    static char payload[64]; static uint64_t msg[8]; memset(payload, 0, sizeof payload); strlcpy(payload, arg, sizeof payload);
    memset(msg, 0, sizeof msg); msg[0] = ui_hash(cat); msg[1] = ui_hash(act); msg[2] = (uint64_t)payload; msg[3] = 1; msg[4] = 1;
    uint64_t vt = 0, fn = 0; memcpy(&vt, (void *)root, 8); memcpy(&fn, (void *)(vt + 0x28), 8);
    LOG("前端路径：发 %s:%s %s", cat, act, arg);
    ((void (*)(uint64_t, void *))fn)(root, msg);
}
// ---------- 一键重开（暂停菜单「一键重开」= 保存 + 登出 + 同难度登入；标准：不慢于正常进局，只出现开门读取画面，不露主菜单 / 选角页） ----------
// 只走原版的路：替玩家发原版「保存并退出」(PausePanelMessage:ExitGame；退出时的存档提交都在 ToFrontEnd 之前完成) → 回到前端（ToFrontEnd）的下一帧
// 直接发原版 CharacterSelect:LoadCharacter:<原难度>：前端记着的「选中角色」就是刚退出的那个（宿主实测：玩第二行的角色退出后直接载入，进的仍是它），
// 不经主菜单 / 选角页。选角页那次存盘（存角色列表）会挪到进局后执行一次，存档图标转 2 秒——这是原版存档图标的动画（每帧 OpenPanel 带帧号，main+0x45ad60），
// 不是故障；早先「进局后存档图标每 0.03 秒重开」是把动画读成了故障，为躲它等选角页存盘多花了 2–3 秒。
// 进局后核对局内角色名就是刚退出的那个（不符记红）。兜底：900 帧（约 15 秒，按出图帧计，切后台不走）没出读取画面 = 放弃（黑屏撤掉，停在前端）；
// 载入发出 90 帧没出读取画面补发一次。重开期间宿主丢掉所有界面触点（前端另有吞触点的遮罩）。状态只在游戏主线程读写；进出空闲时调前端的遮罩钩子。
// ---------- One-tap restart (pause menu 「一键重开」(Quick Restart) = save + log out + log in at the same difficulty; standard: no slower than a normal run entry, only the door-opening loading screen appears, the main menu / character select page are not exposed) ----------
// Only the original game's path is taken: send the original 「保存并退出」(Save and Exit) for the player (PausePanelMessage:ExitGame; the save commits on exit all complete before ToFrontEnd) → on the frame after returning to the front end (ToFrontEnd)
// send the original CharacterSelect:LoadCharacter:<original difficulty> directly: the "selected character" the front end remembers is the one that just exited (measured on the host: after exiting while playing the second-row character, a direct load still enters as it),
// without going through the main menu / character select page. The character select page's save (saving the character list) is moved to run once after entering the run, and the save icon spins for 2 seconds -- this is the original save icon's animation (OpenPanel every frame with a frame number, main+0x45ad60),
// not a fault; the earlier "after entering the run the save icon reopens every 0.03 seconds" was the animation misread as a fault, and waiting for the character select page save to dodge it cost an extra 2–3 seconds.
// After entering the run, check that the in-run character name is the one that just exited (log a failure line on mismatch). Fallback: 900 frames (about 15 seconds, counted in presented frames, not advancing in the background) without the loading screen = give up (the black screen is removed, staying on the front end);
// if the loading screen has not appeared 90 frames after the load was sent, resend once. During the restart the host drops all UI touch points (the front end additionally has a touch-swallowing cover). The state is read and written only on the game main thread; the front end's cover hook is called when entering and leaving idle.
enum { RS_IDLE, RS_EXIT, RS_FRONT, RS_LOAD, RS_SHOW };   // RS_SHOW：读取画面已出、还没进局——开门动画露出来，遮罩改透明但照样吞触点、宿主照样丢触点（读取期间点到底下前端的「开始」，弹出难度框挂进局内） | RS_SHOW: the loading screen is up and the run has not been entered yet -- the door-opening animation is exposed, the cover turns transparent but still swallows touch points, and the host still drops touch points (during loading a tap reaching 「开始」(PLAY) on the front end underneath pops up the difficulty box attached inside the run)
static _Atomic int g_rs; static int g_rs_req, g_rs_diff, g_rs_verify, g_rs_resent; static char g_rs_name[64]; static uint64_t g_rs_t0, g_rs_f0, g_rs_sf;
int dm_restart_busy(void) { return atomic_load(&g_rs) != RS_IDLE; }
void (*g_dm_restart_cover_hook)(int on);   // 前端登记（iOS front_ios.m）：0 撤、1 黑屏（不露主菜单）、2 透明（露出读取画面）；1 / 2 都吞掉触点。游戏主线程调 | registered by the front end (iOS front_ios.m): 0 remove, 1 black screen (main menu not exposed), 2 transparent (loading screen exposed); 1 / 2 both swallow touch points. Called from the game main thread
static void rs_set(int st) { if (st == RS_LOAD) g_rs_verify = 1; int was = atomic_exchange(&g_rs, st), cv = st == RS_IDLE ? 0 : st == RS_SHOW ? 2 : 1, cw = was == RS_IDLE ? 0 : was == RS_SHOW ? 2 : 1; if (cv != cw && g_dm_restart_cover_hook) g_dm_restart_cover_hook(cv); }
static void rs_on_uimsg(const char *cat, const char *act, const char *arg) {
    if (!strcmp(cat, "PanelManager") && !strcmp(act, "ClosePanel") && !strcmp(arg, "TouchRestart")) g_rs_req = 1;
    int st = atomic_load(&g_rs); if (st == RS_IDLE) return;
    if (st == RS_EXIT && !strcmp(cat, "FrontEndNavigation") && !strcmp(act, "ToFrontEnd")) { rs_set(RS_FRONT); LOG("一键重开：已回到前端（%llu ms）", (unsigned long long)(fe_ms() - g_rs_t0)); }
    if ((st == RS_LOAD || st == RS_FRONT) && !strcmp(cat, "PanelManager") && !strcmp(arg, "LoadScreenPanel") && !strcmp(act, "OpenPanel")) { rs_set(RS_SHOW); LOG("一键重开：「%s」难度 %d 进局读取（从点按钮起 %llu ms）", g_rs_name, g_rs_diff, (unsigned long long)(fe_ms() - g_rs_t0)); }
    if (st == RS_SHOW && !strcmp(cat, "PanelManager") && !strcmp(arg, "LoadScreenPanel") && (!strcmp(act, "UnloadPanel") || !strcmp(act, "ClosePanel"))) { rs_set(RS_IDLE); LOG("一键重开：已进局（从点按钮起 %llu ms）", (unsigned long long)(fe_ms() - g_rs_t0)); }
}
static void rs_abort(const char *why) { rs_set(RS_IDLE); LOG("【红】一键重开：%s → 放弃，停在前端（从点按钮起 %llu ms）", why, (unsigned long long)(fe_ms() - g_rs_t0)); }
void dm_ui_send(const char *cat, const char *act, const char *arg) { fe_send(cat, act, arg); }   // 别的模块往界面根发一条原版界面消息（秘境选层页开编年史面板用）。只许游戏主线程调 | another module sends an original-game UI message to the UI root (used by the rift level selection page to open the chronicle panel). May only be called from the game main thread
static void rs_load(uint64_t fr, const char *how) { char a[4]; snprintf(a, sizeof a, "%d", g_rs_diff); fe_send("CharacterSelect", "LoadCharacter", a); g_rs_sf = fr; rs_set(RS_LOAD); LOG("一键重开：%s → 原版载入「%s」，难度 %d（从点按钮起 %llu ms）", how, g_rs_name, g_rs_diff, (unsigned long long)(fe_ms() - g_rs_t0)); }
// 别的模块替玩家走一键重开（秘境 dm_rift.c：一局里能生成的新秘境图用完时）。原版「保存并退出」要选项页（PauseLayout*）开着才处理（宿主实测：
// 没开时发 ExitGame，900 帧回不到前端），所以先替玩家开选项页、等 30 帧再走和按钮同一条路。只在游戏主线程用（请求是原子量，任何线程可调）
// Another module performs a one-tap restart for the player (rift dm_rift.c: when the new rift maps that can be generated within one run are used up). The original game's 「保存并退出」(Save and Exit) is only handled while the options page (PauseLayout*) is open (measured on the host:
// sending ExitGame when it is not open, 900 frames without getting back to the front end), so first open the options page for the player, wait 30 frames, then take the same path as the button. Used only on the game main thread (the request is an atomic, callable from any thread)
static _Atomic int g_rs_ext; static uint64_t g_rs_ext_f;
void dm_restart_request(void) { atomic_store(&g_rs_ext, 1); }
static void rs_pump(void) {   // 取手柄处（游戏主线程）每次调 | called every time where the controller is fetched (game main thread)
    uint64_t fr = dm_guest_frame();
    { int e = atomic_load(&g_rs_ext); if (e == 1 && atomic_load(&g_rs) == RS_IDLE) { atomic_store(&g_rs_ext, 2); g_rs_ext_f = fr; fe_send("PanelManager", "OpenPanel", getenv("DM_RS_PAUSE_PANEL") ? getenv("DM_RS_PAUSE_PANEL") : "PauseLayout"); LOG("一键重开：别的模块请求 → 先开选项页"); }
      else if (e == 2 && fr - g_rs_ext_f > 30) { atomic_store(&g_rs_ext, 0); g_rs_req = 1; } }
    if (g_rs_req) { g_rs_req = 0; if (atomic_load(&g_rs) != RS_IDLE) return;
        int dm_diag_difficulty(void); module_t *m = main_module(); uint8_t *b = m ? (uint8_t *)m->base : NULL; int d = dm_diag_difficulty();
        uint64_t pl = b ? ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)) : 0, pd = pl ? *(uint64_t *)(pl + 0x10) : 0;   // 客户端玩家单位，玩家数据开头即名字（同 dm_elysium.c） | client player unit; the player data starts with the name (same as dm_elysium.c)
        if (d < 0 || !pd || !*(const char *)pd) { LOG("【红】一键重开：取不到当前角色 / 难度（难度 %d），不动", d); return; }
        strlcpy(g_rs_name, (const char *)pd, sizeof g_rs_name); g_rs_diff = d; g_rs_t0 = fe_ms(); g_rs_f0 = fr; g_rs_resent = 0; rs_set(RS_EXIT);
        LOG("一键重开：「%s」难度 %d → 原版保存并退出", g_rs_name, d); fe_send("PausePanelMessage", "ExitGame", ""); return; }
    int st = atomic_load(&g_rs);
    if (st == RS_IDLE && g_rs_verify) {   // 进局后核对载入的就是刚退出的那个角色（读到玩家名为止，最多 1800 帧） | after entering the run, check that the loaded character is the one that just exited (until the player name is read, at most 1800 frames)
        module_t *m = main_module(); uint8_t *b = m ? (uint8_t *)m->base : NULL; uint64_t pl = b ? ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)) : 0, pd = pl ? *(uint64_t *)(pl + 0x10) : 0;
        if (pd && *(const char *)pd) { int same = !strcmp((const char *)pd, g_rs_name); g_rs_verify = 0; LOG("%s一键重开：进局后核对，当前角色「%s」，刚退出的是「%s」→ %s", same ? "" : "【红】", (const char *)pd, g_rs_name, same ? "相符" : "不符"); }
        else if (fr - g_rs_f0 > 1800) { g_rs_verify = 0; LOG("【红】一键重开：1800 帧没读到局内角色，核对不了"); } }
    if (st == RS_IDLE) return;
    if (st == RS_SHOW) { if (fr - g_rs_f0 > 3600) { rs_set(RS_IDLE); LOG("【红】一键重开：读取画面 3600 帧没撤，放开触点"); } return; }   // 读取本身由原版管，只等它撤 | the loading itself is handled by the original game; just wait for it to be taken down
    if (fr - g_rs_f0 > 900) { rs_abort(st == RS_EXIT ? "900 帧没回到前端" : st == RS_FRONT ? "900 帧没发出载入" : "900 帧没出读取画面"); return; }
    if (st == RS_FRONT) { static uint64_t hf; const char *h = getenv("DM_TEST_RS_HOLD");   // 测试开关 DM_TEST_RS_HOLD=<帧>：回到前端后多停这么多帧再载入（验遮罩吞触点用，产品不设） | test switch DM_TEST_RS_HOLD=<frames>: after returning to the front end, stay this many more frames before loading (to verify the cover swallows touch points; not set in the product)
        if (h) { if (!hf) hf = fr; if (fr - hf < (uint64_t)atoi(h)) return; hf = 0; }
        // 选中刚退出的角色：回到前端时原版把选中重置到列表第一行（按存档的「最近玩过」排，iCloud 同步来的更新的档 / 改过系统时间时第一行不是它；
        // 固定游戏时间时进成了别人）。在原版前端角色列表里按名字找到它，把下标写进原版选中槽（载入 0x80a004 只读这个槽：下标 +0、有效 +8），读回核对再载入。
        // 不调原版 main+0x7fd208：它是「选中并开始」——写完槽接着直接载入或弹难度框（弹出的 CharacterDifficultyModal 挂进局内）
        // 列表 / 选中槽按 [main+0x2d7badc] 成对：≠ 1 → 模式 1、列表 main+0x2d710f0、选中槽 main+0x2d7b950（载入 0x80a004 读的就是它）；= 1 → 模式 0、列表 0x2d71108、槽 0x2d7b960。
        // 列表项 +0x10 是角色名（内联，最长 16 字节）
        // Select the character that just exited: on returning to the front end the original game resets the selection to the first row of the list (sorted by the save's "most recently played"; with a newer save synced from iCloud / a changed system time, the first row is not it;
        // with a fixed game time it entered as someone else). Find it by name in the original front-end character list, write its index into the original selection slot (load 0x80a004 reads only this slot: index +0, valid +8), read it back to check, then load.
        // The original main+0x7fd208 is not called: it is "select and start" -- after writing the slot it goes straight on to load or pop up the difficulty box (the CharacterDifficultyModal that pops up gets attached inside the run)
        // The list / selection slot are paired by [main+0x2d7badc]: ≠ 1 → mode 1, list main+0x2d710f0, selection slot main+0x2d7b950 (this is the one load 0x80a004 reads); = 1 → mode 0, list 0x2d71108, slot 0x2d7b960.
        // List entry +0x10 is the character name (inline, at most 16 bytes)
        { module_t *m = main_module(); uint8_t *b = (uint8_t *)m->base; int mode = *(int32_t *)(b + 0x2d7badc) != 1;
          uint8_t *lv = b + (mode ? 0x2d710f0 : 0x2d71108), *slot = b + (mode ? 0x2d7b950 : 0x2d7b960); uint64_t arr = *(uint64_t *)lv, n = *(uint64_t *)(lv + 8); int64_t idx = -1;
          for (uint64_t i = 0; arr && i < n && i < 256 && idx < 0; i++) { uint64_t e = *(uint64_t *)(arr + 8 * i); if (e && !strncmp((const char *)(e + 0x10), g_rs_name, 16)) idx = (int64_t)i; }
          if (idx < 0) { rs_abort("原版角色列表里没有刚退出的角色"); LOG("一键重开：列表 %llu 项，要的是「%s」", (unsigned long long)n, g_rs_name); return; }
          if (getenv("DM_TEST_RS_WRONGSEL") && n > 1) { *(uint64_t *)slot = (uint64_t)(idx + 1) % n; slot[8] = 1; LOG("DM_TEST_RS_WRONGSEL：先把选中改成第 %llu 项（模拟回前端后第一行不是刚退出的角色）", (unsigned long long)((idx + 1) % n)); }   // 测试开关 | test switch
          uint64_t was = *(uint64_t *)slot; if (was != (uint64_t)idx || !slot[8]) { *(uint64_t *)slot = (uint64_t)idx; slot[8] = 1; }
          uint64_t now = *(uint64_t *)slot, e = now < n ? *(uint64_t *)(arr + 8 * now) : 0;
          if (!slot[8] || now != (uint64_t)idx || !e || strncmp((const char *)(e + 0x10), g_rs_name, 16)) { rs_abort("选中刚退出的角色没成功"); LOG("一键重开：选中槽 %llu（有效 %d），要的是第 %lld 项「%s」", (unsigned long long)now, slot[8], (long long)idx, g_rs_name); return; }
          LOG("一键重开：原版列表第 %lld 项是「%s」，选中槽原为 %llu → %s", (long long)idx, g_rs_name, (unsigned long long)was, was == (uint64_t)idx ? "本来就是它" : "已改选它"); }
        rs_load(fr, "已回到前端"); return; }
    if (st == RS_LOAD && !g_rs_resent && fr - g_rs_sf > 90) { g_rs_resent = 1; rs_load(fr, "载入发出 90 帧没出读取画面，补发"); }
}
// 按键脚本的「发界面消息」步（hle_hid.c MSG:…）：任意线程登记，取手柄处（游戏主线程）发出。单槽，脚本一步一条
// The key script's "send UI message" step (hle_hid.c MSG:...): registered from any thread, sent where the controller is fetched (game main thread). Single slot, one message per script step
static char g_sendq[96]; static _Atomic int g_sendq_on;
void dm_ui_send_queue(const char *spec) { strlcpy(g_sendq, spec, sizeof g_sendq); atomic_store(&g_sendq_on, 1); }
void dm_ui_frontend_pump(void) {   // 取手柄处调；各动作之间隔开几帧 | called where the controller is fetched; actions are spaced a few frames apart
    if (atomic_exchange(&g_sendq_on, 0)) { char b[96], *c1, *c2; strlcpy(b, g_sendq, sizeof b); c1 = strchr(b, ':'); if (c1) { *c1++ = 0; c2 = strchr(c1, ':'); if (c2) *c2++ = 0; fe_send(b, c1, c2 ? c2 : ""); } }
    // 诊断：DM_DBG_FE_SEND="帧:类别:动作:参数;…"（帧号从 DM_UI_TAPS_AT 命中起算）到帧直接往界面根控件发一条界面消息，探「某条消息此刻收不收」用
    // Diagnostics: DM_DBG_FE_SEND="frame:category:action:argument;..." (frame numbers count from the DM_UI_TAPS_AT hit) sends a UI message directly to the UI root widget at the frame, to probe "whether a given message is accepted right now"
    { static struct { uint64_t fr; char c[48], a[48], g[48]; int done; } ds[8]; static int nd = -1; if (nd < 0) { nd = 0; const char *e = getenv("DM_DBG_FE_SEND"); char buf[512]; if (e) { strlcpy(buf, e, sizeof buf);
          for (char *t = strtok(buf, ";"); t && nd < 8; t = strtok(NULL, ";")) { unsigned long long f; ds[nd].g[0] = 0; if (sscanf(t, "%llu:%47[^:]:%47[^:]:%47s", &f, ds[nd].c, ds[nd].a, ds[nd].g) >= 3) { ds[nd].fr = f; nd++; } } } }
      uint64_t b = atomic_load(&g_taps_base), f0 = dm_guest_frame(); if (nd > 0 && b) for (int i = 0; i < nd; i++) if (!ds[i].done && f0 >= b + ds[i].fr) { ds[i].done = 1; if (ds[i].c[0] == '!') { void uimsg_emit(const char *, const char *, const char *); LOG("前端路径：只给订阅者补发 %s:%s %s（不进游戏）", ds[i].c + 1, ds[i].a, ds[i].g); uimsg_emit(ds[i].c + 1, ds[i].a, ds[i].g); } else fe_send(ds[i].c, ds[i].a, ds[i].g); } }   // 类别前加 ! = 只发给订阅者、不进游戏（造「消息上开着、游戏里没有」的现场用） | a ! before the category = sent only to subscribers, not into the game (for setting up the "open according to messages, absent in the game" scenario)
    rs_pump(); if (atomic_load(&g_rs) != RS_IDLE) return;   // 一键重开进行中：主菜单 / 选角页由它走，前端自动化不插手（不然主菜单上会按两次 X） | one-tap restart in progress: it handles the main menu / character select page and the front-end automation stays out (otherwise X would be pressed twice on the main menu)
    static uint64_t next; uint64_t fr = dm_guest_frame(); if (fr < next) return;
    if (atomic_load(&g_cc_open)) {   // 职业选择页的「返回」只在有存档（从选角页进来）时出现；无存档时这页就是首页 | 「返回」(Back) on the class selection page appears only when there are saves (entered from the character select page); with no saves this page is the home page
        module_t *m = main_module(); uint64_t root = 0; if (m) memcpy(&root, m->base + 0x2f05458, 8);
        uint64_t pn = root ? ui_find(root, "CharacterCreatePanel", 0) : 0, bk = pn ? ui_find(pn, "TouchBack", 0) : 0;
        if (bk) { const char *sv = getenv("DM_SAVE"); uint8_t want = !sv || has_character(sv, 0);
            ui_show_ours(bk, want); }
    }
    { int nk = atomic_exchange(&g_fe_name_key, 0); if (nk) { fe_key(nk == 1 ? DM_BTN_PLUS : DM_BTN_B); next = fr + 8; LOG("前端路径：起名页点了「%s」→ %s", nk == 1 ? "创建" : "返回", nk == 1 ? "＋" : "B"); return; } }
    if (atomic_exchange(&g_fe_name_tap, 0)) { fe_key(DM_BTN_A); next = fr + 8; LOG("前端路径：起名页点了输入框 → 重新弹键盘（A）"); return; }
    // 手柄模式的建角模式页：按 Y = 勾 / 取消「极乐世界」。这一页原版只有写死的两行（天梯、标准 / 专家），手柄动作只认 A / B / X（main+0xc31c24），Y 空着；
    // 勾选框是我们自己的控件 TouchBliss（手柄版布局 touch_layout.pad_bliss），翻它 = 替玩家在框心点一下（悬停一帧 → 按下 → 抬起，原版开关按钮自己切换）；勾没勾仍由上面每帧读
    // Character-creation mode page in controller mode: press Y = check / uncheck "Elysium". In the original game this page has only two
    // hard-coded rows (Ladder, Standard / Hardcore), and its controller actions only recognize A / B / X (main+0xc31c24); Y is unused.
    // The checkbox is our own widget TouchBliss (controller layout touch_layout.pad_bliss); flipping it = clicking once at the center of the box
    // on the player's behalf (hover for one frame -> press -> release; the original game's toggle button switches itself).
    // Whether it is checked is still read every frame by the code above.
    { int dm_padmode_current(void); uint64_t dm_padmode_page_keys(uint64_t); static int ck; module_t *mm = main_module(); uint64_t root = 0; if (mm) memcpy(&root, mm->base + 0x2f05458, 8);
      // 「天梯 / 非天梯」那一行在手柄版布局里挪走了（同触屏版，单机只支持标准 / 专家），但原版的焦点一进页仍停在它上面（实跑：按右弹「必须用线上角色才能体验天梯」）：
      // 进页后替玩家按一次十字键下，把焦点落到「标准 / 专家」那一行，之后这一页的上 / 下键收掉（只剩一行，不让焦点回到看不见的那行）。页重开或刚切到手柄模式时重做
      // The "Ladder / Non-Ladder" row has been moved away in the controller layout (same as the touch version; single player only supports
      // Standard / Hardcore), but the original game's focus still rests on it when the page is entered (real run: pressing Right pops up
      // "You must use an online character to play Ladder"):
      // after entering the page, press D-pad Down once on the player's behalf so the focus lands on the "Standard / Hardcore" row, and from then on
      // swallow Up / Down on this page (only one row is left; do not let the focus return to the invisible row).
      // Redo this when the page is reopened or when controller mode has just been switched on.
      static int prep; static uint64_t prep_at; int padpage = dm_padmode_current() && root && atomic_load(&g_fe_opt_open);
      static int after_modal; if (padpage && atomic_load(&g_fe_confirm_open)) after_modal = 1; if (!padpage) after_modal = 0;
      if (!padpage || atomic_load(&g_fe_confirm_open)) prep = 0;   // 确认框（专家警告）关掉后原版把焦点放回页的缺省项（看不见的那一行），所以也重做 | After the confirmation dialog (Hardcore warning) closes, the original game puts the focus back on the page's default item (the invisible row), so redo it then as well
      else {
          if (prep == 0) { prep = 1; prep_at = fr + (after_modal ? 2 : 20); after_modal = 0; }   // 刚进页等面板建好；确认框刚关则马上做 | Just entered the page: wait for the panel to be built; if the confirmation dialog has just closed, do it right away
          else if (prep == 1 && fr >= prep_at) { fe_key(DM_BTN_DOWN); prep = 2; prep_at = fr + 8; LOG("前端路径：手柄模式页 → 焦点下移到「标准 / 专家」那一行（十字键下）"); }
          else if (prep == 2 && fr >= prep_at) prep = 3; }
      if (padpage) { uint64_t bw = ui_find(root, "TouchBliss", 0);   // 勾没勾：同触屏路径的读法（那边只在有触点的帧里读，手柄路径读不到——实跑：勾翻了但进局没生效） | Checked or not: read the same way as the touch path (that path only reads on frames that have a touch point, so the controller path never gets a reading; real run: the checkbox flipped but had no effect once in game)
          if (bw) { int on = *(int32_t *)(bw + 0x6e8) == 4; if (atomic_exchange(&g_fe_bliss, on) != on) { LOG("前端路径：极乐世界 %s", on ? "勾上" : "取消"); dm_elysium_arm(on); } } }
      if (padpage && !atomic_load(&g_fe_confirm_open)) {
          // 焦点还没落到位的那几帧里左 / 右 / A 也先收掉（否则落在看不见的那一行上：按右会弹天梯提示）；落位后只收上 / 下
          // X 一直收掉：原版这一页 X = 切到经典版角色，单机不支持那类角色（同触屏版；提示条里那一条在布局里去掉了）
          // During the few frames before the focus has landed, Left / Right / A are swallowed too (otherwise they act on the invisible row:
          // pressing Right pops up the ladder notice); once it has landed, only Up / Down are swallowed.
          // X is always swallowed: on this page the original game's X = switch to a Classic character, and single player does not support that kind
          // of character (same as the touch version; that entry in the button legend has been removed in the layout).
          { void dm_padmode_legend_hide(const char *, int); dm_padmode_legend_hide("CharacterOptionsLegend", 2); }   // 提示条里原版运行时加的「X 经典版」那一条拿掉（布局里删预设不够：这一页的代码每次刷新都会再加） | Remove the 「X 经典版」(X Classic) entry that the original game adds to the legend at runtime (deleting the preset in the layout is not enough: this page's code adds it again on every refresh)
          if ((dm_padmode_page_keys(DM_BTN_Y | DM_BTN_X | (prep == 3 ? DM_BTN_UP | DM_BTN_DOWN : DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_A)) & DM_BTN_Y) && !ck) { ck = 1; LOG("前端路径：手柄模式页按 Y → 翻「极乐世界」的勾"); }
          if (ck) { int32_t vw = *(int32_t *)(mm->base + 0x211da38), vh = *(int32_t *)(mm->base + 0x211da3c); int x = vw / 2 + (-1551 + 35) * vh / 2160, y = (1330 + 33) * vh / 2160;
              ui_send_mouse(mm, root, ui_hash("MouseMove"), x, y);
              if (ck == 2) ui_send_mouse(mm, root, ui_hash("LeftMouseButtonDown"), x, y); else if (ck == 3) ui_send_mouse(mm, root, ui_hash("LeftMouseButtonUp"), x, y);
              if (++ck > 3) ck = 0; return; } }
      else ck = 0; }
    // 带按键脚本的用例走自己的键序（真值对照用），不插手；DM_FE_AUTO=1 强制开、=0 强制关（画面闸门的「选角页」对照的是原版主菜单那一拍，不带键序）
    // Cases with a key script follow their own key sequence (for ground truth comparison), no interference; DM_FE_AUTO=1 forces on, =0 forces off (the screenshot gate's "character select page" compares against the original game's main menu tick, without a key sequence)
    static int fa = -1; if (fa < 0) { const char *e = getenv("DM_FE_AUTO"); fa = e ? atoi(e) != 0 : !getenv("DM_KEYS"); }
    if (!fa) return;
    // 手柄模式走原版自己的前端流程：下面这些「替玩家按键」都是给触屏定的，手柄模式下一律不做——
    // 线上（实体手柄）：主菜单按 X 开选角列表、A 选定后列表收起回主菜单（原版下一步是在主菜单按 A 开始游戏），我们「列表被收起就替按 X 重开」把玩家卡在选人这一步，
    // 一连重开 8 次、进不了游戏；界面每次重建后的「有存档 → 替按 X 开选角页」、教学提示自动 B、起名页自动 A / ＋ 同样会和玩家自己的按键抢。
    // 只留不经按键、直接发原版消息的两条：一个角色都没有时直达建角、「选择游戏模式」直接走离线。攒下的待办标志清掉，不带回触屏模式
    // Controller mode follows the original game's own front-end flow: the "key presses on the player's behalf" below were all designed for touch, and none of them are done in controller mode --
    // In released builds (physical controller): on the main menu X opens the character list, and after A selects, the list collapses back to the main menu (the original game's next step is pressing A on the main menu to start the game); our "list collapsed, so press X to reopen it" stuck the player at the character selection step,
    // reopening 8 times in a row with no way into the game; likewise "has a save → press X to open the character select page" after every UI rebuild, auto B on tutorial tips, and auto A / + on the naming page all fight with the player's own key presses.
    // Only the two that go without key presses and send the original game's messages directly are kept: going straight to character creation when there is no character at all, and 「选择游戏模式」(Select game modes) going straight to offline. Accumulated pending flags are cleared and not carried back into touch mode
    { int dm_padmode_current(void);
      if (dm_padmode_current()) {
          atomic_store(&g_fe_skip_tut, 0); atomic_store(&g_fe_back, 0); atomic_store(&g_fe_options, 0); atomic_store(&g_fe_name, 0); atomic_store(&g_fe_name_done, 0);
          if (atomic_exchange(&g_fe_mode_modal, 0)) { atomic_store(&g_fe_menu, 0); fe_send("FrontEndNavigation", "ToCharacterCreate", "1"); next = fr + 8; return; }
          if (atomic_exchange(&g_fe_menu, 0)) { const char *sv = getenv("DM_SAVE"); if (sv && !has_character(sv, 0)) { fe_send("FrontEndNavigation", "ToCharacterCreate", "1"); next = fr + 8; }
              else { static int lg; if (lg++ < 6) LOG("前端路径：手柄模式，有存档 → 停在原版主菜单，不替玩家开选角页"); } }
          return; } }
    if (atomic_exchange(&g_fe_skip_tut, 0)) { fe_key(DM_BTN_B); next = fr + 8; LOG("前端路径：教学提示框 → 自动跳过（B）"); return; }
    if (atomic_exchange(&g_fe_back, 0)) { fe_key(DM_BTN_B); next = fr + 8; LOG("前端路径：模式页点了牌子以外 → 返回（B）"); return; }
    // 模式页点了牌子 → 替玩家「接受」。原版「接受」（main+0xc3213c）认的是专家牌的勾选状态：[面板+0x211]（这一行在用）且 [[面板+0x1e0]+0xa88]（专家牌控件的勾选字节）非 0 = 专家，弹警告。
    // 牌子是原版的开关按钮：鼠标点专家牌 = 把它的勾翻一下（实跑读数：连点三次专家牌，状态 专家 → 标准 → 专家）；点标准牌不动专家牌的勾。
    // 所以「点专家 → 警告 → 取消 → 再点标准」时专家牌还勾着，替按的 A 又弹专家警告（线上反馈；更早的包同样）；「取消后再点一次专家」则反而进了标准。
    // 接受之前先核一眼（只读）：专家牌勾没勾与玩家点的那张不一致，就替玩家再点一下专家牌（原版自己的鼠标点击，把勾翻过来），一致了才按 A；
    // 点两次还不一致就不按、报红——宁可不动也不替玩家建错模式。
    // A card was tapped on the mode page -> press "Accept" for the player. The original "Accept" (main+0xc3213c) looks at the checked state of
    // the Hardcore card: [panel+0x211] (this row is in use) and [[panel+0x1e0]+0xa88] (the checked byte of the Hardcore card widget) non-zero
    // = Hardcore, show the warning.
    // The cards are the original toggle buttons: a mouse click on the Hardcore card = flip its check (readings from a real run: three clicks in a row
    // on the Hardcore card give Hardcore -> Standard -> Hardcore); clicking the Standard card does not touch the Hardcore card's check.
    // So with "tap Hardcore -> warning -> cancel -> tap Standard" the Hardcore card is still checked and the A pressed for the player brings up the
    // Hardcore warning again (reported from a released build; earlier builds behave the same); "tap Hardcore once more after cancel" instead
    // ends up in Standard.
    // Before accepting, take one look first (read-only): if whether the Hardcore card is checked does not match the card the player tapped, click the
    // Hardcore card once more for the player (the original game's own mouse click, which flips the check), and press A only once they match;
    // if they still do not match after two clicks, do not press and report a failure -- better to do nothing than to create the wrong mode for the player.
    { static int st, tries, ck; static uint64_t at; module_t *mm = main_module(); uint64_t root = 0; if (mm) memcpy(&root, mm->base + 0x2f05458, 8);
      if (atomic_exchange(&g_fe_options, 0) && !st) { st = 1; tries = 0; ck = 0; at = fr + 4; }
      if (st && fr >= at) { uint64_t pn = root ? ui_find(root, "CharacterOptionsPanel", 0) : 0, hw = pn ? *(uint64_t *)(pn + 0x1e0) : 0;
          if (!pn || !atomic_load(&g_fe_opt_open) || atomic_load(&g_fe_confirm_open)) st = 0;
          else if (ck) {   // 替点专家牌：悬停一帧 → 按下 → 抬起（控件没被悬停过不收点击），坐标 = 布局里专家牌的中心（距屏幕中线 +173…+1644、纵 899–1259，按 2160 高） | Click the Hardcore card for the player: hover one frame -> press -> release (a widget that was never hovered does not accept clicks); coordinates = the center of the Hardcore card in the layout (+173 to +1644 from the screen center line, vertical 899 to 1259, for a height of 2160)
              int32_t vw = *(int32_t *)(mm->base + 0x211da38), vh = *(int32_t *)(mm->base + 0x211da3c); int x = vw / 2 + 908 * vh / 2160, y = 1079 * vh / 2160;
              ui_send_mouse(mm, root, ui_hash("MouseMove"), x, y);
              if (ck == 2) ui_send_mouse(mm, root, ui_hash("LeftMouseButtonDown"), x, y); else if (ck == 3) ui_send_mouse(mm, root, ui_hash("LeftMouseButtonUp"), x, y);
              if (++ck > 3) { ck = 0; at = fr + 4; } }
          else { int want = atomic_load(&g_fe_opt_side), cur = *(uint8_t *)(pn + 0x211) && hw && *(uint8_t *)(hw + 0xa88);
              if (cur == want) { st = 0; fe_key(DM_BTN_A); next = fr + 8; LOG("前端路径：模式页点了%s牌（原版状态一致）→ 接受（A）", want ? "专家" : "标准"); return; }
              if (tries++ < 2) { ck = 1; LOG("前端路径：模式页点了%s牌，原版状态还是%s → 替玩家点一下专家牌把勾翻过来（第 %d 次）", want ? "专家" : "标准", cur ? "专家" : "标准", tries); }
              else { st = 0; LOG("【红】前端路径：模式页点了%s牌，翻两次后原版状态仍是%s，不替玩家接受", want ? "专家" : "标准", cur ? "专家" : "标准"); } } }
      if (st) return; }
    // 起名页：一进来就弹系统键盘（A）；输完名字自动接受（PLUS）进局。取消键盘则停在本页，点名字框可再弹（鼠标点击）
    // Naming page: pop up the system keyboard on entry (A); after the name is entered, accept automatically (PLUS) and enter the run. If the keyboard is cancelled, stay on this page; tapping the name box pops it up again (mouse click)
    if (atomic_exchange(&g_fe_name, 0)) { fe_key(DM_BTN_A); next = fr + 8; LOG("前端路径：起名页 → 弹键盘（A）"); return; }
    // 键盘返回后，等名字真的进了名字框再按接受：游戏在另一条线程上等键盘（真对话框弹着时主线程照常出帧），返回串要等那条线程交回、
    // 主线程才放进名字框；抢先按 = 用空名字创建，报「少于两个字符」且停在死路（3 个汉字 / 9 个字母都被拒）。
    // 名字框 = 起名面板下的 InputText 控件（虚表 main+0x20b8718），文本是 {+0x558 指针, +0x560 字节数}。2 秒还没对上就不替玩家按（留给玩家手动确认），不许超时照按
    // After the keyboard returns, wait until the name has really entered the name box before pressing accept: the game waits for the keyboard on another thread (while the real dialog is up the main thread keeps producing frames as usual), and the returned string is put into the name box by the main thread
    // only after that thread hands it back; pressing early = creating with an empty name, which reports 「少于两个字符」(fewer than two characters) and gets stuck in a dead end (3 Chinese characters / 9 letters were both rejected).
    // Name box = the InputText widget under the naming panel (vtable main+0x20b8718), whose text is {+0x558 pointer, +0x560 byte count}. If it still does not match after 2 seconds, do not press for the player (leave it for the player to confirm manually); pressing anyway on timeout is not allowed
    { static int seen; if (!atomic_load(&g_fe_name_done)) seen = 0; else if (!seen) { seen = 1; atomic_store(&g_fe_name_ms, fe_ms()); } }   // 2 秒从主线程第一次看到「键盘已交回」算起（主线程自己等键盘的情形：交回后才轮到这里） | the 2 seconds count from the first time the main thread sees "keyboard handed back" (the case where the main thread itself waits for the keyboard: this is only reached after the hand-back)
    if (atomic_load(&g_fe_name_done)) {
        module_t *m = main_module(); uint64_t root = 0; if (m) memcpy(&root, m->base + 0x2f05458, 8);
        uint64_t pn = root ? ui_find(root, "CharacterCreateNamePanel", 0) : 0, it = pn ? ui_find(pn, "InputText", 0) : 0; size_t wl = strlen(g_fe_name_text);
        int same = 0; if (it && *(uint64_t *)it - (uint64_t)(uintptr_t)m->base == 0x20b8718) { const char *t = *(const char **)(it + 0x558); uint64_t n = *(uint64_t *)(it + 0x560); same = t && n == wl && !memcmp(t, g_fe_name_text, wl); }
        if (same) { atomic_store(&g_fe_name_done, 0); fe_key(DM_BTN_PLUS); next = fr + 8; LOG("前端路径：名字已进名字框（看到键盘交回后 %llu ms）→ 接受（PLUS）", (unsigned long long)(fe_ms() - atomic_load(&g_fe_name_ms))); }
        else if (fe_ms() - atomic_load(&g_fe_name_ms) > 2000) { atomic_store(&g_fe_name_done, 0); LOG("【红】前端路径：看到键盘交回 2 秒后名字框仍不是「%s」（%s），不自动接受，留给玩家手动确认", g_fe_name_text, it ? "内容不符" : "没找到名字框"); }
        return;
    }
    if (atomic_load(&g_fe_mode_modal)) { atomic_store(&g_fe_mode_modal, 0); atomic_store(&g_fe_menu, 0); fe_send("FrontEndNavigation", "ToCharacterCreate", "1"); next = fr + 8; return; }
    if (atomic_load(&g_fe_menu)) { atomic_store(&g_fe_menu, 0); const char *sv = getenv("DM_SAVE");
        if (sv && !has_character(sv, 0)) fe_send("FrontEndNavigation", "ToCharacterCreate", "1");
        else { fe_key(DM_BTN_X); LOG("前端路径：有存档 → 打开选角页（X）"); }
        next = fr + 8; return; }
    // 列表被收起后（没有新的主菜单消息）：停在主菜单满 20 帧且没有别的弹窗 → 再开选角页
    // 列表收起不一定有 ClosePanel 消息（点列表行 = 选定并收起，只出 SelectCharacter）：直接读选角面板控件的可见位
    // After the list is collapsed (no new main menu message): sitting on the main menu for a full 20 frames with no other popup → reopen the character select page
    // Collapsing the list does not necessarily produce a ClosePanel message (tapping a list row = select and collapse, only SelectCharacter is emitted): read the visible bit of the character select panel widget directly
    int ribbon = atomic_load(&g_fe_ribbon), list_vis = 0; if (ribbon) { module_t *m = main_module(); uint64_t root = 0; if (m) memcpy(&root, m->base + 0x2f05458, 8); uint64_t w = root ? ui_find(root, "CharacterSelectPanel", 0) : 0; list_vis = w && *(uint8_t *)(w + 0x50); }   // 只有停在主菜单时才用得到这一位；局内不找（局内没有这个面板，每帧都是整树比完） | this bit is only needed while sitting on the main menu; not searched in a run (this panel does not exist in a run, so every frame would compare the whole tree)
    static uint64_t bare; if (ribbon && !list_vis && !atomic_load(&g_fe_confirm_open)) { if (!bare) bare = fr; else if (fr - bare >= 20) { bare = 0; const char *sv = getenv("DM_SAVE"); if (!sv || has_character(sv, 0)) { fe_key(DM_BTN_X); next = fr + 30; LOG("前端路径：选角列表被收起 → 重新打开（X）"); }
            else { atomic_store(&g_fe_ribbon, 0); fe_send("FrontEndNavigation", "ToCharacterCreate", "1"); next = fr + 30; LOG("前端路径：已没有角色（删掉了最后一个）→ 直达职业选择"); } } } else bare = 0;
}
// ---------- 模式切换事务的登记项（dm_mode_items.h；游戏主线程，改布局之前调） ----------
// 前端路径理顺与各处替按键：在途的请求全部作废。这些请求都是触摸点击排下的（点页签替按 L / R、点设置页按钮替按返回、点对话菜单行替按上下再确认…），
// 界面马上要整套重建，留着会在新界面上落地
// ---------- Registered items of the mode switch transaction (dm_mode_items.h; game main thread, called before the layout is changed) ----------
// Front-end path straightening and the various key presses made on the player's behalf: all in-flight requests are voided. These requests are all queued by touch taps (tapping a tab presses L / R, tapping a settings page button presses Back, tapping a dialog menu row presses up / down and then confirm...),
// and the whole UI is about to be rebuilt, so leaving them would make them land on the new UI
void dm_ui_auto_mode_leave(void) {
    atomic_store(&g_fe_skip_tut, 0); atomic_store(&g_fe_back, 0); atomic_store(&g_fe_options, 0); atomic_store(&g_fe_name, 0); atomic_store(&g_fe_name_done, 0); atomic_store(&g_fe_name_key, 0); atomic_store(&g_fe_name_tap, 0);
    atomic_store(&g_fe_key_until, 0); atomic_store(&g_fe_key_mask, 0); atomic_store(&g_pulse_until, 0);   // 「独占的一下」（g_alone_*：进局常态替按一次显示物品键）不在此列：它不是触摸点击排下的，两种模式都要发，由它自己等切换完成（dm_ui_alone_take） | "The exclusive press" (g_alone_*: the one show-items key press made routinely on entering a run) is not among these: it is not queued by a touch tap and must be sent in both modes; it waits for the switch to finish by itself (dm_ui_alone_take)
    atomic_store(&g_set_back_at, 0); atomic_store(&g_speech_back_at, 0); atomic_store(&g_tab_left, 0); atomic_store(&g_tab_direct, 0); atomic_store(&g_tab_then, 0); atomic_store(&g_del_q, 0); atomic_store(&g_del_hold, 0);
}
// 界面触摸这一层记着的控件地址与挪位表：挪过的原版控件先写回原值（重建后有的面板原样留着），再把记的地址清掉（重建后旧地址是悬空的，新控件正好分配在旧地址上时认不出）
// Widget addresses and the move table remembered by this UI touch layer: moved original widgets are first written back to their original values (after a rebuild some panels are kept as they were), then the remembered addresses are cleared (after a rebuild the old addresses are dangling, and when a new widget happens to be allocated at an old address the two cannot be told apart)
void dm_ui_cache_mode_leave(void) {
    module_t *m = main_module(); uint64_t root = 0; if (m) memcpy(&root, m->base + 0x2f05458, 8);
    { uint64_t bk = root ? ui_panel(root, "BankExpansionLayout") : 0, tabs = bk && *(uint8_t *)(bk + 0x50) ? ui_find(bk, "BankTabs", 0) : 0;   // 储藏箱开着：记下页签，重建后选回去（ui_pages_pump） | Stash is open: remember the tab and select it again after the rebuild (ui_pages_pump)
      g_bank_tab_keep = tabs ? TAB_SEL(tabs) : -1; g_bank_tab_until = dm_guest_frame() + 120; }
    for (int o = 0; o < MV_N; o++) mv_restore(root, o);
    g_nmv = 0; dm_wref_set(&g_down, 0); dm_wref_set(&g_slot, 0); dm_wref_set(&g_bd_hit_ref, 0); dm_wref_set(&g_click, 0); dm_wref_set(&g_tab_ref, 0); atomic_store(&g_tab_have, 0); atomic_store(&g_ut_phase, 0);
    // 在途的触摸请求（触屏专属的泵切到手柄后不再跑，留着会在切回触屏时突然落地）和保持中的悬停 / 没发完的指针事件
    // In-flight touch requests (the touch-only pumps no longer run after switching to the controller, and leaving them would make them land suddenly on switching back to touch) plus a held hover / pointer events not yet fully sent
    atomic_store(&g_wtap, 0); atomic_store(&g_buy_at, 0); atomic_store(&g_tap_req, 0); atomic_store(&g_use_req, 0); atomic_store(&g_long_req, 0); atomic_store(&g_ident_req, 0);
    atomic_store(&g_dg_kind, 0); atomic_store(&g_bd_state, 0); atomic_store(&g_swaptab_tap, 0); atomic_store(&g_mercpg, 0);
    atomic_store(&g_up_hold, 0); atomic_store(&g_up_r, atomic_load(&g_up_w));
}
