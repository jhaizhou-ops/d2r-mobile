#include "../host/dm.h"
#include <android/api-level.h>
#include <errno.h>
#include <stdatomic.h>

enum { APP_SLOT = 2, GUEST_SLOT_COUNT = 8 };
typedef struct {
    uintptr_t slots[GUEST_SLOT_COUNT + 1];
    void *owned_tls;
    void *owned_tlr;
    void *owned_thread;
    unsigned registry_index;
} guest_slots;
static pthread_key_t context_key;
static pthread_once_t context_once = PTHREAD_ONCE_INIT;
static _Atomic uintptr_t active_contexts[4096];
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "signal-safe context membership requires lock-free pointers");

static uintptr_t *host_tcb(void) { return (uintptr_t *)__builtin_thread_pointer(); }
static pthread_key_t slot_key(unsigned i) {
    const pthread_key_t keys[] = {g_key18, g_keytls, g_keytlr, g_keygate,
                                g_keyres[0], g_keyres[1], g_keyaspect, g_keyaspectui};
    if (i >= GUEST_SLOT_COUNT) die("Android guest slot index %u", i);
    return keys[i];
}
static void destroy_context(void *p) {
    guest_slots *c = p;
    if (!c) return;
    if (host_tcb()[APP_SLOT] != (uintptr_t)c->slots) die("Android app TLS slot changed while attached");
    host_tcb()[APP_SLOT] = 0;
    atomic_store_explicit(&active_contexts[c->registry_index], 0, memory_order_release);
    free(c->owned_thread);
    free(c->owned_tlr);
    free(c->owned_tls);
    free(c);
}
static void create_context_key(void) {
    if (android_get_device_api_level() < 29) die("Android guest slots require API 29+");
    if (pthread_key_create(&context_key, destroy_context)) die("Android guest context key");
}
uintptr_t *dm_android_slots(void) {
    pthread_once(&context_once, create_context_key);
    guest_slots *c = pthread_getspecific(context_key);
    if (!c) {
        if (host_tcb()[APP_SLOT]) die("Android application TLS slot already occupied");
        c = calloc(1, sizeof *c);
        if (!c || pthread_setspecific(context_key, c)) die("Android guest context allocation");
        unsigned i;
        for (i = 0; i < sizeof active_contexts / sizeof *active_contexts; ++i) {
            uintptr_t empty = 0;
            if (atomic_compare_exchange_strong_explicit(&active_contexts[i], &empty,
                (uintptr_t)c->slots, memory_order_release, memory_order_relaxed)) break;
        }
        if (i == sizeof active_contexts / sizeof *active_contexts) die("Android guest context registry capacity");
        c->registry_index = i;
        host_tcb()[APP_SLOT] = (uintptr_t)c->slots;
    }
    if (host_tcb()[APP_SLOT] != (uintptr_t)c->slots) die("Android guest context mismatch");
    return c->slots;
}
int dm_android_thread_has_guest_context(void) {
    uintptr_t p = host_tcb()[APP_SLOT];
    if (!p) return 0;
    for (unsigned i = 0; i < sizeof active_contexts / sizeof *active_contexts; ++i)
        if (atomic_load_explicit(&active_contexts[i], memory_order_acquire) == p) return 1;
    return 0;
}
void dm_android_slots_sync(void) {
    uintptr_t *s = dm_android_slots();
    for (unsigned i = 0; i < GUEST_SLOT_COUNT; ++i) s[i + 1] = (uintptr_t)pthread_getspecific(slot_key(i));
    guest_slots *c = pthread_getspecific(context_key);
    if (!c->owned_tls && s[2]) c->owned_tls = (void *)s[2];
    if (!c->owned_tlr && s[3]) {
        c->owned_tlr = (void *)s[3];
        c->owned_thread = *(void **)((uint8_t *)c->owned_tlr + 0x1f8);
    }
}
void dm_android_slot_set(pthread_key_t key, void *value) {
    if (pthread_setspecific(key, value)) die("Android pthread slot write");
    uintptr_t *s = dm_android_slots();
    for (unsigned i = 0; i < GUEST_SLOT_COUNT; ++i) if (slot_key(i) == key) { s[i + 1] = (uintptr_t)value; return; }
    die("Android unknown guest slot key %u", key);
}
uint64_t dm_android_slot_offset(unsigned i) {
    if (i >= GUEST_SLOT_COUNT) die("Android guest slot offset %u", i);
    return (i + 1) * sizeof(uintptr_t);
}
void dm_android_slots_detach(void) {
    pthread_once(&context_once, create_context_key);
    guest_slots *c = pthread_getspecific(context_key);
    if (c) {
        pthread_setspecific(context_key, NULL);
        destroy_context(c);
    }
}
