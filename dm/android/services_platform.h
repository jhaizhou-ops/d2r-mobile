#pragma once
#include <stddef.h>
#include <stdint.h>

enum dm_android_platform_mode { DM_ANDROID_HOST_SHELL, DM_ANDROID_HOST_ACTIVITY };
typedef struct {
    enum dm_android_platform_mode mode;
    const char *profile_directory; // owned app-private directory, not game save contents
    const char *locale; // actual Configuration/Settings language tag, when property access is unavailable
    const char *locale_source; // e.g. Activity.Configuration or Settings.System.system_locales
    const int *add_on_content_indices;
    size_t add_on_content_count;
    int content_manifest_verified;
    int prefetch_present, audio_present, graphics_present;
    int (*stop_prefetch)(void);
    int (*pause_audio)(int);
    int (*suspend_and_drain_graphics)(int);
    int (*flush_diagnostics)(void);
} dm_android_platform_config;
typedef struct {
    int focus; // 1 focused, 2 unfocused, 3 background
    int surface_available;
    uint32_t width, height;
    float density, refresh_hz;
    int thermal_status; // -1 unknown, otherwise Android Thermal Status
    uint64_t generation;
} dm_android_window_state;
typedef struct {
    int bound, oe_initialized, offline_policy_initialized, parental_policy_initialized;
    int surface_available, online_requests_enabled, core_count;
    int account_initialized, content_initialized, error_recorder_ready;
    uint64_t physical_memory_bytes;
    char device_model[128], locale[32], locale_source[64];
} dm_android_platform_status;

int dm_android_platform_bind(const dm_android_platform_config *config);
int dm_android_platform_update_window(const dm_android_window_state *state);
void dm_android_platform_get_window(dm_android_window_state *state);
void dm_android_platform_get_status(dm_android_platform_status *status);
int dm_android_platform_oe_init(void *context);
int dm_android_platform_aoc_init(void *context);
int dm_android_platform_error_init(void *context);
int dm_android_platform_parental_init(void *context);
int dm_android_platform_abort_init(void *context);
int dm_android_platform_register_services(void);
int dm_android_platform_account_init(void);
int dm_android_platform_account_uid(uint64_t uid[2]);
int dm_android_platform_account_open(const uint64_t uid[2], uint64_t handle[3]);
int dm_android_platform_account_close(uint64_t handle[3]);
int dm_android_platform_account_handle_uid(const uint64_t handle[3], uint64_t uid[2]);
int dm_android_platform_account_open_count(void);
int dm_android_platform_account_nickname(const uint64_t uid[2], char nickname[33]);
void dm_android_platform_device_uuid(uint8_t uuid[16]);
uint64_t dm_android_platform_language_code(void);
int dm_android_platform_aoc_count(void);
int dm_android_platform_aoc_list(int *out, int offset, int capacity);
void dm_android_platform_record_error(uint32_t result, const char *context);
int dm_android_platform_network_init(int subsystem);
int dm_android_platform_network_finalize(int subsystem);
void dm_android_platform_network_request(int submit);
int dm_android_platform_network_ready(void);
void dm_android_platform_setting(const char *name, int value);
int dm_android_platform_setting_get(const char *name, int *value);
int dm_android_platform_shutdown(void);
// Side state belongs to the actual guest object address; lifecycle is explicit.
void dm_android_platform_object_create(void *object, int kind);
void dm_android_platform_object_destroy(void *object, int kind);
int dm_android_platform_object_exists(const void *object, int kind);
void dm_android_platform_object_ready(void *object, int kind);
int dm_android_platform_object_is_ready(const void *object, int kind);
int dm_android_platform_utf8_valid(const char *text);
int dm_android_platform_verify(const char *result_json);
