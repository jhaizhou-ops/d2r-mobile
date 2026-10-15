#pragma once
#include <stddef.h>

/* The front end owns a stable, acquired ANativeWindow while the worker runs. */
void *dm_android_native_window(void);
void *dm_vk_native_window(void);
void dm_vk_graphics_begin(void *transfer_memory, size_t size);
void dm_vk_graphics_end(void);
int dm_vk_graphics_live(void);
