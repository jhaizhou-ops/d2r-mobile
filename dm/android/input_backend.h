#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t id; float x, y; } dm_android_pointer;
typedef struct {
    const char *source;
    uint32_t panel_width, panel_height;
    // Must read the actual installed Android listener/route readiness. This
    // callback must be nonblocking and must not call back into input_backend.
    int (*source_ready)(void *opaque);
    void *opaque;
    // 0 = no logical action channel (no connected Npad is reported).
    // 1 = FullKey; 2 = Handheld. These describe logical Android input, not a
    // claim that a physical Switch controller has been attached.
    uint32_t logical_npad_id, logical_npad_style;
} dm_android_input_config;
typedef struct {
    int64_t sampling;
    uint64_t buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes, reserved;
} dm_android_npad_state;
typedef struct {
    int bound, touch_initialized, npad_initialized;
    uint32_t requested_styles, requested_ids[10], requested_id_count;
    int handheld_activation_mode;
    uint64_t touch_publications, touch_samples, action_publications, npad_samples;
} dm_android_input_status;
// All configuration/publication/init methods return 0 or positive errno.
int dm_android_input_bind(const dm_android_input_config *config);
int dm_android_input_publish_touch(const dm_android_pointer *points, size_t count);
void dm_android_input_cancel(void);
int dm_android_input_publish_actions(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
int dm_android_input_initialize_touch(void);
int dm_android_input_initialize_npad(void);
int dm_android_input_set_ids(const uint32_t *ids, size_t count);
int dm_android_input_set_styles(uint32_t styles);
int dm_android_input_set_handheld_mode(int mode);
int dm_android_input_get_style(uint32_t id, uint32_t *style);
int dm_android_input_read_npad(dm_android_npad_state *out, uint32_t id, uint32_t style);
// read_touch returns number of states (0 or 1) or negative errno.
int dm_android_input_read_touch(void *out, int capacity);
void dm_android_input_status_get(dm_android_input_status *out);
