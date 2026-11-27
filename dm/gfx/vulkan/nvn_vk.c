// Android NVN replay adapter. Genuine Vulkan commands; no CPU-only success fallback.
#include "../../host/dm.h"
#include "../nvn/nvn_obj.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/nvn_pace.h"
#include "../nvn/nvn_adaptive_pace.h"
#include "../../host/front.h"
#include "nvn_vk.h"
#include "engine/xbg_game_internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#define VLOG(...) LOG("[vk] " __VA_ARGS__)
static int g_ready,g_in_sub,g_npool;
static uint64_t g_present_n;
static dm_vk_config g_config;
// 公用宿主登记翻译耗时读数；Vulkan 尚未输出 Metal 专用的交替测量报表。
// Shared host registers translation timing readers; Vulkan does not emit Metal's alternating-measurement report.
uint64_t (*g_ub_sub_cpu_ns)(void), (*g_ub_sub_cyc)(void);
// Vulkan 每次提交都全画，没有 Metal 的冻结跳画和外推开关需要锁存。
// Vulkan replays every draw; there are no Metal freeze-elision or extrapolation switches to latch.
uint64_t mtl_host_latch(void) { return 0; }
void mtl_host_apply(uint64_t value) { (void)value; }
static pthread_mutex_t g_pool_mu=PTHREAD_MUTEX_INITIALIZER;
typedef struct {uint64_t lo,size;} pool_range;
static pool_range g_pools[4096];
static uint64_t *g_handles;static size_t g_nhandles,g_cap;
typedef struct direct_record{struct direct_record *next;size_t size;uint8_t data[];} direct_record;
static direct_record *g_direct;
static double g_times[1024];
static uint64_t g_time_ids[1024];
static uint64_t g_cnt_report[32],g_cnt_reset[32];
int xbg_evguard_fault(void *addr,int is_write,void *uc);   // xbg_game.cpp: diagnostic marker gfx-event-guard
static int vk_wtrack_fault(void *addr,int is_write,void *uc){(void)is_write;(void)uc;return xbg_wtrack_fault(addr);}
static uint64_t vk_now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
int dm_vk_initialize(const dm_vk_config *config,char *error,size_t bytes){
    if(!config||config->size!=sizeof(*config)||!config->game.present){if(error&&bytes)snprintf(error,bytes,"Invalid Android Vulkan config/present callback");return -1;}
    if(g_ready){if(error&&bytes)snprintf(error,bytes,"Vulkan adapter already initialized");return -1;}
    int r=xbg_game_initialize(&config->game,error,bytes);if(r)return r;
    // 写跟踪的存储快照（xbg_game.cpp wtrack，gfx-wtrack 开启）：缺页与宿主写客体内存都走 crash.c 的钩子，引擎不另装信号处理
    // Write-tracked storage snapshots (xbg_game.cpp wtrack, opt-in gfx-wtrack): faults and host writes into guest memory go through crash.c's hooks; the engine installs no handler of its own
    static int hooked;if(!hooked){hooked=1;xbg_wtrack_use_host_hooks();dm_fault_hook_register(vk_wtrack_fault);dm_guest_write_prep_register(xbg_wtrack_prepare);dm_fault_hook_register(xbg_evguard_fault);}
    r=xbg_game_set_sampler_mirror_clamp_to_edge(config->sampler_mirror_clamp_to_edge);
    if(r){xbg_game_shutdown();return r;}g_config=*config;g_ready=1;return 0;
}
void mtl_init(void){if(!g_ready)die("真实Vulkan后端未初始化（Surface/设备/着色器目录需先接入）");}
void bf_want_note(const char *key){(void)key;}
void mtl_pool_add(const void *mem,uint64_t size){mtl_init();if(!mem||!size||size>UINTPTR_MAX-(uintptr_t)mem)die("Invalid GPU pool");pthread_mutex_lock(&g_pool_mu);if(g_npool==4096)die("Too many GPU pools");g_pools[g_npool++]=(pool_range){(uintptr_t)mem,size};pthread_mutex_unlock(&g_pool_mu);xbg_legacy_pool_add((void*)mem,size);}
void mtl_pool_del(const void *mem){xbg_legacy_pool_del((void*)mem);pthread_mutex_lock(&g_pool_mu);for(int i=0;i<g_npool;i++)if(g_pools[i].lo==(uintptr_t)mem){g_pools[i]=g_pools[--g_npool];break;}pthread_mutex_unlock(&g_pool_mu);}
int mtl_addr(uint64_t addr,uint64_t len,void **buf,uint64_t *off){int ok=0;*buf=NULL;*off=0;pthread_mutex_lock(&g_pool_mu);for(int i=0;i<g_npool;i++){pool_range p=g_pools[i];if(addr>=p.lo&&addr-p.lo<=p.size&&len<=p.size-(addr-p.lo)){*buf=(void*)p.lo;*off=addr-p.lo;ok=1;break;}}pthread_mutex_unlock(&g_pool_mu);return ok;}
static void append_handle(uint64_t h){if(!g_in_sub)die("NVN command outside submit");if(g_nhandles==g_cap){g_cap=g_cap?g_cap*2:64;g_handles=realloc(g_handles,g_cap*sizeof(*g_handles));if(!g_handles)die("Submit allocation");}g_handles[g_nhandles++]=h;}
static void direct(uint32_t op,const void *payload,size_t bytes){size_t total=(8+bytes+7)&~(size_t)7;direct_record *r=calloc(1,sizeof(*r)+total+8);if(!r)die("Direct record allocation");r->size=total+8;r->next=g_direct;g_direct=r;op_hdr h={op,(uint32_t)total},end={OP_END,8};memcpy(r->data,&h,8);memcpy(r->data+8,payload,bytes);memcpy(r->data+total,&end,8);xbg_legacy_pool_add(r->data,r->size);append_handle((uintptr_t)r->data);}
void mtl_sub_begin(void){mtl_init();if(g_in_sub)die("NVN submit reentry");g_in_sub=1;g_nhandles=0;}
void mtl_exec(const uint8_t *p){append_handle((uintptr_t)p);}
void mtl_exec_rest(const uint64_t *h,int n){(void)h;(void)n;}
void mtl_sub_index(const uint64_t *h,int n){(void)h;(void)n;}
void mtl_sub_copy(uint64_t src,uint64_t dst,uint64_t n){uint64_t p[]={src,dst,n};direct(OP_COPY_B2B,p,sizeof(p));}
// nvnEventGetValue 之前：延后发布的事件先落到内存（xbg_game.cpp gfx-defer-events） | before nvnEventGetValue: a deferred event is published first (xbg_game.cpp gfx-defer-events)
void mtl_event_read(const volatile uint32_t *val){xbg_game_event_read((const void*)val);}
void mtl_sub_event(volatile uint32_t *val,uint32_t mode,uint32_t v){struct{uint64_t a;uint32_t m,v;}p={(uintptr_t)val,mode,v};direct(OP_SIGNAL_EVENT,&p,sizeof(p));}
void mtl_sub_counter(uint64_t addr,uint32_t type){uint64_t p[]={type,addr};direct(OP_REPORT_COUNTER,p,sizeof(p));g_cnt_report[type&31]++;}
uint64_t mtl_sub_commit(void){if(!g_in_sub)die("NVN commit without begin");xbg_submit((int)g_nhandles,g_handles);uint64_t f=xbg_legacy_last_fence();while(g_direct){direct_record *r=g_direct;g_direct=r->next;xbg_legacy_pool_del(r->data);free(r);}g_in_sub=0;g_nhandles=0;return f;}
uint64_t mtl_last_fence(void){return xbg_legacy_last_fence();}
int mtl_wait(uint64_t fence,uint64_t timeout){return xbg_legacy_wait(fence,timeout);}
void mtl_finish(void){xbg_legacy_finish();}
void mtl_drain(void){xbg_legacy_finish();}
void mtl_reg_tex(int id,const tex_t *t,const view_t *v){xbg_legacy_register_texture(id,t,v);}
void mtl_reg_samp(int id,const samp_par *s){xbg_legacy_register_sampler(id,s);}
void mtl_set_tex_pool_n(int n){(void)n;}
void mtl_prog_release(prog_t *p){xbg_legacy_prog_release(p);}
void mtl_tex_release(tex_t *t){xbg_legacy_tex_release(t);}
void mtl_xv_window_tex(uint64_t serial){(void)serial;}
void mtl_read_texels(const tex_t *t,unsigned level,unsigned layer,const NVNcopyRegion *r,uint8_t *dst,size_t row,size_t img){xbg_legacy_read_texels(t,level,layer,r,dst,row,img);}
// 上屏时刻：Vulkan 没有 Metal 的 presentedTime 回调，用出图排队完成时的 CPU 时刻（单调钟，秒）代替；稳态下相邻两次之差就是帧间隔。
// HLE 的掉帧统计与动态 3D 比例（hle_oe.c，与 iOS 同规则）靠它；原先恒为 0（无读数），安卓上动态比例从不判。
// Present times: Vulkan has no presentedTime callback like Metal, so the CPU time (monotonic clock, seconds) when the present was queued stands in;
// in steady state the difference of two neighbours is the frame interval. The HLE's dropped-frame count and the dynamic 3D scale (hle_oe.c, the iOS
// rules) depend on it; it used to be always 0 (no reading), so the dynamic scale never judged on Android.
static double g_present_at[1024];
static _Atomic double g_present_period;   // 本次出图的节拍周期（秒；平台出图读它排上屏时刻） | this present's pacing period (s; the platform present reads it to schedule display time)
double dm_present_period(void){return g_present_period;}
uint64_t mtl_present(const tex_t *t,double period,const int crop[4]){g_present_period=period;win_t window={0};window.interval=period>0?1:0;if(crop)memcpy(window.crop,crop,sizeof(window.crop));xbg_present_texture(&window,t);uint64_t n=++g_present_n;
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);g_present_at[n%1024]=(double)ts.tv_sec+ts.tv_nsec*1e-9;return n;}
void mtl_present_wait(uint64_t ticket){if(ticket>g_present_n)die("Unknown present ticket");}
void mtl_present_flush(void){mtl_drain();}
double mtl_presented_time(uint64_t n){return n && n<=g_present_n && g_present_n-n<1024?g_present_at[n%1024]:0;}
uint64_t mtl_present_count(void){return g_present_n;}
int mtl_has_window(void){return g_config.has_window?g_config.has_window(g_config.game.userdata):0;}
double front_display_refresh(char *name, size_t n);
double mtl_display_refresh(void) {
    double hz = front_display_refresh(NULL, 0); hz = hz > 0 ? hz : g_config.display_hz;
#ifdef __OHOS__
    // 只供出图节拍与动态 3D 比例使用；DM_PACE_REF60=0 恢复按原始刷新率分档 | Used only by present pacing and dynamic 3D scale; DM_PACE_REF60=0 restores raw-refresh tiers
    static int ref60 = -1; if (ref60 < 0) { const char *e = getenv("DM_PACE_REF60"); ref60 = !(e && *e == '0'); }
    if (ref60) hz = nvn_pace_reference_hz(hz);
#endif
    return hz;
}
int mtl_display_hz(void) { return (int)(mtl_display_refresh() + 0.5); }
double dm_pace_hz(double screen_hz, double floor_hz) { return nvn_pace_hz(screen_hz, floor_hz); }
__attribute__((weak)) int dm_fps_setting(void){return 60;}
int mtl_fps_target(void){return dm_fps_setting()==30?30:60;}
int mtl_link_active(void){return 0;}
int mtl_link_wait_tick(uint64_t timeout){(void)timeout;return 0;}
int mtl_warm_wait(int timeout){(void)timeout;return 0;}
void xbg_record_command(cmd_t *c,const void *record,uint32_t bytes){if(!c||!c->recording||bytes<8||(bytes&7))die("Invalid cloned record");for(int tries=0;!c->cur||c->cur+bytes+24>c->end;tries++){if(!c->cb||tries==2)die("Command memory exhausted");xbg_invoke_command_callback(c,bytes+24);}memcpy(c->cur,record,bytes);c->cur+=bytes;}
// ---------- 计时与统计（无图形时全 0） ----------
// ---------- Timing and statistics (all 0 without graphics) ----------
static _Atomic uint64_t g_cur_frame;
void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); xbg_game_set_frame(f); }
void mtl_ub_present(double iv) { (void)iv; }
// 长帧日志（hle_nvn.c）的 GPU、新建管线、着色器载入与贴图上传几项取自引擎的读数（xbg_game_frame_counters）；GPU 时刻跨度没有读数。
// The long-frame log's (hle_nvn.c) GPU, new-pipeline, shader-load and texture-upload items come from the engine's readings
// (xbg_game_frame_counters); there is no reading for GPU time spans.
double mtl_gpu_ms(uint64_t f) { return xbg_game_gpu_ms(f); }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = 0; xbg_game_stats s; xbg_game_get_stats(&s); *draws = s.draws; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { xbg_frame_counters c; xbg_game_frame_counters(&c); *uploads = c.uploads; *reuploads = c.reuploads; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { static _Atomic uint64_t taken; xbg_frame_counters c; xbg_game_frame_counters(&c); return c.upload_ns - atomic_exchange(&taken, c.upload_ns); }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_bytes(void) { xbg_frame_counters c; xbg_game_frame_counters(&c); return c.upload_bytes; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { xbg_frame_counters c; xbg_game_frame_counters(&c); *lib = c.shader_loads; *pso = c.pipelines; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { xbg_frame_counters c; xbg_game_frame_counters(&c); *lib_ns = c.shader_load_ns; *pso_ns = c.pipeline_ns; }
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = *grown = 0; *n = 0; }
void mtl_counter_stats(uint64_t rep[32], uint64_t rst[32]) { memcpy(rep, g_cnt_report, sizeof g_cnt_report); memcpy(rst, g_cnt_reset, sizeof g_cnt_reset); }
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb, int heap_ok, unsigned long long heap_mb, unsigned long long heap_free_mb, unsigned long long heap_maxblk_kb) {
    (void)frame; (void)rss_mb; (void)heap_ok; (void)heap_mb; (void)heap_free_mb; (void)heap_maxblk_kb;
}
void mtl_res_snapshot(mtl_res_count *out) { memset(out, 0, sizeof *out); out->pools = (uint32_t)g_npool; }
void mtl_res_counts(uint32_t tick, const char *why) { xbg_game_stats s; xbg_game_get_stats(&s); VLOG("资源 拍 %u（%s）：内存池 %d，绘制 %llu，派发 %llu", tick, why, g_npool, (unsigned long long)s.draws, (unsigned long long)s.dispatches); }
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy) { (void)on; (void)px; (void)py; (void)grid; (void)cx; (void)cy; }
int mtl_ui_ghost_rect(float r[4]) { (void)r; return 0; }
int dm_nvn_ruler_truth(void) { return 0; }
void dm_gfx_shot_at(long k, const char *path) { (void)k; VLOG("截图接口暂未接入：%s", path); }
