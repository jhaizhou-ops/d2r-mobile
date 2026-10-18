#include "dm_touch_controls.h"
#include "../../host/dm_input.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int checks, cases;
#define CHECK(expr) do { ++checks; if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
#define NEAR(a,b,e) CHECK(fabs((double)(a)-(double)(b)) <= (e))
#define RUN(f) do { int before = checks; f(); ++cases; printf("PASS %s (%d checks)\n", #f, checks-before); } while (0)

static const uint64_t keys[DM_TC_COUNT] = {
    DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_R, 0,   /* 回城钮不出按键：点下由宿主施放回城 | The town portal button emits no key: on tap the host casts town portal */
    DM_BTN_LEFT, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_RIGHT, DM_BTN_PLUS, DM_BTN_MINUS
};
static void press(dm_tc_state *s, int slot, int target) {
    dm_tc_target t = s->layout.target[target];
    dm_tc_begin(s, slot, t.x, t.y);
    if (target >= DM_TC_MAIN && target <= DM_TC_SKILL4) dm_tc_tick(s, s->last_time_s + 0.1);   /* 主钮 / 技能钮按下后有判定时间（点按还是拖动），这里的「按下」= 已过判定时间 | After the main button / a skill button is pressed there is a decision time (tap or drag); here "pressed" = the decision time has passed */
}
static int g_tall;   /* 本文件里跟着 dm_tc_set_tall_scale 一起设 | Set together with dm_tc_set_tall_scale in this file */
static void layout_at(float w, float h) {
    dm_tc_layout l; dm_tc_layout_make(&l, w, h);
    NEAR(l.width,w,0); NEAR(l.height,h,0);
    for (int i=0; i<DM_TC_COUNT; ++i) {
        dm_tc_target a=l.target[i];
        CHECK(a.r>0 && isfinite(a.r));
        CHECK(a.x-a.r>=0 && a.x+a.r<=w);
        CHECK(a.y-a.r>=0 && a.y+a.r<=h);
        CHECK(a.button==keys[i]);
        CHECK(dm_tc_hit(&l,a.x,a.y)==i);
        for (int j=i+1; j<DM_TC_COUNT; ++j) {
            dm_tc_target b=l.target[j];
            CHECK(hypotf(a.x-b.x,a.y-b.y)>a.r+b.r);
        }
    }
    float k = g_tall ? 1.15f : 1;   // 平板放大有固定上限，不随屏幕高度增长 | Tablet boost is bounded independently of display height.
    NEAR(l.target[DM_TC_MAIN].r,40*k,0.001);
    for (int i=DM_TC_SKILL1; i<=DM_TC_SKILL4; ++i) {
        NEAR(l.target[i].r,26*k,0.001);
        CHECK(hypotf(l.target[i].x-l.target[0].x,l.target[i].y-l.target[0].y)>=96*k);
        CHECK(hypotf(l.target[i].x-l.target[0].x,l.target[i].y-l.target[0].y)<=100*k);
    }
    /* 底部 UI v4 六格（第 0 格难度）：腰带四列 → 第 1–4 格，功能入口 → 第 5 格；效果图 2868×1320 里格心 x = 1090 + 120×格号、y = 1241.5、
       格半宽 57，按屏高换算、以屏宽中线（效果图 1434）居中。
       Bottom UI v4 six cells (cell 0 is difficulty): the four belt columns → cells 1–4, function entry → cell 5; in the 2868×1320 mockup the cell center x = 1090 + 120 × cell number, y = 1241.5,
              cell half-width 57, converted by screen height and centered on the screen-width center line (mockup 1434). */
    const int order[5] = {DM_TC_BELT1, DM_TC_BELT2, DM_TC_BELT3, DM_TC_BELT4, DM_TC_MENU};
    float u = h/1320;
    for (int c=0; c<5; ++c) {
        dm_tc_target t=l.target[order[c]];
        NEAR(t.r,57*u,0.001);
        NEAR(t.y,1241.5*u,0.001);
        NEAR(t.x,w/2+(1090+120*(c+1)-1434)*u,0.01);
    }
    /* 回城钮（不在底栏）：技能弧最上面那颗（技能四）正上方的圆钮，同半径、间距 12 | Town portal button (not in the bottom bar): the round button directly above the topmost one on the skill arc (skill four), same radius, gap 12 */
    { dm_tc_target p=l.target[DM_TC_PORTAL], s4=l.target[DM_TC_SKILL4];
      NEAR(p.r,26*k,0.001); NEAR(p.x,s4.x,0.001); NEAR(p.y,s4.y-s4.r-12*k-p.r,0.001); }
}
/* 难度格：第 0 格 = 腰带第 1 格左邻一个格距；点开选择条，选第 8 格 → diff_request=8 并收起；选择期间不发任何按键 | Difficulty cell: cell 0 = one cell pitch to the left of belt cell 1; tap to open the selector strip, select cell 8 → diff_request=8 and collapse; no key is sent during selection */
static void difficulty_picker(void) {
    dm_tc_state s; dm_tc_init(&s,956,440);
    dm_tc_layout *l=&s.layout; float pitch=440.0f*120/1320;
    NEAR(l->diff.x,l->target[DM_TC_BELT1].x-pitch,0.01); NEAR(l->diff.y,l->target[DM_TC_BELT1].y,0.01);
    NEAR(l->pick[7].x-l->pick[0].x,7*pitch,0.01); CHECK(l->pick[0].y+l->pick[0].r<l->diff.y-l->diff.r);
    NEAR((l->pick[3].x+l->pick[4].x)/2,l->width/2+(1390-1434)*440.0f/1320,0.01);   /* 选择条中线 = 两球中线（效果图 1390） | Selector strip center line = the center line between the two orbs (mockup 1390) */
    dm_tc_begin(&s,0,l->diff.x,l->diff.y); CHECK(s.diff_open==1 && s.buttons==0); dm_tc_end(&s,0); CHECK(s.buttons==0);
    dm_tc_begin(&s,0,l->pick[7].x,l->pick[7].y); CHECK(s.diff_request==8 && s.diff_open==0 && s.buttons==0); dm_tc_end(&s,0);
    s.diff_request=0;
    dm_tc_begin(&s,0,l->diff.x,l->diff.y); dm_tc_end(&s,0); CHECK(s.diff_open==1);
    press(&s,0,DM_TC_BELT2); CHECK(s.diff_open==0 && s.diff_request==0 && s.buttons==0); dm_tc_end(&s,0);   /* 点别处：收起并照常处理（腰带按下不再即喝，松手后等单双击，见 belt_tap） | Tap elsewhere: collapses and is handled as usual (a belt press no longer drinks immediately; it waits for single / double tap after release, see belt_tap) */
    dm_tc_begin(&s,0,l->diff.x,l->diff.y); dm_tc_end(&s,0); dm_tc_set_navigation_mode(&s,1); CHECK(s.diff_open==0);
    dm_tc_begin(&s,0,l->diff.x,l->diff.y); CHECK(s.diff_open==0);   /* 导航模式（功能页）不理难度格 | Navigation mode (function page) ignores the difficulty cell */
}
static void layout_667(void) { layout_at(667,375); }
static void layout_812(void) { layout_at(812,375); }
static void layout_956(void) { layout_at(956,440); }
static void layout_1024(void) { layout_at(1024,768); }
static void layout_ipad169(void) { dm_tc_set_tall_scale(g_tall = 1); layout_at(1366,768); dm_tc_set_tall_scale(g_tall = 0); }   /* 13 寸 iPad 上居中的 16:9 块：开了放大 | The centered 16:9 block on the 13-inch iPad: scale-up is on */
static void layout_pixels(void) { layout_at(2868,1320); }   /* Mac 宿主触屏用例通路把像素数当点数传：没开放大，主钮半径仍是 40、位置不变 | The Mac host touch-case path passes pixel counts as point counts: scale-up is not on, the main button radius is still 40 and its position is unchanged */
static void small_layout(void) {
    dm_tc_layout l; dm_tc_layout_make(&l,333.5f,187.5f);
    NEAR(l.target[0].r,20,0.001); NEAR(l.joy_radius,30,0.001);
    for(int i=0;i<DM_TC_COUNT;i++) {
        CHECK(l.target[i].x>=l.target[i].r && l.target[i].x+l.target[i].r<=l.width);
        CHECK(l.target[i].y>=l.target[i].r && l.target[i].y+l.target[i].r<=l.height);
        for(int j=i+1;j<DM_TC_COUNT;j++) CHECK(hypotf(l.target[i].x-l.target[j].x,l.target[i].y-l.target[j].y)>l.target[i].r+l.target[j].r);
    }
}
static void hit_edges(void) {
    dm_tc_layout l; dm_tc_layout_make(&l,667,375);
    for(int i=0;i<DM_TC_COUNT;i++) {
        dm_tc_target t=l.target[i];
        CHECK(dm_tc_hit(&l,t.x+t.r,t.y)==i);
        CHECK(dm_tc_hit(&l,t.x+t.r+0.01f,t.y)==DM_TC_NONE);
    }
    CHECK(dm_tc_hit(&l,-1,50)==DM_TC_NONE);
    CHECK(dm_tc_hit(&l,800,50)==DM_TC_NONE);
    CHECK(dm_tc_hit(&l,NAN,50)==DM_TC_NONE);
}
static void key_hold_and_ownership(void) {
    dm_tc_state s; dm_tc_init(&s,667,375);
    for(int i=0;i<DM_TC_COUNT;i++) {
        if(i>=DM_TC_BELT1&&i<=DM_TC_BELT4) continue;   /* 腰带格松手后等单双击，见 belt_single_double | A belt slot waits for single / double tap after release, see belt_single_double */
        int release=0;   /* 技能钮与主钮同一套：按下即键下、松手键起（按住 = 持续施放） | Skill buttons use the same scheme as the main button: key down on press, key up on release (hold = continuous cast) */
        press(&s,0,i);
        CHECK(s.buttons==(release?0:keys[i]));
        dm_tc_move(&s,0,350,100); CHECK(s.buttons==(release?0:keys[i]));
        dm_tc_move(&s,0,s.layout.target[0].x,s.layout.target[0].y); CHECK(s.buttons==(release?0:keys[i]));
        dm_tc_tick(&s,s.last_time_s+0.1); dm_tc_end(&s,0); CHECK(s.buttons==(release?keys[i]:0));
        if(release) { CHECK(dm_tc_pulse_end(&s,s.pulse_epoch)); CHECK(s.buttons==0); }
    }
}
static void two_fingers_same_key(void) {
    dm_tc_state s; dm_tc_init(&s,667,375);
    press(&s,0,DM_TC_MAIN); press(&s,1,DM_TC_MAIN);
    CHECK(s.buttons==DM_BTN_A); dm_tc_end(&s,0); CHECK(s.buttons==DM_BTN_A);
    dm_tc_end(&s,1); CHECK(s.buttons==0);
}
static void joystick_deadzone(void) {
    /* 浮动底盘。667×375（k = 1、安全区 0）手算：静止位 (max(90, 0+60)+30, 375−115) = (120, 260)；起摇杆区 x < 667×0.40 = 266.8 且 y > 375×0.45 = 168.75；
       夹回只为底盘整圈在屏内：x ≥ 60+8 = 68、y ≤ 375−60−8 = 307（不让安全区）。落在区里起手：圆心 = 落点，位移为零
       Floating base. Worked by hand for 667x375 (k = 1, safe area 0): rest position (max(90, 0+60)+30, 375-115) = (120, 260); stick-start zone x < 667x0.40 = 266.8 and y > 375x0.45 = 168.75;
              clamping only keeps the whole base on screen: x >= 60+8 = 68, y <= 375-60-8 = 307 (not respecting the safe area). Starting inside the zone: centre = touch point, zero displacement */
    dm_tc_state s; dm_tc_init(&s,667,375); NEAR(s.layout.joy_x,120,0.001); NEAR(s.layout.joy_y,260,0.001);
    NEAR(s.layout.joy_zone_r,266.8,0.001); NEAR(s.layout.joy_zone_t,168.75,0.001); NEAR(s.layout.joy_min_x,68,0.001); NEAR(s.layout.joy_max_y,307,0.001);
    dm_tc_begin(&s,0,90,275);
    CHECK(s.joy_slot==0); NEAR(s.joy_origin_x,90,0); NEAR(s.joy_origin_y,275,0);
    CHECK(s.lx==0 && s.ly==0);
    dm_tc_move(&s,0,96,275); CHECK(s.lx==0 && s.ly==0);
    dm_tc_move(&s,0,96.1f,275); CHECK(s.lx>0 && s.lx<100 && s.ly==0);
    dm_tc_move(&s,0,123,275); CHECK(abs(s.lx-16384)<=1 && s.ly==0);
    dm_tc_end(&s,0);
    /* 区里别处：圆心跟落点；按住后拖到多远都照样识别，底盘不跟手走；松手后静止位不变 | elsewhere in the zone: the centre follows the touch point; once held, dragging any distance is still recognised and the base does not follow the finger; after release the rest position is unchanged */
    dm_tc_begin(&s,0,200,200); CHECK(s.joy_slot==0); NEAR(s.joy_origin_x,200,0); NEAR(s.joy_origin_y,200,0); CHECK(s.lx==0 && s.ly==0);
    dm_tc_move(&s,0,600,200); CHECK(s.lx==DM_STICK_MAX && s.ly==0); NEAR(s.joy_origin_x,200,0);
    dm_tc_move(&s,0,200,25); CHECK(s.lx==0 && s.ly==DM_STICK_MAX); NEAR(s.joy_origin_y,200,0);
    dm_tc_end(&s,0); CHECK(s.joy_slot==-1 && s.lx==0 && s.ly==0); NEAR(s.layout.joy_x,120,0.001);
    /* 贴左下角落指 (10,370)：底盘画在夹回后的 (68,307)，轴值按手指相对落点算——落指那一下为 0；向右挪 6 点仍在死区；7 点起出值 (7−6)/54 × 32767 = 607；
       向右拖满 60 点 = 向右满偏（不是按底盘圆心算出来的「左下」）。底盘不跟手走
       touching down against the bottom-left corner (10,370): the base is drawn at the clamped (68,307), and the axes are computed from the finger relative to the touch point - 0 on touch-down; moving 6 points right is still inside the dead zone; from 7 points the output is (7-6)/54 x 32767 = 607;
              dragging the full 60 points right = full right deflection (not the "down-left" one would get by computing from the base centre). The base does not follow the finger */
    dm_tc_begin(&s,0,10,370); CHECK(s.joy_slot==0); NEAR(s.joy_origin_x,68,0); NEAR(s.joy_origin_y,307,0); CHECK(s.lx==0 && s.ly==0);
    dm_tc_move(&s,0,16,370); CHECK(s.lx==0 && s.ly==0);
    dm_tc_move(&s,0,17,370); CHECK(abs(s.lx-607)<=1 && s.ly==0);
    dm_tc_move(&s,0,70,370); CHECK(s.lx==DM_STICK_MAX && s.ly==0); NEAR(s.joy_origin_x,68,0); NEAR(s.joy_origin_y,307,0);
    dm_tc_end(&s,0);
    dm_tc_begin(&s,0,266,169); CHECK(s.joy_slot==0); NEAR(s.joy_origin_x,266,0); NEAR(s.joy_origin_y,169,0); dm_tc_end(&s,0);   /* 区的右上角以内 | just inside the zone's top-right corner */
    /* 区外：不起摇杆，拖也不变成摇杆 | outside the zone: no stick starts, and dragging does not turn into a stick either */
    dm_tc_begin(&s,0,267,275); CHECK(s.joy_slot==-1 && s.lx==0); dm_tc_end(&s,0);
    dm_tc_begin(&s,0,100,168); CHECK(s.joy_slot==-1 && s.lx==0); dm_tc_end(&s,0);
    dm_tc_begin(&s,0,300,150); CHECK(s.joy_slot==-1 && s.lx==0 && s.ly==0); dm_tc_move(&s,0,200,250); CHECK(s.joy_slot==-1 && s.lx==0); dm_tc_end(&s,0);
    /* 区里很快原地松开的那一下兼作点击（落点记成点世界），按久了 / 拖开了的不算 | a quick release in place inside the zone doubles as a tap (the touch point is recorded as a world tap); held long / dragged away does not count */
    { unsigned q = s.world_tap_seq; dm_tc_begin_at(&s,0,150,250,40.0); dm_tc_end_at(&s,0,40.1); CHECK(s.world_tap_seq==q+1); NEAR(s.world_tap_x,150,0); NEAR(s.world_tap_y,250,0);
      dm_tc_begin_at(&s,0,150,250,41.0); dm_tc_move_at(&s,0,200,250,41.05); dm_tc_end_at(&s,0,41.1); CHECK(s.world_tap_seq==q+1); }
    /* 拖远了的摇杆手指被系统取消，0.2 秒后在离取消处 40 点的 (236,128) 重新落下（(260,160) 偏 (−24,−32)；已在起摇杆区上界 168.75 之外）：接着当摇杆，落点与底盘沿用取消前的 (90,275)、方向按旧落点算；超时：不接 | the stick finger, dragged far away, is cancelled by the system and 0.2 s later touches down again at (236,128), 40 points from where it was cancelled ((260,160) offset by (-24,-32); already above the zone's upper bound 168.75): it continues as the stick, the touch point and base stay at (90,275) from before the cancel and the direction is computed from the old touch point; after the timeout: not continued */
    dm_tc_begin_at(&s,0,90,275,50.0); dm_tc_move_at(&s,0,260,160,50.2); CHECK(s.lx>0 && s.ly>0); dm_tc_cancel_finger(&s,0); CHECK(s.joy_slot==-1 && s.lx==0);
    dm_tc_begin_at(&s,1,236,128,50.4); CHECK(s.joy_slot==1 && s.lx>0 && s.ly>0); NEAR(s.joy_origin_x,90,0); NEAR(s.joy_origin_y,275,0); dm_tc_cancel_finger(&s,1);
    dm_tc_begin_at(&s,2,236,128,50.4+0.36); CHECK(s.joy_slot==-1); dm_tc_end_at(&s,2,51.5);          /* 超过 0.35 秒、落点在区外 | beyond 0.35 s, touch point outside the zone */
    /* 取消后在区里别处重落（离取消处 140 点，不算接续）：按新落点起一个新的，圆心 (265, min(300,307)) = (265,300)，落指那一下轴为 0 | after a cancel, touching down elsewhere in the zone (140 points from where it was cancelled, not a continuation): a new stick starts at the new touch point, centre (265, min(300,307)) = (265,300), axes 0 on touch-down */
    dm_tc_begin_at(&s,0,90,275,52.0); dm_tc_move_at(&s,0,260,160,52.2); dm_tc_cancel_finger(&s,0);
    dm_tc_begin_at(&s,1,265,300,52.3); CHECK(s.joy_slot==1); NEAR(s.joy_origin_x,265,0); NEAR(s.joy_origin_y,300,0); CHECK(s.lx==0 && s.ly==0); dm_tc_end_at(&s,1,53.5);
}
/* 956×440（k = 1）手算：夹回界 x ≥ 60+8 = 68、y ≤ 440−68 = 372，只决定底盘画在哪：(6,434) → (68,372)；(60,400) → (68,372)；(150,330) → 不夹。
   三处落指那一下轴都为 0；各自向右、向上、向左上（45°）拖 60 点，轴值只看手指相对落点，三处完全相同：
   向右 (60,0) → (32767, 0)；向上 (0,60) → (0, 32767)；向左上 (−42.43, 42.43) → 斜向预修正后 (−1, 2)/√5 × 32767 = (−14654, 29308)（走出来的屏幕方向 = 手指方向，同 joystick_axes 那例）。
   贴左下角时向左 / 向下拖不出 60 点（手指到屏幕边了）是物理限制，不在这里测
   Worked by hand for 956x440 (k = 1): clamp bounds x >= 60+8 = 68, y <= 440-68 = 372, which only decide where the base is drawn: (6,434) -> (68,372); (60,400) -> (68,372); (150,330) -> not clamped.
      At all three touch points the axes are 0 on touch-down; dragging 60 points right, up and up-left (45 degrees) from each, the axes depend only on the finger relative to the touch point and are identical for all three:
      right (60,0) -> (32767, 0); up (0,60) -> (0, 32767); up-left (-42.43, 42.43) -> after the diagonal pre-correction (-1, 2)/sqrt(5) x 32767 = (-14654, 29308) (the screen direction walked = the finger direction, as in the joystick_axes example).
      Near the bottom-left corner a drag of 60 points to the left / downwards is not possible (the finger reaches the screen edge); that is a physical limit and not tested here */
static void joystick_clamp_956(void) {
    static const float at[3][4] = {{6, 434, 68, 372}, {60, 400, 68, 372}, {150, 330, 150, 330}};
    dm_tc_state s; dm_tc_init(&s,956,440); NEAR(s.layout.joy_min_x,68,0.001); NEAR(s.layout.joy_max_y,372,0.001);
    for (int i = 0; i < 3; i++) { float x = at[i][0], y = at[i][1];
        dm_tc_begin(&s,0,x,y); NEAR(s.joy_origin_x,at[i][2],0); NEAR(s.joy_origin_y,at[i][3],0); CHECK(s.lx==0 && s.ly==0);
        dm_tc_move(&s,0,x+60,y); CHECK(s.lx==DM_STICK_MAX && s.ly==0);
        dm_tc_move(&s,0,x,y-60); CHECK(s.lx==0 && s.ly==DM_STICK_MAX);
        dm_tc_move(&s,0,x-42.4264f,y-42.4264f); CHECK(abs(s.lx+14654)<=2 && abs(s.ly-29308)<=2);
        NEAR(s.joy_origin_x,at[i][2],0); NEAR(s.joy_origin_y,at[i][3],0); dm_tc_end(&s,0); }
}
static void joystick_axes(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_begin(&s,0,90,275);   /* 圆心 90,275；下面各点 = 原用例整体平移 (−10, +75) | center 90,275; the points below = the original cases shifted as a whole by (−10, +75) */
    dm_tc_move(&s,0,150,275); CHECK(s.lx==DM_STICK_MAX && s.ly==0);
    dm_tc_move(&s,0,30,275); CHECK(s.lx==-DM_STICK_MAX && s.ly==0);
    dm_tc_move(&s,0,90,215); CHECK(s.lx==0 && s.ly==DM_STICK_MAX);
    dm_tc_move(&s,0,90,335); CHECK(s.lx==0 && s.ly==-DM_STICK_MAX);
    dm_tc_move(&s,0,690,275); CHECK(s.lx==DM_STICK_MAX && s.ly==0);
    dm_tc_move(&s,0,150,215); CHECK(abs(s.lx-14654)<=1 && abs(s.ly-29308)<=1);   /* 手指 45°：斜向预修正后 (1,2)/√5 × 32767 = (14654, 29308)，幅度不变 | Finger at 45°: after diagonal pre-correction (1,2)/√5 × 32767 = (14654, 29308), magnitude unchanged */
    dm_tc_move(&s,0,90+60*0.8660254f,275-30); CHECK(abs(s.lx-21451)<=2 && abs(s.ly-24769)<=2);   /* 手指 30°：(cos30, 2 sin30)/√(0.75+1) × 32767 = (21451, 24769) | Finger at 30°: (cos30, 2 sin30)/√(0.75+1) × 32767 = (21451, 24769) */
    dm_tc_set_iso_fix(0); dm_tc_move(&s,0,150,215); CHECK(abs(s.lx-23170)<=1 && abs(s.ly-23170)<=1); dm_tc_set_iso_fix(1);
    dm_tc_end(&s,0); CHECK(s.joy_slot==-1 && s.lx==0 && s.ly==0);
}
static void joystick_plus_skills(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_begin(&s,0,90,275); dm_tc_move(&s,0,150,275);
    for(int i=0;i<5;i++) press(&s,i+1,i);
    uint64_t all=keys[0]|keys[1]|keys[2]|keys[3]|keys[4];
    CHECK(s.buttons==all); CHECK(s.lx==DM_STICK_MAX);   /* 主钮与四个技能钮：按着的键都按着（按住 = 持续施放） | Main button and the four skill buttons: every pressed key stays down (hold = continuous cast) */
    dm_tc_end(&s,3); CHECK(s.buttons==(all&~keys[2]) && s.lx==DM_STICK_MAX);   /* 移动中松开一个：只有它的键起 | Release one while moving: only its key goes up */
    dm_tc_end(&s,0); CHECK(s.lx==0 && s.ly==0); CHECK(s.buttons==(all&~keys[2]));
}
static void no_auto_handoff(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_begin(&s,0,90,275); dm_tc_begin(&s,1,150,295);   /* 第二根手指也落在底座附近：摇杆已有主，它不接手 | a second finger also lands near the base: the joystick already has an owner, so it does not take over */
    dm_tc_end(&s,0); dm_tc_move(&s,1,210,295);
    CHECK(s.joy_slot==-1 && s.lx==0 && s.ly==0 && s.buttons==0);
    dm_tc_begin(&s,2,90,275); CHECK(s.joy_slot==2);
}
static void background_no_key(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_begin(&s,0,400,100);
    CHECK(s.buttons==0 && s.joy_slot==-1);
    dm_tc_move(&s,0,s.layout.target[0].x,s.layout.target[0].y); CHECK(s.buttons==0);
    dm_tc_begin(&s,1,10,10); dm_tc_move(&s,1,100,200); CHECK(s.joy_slot==-1 && s.buttons==0);
    dm_tc_begin(&s,2,667*0.43f,200); CHECK(s.joy_slot==-1);
    dm_tc_begin(&s,3,100,375*0.25f); CHECK(s.joy_slot==-1);
}
static void circles_before_joystick(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); press(&s,0,DM_TC_PORTAL);
    CHECK(s.buttons==0 && s.joy_slot==-1); CHECK(s.finger[0].target==DM_TC_PORTAL);   /* 圆钮先于摇杆认领这根手指；回城钮自己不出按键 | Round buttons claim this finger before the stick does; the town portal button itself emits no key */
}
static void cancel_and_resize(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_begin(&s,0,100,200); dm_tc_move(&s,0,160,200); press(&s,1,DM_TC_MAIN);
    dm_tc_cancel_all(&s); CHECK(s.buttons==0 && s.lx==0 && s.ly==0 && s.joy_slot==-1);
    for(int i=0;i<DM_TC_MAX_TOUCH;i++) CHECK(!s.finger[i].active);
    dm_tc_begin(&s,0,100,200); press(&s,1,DM_TC_MAIN); dm_tc_resize(&s,812,375);
    CHECK(s.buttons==0 && s.lx==0 && s.ly==0 && s.joy_slot==-1); NEAR(s.layout.width,812,0);
    for(int i=0;i<DM_TC_MAX_TOUCH;i++) CHECK(!s.finger[i].active);
    dm_tc_move(&s,1,s.layout.target[0].x,s.layout.target[0].y); CHECK(s.buttons==0);
}
static void invalid_slots_and_repeated_begin(void) {
    dm_tc_state s; dm_tc_init(&s,667,375);
    press(&s,-1,0); press(&s,DM_TC_MAX_TOUCH,0); dm_tc_move(&s,-1,1,1); dm_tc_end(&s,DM_TC_MAX_TOUCH);
    CHECK(s.buttons==0); dm_tc_begin(&s,0,NAN,10); CHECK(!s.finger[0].active);
    press(&s,0,DM_TC_MAIN); press(&s,0,DM_TC_SKILL1); CHECK(s.buttons==DM_BTN_A);
    dm_tc_move(&s,0,NAN,INFINITY); CHECK(s.buttons==DM_BTN_A);
    dm_tc_end(&s,0); CHECK(s.buttons==0);
    dm_tc_begin(&s,0,-1,200); dm_tc_begin(&s,1,100,500); CHECK(s.joy_slot==-1);
}
static void all_slots_hold(void) {
    dm_tc_state s; dm_tc_init(&s,667,375);
    for(int i=0;i<DM_TC_MAX_TOUCH;i++) press(&s,i,DM_TC_MAIN);
    for(int i=0;i<DM_TC_MAX_TOUCH-1;i++) { dm_tc_end(&s,i); CHECK(s.buttons==DM_BTN_A); }
    dm_tc_end(&s,DM_TC_MAX_TOUCH-1); CHECK(s.buttons==0);
}
static void enabled_targets(void) {
    dm_tc_state s; dm_tc_init(&s,667,375);
    CHECK(s.enabled_targets == ((1u<<DM_TC_COUNT)-1));
    dm_tc_begin(&s,0,100,200); dm_tc_move(&s,0,160,200); press(&s,1,DM_TC_MAIN);
    uint32_t ui = (1u<<DM_TC_MAIN)|(1u<<DM_TC_SKILL1)|(1u<<DM_TC_SKILL2)|
        (1u<<DM_TC_SKILL3)|(1u<<DM_TC_SKILL4)|(1u<<DM_TC_MENU);
    dm_tc_enable_targets(&s,ui);
    CHECK(s.enabled_targets==ui); CHECK(s.buttons==0 && s.lx==0 && s.ly==0 && s.joy_slot==-1);
    for(int i=0;i<DM_TC_COUNT;i++) {
        press(&s,0,i);
        CHECK(s.buttons==((ui&(1u<<i))?keys[i]:0));
        CHECK(s.joy_slot==-1); dm_tc_end(&s,0); dm_tc_pulse_end(&s,s.pulse_epoch);
    }
    press(&s,0,DM_TC_MAIN); dm_tc_enable_targets(&s,ui); CHECK(s.buttons==DM_BTN_A);
    dm_tc_enable_targets(&s,0); CHECK(s.buttons==0);
    press(&s,0,DM_TC_SKILL1); CHECK(s.buttons==0 && s.joy_slot==-1);   /* 全禁用时点钮不出键、也不落进摇杆；重新启用后照常出键（原先用回城钮验，它 不出按键，换成技能一） | When everything is disabled, tapping a button emits no key and does not fall through to the stick either; after re-enabling, keys are emitted as usual (this used to be verified with the town portal button, which emits no key, so it was changed to skill one) */
    dm_tc_end(&s,0); dm_tc_enable_targets(&s,(1u<<DM_TC_COUNT)-1); press(&s,0,DM_TC_SKILL1); CHECK(s.buttons==DM_BTN_B);
}
static void ui_scene_messages(void) {
    dm_tc_scene scene={0};
    CHECK(dm_tc_navigation(&scene,"FrontEndNavigation","ToFrontEnd","")==1);
    CHECK(dm_tc_navigation(&scene,"PanelManager","OpenPanel","LoadScreenPanel")==1);
    CHECK(dm_tc_navigation(&scene,"PanelManager","UnloadPanel","LoadScreenPanel")==0);
    CHECK(dm_tc_navigation(&scene,"PanelManager","OpenPanel","PauseLayoutGarden")==1);
    CHECK(dm_tc_navigation(&scene,"PanelManager","ClosePanel","PauseLayoutGarden")==0);
    CHECK(dm_tc_navigation(&scene,"PanelManager","OpenPanel","AutoMap")==0);
    CHECK(dm_tc_navigation(&scene,"PanelManager","OpenExclusivePanel","BankExpansionLayout")==1);
    CHECK(dm_tc_navigation(&scene,"PanelManager","OpenPanel","PlayerInventoryExpansionLayout")==1);
    CHECK(dm_tc_navigation(&scene,"PanelManager","ClosePanel","BankExpansionLayout")==1);
    CHECK(dm_tc_navigation(&scene,"PanelManager","ClosePanel","PlayerInventoryExpansionLayout")==0);
    CHECK(dm_tc_navigation(&scene,"PanelManager","OpenPanel","HUDPanel")==0);
    CHECK(dm_tc_navigation(&scene,"PanelManager","OpenPanel","SkillsTreePanel")==1);
    CHECK(dm_tc_navigation(&scene,"PanelManager","UnloadAll","")==0);
    CHECK(dm_tc_navigation(&scene,"FrontEndNavigation","ToFrontEnd","")==1);
}
static void pointer_tap(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_end_at(&s,0,10.2);
    CHECK(s.pointer_valid); NEAR(s.pointer_x,300,0.001); NEAR(s.pointer_y,180,0.001);
    CHECK(s.pulse_button==DM_BTN_A); CHECK(s.buttons==DM_BTN_A);
}
/* 物品区（前端松手前置 pointer_item）：单击只移光标不发键；0.4 秒内同处再点 = 按住 A 0.6 秒；开着储藏箱时 = 按住 Y；隔久了再点仍是单击 | Item area (the front end sets pointer_item before release): a single tap only moves the cursor and sends no key; another tap at the same spot within 0.4 seconds = hold A for 0.6 seconds; with the stash open = hold Y; a tap after a long gap is still a single tap */
/* 高级仓库的成叠格（命中类 8）：单击只移光标；双击 = stack_take_seq 加一（取 1 件），不按住 Y、不计「使用」；开着箱子也一样；长按照物品区计 long_press_seq（宿主取 3 件） | Stack cells of the advanced stash (hit class 8): a single tap only moves the cursor; double tap = stack_take_seq plus one (take 1 item), no hold-Y and no "use" count; the same with the stash open; a long press counts long_press_seq as in the item area (the host takes 3 items) */
static void pointer_stack_tap(void) {
    dm_tc_scene scene={0}; dm_tc_navigation(&scene,"PanelManager","UnloadAll",""); dm_tc_navigation(&scene,"PanelManager","OpenPanel","BankExpansionLayout"); dm_tc_set_panel_state(1);
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1); s.mouse_click_mode=1;
    dm_tc_begin_at(&s,0,104,91,10.0); dm_tc_pointer_item(&s,8); dm_tc_end_at(&s,0,10.1);
    CHECK(s.pointer_valid); NEAR(s.pointer_x,104,0.001); CHECK(s.stack_take_seq==0); CHECK(s.pulse_button==0);
    dm_tc_begin_at(&s,0,104,91,10.3); dm_tc_pointer_item(&s,8); dm_tc_end_at(&s,0,10.4);
    CHECK(s.stack_take_seq==1); CHECK(s.pulse_button==0); CHECK(s.use_seq==0); CHECK(s.buttons==0);
    dm_tc_begin_at(&s,0,104,91,12.0); dm_tc_pointer_item(&s,8); dm_tc_tick(&s,12.9); dm_tc_end_at(&s,0,12.9);
    CHECK(s.long_press_seq==1); CHECK(s.stack_take_seq==1);
    dm_tc_begin_at(&s,0,104,91,14.0); dm_tc_pointer_item(&s,8); dm_tc_move_at(&s,0,200,150,14.1); CHECK(s.finger[0].shop_drag); CHECK(s.pulse_button==0); CHECK(s.buttons==0);   /* 从一叠上拖出：不拿起（不发键），松手由前端按落点取 1 件 | Dragging out from a stack: no pick up (no key sent), on release the front end takes 1 item according to the drop point */
    dm_tc_end_at(&s,0,14.5); CHECK(s.pulse_button==0); CHECK(s.stack_take_seq==1);
    dm_tc_navigation(&scene,"PanelManager","UnloadAll","");
}
static void pointer_item_tap(void) {
    dm_tc_scene scene={0}; dm_tc_navigation(&scene,"PanelManager","UnloadAll","");
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_pointer_item(&s,1); dm_tc_end_at(&s,0,10.1);
    CHECK(s.pointer_valid); NEAR(s.pointer_x,300,0.001); NEAR(s.pointer_y,180,0.001); CHECK(s.pulse_button==0); CHECK(s.buttons==0);
    dm_tc_begin_at(&s,0,302,181,10.3); dm_tc_pointer_item(&s,1); dm_tc_end_at(&s,0,10.4);
    CHECK(s.pulse_button==0); CHECK(s.use_seq==1);   /* 双击：不发按住 A，计一次交给宿主当场使用 | Double tap: does not send hold-A, counts once and hands it to the host to use on the spot */
    dm_tc_begin_at(&s,0,302,181,10.6); dm_tc_pointer_item(&s,1); dm_tc_end_at(&s,0,10.7); CHECK(s.pulse_button==0);   /* 双击之后的第三下重新算单击 | The third tap after a double tap counts as a single tap again */
    dm_tc_begin_at(&s,0,302,181,12.0); dm_tc_pointer_item(&s,1); dm_tc_end_at(&s,0,12.1); CHECK(s.pulse_button==DM_BTN_ZL); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));   /* 隔 1.3 秒又点同一处：换一件比较（ZL） | Tapped the same spot again 1.3 seconds later: compare against another item (ZL) */
    dm_tc_begin_at(&s,0,400,181,12.2); dm_tc_pointer_item(&s,1); dm_tc_end_at(&s,0,12.3); CHECK(s.pulse_button==0);   /* 换了位置：单击 | Position changed: single tap */
    dm_tc_navigation(&scene,"PanelManager","OpenPanel","BankExpansionLayout"); dm_tc_set_panel_state(1);   /* 储藏箱开着 = 宿主读面板树设进来的位，不靠消息 | Stash open = a bit the host sets from reading the panel tree, not relying on messages */
    dm_tc_begin_at(&s,0,400,181,12.5); dm_tc_pointer_item(&s,1); dm_tc_end_at(&s,0,12.6);
    CHECK(s.pulse_button==DM_BTN_Y); NEAR(s.pulse_hold_s,0.6,0.001); dm_tc_pulse_end(&s,s.pulse_epoch);
    dm_tc_navigation(&scene,"PanelManager","ClosePanel","BankExpansionLayout"); dm_tc_set_panel_state(0);
    dm_tc_begin_at(&s,0,100,100,13.5); dm_tc_end_at(&s,0,13.6); CHECK(s.pulse_button==DM_BTN_A); NEAR(s.pulse_hold_s,0,0.001);   /* 非物品区：照旧点 = A | Outside the item area: tap = A as before */
    dm_tc_pulse_end(&s,s.pulse_epoch); s.mouse_click_mode=1;   /* 前端开了鼠标点击：非物品区点按不按 A，改记一次鼠标点击 | The front end turned on mouse click: a tap outside the item area does not press A, and one mouse click is recorded instead */
    dm_tc_begin_at(&s,0,120,90,14.5); dm_tc_end_at(&s,0,14.6); CHECK(s.pulse_button==0); CHECK(s.mouse_click_seq==1); NEAR(s.mouse_click_x,120,0.001); NEAR(s.mouse_click_y,90,0.001);
    dm_tc_begin_at(&s,0,300,180,15.5); dm_tc_pointer_item(&s,1); dm_tc_end_at(&s,0,15.6); CHECK(s.mouse_click_seq==1); CHECK(s.pulse_button==0);   /* 物品区单击不算鼠标点击 | A single tap in the item area does not count as a mouse click */
}
/* 界面连续手势：鼠标点击模式下起手不在物品区 / 面板外、位移过 10 点阈值 → 按那一刻的方向定型（纵 1 = 滚动，横 2 = 滑块），定型后不变；
   不发任何键、不记鼠标点击（拖出去又拖回原处也不算点按）；物品区起手照旧是拖放；面板外起手不定型；没开鼠标点击模式不定型
   UI continuous gesture: in mouse click mode, when the touch does not start in the item area / outside the panel and the displacement passes the 10-point threshold → the type is fixed by the direction at that moment (vertical 1 = scroll, horizontal 2 = slider), and does not change once fixed;
      no key is sent and no mouse click is recorded (dragging out and back to the original spot does not count as a tap either); a touch starting in the item area is drag-and-drop as before; a touch starting outside the panel fixes no type; with mouse click mode off no type is fixed */
static void ui_drag_typing(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1); s.mouse_click_mode=1;
    dm_tc_begin_at(&s,0,300,200,1.0); dm_tc_move_at(&s,0,302,209,1.1); CHECK(s.finger[0].ui_drag==0);            /* 位移 9.2 点：没过阈值 | Displacement 9.2 points: threshold not passed */
    dm_tc_move_at(&s,0,303,212,1.2); CHECK(s.finger[0].ui_drag==1);                                              /* 过阈值，纵向为主 → 滚动 | Past the threshold, mainly vertical → scroll */
    dm_tc_move_at(&s,0,400,214,1.3); CHECK(s.finger[0].ui_drag==1); CHECK(s.buttons==0); CHECK(s.pulse_button==0); /* 之后横着走也不改型，不发键 | Moving sideways afterward does not change the type either, no key sent */
    dm_tc_move_at(&s,0,300,200,1.4); dm_tc_end_at(&s,0,1.45); CHECK(s.mouse_click_seq==0); CHECK(s.pulse_button==0); /* 拖回原处松手：不算点按 | Dragged back to the original spot and released: does not count as a tap */
    dm_tc_begin_at(&s,0,300,200,2.0); dm_tc_move_at(&s,0,312,204,2.1); CHECK(s.finger[0].ui_drag==2);            /* 横向为主 → 滑块 | Mainly horizontal → slider */
    dm_tc_end_at(&s,0,2.2); CHECK(s.mouse_click_seq==0); CHECK(s.pulse_button==0);
    dm_tc_begin_at(&s,0,300,200,3.0); dm_tc_pointer_item(&s,1); dm_tc_move_at(&s,0,300,230,3.1); CHECK(s.finger[0].ui_drag==0); CHECK(s.finger[0].pointer_dragged);   /* 物品区起手：照旧拖放 | Touch starting in the item area: drag-and-drop as before */
    dm_tc_pointer_release(&s,1); dm_tc_end_at(&s,0,3.2); if (s.pulse_button) dm_tc_pulse_end(&s,s.pulse_epoch);
    dm_tc_begin_at(&s,0,300,200,4.0); dm_tc_pointer_item(&s,3); dm_tc_move_at(&s,0,300,240,4.1); CHECK(s.finger[0].ui_drag==0);   /* 面板外起手：不当滚动 | Touch starting outside the panel: not treated as a scroll */
    dm_tc_end_at(&s,0,4.2); if (s.pulse_button) dm_tc_pulse_end(&s,s.pulse_epoch);
    s.mouse_click_mode=0;
    dm_tc_begin_at(&s,0,300,200,5.0); dm_tc_move_at(&s,0,300,240,5.1); CHECK(s.finger[0].ui_drag==0);            /* 没开鼠标点击模式（手柄光标拖放那条老路）：不定型 | Mouse click mode not on (the old controller-cursor drag-and-drop path): no type is fixed */
    dm_tc_end_at(&s,0,5.2);
}
/* 拖放松手：落点是物品区 = 在落点按 A 放下；不是（拖到面板外）= 指针回起点、按 Y 丢弃（按住 0.15 秒） | Drag-and-drop release: the drop point is the item area = press A at the drop point to put down; it is not (dragged outside the panel) = the pointer returns to the start point and Y is pressed to discard (held 0.15 seconds) */
/* 面板态：手指落在被禁用的目标上（底栏腰带格）照样当指针用——面板的按钮可能就在那下面 | Panel state: a finger landing on a disabled target (bottom bar belt slot) is still used as a pointer - the panel's button may be right under it */
static void pointer_over_disabled_target(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); s.mouse_click_mode=1; dm_tc_set_navigation_mode(&s,1); dm_tc_enable_targets(&s,dm_tc_nav_mask(1));
    const dm_tc_target *b=&s.layout.target[DM_TC_BELT1];
    dm_tc_begin_at(&s,0,b->x,b->y,10.0); CHECK(s.pointer_slot==0); dm_tc_end_at(&s,0,10.1); CHECK(s.mouse_click_seq==1); CHECK(s.buttons==0);
}
/* 游戏态点按 = 点世界：空处短按记一笔；按住拖动（摇杆）、点技能钮、面板态都不记。城里没目标时主按钮不发键 | Tap in game state = world tap: a short press on empty space records one; hold-and-drag (stick), tapping a skill button, and panel state do not record. The main button sends no key when there is no target in town */
static void world_tap(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_enable_targets(&s,(1u<<DM_TC_COUNT)-1);
    dm_tc_begin_at(&s,0,400,120,10.0); dm_tc_end_at(&s,0,10.1); CHECK(s.world_tap_seq==1); NEAR(s.world_tap_x,400,0.001); NEAR(s.world_tap_y,120,0.001);
    dm_tc_begin_at(&s,0,250,150,11.0); CHECK(s.joy_slot==-1); dm_tc_end_at(&s,0,11.1); CHECK(s.world_tap_seq==2); NEAR(s.world_tap_x,250,0.001);   /* 左侧、起摇杆区以外点一下：点世界（不起摇杆） | a tap on the left side outside the stick-start zone: a world tap (no stick) */
    dm_tc_begin_at(&s,0,120,250,11.5); CHECK(s.joy_slot==0); dm_tc_end_at(&s,0,11.6); CHECK(s.world_tap_seq==3); NEAR(s.world_tap_x,120,0.001); NEAR(s.world_tap_y,250,0.001);   /* 起摇杆区里很快原地松开：起过摇杆（位移为零），也兼作点世界 | a quick release in place inside the stick-start zone: the stick did start (zero displacement), and it also counts as a world tap */
    dm_tc_begin_at(&s,0,120,250,12.0); dm_tc_move_at(&s,0,170,250,12.1); dm_tc_end_at(&s,0,12.2); CHECK(s.world_tap_seq==3);   /* 拖了 = 摇杆 | Dragged = stick */
    dm_tc_begin_at(&s,0,400,120,13.0); dm_tc_end_at(&s,0,13.6); CHECK(s.world_tap_seq==3);                         /* 按了 0.6 秒：不算点按 | Held for 0.6 seconds: does not count as a tap */
    const dm_tc_target *m=&s.layout.target[DM_TC_MAIN];
    dm_tc_begin_at(&s,0,m->x,m->y,14.0); CHECK(s.buttons==m->button); dm_tc_end_at(&s,0,14.1); CHECK(s.world_tap_seq==3);   /* 点主按钮：发键，不算点世界 | Tapping the main button: sends the key, does not count as a world tap */
    dm_tc_set_joy_block(&s,1); dm_tc_begin_at(&s,0,120,250,20.5); CHECK(s.joy_slot==-1 && s.lx==0 && s.ly==0); dm_tc_end_at(&s,0,20.6); CHECK(s.world_tap_seq==4); NEAR(s.world_tap_x,120,0.001);   /* 讲话文字框单独开着：同一处点一下不起摇杆、没有方向，只当一次点击 | speech text box open on its own: a tap in the same place starts no stick and gives no direction, it is just one click */
    dm_tc_set_joy_block(&s,0); dm_tc_begin_at(&s,0,120,250,21.5); CHECK(s.joy_slot==0); dm_tc_end_at(&s,0,21.6); CHECK(s.world_tap_seq==5);   /* 文字框关了：起摇杆，短按照样兼作点世界 | text box closed: the stick starts, and a short press still doubles as a world tap */
    dm_tc_set_navigation_mode(&s,1); dm_tc_begin_at(&s,0,400,120,16.0); dm_tc_end_at(&s,0,16.1); CHECK(s.world_tap_seq==5);   /* 面板态不记 | Not recorded in panel state */
}
/* 面板外点按 = 按 B 到面板关掉为止：第一下在 tick 里发；面板还开着，0.3 秒后再发；面板关了就不再发；至多 3 下 | Tap outside the panel = press B until the panel closes: the first press is sent in tick; if the panel is still open, send again 0.3 seconds later; once the panel is closed no more are sent; at most 3 presses */
static void pointer_outside_close(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); s.mouse_click_mode=1; dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,330,60,10.0); dm_tc_pointer_item(&s,3); dm_tc_end_at(&s,0,10.1); CHECK(s.pulse_button==0);
    dm_tc_tick(&s,10.12); CHECK(s.pulse_button==DM_BTN_B); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_tick(&s,10.2); CHECK(s.pulse_button==0);                    /* 不到 0.3 秒 | Less than 0.3 seconds */
    dm_tc_tick(&s,10.45); CHECK(s.pulse_button==DM_BTN_B); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));   /* 面板还开着：再按 | Panel still open: press again */
    dm_tc_set_navigation_mode(&s,0); dm_tc_tick(&s,10.8); CHECK(s.pulse_button==0); CHECK(s.close_left==0);   /* 面板关了：停 | Panel closed: stop */    dm_tc_set_navigation_mode(&s,1);   /* 第一下返回引出了确认框：后面不再按 | The first Back brought up a confirmation box: no more presses after that */
    dm_tc_begin_at(&s,0,330,60,20.0); dm_tc_pointer_item(&s,3); dm_tc_end_at(&s,0,20.1); dm_tc_tick(&s,20.12); CHECK(s.pulse_button==DM_BTN_B); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    { dm_tc_scene sc={0}; dm_tc_navigation(&sc,"PanelManager","OpenPanel","ConfirmationModal"); }
    dm_tc_tick(&s,20.5); CHECK(s.pulse_button==0); CHECK(s.close_left==0);
}
static void scene_dialog_only(void) {   /* 只开着 NPC 对话菜单 = 底栏十字可点的那个状态 | Only the NPC dialog menu is open = the state in which the bottom bar D-pad is tappable */
    dm_tc_scene sc={0};
    dm_tc_navigation(&sc,"PanelManager","UnloadPanel","LoadScreenPanel"); CHECK(!dm_tc_scene_dialog_only(&sc));
    CHECK(dm_tc_navigation(&sc,"PanelManager","OpenPanel","NpcDialogPanel")); CHECK(dm_tc_scene_dialog_only(&sc));
    dm_tc_navigation(&sc,"PanelManager","OpenPanel","NpcTradePanel"); CHECK(!dm_tc_scene_dialog_only(&sc));
    dm_tc_navigation(&sc,"PanelManager","ClosePanel","NpcTradePanel"); dm_tc_navigation(&sc,"PanelManager","ClosePanel","NpcDialogPanel"); CHECK(!dm_tc_scene_dialog_only(&sc));
}
static void pointer_drag_out(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_move_at(&s,0,320,180,10.1); CHECK(s.pulse_button==DM_BTN_A); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_move_at(&s,0,20,300,10.5); dm_tc_pointer_release(&s,3); dm_tc_end_at(&s,0,10.6);   /* 3 = 手指在面板外 | 3 = the finger is outside the panel */
    CHECK(s.pulse_button==DM_BTN_Y); NEAR(s.pulse_hold_s,0.15,0.001); NEAR(s.pointer_x,300,0.001); NEAR(s.pointer_y,180,0.001);
    CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_begin_at(&s,0,300,180,11.0); dm_tc_move_at(&s,0,320,180,11.1); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_move_at(&s,0,340,200,11.5); dm_tc_end_at(&s,0,11.6); CHECK(s.pulse_button==DM_BTN_A);   /* 没置位 = 缺省按物品区：照旧放下 | Not set = defaults to item area: put down as before */
    CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_begin_at(&s,0,300,180,12.0); dm_tc_move_at(&s,0,320,180,12.1); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_move_at(&s,0,400,120,12.5); dm_tc_pointer_release(&s,4); dm_tc_end_at(&s,0,12.6);   /* 4 = 抬起点不在物品区、手指点在：放在手指点 | 4 = the lift point is not in the item area but the finger point is: put down at the finger point */
    CHECK(s.pulse_button==DM_BTN_A); NEAR(s.pointer_x,400,0.001); NEAR(s.pointer_y,120,0.001); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_begin_at(&s,0,300,180,13.0); dm_tc_move_at(&s,0,320,180,13.1); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_move_at(&s,0,400,120,13.5); dm_tc_pointer_release(&s,0); dm_tc_end_at(&s,0,13.6);   /* 0 = 面板里的空白处：放回原处，不丢 | 0 = blank area inside the panel: put back in place, not discarded */
    CHECK(s.pulse_button==DM_BTN_A); NEAR(s.pointer_x,300,0.001); NEAR(s.pointer_y,180,0.001);
}
static void pointer_drag(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_move_at(&s,0,320,180,10.1);
    CHECK(s.pointer_valid); NEAR(s.pointer_x,300,0.001); NEAR(s.pointer_y,180,0.001);
    CHECK(s.pulse_button==DM_BTN_A); uint64_t first=s.pulse_epoch;
    CHECK(dm_tc_pulse_end(&s,first));
    dm_tc_move_at(&s,0,340,200,10.2);
    NEAR(s.pointer_x,340,0.001); NEAR(s.pointer_y,160,0.001);
    dm_tc_end_at(&s,0,10.3); NEAR(s.pointer_x,340,0.001); NEAR(s.pointer_y,160,0.001);
    CHECK(s.pulse_button==DM_BTN_A); CHECK(s.pulse_epoch!=first);
}
static void pointer_long(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_tick(&s,10.5);
    CHECK(s.pointer_valid); NEAR(s.pointer_x,300,0.001); NEAR(s.pointer_y,180,0.001);
    CHECK(s.pulse_button==0); dm_tc_end_at(&s,0,10.6); CHECK(s.pulse_button==0);
}
static void pointer_shop_drag(void) {   /* 从商人货架的货上起拖：不发「拿起」、光标留在货上；松手不发任何键、只计一次（买不买由前端按落点定） | Starting a drag on goods on the vendor shelf: no "pick up" is sent and the cursor stays on the goods; on release no key is sent, only one count is made (whether to buy is decided by the front end from the drop point) */
    dm_tc_state s; dm_tc_init(&s,667,375); s.mouse_click_mode=1; dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,150,200,10.0); dm_tc_pointer_item(&s,7); CHECK(s.pointer_item==1); CHECK(s.pointer_shop==1);
    dm_tc_move_at(&s,0,300,220,10.3); CHECK(s.pulse_button==0); CHECK(s.finger[0].shop_drag); NEAR(s.pointer_x,2,0.001); NEAR(s.pointer_y,2,0.001);   /* 光标挪到角上（收说明框），不跟手指 | The cursor moves to the corner (dismissing the tooltip) and does not follow the finger */
    dm_tc_move_at(&s,0,480,280,10.8); NEAR(s.pointer_x,2,0.001); NEAR(s.finger[0].aim_origin_x,150,0.001);
    dm_tc_end_at(&s,0,11.0); CHECK(s.pulse_button==0); CHECK(s.shop_drop_seq==1); CHECK(s.pointer_shop==0);
    dm_tc_begin_at(&s,0,150,200,12.0); dm_tc_pointer_item(&s,7); dm_tc_end_at(&s,0,12.1); CHECK(s.shop_drop_seq==1);   /* 点货不买 | Tapping goods does not buy */
}
static void pointer_long_seq(void) {   /* 物品区按住不动满长按时长、松手时计一次（前端据此调宿主「补充到腰带」）；按住后又拖走的不计；别处的长按不计 | In the item area, holding still for the full long-press duration counts once on release (the front end calls the host's "refill to belt" based on this); holding and then dragging away does not count; long presses elsewhere do not count */
    dm_tc_state s; dm_tc_init(&s,667,375); s.mouse_click_mode=1; dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_pointer_item(&s,1); dm_tc_tick(&s,10.6); CHECK(s.long_press_seq==0);
    dm_tc_end_at(&s,0,11.6); CHECK(s.long_press_seq==1); CHECK(s.pulse_button==0);
    dm_tc_begin_at(&s,0,300,180,12.0); dm_tc_pointer_item(&s,1); dm_tc_tick(&s,12.6); dm_tc_move_at(&s,0,360,220,12.8); dm_tc_end_at(&s,0,13.2); CHECK(s.long_press_seq==1);
    dm_tc_pulse_end(&s,s.pulse_epoch);
    dm_tc_begin_at(&s,0,300,180,14.0); dm_tc_pointer_item(&s,0); dm_tc_tick(&s,15.0); dm_tc_end_at(&s,0,15.1); CHECK(s.long_press_seq==1);
}
static void pointer_long_then_drag(void) {   /* 按住停 0.5 秒再拖：照样拿起、松手放下 | Hold for 0.5 seconds and then drag: still picks up, and puts down on release */
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_tick(&s,10.5); CHECK(s.pulse_button==0);
    dm_tc_move_at(&s,0,340,200,10.6); CHECK(s.pulse_button==DM_BTN_A); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch));
    dm_tc_move_at(&s,0,400,240,11.0); dm_tc_end_at(&s,0,11.1); CHECK(s.pulse_button==DM_BTN_A);
}
static void pointer_control_hit(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1);
    press(&s,0,DM_TC_MAIN); CHECK(!s.pointer_valid); CHECK(s.buttons==DM_BTN_A);
}
static void pointer_game_mode(void) {
    dm_tc_state s; dm_tc_init(&s,667,375);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_move_at(&s,0,340,200,10.1);
    CHECK(!s.pointer_valid); CHECK(s.joy_slot==-1); dm_tc_end_at(&s,0,10.2); CHECK(!s.pointer_valid);
}
static void pointer_pulse_queue(void) {
    dm_tc_state s; dm_tc_init(&s,667,375); dm_tc_set_navigation_mode(&s,1);
    dm_tc_begin_at(&s,0,300,180,10.0); dm_tc_move_at(&s,0,320,180,10.1);
    // 「拿起」还没收手指就抬了：指针留在起点（游戏还没取走这下 A），「放下」只排着
    // The finger lifted before "pick up" was withdrawn: the pointer stays at the start point (the game has not yet taken this A), and "put down" is only queued
    dm_tc_end_at(&s,0,10.2); CHECK(s.pulse_button==DM_BTN_A); NEAR(s.pointer_x,300,0.001); NEAR(s.pointer_y,180,0.001); CHECK(!dm_tc_pulse_pending(&s));
    uint64_t first=s.pulse_epoch; CHECK(dm_tc_pulse_end(&s,first));
    // 收回后 A 松开、指针仍在起点，等前端 resume（中间留一次松开给游戏看）
    // After the withdrawal A is released and the pointer is still at the start point, waiting for the front end's resume (one release is left in between for the game to see)
    CHECK(s.pulse_button==0); CHECK(s.buttons==0); CHECK(dm_tc_pulse_pending(&s)); NEAR(s.pointer_y,180,0.001);
    CHECK(dm_tc_pulse_resume(&s)); CHECK(s.pulse_button==DM_BTN_A); CHECK(s.pulse_epoch!=first); NEAR(s.pointer_x,320,0.001); NEAR(s.pointer_y,140,0.001);
    CHECK(!dm_tc_pulse_resume(&s)); CHECK(dm_tc_pulse_end(&s,s.pulse_epoch)); CHECK(!dm_tc_pulse_pending(&s));
}

/* 整个技能扇区让开左右安全区，换组键不挤入底栏。 | Keep the whole skill fan clear of the side safe areas and the set switch out of the bottom bar. */
static void safe_insets(void) {
    dm_tc_layout a,b;dm_tc_layout_make(&a,956,440);
    dm_tc_set_safe_insets(62,62);dm_tc_layout_make(&b,956,440);dm_tc_set_safe_insets(0,0);
    for(int i=DM_TC_MAIN;i<=DM_TC_PORTAL;i++) {
        NEAR(a.target[i].x-b.target[i].x,62,.001);NEAR(a.target[i].y,b.target[i].y,.001);
        CHECK(b.target[i].x+b.target[i].r<=956-62-8+.001f);
    }
    NEAR(a.swap.x-b.swap.x,62,.001);NEAR(a.swap.y,b.swap.y,.001);
    NEAR(a.joy_x,120,.001);NEAR(b.joy_x,152,.001);NEAR(a.joy_y,325,.001);
    dm_tc_layout c;dm_tc_layout_make(&c,956,440);NEAR(c.target[DM_TC_MAIN].x,a.target[DM_TC_MAIN].x,.001);
}
static void swap_key(void) {
    dm_tc_layout l;dm_tc_layout_make(&l,956,440);
    NEAR(l.swap.r,22,.001);CHECK(l.swap.x<l.target[DM_TC_PORTAL].x);
    float travel=hypotf(l.swap.x-l.target[DM_TC_MAIN].x,l.swap.y-l.target[DM_TC_MAIN].y);
    NEAR(travel,hypotf(l.target[DM_TC_PORTAL].x-l.target[DM_TC_MAIN].x,l.target[DM_TC_PORTAL].y-l.target[DM_TC_MAIN].y),.001);
    CHECK(dm_tc_hit(&l,l.swap.x,l.swap.y)==DM_TC_SWAP);
    // 两键间的空隙不属于任何按钮；从攻击键滑入换组键也不会切组。
    // The gap belongs to neither button; dragging an attack touch onto the switch does not switch sets.
    dm_tc_target main=l.target[DM_TC_MAIN];
    float d=main.r+12;
    CHECK(dm_tc_hit(&l,main.x+(l.swap.x-main.x)*d/travel,main.y+(l.swap.y-main.y)*d/travel)==DM_TC_NONE);
    dm_tc_state s;dm_tc_init(&s,956,440);press(&s,0,DM_TC_MAIN);
    dm_tc_move(&s,0,l.swap.x,l.swap.y);CHECK(s.skill_set==0);CHECK(s.finger[0].target==DM_TC_MAIN);
    dm_tc_end(&s,0);CHECK(s.skill_set==0);
    dm_tc_begin(&s,0,l.swap.x,l.swap.y);CHECK(s.skill_set==0);dm_tc_end(&s,0);CHECK(s.skill_set==1);
    dm_tc_begin(&s,0,l.swap.x,l.swap.y);dm_tc_move(&s,0,l.swap.x+l.swap.r+1,l.swap.y);
    dm_tc_move(&s,0,l.swap.x,l.swap.y);dm_tc_end(&s,0);CHECK(s.skill_set==1);
    dm_tc_begin(&s,0,l.swap.x,l.swap.y);dm_tc_cancel_finger(&s,0);CHECK(s.skill_set==1);
    dm_tc_begin(&s,0,l.swap.x,l.swap.y);dm_tc_resize(&s,812,375);CHECK(s.skill_set==1);
    dm_tc_end(&s,0);CHECK(s.skill_set==1);
}
/* 实际设备尺寸下检查整个技能区的触控间隔与边界，不依赖绘图预览。
   Check touch gaps and bounds for the complete skill cluster at device sizes, independently of rendered previews.
   Check touch gaps and bounds for the complete skill cluster at device sizes, independently of rendered previews. */
static void skill_cluster_clearance(void) {
    const float sizes[][6]={{333.5f,187.5f,0,0,0,0},{667,375,0,0,0,0},{812,375,44,44,21,0},
        {874,402,62,62,21,0},{956,440,62,62,34,0},{1024,768,0,0,20,1},{1194,834,0,0,24,1},
        {1366,1024,0,0,24,1},{1366,768,32,32,24,1},{744,450,0,0,20,1},{507,285,0,0,20,1},{600,337,0,0,24,1},{874,402,62,0,21,0}};
    for(int above=0;above<2;above++) for(int n=0;n<(int)(sizeof(sizes)/sizeof(sizes[0]));n++) {
        float w=sizes[n][0],h=sizes[n][1],bottom=sizes[n][4];dm_tc_set_tall_scale((int)sizes[n][5]);
        dm_tc_set_swap_above(above);dm_tc_set_safe_bottom(bottom);
        dm_tc_set_safe_insets(sizes[n][2],sizes[n][3]);dm_tc_layout l;dm_tc_layout_make(&l,w,h);
        float k=dm_tc_scale_for_size(w,h);dm_tc_target group[7];
        CHECK(k<=1.15f);CHECK(l.tablet==(int)sizes[n][5]);CHECK(l.swap_above==above);
        if (!above) {
            CHECK(l.swap.x<l.target[DM_TC_PORTAL].x);
            dm_tc_target main=l.target[0],portal=l.target[DM_TC_PORTAL];
            NEAR(hypotf(l.swap.x-main.x,l.swap.y-main.y),hypotf(portal.x-main.x,portal.y-main.y),.001);
        }
        for(int i=0;i<6;i++)group[i]=l.target[i];group[6]=l.swap;
        for(int i=0;i<7;i++) {
            dm_tc_target t=group[i];CHECK(t.x-t.r>=sizes[n][2]);CHECK(t.x+t.r<=w-sizes[n][3]);
            CHECK(t.y-t.r>=0);CHECK(t.y+t.r<=h-fmaxf(24*k,bottom+8*k)+.001f);
            if(sizes[n][5] || (w>=667 && h>=375)) CHECK(t.r*2>=44);
            for(int j=i+1;j<7;j++)CHECK(hypotf(t.x-group[j].x,t.y-group[j].y)>=t.r+group[j].r+12*k-.001f);
            for(int j=DM_TC_BELT1;j<=DM_TC_MENU;j++) {
                dm_tc_target bar=l.target[j];CHECK(hypotf(t.x-bar.x,t.y-bar.y)>t.r+bar.r+8*k);
            }
            dm_tc_target map=l.target[DM_TC_MAP];
            CHECK(hypotf(t.x-map.x,t.y-map.y)>=t.r+map.r+12*k-.001f);
            CHECK(dm_tc_hit(&l,t.x,t.y)==(i==6?DM_TC_SWAP:i));
        }
    }
    dm_tc_set_tall_scale(1);dm_tc_set_safe_insets(0,0);dm_tc_set_safe_bottom(24);dm_tc_set_swap_above(0);
    dm_tc_state s;dm_tc_init(&s,1194,672);dm_tc_layout before=s.layout;
    press(&s,0,DM_TC_MAIN);CHECK(s.buttons!=0);dm_tc_set_swap_above(1);dm_tc_resize(&s,1194,834);
    CHECK(s.buttons==0 && s.joy_slot==DM_TC_NONE);CHECK(s.layout.swap_above==1);
    // 平板宽度相同时，增加高度不应扩大按钮或增加到屏幕边缘的距离。
    // Same tablet width: increasing height must not increase button sizes or edge reach distances.
    NEAR(s.layout.target[0].r,before.target[0].r,.001);
    NEAR(s.layout.height-s.layout.target[0].y,before.height-before.target[0].y,.001);
    for(int i=0;i<5;i++)NEAR(s.layout.target[i].x,before.target[i].x,.001);
    dm_tc_set_tall_scale(0);dm_tc_set_safe_insets(0,0);dm_tc_set_safe_bottom(0);dm_tc_set_swap_above(0);
}
/* 腰带展开手势（956×440，u = 440/1320 = 1/3：腰带第 1 列格心 x = 478 + (1210−1434)/3 = 403.33、y = 1241.5/3 = 413.83）。
   （腰带一律松手后等过双击窗才喝。）只有 1 排：按下不发键、松手过窗 → 一个脉冲。4 排：按下不发键；松手没滑 → 过窗一个脉冲；上滑过 24 点 → 请求 1（展开）且不发键；下滑 → 请求 2。
   展开着：点上一排（y 高 165/3 = 55）同列 → 请求 0x100 | 1<<4 | 0，不发键；点世界别处 → 请求 2（收起）。
   Belt expand gesture (956×440, u = 440/1320 = 1/3: belt column 1 cell center x = 478 + (1210−1434)/3 = 403.33, y = 1241.5/3 = 413.83).
      (The belt always drinks only after release and once the double-tap window has passed.) Only 1 row: no key on press, release and window passed → one pulse. 4 rows: no key on press; release without sliding → one pulse after the window; slide up past 24 points → request 1 (expand) and no key; slide down → request 2.
      While expanded: tap the same column one row up (y higher by 165/3 = 55) → request 0x100 | 1<<4 | 0, no key; tap elsewhere in the world → request 2 (collapse). */
static void belt_expand(void) {
    dm_tc_state s; dm_tc_init(&s, 956, 440); float bx = s.layout.target[DM_TC_BELT1].x, by = s.layout.target[DM_TC_BELT1].y;
    NEAR(bx, 403.333f, 0.01); NEAR(by, 413.833f, 0.01);
    dm_tc_begin_at(&s, 0, bx, by, 1.0); CHECK(s.buttons == 0); dm_tc_end_at(&s, 0, 1.05); CHECK(!s.belt_request); dm_tc_tick(&s, 1.05 + 0.01); CHECK(s.pulse_button == DM_BTN_LEFT); dm_tc_pulse_end(&s, s.pulse_epoch);
    s.belt_rows = 4; s.pulse_button = 0;
    dm_tc_begin_at(&s, 0, bx, by, 2.0); CHECK(s.buttons == 0); dm_tc_end_at(&s, 0, 2.05); dm_tc_tick(&s, 2.05 + 0.01); CHECK(s.pulse_button == DM_BTN_LEFT); CHECK(!s.belt_request); s.pulse_button = 0;
    dm_tc_begin_at(&s, 0, bx, by, 3.0); dm_tc_tick(&s, 3.4); CHECK(s.buttons == 0); dm_tc_end_at(&s, 0, 3.5); dm_tc_tick(&s, 3.5 + 0.01); CHECK(s.pulse_button == DM_BTN_LEFT); s.pulse_button = 0;   /* 按住半秒不动：按住期间不发键，松手才喝 | Hold for half a second without moving: no key is sent while holding, drinks only on release */
    dm_tc_begin_at(&s, 0, bx, by, 4.0); dm_tc_move_at(&s, 0, bx, by - 30, 4.05); CHECK(s.belt_request == 0); CHECK(s.buttons == 0); dm_tc_end_at(&s, 0, 4.1); CHECK(s.belt_request == 1); CHECK(s.pulse_button == 0); s.belt_request = 0;
    dm_tc_begin_at(&s, 0, bx, by, 5.0); dm_tc_move_at(&s, 0, bx, by + 25, 5.05); dm_tc_end_at(&s, 0, 5.1); CHECK(s.belt_request == 2); CHECK(s.pulse_button == 0); s.belt_request = 0;
    dm_tc_begin_at(&s, 0, bx, by, 5.2); dm_tc_tick(&s, 5.4); dm_tc_move_at(&s, 0, bx, by - 30, 5.5); dm_tc_end_at(&s, 0, 5.55); CHECK(s.belt_request == 1); CHECK(s.pulse_button == 0); CHECK(s.buttons == 0); s.belt_request = 0;   /* 按下停 0.2 秒再上滑：展开，不喝（实测撞到过） | Press, stay 0.2 seconds, then slide up: expands, does not drink (hit in actual testing) */
    dm_tc_begin_at(&s, 0, bx, by, 5.6); dm_tc_tick(&s, 5.8); dm_tc_move_at(&s, 0, bx, by + 36, 5.95); dm_tc_end_at(&s, 0, 5.95); CHECK(s.belt_request == 2); CHECK(s.pulse_button == 0); s.belt_request = 0;   /* 0.35 秒慢慢下滑 36 点：收起，不喝（真机撞到过） | Slowly sliding down 36 points over 0.35 seconds: collapses, does not drink (hit on a real device) */
    dm_tc_begin_at(&s, 0, bx, by, 5.96); dm_tc_move_at(&s, 0, bx + 40, by, 5.97); dm_tc_end_at(&s, 0, 5.98); CHECK(!s.belt_request); CHECK(s.pulse_button == 0);   /* 横着滑走：什么都不做 | Sliding away sideways: does nothing */
    CHECK(s.layout.target[DM_TC_BELT3].button == DM_BTN_DOWN && s.layout.target[DM_TC_BELT4].button == DM_BTN_RIGHT);   /* 第 3 列 = 下、第 4 列 = 右（原版实测） | Column 3 = down, column 4 = right (measured in the original game) */
    s.belt_open = 1;
    dm_tc_begin_at(&s, 0, bx, by - 55.0f, 6.0); CHECK(s.buttons == 0); dm_tc_end_at(&s, 0, 6.05); dm_tc_tick(&s, 6.05 + 0.01); CHECK(s.belt_request == (0x100 | 1 << 4)); CHECK(s.pulse_button == 0); CHECK(s.world_tap_seq == 0); s.belt_request = 0;
    dm_tc_begin_at(&s, 0, bx + 40*3, by - (165.0f + 2*162.0f)/3, 7.0); dm_tc_end_at(&s, 0, 7.05); dm_tc_tick(&s, 7.05 + 0.01); CHECK(s.belt_request == (0x100 | 3 << 4 | 3)); s.belt_request = 0;
    dm_tc_begin_at(&s, 0, bx, by, 8.0); CHECK(!s.belt_request); dm_tc_end_at(&s, 0, 8.05); dm_tc_tick(&s, 8.05 + 0.01); CHECK(s.pulse_button == DM_BTN_LEFT); s.pulse_button = 0;
    CHECK(dm_tc_belt_outside(&s, 600, 150)); CHECK(!dm_tc_belt_outside(&s, bx, by)); CHECK(!dm_tc_belt_outside(&s, bx, by - 165.0f/3)); s.belt_open = 0; CHECK(!dm_tc_belt_outside(&s, 600, 150)); s.belt_open = 1;
    dm_tc_begin_at(&s, 0, 100, 250, 9.0); CHECK(s.belt_request == 2); CHECK(s.joy_slot == DM_TC_NONE); dm_tc_end_at(&s, 0, 9.05); CHECK(s.world_tap_seq == 0); CHECK(s.buttons == 0);   /* 点别处（摇杆区）：只收起，不成摇杆、不点世界 | Tap elsewhere (stick zone): only collapses, does not become the stick, does not tap the world */
}
/* 第二组里点得太短的技能等 ZL 领先再补发。等的时候点了多排腰带：腰带那下不许带 ZL（ZL + 腰带 = 喂佣兵）、
   不许被补发盖掉；腰带脉冲收回后 ZL 再领先满 DM_TC_ZL_LEAD_S 才补技能；全取消把等着的补发一起作废
   A skill tapped too briefly in the second set waits for ZL to lead before the make-up send. A multi-row belt is tapped while waiting: the belt tap must not carry ZL (ZL + belt = feed the mercenary) and
      must not be overwritten by the make-up send; after the belt pulse is withdrawn, ZL leads for a full DM_TC_ZL_LEAD_S again before the skill is made up; cancel-all voids the pending make-up send as well */
static void deferred_skill_vs_belt(void) {
    /* 技能在等补发时点腰带：腰带那下排着，技能先按时补发（ZL 领先），等它收回后腰带再发、不带 ZL | tapping the belt while a skill awaits resend: the belt tap queues, the skill resends on time first (ZL leading), and after it is released the belt key is sent, without ZL */
    dm_tc_state s; dm_tc_init(&s, 667, 375); s.skill_set = 1; s.belt_rows = 4; s.last_time_s = 10.0;
    dm_tc_target k = s.layout.target[DM_TC_SKILL1], b = s.layout.target[DM_TC_BELT1];
    dm_tc_begin_at(&s, 0, k.x, k.y, 10.000); dm_tc_end_at(&s, 0, 10.050); CHECK(s.defer_button == k.button);
    dm_tc_begin_at(&s, 1, b.x, b.y, 10.080); dm_tc_end_at(&s, 1, 10.120);
    double skill = -1, belt = -1; int belt_zl = 0, zl_gap = 0;   /* ZL 从技能松手（10.050）起就按着，补发前一直不断 | ZL is held from the skill release (10.050) onward, with no break until the make-up send */
    for (double t = 10.13; t < 11.0; t += 0.01) { dm_tc_tick(&s, t);
        if (skill < 0 && !(s.buttons & DM_BTN_ZL)) zl_gap = 1;
        if (s.pulse_button == k.button && skill < 0) { skill = t; dm_tc_tick(&s, t + 0.03); s.last_time_s = t + 0.03; CHECK(dm_tc_pulse_end(&s, s.pulse_epoch)); }
        else if (s.pulse_button == b.button && belt < 0) { belt = t; belt_zl = (s.buttons & DM_BTN_ZL) != 0; CHECK(dm_tc_pulse_end(&s, s.pulse_epoch)); } }
    CHECK(skill >= 10.000 + DM_TC_ZL_LEAD_S - 1e-9 && !zl_gap);   /* 技能补发：ZL 领先够、中间不断 | Skill make-up send: ZL leads long enough, with no break in between */
    CHECK(belt > skill && belt >= 10.120 && !belt_zl);           /* 腰带那下没丢、没被盖、不带 ZL（不变成喂佣兵） | The belt tap was not lost, not overwritten, and carries no ZL (does not turn into feeding the mercenary) */
    dm_tc_state c; dm_tc_init(&c, 667, 375); c.skill_set = 1; c.last_time_s = 20.0;
    dm_tc_begin_at(&c, 0, k.x, k.y, 20.000); dm_tc_end_at(&c, 0, 20.050); CHECK(c.defer_button);
    dm_tc_cancel_all(&c); dm_tc_tick(&c, 20.30); CHECK(c.defer_button == 0 && c.pulse_button == 0 && c.buttons == 0);
}
/* 手指一直按着第二组技能时点腰带：喝药那一脉冲期间不许有 ZL（ZL + 腰带键 = 喂佣兵）、也不许剩下不带 ZL 的技能键（= 放成第一组技能）；
   脉冲收回后 ZL 先回来，技能键晚 DM_TC_ZL_LEAD_S 再回来。期望按规则手算
   Tapping the belt while a finger keeps holding a second-set skill: during the potion pulse there must be no ZL (ZL + belt key = feed the mercenary), nor a leftover skill key without ZL (= cast as the first-set skill);
   after the pulse is released ZL comes back first, and the skill key comes back DM_TC_ZL_LEAD_S later. Expectations computed by hand from the rule */
static void held_skill2_vs_belt(void) {
    dm_tc_state s; dm_tc_init(&s, 667, 375); s.skill_set = 1; s.last_time_s = 30.0;
    dm_tc_target k = s.layout.target[DM_TC_SKILL1], b = s.layout.target[DM_TC_BELT1];
    dm_tc_begin_at(&s, 0, k.x, k.y, 30.000); for (double t = 30.01; t < 30.60; t += 0.01) dm_tc_tick(&s, t);
    CHECK((s.buttons & DM_BTN_ZL) && (s.buttons & k.button));   /* 按住半秒多：ZL + 技能键都在 | Held for a bit over half a second: ZL + the skill key are both present */
    dm_tc_begin_at(&s, 1, b.x, b.y, 30.600); dm_tc_end_at(&s, 1, 30.640);
    double belt = -1, back = -1, zl_back = -1, zl_off = -1; int bad_zl = 0, bad_key = 0;
    for (double t = 30.65; t < 31.60; t += 0.01) { dm_tc_tick(&s, t);
        if (belt < 0 && zl_off < 0 && !(s.buttons & DM_BTN_ZL)) { zl_off = t; CHECK(!(s.buttons & k.button)); }   /* ZL 先松（技能键同时松），这时腰带键还没按 | ZL is released first (the skill key is released at the same time), and at this point the belt key is not pressed yet */
        if (s.pulse_button == b.button) { if (belt < 0) belt = t; if (s.buttons & DM_BTN_ZL) bad_zl = 1; if (s.buttons & k.button) bad_key = 1;
            if (t - belt > 0.025) { CHECK(dm_tc_pulse_end(&s, s.pulse_epoch)); dm_tc_tick(&s, t); } }
        else if (belt >= 0) { if (zl_back < 0 && (s.buttons & DM_BTN_ZL)) zl_back = t; if (back < 0 && (s.buttons & k.button)) { back = t; CHECK(s.buttons & DM_BTN_ZL); } } }
    CHECK(belt >= 30.640 && !bad_zl && !bad_key);   /* 喝药脉冲发了、期间没有 ZL、没有技能键 | The potion pulse was sent, with no ZL and no skill key during it */
    CHECK(zl_off > 0 && belt - zl_off >= DM_TC_ZL_LEAD_S - 0.011);   /* ZL 松开后空够一个领先时间腰带键才按（同一拍松 ZL + 按腰带键，游戏仍当 ZL 在） | the belt key is pressed only after a full lead time has gone by since ZL was released (releasing ZL + pressing the belt key in the same tick, the game still takes ZL as held) */
    CHECK(zl_back > belt && back - zl_back >= DM_TC_ZL_LEAD_S - 0.011);    /* 收回后 ZL 先回、技能键晚一个领先时间回 | After the release ZL comes back first, and the skill key comes back one lead time later */
    dm_tc_end_at(&s, 0, 31.60);
}
/* 起手落在模态框外 / 被上层盖住（宿主判类 6）时，上下滑不起界面滚动手势；面板里其它地方（类 0）照旧起 | When the touch starts outside a modal box / covered by an upper layer (host hit class 6), sliding up or down does not start the UI scroll gesture; elsewhere in the panel (class 0) it starts as before */
static void no_ui_drag_outside_modal(void) {
    for (int kind = 0; kind <= 6; kind += 6) {
        dm_tc_state s; dm_tc_init(&s, 667, 375); dm_tc_set_navigation_mode(&s, 1); s.mouse_click_mode = 1; s.last_time_s = 1.0;
        dm_tc_begin_at(&s, 0, 300, 100, 1.0); dm_tc_pointer_item(&s, kind);
        dm_tc_move_at(&s, 0, 300, 140, 1.05); dm_tc_move_at(&s, 0, 300, 200, 1.10);
        CHECK(s.finger[0].ui_drag == (kind == 6 ? 0 : 1));
    }
}
/* 腰带点一下 = 自己喝，松手那一刻就出腰带键（不带 ZL，第二组技能下也不带）；不判双击：连点两下 = 两个腰带键脉冲，哪一下都不带 ZL（不喂雇佣兵）。
   展开后的上面几排按下就出 belt_request 0x1rc，点两下两次。期望按规则手算
   One tap on the belt = the player drinks; the belt key fires at the moment of release (without ZL, also under the second skill set); no double-tap detection: two quick taps = two belt key pulses, neither with ZL (no mercenary feeding).
      Upper rows when expanded send belt_request 0x1rc on press, two taps send two. Expected values are computed by hand from the rules */
static void tap_belt(dm_tc_state *s, int slot, int target, double t0) {
    dm_tc_target t = s->layout.target[target]; dm_tc_begin_at(s, slot, t.x, t.y, t0); dm_tc_end_at(s, slot, t0 + 0.08);
}
static void belt_single_double(void) {
    for (int set = 0; set < 2; ++set) {
        dm_tc_state s; dm_tc_init(&s, 956, 440); s.skill_set = set; uint64_t k = s.layout.target[DM_TC_BELT2].button;
        /* 点一下：按着不出键，松手即出 | single tap: no key while held, key on release */
        double t = 10; dm_tc_tick(&s, t); dm_tc_begin_at(&s, 0, s.layout.target[DM_TC_BELT2].x, s.layout.target[DM_TC_BELT2].y, t); CHECK(s.buttons == 0);
        dm_tc_end_at(&s, 0, t + 0.08); CHECK(s.buttons == k);   /* 不带 ZL | Without ZL */
        CHECK(dm_tc_pulse_end(&s, s.pulse_epoch)); CHECK(s.buttons == 0);
        /* 0.12 秒内连点两下，第一下的脉冲已收回：两个脉冲，都不带 ZL | two taps within 0.12 s, the first pulse already released: two pulses, neither with ZL */
        t = 20; dm_tc_tick(&s, t); tap_belt(&s, 0, DM_TC_BELT2, t); CHECK(s.buttons == k); unsigned e1 = s.pulse_epoch; CHECK(dm_tc_pulse_end(&s, e1));
        tap_belt(&s, 1, DM_TC_BELT2, t + 0.12); CHECK(s.buttons == k && s.pulse_epoch != e1); CHECK(dm_tc_pulse_end(&s, s.pulse_epoch)); CHECK(s.buttons == 0);
        /* 连点两下，第一下的脉冲还在发：第二下排着，收回后的下一拍补发，仍不带 ZL，一共两个 | two quick taps while the first pulse is still being sent: the second queues and is sent on the tick after release, still without ZL, two in total */
        t = 30; dm_tc_tick(&s, t); tap_belt(&s, 0, DM_TC_BELT2, t); e1 = s.pulse_epoch; tap_belt(&s, 1, DM_TC_BELT2, t + 0.10); CHECK(s.pulse_epoch == e1 && s.buttons == k);
        CHECK(dm_tc_pulse_end(&s, e1)); CHECK(s.buttons == 0); dm_tc_tick(&s, t + 0.20); CHECK(s.buttons == k && s.pulse_epoch != e1); CHECK(dm_tc_pulse_end(&s, s.pulse_epoch));
        dm_tc_tick(&s, t + 1.0); CHECK(s.buttons == 0 && !s.pulse_button);   /* 不多喝 | no extra drink */
        /* 换一格 | switch to another slot */
        t = 40; dm_tc_tick(&s, t); tap_belt(&s, 0, DM_TC_BELT3, t); CHECK(s.buttons == s.layout.target[DM_TC_BELT3].button); dm_tc_pulse_end(&s, s.pulse_epoch);
        /* 展开的上面几排（第 1 排第 2 列）：按下就请求，点两下请求两次 | upper rows when expanded (row 1, column 2): request on press, two taps make two requests */
        s.belt_rows = 3; s.belt_open = 1; float u = s.layout.height / 1320.0f, x = s.layout.target[DM_TC_BELT2].x, y = s.layout.target[DM_TC_BELT2].y - 165 * u;
        t = 60; dm_tc_tick(&s, t); dm_tc_begin_at(&s, 0, x, y, t); CHECK(s.belt_request == (0x100 | 1 << 4 | 1)); CHECK(s.buttons == 0); dm_tc_end_at(&s, 0, t + 0.08); s.belt_request = 0;
        dm_tc_begin_at(&s, 0, x, y, t + 0.2); CHECK(s.belt_request == (0x100 | 1 << 4 | 1)); CHECK(s.buttons == 0); dm_tc_end_at(&s, 0, t + 0.3); s.belt_request = 0;
        dm_tc_tick(&s, t + 1); CHECK(s.belt_request == 0);
    }
}
int main(void) {
    RUN(layout_667); RUN(layout_812); RUN(layout_956); RUN(layout_1024); RUN(layout_ipad169); RUN(layout_pixels); RUN(small_layout); RUN(difficulty_picker);
    RUN(hit_edges); RUN(key_hold_and_ownership); RUN(two_fingers_same_key); RUN(joystick_deadzone); RUN(joystick_clamp_956);
    RUN(joystick_axes); RUN(joystick_plus_skills); RUN(no_auto_handoff); RUN(background_no_key);
    RUN(circles_before_joystick); RUN(cancel_and_resize); RUN(deferred_skill_vs_belt); RUN(held_skill2_vs_belt); RUN(belt_single_double); RUN(no_ui_drag_outside_modal); RUN(invalid_slots_and_repeated_begin); RUN(all_slots_hold); RUN(enabled_targets); RUN(ui_scene_messages);
    RUN(pointer_tap); RUN(world_tap); RUN(pointer_outside_close); RUN(scene_dialog_only); RUN(pointer_item_tap); RUN(pointer_stack_tap); RUN(pointer_over_disabled_target); RUN(ui_drag_typing); RUN(pointer_drag_out); RUN(pointer_drag); RUN(pointer_long); RUN(pointer_long_then_drag); RUN(pointer_long_seq); RUN(pointer_shop_drag); RUN(pointer_control_hit); RUN(pointer_game_mode); RUN(pointer_pulse_queue); RUN(safe_insets); RUN(swap_key); RUN(skill_cluster_clearance);
    RUN(belt_expand);
    printf("PASS %d/%d cases; %d/%d checks\n",cases,cases,checks,checks); return 0;
}
