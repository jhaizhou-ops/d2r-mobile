#include "../host/dm.h"
#include "../host/dm_custom.h"
#include "services_platform.h"
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <errno.h>
#include <sys/uio.h>
#include <dlfcn.h>
void dm_backtrace(uint64_t fp);   /* defined below; used by the non-guest diagnostic */

static pthread_once_t signals_once = PTHREAD_ONCE_INIT;
static pthread_once_t alt_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t alt_key;
static _Atomic int signals_ready;
static _Atomic uint64_t frames, svc_calls[256], prof_ns[PROF_N], prof_bytes[PROF_N];
static void (*ui_subscribers[16])(const char *, const char *, const char *);
static size_t ui_count;
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*write_hooks[8])(void *, size_t);
static size_t write_hook_count;
static pthread_mutex_t write_hook_lock = PTHREAD_MUTEX_INITIALIZER;
typedef int (*fault_hook)(void *, int, void *);
static _Atomic(fault_hook) fault_hooks[8];
static pthread_mutex_t fault_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static const int handled_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT};
static struct sigaction previous_signals[4];
void dm_fault_hook_register(fault_hook fn) {
    if (!fn) die("Android null fault hook");
    pthread_mutex_lock(&fault_hook_lock);
    unsigned i;
    for (i = 0; i < sizeof fault_hooks / sizeof *fault_hooks; ++i) {
        fault_hook old = atomic_load_explicit(&fault_hooks[i], memory_order_acquire);
        if (old == fn) die("Android duplicate fault hook");
        if (!old) { atomic_store_explicit(&fault_hooks[i], fn, memory_order_release); break; }
    }
    if (i == sizeof fault_hooks / sizeof *fault_hooks) die("Android fault hook capacity");
    pthread_mutex_unlock(&fault_hook_lock);
}
/* Linux arm64's ESR context record describes the faulting access. Unknown or
 * malformed records must not be treated as write faults. */
static int fault_is_write(const ucontext_t *u) {
    const unsigned char *p = u->uc_mcontext.__reserved;
    size_t left = sizeof u->uc_mcontext.__reserved;
    while (left >= 8) {
        uint32_t magic, size; memcpy(&magic, p, 4); memcpy(&size, p + 4, 4);
        if (!magic || size < 8 || size > left || size % 16) return 0;
        if (magic == 0x45535201 && size >= 16) {
            uint64_t esr; memcpy(&esr, p + 8, 8);
            unsigned ec = (unsigned)(esr >> 26) & 63;
            return (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));
        }
        p += size; left -= size;
    }
    return 0;
}
static void chain_signal(int sig, siginfo_t *info, void *opaque) {
    for (unsigned i = 0; i < sizeof handled_signals / sizeof *handled_signals; ++i) {
        if (handled_signals[i] != sig) continue;
        struct sigaction old = previous_signals[i];
        if (old.sa_handler == SIG_IGN) return;
        if (old.sa_handler == SIG_DFL) {
            sigaction(sig, &old, NULL);
            syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);
            return;
        }
        if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(sig, info, opaque);
        else old.sa_handler(sig);
        return;
    }
    _exit(128 + sig);
}
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) {
    if (!fn) die("Android null guest write hook");
    pthread_mutex_lock(&write_hook_lock);
    if (write_hook_count >= sizeof write_hooks / sizeof *write_hooks) die("Android guest write hook capacity");
    for (size_t i = 0; i < write_hook_count; ++i) if (write_hooks[i] == fn) die("Android duplicate guest write hook");
    write_hooks[write_hook_count++] = fn;
    pthread_mutex_unlock(&write_hook_lock);
}
void dm_guest_write_prepare(void *address, size_t length) {
    if (!address && length) die("Android invalid guest write buffer");
    void (*copy[8])(void *, size_t);
    pthread_mutex_lock(&write_hook_lock);
    size_t n = write_hook_count; memcpy(copy, write_hooks, n * sizeof *copy);
    pthread_mutex_unlock(&write_hook_lock);
    for (size_t i = 0; i < n; ++i) copy[i](address, length);
}

static void fatal_signal(int sig, siginfo_t *info, void *opaque) {
    ucontext_t *u = opaque;
    if (sig != SIGSEGV || !fault_is_write(u)) {
        static const char entered[] = "ANDROID_NONWRITE_SIGNAL_ENTRY\n";
        write(STDERR_FILENO, entered, sizeof entered - 1);
    }
    /* Preserve a register-only breadcrumb even if the fault hook itself or a
     * prior ART handler encounters a second fault. No allocation or locks. */
    static _Atomic unsigned breadcrumbs;
    {
        unsigned count=atomic_fetch_add(&breadcrumbs,1);
        if(count<32 || !fault_is_write(u)) {
            char b[256];int n=snprintf(b,sizeof b,"ANDROID_SIGNAL_ENTRY sig=%d code=%d tid=%ld address=%p pc=%#llx lr=%#llx sp=%#llx guest=%d\n",
                sig,info->si_code,syscall(SYS_gettid),info->si_addr,(unsigned long long)u->uc_mcontext.pc,
                (unsigned long long)u->uc_mcontext.regs[30],(unsigned long long)u->uc_mcontext.sp,dm_android_thread_has_guest_context());
            if(n>0)write(STDERR_FILENO,b,(size_t)n<sizeof b?(size_t)n:sizeof b-1);
        }
    }
    if ((sig == SIGSEGV || sig == SIGBUS) && info->si_code > 0) {
        int is_write = fault_is_write(u);
        for (unsigned i = 0; i < sizeof fault_hooks / sizeof *fault_hooks; ++i) {
            fault_hook fn = atomic_load_explicit(&fault_hooks[i], memory_order_acquire);
            if (!fn) break;
            if (fn(info->si_addr, is_write, opaque)) return;
        }
    }
    /* ART uses SIGSEGV for managed implicit checks. Claim only threads that
     * actually own a registered guest context; preserve the previous handler. */
    if (!dm_android_thread_has_guest_context()) {
        /* MERGE DIAG (test): also dump a raw frame-pointer chain for non-guest threads
         * (e.g. the JNI surface thread) before chaining to the previous handler. */
        { Dl_info di; if (dladdr((const void *)&dm_backtrace, &di) && di.dli_fbase)
              fprintf(stderr, "[dm] HOST_BASE %p %s\n", di.dli_fbase, di.dli_fname ? di.dli_fname : "?"); }
        fprintf(stderr, "[dm] NON_GUEST_ABORT sig=%d tid=%ld fp=%#llx\n", sig, syscall(SYS_gettid),
                (unsigned long long)u->uc_mcontext.regs[29]);
        dm_backtrace(u->uc_mcontext.regs[29]);
        chain_signal(sig, info, opaque); return; }
    char line[256];
    int n = snprintf(line, sizeof line,
        "ANDROID_FATAL signal=%d tid=%ld address=%p pc=%#llx lr=%#llx sp=%#llx\n",
        sig, syscall(SYS_gettid), info->si_addr,
        (unsigned long long)u->uc_mcontext.pc, (unsigned long long)u->uc_mcontext.regs[30],
        (unsigned long long)u->uc_mcontext.sp);
    if (n > 0) write(STDERR_FILENO, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    for(unsigned i=0;i<31;i+=3) {
        n=snprintf(line,sizeof line,"ANDROID_REG x%u=%#llx x%u=%#llx x%u=%#llx\n",i,(unsigned long long)u->uc_mcontext.regs[i],
            i+1,(unsigned long long)u->uc_mcontext.regs[i+1<31?i+1:30],i+2,(unsigned long long)u->uc_mcontext.regs[i+2<31?i+2:30]);
        if(n>0)write(STDERR_FILENO,line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
    }
    for (int i = 0; i < g_nmod; ++i) {
        n = snprintf(line, sizeof line, "ANDROID_MODULE %s base=%p pc_offset=%#llx lr_offset=%#llx\n",
            g_mod[i].name, g_mod[i].base, (unsigned long long)(u->uc_mcontext.pc - (uintptr_t)g_mod[i].base),
            (unsigned long long)(u->uc_mcontext.regs[30] - (uintptr_t)g_mod[i].base));
        if (n > 0) write(STDERR_FILENO, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
    }
    dm_backtrace(u->uc_mcontext.regs[29]);
    _exit(128 + sig);
}
static void setup_signals(void) {
    struct sigaction sa = {.sa_sigaction = fatal_signal, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof handled_signals / sizeof *handled_signals; ++i)
        if (sigaction(handled_signals[i], &sa, &previous_signals[i])) die("Android crash signal setup errno=%d", errno);
    atomic_store(&signals_ready, 1);
}
static void destroy_alt(void *p) {
    stack_t off = {.ss_flags = SS_DISABLE};
    sigaltstack(&off, NULL);
    free(p);
}
static void setup_alt_key(void) {
    if (pthread_key_create(&alt_key, destroy_alt)) die("Android crash alt-stack key");
}
void crash_thread_init(void) {
    pthread_once(&signals_once, setup_signals);
    pthread_once(&alt_key_once, setup_alt_key);
    if (pthread_getspecific(alt_key)) return;
    void *p = malloc(128 * 1024);
    if (!p) die("Android signal stack allocation");
    stack_t alt = {.ss_sp = p, .ss_size = 128 * 1024};
    if (sigaltstack(&alt, NULL) || pthread_setspecific(alt_key, p)) die("Android signal stack attachment");
}
int dm_android_diagnostics_ready(void) {
    if (!atomic_load(&signals_ready)) return 0;
    stack_t alt;
    if (sigaltstack(NULL, &alt) || (alt.ss_flags & SS_DISABLE) || alt.ss_size < 128 * 1024) return 0;
    return alt.ss_sp == pthread_getspecific(alt_key);
}
int dm_android_diagnostics_flush(void) {
    if (fflush(stderr) || fflush(stdout)) return errno ? errno : EIO;
    return 0;
}
void crash_watch_frame(void) { atomic_fetch_add(&frames, 1); }
__attribute__((weak)) void dm_android_front_presented(uint64_t frame);
__attribute__((weak)) void dm_3d_scale_feed(double iv_ms);   // 动态 3D 比例（hle_oe.c）；core-only 探针里为空 | the dynamic 3D scale (hle_oe.c); null in core-only probes
void dm_android_nvn_presented(uint64_t frame) {
    dm_guest_frame_tick(); crash_watch_frame();
    /* 动态 3D 比例的输入（上游建议 §0e）：在出图点算两帧节拍之差当实际上屏间隔（ms）。
     * 与共享层 mg_pres_iv 同源思路，但 Android 拿不到 presentedTime，就用出图时刻推。
     * Input for the dynamic 3D scale (§0e): the delta between present points as the actual
     * on-screen interval (ms). Same idea as the shared g_pres_iv, but Android has no presented
     * timestamps, so the present-call time is used. */
    { static uint64_t prev_ns; uint64_t t=now_ns();
      if (dm_3d_scale_feed) dm_3d_scale_feed(prev_ns && t>prev_ns ? (double)(t-prev_ns)/1e6 : 0);
      prev_ns=t; }
    if (dm_android_front_presented) dm_android_front_presented(frame);
}
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) {
    if (id < 0 || id >= PROF_N) die("Android invalid profile counter %d", id);
    atomic_fetch_add(&prof_ns[id], ns); atomic_fetch_add(&prof_bytes[id], bytes);
}
void dm_prof_mark(const char *name) { LOG("ANDROID_PROFILE %s time_ns=%llu", name, now_ns()); }
void uimsg_subscribe(void (*fn)(const char *, const char *, const char *)) {
    pthread_mutex_lock(&ui_lock);
    if (ui_count >= sizeof ui_subscribers / sizeof *ui_subscribers) die("Android UI subscriber capacity");
    ui_subscribers[ui_count++] = fn;
    pthread_mutex_unlock(&ui_lock);
}
void dm_android_uimsg(const char *category, const char *action, const char *argument) {
    void (*copy[16])(const char *, const char *, const char *);
    pthread_mutex_lock(&ui_lock);
    size_t n = ui_count; memcpy(copy, ui_subscribers, n * sizeof *copy);
    pthread_mutex_unlock(&ui_lock);
    for (size_t i = 0; i < n; ++i) copy[i](category, action, argument);
}
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    (void)kind; (void)name;
    if (getenv("DM_HLECOUNT")) die("Android per-import counting thunks not implemented");
    return fn;
}
void hlecount_seal(void) {}
void hlecount_svc(int n) { if (n >= 0 && n < 256) atomic_fetch_add(&svc_calls[n], 1); }
uint64_t dm_android_svc_count(int n) { return n >= 0 && n < 256 ? atomic_load(&svc_calls[n]) : 0; }
/* svc_host_call 的实现移到了 svc_host_android.c（0x80 起的宿主调用分发：zlib/瞄准门/渲染门/原样代调）。 */
void dm_backtrace(uint64_t fp) {
    /* dm_thread_enter switches to an SDK-owned alias stack, outside Bionic's
     * pthread stack bounds. Self process_vm_readv validates each read in the
     * kernel and cannot cause a nested fault on an invalid guest frame. */
    uintptr_t frame=(uintptr_t)fp&0x00ffffffffffffffULL;
    LOG("ANDROID_BACKTRACE fp=%#llx SDK_owned_stack=true",fp);
    for(unsigned i=0;i<32 && frame && !(frame&15);++i){
        uint64_t pair[2];struct iovec local={pair,sizeof pair},remote={(void *)frame,sizeof pair};
        if(syscall(SYS_process_vm_readv,getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof pair)break;
        module_t *m=module_of((void *)(uintptr_t)pair[1]);
        LOG("ANDROID_CALLER %u lr=%#llx module=%s offset=%#llx",i,pair[1],m?m->name:"host",
            m?pair[1]-(uint64_t)(uintptr_t)m->base:0);
        uintptr_t next=(uintptr_t)pair[0]&0x00ffffffffffffffULL;if(next<=frame)break;frame=next;
    }
}
/* 输出分辨率：按「分辨率档」（自定义页 q_res，Android 专属）与屏幕尺寸算出来，比例严格等于 SurfaceView。
 * 档位预算见 docs/android-resolution-plan.zh-CN.md：low 1.8 MP / standard 2.4 MP / high = 屏幕原生。
 * DM_GAME_RES=宽x高 仍然最优先（脚本、调试、quality.txt 老档别名都走它）。
 * Output resolution: derived from the "resolution tier" (Custom page q_res, Android only) and the surface size,
 * with the aspect ratio exactly equal to the SurfaceView. Budgets: docs/android-resolution-plan.zh-CN.md
 * (low 1.8 MP / standard 2.4 MP / high = screen native). DM_GAME_RES=WxH still wins (scripts, debugging, old quality.txt aliases). */
static unsigned g_out_w, g_out_h;   // 最近一次算出的出图（hle_oe.c 反算 3D 比例要读同一个值） | last derived output (hle_oe.c reads the same value to derive the 3D scale)

static int game_res_env(unsigned *w, unsigned *h) {
    const char *e = getenv("DM_GAME_RES");
    if (!e) return 0;
    unsigned a = 0, b = 0; char tail = 0;
    if (sscanf(e, "%ux%u%c", &a, &b, &tail) == 2 && a >= 640 && a <= 3840 && b >= 360 && b <= 2160) { *w = a; *h = b; return 1; }
    static int bad;
    if (!bad) { bad = 1; LOG("DM_GAME_RES 非法：%s（要 宽x高，如 1920x1080）", e); }
    return 0;
}
static void quality_out_compute(unsigned *ow, unsigned *oh) {
    *ow = *oh = 0;
    dm_android_window_state ws; dm_android_platform_get_window(&ws);
    unsigned W = ws.width, H = ws.height;
    if (W < 640 || H < 360) return;                       /* 窗口还没到位：调用方兜底 1280x720 | surface not ready yet: caller falls back to 1280x720 */
    /* 三档改成**按纵向像素**定义（800p / 1080p / 原生）：宽度由高度反推，比例严格保持，
       所以任何屏幕比例下都不会拉伸。纵向像素是用户对清晰度最直观的量，也和"1080p"这类
       常规叫法对得上。高度不超过面板高度（原生档）。
       Tiers are defined by vertical pixels (800p / 1080p / native): width is derived from the height,
       so the aspect ratio is always preserved and nothing is stretched. */
    int tier = dm_custom_res_tier();                      /* 0=800p / 1=1080p / 2=原生 | native */
    unsigned target_h = tier == 0 ? 800u : tier == 1 ? 1080u : H;
    if (target_h > H) target_h = H;                        /* 不超过面板 | never above the panel */
    unsigned h = target_h & ~1u;
    unsigned w = ((unsigned)(h * (double)W / H + 0.5)) & ~1u;   /* 宽由高推：比例严格不变 | width from height: aspect exactly preserved */
    if (w < 320 || h < 180) return;
    *ow = w; *oh = h;
}
uint64_t front_game_res(int docked) {
    unsigned w = 0, h = 0;
    if (game_res_env(&w, &h)) { g_out_w = w; g_out_h = h; return ((uint64_t)h << 32) | w; }
    (void)docked;
    quality_out_compute(&w, &h);
    if (!w || !h) { static int once; if (!once++) LOG("【红】窗口尺寸未就绪，出图回落 1280x720"); w = 1280; h = 720; }
    g_out_w = w; g_out_h = h;
    static int logged;
    if (!logged++) LOG("出图：按分辨率档算 → %ux%u（tier=%d，比例不变；改档要重启游戏）", w, h, dm_custom_res_tier());
    return ((uint64_t)h << 32) | w;
}
unsigned dm_android_quality_out_px(void) {   /* 3D 比例反算用：最终出图像素（含 DM_GAME_RES 覆盖） | for the 3D-scale derivation: final output pixels (incl. DM_GAME_RES override) */
    unsigned w = 0, h = 0;
    if (game_res_env(&w, &h)) return w * h;
    if (g_out_w && g_out_h) return g_out_w * g_out_h;
    quality_out_compute(&w, &h);
    return w * h;
}
/* dm_game_res 由 dm_uiscreen.c 提供（P2 起编入）：先问前端再按真全屏规则处理 | dm_game_res comes from dm_uiscreen.c (compiled since P2) */
