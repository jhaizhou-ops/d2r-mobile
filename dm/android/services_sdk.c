#include "../host/dm.h"
#include "services_sdk.h"

static const char *const service_names[] = {
    "oe", "aoc", "error-context", "fs", "parental-control", "abort-observer"
};
static struct service_backend {
    dm_android_service_init_fn initialize;
    void *context;
    int state; // 0 registered/uninitialized, 1 initializing, 2 initialized
} backends[DM_ANDROID_SERVICE_COUNT];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

void dm_android_service_register(enum dm_android_sdk_service service,
                                dm_android_service_init_fn initialize,
                                void *context) {
    if ((unsigned)service >= DM_ANDROID_SERVICE_COUNT || !initialize)
        die("invalid Android service registration");
    pthread_mutex_lock(&lock);
    struct service_backend *backend = &backends[service];
    if (backend->initialize || backend->state)
        die("Android service registered twice: %s", service_names[service]);
    backend->initialize = initialize;
    backend->context = context;
    pthread_mutex_unlock(&lock);
}

void dm_android_service_initialize(enum dm_android_sdk_service service) {
    if ((unsigned)service >= DM_ANDROID_SERVICE_COUNT)
        die("invalid Android service initialization");
    pthread_mutex_lock(&lock);
    struct service_backend *backend = &backends[service];
    if (!backend->initialize)
        die("Android service backend missing: %s", service_names[service]);
    while (backend->state == 1) pthread_cond_wait(&changed, &lock);
    if (backend->state == 2) { pthread_mutex_unlock(&lock); return; }
    backend->state = 1;
    dm_android_service_init_fn initialize = backend->initialize;
    void *context = backend->context;
    pthread_mutex_unlock(&lock);
    int rc = initialize(context);
    if (rc) die("Android service initialization failed: %s (%d)", service_names[service], rc);
    pthread_mutex_lock(&lock);
    backend->state = 2;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    LOG("Android service initialized: %s", service_names[service]);
}
