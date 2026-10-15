/* Build a host-publication plan from completed GPU writes. Replay's original
 * write ledger remains untouched: its order and CPU payloads are still needed
 * while recording. Only final bytes from the same pool incarnation can merge. */
#ifndef NVN_VK_WRITEBACK_PLAN_H
#define NVN_VK_WRITEBACK_PLAN_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct { uint64_t addr,size,pool_serial; } NvnVkWritebackSpan;

static int nvn_vk_writeback_span_compare(const void *a,const void *b) {
    const NvnVkWritebackSpan *x=a,*y=b;
    if(x->pool_serial!=y->pool_serial)return x->pool_serial<y->pool_serial?-1:1;
    if(x->addr!=y->addr)return x->addr<y->addr?-1:1;
    return x->size==y->size?0:x->size<y->size?-1:1;
}

/* Mutates only caller-owned scratch. SIZE_MAX denotes an invalid span. */
static inline size_t nvn_vk_writeback_plan(NvnVkWritebackSpan *spans,size_t count) {
    if(count && !spans)return SIZE_MAX;
    for(size_t i=0;i<count;i++)if(spans[i].size>UINT64_MAX-spans[i].addr)return SIZE_MAX;
    if(count>1)qsort(spans,count,sizeof(*spans),nvn_vk_writeback_span_compare);
    size_t written=0;
    for(size_t i=0;i<count;i++) {
        NvnVkWritebackSpan next=spans[i];if(!next.size)continue;
        if(written) {
            NvnVkWritebackSpan *last=&spans[written-1];
            uint64_t end=last->addr+last->size;
            if(last->pool_serial==next.pool_serial && next.addr<=end) {
                uint64_t next_end=next.addr+next.size;
                if(next_end>end)last->size=next_end-last->addr;
                continue;
            }
        }
        spans[written++]=next;
    }
    return written;
}
#endif
