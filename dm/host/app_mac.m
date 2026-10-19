// macOS App 形态（双击起）：宿主被包成 <名>.app 时，启动最前面替玩家把命令行那套准备好——
//   窗口开着；存档放 ~/Library/Application Support/D2R Mobile/save（与开发用的工作区、测试存档都不相干）；
//   游戏资源文件夹第一次启动时请玩家选（记在偏好里，下次不再问；选的不是资源文件夹给明确报错、重选）；
//   程序文件（改写后的游戏程序，产物目录）随包放在 Contents/Resources/product；日志写到 ~/Library/Logs/D2R Mobile/host.log。
// 命令行起（闸门、截图、脚本）不走这里：不在 .app 包里，或带了参数，一律原样。打包脚本 dm/tools/mac_app.sh。
// macOS App form (started by double-click): when the host is packaged as <name>.app, the very first thing at startup is to prepare the command-line setup on the player's behalf --
//   the window is on; saves go to ~/Library/Application Support/D2R Mobile/save (unrelated to the development workspace and to test saves);
//   the game resource folder is chosen by the player on first launch (remembered in preferences and not asked again; choosing something that is not a resource folder gives a clear error and another try);
//   the program files (the rewritten game program, the build output directory) are in Contents/Resources/product inside the package; the log is written to ~/Library/Logs/D2R Mobile/host.log.
// Command-line starts (gates, screenshots, scripts) do not come here: not inside an .app package, or with arguments, everything stays as it was. Packaging script: dm/tools/mac_app.sh.
#import <Cocoa/Cocoa.h>
#include <sys/stat.h>
#include "hle.h"
#include "dm_i18n.h"
#include "dm_custom.h"
#include "app_mac_overlay.h"
#define T(k) (@(dm_ui(k)))

#ifndef APP_NAME
#define APP_NAME "D2R Mobile"   // 存档与日志目录名；老接口版构建时另给（host/Makefile CFLAGS_M3），两个包不共用存档 | name of the save and log directory; given separately when building the older-API version (host/Makefile CFLAGS_M3), so the two packages do not share saves
#endif
static NSString *const kRomfsKey = @"RomfsDir";

// 提示文字里带一个参数（中英表里的格式是 C 的 %s）：按 UTF-8 拼。-[NSString stringWithFormat:] 的 %s 按系统旧编码解读字节，
// 中文文件夹名（的「D2R Mobile」）在提示框里显示成空的引号
// The prompt text carries one argument (the format in the i18n table is C %s): concatenated as UTF-8. The %s of -[NSString stringWithFormat:] interprets bytes in the legacy system encoding,
// so a Chinese folder name (e.g. "D2R Mobile" in Chinese) showed up as empty quotes in the alert
static NSString *fmt1(const char *key, NSString *arg) {
    const char *dm_ui(const char *key); char b[1024]; snprintf(b, sizeof b, dm_ui(key), arg.UTF8String ?: ""); return @(b) ?: @"";
}
static NSString *g_sup;   // 应用支持目录（dm_app_bootstrap 里定）：选的是 data 那一层、文件夹名又不叫 data 时，在这里建个壳目录 | Application Support directory (set in dm_app_bootstrap): when the pick is the data level and the folder is not named data, a shell directory is created here
// 这个文件夹是不是游戏资源。只看 build_manifest.psv 在哪，两种摆法都认（选了一个直接装着各个 .data 和 build_manifest.psv 的文件夹，被拒）：
//   ① 选的是资源根（里面有 data/build_manifest.psv）；选到上一两层（解包目录的根、Update）也认，往里找一层；
//   ② 选的就是 data 那一层（文件夹里直接有 build_manifest.psv）：文件夹名就叫 data → 用它的上一层；名字不叫 data → 在应用支持目录里建壳目录
//      romfs_flat，里面一个指向它的 data 链接，壳目录当资源根（游戏读「根/data/…」的写法不变）。返回资源根，不对返回 nil 并给出原因
// Whether this folder is the game resources. Only looks at where build_manifest.psv is; both layouts are accepted (picking a folder that directly holds the .data files and build_manifest.psv used to be rejected):
//   ① The pick is the resource root (contains data/build_manifest.psv); picking one or two levels above (the root of the unpacked directory, Update) is also accepted, searching one level down;
//   ② The pick is the data level itself (build_manifest.psv directly inside the folder): if the folder is named data → use its parent; if not named data → create a shell directory
//      romfs_flat in the Application Support directory containing a data link pointing to it, and use the shell directory as the resource root (the game's "root/data/..." reads stay unchanged). Returns the resource root; otherwise returns nil and gives the reason
static NSString *romfs_root(NSString *dir, NSString **why) {
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *sub in @[@"", @"RomFS", @"romfs", @"Update/RomFS"]) {
        NSString *d = sub.length ? [dir stringByAppendingPathComponent:sub] : dir;
        if ([fm fileExistsAtPath:[d stringByAppendingPathComponent:@"data/build_manifest.psv"]]) {
            if (![fm isReadableFileAtPath:[d stringByAppendingPathComponent:@"data/build_manifest.psv"]]) { if (why) *why = T("dmAppNoRead"); return nil; }
            return d;
        }
    }
    NSString *m = [dir stringByAppendingPathComponent:@"build_manifest.psv"];
    if ([fm fileExistsAtPath:m]) {
        if (![fm isReadableFileAtPath:m]) { if (why) *why = T("dmAppNoRead"); return nil; }
        if ([dir.lastPathComponent isEqualToString:@"data"]) return dir.stringByDeletingLastPathComponent;
        if (g_sup) { NSString *shim = [g_sup stringByAppendingPathComponent:@"romfs_flat"], *link = [shim stringByAppendingPathComponent:@"data"];
            [fm createDirectoryAtPath:shim withIntermediateDirectories:YES attributes:nil error:nil];
            NSString *cur = [fm destinationOfSymbolicLinkAtPath:link error:nil];
            if (![cur isEqualToString:dir]) { [fm removeItemAtPath:link error:nil]; [fm createSymbolicLinkAtPath:link withDestinationPath:dir error:nil]; }
            if ([fm fileExistsAtPath:[link stringByAppendingPathComponent:@"build_manifest.psv"]]) { LOG("App：选的是 data 那一层（%s），经壳目录 %s 当资源根", dir.UTF8String, shim.UTF8String); return shim; } }
    }
    if (why) *why = fmt1("dmAppNotRomfs", dir.lastPathComponent);
    return nil;
}
static void alert(NSString *title, NSString *text) {
    NSAlert *a = [NSAlert new]; a.messageText = title; a.informativeText = text; [a addButtonWithTitle:T("dmAppOK")]; [a runModal];
}
// 请玩家选资源文件夹，直到选对或取消。返回资源根目录，取消返回 nil
// Ask the player to choose the resource folder until it is right or cancelled. Returns the resource root, or nil on cancel
static NSString *pick_romfs(NSString *first_why) {
    NSString *why = first_why;
    for (;;) {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseDirectories = YES; p.canChooseFiles = NO; p.allowsMultipleSelection = NO; p.prompt = T("dmAppPickUse");
        p.message = why ? fmt1("dmAppPickAgain", why) : T("dmAppPickMsg");
        if ([p runModal] != NSModalResponseOK) return nil;
        NSString *w = nil, *r = romfs_root(p.URL.path, &w);
        if (r) return r;
        alert(T("dmAppNotRomfsTitle"), w); why = w;
    }
}

// 包里的一项（product / msl / romfs_overlay）：实物在 Contents/Resources/<名> 就用它；没有就看 Resources/paths.txt 里「名=本机路径」那一行
// （mac_app.sh 的「不带派生物」出法：改写后的游戏程序、着色器库、界面覆盖层都从游戏内容派生，公开发布的包不带，指向本机自己构建出来的）
// One item of the package (product / msl / romfs_overlay): if the real thing is in Contents/Resources/<name> it is used; otherwise the line "name=local path" in Resources/paths.txt is consulted
// (the "without derived content" build of mac_app.sh: the rewritten game program, the shader library and the UI overlay are all derived from game content, a publicly released package does not carry them and points at what this machine built itself)
static NSString *res_item(NSString *name) {
    NSFileManager *fm = NSFileManager.defaultManager; NSString *rp = NSBundle.mainBundle.resourcePath, *d = [rp stringByAppendingPathComponent:name];
    if ([fm fileExistsAtPath:d]) return d;
    NSString *txt = [NSString stringWithContentsOfFile:[rp stringByAppendingPathComponent:@"paths.txt"] encoding:NSUTF8StringEncoding error:nil];
    for (NSString *ln in [txt componentsSeparatedByString:@"\n"]) { NSString *pre = [name stringByAppendingString:@"="]; if ([ln hasPrefix:pre]) { d = [ln substringFromIndex:pre.length]; return [fm fileExistsAtPath:d] ? d : nil; } }
    return nil;
}
static void upd_start(void);   // 更新检查（在文件后面） | update check (later in this file)
// main() 最前面调。是 App 形态就把环境与参数备好并返回 1；不是返回 0、什么都不动
// Called at the very start of main(). In App form it prepares the environment and arguments and returns 1; otherwise it returns 0 and touches nothing
int dm_app_bootstrap(int *argc, char ***argv) {
    @autoreleasepool {
        NSString *bp = NSBundle.mainBundle.bundlePath;
        if (*argc > 1 || ![bp.pathExtension isEqualToString:@"app"]) return 0;
        NSFileManager *fm = NSFileManager.defaultManager;
        if (!getenv("DM_LANG")) setenv("DM_LANG", [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"] ? "zh-Hans" : "en", 1);   // 命令行宿主一律简中；App 形态跟系统首选语言（提示框与游戏内同一个开关） | The command-line host is always Simplified Chinese; the App form follows the system's preferred language (dialogs and in-game text use the same switch)
        NSString *sup = [[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@APP_NAME] copy];
        if (getenv("DM_APP_SUPPORT")) sup = @(getenv("DM_APP_SUPPORT"));   // 测试：存档等放到指定目录，不碰玩家那份 | Test: put saves and the rest in a given directory, leaving the player's own untouched
        NSString *logd = [[NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"Logs/" APP_NAME] copy];
        NSString *save = [sup stringByAppendingPathComponent:@"save"], *temp = [sup stringByAppendingPathComponent:@"temp"];
        for (NSString *d in @[sup, logd, save, temp]) [fm createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
        g_sup = sup;
        // 双击起没有终端：日志落文件，上一次的留一份
        // A double-click start has no terminal: the log goes to a file, and the previous one is kept
        NSString *lf = [logd stringByAppendingPathComponent:@"host.log"];
        [fm removeItemAtPath:[logd stringByAppendingPathComponent:@"host.prev.log"] error:nil];
        [fm moveItemAtPath:lf toPath:[logd stringByAppendingPathComponent:@"host.prev.log"] error:nil];
        if (!isatty(2)) { freopen(lf.fileSystemRepresentation, "w", stderr); setvbuf(stderr, NULL, _IOLBF, 0); }
        // 不要系统的「窗口状态恢复」：上次被强退 / 崩过之后，系统会在启动时弹「上次意外退出，要重新打开窗口吗」的模态框——游戏没有可恢复的窗口，
        // 框却挡着主窗口收不到鼠标键盘（游戏在后面照跑）。等同于带启动参数 -ApplePersistenceIgnoreState YES；要在建 NSApplication 之前设
        // No system "window state restoration": after a forced quit / crash the system shows a modal "the app quit unexpectedly, reopen windows?" at launch - the game has no window to restore,
        // yet the dialog covers the main window, which then receives no mouse or keyboard (the game keeps running behind it). Equivalent to the launch argument -ApplePersistenceIgnoreState YES; must be set before NSApplication is created
        [NSUserDefaults.standardUserDefaults setVolatileDomain:@{ @"ApplePersistenceIgnoreState": @YES } forName:NSArgumentDomain];
        [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular]; [NSApp activateIgnoringOtherApps:YES];
        NSString *prod = res_item(@"product");
        if (!prod || ![fm fileExistsAtPath:[prod stringByAppendingPathComponent:@"main.patched.bin"]]) {
            alert(T("dmAppBrokenTitle"), T("dmAppBroken")); exit(1);
        }
        NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
        // 包标识换过（内测正式包为了 iCloud 改用与 iOS 包同一个标识）：偏好按包标识分域，选过的资源文件夹、看过键位对照这两项从旧域（公开口径的标识）接过来一次，免得再选一遍
        // The bundle identifier has changed (the internal build switched to the same identifier as the iOS package for iCloud): preferences are scoped by bundle identifier, so the two items - the chosen resource folder and having seen the key reference - are carried over once from the old domain (the identifier of public builds), to avoid choosing again
        if (![ud stringForKey:kRomfsKey] && ![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"org.d2rmobile.mac"]) { NSUserDefaults *old = [[NSUserDefaults alloc] initWithSuiteName:@"org.d2rmobile.mac"]; NSString *r = [old stringForKey:kRomfsKey];
            if (r) { [ud setObject:r forKey:kRomfsKey]; if ([old boolForKey:@"KeysShown"]) [ud setBool:YES forKey:@"KeysShown"]; LOG("App：从旧包标识的偏好里接过资源文件夹 %s", r.UTF8String); } }
        NSString *saved = [ud stringForKey:kRomfsKey], *why = nil, *rom = saved ? romfs_root(saved, &why) : nil;
        if (getenv("DM_APP_ROMFS")) {   // 测试（没人点对话框时用）：直接给资源文件夹；不对就把同一句报错写进日志并退出，不弹框、不记进偏好 | Test (for when nobody is there to click the dialog): give the resource folder directly; if it is wrong, write the same error sentence to the log and exit, with no dialog and nothing saved to preferences
            rom = romfs_root(@(getenv("DM_APP_ROMFS")), &why);
            if (!rom) { LOG("【红】App：DM_APP_ROMFS 给的不是游戏资源文件夹：%s", why.UTF8String); exit(2); }
        } else if (!rom) {
            rom = pick_romfs(saved ? fmt1("dmAppRomfsGone", why) : nil);
            if (!rom) { LOG("App：没有选资源文件夹，退出"); exit(0); }
            [ud setObject:rom forKey:kRomfsKey];
        }
        { void dm_custom_init(const char *dir); dm_custom_init(sup.fileSystemRepresentation); }   // 自定义设置文件的目录现在就给（同 iOS 壳）：不给的话要等存档挂上才知道在哪，而语言开关正是在挂存档那一刻读它改游戏语言的——晚了开关就不生效 | the directory of the custom settings file is given right now (same as the iOS shell): otherwise it is only known once the save is mounted, and the language switch is read to change the game language at exactly that moment - too late and the switch has no effect
        if (!getenv("DM_TOUCH_DEFAULTS")) setenv("DM_TOUCH_DEFAULTS", "1", 1);   // 产品缺省同 iOS 包（hle_fs.c settings_seed：玩家没碰过的设置键补上——小地图放右上角、进局自动打开、显示物品用开关式等；已有的键不动）。命令行宿主缺省不做，那边要对 Switch 真值比图 | product defaults same as the iOS package (hle_fs.c settings_seed: fills in settings keys the player has never touched - minimap in the top-right corner, opened automatically on entering a game, show-items as a toggle, and so on; existing keys are left alone). The command-line host does not do this by default, since it has to be compared against Switch ground-truth images
        if (!getenv("DM_APP_SUPPORT")) setenv("DM_CLOUD_REAL", "1", 1);   // iCloud 同步（ios/Sources/cloud_ios.m，两端共用）只认玩家真实的存档目录；测试用的支持目录一律不同步 | iCloud sync (ios/Sources/cloud_ios.m, shared by both platforms) only recognises the player's real save directory; a support directory used for testing is never synced
        setenv("DM_WINDOW", "1", 1); setenv("DM_APP", "1", 1);
        if (!getenv("DM_AUDIO")) setenv("DM_AUDIO", "1", 1);   // 产品要出声（hle_audio.c 在 Mac 上的缺省是给命令行回放 / 闸门的静音接收端）；无人值守测试显式 DM_AUDIO=0 | the product must play sound (on the Mac, hle_audio.c defaults to the silent sink used for command-line replays / gates); unattended tests set DM_AUDIO=0 explicitly
        setenv("DM_SAVE", save.fileSystemRepresentation, 1); setenv("DM_TEMP", temp.fileSystemRepresentation, 1);
        setenv("DM_ROMFS", rom.fileSystemRepresentation, 1); setenv("DM_ROOT", sup.fileSystemRepresentation, 1);
        NSString *shmap = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"shader_map.tsv"];   // 图形后端的着色器映射表：命令行起时按可执行文件旁的源码树找，包里放在 Resources | The graphics backend's shader mapping table: when started from the command line it is found via the source tree next to the executable; in the package it is in Resources
        if ([fm fileExistsAtPath:shmap]) setenv("DM_NVN_SHMAP", shmap.fileSystemRepresentation, 1);
        NSString *msl = res_item(@"msl");   // 编好的着色器库（msl/b-<翻译链版本>/） | The compiled shader library (msl/b-<translation chain version>/)
        if (msl) setenv("DM_MSL_DIR", msl.fileSystemRepresentation, 1);
        BOOL missing_overlay = NO;
        NSString *ov = dm_mac_overlay(res_item(@"romfs_overlay"), res_item(@"romfs_overlay_variants"), dm_custom_get, &missing_overlay);
        if (missing_overlay) LOG("App：缺少已启用自定义功能的覆盖层组合，暂用基础覆盖层；请重新生成并打包覆盖层");
        if (ov) setenv("DM_ROMFS_OVERLAY", ov.fileSystemRepresentation, 1);
        static char *av[5]; av[0] = (*argv)[0]; av[1] = strdup(prod.fileSystemRepresentation); av[2] = "main"; av[3] = "sdk"; av[4] = NULL;
        *argv = av; *argc = 4;
        upd_start();
        LOG("App：存档 %s；资源 %s；程序文件 %s；界面覆盖层 %s；日志 %s", save.UTF8String, rom.UTF8String, prod.UTF8String, getenv("DM_ROMFS_OVERLAY") ?: "（没有，原版界面）", lf.UTF8String);
        return 1;
    }
}

// ---- 更新检查 ----
// 包的 Info.plist 里带了 SWUpdateURL（打包脚本 DM_MAC_UPDATE_URL；内测包指向下载站的 latest_mac.json）才检查；公开版 / 自己构建的包不带 = 不联网、不提示。
// 版本文件：{"build": 整数构建号, "note": "一句说明", "note_en": "English", "url": "下载页"}。主菜单出现时读一次（两次之间至少隔 60 秒），2 秒超时；
// build 比包内构建号大 → 交给两端共用的提示核心（host/dm_update.c：手柄版主菜单右上角「构建 N」下面一行「有新版本 N · 说明」，按 Y / 键盘 I 打开下载页）；
// 读不到 / 超时 / 格式不对一律静默（只记日志），不影响启动。不自动下载、不替换。下载页地址只认与版本文件同一个主机的 http(s) 地址
// ---- Update check ----
// Checked only when the package's Info.plist carries SWUpdateURL (the packaging script's DM_MAC_UPDATE_URL); public builds / packages you build yourself do not carry it = no network access, no hint.
// Version file: {"build": integer build number, "note": "one sentence", "note_en": "English", "url": "download page"}. Read once when the main menu appears (at least 60 seconds between two reads), with a 2-second timeout;
// build larger than the package's build number -> handed to the hint core shared by both platforms (host/dm_update.c: one line "New version N - note" under "Build N" at the top right of the gamepad-layout main menu; Y / keyboard I opens the download page);
// unreadable / timed out / malformed are all silent (logged only) and do not affect startup. Nothing is downloaded or replaced automatically. The download page address is accepted only if it is an http(s) address on the same host as the version file
void dm_update_init(int own_build, void (*on_select)(void), void (*on_click)(void)); void dm_update_offer(int new_build, const char *note, int urgent); void dm_update_safe_left(int units);
static NSString *g_upd_url, *g_upd_page; static int g_upd_own; static double g_upd_last;
static void upd_check(void) {
    double now = CFAbsoluteTimeGetCurrent(); if (g_upd_last && now - g_upd_last < 60) return; g_upd_last = now;
    NSURL *u = [NSURL URLWithString:g_upd_url]; if (!u) return;
    NSURLSessionConfiguration *cf = NSURLSessionConfiguration.ephemeralSessionConfiguration; cf.timeoutIntervalForRequest = 2; cf.timeoutIntervalForResource = 2; cf.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    [[[NSURLSession sessionWithConfiguration:cf] dataTaskWithURL:u completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
        NSInteger code = [r isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)r).statusCode : 0;
        NSDictionary *j = (!e && code == 200 && d) ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
        NSNumber *b = [j isKindOfClass:NSDictionary.class] ? j[@"build"] : nil;
        if (![b isKindOfClass:NSNumber.class] || b.intValue < 1) { LOG("更新：读版本文件没成（%s，HTTP %ld），本次不提示", e ? e.localizedDescription.UTF8String : "格式不对", (long)code); return; }
        NSString *zh = [j[@"note"] isKindOfClass:NSString.class] ? j[@"note"] : @"", *en = [j[@"note_en"] isKindOfClass:NSString.class] ? j[@"note_en"] : zh, *pg = [j[@"url"] isKindOfClass:NSString.class] ? j[@"url"] : nil;
        NSURL *pu = pg ? [NSURL URLWithString:pg] : nil; BOOL okpg = pu && ([pu.scheme isEqualToString:@"https"] || [pu.scheme isEqualToString:@"http"]) && [pu.host isEqualToString:u.host];
        g_upd_page = okpg ? [pg copy] : nil;
        if (b.intValue > g_upd_own) { const char *l = getenv("DM_LANG"); NSString *note = (l && !strncmp(l, "zh", 2)) ? zh : en; LOG("更新：最新构建 %d 比自己（%d）新 → 主菜单提示（下载页 %s）", b.intValue, g_upd_own, okpg ? "有" : "没给或不同主机，不可点"); dm_update_offer(b.intValue, note.UTF8String, 0); }
        else { LOG("更新：最新构建 %d，不比自己（%d）新，不提示", b.intValue, g_upd_own); dm_update_offer(0, "", 0); }
    }] resume];
}
static void upd_click(void) { NSString *pg = g_upd_page; if (!pg) return; dispatch_async(dispatch_get_main_queue(), ^{ LOG("更新：打开下载页"); if (!getenv("DM_TEST_UPDATE_NOOPEN")) [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:pg]]; }); }
static void upd_start(void) {
    NSDictionary *inf = NSBundle.mainBundle.infoDictionary; const char *te = getenv("DM_TEST_UPDATE_URL");   // 测试：换一个版本文件地址（本机起的小服务 / 不存在的地址） | test: use a different version file address (a small local server / a non-existent address)
    g_upd_url = te ? @(te) : [inf[@"SWUpdateURL"] description]; g_upd_own = [[inf[@"CFBundleVersion"] description] intValue];
    if (!g_upd_url.length || g_upd_own < 1) { LOG("更新：包里没配版本文件地址，不检查"); return; }
    dm_update_init(g_upd_own, upd_check, upd_click); dm_update_safe_left(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ upd_check(); });   // 启动时查一次：手柄版主菜单不发「选角页出现」那条消息（核心靠它叫外壳去查），不能只等回调；回调若来了也照查（两次之间隔 60 秒） | check once at launch: the gamepad-layout main menu does not send the "character selection appeared" message (which the core relies on to ask the shell to check), so waiting for the callback alone is not enough; if the callback does come it checks as well (at least 60 seconds apart)
}

// 手柄代码（ios/Sources/dm_gamepad.m，两边共用）问「现在是不是面板 / 读取画面开着」：只用来决定空着的 ZR 格要不要当免费回城。
// 宿主没有 iOS 壳那套面板栈；一律答「是」= ZR 整颗交给原版（面板里它是翻页签键），手柄上的免费回城这一版没有（记在已知不足里）
// The gamepad code (ios/Sources/dm_gamepad.m, shared by both sides) asks "is a panel / loading screen open right now": used only to decide whether the empty ZR slot should act as the free town portal.
// The host has no panel stack like the iOS shell; it always answers "yes" = ZR is handed entirely to the original game (in panels it is the tab-switch key), so the free town portal on the gamepad is absent in this version (listed under known gaps)
int dm_front_navigation(void) { return 1; }
