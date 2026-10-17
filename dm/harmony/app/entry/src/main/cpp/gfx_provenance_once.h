#pragma once
#include <stdint.h>

// Called only after synchronous Session::present returns (Session lock released),
// on the guest callback thread. No API calls from UI/control/watchdog threads.
struct XblProvenanceOnce {
    bool attempted=false,active=false;
    uint64_t armed_ns=0;
    template<class Consume,class Arm,class Close,class Receipt>
    void poll(uint64_t now,Consume consume,Arm arm,Close close,Receipt receipt){
        if(active&&now>=armed_ns&&now-armed_ns>=10000000000ULL){
            receipt("CLOSE_REQUEST",0,now);
            const int rc=close(); // May block: external watchdog is separate.
            active=false;receipt("CLOSE_RETURN",rc,now);
        }
        if(attempted||!consume())return;
        attempted=true;receipt("ARM_REQUEST",0,now);
        const int rc=arm();receipt("ARM_RETURN",rc,now);
        active=rc==0;armed_ns=now;
    }
};
