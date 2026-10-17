/* Android adapter for the unchanged upstream portable touch state machine.
 * No game-version addresses or Apple UI/skill observation hooks are linked. */
#include "../ios/Sources/dm_touch_controls.h"
#include "input_backend.h"
#include "ui_state_android.h"
#include "../host/dm_cursor.h"
#include "../host/dm_actions.h"
#include "../host/dm_hud.h"
#include "../ios/Sources/dm_touch_native_art.h"
#include "../host/dm.h"
#include <jni.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static dm_tc_state controls;
static struct {int active,id;} slots[16];
static int ready,enabled;
static uint64_t pulse_epoch,pulse_samples;
static double pulse_start,pulse_seen;
static uint64_t last_buttons;
static int32_t last_axes[4];
static int published;
static int access_slot=-1;
static double access_until;

static double seconds(void) {return now_ns()/1e9;}
static void restrict_layout(void) {
    /* These upstream widgets require an audited original-game bridge. Keep
     * them absent until 91738 has native binding/scene identities. */
    controls.layout.target[DM_TC_PORTAL].r=0;
    controls.layout.diff.r=0;
    for(int i=0;i<8;i++)controls.layout.pick[i].r=0;
    controls.belt_rows=1;
}
/* UI 状态机（P1）：游戏态掩码 = 全部除回城（地图键暂时保留，等 P2 接了"点小地图开图"再按 iOS 去掉）；
 * 面板态（对齐 front_ios.m:183-185）：导航态不给五个手柄圆钮（dm_tc_nav_mask(0)=0），只开对话菜单（3）时给底栏十字。 */
static uint32_t game_mask(void){return ((1u<<DM_TC_COUNT)-1)&~(1u<<DM_TC_PORTAL);}
static unsigned applied_rev=0xffffffffu;static uint32_t applied_mask;
static int applied_nav;
/* P2：模型序号 → dm_ui_* 派发（对照 front_ios.m:622-649 的序号差分） | P2: model sequences -> dm_ui_* dispatch (front_ios.m:622-649) */
static unsigned last_seq[8];
static void dispatch_seqs(void) {
    void dm_ui_world_tap(float,float,float,float); void dm_ui_item_tap(void); void dm_ui_use_item(void); void dm_ui_long_press(void);
    void dm_ui_identify_hovered(void); void dm_ui_tab_go(void); void dm_stash_take(int);
    unsigned cur[8]={controls.item_tap_seq,controls.use_seq,controls.long_press_seq,controls.ident_seq,
                     controls.tab_tap_seq,controls.world_tap_seq,controls.stack_take_seq,controls.mouse_click_seq};
    if(cur[0]!=last_seq[0])dm_ui_item_tap();
    if(cur[1]!=last_seq[1])dm_ui_use_item();
    if(cur[2]!=last_seq[2])dm_ui_long_press();
    if(cur[3]!=last_seq[3])dm_ui_identify_hovered();
    if(cur[4]!=last_seq[4])dm_ui_tab_go();
    if(cur[5]!=last_seq[5]){float nx=controls.world_tap_x/(controls.layout.width>0?controls.layout.width:1),
                                   ny=controls.world_tap_y/(controls.layout.height>0?controls.layout.height:1);
        dm_ui_world_tap(nx,ny,nx,ny);}
    if(cur[6]!=last_seq[6])dm_stash_take(1);
    if(cur[7]!=last_seq[7]){   /* 面板内非物品区点按 = 界面鼠标点击（暂停菜单按钮、布局里插回的关闭/修理等只认鼠标；对齐 front_ios.m:649） */
        float w=controls.layout.width>0?controls.layout.width:1.f,h=controls.layout.height>0?controls.layout.height:1.f;
        float nx=controls.mouse_click_x/w,ny=controls.mouse_click_y/h;
        dm_ui_touch(0,nx,ny);dm_ui_touch(2,nx,ny);
    }
    { static unsigned dbg; unsigned diff=0; for(int i=0;i<8;i++)diff|=(cur[i]!=last_seq[i])<<i;
      if(diff&&dbg++<64)LOG("TOUCH_SEQ 派发 diff=%#x item=%u use=%u long=%u ident=%u tab=%u world=%u stack=%u click=%u",diff,cur[0],cur[1],cur[2],cur[3],cur[4],cur[5],cur[6],cur[7]); }
    memcpy(last_seq,cur,sizeof cur);
}
/* 面板态"挂起抬起"（对齐 front_ios.m:518-520 / 601-650）：
 *   落下：指针手指问宿主「这是不是物品区」（主线程下一帧作答）。
 *   移动：起手定型出滚动/滑块 → dm_ui_drag_begin/move（游戏自己的滚轮 / 滑块）；拖放中按"物品抬在手指上方"问落点。
 *   松手：答案没回来就先挂起这一根（0.25 秒兜底），回来后把分类喂回模型（dm_tc_pointer_item），
 *         拖放的按松手这一刻的位置重问落点 → dm_tc_pointer_release[_at]。
 * 喂回模型之后，双击（使用 / 装备 / 免费鉴定 / 取一件）、长按（补充到腰带）、单击（鉴定态鉴定）、页签、点面板外关面板
 * 全都由模型自己判出来，序号派发（dispatch_seqs）照旧把它们发成 dm_ui_*。 */
static unsigned pend_q; static int pend_slot=-1; static int pend_up; static double pend_at,pend_end_at;
static unsigned pend_rel; static int pend_rel_asked; static int ui_drag_on; static float lift_x, lift_y; static int item_fed;
/* 命中答案一回来就告诉手势层（只有物品区驱动手柄光标、才算拖放）——对齐 front_ios.m:426 */
static void feed_item_kind(void) {
    if(!pend_q||item_fed)return;
    int ir=dm_ui_item_result(pend_q);
    if(ir>=0){dm_tc_pointer_item(&controls,ir);item_fed=1;}
}
static void resolve_pending(void) {
    if(pend_slot<0||!pend_up)return;
    int slot=pend_slot; double now=seconds();
    float w=controls.layout.width>0?controls.layout.width:1.f, h=controls.layout.height>0?controls.layout.height:1.f;
    dm_tc_finger *pf=&controls.finger[slot];
    if(ui_drag_on){ui_drag_on=0;dm_ui_drag_end();}
    int wait=0;
    if(pend_q&&dm_ui_item_result(pend_q)<0)wait=1;
    else if(!pend_rel_asked&&pf->pointer_dragged){   /* 抬起点可能比最后一次移动更远：按松手这一刻的位置再问一次 */
        float ex=lift_x>=0?lift_x*w:pf->x, ey=lift_y>=0?lift_y*h:pf->y;
        pend_rel=dm_ui_drop_query(ex/w,(ey-DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(w,h))/h,ey/h);
        pend_rel_asked=1;wait=1;
    }
    if(wait&&now-pend_end_at<0.25)return;
    if(pend_rel_asked){
        int rr=dm_ui_item_result(pend_rel); if(rr<0)rr=dm_ui_drop_last(); if(rr<0)rr=0;
        if(pf->shop_drag){   /* 从货架起的拖：落在背包类物品区 = 买光标选中的那件；别处松手不买 */
            if(rr==5&&!dm_ui_drop_bag())LOG("TOUCH_P2 货架拖出松在背包格以外 → 不买");
            else if(rr==5||rr==11){
                float ox=pf->aim_origin_x/w, oy=pf->aim_origin_y/h;
                if(rr==11){dm_ui_stack_cube_at(ox,oy);LOG("TOUCH_P2 一叠拖到魔盒格 → 1 件进魔盒");}
                else if(rr==5){controls.pointer_x=pf->aim_origin_x;controls.pointer_y=pf->aim_origin_y;   /* 光标目标也改回那件货 */
                LOG("TOUCH_P2 货架的货拖到背包松手 → 买");dm_ui_shop_buy_at(ox,oy);}
            }
        }else if(pf->pointer_dragged){
            float dx=0,dy=0;
            if((rr==5&&dm_ui_drop_point(&dx,&dy))||(rr==0&&dm_ui_origin_point(&dx,&dy)))
                dm_tc_pointer_release_at(&controls,dx*w,dy*h);
            else { if(rr==7){dm_shop_sell_cursor();rr=0;} dm_tc_pointer_release(&controls,rr); }
        }
    }
    if(pend_q&&!item_fed){int ir=dm_ui_item_result(pend_q);dm_tc_pointer_item(&controls,ir<0?0:ir);}
    dm_tc_end_at(&controls,slot,pend_end_at>0?pend_end_at:now);   /* 抬起时刻按真正松手：双击 / 长按判定不受挂起影响 */
    pend_q=0;pend_rel=0;pend_rel_asked=0;pend_up=0;pend_slot=-1;pend_end_at=0;item_fed=0;
}
static void apply_ui_locked(void) {
    unsigned rev=dm_android_ui_rev();
    if(rev==applied_rev)return;
    applied_rev=rev;
    int nm=dm_android_ui_panel_mode();
    dm_tc_set_navigation_mode(&controls,nm!=0);
    dm_tc_set_joy_block(&controls,nm==3);
    applied_nav=nm!=0?1:0;
    applied_mask=nm?(nm==3?(1u<<DM_TC_MENU):0u):game_mask();
    dm_tc_enable_targets(&controls,applied_mask);
    LOG("TOUCH_UI_MODE panel=%d mask=%#x frontend=%d",nm,applied_mask,dm_android_ui_frontend());
}
static int publish(void) {
    { void dm_cursor_set_screen(float,float,float,float); void dm_cursor_clear(void);
      if(controls.pointer_valid&&controls.layout.width>0&&controls.layout.height>0)
          dm_cursor_set_screen(controls.pointer_x/controls.layout.width,controls.pointer_y/controls.layout.height,1.f,1.f);
      else dm_cursor_clear(); }
    int32_t axes[4]={controls.lx,controls.ly,controls.rx,controls.ry};
    if(published && controls.buttons==last_buttons && !memcmp(axes,last_axes,sizeof axes))return 0;
    int rc=dm_android_input_publish_actions(controls.buttons,axes[0],axes[1],axes[2],axes[3]);
    if(!rc){last_buttons=controls.buttons;memcpy(last_axes,axes,sizeof axes);published=1;
        static unsigned logged;if(logged++<128)LOG("ANDROID_UPSTREAM_TOUCH buttons=%#llx axes=%d,%d,%d,%d set=%d",(unsigned long long)controls.buttons,axes[0],axes[1],axes[2],axes[3],controls.skill_set);}
    return rc;
}
static void cancel(void) {
    if(ready)dm_tc_cancel_all(&controls);
    memset(slots,0,sizeof slots);pulse_epoch=0;published=0;
    access_slot=-1;pend_slot=-1;pend_up=0;pend_q=0;pend_rel=0;pend_rel_asked=0;ui_drag_on=0;item_fed=0;
    dm_android_input_cancel();
}
static void tick(void) {
    if(!ready || !enabled)return;
    feed_item_kind();resolve_pending();
    double now=seconds();dm_android_input_status input;dm_android_input_status_get(&input);
    if(access_slot>=0 && now>=access_until){dm_tc_end_at(&controls,access_slot,now);access_slot=-1;}
    dm_tc_tick(&controls,now);
    if(controls.pulse_button) {
        if(pulse_epoch!=controls.pulse_epoch){pulse_epoch=controls.pulse_epoch;pulse_samples=input.npad_samples;pulse_start=now;pulse_seen=0;}
        if(!pulse_seen && input.npad_samples>pulse_samples)pulse_seen=now;
        double hold=fmax(0.12,controls.pulse_hold_s);
        if((pulse_seen && now-pulse_seen>=hold && input.npad_samples-pulse_samples>=3) || now-pulse_start>2.0+hold) {
            dm_tc_pulse_end(&controls,pulse_epoch);pulse_samples=input.npad_samples;pulse_start=now;
        }
    }else if(dm_tc_pulse_pending(&controls) && (input.npad_samples-pulse_samples>=2 || now-pulse_start>0.5))dm_tc_pulse_resume(&controls);
    if(controls.belt_request){int br=controls.belt_request;controls.belt_request=0;
        void dm_belt_expand(int);void dm_belt_use(int,int);
        if(br==1||br==2)dm_belt_expand(br==1);else dm_belt_use(br&0xf,br>>4&0xf);}
    (void)publish();
    dispatch_seqs();
    { int any=0;for(int i=0;i<16;i++)any|=controls.finger[i].active;
      dm_ui_hand_idle(!any&&!controls.pulse_button&&!dm_cursor_pending());
      dm_tc_set_hover_unid(dm_ui_hover_unid()); }
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeResize(JNIEnv *env,jclass cls,jfloat w,jfloat h,jfloat left,jfloat right) {
    (void)env;(void)cls;if(!isfinite(w)||!isfinite(h)||w<=0||h<=0)return;
    pthread_mutex_lock(&lock);dm_tc_set_tall_scale(1);dm_tc_set_safe_insets(left,right);
    if(ready){cancel();dm_tc_resize(&controls,w,h);}else{dm_tc_init(&controls,w,h);controls.mouse_click_mode=1;ready=1;}   /* 功能页里非物品区的点按走界面鼠标点击（front_ios.m:145 同一句） | taps outside the item area in function pages go through UI mouse clicks */
    restrict_layout();apply_ui_locked();pthread_mutex_unlock(&lock);
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeEnable(JNIEnv *env,jclass cls,jboolean on) {
    (void)env;(void)cls;pthread_mutex_lock(&lock);cancel();enabled=on!=0;apply_ui_locked();pthread_mutex_unlock(&lock);
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeCancel(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;pthread_mutex_lock(&lock);cancel();pthread_mutex_unlock(&lock);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeEvent(JNIEnv *env,jclass cls,jintArray ids,jfloatArray xs,jfloatArray ys,jboolean cancelled,jfloat lift_nx,jfloat lift_ny) {
    (void)cls;if(!ids||!xs||!ys)return EINVAL;
    int n=(*env)->GetArrayLength(env,ids);
    if(n>16||(*env)->GetArrayLength(env,xs)!=n||(*env)->GetArrayLength(env,ys)!=n)return EINVAL;
    jint id[16];jfloat x[16],y[16];
    (*env)->GetIntArrayRegion(env,ids,0,n,id);(*env)->GetFloatArrayRegion(env,xs,0,n,x);(*env)->GetFloatArrayRegion(env,ys,0,n,y);
    if((*env)->ExceptionCheck(env))return EINVAL;
    for(int i=0;i<n;i++){if(!isfinite(x[i])||!isfinite(y[i]))return EINVAL;for(int j=0;j<i;j++)if(id[i]==id[j])return EINVAL;}
    pthread_mutex_lock(&lock);apply_ui_locked();feed_item_kind();resolve_pending();
    lift_x=lift_nx;lift_y=lift_ny;
    if(cancelled){cancel();pthread_mutex_unlock(&lock);return 0;}
    if(!ready||!enabled){pthread_mutex_unlock(&lock);return ENODEV;}
    double now=seconds();
    for(int slot=0;slot<16;slot++)if(slots[slot].active){int seen=0;for(int i=0;i<n;i++)seen|=slots[slot].id==id[i];
        if(!seen){slots[slot].active=0;
            if(slot==pend_slot){pend_up=1;pend_end_at=now;}   /* 指针手指挂起：等宿主的分类 / 落点答案 */
            else dm_tc_end_at(&controls,slot,now);}}
    dm_android_pointer raw[16];int nr=0;
    for(int i=0;i<n;i++) {
        int slot=0;while(slot<16&&(!slots[slot].active||slots[slot].id!=id[i]))slot++;
        float px=x[i]*controls.layout.width,py=y[i]*controls.layout.height;
        if(slot==16){slot=0;while(slot<16&&(slots[slot].active||controls.finger[slot].active))slot++;if(slot==16){pthread_mutex_unlock(&lock);return EOVERFLOW;}
            slots[slot].active=1;slots[slot].id=id[i];dm_tc_begin_at(&controls,slot,px,py,now);
            if(controls.navigation_mode&&controls.pointer_slot==slot){   /* 功能页里的指针手指：问宿主这是不是物品区 */
                pend_q=dm_ui_item_query(x[i],y[i]);pend_slot=slot;pend_up=0;pend_at=now;pend_end_at=0;pend_rel=0;pend_rel_asked=0;item_fed=0;}
        }else{
            dm_tc_move_at(&controls,slot,px,py,now);
            dm_tc_finger *pf=&controls.finger[slot];
            float w=controls.layout.width>0?controls.layout.width:1.f, h=controls.layout.height>0?controls.layout.height:1.f;
            if(controls.navigation_mode&&controls.pointer_slot==slot){
                if(pf->ui_drag){   /* 界面连续手势：上下拖 = 滚动、横向 = 滑块；定型后交给宿主按游戏自己的命中去做 */
                    if(!ui_drag_on){ui_drag_on=1;dm_ui_drag_lift(DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(w,h)/h);
                        dm_ui_drag_begin(pf->ui_drag,pf->aim_origin_x/w,pf->aim_origin_y/h,44.f/h);}
                    dm_ui_drag_move(x[i],y[i]);}
                if(pf->pointer_dragged){   /* 拖放中：问落点（物品抬在手指上方），松手用最近一次的回答 */
                    pend_rel=dm_ui_drop_query(x[i],(py-DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(w,h))/h,y[i]);
                    pend_rel_asked=1;}
            }
        }
        if(controls.finger[slot].target==DM_TC_NONE)raw[nr++]=(dm_android_pointer){.id=(uint32_t)id[i],.x=x[i],.y=y[i]};
    }
    tick();int rc=dm_android_input_publish_touch(raw,(size_t)nr);if(!rc)rc=publish();
    pthread_mutex_unlock(&lock);return rc;
}
JNIEXPORT jfloatArray JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeFrame(JNIEnv *env,jclass cls) {
    (void)cls;jfloat data[58]={0};pthread_mutex_lock(&lock);apply_ui_locked();tick();
    for(int i=0;i<DM_TC_COUNT;i++){dm_tc_target t=controls.layout.target[i];data[i*3]=t.x;data[i*3+1]=t.y;data[i*3+2]=(applied_mask&(1u<<i))?t.r:0;
        for(int j=0;j<16;j++)if(controls.finger[j].active&&controls.finger[j].target==i)data[44+i]=1;}
    data[36]=controls.layout.joy_x;data[37]=controls.layout.joy_y;data[38]=controls.layout.joy_radius;
    data[39]=controls.layout.swap.x;data[40]=controls.layout.swap.y;data[41]=controls.layout.swap.r;
    data[42]=data[36]+controls.lx/32767.f*data[38];data[43]=data[37]-controls.ly/32767.f*data[38];
    data[56]=(float)controls.skill_set;data[57]=controls.joy_slot!=DM_TC_NONE;
    if(applied_nav){data[38]=0;data[41]=0;}   /* 面板态不画摇杆/换组（对齐 DmTouchOverlay：navigationMode 直接跳过） | no stick / set-switch while a panel is open */
    pthread_mutex_unlock(&lock);jfloatArray out=(*env)->NewFloatArray(env,58);if(out)(*env)->SetFloatArrayRegion(env,out,0,58,data);return out;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeActivate(JNIEnv *env,jclass cls,jint target) {
    (void)env;(void)cls;pthread_mutex_lock(&lock);
    if(ready&&enabled){int slot=15;while(slot>=0&&(slots[slot].active||controls.finger[slot].active))slot--;
        if(slot>=0){double now=seconds();float x,y;
            if(target>=0&&target<DM_TC_COUNT){x=controls.layout.target[target].x;y=controls.layout.target[target].y;}
            else if(target==12){x=controls.layout.swap.x;y=controls.layout.swap.y;}
            else {x=controls.layout.joy_x;y=controls.layout.joy_y;}
            dm_tc_begin_at(&controls,slot,x,y,now);
            if(target>=20&&target<=23){float radius=controls.layout.joy_radius;dm_tc_move_at(&controls,slot,x+(target==20?-radius:target==21?radius:0),y+(target==22?-radius:target==23?radius:0),now);access_slot=slot;access_until=now+0.25;}
            else dm_tc_end_at(&controls,slot,now+0.01);
            tick();
        }
    }
    pthread_mutex_unlock(&lock);
}

/* Java 侧轮询：局外（前端页面）返回 1——覆盖层隐藏、触摸直通游戏（对齐 iOS：局外 overlay.hidden=YES）。 */
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeFrontend(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;pthread_mutex_lock(&lock);apply_ui_locked();int fe=dm_android_ui_frontend();pthread_mutex_unlock(&lock);return fe;
}

/* P3：底栏 v4 数据（Java 每帧拿）：{valid,hp,hp_max,mp,mp_max,level,exp‰,belt0..3,players,quest,points,dead,portal,panel_mode,frontend} */
JNIEXPORT jfloatArray JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeHud(JNIEnv *env,jclass cls) {
    (void)cls;dm_hud_info h;dm_hud_get(&h);
    jfloat d[18]={h.valid?1.f:0.f,(float)h.hp,(float)h.hp_max,(float)h.mp,(float)h.mp_max,(float)h.level,(float)h.exp_permille,
                  (float)h.belt[0],(float)h.belt[1],(float)h.belt[2],(float)h.belt[3],(float)h.players,(float)h.quest_unread,(float)h.points,
                  h.dead?1.f:0.f,(float)h.portal,(float)dm_android_ui_panel_mode(),(float)dm_android_ui_frontend()};
    jfloatArray out=(*env)->NewFloatArray(env,18);if(out)(*env)->SetFloatArrayRegion(env,out,0,18,d);return out;
}

/* P7 技能钮图标（对齐 iOS shown_glyph + skillImage）：5 个技能钮各 10 个数
 * {status, atlas, frame_常态, frame_按住, 帧 x, y, w, h, 换武器组位, 剩余充能}；status：0 未知 1 空位 2 有图。
 * 快照只是旧了（>250ms 没刷新）照取到那一刻画——绑定没变，只是没人刷新（front_ios.m shown_glyph 同一口径）。 */
JNIEXPORT jfloatArray JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeSkillIcons(JNIEnv *env,jclass cls) {
    (void)cls;pthread_mutex_lock(&lock);
    dm_tg_snapshot snap;memset(&snap,0,sizeof snap);dm_touch_game_snapshot(&snap);
    uint64_t now=now_ns();jfloat d[70];
    for(int i=0;i<5;i++){
        dm_tc_target t=controls.layout.target[i];
        dm_ta_glyph g=dm_ta_current_set(&snap,t.button,now,controls.skill_set);
        if(g.status==DM_TA_UNKNOWN&&snap.captured_ns&&now>snap.captured_ns&&now-snap.captured_ns>DM_TG_MAX_AGE_NS)
            g=dm_ta_current_set(&snap,t.button,snap.captured_ns,controls.skill_set);
        dm_ta_frame f={0,0,0,0},pf={0,0,0,0};
        if(g.status==DM_TA_GLYPH){dm_ta_frame_at(g.atlas,g.normal_frame,&f);dm_ta_frame_at(g.atlas,g.pressed_frame,&pf);}
        int kind=0,charges=0;
        if(g.status==DM_TA_GLYPH)kind=dm_swapskill_slot(g.slot,&charges);
        jfloat *o=d+i*14;
        o[0]=(jfloat)g.status;o[1]=(jfloat)g.atlas;o[2]=(jfloat)g.normal_frame;o[3]=(jfloat)g.pressed_frame;
        o[4]=(jfloat)f.x;o[5]=(jfloat)f.y;o[6]=(jfloat)f.width;o[7]=(jfloat)f.height;
        o[8]=(jfloat)pf.x;o[9]=(jfloat)pf.y;o[10]=(jfloat)pf.width;o[11]=(jfloat)pf.height;
        o[12]=(jfloat)kind;o[13]=(jfloat)charges;
    }
    pthread_mutex_unlock(&lock);
    jfloatArray out=(*env)->NewFloatArray(env,70);if(out)(*env)->SetFloatArrayRegion(env,out,0,70,d);return out;
}
