// 宿主发起的客体线程（Granny 预解码等宿主后台活要调游戏自己的分配器 / 锁，需要完整的 SDK 线程上下文）。
// 做法：在客体线程上（例如 guest_main 进 nnMain 之前）直接调 SDK 的 nn::os::CreateThread / StartThread，入口填宿主函数——
// 线程由 SDK 建，ThreadType、线程句柄、TLS 槽全齐，游戏的 malloc / nn::os 锁照常可用（锁标签字写的是该线程的客体句柄）。
// 约束：必须在客体线程上调用（SDK 建线程走 SVC 门）；不暴露给游戏。
// 自检 DM_GUEST_SPAWN_TEST=1：guest_main 进 nnMain 前起一个线程，做「SDK malloc/free + nn::os 互斥锁加锁、核持有者是自己」，WaitThread 收尾后报结果。
// Host-initiated guest threads (host background work such as Granny pre-decoding needs to call the game's own allocator / locks and needs a complete SDK thread context).
// Approach: on a guest thread (e.g. before guest_main enters nnMain) call the SDK's nn::os::CreateThread / StartThread directly, with a host function as the entry --
// the thread is created by the SDK, so ThreadType, thread handle and TLS slots are all in place, and the game's malloc / nn::os locks work as usual (the lock tag word holds this thread's guest handle).
// Constraints: must be called on a guest thread (SDK thread creation goes through the SVC gate); not exposed to the game.
// Self-test DM_GUEST_SPAWN_TEST=1: before guest_main enters nnMain, start one thread that does "SDK malloc/free + lock an nn::os mutex and check that the owner is itself", and report the result after WaitThread finishes.
#include "hle.h"
#include <stdatomic.h>
#include <unistd.h>
typedef int (*create_fn)(void *tt, void (*fn)(void *), void *arg, void *stack, size_t size, int prio);
typedef void (*tt_fn)(void *tt);
static void *need(const char *n) { void *p = guest_sym(n); if (!p) die("宿主客体线程：缺符号 %s", n); return p; }
#ifdef __ANDROID__
typedef struct { uint8_t thread_type[0x200]; void *stack; } owned_guest_thread;
#endif
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) {
    stack_size = (stack_size + 0xfff) & ~(size_t)0xfff;
#ifdef __ANDROID__
    owned_guest_thread *owned = calloc(1, sizeof *owned);
    void *tt = owned;
#else
    void *tt = calloc(1, 0x200);                                 // nn::os::ThreadType（0x1c0 字节，留余量） | nn::os::ThreadType (0x1c0 bytes, with headroom)
#endif
    void *stack = NULL;
#ifdef __ANDROID__
    // Bionic aligned_alloc is API 28; this host supports API 26.
    if (posix_memalign(&stack, 0x1000, stack_size)) stack = NULL;
#else
    stack = aligned_alloc(0x1000, stack_size);              // SDK 要求栈 4 KiB 对齐、大小 4 KiB 整数倍 | the SDK requires the stack to be 4 KiB aligned and its size a multiple of 4 KiB
#endif
    if (!tt || !stack) die("宿主客体线程：分配失败");
#ifdef __ANDROID__
    owned->stack = stack;
#endif
    int rc = ((create_fn)need("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"))(tt, fn, arg, stack, stack_size, prio);
    if (rc) die("宿主客体线程：CreateThread 失败 %#x", rc);
    ((tt_fn)need("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"))(tt);
    return tt;
}
void dm_guest_thread_join(void *tt) {
    ((tt_fn)need("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"))(tt);
    ((tt_fn)need("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"))(tt);
#ifdef __ANDROID__
    owned_guest_thread *owned = tt;
    free(owned->stack);
    free(owned);
#endif
}

static struct { int malloc_ok, lock_owned, handle; } g_st;
static void selftest_body(void *arg) {
    (void)arg;
    void *(*gmalloc)(size_t) = need("malloc"); void (*gfree)(void *) = need("free");
    void *p = gmalloc(4096); if (p) { memset(p, 0x5a, 4096); gfree(p); g_st.malloc_ok = 1; }
    uint8_t mtx[0x40] __attribute__((aligned(16))) = {0};           // nn::os::MutexType（0x20 字节，留余量） | nn::os::MutexType (0x20 bytes, with headroom)
    ((void (*)(void *, int, int))need("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi"))(mtx, 0, 0);
    ((void (*)(void *))need("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"))(mtx);
    g_st.lock_owned = ((int (*)(const void *))need("_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE"))(mtx) & 1;
    ((void (*)(void *))need("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"))(mtx);
    uint32_t thr_cur_handle(void); g_st.handle = (int)thr_cur_handle();
}
static void park_body(void *arg) { (void)arg; for (;;) sleep(3600); }   // 常驻（=2）：模拟预解码线程整局存活，占住句柄 | resident (=2): simulates the pre-decode thread staying alive for the whole run, occupying the handle
void dm_guest_spawn_selftest(void) {
    const char *e = getenv("DM_GUEST_SPAWN_TEST"); if (!e) return;
    if (atoi(e) == 2) { dm_guest_thread_spawn(park_body, NULL, 16, 16 << 10); LOG("宿主客体线程自检：另起常驻线程（之后游戏线程句柄顺延）"); }
    void *tt = dm_guest_thread_spawn(selftest_body, NULL, 16, 64 << 10);
    dm_guest_thread_join(tt);
    uint32_t thr_cur_handle(void); int mine = (int)thr_cur_handle();
    int ok = g_st.malloc_ok && g_st.lock_owned && g_st.handle && g_st.handle != mine;
    LOG("%s宿主客体线程自检：SDK malloc/free %s；互斥锁持有者是本线程 %s；线程句柄 %#x（主线程 %#x）", ok ? "" : "【红】",
        g_st.malloc_ok ? "通" : "不通", g_st.lock_owned ? "是" : "否", g_st.handle, mine);
}
