// 手柄接入（原版 Switch 本来就是手柄操作，恢复手柄支持；复用原版手柄交互，不自己设计）。
// iOS GameController（MFi / Xbox / PS / Switch Pro）的扩展手柄 → 原版手柄输入（宿主 dm_pad_set，与触屏输入在游戏取手柄时合并）。
// 四个面键按键帽上印的字母对应：提示条写哪个字母，就按手柄上印着那个字母的键（提示条画的是 Switch 键帽，不跟手柄型号换图）。
//   系统给的 buttonA/B/X/Y 直接发游戏的 A/B/X/Y，不认手柄类型：系统对任天堂手柄按键帽字母给（印 A 的右键 = buttonA），Xbox 字母与位置一致，
//   没字母的（PS 等）按菱形位置给（下方键 = buttonA = 确认）。规则在 host/dm_padmap.h（纯函数，宿主单测 t_padmap 核）；连接时记一行厂商名与产品类别。
//   （先前一律按 Switch 的位置排——右键确认——Xbox 手柄玩家照提示条「A 选择」按印着 A 的下方键，实际发的是 B，选角页选不了人。）
//   （之后按 GCExtendedGamepad.h 的菱形约定认类型再翻译：任天堂手柄被换了两次，A/B、X/Y 全反——公开 日志。）玩家在系统设置里改过键位的，系统已先换好再给我们。
//   肩键 L/R、扳机 ZL/ZR、摇杆按下 LS/RS、十字键、菜单键→＋、选项键→－。
//   选项键（buttonOptions）有的手柄没有，那时减号没有来源：不拿 Home 键顶（头文件：系统不吃才轮到 App，靠不住），留空。Xbox 的视图键、PS 的分享 / 创建键、Switch Pro 的 − 都是选项键。
// 摇杆照原值换成 ±32767（GameController 与原版都是上为正）。
//
// 谁接管（谁有明确输入谁接管，不因漂移来回切）：
//   · 手柄「明确输入」= 按下任何一颗键，或把摇杆从中位推出来。摇杆带迟滞：推过 25% 才算推出，回到 15% 以内才算回中——停在阈值附近的漂移不会一会儿算一会儿不算。
//   · 手柄有明确输入 → 手柄模式（布局换原版手柄版）；碰屏幕 → 触屏模式（前端调 dm_gamepad_touched）。
//   · 碰过屏幕之后，摇杆要先回中、再推出来才重新接管：一直偏着的摇杆（漂移大的手柄）不会把布局又切回手柄版。按键随时接管。
//   · 触屏模式下手柄摇杆的值不交给游戏（手柄连着放在一边、摇杆有点漂移，不该盖掉触屏摇杆）；手柄模式下没推出来的摇杆给 0、推出来的给原值。
//   · 游戏取手柄处（hle_hid.c fill）：触屏摇杆有值就用触屏的，否则用手柄的。
// 多个手柄：每个手柄各存各的键和摇杆，交给游戏的是合起来的——键取或、每根摇杆取推得最远的那个。断开一个只拿掉它那一份，别的手柄按着的键不丢。
// 失活 / 进后台 / 来电（前端 front_ios_focus_event → dm_gamepad_reset）：所有手柄的键和摇杆清零。回调只在值变化时来，按着键切走、在后台松开，回来不会补一次「松开」。
// Controller support (the original Switch game is played with a controller, so controller support is restored, reusing the original controller interaction rather than designing our own).
// iOS GameController extended gamepads (MFi / Xbox / PS / Switch Pro) → original controller input (host dm_pad_set, merged with touch input when the game reads the controller).
// The four face buttons follow the letters printed on the keycaps: whatever letter the prompt shows, press the button printed with that letter (the prompt draws Switch keycaps and does not change with the controller model).
//   The system's buttonA/B/X/Y are sent directly as the game's A/B/X/Y regardless of controller type: the system reports Nintendo controllers by keycap letter (the right button printed A = buttonA), Xbox letters match positions,
//   and controllers without letters (PS etc.) by diamond position (bottom button = buttonA = confirm). The rule lives in host/dm_padmap.h (pure function, checked by the host unit test t_padmap); the vendor name and product category are logged on connect.
//   (Previously everything followed the Switch layout — right button confirms — so Xbox players pressing the bottom button printed A per the prompt "A Select" actually sent B and could not pick a character.)
//   (Later the type was recognized and translated per the diamond convention in GCExtendedGamepad.h: Nintendo controllers were swapped twice and A/B, X/Y were all reversed — a public log.) If the player remapped buttons in system settings, the system has already applied that before reporting.
//   Shoulders L/R, triggers ZL/ZR, stick presses LS/RS, D-pad, menu button → +, options button → −.
//   Some controllers have no options button (buttonOptions); then minus has no source: the Home button is not used as a substitute (per the header the app only gets it when the system does not take it, which is unreliable), so it stays empty. Xbox's View, PS's Share / Create and Switch Pro's − are all the options button.
// Stick values are scaled to ±32767 unchanged (GameController and the original both use up = positive).
// Who takes over (whoever gives explicit input takes over; drift does not toggle it back and forth):
//   · Explicit controller input = pressing any button, or pushing a stick out of the center. The stick has hysteresis: it counts as pushed past 25% and as centered again within 15% — drift around the threshold does not flicker.
//   · Explicit controller input → controller mode (layout switches to the original controller version); touching the screen → touch mode (the front end calls dm_gamepad_touched).
//   · After the screen has been touched, a stick must return to center and be pushed out again to take over: a stick that stays off center (a controller with large drift) does not switch the layout back. Buttons take over at any time.
//   · In touch mode the controller stick values are not passed to the game (a connected controller lying aside with a slightly drifting stick must not override the touch stick); in controller mode an unpushed stick gives 0 and a pushed one its value.
//   · Where the game reads the controller (hle_hid.c fill): the touch stick is used when it has a value, otherwise the controller's.
// Multiple controllers: each keeps its own buttons and sticks, and the game gets the combination — buttons ORed, each stick taking whichever is pushed farthest. Disconnecting one removes only its share; buttons held on other controllers are not dropped.
// Resign active / background / incoming call (front end front_ios_focus_event → dm_gamepad_reset): all controllers' buttons and sticks are cleared. Callbacks arrive only on value changes, so a button held while switching away and released in the background would never get a release when coming back.
#import <GameController/GameController.h>
#include "dm_input.h"
#include "dm_padmap.h"
#include "hle.h"

static void (*g_on_mode)(int pad);
static int g_pad_mode, g_need_center;
void dm_padmode_request(int pad);   // 宿主 dm_padmode.c：布局换成原版手柄版 / 触屏版 | Host dm_padmode.c: switch the layouts to the original controller version / the touch version
static void set_mode(int pad) { if (g_pad_mode == pad) return; g_pad_mode = pad; dm_padmode_request(pad); LOG("手柄：%s", pad ? "手柄有输入 → 手柄模式" : "碰了屏幕 / 手柄断开 → 触屏模式"); if (g_on_mode) g_on_mode(pad); }
int dm_gamepad_mode(void) { return g_pad_mode; }

// 各手柄各存各的（槽 0、1 留给触点脚本的两个虚拟手柄，其余给实体手柄）
// Each controller is stored separately (slots 0 and 1 are reserved for the touch script's two virtual controllers, the rest are for physical controllers)
enum { NSLOT = 6 };
static struct { const void *who; uint64_t b; int32_t ax[4]; } g_s[NSLOT];
static int slot_of(const void *who, int make) { int fr = -1; for (int i = 0; i < NSLOT; i++) { if (g_s[i].who == who) return i; if (fr < 0 && !g_s[i].who) fr = i; }
    if (make && fr >= 0) { memset(&g_s[fr], 0, sizeof g_s[fr]); g_s[fr].who = who; return fr; } return -1; }

static int32_t axis(float v) { if (v > 1) v = 1; if (v < -1) v = -1; return (int32_t)lroundf(v * DM_STICK_MAX); }
static int stick_on(int was, int32_t x, int32_t y) { double m = hypot(x, y) / DM_STICK_MAX; return was ? m >= 0.15 : m >= 0.25; }   // 迟滞：推过 25% 算推出，回到 15% 内算回中 | Hysteresis: past 25% counts as pushed out, back within 15% counts as centered
// 把各手柄合起来、定模式、交给宿主。任何一份变了都重算一遍
// Merge all controllers, decide the mode and hand the result to the host. Recompute whenever any one of them changes
static void apply(void) {
    uint64_t b = 0; int32_t ax[4] = {0}; double best[2] = {0};
    for (int i = 0; i < NSLOT; i++) { if (!g_s[i].who) continue; b |= g_s[i].b;
        for (int k = 0; k < 2; k++) { double m = hypot(g_s[i].ax[2 * k], g_s[i].ax[2 * k + 1]); if (m > best[k]) { best[k] = m; ax[2 * k] = g_s[i].ax[2 * k]; ax[2 * k + 1] = g_s[i].ax[2 * k + 1]; } } }
    static int on[2]; static uint64_t prev_b;
    on[0] = stick_on(on[0], ax[0], ax[1]); on[1] = stick_on(on[1], ax[2], ax[3]);
    if (!on[0] && !on[1]) g_need_center = 0;   // 碰过屏幕后摇杆回过中了：之后再推出来算数 | The stick has returned to center since the screen was touched: pushing it out again from now on counts
    if ((b & ~prev_b) || ((on[0] || on[1]) && !g_need_center)) { set_mode(1);
#if TARGET_OS_OSX
        { void dm_padmode_request_kbd(int kbd); dm_padmode_request_kbd(0); }   // Mac App：实体手柄有输入，键位图标换回手柄键帽 | Mac App: a physical gamepad had input, switch the key icons back to gamepad key caps
#endif
    }
    prev_b = b;
    if (!g_pad_mode || !on[0]) ax[0] = ax[1] = 0; if (!g_pad_mode || !on[1]) ax[2] = ax[3] = 0;   // 触屏模式不交摇杆；手柄模式下没推出来的给 0 | Sticks are not delivered in touch mode; in controller mode a stick that is not pushed out gives 0
    // ZR：原版六个技能格之一。这一格空着（没绑技能）时按它 = 免费回城（手柄玩家也免费回城，与触屏回城钮一致）；绑了技能就整颗交还原版（不再整颗占用）。
    // 想把回城放在别的键上：原版选技能界面「综合技能」里有回城技能可绑（宿主进局时挂上的，dm_freetp.c），那条走原版自己的施放，不经这里。
    // 只在游戏态接管：面板开着时 ZR 照常交给原版（储藏箱 / 编年史用 ZL / ZR 翻页签）；按下时在游戏态接管的那一下，到松开前都不交给游戏
    // ZR: one of the six original skill slots. While this slot is empty (no skill bound), pressing it = free town portal (controller players get
    // the free town portal too, consistent with the touch town portal button); once a skill is bound the whole button is handed back to the
    // original game (we no longer claim the whole button).
    // To put town portal on another key: the original skill selection screen has a town portal skill under "General Skills" that can be bound
    // (attached by the host on joining a run, dm_freetp.c); that one goes through the original game's own cast and does not pass through here.
    // Taken over only in game state: with a panel open ZR goes to the original game as usual (the stash / Chronicle use ZL / ZR to switch tabs);
    // a press that was taken over in game state is not handed to the game until it is released
    { static uint64_t prev; static int mine; void dm_tp_request(void); int dm_front_navigation(void), dm_freetp_zr_free(int zl);
      if ((b & DM_BTN_ZR) && !(prev & DM_BTN_ZR)) { mine = !dm_front_navigation() && dm_freetp_zr_free((b & DM_BTN_ZL) != 0); if (mine) { dm_tp_request(); LOG("手柄：ZR（这一格空着）→ 免费回城"); } }
      prev = b; if (mine) b &= ~(uint64_t)DM_BTN_ZR; if (!(b & DM_BTN_ZR) && !(prev & DM_BTN_ZR)) mine = 0; }
    dm_pad_set(b, ax[0], ax[1], ax[2], ax[3]);
}
static void put(const void *who, uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    int i = slot_of(who, 1); if (i < 0) return; g_s[i].b = b; g_s[i].ax[0] = lx; g_s[i].ax[1] = ly; g_s[i].ax[2] = rx; g_s[i].ax[3] = ry; apply();
}
static void drop(const void *who) {   // 这个手柄没了：只拿掉它那一份；一个手柄都不剩才回触屏模式 | This controller is gone: remove only its share; return to touch mode only when no controller is left
    int i = slot_of(who, 0); if (i < 0) return; memset(&g_s[i], 0, sizeof g_s[i]); apply();
    for (int k = 0; k < NSLOT; k++) if (g_s[k].who) return;
    set_mode(0);
}
void dm_gamepad_touched(void) { if (g_pad_mode) { g_need_center = 1; set_mode(0); apply(); } }   // 前端：碰了屏幕。摇杆此刻偏着的话要先回中才重新接管 | Front end: the screen was touched. If a stick is deflected right now it must return to center before taking over again
void dm_gamepad_reset(void) {   // 前端：失活 / 进后台 / 来电 | Front end: resign active / enter background / incoming call
    int any = 0; for (int i = 0; i < NSLOT; i++) if (g_s[i].who) { any |= g_s[i].b != 0 || g_s[i].ax[0] || g_s[i].ax[1] || g_s[i].ax[2] || g_s[i].ax[3]; g_s[i].b = 0; memset(g_s[i].ax, 0, sizeof g_s[i].ax); }
    if (any) { LOG("手柄：失活，手柄的键和摇杆清零"); apply(); }
}
// 测试（触点脚本 pad / pad2 / padoff / padoff2 步）：没有实体手柄时走同一条路，只少了 GameController 回调那一层。n = 1 或 2：两个虚拟手柄
// Test (touch script pad / pad2 / padoff / padoff2 steps): without a physical controller the same path is taken, minus only the GameController callback
// layer. n = 1 or 2: two virtual controllers
void dm_gamepad_inject_n(int n, uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { put((const void *)(intptr_t)(n == 2 ? 2 : 1), b, lx, ly, rx, ry); }
void dm_gamepad_drop_n(int n) { LOG("手柄：断开（虚拟手柄 %d）", n == 2 ? 2 : 1); drop((const void *)(intptr_t)(n == 2 ? 2 : 1)); }
void dm_gamepad_inject(uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { dm_gamepad_inject_n(1, b, lx, ly, rx, ry); }
static void publish(GCController *c, GCExtendedGamepad *g) {
    uint64_t b = 0;
    b |= dm_padmap_face(g.buttonA.pressed, g.buttonB.pressed, g.buttonX.pressed, g.buttonY.pressed);   // 系统给哪个字母发哪个字母（dm_padmap.h） | Send whichever letter the system reports (dm_padmap.h)
    if (g.leftShoulder.pressed) b |= DM_BTN_L; if (g.rightShoulder.pressed) b |= DM_BTN_R; if (g.leftTrigger.pressed) b |= DM_BTN_ZL; if (g.rightTrigger.pressed) b |= DM_BTN_ZR;
    if (g.leftThumbstickButton.pressed) b |= DM_BTN_LS; if (g.rightThumbstickButton.pressed) b |= DM_BTN_RS;
    if (g.buttonMenu.pressed) b |= DM_BTN_PLUS; if (g.buttonOptions.pressed) b |= DM_BTN_MINUS;
    if (g.dpad.left.pressed) b |= DM_BTN_LEFT; if (g.dpad.up.pressed) b |= DM_BTN_UP; if (g.dpad.right.pressed) b |= DM_BTN_RIGHT; if (g.dpad.down.pressed) b |= DM_BTN_DOWN;
    put((__bridge const void *)c, b, axis(g.leftThumbstick.xAxis.value), axis(g.leftThumbstick.yAxis.value), axis(g.rightThumbstick.xAxis.value), axis(g.rightThumbstick.yAxis.value));
}
static void attach(GCController *c) {
    GCExtendedGamepad *g = c.extendedGamepad; if (!g) { LOG("手柄：%s 不是扩展手柄，不接", c.vendorName.UTF8String ?: "?"); return; }
    LOG("手柄：连上 %s，类别 %s → 面键 buttonA/B/X/Y 直接发 A/B/X/Y", c.vendorName.UTF8String ?: "?", c.productCategory.UTF8String ?: "?");
    __weak GCController *wc = c;
    g.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el) { (void)el; GCController *sc = wc; if (sc) publish(sc, gp); };
}
void dm_gamepad_start(void (*on_mode)(int pad)) {
    g_on_mode = on_mode;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *n) { attach(n.object); }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *n) {
        LOG("手柄：断开 %s", ((GCController *)n.object).vendorName.UTF8String ?: "?"); drop((__bridge const void *)n.object); }];
    for (GCController *c in GCController.controllers) attach(c);
}
