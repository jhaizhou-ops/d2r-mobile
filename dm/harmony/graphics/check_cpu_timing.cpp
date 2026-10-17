#include "xbg_cpu_timing.h"
static uint64_t now,reads;
static uint64_t clock_read(){++reads;return now;}
extern "C" int xbg_cpu_timing_selftest(){
    now=reads=0;XbgCpuTiming t(clock_read);
    {XbgCpuTiming::Scope disabled(nullptr,XbgCpuTiming::Draw);}
    if(reads)return 1; // default-off must not read the clock
    {
        XbgCpuTiming::Scope outer(&t,XbgCpuTiming::Draw);
        now=10;
        {XbgCpuTiming::Scope child(&t,XbgCpuTiming::Buffer);now=30;}
        now=40;
        {XbgCpuTiming::Scope child(&t,XbgCpuTiming::Descriptor);
            now=50;{XbgCpuTiming::Scope nested(&t,XbgCpuTiming::Buffer);now=70;}now=80;}
        now=100;
    }
    if(t.top || reads!=8)return 2;
    auto &d=t.counters[XbgCpuTiming::Draw];auto &b=t.counters[XbgCpuTiming::Buffer];auto &s=t.counters[XbgCpuTiming::Descriptor];
    if(d.calls!=1 || d.inclusive_ns!=100 || d.exclusive_ns!=40 || d.max_ns!=100)return 3;
    if(b.calls!=2 || b.inclusive_ns!=40 || b.exclusive_ns!=40 || b.max_ns!=20)return 4;
    if(s.calls!=1 || s.inclusive_ns!=40 || s.exclusive_ns!=20 || s.max_ns!=40)return 5;
    // Same-kind recursion must also be exclusive, without double counting.
    now=110;{XbgCpuTiming::Scope a(&t,XbgCpuTiming::Other);now=120;
        {XbgCpuTiming::Scope b2(&t,XbgCpuTiming::Other);now=140;}now=150;}
    if(t.counters[XbgCpuTiming::Other].exclusive_ns!=40 || t.counters[XbgCpuTiming::Other].calls!=2)return 6;
    return 0;
}
