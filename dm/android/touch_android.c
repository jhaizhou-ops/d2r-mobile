#include "touch_android.h"
#include "../ios/Sources/dm_touch_controls.h"
#include "../host/dm_input.h"
#include "../host/dm_cursor.h"
#include "../host/dm_actions.h"
#include "../host/dm_hud.h"
#include "../host/dm_custom.h"
#include "../host/dm_world.h"
#include "../ios/Sources/dm_touch_native_art.h"
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

uint64_t now_ns(void);
void dm_uiscreen_set_panel(int panel);
void dm_tp_request(void);
/* 绑定观测桥未接入时不捏造技能绑定；基础键位仍走共用状态机。 | Do not fabricate skill bindings if the observation bridge is absent; basic keys still use the shared state machine. */
void dm_touch_game_set_scene(int game, uint64_t scene, uint64_t world) __attribute__((weak));

typedef struct { int phase, slot; float x, y; double time; } touch_event;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static dm_tc_state tc;
static dm_tc_scene scene;
/* 面板栈读数（宿主 dm_cursor.c dm_hud_stack_msg，iOS / Mac 同一份）：开着的非常驻面板数，只开着 NPC 对话菜单时 −1
   Panel stack reading (host dm_cursor.c dm_hud_stack_msg, shared with iOS / Mac): open non-resident panels, −1 when only the NPC dialog menu is open */
int dm_hud_stack_msg(const char *act, const char *arg);
static int hud_blocked;
static touch_event queue[256];
static unsigned head, tail;
static int ready, focused = 1, cancel, resizing, ui_drag;
static float width = 800, height = 375, safe_left, safe_right, safe_bottom;
static int tablet_layout, swap_above;
typedef struct { int kind; float x,y; double time; uint64_t key; } direct_gesture;
static direct_gesture direct[DM_TC_MAX_TOUCH];
static uint32_t selected_point;
static double stat_next, skill_add_at;
static unsigned map_world_seq;
static int map_world_pending;
static uint64_t epoch = 1, world = 1, pulse_epoch, pulse_sample, resume_sample;
static double pulse_hold_start;
static unsigned seen_world, seen_mouse, seen_tab, seen_use, seen_ident, seen_long, seen_item, seen_stack;
/* 前端页面（主菜单 / 选角 / 建角 / 读取画面，即不在局内），同 iOS front_ios.m frontendMode：不画控件，第一根手指直接当原生点击交给游戏界面
   （dm_ui_touch → 鼠标消息，dm_cursor.c），纵向拖过 10 点改成滚动。只在装了触屏覆盖层（DM_ROMFS_OVERLAY）时这样：覆盖层把选角页换成键鼠版
   （「开始」「创建新角色」靠点）；没有覆盖层时前端是原版手柄页，仍走导航键。
   Front-end pages (main menu / character select / creation / loading, i.e. not in a run), as iOS front_ios.m frontendMode: no controls are drawn, and
   the first finger goes straight to the game UI as a native click (dm_ui_touch → mouse messages, dm_cursor.c); a vertical drag past 10 points scrolls.
   Only with the touch overlay (DM_ROMFS_OVERLAY): it turns character select into the keyboard-and-mouse page ("Play", "Create New" are tapped);
   without it the front end is the original controller page and keeps the navigation keys. */
static int frontend, fe_slot = -1, fe_drag;
static float fe_x0, fe_y0;
/* 面板态的五个虚拟手柄圆钮：缺省不给（同 iOS front_ios.m navigationMode：点按 / 双击 / 拖放 / 点页签 / 面板外点按已全部替代）；DM_TOUCH_NAV_PAD=1 调试时叫回来
   The five virtual pad buttons in panel state: off by default (as iOS front_ios.m navigationMode: tap / double tap / drag / tab tap / tap outside
   the panel replace them all); DM_TOUCH_NAV_PAD=1 brings them back for debugging */
static int nav_pad(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_TOUCH_NAV_PAD"); v = e && e[0] == '1'; } return v; }
static int touch_overlay(void) { static int v = -1; if (v < 0) { const char *o = getenv("DM_ROMFS_OVERLAY"); v = o && *o; } return v; }
static void fe_end_locked(void) {
    if (fe_slot >= 0) { if (fe_drag) dm_ui_drag_end(); else dm_ui_touch(3, 0, 0); }
    fe_slot = -1; fe_drag = 0;
}
static void fe_event_locked(const touch_event *e) {
    if (e->phase == 0 && fe_slot < 0) { fe_slot = e->slot; fe_x0 = e->x; fe_y0 = e->y; fe_drag = 0; }
    if (e->slot != fe_slot) return;
    float nx = e->x / width, ny = e->y / height, dy = fabsf(e->y - fe_y0);
    if (e->phase == 1 && !fe_drag && dy > 10 && dy > fabsf(e->x - fe_x0) && dm_ui_fe_drag_ok()) {
        fe_drag = 1; dm_ui_touch(3, 0, 0); dm_ui_drag_begin(1, fe_x0 / width, fe_y0 / height, 0);
    }
    if (fe_drag) { if (e->phase == 1) dm_ui_drag_move(nx, ny); else fe_end_locked(); return; }
    if (e->phase == 3) { fe_end_locked(); return; }
    dm_ui_touch(e->phase, nx, ny);
    if (e->phase == 2) fe_slot = -1;
}

static void init_locked(void) {
    if (ready) return;
    dm_tc_set_tall_scale(tablet_layout);
    dm_tc_init(&tc, width, height);
    tc.mouse_click_mode = 1;
    dm_tc_set_navigation_mode(&tc, 1);
    dm_tc_enable_targets(&tc, dm_tc_nav_mask(nav_pad()));   /* 开局读取期间也是面板态，同样不出圆钮 | loading at game start is panel state too, no buttons either */
    ready = 1;
}
static void cancel_locked(void) {
    dm_tc_cancel_all(&tc);
    if (ui_drag) dm_ui_drag_end();
    ui_drag = 0;
    fe_end_locked();
    pulse_epoch = pulse_sample = resume_sample = 0;
    pulse_hold_start = 0;
    memset(direct,0,sizeof direct); selected_point=0; stat_next=skill_add_at=0; map_world_pending=0;
    dm_cursor_clear();
}
void dm_android_touch_resize(float w, float h, float l, float r, float b) {
    if (!isfinite(w) || !isfinite(h) || w <= 0 || h <= 0) return;
    pthread_mutex_lock(&lock);
    width = w; height = h; safe_left = fmaxf(0,l); safe_right = fmaxf(0,r); safe_bottom = fmaxf(0,b);
    resizing = cancel = 1; head = tail = 0;
    pthread_mutex_unlock(&lock);
}
void dm_android_touch_tablet(int tablet) {
    pthread_mutex_lock(&lock);
    tablet_layout = !!tablet; resizing = cancel = 1;
    pthread_mutex_unlock(&lock);
}
void dm_android_touch_focus(int active) {
    pthread_mutex_lock(&lock);
    focused = !!active; cancel = 1; head = tail = 0;
    pthread_mutex_unlock(&lock);
}
void dm_android_touch_event(int phase, int slot, float x, float y) {
    if (phase < 0 || phase > 3 || slot < 0 || slot >= DM_TC_MAX_TOUCH || !isfinite(x) || !isfinite(y)) return;
    pthread_mutex_lock(&lock);
    if (focused) {
        /* 溢出只取消，绝不遗留按住的摇杆/按键。 | Overflow cancels instead of leaving a stick/key held. */
        if (tail - head == 256) { cancel = 1; head = tail = 0; }
        else { queue[tail++ % 256] = (touch_event){phase, slot, x, y, now_ns() / 1e9}; }
    }
    pthread_mutex_unlock(&lock);
}
void dm_android_touch_uimsg(const char *cat, const char *act, const char *arg) {
    if (!cat || !act) return;
    pthread_mutex_lock(&lock);
    init_locked();
    dm_tc_scene before = scene;
    dm_tc_set_panel_state(dm_ui_panel_state());
    if (!strcmp(cat, "PanelManager")) hud_blocked = dm_hud_stack_msg(act, arg ? arg : "");
    int nav = dm_tc_navigation(&scene, cat, act, arg ? arg : "");
    /* 同 iOS front_ios.m：局内只要面板栈里还开着非常驻面板就是面板态（不靠名单）；再分出死亡弹窗（2）与只开着对话菜单（3）
       As iOS front_ios.m: in game any open non-resident panel means panel state (not a name list); then split out the death dialog (2) and dialog menu only (3) */
    if (scene.game && hud_blocked > 0) nav = 1;
    nav = dm_tc_panel_mode(nav, &scene, hud_blocked);
    if (before.game != scene.game || before.panels != scene.panels) {
        ++epoch; if (before.game != scene.game) ++world;
        cancel_locked(); head = tail = 0;
    }
    int fe = !scene.game && touch_overlay();
    if (fe != frontend) { cancel_locked(); head = tail = 0; frontend = fe; }
    dm_tc_set_navigation_mode(&tc, nav);
    dm_ui_map_hold(nav != 0);   /* 同 iOS：面板开着全屏地图先让成小地图，关掉回到玩家要的那档 | as iOS: while a panel is open the full-screen map steps aside to the minimap, and after it closes it returns to what the player wants */
    dm_tc_set_joy_block(&tc, !!(dm_ui_panel_state() & DM_PS_TEXT));
    /* 游戏态不要右上地图键（同 iOS front_ios.m navigationMode：点右上角小地图本身就开全屏地图，见 direct_begin）
       No top-right map key in game state (as iOS front_ios.m navigationMode: tapping the top-right minimap itself opens the full map, see direct_begin) */
    /* 3 = 只开着 NPC 对话菜单：底栏十字可点（原版 ＋ 键此时 = 关对话并开功能页），同 iOS | 3 = only the NPC dialog menu is open: the bar's cross is tappable, as iOS */
    dm_tc_enable_targets(&tc, nav ? (dm_tc_nav_mask(nav_pad() && nav != 2 && !dm_tc_scene_dead()) | (nav == 3 ? 1u << DM_TC_MENU : 0u))
                                  : (((1u << DM_TC_COUNT) - 1) & ~(1u << DM_TC_MAP)));
    dm_uiscreen_set_panel(nav && !dm_tc_scene_dead() && !dm_tc_scene_dialog_only(&scene));
    if (dm_touch_game_set_scene) dm_touch_game_set_scene(!nav, epoch, world);
    pthread_mutex_unlock(&lock);
}
static void sync_host(void) {
    dm_tc_set_panel_state(dm_ui_panel_state());
    dm_tc_set_tap_box(dm_ui_tap_box());
    dm_tc_set_cur_stack(dm_ui_cur_stack_id());
    dm_tc_set_hover_unid(dm_ui_hover_unid());
    dm_hud_info info; dm_hud_get(&info);
    dm_tc_set_holding(&tc, info.valid && info.holding && !dm_ui_targeting());
    tc.belt_rows = dm_belt_rows(&tc.belt_open);
}
static void requests(void) {
    if (seen_tab != tc.tab_tap_seq) { seen_tab = tc.tab_tap_seq; dm_ui_tab_go(); }
    if (seen_use != tc.use_seq) { seen_use = tc.use_seq; dm_ui_use_item(); }
    if (seen_ident != tc.ident_seq) { seen_ident = tc.ident_seq; dm_ui_identify_hovered(); }
    if (seen_long != tc.long_press_seq) { seen_long = tc.long_press_seq; dm_ui_long_press(); }
    if (seen_item != tc.item_tap_seq) { seen_item = tc.item_tap_seq; dm_ui_item_tap(); }
    if (seen_stack != tc.stack_take_seq) { seen_stack = tc.stack_take_seq; dm_stash_take(1); }
    if (seen_world != tc.world_tap_seq) {
        seen_world = tc.world_tap_seq;
        float nx = tc.world_tap_x / width, ny = tc.world_tap_y / height;
        dm_ui_world_tap(nx, ny, nx, ny);
    }
    if (seen_mouse != tc.mouse_click_seq) {
        seen_mouse = tc.mouse_click_seq;
        float nx = tc.mouse_click_x / width, ny = tc.mouse_click_y / height;
        dm_ui_touch(0,nx,ny); dm_ui_touch(2,nx,ny);
    }
    if (tc.diff_request) { dm_hud_request_players(tc.diff_request); tc.diff_request = 0; }
    if (tc.belt_request) {
        int br = tc.belt_request; tc.belt_request = 0;
        if (br == 1 || br == 2) dm_belt_expand(br == 1);
        else dm_belt_use(br & 15, (br >> 4) & 15);
    }
}
static void release_pointer(dm_tc_finger *f) {
    float lift = DM_TC_POINTER_LIFT_PT * dm_tc_scale_for_size(width,height);
    int kind = dm_ui_drop_at_now(f->x/width, (f->y-lift)/height, f->y/height);
    float x,y;
    if (f->shop_drag) {
        if (kind == 11) dm_ui_stack_cube_at(f->aim_origin_x/width,f->aim_origin_y/height);
        else if (kind == 5 && dm_ui_drop_bag()) {
            if (tc.pointer_stack) dm_ui_stack_take_at(f->aim_origin_x/width,f->aim_origin_y/height);
            else dm_ui_shop_buy_at(f->aim_origin_x/width,f->aim_origin_y/height);
        }
        tc.pointer_x = f->aim_origin_x; tc.pointer_y = f->aim_origin_y;
    } else if ((kind == 5 && dm_ui_drop_point(&x,&y)) || (kind == 0 && dm_ui_origin_point(&x,&y))) dm_tc_pointer_release_at(&tc,x*width,y*height);
    else { if (kind == 7) { dm_shop_sell_cursor(); kind = 0; } dm_tc_pointer_release(&tc,kind); }
}
/* 空技能位和小地图先截获，不发攻击、世界点击或摇杆输入。
   Capture empty skill slots and minimap taps before emitting attack, world-tap or joystick input. */
static int direct_begin(const touch_event *e) {
    if (tc.navigation_mode) return 0;
    int target=dm_tc_hit(&tc.layout,e->x,e->y), kind=0; uint64_t key=0;
    if (target>=DM_TC_MAIN && target<=DM_TC_SKILL4) {
        dm_tg_snapshot snapshot; dm_touch_game_snapshot(&snapshot);
        dm_ta_glyph g=dm_ta_current_set(&snapshot,tc.layout.target[target].button,now_ns(),tc.skill_set);
        if (g.status==DM_TA_EMPTY) { kind=1; key=tc.layout.target[target].button|(tc.skill_set?DM_BTN_ZL:0); }
    }
    if (!kind && target==DM_TC_MAP) kind=2;
    float r[4];
    if (!kind && target<0 && dm_ui_minimap_rect(r)) {
        float h=(r[3]-r[1])*height, band=h/4;
        if (band<30) band=fminf(30,h*.4f);
        if (e->x>=r[0]*width && e->x<=r[2]*width && e->y>=r[1]*height && e->y<=r[1]*height+band) kind=2;
    }
    if (!kind && target<0 && dm_ui_map_is_full() && !(e->x<tc.layout.joy_zone_r && e->y>=tc.layout.joy_zone_t)) kind=3;
    if (!kind) return 0;
    direct[e->slot]=(direct_gesture){kind,e->x,e->y,e->time,key}; return 1;
}
static int direct_event(const touch_event *e) {
    direct_gesture *g=&direct[e->slot]; if (!g->kind) return 0;
    if (hypotf(e->x-g->x,e->y-g->y)>14) g->kind=-1;
    if (e->phase==2) {
        if (!tc.navigation_mode && g->kind>0 && e->time-g->time<.6) {
            if (g->kind==1) dm_ui_bind_begin(g->key);
            else if (dm_ui_map_is_full()) dm_ui_map_full(0);
            else if (g->kind==2) { map_world_seq=dm_world_tap(e->x/width,e->y/height); map_world_pending=1; }
        }
        memset(g,0,sizeof *g);
    } else if (e->phase==3) memset(g,0,sizeof *g);
    return 1;
}
static void point_tick(double now) {
    if (tc.navigation_mode && !dm_ui_bind_active() && tc.pointer_slot>=0) {
        dm_tc_finger *f=&tc.finger[tc.pointer_slot]; float nx=0,ny=0;
        int hit=dm_ui_bind_skill_hit(&nx,&ny); uint32_t id=dm_ui_hit_id();
        if (f->active && !f->pointer_dragged && !f->ui_drag && now-f->start_time_s>=.5 && hypotf(f->x-f->aim_origin_x,f->y-f->aim_origin_y)<=10*dm_tc_scale_for_size(width,height)) {
            if (hit==2 && id>=1 && id<=4) {
                if (!f->pointer_long) { f->pointer_long=1; stat_next=now; selected_point=id; tc.pointer_valid=1; tc.pointer_x=nx*width; tc.pointer_y=ny*height; }
                if (now>=stat_next) { stat_next=now+.15; dm_stat_add((int)id-1,1); }
            } else if (hit==1 && !f->pointer_long) dm_tc_pointer_press(&tc,nx*width,ny*height,DM_BTN_Y);
        }
    }
    if (skill_add_at>0 && now>=skill_add_at) {
        dm_points_info points; dm_points_get(&points);
        if (points.tree_open && points.sel_skill>=0 && !dm_cursor_pending()) {
            skill_add_at=0;
            if (points.sel_block==0 || points.sel_block==5) dm_skill_add_selected();
        } else if (now>skill_add_at+1 || !points.tree_open) skill_add_at=0;
    }
}
void dm_android_touch_step(uint64_t guest_now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    (void)guest_now;
    pthread_mutex_lock(&lock);
    init_locked();
    int next_swap = dm_custom_get("touch_swap_above");
    if (next_swap != swap_above) { swap_above = next_swap; resizing = cancel = 1; }
    if (cancel) { cancel_locked(); cancel = 0; }
    if (resizing) {
        dm_tc_set_tall_scale(tablet_layout); dm_tc_set_swap_above(swap_above);
        dm_tc_set_safe_insets(safe_left,safe_right); dm_tc_set_edge_bottom(safe_bottom); dm_tc_set_safe_bottom(safe_bottom);
        dm_ui_safe_left((int)lroundf(safe_left * 2160 / height));
        dm_ui_safe_right((int)lroundf(safe_right * 2160 / height));
        dm_tc_resize(&tc,width,height); resizing = 0;
    }
    if (!focused) { pthread_mutex_unlock(&lock); return; }
    sync_host();
    if (map_world_pending) {
        int result=dm_world_tap_result(map_world_seq);
        if (result>=0) { map_world_pending=0; if (!result && !tc.navigation_mode) dm_ui_map_full(1); }
    }
    /* 本次取手柄前排队的触点整批在这里套用：按下与抬起落在同一批（点得比游戏两次取手柄的间隔还短、或游戏卡住没取输入）时，状态机算出的键在批末已经松开，
       游戏一次也看不到——iOS 由前端异步调 dm_input_set，按下沿进 hle_hid.c 的 g_press_latch 锁存，这里同样：批内任何时刻按着过的键这次一并给出，下一次照实际值。
       The touches queued before this controller read are applied here as one batch: when press and release land in the same batch (a tap shorter than the
       interval between two reads, or the game stalling its input reads), the state machine's key is already up at the end of the batch and the game never
       sees it. On iOS the front end calls dm_input_set asynchronously and the press edge is latched in hle_hid.c g_press_latch; likewise here, keys held at
       any point in the batch are delivered with this read, and the next read reports the actual state. */
    /* 批内锁存按手指归属；滑出取消只撤销该手指，保留其他手指的快按。
       Attribute batch latches to each finger; slide cancellation revokes only that finger's quick tap. */
    uint64_t latched[DM_TC_MAX_TOUCH] = {0};
    while (head != tail) {
        touch_event e = queue[head++ % 256];
        if (frontend) { fe_event_locked(&e); continue; }
        if (direct_event(&e) || (e.phase==0 && direct_begin(&e))) continue;
        dm_tc_finger *f = &tc.finger[e.slot];
        uint64_t before_buttons = tc.buttons;
        int before_target = f->target;
        int event_cancelled = 0;
        if (e.phase == 0) latched[e.slot] = 0;
        if (e.phase == 0) {
            if (dm_tc_belt_outside(&tc,e.x,e.y)) { dm_belt_expand(0); continue; }
            { int dm_cam_mode(void), dm_cam_style(void); tc.cam_on = dm_cam_mode(); tc.cam_orbit = dm_cam_style() == 1; }   /* 镜头实验：右半屏拖动看四周要知道档 | camera experiment: looking around on the right half needs the mode */
            dm_tc_begin_at(&tc,e.slot,e.x,e.y,e.time);
            if (!tc.navigation_mode && f->target == DM_TC_PORTAL) { dm_tc_cancel_finger(&tc,e.slot); dm_tp_request(); }
            if (tc.navigation_mode && tc.pointer_slot == e.slot) dm_tc_pointer_item(&tc,dm_ui_item_at_now(e.x/width,e.y/height));
        } else if (e.phase == 1 || e.phase == 2) {
            dm_tc_move_at(&tc,e.slot,e.x,e.y,e.time);
            if (tc.cam_pitch != 0 || tc.cam_yaw != 0) { void dm_cam_tilt(float); void dm_cam_turn(float); if (tc.cam_pitch != 0) dm_cam_tilt(tc.cam_pitch); if (tc.cam_yaw != 0) dm_cam_turn(tc.cam_yaw); tc.cam_pitch = tc.cam_yaw = 0; }   /* 镜头实验：右半屏拖动看四周（dm_touch_controls.c） | camera experiment: drag on the right half to look around */
            event_cancelled = f->aim_cancelled;
            if (tc.navigation_mode && tc.pointer_slot == e.slot && f->ui_drag) {
                if (!ui_drag) {
                    ui_drag = 1; dm_ui_drag_lift(DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(width,height)/height);
                    dm_ui_drag_begin(f->ui_drag,f->aim_origin_x/width,f->aim_origin_y/height,44/height);
                }
                dm_ui_drag_move(e.x/width,e.y/height);
            }
            if (e.phase == 2) {
                tc.tap_button=0; tc.tap_move=0;
                if (tc.navigation_mode) {
                    float nx,ny;
                    int hit=dm_ui_bind_skill_hit(&nx,&ny);
                    if (!hit) selected_point=0;
                    else {
                        tc.tap_x=nx*width; tc.tap_y=ny*height;
                        if (hit==1 && dm_ui_bind_active()==2) tc.tap_button=DM_BTN_Y;
                        else if (!dm_ui_bind_active()) tc.tap_move=1;
                    }
                }
                if (tc.navigation_mode && tc.pointer_slot == e.slot && (f->pointer_dragged || f->shop_drag)) release_pointer(f);
                if (ui_drag && tc.pointer_slot == e.slot) { dm_ui_drag_end(); ui_drag = 0; }
                unsigned moved=tc.tap_move_seq;
                dm_tc_end_at(&tc,e.slot,e.time);
                if (moved!=tc.tap_move_seq) {
                    uint32_t id=dm_ui_hit_id();
                    if (id && id==selected_point) {
                        if (id<=4) dm_stat_add((int)id-1,1);
                        else skill_add_at=e.time+.3;
                    } else selected_point=id;
                }
            }
        } else {
            /* 系统取消物品拖动时回原位，不当作丢弃/购买。 | System cancellation returns dragged items; it never buys or discards. */
            if (tc.pointer_slot == e.slot && f->pointer_dragged && !f->shop_drag) {
                dm_tc_pointer_release(&tc,0); dm_tc_end_at(&tc,e.slot,e.time);
            } else dm_tc_cancel_finger(&tc,e.slot);
            if (ui_drag) { dm_ui_drag_end(); ui_drag = 0; }
        }
        int target = e.phase == 2 ? before_target : f->target;
        if (e.phase == 3 || event_cancelled || f->aim_cancelled) latched[e.slot] = 0;
        else {
            latched[e.slot] |= tc.buttons & ~before_buttons;
            if (target >= 0 && target < DM_TC_COUNT)
                latched[e.slot] |= tc.buttons & (tc.layout.target[target].button | (tc.skill_set ? DM_BTN_ZL : 0));
        }
        requests();
    }
    double now = now_ns()/1e9;
    /* 当期iOS：游戏内按住技能连续施放；换绑从技能树长按进入。
       Current iOS: holding a gameplay skill keeps casting; rebinding starts by holding an icon in the skill tree. */
    point_tick(now);
    dm_tc_tick(&tc,now); requests();
    uint64_t sample = dm_input_samples();
    if (tc.pulse_button && pulse_epoch != tc.pulse_epoch) {
        pulse_epoch = tc.pulse_epoch; pulse_sample = sample; pulse_hold_start = 0;
    }
    if (tc.pulse_button && !dm_cursor_pending()) {
        if (!pulse_hold_start) pulse_hold_start = now;
        if (sample - pulse_sample >= 4 && now - pulse_hold_start >= tc.pulse_hold_s) {
            dm_tc_pulse_end(&tc,pulse_epoch); resume_sample = sample + 2;
        }
    }
    if (!tc.pulse_button && dm_tc_pulse_pending(&tc) && resume_sample && sample >= resume_sample) { dm_tc_pulse_resume(&tc); resume_sample = 0; }
    if (tc.pointer_valid) dm_cursor_set_screen(tc.pointer_x,tc.pointer_y,width,height); else dm_cursor_clear();
    int active = 0; for (int i=0;i<DM_TC_MAX_TOUCH;++i) active |= tc.finger[i].active;
    dm_ui_hand_idle(!active && !tc.pulse_button && !dm_tc_pulse_pending(&tc) && !dm_cursor_pending() && !dm_ui_touch_active());
    *buttons |= tc.buttons;
    for (int i=0;i<DM_TC_MAX_TOUCH;++i) *buttons |= latched[i];
    if (!*lx && !*ly) { *lx = tc.lx; *ly = tc.ly; }
    if (!*rx && !*ry) { *rx = tc.rx; *ry = tc.ry; }
    pthread_mutex_unlock(&lock);
}
void dm_android_touch_layout(float out[DM_ANDROID_TOUCH_DRAW_FLOATS]) {
    pthread_mutex_lock(&lock); init_locked();
    memset(out,0,sizeof(float)*DM_ANDROID_TOUCH_DRAW_FLOATS);
    out[0]=tc.layout.width; out[1]=tc.layout.height; out[2]=tc.navigation_mode; out[3]=tc.skill_set;
    for (int i=0;i<23;++i) {
        float *p = out+4+i*5;
        dm_tc_target t = i<DM_TC_COUNT ? tc.layout.target[i] : i==13 ? tc.layout.swap : i>=15 ? tc.layout.pick[i-15] : tc.layout.diff;
        if (i==12) {
            p[0]=tc.joy_slot>=0?tc.joy_origin_x:tc.layout.joy_x; p[1]=tc.joy_slot>=0?tc.joy_origin_y:tc.layout.joy_y;
            p[2]=tc.layout.joy_radius; p[3]=!tc.navigation_mode; p[4]=tc.joy_slot>=0;
        } else {
            p[0]=t.x; p[1]=t.y; p[2]=t.r;
            p[3]=i<DM_TC_COUNT?!!(tc.enabled_targets&(1u<<i)):!tc.navigation_mode && (i<15 || tc.diff_open);
            /* 快照包含松手后仍在发出的脉冲；查询只读，probe和onDraw连续读取不能消费高亮。
               Include pulses that remain active after release; reads are non-consuming so probe/onDraw see the same highlight. */
            p[4]=t.button && (tc.buttons&t.button)==t.button;
            for (int j=0;j<DM_TC_MAX_TOUCH;++j) {
                const dm_tc_finger *f=&tc.finger[j];
                if (!f->active || f->aim_cancelled) continue;
                if ((i<DM_TC_COUNT && f->target==i) || (i==13 && f->target==DM_TC_SWAP)) p[4]=1;
                /* 难度格与腰带消费共用负target；还要命中当前圆，不能把所有选项一起点亮。
                   Difficulty and consumed belt touches share a negative target; hit-test this circle rather than lighting every choice. */
                if (i>=14 && f->target==DM_TC_DIFF && hypotf(f->x-t.x,f->y-t.y)<=t.r) p[4]=1;
            }
        }
        if (frontend) p[3]=0;   /* 前端页面不画控件（见 frontend） | No controls drawn on front-end pages (see frontend) */
    }
    dm_tg_snapshot native; dm_touch_game_snapshot(&native);
    for (int i=0;i<6;++i) {
        float *p=out+DM_ANDROID_TOUCH_GLYPHS+i*8;
        dm_ta_glyph g=i==5 ? dm_ta_skill(359) : dm_ta_current_set(&native,tc.layout.target[i].button,now_ns(),tc.skill_set);
        dm_ta_frame frame;
        p[0]=g.status; p[1]=g.atlas;
        int held=out[4+i*5+4]!=0;
        if (g.status==DM_TA_GLYPH && dm_ta_frame_at(g.atlas,held?g.pressed_frame:g.normal_frame,&frame)) {
            p[2]=frame.x; p[3]=frame.y; p[4]=frame.width; p[5]=frame.height;
            if (g.slot>=0) { int charges=0; p[6]=dm_swapskill_slot(g.slot,&charges); p[7]=charges; }
        }
    }
    dm_hud_info h; dm_hud_get(&h);
    float *hud=out+DM_ANDROID_TOUCH_HUD;
    hud[0]=!frontend && dm_hud_bar_visible(&h,tc.navigation_mode && !dm_tc_scene_dialog_only(&scene),hud_blocked>0);
    hud[1]=h.level; hud[2]=h.exp_permille;
    for (int i=0;i<4;++i) hud[3+i]=h.belt[i];
    hud[7]=h.players; hud[8]=h.hp; hud[9]=h.hp_max; hud[10]=h.mp; hud[11]=h.mp_max;
    hud[12]=h.points; hud[13]=h.quest_unread; hud[14]=h.dead; hud[15]=tc.map_cancel_active;
    hud[16]=tc.lx; hud[17]=tc.ly;   /* 摇杆推量（−32767..32767），画摇杆头用，同 iOS DmTouchOverlay | stick deflection (−32767..32767) for drawing the thumb, as iOS DmTouchOverlay */
    pthread_mutex_unlock(&lock);
}
