// 客体内存写保护脏页跟踪（nvn_dirty.c）
// Write-protect dirty-page tracking for guest memory (nvn_dirty.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

void nvn_dirty_add_range(const void *mem, size_t size);           // 登记一个内存池为跟踪区间（首次调用时注册缺页钩子与宿主写前钩子） | Register a memory pool as a tracked range (the first call registers the page-fault hook and the host pre-write hook)
void nvn_dirty_stat(int *live, int *peak, int *cap);                // 在册的内存池数、峰值、跟踪表容量（内存读数行用） | number of registered memory pools, peak, tracking table capacity (for the memory readout line)
void nvn_dirty_remove_range(const void *mem);                     // 池销毁：受保护页还原可写并标失效 | Pool destruction: restore protected pages to writable and mark invalid
int nvn_dirty_tracked(const void *addr, size_t len);
void nvn_dirty_protect(const void *addr, size_t len);              // 覆盖页设只读 | Set the covered pages read-only
uint64_t nvn_dirty_now(void);                                      // 当前代号：上传前取，作为该份内容的戳 | Current generation: taken before upload, as the stamp of that content
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp); // 覆盖页在戳之后被写过 → 1 | Covered pages were written after the stamp -> 1
/* ==== ANDROID-BEGIN（非上游新增区，见 docs/android/isolation-analysis.zh-CN.md）==== */
int nvn_dirty_next_range(const void *addr, size_t len, uint64_t stamp, size_t *cursor, size_t *offset, size_t *range_len); // 下一个代号大于戳的连续页段（相对 addr；未跟踪时返回剩余全段） | Next contiguous page span newer than stamp (offset from addr; untracked ranges return the remaining span)
/* ==== ANDROID-END ==== */
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps);           // 累计：游戏写触发的缺页次数、宿主写前解保护的页数 | Cumulative: number of page faults triggered by game writes, number of pages unprotected before host writes
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns);   // 累计：本模块 mprotect 调用次数、覆盖页数、耗时 | Cumulative: this module's mprotect call count, pages covered, time spent
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total); // 当前存活槽、槽表高水位、登记总次数 | Currently live slots, slot table high-water mark, total number of registrations

/* ==== ANDROID-BEGIN（非上游新增区，见 docs/android/isolation-analysis.zh-CN.md）==== */
typedef struct {
    uint64_t calls,page_reads,block_reads,skipped_pages,range_fast_paths;
} NvnDirtyQueryStats;
/* Optional process-wide query counters; statistics stay off on the FPS path. */
void nvn_dirty_query_stats(NvnDirtyQueryStats *stats,int *summary_enabled,int *recording);
/* ==== ANDROID-END ==== */
