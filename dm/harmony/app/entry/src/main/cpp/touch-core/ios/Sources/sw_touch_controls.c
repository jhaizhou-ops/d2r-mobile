#include "sw_touch_controls.h"
#include "../../host/sw_input.h"
#include <math.h>
#include <string.h>

#include <stdatomic.h>
#define SW_TC_POINTER_MOVE_PT 10.0f
#define SW_TC_POINTER_HOLD_S 0.5
#define SW_TC_POINTER_TAP_S 0.35

/* 控件缩放。手机（画面高 ≤ 440 点，最大的 iPhone 横屏就是 440）：min(1, 宽/667, 高/375)，与原来一字不差。
   比手机高的画面（iPad 上居中的 16:9 那块，13 寸是 1366×768 点）：按画面高度等比放大（高/440），否则摇杆与技能钮还是手机的点数大小，
   在大画面里只剩一小簇（Mac 上按 iPad 尺寸出图所见），而底栏与游戏界面都是按画面高度缩放的。
   这条放大只在前端显式打开时生效（sw_tc_set_tall_scale，front_ios.m 建视图时开）：Mac 宿主的触屏用例通路（sw/host/touch_mac.c）是把像素数
   （2868×1320）当点数传进来的，13 个用例的坐标按「缩放 = 1」写；不加这道开关，1320 > 440 会把它们的控件放大 3 倍、主钮挪位（skill_main 红）。 */
static int g_tall_scale;
void sw_tc_set_tall_scale(int on) { g_tall_scale = !!on; }
float sw_tc_scale_for_size(float width, float height) { float k = fminf(1.0f, fminf(width / 667.0f, height / 375.0f)); return g_tall_scale && k >= 1.0f && height > 440.0f ? height / 440.0f : k; }
static float scale_for(const sw_tc_layout *l) { return sw_tc_scale_for_size(l->width, l->height); }

/* 安全区（iPhone 横屏左右各有一条让给灵动岛 / 圆角的区域，17 Pro Max 实读各 62 点）：右侧技能键整组、左侧摇杆按最小位移平移到全部落在安全区内，
   组内相对排布不变。岛在哪一侧取决于玩家怎么拿，两侧都内缩。缺省 0（Mac 宿主、单测不设 = 原布局）。 */
static float g_safe_l, g_safe_r;
/* 屏幕底边归系统的那一条（点）：从这里起手往上划会被系统当成底边手势（iPad 上连着两次就拉出程序坞），而移动恰好是同一处反复起手往上推。
   这一条里起手不当摇杆、松手也不当点地；从上面起手的摇杆拖进来照常持有。只有 iPad 的前端会给非零值，手机恒为 0。 */
static float g_edge_b;
void sw_tc_set_edge_bottom(float bottom) { g_edge_b = isfinite(bottom) && bottom > 0 ? bottom : 0; }
void sw_tc_set_safe_insets(float left, float right) { g_safe_l = isfinite(left) && left > 0 ? left : 0; g_safe_r = isfinite(right) && right > 0 ? right : 0; }

void sw_tc_layout_make(sw_tc_layout *l, float width, float height) {
    memset(l, 0, sizeof(*l));
    l->width = isfinite(width) && width > 0 ? width : 0;
    l->height = isfinite(height) && height > 0 ? height : 0;
    float k = scale_for(l);
    float main_x = l->width - 56 * k, main_y = l->height - 82 * k;
    { float over = main_x + 40*k - (l->width - g_safe_r); if (over > 0) main_x -= over; }   /* 右侧一组里最靠右的是主技能键（半径 40） */
    l->target[SW_TC_MAIN] = (sw_tc_target){ main_x, main_y, 40*k, SW_BTN_A };
    /* 四分之一弧的单位向量：180°、150°、120°、90°。 */
    const float dx[4] = {-1, -0.8660254038f, -0.5f, 0};
    const float dy[4] = {0, -0.5f, -0.8660254038f, -1};
    const uint64_t skill[4] = {SW_BTN_B, SW_BTN_X, SW_BTN_Y, SW_BTN_R};
    for (int i = 0; i < 4; ++i)
        l->target[SW_TC_SKILL1+i] = (sw_tc_target){main_x+112*k*dx[i], main_y+112*k*dy[i], 26*k, skill[i]};
    /* 底栏 = 底部 UI v4 六格（NativeHUD/v4，游戏 HUD 按屏高缩放、以屏幕中线居中，所以按屏高换算）：
       效果图 2868×1320 里格心 x = 1090 + 120×格号、y = 1241.5、格宽 114；第 0 格难度，1–4 腰带四列，5 功能入口（PLUS）。
       回城不在底栏（离药水太近易误触）：改为技能弧最上面那颗（技能四）正上方的圆钮，同半径、间距 12，点下由宿主施放原版回城技能（sw_freetp.c），不出按键。 */
    /* 腰带四列对十字键：左 = 第 1 列、上 = 第 2 列、下 = 第 3 列、右 = 第 4 列（原版实测：按「右」第 4 列减、按「下」第 3 列减；首版把后两个写反了，真机才撞到） */
    const uint64_t bottom[5] = {SW_BTN_LEFT, SW_BTN_UP, SW_BTN_DOWN, SW_BTN_RIGHT, SW_BTN_PLUS};
    const int cell[5] = {1, 2, 3, 4, 5};
    float u = l->height / 1320.0f;
    for (int i = 0; i < 5; ++i)
        l->target[SW_TC_BELT1+i] = (sw_tc_target){l->width/2+(1090+120*cell[i]-1434)*u, 1241.5f*u, 57*u, bottom[i]};
    { const sw_tc_target *s4 = &l->target[SW_TC_SKILL4]; l->target[SW_TC_PORTAL] = (sw_tc_target){s4->x, s4->y-s4->r-12*k-26*k, 26*k, 0}; }
    /* 第 0 格难度与选择条：选择条 8 格同格宽、格距 120，中线对齐两球中线（效果图 1390），格心 y = 1051.5（七格之上、避开数值文字） */
    l->diff = (sw_tc_target){l->width/2+(1090-1434)*u, 1241.5f*u, 57*u, 0};
    for (int i = 0; i < 8; ++i) l->pick[i] = (sw_tc_target){l->width/2+(1390+(i-3.5f)*120-1434)*u, 1051.5f*u, 57*u, 0};
    l->target[SW_TC_MAP] = (sw_tc_target){l->width-30*k, 30*k, 22*k, SW_BTN_MINUS};
    /* 技能组切换：主技能左下、技能一下方，避开主技能与弧上四键 */
    /* 位置与大小：技能一正下方，半径 22（直径 44 点 ≈ 7.3 毫米）；上沿离技能一下沿 4 点、下沿离屏幕底 10 点（956×440） */
    /* 比手机窄的画面（16:9：iPad 留边 / 真全屏的游戏视图、667×375）底栏按屏高缩放、右端雕像翼尖（效果图 x 2075）会伸到换组键底下：
       键向右让到离翼尖 8 点以外，大小不变（量：1366×768 下原位重叠 21 点；956×440 下本来就隔着 28 点，不动） */
    { float sx = main_x-112*k, bar_r = l->width/2+(2015-1434)*u+8*k+22*k;   /* 右雕像翼尖：七格时 2075，六格石条右端收进 60 */ if (sx < bar_r) sx = bar_r;
      l->swap = (sw_tc_target){sx, main_y+52*k, 22*k, 0}; }
    /* 底栏外侧是血 / 蓝球；球不是输入目标。 */
    /* 左摇杆是浮动底盘：左下「起摇杆区」里手指落在哪，底盘圆心就落在哪，松手回静止位。
       静止位：圆心离安全区左界 90（底盘半径 60 + 30）、不小于 120，离屏幕底 115——只是没按时画在哪。
       起摇杆区：屏宽的左 40% × 屏高的下 55%（左与下到屏幕边：拇指压在刘海那一条上也算）；区里我们的圆钮 / 腰带格、原版界面上可点的东西各自优先。
       夹回：落点贴边时圆心挪回来，整个底盘留在安全区左界以右、底盘下沿离屏幕底不少于 25（不撞系统横条） */
    l->joy_radius = 60*k;
    l->joy_x = fmaxf(90*k, g_safe_l + 60*k) + 30*k;
    l->joy_y = l->height - 115*k;
    l->joy_zone_r = l->width*0.40f; l->joy_zone_t = l->height*0.45f;
    l->joy_min_x = g_safe_l + l->joy_radius; l->joy_max_y = l->height - 25*k - l->joy_radius;
}

int sw_tc_hit(const sw_tc_layout *l, float x, float y) {
    if (!isfinite(x) || !isfinite(y) || x < 0 || y < 0 || x > l->width || y > l->height)
        return SW_TC_NONE;
    for (int i = 0; i < SW_TC_COUNT; ++i) {
        const sw_tc_target *t = &l->target[i];
        float dx = x-t->x, dy = y-t->y;
        if (t->r > 0 && dx*dx+dy*dy <= t->r*t->r) return i;
    }
    float sx = x-l->swap.x, sy = y-l->swap.y;
    if (l->swap.r > 0 && sx*sx+sy*sy <= l->swap.r*l->swap.r) return SW_TC_SWAP;
    return SW_TC_NONE;
}

static int skill_target(int target) {
    return target >= SW_TC_MAIN && target <= SW_TC_SKILL4;
}

/* 拖动方向（单位向量，屏幕坐标）：拖动不过 10 点死区 = 没有方向 */
static int aim_dir(const sw_tc_state *s, const sw_tc_finger *f, float *ux, float *uy) {
    float dx = f->x-f->aim_origin_x, dy = f->y-f->aim_origin_y, d = hypotf(dx, dy);
    if (!(d > 10 * scale_for(&s->layout))) return 0;
    *ux = dx/d; *uy = dy/d; return 1;
}

static int inside_map_cancel(const sw_tc_state *s, const sw_tc_finger *f) {
    const sw_tc_target *map = &s->layout.target[SW_TC_MAP];
    return map->r > 0 && fabsf(f->x-map->x) <= map->r && fabsf(f->y-map->y) <= map->r;
}

static int g_iso_fix = 1;
void sw_tc_set_iso_fix(int on) { g_iso_fix = !!on; }
static void update_aim(sw_tc_state *s) {
    s->rx = s->ry = 0;
    s->aim_slot = SW_TC_NONE;
    s->map_cancel_active = 0;
    if (s->navigation_mode) return;
    uint64_t latest = 0;
    for (int i = 0; i < SW_TC_MAX_TOUCH; ++i) {
        const sw_tc_finger *f = &s->finger[i];
        if (!f->active || !skill_target(f->target)) continue;
        if (inside_map_cancel(s, f)) s->map_cancel_active = 1;
        if (!f->aim_cancelled && f->aim_move_sequence > latest) {
            latest = f->aim_move_sequence;
            s->aim_slot = i;
        }
    }
    if (s->pulse_button) { s->rx = s->pulse_rx; s->ry = s->pulse_ry; return; }
    float radius = 60 * scale_for(&s->layout);
    if (s->aim_slot == SW_TC_NONE || radius <= 0) return;
    const sw_tc_finger *f = &s->finger[s->aim_slot];
    if (f->target != SW_TC_MAIN && hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y) <= 10 * scale_for(&s->layout)) return;   /* 技能钮：拖动不过 10 点 = 点按，不给方向，原版自动选目标 */
    float dx = (f->x-f->aim_origin_x)/radius;
    float dy = (f->aim_origin_y-f->y)/radius;
    s->rx = (int32_t)lroundf(fmaxf(-1, fminf(1, dx))*SW_STICK_MAX);
    s->ry = (int32_t)lroundf(fmaxf(-1, fminf(1, dy))*SW_STICK_MAX);
}

static void update(sw_tc_state *s) {
    s->buttons = 0;
    for (int i = 0; i < SW_TC_MAX_TOUCH; ++i) {
        sw_tc_finger *f = &s->finger[i];
        if (f->active && !f->aim_cancelled && f->target >= 0 && f->target < SW_TC_COUNT) {
            uint64_t b = f->target==SW_TC_PORTAL && s->portal_pending ? SW_BTN_ZL : s->layout.target[f->target].button;
            /* 拖动定方向（核心手势）。技能钮：按下后先等 SW_TC_AIM_SETTLE_S 看是点按还是拖动——拖过死区就带着右轴方向立刻键下，
               没拖到点也键下（点按 = 原版自动选目标 / 朝向）；否则键先下、右轴后偏，按下那一发朝的是旧朝向。
               主钮：按下即键下，不等（它是最高频的键，点按不该多等；按下就拖的第一发是原生朝向，第二发起跟手）。
               按住期间方向再变由宿主的瞄准适配每拍重取，这里不做「松—按」；判定时间不为「按下稍停再拖」加长。 */
            if (!s->navigation_mode && f->target >= SW_TC_SKILL1 && f->target <= SW_TC_SKILL4) {
                float ux, uy;
                if (!f->key_down && !aim_dir(s, f, &ux, &uy) && s->last_time_s - f->start_time_s < SW_TC_AIM_SETTLE_S) b = 0;
                else if (!f->key_down) { f->key_down = 1; f->key_down_s = s->last_time_s; }
            }
            /* 多排腰带：腰带格上滑 = 展开、下滑 = 收起，所以键不在按下时发；一律等松手那一刻按位移判（sw_tc_end_at）。不设「按住到点就喝」的计时：
               手慢一点的上滑 / 下滑会先被它判成喝药（真机与 mini 各撞一次）。只有 1 排时照旧按下即喝 */
            /* 只有 1 排时也一律松手那一刻喝（和多排同一条路） */
            if (!s->navigation_mode && f->target >= SW_TC_BELT1 && f->target <= SW_TC_BELT4) b = 0;
            s->buttons |= b;
        }
    }
    s->buttons |= s->pulse_button;
    /* 第二套技能：ZL 是原版的换组修饰键。只在用技能的那一刻带上——手指按着主钮 / 技能钮、或技能脉冲在发（含等着发）时；
       其余时候不按：一直按着会让十字（+）、腰带、回城都变成「ZL + 键」的组合（Ⅱ 态下十字开不了面板）。
       ZL 先下、主钮的键晚 SW_TC_ZL_LEAD_S 再下（同一帧按下游戏只认主键 = 放成第一组的技能）。 */
    if (s->skill_set && !s->navigation_mode) {
        /* 在发的脉冲只有是技能键（主钮 / 技能钮补的那一下）才带 ZL：多排腰带松手补的喝药脉冲带上 ZL 就成了原版「ZL + 腰带键 = 喂佣兵」
           （本机 实跑：第二组下点腰带，0x26 包目标字节 = 1、佣兵生命 642 → 762 涨、玩家不变） */
        /* 等着补发的技能也要 ZL 领先——但只在没有别的脉冲在发时：腰带脉冲在发时带上 ZL 同样成了「喂佣兵」（单测探针读到 0x1100） */
        int use = s->defer_button != 0 && !s->pulse_button;
        for (int t = SW_TC_MAIN; t <= SW_TC_SKILL4 && !use; ++t) if (s->pulse_button && s->pulse_button == s->layout.target[t].button) use = 1;
        /* 手指按着第二组技能时发「给玩家喝」的腰带脉冲：ZL 还按着就成了原版「ZL + 腰带键 = 喂佣兵」。ZL 和按着的技能键要一起让开
           （只松 ZL 不松技能键 = 放成第一组的技能），而且要先让开、隔一段再按腰带键：同一拍里「松 ZL + 按腰带键」游戏仍按 ZL 还在算
           （真局实测：发出的还是喂佣兵包——宿主取手柄处对按下沿做锁存，这一拍采到的是 ZL 与腰带键都在）。所以分三段：
           等着发（belt_gap_cell）→ 脉冲在发 → 收回后 ZL 先回来、领先满 SW_TC_ZL_LEAD_S 技能键再回来（同一帧按下游戏只认主键） */
        int belt_pulse = s->belt_gap_cell != 0;
        for (int t = SW_TC_BELT1; t <= SW_TC_BELT4; ++t) if (s->pulse_button && s->pulse_button == s->layout.target[t].button) belt_pulse = 1;
        if (belt_pulse) s->belt_pulse_s = s->last_time_s;
        for (int i = 0; i < SW_TC_MAX_TOUCH; ++i) {
            const sw_tc_finger *f = &s->finger[i];
            if (!f->active || f->aim_cancelled || f->target < SW_TC_MAIN || f->target > SW_TC_SKILL4) continue;
            if (!belt_pulse) use = 1;
            if (belt_pulse || s->last_time_s - f->start_time_s < SW_TC_ZL_LEAD_S || (s->belt_pulse_s > 0 && s->last_time_s - s->belt_pulse_s < SW_TC_ZL_LEAD_S)) s->buttons &= ~s->layout.target[f->target].button;
        }
        if (use) s->buttons |= SW_BTN_ZL;
    }
    update_aim(s);
    s->lx = s->ly = 0;
    if (s->joy_slot < 0) return;
    sw_tc_finger *f = &s->finger[s->joy_slot];
    float dx = f->x-s->joy_origin_x, dy = s->joy_origin_y-f->y;
    /* 输入行程不跟大屏的视觉放大走：拇指不随屏幕变大。满偏 60 点、死区 6 点，手机与 iPad 一致
       （原先乘视觉倍数，12.9 寸 iPad 要推 105 点 ≈ 20 mm 才满偏，是手机的两倍）。底盘和摇杆头的大小仍按视觉倍数画。 */
    float in_k = fminf(1.0f, scale_for(&s->layout));
    float distance = hypotf(dx, dy), radius = 60 * in_k;
    float dead = 6 * in_k;
    if (distance <= dead || radius <= dead) return;
    float strength = fminf(1, (distance-dead)/(radius-dead));
    /* 斜向预修正：原版把摇杆做纯 45° 旋转成世界方向、不补偿等距画面的 2:1，人实际走的屏幕角 = atan(tan(摇杆角) / 2)（推 45° 走 26.6°）。
       手柄玩家看不见摇杆和画面的对应，触屏上手指画的就是屏幕方向，所以送值前把纵向放大 2 倍再缩回原幅度：屏幕角 = 手指角，推多深不变。
       只动游戏态的移动摇杆（导航态没有摇杆值；技能方向走右轴，不经这里） */
    float ex = dx/distance, ey = dy/distance;
    if (g_iso_fix) { float n = hypotf(ex, 2*ey); ex /= n; ey = 2*ey/n; }
    s->lx = (int32_t)lroundf(ex * strength * SW_STICK_MAX);
    s->ly = (int32_t)lroundf(ey * strength * SW_STICK_MAX);
}

static int pointer_finger(const sw_tc_state *s, const sw_tc_finger *f) {
    return s->navigation_mode && f->active && s->pointer_slot >= 0 &&
        &s->finger[s->pointer_slot] == f;
}

static void update_pointer(sw_tc_state *s) {
    if (!s->pointer_valid || s->pointer_slot < 0 || s->pointer_slot >= SW_TC_MAX_TOUCH) return;
    sw_tc_finger *f = &s->finger[s->pointer_slot];
    if (f->pointer_dragged && s->pointer_pickup_done && !f->shop_drag && !f->locked_drag) {
        s->pointer_x = f->x;
        s->pointer_y = f->y - SW_TC_POINTER_LIFT_PT * scale_for(&s->layout);
    }
}

/* 指针脉冲（kind 1 = 拖放的拿起，2 = 放下 / 点按）：指针先落到 (x,y) 再按 A。上一个脉冲还没收（游戏还没取走）时不动指针、只记下，
   等前端在上一个收回且游戏已看到一次「A 松开」之后调 sw_tc_pulse_resume 才开始——否则两下 A 连成一下、或拿起落在放下的位置 */
static void pointer_pulse_btn(sw_tc_state *s, float x, float y, int kind, uint64_t button, double hold_s) {
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
static void pointer_pulse_start(sw_tc_state *s, float x, float y, int kind) { pointer_pulse_btn(s, x, y, kind, SW_BTN_A, 0); }
void sw_tc_pointer_release(sw_tc_state *s, int kind) { s->pointer_release_item = kind == 1 || kind == 3 || kind == 4 || kind == 8 ? kind : 0; }
void sw_tc_pointer_release_at(sw_tc_state *s, float x, float y) { s->pointer_release_item = 5; s->release_x = x; s->release_y = y; }
void sw_tc_set_holding(sw_tc_state *s, int holding) { s->holding = !!holding; }
void sw_tc_pointer_item(sw_tc_state *s, int kind) {
    if (kind == 7) { s->pointer_shop = 1; kind = 1; }
    s->pointer_lock = kind == 10; if (kind == 10) kind = 1;
    s->pointer_stack = kind == 8; if (kind == 8) kind = 1;   /* 8 = 高级仓库的成叠格：照物品区，只有双击不同 */   /* 7 = 商人货架上的货：别的都照物品区，只有起拖不同 */
    s->pointer_item = (kind >= 1 && kind <= 3) || kind == 6 || kind == 11 ? kind : 0;   /* 11 = 这一下不点（底下是被盖住的按钮），但可以起滚动手势（设置页列表里的文字 / 空处） */
    if (s->mouse_click_mode && s->pointer_slot >= 0 && s->pointer_slot < SW_TC_MAX_TOUCH && s->finger[s->pointer_slot].active && !s->finger[s->pointer_slot].pointer_dragged) {
        if (kind == 1) { if (!s->pulse_button) { s->pointer_valid = 1; s->pointer_x = s->finger[s->pointer_slot].aim_origin_x; s->pointer_y = s->finger[s->pointer_slot].aim_origin_y; } }
        else s->pointer_valid = 0;   /* 不在物品区：放开手柄光标（前端据此清掉光标请求，不再推摇杆） */
    }
}   /* 1 物品区 2 页签（点按不发键也不发鼠标，前端另调宿主切页） */
static _Atomic int g_dead_modal;
static _Atomic int g_vendor_open;   /* 商人交易页开着：ZL 是切货架页签，「再点同一处 = 比较」不发 */
static _Atomic int g_close_stop;   /* 「面板外点按 = 按返回到面板关掉」途中弹出了确认框（名字以 Modal 结尾的面板打开）：后面的返回键不再按 */
static _Atomic int g_tap_box;   /* 最近一次单击的物品是魔盒（前端每拍从宿主同步） */
void sw_tc_set_tap_box(int on) { atomic_store(&g_tap_box, on != 0); }
static _Atomic unsigned g_cur_stack;
void sw_tc_set_cur_stack(unsigned id) { atomic_store(&g_cur_stack, id); }
static _Atomic unsigned g_hover_unid;
void sw_tc_set_hover_unid(unsigned id) { atomic_store(&g_hover_unid, id); }
static _Atomic int g_imbue_open;   /* 单件任务槽面板开着（灌魔 / 打孔 / 个性化 / 古墓底座，同一个 ImbueItemsPanel） */
static _Atomic int g_cube_open;   /* 赫拉迪克方块页开着：双击 = 按住 Y（原版「移到另一侧」：背包 → 方块 / 方块 → 背包），不是「使用 / 装备」 */
static _Atomic int g_vault_open;   /* 储藏箱开在第六页「编年史」：双击背包物品 = 存进编年史仓库（走 use_seq，宿主 ui_use_pump 判页），不发原版「按住 Y」——那一页游戏当魔盒页 */
static _Atomic int g_bank_open;   /* 储藏箱开着（前端从宿主读游戏面板树的结果设进来，不靠开关消息）：双击 = 按住 Y（移到另一侧） */
void sw_tc_set_panel_state(int bits) { atomic_store(&g_bank_open, (bits & 1) != 0); atomic_store(&g_vendor_open, (bits & 2) != 0); atomic_store(&g_dead_modal, (bits & 4) != 0); atomic_store(&g_imbue_open, (bits & 16) != 0); atomic_store(&g_cube_open, (bits & 32) != 0); atomic_store(&g_vault_open, (bits & 64) != 0); }   /* 位同 sw_cursor.h 的 SW_PS_*；宿主读游戏状态得来 */

static void pointer_drag_begin(sw_tc_state *s, sw_tc_finger *f) {
    f->pointer_dragged = 1;
    if (s->pointer_lock) { f->locked_drag = 1; return; }   /* 单件槽占用时从别的物品上起拖：不拿起，这次拖动什么都不做 */
    if (s->pointer_shop || s->pointer_stack) { f->shop_drag = 1; s->pointer_x = 2; s->pointer_y = 2; update(s); return; }   /* 从货架上的货起拖：不拿起；光标挪到屏幕角上，让货上的说明框收掉（买的时候宿主再把光标放回这件货上） */
    s->pointer_pickup_done = 0;
    pointer_pulse_start(s, f->aim_origin_x, f->aim_origin_y, 1);
}

void sw_tc_cancel_all(sw_tc_state *s) {
    ++s->gesture_epoch; s->portal_pending=s->portal_phase=0; s->diff_open=0;
    memset(s->finger, 0, sizeof(s->finger));
    for (int i = 0; i < SW_TC_MAX_TOUCH; ++i) s->finger[i].target = SW_TC_NONE;
    s->joy_slot = SW_TC_NONE;
    s->joy_origin_x = s->joy_origin_y = 0;
    s->buttons = 0;
    s->lx = s->ly = s->rx = s->ry = 0;
    s->aim_slot = SW_TC_NONE;
    s->aim_move_sequence = 0;
    s->map_cancel_active = 0;
    s->pulse_button = 0; s->pulse_rx = s->pulse_ry = 0;
    s->belt_q = 0; s->belt_gap_cell = 0;   /* 排着的、等着发的喝药一起作废 */   /* 等着判单双击的腰带格、等着发的喂雇佣兵一起作废 */
    s->defer_button = 0;   /* 等着补发的那下技能也作废：原先取消后 0.2 秒照样放出来 */
    s->pointer_slot = SW_TC_NONE;
    s->pointer_valid = 0; s->pointer_x = s->pointer_y = 0;
    s->pointer_pickup_done = 0;
    s->pointer_pulse_active = s->pointer_pulse_pending = 0;
    s->pointer_pending_x = s->pointer_pending_y = 0;
}

int sw_tc_pulse_end(sw_tc_state *s, uint64_t epoch) {
    if (!s->pulse_button || s->pulse_epoch != epoch) return 0;
    int ended_pointer = s->pointer_pulse_active;
    if (s->defer_button && s->defer_at_s < s->last_time_s + SW_TC_ZL_LEAD_S) s->defer_at_s = s->last_time_s + SW_TC_ZL_LEAD_S;   /* 等着补发的技能：从这一刻起 ZL 才能领先（见 sw_tc_tick） */
    s->pulse_button = 0; s->pulse_rx = s->pulse_ry = 0; s->pulse_hold_s = 0;
    s->pointer_pulse_active = 0;
    if (ended_pointer == 1) {
        s->pointer_pickup_done = 1;
        update_pointer(s);
    }
    update(s);
    return 1;
}

int sw_tc_pulse_pending(const sw_tc_state *s) { return !s->pulse_button && s->pointer_pulse_pending; }

int sw_tc_pulse_resume(sw_tc_state *s) {
    if (s->pulse_button || !s->pointer_pulse_pending) return 0;
    int kind = s->pointer_pulse_pending;
    s->pointer_pulse_pending = 0;
    pointer_pulse_btn(s, s->pointer_pending_x, s->pointer_pending_y, kind, s->pointer_pending_button ? s->pointer_pending_button : SW_BTN_A, s->pointer_pending_hold);
    s->pointer_pending_button = 0; s->pointer_pending_hold = 0;
    update(s);
    return 1;
}

void sw_tc_init(sw_tc_state *s, float width, float height) {
    memset(s, 0, sizeof(*s));
    s->pointer_release_item = 1;
    sw_tc_layout_make(&s->layout, width, height);
    s->enabled_targets = (1u << SW_TC_COUNT) - 1;
    sw_tc_cancel_all(s);
}

void sw_tc_resize(sw_tc_state *s, float width, float height) {
    uint64_t portal = s->layout.target[SW_TC_PORTAL].button;
    sw_tc_cancel_all(s);
    sw_tc_layout_make(&s->layout, width, height);
    s->layout.target[SW_TC_PORTAL].button = portal;
}

void sw_tc_enable_targets(sw_tc_state *s, uint32_t mask) {
    mask &= (1u << SW_TC_COUNT) - 1;
    if (mask == s->enabled_targets) return;
    sw_tc_cancel_all(s);
    s->enabled_targets = mask;
}

void sw_tc_set_joy_block(sw_tc_state *s, int block) { s->joy_block = block != 0; }
void sw_tc_set_navigation_mode(sw_tc_state *s, int navigation_mode) {
    navigation_mode = !!navigation_mode;
    if (navigation_mode == s->navigation_mode) return;
    sw_tc_cancel_all(s);
    s->navigation_mode = navigation_mode;
}

/* 系统取消了这根手指（通知横幅、系统手势）：不当松手——不补脉冲、不点世界、不点界面；只把它按着的键、摇杆、光标放掉 */
void sw_tc_cancel_finger(sw_tc_state *s, int slot) {
    if (slot < 0 || slot >= SW_TC_MAX_TOUCH) return;
    ++s->gesture_epoch;
    if (s->finger[slot].active && s->finger[slot].target == SW_TC_PORTAL) { sw_tc_cancel_portal(s); }
    if (slot == s->joy_slot && s->finger[slot].active) { s->joy_lost_s = s->last_time_s; s->joy_lost_x = s->finger[slot].x; s->joy_lost_y = s->finger[slot].y; s->joy_lost_ox = s->joy_origin_x; s->joy_lost_oy = s->joy_origin_y; }   /* 系统取消了摇杆那根手指：记下，紧接着在附近重新落下的接着当摇杆 */
    s->finger[slot] = (sw_tc_finger){.target=SW_TC_NONE};
    if (slot == s->joy_slot) { s->joy_slot = SW_TC_NONE; s->joy_origin_x = s->joy_origin_y = 0; }
    if (slot == s->aim_slot) s->aim_slot = SW_TC_NONE;
    if (slot == s->pointer_slot) { s->pointer_item = 0; s->pointer_shop = 0; s->pointer_stack = 0; s->pointer_lock = 0; s->pointer_slot = SW_TC_NONE; s->pointer_valid = 0; }
    update(s);
}
void sw_tc_cancel_portal(sw_tc_state *s) {
    ++s->gesture_epoch; s->portal_pending=s->portal_phase=0;
    for (int i=0;i<SW_TC_MAX_TOUCH;++i)
        if(s->finger[i].active && s->finger[i].target==SW_TC_PORTAL)
            s->finger[i]=(sw_tc_finger){.target=SW_TC_NONE};
    update(s);
}
void sw_tc_set_portal_binding(sw_tc_state *s,uint64_t mask,uint64_t identity,uint64_t scene) {
    if(s->layout.target[SW_TC_PORTAL].button==mask && s->portal_binding_identity==identity && s->portal_scene_epoch==scene)return;
    sw_tc_cancel_portal(s);
    s->portal_binding_identity=identity;s->portal_scene_epoch=scene;
    s->layout.target[SW_TC_PORTAL].button=mask;update(s);
}
void sw_tc_set_portal_button(sw_tc_state *s,uint64_t mask) {
    sw_tc_set_portal_binding(s,mask,s->portal_binding_identity,s->portal_scene_epoch);
}
int sw_tc_portal_ack(sw_tc_state *s,uint64_t epoch,uint64_t identity,uint64_t scene) {
    if(!s->portal_pending || s->gesture_epoch!=epoch || s->portal_binding_identity!=identity || s->portal_scene_epoch!=scene)return 0;
    int active=0;for(int i=0;i<SW_TC_MAX_TOUCH;i++)active|=s->finger[i].active && s->finger[i].target==SW_TC_PORTAL;
    if(!active)return 0;
    s->portal_pending=0;s->portal_phase=2;update(s);return 1;
}

/* 腰带展开着、这一点不在腰带上（底排四格与展开的几排之外）：前端据此在自己的手势（小地图块、空位十字）之前先收腰带并吃掉这一下 */
int sw_tc_belt_outside(const sw_tc_state *s, float x, float y) {
    if (s->navigation_mode || !s->belt_open || s->belt_rows <= 1) return 0;
    int hit = sw_tc_hit(&s->layout, x, y); if (hit >= SW_TC_BELT1 && hit <= SW_TC_BELT4) return 0;
    float u = s->layout.height / 1320.0f;
    for (int r = 1; r < s->belt_rows && r < 4; ++r) for (int c = 0; c < 4; ++c) {
        const sw_tc_target *t = &s->layout.target[SW_TC_BELT1+c];
        if (fabsf(x - t->x) <= 60*u && fabsf(y - (t->y - (165.0f + 162.0f*(r-1))*u)) <= 53.75f*u) return 0;
    }
    return 1;
}
/* 腰带点一下 = 自己喝，点几下喝几瓶（原版手感，不判双击）。底排 = 补一个腰带键脉冲（不带 ZL，第二组下也不带）；上面几排手柄没有键 = belt_request 交宿主喝那一格。
   喂雇佣兵不在腰带上做手势：佣兵面板里的原版入口和「自动给佣兵喝药」设置照旧 */
static void belt_fire(sw_tc_state *s, int cell) {
    if (cell >= 0x100) { s->belt_request = cell; return; }
    uint64_t btn = s->layout.target[SW_TC_BELT1 + (cell & 0xf)].button; if (!btn) return;
    if (s->pulse_button || s->defer_button || s->belt_gap_cell) { s->belt_q = cell; return; }   /* 有脉冲在发 / 有补发在等：排着（sw_tc_tick 空了再发） */
    if (s->skill_set && !s->navigation_mode) for (int i = 0; i < SW_TC_MAX_TOUCH; ++i) {   /* 按着第二组技能：先松 ZL 和技能键，隔 SW_TC_ZL_LEAD_S 再按腰带键（见 update） */
        const sw_tc_finger *f = &s->finger[i]; if (!f->active || f->aim_cancelled || f->target < SW_TC_MAIN || f->target > SW_TC_SKILL4) continue;
        s->belt_gap_cell = 0x1000 | cell; s->belt_gap_at = s->last_time_s + SW_TC_ZL_LEAD_S; update(s); return; }
    s->pulse_rx = s->pulse_ry = 0; s->pulse_button = btn; ++s->pulse_epoch;
    update(s);
}
void sw_tc_begin_at(sw_tc_state *s, int slot, float x, float y, double now_s) {
    if (slot < 0 || slot >= SW_TC_MAX_TOUCH || s->finger[slot].active || !isfinite(x) || !isfinite(y)) return;
    s->last_time_s = now_s;
    sw_tc_finger *f = &s->finger[slot];
    if (!s->navigation_mode) {   /* 难度：点第 0 格开 / 收选择条；展开时点某格 = 选档并收起，点别处只收起、照常处理 */
        const sw_tc_target *d = &s->layout.diff;
        int on_diff = d->r > 0 && hypotf(x-d->x, y-d->y) <= d->r, pick = -1;
        for (int i = 0; s->diff_open && i < 8; ++i) if (hypotf(x-s->layout.pick[i].x, y-s->layout.pick[i].y) <= s->layout.pick[i].r) pick = i;
        if (on_diff || pick >= 0) {
            if (pick >= 0) s->diff_request = pick + 1;
            s->diff_open = on_diff ? !s->diff_open : 0;
            *f = (sw_tc_finger){.active=1, .target=SW_TC_DIFF, .x=x, .y=y}; update(s); return;
        }
        s->diff_open = 0;
    }
    int hit = sw_tc_hit(&s->layout, x, y);
    if (!s->navigation_mode && s->belt_open && s->belt_rows > 1) {   /* 腰带展开着：点上面几排 = 喝那一格；点腰带以外 = 只收起 */
        float u = s->layout.height / 1320.0f;   /* 展开的几排是游戏自己画的：列与底排对齐，排距 162、第一排比底排高 165（效果图单位；真机 2868×1320 实量，行距由 gen_hud_v4.py 的 bpy 定，改那边要一起改） */
        for (int r = 1; r < s->belt_rows && r < 4; ++r) for (int c = 0; c < 4; ++c) {
            const sw_tc_target *t = &s->layout.target[SW_TC_BELT1+c];
            if (fabsf(x - t->x) <= 60*u && fabsf(y - (t->y - (165.0f + 162.0f*(r-1))*u)) <= 53.75f*u) {
                belt_fire(s, 0x100 | r << 4 | c);   /* 按下就喝那一格 */
                *f = (sw_tc_finger){.active=1, .target=SW_TC_DIFF, .x=x, .y=y}; update(s); return;
            }
        }
        if (hit < SW_TC_BELT1 || hit > SW_TC_BELT4) {   /* 点别处：只收腰带，这一下不再做别的（不走路、不点世界、不按键） */
            s->belt_request = 2; *f = (sw_tc_finger){.active=1, .target=SW_TC_DIFF, .x=x, .y=y}; update(s); return;
        }
    }
    if (hit == SW_TC_SWAP) {
        if (s->navigation_mode) hit = SW_TC_NONE;
        else { s->skill_set = !s->skill_set; *f = (sw_tc_finger){.active=1, .target=SW_TC_SWAP, .x=x, .y=y}; update(s); return; }
    }
    int enabled = hit >= 0 && (s->enabled_targets & (1u << hit));
    /* 面板态：被禁用的目标（底栏腰带 / 功能键 / 回城、圆钮收起后的技能钮位置）不占手指——它们压着的地方可能正是面板的按钮
       （设置页「应用」在屏幕底部，落在禁用的底栏目标上，既不判类也不点击）。游戏态仍占着（防隐藏的回城触点变成摇杆） */
    if (s->navigation_mode && hit >= 0 && !enabled) hit = SW_TC_NONE;
    if(enabled && hit==SW_TC_PORTAL && s->layout.target[hit].button && s->portal_phase==0) {
        ++s->gesture_epoch;
        uint64_t mask=s->layout.target[hit].button;
        s->portal_pending=(mask&SW_BTN_ZL) && (mask&~SW_BTN_ZL);
        s->portal_phase=s->portal_pending?1:2;
    }
    *f = (sw_tc_finger){.active=1, .target=enabled ? hit : SW_TC_NONE, .x=x, .y=y,
                           .aim_origin_x=x, .aim_origin_y=y,
                           .start_time_s=now_s};
    /* 禁用圆仍阻止落入左摇杆区，防止隐藏回城触点变成移动。 */
    if (s->navigation_mode && hit == SW_TC_NONE && s->pointer_slot == SW_TC_NONE) {
        s->pointer_slot = slot;
        s->pointer_item = 0;
        /* 鼠标点击模式下，手柄光标只跟「落在物品区的手指」走（前端答出命中后调 sw_tc_pointer_item）：
           光标是靠推摇杆走的，在没有自由光标的页面（选项、暂停菜单、小站）推摇杆 = 改当前选项 / 挪焦点（点页签顺带改了「显示物品」） */
        if (!s->mouse_click_mode) { s->pointer_valid = 1; s->pointer_x = x; s->pointer_y = y; }
    } else if (!s->navigation_mode && hit == SW_TC_NONE && g_edge_b > 0 && x >= 0 && x < s->layout.width*0.43f && y > s->layout.height - g_edge_b) {
        f->target = SW_TC_EDGE;
    } else if (!s->navigation_mode && !s->joy_block && hit == SW_TC_NONE && s->joy_slot == SW_TC_NONE && x >= 0 && y <= s->layout.height &&
        ((x < s->layout.joy_zone_r && y > s->layout.joy_zone_t) ||
         (x < s->layout.width*0.43f && y > s->layout.height*0.25f && s->joy_lost_s > 0 && now_s - s->joy_lost_s <= SW_TC_JOY_RESUME_S && hypotf(x - s->joy_lost_x, y - s->joy_lost_y) <= SW_TC_JOY_GRAB_K * s->layout.joy_radius))) {
        /* 浮动底盘：落在起摇杆区里就起摇杆，底盘圆心 = 落点（贴边时夹回，见布局处），方向与幅度按「手指相对底盘圆心」算，按住后拖到多远都照样识别，
           底盘不跟手走；落在区外照旧是点地。这一下若很快原地松开，仍兼作一次点击（sw_tc_end_at）：区里的怪、掉落物、NPC 要点得到。
           摇杆那根手指刚被系统取消（通知横幅、系统手势；sw_tc_cancel_finger 记下时刻与位置），SW_TC_JOY_RESUME_S 内在原处附近重新落下的手指接着当摇杆，
           哪怕落点已拖出起摇杆区。只开着讲话文字框 / 对话菜单时不起摇杆（joy_block）：夹回会让落点 ≠ 圆心、一落下就有方向，那一下只该关框 */
        int resume = s->joy_lost_s > 0 && now_s - s->joy_lost_s <= SW_TC_JOY_RESUME_S && hypotf(x - s->joy_lost_x, y - s->joy_lost_y) <= SW_TC_JOY_GRAB_K * s->layout.joy_radius;
        s->joy_lost_s = 0;
        f->target = SW_TC_JOYSTICK;
        s->joy_slot = slot;
        /* 接续的沿用取消前的圆心，不按新落点重定：触摸被系统反复打断又重来的机器上，每重来一次底盘就跳到手指当前位置，看着像底盘追着手指跑 */
        s->joy_origin_x = resume ? s->joy_lost_ox : fmaxf(x, s->layout.joy_min_x);
        s->joy_origin_y = resume ? s->joy_lost_oy : fminf(y, s->layout.joy_max_y);
    }
    update(s);
}

void sw_tc_move_at(sw_tc_state *s, int slot, float x, float y, double now_s) {
    if (slot < 0 || slot >= SW_TC_MAX_TOUCH || !s->finger[slot].active || !isfinite(x) || !isfinite(y)) return;
    s->last_time_s = now_s;
    sw_tc_finger *f = &s->finger[slot];
    f->x = x;
    f->y = y;
    if (pointer_finger(s, f)) {
        float distance = hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y);
        if (!f->pointer_dragged && distance > SW_TC_POINTER_MOVE_PT * scale_for(&s->layout) && (!s->mouse_click_mode || s->pointer_item == 1))
            pointer_drag_begin(s, f);   /* 鼠标点击模式：只有从物品区起手才是拖放；别处的滑动不发键。按住不动超过长按时长再拖也算拖放（真人「按住停一下再拖」，z4：0.5 秒后才动的拖放拿不起来） */
        /* 起手在模态框外 / 被上层盖住（类 6 = 这一下不点）也不起滚动 / 滑块手势：原先模态框开着时框外上下滑照样给底下发滚轮 */
        else if (s->mouse_click_mode && s->pointer_item != 1 && s->pointer_item != 3 && s->pointer_item != 6 && !f->ui_drag && !f->pointer_dragged && distance > SW_TC_POINTER_MOVE_PT * scale_for(&s->layout))
            f->ui_drag = fabsf(f->x - f->aim_origin_x) > fabsf(f->y - f->aim_origin_y) ? 2 : 1;   /* 界面连续手势：过阈值那一刻按方向定型（纵 = 滚动，横 = 滑块） */
        update_pointer(s);
    } else if (!s->navigation_mode && skill_target(f->target)) {
        if (inside_map_cancel(s, f)) f->aim_cancelled = 1;
        if (!f->aim_cancelled) f->aim_move_sequence = ++s->aim_move_sequence;
    }
    update(s);
}

void sw_tc_end_at(sw_tc_state *s, int slot, double now_s) {
    if (slot < 0 || slot >= SW_TC_MAX_TOUCH) return;
    s->last_time_s = now_s;
    if(s->finger[slot].active && s->finger[slot].target==SW_TC_PORTAL) {
        s->finger[slot]=(sw_tc_finger){.target=SW_TC_NONE};
        for(int i=0;i<SW_TC_MAX_TOUCH;i++)if(s->finger[i].active && s->finger[i].target==SW_TC_PORTAL) {update(s);return;}
        sw_tc_cancel_portal(s);return;
    }
    sw_tc_finger done = s->finger[slot];
    if (s->navigation_mode && slot == s->pointer_slot) {
        float distance = hypotf(done.x-done.aim_origin_x, done.y-done.aim_origin_y);
        double elapsed = now_s - done.start_time_s;
        /* 商人货架上的货长按 = 原版的「按住 Y 大量购买」（提示条原文；宿主读数：约 1.3 秒）：背包里有没满的卷轴书 / 同类叠时买满，没有就和买 1 份一样；买多少、钱够不够都由游戏定，这里只替玩家按键 */
        if (done.pointer_long && !done.pointer_dragged && !done.shop_drag && s->pointer_item == 1 && s->pointer_shop) pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, SW_BTN_Y, 1.3);
        else if (done.pointer_long && !done.pointer_dragged && !done.shop_drag && s->pointer_item == 1 && !s->pointer_lock) ++s->long_press_seq;   /* 长按在松手时才算（按住没动）：按住到点就算的话，「按住停一下再拖」的药水会先被放进腰带（mini 药拖腰带脚本里多出 2 次） */
        if (done.locked_drag) { }   /* 单件槽占用时拖别的物品：不发任何键 */
        else if (done.shop_drag) { ++s->shop_drop_seq; }   /* 买不买由前端按落点定；这里不发任何键 */
        else if (done.pointer_dragged) {
            if (s->pointer_release_item == 1) pointer_pulse_start(s, done.x, done.y - SW_TC_POINTER_LIFT_PT * scale_for(&s->layout), 2);   /* 放在抬起点（物品显示的位置） */
            else if (s->pointer_release_item == 5) pointer_pulse_start(s, s->release_x, s->release_y, 2);                                    /* 宿主给的放下位置（装备槽 = 槽心） */
            else if (s->pointer_release_item == 4) pointer_pulse_start(s, done.x, done.y, 2);                                              /* 放在手指点（玩家对着手指放） */
            else if (s->pointer_release_item == 3) pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, SW_BTN_Y, 0.15);          /* 手指在面板外 = 丢弃：指针回原处、到位后按 Y */
            else if (s->pointer_release_item == 8) {   /* 松在单件任务槽上：按原版走法重做——先放回原格，再对它按住 Y（原版的「放进槽」）。只把物品拿在手上停在槽上方不算进槽：槽会显示它，但点确认不成交（本机 实测没有成交包） */
                pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2); pointer_pulse_btn(s, done.aim_origin_x, done.aim_origin_y, 2, SW_BTN_Y, 1.3); }
            else pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);                                                          /* 面板里的空白处：放回原处 */
        } else if (!done.ui_drag && !done.pointer_long && elapsed < SW_TC_POINTER_TAP_S && distance <= SW_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {   /* 拖出去又拖回原处不算点按 */
            /* 单件任务槽面板开着时，手上那件就是「槽里那件」：点背包格仍是放回去，点别处按没拿着处理——确认 / 关闭按钮要能点，点面板外 = 关面板（不是把槽里那件丢到地上） */
            if (s->pointer_lock) { s->pointer_x = done.aim_origin_x; s->pointer_y = done.aim_origin_y; }   /* 单件槽占用时点别的物品：只移光标出说明，单击 / 双击都不动它 */
            else if (s->holding && s->pointer_item != 0 && !(atomic_load(&g_imbue_open) && s->pointer_item != 1)) { if (s->pointer_item == 1) pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);
                              else if (s->pointer_item == 3) pointer_pulse_btn(s, s->pointer_x, s->pointer_y, 2, SW_BTN_Y, 0.15); }   /* 手上还拿着物品（上次没放成）：点物品区 = 放下；点面板外 / 两侧黑网格 = 丢（Y）；点页签等别处不动。
                              点按钮（类 0）不在这里吞掉、照常发鼠标点击：原先连合成钮也吞，手上有东西时点合成什么都不发；合成钮由宿主先收手上那件再按，见 sw_cursor.c */
            else if (s->pointer_item == 2) { ++s->tab_tap_seq; }
            else if (s->pointer_item == 6 || s->pointer_item == 11) { }   /* 压着被上层面板盖住的按钮：不点、不发键 */
            else if (s->pointer_item == 3) { if (!atomic_load(&g_dead_modal)) { s->close_left = 3; s->close_next_s = now_s; atomic_store(&g_close_stop, 0); } }   /* 面板外：不发鼠标点击（会落到游戏世界 = 点地走路），按 B 关面板——子菜单里一下 B 只退一级，所以按到面板关掉为止（至多 3 下，见 sw_tc_tick）；死亡弹窗开着时不动 */
            else if (s->pointer_item) {   /* 物品区：单击只移光标（游戏自己出说明）；紧接着在同一处再点一下 = 按住 A / Y */
                float ox = done.aim_origin_x, oy = done.aim_origin_y;
                int dbl = now_s - s->last_tap_s < SW_TC_DOUBLE_TAP_S && hypotf(ox - s->last_tap_x, oy - s->last_tap_y) <= 2 * SW_TC_POINTER_MOVE_PT * scale_for(&s->layout);
                if (dbl) { if (atomic_load(&g_hover_unid) && !s->pointer_stack && !s->pointer_shop) ++s->ident_seq;   /* 未鉴定物品：双击 = 免费鉴定（各页都先判；商人货架上的货不算我方物品，宿主也不发布） */
                    else if (s->pointer_stack) ++s->stack_take_seq;   /* 成叠格：取 1 件（原版短按 Y；这里不按键，按住 0.6 秒会变成「移动 3 个」） */
                    else if (atomic_load(&g_imbue_open)) pointer_pulse_btn(s, ox, oy, 2, SW_BTN_Y, 1.3);   /* 单件任务槽面板：原版对背包物品按住 Y = 放进槽（宿主读数：约 1.3 秒）；不能当「使用」——否则在古墓底座前双击法杖会被装到手上 */
                    else if (atomic_load(&g_bank_open) && !atomic_load(&g_tap_box) && !atomic_load(&g_vault_open)) {
                        /* 背包里的物品：先发原版的 X（移动到堆叠栏）。能堆叠的（符文 / 宝石 / 材料）游戏收下、进堆叠格；不能堆叠的游戏对 X 不做任何事，0.4 秒后它还在光标下就退回按住 Y。
                           不直接按住 Y：那是「到仓库当前这一页」，在符文 / 宝石页且背包带着魔盒时会进页里嵌的魔盒格。仓库一侧的物品不走这里，仍是按住 Y 取回 */
                        if (atomic_load(&g_cur_stack)) { pointer_pulse_btn(s, ox, oy, 2, SW_BTN_X, 0.15); s->stash_x_check_s = now_s + 0.4; s->stash_x_x = ox; s->stash_x_y = oy; s->stash_x_id = atomic_load(&g_cur_stack); }
                        else pointer_pulse_btn(s, ox, oy, 2, SW_BTN_Y, SW_TC_HOLD_USE_S); }
                    else if (atomic_load(&g_cube_open) && !atomic_load(&g_bank_open)) pointer_pulse_btn(s, ox, oy, 2, SW_BTN_Y, SW_TC_HOLD_USE_S);   /* 方块页（不是箱子里嵌的那格）：原版按住 Y 一步送进 / 送出方块；与箱子页双击同一条规则 */
                    else ++s->use_seq;   /* 开着箱子：按住 Y = 移到另一侧（魔盒除外：原版开箱时按住 A 仍是打开魔盒，读码）；否则交给宿主当场使用（原先按住 A 0.6 秒，约 0.4 秒后才生效） */
                    s->last_tap_s = -1e9; }
                else {   /* 单击：移光标。隔了一会儿（不算双击）又点同一处 = 与另一件已装备的比较（原版按 ZL 循环；说明框文案「再点一次与另一件比较」） */
                    int again = now_s - s->last_tap_s < 8.0 && hypotf(ox - s->last_tap_x, oy - s->last_tap_y) <= 2 * SW_TC_POINTER_MOVE_PT * scale_for(&s->layout);
                    if (again && !atomic_load(&g_bank_open) && !atomic_load(&g_vendor_open)) pointer_pulse_btn(s, ox, oy, 2, SW_BTN_ZL, 0);   /* 开着箱子 / 商人页时 ZL 是切页签，不发（货上点第二下把货架翻到了下一页） */
                    else if (!s->pulse_button) { s->pointer_x = ox; s->pointer_y = oy; }
                    ++s->item_tap_seq;                     s->last_tap_s = now_s; s->last_tap_x = ox; s->last_tap_y = oy; }
            } else if (s->tap_button) {
                s->pointer_valid = 1; pointer_pulse_btn(s, s->tap_x, s->tap_y, 2, s->tap_button, 0);   /* 前端改写：光标到点 + 按键（技能绑定里点技能） */
            } else if (s->tap_move) {
                s->pointer_valid = 1; s->pointer_x = s->tap_x; s->pointer_y = s->tap_y; ++s->tap_move_seq;
            } else if (s->mouse_click_mode) {
                s->mouse_click_x = done.aim_origin_x; s->mouse_click_y = done.aim_origin_y; ++s->mouse_click_seq;
            } else pointer_pulse_start(s, done.aim_origin_x, done.aim_origin_y, 2);
        }
        s->pointer_item = 0; s->pointer_release_item = 1; s->pointer_shop = 0; s->pointer_stack = 0; s->pointer_lock = 0;
        s->pointer_slot = SW_TC_NONE;
        s->finger[slot] = (sw_tc_finger){.target=SW_TC_NONE};
        update(s);
        return;
    }
    /* 技能钮：按住 = 持续施放、拖动定方向、松手键起，不再是「按住瞄准、松手放一下」。
       唯一要补的：点得太短，键还没来得及按下手就松了（不到点按 / 拖动的判定时间，或第二组里 ZL 还没领先够）——补成一个带方向的脉冲；
       第二组里 ZL 还没领先够时，这个脉冲延到够了再发（sw_tc_tick）。 */
    if (done.active && done.target >= SW_TC_SKILL1 && done.target <= SW_TC_SKILL4 && !done.aim_cancelled && !s->navigation_mode
        && (!done.key_down || now_s - done.key_down_s < SW_TC_KEY_MIN_S || (s->skill_set && now_s - done.start_time_s < SW_TC_ZL_LEAD_S))) {   /* 键没按下过，或按下不到 2 帧（游戏可能漏读）：补成脉冲 */
        float ux = 0, uy = 0; int has = aim_dir(s, &done, &ux, &uy);
        s->pulse_rx = has ? (int32_t)lroundf(ux*SW_STICK_MAX) : 0; s->pulse_ry = has ? (int32_t)lroundf(-uy*SW_STICK_MAX) : 0;
        if (s->skill_set && now_s - done.start_time_s < SW_TC_ZL_LEAD_S) { s->defer_button = s->layout.target[done.target].button; s->defer_at_s = done.start_time_s + SW_TC_ZL_LEAD_S; }
        else { s->pulse_button = s->layout.target[done.target].button; ++s->pulse_epoch; }
    }
    if (done.active && !s->navigation_mode && done.target >= SW_TC_BELT1 && done.target <= SW_TC_BELT4) {
        /* 腰带格松手这一刻判：多排时上移过阈值 = 展开，下移过阈值 = 收起；几乎没动 = 点了一下，这一刻就喝；横着滑走了什么都不做 */
        float th = SW_TC_BELT_SWIPE_PT * scale_for(&s->layout), up = done.aim_origin_y - done.y, dx = done.x - done.aim_origin_x;
        if (s->belt_rows > 1 && up > th) s->belt_request = 1;
        else if (s->belt_rows > 1 && up < -th) s->belt_request = 2;
        else if (fabsf(dx) <= th && fabsf(up) <= th) belt_fire(s, 0x10 | (done.target - SW_TC_BELT1));
    }
    if (done.active && !s->navigation_mode && (done.target == SW_TC_NONE || done.target == SW_TC_JOYSTICK) && now_s - done.start_time_s < SW_TC_POINTER_TAP_S &&   /* 起摇杆区里很快原地松开的那一下兼作点击 */
        hypotf(done.x - done.aim_origin_x, done.y - done.aim_origin_y) <= SW_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {
        s->world_tap_x = done.aim_origin_x; s->world_tap_y = done.aim_origin_y; ++s->world_tap_seq;   /* 点游戏世界 */
    }
    s->finger[slot] = (sw_tc_finger){.target=SW_TC_NONE};
    if (slot == s->joy_slot) {
        s->joy_slot = SW_TC_NONE;
        s->joy_origin_x = s->joy_origin_y = 0;
    }
    update(s);
}

void sw_tc_begin(sw_tc_state *s, int slot, float x, float y) { sw_tc_begin_at(s, slot, x, y, s->last_time_s); }
void sw_tc_move(sw_tc_state *s, int slot, float x, float y) { sw_tc_move_at(s, slot, x, y, s->last_time_s); }
void sw_tc_end(sw_tc_state *s, int slot) { sw_tc_end_at(s, slot, s->last_time_s); }

void sw_tc_pointer_press(sw_tc_state *s, float x, float y, uint64_t button) {
    if (s->pointer_slot >= 0 && s->pointer_slot < SW_TC_MAX_TOUCH) s->finger[s->pointer_slot].pointer_long = 1;
    s->pointer_valid = 1; pointer_pulse_btn(s, x, y, 2, button, 0); update(s);
}
void sw_tc_tick(sw_tc_state *s, double now_s) {
    s->last_time_s = now_s;
    /* 补发不许盖掉在发的脉冲（腰带喝药等）：在发就等；那个脉冲收回时（sw_tc_pulse_end）把补发时刻推到 ZL 再领先满 SW_TC_ZL_LEAD_S 之后（原先到点直接覆盖，腰带那下没了） */
    if (s->belt_gap_cell && now_s >= s->belt_gap_at && !s->pulse_button) { uint64_t btn = s->layout.target[SW_TC_BELT1 + (s->belt_gap_cell & 0xf)].button; s->belt_gap_cell = 0;
        if (btn && !s->navigation_mode) { s->pulse_rx = s->pulse_ry = 0; s->pulse_button = btn; ++s->pulse_epoch; } update(s); }
    if (s->belt_q && !s->pulse_button && !s->defer_button && !s->belt_gap_cell) { int q = s->belt_q; s->belt_q = 0; if (!s->navigation_mode) belt_fire(s, q); }
    if (s->defer_button && !s->pulse_button && now_s >= s->defer_at_s) { s->pulse_button = s->defer_button; s->defer_button = 0; ++s->pulse_epoch; update(s); }
    else if (!s->navigation_mode) {   /* 主钮 / 技能钮上有手指：判定时间、ZL 领先这两个时刻要按时刷新键值 */
        for (int i = 0; i < SW_TC_MAX_TOUCH; ++i) if (s->finger[i].active && (s->finger[i].target >= SW_TC_MAIN && s->finger[i].target <= SW_TC_SKILL4)) { update(s); break; }
    }
    if (s->stash_x_check_s > 0 && now_s >= s->stash_x_check_s) {   /* 发过 X 的那件还在光标下（不能堆叠 / 这个存档没有堆叠栏 / 那一叠满了）：退回按住 Y，移到仓库当前页。核的是同一个物品 id，X 已经把它移走就什么都不做 */
        s->stash_x_check_s = 0;
        if (atomic_load(&g_bank_open) && atomic_load(&g_cur_stack) == s->stash_x_id && !s->holding && !s->pulse_button) { pointer_pulse_btn(s, s->stash_x_x, s->stash_x_y, 2, SW_BTN_Y, SW_TC_HOLD_USE_S); update(s); }
    }
    if (s->close_left > 0 && atomic_exchange(&g_close_stop, 0)) s->close_left = 0;   /* 返回键引出了确认框（设置页有未应用的改动）：停手，让玩家自己答——再按一下就成了替他选「放弃」（改动丢失） */
    if (s->close_left > 0) {   /* 面板外点按 = 关到底：面板还开着、上一下 B 已收回，就隔 0.3 秒再按一下 */
        if (!s->navigation_mode) s->close_left = 0;
        else if (!s->pulse_button && now_s >= s->close_next_s) { s->pulse_button = SW_BTN_B; s->pulse_hold_s = 0; s->pulse_rx = s->pulse_ry = 0; s->pointer_pulse_active = 2; ++s->pulse_epoch; --s->close_left; s->close_next_s = now_s + 0.3; update(s); }
    }
    if (s->pointer_slot < 0 || s->pointer_slot >= SW_TC_MAX_TOUCH) return;
    sw_tc_finger *f = &s->finger[s->pointer_slot];
    if (!f->active || f->pointer_dragged || f->pointer_long) return;
    if (s->mouse_click_mode && s->pointer_item != 1) return;
    if (now_s - f->start_time_s >= SW_TC_POINTER_HOLD_S &&
        hypotf(f->x-f->aim_origin_x, f->y-f->aim_origin_y) <= SW_TC_POINTER_MOVE_PT * scale_for(&s->layout)) {
        f->pointer_long = 1;
        s->pointer_x = f->aim_origin_x;
        s->pointer_y = f->aim_origin_y;
        update(s);
    }
}

int sw_tc_navigation(sw_tc_scene *s, const char *cat, const char *act, const char *arg) {
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
/* 死亡弹窗开着：也算导航态（触点走界面点击，能点到「继续」），但五个手柄圆钮不给——死后点技能钮位置会触发复活，容易误触 */
int sw_tc_scene_dead(void) { return atomic_load(&g_dead_modal); }
int sw_tc_scene_dialog_only(const sw_tc_scene *s) { return s->game && s->panels == (1ull << 9); }   /* 面板表第 9 项 = NpcDialogPanel（见 sw_tc_navigation 的 names） */
