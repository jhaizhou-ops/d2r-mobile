// mmap 接口由 VirtualAlloc / VirtualProtect 实现，不把私有映射伪装成共享映射。
// mmap uses VirtualAlloc / VirtualProtect; private copies never masquerade as shared mappings.
#pragma once
#include <sys/types.h>
#include <stddef.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_SHARED 1
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)
#define MADV_FREE 8
#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset);
int mprotect(void *address, size_t size, int protection);
int munmap(void *address, size_t size);
int madvise(void *address, size_t size, int advice);
#ifdef __cplusplus
}
#endif
