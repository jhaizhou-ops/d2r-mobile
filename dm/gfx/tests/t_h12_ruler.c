/* Host regression for the timing arithmetic and error paths of both lanes. */
#include <assert.h>
#include <stdio.h>
#include <time.h>
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif

static struct timespec observations[2];
static int attempts, fail_at;

static int fake_cpu_clock(int which, struct timespec *out) {
    assert(which == CLOCK_THREAD_CPUTIME_ID && attempts < 2);
    *out = observations[attempts++];
    return attempts == fail_at ? -1 : 0;
}
#define clock_gettime fake_cpu_clock
#include "../../harmony/native/h12_measure.c"

int main(void) {
    observations[0] = (struct timespec){.tv_sec=2, .tv_nsec=999999900};
    observations[1] = (struct timespec){.tv_sec=3, .tv_nsec=300};
    assert(dm_h12_ruler_ns() == 400 && attempts == 2);
    attempts = 0; observations[1] = observations[0];
    assert(dm_h12_ruler_ns() == 0);
    attempts = 0; observations[1].tv_sec--;
    assert(dm_h12_ruler_ns() == 0);
    attempts = 0; fail_at = 1;
    assert(dm_h12_ruler_ns() == 0 && attempts == 1);
    attempts = 0; fail_at = 2;
    assert(dm_h12_ruler_ns() == 0 && attempts == 2);
    puts("PASS ruler: second rollover, frozen/reversed clock, failed start/end reads");
    return 0;
}
