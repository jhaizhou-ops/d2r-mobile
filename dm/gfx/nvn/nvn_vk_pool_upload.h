/* Copy guest ranges into the Vulkan pool mirror without overwriting bytes
 * owned by GPU writes in the current submission. */
#ifndef NVN_VK_POOL_UPLOAD_H
#define NVN_VK_POOL_UPLOAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t addr,size; } PoolUploadRange;

static inline uint64_t nvn_vk_pool_copy_guest_span(uint8_t *mapped,
        uint64_t base_addr,uint64_t map_offset,uint64_t addr,uint64_t len,
        const PoolUploadRange *writes,size_t write_count) {
    uint64_t cursor=addr,end=addr+len,copied=0;
    while(cursor<end) {
        uint64_t first=end,skip=cursor;
        for(size_t i=0;i<write_count;i++) {
            PoolUploadRange r=writes[i];
            if(cursor>=r.addr && cursor<r.addr+r.size && r.addr+r.size>skip)skip=r.addr+r.size;
            else if(r.addr>cursor && r.addr<first)first=r.addr;
        }
        if(skip>cursor){cursor=skip<end?skip:end;continue;}
        uint64_t bytes=first-cursor;
        memcpy(mapped+map_offset+(cursor-base_addr),(const void *)(uintptr_t)cursor,(size_t)bytes);
        copied+=bytes;cursor=first;
    }
    return copied;
}

#endif
