// nn::hid（手柄 / 触屏）：真机经 hid 服务读共享内存；宿主从 dm_input 当前状态取，由前端（Mac 键盘 / iOS 触屏）写入。
// 连接的手柄跟运行模式走：DM_OPMODE=1（主机，默认）→ 1P 全键手柄；0（掌机）→ 掌机手柄。震动不支持（设备数 0）。
// nn::hid (controller / touchscreen): on the real device shared memory is read through the hid service; the host takes it from the current dm_input state, written by the front end (Mac keyboard / iOS touchscreen).
// The connected controller follows the operation mode: DM_OPMODE=1 (console, default) → 1P full-key controller; 0 (handheld) → handheld controller. Vibration is not supported (device count 0).
#include "hle.h"
#include "dm_prof.h"
#include "dm_uiscreen.h"
#include "dm_actions.h"
#include "dm_cursor_item.h"
#include <unistd.h>
#include "dm_input.h"
#include "front.h"
#include <stdatomic.h>
#include "dm_cursor.h"
#include <math.h>
_Atomic int g_dm_l_presses;

enum { NPAD_HANDHELD = 0x20 };
enum { STYLE_FULLKEY = 1u << 0, STYLE_HANDHELD = 1u << 1 };
enum { ATTR_CONNECTED = 1u << 0 };

// nn::hid::NpadFullKeyState / NpadHandheldState 布局相同
// nn::hid::NpadFullKeyState / NpadHandheldState have the same layout
typedef struct {
    int64_t sampling;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes;
    uint32_t _pad;
} NpadState;
_Static_assert(sizeof(NpadState) == 0x28, "NpadState");

static struct { _Atomic uint64_t buttons; _Atomic int32_t lx, ly, rx, ry; } g_in;
static _Atomic int64_t g_sampling;
static _Atomic uint64_t g_sampled_right;
void dm_input_sampled_right(int32_t *rx,int32_t *ry) {
    uint64_t v=atomic_load_explicit(&g_sampled_right,memory_order_acquire);
    *rx=(int32_t)(uint32_t)v;*ry=(int32_t)(uint32_t)(v>>32);
}

// 测试开关 DM_TEST_HID_STALL_MS=<毫秒>：前端发布的 A 每出现一次按下沿，游戏下一次取手柄就先睡这么久再读——模拟「点下去那一刻游戏卡顿」。
// 开功能页后游戏约 0.4 s 没取输入，墙钟 0.12 s 的点按脉冲整段被漏掉（点了没反应）；前端现按 dm_input_samples 收脉冲
// Test switch DM_TEST_HID_STALL_MS=<milliseconds>: each time a press edge of the A published by the front end appears, the game's next controller read first sleeps this long before reading -- simulates "the game stalls at the moment of the tap".
// After opening the function page the game did not read input for about 0.4 s, and a tap pulse of 0.12 s wall clock was lost entirely (tapped, no response); the front end now ends the pulse by dm_input_samples
static int g_stall_ms = -1; static _Atomic int g_stall_due;
// 按下沿锁存：前端发布的键从「没按」变「按下」时记进 g_press_latch，游戏下一次取手柄时一并给出、给出后清掉。
// 按下与抬起落在游戏两次取手柄之间（脚本落后时两步连发、或游戏读条 / 开面板卡住取输入）原先整下按键被漏掉—— 真机 rf1 第 2、3 次重开：
// 底栏菜单键按下到抬起只隔 0.09 ms，游戏没看到，功能页没开。锁存只多给一次采样，之后照前端现值（抬起就是抬起）
// Press-edge latch: when a key published by the front end goes from "not pressed" to "pressed" it is recorded in g_press_latch, delivered along with the game's next controller read and cleared once delivered.
// When press and release fell between two controller reads of the game (two steps sent back to back when the script lags, or the game stalling input reads while loading / opening a panel) the whole key press used to be lost -- real device rf1, restarts 2 and 3:
// the bottom bar menu key had only 0.09 ms from press to release, the game did not see it and the function page did not open. The latch only gives one extra sample, afterwards the front end's current value applies (released is released)
static _Atomic uint64_t g_press_latch;
// 手柄（dm_input.h dm_pad_set）：按键与四轴打包在两个原子量里，按下沿同样锁存到游戏下一次取（手柄的短按不漏）
// Controller (dm_input.h dm_pad_set): buttons and the four axes are packed into two atomics, and press edges are likewise latched until the game's next
// read (short controller presses are not lost)
static _Atomic uint64_t g_pad_btn, g_pad_axes, g_pad_latch;
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_pad_btn, buttons); atomic_fetch_or(&g_pad_latch, buttons & ~old);
    atomic_store(&g_pad_axes, (uint64_t)(uint16_t)lx | (uint64_t)(uint16_t)ly << 16 | (uint64_t)(uint16_t)rx << 32 | (uint64_t)(uint16_t)ry << 48);
}
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (g_stall_ms < 0) { const char *e = getenv("DM_TEST_HID_STALL_MS"); g_stall_ms = e ? atoi(e) : 0; if (g_stall_ms) LOG("DM_TEST_HID_STALL_MS：A 每次按下沿后游戏取手柄先睡 %d ms", g_stall_ms); }
    if (g_stall_ms > 0 && (buttons & DM_BTN_A) && !(g_in.buttons & DM_BTN_A)) atomic_store(&g_stall_due, 1);
    atomic_fetch_or(&g_press_latch, buttons & ~g_in.buttons);   // 新按下的键记下来，保证游戏至少取到一次（见 fill） | record newly pressed keys, guaranteeing the game reads them at least once (see fill)
    g_in.buttons = buttons; g_in.lx = lx; g_in.ly = ly; g_in.rx = rx; g_in.ry = ry;
}
// 测试开关 DM_UI_TAPS="帧:x,y[:动作];…"（x / y = 画面千分比）。动作缺省 = 点一下（走 dm_ui_touch：按下、隔 6 帧抬起）；其余走 dm_ui_ptr（探查 / 界面用例用）：
//   k 点页签 / 对话菜单项（宿主按键切过去）；t<帧数> 手指按住这么多帧再抬（走 dm_ui_touch）；h<帧数> 悬停；p<帧数> 左键按住这么多帧再抬；r 右键；l 左键（指针路）；d<x2>,<y2> 按住拖到另一点（20 帧走完）；g<x2>,<y2> 连续手势（dm_ui_drag_*：按起止方向定型为滚动 / 滑块，60 帧走完）；W 点游戏世界（dm_world_tap）；N<值> 数量框设值（dm_num_set）；A<号> 买卖 / 加点入口（dm_actions.c）；w<量> 滚轮（120 一格，负 = 向下）；m<0|1> 地图切回小地图 / 切全屏（dm_ui_map_full）；前缀 s / c = 按着 Shift / Ctrl（VK 0x10 / 0x11）
// Test switch DM_UI_TAPS="frame:x,y[:action];..." (x / y = per-mille of the picture). Default action = one tap (via dm_ui_touch: press, lift 6 frames later); the others go through dm_ui_ptr (for probing / UI cases):
//   k tap a tab / dialog menu item (the host presses keys to switch to it); t<frames> finger held this many frames then lifted (via dm_ui_touch); h<frames> hover; p<frames> left button held this many frames then released; r right button; l left button (pointer path); d<x2>,<y2> hold and drag to another point (done in 20 frames); g<x2>,<y2> continuous gesture (dm_ui_drag_*: typed as scroll / slider by the start-to-end direction, done in 60 frames); W tap the game world (dm_world_tap); N<value> set the quantity box value (dm_num_set); A<number> buy/sell / stat point entries (dm_actions.c); w<amount> wheel (120 per notch, negative = down); m<0|1> map back to minimap / to full screen (dm_ui_map_full); prefix s / c = holding Shift / Ctrl (VK 0x10 / 0x11)
static void ui_taps_step(void) {
    static struct { uint64_t fr; float x, y, x2, y2; int st, kind, arg, shift; } tp[128]; static int n = -1;
    if (n < 0) { n = 0; const char *e = getenv("DM_UI_TAPS"); char buf[4096]; if (e) { strlcpy(buf, e, sizeof buf); for (char *t = strtok(buf, ";"); t && n < 128; t = strtok(NULL, ";")) {
        unsigned long long f; int x, y, c = 0; if (sscanf(t, "%llu:%d,%d%n", &f, &x, &y, &c) < 3) continue;
        tp[n].fr = f; tp[n].x = x / 1000.0f; tp[n].y = y / 1000.0f; tp[n].kind = 0; const char *a = t + c;
        if (*a == ':') { a++; if (*a == 's' || *a == 'c') { tp[n].shift = *a == 's' ? 0x10 : 0x11; a++; } tp[n].kind = *a; int p = 0, q = 0;
            if ((*a == 'd' || *a == 'g') && sscanf(a + 1, "%d,%d", &p, &q) == 2) { tp[n].x2 = p / 1000.0f; tp[n].y2 = q / 1000.0f; } else tp[n].arg = atoi(a + 1); }
        n++; } LOG("DM_UI_TAPS：%d 项", n); } }
    uint64_t fr = dm_guest_frame();
    static int rel = -1; if (rel < 0) rel = getenv("DM_UI_TAPS_AT") != NULL;
    if (rel) { uint64_t b = dm_ui_taps_base(); if (!b || fr < b) return; fr -= b; }
    for (int i = 0; i < n; i++) { if (fr < tp[i].fr || tp[i].st < 0) continue; int d = (int)(fr - tp[i].fr), k = tp[i].kind; float x = tp[i].x, y = k == 'W' ? tp[i].y : dm_ui_ny(tp[i].y);   // 脚本坐标是真实画面千分比；点世界照用，点界面换到界面矩形里 | script coordinates are per-mille of the real picture; world taps use them as is, UI taps are converted into the UI rectangle
        if (!k || k == 't') { int hold = k ? tp[i].arg : 6; if (tp[i].st == 0) { tp[i].st = 1; dm_ui_touch(0, x, y); } else if (tp[i].st == 1 && d >= hold) { tp[i].st = -1; dm_ui_touch(2, x, y); } continue; }
        if (k == 'N') { void dm_num_set(int); dm_num_set(tp[i].arg); tp[i].st = -1; continue; }   // 数量框设值（dm_textinput.c；-1 全部、-2 一半） | set the quantity box value (dm_textinput.c; -1 all, -2 half)
        if (k == 'A') { void dm_shop_buy(void), dm_shop_sell_cursor(void), dm_stat_add(int, int), dm_skill_add_selected(void); int v = tp[i].arg;   // 动作入口（dm_actions.c）：A1 买、A2 卖手上的、A3 技能 +1、A4 把背包里第一件能卖的拿到手上（测试用）、A5 把背包里第一瓶药水放进腰带（测试用）、A6 直接卖身上第一件能卖的（测试用）、A21–A24 立即使用背包里第一件药水 / 卷轴 / 书 / 方块（测试用，dm_item_use）、A1000+id 把该物品放进腰带、A1xy 属性 x（0–3）加 y（1 / 5 / 9 = 全部） | action entries (dm_actions.c): A1 buy, A2 sell what is on the cursor, A3 skill +1, A4 pick up the first sellable item in the inventory (for testing), A5 put the first potion in the inventory into the belt (for testing), A6 directly sell the first sellable item carried (for testing), A21-A24 immediately use the first potion / scroll / tome / cube in the inventory (for testing, dm_item_use), A1000+id put that item into the belt, A1xy add y (1 / 5 / 9 = all) to stat x (0–3)
            if (v == 1) dm_shop_buy(); else if (v == 2) dm_shop_sell_cursor(); else if (v == 3) dm_skill_add_selected(); else if (v == 4) { void dm_test_hold_first_sellable(void); dm_test_hold_first_sellable(); } else if (v == 6) { void dm_test_sell_first(void); dm_test_sell_first(); } else if (v == 5) { void dm_test_belt_first_potion(void); dm_test_belt_first_potion(); } else if (v == 35 || v == 38 || v == 39) { void dm_inv_sort(int); dm_inv_sort(v == 35 ? 0 : v == 38 ? 3 : 4); } else if (v == 36) { void dm_name_random(void); dm_name_random(); } else if (v >= 44 && v <= 46) { void dm_test_identify(int); dm_test_identify(v - 43); } else if (v == 74) { void dm_held_stow(void); dm_held_stow(); } else if (v == 75) { void dm_test_swap_held(void); dm_test_swap_held(); } else if (v >= 70 && v <= 73) { dm_uiscreen_enable(v - 70); }   /* A70–A73：界面画面 关 / 贴顶 / 居中 / 贴底（走前端用的同一个入口） | A70-A73: UI picture off / top-aligned / centered / bottom-aligned (through the same entry the front end uses) */ else if (v == 47 || v == 48) { dm_stash_take(v == 47 ? 1 : 3); } else if (v == 43) { dm_cursor_item_info ci; int hw = 0, hh = 0, c = dm_cursor_item(&ci), hd = dm_held_size(&hw, &hh); LOG("测试：光标物品 %s 容器 %d 格 %d,%d 占 %dx%d 叠数 %d；手上 %d（%dx%d）；等目标 %d", c ? ci.code : "无", c ? ci.container : 0, c ? ci.gx : 0, c ? ci.gy : 0, c ? ci.gw : 0, c ? ci.gh : 0, c ? ci.stack : 0, hd, hw, hh, dm_item_targeting()); } else if (v >= 50 && v <= 69) { void dm_test_stash_take(int); dm_test_stash_take(v - 50 ? v - 50 : 1); } else if (v >= 40 && v <= 42) { void dm_test_equip(int); dm_test_equip(v - 39); } else if (v == 33 || v == 34) { void dm_belt_expand(int); dm_belt_expand(v == 33); } else if (v == 31 || v == 32) { void dm_item_compare(int); dm_item_compare(v == 31); } else if (v == 30) { void dm_test_hover_at(float, float); dm_test_hover_at(x, y); } else if (v >= 21 && v <= 26) { void dm_test_use_first(int); dm_test_use_first(v - 20); } else if (v >= 1000) { void dm_belt_put(uint32_t); dm_belt_put((uint32_t)(v - 1000)); } else if (v >= 100) dm_stat_add(v / 10 % 10, v % 10 == 9 ? -1 : v % 10); tp[i].st = -1; continue; }
        if (k == 'W') { unsigned dm_world_tap(float, float); dm_world_tap(x, y); tp[i].st = -1; continue; }   // 点游戏世界（dm_world.c：点目标 = 交互） | tap the game world (dm_world.c: tapping a target = interact)
        if (k == 'g') { float t = d / 60.0f; if (t > 1) t = 1;
            if (tp[i].st == 0) { tp[i].st = 1; int hz = fabsf(tp[i].x2 - x) * 16 > fabsf(tp[i].y2 - y) * 9; dm_ui_drag_begin(hz ? 2 : 1, x, y, tp[i].arg ? tp[i].arg / 1000.0f : 0.05f); }
            dm_ui_drag_move(x + (tp[i].x2 - x) * t, y + (tp[i].y2 - y) * t); if (d >= 62) { dm_ui_drag_end(); tp[i].st = -1; } continue; }
        if (k == 'k') { static unsigned ks[128]; if (tp[i].st == 0) { tp[i].st = 1; ks[i] = dm_ui_item_query(x, y); } else if (d >= 3) { tp[i].st = -1; if (dm_ui_item_result(ks[i]) == 2) dm_ui_tab_go(); } continue; }   // k = 按「页签 / 对话菜单」的走法点一下（问命中 → 是就让宿主按键） | k = tap once the "tab / dialog menu" way (query the hit → if yes, let the host press the key)
        if (tp[i].st == 0) { tp[i].st = 1; dm_ui_item_query(x, y); dm_ui_ptr(DM_UP_HOVER, x, y, 0); if (tp[i].shift) dm_ui_ptr(DM_UP_KEYDOWN, x, y, tp[i].shift); continue; }
        int done = 0;
        if (k == 'h') done = d >= tp[i].arg;
        else if (k == 'r' || k == 'l') { if (tp[i].st == 1 && d >= 4) { tp[i].st = 2; dm_ui_ptr(k == 'r' ? DM_UP_RDOWN : DM_UP_LDOWN, x, y, 0); dm_ui_ptr(k == 'r' ? DM_UP_RUP : DM_UP_LUP, x, y, 0); } done = d >= 10; }
        else if (k == 'p') { if (tp[i].st == 1 && d >= 4) { tp[i].st = 2; dm_ui_ptr(DM_UP_LDOWN, x, y, 0); } else if (tp[i].st == 2 && d >= 4 + tp[i].arg) { tp[i].st = 3; dm_ui_ptr(DM_UP_LUP, x, y, 0); } done = d >= 10 + tp[i].arg; }
        else if (k == 'm') { if (tp[i].st == 1) { tp[i].st = 2; dm_ui_map_full(tp[i].arg); } done = d >= 4; }
        else if (k == 'w') { if (tp[i].st == 1 && d >= 4) { tp[i].st = 2; dm_ui_ptr(DM_UP_SCROLL, x, y, tp[i].arg); } done = d >= 8; }
        else if (k == 'd') { if (tp[i].st == 1 && d >= 4) { tp[i].st = 2; dm_ui_ptr(DM_UP_LDOWN, x, y, 0); }
            else if (tp[i].st == 2 && d >= 6) { float t = (d - 6) / 20.0f; if (t > 1) t = 1; dm_ui_ptr(DM_UP_MOVE, x + (tp[i].x2 - x) * t, y + (tp[i].y2 - y) * t, 0); if (d >= 30) { tp[i].st = 3; dm_ui_ptr(DM_UP_LUP, 0, 0, 0); } }
            done = d >= 36; }
        else done = 1;
        if (done) { tp[i].st = -1; if (tp[i].shift) dm_ui_ptr(DM_UP_KEYUP, x, y, tp[i].shift); dm_ui_ptr(DM_UP_END, x, y, 0); } }
}
static _Atomic uint64_t g_stick_ns;
uint64_t dm_stick_last_ns(void) { return atomic_load(&g_stick_ns); }   // 「点地移动包只许来自摇杆」的判据（diag.c c2s_deliver） | criterion for "click-to-move packets may only come from the stick" (diag.c c2s_deliver)
uint64_t dm_input_samples(void) { return (uint64_t)atomic_load(&g_sampling); }   // 游戏已取手柄的累计次数（前端据此判断一下按键是否已被游戏看到） | cumulative number of times the game has read the controller (the front end uses it to judge whether a key press has been seen by the game)

static uint32_t connected_id(void) { return dm_opmode() == 0 ? NPAD_HANDHELD : 0; }   // 跟运行模式走（hle_oe.c 唯一来源） | follows the operation mode (hle_oe.c is the single source)
static uint32_t style_of(uint32_t id) {
    if (id != connected_id()) return 0;
    return id == NPAD_HANDHELD ? STYLE_HANDHELD : STYLE_FULLKEY;
}
// 脚本化输入（不开窗也能注入），逗号分隔两种项：
//   按时刻：键@秒[+按住秒]        DM_KEYS="A@8,A@12+0.5"＝第 8 秒按 A（默认按住 0.2 秒）、第 12 秒按 A 0.5 秒；时刻从宿主初始化算起
//   按 UI 消息：键@前置[%延时秒][+按住秒][>确认]    DM_KEYS="A@OpenPanel:MainMenuButtonRibbon>OpenPanel:CharacterDifficultyModal,A@OpenPanel:CharacterDifficultyModal>FrontEndNavigation:ToGame"
//     规则同 dm/verify/ryu/drive.py：消息串 = 「类别:动作:面板名」，前置 / 确认按子串匹配；各步依次执行。
//     前置在上一步按下之后（第一步：开局起）出现过、且上一次按键已空闲（见下）→ 按下 3 帧（50 ms；
//     「+按住秒」改按住时长，如 LSL@UnloadPanel:LoadScreenPanel+0.9 = 装载画面结束即推左摇杆 0.9 秒，免得按宿主时刻按键时还没进局）；
//     150 个服务端拍内没等到确认才重按，3 次都没等到报红中止；墙钟 60 秒只作兜底并报红；无确认项的步按下即算完成。
//     前置留空（如 LSU@%0.2+0.35）= 紧接上一步完成（确认到 / 无确认项则松开）后延时再按。
//     空闲 = 上一次按键已松开，且按住期间最后的 InputMessage:ControllerActionBegin 已等到 ControllerActionEnd（Begin 只在面板消费
//     输入时才发：wild1 真值局内按 A 开小站前后无 Begin / End，面板接管后才成对出现；按住期间没出现 Begin ⇒ 松开即空闲。
//     摇杆伪键同此规则）。原先「必须见 End」在局内按键后永远等不到。
// 键名 = dm_input.h 的 DM_BTN_* 去前缀，另有左摇杆伪键 LSL / LSR / LSU / LSD（按住期间推满）。与前端写入的按键按位或。
// 触点项 T<x>:<y>（只用于按 UI 消息的步）：按住期间在出图画面归一化坐标 (x, y) 落一根手指（id 0，经 dm_touch_set），松开 / 确认即抬起。
// Scripted input (can inject without opening a window), two kinds of comma-separated items:
//   By time: key@seconds[+hold seconds]        DM_KEYS="A@8,A@12+0.5" = press A at second 8 (held 0.2 s by default), press A for 0.5 s at second 12; time counts from host initialization
//   By UI message: key@pre[%delay seconds][+hold seconds][>confirm]    DM_KEYS="A@OpenPanel:MainMenuButtonRibbon>OpenPanel:CharacterDifficultyModal,A@OpenPanel:CharacterDifficultyModal>FrontEndNavigation:ToGame"
//     Same rules as dm/verify/ryu/drive.py: message string = "category:action:panel name", pre / confirm match by substring; steps run in order.
//     The pre has appeared after the previous step was pressed (first step: since start) and the previous key press is idle (see below) → press for 3 frames (50 ms;
//     "+hold seconds" changes the hold duration, e.g. LSL@UnloadPanel:LoadScreenPanel+0.9 = push the left stick for 0.9 s as soon as the loading screen ends, so a key pressed by host time does not land before the run is entered);
//     only re-press if no confirm arrives within 150 server ticks, after 3 times without one report a failure and abort; the 60 s wall clock is only a fallback and reports a failure; a step with no confirm item is done once pressed.
//     An empty pre (e.g. LSU@%0.2+0.35) = press after a delay right after the previous step completes (confirm arrived / released if no confirm item).
//     Idle = the previous key press has been released, and the last InputMessage:ControllerActionBegin during the hold has got its ControllerActionEnd (Begin is only sent when a panel consumes
//     input: in the wild1 ground truth, pressing A in a run to open the waypoint has no Begin / End before or after, they only appear in pairs once a panel takes over; no Begin during the hold ⇒ idle on release.
//     Stick pseudo-keys follow the same rule). The former "must see End" never arrived after in-run key presses.
// Key name = DM_BTN_* from dm_input.h without the prefix, plus the left stick pseudo-keys LSL / LSR / LSU / LSD (pushed to full while held). ORed bitwise with the keys written by the front end.
// Touch item T<x>:<y> (only for steps driven by UI messages): while held, one finger (id 0, via dm_touch_set) is put down at normalized rendered-picture coordinates (x, y), lifted on release / confirm.
#define K(n) {DM_BTN_##n, #n}                     // 键名直接由 dm_input.h 的 DM_BTN_* 生成，位序只有一处定义 | key names are generated directly from DM_BTN_* in dm_input.h, the bit order is defined in one place only
static const struct { uint32_t mask; const char *name; } k_names[] = {
    K(A), K(B), K(X), K(Y), K(LS), K(RS), K(L), K(R), K(ZL), K(ZR), K(PLUS), K(MINUS), K(LEFT), K(UP), K(RIGHT), K(DOWN)};
#undef K
// 左摇杆伪键（只在脚本里用）：按住期间摇杆推满该方向；位在 DM_BTN_* 之外，不进按钮位
// Left stick pseudo-keys (used only in scripts): while held the stick is pushed fully in that direction; the bits lie outside DM_BTN_* and do not enter the button bits
enum { PK_LSL = 1u << 24, PK_LSR = 1u << 25, PK_LSU = 1u << 26, PK_LSD = 1u << 27, PK_MASK = 0xFu << 24, PK_TOUCH = 1u << 28, PK_MSG = 1u << 29, PK_CLICK = 1u << 30 };   // PK_CLICK：界面点击步（C<x>:<y>，经 dm_ui_touch = 与 iOS 前端真手指同一条注入鼠标消息的路）   // PK_TOUCH：触点步（T<x>:<y>，经 dm_touch_set） | PK_CLICK: UI click step (C<x>:<y>, via dm_ui_touch = the same injected mouse message path as a real finger on the iOS front end)   // PK_TOUCH: touch point step (T<x>:<y>, via dm_touch_set)
static const struct { uint32_t mask; const char *name; } k_stick[] = {{PK_LSL, "LSL"}, {PK_LSR, "LSR"}, {PK_LSU, "LSU"}, {PK_LSD, "LSD"}};
#define KMAX 256
static struct { uint32_t mask; uint64_t t0, t1; } g_keys[KMAX];
static int g_nkeys;
static uint64_t g_kbase;
// 按 UI 消息的步骤（状态由 UI 消息回调与手柄读取两路驱动，g_klock 保护）
// Steps driven by UI messages (state is driven from two sides, the UI message callback and the controller read, protected by g_klock)
enum { KS_PRE, KS_HOLD, KS_OK, KS_DONE };
static struct { uint32_t mask; char pre[96], ok[96]; int seen_pre, seen_server, hold_override; double delay_seconds, hold_seconds; uint64_t seen_clock, seen_at; float tx, ty; char msg[96]; } g_steps[KMAX];   // 局内按服务端拍；进游戏前按出图帧 | in a run, by server ticks; before entering the game, by presented frames
static int g_nsteps, g_kcur, g_kstate, g_ktries, g_krel = 1, g_kbeg, g_kend;   // beg/end 是动作生命周期的0/1标记，不是消息次数；空闲 = 已松手且最后Begin已End | beg/end are 0/1 markers of the action lifecycle, not message counts; idle = released and the last Begin has got its End
static uint64_t g_kpress, g_kpress_clock; static int g_kpress_server;
static pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static struct { char msg[160]; int32_t x, y; } g_cursor_tests[KMAX];
static int g_ncursor_tests, g_cursor_cur;
#define SERVER_TICKS_PER_SECOND 25ull
#define PRODUCT_FRAMES_PER_SECOND 60ull
#define ACTION_HOLD_TICKS 2ull   // 动作键 2 拍（约 80 ms，至少跨一整拍）：按太久减号会被认成连按，自动地图切到满屏档（实测 15 拍时 automap 红） | action keys 2 ticks (about 80 ms, spanning at least one full tick): held too long, minus is taken as a repeated press and the automap switches to the full-screen mode (measured: automap red at 15 ticks)
#define DIRECTION_HOLD_TICKS 3ull // 方向键至少 3 拍：连续采样三次即可可靠采到 | direction keys at least 3 ticks: three consecutive samples are enough to catch it reliably
#define OK_WAIT_TICKS 150ull
#define WALL_FALLBACK_NS 60000000000ull

typedef struct { uint64_t value; int server; } script_clock;
static script_clock script_clock_now(void) {
    uint64_t tick = dm_server_tick(); { extern _Atomic int g_dm_frozen; if (atomic_load(&g_dm_frozen)) tick = UINT64_MAX; }   // 世界冻结时（面板冻结，diag.c）拍号不走，脚本改按出图帧计时，否则延时和按住时长永远到不了 | while the world is frozen (panel freeze, diag.c) the tick number does not advance, the script times by presented frames instead, otherwise delays and hold durations would never be reached
    return tick == UINT64_MAX ? (script_clock){dm_guest_frame(), 0} : (script_clock){tick, 1};
}
static uint64_t script_clock_elapsed(script_clock now, uint64_t start, int start_server) {
    return now.server == start_server && now.value >= start ? now.value - start : 0;
}
static uint64_t script_seconds_to_units(double seconds, int server) {
    return (uint64_t)(seconds * (server ? SERVER_TICKS_PER_SECOND : PRODUCT_FRAMES_PER_SECOND));
}
static uint64_t step_hold_units(const typeof(g_steps[0]) *st, int server) {
    if (st->hold_override) return script_seconds_to_units(st->hold_seconds, server);
    return (st->mask & (DM_BTN_UP | DM_BTN_DOWN | DM_BTN_LEFT | DM_BTN_RIGHT | PK_MASK)) ? DIRECTION_HOLD_TICKS : ACTION_HOLD_TICKS;
}

static void cursor_tests_parse(void) {
    g_ncursor_tests = g_cursor_cur = 0;
    const char *e = getenv("DM_TEST_CURSOR"); if (!e || !*e) return;
    char buf[4096]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *save, *tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        char *colon = strrchr(tok, ':'); int x, y; char extra;
        if (!colon || sscanf(colon + 1, "%d,%d%c", &x, &y, &extra) != 2 || g_ncursor_tests == KMAX)
            die("DM_TEST_CURSOR 项「%s」格式：UI消息触发:x,y", tok);
        *colon = 0;
        if (!*tok) die("DM_TEST_CURSOR 项触发串为空");
        strncpy(g_cursor_tests[g_ncursor_tests].msg, tok, sizeof g_cursor_tests[0].msg - 1);
        g_cursor_tests[g_ncursor_tests].msg[sizeof g_cursor_tests[0].msg - 1] = 0;
        g_cursor_tests[g_ncursor_tests].x = (int32_t)x;
        g_cursor_tests[g_ncursor_tests].y = (int32_t)y;
        g_ncursor_tests++;
    }
}

static void step_chain(void) {             // 持 g_klock：当前步前置留空 ⇒ 上一步刚完成即算见到 | holds g_klock: the current step's pre is left empty ⇒ counts as seen as soon as the previous step completes
    if (g_kcur < g_nsteps && !g_steps[g_kcur].pre[0] && !g_steps[g_kcur].seen_pre) { script_clock c = script_clock_now(); g_steps[g_kcur].seen_pre = 1; g_steps[g_kcur].seen_at = now_ns(); g_steps[g_kcur].seen_clock = c.value; g_steps[g_kcur].seen_server = c.server; }
}
static void keys_on_uimsg(const char *cat, const char *act, const char *arg) {
    char m[256]; snprintf(m, sizeof m, "%s:%s:%s", cat, act, arg);
    pthread_mutex_lock(&g_klock);
    if (g_cursor_cur < g_ncursor_tests && strstr(m, g_cursor_tests[g_cursor_cur].msg)) {
        int i = g_cursor_cur++;
        dm_cursor_set(g_cursor_tests[i].x, g_cursor_tests[i].y);
    }
    // 长按/摇杆的重复触发每次发 Begin，松手只发一次 End（iOS宿主现场：73 Begin/1 End）。
    // 新 Begin 标记动作仍忙并撤销旧 End；重复 Begin 不增加等待数，End 才标记结束。
    // Repeat triggers of a long press/stick send a Begin each time, release sends only one End (observed on the iOS host: 73 Begin/1 End).
    // A new Begin marks the action as still busy and cancels the old End; repeated Begins do not increase the wait count, only End marks it finished.
    if (strstr(m, "InputMessage:ControllerActionBegin") && !g_krel) { g_kbeg = 1; g_kend = 0; }
    if (strstr(m, "InputMessage:ControllerActionEnd")) g_kend = 1;
    if (g_kcur < g_nsteps) {
        if (g_kstate != KS_PRE && g_steps[g_kcur].ok[0] && strstr(m, g_steps[g_kcur].ok)) {
            LOG("hid 脚本步 %d 确认「%s」（第 %d 次按键，等待时钟单位 %llu，墙钟 %llu ms）", g_kcur + 1, m, g_ktries,
                (unsigned long long)(script_clock_now().value - g_kpress_clock), (unsigned long long)((now_ns() - g_kpress) / 1000000ull));
            if (g_kstate == KS_HOLD && (g_steps[g_kcur].mask & PK_TOUCH)) dm_touch_set(NULL, 0);
            if (g_kstate == KS_HOLD && (g_steps[g_kcur].mask & PK_CLICK)) dm_ui_touch(2, g_steps[g_kcur].tx, g_steps[g_kcur].ty);   // 触点步按住期间确认：抬起手指 | touch step confirmed while held: lift the finger
            g_krel = 1;                // 按住期间就确认（如开面板）：状态离开 KS_HOLD、不再输出按键 ⇒ 此刻即松开（wild1 步 3 原卡在这里） | confirmed while still held (e.g. opening a panel): the state leaves KS_HOLD and no more key is output ⇒ released at this moment (wild1 step 3 used to get stuck here)
            g_kcur++; g_kstate = KS_PRE; g_ktries = 0; step_chain();
        }
        // 前置：当前步在等前置时看它；当前步已按下时看下一步（上一步按下之后出现的消息才算）
        // Pre: checked while the current step is waiting for its pre; when the current step is already pressed, the next step's is checked (only messages appearing after the previous step was pressed count)
        int j = g_kstate == KS_PRE ? g_kcur : g_kcur + 1;
        if (j < g_nsteps && !g_steps[j].seen_pre && g_steps[j].pre[0] && strstr(m, g_steps[j].pre)) { script_clock c = script_clock_now(); g_steps[j].seen_pre = 1; g_steps[j].seen_at = now_ns(); g_steps[j].seen_clock = c.value; g_steps[j].seen_server = c.server; }
    }
    pthread_mutex_unlock(&g_klock);
}
static uint32_t steps_now(void) {
    if (!g_nsteps) return 0;
    uint64_t t = now_ns(); uint32_t m = 0;
    pthread_mutex_lock(&g_klock);
    if (g_kcur < g_nsteps) {
        typeof(g_steps[0]) *st = &g_steps[g_kcur];
        script_clock c = script_clock_now();
        // 时钟来源切换（主菜单帧 ↔ 局内拍 ↔ 冻结期帧）：起点按新来源重计，否则跨来源的已过时长恒为 0、永远等不到
        // Clock source switch (main menu frames ↔ in-run ticks ↔ frozen-period frames): the start point is recounted from the new source, otherwise the elapsed time across sources is always 0 and the wait never ends
        if (st->seen_pre && c.server != st->seen_server) { st->seen_clock = c.value; st->seen_server = c.server; }
        if (g_kstate != KS_PRE && c.server != g_kpress_server) { g_kpress_clock = c.value; g_kpress_server = c.server; }
        uint64_t elapsed = script_clock_elapsed(c, st->seen_clock, st->seen_server);
        int dm_loadscreen_up(void); void dm_hold_for_loadscreen(void);
        if (g_kstate == KS_PRE && st->seen_pre && elapsed >= script_seconds_to_units(st->delay_seconds, c.server) && dm_loadscreen_up()) dm_hold_for_loadscreen();   // 到点（前置见过且延时已满）但读取画面还开着：冻结拍、先不按（diag.c）。延时没满不冻：小站换幕后排着一个 62 秒后才按的步，读取画面一开就把时钟冻在服务端处理传送之前，装载等服务端、服务端等时钟，卡死 | due (pre seen and delay elapsed) but the loading screen is still up: freeze the tick, do not press yet (diag.c). Not frozen while the delay has not elapsed: after a waypoint act change a step due to press 62 seconds later was queued, and as soon as the loading screen opened the clock was frozen before the server handled the teleport, loading waited for the server, the server waited for the clock, deadlock
        else if (g_kstate == KS_PRE && st->seen_pre && g_krel && (g_kend >= g_kbeg || (st->mask & (PK_MSG | PK_CLICK))) && elapsed >= script_seconds_to_units(st->delay_seconds, c.server)) {   // 消息步不按键，不必等上一个键的手柄动作收尾（上一步按住期间重复触发的 Begin 有时等不到 End，Mac iOS 包 3 局里 1 局卡在这） | a message step presses no key, so it need not wait for the previous key's controller action to wrap up (a Begin re-triggered while the previous step is held sometimes never gets its End, 1 of 3 runs of the Mac iOS app got stuck here)
            g_kstate = KS_HOLD; g_kpress = t; g_kpress_clock = c.value; g_kpress_server = c.server; g_krel = 0; g_kbeg = g_kend = 0; g_ktries = 1;
            if (st->mask & PK_TOUCH) { dm_touch p = {0, st->tx, st->ty}; dm_touch_set(&p, 1); }
            if (st->mask & PK_MSG) dm_ui_send_queue(st->msg);
            if (st->mask & PK_CLICK) dm_ui_touch(0, st->tx, st->ty);
            LOG("hid 脚本步 %d 按下 mask=%#x（前置「%s」）", g_kcur + 1, st->mask, st->pre);
        } else if (g_kstate == KS_OK && t - g_kpress >= WALL_FALLBACK_NS) {
            die("【红】hid 脚本步 %d：墙钟兜底超时（等待服务端拍 %llu，墙钟 %llu ms，确认「%s」）", g_kcur + 1,
                (unsigned long long)script_clock_elapsed(c, g_kpress_clock, g_kpress_server), (unsigned long long)((t - g_kpress) / 1000000ull), st->ok);
        } else if (g_kstate == KS_OK && script_clock_elapsed(c, g_kpress_clock, g_kpress_server) >= OK_WAIT_TICKS) {
            if (g_ktries >= 3) die("【红】hid 脚本步 %d：按 3 次都没等到确认「%s」（等待服务端拍 %llu，墙钟 %llu ms）", g_kcur + 1, st->ok,
                (unsigned long long)script_clock_elapsed(c, g_kpress_clock, g_kpress_server), (unsigned long long)((t - g_kpress) / 1000000ull));
            LOG("hid 脚本步 %d 等待确认：服务端拍 %llu，墙钟 %llu ms；重按（第 %d 次）", g_kcur + 1,
                (unsigned long long)script_clock_elapsed(c, g_kpress_clock, g_kpress_server), (unsigned long long)((t - g_kpress) / 1000000ull), g_ktries + 1);
            g_kstate = KS_HOLD; g_kpress = t; g_kpress_clock = c.value; g_kpress_server = c.server; g_krel = 0; g_kbeg = g_kend = 0; g_ktries++;
            if (st->mask & PK_TOUCH) { dm_touch p = {0, st->tx, st->ty}; dm_touch_set(&p, 1); }
            if (st->mask & PK_MSG) dm_ui_send_queue(st->msg);
            if (st->mask & PK_CLICK) dm_ui_touch(0, st->tx, st->ty);
            LOG("hid 脚本步 %d 重按（第 %d 次）", g_kcur + 1, g_ktries);
        }
        if (g_kstate == KS_HOLD) {
            uint64_t held = script_clock_elapsed(c, g_kpress_clock, g_kpress_server);
            if (held < step_hold_units(st, c.server)) m = st->mask & ~(PK_TOUCH | PK_MSG | PK_CLICK);
            else if (st->ok[0]) { LOG("hid 脚本步 %d 松开（服务端拍 %llu，按住 %llu 拍）", g_kcur + 1, (unsigned long long)c.value, (unsigned long long)held); g_krel = 1; g_kstate = KS_OK; if (st->mask & PK_TOUCH) dm_touch_set(NULL, 0); if (st->mask & PK_CLICK) dm_ui_touch(2, st->tx, st->ty); }
            else {                                                // 无确认项：松开即完成 | no confirm item: done on release
                LOG("hid 脚本步 %d 松开（时钟 %llu，按住 %llu 拍）", g_kcur + 1, (unsigned long long)c.value, (unsigned long long)held); g_krel = 1; if (st->mask & PK_TOUCH) dm_touch_set(NULL, 0); if (st->mask & PK_CLICK) dm_ui_touch(2, st->tx, st->ty);
                g_kcur++; g_kstate = KS_PRE; g_ktries = 0; step_chain();
            }
        }
    }
    pthread_mutex_unlock(&g_klock);
    return m;
}
static void keys_parse(void) {
    g_nkeys = 0; g_kbase = now_ns();
    cursor_tests_parse();
    const char *e = getenv("DM_KEYS"); if (!e) e = "";
    char buf[4096]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *at = strchr(tok, '@'); if (!at) die("DM_KEYS 项「%s」缺 @时刻 / @消息", tok);
        *at = 0;
        uint32_t mask = 0; float tx = 0, ty = 0;
        if (tok[0] == 'T' && ((tok[1] >= '0' && tok[1] <= '9') || tok[1] == '.')) {   // 触点：T<x>:<y>（出图画面归一化坐标 [0,1)，左上为原点） | touch point: T<x>:<y> (normalized coordinates of the rendered picture [0,1), origin at top left)
            char *c = strchr(tok, ':'); if (!c) die("DM_KEYS 触点项「%s」格式：T<x>:<y>@前置…", tok);
            tx = (float)atof(tok + 1); ty = (float)atof(c + 1); mask = PK_TOUCH;
        }
        if (tok[0] == 'C' && ((tok[1] >= '0' && tok[1] <= '9') || tok[1] == '.')) {   // 界面点击：C<x>:<y>（出图画面归一化坐标），按下 / 抬起各发一次 dm_ui_touch | UI click: C<x>:<y> (normalized coordinates of the rendered picture), dm_ui_touch is sent once each for press / release
            char *c = strchr(tok, ':'); if (!c) die("DM_KEYS 点击项「%s」格式：C<x>:<y>@前置…", tok);
            tx = (float)atof(tok + 1); ty = (float)atof(c + 1); mask = PK_CLICK;
        }
        const char *msg = NULL;   // 发界面消息的步：MSG:<类别>:<动作>[:<参数>]@前置[>确认]——不按键，到点往界面根控件发这条消息（选角页换成键鼠版界面后手柄确认键进不了局，测试进局的最后一步改发 MainMenuHD:Play） | step that sends a UI message: MSG:<category>:<action>[:<argument>]@pre[>confirm] -- presses no key, at the due time sends this message to the UI root widget (after the character select page was switched to the keyboard-and-mouse UI the controller confirm key can no longer enter a run, so the last step of test run entry sends MainMenuHD:Play instead)
        if (!strncmp(tok, "MSG:", 4)) { if (!strchr(tok + 4, ':') || strlen(tok + 4) >= sizeof g_steps[0].msg) die("DM_KEYS 消息项「%s」格式：MSG:<类别>:<动作>[:<参数>]@前置[>确认]", tok); msg = tok + 4; mask = PK_MSG; }
        for (int i = 0; i < (int)(sizeof k_names / sizeof *k_names); i++) if (!strcmp(tok, k_names[i].name)) mask = k_names[i].mask;
        for (int i = 0; i < 4; i++) if (!strcmp(tok, k_stick[i].name)) mask = k_stick[i].mask;
        if (!mask) die("DM_KEYS 未知键名「%s」", tok);
        const char *v = at + 1;
        if ((*v >= '0' && *v <= '9') || *v == '.') {
            if (mask & (PK_TOUCH | PK_MSG | PK_CLICK)) die("DM_KEYS 触点项 / 消息项只支持按 UI 消息的步（…@前置…），不支持按时刻");
            char *plus = strchr(v, '+');
            double t = atof(v), d = plus ? atof(plus + 1) : 0.2;
            if (g_nkeys == KMAX) die("DM_KEYS 超过 %d 项", KMAX);
            g_keys[g_nkeys++] = (typeof(g_keys[0])){mask, (uint64_t)(t * 1e9), (uint64_t)((t + d) * 1e9)};
        } else {
            if (g_nsteps == KMAX) die("DM_KEYS 超过 %d 项", KMAX);
            typeof(g_steps[0]) *st = &g_steps[g_nsteps++]; st->mask = mask; st->tx = tx; st->ty = ty; st->msg[0] = 0; if (msg) strcpy(st->msg, msg);
            const char *gt = strchr(v, '>');
            size_t np = gt ? (size_t)(gt - v) : strlen(v);
            if (np >= sizeof st->pre || (gt && (!gt[1] || strlen(gt + 1) >= sizeof st->ok))) die("DM_KEYS 消息步「%s」格式：键@前置[%%延时秒][+按住秒][>确认]", v);
            memcpy(st->pre, v, np); st->pre[np] = 0;
            char *pp = strchr(st->pre, '+'); if (pp) { st->hold_override = 1; st->hold_seconds = atof(pp + 1); *pp = 0; }   // 前置+按住秒 → 局内 25 拍/秒，主菜单 60 出图帧/秒；不足 1 单位即不可采样 | pre+hold seconds → 25 ticks/s in a run, 60 presented frames/s in the main menu; less than 1 unit cannot be sampled
            char *pc = strchr(st->pre, '%'); if (pc) { st->delay_seconds = atof(pc + 1); *pc = 0; }   // 前置%秒 → 局内 25 拍/秒，主菜单 60 出图帧/秒 | pre%seconds → 25 ticks/s in a run, 60 presented frames/s in the main menu
            if (gt) strcpy(st->ok, gt + 1);
        }
    }
    if (g_nsteps || g_ncursor_tests) { g_steps[0].seen_pre = 0; if (g_nsteps && !g_steps[0].pre[0]) { script_clock c = script_clock_now(); g_steps[0].seen_pre = 1; g_steps[0].seen_at = now_ns(); g_steps[0].seen_clock = c.value; g_steps[0].seen_server = c.server; } void uimsg_subscribe(void (*)(const char *, const char *, const char *)); uimsg_subscribe(keys_on_uimsg); }
    LOG("hid 脚本输入：按时刻 %d 项，按 UI 消息 %d 步", g_nkeys, g_nsteps);
}
static pthread_once_t g_kone = PTHREAD_ONCE_INIT;
void hid_keys_init(void) { pthread_once(&g_kone, keys_parse); }
static uint64_t keys_now(void) {
    pthread_once(&g_kone, keys_parse);
    uint64_t t = now_ns() - g_kbase, m = steps_now();
    for (int i = 0; i < g_nkeys; i++) if (t >= g_keys[i].t0 && t < g_keys[i].t1) m |= g_keys[i].mask;
    static _Atomic uint64_t seen;                 // 每个键第一次真被游戏读到时打一行（证明注入到达） | print one line the first time each key is really read by the game (proves the injection arrived)
    uint64_t newbits = m & ~atomic_fetch_or(&seen, m);
    if (newbits) LOG("hid 脚本键首次被读到：mask=%#llx @%.2fs", newbits, t / 1e9);
    return m;
}

// 可选实际GetNpadState返回收样；状态变化+最多每秒一次活动见证，sampling单独变化不刷屏。
// 1024条后明确limit_reached，截断日志不作完整手势证据；不开环境变量不写文件。
// Optional sampling of actual GetNpadState returns; state changes + an activity witness at most once per second, a change in sampling alone does not flood the log.
// After 1024 entries limit_reached is stated explicitly, a truncated log does not count as complete gesture evidence; without the environment variable no file is written.
static FILE *g_receipt_file;
static pthread_once_t g_receipt_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_receipt_lock = PTHREAD_MUTEX_INITIALIZER;
static void receipt_init(void) {
    const char *path = getenv("DM_HID_RECEIPT_LOG");
    if (path && *path) {
        g_receipt_file = fopen(path, "w");
        if (!g_receipt_file) LOG("【红】HID返回收样文件打开失败：%s", path);
    }
}
static void receipt(const NpadState *s, uint32_t id, uint32_t style, uint64_t km, uint64_t read_start_ns) {
    if (!g_receipt_file) return;
    static NpadState last; static uint32_t last_id, last_style;
    static uint64_t last_km, last_ns; static unsigned count; static int have, truncated;
    NpadState state = *s; state.sampling = 0; uint64_t t = now_ns();
    pthread_mutex_lock(&g_receipt_lock);
    if (truncated || (have && id == last_id && style == last_style && km == last_km && !memcmp(&state, &last, sizeof state) && t - last_ns < 1000000000ull)) {
        pthread_mutex_unlock(&g_receipt_lock); return;
    }
    if (count == 1024) {
        fprintf(g_receipt_file, "{\"event\":\"limit_reached\",\"limit\":1024}\n"); truncated = 1;
    } else {
        fprintf(g_receipt_file, "{\"event\":\"npad_return\",\"read_start_ns\":%llu,\"time_ns\":%llu,\"sampling\":%lld,\"id\":%u,\"style\":%u,\"buttons\":%llu,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"attributes\":%u,\"script_mask\":%llu}\n",
                (unsigned long long)read_start_ns, (unsigned long long)t, (long long)s->sampling, id, style, (unsigned long long)s->buttons,
                s->lx, s->ly, s->rx, s->ry, s->attributes, (unsigned long long)km);
        last = state; last_id = id; last_style = style; last_km = km; last_ns = t; have = 1; count++;
    }
    if (fflush(g_receipt_file)) { LOG("【红】HID返回收样写入失败"); truncated = 1; }
    pthread_mutex_unlock(&g_receipt_lock);
}

void (*dm_pad_hook)(uint64_t, uint64_t *, int32_t *, int32_t *, int32_t *, int32_t *);
static void fill(NpadState *s, uint32_t id, uint32_t style) {
    pthread_once(&g_receipt_once, receipt_init);
    DM_PROF(0, "taps", ui_taps_step());
    DM_PROF(0, "ui_frontend", dm_ui_frontend_pump());
    { void dm_textinput_pump(void); DM_PROF(0, "textinput", dm_textinput_pump()); }   // 文本框补「弹键盘」那一下、数量框设值（dm_textinput.c） | the extra "bring up keyboard" press for text boxes, setting the value of quantity boxes (dm_textinput.c)
    { void dm_update_pump(void); DM_PROF(0, "update", dm_update_pump()); }       // 选角页的构建号与新版本提示（dm_update.c） | build number and new-version notice on the character select page (dm_update.c)
    { void dm_actions_pump(void); DM_PROF(0, "actions", dm_actions_pump()); }     // 商人买卖 / 加点入口与读数（dm_actions.c） | vendor buy/sell / stat point entry and readout (dm_actions.c)
    { void dm_world_pump(void); DM_PROF(0, "world", dm_world_pump()); }   // 点世界目标 = 交互（dm_world.c） | tapping a world target = interact (dm_world.c)
    DM_PROF(0, "ui_mouse", dm_ui_mouse_pump());   // 界面鼠标消息（原生点击）在取手柄处发：主线程、每帧一条 | UI mouse messages (native clicks) are sent at the controller read: main thread, one per frame
    { void dm_aim_cone_pump(void); DM_PROF(0, "aim_cone", dm_aim_cone_pump()); dm_ipf_frame(0); }   // 手指方向选目标：右摇杆归零时恢复（hle_private_aim.c） | target selection by finger direction: restored when the right stick returns to zero (hle_private_aim.c)
    if (atomic_load(&g_stall_due) && atomic_exchange(&g_stall_due, 0)) usleep((useconds_t)g_stall_ms * 1000);   // DM_TEST_HID_STALL_MS
    uint64_t read_start_ns = g_receipt_file ? now_ns() : 0; // 覆盖整个多原子字段读取区间 | covers the whole read interval of the multi-atomic fields
    memset(s, 0, sizeof *s);
    s->sampling = ++g_sampling;
    if (!(style_of(id) & style)) { receipt(s, id, style, 0, read_start_ns); return; } // 没接这个样式：全0返回也收样 | this style is not connected: an all-zero return is sampled as well
    uint64_t km = keys_now();
    // 各通道的键先合起来（触屏 / 按键脚本 / 宿主替按 + 手柄；手柄没连时全 0），再裁决「独占的一下」——替玩家按的那一下要看的是游戏这一次实际会收到什么，
    // 不是某一个通道（先前只看了触屏通道，手柄玩家进局头 1.5 秒按 ＋ 开背包仍会把独占的 L 撞成换页签）
    // Merge the buttons of all channels first (touch / key script / host-pressed + controller; all 0 when no controller is connected), then arbitrate the
    // "exclusive press": the press made on the player's behalf must look at what the game will actually receive this time,
    // not at one single channel (previously only the touch channel was checked, so a controller player pressing + to open the inventory in the first 1.5
    // seconds after joining would still turn the exclusive L into a tab switch)
    { uint64_t dm_ui_alone_take(uint64_t);
      uint64_t tb = ((g_in.buttons | atomic_load(&g_press_latch)) & ~dm_ui_auto_release()) | (km & ~(uint64_t)PK_MASK) | dm_ui_auto_buttons(), pb = atomic_load(&g_pad_btn) | atomic_load(&g_pad_latch);
      static int t_old = -1; if (t_old < 0) t_old = getenv("DM_TEST_ALONE_NOPAD") != NULL;   // 测试对照：复刻改前的做法（裁决不看手柄键、独占时手柄键照样并进去） | Test control: reproduces the pre-change behavior (the arbitration ignores controller buttons, and controller buttons are still merged in during an exclusive press)
      uint64_t alone = dm_ui_alone_take(t_old ? tb : tb | pb);
      if (alone) s->buttons = t_old ? alone | atomic_load(&g_pad_btn) | atomic_exchange(&g_pad_latch, 0) : alone;   // 独占的一下（dm_cursor.c）：别的键这几帧不交，两条按下沿锁存都不清，之后照常给出 | The exclusive press (dm_cursor.c): other buttons are withheld for these few frames, neither press-edge latch is cleared, and they are delivered as usual afterwards
      else s->buttons = ((g_in.buttons | atomic_exchange(&g_press_latch, 0)) & ~dm_ui_auto_release()) | (km & ~(uint64_t)PK_MASK) | dm_ui_auto_buttons() | atomic_load(&g_pad_btn) | atomic_exchange(&g_pad_latch, 0); }
    s->lx = g_in.lx; s->ly = g_in.ly; s->rx = g_in.rx; s->ry = g_in.ry;
    // 摇杆：触屏摇杆有值就用触屏的，否则用手柄的。手柄那边只在手柄模式、且摇杆确实推出来了才给非零值（dm_gamepad.m），放在一边有点漂移的手柄到不了这里
    // Sticks: if the touch stick has a value use it, otherwise use the controller's. The controller side only gives a non-zero value in controller mode and when the
    // stick has actually been pushed out (dm_gamepad.m), so a slightly drifting controller left lying aside never reaches here
    { uint64_t ax = atomic_load(&g_pad_axes); int16_t plx = (int16_t)ax, ply = (int16_t)(ax >> 16), prx = (int16_t)(ax >> 32), pry = (int16_t)(ax >> 48);
      if (!(s->lx || s->ly) && (plx || ply)) { s->lx = plx; s->ly = ply; } if (!(s->rx || s->ry) && (prx || pry)) { s->rx = prx; s->ry = pry; } }
    { int dm_padmode_input_gate(uint64_t *); if (dm_padmode_input_gate(&s->buttons)) s->lx = s->ly = s->rx = s->ry = 0; }   // 手柄 / 触屏切换事务进行中：键和摇杆都不交给游戏；触发切换时按着的键松开前也不交（dm_padmode.c） | Controller / touch switch transaction in progress: neither keys nor sticks are handed to the game; keys held when the switch was triggered are not handed over either until released (dm_padmode.c)
    if (dm_pad_hook) dm_pad_hook(guest_ns(), &s->buttons, &s->lx, &s->ly, &s->rx, &s->ry);
    // 镜头实验：摇杆 / 瞄准按透视画面方向换成原版方向（dm_cam.c；镜头关着原样不动）。放在 dm_pad_hook 之后：安卓的触屏摇杆是在那里才填进来的
    // （touch_android.c dm_android_touch_step），放在前面它就没被换算，镜头转过以后推上仍按原版画面的「上」走（OnePlus 13 实测）。
    // iOS 与 Mac 键盘的摇杆从 dm_input_set 进来、早就在 s 里；Mac 的 dm_pad_hook 不给摇杆值，所以挪到这里对它们没有影响
    // Camera experiment: sticks / aim converted from perspective-picture directions to original directions (dm_cam.c; unchanged while the camera is off).
    // After dm_pad_hook: Android's touch stick is filled in only there (touch_android.c dm_android_touch_step), so before it the touch stick was not
    // converted, and after the camera turned, pushing up still walked "up" on the original picture (measured on a OnePlus 13). The iOS and Mac keyboard
    // sticks come in through dm_input_set and are already in s, and the Mac's dm_pad_hook gives no stick values, so this move does not affect them
    { void dm_cam_remap_stick(int32_t *, int32_t *, int32_t *, int32_t *); dm_cam_remap_stick(&s->lx, &s->ly, &s->rx, &s->ry); }
    { uint64_t dm_runeword_pad_filter(uint64_t); s->buttons = dm_runeword_pad_filter(s->buttons); }   // 手柄模式符文之语清单开着时十字键 / A / B / L / R 归清单（dm_runeword.c） | While the runeword list is open in controller mode, the D-pad / A / B / L / R belong to the list (dm_runeword.c)
    { uint64_t dm_padmode_page_filter(uint64_t); s->buttons = dm_padmode_page_filter(s->buttons); }   // 手柄模式我们自己的页（自定义页等）开着时，页声明的键归页（dm_padmode.c） | While one of our own pages (the Custom page etc.) is open in controller mode, the buttons the page declares belong to the page (dm_padmode.c)
    { uint64_t dm_swapskill_filter(uint64_t); s->buttons = dm_swapskill_filter(s->buttons); }   // 副手组技能：换武器好之前压住那个键（dm_actions.c） | off-hand set skill: hold that key back until the weapon swap is done (dm_actions.c)
    { static int trx, tryy, tdelay, held, tset = -1; if (tset < 0) { const char *e = getenv("DM_TEST_RSTICK"); tset = e && sscanf(e, "%d,%d,%d", &trx, &tryy, &tdelay) >= 2; }   // 测试开关 DM_TEST_RSTICK=rx,ry[,延迟次数]：按键脚本按着 A / B / X / Y 时把右摇杆置成这个值（模拟「按住技能钮拖动」，游戏自己也看得到右摇杆）；延迟 = 按下后第几次取手柄才开始给（真手指是先按下、再拖出方向） | test switch DM_TEST_RSTICK=rx,ry[,delay count]: while the key script holds A / B / X / Y, set the right stick to this value (simulates "hold a skill button and drag", the game itself also sees the right stick); delay = on which controller read after the press it starts being given (a real finger presses first, then drags out a direction)
      if (tset > 0) { held = (s->buttons & 0xf) ? held + 1 : 0; if (held > tdelay) { s->rx = trx; s->ry = tryy; } } }
    if (s->lx || s->ly || (km & PK_MASK & ~(uint64_t)PK_TOUCH)) atomic_store(&g_stick_ns, now_ns());   // 玩家的摇杆（前端 / 脚本伪键）最近一次非零；光标改写的摇杆不算 | the last time the player's stick (front end / script pseudo-keys) was non-zero; a stick rewritten by the cursor does not count
    dm_cursor_apply(&s->buttons, &s->lx, &s->ly);   // 指针目标：本次采样内按原版光标位置改写左摇杆 / A（见 dm_cursor.h） | pointer target: within this sample, rewrite the left stick / A according to the original game's cursor position (see dm_cursor.h)
    { static uint64_t prev; if ((s->buttons & DM_BTN_L) && !(prev & DM_BTN_L)) { int n = atomic_fetch_add(&g_dm_l_presses, 1) + 1; if (n <= 3) LOG("取手柄处：「显示物品」键（L）交给了游戏，本进程第 %d 次", n); } prev = s->buttons; }   // 「显示物品」键（L）交给游戏的按下次数（首领掉落读数用，dm_bossrush.c） | number of presses of the 「显示物品」(Show Items) key (L) delivered to the game (used by the boss drop readout, dm_bossrush.c)
    if (km & PK_LSL) s->lx = -32767; if (km & PK_LSR) s->lx = 32767;   // 脚本摇杆伪键：推满（Horizon 摇杆 ±32767，上为正） | script stick pseudo-keys: push to full (Horizon stick ±32767, up is positive)
    if (km & PK_LSU) s->ly = 32767;  if (km & PK_LSD) s->ly = -32767;
    // 镜头实验：脚本摇杆伪键在上面的换算之后才写进来，镜头开着时补换算一次，测试脚本推的方向才和手指推的一样（镜头关着时是空操作）
    // Camera experiment: the script stick pseudo-keys are written after the conversion above, so convert them once more while the camera is on, so a test
    // script pushes the same way a finger does (a no-op while the camera is off)
    if (km & PK_MASK & ~(uint64_t)PK_TOUCH) { void dm_cam_remap_stick(int32_t *, int32_t *, int32_t *, int32_t *); int32_t zx = 0, zy = 0; dm_cam_remap_stick(&s->lx, &s->ly, &zx, &zy); }
    s->attributes = ATTR_CONNECTED;
    atomic_store_explicit(&g_sampled_right,(uint32_t)s->rx|((uint64_t)(uint32_t)s->ry<<32),memory_order_release);
    receipt(s, id, style, km, read_start_ns);
}

HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "连 hid 服务；宿主从 dm_input 取输入", void, h_hid_InitNpad, void) {}
HLE_FORCE("_ZN2nn3hid21InitializeTouchScreenEv", "连 hid 服务；触屏输入由前端另接", void, h_hid_InitTouch, void) {}
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "支持的手柄号；宿主固定", void, h_hid_SetIds, const uint32_t *ids, uint64_t n) { (void)ids; (void)n; }
// BitFlagSet<32> 按值传在 w 寄存器
// BitFlagSet<32> is passed by value in a w register
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "支持的手柄样式；宿主固定", void, h_hid_SetStyles, uint32_t styles) { (void)styles; }
HLE_FORCE("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", "掌机手柄激活方式；宿主固定", void, h_hid_SetHhMode, int m) { (void)m; }
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "手柄样式查询；跟 DM_OPMODE 走", uint32_t, h_hid_GetStyle, const uint32_t *id) { return style_of(*id); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", "全键手柄状态；取 dm_input", void, h_hid_GetFullKey, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", "掌机手柄状态；取 dm_input", void, h_hid_GetHandheld, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_HANDHELD); }

// 震动：设备数 0，游戏不会再初始化 / 发送
// Vibration: device count 0, the game will not initialize / send any further
HLE_FORCE("_ZN2nn3hid20IsVibrationPermittedEv", "震动许可；宿主不支持（返回 bool）", unsigned, h_hid_VibPermit, void) { return 0; }
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
          "震动设备；宿主 0 个", int, h_hid_GetVibHandles, void *out, int max, const uint32_t *id, uint32_t style) { (void)out; (void)max; (void)id; (void)style; return 0; }
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "震动；宿主不支持", void, h_hid_InitVib, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "震动；宿主不支持", void, h_hid_SendVib, const void *h, const void *v) { (void)h; (void)v; }

// 触屏：nn::hid::TouchScreenState<16> = { int64 sampling; int32 count; int32 _r; TouchState touches[16]（各 0x28） }
// 内容由 hle_touch.c 按前端推来的触点填（布局与短按锁存见该文件）；游戏只看第 0 份，返回写入的状态份数 1
// Touchscreen: nn::hid::TouchScreenState<16> = { int64 sampling; int32 count; int32 _r; TouchState touches[16] (0x28 each) }
// Contents are filled by hle_touch.c from the touch points pushed by the front end (see that file for the layout and the short-press latch); the game only looks at entry 0, returns the number of states written, 1
typedef struct { int64_t sampling; int32_t count, _r; uint8_t touches[16][0x28]; } TouchState16;
_Static_assert(sizeof(TouchState16) == 0x290, "TouchScreenState<16>");
int touch_fill(void *out, uint64_t now_ns);
HLE_FORCE("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", "触屏状态：前端触点（hle_touch.c）", int, h_hid_GetTouch, TouchState16 *out, int n) {
    if (n < 1) return 0;
    touch_fill(out, guest_ns());
    return 1;
}
