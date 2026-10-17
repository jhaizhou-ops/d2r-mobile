#pragma once
// Fixed direct-mapped table: collisions return UNKNOWN, never an invented epoch.
// Kept across arm/close, including when output is disabled; O(1), no allocation.
struct XbgProvenance {
    using U=unsigned long long;
    struct Pool { U address=0,epoch=0;bool live=false; } pools[256];
    U sequence=0,target=0,bytes=0;unsigned events=0,draws=0;bool armed=false;
    static unsigned slot(U a){return unsigned((a>>12)^(a>>24)^(a>>36))&255;}
    U pool(U address,bool live){U e=++sequence;pools[slot(address)]={address,e,live};return e;}
    U epoch(U address)const{const Pool &p=pools[slot(address)];return p.address==address&&p.live?p.epoch:0;}
    void arm(U present){target=present+2;bytes=0;events=0;draws=0;armed=true;}
    bool window(U present)const{return armed&&target==present+1;}
    static bool fits(U used,U n,U cap){return used<=cap&&n<=cap-used;}
    bool accept(unsigned n){if(events>=2048||!fits(bytes,n,256*1024-128))return false;++events;bytes+=n;return true;}
    bool draw(){if(draws>=64)return false;++draws;return true;}
};
