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
        CHECK(s.buttons==buttons[d]); CHECK(!s.pointer_valid); CHECK(!s.lx && !s.ly);
        for(int i=0;i<40;i++) { dm_tc_tick(&s,1+i*.02); CHECK(s.buttons==buttons[d]); }
        dm_tc_end_at(&s,0,2); CHECK(!s.buttons); CHECK(!s.mouse_click_seq && !s.world_tap_seq);
    }
    setup(&s); begin(&s,0,3,1); dm_tc_end_at(&s,0,1.01);
    CHECK(s.buttons==DM_BTN_RIGHT && s.panel_dpad_pulse); CHECK(dm_tc_dpad_active(&s));
    CHECK(dm_tc_pulse_end(&s,s.pulse_epoch)); CHECK(!s.buttons && !s.panel_dpad_pulse);
    setup(&s); begin(&s,0,0,1); dm_tc_move_at(&s,0,400,50,1.2);
    CHECK(!s.buttons); dm_tc_end_at(&s,0,1.3); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); dm_tc_move_at(&s,0,s.layout.dpad[3].x,s.layout.dpad[3].y,1.2);
    CHECK(s.buttons==DM_BTN_RIGHT); dm_tc_cancel_finger(&s,0); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); begin(&s,1,1,1.1);
    CHECK(s.buttons==DM_BTN_UP); dm_tc_end_at(&s,0,2); CHECK(s.buttons==DM_BTN_DOWN);
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
    begin(&s,1,0,1.1); CHECK(!s.buttons && s.pointer_slot==0); CHECK(!dm_tc_dpad_active(&s));
    dm_tc_cancel_finger(&s,0); CHECK(!s.buttons); dm_tc_end_at(&s,1,2); CHECK(!s.buttons);
    setup(&s); s.holding=1; begin(&s,0,0,1); CHECK(!s.buttons); dm_tc_end_at(&s,0,2); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); dm_tc_begin_at(&s,1,600,160,1.2);
    CHECK(!s.buttons && s.pointer_slot==1); dm_tc_cancel_finger(&s,1); CHECK(!s.buttons);
    dm_tc_end_at(&s,0,2); CHECK(!s.buttons);
    setup(&s); begin(&s,0,0,1); dm_tc_end_at(&s,0,1.01);
    dm_tc_begin_at(&s,1,600,160,1.02); CHECK(!s.buttons && !s.panel_dpad_pulse);
    setup(&s); dm_tc_set_panel_dpad(&s,0); begin(&s,0,3,1);
    CHECK(s.finger[0].target!=DM_TC_DPAD && !s.buttons);

    const float sizes[][2]={{667,375},{874,402},{932,430},{1024,576},{1366,768}};
    for(unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
        dm_tc_set_safe_insets(62,62); dm_tc_layout l; dm_tc_layout_make(&l,sizes[i][0],sizes[i][1]);
        for(int d=0;d<4;d++) {
            dm_tc_target t=l.dpad[d]; CHECK(t.r*2>=44); CHECK(t.x-t.r>=62 && t.x+t.r<=l.width);
            CHECK(t.y-t.r>=0 && t.y+t.r<=l.height); CHECK(dm_tc_dpad_hit(&l,t.x,t.y)==d);
            for(int other=d+1;other<4;other++)
                CHECK(hypotf(t.x-l.dpad[other].x,t.y-l.dpad[other].y)>=t.r+l.dpad[other].r);
        }
        CHECK(dm_tc_dpad_hit(&l,l.dpad[0].x,l.dpad[2].y)==-1);
    }
    /* 真机 874×402 商店货架左缘约 176 点，十字键需完整留在它左侧。
       On the 874×402 device the vendor shelf starts near x=176; keep every target clear. */
    dm_tc_layout phone; dm_tc_layout_make(&phone,874,402);
    for(int d=0;d<4;d++) CHECK(phone.dpad[d].x+phone.dpad[d].r<=172);
    dm_tc_set_safe_insets(0,0);
    printf("PASS panel D-pad %d checks\n",checks); return 0;
}
