// 诊断：探针 DM_PROBE、UI 消息总线 DM_UIMSG、主线程采样 DM_SAMPLE。从 crash.c 拆出（crash.c 只管信号处理）。
// Diagnostics: probes DM_PROBE, UI message bus DM_UIMSG, main-thread sampling DM_SAMPLE. Split out of crash.c (crash.c only handles signals).
#include "dm_prof.h"
#include "dm.h"
#include "dm_uctx.h"
#include "dm_hud.h"
#include "dm_cursor.h"
#include "dm_hud_exp.h"
#include "dm_elysium.h"
#include "dm_bossrush.h"
#include <TargetConditionals.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdatomic.h>
#include "dm_i18n.h"
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <malloc/malloc.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
void crash_off(const char *tag, uint64_t a);
#define off crash_off
void dm_backtrace(uint64_t fp);
uint64_t dm_mtl_l3_sample(uint32_t tick) __attribute__((weak));
void dm_mtl_l3_tick(uint32_t tick) __attribute__((weak));

// 句柄数：数 /dev/fd 目录（代价 ∝ 实际打开数）。iOS SDK 没有 libproc / sys/proc_info.h（真机冒烟构建失败抓到）。
// 不能逐个探测到 getdtablesize()：宿主上限 1048576，一次 5 s CPU、每 250 拍一次，所有宿主局后台常占半核（inv_tap 负载偶发红时查到）。
// /dev/fd 打不开（沙盒）才退回 fcntl 探测，封顶 4096；方法首次打一行
// Handle count: count the /dev/fd directory (cost ∝ number actually open). The iOS SDK has no libproc / sys/proc_info.h (caught by a failed real device smoke test build).
// Cannot probe one by one up to getdtablesize(): the host limit is 1048576, 5 s of CPU each time, once every 250 ticks, so every host run kept half a core busy in the background (found while investigating intermittent inv_tap failures under load).
// Only when /dev/fd cannot be opened (sandbox) fall back to fcntl probing, capped at 4096; the method is printed once on first use
static int l3_fd_count(void) {
    static int said; int n = 0;
    DIR *d = opendir("/dev/fd");
    if (d) {
        int self = dirfd(d); struct dirent *e;
        while ((e = readdir(d))) if (e->d_name[0] != '.' && atoi(e->d_name) != self) n++;
        closedir(d);
        if (!said++) LOG("L3MEM 句柄计数：/dev/fd 目录");
        return n;
    }
    int top = getdtablesize(); if (top > 4096) top = 4096;
    for (int fd = 0; fd < top; fd++) if (fcntl(fd, F_GETFD) != -1) n++;
    if (!said++) LOG("L3MEM 句柄计数：/dev/fd 打不开，fcntl 探测前 %d 个", top);
    return n;
}

static pthread_mutex_t g_l3mem_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_l3mem_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_l3mem_thread;
static int g_l3mem_thread_started;
static int g_l3mem_pending;
static uint32_t g_l3mem_pending_tick;
static uint64_t g_l3mem_seq;

static void l3mem_report(uint32_t tick, uint64_t seq) {
    uint64_t started = now_ns();
    malloc_statistics_t ms;
    task_vm_info_data_t vi;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    uint64_t t = now_ns();
    malloc_zone_statistics(NULL, &ms);
    uint64_t malloc_ns = now_ns() - t;
    t = now_ns();
    uint64_t metal = dm_mtl_l3_sample ? dm_mtl_l3_sample(tick) : 0;
    uint64_t metal_ns = now_ns() - t;
    t = now_ns();
    int handles = l3_fd_count();
    uint64_t fd_ns = now_ns() - t;
    uint64_t footprint = 0;
    t = now_ns();
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &count) == KERN_SUCCESS)
        footprint = vi.phys_footprint;
    uint64_t task_ns = now_ns() - t;
    LOG("[dm] L3MEM 拍 %u 堆在用 %llu Metal已分配 %llu 句柄 %d phys_footprint %llu 耗时_ns %llu malloc_ns %llu metal_ns %llu fd_ns %llu task_ns %llu 线程 dm-l3mem 序号 %llu",
        tick,
        (unsigned long long)ms.size_in_use, (unsigned long long)metal, handles,
        (unsigned long long)footprint, (unsigned long long)(now_ns() - started),
        (unsigned long long)malloc_ns, (unsigned long long)metal_ns,
        (unsigned long long)fd_ns, (unsigned long long)task_ns,
        (unsigned long long)seq);
}

static void *l3mem_main(void *arg) {
    (void)arg;
#if defined(__APPLE__)
    pthread_setname_np("dm-l3mem");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
    for (;;) {
        pthread_mutex_lock(&g_l3mem_mu);
        while (!g_l3mem_pending) pthread_cond_wait(&g_l3mem_cv, &g_l3mem_mu);
        uint32_t tick = g_l3mem_pending_tick;
        uint64_t seq = ++g_l3mem_seq;
        g_l3mem_pending = 0;
        pthread_mutex_unlock(&g_l3mem_mu);
        l3mem_report(tick, seq);
    }
    return NULL;
}

static void l3mem_request(uint32_t tick) {
    pthread_mutex_lock(&g_l3mem_mu);
    if (!g_l3mem_thread_started) {
        if (pthread_create(&g_l3mem_thread, NULL, l3mem_main, NULL)) {
            pthread_mutex_unlock(&g_l3mem_mu);
            LOG("【红】L3MEM 采样线程创建失败");
            return;
        }
        pthread_detach(g_l3mem_thread);
        g_l3mem_thread_started = 1;
    }
    g_l3mem_pending_tick = tick;
    g_l3mem_pending = 1;
    pthread_cond_signal(&g_l3mem_cv);
    pthread_mutex_unlock(&g_l3mem_mu);
}

// ---------- 探针 DM_PROBE=模块:偏移[,模块:偏移...]（Mac 专用开发诊断）----------
// 运行时改代码页（mprotect 可写 → 写 brk → 改回可执行）并执行运行时写出的跳板：只限 Mac 开发调试，iOS 不可用（签名代码页不可写、
// 运行时指令页不可执行）、不进成品。成品诊断走改写器离线插的观测门（OBS 表 → gate_obs_set），如下方 DM_UIMSG。
// ---------- Probes DM_PROBE=module:offset[,module:offset...] (Mac-only development diagnostics) ----------
// Modifies code pages at runtime (mprotect writable → write brk → back to executable) and executes trampolines written at runtime: Mac development debugging only, unavailable on iOS (signed code pages are not writable,
// runtime instruction pages are not executable), and not part of the shipped product. Product diagnostics go through observation gates inserted offline by the rewriter (OBS table → gate_obs_set), such as DM_UIMSG below.
#if !TARGET_OS_IPHONE
// 写法 模块:偏移[@x0 偏移]。在客体地址换上 brk，命中时打印 x0..x3（及其指向的可读字符串）与回溯，原指令放进跳板槽执行后跳回。
// 跳板借用 x16：只在函数入口或 x16 已死的点下探针；原指令不能是 PC 相对类（adr/adrp/b/bl/ldr 字面量/cbz…）。
// Syntax module:offset[@x0 offset]. Put a brk at the guest address; on hit print x0..x3 (and the readable strings they point to) and a backtrace, then execute the original instruction from a trampoline slot and jump back.
// The trampoline borrows x16: place probes only at function entries or at points where x16 is dead; the original instruction must not be PC-relative (adr/adrp/b/bl/ldr literal/cbz...).
#define MAX_PROBE 16
typedef void (*probe_fn)(ucontext_t *uc);
static struct { uint64_t at; uint32_t *slot; unsigned hits; char name[40]; long dump; probe_fn fn; } g_probe[MAX_PROBE];   // dump：命中时打印 [x0+dump] 16 字节（-1 = 不打）；fn：专用处理（替代缺省打印） | dump: on hit print 16 bytes at [x0+dump] (-1 = do not print); fn: dedicated handler (replaces the default print)
static int g_nprobe;
static void probe_str(uint64_t p) {
    char buf[80]; vm_size_t got = 0;
    if (p < 0x1000 || vm_read_overwrite(mach_task_self(), p, sizeof buf - 1, (vm_address_t)buf, &got) || !got) return;
    buf[got] = 0; int n = 0; while (buf[n] >= 0x20 && buf[n] < 0x7f) n++;
    if (n >= 3 && (buf[n] == 0 || n == (int)got)) { fprintf(stderr, " \"%.*s\"", n, buf); return; }
    // 游戏字符串对象 {char* 指针, u64 长度, u64 容量|最高位}（0xdf67f0 FJSONUtils 的路径参数即此布局）
    // Game string object {char* pointer, u64 length, u64 capacity|top bit} (the path argument of 0xdf67f0 FJSONUtils has exactly this layout)
    if (got >= 24) {
        uint64_t ptr, len; memcpy(&ptr, buf, 8); memcpy(&len, buf + 8, 8);
        char t[96]; got = 0;
        if (len >= 3 && len < sizeof t && !vm_read_overwrite(mach_task_self(), ptr, len, (vm_address_t)t, &got) && got == len) {
            int k = 0; while (k < (int)len && t[k] >= 0x20 && t[k] < 0x7f) k++;
            if (k == (int)len) fprintf(stderr, " S\"%.*s\"", (int)len, t);
        }
    }
}
// DM_PROBE_DUMP=模块:偏移:长度[:文件]：任一探针首次命中时把该段客体内存写成文件（缺省 run/platform/probe_dump.bin）
// DM_PROBE_DUMP=module:offset:length[:file]: on the first hit of any probe, write that range of guest memory to a file (default run/platform/probe_dump.bin)
static void probe_dump_once(void) {
    static int done; const char *e = getenv("DM_PROBE_DUMP"); if (done || !e) return; done = 1;
    char spec[512]; strlcpy(spec, e, sizeof spec);
    char *mod = strtok(spec, ":"), *o = strtok(NULL, ":"), *n = strtok(NULL, ":"), *f = strtok(NULL, ":");
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (mod && !strcmp(g_mod[i].name, mod)) m = &g_mod[i];
    if (!m || !o || !n) { LOG("DM_PROBE_DUMP 格式：模块:偏移:长度[:文件]"); return; }
    char def[512]; snprintf(def, sizeof def, "%s%s/run/platform/probe_dump.bin", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work");
    FILE *fp = fopen(f ? f : def, "wb"); if (!fp) return;
    fwrite(m->base + strtoull(o, 0, 16), 1, strtoull(n, 0, 16), fp); fclose(fp);
    LOG("探针转储 %s+%s 长 %s → %s", mod, o, n, f ? f : def);
}
static int probe_hit(ucontext_t *uc) {   // 由 probe_install 注册给 crash.c 的 SIGTRAP 分支 | Registered by probe_install with the SIGTRAP branch of crash.c
    uint64_t pc = UC_PC(uc);
    for (int i = 0; i < g_nprobe; i++) if (g_probe[i].at == pc) {
        probe_dump_once();
        if (g_probe[i].fn) { g_probe[i].hits++; g_probe[i].fn(uc); UC_PC(uc) = (uint64_t)g_probe[i].slot; return 1; }
        { extern uint64_t g_t0_ns; uint64_t now_ns(void); fprintf(stderr, "[dm] PROBE %s #%u [%.4fs]", g_probe[i].name, ++g_probe[i].hits, (now_ns() - g_t0_ns) / 1e9); }
        for (int r = 0; r < 4; r++) { uint64_t v = UC_X(uc, r); fprintf(stderr, " x%d=%#llx", r, v); probe_str(v); }
        for (int r = 19; r < 24; r++) fprintf(stderr, " x%d=%#llx", r, UC_X(uc, r));
        { uint64_t x8 = UC_X(uc, 8); Dl_info di; fprintf(stderr, " x8=%#llx", x8);   // x8：间接调用目标常在这里（blr x8 前下探针） | x8: indirect call targets are often here (probe placed before blr x8)
          if (dladdr((void *)x8, &di) && di.dli_sname) fprintf(stderr, "（%s+%#lx）", di.dli_sname, (unsigned long)(x8 - (uint64_t)di.dli_saddr)); }
        if (g_probe[i].dump >= 0) {
            uint8_t b[16]; vm_size_t got = 0;
            uint64_t at = UC_X(uc, 0) + (g_probe[i].dump & 0xffffff);
            if (g_probe[i].dump & 0x1000000) { uint64_t p = 0; vm_size_t g = 0; if (!vm_read_overwrite(mach_task_self(), at, 8, (vm_address_t)&p, &g)) at = p; else at = 0; }   // @*偏移：先解引用再转储 | @*offset: dereference first, then dump
            if (at && !vm_read_overwrite(mach_task_self(), at, 16, (vm_address_t)b, &got) && got == 16) {
                fprintf(stderr, " [%s%#lx]=", (g_probe[i].dump & 0x1000000) ? "*x0+" : "x0+", g_probe[i].dump & 0xffffff); for (int k = 0; k < 16; k++) fprintf(stderr, "%02x", b[k]); }
        }
        fputc('\n', stderr);
        dm_backtrace(UC_FP(uc));
        UC_PC(uc) = (uint64_t)g_probe[i].slot;
        return 1;
    }
    return 0;
}
static uint32_t *g_slots; static int g_slots_sealed;
static void probe_add(module_t *m, uint64_t off, long dump, probe_fn fn) {
    if (g_nprobe >= MAX_PROBE) die("探针过多");
    if (!g_slots) g_slots = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t at = (uint64_t)m->base + off;
    uint32_t *w = (uint32_t *)at, *sl = g_slots + g_nprobe * 8;
    sl[0] = *w;                                  // 原指令 | Original instruction
    sl[1] = 0x58000050;                          // ldr x16, #8
    sl[2] = 0xD61F0200;                          // br x16
    *(uint64_t *)(sl + 3) = at + 4;
    uint64_t pg = at & ~0x3FFFull;
    if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE)) die("probe mprotect");
    *w = 0xD43E0000;                             // brk #0xf000
    mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(w, 4);
    g_probe[g_nprobe].at = at; g_probe[g_nprobe].slot = sl; g_probe[g_nprobe].dump = dump; g_probe[g_nprobe].fn = fn;
    snprintf(g_probe[g_nprobe].name, sizeof g_probe[0].name, "%s+%#llx", m->name, off);
    LOG("探针 %s（原指令 %08x）", g_probe[g_nprobe].name, sl[0]);
    g_nprobe++;
}

#endif   // !TARGET_OS_IPHONE（DM_PROBE）

// ---------- DM_UIMSG=1：UI 消息总线（main 0xb1dc60 观测门 OBS_UIMSG，x1 = 消息 {u64 类型哈希, u64 动作哈希, char* 面板名, …}）----------
// 哈希是 FNV-1a 64；首次命中时扫 main 只读段的全部字符串建反查表，消息直接以名字打印（便于与原版消息流逐条对照）
// ---------- DM_UIMSG=1: UI message bus (observation gate OBS_UIMSG at main 0xb1dc60, x1 = message {u64 type hash, u64 action hash, char* panel name, ...}) ----------
// Hashes are FNV-1a 64; on first hit all strings in main's read-only segment are scanned to build a reverse lookup table, and messages are printed directly by name (for comparing entry by entry against the original game's message stream)
#define UIH_BITS 18
static struct { uint64_t h; const char *s; } *g_uih;
static uint64_t fnv1a(const char *p, size_t n) { uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 0x100000001b3ull; return h; }
static void uih_build(void) {
    module_t *m = &g_mod[0];
    g_uih = calloc(1u << UIH_BITS, sizeof *g_uih);
    const char *p = (const char *)m->base + m->ro_off, *e = (const char *)m->base + m->data_off;
    while (p < e) {
        const char *q = p; while (q < e && *q >= 0x20 && *q < 0x7f) q++;
        if (q < e && !*q && q - p >= 3) {
            uint64_t h = fnv1a(p, q - p); unsigned i = (unsigned)(h >> (64 - UIH_BITS));
            while (g_uih[i].s && g_uih[i].h != h) i = (i + 1) & ((1u << UIH_BITS) - 1);
            g_uih[i].h = h; g_uih[i].s = p;
        }
        p = q + 1;
    }
}
static const char *uih_name(uint64_t h, char *buf) {
    if (g_uih) for (unsigned i = (unsigned)(h >> (64 - UIH_BITS)); g_uih[i].s; i = (i + 1) & ((1u << UIH_BITS) - 1))
        if (g_uih[i].h == h) return g_uih[i].s;
    snprintf(buf, 24, "%#llx", h); return buf;
}
// 输出两路：DM_UIMSG=<路径> 时按 dm/verify/ui_message_stream.md 写 JSONL（每条一行、不折叠、不带时间，供 uicmp.py 与真值对照）；
// stderr 上另打一行人读摘要（相邻相同合并计数）。DM_UIMSG=1 只打 stderr。
// Output goes two ways: with DM_UIMSG=<path>, JSONL is written per dm/verify/ui_message_stream.md (one line per message, not folded, no timestamps, for uicmp.py to compare against ground truth);
// a human-readable summary line is also printed on stderr (adjacent identical lines merged with a count). DM_UIMSG=1 prints to stderr only.
static FILE *g_uifp; static unsigned g_uiseq;
static int in_game(void); static uint32_t cur_tick(void);
static void res_report(const char *why) { void (*f)(uint32_t, const char *) = dm_res_report_cb(); if (f) f(in_game() ? cur_tick() : 0, why); }   // 定义在拍入口一节；DM_UIMSG 行带拍号用 | Defined in the tick entry section; used so DM_UIMSG lines carry the tick number
static pthread_mutex_t g_uilock = PTHREAD_MUTEX_INITIALIZER;
static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); fputc(*s, f); }
    fputc('"', f);
}
// ---------- 启动时间线 DM_BOOTPROF=1 ----------
// 分段：① 宿主装载 ② SDK 初始化 ③ init_array（main.c 打点）；④ nnMain→ToFrontEnd ⑤ 装载面板开→关 ⑥ →MainMenuButtonRibbon（UI 消息打点）。
// 每段记墙钟与各计数器增量；等待类为全线程累加（线程·秒），「主线程等」单列。到 MainMenuButtonRibbon 输出一张表。
// ---------- Boot timeline DM_BOOTPROF=1 ----------
// Segments: ① host loading ② SDK initialization ③ init_array (marked in main.c); ④ nnMain→ToFrontEnd ⑤ loading panel open→close ⑥ →MainMenuButtonRibbon (marked by UI messages).
// Each segment records wall clock and the delta of each counter; wait-type counters are summed over all threads (thread-seconds), with 「主线程等」(main thread wait) listed separately. A table is printed on reaching MainMenuButtonRibbon.
static _Atomic uint64_t g_pns[PROF_N], g_pby[PROF_N];
static const char *const g_pname[PROF_N] = {"RomFS读", "锁等", "条件变量等", "地址等", "同步对象等", "Sleep", "主线程等", "出图等(图形)", "着色器/管线(图形)", "TACT解码", "BC软解(图形)"};
#define MAX_SEG 12
static struct { const char *name; uint64_t t, ns[PROF_N], by[PROF_N]; } g_seg[MAX_SEG];
static int g_nseg, g_prof_done;
static pthread_mutex_t g_proflock = PTHREAD_MUTEX_INITIALIZER;
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) {
    if (!g_flag.bootprof || (unsigned)id >= PROF_N) return;
    atomic_fetch_add(&g_pns[id], ns); if (bytes) atomic_fetch_add(&g_pby[id], bytes);
}
static void prof_table(void) {
    extern uint64_t g_t0_ns;
    fprintf(stderr, "[dm] ===== 启动时间线（DM_BOOTPROF；等待类为全线程累加的线程·秒）=====\n");
    fprintf(stderr, "[dm] %-40s %8s %8s", "分段", "墙钟s", "累计s");
    for (int k = 0; k < PROF_N; k++) fprintf(stderr, " %10s", g_pname[k]);
    fprintf(stderr, " %9s %9s\n", "读MiB", "解码MiB");
    uint64_t prev_t = g_t0_ns, prev_ns[PROF_N] = {0}, prev_by[PROF_N] = {0};
    for (int j = 0; j < g_nseg; j++) {
        fprintf(stderr, "[dm] %-40s %8.3f %8.3f", g_seg[j].name, (g_seg[j].t - prev_t) / 1e9, (g_seg[j].t - g_t0_ns) / 1e9);
        for (int k = 0; k < PROF_N; k++) fprintf(stderr, " %10.3f", (g_seg[j].ns[k] - prev_ns[k]) / 1e9);
        fprintf(stderr, " %9.1f %9.1f\n", (g_seg[j].by[PROF_FS_READ] - prev_by[PROF_FS_READ]) / 1048576.0, (g_seg[j].by[PROF_TACT] - prev_by[PROF_TACT]) / 1048576.0);
        prev_t = g_seg[j].t; memcpy(prev_ns, g_seg[j].ns, sizeof prev_ns); memcpy(prev_by, g_seg[j].by, sizeof prev_by);
    }
}
void dm_prof_mark(const char *seg) {
    if (!g_flag.bootprof) return;
    uint64_t now_ns(void);
    pthread_mutex_lock(&g_proflock);
    if (!g_prof_done && g_nseg < MAX_SEG) {
        g_seg[g_nseg].name = seg; g_seg[g_nseg].t = now_ns();
        for (int k = 0; k < PROF_N; k++) { g_seg[g_nseg].ns[k] = g_pns[k]; g_seg[g_nseg].by[k] = g_pby[k]; }
        g_nseg++;
        if (strstr(seg, "MainMenuButtonRibbon")) { g_prof_done = 1; prof_table(); }
    }
    pthread_mutex_unlock(&g_proflock);
}
static _Atomic uint64_t g_ribbon_ns;
// 进局装载段（DM_BOOTPROF=1）：读取画面开 → 撤，记墙钟与同一组计数器的增量；进日志并追加到 loadprof.txt（日志在进局时常被冲掉）
// In-run load segment (DM_BOOTPROF=1): loading screen open → taken down; wall clock and deltas of the same counters; logged and appended to loadprof.txt (the log is often flushed out while entering)
static struct { uint64_t t, ns[PROF_N], by[PROF_N]; int on; } g_ld;
static void prof_load_begin(void) {
    if (!g_flag.bootprof || g_ld.on) return; uint64_t now_ns(void);
    g_ld.t = now_ns(); for (int k = 0; k < PROF_N; k++) { g_ld.ns[k] = g_pns[k]; g_ld.by[k] = g_pby[k]; } g_ld.on = 1;
}
static void prof_load_end(void) {
    if (!g_flag.bootprof || !g_ld.on) return; uint64_t now_ns(void); g_ld.on = 0;
    char line[1024]; size_t o = snprintf(line, sizeof line, "进局装载 墙钟 %.3f s：", (now_ns() - g_ld.t) / 1e9);
    for (int k = 0; k < PROF_N && o < sizeof line; k++) o += snprintf(line + o, sizeof line - o, " %s %.3f", g_pname[k], (g_pns[k] - g_ld.ns[k]) / 1e9);
    if (o < sizeof line) snprintf(line + o, sizeof line - o, " 读 %.1f MiB 解码 %.1f MiB", (g_pby[PROF_FS_READ] - g_ld.by[PROF_FS_READ]) / 1048576.0, (g_pby[PROF_TACT] - g_ld.by[PROF_TACT]) / 1048576.0);
    LOG("%s", line);
    FILE *f = fopen("loadprof.txt", "a"); if (f) { time_t w = time(NULL); struct tm tmv; localtime_r(&w, &tmv); char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", &tmv); fprintf(f, "%s\t%s\n", ts, line); fclose(f); }
}
static void prof_on_uimsg(uint64_t act, const char *arg) {   // UI 消息边界（动作哈希按 FNV-1a） | UI message boundary (action hash by FNV-1a)
    if (!g_flag.bootprof) return;
    static int seen_fe, seen_open, seen_close;
    if (act == 0x61b685f774a14250ull && !seen_fe) { seen_fe = 1; dm_prof_mark("④ nnMain → ToFrontEnd"); }
    else if (!strcmp(arg, "MainMenuLoadingPanel") && act == 0x723d40e52f74e229ull && !seen_open) { seen_open = 1; dm_prof_mark("④b ToFrontEnd → 装载面板打开"); }
    else if (!strcmp(arg, "MainMenuLoadingPanel") && act != 0x723d40e52f74e229ull && seen_open && !seen_close) { seen_close = 1; dm_prof_mark("⑤ 装载期（装载面板开 → 关）"); }
    if (!strcmp(arg, "LoadScreenPanel") && act == 0x723d40e52f74e229ull) prof_load_begin();
    else if (!strcmp(arg, "MainMenuButtonRibbon")) { if (!g_ribbon_ns) { uint64_t now_ns(void); g_ribbon_ns = now_ns(); } dm_prof_mark("⑥ → MainMenuButtonRibbon"); }
}
uint64_t dm_prof_ribbon_ns(void) { return g_ribbon_ns; }   // 第一次 OpenPanel MainMenuButtonRibbon 的 now_ns 时刻；未出现为 0（需 DM_BOOTPROF=1） | now_ns time of the first OpenPanel MainMenuButtonRibbon; 0 if it has not appeared (needs DM_BOOTPROF=1)
static int g_ui_print;   // DM_UIMSG 设置时才输出消息流；BOOTPROF 只借观测门打点 | The message stream is output only when DM_UIMSG is set; BOOTPROF only borrows the observation gate for timing marks
static void uimsg_print(const uint64_t *regs);

static void (*g_ui_sub[8])(const char *cat, const char *act, const char *arg); static _Atomic int g_nui_sub;   // UI 消息订阅者（DM_KEYS 按消息驱动、启动块预取到选角收尾） | UI message subscribers (DM_KEYS driven by messages, startup block prefetch wrapping up at character select)
// 宿主补发一条界面消息给所有订阅者（面板状态自愈用，见 dm_cursor.c ui_panel_heal）：只走订阅者，不进游戏
// The host sends one extra UI message to all subscribers (for panel state self-healing, see ui_panel_heal in dm_cursor.c): goes only to subscribers, not into the game
void uimsg_emit(const char *cat, const char *act, const char *arg) {
    for (int k = 0, n = atomic_load(&g_nui_sub); k < n; k++) g_ui_sub[k](cat, act, arg);
}
void uimsg_subscribe(void (*fn)(const char *cat, const char *act, const char *arg)) {
    if (!gate_obs_present(OBS_UIMSG)) die("产物没有 UI 观测门 OBS_UIMSG（需 r6 起的产物）：UI 消息订阅无法工作");
    int k = atomic_load(&g_nui_sub); if (k == 8) die("UI 消息订阅者超过 8 个");
    g_ui_sub[k] = fn; atomic_store(&g_nui_sub, k + 1); gate_obs_set(OBS_UIMSG, uimsg_print);
}

// 怪物血条名字后标抗性（按当前难度、读这只怪身上的属性，连随机词缀的抗性都对；全 0 不显示、≥100 标「免」）。
// 读码：血条信息由客户端 main+0x455a80(单位) 建，名字拼在它自己栈上 0x800 字节的缓冲区（0x458500 起按 0x800 封顶），
// 载荷 = 0x40 字节新对象：+0 名字缓冲区指针、+0x20 副标题；随后同步派发 MonsterHealth/SetInfo（就是本观测门），派发完载荷即释放。
// UI 观测门在宿主与 iOS 上都常装（ticks_install → shotui_install 无条件订阅读取画面状态；iOS 前端另订阅 touch_ui_message），这里只在 uimsg_print 里加一次调用。
// 所以本回调触发时 x19（被调方保存寄存器，0x455aa4 起一直是单位）就是这只怪；就地往缓冲区追加，长度守 0x800。
// 抗性属性 36 物理 / 37 魔法 / 39 火 / 41 电 / 43 冰 / 45 毒（itemstatcost.txt），走游戏取属性函数 main+0x5b9290（累计层，含难度与词缀）。
// 开关：自定义页 item_notes（与物品注释同一项）；运行中即时生效。
// Append resistances after the name on the monster health bar (per current difficulty, read from this monster's own stats, so even random-affix resistances are right; all 0 is not shown, ≥100 is marked 「免」(immune)).
// Code reading: the health bar info is built by the client's main+0x455a80(unit); the name is assembled in a 0x800-byte buffer on its own stack (capped at 0x800 from 0x458500 on),
// payload = a new 0x40-byte object: +0 name buffer pointer, +0x20 subtitle; then MonsterHealth/SetInfo is dispatched synchronously (that is this observation gate), and the payload is freed right after dispatch.
// The UI observation gate is always installed on both host and iOS (ticks_install → shotui_install unconditionally subscribes to loading screen state; the iOS front end additionally subscribes touch_ui_message); here only one call is added in uimsg_print.
// So when this callback fires, x19 (callee-saved register, the unit all along from 0x455aa4 on) is this monster; append to the buffer in place, keeping the length within 0x800.
// Resistance stats 36 physical / 37 magic / 39 fire / 41 lightning / 43 cold / 45 poison (itemstatcost.txt), via the game's stat getter main+0x5b9290 (accumulated layer, including difficulty and affixes).
// Switch: item_notes on the customization page (same entry as item notes); takes effect immediately at runtime.
#define UIH_MONSTERHEALTH 0xd3a07a5ecad1a9a3ull
#define UIH_SETINFO       0x40ffba2d98a3ff97ull
static void monres_on_setinfo(const uint64_t *regs, uint64_t payload) {
    int dm_custom_get(const char *key), lang = dm_lang_index(); if (!dm_custom_get("item_notes") || (lang != 8 && lang != 2 && lang != 10)) return;   // 物品注释一系做了简中与西语（zhCN、esES、esMX 三列，annot_strings.py LANGS）；别的语言不显示 | The item-annotation family (物品注释) is done in Simplified Chinese and Spanish (three columns zhCN, esES, esMX; annot_strings.py LANGS); other languages do not show it
    module_t *mm = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) mm = &g_mod[i];
    uint64_t unit = regs[19], name = 0; uint32_t type = 0; vm_size_t got = 0;
    if (!mm || !unit || vm_read_overwrite(mach_task_self(), unit, 4, (vm_address_t)&type, &got) || type != 1) return;   // 只认怪物（单位类型 1） | Monsters only (unit type 1)
    if (vm_read_overwrite(mach_task_self(), payload, 8, (vm_address_t)&name, &got) || !name) return;
    int (*get)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(mm->base + 0x5b9290);
    static const int id[6] = {36, 37, 39, 41, 43, 45};
    static const char *lb[6] = {"物", "魔", "火", "电", "冰", "毒"}, *lbes[6] = {"Fís", "Mág", "Fue", "Ray", "Frí", "Ven"}, *cc[6] = {"\xee\x81\xbe" "4", "\xee\x81\xbe" "8", "\xee\x81\xbe" "1", "\xee\x81\xbe" "9", "\xee\x81\xbe" "3", "\xee\x81\xbe" "2"};   // 颜色：字符串表加载时把「ÿc<码>」换成私用区 U+E07E + <码>（UTF-8 EE 81 BE，读缓冲区实测），这里直接写换后的形式（暗金 / 橙 / 红 / 黄 / 蓝 / 绿） | Colors: when the string table loads, "ÿc<code>" is replaced by private-use U+E07E + <code> (UTF-8 EE 81 BE, measured by reading the buffer); here we write the replaced form directly (unique gold / orange / red / yellow / blue / green)
    char add[160] = ""; size_t n = 0;
    for (int i = 0; i < 6; i++) { int v = get(unit, id[i], 0); if (!v) continue;
        int w = lang != 8 ? (v >= 100 ? snprintf(add + n, sizeof add - n, " %s%s inm", cc[i], lbes[i]) : snprintf(add + n, sizeof add - n, " %s%s %d", cc[i], lbes[i], v))   // 西语：简称与数字之间空一格，免疫写 inm（inmune） | Spanish: one space between the abbreviation and the number; immunity is written inm (inmune)
                          : v >= 100 ? snprintf(add + n, sizeof add - n, " %s%s免", cc[i], lb[i]) : snprintf(add + n, sizeof add - n, " %s%s%d", cc[i], lb[i], v);
        if (w < 0 || (size_t)w >= sizeof add - n) return; n += (size_t)w; }
    { static int lg = -1; static uint64_t last; if (lg < 0) lg = getenv("DM_TEST_MONRES_LOG") != NULL;   // 回归判据：单位换了才记一行（拼出的串 + 六项属性原值） | Regression criterion: log a line only when the unit changes (the assembled string + the raw values of the six stats)
      if (lg && unit != last) { last = unit; int v[6]; for (int i = 0; i < 6; i++) v[i] = get(unit, id[i], 0);
          LOG("DM_TEST_MONRES_LOG：单位 %#llx 属性 36/37/39/41/43/45 = %d/%d/%d/%d/%d/%d → 追加「%s」", (unsigned long long)unit, v[0], v[1], v[2], v[3], v[4], v[5], n ? add : "（全 0 不加）"); } }
    if (!n) return;   // 全 0 不显示 | All 0: not shown
    char *buf = (char *)name; size_t len = strnlen(buf, 0x800); if (len >= 0x800) return;
    size_t keep = len; while (keep && buf[keep - 1] == ' ') keep--;   // 原串前后各带两个空格（「  沉沦魔  」），抗性接在名字后、尾空格放回最后 | The original string has two spaces on each side (「  沉沦魔  」(Fallen)); the resistances go right after the name and the trailing spaces are put back at the end
    if (keep + n + 5 + (len - keep) >= 0x800) return;
    memcpy(buf + keep, add, n); memcpy(buf + keep + n, "\xee\x81\xbe" "0", 4); memset(buf + keep + n + 4, ' ', len - keep); buf[keep + n + 4 + (len - keep)] = 0;
}

static void uimsg_print(const uint64_t *regs) {   // 观测门回调：只读寄存器 | Observation gate callback: registers are read-only
    if ((g_ui_print || atomic_load(&g_nui_sub)) && !g_uih) uih_build();
    uint64_t msg = regs[1], f[3] = {0}; vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), msg, sizeof f, (vm_address_t)f, &got) || got != sizeof f) return;   // 非消息调用：不输出 | Not a message call: no output
    char b0[24], b1[24], nm[65] = ""; int have_arg = 0; got = 0;
    if (f[2] && !vm_read_overwrite(mach_task_self(), f[2], 64, (vm_address_t)nm, &got)) {
        nm[got] = 0; have_arg = 1;
        size_t k = 0; for (; k < got && nm[k]; k++) if ((uint8_t)nm[k] < 0x20 || (uint8_t)nm[k] >= 0x7f) { have_arg = 0; break; }   // 面板名都是可见 ASCII；有些消息（如 InputMessage MouseMove）这一格不是字符串指针， | Panel names are all printable ASCII; for some messages (e.g. InputMessage MouseMove) this slot is not a string pointer,
                                                                                                         // 指到的字节碰巧像字符（出过一次：arg 写成 "\xf0\xbc\xb8"，断言脚本按 UTF-8 读 uimsg.jsonl 报错，inv_tap 假红两次）
                                                                                                         // The pointed-to bytes happened to look like characters (happened once: arg was written as "\xf0\xbc\xb8", the assertion script reading uimsg.jsonl as UTF-8 errored out, and inv_tap failed spuriously twice)
        if (k == got) have_arg = 0;                                         // 64 字节内没有终止 0 | No terminating 0 within 64 bytes
    }
    if (f[0] == UIH_MONSTERHEALTH && f[1] == UIH_SETINFO && f[2]) monres_on_setinfo(regs, f[2]);
    prof_on_uimsg(f[1], have_arg ? nm : "");
    char c0[24], c1[24], stage[256];
    const char *n0 = uih_name(f[0], c0), *n1 = uih_name(f[1], c1);   // 名字表里有就返回表里的串、不写进 c0 / c1（原先拿 c0 / c1 比「ToGame」永远比不上） | If the name table has it, return the table's string without writing into c0 / c1 (it used to compare c0 / c1 against "ToGame", which could never match)
    snprintf(stage, sizeof stage, "%s:%s:%s", n0, n1, have_arg ? nm : "");
    crash_watch_progress(cur_tick(), stage);
    // 看门狗的「进局前按出图帧算进展」：启动后到主菜单出来为止（主菜单上不动 = 按键序卡住，照判死）；
    // 从局里退回前端（ToFrontEnd 且之前进过局）后重新按出图帧算，直到再进局——局后停在前端发呆不是卡住（回前端 15 秒报「卡在 ClosePanel:SaveIndicatorPanel」的误报）
    // The watchdog's "before joining a run, measure progress by presented frames": from launch until the main menu appears (no movement on the main menu = the key sequence is stuck, still judged dead);
    // after returning from a run to the front end (ToFrontEnd and a run was joined before), measure by presented frames again until the next run is joined -- idling on the front end after a run is not being stuck (the false report 「卡在 ClosePanel:SaveIndicatorPanel」(stuck at ClosePanel:SaveIndicatorPanel) 15 seconds after returning to the front end)
    { static int played, back; if (!strcmp(n0, "FrontEndNavigation") && !strcmp(n1, "ToGame")) { played = 1; back = 0; crash_watch_pregame_done(); }
      else if (!strcmp(n0, "FrontEndNavigation") && !strcmp(n1, "ToFrontEnd") && played) { back = 1; void crash_watch_pregame_again(void); crash_watch_pregame_again(); }
      else if (have_arg && !strcmp(nm, "MainMenuButtonRibbon") && !back) crash_watch_pregame_done(); }
    for (int k = 0, n = atomic_load(&g_nui_sub); k < n; k++) { char s0[24], s1[24]; g_ui_sub[k](uih_name(f[0], s0), uih_name(f[1], s1), have_arg ? nm : ""); }
    if (!g_ui_print) return;
    const char *cat = uih_name(f[0], b0), *act = uih_name(f[1], b1);
    pthread_mutex_lock(&g_uilock);
    if (g_uifp) {
        fprintf(g_uifp, "{\"seq\":%u,\"cat_h\":\"0x%016llx\",\"act_h\":\"0x%016llx\",\"arg\":", g_uiseq++, f[0], f[1]);
        if (have_arg) json_str(g_uifp, nm); else fputs("null", g_uifp);
        if (cat != b0) { fputs(",\"cat\":", g_uifp); json_str(g_uifp, cat); }
        if (act != b1) { fputs(",\"act\":", g_uifp); json_str(g_uifp, act); }
        if (in_game()) fprintf(g_uifp, ",\"tick\":%u", cur_tick());   // 局内带服务端当前拍号（触屏用例按拍对齐面板开关；uicmp 只比 cat_h/act_h/arg，不受影响） | In a run, include the server's current tick number (touch test cases align panel open/close by tick; uicmp only compares cat_h/act_h/arg and is unaffected)
        fputs("}\n", g_uifp); fflush(g_uifp);
    }
    if (!strcmp(cat, "PanelManager") && have_arg) {   // 长帧日志的上下文：记最近打开且未关的面板 | Context for the long-frame log: remember the most recently opened and not yet closed panel
        extern char g_lf_panel[48];
        if (!strcmp(act, "OpenPanel")) strlcpy(g_lf_panel, nm, sizeof g_lf_panel); else if (!strcmp(act, "ClosePanel") && !strcmp(g_lf_panel, nm)) g_lf_panel[0] = 0;
    }
    extern uint64_t g_t0_ns; uint64_t now_ns(void);
    static uint64_t last[2]; static char lastnm[65]; static unsigned rep;   // stderr 摘要：相邻相同合并计数 | stderr summary: adjacent identical lines are merged with a count
    if (f[0] == last[0] && f[1] == last[1] && !strcmp(have_arg ? nm : "", lastnm)) { rep++; pthread_mutex_unlock(&g_uilock); return; }
    if (rep) fprintf(stderr, "[dm] UIMSG   （上一条又重复 %u 次）\n", rep);
    rep = 0; last[0] = f[0]; last[1] = f[1]; strlcpy(lastnm, have_arg ? nm : "", sizeof lastnm);
    fprintf(stderr, "[dm] UIMSG [%.2fs] %s %s %s\n", (now_ns() - g_t0_ns) / 1e9, cat, act, have_arg ? nm : "");
    pthread_mutex_unlock(&g_uilock);
}

void crash_set_trap_hook(int (*fn)(ucontext_t *uc));
// SetLanguage 记录（观测门 5，常开）：原版切语言（声音页「文字语言」或读设置时）走 0x317298 清空 + 重装字符串表，r34 起不再剪（英文版）。
// 每次换语言记一行（号 → 名表 main+0x20ae478：0 enUS … 8 zhCN）；当前语言号的全局量是 main+0x20ae4e0（SetLanguage 写）
// 客户端事件包（观测门 15，r35 起）：入口 main+0x442bc0，x0 = 包，第 2 字节 = 事件子类型（跳转表 0x1da6752，0–0x19）。
// 子类型 0x17 = 离线难度（/players）变了 → 0x442d5c 按串号 0x6a47 取「游戏难度设为 N…」，写聊天栏一行 + 系统提示框（宿主 A/B 实测：开关关时 0x17 到后约 10 秒出这条提示，开着拦掉后不出；
// patch_nso.py 观测门 15 的说明里写的 0x19 是读跳转表时认错了，下次换代改）。秘境局里（dm_quiet_difficulty_msg 开着）把这一字节改成表外值，
// 原版自己走默认分支（不显示）；人数本身在服务端照常生效。开关不开时门块进出一次即返回，什么都不改
// SetLanguage log (observation gate 5, always on): the original game's language switch (「文字语言」(Text Language) on the audio page, or when settings are read) goes through 0x317298 to clear + reload the string table; no longer cut since r34 (English version).
// Log a line on every language change (number → name table main+0x20ae478: 0 enUS ... 8 zhCN); the global holding the current language number is main+0x20ae4e0 (written by SetLanguage)
// Client event packet (observation gate 15, since r35): entry main+0x442bc0, x0 = packet, byte 2 = event subtype (jump table 0x1da6752, 0–0x19).
// Subtype 0x17 = offline difficulty (/players) changed → 0x442d5c fetches 「游戏难度设为 N…」(Game difficulty set to N...) by string number 0x6a47 and writes a chat line + a system prompt box (host A/B measured: with the switch off this prompt appears about 10 seconds after 0x17 arrives; with it on and blocked it does not appear;
// the 0x19 written in the observation gate 15 description in patch_nso.py was a misreading of the jump table, to be fixed at the next build output generation). In a rift run (dm_quiet_difficulty_msg on) this byte is changed to an out-of-table value,
// and the original game takes its default branch by itself (shows nothing); the player count itself still takes effect on the server as usual. When the switch is off the gate block returns after one pass in and out, changing nothing
static _Atomic int g_quiet_diff;
void dm_quiet_difficulty_msg(int on) { atomic_store(&g_quiet_diff, on); }
static void event_msg_obs(const uint64_t *regs) {
    uint8_t *pkt = (uint8_t *)(uintptr_t)regs[0]; if (!atomic_load(&g_quiet_diff) || !pkt || pkt[1] != 0x17) return;
    static int n; if (n++ < 8) LOG("秘境：拦下原版「游戏难度设为 N」提示（事件子类型 0x17）"); pkt[1] = 0xff;
}
static void setlang_obs(const uint64_t *regs) {
    dm_ui_gen_bump(DM_UIGEN_LANG);   // 界面代号（dm_cursor.h）：切语言会重建整套界面 | UI generation (dm_cursor.h): switching language rebuilds the entire UI
    { void dm_padmode_lang_reset(void); dm_padmode_lang_reset(); }   // 串表清空之前：我们换进串表的指针先写回原文、记的节点清空（dm_padmode.c） | Before the string table is cleared: write the original text back over the pointers we swapped into the string table and clear the recorded nodes (dm_padmode.c)
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    uint32_t idx = 0; const char *nm = NULL; vm_size_t got = 0;   // 安全读（不用 tk_rd：它给拍状态采集计数） | Safe read (not tk_rd: that one counts toward tick state collection)
    if (regs[1] && !vm_read_overwrite(mach_task_self(), regs[1], 4, (vm_address_t)&idx, &got) && got == 4 && idx < 13) {
        uint64_t p = 0; memcpy(&p, m->base + 0x20ae478 + 8ull * idx, 8); nm = (const char *)p; }
    static uint32_t last = 0xffffffff; if (idx != last) { last = idx; LOG("SetLanguage：语言 %s（号 %u），重装字符串表", nm ? nm : "?", idx); }
    if (nm) atomic_store(&g_dm_lang, (int)idx);   // 我们自己的 UIKit 文字跟着换（dm_i18n.c） | our own UIKit text switches along with it (dm_i18n.c)
}
void probe_install(void) {
    if (gate_obs_present(OBS_SETLANG)) gate_obs_set(OBS_SETLANG, setlang_obs);
    if (gate_obs_present(OBS_EVENT_MSG)) gate_obs_set(OBS_EVENT_MSG, event_msg_obs);
    { void dm_ui_tooltip_obs(const uint64_t *regs); if (gate_obs_present(OBS_TOOLTIP)) gate_obs_set(OBS_TOOLTIP, dm_ui_tooltip_obs); }   // 说明框停靠（dm_cursor.c） | tooltip docking (dm_cursor.c)
    const char *ui = getenv("DM_UIMSG");
    if (ui && strcmp(ui, "1") && !(g_uifp = fopen(ui, "w"))) die("DM_UIMSG 打不开 %s", ui);
    g_ui_print = ui != NULL;
    if (ui || g_flag.bootprof) {                     // UI 总线：改写器离线插的观测门（r6 起的产物都带；零运行时改代码） | UI bus: observation gate inserted offline by the rewriter (present in all build outputs since r6; zero runtime code modification)
        if (!gate_obs_present(OBS_UIMSG)) LOG("【红】产物没有 UI 观测门 OBS_UIMSG（需 r6 起的产物）：DM_UIMSG / 启动分段 ④–⑥ 无输出");
        else gate_obs_set(OBS_UIMSG, uimsg_print);
    }
#if !TARGET_OS_IPHONE
    crash_set_trap_hook(probe_hit);
    const char *e = getenv("DM_PROBE");
    if (e) {
        char spec[512]; strlcpy(spec, e, sizeof spec);
        for (char *t = strtok(spec, ","); t; t = strtok(NULL, ",")) {
            char *c = strchr(t, ':'); if (!c) die("DM_PROBE 格式：模块:偏移");
            *c = 0; module_t *m = NULL;
            for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, t)) m = &g_mod[i];
            if (!m) die("DM_PROBE 无此模块 %s", t);
            char *at_ = strchr(c + 1, '@');
            long dump = -1;
            if (at_) dump = at_[1] == '*' ? (0x1000000 | strtol(at_ + 2, 0, 16)) : strtol(at_ + 1, 0, 16);   // @偏移 / @*偏移 | @offset / @*offset
            probe_add(m, strtoull(c + 1, 0, 16), dump, NULL);
        }
    }
    if (g_slots && !g_slots_sealed) { mprotect(g_slots, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(g_slots, 0x4000); g_slots_sealed = 1; }
#endif
}

// ---------- 采样 DM_SAMPLE=毫秒：定时打断调用 crash_sample_self() 的那个线程（客体主线程），打印 pc 与回溯 ----------
// ---------- Sampling DM_SAMPLE=milliseconds: periodically interrupt the thread that called crash_sample_self() (the guest main thread) and print pc and backtrace ----------
static pthread_t g_sample_pt;
// 信号处理里只做异步信号安全的事：把 pc / lr / 帧指针链拷进无锁环形缓冲；符号化与打印由采样线程做
// （曾在处理函数里直接 fprintf：被打断的线程若正持 stderr 锁，处理函数自锁死、客体主线程挂住）
// Do only async-signal-safe things in the signal handler: copy pc / lr / the frame pointer chain into a lock-free ring buffer; symbolization and printing are done by the sampling thread
// (it once called fprintf directly in the handler: if the interrupted thread happened to hold the stderr lock, the handler deadlocked on itself and the guest main thread hung)
#define SRING 1024
static struct { uint64_t t, pc, lr, bt[16]; int n; uint32_t h; } g_sring[SRING];
static _Atomic unsigned g_shead;
static void on_sample(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; ucontext_t *uc = uc_;
    unsigned i = atomic_load(&g_shead) % SRING;
    extern uint64_t now_ns(void);
    uint32_t thr_cur_handle(void);
    g_sring[i].h = thr_cur_handle(); g_sring[i].t = now_ns(); g_sring[i].pc = UC_PC(uc); g_sring[i].lr = UC_LR(uc);
    uint64_t fp = UC_FP(uc); int n = 0;
    // 帧链用安全读：被打断的函数可能把 x29 当普通寄存器用（直接解引用曾段错误）
    // The frame chain uses safe reads: the interrupted function may be using x29 as an ordinary register (a direct dereference once segfaulted)
    for (; n < 16 && fp && !(fp & 7); n++) {
        uint64_t fr[2]; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), fp, sizeof fr, (vm_address_t)fr, &got) || got != sizeof fr) break;
        g_sring[i].bt[n] = fr[1];
        if (fr[0] <= fp) { n++; break; }
        fp = fr[0];
    }
    g_sring[i].n = n;
    atomic_fetch_add(&g_shead, 1);
}
int thr_find_by_name(const char *name, pthread_t *out);
static const char *g_sample_name;
static void *sampler(void *p) {
    useconds_t us = (useconds_t)(uintptr_t)p; unsigned printed = 0;
    for (;;) {
        usleep(us);
        if (g_sample_name && !strcmp(g_sample_name, "*")) {        // 全部客体线程轮一遍 | Cycle through all guest threads
            int thr_list(pthread_t *out, int max); pthread_t all[64]; int n = thr_list(all, 64);
            for (int k = 0; k < n; k++) { pthread_kill(all[k], SIGUSR2); usleep(200); }
        } else {
            if (g_sample_name) { pthread_t t; if (!thr_find_by_name(g_sample_name, &t)) continue; g_sample_pt = t; }
            pthread_kill(g_sample_pt, SIGUSR2);
        }
        for (unsigned h = atomic_load(&g_shead); printed < h; printed++) {
            unsigned i = printed % SRING;
            extern uint64_t g_t0_ns;
            fprintf(stderr, "[dm] SAMPLE #%u [%.3fs] thr=%#x", printed + 1, (g_sring[i].t - g_t0_ns) / 1e9, g_sring[i].h); off("pc", g_sring[i].pc); off("lr", g_sring[i].lr);
            fprintf(stderr, "\n[dm] bt:"); for (int k = 0; k < g_sring[i].n; k++) off("", g_sring[i].bt[k]); fputc('\n', stderr);
        }
    }
}
void crash_sample_self(void) {
    const char *e = getenv("DM_SAMPLE"); if (!e) return;
    g_sample_pt = pthread_self();
    const char *c = strchr(e, ':'); if (c) g_sample_name = strdup(c + 1);
    struct sigaction sa = {.sa_sigaction = on_sample, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigaction(SIGUSR2, &sa, 0);
    pthread_t t; pthread_create(&t, NULL, sampler, (void *)(uintptr_t)(atoi(e) * 1000)); pthread_detach(t);
}

// ---------- DM_INFLATE_BENCH=<样本文件>：用游戏映像里的 zlib（main 0xf3a930 inflateInit_、0xf3aa90 inflate）解同一批样本并计时 ----------
// 样本格式同 scratchpad infl/bench.c：{CKey16, 原始长, 块数, [模式, 压缩长, 解压长, 数据]...}；逐文件核 MD5 == CKey。
// 只在客体线程初始化之后、SDK 初始化之前调（zlib 不依赖 SDK：z_stream 的 zalloc / zfree 填宿主函数）。
// ---------- DM_INFLATE_BENCH=<sample file>: decompress the same batch of samples with the zlib in the game image (main 0xf3a930 inflateInit_, 0xf3aa90 inflate) and time it ----------
// Sample format same as scratchpad infl/bench.c: {CKey16, raw length, block count, [mode, compressed length, decompressed length, data]...}; verify MD5 == CKey file by file.
// Called only after guest thread initialization and before SDK initialization (zlib does not depend on the SDK: z_stream's zalloc / zfree are filled with host functions).
#include <CommonCrypto/CommonDigest.h>
static void *g_zarena[64]; static int g_nzarena;
static void *bench_zalloc(void *o, unsigned items, unsigned size) { (void)o; void *p = calloc(items, size); if (g_nzarena < 64) g_zarena[g_nzarena++] = p; return p; }
static void bench_zfree(void *o, void *p) { (void)o; (void)p; }      // 每块结束统一释放 | Everything is freed together at the end of each block
void inflate_bench_maybe(void) {
    const char *path = getenv("DM_INFLATE_BENCH"); if (!path) return;
    FILE *f = fopen(path, "rb"); if (!f) die("打不开 %s", path);
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz); if (fread(buf, 1, sz, f) != (size_t)sz) die("读样本失败"); fclose(f);
    int (*init)(void *, const char *, int) = (void *)(g_mod[0].base + 0xf3a930);
    int (*inflate)(void *, int) = (void *)(g_mod[0].base + 0xf3aa90);
    uint8_t *out = malloc(64 << 20); uint64_t now_ns(void);
    int rounds = getenv("DM_INFLATE_ROUNDS") ? atoi(getenv("DM_INFLATE_ROUNDS")) : 3;
    double best = 1e9; long nfile = 0, bad = 0; uint64_t zin = 0, zout = 0;
    for (int r = 0; r < rounds; r++) {
        uint64_t t = 0; nfile = bad = 0; zin = zout = 0;
        for (uint8_t *p = buf; p < buf + sz;) {
            uint8_t ckey[16]; memcpy(ckey, p, 16); uint32_t cs, nc; memcpy(&cs, p + 16, 4); memcpy(&nc, p + 20, 4); p += 24;
            size_t pos = 0; int ok = 1;
            for (uint32_t k = 0; k < nc; k++) {
                uint8_t mode = p[0]; uint32_t n, dl; memcpy(&n, p + 1, 4); memcpy(&dl, p + 5, 4); p += 9;
                if (mode == 'Z') {
                    uint8_t zs[112] = {0};                    // 标准 64 位 z_stream（游戏 inflate 读 +0 next_in、+0x18 next_out、+0x38 state） | Standard 64-bit z_stream (the game's inflate reads +0 next_in, +0x18 next_out, +0x38 state)
                    *(uint8_t **)(zs + 0) = p; *(uint32_t *)(zs + 8) = n; *(uint8_t **)(zs + 24) = out + pos; *(uint32_t *)(zs + 32) = dl;
                    *(void **)(zs + 64) = (void *)bench_zalloc; *(void **)(zs + 72) = (void *)bench_zfree;
                    uint64_t t0 = now_ns();
                    int rc = init(zs, "1.2.11", 112); if (!rc) rc = inflate(zs, 4);
                    t += now_ns() - t0;
                    if (rc != 1 || *(uint64_t *)(zs + 40) != dl) ok = 0;
                    for (int i = 0; i < g_nzarena; i++) free(g_zarena[i]);
                    g_nzarena = 0; zin += n; zout += dl;
                } else if (mode == 'N') memcpy(out + pos, p, n);
                else ok = 0;
                pos += dl; p += n;
            }
            uint8_t md[16]; CC_MD5(out, (CC_LONG)pos, md);
            if (!ok || pos != cs || memcmp(md, ckey, 16)) bad++;
            nfile++;
        }
        if (t / 1e9 < best) best = t / 1e9;
    }
    fprintf(stderr, "[dm] 游戏 zlib     文件 %ld 不符 %ld | zlib 输入 %.1f MiB 输出 %.1f MiB | 解码最小 %.3f s（%d 轮）= %.0f MiB/s 输出\n",
            nfile, bad, zin / 1048576.0, zout / 1048576.0, best, rounds, zout / 1048576.0 / best);
    exit(bad != 0);
}

// ---------- DM_TICKS=<路径>[ DM_TICKS_N=拍数][ DM_TICKS_FULL=1 逐单位明细]：逐拍 6 格游戏状态哈希（main 0x5d3bc0 服务端拍入口观测门 OBS_TICK）----------
// 口径 = dm/verify/tick_state_format.md（真值侧 dm/verify/oracle/swtick.py 逐行对照）：入口第一条指令前取，基址 G = x0；
// 5 类单位表（128 桶、next=+0x158）+ 全局种子 G+0x1a8，按 (type, id) 稳定升序并入各格 FNV-1a 64。读不到的指针按真值 Reader 语义（读失败 = 无）。
// 每拍一行 JSON（字段与 swtick.py 的 json.dumps 同序同分隔）；同拍号只取一次，断档报红；记满 DM_TICKS_N 拍即正常退出。不开时观测门不挂回调。
// ---------- DM_TICKS=<path>[ DM_TICKS_N=tick count][ DM_TICKS_FULL=1 per-unit detail]: per-tick 6-cell game state hash (observation gate OBS_TICK at the server tick entry main 0x5d3bc0) ----------
// Convention = dm/verify/tick_state_format.md (compared line by line against dm/verify/oracle/swtick.py on the ground-truth side): taken before the first instruction of the entry, base G = x0;
// 5 unit tables (128 buckets, next=+0x158) + global seed G+0x1a8, merged in stable ascending (type, id) order into each cell's FNV-1a 64. Unreadable pointers follow the ground-truth Reader semantics (failed read = None).
// One JSON line per tick (fields in the same order and with the same separators as swtick.py's json.dumps); each tick number is taken only once, a gap reports a failure; exits normally once DM_TICKS_N ticks are recorded. When off, no callback is attached to the observation gate.
static int tk_rd(uint64_t a, void *buf, size_t n);
static void cam_threats_tick(uint64_t G, uint64_t pl);
static _Atomic uint64_t g_player_path;   // 本地玩家的路径头（状态泵每拍更新） | path head of the local player (updated by the state pump every tick)
// 本地玩家的逻辑位置（16.16 定点，格）：给图形层的连拍逐帧记一份，好和画面位移对照。读不到返回 0
// Logical position of the local player (16.16 fixed point, tiles): the graphics layer's burst capture records one per frame so it can be compared with on-screen displacement. Returns 0 when it cannot be read
int dm_diag_player_fixed(uint32_t *x, uint32_t *y) { uint64_t pp = atomic_load(&g_player_path); uint32_t c[2]; if (!pp || !tk_rd(pp, c, 8)) return 0; *x = c[0]; *y = c[1]; return 1; }
static FILE *g_tkfp; static int g_tkfull; static long g_tkleft = -1; static int64_t g_tklast = -1; static uint64_t g_tkt0; static unsigned g_tkreads;
static FILE *g_spfp;
// ---- 难度档位与 HUD 数字（dm_hud.h）：都在拍入口（游戏线程）读写客体，前端经 dm_hud_get / dm_hud_request_players 交互 ----
// ---- Difficulty setting and HUD numbers (dm_hud.h): both read and write the guest at the tick entry (game thread); the front end interacts via dm_hud_get / dm_hud_request_players ----
static int g_players_applied; static dm_hud_info g_hud;   // 只在拍线程上读写；对外发布经 dm_hud_publish | Read and written only on the tick thread; published externally via dm_hud_publish
static module_t *main_mod(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return &g_mod[i]; return NULL; }
static void *players_obj(module_t *main) { return ((void *(*)(void))(main->base + 0x3102c0))(); }   // 原版 /players 处理函数里同一调用（无参，返回单例） | The same call as in the original game's /players handler (no arguments, returns a singleton)

static void players_apply(void) {          // 有新请求就照原版 /players：模式值 ∈ {0,5,7} 才允许 → main+0x1199c50(对象, N) | On a new request do as the original game's /players does: only allowed when the mode value ∈ {0,5,7} → main+0x1199c50(object, N)
    int want = dm_hud_requested_players();
    if (!want || want == g_players_applied) return;
    g_players_applied = want;
    module_t *main = main_mod();
    if (!main) { LOG("【红】DM_PLAYERS：没有 main 模块"); return; }
    uint32_t opmode = 0;
    if (!tk_rd((uint64_t)main->base + 0x2d20b8c, &opmode, 4)) { LOG("【红】DM_PLAYERS：读 main+0x2d20b8c 失败"); return; }
    if (opmode != 0 && opmode != 5 && opmode != 7) { LOG("DM_PLAYERS：模式 %u 不在原版允许集合 {0,5,7}，不改档", opmode); return; }
    void *obj = players_obj(main);
    if (!obj) { LOG("【红】DM_PLAYERS：0x3102c0 返回空对象"); return; }
    ((void (*)(void *, uint32_t))(main->base + 0x1199c50))(obj, (uint32_t)want);
    LOG("DM_PLAYERS：原版路径 main+0x3102c0() → main+0x1199c50(obj,%d)（模式值 %u）", want, opmode);
}

static int stat_read(uint64_t unit, uint32_t want, uint32_t *value) {
    uint64_t sl = 0, sb = 0; uint32_t sc = 0;
    if (!tk_rd(unit + 0x88, &sl, 8) || !sl || !tk_rd(sl + 0x30, &sb, 8) || !tk_rd(sl + 0x38, &sc, 4)) return 0;
    if (sc > 512) sc = 512;
    uint32_t pair[2];
    for (uint32_t i = 0; i < sc; i++) if (tk_rd(sb + 8 * i, pair, sizeof pair) && pair[0] == want << 16) { *value = pair[1]; return 1; }   // a = statId<<16 | 层（tick_state_format.md），取第 0 层 | a = statId<<16 | layer (tick_state_format.md), take layer 0
    return 0;
}

// 测试开关 DM_TEST_HP1=1：每拍把受控玩家的生命（stat 6）压到 1 点，挨一下就死——造死亡画面用（触屏用例验「继续」按钮），产品不设
// Test switch DM_TEST_HP1=1: every tick push the controlled player's life (stat 6) down to 1 point so one hit kills -- for producing the death screen (touch test cases verify the 「继续」(Continue) button); not set in the product
static uint32_t g_hp1_to = 256;   // DM_TEST_HP1=<生命定点值，缺省 256 = 1 点；0 = 直接归零> | DM_TEST_HP1=<life fixed-point value, default 256 = 1 point; 0 = straight to zero>
static void test_hp1(uint64_t unit) {
    uint64_t sl = 0, sb = 0; uint32_t sc = 0;
    if (!tk_rd(unit + 0x88, &sl, 8) || !sl || !tk_rd(sl + 0x30, &sb, 8) || !tk_rd(sl + 0x38, &sc, 4)) return;
    if (sc > 512) sc = 512; uint32_t pair[2];
    for (uint32_t i = 0; i < sc; i++) if (tk_rd(sb + 8 * i, pair, sizeof pair) && pair[0] == 6u << 16) { uint32_t to = g_hp1_to; if (pair[1] > to) *(uint32_t *)(sb + 8 * i + 4) = to; return; }
}
static void state_dump_monsters(uint64_t G, char *out, size_t cap, size_t *used) {   // 怪物表 G+0x2630：每只 id / 类别 / stat 100（人数档）/ stat 7（最大生命 ×256），至多 64 只 | Monster table G+0x2630: per monster id / class / stat 100 (/players setting) / stat 7 (max life ×256), at most 64 monsters
    int count = 0;
    int w = snprintf(out + *used, cap - *used, ",\"monsters\":["); if (w < 0 || (size_t)w >= cap - *used) return; *used += (size_t)w;
    for (int b = 0; b < 128 && count < 64; b++) {
        uint64_t u = 0; if (!tk_rd(G + 0x2630 + 8 * b, &u, 8)) continue;
        for (int k = 0; u && k < 4096 && count < 64; k++) {
            uint32_t id = 0, cls = 0, stat100 = 0, stat7 = 0;
            if (tk_rd(u + 4, &cls, 4) && tk_rd(u + 8, &id, 4) && stat_read(u, 100, &stat100) && stat_read(u, 7, &stat7)) {
                w = snprintf(out + *used, cap - *used, "%s{\"id\":%u,\"cls\":%u,\"stat100\":%u,\"stat7\":%u}", count ? "," : "", id, cls, stat100, stat7);
                if (w < 0 || (size_t)w >= cap - *used) return; *used += (size_t)w; count++;
            }
            if (!tk_rd(u + 0x158, &u, 8)) break;
        }
    }
    w = snprintf(out + *used, cap - *used, "]"); if (w >= 0 && (size_t)w < cap - *used) *used += (size_t)w;
}
static int tk_rd(uint64_t a, void *buf, size_t n) {           // = swtick Reader.rd：地址 0 不计数、直接无；读失败无 | = swtick Reader.rd: address 0 is not counted and is None directly; a failed read is None
    if (!a) return 0;
    g_tkreads++; vm_size_t got = 0;
    { int dm_guest_heap_readable(uint64_t, size_t); if (dm_guest_heap_readable(a, n)) { memcpy(buf, (const void *)(uintptr_t)a, n); return 1; } }   // 客体堆内：直接读（原先每个字段一次内核调用，主线程每拍几十次） | Inside the guest heap: read directly (it used to be one kernel call per field, dozens of times per tick on the main thread)
    return !vm_read_overwrite(mach_task_self(), a, n, (vm_address_t)buf, &got) && got == n;
}
typedef struct { uint32_t type, cls, id, mode, act, seed, rlo, rhi, x, y, xf, yf, it[5], has_it, nst; uint32_t st[96][2]; unsigned ord; } tk_unit;
static int tk_cmp(const void *a, const void *b) {
    const tk_unit *u = a, *v = b;
    if (u->type != v->type) return u->type < v->type ? -1 : 1;
    if (u->id != v->id) return u->id < v->id ? -1 : 1;
    return u->ord < v->ord ? -1 : u->ord > v->ord;        // 稳定：同键按采集次序（= Python sort） | Stable: equal keys keep collection order (= Python sort)
}
static uint64_t tk_fnv(uint64_t h, const void *p, size_t n) { for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull; return h; }

typedef struct { uint32_t id, cls, mode, page, x, y; unsigned ord; } state_item;
static int state_item_cmp(const void *a, const void *b) {
    const state_item *u = a, *v = b;
    if (u->id != v->id) return u->id < v->id ? -1 : 1;
    return u->ord < v->ord ? -1 : u->ord > v->ord;
}
static int state_read_item(uint64_t u, state_item *it, size_t ord, uint64_t *next) {   // 物品单位 → 一项；d+0xb0 = 链表下一件 | Item unit → one entry; d+0xb0 = next item in the list
    uint32_t id = 0, cls = 0, mode = 0, x = 0, y = 0; uint64_t d = 0, pp = 0; uint8_t page = 0;
    if (!tk_rd(u + 8, &id, 4) || !tk_rd(u + 4, &cls, 4) || !tk_rd(u + 0xC, &mode, 4) || !tk_rd(u + 0x10, &d, 8) || !d) return 0;
    if (!tk_rd(d + 0x55, &page, 1)) return 0;
    if (tk_rd(u + 0x38, &pp, 8) && pp) { tk_rd(pp + 0x10, &x, 4); tk_rd(pp + 0x14, &y, 4); }
    *it = (state_item){id, cls, mode, page, x, y, (unsigned)ord};
    *next = 0; tk_rd(d + 0xB0, next, 8);
    return 1;
}
enum { CLS_TBK = 533, CLS_TSC = 544 };   // 回城书 / 回城卷轴（原版 misc.txt 行序 + 武器 306 + 护甲 217，跳过 Expansion 分隔行；与手机存档 tsc=544 对上） | Tome of Town Portal / Scroll of Town Portal (the original game's misc.txt row order + weapons 306 + armor 217, skipping the Expansion separator row; matches tsc=544 in the phone save)
static uint64_t g_player_unit;   // 最近一拍找到的受控玩家单位（0 = 没读到） | Controlled player unit found on the most recent tick (0 = not read)
// 测试开关 DM_TEST_WORLD_SEEK（造现场用，客户端一侧在 dm_world.c w_test_seek_step）的服务端一半：探路不走路，用游戏服务端自己的传送 main+0x7cc530(游戏, 单位, 房间, x, y, 0, 0)
// ——传送技能、传送阵、任务挪人都调它（D2MOO sub_6FCBDFE0）：房间给 0 时它在单位当前房间与相邻房间里按坐标找，在落点旁找空位，给客户端发重定位包。
// 所以一次只能跳到相邻房间：按关卡房间的矩形贴边关系做广度优先，朝「没去过的房间」一间一间跳；门、怪、墙都不挡。人进了一间房，相邻房间就会载入，要找的物件随之出现。
// 关卡结构与客户端同一份公共库布局（dm_world.c 探路一节）：单位 +0x38 路径、路径 +0x20 房间、房间 +0x18 关卡房间、关卡房间 +0x90 关卡 / +0x48 下一个 / +0x60.. 矩形（×5 = 格）。
// g_dm_seek_tp：1 = 探路中（这里每 6 拍跳一间）；3 = 客户端已看见目标，请传到它旁边（坐标在 g_dm_seek_tp_x / y）；2 = 已传到；0 = 没在用。
// Server half of the test switch DM_TEST_WORLD_SEEK (for test setup; the client side is w_test_seek_step in dm_world.c): seeking does not walk, it uses the game server's own teleport main+0x7cc530(game, unit, room, x, y, 0, 0)
// -- the Teleport skill, teleport pads and quest relocation all call it (D2MOO sub_6FCBDFE0): with room 0 it searches by coordinates in the unit's current room and adjacent rooms, finds a free spot near the landing point, and sends the client a reposition packet.
// So one jump can only reach an adjacent room: do a breadth-first search over the edge-sharing relation of the level rooms' rectangles and jump room by room toward "unvisited rooms"; doors, monsters and walls do not block. Once the player is in a room, the adjacent rooms get loaded and the object being sought shows up with them.
// The level structures use the same common-library layout as the client (seeking section of dm_world.c): unit +0x38 path, path +0x20 room, room +0x18 level room, level room +0x90 level / +0x48 next / +0x60.. rectangle (×5 = cells).
// g_dm_seek_tp: 1 = seeking (here it jumps one room every 6 ticks); 3 = the client has seen the target, please teleport next to it (coordinates in g_dm_seek_tp_x / y); 2 = teleported; 0 = not in use.
_Atomic int g_dm_seek_tp; int g_dm_seek_tp_x, g_dm_seek_tp_y;
static void seek_tp_step(uint64_t G, uint64_t pl, module_t *main) {
    static struct { uint64_t r; int x, y, w, h; uint8_t st, miss; } rm[400]; static int n, hops, bias_on = -1, bias_in, bx, by; static uint64_t lv0; static uint32_t next_tk;
    int mode = atomic_load(&g_dm_seek_tp); uint32_t tk = 0; if (!mode || mode == 2 || !tk_rd(G + 0x170, &tk, 4) || tk < next_tk) return; next_tk = tk + 6;
    int (*tp)(uint64_t, uint64_t, uint64_t, int, int, int, int) = (int (*)(uint64_t, uint64_t, uint64_t, int, int, int, int))(main->base + 0x7cc530);
    uint64_t pp = 0, ar = 0, dr = 0, lv = 0; uint16_t c[4] = {0}; if (!tk_rd(pl + 0x38, &pp, 8) || !pp || !tk_rd(pp, c, 8) || !tk_rd(pp + 0x20, &ar, 8) || !ar || !tk_rd(ar + 0x18, &dr, 8) || !dr || !tk_rd(dr + 0x90, &lv, 8) || !lv) return;
    int px = c[1], py = c[3];
    if (mode == 3) { int ok = tp(G, pl, 0, g_dm_seek_tp_x, g_dm_seek_tp_y, 0, 0); tk_rd(pp, c, 8); LOG("传送探路：拍 %u 传到目标 %d,%d 旁：%s，现在 %d,%d（共跳 %d 次）", tk, g_dm_seek_tp_x, g_dm_seek_tp_y, ok ? "成" : "不成（目标不在当前或相邻房间 / 旁边没空位）", c[1], c[3], hops); atomic_store(&g_dm_seek_tp, 2); return; }
    if (bias_on < 0) { const char *e = getenv("DM_TEST_SEEK_BIAS"); bias_on = e && sscanf(e, "%d,%d", &bx, &by) == 2; }
    if (lv != lv0) { lv0 = lv; n = 0; hops = 0; uint64_t r = 0; tk_rd(lv + 0x10, &r, 8); while (r && n < 400) { int q[4] = {0}; if (!tk_rd(r + 0x60, q, 16)) break; rm[n].r = r; rm[n].x = q[0] * 5; rm[n].y = q[1] * 5; rm[n].w = q[2] * 5; rm[n].h = q[3] * 5; rm[n].miss = 0; rm[n++].st = 0; if (!tk_rd(r + 0x48, &r, 8)) break; }
        bias_in = 0; for (int i = 0; i < n && bias_on; i++) bias_in |= bx >= rm[i].x && bx < rm[i].x + rm[i].w && by >= rm[i].y && by < rm[i].y + rm[i].h;   // 偏向点不在这个关卡的任何房间里（多段探路、换了关卡）就不用它 | If the bias point is not inside any room of this level (multi-segment seeking, the level changed), do not use it
        uint32_t id = 0; tk_rd(lv + 0x1f8, &id, 4); LOG("传送探路：拍 %u 关卡 %u，关卡房间 %d 间，玩家 %d,%d%s", tk, id, n, px, py, bias_in ? "，按偏向点选房" : ""); }
    int ci = -1; for (int i = 0; i < n; i++) if (rm[i].r == dr) ci = i; if (ci < 0) return; if (rm[ci].st != 2) rm[ci].st = 2;
    // 广度优先：从当前房间出发，经「贴边且没被记成跳不进」的房间；候选 = 走得到的没去过的房间，有偏向点取离它最近的，否则取层数最少的
    // Breadth-first: start from the current room and go through rooms that "share an edge and are not marked cannot-jump-into"; candidates = reachable unvisited rooms; with a bias point take the one nearest to it, otherwise the one with the fewest layers
    static short par[400], que[400]; int qh = 0, qt = 0, best = -1; long bd = -1; for (int i = 0; i < n; i++) par[i] = -1; par[ci] = (short)ci; que[qt++] = (short)ci;
    while (qh < qt) { int a = que[qh++]; if (rm[a].st == 0) { long d = bias_in ? (long)(rm[a].x + rm[a].w / 2 - bx) * (rm[a].x + rm[a].w / 2 - bx) + (long)(rm[a].y + rm[a].h / 2 - by) * (rm[a].y + rm[a].h / 2 - by) : qh; if (bd < 0 || d < bd) { bd = d; best = a; } }
        for (int j = 0; j < n; j++) { if (par[j] >= 0 || rm[j].st == 3) continue; int ox = rm[a].x < rm[j].x + rm[j].w && rm[j].x < rm[a].x + rm[a].w, oy = rm[a].y < rm[j].y + rm[j].h && rm[j].y < rm[a].y + rm[a].h;
            int tx = rm[a].x + rm[a].w == rm[j].x || rm[j].x + rm[j].w == rm[a].x, ty = rm[a].y + rm[a].h == rm[j].y || rm[j].y + rm[j].h == rm[a].y; if ((tx && oy) || (ty && ox)) { par[j] = (short)a; que[qt++] = (short)j; } } }
    if (best < 0) { int v = 0, bad = 0; for (int i = 0; i < n; i++) { v += rm[i].st == 2; bad += rm[i].st == 3; } LOG("传送探路：拍 %u 跳得到的房间都去过了（%d 间里去过 %d、跳不进 %d，共跳 %d 次），没等到目标", tk, n, v, bad, hops); atomic_store(&g_dm_seek_tp, 0); return; }
    int step = best; while (par[step] != ci) step = par[step];
    static const int8_t at[5][2] = {{2, 2}, {1, 1}, {3, 1}, {1, 3}, {3, 3}}; int ok = 0, hx = 0, hy = 0; for (int k = 0; k < 5 && !ok; k++) { hx = rm[step].x + rm[step].w * at[k][0] / 4; hy = rm[step].y + rm[step].h * at[k][1] / 4; ok = tp(G, pl, 0, hx, hy, 0, 0); }
    if (!ok) { rm[step].st = 3; LOG("传送探路：拍 %u 跳不进房 (%d,%d) %dx%d（五个落点都不成），记成跳不进", tk, rm[step].x, rm[step].y, rm[step].w, rm[step].h); return; }
    hops++; tk_rd(pp, c, 8);
    // 传送成了但人没落在那间房里（那间整间站不了人，如火焰之河的岩浆房：游戏在旁边找了空位）：两次都这样就把它记成跳不进，否则会对着同一间一直跳（火焰之河 474 跳原地不动）
    // The teleport succeeded but the player did not land in that room (nobody can stand anywhere in that room, e.g. a lava room in the River of Flame: the game found a free spot nearby): if this happens twice, mark it as cannot-jump-into, otherwise it would keep jumping at the same room forever (River of Flame 474 jumped in place without moving)
    int inroom = c[1] >= rm[step].x && c[1] < rm[step].x + rm[step].w && c[3] >= rm[step].y && c[3] < rm[step].y + rm[step].h; if (!inroom && ++rm[step].miss >= 2) rm[step].st = 3;
    if (hops <= 40 || hops % 20 == 0 || !inroom) LOG("传送探路：拍 %u 第 %d 跳 → 房 (%d,%d)，落在 %d,%d%s（目标房 (%d,%d)）", tk, hops, rm[step].x, rm[step].y, c[1], c[3], inroom ? "" : rm[step].st == 3 ? "，两次都没落进去，记成跳不进" : "，没落进这间", rm[best].x, rm[best].y);
}
// ---------- 合成读数（常开）：服务端收到合成命令包 0x20 的那一拍记一行现场，2 秒后再记一行结果 ----------
// 玩家报「点了合成没反应」时一读就能定根：请求到没到服务端、手上（光标）有没有物品、魔盒里是什么、任务字与本局已开门标志、事后魔盒变没变、营地出没出红门。
// 原版合成失败没有任何提示（服务端校验 0x6e8740 / 配方匹配 0x702f60 / 开门 0x720a00 拒绝都不回包），所以只记日志、不加提示。
// 读法：玩家物品栏 [玩家+0x90]，光标物品 [物品栏+0x40]（0x6e8740 就是按它拒的）；物品链 [物品栏+0x10] 起，数据块 [物品+0x10]、下一件 [数据块+0xb0]、页 数据块+0x55（3 = 魔盒）、
// 品质 数据块+0；码 0x570260(物品)；任务物品来源难度 = 属性 356（op 28 要求它等于当前难度 G+0x104）；任务块 [[[玩家+0x10]+0x40+难度×8]]，与 .d2s 每难度 96 字节相同；
// 本局已开奶牛门 = [[[G+0x21d8]+0x10]] 字节 +9 位 3（0x720a2c–0x720a38）。
// ---------- Transmute readout (always on): log one line of state on the tick the server receives transmute command packet 0x20, and one more line of the result 2 seconds later ----------
// When a player reports "tapped transmute and nothing happened", one read pins down the root cause: whether the request reached the server, whether there is an item in hand (cursor), what is in the cube, the quest word and this run's portal-opened flag, whether the cube changed afterwards, whether a red portal appeared in camp.
// The original game gives no feedback at all when a transmute fails (server validation 0x6e8740 / recipe match 0x702f60 / portal open 0x720a00 all refuse without a reply packet), so we only log and add no prompt.
// How to read: player inventory object [player+0x90], cursor item [inventory object+0x40] (this is what 0x6e8740 refuses on); the item chain starts at [inventory object+0x10], data block [item+0x10], next item [data block+0xb0], page at data block+0x55 (3 = cube),
// quality at data block+0; code 0x570260(item); quest item source difficulty = stat 356 (op 28 requires it to equal the current difficulty G+0x104); quest block [[[player+0x10]+0x40+difficulty×8]], same as the 96 bytes per difficulty in .d2s;
// cow portal already opened in this run = [[[G+0x21d8]+0x10]] byte +9 bit 3 (0x720a2c–0x720a38).
static struct { uint64_t G; uint32_t at; int n0; char before[400]; } g_cube;
static int cube_items(uint64_t player, char *o, int cap) {   // 魔盒（页 3）里的物品：返回件数，o 里写「码/品质/来源难度」 | Items in the cube (page 3): returns the count and writes "code/quality/source difficulty" into o
    module_t *mm = main_mod(); uint64_t inv = 0, it = 0; int n = 0, k = 0; o[0] = 0; if (!mm || !tk_rd(player + 0x90, &inv, 8) || !inv || !tk_rd(inv + 0x10, &it, 8)) return -1;
    for (int c = 0; it && c < 400; c++) { uint64_t d = 0; if (!tk_rd(it + 0x10, &d, 8) || !d) break;
        if (*(uint8_t *)(uintptr_t)(d + 0x55) == 3) { uint32_t code = ((uint32_t (*)(uint64_t))(mm->base + 0x570260))(it); char cs[5]; memcpy(cs, &code, 4); cs[4] = 0; for (int i = 3; i >= 0 && cs[i] == ' '; i--) cs[i] = 0;
            uint64_t pp = 0; int32_t gx = -1, gy = -1; uint32_t id = 0, md = 0; tk_rd(it + 8, &id, 4); tk_rd(it + 0xc, &md, 4); if (tk_rd(it + 0x38, &pp, 8) && pp) { tk_rd(pp + 0x10, &gx, 4); tk_rd(pp + 0x14, &gy, 4); }
            k += snprintf(o + k, cap - k, "%sid%u@%d,%d/%s/品质%u/来源难度%d/模式%u", n ? " " : "", id, gx, gy, cs, *(uint32_t *)(uintptr_t)d, ((int (*)(uint64_t, int, int))(mm->base + 0x5b9290))(it, 356, 0), md); if (k >= cap) k = cap - 1; n++; }
        if (!tk_rd(d + 0xb0, &it, 8)) break; }
    return n;
}
static void cube_diag_req(uint64_t G, uint64_t player, const uint8_t *pkt, uint32_t len) {
    if (!G || !player) return;
    uint64_t inv = 0, cur = 0, pd = 0, qr = 0, q = 0, ca = 0, cb = 0, cc = 0; uint32_t pm = 0, tq = 0, cn = 0; uint8_t diff = 0, xp = 0, opened = 0xff;
    tk_rd(G + 0x170, &tq, 4); tk_rd(G + 0x104, &diff, 1); tk_rd(G + 0x106, &xp, 1); tk_rd(player + 0xc, &pm, 4);
    if (tk_rd(player + 0x90, &inv, 8) && inv) tk_rd(inv + 0x40, &cur, 8);
    if (tk_rd(player + 0x10, &pd, 8) && pd && tk_rd(pd + 0x40 + 8 * diff, &qr, 8) && qr) tk_rd(qr, &q, 8);
    uint16_t w4 = 0, w26 = 0, w40 = 0; if (q) { tk_rd(q + 8, &w4, 2); tk_rd(q + 52, &w26, 2); tk_rd(q + 80, &w40, 2); }
    if (tk_rd(G + 0x21d8, &ca, 8) && ca && tk_rd(ca + 0xc, &cn, 4) && cn && tk_rd(ca + 0x10, &cb, 8) && cb && tk_rd(cb, &cc, 8) && cc) tk_rd(cc + 9, &opened, 1);
    char items[400]; int n = cube_items(player, items, sizeof items), lv = 0; { uint64_t pp = 0, r = 0, r2 = 0, l = 0; if (tk_rd(player + 0x38, &pp, 8) && pp && tk_rd(pp + 0x20, &r, 8) && r && tk_rd(r + 0x18, &r2, 8) && r2 && tk_rd(r2 + 0x90, &l, 8) && l) tk_rd(l + 0x1f8, &lv, 4); }
    // 包：[1..4] 魔盒 id、[5] 页、[6] 格、[7] = 原料数 − 1，之后每 5 字节一项 {物品 id, 格字节（低 4 位 x、高 4 位 y）}（0x6e8938–0x6e8b0c 按这些逐项核，对不上静默丢）
    // Packet: [1..4] cube id, [5] page, [6] cell, [7] = ingredient count − 1, then 5 bytes per entry {item id, cell byte (low 4 bits x, high 4 bits y)} (0x6e8938–0x6e8b0c checks these entry by entry and silently drops on mismatch)
    char hx[160]; int k = 0; uint32_t bid = 0; if (len >= 8) memcpy(&bid, pkt + 1, 4);
    k += snprintf(hx + k, sizeof hx - k, "魔盒 id%u 原料 %d 项：", bid, len >= 8 ? pkt[7] + 1 : -1);
    for (uint32_t i = 8; i + 5 <= len && k < (int)sizeof hx - 24; i += 5) { uint32_t id; memcpy(&id, pkt + i, 4); k += snprintf(hx + k, sizeof hx - k, " id%u@%d,%d", id, pkt[i + 4] & 15, pkt[i + 4] >> 4); }
    LOG("合成读数：拍 %u 服务端收到合成请求（%s；长 %u）｜关卡 %d 难度 %u 版本字节 %u 玩家模式 %u｜光标物品 %#llx%s｜魔盒 %d 件：%s｜任务字 4=%#06x 26=%#06x 40=%#06x｜本局已开门字节 %s%#x",
        tq, hx, len, lv, diff, xp, pm, (unsigned long long)cur, cur ? "（非空：原版 0x6e8740 会丢掉这次合成）" : "（空）", n, items, w4, w26, w40, opened == 0xff ? "读不到 " : "", opened == 0xff ? 0 : opened);
    g_cube.G = G; g_cube.at = tq + 50; g_cube.n0 = n; strlcpy(g_cube.before, items, sizeof g_cube.before);
}
static void cube_diag_tick(uint64_t G, uint64_t player) {   // 合成请求后 2 秒：魔盒变没变、玩家所在关卡有没有红门（物件 60） | 2 seconds after a transmute request: whether the cube changed, and whether the player's level has a red portal (object 60)
    uint32_t tq = 0; if (!g_cube.at || G != g_cube.G || !player || !tk_rd(G + 0x170, &tq, 4) || (int32_t)(tq - g_cube.at) < 0) return;
    g_cube.at = 0; char items[400]; int n = cube_items(player, items, sizeof items), red = 0;
    for (int b = 0; b < 128; b++) { uint64_t u = 0; if (!tk_rd(G + 0x2a30 + 8 * b, &u, 8)) continue; for (int c = 0; u && c < 512; c++) { uint32_t cl = 0; tk_rd(u + 4, &cl, 4); red += cl == 60; if (!tk_rd(u + 0x158, &u, 8)) break; } }
    LOG("合成读数：拍 %u 合成后 2 秒｜魔盒 %d → %d 件%s：%s｜本局红门（物件 60）%d 扇", tq, g_cube.n0, n, strcmp(items, g_cube.before) ? "（变了）" : "（没变：服务端没执行配方）", items, red);
}

// 客户端自己记的本地玩家单位号（main+0x2d20e4c 本地玩家槽号 → main+0x2d60380[槽号]，原版 0x45f9d0 同口径）；读不到（还没进局）给 0xffffffff。
// 「谁是受控玩家」三处都按它认：每拍功能的入口 hud_tick、状态转储 state_dump、位置核对 poschk。玩家表里角色死后留下的尸体也是「模式 ≠ 0、有坐标」的玩家类单位，
// 只按在场数会把「活人 + 自己的尸体」报成不唯一——那是量具口径旧，不是场上有两个玩家
// The local player unit id as recorded by the client itself (main+0x2d20e4c local player slot → main+0x2d60380[slot], same reading as the original 0x45f9d0); 0xffffffff if unreadable (not in a game yet).
// All three "who is the controlled player" sites go by it: the per-tick feature entry hud_tick, the state dump state_dump, the position check poschk. A corpse left in the player table after the character dies is also a player-type unit with "mode ≠ 0 and coordinates",
// so counting units present reports "living player + own corpse" as non-unique. That is a stale measuring criterion, not two players on the field
static uint32_t local_player_id(void) { uint32_t slot = 8, lid = 0xffffffffu; module_t *mm = main_mod(); if (mm && tk_rd((uint64_t)mm->base + 0x2d20e4c, &slot, 4) && slot < 8) tk_rd((uint64_t)mm->base + 0x2d60380 + 4 * slot, &lid, 4); return lid; }
static void hud_tick(uint64_t G) {        // 每拍：受控玩家等级 / 本级经验‰ / 腰带四列件数 / 回城次数 + 当前人数档 → g_hud（前端 dm_hud_get） | Every tick: controlled player's level / current-level experience ‰ / item counts of the four belt columns / town portal count + current /players setting → g_hud (front end dm_hud_get)
    dm_hud_info h = {0};
    uint64_t player = 0, inv = 0, u = 0, next = 0;
    // 受控玩家 = 客户端自己记的本地玩家单位号那个单位（main+0x2d20e4c 本地玩家槽号 → main+0x2d60380[槽号] 单位号，原版 0x45f9d0 同口径），
    // 且模式 ≠ 0、坐标 ≠ (0,0)。原先只按「玩家表里第一个模式 ≠ 0 且有坐标的单位」取：角色死过一次后地上留着尸体（也是玩家类单位、模式 17、有坐标），
    // 再死的那几拍里真玩家模式是 0（正在死），取到的就成了尸体——首领连战 / 秘境 / 免费回城 / 个人仓库 / 编年史 / 百宝囊全都把尸体当成「换了玩家」重新初始化一遍，
    // 等真玩家变成模式 17 又换回来再初始化一遍（宿主实测：首领连战里第二次死亡那一刻日志连出两轮各模块的进局行，连战进度清零）。
    // 认出了单位号就只认它：它正在死的那几拍这一拍没有受控玩家（和没有尸体时第一次死一样），绝不退到别的玩家类单位。读不到单位号（还没进局）才照旧扫
    // Controlled player = the unit whose unit id is the local player unit id the client itself records (main+0x2d20e4c local player slot number → main+0x2d60380[slot number] unit id, same convention as the original game's 0x45f9d0),
    // with mode ≠ 0 and coordinates ≠ (0,0). Previously it was taken only as "the first unit in the player table with mode ≠ 0 and with coordinates": after the character has died once a corpse stays on the ground (also a player-class unit, mode 17, with coordinates),
    // and during the few ticks of dying again the real player's mode is 0 (dying), so what got taken was the corpse -- boss rush / rift / free town portal / personal stash / Chronicle / pouch all treated the corpse as "the player changed" and re-initialized once,
    // and when the real player became mode 17 it switched back and they initialized once more (measured on the host: at the moment of the second death in a boss rush the log showed two rounds in a row of every module's run-entry lines, and the boss rush progress was reset to zero).
    // Once the unit id is known, only that unit is accepted: during the few ticks it is dying there is no controlled player this tick (the same as a first death with no corpse), and it never falls back to another player-class unit. Only when the unit id cannot be read (not in a run yet) does it scan as before
    uint32_t lid = local_player_id();
    for (int b = 0; b < 128 && !player; b++) {
        if (!tk_rd(G + 0x2230 + 8 * b, &u, 8)) continue;
        for (int k = 0; u && k < 4096; k++) {
            uint32_t mode = 0, id = 0; uint64_t pp = 0; uint16_t c[4] = {0};
            if (tk_rd(u + 0xC, &mode, 4) && mode && tk_rd(u + 0x38, &pp, 8) && pp && tk_rd(pp, c, 8) && (c[1] || c[3]) && (lid == 0xffffffffu || (tk_rd(u + 8, &id, 4) && id == lid))) { player = u; break; }
            if (!tk_rd(u + 0x158, &u, 8)) break;
        }
    }
    uint32_t sig = 0;
    if (player && tk_rd(player + 0x90, &inv, 8) && tk_rd(inv, &sig, 4) && sig == 0x01020304) {
        { static int hp1 = -1; if (hp1 < 0) { const char *e = getenv("DM_TEST_HP1"); hp1 = e != NULL; if (e && e[0] == '0') g_hp1_to = 0; } if (hp1 && cur_tick() > 200) test_hp1(player); }
        uint32_t lv = 0, xp = 0;
        stat_read(player, 12, &lv); stat_read(player, 13, &xp);
        // 生命 / 法力（定点 ×256）走游戏自己的取属性函数 main+0x5b9290(单位, 属性号, 0)：属性表 +0x1c < 0 时读累计层 +0xe8（含装备 / 护身符加成，角色页同口径），
        // 否则读基础层 +0x30。stat_read 只读基础层，带 +生命 / +法力的装备时上限偏小、出现「当前 > 上限」（88 级法师 967 读成 809）
        // Life / mana (fixed point ×256) go through the game's own stat getter main+0x5b9290(unit, stat number, 0): when stat table +0x1c < 0 it reads the accumulated layer +0xe8 (including gear / charm bonuses, same convention as the character page),
        // otherwise it reads the base layer +0x30. stat_read reads only the base layer, so with +life / +mana gear the maximum comes out low and "current > max" appears (the level 88 Sorceress's 967 was read as 809)
        module_t *mm = main_mod();
        if (mm) { int (*get)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(mm->base + 0x5b9290);
            h.hp = get(player, 6, 0) >> 8; h.hp_max = get(player, 7, 0) >> 8; h.mp = get(player, 8, 0) >> 8; h.mp_max = get(player, 9, 0) >> 8; }
        uint32_t sp = 0, kp = 0, pm = 0; stat_read(player, 4, &sp); stat_read(player, 5, &kp); h.points = (int)(sp + kp);
        if (tk_rd(player + 0xC, &pm, 4)) h.dead = dm_hud_player_mode_dead(pm);   // 单位 +0xC = 动画模式（dm_hud.h） | Unit +0xC = animation mode (dm_hud.h)
        // 手上物品：这里是服务端玩家，只作诊断；h.holding 由 dm_hud_get 取客户端的真值（dm_hud.c）。两端持续不一致（服务端光标挂着、客户端手上空了）记红行：
        // 卡死就是这样——服务端还挂着放进仓库的魔盒，原先拿服务端这一份当「手上有东西」，拖放收尾链一遍遍按返回关面板
        // Held item: this is the server-side player, for diagnostics only; h.holding takes the client's true value via dm_hud_get (dm_hud.c). If the two sides stay inconsistent (server cursor still holding, client hand empty), log a red line:
        // that is exactly the hang: the server still held the Horadric Cube that had been put into the stash, the old code took the server copy as "something in hand", and the drag-and-drop wrap-up chain kept pressing Back to close panels
        { uint64_t cur = 0; tk_rd(inv + 0x40, &cur, 8);
          static uint64_t lcur; if (cur != lcur) { uint32_t id = 0; if (cur) tk_rd(cur + 8, &id, 4); LOG("手上物品（服务端）：%s（物品 id %u）", cur ? "拿着" : "空了", id); lcur = cur; }   // 每次变化记一行：「看不见的手上物品」排查用 | Logs one line on every change: for investigating the "invisible held item"
          static uint64_t mcur; static int mn; if (cur && !dm_hud_held()) { if (++mn == 50 && cur != mcur) { uint32_t id = 0; tk_rd(cur + 8, &id, 4); mcur = cur; LOG("【红】手上物品：服务端光标挂着物品（id %u）、客户端手上已空 2 秒——两端不一致，按客户端（手上空）处理", id); } } else mn = 0; }   // 非零 = 拿着；值随物品不同（dm_cursor 的「放不下往里挪」靠它认出换成了别的物品）   // 物品栏 +0x40 = 光标上拿着的物品（同 DM_STATE 的 cursor） | Nonzero = holding; the value differs per item (dm_cursor's "can't place, shift inward" relies on it to detect that the item was swapped for another)   // Inventory +0x40 = item held on the cursor (same as DM_STATE's cursor)
        h.level = (int)lv;
        { uint64_t pp = 0, r1 = 0, r2 = 0, lvl = 0; uint16_t c[4] = {0}; uint32_t no = 0;   // 坐标：单位 +0x38 → 路径头 4 个 16 位（x 小数 / x / y 小数 / y） | Coordinates: unit +0x38 → 4 16-bit values at the head of the path (x fraction / x / y fraction / y)
          if (tk_rd(player + 0x38, &pp, 8) && tk_rd(pp, c, 8)) { h.x = c[1]; h.y = c[3]; }
          atomic_store(&g_player_path, pp);   // 连拍（dm_gfx_burst）逐帧读位置用 | used by burst capture (dm_gfx_burst) to read the position per frame
          // 区域号（levels.txt 的 Id：1 = 罗格营地，2 = 鲜血荒地…）：路径 +0x20 → 所在房间 +0x18 → 区域房间 +0x90 → 区域 +0x1f8
          // Area number (Id in levels.txt: 1 = Rogue Encampment, 2 = Blood Moor...): path +0x20 → current room +0x18 → area room +0x90 → area +0x1f8
          if (pp && tk_rd(pp + 0x20, &r1, 8) && tk_rd(r1 + 0x18, &r2, 8) && tk_rd(r2 + 0x90, &lvl, 8) && tk_rd(lvl + 0x1f8, &no, 4) && no < 200) h.area = (int)no; }
        if (lv >= 99) h.exp_permille = 1000;
        else if (lv >= 1) {
            uint32_t lo = k_exp[lv - 1], hi = k_exp[lv];
            h.exp_permille = hi > lo && xp > lo ? (int)((uint64_t)(xp - lo) * 1000 / (hi - lo)) : 0;
            if (h.exp_permille > 1000) h.exp_permille = 1000;
        }
        state_item it;
        if (tk_rd(inv + 0x10, &u, 8))
            for (unsigned k = 0; u && k < 1024 && state_read_item(u, &it, 0, &next); k++) {
                if (it.mode == 2) h.belt[it.x % 4]++;                       // 腰带物品 x = 0..15，列 = x % 4（与 LEFT/UP/RIGHT/DOWN 对应） | Belt items x = 0..15, column = x % 4 (matching LEFT/UP/RIGHT/DOWN)
                if (it.cls == CLS_TSC) h.portal++;
                if (it.cls == CLS_TBK) { uint32_t q = 0; if (stat_read(u, 70, &q)) h.portal += (int)q; }   // stat 70 = quantity
                u = next;
            }
        h.valid = 1;
    }
    module_t *main = main_mod(); uint32_t pl = 0;
    if (main) { void *obj = players_obj(main); if (obj && tk_rd((uint64_t)obj + 0xc, &pl, 4)) h.players = (int)pl; }
    static int logged;
    g_player_unit = player; { uint8_t gv; extern _Atomic int g_dm_game_ver; if (tk_rd(G + 0x106, &gv, 1)) atomic_store(&g_dm_game_ver, gv); }   // 房间初始化要的数据版本参数（dm_world.c 小地图标出小站与出入口用） | Data version parameter needed for room initialization (used by dm_world.c to mark waypoints and entrances/exits on the minimap)
    { uint32_t tq = 0; static int once; const char *tg = getenv("DM_TEST_GOLD"); if (tg && main && player && tk_rd(G + 0x170, &tq, 4) && tq >= 30 && !once++) ((void (*)(uint64_t, int, int, int))(main->base + 0x5bc1f0))(player, 14, atoi(tg), 0); }   // 测试开关 DM_TEST_GOLD=<数>：进局第 30 拍把身上金币设成该数（验金币数量框用；标准存档金币为 0，数量弹窗开不出来） | Test switch DM_TEST_GOLD=<amount>: at tick 30 after joining the run set the gold on person to that amount (for verifying the gold amount box; the standard save has 0 gold, so the amount popup cannot be opened)
    // 测试开关 DM_TEST_GOTO_LEVEL=<拍>:<关卡>[;<拍>:<关卡>…]（造现场用）：到拍调原版换关 main+0x634000(游戏, 玩家, 关卡, 11)——王座进世界之石大殿那一下（0x755d38），
    // 可跨幕、不要求开过那一幕的小站（新建的标准术士也能进；小站传送 0x6aaea0 对没开那幕的角色会断言 main+0x6ab2d8）。小地图终点逐图核对用
    // Test switch DM_TEST_GOTO_LEVEL=<tick>:<level>[;<tick>:<level>...] (for test setup): at the tick call the original game's level change main+0x634000(game, player, level, 11) -- the one used going from the Throne into the Worldstone Chamber (0x755d38);
    // it can cross acts and does not require that act's waypoint to have been activated (a freshly created standard Warlock can enter too; waypoint travel 0x6aaea0 asserts at main+0x6ab2d8 for a character that has not unlocked that act). Used for checking minimap goals map by map
    { static char q[300]; static int init; uint32_t tq = 0; if (!init) { init = 1; const char *e = getenv("DM_TEST_GOTO_LEVEL"); if (e) strlcpy(q, e, sizeof q); }
      if (q[0] && main && player && tk_rd(G + 0x170, &tq, 4)) { unsigned at = 0, to = 0; int nn = 0;
          if (sscanf(q, "%u:%u%n", &at, &to, &nn) >= 2 && tq >= at) { const char *rest = q + nn + (q[nn] == ';'); memmove(q, rest, strlen(rest) + 1);
              ((void (*)(uint64_t, uint64_t, int, int))(main->base + 0x634000))(G, player, (int)to, 11); LOG("DM_TEST_GOTO_LEVEL：拍 %u 换到关卡 %u", tq, to); } } }
    // 测试开关 DM_TEST_MAP_FULL=<拍>：到拍切到全屏地图（同触摸点小地图，dm_ui_map_full；要小地图本来开着）
    // Test switch DM_TEST_MAP_FULL=<tick>: at the tick switch to the full-screen map (same as tapping the minimap by touch, dm_ui_map_full; the minimap must already be open)
    { static int at = -1; uint32_t tq = 0; if (at < 0) { const char *e = getenv("DM_TEST_MAP_FULL"); at = e ? atoi(e) : 0; }
      if (at > 0 && main && player && tk_rd(G + 0x170, &tq, 4) && tq >= (uint32_t)at) { at = 0; void dm_ui_map_full(int); dm_ui_map_full(1); LOG("DM_TEST_MAP_FULL：拍 %u 切全屏地图", tq); } }
    // 测试开关 DM_TEST_KILL=<拍>：到该拍让受控玩家真死一次——调游戏服务端自己的「开始死亡模式」main+0x6cd2c0(G, 玩家, 0, 模式 0, 攻击者类型 0, 攻击者 id = 自己, 0, 0)，
    // 就是伤害结算在生命归零后对玩家发的那一次调用（0x7d9508），后面的死亡动画、死亡画面、重生留尸体都是游戏自己走；不是直接写血量（那样不触发死亡）
    // Test switch DM_TEST_KILL=<tick>: at that tick make the controlled player really die once -- call the game server's own "start death mode" main+0x6cd2c0(G, player, 0, mode 0, attacker type 0, attacker id = self, 0, 0),
    // which is the very call damage resolution makes on the player after life reaches zero (0x7d9508); the death animation, death screen and respawn leaving a corpse that follow are all done by the game itself; this is not writing life directly (that does not trigger death)
    { uint32_t tq = 0, pid = 0; static int killed; const char *tk = getenv("DM_TEST_KILL");
      if (tk && main && player && !killed && tk_rd(G + 0x170, &tq, 4) && tq >= (uint32_t)atoi(tk) && tk_rd(player + 8, &pid, 4)) { killed = 1;
          int r = ((int (*)(uint64_t, uint64_t, uint64_t, int, int, uint32_t, int, int))(main->base + 0x6cd2c0))(G, player, 0, 0, 0, pid, 0, 0); LOG("DM_TEST_KILL：拍 %u 对玩家 id %u 开始死亡模式 → %d", tq, pid, r); } }
    { uint32_t tq = 0; static int once2; const char *ts = getenv("DM_TEST_STAT");   // 测试开关 DM_TEST_STAT=<属性号>=<值>[,…]：进局第 30 拍设玩家属性（5 剩余技能点、15 箱内金币…；验加点 / 买卖用） | Test switch DM_TEST_STAT=<stat number>=<value>[,...]: set player stats at tick 30 after joining the run (5 remaining skill points, 15 gold in stash...; for verifying point allocation / buying and selling)
      if (ts && main && player && tk_rd(G + 0x170, &tq, 4) && tq >= 30 && !once2++) { char buf[128]; strlcpy(buf, ts, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) { int a = 0, v = 0; if (sscanf(t, "%d=%d", &a, &v) == 2) ((void (*)(uint64_t, int, int, int))(main->base + 0x5bc1f0))(player, a, v, 0); } } }
    // 测试开关 DM_TEST_MERC_HP=<百分比>[@拍]（造现场用，缺省第 60 拍）：把佣兵当前生命设成上限的这个比例（服务端直接写属性 6，不走受伤流程：头像血条要等下一次生命变化才跟上）。
    // 设了这个变量就同时记佣兵生命读数：有变化时一行「佣兵 类号 id 生命 当前/上限」（每 25 拍最多一行，回血会一直变）。佣兵 = 怪物表里类号 271 / 338 / 359 / 560 / 561 且活着的那只。
    // Test switch DM_TEST_MERC_HP=<percent>[@tick] (for test setup, default tick 60): set the mercenary's current life to this fraction of its maximum (the server writes stat 6 directly, bypassing the damage flow: the portrait health bar only catches up on the next life change).
    // Setting this variable also logs mercenary life readouts: on change, one line 「佣兵 类号 id 生命 当前/上限」(mercenary class id life current/max) (at most one line per 25 ticks; regeneration keeps it changing). Mercenary = the live one in the monster table with class 271 / 338 / 359 / 560 / 561.
    { static int set_done, lhp = -1, lmx = -1; static uint32_t lt; static const char *e; static int e_got; if (!e_got) { e_got = 1; e = getenv("DM_TEST_MERC_HP"); }   /* 只取一次：这段每拍都走 | Fetch only once: this section runs every tick */ uint32_t tq = 0;
      if (e && main && player && tk_rd(G + 0x170, &tq, 4)) { int pct = 50, at = 60; sscanf(e, "%d@%d", &pct, &at); uint64_t merc = 0;
          for (int b = 0; b < 128 && !merc; b++) { uint64_t u = 0; if (!tk_rd(G + 0x2630 + 8 * b, &u, 8)) continue;
              for (int k = 0; u && k < 4096; k++) { uint32_t cls = 0, mode = 0; if (tk_rd(u + 4, &cls, 4) && tk_rd(u + 0xC, &mode, 4) && (cls == 271 || cls == 338 || cls == 359 || cls == 560 || cls == 561) && mode != 0 && mode != 12) { merc = u; break; } if (!tk_rd(u + 0x158, &u, 8)) break; } }
          if (merc) { int (*get)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(main->base + 0x5b9290); int hp = get(merc, 6, 0), mx = get(merc, 7, 0);
              if (!set_done && tq >= (uint32_t)at) { set_done = 1; ((void (*)(uint64_t, int, int, int))(main->base + 0x5bc1f0))(merc, 6, (int)((long long)mx * pct / 100), 0); hp = get(merc, 6, 0); LOG("DM_TEST_MERC_HP：拍 %u 把佣兵生命设成上限的 %d%% → %d/%d", tq, pct, hp >> 8, mx >> 8); lt = 0; }
              // 服务端「上次发过的百分比」（[佣兵单位+0x10]+0x54）：玩家的定时事件每 30 拍轮询一次（main+0x6cd870 → 0x6c26b0 → 0x63b580，进局后第一次在第 250 拍），算出的百分比与它不同才给客户端发生命通知（包 0x7f）并更新它。
              // 佣兵页「生命」一行与头像血条靠这条通知刷新，所以这行日志 = 「服务端这一拍发了通知」的读数（查「压血后面板仍满血」用）
              // The server's "last percentage sent" ([mercenary unit+0x10]+0x54): the player's timed event polls every 30 ticks (main+0x6cd870 → 0x6c26b0 → 0x63b580, first time at tick 250 after joining the run); only when the computed percentage differs from it does it send the client a life notification (packet 0x7f) and update it.
              // The 「生命」(Life) row on the mercenary page and the portrait health bar refresh from this notification, so this log line = the readout of "the server sent a notification on this tick" (for investigating "panel still shows full life after life was pushed down")
              { static int32_t lc = -12345; uint64_t md = 0; int32_t cv = 0; if (tk_rd(merc + 0x10, &md, 8) && md && tk_rd(md + 0x54, &cv, 4) && cv != lc) { if (lc != -12345) LOG("DM_TEST_MERC_HP：拍 %u 佣兵「上次发过的百分比」%d → %d", tq, lc, cv); lc = cv; } }
              if ((hp != lhp || mx != lmx) && (!lt || tq - lt >= 25)) { uint32_t cls = 0, id = 0; tk_rd(merc + 4, &cls, 4); tk_rd(merc + 8, &id, 4); LOG("DM_TEST_MERC_HP：拍 %u 佣兵 类号 %u id %u 生命 %d/%d", tq, cls, id, hp >> 8, mx >> 8); lhp = hp; lmx = mx; lt = tq; } } } }
    // 测试开关 DM_TEST_MERC_KILL=<拍>（造现场用）：到这一拍调游戏自己的杀怪函数（main+0x7d82b0(游戏, 佣兵, 攻击者=0, 1)）让佣兵死，走原版死亡流程（死亡动作、通知主人、头像消失、可找 NPC 复活），不是写血量。
    // Test switch DM_TEST_MERC_KILL=<tick> (for test setup): at this tick call the game's own kill function (main+0x7d82b0(game, mercenary, attacker=0, 1)) to make the mercenary die, going through the original game's death flow (death animation, owner notified, portrait disappears, can be revived at an NPC), not by writing life.
    { static int done; static uint64_t km; static uint32_t kid; static int ls = -1, lc = -1; static const char *e; static int e_got; if (!e_got) { e_got = 1; e = getenv("DM_TEST_MERC_KILL"); }   /* 只取一次：这段每拍都走 | Fetch only once: this section runs every tick */ uint32_t tq = 0;
      if (done && main && tk_rd(G + 0x170, &tq, 4)) { uint32_t id = 0, sm = 99, cm = 99;   // 杀后跟读：服务端与客户端两边这只佣兵的模式，有变化记一行（99 = 单位已不在） | Follow-up read after the kill: this mercenary's mode on both server and client, log a line on change (99 = unit no longer exists)
          if (tk_rd(km + 8, &id, 4) && id == kid) tk_rd(km + 0xC, &sm, 4); else sm = 99;
          for (uint64_t u = *(uint64_t *)(main->base + 0x2d603a0 + 1 * 0x400 + (kid & 0x7f) * 8); u; u = *(uint64_t *)(u + 0x158)) if (*(uint32_t *)(u + 8) == kid) { cm = *(uint32_t *)(u + 0xC); break; }
          if ((int)sm != ls || (int)cm != lc) { LOG("DM_TEST_MERC_KILL：拍 %u 佣兵 id %u 模式 服务端 %u 客户端 %u", tq, kid, sm, cm); ls = (int)sm; lc = (int)cm; } }
      if (e && !done && main && player && tk_rd(G + 0x170, &tq, 4) && tq >= (uint32_t)atoi(e)) { uint64_t merc = 0;
          for (int b = 0; b < 128 && !merc; b++) { uint64_t u = 0; if (!tk_rd(G + 0x2630 + 8 * b, &u, 8)) continue;
              for (int k = 0; u && k < 4096; k++) { uint32_t cls = 0, mode = 0; if (tk_rd(u + 4, &cls, 4) && tk_rd(u + 0xC, &mode, 4) && (cls == 271 || cls == 338 || cls == 359 || cls == 560 || cls == 561) && mode != 0 && mode != 12) { merc = u; break; } if (!tk_rd(u + 0x158, &u, 8)) break; } }
          if (merc) { done = 1; uint32_t cls = 0, id = 0, m0 = 0, m1 = 0; tk_rd(merc + 4, &cls, 4); tk_rd(merc + 8, &id, 4); km = merc; kid = id; tk_rd(merc + 0xC, &m0, 4);
              ((void (*)(uint64_t, uint64_t, uint64_t, int))(main->base + 0x7d82b0))(G, merc, 0, 1); tk_rd(merc + 0xC, &m1, 4);
              LOG("DM_TEST_MERC_KILL：拍 %u 杀佣兵 类号 %u id %u，模式 %u → %u（0 = 死亡中，12 = 已死）", tq, cls, id, m0, m1); } } }
    // 测试开关 DM_TEST_MON_KILL=<类号>[,<类号>…][@<拍数>]（造现场用，缺省 50）：这些类号的怪活着被看见满这么多拍后，调游戏自己的杀怪函数 main+0x7d82b0(游戏, 怪, 攻击者 = 受控玩家, 1) 杀掉——
    // 走原版死亡流程，任务的击杀回调照常触发、击杀算在玩家头上（打任务怪用：远古人 540 / 541 / 542、涅拉塞克 526、巴尔 544…）。每 5 拍扫一遍。
    // Test switch DM_TEST_MON_KILL=<class>[,<class>...][@<ticks>] (for test setup, default 50): once a monster of these classes has been seen alive for this many ticks, call the game's own kill function main+0x7d82b0(game, monster, attacker = controlled player, 1) to kill it --
    // this goes through the original game's death flow, quest kill callbacks fire as usual and the kill is credited to the player (for quest monsters: Ancients 540 / 541 / 542, Nihlathak 526, Baal 544...). Scanned every 5 ticks.
    { static int n = -1, delay = 50, ns; static uint32_t cls[16]; static struct { uint32_t id, t0; } seen[64]; static int rr; uint32_t tq = 0;
      if (n < 0) { n = 0; const char *e = getenv("DM_TEST_MON_KILL"); char buf[160]; if (e) { strlcpy(buf, e, sizeof buf); char *at = strchr(buf, '@'); if (at) { *at = 0; delay = atoi(at + 1); } for (char *sv, *t = strtok_r(buf, ",", &sv); t && n < 16; t = strtok_r(NULL, ",", &sv)) cls[n++] = (uint32_t)atoi(t); } }
      if (n && main && player && tk_rd(G + 0x170, &tq, 4) && tq % 5 == 0)
          for (int b = 0; b < 128; b++) { uint64_t u = 0; if (!tk_rd(G + 0x2630 + 8 * b, &u, 8)) continue;
              for (int k = 0; u && k < 4096; k++) { uint32_t c = 0, mode = 0, id = 0; uint64_t nx = 0; tk_rd(u + 0x158, &nx, 8); int hit = 0; if (tk_rd(u + 4, &c, 4) && tk_rd(u + 0xC, &mode, 4) && tk_rd(u + 8, &id, 4)) for (int i = 0; i < n; i++) hit |= cls[i] == c;
                  if (hit && mode != 0 && mode != 12) { int si = -1; for (int i = 0; i < ns; i++) if (seen[i].id == id) si = i; if (si < 0) { si = ns < 64 ? ns++ : rr++ % 64; seen[si].id = id; seen[si].t0 = tq; }   /* 表满了轮着覆盖旧项（路上杀过的怪早就不在了） | When the table is full, overwrite old entries in rotation (monsters killed along the way are long gone) */
                      if (si >= 0 && tq - seen[si].t0 >= (uint32_t)delay) { ((void (*)(uint64_t, uint64_t, uint64_t, int))(main->base + 0x7d82b0))(G, u, player, 1); uint32_t m1 = 99; tk_rd(u + 0xC, &m1, 4); LOG("DM_TEST_MON_KILL：拍 %u 杀怪 类号 %u id %u，模式 %u → %u", tq, c, id, mode, m1); seen[si].t0 = tq; } }
                  u = nx; } } }
    // 测试开关 DM_TEST_MON_HP=<类号>[,<类号>…]（造现场用）：这些类号的怪只要活着、生命高于 1，就把当前生命写成 1（服务端属性 6 = 256；每 5 拍扫一遍，回血也压回去），再打一下就死。
    // 用途：低级的造现场角色打不动头目（墨菲斯托 242、迪亚波罗 243、巴尔 544…），而去下一幕的门只在头目死后出现。每只怪第一次被压时记一行。
    // Test switch DM_TEST_MON_HP=<class>[,<class>...] (for test setup): for monsters of these classes, as long as they are alive with life above 1, write current life to 1 (server stat 6 = 256; scanned every 5 ticks, regeneration is pushed back down too), so one more hit kills them.
    // Use: a low-level test setup character cannot hurt the bosses (Mephisto 242, Diablo 243, Baal 544...), and the door to the next act only appears after the boss dies. A line is logged the first time each monster is pushed down.
    { static int cls_n = -1; static uint32_t cls[8], seen[32]; static int ns; uint32_t tq = 0;
      if (cls_n < 0) { cls_n = 0; const char *e = getenv("DM_TEST_MON_HP"); char buf[96]; if (e) { strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t && cls_n < 8; t = strtok_r(NULL, ",", &sv)) cls[cls_n++] = (uint32_t)atoi(t); } }
      if (cls_n > 0 && main && player && tk_rd(G + 0x170, &tq, 4) && tq % 5 == 0) { int (*get)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(main->base + 0x5b9290);
          for (int b = 0; b < 128; b++) { uint64_t u = 0; if (!tk_rd(G + 0x2630 + 8 * b, &u, 8)) continue;
              for (int k = 0; u && k < 4096; k++) { uint32_t c = 0, mode = 0, id = 0; int hit = 0; if (tk_rd(u + 4, &c, 4) && tk_rd(u + 0xC, &mode, 4) && tk_rd(u + 8, &id, 4)) { for (int i = 0; i < cls_n; i++) hit |= cls[i] == c;
                      if (hit && mode != 0 && mode != 12) { int hp = get(u, 6, 0); if (hp > 256) { ((void (*)(uint64_t, int, int, int))(main->base + 0x5bc1f0))(u, 6, 256, 0);
                              int old = 0; for (int i = 0; i < ns; i++) old |= seen[i] == id; if (!old) { if (ns < 32) seen[ns++] = id; LOG("DM_TEST_MON_HP：拍 %u 怪 类号 %u id %u 生命 %d/%d → 1", tq, c, id, hp >> 8, get(u, 7, 0) >> 8); } } } }
                  if (!tk_rd(u + 0x158, &u, 8)) break; } } } }
    // 测试开关 DM_TEST_DUR=<值>[@拍]（造现场用，缺省第 60 拍）：把玩家身上穿着的、有耐久的装备的当前耐久都设成这个值（不超过各自上限；服务端写属性 72，客户端那份由 dm_world_pump 按物品 id 同步写，界面才看得到）。
    // 设了这个变量就同时记耐久读数：穿着的装备耐久有变化时一行「物品 id 类号 耐久 当前/上限」。
    // Test switch DM_TEST_DUR=<value>[@tick] (for test setup, default tick 60): set the current durability of all items the player is wearing that have durability to this value (not above each item's maximum; the server writes stat 72, and the client copy is written in sync by dm_world_pump by item id, so the UI can see it).
    // Setting this variable also logs durability readouts: one line 「物品 id 类号 耐久 当前/上限」(item id class durability current/max) whenever a worn item's durability changes.
    { static int set_done; static struct { uint32_t id; int dur; } last[16]; static int nl; static const char *e; static int e_got; if (!e_got) { e_got = 1; e = getenv("DM_TEST_DUR"); }   /* 只取一次：这段每拍都走 | Fetch only once: this section runs every tick */ uint32_t tq = 0; uint64_t iv = 0, u = 0, next = 0; uint32_t sg = 0;
      if (e && main && player && tk_rd(G + 0x170, &tq, 4) && tk_rd(player + 0x90, &iv, 8) && tk_rd(iv, &sg, 4) && sg == 0x01020304 && tk_rd(iv + 0x10, &u, 8)) { int v = 1, at = 60; sscanf(e, "%d@%d", &v, &at);
          int (*get)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(main->base + 0x5b9290); int doset = !set_done && tq >= (uint32_t)at; extern _Atomic int g_dm_dur_sync_n; extern uint32_t g_dm_dur_sync[16][2]; int ns = 0; state_item it;
          for (unsigned k = 0; u && k < 1024 && state_read_item(u, &it, 0, &next); k++, u = next) { if (it.mode != 1) continue; int mx = get(u, 73, 0), dur = get(u, 72, 0); if (mx <= 0) continue;
              if (doset) { int nv = v < mx ? v : mx; ((void (*)(uint64_t, int, int, int))(main->base + 0x5bc1f0))(u, 72, nv, 0); dur = get(u, 72, 0); if (ns < 16) { g_dm_dur_sync[ns][0] = it.id; g_dm_dur_sync[ns][1] = (uint32_t)nv; ns++; } }
              int li = -1; for (int i = 0; i < nl; i++) if (last[i].id == it.id) li = i; if (li < 0 && nl < 16) { li = nl++; last[li].id = it.id; last[li].dur = -1; }
              if (li >= 0 && last[li].dur != dur) { LOG("DM_TEST_DUR：拍 %u 物品 id %u 类号 %u 耐久 %d/%d%s", tq, it.id, it.cls, dur, mx, doset ? "（本拍设的）" : ""); last[li].dur = dur; } }
          if (doset) { set_done = 1; atomic_store(&g_dm_dur_sync_n, ns); } } }
    // 测试开关 DM_TEST_POS_LOG=1：玩家所在格有变化时记一行（每 25 拍最多一行；「这一步之后能不能走动」的判据用）
    // Test switch DM_TEST_POS_LOG=1: log a line when the cell the player is in changes (at most one line per 25 ticks; used as the criterion for "can the player still move after this step")
    { static int on = -1, lx, ly; static uint32_t lt; uint32_t tq = 0; if (on < 0) on = getenv("DM_TEST_POS_LOG") != NULL;
      if (on && main && player && tk_rd(G + 0x170, &tq, 4) && tq - lt >= 25) { uint64_t pp = 0; tk_rd(player + 0x38, &pp, 8); if (pp) { int x = *(uint16_t *)(pp + 2), y = *(uint16_t *)(pp + 6); if (x != lx || y != ly) { LOG("DM_TEST_POS_LOG：拍 %u 位置 %d,%d", tq, x, y); lx = x; ly = y; lt = tq; } } } }
    { void dm_freetp_server_step(uint8_t *, uint64_t, uint64_t); if (main) dm_freetp_server_step((uint8_t *)main->base, G, player); }   // 免费回城（dm_freetp.c） | Free town portal (dm_freetp.c)
    // 测试开关 DM_TEST_STAT_LOG=<属性号>[,…]：这些玩家属性（14 身上金币、15 箱内金币、12 等级…）有变化时记一行（买卖 / 雇佣 / 修理这类成交的判据用）
    // Test switch DM_TEST_STAT_LOG=<stat number>[,...]: log a line when these player stats (14 gold on person, 15 gold in stash, 12 level...) change (used as the criterion for completed deals such as buying and selling / hiring / repairing)
    { static int ids[8], n = -1; static int last[8]; if (n < 0) { n = 0; const char *e = getenv("DM_TEST_STAT_LOG"); char buf[64]; if (e) { strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t && n < 8; t = strtok_r(NULL, ",", &sv)) { ids[n] = atoi(t); last[n++] = -1; } } }
      uint32_t tq = 0; if (n > 0 && main && player && tk_rd(G + 0x170, &tq, 4)) { int v[8], ch = 0; for (int i = 0; i < n; i++) { v[i] = ((int (*)(uint64_t, int, int))(main->base + 0x5b9290))(player, ids[i], 0); ch |= v[i] != last[i]; }
          if (ch) { char b[200]; int k = 0; for (int i = 0; i < n; i++) { k += snprintf(b + k, sizeof b - k, " %d=%d", ids[i], v[i]); if (k >= (int)sizeof b) k = (int)sizeof b - 1; last[i] = v[i]; } LOG("DM_TEST_STAT_LOG：拍 %u%s", tq, b); } } }
    // 测试开关 DM_TEST_SAVE_AT=<拍>[,<拍>…]：到这些拍请游戏走一次它自己的定时自动存盘（dm_request_save），造「局内有存档提交」的现场（iCloud 局内攒着、出局 / 切后台才传的验证用）
    // Test switch DM_TEST_SAVE_AT=<tick>[,<tick>...]: at these ticks ask the game to run its own timed autosave once (dm_request_save), setting up the "a save was committed during the run" test case (for verifying that iCloud accumulates during a run and only uploads on leaving the run / going to background)
    { static int at[8], n = -1, done; uint32_t tq = 0; if (n < 0) { n = 0; const char *e = getenv("DM_TEST_SAVE_AT"); char buf[96]; if (e) { strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t && n < 8; t = strtok_r(NULL, ",", &sv)) at[n++] = atoi(t); } }
      if (n > 0 && done < n && tk_rd(G + 0x170, &tq, 4) && tq >= (uint32_t)at[done]) { int dm_request_save(void); LOG("DM_TEST_SAVE_AT：拍 %u 请游戏存一次盘 → %d", tq, dm_request_save()); done++; } }
    // 副手组技能（dm_actions.c）：发布服务端记的当前武器组 [[玩家+0x10]+0xf0]+0x5a0（换武器包尾要带它，服务端 0x6de790 比对）
    // Off-hand set skills (dm_actions.c): publish the current weapon set as recorded by the server [[player+0x10]+0xf0]+0x5a0 (the weapon swap packet must carry it at the end; the server's 0x6de790 compares it)
    { void dm_swapskill_srv_wset(int); uint64_t pd = 0, q = 0; uint8_t w = 0; if (player && tk_rd(player + 0x10, &pd, 8) && pd && tk_rd(pd + 0xf0, &q, 8) && q && tk_rd(q + 0x5a0, &w, 1)) dm_swapskill_srv_wset(w); }
    // 免费鉴定（未鉴定物品双击，触摸层 → dm_actions.c dm_item_identify_free 记下物品 id）：在服务端拍里调游戏自己的鉴定函数
    // main+0x78acf0(游戏, 服务端玩家, 服务端物品)——鉴定卷轴 / 书在服务端走的那个（misc.txt pSpell 表 0x20ad7e8 第 1 项），凯恩「鉴定全部」最后也调它：
    // 已鉴定返回 0；否则经 0x64c5f0 置 [物品数据+0x18] |= 0x10 并自己广播物品更新（0x9c / 0x9d）。不扣卷轴（扣数是外层 0x786410 的事）、不查位置、不要 NPC
    // 测试开关 DM_TEST_IDENT_FREE=<拍>:<物品 id>[+<物品 id>…]（造现场用：编年史要鉴定过才记「发现」）：到拍起每拍一件走下面同一条免费鉴定（队列只有一格）
    // Free identify (double-tap on an unidentified item, touch layer → dm_actions.c dm_item_identify_free records the item id): in the server tick, call the game's own identify function
    // main+0x78acf0(game, server player, server item) -- the one identify scrolls / tomes use on the server (misc.txt pSpell table 0x20ad7e8 entry 1), which Cain's 「鉴定全部」(Identify All) also ends up calling:
    // returns 0 if already identified; otherwise via 0x64c5f0 sets [item data+0x18] |= 0x10 and broadcasts the item update itself (0x9c / 0x9d). No scroll is consumed (consuming is the outer 0x786410's job), no position check, no NPC needed
    // Test switch DM_TEST_IDENT_FREE=<tick>:<item id>[+<item id>...] (for test setup: the chronicle only records 「发现」(discovered) for identified items): from that tick on, one item per tick goes through the same free identify below (the queue has only one slot)
    { static int at = -1, ni, ki; static uint32_t iids[8]; uint32_t tq = 0;
      if (at < 0) { const char *e = getenv("DM_TEST_IDENT_FREE"), *c = e ? strchr(e, ':') : NULL; at = c ? atoi(e) : 0; for (char *q; c && ni < 8; c = *q == '+' ? q : NULL) { iids[ni++] = (uint32_t)strtoul(c + 1, &q, 10); } }
      if (at > 0 && ki < ni && main && player && tk_rd(G + 0x170, &tq, 4) && tq >= (uint32_t)at) { void dm_item_identify_free(uint32_t); dm_item_identify_free(iids[ki]); LOG("测试 DM_TEST_IDENT_FREE：拍 %u 鉴定物品 id %u", tq, iids[ki]); ki++; } }
    { uint32_t dm_item_identify_free_take(void); uint64_t dm_diag_srv_item(uint32_t); uint32_t fid = main && player ? dm_item_identify_free_take() : 0;
      if (fid) { uint64_t si = dm_diag_srv_item(fid); int r = si ? ((int (*)(uint64_t, uint64_t, uint64_t))(main->base + 0x78acf0))(G, player, si) : -1;
          LOG("免费鉴定：物品 id %u → %s", fid, r < 0 ? "服务端找不到这件，没调" : r ? "已调原版鉴定（服务端置已鉴定并广播更新）" : "原版判已鉴定，没变"); } }
    // 测试开关 DM_TEST_GIVE=<物品码>[:品质[:件数]][,…][@拍]：到拍（缺省 40）在玩家脚边地上生成物品（造现场用：洗点道具、未鉴定装备、成叠的宝石符文等）。
    // 走游戏自己的「按物品码在某单位旁边掉一件」main+0x7c0b20(游戏, 单位, 物品码, 品质)（任务奖励用的那个：查码表、在单位旁找空地、物品等级取游戏的、调通用生成函数 0x656780）。
    // 品质：2 普通（缺省）、4 魔法、5 套装、6 稀有、7 暗金；0 = 让游戏按品质比例表自己掷
    // Test switch DM_TEST_GIVE=<item code>[:quality[:count]][,...][@tick]: at the tick (default 40) spawn items on the ground at the player's feet (for test setup: respec items, unidentified gear, stacks of gems and runes, etc.).
    // Uses the game's own "drop one item by item code next to a unit" main+0x7c0b20(game, unit, item code, quality) (the one used for quest rewards: looks up the code table, finds free ground next to the unit, takes the item level from the game, calls the generic spawn function 0x656780).
    // Quality: 2 normal (default), 4 magic, 5 set, 6 rare, 7 unique; 0 = let the game roll by its quality ratio table
    { uint32_t tq = 0; static int given; const char *tg = getenv("DM_TEST_GIVE");
      if (tg && main && player && !given && tk_rd(G + 0x170, &tq, 4)) { char buf[256]; strlcpy(buf, tg, sizeof buf); int at = 40; char *ap = strrchr(buf, '@'); if (ap) { *ap = 0; at = atoi(ap + 1); }
          if (tq >= (uint32_t)at) { given = 1; for (char *sv, *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) { char code[8] = ""; int q = 2, n = 1; if (sscanf(t, "%4[^:]:%d:%d", code, &q, &n) < 1) continue; uint32_t c = 0x20202020; memcpy(&c, code, strlen(code) > 4 ? 4 : strlen(code)); int ok = 0, inv = 0;
                  uint32_t cls = 0, id0 = 0; for (int i = 0; i < n && i < 64; i++) { uint64_t u = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, int))(main->base + 0x7c0b20))(G, player, c, q); if (u) { ok++; tk_rd(u + 4, &cls, 4); if (!id0) tk_rd(u + 8, &id0, 4);
                          if (getenv("DM_TEST_GIVE_INV")) {   // DM_TEST_GIVE_INV=1：生成后当场替玩家捡进背包——把真实的捡物品包（0x16：单位 id、物品格坐标、1）交给游戏自己的收包分发 main+0x6e5b40(游戏, 玩家, 包, 长度)，前后照它的调用点（0x632634 / 0x63263c）置清「处理中」标志；背包放不下时游戏自己不捡、物品留在地上 | DM_TEST_GIVE_INV=1: right after spawning, pick it into the inventory on the player's behalf -- hand a real item pickup packet (0x16: unit id, item cell coordinates, 1) to the game's own packet dispatch main+0x6e5b40(game, player, packet, length), setting and clearing the "processing" flag before and after as its call site does (0x632634 / 0x63263c); when the inventory is full the game itself does not pick it up and the item stays on the ground
                              uint64_t ip = 0; uint8_t pk[17] = { 0x16 }; uint32_t one = 1; tk_rd(u + 0x38, &ip, 8); memcpy(pk + 1, (void *)(u + 8), 4); if (ip) { memcpy(pk + 5, (void *)(ip + 0x10), 4); memcpy(pk + 9, (void *)(ip + 0x14), 4); } memcpy(pk + 13, &one, 4);
                              *(uint32_t *)(G + 0x19050) = 1; ((void (*)(uint64_t, uint64_t, uint8_t *, uint32_t))(main->base + 0x6e5b40))(G, player, pk, 17); *(uint32_t *)(G + 0x19050) = 0; inv += *(uint32_t *)(u + 0xc) != 3; } } }
                  LOG("DM_TEST_GIVE：拍 %u 在玩家旁生成「%s」品质 %d：%d / %d 件成功（物品类号 %u，第一件单位 id %u）%s", tq, code, q, ok, n, cls, id0, !getenv("DM_TEST_GIVE_INV") ? "" : inv == ok ? "，已全部捡进背包" : "，【注意】有没捡进背包的（留在地上）"); } } } }
    if (main && player) DM_PROF(1, "s.elysium", dm_elysium_stash_step(G, player, (uint8_t *)main->base));   // 新建极乐世界角色后补齐共享仓库（dm_elysium.c） | Fill in the shared stash after a new Elysium character is created (dm_elysium.c)
    { void dm_townwp_tick(uint64_t, uint64_t, uint8_t *); if (main && player) DM_PROF(1, "s.townwp", dm_townwp_tick(G, player, (uint8_t *)main->base)); }   // 进局落在城镇小站旁（dm_townwp.c） | Land next to the town waypoint on joining a run (dm_townwp.c)
    { int dm_cloud_conflict_take(void); static int shown;   // iCloud 共享仓库分叉、本机那份收进了待合并收件箱（dm_cloudsync.c）：进局后弹一次原版系统提示（每次启动一次） | the iCloud shared stash forked and the local copy was put in the to-merge inbox (dm_cloudsync.c): one original system notice after entering a run (once per launch)
      if (main && player && !shown && dm_cloud_conflict_take()) { shown = 1; dm_game_notice((uint8_t *)main->base, dm_str((uint8_t *)main->base, "dmCloudConflict")); LOG("iCloud 同步：弹了共享仓库收进收件箱的提示"); } }
    { int dm_cloud_char_conflict_take(char *, size_t); char nm[256], msg[400];   // iCloud 首次同步遇到同名角色、用了云端那份（dm_cloudsync.c）：进局后提示一次 | iCloud first sync met a character with the same name and used the cloud copy (dm_cloudsync.c): one notice after entering a run
      if (main && player && dm_cloud_char_conflict_take(nm, sizeof nm)) { snprintf(msg, sizeof msg, dm_str((uint8_t *)main->base, "dmCloudCharConflict"), nm); dm_game_notice((uint8_t *)main->base, msg); LOG("iCloud 同步：弹了同名角色用云端那份的提示（%s）", nm); } }
    if (main && player) DM_PROF(1, "s.bossrush", dm_bossrush_tick(G, player, (uint8_t *)main->base));   // 首领连战（dm_bossrush.c） | Boss rush (dm_bossrush.c)
    if (main && player) { void dm_rift_tick(uint64_t, uint64_t, uint8_t *); DM_PROF(1, "s.rift", dm_rift_tick(G, player, (uint8_t *)main->base)); }   // 秘境爬塔（dm_rift.c） | Rift tower climb (dm_rift.c)
    { void dm_pages_tick(uint64_t, uint64_t, uint8_t *); if (main) DM_PROF(1, "s.pages", dm_pages_tick(G, player, (uint8_t *)main->base)); }   // 个人仓库 2–5 页（dm_pages.c）；不在局内 player 为 0，模块自己清状态 | Personal stash pages 2–5 (dm_pages.c); player is 0 when not in a run, and the module clears its own state
    { void dm_vault_tick(uint64_t, uint64_t, uint8_t *); if (main) DM_PROF(1, "s.vault", dm_vault_tick(G, player, (uint8_t *)main->base)); }
    { void dm_autobuff_srv_tick(uint8_t *, uint64_t, uint64_t); if (main && player) DM_PROF(1, "s.autobuff", dm_autobuff_srv_tick((uint8_t *)main->base, G, player)); }   // 增益技能自动补：放主线程点名的技能 | Buff auto-recast: cast the skill named by the main thread
    { void dm_pouch_srv_tick(uint8_t *, uint64_t, uint64_t); if (main) DM_PROF(1, "s.pouch", dm_pouch_srv_tick((uint8_t *)main->base, G, player)); }
    { void dm_cloudmerge_srv_tick(uint8_t *, uint64_t, uint64_t); if (main) DM_PROF(1, "s.cloudmerge", dm_cloudmerge_srv_tick((uint8_t *)main->base, G, player)); }   // iCloud 收件箱局内并回（dm_cloudmerge.c）；不在局内 player 为 0   // 百宝囊收 / 放、存放的系统提示（dm_pouch.c）；不在局内 player 为 0   // 编年史仓库存取（dm_vault.c）；同上 | iCloud inbox in-game merge (dm_cloudmerge.c); player is 0 when not in game   // System notices for pouch take / put and deposit (dm_pouch.c); player is 0 when not in game   // Chronicle stash take / store (dm_vault.c); same as above
    if (main && player) DM_PROF(1, "s.kills", dm_kills_tick(G, player, (uint8_t *)main->base)); dm_ipf_frame(1);   // 刷怪积分牌（dm_kills.c）：在首领连战之后，连战本关的首领它已记好 id | Kill board (dm_kills.c): runs after boss rush, which has already recorded the id of the boss of the current boss rush stage
    cube_diag_tick(G, player);   // 合成读数 | Transmute readout
    cam_threats_tick(G, player);
    if (main && player) seek_tp_step(G, player, main);
    if (main && h.valid) h.quest_unread = *(volatile uint8_t *)(main->base + 0x2d5ff58) != 0;   // 任务提示：收到任务更新包时游戏置 1（0x306cfc），原版只在点 HUD 任务提示按钮时清（0xbbe6ec） | Quest hint: the game sets it to 1 when a quest update packet arrives (0x306cfc); the original game only clears it when the HUD quest hint button is tapped (0xbbe6ec)
    dm_hud_info ha = h, hb = g_hud; ha.x = ha.y = hb.x = hb.y = 0;   // 坐标每步都变，不算「变化」（否则走几步就把 200 行记满） | Coordinates change on every step and do not count as a "change" (otherwise a few steps would fill up the 200 lines)
    if (memcmp(&ha, &hb, sizeof ha) && logged < 200) { logged++;   // 只在变化时记一行（前 200 次）：真机 / Mac 核对读数 | Log a line only on change (first 200 times): readout for cross-checking real device / Mac
        LOG("dm_hud：区域 %d (%d,%d) Lv %d 经验 %d‰ 生命 %d/%d 法力 %d/%d 腰带 %d/%d/%d/%d 回城 %d 人数档 %d 点数 %d 新任务 %d%s%s", h.area, h.x, h.y, h.level, h.exp_permille, h.hp, h.hp_max, h.mp, h.mp_max, h.belt[0], h.belt[1], h.belt[2], h.belt[3], h.portal, h.players, h.points, h.quest_unread, h.dead ? " 已死亡" : "", h.valid ? "" : "（未读到玩家）"); }
    g_hud = h; dm_hud_publish(&h);
}

static void state_dump(const uint64_t *regs) {
    static char *prev;
    uint64_t G = regs[0], player = 0, inv = 0, cursor = 0;
    int players = 0; uint32_t lid = local_player_id(); uint64_t mine = 0;
    for (int b = 0; b < 128; b++) {
        uint64_t u = 0;
        if (!tk_rd(G + 0x2230 + 8 * b, &u, 8)) continue;
        for (int k = 0; u && k < 4096; k++) {
            uint32_t mode = 0, id = 0;
            uint64_t pp = 0;
            uint16_t c[4] = {0};
            if (tk_rd(u + 0xC, &mode, 4) && mode && tk_rd(u + 0x38, &pp, 8) && pp && tk_rd(pp, c, 8) && (c[1] || c[3])) {
                if (!players++) player = u;
                if (lid != 0xffffffffu && tk_rd(u + 8, &id, 4) && id == lid) mine = u;
            }
            if (!tk_rd(u + 0x158, &u, 8)) break;
        }
    }
    if (lid != 0xffffffffu) { player = mine; players = mine ? 1 : 0; }   // 认得出本地玩家单位号：只认它（别的在场玩家类单位是尸体），它正在死的那几拍当作没有 | local player unit id is known: accept only that unit (other player-type units present are corpses); treat it as absent on the ticks while it is dying
    static int player_warned;
    if (players == 0) return;
    if (players != 1) {
        if (!player_warned) { LOG("【红】DM_STATE：受控玩家不唯一（在场 %d 个）", players); player_warned = 1; }
        if (!player) return;
    }
    if (!tk_rd(player + 0x90, &inv, 8)) return;
    uint32_t sig = 0;
    if (!tk_rd(inv, &sig, 4) || sig != 0x01020304) return;
    tk_rd(inv + 0x40, &cursor, 8);
    static state_item items[1025]; size_t n = 0; uint64_t u = 0, next = 0;   // static：同 cur，拍入口在客体栈上；只在拍线程上用 | static: same as cur, the tick entry is on a guest stack; used only on the tick thread
    if (!tk_rd(inv + 0x10, &u, 8)) return;
    for (unsigned k = 0; u && k < 1024 && state_read_item(u, &items[n], n, &next); k++) { n++; u = next; }
    // 光标上的物品不在 inventory 链表里（实测拿起后链表少一件）⇒ 单独补进 items，mode 即原版 4
    // The item on the cursor is not in the inventory list (measured: the list is one item shorter after picking it up) ⇒ add it to items separately, with mode 4 as in the original game
    if (cursor && state_read_item(cursor, &items[n], n, &next)) {
        int dup = 0; for (size_t i = 0; i < n; i++) dup |= items[i].id == items[n].id;
        if (!dup) n++;
    }
    qsort(items, n, sizeof *items, state_item_cmp);
    uint32_t cursor_id = 0; int has_cursor = 0;
    if (cursor && tk_rd(cursor + 8, &cursor_id, 4)) has_cursor = 1;
    static char cur[131072]; size_t used = 0;   // static：拍入口观测门跑在客体线程栈上，128 KB 局部数组会被内联进 tick_gate 的栈帧（实测最小客体栈只有 16 KB）；只在拍线程上用 | static: the tick entry observation gate runs on a guest thread stack, and a 128 KB local array would be inlined into tick_gate's stack frame (the smallest guest stack measured is only 16 KB); used only on the tick thread
    int w = snprintf(cur, sizeof cur, "\"items\":[");
    if (w < 0 || (size_t)w >= sizeof cur) return; used = (size_t)w;
    for (size_t i = 0; i < n; i++) {
        state_item *it = &items[i];
        w = snprintf(cur + used, sizeof cur - used, "%s{\"id\":%u,\"cls\":%u,\"mode\":%u,\"page\":%u,\"x\":%u,\"y\":%u}",
                     i ? "," : "", it->id, it->cls, it->mode, it->page, it->x, it->y);
        if (w < 0 || (size_t)w >= sizeof cur - used) return; used += (size_t)w;
    }
    int pl = g_hud.players;   // 拍线程自己写的，同线程读不用锁 | Written by the tick thread itself; reading on the same thread needs no lock
    w = has_cursor ? snprintf(cur + used, sizeof cur - used, "],\"cursor\":%u,\"players\":%d", cursor_id, pl)
                   : snprintf(cur + used, sizeof cur - used, "],\"cursor\":null,\"players\":%d", pl);
    if (w < 0 || (size_t)w >= sizeof cur - used) return; used += (size_t)w;
    state_dump_monsters(G, cur, sizeof cur, &used);
    uint32_t tick = 0;
    if (!tk_rd(G + 0x170, &tick, 4)) return;
    if (prev && !strcmp(prev, cur)) return;
    free(prev); prev = strdup(cur);
    if (!prev) return;
    fprintf(g_spfp, "{\"tick\":%u,%s}\n", tick, cur);
    fflush(g_spfp);
}
static void ticks_hash(const uint64_t *regs) {
    static const uint32_t TB[5] = {0x2230, 0x2630, 0x2a30, 0x3230, 0x2e30};
    static tk_unit *us; static size_t cap;
    uint64_t G = regs[0]; uint32_t tick;
    g_tkreads = 0;
    if (!tk_rd(G + 0x170, &tick, 4)) return;
    if (tick == g_tklast) return;                               // 同一拍号只取一次 | Take each tick number only once
    // 同一进程里先后进两次局（退回选角再进、连建两个角色）：新一局拍号从 0 / 1 重数，这不是断档；局内跳号才报红
    // Joining a run twice in the same process (back to character select and in again, or creating two characters in a row): the new run's tick number restarts from 0 / 1, which is not a gap; only a jump within a run reports a failure
    if (g_tklast >= 0 && tick != g_tklast + 1) { if (tick <= 1 && (long long)tick < g_tklast) LOG("DM_TICKS 新一局：拍号 %lld → %u", g_tklast, tick); else LOG("【红】DM_TICKS 拍号断档 %lld → %u", g_tklast, tick); }
    g_tkreads = 1;                                              // 上面读拍号算一次（swtick tick_line 首读同） | Reading the tick number above counts as one read (same as the first read in swtick tick_line)
    static uint8_t tab[0x1400]; unsigned bad = 0; size_t n = 0;   // static：同上，拍入口在客体栈上，只在拍线程上用 | static: as above, the tick entry is on a guest stack; used only on the tick thread
    if (!tk_rd(G + TB[0], tab, sizeof tab)) memset(tab, 0, sizeof tab);
    for (int t = 0; t < 5; t++)
        for (int b = 0; b < 128; b++) {
            uint64_t u; memcpy(&u, tab + TB[t] - TB[0] + 8 * b, 8);
            for (int k = 0; u && k < 4096; k++) {
                uint8_t hd[0x160];
                if (!tk_rd(u, hd, sizeof hd)) { bad++; break; }
                if (n == cap) { cap = cap ? cap * 2 : 1024; us = realloc(us, cap * sizeof *us); }
                tk_unit *x = &us[n]; memset(x, 0, offsetof(tk_unit, st)); x->ord = (unsigned)n;
                memcpy(&x->type, hd, 4); memcpy(&x->cls, hd + 4, 4); memcpy(&x->id, hd + 8, 4); memcpy(&x->mode, hd + 12, 4);
                x->act = hd[0x18]; memcpy(&x->seed, hd + 0x30, 4); memcpy(&x->rlo, hd + 0x28, 4); memcpy(&x->rhi, hd + 0x2c, 4);
                uint64_t q;
                if (t == 4) { uint8_t d[0x1c]; memcpy(&q, hd + 0x10, 8);
                    if (tk_rd(q, d, sizeof d)) { memcpy(&x->it[0], d, 4); memcpy(&x->it[1], d + 4, 4); memcpy(&x->it[2], d + 8, 4); memcpy(&x->it[3], d + 0x18, 4); memcpy(&x->it[4], d + 0x10, 4); x->has_it = 1; } }
                if (t == 0 || t == 1 || t == 3) { uint16_t p[4]; memcpy(&q, hd + 0x38, 8);
                    if (tk_rd(q, p, sizeof p)) { x->xf = p[0]; x->x = p[1]; x->yf = p[2]; x->y = p[3]; } }
                uint8_t sl[0x40]; memcpy(&q, hd + 0x88, 8);
                if (tk_rd(q, sl, sizeof sl)) {
                    uint64_t sb; uint32_t sc; memcpy(&sb, sl + 0x30, 8); memcpy(&sc, sl + 0x38, 4);
                    uint32_t m = sc < 96 ? sc : 96;
                    if (sb && sc && tk_rd(sb, x->st, 8 * m)) x->nst = m;
                }
                if (x->type != (uint32_t)t) bad++;
                n++;
                memcpy(&u, hd + 0x158, 8);
            }
        }
    qsort(us, n, sizeof *us, tk_cmp);
    uint64_t h[6]; for (int i = 0; i < 6; i++) h[i] = 0xcbf29ce484222325ull;
    uint8_t gs[8] = {0}; if (!tk_rd(G + 0x1a8, gs, 8)) memset(gs, 0, 8);
    h[5] = tk_fnv(h[5], gs, 8);
    unsigned cnt[5] = {0};
    for (size_t i = 0; i < n; i++) {
        tk_unit *u = &us[i]; uint32_t t = u->type;
        if (t >= 5) continue;
        cnt[t]++;
        uint32_t f9[9] = {t, u->cls, u->id, u->mode, u->act, u->x, u->y, u->xf, u->yf}, f2[2] = {u->rlo, u->rhi};
        uint64_t hh = tk_fnv(h[t], f9, sizeof f9); hh = tk_fnv(hh, f2, sizeof f2);
        if (t == 4) hh = tk_fnv(hh, u->it, sizeof u->it);
        if (t <= 1 || t == 4) { hh = tk_fnv(hh, &u->nst, 4); hh = tk_fnv(hh, u->st, 8 * (size_t)u->nst); }
        h[t] = hh;
    }
    if (!g_tkt0) g_tkt0 = now_ns();
    fprintf(g_tkfp, "{\"tick\": %u, \"n\": [%u, %u, %u, %u, %u], \"frames\": %llu, \"h\": [", tick, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], (unsigned long long)dm_nvn_frame_count());
    for (int i = 0; i < 6; i++) fprintf(g_tkfp, "%s\"%016llx\"", i ? ", " : "", h[i]);
    fprintf(g_tkfp, "], \"bad\": %u, \"reads\": %u, \"game\": \"%#llx\", \"dt\": %.3f}\n", bad, g_tkreads, G, (now_ns() - g_tkt0) / 1e9);
    if (g_tkfull)                                               // = swtick --full：每单位一行 {"t": 拍号, "u": {…}}（排序后次序） | = swtick --full: one line per unit {"t": tick number, "u": {...}} (in sorted order)
        for (size_t i = 0; i < n; i++) {
            tk_unit *u = &us[i];
            fprintf(g_tkfp, "{\"t\": %u, \"u\": {\"type\": %u, \"cls\": %u, \"id\": %u, \"mode\": %u, \"act\": %u, \"seed\": %u, \"rlo\": %u, \"rhi\": %u, "
                    "\"x\": %u, \"y\": %u, \"xf\": %u, \"yf\": %u, \"it\": ", tick, u->type, u->cls, u->id, u->mode, u->act, u->seed, u->rlo, u->rhi, u->x, u->y, u->xf, u->yf);
            if (u->has_it) fprintf(g_tkfp, "[%u, %u, %u, %u, %u]", u->it[0], u->it[1], u->it[2], u->it[3], u->it[4]); else fputs("null", g_tkfp);
            fputs(", \"st\": [", g_tkfp);
            for (uint32_t j = 0; j < u->nst; j++) fprintf(g_tkfp, "%s[%u, %u]", j ? ", " : "", u->st[j][0], u->st[j][1]);
            fputs("]}}\n", g_tkfp);
        }
    fflush(g_tkfp);
    g_tklast = tick;
    if (g_tkleft > 0 && --g_tkleft == 0) { fclose(g_tkfp); res_report("退出"); LOG("DM_TICKS 记满，最后拍号 %u，退出", tick); _exit(0); }
}

// ---------- 客户端命令包：DM_C2S_LOG=<路径>（main 0x6e5b40 命令分派入口观测门 OBS_C2S）/ DM_C2S_REPLAY=<路径> ----------
// 格式 = dm/verify/command_packet_format.md：每条一行 {"tick":拍号,"unit":单位id|null,"len":长度,"pkt":"前 min(len,512) 字节十六进制"}。
// 回放：收包泵投递点 0x5d3a88 的剪口（svc #0x81，仅宿主）在回放时丢弃客户端自发命令；拍入口（OBS_TICK，入口值 = T）按文件顺序
// 把 tick = T 的包交给 0x6e5b40(G, 单位, 包, 长度)，照收包函数 0x632510 的做法在调用期间置 [G+0x19050] = 1。
// 时序依据：收包泵（主循环，0x2c23c4 / 0x2cfa38）先于拍函数 0x5d3bc0 跑，拍号在拍函数开头（0x5d3bf8）才自增 ⇒ 记为 T 的包恰在入口值 T 的拍之前处理；
// 注入先于本拍的状态哈希（真值的拍哈希同样含这些包的效果）。
// ---------- Client command packets: DM_C2S_LOG=<path> (observation gate OBS_C2S at the command dispatch entry main 0x6e5b40) / DM_C2S_REPLAY=<path> ----------
// Format = dm/verify/command_packet_format.md: one line per entry {"tick":tick number,"unit":unit id|null,"len":length,"pkt":"hex of the first min(len,512) bytes"}.
// Replay: the cut at the receive pump delivery point 0x5d3a88 (svc #0x81, host only) drops the client's self-initiated commands during replay; the tick entry (OBS_TICK, entry value = T), in file order,
// hands packets with tick = T to 0x6e5b40(G, unit, packet, length), setting [G+0x19050] = 1 for the duration of the call as the receive function 0x632510 does.
// Timing basis: the receive pump (main loop, 0x2c23c4 / 0x2cfa38) runs before the tick function 0x5d3bc0, and the tick number is only incremented at the start of the tick function (0x5d3bf8) ⇒ a packet recorded as T is handled right before the tick whose entry value is T;
// injection precedes this tick's state hash (the ground-truth tick hash likewise includes the effects of these packets).
static FILE *g_c2sfp;
static struct c2s_ent { uint32_t tick, unit, len; int has_unit, sys; uint8_t *pkt; } *g_rp; static size_t g_nrp, g_irp;
static uint64_t g_lastG; static uint32_t g_sysclient; static int g_sysclient_set; static unsigned g_sysdrop;   // 最近一次拍入口的 G；系统类消息的客户端号（取宿主自己的加入消息） | G of the most recent tick entry; client number for system-class messages (taken from the host's own join message)
static int64_t g_rplast = -1; static unsigned g_rpdrop;
// 越肩镜头（dm_cam.c dm_cam_attack）：放技能的命令包带着目标——对地点（0x05 / 0x08 / 0x0c / 0x0f：x、y 各 2 字节，子格；落点附近的活怪见下）或对单位
// （0x06 / 0x07 / 0x09 / 0x0a / 0x0d / 0x0e / 0x10 / 0x11：类型、单位号各 4 字节；只认玩家 0 与怪 1，它们的路径 +0x38 → +0 / +4 是 16.16 定点子格坐标）。
// 把玩家到目标的方向交给镜头，原地放远程技能时镜头也转去看目标。只读服务端单位表，镜头没开时什么都不做
// Over-the-shoulder camera (dm_cam.c dm_cam_attack): skill cast commands carry their target -- a location (0x05 / 0x08 / 0x0c / 0x0f: x, y, 2 bytes each,
// sub-cells) or a unit (0x06 / 0x07 / 0x09 / 0x0a / 0x0d / 0x0e / 0x10 / 0x11: type and unit id, 4 bytes each; only players 0 and monsters 1, whose path
// +0x38 -> +0 / +4 holds 16.16 fixed-point sub-cell coordinates). The direction from the player to the target goes to the camera, so a ranged skill cast in
// place also turns the camera towards the target. Reads the server unit tables only, and does nothing while the camera is off
static void cam_c2s(uint64_t G, uint64_t pl, const uint8_t *b, uint32_t n) {
    int dm_cam_mode(void); void dm_cam_attack(float dx, float dz);
    if (!n || !pl || dm_cam_mode() != 1) return;
    uint8_t op = b[0]; float tx, ty;
    if ((op == 0x05 || op == 0x08 || op == 0x0c || op == 0x0f) && n >= 5) {
        // 对地点：触屏点按的自动选怪在这一版发的是「对目标所在格放」（Redmi 实测：贴着僵尸时 0x05 落在僵尸那格，离人 0.7 格；没有怪时落在脚下，同样 0.7 格），
        // 光看距离分不出来。所以在那一格 2 格内找一只活着的怪，朝它的真实位置转；找不到 = 空放 / 对自己放，不转
        // A location: touch auto-targeting in this build casts "at the target's cell" (measured on the Redmi: next to a zombie, 0x05 landed on the zombie's
        // cell, 0.7 cells from the player; with no monster it landed at the feet, also 0.7 cells away), so distance alone cannot tell them apart. Look for a
        // living monster within 2 cells of that cell and turn towards its real position; none = an empty cast or a cast on oneself, no turn
        float lx = (float)(b[1] | b[2] << 8), ly = (float)(b[3] | b[4] << 8), best = 4; uint64_t u = 0; int found = 0;
        for (int bk = 0; bk < 128; bk++) if (tk_rd(G + 0x2230 + 1 * 0x400 + 8 * bk, &u, 8)) for (int k = 0; u && k < 4096; k++) {
            uint32_t mode = 0; uint64_t tp = 0; uint32_t xy[2];
            if (tk_rd(u + 0xC, &mode, 4) && mode != 0 && mode != 12 && tk_rd(u + 0x38, &tp, 8) && tp && tk_rd(tp, xy, 8)) {
                float mx = xy[0] / 65536.0f, my = xy[1] / 65536.0f, d2 = (mx - lx) * (mx - lx) + (my - ly) * (my - ly);
                if (d2 < best) { best = d2; tx = mx; ty = my; found = 1; } }
            if (!tk_rd(u + 0x158, &u, 8)) break; }
        if (!found) return;
    }
    else if ((op == 0x06 || op == 0x07 || op == 0x09 || op == 0x0a || op == 0x0d || op == 0x0e || op == 0x10 || op == 0x11) && n >= 9) {
        uint32_t type = b[1] | b[2] << 8 | b[3] << 16 | (uint32_t)b[4] << 24, id = b[5] | b[6] << 8 | b[7] << 16 | (uint32_t)b[8] << 24; if (type > 1) return;
        uint64_t u = 0, hit = 0;
        for (int bk = 0; bk < 128 && !hit; bk++) if (tk_rd(G + 0x2230 + type * 0x400 + 8 * bk, &u, 8)) for (int k = 0; u && k < 4096; k++) { uint32_t i2 = 0; if (tk_rd(u + 8, &i2, 4) && i2 == id) { hit = u; break; } if (!tk_rd(u + 0x158, &u, 8)) break; }
        uint64_t tp = 0; uint32_t xy[2]; if (!hit || !tk_rd(hit + 0x38, &tp, 8) || !tp || !tk_rd(tp, xy, 8)) return;
        tx = xy[0] / 65536.0f; ty = xy[1] / 65536.0f;
    } else return;
    uint64_t pp = 0; uint32_t pxy[2]; if (!tk_rd(pl + 0x38, &pp, 8) || !pp || !tk_rd(pp, pxy, 8)) return;
    dm_cam_attack(tx - pxy[0] / 65536.0f, ty - pxy[1] / 65536.0f);
}
// 越肩镜头的怪物指示（dm_cam.c dm_cam_threats）：透视画面只看得见前方，怪从画面外很快贴上脸（维护者试玩）。每拍从服务端怪物表取玩家
// DM_CAM_THREAT_RANGE（缺省 30）格内活着的怪（模式不是 0 死亡中 / 12 已死），去掉佣兵（类号 271 / 338 / 359 / 560 / 561），带上配色档（hle_target.c dm_threat_tier：普通 / 冠军 / 独特 / 首领）交给镜头；城里不取。
// 玩家自己的召唤物（骷髅、魔像、狼、复活的怪等）不算：客户端宠物链表 main+0x2d33528（每项 +8 单位号、+0xc 属主号、+0x30 下一项，同 dm_actions.c
// 喂佣兵处）里属主 = 玩家的单位号，扫怪时跳过；别人的召唤物照算
// Monster indicators of the over-the-shoulder camera (dm_cam.c dm_cam_threats): the perspective picture only shows what is ahead, and monsters reach the
// character's face from outside the picture fast (the maintainer's play test). Every tick, take the living monsters (mode neither 0 dying nor 12 dead)
// within DM_CAM_THREAT_RANGE (default 30) cells of the player from the server monster table, drop mercenaries (classes 271 / 338 / 359 / 560 / 561), and
// hand them with their tier to the camera; nothing in town. The player's own summons (skeletons, golems, wolves, revived
// monsters and so on) do not count: the unit ids owned by the player in the client pet list main+0x2d33528 (each entry +8 unit id, +0xc owner id, +0x30
// next, as at the mercenary feeding in dm_actions.c) are skipped while scanning; other players' summons still count
static void cam_threats_tick(uint64_t G, uint64_t pl) {
    int dm_cam_mode(void); void dm_cam_set_threats(const float *xy, const int *tier, int n); int dm_threat_tier(uint64_t unit);
    if (dm_cam_mode() <= 0 || !pl) return;   // 越肩与第一人称都收（原先只认越肩，第一人称下一个也没有） | both over-the-shoulder and first person (it used to take mode 1 only, so first person had none)
    static float range = -1; if (range < 0) { const char *e = getenv("DM_CAM_THREAT_RANGE"); range = e ? (float)atof(e) : 30; }
    dm_hud_info h; dm_hud_get(&h); if (h.valid && dm_hud_area_is_town(h.area)) { dm_cam_set_threats(NULL, NULL, 0); return; }
    uint64_t pp = 0; uint32_t pxy[2]; if (!tk_rd(pl + 0x38, &pp, 8) || !pp || !tk_rd(pp, pxy, 8)) return;
    float px = pxy[0] / 65536.0f, py = pxy[1] / 65536.0f, xy[64]; int tier[32], n = 0; uint64_t u = 0;
    uint32_t me = 0, pet[64]; int np = 0; module_t *m = main_mod(); uint64_t e = 0;   // 玩家自己的召唤物 | the player's own summons
    if (m && tk_rd(pl + 8, &me, 4) && tk_rd((uint64_t)m->base + 0x2d33528, &e, 8))
        for (int k = 0; e && k < 256 && np < 64; k++) { uint32_t id = 0, owner = 0; if (tk_rd(e + 8, &id, 4) && tk_rd(e + 0xc, &owner, 4) && owner == me) pet[np++] = id; if (!tk_rd(e + 0x30, &e, 8)) break; }
    for (int bk = 0; bk < 128 && n < 32; bk++) if (tk_rd(G + 0x2230 + 1 * 0x400 + 8 * bk, &u, 8)) for (int k = 0; u && k < 4096 && n < 32; k++) {
        uint32_t cls = 0, mode = 0, mxy[2]; uint64_t tp = 0;
        uint32_t uid = 0; int mine = 0; if (np && tk_rd(u + 8, &uid, 4)) for (int k = 0; k < np && !mine; k++) mine = pet[k] == uid;
        if (!mine && tk_rd(u + 4, &cls, 4) && tk_rd(u + 0xC, &mode, 4) && mode != 0 && mode != 12 && cls != 271 && cls != 338 && cls != 359 && cls != 560 && cls != 561
            && tk_rd(u + 0x38, &tp, 8) && tp && tk_rd(tp, mxy, 8)) {
            float mx = mxy[0] / 65536.0f, my = mxy[1] / 65536.0f, dx = mx - px, dy = my - py;
            if (dx * dx + dy * dy <= range * range) { int r = dm_threat_tier(u); xy[2 * n] = mx; xy[2 * n + 1] = my; tier[n++] = r < 0 ? 0 : r; } }
        if (!tk_rd(u + 0x158, &u, 8)) break; }
    dm_cam_set_threats(xy, tier, n);
}
static void c2s_obs(const uint64_t *regs) {
    uint32_t tick = 0, uid = 0, len = (uint32_t)regs[3]; int hu = 0;
    tk_rd(regs[0] + 0x170, &tick, 4);
    if (regs[1] && tk_rd(regs[1] + 8, &uid, 4)) hu = 1;
    uint8_t b[512]; uint32_t n = len < 512 ? len : 512; if (!tk_rd(regs[2], b, n)) n = 0;
    if (n >= 1 && b[0] == 0x20) cube_diag_req(regs[0], regs[1], b, n);   // 合成读数 | Transmute readout
    dm_bossrush_c2s(regs[0], regs[1], b, n);
    cam_c2s(regs[0], regs[1], b, n);
    { void dm_rift_c2s(uint64_t, const uint8_t *, uint32_t); dm_rift_c2s(regs[0], b, n); }
    { void dm_pouch_c2s(uint64_t *); dm_pouch_c2s((uint64_t *)(uintptr_t)regs); }   // 珍宝匣：符文 / 宝石 / 材料拖进共享页时改道进高级仓库（dm_pouch.c；会换掉 x2 / x3 指向的包） | Trove: runes / gems / materials dragged into the shared tab are rerouted into the advanced stash tabs (dm_pouch.c; replaces the packet x2 / x3 point to)
    { void dm_shoprefresh_c2s(uint64_t, uint64_t, const uint8_t *, uint32_t); dm_shoprefresh_c2s(regs[0], regs[1], b, n); }   // 商人交易页「刷新货架」（dm_shoprefresh.c）   // 首领连战：玩家点了连战的门（门只是外观，换关由 dm_bossrush.c 做） | Vendor trade page 「刷新货架」(Refresh Stock) (dm_shoprefresh.c)   // Boss rush: the player tapped the boss rush door (the door is appearance only; the level change is done by dm_bossrush.c)
    if (!g_c2sfp) return;                       // 门在产品里常挂（首领连战要看），没开 DM_C2S_LOG 时不记 | The gate is usually installed in the product (boss rush needs it); nothing is logged when DM_C2S_LOG is not on
    // 诊断开关 DM_TEST_WARP_DIAG=1（只读）：收到「与出口砖交互」包（0x40 [砖 id]）这一刻，把服务端处理函数 main+0x6dbea0 的判定逐项读出来记一行——点了出口不进时看是哪一项拒的（全部拒绝都不回包）。
    // 判定顺序：当前动作能否被打断 main+0x6c5730(游戏, 玩家, 2, 0, 0)（0 = 放行）→ 砖对应的出口可用 main+0x5592f0(砖)（非 0 = 放行）→ 距离 main+0x5c7050(玩家, 砖)（查表距离，≤4 才当场换，>4 什么都不做）。
    // Diagnostic switch DM_TEST_WARP_DIAG=1 (read-only): at the moment an "interact with exit tile" packet (0x40 [tile id]) is received, read out each check of the server handler main+0x6dbea0 and log one line -- to see which check refused when tapping an exit does not go in (no refusal sends a reply packet).
    // Check order: whether the current action can be interrupted main+0x6c5730(game, player, 2, 0, 0) (0 = allowed) → the exit for this tile is usable main+0x5592f0(tile) (non-zero = allowed) → distance main+0x5c7050(player, tile) (table-lookup distance; only ≤4 changes level on the spot, >4 does nothing).
    if (n >= 5 && b[0] == 0x40 && regs[1] && getenv("DM_TEST_WARP_DIAG")) { module_t *mm = main_mod(); uint32_t id = b[1] | b[2] << 8 | b[3] << 16 | (uint32_t)b[4] << 24, pm = 99; uint64_t G = regs[0], pl = regs[1], tile = 0, u = 0; tk_rd(pl + 0xC, &pm, 4);
        for (int bk = 0; bk < 128 && !tile; bk++) if (tk_rd(G + 0x2230 + 5 * 0x400 + 8 * bk, &u, 8)) for (int k = 0; u && k < 4096; k++) { uint32_t i2 = 0; if (tk_rd(u + 8, &i2, 4) && i2 == id) { tile = u; break; } if (!tk_rd(u + 0x158, &u, 8)) break; }
        int chk = mm ? ((int (*)(uint64_t, uint64_t, int, uint64_t, int))((uintptr_t)mm->base + 0x6c5730))(G, pl, 2, 0, 0) : -1, usable = mm && tile ? ((int (*)(uint64_t))((uintptr_t)mm->base + 0x5592f0))(tile) : -1, dist = mm && tile ? ((int (*)(uint64_t, uint64_t))((uintptr_t)mm->base + 0x5c7050))(pl, tile) : -1;
        LOG("DM_TEST_WARP_DIAG：拍 %u 收到 0x40 砖 id %u%s：玩家模式 %u，能否打断 %d（0 = 放行），出口可用 %d（非 0 = 放行），距离 %d（≤4 当场换）", tick, id, tile ? "" : "（服务端单位表里没有这块砖）", pm, chk, usable, dist); }
    fprintf(g_c2sfp, "{\"tick\":%u,\"unit\":", tick);
    if (hu) fprintf(g_c2sfp, "%u", uid); else fputs("null", g_c2sfp);
    fprintf(g_c2sfp, ",\"len\":%u,\"pkt\":\"", len);
    for (uint32_t i = 0; i < n; i++) fprintf(g_c2sfp, "%02x", b[i]);
    fputs("\"}\n", g_c2sfp); fflush(g_c2sfp);
}
// 在不在局内：服务端局循环 0x5d5120 遍历 main+0x2d665d8 起 1024 个局号槽（0 / -1 = 空），按局号查局对象、以 对象+0x10 为 G 调拍入口 0x5d3bc0。
// 没有活的局号 ⇒ 局已结束（存盘退出、致命错误踢出等任何路径），G 已不可读：清 g_lastG。离局后拍号是固定垃圾值
// Whether we are in a run: the server run loop 0x5d5120 walks 1024 run-number slots starting at main+0x2d665d8 (0 / -1 = empty), looks up the run object by run number, and calls the tick entry 0x5d3bc0 with object+0x10 as G.
// No live run number ⇒ the run has ended (save-and-exit, kicked by a fatal error, or any other path) and G is no longer readable: clear g_lastG. After leaving a run the tick number is a fixed garbage value
static int game_live(void) {
    static const uint64_t *slots;
    if (!slots) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) slots = (const uint64_t *)(g_mod[i].base + 0x2d665d8); if (!slots) return 1; }
    for (int i = 0; i < 0x400; i++) if (slots[i] + 1 >= 2) return 1;
    return 0;
}
static int in_game(void) { if (g_lastG && !game_live()) g_lastG = 0; return g_lastG != 0; }
// 让游戏现在就走一次它自己的定时自动存盘（前端进后台时调）：游戏每拍在拍入口函数（main+0x5d3bc0，G = 第一个参数）里判
// 「现在 − 上次存盘时刻 [G+0x5800]（毫秒）> 300000」就把本局全部玩家序列化存盘并提交（main+0x5d4284–0x5d4c20）。原版没有「按 HOME / 进后台存盘」这条路
// （收到后台焦点通知后 2.5 秒内不存），所以把上次存盘时刻拨到很久以前，下一拍它就按原路径自己存。返回 1 = 已拨（在局内）
// Make the game run its own timed autosave right now (called when the front end goes to the background): every tick, in the tick entry function (main+0x5d3bc0, G = first argument), the game checks
// "now − last save time [G+0x5800] (ms) > 300000" and if so serializes, saves and commits all players of this run (main+0x5d4284–0x5d4c20). The original game has no "save on HOME / going to background" path
// (it does not save within 2.5 seconds of receiving the background focus notification), so the last save time is set to long ago and on the next tick it saves by itself along the original path. Returns 1 = set (in a run)
int dm_request_save(void) {
    if (!in_game()) return 0;
    uint64_t last = 0; if (!tk_rd(g_lastG + 0x5800, &last, 8) || !last) return 0;   // 0 = 游戏还没开始计时（刚进局第一拍），不动 | 0 = the game has not started timing yet (first tick right after joining the run), leave it alone
    *(volatile uint64_t *)(uintptr_t)(g_lastG + 0x5800) = 1;
    return 1;
}
// 游戏是不是停着（暂停菜单 / 大秘境选层页这类开着：同上两个界面状态字节）。不在局内返回 0
// Whether the game is stopped (pause menu / Greater Rift level-selection page and the like open: the same two UI state bytes as above). Returns 0 when not in a game
int dm_game_paused(void) { uint8_t *b = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b && in_game() && (*(volatile uint8_t *)(b + 0x2d5ff51) | *(volatile uint8_t *)(b + 0x2d5ff67)); }
// 测试用：按 id 取服务端物品单位（物品表 G+0x2e30，128 桶、next=+0x158）；不在局内 / 没找到返回 0
// For tests: get a server item unit by id (item table G+0x2e30, 128 buckets, next=+0x158); returns 0 when not in a run / not found
uint64_t dm_diag_srv_item(uint32_t id) { if (!in_game()) return 0; uint64_t u = 0; tk_rd(g_lastG + 0x2e30 + 8 * (id & 0x7f), &u, 8); for (int k = 0; u && k < 4096; k++) { uint32_t ty = 0, uid = 0; if (!tk_rd(u, &ty, 4) || !tk_rd(u + 8, &uid, 4)) return 0; if (ty == 4 && uid == id) return u; if (!tk_rd(u + 0x158, &u, 8)) return 0; } return 0; }
// 佣兵自动喂药用（dm_actions.c）：按单位 id 取服务端那只佣兵（怪物表 G+0x2630，128 桶、next=+0x158）的生命真值与「生命药药效还在」（状态 100）。
// 不读客户端那只：客户端怪物的生命只是服务端发来的 1/128 比例，进局头两百多拍还没发过、读出来是没初始化的值（宿主：满血佣兵读成 9%，误喂两瓶）。
// 返回 1 = 找到且活着（怪物模式 0 死亡中 / 12 已死不算）；游戏主线程调（服务端拍与它同一条线程）
// For mercenary auto-potion (dm_actions.c): look up the server-side mercenary by unit id (monster table G+0x2630, 128 buckets, next=+0x158) and return
// its true life and "healing potion effect still active" (state 100).
// The client-side one is not read: a client monster's life is only a 1/128 ratio sent by the server; for the first two hundred-odd ticks after joining
// it has not been sent yet and reads as an uninitialized value (host: a full-life mercenary read as 9% and was wrongly fed two potions).
// Returns 1 = found and alive (monster mode 0 dying / 12 dead do not count); called on the game main thread (server ticks run on that same thread)
int dm_diag_srv_merc(uint32_t id, int *hp, int *mx, int *healing) {
    module_t *m = main_mod(); if (!in_game() || !m) return 0; uint64_t u = 0; tk_rd(g_lastG + 0x2630 + 8 * (id & 0x7f), &u, 8);
    for (int k = 0; u && k < 4096; k++) { uint32_t ty = 0, uid = 0, mode = 0; if (!tk_rd(u, &ty, 4) || !tk_rd(u + 8, &uid, 4) || !tk_rd(u + 0xc, &mode, 4)) return 0;
        if (ty == 1 && uid == id) { if (mode == 0 || mode == 12) return 0;
            int (*get)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(m->base + 0x5b9290); *hp = get(u, 6, 0) >> 8; *mx = get(u, 7, 0) >> 8;
            uint64_t sl = 0; uint32_t fl = 0, w = 0; *healing = tk_rd(u + 0x88, &sl, 8) && sl && tk_rd(sl + 0x1c, &fl, 4) && fl >> 31 && tk_rd(sl + 0xb30 + 4 * (100 >> 5), &w, 4) && (w >> (100 & 31) & 1);   // 状态位图读法同 dm_actions.c a_has_state | State bitmap is read the same way as a_has_state in dm_actions.c
            return 1; }
        if (!tk_rd(u + 0x158, &u, 8)) return 0; }
    return 0;
}
int dm_diag_difficulty(void) { uint8_t d = 0; return in_game() && tk_rd(g_lastG + 0x104, &d, 1) && d < 3 ? d : -1; }   // 当前局难度（G+0x104：0 普通 1 噩梦 2 地狱），不在局内 -1；一键重开用（dm_cursor.c） | Difficulty of the current run (G+0x104: 0 Normal 1 Nightmare 2 Hell), -1 when not in a run; used by one-tap restart (dm_cursor.c)
uint32_t dm_cur_tick(void) { uint32_t t = 0; if (in_game()) tk_rd(g_lastG + 0x170, &t, 4); return t; }   // 触点脚本（touch_mac.c）用 | For the touch point script (touch_mac.c)
static uint32_t cur_tick(void) { return dm_cur_tick(); }
static _Atomic int g_holding;   // DM_TICK_HOLD 冻结中（定义见下） | DM_TICK_HOLD freeze in progress (defined below)
// hid 脚本用：不在局内、或 DM_TICK_HOLD 冻结中（拍号不前进，按拍计的延时永远到不了——host_savecmp 退出键序 %1 死等 451 s）返回 UINT64_MAX，脚本改按出图帧计
// For the hid script: when not in a run, or while DM_TICK_HOLD is frozen (the tick number does not advance, so tick-counted delays never come due -- the host_savecmp exit key sequence %1 waited dead for 451 s), return UINT64_MAX and the script counts by presented frames instead
uint64_t dm_server_tick(void) { return in_game() && !atomic_load(&g_holding) ? dm_cur_tick() : UINT64_MAX; }
static void c2s_sys_obs(const uint64_t *regs) {         // 观测门 4：0x6310b0(buf = u32 客户端号 + 包, 包长) | Observation gate 4: 0x6310b0(buf = u32 client number + packet, packet length)
    uint32_t len = (uint32_t)regs[1], cid; if (tk_rd(regs[0], &cid, 4) && !g_sysclient_set) { g_sysclient = cid; g_sysclient_set = 1; }
    if (!g_c2sfp) return;
    uint8_t b[512]; uint32_t n = len < 512 ? len : 512; if (!tk_rd(regs[0] + 4, b, n)) n = 0;
    fprintf(g_c2sfp, "{\"tick\":%u,\"q\":\"sys\",\"unit\":null,\"len\":%u,\"pkt\":\"", cur_tick(), len);
    for (uint32_t i = 0; i < n; i++) fprintf(g_c2sfp, "%02x", b[i]);
    fputs("\"}\n", g_c2sfp); fflush(g_c2sfp);
}
static uint32_t g_rpfirst, g_rplast_tick;              // 回放文件首 / 末条拍号：只在 [首, 末] 区间丢弃宿主客户端自发的消息（之前的加入、之后的存盘退出放行） | First / last tick numbers in the replay file: self-initiated host client messages are dropped only within [first, last] (the join before it and the save-and-exit after it are let through)
static int rp_window(void) { uint32_t t = cur_tick(); return g_rp && g_nrp && g_lastG && t >= g_rpfirst && t <= g_rplast_tick; }
// 客户端时序消息 0x6f（进度，写客户端对象 +0x6c0）/ 0x6d（心跳，处理 0x6317f4）：回放时既不注入真值的、也不丢宿主自发的。
// 依据（静态查全）：+0x6c0 只被拿去回确认 0x2d（各发包路径 ldr 6c4/6c0、不等则 str 6c4 并发 0x2d）和填下行包字段
// （0x6ced34 → 0x95 包；0x7bdb2c → 玩家数据 +0x204 → 0x7bce14 外推 +0x208 / 0x7e70c4 → 0xa5 包），不进拍内状态、也不控拍节奏；
// 0x6d 只回 0x2d / 0x8f 并记服务端墙钟到 +0x518。注入真值的 = 把另一条时间线的确认回给宿主客户端：cvd5 负载下在拍 244（真值心跳）被踢
// Client timing messages 0x6f (progress, writes client object +0x6c0) / 0x6d (heartbeat, handled at 0x6317f4): during replay, neither inject the ground-truth ones nor drop the host's self-initiated ones.
// Basis (exhaustive static search): +0x6c0 is only used to send back acknowledgement 0x2d (each send path does ldr 6c4/6c0, and if unequal does str 6c4 and sends 0x2d) and to fill downstream packet fields
// (0x6ced34 → packet 0x95; 0x7bdb2c → player data +0x204 → 0x7bce14 extrapolates +0x208 / 0x7e70c4 → packet 0xa5); it does not enter in-tick state and does not control tick pacing;
// 0x6d only replies 0x2d / 0x8f and records the server wall clock at +0x518. Injecting the ground-truth ones = returning another timeline's acknowledgements to the host client: under cvd5 load it got kicked at tick 244 (a ground-truth heartbeat)
static int sys_clock_msg(uint8_t id) { return id == 0x6f || id == 0x6d; }
void c2s_deliver_sys(uint64_t *regs) {   // svc #0x82：收包泵 → 0x6310b0(buf, 包长) | svc #0x82: receive pump → 0x6310b0(buf, packet length)
    uint8_t id = 0; tk_rd(regs[0] + 4, &id, 1);
    if (rp_window() && !sys_clock_msg(id)) { if (!g_sysdrop++) LOG("DM_C2S_REPLAY：拍 %u 起丢弃客户端自发的系统类消息（首条）", cur_tick()); return; }
    static void (*sysm)(uint64_t, uint64_t);
    if (!sysm) { module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
                 sysm = (void (*)(uint64_t, uint64_t))(m->base + 0x6310b0); }
    sysm(regs[0], regs[1]);
}
// 产品规则：背包放不下时，拾取自动进赫拉迪克方块。游戏自己有「地上物品直接捡进方块」的命令包 0x5f（手柄版专门操作发的，
// 服务端 0x6e4850 自己复核方块位置与空位后落位并通知客户端）；这里只在投递前把满足条件的普通拾取包 0x16 换成 0x5f，物品移动全走原版路径。
// 条件全用游戏自己的查询函数判断：自动找位（装备位 / 腰带 / 背包，0x646840）失败 + 背包里有方块（0x565e50）+ 方块页有位（0x55f620 页 3）。
// 卷轴 / 书 / 可堆叠物不换（原版里背包满时它们还能入书或合并）；拾到光标（标志 0）不换；任一条件不成立原包原样投递。DM_CUBE_PICKUP=0 关。
// 0x16：[16][u32 物品id][u32 x][u32 y][u32 标志]；0x5f：[5f][u32 物品id][u32 方块id][u32 x][u32 y][u32 方块在背包 x|y<<16][u32 方块内目标 x|y<<16]
// Product rule: when the inventory is full, pickups go into the Horadric Cube automatically. The game has its own "pick a ground item straight into the cube" command packet 0x5f (sent by a controller-only action;
// the server's 0x6e4850 re-checks the cube position and free space itself, then places the item and notifies the client); here we only swap a qualifying ordinary pickup packet 0x16 for 0x5f before delivery, so all item movement goes through the original game's path.
// All conditions are checked with the game's own query functions: auto-placement (equipment slots / belt / inventory, 0x646840) fails + there is a cube in the inventory (0x565e50) + the cube page has room (0x55f620 page 3).
// Scrolls / tomes / stackables are not swapped (in the original game they can still go into a tome or merge when the inventory is full); pickup to cursor (flag 0) is not swapped; if any condition fails the original packet is delivered unchanged. DM_CUBE_PICKUP=0 turns it off.
// 0x16: [16][u32 item id][u32 x][u32 y][u32 flag]; 0x5f: [5f][u32 item id][u32 cube id][u32 x][u32 y][u32 cube position in inventory x|y<<16][u32 target inside cube x|y<<16]
typedef struct { uint64_t a; uint32_t b, c; uint8_t ok; uint8_t pad[47]; } autoplace_res;   // 0x646840 的结果（经 x8 返回），+0x10 = 是否找到位置 | Result of 0x646840 (returned via x8), +0x10 = whether a position was found
static int cube_pickup_rewrite(const uint64_t *regs, uint8_t out[4 + 25]) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_CUBE_PICKUP"); on = !(e && !strcmp(e, "0")); }
    uint8_t in[4 + 17]; uint32_t id, flag;
    if (!on || regs[1] != 17 || !tk_rd(regs[0], in, sizeof in) || in[4] != 0x16) return 0;
    memcpy(&id, in + 5, 4); memcpy(&flag, in + 17, 4); if (!flag) return 0;
    module_t *m = main_mod(); uint64_t G = g_lastG, p = g_player_unit, u = 0, game = 0, inv = 0, pp = 0;
    if (!m || !in_game() || !p || !tk_rd(G + 0x2e30 + 8 * (id & 0x7f), &u, 8)) return 0;
    for (int k = 0; u && k < 4096; k++) {   // 物品表：128 桶，下一件 +0x158 | Item table: 128 buckets, next item at +0x158
        uint32_t t = 0, i = 0; if (!tk_rd(u, &t, 4) || !tk_rd(u + 8, &i, 4)) return 0;
        if (t == 4 && i == id) break;
        if (!tk_rd(u + 0x158, &u, 8)) return 0;
    }
    uint32_t mode = 0; if (!u || !tk_rd(u + 0xC, &mode, 4) || mode != 3) return 0;   // 3 = 在地上 | 3 = on the ground
    int (*is_type)(uint64_t, int) = (void *)(m->base + 0x574bc0); int (*stackable)(uint64_t) = (void *)(m->base + 0x5722f0);
    if (is_type(u, 0x16) || is_type(u, 0x12) || stackable(u)) return 0;
    if (!tk_rd(p + 0xd8, &game, 8) || !game || !tk_rd(p + 0x90, &inv, 8) || !inv) return 0;
    if (((autoplace_res (*)(uint64_t, uint64_t, uint64_t))(m->base + 0x646840))(game, p, u).ok) return 0;
    uint64_t cube = ((uint64_t (*)(uint64_t))(m->base + 0x565e50))(p); if (!cube) return 0;
    uint32_t tx = 0, ty = 0, cid = 0, cx = 0, cy = 0;
    if (!((int (*)(uint64_t, uint64_t, int, uint32_t *, uint32_t *, int))(m->base + 0x55f620))(inv, u, 9, &tx, &ty, 3)) return 0;
    if (!tk_rd(cube + 8, &cid, 4) || !tk_rd(cube + 0x38, &pp, 8) || !pp || !tk_rd(pp + 0x10, &cx, 4) || !tk_rd(pp + 0x14, &cy, 4)) return 0;
    uint32_t cxy = (cx & 0xffff) | cy << 16, txy = (tx & 0xffff) | ty << 16;
    memcpy(out, in, 4); out[4] = 0x5f; memcpy(out + 5, &id, 4); memcpy(out + 9, &cid, 4); memcpy(out + 13, in + 9, 8); memcpy(out + 21, &cxy, 4); memcpy(out + 25, &txy, 4);
    static int logged; if (logged++ < 40) { uint32_t code = ((uint32_t (*)(uint64_t))(m->base + 0x570260))(u); int dm_potpick_auto(uint32_t);
        LOG("规则：背包放不下，拾取物品 %u（%.4s，%s）改进方块 %u（方块格 %u,%u）", id, (const char *)&code, dm_potpick_auto(id) ? "药水自动拾取发的" : "玩家点的", cid, tx & 0xffff, ty); }
    return 1;
}
// 药水自动拾取的服务端一道（说明见 dm_potpick.c 文件头）：自动拾取发出的 0x16 在处理这一刻用原版腰带找位 main+0x561de0(物品栏, 物品, &槽, 1)
// 对服务端状态再核一次，放不进腰带就丢包（返回 1），药留在地上——不进背包、不进魔盒。玩家自己点的 0x16 不在自动拾取的表里，不经过这里。
// Server-side pass of potion auto-pickup (see the dm_potpick.c file header): at the moment a 0x16 sent by auto-pickup is handled, check server state once more with the original game's belt slot search main+0x561de0(inventory object, item, &slot, 1);
// if it does not fit in the belt, drop the packet (return 1) and the potion stays on the ground -- not into the inventory, not into the cube. A 0x16 the player tapped themselves is not in the auto-pickup table and does not pass through here.
static int potpick_gate(const uint64_t *regs) {
    int dm_potpick_auto(uint32_t); uint8_t in[4 + 17]; uint32_t id;
    if (regs[1] != 17 || !tk_rd(regs[0], in, sizeof in) || in[4] != 0x16) return 0;
    memcpy(&id, in + 5, 4); if (!dm_potpick_auto(id)) return 0;
    module_t *m = main_mod(); uint64_t G = g_lastG, p = g_player_unit, u = 0, inv = 0;
    if (!m || !in_game() || !p || !tk_rd(G + 0x2e30 + 8 * (id & 0x7f), &u, 8)) return 0;
    for (int k = 0; u && k < 4096; k++) { uint32_t t = 0, i = 0; if (!tk_rd(u, &t, 4) || !tk_rd(u + 8, &i, 4)) return 0; if (t == 4 && i == id) break; if (!tk_rd(u + 0x158, &u, 8)) return 0; }
    uint32_t mode = 0, slot = 0; if (!u || !tk_rd(u + 0xC, &mode, 4) || mode != 3 || !tk_rd(p + 0x90, &inv, 8) || !inv) return 0;
    uint32_t code = ((uint32_t (*)(uint64_t))(m->base + 0x570260))(u);
    if (((int (*)(uint64_t, uint64_t, uint32_t *, int))(m->base + 0x561de0))(inv, u, &slot, 1)) { static int ok; if (ok++ < 40) LOG("药水自动拾取：物品 %u（%.4s）到服务端时腰带放得进（槽 %u），放行", id, (const char *)&code, slot); return 0; }
    static int dr; if (dr++ < 40) LOG("药水自动拾取：物品 %u（%.4s）到服务端时腰带已放不下 → 不捡，留在地上（丢包第 %d 次）", id, (const char *)&code, dr);
    return 1;
}
static int dm_move_allowed(void) { extern _Atomic uint64_t g_dm_move_allow_until_ns; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec < atomic_load(&g_dm_move_allow_until_ns); }   // 由目标触发的走近（dm_world.c） | Approach triggered by a target (dm_world.c)
void c2s_deliver(uint64_t *regs) {   // svc #0x81：收包泵 → 0x632510(包, 长度, 标志, 回调) | svc #0x81: receive pump → 0x632510(packet, length, flags, callback)
    { uint8_t lop = 0xff; if (dm_lat_on() && regs[0] && regs[1] >= 1 && tk_rd(regs[0] + 4, &lop, 1) && lop != 0x6d) DM_LAT("c2s %02x", lop); }
    // 只按拍号区间 [文件首拍, 末拍] 丢（之前的进局装载期命令、之后的存盘退出放行，与真值局一致：那些命令真值客户端照发、只是没录进文件）。
    // 去掉原先的「!g_lastG（尚未见到拍入口）也丢」：那是按到达时序判的——装载期客户端命令赶在服务端第一拍之前到达就被丢，
    // 晚到则放行，丢掉的若是装载握手请求，客户端会永远等应答（r13 cvd4 一次进局装载期卡死、服务端只跑 0–2 拍，疑为此）
    // Drop only within the tick range [first tick in file, last tick] (the join-time loading commands before it and the save-and-exit after it are let through, matching the ground-truth run: the ground-truth client sent those commands too, they just were not recorded in the file).
    // Removed the former "also drop when !g_lastG (tick entry not yet seen)": that was judged by arrival timing -- client commands during loading that arrived before the server's first tick were dropped,
    // later ones were let through; if a dropped one was a loading handshake request, the client would wait for the reply forever (r13 cvd4 once hung in join-time loading with the server running only ticks 0–2, suspected to be this)
    if (g_rp && (!g_nrp || rp_window())) { if (!g_rpdrop++) LOG("DM_C2S_REPLAY：拍 %u 起丢弃客户端自发命令（首条）", cur_tick()); return; }
    // 命令号在投递缓冲的第 4 字节（前 4 字节是头，同 cube_pickup_rewrite）：原先这里读的是第 0 字节，规则从没匹配上过。
    // 产品规则：除摇杆外任何触点不许触发移动。键鼠版「点地走 / 跑到某点」是命令包 0x01 / 0x03；手柄摇杆移动是 0x42，
    // 松开摇杆时补一条 0x01（停在当前位置，实测）。所以：玩家摇杆最近 0.7 秒内没动过时到达的 0x01 / 0x03 一律不投递
    // （来源只可能是界面鼠标点击落到了点击接收器上）。走向目标（0x02 / 0x04 等带单位的）不拦——点 NPC / 物品走过去是原版行为。
    // The command number is byte 4 of the delivery buffer (the first 4 bytes are a header, same as cube_pickup_rewrite): this used to read byte 0, so the rule never matched.
    // Product rule: no touch point other than the stick may trigger movement. The keyboard-and-mouse "click ground to walk / run to a point" is command packet 0x01 / 0x03; controller stick movement is 0x42,
    // and releasing the stick adds one 0x01 (stop at the current position, measured). So: any 0x01 / 0x03 that arrives when the player's stick has not moved within the last 0.7 seconds is not delivered
    // (the only possible source is a UI mouse click landing on the click receiver). Walking to a target (0x02 / 0x04 etc., which carry a unit) is not blocked -- walking over after tapping an NPC / item is the original game's behavior.
    { uint64_t dm_stick_last_ns(void); uint8_t op = 0xff; if (regs[0] && regs[1] >= 1 && tk_rd(regs[0] + 4, &op, 1) && (op == 0x01 || op == 0x03) && now_ns() - dm_stick_last_ns() > 700000000ull && !dm_move_allowed()) {
        static int dropped; if (dropped++ < 10) LOG("产品规则：丢弃非摇杆来源的点地移动包 0x%02x（第 %d 条）", op, dropped); return; } }
    static void (*recv)(uint64_t, uint64_t, uint64_t, uint64_t);
    if (!recv) { module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
                 recv = (void (*)(uint64_t, uint64_t, uint64_t, uint64_t))(m->base + 0x632510); }
    if (potpick_gate(regs)) return;
    uint8_t cube[4 + 25]; if (cube_pickup_rewrite(regs, cube)) { recv((uint64_t)(uintptr_t)cube, 25, regs[2], regs[3]); return; }
    // 捡物品包（0x16）记一行去向（腰带案：玩家手动点地上的药捡起来原先不打日志，线上日志缺这一环）：物品码、谁发的、处理完落在哪个容器 / 页 / 格。
    // 处理是同步的（收包泵里直接分发），所以投递前记下 id 与码、投递后读这件在服务端的位置。封顶：每 25 拍最多 4 行，超出的只计数、下一行带出，刷怪掉落高峰冲不爆日志。
    // For the pick-up-item packet (0x16), log one line saying where the item went (belt case: a player manually tapping a potion on the ground to pick it up
    // used to log nothing, so logs from released builds were missing this link): item code, who sent it, and which container / page / cell it ended up in after
    // handling.
    // Handling is synchronous (dispatched directly in the packet receive pump), so record the id and code before delivery and read the item's server-side
    // position after delivery. Cap: at most 4 lines per 25 ticks; the excess is only counted and reported on the next line, so monster-drop peaks cannot
    // flood the log.
    uint32_t pk_id = 0, pk_code = 0; int pk_auto = 0;
    { uint8_t in[4 + 17]; int dm_potpick_auto(uint32_t); module_t *m = main_mod();
      if (m && in_game() && regs[1] == 17 && tk_rd(regs[0], in, sizeof in) && in[4] == 0x16) { memcpy(&pk_id, in + 5, 4); uint64_t u = dm_diag_srv_item(pk_id);
          if (u) { pk_code = ((uint32_t (*)(uint64_t))(m->base + 0x570260))(u); pk_auto = dm_potpick_auto(pk_id); } else pk_id = 0; } }
    recv(regs[0], regs[1], regs[2], regs[3]);
    if (pk_id) { static uint32_t win, used, skipped; uint32_t t = cur_tick(); if (t - win >= 25) { win = t; used = 0; }
        if (used >= 4) skipped++;
        else { used++; uint64_t u = dm_diag_srv_item(pk_id), d = 0, pp = 0; uint32_t mode = 0, x = 0, y = 0; uint8_t page = 0; char where[64];
            if (!u || !tk_rd(u + 0xc, &mode, 4) || !tk_rd(u + 0x10, &d, 8) || !d) snprintf(where, sizeof where, "服务端已没有这件（叠进了别的一叠 / 金币等）");
            else if (mode == 3) snprintf(where, sizeof where, "还在地上（没捡成）");
            else { tk_rd(d + 0x55, &page, 1); if (tk_rd(u + 0x38, &pp, 8) && pp) { tk_rd(pp + 0x10, &x, 4); tk_rd(pp + 0x14, &y, 4); } snprintf(where, sizeof where, "容器 %u 页 %u 格 %d,%d", mode, page, (int)x, (int)y); }
            if (skipped) LOG("拾取：物品 %u（%.4s，%s）→ %s（此前另有 %u 条没记）", pk_id, (const char *)&pk_code, pk_auto ? "药水自动拾取发的" : "玩家点的", where, skipped);
            else LOG("拾取：物品 %u（%.4s，%s）→ %s", pk_id, (const char *)&pk_code, pk_auto ? "药水自动拾取发的" : "玩家点的", where);
            skipped = 0; } }
}
static uint64_t unit_by_id(uint64_t G, uint32_t id) {   // 玩家表 G+0x2230：128 桶、next=+0x158（tick_state_format.md 同口径） | Player table G+0x2230: 128 buckets, next=+0x158 (same convention as tick_state_format.md)
    for (int b = 0; b < 128; b++) {
        uint64_t u = 0; tk_rd(G + 0x2230 + 8 * b, &u, 8);
        for (int k = 0; u && k < 4096; k++) { uint32_t x; if (!tk_rd(u + 8, &x, 4)) break; if (x == id) return u; tk_rd(u + 0x158, &u, 8); }
    }
    return 0;
}
static void c2s_inject(uint64_t G) {
    uint32_t tick; if (!tk_rd(G + 0x170, &tick, 4) || tick == g_rplast) return;
    g_rplast = tick;
    {   // 合成 UI 消息「DM:Tick:<拍号>#」：DM_KEYS 可按拍号触发（如真值局后存盘退出从游玩段末拍起按键）；# 防前缀误配 | Synthetic UI message "DM:Tick:<tick>#": DM_KEYS can trigger by tick number (e.g. after a ground-truth run, the save-and-exit keys start from the last tick of the play segment); # guards against prefix mismatches
        char a[16]; snprintf(a, sizeof a, "%u#", tick);
        for (int i = 0, n = atomic_load(&g_nui_sub); i < n; i++) g_ui_sub[i]("DM", "Tick", a);
    }
    while (g_irp < g_nrp && g_rp[g_irp].tick < tick) { LOG("【红】DM_C2S_REPLAY：第 %zu 条拍号 %u 已过（当前 %u），丢弃", g_irp + 1, g_rp[g_irp].tick, tick); g_irp++; }
    static void (*disp)(uint64_t, uint64_t, uint64_t, uint32_t);
    if (!disp) { module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
                 disp = (void (*)(uint64_t, uint64_t, uint64_t, uint32_t))(m->base + 0x6e5b40); }
    static void (*sysm)(uint64_t, uint64_t);
    if (!sysm) { module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
                 sysm = (void (*)(uint64_t, uint64_t))(m->base + 0x6310b0); }
    size_t end = g_irp; while (end < g_nrp && g_rp[end].tick == tick) end++;
    for (size_t k = g_irp; k < end; k++) if (g_rp[k].sys && g_rp[k].len && !sys_clock_msg(g_rp[k].pkt[0])) {   // 收包泵先抽干系统类队列，再处理命令包：同一拍里照此顺序 | The receive pump drains the system-class queue first, then handles command packets: keep that order within the same tick
        struct c2s_ent *e = &g_rp[k]; uint8_t *buf = malloc(4 + e->len);
        memcpy(buf, &g_sysclient, 4); memcpy(buf + 4, e->pkt, e->len); sysm((uint64_t)buf, e->len); free(buf);
    }
    for (; g_irp < end; g_irp++) {
        struct c2s_ent *e = &g_rp[g_irp]; if (e->sys) continue;
        uint64_t u = e->has_unit ? unit_by_id(G, e->unit) : 0;
        if (e->has_unit && !u) die("【红】DM_C2S_REPLAY 第 %zu 条：拍 %u 找不到单位 id %u", g_irp + 1, tick, e->unit);
        volatile uint32_t *busy = (volatile uint32_t *)(G + 0x19050);
        *busy = 1; disp(G, u, (uint64_t)e->pkt, e->len); *busy = 0;
    }
    if (g_irp == g_nrp && g_nrp) { static int once; if (!once++) LOG("DM_C2S_REPLAY：%zu 条全部注入（最后拍 %u）", g_nrp, tick); }
    if (g_nrp && tick > g_rplast_tick) {   // 过了文件末拍：宿主客户端自发消息从此放行；发合成 UI 消息「DM:ReplayDone:」，DM_KEYS 可等它再按存盘退出 | Past the last tick in the file: self-initiated messages from the host client are let through from here on; emit the synthetic UI message "DM:ReplayDone:" so DM_KEYS can wait for it before pressing save-and-exit
        static int done; if (!done++) {
            LOG("DM_C2S_REPLAY：拍 %u 已过文件末拍 %u，放行客户端自发消息（合成 UI 消息 DM:ReplayDone）", tick, g_rplast_tick);
            for (int i = 0, n = atomic_load(&g_nui_sub); i < n; i++) g_ui_sub[i]("DM", "ReplayDone", "");
        }
    }
}
static void c2s_load(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) die("DM_C2S_REPLAY 打不开 %s", path);
    char line[2048]; size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '{')) continue;
        struct c2s_ent e = {0}; char *p;
        if (!(p = strstr(line, "\"tick\":"))) die("DM_C2S_REPLAY 行缺 tick：%s", line); e.tick = (uint32_t)strtoul(p + 7, NULL, 10);
        if ((p = strstr(line, "\"q\":"))) { p += 4; while (*p == ' ') p++; e.sys = !strncmp(p, "\"sys\"", 5); }
        if (!(p = strstr(line, "\"unit\":"))) die("DM_C2S_REPLAY 行缺 unit：%s", line); p += 7; while (*p == ' ') p++;
        if (strncmp(p, "null", 4)) { e.has_unit = 1; e.unit = (uint32_t)strtoul(p, NULL, 10); }
        if (!(p = strstr(line, "\"len\":"))) die("DM_C2S_REPLAY 行缺 len：%s", line); e.len = (uint32_t)strtoul(p + 6, NULL, 10);
        if (!(p = strstr(line, "\"pkt\":"))) die("DM_C2S_REPLAY 行缺 pkt：%s", line); p = strchr(p + 6, '"') + 1;
        size_t nh = strcspn(p, "\""); if (nh / 2 != e.len) die("【红】DM_C2S_REPLAY 拍 %u：包长 %u 与字节数 %zu 不符（> 512 字节的包录制时被截断，无法回放）", e.tick, e.len, nh / 2);
        e.pkt = malloc(e.len ? e.len : 1);
        for (uint32_t i = 0; i < e.len; i++) { unsigned v; sscanf(p + 2 * i, "%2x", &v); e.pkt[i] = (uint8_t)v; }
        if (g_nrp && e.tick < g_rp[g_nrp - 1].tick) die("【红】DM_C2S_REPLAY 拍号倒退 %u < %u", e.tick, g_rp[g_nrp - 1].tick);
        if (g_nrp == cap) { cap = cap ? cap * 2 : 256; g_rp = realloc(g_rp, cap * sizeof *g_rp); }
        g_rp[g_nrp++] = e;
    }
    fclose(f);
    if (!g_rp) g_rp = malloc(1);                      // 空文件也是回放模式：只丢弃自发命令 | An empty file is replay mode too: only self-initiated commands are dropped
    g_rpfirst = g_nrp ? g_rp[0].tick : 0; g_rplast_tick = g_nrp ? g_rp[g_nrp - 1].tick : 0;
    LOG("DM_C2S_REPLAY ← %s：%zu 条%s", path, g_nrp, g_nrp ? "" : "（空：只关自发命令）");
}

// DM_TICK_HOLD=拍号[:UI 子串]（测试驱动用）：进入该拍即冻结客体时钟，见到 UI 子串（缺省 PausePanelMessage:ExitGame）才解冻 ⇒ 期间到不了下一拍。
// 解冻点不能是 OpenPanel:PauseLayout：cvd5 实测该消息发出时暂停尚未生效，解冻后又溜过一拍（402 对真值 401），两遍相同。
// 用途：存盘退出按键钉在拍 E 入口按下，暂停（= 存盘）必落在拍 E。实测不冻结时同样在拍 N 按键，暂停落在 N 或 N+1（395→396、409→410、400→400）；10 秒兜底报红
// DM_TICK_HOLD=E:SHOT（截图用）：拍 E 冻结，冻结期间第一张截图写盘完成（dm_shot_written，成败都算）即解冻，不等 UI 消息
// 冻结只停客体时钟还不够：主循环 0x2c1340 每帧先取一次时刻快照 t（0x2c1b34），再反复调拍分派 0x5d5120(t)，直到「下一拍时限
// [main+0x2d685d8] > t」才停（每跑一拍时限 +40，0x5d51fc）。服务端落后 ≥40 ms 时，同一帧会用冻结前的快照连跑补拍，
// 冻结拍 E 之后照样跑 E+1（负载 28 下暂停拍 402 对目标 401）。所以冻结时把时限抬到不可达，解冻时还原为
// max(原值 + 40, 解冻时刻 + 40)：无积压时 = 原值 + 40，与不冻结相同；有积压时免掉补拍（真值局拍 E 之后不再有拍）。
// DM_TICK_HOLD=tick[:UI substring] (for the test driver): freeze the guest clock on entering that tick, and unfreeze only when the UI substring (default PausePanelMessage:ExitGame) is seen ⇒ the next tick cannot be reached in between.
// The unfreeze point cannot be OpenPanel:PauseLayout: measured on cvd5, the pause has not yet taken effect when that message is sent, and one more tick slips by after unfreezing (402 vs ground truth 401), identical on both passes.
// Use: the save-and-exit key press is pinned to the entry of tick E, so the pause (= save) must land on tick E. Measured without freezing, pressing likewise at tick N, the pause lands on N or N+1 (395→396, 409→410, 400→400); a 10-second fallback reports a failure
// DM_TICK_HOLD=E:SHOT (for screenshots): freeze at tick E, unfreeze as soon as the first screenshot during the freeze has been written to disk (dm_shot_written, success or failure both count), without waiting for a UI message
// Freezing only the guest clock is not enough: the main loop 0x2c1340 takes one time snapshot t per frame (0x2c1b34), then repeatedly calls the tick dispatch 0x5d5120(t) until "next tick deadline
// [main+0x2d685d8] > t" (each tick run adds +40 to the deadline, 0x5d51fc). When the server is ≥40 ms behind, the same frame runs catch-up ticks using the pre-freeze snapshot,
// so E+1 still runs after the frozen tick E (under load 28, pause tick 402 vs target 401). So on freeze the deadline is raised to unreachable, and on unfreeze restored to
// max(original + 40, unfreeze time + 40): with no backlog = original + 40, same as without freezing; with backlog the catch-up ticks are skipped (in the ground-truth run there are no ticks after tick E).
static uint32_t g_hold_tick; static char g_hold_ui[96] = "PausePanelMessage:ExitGame"; static _Atomic int g_holding; static uint64_t g_hold_t0;
static volatile uint32_t *g_hold_dl; static uint32_t g_hold_dl0; static int32_t g_hold_org; static int g_hold_org_ok; static uint32_t g_hold_dl1;
static uint32_t guest_ms(void) { return (uint32_t)(guest_ns() / 1000000); }
static volatile uint32_t *hold_dl(void) {
    if (!g_hold_dl) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_hold_dl = (volatile uint32_t *)(g_mod[i].base + 0x2d685d8);
    return g_hold_dl;
}
// 快照 t 是局内毫秒（时限从 0 起、每拍 +40，实测拍 200 时限 = 8000），原点 O 不直接可读：t = 客体毫秒 − O。
// 每次拍入口都有 时限 ≤ t快照 ≤ 客体毫秒 − O ⇒ (客体毫秒 − 时限) ≥ O；无积压时到期后一帧内就跑 ⇒ 取最小值估 O，只会估大（≤ 一帧）。
// The snapshot t is in-run milliseconds (the deadline starts at 0 and gains +40 per tick; measured deadline at tick 200 = 8000); the origin O is not directly readable: t = guest ms − O.
// At every tick entry, deadline ≤ t snapshot ≤ guest ms − O ⇒ (guest ms − deadline) ≥ O; with no backlog the tick runs within one frame of coming due ⇒ take the minimum to estimate O, which can only overestimate (≤ one frame).
static void hold_track_origin(void) {
    int32_t d = (int32_t)(guest_ms() - *hold_dl());
    if (!g_hold_org_ok || d < g_hold_org) { g_hold_org = d; g_hold_org_ok = 1; }
}
static void hold_freeze(void) {
    hold_dl();
    g_hold_dl0 = *g_hold_dl; *g_hold_dl = 0xF0000000u;   // 拍 E 跑完 +40 仍远大于任何时刻快照（无符号比较 b.ls） | After tick E runs, +40 is still far greater than any time snapshot (unsigned compare, b.ls)
    guest_clock_freeze(1);
}
static void hold_unfreeze(void) {
    guest_clock_freeze(0);
    if (!g_hold_dl) return;
    uint32_t t = guest_ms() - (uint32_t)g_hold_org, d = g_hold_dl0 + 40;   // t 只会估小（O 估大 ≤ 一帧），+40 后仍 > 真 t ⇒ 解冻后不立即补拍 | t can only be underestimated (O is overestimated by ≤ one frame), so after +40 it is still > the true t ⇒ no immediate catch-up tick after unfreezing
    *g_hold_dl = g_hold_dl1 = (int32_t)(d - (t + 40)) < 0 ? t + 40 : d;
}
static void hold_release(const char *why) {
    if (!atomic_exchange(&g_holding, 0)) return;
    uint32_t last = (uint32_t)g_rplast;                // 冻结期间最后一次拍入口的拍号 = 暂停（存盘 / 截图）拍 | Tick number of the last tick entry during the freeze = the pause (save / screenshot) tick
    hold_unfreeze();
    LOG("DM_TICK_HOLD：「%s」→ 解冻（冻结 %.3f s）；冻结拍 %u（目标 %u）%s；下一拍时限 %u → %u", why, (now_ns() - g_hold_t0) / 1e9, last, g_hold_tick,
        last == g_hold_tick ? "" : "【红】不等", g_hold_dl0 + 40, g_hold_dl1);
}
// 按键脚本用（hle_hid.c）：按拍号触发的局内按键到点时读取画面还没撤（负载高时客户端加载慢、服务端拍照走—— 画面闸门 NPC 页
// 拍 84 按 A 被读取画面吃掉，读取画面拍 98 才撤），冻结客体时钟等它撤下，按键随后再按；读取画面本来就先撤时不触发、时序不变
// For the key script (hle_hid.c): when an in-run key press triggered by tick number comes due while the loading screen is still up (under high load the client loads slowly while the server keeps ticking -- screenshot gate NPC page:
// the A press at tick 84 was swallowed by the loading screen, which only went away at tick 98), freeze the guest clock until it goes away and press the key afterwards; when the loading screen is already gone first, this does not trigger and timing is unchanged
static _Atomic int g_lholding, g_lhold_giveup;
static void lhold_release(void);
void dm_hold_for_loadscreen(void) {
    if (!g_hold_org_ok || atomic_load(&g_lhold_giveup)) return;
    // 冻了 8 秒读取画面还没撤 = 这次装载要服务端走拍才能完成（局内经小站 / 传送门换图：客户端等服务端把人挪过去），冻着时钟就互相等死。解冻放行，这块读取画面不再冻
    // Loading screen still up after 8 seconds frozen = this load needs the server to tick before it can finish (in-run map change via waypoint / portal: the client waits for the server to move the player over); with the clock frozen they deadlock waiting on each other. Unfreeze and let it through; this loading screen is not frozen again
    if (atomic_load(&g_lholding)) { if (now_ns() - g_hold_t0 > 8000000000ull) { atomic_store(&g_lhold_giveup, 1); LOG("读取画面冻结 8 秒没撤：这次装载要服务端走拍，解冻放行（本次读取画面不再冻结）"); lhold_release(); } return; }
    if (atomic_load(&g_holding) || atomic_exchange(&g_lholding, 1)) return;
    hold_freeze(); g_hold_t0 = now_ns(); atomic_store(&g_holding, 1);
    LOG("读取画面未撤：拍 %u 冻结客体时钟，撤下后放行", cur_tick());
}
static void lhold_release(void) {
    if (!atomic_exchange(&g_lholding, 0)) return;
    if (atomic_exchange(&g_holding, 0)) { hold_unfreeze(); LOG("读取画面已撤 → 解冻（冻结 %.3f s，拍 %u）", (now_ns() - g_hold_t0) / 1e9, cur_tick()); }
}
static void hold_on_uimsg(const char *cat, const char *act, const char *arg) {
    char m[256]; snprintf(m, sizeof m, "%s:%s:%s", cat, act, arg);
    if (strcmp(g_hold_ui, "SHOT") && atomic_load(&g_holding) && strstr(m, g_hold_ui)) hold_release(m);
}
// ---- 常开轻量性能记录：每 10 秒往 perf.txt 追加一行，供直装实玩后取回（iOS 工作目录 = Documents，已开文件共享）----
// 列：墙钟时刻 / 进程秒 / 温控档位 / 3D 渲染比例 / 上屏帧率 / 帧间隔落在 16.7、25、≥33 ms 三档的占比（界线 20.8、29.2 ms）与被丢弃帧数 /
//     每帧提交 CPU ms / 场景（局内 Lv 或 菜单）。数据源 = hle_nvn.c 的实际上屏间隔（与 boot.txt「上屏」行同源），不另做统计口径。
// iOS 缺省开；DM_PERF_FILE=<路径> 指定文件（Mac 上只有设了才写），DM_PERF_FILE=0 关。文件到 256 KB 时改名 perf.old.txt 重新开始。
// ---- Always-on lightweight perf record: append one line to perf.txt every 10 seconds, to be pulled back after a direct-install real play session (iOS working directory = Documents, file sharing enabled) ----
// Columns: wall-clock time / process seconds / thermal state / 3D render scale / present frame rate / share of frame intervals falling in the 16.7, 25, ≥33 ms buckets (boundaries 20.8, 29.2 ms) and number of dropped frames /
//     per-frame submit CPU ms / scene (in-run Lv or menu). Data source = the actual present intervals in hle_nvn.c (same source as the 「上屏」(present) line in boot.txt); no separate statistic is kept.
// On by default on iOS; DM_PERF_FILE=<path> picks the file (on Mac it is only written when set), DM_PERF_FILE=0 turns it off. When the file reaches 256 KB it is renamed perf.old.txt and started over.
#include <TargetConditionals.h>
#include <sys/stat.h>
#include <time.h>
int dm_thermal_now(void);
int dm_ingame_now(void) { dm_hud_info h; dm_hud_get(&h); return h.valid; }   // 当前是否在局内（读到受控玩家）：动态 3D 比例只在局内判 | Whether we are in a run right now (the controlled player was read): the dynamic 3D scale is only evaluated in a run
// ---- 长帧日志：hle_nvn.c 在某帧 > 20 ms（或 GPU > 16.7 ms）时调一次；这里补上「读取画面是否盖着 / 当前面板 / 区域号」，
// 写进 boot.txt（LOG）并追加到 perf.txt（行首「长帧」，与每 10 秒的汇总行同一文件，实玩后取回能直接看见卡在哪一帧、卡在什么上）----
// ---- Long-frame log: hle_nvn.c calls this once when a frame is > 20 ms (or GPU > 16.7 ms); this adds "whether the loading screen is covering / current panel / area number",
// writes it to boot.txt (LOG) and appends it to perf.txt (line prefix 「长帧」(long frame), the same file as the every-10-seconds summary lines, so after real play it can be retrieved and you can see directly which frame stalled and on what) ----
char g_lf_panel[48];
static int g_loadscreen_seen;
int dm_loadscreen_up(void);
void dm_longframe_note(const char *what) {
    static const char *path; static int init; static unsigned n;
    if (!init) { init = 1; const char *e = getenv("DM_PERF_FILE"); path = e ? (strcmp(e, "0") ? e : NULL) : (DM_PHONE ? "perf.txt" : NULL); }
    dm_hud_info h; dm_hud_get(&h); char ctx[160];
    snprintf(ctx, sizeof ctx, "读取画面 %s，面板 %s，区域 %d%s", g_loadscreen_seen ? (dm_loadscreen_up() ? "盖着" : "已撤") : "未见过", g_lf_panel[0] ? g_lf_panel : "无", h.area, h.valid ? "" : "（未读到玩家）");
    LOG("[长帧] %s；%s", what, ctx);
    if (!path || n++ >= 2000) return;   // 一次运行最多落 2000 行，防异常刷屏把文件撑大 | At most 2000 lines are written per run, to keep abnormal log flooding from bloating the file
    FILE *fp = fopen(path, "a"); if (!fp) return;
    time_t w = time(NULL); struct tm tmv; localtime_r(&w, &tmv); char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    fprintf(fp, "长帧\t%s\t%s；%s\n", ts, what, ctx); fclose(fp);
}
void dm_perf_present(double iv) {   // 出图线程每帧调一次（滞后 8 帧的实际上屏间隔 ms；−1 = 被系统丢弃，0 = 无读数） | Called once per frame by the present thread (the actual present interval in ms, lagging by 8 frames; −1 = dropped by the system, 0 = no reading)
    static const char *path; static int init; static unsigned c16, c25, c33, disc; static double sum; static uint64_t t0, f0, cpu0, tstart;
    if (!init) { init = 1; const char *e = getenv("DM_PERF_FILE"); path = e ? (strcmp(e, "0") ? e : NULL) : (DM_PHONE ? "perf.txt" : NULL); t0 = tstart = now_ns(); f0 = dm_nvn_frame_count(); cpu0 = dm_nvn_sub_cpu_ns(); }
    dm_3d_scale_feed(iv);   // 动态 3D 比例的控制器吃同一份上屏间隔（hle_oe.c） | The dynamic 3D scale controller consumes the same present intervals (hle_oe.c)
    if (!path) return;
    if (iv < 0) disc++; else if (iv > 0) { sum += iv; if (iv <= 20.8) c16++; else if (iv <= 29.2) c25++; else c33++; }
    uint64_t now = now_ns(); if (now - t0 < 10000000000ull) return;
    unsigned n = c16 + c25 + c33; uint64_t f = dm_nvn_frame_count(), cpu = dm_nvn_sub_cpu_ns();
    struct stat st; if (!stat(path, &st) && st.st_size > 256 * 1024) rename(path, "perf.old.txt");
    FILE *fp = fopen(path, "a");
    if (fp) {
        if (ftell(fp) == 0) fprintf(fp, "# 时刻\t进程秒\t温控\t3D比例\t上屏帧率\t16.7ms档%%\t25ms档%%\t≥33ms档%%\t丢弃帧\t提交CPUms\t场景\n");
        time_t w = time(NULL); struct tm tmv; localtime_r(&w, &tmv); char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
        dm_hud_info h; dm_hud_get(&h); char scene[64]; if (h.valid) snprintf(scene, sizeof scene, "局内 区域%d (%d,%d) Lv%d%s", h.area, h.x, h.y, h.level, h.dead ? "(死亡)" : ""); else snprintf(scene, sizeof scene, "菜单");
        fprintf(fp, "%s\t%.0f\t%d\t%.2f\t%.1f\t%.1f\t%.1f\t%.1f\t%u\t%.2f\t%s\n", ts, (now - tstart) / 1e9, dm_thermal_now(), dm_3d_scale_cur(),
                sum > 0 ? n * 1000.0 / sum : (f - f0) / ((now - t0) / 1e9), n ? c16 * 100.0 / n : 0, n ? c25 * 100.0 / n : 0, n ? c33 * 100.0 / n : 0, disc, f > f0 ? (cpu - cpu0) / 1e6 / (double)(f - f0) : 0, scene);
        fclose(fp);
    }
    LOG("[perf] 温控 %d 3D %.2f 帧率 %.1f 三档 %u/%u/%u 丢弃 %u", dm_thermal_now(), dm_3d_scale_cur(), sum > 0 ? n * 1000.0 / sum : (f - f0) / ((now - t0) / 1e9), c16, c25, c33, disc);   // 同一行进 boot.txt（冒烟可核） | The same line goes into boot.txt (the smoke test can check it)
    c16 = c25 = c33 = disc = 0; sum = 0; t0 = now; f0 = f; cpu0 = cpu;
}
void dm_shot_written(const char *path, int ok) {
    if (g_hold_tick && !strcmp(g_hold_ui, "SHOT") && atomic_load(&g_holding)) {
        static int udc_ab_shots;
        int ab = (getenv("DM_NVN_UDC_AB") && !strcmp(getenv("DM_NVN_UDC_AB"), "1")) || (getenv("DM_NVN_DEFER_AB") && !strcmp(getenv("DM_NVN_DEFER_AB"), "1")) || (getenv("DM_NVN_RULER") && !strcmp(getenv("DM_NVN_RULER"), "1"));   // 同进程 A/B/C：前三张截图都在冻结拍上 | Same-process A/B/C: the first three screenshots are all on frozen ticks
        if (ab && ok && ++udc_ab_shots < 3) return;
        char m[600]; snprintf(m, sizeof m, "截图写盘%s %s", ok ? "完成" : "失败", path); hold_release(m);
    }
}
static void *hold_guard(void *a) {
    (void)a; uint64_t seen = crash_watch_progress_count(), deadline = now_ns() + 10000000000ull;
    for (;;) {
        sleep(1);
        uint64_t count = crash_watch_progress_count();
        if (count != seen) { seen = count; deadline = now_ns() + 10000000000ull; }
        if (now_ns() < deadline) continue;
        if (atomic_exchange(&g_holding, 0)) { hold_unfreeze(); LOG("【红】DM_TICK_HOLD：连续 10 秒无进展，强制解冻；卡在「%s」", !strcmp(g_hold_ui, "SHOT") ? "截图写盘" : g_hold_ui); }
        return NULL;
    }
}
// DM_POSCHK=E:x:y:xf:yf（走位方案）：拍 E 入口（拍号未自增、早于冻结与注入）读玩家表 G+0x2230 唯一玩家的路径 [u+0x38]
// → u16 xf=[+0] x=[+2] yf=[+4] y=[+6]（tick_state_format.md 同口径），与真值 arrive 事件比；不等或读不到即【红】。只核一次
// DM_POSCHK=E:x:y:xf:yf (positioning plan): at the entry of tick E (tick number not yet incremented, before freezing and injection) read the path [u+0x38] of the only player in the player table G+0x2230
// → u16 xf=[+0] x=[+2] yf=[+4] y=[+6] (same convention as tick_state_format.md) and compare with the ground-truth arrive event; if unequal or unreadable it is 【红】 (red). Checked only once
static uint32_t g_pos_tick, g_pos_want[4]; static int g_pos_on, g_pos_bad;
static void shotui_cancel_all(void);
static void poschk(uint64_t G) {
    // 玩家表里除受控玩家外还有占位玩家单位（cvd5 拍 160 真值：id 1 mode 6 在 (5802,4417)，id 2–7 mode 0、坐标 (0,0)）：
    // 取 mode ≠ 0 且 (x, y) ≠ (0, 0) 的唯一一个；不唯一即【红】（arrive 事件不带单位 id）
    // Besides the controlled player, the player table also holds placeholder player units (cvd5 tick 160 ground truth: id 1 mode 6 at (5802,4417), id 2–7 mode 0, coordinates (0,0)):
    // take the only one with mode ≠ 0 and (x, y) ≠ (0, 0); if not unique it is 【红】 (red) (the arrive event carries no unit id)
    int n = 0, hit = 0, mine = 0; uint16_t q[4] = {0}, qm[4] = {0}; uint32_t lid = local_player_id();
    for (int b = 0; b < 128; b++) { uint64_t u = 0; tk_rd(G + 0x2230 + 8 * b, &u, 8);
        for (int k = 0; u && k < 4096; k++) {
            uint32_t mode = 0, id = 0; uint64_t pp = 0; uint16_t c[4] = {0}; n++;
            if (tk_rd(u + 12, &mode, 4) && mode && tk_rd(u + 0x38, &pp, 8) && pp && tk_rd(pp, c, 8) && (c[1] || c[3])) { if (!hit++) memcpy(q, c, 8);
                if (lid != 0xffffffffu && tk_rd(u + 8, &id, 4) && id == lid) { mine = 1; memcpy(qm, c, 8); } }
            if (!tk_rd(u + 0x158, &u, 8)) break; } }
    if (lid != 0xffffffffu) { hit = mine; memcpy(q, qm, 8); }   // 认得出本地玩家单位号：只认它（见 local_player_id） | local player unit id is known: accept only that unit (see local_player_id)
    int ok = hit == 1;
    uint32_t xf = q[0], x = q[1], yf = q[2], y = q[3];
    int same = ok && x == g_pos_want[0] && y == g_pos_want[1] && xf == g_pos_want[2] && yf == g_pos_want[3];
    if (!same) { g_pos_bad = 1; shotui_cancel_all(); }   // 不一致不截图（screenshot_gate.md 方案第 4 条） | On mismatch no screenshot is taken (screenshot_gate.md plan item 4)
    if (!ok) LOG("【红】DM_POSCHK 拍 %u：受控玩家不唯一（玩家表 %d 个单位，在场 %d 个）", g_pos_tick, n, hit);
    else LOG("%sDM_POSCHK 拍 %u 玩家 (%u,%u)/(%u,%u) 真值 (%u,%u)/(%u,%u) %s", same ? "" : "【红】", g_pos_tick, x, y, xf, yf,
             g_pos_want[0], g_pos_want[1], g_pos_want[2], g_pos_want[3], same ? "一致" : "不一致");
}
// ---------- 面板开着时冻结世界（面板开着整个游戏冻结，免得开着面板被打死；药水回复、佣兵、在飞的投射物原样冻住，关面板继续） ----------
// 缺省关：DM_PANEL_FREEZE=1 才按面板冻（dm_cursor.c 每帧读面板树发布 g_dm_frz_want）；测试开关 DM_TEST_FREEZE="<拍>:<拍数>[;…]" 到拍后冻这么多次拍入口。两个都没设就不挂门、行为与没有这段一样。
// 做法：四个入口观测门，全部只换入口参数，不动局内时钟——
//   服务端：拍入口 main+0x5d3bf0 无条件给拍号 +1，这里在入口先减 1 抵掉；世界模拟总入口 main+0x5eb2c0 只读 G+0x170 拍号与 G+0x188 事件队列，把 x0 换成队列为空的替身（门 10），真队列原样留着。
//   拍入口其余部分照走：收包照处理（面板里的买卖 / 穿脱 / 存取照常成交）、给客户端的发包、自动存盘判定（切后台照常存）。拍号停在最后执行过的那一拍；拍入口里有几段按拍号取模的周期任务，拍号是 20 / 12 / 11 / 1500 的倍数时晚一拍再冻。
//   客户端：逐单位的逐拍更新 main+0x4b7230（门 11，替身只带「跳过」标志 [+0x124] 第 23 位）、逐单位的每帧更新 main+0x4b74d0（门 12，替身类型 9）、渲染每帧入口 main+0x2c3c90（门 13：把世界帧时长 [main+0x20cebb8] 写 0、世界累计时间 [main+0x3114208] 写回冻结那一刻，原版暂停在 0x2c1574 做的是同一件事）。
//   渲染侧握手 mtl_world_frozen(1 / 0)（dm/gfx/nvn/nvn_mtl.m）：冻结后第二帧起不画世界，只画界面与末尾合成。
// ---------- Freeze the world while a panel is open (with a panel open the whole game is frozen, so you do not get killed with a panel open; potion recovery, mercenaries and projectiles in flight are frozen as they are and continue when the panel closes) ----------
// Off by default: only DM_PANEL_FREEZE=1 freezes by panel (dm_cursor.c reads the panel tree every frame and publishes g_dm_frz_want); the test switch DM_TEST_FREEZE="<tick>:<tick count>[;...]" freezes that many tick entries once the tick is reached. With neither set no gates are attached and behavior is the same as without this section.
// Approach: four entry observation gates, all of which only swap entry arguments and do not touch the in-run clock --
//   Server: the tick entry main+0x5d3bf0 unconditionally adds 1 to the tick number, so here 1 is subtracted at entry to cancel it; the world simulation main entry main+0x5eb2c0 reads only the tick number at G+0x170 and the event queue at G+0x188, so x0 is swapped for a stand-in whose queue is empty (gate 10), and the real queue is left as is.
//   The rest of the tick entry runs as usual: received packets are processed as usual (buy/sell / equip/unequip / deposit/withdraw inside panels complete as usual), packets are sent to the client, and the autosave decision runs (going to the background saves as usual). The tick number stays at the last executed tick; the tick entry has several periodic tasks taken modulo the tick number, so when the tick number is a multiple of 20 / 12 / 11 / 1500 the freeze starts one tick later.
//   Client: the per-unit per-tick update main+0x4b7230 (gate 11, the stand-in carries only the "skip" flag, bit 23 of [+0x124]), the per-unit per-frame update main+0x4b74d0 (gate 12, stand-in type 9), the render per-frame entry main+0x2c3c90 (gate 13: writes 0 to the world frame duration [main+0x20cebb8] and writes the world accumulated time [main+0x3114208] back to the moment of freezing, the same thing the original game's pause does at 0x2c1574).
//   Render-side handshake mtl_world_frozen(1 / 0) (dm/gfx/nvn/nvn_mtl.m): from the second frame after freezing the world is not drawn, only the UI and the final composite.
_Atomic int g_dm_frozen;
_Atomic int g_dm_frz_want;   // dm_cursor.c 每帧发布：算冻结的面板有没有在屏上 | Published every frame by dm_cursor.c: whether a panel that counts for freezing is on screen
void mtl_world_frozen(int on) __attribute__((weak));
static uint32_t frz_snapshot(uint64_t G, int *nmon, int *php) {   // 冻没冻住的读数：全部怪 / 佣兵的 id、格坐标、模式、生命折成一个校验值，外加玩家生命 | Reading for whether the freeze held: the id, cell coordinates, mode and life of all monsters / mercenaries folded into one checksum, plus the player's life
    module_t *main = main_mod(); int (*get)(uint64_t, int, int) = (int (*)(uint64_t, int, int))(main->base + 0x5b9290); uint32_t h = 2166136261u; int n = 0;
    for (int b = 0; b < 128; b++) { uint64_t u = 0; if (!tk_rd(G + 0x2630 + 8 * b, &u, 8)) continue;
        for (int k = 0; u && k < 4096; k++) { uint32_t v[3] = {0}; uint64_t pp = 0; uint16_t c[4] = {0}; tk_rd(u + 8, &v[0], 4); tk_rd(u + 0xC, &v[1], 4); v[2] = (uint32_t)get(u, 6, 0); if (tk_rd(u + 0x38, &pp, 8) && pp) tk_rd(pp, c, 8);
            for (int i = 0; i < 3; i++) h = (h ^ v[i]) * 16777619u; h = (h ^ c[1]) * 16777619u; h = (h ^ c[3]) * 16777619u; n++; if (!tk_rd(u + 0x158, &u, 8)) break; } }
    *nmon = n; *php = g_player_unit ? get(g_player_unit, 6, 0) >> 8 : -1; return h;
}
static void freeze_world_obs(const uint64_t *rc) { uint64_t *r = (uint64_t *)rc; static uint64_t fq[0x1500 / 8], fg[0x200 / 8]; if (atomic_load(&g_dm_frozen)) { fg[0x170 / 8] = 0; fg[0x188 / 8] = (uint64_t)(uintptr_t)fq; r[0] = (uint64_t)(uintptr_t)fg; } }
static void freeze_client_obs(const uint64_t *rc) { uint64_t *r = (uint64_t *)rc; static uint32_t fu[0x200 / 4]; if (atomic_load(&g_dm_frozen) && r[0]) { fu[0x124 / 4] = 1u << 23; r[0] = (uint64_t)(uintptr_t)fu; } }
static void freeze_anim_obs(const uint64_t *rc) { uint64_t *r = (uint64_t *)rc; static uint32_t fu[0x200 / 4]; if (atomic_load(&g_dm_frozen) && r[0]) { fu[0] = 9; r[0] = (uint64_t)(uintptr_t)fu; } }
static void freeze_render_obs(const uint64_t *rc) { (void)rc; static uint64_t t0; static int was; module_t *m = main_mod(); if (!m) return; uint32_t *dt = (uint32_t *)((uintptr_t)m->base + 0x20cebb8); uint64_t *wt = (uint64_t *)((uintptr_t)m->base + 0x3114208);
    if (atomic_load(&g_dm_frozen)) { if (!was) { was = 1; t0 = *wt; } *dt = 0; *wt = t0; } else was = 0; }
static _Atomic int g_frz_by_panel;   // 当前这次冻结是面板引起的（不是测试时间窗） | The current freeze was caused by a panel (not a test time window)
// 每个出图帧调（dm_cursor.c 面板状态泵）：解冻本来在拍入口做，但拍入口不走的时候轮不到——面板冻着时切到暂停菜单（原版暂停，拍停）、点「保存并退出」、换角色，
// 冻结状态会一直挂到下一个角色进局第 0 拍。所以算冻结的面板已不在屏上、而拍入口连着 10 帧没来解冻，就在这里解。
// Called every presented frame (the dm_cursor.c panel state pump): unfreezing is normally done at the tick entry, but when the tick entry is not running it never gets its turn -- with a panel frozen, switching to the pause menu (the original game's pause, ticks stopped), tapping 「保存并退出」(Save and Exit), and changing characters,
// the frozen state would hang on until tick 0 of the next character's run. So when the panel that counts for freezing is no longer on screen and the tick entry has not come to unfreeze for 10 consecutive frames, unfreeze here.
void dm_freeze_frame(int want) { static int idle;
    if (!atomic_load(&g_dm_frozen) || !atomic_load(&g_frz_by_panel) || want) { idle = 0; return; }
    if (++idle < 10) return; idle = 0; atomic_store(&g_dm_frozen, 0); atomic_store(&g_frz_by_panel, 0); if (mtl_world_frozen) mtl_world_frozen(0);
    LOG("面板冻结：解冻（面板已不在屏上而拍入口没在走：原版暂停 / 退出 / 换角色）"); }
static int g_frz_on;   // 四个冻结入口门已挂上（freeze_attach） | The four freeze entry gates are attached (freeze_attach)
static int freeze_attach(void) {   // 挂冻结用的四个入口门（产物里有门 10–13 才行），挂过就不再挂。返回 1 = 能冻 | Attach the four entry gates used for freezing (needs gates 10–13 in the product); attached only once. Returns 1 = can freeze
    static int tried; if (tried) return g_frz_on; tried = 1;
    g_frz_on = gate_obs_present(OBS_FREEZE_WORLD) && gate_obs_present(OBS_FREEZE_CLIENT) && gate_obs_present(OBS_FREEZE_ANIM) && gate_obs_present(OBS_FREEZE_RENDER);
    if (g_frz_on) { gate_obs_set(OBS_FREEZE_WORLD, freeze_world_obs); gate_obs_set(OBS_FREEZE_CLIENT, freeze_client_obs); gate_obs_set(OBS_FREEZE_ANIM, freeze_anim_obs); gate_obs_set(OBS_FREEZE_RENDER, freeze_render_obs); }
    return g_frz_on;
}
// 暂停菜单开着进后台时存一次盘：原版单机暂停时游戏不走拍，前端请的那次存盘要等下一拍，进后台 2.5 秒等不到（10142104 日志 4 次），
// 上次自动存盘到开暂停之间的进度在后台被系统回收时会丢。做法：先冻住世界（同面板冻结：世界 / 客户端 / 动画 / 渲染四个入口换成空跑，拍号不前进——怪不动、时间不走），
// 再临时放开原版暂停的两个界面状态字节让拍走起来、执行那次存盘；等到提交（前端 background_wait_save）后写回两个字节、解冻。前端在主线程调
// Save once when going to the background with the pause menu open: in single-player pause the original game does not tick, so the save the front end asks for waits for the next tick and the 2.5 s background wait never sees it (seen 4 times in a player's log);
// progress between the last autosave and opening the pause menu is lost if the system reclaims the app in the background. Method: first freeze the world (same as panel freezing: the world / client / animation / render entry points become no-ops and the tick number does not advance, so monsters do not move and time does not pass),
// then temporarily release the two UI state bytes of the original pause so ticks run and perform the save; after the commit (front end background_wait_save) write the two bytes back and unfreeze. Called by the front end on the main thread
static _Atomic int g_bgsave; static uint8_t g_bg_pause[2];
static int save_unpause_begin(void) {   // 1 = 冻住世界并放开了暂停；0 = 游戏没停着（不用做）；-1 = 产物没有冻结门，做不了 | 1 = froze the world and released the pause; 0 = the game is not stopped (nothing to do); -1 = the product has no freeze gates, cannot do it
    uint8_t *b = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base;
    if (!b || !in_game()) return 0; volatile uint8_t *f = b + 0x2d5ff51, *g = b + 0x2d5ff67; if (!*f && !*g) return 0;
    if (!freeze_attach()) { LOG("【红】停着存盘：游戏停着，但产物里没有冻结门 10–13，不放开暂停（这次存不了）"); return -1; }
    atomic_store(&g_frz_by_panel, 0); atomic_store(&g_bgsave, 1); atomic_store(&g_dm_frozen, 1);   // 先冻、后放开：放开后的第一拍世界就已经是空跑 | Freeze first, then release: the world is already idle on the first tick after release
    int nm = 0, hp = 0; uint32_t h = g_lastG ? frz_snapshot(g_lastG, &nm, &hp) : 0;
    g_bg_pause[0] = *f; g_bg_pause[1] = *g; *f = 0; *g = 0;
    LOG("停着存盘：游戏停着 → 冻住世界、临时放开原版暂停（怪 / 佣兵 %d 个，快照 %08x，玩家生命 %d）", nm, h, hp);
    return 1;
}
static void save_unpause_end(void) {
    if (!atomic_load(&g_bgsave)) return;
    uint8_t *b = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base;
    if (b) { *(volatile uint8_t *)(b + 0x2d5ff51) = g_bg_pause[0]; *(volatile uint8_t *)(b + 0x2d5ff67) = g_bg_pause[1]; }   // 先恢复暂停、后解冻：解冻后的拍不再走 | Restore the pause first, then unfreeze: ticks after unfreezing no longer advance
    int nm = 0, hp = 0; uint32_t h = g_lastG ? frz_snapshot(g_lastG, &nm, &hp) : 0;
    atomic_store(&g_bgsave, 0); atomic_store(&g_dm_frozen, 0);
    LOG("停着存盘：恢复原版暂停、解冻（怪 / 佣兵 %d 个，快照 %08x，玩家生命 %d）", nm, h, hp);
}
// 前端要进程马上可能没了之前（iOS 进后台、Mac 退出）请游戏存一次盘并等提交：游戏停着（暂停菜单 / 大秘境面板开着）就先冻住世界、
// 临时放开原版暂停让拍走起来执行这次存盘，等完写回暂停、解冻——玩家回来时世界与暂停都和离开前一样。等到「出现过存档提交、之后 0.2 秒没有新的提交」
// （角色与共享箱是两个容器，可能分两次），最多 max_s 秒；还没进局（读取画面）就每 20 ms 再试，进了局再请。返回 1 = 请到了存盘。
// *commits = 期间存档提交次数，*froze = 是否冻结放开过暂停，*ms = 用时。在前端主线程调（游戏拍在客体线程照跑）
// Before the process may go away at any moment (iOS going to the background, quitting on Mac), the front end asks the game to save once and waits for the commit. If the game is stopped (pause menu / Greater Rift panel open), it first freezes the world,
// temporarily releases the original pause so ticks run and perform this save, and after waiting writes the pause back and unfreezes, so when the player returns both the world and the pause are as they left them. It waits until "a save commit has happened and no new commit for 0.2 s"
// (character and shared stash are two containers and may commit separately), at most max_s seconds; before entering a game (loading screen) it retries every 20 ms and asks once in game. Returns 1 = the save was requested.
// *commits = number of save commits in the meantime, *froze = whether it froze and released the pause, *ms = time taken. Called on the front end's main thread (game ticks keep running on the guest thread)
uint64_t dm_fs_commit_count(void);
int dm_save_wait(double max_s, uint64_t *commits, int *froze, double *ms) {
    uint64_t c0 = dm_fs_commit_count(), t0 = now_ns(), last = c0, last_t = 0, lim = (uint64_t)(max_s * 1e9); int asked = 0, bg = 0;
    for (;;) {
        if (!bg && in_game()) bg = save_unpause_begin();   // 先冻、放开暂停，再请存盘 | Freeze first, release the pause, then ask for the save
        if (!asked) asked = dm_request_save();
        usleep(20000); uint64_t c = dm_fs_commit_count(), t = now_ns();
        if (c != last) { last = c; last_t = t; }
        if ((asked && last_t && t - last_t >= 200000000ull) || t - t0 >= lim) break;
    }
    if (bg > 0) save_unpause_end();
    *commits = last - c0; *froze = bg > 0; *ms = (now_ns() - t0) / 1e6;
    return asked;
}
static void freeze_tick(uint64_t G) {   // 拍入口：定这一拍冻不冻，冻就把拍号先减 1 | Tick entry: decide whether this tick is frozen; if so, decrement the tick number by 1 first
    static struct { uint32_t at, n; int st; } w[8]; static int nw = -1, panel, cur = -1; static uint32_t left, calls;
    if (nw < 0) { nw = 0; const char *e = getenv("DM_TEST_FREEZE"); char buf[160]; if (e) { strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ";", &sv); t && nw < 8; t = strtok_r(NULL, ";", &sv)) if (sscanf(t, "%u:%u", &w[nw].at, &w[nw].n) == 2) nw++; }
        e = getenv("DM_PANEL_FREEZE"); panel = e && e[0] != '0';
        if (nw || panel) { if (freeze_attach()) LOG("面板冻结：四个入口门已挂（按面板 %d，时间窗 %d 个）", panel, nw); else LOG("【红】面板冻结：产物里没有观测门 10–13，不生效"); } }
    if (!g_frz_on) return;
    uint32_t t = 0; if (!tk_rd(G + 0x170, &t, 4)) return; int frozen = atomic_load(&g_dm_frozen);
    if (cur < 0 && !frozen) for (int i = 0; i < nw; i++) if (!w[i].st && t >= w[i].at) { cur = i; w[i].st = 1; left = w[i].n; break; }
    if (cur >= 0 && frozen && left && !--left) { w[cur].st = 2; cur = -1; }
    int want = cur >= 0 || (panel && atomic_load(&g_dm_frz_want)) || atomic_load(&g_bgsave);
    if (want && !frozen) { if (t % 20 == 0 || t % 12 == 0 || t % 11 == 0 || t % 1500 == 0) return;   // 晚一拍 | One tick later
        int nm = 0, hp = 0; uint32_t h = frz_snapshot(G, &nm, &hp); atomic_store(&g_frz_by_panel, cur < 0); atomic_store(&g_dm_frozen, 1); frozen = 1; calls = 0; if (mtl_world_frozen) mtl_world_frozen(1); LOG("面板冻结：拍 %u 冻结（%s）；怪 %d 只 校验 %08x 玩家生命 %d", t, cur >= 0 ? "时间窗" : "面板开着", nm, h, hp); }
    else if (!want && frozen) { int nm = 0, hp = 0; uint32_t h = frz_snapshot(G, &nm, &hp); atomic_store(&g_dm_frozen, 0); frozen = 0; if (mtl_world_frozen) mtl_world_frozen(0); LOG("面板冻结：拍 %u 解冻（冻了 %u 次拍入口）；怪 %d 只 校验 %08x 玩家生命 %d", t, calls, nm, h, hp); }
    if (frozen) { calls++; uint32_t t1 = t - 1; memcpy((void *)(uintptr_t)(G + 0x170), &t1, 4); }
}
static void tick_gate_product(const uint64_t *regs) { g_lastG = regs[0]; players_apply(); hud_tick(regs[0]); freeze_tick(regs[0]); }   // 产品缺省的拍入口：只落地难度档、发布 HUD 数字 | The product default tick entry: only applies the difficulty level and publishes the HUD numbers
static void tick_gate(const uint64_t *regs) {         // 拍入口：先注入本拍命令包，再取状态哈希 | Tick entry: first inject this tick's command packets, then take the state hash
    {   // 拍入口只在一条客体线程上跑：state_dump / ticks_hash 的 static 缓冲靠这一点不竞争。换线程即中止，别让输出悄悄写乱 | The tick entry runs on only one guest thread: the static buffers of state_dump / ticks_hash rely on this to be race-free. Abort if the thread changes, do not let the output silently get garbled
        static _Atomic uintptr_t owner; uintptr_t me = (uintptr_t)pthread_self(), exp = 0;
        if (!atomic_compare_exchange_strong(&owner, &exp, me) && exp != me) { LOG("【红】拍入口换了线程（首个 %#lx，本次 %#lx）：static 缓冲会竞争", (unsigned long)exp, (unsigned long)me); abort(); }
    }
    g_lastG = regs[0];
    players_apply();
    hud_tick(regs[0]);
    crash_watch_progress(cur_tick(), "拍入口");
    if (getenv("DM_TEST_LEAK")) {
        static void **leaks; static size_t nleaks;
        void *p = malloc(64 * 1024);
        if (!p) die("DM_TEST_LEAK：64 KiB 分配失败");
        memset(p, 0xa5, 64 * 1024);
        void **next = realloc(leaks, (nleaks + 1) * sizeof(*leaks));
        if (!next) die("DM_TEST_LEAK：泄漏表扩容失败");
        leaks = next; leaks[nleaks++] = p;
    }
    if (g_pos_on && cur_tick() == g_pos_tick) { g_pos_on = 0; poschk(regs[0]); }
    if (dm_mtl_l3_tick) dm_mtl_l3_tick(cur_tick());
    { static uint32_t last; uint32_t t = cur_tick(); if (t && t % 250 == 0 && t != last) { last = t; l3mem_request(t); res_report("每250拍"); } }   // 资源计数按游戏拍取样，L3MEM 在宿主低优先级线程取样 | Resource counts are sampled by game tick; L3MEM is sampled on a low-priority host thread
    {   // DM_TEST_LAG=拍[:毫秒]（必现注入点，缺省 300 ms）：在该拍入口睡一下造补拍积压，冻结 / 补拍改动的确定性回归用 | DM_TEST_LAG=tick[:ms] (a guaranteed-reproduction injection point, default 300 ms): sleeps at that tick's entry to build up a backlog of catch-up ticks, for deterministic regression of freeze / catch-up tick changes
        static uint32_t lt, ms; static int st;
        if (!st) { const char *e = getenv("DM_TEST_LAG"); st = e ? 1 : 2; if (e) { lt = (uint32_t)strtoul(e, 0, 0); const char *c = strchr(e, ':'); ms = c ? (uint32_t)strtoul(c + 1, 0, 0) : 300; } }
        if (st == 1 && cur_tick() == lt) { st = 2; usleep(ms * 1000); LOG("DM_TEST_LAG：拍 %u 入口睡 %u ms", lt, ms); } }
    hold_track_origin();   // 冻结 / 解冻换算时限用；读取画面冻结（dm_hold_for_loadscreen）不依赖 DM_TICK_HOLD，所以每拍都跟 | Used for converting time limits on freeze / unfreeze; the loading screen freeze (dm_hold_for_loadscreen) does not depend on DM_TICK_HOLD, so it is tracked every tick
    // 按拍号截图的局（DM_TICK_HOLD=E:SHOT）：进局第 5 拍读取画面还没撤就先冻结到它撤下（dm_hold_for_loadscreen），之后的拍号时间线都落在客户端加载完之后。
    // 负载高时客户端加载慢、服务端拍照走：出过一次 NPC 页读取画面拍 85 才撤，拍 84 的 A 在撤屏当拍按下、游戏不认，对话面板没开（按键到点才冻结救不了这一下）
    // Runs that take screenshots by tick number (DM_TICK_HOLD=E:SHOT): if at tick 5 of the run the loading screen has not been taken down yet, freeze until it is taken down (dm_hold_for_loadscreen), so that the tick timeline afterwards all falls after the client has finished loading.
    // Under high load the client loads slowly while server ticks keep going: it happened once that the loading screen of an NPC page was only taken down at tick 85, the A of tick 84 was pressed on the very tick the screen came down, the game did not accept it, and the dialogue panel did not open (freezing only when the key is due cannot save this one)
    { int dm_loadscreen_up(void); if (g_hold_tick && !strcmp(g_hold_ui, "SHOT") && cur_tick() == 5 && dm_loadscreen_up()) dm_hold_for_loadscreen(); }
    if (g_hold_tick && g_pos_bad && !strcmp(g_hold_ui, "SHOT") && cur_tick() == g_hold_tick) { static int o2; if (!o2++) LOG("DM_TICK_HOLD：坐标核对不一致，不截图也不冻结"); }
    else if (g_hold_tick) { uint32_t t = cur_tick(); if (t == g_hold_tick) { static int once; if (!once++) {
        hold_freeze();
        g_hold_t0 = now_ns(); atomic_store(&g_holding, 1); LOG("DM_TICK_HOLD：拍 %u 冻结客体时钟，等「%s」", t, g_hold_ui);
        pthread_t th; if (!pthread_create(&th, NULL, hold_guard, NULL)) pthread_detach(th); } } }
    if (g_rp) c2s_inject(regs[0]);
    if (g_spfp) state_dump(regs);
    if (g_tkfp) ticks_hash(regs);
    { void dm_bright_server_tick(uint64_t G); dm_bright_server_tick(regs[0]); }   // 3D 画面加亮的测试开关 DM_TEST_DAYTIME（dm_bright.c） | The test switch DM_TEST_DAYTIME for 3D picture brightening (dm_bright.c)
    freeze_tick(regs[0]);   // 面板冻结：放在最后，前面各段读到的还是没减过的拍号 | Panel freeze: placed last, so the sections before it still read the tick number before the decrement
}
// ---------- DM_SOAK=<路径>[,间隔秒（缺省 60）]：浸泡读数，宿主线程定时写一行 TSV ----------
// 列：秒 拍号（不在局内 -1） 句柄 存活线程 线程对象 malloc在用MiB phys_footprintMiB（拍号需拍入口观测门已挂：DM_TICKS / DM_C2S_* 任一）
// ---------- DM_SOAK=<path>[,interval seconds (default 60)]: soak readings, a host thread periodically writes one TSV line ----------
// Columns: seconds, tick number (-1 when not in a run), handles, live threads, thread objects, malloc in-use MiB, phys_footprint MiB (the tick number requires the tick entry observation gate to be attached: any of DM_TICKS / DM_C2S_*)
static void *soak_main(void *arg) {
    FILE *f = arg; int iv = 60; const char *e = strchr(getenv("DM_SOAK"), ','); if (e) iv = atoi(e + 1);
    fprintf(f, "秒\t拍号\t句柄\t存活线程\t线程对象\tmalloc_MiB\tfootprint_MiB\n"); fflush(f);
    for (;;) {
        sleep(iv);
        void thr_counts(int *, int *, int *); int h, l, o; thr_counts(&h, &l, &o);
        malloc_statistics_t ms; malloc_zone_statistics(NULL, &ms);
        task_vm_info_data_t vi; mach_msg_type_number_t c = TASK_VM_INFO_COUNT; uint64_t fp = 0;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c) == KERN_SUCCESS) fp = vi.phys_footprint;
        long long tk = in_game() ? (long long)cur_tick() : -1;        // 不在局内（未进局 / 已离局）写 -1 | Writes -1 when not in a run (not yet entered / already left)
        fprintf(f, "%.0f\t%lld\t%d\t%d\t%d\t%.1f\t%.1f\n", (now_ns() - g_t0_ns) / 1e9, tk, h, l, o, ms.size_in_use / 1048576.0, fp / 1048576.0);
        fflush(f);
    }
    return NULL;
}
static void soak_install(void) {
    const char *e = getenv("DM_SOAK"); if (!e) return;
    char p[1024]; snprintf(p, sizeof p, "%.*s", (int)strcspn(e, ","), e);
    FILE *f = fopen(p, "w"); if (!f) die("DM_SOAK 打不开 %s", p);
    pthread_t t; if (pthread_create(&t, NULL, soak_main, f)) die("DM_SOAK 起线程失败"); pthread_detach(t);
    LOG("DM_SOAK → %s", p);
}
// ---------- DM_NVN_SHOT_UI=<触发>+<帧偏移>:<路径>[,…]：UI 消息命中后第 N 帧截图（画面闸门用）----------
// 触发 = 消息串「类别:动作:参数」的子串（与 DM_KEYS 同规则，如 OpenPanel:CharacterCreatePanel、UnloadPanel:LoadScreenPanel）；每条只触发一次
// ---------- DM_NVN_SHOT_UI=<trigger>+<frame offset>:<path>[,...]: take a screenshot on the Nth frame after a UI message hits (used by the screenshot gate) ----------
// Trigger = a substring of the message string "category:action:argument" (same rule as DM_KEYS, e.g. OpenPanel:CharacterCreatePanel, UnloadPanel:LoadScreenPanel); each one triggers only once
#define SHOTUI_MAX 8
static struct { char trig[96], path[512]; long off; int done; } g_shotui[SHOTUI_MAX]; static int g_nshotui;
static pthread_mutex_t g_shotgate_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_loadscreen_open, g_loadscreen_unload_pending;   // g_loadscreen_seen 的定义在上面（长帧日志也读它） | g_loadscreen_seen is defined above (the long-frame log reads it too)
static unsigned g_loadscreen_generation;
static int g_loadscreen_delay_ms;

// 读取画面已撤：见过读取画面、且最近一次已撤（含测试推迟）。宿主内唯一判定处
// Loading screen taken down: a loading screen has been seen, and the most recent one has been taken down (including the test delay). The only place in the host that decides this
int dm_loadscreen_up(void) { pthread_mutex_lock(&g_shotgate_mu); int u = g_loadscreen_seen && g_loadscreen_open; pthread_mutex_unlock(&g_shotgate_mu); return u; }   // 读取画面此刻开着（没见过 = 不算开） | The loading screen is open right now (never seen = not counted as open)
int dm_loadscreen_down(void) { pthread_mutex_lock(&g_shotgate_mu); int d = g_loadscreen_seen && !g_loadscreen_open; pthread_mutex_unlock(&g_shotgate_mu); return d; }
static void loadscreen_close_now(unsigned generation) {
    pthread_mutex_lock(&g_shotgate_mu);
    if (generation == g_loadscreen_generation && g_loadscreen_unload_pending) {
        g_loadscreen_unload_pending = 0; g_loadscreen_open = 0;
        LOG("读取画面已撤"); prof_load_end();
    }
    pthread_mutex_unlock(&g_shotgate_mu);
    if (!dm_loadscreen_up()) { lhold_release(); atomic_store(&g_lhold_giveup, 0); }
}
static void *loadscreen_delayed_close(void *arg) {
    unsigned generation = *(unsigned *)arg; free(arg);
    usleep((useconds_t)g_loadscreen_delay_ms * 1000);
    loadscreen_close_now(generation);
    return NULL;
}
static void (*g_sharelog_cb)(void);   // 外壳登记的「分享诊断日志」回调（iOS：dm/ios/Sources/dm_sharelog.m 打包日志、弹系统分享面板）；游戏主线程调 | The 「分享诊断日志」(Share Diagnostics) callback registered by the shell (iOS: dm/ios/Sources/dm_sharelog.m packs the logs and presents the system share sheet); called on the game main thread
void dm_sharelog_init(void (*cb)(void)) { g_sharelog_cb = cb; }
static void loadscreen_on_uimsg(const char *cat, const char *act, const char *arg) {
    // 建角起名面板开着没有（hle_swkbd.c / 前端输入框据此按游戏的角色名规则拦字符：游戏自己只收 2–15 个英文字母，见 hle_swkbd.c 头注）
    // 我们的底栏直接开任务面板、不经原版的任务提示按钮，提示字节没人清：任务面板一打开就清（= 已读）。本回调在客户端主线程，与游戏自己读写这个字节同线程
    // Whether the character creation naming panel is open (hle_swkbd.c / the front-end input box use it to block characters according to the game's character name rules: the game itself accepts only 2–15 English letters, see the header comment of hle_swkbd.c)
    // Our bottom bar opens the quest panel directly, without going through the original game's quest hint button, so nobody clears the hint byte: it is cleared as soon as the quest panel opens (= read). This callback is on the client main thread, the same thread on which the game itself reads and writes this byte
    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "QuestLogPanelExpansion") && (!strcmp(act, "OpenPanel") || !strcmp(act, "OpenExclusivePanel"))) { module_t *qm = main_mod(); if (qm) *(volatile uint8_t *)(qm->base + 0x2d5ff58) = 0; }
    { void dm_textinput_on_uimsg(const char *, const char *, const char *); dm_textinput_on_uimsg(cat, act, arg); }
    { void dm_update_on_uimsg(const char *, const char *, const char *); dm_update_on_uimsg(cat, act, arg); }
    { void dm_kills_on_uimsg(const char *, const char *, const char *); dm_kills_on_uimsg(cat, act, arg); } { void dm_rift_ui_on_uimsg(const char *, const char *, const char *); dm_rift_ui_on_uimsg(cat, act, arg); }   // 编年史「首领」页签（dm_kills.c） | The chronicle's 「首领」(Bosses) tab (dm_kills.c)
    { void dm_vault_on_uimsg(const char *, const char *, const char *); dm_vault_on_uimsg(cat, act, arg); }
    { void dm_padmode_on_uimsg(const char *, const char *, const char *); dm_padmode_on_uimsg(cat, act, arg); }   // 手柄版暂停菜单「游戏难度」行（dm_padmode.c） | "Game Difficulty" row of the controller pause menu (dm_padmode.c)
    { void dm_pouch_on_uimsg(const char *, const char *, const char *); dm_pouch_on_uimsg(cat, act, arg); }   // 百宝囊框 / 存放按钮（dm_pouch.c）   // 储藏箱第六页「编年史」（dm_vault_ui.c） | Pouch frame / deposit button (dm_pouch.c)   // stash page six 「编年史」(Chronicle) (dm_vault_ui.c)
    { void dm_shoprefresh_on_uimsg(const char *, const char *, const char *, uint8_t *); module_t *sm = main_mod(); dm_shoprefresh_on_uimsg(cat, act, arg, sm ? (uint8_t *)sm->base : NULL); }   // 商人交易页「刷新货架」按钮（dm_shoprefresh.c） | The 「刷新货架」(Refresh Stock) button on the vendor trade page (dm_shoprefresh.c)
    { void dm_runeword_on_uimsg(const char *, const char *, const char *); dm_runeword_on_uimsg(cat, act, arg); }   // 符文之语助手（dm_runeword.c） | Runeword helper (dm_runeword.c)
    { void dm_beltauto_on_uimsg(const char *, const char *, const char *); dm_beltauto_on_uimsg(cat, act, arg); }   // 开背包自动补腰带（dm_beltauto.c） | Auto-refill the belt when the inventory opens (dm_beltauto.c)
    { extern void (*g_dm_charsel_hook)(void); if (g_dm_charsel_hook && !strcmp(cat, "FrontEndNavigation") && !strcmp(act, "ToFrontEnd")) g_dm_charsel_hook(); }   // 退出这一局、回到前端（主菜单 / 选角页）：iOS 把这一局攒下的存档传 iCloud（cloud_ios.m）   // 构建号 + 新版本提示（dm_update.c） | Exiting this run and returning to the front end (main menu / character select page): iOS uploads the saves accumulated during this run to iCloud (cloud_ios.m)   // build number + new version notice (dm_update.c)
    if (!strcmp(cat, "PanelManager") && !strcmp(act, "ClosePanel") && !strcmp(arg, "TouchShareLog")) {   // 暂停菜单「分享诊断日志」（按钮发这条空操作，布局见 touch_layout.py _pause_no_help） | Pause menu 「分享诊断日志」(Share Diagnostics) (the button sends this no-op; for the layout see touch_layout.py _pause_no_help)
        LOG("诊断日志：点了「分享诊断日志」%s", g_sharelog_cb ? "" : "（外壳没登记，不弹）"); if (g_sharelog_cb) g_sharelog_cb(); }
    {   // DM_TEST_FRZ=<开消息>[><关消息>]（测试用）：界面消息子串命中就置 / 清图形后端的「世界已冻结」状态位（nvn_mtl.m frz_op）——没有真正的冻结握手时用它顶替 | DM_TEST_FRZ=<open message>[><close message>] (for testing): when a UI message substring hits, set / clear the graphics backend's "world frozen" status bit (nvn_mtl.m frz_op) -- used as a stand-in when there is no real freeze handshake
        static const char *on, *off; static int rd; void mtl_world_frozen(int);
        if (!rd) { rd = 1; const char *e = getenv("DM_TEST_FRZ"); if (e && *e) { char *d = strdup(e), *g = strchr(d, '>'); if (g) { *g = 0; off = g + 1; } on = d; } }
        if (on) { char m[256]; snprintf(m, sizeof m, "%s:%s:%s", cat, act, arg);
            if (strstr(m, on)) { LOG("DM_TEST_FRZ：「%s」命中 → 世界冻结状态位 开", m); mtl_world_frozen(1); }
            else if (off && strstr(m, off)) { LOG("DM_TEST_FRZ：「%s」命中 → 世界冻结状态位 关", m); mtl_world_frozen(0); } }
    }
    if (!strcmp(cat, "PanelManager") && !strcmp(arg, "CharacterCreateNamePanel")) { extern int g_swkbd_name_panel; g_swkbd_name_panel = !strcmp(act, "OpenPanel") || !strcmp(act, "OpenExclusivePanel"); }
    { void dm_ui_on_uimsg(const char *, const char *, const char *); dm_ui_on_uimsg(cat, act, arg); }   // 前端路径理顺（dm_cursor.c）：借这个恒订阅的回调 | Front-end path straightening (dm_cursor.c): borrows this always-subscribed callback
    if (strcmp(arg, "LoadScreenPanel")) return;
    pthread_mutex_lock(&g_shotgate_mu);
    if (!strcmp(act, "OpenPanel") || !strcmp(act, "OpenExclusivePanel")) {
        g_loadscreen_seen = 1; g_loadscreen_open = 1; g_loadscreen_unload_pending = 0; g_loadscreen_generation++; prof_load_begin();
        pthread_mutex_unlock(&g_shotgate_mu); return;
    }
    if (strcmp(act, "UnloadPanel") && strcmp(act, "ClosePanel")) { pthread_mutex_unlock(&g_shotgate_mu); return; }
    g_loadscreen_seen = 1; g_loadscreen_generation++;
    if (!g_loadscreen_delay_ms) {
        g_loadscreen_open = 0; g_loadscreen_unload_pending = 0;
        pthread_mutex_unlock(&g_shotgate_mu); LOG("读取画面已撤"); prof_load_end(); lhold_release(); return;
    }
    g_loadscreen_unload_pending = 1;
    unsigned *generation = malloc(sizeof *generation); *generation = g_loadscreen_generation;
    pthread_mutex_unlock(&g_shotgate_mu);
    LOG("DM_TEST_LOADSCREEN_DELAY_MS：读取画面撤下延迟 %d ms", g_loadscreen_delay_ms);
    pthread_t th; if (!pthread_create(&th, NULL, loadscreen_delayed_close, generation)) pthread_detach(th); else free(generation);
}
// 截图前置的页面分类只在这里判（闸门不再按触发串猜）：不需要等时当场写一行显式结论，需要等时由 shotui_wait_loadscreen 写「已撤」或「未撤」
// The page classification for the screenshot precondition is decided only here (the gate check no longer guesses from the trigger string): when no wait is needed an explicit verdict line is written on the spot, and when a wait is needed shotui_wait_loadscreen writes 「已撤」(taken down) or 「未撤」(not taken down)
static int shotui_wait_needed(const char *trigger) {
    int seen;
    pthread_mutex_lock(&g_shotgate_mu); seen = g_loadscreen_seen; pthread_mutex_unlock(&g_shotgate_mu);
    if (strstr(trigger, "LoadScreenPanel")) { LOG("SHOT 前置：不适用（截读取画面本身）"); return 0; }
    if (strstr(trigger, "MainMenuButtonRibbon")) { LOG("SHOT 前置：不适用（主菜单页）"); return 0; }
    if (!seen) { LOG("SHOT 前置：本局无读取画面"); return 0; }
    return 1;
}
struct shotui_wait_arg { int index; char label[96]; };
static void *shotui_wait_loadscreen(void *arg) {
    struct shotui_wait_arg *a = arg; uint64_t start = now_ns();
    uint64_t not_before = start + (uint64_t)g_loadscreen_delay_ms * 1000000ull;
    for (;;) {
        pthread_mutex_lock(&g_shotgate_mu); int closed = !g_loadscreen_open && !g_loadscreen_unload_pending; pthread_mutex_unlock(&g_shotgate_mu);
        if (closed && now_ns() >= not_before) {
            LOG("SHOT 前置：读取画面已撤 拍 %s 等待 %llu ms", a->label, (unsigned long long)((now_ns() - start) / 1000000));
            { module_t *mm = main_mod(); if (mm) LOG("SHOT 时原版手柄光标 (%d,%d)（0,0 = 该界面没有自由光标，走焦点导航）", *(int32_t *)(mm->base + 0x2d128f8), *(int32_t *)(mm->base + 0x2d128fc)); }
            dm_gfx_shot_at(g_shotui[a->index].off, g_shotui[a->index].path); free(a); return NULL;
        }
        if (now_ns() - start >= 15ull * 1000000000ull) { LOG("读取画面未撤（等待 15 秒）"); free(a); return NULL; }
        usleep(10000);
    }
}
static void shotui_on_uimsg(const char *cat, const char *act, const char *arg) {
    char m[256]; snprintf(m, sizeof m, "%s:%s:%s", cat, act, arg);
    for (int i = 0; i < g_nshotui; i++) if (!g_shotui[i].done && strstr(m, g_shotui[i].trig)) {
        g_shotui[i].done = 1; LOG("DM_NVN_SHOT_UI：「%s」命中 → 第 +%ld 帧截图 %s", m, g_shotui[i].off, g_shotui[i].path);
        if (!shotui_wait_needed(g_shotui[i].trig)) dm_gfx_shot_at(g_shotui[i].off, g_shotui[i].path);
        else {
            struct shotui_wait_arg *w = calloc(1, sizeof *w); w->index = i; strlcpy(w->label, g_shotui[i].trig, sizeof w->label);
            pthread_t th; if (!pthread_create(&th, NULL, shotui_wait_loadscreen, w)) pthread_detach(th); else free(w);
        }
    }
}
static void shotui_cancel_all(void) {
    for (int i = 0; i < g_nshotui; i++) if (!g_shotui[i].done) { g_shotui[i].done = 1; LOG("DM_NVN_SHOT_UI：坐标核对不一致，撤掉截图 %s", g_shotui[i].path); }
}
static void shotui_install(void) {
    // 读取画面状态始终跟踪（截图前置与触点起局共用这一个来源，dm_loadscreen_down）；测试开关 DM_TEST_LOADSCREEN_DELAY_MS 推迟它的「已撤」
    // The loading screen state is always tracked (the screenshot precondition and the touch-point run start share this single source, dm_loadscreen_down); the test switch DM_TEST_LOADSCREEN_DELAY_MS delays its 「已撤」(taken down)
    void uimsg_subscribe(void (*)(const char *, const char *, const char *));
    const char *delay = getenv("DM_TEST_LOADSCREEN_DELAY_MS"); g_loadscreen_delay_ms = delay ? atoi(delay) : 0;
    uimsg_subscribe(loadscreen_on_uimsg);
    { extern uint32_t (*dm_thread_tick_fn)(void); dm_thread_tick_fn = dm_cur_tick; }   // 线程建 / 退记拍号（hle_thread.c 现场导出用） | Thread creation / exit records the tick number (used by the hle_thread.c state dump)
    { extern int (*g_nvn_loadscreen_up)(void); g_nvn_loadscreen_up = dm_loadscreen_up; }   // 出图处「进局读取画面等管线预热」用 | Used at present for "the run-entry loading screen waits for pipeline warm-up"
    const char *e = getenv("DM_NVN_SHOT_UI"); if (!e) return;
    char buf[4096]; strlcpy(buf, e, sizeof buf);
    for (char *sv, *tok = strtok_r(buf, ",", &sv); tok; tok = strtok_r(NULL, ",", &sv)) {
        char *plus = strchr(tok, '+'), *colon = plus ? strchr(plus, ':') : NULL;
        if (!plus || !colon || plus == tok || !colon[1]) die("DM_NVN_SHOT_UI 项「%s」格式：<触发>+<帧偏移>:<路径>", tok);
        if (g_nshotui == SHOTUI_MAX) die("DM_NVN_SHOT_UI 超过 %d 项", SHOTUI_MAX);
        typeof(g_shotui[0]) *x = &g_shotui[g_nshotui++];
        snprintf(x->trig, sizeof x->trig, "%.*s", (int)(plus - tok), tok); x->off = atol(plus + 1); strlcpy(x->path, colon + 1, sizeof x->path);
    }
    uimsg_subscribe(shotui_on_uimsg);
    LOG("DM_NVN_SHOT_UI：%d 项", g_nshotui);
}
// 资源计数的「读取画面撤下」触发：与每 250 拍取样同条件（挂了拍观测门才订阅，产品无回放时不加 UI 消息回调）
// The "loading screen taken down" trigger for resource counting: same condition as the every-250-ticks sampling (subscribed only when the tick observation gate is attached; the product without replay adds no UI message callback)
void dm_note_loadscreen_unload(void);   // hle_oe.c：DM_OE_SCRIPT 的 @ingame 触发（按事件而非墙钟，负载下不错位） | hle_oe.c: the @ingame trigger of DM_OE_SCRIPT (by event rather than wall clock, so it does not drift under load)
static void res_on_uimsg(const char *cat, const char *act, const char *arg) { (void)cat; if (!strcmp(act, "UnloadPanel") && !strcmp(arg, "LoadScreenPanel")) { dm_note_loadscreen_unload(); l3mem_request(in_game() ? cur_tick() : 0); res_report("读取画面已撤"); } }
void ticks_install(void) {
    const char *e = getenv("DM_TICKS"), *rp = getenv("DM_C2S_REPLAY"), *lg = getenv("DM_C2S_LOG"), *sp = getenv("DM_STATE");
    const char *pn = getenv("DM_PLAYERS");
    if (pn && *pn) { dm_hud_request_players(atoi(pn)); LOG("DM_PLAYERS：起局请求档位 %d", dm_hud_requested_players()); }
    if ((e || rp || lg) && gate_obs_present(OBS_UIMSG)) { void uimsg_subscribe(void (*)(const char *, const char *, const char *)); uimsg_subscribe(res_on_uimsg); }
    soak_install();
    shotui_install();
    if (lg) {
        if (!gate_obs_present(OBS_C2S)) die("DM_C2S_LOG：产物没有命令分派观测门 OBS_C2S（需 r9 起的产物）");
        if (!(g_c2sfp = fopen(lg, "w"))) die("DM_C2S_LOG 打不开 %s", lg);
        gate_obs_set(OBS_C2S, c2s_obs); LOG("DM_C2S_LOG → %s", lg);
        if (gate_obs_present(OBS_C2S_SYS)) gate_obs_set(OBS_C2S_SYS, c2s_sys_obs); else LOG("【红】DM_C2S_LOG：产物没有系统类消息观测门 OBS_C2S_SYS（需 r10 起），系统类消息不记");
    }
    if (rp) {
        if (!gate_svc_present(0x81)) die("DM_C2S_REPLAY：产物没有收包投递剪口（svc #0x81，需 r9 起的产物）");
        if (!gate_svc_present(0x82)) LOG("【红】DM_C2S_REPLAY：产物没有系统类消息剪口（svc #0x82，需 r10 起），宿主客户端自发的系统类消息无法丢弃");
        if (gate_obs_present(OBS_C2S_SYS) && !g_c2sfp) gate_obs_set(OBS_C2S_SYS, c2s_sys_obs);   // 取宿主客户端号（注入系统类消息用） | Gets the host client number (used for injecting system-class messages)
        c2s_load(rp);
    }
    if (e) {
        if (!(g_tkfp = fopen(e, "w"))) die("DM_TICKS 打不开 %s", e);
        const char *nn = getenv("DM_TICKS_N"); if (nn) g_tkleft = atol(nn);
        g_tkfull = getenv("DM_TICKS_FULL") != NULL;
        LOG("DM_TICKS → %s%s%s", e, nn ? "，拍数 " : "", nn ? nn : "");
    }
    if (sp) {
        if (!(g_spfp = fopen(sp, "w"))) die("DM_STATE 打不开 %s", sp);
        LOG("DM_STATE → %s", sp);
    }
    // 产品缺省也要挂拍入口：UIKit 底栏的数字（等级 / 经验 / 腰带 / 回城 / 难度档）和难度条落地都在拍入口做（players_apply / hud_tick）。
    // 之前只有带 DM_TICKS / DM_STATE / DM_C2S_* 时才挂——所有用例都带，玩家点图标启动反而没有：perf.txt 场景列全是「菜单」、底栏不显示、点难度条不生效。
    // 不带测试变量时挂精简版（只做这两件）；带了就由下面换成完整版
    // The product default must hook the tick entry too: the numbers on the UIKit bottom bar (level / experience / belt / return to town / difficulty level) and applying the difficulty bar are all done at the tick entry (players_apply / hud_tick).
    // Previously it was hooked only with DM_TICKS / DM_STATE / DM_C2S_* -- every test case has them, but a player launching by tapping the icon did not: the scene column of perf.txt was all 「菜单」(menu), the bottom bar did not show, and tapping the difficulty bar had no effect.
    // Without test variables a slim version is hooked (doing only these two things); with them, it is replaced by the full version below
    if (gate_obs_present(OBS_C2S)) gate_obs_set(OBS_C2S, c2s_obs);   // 首领连战要看「点了哪个单位」的命令包；DM_C2S_LOG 记包也走这个回调 | Consecutive boss fights need to see the command packet for "which unit was clicked"; DM_C2S_LOG packet logging also goes through this callback
    else LOG("【红】产物没有命令分派观测门 OBS_C2S：首领连战的门点了不换关");
    if (gate_obs_present(OBS_TICK)) gate_obs_set(OBS_TICK, tick_gate_product);
    else LOG("【红】产物没有拍入口观测门 OBS_TICK：底栏数字 / 难度条 / 动态 3D 比例（dm_ingame_now）都不工作");
    { module_t *tm = main_mod(); void dm_textbox_install(uint8_t *); if (tm) dm_textbox_install((uint8_t *)tm->base); }   // 普通文字框里的注释颜色码（dm_textbox.c，观测门 14） | Annotation color codes in ordinary text boxes (dm_textbox.c, observation gate 14)
    { module_t *tm = main_mod(); void dm_turnsmooth_install(uint8_t *); if (tm) dm_turnsmooth_install((uint8_t *)tm->base); }   // 转向回跳平滑 / 模型转身参数读取（dm_turnsmooth.c，观测门 19 / 20） | turn snap-back smoothing / reading the model turning parameters (dm_turnsmooth.c, observation gates 19 / 20)
    if (e || rp || lg || sp) {                              // DM_C2S_LOG 也挂拍入口：系统类消息的拍号要从 G 读 | DM_C2S_LOG also hooks the tick entry: the tick number of system-class messages has to be read from G
        if (!gate_obs_present(OBS_TICK)) die("DM_TICKS / DM_STATE / DM_C2S_REPLAY / DM_C2S_LOG：产物没有拍入口观测门 OBS_TICK（需 r8 起的产物）");
        gate_obs_set(OBS_TICK, tick_gate);
        void uimsg_subscribe(void (*)(const char *, const char *, const char *));
        const char *pc = getenv("DM_POSCHK");
        if (pc) { if (sscanf(pc, "%u:%u:%u:%u:%u", &g_pos_tick, &g_pos_want[0], &g_pos_want[1], &g_pos_want[2], &g_pos_want[3]) != 5) die("DM_POSCHK 格式：E:x:y:xf:yf（得到「%s」）", pc);
                  g_pos_on = 1; LOG("DM_POSCHK：拍 %u 核玩家坐标 (%u,%u)/(%u,%u)", g_pos_tick, g_pos_want[0], g_pos_want[1], g_pos_want[2], g_pos_want[3]); }
        const char *h = getenv("DM_TICK_HOLD");
        if (h) { g_hold_tick = (uint32_t)strtoul(h, 0, 0); const char *c = strchr(h, ':'); if (c) snprintf(g_hold_ui, sizeof g_hold_ui, "%s", c + 1);
                 uimsg_subscribe(hold_on_uimsg); LOG("DM_TICK_HOLD：拍 %u 冻结客体时钟直到「%s」", g_hold_tick, g_hold_ui); }
    }
}
