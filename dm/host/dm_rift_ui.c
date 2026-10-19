// 大秘境面板：像编年史仓库 / 刷怪积分牌一样用原版控件；是独立面板，不挂编年史页签。
// 布局 dm/ios/tools/touch_layout.py _rift_panel：借原版「消息记录」面板外壳 MessageLogPanel（控件 TouchRift / TouchRF_*）；数据与动作走 dm_rift.c 的 dm_rift_menu*（服务端拍里执行）。
// 点城里的门 → dm_rift.c 置「要开页」→ 这里（游戏主线程，dm_actions.c 每帧泵）替玩家开面板；点按经 ClosePanel:TouchRF_* 回到这里。
// 面板开着时走原版单机暂停（同暂停菜单，见 pause_set）：世界停、名牌 / 小地图标注不画。
// 写控件的手法照 dm_kills.c（按名字找、挪矩形、改字），只写我们自己的 Touch* 控件。
// Greater rift panel: uses original game widgets like the chronicle vault / kill board; it is a standalone panel, not attached as a chronicle tab.
// Layout _rift_panel in dm/ios/tools/touch_layout.py: borrows the shell of the original 「消息记录」(Message Log) panel MessageLogPanel (widgets TouchRift / TouchRF_*); data and actions go through dm_rift_menu* in dm_rift.c (executed in the server tick).
// Tapping the town door -> dm_rift.c sets "page wanted" -> here (game main thread, pumped every frame from dm_actions.c) the panel is opened on the player's behalf; taps come back here via ClosePanel:TouchRF_*.
// While the panel is open the original single-player pause is used (same as the pause menu, see pause_set): the world stops, nameplates / minimap markers are not drawn.
// Widgets are written the same way as in dm_kills.c (find by name, move rectangles, change text), writing only our own Touch* widgets.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>
#include "dm.h"
#include "dm_widget.h"
#include "dm_cursor.h"
#include "dm_rift.h"
#include "dm_i18n.h"
#include "dm_input.h"
void dm_ui_send(const char *cat, const char *act, const char *arg);   // dm_cursor.c
uint64_t dm_guest_frame(void);

static _Atomic int g_dirty = 1, g_step;   // g_step = 页上按了 ±1 / ±10 攒下的步数 | g_step = the steps accumulated from pressing ±1 / ±10 on the page
static int g_pair = -1;   // 英文版赌博区里「两个数并排」的那一行（没有 = -1；只在游戏主线程用） | the row with "two numbers side by side" in the gambling section of the English version (none = -1; used only on the game's main thread)
static _Atomic int g_act;                        // 1 开始、2 继续、3 放弃（主线程泵里执行） | 1 start, 2 continue, 3 abandon (executed in the main thread pump)
static int g_sel = -1, g_opened, g_open_f, g_seq;
static uint8_t *rb(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static uint64_t find(uint64_t w, const char *name, int dep) {
    if (!w || dep > 14) return 0; const char *nm = *(const char **)(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = find(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
static uint64_t panel_named(const char *nm) { uint8_t *b = rb(); uint64_t root = b ? *(uint64_t *)(b + 0x2f05458) : 0; return root ? dm_ui_top(root, nm) : 0; }   // 面板是界面根的直接子控件（dm_widget.h） | Panels are direct children of the UI root (dm_widget.h)
static uint64_t panel(void) { return panel_named("MessageLogPanel"); }
static int panel_up(void) { uint64_t p = panel(); return p && *(uint8_t *)(uintptr_t)(p + 0x51); }   // 面板可见字节 +0x51（原版暂停判定读 BugPanel 也是它） | Panel visible byte +0x51 (the original pause check also reads this for BugPanel)
static uint64_t body(void) { uint64_t cp = panel(), bd = cp ? find(cp, "TouchRift", 1) : 0; return bd ? find(bd, "TouchRF_Body", 0) : 0; }
// 原版单机暂停：主循环 0x2c14a0–0x2c1518 判「单机（游戏类型 [main+0x2d20b8c] == 0）且界面状态字节 [main+0x2d5ff51] | [main+0x2d5ff67] 非 0」即走暂停帧；
// 暂停菜单开着时原版置的就是 +0x2d5ff51。面板开着置 1、关了写回 0（只在我们置过时写回）。暂停帧不跑服务端收包与拍：开始 / 继续在关面板之后执行，放弃要先放开暂停（服务端拍里才换页）
// The original single-player pause: the main loop at 0x2c14a0–0x2c1518 takes the paused frame when "single-player (game type [main+0x2d20b8c] == 0) and UI state bytes [main+0x2d5ff51] | [main+0x2d5ff67] non-zero";
// +0x2d5ff51 is exactly what the original game sets while the pause menu is open. Set to 1 while the panel is open and written back to 0 when closed (only if we set it). Paused frames do not run the server packet receive or ticks: start / continue execute after the panel closes, and abandon must release the pause first (the page is only switched in the server tick)
static void pause_set(int on) { static int mine; uint8_t *b = rb(); if (!b) return; volatile uint8_t *f = b + 0x2d5ff51;
    if (on && !mine && *(uint32_t *)(b + 0x2d20b8c) == 0 && !*f) { *f = 1; mine = 1; LOG("大秘境面板：开着，走原版单机暂停（界面状态 +9 置 1）"); }
    else if (!on && mine) { mine = 0; *f = 0; LOG("大秘境面板：已关，放开暂停"); } }
// 门的头顶名牌不许压在面板上。名牌 = 界面根绘制 main+0xb0d028 每帧调 main+0x455a80(单位)，三路来源：目标槽（main+0x2082c90 + 玩家号 × 16，0xb0d140）、交互目标、输入管理器的单位；
// 后两路只在模态面板计数 [main+0x2d12740] == 0 时画（0xb0d15c），计数非 0 时目标更新 0x2f4320 也转去清目标槽（0x2f4384）。暂停字节不在这条链上（Mac 截图：置了暂停名牌照画）。
// 面板开着计数 +1、关了 −1（只在自己加过时减，原版 48 处写入也都是加减 1），加的当帧调原版清槽 main+0x2f3e40(玩家号, 0)（暂停帧里目标更新跑不跑没读清）
// The door's overhead nameplate must not cover the panel. Nameplate = the UI root draw main+0xb0d028 calls main+0x455a80(unit) every frame, from three sources: the target slot (main+0x2082c90 + player number × 16, 0xb0d140), the interaction target, and the input manager's unit;
// the latter two are drawn only when the modal panel count [main+0x2d12740] == 0 (0xb0d15c), and when the count is non-zero the target update 0x2f4320 also switches to clearing the target slot (0x2f4384). The pause byte is not on this chain (Mac screenshot: with pause set the nameplate is still drawn).
// Count +1 while the panel is open, −1 when closed (decrement only if we incremented; the original game's 48 write sites are all plus or minus 1 too); on the frame of the increment call the original slot clear main+0x2f3e40(player number, 0) (whether the target update runs in paused frames was not made clear by reading the code)
static void plate_hide(int on) { static int mine; uint8_t *b = rb(); if (!b) return; volatile int32_t *c = (volatile int32_t *)(b + 0x2d12740);
    if (on && !mine) { mine = 1; LOG("大秘境面板：模态面板计数 %d → %d，清目标槽（名牌不画）", *c, *c + 1); *c += 1; ((void (*)(uint32_t, uint64_t))(b + 0x2f3e40))(*(uint32_t *)(b + 0x2d20e4c), 0); }
    else if (!on && mine) { mine = 0; *c -= 1; LOG("大秘境面板：模态面板计数放回 %d", *c); } }
static void rect(uint64_t w, int x, int y) { if (!w) return; int32_t *r = (int32_t *)(w + 0x70); if (r[0] == x && r[1] == y) return; r[0] = x; r[1] = y; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x10))(w); }
static void text(uint64_t w, const char *s) {   // 同 dm_kills.c kb_text：文字控件（虚表 main+0x20ba4c8），串 +0x88 / 长 +0x90，改完 main+0xadc434 重排；字没变不写 | Same as kb_text in dm_kills.c: text widget (vtable main+0x20ba4c8), string +0x88 / length +0x90, re-layout with main+0xadc434 after changing; not written if the text is unchanged
    uint8_t *b = rb(); if (!w || *(uint64_t *)w - (uint64_t)(uintptr_t)b != 0x20ba4c8) return;
    size_t n = strlen(s); if (*(uint64_t *)(w + 0x90) == n && !memcmp(*(const char **)(w + 0x88), s, n)) return;
    ((void *(*)(uint64_t, const char *, uint64_t))(b + 0x136e5c))(w + 0x88, s, n); ((void (*)(uint64_t))(b + 0xadc434))(w);
}
// 按钮置灰：按钮控件四个帧号 +0x6e8 常态 / +0x6ec 按下 / +0x6f0 悬停 / +0x6f4 禁用，此刻的帧 +0x610（宿主读：两种帧号配置的按钮逐字段对过）。
// 置灰 = 四个都换成禁用帧（原值存着，恢复时写回）；点按照常发消息，宿主在边界不动层数
// Greying out a button: the button widget's four frame numbers +0x6e8 normal / +0x6ec pressed / +0x6f0 hover / +0x6f4 disabled, current frame +0x610 (read on the host: buttons with two frame-number configurations compared field by field).
// Greyed out = all four replaced with the disabled frame (original values are kept and written back on restore); taps still send messages as usual, and the host does not move the level at the limits
DM_WIDGET_RED_DEF
static void btn_grey(uint64_t w, int grey) {
    static struct { dm_wref r; int32_t f[4]; } sv[8]; static int n; if (!w || !DM_WIDGET_OK(rb(), w, DM_VT_BUTTON, "秘境面板")) return; int i = 0; while (i < n && dm_wref_get(&sv[i].r) != w) i++;
    if (i == n) { i = 0; while (i < n && dm_wref_get(&sv[i].r)) i++; if (i == 8) return; if (i == n) n++; dm_wref_set(&sv[i].r, w); memcpy(sv[i].f, (void *)(uintptr_t)(w + 0x6e8), 16); }   // 重建后旧条目代号不符、取到 0：腾出来给新控件用 | After a rebuild, an old entry's generation does not match and it reads 0: freed up for a new widget
    int32_t *f = (int32_t *)(uintptr_t)(w + 0x6e8); int32_t want[4]; for (int k = 0; k < 4; k++) want[k] = grey ? sv[i].f[3] : sv[i].f[k];
    if (memcmp(f, want, 16)) { memcpy(f, want, 16); *(int32_t *)(uintptr_t)(w + 0x610) = want[0]; }
}
static void color(uint64_t w, int r, int g, int bl) { if (!w) return; int32_t *c = (int32_t *)(w + 0xbc); c[0] = r; c[1] = g; c[2] = bl; }   // 字色 +0xbc / +0xc0 / +0xc4（dm_vault_ui.c） | Text color +0xbc / +0xc0 / +0xc4 (dm_vault_ui.c)
void dm_rift_ui_on_uimsg(const char *cat, const char *act, const char *arg) {
    if (strcmp(cat, "PanelManager") || strcmp(act, "ClosePanel") || strncmp(arg, "TouchRF_", 8)) return;
    const char *k = arg + 8;
    if (!strcmp(k, "M1")) atomic_fetch_add(&g_step, -1); else if (!strcmp(k, "P1")) atomic_fetch_add(&g_step, 1);
    else if (!strcmp(k, "M10")) atomic_fetch_add(&g_step, -10); else if (!strcmp(k, "P10")) atomic_fetch_add(&g_step, 10);
    else if (!strcmp(k, "Start")) atomic_store(&g_act, 1); else if (!strcmp(k, "Cont")) atomic_store(&g_act, 2); else if (!strcmp(k, "Abandon") || !strcmp(k, "Resel")) atomic_store(&g_act, 3);   // 已完成时的「重新选层」是和「放弃」同位置的另一个按钮，动作同放弃（服务端回选层页） | "Reselect level" when completed is a separate button at the same position as "Abandon", with the same action as Abandon (the server returns to the level selection page)
    atomic_store(&g_dirty, 1);
}
// 佣兵面板六个新装备格的暗红盖板（TouchMercLock_<部位>，布局 merc_slots.py）：没解锁时挪到各自的格子上，解锁了挪出屏。只写我们自己的 Touch* 控件
// Dark red covers for the six new equipment slots in the mercenary panel (TouchMercLock_<body location>, layout merc_slots.py): moved onto their cells when not unlocked, moved off-screen once unlocked. Only our own Touch* widgets are written
static void merc_lock_pump(void) {
    static const struct { const char *n; } k[] = { { "TouchMercLock_neck" }, { "TouchMercLock_gloves" }, { "TouchMercLock_right_hand" }, { "TouchMercLock_belt" }, { "TouchMercLock_left_hand" }, { "TouchMercLock_feet" } };
    uint64_t p = panel_named("HirelingInventoryPanel"); if (!p || !*(uint8_t *)(uintptr_t)(p + 0x51)) return; int lock = !dm_rift_merc_unlocked();
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {   // 盖板的横坐标取同部位物品格 slot_<部位> 的（布局里两者同一个矩形，盖板只是先放在屏外），格位只在布局脚本里定一处 | The cover's x is taken from the item cell slot_<body location> of the same body location (in the layout the two share one rectangle, the cover is just initially placed off-screen), so cell positions are defined in one place only, the layout script
        char sn[32]; snprintf(sn, sizeof sn, "slot_%s", k[i].n + 14); uint64_t w = find(p, k[i].n, 0), sl = find(p, sn, 0);
        if (w && sl) rect(w, lock ? ((int32_t *)(uintptr_t)(sl + 0x70))[0] : 99999, ((int32_t *)(uintptr_t)(w + 0x70))[1]); }
    // 没解锁时手柄把光标移到这六格之一就弹一句提示（触屏的点击 / 拖放在触摸层按命中结果弹，dm_cursor.c ui_classify）：界面的焦点控件 [界面根+0xd0]+0x190 / 悬停控件 +0x178 落在其中一格、且和上一帧不是同一格时弹一次。
    // 只读原版控件，不拦这一下（格子是空的，原版自己不会做什么）；副手格不在这六格里，不弹
    // When not unlocked, moving the controller cursor onto one of these six cells pops up a one-line hint (touch taps / drags pop it in the touch layer by hit result, dm_cursor.c ui_classify): it pops once when the UI's focus widget [UI root+0xd0]+0x190 / hover widget +0x178 lands on one of the cells and it is not the same cell as in the previous frame.
    // Only reads original widgets and does not block the press (the cell is empty, and the original game does nothing by itself); the off-hand cell is not among these six and gets no hint
    if (lock) { static dm_wref last; uint8_t *b = rb(); uint64_t pm = b ? *(uint64_t *)(b + 0x2f05458) : 0, fm = pm ? *(uint64_t *)(uintptr_t)(pm + 0xd0) : 0, cur = 0;
        for (int c = 0; fm && c < 2 && !cur; c++) { uint64_t w = *(uint64_t *)(uintptr_t)(fm + (c ? 0x178 : 0x190)); const char *nm = w ? *(const char **)(uintptr_t)(w + 8) : NULL;
            if (!nm || strncmp(nm, "slot_", 5) || !dm_widget_is_slot(b, w) || *(uint32_t *)(uintptr_t)(w + 0x5c8) != 1) continue;   // +0x5c8（属主）是物品格专有字段：先核类。指针本身不会悬空——原版控件析构（main+0xabee7c → 0xabdf4c）会把焦点管理器里指向自己的 +0x190 / +0x178 清掉 | +0x5c8 (owner) is an item-slot-only field: check the class first. The pointer itself cannot dangle: the game's widget destructor (main+0xabee7c → 0xabdf4c) clears the focus manager's +0x190 / +0x178 that point to it
            for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strcmp(nm + 5, k[i].n + 14)) cur = w; }
        if (cur && cur != dm_wref_get(&last)) dm_rift_merc_locked_hint(); dm_wref_set(&last, cur); }
    // 六个新物品格（布局里我们加的 slot_<部位>，原版物品格控件）：原版佣兵面板的初始化只给它认识的四个格填「身体部位号」（控件 +0x5d8：头 1 / 身 3 / 右手 4 / 左手 5）和格子尺寸（+0x5b8 两个 32 位 = 98），
    // 我们加的六个这两处是 0，控件就不知道该画佣兵哪个部位上的物品（客户端佣兵身上有这些物品，格子却是空的——实测）。面板每次建出来时补一次，值照玩家物品栏面板里同名格的（2 / 6 / 7 / 8 / 9 / 10）
    // The six new item cells (slot_<body location> that we add in the layout, original item cell widgets): the original mercenary panel's initialization fills in the "body location number" (widget +0x5d8: head 1 / torso 3 / right hand 4 / left hand 5) and the cell size (+0x5b8, two 32-bit values = 98) only for the four cells it knows,
    // for the six we add these two are 0, so the widget does not know which mercenary body location's item to draw (the client mercenary has these items on it, yet the cells are empty -- measured). Filled in once each time the panel is built, with the values of the same-named cells in the player inventory panel (2 / 6 / 7 / 8 / 9 / 10)
    { static const struct { const char *n; uint32_t loc; } sl[] = { { "slot_neck", 2 }, { "slot_right_hand", 6 }, { "slot_left_hand", 7 }, { "slot_belt", 8 }, { "slot_feet", 9 }, { "slot_gloves", 10 } };
      for (unsigned i = 0; i < sizeof sl / sizeof *sl; i++) { uint64_t w = find(p, sl[i].n, 0); if (!w || *(uint64_t *)w - (uint64_t)(uintptr_t)rb() != 0x2089fa0 || *(uint32_t *)(uintptr_t)(w + 0x5d8) == sl[i].loc) continue;
          *(uint32_t *)(uintptr_t)(w + 0x5d8) = sl[i].loc; *(uint64_t *)(uintptr_t)(w + 0x5b8) = 0x6200000062ull; LOG("佣兵装备位：面板里的 %s 补上部位号 %u", sl[i].n, sl[i].loc); } }
}
void dm_rift_ui_pump(void) {
    merc_lock_pump();
    uint8_t *b = rb(); if (!b) return; uint64_t fr = dm_guest_frame();
    dm_rift_menu_t m; int want = dm_rift_menu(&m), up = panel_up();
    if (!want) { if (g_opened) { g_opened = 0; g_sel = -1; } }
    else if (!g_opened && getenv("DM_RIFT_TEST_KB")) { g_opened = 3; g_open_f = (int)fr; dm_ui_send("PanelManager", "OpenPanel", "ChroniclePanel"); return; }   // 测试：开到编年史积分牌「秘境」项（看积分牌秘境页） | Test: open to the 「秘境」(Rift) entry of the chronicle kill board (to view the kill board rift page)
    else if (g_opened == 3) { uint64_t cp = panel_named("ChroniclePanel"); if (cp && (int)fr - g_open_f > 6) { g_opened = 2;
        dm_ui_send("ChroniclePanelMessage", "ToggleChildPanel", "TouchKillBoard"); dm_ui_send("PanelManager", "ClosePanel", "TouchKB_Act5");
        uint64_t tb = find(cp, "ChronicleTabs", 2); if (tb) *(int32_t *)(tb + 0xdc4) = 3; } return; }
    // 面板状态只认屏上的事实（面板可见字节，原版自己判面板开没开也读它），不数帧（旧实现关面板后没人清状态，再点门不开）：
    //   0 没开 → 发 OpenPanel → 1 已请求 →（可见了）→ 2 在屏上 →（不可见了：关闭钮 / 点面板外 / 游戏自己收掉，不论有没有配对的关消息）→ 清「要开页」回 0。
    //   停在 1 时玩家又点了一次门（点门次数变了）就再发一次 OpenPanel
    // Panel state trusts only on-screen facts (the panel visible byte, which the original game itself also reads to tell whether a panel is open), not frame counting (the old implementation had nobody clearing the state after the panel closed, so tapping the door again did not open it):
    //   0 not open -> send OpenPanel -> 1 requested -> (became visible) -> 2 on screen -> (became invisible: close button / tap outside the panel / closed by the game itself, whether or not there was a matching close message) -> clear "page wanted" and return to 0.
    //   If the player taps the door again while stuck at 1 (the door tap count changed), send OpenPanel once more
    else if (!g_opened || (g_opened == 1 && !up && dm_rift_menu_seq() != g_seq)) { g_opened = 1; g_seq = dm_rift_menu_seq(); g_sel = -1; dm_ui_send("PanelManager", "OpenPanel", "MessageLogPanel"); LOG("大秘境面板：点了门，开面板（%s）", m.menu == 3 ? "离开这一场的确认" : m.menu == 2 ? "继续 / 放弃" : "选层"); return; }
    if (g_opened == 1 && up) g_opened = 2;
    if (want && g_opened == 2 && !up) { LOG("大秘境面板：玩家关了面板"); dm_rift_menu_close(); g_opened = 0; g_sel = -1; }
    pause_set(up && !dm_rift_menu_pending()); plate_hide(up);
    uint64_t bw = body(); if (!bw || !up) return;
    if (!want) { m.menu = 1; dm_rift_menu_peek(&m); }   // 玩家自己翻到这一页（没点门）：照样显示选层，只是「开始」要在城里才执行 | The player browsed to this page themselves (no door tap): level selection is shown all the same, but 「开始」(Enter Rift) only executes while in town
    { static const char *au = (const char *)1; static int at; if (au == (const char *)1) au = getenv("DM_RIFT_MENU_AUTO");   // 测试开关 DM_RIFT_MENU_AUTO=<层>[,cont|,abandon]：页显示 90 帧后替玩家点（脚本触点打不到被挡的门 / 按钮另用系统鼠标事件验） | Test switch DM_RIFT_MENU_AUTO=<level>[,cont|,abandon]: taps on the player's behalf 90 frames after the page is shown (scripted touch points cannot hit a blocked door / buttons are verified separately with system mouse events)
      if (au && want) { if (!at) at = (int)fr; if ((int)fr - at > 90) { at = 0; static int lg; if (lg++ < 4) LOG("秘境选层页：测试开关替玩家点 %s（页 %d）", au, m.menu);
          if (m.menu == 3) { static int nth; int cancel = strstr(au, "cancel") && !nth++;   // 确认页：值里带 cancel = 第一次出现时关掉（当作取消），之后带 next 的点「继续」 | confirmation page: when the value contains cancel = close it the first time it appears (treated as cancel), afterwards press "Continue" on the one carrying next
              if (cancel) dm_ui_send("PanelManager", "ClosePanel", "MessageLogPanel"); else if (strstr(au, "next")) atomic_store(&g_act, 2); }
          else if (m.menu == 1 && atoi(au) > 0) { g_sel = atoi(au); if (g_sel > m.unlock) g_sel = m.unlock; atomic_store(&g_act, 1); } else if (m.menu == 2 && (strstr(au, "cont") || strstr(au, "abandon"))) atomic_store(&g_act, strstr(au, "abandon") ? 3 : 2); } } else at = 0; }
    // 手柄模式：这一页的按钮原版焦点导航不认识，照积分牌 / 编年史仓库页的做法接管本页的键（dm_padmode_page_keys），走的还是下面同一套动作号：
    //   选层页 左 / 右 = −1 / +1、下 / 上 = −10 / +10、A = 开始；进行中 / 已完成页 A = 继续、X = 放弃 / 重新选层（放弃不可撤销，不放在 A 上）；B 没接管 = 原版关面板。
    //   键位说明走原版底部提示条（动作号 0 = A、2 = X），字用按钮上的同一个串
    // Controller mode: the original focus navigation does not know this page's buttons, so the page's keys are taken over the same way as on the kill board
    // / Chronicle vault pages (dm_padmode_page_keys), still going through the same action numbers below:
    //   level selection page: left / right = −1 / +1, down / up = −10 / +10, A = start; in-progress / completed pages: A = continue, X = quit / reselect the
    //   level (quitting is irreversible, so it is not on A); B is not taken over = the original game closes the panel.
    //   The key legend uses the original bottom button legend (action number 0 = A, 2 = X), with the same strings as on the buttons
    { int dm_padmode_current(void); uint64_t dm_padmode_page_keys(uint64_t); void dm_padmode_legend(int, const char *);
      { int kx = dm_padmode_current() ? 52 : 99999, px = kx;
#if TARGET_OS_OSX
        // Mac App：玩家用键盘时按钮左端显示键盘键帽（布局里名字加 K 的那几个，只有 Mac 的覆盖层带），手柄有输入时显示手柄键帽；两套同位，一套在屏上另一套挪出屏
        // Mac App: when the player uses the keyboard, the left end of a button shows the keyboard key cap (the ones whose layout names end in K, carried only by the Mac overlay), and when a gamepad has input the gamepad key cap is shown; the two sets share a position, one on screen and the other moved off screen
        { int dm_padmode_kbd(void); int qx = 99999; if (dm_padmode_kbd()) { qx = kx; px = 99999; }
          rect(find(bw, "TouchRF_KeyAK", 0), qx, 1181); rect(find(bw, "TouchRF_KeyA2K", 0), qx, 1181); rect(find(bw, "TouchRF_KeyXK", 0), m.menu == 3 ? 99999 : qx, 844); }
#endif
        rect(find(bw, "TouchRF_KeyA", 0), px, 1181); rect(find(bw, "TouchRF_KeyA2", 0), px, 1181); rect(find(bw, "TouchRF_KeyX", 0), m.menu == 3 ? 99999 : px, 844); }   // 确认页上面那个钮不在，它的键帽也不显示   // 按钮左端的原版键帽：只在手柄模式显示 | the upper button is absent on the confirmation page, so its key cap is not shown either
      { uint64_t an = find(panel(), "TouchRF_Anchor", 1); int pad = dm_padmode_current(); rect(find(an, "TouchRF_CloseX", 1), pad ? 99999 : 1766, 64); rect(find(an, "TouchClose", 1), pad ? 99999 : 1705, -11); rect(find(an, "TouchRF_CloseBtn", 1), pad ? 1868 : 99999, 8); }   // 外框右上角的关闭钮只留一个：手柄 / 键鼠显示原版红框（按 B 或鼠标点），触屏显示金色 X（与别的面板同图同处），另一个挪出屏 | only one close button stays at the top right of the frame: gamepad / keyboard-and-mouse show the original red frame (press B or click it), touch shows the gold X (same image and place as other panels), and the other one is moved off screen
      if (dm_padmode_current()) { int ch = m.menu >= 2, cf = m.menu == 3;   // cf：确认页，只有 A = 继续；取消 = B 关面板（原版） | cf: confirmation page, only A = continue; cancel = B closes the panel (as in the original)
          uint64_t ev = dm_padmode_page_keys(DM_BTN_A | DM_BTN_X | DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_UP | DM_BTN_DOWN);
          dm_padmode_legend(0, dm_str(b, ch ? "dmRiftBtnCont" : "dmRiftBtnStart")); if (ch && !cf) dm_padmode_legend(2, dm_str(b, m.done ? "dmRiftBtnResel" : "dmRiftBtnAbandon"));
          if (!ch) { int st = (ev & DM_BTN_LEFT ? -1 : 0) + (ev & DM_BTN_RIGHT ? 1 : 0) + (ev & DM_BTN_DOWN ? -10 : 0) + (ev & DM_BTN_UP ? 10 : 0); if (st) atomic_fetch_add(&g_step, st); if (ev & DM_BTN_A) atomic_store(&g_act, 1); }
          else if (ev & DM_BTN_A) atomic_store(&g_act, 2);
          else if (cf) { }
          else if (m.done) { if (ev & DM_BTN_X) atomic_store(&g_act, 3); }   // 已完成页的 X = 重新选层（进度已记，不丢东西）：按一下 | X on the completed page = reselect the level (progress is already recorded, nothing is lost): a single press
          else { static uint64_t t0; uint64_t now_ns(void), dm_padmode_page_held(void), now = now_ns();   // 进行中页的 X = 放弃（本层作废、不可撤销）：按住 0.8 秒才算，防误按 | X on the in-progress page = quit (this level is forfeited, irreversible): it only counts after being held for 0.8 seconds, to prevent accidental presses
              if (!(dm_padmode_page_held() & DM_BTN_X)) t0 = 0; else if (!t0) t0 = now; else if (t0 != 1 && now - t0 >= 800000000ull) { t0 = 1; atomic_store(&g_act, 3); LOG("大秘境面板：手柄按住 X 0.8 秒 → 放弃"); } }
          if (ev) { atomic_store(&g_dirty, 1); LOG("大秘境面板：手柄键 %#llx（%s页）", (unsigned long long)ev, cf ? "确认" : ch ? "继续 / 放弃" : "选层"); } } }
    int a = atomic_exchange(&g_act, 0);
    if (g_sel < 0) g_sel = m.def < 1 ? 1 : m.def > m.unlock ? m.unlock : m.def;
    { int st = atomic_exchange(&g_step, 0); if (st) { g_sel += st; if (g_sel < 1) g_sel = 1; if (g_sel > m.unlock) g_sel = m.unlock; } }
    if (a == 1 && m.menu == 1) { dm_rift_menu_start(g_sel); pause_set(0); dm_ui_send("PanelManager", "ClosePanel", "MessageLogPanel"); return; }
    if (a == 2 && m.menu == 2) { dm_rift_menu_continue(); pause_set(0); dm_ui_send("PanelManager", "ClosePanel", "MessageLogPanel"); return; }
    if (a == 2 && m.menu == 3) { LOG("大秘境面板：确认页点了继续"); dm_rift_menu_next(); pause_set(0); dm_ui_send("PanelManager", "ClosePanel", "MessageLogPanel"); return; }
    if (a == 3 && m.menu == 2) { dm_rift_menu_abandon(); pause_set(0); g_sel = -1; return; }   // 放弃后服务端拍里把页换回选层（面板不关；放开暂停让拍走到） | After abandon, the server tick switches the page back to level selection (the panel stays open; release the pause so the tick gets there)
    static int frame; if (!atomic_exchange(&g_dirty, 0) && ++frame % 15) return;
    rect(find(bw, "TouchRF_Sel", 0), m.menu >= 2 ? 99999 : 0, 0); rect(find(bw, "TouchRF_Cho", 0), m.menu >= 2 ? 0 : 99999, 0);
    char t[128];
    // 英文版：左栏「标签 + 右对齐的值」英文比中文宽，标签写全称用中号字会顶到数值。布局里每行另有一个小一号的标签控件 …LE（缺省在屏外）：
    // 游戏语言不是中文时用它、把中号那个挪出去，数值列再右移 16（右缘 496，离右边列表 530 留 34）。中文：中号标签在原位、数值列不动，画面不变
    // English version: in the left column's "label + right-aligned value" rows English is wider than Chinese, and a full-name label in the medium font would
    // run into the value. The layout has an extra, one-size-smaller label widget ...LE for each row (off screen by default):
    // when the game language is not Chinese it is used and the medium one is moved out, and the value column shifts right by another 16 (right edge 496,
    // leaving 34 before the list on the right at 530). Chinese: the medium label stays in place, the value column does not move, and the screen is unchanged
    int en = !dm_lang_zh();
    { int dx = en ? 16 : 0; static const char *kv[] = { "Range", "Best", "Str", "Lvl", "Time" };
      for (int i = 0; i < 5; i++) { int y = 414 + 64 * i; snprintf(t, sizeof t, "TouchRF_%s", kv[i]); rect(find(bw, t, 0), 230 + dx, y);
          snprintf(t, sizeof t, "TouchRF_%sL", kv[i]); rect(find(bw, t, 0), en ? 99999 : 0, y); snprintf(t, sizeof t, "TouchRF_%sLE", kv[i]); rect(find(bw, t, 0), en ? 0 : 99999, y + (en ? 4 : 0)); }
      for (int i = 0; i < 4; i++) { int y = 842 + 64 * i; for (int c = 0; c < 2; c++) { snprintf(t, sizeof t, "TouchRF_Gam%d%c", i, "US"[c]); rect(find(bw, t, 0), 230 + dx - (c == 0 && i == g_pair ? 210 : 0), y); }   // 成对行：金字那个值左移 210，和绿字并排（170 时两个数几乎贴着，Mac 截图） | paired row: the gold value is shifted left by 210 to sit beside the green one (at 170 the two numbers almost touch, per Mac screenshot)
          snprintf(t, sizeof t, "TouchRF_Gam%dL", i); rect(find(bw, t, 0), en ? 99999 : 0, y); snprintf(t, sizeof t, "TouchRF_Gam%dLE", i); rect(find(bw, t, 0), en ? 0 : 99999, y + (en ? 4 : 0)); } }
    rect(find(bw, "TouchRF_MsN", 0), en ? 99999 : 1030, -24); rect(find(bw, "TouchRF_MsNE", 0), en ? 930 : 99999, -24);   // 右上那句：英文用更宽、字更小的那个控件；纵坐标 −24 = touch_layout.py 的 RF_T0（第一行小标题整体上抬） | the top-right sentence: English uses the wider widget with the smaller font; y coordinate −24 = RF_T0 in touch_layout.py (the first row of small headings is raised as a whole)
    rect(find(bw, "TouchRF_AbT", 0), en ? 99999 : 0, 746); rect(find(bw, "TouchRF_AbTE", 0), en ? 0 : 99999, 746); rect(find(bw, "TouchRF_ContT", 0), en ? 99999 : 0, 1083); rect(find(bw, "TouchRF_ContTE", 0), en ? 0 : 99999, 1083);
    if (m.menu == 3) {
        // 守护者处那扇门的确认页（公开仓库）：借「继续 / 放弃」那一页的控件，不加新控件——
        // 标题问一句、三行说明后果、下面那个「继续」钮 = 确认；上面那个钮（放弃 / 重新选层）两个都挪走，说明写「关闭本页 = 留在这里」。取消 = 右上角 X / 手柄 B / 点面板外
        // Confirmation page of the door at the Guardian: it borrows the widgets of the "Continue / Abandon" page and adds no new widget -
        // a title that asks the question, three lines explaining the consequence, and the lower "Continue" button = confirm; the upper button (Abandon / choose level again) is moved away in both variants, and the explanation says "closing this page = stay here". Cancel = the X at the top right / gamepad B / a tap outside the panel
        int retry = m.next <= m.tier;   // 没有更高的层可去（超时未过关，或已在最高层）：那扇门是重打 | no higher level to go to (overtime without passing, or already at the highest level): that door is a replay
        snprintf(t, sizeof t, dm_str(rb(), "dmRiftTierN"), m.next); text(find(bw, "TouchRF_ChTier", 0), t);
        text(find(bw, "TouchRF_ChT", 0), dm_str(rb(), retry ? "dmRiftTitleRetry" : "dmRiftTitleNext")); text(find(bw, "TouchRF_ChNote", 0), "");
        text(find(bw, "TouchRF_ChLeftL", 0), dm_str(rb(), "dmRiftLeaveWarn1")); text(find(bw, "TouchRF_ChLeft", 0), ""); text(find(bw, "TouchRF_ChProgL", 0), dm_str(rb(), "dmRiftLeaveWarn2")); text(find(bw, "TouchRF_ChProg", 0), ""); text(find(bw, "TouchRF_ChNoteL", 0), "");
        rect(find(bw, "TouchRF_Abandon", 0), 99999, 818); rect(find(bw, "TouchRF_Resel", 0), 99999, 818);
        text(find(bw, en ? "TouchRF_AbTE" : "TouchRF_AbT", 0), dm_str(rb(), "dmRiftCapStay"));
        snprintf(t, sizeof t, dm_str(rb(), "dmRiftCapGo"), m.next); text(find(bw, en ? "TouchRF_ContTE" : "TouchRF_ContT", 0), t);
    } else if (m.menu == 2) {
        snprintf(t, sizeof t, dm_str(rb(), "dmRiftTierN"), m.tier); text(find(bw, "TouchRF_ChTier", 0), t);
        // 已打倒守护者：两句话各占一整行（写进左边的标签控件、值留空）；写进右对齐的值控件会压住标签（Mac 截图）
        // 「本层」三行 + 放弃上方的短句，按守护者打倒没有分两套（已打倒时放弃不丢进度：最高通关照记）。已打倒的三句各占一整行，写进左边的标签控件、值留空
        // Guardian defeated: the two sentences each take a full row (written into the label widget on the left, value left empty); writing into the right-aligned value widget would cover the label (Mac screenshot)
        // The three 「本层」(This Level) rows + the short sentence above Abandon come in two sets depending on whether the guardian is defeated (abandoning after it is defeated loses no progress: the highest clear is still recorded). The three sentences for the defeated case each take a full row, written into the label widget on the left with the value left empty
        { int dm_padmode_current(void); text(find(bw, en ? "TouchRF_AbTE" : "TouchRF_AbT", 0), dm_str(rb(), m.done ? "dmRiftCapResel" : dm_padmode_current() && !dm_ui_pc_desktop() ? "dmRiftCapVoidPad" : "dmRiftCapVoid")); }   // 手柄模式：放弃要按住 X（见上面的手柄段） | Controller mode: abandoning requires holding X (see the controller section above)
        // 已完成页：上面一组 = 说明「换一层再打」+ 按钮「重新选层」（进度已记，叫「放弃」不对），下面一组 = 说明「回到守护者处」+ 按钮「继续」；
        // 未完成页 = 「本层作废 / 放弃」「回到离开时的位置 / 继续」。「重新选层」是和「放弃」同位置的另一个按钮，挪进挪出（按钮上的字宿主写不了）
        // Completed page: upper group = description "try another level" + button "Reselect level" (progress is already recorded, so calling it "Abandon" is
        // wrong), lower group = description "return to the Guardian's spot" + button "Continue";
        // uncompleted page = "this level is void / Abandon" and "return to the position at the time of leaving / Continue". "Reselect level" is a separate
        // button at the same position as "Abandon", moved in and out (the host cannot write button text)
        rect(find(bw, "TouchRF_Abandon", 0), m.done ? 99999 : 0, 818); rect(find(bw, "TouchRF_Resel", 0), m.done ? 0 : 99999, 818);
        text(find(bw, en ? "TouchRF_ContTE" : "TouchRF_ContT", 0), dm_str(rb(), m.done ? "dmRiftCapGuard" : m.overtime ? "dmRiftCapContOver" : "dmRiftCapCont"));
        text(find(bw, "TouchRF_ChNote", 0), ""); text(find(bw, "TouchRF_ChT", 0), dm_str(rb(), m.done ? (m.inc ? "dmRiftTitleDone" : "dmRiftTitleLate") : m.overtime ? "dmRiftTitleLate" : "dmRiftTitleRun"));   // 进行中但已超时：标题同「超时未过关」 | in progress but overtime: the title is the same as "Time's Up"
        if (m.done) { text(find(bw, "TouchRF_ChLeftL", 0), dm_str(rb(), "dmRiftGuardDown")); text(find(bw, "TouchRF_ChLeft", 0), ""); text(find(bw, "TouchRF_ChProgL", 0), dm_str(rb(), m.inc ? "dmRiftNextOpen" : "dmRiftRetryOpen")); text(find(bw, "TouchRF_ChProg", 0), ""); text(find(bw, "TouchRF_ChNoteL", 0), dm_str(rb(), m.inc ? "dmRiftSaved" : "dmRiftNotCounted")); }
        else { int s = m.secs_left; text(find(bw, "TouchRF_ChLeftL", 0), dm_str(rb(), "dmRiftTimeLeft")); snprintf(t, sizeof t, "%d:%02d", s / 60, s % 60); text(find(bw, "TouchRF_ChLeft", 0), t);
            text(find(bw, "TouchRF_ChProgL", 0), dm_str(rb(), "dmRiftProgress")); snprintf(t, sizeof t, "%d%%", (int)(m.progress * 100)); text(find(bw, "TouchRF_ChProg", 0), t); text(find(bw, "TouchRF_ChNoteL", 0), ""); }
    } else {
        snprintf(t, sizeof t, "%d", g_sel); text(find(bw, "TouchRF_Num", 0), t);
        btn_grey(find(bw, "TouchRF_M1", 0), g_sel <= 1); btn_grey(find(bw, "TouchRF_M10", 0), g_sel <= 1); btn_grey(find(bw, "TouchRF_P1", 0), g_sel >= m.unlock); btn_grey(find(bw, "TouchRF_P10", 0), g_sel >= m.unlock);   // 到边界置灰 | Greyed out at the limits
        snprintf(t, sizeof t, "1 – %d", m.unlock); text(find(bw, "TouchRF_Range", 0), t);
        snprintf(t, sizeof t, "%d", m.best); text(find(bw, "TouchRF_Best", 0), t);
        // 「怪物强度」= 这一层怪物生命的总倍率（人数档 × 等级 × 41 层起的额外加成，相对地狱 /players 1 的原生怪）、「怪物等级」= 这一层的等级，都取秘境主逻辑的同一套算法
        // "Monster Power" = the total life multiplier of this level's monsters (player-count factor × level × the extra bonus from level 41 on, relative to a
        // native Hell /players 1 monster); "Monster Level" = this level's level; both come from the same algorithm as the main Rift logic
        int lv = dm_rift_tier_level(g_sel, m.clvl), hx = dm_rift_tier_hp_x10(g_sel, m.clvl);
        if (hx < 100) snprintf(t, sizeof t, "×%d.%d", hx / 10, hx % 10); else snprintf(t, sizeof t, "×%d", (hx + 5) / 10); text(find(bw, "TouchRF_Str", 0), t);
        if (lv) snprintf(t, sizeof t, "%d", lv); else snprintf(t, sizeof t, "%s", dm_str(rb(), "dmRiftLvlBase")); text(find(bw, "TouchRF_Lvl", 0), t);
        text(find(bw, "TouchRF_Time", 0), "15:00");
        uint32_t gu, gs, nu, ns; dm_rift_gamble_vals(m.best, &gu, &gs); dm_rift_gamble_vals(g_sel, &nu, &ns);   // 安亚赌博：当前（按最高通关）+ 通关所选层后 | Anya gambling: current (by highest clear) + after clearing the selected level
        // 四行：通关过 = 暗金 / 套装（当前）；所选层高于最高通关再加「通关后暗金 / 通关后套装」（通关所选层之后的值）；没通关过只有后两行
        // Four rows: has cleared = unique / set (current); if the selected level is above the highest clear, add 「通关后暗金 / 通关后套装」(unique after clear / set after clear) (the values after clearing the selected level); never cleared = only the last two rows
        char ln[4][3][40]; memset(ln, 0, sizeof ln); int r = 0;
        if (gu) { snprintf(ln[r][0], 40, "%s", dm_str(rb(), "dmRiftGamUni")); snprintf(ln[r++][1], 40, "%u.%u%%", gu / 1000, gu / 100 % 10); snprintf(ln[r][0], 40, "%s", dm_str(rb(), "dmRiftGamSet")); snprintf(ln[r++][2], 40, "%u.%u%%", gs / 1000, gs / 100 % 10); }
        // 英文（别硬造「Unique after / Set after」）：「通关后」那两行换成一行小标题「After This Level」+ 一行两个数并排（金 = Unique、绿 = Set，同上面两行的颜色）
        // English (do not force "Unique after / Set after"): the two "after clearing" rows become one small heading "After This Level" plus one row with two
        // numbers side by side (gold = Unique, green = Set, same colors as the two rows above)
        int pair = -1;
        if ((!gu || g_sel > m.best) && en) { snprintf(ln[r++][0], 40, "%s", dm_str(rb(), "dmRiftGamAfterEn")); pair = r; snprintf(ln[r][1], 40, "%u.%u%%", nu / 1000, nu / 100 % 10); snprintf(ln[r++][2], 40, "%u.%u%%", ns / 1000, ns / 100 % 10); }
        else if (!gu || g_sel > m.best) { snprintf(ln[r][0], 40, "%s", dm_str(rb(), "dmRiftGamUniAfter")); snprintf(ln[r++][1], 40, "%u.%u%%", nu / 1000, nu / 100 % 10); snprintf(ln[r][0], 40, "%s", dm_str(rb(), "dmRiftGamSetAfter")); snprintf(ln[r++][2], 40, "%u.%u%%", ns / 1000, ns / 100 % 10); }
        g_pair = pair;
        for (int k = 0; k < 4; k++) for (int c = 0; c < 3; c++) { char nm[24]; snprintf(nm, sizeof nm, "TouchRF_Gam%d%s", k, c == 0 ? (en ? "LE" : "L") : c == 1 ? "U" : "S"); text(find(bw, nm, 0), ln[k][c]); }   // 标签写进当前语言用的那个控件（另一个在屏外） | the label is written into the widget used for the current language (the other one is off screen)
    }
    int nxt = -1; for (int i = 0; i < 10; i++) if (!(m.ms & (1u << i))) { nxt = i; break; }
    uint64_t list = find(bw, "TouchRF_List", 4);
    for (int i = 0; i < 10 && list; i++) { char nm[20]; snprintf(nm, sizeof nm, "TouchRF_Row%d", i); uint64_t w = find(list, nm, 0); if (!w) continue;
        uint64_t spanish_name = find(w, "NameEs", 0);
        if (spanish_name) { int es = dm_lang_es(); rect(find(w, "Name", 0), es ? 99999 : 330, 0); rect(spanish_name, es ? 330 : 99999, 0); }
        int got = (m.ms >> i) & 1; uint64_t st = find(w, "State", 0);
        text(st, got ? dm_str(rb(), "dmRiftMsGot") : i == nxt ? dm_str(rb(), "dmRiftMsNext") : dm_str(rb(), "dmRiftMsNo"));
        if (got) color(st, 0, 255, 0); else if (i == nxt) color(st, 255, 215, 0); else color(st, 160, 160, 160); }
}
