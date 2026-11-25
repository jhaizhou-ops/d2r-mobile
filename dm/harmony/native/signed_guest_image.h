#pragma once
#include <stddef.h>
#include <stdint.h>

/* The on-disk descriptor is emitted by build_signed_guest.py. */
typedef struct {
    uint64_t magic, version, raw_size, mapped_size, text_end, patch_off;
    uint64_t key_page_off, key_page_size, ro_off, data_off;
    unsigned char image_sha256[32], fixups_sha256[32];
} dm_signed_guest_descriptor;

int dm_signed_guest_prepare(const char *dir, char *error, size_t error_size);
void *dm_signed_guest_image(const char *name, const dm_signed_guest_descriptor **descriptor);
