/* 背包 / 商店十字键：合成场景和真实手势状态机，不需要游戏文件。
   Inventory/vendor D-pad: synthetic scenes and the production gesture state machine, no game files. */
#include "dm_touch_controls.h"
#include "../../host/dm_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void msg(dm_tc_scene *s,const char *act,const char *panel) { dm_tc_navigation(s,"PanelManager",act,panel); }
static void begin(dm_tc_state *s,int slot,int direction,double time) {
    dm_tc_target t=s->layout.dpad[direction]; dm_tc_begin_at(s,slot,t.x,t.y,time);
}
static void setup(dm_tc_state *s) {
    dm_tc_init(s,874,402); dm_tc_set_navigation_mode(s,1); dm_tc_enable_targets(s,0);
    dm_tc_set_panel_dpad(s,1);
}
int main(void) {
    dm_tc_scene scene={0};
    msg(&scene,"UnloadPanel","LoadScreenPanel"); CHECK(!dm_tc_scene_panel_dpad(&scene));
    msg(&scene,"OpenPanel","PlayerInventoryExpansionLayout"); CHECK(dm_tc_scene_panel_dpad(&scene));
    msg(&scene,"OpenPanel","PauseLayoutGarden"); CHECK(!dm_tc_scene_panel_dpad(&scene));
    msg(&scene,"UnloadPanel","PauseLayoutGarden"); CHECK(dm_tc_scene_panel_dpad(&scene));
    msg(&scene,"OpenPanel","YouDiedModal"); CHECK(!dm_tc_scene_panel_dpad(&scene));
    msg(&scene,"ClosePanel","YouDiedModal"); CHECK(dm_tc_scene_panel_dpad(&scene));
    msg(&scene,"OpenPanel","ButtonBindingModal"); CHECK(!dm_tc_scene_panel_dpad(&scene));
    msg(&scene,"ClosePanel","ButtonBindingModal");
    msg(&scene,"OpenPanel","NpcDialogPanel"); msg(&scene,"OpenPanel","VendorPanelLayout");
    msg(&scene,"OpenPanel","NpcTradePanel"); CHECK(dm_tc_scene_panel_dpad(&scene));
    msg(&scene,"ClosePanel","PlayerInventoryExpansionLayout"); CHECK(dm_tc_scene_panel_dpad(&scene));
    dm_tc_navigation(&scene,"FrontEndNavigation","ToFrontEnd",""); CHECK(!dm_tc_scene_panel_dpad(&scene));
    CHECK(!scene.panels);

    dm_tc_state s;
    const uint64_t buttons[4]={DM_BTN_UP,DM_BTN_DOWN,DM_BTN_LEFT,DM_BTN_RIGHT};
    for(int d=0;d<4;d++) {
        setup(&s); s.pointer_valid=1; begin(&s,0,d,1);
        CHECK(s.finger[0].target==DM_TC_DPAD); CHECK(s.pointer_slot==DM_TC_NONE);
        CHECK(s.buttons==buttons[d]); CHECK(dm_tc_dpad_press_accepted(&s,0)); CHECK(!s.pointer_valid); CHECK(!s.lx && !s.ly);
        for(int i=0;i<40;i++) { dm_tc_tick(&s,1+i*.02); CHECK(s.buttons==buttons[d]); }
        dm_tc_end_at(&s,0,2); CHECK(!s.buttons); CHECK(!dm_tc_dpad_press_accepted(&s,0)); CHECK(!s.mouse_click_seq && !s.world_tap_seq);
    }
    setup(&s); begin(&s,0,3,1); dm_tc_end_at(&s,0,1.01);
    CHECK(s.buttons==DM_BTN_RIGHT && s.panel_dpad_pulse); CHECK(dm_tc_dpad_active(&s));
    CHECK(dm_tc_pulse_end(&s,s.pulse_epoch)); CHECK(!s.buttons && !s.panel_dpad_pulse);
    setup(&s); begin(&s,0,0,1); dm_tc_move_at(&s,0,400,50,1.2);
    CHECK(!s.buttons); dm_tc_end_at(&s,0,1.3); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); dm_tc_move_at(&s,0,s.layout.dpad[3].x,s.layout.dpad[3].y,1.2);
    CHECK(s.buttons==DM_BTN_RIGHT); dm_tc_cancel_finger(&s,0); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); begin(&s,1,1,1.1);
    CHECK(s.buttons==DM_BTN_UP); CHECK(dm_tc_dpad_press_accepted(&s,0)); CHECK(!dm_tc_dpad_press_accepted(&s,1));
    dm_tc_end_at(&s,0,2); CHECK(s.buttons==DM_BTN_DOWN);
    dm_tc_end_at(&s,1,2); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); begin(&s,1,1,1.2); dm_tc_end_at(&s,1,1.21);
    CHECK(s.buttons==DM_BTN_UP && !s.panel_dpad_pulse); dm_tc_end_at(&s,0,2); CHECK(!s.buttons);
    setup(&s); begin(&s,0,1,1); dm_tc_set_panel_dpad(&s,0);
    CHECK(!s.buttons && !s.finger[0].active); CHECK(!dm_tc_dpad_active(&s));
    setup(&s); begin(&s,0,3,1); dm_tc_end_at(&s,0,1.01); dm_tc_set_navigation_mode(&s,0);
    CHECK(!s.panel_dpad && !s.panel_dpad_pulse && !s.buttons);
    setup(&s); begin(&s,0,2,1); dm_tc_resize(&s,932,430); CHECK(!s.buttons && !s.finger[0].active);

    /* 物品触点正在拖放时，第二根手指不能用方向键抢走光标。
       A second finger on the D-pad cannot steal the cursor from an active item gesture. */
    setup(&s); dm_tc_begin_at(&s,0,600,160,1); CHECK(s.pointer_slot==0);
    begin(&s,1,0,1.1); CHECK(!s.buttons && s.pointer_slot==0); CHECK(!dm_tc_dpad_press_accepted(&s,1)); CHECK(!dm_tc_dpad_active(&s));
    dm_tc_cancel_finger(&s,0); CHECK(!s.buttons); dm_tc_end_at(&s,1,2); CHECK(!s.buttons);
    setup(&s); s.holding=1; begin(&s,0,0,1); CHECK(!s.buttons); dm_tc_end_at(&s,0,2); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); dm_tc_begin_at(&s,1,600,160,1.2);
    CHECK(!s.buttons && s.pointer_slot==1); dm_tc_cancel_finger(&s,1); CHECK(!s.buttons);
    dm_tc_end_at(&s,0,2); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); dm_tc_end_at(&s,0,1.01);
    dm_tc_begin_at(&s,1,600,160,1.02); CHECK(!s.buttons && !s.panel_dpad_pulse);
    setup(&s); dm_tc_set_panel_dpad(&s,0); begin(&s,0,3,1);
    CHECK(s.finger[0].target!=DM_TC_DPAD && !s.buttons);

    /* 保护区空隙不起指针，也不在移动 / 松手时退回关窗逻辑。
       Guard gaps never create a pointer or fall through to window closing on move/release. */
    dm_tc_set_safe_insets(62,62);
    const float gaps[][2]={{116,287},{50,287},{116,220},{116,354},{50,220},{173,220},{132,271}};
    for(unsigned i=0;i<sizeof gaps/sizeof *gaps;i++) {
        setup(&s); float x=gaps[i][0],y=gaps[i][1];
        CHECK(dm_tc_dpad_guard_hit(&s.layout,x,y)); CHECK(dm_tc_dpad_hit(&s.layout,x,y)==-1);
        dm_tc_begin_at(&s,0,x,y,1); CHECK(s.finger[0].target==DM_TC_DPAD);
        CHECK(s.pointer_slot==DM_TC_NONE && !s.buttons && !dm_tc_dpad_active(&s)); CHECK(!dm_tc_dpad_press_accepted(&s,0));
        dm_tc_move_at(&s,0,s.layout.dpad[3].x,s.layout.dpad[3].y,1.02); CHECK(!s.buttons);
        dm_tc_move_at(&s,0,830,300,1.03); dm_tc_end_at(&s,0,1.05); dm_tc_tick(&s,1.3);
        CHECK(!s.buttons && !s.close_left && !s.pulse_button);
        CHECK(!s.mouse_click_seq && !s.world_tap_seq && !s.shop_drop_seq);
    }
    const float enlarged[][2]={{116,230},{116,344},{59,287},{173,287}};
    for(int d=0;d<4;d++) {
        setup(&s); CHECK(hypotf(enlarged[d][0]-s.layout.dpad[d].x,enlarged[d][1]-s.layout.dpad[d].y)>s.layout.dpad[d].r);
        dm_tc_begin_at(&s,0,enlarged[d][0],enlarged[d][1],1);
        CHECK(s.buttons==buttons[d] && s.pointer_slot==DM_TC_NONE);
        dm_tc_end_at(&s,0,1.2); CHECK(!s.buttons && !s.close_left);
    }
    setup(&s); dm_tc_begin_at(&s,0,600,160,1); dm_tc_begin_at(&s,1,50,287,1.1);
    CHECK(s.pointer_slot==0 && !s.buttons); dm_tc_end_at(&s,1,1.2); CHECK(s.pointer_slot==0 && !s.close_left);
    setup(&s); dm_tc_begin_at(&s,0,50,287,1); dm_tc_set_panel_dpad(&s,0); dm_tc_end_at(&s,0,1.1);
    CHECK(!s.buttons && !s.close_left && !s.finger[0].active);
    /* 货架从约 x176 起仍走物品指针；保护区以外的左右空白照旧关窗。
       The shelf from about x176 still uses item gestures; blank space on both sides still closes panels. */
    setup(&s); CHECK(!dm_tc_dpad_guard_hit(&s.layout,176,287)); dm_tc_begin_at(&s,0,176,287,1);
    CHECK(s.pointer_slot==0); dm_tc_pointer_item(&s,1); dm_tc_end_at(&s,0,1.2); CHECK(!s.close_left);
    const float outside[][2]={{25,150},{830,300}};
    for(int i=0;i<2;i++) {
        setup(&s); dm_tc_begin_at(&s,0,outside[i][0],outside[i][1],1); CHECK(s.pointer_slot==0);
        dm_tc_pointer_item(&s,3); dm_tc_end_at(&s,0,1.1); dm_tc_tick(&s,1.2); CHECK(s.buttons==DM_BTN_B);
    }
    setup(&s); dm_tc_set_panel_dpad(&s,0); dm_tc_begin_at(&s,0,50,287,1); CHECK(s.pointer_slot==0);
    dm_tc_pointer_item(&s,3); dm_tc_end_at(&s,0,1.1); dm_tc_tick(&s,1.2); CHECK(s.buttons==DM_BTN_B);
    dm_tc_set_safe_insets(0,0);

    /* 横屏点数：包含 16:9 小屏、常见全面屏与 iPad 的 16:9 界面子视图。
       Landscape points: small 16:9 phones, common wide phones, and the iPad's 16:9 UI subview. */
    const float sizes[][4]={
        {667,375,0,0},{736,414,0,0},{780,360,50,0},{812,375,50,0},
        {844,390,47,0},{852,393,62,0},{874,402,62,0},{896,414,48,0},
        {926,428,47,0},{932,430,59,0},{956,440,62,0},
        {1024,576,0,1},{1080,607.5f,0,1},{1133,637.3125f,0,1},
        {1180,663.75f,0,1},{1194,671.625f,0,1},{1210,680.625f,0,1},
        {1366,768.375f,0,1},{1376,774,0,1}
    };
    for(unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) for(int rotated=0;rotated<2;rotated++) {
        float w=sizes[i][0],h=sizes[i][1],safe=sizes[i][2];
        dm_tc_set_tall_scale((int)sizes[i][3]); dm_tc_set_safe_insets(rotated?0:safe,rotated?safe:0);
        dm_tc_layout l; dm_tc_layout_make(&l,w,h);
        CHECK(l.dpad_guard[0]>=0 && l.dpad_guard[1]>=0);
        CHECK(l.dpad_guard[0]+l.dpad_guard[2]<=w && l.dpad_guard[1]+l.dpad_guard[3]<=h);
        /* 独立量得的货架左沿：1957×900 原版命中框 x=397。不要截走最左列的触点。
           Independently measured shelf edge: native hit box x=397 in a 1957x900 viewport. */
        float shelf=w*.5f-(1957*.5f-397)*h/900;
        CHECK(l.dpad_guard[0]+l.dpad_guard[2]<shelf);
        for(int d=0;d<4;d++) {
            dm_tc_target t=l.dpad[d]; CHECK(t.r*2>=44);
            CHECK(t.x-t.r>=-0.001f && t.x+t.r<shelf);
            CHECK(t.y-t.r>=0 && t.y+t.r<=l.height); CHECK(dm_tc_dpad_hit(&l,t.x,t.y)==d);
            for(int other=d+1;other<4;other++)
                CHECK(hypotf(t.x-l.dpad[other].x,t.y-l.dpad[other].y)+0.001f>=t.r+l.dpad[other].r);
            dm_tc_init(&s,w,h); dm_tc_set_navigation_mode(&s,1); dm_tc_enable_targets(&s,0); dm_tc_set_panel_dpad(&s,1);
            begin(&s,0,d,1); CHECK(s.buttons==buttons[d] && s.pointer_slot==DM_TC_NONE);
            dm_tc_move_at(&s,0,shelf+2,h*.5f,1.1); dm_tc_end_at(&s,0,1.2);
            CHECK(!s.buttons && !s.close_left && !s.mouse_click_seq);
        }
        CHECK(dm_tc_dpad_hit(&l,l.dpad[0].x,l.dpad[2].y)==-1);
        /* 商品区内密集取点：扩大的圆形热区也不能越过已裁剪的保护区。
           Dense shelf samples: even the enlarged circular targets must respect the clipped guard. */
        for(float x=shelf+.1f;x<shelf+20;x+=2) for(float y=h*.2f;y<h*.72f;y+=3) {
            CHECK(!dm_tc_dpad_guard_hit(&l,x,y)); CHECK(dm_tc_dpad_hit(&l,x,y)==-1);
        }
        dm_tc_init(&s,w,h); dm_tc_set_navigation_mode(&s,1); dm_tc_enable_targets(&s,0); dm_tc_set_panel_dpad(&s,1);
        dm_tc_begin_at(&s,0,shelf+1,h*.55f,1); CHECK(s.pointer_slot==0);
        dm_tc_pointer_item(&s,7); dm_tc_end_at(&s,0,1.2); CHECK(!s.buttons && !s.close_left);
        dm_tc_cancel_all(&s);
        dm_tc_begin_at(&s,0,w-5,h*.5f,2); CHECK(s.pointer_slot==0);
        dm_tc_pointer_item(&s,3); dm_tc_end_at(&s,0,2.1); dm_tc_tick(&s,2.2); CHECK(s.buttons==DM_BTN_B);
        dm_tc_init(&s,w,h); dm_tc_set_navigation_mode(&s,1); dm_tc_enable_targets(&s,0); dm_tc_set_panel_dpad(&s,1);
        float gx=l.dpad_guard[0]+1,gy=l.dpad_guard[1]+1;
        CHECK(dm_tc_dpad_hit(&l,gx,gy)==-1);
        dm_tc_begin_at(&s,0,gx,gy,3); CHECK(s.finger[0].target==DM_TC_DPAD);
        dm_tc_move_at(&s,0,w-5,h*.5f,3.1); dm_tc_end_at(&s,0,3.2); dm_tc_tick(&s,3.5);
        CHECK(!s.buttons && !s.close_left && !s.mouse_click_seq);
        dm_tc_resize(&s,932,430); CHECK(!s.buttons && !s.finger[0].active);
    }
    /* 极窄多窗口回退：不画也不拦，物品仍可直接触摸。
       Very narrow multiwindow fallback: no drawing or interception; direct item touch remains available. */
    dm_tc_set_safe_insets(0,0); dm_tc_set_tall_scale(1); dm_tc_init(&s,507,285.1875f);
    dm_tc_set_navigation_mode(&s,1); dm_tc_enable_targets(&s,0); dm_tc_set_panel_dpad(&s,1);
    CHECK(!s.layout.dpad_guard[2]);
    for(int d=0;d<4;d++) CHECK(!s.layout.dpad[d].r);
    dm_tc_begin_at(&s,0,40,160,1); CHECK(s.pointer_slot==0 && s.finger[0].target!=DM_TC_DPAD);
    dm_tc_resize(&s,1024,576); CHECK(s.layout.dpad[0].r>=22 && s.layout.dpad_guard[2]>0);
    begin(&s,0,0,2); CHECK(s.buttons==DM_BTN_UP); dm_tc_end_at(&s,0,2.2); CHECK(!s.buttons);
    dm_tc_set_tall_scale(0); dm_tc_set_safe_insets(62,62);
    /* 真机 874×402 商店货架左缘约 176 点，十字键需完整留在它左侧。
       On the 874×402 device the vendor shelf starts near x=176; keep every target clear. */
    dm_tc_layout phone; dm_tc_layout_make(&phone,874,402);
    for(int d=0;d<4;d++) CHECK(phone.dpad[d].x+phone.dpad[d].r<=172);
    dm_tc_set_safe_insets(0,0);
    printf("PASS panel D-pad %d checks\n",checks); return 0;
}
