#pragma once
#include <stdint.h>

// UI presses use real guest getter samples, never timer ticks as consumption.
// Physical controllers and analog axes retain their original continuous path.
class XblButtonPulses {
    struct Key {
        bool down=false, active=false, gap=false, seen=false;
        unsigned queued=0;
        uint64_t samples=0;
        double began=0, firstSeen=0;
    } keys[16];
    static void begin(Key &k,uint64_t samples,double now) {
        k.active=true;k.gap=false;k.seen=false;k.samples=samples;k.began=now;
    }
public:
    void clear() { for(auto &k:keys)k=Key{}; }
    // Movement cancels presses waiting in the queue (walking interrupts attacks immediately).
    void dropQueued() { for(auto &k:keys)k.queued=0; }
    void request(unsigned mask,uint64_t samples,double now) {
        for(unsigned i=0;i<16;++i) {
            auto &k=keys[i];const bool down=(mask&(1u<<i))!=0;
            if(down&&!k.down) {
                if(!k.active&&!k.gap)begin(k,samples,now);
                else if(k.queued<0)++k.queued; // no replay of extra taps: rapid tapping must not keep attacking after release
            }
            k.down=down;
        }
    }
    unsigned tick(uint64_t samples,double now) {
        unsigned mask=0;
        for(unsigned i=0;i<16;++i) {
            auto &k=keys[i];
            if(k.active) {
                if(!k.seen&&samples>k.samples){k.seen=true;k.firstSeen=now;}
                const bool taken=k.seen&&samples-k.samples>=2&&now-k.firstSeen>=0.06; // short tap: 2 real samples / 60 ms
                if(!k.down&&(taken||now-k.began>=2.0)) {
                    k.active=false;k.gap=true;k.samples=samples;k.began=now;
                }
            } else if(k.gap&&(samples-k.samples>=2||now-k.began>=0.5)) {
                k.gap=false;
                if(k.queued){--k.queued;begin(k,samples,now);}
            }
            if(k.active)mask|=1u<<i;
        }
        return mask;
    }
};
