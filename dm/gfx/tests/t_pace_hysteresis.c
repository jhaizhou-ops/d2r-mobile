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
// H-13 B: probe-up needs CPU headroom (87% of the faster period), except once the stable period reaches the cap; backoff doubles to the cap.
int pace_probe_gate_test(void) {
 const unsigned long long cap=NVN_PACE_PROBE_CAP_NS, s3=3000000000ull;
 CHECK(nvn_pace_probe_allowed(16.667,12.0,14.0,s3,cap));     // both under 14.5 ms
 CHECK(!nvn_pace_probe_allowed(16.667,12.0,19.7,s3,cap));    // H-11 camp translation 19.7 ms blocks the 60 probe
 CHECK(!nvn_pace_probe_allowed(16.667,15.0,10.0,s3,cap));    // game thread over the limit
 CHECK(nvn_pace_probe_allowed(16.667,12.0,-1,s3,cap));       // no async translation reading: game side only
 CHECK(!nvn_pace_probe_allowed(16.667,-1,-1,s3,cap));        // no reading at all: wait
 CHECK(nvn_pace_probe_allowed(16.667,30.0,30.0,cap,cap));    // cap reached: probe anyway (no lock-in)
 CHECK(!nvn_pace_probe_allowed(0,1,1,s3,cap));
 CHECK(nvn_pace_probe_allowed(25.0,21.0,21.5,s3,cap));       // 40 fps probe from 30: limit 21.75 ms
 CHECK(nvn_pace_backoff_next(s3,cap)==6000000000ull);
 unsigned long long b=s3; for(int i=0;i<10;i++) b=nvn_pace_backoff_next(b,cap);
 CHECK(b==cap);
 CHECK(nvn_pace_backoff_next(cap,cap)==cap);
 CHECK(nvn_pace_backoff_next(48000000000ull,60000000000ull)==60000000000ull);   // old cap still honoured when the gate is off
 return 0;
}
int main(void) { int r=pace_hysteresis_test(); return r?r:pace_probe_gate_test(); }

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
int (*g_nvn_loadscreen_up)(void);
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
// H-13 B on the real controller: with translation at 19.7 ms the 60 probe waits (no hiccup every few seconds), then fires once the
// stable period reaches the 120 s cap; with headroom it probes after the normal 3 s backoff. Frames here are 100 ms apart.
int pace_gate_branch_test(void) {
 g_pa_k=2; g_pa_xcpu_ms=19.7; advance(60,0); advance(60,0);            // settle at 30 (k=2 at 60 Hz) after the previous test
 unsigned waited=0; while (g_pa_k==2 && waited<1500) { advance(60,0); waited+=60; }
 CHECK(g_pa_k==1); CHECK(waited>=960 && waited<=1320);                   // probed only near the 120 s cap (1200 frames), not after the 3 s backoff
 advance(80,0); CHECK(g_pa_k==1);                                        // probe held without misses
 g_pa_k=2; g_pa_xcpu_ms=12.0; advance(60,0);
 waited=0; while (g_pa_k==2 && waited<1200) { advance(60,0); waited+=60; }
 CHECK(g_pa_k==1); CHECK(waited<=180);                                   // headroom: probe after the short backoff
 return 0;
}
#endif
