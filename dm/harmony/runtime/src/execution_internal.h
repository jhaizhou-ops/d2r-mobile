#ifndef XBLR_EXECUTION_INTERNAL_H
#define XBLR_EXECUTION_INTERNAL_H
#include "xblr_execution.h"
#include <setjmp.h>
#include <pthread.h>
typedef struct __attribute__((aligned(16))) xblr_cpu_frame {
    uint64_t x[31],sp;
    uint8_t q[32][16];
    uint64_t nzcv,fpcr,fpsr,continuation,site,host_x18,padding[2];
} xblr_cpu_frame;
typedef struct xblr_instruction_site {
    void (*operation)(xblr_cpu_frame *);
    uint64_t original_pc,resume,taken,target;
    uint32_t kind,arg,extra,reserved;
} xblr_instruction_site;
typedef struct xblr_guest_thread {
    uint64_t virtual_x18,tls_pointer;
    uint8_t tls[8192],tlr[0x200],thread_object[0x400];
    uint32_t handle,priority,core;
    uint64_t id,core_mask;
    jmp_buf escape;
    xblr_execution_report *report;
    xblr_execution_report local_report;
    int is_main,escape_armed;
} xblr_guest_thread;
xblr_guest_thread *xblr_current_thread(void);
_Noreturn void xblr_execution_stop(uint32_t status);
void *xblr_execution_symbol(const char *name);
void xblr_instruction_dispatch(xblr_cpu_frame *frame);
void xblr_instruction_gate(void);
int xblr_svc_dispatch(xblr_guest_thread *thread,xblr_cpu_frame *frame,const xblr_instruction_site *site);
#endif
