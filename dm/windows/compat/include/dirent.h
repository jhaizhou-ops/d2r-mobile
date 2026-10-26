// UTF-8 目录遍历由 FindFirstFileW 提供，包含宿主所需的 d_type。
// UTF-8 directory traversal uses FindFirstFileW and includes the host's required d_type.
#pragma once
#include <windows.h>
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
struct dirent { unsigned char d_type; char d_name[1024]; };
typedef struct { HANDLE find; WIN32_FIND_DATAW data; int first; struct dirent entry; } DIR;
#ifdef __cplusplus
extern "C" {
#endif
DIR *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int closedir(DIR *directory);
#ifdef __cplusplus
}
#endif
