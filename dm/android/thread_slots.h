#pragma once
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* Android Q+ reserves TLS_SLOT_APP (TP + 16 on arm64) for applications.
 * https://android.googlesource.com/platform/bionic/+/refs/heads/android13-release/libc/platform/bionic/tls_defines.h
 * Guest x18/TLS/TLR/gates live in a separate owned array. Bionic TP and x18
 * stay unchanged. This also works when the runtime is loaded using dlopen. */
uintptr_t *dm_android_slots(void);
void dm_android_slots_sync(void);
void dm_android_slot_set(pthread_key_t key, void *value);
uint64_t dm_android_slot_offset(unsigned index);
void dm_android_slots_detach(void);
/* Signal-safe membership check; never dereferences another library's APP slot. */
int dm_android_thread_has_guest_context(void);
