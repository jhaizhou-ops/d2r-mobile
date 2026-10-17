#include "touch_core_pulse.h"
#include <string.h>

void XblCorePulseClear(XblCorePulse *p){if(p)memset(p,0,sizeof(*p));}
int XblCorePulsePoll(XblCorePulse *p,sw_tc_state *s,uint64_t n,double now,int pending){
    if(!p||!s||!(now>=0&&now<=1.7976931348623157e308)||
       !(s->pulse_hold_s>=0&&s->pulse_hold_s<=1.7976931348623157e308))return XBL_PULSE_INVALID;
    /* Reset on lifecycle/runtime generation changes before accepting new data.
     * Reject rollback instead of unsigned-underflow pretending consumption. */
    if(p->initialized&&(n<p->last_samples||now<p->last_now))return XBL_PULSE_INVALID;
    p->initialized=1;p->last_samples=n;p->last_now=now;
    if(s->pulse_button){
        if(!p->tracking||p->epoch!=s->pulse_epoch){
            p->tracking=1;p->epoch=s->pulse_epoch;p->samples=n;p->began=now;
            p->seen=0;p->freed=0;p->gap=0;
        }
        if(!p->seen&&n>p->samples){p->seen=1;p->first_seen=now;}
        /* Non-pointer skill pulses do not need an unavailable cursor API. */
        const int free_now=s->pointer_pulse_active?pending==0:1;
        if(p->seen&&!p->freed&&free_now){p->freed=1;p->first_free=now;}
        const double hold=s->pulse_hold_s;
        const int taken=p->seen&&n-p->samples>=3&&now-p->first_seen>=0.12&&
                        free_now&&(hold<=0||(p->freed&&now-p->first_free>=hold));
        if(taken||now-p->began>2.0+hold){
            if(!sw_tc_pulse_end(s,p->epoch))return XBL_PULSE_INVALID;
            p->tracking=0;p->gap=1;p->samples=n;p->began=now;
            return taken?XBL_PULSE_RELEASED:XBL_PULSE_TIMEOUT;
        }
    }else if(sw_tc_pulse_pending(s)){
        /* No known release publication: start a fresh release gap now. */
        if(!p->gap){p->tracking=0;p->gap=1;p->samples=n;p->began=now;}
        if(n-p->samples>=2||now-p->began>0.5){
            if(!sw_tc_pulse_resume(s))return XBL_PULSE_INVALID;
            /* Resume output must be published before measuring consumption. */
            p->tracking=1;p->gap=0;p->epoch=s->pulse_epoch;p->samples=n;p->began=now;
            p->seen=0;p->freed=0;
            return XBL_PULSE_RESUMED;
        }
    }else{p->tracking=0;p->gap=0;}
    return XBL_PULSE_IDLE;
}
