// 宿主生命周期：优雅退出（SIGTERM；iOS 由 App 终止回调调 dm_host_shutdown）。
// 停后台线程（启动块预取）、暂停音频、图形挂起（等在途 NVN 调用结束并清空 GPU 工作），再 _exit(0)；1.5 秒兜底直接 _exit(0)。
// 缺省动作同样约 0.08 秒退出（退出码 143）；本函数的价值是有序停后台线程、退出码 0、给 iOS 生命周期回调一个入口。
// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Stop background threads (boot block prefetch), pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#ifdef __ANDROID__
#include "../android/services_platform.h"
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#else
#include <dispatch/dispatch.h>
#endif
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void tact_prefetch_stop(void);
void granny_prof_dump(void);
void hlecount_flush(void);
#ifdef __ANDROID__
static _Atomic int shutdown_started, shutdown_done;
static int termination_pipe[2] = {-1, -1};
static pthread_once_t term_once = PTHREAD_ONCE_INIT;
static void *shutdown_watchdog(void *unused) {
    (void)unused;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 1; deadline.tv_nsec += 500000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {}
    if (!atomic_load(&shutdown_done)) { LOG("Android shutdown did not drain real backends in time"); _exit(124); }
    return NULL;
}
void dm_host_shutdown(const char *why) {
    if (atomic_exchange(&shutdown_started, 1)) return;
    LOG("Android shutdown requested: %s", why);
    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, shutdown_watchdog, NULL)) die("Android shutdown watchdog creation");
    pthread_detach(watchdog);
    int error = dm_android_platform_shutdown();
    if (error) die("Android backend shutdown failed: %d", error);
    atomic_store(&shutdown_done, 1);
    LOG("Android registered backends drained; explicitly absent backends skipped");
    _exit(0);
}
static void term_signal(int signal_number) {
    (void)signal_number;
    int saved_errno = errno;
    char message = 1;
    if (termination_pipe[1] >= 0) (void)write(termination_pipe[1], &message, 1);
    errno = saved_errno;
}
static void *term_worker(void *unused) {
    (void)unused;
    for (;;) {
        char message;
        ssize_t count = read(termination_pipe[0], &message, 1);
        if (count == 1) dm_host_shutdown("SIGTERM");
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) die("Android termination signal pipe failed");
    }
}
static void install_termination(void) {
    if (pipe2(termination_pipe, O_CLOEXEC)) die("Android termination pipe");
    int flags = fcntl(termination_pipe[1], F_GETFL);
    if (flags < 0 || fcntl(termination_pipe[1], F_SETFL, flags | O_NONBLOCK)) die("Android termination pipe flags");
    pthread_t worker;
    if (pthread_create(&worker, NULL, term_worker, NULL)) die("Android termination worker");
    pthread_detach(worker);
    struct sigaction action = {.sa_handler = term_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL)) die("Android SIGTERM registration");
}
void dm_term_install(void) { pthread_once(&term_once, install_termination); }
#else
void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("收到 %s：优雅退出", why);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("【红】优雅退出 1.5 秒未完成，直接退出"); _exit(0); });
    tact_prefetch_stop();
    granny_prof_dump();
    hlecount_flush();                               // DM_HLECOUNT：_exit 不走 atexit，这里写一份 | DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("优雅退出完成（%.3f 秒）", (now_ns() - t0) / 1e9);
    _exit(0);
}
void dm_term_install(void) {
    signal(SIGTERM, SIG_IGN);                       // 不走缺省动作：交给 GCD 信号源在普通线程上收尾 | do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
}
#endif
