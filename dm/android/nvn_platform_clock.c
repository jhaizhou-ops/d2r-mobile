#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nvn_platform_clock.h"
#include <android/api-level.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

uint64_t dm_android_nvn_clock_ns(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)) { fprintf(stderr,"[nvn-android] CLOCK_MONOTONIC failed errno=%d\n",errno);__builtin_trap(); }
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
uint64_t dm_android_nvn_thread_cpu_ns(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)) { fprintf(stderr,"[nvn-android] thread CPU clock failed errno=%d\n",errno);__builtin_trap(); }
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}
void dm_android_nvn_sleep_until(uint64_t deadline) {
    struct timespec t={(time_t)(deadline/1000000000ULL),(long)(deadline%1000000000ULL)};
    int error;do {error=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL);}while(error==EINTR);
    if(error) { fprintf(stderr,"[nvn-android] absolute sleep failed errno=%d\n",error);__builtin_trap(); }
}
void dm_android_nvn_schedule_thread(const char *role) {
    /* An unprivileged Android application has no Darwin real-time/QoS API.
     * Request a modest native nice priority, then report the actual scheduler.
     * Permission failure is a measured state, never reported as RT success.
     */
    static _Thread_local int done;
    if(done)return;done=1;
    pid_t tid=(pid_t)syscall(SYS_gettid);
    int error=0;if(setpriority(PRIO_PROCESS,(id_t)tid,-2))error=errno;
    errno=0;int nice=getpriority(PRIO_PROCESS,(id_t)tid);int nice_error=errno;
    int policy=0;struct sched_param parameter={0};int query=pthread_getschedparam(pthread_self(),&policy,&parameter);
    fprintf(stderr,"[nvn-android] %s tid=%d requested_nice=-2 actual_nice=%d nice_errno=%d set_errno=%d policy=%d priority=%d sched_query=%d; Darwin hardware instruction/cycle statistics unavailable\n",
            role,(int)tid,nice,nice_error,error,policy,parameter.sched_priority,query);
}

/* API33 hint functions are deliberately obtained by name. The API29 binary
 * must not acquire strong relocations for newer libandroid entry points. */
typedef struct APerformanceHintManager HintManager;
typedef struct APerformanceHintSession HintSession;
typedef struct { pthread_t thread; pid_t tid; } HintOwner;
typedef struct {
    uint64_t generation,start,cpu_start,wait_start,wait_ns,waits[DM_NVN_WAIT_KIND_COUNT];
    unsigned active,wait_depth,discard; enum dm_android_nvn_wait_kind wait_kind;
} WorkFrame;
static _Thread_local WorkFrame work_frame;
static struct {
    pthread_mutex_t lock; _Atomic uint64_t generation;
    int initialized,enabled,api,foreground,mismatch,key_ready,failed;
    pthread_key_t owner_key; HintOwner *owner;
    void *library; HintManager *manager; HintSession *session;
    HintManager *(*get_manager)(void);
    HintSession *(*create_session)(HintManager *,const int32_t *,size_t,int64_t);
    int (*report)(HintSession *,int64_t);
    void (*close)(HintSession *);
    uint64_t frames,valid,skipped,wall_ns,cpu_ns,waits[DM_NVN_WAIT_KIND_COUNT],success,errors;
} hint={.lock=PTHREAD_MUTEX_INITIALIZER,.generation=1,.foreground=1};
static const int64_t hint_target_ns=33333333;

static void hint_close_locked(void) {
    if(hint.session){hint.close(hint.session);hint.session=NULL;}
}
static void hint_owner_exited(void *pointer) {
    HintOwner *owner=pointer;
    pthread_mutex_lock(&hint.lock);
    if(hint.owner==owner){hint_close_locked();hint.owner=NULL;
        atomic_fetch_add(&hint.generation,1);}
    pthread_mutex_unlock(&hint.lock);free(owner);
}
static void hint_initialize_locked(void) {
    if(hint.initialized)return;hint.initialized=1;
    const char *setting=getenv("DM_PERFORMANCE_HINTS");hint.enabled=setting&&!strcmp(setting,"1");
    hint.api=android_get_device_api_level();
    const char *reason="opt_in_off";
    if(hint.enabled){
        reason="api_below_33";
        if(hint.api>=33){
            hint.library=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);reason="symbols_unavailable";
            if(hint.library){
                hint.get_manager=(HintManager *(*)(void))dlsym(hint.library,"APerformanceHint_getManager");
                hint.create_session=(HintSession *(*)(HintManager *,const int32_t *,size_t,int64_t))dlsym(hint.library,"APerformanceHint_createSession");
                hint.report=(int (*)(HintSession *,int64_t))dlsym(hint.library,"APerformanceHint_reportActualWorkDuration");
                hint.close=(void (*)(HintSession *))dlsym(hint.library,"APerformanceHint_closeSession");
                if(hint.get_manager&&hint.create_session&&hint.report&&hint.close){
                    hint.manager=hint.get_manager();reason=hint.manager?"ready":"manager_unavailable";}
            }
        }
    }
    hint.key_ready=pthread_key_create(&hint.owner_key,hint_owner_exited)==0;
    if(!hint.key_ready)reason="owner_cleanup_unavailable";
    fprintf(stderr,"[nvn-android] ADPF_INIT enabled=%d api=%d backend=native33 target_ns=%lld manager_available=%d reason=%s\n",
            hint.enabled,hint.api,(long long)hint_target_ns,hint.manager!=NULL,reason);
}
static int hint_same_owner_locked(void) {
    return hint.owner&&pthread_equal(hint.owner->thread,pthread_self())&&hint.owner->tid==gettid();
}
static void hint_mismatch_locked(void) {
    if(hint.mismatch)return;hint.mismatch=1;hint_close_locked();atomic_fetch_add(&hint.generation,1);
    fprintf(stderr,"[nvn-android] ADPF_THREAD_MISMATCH owner_tid=%d caller_tid=%d reporting_disabled=1\n",
            hint.owner?(int)hint.owner->tid:-1,(int)gettid());
}
void dm_android_nvn_work_check_thread(void) {
    /* Called before mtl_sub_begin acquires the Vulkan lock. */
    pthread_mutex_lock(&hint.lock);
    if(hint.owner&&!hint_same_owner_locked())hint_mismatch_locked();
    pthread_mutex_unlock(&hint.lock);
}
void dm_android_nvn_work_begin(void) {
    pthread_mutex_lock(&hint.lock);hint_initialize_locked();
    if(hint.owner&&!hint_same_owner_locked())hint_mismatch_locked();
    if(work_frame.discard&&work_frame.generation==atomic_load(&hint.generation)){
        pthread_mutex_unlock(&hint.lock);return;
    }
    if(work_frame.active&&work_frame.generation==atomic_load(&hint.generation)){
        work_frame.active=0;work_frame.discard=1;
        fprintf(stderr,"[nvn-android] ADPF_FRAME_SKIPPED reason=duplicate_acquire tid=%d\n",(int)gettid());
        pthread_mutex_unlock(&hint.lock);return;
    }
    if(!hint.owner&&hint.key_ready&&hint.foreground&&!hint.mismatch){
        HintOwner *old=pthread_getspecific(hint.owner_key);
        if(old){pthread_setspecific(hint.owner_key,NULL);free(old);}
        HintOwner *owner=malloc(sizeof *owner);
        if(owner){*owner=(HintOwner){pthread_self(),gettid()};
            if(pthread_setspecific(hint.owner_key,owner))free(owner);else hint.owner=owner;}
        if(hint.owner&&hint.enabled&&hint.manager&&!hint.failed){
            int32_t tid=hint.owner->tid;hint.session=hint.create_session(hint.manager,&tid,1,hint_target_ns);
            if(!hint.session)hint.failed=1;
            fprintf(stderr,"[nvn-android] ADPF_SESSION tid=%d available=%d target_ns=%lld\n",
                    (int)tid,hint.session!=NULL,(long long)hint_target_ns);
        }
    }
    memset(&work_frame,0,sizeof work_frame);
    if(hint.foreground&&!hint.mismatch&&hint_same_owner_locked()){
        work_frame.generation=atomic_load(&hint.generation);
        work_frame.cpu_start=dm_android_nvn_thread_cpu_ns();work_frame.start=dm_android_nvn_clock_ns();work_frame.active=1;
    }
    pthread_mutex_unlock(&hint.lock);
}
uint64_t dm_android_nvn_work_wait_begin(enum dm_android_nvn_wait_kind kind) {
    WorkFrame *f=&work_frame;
    if(!f->active||f->generation!=atomic_load(&hint.generation)||kind>=DM_NVN_WAIT_KIND_COUNT)return 0;
    if(!f->wait_depth++){f->wait_start=dm_android_nvn_clock_ns();f->wait_kind=kind;}
    return f->wait_start;
}
void dm_android_nvn_work_wait_end(uint64_t token) {
    WorkFrame *f=&work_frame;
    if(!token||!f->active||!f->wait_depth||token!=f->wait_start)return;
    if(!--f->wait_depth){uint64_t duration=dm_android_nvn_clock_ns()-f->wait_start;
        f->wait_ns+=duration;f->waits[f->wait_kind]+=duration;f->wait_start=0;}
}
void dm_android_nvn_work_end(uint64_t frame) {
    /* mtl_present has released G.lock. Never call the hint service from a wait hook. */
    uint64_t now=dm_android_nvn_clock_ns(),cpu=dm_android_nvn_thread_cpu_ns();
    pthread_mutex_lock(&hint.lock);hint_initialize_locked();hint.frames++;
    WorkFrame *f=&work_frame;
    if(hint.owner&&!hint_same_owner_locked())hint_mismatch_locked();
    int valid=f->active&&f->generation==atomic_load(&hint.generation)&&hint.foreground&&!hint.mismatch
        &&hint_same_owner_locked()&&!f->wait_depth&&now>f->start&&now-f->start>f->wait_ns&&cpu>=f->cpu_start;
    if(valid){uint64_t work=now-f->start-f->wait_ns;
        hint.valid++;hint.wall_ns+=work;hint.cpu_ns+=cpu-f->cpu_start;
        for(unsigned i=0;i<DM_NVN_WAIT_KIND_COUNT;i++)hint.waits[i]+=f->waits[i];
        if(hint.session){int rc=hint.report(hint.session,(int64_t)work);
            if(!rc)hint.success++;else {hint.errors++;hint.failed=1;hint_close_locked();
                fprintf(stderr,"[nvn-android] ADPF_REPORT_FAILED rc=%d reporting_disabled=1\n",rc);}}
    }else hint.skipped++;
    f->active=f->discard=0;
    if(hint.frames>=60){
        fprintf(stderr,"[nvn-android] ADPF_WORK_PROFILE frame=%llu frames=%llu valid_frames=%llu skipped_frames=%llu enabled=%d available=%d tid=%d target_ns=%lld cpu_work_wall_ms=%.3f thread_cpu_ms=%.3f excluded_fence_ms=%.3f excluded_queue_idle_ms=%.3f excluded_acquire_ms=%.3f excluded_present_ms=%.3f excluded_event_ms=%.3f report_success=%llu report_failed=%llu\n",
            (unsigned long long)frame,(unsigned long long)hint.frames,(unsigned long long)hint.valid,(unsigned long long)hint.skipped,
            hint.enabled,hint.session!=NULL,hint.owner?(int)hint.owner->tid:-1,(long long)hint_target_ns,hint.wall_ns/1e6,hint.cpu_ns/1e6,
            hint.waits[DM_NVN_WAIT_FENCE]/1e6,hint.waits[DM_NVN_WAIT_QUEUE_IDLE]/1e6,hint.waits[DM_NVN_WAIT_ACQUIRE]/1e6,
            hint.waits[DM_NVN_WAIT_PRESENT]/1e6,hint.waits[DM_NVN_WAIT_EVENT]/1e6,(unsigned long long)hint.success,(unsigned long long)hint.errors);
        hint.frames=hint.valid=hint.skipped=hint.wall_ns=hint.cpu_ns=hint.success=hint.errors=0;
        memset(hint.waits,0,sizeof hint.waits);
    }
    pthread_mutex_unlock(&hint.lock);
}
void dm_android_nvn_work_reset(int foreground) {
    pthread_mutex_lock(&hint.lock);hint_close_locked();hint.owner=NULL;hint.foreground=foreground!=0;
    hint.mismatch=hint.failed=0;atomic_fetch_add(&hint.generation,1);
    if(hint.initialized)fprintf(stderr,"[nvn-android] ADPF_RESET foreground=%d session_closed=1\n",hint.foreground);
    pthread_mutex_unlock(&hint.lock);
}
