// 通过 HeapWalk 汇总 CRT 默认堆；仅供诊断。
// Summarize the CRT default heap through HeapWalk, for diagnostics only.
#pragma once
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned blocks_in_use; size_t size_in_use, max_size_in_use, size_allocated; } malloc_statistics_t;
static inline void malloc_zone_statistics(void *zone, malloc_statistics_t *out) {
    (void)zone;
    memset(out, 0, sizeof *out);
    HANDLE heap = GetProcessHeap();
    if (!HeapLock(heap)) { fprintf(stderr, "[dm] HeapLock failed: %lu\n", GetLastError()); return; }
    PROCESS_HEAP_ENTRY entry = {0};
    while (HeapWalk(heap, &entry)) if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
        out->blocks_in_use++; out->size_in_use += entry.cbData;
    }
    DWORD error = GetLastError();
    if (!HeapUnlock(heap)) fprintf(stderr, "[dm] HeapUnlock failed: %lu\n", GetLastError());
    if (error != ERROR_NO_MORE_ITEMS) fprintf(stderr, "[dm] HeapWalk failed: %lu\n", error);
    out->size_allocated = out->size_in_use;
}
