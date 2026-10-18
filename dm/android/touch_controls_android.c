/* Android adapter for the unchanged upstream portable touch state machine.
 * No game-version addresses or Apple UI/skill observation hooks are linked. */
#include "../ios/Sources/dm_touch_controls.h"
#include "input_backend.h"
#include "ui_state_android.h"
#include "../host/dm_cursor.h"
#include "../host/dm_actions.h"
#include "../host/dm_input.h"
#include "../host/dm_hud.h"
#include "../host/dm_i18n.h"
#include "../ios/Sources/dm_touch_game_state.h"
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
/* 小地图的点击区：**两端共用一条规则**，判定在 dm/host/dm_cursor.c 的 dm_ui_minimap_tap_hit（地图框顶部那一条「场景名」，
 * 整宽 × 框高 1/4；为什么不是整块见那边的注释）。这里只做三件事：归一化坐标、排除落在控件命中圆上的点（回城钮挪到技能弧上方后
 * 在营地里被小地图框盖住）、面板态不拦。不看档位（对齐 iOS：触屏模式下 ui_map_pump 把三档钉在微缩档上）。
 * The minimap tap area: **one rule shared by both ends**, judged in dm/host/dm_cursor.c's dm_ui_minimap_tap_hit (the 「area name」 strip at the top
 * of the map frame, full width x 1/4 of the frame height; why not the whole block is explained there). This side only does three things: normalize
 * the coordinates, exclude points landing on a widget's hit circle (after the town portal button moved above the skill arc it is covered by the map
 * frame in camp), and skip while in panel mode. In fullscreen mode (0) the game gives no rect for "that block", so the top-right area is the switch. */
static int in_minimap(float x,float y) {
    if(controls.navigation_mode)return 0;
    float w=controls.layout.width,h=controls.layout.height; if(w<=0||h<=0)return 0;
    int dm_ui_minimap_tap_hit(float,float); int hit=dm_ui_minimap_tap_hit(x/w,y/h);
    if(hit<0){   /* 矩形还没读到过（进局头几拍、地图还关着）：按同一条规则的形状给个兜底 —— 那一条场景名所在的位置。
                    没有兜底的话，这一块会被当成世界点按，玩家点场景名「没反应」（真机口径）。
                    Rect not read yet (the first beats after entering, with the map still closed): fall back to the shape of the same rule -- where that
                    area-name strip sits. Without it this area counts as a world tap and tapping the area name "does nothing" (device report). */
        if(dm_tc_hit(&controls.layout,x,y)>=0)return 0;
        return (x>=w*0.62f&&y<=h*0.16f)||(x<=w*0.38f&&y<=h*0.16f); }   /* 左右两侧都算（不看档位，同 iOS）| either side counts (no setting branch, same as iOS) */
    if(!hit)return 0;
    if(dm_tc_hit(&controls.layout,x,y)>=0)return 0;
    return 1;
}
static void restrict_layout(void) {
    /* 难度条 / 8 格选择条仍缺原版桥梁，继续留空（These upstream widgets require an audited original-game bridge
     * and stay absent until 91738 has native binding/scene identities）。
     * 回城钮（技能四上方圆钮）已按 iOS 口径接上：位置由布局给（不出按键），
     * 点下由宿主 dm_freetp 施放原版回城技能（front_ios.m:516 同口径）——此前照老注释一起藏了，用户口径「iOS 有它」。 */
    controls.layout.diff.r=0;
    for(int i=0;i<8;i++)controls.layout.pick[i].r=0;
    controls.belt_rows=1;
}
/* UI 状态机（P1）：游戏态掩码 = 全部除右上「地图键」（对齐 front_ios.m:186：游戏态不要地图键，地图那一块由宿主 in_minimap 认，不进模型）；
 * 回城钮照 iOS 给（位置由布局给、不出按键，点下由宿主 dm_freetp 开门）。
 * 面板态（对齐 front_ios.m:183-185）：导航态不给五个手柄圆钮（dm_tc_nav_mask(0)=0），只开对话菜单（3）时给底栏十字。 */
static uint32_t game_mask(void){return ((1u<<DM_TC_COUNT)-1)&~(1u<<DM_TC_MAP);}   /* 游戏态不要右上地图键（对齐 front_ios.m:186）：小地图那一块由宿主认（in_minimap）、这根手指不进模型，不进才不会按 MINUS 把小地图关掉 */
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
static unsigned map_seq; static int map_asked; static int map_slot=-1; static float map_x,map_y; static double map_t;   /* 小地图块点按（对齐 front_ios.m:579-587 的 _mapTouch） */
static int mapbk_slot=-1; static float mapbk_x,mapbk_y; static double mapbk_t;   /* 全屏地图开着时点空处那根（对齐 front_ios.m:517 / 591-593 的 _mapBackTouch） */
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
    /* 加点 / 技能绑定：命中技能图标 / 属性「+」的点按 = 只选中或绑定，不发鼠标点击（对齐 front_ios.m:333-345）——
       绑定态（等玩家点技能）点技能图标 = 光标到图标 + 按 Y（原版绑定动作，绑上后技能才出现在操作界面上）；平时只选中（光标移过去、出说明）。 */
    if(controls.navigation_mode&&!ui_drag_on){
        float bx,by; controls.tap_button=0; controls.tap_move=0;
        int hk=dm_ui_bind_skill_hit(&bx,&by);
        if(hk){ controls.tap_x=bx*controls.layout.width; controls.tap_y=by*controls.layout.height;
            if(hk==1&&dm_ui_bind_active()==2) controls.tap_button=DM_BTN_Y;
            else if(!dm_ui_bind_active()) controls.tap_move=1; }
    }
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
    map_slot=-1;mapbk_slot=-1;
    dm_android_input_cancel();
}
int dm_ui_map_is_full(void); void dm_ui_map_full(int);
unsigned dm_world_tap(float,float); int dm_world_tap_result(unsigned);
/* 加点 / 技能绑定（对齐 front_ios.m:333-345 / 436-455 / 630-635）：技能树与属性页的点按落点由宿主判类
 * （dm_ui_bind_skill_hit：1 = 技能图标、2 = 属性「+」）：
 *   · 绑定态（dm_ui_bind_active()==2 = 等玩家点技能）：点技能图标 = 光标到图标 + 按 Y（resolve_pending 里设 tap_button）
 *   · 平时：点技能图标 / 属性「+」只选中（光标移过去、出说明；点击当场加点且不可撤销）——同一个再点一下 = 加 1 点
 *   · 长按技能图标 = 光标到图标 + 按 Y（弹原版「重新绑定技能」框）；长按属性「+」= 连续加点（0.15 秒 1 点）
 * 光标停靠：说明框跟着手柄光标停的那一格一直出（技能树一开就盖住半个树、关不掉），进绑定态先把光标停到面板左侧空花格，
 * 选中看完 1 秒停到屏幕左缘（只停在有自由光标的页）。 */
static unsigned bd_sel_id; static double bd_add_at,bd_park_at,bd_stat_next; static int bd_was;
static void bind_control(void) {
    if(!ready||!controls.navigation_mode){ bd_sel_id=0;bd_add_at=0;bd_park_at=0;bd_was=0;return; }
    double now=seconds();
    int st=dm_ui_bind_active();
    if(st==2&&bd_was!=2){ controls.pointer_valid=1;controls.pointer_x=0.155f*controls.layout.width;controls.pointer_y=0.5f*controls.layout.height;bd_park_at=0; }
    bd_was=st;
    if(st==2){ bd_sel_id=0; return; }   /* 绑定态不认加点 */
    for(int i=0;i<DM_TC_MAX_TOUCH;i++){ dm_tc_finger *f=&controls.finger[i];
        if(!f->active||f->pointer_dragged||f->ui_drag)continue;
        if(now-f->start_time_s<0.5)continue;
        if(hypotf(f->x-f->aim_origin_x,f->y-f->aim_origin_y)>10.f*dm_tc_scale_for_size(controls.layout.width,controls.layout.height))continue;
        float nx=0,ny=0;uint32_t hid=dm_ui_hit_id();int hk=dm_ui_bind_skill_hit(&nx,&ny);
        if(hk==1&&!f->pointer_long){ f->pointer_long=1; dm_tc_pointer_press(&controls,nx*controls.layout.width,ny*controls.layout.height,DM_BTN_Y);
            LOG("技能树：长按技能 → 光标到点 + 按 Y（开原版绑定框）"); }
        else if(hk==2&&hid>=1&&hid<=4){ if(!f->pointer_long){ f->pointer_long=1;bd_stat_next=now;
                controls.pointer_valid=1;controls.pointer_x=nx*controls.layout.width;controls.pointer_y=ny*controls.layout.height;
                LOG("属性页：按住「+」→ 连续加点"); }
            if(now>=bd_stat_next){ bd_stat_next=now+0.15; dm_stat_add((int)hid-1,1); } } }
    { static unsigned sm; if(controls.tap_move_seq!=sm){ sm=controls.tap_move_seq; unsigned hid=dm_ui_hit_id();
        if(!hid)bd_sel_id=0;
        else if(hid==bd_sel_id)bd_add_at=now+0.05;
        else{ bd_sel_id=hid;bd_park_at=now+1.0;LOG("加点：选中标识 %#x（同一个再点一下 = 加 1 点）",hid); } } }
    if(bd_add_at>0&&now>=bd_add_at){ bd_add_at=0;
        if(bd_sel_id>>31){ dm_points_info pi;dm_points_get(&pi);
            if(pi.tree_open&&pi.sel_skill>=0&&!dm_cursor_pending()){ if(pi.sel_block==0){ dm_skill_add_selected();LOG("加点：同一个再点一下 → 技能加 1 点"); }
                else LOG("加点：不能加（原因 %d），不发",pi.sel_block); } }
        else if(bd_sel_id>=1&&bd_sel_id<=4){ dm_stat_add((int)bd_sel_id-1,1);LOG("加点：同一个「+」再点一下 → 属性 %u 加 1",bd_sel_id-1); } }
    if(bd_park_at>0&&now>=bd_park_at){ bd_park_at=0;
        if(dm_ui_free_cursor()){ controls.pointer_valid=1;controls.pointer_x=0.02f*controls.layout.width;controls.pointer_y=0.5f*controls.layout.height; } }
}
static void tick(void) {
    if(!ready || !enabled)return;
    bind_control();
    feed_item_kind();resolve_pending();
    double now=seconds();dm_android_input_status input;dm_android_input_status_get(&input);
    if(access_slot>=0 && now>=access_until){dm_tc_end_at(&controls,access_slot,now);access_slot=-1;}
    if(map_asked){int wr=dm_world_tap_result(map_seq);
        if(wr>=0){map_asked=0; if(wr==0&&!controls.navigation_mode){dm_ui_map_full(1);LOG("地图块：底下没有目标 → 全景图");}
            else LOG("地图块：底下有世界目标（类 %d）→ 交互、不切地图",wr);}}
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
static void raise_skill_arc_locked(float bottom) {   /* 手势条安全边距：技能弧（主技能 / 技能 1-4 / 换组）整体上抬，别压在手势条下（用户口径"攻击键偏下不好按"） */
    if(bottom<=0)return;
    float max_up=controls.layout.height*0.06f; if(bottom>max_up)bottom=max_up;   /* 最多抬屏高的 6%，别把弧抬到腰带上 */
    for(int i=0;i<=5;i++)controls.layout.target[i].y-=bottom;   /* 主技能 / 技能 1-4 / 回城钮（回城钮在技能四正上方，一起抬才保住间距） */
    if(controls.layout.swap.r>0)controls.layout.swap.y-=bottom;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeResize(JNIEnv *env,jclass cls,jfloat w,jfloat h,jfloat left,jfloat right,jfloat bottom) {
    (void)env;(void)cls;if(!isfinite(w)||!isfinite(h)||w<=0||h<=0)return;
    pthread_mutex_lock(&lock);dm_tc_set_tall_scale(1);dm_tc_set_safe_insets(left,right);
    if(ready){cancel();dm_tc_resize(&controls,w,h);}else{dm_tc_init(&controls,w,h);controls.mouse_click_mode=1;ready=1;}   /* 功能页里非物品区的点按走界面鼠标点击（front_ios.m:145 同一句） | taps outside the item area in function pages go through UI mouse clicks */
    if(isfinite(bottom)&&bottom>0)raise_skill_arc_locked(bottom);
    restrict_layout();apply_ui_locked();pthread_mutex_unlock(&lock);
}
/* 取串（dm/i18n/ui_text.tsv）：界面文案跟游戏语言（英文版进英文） */
JNIEXPORT jstring JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeUiText(JNIEnv *env,jclass cls,jstring key) {
    (void)cls;if(!key)return NULL;
    const char *k=(*env)->GetStringUTFChars(env,key,NULL);if(!k)return NULL;
    const char *s=dm_ui(k);(*env)->ReleaseStringUTFChars(env,key,k);
    if(!s)return NULL;
    jstring r=(*env)->NewStringUTF(env,s);return r;
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
            if(slot==map_slot){   /* 小地图那一块：没进模型（iOS 同口径），松手这一刻定去向 */
                map_slot=-1;
                { float w=controls.layout.width>0?controls.layout.width:1.f,h=controls.layout.height>0?controls.layout.height:1.f;
                  float lx=lift_x>=0?lift_x*w:map_x,ly=lift_y>=0?lift_y*h:map_y;   /* 抬起那根手指的位置（前端给了就用它，判点按/拖动） */
                  if(hypotf(lx-map_x,ly-map_y)<=14.f&&now-map_t<0.6){
                      /* 照 iOS（front_ios.m:579-587 _mapTouch 的松手分支）：小地图那一块底下也是游戏世界——NPC / 物件 / 掉落物站在那里时，
                         点它应当是交互而不是切地图（恰西在铁匠铺前点不到）。所以小图态先把这一下交给世界点按，答「没有目标」再切全景图；
                         全景图开着时点这一块 = 切回小地图。档位（全屏 / 微缩放左 / 微缩放右）只由设置页决定，这一下不碰它。
                         Same as iOS (front_ios.m:579-587, the release branch of _mapTouch): under the minimap block there is also the game world, and when
                         an NPC / object / dropped item stands there, tapping it should be an interaction rather than a map switch (Charsi could not be
                         tapped in front of the smithy). So in minimap state the tap first goes to the world tap, and only a "no target" answer switches to
                         the fullscreen map; while the fullscreen map is open, tapping this block switches back. The setting (fullscreen / mini left / mini
                         right) is decided on the options page only and is not touched here. */
                      if(dm_ui_map_is_full()){dm_ui_map_full(0);LOG("地图块：点小地图那一条 (%.0f,%.0f) → 回小地图",map_x,map_y);}
                      else{map_seq=dm_world_tap(map_x/w,map_y/h);map_asked=1;LOG("地图块：点小地图那一条 (%.0f,%.0f) → 先看底下有没有世界目标",map_x,map_y);}
                  } }
                continue; }
            if(slot==mapbk_slot){   /* 全屏地图开着、点在空处：点按 = 回小地图（front_ios.m:591-593）；这根进过模型，照常走下面的结束 */
                mapbk_slot=-1;
                { float w=controls.layout.width>0?controls.layout.width:1.f,h=controls.layout.height>0?controls.layout.height:1.f;
                  float lx=lift_x>=0?lift_x*w:mapbk_x,ly=lift_y>=0?lift_y*h:mapbk_y;
                  if(hypotf(lx-mapbk_x,ly-mapbk_y)<=14.f&&now-mapbk_t<0.6&&dm_ui_map_is_full()){dm_ui_map_full(0);LOG("地图块：全屏地图开着点空处 → 回小地图");} } }
            if(slot==pend_slot){pend_up=1;pend_end_at=now;}   /* 指针手指挂起：等宿主的分类 / 落点答案 */
            else dm_tc_end_at(&controls,slot,now); }}
    dm_android_pointer raw[16];int nr=0;
    for(int i=0;i<n;i++) {
        int slot=0;while(slot<16&&(!slots[slot].active||slots[slot].id!=id[i]))slot++;
        float px=x[i]*controls.layout.width,py=y[i]*controls.layout.height;
        if(slot==16){slot=0;while(slot<16&&(slots[slot].active||controls.finger[slot].active))slot++;if(slot==16){pthread_mutex_unlock(&lock);return EOVERFLOW;}
            slots[slot].active=1;slots[slot].id=id[i];
            if(in_minimap(px,py)){map_slot=slot;map_x=px;map_y=py;map_t=now;   /* 小地图那一块：这根手指不进手势层（iOS 同口径）——不进就不会按 MINUS 把小地图关掉，只在松手时定去向 */
                LOG("地图块：按在小地图上（%.0f,%.0f，松手定去向）",px,py);
                continue;}
            dm_tc_begin_at(&controls,slot,px,py,now);
            { int tg=controls.finger[slot].target;
              if(tg==DM_TC_PORTAL&&!controls.navigation_mode){   /* 回城钮：不出按键，点下由宿主施放原版回城技能（dm_freetp.c；front_ios.m:516 同口径） */
                  void dm_tp_request(void);dm_tp_request();LOG("前端：点回城钮 → 免费回城（宿主 dm_freetp.c）"); }
              if(tg==DM_TC_NONE&&!controls.navigation_mode&&dm_ui_map_is_full()){mapbk_slot=slot;mapbk_x=px;mapbk_y=py;mapbk_t=now;}   /* 全屏地图开着、这根手指没落在任何键上：松手若是点按就回小地图（front_ios.m:517） */
              if(tg>=DM_TC_MAIN&&tg<=DM_TC_SKILL4&&!controls.navigation_mode){   /* 空技能位：点一下 = 给这个位选技能（宿主开技能树） */
                  dm_tg_snapshot snap;memset(&snap,0,sizeof snap);dm_touch_game_snapshot(&snap);
                  dm_ta_glyph g=dm_ta_current_set(&snap,controls.layout.target[tg].button,now_ns(),controls.skill_set);
                  if(g.status!=DM_TA_GLYPH){ void dm_ui_bind_begin(uint64_t); dm_ui_bind_begin((uint64_t)controls.layout.target[tg].button);
                      LOG("触屏：点空技能位（位 %d）→ 给键 %#llx 选技能",tg,(unsigned long long)controls.layout.target[tg].button); } } }
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

/* P7 技能钮图标（对齐 iOS shown_glyph + skillImage）：6 个圆钮各 14 个数
 * {status, atlas, frame_常态, frame_按住, 帧 x, y, w, h, 换武器组位, 剩余充能}；status：0 未知 1 空位 2 有图。
 * 前 5 个 = 主技能 / 技能 1-4（按绑定快照取图）；第 6 个 = 回城钮（固定取原版回城技能 359 的图，
 * 对齐 DmTouchOverlay.m:203 的 dm_ta_skill(359)；不出按键、无次数角标）。
 * 快照只是旧了（>250ms 没刷新）照取到那一刻画——绑定没变，只是没人刷新（front_ios.m shown_glyph 同一口径）。 */
JNIEXPORT jfloatArray JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeSkillIcons(JNIEnv *env,jclass cls) {
    (void)cls;pthread_mutex_lock(&lock);
    dm_tg_snapshot snap;memset(&snap,0,sizeof snap);dm_touch_game_snapshot(&snap);
    uint64_t now=now_ns();jfloat d[84];
    for(int i=0;i<6;i++){
        dm_ta_glyph g;
        int kind=0,charges=0;
        if(i==5){   /* 回城钮：原版回城技能图（技能描述表 Town Portal：通用技能页第 18 格，219 / 220 / 411 同一张），不显示次数 */
            g=dm_ta_skill(359);
        }else{
            dm_tc_target t=controls.layout.target[i];
            g=dm_ta_current_set(&snap,t.button,now,controls.skill_set);
            if(g.status==DM_TA_UNKNOWN&&snap.captured_ns&&now>snap.captured_ns&&now-snap.captured_ns>DM_TG_MAX_AGE_NS)
                g=dm_ta_current_set(&snap,t.button,snap.captured_ns,controls.skill_set);
            if(g.status==DM_TA_GLYPH)kind=dm_swapskill_slot(g.slot,&charges);
        }
        dm_ta_frame f={0,0,0,0},pf={0,0,0,0};
        if(g.status==DM_TA_GLYPH){dm_ta_frame_at(g.atlas,g.normal_frame,&f);dm_ta_frame_at(g.atlas,g.pressed_frame,&pf);}
        jfloat *o=d+i*14;
        o[0]=(jfloat)g.status;o[1]=(jfloat)g.atlas;o[2]=(jfloat)g.normal_frame;o[3]=(jfloat)g.pressed_frame;
        o[4]=(jfloat)f.x;o[5]=(jfloat)f.y;o[6]=(jfloat)f.width;o[7]=(jfloat)f.height;
        o[8]=(jfloat)pf.x;o[9]=(jfloat)pf.y;o[10]=(jfloat)pf.width;o[11]=(jfloat)pf.height;
        o[12]=(jfloat)kind;o[13]=(jfloat)charges;
    }
    pthread_mutex_unlock(&lock);
    jfloatArray out=(*env)->NewFloatArray(env,84);if(out)(*env)->SetFloatArrayRegion(env,out,0,84,d);return out;
}
