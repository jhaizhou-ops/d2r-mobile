// 将 Win32 异常交给宿主原有的信号钩子；保留 CRT 普通信号。
// Route Win32 exceptions to existing host signal hooks while retaining ordinary CRT signals.
#pragma once
#include_next <signal.h>
#include <sys/ucontext.h>
#define SIGBUS 10
#define SIGALRM 14
#define SIGUSR2 12
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#define SA_SIGINFO 1
#define SA_ONSTACK 2
#define SA_RESTART 4
#define SA_NODEFER 8
#define SS_DISABLE 1
typedef unsigned dm_sigset_t;
typedef struct { void *si_addr; } siginfo_t;
typedef struct { void *ss_sp; size_t ss_size; int ss_flags; } stack_t;
struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
    int sa_flags;
    dm_sigset_t sa_mask;
};
#ifdef __cplusplus
extern "C" {
#endif
int sigaction(int signal, const struct sigaction *action, struct sigaction *previous);
int sigemptyset(dm_sigset_t *set);
int sigaltstack(const stack_t *stack, stack_t *previous);
unsigned alarm(unsigned seconds);
#ifdef __cplusplus
}
#endif
