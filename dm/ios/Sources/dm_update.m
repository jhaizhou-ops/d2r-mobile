// 构建号 + 新版本提示的 iOS 侧。
// 画面上的字和按钮是游戏自己的控件（布局 tools/touch_layout.py，写字与显隐 dm/host/dm_update.c）；这里只管三件：
//   ① 把自己的构建号（CFBundleVersion，与日志头「构建」同一个数）交给宿主，选角页左下角常显；
//   ② 每次选角页出现（启动、从局里退回来）都在后台取一次版本文件（这是我们自己加的唯一一处联网，只读一个小文件，单次 10 秒超时、失败隔几秒重试共 3 次，不挡启动与进局；同一时刻只有一个查询在途）；
//   ③ 玩家点了「更新」→ 把版本文件里的安装链接交给系统打开（系统安装，整包覆盖，存档与资源不动）。
// 版本文件地址不在仓库里：出包脚本写进 Info.plist 的 SWUpdateURL（tools/adhoc.sh 传 DM_UPDATE_URL）。没有这个键、或构建号不是大于 1 的整数
// （开发签名包是 1）→ 不联网、不提示。不比自己新：画面上什么都不出；没网 / 超时 / 地址失效 / 格式不对：选角页构建号右边出一行失败提示（dm_update_fail：没有无线数据许可 → 提示去设置里允许；HTTP 5xx → 提示服务暂不可用；其余 → 提示去浏览器打开版本文件所在的站点），这次启动取到了就撤掉。
// 版本文件 latest.json（UTF-8）四个字段：{"build": 10051432, "note": "一句话 ≤ 24 个汉字", "urgent": false, "install_url": "…"}，缺字段或类型不对 = 格式不对。
// 每次查完按新结果处理（不限一天一次，进首页就查、有新的就提示）：比自己新 → 显示 / 换成这次的；不比自己新 → 撤掉已有提示；
// 取不到 / 格式不对 → 已有的提示留着不动（一次网络抖动不该把确实存在的新版本提示抹掉；这次启动还没提示过的话照旧什么都不出）。
// 测试注入（只影响这三件的输入，不设就是产品行为）：DM_TEST_UPDATE_URL 换版本文件地址（可用 file://），DM_TEST_UPDATE_BUILD 换自己的构建号，
//   DM_TEST_UPDATE_OPEN=0 点「更新」只记日志不真打开链接；DM_TEST_UPDATE_DENY=1 把取不到当成「无线数据许可被拒」。
// iOS side of the build number + new-version notice.
// The text and button on screen are the game's own widgets (layout in tools/touch_layout.py, text writing and show/hide in dm/host/dm_update.c); only three things are handled here:
//   ① Hand our own build number (CFBundleVersion, the same number as 「构建」(Build) in the log header) to the host, always shown at the bottom-left of the character select page;
//   ② Each time the character select page appears (launch, returning from a run), fetch the version file once in the background (this is the only network access we added ourselves; it reads one small file, 10-second timeout per attempt, retries a few seconds after a failure for 3 attempts in total, does not block launch or entering a run; only one query is in flight at any moment);
//   ③ The player tapped 「更新」(Update) → hand the install link from the version file to the system to open (system install, whole-package overwrite, saves and assets untouched).
// The version file address is not in the repository: the packaging script writes it into SWUpdateURL in Info.plist (tools/adhoc.sh passes DM_UPDATE_URL). Without this key, or when the build number is not an integer greater than 1
// (dev-signed builds are 1) → no network access, no notice. Not newer than ours: nothing appears on screen; no network / timeout / dead address / bad format: a failure notice line appears to the right of the build number on the character select page (dm_update_fail: no wireless data permission → tell the player to allow it in Settings; HTTP 5xx → service-unavailable notice; otherwise → tell the player to open in a browser the site that serves the version file), removed once a fetch succeeds during this launch.
// The version file latest.json (UTF-8) has four fields: {"build": 10051432, "note": "one sentence, ≤ 24 Chinese characters", "urgent": false, "install_url": "..."}; a missing field or wrong type = bad format.
// After each query, act on the new result (not limited to once a day; query on entering the home page and show a notice if there is a new one): newer than ours → show / replace with this one; not newer than ours → remove the existing notice;
// fetch failed / bad format → leave the existing notice untouched (one network glitch should not erase a notice for a new version that really exists; if nothing has been shown yet during this launch, still show nothing as before).
// Test injection (affects only the inputs of these three things; unset = product behavior): DM_TEST_UPDATE_URL replaces the version file address (file:// is allowed), DM_TEST_UPDATE_BUILD replaces our own build number,
//   DM_TEST_UPDATE_OPEN=0 makes tapping 「更新」(Update) only log without actually opening the link; DM_TEST_UPDATE_DENY=1 treats a failed fetch as "wireless data permission denied".
#import <UIKit/UIKit.h>
#import <Network/Network.h>
#include <stdatomic.h>
void plog(const char *fmt, ...);
void dm_update_init(int own_build, void (*on_select)(void), void (*on_click)(void));   // dm/host/dm_update.c
void dm_update_offer(int new_build, const char *note, int urgent);
void dm_update_safe_left(int units);
void dm_update_fail(int kind);
void dm_update_site(const char *host);

static NSString *g_url, *g_install; static int g_own; static _Atomic int g_inflight;

// 取不到时补一行系统眼里的网络状态：分清「手机没连网」和「这个 App 没拿到无线数据许可」（两种情况请求报的都是「没有网络」）。只读，不弹框
// When the fetch fails, add one line with the network state as the system sees it: tell apart "the phone is not connected" and "this App has not been granted wireless data permission" (in both cases the request reports "no network"). Read-only, no alert
static void upd_net_why(void) {
    nw_path_monitor_t m = nw_path_monitor_create(); nw_path_monitor_set_queue(m, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    nw_path_monitor_set_update_handler(m, ^(nw_path_t p) {
        static const char *st[] = { "无效", "通", "不通", "要先建连接" }, *why[] = { "系统没给原因", "蜂窝数据许可被拒", "无线局域网数据许可被拒", "本地网络许可被拒", "要先开 VPN" };
        nw_path_status_t s = nw_path_get_status(p); nw_path_unsatisfied_reason_t r = nw_path_get_unsatisfied_reason(p);
        plog("更新：系统网络路径 %s%s%s；无线局域网 %s、蜂窝 %s", s <= 3 ? st[s] : "未知", s == nw_path_status_satisfied ? "" : "，原因：", s == nw_path_status_satisfied ? "" : r <= 4 ? why[r] : "未知",
             nw_path_uses_interface_type(p, nw_interface_type_wifi) ? "在用" : "没用上", nw_path_uses_interface_type(p, nw_interface_type_cellular) ? "在用" : "没用上");
        // 选角页失败提示：系统说「数据许可被拒」= 玩家自己能在设置里开；其余 = 让玩家去浏览器手动更新
        // Failure notice on the character select page: the system says "data permission denied" = the player can turn it on in Settings themselves; anything else = have the player update manually in a browser
        const char *td = getenv("DM_TEST_UPDATE_DENY");   // 测试：=1 当成系统说「数据许可被拒」（Mac 上造不出这种现场） | Test: =1 behaves as if the system said "data permission denied" (this situation cannot be created on Mac)
        int denied = (td && atoi(td)) || (s != nw_path_status_satisfied && (r == nw_path_unsatisfied_reason_cellular_denied || r == nw_path_unsatisfied_reason_wifi_denied));
        dm_update_fail(denied ? 1 : 2); plog("更新：选角页显示%s", denied ? "「无法联网检查更新 · 请在设置里允许无线数据」" : "「检查更新失败 · 浏览器手动更新」");
        nw_path_monitor_cancel(m);
    });
    nw_path_monitor_start(m);
}

#include "dm_update_response.h"
static void upd_fetch(int n);
static void on_select(void) {   // 宿主在游戏主线程调：选角页每出现一次调一次 | Called by the host on the game main thread: once each time the character select page appears
    dispatch_async(dispatch_get_main_queue(), ^{   // 左安全区换成布局单位（2160 高）交给宿主，算法同 front_ios.m 给佣兵头像用的那处 | Convert the left safe area into layout units (2160 high) and hand it to the host; same algorithm as the place in front_ios.m used for the mercenary portrait
        UIWindow *w = nil; for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) { w = ((UIWindowScene *)s).windows.firstObject; if (w) break; }
        const char *tl = getenv("DM_TEST_SAFE_L"); CGFloat l = tl ? atof(tl) : w.safeAreaInsets.left, hh = w.bounds.size.height; if (hh > 0) dm_update_safe_left((int)lround(l / hh * 2160));
    });
    if (!g_url) return;
    if (atomic_exchange(&g_inflight, 1)) { plog("更新：上一次查询还没回来，这次不再发"); return; }   // 同一时刻只有一个查询（含重试）在途 | Only one query (including retries) is in flight at any moment
    upd_fetch(1);
}
// 一次查询最多试 UPD_TRIES 次（首页出过一次「检查更新失败」，服务端那段时间全是 200，请求多半没出手机）。
// 读代码能列出的客户端一次性失败：① 刚启动 / 刚回前台时系统网络路径还没就绪，waitsForConnectivity = NO 时立刻报 -1009；② 冷连接 DNS + TCP + TLS
// 在蜂窝上超过原先 5 秒的总时限（-1001）；③ 每次新建会话、不复用连接，次次都是冷连接；④ DNS 首查失败（-1003）/ 连接中途丢（-1005）。
// 改：请求 10 秒、整体 15 秒；会话全程复用一个；失败隔 3 秒、6 秒再试，三次都失败才出失败提示（一次失败不出字）；每次都记第几次、错误码。
// 不开 waitsForConnectivity：数据许可被拒时它会一直等到整体时限，玩家要多等十几秒才看到「去设置里允许」。
// 测试：DM_TEST_UPDATE_FAIL_FIRST=N 前 N 次故意查一个不存在的域名（造「一次失败、再试成功」）
// One query tries at most UPD_TRIES times (the home page once showed 「检查更新失败」(Update check failed) while the server returned all 200s during that period, so the request most likely never left the phone).
// One-off client failures that can be listed by reading the code: ① right after launch / returning to the foreground the system network path is not ready yet, and with waitsForConnectivity = NO it reports -1009 immediately; ② cold-connection DNS + TCP + TLS
// on cellular exceeds the former 5-second total limit (-1001); ③ a new session was created every time with no connection reuse, so every attempt was a cold connection; ④ the first DNS lookup fails (-1003) / the connection drops midway (-1005).
// Changes: request 10 seconds, overall 15 seconds; one session reused throughout; retry 3 seconds and 6 seconds after a failure, and show the failure notice only after all three fail (a single failure shows no text); log the attempt number and error code every time.
// waitsForConnectivity is not turned on: when the data permission is denied it waits until the overall limit, and the player would wait ten-odd more seconds before seeing 「去设置里允许」(Allow it in Settings).
// Test: DM_TEST_UPDATE_FAIL_FIRST=N deliberately queries a non-existent domain for the first N attempts (to create "fail once, succeed on retry")
#define UPD_TRIES 3
static void upd_fetch(int n) {
    static NSURLSession *ses; static dispatch_once_t once;
    dispatch_once(&once, ^{ NSURLSessionConfiguration *c = NSURLSessionConfiguration.ephemeralSessionConfiguration; c.timeoutIntervalForRequest = 10; c.timeoutIntervalForResource = 15;
        c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData; c.waitsForConnectivity = NO; ses = [NSURLSession sessionWithConfiguration:c]; });
    const char *tf = getenv("DM_TEST_UPDATE_FAIL_FIRST"); NSString *us = tf && n <= atoi(tf) ? @"https://nonexistent-dm.invalid/latest.json" : g_url;
    NSURL *url = [NSURL URLWithString:us]; if (!url) { plog("更新：版本文件地址 %s 不是合法地址，不查", us.UTF8String); g_url = nil; atomic_store(&g_inflight, 0); return; }
    [[ses dataTaskWithURL:url completionHandler:^(NSData *d, NSURLResponse *r, NSError *e) {
        int build = 0, urgent = 0, safe = 0, fail_kind = 2; NSString *note = nil, *install = nil;
        NSString *why = dm_update_response(d, r, e, &build, &note, &urgent, &install, &safe, &fail_kind);
        if (safe) { extern int g_dm_rt_ab; if (g_dm_rt_ab) { g_dm_rt_ab = 0; plog("画法：版本文件 render_safe = true → 本局退回原画法"); } }
        if (why) {
            plog("更新：查 %s（第 %d / %d 次）→ %s%s", us.UTF8String, n, UPD_TRIES, why.UTF8String, n < UPD_TRIES ? "，稍后再试" : "，已有的提示不动");
            if (e) plog("更新：第 %d 次取不到的错误码 %s %ld", n, e.domain.UTF8String, (long)e.code);
            if (n < UPD_TRIES) { dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * n) * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ upd_fetch(n + 1); }); return; }
            atomic_store(&g_inflight, 0);
            if (e) upd_net_why(); else dm_update_fail(fail_kind);   // 三次都失败才出字：网络错误由系统网络路径定原因（upd_net_why 里设），HTTP 5xx = 服务故障，其余非 200 / 格式不对 = 其他失败 | Text appears only after all three attempts fail: for a network error the reason is determined by the system network path (set in upd_net_why); HTTP 5xx = service failure, other non-200 / bad format = other failures
            return; }
        atomic_store(&g_inflight, 0);
        if (n > 1) plog("更新：第 %d 次取到了（前面 %d 次失败）", n, n - 1);
        dm_update_fail(0);   // 这次取到了：撤掉失败提示 | Fetched successfully this time: remove the failure notice
        if (build <= g_own) { plog("更新：查 %s → 最新构建 %d，不比自己（%d）新，不提示（已有的提示撤掉）", us.UTF8String, build, g_own); dm_update_offer(0, "", 0); return; }
        plog("更新：查 %s → 有新构建 %d（%s，自己 %d）：%s", us.UTF8String, build, urgent ? "建议立即更新" : "普通", g_own, note.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{ g_install = install; });
        dm_update_offer(build, note.UTF8String, urgent);
    }] resume];
}
static void on_click(void) {   // 宿主在游戏主线程调：玩家点了「更新」 | Called by the host on the game main thread: the player tapped 「更新」(Update)
    dispatch_async(dispatch_get_main_queue(), ^{
        NSURL *u = g_install ? [NSURL URLWithString:g_install] : nil; if (!u) { plog("更新：点了「更新」但没有安装链接"); return; }
        const char *to = getenv("DM_TEST_UPDATE_OPEN"); if (to && !atoi(to)) { plog("更新：点了「更新」→ 安装链接 %s（DM_TEST_UPDATE_OPEN=0，不真打开）", g_install.UTF8String); return; }
        plog("更新：点了「更新」→ 交系统打开安装链接");
        [UIApplication.sharedApplication openURL:u options:@{} completionHandler:^(BOOL ok) { plog("更新：系统%s安装链接", ok ? "已接手" : "没能打开"); }];
    });
}
void dm_update_boot(void) {   // main.m 起宿主前调一次 | main.m calls this once before starting the host
    const char *tb = getenv("DM_TEST_UPDATE_BUILD"), *tu = getenv("DM_TEST_UPDATE_URL");
    NSString *v = tb ? @(tb) : [NSBundle.mainBundle.infoDictionary[@"CFBundleVersion"] description]; g_own = v.intValue;
    if (![v isEqualToString:[NSString stringWithFormat:@"%d", g_own]] || g_own < 0) g_own = 0;
    NSString *url = tu ? @(tu) : NSBundle.mainBundle.infoDictionary[@"SWUpdateURL"];
    if ([url isKindOfClass:NSString.class] && url.length && g_own > 1) g_url = url;
    if (g_url) dm_update_site([NSURL URLWithString:g_url].host.UTF8String);   // 检查失败时提示玩家去浏览器打开的站点 = 版本文件所在的主机 | The site the player is told to open in a browser when the check fails = the host that serves the version file
    plog("更新：自己的构建 %d，%s", g_own, g_url ? "到选角页后查一次新版本" : g_own > 1 ? "包里没有版本文件地址，不查新版本" : "开发包，不查新版本");
    dm_update_init(g_own, on_select, on_click);
}
