// CRT 描述符上限、真实进程 CPU 时间与调用线程优先级；不支持的资源类别不伪造成功。
// CRT descriptor limits, actual process CPU times and calling-thread priority; unsupported resource kinds never fake success.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <windows.h>
#include <sys/time.h>
#define RLIMIT_NOFILE 7
#define PRIO_PROCESS 0
#define RUSAGE_SELF 0
typedef uint64_t rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
struct rusage { struct timeval ru_utime, ru_stime; };
static inline int getrusage(int who, struct rusage *out) {
    if (who != RUSAGE_SELF) { errno = EINVAL; return -1; }
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) { errno = EIO; return -1; }
    uint64_t u = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    uint64_t k = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    out->ru_utime = (struct timeval){(long)(u / 10000000), (long)(u % 10000000 / 10)};
    out->ru_stime = (struct timeval){(long)(k / 10000000), (long)(k % 10000000 / 10)};
    return 0;
}
static inline int getrlimit(int resource, struct rlimit *limit) {
    if (resource != RLIMIT_NOFILE) { errno = EINVAL; return -1; }
    limit->rlim_cur = limit->rlim_max = (rlim_t)_getmaxstdio();
    return 0;
}
// Linux 上 setpriority(PRIO_PROCESS,0,...) 调整调用线程；不要降整个 Windows 进程的优先级。
// On Linux setpriority(PRIO_PROCESS,0,...) changes the calling thread; do not lower the entire Windows process.
static inline int setpriority(int kind, int who, int priority) {
    if (kind != PRIO_PROCESS || who != 0 || priority < 0 || priority > 19) { errno = EINVAL; return -1; }
    int value = priority >= 10 ? THREAD_PRIORITY_LOWEST : priority > 0 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
    if (SetThreadPriority(GetCurrentThread(), value)) return 0;
    fprintf(stderr, "[dm] SetThreadPriority failed: %lu\n", GetLastError()); errno = EACCES; return -1;
}
