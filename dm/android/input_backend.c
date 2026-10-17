#include <stdatomic.h>
#include "../host/dm.h"
#include "../host/front.h"
#include "../host/dm_input.h"
#include "input_backend.h"
#include "ui_state_android.h"
#include <errno.h>
#include <math.h>
#include <stddef.h>

_Static_assert(sizeof(dm_android_npad_state) == 0x28, "Npad ABI size");
_Static_assert(offsetof(dm_android_npad_state, buttons) == 8, "Npad ABI buttons");
_Static_assert(offsetof(dm_android_npad_state, attributes) == 0x20, "Npad ABI attributes");
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static dm_android_input_config configuration;
static char input_source[128];
static dm_android_input_status status;
static struct { int active; uint32_t id; } pointer_slots[16];
/* 面板态"挂起抬起"用：驱动界面点击的那根手指 | for the deferred lift in panel state: the finger driving the UI click */
static int32_t ui_id = -1; static float ui_x, ui_y;
static struct { uint64_t buttons, pressed; int32_t lx, ly, rx, ry; } actions;
static int32_t sampled_rx, sampled_ry;
static uint64_t last_stick_activity;
int touch_fill(void *out, uint64_t time_ns);
void dm_touch_cancel(void);
static int valid_id(uint32_t id) { return id <= 7 || id == 0x10 || id == 0x20; }
static int ready_locked(void) { return status.bound && configuration.source_ready(configuration.opaque) == 1; }
static uint32_t style_locked(uint32_t id) {
    if (!status.npad_initialized || !ready_locked() || id != configuration.logical_npad_id) return 0;
    int accepted = 0;
    for (uint32_t i = 0; i < status.requested_id_count; i++) accepted |= status.requested_ids[i] == id;
    if (!accepted) return 0;
    // No physical Joy-Cons are attached by this logical action backend.
    // libnx's activation mode 2 means zero attached Joy-Cons are required;
    // 0/1 require two/one and therefore cannot expose this logical Handheld.
    if (id == 0x20 && status.handheld_activation_mode != 2) return 0;
    return configuration.logical_npad_style & status.requested_styles;
}
int dm_android_input_bind(const dm_android_input_config *config) {
    if (!config || !config->source || !*config->source || strlen(config->source) >= sizeof input_source || !config->source_ready ||
        config->panel_width != 1280 || config->panel_height != 720 ||
        (config->logical_npad_style && config->logical_npad_style != 1 && config->logical_npad_style != 2) ||
        !valid_id(config->logical_npad_id) ||
        (config->logical_npad_style == 2 && config->logical_npad_id != 0x20) ||
        (config->logical_npad_style == 1 && config->logical_npad_id == 0x20)) return EINVAL;
    if (config->source_ready(config->opaque) != 1) return ENODEV;
    pthread_mutex_lock(&input_lock);
    if (status.bound) { pthread_mutex_unlock(&input_lock); return EALREADY; }
    configuration = *config;
    strlcpy(input_source, config->source, sizeof input_source); configuration.source = input_source;
    status.bound = 1; status.requested_styles = 3; status.handheld_activation_mode = 0;
    status.requested_ids[0] = configuration.logical_npad_id; status.requested_id_count = 1;
    pthread_mutex_unlock(&input_lock);
    LOG("ANDROID_INPUT_BIND source=%s panel=1280x720 logical_npad_style=%u logical_npad_id=%u physical_switch_controller=false",
        input_source, config->logical_npad_style, config->logical_npad_id);
    return 0;
}
int dm_android_input_publish_touch(const dm_android_pointer *points, size_t count) {
    if (count > 16 || (!points && count)) return EINVAL;
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(points[i].x) || !isfinite(points[i].y)) return EINVAL;
        for (size_t j = 0; j < i; j++) if (points[i].id == points[j].id) return EINVAL;
    }
    /* P2 直接点击（对齐 iOS front_ios.m:177/505）：局外页面触摸 = 原生界面鼠标点击（dm_ui_touch，游戏自己命中与执行），
     * 面板态并行（点击 + 触摸屏）；游戏态照旧只走触摸屏。只有"第一根手指"驱动界面点击（同 iOS 的 _uiFinger）。
     * 局外不再推触摸屏（iOS: frontend 分支直接 return，不 pushFrom），免得同一根手指又当鼠标又当屏。 */
    { void dm_ui_touch(int, float, float);
      int fe = dm_android_ui_frontend(), nm = dm_android_ui_panel_mode();
      int ui_click = fe || nm;
      if (!ui_click) { ui_id = -1; }
      else if (count == 0) {
          if (ui_id >= 0) {
              if (fe) { dm_ui_touch(2, ui_x, ui_y); ui_id = -1; }   /* 局外：即点即抬 | front-end: press+lift immediately */
              /* 面板态：抬起挂起（触摸层按"这是不是物品区"的分类收尾：不是物品才发抬=点击；是物品/页签发作废） | panel state: defer the lift until the classification answer arrives */
          }
      }
      else {
          const dm_android_pointer *p = NULL;
          for (size_t i = 0; i < count; i++) if ((int32_t)points[i].id == ui_id) p = &points[i];
          if (p) { ui_x = p->x; ui_y = p->y; dm_ui_touch(1, ui_x, ui_y); }
          else if (ui_id < 0) { ui_id = (int32_t)points[0].id; ui_x = points[0].x; ui_y = points[0].y; dm_ui_touch(0, ui_x, ui_y); }
          else if (fe) { dm_ui_touch(2, ui_x, ui_y); ui_id = -1; }
          /* 面板态：抬起挂起（同上） | panel state: deferred (see above) */
      }
      if (fe) return 0;   /* 局外：只走界面点击，不推触摸屏 | front-end: UI clicks only, no touchscreen push */
    }
    pthread_mutex_lock(&input_lock);
    if (!status.bound) { pthread_mutex_unlock(&input_lock); return ENODEV; }
    if (!ready_locked() && count) { pthread_mutex_unlock(&input_lock); return EAGAIN; }
    for (size_t slot = 0; slot < 16; slot++) {
        int present = 0;
        for (size_t i = 0; i < count; i++) present |= pointer_slots[slot].active && points[i].id == pointer_slots[slot].id;
        if (!present) pointer_slots[slot].active = 0;
    }
    dm_touch touches[16];
    for (size_t i = 0; i < count; i++) {
        size_t slot = 0;
        while (slot < 16 && (!pointer_slots[slot].active || pointer_slots[slot].id != points[i].id)) slot++;
        if (slot == 16) { slot = 0; while (pointer_slots[slot].active) slot++; pointer_slots[slot].active = 1; pointer_slots[slot].id = points[i].id; }
        touches[i] = (dm_touch){.id = (uint32_t)slot, .x = fminf(fmaxf(points[i].x, 0), 1), .y = fminf(fmaxf(points[i].y, 0), 1)};
    }
    dm_touch_set(touches, (int)count); status.touch_publications++;
    pthread_mutex_unlock(&input_lock); return 0;
}
/* 面板态挂起的界面抬起：cancel=1 → 作废（相位 3），否则正常抬（相位 2）；由触摸层在分类结果到达后调。 */
void dm_android_input_ui_up(int cancel) {
    void dm_ui_touch(int, float, float);
    if (ui_id < 0) return;
    dm_ui_touch(cancel ? 3 : 2, ui_x, ui_y);
    ui_id = -1;
}
void dm_android_input_cancel(void) {
    pthread_mutex_lock(&input_lock);
    memset(pointer_slots, 0, sizeof pointer_slots); memset(&actions, 0, sizeof actions);
    ui_id = -1;
    dm_touch_cancel();
    pthread_mutex_unlock(&input_lock);
}
int dm_android_input_initialize_touch(void) {
    pthread_mutex_lock(&input_lock);
    int result = ready_locked() ? 0 : ENODEV;
    if (!result) status.touch_initialized = 1;
    pthread_mutex_unlock(&input_lock); return result;
}
int dm_android_input_initialize_npad(void) {
    pthread_mutex_lock(&input_lock);
    int result = ready_locked() ? 0 : ENODEV;
    if (!result) status.npad_initialized = 1;
    pthread_mutex_unlock(&input_lock); return result;
}
int dm_android_input_set_ids(const uint32_t *ids, size_t count) {
    if (count > 10 || (!ids && count)) return EINVAL;
    for (size_t i = 0; i < count; i++) { if (!valid_id(ids[i])) return EINVAL; for (size_t j = 0; j < i; j++) if (ids[i] == ids[j]) return EINVAL; }
    pthread_mutex_lock(&input_lock);
    if (!status.bound) { pthread_mutex_unlock(&input_lock); return ENODEV; }
    if (count) memcpy(status.requested_ids, ids, count * sizeof *ids);
    status.requested_id_count = (uint32_t)count;
    pthread_mutex_unlock(&input_lock); return 0;
}
int dm_android_input_set_styles(uint32_t styles) {
    pthread_mutex_lock(&input_lock);
    if (!status.bound) { pthread_mutex_unlock(&input_lock); return ENODEV; }
    status.requested_styles = styles;
    pthread_mutex_unlock(&input_lock); return 0;
}
int dm_android_input_set_handheld_mode(int mode) {
    if (mode < 0 || mode > 2) return EINVAL;
    pthread_mutex_lock(&input_lock);
    if (!status.bound) { pthread_mutex_unlock(&input_lock); return ENODEV; }
    status.handheld_activation_mode = mode;
    pthread_mutex_unlock(&input_lock); return 0;
}
int dm_android_input_get_style(uint32_t id, uint32_t *style) {
    if (!style || !valid_id(id)) return EINVAL;
    pthread_mutex_lock(&input_lock);
    if (!status.npad_initialized) { pthread_mutex_unlock(&input_lock); return ENODEV; }
    *style = style_locked(id); pthread_mutex_unlock(&input_lock); return 0;
}
int dm_android_input_publish_actions(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (buttons & ~0xffffULL || lx < -32767 || lx > 32767 || ly < -32767 || ly > 32767 || rx < -32767 || rx > 32767 || ry < -32767 || ry > 32767) return EINVAL;
    pthread_mutex_lock(&input_lock);
    if (!ready_locked() || !configuration.logical_npad_style) { pthread_mutex_unlock(&input_lock); return ENODEV; }
    { extern _Atomic int g_dm_l_presses; uint64_t prev = actions.buttons;
      if ((buttons & DM_BTN_L) && !(prev & DM_BTN_L)) atomic_fetch_add(&g_dm_l_presses, 1); }   /* 首领掉落读数用（dm_bossrush.c） */
    actions.pressed |= buttons & ~actions.buttons; actions.buttons = buttons;
    actions.lx = lx; actions.ly = ly; actions.rx = rx; actions.ry = ry; status.action_publications++;
    pthread_mutex_unlock(&input_lock); return 0;
}
void dm_ui_frontend_pump(void); void dm_ui_mouse_pump(void); void dm_cursor_apply(uint64_t *, int32_t *, int32_t *); void dm_aim_cone_pump(void);
void dm_actions_pump(void); void dm_world_pump(void); uint64_t dm_swapskill_filter(uint64_t);
int dm_android_input_read_npad(dm_android_npad_state *out, uint32_t id, uint32_t style) {
    if (!out || !valid_id(id) || (style != 1 && style != 2)) return EINVAL;
    /* 每次取手柄：先跑每拍泵（界面/商人/世界点按/鼠标），再取本拍值，最后改键（副手换手过滤 → 光标改写）
     * （顺序对照 hle_hid.c fill()：frontend → actions → world → mouse_pump → … → swapskill_filter → cursor_apply → aim_cone）。 */
    dm_ui_frontend_pump(); dm_actions_pump(); dm_world_pump(); dm_ui_mouse_pump();
    { void dm_update_pump(void); dm_update_pump(); }   /* P8d：主菜单构建号 / 新版本提示（没 init 时不出声）*/
    pthread_mutex_lock(&input_lock);
    if (!status.npad_initialized) { pthread_mutex_unlock(&input_lock); return ENODEV; }
    memset(out, 0, sizeof *out); out->sampling = (int64_t)++status.npad_samples;
    if (style_locked(id) & style) {
        out->buttons = actions.buttons | actions.pressed; actions.pressed = 0;
        out->lx = actions.lx; out->ly = actions.ly; out->rx = actions.rx; out->ry = actions.ry;
        out->attributes = 1; sampled_rx = out->rx; sampled_ry = out->ry;
        if (out->lx || out->ly) last_stick_activity = now_ns();
        out->buttons = dm_swapskill_filter(out->buttons);
        dm_cursor_apply(&out->buttons, &out->lx, &out->ly);
    }
    dm_aim_cone_pump();
    static uint64_t witnessed_buttons;
    static int32_t witnessed_axes[4];
    static unsigned receipt_lines;
    int32_t current_axes[4]={out->lx,out->ly,out->rx,out->ry};
    if(receipt_lines<256 && (status.npad_samples==1 || out->buttons!=witnessed_buttons || memcmp(current_axes,witnessed_axes,sizeof current_axes))) {
        LOG("ANDROID_NPAD_RETURN sample=%llu id=%u style=%u attributes=%u buttons=%#llx axes=%d,%d,%d,%d publications=%llu",
            (unsigned long long)status.npad_samples,id,style,out->attributes,(unsigned long long)out->buttons,
            out->lx,out->ly,out->rx,out->ry,
            (unsigned long long)status.action_publications);
        witnessed_buttons=out->buttons;
        memcpy(witnessed_axes,current_axes,sizeof current_axes);receipt_lines++;
    }
    pthread_mutex_unlock(&input_lock); return 0;
}
int dm_android_input_read_touch(void *out, int capacity) {
    if (capacity < 0 || (capacity && !out)) return -EINVAL;
    if (!capacity) return 0;
    pthread_mutex_lock(&input_lock);
    if (!status.touch_initialized) { pthread_mutex_unlock(&input_lock); return -ENODEV; }
    if (!ready_locked()) { memset(pointer_slots, 0, sizeof pointer_slots); dm_touch_cancel(); }
    touch_fill(out, guest_ns()); status.touch_samples++;
    pthread_mutex_unlock(&input_lock); return 1;
}
void dm_android_input_status_get(dm_android_input_status *out) {
    if (!out) return;
    pthread_mutex_lock(&input_lock); *out = status; pthread_mutex_unlock(&input_lock);
}
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    int error = dm_android_input_publish_actions(buttons, lx, ly, rx, ry);
    if (error) die("Logical Android action publication rejected error=%d", error);
}
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    (void)buttons; (void)lx; (void)ly; (void)rx; (void)ry;
    die("Physical gamepad publication requires a registered Android controller backend");
}
uint64_t dm_input_samples(void) {
    pthread_mutex_lock(&input_lock); uint64_t samples = status.npad_samples; pthread_mutex_unlock(&input_lock); return samples;
}
void dm_input_sampled_right(int32_t *rx, int32_t *ry) {
    pthread_mutex_lock(&input_lock); *rx = sampled_rx; *ry = sampled_ry; pthread_mutex_unlock(&input_lock);
}
uint64_t dm_input_stick_activity(void) {
    pthread_mutex_lock(&input_lock); uint64_t time = last_stick_activity; pthread_mutex_unlock(&input_lock); return time;
}
