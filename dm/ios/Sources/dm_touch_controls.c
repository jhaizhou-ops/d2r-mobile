#include "dm_touch_controls.h"
#include "../../host/dm_input.h"
#include <math.h>
#include <string.h>

#include <stdatomic.h>
#define DM_TC_POINTER_MOVE_PT 10.0f
#define DM_TC_POINTER_HOLD_S 0.5
#define DM_TC_POINTER_TAP_S 0.35

/* UIKit 使用点数：手机保持常规大小，平板最多放大 15%。
   锚点间距使用相同的有界比例，不随屏幕高度增长。
   Mac 的像素坐标回放默认不开平板缩放。
   UIKit uses points: phone controls keep their normal size, tablet controls grow at most 15%.
      Anchor distances use the same bounded scale rather than growing with screen height.
      Tablet scaling stays off by default for the Mac pixel-coordinate replay path. */
static int g_tall_scale, g_swap_above;
void dm_tc_set_tall_scale(int on) { g_tall_scale = !!on; }
void dm_tc_set_swap_above(int above) { g_swap_above = !!above; }
float dm_tc_scale_for_size(float width, float height) {
    float fit=fminf(width/667.0f,height/375.0f);
    return g_tall_scale ? fminf(1.15f,fmaxf(1.0f,fit)) : fminf(1.0f,fit);
}
static float scale_for(const dm_tc_layout *l) { return dm_tc_scale_for_size(l->width, l->height); }

/* 安全区（iPhone 横屏左右各有一条让给灵动岛 / 圆角的区域，17 Pro Max 实读各 62 点）：右侧技能键整组、左侧摇杆按最小位移平移到全部落在安全区内，
   组内相对排布不变。岛在哪一侧取决于玩家怎么拿，两侧都内缩。缺省 0（Mac 宿主、单测不设 = 原布局）。
   Safe area (in iPhone landscape there is a strip on each of the left and right yielded to the Dynamic Island / rounded corners; read as 62 points each on a 17 Pro Max): the whole right-side skill key group and the left stick are translated by the minimum displacement so that they lie entirely inside the safe area,
      with the relative arrangement within the group unchanged. Which side the island is on depends on how the player holds the device, so both sides are inset. Default 0 (Mac host and unit tests do not set it = original layout). */
static float g_safe_l, g_safe_r, g_safe_b;
void dm_tc_set_safe_bottom(float bottom) { g_safe_b=isfinite(bottom) && bottom>0 ? bottom : 0; }
/* 屏幕底边归系统的那一条（点）：从这里起手往上划会被系统当成底边手势（iPad 上连着两次就拉出程序坞），而移动恰好是同一处反复起手往上推。
   这一条里起手不当摇杆、松手也不当点地；从上面起手的摇杆拖进来照常持有。只有 iPad 的前端会给非零值，手机恒为 0。
   The strip along the bottom edge of the screen that belongs to the system (points): a swipe up starting here is taken by the system as a bottom-edge gesture (on iPad two in a row pull out the Dock), and movement is exactly a repeated push upward starting from the same place.
      A touch starting in this strip is not treated as the stick, and its release is not treated as a ground tap; a stick drag that started above and moves in keeps its hold as usual. Only the iPad front end passes a non-zero value; on phones it is always 0. */
static float g_edge_b;
void dm_tc_set_edge_bottom(float bottom) { g_edge_b = isfinite(bottom) && bottom > 0 ? bottom : 0; }
void dm_tc_set_safe_insets(float left, float right) { g_safe_l = isfinite(left) && left > 0 ? left : 0; g_safe_r = isfinite(right) && right > 0 ? right : 0; }

void dm_tc_layout_make(dm_tc_layout *l, float width, float height) {
    memset(l, 0, sizeof(*l));
    l->width = isfinite(width) && width > 0 ? width : 0;
    l->height = isfinite(height) && height > 0 ? height : 0;
    float k = scale_for(l);
    l->tablet=g_tall_scale; l->swap_above=g_swap_above;
    // 右拇指技能扇区：主钮在下，四个技能保持原大小并留出独立触控间隔。
    // 技能扇区锚点避开侧边与底部安全区。
    // Right-thumb skill fan: main button at the bottom, the four skills keep their original size with independent touch gaps.
    // The skill fan anchors avoid the side and bottom safe areas.
    float main_x=l->width-g_safe_r-86*k, main_y=l->height-88*k-fmaxf(24*k,g_safe_b+8*k);
    if (g_tall_scale && height<340) main_y=fmaxf(main_y,190*k);
    // 整组向右下移动；小尺寸平板窗口保留原有底栏间隔。
    // Move the whole fan down-right. Compact tablet windows need the original belt clearance.
    main_x+=12*k;
    if (!(g_tall_scale && height<340)) main_y+=28*k;
    l->target[DM_TC_MAIN]=(dm_tc_target){main_x,main_y,40*k,DM_BTN_A};
    const float dx[4]={-96,-84,-34,34},dy[4]={18,-50,-92,-92};
    const uint64_t skill[4]={DM_BTN_B,DM_BTN_X,DM_BTN_Y,DM_BTN_R};
    for(int i=0;i<4;i++)
        l->target[DM_TC_SKILL1+i]=(dm_tc_target){main_x+dx[i]*k,main_y+dy[i]*k,26*k,skill[i]};
    /* 底栏 = 底部 UI v4 六格（NativeHUD/v4，游戏 HUD 按屏高缩放、以屏幕中线居中，所以按屏高换算）：
       效果图 2868×1320 里格心 x = 1090 + 120×格号、y = 1241.5、格宽 114；第 0 格难度，1–4 腰带四列，5 功能入口（PLUS）。
       回城不在底栏（离药水太近易误触）：改为技能弧最上面那颗（技能四）正上方的圆钮，同半径、间距 12，点下由宿主施放原版回城技能（dm_freetp.c），不出按键。
       Bottom bar = the six cells of bottom UI v4 (NativeHUD/v4; the game HUD scales by screen height and is centered on the screen centerline, so convert by screen height):
              in the 2868×1320 mock, cell center x = 1090 + 120×cell index, y = 1241.5, cell width 114; cell 0 difficulty, 1–4 the four belt columns, 5 the function entry (PLUS).
              Town portal is not in the bottom bar (too close to the potions, easy to hit by mistake): it is instead a round button directly above the topmost one on the skill arc (skill four), same radius, gap 12; on tap the host casts the original game's town portal skill (dm_freetp.c), with no key press. */
    /* 腰带四列对十字键：左 = 第 1 列、上 = 第 2 列、下 = 第 3 列、右 = 第 4 列（原版实测：按「右」第 4 列减、按「下」第 3 列减；首版把后两个写反了，真机才撞到） | The four belt columns map to the D-pad: left = column 1, up = column 2, down = column 3, right = column 4 (measured in the original game: pressing "right" decrements column 4, pressing "down" decrements column 3; the first version had the last two swapped, which was only hit on a real device) */
    const uint64_t bottom[5] = {DM_BTN_LEFT, DM_BTN_UP, DM_BTN_DOWN, DM_BTN_RIGHT, DM_BTN_PLUS};
    const int cell[5] = {1, 2, 3, 4, 5};
    float u = l->height / 1320.0f;
    for (int i = 0; i < 5; ++i)
        l->target[DM_TC_BELT1+i] = (dm_tc_target){l->width/2+(1090+120*cell[i]-1434)*u, 1241.5f*u, 57*u, bottom[i]};
    { const dm_tc_target *s4 = &l->target[DM_TC_SKILL4]; l->target[DM_TC_PORTAL] = (dm_tc_target){s4->x, s4->y-s4->r-12*k-26*k, 26*k, 0}; }
    // 小尺寸平板窗口：传送门放在技能三上方，避开右上角地图按钮。
    // Small tablet windows: put the portal above skill 3, clear of the top-right map control.
    if (g_tall_scale && height<340)
        l->target[DM_TC_PORTAL]=(dm_tc_target){main_x-34*k,main_y-156*k,26*k,0};
    /* 第 0 格难度与选择条：选择条 8 格同格宽、格距 120，中线对齐两球中线（效果图 1390），格心 y = 1051.5（七格之上、避开数值文字） | Cell 0 difficulty and the selector bar: the selector bar has 8 cells of the same cell width, cell pitch 120, centerline aligned with the centerline between the two orbs (mock 1390), cell center y = 1051.5 (above the seven cells, clear of the value text) */
    l->diff = (dm_tc_target){l->width/2+(1090-1434)*u, 1241.5f*u, 57*u, 0};
    for (int i = 0; i < 8; ++i) l->pick[i] = (dm_tc_target){l->width/2+(1390+(i-3.5f)*120-1434)*u, 1051.5f*u, 57*u, 0};
    l->target[DM_TC_MAP] = (dm_tc_target){l->width-30*k, 30*k, 22*k, DM_BTN_MINUS};
    // 默认换组键沿传送门外弧向左排布，保持到主技能的半径不变。
    // Default: continue the portal's outer arc to its left, preserving the radius around the primary.
    const dm_tc_target *portal=&l->target[DM_TC_PORTAL];
    float px=portal->x-main_x,py=portal->y-main_y,cs=cosf(.46f),sn=sinf(.46f);
    l->swap=(dm_tc_target){main_x+px*cs+py*sn,main_y-px*sn+py*cs,22*k,0};
    if (g_swap_above) l->swap=(dm_tc_target){main_x-124*k,main_y-114*k,22*k,0};
    /* 底栏外侧是血 / 蓝球；球不是输入目标。 | Outside the bottom bar are the life / mana orbs; the orbs are not input targets. */
    /* 左摇杆是浮动底盘：左下「起摇杆区」里手指落在哪，底盘圆心就落在哪，松手回静止位。
       静止位：圆心离安全区左界 90（底盘半径 60 + 30）、不小于 120，离屏幕底 115——只是没按时画在哪。
       起摇杆区：屏宽的左 40% × 屏高的下 55%（左与下到屏幕边：拇指压在刘海那一条上也算）；区里我们的圆钮 / 腰带格、原版界面上可点的东西各自优先。
       夹回：只为底盘整圈画在屏内——落点离左沿 / 底沿不到「半径 + 8」时底盘圆心挪回到那条线上；只影响画，轴值仍按手指相对落点算（摇杆头 = 底盘圆心 + 轴值 × 半径）。不让安全区（原先左界取「安全区 + 半径」，刘海机型约 122 点：
       拇指自然落在离左边 60–120 点的人一落指就带明显的向左分量；左下角没有刘海也没有系统横条）。静止位仍让安全区，那只是没按时画在哪
       The left stick is a floating base: wherever the finger lands inside the bottom-left "stick-start zone", the base centre goes there, and it returns to the rest position on release.
              Rest position: centre 90 from the left edge of the safe area (base radius 60 + 30), not less than 120, and 115 from the bottom of the screen - only where it is drawn while not pressed.
              Stick-start zone: the left 40% of the screen width x the lower 55% of the screen height (reaching the screen edges on the left and bottom: a thumb on the notch strip counts too); inside the zone our round buttons / belt cells and the clickable things of the original UI each take priority.
              Clamping: only so that the whole base is drawn on screen - when the touch point is closer than "radius + 8" to the left / bottom edge the base centre is moved back onto that line; it only affects drawing, and the axes are still computed from the finger relative to the touch point (stick knob = base centre + axes x radius). The safe area is not respected (the left bound used to be "safe area + radius", about 122 points on notched devices:
              a thumb landing naturally 60–120 points from the left edge got a clear leftward component the moment it touched down; the bottom-left corner has neither a notch nor the system bar). The rest position still respects the safe area, which is only where it is drawn while not pressed */
    l->joy_radius = 60*k;
    l->joy_x = fmaxf(90*k, g_safe_l + 60*k) + 30*k;
    l->joy_y = l->height - 115*k;
    l->joy_zone_r = l->width*0.40f; l->joy_zone_t = l->height*0.45f;
    l->joy_min_x = l->joy_radius + 8*k; l->joy_max_y = l->height - l->joy_radius - 8*k;
}

int dm_tc_hit(const dm_tc_layout *l, float x, float y) {
    if (!isfinite(x) || !isfinite(y) || x < 0 || y < 0 || x > l->width || y > l->height)
        return DM_TC_NONE;
    for (int i = 0; i < DM_TC_COUNT; ++i) {
        const dm_tc_target *t = &l->target[i];
        float dx = x-t->x, dy = y-t->y;
        if (t->r > 0 && dx*dx+dy*dy <= t->r*t->r) return i;
    }
    float sx = x-l->swap.x, sy = y-l->swap.y;
    if (l->swap.r > 0 && sx*sx+sy*sy <= l->swap.r*l->swap.r) return DM_TC_SWAP;
    return DM_TC_NONE;
}

static int skill_target(int target) {
    return target >= DM_TC_MAIN && target <= DM_TC_SKILL4;
}

/* 拖动方向（单位向量，屏幕坐标）：拖动不过 10 点死区 = 没有方向 | Drag direction (unit vector, screen coordinates): a drag that does not pass the 10-point dead zone = no direction */
static int aim_dir(const dm_tc_state *s, const dm_tc_finger *f, float *ux, float *uy) {
    float dx = f->x-f->aim_origin_x, dy = f->y-f->aim_origin_y, d = hypotf(dx, dy);
    if (!(d > 10 * scale_for(&s->layout))) return 0;
    *ux = dx/d; *uy = dy/d; return 1;
}

static int inside_map_cancel(const dm_tc_state *s, const dm_tc_finger *f) {
    const dm_tc_target *map = &s->layout.target[DM_TC_MAP];
    return map->r > 0 && fabsf(f->x-map->x) <= map->r && fabsf(f->y-map->y) <= map->r;
}

static int g_iso_fix = 1;
void dm_tc_set_iso_fix(int on) { g_iso_fix = !!on; }
static void update_aim(dm_tc_state *s) {
    s->rx = s->ry = 0;
    s->aim_slot = DM_TC_NONE;
    s->map_cancel_active = 0;
    if (s->navigation_mode) return;
    uint64_t latest = 0;
    for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) {
        const dm_tc_finger *f = &s->finger[i];
        if (!f->active || !skill_target(f->target)) continue;
        if (inside_map_cancel(s, f)) s->map_cancel_active = 1;
        if (!f->aim_cancelled && f->aim_move_sequence > latest) {
            latest = f->aim_move_sequence;
            s->aim_slot = i;
        }
    }
    if (s->pulse_button) { s->rx = s->pulse_rx; s->ry = s->pulse_ry; return; }
    float radius = 60 * scale_for(&s->layout);
    if (s->aim_slot == DM_TC_NONE || radius <= 0) return;
    const dm_tc_finger *f = &s->finger[s->aim_slot];
    if (hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y) <= 10 * scale_for(&s->layout)) return;   /* 主钮与技能钮同一死区：拖动不过 10 点 = 不给方向，原版自动选目标；长按连射时手指的小抖动不再改射击方向（公开 issue #92） | The main button and skill buttons share one dead zone: a drag under 10 points gives no direction and the game auto-targets; small finger jitter while holding to attack no longer changes the shooting direction (public issue #92) */
    float dx = (f->x-f->aim_origin_x)/radius;
    float dy = (f->aim_origin_y-f->y)/radius;
    s->rx = (int32_t)lroundf(fmaxf(-1, fminf(1, dx))*DM_STICK_MAX);
    s->ry = (int32_t)lroundf(fmaxf(-1, fminf(1, dy))*DM_STICK_MAX);
}

static void update(dm_tc_state *s) {
    s->buttons = 0;
    for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) {
        dm_tc_finger *f = &s->finger[i];
        if (f->active && !f->aim_cancelled && f->target >= 0 && f->target < DM_TC_COUNT) {
            uint64_t b = f->target==DM_TC_PORTAL && s->portal_pending ? DM_BTN_ZL : s->layout.target[f->target].button;
            /* 拖动定方向（核心手势）。技能钮：按下后先等 DM_TC_AIM_SETTLE_S 看是点按还是拖动——拖过死区就带着右轴方向立刻键下，
               没拖到点也键下（点按 = 原版自动选目标 / 朝向）；否则键先下、右轴后偏，按下那一发朝的是旧朝向。
               主钮：按下即键下，不等（它是最高频的键，点按不该多等；按下就拖的第一发是原生朝向，第二发起跟手）。
               按住期间方向再变由宿主的瞄准适配每拍重取，这里不做「松—按」；判定时间不为「按下稍停再拖」加长。
               Drag to set direction (the core gesture). Skill button: after press, first wait DM_TC_AIM_SETTLE_S to see whether it is a tap or a drag -- once dragged past the dead zone, key down immediately with the right-axis direction,
                              and key down too if the time is reached without dragging (tap = the original game's auto target / facing); otherwise the key would go down first and the right axis deflect later, and the shot at press would aim at the old facing.
                              Main button: key down on press, no wait (it is the most frequent key and a tap should not wait longer; with press-then-drag the first shot uses the native facing, and from the second shot on it follows the finger).
                              Direction changes while held are re-read every tick by the host's aim adapter, so no "release-press" is done here; the decision time is not lengthened for "press, pause, then drag". */
            if (!s->navigation_mode && f->target >= DM_TC_SKILL1 && f->target <= DM_TC_SKILL4) {
                float ux, uy;
                if (!f->key_down && !aim_dir(s, f, &ux, &uy) && s->last_time_s - f->start_time_s < DM_TC_AIM_SETTLE_S) b = 0;
                else if (!f->key_down) { f->key_down = 1; f->key_down_s = s->last_time_s; }
            }
            /* 多排腰带：腰带格上滑 = 展开、下滑 = 收起，所以键不在按下时发；一律等松手那一刻按位移判（dm_tc_end_at）。不设「按住到点就喝」的计时：
               手慢一点的上滑 / 下滑会先被它判成喝药（真机与 mini 各撞一次）。只有 1 排时照旧按下即喝
               Multi-row belt: swiping up on a belt cell = expand, swiping down = collapse, so the key is not sent on press; always wait for the moment of release and decide by displacement (dm_tc_end_at). No "drink once held long enough" timer:
                              a slightly slow swipe up / down would first be judged by it as drinking a potion (hit once each on a real device and on the second Mac). With only 1 row, still drink on press as before */
            /* 只有 1 排时也一律松手那一刻喝（和多排同一条路） | with only 1 row, also always drink at the moment of release (same path as multiple rows) */
            if (!s->navigation_mode && f->target >= DM_TC_BELT1 && f->target <= DM_TC_BELT4) b = 0;
            s->buttons |= b;
        }
    }
    s->buttons |= s->pulse_button;
    /* 第二套技能：ZL 是原版的换组修饰键。只在用技能的那一刻带上——手指按着主钮 / 技能钮、或技能脉冲在发（含等着发）时；
       其余时候不按：一直按着会让十字（+）、腰带、回城都变成「ZL + 键」的组合（Ⅱ 态下十字开不了面板）。
       ZL 先下、主钮的键晚 DM_TC_ZL_LEAD_S 再下（同一帧按下游戏只认主键 = 放成第一组的技能）。
       Second skill set: ZL is the original game's set-switch modifier. It is added only at the moment a skill is used -- while a finger is on the main button / a skill button, or a skill pulse is being sent (including waiting to be sent);
              it is not pressed the rest of the time: holding it all the time would turn the cross (+), the belt and town portal all into "ZL + key" combos (in state Ⅱ the cross could not open the panel).
              ZL goes down first, and the main button's key goes down DM_TC_ZL_LEAD_S later (pressed in the same frame, the game only recognizes the main key = casts the first set's skill). */
    if (s->skill_set && !s->navigation_mode) {
        /* 在发的脉冲只有是技能键（主钮 / 技能钮补的那一下）才带 ZL：多排腰带松手补的喝药脉冲带上 ZL 就成了原版「ZL + 腰带键 = 喂佣兵」
           （本机 实跑：第二组下点腰带，0x26 包目标字节 = 1、佣兵生命 642 → 762 涨、玩家不变）
           The pulse being sent carries ZL only if it is a skill button (the make-up press of the main button / a skill button): a drink pulse made up on release of a multi-row belt, if it carried ZL, would become the original game's "ZL + belt key = feed the mercenary"
                      (actual run on the local machine: tapping the belt under the second set, 0x26 packet target byte = 1, mercenary life rose 642 → 762, player unchanged) */
        /* 等着补发的技能也要 ZL 领先——但只在没有别的脉冲在发时：腰带脉冲在发时带上 ZL 同样成了「喂佣兵」（单测探针读到 0x1100） | A skill waiting for a make-up send also needs ZL to lead -- but only when no other pulse is being sent: adding ZL while a belt pulse is being sent likewise becomes "feed the mercenary" (the unit-test probe read 0x1100) */
        int use = s->defer_button != 0 && !s->pulse_button;
        for (int t = DM_TC_MAIN; t <= DM_TC_SKILL4 && !use; ++t) if (s->pulse_button && s->pulse_button == s->layout.target[t].button) use = 1;
        /* 手指按着第二组技能时发「给玩家喝」的腰带脉冲：ZL 还按着就成了原版「ZL + 腰带键 = 喂佣兵」。ZL 和按着的技能键要一起让开
           （只松 ZL 不松技能键 = 放成第一组的技能），而且要先让开、隔一段再按腰带键：同一拍里「松 ZL + 按腰带键」游戏仍按 ZL 还在算
           （真局实测：发出的还是喂佣兵包——宿主取手柄处对按下沿做锁存，这一拍采到的是 ZL 与腰带键都在）。所以分三段：
           等着发（belt_gap_cell）→ 脉冲在发 → 收回后 ZL 先回来、领先满 DM_TC_ZL_LEAD_S 技能键再回来（同一帧按下游戏只认主键）
           The "drink for the player" belt pulse sent while a finger holds a skill of the second skill set: with ZL still held it becomes the original game's "ZL + belt key = feed the mercenary". ZL and the held skill key have to get out of the way together
           (releasing only ZL and not the skill key = casting the first set's skill), and they have to get out of the way first, with the belt key pressed after a gap: with "release ZL + press belt key" in the same tick the game still counts ZL as held
           (measured in a real run: what went out was still the feed-the-mercenary packet -- where the host fetches the controller it latches the press edge, and what was sampled this tick was both ZL and the belt key down). So there are three stages:
           waiting to send (belt_gap_cell) → the pulse is being sent → after it is withdrawn ZL comes back first, and the skill key comes back once ZL has led by a full DM_TC_ZL_LEAD_S (pressed in the same frame, the game only recognizes the main key) */
        int belt_pulse = s->belt_gap_cell != 0;
        for (int t = DM_TC_BELT1; t <= DM_TC_BELT4; ++t) if (s->pulse_button && s->pulse_button == s->layout.target[t].button) belt_pulse = 1;
        if (belt_pulse) s->belt_pulse_s = s->last_time_s;
        for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) {
            const dm_tc_finger *f = &s->finger[i];
            if (!f->active || f->aim_cancelled || f->target < DM_TC_MAIN || f->target > DM_TC_SKILL4) continue;
            if (!belt_pulse) use = 1;
            if (belt_pulse || s->last_time_s - f->start_time_s < DM_TC_ZL_LEAD_S || (s->belt_pulse_s > 0 && s->last_time_s - s->belt_pulse_s < DM_TC_ZL_LEAD_S)) s->buttons &= ~s->layout.target[f->target].button;
        }
        if (use) s->buttons |= DM_BTN_ZL;
    }
    update_aim(s);
    s->lx = s->ly = 0;
    if (s->joy_slot < 0) return;
    dm_tc_finger *f = &s->finger[s->joy_slot];
    float dx = f->x-s->joy_touch_x, dy = s->joy_touch_y-f->y;   /* 相对落点，不相对底盘圆心：夹回只影响底盘画在哪，不进算法（贴角落指时圆心在手指右上方，按圆心算会出现手指往右、人往下） | relative to the touch point, not the base centre: clamping only affects where the base is drawn and does not enter the computation (touching down in a corner puts the centre to the upper right of the finger, and computing from the centre would make the character move down while the finger moves right) */
    /* 输入行程不跟大屏的视觉放大走：拇指不随屏幕变大。满偏 60 点、死区 6 点，手机与 iPad 一致
       （原先乘视觉倍数，12.9 寸 iPad 要推 105 点 ≈ 20 mm 才满偏，是手机的两倍）。底盘和摇杆头的大小仍按视觉倍数画。
       Input travel does not follow the visual enlargement on large screens: the thumb does not grow with the screen. Full deflection 60 points, dead zone 6 points, same on phone and iPad
              (it used to be multiplied by the visual factor, so a 12.9-inch iPad needed a 105-point ≈ 20 mm push for full deflection, twice the phone's). The size of the base and the stick head is still drawn by the visual factor. */
    float in_k = fminf(1.0f, scale_for(&s->layout));
    float distance = hypotf(dx, dy), radius = 60 * in_k;
    float dead = 6 * in_k;
    if (distance <= dead || radius <= dead) return;
    float strength = fminf(1, (distance-dead)/(radius-dead));
    /* 斜向预修正：原版把摇杆做纯 45° 旋转成世界方向、不补偿等距画面的 2:1，人实际走的屏幕角 = atan(tan(摇杆角) / 2)（推 45° 走 26.6°）。
       手柄玩家看不见摇杆和画面的对应，触屏上手指画的就是屏幕方向，所以送值前把纵向放大 2 倍再缩回原幅度：屏幕角 = 手指角，推多深不变。
       只动游戏态的移动摇杆（导航态没有摇杆值；技能方向走右轴，不经这里）
       Diagonal pre-correction: the original game turns the stick into a world direction by a pure 45° rotation and does not compensate for the 2:1 of the isometric view, so the screen angle the character actually walks = atan(tan(stick angle) / 2) (push 45°, walk 26.6°).
              Controller players do not see the mapping between stick and picture, but on a touchscreen what the finger draws is the screen direction, so before sending the value the vertical component is doubled and then scaled back to the original magnitude: screen angle = finger angle, push depth unchanged.
              Only the movement stick in game state is affected (navigation state has no stick value; skill direction goes through the right axis and does not pass here) */
    float ex = dx/distance, ey = dy/distance;
    if (g_iso_fix) { float n = hypotf(ex, 2*ey); ex /= n; ey = 2*ey/n; }
    s->lx = (int32_t)lroundf(ex * strength * DM_STICK_MAX);
    s->ly = (int32_t)lroundf(ey * strength * DM_STICK_MAX);
}

static int pointer_finger(const dm_tc_state *s, const dm_tc_finger *f) {
    return s->navigation_mode && f->active && s->pointer_slot >= 0 &&
        &s->finger[s->pointer_slot] == f;
}

static void update_pointer(dm_tc_state *s) {
    if (!s->pointer_valid || s->pointer_slot < 0 || s->pointer_slot >= DM_TC_MAX_TOUCH) return;
    dm_tc_finger *f = &s->finger[s->pointer_slot];
    if (f->pointer_dragged && s->pointer_pickup_done && !f->shop_drag && !f->locked_drag) {
        s->pointer_x = f->x;
        s->pointer_y = f->y - DM_TC_POINTER_LIFT_PT * scale_for(&s->layout);
    }
}

/* 指针脉冲（kind 1 = 拖放的拿起，2 = 放下 / 点按）：指针先落到 (x,y) 再按 A。上一个脉冲还没收（游戏还没取走）时不动指针、只记下，
   等前端在上一个收回且游戏已看到一次「A 松开」之后调 dm_tc_pulse_resume 才开始——否则两下 A 连成一下、或拿起落在放下的位置
   Pointer pulse (kind 1 = pick-up of a drag-and-drop, 2 = put-down / tap): the pointer first lands at (x,y), then A is pressed. While the previous pulse has not been released (the game has not consumed it yet) the pointer is not moved and the request is only recorded,
      and it starts only when the front end calls dm_tc_pulse_resume, after the previous one is released and the game has seen "A released" once -- otherwise two A presses merge into one, or the pick-up lands at the put-down position */
static void pointer_pulse_btn(dm_tc_state *s, float x, float y, int kind, uint64_t button, double hold_s) {
    if (!s->pulse_button) {
        s->pointer_x = x; s->pointer_y = y;
        s->pulse_button = button;
        s->pulse_hold_s = hold_s;
        s->pulse_rx = s->pulse_ry = 0;
        s->pointer_pulse_active = kind;
        ++s->pulse_epoch;
    } else {
        s->pointer_pulse_pending = kind;
        s->pointer_pending_x = x;
        s->pointer_pending_y = y;
        s->pointer_pending_button = button; s->pointer_pending_hold = hold_s;
    }
}
static void pointer_pulse_start(dm_tc_state *s, float x, float y, int kind) { pointer_pulse_btn(s, x, y, kind, DM_BTN_A, 0); }
void dm_tc_pointer_release(dm_tc_state *s, int kind) { s->pointer_release_item = kind == 1 || kind == 3 || kind == 4 || kind == 8 ? kind : 0; }
void dm_tc_pointer_release_at(dm_tc_state *s, float x, float y) { s->pointer_release_item = 5; s->release_x = x; s->release_y = y; }
void dm_tc_set_holding(dm_tc_state *s, int holding) { s->holding = !!holding; }
void dm_tc_pointer_item(dm_tc_state *s, int kind) {
    if (kind == 7) { s->pointer_shop = 1; kind = 1; }
    s->pointer_lock = kind == 10; if (kind == 10) kind = 1;
    s->pointer_stack = kind == 8; if (kind == 8) kind = 1;   /* 8 = 高级仓库的成叠格：照物品区，只有双击不同 | 8 = stacked cell of the advanced stash: follows the item area, only double tap differs */   /* 7 = 商人货架上的货：别的都照物品区，只有起拖不同 | 7 = goods on the vendor shelf: everything else follows the item area, only drag start differs */
    s->pointer_item = (kind >= 1 && kind <= 3) || kind == 6 || kind == 11 ? kind : 0;   /* 11 = 这一下不点（底下是被盖住的按钮），但可以起滚动手势（设置页列表里的文字 / 空处） | 11 = this tap does not click (a covered button is underneath), but it may start a scroll gesture (text / empty space in the settings page list) */
    if (s->mouse_click_mode && s->pointer_slot >= 0 && s->pointer_slot < DM_TC_MAX_TOUCH && s->finger[s->pointer_slot].active && !s->finger[s->pointer_slot].pointer_dragged) {
        if (kind == 1) { if (!s->pulse_button) { s->pointer_valid = 1; s->pointer_x = s->finger[s->pointer_slot].aim_origin_x; s->pointer_y = s->finger[s->pointer_slot].aim_origin_y; } }
        else s->pointer_valid = 0;   /* 不在物品区：放开手柄光标（前端据此清掉光标请求，不再推摇杆） | Not in the item area: release the controller cursor (based on this the front end clears the cursor request and stops pushing the stick) */
    }
}   /* 1 物品区 2 页签（点按不发键也不发鼠标，前端另调宿主切页） | 1 item area, 2 tab (a tap sends neither a key nor a mouse click; the front end separately calls the host to switch pages) */
static _Atomic int g_dead_modal;
static _Atomic int g_vendor_open;   /* 商人交易页开着：ZL 是切货架页签，「再点同一处 = 比较」不发 | Vendor trade page open: ZL switches shelf tabs, so "tap the same spot again = compare" is not sent */
static _Atomic int g_close_stop;   /* 「面板外点按 = 按返回到面板关掉」途中弹出了确认框（名字以 Modal 结尾的面板打开）：后面的返回键不再按 | A confirmation box popped up (a panel whose name ends in Modal opened) in the middle of "tap outside the panel = press back until the panel closes": stop pressing the remaining back presses */
static _Atomic int g_tap_box;   /* 最近一次单击的物品是魔盒（前端每拍从宿主同步） | The most recently single-tapped item is the cube (synced by the front end from the host every tick) */
void dm_tc_set_tap_box(int on) { atomic_store(&g_tap_box, on != 0); }
static _Atomic unsigned g_cur_stack;
void dm_tc_set_cur_stack(unsigned id) { atomic_store(&g_cur_stack, id); }
static _Atomic unsigned g_hover_unid;
void dm_tc_set_hover_unid(unsigned id) { atomic_store(&g_hover_unid, id); }
static _Atomic int g_imbue_open;   /* 单件任务槽面板开着（灌魔 / 打孔 / 个性化 / 古墓底座，同一个 ImbueItemsPanel） | Single-item quest slot panel open (imbue / socket / personalize / tomb pedestal, all the same ImbueItemsPanel) */
static _Atomic int g_cube_open;   /* 赫拉迪克方块页开着：双击 = 按住 Y（原版「移到另一侧」：背包 → 方块 / 方块 → 背包），不是「使用 / 装备」 | Horadric Cube page open: double tap = hold Y (the original game's "move to the other side": inventory → cube / cube → inventory), not "use / equip" */
static _Atomic int g_vault_open;   /* 储藏箱开在第六页「编年史」：双击背包物品 = 存进编年史仓库（走 use_seq，宿主 ui_use_pump 判页），不发原版「按住 Y」——那一页游戏当魔盒页 | Stash open on page six 「编年史」(Chronicle): double-tapping an inventory item = store it in the Chronicle vault (goes through use_seq; the host's ui_use_pump determines the page); do not send the original game's "hold Y" -- the game treats that page as the cube page */
static _Atomic int g_bank_open;   /* 储藏箱开着（前端从宿主读游戏面板树的结果设进来，不靠开关消息）：双击 = 按住 Y（移到另一侧） | Stash open (set by the front end from the host's reading of the game panel tree, not from open/close messages): double tap = hold Y (move to the other side) */
int dm_tc_panel_mode(int nav, const dm_tc_scene *scene, int blocked) {   /* 原在 front_ios.m（nmode）；菜单上又开了雇佣列表这类页时读数 > 0，不算「只开着对话菜单」 | formerly in front_ios.m (nmode); when a page such as the hire list is also open over the menu the reading is > 0, which does not count as "only the dialog menu open" */
    return nav ? (dm_tc_scene_dead() ? 2 : dm_tc_scene_dialog_only(scene) && blocked <= 0 ? 3 : 1) : 0;
}
void dm_tc_set_panel_state(int bits) { atomic_store(&g_bank_open, (bits & 1) != 0); atomic_store(&g_vendor_open, (bits & 2) != 0); atomic_store(&g_dead_modal, (bits & 4) != 0); atomic_store(&g_imbue_open, (bits & 16) != 0); atomic_store(&g_cube_open, (bits & 32) != 0); atomic_store(&g_vault_open, (bits & 64) != 0); }   /* 位同 dm_cursor.h 的 DM_PS_*；宿主读游戏状态得来 | Bits are the same as DM_PS_* in dm_cursor.h; obtained by the host reading the game state */

static void pointer_drag_begin(dm_tc_state *s, dm_tc_finger *f) {
    f->pointer_dragged = 1;
    if (s->pointer_lock) { f->locked_drag = 1; return; }   /* 单件槽占用时从别的物品上起拖：不拿起，这次拖动什么都不做 | Drag starting from another item while the single-item slot is occupied: do not pick up; this drag does nothing */
    if (s->pointer_shop || s->pointer_stack) { f->shop_drag = 1; s->pointer_x = 2; s->pointer_y = 2; update(s); return; }   /* 从货架上的货起拖：不拿起；光标挪到屏幕角上，让货上的说明框收掉（买的时候宿主再把光标放回这件货上） | Drag starting from goods on the shelf: do not pick up; move the cursor to a screen corner so the tooltip on the goods closes (at purchase time the host puts the cursor back on that item) */
    s->pointer_pickup_done = 0;
    pointer_pulse_start(s, f->aim_origin_x, f->aim_origin_y, 1);
}

void dm_tc_cancel_all(dm_tc_state *s) {
    ++s->gesture_epoch; s->portal_pending=s->portal_phase=0; s->diff_open=0;
    memset(s->finger, 0, sizeof(s->finger));
    for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) s->finger[i].target = DM_TC_NONE;
    s->joy_slot = DM_TC_NONE;
    s->joy_origin_x = s->joy_origin_y = 0;
    s->buttons = 0;
    s->lx = s->ly = s->rx = s->ry = 0;
    s->aim_slot = DM_TC_NONE;
    s->aim_move_sequence = 0;
    s->map_cancel_active = 0;
    s->pulse_button = 0; s->pulse_rx = s->pulse_ry = 0;
    s->belt_q = 0; s->belt_gap_cell = 0;   /* 排着的、等着发的喝药一起作废 | queued and pending drinks are voided as well */   /* 等着判单双击的腰带格、等着发的喂雇佣兵一起作废 | The belt cell waiting for single/double tap decision and the pending mercenary feed are voided together */
    s->defer_button = 0;   /* 等着补发的那下技能也作废：原先取消后 0.2 秒照样放出来 | The skill press waiting for a make-up send is voided as well: previously it was still cast 0.2 seconds after the cancel */
    s->pointer_slot = DM_TC_NONE;
    s->pointer_valid = 0; s->pointer_x = s->pointer_y = 0;
    s->pointer_pickup_done = 0;
    s->pointer_pulse_active = s->pointer_pulse_pending = 0;
    s->pointer_pending_x = s->pointer_pending_y = 0;
}

int dm_tc_pulse_end(dm_tc_state *s, uint64_t epoch) {
    if (!s->pulse_button || s->pulse_epoch != epoch) return 0;
    int ended_pointer = s->pointer_pulse_active;
    if (s->defer_button && s->defer_at_s < s->last_time_s + DM_TC_ZL_LEAD_S) s->defer_at_s = s->last_time_s + DM_TC_ZL_LEAD_S;   /* 等着补发的技能：从这一刻起 ZL 才能领先（见 dm_tc_tick） | Skill waiting for a make-up send: ZL can start leading only from this moment (see dm_tc_tick) */
    s->pulse_button = 0; s->pulse_rx = s->pulse_ry = 0; s->pulse_hold_s = 0;
    s->pointer_pulse_active = 0;
    if (ended_pointer == 1) {
        s->pointer_pickup_done = 1;
        update_pointer(s);
    }
    update(s);
    return 1;
}

int dm_tc_pulse_pending(const dm_tc_state *s) { return !s->pulse_button && s->pointer_pulse_pending; }

int dm_tc_pulse_resume(dm_tc_state *s) {
    if (s->pulse_button || !s->pointer_pulse_pending) return 0;
    int kind = s->pointer_pulse_pending;
    s->pointer_pulse_pending = 0;
    pointer_pulse_btn(s, s->pointer_pending_x, s->pointer_pending_y, kind, s->pointer_pending_button ? s->pointer_pending_button : DM_BTN_A, s->pointer_pending_hold);
    s->pointer_pending_button = 0; s->pointer_pending_hold = 0;
    update(s);
    return 1;
}

void dm_tc_init(dm_tc_state *s, float width, float height) {
    memset(s, 0, sizeof(*s));
    s->pointer_release_item = 1;
    dm_tc_layout_make(&s->layout, width, height);
    s->enabled_targets = (1u << DM_TC_COUNT) - 1;
    dm_tc_cancel_all(s);
}

void dm_tc_resize(dm_tc_state *s, float width, float height) {
    uint64_t portal = s->layout.target[DM_TC_PORTAL].button;
    dm_tc_cancel_all(s);
    dm_tc_layout_make(&s->layout, width, height);
    s->layout.target[DM_TC_PORTAL].button = portal;
}

void dm_tc_enable_targets(dm_tc_state *s, uint32_t mask) {
    mask &= (1u << DM_TC_COUNT) - 1;
    if (mask == s->enabled_targets) return;
    dm_tc_cancel_all(s);
    s->enabled_targets = mask;
}

void dm_tc_set_joy_block(dm_tc_state *s, int block) { s->joy_block = block != 0; }
void dm_tc_set_navigation_mode(dm_tc_state *s, int navigation_mode) {
    navigation_mode = !!navigation_mode;
    if (navigation_mode == s->navigation_mode) return;
    dm_tc_cancel_all(s);
    s->navigation_mode = navigation_mode;
}

/* 系统取消了这根手指（通知横幅、系统手势）：不当松手——不补脉冲、不点世界、不点界面；只把它按着的键、摇杆、光标放掉 | The system cancelled this finger (notification banner, system gesture): do not treat it as a release -- no make-up pulse, no world tap, no UI click; only let go of the keys, stick and cursor it was holding */
void dm_tc_cancel_finger(dm_tc_state *s, int slot) {
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH) return;
    ++s->gesture_epoch;
    if (s->finger[slot].active && s->finger[slot].target == DM_TC_PORTAL) { dm_tc_cancel_portal(s); }
    if (slot == s->joy_slot && s->finger[slot].active) { s->joy_lost_s = s->last_time_s; s->joy_lost_x = s->finger[slot].x; s->joy_lost_y = s->finger[slot].y; s->joy_lost_ox = s->joy_origin_x; s->joy_lost_oy = s->joy_origin_y; s->joy_lost_tx = s->joy_touch_x; s->joy_lost_ty = s->joy_touch_y; }   /* 系统取消了摇杆那根手指：记下，紧接着在附近重新落下的接着当摇杆 | the system cancelled the joystick finger: record it, and a finger landing again nearby right afterwards continues as the joystick */
    s->finger[slot] = (dm_tc_finger){.target=DM_TC_NONE};
    if (slot == s->joy_slot) { s->joy_slot = DM_TC_NONE; s->joy_origin_x = s->joy_origin_y = 0; }
    if (slot == s->aim_slot) s->aim_slot = DM_TC_NONE;
    if (slot == s->pointer_slot) { s->pointer_item = 0; s->pointer_shop = 0; s->pointer_stack = 0; s->pointer_lock = 0; s->pointer_slot = DM_TC_NONE; s->pointer_valid = 0; }
    update(s);
}
void dm_tc_cancel_portal(dm_tc_state *s) {
    ++s->gesture_epoch; s->portal_pending=s->portal_phase=0;
    for (int i=0;i<DM_TC_MAX_TOUCH;++i)
        if(s->finger[i].active && s->finger[i].target==DM_TC_PORTAL)
            s->finger[i]=(dm_tc_finger){.target=DM_TC_NONE};
    update(s);
}
void dm_tc_set_portal_binding(dm_tc_state *s,uint64_t mask,uint64_t identity,uint64_t scene) {
    if(s->layout.target[DM_TC_PORTAL].button==mask && s->portal_binding_identity==identity && s->portal_scene_epoch==scene)return;
    dm_tc_cancel_portal(s);
    s->portal_binding_identity=identity;s->portal_scene_epoch=scene;
    s->layout.target[DM_TC_PORTAL].button=mask;update(s);
}
void dm_tc_set_portal_button(dm_tc_state *s,uint64_t mask) {
    dm_tc_set_portal_binding(s,mask,s->portal_binding_identity,s->portal_scene_epoch);
}
int dm_tc_portal_ack(dm_tc_state *s,uint64_t epoch,uint64_t identity,uint64_t scene) {
    if(!s->portal_pending || s->gesture_epoch!=epoch || s->portal_binding_identity!=identity || s->portal_scene_epoch!=scene)return 0;
    int active=0;for(int i=0;i<DM_TC_MAX_TOUCH;i++)active|=s->finger[i].active && s->finger[i].target==DM_TC_PORTAL;
    if(!active)return 0;
    s->portal_pending=0;s->portal_phase=2;update(s);return 1;
}

/* 腰带展开着、这一点不在腰带上（底排四格与展开的几排之外）：前端据此在自己的手势（小地图块、空位十字）之前先收腰带并吃掉这一下 | Belt expanded and this point is not on the belt (outside the four bottom-row cells and the expanded rows): based on this the front end collapses the belt and consumes this tap before its own gestures (minimap block, empty-slot cross) */
int dm_tc_belt_outside(const dm_tc_state *s, float x, float y) {
    if (s->navigation_mode || !s->belt_open || s->belt_rows <= 1) return 0;
    int hit = dm_tc_hit(&s->layout, x, y); if (hit >= DM_TC_BELT1 && hit <= DM_TC_BELT4) return 0;
    float u = s->layout.height / 1320.0f;
    for (int r = 1; r < s->belt_rows && r < 4; ++r) for (int c = 0; c < 4; ++c) {
        const dm_tc_target *t = &s->layout.target[DM_TC_BELT1+c];
        if (fabsf(x - t->x) <= 60*u && fabsf(y - (t->y - (165.0f + 162.0f*(r-1))*u)) <= 53.75f*u) return 0;
    }
    return 1;
}
/* 腰带点一下 = 自己喝，点几下喝几瓶（原版手感，不判双击）。底排 = 补一个腰带键脉冲（不带 ZL，第二组下也不带）；上面几排手柄没有键 = belt_request 交宿主喝那一格。
   喂雇佣兵不在腰带上做手势：佣兵面板里的原版入口和「自动给佣兵喝药」设置照旧
   One tap on the belt = the player drinks, several taps drink several (original feel, no double-tap detection). Bottom row = one belt key pulse (without ZL, also under the second set); upper rows have no controller key = belt_request asks the host to drink that cell.
      Feeding the mercenary has no gesture on the belt: the game's own entry in the mercenary panel and the "auto-feed mercenary" setting are unchanged */
static void belt_fire(dm_tc_state *s, int cell) {
    if (cell >= 0x100) { s->belt_request = cell; return; }
    uint64_t btn = s->layout.target[DM_TC_BELT1 + (cell & 0xf)].button; if (!btn) return;
    if (s->pulse_button || s->defer_button || s->belt_gap_cell) { s->belt_q = cell; return; }   /* 有脉冲在发 / 有补发在等：排着（dm_tc_tick 空了再发） | A pulse is being sent / a make-up send is waiting: queue it (dm_tc_tick sends it when free) */
    if (s->skill_set && !s->navigation_mode) for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) {   /* 按着第二组技能：先松 ZL 和技能键，隔 DM_TC_ZL_LEAD_S 再按腰带键（见 update） | a skill of the second skill set is held: first release ZL and the skill key, and press the belt key after DM_TC_ZL_LEAD_S (see update) */
        const dm_tc_finger *f = &s->finger[i]; if (!f->active || f->aim_cancelled || f->target < DM_TC_MAIN || f->target > DM_TC_SKILL4) continue;
        s->belt_gap_cell = 0x1000 | cell; s->belt_gap_at = s->last_time_s + DM_TC_ZL_LEAD_S; update(s); return; }
    s->pulse_rx = s->pulse_ry = 0; s->pulse_button = btn; ++s->pulse_epoch;
    update(s);
}
void dm_tc_begin_at(dm_tc_state *s, int slot, float x, float y, double now_s) {
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH || s->finger[slot].active || !isfinite(x) || !isfinite(y)) return;
    s->last_time_s = now_s;
    dm_tc_finger *f = &s->finger[slot];
    if (!s->navigation_mode) {   /* 难度：点第 0 格开 / 收选择条；展开时点某格 = 选档并收起，点别处只收起、照常处理 | Difficulty: tapping cell 0 opens / collapses the selector bar; when expanded, tapping a cell = select that level and collapse; tapping elsewhere only collapses and is then handled as usual */
        const dm_tc_target *d = &s->layout.diff;
        int on_diff = d->r > 0 && hypotf(x-d->x, y-d->y) <= d->r, pick = -1;
        for (int i = 0; s->diff_open && i < 8; ++i) if (hypotf(x-s->layout.pick[i].x, y-s->layout.pick[i].y) <= s->layout.pick[i].r) pick = i;
        if (on_diff || pick >= 0) {
            if (pick >= 0) s->diff_request = pick + 1;
            s->diff_open = on_diff ? !s->diff_open : 0;
            *f = (dm_tc_finger){.active=1, .target=DM_TC_DIFF, .x=x, .y=y}; update(s); return;
        }
        s->diff_open = 0;
    }
    int hit = dm_tc_hit(&s->layout, x, y);
    if (!s->navigation_mode && s->belt_open && s->belt_rows > 1) {   /* 腰带展开着：点上面几排 = 喝那一格；点腰带以外 = 只收起 | Belt expanded: tapping the upper rows = drink that cell; tapping outside the belt = only collapse */
        float u = s->layout.height / 1320.0f;   /* 展开的几排是游戏自己画的：列与底排对齐，排距 162、第一排比底排高 165（效果图单位；真机 2868×1320 实量，行距由 gen_hud_v4.py 的 bpy 定，改那边要一起改） | The expanded rows are drawn by the game itself: columns align with the bottom row, row pitch 162, the first row is 165 above the bottom row (mock units; measured on a real device at 2868×1320; the row spacing is set by bpy in gen_hud_v4.py, and changing it there requires changing it here too) */
        for (int r = 1; r < s->belt_rows && r < 4; ++r) for (int c = 0; c < 4; ++c) {
            const dm_tc_target *t = &s->layout.target[DM_TC_BELT1+c];
            if (fabsf(x - t->x) <= 60*u && fabsf(y - (t->y - (165.0f + 162.0f*(r-1))*u)) <= 53.75f*u) {
                belt_fire(s, 0x100 | r << 4 | c);   /* 按下就喝那一格 | drink that cell on press */
                *f = (dm_tc_finger){.active=1, .target=DM_TC_DIFF, .x=x, .y=y}; update(s); return;
            }
        }
        if (hit < DM_TC_BELT1 || hit > DM_TC_BELT4) {   /* 点别处：只收腰带，这一下不再做别的（不走路、不点世界、不按键） | Tap elsewhere: only collapse the belt; this tap does nothing else (no walking, no world tap, no key press) */
            s->belt_request = 2; *f = (dm_tc_finger){.active=1, .target=DM_TC_DIFF, .x=x, .y=y}; update(s); return;
        }
    }
    if (hit == DM_TC_SWAP) {
        if (s->navigation_mode) hit = DM_TC_NONE;
        else { *f = (dm_tc_finger){.active=1, .target=DM_TC_SWAP, .x=x, .y=y}; update(s); return; }
    }
    int enabled = hit >= 0 && (s->enabled_targets & (1u << hit));
    /* 面板态：被禁用的目标（底栏腰带 / 功能键 / 回城、圆钮收起后的技能钮位置）不占手指——它们压着的地方可能正是面板的按钮
       （设置页「应用」在屏幕底部，落在禁用的底栏目标上，既不判类也不点击）。游戏态仍占着（防隐藏的回城触点变成摇杆）
       Panel state: disabled targets (bottom-bar belt / function key / town portal, and the skill button positions after the round buttons are hidden) do not capture fingers -- the place they cover may be exactly a panel button
              (「应用」(Apply) on the settings page is at the bottom of the screen and landed on a disabled bottom-bar target, getting neither classified nor clicked). In game state they still capture (preventing a hidden town-portal touch point from turning into the stick) */
    if (s->navigation_mode && hit >= 0 && !enabled) hit = DM_TC_NONE;
    if(enabled && hit==DM_TC_PORTAL && s->layout.target[hit].button && s->portal_phase==0) {
        ++s->gesture_epoch;
        uint64_t mask=s->layout.target[hit].button;
        s->portal_pending=(mask&DM_BTN_ZL) && (mask&~DM_BTN_ZL);
        s->portal_phase=s->portal_pending?1:2;
    }
    *f = (dm_tc_finger){.active=1, .target=enabled ? hit : DM_TC_NONE, .x=x, .y=y,
                           .aim_origin_x=x, .aim_origin_y=y,
                           .start_time_s=now_s};
    /* 禁用圆仍阻止落入左摇杆区，防止隐藏回城触点变成移动。 | A disabled circle still blocks falling through into the left stick area, preventing a hidden town-portal touch point from turning into movement. */
    if (s->navigation_mode && hit == DM_TC_NONE && s->pointer_slot == DM_TC_NONE) {
        s->pointer_slot = slot;
        s->pointer_item = 0;
        /* 鼠标点击模式下，手柄光标只跟「落在物品区的手指」走（前端答出命中后调 dm_tc_pointer_item）：
           光标是靠推摇杆走的，在没有自由光标的页面（选项、暂停菜单、小站）推摇杆 = 改当前选项 / 挪焦点（点页签顺带改了「显示物品」）
           In mouse click mode the controller cursor follows only "a finger that landed in the item area" (the front end calls dm_tc_pointer_item once it has the hit answer):
                      the cursor moves by pushing the stick, and on pages without a free cursor (options, pause menu, waypoint) pushing the stick = changing the current option / moving the focus (tapping a tab changed 「显示物品」(Show Items) as a side effect) */
        if (!s->mouse_click_mode) { s->pointer_valid = 1; s->pointer_x = x; s->pointer_y = y; }
    } else if (!s->navigation_mode && hit == DM_TC_NONE && g_edge_b > 0 && x >= 0 && x < s->layout.width*0.43f && y > s->layout.height - g_edge_b) {
        f->target = DM_TC_EDGE;
    } else if (!s->navigation_mode && !s->joy_block && hit == DM_TC_NONE && s->joy_slot == DM_TC_NONE && x >= 0 && y <= s->layout.height &&
        ((x < s->layout.joy_zone_r && y > s->layout.joy_zone_t) ||
         (x < s->layout.width*0.43f && y > s->layout.height*0.25f && s->joy_lost_s > 0 && now_s - s->joy_lost_s <= DM_TC_JOY_RESUME_S && hypotf(x - s->joy_lost_x, y - s->joy_lost_y) <= DM_TC_JOY_GRAB_K * s->layout.joy_radius))) {
        /* 浮动底盘：落在起摇杆区里就起摇杆，底盘圆心 = 落点（贴边时夹回，见布局处），方向与幅度按「手指相对落点」算，按住后拖到多远都照样识别，
           底盘不跟手走；落在区外照旧是点地。这一下若很快原地松开，仍兼作一次点击（dm_tc_end_at）：区里的怪、掉落物、NPC 要点得到。
           摇杆那根手指刚被系统取消（通知横幅、系统手势；dm_tc_cancel_finger 记下时刻与位置），DM_TC_JOY_RESUME_S 内在原处附近重新落下的手指接着当摇杆，
           哪怕落点已拖出起摇杆区。只开着讲话文字框 / 对话菜单时不起摇杆（joy_block）：那一下只该关框、人不走（落指位移为零本来也不出方向，但手指稍一滑就会走，照旧整根挡掉）
           Floating base: a touch inside the stick-start zone starts the stick with the base centre = the touch point (clamped near the edges, see the layout), direction and magnitude are computed from "finger relative to the touch point", and once held, dragging any distance is still recognised;
                      the base does not follow the finger; a touch outside the zone is a ground tap as before. If this touch is released quickly in place it still doubles as a tap (dm_tc_end_at): monsters, dropped items and NPCs inside the zone must remain tappable.
                      When the stick finger has just been cancelled by the system (notification banner, system gesture; dm_tc_cancel_finger records the time and position), a finger touching down again near the same place within DM_TC_JOY_RESUME_S continues as the stick,
                      even if the touch point has been dragged out of the stick-start zone. The stick does not start while only a speech text box / dialogue menu is open (joy_block): that touch should only close the box without moving the character (zero displacement on touch-down gives no direction anyway, but a slight slide would walk, so the whole finger is still blocked) */
        int resume = s->joy_lost_s > 0 && now_s - s->joy_lost_s <= DM_TC_JOY_RESUME_S && hypotf(x - s->joy_lost_x, y - s->joy_lost_y) <= DM_TC_JOY_GRAB_K * s->layout.joy_radius;
        s->joy_lost_s = 0;
        f->target = DM_TC_JOYSTICK;
        s->joy_slot = slot;
        /* 接续的沿用取消前的圆心，不按新落点重定：触摸被系统反复打断又重来的机器上，每重来一次底盘就跳到手指当前位置，看着像底盘追着手指跑 | a continuation keeps the centre from before the cancel instead of re-centring on the new touch point: on devices where touches are repeatedly interrupted by the system and restarted, the base would jump to the finger's current position on every restart and look as if it were chasing the finger */
        s->joy_origin_x = resume ? s->joy_lost_ox : fmaxf(x, s->layout.joy_min_x);
        s->joy_origin_y = resume ? s->joy_lost_oy : fminf(y, s->layout.joy_max_y);
        s->joy_touch_x = resume ? s->joy_lost_tx : x; s->joy_touch_y = resume ? s->joy_lost_ty : y;   /* 接续的沿用取消前的落点，同样不按新落点重定 | a continuation keeps the touch point from before the cancel, likewise not re-centred on the new touch point */
    }
    update(s);
}

void dm_tc_move_at(dm_tc_state *s, int slot, float x, float y, double now_s) {
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH || !s->finger[slot].active || !isfinite(x) || !isfinite(y)) return;
    s->last_time_s = now_s;
    dm_tc_finger *f = &s->finger[slot];
    f->x = x;
    f->y = y;
    if (pointer_finger(s, f)) {
        float distance = hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y);
        if (!f->pointer_dragged && distance > DM_TC_POINTER_MOVE_PT * scale_for(&s->layout) && (!s->mouse_click_mode || s->pointer_item == 1))
            pointer_drag_begin(s, f);   /* 鼠标点击模式：只有从物品区起手才是拖放；别处的滑动不发键。按住不动超过长按时长再拖也算拖放（真人「按住停一下再拖」，z4：0.5 秒后才动的拖放拿不起来） | Mouse click mode: only a gesture starting in the item area is a drag-and-drop; swipes elsewhere send no key. Holding still longer than the long-press duration and then dragging also counts as drag-and-drop (a real person "holds for a moment and then drags"; z4: a drag that started moving only after 0.5 seconds could not pick the item up) */
        /* 起手在模态框外 / 被上层盖住（类 6 = 这一下不点）也不起滚动 / 滑块手势：原先模态框开着时框外上下滑照样给底下发滚轮 | A gesture starting outside the modal box / covered by an upper layer (class 6 = this tap does not click) does not start a scroll / slider gesture either: previously, with a modal box open, swiping up and down outside it still sent wheel events to what was underneath */
        else if (s->mouse_click_mode && s->pointer_item != 1 && s->pointer_item != 3 && s->pointer_item != 6 && !f->ui_drag && !f->pointer_dragged && distance > DM_TC_POINTER_MOVE_PT * scale_for(&s->layout))
            f->ui_drag = fabsf(f->x - f->aim_origin_x) > fabsf(f->y - f->aim_origin_y) ? 2 : 1;   /* 界面连续手势：过阈值那一刻按方向定型（纵 = 滚动，横 = 滑块） | Continuous UI gesture: the type is fixed by direction at the moment the threshold is crossed (vertical = scroll, horizontal = slider) */
        update_pointer(s);
    } else if (!s->navigation_mode && f->target == DM_TC_NONE && s->cam_on > 0 && f->aim_origin_x >= s->layout.width * 0.5f) {
        /* 镜头实验（cam_on = 前端给的档）：右半屏空白处（不在键上、不起摇杆）拖动看四周。越肩只认上下 = 俯仰；第一人称上下 = 俯仰、左右 = 转身。
           手指往上 = 视线往上，往右 = 往右转。位移过点按阈值才定型，定型后松手不算点地。一屏高 = 90°、一屏宽 = 180°。
           这里只累计弧度（cam_pitch / cam_yaw），前端取走交给宿主（dm_cam_tilt / dm_cam_turn）
           Camera experiment (cam_on = the mode the front end gives): drag on the empty right half (not on a key, not starting the stick) to look around.
           Over-the-shoulder takes vertical only = pitch; first person vertical = pitch, horizontal = turn. Finger up = look up, right = turn right. Fixed only
           once the move crosses the tap threshold, and once fixed the release is not a ground tap. A screen height = 90 degrees, a screen width = 180 degrees.
           Only radians are summed here (cam_pitch / cam_yaw); the front end takes them and hands them to the host (dm_cam_tilt / dm_cam_turn) */
        float ax = fabsf(x - f->aim_origin_x), ay = fabsf(y - f->aim_origin_y), th = DM_TC_POINTER_MOVE_PT * scale_for(&s->layout);
        if (!f->cam_drag && (ay > th || (s->cam_on == 2 && ax > th)) && (s->cam_on == 2 || ay > ax)) { f->cam_drag = 1; f->cam_x = f->aim_origin_x; f->cam_y = f->aim_origin_y; }
        if (f->cam_drag && s->layout.height > 0 && s->layout.width > 0) {
            s->cam_pitch += (f->cam_y - y) / s->layout.height * 1.5707963f; f->cam_y = y;
            if (s->cam_on == 2) s->cam_yaw += (x - f->cam_x) / s->layout.width * 3.1415927f;
            f->cam_x = x; }
    } else if (f->target==DM_TC_SWAP) {
        if (dm_tc_hit(&s->layout,x,y)!=DM_TC_SWAP) f->aim_cancelled=1;
    } else if (!s->navigation_mode && skill_target(f->target)) {
        if (inside_map_cancel(s, f)) f->aim_cancelled = 1;
        if (!f->aim_cancelled) f->aim_move_sequence = ++s->aim_move_sequence;
    }
    update(s);
}

void dm_tc_end_at(dm_tc_state *s, int slot, double now_s) {
    if (slot < 0 || slot >= DM_TC_MAX_TOUCH) return;
    s->last_time_s = now_s;
    if(s->finger[slot].active && s->finger[slot].target==DM_TC_PORTAL) {
        s->finger[slot]=(dm_tc_finger){.target=DM_TC_NONE};
        for(int i=0;i<DM_TC_MAX_TOUCH;i++)if(s->finger[i].active && s->finger[i].target==DM_TC_PORTAL) {update(s);return;}
        dm_tc_cancel_portal(s);return;
    }
    dm_tc_finger done = s->finger[slot];
    if (done.target==DM_TC_SWAP) {
        s->finger[slot]=(dm_tc_finger){.target=DM_TC_NONE};
        if (!s->navigation_mode && !done.aim_cancelled && dm_tc_hit(&s->layout,done.x,done.y)==DM_TC_SWAP)
            s->skill_set=!s->skill_set;
        update(s);return;
    }
    if (s->navigation_mode && slot == s->pointer_slot) {
        float distance = hypotf(done.x-done.aim_origin_x, done.y-done.aim_origin_y);
        double elapsed = now_s - done.start_time_s;
        /* 商人货架上的货长按 = 原版的「按住 Y 大量购买」（提示条原文；宿主读数：约 1.3 秒）：背包里有没满的卷轴书 / 同类叠时买满，没有就和买 1 份一样；买多少、钱够不够都由游戏定，这里只替玩家按键 | Long press on goods on the vendor shelf = the original game's 「按住 Y 大量购买」(Hold Y to buy in bulk) (button legend text; host reading: about 1.3 seconds): when the inventory has a non-full scroll tome / stack of the same kind it buys until full, otherwise it is the same as buying 1; how many to buy and whether there is enough gold are decided by the game, and here we only press the key for the player */
        if (done.pointer_long && !done.pointer_dragged && !done.shop_drag && s->pointer_item == 1 && s->pointer_shop) pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, DM_BTN_Y, 1.3);
        else if (done.pointer_long && !done.pointer_dragged && !done.shop_drag && s->pointer_item == 1 && !s->pointer_lock) ++s->long_press_seq;   /* 长按在松手时才算（按住没动）：按住到点就算的话，「按住停一下再拖」的药水会先被放进腰带（mini 药拖腰带脚本里多出 2 次） | Long press counts only at release (held without moving): if it counted as soon as the hold time was reached, a potion that is "held for a moment and then dragged" would first be put into the belt (2 extra occurrences in the potion-drag-to-belt script on the second Mac) */
        if (done.locked_drag) { }   /* 单件槽占用时拖别的物品：不发任何键 | Dragging another item while the single-item slot is occupied: send no key */
        else if (done.shop_drag) { ++s->shop_drop_seq; }   /* 买不买由前端按落点定；这里不发任何键 | Whether to buy is decided by the front end from the drop point; no key is sent here */
        else if (done.pointer_dragged) {
            if (s->pointer_release_item == 1) pointer_pulse_start(s, done.x, done.y - DM_TC_POINTER_LIFT_PT * scale_for(&s->layout), 2);   /* 放在抬起点（物品显示的位置） | Place at the lifted point (where the item is displayed) */
            else if (s->pointer_release_item == 5) pointer_pulse_start(s, s->release_x, s->release_y, 2);                                    /* 宿主给的放下位置（装备槽 = 槽心） | Drop position given by the host (equipment slot = slot center) */
            else if (s->pointer_release_item == 4) pointer_pulse_start(s, done.x, done.y, 2);                                              /* 放在手指点（玩家对着手指放） | Place at the finger point (the player aims with the finger) */
            else if (s->pointer_release_item == 3) pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, DM_BTN_Y, 0.15);          /* 手指在面板外 = 丢弃：指针回原处、到位后按 Y | Finger outside the panel = discard: pointer returns to the original spot and presses Y once in place */
            else if (s->pointer_release_item == 8) {   /* 松在单件任务槽上：按原版走法重做——先放回原格，再对它按住 Y（原版的「放进槽」）。只把物品拿在手上停在槽上方不算进槽：槽会显示它，但点确认不成交（本机 实测没有成交包） | Released on the single-item quest slot: redo it the original game's way -- first put it back in its original cell, then hold Y on it (the original game's "put into slot"). Merely holding the item and hovering over the slot does not count as entering the slot: the slot shows it, but tapping confirm does not complete the transaction (measured on the local machine: no transaction packet) */
                pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2); pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, DM_BTN_Y, 1.3); }
            else pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);                                                          /* 面板里的空白处：放回原处 | Blank area inside the panel: put it back where it was */
        } else if (!done.ui_drag && !done.pointer_long && elapsed < DM_TC_POINTER_TAP_S && distance <= DM_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {   /* 拖出去又拖回原处不算点按 | Dragging out and back to the original spot does not count as a tap */
            /* 单件任务槽面板开着时，手上那件就是「槽里那件」：点背包格仍是放回去，点别处按没拿着处理——确认 / 关闭按钮要能点，点面板外 = 关面板（不是把槽里那件丢到地上） | While the single-item quest slot panel is open, the held item is "the one in the slot": tapping an inventory cell still puts it back, tapping elsewhere is handled as if nothing were held -- the confirm / close buttons must be tappable, and tapping outside the panel = close the panel (not drop the slot's item on the ground) */
            if (s->pointer_lock) { s->pointer_x = done.aim_origin_x; s->pointer_y = done.aim_origin_y; }   /* 单件槽占用时点别的物品：只移光标出说明，单击 / 双击都不动它 | Tapping another item while the single-item slot is occupied: only move the cursor to show the tooltip; neither single nor double tap moves it */
            else if (s->holding && s->pointer_item != 0 && !(atomic_load(&g_imbue_open) && s->pointer_item != 1)) { if (s->pointer_item == 1) pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);
                              else if (s->pointer_item == 3) pointer_pulse_btn(s, s->pointer_x, s->pointer_y, 2, DM_BTN_Y, 0.15); }   /* 手上还拿着物品（上次没放成）：点物品区 = 放下；点面板外 / 两侧黑网格 = 丢（Y）；点页签等别处不动。
                              点按钮（类 0）不在这里吞掉、照常发鼠标点击：原先连合成钮也吞，手上有东西时点合成什么都不发；合成钮由宿主先收手上那件再按，见 dm_cursor.c
                              Still holding an item (the last drop did not succeed): tap the item area = put down; tap outside the panel / the black grid on either side = drop (Y); tapping tabs or elsewhere does nothing.
                                                            Tapping a button (class 0) is not swallowed here and sends a mouse click as usual: previously even the transmute button was swallowed, and tapping transmute with something in hand sent nothing; for the transmute button the host first stows the held item and then presses, see dm_cursor.c */
            else if (s->pointer_item == 2) { ++s->tab_tap_seq; }
            else if (s->pointer_item == 6 || s->pointer_item == 11) { }   /* 压着被上层面板盖住的按钮：不点、不发键 | Pressing on a button covered by an upper panel: no click, no key */
            else if (s->pointer_item == 3) { if (!atomic_load(&g_dead_modal)) { s->close_left = 3; s->close_next_s = now_s; atomic_store(&g_close_stop, 0); } }   /* 面板外：不发鼠标点击（会落到游戏世界 = 点地走路），按 B 关面板——子菜单里一下 B 只退一级，所以按到面板关掉为止（至多 3 下，见 dm_tc_tick）；死亡弹窗开着时不动 | Outside the panel: do not send a mouse click (it would land in the game world = tap ground to walk); press B to close the panel -- in a submenu one B only goes back one level, so press until the panel is closed (at most 3 presses, see dm_tc_tick); do nothing while the death popup is open */
            else if (s->pointer_item) {   /* 物品区：单击只移光标（游戏自己出说明）；紧接着在同一处再点一下 = 按住 A / Y | Item area: a single tap only moves the cursor (the game shows the tooltip itself); another tap at the same spot right after = hold A / Y */
                float ox = done.aim_origin_x, oy = done.aim_origin_y;
                int dbl = now_s - s->last_tap_s < DM_TC_DOUBLE_TAP_S && hypotf(ox - s->last_tap_x, oy - s->last_tap_y) <= 2 * DM_TC_POINTER_MOVE_PT * scale_for(&s->layout);
                if (dbl) { if (atomic_load(&g_hover_unid) && !s->pointer_stack && !s->pointer_shop) ++s->ident_seq;   /* 未鉴定物品：双击 = 免费鉴定（各页都先判；商人货架上的货不算我方物品，宿主也不发布） | Unidentified item: double tap = free identify (checked first on every page; goods on the vendor shelf are not our items, and the host does not publish them either) */
                    else if (s->pointer_stack) ++s->stack_take_seq;   /* 成叠格：取 1 件（原版短按 Y；这里不按键，按住 0.6 秒会变成「移动 3 个」） | Stacked cell: take 1 (short press Y in the original game; no key is pressed here, since holding for 0.6 seconds would become "move 3") */
                    else if (atomic_load(&g_imbue_open)) pointer_pulse_btn(s, ox, oy, 2, DM_BTN_Y, 1.3);   /* 单件任务槽面板：原版对背包物品按住 Y = 放进槽（宿主读数：约 1.3 秒）；不能当「使用」——否则在古墓底座前双击法杖会被装到手上 | Single-item quest slot panel: in the original game holding Y on an inventory item = put it into the slot (host reading: about 1.3 seconds); it must not be treated as "use" -- otherwise double-tapping the staff in front of the tomb pedestal would equip it in hand */
                    else if (atomic_load(&g_bank_open) && !atomic_load(&g_tap_box) && !atomic_load(&g_vault_open)) {
                        /* 背包里的物品：先发原版的 X（移动到堆叠栏）。能堆叠的（符文 / 宝石 / 材料）游戏收下、进堆叠格；不能堆叠的游戏对 X 不做任何事，0.4 秒后它还在光标下就退回按住 Y。
                           不直接按住 Y：那是「到仓库当前这一页」，在符文 / 宝石页且背包带着魔盒时会进页里嵌的魔盒格。仓库一侧的物品不走这里，仍是按住 Y 取回
                           Item in the inventory: first send the original game's X (move to the stacking tab). Stackable ones (runes / gems / materials) are accepted by the game and go into a stack cell; for non-stackable ones the game does nothing on X, and if it is still under the cursor after 0.4 seconds, fall back to hold Y.
                                                      Do not hold Y directly: that means "to the stash's current page", and on the rune / gem page with a cube in the inventory it would go into the cube cell embedded in the page. Items on the stash side do not go through here; they are still retrieved by holding Y */
                        if (atomic_load(&g_cur_stack)) { pointer_pulse_btn(s, ox, oy, 2, DM_BTN_X, 0.15); s->stash_x_check_s = now_s + 0.4; s->stash_x_x = ox; s->stash_x_y = oy; s->stash_x_id = atomic_load(&g_cur_stack); }
                        else pointer_pulse_btn(s, ox, oy, 2, DM_BTN_Y, DM_TC_HOLD_USE_S); }
                    else if (atomic_load(&g_cube_open) && !atomic_load(&g_bank_open)) pointer_pulse_btn(s, ox, oy, 2, DM_BTN_Y, DM_TC_HOLD_USE_S);   /* 方块页（不是箱子里嵌的那格）：原版按住 Y 一步送进 / 送出方块；与箱子页双击同一条规则 | Cube page (not the cell embedded in the stash): in the original game holding Y sends the item into / out of the cube in one step; same rule as double tap on the stash page */
                    else ++s->use_seq;   /* 开着箱子：按住 Y = 移到另一侧（魔盒除外：原版开箱时按住 A 仍是打开魔盒，读码）；否则交给宿主当场使用（原先按住 A 0.6 秒，约 0.4 秒后才生效） | Stash open: hold Y = move to the other side (except the cube: in the original game holding A with the stash open still opens the cube, per the code); otherwise hand it to the host to use on the spot (it used to hold A for 0.6 seconds and took effect only after about 0.4 seconds) */
                    s->last_tap_s = -1e9; }
                else {   /* 单击：移光标。隔了一会儿（不算双击）又点同一处 = 与另一件已装备的比较（原版按 ZL 循环；说明框文案「再点一次与另一件比较」） | Single tap: move the cursor. Tapping the same spot again after a while (not a double tap) = compare with the other equipped item (the original game cycles with ZL; tooltip text 「再点一次与另一件比较」(Tap again to compare with the other one)) */
                    int again = now_s - s->last_tap_s < 8.0 && hypotf(ox - s->last_tap_x, oy - s->last_tap_y) <= 2 * DM_TC_POINTER_MOVE_PT * scale_for(&s->layout);
                    if (again && !atomic_load(&g_bank_open) && !atomic_load(&g_vendor_open)) pointer_pulse_btn(s, ox, oy, 2, DM_BTN_ZL, 0);   /* 开着箱子 / 商人页时 ZL 是切页签，不发（货上点第二下把货架翻到了下一页） | With the stash / vendor page open ZL switches tabs, so do not send it (a second tap on goods flipped the shelf to the next page) */
                    else if (!s->pulse_button) { s->pointer_x = ox; s->pointer_y = oy; }
                    ++s->item_tap_seq;                     s->last_tap_s = now_s; s->last_tap_x = ox; s->last_tap_y = oy; }
            } else if (s->tap_button) {
                s->pointer_valid = 1; pointer_pulse_btn(s, s->tap_x, s->tap_y, 2, s->tap_button, 0);   /* 前端改写：光标到点 + 按键（技能绑定里点技能） | Front-end rewrite: cursor to the point + key press (tapping a skill in skill binding) */
            } else if (s->tap_move) {
                s->pointer_valid = 1; s->pointer_x = s->tap_x; s->pointer_y = s->tap_y; ++s->tap_move_seq;
            } else if (s->mouse_click_mode) {
                s->mouse_click_x = done.aim_origin_x; s->mouse_click_y = done.aim_origin_y; ++s->mouse_click_seq;
            } else pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);
        }
        s->pointer_item = 0; s->pointer_release_item = 1; s->pointer_shop = 0; s->pointer_stack = 0; s->pointer_lock = 0;
        s->pointer_slot = DM_TC_NONE;
        s->finger[slot] = (dm_tc_finger){.target=DM_TC_NONE};
        update(s);
        return;
    }
    /* 技能钮：按住 = 持续施放、拖动定方向、松手键起，不再是「按住瞄准、松手放一下」。
       唯一要补的：点得太短，键还没来得及按下手就松了（不到点按 / 拖动的判定时间，或第二组里 ZL 还没领先够）——补成一个带方向的脉冲；
       第二组里 ZL 还没领先够时，这个脉冲延到够了再发（dm_tc_tick）。
       Skill button: hold = continuous cast, drag sets the direction, release lifts the key; no longer "hold to aim, release to cast once".
              The only thing to make up: the tap was too short and the finger lifted before the key could go down (shorter than the tap / drag decision time, or in the second set ZL has not led long enough) -- make it up as one pulse with direction;
              in the second set, when ZL has not led long enough, this pulse is delayed until it has (dm_tc_tick). */
    if (done.active && done.target >= DM_TC_SKILL1 && done.target <= DM_TC_SKILL4 && !done.aim_cancelled && !s->navigation_mode
        && (!done.key_down || now_s - done.key_down_s < DM_TC_KEY_MIN_S || (s->skill_set && now_s - done.start_time_s < DM_TC_ZL_LEAD_S))) {   /* 键没按下过，或按下不到 2 帧（游戏可能漏读）：补成脉冲 | The key was never pressed, or was pressed for less than 2 frames (the game may miss it): make it up as a pulse */
        float ux = 0, uy = 0; int has = aim_dir(s, &done, &ux, &uy);
        s->pulse_rx = has ? (int32_t)lroundf(ux*DM_STICK_MAX) : 0; s->pulse_ry = has ? (int32_t)lroundf(-uy*DM_STICK_MAX) : 0;
        if (s->skill_set && now_s - done.start_time_s < DM_TC_ZL_LEAD_S) { s->defer_button = s->layout.target[done.target].button; s->defer_at_s = done.start_time_s + DM_TC_ZL_LEAD_S; }
        else { s->pulse_button = s->layout.target[done.target].button; ++s->pulse_epoch; }
    }
    if (done.active && !s->navigation_mode && done.target >= DM_TC_BELT1 && done.target <= DM_TC_BELT4) {
        /* 腰带格松手这一刻判：多排时上移过阈值 = 展开，下移过阈值 = 收起；几乎没动 = 点了一下，这一刻就喝；横着滑走了什么都不做 | decided when a belt cell is released: with multiple rows, moved up past the threshold = expand, down past the threshold = collapse; barely moved = a tap, drink right then; slid away sideways = do nothing */
        float th = DM_TC_BELT_SWIPE_PT * scale_for(&s->layout), up = done.aim_origin_y - done.y, dx = done.x - done.aim_origin_x;
        if (s->belt_rows > 1 && up > th) s->belt_request = 1;
        else if (s->belt_rows > 1 && up < -th) s->belt_request = 2;
        else if (fabsf(dx) <= th && fabsf(up) <= th) belt_fire(s, 0x10 | (done.target - DM_TC_BELT1));
    }
    if (done.active && !s->navigation_mode && !done.cam_drag && (done.target == DM_TC_NONE || done.target == DM_TC_JOYSTICK) && now_s - done.start_time_s < DM_TC_POINTER_TAP_S &&   /* 起摇杆区里很快原地松开的那一下兼作点击 | a quick release in place inside the stick-start zone doubles as a tap */
        hypotf(done.x - done.aim_origin_x, done.y - done.aim_origin_y) <= DM_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {
        s->world_tap_x = done.aim_origin_x; s->world_tap_y = done.aim_origin_y; ++s->world_tap_seq;   /* 点游戏世界 | Tap the game world */
    }
    s->finger[slot] = (dm_tc_finger){.target=DM_TC_NONE};
    if (slot == s->joy_slot) {
        s->joy_slot = DM_TC_NONE;
        s->joy_origin_x = s->joy_origin_y = 0;
    }
    update(s);
}

void dm_tc_begin(dm_tc_state *s, int slot, float x, float y) { dm_tc_begin_at(s, slot, x, y, s->last_time_s); }
void dm_tc_move(dm_tc_state *s, int slot, float x, float y) { dm_tc_move_at(s, slot, x, y, s->last_time_s); }
void dm_tc_end(dm_tc_state *s, int slot) { dm_tc_end_at(s, slot, s->last_time_s); }

void dm_tc_pointer_press(dm_tc_state *s, float x, float y, uint64_t button) {
    if (s->pointer_slot >= 0 && s->pointer_slot < DM_TC_MAX_TOUCH) s->finger[s->pointer_slot].pointer_long = 1;
    s->pointer_valid = 1; pointer_pulse_btn(s, x, y, 2, button, 0); update(s);
}
void dm_tc_tick(dm_tc_state *s, double now_s) {
    s->last_time_s = now_s;
    /* 补发不许盖掉在发的脉冲（腰带喝药等）：在发就等；那个脉冲收回时（dm_tc_pulse_end）把补发时刻推到 ZL 再领先满 DM_TC_ZL_LEAD_S 之后（原先到点直接覆盖，腰带那下没了） | The make-up send must not overwrite a pulse being sent (belt potion, etc.): wait while one is being sent; when that pulse is released (dm_tc_pulse_end), push the make-up time to after ZL has again led for a full DM_TC_ZL_LEAD_S (it used to overwrite directly when due, and the belt press was lost) */
    if (s->belt_gap_cell && now_s >= s->belt_gap_at && !s->pulse_button) { uint64_t btn = s->layout.target[DM_TC_BELT1 + (s->belt_gap_cell & 0xf)].button; s->belt_gap_cell = 0;
        if (btn && !s->navigation_mode) { s->pulse_rx = s->pulse_ry = 0; s->pulse_button = btn; ++s->pulse_epoch; } update(s); }
    if (s->belt_q && !s->pulse_button && !s->defer_button && !s->belt_gap_cell) { int q = s->belt_q; s->belt_q = 0; if (!s->navigation_mode) belt_fire(s, q); }
    if (s->defer_button && !s->pulse_button && now_s >= s->defer_at_s) { s->pulse_button = s->defer_button; s->defer_button = 0; ++s->pulse_epoch; update(s); }
    else if (!s->navigation_mode) {   /* 主钮 / 技能钮上有手指：判定时间、ZL 领先这两个时刻要按时刷新键值 | A finger is on the main button / a skill button: the key values must be refreshed on time at two moments, the decision time and the ZL lead */
        for (int i = 0; i < DM_TC_MAX_TOUCH; ++i) if (s->finger[i].active && (s->finger[i].target >= DM_TC_MAIN && s->finger[i].target <= DM_TC_SKILL4)) { update(s); break; }
    }
    if (s->stash_x_check_s > 0 && now_s >= s->stash_x_check_s) {   /* 发过 X 的那件还在光标下（不能堆叠 / 这个存档没有堆叠栏 / 那一叠满了）：退回按住 Y，移到仓库当前页。核的是同一个物品 id，X 已经把它移走就什么都不做 | The item that X was sent for is still under the cursor (not stackable / this save has no stacking tab / that stack is full): fall back to hold Y, moving it to the stash's current page. The check is on the same item id; if X has already moved it away, do nothing */
        s->stash_x_check_s = 0;
        if (atomic_load(&g_bank_open) && atomic_load(&g_cur_stack) == s->stash_x_id && !s->holding && !s->pulse_button) { pointer_pulse_btn(s, s->stash_x_x, s->stash_x_y, 2, DM_BTN_Y, DM_TC_HOLD_USE_S); update(s); }
    }
    if (s->close_left > 0 && atomic_exchange(&g_close_stop, 0)) s->close_left = 0;   /* 返回键引出了确认框（设置页有未应用的改动）：停手，让玩家自己答——再按一下就成了替他选「放弃」（改动丢失） | The back button brought up a confirmation box (the settings page has unapplied changes): stop and let the player answer -- one more press would choose 「放弃」(Discard) on their behalf (changes lost) */
    if (s->close_left > 0) {   /* 面板外点按 = 关到底：面板还开着、上一下 B 已收回，就隔 0.3 秒再按一下 | Tap outside the panel = close all the way: if the panel is still open and the previous B has been released, press once more after 0.3 seconds */
        if (!s->navigation_mode) s->close_left = 0;
        else if (!s->pulse_button && now_s >= s->close_next_s) { s->pulse_button = DM_BTN_B; s->pulse_hold_s = 0; s->pulse_rx = s->pulse_ry = 0; s->pointer_pulse_active = 2; ++s->pulse_epoch; --s->close_left; s->close_next_s = now_s + 0.3; update(s); }
    }
    if (s->pointer_slot < 0 || s->pointer_slot >= DM_TC_MAX_TOUCH) return;
    dm_tc_finger *f = &s->finger[s->pointer_slot];
    if (!f->active || f->pointer_dragged || f->pointer_long) return;
    if (s->mouse_click_mode && s->pointer_item != 1) return;
    if (now_s - f->start_time_s >= DM_TC_POINTER_HOLD_S &&
        hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y) <= DM_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {
        f->pointer_long = 1;
        s->pointer_x = f->aim_origin_x;
        s->pointer_y = f->aim_origin_y;
        update(s);
    }
}

int dm_tc_navigation(dm_tc_scene *s, const char *cat, const char *act, const char *arg) {
    static const char *names[]={"PlayerInventoryExpansionLayout","SkillsTreePanel","PauseLayout","PauseLayoutGarden","CharacterStatsPanel","QuestLogPanelExpansion","BankExpansionLayout","HoradricCubeLayout","WaypointsPanelExpansion","NpcDialogPanel","NpcTradePanel","ButtonBindingModal","SkillSelect","YouDiedModal"};
    if(!strcmp(cat,"FrontEndNavigation")&&!strcmp(act,"ToFrontEnd")) { s->game=0;s->panels=0; }
    if(!strcmp(cat,"PanelManager")) {
        if(!strcmp(arg,"LoadScreenPanel")&&!strcmp(act,"OpenPanel"))s->game=0;
        if(!strcmp(arg,"LoadScreenPanel")&&!strcmp(act,"UnloadPanel"))s->game=1;
        if(!strcmp(act,"UnloadAll"))s->panels=0;
        { size_t al=strlen(arg); if((!strcmp(act,"OpenPanel")||!strcmp(act,"OpenExclusivePanel"))&&al>5&&!strcmp(arg+al-5,"Modal"))atomic_store(&g_close_stop,1); }
        for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++)if(!strcmp(arg,names[i])) {
            if(!strcmp(act,"OpenPanel")||!strcmp(act,"OpenExclusivePanel"))s->panels|=1ull<<i;
            if(!strcmp(act,"ClosePanel")||!strcmp(act,"UnloadPanel"))s->panels&=~(1ull<<i);
        }
    }
    return !s->game||s->panels!=0;
}
/* 死亡弹窗开着：也算导航态（触点走界面点击，能点到「继续」），但五个手柄圆钮不给——死后点技能钮位置会触发复活，容易误触 | Death popup open: also counts as navigation state (touch points go through UI clicks, so 「继续」(Continue) can be tapped), but the five controller round buttons are not offered -- after death a tap at a skill button position triggers resurrection, which is easy to hit by mistake */
int dm_tc_scene_dead(void) { return atomic_load(&g_dead_modal); }
int dm_tc_scene_dialog_only(const dm_tc_scene *s) { return s->game && s->panels == (1ull << 9); }   /* 面板表第 9 项 = NpcDialogPanel（见 dm_tc_navigation 的 names） | Entry 9 of the panel table = NpcDialogPanel (see names in dm_tc_navigation) */

