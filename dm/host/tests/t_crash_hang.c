// 单测（crash.c 崩溃处理不会把崩溃变成卡死）：每一例在子进程里装上真的崩溃处理再故意崩，父进程限时等它退出、读它的 stderr。
//   1 现场导出正常返回：子进程马上以 128+信号号退出，日志里有信号安全的第一段（CRASH 行、寄存器、回溯）
//   2 现场导出永远不返回（模拟卡在锁上）：3 秒闹钟到点直接退出，日志里有「3 秒没走完」
//   3 崩溃线程自己持着一把锁，现场导出去拿同一把锁（真机就是这样：jemalloc 持锁时栈溢出，崩溃处理 fread → malloc 等同一把锁）：同 2
// 修前 2、3 两例子进程永远不退（父进程 8 秒后判失败、杀掉它）
// Unit test (crash.c's crash handler never turns a crash into a hang): each case installs the real crash handler in a child process and crashes on
// purpose; the parent waits for it to exit within a time limit and reads its stderr.
//   1 The dump returns normally: the child exits at once with 128 + the signal number, and the log has the signal-safe first part (CRASH line,
//     registers, backtrace)
//   2 The dump never returns (stuck on a lock): the 3-second alarm exits the process, and the log has "3 秒没走完" (did not finish in 3 s)
//   3 The crashing thread holds a lock and the dump tries to take the same lock (what happened on the phone: a stack overflow while jemalloc held its
//     lock, then the crash handler's fread -> malloc waited on that same lock): as 2
// Before the fix cases 2 and 3 never exit (the parent fails them after 8 s and kills the child)
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// 安卓 NDK 静态链接（-DDM_STATIC_TEST，在电脑上经 qemu 跑）没有 libdl：给 dladdr 一个「查不到」的替身，第二段就照原值打
// The Android NDK static build (-DDM_STATIC_TEST, run on a PC through qemu) has no libdl: dladdr gets a "not found" stand-in, so the second part prints
// raw values
#ifdef DM_STATIC_TEST
#include <dlfcn.h>
int dladdr(const void *addr, Dl_info *info) { (void)addr; memset(info, 0, sizeof *info); return 0; }
#endif
void crash_install(void);
void crash_set_dump_hook(void (*fn)(void));

static pthread_mutex_t g_held = PTHREAD_MUTEX_INITIALIZER;
static void dump_returns(void) {}
static void dump_blocks(void) { for (;;) pause(); }
static void dump_takes_held_lock(void) { pthread_mutex_lock(&g_held); }

static void crash_now(void) { *(volatile int *)8 = 1; }

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

// 跑一例：返回 1 = 通过。want_exit = 期望的退出码，need = 日志里必须有的字样，within = 最长秒数
// Run one case: returns 1 = pass. want_exit = the expected exit code, need = text that must be in the log, within = the longest allowed seconds
static int run(const char *name, void (*hook)(void), int hold_lock, int want_exit, const char *need, double within) {
    int fds[2]; if (pipe(fds)) { perror("pipe"); return 0; }
    double t0 = now_s();
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]); dup2(fds[1], 2);
        crash_install();
        crash_set_dump_hook(hook);
        if (hold_lock) pthread_mutex_lock(&g_held);
        crash_now();
        _exit(0);
    }
    close(fds[1]);
    char log[16384]; size_t n = 0; int status = 0, done = 0;
    while (now_s() - t0 < 8) {
        struct pollfd p = {fds[0], POLLIN, 0};
        if (poll(&p, 1, 100) > 0) { ssize_t k = read(fds[0], log + n, sizeof log - 1 - n); if (k > 0) n += (size_t)k; }
        if (waitpid(pid, &status, WNOHANG) == pid) { done = 1; break; }
    }
    double took = now_s() - t0;
    if (!done) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    for (;;) { struct pollfd p = {fds[0], POLLIN, 0}; if (poll(&p, 1, 0) <= 0) break; ssize_t k = read(fds[0], log + n, sizeof log - 1 - n); if (k <= 0) break; n += (size_t)k; }
    close(fds[0]); log[n] = 0;
    int code = done && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int ok = done && code == want_exit && took <= within && strstr(log, "[dm] CRASH sig=11") && strstr(log, "signal-safe") && (!need || strstr(log, need));
    printf("%-34s %s：%s，退出码 %d（期望 %d），%.1f 秒（限 %.0f）%s\n", name, ok ? "✓" : "✗", done ? "子进程退出" : "子进程卡住、已杀", code, want_exit, took, within,
           need && !strstr(log, need) ? "，缺日志字样" : "");
    if (!ok) fprintf(stderr, "---- %s 的日志 | log ----\n%s\n----\n", name, log);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= run("现场导出正常返回 | dump returns", dump_returns, 0, 128 + SIGSEGV, NULL, 2);
    ok &= run("现场导出永不返回 | dump blocks", dump_blocks, 0, 128 + SIGSEGV, "3 秒没走完", 6);
    ok &= run("持锁崩溃、导出等同一把锁 | held lock", dump_takes_held_lock, 1, 128 + SIGSEGV, "3 秒没走完", 6);
    printf("%s\n", ok ? "t_crash_hang：全部通过 | all passed" : "t_crash_hang：有失败 | failures");
    return !ok;
}
