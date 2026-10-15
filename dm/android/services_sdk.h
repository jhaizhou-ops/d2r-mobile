#pragma once

// Backends are registered by the Android host only after their real platform
// implementations are connected. A missing backend is a startup error.
enum dm_android_sdk_service {
    DM_ANDROID_SERVICE_OE,
    DM_ANDROID_SERVICE_AOC,
    DM_ANDROID_SERVICE_ERROR_CONTEXT,
    DM_ANDROID_SERVICE_FS,
    DM_ANDROID_SERVICE_PARENTAL_CONTROL,
    DM_ANDROID_SERVICE_ABORT_OBSERVER,
    DM_ANDROID_SERVICE_COUNT
};

typedef int (*dm_android_service_init_fn)(void *context);
void dm_android_service_register(enum dm_android_sdk_service service,
                                dm_android_service_init_fn initialize,
                                void *context);
void dm_android_service_initialize(enum dm_android_sdk_service service);
