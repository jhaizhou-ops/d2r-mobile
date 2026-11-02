// 安卓手柄接入（iOS dm/ios/Sources/dm_gamepad.m 的移植：合并 / 模式 / 键帽 / ZR 免费回城那套核心逻辑逐条照搬，平台层换成安卓的事件模型）。
// 输入来源：按键走 KeyEvent（DmActivity.dispatchKeyEvent），摇杆 / 十字键帽 / 扳机轴走 onGenericMotionEvent；Java 侧按设备合并成
//   「整只手柄的现值」（电平不是边沿，与 iOS GameController 的 valueChangedHandler 同一个模型），有变化才经 nativePad 送来。
//   插拔由 InputManager 监听（连接记一行日志，断开 nativePadGone 只拿掉它那份输入）。
// 面键：安卓（内核按 Linux 手柄规范报 BTN_SOUTH/EAST/NORTH/WEST，系统键表再映射）一律按位置给——下 = BUTTON_A、右 = BUTTON_B、
//   左 = BUTTON_X、上 = BUTTON_Y，与键帽印什么字母无关。这与苹果不同（苹果对任天堂手柄按字母报，依据见 dm_padmap.h），所以这里的
//   面键表不用 dm_padmap_face_xbox（那是苹果的字母 / 位置混合语义），按位置自己排：
//   任天堂手柄（vendor 0x057e）：游戏按 Switch 键位认字母（右 A、下 B、上 X、左 Y）→ 位置对字母各换一次 → face(b, a, y, x)，Switch 键帽；
//   Xbox / PlayStation / 其余：与 PC / Xbox 版同位置同功能（下 A 确认、右 B 返回、上 X、左 Y）→ face(a, b, y, x)，Xbox / PS 键帽
//   （其余手柄用原版 Switch 键帽）。覆盖层带没带那两套键帽由标记文件决定（dm_padmode.c 开机查），没带就退原版键帽。
// 谁接管（与 iOS 同一套）：手柄「明确输入」（按下任何键，或把摇杆从中位推出来——迟滞 25% 推出 / 15% 回中）→ 手柄模式（布局换原版
//   手柄版，触屏控件层隐藏）；碰屏幕 → 触屏模式（touch_android.c 在取手柄处调 dm_gamepad_touched，那一下整根吞掉）；碰过屏幕后摇杆
//   要先回中再推出来才重新接管；触屏模式不交摇杆值，手柄模式下没推出来的摇杆给 0。多个手柄各存各的，键取或、摇杆取推得最远的，
//   断开只拿掉它那份。失焦 / 后台（jni_android.c nativeFocus(0) → dm_gamepad_reset）键和摇杆清零，回来不补「松开」。
//   ZR 空格 = 免费回城：只在游戏态接管（dm_front_navigation），绑了技能整颗交还原版（dm_freetp.c），与 iOS 一致。
// 线程：nativePad 在 UI 线程来，测试开关在游戏线程的取手柄 tick 里泵——合并状态用一把小互斥锁护住（锁内调的宿主接口
//   dm_pad_set / dm_padmode_request 本就允许任意线程）。set_mode 回调（touch 层藏控件）拿 touch 层的锁：touch 层调
//   dm_gamepad_touched 一律在放掉自己的锁之后（touch_android.c），两个方向不会互等。
// 测试开关 DM_TEST_PAD=<毫秒>:<按键位>:<lx>:<ly>:<rx>:<ry>[,…]（files/dm/env；按键位与摇杆是 dm_input.h 的游戏值，同 iOS 触点脚本的
//   pad 步）：没有实体手柄时把一只虚拟手柄按时刻表推进同一条路。它与 adb `input keyevent`（按键，虚拟设备同样收）合起来，不接手柄
//   就能全链路验证。
// Android controller input (a port of iOS dm/ios/Sources/dm_gamepad.m: the merge / mode / glyph / free-town-portal-ZR core is carried over item by
// item, with the platform layer replaced by Android's event model).
// Input sources: buttons arrive as KeyEvents (DmActivity.dispatchKeyEvent), sticks / hat / trigger axes via onGenericMotionEvent; the Java side merges
// them per device into "the whole controller's current values" (levels, not edges -- the same model as iOS GameController's valueChangedHandler) and
// hands them over through nativePad only on change. Connects and disconnects come from the InputManager listener (a connect logs one line; a
// disconnect's nativePadGone removes only that controller's share).
// Face buttons: Android (the kernel reports BTN_SOUTH/EAST/NORTH/WEST per the Linux gamepad convention and the system keylayout maps them) always
// reports by position -- bottom = BUTTON_A, right = BUTTON_B, left = BUTTON_X, top = BUTTON_Y -- regardless of the letters on the keycaps. This differs
// from Apple (where Nintendo controllers report by letter; evidence in dm_padmap.h), so the face table here does not use dm_padmap_face_xbox (Apple's
// mixed letter / positional semantics) and is laid out by position instead:
//   Nintendo controllers (vendor 0x057e): the game takes Switch letters by position (right A, bottom B, top X, left Y) -> each swapped once against
//     position -> face(b, a, y, x), Switch glyphs;
//   Xbox / PlayStation / the rest: same position = same function as the PC / Xbox version (bottom A confirm, right B back, top X, left Y) ->
//     face(a, b, y, x), Xbox / PS glyphs (other controllers keep the original Switch glyphs). Whether the overlay carries those two glyph sets is
//     decided by its marker files (checked at boot in dm_padmode.c); without them the original glyphs are used.
// Who takes over (as on iOS): a controller's "clear input" (any button pressed, or a stick pushed out of centre -- 25% out / 15% back hysteresis) ->
// controller mode (layouts switch to the original controller version, the touch control layer hides); touching the screen -> touch mode
// (touch_android.c calls dm_gamepad_touched at the controller read and swallows that whole touch); after a touch the stick must return to centre
// before it can take over again; in touch mode stick values are not delivered, and in controller mode a stick not pushed out gives 0. Multiple
// controllers are stored separately (buttons ORed, each stick takes the furthest push), and a disconnect removes only that one's share. Focus loss /
// background (jni_android.c nativeFocus(0) -> dm_gamepad_reset) zeroes buttons and sticks; no "release" is replayed on return. An empty ZR slot = the
// free town portal: taken over only in the gameplay state (dm_front_navigation); once a skill is bound the whole button goes back to the original
// game (dm_freetp.c), as on iOS.
// Threads: nativePad arrives on the UI thread, the test switch is pumped on the game thread's controller-read tick -- the merge state is guarded by
// one small mutex (the host interfaces called under it, dm_pad_set / dm_padmode_request, already accept any thread). set_mode's callback (the touch
// layer hiding its controls) takes the touch layer's lock: the touch layer always calls dm_gamepad_touched after dropping its own lock
// (touch_android.c), so the two directions never wait on each other.
// Test switch DM_TEST_PAD=<ms>:<button bits>:<lx>:<ly>:<rx>:<ry>[,...] (files/dm/env; the button bits and sticks are the game values of dm_input.h,
// as in the pad step of the iOS touch scripts): without a physical controller a virtual one is advanced through the same path on a schedule. Together
// with adb `input keyevent` (buttons; virtual devices are accepted too) it verifies the whole chain without a controller.
#include "touch_android.h"
#include "../host/dm.h"
#include "../host/dm_input.h"
#include "../host/dm_padmap.h"
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Java 侧送来的「设备类型」（DmActivity.java 按 vendor id 与名字认）：面键键位与键帽样式见文件头
// The "device kind" handed over by the Java side (DmActivity.java identifies it by vendor id and name): face layout and glyphs, see the file header
enum { DM_PAD_KIND_OTHER = 0, DM_PAD_KIND_NINTENDO, DM_PAD_KIND_XBOX, DM_PAD_KIND_PS };
// Java 侧的通用手柄键位（DmActivity.java 用同一组数字）：安卓 KEYCODE / 轴 → 这里 → 游戏 DM_BTN_*。位置语义见文件头
// The generic controller bits from the Java side (the same numbers in DmActivity.java): Android KEYCODEs / axes -> here -> the game's DM_BTN_*.
enum {
    GPB_A = 1 << 0, GPB_B = 1 << 1, GPB_X = 1 << 2, GPB_Y = 1 << 3,
    GPB_L1 = 1 << 4, GPB_R1 = 1 << 5, GPB_L2 = 1 << 6, GPB_R2 = 1 << 7,
    GPB_LS = 1 << 8, GPB_RS = 1 << 9, GPB_START = 1 << 10, GPB_BACK = 1 << 11,
    GPB_DL = 1 << 12, GPB_DU = 1 << 13, GPB_DR = 1 << 14, GPB_DD = 1 << 15,
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int g_pad_mode, g_need_center;
void dm_padmode_request(int pad);        // 宿主 dm_padmode.c：布局换成原版手柄版 / 触屏版 | Host dm_padmode.c: switch the layouts
void dm_tp_request(void);                // 宿主 dm_freetp.c：请求免费回城 | Host dm_freetp.c: request the free town portal
int dm_freetp_zr_free(int zl);           // 宿主 dm_freetp.c：ZR 这一格现在空着吗 | Host dm_freetp.c: is the ZR slot empty right now
static void set_mode(int pad) { if (g_pad_mode == pad) return; g_pad_mode = pad; dm_padmode_request(pad); LOG("手柄：%s", pad ? "手柄有输入 → 手柄模式" : "碰了屏幕 / 手柄断开 → 触屏模式"); dm_android_touch_pad_mode(pad); }

// 面板 / 读取画面 / 前端页开着（touch_android.c 的原子量，同 iOS front_ios.m 的 g_nav_pub）：dm_gamepad.m 据此只在游戏态接管 ZR
// A panel / loading screen / front-end page is open (touch_android.c's atomic, as iOS front_ios.m's g_nav_pub): as in dm_gamepad.m, ZR is taken over only in the gameplay state
int dm_front_navigation(void) { return dm_android_touch_navigation(); }

// 各手柄各存各的（槽 1、2 留给测试开关的虚拟手柄，其余给实体；who = deviceId + 4，把注入用的 −1 也挪开）
// Each controller is stored separately (slots 1 and 2 are reserved for the test switch's virtual controller, the rest for physical ones; who = deviceId + 4, which also moves the injection id -1 aside)
enum { NSLOT = 6 };
static struct { const void *who; uint64_t b; int32_t ax[4]; int glyph; } g_s[NSLOT];
static int g_glyph = DM_GLYPH_PAD;   // 最近一次明确输入来自哪种手柄 → 键帽样式 | Which kind of controller the latest clear input came from -> glyph style
static const void *who_of(int device) { return (const void *)(intptr_t)(device + 4); }
static int slot_of(const void *who, int make) { int fr = -1; for (int i = 0; i < NSLOT; i++) { if (g_s[i].who == who) return i; if (fr < 0 && !g_s[i].who) fr = i; }
    if (make && fr >= 0) { memset(&g_s[fr], 0, sizeof g_s[fr]); g_s[fr].who = who; return fr; } return -1; }

static int32_t axis(float v) { if (v > 1) v = 1; if (v < -1) v = -1; return (int32_t)lroundf(v * DM_STICK_MAX); }
static int stick_on(int was, int32_t x, int32_t y) { double m = hypot(x, y) / DM_STICK_MAX; return was ? m >= 0.15 : m >= 0.25; }   // 迟滞：推过 25% 算推出，回到 15% 内算回中 | Hysteresis: past 25% counts as pushed out, back within 15% counts as centered

// Java 的通用键位 → 游戏键（面键按类型排，见文件头；其余一一对应：肩 L1/R1 → L/R、扳机 L2/R2 → ZL/ZR、摇杆按下 → LS/RS、
// 开始 / 选择 → ＋ / －、十字键）
// The Java generic bits -> game buttons (the face laid out by kind, see the file header; the rest map one to one: shoulders L1/R1 -> L/R,
// triggers L2/R2 -> ZL/ZR, stick presses -> LS/RS, start / select -> + / -, D-pad)
static uint64_t game_bits(int kind, uint64_t b) {
    int a = !!(b & GPB_A), bb = !!(b & GPB_B), x = !!(b & GPB_X), y = !!(b & GPB_Y);
    uint64_t g = kind == DM_PAD_KIND_NINTENDO ? dm_padmap_face(bb, a, y, x) : dm_padmap_face(a, bb, y, x);
    if (b & GPB_L1) g |= DM_BTN_L; if (b & GPB_R1) g |= DM_BTN_R; if (b & GPB_L2) g |= DM_BTN_ZL; if (b & GPB_R2) g |= DM_BTN_ZR;
    if (b & GPB_LS) g |= DM_BTN_LS; if (b & GPB_RS) g |= DM_BTN_RS;
    if (b & GPB_START) g |= DM_BTN_PLUS; if (b & GPB_BACK) g |= DM_BTN_MINUS;
    if (b & GPB_DL) g |= DM_BTN_LEFT; if (b & GPB_DU) g |= DM_BTN_UP; if (b & GPB_DR) g |= DM_BTN_RIGHT; if (b & GPB_DD) g |= DM_BTN_DOWN;
    return g;
}
static int glyph_of(int kind) { return kind == DM_PAD_KIND_XBOX ? DM_GLYPH_XBOX : kind == DM_PAD_KIND_PS ? DM_GLYPH_PS : DM_GLYPH_PAD; }
// 注：从 v2.0.0 树出包时把上面 DM_GLYPH_PS 换成 DM_GLYPH_PAD——PS 键帽（DM_GLYPH_PS 与 ps_glyphs 标记）是 v2.0.0 之后的 PR #119 才进的，
// 那版树上没有这个枚举值，覆盖层也没带那套图，宿主同样会退回 Switch 键帽 | Note: when packaging from the v2.0.0 tree, replace DM_GLYPH_PS
// above with DM_GLYPH_PAD -- PS glyphs (the DM_GLYPH_PS enum and the ps_glyphs marker) only arrived after v2.0.0 in PR #119; that tree has no such
// enum and its overlay carries no such set, and the host falls back to the Switch glyphs all the same

// 把各手柄合起来、定模式、交给宿主。任何一份变了都重算一遍（在手柄锁内调；与 dm_gamepad.m 的 apply 同一段逻辑）
// Merge all controllers, decide the mode and hand the result to the host. Recomputed whenever any one of them changes (called under the controller
// lock; the same logic as apply in dm_gamepad.m)
static void apply(void) {
    uint64_t b = 0; int32_t ax[4] = {0}; double best[2] = {0};
    for (int i = 0; i < NSLOT; i++) { if (!g_s[i].who) continue; b |= g_s[i].b;
        for (int k = 0; k < 2; k++) { double m = hypot(g_s[i].ax[2 * k], g_s[i].ax[2 * k + 1]); if (m > best[k]) { best[k] = m; ax[2 * k] = g_s[i].ax[2 * k]; ax[2 * k + 1] = g_s[i].ax[2 * k + 1]; } } }
    static int on[2]; static uint64_t prev_b;
    on[0] = stick_on(on[0], ax[0], ax[1]); on[1] = stick_on(on[1], ax[2], ax[3]);
    if (!on[0] && !on[1]) g_need_center = 0;   // 碰过屏幕后摇杆回过中了：之后再推出来算数 | The stick has returned to centre since the screen was touched: pushing it out again from now on counts
    if ((b & ~prev_b) || ((on[0] || on[1]) && !g_need_center)) { dm_padmode_request_glyph(g_glyph); set_mode(1); }   // 先报键帽样式再要手柄版：同一次切换里就换成这只手柄的键帽 | Report the glyph style before asking for the controller layout, so the same switch already uses this controller's glyphs
    prev_b = b;
    if (!g_pad_mode || !on[0]) ax[0] = ax[1] = 0; if (!g_pad_mode || !on[1]) ax[2] = ax[3] = 0;   // 触屏模式不交摇杆；手柄模式下没推出来的给 0 | Sticks are not delivered in touch mode; in controller mode a stick not pushed out gives 0
    // ZR：原版六个技能格之一。这一格空着（没绑技能）时按它 = 免费回城（手柄玩家也免费回城，与触屏回城钮一致）；绑了技能就整颗交还原版。
    // 只在游戏态接管：面板开着时 ZR 照常交给原版（储藏箱 / 编年史用 ZL / ZR 翻页签）；按下时在游戏态接管的那一下，到松开前都不交给游戏
    // ZR: one of the six original skill slots. While this slot is empty (no skill bound), pressing it = the free town portal (controller players get it
    // too, consistent with the touch town portal button); once a skill is bound the whole button goes back to the original game. Taken over only in the
    // gameplay state: with a panel open ZR reaches the original game as usual (the stash / Chronicle page with ZL / ZR); a press taken over in the
    // gameplay state is not handed to the game until it is released
    { static uint64_t prev; static int mine;
      if ((b & DM_BTN_ZR) && !(prev & DM_BTN_ZR)) { mine = !dm_front_navigation() && dm_freetp_zr_free((b & DM_BTN_ZL) != 0); if (mine) { dm_tp_request(); LOG("手柄：ZR（这一格空着）→ 免费回城"); } }
      prev = b; if (mine) b &= ~(uint64_t)DM_BTN_ZR; if (!(b & DM_BTN_ZR) && !(prev & DM_BTN_ZR)) mine = 0; }
    dm_pad_set(b, ax[0], ax[1], ax[2], ax[3]);
}
static void put(const void *who, uint64_t b, int32_t lx, int32_t ly, int32_t rx, int32_t ry, int glyph) {
    int i = slot_of(who, 1); if (i < 0) return;
    if ((b & ~g_s[i].b) || hypot(lx, ly) >= 0.25 * DM_STICK_MAX || hypot(rx, ry) >= 0.25 * DM_STICK_MAX) g_glyph = glyph;   // 这只手柄有明确输入：键帽跟它的类型 | This controller gave clear input: the glyphs follow its kind
    g_s[i].b = b; g_s[i].ax[0] = lx; g_s[i].ax[1] = ly; g_s[i].ax[2] = rx; g_s[i].ax[3] = ry; g_s[i].glyph = glyph; apply();
}
static void drop(const void *who) {   // 这个手柄没了：只拿掉它那一份；一个手柄都不剩才回触屏模式 | This controller is gone: remove only its share; return to touch mode only when none is left
    int i = slot_of(who, 0); if (i < 0) return; memset(&g_s[i], 0, sizeof g_s[i]); apply();
    for (int k = 0; k < NSLOT; k++) if (g_s[k].who) return;
    set_mode(0);
}
void dm_gamepad_touched(void) {   // 前端（touch_android.c，放掉自己的锁之后调）：碰了屏幕。摇杆此刻偏着的话要先回中才重新接管 | The front end (touch_android.c, after dropping its own lock): the screen was touched. If a stick is deflected right now it must return to centre before taking over again
    pthread_mutex_lock(&lock);
    if (g_pad_mode) { g_need_center = 1; set_mode(0); apply(); }
    pthread_mutex_unlock(&lock);
}
void dm_gamepad_reset(void) {   // 前端（jni_android.c，失焦 / 后台）：所有手柄的键和摇杆清零 | The front end (jni_android.c, focus loss / background): zero every controller's buttons and sticks
    pthread_mutex_lock(&lock);
    int any = 0; for (int i = 0; i < NSLOT; i++) if (g_s[i].who) { any |= g_s[i].b != 0 || g_s[i].ax[0] || g_s[i].ax[1] || g_s[i].ax[2] || g_s[i].ax[3]; g_s[i].b = 0; memset(g_s[i].ax, 0, sizeof g_s[i].ax); }
    if (any) { LOG("手柄：失活，手柄的键和摇杆清零"); apply(); }
    pthread_mutex_unlock(&lock);
}

// Java 送来一只手柄的现值（电平）：device = InputDevice 的 id（注入为 −1），kind 见 enum，buttons = GPB_* 位，
// 轴为 ±1（上为正，Java 侧已把安卓的「上为负」翻转）
// Java hands over one controller's current values (levels): device = the InputDevice id (-1 for injection), kind as in the enum, buttons = GPB_*
// bits, axes as ±1 (up positive; the Java side has already flipped Android's "up is negative")
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativePad(JNIEnv *env, jclass cls, jint device, jint kind, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry) {
    (void)env; (void)cls;
    pthread_mutex_lock(&lock);
    put(who_of(device), game_bits(kind, (uint32_t)buttons), axis(lx), axis(ly), axis(rx), axis(ry), glyph_of(kind));
    pthread_mutex_unlock(&lock);
}
JNIEXPORT void JNICALL Java_com_xiaoboluo_d2r_DmActivity_nativePadGone(JNIEnv *env, jclass cls, jint device) {
    (void)env; (void)cls;
    pthread_mutex_lock(&lock);
    drop(who_of(device));
    pthread_mutex_unlock(&lock);
}

// 测试开关 DM_TEST_PAD（见文件头）：取手柄 tick 里泵（touch_android.c 在拿它自己的锁之前调——这里会经 set_mode 碰 touch 层的锁）
// Test switch DM_TEST_PAD (see the file header): pumped on the controller-read tick (touch_android.c calls it before taking its own lock -- this goes
// through set_mode, which touches the touch layer's lock)
void dm_android_gamepad_test_pump(void) {
    static struct { uint64_t at; uint64_t b; int32_t ax[4]; } ev[16]; static int n = -1, k; static uint64_t t0;
    if (n < 0) { n = 0; const char *e = getenv("DM_TEST_PAD"); if (!e || !*e) return; t0 = now_ns();
        for (const char *q = e; q && *q && n < 16; ) { unsigned long long ms, b; int v[4] = {0};
            int got = sscanf(q, "%llu:%llx:%d:%d:%d:%d", &ms, &b, &v[0], &v[1], &v[2], &v[3]);
            if (got >= 2 && ms < 3600000ull) { ev[n].at = t0 + ms * 1000000ull; ev[n].b = b; for (int i = 0; i < 4; i++) ev[n].ax[i] = got >= 3 + i ? v[i] : 0; n++; }
            q = strchr(q, ','); if (q) q++; }
        LOG("DM_TEST_PAD：%d 段（虚拟手柄，按键位与摇杆是 dm_input.h 的游戏值）", n); }
    if (!n) return;
    uint64_t now = now_ns();
    while (k < n && now >= ev[k].at) { pthread_mutex_lock(&lock); put((const void *)(intptr_t)1, ev[k].b, ev[k].ax[0], ev[k].ax[1], ev[k].ax[2], ev[k].ax[3], DM_GLYPH_PAD); pthread_mutex_unlock(&lock); k++; }
}
