// 鸿蒙触控：iOS front_ios.m 的 DmMetalView（触点 → 手势层 dm_touch_controls.c → 宿主）与 touch_ui_message 逐段搬运，不另起一套。
// 差别只在平台外壳：UITouch 换成触点 id（ArkTS 传来）、CADisplayLink 换成 ArkTS 每帧调 dm_ohos_touch_tick、dispatch_async 换成原子量（下一拍在 UI 线程生效）。
// 绘图（DmTouchOverlay drawRect）在 ArkTS IosOverlay.ets，这里只给它一份状态快照（touch_ohos.h DM_OS_*）。
// 未搬：手柄模式（dm_gamepad.m）、小地图任务终点名牌（mapGoalSync）、秘境任务条（dm_rift_hud）、掉落震动、货架拖货跟手图（Metal 回放层专有）、测试脚本 / 截图 / 无障碍。
// HarmonyOS touch: a section-by-section port of iOS front_ios.m's DmMetalView (touches → gesture layer dm_touch_controls.c → host) and touch_ui_message, not a separate design.
// Only the platform shell differs: UITouch becomes a touch id (from ArkTS), CADisplayLink becomes ArkTS calling dm_ohos_touch_tick every frame, dispatch_async becomes atomics (applied on the UI thread next tick).
// Drawing (DmTouchOverlay drawRect) is in ArkTS IosOverlay.ets; this file only gives it a state snapshot (touch_ohos.h DM_OS_*).
// Not ported: controller mode (dm_gamepad.m), minimap quest goal plates (mapGoalSync), the Rift objective bar (dm_rift_hud), drop haptics, the vendor drag ghost (Metal replay layer only), test scripts / capture / accessibility.
#include "touch_ohos.h"
#include "../../host/dm.h"
#include "../../host/front.h"
#include "../../host/dm_input.h"
#include "../../host/dm_cursor.h"
#include "../../host/dm_actions.h"
#include "../../host/dm_hud.h"
#include "../../host/dm_world.h"
#include "../../host/dm_uiscreen.h"
#include "../../ios/Sources/dm_touch_controls.h"
#include "../../ios/Sources/dm_touch_game_state.h"
#include "../../ios/Sources/dm_touch_native_art.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

uint64_t now_ns(void);
void dm_tp_request(void);
int dm_padmode_busy(void);
uint32_t dm_padmode_rebuilds(void);
void dm_ui_key_pulse_alone(uint64_t mask, int frames);
int dm_swapskill_slot(int slot, int *charges);
int dm_hud_stack_msg(const char *act, const char *arg);
int dm_custom_get(const char *key);
uint64_t dm_tg_portal_mask(const dm_tg_snapshot *s, uint64_t n);
extern void (*g_dm_restart_cover_hook)(int);

static double media_time(void) { return now_ns() / 1e9; }   // CACurrentMediaTime：单调时钟 | CACurrentMediaTime: monotonic clock

// ---- 触点（UITouch 的替身）：id → 当前位置（窗口 vp）| Touches (stand-ins for UITouch): id → current position (window vp) ----
typedef struct { int id, live, ended; float x, y; } touch_t;
static touch_t g_t[32];
static touch_t *tfind(int id) { for (int i = 0; i < 32; i++) if (g_t[i].live && g_t[i].id == id) return &g_t[i]; return NULL; }
static touch_t *tadd(int id, float x, float y) { touch_t *t = tfind(id); if (!t) for (int i = 0; i < 32; i++) if (!g_t[i].live) { t = &g_t[i]; break; } if (!t) return NULL; *t = (touch_t){id, 1, 0, x, y}; return t; }
#define NOT (-1)

// ---- 视图状态（DmMetalView 的实例变量）| View state (DmMetalView's instance variables) ----
static float g_win_w, g_win_h, g_safe_l, g_safe_r, g_safe_b;   // 整个窗口 | whole window
static float g_vr[4];                                          // 游戏视图在窗口里的矩形 | game view rectangle in the window
#define BW g_vr[2]
#define BH g_vr[3]
static int _slot[16];   // 触点 id，NOT = 空 | touch id, NOT = empty
static dm_tc_state _controls;
static int g_ready;
static int _padMode, _feMode = 1, _navigation = -1;
static int _swallow[16], _nswallow;
static int _pendEnd = NOT; static double _pendEndTime, _pendWall; static int _pendFire, _finalAsked;
static int _uiFinger = NOT; static float _uiFeOx, _uiFeOy; static int _uiFeDrag, _uiDragOn;
static int _mapTouch = NOT, _bindTouch = NOT;
static uint64_t _bindKey; static uint32_t _selId; static double _statNext, _tipUntil, _addAt;
static unsigned _mapWorldSeq; static int _mapWorldAsked; static float _mapWorldPx, _mapWorldPy;
static float _mapP0x, _mapP0y, _bindP0x, _bindP0y; static double _mapT0;
static int _pulseTicking; static uint64_t _pulseEpoch, _pulseS0; static double _pulseT0, _pulseSeen, _pulseFree;
static unsigned _itemSeq, _relSeq; static int _relAsked, _itemAsked, _itemKind;
static uint64_t _modifierPublishEndNS, _modifierPublishEpoch;
// 覆盖层属性（DmTouchOverlay 的 property）| Overlay properties (DmTouchOverlay's properties)
static int ov_hidden = 1, ov_nav, ov_dialogOnly, ov_hudBlocked, g_dirty = 1;
static dm_hud_info ov_hud; static dm_tg_snapshot ov_native;
// 游戏线程 → UI 线程（iOS 的 dispatch_async）| Game thread → UI thread (iOS's dispatch_async)
static _Atomic int g_touch_frontend = 1, g_touch_navigation, g_hud_blocked, g_nav_pub, g_cover;
static _Atomic int g_focused = 1;
static int _labelsArmed, _labelsDone; static double _labelsAt;

static void loc(int id, float *x, float *y) { touch_t *t = tfind(id); *x = t ? t->x - g_vr[0] : 0; *y = t ? t->y - g_vr[1] : 0; }   // locationInView:self
static int touch_done(int id) { touch_t *t = tfind(id); return !t || t->ended; }

// 真全屏：世界那一侧按整窗算（front_ios.m world_rect / world_norm）| True fullscreen: the world side is computed on the whole window (front_ios.m world_rect / world_norm)
static void world_rect(float r[4]) {
    int x, y, w, h, rw = 0, rh = 0;
    r[0] = 0; r[1] = 0; r[2] = BW; r[3] = BH;
    if (!front_ohos_full()) return;
    dm_ui_rect(&x, &y, &w, &h);
    if (dm_screen_real(&rw, &rh) && h > 0 && h < rh) { r[0] = -g_vr[0]; r[1] = -g_vr[1]; r[2] = g_win_w; r[3] = g_win_h; }
}
static void world_norm(float x, float y, float *nx, float *ny) { float r[4]; world_rect(r); *nx = (x - r[0]) / r[2]; *ny = (y - r[1]) / r[3]; }

static void push_from(void) {
    dm_touch ts[16]; int n = 0;
    if (BW > 0 && BH > 0) for (int i = 0; i < 16; i++) if (_slot[i] != NOT) {
        float px, py; loc(_slot[i], &px, &py);
        ts[n++] = (dm_touch){(uint32_t)i, fminf(fmaxf(px / BW, 0), .99999f), fminf(fmaxf(py / BH, 0), .99999f)};
    }
    dm_touch_set(ts, n);
    if (_controls.pointer_valid) dm_cursor_set_screen(_controls.pointer_x, _controls.pointer_y, BW, BH); else dm_cursor_clear();
    dm_input_set(_controls.buttons, _controls.lx, _controls.ly, _controls.rx, _controls.ry);
    if (_controls.belt_request) { int br = _controls.belt_request; _controls.belt_request = 0;
        if (br == 1 || br == 2) dm_belt_expand(br == 1); else dm_belt_use(br & 0xf, br >> 4 & 0xf); }
    if (_controls.diff_request) { dm_hud_request_players(_controls.diff_request); _controls.diff_request = 0; }
    uint64_t publishEnd = _controls.portal_pending ? now_ns() : 0;
    if (_controls.portal_pending && _modifierPublishEpoch != _controls.gesture_epoch) { _modifierPublishEpoch = _controls.gesture_epoch; _modifierPublishEndNS = publishEnd; }
    if (!_controls.portal_phase) { _modifierPublishEpoch = 0; _modifierPublishEndNS = 0; }
    g_dirty = 1;
}
static void cancel_controls(void) {
    _pendEnd = NOT; _finalAsked = 0;
    for (int i = 0; i < 16; i++) _slot[i] = NOT; dm_tc_cancel_all(&_controls); push_from();
    if (_uiDragOn) { _uiDragOn = 0; dm_ui_drag_end(); }
    _mapTouch = _bindTouch = NOT;
}
static void restart_cover_hook(int mode) { atomic_store(&g_cover, mode); }

static void ensure_init(void) {
    if (g_ready) return;
    for (int i = 0; i < 16; i++) _slot[i] = NOT;
    dm_tc_set_tall_scale(0);
    dm_tc_init(&_controls, BW > 0 ? BW : 800, BH > 0 ? BH : 450); _controls.mouse_click_mode = 1;
    g_dm_restart_cover_hook = restart_cover_hook;
    g_ready = 1;
}

// layoutSubviews：视图尺寸 / 安全区变了就重排控件 | layoutSubviews: re-lay out the widgets when the view size / safe area changes
static void layout_view(void) {
    float vr[4]; front_ohos_view_rect(g_win_w, g_win_h, vr);
    // 安全区只算落在游戏视图里的那部分 | Only the part of the safe area that falls inside the game view counts
    float sl = fmaxf(0, g_safe_l - vr[0]), sr = fmaxf(0, g_safe_r - (g_win_w - vr[0] - vr[2])), sb = fmaxf(0, g_safe_b - (g_win_h - vr[1] - vr[3]));
    static float lsl = -1, lsr = -1, lsb = -1; int safeChanged = sl != lsl || sr != lsr || sb != lsb;
    memcpy(g_vr, vr, sizeof vr);
    if (safeChanged) { lsl = sl; lsr = sr; lsb = sb; dm_tc_set_safe_bottom(sb); dm_tc_set_safe_insets(sl, sr); }
    dm_tc_set_edge_bottom(0);
    if (BH > 0) { dm_ui_safe_left((int)lroundf(sl / BH * 2160)); dm_ui_safe_right((int)lroundf(sr / BH * 2160)); }
    if (BW > 0 && BH > 0 && (safeChanged || _controls.layout.width != BW || _controls.layout.height != BH)) {
        cancel_controls(); dm_tc_resize(&_controls, BW, BH); g_dirty = 1;
        LOG("前端：游戏视图 %.0fx%.0f 放在 (%.0f,%.0f)（窗口 %.0fx%.0f）", BW, BH, g_vr[0], g_vr[1], g_win_w, g_win_h);
    }
}
void dm_ohos_touch_screen(float w, float h, float l, float r, float b) {
    if (!(w > 0 && h > 0)) return;
    g_win_w = w; g_win_h = h; g_safe_l = l; g_safe_r = r; g_safe_b = b;
    ensure_init(); layout_view();
}

static void hud_blocked(int n) { ov_hudBlocked = n > 0; ov_dialogOnly = n < 0; dm_tc_set_joy_block(&_controls, n < 0); g_dirty = 1; }
static void frontend_mode(int fe) { _feMode = fe; ov_hidden = fe || _padMode; if (fe) { _pendEnd = NOT; _finalAsked = 0; for (int i = 0; i < 16; i++) _slot[i] = NOT; dm_tc_cancel_all(&_controls); push_from(); } g_dirty = 1; }
static void item_labels_on(void) {   // 掉落物名字默认常显（front_ios.m itemLabelsOn）| Dropped item names shown by default (front_ios.m itemLabelsOn)
    const char *e = getenv("DM_TOUCH_DEFAULTS");
    if (_labelsDone || _labelsArmed || (e && !strcmp(e, "0"))) return;
    _labelsArmed = 1; _labelsAt = media_time() + 1.5;
}
static void navigation_mode(int n) {
    if (_navigation == n) return;
    dm_tc_set_navigation_mode(&_controls, n); _navigation = n; atomic_store(&g_nav_pub, n);
    static int pad = -1; if (pad < 0) { const char *e = getenv("DM_TOUCH_NAV_PAD"); pad = e && e[0] == '1'; }
    uint32_t mask = n ? (dm_tc_nav_mask(pad && n != 2) | (n == 3 ? 1u << DM_TC_MENU : 0)) :
        (((1u << DM_TC_COUNT) - 1) & ~(1u << DM_TC_MAP));   // 游戏态不要右上地图键 | No top-right map button in game mode
    dm_tc_enable_targets(&_controls, mask); ov_nav = n != 0; push_from();
    if (!n) item_labels_on();
    dm_ui_map_hold(n != 0);   // 同 iOS：面板开着全屏地图先让成小地图，关掉回到玩家要的那档 | as iOS: while a panel is open the full-screen map steps aside to the minimap, and after it closes it returns to what the player wants
}
// 游戏线程发来的状态在 UI 线程落地（dispatch_async 的替身）| Apply state from the game thread on the UI thread (stand-in for dispatch_async)
static void apply_async(void) {
    static int hb = 0; int n = atomic_load(&g_hud_blocked); if (n != hb) { hb = n; hud_blocked(n); }
    int fe = atomic_load(&g_touch_frontend); if (fe != _feMode) frontend_mode(fe);
    navigation_mode(atomic_load(&g_touch_navigation));
}

static uint64_t skill_key_at(float px, float py, int *empty) {
    int i = dm_tc_hit(&_controls.layout, px, py);
    if ((i == DM_TC_MAIN || (i >= DM_TC_SKILL1 && i <= DM_TC_SKILL4)) && (_controls.enabled_targets & (1u << i))) { const dm_tc_target *t = &_controls.layout.target[i];
        dm_ta_glyph g = dm_ta_current_set(&ov_native, t->button, now_ns(), _controls.skill_set); *empty = g.status == DM_TA_EMPTY;
        return t->button | (_controls.skill_set ? DM_BTN_ZL : 0); }
    return 0;
}
static int minimap_rect(float tap[4]) {
    float r[4]; if (BW <= 0 || !dm_ui_minimap_rect(r)) return 0;
    float x0 = r[0] * BW, y0 = r[1] * BH, w = (r[2] - r[0]) * BW, h = (r[3] - r[1]) * BH, band = h / 4; if (band < 30) band = fminf(30, h * 0.4f);
    tap[0] = x0; tap[1] = y0; tap[2] = w; tap[3] = band; return 1;
}
static int in_minimap(float px, float py) {
    float tap[4]; if (_navigation || !minimap_rect(tap)) return 0;
    int control = dm_tc_hit(&_controls.layout, px, py);
    if (control >= 0 || control == DM_TC_SWAP) return 0;
    return px >= tap[0] && px <= tap[0] + tap[2] && py >= tap[1] && py <= tap[1] + tap[3];
}
static void refresh_portal_binding(void) {
    int above = dm_custom_get("touch_swap_above") != 0;
    if (_controls.layout.swap_above != above) { cancel_controls(); dm_tc_set_swap_above(above); dm_tc_resize(&_controls, BW, BH); g_dirty = 1; }
    { dm_hud_info h; dm_hud_get(&h); h.x = h.y = 0; if (memcmp(&h, &ov_hud, sizeof h)) { ov_hud = h; g_dirty = 1; } }
    dm_tg_snapshot snapshot; memset(&snapshot, 0, sizeof snapshot);
    dm_touch_game_snapshot(&snapshot);
    // 只在图标会变时重画（setNativeMapping 的脏判）| Redraw only when the icons would change (setNativeMapping's dirty check)
    { static dm_ta_glyph prev[5]; static const uint64_t b[5] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y, DM_BTN_R}; uint64_t now = now_ns();
      for (int i = 0; i < 5; i++) { dm_ta_glyph g = dm_ta_current_set(&snapshot, b[i], now, _controls.skill_set);
          if (g.status != prev[i].status || g.raw_skill_id != prev[i].raw_skill_id || g.atlas != prev[i].atlas || g.normal_frame != prev[i].normal_frame || g.slot != prev[i].slot || g.effective_slot != prev[i].effective_slot) g_dirty = 1;
          prev[i] = g; } }
    ov_native = snapshot;
    if (_navigation) return;
    if (!snapshot.supported) return;
    uint64_t mask = 0; (void)dm_tg_portal_mask;   // 免费回城（dm_freetp.c）| Free town portal (dm_freetp.c)
    if (_controls.layout.target[DM_TC_PORTAL].button != mask || _controls.portal_binding_identity != snapshot.binding_identity || _controls.portal_scene_epoch != snapshot.scene_epoch) {
        dm_tc_set_portal_binding(&_controls, mask, snapshot.binding_identity, snapshot.scene_epoch);
        _modifierPublishEpoch = 0; _modifierPublishEndNS = 0; push_from();
    }
    if (_controls.portal_pending && mask && snapshot.ready && snapshot.secondary_group && snapshot.group_seen == 63 &&
        _modifierPublishEpoch == _controls.gesture_epoch && _modifierPublishEndNS &&
        snapshot.group_started_ns > _modifierPublishEndNS && snapshot.captured_ns > _modifierPublishEndNS &&
        dm_tc_portal_ack(&_controls, _modifierPublishEpoch, snapshot.binding_identity, snapshot.scene_epoch)) push_from();
}
static void bind_tap_sync(void) {
    float nx = 0, ny = 0;
    { static int was; int st = dm_ui_bind_active(); if (st == 2 && was != 2 && BW > 0) { _controls.pointer_valid = 1; _controls.pointer_x = 0.155f * BW; _controls.pointer_y = 0.5f * BH; } was = st; }
    int hk = dm_ui_bind_skill_hit(&nx, &ny); _controls.tap_button = 0; _controls.tap_move = 0;
    if (!hk) _selId = 0;
    if (hk) { _controls.tap_x = nx * BW; _controls.tap_y = ny * BH;
        if (hk == 1 && dm_ui_bind_active() == 2) _controls.tap_button = DM_BTN_Y;
        else if (!dm_ui_bind_active()) _controls.tap_move = 1; }
}

// ---- 脉冲收回（armPulseEnd / pulseTick；iOS 每 1/120 秒排一次，这里每帧 tick 跑）| Pulse withdrawal (armPulseEnd / pulseTick; iOS reschedules every 1/120 s, here it runs on every tick) ----
static void pulse_tick(void) {
    if (!_pulseTicking) return;
    uint64_t n = dm_input_samples(); double now = media_time();
    if (_controls.pulse_button) {
        if (_pulseEpoch != _controls.pulse_epoch) { _pulseEpoch = _controls.pulse_epoch; _pulseS0 = n; _pulseT0 = now; _pulseSeen = 0; _pulseFree = 0; }
        if (!_pulseSeen && n > _pulseS0) _pulseSeen = now;
        if (_pulseSeen && !_pulseFree && !dm_cursor_pending()) _pulseFree = now;
        double hold = _controls.pulse_hold_s;
        int taken = _pulseSeen && now - _pulseSeen >= 0.12 && n - _pulseS0 >= 3 && !dm_cursor_pending() && (hold <= 0 || (_pulseFree && now - _pulseFree >= hold));
        if (taken || now - _pulseT0 > 2.0 + hold) {
            if (!taken) LOG("[touch] 【红】脉冲 2 秒内游戏没取走（取了 %llu 次，扣着 %d 下），强制收回", (unsigned long long)(n - _pulseS0), dm_cursor_pending());
            if (dm_tc_pulse_end(&_controls, _pulseEpoch)) push_from();
            _pulseS0 = n; _pulseT0 = now;
        }
    } else if (dm_tc_pulse_pending(&_controls)) {
        if (n - _pulseS0 >= 2 || now - _pulseT0 > 0.5) { if (dm_tc_pulse_resume(&_controls)) push_from(); }
    } else _pulseTicking = 0;
}
static void arm_pulse_end(void) {
    if (_pulseTicking || (!_controls.pulse_button && !dm_tc_pulse_pending(&_controls))) return;
    _pulseTicking = 1; _pulseEpoch = 0; pulse_tick();
}

// ---- 前端页：第一根手指 = 界面点击，纵向拖 = 滚动（uiTouch:phase:）| Front-end pages: first finger = UI click, vertical drag = scroll (uiTouch:phase:) ----
static void ui_touch(int id, int ph) {
    if (BW <= 0 || BH <= 0) return;
    if (ph == 0 && _uiFinger == NOT) { _uiFinger = id; loc(id, &_uiFeOx, &_uiFeOy); _uiFeDrag = 0; }
    if (id != _uiFinger) return;
    float px, py; loc(id, &px, &py); float dy = fabsf(py - _uiFeOy);
    if (ph == 1 && !_uiFeDrag && !_uiDragOn && dy > 10 && dy > fabsf(px - _uiFeOx) && dm_ui_fe_drag_ok()) {
        _uiFeDrag = 1; _uiDragOn = 1; dm_ui_touch(3, 0, 0); dm_ui_drag_begin(1, _uiFeOx / BW, _uiFeOy / BH, 0);
        LOG("前端：纵向拖 → 滚动（起手 %.0f,%.0f），这次不点", _uiFeOx, _uiFeOy); }
    if (_uiFeDrag) { if (ph == 1) dm_ui_drag_move(px / BW, py / BH); else if (ph == 2) { _uiFeDrag = 0; _uiDragOn = 0; dm_ui_drag_end(); _uiFinger = NOT; } return; }
    dm_ui_touch(ph, px / BW, py / BH);
    if (ph == 2) _uiFinger = NOT;
}

static void sweep_ended_slots(void) {
    for (int i = 0; i < 16; i++) if (_slot[i] != NOT && _slot[i] != _pendEnd && touch_done(_slot[i])) {
        LOG("前端：【红】清理残留触摸槽 %d（控件层 %s）", i, _navigation ? "面板态" : "游戏态");
        if (_uiDragOn && _controls.pointer_slot == i) { _uiDragOn = 0; dm_ui_drag_end(); }
        dm_tc_cancel_finger(&_controls, i); _slot[i] = NOT; }
    if (_mapTouch != NOT && touch_done(_mapTouch)) _mapTouch = NOT;
    if (_bindTouch != NOT && touch_done(_bindTouch)) _bindTouch = NOT;
    if (_uiFinger != NOT && touch_done(_uiFinger)) { _uiFinger = NOT;
        if (_uiFeDrag || _uiDragOn) { _uiFeDrag = 0; _uiDragOn = 0; dm_ui_drag_end(); } else dm_ui_touch(3, 0, 0); }
}
static void finish_touch(int id, const char *phase);

static void touches_began(int id) {
    sweep_ended_slots();
    { if (dm_padmode_busy()) { if (_nswallow < 16) _swallow[_nswallow++] = id; return; } }
    if (atomic_load(&g_touch_frontend)) { ui_touch(id, 0); return; }
    refresh_portal_binding();
    if (_pendEnd != NOT) { int pt = _pendEnd; _pendFire = 1; finish_touch(pt, "ended"); _pendFire = 0; _pendEnd = NOT; _finalAsked = 0; }
    float qx, qy; loc(id, &qx, &qy);
    { int ex = 0; _controls.belt_rows = dm_belt_rows(&ex); _controls.belt_open = ex;
      if (dm_tc_belt_outside(&_controls, qx, qy)) { dm_belt_expand(0); LOG("前端：腰带展开着点别处 (%.0f,%.0f) → 只收腰带", qx, qy); return; } }
    if (!_navigation && _bindTouch == NOT && !dm_ui_bind_active()) { int em = 0; uint64_t bk = skill_key_at(qx, qy, &em); if (bk && em) { _bindTouch = id; _bindKey = bk; _bindP0x = qx; _bindP0y = qy; _mapT0 = media_time(); return; } }
    if (_mapTouch == NOT && in_minimap(qx, qy)) { _mapTouch = id; _mapP0x = qx; _mapP0y = qy; _mapT0 = media_time(); return; }
    for (int i = 0; i < 16; i++) if (_slot[i] == NOT) {
        _slot[i] = id; { int ex = 0; _controls.belt_rows = dm_belt_rows(&ex); _controls.belt_open = ex; } dm_tc_begin_at(&_controls, i, qx, qy, media_time());
        if (!_navigation && _controls.finger[i].target == DM_TC_PORTAL) { dm_tp_request(); LOG("前端：点回城钮 → 免费回城"); }
        if (_controls.navigation_mode && _controls.pointer_slot == i && BW > 0) { _itemSeq = dm_ui_item_query(qx / BW, qy / BH); _itemAsked = 1; _itemKind = -1; }
        break;
    }
    push_from();
}
static int swallowed(int id, int remove) {
    for (int i = 0; i < _nswallow; i++) if (_swallow[i] == id) { if (remove) _swallow[i] = _swallow[--_nswallow]; return 1; }
    return 0;
}
static void touches_moved(int id) {
    if (swallowed(id, 0)) return;
    if (atomic_load(&g_touch_frontend)) { ui_touch(id, 1); return; }
    if (_uiFinger == id) { ui_touch(id, 1); return; }
    for (int i = 0; i < 16; i++) if (_slot[i] == id) { float px, py; loc(id, &px, &py); dm_tc_move_at(&_controls, i, px, py, media_time());
        if (_controls.navigation_mode && _controls.pointer_slot == i && _controls.finger[i].ui_drag && BW > 0) { dm_tc_finger *f = &_controls.finger[i];
            if (!_uiDragOn) { _uiDragOn = 1; dm_ui_drag_lift(DM_TC_POINTER_LIFT_PT * dm_tc_scale_for_size(BW, BH) / BH); dm_ui_drag_begin(f->ui_drag, f->aim_origin_x / BW, f->aim_origin_y / BH, 44.0f / BH); }
            dm_ui_drag_move(px / BW, py / BH); }
        if (_controls.navigation_mode && _controls.pointer_slot == i && _controls.finger[i].pointer_dragged && BW > 0) { _relSeq = dm_ui_drop_query(px / BW, (py - DM_TC_POINTER_LIFT_PT * dm_tc_scale_for_size(BW, BH)) / BH, py / BH); _relAsked = 1; }
        break; }
    push_from();
    arm_pulse_end();
}
static void finish_touch(int id, const char *phase) {
    if (swallowed(id, 1)) return;
    if (_uiFinger == id) { ui_touch(id, 2); return; }
    float px, py; loc(id, &px, &py);
    if (id == _mapTouch) { _mapTouch = NOT;
        if (!strcmp(phase, "ended") && !_navigation && hypotf(px - _mapP0x, py - _mapP0y) <= 14 && media_time() - _mapT0 < 0.6) {
            if (dm_ui_map_is_full()) { dm_ui_map_full(0); LOG("前端：点小地图那一块 (%.0f,%.0f) → 回小地图", px, py); }
            else if (BW > 0) { float wx, wy; world_norm(px, py, &wx, &wy); _mapWorldSeq = dm_world_tap(wx, wy); _mapWorldAsked = 1; _mapWorldPx = px; _mapWorldPy = py; } }
        goto done; }
    if (id == _bindTouch) { _bindTouch = NOT;
        if (!strcmp(phase, "ended") && !_navigation && hypotf(px - _bindP0x, py - _bindP0y) <= 14) { dm_ui_bind_begin(_bindKey); LOG("前端：点空技能位 (%.0f,%.0f) → 给键 %#llx 选技能", px, py, (unsigned long long)_bindKey); }
        goto done; }
    if (!_pendFire && _pendEnd == NOT && !strcmp(phase, "ended") && _controls.navigation_mode && _controls.pointer_slot >= 0 && _slot[_controls.pointer_slot] == id && BW > 0) {
        dm_tc_finger *pf = &_controls.finger[_controls.pointer_slot]; int wait = 0;
        if (_itemAsked && dm_ui_item_result(_itemSeq) < 0) wait = 1;
        else if (pf->pointer_dragged) {
            dm_tc_move_at(&_controls, _controls.pointer_slot, px, py, media_time());
            _relSeq = dm_ui_drop_query(px / BW, (py - DM_TC_POINTER_LIFT_PT * dm_tc_scale_for_size(BW, BH)) / BH, py / BH); _relAsked = 1; _finalAsked = 1; wait = 1; }
        if (wait) { _pendEnd = id; _pendEndTime = media_time(); _pendWall = media_time(); return; }   // 触点记录留着，放行时再删 | keep the touch record until released
    }
    for (int i = 0; i < 16; i++) if (_slot[i] == id) {
        if (!strcmp(phase, "cancelled")) { dm_tc_finger *cf = &_controls.finger[i]; int drag = _controls.navigation_mode && _controls.pointer_slot == i && (cf->pointer_dragged || cf->shop_drag);
            if (_uiDragOn && _controls.pointer_slot == i) { _uiDragOn = 0; dm_ui_drag_end(); }
            if (drag) { int shop = cf->shop_drag; if (!shop) dm_tc_pointer_release(&_controls, 0); dm_tc_end_at(&_controls, i, media_time()); LOG("前端：拖动中被系统取消触摸 → %s", shop ? "不买不取" : "放回原处"); }
            else { dm_tc_cancel_finger(&_controls, i); LOG("前端：系统取消触摸（槽 %d）→ 不当松手", i); }
            if (_controls.pointer_slot < 0) { _relAsked = 0; _itemAsked = 0; }
            _slot[i] = NOT; break; }
        if (_controls.navigation_mode && _controls.pointer_slot == i && _controls.finger[i].shop_drag) { int rk = _relAsked ? dm_ui_item_result(_relSeq) : 0; if (rk < 0) rk = dm_ui_drop_last();
            if (rk == 5 && !dm_ui_drop_bag()) LOG("前端：%s拖出后松在背包格以外的物品区 → 不%s", _itemKind == 8 ? "一叠" : "货架上的货", _itemKind == 8 ? "取" : "买");
            else if ((rk == 5 || rk == 11) && BW > 0) { dm_tc_finger *sf = &_controls.finger[i];
                if (rk == 11) { dm_ui_stack_cube_at(sf->aim_origin_x / BW, sf->aim_origin_y / BH); LOG("前端：高级仓库里的一叠拖到魔盒格上松手 → 1 件进魔盒"); }
                else if (_itemKind == 8) { dm_ui_stack_take_at(sf->aim_origin_x / BW, sf->aim_origin_y / BH); LOG("前端：高级仓库里的一叠拖到背包松手 → 取 1 件"); }
                else { LOG("前端：货架上的货拖到背包松手 → 买"); dm_ui_shop_buy_at(sf->aim_origin_x / BW, sf->aim_origin_y / BH); }
                _controls.pointer_x = sf->aim_origin_x; _controls.pointer_y = sf->aim_origin_y; } }
        else if (_controls.navigation_mode && _controls.pointer_slot == i && _controls.finger[i].pointer_dragged && _relAsked) { int rr = dm_ui_item_result(_relSeq); if (rr < 0) rr = dm_ui_drop_last(); if (rr < 0) rr = 0; float dx, dy;
            if ((rr == 5 && dm_ui_drop_point(&dx, &dy)) || (rr == 0 && dm_ui_origin_point(&dx, &dy))) dm_tc_pointer_release_at(&_controls, dx * BW, dy * BH);
            else { if (rr == 7) { dm_shop_sell_cursor(); rr = 0; } dm_tc_pointer_release(&_controls, rr < 0 ? 1 : rr); } }
        _relAsked = 0;
        if (_controls.navigation_mode && _controls.pointer_slot == i && _itemAsked) { _itemAsked = 0; dm_tc_pointer_item(&_controls, dm_ui_item_result(_itemSeq)); bind_tap_sync(); }
        unsigned wt = _controls.world_tap_seq, mc = _controls.mouse_click_seq, tt = _controls.tab_tap_seq, us = _controls.use_seq, ids = _controls.ident_seq, sts = _controls.stack_take_seq, lps = _controls.long_press_seq, its = _controls.item_tap_seq;
        int dragged = _uiDragOn && _controls.pointer_slot == i;
        if (_uiDragOn && _controls.pointer_slot == i) { _uiDragOn = 0; dm_ui_drag_end(); }
        int wasPtr = _controls.pointer_slot == i; unsigned tm = _controls.tap_move_seq;
        if (_controls.finger[i].target == DM_TC_SWAP) dm_tc_move_at(&_controls, i, px, py, media_time());
        if (_pendFire) dm_tc_end_at(&_controls, i, _pendEndTime); else dm_tc_end_at(&_controls, i, media_time()); _slot[i] = NOT;
        if (_controls.tap_move_seq != tm) { uint32_t hid = dm_ui_hit_id();
            if (hid && hid == _selId) { if (hid <= 4) { dm_stat_add((int)hid - 1, 1); LOG("前端：再点同一个「+」（属性 %u）→ 加 1 点", hid - 1); }
                else _addAt = _controls.last_time_s + 0.3; }
            else { _selId = hid; LOG("前端：点加点类按钮 → 选中（标识 %#x）", hid); } }
        if (_controls.tab_tap_seq != tt) dm_ui_tab_go();
        if (_controls.long_press_seq != lps) dm_ui_long_press();
        if (_controls.item_tap_seq != its) dm_ui_item_tap();
        if (_controls.stack_take_seq != sts) { dm_stash_take(1); LOG("前端：双击高级仓库里的一叠 → 取 1 件"); }
        if (_controls.ident_seq != ids) dm_ui_identify_hovered();
        if (_controls.use_seq != us) dm_ui_use_item();
        if (_controls.world_tap_seq != wt && BW > 0) { float wx, wy; world_norm(_controls.world_tap_x, _controls.world_tap_y, &wx, &wy); dm_ui_world_tap(_controls.world_tap_x / BW, _controls.world_tap_y / BH, wx, wy); }
        int itemTap = _controls.item_tap_seq != its && !dragged && _controls.use_seq == us && _controls.ident_seq == ids && _controls.stack_take_seq == sts && _controls.long_press_seq == lps && _controls.mouse_click_seq == mc;
        if (wasPtr && _controls.navigation_mode) _tipUntil = (_controls.tap_move_seq != tm || itemTap) ? -1 : _controls.last_time_s + (_controls.mouse_click_seq != mc ? 0.05 : 2.8);
        if (_controls.mouse_click_seq != mc && BW > 0) { float nx = _controls.mouse_click_x / BW, ny = _controls.mouse_click_y / BH; dm_ui_touch(0, nx, ny); dm_ui_touch(2, nx, ny); }
        break; }
done:
    push_from();
    arm_pulse_end();
}

void dm_ohos_touch_event(int phase, int id, float x, float y) {
    if (phase < 0 || phase > 3 || !isfinite(x) || !isfinite(y)) return;
    ensure_init();
    if (atomic_load(&g_cover)) return;   // 一键重开遮罩吞掉触点 | The quick-restart cover swallows touches
    if (phase != 0 && !tfind(id)) return;
    if (!atomic_load(&g_focused)) return;
    apply_async();
    if (phase == 0) { if (!tadd(id, x, y)) return; touches_began(id); return; }
    touch_t *t = tfind(id); t->x = x; t->y = y;
    if (phase == 1) { touches_moved(id); return; }
    t->ended = 1;
    finish_touch(id, phase == 2 ? "ended" : "cancelled");
    if (_pendEnd != id) t->live = 0;
}
void dm_ohos_touch_focus(int active) {
    atomic_store(&g_focused, !!active);
    if (!active && g_ready) { for (int i = 0; i < 32; i++) g_t[i].live = 0; _nswallow = 0; _uiFinger = NOT; _uiFeDrag = 0; cancel_controls(); }
}

// ---- 每帧（tickTouchControls + refreshPortalBinding）| Every frame (tickTouchControls + refreshPortalBinding) ----
static void tick_touch_controls(double now) {
    { int hide = _feMode || _padMode || dm_padmode_busy(); if (ov_hidden != hide) { ov_hidden = hide; g_dirty = 1; } }
    dm_tc_set_tap_box(dm_ui_tap_box()); dm_tc_set_cur_stack(dm_ui_cur_stack_id()); dm_tc_set_hover_unid(dm_ui_hover_unid());
    { dm_hud_info hi; dm_hud_get(&hi); dm_tc_set_holding(&_controls, hi.valid && hi.holding && !dm_ui_targeting()); }
    if (_itemAsked && _controls.navigation_mode && _controls.pointer_slot >= 0) { int ir = dm_ui_item_result(_itemSeq); if (ir >= 0) { _itemAsked = 0; _itemKind = ir; dm_tc_pointer_item(&_controls, ir); } }
    if (_pendEnd != NOT) {
        int ready = !(_itemAsked && dm_ui_item_result(_itemSeq) < 0) && !(_finalAsked && dm_ui_item_result(_relSeq) < 0);
        if (ready || media_time() - _pendWall > 0.25) { int pt = _pendEnd;
            LOG("前端：抬手挂起 %.0f ms 后结束（%s）", (media_time() - _pendWall) * 1000, ready ? "答案已回" : "超时，按没答上处理");
            _pendFire = 1; finish_touch(pt, "ended"); _pendFire = 0; _pendEnd = NOT; _finalAsked = 0;
            touch_t *t = tfind(pt); if (t && t->ended) t->live = 0; }
    }
    bind_tap_sync();
    if (_controls.navigation_mode && !dm_ui_bind_active() && !_itemAsked && _controls.pointer_slot >= 0 && _controls.pointer_slot < DM_TC_MAX_TOUCH) { dm_tc_finger *f = &_controls.finger[_controls.pointer_slot]; float nx = 0, ny = 0; uint32_t hid = dm_ui_hit_id();
        if (f->active && !f->pointer_dragged && !f->ui_drag && now - f->start_time_s >= 0.5 && hypotf(f->x - f->aim_origin_x, f->y - f->aim_origin_y) <= 10 * dm_tc_scale_for_size(BW, BH) && dm_ui_bind_skill_hit(&nx, &ny) == 2 && hid >= 1 && hid <= 4) {
            if (!f->pointer_long) { f->pointer_long = 1; _statNext = now; _selId = hid; _controls.pointer_valid = 1; _controls.pointer_x = nx * BW; _controls.pointer_y = ny * BH; LOG("前端：按住「+」（属性 %u）→ 连续加点", hid - 1); }
            if (now >= _statNext) { _statNext = now + 0.15; dm_stat_add((int)hid - 1, 1); } } }
    if (_controls.navigation_mode && !dm_ui_bind_active() && !_itemAsked && _controls.pointer_slot >= 0 && _controls.pointer_slot < DM_TC_MAX_TOUCH) { dm_tc_finger *f = &_controls.finger[_controls.pointer_slot]; float nx = 0, ny = 0;
        if (_pendEnd == NOT && f->active && !f->pointer_dragged && !f->pointer_long && now - f->start_time_s >= 0.5 && hypotf(f->x - f->aim_origin_x, f->y - f->aim_origin_y) <= 10 * dm_tc_scale_for_size(BW, BH) && dm_ui_bind_skill_hit(&nx, &ny) == 1) {
            dm_tc_pointer_press(&_controls, nx * BW, ny * BH, DM_BTN_Y); LOG("前端：技能树里长按技能 (%.0f,%.0f) → 开绑定框", nx * BW, ny * BH); } }
    if (_mapWorldAsked) { int wr = dm_world_tap_result(_mapWorldSeq);
        if (wr >= 0) { _mapWorldAsked = 0; if (wr == 0 && !_navigation) { dm_ui_map_full(1); LOG("前端：点小地图那一块 (%.0f,%.0f) → 全屏地图", _mapWorldPx, _mapWorldPy); }
            else LOG("前端：点小地图那一块 (%.0f,%.0f) → 底下有世界目标（类 %d），交互、不切地图", _mapWorldPx, _mapWorldPy, wr); } }
    if (_addAt > 0 && now >= _addAt) { dm_points_info pi; dm_points_get(&pi);
        if (pi.tree_open && pi.sel_skill >= 0 && !dm_cursor_pending()) { _addAt = 0; if (pi.sel_block == 0 || pi.sel_block == 5) dm_skill_add_selected(); else LOG("前端：再点同一个技能 → 不能加（原因 %d），不发", pi.sel_block); }
        else if (now > _addAt + 1.0 || !pi.tree_open) _addAt = 0; }
    if (!_controls.navigation_mode || _padMode) _tipUntil = -2;
    else if (_controls.pointer_slot >= 0) _tipUntil = -1;
    else if (_tipUntil == -2) { if (dm_ui_free_cursor()) _tipUntil = now + 0.3; }
    else if (_tipUntil > 0 && now >= _tipUntil && !_controls.holding && !dm_ui_bind_active() && !dm_ui_touch_active() && !_controls.pulse_button && !dm_tc_pulse_pending(&_controls) && !dm_cursor_pending()) {
        _controls.pointer_valid = 1; _controls.pointer_x = 0.02f * BW; _controls.pointer_y = 0.5f * BH; _tipUntil = 0; }
    uint64_t tb = _controls.buttons;
    dm_tc_tick(&_controls, now);
    { int fg = 0; for (int i = 0; i < DM_TC_MAX_TOUCH; i++) fg |= _controls.finger[i].active; dm_ui_hand_idle(!fg && !_controls.pulse_button && !dm_tc_pulse_pending(&_controls) && !dm_cursor_pending() && !dm_ui_touch_active()); }
    if (_controls.buttons != tb && !_controls.pulse_button) push_from();
    else if (_controls.belt_request && !_controls.pulse_button) push_from();
    if (_controls.pulse_button && !_pulseTicking) { push_from(); arm_pulse_end(); }
    if (_controls.pointer_valid) dm_cursor_set_screen(_controls.pointer_x, _controls.pointer_y, BW, BH);
    else if (_controls.navigation_mode) dm_cursor_clear();
    if (_labelsArmed && media_time() >= _labelsAt) { _labelsArmed = 0; if (!_navigation && !_labelsDone) { _labelsDone = 1; dm_ui_key_pulse_alone(DM_BTN_L, 9); } }
}

// 外观按落指立即反馈（DmTouchOverlay button_held）| Visual feedback starts on contact (DmTouchOverlay button_held)
static int button_held(const dm_tc_state *s, int target) {
    uint64_t button = target >= 0 && target < DM_TC_COUNT ? s->layout.target[target].button : 0;
    if (button && (s->buttons & button) == button) return 1;
    for (int i = 0; i < DM_TC_MAX_TOUCH; i++) if (s->finger[i].active && !s->finger[i].aim_cancelled && s->finger[i].target == target) return 1;
    return 0;
}
// 技能钮画哪个图标（DmTouchOverlay shown_glyph：快照旧了照旧画）| Which icon a skill button draws (DmTouchOverlay shown_glyph: draw as before if the snapshot is stale)
static dm_ta_glyph shown_glyph(const dm_tg_snapshot *n, uint64_t button, uint64_t now, int set) {
    dm_ta_glyph g = dm_ta_current_set(n, button, now, set);
    if (g.status == DM_TA_UNKNOWN && n && n->captured_ns && now > n->captured_ns && now - n->captured_ns > DM_TG_MAX_AGE_NS) g = dm_ta_current_set(n, button, n->captured_ns, set);
    return g;
}
static void glyph_out(float *o, dm_ta_glyph g, int held, int ss, int charges) {
    o[0] = (float)g.status; o[1] = -1; o[2] = o[3] = o[4] = o[5] = 0; o[6] = (float)ss; o[7] = (float)charges;
    dm_ta_frame f;
    if (g.status == DM_TA_GLYPH && dm_ta_frame_at(g.atlas, held ? g.pressed_frame : g.normal_frame, &f)) { o[1] = (float)g.atlas; o[2] = (float)f.x; o[3] = (float)f.y; o[4] = (float)f.width; o[5] = (float)f.height; }
}

int dm_ohos_touch_tick(float *o, int cap) {
    if (!o || cap < DM_OS_FLOATS) return -1;
    ensure_init();
    { float vr[4]; front_ohos_view_rect(g_win_w, g_win_h, vr); if (memcmp(vr, g_vr, sizeof vr)) layout_view(); }
    apply_async();
    double now = media_time();
    tick_touch_controls(now);
    refresh_portal_binding();
    pulse_tick();
    { static int lc = -1; int c = atomic_load(&g_cover); if (c != lc) { lc = c; g_dirty = 1; } }
    if (!g_dirty) return 0;
    g_dirty = 0;
    const dm_tc_state *s = &_controls; const dm_tc_layout *l = &s->layout; uint64_t hostNow = now_ns();
    memset(o, 0, sizeof(float) * DM_OS_FLOATS);
    o[DM_OS_VISIBLE] = !ov_hidden; o[DM_OS_VX] = g_vr[0]; o[DM_OS_VY] = g_vr[1]; o[DM_OS_VW] = g_vr[2]; o[DM_OS_VH] = g_vr[3];
    o[DM_OS_NAV] = ov_nav; o[DM_OS_SNAV] = s->navigation_mode; o[DM_OS_DIALOG] = ov_dialogOnly; o[DM_OS_BLOCKED] = ov_hudBlocked; o[DM_OS_SET] = s->skill_set;
    o[DM_OS_MAPCANCEL] = s->map_cancel_active; o[DM_OS_DIFFOPEN] = s->diff_open; o[DM_OS_PLUSHELD] = (s->buttons & DM_BTN_PLUS) != 0;
    o[DM_OS_BARVIS] = dm_hud_bar_visible(&ov_hud, ov_nav && !ov_dialogOnly, ov_hudBlocked); o[DM_OS_DEAD] = ov_hud.valid && ov_hud.dead;
    o[DM_OS_JOYACT] = s->joy_slot >= 0; o[DM_OS_JOYX] = s->joy_slot >= 0 ? s->joy_origin_x : l->joy_x; o[DM_OS_JOYY] = s->joy_slot >= 0 ? s->joy_origin_y : l->joy_y; o[DM_OS_JOYR] = l->joy_radius;
    o[DM_OS_LX] = s->lx / 32767.0f; o[DM_OS_LY] = s->ly / 32767.0f;
    o[DM_OS_SWX] = l->swap.x; o[DM_OS_SWY] = l->swap.y; o[DM_OS_SWR] = l->swap.r; o[DM_OS_SWHELD] = button_held(s, DM_TC_SWAP);
    o[DM_OS_COVER] = atomic_load(&g_cover);
    for (int i = 0; i < DM_TC_COUNT; i++) { float *t = o + DM_OS_TARGETS + i * DM_OS_TSTRIDE;
        t[0] = l->target[i].x; t[1] = l->target[i].y; t[2] = l->target[i].r; t[3] = (s->enabled_targets >> i) & 1; t[4] = button_held(s, i); t[5] = (float)(l->target[i].button & 0xffff); }
    for (int i = 0; i <= DM_TC_SKILL4; i++) {
        int held = button_held(s, i), ssKind = 0, ssCharges = 0;
        dm_ta_glyph g = shown_glyph(&ov_native, l->target[i].button, hostNow, s->skill_set);
        if (g.status != DM_TA_EMPTY && g.status != DM_TA_UNKNOWN) ssKind = dm_swapskill_slot(g.slot, &ssCharges);
        glyph_out(o + DM_OS_GLYPHS + i * DM_OS_GSTRIDE, g, held, ssKind, ssCharges);
    }
    glyph_out(o + DM_OS_GLYPHS + 5 * DM_OS_GSTRIDE, dm_ta_skill(359), button_held(s, DM_TC_PORTAL), 0, 0);
    { static float last[6 * DM_OS_GSTRIDE]; float *g = o + DM_OS_GLYPHS;   // 技能钮读数变了记一行（排查用）| log one line when the skill button readings change (diagnostics)
      if (memcmp(last, g, sizeof last)) { memcpy(last, g, sizeof last);
          LOG("触控：技能钮读数 %s", "");
          for (int i = 0; i < 6; i++) LOG("  钮 %d：状态 %.0f 图集 %.0f 帧 (%.0f,%.0f %.0fx%.0f)%s", i, g[i * 8], g[i * 8 + 1], g[i * 8 + 2], g[i * 8 + 3], g[i * 8 + 4], g[i * 8 + 5],
              i < 5 ? "" : "（回城）"); } }
    { const dm_hud_info *h = &ov_hud; float *u = o + DM_OS_HUD;
      u[0] = h->valid; u[1] = h->hp; u[2] = h->hp_max; u[3] = h->mp; u[4] = h->mp_max; u[5] = h->level; u[6] = h->exp_permille; u[7] = h->points; u[8] = h->quest_unread;
      for (int i = 0; i < 4; i++) u[9 + i] = h->belt[i];
      u[13] = h->players; u[14] = h->dead; }
    return 1;
}

// ---- 界面消息（游戏线程；front_ios.m hud_stack_message / touch_ui_message）| UI messages (game thread; front_ios.m hud_stack_message / touch_ui_message) ----
static void hud_stack_message(const char *act, const char *arg) { atomic_store(&g_hud_blocked, dm_hud_stack_msg(act, arg)); }
void dm_ohos_touch_uimsg(const char *cat, const char *act, const char *arg) {
    if (!cat || !act) return; if (!arg) arg = "";
    if (!strcmp(cat, "PanelManager")) hud_stack_message(act, arg);
    static dm_tc_scene scene; static uint64_t sceneEpoch = 1, worldEpoch = 1;
    dm_tc_scene before = scene;
    dm_tc_set_panel_state(dm_ui_panel_state());
    int mode = dm_tc_navigation(&scene, cat, act, arg);
    if (scene.game && atomic_load(&g_hud_blocked) > 0) mode = 1;
    static int prevMode = 1;
    if (before.game != scene.game) ++worldEpoch;
    { static uint32_t rb0; uint32_t rb = dm_padmode_rebuilds(); if (rb != rb0) { rb0 = rb; ++worldEpoch; } }
    if (mode != prevMode) ++sceneEpoch;
    prevMode = mode;
    dm_touch_game_set_scene(!mode, sceneEpoch, worldEpoch);
    atomic_store(&g_touch_frontend, !scene.game);
    { void front_gtx_scene(int); int dm_loadscreen_up(void); front_gtx_scene(dm_loadscreen_up() ? 2 : scene.game ? 1 : 3); }   // OpenGTX 场景信息（含读取画面 = LOADING，同时驱动 FAST Kit 装载加速）| OpenGTX scene info (loading screen = LOADING; also drives the FAST Kit load hint)
    int nmode = dm_tc_panel_mode(mode, &scene, atomic_load(&g_hud_blocked));
    atomic_store(&g_touch_navigation, nmode);
    dm_uiscreen_set_panel(nmode == 1);
}
