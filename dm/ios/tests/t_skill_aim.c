#include "dm_touch_controls.h"
#include "../../host/dm_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x); exit(1); } } while (0)
static void begin(dm_tc_state *s,int n,int target) { dm_tc_begin(s,n,s->layout.target[target].x,s->layout.target[target].y); if(target>=DM_TC_MAIN && target<=DM_TC_SKILL4) dm_tc_tick(s,s->last_time_s+0.1);   /* 主钮 / 技能钮按下后有判定时间（点按还是拖动），这里的「按下」= 已过判定时间 | After the main button / a skill button is pressed there is a decision time (tap or drag); here "pressed" = the decision time has passed */ }
static void drag(dm_tc_state *s,int n,int target,float dx,float dy) { dm_tc_move(s,n,s->layout.target[target].x+dx,s->layout.target[target].y+dy); }
int main(void) {
    dm_tc_state s; dm_tc_init(&s,667,375);
    CHECK(s.rx==0 && s.ry==0 && s.aim_slot==DM_TC_NONE && !s.navigation_mode && !s.map_cancel_active);
    begin(&s,0,DM_TC_MAIN); CHECK(s.buttons==DM_BTN_A && s.rx==0 && s.ry==0);
    /* 主钮长按连射时手指的小抖动（10 点死区内）不改方向，仍按住攻击（公开 issue #92） | Small finger jitter (within the 10-point dead zone) while holding the main button to attack keeps the direction and keeps attacking (public issue #92) */
    drag(&s,0,DM_TC_MAIN,6,4); CHECK(s.buttons==DM_BTN_A && s.rx==0 && s.ry==0);
    drag(&s,0,DM_TC_MAIN,-8,-5); CHECK(s.buttons==DM_BTN_A && s.rx==0 && s.ry==0);
    drag(&s,0,DM_TC_MAIN,12,0); CHECK(s.rx>0 && s.ry==0);
    drag(&s,0,DM_TC_MAIN,60,0); CHECK(s.rx==32767 && s.ry==0 && s.aim_slot==0);
    drag(&s,0,DM_TC_MAIN,0,60); CHECK(s.rx==0 && s.ry==-32767);
    drag(&s,0,DM_TC_MAIN,30,-30); CHECK(s.rx==16384 && s.ry==16384);
    drag(&s,0,DM_TC_MAIN,120,-120); CHECK(s.rx==32767 && s.ry==32767);
    drag(&s,0,DM_TC_MAIN,0,0); CHECK(s.rx==0 && s.ry==0 && s.aim_slot==0);
    dm_tc_begin(&s,1,90,275); dm_tc_move(&s,1,150,275);
    drag(&s,0,DM_TC_MAIN,60,0); CHECK(s.lx==32767 && s.ly==0 && s.rx==32767 && s.ry==0);
    /* 技能钮与主钮同一套：按下即键下、按住 = 持续施放，后拖动的那根手指定方向，松手键起、不发脉冲 | On-screen skill buttons use the same scheme as the main button: key down on press, hold = continuous cast, the finger that dragged later sets the direction, on release the key goes up, no pulse sent */
    begin(&s,2,DM_TC_SKILL1); CHECK(s.buttons==(DM_BTN_A|DM_BTN_B));
    drag(&s,2,DM_TC_SKILL1,0,60); CHECK(s.lx==32767 && s.rx==0 && s.ry==-32767 && s.aim_slot==2 && s.buttons==(DM_BTN_A|DM_BTN_B));
    dm_tc_tick(&s,s.last_time_s+0.1); dm_tc_end(&s,2); CHECK(s.buttons==DM_BTN_A && s.rx==32767 && s.ry==0 && s.aim_slot==0 && !s.pulse_button);   /* 键已按住够久：松手键起，不补脉冲 | The key has been held long enough: on release the key goes up, no make-up pulse */
    begin(&s,2,DM_TC_MAIN);
    dm_tc_target map=s.layout.target[DM_TC_MAP];
    dm_tc_move(&s,2,map.x+map.r,map.y-map.r);
    CHECK(s.map_cancel_active && s.buttons==DM_BTN_A && s.rx==32767 && s.lx==32767);
    dm_tc_move(&s,2,map.x-map.r-1,map.y); CHECK(!s.map_cancel_active && s.buttons==DM_BTN_A && s.aim_slot==0);
    dm_tc_end(&s,0); CHECK(s.buttons==0 && s.rx==0 && s.ry==0 && s.aim_slot==DM_TC_NONE && s.lx==32767);
    drag(&s,2,DM_TC_MAIN,60,0); CHECK(s.buttons==0 && s.rx==0 && s.ry==0);
    dm_tc_end(&s,2); CHECK(!s.map_cancel_active);
    begin(&s,0,DM_TC_SKILL2); drag(&s,0,DM_TC_SKILL2,60,0);
    dm_tc_move(&s,0,map.x,map.y); CHECK(s.map_cancel_active && s.buttons==0 && s.rx==0 && s.ry==0 && s.lx==32767);
    dm_tc_end(&s,0); CHECK(!s.map_cancel_active && s.lx==32767 && s.buttons==0 && !s.pulse_button);   /* 取消区松手不放 | Releasing in the cancel zone does not cast */
    begin(&s,0,DM_TC_MAIN); drag(&s,0,DM_TC_MAIN,60,0);
    dm_tc_set_navigation_mode(&s,1); CHECK(s.navigation_mode && s.buttons==0 && s.lx==0 && s.rx==0 && s.ry==0);
    begin(&s,0,DM_TC_MAIN); drag(&s,0,DM_TC_MAIN,60,0); CHECK(s.buttons==DM_BTN_A && s.rx==0 && s.ry==0);
    dm_tc_move(&s,0,map.x,map.y); CHECK(!s.map_cancel_active && s.buttons==DM_BTN_A);
    dm_tc_set_navigation_mode(&s,1); CHECK(s.buttons==DM_BTN_A); /* identical mode does not cancel */
    dm_tc_set_navigation_mode(&s,0); CHECK(!s.navigation_mode && s.buttons==0 && s.rx==0 && s.ry==0);
    begin(&s,0,DM_TC_SKILL3); drag(&s,0,DM_TC_SKILL3,-60,0); CHECK(s.buttons==DM_BTN_Y && s.rx==-32767 && s.ry==0);   /* 按住技能钮拖动：键一直按着，朝拖动方向 | Hold an on-screen skill button and drag: the key stays down, facing the drag direction */
    dm_tc_move(&s,0,NAN,0); CHECK(s.buttons==DM_BTN_Y && s.rx==-32767); dm_tc_tick(&s,s.last_time_s+0.1); dm_tc_end(&s,0); CHECK(s.buttons==0 && s.rx==0 && s.ry==0 && !s.pulse_button);   /* 坏坐标不动；松手键起 | Bad coordinates do nothing; on release the key goes up */
    dm_tc_cancel_all(&s); CHECK(s.buttons==0 && s.rx==0 && s.ry==0 && s.aim_slot==DM_TC_NONE && !s.map_cancel_active && !s.pulse_button);
    begin(&s,0,DM_TC_SKILL4); drag(&s,0,DM_TC_SKILL4,5,-5);
    CHECK(s.buttons==DM_BTN_R && s.rx==0 && s.ry==0);   /* 拖动不过 10 点死区 = 点按：不给方向，原版自动选目标 | A drag that does not pass the 10-point dead zone = a tap: no direction given, the original game auto-selects the target */
    dm_tc_tick(&s,s.last_time_s+0.1); dm_tc_end(&s,0); CHECK(s.buttons==0);
    begin(&s,0,DM_TC_SKILL4); drag(&s,0,DM_TC_SKILL4,0,-60);
    dm_tc_resize(&s,333.5f,187.5f); CHECK(s.rx==0 && s.ry==0 && s.buttons==0 && s.aim_slot==DM_TC_NONE && !s.pulse_button);
    begin(&s,0,DM_TC_MAIN); drag(&s,0,DM_TC_MAIN,15,-15); CHECK(s.rx==16384 && s.ry==16384);
    dm_tc_enable_targets(&s,0); CHECK(s.rx==0 && s.ry==0 && s.buttons==0);
    dm_tc_enable_targets(&s,(1u<<DM_TC_COUNT)-1);
    const int non_skill[]={DM_TC_PORTAL,DM_TC_BELT1,DM_TC_MENU,DM_TC_MAP};
    for(unsigned i=0;i<sizeof(non_skill)/sizeof(*non_skill);i++) {
        begin(&s,0,non_skill[i]); drag(&s,0,non_skill[i],30,-30); CHECK(s.rx==0 && s.ry==0 && s.aim_slot==DM_TC_NONE); dm_tc_end(&s,0);
    }
    dm_tc_resize(&s,667,375); dm_tc_set_portal_binding(&s,DM_BTN_ZL|DM_BTN_B,77,3); begin(&s,3,DM_TC_PORTAL);
    begin(&s,0,DM_TC_MAIN); drag(&s,0,DM_TC_MAIN,60,0);
    CHECK(s.buttons==(DM_BTN_ZL|DM_BTN_A) && s.rx==32767);
    CHECK(dm_tc_portal_ack(&s,s.gesture_epoch,77,3)); CHECK(s.buttons==(DM_BTN_ZL|DM_BTN_B|DM_BTN_A) && s.rx==32767);
    dm_tc_end(&s,0); CHECK(s.buttons==(DM_BTN_ZL|DM_BTN_B) && s.rx==0); dm_tc_end(&s,3); CHECK(s.buttons==0);
    dm_tc_target main=s.layout.target[DM_TC_MAIN];
    dm_tc_begin(&s,0,main.x+10,main.y+10);
    dm_tc_move(&s,0,main.x+40,main.y-20); CHECK(s.rx==16384 && s.ry==16384);
    dm_tc_move(&s,0,map.x,map.y); CHECK(s.map_cancel_active);
    dm_tc_cancel_all(&s); CHECK(!s.map_cancel_active && s.rx==0 && s.ry==0 && s.aim_slot==DM_TC_NONE);
    {   /* 拖动定方向：技能钮按下后先等判定时间；拖过死区带着方向立刻键下；按住中改方向键不动（右轴跟着变，宿主每拍重取）；点得太短补一个带方向的脉冲 | Drag to set direction: after an on-screen skill button is pressed, first wait for the decision time; dragging past the dead zone presses the key immediately with the direction; changing direction while holding leaves the key untouched (the right axis follows, the host re-reads it every tick); a tap that is too short gets one make-up pulse with the direction */
        dm_tc_state t; dm_tc_init(&t,667,375); dm_tc_target k=t.layout.target[DM_TC_SKILL2];
        dm_tc_tick(&t,1.00); dm_tc_begin_at(&t,0,k.x,k.y,1.00); CHECK(t.buttons==0);                         /* 刚按下：等判定 | Just pressed: waiting for the decision */
        dm_tc_move_at(&t,0,k.x+60,k.y,1.03); CHECK(t.buttons==DM_BTN_X && t.rx==32767 && t.ry==0);            /* 0.03 秒就拖过死区：带着方向键下 | Dragged past the dead zone in just 0.03 seconds: key down carrying the direction */
        dm_tc_tick(&t,1.40); dm_tc_move_at(&t,0,k.x,k.y-60,1.40); CHECK(t.buttons==DM_BTN_X && t.rx==0 && t.ry==32767);   /* 改成朝上：键一直按着，右轴是新方向 | Changed to upward: the key stays down, and the right axis is the new direction */
        dm_tc_end_at(&t,0,2.00); CHECK(t.buttons==0 && !t.pulse_button);                                    /* 键按下过：松手键起，不发脉冲 | The key had been pressed: on release the key goes up, no pulse sent */
        dm_tc_tick(&t,3.00); dm_tc_begin_at(&t,0,k.x,k.y,3.00); dm_tc_end_at(&t,0,3.05); CHECK(t.pulse_button==DM_BTN_X && t.buttons==DM_BTN_X && t.rx==0);   /* 点得比判定时间短、没拖：补一个不带方向的脉冲 | Tapped shorter than the decision time, no drag: send one make-up pulse with no direction */
        CHECK(dm_tc_pulse_end(&t,t.pulse_epoch) && t.buttons==0);
        dm_tc_tick(&t,4.00); dm_tc_begin_at(&t,0,k.x,k.y,4.00); dm_tc_tick(&t,4.10); CHECK(t.buttons==DM_BTN_X && t.rx==0);   /* 没拖、过了判定时间：键下，不带方向 | No drag and the decision time has passed: key down, with no direction */
        dm_tc_end_at(&t,0,4.30); CHECK(t.buttons==0 && !t.pulse_button);
        dm_tc_target mn=t.layout.target[DM_TC_MAIN];
        dm_tc_tick(&t,5.00); dm_tc_begin_at(&t,0,mn.x,mn.y,5.00); CHECK(t.buttons==DM_BTN_A); dm_tc_end_at(&t,0,5.03); CHECK(t.buttons==0 && !t.pulse_button);   /* 主钮：按下即键下，点按不等 | Main button: key down on press, a tap does not wait */
        dm_tc_tick(&t,6.00); dm_tc_begin_at(&t,0,k.x,k.y,6.00); dm_tc_tick(&t,6.09); CHECK(t.buttons==DM_BTN_X); dm_tc_end_at(&t,0,6.10); CHECK(t.pulse_button==DM_BTN_X && t.buttons==DM_BTN_X);   /* 技能钮点 0.10 秒：键才按下 0.01 秒就松手 → 补成脉冲，不让游戏漏读 | On-screen skill button tapped for 0.10 seconds: released when the key had been down only 0.01 seconds → made up into a pulse so the game does not miss the read */
        CHECK(dm_tc_pulse_end(&t,t.pulse_epoch) && t.buttons==0);
    }
    {   /* 技能组切换：点一下进第二套。ZL 只在用技能时带（手指按着主钮 / 技能钮、技能脉冲在发），且先于技能键 0.17 秒；十字 / 腰带等不带；导航态不按；取消/改尺寸不丢模式 | Skill set switch: one tap enters the second set. ZL is held only while a skill is in use (a finger is on the main button / an on-screen skill button and the skill pulse is being sent), and leads the skill button key by 0.17 seconds; D-pad / belt etc. do not carry it; not pressed in navigation state; cancel/resize does not lose the mode */
        dm_tc_state t; dm_tc_init(&t,667,375); dm_tc_target w=t.layout.swap;
        CHECK(dm_tc_hit(&t.layout,w.x,w.y)==DM_TC_SWAP && t.skill_set==0);
        dm_tc_begin(&t,0,w.x,w.y); CHECK(t.skill_set==0 && t.buttons==0); dm_tc_end(&t,0); CHECK(t.skill_set==1 && t.buttons==0);
        dm_tc_target k=t.layout.target[DM_TC_SKILL3], mn=t.layout.target[DM_TC_MAIN], mu=t.layout.target[DM_TC_MENU];
        dm_tc_tick(&t,10.0); dm_tc_begin_at(&t,1,k.x,k.y,10.0); CHECK(t.buttons==DM_BTN_ZL);            /* 刚按下技能钮：ZL 先下 | On-screen skill button just pressed: ZL goes down first */
        dm_tc_tick(&t,10.3); CHECK(t.buttons==(DM_BTN_ZL|DM_BTN_Y));                                     /* ZL 领先够了：技能键下，按住期间一直按着 | ZL has led long enough: the skill button key goes down and stays down for as long as the hold lasts */
        dm_tc_end_at(&t,1,10.4); CHECK(t.buttons==0 && t.pulse_button==0);                              /* 松手：一起放，不发脉冲 | Release: let go together, no pulse sent */
        dm_tc_tick(&t,11.0); dm_tc_begin_at(&t,1,k.x,k.y,11.0); dm_tc_end_at(&t,1,11.05); CHECK(t.buttons==DM_BTN_ZL && t.pulse_button==0);   /* 点得太短：先只按 ZL | Tapped too briefly: press only ZL first */
        dm_tc_tick(&t,11.1); CHECK(t.buttons==DM_BTN_ZL); dm_tc_tick(&t,11.18); CHECK(t.buttons==(DM_BTN_ZL|DM_BTN_Y));
        dm_tc_pulse_end(&t,t.pulse_epoch); CHECK(t.buttons==0);
        dm_tc_tick(&t,12.0); dm_tc_begin_at(&t,1,mn.x,mn.y,12.0); CHECK(t.buttons==DM_BTN_ZL);          /* 主钮：ZL 先下 | Main button: ZL goes down first */
        dm_tc_tick(&t,12.1); CHECK(t.buttons==DM_BTN_ZL); dm_tc_tick(&t,12.2); CHECK(t.buttons==(DM_BTN_ZL|DM_BTN_A));   /* 0.17 秒后 A 下，按住期间一直按着 | A goes down 0.17 seconds later and stays down for as long as the hold lasts */
        dm_tc_tick(&t,13.0); CHECK(t.buttons==(DM_BTN_ZL|DM_BTN_A)); dm_tc_end_at(&t,1,13.1); CHECK(t.buttons==0);
        dm_tc_begin_at(&t,1,mu.x,mu.y,14.0); CHECK(t.buttons==DM_BTN_PLUS); dm_tc_end_at(&t,1,14.1); CHECK(t.buttons==0);   /* 十字：不带 ZL | D-pad: without ZL */
        { dm_tc_target bl=t.layout.target[DM_TC_BELT1]; dm_tc_tick(&t,15.0); dm_tc_begin_at(&t,1,bl.x,bl.y,15.0); CHECK(t.buttons==0); dm_tc_end_at(&t,1,15.05);   /* 腰带：松手即喝，不带 ZL（带了 = 喂佣兵） | belt: drink on release, without ZL (with it = feed mercenary) */
          dm_tc_tick(&t,15.05+0.01); CHECK(t.buttons==DM_BTN_LEFT); dm_tc_pulse_end(&t,t.pulse_epoch); CHECK(t.buttons==0); }
        dm_tc_cancel_all(&t); CHECK(t.skill_set==1 && t.buttons==0);
        dm_tc_resize(&t,812,375); CHECK(t.skill_set==1);
        dm_tc_set_navigation_mode(&t,1); begin(&t,0,DM_TC_MAIN); CHECK(t.buttons==DM_BTN_A); dm_tc_end(&t,0);
        dm_tc_begin(&t,0,t.layout.swap.x,t.layout.swap.y); CHECK(t.skill_set==1 && t.buttons==0); dm_tc_end(&t,0);
        dm_tc_set_navigation_mode(&t,0); w=t.layout.swap;
        dm_tc_begin(&t,0,w.x,w.y); dm_tc_end(&t,0); CHECK(t.skill_set==0 && t.buttons==0);
    }
    printf("PASS skill drag axes/ownership/map-lock/navigation/resize/Portal coexistence/skill-set swap %d/%d checks\n",checks,checks);
    return 0;
}
