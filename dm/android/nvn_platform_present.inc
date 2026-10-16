/* Included by hle_nvn.c only on Android: virtual NVN objects/pacing are retained;
 * no version-94012 game-address hooks or Darwin instrumentation is linked.
 */
#include "nvn_platform_clock.h"
#include "nvn_platform_backend.h"
static pthread_mutex_t android_suspend_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t android_suspend_cv=PTHREAD_COND_INITIALIZER;
static pthread_mutex_t android_lifecycle_mu=PTHREAD_MUTEX_INITIALIZER;
static int android_suspended,android_inflight;
static _Atomic uint64_t android_nvn_frame;
static void x_drain(void);   // 翻译队列排空（定义见下） | Drain the translation queue (defined below)
static void x_wait(uint64_t job);   // 等任务号被翻译线程执行完（定义见下） | Wait until the translation thread has executed the job (defined below)
static void android_gfx_enter(void) {
    pthread_mutex_lock(&android_suspend_mu);
    while(android_suspended)pthread_cond_wait(&android_suspend_cv,&android_suspend_mu);
    android_inflight++;pthread_mutex_unlock(&android_suspend_mu);
}
static void android_gfx_leave(void) {
    pthread_mutex_lock(&android_suspend_mu);android_inflight--;
    pthread_cond_broadcast(&android_suspend_cv);pthread_mutex_unlock(&android_suspend_mu);
}
void dm_gfx_suspend(int on) {
    pthread_mutex_lock(&android_lifecycle_mu);
    /* Publish the new workload epoch before waking a resumed acquire caller.
     * Hint service calls never hold android_suspend_mu. */
    if(!on)dm_android_nvn_work_reset(1);
    pthread_mutex_lock(&android_suspend_mu);android_suspended=on!=0;
    if(on)while(android_inflight)pthread_cond_wait(&android_suspend_cv,&android_suspend_mu);
    else pthread_cond_broadcast(&android_suspend_cv);
    pthread_mutex_unlock(&android_suspend_mu);if(on){x_drain();dm_android_nvn_work_reset(0);mtl_drain();}
    pthread_mutex_unlock(&android_lifecycle_mu);
}
uint64_t dm_nvn_frame_count(void) {return atomic_load(&android_nvn_frame);}
/* ── 翻译队列（与 Apple 侧同构）：游戏线程只入队，翻译/出图在 nvn-translate 线程按序执行 ──
 * 收益与 iOS v1.0.15 相同：游戏线程下一帧的逻辑与录制与本次翻译重叠。命令载荷已存不可变
 * 快照（snap_tex），翻译线程不读游戏活对象。DM_NVN_SYNC=1 回到同步（对照档）。
 *   Translation queue (same shape as the Apple side): the game thread only enqueues; translation
 *   and present run in order on the nvn-translate thread, overlapping the game's next frame --
 *   the same win as iOS v1.0.15. Payloads already carry immutable snapshots (snap_tex), so the
 *   translation thread never reads live game objects. DM_NVN_SYNC=1 restores synchronous mode. */
enum { XJ_SUBMIT, XJ_PRESENT, XJ_FRAME };
typedef struct { int kind, n; uint64_t *h; const tex_t *tex; double period; int crop[4]; uint64_t frame, sub; } xjob;
#define XQ 32
#define XTK 1024
static xjob g_xq[XQ]; static uint64_t g_xq_in, g_xq_done;
static struct { uint64_t job, tk; } g_xtk[XTK];
static pthread_mutex_t g_xmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_xcv = PTHREAD_COND_INITIALIZER;
static int g_x_sync; static pthread_t g_x_thr;
static _Atomic int g_x_live;   // 队列线程已起（且非 DM_NVN_SYNC 对照档） | translation thread live (and not the DM_NVN_SYNC reference mode)
uint64_t x_sync_mark(void) {
    if (!atomic_load(&g_x_live)) return mtl_last_fence();
    pthread_mutex_lock(&g_xmu); uint64_t job = g_xq_in; pthread_mutex_unlock(&g_xmu);
    return job | (1ull << 63);
}
uint64_t x_sync_mark_resolve(uint64_t m) {
    if (!(m >> 63)) return m;   // 后端栅栏值：原样 | backend fence value: as-is
    x_wait(m & 0x7fffffffffffffffULL);
    return mtl_last_fence();
}
static void x_submit(const uint64_t *h, int n, uint64_t sub) {
    dm_android_nvn_schedule_thread("translate");
    mtl_sub_begin();
    for (int i = 0; i < n; i++) mtl_exec((const uint8_t *)(uintptr_t)h[i]);
    mtl_sub_commit_async();
    snap_tick(sub);
}
static void x_run(const xjob *j, uint64_t job) {
    if (j->kind == XJ_SUBMIT) { x_submit(j->h, j->n, j->sub); free(j->h); }
    else if (j->kind == XJ_PRESENT) { uint64_t tk = mtl_present(j->tex, j->period, j->crop); pthread_mutex_lock(&g_xmu); g_xtk[job % XTK].job = job; g_xtk[job % XTK].tk = tk; pthread_mutex_unlock(&g_xmu); }
    else { mtl_set_frame(j->frame); }
}
static void *x_thread(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "nvn-translate");
    pthread_mutex_lock(&g_xmu);
    for (;;) {
        while (g_xq_done == g_xq_in) pthread_cond_wait(&g_xcv, &g_xmu);
        uint64_t job = g_xq_done + 1; xjob j = g_xq[job % XQ];
        pthread_mutex_unlock(&g_xmu);
        x_run(&j, job);
        pthread_mutex_lock(&g_xmu);
        g_xq_done = job; pthread_cond_broadcast(&g_xcv);
    }
    return NULL;
}
static void x_start(void) {
    const char *e = getenv("DM_NVN_SYNC"); g_x_sync = e && atoi(e);
    if (g_x_sync) { LOG("[nvn-android] DM_NVN_SYNC=1：翻译在提交线程上同步做（对照档）"); return; }
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8 << 20);
    if (pthread_create(&g_x_thr, &at, x_thread, NULL)) { LOG("【红】翻译线程起不来，改为同步翻译"); g_x_sync = 1; }
    else { atomic_store(&g_x_live, 1); LOG("[nvn-android] 翻译线程已起：提交 / 出图在翻译线程上按序执行（队列 %d 项）", XQ); }
    pthread_attr_destroy(&at);
}
static uint64_t x_push(xjob *j) {   // 返回任务号 | Returns the job number
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, x_start);
    pthread_mutex_lock(&g_xmu);
    while (g_xq_in - g_xq_done >= XQ) pthread_cond_wait(&g_xcv, &g_xmu);
    if (j->kind == XJ_SUBMIT) j->sub = atomic_fetch_add(&g_x_subs, 1) + 1;
    uint64_t job = ++g_xq_in;
    if (g_x_sync) {   // 当场执行：单线程提交时与旧路径同序 | Run in place: with single-thread submission the order equals the old path
        pthread_mutex_unlock(&g_xmu); x_run(j, job); pthread_mutex_lock(&g_xmu);
        g_xq_done = job; pthread_cond_broadcast(&g_xcv);
    } else { g_xq[job % XQ] = *j; pthread_cond_broadcast(&g_xcv); }
    pthread_mutex_unlock(&g_xmu);
    return job;
}
static void x_wait(uint64_t job) { pthread_mutex_lock(&g_xmu); while (g_xq_done < job) pthread_cond_wait(&g_xcv, &g_xmu); pthread_mutex_unlock(&g_xmu); }
static void x_drain(void) { pthread_mutex_lock(&g_xmu); uint64_t job = g_xq_in; pthread_mutex_unlock(&g_xmu); x_wait(job); }
static uint64_t x_present_ticket(uint64_t job) {   // 出图任务执行完后换来的票号（0 = 那次出图被丢弃，不用等） | Ticket after the present job ran (0 = dropped, nothing to wait for)
    x_wait(job);
    pthread_mutex_lock(&g_xmu); uint64_t tk = g_xtk[job % XTK].job == job ? g_xtk[job % XTK].tk : 0; pthread_mutex_unlock(&g_xmu);
    return tk;
}
NVN(nvnQueueSubmitCommands,void,NVNqueue *q,int n,const NVNcommandHandle *handles) {
    if(!q || n<0 || (n&&!handles))die("Android NVN submit parameters n=%d",n);
    android_gfx_enter();dm_android_nvn_schedule_thread("submit");dm_android_nvn_work_check_thread();
    ((queue_t *)q)->submits++;
    xjob j = {.kind=XJ_SUBMIT, .n=n, .h=n > 0 ? malloc((size_t)n * sizeof *handles) : NULL};   // 句柄数组是游戏的，入队时拷一份 | The handle array belongs to the game; copy it when enqueuing
    if (n > 0) memcpy(j.h, handles, (size_t)n * sizeof *handles);
    x_push(&j);
    android_gfx_leave();
}
__attribute__((constructor)) static void android_nvn_register(void) {
    nvn_init_slots();
    unsigned profiled=0;
    if(getenv("DM_NVN_PROF"))for(unsigned i=0;i<NVN_NFUNC;i++) {
        if(nvn_slot[i].fn!=(void *)nvn_unimpl_entry && strcmp(nvn_names[i],"nvnQueuePresentTexture")) {
            nvn_real[i]=nvn_slot[i].fn;nvn_slot[i].fn=(void *)nvn_prof_entry;profiled++;
        }
    }
    LOG("[nvn-android] ELF interfaces=%u implemented=%u named-stop=%u profiled=%u; zero unknown-success stubs",
        NVN_NFUNC,g_nsem,NVN_NFUNC-g_nsem,profiled);
}
#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults,void,NVNwindowBuilder *b) {NVNdevice *d=WB(b)->dev;memset(b,0,sizeof(win_builder_t));WB(b)->dev=d;WB(b)->interval=1;}
NVN(nvnWindowBuilderSetDevice,void,NVNwindowBuilder *b,NVNdevice *d) {WB(b)->dev=d;}
NVN(nvnWindowBuilderSetNativeWindow,void,NVNwindowBuilder *b,NVNnativeWindow native) {WB(b)->native=native;}
NVN(nvnWindowBuilderSetTextures,void,NVNwindowBuilder *b,int n,NVNtexture *const *textures) {
    if(n<1 || n>4 || !textures)die("Android NVN window texture count %d",n);WB(b)->ntex=n;
    for(int i=0;i<n;i++){if(!textures[i])die("Android NVN null window texture %d",i);WB(b)->tex[i]=textures[i];}
}
NVN(nvnWindowBuilderSetPresentInterval,void,NVNwindowBuilder *b,int interval) {if(interval<0)die("negative present interval %d",interval);WB(b)->interval=interval;}
NVN(nvnWindowInitialize,NVNboolean,NVNwindow *window,const NVNwindowBuilder *b) {
    const win_builder_t *s=(const void *)b;if(s->ntex<1 || s->ntex>4)return 0;
    void *native=dm_vk_native_window();
    if(!native || (void *)(uintptr_t)s->native!=native)die("Android NVN window requires currently bound real Surface");
    win_t *w=(void *)window;memset(w,0,sizeof(*w));w->magic=MAGIC('W','I','N','D');w->native=s->native;
    w->ntex=s->ntex;w->interval=s->interval;memcpy(w->tex,s->tex,sizeof(w->tex));return 1;
}
NVN(nvnWindowFinalize,void,NVNwindow *window) {mtl_finish();memset(window,0,sizeof(win_t));}
NVN(nvnWindowSetPresentInterval,void,NVNwindow *window,int interval) {if(interval<0)die("negative present interval %d",interval);((win_t *)window)->interval=interval;LOG("NVN present interval 设为 %d（节拍周期 = 1e9/present_hz × interval）",interval);}
NVN(nvnWindowSetCrop,void,NVNwindow *window,int x,int y,int width,int height) {
    if(x<0 || y<0 || width<0 || height<0)die("NVN crop bounds %d %d %d %d",x,y,width,height);
    int crop[4]={x,y,width,height};memcpy(((win_t *)window)->crop,crop,sizeof(crop));
}
static int android_present_hz=-1,android_display_intervals;
void nvn_present_config(int hz,int display_intervals) {if(hz<0 || hz>1000)die("NVN pacing hz=%d",hz);android_present_hz=hz;android_display_intervals=display_intervals;LOG("NVN 节拍配置 hz=%d display_intervals=%d",hz,display_intervals);}
static uint64_t android_present_period(const win_t *w) {
    if(android_present_hz<0) {const char *e=getenv("DM_PRESENT_HZ");android_present_hz=e?atoi(e):60;
        android_display_intervals=mtl_has_window();}
    if(!android_present_hz)return 0;
    /* Original frame-counting behavior remains limited to 60 Hz unless the
     * front end explicitly supplies another tested game cadence. */
    uint64_t interval=w->interval>0?(uint64_t)w->interval:1;
    return (1000000000ULL/(uint64_t)android_present_hz)*(android_display_intervals?interval:1);
}
NVN(nvnWindowAcquireTexture,NVNwindowAcquireTextureResult,NVNwindow *window,NVNsync *sync,int *index) {
    win_t *w=(void *)window;if(!index || w->ntex<1 || w->ntex>4)die("NVN acquire window",0);
    android_gfx_enter();dm_android_nvn_schedule_thread("acquire");dm_android_nvn_work_check_thread();*index=w->next++%w->ntex;
    uint64_t now=now_ns(),period=android_present_period(w);nvn_pacer p={w->pace};uint64_t wait=nvn_pace(&p,now,period);w->pace=p.target;
    if(wait)dm_android_nvn_sleep_until(now+wait);
    if(w->ticket[*index]){uint64_t tk=x_present_ticket(w->ticket[*index]);if(tk)mtl_present_wait(tk);}
    if(sync){sync_t *s=(void *)sync;s->fenced=1;s->fence=mtl_last_fence();}
    dm_android_nvn_work_begin();android_gfx_leave();return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
int (*g_nvn_loadscreen_up)(void);
NVN(nvnQueuePresentTexture,void,NVNqueue *q,NVNwindow *window,int index) {
    (void)q;win_t *w=(void *)window;if(index<0 || index>=w->ntex || index>=4 || !w->tex[index])die("NVN present index %d",index);
    android_gfx_enter();dm_android_nvn_schedule_thread("present");dm_android_nvn_work_check_thread();
    xjob j = {.kind=XJ_PRESENT, .tex=snap_tex((const NVNtexture *)w->tex[index]), .period=android_present_period(w) / 1e9};
    memcpy(j.crop, w->crop, sizeof j.crop);
    w->ticket[index] = x_push(&j);   // 存任务号；取图时经 x_present_ticket 换成出图票号 | Store the job number; the acquire converts it via x_present_ticket
    w->presents++;
    uint64_t frame=atomic_fetch_add(&android_nvn_frame,1)+1;mtl_set_frame(frame);dm_android_nvn_presented(frame);
    dm_android_nvn_work_end(frame);android_gfx_leave();
}
