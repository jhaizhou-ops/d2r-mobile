// 资源档案用 64 位大小和 UTF-8 路径，时间字段与公共宿主一致。
// Resource archives use 64-bit sizes and UTF-8 paths, with the shared host's timestamp fields.
#pragma once
#include_next <sys/stat.h>
#include <stdint.h>
#include <time.h>
#undef stat
#undef fstat
struct dm_windows_stat {
    uint64_t st_dev, st_ino, st_nlink;
    unsigned st_mode;
    int64_t st_size;
    time_t st_atime, st_mtime, st_ctime;
    struct timespec st_atimespec, st_mtimespec, st_ctimespec;
};
#ifdef __cplusplus
extern "C" {
#endif
int dm_windows_stat(const char *path, struct dm_windows_stat *out);
int dm_windows_fstat(int fd, struct dm_windows_stat *out);
int lstat(const char *path, struct dm_windows_stat *out);
#ifdef __cplusplus
}
#endif
#define stat dm_windows_stat
#define fstat dm_windows_fstat
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define S_ISCHR(m) (((m) & _S_IFMT) == _S_IFCHR)
#define S_ISFIFO(m) (((m) & _S_IFMT) == _S_IFIFO)
#endif
#define S_ISLNK(m) (((m) & _S_IFMT) == 0xa000)
#define S_ISSOCK(m) (((m) & _S_IFMT) == 0xc000)
#ifndef S_ISBLK
#define S_ISBLK(m) (((m) & _S_IFMT) == 0x6000)
#endif
