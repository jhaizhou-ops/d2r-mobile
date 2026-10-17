#pragma once
#include <stdint.h>

/* Platform finger IDs are not core slot indices. Pending asynchronous drop/item
 * answers retain their slot until finish(), so an old answer cannot hit a new
 * finger. Ticket zero is never valid; callers bind every query to its ticket. */
typedef struct XblFingerSlots {
    int32_t ids[16];
    uint64_t tickets[16],next_ticket;
    uint32_t active,pending_end;
} XblFingerSlots;

static inline int XblFingerFind(const XblFingerSlots *s,int32_t id) {
    if(!s||id<0)return -1;
    for(int i=0;i<16;i++)if((s->active&(1u<<i))&&s->ids[i]==id)return i;
    return -1;
}
static inline int XblFingerBegin(XblFingerSlots *s,int32_t id,uint64_t *ticket) {
    if(!s||!ticket||id<0||XblFingerFind(s,id)>=0||s->next_ticket==UINT64_MAX)return -1;
    for(int i=0;i<16;i++)if(!(s->active&(1u<<i))){
        s->ids[i]=id;s->tickets[i]=++s->next_ticket;s->active|=1u<<i;
        s->pending_end&=~(1u<<i);*ticket=s->tickets[i];return i;
    }
    return -1;
}
static inline int XblFingerOwns(const XblFingerSlots *s,int slot,uint64_t ticket) {
    return s&&slot>=0&&slot<16&&ticket&&(s->active&(1u<<slot))&&s->tickets[slot]==ticket;
}
static inline int XblFingerPendEnd(XblFingerSlots *s,int slot,uint64_t ticket) {
    if(!XblFingerOwns(s,slot,ticket))return 0;
    s->pending_end|=1u<<slot;return 1;
}
static inline int XblFingerFinish(XblFingerSlots *s,int slot,uint64_t ticket) {
    if(!XblFingerOwns(s,slot,ticket))return 0;
    s->active&=~(1u<<slot);s->pending_end&=~(1u<<slot);return 1;
}
static inline uint32_t XblFingerCancelAll(XblFingerSlots *s) {
    if(!s)return 0;
    uint32_t previous=s->active;s->active=s->pending_end=0;
    /* Preserve ticket sequence across mode/resize/focus cancellation. */
    return previous;
}
