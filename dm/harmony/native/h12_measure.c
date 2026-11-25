/* H-12b measurement builds only. Run at the engine's normal per-thread
 * frame boundary. Never interrupt a guest stack or driver system call. */
#include <stdint.h>
#include <time.h>

uint64_t dm_h12_ruler_ns(void) {
    struct timespec a, b;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a)) return 0;
    uint64_t acc = (uint64_t)a.tv_nsec;
    for (unsigned i = 0; i < 16384; ++i) {
        acc = acc * 1664525 + 1013904223;
        __asm__ volatile("" : "+r"(acc));
    }
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b)) return 0;
    int64_t elapsed = (int64_t)(b.tv_sec-a.tv_sec)*1000000000 + b.tv_nsec-a.tv_nsec;
    return elapsed > 0 ? (uint64_t)elapsed : 0;
}
