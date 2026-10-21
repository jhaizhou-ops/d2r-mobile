// 壳专用（不含游戏内容的公开版安装包，tools/shell_build.sh）：首次启动把全部着色器一次编完，带进度。内测 / 自己用 Xcode 构建的包不编这个文件的内容。
// 为什么要有：使用者的电脑上没有苹果的着色器编译器时，着色器是以源码放进安装包的（tools/d2rm_pack.py、gfx/tools/nometal_run.py），
// 设备上用到哪个现编哪个。真机实测：平均约 80 毫秒一个、单个最长约 450 毫秒；边玩边编的话第一次进一个新场景会顿约 0.8 秒
// （编好的库同一处是 0.12 秒）。所以改成进主菜单之前一次编完——系统会把编好的结果缓存，之后游戏里再建同一个库不花时间。
// 做完的记号放在 Library/Caches（和系统的着色器缓存在同一处，系统清缓存时一起没，下次启动自动重做一遍）。
// 中途切到后台 / 被杀：已编的在系统缓存里，回来从头数一遍，已编的每个只要不到 1 毫秒，等于接着做。
// Shell only (the public install package with no game content, tools/shell_build.sh): on first launch compile all shaders in one go, with progress. Beta packages / packages you build yourself with Xcode do not compile this file's contents.
// Why it exists: when the user's computer has no Apple shader compiler, the shaders are put into the install package as source (tools/d2rm_pack.py, gfx/tools/nometal_run.py),
// and each is compiled on the device when it is first used. Measured on a real device: about 80 ms each on average, about 450 ms for the longest single one; compiling while playing, the first entry into a new scene stutters for about 0.8 seconds
// (0.12 seconds at the same spot with compiled libraries). So it is changed to compiling everything once before entering the main menu -- the system caches the compiled results, and building the same library again in the game afterwards takes no time.
// The completion marker is placed in Library/Caches (the same place as the system's shader cache, so it goes away together when the system clears caches, and the next launch automatically redoes it once).
// Sent to the background / killed midway: what was compiled is in the system cache; on return it counts from the start, each already compiled one taking less than 1 ms, which amounts to resuming.
#ifdef DM_SHELL
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static void pl(NSString *s) { fprintf(stderr, "[dm] %s\n", s.UTF8String); }
static int lg(void) { NSString *l = NSLocale.preferredLanguages.firstObject; return [l hasPrefix:@"zh"] ? 1 : [l hasPrefix:@"es"] ? 2 : 0; }   // 系统首选语言：1 中文 / 2 西语 / 0 其余英文 | System preferred language: 1 Chinese / 2 Spanish / 0 English otherwise
#define L3(zh, en, es) (lg() == 1 ? (zh) : lg() == 2 ? (es) : (en))
static NSString *libdir(void) {   // <包>/msl/b-<链版本>：壳里只有一个版本目录 | <package>/msl/b-<chain version>: the shell has only one version directory
    NSString *msl = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"msl"];
    for (NSString *d in [NSFileManager.defaultManager contentsOfDirectoryAtPath:msl error:nil] ?: @[]) if ([d hasPrefix:@"b-"]) return [msl stringByAppendingPathComponent:d];
    return nil;
}
static NSString *marker(NSString *dir) {
    NSString *c = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    return [c stringByAppendingPathComponent:[NSString stringWithFormat:@"dm_shader_prep_%@_%@.done", dir.lastPathComponent, NSProcessInfo.processInfo.operatingSystemVersionString.length ? @(NSProcessInfo.processInfo.operatingSystemVersion.majorVersion * 100 + NSProcessInfo.processInfo.operatingSystemVersion.minorVersion).stringValue : @"0"]];
}
static BOOL is_source(NSString *path) {   // 编好的库以 MTLB 开头；不是的就是源码 | a compiled library starts with MTLB; anything else is source
    FILE *f = fopen(path.fileSystemRepresentation, "rb"); if (!f) return NO; char h[4] = {0}; size_t n = fread(h, 1, 4, f); fclose(f);
    return n == 4 && memcmp(h, "MTLB", 4) != 0;
}

// 要不要做：返回 1 = 已接手（显示进度页，做完调 done）；0 = 不用做（没有源码着色器，或上次已经做完）
// Whether it needs to run: returns 1 = taken over (shows the progress page, calls done when finished); 0 = nothing to do (no source shaders, or already finished last time)
int dm_shader_prep_begin(void (^done)(void)) {
    NSString *dir = libdir(); if (!dir) return 0;
    NSString *mk = marker(dir); if ([NSFileManager.defaultManager fileExistsAtPath:mk]) return 0;
    NSMutableArray<NSString *> *src = [NSMutableArray array];
    for (NSString *f in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(compare:)] ?: @[])
        if ([f hasSuffix:@".metallib"] && is_source([dir stringByAppendingPathComponent:f])) [src addObject:[dir stringByAppendingPathComponent:f]];
    if (!src.count) return 0;
    UIWindow *win = nil;
    for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (!win || w.isKeyWindow) win = w;
    UIView *host = win.rootViewController.view ?: win; if (!host) { pl(@"【红】着色器准备：没有窗口可放进度页，跳过（进游戏后会边玩边编，第一次进新场景会顿）"); return 0; }
    UIView *v = [[UIView alloc] initWithFrame:host.bounds]; v.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight; v.backgroundColor = UIColor.blackColor;
    UILabel *title = [UILabel new], *sub = [UILabel new], *note = [UILabel new]; UIProgressView *bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    for (UILabel *l in @[title, sub, note]) { l.textColor = [UIColor colorWithWhite:0.85 alpha:1]; l.textAlignment = NSTextAlignmentCenter; l.numberOfLines = 0; l.translatesAutoresizingMaskIntoConstraints = NO; [v addSubview:l]; }
    title.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold]; sub.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightRegular]; note.font = [UIFont systemFontOfSize:13]; note.textColor = [UIColor colorWithWhite:0.6 alpha:1];
    bar.translatesAutoresizingMaskIntoConstraints = NO; bar.progressTintColor = [UIColor colorWithRed:0.78 green:0.36 blue:0.16 alpha:1]; [v addSubview:bar];
    title.text = L3(@"首次启动：正在准备着色器", @"First launch: preparing shaders", @"Primer inicio: preparando los shaders");
    note.text = L3(@"只需要做这一次，做完以后游戏里不会因为着色器卡顿。\n系统更新或重装之后会再做一次。可以切到后台，回来接着做。",
                   @"This is only needed once; afterwards the game will not stutter because of shaders.\nIt runs again after a system update or a reinstall. You can switch away and it continues when you come back.",
                   @"Solo hace falta una vez; después, el juego no se trabará por los shaders.\nSe repite tras una actualización del sistema o una reinstalación. Puedes salir de la app y continuará al volver.");
    [NSLayoutConstraint activateConstraints:@[
        [title.centerXAnchor constraintEqualToAnchor:v.centerXAnchor], [title.bottomAnchor constraintEqualToAnchor:bar.topAnchor constant:-22],
        [bar.centerXAnchor constraintEqualToAnchor:v.centerXAnchor], [bar.centerYAnchor constraintEqualToAnchor:v.centerYAnchor], [bar.widthAnchor constraintEqualToAnchor:v.widthAnchor multiplier:0.5],
        [sub.centerXAnchor constraintEqualToAnchor:v.centerXAnchor], [sub.topAnchor constraintEqualToAnchor:bar.bottomAnchor constant:14],
        [note.centerXAnchor constraintEqualToAnchor:v.centerXAnchor], [note.topAnchor constraintEqualToAnchor:sub.bottomAnchor constant:22], [note.widthAnchor constraintLessThanOrEqualToAnchor:v.widthAnchor multiplier:0.8]]];
    [host addSubview:v];
    UIApplication.sharedApplication.idleTimerDisabled = YES;   // 这几分钟别自动锁屏 | do not auto-lock the screen during these few minutes
    int total = (int)src.count, threads = (int)MAX(2, MIN(6, (NSInteger)NSProcessInfo.processInfo.activeProcessorCount - 2));
    const char *te = getenv("DM_SHADER_PREP_THREADS"); if (te && atoi(te) > 0) threads = atoi(te);   // 测试开关：指定并行数 | test switch: set the degree of parallelism
    pl([NSString stringWithFormat:@"着色器准备：开始，共 %d 个，%d 线程", total, threads]);
    CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent(); __block _Atomic int fin = 0, bad = 0;
    void (^tick)(void) = ^{ int n = atomic_load(&fin); double el = CFAbsoluteTimeGetCurrent() - t0, left = n > 20 ? el / n * (total - n) : -1;
        bar.progress = (float)n / total;
        NSString *eta = left < 0 ? L3(@"正在估算剩余时间", @"estimating time left", @"calculando el tiempo restante") : left >= 90 ? [NSString stringWithFormat:L3(@"预计还要 %.0f 分钟", @"about %.0f min left", @"faltan unos %.0f min"), ceil(left / 60)] : [NSString stringWithFormat:L3(@"预计还要 %.0f 秒", @"about %.0f s left", @"faltan unos %.0f s"), ceil(left / 5) * 5];
        sub.text = [NSString stringWithFormat:@"%d / %d · %@", n, total, eta]; };
    tick();
    NSTimer *tm = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) { tick(); }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); dispatch_semaphore_t sem = dispatch_semaphore_create(threads); dispatch_group_t g = dispatch_group_create();
        for (NSString *p in src) { dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
            dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
                NSString *txt = [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil]; NSError *e = nil;
                id<MTLLibrary> l = txt ? [dev newLibraryWithSource:txt options:nil error:&e] : nil;   // 与游戏里建库用的是同一个调用、同样的参数：缓存才对得上 | the same call with the same arguments as the game uses to build the library: only then does the cache match
                if (!l && atomic_fetch_add(&bad, 1) < 5) pl([NSString stringWithFormat:@"【红】着色器准备：%@ 编不过：%@", p.lastPathComponent, e.localizedDescription ?: @"读不了"]);
                int n = atomic_fetch_add(&fin, 1) + 1; if (n % 500 == 0) pl([NSString stringWithFormat:@"着色器准备：%d / %d，已用 %.0f 秒，温控档位 %ld", n, total, CFAbsoluteTimeGetCurrent() - t0, (long)NSProcessInfo.processInfo.thermalState]);
                dispatch_semaphore_signal(sem); } }); }
        dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
        double el = CFAbsoluteTimeGetCurrent() - t0; int nb = atomic_load(&bad);
        pl([NSString stringWithFormat:@"着色器准备：完成 %d 个，编不过 %d 个，用时 %.1f 秒，%d 线程，结束时温控档位 %ld", total, nb, el, threads, (long)NSProcessInfo.processInfo.thermalState]);
        if (!nb) [@"ok" writeToFile:mk atomically:YES encoding:NSUTF8StringEncoding error:nil];   // 有编不过的就不记完成：下次启动再来一遍（已编的很快） | if any failed to compile, do not record completion: run it again at the next launch (the already compiled ones are fast)
        dispatch_async(dispatch_get_main_queue(), ^{ [tm invalidate]; [v removeFromSuperview]; UIApplication.sharedApplication.idleTimerDisabled = NO; done(); });
    });
    return 1;
}
#endif
