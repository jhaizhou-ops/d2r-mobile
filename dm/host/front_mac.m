// Mac 前端：DM_WINDOW=1 时开窗口（CAMetalLayer，供图形后端出图）+ 键盘 → dm_input；客体改在大栈线程上跑，主线程归 AppKit。
// 不开 DM_WINDOW：客体直接在当前线程跑，行为与无前端时相同（无窗口测试用这个）。
// 键位（任天堂布局）：WASD 左摇杆、方向键十字键、J=A K=B U=X I=Y、Q=L E=R、Z=ZL C=ZR、F=按左摇杆 G=按右摇杆、回车=+ Tab=−。
// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// Key map (Nintendo layout): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick, Return=+ Tab=−.
#import <Cocoa/Cocoa.h>
#include <sys/sysctl.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include "dm.h"
#include "dm_input.h"
#include "dm_uiscreen.h"
void mtl_set_ui_rect_fn(void (*fn)(double *y, double *h));
static void ui_rect_frac(double *fy, double *fh) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); if (dm_screen_real(&rw, &rh) && rh > 0) { *fy = (double)y / rh; *fh = (double)h / rh; } }   // 同 front_ios.m：界面矩形的纵向位置与高（占画面高的比例） | Same as front_ios.m: vertical position and height of the UI rectangle (as fractions of the frame height)
__attribute__((constructor)) static void ui_shift_reg(void) { mtl_set_ui_rect_fn(ui_rect_frac); }   // 不开窗口的跑法（截图 / 闸门）也要生效，所以不放在建图层那里 | Must also take effect when running without a window (screenshots / gate checks), so it is not placed where the layer is created
#include "front.h"
#include "hle.h"
#include <signal.h>

void dm_touch_host_live_enable(void); void dm_touch_host_live(int op, float nx, float ny);   // touch_mac.c
static int g_live;   // App 形态（或 DM_TOUCH_LIVE=1）：鼠标进我们的触屏状态机；否则照旧当原版触摸屏的一个触点 | App form (or DM_TOUCH_LIVE=1): the mouse goes into our touch state machine; otherwise it stays a single touch point of the original touch screen as before
static CAMetalLayer *g_layer;
static NSWindow *g_win;
static NSView *g_view;
// 真全屏（App 形态，屏幕比 16:9 窄——16:10 的 MacBook，「不能全屏」：全屏后上下各 56 点黑边）：照 iPad 那套（front_ios.m、dm_uiscreen.c）——
// 启动时按屏幕比例（全屏时的内容区：屏幕扣掉刘海安全区）定游戏分辨率，局内 3D 铺满、界面按 16:9 贴底（开大面板居中），前端各页与读取画面游戏自己按 16:9 出、上下留边。
// 窗口锁成同一比例，窗口态与全屏态都铺满。屏幕不比 16:9 窄（外接 16:9 / 更宽的屏）：照旧全程 16:9。比例在启动时按主屏定，之后挪到别的屏不重算（游戏分辨率只在启动时定）。
// 测试开关 DM_TEST_SCREEN_ASPECT=<宽>x<高>：当作主屏是这个比例（16x9 = 外接 16:9 屏的口径）
// True full screen (App form, screen narrower than 16:9 - a 16:10 MacBook, "cannot go full screen": 56 points of black bar above and below in full screen): the iPad approach (front_ios.m, dm_uiscreen.c) -
// at launch the game resolution is set from the screen's aspect ratio (the full-screen content area: the screen minus the notch safe area); in game the 3D fills the screen, the interface is 16:9 along the bottom (centred with a large panel open), and the front-end pages and the loading screen are drawn by the game at 16:9 with bars above and below.
// The window is locked to the same ratio, so both windowed and full screen are filled. Screens not narrower than 16:9 (external 16:9 / wider screens): 16:9 throughout as before. The ratio is set from the main screen at launch and not recomputed when moved to another screen (the game resolution is only set at launch).
// Test switch DM_TEST_SCREEN_ASPECT=<w>x<h>: treat the main screen as this ratio (16x9 = the external 16:9 screen case)
static double g_aspect = 16.0 / 9;   // 游戏画面比例（宽 ÷ 高） | aspect ratio of the game picture (width / height)
static int g_full;                    // 1 = 真全屏已开 | 1 = true full screen is on
static void full_setup(void) {
    if (!getenv("DM_APP") || getenv("DM_GAME_RES")) return;
    NSScreen *sc = NSScreen.mainScreen; double w = sc.frame.size.width, h = sc.frame.size.height - sc.safeAreaInsets.top; const char *e = getenv("DM_TEST_SCREEN_ASPECT"); unsigned tw, th;
    if (e && sscanf(e, "%ux%u", &tw, &th) == 2 && tw && th) { w = tw; h = th; }
    if (h <= 0 || w / h >= 16.0 / 9 - 0.01) { LOG("画面：主屏 %.0fx%.0f 不比 16:9 窄 → 全程 16:9", w, h); return; }
    g_aspect = w / h; g_full = 1; dm_uiscreen_enable(DM_UISCREEN_BOTTOM);
    LOG("画面：主屏全屏内容区 %.0fx%.0f（比例 %.3f）比 16:9 窄 → 真全屏：窗口锁这个比例，局内铺满、界面贴底，前端 16:9 留边", w, h, g_aspect);
}
// 游戏画面在视图里的位置：按游戏画面比例放进视图（比例对得上就是整个视图；对不上的四周留黑边）
// Where the game picture sits in the view: fitted into the view by the picture's aspect ratio (if the ratios match, the whole view; otherwise black bars around)
static NSRect game_rect(void) {
    NSSize b = g_view.bounds.size; CGFloat w = b.width, h = b.height;
    if (fabs(w / h / g_aspect - 1) < 0.003) { }   // 比例差 0.3% 以内（窗口高取整的零头、全屏内容区比按刘海安全区算的少 1 点）当对得上，铺满 | within 0.3% (the remainder from rounding the window height, and the full-screen content area being 1 point less than computed from the notch safe area) counts as matching, and it fills
    else if (w > h * g_aspect) w = round(h * g_aspect); else h = round(w / g_aspect);
    return NSMakeRect(floor((b.width - w) / 2), floor((b.height - h) / 2), w, h);
}
static void layout_game(void) {
    NSRect r = game_rect(); if (NSEqualRects(r, g_layer.frame)) return;
    CGFloat sc = g_view.window.backingScaleFactor ?: 2;   // 画布跟着改：作为子层的 CAMetalLayer 改 frame 不会自己改 drawableSize，原先进全屏后一直用窗口态的画布放大显示（发糊） | the canvas follows: a CAMetalLayer used as a sublayer does not change its drawableSize when its frame changes, so after going full screen it used to keep the windowed canvas and scale it up (blurry)
    [CATransaction begin]; [CATransaction setDisableActions:YES]; g_layer.frame = r; g_layer.contentsScale = sc; g_layer.drawableSize = CGSizeMake(round(r.size.width * sc), round(r.size.height * sc)); [CATransaction commit];
    LOG("窗口：内容区 %.0f×%.0f，游戏画面 %.0f×%.0f 放在 (%.0f,%.0f)", g_view.bounds.size.width, g_view.bounds.size.height, r.size.width, r.size.height, r.origin.x, r.origin.y);
}
// 退出（关窗口 / ⌘Q / 菜单）：有局在就先请游戏走一次它自己的存盘并等存档提交（做法同 iOS 壳进后台），最多 3 秒，再有序收尾。
// 暂停菜单 / 大秘境面板开着时游戏停拍：冻住世界、临时放开暂停存这一次（diag.c dm_save_wait，与 iOS 进后台同一个函数）；读取画面里等它进到局内再请。3 秒到了还没有提交就照实记「未存盘退出」
// Quit (close window / Cmd-Q / menu): if a game is running, first ask the game to do one of its own saves and wait for the save commit (same as the iOS shell going to the background), at most 3 s, then shut down in order.
// When the pause menu / Greater Rift panel is open the game does not tick: freeze the world and temporarily release the pause for this one save (diag.c dm_save_wait, the same function as iOS going to the background); on the loading screen wait until in game before asking. If there is still no commit after 3 s, log "quit without saving" as is
static void app_quit(const char *why) {
    static int once; if (once++) return;
    { uint64_t dm_audio_render_calls(void); LOG("窗口：退出（%s）；声卡回调累计 %llu（0 = 这次没出声）", why, (unsigned long long)dm_audio_render_calls()); }
    int dm_ingame_now(void); int dm_save_wait(double max_s, uint64_t *commits, int *froze, double *ms);
    if (!dm_ingame_now() && !dm_uiscreen_ingame()) { LOG("窗口：退出（不在局内，没有待存的进度）"); dm_host_shutdown(why); return; }
    uint64_t n = 0; int froze = 0; double ms = 0; int asked = dm_save_wait(3, &n, &froze, &ms);
    if (n && asked) LOG("窗口：退出前等游戏存盘 %.0f ms，期间存档提交 %llu 次%s", ms, (unsigned long long)n, froze ? "（原先暂停着：冻结世界存盘、已恢复暂停）" : "");
    else LOG("【红】窗口：未存盘退出——3 秒内没等到存档提交（%s%s）", asked ? "已请游戏存盘" : "一直没到能请存盘的状态（读取画面？）", froze ? "，原先暂停着、冻结放开过" : "");
    { int dm_cloud_flush_wait(double sec); dm_cloud_flush_wait(10); }   // 存盘之后把这一局传上 iCloud 再退（没开同步时立即返回） | after saving, upload this session to iCloud before quitting (returns immediately when sync is off)
    dm_host_shutdown(why);
}
static const char *ui(const char *k) { const char *dm_ui(const char *key); return dm_ui(k); }
static void show_keys(void) { NSAlert *a = [NSAlert new]; a.messageText = @(ui("dmAppKeysTitle")); a.informativeText = @(ui("dmAppKeysText")); [a addButtonWithTitle:@(ui("dmAppOK"))]; [a runModal]; }
// 帮助 → 分享日志…：把本次与上一次的日志（~/Library/Logs/D2R Mobile/host.log、host.prev.log）和一份版本信息打成一个压缩包，弹系统的存储面板让玩家选位置（缺省桌面），存好后在访达里选中它。
// 不上传、不发邮件；包里只有这三份文件——不放存档、不放 iCloud 目录里的任何东西。日志里的家目录前缀一律换成 ~（同 iOS 的分享日志 dm_sharelog.m scrub_home：不把用户名带出去）。
// to 非空 = 测试（DM_TEST_SHARELOG）：不弹面板，直接写到这个路径。返回 nil 成功，否则是失败原因
// Help -> Share Log...: pack the logs of this run and the previous one (~/Library/Logs/D2R Mobile/host.log, host.prev.log) and a version-information file into one archive, show the system save panel for the player to choose a location (Desktop by default), and select it in Finder once saved.
// Nothing is uploaded or mailed; the archive contains only these three files - no saves, nothing from the iCloud directory. The home-directory prefix in the logs is always replaced with ~ (same as the iOS share-log dm_sharelog.m scrub_home: the user name is not carried out).
// to non-empty = test (DM_TEST_SHARELOG): no panel, write directly to this path. Returns nil on success, otherwise the reason for failure
static NSString *share_logs(NSString *to) {
    NSFileManager *fm = NSFileManager.defaultManager; NSString *home = NSHomeDirectory(), *logd = [home stringByAppendingPathComponent:@"Library/Logs/D2R Mobile"];
    NSDictionary *inf = NSBundle.mainBundle.infoDictionary; NSString *build = [inf[@"CFBundleVersion"] description] ?: @"0", *ver = [inf[@"CFBundleShortVersionString"] description] ?: @"?";
    NSDateFormatter *df = [NSDateFormatter new]; df.dateFormat = @"yyyyMMdd-HHmm"; NSString *name = [NSString stringWithFormat:@"D2RMobile-logs-%@-%@", build, [df stringFromDate:NSDate.date]];
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"dm_share_%d", getpid()]], *dir = [tmp stringByAppendingPathComponent:name];
    [fm removeItemAtPath:tmp error:nil]; if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) return @"mkdir";
    fflush(stderr);
    NSData *hb = [home dataUsingEncoding:NSUTF8StringEncoding]; NSData *tilde = [@"~" dataUsingEncoding:NSUTF8StringEncoding];
    NSData *(^scrub)(NSData *) = ^NSData *(NSData *d) { if (!d || hb.length < 2) return d; NSMutableData *o = [NSMutableData dataWithCapacity:d.length]; NSRange all = NSMakeRange(0, d.length), r;
        while ((r = [d rangeOfData:hb options:0 range:all]).location != NSNotFound) { [o appendData:[d subdataWithRange:NSMakeRange(all.location, r.location - all.location)]]; [o appendData:tilde]; all = NSMakeRange(NSMaxRange(r), d.length - NSMaxRange(r)); }
        [o appendData:[d subdataWithRange:all]]; return o; };
    int n = 0; for (NSString *f in @[@"host.log", @"host.prev.log"]) { NSData *d = [NSData dataWithContentsOfFile:[logd stringByAppendingPathComponent:f] options:NSDataReadingMappedIfSafe error:nil]; if (d) { [scrub(d) writeToFile:[dir stringByAppendingPathComponent:f] atomically:YES]; n++; } }
    NSString *bt = [NSString stringWithContentsOfFile:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"build.txt"] encoding:NSUTF8StringEncoding error:nil] ?: @"";
    char hw[64] = "?"; size_t hn = sizeof hw; sysctlbyname("hw.model", hw, &hn, NULL, 0); NSProcessInfo *pi = NSProcessInfo.processInfo;
    NSString *info = [NSString stringWithFormat:@"%@\nD2R Mobile (macOS) %@ (%@)\n%@\n%@\nmacOS %@\n%@  %.0f GB\n%@\n", @(ui("dmAppShareInfo")), ver, build, inf[@"CFBundleIdentifier"] ?: @"", [bt stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet],
        pi.operatingSystemVersionString, @(hw), pi.physicalMemory / 1073741824.0, [NSLocale.preferredLanguages componentsJoinedByString:@","]];
    [[scrub([info dataUsingEncoding:NSUTF8StringEncoding]) copy] writeToFile:[dir stringByAppendingPathComponent:@"info.txt"] atomically:YES];
    NSString *zip = [tmp stringByAppendingPathComponent:[name stringByAppendingString:@".zip"]];
    NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/ditto"]; t.arguments = @[@"-c", @"-k", @"--keepParent", dir, zip]; NSError *err = nil;
    if (![t launchAndReturnError:&err]) { [fm removeItemAtPath:tmp error:nil]; return err.localizedDescription ?: @"ditto"; } [t waitUntilExit];
    if (t.terminationStatus != 0 || ![fm fileExistsAtPath:zip]) { [fm removeItemAtPath:tmp error:nil]; return [NSString stringWithFormat:@"ditto %d", t.terminationStatus]; }
    NSURL *dst = to ? [NSURL fileURLWithPath:to] : nil;
    if (!dst) { NSSavePanel *sp = [NSSavePanel savePanel]; sp.nameFieldStringValue = zip.lastPathComponent; sp.directoryURL = [fm URLsForDirectory:NSDesktopDirectory inDomains:NSUserDomainMask].firstObject; sp.canCreateDirectories = YES;
        if ([sp runModal] != NSModalResponseOK) { [fm removeItemAtPath:tmp error:nil]; LOG("分享日志：玩家取消"); return nil; } dst = sp.URL; }
    [fm removeItemAtURL:dst error:nil]; BOOL ok = [fm moveItemAtPath:zip toPath:dst.path error:&err]; [fm removeItemAtPath:tmp error:nil];
    if (!ok) return err.localizedDescription ?: @"move";
    LOG("分享日志：已存（日志 %d 份 + 版本信息，%llu 字节）", n, [[fm attributesOfItemAtPath:dst.path error:nil] fileSize]);
    if (!to) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[dst]];
    return nil;
}
static void share_logs_ui(void) { NSString *e = share_logs(nil); if (e) { LOG("【红】分享日志：打包失败 %s", e.UTF8String); NSAlert *a = [NSAlert new]; a.messageText = @(ui("dmAppShareFail")); a.informativeText = e; [a addButtonWithTitle:@(ui("dmAppOK"))]; [a runModal]; } }
@interface DmWinCtl : NSObject <NSWindowDelegate> @end
// 窗口所在那块屏的名字与刷新率（主线程读 AppKit，存一份给出图线程）：出图节拍按它选整除的帧率（nvn_mtl.m mtl_pace_hz）。
// 窗口换屏、屏幕参数变（改刷新率、接拔显示器）时重读。刷新率取显示模式的值，内建屏读不到（0）时取屏幕的最高帧率
// Name and refresh rate of the screen the window is on (read from AppKit on the main thread, a copy kept for the present thread): present pacing picks an evenly dividing frame rate from it (nvn_mtl.m mtl_pace_hz).
// Re-read when the window moves to another screen or screen parameters change (refresh rate change, display plugged / unplugged). The refresh rate comes from the display mode; when the built-in screen gives none (0), the screen's maximum frame rate is used
static pthread_mutex_t g_scr_mu = PTHREAD_MUTEX_INITIALIZER; static double g_scr_hz; static char g_scr_name[128];
static void screen_note(void) {
    NSScreen *sc = g_win.screen ?: NSScreen.mainScreen; if (!sc) return;
    CGDirectDisplayID id_ = [sc.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(id_); double hz = m ? CGDisplayModeGetRefreshRate(m) : 0; if (m) CGDisplayModeRelease(m);
    if (hz <= 0) hz = (double)sc.maximumFramesPerSecond;
    pthread_mutex_lock(&g_scr_mu); int same = hz == g_scr_hz && !strcmp(g_scr_name, sc.localizedName.UTF8String ?: "");
    g_scr_hz = hz; snprintf(g_scr_name, sizeof g_scr_name, "%s", sc.localizedName.UTF8String ?: "?"); pthread_mutex_unlock(&g_scr_mu);
    if (!same) LOG("屏幕：%s，刷新率 %.2f Hz（最高 %ld 帧）", g_scr_name, hz, (long)sc.maximumFramesPerSecond);
}
double front_display_refresh(char *name, size_t n) {
    pthread_mutex_lock(&g_scr_mu); double hz = g_scr_hz; if (name && n) snprintf(name, n, "%s", g_scr_name); pthread_mutex_unlock(&g_scr_mu); return hz;
}
@implementation DmWinCtl
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w; app_quit("关窗口"); return NO; }
- (void)windowDidResize:(NSNotification *)n { (void)n; layout_game(); }
- (void)windowDidChangeBackingProperties:(NSNotification *)n { (void)n; layout_game(); }
- (void)windowDidChangeScreen:(NSNotification *)n { (void)n; screen_note(); }
- (void)quit:(id)s { (void)s; app_quit("菜单退出"); }
- (void)keys:(id)s { (void)s; show_keys(); }
- (void)share:(id)s { (void)s; share_logs_ui(); }
@end
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
// 原版 16:9 常数，行为不变；DM_GAME_RES=宽x高（主机模式，掌机按 720/900 同比）只供与 iOS 宽屏对拍
// The original game's 16:9 constants, behavior unchanged; DM_GAME_RES=<width>x<height> (docked mode; handheld scaled by 720/900) is only for side-by-side comparison with iOS widescreen
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    if (g_full) return docked ? (uint64_t)(llround(1600 / g_aspect / 2) * 2) << 32 | 1600 : 720ull << 32 | (uint64_t)(llround(720 * g_aspect / 8) * 8);   // 真全屏：宽 1600、高按屏幕比例（16:10 → 1600x1004） | true full screen: width 1600, height by the screen's aspect ratio (16:10 -> 1600x1004)
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}

static NSPoint g_test_last;   // 合成鼠标脚本：上一条带坐标的指令落在视图里的点（dragto 从这里起拖） | synthetic mouse script: the point in the view where the last instruction with coordinates landed (dragto starts dragging from here)
static int g_wheel;   // 滚轮：+1 / −1 = 右摇杆向上 / 向下推一小会儿（原版手柄版用右摇杆滚列表） | Wheel: +1 / -1 = push the right stick up / down for a short while (the original gamepad UI scrolls lists with the right stick)
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    dm_input_set(g_btn, lx, ly, 0, g_wheel * 32767);
}
static int key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → 按摇杆 | F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return g_live ? 40 : 10; case 48: return 11;                            // 回车 Tab → + −（App 形态：回车交给 touch_mac.c 按当前页决定是 ＋ 还是确认，见那里） | Return Tab -> + - (App form: Return is handed to touch_mac.c, which decides by the current page whether it is + or confirm, see there)
        case 24: case 69: return 10;                                                     // = 键（＋ 那个键）、小键盘 + → ＋ | the = key (the one with +) and keypad + -> +
        case 49: return 0;                                                               // 空格 → A（确认） | Space -> A (confirm)
        case 53: return g_live ? 41 : -1;                                                // Esc → 返回（App 形态；局内没有面板时是开功能页，touch_mac.c） | Esc -> back (App form; with no panel open in game it opens the function page, touch_mac.c)
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
static void on_key(NSEvent *e, int down) {
    unsigned short c = e.keyCode;
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    int b = key_bit(c);
    if (down && (b >= 0 || c == 13 || c <= 2)) { void dm_padmode_request_kbd(int kbd); dm_padmode_request_kbd(1); }   // 按了游戏用的键：键位图标换成键盘键帽（鼠标移动 / 点击不算） | a key used by the game was pressed: switch the key icons to keyboard key caps (mouse moves / clicks do not count)
    if (b >= 0) g_btn = down ? (g_btn | 1ull << b) : (g_btn & ~(1ull << b));
    push();
}

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];                       // 信号备用栈是每线程的；崩溃打印要在客体线程上也生效 | The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("客体线程结束"); exit(0);
}

void front_run(void (*guest)(void)) {
    if (!getenv("DM_WINDOW")) { guest(); return; }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        full_setup();
        NSRect r = NSMakeRect(0, 0, 1280, floor(1280 / g_aspect));
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @"D2R Mobile"; win.restorable = NO; g_win = win;
        NSView *v = win.contentView; g_view = v;
        v.wantsLayer = YES; v.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary; win.contentMinSize = NSMakeSize(640, floor(640 / g_aspect)); if (g_full) win.contentAspectRatio = NSMakeSize(1280, 1280 / g_aspect);
        static DmWinCtl *ctl; ctl = [DmWinCtl new]; win.delegate = ctl;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        [v.layer addSublayer:g_layer]; layout_game();
        [win center];
        g_live = getenv("DM_APP") || getenv("DM_TOUCH_LIVE"); if (g_live) dm_touch_host_live_enable();
        if (g_live) {
            win.acceptsMouseMovedEvents = YES;
            [v addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect owner:v userInfo:nil]];
        }
        { // 菜单栏：应用（退出 ⌘Q）/ 显示（全屏 ⌃⌘F）/ 帮助（键位对照） | Menu bar: application (Quit ⌘Q) / View (Full Screen ⌃⌘F) / Help (key mapping)
          NSMenu *bar = [NSMenu new];
          NSMenu *(^add)(const char *) = ^NSMenu *(const char *t) { NSMenuItem *it = [NSMenuItem new]; [bar addItem:it]; NSMenu *m = [[NSMenu alloc] initWithTitle:@(t)]; it.submenu = m; return m; };
          [[add("") addItemWithTitle:@(ui("dmAppQuit")) action:@selector(quit:) keyEquivalent:@"q"] setTarget:ctl];
          NSMenuItem *fs = [add(ui("dmAppMenuView")) addItemWithTitle:@(ui("dmAppFullscreen")) action:@selector(toggleFullScreen:) keyEquivalent:@"f"]; fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
          { NSMenu *hm = add(ui("dmAppMenuHelp")); [[hm addItemWithTitle:@(ui("dmAppKeysTitle")) action:@selector(keys:) keyEquivalent:@""] setTarget:ctl]; [[hm addItemWithTitle:@(ui("dmAppShareLog")) action:@selector(share:) keyEquivalent:@""] setTarget:ctl]; }
          NSApp.mainMenu = bar; }
        { void dm_gamepad_start(void (*on_mode)(int pad)); dm_gamepad_start(NULL); }   // 手柄（与 iOS 壳共用 ios/Sources/dm_gamepad.m）：连上即用，与键盘并存 | Gamepad (ios/Sources/dm_gamepad.m, shared with the iOS shell): works as soon as it is connected, alongside the keyboard
        [win makeKeyAndOrderFront:nil];
        screen_note();
        [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; screen_note(); }];
        [NSApp activateIgnoringOtherApps:YES];
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // 只吃游戏窗口的键；软键盘输入框等其他窗口照常收字 | Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual
            if (e.modifierFlags & NSEventModifierFlagCommand) return e;   // 带 ⌘ 的留给菜单（⌘Q、⌃⌘F） | Keys with ⌘ are left to the menu (⌘Q, ⌃⌘F)
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // 吃掉，免得系统提示音 | Swallow it to avoid the system beep
        }];
        // 左键沿用单点触屏；Mac App 的无按键悬停只交坐标，复用同一套窗口 / 全屏换算，不合成点击。
        // Left clicks reuse single-point touch; Mac App button-free hover only submits coordinates using the same window / full-screen conversion, without synthesizing clicks.
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp | NSEventMaskMouseMoved | NSEventMaskMouseExited handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;
            BOOL hover = e.type == NSEventTypeMouseMoved || e.type == NSEventTypeMouseExited;
            if (hover && getenv("DM_TEST_HOVER_LOG")) LOG("鼠标悬停输入：类型 %ld", (long)e.type);
            if (hover && !g_live) return e;
            if (e.type == NSEventTypeMouseExited) { dm_touch_host_live(3, -1, -1); return e; }
            if (e.type == NSEventTypeLeftMouseUp) { if (g_live) dm_touch_host_live(2, 0, 0); else dm_touch_set(NULL, 0); return nil; }
            NSPoint p = [v convertPoint:e.locationInWindow fromView:nil]; NSRect gr = game_rect();
            if (hover && getenv("DM_TEST_HOVER_LOG")) LOG("鼠标悬停坐标：窗口 %.1f,%.1f 视图 %.1f,%.1f 画面 %.1f,%.1f %.1fx%.1f", e.locationInWindow.x, e.locationInWindow.y, p.x, p.y, gr.origin.x, gr.origin.y, gr.size.width, gr.size.height);
            if (gr.size.width <= 0 || gr.size.height <= 0) return e;
            float nx = (float)((p.x - gr.origin.x) / gr.size.width), ny = (float)(1.0 - (p.y - gr.origin.y) / gr.size.height);   // AppKit 视图原点在左下，触屏原点在左上 | The AppKit view origin is at bottom left, the touchscreen origin is at top left
            if (e.type == NSEventTypeLeftMouseDown && (nx < 0 || nx > 1 || ny < 0 || ny > 1)) return nil;   // 按在黑边上：不算点 | Pressed on the black bars: not a click
            if (g_full && g_live) {   // 真全屏：状态机吃界面矩形里的归一化坐标（同 iOS 的游戏视图）。两步： | true full screen: the state machine takes normalised coordinates inside the interface rectangle (like the iOS game view). Two steps:
                // ① 先换到「实际显示的那块」：游戏当前分辨率（前端 16:9、局内整屏）按比例居中放进游戏画面，与上屏同一口径（nvn_mtl.m present_encode：比例差 1% 内铺满）；
                // ② 再换到界面矩形（局内贴底）。局内界面矩形外是 3D 世界，带符号交下去（点世界换回整屏，touch_mac.c）；不在局内时显示块外是黑边，不算点
                // (1) First map to "the block actually shown": the game's current resolution (16:9 in the front end, the whole screen in game) placed centred in the game picture by aspect ratio, the same measure as presentation (nvn_mtl.m present_encode: fill when the aspect ratios differ by less than 1%);
                // (2) then map to the interface rectangle (along the bottom in game). In game, outside the interface rectangle is the 3D world, passed down with a sign (tapping the world maps back to the whole screen, touch_mac.c); outside the game the area beyond the shown block is the black bars, not counted as a tap
                int rw = 0, rh = 0; double cx = 0, cy = 0, cw = 1, ch = 1;
                if (dm_screen_real(&rw, &rh) && rw > 0 && rh > 0) { double ca = (double)rw / rh, ga = gr.size.width / gr.size.height;
                    if (fabs(ga / ca - 1) >= 0.01) { if (ga > ca) { cw = ca / ga; cx = (1 - cw) / 2; } else { ch = ga / ca; cy = (1 - ch) / 2; } } }
                float vx = (float)((nx - cx) / cw), vy = (float)((ny - cy) / ch);
                double fy = 0, fh = 1; ui_rect_frac(&fy, &fh); float uy = fh > 0 ? (float)((vy - fy) / fh) : vy;
                if (e.type == NSEventTypeLeftMouseDown && !dm_uiscreen_ingame() && (vx < 0 || vx > 1 || uy < 0 || uy > 1)) return nil;
                dm_touch_host_live(hover ? 3 : e.type == NSEventTypeLeftMouseDown ? 0 : 1, vx, uy); return hover ? e : nil; }
            if (hover) { dm_touch_host_live(3, nx, ny); return e; }
            dm_touch t = {0, nx < 0 ? 0 : nx > 1 ? 1 : nx, ny < 0 ? 0 : ny > 1 ? 1 : ny};
            if (g_live) { dm_touch_host_live(e.type == NSEventTypeLeftMouseDown ? 0 : 1, t.x, t.y); return nil; }
            dm_touch_set(&t, 1); return nil;
        }];
        // 右键 = 手柄 B（返回 / 关面板）；滚轮 = 右摇杆上下推 0.12 秒（滚列表）。只在 App 形态接（命令行宿主的测试不受影响）
        // Right click = gamepad B (back / close panel); wheel = push the right stick up or down for 0.12 s (scroll lists). Only wired in App form (tests of the command-line host are unaffected)
        if (g_live) {
            [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskRightMouseDown | NSEventMaskRightMouseUp handler:^NSEvent *(NSEvent *e) {
                if (e.window != win) return e;
                if (e.type == NSEventTypeRightMouseDown) g_btn |= DM_BTN_B; else g_btn &= ~(uint64_t)DM_BTN_B;
                push(); return nil;
            }];
            [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel handler:^NSEvent *(NSEvent *e) {
                if (e.window != win || e.scrollingDeltaY == 0) return e;
                static int gen; int my = ++gen; g_wheel = e.scrollingDeltaY > 0 ? 1 : -1; push();
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.12 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ if (my == gen) { g_wheel = 0; push(); } });
                return nil;
            }];
        }
        if (getenv("DM_APP") && !getenv("DM_APP_ROMFS") && ![NSUserDefaults.standardUserDefaults boolForKey:@"KeysShown"]) { show_keys(); [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"KeysShown"]; }   // 第一次启动给一次键位对照（界面上的按键提示是手柄字母）；以后在「帮助」菜单里 | Show the key mapping once on first launch (button prompts on screen are gamepad letters); afterwards it is in the Help menu
        { const char *e;   // 测试（没人点窗口时用）：DM_TEST_WINSIZE=宽x高 设内容区大小；DM_TEST_FULLSCREEN_AT=<秒> 进全屏；DM_TEST_QUIT_AT=<秒> 走一次退出 | Tests (for when nobody is there to click the window): DM_TEST_WINSIZE=WxH sets the content size; DM_TEST_FULLSCREEN_AT=<seconds> enters full screen; DM_TEST_QUIT_AT=<seconds> goes through one quit
          int w, h; if ((e = getenv("DM_TEST_WINSIZE")) && sscanf(e, "%dx%d", &w, &h) == 2) { [win setContentSize:NSMakeSize(w, h)]; layout_game(); }
          if ((e = getenv("DM_TEST_FULLSCREEN_AT"))) for (const char *q = e; q && *q; q = strchr(q, ';') ? strchr(q, ';') + 1 : NULL)   // 「秒[;秒…]」：每个时刻切一次全屏（来回切） | "seconds[;seconds...]": toggle full screen at each time (back and forth)
              dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(q) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [win toggleFullScreen:nil]; });
          // DM_TEST_PRESENT_SHOT=<秒>:<路径>[;<秒>:<路径>…]：到点取下一次上屏的画面（图形层与上屏同一套编码，见 nvn_mtl.m mtl_grab_next_present）写成 PNG——
          // 看的是玩家真看到的（含留边位置），不是游戏出图贴图（DM_NVN_SHOT_UI 取的那张，裁切区不居中）
          // DM_TEST_PRESENT_SHOT=<seconds>:<path>[;<seconds>:<path>...]: at the given time, grab the next presented picture (the graphics layer uses the same encoding as presentation, see nvn_mtl.m mtl_grab_next_present) and write it as PNG -
          // this is what the player actually sees (including where the bars are), not the game's output texture (the one DM_NVN_SHOT_UI takes, whose crop is not centred)
          if ((e = getenv("DM_TEST_PRESENT_SHOT"))) { void mtl_grab_next_present(void (^cb)(CGImageRef img));
              for (NSString *it in [@(e) componentsSeparatedByString:@";"]) { NSRange c = [it rangeOfString:@":"]; if (c.location == NSNotFound) continue;
                  double at = [it substringToIndex:c.location].doubleValue; NSString *path = [it substringFromIndex:c.location + 1];
                  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(at * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                      NSSize vb = v.bounds.size; NSRect gr = game_rect(); double fy = 0, fh = 1; ui_rect_frac(&fy, &fh);
                      mtl_grab_next_present(^(CGImageRef img) { if (!img) { LOG("【红】DM_TEST_PRESENT_SHOT：%s 没取到", path.UTF8String); return; }
                          CGImageDestinationRef d = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], (__bridge CFStringRef)@"public.png", 1, NULL);
                          if (d) { CGImageDestinationAddImage(d, img, NULL); CGImageDestinationFinalize(d); CFRelease(d); }
                          LOG("DM_TEST_PRESENT_SHOT：%s %zux%zu（内容区 %.0fx%.0f，游戏画面 %.0fx%.0f 在 (%.0f,%.0f)，界面矩形纵向 %.3f 高 %.3f，%s）", path.UTF8String, CGImageGetWidth(img), CGImageGetHeight(img),
                              vb.width, vb.height, gr.size.width, gr.size.height, gr.origin.x, gr.origin.y, fy, fh, dm_uiscreen_ingame() ? "局内" : "局外"); }); }); } }
          // DM_TEST_NSMOUSE=<文件>：合成真正的系统鼠标事件（NSEvent）投进 App 的事件队列，走与真人点击同一条分发路（事件监视器 → 换算 → 状态机），不经测试入口。
          // 每行「秒 down|move|up|hover x,y」，x,y 是内容区里的点（左上角为原点；窗口缺省 1280×720），时刻自窗口建好起算。
          // 坐标后可带锚点「@c」「@b」：按 1280×720 写的 y 换算到实际内容区高度——@c 上下居中的界面（前端页、大面板）加 (高 − 720) / 2，@b 贴底的（局内 HUD）加 (高 − 720)；
          // 不带 = 原样（贴顶）。真全屏下窗口比例跟着屏幕（16:10 屏 1280×803），同一份脚本两种比例都能用
          // DM_TEST_NSMOUSE=<file>: synthesizes real system mouse events (NSEvent) and posts them into the App's event queue, taking the same dispatch path as a real click (event monitor -> conversion -> state machine), not a test entry.
          // Each line is "seconds down|move|up|hover x,y", where x,y is a point in the content area (origin at top-left; window default 1280x720), timed from when the window is created.
          // An anchor "@c" / "@b" may follow the coordinates: a y written for 1280x720 is converted to the actual content height - @c for vertically centered UI (front-end pages, large panels) adds (height - 720) / 2, @b for bottom-anchored UI (in-game HUD) adds (height - 720);
          // none = as is (top-anchored). In true full screen the window aspect follows the screen (1280x803 on a 16:10 screen), so the same script works at both aspects
          if ((e = getenv("DM_TEST_NSMOUSE"))) { NSString *txt = [NSString stringWithContentsOfFile:@(e) encoding:NSUTF8StringEncoding error:nil]; NSTimeInterval t0 = NSProcessInfo.processInfo.systemUptime; int n = 0;
              dispatch_queue_t tq = dispatch_queue_create("dm.test.nsevent", DISPATCH_QUEUE_SERIAL);   // 不排在主队列上：我们自己的弹框是在主队列的一个块里跑模态循环的，那期间主队列上别的块不执行，事件就投不进弹框 | not queued on the main queue: our own dialog runs its modal loop inside a block on the main queue, during which other blocks on the main queue do not execute, so events could not be delivered into the dialog
              for (NSString *ln in [txt componentsSeparatedByString:@"\n"]) { double sec, x = 0, y = 0; char op[8];
                  if (sscanf(ln.UTF8String, "%lf %7s %lf,%lf", &sec, op, &x, &y) < 2 || ln.UTF8String[0] == '#') continue; n++;
                  const char *at = strchr(ln.UTF8String, '@'); double anc = at && at[1] == 'c' ? 0.5 : at && at[1] == 'b' ? 1 : 0;   // 锚点（见上）：在投事件那一刻按当时的内容区高度换算 | Anchor (see above): converted using the content area height at the moment the event is posted
                  if (!strcmp(op, "key") || !strcmp(op, "type")) {   // 「秒 key <键码>」：合成一次真正的键盘按下 / 0.08 秒后松开（键码 = 苹果的虚拟键码，如回车 36、Tab 48、J 38）；「秒 type <字>」：逐字打字。 | "<seconds> key <key code>": synthesise a real key press / release 0.08 s later (key code = Apple virtual key code, e.g. Return 36, Tab 48, J 38); "<seconds> type <text>": type character by character.
                      // 都投给当时的键窗口（真键盘就是这样：弹着输入框时进输入框），并记一行键窗口是不是游戏窗口
                      // All are posted to the key window at that moment (which is what a real keyboard does: into the input field while it is shown), and one line records whether the key window is the game window
                      int ty = !strcmp(op, "type"); char word[64] = ""; if (ty) sscanf(ln.UTF8String, "%*lf %*7s %63s", word); NSString *chars = ty ? @(word) : (x == 36 ? @"\r" : @"");
                      if (ty) {   // 打字：在主线程的运行循环里执行（模态框开着时也走——主队列那时不走），记下第一响应者是谁，再把字经响应链送进去（等于输入法提交了这些字） | typing: executed in the main thread's run loop (which also runs while a modal dialog is open - the main queue does not), noting who the first responder is, then sending the text through the responder chain (as if an input method committed it)
                          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
                              NSWindow *kw = NSApp.keyWindow; id fr = kw.firstResponder; BOOL ok = [NSApp sendAction:@selector(insertText:) to:nil from:chars];
                              LOG("DM_TEST_NSMOUSE：%.2f 秒打字「%s」（键窗口%s，第一响应者 %s，%s）", NSProcessInfo.processInfo.systemUptime - t0, chars.UTF8String, kw == win ? "是游戏窗口" : kw ? "是弹框" : "没有", fr ? NSStringFromClass([fr class]).UTF8String : "无", ok ? "有人收" : "没人收"); });
                              CFRunLoopWakeUp(CFRunLoopGetMain()); });
                          continue; }
                      for (NSUInteger ci = 0; ci < (ty ? chars.length : 1); ci++) { NSString *one = ty ? [chars substringWithRange:NSMakeRange(ci, 1)] : chars; double at = sec + ci * 0.15;
                          for (int dn = 1; dn >= 0; dn--) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((at + (dn ? 0 : 0.08)) * NSEC_PER_SEC)), tq, ^{ NSWindow *kw = NSApp.keyWindow;
                              if (dn) LOG("DM_TEST_NSMOUSE：%.2f 秒投 键盘 %s%s（键窗口%s）", NSProcessInfo.processInfo.systemUptime - t0, ty ? "字 " : "键码 ", ty ? one.UTF8String : [NSString stringWithFormat:@"%d", (int)x].UTF8String, kw == win ? "是游戏窗口" : kw ? "是别的窗口（弹框）" : "没有：App 里没有键窗口");
                              [NSApp postEvent:[NSEvent keyEventWithType:dn ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:(kw ?: win).windowNumber context:nil characters:one charactersIgnoringModifiers:one isARepeat:NO keyCode:ty ? 0 : (unsigned short)x] atStart:NO]; }); }
                      continue; }
                  if (!strcmp(op, "unit") || !strcmp(op, "unith")) {   // 「秒 unit <类型>,<类号>」：盯住这类单位（类号 16777214 = 敌对且可选中的怪、16777215 = 任意），它一在画面里就在它身体框中心投一次真正的鼠标点击；unith = 按住 2.5 秒才抬起 | "<seconds> unit <type>,<class>": watch for units of this kind (class 16777214 = hostile and selectable monster, 16777215 = any) and post one real mouse click at the centre of its body box as soon as it is on screen; unith = hold for 2.5 seconds before releasing
                      double hold = !strcmp(op, "unith") ? 2.5 : 0.1;
                      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ void dm_world_watch(int, uint32_t); int dm_world_watch_pos(float *, float *); float nx = 0, ny = 0; int ok = 0;
                          dm_world_watch((int)x, (uint32_t)y); for (int i = 0; i < 60 && !(ok = dm_world_watch_pos(&nx, &ny)); i++) usleep(50000); dm_world_watch(-1, 0);
                          if (!ok) { LOG("DM_TEST_NSMOUSE：unit %d,%u 3 秒内没进画面，不点", (int)x, (unsigned)y); return; }
                          __block NSPoint pt; dispatch_sync(dispatch_get_main_queue(), ^{ pt = [v convertPoint:NSMakePoint(nx * v.bounds.size.width, (1 - ny) * v.bounds.size.height) toView:nil]; });
                          LOG("DM_TEST_NSMOUSE：%.2f 秒 unit %d,%u 在画面 (%.3f,%.3f) → 投按下，%.1f 秒后抬起", NSProcessInfo.processInfo.systemUptime - t0, (int)x, (unsigned)y, nx, ny, hold);
                          for (int dn = 1; dn >= 0; dn--) { if (!dn) usleep((useconds_t)(hold * 1e6));
                              [NSApp postEvent:[NSEvent mouseEventWithType:dn ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp location:pt modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:1 pressure:dn ? 1 : 0] atStart:NO]; } });
                      continue; }
                  if (!strcmp(op, "dbl") || !strcmp(op, "dragto")) {   // 「秒 dbl x,y」：同一点连点两下（间隔 0.15 秒，时刻自己掐，不靠两条定时——定时有零点几秒的误差，凑不出双击）； | "<seconds> dbl x,y": click the same point twice (0.15 s apart, timed by itself rather than by two scheduled lines - scheduling is off by a few tenths of a second and cannot produce a double click);
                      // 「秒 dragto x,y」：从上一条 down 的位置拖到这里（0.6 秒里匀速 12 步）再抬起，前面要先有一条 down
                      // "<seconds> dragto x,y": drag from the position of the previous down to here (12 even steps within 0.6 s) and release; a down must come before it
                      int dbl = !strcmp(op, "dbl");
                      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ __block NSPoint from, to; dispatch_sync(dispatch_get_main_queue(), ^{ from = g_test_last; to = NSMakePoint(x, v.bounds.size.height - (y + anc * (v.bounds.size.height - 720))); if (dbl) from = to; g_test_last = to; });
                          NSEvent *(^mk)(NSEventType, NSPoint, int) = ^(NSEventType t, NSPoint p, int cc) { __block NSPoint wp; dispatch_sync(dispatch_get_main_queue(), ^{ wp = [v convertPoint:p toView:nil]; });
                              return [NSEvent mouseEventWithType:t location:wp modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:cc pressure:t == NSEventTypeLeftMouseUp ? 0 : 1]; };
                          LOG("DM_TEST_NSMOUSE：%.2f 秒 %s 内容区 (%.0f,%.0f)", NSProcessInfo.processInfo.systemUptime - t0, dbl ? "双击" : "拖到", to.x, v.bounds.size.height - to.y);
                          if (dbl) { for (int i = 0; i < 2; i++) { [NSApp postEvent:mk(NSEventTypeLeftMouseDown, to, i + 1) atStart:NO]; usleep(60000); [NSApp postEvent:mk(NSEventTypeLeftMouseUp, to, i + 1) atStart:NO]; usleep(150000); } }
                          else { for (int i = 1; i <= 12; i++) { [NSApp postEvent:mk(NSEventTypeLeftMouseDragged, NSMakePoint(from.x + (to.x - from.x) * i / 12, from.y + (to.y - from.y) * i / 12), 1) atStart:NO]; usleep(50000); } usleep(200000); [NSApp postEvent:mk(NSEventTypeLeftMouseUp, to, 1) atStart:NO]; } });
                      continue; }
                  NSEventType ty = !strcmp(op, "hover") ? NSEventTypeMouseMoved : !strcmp(op, "down") ? NSEventTypeLeftMouseDown : !strcmp(op, "up") ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDragged; int has = !strcmp(op, "up") ? 0 : 1;
                  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)), tq, ^{ static NSPoint last;
                      if (has) { last = NSMakePoint(x, v.bounds.size.height - (y + anc * (v.bounds.size.height - 720))); g_test_last = last; }   // 内容区的点 → 窗口坐标（原点左下） | point in the content area -> window coordinates (origin at bottom-left)
                      NSEvent *ev = [NSEvent mouseEventWithType:ty location:[v convertPoint:last toView:nil] modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:win.windowNumber context:nil eventNumber:0 clickCount:1 pressure:ty == NSEventTypeLeftMouseUp ? 0 : 1];
                      LOG("DM_TEST_NSMOUSE：%.2f 秒投 %s 内容区 (%.0f,%.0f)（窗口%s是主窗口，App %s在前台，背板缩放 %.1f）", NSProcessInfo.processInfo.systemUptime - t0, ty == NSEventTypeMouseMoved ? "悬停" : ty == NSEventTypeLeftMouseDown ? "按下" : ty == NSEventTypeLeftMouseUp ? "抬起" : "拖动", last.x, v.bounds.size.height - last.y, win.isKeyWindow ? "" : "不", NSApp.isActive ? "" : "不", win.backingScaleFactor);
                      [NSApp postEvent:ev atStart:NO]; }); }
              LOG("DM_TEST_NSMOUSE：%d 条排好", n); }
          if ((e = getenv("DM_TEST_SHARELOG"))) { NSString *to = @(e); dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ NSString *er = share_logs(to); LOG("DM_TEST_SHARELOG：第 8 秒打包到 %s → %s", to.lastPathComponent.UTF8String, er ? er.UTF8String : "OK"); }); }   // 测试：不弹存储面板，第 8 秒直接把压缩包写到这个路径 | test: no save panel, write the archive directly to this path at second 8
          if ((e = getenv("DM_TEST_QUIT_AT"))) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(e) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ app_quit("DM_TEST_QUIT_AT"); }); }
        // 生命周期（hle.h / dm.h）：失焦 → 2、拿回焦点 → 1；最小化 → 3 + 音频暂停 + GPU 挂起，还原 → 恢复（随后拿回焦点 → 1）
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1)
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        // 窗口被别的窗口整个盖住（遮挡状态不含 Visible）：系统不给看不见的窗口显示节拍回调。按「看不见」处理——出图直接丢（mtl_present_discard，
        // 同 iOS 进后台等存盘那段），游戏逻辑照走、声音照放、焦点不变；露出来就照常出图。不处理的话节拍停着、出图全靠兜底丢帧，日志报假红
        // （mini 整表被 Mac App 窗口盖住坐实）。DM_OCCL=0 关掉这项（对照用）；
        // DM_TEST_COVER=<秒>,<时长>：到点用一个不透明窗口把游戏窗口整个盖住，时长到了拿开（验收用，产品不设）
        // Window fully covered by other windows (occlusion state lacks Visible): the system does not deliver display-link callbacks to invisible windows. Treat it as "not visible": discard frames directly (mtl_present_discard,
        // same as the iOS background save-wait period); game logic keeps running, audio keeps playing, focus unchanged; once uncovered, present normally. Without this the display link stalls, presentation relies entirely on fallback frame drops, and the log reports false reds
        // (confirmed when a full regression table on the test Mac was covered by the Mac App window). DM_OCCL=0 disables this (for comparison);
        // DM_TEST_COVER=<seconds>,<duration>: at that time, cover the game window entirely with an opaque window and remove it after the duration (for acceptance testing, not set in the product)
        { const char *e = getenv("DM_OCCL"); int on = !(e && !strcmp(e, "0"));
          [nc addObserverForName:NSWindowDidChangeOcclusionStateNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n;
              int hidden = !(win.occlusionState & NSWindowOcclusionStateVisible); void mtl_present_discard(int on);
              LOG("前端：窗口%s（遮挡处理%s）", hidden ? "整个被盖住 / 不可见" : "露出来了", on ? "开" : "关 DM_OCCL=0");
              if (on) mtl_present_discard(hidden); }];
          double at = 0, dur = 0;
          if ((e = getenv("DM_TEST_COVER")) && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0)
              dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(at * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                  NSWindow *c = [[NSWindow alloc] initWithContentRect:NSInsetRect(win.frame, -40, -40) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                  c.opaque = YES; c.backgroundColor = NSColor.grayColor; c.releasedWhenClosed = NO; [c orderFrontRegardless];
                  LOG("DM_TEST_COVER：盖上 %.1f 秒，游戏窗口%s是主窗口", dur, win.isKeyWindow ? "" : "不");
                  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(dur * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [c orderOut:nil]; LOG("DM_TEST_COVER：拿开"); });
              }); }
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (!win.miniaturized) dm_focus_set(2); g_btn = 0; g_w = g_a = g_s = g_d = 0; push(); { void dm_gamepad_reset(void); dm_gamepad_reset(); } }];   // 失焦：键盘和手柄按着的键清掉（松开事件收不到） | Focus lost: clear the keys held on keyboard and gamepad (their release events will not arrive)
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(1); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        // 客体线程：栈 64 MiB（Switch 主线程栈由 npdm 指定，宿主给足）
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// 软键盘输入框（front.h）：主线程弹 NSAlert + 文本框，客体线程同步等结果。无窗口模式不弹（返回 -1，调用方按取消）
// Software keyboard input box (front.h): the main thread shows an NSAlert + text field and the guest thread waits synchronously for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel)
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    __block int r = 0;
    __block NSString *text = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new];
        a.messageText = title && *title ? @(title) : @"输入";
        NSString *info = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
        a.informativeText = info;
        [a addButtonWithTitle:@"确定"]; [a addButtonWithTitle:@"取消"];
        NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
        f.stringValue = initial ? @(initial) : @"";
        if (guide && *guide) f.placeholderString = @(guide);
        a.accessoryView = f; [a layout]; a.window.initialFirstResponder = f;
        f.target = a.buttons.firstObject; f.action = @selector(performClick:);   // 在输入框里按回车 = 点「确定」（明确接上，不靠系统把回车转给缺省按钮） | pressing Return in the input field = clicking "OK" (wired explicitly, not relying on the system forwarding Return to the default button)
        // 弹框出来后光标要在输入框里：只指定「初始第一响应者」不生效（实测弹出后第一响应者是弹框自己，要先点一下输入框才能打字），等模态循环转起来再显式指定一次
        // The cursor must be in the input field once the dialog appears: specifying only the "initial first responder" does not work (measured: after it appears the first responder is the dialog itself, and the field has to be clicked before typing), so it is specified explicitly once more after the modal loop starts running
        CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ [a.window makeFirstResponder:f]; });
        r = [a runModal] == NSAlertFirstButtonReturn;
        [g_win makeKeyAndOrderFront:nil];   // 弹框关掉后把游戏窗口显式设回键窗口（系统通常自己会还，保险） | after the dialog closes, explicitly make the game window key again (the system usually returns it by itself; this is for safety)
        text = f.stringValue;
    });
    if (!r) return 0;
    (void)max_chars;                              // 截断按 UTF-16 单元在 hle_swkbd.c 统一做 | Truncation is done uniformly in hle_swkbd.c in UTF-16 units
    snprintf(out, out_cap, "%s", text.UTF8String);
    return 1;
}
