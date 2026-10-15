#include "../host/dm.h"
#include "../host/front.h"
#include "../host/dm_input.h"
#include "input_backend.h"
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
void dm_android_input_cancel(void) {
    pthread_mutex_lock(&input_lock);
    memset(pointer_slots, 0, sizeof pointer_slots); memset(&actions, 0, sizeof actions);
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
    actions.pressed |= buttons & ~actions.buttons; actions.buttons = buttons;
    actions.lx = lx; actions.ly = ly; actions.rx = rx; actions.ry = ry; status.action_publications++;
    pthread_mutex_unlock(&input_lock); return 0;
}
int dm_android_input_read_npad(dm_android_npad_state *out, uint32_t id, uint32_t style) {
    if (!out || !valid_id(id) || (style != 1 && style != 2)) return EINVAL;
    pthread_mutex_lock(&input_lock);
    if (!status.npad_initialized) { pthread_mutex_unlock(&input_lock); return ENODEV; }
    memset(out, 0, sizeof *out); out->sampling = (int64_t)++status.npad_samples;
    if (style_locked(id) & style) {
        out->buttons = actions.buttons | actions.pressed; actions.pressed = 0;
        out->lx = actions.lx; out->ly = actions.ly; out->rx = actions.rx; out->ry = actions.ry;
        out->attributes = 1; sampled_rx = out->rx; sampled_ry = out->ry;
        if (out->lx || out->ly) last_stick_activity = now_ns();
    }
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
