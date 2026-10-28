/* Standalone regression harness. Do not link it into the product library.
 * Includes the REAL Android bridge and shared touch state machine. No duplicate
 * implementation of layout serialization, hit testing, or key latching.
 * Compile with the repository root on -I. NDK: --target=aarch64-linux-android31 -O2 -ffunction-sections -fdata-sections
 * -I <repo> -Wl,--gc-sections -Wl,--no-undefined -lm.
 * This executable never opens saves, starts the guest, or sends Android input.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dm/android/touch_android.c"
#include "dm/ios/Sources/dm_touch_controls.c"

_Static_assert(DM_ANDROID_TOUCH_DRAW_FLOATS == 183, "Review the Java/native ABI before changing this test");
enum { DRAW_JOY = 12, DRAW_SWAP = 13, DRAW_DIFF = 14, DRAW_PICK0 = 15 };
static const char *case_name = "startup";
static unsigned checks, cases;
static uint64_t test_clock_ns = UINT64_C(100000000000), test_sample;
static int observed_panel;
static unsigned panel_calls;

#define REQUIRE(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d case=%s expression=%s\n", __FILE__, __LINE__, case_name, #expression); \
        exit(1); \
    } \
} while (0)

static void unexpected_host_call(const char *name) {
    fprintf(stderr, "FAIL case=%s unexpected host path=%s\n", case_name, name);
    exit(2);
}

/* The Android queue test retains dm_android_touch_step(), so its host boundary
 * must link. Neutral read-only state is deliberate. Unrelated mutations fail
 * loudly rather than silently pretending to execute game behavior.
 */
uint64_t now_ns(void) { return test_clock_ns; }
uint64_t dm_input_samples(void) { return test_sample; }
void dm_cursor_clear(void) {}
int dm_cursor_pending(void) { return 0; }
void dm_ui_hand_idle(int idle) { (void)idle; }
int dm_ui_panel_state(void) { return 0; }
int dm_ui_tap_box(void) { return 0; }
uint32_t dm_ui_cur_stack_id(void) { return 0; }
uint32_t dm_ui_hover_unid(void) { return 0; }
int dm_ui_targeting(void) { return 0; }
int dm_ui_touch_active(void) { return 0; }
int dm_ui_bind_active(void) { return 0; }
int dm_belt_rows(int *expanded) { if (expanded) *expanded = 0; return 1; }
void dm_hud_get(dm_hud_info *out) { memset(out, 0, sizeof *out); }
int dm_custom_get(const char *key) { (void)key; return 0; }
void dm_ui_safe_left(int units) { (void)units; }
void dm_ui_safe_right(int units) { (void)units; }
int dm_touch_game_snapshot(dm_tg_snapshot *out) { memset(out,0,sizeof *out); return 0; }
dm_ta_glyph dm_ta_skill(uint32_t id) { (void)id; return (dm_ta_glyph){.status=DM_TA_UNKNOWN,.atlas=-1}; }
dm_ta_glyph dm_ta_current_set(const dm_tg_snapshot *s,uint64_t key,uint64_t now,int set) { (void)s; (void)key; (void)now; (void)set; return dm_ta_skill(0); }
int dm_ta_frame_at(int atlas,int frame,dm_ta_frame *out) { (void)atlas; (void)frame; (void)out; return 0; }
int dm_swapskill_slot(int slot,int *charges) { (void)slot; *charges=0; return 0; }
int dm_ui_minimap_rect(float r[4]) { (void)r; return 0; }
int dm_ui_map_is_full(void) { return 0; }
int dm_cam_mode(void) { return 0; }
void dm_cam_tilt(float rad) { (void)rad; }
void dm_cam_turn(float rad) { (void)rad; }
void dm_ui_map_full(int on) { (void)on; unexpected_host_call(__func__); }
unsigned dm_world_tap(float x,float y) { (void)x; (void)y; unexpected_host_call(__func__); return 0; }
int dm_world_tap_result(unsigned seq) { (void)seq; return -1; }
uint32_t dm_ui_hit_id(void) { return 0; }
void dm_points_get(dm_points_info *out) { memset(out,0,sizeof *out); }
void dm_stat_add(int stat,int n) { (void)stat; (void)n; unexpected_host_call(__func__); }
void dm_skill_add_selected(void) { unexpected_host_call(__func__); }

void dm_cursor_set_screen(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; unexpected_host_call(__func__); }
void dm_ui_touch(int phase, float x, float y) { (void)phase; (void)x; (void)y; unexpected_host_call(__func__); }
int dm_ui_fe_drag_ok(void) { unexpected_host_call(__func__); return 0; }
void dm_ui_drag_begin(int kind, float x, float y, float tolerance) { (void)kind; (void)x; (void)y; (void)tolerance; unexpected_host_call(__func__); }
void dm_ui_drag_move(float x, float y) { (void)x; (void)y; unexpected_host_call(__func__); }
void dm_ui_drag_end(void) { unexpected_host_call(__func__); }
void dm_ui_drag_lift(float y) { (void)y; unexpected_host_call(__func__); }
int dm_ui_item_at_now(float x, float y) { (void)x; (void)y; unexpected_host_call(__func__); return 0; }
int dm_ui_drop_at_now(float x, float lifted, float finger) { (void)x; (void)lifted; (void)finger; unexpected_host_call(__func__); return 0; }
int dm_ui_drop_bag(void) { unexpected_host_call(__func__); return 0; }
int dm_ui_drop_point(float *x, float *y) { (void)x; (void)y; unexpected_host_call(__func__); return 0; }
int dm_ui_origin_point(float *x, float *y) { (void)x; (void)y; unexpected_host_call(__func__); return 0; }
int dm_ui_bind_skill_hit(float *x, float *y) { (void)x; (void)y; unexpected_host_call(__func__); return 0; }
void dm_ui_bind_begin(uint64_t key) { (void)key; unexpected_host_call(__func__); }
void dm_ui_world_tap(float ux, float uy, float wx, float wy) { (void)ux; (void)uy; (void)wx; (void)wy; unexpected_host_call(__func__); }
void dm_ui_stack_take_at(float x, float y) { (void)x; (void)y; unexpected_host_call(__func__); }
void dm_ui_stack_cube_at(float x, float y) { (void)x; (void)y; unexpected_host_call(__func__); }
void dm_ui_shop_buy_at(float x, float y) { (void)x; (void)y; unexpected_host_call(__func__); }
void dm_ui_tab_go(void) { unexpected_host_call(__func__); }
void dm_ui_use_item(void) { unexpected_host_call(__func__); }
void dm_ui_identify_hovered(void) { unexpected_host_call(__func__); }
void dm_ui_long_press(void) { unexpected_host_call(__func__); }
void dm_ui_item_tap(void) { unexpected_host_call(__func__); }
void dm_stash_take(int n) { (void)n; unexpected_host_call(__func__); }
void dm_hud_request_players(int n) { (void)n; unexpected_host_call(__func__); }
void dm_belt_expand(int on) { (void)on; unexpected_host_call(__func__); }
void dm_belt_use(int col, int row) { (void)col; (void)row; unexpected_host_call(__func__); }
void dm_shop_sell_cursor(void) { unexpected_host_call(__func__); }
void dm_tp_request(void) { unexpected_host_call(__func__); }

/* 观测真实桥发出的面板模式；不依赖链接器删除未用代码，不链接产品宿主。
 * Observe the real bridge's panel mode without relying on linker garbage collection or linking the product host. */
void dm_uiscreen_set_panel(int panel) {
    REQUIRE(panel==0 || panel==1);
    observed_panel=panel; ++panel_calls;
}
static void fixture(int navigation) {
    memset(&tc, 0, sizeof tc);
    memset(&scene, 0, sizeof scene);
    memset(queue, 0, sizeof queue);
    head = tail = 0; ready = 0; focused = 1; cancel = resizing = ui_drag = 0;
    width = 800; height = 375; safe_left = safe_right = safe_bottom = 0; tablet_layout = swap_above = 0;
    epoch = world = 1; pulse_epoch = pulse_sample = resume_sample = 0; pulse_hold_start = 0;
    seen_world = seen_mouse = seen_tab = seen_use = seen_ident = seen_long = seen_item = seen_stack = 0;
    frontend = 0; fe_slot = -1; fe_drag = 0; fe_x0 = fe_y0 = 0;
    test_clock_ns = UINT64_C(100000000000); test_sample = 0;
    observed_panel=-1; panel_calls=0;
    dm_tc_set_safe_insets(0, 0); dm_tc_set_edge_bottom(0); dm_tc_set_safe_bottom(0); dm_tc_set_swap_above(0);
    dm_tc_set_panel_state(0); dm_tc_set_tap_box(0); dm_tc_set_cur_stack(0); dm_tc_set_hover_unid(0);
    init_locked();
    dm_tc_set_navigation_mode(&tc, navigation);
    dm_tc_enable_targets(&tc, navigation ? dm_tc_nav_mask(1) : ((1u << DM_TC_COUNT) - 1u));
}

static int field(int record, int component) { return 4 + record * 5 + component; }
static int lit(const float *s, int record) { return s[field(record, 4)] != 0; }
static int visible(const float *s, int record) { return s[field(record, 3)] != 0; }

/* Read as #19 does: probe, then actual draw. Both must see the same data and
 * neither read may consume pulse/pressed state. Guard words detect ABI overrun.
 */
static void read_twice(float result[DM_ANDROID_TOUCH_DRAW_FLOATS]) {
    struct guarded { uint32_t left; float values[DM_ANDROID_TOUCH_DRAW_FLOATS]; uint32_t right; } a, b;
    unsigned char before[sizeof tc];
    a.left = b.left = UINT32_C(0x13579bdf); a.right = b.right = UINT32_C(0x2468ace0);
    memset(a.values, 0xa5, sizeof a.values); memset(b.values, 0x5a, sizeof b.values);
    memcpy(before, &tc, sizeof tc);
    dm_android_touch_layout(a.values);
    dm_android_touch_layout(b.values);
    REQUIRE(a.left == UINT32_C(0x13579bdf) && b.left == a.left);
    REQUIRE(a.right == UINT32_C(0x2468ace0) && b.right == a.right);
    REQUIRE(memcmp(a.values, b.values, sizeof a.values) == 0);
    REQUIRE(memcmp(before, &tc, sizeof tc) == 0);
    for (int i = 0; i < DM_ANDROID_TOUCH_DRAW_FLOATS; ++i) REQUIRE(isfinite(a.values[i]));
    memcpy(result, a.values, sizeof a.values);
}

static void unchanged_geometry(const float *a, const float *b) {
    REQUIRE(a[0] == b[0] && a[1] == b[1]);
    for (int i = 0; i < 23; ++i)
        for (int c = 0; c < 3; ++c) REQUIRE(a[field(i, c)] == b[field(i, c)]);
}

static void down_target(int slot, int target, double at) {
    dm_tc_target t = tc.layout.target[target];
    dm_tc_begin_at(&tc, slot, t.x, t.y, at);
    REQUIRE(tc.finger[slot].active && tc.finger[slot].target == target);
}

static void test_idle_and_abi(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS], b[DM_ANDROID_TOUCH_DRAW_FLOATS]; read_twice(a); read_twice(b);
    REQUIRE(memcmp(a, b, sizeof a) == 0);
    REQUIRE(a[0] == 800 && a[1] == 375 && a[2] == 0 && a[3] == 0);
    REQUIRE(visible(a, DRAW_JOY) && visible(a, DRAW_SWAP) && visible(a, DRAW_DIFF));
    for (int i = 0; i < 23; ++i) REQUIRE(!lit(a, i));
}

static void test_press_release_and_multitouch(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS], b[DM_ANDROID_TOUCH_DRAW_FLOATS]; read_twice(a);
    down_target(0, DM_TC_MAIN, 100.0); read_twice(b);
    REQUIRE(lit(b, DM_TC_MAIN)); REQUIRE(memcmp(a, b, sizeof a) != 0);
    unchanged_geometry(a, b);
    down_target(1, DM_TC_SKILL1, 100.01); read_twice(b);
    REQUIRE(lit(b, DM_TC_MAIN) && lit(b, DM_TC_SKILL1));
    dm_tc_cancel_finger(&tc, 0); read_twice(b);
    REQUIRE(!lit(b, DM_TC_MAIN) && lit(b, DM_TC_SKILL1));
    dm_tc_cancel_all(&tc); read_twice(b);
    for (int i = 0; i < 23; ++i) REQUIRE(!lit(b, i));
    down_target(0, DM_TC_MAIN, 101.0);
    dm_tc_end_at(&tc, 0, 101.1); read_twice(b);
    REQUIRE(!lit(b, DM_TC_MAIN) && tc.buttons == 0);
}

static void test_slide_cancel(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS]; down_target(0, DM_TC_SKILL1, 100.0);
    dm_tc_target map = tc.layout.target[DM_TC_MAP];
    dm_tc_move_at(&tc, 0, map.x, map.y, 100.1);
    REQUIRE(tc.finger[0].active && tc.finger[0].aim_cancelled);
    REQUIRE((tc.buttons & tc.layout.target[DM_TC_SKILL1].button) == 0);
    read_twice(a); REQUIRE(!lit(a, DM_TC_SKILL1));
    dm_tc_end_at(&tc, 0, 100.11); read_twice(a);
    REQUIRE(!lit(a, DM_TC_SKILL1) && tc.pulse_button == 0);
}

static void test_release_pulse(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS], b[DM_ANDROID_TOUCH_DRAW_FLOATS]; down_target(0, DM_TC_SKILL1, 100.0);
    dm_tc_end_at(&tc, 0, 100.01);
    REQUIRE(!tc.finger[0].active && tc.pulse_button == tc.layout.target[DM_TC_SKILL1].button);
    read_twice(a); REQUIRE(lit(a, DM_TC_SKILL1));
    REQUIRE(dm_tc_pulse_end(&tc, tc.pulse_epoch));
    read_twice(b); REQUIRE(!lit(b, DM_TC_SKILL1));
    REQUIRE(memcmp(a, b, sizeof a) != 0); unchanged_geometry(a, b);
}

static void test_swap_and_delayed_pulse(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS], b[DM_ANDROID_TOUCH_DRAW_FLOATS]; read_twice(a);
    dm_tc_target swap = tc.layout.swap;
    dm_tc_begin_at(&tc, 0, swap.x, swap.y, 100.0); read_twice(b);
    REQUIRE(tc.finger[0].target == DM_TC_SWAP && b[3] == 0 && lit(b, DRAW_SWAP));
    REQUIRE(memcmp(a, b, sizeof a) != 0); unchanged_geometry(a, b);
    dm_tc_end_at(&tc, 0, 100.01); read_twice(b);
    REQUIRE(b[3] == 1 && !lit(b, DRAW_SWAP));
    down_target(0, DM_TC_SKILL1, 101.0); dm_tc_end_at(&tc, 0, 101.01);
    REQUIRE(!tc.finger[0].active && tc.defer_button != 0 && tc.pulse_button == 0);
    read_twice(a); REQUIRE(a[3] == 1 && !lit(a, DM_TC_SKILL1));
    dm_tc_tick(&tc, 101.0 + DM_TC_ZL_LEAD_S + 0.01); read_twice(b);
    REQUIRE(tc.defer_button == 0 && tc.pulse_button != 0 && lit(b, DM_TC_SKILL1));
    REQUIRE(memcmp(a, b, sizeof a) != 0); unchanged_geometry(a, b);
    REQUIRE(dm_tc_pulse_end(&tc, tc.pulse_epoch)); read_twice(b); REQUIRE(!lit(b, DM_TC_SKILL1));
    dm_tc_begin_at(&tc, 0, swap.x, swap.y, 102.0); dm_tc_end_at(&tc, 0, 102.01);
    read_twice(b); REQUIRE(b[3] == 0);
}

static void test_difficulty_hit_and_close(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS], b[DM_ANDROID_TOUCH_DRAW_FLOATS]; read_twice(a);
    for (int i = 0; i < 8; ++i) REQUIRE(!visible(a, DRAW_PICK0 + i));
    dm_tc_target diff = tc.layout.diff;
    dm_tc_begin_at(&tc, 0, diff.x, diff.y, 100.0); read_twice(b);
    REQUIRE(tc.diff_open && tc.finger[0].target == DM_TC_DIFF && lit(b, DRAW_DIFF));
    for (int i = 0; i < 8; ++i) { REQUIRE(visible(b, DRAW_PICK0 + i)); REQUIRE(!lit(b, DRAW_PICK0 + i)); }
    REQUIRE(memcmp(a, b, sizeof a) != 0);
    dm_tc_cancel_finger(&tc, 0); REQUIRE(tc.diff_open);
    dm_tc_target pick = tc.layout.pick[3];
    dm_tc_begin_at(&tc, 0, pick.x, pick.y, 100.1); read_twice(b);
    REQUIRE(tc.diff_request == 4 && !tc.diff_open && tc.finger[0].target == DM_TC_DIFF);
    REQUIRE(!lit(b, DRAW_DIFF));
    for (int i = 0; i < 8; ++i) {
        REQUIRE(!visible(b, DRAW_PICK0 + i));
        if (i != 3) REQUIRE(!lit(b, DRAW_PICK0 + i));
    }
    /* Picking immediately collapses the strip. The selected hidden cell's
     * pressed bit may be retained or gated by visibility; no rendered state is
     * asserted for an invisible cell. Incorrectly highlighting DIFF is caught.
     */
    dm_tc_cancel_finger(&tc, 0);
    dm_tc_begin_at(&tc, 0, diff.x, diff.y, 101.0); dm_tc_end_at(&tc, 0, 101.01);
    REQUIRE(tc.diff_open);
    dm_tc_begin_at(&tc, 0, diff.x, diff.y, 101.1); read_twice(b);
    REQUIRE(!tc.diff_open && lit(b, DRAW_DIFF));
    for (int i = 0; i < 8; ++i) REQUIRE(!visible(b, DRAW_PICK0 + i));
}

static void test_belt_diff_target_does_not_light_difficulty(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS]; tc.belt_rows = 2; tc.belt_open = 1;
    dm_tc_target belt = tc.layout.target[DM_TC_BELT1];
    float row_y = belt.y - 165.0f * tc.layout.height / 1320.0f;
    dm_tc_begin_at(&tc, 0, belt.x, row_y, 100.0);
    REQUIRE(tc.finger[0].active && tc.finger[0].target == DM_TC_DIFF);
    REQUIRE(tc.belt_request != 0); read_twice(a);
    REQUIRE(!lit(a, DRAW_DIFF));
    for (int i = 0; i < 8; ++i) REQUIRE(!visible(a, DRAW_PICK0 + i));
}

static void test_navigation_and_resize(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS], b[DM_ANDROID_TOUCH_DRAW_FLOATS]; read_twice(a);
    dm_tc_set_navigation_mode(&tc, 1); dm_tc_enable_targets(&tc, dm_tc_nav_mask(1)); read_twice(b);
    REQUIRE(b[2] == 1 && memcmp(a, b, sizeof a) != 0);
    REQUIRE(!visible(b, DRAW_JOY) && !visible(b, DRAW_SWAP) && !visible(b, DRAW_DIFF));
    dm_tc_resize(&tc, 960, 440); read_twice(b);
    REQUIRE(b[0] == 960 && b[1] == 440 && b[2] == 1);
}

static uint64_t queue_sample(void) {
    uint64_t buttons = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    ++test_sample;
    dm_android_touch_step(test_clock_ns, &buttons, &lx, &ly, &rx, &ry);
    return buttons;
}

static void test_android_queue_ten_short_taps(void) {
    fixture(0); dm_tc_target main = tc.layout.target[DM_TC_MAIN]; unsigned observed = 0;
    for (unsigned i = 0; i < 10; ++i) {
        test_clock_ns += UINT64_C(100000000);
        dm_android_touch_event(0, 0, main.x, main.y);
        test_clock_ns += UINT64_C(5000000);
        dm_android_touch_event(2, 0, main.x, main.y);
        REQUIRE(tail - head == 2);
        uint64_t delivered = queue_sample();
        REQUIRE((delivered & DM_BTN_A) != 0);
        REQUIRE(!tc.finger[0].active && (tc.buttons & DM_BTN_A) == 0);
        ++observed;
        test_clock_ns += UINT64_C(10000000);
        REQUIRE((queue_sample() & DM_BTN_A) == 0);
    }
    REQUIRE(observed == 10);
    printf("DETAIL native_queue_short_taps=%u/10 scope=bridge_samples_not_game_actions\n", observed);
}

static void test_android_focus_cancel(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS]; dm_tc_target main = tc.layout.target[DM_TC_MAIN];
    dm_android_touch_event(0, 0, main.x, main.y);
    REQUIRE((queue_sample() & DM_BTN_A) != 0); read_twice(a); REQUIRE(lit(a, DM_TC_MAIN));
    dm_android_touch_focus(0); REQUIRE(queue_sample() == 0); read_twice(a);
    REQUIRE(!lit(a, DM_TC_MAIN) && tc.buttons == 0 && head == tail);
    dm_android_touch_focus(1); REQUIRE(queue_sample() == 0); read_twice(a); REQUIRE(!lit(a, DM_TC_MAIN));
}

/* 同一批内取消不能把旧按下沿带给游戏；另一手指的快按仍有效。
   Cancellation in a batch must revoke the old edge without discarding another finger's quick tap. */
static void test_android_cancelled_latch(void) {
    fixture(0);
    dm_tc_target main=tc.layout.target[DM_TC_MAIN], map=tc.layout.target[DM_TC_MAP];
    dm_android_touch_event(0,0,main.x,main.y);
    dm_android_touch_event(1,0,map.x,map.y);
    dm_android_touch_event(2,0,map.x,map.y);
    REQUIRE((queue_sample() & DM_BTN_A)==0);
    REQUIRE(tc.buttons==0);
    fixture(0); main=tc.layout.target[DM_TC_MAIN];
    dm_android_touch_event(0,0,main.x,main.y);
    dm_android_touch_event(3,0,main.x,main.y);
    dm_android_touch_event(0,1,main.x,main.y);
    dm_android_touch_event(2,1,main.x,main.y);
    REQUIRE((queue_sample() & DM_BTN_A)!=0);
    REQUIRE((queue_sample() & DM_BTN_A)==0);
}

/* 换组只在原位松手时触发；滑出去再回来的手势仍取消。
   Switching commits on release inside the original target; leaving and returning still cancels. */
static void test_android_swap_release_cancel(void) {
    fixture(0); dm_tc_target swap=tc.layout.swap;
    dm_android_touch_event(0,0,swap.x,swap.y); queue_sample(); REQUIRE(tc.skill_set==0);
    dm_android_touch_event(2,0,swap.x,swap.y); queue_sample(); REQUIRE(tc.skill_set==1);
    dm_android_touch_event(0,0,swap.x,swap.y);
    dm_android_touch_event(1,0,swap.x-100,swap.y);
    dm_android_touch_event(2,0,swap.x,swap.y);
    queue_sample(); REQUIRE(tc.skill_set==1);
}

/* 从加载进入游戏、开关背包、只开NPC对话时，宿主面板模式必须各自正确。
 * Host panel mode must distinguish entering gameplay, opening/closing inventory and an NPC-dialog-only stack. */
static void test_android_panel_observation(void) {
    fixture(0);
    dm_android_touch_uimsg(NULL,"UnloadPanel","LoadScreenPanel");
    REQUIRE(panel_calls==0 && observed_panel==-1);
    dm_android_touch_uimsg("PanelManager","UnloadPanel","LoadScreenPanel");
    REQUIRE(scene.game==1 && tc.navigation_mode==0 && observed_panel==0 && panel_calls==1);
    dm_tc_target main=tc.layout.target[DM_TC_MAIN];
    dm_android_touch_event(0,0,main.x,main.y);
    REQUIRE((queue_sample()&DM_BTN_A)!=0);
    dm_android_touch_event(2,0,main.x,main.y);
    REQUIRE(head!=tail);
    dm_android_touch_uimsg("PanelManager","OpenPanel","PlayerInventoryExpansionLayout");
    REQUIRE(tc.navigation_mode!=0 && observed_panel==1 && panel_calls==2);
    REQUIRE(tc.buttons==0 && head==tail && !tc.finger[0].active);
    dm_android_touch_uimsg("PanelManager","UnloadPanel","PlayerInventoryExpansionLayout");
    REQUIRE(tc.navigation_mode==0 && observed_panel==0 && panel_calls==3);
    dm_android_touch_uimsg("PanelManager","OpenPanel","NpcDialogPanel");
    REQUIRE(tc.navigation_mode!=0 && dm_tc_scene_dialog_only(&scene));
    REQUIRE(observed_panel==0 && panel_calls==4);
    dm_android_touch_uimsg("PanelManager","UnloadAll","");
    REQUIRE(tc.navigation_mode==0 && observed_panel==0 && panel_calls==5);
}

static void run_case(const char *name, void (*body)(void)) {
    case_name = name; body(); ++cases; printf("PASS %s\n", name);
}

int main(void) {
    puts("SCOPE native_layout_and_input_bridge; no_Java_Canvas_no_guest_no_save_access");
    run_case("idle_183_abi", test_idle_and_abi);
    run_case("press_release_multitouch", test_press_release_and_multitouch);
    run_case("slide_cancelled_finger", test_slide_cancel);
    run_case("release_pulse_highlight", test_release_pulse);
    run_case("swap_and_delayed_pulse", test_swap_and_delayed_pulse);
    run_case("difficulty_hit_and_close", test_difficulty_hit_and_close);
    run_case("belt_diff_target_no_false_highlight", test_belt_diff_target_does_not_light_difficulty);
    run_case("navigation_and_resize", test_navigation_and_resize);
    run_case("android_queue_ten_short_taps", test_android_queue_ten_short_taps);
    run_case("android_focus_cancel", test_android_focus_cancel);
    run_case("android_cancelled_latch", test_android_cancelled_latch);
    run_case("android_swap_release_cancel", test_android_swap_release_cancel);
    run_case("android_panel_observation", test_android_panel_observation);
    printf("RESULT PASS cases=%u assertions=%u; game_quick_taps_10_of_10=NOT_TESTED\n", cases, checks);
    return 0;
}
