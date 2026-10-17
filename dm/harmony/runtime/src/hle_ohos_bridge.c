#include "hle_ohos_compat.h"
#include <time.h>
#ifdef XBLR_EXECUTION_ENABLED
#include "execution_internal.h"
#endif
struct xblr_hle_flags g_flag;
uint64_t g_t0_ns;
#ifndef XBLR_FULL_SERVICES
uint64_t now_ns(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts))die("CLOCK_MONOTONIC failed");
    return (uint64_t)ts.tv_sec*1000000000u+(uint64_t)ts.tv_nsec;
}
#endif
void *guest_sym(const char *name) {
#ifdef XBLR_EXECUTION_ENABLED
    void *p=xblr_execution_symbol(name);
    if(!p)die("guest_sym called outside an initialized guest thread: %s",name);
    return p;
#else
    // This binding milestone embeds ORIGINAL bytes as NX. An HLE asking to call
    // the SDK must fail loudly until the Harmony rewriter/thread gate is installed.
    die("guest execution blocked (ORIGINAL_UNREWRITTEN): %s",name);
#endif
}
