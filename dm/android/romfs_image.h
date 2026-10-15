#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size;
} romfs_image_entry;
// One immutable image per host. view_root has directories only; no payloads or
// sparse files are extracted. Set DM_ROMFS=view_root after successful setup.
int romfs_image_setup(const char *image_path, const char *view_root);
int romfs_image_setup_checked(const char *image_path, const char *view_root, uint64_t expected_image_bytes);
int romfs_image_shutdown(void); // EBUSY with live virtual file handles
int romfs_image_view_root(const char *path);
// All lookup/open/read hooks use 0 = ordinary host path/fd, 1 = handled,
// -1 = image/view failure (errno set). Never fall back after a view failure.
int romfs_image_lookup(const char *host_path, int *type, int64_t *size);
int romfs_image_list(const char *host_directory, romfs_image_entry **entries, size_t *count);
int romfs_image_open(const char *host_path, int mode, int *fd);
int romfs_image_note_fd(int fd, const char *host_path);
int romfs_image_pread(int fd, void *buffer, size_t length, int64_t offset, ssize_t *received);
int romfs_image_fd_size(int fd, int64_t *size);
int romfs_image_close_fd(int fd); // closes both descriptor tokens when handled
