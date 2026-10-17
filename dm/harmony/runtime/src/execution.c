#include "execution_internal.h"
#include "hle_ohos_compat.h"
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
_Static_assert(sizeof(xblr_cpu_frame)==832,"assembly frame size");
_Static_assert(offsetof(xblr_cpu_frame,q)==256 && offsetof(xblr_cpu_frame,nzcv)==768 &&
    offsetof(xblr_cpu_frame,site)==800,"assembly offsets");
_Static_assert(sizeof(xblr_instruction_site)==56,"offline metadata ABI");
extern const xblr_image_desc xblr_execution_images[2];
extern const xblr_hle_binding __start_xblr_hle[],__stop_xblr_hle[];
static pthread_key_t thread_key;
static pthread_once_t thread_once=PTHREAD_ONCE_INIT;
static int key_error;
static atomic_int execution_started;
static atomic_uint execution_failure,current_stage;
static atomic_uint completed_stage,execution_finished,terminal_status;
static atomic_uint_fast64_t thread_gate_limit;
static pthread_mutex_t failure_lock=PTHREAD_MUTEX_INITIALIZER;
static xblr_execution_report first_failure;
static xblr_module *modules[2];
static xblr_module_info module_info[2];
extern void xblr_threads_wake_failure(void);
extern void xblr_thread_finish_current(void);
extern void xblr_ui_observe(uint64_t message);
extern void xblr_world_tick_observe(uint64_t game);
extern void xblr_svc_abort_dispatch(void);
int xblr_execution_failed(void) {return (int)atomic_load(&execution_failure);}
static void make_key(void) { key_error=pthread_key_create(&thread_key,free); }
xblr_guest_thread *xblr_current_thread(void) {
    pthread_once(&thread_once,make_key);
    return key_error?NULL:pthread_getspecific(thread_key);
}
_Noreturn void xblr_execution_stop(uint32_t status) {
    xblr_guest_thread *t=xblr_current_thread();
    if(!t||!t->report||!t->escape_armed)abort();
    t->report->status=status;
    fprintf(stderr,"[xblr-exec] STOP status=%u stage=%u pc=%#llx svc=%#llx gates=%llu\n",
        status,t->report->stage_entered,(unsigned long long)t->report->original_pc,
        (unsigned long long)t->report->svc_number,(unsigned long long)t->report->gates);
    pthread_mutex_lock(&failure_lock);
    if(!atomic_load(&execution_failure)) {first_failure=*t->report;atomic_store(&execution_failure,status);}
    pthread_mutex_unlock(&failure_lock);
    xblr_svc_abort_dispatch();
    xblr_threads_wake_failure();
    longjmp(t->escape,1);
}
static xblr_guest_thread *new_thread_context(xblr_execution_report *report) {
    xblr_guest_thread *t=calloc(1,sizeof *t);if(!t)return NULL;
    t->tls_pointer=(uintptr_t)t->tls;t->report=report?report:&t->local_report;
    if(!report)t->local_report=(xblr_execution_report){.abi_version=1,.struct_bytes=sizeof t->local_report,
        .max_gates=atomic_load(&thread_gate_limit),.stage_entered=atomic_load(&current_stage)};
    uintptr_t object=(uintptr_t)t->thread_object;memcpy(t->tlr+0x1f8,&object,8);
    if(pthread_setspecific(thread_key,t)){free(t);return NULL;}return t;
}
void xblr_run_guest_thread(uint64_t entry,uint64_t arg,uint64_t sp) {
    xblr_guest_thread *t=new_thread_context(NULL);if(!t)abort();
    t->report->symbol_address=entry;
    snprintf(t->report->symbol,sizeof t->report->symbol,"guest thread entry %#llx",(unsigned long long)entry);
    t->escape_armed=1;
    if(!setjmp(t->escape)) {sw_thread_enter(entry,arg,sp);xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);}
    t->escape_armed=0;pthread_setspecific(thread_key,NULL);free(t);
}
void xblr_exit_guest_thread(void) {
    xblr_guest_thread *t=xblr_current_thread();if(!t||!t->escape_armed)abort();
    longjmp(t->escape,2);
}
uint64_t xblr_invoke_guest(void *fn,const uint64_t *a,uint32_t n) {
    if(!fn||n>8||(n&&!a)||!xblr_current_thread())abort();uint64_t v[8]={0};
    if(n)memcpy(v,a,n*sizeof *a);
    return ((uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))fn)(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7]);
}
uint64_t xblr_invoke_guest_callback(void *user,void *fn,const uint64_t *a,uint32_t n) {
    (void)user;return xblr_invoke_guest(fn,a,n);
}
void xblr_graphics_fatal(void *user,const char *message) {
    (void)user;xblr_guest_thread *t=xblr_current_thread();
    if(t&&t->report)snprintf(t->report->symbol,sizeof t->report->symbol,"graphics: %.116s",message?message:"unknown");
    fprintf(stderr,"[xblr-exec] GRAPHICS_FAILURE %s\n",message?message:"unknown");
    xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);
}
void *xblr_module_named_symbol(const char *module,const char *name) {
    uint64_t address=0;
    for(unsigned i=0;i<2;++i)if(!strcmp(g_mod[i].name,module)&&!xblr_module_symbol(modules[i],name,&address))return (void*)(uintptr_t)address;
    return NULL;
}
int xblr_execution_has_started(void) {return atomic_load(&execution_started);}
int xblr_get_execution_progress(xblr_execution_progress *p,size_t bytes) {
    if(!p||bytes!=sizeof *p)return XBLR_EXEC_ARGUMENT;
    *p=(xblr_execution_progress){.abi_version=1,.struct_bytes=sizeof *p};
    p->started=(uint32_t)atomic_load(&execution_started);
    p->stage_completed=atomic_load(&completed_stage);
    p->stage_entered=atomic_load(&current_stage);
    p->status=atomic_load(&execution_failure);
    if(!p->status)p->status=atomic_load(&terminal_status);
    p->active=p->started&&!atomic_load(&execution_finished)&&!p->status;
    return 0;
}
int xblr_query_module_memory(uint64_t address,uint64_t *base,uint64_t *size,uint64_t *perm) {
    for(unsigned i=0;i<2;++i) {
        const xblr_image_desc *d=&xblr_execution_images[i];uint64_t b=(uintptr_t)d->base;
        if(address<b||address-b>=d->size)continue;
        uint64_t off=address-b;
        if(off<d->ro_offset){*base=b;*size=d->ro_offset;*perm=5;}
        else if(off<d->data_offset){*base=b+d->ro_offset;*size=d->data_offset-d->ro_offset;*perm=1;}
        else {*base=b+d->data_offset;*size=d->size-d->data_offset;*perm=3;}
        return 1;
    }
    return 0;
}
void *xblr_execution_symbol(const char *name) {
    xblr_guest_thread *t=xblr_current_thread();
    if(!t||!t->report)return NULL;
    uint64_t address=0;
    for(unsigned i=0;i<2;++i)if(!xblr_module_symbol(modules[i],name,&address))return (void*)(uintptr_t)address;
    snprintf(t->report->symbol,sizeof t->report->symbol,"%s",name);
    xblr_execution_stop(XBLR_EXEC_MISSING_SYMBOL);
}
static uint64_t counter(void) {
    uint64_t ns=guest_ns();
    return (ns/1000000000u)*19200000u+(ns%1000000000u)*12u/625u;
}
static void trace_named_port(xblr_guest_thread *t,const xblr_cpu_frame *f) {
    uint64_t b,n,p;char name[13]={0};uint64_t address=f->x[1];
    if(xblr_query_module_memory(address,&b,&n,&p)&&(p&1)&&n-(address-b)>=12)memcpy(name,(void*)(uintptr_t)address,12);
    LOG("ConnectToNamedPort name=%s lr=%#llx fp=%#llx",name[0]?name:"<not in module>",(unsigned long long)f->x[30],(unsigned long long)f->x[29]);
    /* Main guest runs on the dedicated pthread stack. Never dereference arbitrary
     * frame pointers or worker stacks merely to produce diagnostics. */
    if(!t->is_main)return;
    pthread_attr_t attr;void *base;size_t size;
    if(pthread_getattr_np(pthread_self(),&attr))return;
    int e=pthread_attr_getstack(&attr,&base,&size);pthread_attr_destroy(&attr);if(e||size<16)return;
    uint64_t fp=f->x[29],lo=(uintptr_t)base,hi=lo+size;
    for(unsigned i=0;i<12&&!(fp&15)&&fp>=lo&&fp>=f->sp&&fp<=hi-16;++i) {
        uint64_t pair[2];memcpy(pair,(void*)(uintptr_t)fp,sizeof pair);
        module_t *m=module_of((void*)(uintptr_t)pair[1]);
        LOG("guest frame %u %s+%#llx",i,m?m->name:"host",(unsigned long long)(m?pair[1]-(uintptr_t)m->base:pair[1]));
        if(pair[0]<=fp)break;fp=pair[0];
    }
}
void xblr_instruction_dispatch(xblr_cpu_frame *f) {
    xblr_guest_thread *t=xblr_current_thread();
    if(!t)abort();
    xblr_execution_report *r=t->report;
    if(atomic_load(&execution_failure)) {
        pthread_mutex_lock(&failure_lock);*r=first_failure;pthread_mutex_unlock(&failure_lock);
        longjmp(t->escape,1);
    }
    const xblr_instruction_site *s=(const void*)(uintptr_t)f->site;
    if(!t->is_main)r->stage_entered=atomic_load(&current_stage);
    r->original_pc=s->original_pc;r->guest_sp=f->sp;
    ++r->gates;if(r->max_gates&&r->gates>r->max_gates)xblr_execution_stop(XBLR_EXEC_GATE_LIMIT);
    f->x[18]=t->virtual_x18;f->continuation=s->resume;
    switch(s->kind) {
    case 0: if(s->arg==0x5549)xblr_ui_observe(f->x[1]);else if(s->arg==0x5754)xblr_world_tick_observe(f->x[0]);s->operation(f);break;
    case 1: if(s->arg<31)f->x[s->arg]=t->tls_pointer;break;
    case 2: t->tls_pointer=s->arg<31?f->x[s->arg]:0;break;
    case 3: if(s->arg<31)f->x[s->arg]=(uintptr_t)t->tlr;break;
    case 4: if(s->arg<31)f->x[s->arg]=counter();break;
    case 5:
        r->svc_number=s->arg;memcpy(r->svc_args,f->x,sizeof r->svc_args);
        if(s->arg==0x1f)trace_named_port(t,f);
        svc_dispatch((int)s->arg,f->x,s->original_pc);
        break;
    case 6: f->x[18]=s->target;break;
    case 7: case 8: {
        uint64_t value=s->arg==32?(uint32_t)f->x[18]:f->x[18];
        if((value==0)==(s->kind==7))f->continuation=s->taken;
        break;
    }
    case 9: case 10:
        if((((f->x[18]>>s->extra)&1)==0)==(s->kind==9))f->continuation=s->taken;
        break;
    case 11: {
        uint64_t delta=f->x[18]-s->target;
        if(f->x[18]<s->target||delta%4||delta/4>=s->extra)xblr_execution_stop(XBLR_EXEC_BAD_BRANCH);
        f->continuation=s->taken+(delta/4)*8;break;
    }
    default:xblr_execution_stop(XBLR_EXEC_BAD_BRANCH);
    }
    t->virtual_x18=f->x[18];
}
static void invoke(uint32_t stage,const char *name,uint64_t a,uint64_t b) {
    xblr_guest_thread *t=xblr_current_thread();xblr_execution_report *r=t->report;
    r->stage_entered=stage;snprintf(r->symbol,sizeof r->symbol,"%s",name);
    atomic_store(&current_stage,stage);
    r->symbol_address=(uintptr_t)xblr_execution_symbol(name);
    fprintf(stderr,"[xblr-exec] ENTER stage=%u symbol=%s address=%#llx\n",stage,name,(unsigned long long)r->symbol_address);
    r->result_x0=((uint64_t(*)(uint64_t,uint64_t))(uintptr_t)r->symbol_address)(a,b);
    r->stage_completed=stage;
    atomic_store(&completed_stage,stage);
    fprintf(stderr,"[xblr-exec] RETURN stage=%u symbol=%s gates=%llu\n",stage,name,(unsigned long long)r->gates);
}
int xblr_execute_startup(uint32_t max_stage,uint64_t max_gates,xblr_execution_report *r,size_t bytes) {
    if(!r||bytes!=sizeof *r||max_stage<1||max_stage>9||(!max_gates&&max_stage!=9))return XBLR_EXEC_ARGUMENT;
    *r=(xblr_execution_report){.abi_version=1,.struct_bytes=sizeof *r,.max_gates=max_gates,.page_bytes=(uint32_t)sysconf(_SC_PAGESIZE)};
    if(r->page_bytes!=4096)return r->status=XBLR_EXEC_PAGE_SIZE;
    if(atomic_exchange(&execution_started,1))return r->status=XBLR_EXEC_BUSY;
    pthread_once(&thread_once,make_key);
    if(key_error) {atomic_store(&terminal_status,XBLR_EXEC_TLS);atomic_store(&execution_finished,1);return r->status=XBLR_EXEC_TLS;}
    atomic_store(&thread_gate_limit,max_gates);
    xblr_guest_thread *t=new_thread_context(r);
    if(!t) {atomic_store(&terminal_status,XBLR_LOAD_MEMORY);atomic_store(&execution_finished,1);return r->status=XBLR_LOAD_MEMORY;}
    t->is_main=1;t->escape_armed=1;t->handle=svc_thread_main_init();
    int escaped=setjmp(t->escape);
    if(escaped) {
        /* Explicit main ExitThread also publishes only on the restored native
         * stack. Diagnostic early returns and failure stops retain old policy. */
        if(escaped==2)xblr_thread_finish_current();
        goto done;
    }
    for(unsigned i=0;i<2;++i) {
        int e=xblr_bind_image(&xblr_execution_images[i],&modules[i],&module_info[i]);
        if(e)xblr_execution_stop((uint32_t)e);
        memcpy(g_mod[i].name,xblr_execution_images[i].name,16);g_mod[i].base=(uint8_t*)xblr_execution_images[i].base;g_mod[i].size=xblr_execution_images[i].size;
    }
    g_nmod=2;g_t0_ns=now_ns();
    g_flag.thr=!!getenv("SW_THR");g_flag.svc=!!getenv("SW_SVC");g_flag.fs=!!getenv("SW_FS");
    g_flag.movie=!!getenv("SW_MOVIE");g_flag.dbg=!!getenv("SW_DBG");g_flag.imports=!!getenv("SW_IMPORTS");g_flag.bootprof=!!getenv("SW_BOOTPROF");
    xblr_link_report link;
    int e=xblr_link_modules(modules,2,__start_xblr_hle,(size_t)(__stop_xblr_hle-__start_xblr_hle),1,NULL,NULL,&link);
    if(e)xblr_execution_stop((uint32_t)e);
    r->relocations_applied=link.writes_applied;
    invoke(1,"_init_libc0",0,0);if(max_stage==1)goto done;
    invoke(2,"nnosInitialize",t->handle,0);if(max_stage==2)goto done;
    invoke(3,"_init_libc1",0,0);if(max_stage==3)goto done;
    invoke(4,"nninitInitializeSdkModule",0,0);if(max_stage==4)goto done;
    invoke(5,"nninitStartup",0,0);if(max_stage==5)goto done;
    invoke(6,"_init_libc2",0,0);if(max_stage==6)goto done;
    r->stage_entered=7;
    atomic_store(&current_stage,7);
    for(unsigned i=0;i<2;++i) {
        uint64_t p=(uintptr_t)xblr_execution_images[i].base+module_info[i].init_offset;
        if(module_info[i].init_offset)((void(*)(void))(uintptr_t)p)();
    }
    r->stage_completed=7;atomic_store(&completed_stage,7);if(max_stage==7)goto done;
    static char *argv[]={"D2R",NULL}; /* preserve original host/main.c argument contract */
    invoke(8,"_ZN2nn2os11SetHostArgcEi",1,0);
    invoke(8,"_ZN2nn2os11SetHostArgvEPPc",(uintptr_t)argv,0);if(max_stage==8)goto done;
    {extern int xblr_padmode_boot(void);int pe=xblr_padmode_boot();if(pe)xblr_execution_stop((uint32_t)pe);}
    invoke(9,"nnMain",0,0);
done:
    /* SDK worker threads can retain main TLS/module addresses. Keep them alive
     * for the process; a startup invocation is intentionally not restartable. */
    t->escape_armed=0;t->report=NULL;pthread_setspecific(thread_key,NULL);
    atomic_store(&terminal_status,r->status);atomic_store(&execution_finished,1);
    return (int)r->status;
}
