// 引导：装模块后，照 SDK 里 nn::init::Start（sdk+0x2b0470）的真实顺序初始化 SDK 运行时，再进 nnMain。
// Bootstrap: after loading the modules, initialize the SDK runtime in the real order of nn::init::Start (sdk+0x2b0470) in the SDK, then enter nnMain.
#include "dm.h"
#include "front.h"
#include <stdatomic.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <sys/time.h>
#include <unistd.h>
void probe_install(void); void ticks_install(void); void hid_keys_init(void); void tact_prefetch_start(const char *outdir); void tact_chk_init(void); void stubs_report(void); void crash_install(void); void guest_thread_init(void); uint32_t svc_thread_main_init(void);
typedef void (*fn0)(void); typedef void (*fn2)(uint64_t, uint64_t); typedef void (*fnp)(void *);
static void *req(const char *n){ void *p=guest_sym(n); if(!p) die("缺符号 %s", n); return p; }

// 游戏参数：argv[0] 固定为 "D2R"，"--" 之后的原样追加（真机由 nn::init 从进程参数区取，经 SetHostArgc/Argv 交给 SDK）
// Game arguments: argv[0] is fixed to "D2R", and whatever follows "--" is appended as is (on a real device nn::init takes them from the process argument area and hands them to the SDK through SetHostArgc/Argv)
static char *g_argv[64] = {"D2R"}; static int g_argc = 1;

void crash_sample_self(void);
// 存档落盘节流：游戏把脏了的存档容器写盘并提交的「非强制刷新」每拍调一次（main+0x2497ac → main+0xdff8a4，强制 = 0），
// 但写档入口 main+0xdfdf80 对同一容器 30 秒只放行一次（常数在 main+0x20cefa8；新容器的时间戳从建档那一刻起算）——
// 新建角色因此要约 30 秒后才第一次提交，这之前进程被结束（手机上切出去被系统回收是常态）角色就没了。改成 3 秒。
// 受影响的只有非强制写（新角色、键位、改名）；局内角色存盘、设置等本来就是强制写，不走节流。DM_SAVE_THROTTLE=秒 可改（30 = 原版）。
// Save flush throttling: the "non-forced flush" in which the game writes a dirty save container to disk and commits it is called once per tick (main+0x2497ac → main+0xdff8a4, forced = 0),
// but the save-write entry main+0xdfdf80 lets the same container through only once per 30 seconds (the constant is at main+0x20cefa8; a new container's timestamp counts from the moment the save is created) --
// so a newly created character is first committed only after about 30 seconds, and if the process is ended before that (being reclaimed by the system after switching away is normal on a phone) the character is gone. Changed to 3 seconds.
// Only non-forced writes are affected (new character, key bindings, rename); in-run character saves, settings etc. are forced writes anyway and do not go through the throttle. DM_SAVE_THROTTLE=seconds can change it (30 = the original game).
static void save_throttle_patch(void) {
    const char *v = getenv("DM_SAVE_THROTTLE"); int sec = v ? atoi(v) : 3;
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) return;
    int32_t *p = (int32_t *)(m->base + 0x20cefa8);
    if (*p != 30) { LOG("【红】存档落盘节流：main+0x20cefa8 原值 %d ≠ 30（构建号不符？），不改", *p); return; }
    if (sec == 30) return;
    *p = sec < 1 ? 1 : sec;
    LOG("存档落盘节流：main+0x20cefa8 30 → %d 秒", *p);
}
// DM_GAME_SEED=N：游戏自带的固定种子开关（main 数据段 0x20a0074，出厂 0xffffffff = 关）。
// 建局 0x5d0bd0 在 0x5d297c 处判它 ≠ -1 则以 {N, 666} 作种子（存 Game+0x1b8），否则取系统时钟。数据段直接写、不剪口；
// 放在各模块 init_array 之后、nnMain 之前，免得全局构造覆盖。写前断言原值，构建号不符就报红中止。
// DM_GAME_SEED=N: the game's own fixed-seed switch (main data segment 0x20a0074, factory value 0xffffffff = off).
// Game creation 0x5d0bd0 checks at 0x5d297c whether it ≠ -1 and then uses {N, 666} as the seed (stored at Game+0x1b8), otherwise takes the system clock. Written directly into the data segment, no cut;
// placed after each module's init_array and before nnMain, so that global constructors do not overwrite it. The original value is asserted before writing, and a build ID mismatch reports a failure and aborts.
static void game_seed_maybe(void) {
    const char *v = getenv("DM_GAME_SEED");
    if (!v) return;
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) die("DM_GAME_SEED：没有 main 模块");
    uint32_t *p = (uint32_t *)(m->base + 0x20a0074);
    if (*p != 0xffffffffu) die("DM_GAME_SEED：main+0x20a0074 原值 %#x ≠ 0xffffffff（构建号不符？）", *p);
    *p = (uint32_t)strtoull(v, NULL, 0);
    LOG("DM_GAME_SEED：main+0x20a0074 ← %#x", *p);
}

static void guest_main(void) {
    guest_thread_init();
    { void inflate_bench_maybe(void); inflate_bench_maybe(); }   // DM_INFLATE_BENCH 测试模式（诊断用，跑完退出） | DM_INFLATE_BENCH test mode (for diagnostics, exits when done)
    crash_sample_self();   // DM_SAMPLE 采样（诊断用） | DM_SAMPLE sampling (for diagnostics)
    uint32_t hmain = svc_thread_main_init();   // 主线程句柄：nnosInitialize 的 x0（存入 sdk 全局，ThreadManagerImplByHorizon 取用）；x1 = 参数区指针 | Main thread handle: x0 of nnosInitialize (stored in an sdk global, used by ThreadManagerImplByHorizon); x1 = pointer to the argument area
    // 逐条照抄 Start：_init_libc1 = __nnmusl_init_locks（把 stdio 等锁初始化成递归锁，略过会在 putc 嵌套加锁时自锁死）+ __init_ssp。
    // rtld 传入的三个回调：A 只置 rtld 标志（略）、B = 各模块 DT_INIT（nnMain 前）、C = 析构（不做）
    // Copy Start step by step: _init_libc1 = __nnmusl_init_locks (initializes the stdio etc. locks as recursive locks; skipping it self-deadlocks on nested locking in putc) + __init_ssp.
    // The three callbacks passed in by rtld: A only sets an rtld flag (skipped), B = each module's DT_INIT (before nnMain), C = destructors (not done)
    ((fn0)req("_init_libc0"))();
    LOG(">>> nnosInitialize"); ((fn2)req("nnosInitialize"))(hmain, 0);
    ((fn0)req("_init_libc1"))();
    LOG(">>> nninitInitializeSdkModule"); ((fn0)req("nninitInitializeSdkModule"))();
    LOG(">>> nninitStartup"); ((fn0)req("nninitStartup"))();
    ((fn0)req("_init_libc2"))();
    dm_prof_mark("② SDK 初始化（nnosInitialize → nninitStartup）");
    run_module_inits();   // rtld 回调 B：各模块 DT_INIT（全局构造函数） | rtld callback B: each module's DT_INIT (global constructors)
    dm_prof_mark("③ 各模块 init_array");
    ((fnp)req("_ZN2nn2os11SetHostArgcEi"))((void *)(uint64_t)g_argc);
    ((fnp)req("_ZN2nn2os11SetHostArgvEPPc"))(g_argv);
    game_seed_maybe(); save_throttle_patch();
    { void dm_guest_spawn_selftest(void); dm_guest_spawn_selftest(); }   // DM_GUEST_SPAWN_TEST 宿主客体线程自检（hle_guestthread.c） | DM_GUEST_SPAWN_TEST self-test of host-created guest threads (hle_guestthread.c)
    { void dm_padmode_boot(void); dm_padmode_boot(); }   // 触屏覆盖层在：布局前缀改指触屏版（dm_padmode.c），要在游戏建面板之前 | Touch overlay present: repoint the layout prefixes to the touch version (dm_padmode.c); must happen before the game builds its panels
    LOG(">>> nnMain"); ((fn0)req("nnMain"))();
    LOG("nnMain returned");
}

struct dm_flags g_flag;
uint64_t g_t0_ns;
static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"); g_flag.svc = !!getenv("DM_SVC"); g_flag.fs = !!getenv("DM_FS");
    g_flag.movie = !!getenv("DM_MOVIE"); g_flag.dbg = !!getenv("DM_DBG"); g_flag.imports = !!getenv("DM_IMPORTS");
    g_flag.bootprof = !!getenv("DM_BOOTPROF");
}

// 启动时间线零点 = 进程创建时刻（sysctl KERN_PROC_PID 的 p_starttime，墙钟）换算到 now_ns 单调时基；Mac / iOS 同口径
// Zero point of the boot timeline = process creation time (p_starttime from sysctl KERN_PROC_PID, wall clock) converted to the now_ns monotonic time base; same definition on Mac / iOS
uint64_t g_proc_start_ns;
static uint64_t proc_start_mono_ns(void) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user, current;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) die("GetProcessTimes failed");
    uint64_t mono = now_ns();
    GetSystemTimePreciseAsFileTime(&current);
    uint64_t start = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
    uint64_t now = ((uint64_t)current.dwHighDateTime << 32) | current.dwLowDateTime;
    uint64_t age = now > start ? (now - start) * 100 : 0;
    return age < mono ? mono - age : 0;
#elif !defined(__APPLE__)
    // 安卓 App 用 Java 交来的启动时刻：App 进程可能是系统预先分叉备用的（USAP 池），/proc/self/stat 记的是那次分叉，可早于本次启动数小时
    // The Android App uses the start time handed over from Java: the App process may have been forked ahead of time by the system (USAP pool), and /proc/self/stat records that fork, which can be hours before this launch
    if (g_proc_start_ns) return g_proc_start_ns;
    // Linux：/proc/self/stat 第 22 字段 = 进程创建时刻（开机以来的时钟滴答），与 CLOCK_BOOTTIME 同一基准
    // Linux: field 22 of /proc/self/stat = process creation time (clock ticks since boot), on the same basis as CLOCK_BOOTTIME
    uint64_t mono = now_ns(); char buf[1024] = ""; FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return mono;
    size_t got = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[got] = 0;
    const char *p = strrchr(buf, ')'); unsigned long long st = 0;
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu", &st) != 1) return mono;
    long hz = sysconf(_SC_CLK_TCK); struct timespec bt; clock_gettime(CLOCK_BOOTTIME, &bt);
    int64_t age = (int64_t)bt.tv_sec * 1000000000ll + bt.tv_nsec - (int64_t)(st * 1000000000ull / (unsigned long long)(hz > 0 ? hz : 100));
    return age > 0 ? mono - (uint64_t)age : mono;
#else
    struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    uint64_t mono = now_ns(); struct timeval tv; gettimeofday(&tv, NULL);
    if (sysctl(mib, 4, &kp, &n, NULL, 0) || !n) return mono;
    int64_t age = (int64_t)(tv.tv_sec - kp.kp_proc.p_starttime.tv_sec) * 1000000000ll + (tv.tv_usec - kp.kp_proc.p_starttime.tv_usec) * 1000ll;
    return age > 0 ? mono - (uint64_t)age : mono;
#endif
}

// 宿主装载（与 UIKit / Metal 无关）：读 NSO、重定位、强制接管、探针 / 拍门 / 按键、预取起跑。缺省在 main 里同步跑；
// iOS 前端可在 App 的 main() 一开头调 dm_host_preload_start(同一组参数) 让它在后台线程先跑，与 UIKit 启动链并行（⓪ + ① ≈ max(UIKit 链, 宿主装载)）。
// Host loading (independent of UIKit / Metal): read the NSO, relocate, forced take-over, probes / tick gate / keys, start prefetch. By default it runs synchronously in main;
// the iOS front end can call dm_host_preload_start(same arguments) at the very start of the App's main() so that it runs first on a background thread, in parallel with the UIKit startup chain (⓪ + ① ≈ max(UIKit chain, host loading)).
static pthread_key_t g_race_keys[512];
static unsigned g_race_n, g_race_want;
static void *keyrace_thread(void *unused);
static void host_preload(int argc, char **argv) {
    g_t0_ns = proc_start_mono_ns();
    flags_init();
    dm_prof_mark("⓪ 进程创建 → 宿主 main（dyld）");
    if (argc < 2) die("usage: dmhost <dir> [mod...] [-- 游戏参数...]");
    // 时区：游戏把绝对时刻换成本地时刻走两条路，都落到宿主 libc——nn::time::ToCalendarTime（hle_time.c，localtime_r）和客体 libc 的
    // localtime_r / mktime（hle_table.c 直连宿主）。所以宿主跟系统时区走，原版界面（编年史发现时刻等）才是本机时间。
    // 只有确定性对拍（DM_FIXED_TIME）钉成 UTC：同一份回放在本机（东八区）和 mini（东九区）上要换出同样的日历时刻
    // Time zone: the game converts absolute time to local time along two paths, both ending in the host libc -- nn::time::ToCalendarTime (hle_time.c, localtime_r) and the guest libc's
    // localtime_r / mktime (hle_table.c wires them straight to the host). So only when the host follows the system time zone do the original game's screens (chronicle discovery time etc.) show this machine's time.
    // Only deterministic comparison (DM_FIXED_TIME) pins it to UTC: the same replay must convert to the same calendar time on this machine (UTC+8) and on the second Mac (UTC+9)
    if (getenv("DM_FIXED_TIME")) setenv("TZ", "UTC", 1);
    crash_install();
    const char *race = getenv("DM_TEST_KEYRACE");
    if (race && *race) {
        g_race_want = (unsigned)strtoul(race, NULL, 10);
        if (g_race_want > 512) g_race_want = 512;
        pthread_t th;
        if (pthread_create(&th, NULL, keyrace_thread, NULL)) die("DM_TEST_KEYRACE 起线程失败");
        pthread_detach(th);
    }
    { void dm_term_install(void); dm_term_install(); }   // SIGTERM 优雅退出（hle_lifecycle.c） | SIGTERM graceful exit (hle_lifecycle.c)
    { void crash_set_dump_hook(void (*)(void)); void svc_thread_dump(void); crash_set_dump_hook(svc_thread_dump); }   // 崩溃时导出客体线程现场 | Dump guest thread state on a crash
    const char *names[MAX_MOD]; int n = 0;
    int i = 2;
    for (; i < argc && strcmp(argv[i], "--") && n < MAX_MOD; i++) names[n++] = argv[i];
    if (i < argc && !strcmp(argv[i], "--")) for (i++; i < argc && g_argc < 63; i++) g_argv[g_argc++] = argv[i];
    if (!n) { names[n++] = "main"; names[n++] = "sdk"; }
    load_modules(argv[1], names, n);
    uint64_t ta = now_ns();
    probe_install();   // DM_PROBE 探针（诊断用） | DM_PROBE probes (for diagnostics)
    ticks_install();   // DM_TICKS 逐拍状态哈希（拍入口观测门） | DM_TICKS per-tick state hash (tick-entry observation gate)
    { void target_rank_install(void); target_rank_install(); }   // 触屏版自动选怪 Boss > 精英 > 普通（hle_target.c） | Touch version auto-targeting Boss > elite > normal (hle_target.c)
    hid_keys_init();   // DM_KEYS 脚本输入（按 UI 消息驱动的步骤要在消息出现前订阅） | DM_KEYS scripted input (steps driven by UI messages must subscribe before the message appears)
    uint64_t tb = now_ns();
    tact_chk_init();                // DM_TACTCHK 逐块记录（到选角停记） | DM_TACTCHK per-block log (logging stops at character select)
    tact_prefetch_start(argv[1]);   // 启动块后台预解码（缺省开：产物目录 bootlist.tsv；DM_TACT_PREFETCH=0 关） | Background pre-decode of boot blocks (on by default: bootlist.tsv in the build output directory; DM_TACT_PREFETCH=0 turns it off)
    uint64_t tc = now_ns();
    stubs_report();
    if (g_flag.bootprof) LOG("① 细分（装载后）：探针 / 拍门 / 按键 %.1f ms、预取启动（读清单）%.1f ms、桩报告 %.1f ms", (tb - ta) / 1e6, (tc - tb) / 1e6, (now_ns() - tc) / 1e6);
    dm_prof_mark("① 宿主装载（读 NSO / 重定位 / 强制接管）");
    { void loader_hash_log(void); loader_hash_log(); }   // DM_LOADHASH=1：装载结果的地址无关哈希（loader.c） | DM_LOADHASH=1: address-independent hash of the load result (loader.c)
}
static struct { int argc; char **argv; pthread_t th; _Atomic int started; } g_pre;
static void *preload_thread(void *a) { (void)a; host_preload(g_pre.argc, g_pre.argv); return NULL; }
static void *keyrace_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < g_race_want; i++) if (pthread_key_create(&g_race_keys[g_race_n++], NULL)) break;
    return NULL;
}
// 提前起跑宿主装载。调用时机：进程里第一件事之一（iOS：App 的 main() 进 UIApplicationMain 之前），须先设好装载要用的环境变量
// （DM_ROMFS / DM_SAVE / DM_TEMP 等与 cwd——装载与预取起跑读它们）；参数须与随后 dm_main 的参数逐项相同（不同即报红退出）。
// 只能调一次；不调则 main 内同步装载，行为与调用前完全一致。不依赖 UIKit / Metal。
// Start host loading early. When to call: as one of the first things in the process (iOS: in the App's main() before entering UIApplicationMain); the environment variables loading needs must already be set
// (DM_ROMFS / DM_SAVE / DM_TEMP etc. and the cwd -- loading and the prefetch start read them); the arguments must be identical, item by item, to those of the later dm_main (a difference reports a failure and exits).
// May be called only once; if not called, loading is synchronous inside main and behavior is exactly as before this call existed. Does not depend on UIKit / Metal.
void dm_host_preload_start(int argc, char **argv) {
    if (atomic_exchange(&g_pre.started, 1)) return;
    g_pre.argc = argc; g_pre.argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) g_pre.argv[i] = strdup(argv[i]);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    if (pthread_create(&g_pre.th, &a, preload_thread, NULL)) die("dm_host_preload_start：起线程失败");
}
#include <TargetConditionals.h>
int dm_app_bootstrap(int *argc, char ***argv);   // app_mac.m：包成 .app 双击起时备好环境与参数；命令行起原样 | app_mac.m: when packaged as an .app and started by double-click, prepare the environment and arguments; command-line starts are unchanged
int main(int argc, char **argv) {
#if TARGET_OS_OSX   // iOS 包也编这个文件（入口由外壳调），那边没有 app_mac.m | The iOS package compiles this file too (the entry is called by the shell); there is no app_mac.m on that side
    if (dm_app_bootstrap(&argc, &argv)) { void dm_cloud_boot(void); dm_cloud_boot(); }   // App 形态：挂存档之前先做一次 iCloud 同步（同 iOS 壳的启动次序）；取不到容器 / 测试存档时它自己记一行就返回 | App form: do one iCloud sync before mounting the save (same startup order as the iOS shell); when the container cannot be obtained / with a test save it logs one line and returns
#endif
    if (getenv("DM_PRELOAD")) dm_host_preload_start(argc, argv);   // Mac 上验证提前起跑的路径（iOS 由 App 的 main() 调） | Exercise the early-start path on Mac (on iOS it is called from the App's main())
    if (atomic_load(&g_pre.started)) {
        pthread_join(g_pre.th, NULL);
        int same = argc == g_pre.argc; for (int i = 0; same && i < argc; i++) same = !strcmp(argv[i], g_pre.argv[i]);
        if (!same) die("【红】dm_host_preload_start 的参数与 main 不同");
        void crash_thread_init(void); crash_thread_init();   // 信号备用栈是每线程的：装载在别的线程上装过，本线程补上 | The signal alternate stack is per thread: loading installed it on another thread, so add it for this thread
        LOG("宿主装载已在后台提前完成（dm_host_preload_start）");
    } else host_preload(argc, argv);
    front_run(guest_main);
    return 0;
}
