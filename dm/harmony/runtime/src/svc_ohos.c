/* Initial Horizon ABI services, adapted from sw/host/hle_{svc,thread}.c.
 * Guest-visible handles/priorities describe the emulated guest, not native TIDs.
 * No catch-all success: every unimplemented service stops at its original PC.
 */
#define _GNU_SOURCE
#include "execution_internal.h"
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
extern const xblr_image_desc xblr_execution_images[2];
static uint8_t *address_space;
static const uint64_t reserve_bytes=0x200000000ull;
static int reserve(void) {
    if(address_space)return 1;
    void *p=mmap(NULL,reserve_bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED)return 0;
    address_space=p;return 1;
}
static int is_self(xblr_guest_thread *t,uint64_t handle) {
    return (uint32_t)handle==t->handle||(uint32_t)handle==0xffff8000u;
}
int xblr_svc_dispatch(xblr_guest_thread *t,xblr_cpu_frame *f,const xblr_instruction_site *s) {
    uint64_t *r=f->x;
    switch(s->arg) {
    case 0x0c: /* GetThreadPriority */
        if(!is_self(t,r[1]))return 0;
        r[0]=0;r[1]=t->priority;return 1;
    case 0x0e: /* GetThreadCoreMask */
        if(!is_self(t,r[2]))return 0;
        r[0]=0;r[1]=t->core;r[2]=t->core_mask;return 1;
    case 0x10:r[0]=t->core;return 1; /* guest scheduler virtual core */
    case 0x25: /* GetThreadId */
        if(!is_self(t,r[1]))return 0;
        r[0]=0;r[1]=t->id;return 1;
    case 0x1e: {
        struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts))return 0;
        r[0]=0;r[1]=(uint64_t)ts.tv_sec*19200000u+(uint64_t)ts.tv_nsec*12u/625u;return 1;
    }
    case 0x29: { /* GetInfo: original loader's documented guest address partition. */
        if(!reserve())xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);
        uint64_t value,id=r[1],base=(uintptr_t)address_space;
        switch(id) {
        case 0:value=7;break;
        case 1:value=0;break;
        case 2:value=base+0x10000000;break;case 3:value=0x20000000;break;
        case 4:value=base+0x40000000;break;case 5:value=reserve_bytes-0x40000000;break;
        case 6:case 21:value=0xcd500000;break;
        case 7:case 22:value=xblr_execution_images[0].size+xblr_execution_images[1].size;break;
        case 8:value=0;break;
        case 11:if(getrandom(&value,sizeof value,0)!=(ssize_t)sizeof value)return 0;break;
        case 12:value=base;break;case 13:value=reserve_bytes;break;
        case 14:value=base+0x30000000;break;case 15:value=0x10000000;break;
        case 16:value=0x2000000;break;case 17:value=0x400000;break;
        case 18:value=0x0100726014352000ull;break;
        case 20:value=0;break;case 23:value=1;break;
        default:return 0;
        }
        r[0]=0;r[1]=value;return 1;
    }
    case 0x06: { /* QueryMemory of the actual module and worker stack. */
        uint64_t address=r[2],base=0,size=0;uint32_t state=0,perm=0;
        for(unsigned i=0;i<2;++i) {
            const xblr_image_desc *m=&xblr_execution_images[i];uint64_t b=(uintptr_t)m->base;
            if(address>=b&&address<b+m->size) {
                if(address<b+m->ro_offset) {base=b;size=m->ro_offset;state=3;perm=5;}
                else if(address<b+m->data_offset) {base=b+m->ro_offset;size=m->data_offset-m->ro_offset;state=3;perm=1;}
                else {base=b+m->data_offset;size=m->size-m->data_offset;state=4;perm=3;}
                break;
            }
        }
        if(!size) {
            pthread_attr_t attr;void *stack=NULL;size_t bytes=0;
            if(pthread_getattr_np(pthread_self(),&attr))return 0;
            int result=pthread_attr_getstack(&attr,&stack,&bytes);pthread_attr_destroy(&attr);
            if(result)return 0;
            uint64_t b=(uintptr_t)stack;
            if(address>=b&&address-b<bytes) {base=b;size=bytes;state=0xb;perm=3;}
        }
        if(!size&&address_space&&address>=(uintptr_t)address_space&&address-(uintptr_t)address_space<reserve_bytes) {
            base=(uintptr_t)address_space;size=reserve_bytes;state=0;perm=0;
        }
        if(!size)return 0;
        struct {uint64_t base,size;uint32_t state,attribute,permission,ipc_refs,device_refs,pad;} out={base,size,state,0,perm,0,0,0};
        if(!r[0])return 0;
        memcpy((void*)(uintptr_t)r[0],&out,sizeof out);r[0]=0;r[1]=0;return 1;
    }
    case 0x26: /* Break: never manufacture SDK initialization success. */
        xblr_execution_stop(XBLR_EXEC_GUEST_BREAK);
    case 0x27: /* OutputDebugString: actual write and result. */
        if(r[1]>4096||(!r[0]&&r[1]))return 0;
        if(write(STDERR_FILENO,(const void*)(uintptr_t)r[0],(size_t)r[1])!=(ssize_t)r[1])return 0;
        r[0]=0;return 1;
    default:return 0;
    }
}
