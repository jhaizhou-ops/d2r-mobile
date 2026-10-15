#pragma once
#include <stddef.h>
#include <stdint.h>

#define DM_ANDROID_GUEST_NAME_XATTR "user.org.d2rmobile.guest-name"
int dm_android_fs_clone_tree(const char *source, const char *destination);
int dm_android_fs_remove_tree(const char *path);
// A snapshot is synchronized before an atomic directory exchange. The old
// committed tree is removed only after the exchange's parent is synchronized.
int dm_android_fs_commit_snapshot(const char *work, const char *committed);
int dm_android_fs_ensure_directory(const char *path);
int dm_android_fs_sync_tree(const char *path);
int dm_android_fs_initialize(void *context);
// Runs only with an explicit probe fixture directory. Uses no game resources.
int dm_android_fs_verify(const char *fixture_parent, const char *result_json);
