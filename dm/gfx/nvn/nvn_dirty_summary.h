/* Monotonic upper bounds for page generations, not cached query answers.
 * Publish both bounds BEFORE the page generation. A clean bound therefore
 * cannot hide an already published page write. A newer bound only requests
 * the original exact page scan, including partial first/last blocks.
 */
#ifndef NVN_DIRTY_SUMMARY_H
#define NVN_DIRTY_SUMMARY_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "nvn_dirty.h"

#define NVN_DIRTY_BLOCK_SHIFT 6
#define NVN_DIRTY_BLOCK_PAGES (1u << NVN_DIRTY_BLOCK_SHIFT)

typedef struct {
    _Atomic uint64_t newest;
    _Atomic uint64_t *blocks;
    size_t capacity;
} NvnDirtySummary;

static inline void nvn_dirty_summary_max(_Atomic uint64_t *value,uint64_t generation) {
    uint64_t old=atomic_load(value);
    while(old<generation&&!atomic_compare_exchange_weak(value,&old,generation)){}
}

static inline void nvn_dirty_summary_store(NvnDirtySummary *summary,
        _Atomic uint64_t *pages,size_t page,uint64_t generation) {
    nvn_dirty_summary_max(&summary->blocks[page>>NVN_DIRTY_BLOCK_SHIFT],generation);
    nvn_dirty_summary_max(&summary->newest,generation);
    atomic_store(&pages[page],generation);
}

static inline int nvn_dirty_summary_since(const NvnDirtySummary *summary,
        const _Atomic uint64_t *pages,size_t first,size_t last,uint64_t stamp,
        NvnDirtyQueryStats *stats) {
    /* Tiny uniform/vertex ranges already need very few loads. Avoid two
     * summary lookups when an exact scan is cheaper. */
    if(last-first<4){
        for(size_t page=first;page<=last;page++){
            if(stats)stats->page_reads++;
            if(atomic_load(&pages[page])>stamp)return 1;
        }
        return 0;
    }
    if(atomic_load(&summary->newest)<=stamp){
        if(stats){stats->range_fast_paths++;stats->skipped_pages+=last-first+1;}
        return 0;
    }
    size_t cursor=first;
    while(cursor<=last){
        size_t block=cursor>>NVN_DIRTY_BLOCK_SHIFT;
        size_t end=(block+1)<<NVN_DIRTY_BLOCK_SHIFT;
        if(end>last+1)end=last+1;
        if(stats)stats->block_reads++;
        if(atomic_load(&summary->blocks[block])<=stamp){
            if(stats)stats->skipped_pages+=end-cursor;
            cursor=end;continue;
        }
        for(;cursor<end;cursor++){
            if(stats)stats->page_reads++;
            if(atomic_load(&pages[cursor])>stamp)return 1;
        }
    }
    return 0;
}
#endif
