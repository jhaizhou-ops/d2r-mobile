// iCloud 存档同步的 iOS 一侧：拿 iCloud 容器目录、让云端的包先下载好，再调共用核心 dm_cloud_sync（dm/host/dm_cloudsync.c）。
// 启动：dm_cloud_boot 在 boot_thread 里、dm_main 挂存档之前调，最多等 5 秒，拉云端各包、按「后写覆盖先写」换进本地存档，之后游戏照常读存档列表。
// 运行中：存档每提交成功一次（hle_fs.c 的 Commit），提交钩子先把已提交目录 clonefile 成快照（写时复制，近乎零成本）；局内只记成待传，出局 / 切后台时在串行队列里把快照与云端对比、只上传（allow_down = 0）；不在局内的提交当场传。
// 只同步玩家真实存档 Documents/save；测试存档（save.test 等）缺省不同步。测试开关 DM_TEST_ICLOUD_SUB=<子目录名>：测试存档也同步，云端用容器里的这个子目录（与真实存档的 saves 隔开，不会把测试角色传到玩家设备上）。
// 测试开关 DM_TEST_ICLOUD_DIR=<本地目录>：用本地目录当云端（不碰 iCloud），测试存档也同步。不以 / 开头的路径拼到 App 的 Documents 下（下同）。
// 测试开关 DM_TEST_ICLOUD_ROOT=<本地目录>：用本地目录当 iCloud 容器的 Documents（下有 saves / saves_v2），验目录换代与一次性迁移；测试存档也同步。
// Mac 原生 App（宿主 app_mac.m 包成的那个）也编这个文件，同一个容器、同一套规则：真实存档 = 应用支持目录下的 save（启动代码设 DM_CLOUD_REAL=1；
// 带 DM_APP_SUPPORT 起的测试局不设，一律当测试存档、不同步）；没有「切后台」，改成退出 App 前传一次并等它传完（dm_cloud_flush_wait）。
// 开关：「自定义」页 icloud（缺省开）。没登录 iCloud / 本 App 的 iCloud 被关：取不到容器，整套跳过，日志记一行。
// The iOS side of iCloud save sync: obtain the iCloud container directory, let the cloud packages download first, then call the shared core dm_cloud_sync (dm/host/dm_cloudsync.c).
// Startup: dm_cloud_boot is called in boot_thread, before dm_main mounts the save; it waits at most 5 seconds, pulls each cloud package and swaps it into the local save by "last write wins", after which the game reads the save list as usual.
// While running: every time a save commit succeeds (Commit in hle_fs.c), the commit hook first clonefiles the committed directory into a snapshot (copy-on-write, nearly free); in game it is only marked as pending, and on leaving the game / going to the background the snapshot is compared with the cloud on a serial queue and only uploaded (allow_down = 0); commits outside a game are uploaded on the spot.
// Only the player's real save Documents/save is synced; test saves (save.test and so on) are not synced by default. Test switch DM_TEST_ICLOUD_SUB=<subdirectory name>: test saves are synced as well, using this subdirectory of the container in the cloud (separate from the saves of the real save, so test characters are not sent to the player's devices).
// Test switch DM_TEST_ICLOUD_DIR=<local directory>: use a local directory as the cloud (iCloud is not touched); test saves are synced as well. A path not starting with / is joined under the App's Documents (same below).
// Test switch DM_TEST_ICLOUD_ROOT=<local directory>: use a local directory as the Documents of the iCloud container (with saves / saves_v2 under it), to verify the directory generation change and the one-time migration; test saves are synced as well.
// The native Mac App (the one the host's app_mac.m is wrapped into) compiles this file too, with the same container and the same rules: the real save = save under the application support directory (the startup code sets DM_CLOUD_REAL=1;
// test runs launched with DM_APP_SUPPORT do not set it and are always treated as test saves, not synced); there is no "going to the background", so it uploads once before quitting the App and waits for it to finish (dm_cloud_flush_wait).
// Switch: icloud on the Custom page (on by default). Not signed in to iCloud / iCloud turned off for this App: the container cannot be obtained, the whole thing is skipped, and one line is logged.
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif
#import <Network/Network.h>
#include <signal.h>
#include <sys/clonefile.h>
#include <stdatomic.h>
#include "front.h"
#include "hle.h"

int dm_cloud_sync(const char *save, const char *cloud, const char *bak, const char *statef, int allow_down, uint64_t now_ms, int (*ready)(const char *unit));
int dm_cloud_inbox_add(const char *save, const char *statef, const char *pack); uint64_t dm_cloud_pack_stamp(const char *pack);
int dm_cloud_bak_pack(const char *bak, const char *unit, const char *pack, const char *why); void dm_cloud_char_conflict_note(const char *unit);
int dm_custom_get(const char *key);
extern void (*g_dm_save_commit_hook)(const char *commit_dir);

static NSString *g_cloud, *g_save, *g_bak, *g_state;   // 云端目录 / 本地存档 / 备份 / 状态文件 | Cloud directory / local save / backup / state file
static dispatch_queue_t g_q;
static uint64_t now_ms(void) { return (uint64_t)([NSDate date].timeIntervalSince1970 * 1000); }

// 云端某个单位的包是否已在本机（iCloud 里没下载的文件是占位：旧系统是「.<名>.icloud」，新系统是同名但下载状态不是「最新」）
// Whether a given unit's package in the cloud is already on this device (undownloaded files in iCloud are placeholders: on older systems ".<name>.icloud", on newer systems the same name with a download status other than "current")
static int pack_ready(const char *unit) {
    NSString *nm = [NSString stringWithFormat:@"%s.pack", unit];
    if ([NSFileManager.defaultManager fileExistsAtPath:[g_cloud stringByAppendingPathComponent:[NSString stringWithFormat:@".%@.icloud", nm]]]) return 0;
    NSURL *u = [NSURL fileURLWithPath:[g_cloud stringByAppendingPathComponent:nm]]; NSString *st = nil;
    if (![u getResourceValue:&st forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil] || !st) return 1;   // 不是 iCloud 管的文件（或没有这个包）：按已在本机 | Not an iCloud-managed file (or no such package): treated as already on this device
    return [st isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent];
}
static int pending_downloads(int start) {   // 还没下载好的包数；start = 1 时顺手请系统开始下载 | Number of packages not yet downloaded; with start = 1 also ask the system to start downloading
    int n = 0; NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *f in [fm contentsOfDirectoryAtPath:g_cloud error:nil] ?: @[]) {
        NSString *nm = f; if ([f hasPrefix:@"."] && [f hasSuffix:@".icloud"]) nm = [f substringWithRange:NSMakeRange(1, f.length - 8)];
        if (![nm hasSuffix:@".pack"]) continue;
        if (pack_ready([nm substringToIndex:nm.length - 5].UTF8String)) continue;
        n++; if (start) [fm startDownloadingUbiquitousItemAtURL:[NSURL fileURLWithPath:[g_cloud stringByAppendingPathComponent:nm]] error:nil];
    }
    return n;
}
// iCloud 自己的同名文件冲突（两台各写了一份同名包、系统挑了一份当现行版，其余成「未解决的冲突版本」；iCloud 离线冲突第 1 条）。启动同步前在文件协调里做：
//   共享仓库：每个冲突版本收进待合并收件箱（dm_cloud_inbox_add，现行版照常走同步）；
//   角色：包头时刻（游戏最后写它的时刻）最新的那份当现行版（不是系统挑的就换上去），其余整份进备份，记名进局提示。
//   全部收好才把冲突标成已解决并删掉其余版本；有一份收不进就都留着，下次启动再来（宁可多留不丢）。
// iCloud's own same-name file conflict (two devices each wrote a same-name pack, the system picked one as the current version and the rest became "unresolved conflict versions"; iCloud offline conflict item 1). Done inside file coordination before the launch sync:
//   shared stash: each conflict version is stored in the to-merge inbox (dm_cloud_inbox_add; the current version goes through sync as usual);
//   character: the copy with the newest pack header timestamp (the moment the game last wrote it) becomes the current version (swapped in if it is not the one the system picked), the rest go into the backup whole, and the name is recorded for a notice after entering a run.
//   Only once everything is stored are the conflicts marked resolved and the other versions deleted; if any one cannot be stored, all are kept and tried again at the next launch (better keep extra than lose any).
static void take_conflicts(NSString *save) {
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *f in [fm contentsOfDirectoryAtPath:g_cloud error:nil] ?: @[]) {
        if ([f hasPrefix:@"."] || ![f hasSuffix:@".pack"]) continue;
        NSURL *u = [NSURL fileURLWithPath:[g_cloud stringByAppendingPathComponent:f]]; NSArray<NSFileVersion *> *vs = [NSFileVersion unresolvedConflictVersionsOfItemAtURL:u];
        if (!vs.count) continue;
        NSString *unit = f.stringByDeletingPathExtension; int bad = 0;
        if ([unit isEqualToString:@"_stash"]) { for (NSFileVersion *v in vs) bad |= dm_cloud_inbox_add(save.fileSystemRepresentation, g_state.fileSystemRepresentation, v.URL.fileSystemRepresentation) != 0; }
        else { uint64_t best = dm_cloud_pack_stamp(u.fileSystemRepresentation); NSFileVersion *win = nil;
            for (NSFileVersion *v in vs) { uint64_t t = dm_cloud_pack_stamp(v.URL.fileSystemRepresentation); if (t > best) { best = t; win = v; } }
            if (win) { NSError *e = nil; bad |= dm_cloud_bak_pack(g_bak.fileSystemRepresentation, unit.UTF8String, u.fileSystemRepresentation, "iCloud 冲突里较旧的现行版") != 0;
                if (!bad && ![win replaceItemAtURL:u options:0 error:&e]) { bad = 1; LOG("【红】iCloud 同步：%s 换上较新的冲突版本失败 %s", unit.UTF8String, e.localizedDescription.UTF8String ?: "?"); } }
            for (NSFileVersion *v in vs) if (v != win) bad |= dm_cloud_bak_pack(g_bak.fileSystemRepresentation, unit.UTF8String, v.URL.fileSystemRepresentation, "iCloud 冲突版本") != 0;
            if (!bad) dm_cloud_char_conflict_note(unit.UTF8String); }
        if (bad) { LOG("【红】iCloud 同步：%s 有 %lu 个冲突版本没收好，留着下次启动再来", unit.UTF8String, (unsigned long)vs.count); continue; }
        for (NSFileVersion *v in vs) v.resolved = YES;
        NSError *e = nil; if (![NSFileVersion removeOtherVersionsOfItemAtURL:u error:&e]) LOG("iCloud 同步：%s 冲突已标解决，删其余版本没成（%s），不影响", unit.UTF8String, e.localizedDescription.UTF8String ?: "?");
        LOG("iCloud 同步：%s 的 %lu 个 iCloud 冲突版本已收好（%s）", unit.UTF8String, (unsigned long)vs.count, [unit isEqualToString:@"_stash"] ? "进待合并收件箱" : "最新的当现行版、其余进备份");
    }
}
static int run_sync(NSString *save, int down) {   // 在文件协调里做：iCloud 守护进程同时在写这个目录时不会读到半个文件 | Done inside file coordination: half-written files are not read while the iCloud daemon is writing this directory at the same time
    __block int r = -1; NSError *err = nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:[NSURL fileURLWithPath:g_cloud] options:NSFileCoordinatorWritingForMerging error:&err byAccessor:^(NSURL *u) {
        (void)u; if (down) take_conflicts(save);   // 只在启动那次（运行中 save 是提交快照目录，不放收件箱） | only at launch (while running, save is the commit snapshot directory; nothing goes into the inbox)
        r = dm_cloud_sync(save.fileSystemRepresentation, g_cloud.fileSystemRepresentation, g_bak.fileSystemRepresentation, g_state.fileSystemRepresentation, down, now_ms(), pack_ready); }];
    if (err) LOG("【红】iCloud 同步：文件协调失败 %s", err.localizedDescription.UTF8String);
    return r;
}
// 存档提交钩子（hle_fs.c，持着存档锁调）：只做快照，其余放队列。
// 局内（58 分钟提交 127 次、上传 97 次，平均 35 秒碰一次网）：只留最新那份快照记成「待传」，不上传；
// 退出这一局回到前端（主菜单 / 选角页）、切到后台时各传一次（dm_cloud_flush）。不在局内（选角 / 建角 / 删角色）的提交照旧当场传——
// 删角色只在选角页发生，拖到以后传的话被强杀后再启动会把删掉的角色从云端拉回来（启动同步把「本地没了」当丢失、从云端拿回）。
// 被强杀 / 崩溃没来得及传：待传快照随之作废（启动时清掉），启动那次同步看到本地自上次同步后改过、云端没变，照常补传（dm_cloudsync.c lchg && !cchg → UP）
// Save commit hook (hle_fs.c, called while holding the save lock): only takes a snapshot, the rest goes to the queue.
// In a run (127 commits and 97 uploads in 58 minutes, touching the network every 35 seconds on average): keep only the newest snapshot marked as "pending upload", do not upload;
// upload once each when leaving the run back to the front end (main menu / character select page) and when going to background (dm_cloud_flush). Commits outside a run (character select / create / delete character) are still uploaded on the spot --
// character deletion only happens on the character select page; if deferred, a force-kill followed by a relaunch would pull the deleted character back from the cloud (startup sync treats "missing locally" as lost and restores it from the cloud).
// Force-killed / crashed before uploading: the pending snapshot is voided with it (cleared at startup), and the startup sync sees local changed since the last sync and cloud unchanged, so it uploads as usual (dm_cloudsync.c lchg && !cchg → UP)
static NSString *g_pend;          // 局内最新一次提交的快照（只在 g_q 上读写） | Snapshot of the newest commit in the run (read and written only on g_q)
static _Atomic int g_bg;          // App 在后台：此时的提交（失活时请求的那次存盘）当场传 | App in background: a commit at this time (the save requested on resign active) is uploaded on the spot
static _Atomic int g_session;     // 在一局里：进局（第一次出现局内画面）起，到游戏发「回到前端」（FrontEndNavigation:ToFrontEnd）为止——中间的读取画面（切幕 / 传送点）不算出局 | In a run: from entering the run (first appearance of the in-run picture) until the game sends "back to front end" (FrontEndNavigation:ToFrontEnd) -- loading screens in between (act change / waypoint) do not count as leaving the run
static void upload_snap(NSString *snap, const char *why) {   // 在 g_q 上调 | Called on g_q
    if (dm_custom_get("icloud")) { int n = run_sync(snap, 0); LOG("iCloud 同步：%s上传 %d 个", why, n); }
    [NSFileManager.defaultManager removeItemAtPath:snap error:nil];
}
static void on_commit(const char *commit_dir) {
    static unsigned seq; NSString *snap = [NSString stringWithFormat:@"%@.cloudsnap%u", g_save, ++seq];
    [NSFileManager.defaultManager removeItemAtPath:snap error:nil];
    if (clonefile(commit_dir, snap.fileSystemRepresentation, 0)) { LOG("【红】iCloud 同步：存档快照失败 errno=%d，这次提交不上传", errno); return; }
    int defer = atomic_load(&g_session) && !atomic_load(&g_bg);
    dispatch_async(g_q, ^{
        if (defer) { if (g_pend) [NSFileManager.defaultManager removeItemAtPath:g_pend error:nil]; g_pend = snap; return; }   // 局内：换成这一份，等出局 / 切后台 | In a run: replace with this copy and wait for leaving the run / going to background
        if (g_pend) { [NSFileManager.defaultManager removeItemAtPath:g_pend error:nil]; g_pend = nil; }   // 这一份更新，旧的待传作废 | This copy is newer; the old pending upload is voided
        upload_snap(snap, atomic_load(&g_bg) ? "后台存档提交后" : "存档提交后（不在局内）");
    });
}
// 出局 / 切后台：把局内攒下的最新一份传掉。切后台用系统给的后台收尾时间，传完才结束（最多约 30 秒，系统到点前会叫停）
// Leaving a run / going to background: upload the newest copy accumulated during the run. Going to background uses the background completion time granted by the system and ends only after the upload finishes (about 30 seconds at most; the system calls a stop before time is up)
void dm_cloud_flush(int background) {
    if (!g_q) return;   // 没开同步（测试存档 / 没登录 iCloud / 自定义页关了） | Sync not enabled (test save / not signed in to iCloud / turned off on the Custom page)
    if (background) atomic_store(&g_bg, 1);
#if TARGET_OS_IPHONE
    __block UIBackgroundTaskIdentifier tk = UIBackgroundTaskInvalid;
    if (background) tk = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"dm.cloud" expirationHandler:^{ LOG("【红】iCloud 同步：后台收尾时间到，系统叫停（没传完的下次启动补传）"); [UIApplication.sharedApplication endBackgroundTask:tk]; tk = UIBackgroundTaskInvalid; }];
#endif
    dispatch_async(g_q, ^{
        NSString *p = g_pend; g_pend = nil;
        if (p) upload_snap(p, background ? "切到后台，" : "退出这一局回到前端，");
        else LOG("iCloud 同步：%s，局内没有待传的存档", background ? "切到后台" : "退出这一局回到前端");
#if TARGET_OS_IPHONE
        if (tk != UIBackgroundTaskInvalid) dispatch_async(dispatch_get_main_queue(), ^{ if (tk != UIBackgroundTaskInvalid) { [UIApplication.sharedApplication endBackgroundTask:tk]; tk = UIBackgroundTaskInvalid; } });
#endif
    });
}
// 退出 App 前（Mac）：把局内攒下的最新一份传掉，并等队列里的活做完，最多 sec 秒。返回 1 = 做完了（或本来就没开同步），0 = 到点还没完
// Before quitting the App (Mac): upload the latest copy accumulated during the game and wait for the queued work to finish, at most sec seconds. Returns 1 = done (or sync was not on in the first place), 0 = not finished in time
int dm_cloud_flush_wait(double sec) {
    if (!g_q) return 1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(g_q, ^{ NSString *p = g_pend; g_pend = nil; if (p) upload_snap(p, "退出 App 前，"); else LOG("iCloud 同步：退出 App 前，没有待传的存档"); dispatch_semaphore_signal(done); });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(sec * NSEC_PER_SEC)))) { LOG("【红】iCloud 同步：退出 App 前 %.0f 秒没传完（没传完的下次启动补传）", sec); return 0; }
    return 1;
}
void dm_cloud_foreground(void) { atomic_store(&g_bg, 0); }
static void cloud_ingame(int on) { if (on) atomic_store(&g_session, 1); }
static void cloud_charsel(void) { if (atomic_exchange(&g_session, 0)) dm_cloud_flush(0); }   // 退出这一局回到前端（主菜单 / 选角页）：这一局结束 | Left this run back to the front end (main menu / character select page): this run has ended

static NSURL *container(double limit) {   // 取 iCloud 容器：要在主线程以外调，第一次可能要等系统建目录；取不到返回 nil 并记原因 | Get the iCloud container: must be called off the main thread; the first time may have to wait for the system to create the directory; returns nil and logs the reason if it cannot be obtained
    __block NSURL *ct = nil; dispatch_semaphore_t got = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ ct = [NSFileManager.defaultManager URLForUbiquityContainerIdentifier:nil]; dispatch_semaphore_signal(got); });
    if (dispatch_semaphore_wait(got, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(limit * NSEC_PER_SEC)))) { LOG("iCloud 不可用：取容器超过 %.0f 秒，本次只用本地存档", limit); return nil; }
    if (!ct) LOG("iCloud 不可用：没登录 iCloud 或本 App 的 iCloud 被关，只用本地存档");
    return ct;
}

// 云端目录换代（编年史仓库第 5 步；旧版本是覆盖更新的）：本版起存档格式变了（储藏箱 14 列），旧包读新档会静默删掉新列里的物品。
// 云端改用 Documents/saves_v2，旧包仍用 Documents/saves，两边互不覆盖。新包第一次启动（saves_v2 还没有）把 saves 复制过去一次——只读旧目录，旧目录一个字节都不改、不删：
//   先等旧目录里的包全部下载到本机（占位文件复制过去是空的）；逐个文件复制到 saves_v2.tmp，最后写完成清单 sw_migrated.txt（迁移时的全部包名），
//   再把 saves_v2.tmp 整个改名成 saves_v2——改名是最后一步，有 saves_v2 就一定是写完清单的整套。中途被杀：下次启动先删掉残留的 .tmp 从头再来。
//   没等到下载完 / 复制失败 = 本次不迁移也不同步，下次启动再试（先同步会建出空的 saves_v2，之后再也不迁移，别的设备在旧目录里的新档就漏了）。
// 用之前查齐（所有设备，含迁移的那台）：saves_v2 里有清单、清单上的包都到了本机目录（已下载或是 iCloud 占位）才同步。另一台设备上 iCloud 是一个文件一个文件
// 下来的：没查齐就同步，本机较旧的那份会按「云端没有包 → 上传」传上去，后写覆盖掉迁移过来的新进度。查齐过一次就在本地记一笔（<存档>.cloudv2ok），以后不再查
// （之后角色删了包会少，不能一直按清单卡）。返回 0 = 本次跳过同步
// Cloud directory generation change (chronicle vault step 5; older versions were updated by overwrite): from this version the save format changed (stash has 14 columns); an old build reading a new save silently deletes the items in the new columns.
// The cloud now uses Documents/saves_v2 while old builds keep using Documents/saves, so neither overwrites the other. On its first launch (saves_v2 does not exist yet) the new build copies saves over once -- the old directory is only read, not one byte of it is changed or deleted:
//   first wait until all packages in the old directory are downloaded to this device (a placeholder file would copy over as empty); copy file by file into saves_v2.tmp, and finally write the completion manifest sw_migrated.txt (all package names at migration time),
//   then rename saves_v2.tmp as a whole to saves_v2 -- the rename is the last step, so if saves_v2 exists it is always a complete set with the manifest written. Killed midway: on the next launch delete the leftover .tmp first and start over.
//   Download not finished in time / copy failed = no migration and no sync this time, retry on the next launch (syncing first would create an empty saves_v2, after which migration never happens again and new saves from other devices in the old directory would be missed).
// Check completeness before use (all devices, including the one that migrated): sync only when saves_v2 contains the manifest and every package on the manifest has reached this device's directory (downloaded, or present as an iCloud placeholder). On another device iCloud brings files down one
// at a time: syncing before the check passes, this device's older copy would be uploaded under "no package in the cloud → upload", and the later write would overwrite the newer migrated progress. Once the check has passed, record it locally (<save>.cloudv2ok) and never check again
// (later, deleting a character removes packages, so we cannot keep blocking on the manifest forever). Returns 0 = skip sync this time
static NSString *test_path(const char *e) {   // 测试开关里的路径：不以 / 开头的拼到 App 的 Documents 下（真机容器路径每次装包都变） | Paths in test switches: those not starting with / are joined under the App's Documents (the real-device container path changes on every install)
    if (!e) return nil; if (e[0] == '/') return @(e);
#if TARGET_OS_OSX
    return [@(getenv("DM_SAVE") ?: ".").stringByDeletingLastPathComponent stringByAppendingPathComponent:@(e)];   // Mac：拼到存档目录的上一级（应用支持目录） | Mac: joined to the parent of the save directory (the application support directory)
#else
    return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@(e)];
#endif
}
static int v2_ready(NSString *nw, NSString *okf) {   // 迁移过来的整套是否都到了本机 | Whether the whole migrated set has arrived on this device
    NSFileManager *fm = NSFileManager.defaultManager; if ([fm fileExistsAtPath:okf]) return 1;
    NSString *ls = [NSString stringWithContentsOfFile:[nw stringByAppendingPathComponent:@"sw_migrated.txt"] encoding:NSUTF8StringEncoding error:nil];
    if (!ls) { LOG("iCloud 同步：saves_v2 里还没有完成清单（别的设备的迁移还没传完？），本次不同步"); return 0; }
    int miss = 0, n = 0; for (NSString *f in [ls componentsSeparatedByString:@"\n"]) { if (!f.length) continue; n++;
        if (![fm fileExistsAtPath:[nw stringByAppendingPathComponent:f]] && ![fm fileExistsAtPath:[nw stringByAppendingPathComponent:[NSString stringWithFormat:@".%@.icloud", f]]]) miss++; }
    if (miss) { LOG("iCloud 同步：saves_v2 清单 %d 项还缺 %d 项没到本机，本次不同步", n, miss); return 0; }
    [@"" writeToFile:okf atomically:YES encoding:NSUTF8StringEncoding error:nil]; LOG("iCloud 同步：saves_v2 清单 %d 项都已到本机", n); return 1;
}
static int migrate_v2(NSString *docs, double deadline) {
    NSFileManager *fm = NSFileManager.defaultManager; NSString *nw = [docs stringByAppendingPathComponent:@"saves_v2"], *old = [docs stringByAppendingPathComponent:@"saves"], *tmp = [nw stringByAppendingString:@".tmp"];
    if ([fm fileExistsAtPath:nw]) return 1;
    if (![fm fileExistsAtPath:old]) { LOG("iCloud 同步：云端没有旧目录 saves，直接用 saves_v2"); [fm createDirectoryAtPath:nw withIntermediateDirectories:YES attributes:nil error:nil];
        [@"" writeToFile:[nw stringByAppendingPathComponent:@"sw_migrated.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil]; return 1; }   // 空清单：没有要等的包 | Empty manifest: no packages to wait for
    g_cloud = old; int pend = pending_downloads(1);
    while (pend && CFAbsoluteTimeGetCurrent() < deadline) { usleep(100000); pend = pending_downloads(0); }
    g_cloud = nil;
    if (pend) { LOG("iCloud 同步：旧目录 saves 还有 %d 个包没下载完，本次不迁移、不同步，下次启动再试", pend); return 0; }
    const char *kill_at = getenv("DM_TEST_ICLOUD_MIGRATE_KILL");   // 测试：复制完第 N 个文件后自杀（N = marker：写完清单、改名之前），验中途被杀 | Test: kill self after copying the Nth file (N = marker: after writing the manifest, before the rename), to verify being killed midway
    __block BOOL ok = YES; __block NSError *e2 = nil; __block NSMutableString *list = [NSMutableString string]; __block int copied = 0; NSError *err = nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:[NSURL fileURLWithPath:old] options:0 error:&err byAccessor:^(NSURL *u) {
        if ([fm fileExistsAtPath:tmp]) { LOG("iCloud 同步：上次迁移没做完（有 saves_v2.tmp），删掉重来"); [fm removeItemAtPath:tmp error:nil]; }
        ok = [fm createDirectoryAtPath:tmp withIntermediateDirectories:NO attributes:nil error:&e2];
        for (NSString *f in [fm contentsOfDirectoryAtPath:u.path error:nil] ?: @[]) { if (!ok) break; if ([f hasPrefix:@"."]) continue;   // 隐藏文件（系统的）不带 | Hidden files (the system's) are not carried over
            ok = [fm copyItemAtPath:[u.path stringByAppendingPathComponent:f] toPath:[tmp stringByAppendingPathComponent:f] error:&e2]; if (!ok) break;
            if ([f hasSuffix:@".pack"]) [list appendFormat:@"%@\n", f]; copied++;
            if (kill_at && atoi(kill_at) == copied) { LOG("DM_TEST_ICLOUD_MIGRATE_KILL：复制完第 %d 个文件，自杀", copied); kill(getpid(), SIGKILL); } }
        if (ok) ok = [list writeToFile:[tmp stringByAppendingPathComponent:@"sw_migrated.txt"] atomically:YES encoding:NSUTF8StringEncoding error:&e2];
        if (ok && kill_at && !strcmp(kill_at, "marker")) { LOG("DM_TEST_ICLOUD_MIGRATE_KILL：清单已写、改名之前自杀"); kill(getpid(), SIGKILL); }
        if (ok) ok = [fm moveItemAtPath:tmp toPath:nw error:&e2]; }];
    if (!ok || err) { [fm removeItemAtPath:tmp error:nil];
        if ([fm fileExistsAtPath:nw]) { LOG("iCloud 同步：saves_v2 已由别的设备建好，用它"); return 1; }   // 两台同时迁：后改名的那台失败，用先到的那套 | Two devices migrating at once: the one that renames later fails and uses the set that arrived first
        LOG("【红】iCloud 同步：旧目录 saves 复制到 saves_v2 失败（%s），本次不同步", (err ?: e2).localizedDescription.UTF8String ?: "?"); return 0; }
    LOG("iCloud 同步：旧目录 saves 已复制到 saves_v2（%d 个文件，清单 %lu 个包），旧目录留给旧包用", copied, (unsigned long)[list componentsSeparatedByString:@"\n"].count - 1); return 1;
}
// 从没同步成功过的设备先看清云端再同步（iCloud 离线冲突第 2 条）：新设备断网首启时，系统还没把云端条目列到本机，云端目录看着是空的——
// 照常同步会把本机全部当「云端没有」上传（离线时新建的穷共享仓库联网后与云端同名包冲突、盖掉云端），迁移那步还会新建一个空的 saves_v2。
// 所以状态文件里一条记录都没有（从没同步成功过）时，先问系统的云端条目列表（NSMetadataQuery，收集完成才算数）：
//   列出了包 → 这些包都已出现在本机目录（下好的或 iCloud 占位）才同步；列表是空的 → 网络通才同步（世界上第一台设备就是这种）。都不满足 = 本次整套跳过，下次启动再看。
// 列表收集完成在断网时怎么表现、联网时空列表会不会滞后，要真机断网跑才知道（排给池）；滞后漏过去的那种由冲突版本合并兜住（第 1 条）。
// 测试开关 DM_TEST_ICLOUD_FIRST=<收集完成>,<列出几个>,<到本机几个>,<网络通>：不问系统，按给的值判（Mac 宿主 / 本地测试云端验分支用）
// A device that has never synced successfully first gets a clear view of the cloud before syncing (iCloud offline conflict item 2): on a new device's first launch without network, the system has not yet listed the cloud entries locally, so the cloud directory looks empty -
// a normal sync would upload everything local as "not in the cloud" (a poor shared stash newly created offline conflicts with the cloud's same-name pack once online and overwrites the cloud), and the migration step would also create an empty saves_v2.
// So when the state file has no record at all (never synced successfully), first ask the system for its list of cloud entries (NSMetadataQuery, counted only once gathering has finished):
//   packs are listed -> sync only once all of them have appeared in the local directory (downloaded or as iCloud placeholders); the list is empty -> sync only if the network is reachable (the very first device in the world is like this). Neither = skip the whole thing this time and look again at the next launch.
// How gathering completes with no network, and whether an empty list can lag behind when online, can only be known by running on a device with the network cut (queued for the pool); the lagging case that slips through is covered by merging conflict versions (item 1).
// Test switch DM_TEST_ICLOUD_FIRST=<gathering done>,<how many listed>,<how many local>,<network reachable>: does not ask the system and decides by the given values (for verifying the branches on the Mac host / a local test cloud)
static int never_synced(NSString *statef) {
    NSString *t = [NSString stringWithContentsOfFile:statef encoding:NSUTF8StringEncoding error:nil];
    return !t || [t rangeOfString:@"\t"].location == NSNotFound;
}
static int first_listing_ok(double deadline) {
    int got = 0, listed = 0, present = 0, net = 0; const char *e = getenv("DM_TEST_ICLOUD_FIRST");
    if (e) { sscanf(e, "%d,%d,%d,%d", &got, &listed, &present, &net); }
    else {
        __block int g = 0, n = 0, pr = 0; dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block NSMetadataQuery *q = nil; __block id obs = nil;
        // 元数据查询要挂在有运行循环的线程上。iOS 外壳在 boot 子线程上调 dm_cloud_boot，派发到主线程没问题；
        // 但 Mac 宿主在 main() 主线程上直接调（main.c），此时主线程还没进 run loop——派发到主线程自己再等它就是死锁（永远「收集未完成」）。
        // 所以：已经在主线程时就地起查询，用局部 run loop 驱动到收集完成或超时，不再派发。
        // The metadata query must live on a thread with a run loop. The iOS shell calls dm_cloud_boot on a boot thread, so dispatching to the main thread is fine;
        // but the Mac host calls it directly on the main thread in main() (main.c), where the main thread has not entered its run loop yet - dispatching to the main thread and then waiting on it deadlocks (always "gathering unfinished").
        // So: when already on the main thread, start the query in place and drive a local run loop until gathering finishes or the deadline passes.
        BOOL on_main = NSThread.isMainThread;
        void (^startq)(void) = ^{
            q = [NSMetadataQuery new]; q.searchScopes = @[NSMetadataQueryUbiquitousDocumentsScope];
            q.predicate = [NSPredicate predicateWithFormat:@"%K LIKE '*.pack'", NSMetadataItemFSNameKey];
            obs = [NSNotificationCenter.defaultCenter addObserverForName:NSMetadataQueryDidFinishGatheringNotification object:q queue:nil usingBlock:^(NSNotification *note) { (void)note;
                [q disableUpdates]; n = (int)q.resultCount; NSFileManager *fm = NSFileManager.defaultManager;
                for (NSUInteger i = 0; i < q.resultCount; i++) { NSURL *u = [[q resultAtIndex:i] valueForAttribute:NSMetadataItemURLKey]; if (!u) continue;
                    NSString *ph = [u.path.stringByDeletingLastPathComponent stringByAppendingPathComponent:[NSString stringWithFormat:@".%@.icloud", u.lastPathComponent]];
                    if ([fm fileExistsAtPath:u.path] || [fm fileExistsAtPath:ph]) pr++; }
                g = 1; [q stopQuery]; [NSNotificationCenter.defaultCenter removeObserver:obs]; dispatch_semaphore_signal(sem); }];
            if (![q startQuery]) { [NSNotificationCenter.defaultCenter removeObserver:obs]; dispatch_semaphore_signal(sem); } };
        double left = deadline - CFAbsoluteTimeGetCurrent(); if (left < 0.5) left = 0.5;
        if (on_main) {
            startq();
            NSDate *until = [NSDate dateWithTimeIntervalSinceNow:left];
            while (!g && until.timeIntervalSinceNow > 0)
                [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:MIN(0.05, until.timeIntervalSinceNow)]];
            if (!g) { [q stopQuery]; if (obs) [NSNotificationCenter.defaultCenter removeObserver:obs]; }
        } else {
            dispatch_async(dispatch_get_main_queue(), startq);
            if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(left * NSEC_PER_SEC))))
                dispatch_async(dispatch_get_main_queue(), ^{ if (q) { [q stopQuery]; if (obs) [NSNotificationCenter.defaultCenter removeObserver:obs]; } });
        }
        got = g; listed = n; present = pr;
        __block int ns = 0; dispatch_semaphore_t ps = dispatch_semaphore_create(0); nw_path_monitor_t m = nw_path_monitor_create();
        nw_path_monitor_set_queue(m, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
        nw_path_monitor_set_update_handler(m, ^(nw_path_t path) { if (!ns) { ns = nw_path_get_status(path) == nw_path_status_satisfied ? 1 : 2; dispatch_semaphore_signal(ps); } });
        nw_path_monitor_start(m); dispatch_semaphore_wait(ps, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(NSEC_PER_SEC))); nw_path_monitor_cancel(m); net = ns == 1;
    }
    int ok = got && (listed ? present >= listed : net);
    LOG("iCloud 同步：本机从没同步成功过，先看云端条目列表：收集%s、列出 %d 个包、已到本机 %d 个、网络%s → %s", got ? "完成" : "未完成", listed, present, net ? "通" : "不通",
        ok ? "看清了，同步" : "没看清，本次不同步（不上传、不建 saves_v2），下次启动再看");
    return ok;
}
void dm_cloud_boot(void) {
    const char *sv = getenv("DM_SAVE"), *sub = getenv("DM_TEST_ICLOUD_SUB"), *ldir = getenv("DM_TEST_ICLOUD_DIR"), *lroot = getenv("DM_TEST_ICLOUD_ROOT"); if (!sv) return;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *save = @(sv); int real = [save.stringByStandardizingPath isEqualToString:[doc stringByAppendingPathComponent:@"save"].stringByStandardizingPath];
#if TARGET_OS_OSX
    real = getenv("DM_CLOUD_REAL") != NULL; (void)doc;   // Mac：真实存档由启动代码认（app_mac.m：没带 DM_APP_SUPPORT 的那种起法） | Mac: the real save is recognised by the startup code (app_mac.m: the way of launching without DM_APP_SUPPORT)
#endif
    int off = getenv("DM_TEST_ICLOUD_OFF") != NULL;                   // 测试总闸：脚本起的局一律带它（tools/cloud_guard.sh），整局不同步 | Test master gate: runs launched by scripts always carry it (tools/cloud_guard.sh), and the whole run does not sync
    if (!real && !sub && !ldir && !lroot) { if (off) LOG("iCloud 同步：测试开关 DM_TEST_ICLOUD_OFF，本局不同步");   // 测试存档：不同步 | Test save: not synced
#if TARGET_OS_OSX
        else LOG("iCloud 同步：测试用的存档目录（DM_APP_SUPPORT），本局不同步");
#endif
        return; }
    // Mac 上跑的 iOS 包：缺省整个不连真实 iCloud。这个包只发 iPhone / iPad，Mac 上跑的全是开发与测试；开发机登着真实账号时，
    // 它就是那个账号的又一台设备，任何脚本、手点图标都会和真实云端双向同步（出过把测试存档传进真实云端）。本地测试云端（DIR / ROOT）不受影响；
    // 真要在 Mac 上连真实云端：同时给 DM_REAL_ICLOUD=1 与 DM_REAL_ICLOUD_CONFIRM=写入真实云端。真机不走这一支。
    // iOS app running on a Mac: by default it does not connect to real iCloud at all. This package ships only for iPhone / iPad, and everything run on a Mac is development and testing; when the development machine is signed in to a real account,
    // it is one more device of that account, and any script or a tap on the icon would sync both ways with the real cloud (test saves have been uploaded to the real cloud before). The local test cloud (DIR / ROOT) is unaffected;
    // to really connect to the real cloud on a Mac: give both DM_REAL_ICLOUD=1 and DM_REAL_ICLOUD_CONFIRM=写入真实云端. Real devices do not take this branch.
    if (NSProcessInfo.processInfo.isiOSAppOnMac && !ldir && !lroot) { const char *r1 = getenv("DM_REAL_ICLOUD"), *r2 = getenv("DM_REAL_ICLOUD_CONFIRM");
        if (!(r1 && !strcmp(r1, "1") && r2 && !strcmp(r2, "写入真实云端"))) { LOG("iCloud 同步：Mac 上运行：缺省不同步 iCloud"); return; }
        LOG("【红】iCloud 同步：Mac 上运行，已显式放开（DM_REAL_ICLOUD）：本局连真实云端"); }
    if (off) { LOG("iCloud 同步：测试开关 DM_TEST_ICLOUD_OFF，本局不同步"); return; }
    if (!dm_custom_get("icloud")) { LOG("iCloud 同步：自定义页已关，不同步"); return; }
    double t0 = CFAbsoluteTimeGetCurrent(), limit = 5.0;
    if (ldir) g_cloud = test_path(ldir);                             // 测试：本地目录当云端（没有 iCloud 权限的开发包也能验整条链） | Test: use a local directory as the cloud (a dev build without iCloud entitlement can also verify the whole chain)
    else if (sub) { NSURL *ct = container(limit); if (!ct) return; g_cloud = [[ct.path stringByAppendingPathComponent:@"Documents"] stringByAppendingPathComponent:@(sub)]; }
    else { NSString *docs = test_path(lroot); if (!lroot) { NSURL *ct = container(limit); if (!ct) return; docs = [ct.path stringByAppendingPathComponent:@"Documents"]; }
        if (never_synced([save stringByAppendingString:@".cloudstate"]) && (!lroot || getenv("DM_TEST_ICLOUD_FIRST")) && !first_listing_ok(t0 + limit)) return;
        if (!migrate_v2(docs, t0 + limit)) return;
        NSString *nw = [docs stringByAppendingPathComponent:@"saves_v2"]; if (!v2_ready(nw, [save stringByAppendingString:@".cloudv2ok"])) return;
        g_cloud = nw; }
    [NSFileManager.defaultManager createDirectoryAtPath:g_cloud withIntermediateDirectories:YES attributes:nil error:nil];
    g_save = save; g_bak = [save stringByAppendingString:@".cloudbak"]; g_state = [save stringByAppendingString:@".cloudstate"];
    if (ldir) { g_bak = [g_cloud stringByAppendingString:@".cloudbak"]; g_state = [g_cloud stringByAppendingString:@".cloudstate"]; }   // 测试的本地云端：状态与备份跟云端目录走，不与真存档（同一路径、连真实 iCloud 时）共用——共用的话测试局会改写真存档的同步状态 | Local cloud for tests: state and backup follow the cloud directory and are not shared with the real save (same path, when connected to real iCloud) -- if shared, test runs would rewrite the real save's sync state
    g_q = dispatch_queue_create("dm.cloud", DISPATCH_QUEUE_SERIAL);
    for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:save.stringByDeletingLastPathComponent error:nil] ?: @[])   // 上次被杀时没删掉的提交快照 | Commit snapshots not deleted when we were last killed
        if ([f hasPrefix:[save.lastPathComponent stringByAppendingString:@".cloudsnap"]]) [NSFileManager.defaultManager removeItemAtPath:[save.stringByDeletingLastPathComponent stringByAppendingPathComponent:f] error:nil];
    double t1 = CFAbsoluteTimeGetCurrent(); int pend = pending_downloads(1);
    while (pend && CFAbsoluteTimeGetCurrent() - t0 < limit) { usleep(100000); pend = pending_downloads(0); }
    double t2 = CFAbsoluteTimeGetCurrent(); int n = run_sync(save, 1);
    LOG("iCloud 同步：启动分段 取容器与查齐 %.0f ms、等下载 %.0f ms、文件协调与同步 %.0f ms", (t1 - t0) * 1000, (t2 - t1) * 1000, (CFAbsoluteTimeGetCurrent() - t2) * 1000);
    LOG("iCloud 同步：启动 %.0f ms，动作 %d 个%s（云端目录 %s）", (CFAbsoluteTimeGetCurrent() - t0) * 1000, n, pend ? [NSString stringWithFormat:@"，%d 个包没等到下载完、本次跳过", pend].UTF8String : "", ldir ? "本地测试目录" : sub ? sub : "saves_v2");
    g_dm_save_commit_hook = on_commit;
    { extern void (*g_dm_ingame_hook)(int); extern void (*g_dm_charsel_hook)(void); g_dm_ingame_hook = cloud_ingame; g_dm_charsel_hook = cloud_charsel; }   // 进局记一局开始；回到前端 = 这一局结束，传一次 | Entering a run marks the start of a run; returning to the front end = this run has ended, upload once
}
