// 暂停菜单「分享诊断日志」的 iOS 侧。
// 按钮是游戏自己的控件（布局 tools/touch_layout.py 的 _pause_no_help：借原版「消息记录」按钮，换字、改发 PanelManager:ClosePanel:TouchShareLog），
// 宿主收到这条空操作消息后调这里登记的回调（dm/host/diag.c）。这里做两件：
//   ① 打一个文件：头部（构建号、机型、系统版本、温控 / 电量 / 内存 / 存储、本局与上一局的关键性能读数）+ 本局 boot.txt + 上一局 boot.previous.txt（main.m 每次启动前改名留下）；
//   ② 弹系统分享面板（UIActivityViewController），玩家自己选发到哪；iPad / Mac 上分享面板是弹出框，必须给锚点，否则一弹就崩。
// 隐私：只放这两份日志（游戏与宿主自己写的）和上面那几项机器读数；不取设备名、不取任何账号 / 位置 / 通讯录类信息。
// 测试开关 DM_TEST_SHARELOG_CLOSE=<秒>：分享面板照常弹出，过这么多秒自动收起；导出的文件另拷一份到 Documents/sharelog_test.txt 供回归核内容。
// iOS side of the pause menu's 「分享诊断日志」(Share Diagnostics).
// The button is the game's own widget (layout: _pause_no_help in tools/touch_layout.py: borrows the original game's 「消息记录」(Message Log) button, changes the text, and makes it send PanelManager:ClosePanel:TouchShareLog instead),
// and on receiving this no-op message the host calls the callback registered here (dm/host/diag.c). Two things are done here:
//   ① Build one file: header (build number, device model, OS version, thermal / battery / memory / storage, key performance readings of this run and the previous run) + this run's boot.txt + the previous run's boot.previous.txt (left by main.m's rename before each launch);
//   ② Present the system share sheet (UIActivityViewController), and the player chooses where to send it; on iPad / Mac the share sheet is a popover and must be given an anchor, otherwise it crashes as soon as it is presented.
// Privacy: only these two logs (written by the game and the host themselves) and the machine readings listed above are included; no device name, no account / location / contacts information of any kind.
// Test switch DM_TEST_SHARELOG_CLOSE=<seconds>: the share sheet is presented as usual and dismissed automatically after that many seconds; the exported file is also copied to Documents/sharelog_test.txt so regression can check its content.
#import <UIKit/UIKit.h>
#include <stdatomic.h>
#include <sys/sysctl.h>
#include <pwd.h>
#include <unistd.h>
#include "../../host/dm_i18n.h"
void plog(const char *fmt, ...);
void dm_sharelog_init(void (*cb)(void));   // dm/host/diag.c

// 日志里最后一条含 key 的行（不含行尾换行）；没有返回 nil
// The last line in the log containing key (without the trailing newline); returns nil if none
static NSString *last_line(NSData *d, const char *key) {
    if (!d.length) return nil;
    NSData *k = [NSData dataWithBytes:key length:strlen(key)];
    NSRange r = [d rangeOfData:k options:NSDataSearchBackwards range:NSMakeRange(0, d.length)]; if (r.location == NSNotFound) return nil;
    const char *b = d.bytes; NSUInteger s = r.location, e = r.location;
    while (s > 0 && b[s - 1] != '\n') s--;
    while (e < d.length && b[e] != '\n') e++;
    return [[NSString alloc] initWithBytes:b + s length:e - s encoding:NSUTF8StringEncoding] ?: @"（这一行不是合法 UTF-8）";
}
static void perf_block(NSMutableString *h, NSString *title, NSData *d) {
    [h appendFormat:@"\n—— %@ ——\n", title];
    if (!d) { [h appendString:@"（没有这份日志）\n"]; return; }
    static const char *keys[] = { "boot: 版本", "[诊断]", "[perf]", "[nvn] 上屏", "温控档位", "紫闪探针 至出图" };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) { NSString *l = last_line(d, keys[i]); [h appendFormat:@"%@\n", l ?: [NSString stringWithFormat:@"（没有「%@」行）", @(keys[i])]]; }
}
// 日志里的路径会带系统用户主目录（Mac 上是 /Users/<用户名>，iPhone 上是 /var/mobile）：导出时一律换成 ~，不把用户名带出去
// Paths in the log carry the system user's home directory (/Users/<username> on Mac, /var/mobile on iPhone): on export always replace it with ~ so the username does not leak out
static NSData *scrub_home(NSData *d) {
    struct passwd *pw = getpwuid(getuid()); const char *h = pw ? pw->pw_dir : NULL; size_t hn = h ? strlen(h) : 0;
    if (!d || hn < 2) return d;
    NSMutableData *o = [NSMutableData dataWithCapacity:d.length]; const uint8_t *b = d.bytes; size_t n = d.length, i = 0, from = 0;
    while (i + hn <= n) { if (b[i] == (uint8_t)h[0] && !memcmp(b + i, h, hn)) { [o appendBytes:b + from length:i - from]; [o appendBytes:"~" length:1]; i += hn; from = i; } else i++; }
    [o appendBytes:b + from length:n - from]; return o;
}
static NSDate *g_t0;   // 本次启动时刻（dm_sharelog_boot） | Time of this launch (dm_sharelog_boot)
static NSURL *build_file(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    NSData *(^rd)(NSString *) = ^NSData *(NSString *n) { return [NSData dataWithContentsOfFile:[doc stringByAppendingPathComponent:n] options:NSDataReadingMappedIfSafe error:nil]; };   // 映射着读：日志没有字节封顶，整读进内存再拼、再洗各拷一遍，峰值是日志的四倍 | read through a mapping: the log has no byte cap, and reading it whole into memory, then joining and scrubbing with one copy each, peaks at four times the log size
    NSData *cur = rd(@"boot.txt"), *prev = rd(@"boot.previous.txt");
    NSDictionary *inf = NSBundle.mainBundle.infoDictionary; NSString *build = [inf[@"CFBundleVersion"] description] ?: @"未知";
    NSDateFormatter *df = [NSDateFormatter new]; df.dateFormat = @"yyyy-MM-dd HH:mm:ss ZZZ";
    char hw[64] = "未知"; size_t hn = sizeof hw; sysctlbyname("hw.machine", hw, &hn, NULL, 0);
    UIDevice *dev = UIDevice.currentDevice; dev.batteryMonitoringEnabled = YES;
    NSArray<NSString *> *bst = @[ @"未知", @"放电", @"充电中", @"已充满" ]; int bs = (int)dev.batteryState;
    NSDictionary *fsa = [fm attributesOfFileSystemForPath:doc error:nil]; NSProcessInfo *pi = NSProcessInfo.processInfo;
    NSMutableString *h = [NSMutableString string];
    [h appendFormat:@"D2R Mobile Diagnostics\n导出时间 %@\n版本 %@ 构建 %@\n机型 %s 系统 %@\n", [df stringFromDate:NSDate.date], [inf[@"CFBundleShortVersionString"] description] ?: @"未知", build, hw, pi.operatingSystemVersionString];
    [h appendFormat:@"温控档位 %ld 低电量模式 %@ 电量 %.0f%% %@\n", (long)pi.thermalState, pi.lowPowerModeEnabled ? @"开" : @"关", dev.batteryLevel < 0 ? -1.0 : dev.batteryLevel * 100, bs >= 0 && bs <= 3 ? bst[bs] : @"未知"];   // 中文一律走 %@：NSString 的 %s 不按 UTF-8 解 | Chinese always goes through %@: NSString's %s does not decode as UTF-8
    [h appendFormat:@"物理内存 %.1f GB 可用存储 %.1f GB 本次已运行 %.0f 分钟\n", pi.physicalMemory / 1073741824.0, [fsa[NSFileSystemFreeSize] doubleValue] / 1073741824.0, -g_t0.timeIntervalSinceNow / 60];
    perf_block(h, @"本局关键读数（各取最后一条）", cur);
    perf_block(h, @"上一局关键读数（各取最后一条）", prev);
    NSMutableData *out = [[h dataUsingEncoding:NSUTF8StringEncoding] mutableCopy];
    void (^part)(NSString *, NSData *) = ^(NSString *t, NSData *d) {
        [out appendData:[[NSString stringWithFormat:@"\n\n===== %@（%@）=====\n", t, d ? [NSString stringWithFormat:@"%lu 字节", (unsigned long)d.length] : @"没有这份日志"] dataUsingEncoding:NSUTF8StringEncoding]];
        if (d) [out appendData:d]; };
    part(@"本局日志 boot.txt", cur); part(@"上一局日志 boot.previous.txt", prev);
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"sharelog"];   // 只放这一份导出：每次重建这个目录（我们自己的，不碰别的） | Hold only this one export: rebuild this directory every time (it is our own; nothing else is touched)
    [fm removeItemAtPath:dir error:nil]; [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    df.dateFormat = @"yyyyMMdd_HHmmss";
    NSString *p = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@(dm_ui("dmShareLogFile")), build, [df stringFromDate:NSDate.date]]];
    NSData *fin = scrub_home(out);
    if (![fin writeToFile:p atomically:YES]) { plog("诊断日志：【红】导出文件写不进 %s", p.UTF8String); return nil; }
    plog("诊断日志：已打包 %s（%lu 字节：本局 %lu、上一局 %s）", p.lastPathComponent.UTF8String, (unsigned long)fin.length, (unsigned long)cur.length,
         prev ? [NSString stringWithFormat:@"%lu", (unsigned long)prev.length].UTF8String : "没有");
    if (getenv("DM_TEST_SHARELOG_CLOSE")) [fin writeToFile:[doc stringByAppendingPathComponent:@"sharelog_test.txt"] atomically:YES];
    return [NSURL fileURLWithPath:p];
}
static UIViewController *top_vc(void) {   // 同 front_ios.m 系统输入框找宿主的顺序：主窗口 → 前台活跃场景的窗口 → 任一窗口，再顺着已弹出的往上找 | Same order as front_ios.m's system input box uses to find its host: main window → a window of the foreground active scene → any window, then walk up through what is already presented
    UIViewController *vc = nil, *alt = nil, *any = nil;
    for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
        if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) {
            if (!w.rootViewController) continue;
            if (w.isKeyWindow) vc = w.rootViewController;
            else if (s.activationState == UISceneActivationStateForegroundActive && !alt) alt = w.rootViewController;
            else if (!any) any = w.rootViewController; }
    vc = vc ?: alt ?: any; while (vc.presentedViewController) vc = vc.presentedViewController;
    return vc;
}
static _Atomic int g_busy;   // 打包 / 分享面板开着时再点不叠第二个 | Tapping again while packaging / the share sheet is open does not stack a second one
static void on_click(void) {   // 宿主在游戏主线程调：玩家点了「分享诊断日志」 | Called by the host on the game main thread: the player tapped 「分享诊断日志」(Share Diagnostics)
    if (atomic_exchange(&g_busy, 1)) { plog("诊断日志：分享面板还开着，这次点击不再弹"); return; }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSURL *u = build_file();
        dispatch_async(dispatch_get_main_queue(), ^{
            UIViewController *vc = u ? top_vc() : nil;
            if (!vc) { if (u) plog("诊断日志：【红】没找到可弹分享面板的界面"); atomic_store(&g_busy, 0); return; }
            UIActivityViewController *a = [[UIActivityViewController alloc] initWithActivityItems:@[u] applicationActivities:nil];
            a.completionWithItemsHandler = ^(UIActivityType t, BOOL done, NSArray *items, NSError *e) { (void)items;
                plog("诊断日志：分享面板收起（%s%s%s）", done ? "已发到 " : "没发", done ? (t ?: @"未知").UTF8String : "", e ? [NSString stringWithFormat:@"，错误 %@", e.localizedDescription].UTF8String : "");
                atomic_store(&g_busy, 0); };
            UIPopoverPresentationController *pp = a.popoverPresentationController;   // iPad / Mac 上是弹出框：锚在画面正中（暂停菜单所在处），不画箭头 | On iPad / Mac it is a popover: anchored at the center of the picture (where the pause menu is), no arrow drawn
            if (pp) { pp.sourceView = vc.view; pp.sourceRect = CGRectMake(CGRectGetMidX(vc.view.bounds), CGRectGetMidY(vc.view.bounds), 1, 1); pp.permittedArrowDirections = 0; }
            [vc presentViewController:a animated:YES completion:^{
                plog("诊断日志：分享面板已弹出（%s，%s）", pp ? "弹出框" : "底部面板", NSStringFromCGRect(a.view.frame).UTF8String);
                const char *tc = getenv("DM_TEST_SHARELOG_CLOSE"); if (!tc) return;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(tc) * 1e9)), dispatch_get_main_queue(), ^{
                    [a dismissViewControllerAnimated:NO completion:^{ plog("诊断日志：测试开关 DM_TEST_SHARELOG_CLOSE 到点收起分享面板"); atomic_store(&g_busy, 0); }]; });
            }];
        });
    });
}
void dm_sharelog_boot(void) { g_t0 = NSDate.date; dm_sharelog_init(on_click); }   // main.m 起宿主前调一次 | main.m calls this once before starting the host
void dm_sharelog_share(void) { on_click(); }   // 游戏外的界面直接调（资源下载页的「分享诊断日志」按钮，main.m AssetVC） | called directly from screens outside the game (the "Share diagnostic log" button on the asset download page, main.m AssetVC)
