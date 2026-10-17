#include "xbg_game_internal.h"
#include <string.h>

/* Flatten at record time, before the destination can invoke a guest allocator
 * callback or overwrite/recycle any source segment. No resource data is copied:
 * NVN command operands still reference the original game buffers and objects. */
typedef struct clone_context {
    xbg_command_available_fn available;
    void *userdata;
    uint8_t *output;
    size_t capacity,used;
    uint64_t commands;
} clone_context;

static int walk(clone_context *c,const uint8_t *pc,unsigned depth) {
    if(depth>64)return -1;
    for(;;){
        if(++c->commands>10000000)return -2;
        size_t available=c->available(c->userdata,pc);
        if(available<sizeof(op_hdr))return -3;
        op_hdr h;memcpy(&h,pc,sizeof(h));
        if(h.size<8 || (h.size&7) || h.size>available)return -4;
        if(h.op==OP_END)return 0;
        if(h.op==OP_CALL || h.op==OP_JUMP){
            if(h.size<16)return -5;
            uint64_t next;memcpy(&next,pc+8,8);
            if(h.op==OP_JUMP){pc=(const uint8_t *)(uintptr_t)next;continue;}
            int r=walk(c,(const uint8_t *)(uintptr_t)next,depth+1);if(r)return r;
        }else if(h.op!=OP_BEGIN){
            if(h.size>c->capacity-c->used)return -6;
            if(c->output)memcpy(c->output+c->used,pc,h.size);
            c->used+=h.size;
        }
        pc+=h.size;
    }
}
int xbg_command_clone(int count,const uint64_t *handles,void *output,size_t capacity,
    xbg_command_available_fn available,void *userdata,size_t *bytes) {
    if(count<0 || count>1000000 || (count&&!handles) || !available || !bytes)return -7;
    clone_context c={available,userdata,(uint8_t *)output,capacity,0,0};
    for(int i=0;i<count;i++){int r=walk(&c,(const uint8_t *)(uintptr_t)handles[i],0);if(r)return r;}
    *bytes=c.used;return 0;
}
