// 存档副本与原子目录交换接口；实现只用真正的 NTFS 事务，不降级成非原子重命名。
// Save copying and atomic directory swaps; implementation requires real NTFS transactions, never non-atomic renames.
#pragma once
#define RENAME_SWAP 2
#ifdef __cplusplus
extern "C" {
#endif
int clonefile(const char *from, const char *to, int flags);
int renamex_np(const char *from, const char *to, unsigned flags);
#ifdef __cplusplus
}
#endif
