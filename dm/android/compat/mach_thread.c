// Mach 线程操作的 Linux 实现（声明见 compat/include/mach/mach.h）：只给宿主现场导出用（hle_thread.c svc_thread_dump）。
// 挂起 = 给目标线程发实时信号；信号处理（备用栈上）把寄存器写进共享槽，然后原地等到 thread_resume。同一时刻只挂起一条（互斥锁串行）。
// 目标线程 200 ms 内没进信号处理（信号被屏蔽、线程已退出）就撤销请求返回失败；晚到的信号看到请求已撤销立即返回，不会卡住。
// Linux implementation of Mach thread operations (declared in compat/include/mach/mach.h): only for the host's state dump (hle_thread.c svc_thread_dump).
// Suspend = send a real-time signal to the target thread; the signal handler (on the alternate stack) writes the registers into a shared slot and then waits in place until thread_resume. Only one thread is suspended at a time (serialized by a mutex).
// If the target thread does not enter the signal handler within 200 ms (signal blocked, thread already exited), the request is withdrawn and failure is returned; a late signal sees that the request was withdrawn and returns immediately, so nothing gets stuck.
#include <mach/mach.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <fcntl.h>
#include <unistd.h>

enum { ST_IDLE, ST_REQ, ST_HELD, ST_RESUME };
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static _Atomic int g_state, g_target;
static arm_thread_state64_t g_regs;

static void pause_us(long us) { struct timespec ts = {0, us * 1000}; nanosleep(&ts, NULL); }
static void on_suspend(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    int e = errno;
    int want = ST_REQ;
    if (atomic_load(&g_target) != gettid() || !atomic_compare_exchange_strong(&g_state, &want, ST_IDLE)) { errno = e; return; }
    const ucontext_t *uc = uc_;
    for (int i = 0; i < 29; i++) g_regs.__x[i] = uc->uc_mcontext.regs[i];
    g_regs.__fp = uc->uc_mcontext.regs[29]; g_regs.__lr = uc->uc_mcontext.regs[30];
    g_regs.__sp = uc->uc_mcontext.sp; g_regs.__pc = uc->uc_mcontext.pc; g_regs.__cpsr = (uint32_t)uc->uc_mcontext.pstate;
    atomic_store(&g_state, ST_HELD);
    while (atomic_load(&g_state) != ST_RESUME) pause_us(200);
    atomic_store(&g_state, ST_IDLE);
    errno = e;
}
static int suspend_sig(void) { return SIGRTMIN + 4; }
static void install(void) {
    struct sigaction sa = {.sa_sigaction = on_suspend, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigemptyset(&sa.sa_mask); sigaction(suspend_sig(), &sa, NULL);
}

kern_return_t thread_suspend(thread_act_t t) {
    if ((pid_t)t == gettid()) return KERN_FAILURE;
    pthread_once(&g_once, install);
    pthread_mutex_lock(&g_lock);
    atomic_store(&g_target, (int)t); atomic_store(&g_state, ST_REQ);
    if (syscall(SYS_tgkill, getpid(), (pid_t)t, suspend_sig())) goto fail;
    for (int i = 0; i < 1000; i++) { if (atomic_load(&g_state) == ST_HELD) return KERN_SUCCESS; pause_us(200); }
    { int want = ST_REQ; if (!atomic_compare_exchange_strong(&g_state, &want, ST_IDLE)) {   // 恰好在撤销前进了处理：照常持有 | Entered the handler just before the withdrawal: hold as usual
        while (atomic_load(&g_state) != ST_HELD) pause_us(200);
        return KERN_SUCCESS; } }
fail:
    atomic_store(&g_target, 0); atomic_store(&g_state, ST_IDLE);
    pthread_mutex_unlock(&g_lock);
    return KERN_FAILURE;
}
kern_return_t thread_get_state(thread_act_t t, int flavor, thread_state_t out, mach_msg_type_number_t *count) {
    if (flavor != ARM_THREAD_STATE64 || atomic_load(&g_state) != ST_HELD || atomic_load(&g_target) != (int)t) return KERN_FAILURE;
    memcpy(out, &g_regs, sizeof g_regs); if (count) *count = ARM_THREAD_STATE64_COUNT;
    return KERN_SUCCESS;
}
kern_return_t thread_resume(thread_act_t t) {
    if (atomic_load(&g_state) != ST_HELD || atomic_load(&g_target) != (int)t) return KERN_FAILURE;
    atomic_store(&g_state, ST_RESUME);
    while (atomic_load(&g_state) != ST_IDLE) pause_us(50);
    atomic_store(&g_target, 0);
    pthread_mutex_unlock(&g_lock);
    return KERN_SUCCESS;
}

kern_return_t task_threads(mach_port_t task, thread_act_array_t *out, mach_msg_type_number_t *n) {
    (void)task;
    DIR *d = opendir("/proc/self/task"); if (!d) return KERN_FAILURE;
    unsigned cap = 64, k = 0; thread_act_t *a = malloc(cap * sizeof *a); struct dirent *e;
    while (a && (e = readdir(d))) {
        int tid = atoi(e->d_name); if (tid <= 0) continue;
        if (k == cap) { thread_act_t *b = realloc(a, (cap *= 2) * sizeof *a); if (!b) { free(a); a = NULL; break; } a = b; }
        a[k++] = (thread_act_t)tid;
    }
    closedir(d);
    if (!a) return KERN_FAILURE;
    *out = a; *n = k; return KERN_SUCCESS;
}

kern_return_t thread_info(thread_act_t t, int flavor, thread_info_t out, mach_msg_type_number_t *count) {
    (void)count;
    if (flavor != THREAD_BASIC_INFO) return KERN_FAILURE;
    // 问的是调用线程自己（hle_nvn.c 的 thr_sys_ns 每次提交读两次）：getrusage(RUSAGE_THREAD) 一次系统调用、微秒精度。
    // 打开 /proc 读 stat 每次要 open / read / close 加解析，约占 Redmi 营地渲染线程的 0.7%，而且只有时钟滴答（10 ms）精度
    // The calling thread itself (hle_nvn.c's thr_sys_ns reads it twice per submit): getrusage(RUSAGE_THREAD) is one system
    // call with microsecond precision. Reading /proc's stat takes an open, a read, a close and parsing each time, about 0.7% of
    // the render thread in the Redmi camp, and it only has clock-tick (10 ms) precision
    if ((pid_t)t == gettid()) {
        struct rusage ru; if (getrusage(RUSAGE_THREAD, &ru)) return KERN_FAILURE;
        thread_basic_info_data_t *bi = (thread_basic_info_data_t *)out; memset(bi, 0, sizeof *bi);
        bi->user_time.seconds = (int)ru.ru_utime.tv_sec; bi->user_time.microseconds = (int)ru.ru_utime.tv_usec;
        bi->system_time.seconds = (int)ru.ru_stime.tv_sec; bi->system_time.microseconds = (int)ru.ru_stime.tv_usec;
        return KERN_SUCCESS;
    }
    // 读别的线程的 stat 只用 open / read 和栈上缓冲，不用 stdio：崩溃处理导现场时也会走到这里（svc_thread_dump），fopen / fread 要分配内存，
    // 崩溃若正好在分配器持锁时发生就会在这里等死（OnePlus 13：fread → malloc 自锁，进程卡住报「无响应」）
    // Another thread's stat is read with open / read and stack buffers only, no stdio: the crash handler's dump comes here too (svc_thread_dump), and
    // fopen / fread allocate memory, so a crash that lands while the allocator holds its lock would wait here forever (OnePlus 13: fread -> malloc
    // deadlocked on itself, the process hung and was reported "not responding")
    char path[48] = "/proc/self/task/", digits[12], buf[1024]; int n = 16, nd = 0; unsigned v = (unsigned)t;
    do { digits[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (nd) path[n++] = digits[--nd];
    memcpy(path + n, "/stat", 6);
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return KERN_FAILURE;
    ssize_t got = read(fd, buf, sizeof buf - 1); close(fd); if (got <= 0) return KERN_FAILURE; buf[got] = 0;
    char *p = strrchr(buf, ')'); if (!p) return KERN_FAILURE;   // 线程名可含空格：从最后一个 ')' 之后数字段 | The thread name may contain spaces: count fields after the last ')'
    // ')' 后第 1 个字段是 state（总第 3 字段）；utime / stime 是总第 14 / 15 字段，即 state 之后第 11、12 个
    // The first field after ')' is state (field 3 overall); utime / stime are fields 14 / 15 overall, i.e. the 11th and 12th after state
    unsigned long ut = 0, st = 0; int field = 0;
    for (p++; *p && field < 13; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        field++;
        unsigned long num = 0; while (*p >= '0' && *p <= '9') num = num * 10 + (unsigned long)(*p++ - '0');
        if (field == 12) ut = num; else if (field == 13) st = num;
        while (*p && *p != ' ') p++;
    }
    if (field < 13) return KERN_FAILURE;
    long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
    thread_basic_info_data_t *bi = (thread_basic_info_data_t *)out; memset(bi, 0, sizeof *bi);
    bi->user_time.seconds = (int)(ut / hz); bi->user_time.microseconds = (int)(ut % hz * 1000000 / hz);
    bi->system_time.seconds = (int)(st / hz); bi->system_time.microseconds = (int)(st % hz * 1000000 / hz);
    return KERN_SUCCESS;
}
