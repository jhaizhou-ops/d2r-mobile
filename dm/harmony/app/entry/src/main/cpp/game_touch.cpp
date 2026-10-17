#include "game_touch.h"
#include <mutex>
#include <dlfcn.h>
#include <time.h>
#include <cstdio>
#ifdef XBL_RUNTIME_EXECUTION
#include "touch_game_mapper.h"
#include "controller_bridge.h"
#include <cstring>
#include <cmath>
extern "C" {
#include "touch-core/ios/Sources/sw_touch_native_art.h"
}
namespace {
int (*skillTablesFn)(uint8_t *mapping72,uint32_t *ids32)=nullptr;   // runtime xblr_skill_tables (iOS binding tables, read-only)
// Icon on each skill key (iOS sw_ta_current_set): the game's button->slot map plus the bound skill ids. Bank 0 = first weapon set.
sw_ta_glyph skillGlyph(uint64_t button,int skillSet,bool *ok){
    *ok=false;sw_ta_glyph none{};none.status=SW_TA_UNKNOWN;
    uint8_t map[72];uint32_t ids[32];if(!skillTablesFn||!skillTablesFn(map,ids))return none;
    uint8_t bank[SW_TG_SLOTS*SW_TG_STRIDE]{};for(int i=0;i<SW_TG_SLOTS;i++)memcpy(bank+i*SW_TG_STRIDE,&ids[i],4);
    timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);const uint64_t now=uint64_t(ts.tv_sec)*1000000000ull+uint64_t(ts.tv_nsec);
    sw_tg_snapshot snap{};if(!sw_tg_parse(&snap,map,sizeof map,bank,sizeof bank,0,0,0,ids[0],now))return none;
    snap.ready=1;snap.identity_scope=SW_TG_IDENTITY_NATIVE;*ok=true;
    return sw_ta_current_set(&snap,button,now,skillSet);
}
std::mutex touchMutex;
XblGameTouchMapper mapper{};
decltype(&xblr_get_interaction_snapshot) snapshotFn=nullptr;
decltype(&xblr_get_input_samples) samplesFn=nullptr;
decltype(&xblr_world_tap) worldFn=nullptr;
decltype(&xblr_cancel_world) cancelFn=nullptr;
bool foreground=true;
unsigned worldReceipts=0;
uint32_t lastValid=UINT32_MAX,lastCaps=UINT32_MAX,lastMode=UINT32_MAX,lastNav=UINT32_MAX;
uint64_t lastCompleted=0;
unsigned contextReceipts=0;
// Full touch controls (iOS sw_touch_controls core): joystick, main/skill arc, skill-set swap, belt, menu, map.
// Outputs go to the game as a separate controller source (SetTouchController), never through the tap pulse queue.
bool touchControls=true;unsigned lastButtons=~0u;int lastAxes[4]{1,1,1,1};
// Switch controller HUD (not iOS NativeHUD v4): belt cells and the "+" cell measured on 2120x1320, in px of a 1320-high
// screen, relative to the horizontal centre. Belt columns map to D-pad left/up/down/right (iOS mapping).
bool hudV4=false;   // full iOS touch overlay loaded: the game draws the iOS bottom bar, so the core's own v4 geometry applies
void applyHudLayout(sw_tc_layout &l){
    if(hudV4){l.target[SW_TC_PORTAL].r=0;l.diff.r=0;for(auto &p:l.pick)p.r=0;return;}   // free town portal / difficulty strip need iOS host modules
    const float u=l.height/1320.0f,cx=l.width/2;
    const float belt[4]={109,189,267,347};
    for(int i=0;i<4;i++)l.target[SW_TC_BELT1+i]=sw_tc_target{cx+belt[i]*u,1259*u,36*u,l.target[SW_TC_BELT1+i].button};
    l.target[SW_TC_MENU]=sw_tc_target{cx,1259*u,36*u,l.target[SW_TC_MENU].button};
    l.target[SW_TC_PORTAL].r=0;l.diff.r=0;for(auto &p:l.pick)p.r=0;   // free town portal / difficulty strip need iOS host modules
    // The Switch HUD bar (orbs, skill row, belt) starts ~1100 px of 1320: keep the right cluster and the joystick above it.
    const float bar=1100*u;
    {const float bottom=l.swap.y+l.swap.r+4,dy=bottom>bar?bar-bottom:0;
     if(dy<0){for(int t=SW_TC_MAIN;t<=SW_TC_SKILL4;t++)l.target[t].y+=dy;l.target[SW_TC_PORTAL].y+=dy;l.swap.y+=dy;}}
    if(l.joy_max_y>bar-l.joy_radius)l.joy_max_y=bar-l.joy_radius;
    if(l.joy_y>l.joy_max_y)l.joy_y=l.joy_max_y;
}
void publishTouch(){
    unsigned b=0;int a[4]{};
    if(mapper.ready&&touchControls){const sw_tc_state &c=mapper.session.core;b=unsigned(c.buttons&0xffff);a[0]=c.lx;a[1]=c.ly;a[2]=c.rx;a[3]=c.ry;}
    if(b==lastButtons&&!memcmp(a,lastAxes,sizeof a))return;
    lastButtons=b;memcpy(lastAxes,a,sizeof a);SetTouchController(b,a);
}
uint64_t width=0,height=0;
double nowSeconds(){timespec ts{};if(clock_gettime(CLOCK_MONOTONIC,&ts))return -1;return double(ts.tv_sec)+double(ts.tv_nsec)*1e-9;}
void cancel(){
    const bool had_work=mapper.ready||mapper.world_request;
    XblGameTouchCancel(&mapper);
    if(had_work&&cancelFn)cancelFn();
    publishTouch();
}
int observe(){
    xblr_interaction_snapshot s{};
    if(!foreground||!snapshotFn||!width||!height){cancel();return 0;}
    const int rc_snapshot=snapshotFn(&s,sizeof s);
    if(rc_snapshot==47)return -1; // Lock busy: next normal callback retries.
    if(rc_snapshot){cancel();return 0;}
    if((s.valid!=lastValid||s.capabilities!=lastCaps||s.mode!=lastMode||s.navigation!=lastNav)&&contextReceipts++<128){
        fprintf(stderr,"[frontend-touch-context] abi=%u bytes=%u gen=%llu valid=%u cap=%u mode=%u nav=%u targets=%u\n",s.abi_version,s.struct_bytes,(unsigned long long)s.generation,s.valid,s.capabilities,s.mode,s.navigation,s.enabled_targets);
    }
    lastValid=s.valid;lastCaps=s.capabilities;lastMode=(s.valid&XBLR_INTERACTION_MODE)?s.mode:UINT32_MAX;lastNav=(s.valid&XBLR_INTERACTION_MODE)?s.navigation:UINT32_MAX;
    if(s.completed_request&&s.completed_request!=lastCompleted){
        lastCompleted=s.completed_request;
        if(worldReceipts++<128)fprintf(stderr,"[frontend-touch-world-result] serial=%llu expected=%llu matched=%d result=%d businessComplete=UNVERIFIED\n",(unsigned long long)s.completed_request,(unsigned long long)mapper.world_request,s.completed_request==mapper.world_request,s.request_result);
    }
    XblTouchContext c{};
    if(!XblTouchContextRead(&s,&c)){cancel();return 0;}
    if(c.mode==XBLR_INTERACTION_FRONTEND||(c.mode==XBLR_INTERACTION_GAME&&c.navigation!=0)){   // any open panel (inventory, skills, menus, death, NPC dialogue): direct UI taps
        cancel();return XblTouchContextAllows(&c,XBLR_INTERACTION_UI_TAP)?2:0;
    }
    const bool had_work=mapper.ready||mapper.world_request;
    /* World-only input uses a fixed logical height; it draws/captures no
     * invisible skill circles and publishes no joystick/controller outputs.
     * Full combat HUD and T3 will enable separate explicit routes later. */
    mapper.drawn_targets=touchControls?(((1u<<SW_TC_COUNT)-1)&~(1u<<SW_TC_PORTAL)):0;
    const int rc=XblGameTouchObserve(&mapper,&s,375.0f*float(width)/float(height),375.0f);
    if(rc){if(had_work&&cancelFn)cancelFn();publishTouch();return 0;}
    applyHudLayout(mapper.session.core.layout);
    return 1;
}
}
#endif
// Drawing snapshot for the ArkTS overlay (values only). Layout coordinates are logical points inside the observed UI rect.
void XblContentRect(float &x0,float &cw);
int GetTouchHud(double *o,int n){
#ifdef XBL_RUNTIME_EXECUTION
    std::lock_guard<std::mutex> lock(touchMutex);
    if(n<96)return 0;for(int i=0;i<n;i++)o[i]=0;
    if(!mapper.ready||!touchControls){o[0]=0;o[1]=lastMode==UINT32_MAX?0:lastMode;o[2]=lastNav==UINT32_MAX?0:lastNav;o[3]=hudV4;return 16;}   // not in combat: report mode/navigation for the overlay
    const sw_tc_state &c=mapper.session.core;const sw_tc_layout &l=c.layout;
    float cx0=0,ccw=1;XblContentRect(cx0,ccw);   // pillarbox: the core's logical space covers only the game image
    o[0]=1;o[1]=l.width;o[2]=l.height;o[3]=cx0+mapper.context.x*ccw;o[4]=mapper.context.y;o[5]=mapper.context.width*ccw;o[6]=mapper.context.height;
    o[7]=c.skill_set;o[8]=c.joy_slot>=0;o[9]=c.joy_origin_x;o[10]=c.joy_origin_y;
    if(c.joy_slot>=0){o[11]=c.finger[c.joy_slot].x;o[12]=c.finger[c.joy_slot].y;}
    o[13]=l.joy_x;o[14]=l.joy_y;o[15]=l.joy_radius;
    uint32_t pressed=0;for(int i=0;i<SW_TC_MAX_TOUCH;i++)if(c.finger[i].active&&c.finger[i].target>=0&&c.finger[i].target<SW_TC_COUNT)pressed|=1u<<c.finger[i].target;
    for(int t=0;t<SW_TC_COUNT;t++){double *e=o+16+t*5;e[0]=l.target[t].x;e[1]=l.target[t].y;e[2]=l.target[t].r;e[3]=(pressed>>t)&1;e[4]=(c.enabled_targets>>t)&1;}
    double *w=o+16+SW_TC_COUNT*5;w[0]=l.swap.x;w[1]=l.swap.y;w[2]=l.swap.r;w[3]=0;
    for(int i=0;i<SW_TC_MAX_TOUCH;i++)if(c.finger[i].active&&c.finger[i].target==SW_TC_SWAP)w[3]=1;
    // Skill key icons: {status (0 unknown, 1 empty, 2 glyph), atlas, frame} for main + skill 1-4; frame = pressed art while held.
    double *g=w+4;
    for(int t=SW_TC_MAIN;t<=SW_TC_SKILL4;t++){bool ok=false;const sw_ta_glyph gl=skillGlyph(l.target[t].button,c.skill_set,&ok);
        double *e=g+(t-SW_TC_MAIN)*3;e[0]=ok?gl.status:0;e[1]=gl.atlas;e[2]=((pressed>>t)&1)?gl.pressed_frame:gl.normal_frame;}
    o[95]=hudV4;   // full iOS touch overlay: bottom bar, belt and menu cross are drawn by the game
    return 96;
#else
    (void)o;(void)n;return 0;
#endif
}
void SetTouchHudV4(bool on){
#ifdef XBL_RUNTIME_EXECUTION
    hudV4=on;
#else
    (void)on;
#endif
}
void SetTouchControlsEnabled(bool on){
#ifdef XBL_RUNTIME_EXECUTION
    std::lock_guard<std::mutex> lock(touchMutex);if(touchControls!=on){touchControls=on;cancel();}
#else
    (void)on;
#endif
}
void AttachGameTouchRuntime(void *handle){
#ifdef XBL_RUNTIME_EXECUTION
    std::lock_guard<std::mutex> lock(touchMutex);cancel();
    snapshotFn=reinterpret_cast<decltype(snapshotFn)>(dlsym(handle,"xblr_get_interaction_snapshot"));
    samplesFn=reinterpret_cast<decltype(samplesFn)>(dlsym(handle,"xblr_get_input_samples"));
    worldFn=reinterpret_cast<decltype(worldFn)>(dlsym(handle,"xblr_world_tap"));
    cancelFn=reinterpret_cast<decltype(cancelFn)>(dlsym(handle,"xblr_cancel_world"));
    skillTablesFn=reinterpret_cast<decltype(skillTablesFn)>(dlsym(handle,"xblr_skill_tables"));
    fprintf(stderr,"[frontend-touch-route] snapshot=%d samples=%d world=%d cancel=%d controls=ON T3=OFF\n",!!snapshotFn,!!samplesFn,!!worldFn,!!cancelFn);
#else
    (void)handle;
#endif
}
void SetGameTouchSurface(uint64_t w,uint64_t h){
#ifdef XBL_RUNTIME_EXECUTION
    std::lock_guard<std::mutex> lock(touchMutex);if(w&&h){width=w;height=h;}
#else
    (void)w;(void)h;
#endif
}
void CancelGameTouch(){
#ifdef XBL_RUNTIME_EXECUTION
    std::lock_guard<std::mutex> lock(touchMutex);cancel();
#endif
}
void SetGameTouchForeground(bool value){
#ifdef XBL_RUNTIME_EXECUTION
    std::lock_guard<std::mutex> lock(touchMutex);foreground=value;if(!value)cancel();
#else
    (void)value;
#endif
}
void TickGameTouch(){
#ifdef XBL_RUNTIME_EXECUTION
    std::lock_guard<std::mutex> lock(touchMutex);
    if(observe()!=1||!samplesFn)return;
    XblGameTouchTick(&mapper,samplesFn(),nowSeconds());
    publishTouch();
#endif
}
int RouteGameTouch(unsigned phase,int32_t id,float x,float y,uint64_t w,uint64_t h,int64_t ns){
#ifdef XBL_RUNTIME_EXECUTION
    std::lock_guard<std::mutex> lock(touchMutex);width=w;height=h;
    if(phase==3){cancel();return 0;}
    const int route=observe();if(route==2)return 1;
    if(route<0){
        // Never turn a release observed during lock contention into a delayed
        // world click. Release only its own core finger; retain other owners.
        if(phase==2)XblTouchCoreEvent(&mapper.session,3,id,0,0,0);
        return 0;
    }
    if(route!=1||!w||!h||ns<0)return 0;
    const uint64_t old=mapper.world_request;
    const int rc=XblGameTouchEvent(&mapper,phase,id,x/float(w),y/float(h),double(ns)*1e-9,worldFn);
    publishTouch();
    if((mapper.world_request!=old||rc)&&worldReceipts++<128)
        fprintf(stderr,"[frontend-touch-world] id=%d phase=%u rc=%d accepted=%llu businessComplete=UNVERIFIED\n",id,phase,rc,(unsigned long long)mapper.world_request);
    return 0;
#else
    (void)phase;(void)id;(void)x;(void)y;(void)w;(void)h;(void)ns;return 1;
#endif
}
