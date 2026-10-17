// 选角页的构建号与新版本提示。地址为 3.3.94012 原映像偏移。
// 画面上的东西全是游戏自己的控件：布局覆盖层在主菜单面板（MainMenuPanel，选角页那一层）下加了七个（dm/ios/tools/touch_layout.py 的 _UPDATE）——
//   TouchBuild 构建号（左下角常显）；TouchUpdNote + TouchUpdSmall 普通更新的一行字与小「更新」；
//   TouchUpdBg / TouchUpdTitle / TouchUpdDesc / TouchUpdBig 建议立即更新的底板、红字标题、说明与大「更新」（「进入游戏」正上方）。
// 布局里字是空的、按钮与底板在屏幕外；这里在游戏主线程上写字、把要显示的挪回原位。没有这些控件（Mac 宿主跑原版布局）或没人调 dm_update_init = 什么都不做。
// 两个「更新」按钮发 PanelManager:ClosePanel:TouchUpdate，这里收到后转给外壳登记的回调（iOS：交系统打开安装链接，dm/ios/Sources/dm_update.m）。
// 联网、比新旧、一天一次都在外壳那边；这里只认 dm_update_offer 给的结果。
// Build number and new version notice on the character select page. Addresses are offsets in the 3.3.94012 original image.
// Everything on screen is the game's own widgets: the layout overlay adds seven under the main menu panel (MainMenuPanel, the character select page layer) (_UPDATE in dm/ios/tools/touch_layout.py) -
//   TouchBuild build number (always shown at the bottom left); TouchUpdNote + TouchUpdSmall the one line of text and small 「更新」(Update) for an ordinary update;
//   TouchUpdBg / TouchUpdTitle / TouchUpdDesc / TouchUpdBig the backplate, red title, description and large 「更新」(Update) for a recommended immediate update (right above 「进入游戏」(Play)).
// In the layout the text is empty and the buttons and backplate are off screen; here, on the game main thread, we write the text and move what should be shown back into place. With no such widgets (Mac host running the original layout) or nobody calling dm_update_init = do nothing.
// The two 「更新」(Update) buttons send PanelManager:ClosePanel:TouchUpdate; on receiving it here we forward to the callback registered by the shell (iOS: hand the install link to the system to open, dm/ios/Sources/dm_update.m).
// Networking, comparing versions and the once-a-day limit are all on the shell side; here only the result given by dm_update_offer is honored.
#include "hle.h"
#include "dm.h"
#include "dm_i18n.h"
#include "dm_cursor.h"
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <TargetConditionals.h>

static int g_own = -1; static void (*g_on_select)(void), (*g_on_click)(void);
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static struct { int build, urgent; char note[160]; } g_offer;   // g_mu 护 g_offer | g_mu guards g_offer
static _Atomic int g_safe_l = -1, g_dirty, g_click, g_appear, g_fg;   // g_safe_l < 0 = 外壳还没给左安全区，先不写字（免得字先出在安全区里再跳一下）。g_dirty：还要重写几帧（>0 时每帧找控件、写字、摆位置；写完一次后再跟 30 帧，等文字框量出新宽度） | g_safe_l < 0 = the shell has not supplied the left safe area yet, so do not write text for now (to avoid the text first showing inside the safe area and then jumping). g_dirty: how many more frames to rewrite (when >0, every frame find widgets, write text, set positions; after one write keep following for 30 more frames, waiting for the text box to measure the new width)

void dm_update_init(int own_build, void (*on_select)(void), void (*on_click)(void)) { g_own = own_build; g_on_select = on_select; g_on_click = on_click; atomic_store(&g_dirty, 30); }   // 不在这里置「选角页出现」：启动时游戏自己会发一次 OpenPanel CharacterSelectPanel，这里再置就每次启动查两遍 | Do not set "character select page appears" here: at startup the game sends OpenPanel CharacterSelectPanel once by itself, and setting it here too would check twice on every startup
// 外壳查完的结果：任意线程调。new_build > 0 = 有比自己新的构建（note = 一句话更新说明，UTF-8；urgent = 建议立即更新）；0 = 没有，撤掉已有提示
// Result of the shell's check: callable from any thread. new_build > 0 = there is a build newer than ours (note = one-sentence update note, UTF-8; urgent = immediate update recommended); 0 = none, remove any existing notice
void dm_update_offer(int new_build, const char *note, int urgent) {
    pthread_mutex_lock(&g_mu); g_offer.build = new_build; g_offer.urgent = urgent; strlcpy(g_offer.note, note ? note : "", sizeof g_offer.note); pthread_mutex_unlock(&g_mu);
    atomic_store(&g_dirty, 30);
}
// 查新版本失败（玩家停在旧包、更新查询静默失败，玩家不知道有新版）：任意线程调。
// 0 = 这次取到了（撤掉失败提示）；1 = 系统说这个 App 没有无线数据许可（玩家自己能在设置里开）；2 = 其余取不到（地址失效、断网、格式不对…）；3 = HTTP 5xx 服务故障。
// 失败提示只在没有新版本提示时显示（已有的新版本提示照旧留着），写在普通更新提示那一行（TouchUpdNote），不出「更新」按钮
// New version check failed (the player stays on an old package, the update query fails silently, and the player does not know there is a new version): callable from any thread.
// 0 = fetched this time (remove the failure notice); 1 = the system says this App has no wireless data permission (the player can turn it on in Settings); 2 = other fetch failures (URL dead, offline, bad format...); 3 = HTTP 5xx service failure.
// The failure notice is shown only when there is no new version notice (an existing new version notice stays as before), written on the ordinary update notice line (TouchUpdNote), without the 「更新」(Update) button
// 3 = HTTP 5xx 服务故障，提示稍后重试，不引导去同一故障站点。
// 3 = HTTP 5xx service failure: suggest retrying later, not visiting the same failed host.
static _Atomic int g_fail;
static char g_site[96];   // 手动更新去哪：版本文件地址里的主机名（外壳在启动时给，之后不变）；没配更新地址就为空 | Where to go for a manual update: the host name in the version file URL (given by the shell at startup, unchanged afterwards); empty if no update URL is configured
void dm_update_site(const char *host) { snprintf(g_site, sizeof g_site, "%s", host ? host : ""); }
void dm_update_fail(int kind) { if (atomic_exchange(&g_fail, kind) != kind) atomic_store(&g_dirty, 30); }
// 左安全区（布局单位，2160 高；iPhone 横屏约 304，iPad 0）：前端在 iPhone 上铺满整屏，左下角那行字要让到安全线内。任意线程调
// Left safe area (layout units, 2160 high; about 304 on iPhone landscape, 0 on iPad): the front end fills the whole screen on iPhone, so the bottom-left line of text must move inside the safe line. Callable from any thread
void dm_update_safe_left(int units) { atomic_store(&g_safe_l, units < 0 ? 0 : units > 800 ? 800 : units); atomic_store(&g_dirty, 30); }
// 从后台回到前台：外壳调（任意线程）。回来时人若正停在选角页，按「选角页出现」同一条路重查一次（进首页就查）；在局内 / 别的页就作废，不留到以后
// Returning from background to foreground: called by the shell (any thread). If the user is sitting on the character select page on return, recheck once via the same path as "character select page appears" (checked on entering the home page); in a run / on another page it is discarded, not kept for later
void dm_update_foreground(void) { if (g_own >= 0) atomic_store(&g_fg, 1); }
// 模式切换事务完成、界面整套重建之后（dm_mode_items.h 登记的「进」）：控件全是新的、字是空的，重写一遍。重建时游戏不发 OpenPanel MainMenuPanel，等不到下面那条消息
// After the mode switch transaction completes and the whole UI has been rebuilt (the "enter" registered in dm_mode_items.h): the widgets are all new and their text is empty, so write it again. During the rebuild the game does not send OpenPanel MainMenuPanel, so the message below never arrives
void dm_update_mode_enter(void) { if (g_own >= 0) atomic_store(&g_dirty, 30); }
void dm_update_on_uimsg(const char *cat, const char *act, const char *arg) {   // 界面消息回调（diag.c 转来，游戏主线程） | UI message callback (forwarded from diag.c, game main thread)
    if (g_own < 0 || strcmp(cat, "PanelManager")) return;
    if (!strcmp(act, "ClosePanel") && !strcmp(arg, "TouchUpdate")) atomic_store(&g_click, 1);
    else if (!strcmp(act, "OpenPanel") && (!strcmp(arg, "MainMenuPanel") || !strcmp(arg, "CharacterSelectPanel"))) { atomic_store(&g_dirty, 30);   // 面板重建后控件是新的，重写一遍 | After a panel rebuild the widgets are new, so write everything again
        if (arg[0] == 'C') atomic_store(&g_appear, 1); }   // 选角页出现一次（启动、从局里退回来、列表收起后重开）：外壳重查一次新版本 | The character select page appears once (startup, returning from a run, list reopened after being collapsed): the shell rechecks once for a new version
}
static uint8_t *tb(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static uint64_t find(uint64_t w, const char *name, int dep) {   // 按名字找控件：名字 +8、子控件数组 +0x58、个数 +0x60（同 dm_cursor.c ui_find） | Find a widget by name: name +8, child array +0x58, count +0x60 (same as dm_cursor.c ui_find)
    if (!w || dep > 6) return 0; const char *nm = *(const char **)(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = find(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
static void set_text(uint8_t *b, uint64_t w, const char *s);
static void place(uint64_t w, int x, int y) {   // 矩形 x / y 在 +0x70 / +0x74；改完照原版补一次虚表 +0x10 让它重算（同 dm_cursor.c 安全区那处） | Rectangle x / y at +0x70 / +0x74; after changing them, call vtable +0x10 once as the original game does so it recomputes (same as the safe area site in dm_cursor.c)
    int32_t *r = (int32_t *)(w + 0x70); if (r[0] == x && r[1] == y) return; r[0] = x; r[1] = y; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x10))(w);
}
void dm_update_pump(void) {   // 取手柄处每帧调（游戏主线程） | Called every frame at the controller poll (game main thread)
    if (g_own < 0) return;
    if (atomic_exchange(&g_click, 0)) { LOG("更新：点了「更新」按钮"); if (g_on_click) g_on_click(); }
    // 手柄模式（原版手柄主菜单，没有光标点不了按钮）：有新版本提示、人停在主菜单（选角列表没开、没在建角）时，Y = 更新；「Y 更新」写在提示字那一行末尾（原版主菜单的提示条每帧由它自己重建，加进去的条目留不住）
    // Controller mode (the original controller main menu, which has no cursor to tap buttons with): when there is a new version notice and the player is on the main menu (character list not open, not creating a character), Y = update; "Y Update" is written at the end of the hint text line (the original main menu's legend is rebuilt by itself every frame, so an entry added to it does not stay)
    { int dm_padmode_current(void), dm_padmode_busy(void); uint64_t dm_padmode_page_keys(uint64_t);
      uint8_t *pb = tb(); uint64_t pr = pb ? *(uint64_t *)(pb + 0x2f05458) : 0; int have; pthread_mutex_lock(&g_mu); have = g_offer.build > 0; pthread_mutex_unlock(&g_mu);
      if (have && pr && dm_padmode_current() && !dm_padmode_busy()) { uint64_t mm = find(pr, "MainMenuPanel", 0), lg = mm ? find(mm, "MainMenuLegend", 0) : 0, cs = find(pr, "CharacterSelectPanel", 0), cc = find(pr, "CharacterCreatePanel", 0), st = find(pr, "SettingsPanel", 0);
          if (mm && *(uint8_t *)(mm + 0x50) && lg && find(mm, "TouchBuild", 0) && !(cs && *(uint8_t *)(cs + 0x50)) && !(cc && *(uint8_t *)(cc + 0x50)) && !(st && *(uint8_t *)(st + 0x50))) {
              if (dm_padmode_page_keys(0x8) & 0x8) { LOG("更新：手柄主菜单按了 Y（更新）"); if (g_on_click) g_on_click(); } } } }
    if (atomic_exchange(&g_fg, 0)) {   // 选角列表面板在且显示着（可见位 +0x50）、构建号控件也在 = 人停在选角页 | Character select list panel present and shown (visible bit +0x50) and the build number widget also present = the user is sitting on the character select page
        uint8_t *fb = tb(); uint64_t fr = fb ? *(uint64_t *)(fb + 0x2f05458) : 0, fm = fr ? find(fr, "MainMenuPanel", 0) : 0, fl = fr ? find(fr, "CharacterSelectPanel", 0) : 0;
        if (fm && find(fm, "TouchBuild", 0) && fl && *(uint8_t *)(fl + 0x50)) { atomic_store(&g_appear, 1); atomic_store(&g_dirty, 30); LOG("更新：回到前台时停在选角页，重查一次新版本"); } }
    int left = atomic_load(&g_dirty); if (left <= 0) return;
    uint8_t *b = tb(); uint64_t root = b ? *(uint64_t *)(b + 0x2f05458) : 0, mm = root ? find(root, "MainMenuPanel", 0) : 0, bw = mm ? find(mm, "TouchBuild", 0) : 0;
    if (!bw) return;   // 主菜单面板还没建（或跑的是没有这些控件的布局）：留着，建好那帧再写 | Main menu panel not built yet (or running a layout without these widgets): keep it, and write on the frame it is built
    if (atomic_exchange(&g_appear, 0) && g_on_select) g_on_select();   // 外壳这时给左安全区、查一次新版本 | At this point the shell supplies the left safe area and checks once for a new version
    if (atomic_load(&g_safe_l) < 0) return;
    atomic_compare_exchange_strong(&g_dirty, &left, left - 1);   // 期间又被置回 30（新结果 / 面板重建）就不减 | If it was set back to 30 in the meantime (new result / panel rebuilt), do not decrement
    int build, urgent; char note[160]; pthread_mutex_lock(&g_mu); build = g_offer.build; urgent = g_offer.urgent; memcpy(note, g_offer.note, sizeof note); pthread_mutex_unlock(&g_mu);
    char t[256]; snprintf(t, sizeof t, dm_str(b, "dmUpdBuild"), g_own); set_text(b, bw, t);
    int sl = atomic_load(&g_safe_l), bx = sl + 6 > 79 ? sl + 6 : 79; int dm_padmode_current(void); int pad = dm_padmode_current();
    // 手柄模式（原版手柄主菜单）：左下角是原版的按钮栏和提示条，这两行改放右上角、靠右对齐。原版手柄主菜单面板的矩形是 0×0（锚点无从算起），
    // 这份控件在布局里不带锚点（touch_layout.pad_mainmenu），位置全由这里按屏宽（布局单位：2160 = 屏高）算
    // Controller mode (the original controller main menu): the bottom left holds the original button ribbon and legend, so these two lines go to the top right instead, right-aligned. The original controller main menu panel's rectangle is 0×0 (nothing to compute an anchor from),
    // so these widgets carry no anchor in the layout (touch_layout.pad_mainmenu), and their position is computed entirely here from the screen width (layout units: 2160 = screen height)
    int vw = *(int32_t *)(b + 0x211da38), vh = *(int32_t *)(b + 0x211da3c), xr = vh > 0 ? (int)((long)vw * 2160 / vh) : 3840;
    if (pad) place(bw, xr - 420 - bx, 60); else place(bw, bx, 2046);
  // 触屏：左下角那行，离屏幕左边 79 单位，有安全区时贴安全线内 6 单位
  // Touch: the line at the bottom left, 79 units from the left edge of the screen, or 6 units inside the safe line when there is a safe area
    uint64_t nt = find(mm, "TouchUpdNote", 0), sm = find(mm, "TouchUpdSmall", 0), bg = find(mm, "TouchUpdBg", 0), ti = find(mm, "TouchUpdTitle", 0), de = find(mm, "TouchUpdDesc", 0), big = find(mm, "TouchUpdBig", 0);
    if (!nt || !sm || !bg || !ti || !de || !big) { LOG("【红】更新：主菜单面板里的提示控件不全，不显示提示"); return; }
    int normal = build > 0 && !urgent, urg = build > 0 && urgent, fail = build > 0 ? 0 : atomic_load(&g_fail);
    snprintf(t, sizeof t, dm_str(b, "dmUpdNew"), build, note);
    char ft[192]; if (g_site[0]) snprintf(ft, sizeof ft, dm_str(b, "dmUpdFailSite"), g_site); else snprintf(ft, sizeof ft, "%s", dm_str(b, "dmUpdFail"));
    if (pad && normal) { size_t L = strlen(t); snprintf(t + L, sizeof t - L, "   \xee\x80\x91 %s", dm_str(b, "dmUpdButton")); }   // 手柄：行尾接「Y 键帽 + 更新」（键帽 = 界面字体里的手柄键字形 U+E011，原版提示条用的同一个） | Controller: append "Y keycap + Update" at the end of the line (keycap = the controller button glyph U+E011 in the UI font, the same one the original legend uses)
    set_text(b, nt, normal ? t : fail == 1 ? dm_str(b, "dmUpdNoNet") : fail == 3 ? dm_str(b, "dmUpdService") : fail ? ft : "");
    snprintf(t, sizeof t, dm_str(b, "dmUpdNewUrgent"), build, note); if (pad && urg) { size_t L = strlen(t); snprintf(t + L, sizeof t - L, "   \xee\x80\x91 %s", dm_str(b, "dmUpdButton")); } set_text(b, de, urg ? t : ""); set_text(b, ti, urg ? dm_str(b, "dmUpdUrgentTitle") : "");
    if (pad) { place(bg, urg ? xr / 2 - 700 : 99999, urg ? 1506 : 99999); place(ti, xr / 2 - 700, 1524); place(de, xr / 2 - 700, 1610); }
    else { place(bg, urg ? -700 : 99999, urg ? 1506 : 99999); place(big, urg ? -303 : 99999, urg ? 1712 : 99999); }
    // 提示字在构建号右边 341 单位处起；小「更新」跟在它后面隔 34 单位：文字框开了 fitToText，每帧更新（0xadc8e8）把量出的宽度写回 +0x78
    // The notice text starts 341 units to the right of the build number; the small 「更新」(Update) follows it 34 units behind: the text box has fitToText on, and the per-frame update (0xadc8e8) writes the measured width back to +0x78
    int nw = *(int32_t *)(nt + 0x78);
    if (pad) { place(nt, xr - 2000 - bx, 130); place(sm, 99999, 99999); place(big, 99999, 99999); }   // 手柄：提示字在构建号下面一行；两个「更新」按钮不摆出来（按 Y，见上） | Controller: the hint text goes on the line below the build number; the two 「更新」(Update) buttons are not shown (press Y, see above)
    else { place(nt, bx + 341, 2046); place(sm, normal ? bx + 341 + nw + 34 : 99999, normal ? 2030 : 99999); }
    static dm_wref lw; static int ls = -1, lnw; int st = normal ? 1 : urg ? 2 : fail ? 2 + fail : 0;
    if (dm_wref_get(&lw) != bw || ls != st || (normal && lnw != nw)) { dm_wref_set(&lw, bw); ls = st; lnw = nw; LOG("更新：选角页写上构建 %d%s（提示字宽 %d）", g_own, normal ? "，普通更新提示" : urg ? "，建议立即更新提示" : fail == 1 ? "，查更新失败提示（无线数据许可）" : fail == 3 ? "，更新服务故障提示（稍后重试）" : fail ? "，查更新失败提示（浏览器手动更新）" : "，无提示", normal ? nw : 0); }
}
#if TARGET_OS_OSX
// Mac App：鼠标点主菜单右上角那行「有新版本…」= 按 Y（打开下载页）。Mac 玩家第一反应是点它；这一行在手柄版里是纯文字、没有按钮可点，所以按位置认：
// 提示字的文字框摆在 (屏宽 − 2000 − 边距, 130)、宽 2000、字靠右（见上面 pad 那一支），认它靠右的 1400 单位、上下各放宽到 100–230（布局单位：屏高 = 2160）。
// 条件同 Y 键那条：有普通更新提示、主菜单面板显示着、选角 / 建角 / 设置页没开。游戏主线程调（touch_mac.c 取手柄处）；nx / ny = 画面归一化坐标。返回 1 = 点中并已处理
// Mac App: a mouse click on the "New version..." line at the top right of the main menu = pressing Y (open the download page). A Mac player's first reaction is to click it; in the gamepad layout this line is plain text with no button to click, so it is recognised by position:
// the hint's text box sits at (screen width - 2000 - margin, 130), 2000 wide, right-aligned (see the pad branch above), and the right-hand 1400 units are recognised, widened vertically to 100–230 (layout units: screen height = 2160).
// Same conditions as the Y key: an ordinary update hint exists, the main menu panel is showing, and character selection / creation / settings are not open. Called on the game main thread (where touch_mac.c reads the gamepad); nx / ny = normalised picture coordinates. Returns 1 = hit and handled
int dm_update_click_at(float nx, float ny) {
    if (g_own < 0) return 0; int have, dm_padmode_busy(void); pthread_mutex_lock(&g_mu); have = g_offer.build > 0 && !g_offer.urgent; pthread_mutex_unlock(&g_mu);
    uint8_t *b = tb(); uint64_t pr = b ? *(uint64_t *)(b + 0x2f05458) : 0; if (!have || !pr || dm_padmode_busy()) return 0;   // 不另判是不是手柄版：只有 Mac App 会初始化更新提示（g_own ≥ 0），它一直是手柄版 | no separate check for the gamepad layout: only the Mac App initialises the update hint (g_own >= 0), and it is always the gamepad layout
    uint64_t mm = find(pr, "MainMenuPanel", 0), cs = find(pr, "CharacterSelectPanel", 0), cc = find(pr, "CharacterCreatePanel", 0), st = find(pr, "SettingsPanel", 0);
    if (!mm || !*(uint8_t *)(mm + 0x50) || !find(mm, "TouchBuild", 0) || (cs && *(uint8_t *)(cs + 0x50)) || (cc && *(uint8_t *)(cc + 0x50)) || (st && *(uint8_t *)(st + 0x50))) return 0;
    int vw = *(int32_t *)(b + 0x211da38), vh = *(int32_t *)(b + 0x211da3c), xr = vh > 0 ? (int)((long)vw * 2160 / vh) : 3840, sl = atomic_load(&g_safe_l), bx = sl + 6 > 79 ? sl + 6 : 79;
    float x = nx * xr, y = ny * 2160; if (x < xr - 1400 - bx || x > xr - bx + 20 || y < 100 || y > 230) return 0;
    LOG("更新：鼠标点了主菜单的新版本提示"); if (g_on_click) g_on_click(); return 1;
}
#endif
// 文字框（TextBoxWidget，虚表 main+0x20ba4c8）设文本 = 游戏自己到处内联的两步（如 0x38f5bc–0x38f5cc、0x3d8480–0x3d849c）：
// 字符串赋值 0x136e5c(控件+0x88, 字节, 字节数)，再 0xadc434(控件) 刷新省略号截断串。绘制每帧现取 +0x88 现量现画，不用另调重排。不是文字框就不动
// Setting text on a text box (TextBoxWidget, vtable main+0x20ba4c8) = the two steps the game itself inlines everywhere (e.g. 0x38f5bc-0x38f5cc, 0x3d8480–0x3d849c):
// string assign 0x136e5c(widget+0x88, bytes, byte count), then 0xadc434(widget) to refresh the ellipsis-truncated string. Drawing fetches +0x88, measures and draws fresh every frame, so no separate re-layout call is needed. If it is not a text box, leave it alone
static void set_text(uint8_t *b, uint64_t w, const char *s) {
    if (*(uint64_t *)w - (uint64_t)(uintptr_t)b != 0x20ba4c8) return;
    size_t n = strlen(s); if (*(uint64_t *)(w + 0x90) == n && !memcmp(*(const char **)(w + 0x88), s, n)) return;
    ((void *(*)(uint64_t, const char *, uint64_t))(b + 0x136e5c))(w + 0x88, s, n); ((void (*)(uint64_t))(b + 0xadc434))(w);
}
