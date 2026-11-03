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

_Static_assert(DM_ANDROID_TOUCH_DRAW_FLOATS == 185, "Review the Java/native ABI before changing this test");
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
/* 安全区单位由桥按视图高换算后交来，用例直接读；视图矩形用例要验它。 | The bridge hands the safe-area units in after converting by the view height; the view-rectangle case reads them back. */
static int seen_safe_left = -1, seen_safe_right = -1;
void dm_ui_safe_left(int units) { seen_safe_left = units; }
void dm_ui_safe_right(int units) { seen_safe_right = units; }
int dm_touch_game_snapshot(dm_tg_snapshot *out) { memset(out,0,sizeof *out); return 0; }
dm_ta_glyph dm_ta_skill(uint32_t id) { (void)id; return (dm_ta_glyph){.status=DM_TA_UNKNOWN,.atlas=-1}; }
dm_ta_glyph dm_ta_current_set(const dm_tg_snapshot *s,uint64_t key,uint64_t now,int set) { (void)s; (void)key; (void)now; (void)set; return dm_ta_skill(0); }
int dm_ta_frame_at(int atlas,int frame,dm_ta_frame *out) { (void)atlas; (void)frame; (void)out; return 0; }
int dm_swapskill_slot(int slot,int *charges) { (void)slot; *charges=0; return 0; }
int dm_ui_minimap_rect(float r[4]) { (void)r; return 0; }
int dm_ui_map_is_full(void) { return 0; }
int dm_cam_mode(void) { return 0; }
int dm_cam_style(void) { return 0; }
void dm_cam_tilt(float rad) { (void)rad; }
void dm_cam_turn(float rad) { (void)rad; }
void dm_ui_map_full(int on) { (void)on; unexpected_host_call(__func__); }
/* 真全屏 / 真实画面 / 界面矩形的桩：开关与读数由用例给（front_android.c 归 Lead，这里不链产品代码）。
 * True fullscreen / real picture / UI rectangle stubs: the case supplies the switch and the readings (front_android.c is the Lead's and no product code is linked here). */
static int test_full, test_real_w = 1000, test_real_h = 500, test_ui_x, test_ui_y = 150, test_ui_w = 1000, test_ui_h = 350;
int front_android_full(void) { return test_full; }
int dm_screen_real(int *w, int *h) { if (w) *w = test_real_w; if (h) *h = test_real_h; return test_real_h > 0; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { if (x) *x = test_ui_x; if (y) *y = test_ui_y; if (w) *w = test_ui_w; if (h) *h = test_ui_h; }
/* 点世界两个入口分开记：requests 那侧视图归一化与画面归一化都要对，direct_event 那侧只有画面归一化。别的用例不该走到这里。
 * The two world-tap entries are recorded separately: the requests side needs both the view and the picture normalisation, the direct_event side only the picture one. Other cases must not reach here. */
static int world_tap_expected; static float tap_ux, tap_uy, tap_wx, tap_wy, map_wx, map_wy;
unsigned dm_world_tap(float x,float y) { if (!world_tap_expected) unexpected_host_call(__func__); map_wx = x; map_wy = y; return 0; }
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
void dm_ui_world_tap(float ux, float uy, float wx, float wy) {
    if (!world_tap_expected) unexpected_host_call(__func__);
    tap_ux = ux; tap_uy = uy; tap_wx = wx; tap_wy = wy;
}
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
/* 面板栈读数与全屏地图让位（宿主 dm_cursor.c）：桥在每条界面消息上调。面板栈回 0（没有挡底栏的面板，面板态只看场景），地图让位记下最后一次的值。
 * Panel stack reading and the full-screen map stepping aside (host dm_cursor.c), called by the bridge on UI messages. The stack answers 0 (no panel
 * blocking the bottom bar, so the panel mode comes from the scene alone), and the map hold records the last value. */
static int map_hold;
int dm_hud_stack_msg(const char *act, const char *arg) { REQUIRE(act && arg); return 0; }
void dm_ui_map_hold(int on) { REQUIRE(on==0 || on==1); map_hold=on; }
static void fixture(int navigation) {
    memset(&tc, 0, sizeof tc);
    memset(&scene, 0, sizeof scene);
    memset(queue, 0, sizeof queue);
    head = tail = 0; ready = 0; focused = 1; cancel = resizing = ui_drag = 0;
    /* 窗口 = 视图 = 800x375（没给视图矩形时的整屏回退）；真全屏与点世界读数回缺省。 | Window = view = 800x375 (the whole-window fallback); true fullscreen and world readings default. */
    width = 800; height = 375; win_w = 800; win_h = 375; vr_x = vr_y = 0; vr_w = 800; vr_h = 375;
    safe_left = safe_right = safe_bottom = 0; tablet_layout = swap_above = 0;
    test_full = 0; world_tap_expected = 0; tap_ux = tap_uy = tap_wx = tap_wy = map_wx = map_wy = 0; seen_safe_left = seen_safe_right = -1;
    epoch = world = 1; pulse_epoch = pulse_sample = resume_sample = 0; pulse_hold_start = 0;
    seen_world = seen_mouse = seen_tab = seen_use = seen_ident = seen_long = seen_item = seen_stack = 0;
    frontend = 0; fe_slot = -1; fe_drag = 0; fe_x0 = fe_y0 = 0;
    test_clock_ns = UINT64_C(100000000000); test_sample = 0;
    observed_panel=-1; panel_calls=0; map_hold=-1;
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
    REQUIRE(a[DM_ANDROID_TOUCH_HUD + 16] == 0 && a[DM_ANDROID_TOUCH_HUD + 17] == 0);
}

/* The last two HUD values are the stick deflection the Java overlay draws the thumb from (TouchOverlay HUD+16 / HUD+17). */
static void test_stick_deflection(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS];
    const float x = tc.layout.joy_x, y = tc.layout.joy_y, r = tc.layout.joy_radius;
    dm_tc_begin_at(&tc, 0, x, y, 100.0);
    REQUIRE(tc.finger[0].active && tc.finger[0].target == DM_TC_JOYSTICK);
    dm_tc_move_at(&tc, 0, x + r, y, 100.05); read_twice(a);
    REQUIRE(tc.lx > 0);
    REQUIRE(a[DM_ANDROID_TOUCH_HUD + 16] == tc.lx && a[DM_ANDROID_TOUCH_HUD + 17] == tc.ly);
    dm_tc_end_at(&tc, 0, 100.1); read_twice(a);
    REQUIRE(tc.lx == 0 && tc.ly == 0);
    REQUIRE(a[DM_ANDROID_TOUCH_HUD + 16] == 0 && a[DM_ANDROID_TOUCH_HUD + 17] == 0);
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
    REQUIRE(observed_panel==0 && panel_calls==4 && map_hold==1);
    dm_android_touch_uimsg("PanelManager","UnloadAll","");
    REQUIRE(tc.navigation_mode==0 && observed_panel==0 && panel_calls==5 && map_hold==0);
}

/* 游戏视图矩形：dm_android_touch_resize 给整屏（安全区也按整屏给），dm_android_touch_view 给视图矩形；dm_tc 的布局空间是视图尺寸，
 * 触点是窗口 dp，入队前减视图原点（真全屏下 y 可为负）；安全区只算落在视图里的那部分，并按视图高换算成 2160 高的布局单位。
 * Game view rectangle: dm_android_touch_resize gives the whole screen (safe area included) and dm_android_touch_view the view rectangle;
 * dm_tc's layout space is the view size, touches are window dp and get the view origin subtracted before queueing (y may be negative in
 * true fullscreen); only the part of the safe area inside the view counts, converted to 2160-high layout units by the view height. */
static void test_android_view_rect_coordinates(void) {
    fixture(0); float a[DM_ANDROID_TOUCH_DRAW_FLOATS];
    dm_android_touch_resize(1000, 500, 0, 0, 0);
    REQUIRE(width == 1000 && height == 500 && vr_x == 0 && vr_y == 0 && vr_w == 1000 && vr_h == 500);   /* 没给视图矩形时 = 整屏 | view = whole window until a view rect arrives */
    REQUIRE(resizing == 1 && cancel == 1);
    dm_android_touch_view(100, 150, 800, 350);
    REQUIRE(width == 800 && height == 350 && vr_x == 100 && vr_y == 150 && vr_w == 800 && vr_h == 350 && resizing == 1);
    dm_android_touch_view(100, 150, 0, 350);   /* 非法宽度不认 | an invalid width is ignored */
    REQUIRE(vr_w == 800 && width == 800);
    queue_sample(); REQUIRE(resizing == 0);
    REQUIRE(tc.layout.width == 800 && tc.layout.height == 350);   /* 布局空间 = 视图尺寸，不是整屏 | layout space = view size, not the whole screen */
    read_twice(a); REQUIRE(a[0] == 800 && a[1] == 350);           /* 绘图快照头同样是视图尺寸 | the drawing snapshot header is the view size too */
    dm_android_touch_event(0, 0, 500, 300);   /* 窗口 dp → 视图坐标：(500,300) − (100,150) | window dp → view coordinates */
    REQUIRE(tail - head == 1 && queue[(tail - 1) % 256].x == 400 && queue[(tail - 1) % 256].y == 150);
    dm_android_touch_event(1, 0, 500, 50);    /* 视图上方那条 3D：y = −100，允许（点怪 / 点地照常） | the 3D strip above the view: y = −100, allowed (tapping monsters / ground as usual) */
    REQUIRE(tail - head == 2 && queue[(tail - 1) % 256].x == 400 && queue[(tail - 1) % 256].y == -100);
    dm_android_touch_event(3, 0, 500, 50); queue_sample(); REQUIRE(head == tail);
    /* 没有底边安全区：视图左下那一点起的是摇杆。 | Without a bottom inset the same point in the view's bottom-left starts the stick (not DM_TC_EDGE). */
    dm_android_touch_event(0, 1, 300, 480); queue_sample();
    REQUIRE(tc.finger[1].active && tc.finger[1].target == DM_TC_JOYSTICK);
    dm_android_touch_event(3, 1, 300, 480); queue_sample();
    /* 安全区：左边 60 全落在视图左边距（100）里 → 0；右边 150 减视图右边距 100 → 50；底边 40 贴视图底 → 40。
     * Safe area: left 60 falls entirely in the view's left margin (100) → 0; right 150 minus the view's right margin 100 → 50; bottom 40, the view reaches the window bottom → 40. */
    dm_tc_target main0 = tc.layout.target[DM_TC_MAIN]; float joy0 = tc.layout.joy_x;
    dm_android_touch_resize(1000, 500, 60, 150, 40);
    queue_sample();
    REQUIRE(vr_x == 100 && vr_w == 800);   /* resize 不丢掉已给的视图矩形 | resize keeps the view rect that was given */
    REQUIRE(seen_safe_left == 0 && seen_safe_right == (int)lroundf(50 * 2160.0f / 350));
    REQUIRE(fabsf(main0.x - tc.layout.target[DM_TC_MAIN].x - 50) < 0.01f);   /* 右侧安全区 50 把技能组整体左移 | the right inset of 50 moves the skill group left */
    REQUIRE(tc.layout.joy_x == joy0);                                      /* 左侧安全区被视图裁成 0，摇杆不动 | left inset clipped to 0, the stick does not move */
    dm_android_touch_event(0, 1, 300, 480); queue_sample();               /* 视图 (200,330)，离视图底 20 < 40 | view (200,330), 20 above the view's bottom < 40 */
    REQUIRE(tc.finger[1].active && tc.finger[1].target == DM_TC_EDGE);
    dm_android_touch_event(3, 1, 300, 480); queue_sample();
}

/* 点世界换算：nx,ny 仍是视图归一化（宿主按界面矩形归一化），wx,wy 是出图画面归一化。真全屏局内画面铺满整窗，世界矩形 = 整窗在视图坐标里的矩形；
 * 不是真全屏时世界矩形 = 视图矩形，与改动前完全一致。两个入口都走：requests（界面消息那侧）与 direct_event（小地图直点）。
 * World-tap normalisation: nx,ny stay view-normalised (the host normalises by the UI rectangle) and wx,wy are picture-normalised. In a
 * true-fullscreen run the picture fills the whole window, so the world rectangle is the whole window in view coordinates; without true
 * fullscreen it is the view rectangle, exactly as before. Both entries are covered: requests and direct_event. */
static void test_android_world_tap_fullscreen(void) {
    fixture(0); dm_android_touch_resize(1000, 500, 0, 0, 0); dm_android_touch_view(100, 150, 800, 350); queue_sample();
    test_full = 1; world_tap_expected = 1;
    tc.world_tap_x = 500; tc.world_tap_y = 200; ++tc.world_tap_seq; requests();   /* 视图 (500,200) = 窗口 (600,350) | view (500,200) = window (600,350) */
    REQUIRE(fabsf(tap_ux - 500.0f / 800) < 1e-5f && fabsf(tap_uy - 200.0f / 350) < 1e-5f);   /* 界面矩形归一化不变 | UI-rectangle normalisation unchanged */
    REQUIRE(fabsf(tap_wx - 600.0f / 1000) < 1e-5f && fabsf(tap_wy - 350.0f / 500) < 1e-5f);  /* 整窗归一化 | window normalisation */
    test_full = 0;                                                                          /* 普通手机：世界 = 视图，与改动前一致 | ordinary phone: world = view, as before */
    ++tc.world_tap_seq; requests();
    REQUIRE(fabsf(tap_wx - 500.0f / 800) < 1e-5f && fabsf(tap_wy - 200.0f / 350) < 1e-5f);
    test_full = 1; direct[0] = (direct_gesture){2, 500, 250, 100.0, 0};                       /* 小地图那一下：视图 (500,250) = 窗口 (600,400) | the minimap tap: view (500,250) = window (600,400) */
    dm_android_touch_event(2, 0, 600, 400); queue_sample();
    REQUIRE(map_world_pending == 1);
    REQUIRE(fabsf(map_wx - 600.0f / 1000) < 1e-5f && fabsf(map_wy - 400.0f / 500) < 1e-5f);
}

static void run_case(const char *name, void (*body)(void)) {
    case_name = name; body(); ++cases; printf("PASS %s\n", name);
}

int main(void) {
    puts("SCOPE native_layout_and_input_bridge; no_Java_Canvas_no_guest_no_save_access");
    run_case("idle_185_abi", test_idle_and_abi);
    run_case("stick_deflection", test_stick_deflection);
    run_case("press_release_multitouch", test_press_release_and_multitouch);
    run_case("slide_cancelled_finger", test_slide_cancel);
    run_case("release_pulse_highlight", test_release_pulse);
    run_case("swap_and_delayed_pulse", test_swap_and_delayed_pulse);
    run_case("difficulty_hit_and_close", test_difficulty_hit_and_close);
    run_case("belt_diff_target_no_false_highlight", test_belt_diff_target_does_not_light_difficulty);
    run_case("navigation_and_resize", test_navigation_and_resize);
    run_case("android_view_rect_coordinates", test_android_view_rect_coordinates);
    run_case("android_world_tap_fullscreen", test_android_world_tap_fullscreen);
    run_case("android_queue_ten_short_taps", test_android_queue_ten_short_taps);
    run_case("android_focus_cancel", test_android_focus_cancel);
    run_case("android_cancelled_latch", test_android_cancelled_latch);
    run_case("android_swap_release_cancel", test_android_swap_release_cancel);
    run_case("android_panel_observation", test_android_panel_observation);
    printf("RESULT PASS cases=%u assertions=%u; game_quick_taps_10_of_10=NOT_TESTED\n", cases, checks);
    return 0;
}
