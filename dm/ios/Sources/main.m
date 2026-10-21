// dm iOS 宿主 App。启动参数 -mode <名> 选要跑的内容；日志逐行写 Documents/<名>.txt 并 fsync，进程被杀也留得下。
// dm iOS host App. The launch argument -mode <name> selects what to run; the log is written line by line to Documents/<name>.txt with fsync, so it survives even if the process is killed.
#import <UIKit/UIKit.h>
#include <sys/resource.h>
static int g_nofile_was, g_nofile_now;   // 启动时的文件描述符软上限（原值 / 现值），开日志后记一行 | File descriptor soft limit at launch (old / new value); one line is logged after the log opens
#import <Metal/Metal.h>
#import <BackgroundAssets/BackgroundAssets.h>
#import "asset_dl.h"
#import <CommonCrypto/CommonDigest.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <libkern/OSCacheControl.h>
#include <sys/sysctl.h>
#include <os/proc.h>
#include <mach/mach.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
void dm_host_shutdown(const char *why);   // 宿主优雅退出（dm/host/hle_lifecycle.c） | Graceful host exit (dm/host/hle_lifecycle.c)
#include "front.h"                        // front_game_res（全屏输出分辨率） | front_game_res (full-screen output resolution)
#include "../../host/romfs_validate.h"
#include "../../host/dm_i18n.h"
static NSString *T(const char *k) { return @(dm_ui(k)); }   // 我们自己的界面文字（中英表 dm/i18n/ui_text.tsv，语言跟原版设置走） | our own UI text (Chinese-English string table dm/i18n/ui_text.tsv, language follows the original game's setting)
#import <Network/Network.h>
#include "dm_metal_cache_guard.h"

#if __IPHONE_OS_VERSION_MAX_ALLOWED < 260400
// Xcode 26.2 的 SDK 没有这两个方法的声明，这里只补声明；运行时仍要求 iOS 26.4。
// The Xcode 26.2 SDK lacks declarations for these two methods; only the declarations are added here, and the calls still require iOS 26.4 at runtime.
@interface BAAssetPackManager (DmIOS26_4Compatibility)
- (BOOL)assetPackIsAvailableLocallyWithIdentifier:(NSString *)assetPackIdentifier API_AVAILABLE(ios(26.4));
- (void)getLocalStatusOfAssetPackWithIdentifier:(NSString *)assetPackIdentifier
                            completionHandler:(void (^)(BAAssetPackStatus status))completionHandler API_AVAILABLE(ios(26.4));
@end
#endif

static NSString *g_mode;
static int g_log = -1;
static int g_log_tee;
static int g_log_stderr = -1;
static int g_log_pipe[2] = {-1, -1};
static pthread_t g_log_thread;
void plog(const char *fmt, ...);

static void metal_cache_stats(NSString *directory, unsigned long *file_count, unsigned long long *byte_count) {
    *file_count = 0; *byte_count = 0;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSDictionary *entry = [fm attributesOfItemAtPath:directory error:nil];
    if ([entry[NSFileType] isEqualToString:NSFileTypeRegular]) {
        *file_count = 1; *byte_count = [entry[NSFileSize] unsignedLongLongValue]; return;
    }
    if (![entry[NSFileType] isEqualToString:NSFileTypeDirectory]) return;
    for (NSString *relative in [fm enumeratorAtPath:directory]) {
        NSString *path = [directory stringByAppendingPathComponent:relative];
        NSDictionary *attributes = [fm attributesOfItemAtPath:path error:nil];
        if ([attributes[NSFileType] isEqualToString:NSFileTypeRegular]) {
            (*file_count)++; *byte_count += [attributes[NSFileSize] unsignedLongLongValue];
        }
    }
}

static BOOL metal_name(NSString *name) {
    return [name rangeOfString:@"metal" options:NSCaseInsensitiveSearch].location != NSNotFound;
}

static void metal_cache_report_and_clean(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES)[0];
    // 系统按 App 存的着色器缓存：真机读数在 Caches/<bundle id>/com.apple.metal（1.6 MB）与 com.apple.metalfe；Caches/com.apple.metal 为 0（旧位置，一并清）
    // Shader caches the system stores per App: real-device readings are in Caches/<bundle id>/com.apple.metal (1.6 MB) and com.apple.metalfe; Caches/com.apple.metal is 0 (old location, cleared as well)
    NSString *app = [caches stringByAppendingPathComponent:NSBundle.mainBundle.bundleIdentifier ?: @"app"];
    NSArray<NSString *> *targets = @[[app stringByAppendingPathComponent:@"com.apple.metal"], [app stringByAppendingPathComponent:@"com.apple.metalfe"],
                                     [caches stringByAppendingPathComponent:@"com.apple.metal"]];
    unsigned long before_files = 0; unsigned long long before_bytes = 0;
    for (NSString *t in targets) {
        unsigned long f = 0; unsigned long long b = 0; metal_cache_stats(t, &f, &b); before_files += f; before_bytes += b;
        plog("METALCACHE 现存 %lu 个 %llu 字节 %s", f, b, t.fileSystemRepresentation);
    }
    for (NSString *base in @[caches, NSTemporaryDirectory()]) {
        for (NSString *relative in [fm enumeratorAtPath:base]) {
            NSString *path = [base stringByAppendingPathComponent:relative];
            if (metal_name(path.lastPathComponent) && ![targets containsObject:path]) {
                unsigned long files = 0; unsigned long long bytes = 0;
                metal_cache_stats(path, &files, &bytes);
                plog("METALCACHE 候选 %s %llu 字节", path.fileSystemRepresentation, bytes);
            }
        }
    }
    const char *cold = getenv("DM_METAL_COLD");
    if (!cold || strcmp(cold, "1")) return;

    NSString *root = NSHomeDirectory();
    for (NSString *t in targets) {
        if (![fm attributesOfItemAtPath:t error:nil]) continue;
        if (!dm_metal_cache_path_allowed(root.fileSystemRepresentation, t.fileSystemRepresentation)) {
            plog("METALCACHE 【红】拒删 %s", t.fileSystemRepresentation); return;
        }
        NSError *error = nil;
        if (![fm removeItemAtPath:t error:&error]) {
            plog("METALCACHE 【红】删除失败 %s：%s", t.fileSystemRepresentation, error.localizedDescription.UTF8String); return;
        }
    }
    unsigned long after_files = 0; unsigned long long after_bytes = 0;
    for (NSString *t in targets) { unsigned long f = 0; unsigned long long b = 0; metal_cache_stats(t, &f, &b); after_files += f; after_bytes += b; }
    plog("METALCACHE 已清 %lu 个 %llu 字节 → 现存 %lu 个 %llu 字节", before_files, before_bytes, after_files, after_bytes);
}

static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w > 0) { p += w; n -= (size_t)w; }
        else if (w < 0 && errno == EINTR) continue;
        else break;
    }
}

// 测试开关 DM_LOG_PORT=<端口>（冒烟用，Mac 经 iproxy 走 USB 连过来）：App 在本机回环监听，客户端连上先补发 boot.txt 已写内容，再逐行实时转发。
// 不经 devicectl：实测 devicectl --console 首行延迟约 76 s，被杀的会话还拖住下一条命令约 80 s
// Test switch DM_LOG_PORT=<port> (for the smoke test; the Mac connects over USB through iproxy): the App listens on local loopback; when a client connects it first replays what boot.txt already holds, then forwards line by line in real time.
// Not through devicectl: measured, devicectl --console delays the first line by about 76 s, and a killed session stalls the next command by about 80 s
static pthread_mutex_t g_log_client_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_log_client = -1;
static char g_log_path[1024];
static void log_client_write(const char *p, size_t n) {
    pthread_mutex_lock(&g_log_client_mu);
    if (g_log_client >= 0) {
        while (n) { ssize_t w = write(g_log_client, p, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) { close(g_log_client); g_log_client = -1; break; } p += w; n -= (size_t)w; }
    }
    pthread_mutex_unlock(&g_log_client_mu);
}
static void *log_port_reader(void *arg) {   // 客户端发「quit」→ App 自行退出：下次拉起不用 devicectl --terminate-existing（实测它会拖住 37–120 s） | The client sends "quit" → the App exits by itself: the next launch does not need devicectl --terminate-existing (measured to stall for 37–120 s)
    // 收到即在同一连接上同步回执，客户端见到回执才关连接：iproxy 客户端一 EOF 就拆整条 usbmux 连接（iproxy.c 2.1.1），
    // 发完即关时 quit 可能随拆除丢失（冒烟残留进程的根因）。回执不走 tee 管道——_exit 会截断管道里没转发的行
    // On receipt, acknowledge synchronously on the same connection, and the client closes the connection only after seeing the ack: the iproxy client tears down the whole usbmux connection as soon as it sees EOF (iproxy.c 2.1.1),
    // so with send-then-close the quit can be lost in the teardown (root cause of leftover processes after the smoke test). The ack does not go through the tee pipe -- _exit would truncate lines in the pipe that have not been forwarded
    int c = (int)(intptr_t)arg; char b[64], acc[16] = {0}; ssize_t r;
    while ((r = read(c, b, sizeof b - 1)) > 0) {
        b[r] = 0; size_t n = strlen(acc); char cat[sizeof acc + sizeof b]; memcpy(cat, acc, n); memcpy(cat + n, b, (size_t)r + 1);   // 跨块累积：quit 被拆成两次 read 也认 | Accumulate across chunks: quit is recognized even when split across two reads
        if (strstr(cat, "dump")) { void crash_dump_now(const char *why); crash_dump_now("DM_LOG_PORT dump"); cat[0] = 0; }   // 冒烟判红时先发 dump 留现场（logport.py snapshot_hang），再发 quit | When the smoke test fails, send dump first to keep the diagnostics (logport.py snapshot_hang), then send quit
        if (strstr(cat, "quit")) {
            static const char ack[] = "DM_LOG_PORT：收到 quit，退出\n";
            pthread_mutex_lock(&g_log_client_mu); write_all(c, ack, sizeof ack - 1); pthread_mutex_unlock(&g_log_client_mu);   // 同锁：不插进转发中的半行 | Same lock: do not insert into a half line being forwarded
            fprintf(stderr, "%s", ack); dm_host_shutdown("DM_LOG_PORT quit"); _exit(0);
        }
        size_t m = strlen(cat), keep = m < sizeof acc - 1 ? m : sizeof acc - 1; memcpy(acc, cat + m - keep, keep); acc[keep] = 0;
    }
    return NULL;
}
static int log_port_open(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 1)) {
        fprintf(stderr, "【红】DM_LOG_PORT：监听 127.0.0.1:%d 失败：%s\n", port, strerror(errno)); if (s >= 0) close(s); return -1;
    }
    return s;
}
static void *log_port_main(void *arg) {
    int port = (int)(intptr_t)arg, one = 1, s = log_port_open(port);
    if (s < 0) return NULL;
    fprintf(stderr, "DM_LOG_PORT：监听 127.0.0.1:%d\n", port);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            // iOS 挂起 App 时回收其网络资源，监听 socket 失效（L4 切后台：回前台后日志口再连不上）：关掉重建，不退出线程
            // When iOS suspends the App it reclaims its network resources and the listening socket becomes invalid (L4 backgrounding: after returning to the foreground the log port can no longer be connected): close and recreate it, do not exit the thread
            fprintf(stderr, "DM_LOG_PORT：accept 失败（%s），重建监听\n", strerror(errno)); close(s);
            while ((s = log_port_open(port)) < 0) sleep(1);
            continue;
        }
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        pthread_mutex_lock(&g_log_client_mu);   // 持锁补发：补发期间 tee 线程的新行排在后面，不丢不乱序 | Replay while holding the lock: new lines from the tee thread during the replay queue up behind it, nothing lost and nothing out of order
        if (g_log_client >= 0) close(g_log_client);
        char lp[PATH_MAX]; int f = fcntl(g_log, F_GETPATH, lp) == 0 ? open(lp, O_RDONLY) : -1; char b[16384]; ssize_t r;   // 按描述符现在的路径读：首次到前台时日志会改名（log_promote） | read via the descriptor's current path: the log is renamed on first foreground (log_promote)
        if (f >= 0) { while ((r = read(f, b, sizeof b)) > 0) { const char *p = b; while (r > 0) { ssize_t w = write(c, p, (size_t)r); if (w <= 0) break; p += w; r -= w; } } close(f); }
        g_log_client = c;
        pthread_mutex_unlock(&g_log_client_mu);
        pthread_t rd; if (!pthread_create(&rd, NULL, log_port_reader, (void *)(intptr_t)c)) pthread_detach(rd);
    }
    return NULL;
}
static void *log_tee_main(void *unused) {
    (void)unused;
    char buf[4096], line[8192];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(g_log_pipe[0], buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (ssize_t i = 0; i < n; i++) {
            if (used < sizeof line) line[used++] = buf[i];
            if (buf[i] == '\n') {
                pthread_mutex_lock(&g_log_client_mu); write_all(g_log, line, used); fsync(g_log); pthread_mutex_unlock(&g_log_client_mu);   // 与补发同锁：补发读文件时不会漏这行也不会重发 | Same lock as the replay: while the replay reads the file it neither misses this line nor sends it twice
                if (g_log_stderr >= 0) write_all(g_log_stderr, line, used);
                log_client_write(line, used);
                used = 0;
            } else if (used == sizeof line) {
                write_all(g_log, line, used);
                used = 0;
            }
        }
    }
    if (used) {
        write_all(g_log, line, used);
        fsync(g_log);
        write_all(g_log_stderr, line, used);
    }
    return NULL;
}

void plog(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, ap); va_end(ap);
    if (n < 0) return; if (n > (int)sizeof b - 2) n = sizeof b - 2; b[n++] = '\n';
    if (g_log_tee) write_all(2, b, (size_t)n);
    else if (g_log >= 0) { write(g_log, b, n); fsync(g_log); } else write(2, b, n);
}

// ---- 启动计时 T1–T6：零点 = 进程创建（内核 p_starttime，墙钟），单位 ms；日志开了以后写进 <模式>.txt ----
// ---- Launch timing T1–T6: zero point = process creation (kernel p_starttime, wall clock), unit ms; written to <mode>.txt once the log is open ----
static struct timeval g_pst;   // 进程创建时刻 | Process creation time
static double g_T[7];          // T1 静态构造 / T2 main / T3 didFinishLaunching / T4 willConnect 末 / T5 boot_thread 入口 / T6 dm_main 前 | T1 static constructor / T2 main / T3 didFinishLaunching / T4 end of willConnect / T5 boot_thread entry / T6 before dm_main
static double since_start(void) {
    if (!g_pst.tv_sec) { struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
                         if (!sysctl(mib, 4, &kp, &n, NULL, 0) && n) g_pst = kp.kp_proc.p_starttime; }
    struct timeval tv; gettimeofday(&tv, NULL);
    return (tv.tv_sec - g_pst.tv_sec) * 1e3 + (tv.tv_usec - g_pst.tv_usec) / 1e3;
}
__attribute__((constructor)) static void boot_t1(void) { g_T[1] = since_start(); }

// ---- 可执行页探针：每个模式一次启动，被杀即停在最后一行 ----
// ---- Executable-page probe: one launch per mode; if killed, it stops at the last line ----
static const uint32_t k_code[] = { 0xD2800540 /* mov x0,#42 */, 0xD65F03C0 /* ret */ };

static void try_call(uint8_t *p, const char *tag) {
    sys_icache_invalidate(p, sizeof k_code);
    plog("%s: 跳入 %p", tag, p);
    long r = ((long (*)(void))p)();
    plog("%s: 返回 %ld（期望 42）", tag, r);
}

static void probe_anon(void) {   // 匿名页 写 → mprotect 读+执行 → 跳 | Anonymous page: write → mprotect read+execute → jump
    uint8_t *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    plog("anon mmap RW = %p errno %d", p, p == MAP_FAILED ? errno : 0);
    if (p == MAP_FAILED) return;
    memcpy(p, k_code, sizeof k_code);
    int rc = mprotect(p, 0x4000, PROT_READ | PROT_EXEC);
    plog("anon mprotect RX = %d errno %d", rc, rc ? errno : 0);
    if (!rc) try_call(p, "anon");
}

static void probe_mapjit(void) { // MAP_JIT
    uint8_t *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    plog("MAP_JIT mmap RWX = %p errno %d", p, p == MAP_FAILED ? errno : 0);
    if (p == MAP_FAILED) return;
    memcpy(p, k_code, sizeof k_code);   // iOS 无 pthread_jit_write_protect_np；能映射到即直接写 | iOS has no pthread_jit_write_protect_np; if the mapping succeeds, write directly
    try_call(p, "mapjit");
}

static void probe_file(void) {   // 沙箱里的未签名文件 mmap PROT_EXEC | mmap PROT_EXEC of an unsigned file inside the sandbox
    NSString *f = [NSTemporaryDirectory() stringByAppendingPathComponent:@"code.bin"];
    int fd = open(f.fileSystemRepresentation, O_RDWR | O_CREAT | O_TRUNC, 0644);
    uint8_t pg[0x4000] = {0}; memcpy(pg, k_code, sizeof k_code); write(fd, pg, sizeof pg);
    uint8_t *p = mmap(NULL, 0x4000, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    plog("file mmap RX = %p errno %d", p, p == MAP_FAILED ? errno : 0);
    close(fd);
    if (p != MAP_FAILED) try_call(p, "file");
}


// ---- TSD 固定槽探针：连续建键直到拿到固定高号，多余的删掉；再按 loader 的 tsd_check 方式经 tpidrro_el0 读回 ----
// ---- TSD fixed-slot probe: keep creating keys until the fixed high number is obtained, delete the extras; then read back through tpidrro_el0 the way the loader's tsd_check does ----
static int claim_key(pthread_key_t want) {
    pthread_key_t got[512]; int n = 0, ok = 0;
    for (;;) {
        pthread_key_t k; if (pthread_key_create(&k, NULL)) break;
        if (k == want) { ok = 1; break; }
        if (k > want || n == 512) { pthread_key_delete(k); break; }
        got[n++] = k;
    }
    plog("claim %lu: %s，途经 %d 个键（首个 %lu）", (unsigned long)want, ok ? "拿到" : "失败", n, n ? (unsigned long)got[0] : 0ul);
    while (n) pthread_key_delete(got[--n]);
    return ok;
}
static void probe_tsd(void) {
    pthread_key_t k0; pthread_key_create(&k0, NULL); plog("当前首个空闲动态键 %lu", (unsigned long)k0); pthread_key_delete(k0);
    for (pthread_key_t want = 508; want < 512; want++) {
        if (!claim_key(want)) continue;
        pthread_setspecific(want, (void *)(0x1234 + want));
        uint64_t base; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
        uint64_t v = ((uint64_t *)(base & ~7ull))[want];
        plog("槽 %lu 经 tpidrro 读回 %#llx（期望 %#lx）", (unsigned long)want, v, 0x1234 + (unsigned long)want);
    }
}

// ---- boot：跑宿主（loader + HLE）到客体入口；宿主日志走 stderr，重定向到 Documents/boot.txt ----
// ---- boot: run the host (loader + HLE) up to the guest entry; the host log goes to stderr, redirected to Documents/boot.txt ----
int dm_main(int argc, char **argv);   // dm/host/main.c 以 -Dmain=dm_main 编入 | dm/host/main.c is compiled in with -Dmain=dm_main
UIView *front_ios_view(CGRect frame); // front_ios.m
void front_ios_lifecycle_install(void); // front_ios.m
void front_ios_focus_event(int ev);     // front_ios.m
static char *g_boot_argv[8];
static int g_boot_argc = 4;
// RomFS 在哪：开发机由 devicectl 推进 Application Support/romfs；TestFlight / 商店安装在苹果托管资源包 romfs 里（只读）。都没有返回 nil（why 写原因）
// ---- 老机型补充包：显卡不支持 BC 的机型（A16 及以前）要用的 ASTC 贴图，玩家用「文件」拷进「D2R Mobile」或 App 里下载 ----
// 包内 romfs_overlay_astc/pack.json = 本包配套的补充包（档名、字节数、SHA-256，出包时由 dm/tools/astc_pack.py finalize 生成）；没有这个文件 = 本包不带老机型支持。
// 补充包摆法：<某文件夹>/data/<档名>，标准名「Legacy Device Pack」；也认改了名 / 多套一层的。校验通过后它作为 RomFS 覆盖层的第二层（main.m boot_prepare）
// Where RomFS is: on a dev machine devicectl pushes it into Application Support/romfs; TestFlight / store installs have it in the Apple-hosted asset pack romfs (read-only). Returns nil if neither exists (why holds the reason)
// ---- Legacy Device Pack: the ASTC textures needed by models whose GPU does not support BC (A16 and earlier); the player copies it into 「D2R Mobile」 with the Files app or downloads it inside the App ----
// In the bundle, romfs_overlay_astc/pack.json = the Legacy Device Pack matching this build (archive name, byte count, SHA-256; generated at packaging time by dm/tools/astc_pack.py finalize); without this file = this build carries no legacy-device support.
// Legacy Device Pack layout: <some folder>/data/<archive name>, standard name 「Legacy Device Pack」(Legacy Device Supplement Pack); renamed / one-extra-level variants are accepted too. After validation it serves as the second layer of the RomFS overlay (main.m boot_prepare)
static NSDictionary *astc_info(void) {
    static NSDictionary *info; static dispatch_once_t once;
    dispatch_once(&once, ^{ NSData *d = [NSData dataWithContentsOfFile:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"romfs_overlay_astc/pack.json"]];
        NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
        if ([j isKindOfClass:NSDictionary.class] && [j[@"name"] isKindOfClass:NSString.class] && [j[@"size"] isKindOfClass:NSNumber.class] && [j[@"sha256"] isKindOfClass:NSString.class]) info = j; });
    return info;
}
static BOOL astc_is_pack_dir(NSString *d) {   // 这个文件夹的 data 里有补充包的档（或下到一半的 .part） | This folder's data contains the Legacy Device Pack's archive (or a half-downloaded .part)
    NSString *nm = astc_info()[@"name"] ?: @"00-00000008.data"; NSFileManager *fm = NSFileManager.defaultManager; NSString *data = [d stringByAppendingPathComponent:@"data"];
    return [fm fileExistsAtPath:[data stringByAppendingPathComponent:nm]] || [fm fileExistsAtPath:[data stringByAppendingPathComponent:[nm stringByAppendingString:@".part"]]];
}
static int g_astc_none;   // 上一次找补充包的结论是「根本没有」（不是大小 / 内容不对）：弹框里不重复这句原因 | The last search for the Legacy Device Pack concluded "there is none at all" (not a wrong size / wrong content): do not repeat that reason in the dialog
static NSString *g_astc_dir;   // 校验通过的补充包根目录（nil = 没有） | Root directory of the validated Legacy Device Pack (nil = none)
// 找补充包并校验：字节数对、SHA-256 对。哈希 1 GB 要一两秒，结果按（路径、字节数、修改时间）记在偏好里，下次同一个文件不再算。找到返回根目录，否则 nil 并写原因
// Find the Legacy Device Pack and validate it: byte count matches, SHA-256 matches. Hashing 1 GB takes a second or two, so the result is stored in preferences keyed by (path, byte count, modification time) and the same file is not hashed again next time. Returns the root directory if found, otherwise nil with the reason written
static NSString *astc_pack_locate(char *why, size_t n) {
    g_astc_none = 0;
    NSDictionary *info = astc_info(); if (!info) { snprintf(why, n, "%s", dm_ui("dmOldNoSupportPkg")); return nil; }
    NSFileManager *fm = NSFileManager.defaultManager; NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0], *nm = info[@"name"];
    NSMutableArray<NSString *> *cand = [NSMutableArray arrayWithObject:[doc stringByAppendingPathComponent:@"Legacy Device Pack"]];
    for (NSString *a in [[fm contentsOfDirectoryAtPath:doc error:nil] sortedArrayUsingSelector:@selector(compare:)] ?: @[]) {
        NSString *pa = [doc stringByAppendingPathComponent:a]; [cand addObject:pa];
        for (NSString *b in [fm contentsOfDirectoryAtPath:pa error:nil] ?: @[]) [cand addObject:[pa stringByAppendingPathComponent:b]]; }
    for (NSString *d in cand) {
        NSString *f = [[d stringByAppendingPathComponent:@"data"] stringByAppendingPathComponent:nm];
        NSDictionary *at = [fm attributesOfItemAtPath:f error:nil]; if (!at) continue;
        if ((long long)at.fileSize != [info[@"size"] longLongValue]) { snprintf(why, n, dm_ui("dmOldPackSize"), d.lastPathComponent.UTF8String, at.fileSize / 1e9, [info[@"size"] longLongValue] / 1e9); continue; }
        NSString *stamp = [NSString stringWithFormat:@"%@|%llu|%.0f|%@", f, at.fileSize, at.fileModificationDate.timeIntervalSince1970, info[@"sha256"]];
        if ([[NSUserDefaults.standardUserDefaults stringForKey:@"dm_astc_pack_ok"] isEqualToString:stamp]) return d;
        CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent(); NSFileHandle *h = [NSFileHandle fileHandleForReadingAtPath:f]; CC_SHA256_CTX c; CC_SHA256_Init(&c);
        for (;;) { @autoreleasepool { NSData *x = [h readDataOfLength:8 << 20]; if (!x.length) break; CC_SHA256_Update(&c, x.bytes, (CC_LONG)x.length); } }
        unsigned char dg[32]; CC_SHA256_Final(dg, &c); NSMutableString *hex = [NSMutableString new]; for (int i = 0; i < 32; i++) [hex appendFormat:@"%02x", dg[i]];
        plog("boot: 补充包 %s 校验用时 %.1f 秒：%s", f.UTF8String, CFAbsoluteTimeGetCurrent() - t0, [hex isEqualToString:info[@"sha256"]] ? "对" : "不对");
        if (![hex isEqualToString:info[@"sha256"]]) { snprintf(why, n, dm_ui("dmOldPackBad"), d.lastPathComponent.UTF8String); continue; }
        [NSUserDefaults.standardUserDefaults setObject:stamp forKey:@"dm_astc_pack_ok"]; return d;
    }
    if (!why[0]) { snprintf(why, n, "%s", dm_ui("dmOldPackNone")); g_astc_none = 1; }
    return nil;
}
// 找游戏资源，按这个顺序，先到先得（每个出口的屏上文字见 AssetVC start / direct）。资源 = 那批档案文件（NN-XXXXXXXX.data）；配置文件用 App 自带的，
// 密钥与清单游戏不读（见 host/romfs_validate.c），所以这里不看、不补、不报那几个小文件：
//   ① 本机 Application Support/romfs（开发机推进来的）校验通过 → 用它
//   ② 「D2R Mobile」（Documents）下 4 层以内直接放着档案文件的目录：完整 → 用它；不完整 → 返回空、置 g_doc_bad（屏上「…里的资源还不完整：…」，不试托管下载，从下载站补齐）
//   ③ 苹果托管资源包校验通过 → 用它
//   ④ 都没有 → 返回空。「D2R Mobile」里有别的文件夹时置 g_doc_stray（资源界面照常先试托管下载，托管与下载站都走不通时屏上「没找到游戏资源。「D2R Mobile」里现在有：…」）；
//      一个文件夹都没有时两个标志都不置（屏上「还没有游戏资源 请先从内测页…」）
// Find the game resources in this order, first hit wins (for the on-screen text of each exit see AssetVC start / direct). Resources = that batch of
// archive files (NN-XXXXXXXX.data); the config file is the one bundled with the App,
// and the game does not read the key or the manifests (see host/romfs_validate.c), so those small files are not looked at, repaired or reported here:
//   ① Local Application Support/romfs (pushed in from a development machine) passes validation → use it
//   ② A directory within 4 levels under "D2R Mobile" (Documents) that directly contains archive files: complete → use it; incomplete → return empty and set
//   g_doc_bad (on screen "The resources in ... are not complete yet: ...", managed download is not tried, fill in from the download site)
//   ③ Apple-hosted managed asset pack passes validation → use it
//   ④ None of them → return empty. Set g_doc_stray when "D2R Mobile" contains other folders (the resource screen still tries the managed download first as
//   usual; when neither managed download nor the download site works, on screen "Game resources not found. "D2R Mobile" currently contains: ...");
//      when there is no folder at all neither flag is set (on screen "No game resources yet. Please first, from the beta page...")
static int g_doc_bad;   // romfs_locate 的附带结论：「文件」里有资源文件夹但校验没过（why = 哪个文件、现有 / 应有字节数）——此时不去试托管下载 | Side conclusion of romfs_locate: the Files app has an asset folder but validation did not pass (why = which file, present / expected byte counts) -- in this case do not try the hosted download
static int g_pack_wait;   // 托管资源包的目录已经在、系统还没说「本地可用」（还在下 / 还在落盘） | The managed asset pack's directory already exists but the system has not said "locally available" (still downloading / still being written to disk)
static int g_doc_stray;   // 另一条附带结论：「D2R Mobile」里有文件夹但没有一个是资源（why = 现有哪些文件夹），托管资源包也没到——照常去试托管下载，下载站也走不通时把 why 给玩家看 | The other side conclusion: 「D2R Mobile」 has folders but none of them is assets (why = which folders exist now), and the hosted asset pack has not arrived either -- try the hosted download as usual, and show why to the player when the download site is unreachable too
// 本包有没有配置苹果的托管资源包（Info.plist 的 BAHasManagedAssetPacks）。没配置的包（免费账号档、不带游戏内容的壳：tools/project_free.py、
// tools/shell_build.sh 把那几项去掉了）一碰 BAAssetPackManager，系统接口内部断言直接让进程退出——真机实测：没有资源的第一次启动必崩。
// 所以凡是要问那个接口的地方先问这里。另一种同样会崩的包：Info.plist 还声明着，但签名里没有它要的 app group（用自己的 Apple ID / 自签工具重签的
// 包——描述文件里没有我们的 app group）：系统 Swift 层报「app group … is inaccessible」致命错误。所以还要核声明的 app group 拿得到容器目录（拿不到 = 签名没带这项权限）
// Whether this package configures Apple's managed asset packs (BAHasManagedAssetPacks in Info.plist). In packages without it (the free-account build, and the shell without game content: tools/project_free.py and
// tools/shell_build.sh remove those entries), touching BAAssetPackManager makes an assertion inside the system interface terminate the process; measured on a device, the first launch without resources always crashed.
// So every place that would ask that interface asks here first. Another kind of package crashes the same way: Info.plist still declares it, but the signature lacks the app group it needs (packages re-signed with your own Apple ID / a sideloading tool,
// whose provisioning profile does not include our app group): the system Swift layer raises a fatal "app group ... is inaccessible" error. So we also check that the declared app group returns a container directory (none = the signature does not carry that entitlement)
static BOOL ba_configured(void) {
    static int v = -1; if (v >= 0) return v;
    NSString *g = [NSBundle.mainBundle objectForInfoDictionaryKey:@"BAAppGroupID"];
    if (![[NSBundle.mainBundle objectForInfoDictionaryKey:@"BAHasManagedAssetPacks"] boolValue]) v = 0;
    else if (!g.length || ![NSFileManager.defaultManager containerURLForSecurityApplicationGroupIdentifier:g]) { v = 0; plog("boot: 本包声明了托管资源包，但签名里没有它要的 app group「%s」（自签 / 重签的包），不走托管资源包", g.UTF8String ?: "未声明"); }
    else v = 1;
    return v;
}

// iOS 26.0–26.3 没有同步的「本地是否可用」查询；只有系统确认（状态查询为已下载 / 下载完成回调）才置这个标记，只在主线程读写。
// iOS 26.0–26.3 has no synchronous "available locally" query; only system confirmation (status query says downloaded / download-finished callback) sets this flag, read and written only on the main thread.
static BOOL g_pack_legacy_available;

static NSString *romfs_locate(char *why, size_t n) {
    g_doc_bad = 0; g_doc_stray = 0;
    NSString *support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)[0];
    NSString *local = [support stringByAppendingPathComponent:@"romfs"];
    // 只测试用：DM_ROMFS_SKIP_LOCAL=1 跳过开发机推进来的 Application Support/romfs，走「文件」里的玩家路径；
    // DM_ROMFS_TEST_CLEAN=<Documents 下的文件夹名>（不许是 romfs）= 删掉 Documents/<名> 后退出（真机试验后清场，只删点名的那一个）。
    // Test only: DM_ROMFS_SKIP_LOCAL=1 skips the Application Support/romfs pushed in from the dev machine and takes the player path through the Files app;
    // DM_ROMFS_TEST_CLEAN=<folder name under Documents> (must not be romfs) = delete Documents/<name> then exit (cleanup after a real-device trial; deletes only the one named).
    { const char *cl = getenv("DM_ROMFS_TEST_CLEAN");
      if (cl && *cl && strcmp(cl, "romfs") && !strchr(cl, '/')) {
          NSString *doc0 = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
          BOOL r = [NSFileManager.defaultManager removeItemAtPath:[doc0 stringByAppendingPathComponent:@(cl)] error:nil];
          plog("boot: DM_ROMFS_TEST_CLEAN：Documents/%s %s，退出", cl, r ? "已删" : "不存在"); exit(0); } }
    if (!getenv("DM_ROMFS_SKIP_LOCAL") && dm_romfs_validate([local stringByAppendingPathComponent:@"data"].fileSystemRepresentation, why, n)) return local;
    // 直装（Ad Hoc）：玩家在「文件」里把资源拖进「我的 iPhone / D2R Mobile」。标准摆法是 Documents/D2R Mobile Resources/data/…（最终资源包：全五幕、仅简中，
    // 过场视频与旧版音乐的零段已从档案里剔掉、旁带 .holes 区段表，宿主读时换算，见 hle_fs.c）。玩家手里的摆法五花八门（网盘多套一层、文件夹改了名、
    // 只把 data 拖进来、把 data 里面的东西散着拖进来）：不依赖中间哪一层叫什么，谁直接放着档案文件谁就是；不要求玩家挪文件夹。
    // Direct install (Ad Hoc): the player drags the resources into "On My iPhone / D2R Mobile" in the Files app. The standard arrangement is
    // Documents/D2R Mobile Resources/data/... (final asset pack: all five acts, Simplified Chinese only;
    // the zero-filled ranges of cutscene videos and legacy music have been stripped from the archives, with .holes extent tables alongside, and the host converts
    // offsets on read, see hle_fs.c). Players' arrangements vary widely (a cloud drive adds an extra level, the folder was renamed,
    // only data was dragged in, the contents of data were dragged in loose): do not depend on what any intermediate level is called, whichever directory
    // directly contains the archive files is the one; do not require the player to move folders.
    NSFileManager *fm = NSFileManager.defaultManager; NSString *docdir = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    // 只测试用：DM_ROMFS_TEST_DOC=<Documents 下的文件夹名> = 把这个文件夹当成「D2R Mobile」来找资源（造「什么都没放」「文件散着放」这类现场，不用清空真的 Documents）
    // Test only: DM_ROMFS_TEST_DOC=<folder name under Documents> = treat this folder as "D2R Mobile" when looking for resources (sets up scenarios such as "nothing
    // put in" and "files placed loose" without emptying the real Documents)
    { const char *td = getenv("DM_ROMFS_TEST_DOC"); if (td && *td && !strchr(td, '/')) { docdir = [docdir stringByAppendingPathComponent:@(td)]; plog("boot: DM_ROMFS_TEST_DOC：按 Documents/%s 找资源", td); } }
    NSArray<NSString *> *(^subdirs)(NSString *) = ^NSArray<NSString *> *(NSString *d) {   // 子目录（标准名字排前，其余按名字序）；隐藏的、存档目录、App 自己建的 temp / Inbox 不看 | Subdirectories (standard names first, the rest in name order); hidden ones, the save directory, and the App's own temp / Inbox are not looked at
        NSMutableArray *pre = [NSMutableArray array], *rest = [NSMutableArray array];
        for (NSString *nm in [[fm contentsOfDirectoryAtPath:d error:nil] sortedArrayUsingSelector:@selector(compare:)] ?: @[]) { BOOL dir = NO;
            if ([nm hasPrefix:@"."] || [nm.lowercaseString containsString:@"save"] || [nm isEqualToString:@"temp"] || [nm isEqualToString:@"Inbox"] || ![fm fileExistsAtPath:[d stringByAppendingPathComponent:nm] isDirectory:&dir] || !dir) continue;
            if (astc_is_pack_dir([d stringByAppendingPathComponent:nm])) {   // 老机型补充包的文件夹（data 里只有它那一个档）不是游戏资源；但玩家把补充包的档和游戏档放进了同一个 data 时，这个文件夹两样都是，不能跳过 | The folder of the Legacy Device Pack (its data holds only that one archive) is not game resources; but when the player has put the pack's archive and the game archives into the same data folder, this folder is both and must not be skipped
                NSString *pk = astc_info()[@"name"] ?: @"00-00000008.data"; int other = 0;
                for (NSString *f in [fm contentsOfDirectoryAtPath:[[d stringByAppendingPathComponent:nm] stringByAppendingPathComponent:@"data"] error:nil] ?: @[]) other += [f hasSuffix:@".data"] && ![f isEqualToString:pk];
                if (!other) continue; }
            [([nm isEqualToString:@"D2R Mobile Resources"] || [nm isEqualToString:@"romfs"]) ? pre : rest addObject:[d stringByAppendingPathComponent:nm]]; }
        return [pre arrayByAddingObjectsFromArray:rest]; };
    // 候选 = 直接放着档案文件的目录，由浅到深（同一层标准名字在前）。第一个校验通过的就用；都不通过时报档案最多的那个（玩家正在拷的那份）
    // Candidates = directories that directly contain archive files, from shallow to deep (standard names first within a level). The first one that passes
    // validation is used; if none passes, report the one with the most archives (the copy the player is in the middle of copying)
    int (^arcs)(NSString *) = ^int(NSString *d) { int k = 0; for (NSString *f in [fm contentsOfDirectoryAtPath:d error:nil] ?: @[]) k += [f hasSuffix:@".data"]; return k; };
    NSArray<NSString *> *top = subdirs(docdir); NSMutableArray<NSString *> *cand = [NSMutableArray array], *level = [NSMutableArray arrayWithObject:docdir];
    for (int depth = 0; depth <= 4 && level.count; depth++) { NSMutableArray<NSString *> *next = [NSMutableArray array];
        for (NSString *d in level) { if (arcs(d)) [cand addObject:d]; [next addObjectsFromArray:depth ? subdirs(d) : top]; }
        level = next; }
    NSString *arcdir = nil; char w2[512] = "";
    for (NSString *d in cand) if (dm_romfs_validate(d.fileSystemRepresentation, w2, sizeof w2)) { arcdir = d; break; }
    if (arcdir) {
        // 游戏按 <资源根>/data/<档案> 读：目录本来就叫 data → 资源根是它上一层；不叫 data（散着放的）→ 在它里面建 data、把档案与区段表挪进去（同一个盘上改名，不拷贝）
        // The game reads <resource root>/data/<archive>: if the directory is already named data → the resource root is its parent; if not named data (files
        // placed loose) → create data inside it and move the archives and extent tables in (a rename on the same volume, no copy)
        NSString *root = arcdir.stringByDeletingLastPathComponent;
        if (![arcdir.lastPathComponent isEqualToString:@"data"]) {
            NSString *data = [arcdir stringByAppendingPathComponent:@"data"]; NSError *e = nil; int moved = 0, all = 0; root = arcdir;
            BOOL made = [fm createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:&e];
            for (NSString *f in made ? [fm contentsOfDirectoryAtPath:arcdir error:nil] : @[]) { if (!([f hasSuffix:@".data"] || [f hasSuffix:@".data.holes"])) continue;
                NSError *me = nil; all++; if ([fm moveItemAtPath:[arcdir stringByAppendingPathComponent:f] toPath:[data stringByAppendingPathComponent:f] error:&me]) moved++; else e = me; }
            plog("boot: 档案文件散着放在 %s：挪进它下面的 data 文件夹 %d / %d 个%s%s", arcdir.fileSystemRepresentation, moved, all, e ? "；没成：" : "", e ? e.localizedDescription.UTF8String : "");
            if (!made || moved != all) { snprintf(why, n, dm_ui("dmDlTidyFail"), e.localizedDescription.UTF8String ?: dm_ui("dmDlUnknown")); g_doc_bad = 1; return nil; }
        }
        strlcpy(why, w2, n); plog("boot: 「D2R Mobile」里的资源：%s", root.fileSystemRepresentation); return root;
    }
    if (cand.count) {
        // 不完整：把缺的、没拷完的全列出来（不是只报第一个）
        // Incomplete: list everything that is missing or not fully copied (not just the first one)
        NSString *best = cand[0]; for (NSString *d in cand) if (arcs(d) > arcs(best)) best = d;
        NSString *holder = [best.lastPathComponent isEqualToString:@"data"] ? best.stringByDeletingLastPathComponent : best;
        char rep[1500]; dm_romfs_report(best.fileSystemRepresentation, rep, sizeof rep); NSString *where = [holder isEqualToString:docdir] ? T("dmDlAppFolder") : [NSString stringWithFormat:T("dmDlQuoted"), holder.lastPathComponent];
        snprintf(why, n, dm_ui("dmDlIncomplete"), where.UTF8String, rep);
        plog("boot: 资源不完整（%s）：%s", best.fileSystemRepresentation, rep);
        g_doc_bad = 1; return nil;   // 不再往下试托管资源包：它的校验结果会盖掉这条原因（界面只剩托管下载的报错） | Do not go on to try the hosted asset pack: its validation result would overwrite this reason (the screen would only show the hosted download's error)
    }
    // ③ 托管资源包（TestFlight 包的资源在这里）。取路径的接口不管有没有资源包都返回一个路径（系统文档如此，直装包也一样），不能拿它当「有」：
    //    以系统的「这个资源包本地可用」为准，且磁盘上真有那个目录，才去校验；校验不过的原因写成玩家看得懂的清单（同「D2R Mobile」里不完整时那份），
    //    路径与目录里实际有什么记进日志。没到这一步的，不把它的校验结果当原因。
    // ③ Managed asset pack (the resources of the TestFlight build live here). The path API returns a path whether or not an asset pack exists (per the
    // system documentation, and the same for direct-install builds), so it cannot be taken as "present":
    //    go by the system's "this asset pack is locally available", and validate only if that directory really exists on disk; reasons for a failed
    //    validation are written as a list the player can understand (the same list as for an incomplete "D2R Mobile"),
    //    and the path and what the directory actually contains are logged. If this step was not reached, its validation result is not used as the reason.
    g_pack_wait = 0;
    NSString *pack = nil; BOOL avail = NO; const char *tp = getenv("DM_ROMFS_TEST_PACK");   // 只测试用：DM_ROMFS_TEST_PACK=<Documents 下的文件夹名> 当成「本地可用」的托管资源包 | Test only: DM_ROMFS_TEST_PACK=<folder name under Documents> is treated as a "locally available" managed asset pack
    if (tp && *tp && !strchr(tp, '/')) { pack = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0] stringByAppendingPathComponent:@(tp)]; avail = YES; }
    else if (!ba_configured()) plog("boot: 本包没有配置托管资源包，不问系统");
    else if (@available(iOS 26.4, *)) {
        NSError *e = nil; pack = [BAAssetPackManager.sharedManager URLForPath:@"romfs" error:&e].path; avail = [BAAssetPackManager.sharedManager assetPackIsAvailableLocallyWithIdentifier:@"romfs"];
        if (e) plog("boot: 托管资源包取路径报错：%s", e.localizedDescription.UTF8String);
    } else if (@available(iOS 26.0, *)) {
        NSError *e = nil; pack = [BAAssetPackManager.sharedManager URLForPath:@"romfs" error:&e].path; avail = g_pack_legacy_available;
        if (e) plog("boot: 托管资源包路径取不到（iOS 26.4 以前的接口）：%s", e.localizedDescription.UTF8String);
    }
    if (pack) {
        NSArray *in = [fm contentsOfDirectoryAtPath:[pack stringByAppendingPathComponent:@"data"] error:nil]; BOOL there = [fm fileExistsAtPath:pack];
        plog("boot: 托管资源包：系统说本地%s；路径 %s（磁盘上%s，data 里 %d 个文件）", avail ? "可用" : "不可用", pack.fileSystemRepresentation, there ? "有" : "没有", (int)in.count);
        if (avail && there) {
            char w3[512];
            NSString *pdir = [pack stringByAppendingPathComponent:@"data"]; const char *pd = pdir.fileSystemRepresentation;
            if (dm_romfs_validate(pd, w3, sizeof w3)) { strlcpy(why, w3, n); return pack; }
            char rep[1500]; dm_romfs_report(pd, rep, sizeof rep);
            snprintf(why, n, dm_ui("dmDlPackBad"), rep);
            plog("boot: 托管资源包校验不过（%s）｜目录里有：%s", w3, [in componentsJoinedByString:@" "].UTF8String);
            return nil;
        }
        if (!avail && there) g_pack_wait = 1;   // 目录已经在、系统还没说可用：还在下 / 还在落盘，等系统的完成通知 | The directory already exists but the system has not said it is available: still downloading / still being written to disk, wait for the system's completion notification
    }
    // ④ 都没有。「D2R Mobile」里有文件夹（但都不是资源）：why = 现有哪些文件夹，置 g_doc_stray；一个文件夹都没有：还没放资源
    // ④ None of them. "D2R Mobile" contains folders (none of which are resources): why = which folders are there, set g_doc_stray; no folder at all: resources have
    // not been put in yet
    if (top.count) {
        NSMutableArray *names = [NSMutableArray array]; for (NSString *d in top) if (names.count < 6) [names addObject:d.lastPathComponent];
        snprintf(why, n, dm_ui("dmDlNoData"), [names componentsJoinedByString:T("dmDlNameSep")].UTF8String, top.count > 6 ? dm_ui("dmDlEtc") : "");
        plog("boot: %s", why); g_doc_stray = 1;
    } else snprintf(why, n, "%s", dm_ui("dmDlNone"));
    return nil;
}
static int g_need_assets;   // 启动时资源包还没到：先不起宿主，显示下载界面（AssetVC），到了再起 | Asset pack not here yet at launch: do not start the host yet, show the download screen (AssetVC), start once it arrives
// 显卡不支持 BC 压缩格式（A16 及以前芯片，苹果 GPU 族 Apple8 及以下）：游戏素材几乎全是 BC，建第一张就被 Metal 校验 abort（线上缺陷：iPhone 14 Pro Max 启动黑屏 1 秒闪退）。
// 不起宿主，弹框说明后退出。测试开关 DM_TEST_NO_BC=1 当成不支持
// The GPU does not support BC compressed formats (A16 and earlier chips, Apple GPU family Apple8 and below): nearly all game assets are BC, and creating the first one gets aborted by Metal validation (defect in released builds: iPhone 14 Pro Max launches to a black screen and crashes after 1 second).
// Do not start the host; show an alert explaining, then exit. The test switch DM_TEST_NO_BC=1 treats the device as unsupported
static int g_no_bc;
static char g_astc_why[512];   // 没用上补充包的原因（弹框里给玩家看） | Why the Legacy Device Pack was not used (shown to the player in the alert)
static void astc_handoff(UIWindow *w);   // 补充包到位：换成资源界面（AssetVC，资源齐了就地进游戏），定义在 AssetVC 之后 | Legacy Device Pack in place: switch to the asset screen (AssetVC; enters the game in place once assets are complete); defined after AssetVC
static BOOL res_dl_configured(void) {   // 游戏资源的自动下载地址配了没有：测试环境变量，或包内 dm/asset_base.txt / dm/asset_pointers.txt 有非注释行 | Whether an automatic download address for the game resources is configured: the test environment variable, or a non-comment line in the bundled dm/asset_base.txt / dm/asset_pointers.txt
    if (getenv("DM_TEST_NO_DL_BASE")) return NO;   // 只测试用：当成没配下载地址的包（照公开仓库自己出的包就是这样） | Test only: treat this as a build with no download address configured (which is what a build made by yourself from the public repository is)
    const char *e = getenv("DM_ASSET_BASE"); if (e && *e) return YES;
    for (NSString *fn in @[@"asset_base.txt", @"asset_pointers.txt"]) {
        NSString *t = [NSString stringWithContentsOfFile:[[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:fn] encoding:NSUTF8StringEncoding error:nil];
        for (NSString *l in [t componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) { NSString *v = [l stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; if (v.length && ![v hasPrefix:@"#"]) return YES; } }
    return NO;
}
static NSString *no_assets_text(void) { return T(res_dl_configured() ? "dmDlNoAssets" : "dmDlCopyIn"); }   // 「还没有游戏资源」那一页：配了下载地址的包指向内测页；没配的只讲「拷进来」 | The "no game resources yet" page: a build with a download address configured points to the beta page; one without only says "copy them in"
static BOOL astc_dl_configured(void) {   // 自动下载地址配了没有：测试环境变量或包内 dm/astc_pack_base.txt 有非注释行 | Whether an auto-download address is configured: the test environment variable, or dm/astc_pack_base.txt in the bundle has a non-comment line
    const char *e = getenv("DM_ASTC_PACK_BASE"); if (e && *e) return YES;
    NSString *t = [NSString stringWithContentsOfFile:[[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm"] stringByAppendingPathComponent:@"astc_pack_base.txt"] encoding:NSUTF8StringEncoding error:nil];
    for (NSString *l in [t componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) { NSString *v = [l stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; if (v.length && ![v hasPrefix:@"#"]) return YES; }
    return NO;
}
// 安装包自检（只查从发布页的壳做出来的包：壳带着 dm/shell_manifest.txt；Xcode 出的包没有它，不查）。壳清单里每一项都要由 d2rm_pack.py 做进来，
// 缺 = 装的是没做过第一步的壳（或拼装的包）——起宿主会在装载器读 dm/main.fixups.tsv 处退出（线上闪退）。另比产物代号：打包记录 dm/pack_info.txt 与壳的
// dm/shell_info.txt（两份都在才比，旧版打包工具不写打包记录）。不过就不起宿主，弹框说明缺什么、该做哪一步
// Install package self-check (only for packages made from the shell on the releases page: the shell carries dm/shell_manifest.txt; packages built with Xcode do not have it and are not checked). Every item in the shell manifest must have been built in by d2rm_pack.py;
// missing = the shell was installed without doing step 1 (or the package was assembled), and starting the host would exit where the loader reads dm/main.fixups.tsv (a crash for players). The product generation is also compared: the pack record dm/pack_info.txt against the shell's
// dm/shell_info.txt (only when both exist; older pack tools did not write a pack record). If the check fails, do not start the host; show a message saying what is missing and which step to do
static NSString *g_block_title, *g_block_msg, *g_block_why;   // 非空 = 这台设备 / 这个安装包起不了游戏：不起宿主，场景里弹框（BlockVC）说明；why 进日志 | Non-empty = this device / this install package cannot start the game: do not start the host, show a message in the scene (BlockVC); why goes to the log
static NSString *g_romfs; static char g_romfs_why[2048];   // 起宿主前那次资源校验的结果（host_start 写，boot_prepare 用） | Result of the resource check before starting the host (written by host_start, used by boot_prepare)
static UIWindow *g_window;
static NSString *kv_file(NSString *path, NSString *key) {
    NSString *t = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    for (NSString *l in [t componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) if ([l hasPrefix:[key stringByAppendingString:@"="]]) return [l substringFromIndex:key.length + 1];
    return nil;
}
static NSString *pack_check(void) {
    NSString *app = NSBundle.mainBundle.resourcePath; NSFileManager *fm = NSFileManager.defaultManager;
    // 只测试用：DM_TEST_PACK_MANIFEST=<文件，相对 Documents> 换一份壳清单、DM_TEST_PACK_INFO=<文件> 换一份打包记录（Xcode 包里没有壳信息时，同一文件的 SHELL_PRODUCT= 充当壳的代号；造「缺项」「代号不一致」现场，不动包内文件、不破签名）
    // Test only: DM_TEST_PACK_MANIFEST=<file, relative to Documents> substitutes the shell manifest and DM_TEST_PACK_INFO=<file> substitutes the pack record (in an Xcode package, which has no shell info, SHELL_PRODUCT= in the same file stands in for the shell's generation), to create "missing item" and "generation mismatch" cases without touching files in the package or breaking the signature
    NSString *(^tpath)(const char *) = ^NSString *(const char *e) { if (!e) return nil; NSString *p = @(e); return [p hasPrefix:@"/"] ? p :   // 相对路径按 Documents 解析（沙盒读不到容器外；真机容器路径每次变） | Relative paths are resolved against Documents (the sandbox cannot read outside the container, and the device container path changes every time)
        [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0] stringByAppendingPathComponent:p]; };
    NSString *tm = tpath(getenv("DM_TEST_PACK_MANIFEST")), *ti = tpath(getenv("DM_TEST_PACK_INFO"));
    NSString *man = [NSString stringWithContentsOfFile:tm ?: [app stringByAppendingPathComponent:@"dm/shell_manifest.txt"] encoding:NSUTF8StringEncoding error:nil];
    if (!man) { plog("boot: 安装包自检：没有壳清单（Xcode 出的包），不查"); return nil; }
    NSMutableArray<NSString *> *miss = [NSMutableArray array]; int total = 0;
    for (NSString *l in [man componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSString *m = [l stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; if (!m.length) continue; total++;
        BOOL dir = NO; if (![fm fileExistsAtPath:[app stringByAppendingPathComponent:m] isDirectory:&dir] || dir != [m hasSuffix:@"/"]) [miss addObject:m]; }
    if (miss.count) {
        plog("boot: 🔴 安装包自检：壳清单 %d 项缺 %lu 项：%s", total, (unsigned long)miss.count, [miss componentsJoinedByString:@", "].UTF8String);
        return [NSString stringWithFormat:T("dmPackBadMiss"), (int)miss.count, [[miss subarrayWithRange:NSMakeRange(0, MIN(miss.count, (NSUInteger)3))] componentsJoinedByString:@", "]]; }
    NSString *sp = kv_file([app stringByAppendingPathComponent:@"dm/shell_info.txt"], @"PRODUCT") ?: (ti ? kv_file(ti, @"SHELL_PRODUCT") : nil), *pp = kv_file(ti ?: [app stringByAppendingPathComponent:@"dm/pack_info.txt"], @"PRODUCT");
    if (sp && pp && ![sp isEqualToString:pp]) { plog("boot: 🔴 安装包自检：壳的产物代号 %s，打包记录 %s，不一致", sp.UTF8String, pp.UTF8String); return [NSString stringWithFormat:T("dmPackBadProd"), sp, pp]; }
    plog("boot: 安装包自检通过：壳清单 %d 项齐全；产物代号 壳 %s / 打包记录 %s", total, sp.UTF8String ?: "无", pp.UTF8String ?: "无（旧版打包工具）");
    return nil;
}
@interface BlockVC : UIViewController @end
@implementation BlockVC
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated]; static int shown; if (shown++) return;
    UIAlertController *a = [UIAlertController alertControllerWithTitle:g_block_title message:g_block_msg preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:T("dmOldQuit") style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { plog("boot: 玩家点了「退出」（%s）", g_block_why.UTF8String); exit(0); }]];
    [self presentViewController:a animated:YES completion:nil];
    plog("boot: 已弹框（%s）｜屏上标题：%s｜正文：%s", g_block_why.UTF8String, a.title.UTF8String, [a.message stringByReplacingOccurrencesOfString:@"\n" withString:@" ⏎ "].UTF8String);
}
@end
@interface NoBcVC : UIViewController @property (strong) UILabel *label; @property (strong) UIProgressView *bar; @property (assign) BOOL shown; @end
@implementation NoBcVC
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor = UIColor.blackColor; CGRect b = self.view.bounds;
    self.label = [[UILabel alloc] initWithFrame:CGRectMake(40, b.size.height / 2 - 90, b.size.width - 80, 100)]; self.label.textColor = [UIColor colorWithRed:0.85 green:0.72 blue:0.45 alpha:1];
    self.label.textAlignment = NSTextAlignmentCenter; self.label.numberOfLines = 4; self.label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
    self.bar = [[UIProgressView alloc] initWithFrame:CGRectMake(b.size.width * 0.2, b.size.height / 2 + 24, b.size.width * 0.6, 4)]; self.bar.hidden = YES;
    self.bar.progressTintColor = [UIColor colorWithRed:0.80 green:0.25 blue:0.10 alpha:1]; self.bar.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
    [self.view addSubview:self.label]; [self.view addSubview:self.bar];
}
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; if (!self.shown) { self.shown = YES; [self ask]; } }
- (void)ask {
    NSDictionary *info = astc_info(); UIAlertController *a;
    if (!info) a = [UIAlertController alertControllerWithTitle:T("dmOldUnsupTitle") message:T("dmOldUnsupMsg") preferredStyle:UIAlertControllerStyleAlert];
    else {
        NSString *m = [NSString stringWithFormat:T("dmOldNeedMsg"),
                       [info[@"size"] longLongValue] / 1e9, !g_astc_none && g_astc_why[0] ? [NSString stringWithFormat:T("dmOldWhySuffix"), g_astc_why] : @""];
        a = [UIAlertController alertControllerWithTitle:T("dmOldNeedTitle") message:m preferredStyle:UIAlertControllerStyleAlert];
        if (astc_dl_configured()) [a addAction:[UIAlertAction actionWithTitle:T("dmOldDl") style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { [self download]; }]];
        [a addAction:[UIAlertAction actionWithTitle:T("dmOldCopied") style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { [self recheck]; }]];
    }
    [a addAction:[UIAlertAction actionWithTitle:T("dmOldQuit") style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { plog("boot: 玩家点了「退出」（本机型不支持 BC 格式，补充包未就绪）"); exit(0); }]];
    [self presentViewController:a animated:YES completion:nil];
    plog("boot: 已弹「%s」｜屏上标题：%s｜正文：%s｜按钮：%s", info ? "需要老机型补充包" : "暂不支持本机型", a.title.UTF8String, [a.message stringByReplacingOccurrencesOfString:@"\n" withString:@" ⏎ "].UTF8String, [[a.actions valueForKey:@"title"] componentsJoinedByString:@" / "].UTF8String);   // 屏上原文进日志：这一层系统弹框包内截图够不到，中英对不对靠这一行判 | The on-screen text goes into the log: in-package screenshots cannot reach this layer of system dialogs, so whether the Chinese / English is right is judged from this line
}
- (void)recheck {
    g_astc_why[0] = 0; NSString *d = astc_pack_locate(g_astc_why, sizeof g_astc_why);
    if (d) { g_astc_dir = d; plog("boot: 补充包到位 %s，接着检查游戏资源", d.UTF8String); astc_handoff(self.view.window); return; }
    plog("boot: 补充包仍不可用：%s", g_astc_why); [self ask];
}
- (void)download {
    self.label.text = T("dmOldConnecting"); self.bar.hidden = NO; self.bar.progress = 0; UIApplication.sharedApplication.idleTimerDisabled = YES;
    [DmAssetDL.astcPack prepare:^(NSString *err, int64_t remain, int64_t total, BOOL cellular) {
        if (err) { plog("boot: 补充包下载走不了：%s", err.UTF8String); g_astc_none = 0; snprintf(g_astc_why, sizeof g_astc_why, dm_ui("dmOldDlFail"), err.UTF8String); self.label.text = @""; self.bar.hidden = YES; [self ask]; return; }
        [DmAssetDL.astcPack run:^(int64_t done, int64_t tot, NSString *note) {
            self.label.text = [NSString stringWithFormat:T("dmOldProgress"), done / 1e9, tot / 1e9, tot ? 100.0 * done / tot : 0, note ? [@"\n" stringByAppendingString:note] : @""];
            self.bar.progress = tot ? (float)done / tot : 0;
        } done:^(NSString *e2) {
            if (e2) { g_astc_none = 0; snprintf(g_astc_why, sizeof g_astc_why, dm_ui("dmOldDlFail"), e2.UTF8String); self.label.text = @""; self.bar.hidden = YES; [self ask]; return; }
            [self recheck];
        }];
    }];
}
@end
// boot 前提（main() 开头、UIApplicationMain 前做）：存档替换 / 目录 / 环境变量 / cwd——宿主提前装载（dm_host_preload_start）要读这些
// boot prerequisites (done at the start of main(), before UIApplicationMain): save replacement / directories / environment variables / cwd -- the host's early load (dm_host_preload_start) needs to read these
static void boot_prepare(void) {
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"dm"];
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    NSString *support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)[0];
    NSString *romfs = [support stringByAppendingPathComponent:@"romfs"];
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:romfs withIntermediateDirectories:YES attributes:nil error:nil];
    NSURL *romURL = [NSURL fileURLWithPath:romfs]; NSNumber *excluded = @YES; [romURL setResourceValue:excluded forKey:NSURLIsExcludedFromBackupKey error:nil];
    const char *romwhy = g_romfs_why;
    NSString *found = g_romfs;   // host_start 起宿主前刚校验过（唯一一次）；没过的不会走到这里 | host_start checked right before starting the host (the only time); failures never reach here
    if (!found) {
        plog("boot: 🔴 RomFS 没经过起宿主前的校验（不应发生）：%s", romwhy);
        plog("boot: 请推送原版 RomFS：xcrun devicectl device copy to --device <设备> --domain-type appDataContainer --domain-identifier <包名> --source $DM_ROOT/data/switch/3.3.94012/Update/RomFS --destination 'Library/Application Support/romfs'");
        abort();
    }
    romfs = found;
    plog("boot: RomFS 校验通过：%s，挂载 %s", romwhy, romfs.fileSystemRepresentation);
    plog("boot: 文件描述符软上限 %d → %d", g_nofile_was, g_nofile_now);
    NSString *oldrom = [doc stringByAppendingPathComponent:@"romfs"]; if ([fm fileExistsAtPath:oldrom] && ![romfs isEqualToString:oldrom]) plog("boot: 提示：Documents/romfs 没在用（资源取自 %s），可删除", romfs.fileSystemRepresentation);
    if (getenv("DM_SAVE_FRESH")) {   // 测试用（冒烟 / frame_cmp / 浸泡）：本局存档 = App 资源里标准存档的新副本（=1：save_ref；=名：save_ref_名，如 warlock），放在 Documents/save.test 并把存档目录指过去 | For testing (smoke test / frame_cmp / soak): this run's save = a fresh copy of the standard save in the App resources (=1: save_ref; =name: save_ref_name, e.g. warlock), placed in Documents/save.test with the save directory pointed at it
        // 不碰 Documents/save（玩家 / 走查留下的真实存档）：早先是直接换掉 Documents/save，任何人跑冒烟都会把别人上一局建的角色覆盖掉
        // Do not touch Documents/save (the real save left by the player / walkthrough): earlier this replaced Documents/save directly, so anyone running the smoke test overwrote the character someone else created in the previous run
        const char *sf = getenv("DM_SAVE_FRESH");
        NSString *rn = strcmp(sf, "1") ? [@"save_ref_" stringByAppendingString:@(sf)] : @"save_ref";
        NSString *sv = [doc stringByAppendingPathComponent:@"save.test"], *ref = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:rn];
        setenv("DM_SAVE", sv.fileSystemRepresentation, 1);
        NSError *e = nil;
        // save 下各项改成可写（目录 0755、文件 0644）：旧存档可能来自只读源（devicectl 直推的 save_ref 目录 0555 → 子项删不掉），
        // 新副本会沿用 save_ref 的只读权限（源文件 0444）→ 游戏写存档失败
        // Make everything under save writable (directories 0755, files 0644): an old save may come from a read-only source (a save_ref directory pushed by devicectl is 0555 → children cannot be deleted),
        // and a new copy inherits save_ref's read-only permissions (source files 0444) → the game fails to write the save
        void (^writable)(void) = ^{
            [fm setAttributes:@{NSFilePosixPermissions: @0755} ofItemAtPath:sv error:nil];
            for (NSString *rel in [fm enumeratorAtPath:sv]) {
                NSString *pth = [sv stringByAppendingPathComponent:rel]; BOOL dir = NO; [fm fileExistsAtPath:pth isDirectory:&dir];
                [fm setAttributes:@{NSFilePosixPermissions: dir ? @0755 : @0644} ofItemAtPath:pth error:nil];
            }
        };
        if ([fm fileExistsAtPath:sv]) writable();
        // 全新存档 = 新设备：iCloud 同步状态与同步备份（save.test.cloudstate / .cloudbak）一并清掉，否则旧局的同步记录还在，同步按「这台的存档被换了」处理（cfc：首装那条路没走到）
        // Brand-new save = new device: also clear the iCloud sync state and sync backup (save.test.cloudstate / .cloudbak); otherwise the old run's sync record is still there and sync treats it as "this device's save was replaced" (cfc: the first-install path was not reached)
        for (NSString *x in @[@".cloudstate", @".cloudbak"]) { NSString *p = [sv stringByAppendingString:x]; if ([fm fileExistsAtPath:p]) [fm removeItemAtPath:p error:nil]; }
        if ([fm fileExistsAtPath:sv] && ![fm removeItemAtPath:sv error:&e]) plog("boot: 删旧存档失败 %s", e.localizedDescription.UTF8String);
        else if (!strcmp(sf, "empty")) { [fm createDirectoryAtPath:sv withIntermediateDirectories:YES attributes:nil error:nil]; plog("boot: 本局从空存档起（Documents/save.test 已清空 = 首次启动；Documents/save 未动）"); }   // =empty：无存档（冒烟的触摸玩家路径：建角进局） | =empty: no save (the touch player path of the smoke test: create a character and enter a run)
        else if (![fm copyItemAtPath:ref toPath:sv error:&e]) plog("boot: 标准存档副本失败 %s", e.localizedDescription.UTF8String);
        else if (writable(), access([sv stringByAppendingPathComponent:@"SharedStashSoftCoreV2.d2i"].fileSystemRepresentation, W_OK) != 0) plog("boot: 🔴 存档副本仍不可写");
        else plog("boot: 本局用标准存档 %s 新副本（Documents/save.test；Documents/save 未动）", rn.UTF8String);
    } else if (getenv("DM_SAVE_TEST")) {   // 测试用（冒烟的玩家路径那一步）：沿用上一局留下的 Documents/save.test，不重拷、不碰 Documents/save | For testing (the player-path step of the smoke test): reuse Documents/save.test left by the previous run; do not re-copy, do not touch Documents/save
        NSString *sv = [doc stringByAppendingPathComponent:@"save.test"];
        setenv("DM_SAVE", sv.fileSystemRepresentation, 1); plog("boot: 本局沿用测试存档 Documents/save.test（Documents/save 未动）");
    }
    for (NSString *d in @[@"save", @"temp"])
        [fm createDirectoryAtPath:[doc stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];
    { void dm_custom_init(const char *dir); dm_custom_init(doc.fileSystemRepresentation); }   // 「自定义」设置文件在 Documents（覆盖层选择等启动期的读也要用，存档还没挂） | The "Custom" settings file lives in Documents (boot-time reads such as overlay selection need it too, and the save is not mounted yet)
    setenv("DM_ROMFS", romfs.fileSystemRepresentation, 0);
    NSString *bundledOverlay = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:g_astc_dir ? @"romfs_overlay_astc" : @"romfs_overlay"];   // 老机型补充包：贴图指向补充包的那套 | Legacy Device Pack: the set whose textures point to the Legacy Device Pack
    {   // 走覆盖层的自定义开关（物品注释…）：构建时出了全部组合 romfs_overlay_variants/<键+键…>（gen_romfs_overlay.py），按已生效的开关挑键集合正好相等的那份；全关用 romfs_overlay。重启后生效 | Custom switches that go through the overlay (item annotations...): the build produces every combination as romfs_overlay_variants/<key+key...> (gen_romfs_overlay.py); pick the one whose key set exactly equals the switches in effect; with all off use romfs_overlay. Takes effect after restart
        int dm_custom_get(const char *key);
        NSString *vdir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:g_astc_dir ? @"romfs_overlay_astc_variants" : @"romfs_overlay_variants"];
        NSArray<NSString *> *vs = [fm contentsOfDirectoryAtPath:vdir error:nil] ?: @[];
        NSMutableSet<NSString *> *all = [NSMutableSet set], *on = [NSMutableSet set];
        for (NSString *v in vs) [all addObjectsFromArray:[v componentsSeparatedByString:@"+"]];
        for (NSString *k in all) if (dm_custom_get(k.UTF8String)) [on addObject:k];
        for (NSString *v in vs) if ([[NSSet setWithArray:[v componentsSeparatedByString:@"+"]] isEqualToSet:on]) { bundledOverlay = [vdir stringByAppendingPathComponent:v]; break; }
        if (on.count && [bundledOverlay.lastPathComponent isEqualToString:@"romfs_overlay"]) plog("boot: 🔴 开关组合 %s 没有对应的覆盖层，按全关启动", [on.allObjects componentsJoinedByString:@"+"].UTF8String);
    }
    if (g_astc_dir) bundledOverlay = [NSString stringWithFormat:@"%@:%@", bundledOverlay, g_astc_dir];   // 第二层：补充包（data/<档名>，覆盖层索引指向它） | Second layer: the Legacy Device Pack (data/<archive name>, which the overlay index points to)
    if (g_astc_dir || [fm fileExistsAtPath:bundledOverlay]) { setenv("DM_ROMFS_OVERLAY", bundledOverlay.fileSystemRepresentation, 1); plog("boot: RomFS 覆盖层 %s", bundledOverlay.fileSystemRepresentation); }
    else plog("boot: 🔴 App 内缺 romfs_overlay（底部 UI v4），按原版 HUD 启动");
    setenv("DM_SAVE", [doc stringByAppendingPathComponent:@"save"].fileSystemRepresentation, 0);
    setenv("DM_TEMP", [doc stringByAppendingPathComponent:@"temp"].fileSystemRepresentation, 0);
    setenv("DM_NVN_SHMAP", [res stringByAppendingPathComponent:@"shader_map.tsv"].fileSystemRepresentation, 0);
    setenv("DM_MSL_DIR", [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"msl"].fileSystemRepresentation, 0);
    // 工作目录设成 Documents：环境变量里的相对路径（DM_TICKS / DM_C2S_REPLAY / DM_C2S_LOG / DM_NVN_SHOT 的名字部分等）都落在这里（容器路径启动前不可知）
    // Set the working directory to Documents: relative paths in environment variables (DM_TICKS / DM_C2S_REPLAY / DM_C2S_LOG / the name part of DM_NVN_SHOT, etc.) all land here (the container path is not known before launch)
    if (chdir(doc.fileSystemRepresentation)) plog("boot: 🔴 chdir Documents 失败 %s", strerror(errno));
    g_boot_argv[0] = "dmhost"; g_boot_argv[1] = strdup(res.fileSystemRepresentation); g_boot_argv[2] = "main"; g_boot_argv[3] = "sdk";

}
static void boot_begin(void) {
    double tp0 = since_start(); boot_prepare();
    plog("启动计时 boot_prepare %.1f → %.1f ms（开日志 %.1f ms 起）", tp0, since_start(), g_T[2]);
    // 宿主装载（读 NSO / 重定位 / 强制接管 / 预取起跑）提前在后台跑，与 UIKit 启动链并行；dm_main 进来时等它完成。DM_NO_PRELOAD=1 关（对照用）
    // Host loading (read NSO / relocate / forced takeover / prefetch kickoff) runs early in the background, in parallel with the UIKit launch chain; dm_main waits for it to finish on entry. DM_NO_PRELOAD=1 turns it off (for comparison)
    { void dm_sharelog_boot(void); dm_sharelog_boot(); }   // 暂停菜单「分享诊断日志」的回调登记给宿主（dm_sharelog.m） | Register the callback for the pause menu's 「分享诊断日志」(Share Diagnostics) with the host (dm_sharelog.m)
    { void dm_update_boot(void); dm_update_boot(); }   // 构建号交给宿主（选角页左下角）、登记新版本提示的两个回调（dm_update.m） | Hand the build number to the host (bottom-left of the character select page) and register the two callbacks for the new-version notice (dm_update.m)
    if (!getenv("DM_NO_PRELOAD")) dm_host_preload_start(g_boot_argc, g_boot_argv);
    else plog("boot: DM_NO_PRELOAD——宿主装载不提前（对照）");
}
static void *boot_thread(void *arg) {
    (void)arg;
    g_T[5] = since_start();
    char **argv = g_boot_argv;
    { uint64_t r = front_game_res(1), r0 = front_game_res(0);   // 全屏验收④：与游戏建的交换链尺寸（WindowInitialize 行）对照 | Full-screen acceptance ④: compare with the swapchain size the game creates (the WindowInitialize line)
      plog("boot: 前端给游戏的输出分辨率 主机 %llux%llu / 掌机 %llux%llu", r & 0xffffffff, r >> 32, r0 & 0xffffffff, r0 >> 32); }
    g_T[6] = since_start();
    plog("启动计时 T5 boot_thread 入口 %.1f ms · T6 dm_main 前 %.1f ms", g_T[5], g_T[6]);
    { void dm_cloud_boot(void); dm_cloud_boot(); }   // iCloud 同步：挂存档之前把云端更新的角色 / 共享仓库换进来（最多等 5 秒；cloud_ios.m） | iCloud sync: before mounting saves, swap in the characters / shared stash that are newer in the cloud (wait at most 5 seconds; cloud_ios.m)
    plog("boot: dm_main %s", argv[1]);
    int rc = dm_main(g_boot_argc, argv);
    plog("boot: dm_main 返回 %d", rc);
    plog("DONE");
    return NULL;
}
static void run_boot(void) {
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64 << 20);
    pthread_t t; pthread_create(&t, &a, boot_thread, NULL);
}
// 宿主只在场景到前台时起（host_start，主线程调）。系统会在没人操作时把 App 在后台拉起来（后台下载会话有结果、托管资源包的后台事件…）：
// 那种进程一出生就在后台，收不到「进后台」事件，暂停引擎 / 挂起 GPU / 冻结游戏时钟都不会生效——所以后台拉起时根本不起宿主，
// 只做系统要的事（接回下载会话，asset_dl.m）；之后玩家点图标，场景到前台，再照常起。
// The host starts only when the scene comes to the foreground (host_start, called on the main thread). The system launches the App in the background
// with nobody using it (a background download session has results, background events of hosted asset packs, ...):
// such a process is born in the background and never receives an "entered background" event, so pausing the engine / suspending the GPU / freezing the
// game clock would never take effect. So on a background launch the host is not started at all,
// and only what the system asks for is done (reattaching the download session, asset_dl.m); later the player taps the icon, the scene comes to the
// foreground, and the host starts as usual.
static int g_host_up;
static void log_promote(void) {   // 场景第一次到前台：这一轮是玩家真的在用，日志转正 | the scene comes to the foreground for the first time: this launch is the player really using the App, so the log is promoted
    static int done; if (done || ![g_mode isEqualToString:@"boot"]) return; done = 1;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    const char *cur = [doc stringByAppendingPathComponent:@"boot.txt"].fileSystemRepresentation, *prev = [doc stringByAppendingPathComponent:@"boot.previous.txt"].fileSystemRepresentation;
    // 上一局的日志只留最近一轮（玩家多半是「重开了一次才想起来报」）。改名失败（目标被占成目录、磁盘满等）就不动它，本轮继续写 boot.launch.txt，不拿丢掉现场换一份干净的新日志
    // Only the most recent previous log is kept (players mostly "remember to report only after reopening once"). If the rename fails (target occupied by a
    // directory, disk full, etc.) it is left alone and this launch keeps writing boot.launch.txt, rather than giving up the evidence in exchange for a clean
    // new log
    if (rename(cur, prev) != 0 && errno != ENOENT) { plog("boot: 上一轮日志没能改名保留（%s），本轮日志留在 boot.launch.txt", strerror(errno)); return; }
    if (rename(g_log_path, cur) != 0) plog("boot: 本轮日志没能改名成 boot.txt（%s），留在 boot.launch.txt", strerror(errno));
}
static void host_start(void) {
    if (g_host_up) return;
    if (getenv("DM_TEST_BG_RELAUNCH")) return;   // 这次启动只为造后台拉起（见 bg_relaunch_test），马上自己退出 | this launch exists only to stage a background relaunch (see bg_relaunch_test) and exits by itself right away
    { NSString *r = romfs_locate(g_romfs_why, sizeof g_romfs_why);   // 资源校验：起宿主前只在这里做一次（boot_prepare 用这次结果）。没过（启动判定之后资源被挪 / 删）就回资源界面，不闪退 | Resource check: done only once, here, before starting the host (boot_prepare uses this result). If it fails (resources moved / deleted after the launch decision), go back to the resources screen instead of crashing
      if (!r) { plog("boot: 起宿主前资源校验没过（%s），回到资源界面", g_romfs_why); if (g_window) astc_handoff(g_window); return; }
      g_romfs = r; }
#ifdef DM_SHELL
    // 壳：着色器是以源码进包的话，进游戏之前先一次编完（dm_shader_prep.m，带进度页）；做完它回头再调这里
    // Shell: if the shaders are shipped as source in the package, compile them all in one go before entering the game (dm_shader_prep.m, with a progress page); when done it calls back here
    { extern int dm_shader_prep_begin(void (^)(void)); static int asked; if (!asked) { asked = 1; if (dm_shader_prep_begin(^{ host_start(); })) return; } }
#endif
    g_host_up = 1; plog("boot: 场景到前台，起宿主（进程已活 %.1f ms）", since_start());
    { const char *mw = getenv("DM_TEST_MEMWARN");   // 测试开关 DM_TEST_MEMWARN=<秒>：起宿主后这么多秒，照系统的调法调一次内存告警回调（验那一行日志；系统真发告警的时机造不出来） | test switch DM_TEST_MEMWARN=<seconds>: this many seconds after the host starts, call the memory-warning callback once the way the system does (to check that log line; the moment the system really sends a warning cannot be staged)
      if (mw && atof(mw) > 0) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(mw) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ UIApplication *app = UIApplication.sharedApplication; [app.delegate applicationDidReceiveMemoryWarning:app]; }); }
    boot_begin();                    // 覆盖层、宿主装载与预取：也等到这里才做（在后台线程，和后面的界面收尾并行） | overlay, host loading and prefetch: also deferred until here (on a background thread, in parallel with the UI wrap-up that follows)
    front_ios_lifecycle_install();   // 过热 / 不自动锁屏 / 亮度（窗口与焦点在 SceneDelegate） | overheating / no auto-lock / brightness (window and focus are in SceneDelegate)
    run_boot();                      // 宿主线程自己写 DONE | The host thread writes DONE itself
}

// ---- msl：真机逐个加载 App 内 iOS 版 metallib（iOS 18 目标在 iOS 27 上能否用），并按反射 json 取入口函数 ----
// ---- msl: on a real device, load each iOS metallib inside the App one by one (whether an iOS 18 target works on iOS 27), and fetch the entry functions per the reflection json ----
static void probe_msl(void) {
    NSString *dir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"msl"];
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSArray *fs = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.metallib'"]];
    int lib_ok = 0, fn_ok = 0, n = 0; CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
    plog("设备 %s，metallib %lu 个", dev.name.UTF8String, (unsigned long)fs.count);
    for (NSString *f in fs) {
        n++; NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:f]] error:&e];
        if (!lib) { if (n - lib_ok <= 5) plog("加载失败 %s：%s", f.UTF8String, e.localizedDescription.UTF8String); continue; }
        lib_ok++;
        NSData *jd = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:[f.stringByDeletingPathExtension stringByAppendingString:@".json"]]];
        NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
        if ([lib newFunctionWithName:j[@"entry"]]) fn_ok++; else if (n - fn_ok <= 5) plog("取入口失败 %s", f.UTF8String);
    }
    plog("加载成功 %d/%d，取入口成功 %d/%d，用时 %.1f 秒", lib_ok, n, fn_ok, n, CFAbsoluteTimeGetCurrent() - t0);
}

static void run_mode(void) {
    const char *m = g_mode.UTF8String;
    if (!strcmp(m, "anon")) probe_anon();
    else if (!strcmp(m, "mapjit")) probe_mapjit();
    else if (!strcmp(m, "file")) probe_file();
    else if (!strcmp(m, "tsd")) probe_tsd();
    else if (!strcmp(m, "msl")) probe_msl();
    else if (!strcmp(m, "blank")) { plog("blank：空白 Metal 60 帧清屏（发热负对照）"); return; }   // 一直跑到被停 | Runs until stopped
    else plog("未知模式 %s", m);
    plog("DONE");
}

@interface App : UIResponder <UIApplicationDelegate>
@end
static void run_boot(void);
// 资源下载界面（安装时系统不给进度，判断不了是在下还是卡住）：资源包不在本地时显示已下 / 总量 / 速度，出错把原因写在屏上并可重试；到了就地起宿主进游戏
// Asset download screen (the system gives no progress during install, so you cannot tell downloading from stuck): when the asset pack is not local, show downloaded / total / speed; on error write the reason on screen and allow retry; once it arrives, start the host in place and enter the game
@interface AssetVC : UIViewController <BAManagedAssetPackDownloadDelegate>
@property (strong) UILabel *label; @property (strong) UIProgressView *bar; @property (strong) UIButton *retry;
@property (assign) int64_t last; @property (assign) CFAbsoluteTime lastT; @property (assign) double rate;
@property (strong) nw_path_monitor_t netmon; @property (assign) int netDown, stopped;   // 守着网络：停在「下载没开始」那一页时，网络从不通变成通就自己重试 | network watch: while sitting on the "download did not start" page, retry automatically when the network goes from down to up
// 托管下载（TestFlight）：进度到 100% 之后系统还要把资源包装进本地（头文件：完成通知 = 下完且本地可用），这段不给进度；完成回调 / 通知不来就会一直停在进度字上（线上玩家卡 10 分钟）。
// 所以守着系统的「本地可用」状态自己轮询（5 秒一次、30 秒记一行日志），到了就进游戏，不只等回调；100% 后屏上说明在安装、空间不够写清差多少；
// 安装超时 / 暂停 / 报错时给「改用直接下载」（走下载站那条路，包里配了下载地址才给）。packFull = 进度到 100% 的时刻，gone = 已改走直接下载（之后托管的回调只记日志）
// Hosted download (TestFlight): after progress reaches 100% the system still has to install the asset pack locally (header: completion notification = downloaded and available locally), and gives no progress for that part; if the completion callback / notification does not come, the screen stays on the progress text forever (players in the field were stuck for 10 minutes).
// So we watch the system's "available locally" state and poll it ourselves (every 5 seconds, a log line every 30 seconds) and enter the game when it is reached, instead of only waiting for the callback; after 100% the screen explains that it is installing, and if space is short it says by how much;
// on install timeout / pause / error it offers "Use direct download" (the download-site path, only offered if the package has a download address configured). packFull = the moment progress reached 100%, gone = already switched to direct download (after that, callbacks from the hosted download are only logged)
@property (strong) UIButton *useDirect, *share; @property (strong) dispatch_source_t poll; @property (assign) CFAbsoluteTime packT, packFull, logT, testAvailAt;
@property (assign) int64_t packBytes, packDone; @property (assign) int gone, slowShown, finished, textKind, packStop;   // packStop：系统报了暂停 / 失败，屏上留那条说明，不再换回「正在安装」 | packStop: the system reported paused / failed; the screen keeps that explanation and does not switch back to "Installing"
@property (assign) BOOL legacyStatusPending;
@property (assign) CFAbsoluteTime legacyStatusT;
@property (assign) NSUInteger legacyStatus;
@property (strong) NSError *legacyStatusError;
@property (copy) void (^legacyStatusCompletion)(BOOL available);
@end
@implementation AssetVC
- (void)netUp { if (self.netDown && self.stopped) { plog("assets: 网络通了，自动重试"); self.netDown = 0; [self start]; } else self.netDown = 0; }
- (void)watchNet {
    nw_path_monitor_t m = nw_path_monitor_create(); self.netmon = m; nw_path_monitor_set_queue(m, dispatch_get_main_queue()); __weak AssetVC *w = self;
    nw_path_monitor_set_update_handler(m, ^(nw_path_t pth) { AssetVC *v = w; if (!v) return; if (nw_path_get_status(pth) == nw_path_status_satisfied) [v netUp]; else v.netDown = 1; });
    nw_path_monitor_start(m);
}
// 资源到齐后游戏就地在这个控制器里起（见 ready），所以它也要做 DmGameVC 那两项声明——否则首次安装下载完直接玩的那一局里，
// 底边上滑 / 顶边下拉一下就生效、底部横条常亮（下载界面本身没有贴边控件，声明了也无妨）
// Once the assets are complete the game starts in place inside this controller (see ready), so it must make the same two declarations as DmGameVC -- otherwise, in the run played right after the first-install download,
// a single swipe up from the bottom edge / pull down from the top edge takes effect and the home indicator stays lit (the download screen itself has no edge-hugging widgets, so declaring them does no harm)
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor = UIColor.blackColor;
    CGRect b = self.view.bounds;
    self.label = [[UILabel alloc] initWithFrame:CGRectMake(40, b.size.height / 2 - 150, b.size.width - 80, 160)];   // 资源不完整时要列出缺的文件清单：最多 8 行，放不下就缩字（最小到一半） | When assets are incomplete, list the missing files: at most 8 lines, shrink the font if they do not fit (down to half size at minimum)
    self.label.textColor = [UIColor colorWithRed:0.85 green:0.72 blue:0.45 alpha:1]; self.label.textAlignment = NSTextAlignmentCenter; self.label.numberOfLines = 8; self.label.adjustsFontSizeToFitWidth = YES; self.label.minimumScaleFactor = 0.5;
    self.label.font = [UIFont systemFontOfSize:17 weight:UIFontWeightMedium]; self.label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
    self.bar = [[UIProgressView alloc] initWithFrame:CGRectMake(b.size.width * 0.2, b.size.height / 2 + 24, b.size.width * 0.6, 4)];
    self.bar.progressTintColor = [UIColor colorWithRed:0.80 green:0.25 blue:0.10 alpha:1]; self.bar.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
    self.retry = [UIButton buttonWithType:UIButtonTypeSystem]; self.retry.frame = CGRectMake(b.size.width / 2 - 60, b.size.height / 2 + 50, 120, 44);
    [self.retry setTitle:T("dmDlRetry") forState:UIControlStateNormal]; self.retry.hidden = YES; self.retry.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin | UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
    [self.retry addTarget:self action:@selector(start) forControlEvents:UIControlEventTouchUpInside];
    self.useDirect = [UIButton buttonWithType:UIButtonTypeSystem]; [self.useDirect setTitle:T("dmDlUseDirect") forState:UIControlStateNormal]; self.useDirect.hidden = YES;
    [self.useDirect addTarget:self action:@selector(useDirectTap) forControlEvents:UIControlEventTouchUpInside];
    self.share = [UIButton buttonWithType:UIButtonTypeSystem]; [self.share setTitle:T("dmPauseShareLog") forState:UIControlStateNormal]; self.share.hidden = YES;
    [self.share addTarget:self action:@selector(shareTap) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:self.label]; [self.view addSubview:self.bar]; [self.view addSubview:self.retry]; [self.view addSubview:self.useDirect]; [self.view addSubview:self.share];
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    [self watchNet];
    [self start];
}
- (void)viewDidLayoutSubviews {   // 按钮一行居中排：重试 / 改用直接下载 / 分享诊断日志，只排露出来的 | buttons centred in one row: Retry / Use direct download / Share diagnostic log, only the ones showing
    [super viewDidLayoutSubviews]; CGRect b = self.view.bounds; NSMutableArray<UIButton *> *row = [NSMutableArray array]; CGFloat w = 0;
    for (UIButton *x in @[self.retry, self.useDirect, self.share]) if (!x.hidden) { [x sizeToFit]; [row addObject:x]; w += MAX(120, x.bounds.size.width + 24) + (row.count > 1 ? 16 : 0); }
    CGFloat x0 = (b.size.width - w) / 2;
    for (UIButton *x in row) { CGFloat bw = MAX(120, x.bounds.size.width + 24); x.frame = CGRectMake(x0, b.size.height / 2 + 50, bw, 44); x0 += bw + 16; }
}
- (void)showBtn:(UIButton *)x { dispatch_async(dispatch_get_main_queue(), ^{ x.hidden = NO; [self.view setNeedsLayout]; }); }
- (void)shareTap { plog("assets: 玩家点了分享诊断日志"); void dm_sharelog_share(void); dm_sharelog_share(); }
- (void)useDirectTap {
    plog("assets: 玩家点了改用直接下载（托管已等 %.0f 秒，进度 %.2f / %.2f GB%s）", CFAbsoluteTimeGetCurrent() - self.packT, self.packDone / 1e9, self.packBytes / 1e9, self.packFull ? "，已到 100%" : "");
    self.gone = 1; if (self.poll) { dispatch_source_cancel(self.poll); self.poll = nil; }
    self.useDirect.hidden = YES; [self.view setNeedsLayout]; [self direct:nil];
}
- (void)offerDirect:(int)why {   // 托管那条路走不下去时给玩家另一条（why：1 安装超时、2 暂停、3 报错）：包里没配下载地址（照公开仓库出的包）就不给，只记日志 | when the hosted path cannot go on, give the player another path (why: 1 install timeout, 2 paused, 3 error): if the package has no download address configured (packages built from the public repository), it is not offered, only logged
    if (self.gone) return;
    if (!res_dl_configured()) { static int lg; if (!lg++) plog("assets: 托管走不下去（%d：1 安装超时 2 暂停 3 报错），但本包没配资源下载地址，不给「改用直接下载」", why); return; }
    plog("assets: 托管走不下去（%d：1 安装超时 2 暂停 3 报错）→ 露出「改用直接下载」", why); [self showBtn:self.useDirect]; [self showBtn:self.share];
}
static double slow_s(void) { const char *t = getenv("DM_TEST_BA_SLOW"); return t ? atof(t) : 300; }   // 100% 后等系统装进本地多久算慢（头文件只说完成通知 = 本地可用，中间要多久没写；给 5 分钟）；测试开关缩短 | how long to wait after 100% for the system to install it locally before it counts as slow (the header only says the completion notification = available locally, not how long it takes in between; 5 minutes is given); a test switch shortens it
static int64_t free_bytes(void) {   // 给 App 用的可用空间（系统「重要用途」口径，同设置里显示的）；测试开关 DM_TEST_BA_FREE=<GB> 造空间不够 | free space available to the App (the system's "important usage" measure, the same as shown in Settings); test switch DM_TEST_BA_FREE=<GB> stages not enough space
    const char *t = getenv("DM_TEST_BA_FREE"); if (t) return (int64_t)(atof(t) * 1e9);
    NSURL *u = [NSURL fileURLWithPath:NSHomeDirectory()]; NSNumber *v = nil; [u getResourceValue:&v forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil]; return v.longLongValue;
}
- (void)legacyPackReady {
    if (self.gone || g_host_up) return;
    g_pack_legacy_available = YES;
    [self ready];
}
- (void)queryLegacyPack:(void (^)(BOOL available))completion API_AVAILABLE(ios(26.0)) {
    // 旧的状态接口可能联网：并发的查询合并成一次，状态全放在主线程。
    // The old status API may use the network: concurrent queries are coalesced into one, and all state stays on the main thread.
    self.legacyStatusCompletion = completion;
    if (self.legacyStatusPending) return;
    self.legacyStatusPending = YES;
    [BAAssetPackManager.sharedManager getStatusOfAssetPackWithIdentifier:@"romfs" completionHandler:^(BAAssetPackStatus st, NSError *e) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.legacyStatusPending = NO; self.legacyStatusT = CFAbsoluteTimeGetCurrent(); self.legacyStatus = st; self.legacyStatusError = e;
            void (^done)(BOOL) = self.legacyStatusCompletion; self.legacyStatusCompletion = nil;
            if (self.gone || g_host_up) return;
            // 过时的查询结果不能撤销已经收到的下载完成确认。
            // A stale query result must not undo a download completion that has already been received.
            if (!e && (st & BAAssetPackStatusDownloaded)) g_pack_legacy_available = YES;
            if (e) plog("assets: 托管资源包状态查询失败（iOS 26.4 以前的接口）：%s", e.localizedDescription.UTF8String);
            if (done) done(g_pack_legacy_available);
        });
    }];
}
- (BOOL)packAvail {
    if (getenv("DM_TEST_BA")) return self.testAvailAt > 0 && CFAbsoluteTimeGetCurrent() >= self.testAvailAt;
    if (@available(iOS 26.4, *)) return [BAAssetPackManager.sharedManager assetPackIsAvailableLocallyWithIdentifier:@"romfs"];
    if (@available(iOS 26.0, *)) {
        if (!g_pack_legacy_available && !self.legacyStatusPending && CFAbsoluteTimeGetCurrent() - self.legacyStatusT >= 30)
            [self queryLegacyPack:^(BOOL available) { if (available) [self ready]; }];
        return g_pack_legacy_available;
    }
    return NO;
}
- (void)logPack:(BOOL)avail free:(int64_t)fr {
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent(); double w = now - self.packT, f = self.packFull ? now - self.packFull : -1, d = self.packDone / 1e9, t = self.packBytes / 1e9, g = fr / 1e9; int a = avail;
    if (getenv("DM_TEST_BA")) { plog("assets: 托管状态（测试现场）：已等 %.0f 秒，进度 %.2f / %.2f GB（100%% 后 %.0f 秒，-1 = 没到），系统说本地可用 %d，可用空间 %.2f GB", w, d, t, f, a, g); return; }
    if (@available(iOS 26.4, *)) {
        [BAAssetPackManager.sharedManager getLocalStatusOfAssetPackWithIdentifier:@"romfs" completionHandler:^(BAAssetPackStatus st) {
            plog("assets: 托管状态：已等 %.0f 秒，进度 %.2f / %.2f GB（100%% 后 %.0f 秒，-1 = 没到），系统说本地可用 %d，可用空间 %.2f GB；系统状态位 0x%lx（1 可下载 2 有更新 4 最新 8 过期 0x10 已撤 0x20 下载中 0x40 已下完）",
                 w, d, t, f, a, g, (unsigned long)st); }];
    } else if (@available(iOS 26.0, *)) {
        plog("assets: 托管资源包（iOS 26.4 以前的接口）：已等 %.0f 秒，%.2f / %.2f GB，满额已 %.0f 秒，可用 %d，可用空间 %.2f GB，状态 0x%lx，查询在途 %d，错误 %s",
             w, d, t, f, a, g, (unsigned long)self.legacyStatus, self.legacyStatusPending, self.legacyStatusError.localizedDescription.UTF8String ?: "无");
    } else plog("assets: 托管状态：已等 %.0f 秒，进度 %.2f / %.2f GB（100%% 后 %.0f 秒，-1 = 没到），系统说本地可用 %d，可用空间 %.2f GB", w, d, t, f, a, g);
}
- (void)packText:(int64_t)fr {   // 100% 之后屏上那段：空间不够 > 超时 > 正在安装 | the passage on screen after 100%: not enough space > timeout > installing
    NSString *dev = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? @"iPad" : @"iPhone";
    int k = self.packBytes > 0 && fr < self.packBytes ? 3 : self.slowShown ? 2 : 1;
    NSString *t = k == 3 ? [NSString stringWithFormat:T("dmDlInstallSpace"), self.packBytes / 1e9, dev, fr / 1e9, (self.packBytes - fr) / 1e9, dev]
                : k == 2 ? [NSString stringWithFormat:T(res_dl_configured() ? "dmDlPackSlow" : "dmDlPackSlowNoDl"), (int)MAX(1, lround(slow_s() / 60))]   // 没配下载地址就没有「改用直接下载」按钮，文字不提它 | with no download address configured there is no "Use direct download" button, and the text does not mention it
                : [NSString stringWithFormat:T("dmDlInstalling"), self.packBytes / 1e9, fr / 1e9];
    if (k != self.textKind) { self.textKind = k; [self say:t]; }   // 换了一种说法才进日志；只是空间数字变了就只改屏上的字 | only goes into the log when the wording changes; if only the space numbers changed, just update the text on screen
    else if (![self.label.text isEqualToString:t]) dispatch_async(dispatch_get_main_queue(), ^{ self.label.text = t; });
}
- (void)pollPack {
    if (self.gone) return;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent(); BOOL avail = [self packAvail]; int64_t fr = free_bytes();
    if (now - self.logT >= 30) { self.logT = now; [self logPack:avail free:fr]; }
    if (avail) { plog("assets: 轮询：系统说资源包本地可用（完成回调%s）→ 进游戏", self.finished ? "也到了" : "没到"); [self ready]; return; }
    if (!self.packFull || self.packStop) return;
    if (!self.slowShown && now - self.packFull >= slow_s()) { self.slowShown = 1; plog("assets: 进度 100%% 后 %.0f 秒系统仍没装好", now - self.packFull); [self offerDirect:1]; }
    [self packText:fr];
}
- (void)watchPack {   // 托管下载开始后：守着系统状态（主线程定时器） | after the hosted download starts: watch the system state (main-thread timer)
    if (self.poll) return; self.packT = CFAbsoluteTimeGetCurrent(); self.logT = 0;
    dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue()); self.poll = s; __weak AssetVC *w = self;
    dispatch_source_set_timer(s, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
    dispatch_source_set_event_handler(s, ^{ [w pollPack]; }); dispatch_resume(s);
}
// 测试开关 DM_TEST_BA=finish|late|silent|fail（Mac 上的 iOS 包造托管下载现场，资源其实在本地）：3 秒内假进度走到 100%，之后——
// finish 再 3 秒送完成通知；late 什么都不来（配 DM_TEST_BA_SLOW=<秒> 看超时）；silent 不送通知、8 秒后「系统说本地可用」（看轮询自愈）；fail 送下载失败。
// DM_TEST_BA_TAP=<秒>：到点替玩家点「改用直接下载」（按钮没露出来就报红）
// Test switch DM_TEST_BA=finish|late|silent|fail (stages a hosted download scene for the iOS package on the Mac, the resources are actually local): fake progress goes to 100% within 3 seconds, then -
// finish sends the completion notification 3 seconds later; late sends nothing (with DM_TEST_BA_SLOW=<seconds> to see the timeout); silent sends no notification and "the system says it is available locally" after 8 seconds (to see polling recover); fail sends a download failure.
// DM_TEST_BA_TAP=<seconds>: at that time, taps "Use direct download" for the player (reports red if the button is not showing)
- (void)fakeBA:(NSString *)sc API_AVAILABLE(ios(26.0)) {
    plog("assets: 测试现场 DM_TEST_BA=%s", sc.UTF8String); self.packBytes = 4130000000LL; [self watchPack]; [self say:T("dmDlDownloading")];
    NSProgress *g = [NSProgress progressWithTotalUnitCount:self.packBytes]; BAAssetPack *np = nil;
    for (int i = 1; i <= 10; i++) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(i * 0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ g.completedUnitCount = self.packBytes * i / 10; [self downloadOfAssetPack:np hasProgress:g]; });
    if ([sc isEqualToString:@"silent"]) self.testAvailAt = CFAbsoluteTimeGetCurrent() + 11;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if ([sc isEqualToString:@"finish"]) [self downloadOfAssetPackFinished:np];
        if ([sc isEqualToString:@"fail"]) [self downloadOfAssetPack:np failedWithError:[NSError errorWithDomain:@"DmTestBA" code:1 userInfo:@{NSLocalizedDescriptionKey: @"test failure"}]]; });
    const char *tp = getenv("DM_TEST_BA_TAP"); if (tp) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(tp) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (self.useDirect.hidden) plog("【红】DM_TEST_BA_TAP：「改用直接下载」没露出来"); else [self useDirectTap]; });
}
- (void)say:(NSString *)t { plog("assets: %s", t.UTF8String); dispatch_async(dispatch_get_main_queue(), ^{ self.label.text = t; }); }
- (void)fail:(NSString *)what error:(NSError *)e {
    // 直装包没有托管资源，这条路必然失败：报错只进日志，界面给玩家能照做的指引
    // A direct-install build has no hosted assets, so this path always fails: the error goes only to the log, and the screen gives the player instructions they can follow
    plog("assets: %s失败：%s（%s %ld）", what.UTF8String, e.localizedDescription.UTF8String, e.domain.UTF8String, (long)e.code);
    [self say:no_assets_text()];
    [self showBtn:self.retry];
}
- (void)start {
    self.retry.hidden = YES; self.stopped = 0; self.last = 0; self.lastT = CFAbsoluteTimeGetCurrent(); self.packStop = 0; self.textKind = 0;
    char why[2048];
    if (getenv("DM_TEST_BA") && !self.gone) { if (@available(iOS 26.0, *)) [self fakeBA:@(getenv("DM_TEST_BA"))]; return; }   // 测试：造托管下载现场（fakeBA） | test: stage a hosted download scene (fakeBA)
    if (!getenv("DM_ASSETS_TEST") && romfs_locate(why, sizeof why)) { [self ready]; return; }   // 重试时资源已到（如刚在「文件」里拷完） | Assets have already arrived on retry (e.g. just finished copying in the Files app)
    if (!getenv("DM_ASSETS_TEST") && g_doc_bad) { [self direct:@(why)]; return; }   // 有文件夹但不完整（拷到一半 / 下到一半）：不试托管下载，直接从下载站补齐；下载站走不通才报原因 | A folder exists but is incomplete (half copied / half downloaded): do not try the hosted download, fill in the rest straight from the download site; report the reason only if the download site is unreachable
    if (self.gone) { [self direct:nil]; return; }   // 玩家已选「改用直接下载」：重试仍走这条（托管的回调此后不接，回到托管会停在「正在下载」） | the player chose "Use direct download": retries keep going this way (callbacks from the hosted download are not taken from now on; going back to hosted would stay stuck at "Downloading")
    NSString *stray = !getenv("DM_ASSETS_TEST") && g_doc_stray ? @(why) : nil;   // 「D2R Mobile」里有文件夹但都不是资源：托管下载、下载站都走不通时给玩家看这条 | 「D2R Mobile」 has folders but none of them is assets: show the player this message when neither the hosted download nor the download site works
    if (getenv("DM_ROMFS_TEST_PACK")) { [self ready]; return; }   // 只测试用：托管资源包「下完了」但不合格的现场 | Test only: the scenario where the managed asset pack has "finished downloading" but does not pass validation
    if (getenv("DM_ASSETS_TEST") && !strcmp(getenv("DM_ASSETS_TEST"), "direct")) { [self direct:nil]; return; }   // 测试：资源在本地也走真正的直接下载准备（联网取清单、盘点、查空间），配 DM_ASSET_FREE_TEST 造空间不够 | Test: with assets local, still go through the real direct-download preparation (fetch the manifest over the network, take inventory, check space); pair with DM_ASSET_FREE_TEST to fake insufficient space
    if (getenv("DM_ASSETS_TEST")) {   // 测试：资源其实在本地，停 2 秒后走「就地起宿主」 | Test: the assets are actually local; pause 2 seconds, then take the "start the host in place" path
        [self say:T("dmDlTestProgress")];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self ready]; });
        return;
    }
    if (!ba_configured()) { [self direct:stray]; return; }
    if (@available(iOS 26.0, *)) {
        void (^download)(void) = ^{
            [self say:T("dmDlConnecting")];
            BAAssetPackManager *m = BAAssetPackManager.sharedManager; m.delegate = self;
            [m getAssetPackWithIdentifier:@"romfs" completionHandler:^(BAAssetPack *pack, NSError *e) {
                if (!pack) { plog("assets: 没有托管资源包（%s），改从下载站直接下", e.localizedDescription.UTF8String); dispatch_async(dispatch_get_main_queue(), ^{ [self direct:stray]; }); return; }   // 直装包走到这里必然失败 | A direct-install build always fails when it gets here
                [self say:T("dmDlDownloading")];
                plog("assets: 托管资源包 版本 %ld、%.2f GB，请系统确保本地可用（可用空间 %.2f GB）", (long)pack.version, pack.downloadSize / 1e9, free_bytes() / 1e9);
                dispatch_async(dispatch_get_main_queue(), ^{ self.packBytes = pack.downloadSize; [self watchPack]; });
                [m ensureLocalAvailabilityOfAssetPack:pack completionHandler:^(NSError *e2) {
                    if (e2) { self.packStop = 1; [self fail:@"下载游戏资源" error:e2]; [self offerDirect:3]; return; }
                    plog("assets: 系统「确保本地可用」完成回调到了");
                    dispatch_async(dispatch_get_main_queue(), ^{
                        if (@available(iOS 26.4, *)) [self ready];
                        else [self legacyPackReady];
                    });
                }];
            }];
        };
        if (@available(iOS 26.4, *)) download();
        else {
            [self say:T("dmDlConnecting")];
            [self queryLegacyPack:^(BOOL available) { if (available) [self ready]; else download(); }];
        }
    } else [self direct:stray];
}
// 直装包：从我们自己的下载站把资源直接下到 Documents/D2R Mobile Resources（asset_dl.m）。下载站走不通时界面退回原来的指引（或 fallback 给的原因）
// Direct-install build: download the assets straight from our own download site into Documents/D2R Mobile Resources (asset_dl.m). When the download site is unreachable the screen falls back to the original instructions (or the reason given by fallback)
- (void)direct:(NSString *)fallback {
    if (!res_dl_configured()) {   // 这个包没配下载地址（照公开仓库自己出的包）：不去连、不说「下载没开始」，只讲把资源拷进来 | This build has no download address configured (a build made by yourself from the public repository): do not connect, do not say "the download has not started", only explain how to copy the resources in
        plog("assets: 本包没配资源下载地址，不走下载"); [self say:fallback ? [NSString stringWithFormat:@"%@\n%@", fallback, T("dmDlCopyInShort")] : no_assets_text()]; self.bar.hidden = YES; [self showBtn:self.retry]; self.stopped = 1; return; }   // 这条路没有进度可报：不留一条空的进度条 | nothing to report progress on along this path: do not leave an empty progress bar
    [self say:T("dmDlConnecting")];
    [DmAssetDL.shared prepare:^(NSString *err, int64_t remain, int64_t total, BOOL cellular) {
        if (err && DmAssetDL.shared.shortBytes > 0) {   // 设备空间不够（原先只进日志、界面显示的是「先从内测页下载资源包」，跟真实原因无关）：系统提示写清还差多少 | Not enough space on the device (previously this only went to the log and the screen showed 「先从内测页下载资源包」(First download the asset pack from the beta page), unrelated to the real cause): the system prompt states exactly how much is missing
            plog("assets: 直接下载走不了：%s", err.UTF8String);
            NSString *dev = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? @"iPad" : @"iPhone";   // 按设备写 | Wording depends on the device
            NSString *m = [NSString stringWithFormat:T("dmDlSpaceMsg"), DmAssetDL.shared.shortBytes / 1e9, remain / 1e9, dev, DmAssetDL.shared.freeBytes / 1e9, dev];
            NSString *title = [NSString stringWithFormat:T("dmDlSpaceTitle"), dev];
            [self say:[[title stringByAppendingString:@"\n"] stringByAppendingString:m]]; [self showBtn:self.retry];
            UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:m preferredStyle:UIAlertControllerStyleAlert];
            [a addAction:[UIAlertAction actionWithTitle:T("dmOk") style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:a animated:YES completion:^{ plog("assets: 已弹系统提示「%s」｜正文：%s", title.UTF8String, [m stringByReplacingOccurrencesOfString:@"\n" withString:@" ⏎ "].UTF8String); }]; return; }
        // 只测试用：DM_ASSET_OFFLINE_TEST=<秒> = 第一次准备当作没网，这么多秒后当作网络通了（造「没网 → 网络恢复自动重试」现场）
        // Test only: DM_ASSET_OFFLINE_TEST=<seconds> = the first prepare is treated as offline, and after this many seconds the network is treated as back
        // (stages the "offline → automatic retry when the network returns" test case)
        { static int once; const char *ot = getenv("DM_ASSET_OFFLINE_TEST"); if (ot && !once) { once = 1; err = T("dmDlNoServer"); self.netDown = 1;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(ot) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ [self netUp]; }); } }
        if (err) {   // 下载没开始：屏上写真实原因和下一步，不落回「还没有游戏资源」（没网时玩家看到的是「没有资源」，不知道是网络、也不知道能重试） | download did not start: show the real reason and the next step on screen instead of falling back to "no game assets yet" (with no network the player saw "no assets", not knowing it was the network or that a retry was possible)
            plog("assets: 直接下载走不了：%s", err.UTF8String);
            [self say:[NSString stringWithFormat:T("dmDlNotStarted"), fallback ? [fallback stringByAppendingString:@"\n"] : @"", self.netDown ? T("dmDlNetDown") : err, self.netDown ? T("dmDlNextAuto") : T("dmDlNextRetry")]];
            [self showBtn:self.retry]; self.stopped = 1; return; }
        void (^go)(void) = ^{
            [self say:[NSString stringWithFormat:T("dmDlWill"), remain < 100000000 ? [NSString stringWithFormat:@"%.0f MB", remain / 1e6] : [NSString stringWithFormat:@"%.1f GB", remain / 1e9], total / 1e9]];   // 自动开始的大下载，开始前先把要下多少写在屏上 | A large download that starts automatically: show on screen how much will be downloaded before it starts
            self.last = total - remain; self.lastT = CFAbsoluteTimeGetCurrent(); self.rate = 0;
            [DmAssetDL.shared run:^(int64_t done, int64_t tot, NSString *note) {
                CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
                if (now - self.lastT >= 1.0) { self.rate = MAX(0, done - self.last) / (now - self.lastT); self.last = done; self.lastT = now; }
                self.label.text = [NSString stringWithFormat:@"正在下载游戏资源（共 %.1f GB）\n%.2f / %.2f GB（%.0f%%）  %.1f MB/s%@", tot / 1e9, done / 1e9, tot / 1e9, tot ? 100.0 * done / tot : 0, self.rate / 1e6, note ? [@"\n" stringByAppendingString:note] : @""];
                self.bar.progress = tot ? (float)done / tot : 0;
            } done:^(NSString *e2) {
                if (e2) { [self say:[NSString stringWithFormat:T("dmDlStopped"), e2, self.last / 1e9, total / 1e9]]; [self showBtn:self.retry]; self.stopped = 1; return; }
                [self ready];
            }];
        };
        if (!cellular || remain <= 0) { go(); return; }
        UIAlertController *a = [UIAlertController alertControllerWithTitle:T("dmDlCellTitle") message:[NSString stringWithFormat:T("dmDlCellMsg"), remain / 1e9] preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:T("dmDlCellLater") style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { [self say:T("dmDlCellWait")]; [self showBtn:self.retry]; }]];
        [a addAction:[UIAlertAction actionWithTitle:T("dmDlCellGo") style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { plog("assets: 玩家同意用流量下载"); go(); }]];
        [self presentViewController:a animated:YES completion:nil];
    }];
}
- (void)downloadOfAssetPack:(BAAssetPack *)p hasProgress:(NSProgress *)g API_AVAILABLE(ios(26.0)) {
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent(); int64_t done = g.completedUnitCount, total = g.totalUnitCount;
    if (self.gone) return;   // 已改走直接下载：屏上是那条路的进度 | already switched to direct download: the screen shows that path's progress
    self.packDone = done; if (total > 0) self.packBytes = total;
    if (total > 0 && done >= total) {   // 到 100%：之后是系统装包，屏上改说明（进度字不再刷） | reached 100%: from here the system installs the pack, the screen switches to an explanation (the progress text is no longer updated)
        if (!self.packFull) { self.packFull = now; plog("assets: 托管下载进度到 100%%（%.2f GB），等系统装进本地；可用空间 %.2f GB", total / 1e9, free_bytes() / 1e9);
            dispatch_async(dispatch_get_main_queue(), ^{ self.bar.progress = 1; [self packText:free_bytes()]; [self showBtn:self.share]; }); }
        return; }
    if (now - self.lastT >= 1.0) { self.rate = (done - self.last) / (now - self.lastT); self.last = done; self.lastT = now; }
    NSString *t = [NSString stringWithFormat:T("dmDlProgress"), done / 1e9, total / 1e9, total ? 100.0 * done / total : 0, self.rate / 1e6];
    dispatch_async(dispatch_get_main_queue(), ^{ self.label.text = t; self.bar.progress = total ? (float)done / total : 0; });
}
- (void)downloadOfAssetPackBegan:(BAAssetPack *)p API_AVAILABLE(ios(26.0)) { plog("assets: 系统通知托管下载开始 / 继续"); self.packStop = 0; self.textKind = 0; }
- (void)downloadOfAssetPackFinished:(BAAssetPack *)p API_AVAILABLE(ios(26.0)) {
    plog("assets: 系统通知资源包下载完成%s", self.gone ? "（已改走直接下载，不接）" : ""); if (self.gone) return; self.finished = 1;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (@available(iOS 26.4, *)) [self ready];
        else if (!p || [p.identifier isEqualToString:@"romfs"]) [self legacyPackReady];
    }); }
- (void)downloadOfAssetPackPaused:(BAAssetPack *)p API_AVAILABLE(ios(26.0)) {
    plog("assets: 系统通知托管下载暂停%s", self.gone ? "（已改走直接下载）" : ""); if (self.gone) return; self.packStop = 1; [self say:T("dmDlPaused")]; [self offerDirect:2]; }
- (void)downloadOfAssetPack:(BAAssetPack *)p failedWithError:(NSError *)e API_AVAILABLE(ios(26.0)) {
    if (self.gone) { plog("assets: 托管下载失败（已改走直接下载）：%s", e.localizedDescription.UTF8String); return; }
    self.packStop = 1; [self fail:@"下载游戏资源" error:e]; [self offerDirect:3]; }
- (void)ready {
    char why[2048];
    static int started; if (started) return;   // 完成回调与系统的完成通知都会走到这里：只起一次 | Both the completion callback and the system's completion notification end up here: start only once
    if (!romfs_locate(why, sizeof why)) {
        if (g_pack_wait) { if (!self.packFull) [self say:T("dmDlPackWait")]; return; }   // 等系统说本地可用（完成通知或 pollPack 轮询到）；100% 后屏上是 packText 那段 | waiting for the system to say it is available locally (the completion notification, or found by pollPack's polling); after 100% the screen shows the packText passage
        if (self.poll) { plog("assets: 系统说可用但校验没过：%s", why); }
        [self say:[NSString stringWithFormat:T("dmDlNotReady"), @(why)]]; [self showBtn:self.retry]; return; }
    started = 1; if (self.poll) { dispatch_source_cancel(self.poll); self.poll = nil; }
    plog("assets: 资源就绪，就地起宿主");
    g_need_assets = 0;
    self.view = front_ios_view(self.view.window.windowScene.coordinateSpace.bounds);
    [self setNeedsUpdateOfScreenEdgesDeferringSystemGestures]; [self setNeedsUpdateOfHomeIndicatorAutoHidden];
    host_start();
    front_ios_focus_event(3);
}
@end
static void astc_handoff(UIWindow *w) {   // 补充包到位（玩家刚拷好 / 刚下完）：交给资源界面——资源齐了就地进游戏，不齐就照常提示 / 下载 | Legacy Device Pack in place (the player just finished copying / downloading it): hand over to the asset screen -- if assets are complete, enter the game in place; if not, prompt / download as usual
    g_no_bc = 0; g_need_assets = 1; AssetVC *vc = [AssetVC new]; w.rootViewController = vc;
}
// 场景生命周期（Info.plist UIApplicationSceneManifest → SceneDelegate）：建窗口 + 焦点事件交 front_ios.m
// 游戏画面的根控制器：画面铺满全屏（含灵动岛两侧与圆角），所以要向系统声明两件事——
// ① 四边的系统边缘手势延后一拍（底边上滑回桌面、顶边下拉通知）：摇杆 / 技能钮 / 底栏都贴着边，不延后的话一拖就退出游戏；延后 = 第一下只露出横条、第二下才生效
// ② 底部横条自动隐藏（声明了 ① 时系统以 ① 为准、把横条调暗；两个都声明是游戏类 App 的通常做法）
// Scene lifecycle (Info.plist UIApplicationSceneManifest → SceneDelegate): create the window + hand focus events to front_ios.m
// Root controller of the game picture: the picture fills the whole screen (including both sides of the Dynamic Island and the rounded corners), so two things must be declared to the system --
// ① System edge gestures on all four edges are deferred by one step (swipe up from the bottom edge to go home, pull down from the top edge for notifications): the stick / skill buttons / bottom bar all sit against the edges, and without deferral one drag exits the game; deferred = the first swipe only reveals the bar, the second takes effect
// ② The home indicator auto-hides (when ① is declared the system follows ① and dims the indicator; declaring both is the usual practice for game Apps)
@interface DmGameVC : UIViewController
@end
@implementation DmGameVC
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (void)viewSafeAreaInsetsDidChange {   // 安全区实际数值记进启动日志（排控件位置用，从 UIKit 读、不凭印象） | Record the actual safe-area values in the boot log (used for placing widgets; read from UIKit, not from memory)
    [super viewSafeAreaInsetsDidChange]; UIEdgeInsets i = self.view.safeAreaInsets; CGSize z = self.view.bounds.size;
    plog("安全区 上 %.1f 左 %.1f 下 %.1f 右 %.1f（点）；视图 %.0fx%.0f；朝向 %ld", i.top, i.left, i.bottom, i.right, z.width, z.height, (long)self.view.window.windowScene.interfaceOrientation);
}
@end
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    UIWindowScene *ws = (UIWindowScene *)scene;
    self.window = [[UIWindow alloc] initWithWindowScene:ws]; g_window = self.window;
    UIViewController *vc = [DmGameVC new];
    vc.view.backgroundColor = UIColor.blackColor;
    if ([g_mode isEqualToString:@"boot"] && g_block_title) { vc = [BlockVC new]; vc.view.backgroundColor = UIColor.blackColor; }   // 图形芯片不支持 / 安装包不完整：只弹框 | Graphics chip not supported / install package incomplete: only show the message
    else if ([g_mode isEqualToString:@"boot"] && g_no_bc) { vc = [NoBcVC new]; vc.view.backgroundColor = UIColor.blackColor; }   // 本机型不支持：只弹框 | This device model is not supported: only show an alert
    else if ([g_mode isEqualToString:@"boot"] && g_need_assets) vc = [AssetVC new];   // 资源包没到：下载界面 | Asset pack not here yet: download screen
    else if ([g_mode isEqualToString:@"boot"]) vc.view = front_ios_view(ws.coordinateSpace.bounds);   // 图形后端出图的层（不叠任何控件） | The layer the graphics backend presents into (no widgets stacked on it)
    else if ([g_mode isEqualToString:@"blank"]) { UIView *front_ios_blank_view(CGRect); vc.view = front_ios_blank_view(ws.coordinateSpace.bounds); }   // 发热负对照 | Heat negative control
    else {   // 探针模式：屏上标出模式名 | Probe mode: show the mode name on screen
        UILabel *l = [[UILabel alloc] initWithFrame:vc.view.bounds];
        l.text = [NSString stringWithFormat:@"dm iOS · %@", g_mode]; l.textAlignment = NSTextAlignmentCenter;
        l.textColor = UIColor.whiteColor; [vc.view addSubview:l];
    }
    self.window.rootViewController = vc; [self.window makeKeyAndVisible];
    g_T[4] = since_start();
    plog("启动计时 T4 willConnect 末 %.1f ms", g_T[4]);
}
- (void)sceneWillResignActive:(UIScene *)s { if (g_host_up) front_ios_focus_event(0); }
- (void)sceneDidEnterBackground:(UIScene *)s { if (g_host_up) front_ios_focus_event(1); }
- (void)sceneWillEnterForeground:(UIScene *)s { log_promote(); if ([g_mode isEqualToString:@"boot"] && !g_need_assets && !g_no_bc && !g_block_title) { host_start(); if (g_host_up) front_ios_focus_event(2); } }
- (void)sceneDidBecomeActive:(UIScene *)s { if (g_host_up) front_ios_focus_event(3); }
// 场景断开 / App 终止：宿主有序收尾后 _exit（hle_lifecycle.c）。注意：用户在多任务界面划掉一个已在后台挂起的 App 时，系统直接 SIGKILL，不走这两个回调
// Scene disconnect / App termination: the host shuts down in order, then _exit (hle_lifecycle.c). Note: when the user swipes away an App already suspended in the background from the multitasking screen, the system sends SIGKILL directly and neither of these two callbacks runs
- (void)sceneDidDisconnect:(UIScene *)s { if (g_host_up) dm_host_shutdown("iOS 场景断开"); }
@end
// 测试开关 DM_TEST_BG_RELAUNCH=<网址>：造一次真的「系统后台拉起」——用一个会唤起 App 的后台下载会话、推迟 6 秒取这个网址，2 秒后本进程自己退出；
// 系统下完后在后台重新拉起 App（那次启动不带任何环境变量）、把事件交到 handleEventsForBackgroundURLSession。看那次的日志：只该有「后台测试会话」一行，不该起宿主。
// Test switch DM_TEST_BG_RELAUNCH=<URL>: stages a real "background relaunch by the system": using a background download session that wakes the App,
// fetch this URL with a 6 second delay, and have this process exit by itself after 2 seconds;
// once the download finishes the system relaunches the App in the background (that launch carries no environment variables) and delivers the event to
// handleEventsForBackgroundURLSession. Check that launch's log: it should contain only a "background test session" line, and the host must not start.
@interface BgTest : NSObject <NSURLSessionDownloadDelegate> @end
@implementation BgTest
- (void)URLSession:(NSURLSession *)s downloadTask:(NSURLSessionDownloadTask *)t didFinishDownloadingToURL:(NSURL *)u { plog("后台测试会话：下载到手（丢弃）"); }
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)t didCompleteWithError:(NSError *)e { plog("后台测试会话：任务结束%s", e ? e.localizedDescription.UTF8String : ""); }
@end
static NSURLSession *bg_test_session(void) {
    static NSURLSession *s; static dispatch_once_t once;
    dispatch_once(&once, ^{ NSURLSessionConfiguration *c = [NSURLSessionConfiguration backgroundSessionConfigurationWithIdentifier:[NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".bgtest"]];
        c.sessionSendsLaunchEvents = YES; c.discretionary = NO; s = [NSURLSession sessionWithConfiguration:c delegate:[BgTest new] delegateQueue:nil]; });
    return s;
}
static void bg_relaunch_test(void) {
    const char *u = getenv("DM_TEST_BG_RELAUNCH"); NSURL *url = u ? [NSURL URLWithString:@(u)] : nil; if (!url) return;
    plog("后台测试会话：6 秒后才许开始取 %s，2 秒后本进程退出，等系统在后台拉起", u);
    NSURLSessionDownloadTask *t = [bg_test_session() downloadTaskWithURL:url]; t.earliestBeginDate = [NSDate dateWithTimeIntervalSinceNow:6]; [t resume];   // 任务要在本进程退出之后才完成，系统才会拉起（真机实测：马上取的话 2 秒内就下完了，不拉起） | the task must complete only after this process has exited for the system to relaunch the App (measured on a real device: if fetched right away, the download finishes within 2 seconds and there is no relaunch)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ exit(0); });
}

@implementation App
- (void)applicationWillTerminate:(UIApplication *)a { if (g_host_up) dm_host_shutdown("iOS App 终止"); }
// 系统内存告警：只记一行，不放任何缓存。用途：玩家被系统按内存终止前不会有任何通知，上一局日志里至少留下前兆与当时的几项读数
// （常驻、显卡已分配、游戏主堆余量是上次每 300 帧读到的值、离系统内存上限还有多少）。一局最多记 50 行
// System memory warning: log one line only, release no cache. Purpose: a player gets no notice before the system terminates the app for memory, so the previous session's log at least keeps the precursor and a few readouts at that moment
// (resident size, GPU allocated, main-heap free space as last read every 300 frames, and how far from the system memory limit). At most 50 lines per session
- (void)applicationDidReceiveMemoryWarning:(UIApplication *)a {
    static int n; if (++n > 50) return;
    task_vm_info_data_t vi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT; unsigned long long fp = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) fp = vi.phys_footprint >> 20;
    unsigned long long h[3] = {0, 0, 0}, gpu = 0; void dm_guest_heap_last(unsigned long long *); unsigned long long mtl_gpu_allocated_mb(void);
    if (g_host_up) { dm_guest_heap_last(h); gpu = mtl_gpu_allocated_mb(); }
    plog("【内存告警】系统发来内存告警（本局第 %d 次，进程已活 %.0f 秒，applicationState=%ld）：常驻 %llu MB，显卡已分配 %llu MB，游戏堆 %llu MB 余量 %llu MB 最大可分配块 %llu KB，离系统内存上限还有 %llu MB",
         n, since_start() / 1000.0, (long)a.applicationState, fp, gpu, h[0], h[1], h[2], (unsigned long long)(os_proc_available_memory() >> 20));
}
- (void)application:(UIApplication *)a handleEventsForBackgroundURLSession:(NSString *)ident completionHandler:(void (^)(void))done {
    plog("boot: 系统交来后台下载会话 %s 的事件（applicationState=%ld，宿主%s）", ident.UTF8String, (long)a.applicationState, g_host_up ? "在跑" : "没起");
    if ([ident hasSuffix:@".bgtest"]) { plog("后台测试会话：接回"); (void)bg_test_session(); done(); return; }
    dm_asset_dl_bg_events(ident, done);   // 资源直接下载的后台会话（asset_dl.m） | Background session for direct asset download (asset_dl.m)
}
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    g_T[3] = since_start();
    if ([g_mode isEqualToString:@"blank"]) {   // 发热负对照：不锁屏，按 DM_HEAT 采样 | Heat negative control: do not lock the screen, sample per DM_HEAT
        UIApplication.sharedApplication.idleTimerDisabled = YES;
        uint64_t front_blank_presents(void); void front_ios_heat_start(uint64_t (*)(void), int); front_ios_heat_start(front_blank_presents, 0);
    }
    const char *us = getenv("DM_TEST_UISHOT");   // 只测试用：DM_TEST_UISHOT=<秒>:<名字>，到点把当前窗口存成 Documents/<名字>.png——资源界面、着色器准备页这类系统界面层，游戏内截图截不到 | Test only: DM_TEST_UISHOT=<seconds>:<name> saves the current window to Documents/<name>.png at that time -- UIKit-level screens such as the resource screen and the shader preparation page cannot be captured by the in-game screenshot
    if (us && atof(us) > 0 && strchr(us, ':')) { NSString *fn = @(strchr(us, ':') + 1); dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(atof(us) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        // 所有场景的所有窗口按层级从低到高画进同一张图（系统弹框不一定在主窗口里，只取主窗口会得到一张黑图）；
        // 另把最上层呈现的界面里每个文字标签的「实际高度 / 排完整段要的高度」记进日志——截图万一仍截不到，截没截断也有数可读
        // All windows of all scenes are drawn into one image from the lowest level to the highest (a system alert is not necessarily in the main window, and taking only the main window gives a black image);
        // in addition, for every text label in the topmost presented interface, "actual height / height needed to lay out the whole text" is written to the log - should the screenshot still fail to capture it, whether it is truncated can be read from numbers
        NSMutableArray<UIWindow *> *ws = [NSMutableArray array]; for (UIScene *sc in UIApplication.sharedApplication.connectedScenes) if ([sc isKindOfClass:UIWindowScene.class]) [ws addObjectsFromArray:((UIWindowScene *)sc).windows];
        [ws sortUsingComparator:^NSComparisonResult(UIWindow *x, UIWindow *y) { return x.windowLevel < y.windowLevel ? NSOrderedAscending : x.windowLevel > y.windowLevel ? NSOrderedDescending : NSOrderedSame; }];
        if (!ws.count) { plog("【红】DM_TEST_UISHOT：没有窗口"); return; }
        CGRect all = CGRectZero; for (UIWindow *x in ws) all = CGRectUnion(all, x.frame);
        UIImage *im = [[[UIGraphicsImageRenderer alloc] initWithBounds:all] imageWithActions:^(UIGraphicsImageRendererContext *c) { for (UIWindow *x in ws) if (!x.hidden) [x drawViewHierarchyInRect:x.frame afterScreenUpdates:YES]; }];
        for (UIWindow *x in ws) { UIViewController *top = x.rootViewController; while (top.presentedViewController) top = top.presentedViewController;
            plog("DM_TEST_UISHOT：窗口 %s 层级 %.0f %.0fx%.0f%s｜最上层 %s", NSStringFromClass(x.class).UTF8String, x.windowLevel, x.frame.size.width, x.frame.size.height, x.hidden ? "（隐藏）" : "", top ? NSStringFromClass(top.class).UTF8String : "无");
            if (top && top != x.rootViewController) { NSMutableArray<UIView *> *q = [NSMutableArray arrayWithObject:top.view];
                while (q.count) { UIView *v = q.firstObject; [q removeObjectAtIndex:0]; [q addObjectsFromArray:v.subviews];
                    if ([v isKindOfClass:UILabel.class] && ((UILabel *)v).text.length) { UILabel *l = (UILabel *)v; CGFloat need = [l sizeThatFits:CGSizeMake(l.bounds.size.width, CGFLOAT_MAX)].height; BOOL sc = NO; for (UIView *u = l.superview; u; u = u.superview) if ([u isKindOfClass:UIScrollView.class]) sc = YES;
                        plog("DM_TEST_UISHOT：标签 %.0fx%.0f 排完要高 %.0f 行数上限 %ld%s %s｜%.24s", l.bounds.size.width, l.bounds.size.height, need, (long)l.numberOfLines, sc ? " 在滚动区里" : "", need <= l.bounds.size.height + 0.5 ? "不截断" : "【红】高度不够", l.text.UTF8String); } } } }
        NSString *path = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0] stringByAppendingPathComponent:[fn.lastPathComponent stringByAppendingString:@".png"]];
        plog("DM_TEST_UISHOT：%s %s", path.fileSystemRepresentation, [UIImagePNGRepresentation(im) writeToFile:path atomically:YES] ? "已存" : "【红】没存下");
    }); }
    plog("dm iOS 模式 %s · %s %s", g_mode.UTF8String, UIDevice.currentDevice.systemName.UTF8String, UIDevice.currentDevice.systemVersion.UTF8String);
    plog("启动计时 T1 静态构造 %.1f ms · T2 main %.1f ms · T3 didFinishLaunching %.1f ms（零点 = 进程创建）", g_T[1], g_T[2], g_T[3]);
    if ([g_mode isEqualToString:@"boot"]) { plog("boot: 启动完成，宿主等场景到前台再起（applicationState=%ld）", (long)a.applicationState); bg_relaunch_test(); return YES; }   // 资源没到的另等 AssetVC 下完 | if the assets have not arrived, this waits separately for AssetVC to finish downloading
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ run_mode(); });
    return YES;
}
@end

int main(int argc, char *argv[]) {
    g_T[2] = since_start();
    // 文件描述符上限：iOS 进程缺省软上限 256，而游戏进局后常开约 255 个（数据文件、存档、日志、Metal 缓存…），差一个就满——
    // 满了以后任何 open 都失败：存档读不出（首页变成建角页）、Documents 里的文件读不到（NSPOSIXErrorDomain 24 Too many open files）。
    // 之前没撞上是因为从 devicectl 启动时系统有时给 2560。启动第一件事把软上限提到 4096（不超过硬上限）
    // File descriptor limit: an iOS process has a default soft limit of 256, while the game commonly keeps about 255 open once in a run (data files, saves, logs, Metal caches...), one short of full --
    // once full, every open fails: the save cannot be read (the home page turns into the character creation page), files in Documents cannot be read (NSPOSIXErrorDomain 24 Too many open files).
    // We did not hit this before because the system sometimes gives 2560 when launched from devicectl. The first thing at launch is to raise the soft limit to 4096 (not above the hard limit)
    { struct rlimit rl; if (!getrlimit(RLIMIT_NOFILE, &rl)) { rlim_t was = rl.rlim_cur, want = 4096; if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) want = rl.rlim_max;
        if (was < want) { rl.rlim_cur = want; int rc = setrlimit(RLIMIT_NOFILE, &rl); fprintf(stderr, "boot: 文件描述符软上限 %llu → %llu（%s）\n", (unsigned long long)was, (unsigned long long)want, rc ? "失败" : "已设"); g_nofile_was = (int)was; g_nofile_now = rc ? (int)was : (int)want; } else g_nofile_was = g_nofile_now = (int)was; } }
    g_mode = @"boot";   // 缺省 = 玩家点图标 = 起游戏。原缺省是早期探针 anon（匿名页写后执行），TestFlight 版点图标即被代码签名杀掉（崩溃日志 CODESIGNING Invalid Page）；探针须显式 -mode anon | Default = player taps the icon = start the game. The old default was the early probe anon (write to an anonymous page then execute); in the TestFlight build tapping the icon got it killed by code signing right away (crash log CODESIGNING Invalid Page); the probe now requires an explicit -mode anon
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "-mode")) g_mode = @(argv[i + 1]);
    // -mode thermal：轻探温——只读系统温控档位写一行就退出，不起宿主、不建窗口 / Metal 层（原先探温要拉起主菜单 60 帧渲染 20 s，探温本身在给手机加热）。
    // 输出到标准输出（devicectl launch --console 可见）并落 Documents/thermal.txt（整份原子替换，一行：档位 + 本次时刻）
    // -mode thermal: lightweight thermal probe; only reads the system thermal state, writes one line and exits, without starting the host or creating a window / Metal layer (the probe used to bring up the main menu rendering at 60 fps for 20 s, so the probe itself was heating the phone).
    // Output goes to stdout (visible with devicectl launch --console) and to Documents/thermal.txt (replaced atomically as a whole, one line: state + time of this run)
    if ([g_mode isEqualToString:@"thermal"]) {
        int th = (int)NSProcessInfo.processInfo.thermalState;
        NSString *d0 = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
        NSDateFormatter *df = [NSDateFormatter new]; df.dateFormat = @"yyyy-MM-dd HH:mm:ss ZZZ";   // 带上时刻：读的一方据此认「是这一次写的」，不会读到上一次的旧文件 | Include the time: the reader uses it to recognize "written this time" and never reads a stale file from last time
        NSString *line = [NSString stringWithFormat:@"温控档位 %d（轻探）时间 %@\n", th, [df stringFromDate:NSDate.date]];
        [line writeToFile:[d0 stringByAppendingPathComponent:@"thermal.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        fputs(line.UTF8String, stdout); fflush(stdout);
        return 0;
    }
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    const char *log_file = strdup([doc stringByAppendingPathComponent:[g_mode stringByAppendingString:@".txt"]].fileSystemRepresentation);
    // boot 模式先写 boot.launch.txt：进程可能是系统在没人操作时拉起来的（应用预热只跑到这个函数末尾的 UIApplicationMain 之前；后台下载事件会一路跑到
    // didFinishLaunching），那种启动不该动玩家上一局的日志。场景第一次到前台时才把上一局的 boot.txt 留成 boot.previous.txt、把本轮日志改名成 boot.txt（log_promote）。
    // boot mode writes boot.launch.txt first: the process may have been launched by the system with nobody using the device (app prewarming runs only up to
    // just before UIApplicationMain at the end of this function; a background download event runs all the way to
    // didFinishLaunching), and such a launch must not touch the log of the player's previous session. Only when the scene first comes to the foreground is
    // the previous session's boot.txt kept as boot.previous.txt and this launch's log renamed to boot.txt (log_promote).
    if ([g_mode isEqualToString:@"boot"]) log_file = strdup([doc stringByAppendingPathComponent:@"boot.launch.txt"].fileSystemRepresentation);
    g_log = open(log_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if ([g_mode isEqualToString:@"boot"]) {
        // 日志头：报问题时最先要知道的四样（哪个构建、什么机器、什么系统、什么时候）
        // Log header: the four things you need first when a problem is reported (which build, what machine, what OS, when)
        { char hw[64] = "未知"; size_t hn = sizeof hw; sysctlbyname("hw.machine", hw, &hn, NULL, 0);
          NSDictionary *inf = NSBundle.mainBundle.infoDictionary; NSDateFormatter *df = [NSDateFormatter new]; df.dateFormat = @"yyyy-MM-dd HH:mm:ss ZZZ";
          plog("boot: 版本 %s 构建 %s 机型 %s 系统 %s 时间 %s", [inf[@"CFBundleShortVersionString"] description].UTF8String ?: "未知", [inf[@"CFBundleVersion"] description].UTF8String ?: "未知",
               hw, NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String, [df stringFromDate:NSDate.date].UTF8String);
        }
        // 跨提交附件有效性账本的违规记录落 Documents：触发过的键以后每次启动都不再做（玩家最多见一次），也是回收现场的途径
        // Violation records of the cross-commit attachment validity ledger go to Documents: a key that has triggered is never done again on any later launch (the player sees it at most once); it is also how the diagnostic record gets collected
        setenv("DM_XV_FILE", [doc stringByAppendingPathComponent:@"xv_violation.txt"].fileSystemRepresentation, 0);
        const char *log_port = getenv("DM_LOG_PORT");
        strlcpy(g_log_path, log_file, sizeof g_log_path);
        if ((getenv("DM_LOG_TEE") || log_port) && !pipe(g_log_pipe) && (!getenv("DM_LOG_TEE") || (g_log_stderr = dup(2)) >= 0) &&
            !pthread_create(&g_log_thread, NULL, log_tee_main, NULL)) {
            g_log_tee = 1;
            dup2(g_log_pipe[1], 2);
            close(g_log_pipe[1]);
            g_log_pipe[1] = -1;
            pthread_t lp; if (log_port && !pthread_create(&lp, NULL, log_port_main, (void *)(intptr_t)atoi(log_port))) pthread_detach(lp);
        } else {
            dup2(g_log, 2);                           // 宿主 LOG/die 写 stderr → 同一日志文件（提前装载线程也写） | Host LOG/die write to stderr → the same log file (the early-load thread writes there too)
        }
        metal_cache_report_and_clean();
        char why[2048];
        { id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); g_no_bc = !dev.supportsBCTextureCompression || getenv("DM_TEST_NO_BC");
          // 图形层要 Metal 3（参数缓冲里直接写 gpuAddress 的无绑定用法）：A14 / M1（Apple7 家族）起才有，A13 及更早不支持。不支持就不起游戏、弹框说明（不再走到解包 / 建管线时闪退）。
          // 测试开关 DM_TEST_NO_METAL3=1 当成不支持
          // The graphics layer needs Metal 3 (bindless use that writes gpuAddress directly into argument buffers): available from A14 / M1 (the Apple7 family); A13 and earlier do not support it. If unsupported, do not start the game and show a message (instead of crashing later while unpacking / building pipelines).
          // Test switch DM_TEST_NO_METAL3=1 treats the device as unsupported
          if (![dev supportsFamily:MTLGPUFamilyMetal3] || getenv("DM_TEST_NO_METAL3")) {
              g_block_title = T("dmGpuOldTitle"); g_block_msg = T("dmGpuOldMsg"); g_block_why = @"图形芯片不支持 Metal 3";
              plog("boot: 🔴 显卡 %s 不支持 Metal 3%s——不起游戏，弹框说明", dev.name.UTF8String, getenv("DM_TEST_NO_METAL3") ? "（测试开关 DM_TEST_NO_METAL3）" : ""); }
          plog("boot: 显卡 %s，BC 压缩格式%s", dev.name.UTF8String, g_no_bc ? (getenv("DM_TEST_NO_BC") ? "按测试开关 DM_TEST_NO_BC 当成不支持" : "不支持——不起游戏，弹框说明后退出") : "支持"); }
        if (g_no_bc) {   // 不支持 BC：有配套的老机型补充包就照常起（覆盖层换 ASTC 版，见 boot_prepare），没有就弹框 | BC not supported: if the matching Legacy Device Pack is present, start as usual (the overlay switches to the ASTC version, see boot_prepare); otherwise show an alert
            NSString *d = astc_pack_locate(g_astc_why, sizeof g_astc_why);
            if (d) { g_astc_dir = d; g_no_bc = 0; plog("boot: 用老机型补充包 %s", d.UTF8String); }
            else plog("boot: 老机型补充包未就绪：%s", g_astc_why);
        }
        NSString *pb = g_block_title ? nil : pack_check();
        if (pb) { g_block_title = T("dmPackBadTitle"); g_block_msg = [NSString stringWithFormat:@"%@\n\n%@", pb, T("dmPackBadHow")]; g_block_why = @"安装包自检没过"; }
        if (g_block_title) {}   // 起不了游戏：不起宿主，场景里弹框（BlockVC） | The game cannot start: do not start the host; show a message in the scene (BlockVC)
        else if (g_no_bc) {}
        else if (getenv("DM_ASSETS_TEST") || getenv("DM_TEST_BA")) { g_need_assets = 1; plog("boot: DM_ASSETS_TEST——强制走资源下载界面再就地起宿主（测这条路径）"); }
        else if (romfs_locate(why, sizeof why)) plog("boot: 游戏资源就绪，宿主等场景到前台再装载");
        else { g_need_assets = 1; plog("boot: 游戏资源还没到（%s），先显示资源下载界面", why); }
    } else metal_cache_report_and_clean();
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(App.class)); }
}
