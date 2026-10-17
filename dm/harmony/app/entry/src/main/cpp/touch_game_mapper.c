#include "touch_game_mapper.h"

void XblGameTouchCancel(XblGameTouchMapper *m){
    if(!m)return;
    XblTouchCoreCancel(&m->session);XblCorePulseClear(&m->pulse);
    m->ready=0;m->world_request=0;m->world_seen=m->session.core.world_tap_seq;
}
int XblGameTouchObserve(XblGameTouchMapper *m,const xblr_interaction_snapshot *s,float w,float h){
    if(!m)return 40;
    XblTouchContext c;
    if(!XblTouchContextRead(s,&c)||c.mode!=XBLR_INTERACTION_GAME||c.navigation||
       !XblTouchContextAllows(&c,XBLR_INTERACTION_INPUT)){
        XblGameTouchCancel(m);return 47;
    }
    /* Generation is a publication sequence, not a scene epoch. A normal new
     * publication must not release every held finger. Actual context changes do. */
    if(m->ready&&(w!=m->session.width||h!=m->session.height||c.generation<m->context.generation||
       c.capabilities!=m->context.capabilities||c.targets!=m->context.targets||
       c.x!=m->context.x||c.y!=m->context.y||c.width!=m->context.width||c.height!=m->context.height))
        XblGameTouchCancel(m);
    int was_ready=m->ready;
    if(XblTouchCoreConfigure(&m->session,w,h,1,c.targets&m->drawn_targets)){XblGameTouchCancel(m);return 40;}
    if(!was_ready){XblCorePulseClear(&m->pulse);m->world_seen=m->session.core.world_tap_seq;}
    m->context=c;m->ready=1;return 0;
}
int XblGameTouchEvent(XblGameTouchMapper *m,unsigned phase,int32_t id,float nx,float ny,double t,XblWorldTapRequest request){
    if(!m||!m->ready)return 47;
    float x=0,y=0;
    if(phase!=XBL_TOUCH_CANCEL&&!XblTouchContextPoint(&m->context,nx,ny,&x,&y)){
        XblTouchCoreEvent(&m->session,XBL_TOUCH_CANCEL,id,0,0,t);return 40;
    }
    int rc=XblTouchCoreEvent(&m->session,phase,id,x*m->session.width,y*m->session.height,t);
    if(rc)return rc;
    sw_tc_state *core=&m->session.core;
    if(core->world_tap_seq!=m->world_seen){
        m->world_seen=core->world_tap_seq;
        if(!request||!XblTouchContextAllows(&m->context,XBLR_INTERACTION_WORLD_TAP))return 47;
        /* World API expects full rendered-content normalized coordinates,
         * while the gesture core uses logical coordinates inside observed rect. */
        const float wx=m->context.x+core->world_tap_x/m->session.width*m->context.width;
        const float wy=m->context.y+core->world_tap_y/m->session.height*m->context.height;
        uint64_t serial=0;rc=request(wx,wy,&serial);
        if(!rc)m->world_request=serial;
        return rc;
    }
    return 0;
}
int XblGameTouchTick(XblGameTouchMapper *m,uint64_t n,double now){
    if(!m||!m->ready)return 47;
    int rc=XblTouchCoreTick(&m->session,now);if(rc)return rc;
    /* Current candidate has no pointer/T3 delivery API. Unknown pointer pulses
     * cannot be reported as consumed. Ordinary combat pulses still work. */
    return XblCorePulsePoll(&m->pulse,&m->session.core,n,now,-1);
}
