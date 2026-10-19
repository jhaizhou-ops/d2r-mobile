// AppKit window and desktop input; the game thread uses a 64 MiB stack.
// Persistent bindings are read through dm_controls_mac.
#import <Cocoa/Cocoa.h>
#include "dm_controls_mac.h"
#include "touch_mac.h"
#include <sys/sysctl.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#include "dm.h"
#include "dm_input.h"
#include "dm_cursor.h"
#include <stdatomic.h>
#include "dm_uiscreen.h"
#include "dm_i18n.h"
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
static int g_display_occlusion_discard;
static void display_present_occlusion(NSWindow *window,int enabled) {
    int hidden=!(window.occlusionState&NSWindowOcclusionStateVisible);
    LOG("前端：窗口%s（遮挡处理%s）",hidden?"整个被盖住 / 不可见":"露出来了",enabled?"开":"关 DM_OCCL=0");
    if(enabled){void mtl_present_discard(int);g_display_occlusion_discard=hidden;mtl_present_discard(hidden);}
}
static BOOL display_present_interactive(NSWindow *window,const char *reason) {
    if(!g_display_occlusion_discard||!window.visible||window.miniaturized||!window.keyWindow)return NO;
    // Focus or input confirms an interactive window even when occlusion notifications arrive late.
    void mtl_present_discard(int);g_display_occlusion_discard=0;mtl_present_discard(0);
    CGSize drawable=g_layer?g_layer.drawableSize:CGSizeZero;
    LOG("Display: presentation recovered (%s), visible=%d key=%d occlusion=%#lx drawable=%.0fx%.0f",reason,window.visible,window.keyWindow,(unsigned long)window.occlusionState,drawable.width,drawable.height);
    return YES;
}
// The game aspect ratio is fixed at startup; the window can be resized freely.
// On narrow screens, dm_uiscreen keeps the 16:9 UI inside the native-scale world.
// DM_TEST_SCREEN_ASPECT=<width>x<height> tests this contract without changing displays.
static double g_aspect = 16.0 / 9;   // 游戏画面比例（宽 ÷ 高） | aspect ratio of the game picture (width / height)
static int g_full;                    // 1 = 真全屏已开 | 1 = true full screen is on
static void full_setup(void) {
    if (!getenv("DM_APP") || getenv("DM_GAME_RES")) return;
    NSScreen *sc = NSScreen.mainScreen; double w = sc.frame.size.width, h = sc.frame.size.height - sc.safeAreaInsets.top; const char *e = getenv("DM_TEST_SCREEN_ASPECT"); unsigned tw, th;
    if (e && sscanf(e, "%ux%u", &tw, &th) == 2 && tw && th) { w = tw; h = th; }
    if (h <= 0 || w / h >= 16.0 / 9 - 0.01) { LOG("画面：主屏 %.0fx%.0f 不比 16:9 窄 → 全程 16:9", w, h); return; }
    g_aspect = w / h; g_full = 1; dm_uiscreen_enable(DM_UISCREEN_BOTTOM);
    LOG("Display: usable screen %.0fx%.0f, aspect %.3f; native world and 16:9 UI", w, h, g_aspect);
}
static NSUserDefaults *g_display_prefs;
static _Atomic uint64_t g_render_size;
static NSSize g_render_cap, g_auto_size;
static unsigned g_resolution_choice;
static NSRect g_windowed_frame;
static int g_window_transition, g_after_fullscreen = -1;
static NSMenuItem *g_resolution_status;
static const unsigned k_render_widths[] = {0,1280,1600,1920,2560,3200,3840};
static NSSize display_size(unsigned width, double aspect) {
    if (!isfinite(aspect) || aspect <= 0) aspect = 16.0/9;
    double w = fmin(4096, fmax(640, width)), h = round(w / aspect / 2) * 2;
    if (h > 4096) { h = 4096; w = floor(h * aspect / 2) * 2; }
    return NSMakeSize(w, fmax(360,h));
}
static uint64_t display_pack(NSSize size) { return (uint64_t)(uint32_t)size.height << 32 | (uint32_t)size.width; }
static NSSize display_target(unsigned width) { return width ? display_size(width,g_aspect) : g_auto_size; }
static BOOL display_pending(void) { NSSize s = display_target(g_resolution_choice); return s.width > g_render_cap.width || s.height > g_render_cap.height; }
static void display_load(NSScreen *screen) {
    if (!g_display_prefs) g_display_prefs = NSUserDefaults.standardUserDefaults;
    unsigned saved = (unsigned)[g_display_prefs integerForKey:@"DisplayRenderWidth"]; int found = 0;
    for (unsigned i=0;i<sizeof k_render_widths/sizeof *k_render_widths;i++) found |= saved == k_render_widths[i];
    g_resolution_choice = found ? saved : 0;
    CGFloat scale = screen.backingScaleFactor ?: 2; CGFloat width = screen ? screen.frame.size.width * scale : 2560;
    g_auto_size = display_size((unsigned)llround(width/2)*2,g_aspect);
    g_render_cap = display_target(g_resolution_choice); atomic_store(&g_render_size,display_pack(g_render_cap));
    LOG("Display: initial capacity %.0fx%.0f, automatic Retina %.0fx%.0f", g_render_cap.width,g_render_cap.height,g_auto_size.width,g_auto_size.height);
}
static int display_resolution(unsigned width) {
    NSSize target = display_target(width); g_resolution_choice = width; [g_display_prefs setInteger:width forKey:@"DisplayRenderWidth"];
    if (target.width > g_render_cap.width || target.height > g_render_cap.height) { LOG("Display: %.0fx%.0f saved; restart required, capacity %.0fx%.0f",target.width,target.height,g_render_cap.width,g_render_cap.height); return 0; }
    uint64_t packed = display_pack(target);
    if (atomic_exchange(&g_render_size,packed) != packed) { void dm_oe_res_changed(void); dm_oe_res_changed(); }
    LOG("Display: active render size %.0fx%.0f",target.width,target.height);
    return 1;
}
static void display_status(void) {
    uint64_t packed = atomic_load(&g_render_size); NSSize target = display_target(g_resolution_choice);
    BOOL restart = target.width > g_render_cap.width || target.height > g_render_cap.height;
    g_resolution_status.title = [NSString stringWithFormat:@(dm_ui("dmMacResolutionInUse")),(unsigned)packed,(unsigned)(packed>>32),restart ? @(dm_ui("dmMacResolutionPending")) : @""];
}
static NSRect display_visible_frame(void) { return (g_win.screen ?: NSScreen.mainScreen).visibleFrame; }
static BOOL display_frame_valid(NSRect r) {
    if (!isfinite(r.origin.x) || !isfinite(r.origin.y) || !isfinite(r.size.width) || !isfinite(r.size.height)) return NO;
    NSSize content = [g_win contentRectForFrameRect:r].size;
    return content.width >= 640 && content.height >= 360;
}
static BOOL display_frame_maximized(NSRect r, NSRect visible) {
    return r.size.width >= visible.size.width - 2 && r.size.height >= visible.size.height - 2;
}
static NSRect display_bound_frame(NSRect r, NSRect visible) {
    NSSize minimum = [g_win frameRectForContentRect:NSMakeRect(0,0,640,360)].size;
    r.size.width = fmin(visible.size.width, fmax(minimum.width,r.size.width));
    r.size.height = fmin(visible.size.height, fmax(minimum.height,r.size.height));
    r.origin.x = fmax(NSMinX(visible), fmin(r.origin.x,NSMaxX(visible)-r.size.width));
    r.origin.y = fmax(NSMinY(visible), fmin(r.origin.y,NSMaxY(visible)-r.size.height));
    return r;
}
static NSRect display_default_frame(NSRect visible) {
    NSRect r = [g_win frameRectForContentRect:NSMakeRect(0,0,1280,fmax(360,floor(1280/g_aspect)))];
    r = display_bound_frame(r,visible);
    r.origin = NSMakePoint(NSMidX(visible)-r.size.width/2,NSMidY(visible)-r.size.height/2);
    return r;
}
static void display_load_window_frame(BOOL restored) {
    NSRect visible = display_visible_frame(), normal = NSZeroRect;
    id saved = [g_display_prefs objectForKey:@"DisplayNormalFrame"];
    if ([saved isKindOfClass:NSString.class]) normal = NSRectFromString(saved);
    if (!display_frame_valid(normal) || display_frame_maximized(normal,visible)) {
        normal = restored && !g_win.zoomed && display_frame_valid(g_win.frame) && !display_frame_maximized(g_win.frame,visible) ? g_win.frame : display_default_frame(visible);
    }
    g_windowed_frame = display_bound_frame(normal,visible);
}
static void display_save_window_frame(void) {
    if (!g_win || g_window_transition || g_win.zoomed || (g_win.styleMask & NSWindowStyleMaskFullScreen)) return;
    NSRect frame = g_win.frame, visible = display_visible_frame();
    // Autosave can restore a maximized frame without setting AppKit’s zoom state.
    if (!display_frame_valid(frame) || display_frame_maximized(frame,visible)) return;
    g_windowed_frame = display_bound_frame(frame,visible);
    [g_display_prefs setObject:NSStringFromRect(g_windowed_frame) forKey:@"DisplayNormalFrame"];
}
static void display_window_mode(int mode) {
    if (!g_win) return;
    [g_display_prefs setInteger:mode forKey:@"DisplayWindowMode"];
    BOOL fullscreen = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (fullscreen) { if (mode != 2) { g_after_fullscreen = mode; [g_win toggleFullScreen:nil]; } return; }
    if (mode == 2) { display_save_window_frame(); [g_win toggleFullScreen:nil]; return; }
    if (mode == 1) { if (!g_win.zoomed) { display_save_window_frame(); g_window_transition = 1; [g_win zoom:nil]; g_window_transition = 0; } }
    else {
        if (NSIsEmptyRect(g_windowed_frame)) display_load_window_frame(NO);
        g_windowed_frame = display_bound_frame(g_windowed_frame,display_visible_frame());
        g_window_transition = 1;
        if (g_win.zoomed) [g_win zoom:nil];
        [g_win setFrame:g_windowed_frame display:YES];
        g_window_transition = 0; display_save_window_frame();
    }
}
// 游戏画面在视图里的位置：按游戏画面比例放进视图（比例对得上就是整个视图；对不上的四周留黑边）
// Where the game picture sits in the view: fitted into the view by the picture's aspect ratio (if the ratios match, the whole view; otherwise black bars around)
static NSRect game_rect(void) {
    NSSize b = g_view.bounds.size; CGFloat w = b.width, h = b.height;
    if (fabs(w / h / g_aspect - 1) < 0.003) { }   // 比例差 0.3% 以内（窗口高取整的零头、全屏内容区比按刘海安全区算的少 1 点）当对得上，铺满 | within 0.3% (the remainder from rounding the window height, and the full-screen content area being 1 point less than computed from the notch safe area) counts as matching, and it fills
    else if (w > h * g_aspect) w = round(h * g_aspect); else h = round(w / g_aspect);
    return NSMakeRect(floor((b.width - w) / 2), floor((b.height - h) / 2), w, h);
}
static NSCursor *g_game_cursor, *g_game_cursor_pressed;
static unsigned g_cursor_buttons;
static BOOL game_cursor_load(NSString *directory) {
    NSImage *normal = [[NSImage alloc] initWithContentsOfFile:[directory stringByAppendingPathComponent:@"normal.png"]];
    NSImage *pressed = [[NSImage alloc] initWithContentsOfFile:[directory stringByAppendingPathComponent:@"pressed.png"]];
    if (!normal || !pressed) return NO;
    // The original 69×64 px sprites retain their hotspot at Retina scale.
    normal.size = pressed.size = NSMakeSize(34.5,32);
    g_game_cursor = [[NSCursor alloc] initWithImage:normal hotSpot:NSMakePoint(1.5,7)];
    g_game_cursor_pressed = [[NSCursor alloc] initWithImage:pressed hotSpot:NSMakePoint(2,7)];
    return YES;
}
static NSCursor *game_cursor_for_point(NSPoint point, BOOL pressed, BOOL blocked) {
    if (!g_live || blocked || !g_game_cursor || !NSPointInRect(point,game_rect())) return NSCursor.arrowCursor;
    return pressed ? g_game_cursor_pressed : g_game_cursor;
}
static void pc_pointer_exit(void);
@interface DmGameView : NSView
@property NSTrackingArea *gameTrackingArea;
@end
@implementation DmGameView
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (self.gameTrackingArea) [self removeTrackingArea:self.gameTrackingArea];
    if (g_view != self) return;
    self.gameTrackingArea = [[NSTrackingArea alloc] initWithRect:game_rect() options:NSTrackingCursorUpdate|NSTrackingMouseEnteredAndExited|NSTrackingActiveInKeyWindow|NSTrackingEnabledDuringMouseDrag owner:self userInfo:nil];
    [self addTrackingArea:self.gameTrackingArea];
}
- (void)cursorUpdate:(NSEvent *)event {
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    [game_cursor_for_point(point,g_cursor_buttons != 0,self.window.attachedSheet != nil) set];
}
- (void)mouseEntered:(NSEvent *)event { [self cursorUpdate:event]; }
- (void)mouseExited:(NSEvent *)event { (void)event; [NSCursor.arrowCursor set];pc_pointer_exit(); }
@end
static void game_cursor_event(NSEvent *event) {
    display_present_interactive(g_win,"mouse");
    switch (event.type) {
        case NSEventTypeLeftMouseDown: g_cursor_buttons |= 1; break;
        case NSEventTypeRightMouseDown: g_cursor_buttons |= 2; break;
        case NSEventTypeLeftMouseUp: g_cursor_buttons &= ~1u; break;
        case NSEventTypeRightMouseUp: g_cursor_buttons &= ~2u; break;
        default: break;
    }
    NSPoint point = [g_view convertPoint:event.locationInWindow fromView:nil];
    [game_cursor_for_point(point,g_cursor_buttons != 0,g_view.window.attachedSheet != nil) set];
}
static void layout_game(void) {
    NSRect r = game_rect();
    CGFloat sc = g_view.window.backingScaleFactor ?: 2;   // 画布跟着改：作为子层的 CAMetalLayer 改 frame 不会自己改 drawableSize，原先进全屏后一直用窗口态的画布放大显示（发糊） | the canvas follows: a CAMetalLayer used as a sublayer does not change its drawableSize when its frame changes, so after going full screen it used to keep the windowed canvas and scale it up (blurry)
    if (NSEqualRects(r,g_layer.frame) && g_layer.contentsScale == sc && CGSizeEqualToSize(g_layer.drawableSize,CGSizeMake(round(r.size.width*sc),round(r.size.height*sc)))) return;
    [CATransaction begin]; [CATransaction setDisableActions:YES]; g_layer.frame = r; g_layer.contentsScale = sc; g_layer.drawableSize = CGSizeMake(round(r.size.width * sc), round(r.size.height * sc)); [CATransaction commit];
    [g_view updateTrackingAreas];
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
static void show_keys(void) { if(g_live&&dm_ui_pc_desktop()){NSAlert *info=[NSAlert new];info.messageText=@(ui("dmMacGameControls"));info.informativeText=@(ui("dmMacGameControlsHelp"));[info runModal];return;} NSAlert *a = [NSAlert new]; a.messageText = @(ui("dmAppKeysTitle")); NSMutableString *help = [NSMutableString stringWithString:@(ui("dmMacCurrentBindings"))]; int bindings[DM_CTL_COUNT][2]; dm_controls_get(bindings); for (int row = 0; row < DM_CTL_COUNT; row++) { int action = dm_controls_display_action(row); char name[192], first[128], second[128]; dm_controls_action_label(action, name, sizeof name); dm_controls_label(bindings[action][0], first, sizeof first); dm_controls_label(bindings[action][1], second, sizeof second); [help appendFormat:@"%s: %s%@\n", name, first, bindings[action][1] < 0 ? @"" : [NSString stringWithFormat:@" / %s", second]]; } a.informativeText = help; [a addButtonWithTitle:@(ui("dmAppOK"))]; [a runModal]; }
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
@interface DmWinCtl : NSObject <NSWindowDelegate, NSMenuItemValidation, NSMenuDelegate> @end
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
@interface DmWinCtl (Controls)
- (void)controls:(id)sender;
- (void)mouseControls:(id)sender;
- (void)controlsUnavailable;
@end
static void controls_menu_update(NSMenu *menu,id target) {
    NSInteger index=[menu indexOfItemWithTarget:target andAction:@selector(mouseControls:)];
    if(dm_ui_pc_desktop()){
        if(index<0)[[menu addItemWithTitle:[@(ui("dmMacCtlMouseTitle")) stringByAppendingString:@"…"] action:@selector(mouseControls:) keyEquivalent:@""] setTarget:target];
    }else if(index>=0)[menu removeItemAtIndex:index];
}
static NSMenu *controls_menu(id target) {
    NSMenu *menu=[[NSMenu alloc] initWithTitle:@(ui("dmMacCtlTitle"))];
    [[menu addItemWithTitle:[@(ui("dmMacGameControls")) stringByAppendingString:@"…"] action:@selector(controls:) keyEquivalent:@","] setTarget:target];
    menu.delegate=target;controls_menu_update(menu,target);
    return menu;
}
@implementation DmWinCtl
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w; app_quit("关窗口"); return NO; }
- (void)windowDidResize:(NSNotification *)n { (void)n; layout_game(); }
- (void)windowDidEndLiveResize:(NSNotification *)n { (void)n; display_save_window_frame(); if (!g_window_transition && !(g_win.styleMask & NSWindowStyleMaskFullScreen)) [g_display_prefs setInteger:g_win.zoomed ? 1 : 0 forKey:@"DisplayWindowMode"]; }
- (void)windowDidMove:(NSNotification *)n { (void)n; display_save_window_frame(); }
- (BOOL)windowShouldZoom:(NSWindow *)window toFrame:(NSRect)frame {
    (void)frame;
    if (g_window_transition) return YES;
    display_save_window_frame();
    if (!window.zoomed) { [g_display_prefs setInteger:1 forKey:@"DisplayWindowMode"]; return YES; }
    // AppKit’s zoom history may come from autosave rather than the normal frame.
    g_window_transition = 1;
    dispatch_async(dispatch_get_main_queue(), ^{ display_window_mode(0); });
    return NO;
}
- (void)windowDidChangeBackingProperties:(NSNotification *)n { (void)n; layout_game(); }
- (void)windowDidChangeScreen:(NSNotification *)n { (void)n; screen_note(); }
- (void)windowWillEnterFullScreen:(NSNotification *)n { (void)n; display_save_window_frame(); g_window_transition = 1; }
- (void)windowWillExitFullScreen:(NSNotification *)n { (void)n; g_window_transition = 1; }
- (void)windowDidEnterFullScreen:(NSNotification *)n { (void)n; g_window_transition = 0; [g_display_prefs setInteger:2 forKey:@"DisplayWindowMode"]; layout_game(); }
- (void)windowDidExitFullScreen:(NSNotification *)n { (void)n; g_window_transition = 0; int mode = g_after_fullscreen; g_after_fullscreen = -1; if (mode < 0) mode = g_win.zoomed ? 1 : 0; display_window_mode(mode); layout_game(); }
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w { g_window_transition = 0; [g_display_prefs setInteger:w.zoomed ? 1 : 0 forKey:@"DisplayWindowMode"]; }
- (void)windowDidFailToExitFullScreen:(NSWindow *)w { (void)w; g_window_transition = 0; g_after_fullscreen = -1; [g_display_prefs setInteger:2 forKey:@"DisplayWindowMode"]; }
- (void)windowed:(id)s { (void)s; display_window_mode(0); }
- (void)maximized:(id)s { (void)s; display_window_mode(1); }
- (void)toggleFullScreen:(id)s { [g_win toggleFullScreen:s]; }
- (void)resolution:(NSMenuItem *)s {
    BOOL active = display_resolution((unsigned)s.tag); display_status();
    if (!active) { NSAlert *a = [NSAlert new]; a.messageText = @(ui("dmMacResolutionSaved")); a.informativeText = @(ui("dmMacResolutionRestart")); [a addButtonWithTitle:@(ui("dmAppOK"))]; [a beginSheetModalForWindow:g_win completionHandler:nil]; }
}
- (BOOL)validateMenuItem:(NSMenuItem *)s {
    if (s.action == @selector(resolution:)) { s.state = g_resolution_choice == (unsigned)s.tag ? NSControlStateValueOn : NSControlStateValueOff; return !getenv("DM_GAME_RES"); }
    if (s == g_resolution_status) { display_status(); return NO; }
    if (s.action == @selector(windowed:)) s.state = !(g_win.styleMask & NSWindowStyleMaskFullScreen) && !g_win.zoomed ? NSControlStateValueOn : NSControlStateValueOff;
    if (s.action == @selector(maximized:)) s.state = !(g_win.styleMask & NSWindowStyleMaskFullScreen) && g_win.zoomed ? NSControlStateValueOn : NSControlStateValueOff;
    if (s.action == @selector(toggleFullScreen:)) s.state = g_win.styleMask & NSWindowStyleMaskFullScreen ? NSControlStateValueOn : NSControlStateValueOff;
    return !g_window_transition;
}
- (void)quit:(id)s { (void)s; app_quit("菜单退出"); }
- (void)keys:(id)s { (void)s; show_keys(); }
- (void)share:(id)s { (void)s; share_logs_ui(); }
@end

static NSMenu *display_menu(id ctl) {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@(ui("dmAppMenuView"))];
    NSMenuItem *windowed = [menu addItemWithTitle:@(ui("dmMacWindowed")) action:@selector(windowed:) keyEquivalent:@""]; windowed.target = ctl;
    NSMenuItem *max = [menu addItemWithTitle:@(ui("dmMacMaximize")) action:@selector(maximized:) keyEquivalent:@""]; max.target = ctl;
    NSMenuItem *fs = [menu addItemWithTitle:@(ui("dmAppFullscreen")) action:@selector(toggleFullScreen:) keyEquivalent:@"f"]; fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand; fs.target = ctl;
    [menu addItem:NSMenuItem.separatorItem];
    NSMenu *res = [[NSMenu alloc] initWithTitle:@(ui("dmMacRenderResolution"))];
    for (unsigned i = 0; i < sizeof k_render_widths / sizeof *k_render_widths; i++) {
        unsigned width = k_render_widths[i]; NSSize size = display_target(width);
        if (width && size.width != width) continue;
        BOOL restart = size.width > g_render_cap.width || size.height > g_render_cap.height;
        NSString *title = [NSString stringWithFormat:@"%@%u × %u%@", width ? @"" : @(ui("dmMacResolutionAuto")), (unsigned)size.width, (unsigned)size.height, restart ? @(ui("dmMacRestartSuffix")) : @""];
        NSMenuItem *item = [res addItemWithTitle:title action:@selector(resolution:) keyEquivalent:@""]; item.tag = width; item.target = ctl;
    }
    NSMenuItem *parent = [menu addItemWithTitle:res.title action:nil keyEquivalent:@""]; parent.submenu = res;
    g_resolution_status = [menu addItemWithTitle:@"" action:nil keyEquivalent:@""]; g_resolution_status.enabled = NO; display_status();
    return menu;
}

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
// 原版 16:9 常数，行为不变；DM_GAME_RES=宽x高（主机模式，掌机按 720/900 同比）只供与 iOS 宽屏对拍
// The original game's 16:9 constants, behavior unchanged; DM_GAME_RES=<width>x<height> (docked mode; handheld scaled by 720/900) is only for side-by-side comparison with iOS widescreen
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    uint64_t render = atomic_load(&g_render_size); if (render) { uint32_t rw=(uint32_t)render,rh=(uint32_t)(render>>32); return docked ? render : (uint64_t)((rh*4/5)&~1u)<<32 | ((rw*4/5+3)/8*8); }
    if (g_full) return docked ? (uint64_t)(llround(1600 / g_aspect / 2) * 2) << 32 | 1600 : 720ull << 32 | (uint64_t)(llround(720 * g_aspect / 8) * 8);   // 真全屏：宽 1600、高按屏幕比例（16:10 → 1600x1004） | true full screen: width 1600, height by the screen's aspect ratio (16:10 -> 1600x1004)
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}

static NSPoint g_test_last;   // 合成鼠标脚本：上一条带坐标的指令落在视图里的点（dragto 从这里起拖） | synthetic mouse script: the point in the view where the last instruction with coordinates landed (dragto starts dragging from here)
static int g_actions[DM_CTL_COUNT], g_key_latch[128], g_mouse_latch[32];
static unsigned g_release_generation;
static int g_pc_vk_latch[128],g_pc_vk_count[256],g_pc_mouse_down[32],g_pc_mouse_side[32];
static float g_mouse_nx, g_mouse_ny;
static int g_mouse_have;
static double g_wheel_remainder, g_wheel_time;
static BOOL g_wheel_precise;
static int mouse_point(NSEvent *event, NSView *view, int pressed, float *nx, float *ny);
static void input_probe_write(NSDictionary *row) {
    const char *path = getenv("DM_MAC_POINTER_LOG"); static unsigned count[2];
    if (!path || !*path) return;
    NSString *phase=row[@"phase"]; NSEventType type=[row[@"type"] unsignedIntegerValue];
    BOOL motion=![phase isEqual:@"FINAL_MOVE"] && ([phase isEqual:@"MOVE"] || [phase isEqual:@"HOVER"] || [phase isEqual:@"DRAG"] || type==NSEventTypeMouseMoved || type==NSEventTypeLeftMouseDragged || type==NSEventTypeRightMouseDragged || type==NSEventTypeOtherMouseDragged);
    unsigned bucket=motion?0:1, limit=motion?128:1024;
    if (count[bucket]>=limit) return;
    count[bucket]++;
    NSData *data = [NSJSONSerialization dataWithJSONObject:row options:0 error:nil]; FILE *fp = data ? fopen(path,"a") : NULL;
    if (fp) { fwrite(data.bytes,1,data.length,fp); fputc('\n',fp); fclose(fp); }
}
static void pointer_probe(const char *phase, NSEvent *event, float nx, float ny) {
    if (!getenv("DM_MAC_POINTER_LOG") || !event) return;
    NSPoint p = [g_view convertPoint:event.locationInWindow fromView:nil]; int rw=0,rh=0,ux=0,uy=0,uw=0,uh=0; dm_screen_real(&rw,&rh); dm_ui_rect(&ux,&uy,&uw,&uh);
    input_probe_write(@{@"phase":@(phase),@"type":@(event.type),@"time":@(event.timestamp),@"eventWindow":@(event.windowNumber),@"gameWindow":@(g_win.windowNumber),@"location":@[@(event.locationInWindow.x),@(event.locationInWindow.y)],@"viewPoint":@[@(p.x),@(p.y)],@"bounds":NSStringFromRect(g_view.bounds),@"layerFrame":NSStringFromRect(g_layer.frame),@"gameRect":NSStringFromRect(game_rect()),@"scale":@(g_win.backingScaleFactor),@"normalized":@[@(nx),@(ny)],@"render":@[@(rw),@(rh)],@"uiRect":@[@(ux),@(uy),@(uw),@(uh)]});
}
static int wheel_steps(double dx, double dy, BOOL precise, NSEventPhase phase, double time) {
    if (!isfinite(dx) || !isfinite(dy)) return 0;
    if (phase & NSEventPhaseCancelled) { g_wheel_remainder = 0; return 0; }
    if ((phase & NSEventPhaseBegan) || time - g_wheel_time > .25 || precise != g_wheel_precise || dy * g_wheel_remainder < 0) g_wheel_remainder = 0;
    g_wheel_time = time; g_wheel_precise = precise;
    if (fabs(dx) > fabs(dy)) return 0;
    g_wheel_remainder += dy / (precise ? 32.0 : 1.0);
    int steps = (int)fmax(-3,fmin(3,trunc(g_wheel_remainder)));
    if (fabs(g_wheel_remainder) >= 3) g_wheel_remainder = 0; else g_wheel_remainder -= steps;
    return steps;
}
static void push(void) {
    uint64_t buttons = 0; for (int a = 0; a < DM_CTL_COUNT; a++) if (g_actions[a]) buttons |= dm_controls_button(a);
    int32_t lx = (g_actions[DM_CTL_MOVE_RIGHT] != 0) - (g_actions[DM_CTL_MOVE_LEFT] != 0);
    int32_t ly = (g_actions[DM_CTL_MOVE_UP] != 0) - (g_actions[DM_CTL_MOVE_DOWN] != 0);
    int wheel = (g_actions[DM_CTL_SCROLL_UP] != 0) - (g_actions[DM_CTL_SCROLL_DOWN] != 0);
    if (!g_live) { if (buttons & (1ull << 40)) buttons = (buttons & ~(1ull << 40)) | DM_BTN_PLUS; if (buttons & (1ull << 41)) buttons = (buttons & ~(1ull << 41)) | DM_BTN_B; }
    dm_touch_host_aim(g_live && g_mouse_have ? buttons : 0, g_mouse_nx, g_mouse_ny);
    if(g_live&&dm_ui_pc_desktop())dm_touch_host_pointer(g_mouse_nx,g_mouse_ny,g_mouse_have);
    dm_input_set(buttons, lx * 32767, ly * 32767, 0, wheel * 32767);
}
static void pc_mouse_release_held(void) {
    for(int b=0;b<32;b++)if(g_pc_mouse_down[b]){
        // The latched side preserves the DOWN route across editor or reservation changes.
        int side=g_pc_mouse_side[b];
        if(b>=2&&b<=4&&side<0)dm_ui_pc_key(0x100+b-2,0,0);
        else if(b<2||side>=0)dm_touch_host_pc_mouse(b,2,0,0,side);
        g_pc_mouse_down[b]=0;
    }
}
static void controls_release(void) {
    g_cursor_buttons = 0; [NSCursor.arrowCursor set];
    if (g_live) dm_ui_pc_bindings_cancel();
    if(g_live&&dm_ui_pc_desktop())dm_ui_ptr_at(DM_UP_END,-1,-1,0);
    pc_mouse_release_held();
    memset(g_pc_vk_latch,0xff,sizeof g_pc_vk_latch);memset(g_pc_vk_count,0,sizeof g_pc_vk_count);
    g_release_generation++; g_mouse_have = 0; g_wheel_remainder = 0;
    memset(g_actions, 0, sizeof g_actions); memset(g_key_latch, 0xff, sizeof g_key_latch); memset(g_mouse_latch, 0xff, sizeof g_mouse_latch);
    if (g_live) { dm_touch_host_live(2, 0, 0); dm_touch_host_live(5, 0, 0); }
    push();
}
static void pc_pointer_exit(void) {
    if(!g_live)return;
    if(!dm_ui_pc_desktop()){dm_touch_host_live(3,-1,-1);return;}
    g_mouse_have=0;dm_ui_ptr_at(DM_UP_END,-1,-1,0);
    pc_mouse_release_held();
    push();
}
static void control_action(int action, int down, NSEvent *event) {
    if (action < 0 || action >= DM_CTL_COUNT) return;
    if (event && (!g_mouse_have || (event.type != NSEventTypeKeyDown && event.type != NSEventTypeKeyUp && event.type != NSEventTypeFlagsChanged))) g_mouse_have = mouse_point(event, g_view, 1, &g_mouse_nx, &g_mouse_ny);
    int was = g_actions[action];
    if (down) g_actions[action]++; else if (g_actions[action]) g_actions[action]--;
    if (down) { void dm_padmode_request_kbd(int); dm_padmode_request_kbd(1); }
    if ((action == DM_CTL_PRIMARY || action == DM_CTL_SECONDARY) && ((!was && down) || (was && !g_actions[action]))) {
        float nx = 0, ny = 0; int base = action == DM_CTL_PRIMARY ? 0 : 3;
        if (!down) { if (event && mouse_point(event, g_view, 0, &nx, &ny)) { pointer_probe("FINAL_MOVE",event,nx,ny); dm_touch_host_live(base + 1, nx, ny); } pointer_probe("UP",event,nx,ny); dm_touch_host_live(base + 2, 0, 0); }
        else if (mouse_point(event, g_view, 1, &nx, &ny)) { pointer_probe("DOWN",event,nx,ny); if(base==3)dm_touch_host_pc_mouse(1,0,nx,ny,1);else dm_touch_host_live(base, nx, ny); }
    }
    push();
}
static void on_scroll(NSEvent *event) {
    int steps = wheel_steps(event.scrollingDeltaX,event.scrollingDeltaY,event.hasPreciseScrollingDeltas,event.phase,event.timestamp);
    if (!steps) return;
    if(g_live&&dm_ui_pc_desktop()){float nx,ny;if(mouse_point(event,g_view,1,&nx,&ny))dm_ui_pc_wheel(steps,dm_controls_virtual_modifiers(event),event.momentumPhase!=NSEventPhaseNone,nx,ny);return;}
    int action = dm_controls_lookup(dm_controls_event_input(event)); if (action < 0) return;
    if (action == DM_CTL_SCROLL_UP || action == DM_CTL_SCROLL_DOWN) {
        float nx,ny; if (mouse_point(event,g_view,1,&nx,&ny)) { dm_ui_ptr(DM_UP_MOVE,nx,ny,0); dm_ui_ptr(DM_UP_SCROLL,nx,ny,(action == DM_CTL_SCROLL_UP ? 120 : -120) * abs(steps)); }
        return;
    }
    if (event.momentumPhase != NSEventPhaseNone) return;
    control_action(action,1,event); unsigned generation = g_release_generation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(.12*NSEC_PER_SEC)),dispatch_get_main_queue(),^{ if (generation == g_release_generation) control_action(action,0,event); });
}
static int on_pc_key(NSEvent *event,int down) {
    void dm_padmode_request_kbd(int);
    unsigned code=event.keyCode;if(code>=128)return 0;
    int vk=down?dm_controls_virtual_key(event):g_pc_vk_latch[code];if(vk<0||vk>=256)return 0;
    if(down){if(g_pc_vk_latch[code]>=0)return event.isARepeat?dm_ui_pc_key(g_pc_vk_latch[code],dm_controls_virtual_modifiers(event),1):1;g_pc_vk_latch[code]=vk;if(g_pc_vk_count[vk]++)return 1;}
    else{g_pc_vk_latch[code]=-1;if(!g_pc_vk_count[vk]||--g_pc_vk_count[vk])return 1;}
    int accepted=dm_ui_pc_key(vk,dm_controls_virtual_modifiers(event),down);
    if(down){dm_padmode_request_kbd(1);if(!g_mouse_have)g_mouse_have=mouse_point(event,g_view,1,&g_mouse_nx,&g_mouse_ny);}
    push();return accepted;
}
static int mouse_reserved_input(int input){return input>=256&&input<288?dm_ui_pc_mouse_reserved(input-256):0;}
static void pc_mouse_button(NSEvent *event,int down) {
    unsigned button=event.buttonNumber;if(button>=32)return;
    int side=-1;
    if(down){int action=dm_controls_mouse_lookup(256+(int)button);if(!(button>=2&&button<=4&&dm_ui_pc_controls_active())&&!dm_ui_pc_mouse_reserved((int)button))side=action==DM_CTL_PRIMARY?0:action==DM_CTL_SECONDARY?1:-1;g_pc_mouse_side[button]=side;g_pc_mouse_down[button]=1;}
    else{if(!g_pc_mouse_down[button])return;side=g_pc_mouse_side[button];g_pc_mouse_down[button]=0;}
    float nx=g_mouse_nx,ny=g_mouse_ny;if(down)g_mouse_have=mouse_point(event,g_view,1,&nx,&ny);else mouse_point(event,g_view,0,&nx,&ny);
    int mods=dm_controls_virtual_modifiers(event);g_mouse_nx=nx;g_mouse_ny=ny;dm_ui_ptr_at_mods(DM_UP_HOVER,nx,ny,0,mods);
    if(button<2||side>=0){if(!down){pointer_probe("FINAL_MOVE",event,nx,ny);dm_touch_host_pc_mouse_mods((int)button,1,nx,ny,side,mods);}pointer_probe(down?"DOWN":"UP",event,nx,ny);dm_touch_host_pc_mouse_mods((int)button,down?0:2,nx,ny,side,mods);}
    else if(button<=4)dm_ui_pc_key(0x100+(int)button-2,dm_controls_virtual_modifiers(event),down);
    push();
}
static NSEvent *on_pc_mouse_event(NSEvent *event) {
    BOOL up=event.type==NSEventTypeLeftMouseUp||event.type==NSEventTypeRightMouseUp||event.type==NSEventTypeOtherMouseUp;
    if(up){pc_mouse_button(event,0);return nil;}
    if((event.modifierFlags&NSEventModifierFlagCommand)||dm_controls_text_responder(event.window.firstResponder)){controls_release();return event;}
    if(event.type==NSEventTypeMouseMoved||event.type==NSEventTypeLeftMouseDragged||event.type==NSEventTypeRightMouseDragged||event.type==NSEventTypeOtherMouseDragged){
        int mods=dm_controls_virtual_modifiers(event);g_mouse_have=mouse_point(event,g_view,1,&g_mouse_nx,&g_mouse_ny);dm_ui_ptr_at_mods(g_mouse_have?DM_UP_HOVER:DM_UP_END,g_mouse_nx,g_mouse_ny,0,mods);float nx,ny;
        if(mouse_point(event,g_view,0,&nx,&ny))for(int b=0;b<32;b++)if(g_pc_mouse_down[b]&&(b<2||g_pc_mouse_side[b]>=0))dm_touch_host_pc_mouse_mods(b,1,nx,ny,g_pc_mouse_side[b],mods);push();return nil;
    }
    if(event.type==NSEventTypeScrollWheel){on_scroll(event);return nil;}
    float nx,ny;if(!mouse_point(event,g_view,1,&nx,&ny))return event;
    pc_mouse_button(event,1);return nil;
}
static void on_key(NSEvent *event, int down) {
    unsigned short code = event.keyCode; if (code >= 128) return;
    if (down) { if (g_key_latch[code] >= 0) return; int action = dm_controls_lookup(dm_controls_event_input(event)); g_key_latch[code] = action; control_action(action, 1, event); }
    else { int action = g_key_latch[code]; g_key_latch[code] = -1; control_action(action, 0, event); }
}
static NSEvent *on_keyboard_event(NSEvent *event, NSWindow *win) {
    BOOL trace = getenv("DM_MAC_POINTER_LOG") != NULL;
    BOOL repeating = event.type == NSEventTypeKeyDown && event.isARepeat;
    int lookup = trace ? dm_controls_lookup(dm_controls_event_input(event)) : -1;
    int latched = event.keyCode < 128 ? g_key_latch[event.keyCode] : -1;
    NSEvent *(^finish)(BOOL,BOOL,NSString *) = ^NSEvent *(BOOL handled,BOOL returned,NSString *reason) {
        if (trace) { BOOL up = event.type == NSEventTypeKeyUp || (event.type == NSEventTypeFlagsChanged && !dm_controls_modifier_down(event));
            input_probe_write(@{@"phase":up ? @"KEY_UP" : @"KEY_DOWN",@"type":@(event.type),@"time":@(event.timestamp),@"keyCode":@(event.keyCode),@"modifierFlags":@(event.modifierFlags),@"eventWindow":@(event.windowNumber),@"gameWindow":@(win.windowNumber),@"keyWindow":@(NSApp.keyWindow.windowNumber),@"textResponder":@(dm_controls_text_responder(event.window.firstResponder)),@"lookupAction":@(lookup),@"latchedAction":@(latched),@"action":@(up ? latched : lookup),@"handled":@(handled),@"returned":@(returned),@"reason":reason,@"repeat":@(repeating),@"live":@(g_live)}); }
        return returned ? event : nil;
    };
    if (event.window != win) return finish(NO,YES,@"event_window");
    if (event.type == NSEventTypeKeyDown) display_present_interactive(win,"keyboard");
    if (event.type == NSEventTypeKeyUp) { if(g_live&&dm_ui_pc_desktop())on_pc_key(event,0);else on_key(event,0); return finish(YES,YES,@"release"); }
    if (dm_controls_text_responder(event.window.firstResponder) || event.modifierFlags & NSEventModifierFlagCommand) { controls_release(); return finish(NO,YES,@"native_text_or_command"); }
    if(g_live&&dm_ui_pc_desktop()){BOOL down=event.type==NSEventTypeFlagsChanged?dm_controls_modifier_down(event):event.type==NSEventTypeKeyDown;BOOL handled=on_pc_key(event,down);return finish(handled,!handled&&!repeating,repeating?@"pc_repeat":@"pc_native");}
    if (event.type == NSEventTypeFlagsChanged) {
        NSUInteger mask = event.keyCode == 56 || event.keyCode == 60 ? NSEventModifierFlagShift : event.keyCode == 59 || event.keyCode == 62 ? NSEventModifierFlagControl : event.keyCode == 58 || event.keyCode == 61 ? NSEventModifierFlagOption : 0;
        if (mask) on_key(event,dm_controls_modifier_down(event));
        return finish(mask != 0,YES,@"modifier");
    }
    if (!repeating) on_key(event,event.type == NSEventTypeKeyDown);
    return finish(!repeating,NO,repeating ? @"repeat" : @"game");
}

static int mouse_point(NSEvent *event, NSView *view, int pressed, float *nx, float *ny) {
    NSPoint location = event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp || event.type == NSEventTypeFlagsChanged ? [g_win mouseLocationOutsideOfEventStream] : event.locationInWindow;
    NSPoint point = [view convertPoint:location fromView:nil]; NSRect game = game_rect();
    if (game.size.width <= 0 || game.size.height <= 0) return 0;
    *nx = (float)((point.x - game.origin.x) / game.size.width);
    *ny = (float)(1.0 - (point.y - game.origin.y) / game.size.height);
    if (pressed && (*nx < 0 || *nx > 1 || *ny < 0 || *ny > 1)) return 0;
    if (g_full && g_live) {
        int rw = 0, rh = 0; double cx = 0, cy = 0, cw = 1, ch = 1;
        if (dm_screen_real(&rw, &rh) && rw > 0 && rh > 0) {
            double content_aspect = (double)rw / rh, game_aspect = game.size.width / game.size.height;
            if (fabs(game_aspect / content_aspect - 1) >= 0.01) {
                if (game_aspect > content_aspect) { cw = content_aspect / game_aspect; cx = (1 - cw) / 2; }
                else { ch = game_aspect / content_aspect; cy = (1 - ch) / 2; }
            }
        }
        float vx = (float)((*nx - cx) / cw), vy = (float)((*ny - cy) / ch);
        double fy = 0, fh = 1; ui_rect_frac(&fy, &fh);
        *nx = vx; *ny = fh > 0 ? (float)((vy - fy) / fh) : vy;
        return !(pressed && !dm_uiscreen_ingame() && (*nx < 0 || *nx > 1 || *ny < 0 || *ny > 1));
    }
    *nx = fminf(1, fmaxf(0, *nx)); *ny = fminf(1, fmaxf(0, *ny));
    return 1;
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
        full_setup(); display_load(NSScreen.mainScreen); NSInteger restored_mode = [g_display_prefs integerForKey:@"DisplayWindowMode"];
        NSRect r = NSMakeRect(0, 0, 1280, floor(1280 / g_aspect));
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: @"D2R Mobile"; win.restorable = NO; g_win = win;
        NSView *v = [[DmGameView alloc] initWithFrame:win.contentView.bounds]; win.contentView = v; g_view = v;
        v.wantsLayer = YES; v.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary; win.contentMinSize = NSMakeSize(640,360); win.tabbingMode = NSWindowTabbingModeDisallowed; NSWindow.allowsAutomaticWindowTabbing = NO;
        [win setFrameAutosaveName:@"DmMainGameWindow"]; BOOL restored_frame = [win setFrameUsingName:@"DmMainGameWindow"];
        display_load_window_frame(restored_frame);
        static DmWinCtl *ctl; ctl = [DmWinCtl new]; win.delegate = ctl;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        [v.layer addSublayer:g_layer]; layout_game();
        if (!restored_frame) [win center];
        g_live = getenv("DM_APP") || getenv("DM_TOUCH_LIVE"); if (g_live) dm_touch_host_live_enable();
        dm_controls_init(NULL); controls_release(); win.acceptsMouseMovedEvents = YES;
        if (!game_cursor_load(getenv("DM_MAC_CURSOR_DIR") ? @(getenv("DM_MAC_CURSOR_DIR")) : [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"cursors"])) LOG("Cursor: original sprites are missing; keeping the macOS cursor");
        { // 菜单栏：应用（退出 ⌘Q）/ 显示（全屏 ⌃⌘F）/ 帮助（键位对照） | Menu bar: application (Quit ⌘Q) / View (Full Screen ⌃⌘F) / Help (key mapping)
          NSMenu *bar = [NSMenu new];
          NSMenu *(^add)(const char *) = ^NSMenu *(const char *t) { NSMenuItem *it = [NSMenuItem new]; [bar addItem:it]; NSMenu *m = [[NSMenu alloc] initWithTitle:@(t)]; it.submenu = m; return m; };
          [[add(win.title.UTF8String) addItemWithTitle:[NSString stringWithFormat:@(ui("dmMacQuitFormat")), win.title] action:@selector(quit:) keyEquivalent:@"q"] setTarget:ctl];
          { NSMenuItem *item=[NSMenuItem new];item.submenu=controls_menu(ctl);[bar addItem:item]; }
          { NSMenuItem *item = [NSMenuItem new]; item.submenu = display_menu(ctl); [bar addItem:item]; }
          { NSMenu *hm = add(ui("dmAppMenuHelp")); [[hm addItemWithTitle:@(ui("dmAppKeysTitle")) action:@selector(keys:) keyEquivalent:@""] setTarget:ctl]; [[hm addItemWithTitle:@(ui("dmAppShareLog")) action:@selector(share:) keyEquivalent:@""] setTarget:ctl]; }
          NSApp.mainMenu = bar; }
        { void dm_gamepad_start(void (*on_mode)(int pad)); dm_gamepad_start(NULL); }   // 手柄（与 iOS 壳共用 ios/Sources/dm_gamepad.m）：连上即用，与键盘并存 | Gamepad (ios/Sources/dm_gamepad.m, shared with the iOS shell): works as soon as it is connected, alongside the keyboard
        [win makeKeyAndOrderFront:nil];
        display_window_mode(restored_mode >= 0 && restored_mode <= 2 ? (int)restored_mode : 0);
        screen_note();
        [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; screen_note(); }];
        [NSApp activateIgnoringOtherApps:YES];
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged handler:^NSEvent *(NSEvent *event) {
            return on_keyboard_event(event,win);
        }];
        NSEventMask mouseMask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp | NSEventMaskOtherMouseDragged | NSEventMaskMouseMoved | NSEventMaskScrollWheel;
        [NSEvent addLocalMonitorForEventsMatchingMask:mouseMask handler:^NSEvent *(NSEvent *event) {
            if (event.window != win || !g_live) return event;
            game_cursor_event(event);
            if(dm_ui_pc_desktop())return on_pc_mouse_event(event);
            BOOL released = event.type == NSEventTypeLeftMouseUp || event.type == NSEventTypeRightMouseUp || event.type == NSEventTypeOtherMouseUp;
            if (released && event.buttonNumber < 32) { int action = g_mouse_latch[event.buttonNumber]; g_mouse_latch[event.buttonNumber] = -1; control_action(action, 0, event); return nil; }
            if (event.modifierFlags & NSEventModifierFlagCommand || dm_controls_text_responder(event.window.firstResponder)) { controls_release(); return event; }
            if (event.type == NSEventTypeMouseMoved || event.type == NSEventTypeLeftMouseDragged || event.type == NSEventTypeRightMouseDragged || event.type == NSEventTypeOtherMouseDragged) {
                g_mouse_have = mouse_point(event, v, 1, &g_mouse_nx, &g_mouse_ny); push();
                float nx, ny; if (mouse_point(event, v, 0, &nx, &ny)) {
                    if (g_actions[DM_CTL_PRIMARY]) dm_touch_host_live(1, nx, ny);
                    if (g_actions[DM_CTL_SECONDARY]) dm_touch_host_live(4, nx, ny);
                    if (!g_actions[DM_CTL_PRIMARY] && !g_actions[DM_CTL_SECONDARY]) dm_touch_host_live(3,nx,ny);
                }
                return nil;
            }
            float inside_x, inside_y; if (!mouse_point(event, v, 1, &inside_x, &inside_y)) return event;
            if (event.type == NSEventTypeScrollWheel) {
                on_scroll(event); return nil;
            }
            NSUInteger button = event.buttonNumber; if (button >= 32) return event;
            BOOL down = event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeOtherMouseDown;
            if (down) { int action = dm_controls_lookup(dm_controls_event_input(event)); g_mouse_latch[button] = action; control_action(action, 1, event); }
            else { int action = g_mouse_latch[button]; g_mouse_latch[button] = -1; control_action(action, 0, event); }
            return nil;
        }];
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
              display_present_occlusion(win,on); }];
          double at = 0, dur = 0;
          if ((e = getenv("DM_TEST_COVER")) && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0)
              dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(at * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                  NSWindow *c = [[NSWindow alloc] initWithContentRect:NSInsetRect(win.frame, -40, -40) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
                  c.opaque = YES; c.backgroundColor = NSColor.grayColor; c.releasedWhenClosed = NO; [c orderFrontRegardless];
                  LOG("DM_TEST_COVER：盖上 %.1f 秒，游戏窗口%s是主窗口", dur, win.isKeyWindow ? "" : "不");
                  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(dur * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [c orderOut:nil]; LOG("DM_TEST_COVER：拿开"); });
              }); }
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; if (!win.miniaturized) dm_focus_set(2); controls_release(); { void dm_gamepad_reset(void); dm_gamepad_reset(); } }];   // 失焦：键盘和手柄按着的键清掉（松开事件收不到） | Focus lost: clear the keys held on keyboard and gamepad (their release events will not arrive)
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(1); display_present_interactive(win,"key window");dispatch_async(dispatch_get_main_queue(), ^{display_present_interactive(win,"focus confirmed");}); }];
        [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:NSApp queue:nil usingBlock:^(NSNotification *n) { (void)n;display_present_interactive(win,"application active"); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        dispatch_async(dispatch_get_main_queue(), ^{display_present_interactive(win,"visible startup");});
        // 客体线程：栈 64 MiB（Switch 主线程栈由 npdm 指定，宿主给足）
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

@implementation DmWinCtl (Controls)
- (void)menuNeedsUpdate:(NSMenu *)menu {controls_menu_update(menu,self);}
- (void)controls:(id)sender {
    (void)sender; controls_release();
    if(g_live&&dm_ui_pc_desktop()){
        if(!dm_ui_pc_profile_ready()||!dm_ui_pc_controls())[self controlsUnavailable];
    }else dm_controls_show(g_win, ^{ controls_release(); });
}
- (void)controlsUnavailable {
    NSAlert *alert=[NSAlert new];alert.messageText=@(ui("dmMacCharacterControls"));
    alert.informativeText=@(ui("dmMacCharacterControlsHelp"));
    [alert addButtonWithTitle:@(ui("dmAppOK"))];
    if(g_win)[alert beginSheetModalForWindow:g_win completionHandler:nil];else [alert runModal];
}
- (void)mouseControls:(id)sender {(void)sender;controls_release();dm_controls_show_mouse(g_win,mouse_reserved_input,^{controls_release();});}
@end

// 软键盘输入框（front.h）：主线程弹 NSAlert + 文本框，客体线程同步等结果。无窗口模式不弹（返回 -1，调用方按取消）
// Software keyboard input box (front.h): the main thread shows an NSAlert + text field and the guest thread waits synchronously for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel)
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    __block int r = 0;
    __block NSString *text = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        controls_release();
        NSAlert *a = [NSAlert new];
        a.messageText = title && *title ? @(title) : @(ui("dmMacTextTitle"));
        NSString *info = [NSString stringWithFormat:@"%@%@%@", sub ? @(sub) : @"", sub && *sub && guide && *guide ? @"\n" : @"", guide ? @(guide) : @""];
        a.informativeText = info;
        [a addButtonWithTitle:@(ui("dmConfirm"))]; [a addButtonWithTitle:@(ui("dmCancel"))];
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
