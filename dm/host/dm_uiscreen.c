// 游戏有一对「屏幕宽高」全局量 [main+0x211da38]（两个 32 位）：界面按其中的「高」定缩放、按宽高定摆位矩形；3D 出图与命中框不看它（实验，
// 实验得出）。画面比 16:9 窄时把「高」写成 宽÷16×9，界面就完整落在 16:9 里、3D 仍铺满；界面是贴着画面顶边画的，
// 要居中 / 贴底由图形层把界面那一趟绘制整体下移 dm_ui_rect 给的 y（界面独占一趟、帧末合成，量过）。
// 写这对量的只有游戏的窗口初始化（main+0xe10e8c、0xe12ecc，切主机 / 掌机档会再走）。这里不逐个找时机，每帧比一次：
// 全局量不等于我们上次写的值 = 游戏刚重写过 → 记下新的真实值、再写一次。
// 配套：点世界按真实画面算（dm_world.c 用 dm_screen_real）；dm_ui_* 的入口吃的是「界面矩形里的归一化坐标」，由调用方（触摸前端 / 测试点按通路）换算。
// 缺省关。打开：前端调 dm_uiscreen_enable；Mac 宿主测试用环境变量 DM_UI_169=1（等于贴顶）与 DM_UI_YPOS=top | center | bottom。
// The game has a pair of "screen width / height" globals [main+0x211da38] (two 32-bit values): the UI derives its scale from the "height" and its placement rectangle from width and height; 3D rendering and hit boxes do not look at it (experiment,
// obtained by experiment). When the frame is narrower than 16:9, write "height" as width÷16×9, and the UI falls entirely within 16:9 while 3D still fills the screen; the UI is drawn flush with the top edge of the frame,
// so centering / bottom-aligning is done by the graphics layer shifting the whole UI draw pass down by the y given by dm_ui_rect (the UI has a pass of its own, composited at end of frame; measured).
// The only writer of this pair is the game's window initialization (main+0xe10e8c, 0xe12ecc; switching docked / handheld mode goes through it again). Rather than hunting for each occasion, compare once per frame here:
// globals not equal to the value we last wrote = the game has just rewritten them → record the new real values and write once more.
// Companion pieces: world taps are computed on the real frame (dm_world.c uses dm_screen_real); the dm_ui_* entry points take "normalized coordinates within the UI rectangle", converted by the caller (touch front end / test tap path).
// Off by default. To turn on: the front end calls dm_uiscreen_enable; for Mac host testing use the environment variables DM_UI_169=1 (equivalent to top-aligned) and DM_UI_YPOS=top | center | bottom.
#include "dm_uiscreen.h"
#include "hle.h"
#include "dm.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic int g_mode = -1;              // -1 = 还没定（第一次用时看环境变量） | -1 = not decided yet (checks the environment variable on first use)
static _Atomic int g_rw, g_rh, g_uh;         // 真实宽高、写给游戏的界面高；g_rw == 0 = 没在改写 | Real width and height, and the UI height written to the game; g_rw == 0 = not rewriting
static uint8_t *main_base(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static int mode(void) { int m = atomic_load(&g_mode); if (m >= 0) return m;
    const char *e = getenv("DM_UI_169"), *p = getenv("DM_UI_YPOS"); m = !(e && strcmp(e, "0")) ? DM_UISCREEN_OFF : p && !strcmp(p, "center") ? DM_UISCREEN_CENTER : p && !strcmp(p, "bottom") ? DM_UISCREEN_BOTTOM : DM_UISCREEN_TOP;
    int exp = -1; atomic_compare_exchange_strong(&g_mode, &exp, m); return atomic_load(&g_mode); }
void dm_uiscreen_enable(int m) { atomic_store(&g_mode, m < DM_UISCREEN_OFF || m > DM_UISCREEN_BOTTOM ? DM_UISCREEN_OFF : m); }
// 局内 / 局外（前端各页是按 16:9 整页设计的——点击区、标题、3D 人物是一体——整页居中留边；真全屏只在局内开）。
// 唯一信号：dm_uiscreen_ingame()。定义 = HUDPanel 在且读取画面（LoadScreenPanel）不在，即读取画面两头都归局外：进局时读取画面撤掉后的第一帧起才算局内，
// 出局时读取画面一盖上来就算局外。由界面每帧那一处（dm_cursor.c，游戏主线程）锁存，宿主各处与图形层都只读这一个值。
// 界面矩形：局外居中，局内按前端选的摆法（贴底）。前端各页游戏本身就按 16:9 出图（见 dm_game_res），界面矩形 = 整个画面；居中只在「读取中、已是整屏分辨率」那一段用到。
// In a run / out of a run (the front end pages are designed as whole 16:9 pages - tap areas, titles and the 3D character are one piece - so the whole page is centered with margins; true fullscreen is on only in a run).
// The single signal: dm_uiscreen_ingame(). Definition = HUDPanel present and the loading screen (LoadScreenPanel) absent, i.e. both ends of the loading screen count as out of a run: on entering, in-run starts from the first frame after the loading screen is removed,
// and on leaving, out-of-run starts as soon as the loading screen covers the view. It is latched at the per-frame UI site (dm_cursor.c, game main thread), and everywhere in the host and the graphics layer reads only this one value.
// UI rectangle: centered out of a run, placed as the front end chooses in a run (bottom-aligned). On the front end pages the game itself already renders at 16:9 (see dm_game_res), so UI rectangle = the whole frame; centering is only used in the "loading, already at full-screen resolution" stretch.
static _Atomic int g_ingame;
void (*g_dm_charsel_hook)(void);   // 退出一局回到前端那一刻（diag.c 界面消息 FrontEndNavigation:ToFrontEnd 里调） | The moment of quitting a run back to the front end (called in diag.c's UI message FrontEndNavigation:ToFrontEnd)
void (*g_dm_ingame_hook)(int on);   // 进局 / 出局那一刻（读取画面也算一次出局）；iOS：cloud_ios.m 拿「进局」记一局开始 | The moment of entering / leaving a run (the loading screen also counts as leaving once); iOS: cloud_ios.m uses "entering a run" to mark the start of a run
void dm_uiscreen_set_ingame(int on) { if (atomic_exchange(&g_ingame, on != 0) != (on != 0)) { LOG("界面画面：%s", on ? "进局（真全屏）" : "出局（留边）"); if (g_dm_ingame_hook) g_dm_ingame_hook(on != 0); } }
int dm_uiscreen_ingame(void) { return atomic_load(&g_ingame); }
// 局内开着大面板：整块界面移到正中、上下留同样宽的边，关掉回落贴底；不做过渡（直接跳）。
// 判据就是触摸前端的「面板模式」（摇杆收起、单指当光标的那个状态，front_ios.m 在界面消息回调里同步算出），只开着 NPC 对话菜单不算（菜单跟着 NPC 摆，留在贴底）。
// 消息回调与游戏排界面在同一线程、先后同一帧，所以开 / 关那一帧界面就在新位置；图形层下移、前端游戏视图、世界字投影、点世界换算都每帧现读 yoff，自然跟着走
// A large panel open in a run: the whole UI moves to the center with equal margins above and below, and drops back to bottom-aligned when closed; no transition (a direct jump).
// The criterion is the touch front end's "panel mode" (the state where the stick is hidden and a single finger acts as the cursor, computed synchronously by front_ios.m in the UI message callback); having only the NPC dialog menu open does not count (the menu is placed following the NPC and stays bottom-aligned).
// The message callback and the game's UI layout run on the same thread, one after the other in the same frame, so on the open / close frame the UI is already at the new position; the graphics layer shift, the front end game view, the world text projection and the world tap conversion all read yoff fresh every frame and follow along naturally
static _Atomic int g_panel;
int dm_uiscreen_panel(void) { return atomic_load(&g_panel); }
void dm_uiscreen_set_panel(int on) { if (atomic_exchange(&g_panel, on != 0) != (on != 0) && atomic_load(&g_ingame) && atomic_load(&g_rw)) LOG("界面画面：%s", on ? "开大面板 → 界面移到正中" : "大面板关了 → 界面回落"); }
static int yoff(void) { int rh = atomic_load(&g_rh), uh = atomic_load(&g_uh), m = mode(); if (!atomic_load(&g_rw) || rh <= uh || m == DM_UISCREEN_OFF) return 0;
    if (!atomic_load(&g_ingame) || atomic_load(&g_panel)) m = DM_UISCREEN_CENTER; return m == DM_UISCREEN_CENTER ? (rh - uh) / 2 : m == DM_UISCREEN_BOTTOM ? rh - uh : 0; }

// 跟着世界走的界面字（地上物品名、NPC 名字牌、血条、飘字）：游戏用镜头的「世界点 → 屏幕」main+0x1237c60(镜头, 点, out) 算出真实画面像素（y = [镜头+0x144] × (0.5 − 0.5·ndc_y)），
// 直接当界面坐标用（调用方只有 0x90d110 四处与 0xa71bb0）。界面整趟下移 y0 之后这些字就低了 y0（名字牌落到脚下）。
// 镜头对象同时是出图用的，投影矩阵（+0x80 起 4 列）每帧由 0x162f9ac 无条件重算，往对象里写值留不住也不该写。所以在函数入口（观测门 OBS_UI_PROJECT，只在 touch 规则域）
// 把 x0 换成镜头的一份副本，副本的投影每列 y += (2·y0 / 高) × 该列 w ⇒ 出来的 y 正好少 y0；游戏的镜头一个字节不动。y0 = 0（没开 / 贴顶 / 16:9 屏）时不换。
// UI text that follows the world (ground item names, NPC nameplates, health bars, floating text): the game uses the camera's "world point → screen" main+0x1237c60(camera, point, out) to compute real frame pixels (y = [camera+0x144] × (0.5 − 0.5·ndc_y)),
// and uses them directly as UI coordinates (the only callers are four sites in 0x90d110 and 0xa71bb0). Once the whole UI pass is shifted down by y0, this text ends up y0 too low (nameplates land at the feet).
// The camera object is also used for rendering, and the projection matrix (4 columns from +0x80) is unconditionally recomputed every frame by 0x162f9ac, so values written into the object would not stick and should not be written anyway. So at the function entry (observation gate OBS_UI_PROJECT, only in the touch rule domain)
// swap x0 for a copy of the camera, with each projection column of the copy getting y += (2·y0 / height) × that column's w ⇒ the resulting y is smaller by exactly y0; not one byte of the game's camera is changed. When y0 = 0 (off / top-aligned / 16:9 screen), no swap.
static _Atomic int g_proj_on;
static void ui_project_obs(const uint64_t *regs) {
    int dy = atomic_load(&g_ingame) ? yoff() : 0; const uint8_t *c = (const uint8_t *)(uintptr_t)regs[0]; if (!c || c[0x160]) return;   // +0x160 = 投影待重算：函数自己会重算并盖掉副本里的偏移，这一次不换（只在改分辨率后的第一次调用） | +0x160 = projection pending recompute: the function will recompute it itself and overwrite the offset in the copy, so do not swap this time (only on the first call after a resolution change)
    // 越肩镜头（dm_cam.c dm_cam_label_matrix）：副本的投影换成「透视视图投影 ÷ 原版视图」，名字牌、血条、地上物品名和点按都落在透视画面上；界面下移照旧叠在上面
    // Over-the-shoulder camera (dm_cam.c dm_cam_label_matrix): the copy's projection becomes "perspective view-projection / original view", so nameplates,
    // health bars, ground item names and taps land on the perspective picture; the UI shift still applies on top
    int dm_cam_label_matrix(const float p[16], float out[16]);
    float pm[16]; int cam = dm_cam_label_matrix((const float *)(c + 0x80), pm); if (!dy && !cam) return;
    float H = *(const float *)(c + 0x144); if (dy && !(H > 0)) return;
    // 副本放在 64 份的静态环里轮着用（原子取号），不用 _Thread_local：安卓上 dlopen 进来的库，线程第一次碰线程局部变量要现场 malloc，
    // 这道门跑在游戏线程自己的栈上，有的游戏线程栈很小——镜头开着时每次调用都走到这里，实测 OnePlus 13 上 libc 里一路调深、栈溢出（sig 11，出错地址 = sp）。
    // 这个函数只做投影，几微秒就用完副本，64 份不会在用着的时候被轮到
    // The copy lives in a static ring of 64 taken in turn (atomic counter) rather than _Thread_local: in a library loaded with dlopen on Android, a thread's
    // first touch of a thread-local variable mallocs on the spot, and this gate runs on the game thread's own stack, which is small on some game threads --
    // with the camera on every call reaches here, and on a OnePlus 13 that went deep into libc and overflowed the stack (sig 11, fault address = sp).
    // The function only projects and is done with the copy within microseconds, so none of the 64 comes round again while still in use
    static _Alignas(16) uint8_t ring[64][0x170]; static _Atomic unsigned slot;
    uint8_t *copy = ring[atomic_fetch_add(&slot, 1) & 63]; memcpy(copy, c, sizeof ring[0]); float *m = (float *)(copy + 0x80);
    if (cam) memcpy(m, pm, sizeof pm);
    if (dy) { float k = 2.0f * dy / H; for (int j = 0; j < 4; j++) m[j * 4 + 1] += k * m[j * 4 + 3]; }
    ((uint64_t *)(uintptr_t)regs)[0] = (uint64_t)(uintptr_t)copy;
}
int dm_ui_proj_dy(void) { return atomic_load(&g_proj_on) && atomic_load(&g_ingame) ? yoff() : 0; }
// 观测门在不在（老产物没有这道门）：在的话，游戏的「世界点 → 屏幕」在越肩镜头开着时直接给透视画面上的坐标（dm_world.c 点按据此不再自己换算）
// Whether the observation gate exists (older artifacts lack it): when it does, the game's "world point -> screen" gives perspective picture coordinates
// directly while the over-the-shoulder camera is on (dm_world.c taps then skip their own conversion)
int dm_ui_proj_gate(void) { return atomic_load(&g_proj_on); }

// 游戏的出图分辨率（主机 / 掌机档各一个参数位，见 thread.c 与 hle_oe.c）：真全屏打开时按「玩家单位在不在」给值——不在（前端各页）= 屏宽的 16:9，
// 游戏整个按 16:9 出图，3D、界面、点击区天然一致（就是留边模式）；在 = 屏幕原生比例。状态一变推一次「性能模式变化」通知（31），游戏重读参数位、改出图尺寸。
// 玩家单位的建立 / 销毁都落在读取画面期间；读取画面期间界面矩形是居中的 16:9（dm_uiscreen_ingame 为 0），所以读取画面肉眼位置不变。
// The game's render resolution (one parameter slot each for docked / handheld mode, see thread.c and hle_oe.c): with true fullscreen on, the value is given by "whether the player unit exists" - absent (front end pages) = 16:9 of the screen width,
// the game renders entirely at 16:9, and 3D, UI and tap areas are naturally consistent (this is just letterbox mode); present = the screen's native aspect ratio. Whenever the state changes, push one "performance mode changed" notification (31), and the game rereads the parameter slot and changes the render size.
// The player unit's creation / destruction both fall within the loading screen; during the loading screen the UI rectangle is a centered 16:9 (dm_uiscreen_ingame is 0), so the loading screen does not visibly move.
uint64_t front_game_res(int docked); void dm_oe_res_changed(void);
// 游戏处理 31 时拿「屏幕宽高」全局量和新分辨率比（main+0xe10e10），相同就不切。出局时这对量里还是我们写的 16:9 高，正好等于要切去的值，游戏会以为已经切好了。
// 所以在游戏取走 31 的那一刻（hle_oe.c，同一个线程、紧挨着它的比较之前）把真实高写回：同一帧里写回、比较、切换、游戏自己写成新值，中间没有一帧按 4:3 排界面。
// When handling 31 the game compares the "screen width / height" globals with the new resolution (main+0xe10e10) and does not switch if they are equal. On leaving a run this pair still holds the 16:9 height we wrote, which happens to equal the value to switch to, so the game would think the switch is already done.
// So at the moment the game takes 31 (hle_oe.c, same thread, right before its comparison) write the real height back: within the same frame come the write-back, comparison, switch, and the game writing the new value itself, with not a single frame laying out the UI at 4:3 in between.
void dm_uiscreen_res_reread(void) { uint8_t *b = main_base(); int rw = atomic_load(&g_rw); if (!b || !rw) return; int32_t *g = (int32_t *)(b + 0x211da38);
    if (g[0] == rw && g[1] == atomic_load(&g_uh)) g[1] = atomic_load(&g_rh); atomic_store(&g_rw, 0); }
// 游戏处理 31 时（main+0xe10df0–0xe10e0c）只接受「宽高都不超过窗口初始化时的尺寸 [main+0x211da30]」的新值：窗口贴图按启动时的参数位一次建好，之后只在里面改出图区域。
// 所以启动时给屏幕原生值（最大的那个），窗口初始化完、界面每帧那一处第一次跑到时再切到前端的 16:9。
// When handling 31 (main+0xe10df0–0xe10e0c) the game only accepts new values where "both width and height do not exceed the size at window initialization [main+0x211da30]": the window texture is created once from the startup parameter slot, and afterwards only the render region inside it changes.
// So give the screen's native value (the largest one) at startup, and switch to the front end's 16:9 once window initialization is done and the per-frame UI site first runs.
static _Atomic int g_world, g_res_live;
int dm_uiscreen_res_live(void) { return atomic_load(&g_res_live); }
uint64_t dm_game_res(int docked) { uint64_t r = front_game_res(docked); uint32_t w = (uint32_t)r, h = (uint32_t)(r >> 32);   // 先问前端：iOS 前端在第一次被问时才决定开不开真全屏 | Ask the front end first: the iOS front end only decides whether to turn on true fullscreen the first time it is asked
    if (atomic_load(&g_res_live) && mode() != DM_UISCREEN_OFF && !atomic_load(&g_world) && (uint64_t)w * 9 < (uint64_t)h * 16) h = (uint32_t)((uint64_t)w * 9 / 16) & ~1u;
    return (uint64_t)h << 32 | w; }

void dm_uiscreen_pump(void) {
    uint8_t *b = main_base(); if (!b) return;
    { int world = ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)) != 0;
      int first = !atomic_exchange(&g_res_live, 1);
      if ((atomic_exchange(&g_world, world) != world || (first && !world)) && mode() != DM_UISCREEN_OFF) { LOG("界面画面：%s → 游戏分辨率切到 %u x %u", world ? "玩家单位建立" : first ? "窗口已建好、在前端" : "玩家单位销毁", (unsigned)dm_game_res(1), (unsigned)(dm_game_res(1) >> 32)); dm_oe_res_changed(); } }
    static int reg; if (!reg) { reg = 1; if (gate_obs_present(OBS_UI_PROJECT)) { gate_obs_set(OBS_UI_PROJECT, ui_project_obs); atomic_store(&g_proj_on, 1); } } int32_t *g = (int32_t *)(b + 0x211da38); int m = mode(), rw = atomic_load(&g_rw);
    if (!atomic_load(&g_world) && m != DM_UISCREEN_OFF) return;   // 不在局里：游戏自己按 16:9 出图，这对量不改写（出局那一下的写回见 dm_uiscreen_res_reread） | Not in a run: the game renders at 16:9 by itself, so this pair of values is not rewritten (for the write-back at the moment of leaving a run see dm_uiscreen_res_reread)
    if (m == DM_UISCREEN_OFF) { if (rw) { if (g[0] == rw && g[1] == atomic_load(&g_uh)) g[1] = atomic_load(&g_rh); atomic_store(&g_rw, 0); LOG("界面画面：关掉，屏幕高写回 %d", g[1]); } return; }   // 没打开过就什么都不碰 | If it was never turned on, touch nothing
    int w = g[0], h = g[1]; if (w <= 0 || h <= 0) return;
    if (rw && w == rw && h == atomic_load(&g_uh)) return;   // 还是我们上次写的值 | Still the value we wrote last time
    static int n; int uh = h; if ((long)w * 9 < (long)h * 16) { uh = (int)((long)w * 9 / 16) & ~1; g[1] = uh; }
    atomic_store(&g_rh, h); atomic_store(&g_uh, uh); atomic_store(&g_rw, w);
    LOG("界面画面：游戏写入屏幕宽高 %d x %d（第 %d 次）→ 界面用 %d x %d，纵向位置 %d", w, h, ++n, w, uh, yoff());
}
int dm_screen_real(int *w, int *h) { int rw = atomic_load(&g_rw);
    if (rw) { *w = rw; *h = atomic_load(&g_rh); return 1; }
    uint8_t *b = main_base(); if (!b) return 0; *w = *(int32_t *)(b + 0x211da38); *h = *(int32_t *)(b + 0x211da3c); return *w > 0 && *h > 0; }   // 没在改写：就是游戏的值（与改之前逐字节同） | Not rewriting: it is simply the game's value (byte-for-byte the same as before the change)
void dm_ui_rect(int *x, int *y, int *w, int *h) { int rw = 0, rh = 0; dm_screen_real(&rw, &rh); *x = 0; *y = yoff(); *w = rw; *h = atomic_load(&g_rw) ? atomic_load(&g_uh) : rh; }
float dm_ui_ny(float ny) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); if (!dm_screen_real(&rw, &rh) || h <= 0 || h == rh) return ny; return (ny * rh - y) / h; }
float dm_ui_ny_back(float ny) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); if (!dm_screen_real(&rw, &rh) || rh <= 0 || h == rh) return ny; return (ny * h + y) / rh; }
