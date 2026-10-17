#include "touch_core_session.h"

static int finite_time(double t){return t>=0&&t<=1.7976931348623157e308;}
void XblTouchCoreCancel(XblTouchCoreSession *s){
    if(!s)return;
    if(s->initialized)sw_tc_cancel_all(&s->core);
    XblFingerCancelAll(&s->slots);
}
int XblTouchCoreConfigure(XblTouchCoreSession *s,float w,float h,int ready,uint32_t targets){
    if(!s)return 40;
    if(!(w>0&&h>0&&w<=16384&&h<=16384)||targets&~((1u<<SW_TC_COUNT)-1)){
        XblTouchCoreCancel(s);s->game_input_ready=0;return 40;
    }
    if(!s->initialized){sw_tc_init(&s->core,w,h);s->initialized=1;}
    if(s->width!=w||s->height!=h){XblTouchCoreCancel(s);sw_tc_resize(&s->core,w,h);}
    if(s->game_input_ready!=!!ready||s->targets!=targets)XblTouchCoreCancel(s);
    s->width=w;s->height=h;s->targets=targets;s->game_input_ready=!!ready;
    sw_tc_set_navigation_mode(&s->core,0);
    sw_tc_enable_targets(&s->core,ready?targets:0);
    if(!ready)XblTouchCoreCancel(s);
    return 0;
}
int XblTouchCoreEvent(XblTouchCoreSession *s,unsigned phase,int32_t id,float x,float y,double time){
    if(!s||!s->initialized)return 47;
    int slot=XblFingerFind(&s->slots,id);
    if(phase==XBL_TOUCH_CANCEL){
        if(slot>=0){sw_tc_cancel_finger(&s->core,slot);XblFingerFinish(&s->slots,slot,s->slots.tickets[slot]);}
        return 0;
    }
    if(phase>XBL_TOUCH_CANCEL||!finite_time(time)||time<s->last_event_s||
       !(x>=0&&x<=s->width&&y>=0&&y<=s->height)){
        if(slot>=0){sw_tc_cancel_finger(&s->core,slot);XblFingerFinish(&s->slots,slot,s->slots.tickets[slot]);}
        return 40;
    }
    if(!s->game_input_ready)return 47;
    if(phase==XBL_TOUCH_DOWN){
        uint64_t ticket=0;slot=XblFingerBegin(&s->slots,id,&ticket);if(slot<0)return 47;
        sw_tc_begin_at(&s->core,slot,x,y,time);
    }else{
        if(slot<0)return 47;
        if(phase==XBL_TOUCH_MOVE)sw_tc_move_at(&s->core,slot,x,y,time);
        else{
            /* Up includes the final event location; do not use the last frame. */
            sw_tc_move_at(&s->core,slot,x,y,time);sw_tc_end_at(&s->core,slot,time);
            XblFingerFinish(&s->slots,slot,s->slots.tickets[slot]);
        }
    }
    s->last_event_s=time;return 0;
}
int XblTouchCoreTick(XblTouchCoreSession *s,double time){
    if(!s||!s->initialized||!s->game_input_ready)return 47;
    if(!finite_time(time)||time<s->last_event_s)return 40;
    /* Tick time never replaces event timestamps for tap/hold classification. */
    sw_tc_tick(&s->core,time);return 0;
}
