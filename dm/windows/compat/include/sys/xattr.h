// 客体文件名存入 NTFS 备用数据流；不支持备用流的卷明确返回错误。
// Guest file names are stored in NTFS alternate streams; volumes without streams report errors.
#pragma once
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size);
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int lremovexattr(const char *path, const char *name);
#ifdef __cplusplus
}
#endif
