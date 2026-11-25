#include "../nvn/nvn_pace_hysteresis.h"
// Return failing line, no host runtime required (also callable as a DLL).
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
int pace_hysteresis_test(void) {
 nvn_pace_hysteresis s={0};
 CHECK(!nvn_pace_hysteresis_down(&s,5));
 CHECK(!nvn_pace_hysteresis_down(&s,6));
 CHECK(!nvn_pace_hysteresis_down(&s,0));
 CHECK(!nvn_pace_hysteresis_down(&s,29));
 CHECK(nvn_pace_hysteresis_down(&s,6));
 CHECK(!s.bad_windows);
 CHECK(nvn_pace_hysteresis_down(&s,30));
 CHECK(nvn_pace_hysteresis_down(&s,61));
 CHECK(nvn_pace_hysteresis_down(&s,~0u));
 CHECK(!nvn_pace_hysteresis_down(&s,6));
 nvn_pace_hysteresis_reset(&s); // tier/refresh/long gap/probe/counter reset
 CHECK(!nvn_pace_hysteresis_down(&s,6));
 CHECK(nvn_pace_hysteresis_down(&s,29));
 // Negative control: old policy drops on first six, new policy does not.
 nvn_pace_hysteresis_reset(&s);
 CHECK((6>=6) && !nvn_pace_hysteresis_down(&s,6));
 return 0;
}
int main(void) { return pace_hysteresis_test(); }

#ifdef NVN_PACE_INTEGRATION
#ifdef _WIN32
int _fltused; // freestanding host DLL floating-point marker
#endif
// The check script extracts the unmodified production function into this include.
// No duplicated controller: exercise its real if/else ordering with fake clocks.
#include <stdint.h>
#include <stdatomic.h>
#include "../nvn/nvn_adaptive_pace.h"
#define __OHOS__ 1
#define CLOCK_THREAD_CPUTIME_ID 3
#define LOG(...) ((void)0)
static uint64_t test_now=1000000000, test_cpu=1000000, test_late;
static int test_enabled=1;
static uint64_t now_ns(void) { return test_now; }
static uint64_t clock_gettime_nsec_np(int ignored) { (void)ignored; return test_cpu; }
static uint64_t dm_present_late_count(void) { return test_late; }
static double xbg_recent_gpu_ms(void) { return 14; }
static int pace_adapt_enabled(void) { return 1; }
static const char *test_env(const char *key) { (void)key; return test_enabled ? "1" : "0"; }
static int test_strcmp(const char *a,const char *b) { while (*a && *a==*b) { ++a; ++b; } return (unsigned char)*a-(unsigned char)*b; }
#define getenv test_env
#define strcmp test_strcmp
static uint64_t g_pa_wait,g_pa_last;
static double g_pa_ema,g_pa_screen_hz;
static int g_pa_k=2,g_x_sync;
static _Atomic double g_pa_xcpu_ms=14;
#include "pace_adapt_tick_under_test.inc"
static void advance(unsigned frames,unsigned late) {
 for (unsigned i=0;i<frames;++i) { test_now+=100000000; test_cpu+=1000000; if(i<late)++test_late; pace_adapt_tick(60); }
}
int pace_actual_branch_test(void) {
 pace_adapt_tick(60); // initialize
 advance(1,0); advance(59,6); CHECK(g_pa_k==2); // backoff elapsed, pending must block up
 advance(60,0); CHECK(g_pa_k==1); // good window permits existing probe
 advance(80,0); CHECK(g_pa_k==1); // old probe skip+observation succeeds
 g_pa_k=2; test_cpu=0; pace_adapt_tick(60); advance(1,0); // clock reset
 advance(60,6); CHECK(g_pa_k==2); // pending
 test_cpu=1; test_now+=100000000; pace_adapt_tick(60); // backwards clock resets pending/window
 advance(60,6); CHECK(g_pa_k==2); // fresh first mild window
 advance(60,6); CHECK(g_pa_k==3); // second mild window down
 return 0;
}
#endif
