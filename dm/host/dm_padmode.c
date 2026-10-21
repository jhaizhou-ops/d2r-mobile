// 手柄 / 触屏两套界面布局（手柄支持 P2，连上手柄自动回原版手柄界面，碰屏幕回触屏界面，切换不重启、不丢操作）。
// 原版拼布局路径 = "data/global/ui/Layouts/" + 前缀 + 面板名 + "HD.json"（main+0xb84750），前缀取自只读数据段里的三个串
// 「Controller/Console/Switch/」「Controller/Console/」「Controller/」（main+0xb1d1c8 / 0xb1d384 选，先试专用目录再退通用目录）。
// 触屏覆盖层把触屏版整套布局放在 layouts 下三个新目录（gen_romfs_overlay.py，原版 layouts/controller/ 原样保留），这里把三个前缀改写成同长的新目录名：
//   · 开机：覆盖层根目录有标记文件 ctrl_touch 才改写（没有触屏覆盖层的宿主局照原版），在游戏建任何面板之前；
//   · 切换：改写前缀后让游戏重载布局——只对 GetOperationMode 报另一个运行模式、推一次运行模式通知，几帧后报回原值再推一次（hle_oe.c dm_oe_ui_opmode）。
//     原版收到运行模式变化会按当前前缀重建全部面板；手柄样式与性能模式不动，所以不弹「重新连接控制器」、出图分辨率不变（宿主实测）。
// 字符串：手柄说法 ↔ 触摸说法（「按 A 使用」→「双击使用」等 25 条）也按模式换。覆盖层不改字符串文件，旁路表 touch_strings.tsv（gen_romfs_overlay.py）给 键 / 原文校验 / 触摸说法；
//   当前语言串表（main+0x2d71030，节点 {+0 下一个, +8 键, +0x10 键长, +0x20 串, +0x28 串长}，见 dm_bossrush.c）里按键找到节点，触屏模式把串指针换成触摸说法、手柄模式换回原值。
//   原文校验（SHA-1 前 8 位）对不上的不换（非简中语言、原包换版本）；切语言时原版重装串表、节点全换，每 30 帧核一次、节点变了重认。
// 只读数据段宿主可写（loader.c 只封代码区与补丁区），串的原值逐个核对，对不上不写（游戏版本不对）。
// Two sets of UI layouts, controller / touch (controller support P2: connecting a controller automatically returns to the original controller UI, touching the
// screen returns to the touch UI; switching needs no restart and loses no input).
// The original builds a layout path as "data/global/ui/Layouts/" + prefix + panel name + "HD.json" (main+0xb84750), with the prefix taken from three
// strings in the read-only data segment,
// "Controller/Console/Switch/", "Controller/Console/" and "Controller/" (selected at main+0xb1d1c8 / 0xb1d384, trying the specific directory first and
// falling back to the generic one).
// The touch overlay puts the complete touch layout set into three new directories under layouts (gen_romfs_overlay.py; the original layouts/controller/
// is kept as is), and this code rewrites the three prefixes to new directory names of the same length:
//   - Boot: rewrite only if the overlay root has the marker file ctrl_touch (host runs without a touch overlay follow the original), before the game
//     builds any panel;
//   - Switch: after rewriting the prefixes make the game reload its layouts: report the other operation mode to GetOperationMode only and push one
//     operation-mode notification, then a few frames later report the original value again and push another (hle_oe.c dm_oe_ui_opmode).
//     On an operation-mode change the original rebuilds all panels with the current prefixes; controller style and performance mode are not touched, so
//     no "Reconnect controller" prompt appears and the render resolution does not change (measured on the host).
// Strings: controller wording ↔ touch wording ("Press A to use" → "Double-tap to use" and so on, 25 entries) is also switched by mode. The overlay does not
// modify the string files; the sidecar table touch_strings.tsv (gen_romfs_overlay.py) gives key / source checksum / touch wording;
//   the node is found by key in the current language's string table (main+0x2d71030, node {+0 next, +8 key, +0x10 key length, +0x20 string, +0x28 string
//   length}, see dm_bossrush.c); touch mode swaps the string pointer to the touch wording and controller mode swaps the original value back.
//   Entries whose source checksum (first 8 hex digits of SHA-1) does not match are not swapped (non-Simplified-Chinese languages, a different version of
//   the original package); on a language switch the original reloads the string table and all nodes change, so check every 30 frames and re-identify
//   when a node has changed.
// The read-only data segment is writable by the host (loader.c only seals the code region and the patch region); the original value of each string is
// verified one by one, and nothing is written on a mismatch (wrong game version).
#include "hle.h"
#include "dm_hud.h"
#include "dm_input.h"
#include "dm_i18n.h"
#include "dm_modetx.h"
#include "dm_mode_items.h"
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>

static const struct { uint32_t off; const char *pad, *touch; } k_pfx[] = {
    { 0x1d21db5, "Controller/Console/Switch/", "0touch_console_switch_lay/" },
    { 0x1d2c33f, "Controller/Console/", "Ztouch_console_lay/" },
    { 0x1d3b705, "Controller/", "Xtouch_lay/" },
    { 0x1d0f5b8, "Controller/ControllerButtons/%s/%s", "Ztouch_controller_buttons_ui/%s_%s" },   // 键位图标（触屏版是透明的） | Button icons (transparent in the touch version)
    { 0x1d076c7, "Controller/Cursor/Idle", "Xtouch_cursor_lay/Idle" },   // 面板里的手柄光标圆环（触屏版是透明的） | Controller cursor ring in panels (transparent in the touch version)
    { 0x1d3acbc, "Controller/Cursor/Hold", "Xtouch_cursor_lay/Hold" },
    { 0x1d4d5ea, "Controller/Cursor/Held", "Xtouch_cursor_lay/Held" },
};
enum { BOUNCE_FRAMES = 60 };   // 测试开关读串的等待帧数（切换本身不数帧，见 dm_modetx.h） | Number of frames to wait when the test switch reads its sequence (the switch itself does not count frames, see dm_modetx.h)
static int g_have;                  // 有触屏覆盖层、前缀可改 | A touch overlay is present and the prefixes can be rewritten
static _Atomic int g_want = -1;     // 前端要的模式：0 触屏 1 手柄（-1 没要求） | Mode requested by the front end: 0 touch, 1 controller (-1 no request)
static int g_cur;                   // 当前前缀所指：0 触屏 1 手柄 | What the prefixes currently point to: 0 touch, 1 controller
static dm_modetx g_tx;              // 切换事务（dm_modetx.h）；游戏主线程 | Switch transaction (dm_modetx.h); game main thread
static _Atomic int g_freeze;        // 输入冻结：请求一到就立（任意线程），事务完成才撤；取手柄处经 dm_padmode_input_gate 看 | Input freeze: set as soon as a request arrives (any thread), lifted only when the transaction completes; the place where the controller is read checks it through dm_padmode_input_gate
static uint32_t g_sig;              // 回读信号计数：游戏来读了我们报的运行模式（hle_oe.c）+ 界面根下的面板换了一批（地址指纹变了）；事务看它「动过、又连续几帧不动」判重建完成 | Readback signal count: the game came to read the operation mode we report (hle_oe.c) + the set of panels under the UI root was replaced (address fingerprint changed); the transaction judges the rebuild complete by it having "moved, then stayed still for several consecutive frames"
static uint64_t g_tx_from;          // 本次事务开始的帧 | Frame at which this transaction started
static _Atomic uint32_t g_rebuilds; // 布局重建完成的次数：前端据此重认 HUD（技能钮快照的 HUD 集合按控件地址认，重建后全换了） | Number of completed layout rebuilds: the front end uses it to re-identify the HUD (the HUD set of the skill-button snapshot is identified by widget address, and they all change after a rebuild)
uint32_t dm_padmode_rebuilds(void) { return atomic_load(&g_rebuilds); }

// Mac App 的键盘键帽（只在 macOS 宿主里编；iOS 包没有这一套）：手柄版布局不变，只把取键位图标的路径换到覆盖层里那套键盘键名的图
// （ios/tools/kbd_glyphs.py，与触屏的透明键帽同目录、文件名「<平台>K<名字>」）。玩家按了键盘就用键盘键帽，实体手柄有输入就换回手柄键帽；
// 换图要让游戏重载布局，走的就是手柄 / 触屏那同一个切换事务（事务里的「模式」多带一位：第 0 位手柄版、第 1 位键盘键帽）。
// Keyboard key caps of the Mac App (compiled only in the macOS host; the iOS package does not have them): the gamepad layout is unchanged, only the path the key icons are taken from is switched to the keyboard-key images in the overlay
// (ios/tools/kbd_glyphs.py, in the same directory as the touch layout's transparent key caps, file names "<platform>K<name>"). When the player presses the keyboard, keyboard key caps are used; when a physical gamepad has input, it switches back to gamepad key caps;
// swapping images requires the game to reload the layout, which goes through the same switching transaction as gamepad / touch (the transaction's "mode" carries one more bit: bit 0 gamepad layout, bit 1 keyboard key caps).
#if TARGET_OS_OSX
#define KBD_GLYPH_OFF 0x1d0f5b8
static const char k_kbd_glyph[] = "Ztouch_controller_buttons_ui/%sK%s";
static int g_kbd_have, g_kbd, g_kbd_next, g_app;   // 覆盖层带键盘键帽；当前路径指着键盘键帽；下一次写路径时要不要指键盘键帽；Mac App 形态 | the overlay carries keyboard key caps; the current path points at keyboard key caps; whether the next path write should point at keyboard key caps; Mac App form
static _Atomic int g_kbd_want;
void dm_padmode_request_kbd(int kbd) { kbd = kbd ? 1 : 0; if (!g_kbd_have) return; atomic_store(&g_kbd_want, kbd); }   // 前端（任意线程）：键盘有输入 1 / 实体手柄有输入 0。这里不立输入冻结：换键帽要等面板都关了才起（见 kbd_swap_ok），等的时候玩家照常操作；事务起的那一帧才冻结 | front end (any thread): keyboard had input 1 / physical gamepad had input 0. No input freeze is raised here: swapping key caps waits until all panels are closed (see kbd_swap_ok), and the player operates as usual while waiting; the freeze starts only on the frame the transaction starts
int dm_padmode_kbd(void) { return g_kbd; }
// 只换键帽的重载只在「没有面板开着」时起：重载布局会把开着的页重建一遍，设置页的勾选框会全显示成未勾、还多出「全部接受」（实测），
// 所以局内要 HUD 在屏、游戏没停（暂停菜单 / 设置页 / 借壳的页）、手势层认的面板一个没开；前端要删除 / 选项 / 起名 / 确认框都没开。等到条件满足的那一帧再换
// The reload that only swaps key caps starts only when "no panel is open": reloading the layout rebuilds any open page, and the checkboxes of the settings page would all show as unchecked with an extra "Accept All" appearing (measured),
// so in game the HUD must be on screen, the game not stopped (pause menu / settings page / pages borrowing another shell) and none of the panels the gesture layer recognises open; in the front end none of delete / options / naming / confirmation boxes open. The swap happens on the first frame where the conditions hold
static int kbd_swap_ok(uint8_t *b) {
    int dm_ingame_now(void), dm_ui_panel_state(void), dm_ui_fe_drag_ok(void);
    if (!dm_ingame_now()) return dm_ui_fe_drag_ok();
    return !(*(volatile uint8_t *)(b + 0x2d5ff51) | *(volatile uint8_t *)(b + 0x2d5ff67)) && !dm_ui_panel_state();
}
#endif
static int pfx_write(uint8_t *b, int pad) {
    for (unsigned i = 0; i < sizeof k_pfx / sizeof *k_pfx; i++) { const char *p = (const char *)(b + k_pfx[i].off);
#if TARGET_OS_OSX
        if (k_pfx[i].off == KBD_GLYPH_OFF && !strcmp(p, k_kbd_glyph)) continue;
#endif
        if (strcmp(p, k_pfx[i].pad) && strcmp(p, k_pfx[i].touch)) { LOG("【红】手柄 / 触屏布局：main+%#x 不是预期的「%s」（是「%.32s」），不改", k_pfx[i].off, k_pfx[i].pad, p); return 0; } }
    for (unsigned i = 0; i < sizeof k_pfx / sizeof *k_pfx; i++) memcpy(b + k_pfx[i].off, pad ? k_pfx[i].pad : k_pfx[i].touch, strlen(k_pfx[i].pad));
#if TARGET_OS_OSX
    _Static_assert(sizeof k_kbd_glyph == sizeof "Controller/ControllerButtons/%s/%s", "键盘键帽路径要与原版路径等长");
    if (pad && g_kbd_next) memcpy(b + KBD_GLYPH_OFF, k_kbd_glyph, sizeof k_kbd_glyph - 1);
#endif
    return 1;
}
enum { NSTR = 160 };   // 中 / 英 / esES / esMX / ruRU 各一套（去重后 114 条） | One set each for Chinese / English / esES / esMX / ruRU (114 entries after dedup)
static struct { char *key, *val; size_t klen; char sha[9]; uint64_t node, orig, olen; } g_str[NSTR];
static int g_nstr;
static uint8_t *g_b;
static uint64_t rd8(uint64_t a) { return a ? *(volatile uint64_t *)(uintptr_t)a : 0; }
static void unesc(char *s) { char *o = s; for (; *s; s++) { if (*s == '\\' && s[1]) { s++; *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s; } else *o++ = *s; } *o = 0; }
static void str_load(void) {   // 开机读旁路表 | Read the sidecar table at boot
    char p[1100], line[1024]; FILE *f = dm_overlay_file("touch_strings.tsv", p, sizeof p) ? fopen(p, "r") : NULL; if (!f) { LOG("【红】手柄 / 触屏布局：覆盖层有 ctrl_touch 却没有 touch_strings.tsv，字符串不换"); return; }
    while (g_nstr < NSTR && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; char *a = strchr(line, '\t'), *c = a ? strchr(a + 1, '\t') : NULL; if (!c || c - a - 1 != 8) continue;
        *a = 0; *c = 0; unesc(c + 1); g_str[g_nstr].key = strdup(line); g_str[g_nstr].klen = strlen(line); memcpy(g_str[g_nstr].sha, a + 1, 8); g_str[g_nstr].val = strdup(c + 1); g_nstr++; }
    if (g_nstr == NSTR && fgets(line, sizeof line, f)) LOG("【红】手柄 / 触屏布局：touch_strings.tsv 超过 %d 条，后面的没读（调大 NSTR）", NSTR);
    fclose(f); LOG("手柄 / 触屏布局：触摸说法 %d 条（touch_strings.tsv）", g_nstr);
}
static uint64_t str_node(uint64_t map, const char *key, size_t n) {   // 串表里键为 key 的节点（同 dm_bossrush.c loc_has） | The node in the string table whose key is key (same as loc_has in dm_bossrush.c)
    uint64_t cnt = rd8(map), bk = rd8(map + 8); if (!cnt || !bk || (cnt & (cnt - 1))) return 0;
    uint64_t h[4] = {0}; ((void (*)(uint64_t *, const char *, uint64_t, int))(g_b + 0x1b6e1e0))(h, key, n, 0);   // 出参 16 字节 | Output parameter is 16 bytes
    for (uint64_t nd = rd8(bk + 8 * (h[0] & (cnt - 1))); nd; nd = rd8(nd)) if (rd8(nd + 0x10) == n && !memcmp((const void *)(uintptr_t)rd8(nd + 8), key, n)) return nd;
    return 0;
}
static void str_apply(int pad) {   // 游戏主线程：串表里这些键的串按模式指向原文 / 触摸说法 | Game main thread: point the strings of these keys in the string table at the original text / touch wording according to the mode
    static int warned; int nset = 0, nbad = 0;
    for (int i = 0; i < g_nstr; i++) { uint64_t nd = str_node((uint64_t)(uintptr_t)(g_b + 0x2d71030), g_str[i].key, g_str[i].klen); if (!nd) { nbad++; continue; }
        uint64_t *v = (uint64_t *)(uintptr_t)(nd + 0x20);
        if (nd != g_str[i].node) {   // 新节点（开机首次 / 切语言后串表重装）：认原文 | New node (first time after boot / string table reloaded after a language switch): identify the original text
            uint64_t ov = v[0], ol = v[1]; if (ov == (uint64_t)(uintptr_t)g_str[i].val) continue;   // 不会发生：新节点不会指着我们的串 | Cannot happen: a new node never points at our string
            unsigned char md[CC_SHA1_DIGEST_LENGTH]; char hx[9]; CC_SHA1((const void *)(uintptr_t)ov, (CC_LONG)ol, md); snprintf(hx, sizeof hx, "%02x%02x%02x%02x", md[0], md[1], md[2], md[3]);
            g_str[i].node = nd; g_str[i].orig = memcmp(hx, g_str[i].sha, 8) ? 0 : ov; g_str[i].olen = ol; if (!g_str[i].orig) { nbad++; continue; } }
        if (!g_str[i].orig) { nbad++; continue; }
        uint64_t want = pad ? g_str[i].orig : (uint64_t)(uintptr_t)g_str[i].val, wl = pad ? g_str[i].olen : strlen(g_str[i].val);
        if (v[0] != want) { v[1] = wl; v[0] = want; nset++; } }
    if (nset) LOG("手柄 / 触屏布局：串表 %d 条换成%s说法（%d 条没换：不是这一种语言 / 原文对不上 / 键不在）", nset, pad ? "手柄" : "触摸", nbad);
    else if (nbad == g_nstr && g_nstr && !warned) { warned = 1; LOG("手柄 / 触屏布局：串表里 %d 条都没换（语言不在表里或原文对不上），照原版说法", nbad); }
}
// 系统提示框在屏上时的功能键（观测门 OBS_NOTICE_INPUT，原版提示面板的消息处理入口 main+0x3d84a0，游戏主线程）：
// 原版收到 InputMessage / ControllerActionBegin、动作号 9（功能键）就重发提示的回调并把界面消息总线标成「已消费」（[总线+0xb8] = 1）——
// 手柄上是「有提示先按一下关掉」，触屏上玩家点的是屏上的十字按钮：点了只关提示、面板没开，要点第二下。
// 触屏模式下把 x1 换成一份动作号不是 9 的消息副本（消息 {+0 类别, +8 动作, +0x10 → 载荷，载荷 +0x10 = 动作号}）：这个面板当成别的键看、不消费，
// 功能键照常开面板，提示自己到点消失。手柄模式不动（原版）。只换这一个面板看到的副本，别的面板收到的还是原消息
// The function key while a system notification is on screen (observation gate OBS_NOTICE_INPUT, the message handler entry of the original game's notification panel main+0x3d84a0, game main thread):
// on receiving InputMessage / ControllerActionBegin with action number 9 (function key), the original game re-sends the notification's callback and marks the UI message bus as "consumed" ([bus+0xb8] = 1) --
// on a controller that is "if there is a notification, press once first to dismiss it"; on a touchscreen what the player taps is the cross button on screen: the tap only dismisses the notification, the panel does not open, and a second tap is needed.
// In touch mode x1 is replaced with a copy of the message whose action number is not 9 (message {+0 category, +8 action, +0x10 → payload, payload +0x10 = action number}): this panel sees it as some other key and does not consume it,
// the function key opens the panel as usual, and the notification disappears by itself when its time is up. Controller mode is untouched (original game). Only the copy seen by this one panel is replaced; what other panels receive is still the original message
static void notice_input_obs(const uint64_t *regs) {
    if (!g_have || g_cur) return; uint64_t msg = regs[1]; if (!msg) return;
    if (*(uint64_t *)msg != 0x4ae067c6248b6042ull || *(uint64_t *)(msg + 8) != 0x1e439768f7dd4594ull) return;   // InputMessage / ControllerActionBegin（FNV-1a 64）
    uint64_t pay = *(uint64_t *)(msg + 0x10); if (!pay || *(int32_t *)(pay + 0x10) != 9) return;
    static uint64_t m2[8], p2[8]; memcpy(m2, (void *)msg, sizeof m2); memcpy(p2, (void *)pay, sizeof p2); *(int32_t *)((uint8_t *)p2 + 0x10) = -1; m2[2] = (uint64_t)(uintptr_t)p2;
    ((uint64_t *)regs)[1] = (uint64_t)(uintptr_t)m2;
    static int n; if (n++ < 20) LOG("手柄 / 触屏布局：系统提示框在屏上时按了功能键 → 触屏模式不让提示框吃掉这一下");
}
void dm_padmode_boot(void) {   // 开机：游戏建面板之前（main.c，nnMain 前） | Boot: before the game builds its panels (main.c, before nnMain)
    if (gate_obs_present(OBS_NOTICE_INPUT)) gate_obs_set(OBS_NOTICE_INPUT, notice_input_obs);
    module_t *m = NULL; for (int i = 0; i < g_nmod; ++i) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i]; char mk[1100];
    if (!m || !getenv("DM_ROMFS_OVERLAY")) return;
    if (!dm_overlay_file("ctrl_touch", mk, sizeof mk)) { LOG("手柄 / 触屏布局：覆盖层没有 ctrl_touch 标记，照原版布局"); return; }
    int pad = getenv("DM_APP") != NULL;   // Mac App：没有触屏圆钮与底栏的绘制层，开机就用原版手柄版（我们的功能页在手柄版布局里同样有），之后也不切 | Mac App: there is no drawing layer for the touch buttons and bottom bar, so it starts in the original gamepad UI (our feature pages exist in the gamepad layout as well) and never switches afterwards
#if TARGET_OS_OSX
    g_app = pad; g_kbd_have = pad && dm_overlay_file("kbd_glyphs", mk, sizeof mk); g_kbd = g_kbd_next = g_kbd_have; atomic_store(&g_kbd_want, g_kbd);   // Mac App 开机先用键盘键帽（多数人没接手柄）；实体手柄一有输入就换 | the Mac App starts with keyboard key caps (most people have no gamepad connected); it switches as soon as a physical gamepad has input
    if (pad) LOG("手柄 / 触屏布局：键盘键帽 %d", g_kbd_have);
#endif
    if (pfx_write((uint8_t *)m->base, pad)) { g_have = 1; g_cur = pad; atomic_store(&g_want, pad); g_b = (uint8_t *)m->base; str_load(); LOG("手柄 / 触屏布局：开机用%s", pad ? "原版手柄版（Mac App）" : "触屏版（layouts/xtouch_lay 等三个目录）"); }
}
void dm_padmode_request(int pad) { pad = pad ? 1 : 0;
#if TARGET_OS_OSX
    if (g_app) pad = 1;   // Mac App 没有触屏圆钮与底栏的绘制层：手柄断开也留在手柄版，不切触屏版 | the Mac App has no drawing layer for the touch round buttons and bottom bar: stay in the gamepad layout even when the gamepad disconnects, do not switch to the touch layout
#endif
    atomic_store(&g_want, pad); if (g_have && pad != g_cur) atomic_store(&g_freeze, 1); }   // 前端（任意线程）：从这一刻起输入就不再交给游戏 | Front end (any thread): from this moment on input is no longer handed to the game
int dm_padmode_current(void) { return g_cur; }
int dm_padmode_busy(void) { return atomic_load(&g_freeze); }   // 切换事务进行中（含请求已到、事务还没起的那一小段） | Switch transaction in progress (including the short span where the request has arrived but the transaction has not started yet)
int dm_padmode_input_gate(uint64_t *buttons) { static dm_modegate g; return dm_modegate_apply(&g, atomic_load(&g_freeze), buttons); }   // 取手柄处（hle_hid.c fill，游戏主线程） | Where the controller is read (hle_hid.c fill, game main thread)
static void dm_mode_none(void) {}
#define X(name, side, enter, leave) void enter(void), leave(void);
DM_MODE_ITEMS(X)
#undef X
static const struct { const char *name; int side; void (*enter)(void), (*leave)(void); } k_items[] = {
#define X(name, side, enter, leave) { name, side, enter, leave },
    DM_MODE_ITEMS(X)
#undef X
};
static void items_run(int side, int leave) { for (unsigned i = 0; i < sizeof k_items / sizeof *k_items; i++) { if (k_items[i].side != 2 && k_items[i].side != side) continue; void (*f)(void) = leave ? k_items[i].leave : k_items[i].enter; if (f) f(); } }
// 我们自己的页在手柄模式下接管按键（自定义页、积分牌…；原版的焦点导航不认识我们加的控件）：页在屏上时每帧声明要哪些键（dm_padmode_page_keys，游戏主线程），
// 取手柄处（hle_hid.c fill）把这些键扣下不交给原版、按下沿攒给页（dm_padmode_page_keys 的返回值）。页不再声明（3 帧）就失效；失效时还按着的键松开前仍不交给原版。同一时刻只有一页在屏上
// 页接管了的键还想让原版照常收到一次短按时（如编年史仓库页的 Y：短按仍是原版的「丢弃」，按住才是我们的「存入」）：dm_padmode_page_pulse 让取手柄处替玩家按几次采样
// Our own pages take over keys in controller mode (custom page, kill board...; the original focus navigation does not know the widgets we
// added): while a page is on screen it declares every frame which keys it wants (dm_padmode_page_keys, game main thread),
// and where the controller is read (hle_hid.c fill) those keys are withheld from the original game and their press edges are accumulated for
// the page (the return value of dm_padmode_page_keys). Once the page stops declaring (3 frames) the takeover lapses; keys still held when it
// lapses are still not handed to the original game until released. Only one page is on screen at any moment
// When a page still wants the original game to receive one normal short press of a key it has taken over (e.g. Y on the Chronicle vault page:
// a short press is still the original "Drop", only a hold is our "Deposit"): dm_padmode_page_pulse makes the controller read press it for the
// player for a few samples
static _Atomic uint64_t g_pg_mask, g_pg_ev, g_pg_frame, g_pg_held, g_pg_pulse; static _Atomic int g_pg_pulse_n;
// 同一帧里可以有不止一处声明（如储藏箱个人页的 L / R 翻页，加上光标停在未鉴定物品上时的按住 A 鉴定）：同一帧的声明按位并起来，各取各声明的那几个键的按下沿、互不拿走别人的
// There can be more than one declaration in the same frame (e.g. L / R paging on the personal stash page, plus hold A to identify while the
// cursor rests on an unidentified item): declarations of the same frame are ORed together bitwise, and each takes the press edges of only the
// keys it declared, without taking anyone else's
uint64_t dm_padmode_page_keys(uint64_t mask) {   // 声明本帧要接管这些键，返回其中自上次取之后按下过的 | Declare that these keys are taken over this frame; returns those among them that were pressed since the last fetch
    uint64_t f = dm_guest_frame(); if (atomic_exchange(&g_pg_frame, f) == f) atomic_fetch_or(&g_pg_mask, mask); else atomic_store(&g_pg_mask, mask);
    return atomic_fetch_and(&g_pg_ev, ~mask) & mask;
}
uint64_t dm_padmode_page_held(void) { return atomic_load(&g_pg_held); }   // 接管的键里此刻还按着的 | Those of the taken-over buttons that are still held right now
void dm_padmode_page_pulse(uint64_t mask) { atomic_store(&g_pg_pulse, mask); atomic_store(&g_pg_pulse_n, 4); }
uint64_t dm_padmode_page_filter(uint64_t bt) {
    static uint64_t prev, eat; uint64_t down = bt & ~prev; prev = bt;
    uint64_t m = g_cur && dm_guest_frame() - atomic_load(&g_pg_frame) <= 3 ? atomic_load(&g_pg_mask) : 0;
    uint64_t pulse = 0; { int n = atomic_load(&g_pg_pulse_n); if (n > 0) { atomic_store(&g_pg_pulse_n, n - 1); pulse = atomic_load(&g_pg_pulse); } }
    if (m) { atomic_fetch_or(&g_pg_ev, down & m); atomic_store(&g_pg_held, bt & m); eat |= bt & m; return (bt & ~m) | pulse; }
    atomic_store(&g_pg_ev, 0); atomic_store(&g_pg_held, 0); eat &= bt; return (bt & ~eat) | pulse;
}
// 编年史仓库页开着时（手柄模式）把原版「按住 Y」那半句提示的字换成我们实际做的事：原版在储藏箱各页对背包物品的提示是「丢弃（按住移动到私人储藏箱）」，
// 第六页上按住 Y 被我们接管成存入编年史仓库（dm_vault_ui.c）。这半句只写「存入」：原版那条本来就超宽、在限宽里滚动（截图对照），写短到不滚，人就在编年史页上。换法同上面的手柄 / 触屏说法：只换当前语言串表里这个键的串指针，离开那一页换回
// 同一个换法现在有两条（一张小表）：编年史页的「存入」，和光标停在未鉴定物品上时把「捡起」换成「捡起（按住鉴定）」（手柄按住 A = 免费鉴定，dm_cursor.c；
// 原版对未鉴定装备只提示「A 捡起」，往提示条里加条目会被原版每帧按光标下的物品重建盖掉，所以换它取的那条串）
// While the Chronicle vault page is open (controller mode), replace the text of the original "hold Y" half of the hint with what we actually do:
// on every stash page the original hint for an inventory item is "Drop (hold to move to private stash)",
// and on the sixth page holding Y is taken over by us to deposit into the Chronicle vault (dm_vault_ui.c). This half only says "Deposit": the
// original string is already too wide and scrolls within the width limit (compared against screenshots), so it is kept short enough not to
// scroll, and the player is on the Chronicle page anyway. The swap works like the controller / touch wording above: only the string pointer of
// this key in the current language's string table is swapped, and it is swapped back on leaving that page
// The same swap now has two entries (one small table): "Deposit" on the Chronicle page, and replacing "Pick Up" with
// "Pick Up (hold to identify)" while the cursor rests on an unidentified item (controller hold A = free identify, dm_cursor.c;
// for unidentified equipment the original game only hints "A Pick Up", and an entry added to the button legend would be overwritten when the
// original game rebuilds it every frame from the item under the cursor, so the string it fetches is swapped instead)
enum { HINT_VAULT, HINT_IDENT, NHINT };
static struct { const char *key, *ours_key; _Atomic int on; uint64_t node, orig, olen; char ours[48]; } g_hint[NHINT] = {
    { "ControllerPromptItemMoveToPrivateStash", "dmPadHintDeposit" }, { "ControllerPromptItemPickUpFromInventory", "dmPadLegIdentify" } };   // [0]「按住 Y」那半句、[1]「捡起」 | [0] the "hold Y" half of the sentence, [1] "Pick Up"
void dm_padmode_vault_hint(int on) { atomic_store(&g_hint[HINT_VAULT].on, on ? 3 : 0); }   // 游戏主线程，页在屏上时每帧调（3 帧不调就算离开） | Game main thread; called every frame while the page is on screen (3 frames without a call counts as having left)
void dm_padmode_ident_hint(int on) { atomic_store(&g_hint[HINT_IDENT].on, on ? 3 : 0); }   // 同上：光标停在未鉴定物品上时每帧调 | Same as above: called every frame while the cursor rests on an unidentified item
// 切语言：原版把串表整张清空重装（SetLanguage，diag.c 的观测门在清空之前、游戏主线程调这里）。换串表与上面的提示小表都靠「节点地址变了」认重装，
// 新节点要是正好分配在旧地址上就认不出：记着的原文指针已被原版释放——手柄模式会把悬空指针写回串表，触屏模式会把旧语言的触摸说法写进新语言的表（原文校验被跳过）。
// 所以重装之前先收手：此刻指着我们缓冲区的节点写回原文（原版释放的是它自己的串），记的节点 / 原文全清，重装后下一轮按新节点重新认、重新过校验。
// Language switch: the original game clears the whole string table and reloads it (SetLanguage; the observation gate in diag.c calls here
// before the clear, on the game main thread). Both the string table swap and the small hint table above detect a reload by "the node address
// changed",
// so a new node that happens to be allocated at the old address goes undetected: the recorded original text pointer has already been freed by
// the original game -- controller mode would write a dangling pointer back into the string table, and touch mode would write the old
// language's touch wording into the new language's table (the original text check is skipped).
// So let go before the reload: nodes currently pointing at our buffer get the original text written back (what the original game frees is its
// own string), all recorded nodes / original texts are cleared, and after the reload the next round recognizes the new nodes again and runs the
// check again.
void dm_padmode_lang_reset(void) {
    int back = 0;
    for (int i = 0; i < g_nstr; i++) { if (g_str[i].node && g_str[i].orig) { uint64_t *v = (uint64_t *)(uintptr_t)(g_str[i].node + 0x20); if (v[0] == (uint64_t)(uintptr_t)g_str[i].val) { v[0] = g_str[i].orig; v[1] = g_str[i].olen; back++; } }
        g_str[i].node = g_str[i].orig = g_str[i].olen = 0; }
    for (int h = 0; h < NHINT; h++) { if (g_hint[h].node) { uint64_t *v = (uint64_t *)(uintptr_t)(g_hint[h].node + 0x20); if (v[0] == (uint64_t)(uintptr_t)g_hint[h].ours) { v[0] = g_hint[h].orig; v[1] = g_hint[h].olen; back++; } }
        g_hint[h].node = g_hint[h].orig = g_hint[h].olen = 0; }
    if (g_have) LOG("手柄 / 触屏布局：串表要重装（切语言）→ %d 条写回原文，记的节点清空，重装后重新认", back);
}
static void hint_pump(void) {
    for (int h = 0; h < NHINT; h++) { uint64_t *nodep = &g_hint[h].node; char *ours = g_hint[h].ours;
        // 我们这半句走中英表；串指针要常驻（换进串表的是这个缓冲区的地址），每次取来比一下、语言变了才改写缓冲区
        // Our half of the sentence goes through the Chinese-English table; the string pointer must stay resident (what is swapped into the string table
        // is the address of this buffer), so compare on every fetch and rewrite the buffer only when the language has changed
        { const char *w = dm_str(g_b, g_hint[h].ours_key); if (w && strcmp(ours, w)) { if (*nodep && ((uint64_t *)(uintptr_t)(*nodep + 0x20))[0] == (uint64_t)(uintptr_t)ours) ((uint64_t *)(uintptr_t)(*nodep + 0x20))[1] = strlen(w); strlcpy(ours, w, sizeof g_hint[h].ours); } }
        int on = g_cur && atomic_load(&g_hint[h].on) > 0; if (atomic_load(&g_hint[h].on) > 0) atomic_fetch_sub(&g_hint[h].on, 1);
        uint64_t nd = str_node((uint64_t)(uintptr_t)(g_b + 0x2d71030), g_hint[h].key, strlen(g_hint[h].key)); if (!nd) continue;
        uint64_t *v = (uint64_t *)(uintptr_t)(nd + 0x20);
        if (nd != *nodep) { if (v[0] == (uint64_t)(uintptr_t)ours) continue; *nodep = nd; g_hint[h].orig = v[0]; g_hint[h].olen = v[1]; }   // 新节点（开机首次 / 切语言后串表重装）：记原文 | New node (first time after boot / string table reloaded after a language switch): record the original text
        uint64_t want = on ? (uint64_t)(uintptr_t)ours : g_hint[h].orig, wl = on ? strlen(ours) : g_hint[h].olen;
        if (v[0] != want) { v[1] = wl; v[0] = want; LOG("手柄提示：%s → %s", g_hint[h].key, on ? ours : "原文"); } }
}
// 手柄版暂停菜单「游戏难度：N」行（布局 touch_layout.py pad_pause；难度挂暂停菜单）：按钮发 ClosePanel:TouchPadDiff（空操作，这里当信号收），按一下人数档 +1、8 之后回 1。
// 菜单开着时游戏停拍，请求要关了菜单才落地（dm_hud.c 在服务端拍入口取），所以连按时从「上一次请求的档」接着加，当前档位写在这一行我们自己的文字控件 TouchPadDiffTxt 上。
// "Game Difficulty: N" row of the controller pause menu (layout touch_layout.py pad_pause; difficulty lives in the pause menu): the button sends
// ClosePanel:TouchPadDiff (a no-op, received here as a signal); each press raises the /players setting by 1, wrapping back to 1 after 8.
// While the menu is open the game stops ticking and a request only takes effect once the menu is closed (dm_hud.c picks it up at the server tick entry),
// so repeated presses keep adding from "the last requested setting"; the current setting is written on this row in our own text widget TouchPadDiffTxt.
static _Atomic int g_pause_open, g_diff_pend;   // 暂停菜单开着；菜单开着期间已请求、还没落地的档（0 = 没有） | Pause menu is open; the setting requested while the menu is open that has not taken effect yet (0 = none)
void dm_padmode_on_uimsg(const char *cat, const char *act, const char *arg) {   // 界面消息回调（diag.c，游戏主线程） | UI message callback (diag.c, game main thread)
    if (!cat || !act || !arg || strcmp(cat, "PanelManager")) return;
    if (!strncmp(arg, "PauseLayout", 11)) { if (!strcmp(act, "OpenPanel")) { atomic_store(&g_pause_open, 1); atomic_store(&g_diff_pend, 0); } else if (!strcmp(act, "ClosePanel")) atomic_store(&g_pause_open, 0); return; }
    if (strcmp(act, "ClosePanel") || strcmp(arg, "TouchPadDiff")) return;
    dm_hud_info h; dm_hud_get(&h); int cur = atomic_load(&g_diff_pend); if (!cur) cur = h.players;
    if (!cur) { LOG("【红】手柄暂停菜单：游戏难度没读到当前档位，不动"); return; }
    int to = cur % 8 + 1; dm_hud_request_players(to); atomic_store(&g_diff_pend, to); LOG("手柄暂停菜单：游戏难度 %d → %d", cur, to);
}
static uint64_t pm_find(uint64_t w, const char *name, int dep) {   // 按名字找控件：名字 +8、子控件 +0x58、个数 +0x60（同 dm_custom.c） | Find a widget by name: name +8, children +0x58, count +0x60 (same as dm_custom.c)
    if (!w || dep > 6) return 0; const char *nm = (const char *)rd8(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = rd8(w + 0x58), n = rd8(w + 0x60); for (uint64_t i = 0; ch && i < n && i < 256; i++) { uint64_t r = pm_find(rd8(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
// 原版底部提示条（ControllerOverlay 面板 +0x440 的 ButtonLegendScrollWidget）的条目。原版各面板用三个函数管它（如物品栏「比较」main+0x3b61ec；全程序加 80 处、删 74 处）：
//   main+0xaa9358(条目, 动作号, 0, {字, 长}) 建条目；main+0xaa9ef4(提示条, 条目) 加（键帽相同的已有条目只换字，没有才追加）；main+0xaaa610(提示条, 动作号) 删。
//   条目数组 +0x88 / 个数 +0x90、每条 0x40：+0 键帽字形（建条目时由动作号按当前键位换算）、+8 字形个数、+0x10 串指针、+0x18 串长。
// 我们的页走同一个口：页在屏上时每帧声明要哪些条目（dm_padmode_legend），这里每帧核一遍——缺的调原版「加」，不再声明的（离开了那一页）调原版「删」；
// 删之前核那一条的字还是我们的（换到别的页后原版可能已经把同一个键帽换成它自己的提示）。原版在一页之内不定时清条目（实测稳态几百帧补加 0 次），所以不闪。
// 动作号从原版自己的提示读（括号里是 本机 iOS 包逐个实测到的键帽）：0 = A（选择）、1 = Y（丢弃）、2 = X（比较）、4 = 按右摇杆（排序）、7 = ZL、8 = ZR、10 = −（高级属性）、
// 11 / 14 = 十字键左 / 右、12 / 13 = 十字键上 / 下、19 = L、20 = R、23 = B（关闭菜单）
// Entries of the original bottom button legend (the ButtonLegendScrollWidget at +0x440 of the ControllerOverlay panel). The original panels manage it with
// three functions (e.g. inventory "Compare" main+0x3b61ec; 80 add sites and 74 remove sites program-wide):
//   main+0xaa9358(entry, action id, 0, {text, length}) builds an entry; main+0xaa9ef4(button legend, entry) adds it (an existing entry with the same keycap
//   only gets its text replaced, otherwise it is appended); main+0xaaa610(button legend, action id) removes it.
//   Entry array +0x88 / count +0x90, 0x40 per entry: +0 keycap glyphs (derived from the action id under the current key bindings when the entry is
//   built), +8 glyph count, +0x10 string pointer, +0x18 string length.
// Our pages use the same interface: while a page is on screen it declares every frame which entries it wants (dm_padmode_legend), and this code checks
// every frame: missing ones call the original "add", ones no longer declared (the page was left) call the original "remove";
// before removing, verify that the entry's text is still ours (after switching to another page the original may already have replaced the same keycap
// with its own hint). Within one page the original does not clear entries at arbitrary times (measured: 0 re-adds over several hundred frames in steady
// state), so there is no flicker.
// Action ids are read from the original's own hints (in parentheses: the keycap measured one by one on the iOS app on the local Mac): 0 = A (Select),
// 1 = Y (Drop), 2 = X (Compare), 4 = press right stick (Sort), 7 = ZL, 8 = ZR, 10 = - (Advanced Stats),
// 11 / 14 = D-pad left / right, 12 / 13 = D-pad up / down, 19 = L, 20 = R, 23 = B (Close menu)
enum { LG_MAX = 6 };
static struct { int act; char text[48]; } g_lg[LG_MAX], g_lg_on[LG_MAX]; static int g_lg_n, g_lg_on_n; static uint64_t g_lg_frame;
void dm_padmode_legend(int act, const char *text) {   // 游戏主线程，页在屏上时每帧调 | Game main thread; called every frame while the page is on screen
    uint64_t f = dm_guest_frame(); if (f != g_lg_frame) { g_lg_frame = f; g_lg_n = 0; }
    if (g_lg_n < LG_MAX) { g_lg[g_lg_n].act = act; strlcpy(g_lg[g_lg_n].text, text, sizeof g_lg[0].text); g_lg_n++; }
}
// 反过来：原版某一页自己加的条目，我们把那个键收掉了（页在屏上时每帧声明），提示条里这一条也要拿掉——调原版自己的「删」。
// 例：手柄版建角模式页的「X 经典版 / 术士君临」（原版在 main+0xc318d4 一带每次刷新这一页时加；它自己对不能建经典版的职业也是调同一个「删」）
// 条目在哪个提示条上：这一页自己的提示条控件（按名字找，如建角模式页的 CharacterOptionsLegend），不是局内底部那条
// The other way round: for an entry that an original page adds by itself and whose key we have taken away (declared every frame while the page is on screen), the entry in the legend must be removed too -- by calling the original game's own "remove".
// Example: 「X 经典版 / 术士君临」(X Classic / Reign of the Warlock) on the controller layout's character creation mode page (the original game adds it around main+0xc318d4 on every refresh of this page; for classes that cannot create a Classic character it calls the same "remove" itself)
// Which legend the entry is on: this page's own legend widget (found by name, e.g. CharacterOptionsLegend on the character creation mode page), not the one at the bottom in a run
static struct { const char *legend; int act; } g_lh[4]; static int g_lh_n; static uint64_t g_lh_frame;
void dm_padmode_legend_hide(const char *legend, int act) { uint64_t f = dm_guest_frame(); if (f != g_lh_frame) { g_lh_frame = f; g_lh_n = 0; } if (g_lh_n < 4) { g_lh[g_lh_n].legend = legend; g_lh[g_lh_n].act = act; g_lh_n++; } }
static uint64_t legend_obj(uint8_t *b) {
    uint64_t root = rd8((uint64_t)(uintptr_t)(b + 0x2f05458)), ch = root ? rd8(root + 0x58) : 0, n = root ? rd8(root + 0x60) : 0;
    for (uint64_t i = 0; ch && i < n && i < 256; i++) { uint64_t w = rd8(ch + 8 * i); const char *nm = w ? (const char *)(uintptr_t)rd8(w + 8) : NULL;
        if (nm && !strcmp(nm, "ControllerOverlay")) return *(uint8_t *)(uintptr_t)(w + 0x50) ? rd8(w + 0x440) : 0; }
    return 0;
}
static void legend_free(uint8_t *b, uint64_t *ent) {   // 条目里的串超过 15 字节时是原版分配器分配的：照原版调用点用完释放 | When the string in an entry exceeds 15 bytes it was allocated by the original allocator: free it after use as the original call sites do
    if (!(ent[4] >> 63) && (*(volatile uint8_t *)(b + 0x3211700) & 1)) { uint64_t al = rd8((uint64_t)(uintptr_t)(b + 0x32116f8)); ((void (*)(uint64_t, uint64_t))rd8(rd8(al) + 0x28))(al, ent[2]); }
}
static uint64_t legend_find(uint8_t *b, uint64_t lg, int act) {   // 提示条里这个动作号的那一条（按原版同一套比法：建一个空字条目取它的键帽，再比）；没有返回 0 | The entry for this action id in the button legend (compared the same way the original does: build an entry with empty text to get its keycap, then compare); returns 0 if there is none
    uint64_t ent[8] = { 0 }; struct { const char *p; uint64_t n; } sv = { "", 0 }; ((void (*)(void *, int, int, void *))(b + 0xaa9358))(ent, act, 0, &sv); legend_free(b, ent);
    if ((int32_t)ent[1] != 1) return 0;
    uint64_t arr = rd8(lg + 0x88), n = rd8(lg + 0x90); for (uint64_t i = 0; arr && i < n && i < 32; i++) { uint64_t e = arr + 0x40 * i; if (*(int32_t *)(uintptr_t)(e + 8) == 1 && *(uint32_t *)(uintptr_t)e == (uint32_t)ent[0]) return e; }
    return 0;
}
static int legend_ours(uint64_t e, const char *text) { size_t n = strlen(text); return rd8(e + 0x18) == n && !memcmp((const void *)(uintptr_t)rd8(e + 0x10), text, n); }
// 鼠标点在按键提示条的某一条上 = 按它那个键（Mac App：手柄版布局里底部的「ⓧ 默认　ⓑ 关闭菜单」是提示、不是按钮，鼠标点它没有任何响应）。
// 提示条控件（局内底部那条虚表 main+0x20b7960，前端各页的 main+0x20b7a30）自己画、没有子控件，读不到每一条的矩形：点落在控件里时按它的排法估——整排居中，每条 = 键帽 + 字，
// 字宽按字数算（全角 1 个字宽、半角约半个），条与条之间留空。量自 1600×900 截图：字宽约屏高的 2.8%，键帽连间隔约 1.3 个字宽，条间约 0.4–0.95 个字宽（取 0.67）。
// 返回要按的手柄键（DM_BTN_*，0 = 没点在提示条的条目上）。只许游戏主线程调
// A mouse click on one entry of the key-hint bar = pressing that key (Mac App: in the gamepad layout the bottom "X Default  B Close Menu" is a hint, not a button, and clicking it does nothing).
// The hint-bar widget (vtable main+0x20b7960 for the in-game bottom bar, main+0x20b7a30 for the front-end pages) draws itself and has no child widgets, so the rectangle of each entry cannot be read: when the click lands inside the widget, estimate by its layout - the whole row is centred, each entry = key cap + text,
// text width counted by characters (one glyph width for full-width, about half for half-width), with a gap between entries. Measured on 1600x900 screenshots: glyph width about 2.8% of screen height, key cap plus spacing about 1.3 glyph widths, gap between entries about 0.4–0.95 glyph widths (0.67 used).
// Returns the gamepad key to press (DM_BTN_*, 0 = the click is not on an entry of the hint bar). Call only from the game main thread
static uint64_t legend_at(uint8_t *b, uint64_t w, uint64_t xy, int dep) {
    if (!w || dep > 8 || !*(uint8_t *)(uintptr_t)(w + 0x50) || !*(uint8_t *)(uintptr_t)(w + 0x51)) return 0;
    uint64_t vt = rd8(w) - (uint64_t)(uintptr_t)b; if (vt == 0x20b7960 || vt == 0x20b7a30) return   /* 局内底部那条（会滚的）/ 前端各页自己的那条，条目数组同一个位置 | the in-game bottom bar (the scrolling one) / each front-end page's own bar: the entry array is at the same position */ rd8(w + 0x90) && (((int (*)(uint64_t, uint64_t))rd8(rd8(w) + 0x48))(w, xy) & 1) ? w : 0;
    uint64_t ch = rd8(w + 0x58), n = rd8(w + 0x60); for (uint64_t i = n < 256 ? n : 0; ch && i-- > 0; ) { uint64_t r = legend_at(b, rd8(ch + 8 * i), xy, dep + 1); if (r) return r; }
    return 0;
}
// 屏上最上面那条看得见的按键提示条里有没有某个键帽（如 ＋ = 0xe005）。Mac 键盘的回车据此决定当 ＋ 还是当确认（touch_mac.c）。只许游戏主线程调
// Whether the topmost visible key-hint bar on screen contains a given key cap (e.g. + = 0xe005). Return on a Mac keyboard uses this to decide between + and confirm (touch_mac.c). Call only from the game main thread
static uint64_t legend_top(uint8_t *b, uint64_t w, int dep) {
    if (!w || dep > 8 || !*(uint8_t *)(uintptr_t)(w + 0x50) || !*(uint8_t *)(uintptr_t)(w + 0x51)) return 0;
    uint64_t vt = rd8(w) - (uint64_t)(uintptr_t)b; if (vt == 0x20b7960 || vt == 0x20b7a30) return rd8(w + 0x88) && rd8(w + 0x90) && rd8(w + 0x90) <= 12 ? w : 0;
    uint64_t ch = rd8(w + 0x58), n = rd8(w + 0x60); for (uint64_t i = n < 256 ? n : 0; ch && i-- > 0; ) { uint64_t r = legend_top(b, rd8(ch + 8 * i), dep + 1); if (r) return r; }
    return 0;
}
int dm_padmode_legend_has(uint32_t glyph) {
    extern module_t g_mod[]; extern int g_nmod; uint8_t *b = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base;
    uint64_t root = b ? rd8((uint64_t)(uintptr_t)(b + 0x2f05458)) : 0, lg = root ? legend_top(b, root, 0) : 0; if (!lg) return 0;
    { const char *nm = (const char *)(uintptr_t)rd8(lg + 8); if (glyph == 0xe005 && nm && !strcmp(nm, "MainMenuLegend")) return 0; }   // 主菜单例外：它的提示条有「＋ 新角色」，但回车在主菜单要当确认（进入游戏）；新角色用 = 键或鼠标点那一条 | main menu exception: its hint bar has "+ New Character", but Return on the main menu must act as confirm (enter the game); a new character uses the = key or a mouse click on that entry
    uint64_t arr = rd8(lg + 0x88), n = rd8(lg + 0x90); for (uint64_t i = 0; i < n; i++) if (*(int32_t *)(uintptr_t)(arr + 0x40 * i + 8) == 1 && *(uint32_t *)(uintptr_t)(arr + 0x40 * i) == glyph) return 1;
    return 0;
}
uint64_t dm_padmode_legend_click(float nx, float ny) {
    extern module_t g_mod[]; extern int g_nmod; uint8_t *b = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base;
    if (!b || !g_cur) return 0; int W = *(int32_t *)(b + 0x211da38), H = *(int32_t *)(b + 0x211da3c); uint64_t root = rd8((uint64_t)(uintptr_t)(b + 0x2f05458)); if (!root || W <= 0 || H <= 0) return 0;
    int x = (int)(nx * W), y = (int)(ny * H); uint64_t lg = legend_at(b, root, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32), 0); if (!lg) return 0;
    uint64_t arr = rd8(lg + 0x88), n = rd8(lg + 0x90); if (!arr || n > 12) return 0;
    float em = H * 0.0281f, gap = 0.67f * em, wd[12], tot = 0;
    for (uint64_t i = 0; i < n; i++) { const uint8_t *t = (const uint8_t *)(uintptr_t)rd8(arr + 0x40 * i + 0x10); uint64_t tn = rd8(arr + 0x40 * i + 0x18); float tw = 0;
        for (uint64_t k = 0; t && k < tn && k < 96; k++) { if ((t[k] & 0xc0) == 0x80) continue; tw += t[k] < 0x80 ? (t[k] == ' ' ? 0.3f : 0.55f) : 1.0f; }
        wd[i] = (1.3f + tw) * em; tot += wd[i] + (i ? gap : 0); }
    float x0 = W / 2.0f - tot / 2; int hit = -1; for (uint64_t i = 0; i < n; i++) { if (x >= x0 - gap / 2 && x < x0 + wd[i] + gap / 2) hit = (int)i; x0 += wd[i] + gap; }
    if (hit < 0) return 0;
    uint64_t e = arr + 0x40 * (uint64_t)hit; uint32_t gl = *(uint32_t *)(uintptr_t)e; uint64_t btn = 0;
    static const struct { int act; uint64_t btn; } km[] = { { 0, DM_BTN_A }, { 1, DM_BTN_Y }, { 2, DM_BTN_X }, { 4, DM_BTN_RS }, { 7, DM_BTN_ZL }, { 8, DM_BTN_ZR }, { 10, DM_BTN_MINUS }, { 11, DM_BTN_LEFT }, { 14, DM_BTN_RIGHT }, { 12, DM_BTN_UP }, { 13, DM_BTN_DOWN }, { 19, DM_BTN_L }, { 20, DM_BTN_R }, { 23, DM_BTN_B } };
    if (*(int32_t *)(uintptr_t)(e + 8) == 1) for (unsigned k = 0; k < sizeof km / sizeof *km && !btn; k++) {   // 键帽 → 键：照 legend_find 的比法，拿动作号建一个空字条目取它的键帽来比 | key cap -> key: the same comparison as legend_find, build an entry with empty text from the action number and take its key cap to compare
        uint64_t ent[8] = { 0 }; struct { const char *p; uint64_t n; } sv = { "", 0 }; ((void (*)(void *, int, int, void *))(b + 0xaa9358))(ent, km[k].act, 0, &sv); legend_free(b, ent);
        if ((int32_t)ent[1] == 1 && (uint32_t)ent[0] == gl) btn = km[k].btn; }
    if (!btn && gl == 0xe008) btn = DM_BTN_LS;   // 按左摇杆（物品栏「打开魔盒」）同样没有动作号可比（实测 e008） | pressing the left stick (inventory "Open Cube") likewise has no action number to compare with (measured e008)
    if (!btn && gl == 0xe005) btn = DM_BTN_PLUS;   // ＋ 键没有对应的动作号可比（主菜单「＋ 新角色」）：直接认它的键帽码（实测 e005） | the + key has no action number to compare with (main menu "+ New Character"): recognise its key-cap code directly (measured e005)
    LOG("手柄提示条：鼠标点在第 %d 条（共 %llu 条，键帽 %x）→ %s", hit + 1, (unsigned long long)n, gl, btn ? "按它那个键" : "认不出是哪个键，不按");
    return btn;
}
// 鼠标点局内底部 HUD（Mac App）：手柄版 HUD 上的东西不是可点控件（手柄各有专键），鼠标点上去原先被当成「点世界」。按在哪个控件上就按它对应的键：
//   技能格 1–6 = A B Y X R ZR（屏上键帽的次序）；腰带四格 = 十字键 左 上 下 右（原版对应，同 dm_touch_controls.c）；中间的钮 = ＋（开功能页）；跑 / 走钮 = 按左摇杆。
//   血球、魔球、经验条、耐力条：没有对应的键，吃掉这一下（不许落到世界上去发令）。返回 0 = 没点在 HUD 上，1 = 按 *btn，2 = 吃掉不处理。只许游戏主线程调
// Mouse clicks on the in-game bottom HUD (Mac App): the things on the gamepad-layout HUD are not clickable widgets (the gamepad has a dedicated key for each), and a mouse click on them used to be treated as "click the world". Pressing on a widget presses its corresponding key:
//   skill slots 1–6 = A B Y X R ZR (the order of the key caps on screen); the four belt cells = D-pad left up down right (as in the original, same as dm_touch_controls.c); the middle button = + (open the function page); the run / walk button = press the left stick.
//   Life orb, mana orb, experience bar, stamina bar: no corresponding key, so the click is swallowed (it must not fall through to the world and issue an order). Returns 0 = not on the HUD, 1 = press *btn, 2 = swallowed without action. Call only from the game main thread
int dm_padmode_hud_click(float nx, float ny, uint64_t *btn) {
    extern module_t g_mod[]; extern int g_nmod; uint8_t *b = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base;
    if (!b || !g_cur) return 0; int W = *(int32_t *)(b + 0x211da38), H = *(int32_t *)(b + 0x211da3c); uint64_t root = rd8((uint64_t)(uintptr_t)(b + 0x2f05458)); if (!root || W <= 0 || H <= 0) return 0;
    if (*(volatile uint8_t *)(b + 0x2d5ff51) | *(volatile uint8_t *)(b + 0x2d5ff67)) return 0;   // 游戏停着（暂停菜单、大秘境选层页这类借壳的面板开着——手势层的面板名单认不到后者）：HUD 不认，免得点技能格变成在那一页上按 A | the game is stopped (pause menu, or a panel borrowing another shell such as the Greater Rift level-selection page is open - the gesture layer's panel list does not recognise the latter): the HUD is not recognised, so a click on a skill slot does not turn into pressing A on that page
    uint64_t hud = pm_find(root, "HUDPanel", 5); if (!hud || !*(uint8_t *)(uintptr_t)(hud + 0x50) || !*(uint8_t *)(uintptr_t)(hud + 0x51)) return 0;   // pm_find 的第三个参数是起始深度：只看根的直接子面板 | the third argument of pm_find is the starting depth: look only at the direct child panels of the root
    uint64_t xy = (uint64_t)(uint32_t)(nx * W) | ((uint64_t)(uint32_t)(ny * H) << 32), ch = rd8(hud + 0x58), n = rd8(hud + 0x60);
    static const struct { const char *nm; uint64_t btn; } km[] = { { "SkillButton1", DM_BTN_A }, { "SkillButton2", DM_BTN_B }, { "SkillButton3", DM_BTN_Y }, { "SkillButton4", DM_BTN_X }, { "SkillButton5", DM_BTN_R }, { "SkillButton6", DM_BTN_ZR },
        { "LevelUpButton", DM_BTN_PLUS }, { "RunButton", DM_BTN_LS }, { "RunButtonPrompt", DM_BTN_LS }, { "HealthBall", 0 }, { "ManaBall", 0 }, { "ExpBar", 0 }, { "StaminaBar", 0 }, { "SkillSwapIndicator", 0 }, { "LTButtonPrompt", 0 } };
    static const uint64_t belt[4] = { DM_BTN_LEFT, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_RIGHT };
    #define HUD_HIT(w, q) (((int (*)(uint64_t, uint64_t))rd8(rd8(w) + 0x48))((w), (q)) & 1)
    int px = (int)(nx * W), py = (int)(ny * H);
    for (int pass = 0; pass < 2; pass++)   // 先认有键的（跑 / 走钮被耐力条压着一部分），再认只吃掉的 | first recognise the ones with a key (the run / walk button is partly covered by the stamina bar), then the ones that are only swallowed
        for (uint64_t i = n < 64 ? n : 0; ch && i-- > 0; ) { uint64_t w = rd8(ch + 8 * i); if (!w || !*(uint8_t *)(uintptr_t)(w + 0x50)) continue; const char *nm = (const char *)(uintptr_t)rd8(w + 8); if (!nm) continue;
            if (!pass && !strcmp(nm, "Belt") && HUD_HIT(w, xy)) {   // 腰带是一个控件、四格等宽（子控件只是四个键帽字）：沿这一行量出它的左右边，按四等分取格 | the belt is one widget with four equal-width cells (the child widgets are only the four key-cap glyphs): measure its left and right edges along this row and pick the cell by quarters
                int l = px, r = px; while (l > 0 && HUD_HIT(w, (uint64_t)(uint32_t)(l - 2) | ((uint64_t)(uint32_t)py << 32))) l -= 2; while (r < W - 1 && HUD_HIT(w, (uint64_t)(uint32_t)(r + 2) | ((uint64_t)(uint32_t)py << 32))) r += 2;
                int k = r > l ? (px - l) * 4 / (r - l + 1) : 0; if (k < 0) k = 0; if (k > 3) k = 3; *btn = belt[k]; LOG("手柄 HUD：鼠标点在腰带第 %d 格（腰带 x %d–%d）→ 按十字键 %s", k + 1, l, r, k == 0 ? "LEFT" : k == 1 ? "UP" : k == 2 ? "DOWN" : "RIGHT"); return 1; }
            for (unsigned k = 0; k < sizeof km / sizeof *km; k++) if ((km[k].btn != 0) == !pass && !strcmp(nm, km[k].nm) && HUD_HIT(w, xy)) {
                *btn = km[k].btn; uint64_t kb = km[k].btn; const char *kn = kb == DM_BTN_A ? "A" : kb == DM_BTN_B ? "B" : kb == DM_BTN_Y ? "Y" : kb == DM_BTN_X ? "X" : kb == DM_BTN_R ? "R" : kb == DM_BTN_ZR ? "ZR" : kb == DM_BTN_PLUS ? "PLUS" : kb == DM_BTN_LS ? "LS" : "";
                if (kb) LOG("手柄 HUD：鼠标点在 %s → 按 %s", nm, kn); else LOG("手柄 HUD：鼠标点在 %s → 没有对应的键，这一下不处理", nm); return kb ? 1 : 2; } }
    #undef HUD_HIT
    return 0;
}
static void legend_pump(uint8_t *b, uint64_t frame) {   // 游戏主线程每帧 | Game main thread, every frame
    static int miss, tot; static uint64_t logged; int test = getenv("DM_TEST_LEGEND") != NULL;
    // 测试开关 DM_TEST_LEGEND=<动作号>:<字>[,…]：手柄模式下一直声明这些条目；DM_TEST_LEGEND_OFF=<帧>：到这一帧起不再声明。DM_TEST_LEGEND_LOG=1：每 120 帧记一行提示条现状（不声明条目）
    // Test switch DM_TEST_LEGEND=<action id>:<text>[,...]: keep declaring these entries in controller mode; DM_TEST_LEGEND_OFF=<frame>: stop declaring from
    // this frame on. DM_TEST_LEGEND_LOG=1: log one line with the button legend's current state every 120 frames (declares no entries)
    if (test) { static int n = -1; static struct { int a; char t[24]; } tv[LG_MAX]; if (n < 0) { n = 0; const char *e = getenv("DM_TEST_LEGEND"); for (const char *q = e; q && *q && n < LG_MAX; ) { int a; char t[24]; if (sscanf(q, "%d:%23[^,]", &a, t) != 2) break; tv[n].a = a; strlcpy(tv[n].t, t, sizeof tv[n].t); n++; q = strchr(q, ','); if (q) q++; } }
      static long off = -1; if (off < 0) { const char *e = getenv("DM_TEST_LEGEND_OFF"); off = e ? atol(e) : 0; }
      if (!off || frame < (uint64_t)off) for (int i = 0; i < n; i++) dm_padmode_legend(tv[i].a, tv[i].t); }
    if (g_cur && frame - g_lh_frame <= 2) for (int i = 0; i < g_lh_n; i++) { uint64_t w = pm_find(rd8((uint64_t)(uintptr_t)(b + 0x2f05458)), g_lh[i].legend, 0);
        if (w && legend_find(b, w, g_lh[i].act)) { ((void (*)(uint64_t, int))(b + 0xaaa610))(w, g_lh[i].act); static int lgd; if (lgd++ < 8) LOG("手柄提示条：%s 上原版的动作 %d 那一条拿掉（这一页把这个键收掉了）", g_lh[i].legend, g_lh[i].act); } }
    uint64_t lg = g_cur ? legend_obj(b) : 0; if (!lg) { g_lg_on_n = 0; return; }   // 提示条不在屏上（面板关了 / 回触屏模式）：原版自己会重建，记的清掉 | Button legend not on screen (panel closed / back to touch mode): the original rebuilds it by itself, so clear what we recorded
    int live = frame - g_lg_frame <= 2 ? g_lg_n : 0;   /* 页的声明与这里谁先谁后不定：两帧内声明过都算在屏上 | The order between a page's declaration and this code is not fixed: anything declared within the last two frames counts as on screen */
    for (int k = 0; k < g_lg_on_n; k++) { int keep = 0; for (int i = 0; i < live; i++) keep |= g_lg[i].act == g_lg_on[k].act;   // 上一轮加过、这一轮不再声明的：删 | Added last round and no longer declared this round: remove
        if (!keep) { uint64_t e = legend_find(b, lg, g_lg_on[k].act); if (e && legend_ours(e, g_lg_on[k].text)) ((void (*)(uint64_t, int))(b + 0xaaa610))(lg, g_lg_on[k].act); } }
    for (int i = 0; i < live; i++) { tot++; uint64_t e = legend_find(b, lg, g_lg[i].act); if (e && legend_ours(e, g_lg[i].text)) continue; miss++;
        uint64_t ent[8] = { 0 }; struct { const char *p; uint64_t n; } sv = { g_lg[i].text, strlen(g_lg[i].text) };
        ((void (*)(void *, int, int, void *))(b + 0xaa9358))(ent, g_lg[i].act, 0, &sv); ((void (*)(uint64_t, void *))(b + 0xaa9ef4))(lg, ent); legend_free(b, ent); }
    g_lg_on_n = live; memcpy(g_lg_on, g_lg, sizeof g_lg[0] * (size_t)live);
    if ((test || getenv("DM_TEST_LEGEND_LOG")) && frame - logged >= 120) { logged = frame; uint64_t arr = rd8(lg + 0x88), n = rd8(lg + 0x90); char o[300]; size_t L = 0; o[0] = 0;
        for (uint64_t i = 0; arr && i < n && i < 10; i++) L += (size_t)snprintf(o + L, L < sizeof o ? sizeof o - L : 0, " %x", *(uint32_t *)(uintptr_t)(arr + 0x40 * i));
        LOG("手柄提示条：帧 %llu，近 120 帧声明 %d 次、补加 %d 次；提示条现有 %llu 条，字形%s", (unsigned long long)frame, tot, miss, (unsigned long long)n, o); tot = miss = 0; }
}
static void diff_text(uint8_t *b, uint64_t frame) {   // 游戏主线程：菜单开着时每 6 帧核一次这行的字（只写我们自己的 TouchPadDiffTxt，文字控件虚表 main+0x20ba4c8，同 dm_pouch.c w_text） | Game main thread: while the menu is open, check this row's text every 6 frames (only writes our own TouchPadDiffTxt; text widget vtable main+0x20ba4c8, same as w_text in dm_pouch.c)
    if (!g_cur || !atomic_load(&g_pause_open) || frame % 6) return;
    uint64_t w = pm_find(rd8((uint64_t)(uintptr_t)(b + 0x2f05458)), "TouchPadDiffTxt", 0); if (!w || rd8(w) - (uint64_t)(uintptr_t)b != 0x20ba4c8) return;
    dm_hud_info h; dm_hud_get(&h); int n = atomic_load(&g_diff_pend); if (!n) n = h.players; if (!n) return;
    char t[64]; snprintf(t, sizeof t, dm_str(b, "dmPadDiffLine"), n); size_t L = strlen(t);
    if (rd8(w + 0x90) == L && !memcmp((const void *)(uintptr_t)rd8(w + 0x88), t, L)) return;
    ((void (*)(uint64_t, const char *, uint64_t))(b + 0x136e5c))(w + 0x88, t, L); ((void (*)(uint64_t))(b + 0xadc434))(w);
}
// 测试（触点脚本 padcur 步，没有实体手柄时造现场用）：把原版手柄光标直接放到视口归一化坐标处并让游戏当场重算悬停（dm_ui_hover_now）。
// 只是摆光标——摇杆推光标有加速、跟帧率走，按时长推落不准格子；之后的 A / Y 仍走手柄键那条路
// Test (touch script padcur step, for setting up a scenario without a physical controller): put the original controller cursor directly at viewport-normalized
// coordinates and have the game recompute hover on the spot (dm_ui_hover_now).
// This only positions the cursor: pushing the cursor with the stick has acceleration and follows the frame rate, so pushing for a set duration does not
// land on a cell reliably; the A / Y that follow still go through the controller button path
static _Atomic uint64_t g_test_cur;   // 位 63 = 有请求；x、y 各 16 位（×65535） | Bit 63 = request pending; x and y are 16 bits each (×65535)
void dm_padmode_test_cursor(float nx, float ny) { nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; ny = ny < 0 ? 0 : ny > 1 ? 1 : ny; atomic_store(&g_test_cur, 1ull << 63 | (uint64_t)(uint32_t)(nx * 65535 + .5f) << 16 | (uint32_t)(ny * 65535 + .5f)); }
void dm_padmode_step(uint8_t *b, uint64_t frame) {   // 游戏主线程每帧（dm_world.c） | every frame on the game main thread (dm_world.c)
    { static long at = -1; if (at < 0) { const char *e = getenv("DM_TEST_PADMODE_LANG_RESET"); at = e ? atol(e) : 0; }   // 测试开关 DM_TEST_PADMODE_LANG_RESET=<帧>：到帧当作串表要重装调一次复位（真切语言要走选项页；这里验「写回原文 → 重新认 → 重新换」） | Test switch DM_TEST_PADMODE_LANG_RESET=<frame>: at that frame call the reset once as if the string table were about to be reloaded (a real language switch has to go through the options page; this verifies "write back the original text -> recognize again -> swap again")
      if (at > 0 && frame >= (uint64_t)at) { at = 0; dm_padmode_lang_reset(); } }
    { uint64_t tc = atomic_exchange(&g_test_cur, 0); if (tc >> 63) { int dm_ui_hover_now(int, int); int W = *(int32_t *)(b + 0x211da38), H = *(int32_t *)(b + 0x211da3c), x = (int)(((tc >> 16) & 0xffff) / 65535.0 * W), y = (int)((tc & 0xffff) / 65535.0 * H);
        int ok = dm_ui_hover_now(x, y); LOG("测试 padcur：手柄光标放到 (%d,%d) → %d", x, y, ok); } }
    diff_text(b, frame);
    legend_pump(b, frame);
    if (g_have) hint_pump();
    void dm_oe_ui_opmode(int m); int dm_opmode(void);
    // 测试开关 DM_TEST_PADMODE=<帧>:<0|1>[,<帧>:<0|1>…]（宿主没有手柄 / 触屏，用它切模式）；每次切后与开机第 300 帧各用原版取串 0x7f5630 读前 3 条记一行
    // Test switch DM_TEST_PADMODE=<frame>:<0|1>[,<frame>:<0|1>...] (the host has no controller / touchscreen, so this switches modes); after each switch and
    // at frame 300 after boot, read the first 3 entries with the original string getter 0x7f5630 and log one line
    { static int n = -1, k; static struct { uint64_t f; int m; } ev[8]; static uint64_t chk = 300;
      if (n < 0) { n = 0; const char *e = getenv("DM_TEST_PADMODE"); for (const char *q = e; q && *q && n < 8; ) { unsigned long long f; int m; if (sscanf(q, "%llu:%d", &f, &m) != 2) break; ev[n].f = f; ev[n].m = m; n++; q = strchr(q, ','); if (q) q++; } }
      if (k < n && frame >= ev[k].f) { dm_padmode_request(ev[k].m); chk = frame + BOUNCE_FRAMES + 5; k++; }
      if (g_have && chk && frame >= chk && (n || getenv("DM_TEST_PADMODE_READ"))) { chk = 0; char o[512]; size_t L = 0; o[0] = 0;
          for (int i = 0; i < g_nstr && i < 3; i++) { struct { const char *s; uint64_t n; } q = { g_str[i].key, g_str[i].klen }; const char *r = ((const char *(*)(void *))(b + 0x7f5630))(&q);
              L += (size_t)snprintf(o + L, L < sizeof o ? sizeof o - L : 0, "%s%s=「%s」", i ? "、" : "", g_str[i].key, r ? r : "(空)"); }
          LOG("DM_TEST_PADMODE：帧 %llu 模式 %s，原版取串 %s", (unsigned long long)frame, g_cur ? "手柄" : "触屏", o); } }
#if TARGET_OS_OSX
    // 建角模式页「极乐世界」那行左边写死的手柄键帽（布局 touch_layout.py 的 TouchBlissKey，原位 x −1665）：Mac 的覆盖层同位带了一个键盘键帽 TouchBlissKeyK，按当前键帽对调
    // The hard-coded gamepad key cap to the left of the "Elysium" row on the character-creation mode page (TouchBlissKey in the layout touch_layout.py, original position x -1665): the Mac overlay carries a keyboard key cap TouchBlissKeyK at the same position, and the two are swapped according to the current key caps
    if (g_kbd_have && frame % 10 == 0) { uint64_t root = rd8((uint64_t)(uintptr_t)(b + 0x2f05458)), a = root ? pm_find(root, "TouchBlissKey", 0) : 0, k = a ? pm_find(root, "TouchBlissKeyK", 0) : 0;
        for (int i = 0; i < 2 && a && k; i++) { uint64_t w = i ? k : a; int32_t *r = (int32_t *)(uintptr_t)(w + 0x70), x = (i ? g_kbd : !g_kbd) ? -1665 : 99999; if (r[0] != x) { r[0] = x; ((void (*)(uint64_t))rd8(rd8(w) + 0x10))(w); } } }
    // 测试开关 DM_TEST_KBDCAPS=<帧>:<0|1>[,…]：到帧当作「实体手柄有输入（0）/ 键盘有输入（1）」（无人值守的局没有实体手柄）
    // Test switch DM_TEST_KBDCAPS=<frame>:<0|1>[,...]: at that frame act as if "a physical gamepad had input (0) / the keyboard had input (1)" (unattended runs have no physical gamepad)
    { static int n = -1, k; static struct { uint64_t f; int m; } ev[8];
      if (n < 0) { n = 0; const char *e = getenv("DM_TEST_KBDCAPS"); for (const char *q = e; q && *q && n < 8; ) { unsigned long long f; int m; if (sscanf(q, "%llu:%d", &f, &m) != 2) break; ev[n].f = f; ev[n].m = m; n++; q = strchr(q, ','); if (q) q++; } }
      if (k < n && frame >= ev[k].f) { LOG("DM_TEST_KBDCAPS：帧 %llu 请求键盘键帽 %d", (unsigned long long)frame, ev[k].m); dm_padmode_request_kbd(ev[k].m); k++; } }
#endif
    if (g_have && frame % 30 == 0) str_apply(g_cur);   // 串表开机后才装、切语言会重装：定期核 | The string table is loaded only after boot and reloaded on a language switch: check periodically
    if (!g_have) return;
    { uint32_t dm_oe_ui_opmode_reads(void); static uint64_t fp0; static uint32_t nfp; uint64_t root = rd8((uint64_t)(uintptr_t)(b + 0x2f05458)), ch = root ? rd8(root + 0x58) : 0, n = root ? rd8(root + 0x60) : 0, fp = n * 0x9e3779b97f4a7c15ull;
      for (uint64_t i = 0; ch && i < n && i < 256; i++) fp = (fp << 7 | fp >> 57) ^ rd8(ch + 8 * i);
      if (fp != fp0) { fp0 = fp; nfp++; } g_sig = dm_oe_ui_opmode_reads() + nfp; }
#if TARGET_OS_OSX
    int w = atomic_load(&g_want), wx = w, cx = g_cur;
    if (g_kbd_have) { int tp = w >= 0 ? w : g_cur, tk = tp ? atomic_load(&g_kbd_want) : 0; if (tk != g_kbd && tp == g_cur && !kbd_swap_ok(b)) tk = g_kbd;
        wx = tp | tk << 1; cx = g_cur | g_kbd << 1; if (wx == cx && w < 0) wx = -1; }   // 键帽那一位并进事务的「模式」里 | the key-cap bit is merged into the transaction's "mode"
    int act = dm_modetx_step(&g_tx, frame, wx, cx, g_sig);
    if (act == DM_TXA_BEGIN) { g_kbd_next = wx >> 1; w = wx & 1; }
#else
    int w = atomic_load(&g_want), act = dm_modetx_step(&g_tx, frame, w, g_cur, g_sig);
#endif
    if (act == DM_TXA_NONE) { if (g_tx.st == DM_TX_IDLE && atomic_load(&g_freeze)) { atomic_store(&g_freeze, 0); if (atomic_load(&g_want) != w) atomic_store(&g_freeze, 1); } return; }   // 没在切：请求时立的冻结（想要的就是当前模式 / 又改回来了）撤掉；撤的同时又来了新请求的重新立上 | Not switching: the freeze set at request time (the wanted mode is the current one / it was changed back) is lifted; if a new request arrives at the same time as the lift, it is set again
    if (act == DM_TXA_BEGIN) {
        if (!pfx_write(b, w)) { g_tx.st = DM_TX_IDLE; atomic_store(&g_want, -1); atomic_store(&g_freeze, 0); return; }
        atomic_store(&g_freeze, 1); g_tx_from = frame;
        items_run(g_cur, 1);   // 旧一侧各登记项收手（dm_mode_items.h）：在途的替按键作废、挪过的控件写回、缓存的控件地址清掉 | Registered items of the old side stand down (dm_mode_items.h): in-flight key presses on the player's behalf are voided, moved widgets written back, cached widget addresses cleared
        g_cur = w; str_apply(w); dm_oe_ui_opmode(!dm_opmode());
#if TARGET_OS_OSX
        g_kbd = g_kbd_next; if (g_kbd_have) LOG("手柄 / 触屏布局：键盘键帽 → %d", g_kbd);
#endif
        LOG("手柄 / 触屏布局：切到%s版——开始（输入冻结，让游戏重载布局）", w ? "原版手柄" : "触屏"); return; }
    if (g_tx.timeouts) LOG("【红】手柄 / 触屏布局：%d 帧内没回读到界面重建（游戏没来读运行模式、界面根下的面板也没换），按超时往下走", DM_MODETX_TIMEOUT);
    if (act == DM_TXA_REPORT_BACK) { dm_oe_ui_opmode(-1); return; }
    atomic_fetch_add(&g_rebuilds, 1); items_run(g_cur, 0);   // 新一侧各登记项就位 | Registered items of the new side take their places
    if (atomic_load(&g_want) == g_cur) atomic_store(&g_want, -1);
    atomic_store(&g_freeze, 0);   // 放开输入：冻结期间按着过的键先松开过一次才交给游戏（dm_modegate）；事务中又来了反向请求的，下一帧接着起下一次（请求时已重新立冻结） | Release input: keys held during the freeze must be let go once before being handed to the game (dm_modegate); if an opposite request arrived during the transaction, the next switch starts on the next frame (the freeze was already set again at request time)
    if (atomic_load(&g_want) >= 0 && atomic_load(&g_want) != g_cur) atomic_store(&g_freeze, 1);
    LOG("手柄 / 触屏布局：切到%s版——完成（%llu 帧，两次重建里 %d 次靠超时）", g_cur ? "原版手柄" : "触屏", (unsigned long long)(frame - g_tx_from), g_tx.timeouts);
    if (g_cur) {   // 切到手柄版时记一行六个技能格各绑的技能号（第二组 = 按着 ZL）：面 → 格表 main+0x2d13cbc（6 × {逻辑键, 普通格, 替代格}）、格 → 技能表 main+0x2d5f0c0（每格 28 字节，武器第二组在 +16 格），读法同 dm_freetp.c | On switching to the controller layout, log one line with the skill number bound to each of the six skill slots (second set = ZL held): face button -> slot table main+0x2d13cbc (6 x {logical key, normal slot, alternate slot}), slot -> skill table main+0x2d5f0c0 (28 bytes per slot, the second weapon set is at +16 slots), read the same way as dm_freetp.c
        int dm_swapskill_wset(void); int bank = dm_swapskill_wset() ? 16 : 0; char o[240]; size_t L = 0; o[0] = 0;
        static const struct { uint32_t lg; const char *nm; } face[6] = { { 0x2000, "A" }, { 0x1000, "B" }, { 0x8000, "X" }, { 0x4000, "Y" }, { 0x200, "R" }, { 0x800, "ZR" } };
        for (int f = 0; f < 6; f++) for (int i = 0; i < 6; i++) { uint32_t lg, sl[2], id[2] = { 0xffffffffu, 0xffffffffu }; memcpy(&lg, b + 0x2d13cbc + 12 * i, 4); memcpy(sl, b + 0x2d13cbc + 12 * i + 4, 8); if (lg != face[f].lg) continue;
            for (int k = 0; k < 2; k++) if (sl[k] < 12) memcpy(&id[k], b + 0x2d5f0c0 + (uint64_t)(bank + sl[k]) * 28, 4);
            L += (size_t)snprintf(o + L, L < sizeof o ? sizeof o - L : 0, " %s=%d/%d", face[f].nm, (int)id[0], (int)id[1]); }
        LOG("手柄 / 触屏布局：技能格（技能号，/ 后是按着 ZL 的第二组，-1 = 空）%s", o); }
}
