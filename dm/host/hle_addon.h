#pragma once
#include <stddef.h>

// 在启动客体前登记已验证的目录；序号为正数，最多 64 项，不支持运行中替换。
// Register validated directories before starting the guest; positive indexes, up to 64 entries, no runtime replacement.
int dm_addon_register(int index, const char *directory);
int dm_addon_count(void);
int dm_addon_list(int *out, int offset, int capacity);
int dm_addon_path(int index, char *out, size_t capacity);
