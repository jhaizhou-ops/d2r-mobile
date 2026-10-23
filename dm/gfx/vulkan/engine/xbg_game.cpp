#include "../../nvn/nvn_copy_pitch.h"
#include "../../xtrace.h"
#include <pthread.h>
#include "xbg_game_internal.h"
#include "xbg_cpu_timing.h"
#include "xbg_completion_wait.h"
#ifdef DM_COMPLETION_WAIT_SELFTEST
extern "C" int xbg_completion_wait_selftest(char *report,size_t bytes);
#endif
#include "xbg_descriptor_timing.h"
#include "xbg_draw_timing.h"
#include "xbg_provenance.h"
#include "xbg_pipeline_key.h"
#include "xbg_vulkan.h"
#include "xbg_depth.h"
#include "xbg_sampler.h"
#include "xbg_framebuffer.h"
#include "xbg_clear.h"
#include "xbg_blit.h"
#include "xbg_bc_policy.h"
#include "xbg_writeback_policy.h"
#include "xbg_clear_spv.h"
#include "xbg_bright_spv.h"
#include "xbg_spirv_quad.h"
#include "xbg_spirv_warp.h"
#include "xbg_spirv_ldc.h"
#include "xbg_spirv_temps.h"
#include "xbg_spirv_raster.h"
#include "../../nvn/nvn_cover.h"
extern "C" {
#include "../../tex/nvn_deswizzle.h"
// The HLE's write-protect dirty-page tracker over the guest memory pools (dm/gfx/nvn/nvn_dirty.c). Weak: hosts and tests
// without it simply do not get the cross-frame vertex/index cache.
__attribute__((weak)) int nvn_dirty_tracked(const void *addr,size_t len);
__attribute__((weak)) void nvn_dirty_protect(const void *addr,size_t len);
__attribute__((weak)) uint64_t nvn_dirty_now(void);
__attribute__((weak)) int nvn_dirty_since(const void *addr,size_t len,uint64_t stamp);
__attribute__((weak)) void nvn_dirty_slot_stats(int *live,int *used_slots,uint64_t *adds_total);
// 3D 画面加亮的曝光 k（宿主 dm/host/dm_bright.c）；弱：不链宿主的图形单测不加亮
// Exposure k of the 3D picture brightening (the host's dm/host/dm_bright.c); weak: graphics unit tests without the host do not brighten
__attribute__((weak)) float dm_bright_k(void);
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sys/mman.h>
#include <sys/resource.h>
#include <signal.h>
#if defined(__ANDROID__)
#include <ucontext.h>
#endif
#include <cerrno>
#include <unistd.h>
#include <dlfcn.h>
#include <map>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <functional>
#include <deque>
#include <condition_variable>
#include <set>

// Flags of the NVN memory pool that holds a guest range (dm/gfx/nvn/hle_nvn.c), or -1. Weak: hosts without it keep every
// storage writeback.
extern "C" __attribute__((weak)) int nvn_pool_flags(const void *addr,size_t len);
// 真全屏界面下移（同 nvn_mtl.m 的 g_ui_rect_fn / ui_mat_patch；鸿蒙平台层登记，其他平台不登记 = 一切照旧）：屏幕比 16:9 窄时宿主把界面按 16:9
// 摆在画面底部（dm/host/dm_uiscreen.c），前端登记界面矩形的纵向位置与高（画面高的比例）；界面绘制（4 顶点、2 个属性、顶点 20 字节）的投影矩阵加纵向偏移、
// 剪裁跟着下移，盖满整个界面矩形的无贴图块（开面板压暗、转场黑幕）视口纵向放大到整个画面。
// True-fullscreen UI shift (as nvn_mtl.m g_ui_rect_fn / ui_mat_patch; registered by the HarmonyOS platform layer, nothing changes where nobody registers): on screens
// narrower than 16:9 the host lays the UI out at 16:9 at the bottom of the picture (dm/host/dm_uiscreen.c); the front end registers the UI rectangle's vertical position
// and height (fractions of the picture height). UI draws (4 vertices, 2 attributes, 20-byte vertices) get a vertical offset in the projection matrix and a shifted
// scissor; the untextured block covering the whole UI rectangle (panel dimming, transition curtain) gets its viewport scaled to the whole picture.
static void (*g_ui_rect_fn)(double *y,double *h);
static double g_ui_moff;static uint64_t g_ui_moff_addr;
extern "C" void xbg_set_ui_rect_fn(void (*fn)(double *y,double *h)){g_ui_rect_fn=fn;}
namespace {
xbg_game_config cfg{};
std::recursive_mutex mutex;
uint32_t deferred_live_profile=0;
// ---- Asynchronous translation (Claude, C076): the game thread queues submits/presents/resource updates;
// one graphics thread translates NVN -> Vulkan and owns the VkQueue, in order. Calls that need results
// (fence waits, readback, finish, command-memory allocation) drain the queue first. Like a real GPU,
// guest memory is read at execution; the game only reuses it after the fence (waits drain).
// files/gfx-sync-submit containing '1' restores synchronous translation.
bool gfx_async=false;std::atomic<uint64_t> gfx_enqueued{0},gfx_executed{0};std::atomic<int> gfx_presents_queued{0};
std::mutex gfx_job_mutex;std::condition_variable gfx_job_cv,gfx_idle_cv;std::deque<std::function<void()>> gfx_jobs;bool gfx_busy=false,gfx_stop=false;std::thread gfx_worker;std::thread::id gfx_worker_id;
void gfx_worker_loop(){
    gfx_worker_id=std::this_thread::get_id();
    pthread_setname_np(pthread_self(),"dm-gfx");
    // Graphics thread on performance cores (QoS USER_INTERACTIVE, as for the audio thread).
    {void *h=dlopen("libqos.so",RTLD_NOW);auto set=h?reinterpret_cast<int(*)(int)>(dlsym(h,"OH_QoS_SetThreadQoS")):nullptr;int r=set?set(5):-1;xbg_logf("NVN async graphics thread QoS=USER_INTERACTIVE rc=%d",r);}
    for(;;){std::function<void()> job;
        {std::unique_lock<std::mutex> l(gfx_job_mutex);gfx_job_cv.wait(l,[]{return gfx_stop||!gfx_jobs.empty();});if(gfx_jobs.empty())return;job=std::move(gfx_jobs.front());gfx_jobs.pop_front();gfx_busy=true;}
        job();
        {std::lock_guard<std::mutex> l(gfx_job_mutex);gfx_busy=false;}gfx_idle_cv.notify_all();
    }
}
void gfx_enqueue(std::function<void()> job){
    if(!gfx_async || std::this_thread::get_id()==gfx_worker_id){job();return;}
    {std::lock_guard<std::mutex> l(gfx_job_mutex);gfx_jobs.push_back(std::move(job));}gfx_job_cv.notify_one();
}
std::atomic<uint64_t> gfx_drain_calls{0},gfx_drain_ns{0},gfx_pace_ns{0},gfx_wait_calls{0};
void gfx_drain(){
    if(!gfx_async || std::this_thread::get_id()==gfx_worker_id)return;
    uint64_t t0=uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    {std::unique_lock<std::mutex> l(gfx_job_mutex);gfx_idle_cv.wait(l,[]{return gfx_jobs.empty() && !gfx_busy;});}
    ++gfx_drain_calls;gfx_drain_ns+=uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())-t0;
}
void gfx_async_stop(){if(!gfx_worker.joinable())return;{std::lock_guard<std::mutex> l(gfx_job_mutex);gfx_stop=true;}gfx_job_cv.notify_all();gfx_worker.join();gfx_stop=false;gfx_async=false;}
char error_text[1024]{};
bool initialized=false,failed=false;
VkBool32 sampler_mirror_clamp_enabled=VK_FALSE;
// Formatless storage access per format needs VK_KHR_format_feature_flags2 (core in Vulkan 1.3). Without it (Vulkan 1.1 drivers
// such as Adreno 730 512.615) the spec's "Formats Without Shader Storage Format" rule applies: the shaderStorageImage{Read,Write}
// WithoutFormat features cover exactly these formats, where the format also supports VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT.
bool format_feature_flags2=false;
// NVN DEPTH16 (0x32) is stored as X8_D24 when the driver cannot linearly filter D16_UNORM but can filter X8_D24: the game samples
// shadow maps with linear depth comparison, and Adreno 730 512.615 reports no SAMPLED_IMAGE_FILTER_LINEAR for D16_UNORM (or
// D32_SFLOAT). Transfers convert 16 <-> 24-bit depth with exact rounding; constant depth bias is rescaled to the finer depth unit.
bool depth16_as_d24=false;
uint32_t d16_to_d24(uint32_t k){return uint32_t((uint64_t(k)*2*16777215u+65535u)/(2*65535u));}
uint32_t d24_to_d16(uint32_t v){return uint32_t((uint64_t(v&0xffffffu)*2*65535u+16777215u)/(2*16777215u));}
bool storage_without_format_listed(VkFormat f){
    switch(f){
    case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SNORM:case VK_FORMAT_R8G8B8A8_UINT:case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R32_UINT:case VK_FORMAT_R32_SINT:case VK_FORMAT_R32_SFLOAT:case VK_FORMAT_R32G32_UINT:case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:case VK_FORMAT_R32G32B32A32_UINT:case VK_FORMAT_R32G32B32A32_SINT:case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UINT:case VK_FORMAT_R16G16B16A16_SINT:case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:case VK_FORMAT_R16_SFLOAT:case VK_FORMAT_R16G16B16A16_UNORM:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_UNORM:case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R16_UNORM:case VK_FORMAT_R8_UNORM:case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16_SNORM:case VK_FORMAT_R8G8_SNORM:case VK_FORMAT_R16_SNORM:case VK_FORMAT_R8_SNORM:case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R8G8_SINT:case VK_FORMAT_R16_SINT:case VK_FORMAT_R8_SINT:case VK_FORMAT_A2B10G10R10_UINT_PACK32:case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R8G8_UINT:case VK_FORMAT_R16_UINT:case VK_FORMAT_R8_UINT:return true;
    default:return false;
    }
}
uint64_t last_attachment_trace=UINT64_MAX;
struct Capture {
    std::string directory;FILE *trace=nullptr;uint64_t frame=0,bytes=0;
    unsigned events=0;bool active=false;
    ~Capture(){if(trace)fclose(trace);}
} capture;
constexpr uint64_t capture_limit=64ull*1024*1024;
void capture_log(const char *format,...){
    if(!capture.trace)return;va_list args;va_start(args,format);vfprintf(capture.trace,format,args);va_end(args);fputc('\n',capture.trace);fflush(capture.trace);
}
bool capture_begin(const char *kind,uint64_t present){
    capture.active=capture.trace && capture.frame==present+1 && capture.events<16;
    if(capture.active){++capture.events;capture_log("EVENT frame=%llu event=%u kind=%s",(unsigned long long)capture.frame,capture.events,kind);}
    return capture.active;
}
void capture_data(const char *label,const void *data,size_t bytes){
    if(!capture.active || !bytes)return;
    if(bytes>capture_limit-capture.bytes){capture_log("SKIP label=%s bytes=%zu reason=64MiB-budget",label,bytes);return;}
    char leaf[256];snprintf(leaf,sizeof(leaf),"f%llu-e%u-%s.bin",(unsigned long long)capture.frame,capture.events,label);
    std::string path=capture.directory+"/"+leaf;FILE *file=fopen(path.c_str(),"wb");
    if(!file){capture_log("IO_ERROR file=%s",leaf);return;}
    size_t written=fwrite(data,1,bytes,file);int closed=fclose(file);capture.bytes+=written;
    capture_log("FILE name=%s bytes=%zu requested=%zu closed=%d",leaf,written,bytes,closed);
}
// Diagnostic reads must remain available after runtime's fatal callback performs
// a nonlocal exit with the backend mutex held. Counters do not own GPU state.
struct Stats {
    std::atomic<uint64_t> submissions{0},draws{0},dispatches{0},presents{0},shader_loads{0};
    void reset(){submissions=0;draws=0;dispatches=0;presents=0;shader_loads=0;}
} stats;

// All access is under the existing recursive gfx mutex. No resource ownership.
XbgProvenance provenance;
FILE *provenance_file=nullptr;
void provenance_close(const char *reason){
    if(!provenance_file){provenance.armed=false;return;}
    FILE *f=provenance_file;provenance_file=nullptr;provenance.armed=false;
    char tail[128];int n=snprintf(tail,sizeof(tail),"STOP reason=%s events=%u draws=%u\n",reason,provenance.events,provenance.draws);
    bool bad=n<0||size_t(n)>=sizeof(tail);
    if(!bad)bad=fwrite(tail,1,size_t(n),f)!=size_t(n);
    if(fflush(f))bad=true;if(fclose(f))bad=true;
    xbg_logf("NVN PROVENANCE_CLOSE reason=%s io=%s",reason,bad?"FAIL":"OK");
}
void provenance_log(const char *format,...){
    if(!provenance_file || !provenance.window(stats.presents.load()))return;
    char line[2048];va_list a;va_start(a,format);int n=vsnprintf(line,sizeof(line)-1,format,a);va_end(a);
    if(n<0||size_t(n)>=sizeof(line)-1){provenance_close("FORMAT");return;}
    line[n++]='\n';if(!provenance.accept(unsigned(n))){provenance_close("LIMIT");return;}
    if(fwrite(line,1,size_t(n),provenance_file)!=size_t(n)||fflush(provenance_file))provenance_close("IO_FAIL");
}
void provenance_draw(){
    if(!provenance_file || !provenance.window(stats.presents.load()))return;
    if(!provenance.draw()){provenance_close("DRAW_LIMIT");return;}
    provenance_log("DRAW ordinal=%u frame=%llu submit=%llu",provenance.draws,(unsigned long long)provenance.target,(unsigned long long)stats.submissions.load());
}

uint64_t timing_clock(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
XbgCpuTiming timing(timing_clock);
thread_local XbgCpuTiming *timing_current=nullptr;
uint64_t timing_first=0,timing_last=0,timing_start_ns=0;
xbg_game_stats timing_before{};
bool timing_active=false;
// thread_local reads in a dlopen'ed libdm.so go through the dynamic TLS resolver: every CPU_SCOPE, perf_clock() and detail
// root paid one on every call, about 0.6% of the D2R camp's game thread in the resolver alone. timing_any counts the
// TimingRoots that set a timer on any thread; while it is 0 every timing_current is null, so the hot paths skip the
// thread_local. A/B target tlsgate reads it every time (tls_gate false). img_fast: get_image's inline front path (A/B
// target imgfast). Both are refreshed once a frame.
std::atomic<int> timing_any{0};bool tls_gate=true,img_fast=true;
// Color images are created without MUTABLE_FORMAT, which on Adreno keeps UBWC compression for render targets (Redmi K50 Ultra
// camp: GPU 42 -> 36 ms, 21.2 -> 23.8 fps). A view in a different format upgrades the image in place (upgrade_mutable), as
// nvn_mtl.m tex_upgrade_pfv does for Metal's lossless compression. Marker gfx-mutable-images = '1' creates every color
// image mutable, as before.
// Sample BC textures in their own formats where the device can (marker gfx-native-bc = 1 / 0, else DM_GFX_NATIVE_BC, which
// the Android app sets to 1). Otherwise they are decoded on upload to RGBA8 / R16 / RG16 / RGBA16F (bcdec): 4-8x the bytes
// in memory and in every texture fetch. Adreno 730 samples all of BC1-7. Exception kept: the iOS ASTC pack copies ASTC
// blocks into the game's BC arrays, which needs a decoded destination.
bool native_bc=false;uint64_t native_bc_images=0,decoded_bc_images=0;int64_t image_bytes_live=0,bc_image_bytes_live=0,bc_data_bytes_live=0;   // live image memory (driver sizes) and BC payload
bool no_mutable_images=true;uint64_t mutable_upgrades=0;unsigned mutable_test=0; // mutable_test (marker gfx-mutable-test = N): force the upgrade on every Nth color image when first sampled (test)
struct Image;void upgrade_mutable(Image &im);
bool perf_timing=true; // always-on wall-time breakdown for NVN PERF (exclusive deltas)
// Every scope reads the clock twice (~85k reads per D2R camp frame; ~105 ns each through the vDSO on Adreno 730
// phones, ~19% of the render thread when every frame was timed, still ~4% at one frame in 8). Time one frame in
// perf_sample (marker gfx-perf-sample = N; 1 = every frame, 0 = off; else DM_GFX_PERF_SAMPLE, else 32): 32 leaves about
// two timed frames per 60-frame window. Parts and descriptor lines are averaged over the timed frames. A timed frame still
// took ~4 ms more of the render thread in the D2R camp on the Redmi K50 Ultra and so missed its vsync: one visible hitch
// every 32 frames. The Android app turns it off (DM_GFX_PERF_SAMPLE=0); measurement runs set the marker.
unsigned perf_sample=32;bool perf_frame_timed=true;uint64_t perf_timed_frames=0;
XbgCpuTiming::Counter perf_timing_before[XbgCpuTiming::Count];
bool descriptor_detail=false;
XbgDescriptorTiming descriptor_timing;
thread_local XbgCpuTiming::Scope *descriptor_root=nullptr;
struct DescriptorRoot {
    bool on=descriptor_detail || !tls_gate;XbgCpuTiming::Scope *previous=nullptr;
    DescriptorRoot(XbgCpuTiming::Scope *scope){if(on){previous=descriptor_root;descriptor_root=descriptor_detail?scope:nullptr;}}
    ~DescriptorRoot(){if(on)descriptor_root=previous;}
};
#define DESC_SCOPE(kind) XbgDescriptorTiming::Scope descriptor_scope(descriptor_detail?&descriptor_timing:nullptr,(descriptor_detail || !tls_gate)?descriptor_root:nullptr,XbgDescriptorTiming::kind)
#define DESC_CALL(kind, expr) [&](){DESC_SCOPE(kind);return (expr);}()

bool draw_detail=false;
XbgDrawTiming draw_timing;
thread_local XbgCpuTiming::Scope *draw_root=nullptr;
struct DrawRoot {
    bool on=draw_detail || !tls_gate;XbgCpuTiming::Scope *previous=nullptr;
    DrawRoot(XbgCpuTiming::Scope *scope){if(on){previous=draw_root;draw_root=draw_detail?scope:nullptr;}}
    ~DrawRoot(){if(on)draw_root=previous;}
};
#define DRAW_SCOPE(kind) XbgDrawTiming::Scope draw_scope(draw_detail?&draw_timing:nullptr,(draw_detail || !tls_gate)?draw_root:nullptr,XbgDrawTiming::kind)
bool live_profile_used=false;
struct PipelineCounts {uint64_t hits=0,misses=0,evictions=0,bypass=0;} pipeline_counts,pipeline_before;
void report_pipeline_cache();
struct TimingRoot {
    XbgCpuTiming *previous=timing_current;bool counted=false;
    TimingRoot(){timing_current=(timing_active||(perf_timing&&perf_frame_timed))?&timing:nullptr;if(timing_current){++timing_any;counted=true;}}
    ~TimingRoot(){timing_current=previous;if(counted)--timing_any;}
};
inline XbgCpuTiming *timing_now(){return (!tls_gate || timing_any.load(std::memory_order_relaxed))?timing_current:nullptr;}
#define CPU_SCOPE(kind) XbgCpuTiming::Scope cpu_scope(timing_now(),XbgCpuTiming::kind)
// Hot-path sub-timers: free on untimed frames.
inline uint64_t perf_clock(){return timing_now()?timing_clock():0;}
void timing_start(){
    timing_active=true;timing_start_ns=timing_clock();pipeline_before=pipeline_counts;
    timing_before={stats.submissions.load(),stats.draws.load(),stats.dispatches.load(),stats.presents.load(),stats.shader_loads.load()};
}
void descriptor_report();
void draw_report();
void timing_report(const char *reason){
    if(!timing_active)return;
    uint64_t elapsed=timing_clock()-timing_start_ns;timing_active=false;timing_first=0;
    uint64_t measured=0;for(const auto &c:timing.counters)measured+=c.exclusive_ns;
    xbg_logf("NVN CPU_TIMING v=1 reason=%s first=%llu last=%llu frames=%llu wall_ns=%llu measured_ns=%llu uncovered_ns=%llu draws=%llu dispatches=%llu submissions=%llu shaders=%llu",reason,
        (unsigned long long)(timing_before.presents+1),(unsigned long long)stats.presents.load(),(unsigned long long)(stats.presents-timing_before.presents),
        (unsigned long long)elapsed,(unsigned long long)measured,(unsigned long long)(elapsed>=measured?elapsed-measured:0),
        (unsigned long long)(stats.draws-timing_before.draws),(unsigned long long)(stats.dispatches-timing_before.dispatches),
        (unsigned long long)(stats.submissions-timing_before.submissions),(unsigned long long)(stats.shader_loads-timing_before.shader_loads));
    report_pipeline_cache();
    static const char *names[]={"other","draw","dispatch","descriptor","pass","buffer","pipeline","submit","fence","present","cleanup","hash","image","log","program","cpuwrite","snapshot"};
    for(unsigned i=0;i<XbgCpuTiming::Count;i++){const auto &c=timing.counters[i];xbg_logf("NVN CPU_TIMING part=%s calls=%llu exclusive_ns=%llu inclusive_ns=%llu max_ns=%llu",names[i],(unsigned long long)c.calls,(unsigned long long)c.exclusive_ns,(unsigned long long)c.inclusive_ns,(unsigned long long)c.max_ns);}
    descriptor_report();
    draw_report();
}
void descriptor_report(){
    if(!descriptor_detail)return;
    const uint64_t total=timing.counters[XbgCpuTiming::Descriptor].exclusive_ns;
    const uint64_t tagged=descriptor_timing.sum();
    xbg_logf("NVN DESC_TIMING v=2 first=%llu last=%llu descriptor_ns=%llu tagged_ns=%llu remaining_ns=%llu coverage=%s clock_reads=%llu",(unsigned long long)(timing_before.presents+1),(unsigned long long)stats.presents.load(),(unsigned long long)total,(unsigned long long)tagged,(unsigned long long)(tagged<=total?total-tagged:0),tagged<=total && !descriptor_timing.invalid && !descriptor_timing.top?"OK":"INVALID",(unsigned long long)descriptor_timing.reads);
    const char *names[]={"setup","buffer_bind","image_lookup","image_transition","image_view","sampler_bind","element_rest"};
    for(unsigned i=0;i<XbgDescriptorTiming::Count;++i)xbg_logf("NVN DESC_TIMING part=%s calls=%llu exclusive_ns=%llu",names[i],(unsigned long long)descriptor_timing.counters[i].calls,(unsigned long long)descriptor_timing.counters[i].ns);
    xbg_logf("NVN DESC_TIMING part=remaining calls=0 exclusive_ns=%llu",(unsigned long long)(tagged<=total?total-tagged:0));
}
void draw_report(){
    if(!draw_detail)return;
    const uint64_t total=timing.counters[XbgCpuTiming::Draw].exclusive_ns;
    const uint64_t tagged=draw_timing.finish(total);
    const uint64_t remaining=tagged<=total?total-tagged:0;
    xbg_logf("NVN DRAW_TIMING v=1 first=%llu last=%llu draw_ns=%llu tagged_ns=%llu remaining_ns=%llu coverage=%s clock_reads=%llu flags=%u",(unsigned long long)(timing_before.presents+1),(unsigned long long)stats.presents.load(),(unsigned long long)total,(unsigned long long)tagged,(unsigned long long)remaining,draw_timing.flags?"INVALID":"OK",(unsigned long long)draw_timing.reads,draw_timing.flags);
    const char *names[]={"writeback","vertex_prepare"};
    for(unsigned i=0;i<XbgDrawTiming::Count;++i)xbg_logf("NVN DRAW_TIMING part=%s calls=%llu exclusive_ns=%llu",names[i],(unsigned long long)draw_timing.counters[i].calls,(unsigned long long)draw_timing.counters[i].ns);
    xbg_logf("NVN DRAW_TIMING part=remaining calls=0 exclusive_ns=%llu",(unsigned long long)remaining);
    xbg_logf("NVN DRAW_TIMING counters storage_ranges=%llu images_visited=%llu writeback_bytes=%llu index_expand_calls=%llu index_expand_bytes=%llu",(unsigned long long)draw_timing.storage_ranges,(unsigned long long)draw_timing.images_visited,(unsigned long long)draw_timing.writeback_bytes,(unsigned long long)draw_timing.index_expand_calls,(unsigned long long)draw_timing.index_expand_bytes);
}
void timing_boundary(){
    if(timing_active){if(stats.presents>=timing_last)timing_report("frames");else if(timing_clock()-timing_start_ns>=60000000000ull)timing_report("60s-next-present");}
    else if(timing_first && stats.presents+1==timing_first)timing_start();
}
// 最近一个统计窗的每帧 GPU / 翻译 CPU 毫秒（宿主 hle_oe.c 的 GPU 时间 3D 比例控制器、hle_nvn.c 的自适应出图档读）| per-frame GPU / translation CPU ms of the last
std::atomic<double> perf_gpu_ms{-1},perf_cpu_ms{-1};
extern "C" double xbg_recent_gpu_ms(void){return perf_gpu_ms.load();}
extern "C" double xbg_recent_cpu_ms(void){return perf_cpu_ms.load();}
bool sync_mode=false; // diagnostic: marker files/gfx-sync-mode resolves all GPU results at each submit end
std::atomic<uint64_t> completed{0};uint64_t submitted=0; // guest fence values: submitted xbg_submit calls / GPU-complete ones (see Slot::covers)
// 完成通知（鸿蒙缺省开，DM_GFX_COMPLETION_WAIT=0/1 改）：游戏线程等 fence 时，目标批次一完成就从发布点被唤醒返回，不必排在后续提交后面等引擎锁。
// 关着时发布点与原来一样直接写 completed。
// Completion notification (on by default on HarmonyOS; DM_GFX_COMPLETION_WAIT=0/1 overrides): a game thread waiting on a fence is woken at the publish point as soon as its
// batch completes, instead of queueing for the engine lock behind later submits. Off, the publish point writes completed directly, as before.
// ifdef __OHOS__
#ifdef __OHOS__
constexpr bool completion_wait_default=true;
#else
constexpr bool completion_wait_default=false;
#endif
bool completion_wait_enabled=completion_wait_default;
// 栅栏等待是否持引擎锁（旧路径）：鸿蒙缺省不持，标记 gfx-legacy-waitall = 1 退回
// ifdef __OHOS__
#ifdef __OHOS__
bool legacy_waitall=false;
#else
bool legacy_waitall=true;
#endif
xbg_completion::Completion completion_wait(completed);
void publish_completed(uint64_t value){
    // 只在原写回完成点发布；关着时不取 completion 小锁，开关仅初始化时读取。
    // Publish only at the original writeback-complete points; off, the small lock is never taken; the mode is fixed at initialization.
    if(completion_wait_enabled)completion_wait.publish(value);else completed=value;
}
VkCommandPool command_pool{};
VkPipelineCache pipeline_cache{};
VkPhysicalDeviceProperties properties{};
VkPhysicalDeviceMemoryProperties memories{};
uint32_t timestamp_bits=0;
VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
void need(bool condition,const char *text) { if (!condition) xbg_fatalf("%s",text); }
void check(VkResult r,const char *call) { if(r!=VK_SUCCESS) xbg_fatalf("%s failed: %d",call,int(r)); }
#define VK(call) check((call),#call)
uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memories.memoryTypeCount;i++)
        if((bits&(1u<<i)) && (memories.memoryTypes[i].propertyFlags&flags)==flags) return i;
    xbg_fatalf("No Vulkan memory type for flags %#x",flags);
}
// ---------------------------------------------------------------------------
// Deferred submission. All GPU work of a guest submission is recorded into one
// command buffer (per frame slot) instead of submit+wait per operation. Every
// operation still starts with a full memory barrier, so GPU-side ordering and
// visibility match the former one-submit-per-operation model. The CPU waits
// only where it needs GPU results (storage writeback, readback, queries).
// Guest memory read by the GPU (uniform/storage/vertex/index data, uploads) is
// snapshotted into host arenas at record time, exactly as before.
// ---------------------------------------------------------------------------
uint32_t find_memory(uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred){
    for(uint32_t i=0;i<memories.memoryTypeCount;i++)
        if((bits&(1u<<i)) && (memories.memoryTypes[i].propertyFlags&(required|preferred))==(required|preferred)) return i;
    return memory_type(bits,required);
}
std::map<VkFormat,VkFormatProperties> format_property_cache;
// Core formats (< 256) answer from a flat table: format_properties is asked for every vertex attribute of every draw.
VkFormatProperties format_flat[256];bool format_flat_known[256];
const VkFormatProperties &format_properties(VkFormat f){
    if(uint32_t(f)<256){if(!format_flat_known[f]){vkGetPhysicalDeviceFormatProperties(cfg.physical,f,&format_flat[f]);format_flat_known[f]=true;}return format_flat[f];}
    auto it=format_property_cache.find(f);if(it!=format_property_cache.end())return it->second;
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(cfg.physical,f,&p);return format_property_cache.emplace(f,p).first->second;
}
struct ArenaChunk{VkBuffer buffer{};VkDeviceMemory memory{};uint8_t *mapped{};VkDeviceSize size=0,used=0;bool coherent=true;};
constexpr VkBufferUsageFlags arena_usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
constexpr VkDeviceSize arena_chunk_bytes=32ull*1024*1024;
constexpr VkDeviceSize arena_flush_bytes=96ull*1024*1024;
VkDeviceSize arena_alignment=256;
struct Buffer {
    // A view into an arena chunk. Arena memory lives until the owning frame slot
    // is retired after its fence, so no per-buffer destruction is needed.
    VkBuffer buffer{}; VkDeviceMemory memory{}; VkDeviceSize offset=0; size_t size{}; void *mapped{}; bool coherent=true;
    void flush(){if(coherent)return;VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};r.memory=memory;r.size=VK_WHOLE_SIZE;VK(vkFlushMappedMemoryRanges(cfg.device,1,&r));}
    void invalidate(){if(coherent)return;VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};r.memory=memory;r.size=VK_WHOLE_SIZE;VK(vkInvalidateMappedMemoryRanges(cfg.device,1,&r));}
};
struct Arena {
    std::vector<ArenaChunk> chunks;size_t current=0;bool cached=false;VkDeviceSize used_total=0,peak=0;
    ArenaChunk make(VkDeviceSize bytes){
        CPU_SCOPE(Buffer);ArenaChunk c;c.size=bytes;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=arena_usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;VK(vkCreateBuffer(cfg.device,&ci,nullptr,&c.buffer));
        VkMemoryRequirements r;vkGetBufferMemoryRequirements(cfg.device,c.buffer,&r);
        uint32_t type=cached?find_memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT):find_memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(cached && !(memories.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT))type=find_memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        c.coherent=(memories.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        {static int logged[2];if(!logged[cached]++)xbg_logf("NVN arena chunk cached=%d memoryType=%u flags=%#x heap=%u",int(cached),type,unsigned(memories.memoryTypes[type].propertyFlags),memories.memoryTypes[type].heapIndex);}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=type;
        VK(vkAllocateMemory(cfg.device,&ai,nullptr,&c.memory));VK(vkBindBufferMemory(cfg.device,c.buffer,c.memory,0));
        void *p=nullptr;VK(vkMapMemory(cfg.device,c.memory,0,VK_WHOLE_SIZE,0,&p));c.mapped=static_cast<uint8_t*>(p);return c;
    }
    Buffer alloc(size_t n){
        need(n && n<=256ull*1024*1024,"Invalid or excessive buffer size");
        for(size_t i=current;;++i){
            if(i==chunks.size())chunks.push_back(make(std::max<VkDeviceSize>(arena_chunk_bytes,(n+arena_alignment-1)&~(arena_alignment-1))));
            ArenaChunk &c=chunks[i];VkDeviceSize at=(c.used+arena_alignment-1)&~(arena_alignment-1);
            if(at+n>c.size)continue;
            current=i; // leftovers in skipped chunks are abandoned until reset
            c.used=at+n;used_total+=n;peak=std::max(peak,used_total);
            Buffer b;b.buffer=c.buffer;b.memory=c.memory;b.offset=at;b.size=n;b.mapped=c.mapped+at;b.coherent=c.coherent;return b;
        }
    }
    // reset() keeps all memory mapped (a synchronous reader may still copy out
    // of it); trim() releases oversized/extra chunks before the next recording.
    size_t keep_chunks=3;
    void reset(){for(auto &c:chunks)c.used=0;current=0;used_total=0;}
    void trim(){
        size_t kept=0;std::vector<ArenaChunk> keep;
        for(auto &c:chunks){if(c.size==arena_chunk_bytes && kept<keep_chunks){keep.push_back(c);++kept;}else destroy(c);}
        chunks.swap(keep);current=0;
    }
    static void destroy(ArenaChunk &c){if(c.mapped)vkUnmapMemory(cfg.device,c.memory);if(c.buffer)vkDestroyBuffer(cfg.device,c.buffer,nullptr);if(c.memory)vkFreeMemory(cfg.device,c.memory,nullptr);c=ArenaChunk{};}
    void release(){for(auto &c:chunks)destroy(c);chunks.clear();current=0;used_total=0;}
};
// Deferred destruction until the GPU finished the slot that referenced objects.
struct Retire {
    std::vector<std::function<void()>> handles;std::vector<std::shared_ptr<void>> objects;
    void run(){for(auto &h:handles)h();handles.clear();objects.clear();}
};
// Deferred GPU->CPU results. Storage writebacks and counter reports are
// resolved at the end of each guest submission (when the guest may observe
// completion) or earlier if anything touches the affected guest memory.
struct EpochQuery {const void *slot;VkQueryPool pool;uint32_t index;uint64_t value;bool known;};
// Occlusion counters accumulate from the last reset; the game may never reset, so queries are kept
// incrementally: known prefix and folded prefix sum advance once, consumed entries are dropped (C103).
struct EpochState{std::deque<EpochQuery> q;uint64_t base=0,known_upto=0,summed_upto=0,sum=0;uint64_t size()const{return base+q.size();}
    void advance_known(){while(known_upto<size() && q[known_upto-base].known)++known_upto;}
    uint64_t prefix(uint64_t count){while(summed_upto<count && summed_upto<size()){sum+=q[summed_upto-base].value;++summed_upto;}return sum;}
    void trim(){while(base<summed_upto && !q.empty()){q.pop_front();++base;}}};
uint64_t counter_epoch=0;std::map<uint64_t,EpochState> epochs;
struct PendingReport {uint64_t addr,type,epoch;size_t count;const void *slot;VkQueryPool ts;uint32_t ts_index;uint64_t ts_value;bool ts_known;};
std::vector<PendingReport> pending_reports;
// Command-buffer SignalEvent (4 per frame in D2R) completes when the GPU reaches it. Completing all work at the signal stalls
// the CPU on the GPU at every signal; marker file gfx-defer-events = '1' instead submits the work so far without waiting and
// publishes the value when that submission's slot retires (as the Metal backend writes it from a completion handler).
// '2' does not submit at the signal: the value waits in the slot being recorded and is published when that slot retires,
// so a frame stays one submission and the 2-slot ring does not wrap inside it.
// The game reads event values only through nvnEventGetValue, and the host's CPU event initialize, finalize and signal land a
// pending value first (xbg_game_event_read); a command-buffer wait on an event with a pending value completes the GPU work
// first (main's 07ea121). Without the marker the host may set the default through DM_GFX_DEFER_EVENTS (the Android app sets
// 2); a marker '0' turns deferral off.
struct PendingEvent {uint64_t addr;uint32_t mode,v;const void *slot;};
std::vector<PendingEvent> pending_events,recording_events;   // recording_events: mode 2, slot not yet submitted
int defer_events=0;uint64_t events_deferred=0,event_reads=0;
struct PendingWriteback {uint64_t addr,bytes;Buffer buffer;const void *slot;};
std::vector<PendingWriteback> pending_writebacks;
// Storage writebacks superseded by a newer GPU result (gfx-wb-skip). In the D2R camp five compute outputs, 1.37 MB a frame
// (1.25 MB of it a buffer that changes by a few KB), were written back at every retire although nothing read them from guest
// memory: the next frame's dispatches and draws take them from the pending GPU copy (pending_view), and by the time a slot
// retires the next frame has recorded a newer writeback of the same ranges. Reading them back took ~1 ms a frame on Adreno
// 730, whose host-visible memory reads at 1.3 GB/s. A writeback is dropped when a newer pending writeback covers its range,
// the range lies in a pool the game's CPU never reads (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) and no image aliases it (image
// uploads read guest memory directly). Engine reads still get the newest result: the newer pending writeback until its slot
// retires, guest memory after. Off unless the host asks (DM_GFX_WB_SKIP, which the Android app sets to 1); marker
// gfx-wb-skip = 1 / 0; A/B target wbskip.
bool wb_skip=false;uint64_t wb_skipped=0,wb_skipped_bytes=0;
void cpu_access(uint64_t address,uint64_t bytes,unsigned why=0);   // resolve pending GPU results overlapping a CPU access
void resolve_pending();
void resolve_slot_writebacks(const void *slot);
void resolve_known_reports();
extern bool resolving_pending,read_preflight;
// perf, by caller (0 other, 1 vertex, 2 index, 3 storage, 4 texel, 5 uniform): snap_site_bytes = bytes copied in full,
// snap_site_req = bytes asked for, before the vertex cache, write tracking and pending-result views (their copies are logged
// on their own lines: vcache fills, wtrack copied_kb).
uint64_t snap_site_bytes[6]={},snap_site_req[6]={};unsigned snap_site=0;
std::unordered_set<uint64_t> storage_pages;uint64_t storage_snap_count=0;
namespace wtrack{extern uint64_t copied_bytes,dirty_pages,mismatches,fallbacks;}
extern std::atomic<uint64_t> memo_epoch;extern uint64_t key_diff[14];extern uint64_t memo_bump[5];extern uint64_t xmemo_hits,xmemo_drops;struct PinRef{VkDescriptorSetLayout layout;uint32_t refs;};extern std::unordered_map<VkDescriptorSet,PinRef> memo_pinned[2];
extern uint64_t memo_front_hits;extern uint64_t desc_memo_hits,desc_memo_misses,desc_memo_why[6],desc_ns[6],noload_passes,img_cache_hits,img_cache_misses,noload_why[8],binds_skipped,set_binds,set_bind_offsets,set_binds_offsets_only,offset_changes[10],offset_changed_at[32];
namespace warm{extern uint64_t hits,creates,create_ns,create_max_ns;extern std::atomic<uint64_t> built,failed,build_ns,async_built,async_skips,compiled;extern bool async_on;void flush();void register_module(VkShaderModule m,const std::string &key);
    extern bool keep_rewritten;bool take_rewritten(const std::string &key,std::vector<uint32_t> &out);}
// 长帧日志的读数（nvn_vk.c 把它们接到 hle_nvn.c 的 mtl_create_* / mtl_pkg_stats / mtl_upload_* / mtl_gpu_ms）：自启动起的累计，图形线程写、
// 游戏线程读。只算帧里同步做的：管线现建（预热和后台建的不算）、着色器载入（读文件、SPIR-V 改写、建模块）、贴图整份上传（含解交错与 BC 解码）。
// Readings for the long-frame log (nvn_vk.c wires them to hle_nvn.c's mtl_create_* / mtl_pkg_stats / mtl_upload_* / mtl_gpu_ms): totals since
// start, written on the graphics thread and read on the game thread. Only work done synchronously in a frame counts: pipelines created on the
// spot (not warm-up or background builds), shader loads (file read, SPIR-V rewrites, module creation) and whole texture uploads (with the
// deswizzle and BC decode).
namespace hud{
std::atomic<uint64_t> pipelines{0},pipeline_ns{0},shaders{0},shader_ns{0},uploads{0},reuploads{0},upload_bytes{0},upload_ns{0},frame{0};
// 每帧的 GPU 时间：每个槽的时间戳跨度记到提交它时的 HLE 帧（xbg_game_set_frame）；读的是 8 帧以前的帧，与正在写的不是同一格。
// GPU time per frame: each slot's timestamp span goes to the HLE frame current when it was submitted (xbg_game_set_frame); readers ask for a
// frame 8 frames back, never the entry being written.
struct Gpu{std::atomic<uint64_t> frame{~0ull},ns{0};};Gpu gpu[64];
void gpu_add(uint64_t f,uint64_t ns){Gpu &g=gpu[f%64];if(g.frame.load(std::memory_order_relaxed)!=f){g.ns.store(0,std::memory_order_relaxed);g.frame.store(f,std::memory_order_release);}g.ns.fetch_add(ns,std::memory_order_release);}
}
uint64_t cleanup_ns[5]={},writeback_bytes=0; // perf: cleanup phase split
// Driver/support constant buffers are rebuilt only when their inputs change (iOS: reuse parameter
// blocks for identical inputs). Generations bump on binding ops; slot_serial on every frame-slot switch.
uint64_t tex_gen[6]={},buf_gen[6]={},slot_serial=1;
struct DriverCache{uint64_t gen=0,slot=0;Buffer buffer;};DriverCache driver_cache[6][2];
struct SupportCache{float w=-1,h=-1;uint64_t slot=0;Buffer buffer;};SupportCache support_cache;
struct SetKeyHash{size_t operator()(const std::vector<uint64_t> &k)const{uint64_t h=1469598103934665603ull;for(uint64_t v:k){h^=v;h*=1099511628211ull;h^=h>>32;}return size_t(h);}};
uint64_t set_cache_hits=0,set_cache_misses=0,uniform_trimmed=0;bool template_enabled=true;uint64_t template_updates=0,plain_updates=0;bool set_cache_enabled=true; // same-process A/B showed no gain on Maleoon
// In-process A/B (iOS method: alternate in the same process, same thermal state). files/gfx-ab-toggle holding
// N (seconds) flips ab_on every N seconds; ab_on=false disables the optimization under test. Perf lines carry ab=.
int ab_period=0;bool ab_on=true;
bool ab_frame_on=true;
// Which optimization gfx-ab-toggle switches off in its off phase (marker gfx-ab-target: wtrack, pipefast, query or timestamps).
char ab_target[16]={0};
inline bool ab_off(const char *name){return ab_period>0 && !ab_frame_on && !strcmp(ab_target,name);}
bool ab_enabled(){if(ab_period<=0)return true;ab_on=((std::chrono::steady_clock::now().time_since_epoch().count()/1000000000ll/ab_period)&1)==0;return ab_on;}
struct Slot {
    bool reset_queued=false,reset_busy=false;VkResult reset_result=VK_SUCCESS;   // poolreset: a reset job was queued for this submission / still runs (under poolreset::mu)
    // xview: this submission read a pending result of the other slot through pending_view (from its readback arena).
    bool xview=false;
    // 提交时的 HLE 帧：退休时这次提交的 GPU 时间记到它名下（hud::gpu_add）。
    // The HLE frame at submission: on retire this submission's GPU time is added to it (hud::gpu_add).
    uint64_t hud_frame=0;
    struct WaitCount{std::atomic<int> n{0};WaitCount()=default;WaitCount(const WaitCount&){}WaitCount &operator=(const WaitCount&){n.store(0);return *this;}} waiters;   // 不持引擎锁等本槽栅栏的线程数（xbg_legacy_wait）；复位栅栏前须等它们离开 | threads waiting on this slot's fence without the engine lock (xbg_legacy_wait); the fence is reset only after they leave
    VkCommandBuffer cmd{};VkCommandPool pool{};VkFence fence{};VkQueryPool timestamps{};bool recording=false,pending=false;uint64_t ops=0,covers=0; // covers: guest fence value complete once this slot retires
    std::vector<VkQueryPool> occlusion_pools,timestamp_pools;uint32_t occlusion_used=0,timestamp_used=0;
    Retire retire;Arena stream,readback;std::vector<VkDescriptorPool> pools;size_t pool_next=0;
    // gfx-xview-guard (recording_slot, readback_buffer): ranges of this slot's readback buffers the other slot read through
    // pending views (viewed), the ranges guarded during this recording, and the copies and fills that write guarded
    // buffers from the GPU in a prologue command buffer submitted ahead of cmd.
    struct XRange{VkBuffer buffer;VkDeviceSize begin,end;};struct ProOp{VkBuffer src,dst;VkDeviceSize src_off,dst_off,size;};
    std::vector<XRange> viewed,guard;std::vector<ProOp> pro_ops;VkCommandBuffer pro{};
    // gfx-upload-prologue：序言命令缓冲里整份上传的图像（from：这次提交开始时它的布局）。
    // gfx-upload-prologue: whole image uploads the prologue command buffer performs (from: the image's layout when this submission starts).
    struct ProImage{VkImage image;VkImageLayout from;VkImageAspectFlags aspects;uint32_t levels,layers;VkBuffer buffer;std::vector<VkBufferImageCopy> copies;};
    std::vector<ProImage> pro_images;
    std::map<std::vector<uint64_t>,VkFramebuffer> framebuffers;
    std::map<std::array<uint64_t,4>,VkBufferView> texel_views; // per-frame texel views (same snapshot -> one view)
    Buffer query_results;size_t query_copied=0; // occlusion results copied by the GPU at the end of the frame
    std::vector<std::pair<uint64_t,uint64_t>> query_refs; // (epoch, absolute index) per used occlusion query, in result order
    std::unordered_map<std::vector<uint64_t>,VkDescriptorSet,SetKeyHash> set_cache; // identical descriptor contents -> one set per frame
    std::unordered_map<VkDescriptorSetLayout,std::vector<VkDescriptorSet>> spare_sets; // batch-allocated, reset with the pools
    struct PassRec {
        uint32_t q0=0,q1=0,w=0,h=0,att=0,fmt=0,draws0=0,draws1=0,x=0,y=0,z=0;uint64_t verts0=0,verts1=0;
        bool compute=false,complete=false,mixed=false,dz=false;std::string first,last;
    };
    bool prof=false;VkQueryPool prof_pool{};uint32_t prof_used=0,prof_slot=0,prof_skipped=0;uint64_t prof_frame=0;
    std::vector<PassRec> prof_recs;int prof_open=-1;
    std::unordered_map<VkDescriptorSetLayout,std::vector<VkDescriptorSet>> used_sets;
    std::vector<struct Descriptors*> desc_used; // pooled per-draw descriptor records, returned after this slot's fence // handed out this frame; recycled into spare_sets after the slot fence (no per-frame pool reset)
};
Slot slots[2];unsigned slot_index=0;
// Pending views of the other slot's results (xviews), and arena reuse while a slot with such views was in flight (races).
// gfx-xview-guard (marker 1 / 0, else DM_GFX_XVIEW_GUARD; A/B target xguard): at such a reuse, a new readback buffer that
// overlaps a range the reader viewed is not written by the CPU; its contents go to a stream buffer and a prologue command
// buffer copies them in after a barrier, so the reader's work is done first (guarded allocations a frame and their KB). The
// buffers stay where they were: moving them to another arena or another half of the chunk cost 1.72-1.85 ms of CPU a frame,
// because about 145 memoized descriptor sets a frame bind read-only storage at a fixed buffer and offset.
uint64_t xviews=0,xview_races=0,xview_guarded=0,xview_guarded_bytes=0;bool xview_guard=false;
// gfx-upload-prologue（标记 1 / 0，否则 DM_GFX_UPLOAD_PROLOGUE；A/B 目标 uppro）：这次提交里还没用过的图像，整份上传改由序言命令缓冲在提交之前做，
// 不再在帧中间结束打开的通道再加两道全屏障（每结束一次，分块 GPU 就要把通道的附件写出再读回）。计数：进序言的、仍在命令缓冲里做的、其中打断了通道的。
// gfx-upload-prologue (marker 1 / 0, else DM_GFX_UPLOAD_PROLOGUE; A/B target uppro): a whole upload of an image not yet used in this
// submission is done by the prologue command buffer ahead of it, instead of ending the open pass in mid-frame with two full barriers
// (each end makes a tiling GPU write the pass's attachments out and read them back). Counted: uploads in the prologue, uploads still in
// the command buffer, and those of them that ended an open pass.
uint64_t uploads_prologue=0,uploads_inpass=0,uploads_broke_pass=0;bool upload_prologue=false;
// gfx-shader-reuse（标记 1 / 0 / 2，否则 DM_GFX_SHADER_REUSE；A/B 目标 shreuse）：着色器载入直接用预热线程改写好的 SPIR-V（quad、warp、
// tidy 之后），不在提交线程上再改写；Redmi K50 Ultra 上改写占载入时间的约 80%，大的片段着色器一个 6-9 ms。2：照样改写并比对，用改写的结果。
// gfx-shader-reuse (marker 1 / 0 / 2, else DM_GFX_SHADER_REUSE; A/B target shreuse): a shader load takes the SPIR-V the warm-up threads
// already rewrote (after quad, warp and tidy) instead of rewriting it again on the submit thread; on the Redmi K50 Ultra the rewrites
// are about 80% of a load, 6-9 ms for a large fragment shader. 2: rewrite as well and compare, using the rewritten result.
int shader_reuse=0;uint64_t shaders_reused=0,shader_reuse_mismatches=0;
// Command pool resets off the render thread. vkResetCommandPool took ~0.4 ms of every frame on the Redmi K50 Ultra's render
// thread. After a slot is submitted, the reset thread waits for its fence and resets its pool; retiring the slot first waits
// for that job, so vkResetFences never overlaps the thread's wait (concurrent waits are allowed), and reusing the slot skips
// its reset. Marker gfx-async-pool-reset = 1 / 0, else DM_GFX_ASYNC_POOL_RESET (the Android app sets 1); A/B target
// poolreset, decided per submission. NVN PERF poolreset: resets queued, retirements that had to wait for one, and how long.
namespace poolreset {
bool enabled=false,stop=false;std::thread *worker=nullptr;std::mutex mu;std::condition_variable cv;   // never destroyed: a joinable std::thread left at exit would terminate
struct Job{Slot *slot;VkFence fence;VkCommandPool pool;};std::vector<Job> jobs;
uint64_t queued=0,waits=0,wait_ns=0;   // render thread only
void run(){
    for(;;){Job j;{std::unique_lock<std::mutex> l(mu);cv.wait(l,[]{return stop || !jobs.empty();});if(jobs.empty())return;j=jobs.front();jobs.erase(jobs.begin());}
        VkResult r=vkWaitForFences(cfg.device,1,&j.fence,VK_TRUE,std::max<uint64_t>(cfg.fence_timeout_ns,10000000000ull));
        if(r==VK_SUCCESS)r=vkResetCommandPool(cfg.device,j.pool,0);
        {std::lock_guard<std::mutex> l(mu);j.slot->reset_result=r;j.slot->reset_busy=false;}cv.notify_all();}
}
void start(){if(enabled && !worker){stop=false;worker=new std::thread(run);}}
void queue(Slot &s){if(!s.pool || !worker || ab_off("poolreset"))return;{std::lock_guard<std::mutex> l(mu);s.reset_busy=true;s.reset_queued=true;jobs.push_back({&s,s.fence,s.pool});}cv.notify_one();++queued;}
// Wait for this slot's job, if one is queued: before its fence is reset and before its pool is reused.
void finish(Slot &s){if(!s.reset_queued)return;std::unique_lock<std::mutex> l(mu);if(s.reset_busy){uint64_t t0=timing_clock();cv.wait(l,[&]{return !s.reset_busy;});wait_ns+=timing_clock()-t0;++waits;}}
void drain(){if(!worker)return;{std::lock_guard<std::mutex> l(mu);stop=true;}cv.notify_all();worker->join();delete worker;worker=nullptr;}
}
// Where the CPU blocks on a busy slot fence, and why slots are submitted (NVN PERF block_ms).
// The outermost site wins, except that a ring wrap in recording_slot is always counted as ring.
enum SyncSite{SITE_OTHER,SITE_RING,SITE_PRESENT,SITE_EVENT,SITE_ARENA,SITE_GPU_SYNC,SITE_COMPLETE,SITE_POLL,SITE_LEGACY_WAIT,SITE_COUNT};
int sync_site=SITE_OTHER;uint64_t block_ns[SITE_COUNT],block_calls[SITE_COUNT],submit_by[SITE_COUNT],legacy_wait_ns=0;
void fold_completion_wait_stats(){
    // 调用者持 engine；ns/calls 在同一 completion 小锁下成对取清，快返路径不写普通统计。
    // Caller holds engine; take/reset ns and calls as a pair under completion's lock, never writing plain counters on a lock-free return.
    const auto pending=completion_wait.take_gpu_waits();
    block_ns[SITE_LEGACY_WAIT]+=pending.ns;block_calls[SITE_LEGACY_WAIT]+=pending.calls;legacy_wait_ns+=pending.ns;
}
struct SiteScope{int old;explicit SiteScope(int site,bool force=false):old(sync_site){if(force || sync_site==SITE_OTHER)sync_site=site;}~SiteScope(){sync_site=old;}};
uint64_t write_seq=1,barrier_seq=0; // image writes vs. last full barrier (render-pass merging)
// Render-pass merging: consecutive draws into the same framebuffer share one
// render pass. Any non-draw operation, a texture read of something written
// since the last barrier, or a layout change ends the open pass first.
struct Image;
void bind_cache_reset();
// Attachment images of a pass (8 color + depth): fixed capacity, so building a Pass per draw does not allocate.
struct ImageList{Image *v[9];uint32_t n=0;void push_back(Image *i){if(n<9)v[n++]=i;}Image **begin(){return v;}Image **end(){return v+n;}Image *const *begin()const{return v;}Image *const *end()const{return v+n;}
    size_t size()const{return n;}bool empty()const{return !n;}Image *operator[](size_t i)const{return v[i];}};
// depth_attached / depth_used (NVN PERF pass_ends depth_idle, NVN GPASS dz=1): a pass that carries a depth attachment no
// draw tests and no clear touches; a tiler loads and stores that attachment for nothing.
struct OpenPass {VkRenderPass pass{};VkFramebuffer framebuffer{};VkCommandBuffer cmd{};ImageList images;bool depth_attached=false,depth_used=false;} open_pass;
// Framebuffers of cached render passes, kept across frames (creating and destroying one per pass per frame was ~5% of the
// render thread on Adreno 730). Each attachment Image lists the keys of the framebuffers that use its views; the first of
// them to be destroyed (all GPU work that used it is complete by then) takes the framebuffer along. gfx-no-persistent-fb
// keeps the former per-slot cache.
std::map<std::vector<uint64_t>,VkFramebuffer> persistent_framebuffers;bool persistent_fb_enabled=true;uint64_t fb_hits=0,fb_creates=0;
void forget_framebuffers(Image &im,std::vector<VkFramebuffer> *deferred=nullptr);
// Per-slot command pools reset whole (vkResetCommandPool) instead of resetting the command buffer: the buffer reset was
// 3.6% of the render thread on Adreno 730. gfx-no-slot-pools keeps the shared pool. Readback arenas in host-cached memory
// (gfx-readback-cached = '1'): GPU results read back by the CPU (storage writebacks) come from uncached memory otherwise.
bool slot_pools_enabled=true,readback_cached=false;
// Experiment (marker gfx-no-occlusion = '1'): no occlusion queries at all; counter reports then read 0 samples. Measures
// what the per-draw precise queries cost the GPU (count_samples stays on after the game's first counter reset). A/B
// target occ does the same in the off half (on the Redmi K50 Ultra bench the GPU then took 1.75 ms a frame more).
// samples_report_seen: the game has recorded a samples-passed report (D2R never does: its ~8 pending reports a frame are
// all timestamps).
bool heavy_timestamps=false;extern uint64_t pass_fast_hits;   // gfx-heavy-timestamps = '1': timestamp reports end the pass with full barriers, as before
bool no_occlusion_queries=false,samples_report_seen=false;uint64_t samples_reports=0,samples_reported=0,counter_ops[3]={};extern uint64_t pipeline_fast_hits;extern bool pipe_memo;extern uint64_t pipe_memo_hits,pipe_memo_misses;extern uint64_t snap_front_hits,snap_map_hits,snap_invalidations,snap_invalidated;extern std::atomic<uint64_t> quad_rewrites,warp_rewrites,ldc_folds,temps_forwarded,nan_minmax_swaps;namespace vcache{extern bool enabled;extern uint64_t hits,hit_bytes,fills,fill_bytes,refills,volatiles,untracked,flushes;size_t live();VkDeviceSize arena_bytes();}   // counter_ops: resets, timestamp reports, samples reports recorded
// Resident uniform texel copies (texel_acquire, before descriptors()).
struct TexelKey{uint64_t base,bytes;uint32_t format;bool operator==(const TexelKey &o)const{return base==o.base&&bytes==o.bytes&&format==o.format;}};
struct TexelKeyHash{size_t operator()(const TexelKey &k)const{uint64_t x=k.base*0x9E3779B97F4A7C15ull^k.bytes*0xC2B2AE3D27D4EB4Full^k.format;x^=x>>29;return size_t(x*0xBF58476D1CE4E5B9ull);}};
struct TexelResident{Buffer buffer;VkBufferView view{};uint64_t hash=0,used=0,gen=0,checked=0,check_epoch=0;};   // checked: verified in this submission
std::unordered_map<TexelKey,TexelResident,TexelKeyHash> texel_resident[2];Arena texel_arena[2];uint64_t texel_hole_bytes[2]={},texel_live_bytes[2]={};
bool texel_memo_enabled=true;uint64_t texel_gen=0,texel_refreshes=0,texel_fallbacks=0,texel_evictions=0,texel_hits=0;
// A copy verified earlier in this submission stays valid until an in-stream CPU write overlaps it (snapshot_invalidate
// bumps texel_write_epoch), so a ~1 MB range bound by 558 draws is hashed once per frame, not per draw.
uint64_t texel_write_epoch=1,texel_lo=UINT64_MAX,texel_hi=0;void texel_touch(uint64_t address,uint64_t bytes);
void texel_evict(unsigned si,bool all);
// Diagnostic (NVN PERF texel): distinct uniform texel ranges per frame and how many the previous frame also used.
std::unordered_set<uint64_t> texel_frame_keys,texel_prev_keys;uint64_t texel_binds=0,texel_distinct=0,texel_reused=0;
// 显式gfx-pass-profile：每600次呈现采完整一帧的所有slot；整帧上限1024查询/32slot。
// Opt-in gfx-pass-profile: sample all slots of one frame every 600 presents,
// bounded to 1024 timestamp queries and 32 slots per selected frame.
// 标记文件里写「起始 间隔 [帧数]」时按它采样，例如 2115 1 60 = 第 2115 起逐帧采 60 帧（进营地的第一帧落在其中）；空文件同上。
// When the marker holds "FROM PERIOD [COUNT]" frames are sampled by it, e.g. 2115 1 60 = every frame for 60 frames from 2115
// (the camp's first frame falls among them); an empty file samples as above.
constexpr uint32_t pass_prof_query_limit=1024,pass_prof_slot_limit=32;
bool pass_prof_enabled=false;uint64_t pass_prof_next=300,pass_prof_draws=0,pass_prof_frame=UINT64_MAX,pass_prof_period=600,pass_prof_until=UINT64_MAX;
// Vertices (or indices) times instances drawn, for the per-pass verts= of NVN GPASS.
uint64_t pass_prof_verts=0;
uint32_t pass_prof_queries=0,pass_prof_slots=0,pass_prof_skipped_slots=0;
void pass_prof_end(VkCommandBuffer cmd);
void pass_prof_depth_idle(VkCommandBuffer cmd);
uint64_t depth_idle_passes=0;
// Diagnostic (NVN PERF pass_ends): render-pass ends and full barriers per frame, by the guest op being executed
// (pseudo-ops: 60 submit, 61 present, 62 storage-writing draw).
unsigned cur_op=0;uint64_t pass_end_by[64],barrier_by[64];
// Samples-passed counting: one precise occlusion query per run of counted draws inside a pass (Metal counts per pass,
// nvn_mtl.m), not one per draw. A run ends with its pass, at a counter reset, and at a report (which ends the pass), so a
// report still sums exactly the draws since its reset. ~2200 begin/end pairs per D2R camp frame cost the Adreno 730
// ~10 ms of GPU time and ~4 ms of CPU. gfx-query-per-draw = '1' restores one query per draw.
// The run helpers and names are main's (1b5465e); occlusion_batch_enabled is false with gfx-query-per-draw = '1' or a
// gfx-no-query-batch marker.
struct OpenOcclusion {VkCommandBuffer cmd{};VkQueryPool pool{};uint32_t index=0;} open_occlusion;
bool occlusion_batch_enabled=true;uint64_t occlusion_queries_started=0;
void end_occlusion_query(){
    if(open_occlusion.cmd){vkCmdEndQuery(open_occlusion.cmd,open_occlusion.pool,open_occlusion.index);open_occlusion=OpenOcclusion{};}
}
void begin_occlusion_query(VkCommandBuffer cmd,VkQueryPool pool,uint32_t index){
    need(!open_occlusion.cmd,"Nested occlusion query");
    vkCmdBeginQuery(cmd,pool,index,VK_QUERY_CONTROL_PRECISE_BIT);open_occlusion={cmd,pool,index};++occlusion_queries_started;
}
void end_open_pass(){if(open_pass.cmd){end_occlusion_query();vkCmdEndRenderPass(open_pass.cmd);if(open_pass.depth_attached && !open_pass.depth_used){++depth_idle_passes;pass_prof_depth_idle(open_pass.cmd);}pass_prof_end(open_pass.cmd);open_pass=OpenPass{};++pass_end_by[cur_op<64?cur_op:63];}}
void full_barrier(VkCommandBuffer cmd){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);barrier_seq=write_seq;++barrier_by[cur_op<64?cur_op:63];}
struct PerfCounts{size_t pipelines,layouts,samplers,passes,images,host_segments,host_bytes;};
PerfCounts perf_counts();
struct Perf {
    uint64_t frames=0,submits=0,waits=0,sync_writeback=0,sync_readback=0,sync_query=0,sync_other=0,gpu_ns=0,gpu_samples=0,
        submit_cpu_ns=0,present_cpu_ns=0,passes=0,draws=0,dispatches=0,snapshot_hits=0,snapshot_misses=0,pending_views=0,access_by[12]={},folded_clears=0,why_wait=0,why_poll=0,why_access=0,why_cmdwait=0,why_finish=0,why_slot=0,snapshot_bytes=0,arena_peak=0,window_start=0,ops=0;
} perf;
void snapshot_reset();void last_pass_reset();
void release_descriptors(Slot &s);
void snapshot_invalidate(uint64_t address,uint64_t bytes);
// Diagnostic (marker gfx-event-guard): while a SignalEvent's value is deferred (gfx-defer-events), the page holding the event
// word is made inaccessible, so a read of it that bypasses nvnEventGetValue faults and is counted (NVN PERF evguard), then
// the page is opened again. Applying the event opens it first. Pages the dirty tracker or wtrack protect would conflict:
// run it with gfx-vcache=0, gfx-tex-dirty=0 and gfx-wtrack=0.
namespace evguard {
bool enabled=false;constexpr int slots_n=64;
std::atomic<uintptr_t> page[slots_n],word[slots_n];   // guarded page; the pending event word in it (one per slot)
std::atomic<uint64_t> exact{0},other{0},writes{0},guards{0};std::atomic<uintptr_t> last_pc{0},last_addr{0};
// The first faulting access of a guarded page and who made it: the caller (LR; bionic's memcpy is a leaf), x0-x5 at the fault,
// and the pending event word on that page, so a bulk copy that crosses the word can be told from one that stops before it.
std::atomic<uintptr_t> last_lr{0},last_word{0},last_x[6];std::atomic<uintptr_t> callers[8];std::atomic<uint64_t> caller_hits[8];
inline uintptr_t pg(uintptr_t a){return a&~uintptr_t(4095);}
void reclose();
void guard(uint64_t addr){if(!enabled)return;uintptr_t p=pg(uintptr_t(addr));
    for(int i=0;i<slots_n;i++)if(page[i].load()==p && word[i].load()==uintptr_t(addr))return;
    bool shut=false;for(int i=0;i<slots_n;i++)if(page[i].load()==p)shut=true;
    for(int i=0;i<slots_n;i++){uintptr_t z=0;if(word[i].compare_exchange_strong(z,uintptr_t(addr))){page[i]=p;break;}}
    if(!shut){mprotect(reinterpret_cast<void*>(p),4096,PROT_NONE);++guards;}reclose();}
void open(uintptr_t p){mprotect(reinterpret_cast<void*>(p),4096,PROT_READ|PROT_WRITE);for(int i=0;i<slots_n;i++)if(page[i].load()==p){page[i]=0;word[i]=0;}}
// A faulting access opens its page but keeps the pending words; reclose() shuts such pages again (at every guard(), and
// after each engine copy), so later reads of a word that is still pending fault too.
std::atomic<bool> reopened[slots_n];std::atomic<uint64_t> engine_opens{0},recloses{0};
void reclose(){for(int i=0;i<slots_n;i++)if(reopened[i].exchange(false)){uintptr_t p=page[i].load();if(p){mprotect(reinterpret_cast<void*>(p),4096,PROT_NONE);++recloses;}}}
// The engine's own copies of guest memory (stream_buffer) open the guarded pages they cover for the copy and shut them
// after, so the guard stays on for guest reads; they are counted (engine_opens), not reported as faults.
struct EngineRead{uintptr_t pages[8];int n=0;
    EngineRead(const void *src,size_t bytes){if(!enabled || !bytes)return;uintptr_t lo=pg(uintptr_t(src)),hi=pg(uintptr_t(src)+bytes-1);
        for(int i=0;i<slots_n && n<8;i++){uintptr_t q=page[i].load();if(!q || q<lo || q>hi)continue;bool dup=false;for(int k=0;k<n;k++)if(pages[k]==q)dup=true;
            if(!dup){mprotect(reinterpret_cast<void*>(q),4096,PROT_READ|PROT_WRITE);pages[n++]=q;++engine_opens;}}}
    ~EngineRead(){for(int k=0;k<n;k++){bool still=false;for(int i=0;i<slots_n;i++)if(page[i].load()==pages[k])still=true;if(still)mprotect(reinterpret_cast<void*>(pages[k]),4096,PROT_NONE);}}
};
void unguard(uint64_t addr){if(!enabled)return;uintptr_t p=pg(uintptr_t(addr));for(int i=0;i<slots_n;i++)if(page[i].load()==p){open(p);return;}}
int fault(void *a,int w,void *uc){uintptr_t x=uintptr_t(a),p=pg(x);bool mine=false,hit=false;
    uintptr_t near=0;
    for(int i=0;i<slots_n;i++)if(page[i].load()==p){mine=true;uintptr_t e=word[i].load();near=e;if(x>=e && x<e+4)hit=true;}
    if(!mine)return 0;
    ++(hit?exact:other);if(w)++writes;last_addr=x;last_word=near;
#if defined(__aarch64__) && defined(__ANDROID__)
    if(uc){auto &mc=static_cast<ucontext_t*>(uc)->uc_mcontext;last_pc=uintptr_t(mc.pc);uintptr_t lr=uintptr_t(mc.regs[30]);last_lr=lr;
        for(int i=0;i<6;i++)last_x[i]=uintptr_t(mc.regs[i]);
        for(int i=0;i<8;i++){uintptr_t z=0;if(callers[i].load()==lr || callers[i].compare_exchange_strong(z,lr) || callers[i].load()==lr){++caller_hits[i];break;}}}
#endif
    mprotect(reinterpret_cast<void*>(p),4096,PROT_READ|PROT_WRITE);for(int i=0;i<slots_n;i++)if(page[i].load()==p)reopened[i]=true;return 1;}
}
void apply_event(uint64_t addr,uint32_t mode,uint32_t v){
    evguard::unguard(addr);snapshot_invalidate(addr,4);auto ptr=reinterpret_cast<uint32_t*>(addr);
    if(mode==NVN_EVENT_SIGNAL_MODE_ADD)__atomic_fetch_add(ptr,v,__ATOMIC_SEQ_CST);else{need(mode==NVN_EVENT_SIGNAL_MODE_WRITE,"Unknown event mode");__atomic_store_n(ptr,v,__ATOMIC_SEQ_CST);}
}
// Deferred events in recording order; stop at the first whose slot is still in flight (slots retire in submission order).
void apply_ready_events(){
    size_t k=0;for(;k<pending_events.size();++k){const PendingEvent &e=pending_events[k];if(static_cast<const Slot*>(e.slot)->pending)break;apply_event(e.addr,e.mode,e.v);}
    pending_events.erase(pending_events.begin(),pending_events.begin()+k);
}
void retire_slot(Slot &s){
    if(!s.pending)return;
    poolreset::finish(s);
    {CPU_SCOPE(Fence);bool busy=vkGetFenceStatus(cfg.device,s.fence)==VK_NOT_READY;uint64_t b0=busy?timing_clock():0;
     VK(vkWaitForFences(cfg.device,1,&s.fence,VK_TRUE,std::max<uint64_t>(cfg.fence_timeout_ns,10000000000ull)));
     if(busy){block_ns[sync_site]+=timing_clock()-b0;++block_calls[sync_site];}}
    ++perf.waits;
    if(s.prof){
        s.prof=false;
        // 栅栏不替代查询可用性检查；按有效位数计算环绕差，只报告完整查询对。
        // Check availability as well as the fence; mask timestamp wrap to valid bits.
        struct Stamp{uint64_t value=0,available=0;};std::vector<Stamp> t(s.prof_used);
        VkResult status=s.prof_used?vkGetQueryPoolResults(cfg.device,s.prof_pool,0,s.prof_used,t.size()*sizeof(Stamp),t.data(),sizeof(Stamp),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT):VK_SUCCESS;
        const uint64_t mask=timestamp_bits<64?(uint64_t(1)<<timestamp_bits)-1:~0ull;
        double per=properties.limits.timestampPeriod/1000.0,pass_us=0,compute_us=0,span_us=-1;size_t complete=0;
        uint64_t first=0,last=0;
        if(status==VK_SUCCESS || status==VK_NOT_READY)for(size_t i=0;i<s.prof_recs.size();++i){const auto &r=s.prof_recs[i];
            if(!r.complete || r.q0>=t.size() || r.q1>=t.size() || !t[r.q0].available || !t[r.q1].available)continue;
            double us=double((t[r.q1].value-t[r.q0].value)&mask)*per;
            if(!complete)first=t[r.q0].value;last=t[r.q1].value;++complete;
            if(r.compute){compute_us+=us;xbg_logf("NVN GPASS frame=%llu slot=%u ring=%u i=%zu kind=compute us=%.0f cs=%s groups=%u,%u,%u",(unsigned long long)s.prof_frame,s.prof_slot,unsigned(&s-slots),i,us,r.first.c_str(),r.x,r.y,r.z);}
            else{pass_us+=us;xbg_logf("NVN GPASS frame=%llu slot=%u ring=%u i=%zu kind=pass us=%.0f %ux%u att=%u fmt=%u draws=%u verts=%llu first=%s last=%s mixed=%u dz=%u",(unsigned long long)s.prof_frame,s.prof_slot,unsigned(&s-slots),i,us,r.w,r.h,r.att,r.fmt,r.draws1-r.draws0,(unsigned long long)(r.verts1-r.verts0),r.first.empty()?"builtin-clear/no-game-draw":r.first.c_str(),r.last.empty()?"builtin-clear/no-game-draw":r.last.c_str(),unsigned(r.mixed),unsigned(r.dz));}
        }
        if(complete)span_us=double((last-first)&mask)*per;
        xbg_logf("NVN GPASS frame=%llu slot=%u ring=%u summary recorded=%zu complete=%zu skipped=%u queries=%u result=%d pass_us=%.0f compute_us=%.0f span_us=%.0f",(unsigned long long)s.prof_frame,s.prof_slot,unsigned(&s-slots),s.prof_recs.size(),complete,s.prof_skipped,s.prof_used,int(status),pass_us,compute_us,span_us);
    }
    if(s.timestamps){uint64_t t[2]{};if(vkGetQueryPoolResults(cfg.device,s.timestamps,0,2,sizeof(t),t,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS && t[1]>=t[0]){uint64_t mask=timestamp_bits<64?(uint64_t(1)<<timestamp_bits)-1:~0ull;const uint64_t ns=uint64_t(double((t[1]-t[0])&mask)*properties.limits.timestampPeriod);perf.gpu_ns+=ns;++perf.gpu_samples;hud::gpu_add(s.hud_frame,ns);}}
    CPU_SCOPE(Cleanup);uint64_t c0=timing_clock();
    // One readback per occlusion pool (the fence has signalled, so every used query is available).
    {static std::vector<uint64_t> results;size_t used=s.occlusion_used;results.assign(used,0);
     if(s.query_copied==used && used){s.query_results.invalidate();memcpy(results.data(),s.query_results.mapped,used*8);}
     else for(size_t i=0;i*256<used;i++){uint32_t n=uint32_t(std::min<size_t>(256,used-i*256));if(vkGetQueryPoolResults(cfg.device,s.occlusion_pools[i],0,n,n*sizeof(uint64_t),results.data()+i*256,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS)for(uint32_t k=0;k<n;k++)results[i*256+k]=0;}
     s.query_copied=0;
     for(size_t i=0;i<s.query_refs.size();i++){auto e=epochs.find(s.query_refs[i].first);if(e==epochs.end())continue;uint64_t a=s.query_refs[i].second;
         if(a>=e->second.base && a<e->second.size()){auto &q=e->second.q[a-e->second.base];q.value=i<used?results[i]:0;q.known=true;}}
     s.query_refs.clear();for(auto &e:epochs)e.second.advance_known();}
    for(auto &r:pending_reports)if(r.slot==&s && !r.ts_known){uint64_t v=0;if(vkGetQueryPoolResults(cfg.device,r.ts,r.ts_index,1,sizeof(v),&v,sizeof(v),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS)r.ts_value=v;r.ts_known=true;}
    s.occlusion_used=0;s.timestamp_used=0;uint64_t c1=timing_clock();
    if(!resolving_pending){resolve_slot_writebacks(&s);resolve_known_reports();
        bool waiting=false;for(auto &r:pending_reports)if(r.slot==&s)waiting=true;if(!waiting)publish_completed(std::max(completed.load(),s.covers));}
    uint64_t c2=timing_clock();while(s.waiters.n.load())std::this_thread::yield();VK(vkResetFences(cfg.device,1,&s.fence));   // 栅栏已发信号，等待者马上返回 | the fence has signalled, so waiters return at once
    for(auto &f:s.framebuffers)vkDestroyFramebuffer(cfg.device,f.second,nullptr);s.framebuffers.clear();
    for(auto &v:s.texel_views)vkDestroyBufferView(cfg.device,v.second,nullptr);s.texel_views.clear();
    uint64_t c3=timing_clock();s.retire.run();release_descriptors(s);uint64_t c4=timing_clock();
    perf.arena_peak=std::max<uint64_t>(perf.arena_peak,s.stream.peak+s.readback.peak);s.stream.peak=s.readback.peak=0;
    s.stream.reset();s.readback.reset();
    // Every set is fully rewritten before use, so sets are recycled instead of resetting pools (vkAllocateDescriptorSets cost ~15 us/set on Maleoon).
    for(auto &u:s.used_sets){auto &sp=s.spare_sets[u.first];sp.insert(sp.end(),u.second.begin(),u.second.end());u.second.clear();}s.set_cache.clear();
    cleanup_ns[0]+=c1-c0;cleanup_ns[1]+=c2-c1;cleanup_ns[2]+=c3-c2;cleanup_ns[3]+=c4-c3;cleanup_ns[4]+=timing_clock()-c4;
    s.pending=false;s.xview=false;
    if(!resolving_pending)apply_ready_events();   // after this slot's writebacks; inside resolve_pending its own copy runs first
}
Slot &recording_slot(){
    Slot &s=slots[slot_index];
    if(!s.recording){
        {SiteScope site(SITE_RING,true);retire_slot(s);}
        // From here on this slot reuses its readback arena while the other slot, still in flight, may read it through pending
        // views (xview): its GPU reads then race the CPU copies of this submission.
        {const Slot &o=slots[slot_index^1];if(o.pending && o.xview)++xview_races;
         s.guard.clear();if(o.pending && xview_guard && !ab_off("xguard"))s.guard.swap(s.viewed);s.viewed.clear();s.pro_ops.clear();s.pro_images.clear();}
        // Pending writebacks still read this slot's readback arena; finish them before trimming.
        for(auto &w:pending_writebacks)if(w.slot==&s){++perf.why_slot;resolve_pending();break;}
        s.stream.trim();s.readback.keep_chunks=s.guard.empty()?1:s.readback.chunks.size();s.readback.trim();
        if(texel_memo_enabled && (slot_serial&15)<2)texel_evict(slot_index,false);   // this slot's copies: its previous submission retired
        if(!s.cmd){if(slot_pools_enabled){VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=cfg.queue_family;VK(vkCreateCommandPool(cfg.device,&cp,nullptr,&s.pool));}
            VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=s.pool?s.pool:command_pool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;VK(vkAllocateCommandBuffers(cfg.device,&a,&s.cmd));}
        s.readback.cached=readback_cached;
        if(!s.fence){VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK(vkCreateFence(cfg.device,&f,nullptr,&s.fence));}
        if(!s.timestamps && timestamp_bits){VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=2;VK(vkCreateQueryPool(cfg.device,&q,nullptr,&s.timestamps));}
        bind_cache_reset();if(s.pool){if(s.reset_queued){poolreset::finish(s);s.reset_queued=false;VK(s.reset_result);}else VK(vkResetCommandPool(cfg.device,s.pool,0));}else{VK(vkResetCommandBuffer(s.cmd,0));if(s.pro)VK(vkResetCommandBuffer(s.pro,0));}VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VK(vkBeginCommandBuffer(s.cmd,&b));
        if(s.timestamps){vkCmdResetQueryPool(s.cmd,s.timestamps,0,2);vkCmdWriteTimestamp(s.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,s.timestamps,0);}
        s.recording=true;s.ops=0;
        const uint64_t frame=stats.presents.load()+1;
        if(pass_prof_enabled && timestamp_bits){
            if(stats.presents.load()>=pass_prof_next && stats.presents.load()<pass_prof_until && pass_prof_frame!=frame){pass_prof_frame=frame;pass_prof_next=stats.presents.load()+pass_prof_period;pass_prof_queries=pass_prof_slots=pass_prof_skipped_slots=0;}
            if(frame==pass_prof_frame){
                if(pass_prof_slots<pass_prof_slot_limit){
                    if(!s.prof_pool){VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=pass_prof_query_limit;VK(vkCreateQueryPool(cfg.device,&q,nullptr,&s.prof_pool));}
                    vkCmdResetQueryPool(s.cmd,s.prof_pool,0,pass_prof_query_limit);s.prof=true;s.prof_used=0;s.prof_recs.clear();s.prof_open=-1;s.prof_skipped=0;s.prof_frame=frame;s.prof_slot=pass_prof_slots++;
                }else ++pass_prof_skipped_slots;
            }
        }
        // Order against work of earlier submissions on this queue.
        {VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;vkCmdPipelineBarrier(s.cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);barrier_seq=write_seq;}
        for(auto q:s.occlusion_pools)vkCmdResetQueryPool(s.cmd,q,0,256);
        for(auto q:s.timestamp_pools)vkCmdResetQueryPool(s.cmd,q,0,64);
    }
    return s;
}
// Submit the recording slot. With wait, block until all submitted work is done.
void submit_slots(bool wait){
    Slot &s=slots[slot_index];
    if(s.recording){
        if(open_pass.cmd==s.cmd){unsigned saved=cur_op;cur_op=60;end_open_pass();cur_op=saved;}barrier_seq=0;
        // Occlusion results: GPU copies them into mapped memory (vkGetQueryPoolResults is slow on this driver).
        s.query_copied=0;if(s.occlusion_used){size_t used=s.occlusion_used;s.query_results=s.readback.alloc(used*8);
            for(size_t i=0;i*256<used;i++){uint32_t n=uint32_t(std::min<size_t>(256,used-i*256));vkCmdCopyQueryPoolResults(s.cmd,s.occlusion_pools[i],0,n,s.query_results.buffer,s.query_results.offset+i*256*8,8,VK_QUERY_RESULT_64_BIT);}
            VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};hb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;hb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(s.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&hb,0,nullptr,0,nullptr);
            s.query_copied=used;}
        if(s.timestamps)vkCmdWriteTimestamp(s.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.timestamps,1);
        VK(vkEndCommandBuffer(s.cmd));
        VkCommandBuffer cbs[2]{s.cmd,VK_NULL_HANDLE};uint32_t ncb=1;
        if(!s.pro_ops.empty() || !s.pro_images.empty()){
            if(!s.pro){VkCommandBufferAllocateInfo pa{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};pa.commandPool=s.pool?s.pool:command_pool;pa.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;pa.commandBufferCount=1;VK(vkAllocateCommandBuffers(cfg.device,&pa,&s.pro));}
            VkCommandBufferBeginInfo pb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};pb.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VK(vkBeginCommandBuffer(s.pro,&pb));
            // Earlier submissions first (the other slot still reads these ranges), then the copies, then this submission.
            VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};before.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;before.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(s.pro,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
            for(const auto &op:s.pro_ops){VkBufferCopy r{op.src_off,op.dst_off,op.size};vkCmdCopyBuffer(s.pro,op.src,op.dst,1,&r);}
            // gfx-upload-prologue：所有图像一道屏障转成传输目标、各自拷贝、再一道屏障回到 GENERAL。
            // gfx-upload-prologue: one barrier takes every image to a transfer destination, the copies, then one barrier back to GENERAL.
            if(!s.pro_images.empty()){
                std::vector<VkImageMemoryBarrier> ib(s.pro_images.size(),VkImageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER});
                for(size_t i=0;i<ib.size();i++){const auto &u=s.pro_images[i];auto &b=ib[i];b.oldLayout=u.from;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.image=u.image;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
                    b.subresourceRange={u.aspects,0,u.levels,0,u.layers};b.srcAccessMask=u.from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;}
                vkCmdPipelineBarrier(s.pro,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,uint32_t(ib.size()),ib.data());
                for(const auto &u:s.pro_images)vkCmdCopyBufferToImage(s.pro,u.buffer,u.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(u.copies.size()),u.copies.data());
                for(auto &b:ib){b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;}
                vkCmdPipelineBarrier(s.pro,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,uint32_t(ib.size()),ib.data());
                s.pro_images.clear();}
            VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER};after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(s.pro,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&after,0,nullptr,0,nullptr);
            VK(vkEndCommandBuffer(s.pro));cbs[0]=s.pro;cbs[1]=s.cmd;ncb=2;s.pro_ops.clear();}
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=ncb;si.pCommandBuffers=cbs;
        {CPU_SCOPE(Submit);VK(vkQueueSubmit(cfg.queue,1,&si,s.fence));}++submit_by[sync_site];poolreset::queue(s);
        s.recording=false;s.pending=true;s.covers=submitted;s.hud_frame=hud::frame.load(std::memory_order_relaxed);slot_index^=1;++perf.submits;snapshot_reset();++slot_serial;
        pending_events.insert(pending_events.end(),recording_events.begin(),recording_events.end());recording_events.clear();
    }
    if(wait){retire_slot(slots[slot_index]);retire_slot(slots[slot_index^1]);}
}
// Pipelines persist across launches (the driver validates the header and
// ignores incompatible data), so later launches skip shader compilation.
size_t pipelines_unsaved=0;uint64_t pipeline_save_frame=0;constexpr uint64_t compile_ns=2000000;
std::string pipeline_cache_path(){std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');return (cut==std::string::npos?dir:dir.substr(0,cut))+"/vk-pipeline-cache.bin";}
std::vector<uint8_t> load_pipeline_cache(){
    std::vector<uint8_t> data;FILE *f=fopen(pipeline_cache_path().c_str(),"rb");if(!f)return data;
    fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    if(n>32 && n<(256l<<20)){data.resize(size_t(n));if(fread(data.data(),1,data.size(),f)!=data.size())data.clear();}
    fclose(f);
    if(data.size()>=32){uint32_t header[4];memcpy(header,data.data(),16);if(header[1]!=1 || header[2]!=properties.vendorID || header[3]!=properties.deviceID || memcmp(data.data()+16,properties.pipelineCacheUUID,16))data.clear();}
    xbg_logf("NVN pipeline cache load bytes=%zu",data.size());return data;
}
// The cache is written by its own thread: a save serializes the whole cache (17.7 MB on the Redmi K50 Ultra) and writes
// it, which ran on the graphics thread every 300 frames while pipelines were new. Marker gfx-sync-cache-save = '1' saves on
// the graphics thread as before. Only real compiles count as unsaved: a creation under 2 ms came from the driver's cache,
// and with a warm cache each start counted its ~200 warm-up builds as new and rewrote the cache about six times.
bool sync_cache_save=false;std::thread cache_saver;std::atomic<bool> cache_save_busy{false};
void write_pipeline_cache(size_t unsaved){
    uint64_t t0=timing_clock();size_t n=0;bool ok=false;
    if(vkGetPipelineCacheData(cfg.device,pipeline_cache,&n,nullptr)==VK_SUCCESS && n){std::vector<uint8_t> data(n);
        if(vkGetPipelineCacheData(cfg.device,pipeline_cache,&n,data.data())==VK_SUCCESS){std::string path=pipeline_cache_path(),tmp=path+".tmp";
            if(FILE *f=fopen(tmp.c_str(),"wb")){ok=fwrite(data.data(),1,n,f)==n;ok=fclose(f)==0&&ok;if(ok)ok=rename(tmp.c_str(),path.c_str())==0;}}}
    xbg_logf("NVN pipeline cache save bytes=%zu new=%zu ok=%d ms=%.1f on=%s",n,unsaved,int(ok),double(timing_clock()-t0)/1e6,sync_cache_save?"graphics":"saver");
}
void cache_save_join(){if(cache_saver.joinable())cache_saver.join();}
void save_pipeline_cache(){
    if(!pipeline_cache || !pipelines_unsaved || cache_save_busy.load())return;
    warm::flush();const size_t unsaved=pipelines_unsaved;pipelines_unsaved=0;
    if(sync_cache_save){write_pipeline_cache(unsaved);return;}
    cache_save_join();cache_save_busy=true;
    cache_saver=std::thread([unsaved]{setpriority(PRIO_PROCESS,0,10);write_pipeline_cache(unsaved);cache_save_busy=false;});
}
extern bool tex_dirty;extern uint64_t tex_dirty_tracked,tex_dirty_checks,tex_dirty_changed;   // texture checks through the dirty tracker (get_image)
// 3D 画面加亮的读数（定义在 dispatch 前的 namespace bright）
// Readings of the 3D picture brightening (defined in namespace bright before dispatch)
namespace bright{extern bool enabled;extern uint64_t applied;extern float last_k;}
void perf_frame(){
    {texel_distinct+=texel_frame_keys.size();for(auto k:texel_frame_keys)if(texel_prev_keys.count(k))++texel_reused;texel_prev_keys.swap(texel_frame_keys);texel_frame_keys.clear();}
    if(completion_wait_enabled)fold_completion_wait_stats();
    if(perf_frame_timed)++perf_timed_frames;perf_frame_timed=perf_sample && stats.presents.load()%perf_sample==0;   // the next frame
    {bool was=ab_frame_on;ab_frame_on=ab_enabled();tls_gate=!ab_off("tlsgate");img_fast=!ab_off("imgfast");if(was!=ab_frame_on && ab_period>0){perf=Perf{};perf.window_start=timing_clock();set_cache_hits=set_cache_misses=0;return;}}
    pipelines_unsaved+=size_t(warm::compiled.exchange(0));
    if(pipelines_unsaved && stats.presents.load()>=pipeline_save_frame+300){pipeline_save_frame=stats.presents.load();save_pipeline_cache();}
    uint64_t now=timing_clock();if(!perf.window_start){perf.window_start=now;perf.frames=0;return;}
    if(++perf.frames<60)return;
    double wall=double(now-perf.window_start)/1e6,f=double(perf.frames);
    if(poolreset::enabled){xbg_logf("NVN PERF poolreset queued=%.2f waits=%.2f wait_ms=%.3f",poolreset::queued/f,poolreset::waits/f,poolreset::wait_ns/1e6/f);poolreset::queued=poolreset::waits=poolreset::wait_ns=0;}
    const double tf=perf_timed_frames?double(perf_timed_frames):1.0;   // parts and descriptor timers come from the timed frames only
    xbg_logf("NVN PERF v=1 sync=%d frames=%llu wall_ms=%.1f fps=%.2f submit_cpu_ms=%.2f present_cpu_ms=%.2f gpu_ms=%.2f gpu_samples=%llu submits=%.2f waits=%.2f sync_wb=%llu sync_rb=%llu sync_q=%llu ops=%.1f folded=%.1f passes=%.1f draws=%.1f dispatches=%.1f why=w%llu/p%llu/a%llu(%llu,%llu,%llu,%llu,%llu,%llu|%llu,%llu,%llu,%llu)/c%llu/f%llu/s%llu views=%llu snap_hit=%llu snap_miss=%llu snap_mb=%.1f arena_peak_mb=%.1f pipelines=%zu layouts=%zu samplers=%zu passes=%zu images=%zu host_segments=%zu host_mb=%.1f",
        int(sync_mode),(unsigned long long)perf.frames,wall,f*1000.0/wall,double(perf.submit_cpu_ns)/1e6/f,double(perf.present_cpu_ns)/1e6/f,perf.gpu_samples?double(perf.gpu_ns)/1e6/f:-1.0,(unsigned long long)perf.gpu_samples,
        double(perf.submits)/f,double(perf.waits)/f,(unsigned long long)perf.sync_writeback,(unsigned long long)perf.sync_readback,(unsigned long long)perf.sync_query,double(perf.ops)/f,double(perf.folded_clears)/f,double(perf.passes)/f,double(perf.draws)/f,double(perf.dispatches)/f,
        (unsigned long long)perf.why_wait,(unsigned long long)perf.why_poll,(unsigned long long)perf.why_access,(unsigned long long)perf.access_by[1],(unsigned long long)perf.access_by[2],(unsigned long long)perf.access_by[3],(unsigned long long)perf.access_by[4],(unsigned long long)perf.access_by[5],(unsigned long long)perf.access_by[6],(unsigned long long)perf.access_by[8],(unsigned long long)perf.access_by[9],(unsigned long long)perf.access_by[10],(unsigned long long)perf.access_by[11],(unsigned long long)perf.why_cmdwait,(unsigned long long)perf.why_finish,(unsigned long long)perf.why_slot,(unsigned long long)perf.pending_views,(unsigned long long)perf.snapshot_hits,(unsigned long long)perf.snapshot_misses,double(perf.snapshot_bytes)/1048576.0,double(perf.arena_peak)/1048576.0,perf_counts().pipelines,perf_counts().layouts,perf_counts().samplers,perf_counts().passes,perf_counts().images,perf_counts().host_segments,double(perf_counts().host_bytes)/1048576.0);
    if(!timing_active){
        static const char *names[]={"other","draw","dispatch","descriptor","pass","buffer","pipeline","submit","fence","present","cleanup","hash","image","log","program","cpuwrite","snapshot"};
        char line[1024];int at=snprintf(line,sizeof(line),"NVN PERF parts_ms_per_frame timed=%llu",(unsigned long long)perf_timed_frames);
        for(unsigned i=0;i<XbgCpuTiming::Count && at<int(sizeof(line))-48;i++){const auto &c=timing.counters[i];if(c.calls<perf_timing_before[i].calls)perf_timing_before[i]=XbgCpuTiming::Counter{};uint64_t ns=c.exclusive_ns-perf_timing_before[i].exclusive_ns,calls=c.calls-perf_timing_before[i].calls;at+=snprintf(line+at,sizeof(line)-at," %s=%.2f/%.1f",names[i],double(ns)/1e6/tf,double(calls)/tf);perf_timing_before[i]=c;}
        xbg_logf("%s",line);
    }
    xbg_logf("NVN PERF storage_snapshots_per_frame=%.1f union_mb_per_frame=%.2f",storage_snap_count/f,storage_pages.size()*4096.0/1048576.0/tf);storage_pages.clear();storage_snap_count=0;
    xbg_logf("NVN PERF wtrack copied_kb_per_frame=%.1f dirty_pages_per_frame=%.1f mismatches=%llu fallbacks=%llu",wtrack::copied_bytes/1024.0/f,wtrack::dirty_pages/f,(unsigned long long)wtrack::mismatches,(unsigned long long)wtrack::fallbacks);wtrack::copied_bytes=wtrack::dirty_pages=0;
    xbg_logf("NVN PERF uniform_trimmed_bindings=%llu snap_mb_per_frame other=%.2f vertex=%.2f index=%.2f storage=%.2f texel=%.2f uniform=%.2f requested vertex=%.2f index=%.2f storage=%.2f",(unsigned long long)uniform_trimmed,snap_site_bytes[0]/1048576.0/f,snap_site_bytes[1]/1048576.0/f,snap_site_bytes[2]/1048576.0/f,snap_site_bytes[3]/1048576.0/f,snap_site_bytes[4]/1048576.0/f,snap_site_bytes[5]/1048576.0/f,snap_site_req[1]/1048576.0/f,snap_site_req[2]/1048576.0/f,snap_site_req[3]/1048576.0/f);for(auto &b:snap_site_bytes)b=0;for(auto &b:snap_site_req)b=0;
    xbg_logf("NVN PERF quad_rewrites=%llu warp_rewrites=%llu ldc_folds=%llu temps_forwarded=%llu nan_minmax=%llu",(unsigned long long)quad_rewrites.load(std::memory_order_relaxed),(unsigned long long)warp_rewrites.load(std::memory_order_relaxed),(unsigned long long)ldc_folds.load(std::memory_order_relaxed),(unsigned long long)temps_forwarded.load(std::memory_order_relaxed),(unsigned long long)nan_minmax_swaps.load(std::memory_order_relaxed));
    xbg_logf("NVN PERF snapfront front_hits=%.1f map_hits=%.1f invalidations=%.1f erased=%.1f",snap_front_hits/f,snap_map_hits/f,snap_invalidations/f,snap_invalidated/f);xbg_logf("NVN PERF textures bc_native=%llu bc_decoded=%llu live_images=%.1fMB bc_images=%.1fMB bc_payload=%.1fMB",(unsigned long long)native_bc_images,(unsigned long long)decoded_bc_images,image_bytes_live/1048576.0,bc_image_bytes_live/1048576.0,bc_data_bytes_live/1048576.0);snap_front_hits=snap_map_hits=snap_invalidations=snap_invalidated=0;
    if(bright::enabled){xbg_logf("NVN PERF bright applied=%.2f k=%.2f",bright::applied/f,bright::last_k);bright::applied=0;}
    if(tex_dirty){xbg_logf("NVN PERF texdirty tracked=%.1f checks=%.1f changed=%.1f",tex_dirty_tracked/f,tex_dirty_checks/f,tex_dirty_changed/f);tex_dirty_tracked=tex_dirty_checks=tex_dirty_changed=0;}
    if(vcache::enabled){xbg_logf("NVN PERF vcache hits=%.1f (%.2fMB) fills=%.1f (%.2fMB) refills=%.2f volatile=%llu untracked=%llu flushes=%llu entries=%zu arena=%.1fMB pools=%d",vcache::hits/f,vcache::hit_bytes/1048576.0/f,vcache::fills/f,vcache::fill_bytes/1048576.0/f,vcache::refills/f,(unsigned long long)vcache::volatiles,(unsigned long long)vcache::untracked,(unsigned long long)vcache::flushes,vcache::live(),vcache::arena_bytes()/1048576.0,[]{int live=0,used=0;uint64_t adds=0;if(nvn_dirty_slot_stats)nvn_dirty_slot_stats(&live,&used,&adds);return used;}());vcache::hits=vcache::hit_bytes=vcache::fills=vcache::fill_bytes=vcache::refills=0;}
    {static uint64_t pm=0,pe=0;xbg_logf("NVN PERF pipelines_new=%llu evicted=%llu warm_hits=%llu created=%llu create_ms=%.1f create_max_ms=%.1f warm_built=%llu warm_failed=%llu warm_ms=%.0f async_skips=%llu async_built=%llu",(unsigned long long)(pipeline_counts.misses-pm),(unsigned long long)(pipeline_counts.evictions-pe),(unsigned long long)warm::hits,(unsigned long long)warm::creates,warm::create_ns/1e6,warm::create_max_ns/1e6,(unsigned long long)warm::built.load(),(unsigned long long)warm::failed.load(),warm::build_ns.load()/1e6,(unsigned long long)warm::async_skips.exchange(0),(unsigned long long)warm::async_built.load());warm::hits=warm::creates=warm::create_ns=warm::create_max_ns=0;pm=pipeline_counts.misses;pe=pipeline_counts.evictions;}
    xbg_logf("NVN PERF setcache ab=%d hits=%.1f misses=%.1f template=%.1f plain=%.1f memo_hits=%.1f memo_misses=%.1f memo_front=%.1f why=texel%.1f/simg%.1f/dm%.1f/dynbuf%.1f/store%.1f",int(ab_frame_on),set_cache_hits/f,set_cache_misses/f,template_updates/f,plain_updates/f,desc_memo_hits/f,desc_memo_misses/f,memo_front_hits/f,desc_memo_why[0]/f,desc_memo_why[1]/f,desc_memo_why[2]/f,desc_memo_why[3]/f,desc_memo_why[4]/f);desc_memo_hits=desc_memo_misses=memo_front_hits=0;for(auto &w:desc_memo_why)w=0;xbg_logf("NVN PERF keydiff shader%.0f pool%.0f ubo%.0f uaddr%.0f size%.0f vp%.0f alpha%.0f drv%.0f tex%.0f samp%.0f saddr%.0f same%.0f new%.0f len%.0f",key_diff[0]/tf,key_diff[1]/tf,key_diff[2]/tf,key_diff[3]/tf,key_diff[4]/tf,key_diff[5]/tf,key_diff[6]/tf,key_diff[7]/tf,key_diff[8]/tf,key_diff[9]/tf,key_diff[10]/tf,key_diff[11]/tf,key_diff[12]/tf,key_diff[13]/tf);for(auto &k:key_diff)k=0;   // per timed frame
    xbg_logf("NVN PERF binds_skipped_per_frame=%.1f xmemo_hits=%.1f drops=%llu bumps=%llu/%llu/%llu/%llu/%llu pinned=%zu/%zu",binds_skipped/f,xmemo_hits/f,(unsigned long long)xmemo_drops,(unsigned long long)memo_bump[0],(unsigned long long)memo_bump[1],(unsigned long long)memo_bump[2],(unsigned long long)memo_bump[3],(unsigned long long)memo_bump[4],memo_pinned[0].size(),memo_pinned[1].size());binds_skipped=0;{char top[160]="";int at=0;bool used[32]={};
     for(int r=0;r<4;r++){int best=-1;for(int k=0;k<32;k++)if(!used[k] && offset_changed_at[k] && (best<0 || offset_changed_at[k]>offset_changed_at[best]))best=k;if(best<0)break;used[best]=true;at+=snprintf(top+at,sizeof(top)-size_t(at),"%s%d:%.1f",r?",":"",best,offset_changed_at[best]/f);}
     xbg_logf("NVN PERF binds set_binds=%.1f offsets_per_bind=%.1f offsets_only=%.1f changed n1=%.1f n2=%.1f n3=%.1f n4+=%.1f at=%s",set_binds/f,set_binds?double(set_bind_offsets)/double(set_binds):0.0,set_binds_offsets_only/f,offset_changes[1]/f,offset_changes[2]/f,offset_changes[3]/f,(offset_changes[4]+offset_changes[5]+offset_changes[6]+offset_changes[7]+offset_changes[8]+offset_changes[9])/f,top);}
    set_binds=set_bind_offsets=set_binds_offsets_only=0;for(auto &x:offset_changes)x=0;for(auto &x:offset_changed_at)x=0;xmemo_hits=0;xbg_logf("NVN PERF noload_passes_per_frame=%.2f img_cache_hits=%.1f misses=%.1f why=%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f/%.1f",noload_passes/f,img_cache_hits/f,img_cache_misses/f,noload_why[0]/f,noload_why[1]/f,noload_why[2]/f,noload_why[3]/f,noload_why[4]/f,noload_why[5]/f,noload_why[6]/f,noload_why[7]/f);for(auto &w:noload_why)w=0;noload_passes=0;img_cache_hits=img_cache_misses=0;xbg_logf("NVN PERF desc_ns_per_frame memo_key=%.2f tmpl_update=%.2f alloc=%.2f setkey=%.2f build=%.2f hit_total=%.2f",desc_ns[0]/1e6/tf,desc_ns[1]/1e6/tf,desc_ns[2]/1e6/tf,desc_ns[3]/1e6/tf,desc_ns[4]/1e6/tf,desc_ns[5]/1e6/tf);for(auto &x:desc_ns)x=0;template_updates=plain_updates=0;set_cache_hits=set_cache_misses=0;
    xbg_logf("NVN PERF async=%d drains=%.2f drain_ms=%.2f pace_ms=%.2f waits=%.2f",int(gfx_async),gfx_drain_calls.exchange(0)/f,gfx_drain_ns.exchange(0)/1e6/f,gfx_pace_ns.exchange(0)/1e6/f,gfx_wait_calls.exchange(0)/f);xbg_logf("NVN PERF events defer=%d deferred=%.2f cpu_reads_pending=%llu epochs=%zu reports=%zu",int(defer_events),events_deferred/f,(unsigned long long)event_reads,epochs.size(),pending_reports.size());if(evguard::enabled){Dl_info di{};uintptr_t pc=evguard::last_pc.load();const char *mod=pc && dladdr(reinterpret_cast<void*>(pc),&di) && di.dli_fname?di.dli_fname:"?";xbg_logf("NVN PERF evguard guards=%llu exact=%llu other=%llu writes=%llu last_addr=%#llx last_pc=%#llx in %s",(unsigned long long)evguard::guards.load(),(unsigned long long)evguard::exact.load(),(unsigned long long)evguard::other.load(),(unsigned long long)evguard::writes.load(),(unsigned long long)evguard::last_addr.load(),(unsigned long long)pc,mod);
     xbg_logf("NVN PERF evguard engine_opens=%llu recloses=%llu",(unsigned long long)evguard::engine_opens.load(),(unsigned long long)evguard::recloses.load());
     xbg_logf("NVN PERF evguard last word=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx x4=%#llx x5=%#llx",(unsigned long long)evguard::last_word.load(),(unsigned long long)evguard::last_x[0].load(),(unsigned long long)evguard::last_x[1].load(),(unsigned long long)evguard::last_x[2].load(),(unsigned long long)evguard::last_x[3].load(),(unsigned long long)evguard::last_x[4].load(),(unsigned long long)evguard::last_x[5].load());
     for(int i=0;i<8;i++){uintptr_t lr=evguard::callers[i].load();if(!lr)break;Dl_info li{};const char *lm="?",*ls="?";uintptr_t base=0,sym=0;
         if(dladdr(reinterpret_cast<void*>(lr),&li)){if(li.dli_fname)lm=li.dli_fname;if(li.dli_sname)ls=li.dli_sname;base=uintptr_t(li.dli_fbase);sym=uintptr_t(li.dli_saddr);}
         xbg_logf("NVN PERF evguard caller lr=%#llx %s+%#llx (%s+%#llx) hits=%llu",(unsigned long long)lr,lm,(unsigned long long)(lr-base),ls,(unsigned long long)(sym?lr-sym:0),(unsigned long long)evguard::caller_hits[i].load());}}events_deferred=0;
    {static const char *site_names[SITE_COUNT]={"other","ring","present","event","arena","gpu_sync","complete","poll","legacy_wait"};
     char line[768];int at=snprintf(line,sizeof(line),"NVN PERF block_ms");
     for(int i=0;i<SITE_COUNT && at<int(sizeof(line))-40;i++)if(block_calls[i])at+=snprintf(line+at,sizeof(line)-at," %s=%.2f/%.2f",site_names[i],block_ns[i]/1e6/f,block_calls[i]/f);
     at+=snprintf(line+at,sizeof(line)-at," legacy_wait_total=%.2f submit_by",legacy_wait_ns/1e6/f);
     for(int i=0;i<SITE_COUNT && at<int(sizeof(line))-40;i++)if(submit_by[i])at+=snprintf(line+at,sizeof(line)-at," %s=%.2f",site_names[i],submit_by[i]/f);
     xbg_logf("%s",line);for(int i=0;i<SITE_COUNT;i++)block_ns[i]=block_calls[i]=submit_by[i]=0;legacy_wait_ns=0;}
    {char line[1024];int at=snprintf(line,sizeof(line),"NVN PERF pass_ends");
     for(int i=0;i<64 && at<int(sizeof(line))-24;i++)if(pass_end_by[i])at+=snprintf(line+at,sizeof(line)-at," op%d=%.1f",i,pass_end_by[i]/f);
     at+=snprintf(line+at,sizeof(line)-at," fb_hits=%.1f fb_creates=%.2f fb_live=%zu texel_binds=%.1f distinct=%.1f reused=%.1f resident=%zu/%zu hits=%.1f refresh=%.1f fallback=%.2f evict=%.2f occlusion=%.1f reports=%.1f samples=%.0f ops=%.1f/%.1f/%.1f pending=%zu pipe_fast=%.1f pass_fast=%.1f barriers",fb_hits/f,fb_creates/f,persistent_framebuffers.size(),texel_binds/f,texel_distinct/f,texel_reused/f,texel_resident[0].size(),texel_resident[1].size(),texel_hits/f,texel_refreshes/f,texel_fallbacks/f,texel_evictions/f,occlusion_queries_started/f,samples_reports/f,samples_reported/f,counter_ops[0]/f,counter_ops[1]/f,counter_ops[2]/f,pending_reports.size(),pipeline_fast_hits/f,pass_fast_hits/f);pipeline_fast_hits=0;pass_fast_hits=0;fb_hits=fb_creates=0;occlusion_queries_started=samples_reports=samples_reported=0;for(auto &c:counter_ops)c=0;texel_binds=texel_distinct=texel_reused=0;texel_hits=texel_refreshes=texel_fallbacks=texel_evictions=0;
     for(int i=0;i<64 && at<int(sizeof(line))-24;i++)if(barrier_by[i])at+=snprintf(line+at,sizeof(line)-at," op%d=%.1f",i,barrier_by[i]/f);
     if(at<int(sizeof(line))-24)at+=snprintf(line+at,sizeof(line)-at," depth_idle=%.1f",depth_idle_passes/f);
     xbg_logf("%s",line);for(int i=0;i<64;i++)pass_end_by[i]=barrier_by[i]=0;depth_idle_passes=0;}
    xbg_logf("NVN PERF cleanup_ms_per_frame queries=%.2f writebacks=%.2f(%.1fMB) views=%.2f retire=%.2f arena_pools=%.2f",cleanup_ns[0]/1e6/f,cleanup_ns[1]/1e6/f,writeback_bytes/1048576.0/f,cleanup_ns[2]/1e6/f,cleanup_ns[3]/1e6/f,cleanup_ns[4]/1e6/f);for(auto &c:cleanup_ns)c=0;writeback_bytes=0;
    xbg_logf("NVN PERF xview views=%.2f races=%.2f guarded=%.2f guarded_kb=%.1f",xviews/f,xview_races/f,xview_guarded/f,xview_guarded_bytes/1024.0/f);xviews=xview_races=xview_guarded=xview_guarded_bytes=0;
    if(uploads_prologue || uploads_inpass){xbg_logf("NVN PERF uploads window prologue=%llu inpass=%llu broke_pass=%llu",(unsigned long long)uploads_prologue,(unsigned long long)uploads_inpass,(unsigned long long)uploads_broke_pass);uploads_prologue=uploads_inpass=uploads_broke_pass=0;}
    if(shaders_reused || shader_reuse_mismatches){xbg_logf("NVN PERF shader_reuse window reused=%llu mismatches=%llu",(unsigned long long)shaders_reused,(unsigned long long)shader_reuse_mismatches);shaders_reused=shader_reuse_mismatches=0;}
    if(wb_skip){xbg_logf("NVN PERF wbskip ab=%d skipped=%.1f (%.2fMB)",int(ab_frame_on),wb_skipped/f,wb_skipped_bytes/1048576.0/f);wb_skipped=wb_skipped_bytes=0;}
    if(pipe_memo){xbg_logf("NVN PERF pipememo ab=%d hits=%.1f misses=%.1f",int(ab_frame_on),pipe_memo_hits/f,pipe_memo_misses/f);pipe_memo_hits=pipe_memo_misses=0;}
    {double gm=perf.gpu_samples?double(perf.gpu_ns)/1e6/f:-1.0,cm=double(perf.submit_cpu_ns)/1e6/f;perf_gpu_ms.store(gm);perf_cpu_ms.store(cm);}
    perf=Perf{};perf.window_start=now;perf_timed_frames=0;
}
void occlusion_query(VkQueryPool &pool,uint32_t &index){
    Slot &s=recording_slot();index=s.occlusion_used%256;
    if(s.occlusion_used/256==s.occlusion_pools.size()){VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_OCCLUSION;q.queryCount=256;VkQueryPool p;VK(vkCreateQueryPool(cfg.device,&q,nullptr,&p));s.occlusion_pools.push_back(p);end_open_pass();vkCmdResetQueryPool(s.cmd,p,0,256);}
    pool=s.occlusion_pools[s.occlusion_used/256];{auto &e=epochs[counter_epoch];e.q.push_back({&s,pool,index,0,false});s.query_refs.push_back({counter_epoch,e.size()-1});}++s.occlusion_used;
}
void timestamp_query(VkQueryPool &pool,uint32_t &index,const void *&owner){
    Slot &s=recording_slot();index=s.timestamp_used%64;owner=&s;
    if(s.timestamp_used/64==s.timestamp_pools.size()){VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=64;VkQueryPool p;VK(vkCreateQueryPool(cfg.device,&q,nullptr,&p));s.timestamp_pools.push_back(p);end_open_pass();vkCmdResetQueryPool(s.cmd,p,0,64);}
    pool=s.timestamp_pools[s.timestamp_used/64];++s.timestamp_used;
}
// Synchronous point: everything recorded so far has completed on the GPU.
void gpu_sync(uint64_t &reason){++reason;SiteScope site(SITE_GPU_SYNC);submit_slots(true);}
void retire_handle(std::function<void()> f){recording_slot().retire.handles.push_back(std::move(f));}
template<class T>void retire_object(std::shared_ptr<T> p){recording_slot().retire.objects.push_back(std::move(p));}
Buffer stream_buffer(size_t n,const void *data=nullptr,size_t copy=0,bool zero=true){
    Buffer b=recording_slot().stream.alloc(n);
    if(data && copy){need(copy<=n,"Buffer copy overflow");evguard::EngineRead er(data,copy);memcpy(b.mapped,data,copy);}
    if(zero && copy<n)memset(static_cast<uint8_t*>(b.mapped)+copy,0,n-copy);
    b.flush();return b;
}
Buffer readback_buffer(size_t n,const void *data=nullptr,size_t copy=0){
    Slot &s=recording_slot();Buffer b=s.readback.alloc(n);
    for(const auto &g:s.guard)if(g.buffer==b.buffer && b.offset<g.end && g.begin<b.offset+n){
        if(data && copy)need(copy<=n,"Buffer copy overflow");
        Buffer src=stream_buffer(n,data,data?copy:0,true);s.pro_ops.push_back({src.buffer,b.buffer,src.offset,b.offset,VkDeviceSize(n)});
        ++xview_guarded;xview_guarded_bytes+=n;return b;}
    if(data && copy){need(copy<=n,"Buffer copy overflow");memcpy(b.mapped,data,copy);}else copy=0;
    if(copy<n)memset(static_cast<uint8_t*>(b.mapped)+copy,0,n-copy);   // only the bytes the copy leaves
    b.flush();return b;
}
// Per-submission snapshot cache of guest memory ranges. Guest command streams
// cannot change memory between draws except through in-stream CPU operations
// (uniform updates, buffer copies, events, writebacks), which invalidate it.
// Bump arena for the per-submission snapshot maps' nodes. The maps are dropped wholesale every submission: freeing ~3k nodes
// one by one with new/delete (scudo's locking) was ~3% of the render thread, and walking the trees to recycle them still
// ~1% of the D2R camp's on the Redmi K50 Ultra (__tree destroy under snapshot_reset). Nodes are never freed singly now:
// snapshot_reset builds both maps empty in place (keys and Buffer are trivially destructible, and the allocator frees
// nothing) and rewinds the arena. The maps are never destroyed, so no destructor at exit walks the rewound nodes.
// Graphics thread only (engine mutex).
struct SnapArena{
    static constexpr size_t chunk_bytes=256*1024;std::vector<uint8_t*> chunks;size_t chunk=0,used=0;
    void *alloc(size_t n){n=(n+15)&~size_t(15);need(n<=chunk_bytes,"Snapshot map node too large");
        if(chunks.empty() || used+n>chunk_bytes){if(!chunks.empty())++chunk;used=0;if(chunk==chunks.size())chunks.push_back(static_cast<uint8_t*>(::operator new(chunk_bytes)));}
        void *p=chunks[chunk]+used;used+=n;return p;}
    void rewind(){chunk=0;used=0;}
};
SnapArena &snap_arena(){static SnapArena *a=new SnapArena;return *a;}
template<class T> struct SnapAlloc {
    using value_type=T;SnapAlloc()=default;template<class U> SnapAlloc(const SnapAlloc<U>&){}
    T *allocate(size_t n){return static_cast<T*>(snap_arena().alloc(n*sizeof(T)));}
    void deallocate(T*,size_t){}   // snapshot_reset rewinds the arena
    template<class U> bool operator==(const SnapAlloc<U>&)const{return true;}template<class U> bool operator!=(const SnapAlloc<U>&)const{return false;}
};
using SnapMap=std::map<std::pair<uint64_t,uint64_t>,Buffer,std::less<std::pair<uint64_t,uint64_t>>,SnapAlloc<std::pair<const std::pair<uint64_t,uint64_t>,Buffer>>>;
static_assert(std::is_trivially_destructible<Buffer>::value,"snapshot_reset skips the map nodes' destructors");
SnapMap &snapshots=*new SnapMap(),&uniform_snapshots=*new SnapMap();
void snapshot_maps_reset(){new(&snapshots) SnapMap();new(&uniform_snapshots) SnapMap();snap_arena().rewind();}
uint64_t snapshot_maxlen=0;
void desc_memo_reset();
// Direct-mapped front for the two snapshot maps: most lookups repeat within a submission (~12k of 15k per D2R camp frame).
// Entries carry snapshot_epoch, which every reset bumps, so a stale entry never matches. Every front entry mirrors a map
// entry of the same key, so an invalidation clears just the front slots of the map entries it erases; bumping the epoch
// there (A/B target snapinval) emptied the whole front on every in-stream CPU write, overlapping or not.
struct SnapFront{uint64_t address=0,bytes=0,epoch=0;Buffer buffer;};SnapFront snap_front[2][2048];uint64_t snapshot_epoch=1;
uint64_t snap_front_hits=0,snap_map_hits=0,snap_invalidations=0,snap_invalidated=0;   // perf (NVN PERF snapfront)
inline SnapFront &snap_front_for(int which,uint64_t a,uint64_t n){uint64_t x=a*0x9E3779B97F4A7C15ull^n*0xC2B2AE3D27D4EB4Full;return snap_front[which][(x^(x>>31))&2047];}
void snapshot_reset(){snapshot_maps_reset();snapshot_maxlen=0;desc_memo_reset();++texel_write_epoch;++snapshot_epoch;last_pass_reset();}   // the guest may write memory between submissions: texel copies are re-verified
void snapshot_invalidate(uint64_t address,uint64_t bytes){
    ++snap_invalidations;if(ab_off("snapinval"))++snapshot_epoch;
    if(address<texel_hi && address+bytes>texel_lo)texel_touch(address,bytes);
    uint64_t lo=address>snapshot_maxlen?address-snapshot_maxlen:0;int which=0;
    for(auto *m:{&snapshots,&uniform_snapshots}){
        for(auto it=m->lower_bound({lo,0});it!=m->end() && it->first.first<address+bytes;){uint64_t b=it->first.first,n=it->first.second,e=b+n;
            if(address<e && address+bytes>b){SnapFront &f=snap_front_for(which,b,n);if(f.address==b && f.bytes==n)f.epoch=0;++snap_invalidated;it=m->erase(it);}else ++it;}
        ++which;}
}
// GPU-resident view of a guest range whose newest content is a pending
// (not yet written back) GPU result. The producing operation is followed by a
// full barrier, so later reads of the view observe it in queue order.
bool pending_view(uint64_t address,uint64_t bytes,VkDeviceSize align,Buffer &out){
    for(auto w=pending_writebacks.rbegin();w!=pending_writebacks.rend();++w){
        if(!(address<w->addr+w->bytes && address+bytes>w->addr))continue;
        if(address<w->addr || address+bytes>w->addr+w->bytes)return false;
        VkDeviceSize delta=address-w->addr;if(align>1 && (w->buffer.offset+delta)%align)return false;
        out=w->buffer;out.offset+=delta;out.mapped=static_cast<uint8_t*>(out.mapped)+delta;out.size=bytes;++perf.pending_views;
        if(w->slot!=&slots[slot_index]){++xviews;slots[slot_index].xview=true;
            const_cast<Slot*>(static_cast<const Slot*>(w->slot))->viewed.push_back({out.buffer,out.offset,out.offset+bytes});
            static unsigned n=0;if(n++<24)xbg_logf("NVN XVIEW frame=%llu addr=%#llx bytes=%llu writeback=%#llx+%llu",(unsigned long long)stats.presents.load(),(unsigned long long)address,(unsigned long long)bytes,(unsigned long long)w->addr,(unsigned long long)w->bytes);}
        return true;
    }
    return false;
}
// Write-tracked storage snapshots. Large read-only storage bindings (one 19.5 MB buffer per frame in the field,
// of which only ~7 pages change) used to be copied whole every frame. Guest pages are write-protected after a
// copy; a SIGSEGV on a tracked page marks it dirty and re-enables writes, so each frame copies only dirty pages
// into a persistent per-address buffer (two versions; a version is reused only once its frame slot retired).
// Every 240th use is verified against guest memory in full. On by default where the host routes faults here
// (xbg_wtrack_use_host_hooks); files/gfx-no-wtrack or gfx-wtrack = '0' disables it.
namespace wtrack {
constexpr size_t min_bytes=1u<<20;constexpr unsigned max_regions=8,versions=2;bool enabled=true;int marker=-1;   // marker: gfx-wtrack 1 / 0 (or gfx-no-wtrack), -1 = none
struct Region{std::atomic<uintptr_t> begin{0},end{0};std::atomic<uint8_t> *dirty=nullptr;};
Region regions[max_regions];std::atomic<unsigned> nregions{0};struct sigaction previous{};bool installed=false,host_hooks=false;
// Re-enable writes to the tracked pages in [a,a+len) and mark them dirty. Signal-safe: atomics and mprotect only.
bool unprotect(uintptr_t a,size_t len){if(!len || a>UINTPTR_MAX-len)return false;bool any=false;unsigned n=nregions.load(std::memory_order_acquire);
    for(unsigned i=0;i<n;i++){uintptr_t b=regions[i].begin.load(std::memory_order_relaxed),e=regions[i].end.load(std::memory_order_relaxed);
        uintptr_t lo=std::max(a,b),hi=std::min(a+len,e);if(lo>=hi)continue;size_t p0=(lo-b)>>12,p1=(hi-1-b)>>12;
        if(mprotect(reinterpret_cast<void*>(b+(p0<<12)),(p1-p0+1)<<12,PROT_READ|PROT_WRITE))continue;
        for(size_t pg=p0;pg<=p1;pg++)regions[i].dirty[pg].store(1,std::memory_order_release);any=true;}
    return any;}
void handler(int sig,siginfo_t *si,void *uc){
    if(unprotect(uintptr_t(si->si_addr),1))return;
    if(previous.sa_flags&SA_SIGINFO){if(previous.sa_sigaction){previous.sa_sigaction(sig,si,uc);return;}}
    else if(previous.sa_handler!=SIG_DFL && previous.sa_handler!=SIG_IGN && previous.sa_handler){previous.sa_handler(sig);return;}
    signal(sig,SIG_DFL); // return and re-fault into the default crash path
}
struct Version{VkBuffer buffer{};VkDeviceMemory memory{};uint8_t *mapped=nullptr;bool coherent=true;uint64_t last_use=0;std::vector<uint8_t> stale;};
struct Tracked{uint64_t address=0;size_t bytes=0;uintptr_t pbegin=0;size_t npages=0;unsigned region=0;Version v[versions];uint64_t uses=0;size_t verify_next=0;};
std::map<std::pair<uint64_t,size_t>,std::unique_ptr<Tracked>> tracked;bool exhausted=false;
uint64_t copied_bytes=0,dirty_pages=0,mismatches=0,fallbacks=0;
bool activate(Tracked &t){
    if(!installed){   // a host that routes faults to xbg_wtrack_fault marks this installed (xbg_wtrack_use_host_hooks)
        struct sigaction sa{};sa.sa_sigaction=handler;sa.sa_flags=SA_SIGINFO|SA_ONSTACK|SA_NODEFER;sigemptyset(&sa.sa_mask);if(sigaction(SIGSEGV,&sa,&previous)){enabled=false;return false;}installed=true;}
    unsigned n=nregions.load();if(n>=max_regions)return false;
    t.pbegin=uintptr_t(t.address)&~uintptr_t(4095);uintptr_t pend=(uintptr_t(t.address)+t.bytes+4095)&~uintptr_t(4095);
    for(unsigned i=0;i<n;i++){uintptr_t b=regions[i].begin.load(),e=regions[i].end.load();if(t.pbegin<e && pend>b)return false;} // overlapping regions unsupported
    // Pages the HLE's dirty-page tracker has ever protected (the vertex/index cache) are its for good: its fault hook runs
    // first and takes a fault on such a page as already handled, so a protection of ours there would re-fault forever.
    if(nvn_dirty_since && nvn_dirty_tracked)for(uintptr_t pg=t.pbegin;pg<pend;pg+=4096){const void *q=reinterpret_cast<const void*>(pg);if(nvn_dirty_tracked(q,1) && nvn_dirty_since(q,1,0)){xbg_logf("NVN wtrack region refused addr=%#llx: page %#llx belongs to the dirty-page tracker",(unsigned long long)t.address,(unsigned long long)pg);return false;}}
    size_t npages=(pend-t.pbegin)>>12;
    auto *dirty=new std::atomic<uint8_t>[npages];for(size_t i=0;i<npages;i++)dirty[i].store(0);
    for(auto &v:t.v){
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=t.bytes;ci.usage=arena_usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;VK(vkCreateBuffer(cfg.device,&ci,nullptr,&v.buffer));
        VkMemoryRequirements r;vkGetBufferMemoryRequirements(cfg.device,v.buffer,&r);uint32_t type=find_memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        v.coherent=(memories.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=type;VK(vkAllocateMemory(cfg.device,&ai,nullptr,&v.memory));VK(vkBindBufferMemory(cfg.device,v.buffer,v.memory,0));
        void *p=nullptr;VK(vkMapMemory(cfg.device,v.memory,0,VK_WHOLE_SIZE,0,&p));v.mapped=static_cast<uint8_t*>(p);v.stale.assign(npages,1);
    }
    regions[n].dirty=dirty;regions[n].begin.store(t.pbegin);regions[n].end.store(pend);nregions.store(n+1,std::memory_order_release);t.region=n;
    if(mprotect(reinterpret_cast<void*>(t.pbegin),pend-t.pbegin,PROT_READ)){regions[n].end.store(t.pbegin);return false;}
    t.npages=npages;xbg_logf("NVN wtrack region=%u addr=%#llx bytes=%zu pages=%zu",n,(unsigned long long)t.address,t.bytes,t.npages);return true;
}
void copy_page(Tracked &t,Version &v,size_t pg){
    uint64_t lo=std::max<uint64_t>(t.address,t.pbegin+(pg<<12)),hi=std::min<uint64_t>(t.address+t.bytes,t.pbegin+((pg+1)<<12));
    if(hi>lo){memcpy(v.mapped+(lo-t.address),reinterpret_cast<const void*>(uintptr_t(lo)),size_t(hi-lo));copied_bytes+=hi-lo;}
}
bool snapshot(uint64_t address,size_t bytes,Buffer &out){
    auto &slot=tracked[{address,bytes}];
    if(!slot){if(exhausted)return false;slot=std::make_unique<Tracked>();slot->address=address;slot->bytes=bytes;if(!activate(*slot)){slot->npages=0;exhausted=nregions.load()>=max_regions;}}
    Tracked &t=*slot;if(!t.npages)return false;
    // Clear the flag, re-protect, then copy: a write after the protect faults and re-marks the page.
    Region &r=regions[t.region];
    for(size_t pg=0;pg<t.npages;){
        if(!r.dirty[pg].exchange(0,std::memory_order_acq_rel)){++pg;continue;}
        size_t end=pg+1;while(end<t.npages && r.dirty[end].exchange(0,std::memory_order_acq_rel))++end;
        if(mprotect(reinterpret_cast<void*>(t.pbegin+(pg<<12)),(end-pg)<<12,PROT_READ)){enabled=false;xbg_logf("NVN wtrack mprotect failed errno=%d; disabled",errno);return false;}
        for(size_t i=pg;i<end;i++)for(auto &v:t.v)v.stale[i]=1;dirty_pages+=end-pg;pg=end;
    }
    Version *pick=nullptr;
    for(auto &v:t.v)if(v.last_use==slot_serial){pick=&v;break;}
    if(pick && std::find(pick->stale.begin(),pick->stale.end(),uint8_t(1))!=pick->stale.end())pick=nullptr; // changed within this slot: take another version
    if(!pick)for(auto &v:t.v)if(v.last_use+2<=slot_serial && (!pick || v.last_use>pick->last_use))pick=&v;
    if(!pick){++fallbacks;return false;}
    for(size_t pg=0;pg<t.npages;pg++)if(pick->stale[pg]){copy_page(t,*pick,pg);pick->stale[pg]=0;}
    // Spot check for writes the fault path missed: 16 pages per use, cycling through the range (a full pass of a 19.6 MB
    // buffer every ~300 uses, about the old rate). Reading the copy back is slow (arena memory on Adreno 730 reads at
    // ~1.1 GB/s), so the former check of the whole range every 240 uses read 19.6 MB in one frame, ~18 ms at that speed.
    ++t.uses;{size_t bad=0;for(unsigned k=0;k<16 && k<t.npages;k++){size_t pg=t.verify_next++%t.npages;uint64_t lo=std::max<uint64_t>(t.address,t.pbegin+(pg<<12)),hi=std::min<uint64_t>(t.address+t.bytes,t.pbegin+((pg+1)<<12));
            if(hi>lo && memcmp(pick->mapped+(lo-t.address),reinterpret_cast<const void*>(uintptr_t(lo)),size_t(hi-lo))){++bad;copy_page(t,*pick,pg);}}
        if(bad){mismatches+=bad;xbg_logf("NVN wtrack MISMATCH addr=%#llx pages=%zu (untracked writes; resynced)",(unsigned long long)t.address,bad);}}
    if(!pick->coherent){VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};mr.memory=pick->memory;mr.size=VK_WHOLE_SIZE;VK(vkFlushMappedMemoryRanges(cfg.device,1,&mr));}
    pick->last_use=slot_serial;out=Buffer{};out.buffer=pick->buffer;out.memory=pick->memory;out.offset=0;out.size=bytes;out.mapped=pick->mapped;out.coherent=pick->coherent;return true;
}
// Renderer shutdown, once the queue is idle (from main's 65721a2): the tracked guest pages become writable again and the
// tracked copies are freed, so a restarted renderer tracks afresh; a fault handler the engine installed itself is replaced by
// the previous one, while host fault hooks stay with the host. Tracking stays enabled for the next start.
void release(){
    for(unsigned i=0;i<nregions.load();i++){
        uintptr_t b=regions[i].begin.load(),e=regions[i].end.load();
        if(e>b)need(mprotect(reinterpret_cast<void*>(b),e-b,PROT_READ|PROT_WRITE)==0,"Restore tracked guest pages failed");
        delete[] regions[i].dirty;regions[i].dirty=nullptr;regions[i].begin=0;regions[i].end=0;
    }
    nregions=0;
    for(auto &t:tracked)for(auto &v:t.second->v){
        if(v.mapped)vkUnmapMemory(cfg.device,v.memory);
        if(v.buffer)vkDestroyBuffer(cfg.device,v.buffer,nullptr);
        if(v.memory)vkFreeMemory(cfg.device,v.memory,nullptr);
    }
    tracked.clear();exhausted=false;
    if(installed && !host_hooks){sigaction(SIGSEGV,&previous,nullptr);installed=false;}
}
}
// Host fault hooks for write tracking. A host with one SIGSEGV handler of its own (dm crash.c) calls
// xbg_wtrack_use_host_hooks() once and routes faults here instead of letting the engine install a handler;
// xbg_wtrack_prepare is for host writes into guest memory that do not fault (a syscall gets EFAULT).
// With the host owning the fault handler, write tracking is on unless a marker turns it off (Redmi K50 Ultra camp A/B: CPU
// -1.15 ms per frame, no mismatches); without it, it stays opt-in (gfx-wtrack = '1') with the engine's own handler.
extern "C" void xbg_wtrack_use_host_hooks(void){wtrack::installed=wtrack::host_hooks=true;if(wtrack::marker!=0)wtrack::enabled=true;xbg_logf("NVN wtrack through host hooks enabled=%d",int(wtrack::enabled));}
extern "C" int xbg_wtrack_fault(void *addr){return wtrack::unprotect(uintptr_t(addr),1);}
extern "C" void xbg_wtrack_prepare(void *addr,size_t len){if(len)wtrack::unprotect(uintptr_t(addr),len);}
// Texture content checks through the write-protect tracker (nvn_dirty.c), as nvn_mtl.m tex_mtl does on iOS: once a texture
// has been unchanged for 4 checks its pages are write-protected with the tracker's stamp, and a check then hashes it only if
// the guest wrote its pages since. The hash checks re-hashed every bound texture on its 16th submission (~0.27 ms of the
// render thread per D2R fight frame on the Redmi K50 Ultra) and saw a streamed write up to 16 submissions late. Untracked
// pools keep them. Marker gfx-tex-dirty = 1 / 0, else DM_GFX_TEX_DIRTY (the Android app sets 1); A/B target texdirty;
// NVN PERF texdirty: textures that started tracking, checks that hashed after a write, changes found.
bool tex_dirty=false;uint64_t tex_dirty_tracked=0,tex_dirty_checks=0,tex_dirty_changed=0;
// Cross-frame copies of vertex and index ranges (marker gfx-vcache = 1 / 0, else DM_GFX_VCACHE, which the Android app sets
// to 1; A/B target vcache). In the D2R camp 99% of the 6.4 MB of
// vertex data and all 1.45 MB of index data a frame copies are the same as the frame before (diagnostic gfx-snap-census).
// A range is copied once into a persistent arena and reused while the HLE's dirty-page tracker reports no write to its
// pages since the copy: take the stamp, write-protect the pages, then copy, so any later write faults and is newer than the
// stamp. Copies are never rewritten (a changed range gets a new one), so in-flight submissions keep reading theirs. A range
// that changes again within 8 submissions of being copied is volatile: it is left to the per-submission snapshot for 600
// submissions, and its pages are no longer protected. Ranges overlapping write-tracked storage are skipped and write tracking
// refuses pages this tracker has protected: two trackers must never share a page (each would take the other's faults).
// Entries in a deleted pool are dropped, and past the budget the whole arena is handed to the retiring slot and the cache
// starts over.
namespace vcache {
bool enabled=false;uint64_t hits=0,hit_bytes=0,fills=0,fill_bytes=0,refills=0,volatiles=0,untracked=0,flushes=0;
constexpr size_t min_bytes=256;constexpr VkDeviceSize budget=64ull<<20;constexpr uint64_t volatile_window=8,retry_after=600;
enum State:uint8_t{EMPTY,CACHED,VOLATILE,UNTRACKED};
struct Entry{Buffer buffer;uint64_t stamp=0,filled=0,retry=0;State state=EMPTY;};
struct KeyHash{size_t operator()(const std::pair<uint64_t,uint64_t> &k)const{uint64_t x=k.first*0x9E3779B97F4A7C15ull^k.second*0xC2B2AE3D27D4EB4Full;return size_t(x^(x>>29));}};
std::unordered_map<std::pair<uint64_t,uint64_t>,Entry,KeyHash> map;Arena arena;
size_t live(){return map.size();}
VkDeviceSize arena_bytes(){return arena.used_total;}
bool overlaps_wtrack(uint64_t a,uint64_t n){unsigned k=wtrack::nregions.load(std::memory_order_acquire);
    for(unsigned i=0;i<k;i++){uint64_t b=wtrack::regions[i].begin.load(std::memory_order_relaxed),e=wtrack::regions[i].end.load(std::memory_order_relaxed);if(a<e && a+n>b)return true;}return false;}
void flush(){if(arena.chunks.empty() && map.empty())return;std::vector<ArenaChunk> old;old.swap(arena.chunks);arena.current=0;arena.used_total=0;arena.peak=0;map.clear();++flushes;
    retire_handle([old]()mutable{for(auto &c:old)Arena::destroy(c);});}   // after the recording slot retires, no submission reads them
void drop_pool(uint64_t base,uint64_t bytes){for(auto it=map.begin();it!=map.end();){if(it->first.first<base+bytes && it->first.first+it->first.second>base)it=map.erase(it);else ++it;}}
bool lookup(uint64_t address,size_t bytes,Buffer &out){
    if(bytes<min_bytes || !nvn_dirty_since || !nvn_dirty_tracked || !nvn_dirty_protect || !nvn_dirty_now)return false;
    const void *p=reinterpret_cast<const void*>(uintptr_t(address));Entry &e=map[{address,uint64_t(bytes)}];
    if(e.state==VOLATILE || e.state==UNTRACKED){if(slot_serial<e.retry)return false;e.state=EMPTY;}
    if(e.state==CACHED){
        if(!nvn_dirty_since(p,bytes,e.stamp)){++hits;hit_bytes+=bytes;out=e.buffer;return true;}
        if(slot_serial-e.filled<volatile_window){e.state=VOLATILE;e.retry=slot_serial+retry_after;++volatiles;return false;}
        ++refills;
    }
    if(!nvn_dirty_tracked(p,bytes) || overlaps_wtrack(address,bytes)){e.state=UNTRACKED;e.retry=slot_serial+retry_after;++untracked;return false;}
    if(arena.used_total+bytes>budget)flush();
    Entry &f=map[{address,uint64_t(bytes)}];   // flush() may have cleared the map
    uint64_t stamp=nvn_dirty_now();nvn_dirty_protect(p,bytes);
    Buffer b=arena.alloc(bytes);memcpy(b.mapped,p,bytes);b.flush();
    f.buffer=b;f.stamp=stamp;f.filled=slot_serial;f.state=CACHED;++fills;fill_bytes+=bytes;out=b;return true;
}
}
Buffer snapshot_slow(uint64_t address,size_t bytes,VkDeviceSize align);
Buffer snapshot(uint64_t address,size_t bytes,VkDeviceSize align=4){CPU_SCOPE(Snapshot);
    SnapFront &f=snap_front_for(0,address,bytes);if(f.epoch==snapshot_epoch && f.address==address && f.bytes==bytes){++perf.snapshot_hits;++snap_front_hits;return f.buffer;}
    Buffer b=snapshot_slow(address,bytes,align);f.address=address;f.bytes=bytes;f.epoch=snapshot_epoch;f.buffer=b;return b;
}
Buffer snapshot_slow(uint64_t address,size_t bytes,VkDeviceSize align){
    auto key=std::make_pair(address,uint64_t(bytes));auto it=snapshots.find(key);
    if(it!=snapshots.end()){++perf.snapshot_hits;++snap_map_hits;return it->second;}
    {Buffer view;if(pending_view(address,bytes,align,view)){snapshots.emplace(key,view);snapshot_maxlen=std::max<uint64_t>(snapshot_maxlen,bytes);return view;}}
    ++perf.snapshot_misses;perf.snapshot_bytes+=bytes;snap_site_req[snap_site<6?snap_site:0]+=bytes;if(snap_site==3){static unsigned logged=0;if(logged<40 && bytes>=65536){++logged;xbg_logf("NVN PERF storage_snapshot addr=%#llx bytes=%zu frame=%llu",(unsigned long long)address,bytes,(unsigned long long)stats.presents.load());}++storage_snap_count;if(timing_now())for(uint64_t pg=address>>12;pg<=(address+bytes-1)>>12;pg++)storage_pages.insert(pg);}   // page union: timed frames only (~4.8k inserts per D2R frame)
    cpu_access(address,bytes,1);   // a pending GPU result that only partly covers the range (or is misaligned for a view) lands first
    if(snap_site==3 && bytes>=wtrack::min_bytes && wtrack::enabled && !ab_off("wtrack")){Buffer t;if(wtrack::snapshot(address,bytes,t)){snapshots.emplace(key,t);snapshot_maxlen=std::max<uint64_t>(snapshot_maxlen,bytes);return t;}}
    if((snap_site==1 || snap_site==2) && vcache::enabled && !ab_off("vcache")){Buffer v;if(vcache::lookup(address,bytes,v)){snapshots.emplace(key,v);snapshot_maxlen=std::max<uint64_t>(snapshot_maxlen,bytes);return v;}}
    snap_site_bytes[snap_site<6?snap_site:0]+=bytes;
    Buffer b=stream_buffer(bytes,reinterpret_cast<const void*>(address),bytes,false);snapshots.emplace(key,b);snapshot_maxlen=std::max<uint64_t>(snapshot_maxlen,bytes);return b;
}
struct Commands {
    VkCommandBuffer command{};bool draw=false;
    // Non-draw operations keep the former submit-per-operation semantics: they
    // end any open pass and are fenced by full barriers before and after.
    explicit Commands(bool is_draw=false):draw(is_draw){Slot &s=recording_slot();command=s.cmd;++perf.ops;++s.ops;
        if(!draw){end_open_pass();full_barrier(command);}}
    void finish(){if(!draw){end_open_pass();full_barrier(command);}}
};
VkFormat format(int f) {
    static const VkFormat table[]={VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8_UNORM,VK_FORMAT_R8_SNORM,VK_FORMAT_R8_UINT,VK_FORMAT_R8_SINT,
        VK_FORMAT_R16_SFLOAT,VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_UINT,VK_FORMAT_R16_SINT,
        VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32_UINT,VK_FORMAT_R32_SINT,
        VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16_UINT,VK_FORMAT_R16G16_SINT,
        VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R8G8B8_SNORM,VK_FORMAT_R8G8B8_UINT,VK_FORMAT_R8G8B8_SINT,
        VK_FORMAT_R16G16B16_SFLOAT,VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16_SNORM,VK_FORMAT_R16G16B16_UINT,VK_FORMAT_R16G16B16_SINT,
        VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_R8G8B8A8_UINT,VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SNORM,VK_FORMAT_R16G16B16A16_UINT,VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_S8_UINT,VK_FORMAT_D16_UNORM,VK_FORMAT_X8_D24_UNORM_PACK32,VK_FORMAT_D32_SFLOAT,VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_D32_SFLOAT_S8_UINT};
    if(f>0 && f<int(sizeof(table)/sizeof(*table))) return table[f];
    // NVN ASTC 0x79-0x86 (LDR) / 0x87-0x94 (sRGB), order 4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12 = Vulkan order.
    if(f>=0x79 && f<=0x94){int k=(f-0x79)%14;return VkFormat(int(VK_FORMAT_ASTC_4x4_UNORM_BLOCK)+2*k+(f>=0x87?1:0));}
    // NVN BC 0x41-0x50: DXT1 RGB/RGBA, DXT3, DXT5, the same four sRGB, RGTC1 U/S, RGTC2 U/S, BPTC unorm/sRGB, BPTC sfloat/ufloat
    // (the decoder: 0x41 opaque BC1, 0x4f signed BC6H). Used as such only with native_bc; otherwise BC is decoded on upload.
    if(f>=0x41 && f<=0x50){static const VkFormat bc[16]={VK_FORMAT_BC1_RGB_UNORM_BLOCK,VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC2_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_BC1_RGB_SRGB_BLOCK,VK_FORMAT_BC1_RGBA_SRGB_BLOCK,VK_FORMAT_BC2_SRGB_BLOCK,VK_FORMAT_BC3_SRGB_BLOCK,VK_FORMAT_BC4_UNORM_BLOCK,VK_FORMAT_BC4_SNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK,VK_FORMAT_BC5_SNORM_BLOCK,VK_FORMAT_BC7_UNORM_BLOCK,VK_FORMAT_BC7_SRGB_BLOCK,VK_FORMAT_BC6H_SFLOAT_BLOCK,VK_FORMAT_BC6H_UFLOAT_BLOCK};return bc[f-0x41];}
    switch(f){case 0x37:case 0x38:return VK_FORMAT_R8G8B8A8_SRGB;case 0x3d:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;case 0x3e:return VK_FORMAT_A2B10G10R10_UINT_PACK32;case 0x3f:return VK_FORMAT_B10G11R11_UFLOAT_PACK32;case 0x40:return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;case 0x6d:return VK_FORMAT_R8G8B8A8_UNORM;case 0x9a:return VK_FORMAT_B8G8R8A8_UNORM;default:xbg_fatalf("Unsupported NVN format %#x",f);}
}
VkImageAspectFlags aspect(int f){if(f==0x31)return VK_IMAGE_ASPECT_STENCIL_BIT;if(f>=0x32 && f<=0x34)return VK_IMAGE_ASPECT_DEPTH_BIT;if(f==0x35 || f==0x36)return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;return VK_IMAGE_ASPECT_COLOR_BIT;}
VkFormat decoded_format(uint32_t pixels) {
    switch(pixels){
    case XBG_RGBA8_UNORM:return VK_FORMAT_R8G8B8A8_UNORM;
    case XBG_RGBA8_SRGB:return VK_FORMAT_R8G8B8A8_SRGB;
    case XBG_R16_UNORM:return VK_FORMAT_R16_UNORM;
    case XBG_RG16_UNORM:return VK_FORMAT_R16G16_UNORM;
    case XBG_R16_SNORM:return VK_FORMAT_R16_SNORM;
    case XBG_RG16_SNORM:return VK_FORMAT_R16G16_SNORM;
    case XBG_RGBA16_SFLOAT:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case XBG_R8_UNORM:return VK_FORMAT_R8_UNORM;
    case XBG_RG8_UNORM:return VK_FORMAT_R8G8_UNORM;
    default:xbg_fatalf("Invalid decoded BC pixel format %u",pixels);
    }
}
// Read-only capability report, logged once at initialization: what this device actually supports, so a missing capability can be
// named precisely (a Vulkan 1.1 driver lacks much of what 1.3 drivers report) instead of being read off the first fatal error.
void capability_report(){
    const VkPhysicalDeviceFeatures &f=cfg.enabled_core;const VkPhysicalDeviceLimits &l=properties.limits;
    xbg_logf("NVN caps device=\"%s\" vendor=%#x id=%#x api=%u.%u.%u driver=%#x",properties.deviceName,properties.vendorID,properties.deviceID,
        VK_API_VERSION_MAJOR(properties.apiVersion),VK_API_VERSION_MINOR(properties.apiVersion),VK_API_VERSION_PATCH(properties.apiVersion),properties.driverVersion);
    xbg_logf("NVN caps features drawParameters=%u storageReadWithoutFormat=%u storageWriteWithoutFormat=%u storageExtendedFormats=%u gatherExtended=%u "
        "tessellation=%u geometry=%u occlusionQueryPrecise=%u depthBounds=%u depthClamp=%u fillModeNonSolid=%u bc=%u astcLdr=%u etc2=%u "
        "anisotropy=%u logicOp=%u dualSrcBlend=%u independentBlend=%u clipDistance=%u cullDistance=%u float64=%u int64=%u int16=%u",
        unsigned(cfg.enabled_shader_draw_parameters),f.shaderStorageImageReadWithoutFormat,f.shaderStorageImageWriteWithoutFormat,f.shaderStorageImageExtendedFormats,
        f.shaderImageGatherExtended,f.tessellationShader,f.geometryShader,f.occlusionQueryPrecise,f.depthBounds,f.depthClamp,f.fillModeNonSolid,
        f.textureCompressionBC,f.textureCompressionASTC_LDR,f.textureCompressionETC2,f.samplerAnisotropy,f.logicOp,f.dualSrcBlend,f.independentBlend,
        f.shaderClipDistance,f.shaderCullDistance,f.shaderFloat64,f.shaderInt64,f.shaderInt16);
    xbg_logf("NVN caps subgroup size=%u stages=%#x ops=%#x quadAllStages=%u",subgroup.subgroupSize,subgroup.supportedStages,subgroup.supportedOperations,subgroup.quadOperationsInAllStages);
    xbg_logf("NVN caps limits ubRange=%u sbRange=%u ubDyn=%u sbDyn=%u ubAlign=%llu sbAlign=%llu texelAlign=%llu stageSamplers=%u stageSampled=%u "
        "stageStorageImages=%u stageUB=%u stageSB=%u boundSets=%u colorAttachments=%u sharedMemory=%u workgroupInvocations=%u viewports=%u maxAnisotropy=%g",
        l.maxUniformBufferRange,l.maxStorageBufferRange,l.maxDescriptorSetUniformBuffersDynamic,l.maxDescriptorSetStorageBuffersDynamic,
        (unsigned long long)l.minUniformBufferOffsetAlignment,(unsigned long long)l.minStorageBufferOffsetAlignment,(unsigned long long)l.minTexelBufferOffsetAlignment,
        l.maxPerStageDescriptorSamplers,l.maxPerStageDescriptorSampledImages,l.maxPerStageDescriptorStorageImages,l.maxPerStageDescriptorUniformBuffers,
        l.maxPerStageDescriptorStorageBuffers,l.maxBoundDescriptorSets,l.maxColorAttachments,l.maxComputeSharedMemorySize,l.maxComputeWorkGroupInvocations,
        l.maxViewports,double(l.maxSamplerAnisotropy));
    // Every core format up to ASTC 12x12 sRGB (BC, ETC2 and ASTC included) as vkformat:optimalTilingFeatures/bufferFeatures
    // (VkFormatFeatureFlagBits); formats with neither are left out.
    std::string line;unsigned n=0;
    for(int v=1;v<=int(VK_FORMAT_ASTC_12x12_SRGB_BLOCK);v++){const VkFormatProperties &p=format_properties(VkFormat(v));if(!p.optimalTilingFeatures && !p.bufferFeatures)continue;
        char item[40];snprintf(item,sizeof(item)," %d:%#x/%#x",v,p.optimalTilingFeatures,p.bufferFeatures);line+=item;
        if(++n%10==0){xbg_logf("NVN caps formats%s",line.c_str());line.clear();}}
    if(!line.empty())xbg_logf("NVN caps formats%s",line.c_str());
}
struct Image {
    tex_t source{}; uint64_t provenance_epoch=0,provenance_writer=0;unsigned provenance_write_site=0; VkImage image{};VkDeviceMemory memory{};VkFormat fmt{};VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspects{};VkImageUsageFlags usage{};uint32_t width{},height{},depth=1,levels{},layers{},bpp{};VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT;bool uploaded=false,gpu_written=false,decoded=false,mutable_format=false;VkImageCreateInfo create{}; // create: as created (no pNext), for upgrade_mutable
    uint64_t cpu_hash=0,checked_submission=UINT64_MAX,last_write=0;uint32_t stable_checks=0;int64_t mem_bytes=0,bc_bytes=0,bc_data=0;
    // 最近一次经 get_image 取用时的 slot_serial：等于当前值，说明这次提交里已有命令用过它（gfx-upload-prologue 据此判断能否提前上传）。
    // slot_serial at the last get_image of this image: equal to the current value means a command of this submission already used it
    // (gfx-upload-prologue decides by it whether an upload may go ahead of the submission).
    uint64_t slot_use=0;
    uint64_t dirty_stamp=0,dirty_seen=0;   // tex_dirty: the tracker stamp of the checked content (0: not tracked yet) and the tracker generation last checked
    std::map<std::array<uint32_t,8>,VkImageView> views;std::vector<std::vector<uint64_t>> fb_keys; // fb_keys: persistent framebuffers using these views
    ~Image(){image_bytes_live-=mem_bytes;bc_image_bytes_live-=bc_bytes;bc_data_bytes_live-=bc_data;forget_framebuffers(*this);for(auto &v:views)vkDestroyImageView(cfg.device,v.second,nullptr);if(image)vkDestroyImage(cfg.device,image,nullptr);if(memory)vkFreeMemory(cfg.device,memory,nullptr);}
    void transition(VkCommandBuffer cmd,VkImageLayout next){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=layout;b.newLayout=next;b.image=image;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={aspects,0,levels,0,layers};b.srcAccessMask=layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);layout=next;
    }
    VkFormat view_format(const view_t *v) const {
        if(!v || !v->set_format || v->format==source.d.format)return fmt;
        if(decoded){
            need(xbg_bc_view_compatible(source.d.format,v->format),"BC texture view must retain compression and signedness");
            xbg_bc_format info;need(xbg_bc_format_info(v->format,&info)==0,"Invalid BC view format");
            return decoded_format(info.pixels);
        }
        if(source.d.format>=0x41 && source.d.format<=0x50){need(xbg_bc_view_compatible(source.d.format,v->format),"BC texture view must retain compression and signedness");return format(v->format);}   // native BC
        int bw,bh,bytes=nvn_fmt_bpb(v->format,&bw,&bh);
        need(aspects==VK_IMAGE_ASPECT_COLOR_BIT && aspect(v->format)==aspects && bw==1 && bh==1 && bytes==int(bpp),"Incompatible NVN texture view format class");
        return format(v->format);
    }
    VkImageView view(const view_t *v,bool attachment=false,bool storage=false){
        uint32_t base=v&&v->set_levels?v->base_level:0,n=v&&v->set_levels?v->nlevels:levels;
        uint32_t layer=v&&v->set_layers?v->min_layer:0,ls=v&&v->set_layers?v->nlayers:layers;
        need(base<levels && n && n<=levels-base && layer<layers && ls && ls<=layers-layer,"Invalid texture view range");
        if(attachment){need(ls==1,"Layered rendering not implemented");n=1;}
        VkFormat vf=view_format(v);
        if(vf!=fmt){static std::set<std::pair<int,int>> seen;if(seen.insert({int(fmt),int(vf)}).second)xbg_logf("NVN viewfmt image=%d view=%d nvn=%#x->%#x attachment=%d storage=%d mutable=%d",int(fmt),int(vf),source.d.format,v?v->format:0,int(attachment),int(storage),int(mutable_format));
            if(!mutable_format)upgrade_mutable(*this);}
        else if(mutable_test && !mutable_format && !attachment && !storage && aspects==VK_IMAGE_ASPECT_COLOR_BIT && source.serial%mutable_test==0)upgrade_mutable(*this);
        int target=v&&v->set_target?v->target:source.d.target;
        VkImageViewType type=target==2?VK_IMAGE_VIEW_TYPE_3D:target==0?VK_IMAGE_VIEW_TYPE_1D:target==3?VK_IMAGE_VIEW_TYPE_1D_ARRAY:target==4||target==6?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
        need(!attachment || source.d.target!=2,"Rendering directly to volume slices requires 2D compatible views");
        if(!attachment && (target==8 || target==9))type=target==8?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        if(attachment){need(source.d.target!=0 && source.d.target!=3,"1D render targets require 2D storage adaptation");type=VK_IMAGE_VIEW_TYPE_2D;}
        VkImageAspectFlags view_aspects=!attachment && (aspects&VK_IMAGE_ASPECT_DEPTH_BIT)?VK_IMAGE_ASPECT_DEPTH_BIT:aspects;
        VkImageUsageFlags view_usage=attachment?(aspects==VK_IMAGE_ASPECT_COLOR_BIT?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT):storage?VK_IMAGE_USAGE_STORAGE_BIT:VK_IMAGE_USAGE_SAMPLED_BIT;
        need((usage&view_usage)==view_usage,"Image was not allocated for requested view usage");
        const VkFormatProperties &supported=format_properties(vf);
        VkFormatFeatureFlags feature=attachment?(aspects==VK_IMAGE_ASPECT_COLOR_BIT?VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT:VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT):storage?VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT:VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        need(supported.optimalTilingFeatures&feature,"Actual view format does not support requested usage");
        std::array<uint32_t,8> key{base,n,layer,ls,uint32_t(vf),uint32_t(type),view_aspects,view_usage};auto it=views.find(key);if(it!=views.end())return it->second;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=type;vi.format=vf;vi.subresourceRange={aspects,base,n,layer,ls};
        vi.subresourceRange.aspectMask=view_aspects;
        VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};vu.usage=view_usage;vi.pNext=&vu;
        unsigned view_code=v&&v->set_format?v->format:source.d.format;
        if(!attachment && !storage && (view_code==0x37 || view_code==0x6d))vi.components.a=VK_COMPONENT_SWIZZLE_ONE;
        VkImageView result;VK(vkCreateImageView(cfg.device,&vi,nullptr,&result));views[key]=result;return result;
    }
};
std::map<uint64_t,std::shared_ptr<Image>> images;
// Address-ordered index of image storage, rebuilt lazily when images change.
struct ImageRange {uint64_t begin,end;Image *image;};
std::vector<ImageRange> image_index;bool image_index_dirty=true;uint64_t image_index_maxlen=0;
void rebuild_image_index(){
    image_index.clear();image_index_maxlen=0;
    for(auto &e:images){uint64_t b=uintptr_t(e.second->source.mem);if(!b)continue;uint64_t n=e.second->source.L.size;image_index.push_back({b,b+n,e.second.get()});image_index_maxlen=std::max(image_index_maxlen,n);}
    std::sort(image_index.begin(),image_index.end(),[](const ImageRange &a,const ImageRange &b){return a.begin<b.begin;});image_index_dirty=false;
}
template<class F> void for_images_overlapping(uint64_t address,uint64_t bytes,F f){
    if(image_index_dirty)rebuild_image_index();
    uint64_t lo=address>image_index_maxlen?address-image_index_maxlen:0;
    auto it=std::lower_bound(image_index.begin(),image_index.end(),lo,[](const ImageRange &r,uint64_t v){return r.begin<v;});
    for(;it!=image_index.end() && it->begin<address+bytes;++it)if(it->end>address)f(*it->image);
}
// A sampled/storage read inside a draw: barrier only if the image may have been
// written since the last full barrier, is an attachment of the open pass, or
// needs a layout change.
void prepare_read(Image &im,VkCommandBuffer cmd){
    bool in_open=open_pass.cmd && std::find(open_pass.images.begin(),open_pass.images.end(),&im)!=open_pass.images.end();
    if(im.layout!=VK_IMAGE_LAYOUT_GENERAL || im.last_write>barrier_seq || in_open){
        end_open_pass();if(im.layout!=VK_IMAGE_LAYOUT_GENERAL)im.transition(cmd,VK_IMAGE_LAYOUT_GENERAL);full_barrier(cmd);
    }
}
void prepare_attachment(Image &im,VkCommandBuffer cmd){
    bool in_open=open_pass.cmd && std::find(open_pass.images.begin(),open_pass.images.end(),&im)!=open_pass.images.end();
    if(im.layout!=VK_IMAGE_LAYOUT_GENERAL && !in_open){end_open_pass();im.transition(cmd,VK_IMAGE_LAYOUT_GENERAL);}
    im.last_write=++write_seq;
}
void provenance_image(const char *event,const tex_t *t,const Image &im,const view_t *v=nullptr){
    if(!provenance_file || !provenance.window(stats.presents.load()))return;
    provenance_log("IMAGE event=%s requested=%llu source=%llu image=%p mem=%p pool=%p off=%lld bytes=%llu createEpoch=%llu currentEpoch=%llu writer=%llu writeSite=%u writerPhase=declared-or-complete-range-unspecified uploaded=%u gpu=%u fmt=%d target=%d extent=%dx%dx%d levels=%d samples=%d flags=%x stride=%d pkg=%d viewFmt=%d mip=%d/%d layer=%d/%d",
    event,(unsigned long long)t->serial,(unsigned long long)im.source.serial,(void*)im.image,t->mem,(void*)(t->mem && t->off>=0 && uint64_t(t->off)<=uint64_t(t->mem)?uint64_t(t->mem)-uint64_t(t->off):0),(long long)t->off,(unsigned long long)t->L.size,(unsigned long long)im.provenance_epoch,(unsigned long long)provenance.epoch((t->mem && t->off>=0 && uint64_t(t->off)<=uint64_t(t->mem)?uint64_t(t->mem)-uint64_t(t->off):0)),(unsigned long long)im.provenance_writer,im.provenance_write_site,im.uploaded,im.gpu_written,t->d.format,t->d.target,t->d.width,t->d.height,t->d.depth,t->d.levels,t->d.samples,t->d.flags,t->d.stride,t->d.pkg_block_h,v&&v->set_format?v->format:t->d.format,v&&v->set_levels?v->base_level:0,v&&v->set_levels?v->nlevels:int(im.levels),v&&v->set_layers?v->min_layer:0,v&&v->set_layers?v->nlayers:int(im.layers));
}
void provenance_write(Image &im,unsigned site){im.provenance_writer=++provenance.sequence;im.provenance_write_site=site;provenance_image("WRITE",&im.source,im);}

// Invoked only after a completed game draw/clear fence. Read the real Vulkan
// subresources and restore their original layout before guest execution resumes.
void capture_image(Image &im,const view_t *view,const char *role){
    if(!capture.active)return;
    uint32_t base=view&&view->set_levels?view->base_level:0,levels=view&&view->set_levels?view->nlevels:im.levels;
    uint32_t layer=view&&view->set_layers?view->min_layer:0,layers=view&&view->set_layers?view->nlayers:im.layers;
    capture_log("IMAGE role=%s serial=%llu image=%p source=%p sourceBytes=%llu format=%#x vk=%u extent=%ux%ux%u levels=%u baseMip=%u count=%u baseLayer=%u layers=%u layout=%u uploaded=%u gpuWritten=%u",role,(unsigned long long)im.source.serial,(void*)im.image,im.source.mem,(unsigned long long)im.source.L.size,im.source.d.format,unsigned(im.fmt),im.width,im.height,im.depth,im.levels,base,levels,layer,layers,unsigned(im.layout),im.uploaded,im.gpu_written);
    if(im.samples!=VK_SAMPLE_COUNT_1_BIT || im.layout==VK_IMAGE_LAYOUT_UNDEFINED){capture_log("SKIP role=%s reason=uninitialized-or-multisample",role);return;}
    if(base>=im.levels || levels>im.levels-base || layer>=im.layers || layers>im.layers-layer){capture_log("SKIP role=%s reason=invalid-view",role);return;}
    unsigned subresources=0;
    for(uint32_t l=layer;l<layer+layers;l++)for(uint32_t m=base;m<base+levels;m++){
        if(++subresources>16){capture_log("SKIP role=%s reason=16-subresource-limit",role);return;}
        uint32_t w=std::max(1u,im.width>>m),h=std::max(1u,im.height>>m),z=std::max(1u,im.depth>>m);
        VkImageAspectFlags plane=im.aspects&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:im.aspects;
        uint32_t bpp=im.bpp;if(plane==VK_IMAGE_ASPECT_DEPTH_BIT){xbg_depth_format f;if(xbg_depth_format_info(im.source.d.format,&f)){capture_log("SKIP reason=unknown-depth-format");return;}bpp=im.source.d.format==0x32 && depth16_as_d24?4:f.depth_bytes;}
        uint64_t bytes=uint64_t(w)*h*z*bpp;
        if(!bytes || bytes>capture_limit-capture.bytes){capture_log("SKIP role=%s mip=%u layer=%u bytes=%llu reason=64MiB-budget",role,m,l,(unsigned long long)bytes);continue;}
        Buffer buffer=readback_buffer(size_t(bytes));Commands command;VkImageLayout saved=im.layout;
        im.transition(command.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);VkBufferImageCopy region{};region.imageSubresource={plane,m,l,1};region.imageExtent={w,h,z};
        region.bufferOffset=buffer.offset;vkCmdCopyImageToBuffer(command.command,im.image,im.layout,buffer.buffer,1,&region);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=buffer.buffer;host.offset=buffer.offset;host.size=buffer.size;
        vkCmdPipelineBarrier(command.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        im.transition(command.command,saved);command.finish();gpu_sync(perf.sync_readback);buffer.invalidate();
        char label[160];snprintf(label,sizeof(label),"%s-s%llu-m%u-l%u",role,(unsigned long long)im.source.serial,m,l);
        capture_log("PIXELS label=%s vk=%u aspect=%u width=%u height=%u depth=%u rowBytes=%u restoredLayout=%u",label,unsigned(im.fmt),plane,w,h,z,w*bpp,unsigned(im.layout));
        capture_data(label,buffer.mapped,size_t(bytes));
    }
}
std::map<uintptr_t,size_t> pools;
struct RegisteredTexture{tex_t texture{};view_t view{};bool has_view=false;uint64_t revision=0;};
// Bumped by every event that can fail a descriptor memo entry's per-image checks: a texture registration that changes, an
// image created or released for a serial. A memo hit repeats those checks only when it moved since the entry's last ones.
uint64_t memo_check_gen=1;
// 只比较可影响图像/视图的语义字段，忽略 padding、snapshot 指针及未启用的 view 字段。
// Compare image/view semantics only, ignoring padding, snapshot pointers and inactive view fields.
static bool registered_texture_equal(const RegisteredTexture &a,const RegisteredTexture &b){
    const tex_t &x=a.texture,&y=b.texture;
    if(x.serial!=y.serial || x.mem!=y.mem || x.pool!=y.pool || x.off!=y.off ||
       x.d.target!=y.d.target || x.d.format!=y.d.format || x.d.flags!=y.d.flags || x.d.samples!=y.d.samples ||
       x.d.width!=y.d.width || x.d.height!=y.d.height || x.d.depth!=y.d.depth || x.d.levels!=y.d.levels || x.d.stride!=y.d.stride || x.d.pkg_block_h!=y.d.pkg_block_h ||
       x.L.size!=y.L.size || x.L.layer_size!=y.L.layer_size ||
       x.L.align!=y.L.align || x.L.layers!=y.L.layers || x.L.bpb!=y.L.bpb || x.L.tileH!=y.L.tileH || x.L.tileD!=y.L.tileD ||
       !!x.has_layout!=!!y.has_layout || (x.has_layout && memcmp(x.layout,y.layout,sizeof(x.layout))))return false;
    const view_t &u=a.view,&v=b.view;
    return a.has_view==b.has_view && (!a.has_view ||
        (!!u.set_levels==!!v.set_levels && (!u.set_levels || (u.base_level==v.base_level && u.nlevels==v.nlevels)) &&
         !!u.set_layers==!!v.set_layers && (!u.set_layers || (u.min_layer==v.min_layer && u.nlayers==v.nlayers)) &&
         !!u.set_format==!!v.set_format && (!u.set_format || u.format==v.format) &&
         !!u.set_target==!!v.set_target && (!u.set_target || u.target==v.target)));
}
static bool register_texture_record(RegisteredTexture &slot,const tex_t &texture,const view_t *view){
    RegisteredTexture next;next.texture=texture;
    if(view && (view->set_levels || view->set_layers || view->set_format || view->set_target)){next.view=*view;next.has_view=true;}
    bool changed=!slot.revision || !registered_texture_equal(slot,next);
    next.revision=slot.revision+uint64_t(changed);slot=next;if(changed)++memo_check_gen;return changed;
}
struct MemoImage{const tex_t *first;Image *second;const RegisteredTexture *record;uint64_t revision;};
using PoolKey=std::pair<uintptr_t,int64_t>;
PoolKey pool_key(const dpool_t *p){need(p && p->pool,"Missing descriptor pool");return {uintptr_t(p->pool->mem),p->off};}
std::map<PoolKey,std::map<int,RegisteredTexture>> textures;
std::map<int,RegisteredTexture> legacy_textures;std::map<int,samp_par> legacy_samplers;
std::map<int,RegisteredTexture> *texture_pool_cache=nullptr;PoolKey texture_pool_cache_key{}; // descriptors(): last texture pool (entries are never erased before shutdown)
std::map<PoolKey,std::map<int,samp_par>> samplers;
std::map<uint32_t,RegisteredTexture> null_textures;
struct HostSegment{cmd_t *owner;std::unique_ptr<uint8_t[]> memory;size_t bytes;};
std::vector<HostSegment> host_segments;
std::map<uintptr_t,size_t> host_ranges; // address-ordered index of host_segments
size_t host_bytes=0;
uint64_t hash_memory(const void *memory,size_t n){CPU_SCOPE(Hash);const uint8_t *p=static_cast<const uint8_t*>(memory);
    // Word-at-a-time 4-lane mix: change detection only, never persisted.
    uint64_t h[4]={1469598103934665603ull,0x9e3779b97f4a7c15ull,0xc2b2ae3d27d4eb4full,0x165667b19e3779f9ull};size_t i=0;
    for(;i+32<=n;i+=32){for(unsigned k=0;k<4;k++){uint64_t w;memcpy(&w,p+i+8*k,8);h[k]=(h[k]^w)*0x100000001b3ull;h[k]^=h[k]>>29;}}
    uint64_t hash=h[0]^(h[1]*3)^(h[2]*5)^(h[3]*7)^n;for(;i<n;i++){hash^=p[i];hash*=1099511628211ull;}return hash;}
void invalidate_cpu_range(uint64_t address,uint64_t bytes){CPU_SCOPE(CpuWrite);
    need(address+bytes>=address,"CPU write range overflow");cpu_access(address,bytes,4);snapshot_invalidate(address,bytes);
    for_images_overlapping(address,bytes,[&](Image &im){if(draw_detail)draw_timing.image();uint64_t begin=uintptr_t(im.source.mem),end=begin+im.source.L.size;
        // A full storage overwrite replaces the prior GPU contents. A partial
        // overwrite requires GPU-to-NVN readback/merge before applying CPU data.
        need(!im.gpu_written || (address<=begin && address+bytes>=end),"Partial CPU write aliases GPU-written texture; readback/merge required");
        im.gpu_written=false;provenance_write(im,__LINE__);im.uploaded=false;im.checked_submission=UINT64_MAX;
    });
}

bool resolving_pending=false;
// 仅后续写回完整覆盖、池明确不允许CPU读且无图像alias时跳过；半开范围溢出不成立。
// Skip only when a later writeback fully covers a known CPU_NO_ACCESS range with no image alias; overflowing half-open ranges never qualify.
bool writeback_superseded(size_t i){
    static_assert(NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS==1,"Writeback pool flag contract changed");
    return xbg_wb_superseded(pending_writebacks,i,wb_skip && nvn_pool_flags && !ab_off("wbskip"),
        [](uint64_t address,uint64_t bytes){return nvn_pool_flags(reinterpret_cast<const void*>(uintptr_t(address)),size_t(bytes));},
        [](uint64_t address,uint64_t bytes){bool image=false;for_images_overlapping(address,bytes,[&](Image &){image=true;});return image;});
}
// Writebacks of a slot whose fence has signalled can be applied without waiting.
void resolve_slot_writebacks(const void *slot){
    if(pending_writebacks.empty())return;resolving_pending=true;
    std::vector<PendingWriteback> keep;
    for(size_t i=0;i<pending_writebacks.size();i++){auto &w=pending_writebacks[i];if(w.slot!=slot){keep.push_back(w);continue;}
        if(writeback_superseded(i)){++wb_skipped;wb_skipped_bytes+=w.bytes;continue;}
        writeback_bytes+=w.bytes;w.buffer.invalidate();invalidate_cpu_range(w.addr,w.bytes);memcpy(reinterpret_cast<void*>(w.addr),w.buffer.mapped,w.bytes);}
    pending_writebacks.swap(keep);resolving_pending=false;
}
void write_report(const PendingReport &r){
    uint64_t samples=0;auto e=epochs.find(r.epoch);if(e!=epochs.end()){e->second.advance_known();samples=e->second.prefix(r.count);}
    uint64_t value=r.ts_value;if(timestamp_bits<64)value&=(uint64_t(1)<<timestamp_bits)-1;
    if(r.type==NVN_COUNTER_TYPE_SAMPLES_PASSED){++samples_reports;samples_reported+=samples;}
    NVNcounterData result{};result.counter=r.type==NVN_COUNTER_TYPE_SAMPLES_PASSED?samples:0;result.timestamp=uint64_t(double(value)*properties.limits.timestampPeriod*12.0/625.0);
    snapshot_invalidate(r.addr,sizeof(result));evguard::EngineRead er(reinterpret_cast<void*>(r.addr),sizeof(result));memcpy(reinterpret_cast<void*>(r.addr),&result,sizeof(result));
}
// Reports whose timestamp and samples are all known are written in order.
void resolve_known_reports(){
    size_t done=0;
    for(;done<pending_reports.size();done++){const auto &r=pending_reports[done];if(!r.ts_known)break;auto e=epochs.find(r.epoch);bool known=true;if(e!=epochs.end()){e->second.advance_known();known=e->second.known_upto>=std::min<uint64_t>(r.count,e->second.size());}if(!known)break;write_report(r);}
    if(done)pending_reports.erase(pending_reports.begin(),pending_reports.begin()+done);
    // Fold known queries into the running sum up to the earliest count still needed by a pending report, then drop them.
    for(auto &e:epochs){uint64_t limit=e.second.known_upto;for(auto &r:pending_reports)if(r.epoch==e.first)limit=std::min<uint64_t>(limit,r.count);e.second.prefix(limit);e.second.trim();}
    // A report only ever names the epoch current when it was recorded, so an older epoch that no pending report names is
    // dead (retire skips query refs to erased epochs). With deferred events a report is always pending, so erasing only
    // when none was pending let the map grow by every counter reset (~13 per D2R frame).
    for(auto it=epochs.begin();it!=epochs.end();){bool used=it->first==counter_epoch;for(auto &r:pending_reports)if(r.epoch==it->first){used=true;break;}
        if(used)++it;else it=epochs.erase(it);}
}
void resolve_pending(){
    if(resolving_pending || (pending_writebacks.empty() && pending_reports.empty()))return;
    resolving_pending=true;
    gpu_sync(pending_writebacks.empty()?perf.sync_query:perf.sync_writeback);
    auto writebacks=std::move(pending_writebacks);pending_writebacks.clear();
    for(auto &w:writebacks){w.buffer.invalidate();invalidate_cpu_range(w.addr,w.bytes);memcpy(reinterpret_cast<void*>(w.addr),w.buffer.mapped,w.bytes);}
    for(auto &r:pending_reports)write_report(r);
    pending_reports.clear();
    for(auto it=epochs.begin();it!=epochs.end();)if(it->first!=counter_epoch)it=epochs.erase(it);else ++it;
    apply_ready_events();
    resolving_pending=false;
}
// 先完成待写回/报告，再取得命令缓冲、描述符、图像或槽引用；同步会提交并退役槽。
// Resolve writebacks/reports before acquiring command, descriptor, image or slot references; synchronization submits and retires slots.
// 保守同步可能减少异步重叠；范围化优化须另验整个调用链。
// Conservative synchronization may reduce asynchronous overlap; range-based optimization needs whole-call-chain validation.
// 默认开启，与 main 相同；宿主可设 DM_GFX_READ_PREFLIGHT=0 关闭（标记 gfx-read-preflight 为 1 / 0 时以标记为准）。
// Android 关闭：延后事件下几乎总有报告待写，每个 draw 前同步会让 GPU 与翻译串行；Redmi K50 Ultra 营地与 Den of Evil
// 战斗的记录中，没有一次 CPU 读碰到待写回或待写报告（NVN PERF 的 why=a0）。
// On by default, as on main; a host sets DM_GFX_READ_PREFLIGHT=0 to turn it off (a gfx-read-preflight marker, 1 / 0,
// wins). Android does: with deferred events a report is nearly always pending, so a sync before every draw would serialize
// the GPU with translation, and in the Redmi K50 Ultra camp and Den of Evil fight logs no CPU read ever met a pending
// writeback or report (why=a0 in NVN PERF).
bool read_preflight=true;
void preflight_cpu_reads(){if(read_preflight)resolve_pending();}
void cpu_access(uint64_t address,uint64_t bytes,unsigned why){
    if(!bytes)return;
    need(xbg_wb_range_valid(address,bytes),"CPU access range overflow");
    if(resolving_pending)return;
    for(auto &w:pending_writebacks)if(xbg_wb_overlaps(address,bytes,w.addr,w.bytes)){++perf.why_access;if(why<12)++perf.access_by[why];resolve_pending();return;}
    // GPU reads of counter memory (snapshots) see the last resolved report, i.e.
    // one frame of latency for occlusion-driven effects instead of a full stall.
    if(why==1 || why==2)return;
    for(auto &r:pending_reports)if(xbg_wb_overlaps(address,bytes,r.addr,sizeof(NVNcounterData))){++perf.why_access;++perf.access_by[7];resolve_pending();return;}
}
// Guest fence completion. Work recorded for submitted guest command buffers is
// complete when every frame slot is retired; deferred results are then resolved.
void complete_all(){SiteScope site(SITE_COMPLETE);resolve_pending();submit_slots(true);publish_completed(submitted);}
// Block until guest fence `fence` is complete, retiring only the slots needed.
void wait_fence(uint64_t fence){
    for(int i=0;i<2 && fence>completed;i++){Slot &older=slots[slot_index].pending?slots[slot_index]:slots[slot_index^1];if(!older.pending || older.covers<fence)break;retire_slot(older);}
    if(fence>completed)complete_all();
}
bool gpu_poll_complete(){
    SiteScope site(SITE_POLL);
    Slot &cur=slots[slot_index];if(cur.recording && cur.ops)submit_slots(false);
    for(auto &s:slots)if(s.pending){if(vkGetFenceStatus(cfg.device,s.fence)!=VK_SUCCESS)return false;}
    ++perf.why_poll;complete_all(); // everything signaled: retiring/resolving does not block
    return true;
}
// Vulkan buffer/image transfers name exactly one depth/stencil aspect. Keep
// both packed NVN channels, with independent tightly packed planes and copies.
void append_depth_planes(std::vector<uint8_t> &data,std::vector<VkBufferImageCopy> &copies,
    uint32_t format,uint32_t w,uint32_t h,uint32_t slices,const void *packed,size_t packed_bytes,
    size_t row,size_t image,const VkBufferImageCopy &region){
    xbg_depth_format f;need(xbg_depth_format_info(format,&f)==0,"Unknown NVN depth transfer format");
    bool widen=format==0x32 && depth16_as_d24;   // image texels are X8_D24 (4 bytes), guest texels D16 (2 bytes)
    uint64_t pixels=uint64_t(w)*h*slices;need(pixels<=256ull*1024*1024/5,"Depth staging extent exceeds bound");
    size_t begin=(data.size()+15)&~size_t(15),depth_bytes=size_t(pixels)*(widen?4:f.depth_bytes);
    size_t stencil_begin=(begin+depth_bytes+3)&~size_t(3),stencil_bytes=size_t(pixels)*f.stencil_bytes;
    need(stencil_begin+stencil_bytes<=256ull*1024*1024,"Depth staging allocation exceeds bound");data.resize(stencil_begin+stencil_bytes);
    std::vector<uint8_t> d16(packed && widen?size_t(pixels)*2:0);
    if(packed)need(xbg_depth_extract(format,w,h,slices,packed,packed_bytes,row,image,
        f.depth_bytes?(widen?d16.data():data.data()+begin):nullptr,widen?d16.size():depth_bytes,f.stencil_bytes?data.data()+stencil_begin:nullptr,stencil_bytes)==0,"Invalid packed NVN depth upload extent");
    for(size_t i=0;i<d16.size()/2;i++){uint16_t k;memcpy(&k,&d16[i*2],2);uint32_t v=d16_to_d24(k);memcpy(data.data()+begin+i*4,&v,4);}
    if(f.depth_bytes){auto r=region;r.bufferOffset=begin;r.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;copies.push_back(r);}
    if(f.stencil_bytes){auto r=region;r.bufferOffset=stencil_begin;r.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;copies.push_back(r);}
}

void forget_framebuffers(Image &im,std::vector<VkFramebuffer> *deferred){
    for(auto &k:im.fb_keys){auto f=persistent_framebuffers.find(k);if(f==persistent_framebuffers.end())continue;   // already gone with another image
        if(deferred)deferred->push_back(f->second);else vkDestroyFramebuffer(cfg.device,f->second,nullptr);persistent_framebuffers.erase(f);}
    im.fb_keys.clear();
}
// Direct-mapped front for images.find (~12k get_image calls per D2R camp frame): erasing from or clearing images bumps
// images_epoch, so a front entry never outlives its Image.
struct ImgFront{uint64_t serial=0,epoch=0;Image *im=nullptr;};ImgFront img_front[1024];uint64_t images_epoch=1;
// The Image currently registered for a serial, or null (the same front as get_image).
Image *find_image(uint64_t serial){ImgFront &f=img_front[(serial*0x9E3779B97F4A7C15ull)>>54];if(f.epoch==images_epoch && f.serial==serial)return f.im;
    auto it=images.find(serial);if(it==images.end())return nullptr;f.serial=serial;f.epoch=images_epoch;f.im=it->second.get();return f.im;}
Image &get_image_slow(const tex_t *t,bool upload);
Image &get_image_read(const tex_t *t,bool upload);
// The common case, about 10k times a D2R camp frame: the image is in the front cache, and either nothing is uploaded or it was
// checked in this submission already, so none of get_image_slow's checks would run. Inline, without the slow path's large
// frame and timing scope. Provenance logging takes the slow path.
inline Image &get_image(const tex_t *t,bool upload){
    if(read_preflight)return get_image_read(t,upload);
    if(img_fast && t && t->serial && t->magic==MAGIC('T','E','X',0) && !provenance_file){
        const ImgFront &front=img_front[(t->serial*0x9E3779B97F4A7C15ull)>>54];
        if(front.epoch==images_epoch && front.serial==t->serial){Image &im=*front.im;
            if(!upload || (im.checked_submission==stats.submissions && (im.uploaded || im.gpu_written))){im.slot_use=slot_serial;return im;}}}
    return get_image_slow(t,upload);
}
Image &get_image_slow(const tex_t *t,bool upload){CPU_SCOPE(Image);
    need(t && t->serial && t->magic==MAGIC('T','E','X',0),"Invalid NVN texture");
    ImgFront &front=img_front[(t->serial*0x9E3779B97F4A7C15ull)>>54];Image *hit=front.epoch==images_epoch && front.serial==t->serial?front.im:nullptr;
    auto it=hit?images.end():images.find(t->serial);
    if(!hit){
    if(it==images.end() && t->mem){
        // Reinitialized or copied NVN objects with the same storage/layout share
        // GPU contents. Serial-only caches would incorrectly re-upload stale CPU
        // bytes when the new object aliases an already-rendered attachment.
        Image *alias=nullptr;for_images_overlapping(uint64_t(uintptr_t(t->mem)),1,[&](Image &c){if(!alias && c.source.mem==t->mem && !memcmp(&c.source.d,&t->d,sizeof(t->d)))alias=&c;});
        if(alias)for(auto &candidate:images)if(candidate.second.get()==alias){it=images.emplace(t->serial,candidate.second).first;image_index_dirty=true;++memo_check_gen;++memo_bump[3];provenance_image("ALIAS",t,*it->second);break;}
    }
    if(it==images.end()){
        need(t->d.samples<=1 || t->d.samples==2 || t->d.samples==4 || t->d.samples==8,"Unsupported texture sample count");
        need(t->d.target==0 || t->d.target==1 || t->d.target==2 || t->d.target==3 || t->d.target==4 || t->d.target==5 || t->d.target==6 || t->d.target==7 || t->d.target==8 || t->d.target==9,"Unsupported texture target (1D/3D/buffer handled separately)");
        auto p=std::make_shared<Image>();p->source=*t;p->provenance_epoch=provenance.epoch((t->mem && t->off>=0 && uint64_t(t->off)<=uint64_t(t->mem)?uint64_t(t->mem)-uint64_t(t->off):0));p->width=t->d.width;p->height=(t->d.target==0 || t->d.target==3)?1:t->d.height;p->depth=t->d.target==2?t->d.depth:1;p->levels=t->d.levels;p->layers=t->L.layers;p->aspects=aspect(t->d.format);p->samples=VkSampleCountFlagBits(std::max(1,t->d.samples));
        need(p->width && p->height && p->levels && p->levels<=16 && p->layers,"Invalid texture dimensions");
        // 设备不能线性过滤 R16G16_UNORM（Mali-G925 实测：optimalTilingFeatures 缺 FILTER_LINEAR）时，BC4/BC5 解码成 8 位通道，
        // 否则带线性过滤的采样器会拿不到可过滤格式。 | When the device cannot linearly filter R16G16_UNORM (Mali-G925: no FILTER_LINEAR),
        // BC4/BC5 decode to 8-bit channels so linear-filtered samplers stay valid.
        {static int bc8=-1;if(bc8<0){VkFormatProperties q;vkGetPhysicalDeviceFormatProperties(cfg.physical,VK_FORMAT_R16G16_UNORM,&q);bc8=!(q.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);if(bc8)xbg_logf("NVN caps bc 8bit: R16G16_UNORM optimalFeatures=%#x lacks SAMPLED_IMAGE_FILTER_LINEAR",q.optimalTilingFeatures);}if(bc8)xbg_bc_8bit_channels=1;}
        xbg_bc_format info;p->decoded=xbg_bc_format_info(t->d.format,&info)==0;
        if(p->decoded && native_bc && p->samples==VK_SAMPLE_COUNT_1_BIT){const VkFormatFeatureFlags need_bc=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            VkFormatProperties np;vkGetPhysicalDeviceFormatProperties(cfg.physical,format(t->d.format),&np);if((np.optimalTilingFeatures&need_bc)==need_bc)p->decoded=false;}
        if(xbg_bc_format_info(t->d.format,&info)==0)++(p->decoded?decoded_bc_images:native_bc_images);
        if(p->decoded){p->bpp=info.bytes_per_pixel;p->fmt=decoded_format(info.pixels);need(p->samples==VK_SAMPLE_COUNT_1_BIT,"BC texture cannot be multisampled");}
        else {p->fmt=format(t->d.format);p->bpp=t->L.bpb;if(t->d.format==0x32 && depth16_as_d24)p->fmt=VK_FORMAT_X8_D24_UNORM_PACK32;}
        VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(cfg.physical,p->fmt,&fp);
        VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
        if((t->d.flags&0x4) && (fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (p->samples==VK_SAMPLE_COUNT_1_BIT || cfg.enabled_core.shaderStorageImageMultisample))usage|=VK_IMAGE_USAGE_STORAGE_BIT;
        if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=t->d.target==2?VK_IMAGE_TYPE_3D:t->d.target==0||t->d.target==3?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D;ci.format=p->fmt;ci.extent={p->width,p->height,p->depth};ci.mipLevels=p->levels;ci.arrayLayers=p->layers;ci.samples=p->samples;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        p->usage=usage;if(p->aspects==VK_IMAGE_ASPECT_COLOR_BIT && !no_mutable_images)ci.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;p->mutable_format=(ci.flags&VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)!=0;
        if(t->d.target==8 || t->d.target==9)ci.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VkImageFormatProperties supported;VK(vkGetPhysicalDeviceImageFormatProperties(cfg.physical,ci.format,ci.imageType,ci.tiling,ci.usage,ci.flags,&supported));need(supported.sampleCounts&p->samples,"Texture sample count unsupported for actual format/usage");need(p->samples==VK_SAMPLE_COUNT_1_BIT || p->levels==1,"Multisample textures cannot have mipmaps");
        VK(vkCreateImage(cfg.device,&ci,nullptr,&p->image));p->create=ci;VkMemoryRequirements r;vkGetImageMemoryRequirements(cfg.device,p->image,&r);
        p->mem_bytes=int64_t(r.size);image_bytes_live+=p->mem_bytes;if(xbg_bc_format_info(t->d.format,&info)==0){p->bc_bytes=p->mem_bytes;p->bc_data=int64_t(t->L.size);bc_image_bytes_live+=p->bc_bytes;bc_data_bytes_live+=p->bc_data;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VK(vkAllocateMemory(cfg.device,&ai,nullptr,&p->memory));VK(vkBindImageMemory(cfg.device,p->image,p->memory,0));
        it=images.emplace(t->serial,std::move(p)).first;image_index_dirty=true;++memo_check_gen;++memo_bump[4];provenance_image("CREATE",t,*it->second);
    }
    hit=it->second.get();front.serial=t->serial;front.epoch=images_epoch;front.im=hit;}
    Image &im=*hit;
    // NVN CPU-coherent pools may be written without FlushMappedRange. Detect
    // such writes once per submit; command CPU writes also invalidate below.
    if(upload && im.uploaded && !im.gpu_written && im.checked_submission!=stats.submissions){
        if(im.dirty_stamp && !ab_off("texdirty")){   // tracked: hash only after a guest write to its pages since the stamp
            uint64_t now=nvn_dirty_now();
            if(now!=im.dirty_seen && nvn_dirty_since(t->mem,t->L.size,im.dirty_stamp)){
                nvn_dirty_protect(t->mem,t->L.size);uint64_t stamp=nvn_dirty_now();++tex_dirty_checks;   // protect and stamp before reading
                if(hash_memory(t->mem,t->L.size)!=im.cpu_hash){im.uploaded=false;im.stable_checks=0;++tex_dirty_changed;}else im.dirty_stamp=stamp;
                now=stamp;}
            im.dirty_seen=now;}
        // Textures unchanged for 4 checks are re-verified every 16th submission (staggered). With tex_dirty the 4th unchanged
        // check write-protects a tracked texture instead (protect and stamp before the hash), and the tracker answers from then
        // on: protecting at upload faulted ~40k times per level load on the Redmi, as the loader went on writing those pages.
        else if(im.stable_checks<4 || ((stats.submissions+t->serial)&15)==0){
            const bool track=tex_dirty && im.stable_checks==3 && !ab_off("texdirty") && nvn_dirty_tracked && nvn_dirty_protect && nvn_dirty_now && nvn_dirty_since && nvn_dirty_tracked(t->mem,t->L.size);
            uint64_t stamp=0;if(track){nvn_dirty_protect(t->mem,t->L.size);stamp=nvn_dirty_now();}
            uint64_t hash=hash_memory(t->mem,t->L.size);if(hash!=im.cpu_hash){im.uploaded=false;im.stable_checks=0;}else{++im.stable_checks;if(track){im.dirty_stamp=im.dirty_seen=stamp;++tex_dirty_tracked;}}}
        im.checked_submission=stats.submissions;}
    // Diagnostic: CPU writes into textures whose contents the GPU produced (copies) are invisible to us.
    if(upload && im.gpu_written && im.decoded && t->mem && ((stats.submissions+t->serial)&63)==0 && im.checked_submission!=stats.submissions){im.checked_submission=stats.submissions;uint64_t hash=hash_memory(t->mem,t->L.size);if(im.cpu_hash && hash!=im.cpu_hash){static unsigned n=0;if(n++<40)xbg_logf("NVN CPUGPU texture changed by CPU after GPU write serial=%llu fmt=%#x %ux%u layers=%u levels=%u target=%d",(unsigned long long)t->serial,t->d.format,im.width,im.height,im.layers,im.levels,t->d.target);}im.cpu_hash=hash;}
    if(upload && !im.uploaded && !im.gpu_written){
        const uint64_t up_t0=timing_clock();const bool reupload=im.cpu_hash!=0;
        im.dirty_stamp=0;   // a new upload starts on the hash checks again
        need(im.samples==VK_SAMPLE_COUNT_1_BIT,"Cannot upload uninitialized multisample image from CPU storage");
        need(t->mem && t->pool && t->off>=0 && uint64_t(t->off)<=t->pool->size && t->L.size<=t->pool->size-uint64_t(t->off),"Texture storage outside memory pool");
        if(im.aspects!=VK_IMAGE_ASPECT_COLOR_BIT)xbg_logf("NVN depth initial upload serial=%llu format=%#x vkFormat=%u target=%d extent=%ux%ux%u levels=%u layers=%u source=%p bytes=%llu",(unsigned long long)t->serial,t->d.format,unsigned(im.fmt),t->d.target,im.width,im.height,im.depth,im.levels,im.layers,t->mem,(unsigned long long)t->L.size);
        std::vector<uint8_t> linear;std::vector<VkBufferImageCopy> copies;
        for(uint32_t layer=0;layer<im.layers;layer++)for(uint32_t level=0;level<im.levels;level++){
            uint32_t w=std::max(1u,im.width>>level),h=std::max(1u,im.height>>level),depth=std::max(1u,im.depth>>level);
            if(im.aspects!=VK_IMAGE_ASPECT_COLOR_BIT){
                size_t bytes=size_t(w)*h*depth*im.bpp;need(bytes<=256ull*1024*1024,"Packed depth image too large");std::vector<uint8_t> packed(bytes);
                if(t->d.flags&0x10){need(!layer && !level && depth==1,"Linear depth mip/layer/volume layout unsupported");size_t row=t->d.stride?t->d.stride:(size_t(w)*im.bpp+31)&~size_t(31);need(row>=size_t(w)*im.bpp && row*h<=t->L.size,"Linear depth storage extent invalid");for(uint32_t y=0;y<h;y++)memcpy(packed.data()+size_t(y)*w*im.bpp,t->mem+y*row,size_t(w)*im.bpp);}
                else need(nvn_deswizzle(&t->d,level,layer,t->mem,packed.data(),size_t(w)*im.bpp)==0,"Depth image deswizzle failed");
                VkBufferImageCopy region{};region.imageSubresource={0,level,layer,1};region.imageExtent={w,h,depth};
                append_depth_planes(linear,copies,t->d.format,w,h,depth,packed.data(),packed.size(),0,0,region);continue;
            }
            size_t begin=(linear.size()+15)&~size_t(15);
            int cbw=1,cbh=1;bool native_blocks=!im.decoded && nvn_fmt_bpb(t->d.format,&cbw,&cbh) && (cbw>1 || cbh>1);
            if(native_blocks){ // ASTC etc.: copy whole blocks, rows of blocks
                uint32_t bx=(w+cbw-1)/cbw,by=(h+cbh-1)/cbh;linear.resize(begin+size_t(bx)*by*depth*im.bpp);
                need(nvn_deswizzle(&t->d,level,layer,t->mem,linear.data()+begin,size_t(bx)*im.bpp)==0,"Compressed texture deswizzle failed");
                VkBufferImageCopy c{};c.bufferOffset=begin;c.imageSubresource={im.aspects,level,layer,1};c.imageExtent={w,h,depth};copies.push_back(c);continue;}
            linear.resize(begin+size_t(w)*h*depth*im.bpp);
            if(im.decoded){
                int bw,bh,block_bytes=nvn_fmt_bpb(t->d.format,&bw,&bh);uint32_t blocks_x=(w+bw-1)/bw,blocks_y=(h+bh-1)/bh;
                std::vector<uint8_t> blocks(size_t(blocks_x)*blocks_y*depth*block_bytes);
                need(nvn_deswizzle(&t->d,level,layer,t->mem,blocks.data(),size_t(blocks_x)*block_bytes)==0,"BC image deswizzle failed");
                need(xbg_decode_bc_linear(t->d.format,w,h,depth,blocks.data(),blocks.size(),0,0,linear.data()+begin,size_t(w)*h*depth*im.bpp)==0,"BC image decode failed");
            }else if(t->d.flags&0x10){need(!layer && !level && depth==1,"Linear mip/layer/volume texture not implemented");size_t pitch=t->d.stride?t->d.stride:(w*im.bpp+31)&~size_t(31);for(uint32_t y=0;y<h;y++)memcpy(linear.data()+begin+y*w*im.bpp,t->mem+y*pitch,w*im.bpp);}
            else need(nvn_deswizzle(&t->d,level,layer,t->mem,linear.data()+begin,size_t(w)*im.bpp)==0,"Texture deswizzle failed");
            VkBufferImageCopy c{};c.bufferOffset=begin;c.imageSubresource={im.aspects,level,layer,1};c.imageExtent={w,h,depth};copies.push_back(c);
        }
        Buffer staging=stream_buffer(linear.size(),linear.data(),linear.size(),false);for(auto &copy:copies)copy.bufferOffset+=staging.offset;
        // gfx-upload-prologue：这次提交里还没有命令用过这张图，它的新内容可以排在整个提交之前，前面没有读到旧内容的命令。
        // gfx-upload-prologue: no command of this submission used this image yet, so its new contents can go ahead of the whole
        // submission; no earlier command read the old contents.
        if(upload_prologue && im.slot_use!=slot_serial && !ab_off("uppro")){Slot &s=recording_slot();
            s.pro_images.push_back({im.image,im.layout,im.aspects,im.levels,im.layers,staging.buffer,copies});im.layout=VK_IMAGE_LAYOUT_GENERAL;++uploads_prologue;}
        else{++uploads_inpass;if(open_pass.cmd)++uploads_broke_pass;
            Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(c.command,staging.buffer,im.image,im.layout,copies.size(),copies.data());im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();}
        im.uploaded=true;provenance_write(im,__LINE__);im.cpu_hash=hash_memory(t->mem,t->L.size);im.checked_submission=stats.submissions;
        if(reupload)++hud::reuploads;else ++hud::uploads;hud::upload_bytes+=linear.size();hud::upload_ns+=timing_clock()-up_t0;
    }
    provenance_image(upload?"GET_UPLOAD":"GET",t,im);im.slot_use=slot_serial;return im;
}
// read_preflight: main's order (cc91eb1). Copy the descriptor, resolve CPU reads of its storage, then look the image up.
Image &get_image_read(const tex_t *t,bool upload){
    need(t && t->serial && t->magic==MAGIC('T','E','X',0),"Invalid NVN texture");
    need(!t->mem || xbg_wb_range_valid(uint64_t(uintptr_t(t->mem)),t->L.size),"Texture CPU read range overflow or empty");
    bool read=upload || images.find(t->serial)==images.end();
    return xbg_image_after_cpu_access(*t,read,
        [](uint64_t address,uint64_t bytes){cpu_access(address,bytes,9);},
        [upload](const tex_t &stable)->Image &{return get_image_slow(&stable,upload);});
}
const RegisteredTexture &null_texture(const xbg_shader_binding &b){
    // Original host evidence: main+0x1746820 explicitly binds descriptor 0xFFFFF
    // for unused slots; descriptor 0 is the game's null handle. Only those
    // defined null cases receive zero resources. Missing in-pool IDs still fail.
    need(b.type!=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b.type!=VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,"Null writable image descriptor needs robustness2 nullDescriptor support");
    uint32_t key=b.flags&0xffffff00u;auto it=null_textures.find(key);if(it!=null_textures.end())return it->second;
    unsigned dim=(b.flags&(1u<<31))?(b.flags>>8)&7:1;bool array=b.flags&(1u<<12);unsigned scalar=(b.flags>>16)&3;
    need(!(b.flags&(1u<<13)),"Null multisample texture descriptor unsupported");
    RegisteredTexture r;tex_t &t=r.texture;t.magic=MAGIC('T','E','X',0);t.serial=nvn_new_serial();t.d.width=t.d.height=t.d.depth=t.d.levels=1;
    t.d.format=b.flags&(1u<<14)?0x34:scalar==1?0x2f:scalar==2?0x30:dim==5?0x2e:0x25;
    t.d.target=dim==0?(array?3:0):dim==1?(array?4:1):dim==2?2:dim==3?(array?9:8):dim==5?10:-1;need(t.d.target>=0,"Unknown null texture dimension");
    nvn_tex_layout(&t.d,&t.L);
    if(dim==5){static uint8_t zero[65536]{};t.mem=zero;t.L.size=sizeof(zero);}
    else{Image &im=get_image(&t,false);Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkImageSubresourceRange range{im.aspects,0,1,0,im.layers};if(im.aspects==VK_IMAGE_ASPECT_COLOR_BIT){VkClearColorValue zero{};vkCmdClearColorImage(c.command,im.image,im.layout,&zero,1,&range);}else{VkClearDepthStencilValue zero{};vkCmdClearDepthStencilImage(c.command,im.image,im.layout,&zero,1,&range);}im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();im.gpu_written=true,provenance_write(im,__LINE__);}
    xbg_logf("NVN defined null sampled resource: dim=%u array=%d scalar=%u",dim,array,scalar);return null_textures.emplace(key,r).first->second;
}

/* SHA-256 over the original NVN 0x880-byte control segment. */
std::string control_key(const void *data){char key[17];need(xbg_shader_control_key(data,key)==0,"Missing shader control segment");return key;}
struct Shader {std::string key;uint32_t stage;VkShaderModule module{};std::vector<xbg_shader_binding> bindings;std::vector<uint32_t> inputs,input_classes;std::vector<uint8_t> c1;
    std::set<uint32_t> storage_writes; // Vulkan bindings of storage buffers the SPIR-V may write
    bool discards=true; // fragment shader may kill/demote (conservative default)
    XbgRasterFacts raster; // position input (vertex), declared outputs and SampleMask (fragment): first_draw_covers
    Buffer c1_buffer{};bool c1_ready=false;
    ~Shader(){if(module)vkDestroyShaderModule(cfg.device,module,nullptr);}};
// Storage buffers written by a shader. Pointers rooted at a storage-buffer
// variable that reach OpStore/OpCopyMemory/atomics, function calls, phis or
// selects mark that binding written; anything unprovable marks all written.
// Bytes of each uniform block a shader can actually read (iOS c1/cbuf packing idea): Ryujinx declares
// constant buffers as struct{vec4 data[4096];}. When every access uses a constant element index, only
// (max index + 1) * 16 bytes need copying per draw; any other use keeps the full block.
std::map<uint32_t,uint32_t> spirv_uniform_extent(const std::vector<uint32_t> &w){
    std::map<uint32_t,uint32_t> binding,constant,stride,member_type,var_type,result;std::set<uint32_t> uniform_vars,full;std::map<uint32_t,uint32_t> max_index;
    std::map<uint32_t,uint32_t> pointer_pointee,struct_member0;
    for(size_t i=5;i<w.size();){uint32_t n=w[i]>>16,op=w[i]&65535;if(!n||i+n>w.size())return {};
        switch(op){
        case 71:if(n>=4 && w[i+2]==33)binding[w[i+1]]=w[i+3];if(n>=4 && w[i+2]==6)stride[w[i+1]]=w[i+3];break; // Binding / ArrayStride
        case 43:if(n>=4)constant[w[i+2]]=w[i+3];break; // OpConstant (32-bit)
        case 30:if(n>=3)struct_member0[w[i+1]]=w[i+2];break; // OpTypeStruct member 0
        case 32:if(n>=4)pointer_pointee[w[i+1]]=w[i+3];break;
        case 59:if(n>=4 && w[i+3]==2){uniform_vars.insert(w[i+2]);var_type[w[i+2]]=w[i+1];}break; // OpVariable Uniform
        default:break;
        }
        i+=n;
    }
    for(size_t i=5;i<w.size();){uint32_t n=w[i]>>16,op=w[i]&65535;
        if(op==59){i+=n;continue;}
        if(op==5||op==6||op==15||op==71||op==72||op==332||op==5632){i+=n;continue;} // names/entry-point interface/decorations reference without reading
        for(uint32_t k=1;k<n;k++){uint32_t id=w[i+k];if(!uniform_vars.count(id))continue;
            bool ok=(op==65||op==66) && k==3 && n>=6;
            if(ok){auto c=constant.find(w[i+5]);auto m=constant.find(w[i+4]);if(c==constant.end()||m==constant.end()||m->second!=0)ok=false;else max_index[id]=std::max(max_index[id],c->second);}
            if(!ok)full.insert(id);}
        i+=n;
    }
    for(uint32_t v:uniform_vars){auto b=binding.find(v);if(b==binding.end()||full.count(v))continue;
        auto pt=pointer_pointee.find(var_type[v]);if(pt==pointer_pointee.end())continue;auto m0=struct_member0.find(pt->second);if(m0==struct_member0.end())continue;
        auto st=stride.find(m0->second);if(st==stride.end()||st->second!=16)continue;
        auto mi=max_index.find(v);result[b->second]=mi==max_index.end()?16:(mi->second+1)*16;}
    return result;
}
std::set<uint32_t> spirv_storage_writes(const std::vector<uint32_t> &w,bool &unknown){
    std::map<uint32_t,uint32_t> binding,storage_class,root;std::set<uint32_t> written_vars;unknown=false;
    std::set<uint32_t> buffer_block_types;std::map<uint32_t,uint32_t> pointer_pointee;
    for(size_t i=5;i<w.size();){uint32_t n=w[i]>>16,op=w[i]&65535;if(!n||i+n>w.size()){unknown=true;break;}
        auto mark=[&](uint32_t ptr){auto r=root.find(ptr);if(r!=root.end())written_vars.insert(r->second);};
        switch(op){
        case 71:if(n>=4 && w[i+2]==33)binding[w[i+1]]=w[i+3];if(n>=3 && w[i+2]==3)buffer_block_types.insert(w[i+1]);break; // OpDecorate Binding/BufferBlock
        case 32:if(n>=4)pointer_pointee[w[i+1]]=w[i+3];break; // OpTypePointer
        case 59:if(n>=4){storage_class[w[i+2]]=w[i+3];root[w[i+2]]=w[i+2];}break; // OpVariable
        // Function parameters: call sites passing storage pointers are marked
        // at OpFunctionCall, so stores through parameters need no extra mark.
        case 55:if(n>=3)root[w[i+2]]=0xffffffffu;break;
        case 65:case 66:case 67:case 83:case 124:if(n>=4){auto r=root.find(w[i+3]);if(r!=root.end())root[w[i+2]]=r->second;}break;
        case 62:case 63:case 228:if(n>=2){if(root.count(w[i+1]))mark(w[i+1]);else unknown=true;}break; // OpStore/OpCopyMemory/OpAtomicStore
        case 57:for(uint32_t k=4;k<n;k++)mark(w[i+k]);break; // pointer arguments to calls
        case 169:case 245:for(uint32_t k=3;k<n;k++)mark(w[i+k]);break; // select/phi over pointers
        default:if(((op>=229 && op<=242) || op==6035 || op==5250) && n>=4){if(root.count(w[i+3]))mark(w[i+3]);else unknown=true;}break;
        }
        i+=n;
    }
    std::set<uint32_t> out;
    for(uint32_t v:written_vars){auto c=storage_class.find(v);if(c==storage_class.end())continue;if(c->second==12 || c->second==2){auto b=binding.find(v);if(b!=binding.end())out.insert(b->second);else unknown=true;}}
    return out;
}
std::map<std::string,std::unique_ptr<Shader>> shaders;
const uint32_t nvn_stage[6]={0,3,4,2,1,5};
struct State {
    p_set_rt rt{};Shader *shader[6]{};NVNbufferRange ub[6][16]{},sb[6][16]{},vb[16]{};
    uint64_t tex[6][128]{},samp[6][128]{},img[6][16]{};dpool_t texture_pool{},sampler_pool{};
    uint32_t attrib[16]{};vs_st stream[16]{};blend_st blend[8]{};color_st color{};chmask_st mask{0xffffffff};ms_st ms{};poly_st poly{0,1,2,0};ds_st ds{0,1,2,0,8,8,1,1,1,1,1,1};
    VkViewport viewport{0,0,1,1,0,1};VkRect2D scissor{{0,0},{UINT32_MAX,UINT32_MAX}};float blend_color[4]{},bias[3]{};uint32_t stencil_compare[2]{~0u,~0u},stencil_write[2]{~0u,~0u},stencil_ref[2]{};int64_t copy_stride=0,copy_image_stride=0;uint32_t patch_size=3;
    xbg_depth_bounds bounds{0,0,1};bool count_samples=false;uint64_t samples_passed=0;
} state;
// Pipeline memo (gfx-pipe-memo): every input the draw's VkGraphicsPipelineCreateInfo is built from. When a draw's inputs
// equal the previous draw's, it gets the previous draw's pipeline without serializing the create info into a key and
// looking it up (xbg_graphics_pipeline_key with its module-id lookups was ~6% of the game thread on Adreno 730). Raw
// copies of the state fields, so differing padding can only cost a miss. Off unless the host asks (DM_GFX_PIPE_MEMO, which
// the Android app sets to 1); marker gfx-pipe-memo = 1 / 0; A/B target pipememo.
struct PipeMemoKey{const void *shader[5];const void *layout,*pass_def;VkRenderPass pass;VkPipelineLayout pipeline_layout;int32_t prim;uint32_t samples,rt_n,colors,depth,depth_format;
    uint8_t fixed[offsetof(State,viewport)-offsetof(State,attrib)];               // attrib, stream, blend, color, mask, ms, poly, ds
    uint8_t stencil[offsetof(State,copy_stride)-offsetof(State,blend_color)];      // blend_color, bias, stencil compare / write / reference
    uint8_t tail[offsetof(State,count_samples)-offsetof(State,patch_size)];};      // patch_size, bounds
PipeMemoKey pipe_memo_key,pipe_memo_next;VkPipeline pipe_memo_pipeline=VK_NULL_HANDLE;bool pipe_memo=false;uint64_t pipe_memo_hits=0,pipe_memo_misses=0;
void pipe_memo_fill(PipeMemoKey &k,const void *layout,const void *pass_def,VkRenderPass pass,VkPipelineLayout pipeline_layout,int prim,VkSampleCountFlagBits samples,uint32_t colors){
    memset(&k,0,sizeof k);for(unsigned i=0;i<5;i++)k.shader[i]=state.shader[i];k.layout=layout;k.pass_def=pass_def;k.pass=pass;k.pipeline_layout=pipeline_layout;k.prim=prim;k.samples=uint32_t(samples);
    k.rt_n=uint32_t(state.rt.n);k.colors=colors;k.depth=state.rt.depth!=nullptr;k.depth_format=state.rt.depth?uint32_t(state.rt.depth->d.format):0;
    const uint8_t *b=reinterpret_cast<const uint8_t*>(&state);memcpy(k.fixed,b+offsetof(State,attrib),sizeof k.fixed);memcpy(k.stencil,b+offsetof(State,blend_color),sizeof k.stencil);memcpy(k.tail,b+offsetof(State,patch_size),sizeof k.tail);
}
void validate_capabilities(const std::vector<uint32_t> &w,uint32_t stage){
    need(w.size()>=5 && w[0]==0x07230203,"Invalid packaged SPIR-V");bool group_operations=false;VkSubgroupFeatureFlags groups=0;
    for(size_t i=5;i<w.size();){uint32_t count=w[i]>>16,op=w[i]&65535;need(count && i+count<=w.size(),"Invalid SPIR-V instruction bounds");if(op>=333 && op<=366)group_operations=true;
        if(op==17){uint32_t cap=w[i+1];bool ok=true;switch(cap){case 1:case 46:case 47:case 50:break;case 2:ok=cfg.enabled_core.geometryShader;break;case 3:ok=cfg.enabled_core.tessellationShader;break;case 10:ok=cfg.enabled_core.shaderFloat64;break;case 11:ok=cfg.enabled_core.shaderInt64;break;case 22:ok=cfg.enabled_core.shaderInt16;break;case 25:ok=cfg.enabled_core.shaderImageGatherExtended;break;case 32:ok=cfg.enabled_core.shaderClipDistance;break;case 33:ok=cfg.enabled_core.shaderCullDistance;break;case 35:ok=cfg.enabled_core.sampleRateShading;break;case 49:ok=cfg.enabled_core.shaderStorageImageExtendedFormats;break;case 55:ok=cfg.enabled_core.shaderStorageImageReadWithoutFormat;break;case 56:ok=cfg.enabled_core.shaderStorageImageWriteWithoutFormat;break;case 61:groups|=VK_SUBGROUP_FEATURE_BASIC_BIT;break;case 62:groups|=VK_SUBGROUP_FEATURE_VOTE_BIT;break;case 63:groups|=VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;break;case 64:groups|=VK_SUBGROUP_FEATURE_BALLOT_BIT;break;case 65:groups|=VK_SUBGROUP_FEATURE_SHUFFLE_BIT;break;case 66:groups|=VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;break;case 4427:ok=cfg.enabled_shader_draw_parameters;break;default:xbg_fatalf("Shader stage %u has unhandled capability %u",stage,cap);}if(!ok)xbg_fatalf("Shader stage %u requires disabled capability %u",stage,cap);}
        if(op==10)xbg_fatalf("Shader stage %u declares extension requiring explicit backend support",stage);i+=count;
    }
    need((subgroup.supportedOperations&groups)==groups,"Required subgroup operations unavailable");if(group_operations)need(subgroup.supportedStages&(1u<<stage),"Actual subgroup operation unsupported in shader stage");
}
// Fragment-shader quad-neighbour shuffles rewritten to fine derivatives (xbg_spirv_quad.cpp): on Adreno the shuffles read
// garbage in helper invocations. The conservative correction skips bit patterns and unproved control/index flows; the
// translator's SHFL helper calls are rewritten at the call site when they are proven quad swaps of float values in uniform
// control flow (D2R's form: 271 of its 278 calls). Both module
// loaders apply it, so the warm-up builds the same modules the game binds. Off unless the host asks
// (DM_GFX_QUAD_DERIVATIVES, which the Android app sets to 1; marker gfx-quad-derivatives = 1 / 0).
bool quad_derivatives=false;std::atomic<uint64_t> quad_rewrites{0};
void quad_rewrite(std::vector<uint32_t> &words,uint32_t stage,const char *key,const char *loader){
    if(!quad_derivatives || stage!=4)return;
    std::vector<uint32_t> out;xbg_spirv_quad_stats counts;
    size_t k=xbg_spirv_quad_derivatives(words.data(),words.size(),out,&counts);
    if(k){words.swap(out);quad_rewrites.fetch_add(k,std::memory_order_relaxed);}
    if(counts.candidates || counts.calls){
        char line[768];int at=snprintf(line,sizeof(line),"NVN quad shader=%s loader=%s candidates=%zu rewritten=%zu guarded_nonfinite=%zu calls=%zu calls_rewritten=%zu",key,loader,counts.candidates,counts.rewritten,counts.guarded_nonfinite,counts.calls,counts.calls_rewritten);
        for(size_t r=0;r<size_t(xbg_quad_skip::count) && at<int(sizeof(line))-64;++r)if(counts.skipped[r])at+=snprintf(line+at,sizeof(line)-at," skip_%s=%zu",xbg_spirv_quad_skip_name(xbg_quad_skip(r)),counts.skipped[r]);
        for(size_t r=0;r<size_t(xbg_quad_call_skip::count) && at<int(sizeof(line))-64;++r)if(counts.call_skipped[r])at+=snprintf(line+at,sizeof(line)-at," call_skip_%s=%zu",xbg_spirv_quad_call_skip_name(xbg_quad_call_skip(r)),counts.call_skipped[r]);
        xbg_logf("%s",line);
    }
}
// Compute modules translated for 32-lane warps, on wider subgroups (xbg_spirv_warp.cpp): every 32 lanes act as their own
// warp, as the Ryujinx translator emits for wider hosts. On Adreno (64-lane subgroups) the clutter, grass, fluid and PPR
// shaders' warp-aggregated appends otherwise lose lanes 32-63 and overwrite entries. Both module loaders apply it. Off unless
// the host asks (DM_GFX_WARP32, which the Android app sets to 1; marker gfx-warp32 = 1 / 0), and only where subgroups are
// wider than 32 lanes.
bool warp32=false;std::atomic<uint64_t> warp_rewrites{0};
void warp_rewrite(std::vector<uint32_t> &words,uint32_t stage,const char *key,const char *loader){
    if(!warp32 || stage!=5 || subgroup.subgroupSize<=32)return;
    std::vector<uint32_t> out;size_t k=xbg_spirv_warp32(words.data(),words.size(),subgroup.subgroupSize,out);
    if(k){
        // A vote-only module can acquire Ballot capability during rewriting; check the transformed requirements too.
        validate_capabilities(out,stage);words.swap(out);warp_rewrites.fetch_add(k,std::memory_order_relaxed);
        xbg_logf("NVN warp32 shader=%s loader=%s subgroupSize=%u rewrites=%zu",key,loader,subgroup.subgroupSize,k);
    }
}
// Two exact clean-ups of the translator's output, after the rewrites above (they match its form), in both module loaders. Each
// is off unless the host asks; the Android app sets both to 1, and a marker file (1 / 0) still wins.
// - Constant-buffer component fold (xbg_spirv_ldc.cpp; DM_GFX_LDC_FOLD, marker gfx-ldc-fold): an LDC with a computed address
//   is addressed by 32-bit word, so a skinning shader selects each bone-matrix component dynamically, although bone * 48 makes
//   every component a constant.
// - Single-store temporaries forwarded (xbg_spirv_temps.cpp; DM_GFX_FORWARD_TEMPS, marker gfx-forward-temps): every value
//   sits in its own Function variable, stored once and loaded in the same block. Forwarded after the fold, the folded
//   constants reach the access chains directly.
// Redmi K50 Ultra camp at scale 0.75 with both: GPU 15.70 -> 14.34 ms a frame; the shadow map 1,498 -> 711 us, the main pass
// 6,019 -> 5,435 us, the depth pre-pass 1,356 -> 1,059 us. Either one alone changed nothing measurable.
bool ldc_fold=false,forward_temps=false;std::atomic<uint64_t> ldc_folds{0},temps_forwarded{0};
// Maxwell 的浮点 min / max 与饱和（FMNMX、.SAT）遇 NaN 取另一个操作数（饱和得 0）；翻译器出的是 GLSL.std.450 的 FMin / FMax /
// FClamp，有 NaN 操作数时结果 Vulkan 不作规定，驱动可以把 NaN 传下去，写进 UNORM 目标就成了黑块。DM_GFX_NAN_MINMAX（标记
// gfx-nan-minmax）把它们原位换成 NMin / NMax / NClamp：操作数不变，非 NaN 时结果相同，遇 NaN 时与 Maxwell 一致。
// Maxwell's float min / max and saturation (FMNMX, .SAT) take the other operand when one is NaN (saturation gives 0); the
// translator emits GLSL.std.450 FMin / FMax / FClamp, whose result with a NaN operand Vulkan leaves undefined, so a driver may
// pass the NaN on, and written to a UNORM target it comes out black. DM_GFX_NAN_MINMAX (marker gfx-nan-minmax) swaps them in
// place for NMin / NMax / NClamp: the same operands, the same result for values that are not NaN, and Maxwell's result for NaN.
bool nan_minmax=false;std::atomic<uint64_t> nan_minmax_swaps{0};
size_t nan_minmax_swap(std::vector<uint32_t> &w){
    uint32_t glsl=0;size_t k=0;
    for(size_t i=5;i<w.size();){
        uint32_t op=w[i]&0xFFFFu,n=w[i]>>16;if(!n || i+n>w.size())break;
        if(op==11 && n>=5 && !memcmp(&w[i+2],"GLSL.std.450",13))glsl=w[i+1];
        else if(op==12 && n>=5 && glsl && w[i+3]==glsl){uint32_t &e=w[i+4];if(e==37){e=79;++k;}else if(e==40){e=80;++k;}else if(e==43){e=81;++k;}}
        i+=n;
    }
    return k;
}
void tidy_rewrite(std::vector<uint32_t> &words){
    std::vector<uint32_t> out;
    if(nan_minmax){size_t k=nan_minmax_swap(words);if(k)nan_minmax_swaps.fetch_add(k,std::memory_order_relaxed);}
    if(ldc_fold){size_t k=xbg_spirv_ldc_fold(words.data(),words.size(),out);if(k){words.swap(out);ldc_folds.fetch_add(k,std::memory_order_relaxed);}}
    if(forward_temps){size_t k=xbg_spirv_forward_temps(words.data(),words.size(),out);if(k){words.swap(out);temps_forwarded.fetch_add(k,std::memory_order_relaxed);}}
}
// Diagnostic (files/dm/spv-override/<key>.spv): this SPIR-V replaces the bundle's, for offline-optimized test modules.
bool spv_override(const std::string &key,std::vector<uint32_t> &words){
    std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');std::string path=(cut==std::string::npos?dir:dir.substr(0,cut))+"/spv-override/"+key+".spv";
    FILE *f=fopen(path.c_str(),"rb");if(!f)return false;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<20 || n%4){fclose(f);return false;}
    std::vector<uint32_t> w(size_t(n)/4);bool ok=fread(w.data(),1,size_t(n),f)==size_t(n);fclose(f);if(!ok || w[0]!=0x07230203u)return false;
    words.swap(w);xbg_logf("NVN spv override %s words=%zu",key.c_str(),words.size());return true;
}
Shader *load_shader(const void *control){
    std::string key=control_key(control);auto found=shaders.find(key);if(found!=shaders.end())return found->second.get();
    const uint64_t load_t0=timing_clock();
    std::string path=std::string(cfg.shader_directory)+"/"+key+".xgs";FILE *f=fopen(path.c_str(),"rb");if(!f)xbg_fatalf("Missing genuine shader bundle %s",path.c_str());
    fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);need(length>=32 && length<=16*1024*1024,"Invalid shader bundle length");std::vector<uint8_t> bytes(length);need(fread(bytes.data(),1,bytes.size(),f)==bytes.size(),"Shader bundle read failed");fclose(f);
    xbg_shader_header h;memcpy(&h,bytes.data(),sizeof(h));need(h.magic==0x31534758 && h.version==1 && h.stage<6 && h.binding_count<=256 && h.input_count<=16 && h.c1_bytes<=65536 && h.spirv_bytes%4==0,"Invalid shader bundle header");
    size_t offset=sizeof(h),expected=offset+size_t(h.binding_count)*sizeof(xbg_shader_binding)+size_t(h.input_count)*4+h.c1_bytes+h.spirv_bytes;need(expected==bytes.size(),"Shader bundle extent mismatch");
    // 载入各段耗时（读文件、SPIR-V 改写、分析、建模块），写进下面的 loaded 行。
    // Time of each part of the load (file read, SPIR-V rewrites, analysis, module creation), for the loaded line below.
    const uint64_t load_t1=timing_clock();uint64_t load_t2=0,load_t3=0;
    auto p=std::make_unique<Shader>();p->key=key;p->stage=h.stage;p->bindings.resize(h.binding_count);memcpy(p->bindings.data(),bytes.data()+offset,h.binding_count*sizeof(xbg_shader_binding));offset+=h.binding_count*sizeof(xbg_shader_binding);p->inputs.resize(h.input_count);memcpy(p->inputs.data(),bytes.data()+offset,h.input_count*4);offset+=h.input_count*4;p->c1.assign(bytes.begin()+offset,bytes.begin()+offset+h.c1_bytes);offset+=h.c1_bytes;
    std::vector<uint32_t> words(h.spirv_bytes/4);memcpy(words.data(),bytes.data()+offset,h.spirv_bytes);spv_override(key,words);validate_capabilities(words,h.stage);
    // gfx-shader-reuse：预热线程已改写过这个着色器时直接取用；2 时照样改写，与取来的逐字比对。
    // gfx-shader-reuse: take the warm-up threads' rewrite of this shader when there is one; with 2, rewrite as well and compare word by word.
    std::vector<uint32_t> reused;const bool have_reused=shader_reuse && !ab_off("shreuse") && warm::take_rewritten(key,reused);
    if(!have_reused || shader_reuse==2){quad_rewrite(words,h.stage,key.c_str(),"runtime");warp_rewrite(words,h.stage,key.c_str(),"runtime");tidy_rewrite(words);
        if(have_reused && reused!=words){++shader_reuse_mismatches;xbg_logf("NVN shader reuse MISMATCH %s stage=%u words=%zu/%zu",key.c_str(),h.stage,reused.size(),words.size());}}
    else words.swap(reused);
    if(have_reused)++shaders_reused;
    load_t2=timing_clock();
    {auto extent=spirv_uniform_extent(words);uint32_t trimmed=0;for(auto &b:p->bindings)if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source==XBG_SOURCE_UNIFORM){auto e=extent.find(b.binding);uint32_t declared=b.bytes?b.bytes:65536;if(e!=extent.end() && e->second<declared){b.bytes=e->second;++trimmed;}}uniform_trimmed+=trimmed;}
    {bool kill=false;for(size_t i=5;i<words.size();){uint32_t n=words[i]>>16,op=words[i]&0xffff;if(op==252||op==4416||op==5380){kill=true;break;}if(!n)break;i+=n;}p->discards=kill;}
    p->raster=xbg_spirv_raster_facts(words.data(),words.size(),h.stage);
    {bool unknown=false;p->storage_writes=spirv_storage_writes(words,unknown);if(unknown){for(auto &b:p->bindings)if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)p->storage_writes.insert(b.binding);}}
    if(h.stage==0){p->input_classes.resize(p->inputs.size());need(xbg_vertex_input_classes(words.data(),words.size(),p->inputs.data(),p->inputs.size(),p->input_classes.data())==0,"Unsupported vertex input scalar interface");}
    load_t3=timing_clock();
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=words.size()*4;ci.pCode=words.data();VK(vkCreateShaderModule(cfg.device,&ci,nullptr,&p->module));warm::register_module(p->module,key);Shader *out=p.get();shaders.emplace(key,std::move(p));++stats.shader_loads;++hud::shaders;const uint64_t load_t4=timing_clock();hud::shader_ns+=load_t4-load_t0;
    xbg_logf("NVN shader %s stage=%u loaded words=%zu ms read=%.2f rewrite=%.2f analyse=%.2f module=%.2f reused=%d",key.c_str(),h.stage,words.size(),(load_t1-load_t0)/1e6,(load_t2-load_t1)/1e6,(load_t3-load_t2)/1e6,(load_t4-load_t3)/1e6,int(have_reused));return out;
}
// nvnProgramSetShaders assigns a new serial, and NVN shader control/code memory
// must stay unchanged while a program uses it: cache resolved shaders by serial
// instead of re-hashing every control segment on each bind.
std::map<uint64_t,std::array<Shader*,6>> program_cache;
const std::array<Shader*,6> &program_shaders(const prog_t *prog){
    if(prog->serial){auto it=program_cache.find(prog->serial);if(it!=program_cache.end())return it->second;}
    std::array<Shader*,6> out{};for(unsigned i=0;i<prog->nstage && i<6;i++)out[i]=load_shader(prog->sd[i].control);
    if(!prog->serial){static std::array<Shader*,6> scratch;scratch=out;return scratch;}
    return program_cache.emplace(prog->serial,out).first->second;
}
} // namespace

extern "C" void xbg_logf(const char *fmt,...){CPU_SCOPE(Log);char text[1024];va_list a;va_start(a,fmt);vsnprintf(text,sizeof(text),fmt,a);va_end(a);if(cfg.log)cfg.log(cfg.userdata,text);else fprintf(stderr,"[xbg] %s\n",text);}
extern "C" __attribute__((noreturn)) void xbg_fatalf(const char *fmt,...){va_list a;va_start(a,fmt);vsnprintf(error_text,sizeof(error_text),fmt,a);va_end(a);failed=true;xbg_logf("FATAL: %s",error_text);if(cfg.fatal)cfg.fatal(cfg.userdata,error_text);abort();}
extern "C" int xbg_backend_failed(){return failed;}
extern "C" const char *xbg_game_last_error(){return error_text;}
extern "C" int xbg_device_integer(int p,int *out){
    need(initialized && out,"NVN device query before Vulkan initialization");const auto &l=properties.limits;
    switch(p){
    case NVN_DEVICE_INFO_MAX_UNIFORM_BUFFER_SIZE:*out=std::min(65536u,l.maxUniformBufferRange);break;
    case NVN_DEVICE_INFO_COLOR_BUFFER_BINDINGS:*out=std::min(8u,l.maxColorAttachments);break;
    case NVN_DEVICE_INFO_VERTEX_BUFFER_BINDINGS:*out=std::min(16u,l.maxVertexInputBindings);break;
    case NVN_DEVICE_INFO_VERTEX_ATTRIBUTES:*out=std::min(16u,l.maxVertexInputAttributes);break;
    case NVN_DEVICE_INFO_MAX_TEXTURE_ANISOTROPY:*out=cfg.enabled_core.samplerAnisotropy?int(l.maxSamplerAnisotropy):1;break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_X:case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_Y:case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_Z:*out=l.maxComputeWorkGroupSize[p-NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_X];break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS:*out=l.maxComputeWorkGroupInvocations;break;
    case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_X:case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Y:case NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Z:*out=l.maxComputeWorkGroupCount[p-NVN_DEVICE_INFO_MAX_COMPUTE_DISPATCH_WORK_GROUPS_X];break;
    case NVN_DEVICE_INFO_MAX_VIEWPORTS:*out=1;break;
    case NVN_DEVICE_INFO_MAX_TEXTURE_LAYERS:*out=l.maxImageArrayLayers;break;
    case NVN_DEVICE_INFO_MAX_TEXTURE_SIZE:case NVN_DEVICE_INFO_MAX_RECTANGLE_TEXTURE_SIZE:*out=l.maxImageDimension2D;break;
    case NVN_DEVICE_INFO_MAX_3D_TEXTURE_SIZE:*out=l.maxImageDimension3D;break;
    case NVN_DEVICE_INFO_MAX_CUBE_MAP_TEXTURE_SIZE:*out=l.maxImageDimensionCube;break;
    case NVN_DEVICE_INFO_MAX_BUFFER_TEXTURE_SIZE:*out=l.maxTexelBufferElements;break;
    case NVN_DEVICE_INFO_MAX_PATCH_SIZE:*out=cfg.enabled_core.tessellationShader?l.maxTessellationPatchSize:0;break;
    case NVN_DEVICE_INFO_SUBPIXEL_BITS:*out=l.subPixelPrecisionBits;break;
    case NVN_DEVICE_INFO_SUPPORTS_MIN_MAX_FILTERING:case NVN_DEVICE_INFO_SUPPORTS_ASTC_FORMATS:
    case NVN_DEVICE_INFO_SUPPORTS_CONSERVATIVE_RASTER:case NVN_DEVICE_INFO_SUPPORTS_ZERO_FROM_UNMAPPED_VIRTUAL_POOL_PAGES:
    case NVN_DEVICE_INFO_SUPPORTS_PASSTHROUGH_GEOMETRY_SHADERS:case NVN_DEVICE_INFO_SUPPORTS_VIEWPORT_SWIZZLE:
    case NVN_DEVICE_INFO_SUPPORTS_SPARSE_TILED_PACKAGED_TEXTURES:case NVN_DEVICE_INFO_SUPPORTS_ADVANCED_BLEND_MODES:
    case NVN_DEVICE_INFO_SUPPORTS_DRAW_TEXTURE:case NVN_DEVICE_INFO_SUPPORTS_TARGET_INDEPENDENT_RASTERIZATION:
    case NVN_DEVICE_INFO_SUPPORTS_FRAGMENT_COVERAGE_TO_COLOR:case NVN_DEVICE_INFO_SUPPORTS_POST_DEPTH_COVERAGE:
    case NVN_DEVICE_INFO_SUPPORTS_SAMPLE_LOCATIONS:case NVN_DEVICE_INFO_SUPPORTS_FRAGMENT_SHADER_INTERLOCK:*out=0;break;
    case NVN_DEVICE_INFO_SUPPORTS_STENCIL8_FORMAT:{VkFormatProperties f;vkGetPhysicalDeviceFormatProperties(cfg.physical,VK_FORMAT_S8_UINT,&f);*out=(f.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)!=0;break;}
    default:return 0; // NVN ABI sizes/alignments are host-emulated, not Vulkan addresses.
    }return 1;
}

namespace {
struct PipelineEntry {VkPipeline pipeline;uint64_t last_used;};
std::map<std::string,PipelineEntry> pipeline_objects;
// First-level lookup (D2R camp: 2198 per frame, ~7% of the render thread with the launch-stable key): a process-local key
// that names the layout and render-pass definitions by their cached, immutable entries instead of copying their words in.
// Only a miss builds the full key (warm-up list, pipeline_objects). Cleared with the pipeline, layout and render-pass
// caches; an evicted pipeline is dropped from it.
struct FastKeyHash{size_t operator()(const std::vector<uint32_t> &v)const{uint64_t h=1469598103934665603ull;for(uint32_t x:v){h^=x;h*=1099511628211ull;}return size_t(h^(h>>29));}};
std::unordered_map<std::vector<uint32_t>,VkPipeline,FastKeyHash> pipeline_fast;bool pipeline_fast_enabled=true;uint64_t pipeline_fast_hits=0;
bool pipe_memo_valid=false;   // the previous draw's pipeline (pipe_memo_key below); cleared wherever pipeline_fast drops entries
uint64_t pipeline_tick=0;
constexpr size_t pipeline_object_limit=4096; // field scenes exceed 256 live pipelines; eviction+rebuild caused 100ms+ hitches
struct PipelineLease {
    VkPipeline pipeline{};bool transient=false;
    ~PipelineLease(){if(transient && pipeline){VkPipeline p=pipeline;retire_handle([p]{vkDestroyPipeline(cfg.device,p,nullptr);});}}
    PipelineLease()=default;
    PipelineLease(const PipelineLease&)=delete;
    PipelineLease& operator=(const PipelineLease&)=delete;
};
void report_pipeline_cache(){
    xbg_logf("NVN CPU_TIMING pipeline_cache hits=%llu misses=%llu evictions=%llu bypass=%llu entries=%zu limit=%zu",(unsigned long long)(pipeline_counts.hits-pipeline_before.hits),(unsigned long long)(pipeline_counts.misses-pipeline_before.misses),(unsigned long long)(pipeline_counts.evictions-pipeline_before.evictions),(unsigned long long)(pipeline_counts.bypass-pipeline_before.bypass),pipeline_objects.size(),pipeline_object_limit);
}
// Pipeline warm-up (the "precompiled shader cache" of console emulators). The Maleoon driver does not grow the
// VkPipelineCache, so every launch recompiled each graphics pipeline on first use, inside the frame (~100+ ms each).
// Every newly created graphics pipeline is appended to files/vk-pipeline-list.bin (shader bundle keys + the exact
// serialized create-info key, with shader modules identified by a stable hash of their bundle key). At start-up,
// background threads rebuild those pipelines from the list; a draw that needs one takes it ready-made.
// files/gfx-no-warm disables building (recording continues).
namespace warm {
std::mutex mu;std::unordered_map<VkShaderModule,uint64_t> ids;
uint64_t fnv(const std::string &s){uint64_t h=1469598103934665603ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h|(1ull<<63);}
uint64_t module_id(VkShaderModule m){std::lock_guard<std::mutex> l(mu);auto it=ids.find(m);return it==ids.end()?uint64_t(m):it->second;}
std::unordered_map<uint64_t,std::string> id_keys;
void register_module(VkShaderModule m,const std::string &key){std::lock_guard<std::mutex> l(mu);uint64_t id=fnv(key);ids[m]=id;id_keys[id]=key;}
struct Rec{std::vector<std::string> keys;std::vector<uint32_t> words;};
std::vector<Rec> recs;std::unordered_set<std::string> known;std::vector<uint8_t> pending;
std::unordered_map<std::string,VkPipeline> ready;std::vector<std::thread> threads;std::atomic<bool> stop{false};std::atomic<size_t> next{0};
std::atomic<uint64_t> built{0},failed{0},build_ns{0},compiled{0};uint64_t hits=0,creates=0,create_ns=0,create_max_ns=0;bool enabled=true;
std::unordered_map<std::string,VkShaderModule> modules;std::unordered_map<std::string,VkPipelineLayout> layouts;std::unordered_map<std::string,VkRenderPass> passes;
std::vector<VkDescriptorSetLayout> set_layouts;
std::string list_path(){std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');return (cut==std::string::npos?dir:dir.substr(0,cut))+"/vk-pipeline-list.bin";}
void record(const VkGraphicsPipelineCreateInfo &ci,const XbgPipelineKey &key){
    std::string bytes(reinterpret_cast<const char*>(key.words),size_t(key.count)*4);
    std::vector<std::string> keys;{std::lock_guard<std::mutex> l(mu);if(!known.insert(bytes).second)return;
        for(uint32_t i=0;i<ci.stageCount;i++){auto it=ids.find(ci.pStages[i].module);if(it==ids.end())return;keys.push_back(id_keys[it->second]);}}
    auto put=[](uint32_t v){for(int i=0;i<4;i++)pending.push_back(uint8_t(v>>(8*i)));};
    put(0x57504c58);put(uint32_t(keys.size()));for(auto &k:keys){put(uint32_t(k.size()));pending.insert(pending.end(),k.begin(),k.end());}
    put(key.count);pending.insert(pending.end(),bytes.begin(),bytes.end());
}
void flush(){if(pending.empty())return;FILE *f=fopen(list_path().c_str(),"ab");if(!f)return;bool ok=fwrite(pending.data(),1,pending.size(),f)==pending.size();ok=fclose(f)==0&&ok;if(ok)pending.clear();}
// gfx-shader-reuse：预热改写好的 SPIR-V，按着色器键留到 load_shader 取走（取走即删）；没用到的随 shutdown 释放。
// gfx-shader-reuse: SPIR-V the warm-up rewrote, kept by shader key until load_shader takes it (taking removes it); unused entries
// are freed at shutdown.
bool keep_rewritten=false;std::unordered_map<std::string,std::vector<uint32_t>> rewritten;
bool take_rewritten(const std::string &key,std::vector<uint32_t> &out){std::lock_guard<std::mutex> l(mu);auto it=rewritten.find(key);if(it==rewritten.end())return false;out.swap(it->second);rewritten.erase(it);return true;}
VkShaderModule load_module(const std::string &key){
    {std::lock_guard<std::mutex> l(mu);auto it=modules.find(key);if(it!=modules.end())return it->second;}
    FILE *f=fopen((std::string(cfg.shader_directory)+"/"+key+".xgs").c_str(),"rb");if(!f)return VK_NULL_HANDLE;
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<32 || n>16*1024*1024){fclose(f);return VK_NULL_HANDLE;}std::vector<uint8_t> b(static_cast<size_t>(n));bool ok=fread(b.data(),1,b.size(),f)==b.size();fclose(f);if(!ok)return VK_NULL_HANDLE;
    xbg_shader_header h;memcpy(&h,b.data(),sizeof(h));if(h.magic!=0x31534758 || h.version!=1 || h.spirv_bytes%4)return VK_NULL_HANDLE;
    size_t off=sizeof(h)+size_t(h.binding_count)*sizeof(xbg_shader_binding)+size_t(h.input_count)*4+h.c1_bytes;if(off+h.spirv_bytes!=b.size())return VK_NULL_HANDLE;
    std::vector<uint32_t> w(h.spirv_bytes/4);memcpy(w.data(),b.data()+off,h.spirv_bytes);spv_override(key,w);quad_rewrite(w,h.stage,key.c_str(),"warm");warp_rewrite(w,h.stage,key.c_str(),"warm");tidy_rewrite(w);
    if(keep_rewritten){std::lock_guard<std::mutex> l(mu);rewritten.emplace(key,w);}
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=w.size()*4;ci.pCode=w.data();VkShaderModule m;if(vkCreateShaderModule(cfg.device,&ci,nullptr,&m)!=VK_SUCCESS)return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> l(mu);auto ins=modules.emplace(key,m);if(!ins.second){vkDestroyShaderModule(cfg.device,m,nullptr);return ins.first->second;}return m;
}
struct Reader{const uint32_t *w;size_t n,i=0;bool bad=false;uint32_t u(){if(i>=n){bad=true;return 0;}return w[i++];}uint64_t q(){uint64_t lo=u();return lo|(uint64_t(u())<<32);}float f(){uint32_t b=u();float x;memcpy(&x,&b,4);return x;}};
VkPipelineLayout make_layout(const uint32_t *w,uint32_t n){
    std::string k(reinterpret_cast<const char*>(w),size_t(n)*4);{std::lock_guard<std::mutex> l(mu);auto it=layouts.find(k);if(it!=layouts.end())return it->second;}
    Reader r{w,n};VkDescriptorSetLayoutCreateInfo s{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};s.flags=r.u();uint32_t nb=r.u();if(nb>4096)return VK_NULL_HANDLE;std::vector<VkDescriptorSetLayoutBinding> bs(nb);
    for(auto &b:bs){b.binding=r.u();b.descriptorType=VkDescriptorType(r.u());b.descriptorCount=r.u();b.stageFlags=r.u();}s.bindingCount=nb;s.pBindings=bs.data();
    VkPipelineLayoutCreateInfo p{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};p.flags=r.u();uint32_t sets=r.u(),np=r.u();if(sets!=1 || np>16 || r.bad)return VK_NULL_HANDLE;std::vector<VkPushConstantRange> pr(np);for(auto &x:pr){x.stageFlags=r.u();x.offset=r.u();x.size=r.u();}
    if(r.bad || r.i!=n)return VK_NULL_HANDLE;VkDescriptorSetLayout sl;if(vkCreateDescriptorSetLayout(cfg.device,&s,nullptr,&sl)!=VK_SUCCESS)return VK_NULL_HANDLE;
    p.setLayoutCount=1;p.pSetLayouts=&sl;p.pushConstantRangeCount=np;p.pPushConstantRanges=pr.data();VkPipelineLayout pl;if(vkCreatePipelineLayout(cfg.device,&p,nullptr,&pl)!=VK_SUCCESS){vkDestroyDescriptorSetLayout(cfg.device,sl,nullptr);return VK_NULL_HANDLE;}
    std::lock_guard<std::mutex> l(mu);set_layouts.push_back(sl);layouts.emplace(k,pl);return pl;
}
VkRenderPass make_pass(const uint32_t *w,uint32_t n){
    std::string k(reinterpret_cast<const char*>(w),size_t(n)*4);{std::lock_guard<std::mutex> l(mu);auto it=passes.find(k);if(it!=passes.end())return it->second;}
    Reader r{w,n};VkRenderPassCreateInfo p{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};p.flags=r.u();uint32_t na=r.u();if(na>9)return VK_NULL_HANDLE;std::vector<VkAttachmentDescription> a(na);
    for(auto &x:a){x.flags=r.u();x.format=VkFormat(r.u());x.samples=VkSampleCountFlagBits(r.u());x.loadOp=VkAttachmentLoadOp(r.u());x.storeOp=VkAttachmentStoreOp(r.u());x.stencilLoadOp=VkAttachmentLoadOp(r.u());x.stencilStoreOp=VkAttachmentStoreOp(r.u());x.initialLayout=VkImageLayout(r.u());x.finalLayout=VkImageLayout(r.u());}
    VkSubpassDescription s{};s.flags=r.u();s.pipelineBindPoint=VkPipelineBindPoint(r.u());uint32_t nc=r.u();if(nc>8)return VK_NULL_HANDLE;std::vector<VkAttachmentReference> c(nc);for(auto &x:c){x.attachment=r.u();x.layout=VkImageLayout(r.u());}
    VkAttachmentReference d{};bool hd=r.u()!=0;if(hd){d.attachment=r.u();d.layout=VkImageLayout(r.u());}
    uint32_t nd=r.u();if(nd>8)return VK_NULL_HANDLE;std::vector<VkSubpassDependency> dep(nd);for(auto &x:dep){x.srcSubpass=r.u();x.dstSubpass=r.u();x.srcStageMask=r.u();x.dstStageMask=r.u();x.srcAccessMask=r.u();x.dstAccessMask=r.u();x.dependencyFlags=r.u();}
    if(r.bad || r.i!=n)return VK_NULL_HANDLE;
    s.colorAttachmentCount=nc;s.pColorAttachments=c.data();s.pDepthStencilAttachment=hd?&d:nullptr;p.attachmentCount=na;p.pAttachments=a.data();p.subpassCount=1;p.pSubpasses=&s;p.dependencyCount=nd;p.pDependencies=dep.data();
    VkRenderPass rp;if(vkCreateRenderPass(cfg.device,&p,nullptr,&rp)!=VK_SUCCESS)return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> l(mu);auto ins=passes.emplace(k,rp);if(!ins.second){vkDestroyRenderPass(cfg.device,rp,nullptr);return ins.first->second;}return rp;
}
bool build(const Rec &rec){
    Reader r{rec.words.data(),rec.words.size()};if(r.u()!=1)return false;
    uint32_t ln=r.u();if(r.i+ln>r.n)return false;VkPipelineLayout layout=make_layout(r.w+r.i,ln);r.i+=ln;
    uint32_t pn=r.u();if(r.i+pn>r.n)return false;VkRenderPass pass=make_pass(r.w+r.i,pn);r.i+=pn;if(!layout || !pass)return false;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.flags=r.u();ci.subpass=r.u();uint32_t ns=r.u();if(ns>5 || ns!=rec.keys.size())return false;
    VkPipelineShaderStageCreateInfo st[5]{};for(uint32_t i=0;i<ns;i++){st[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st[i].flags=r.u();st[i].stage=VkShaderStageFlagBits(r.u());uint64_t id=r.q();if(id!=fnv(rec.keys[i]))return false;st[i].module=load_module(rec.keys[i]);if(!st[i].module)return false;st[i].pName="main";}
    VkPipelineVertexInputStateCreateInfo v{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};v.flags=r.u();uint32_t nvb=r.u();if(nvb>32)return false;std::vector<VkVertexInputBindingDescription> vb(nvb);for(auto &x:vb){x.binding=r.u();x.stride=r.u();x.inputRate=VkVertexInputRate(r.u());}
    uint32_t nva=r.u();if(nva>32)return false;std::vector<VkVertexInputAttributeDescription> va(nva);for(auto &x:va){x.location=r.u();x.binding=r.u();x.format=VkFormat(r.u());x.offset=r.u();}
    v.vertexBindingDescriptionCount=nvb;v.pVertexBindingDescriptions=vb.data();v.vertexAttributeDescriptionCount=nva;v.pVertexAttributeDescriptions=va.data();
    VkPipelineInputAssemblyStateCreateInfo a{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};a.flags=r.u();a.topology=VkPrimitiveTopology(r.u());a.primitiveRestartEnable=r.u();
    VkPipelineTessellationStateCreateInfo t{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};bool ht=r.u()!=0;if(ht){t.flags=r.u();t.patchControlPoints=r.u();}
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.flags=r.u();vp.viewportCount=r.u();vp.scissorCount=r.u();bool hv=r.u()!=0,hs=r.u()!=0;if(vp.viewportCount>16 || vp.scissorCount>16)return false;
    std::vector<VkViewport> vps(hv?vp.viewportCount:0);for(auto &x:vps){x.x=r.f();x.y=r.f();x.width=r.f();x.height=r.f();x.minDepth=r.f();x.maxDepth=r.f();}
    std::vector<VkRect2D> scs(hs?vp.scissorCount:0);for(auto &x:scs){x.offset.x=int32_t(r.u());x.offset.y=int32_t(r.u());x.extent.width=r.u();x.extent.height=r.u();}
    vp.pViewports=hv?vps.data():nullptr;vp.pScissors=hs?scs.data():nullptr;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.flags=r.u();rs.depthClampEnable=r.u();rs.rasterizerDiscardEnable=r.u();rs.polygonMode=VkPolygonMode(r.u());rs.cullMode=r.u();rs.frontFace=VkFrontFace(r.u());rs.depthBiasEnable=r.u();rs.depthBiasConstantFactor=r.f();rs.depthBiasClamp=r.f();rs.depthBiasSlopeFactor=r.f();rs.lineWidth=r.f();
    VkPipelineMultisampleStateCreateInfo m{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};m.flags=r.u();m.rasterizationSamples=VkSampleCountFlagBits(r.u());m.sampleShadingEnable=r.u();m.minSampleShading=r.f();m.alphaToCoverageEnable=r.u();m.alphaToOneEnable=r.u();bool hm=r.u()!=0;
    uint32_t mask[2]{};if(hm){if(m.rasterizationSamples>VK_SAMPLE_COUNT_64_BIT)return false;for(uint32_t i=0;i<(uint32_t(m.rasterizationSamples)+31)/32;i++)mask[i]=r.u();m.pSampleMask=mask;}
    auto stencil=[&](VkStencilOpState &s){s.failOp=VkStencilOp(r.u());s.passOp=VkStencilOp(r.u());s.depthFailOp=VkStencilOp(r.u());s.compareOp=VkCompareOp(r.u());s.compareMask=r.u();s.writeMask=r.u();s.reference=r.u();};
    VkPipelineDepthStencilStateCreateInfo d{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};d.flags=r.u();d.depthTestEnable=r.u();d.depthWriteEnable=r.u();d.depthCompareOp=VkCompareOp(r.u());d.depthBoundsTestEnable=r.u();d.stencilTestEnable=r.u();stencil(d.front);stencil(d.back);d.minDepthBounds=r.f();d.maxDepthBounds=r.f();
    VkPipelineColorBlendStateCreateInfo b{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};b.flags=r.u();b.logicOpEnable=r.u();b.logicOp=VkLogicOp(r.u());uint32_t nb=r.u();if(nb>8)return false;std::vector<VkPipelineColorBlendAttachmentState> ba(nb);
    for(auto &x:ba){x.blendEnable=r.u();x.srcColorBlendFactor=VkBlendFactor(r.u());x.dstColorBlendFactor=VkBlendFactor(r.u());x.colorBlendOp=VkBlendOp(r.u());x.srcAlphaBlendFactor=VkBlendFactor(r.u());x.dstAlphaBlendFactor=VkBlendFactor(r.u());x.alphaBlendOp=VkBlendOp(r.u());x.colorWriteMask=r.u();}
    b.attachmentCount=nb;b.pAttachments=ba.data();for(float &x:b.blendConstants)x=r.f();
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.flags=r.u();uint32_t nd=r.u();if(nd>64)return false;std::vector<VkDynamicState> ds(nd);for(auto &x:ds)x=VkDynamicState(r.u());dy.dynamicStateCount=nd;dy.pDynamicStates=ds.data();
    if(r.bad || r.i!=r.n)return false;
    ci.stageCount=ns;ci.pStages=st;ci.pVertexInputState=&v;ci.pInputAssemblyState=&a;ci.pTessellationState=ht?&t:nullptr;ci.pViewportState=&vp;ci.pRasterizationState=&rs;ci.pMultisampleState=&m;ci.pDepthStencilState=&d;ci.pColorBlendState=&b;ci.pDynamicState=&dy;ci.layout=layout;ci.renderPass=pass;
    uint64_t c0=timing_clock();VkPipeline pl;if(vkCreateGraphicsPipelines(cfg.device,pipeline_cache,1,&ci,nullptr,&pl)!=VK_SUCCESS)return false;
    if(timing_clock()-c0>=compile_ns)++compiled;
    std::string bytes(reinterpret_cast<const char*>(rec.words.data()),rec.words.size()*4);
    std::lock_guard<std::mutex> l(mu);if(!ready.emplace(bytes,pl).second)vkDestroyPipeline(cfg.device,pl,nullptr);return true;
}
void worker(){
    setpriority(PRIO_PROCESS,0,5); // below the game and graphics threads
    for(;;){if(stop.load())return;size_t i=next.fetch_add(1);if(i>=recs.size())return;uint64_t t0=timing_clock();if(build(recs[i]))++built;else ++failed;build_ns+=timing_clock()-t0;}
}
// Runtime builds for draws that may be skipped (gfx-async-pipelines): a blended draw that writes no depth, stencil or
// storage and counts no samples the game reads is skipped while one background thread builds its pipeline from the
// record the warm-up list keeps; a later frame takes it ready-made (take). The effect then appears a few frames late
// the first time, instead of the frame waiting 22-50 ms for the driver (OnePlus 13, new spell effects). Off unless the
// host asks (DM_GFX_ASYNC_PIPELINES; marker gfx-async-pipelines 1 / 0).
bool async_on=false;std::deque<Rec> async_q;std::unordered_set<std::string> async_pending,async_failed;std::condition_variable async_cv;std::thread async_thread;
std::atomic<uint64_t> async_built{0},async_skips{0};
void async_worker(){
    setpriority(PRIO_PROCESS,0,5);
    for(;;){Rec r;{std::unique_lock<std::mutex> l(mu);async_cv.wait(l,[]{return stop.load() || !async_q.empty();});if(stop.load())return;r=std::move(async_q.front());async_q.pop_front();}
        bool ok=build(r);if(ok)++async_built;else ++failed;
        std::string bytes(reinterpret_cast<const char*>(r.words.data()),r.words.size()*4);std::lock_guard<std::mutex> l(mu);async_pending.erase(bytes);if(!ok)async_failed.insert(bytes);}
}
// Queue the build of this draw's pipeline (once); false when the caller should build it now: a module without a bundle key,
// a record that failed to build in the background before, or the warm-up threads stopped.
bool async_request(const VkGraphicsPipelineCreateInfo &ci,const XbgPipelineKey &key,const std::string &bytes){
    {std::lock_guard<std::mutex> l(mu);if(async_pending.count(bytes))return true;if(stop.load() || async_failed.count(bytes))return false;Rec r;
        for(uint32_t i=0;i<ci.stageCount;i++){auto it=ids.find(ci.pStages[i].module);if(it==ids.end())return false;r.keys.push_back(id_keys[it->second]);}
        r.words.assign(key.words,key.words+key.count);async_pending.insert(bytes);async_q.push_back(std::move(r));}
    if(!async_thread.joinable())async_thread=std::thread(async_worker);
    async_cv.notify_one();record(ci,key);return true;
}
// 首次启动还没有列表。这时随着色器发的种子（shaders/vk-pipeline-list.seed，用 dm/android/tools/merge_warm_lists.py 合并各设备的
// 列表而成）就成为列表，预热便在第一场战斗之前、而不是在战斗中建好那些设备见过的管线；设备建不了的记录在预热时失败（warm_failed）。
// A first launch has no list yet. A seed shipped with the shaders (shaders/vk-pipeline-list.seed, made from devices' lists
// with dm/android/tools/merge_warm_lists.py) then becomes the list, so the warm-up builds the pipelines those devices saw
// before the first fight instead of in it; records a device cannot build fail in the warm-up (warm_failed).
void seed(){
    const std::string list=list_path();if(FILE *f=fopen(list.c_str(),"rb")){fclose(f);return;}
    const std::string from=std::string(cfg.shader_directory)+"/vk-pipeline-list.seed";FILE *in=fopen(from.c_str(),"rb");if(!in)return;
    const std::string tmp=list+".tmp";FILE *out=fopen(tmp.c_str(),"wb");bool ok=out!=nullptr;size_t total=0;
    for(char buf[65536];ok;){size_t n=fread(buf,1,sizeof buf,in);if(!n)break;ok=fwrite(buf,1,n,out)==n;total+=n;}
    fclose(in);if(out)ok=fclose(out)==0 && ok;if(ok)ok=rename(tmp.c_str(),list.c_str())==0;if(!ok)remove(tmp.c_str());
    xbg_logf("NVN warm list seeded from %s bytes=%zu ok=%d",from.c_str(),total,int(ok));
}
void start(){
    seed();
    FILE *f=fopen(list_path().c_str(),"rb");if(f){fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);std::vector<uint8_t> b(n>0 && n<(64l<<20)?size_t(n):0);bool ok=b.empty() || fread(b.data(),1,b.size(),f)==b.size();fclose(f);
        size_t i=0;auto get=[&](uint32_t &v){if(i+4>b.size())return false;memcpy(&v,b.data()+i,4);i+=4;return true;};
        while(ok && i<b.size()){uint32_t magic,ns,count;if(!get(magic)||magic!=0x57504c58||!get(ns)||ns>5)break;Rec r;bool good=true;
            for(uint32_t s=0;s<ns && good;s++){uint32_t len;if(!get(len)||len>256||i+len>b.size()){good=false;break;}r.keys.emplace_back(reinterpret_cast<const char*>(b.data()+i),len);i+=len;}
            if(!good||!get(count)||count>XbgPipelineKey::Capacity||i+size_t(count)*4>b.size())break;r.words.resize(count);memcpy(r.words.data(),b.data()+i,size_t(count)*4);i+=size_t(count)*4;
            if(known.insert(std::string(reinterpret_cast<const char*>(r.words.data()),r.words.size()*4)).second)recs.push_back(std::move(r));}}
    xbg_logf("NVN warm list=%zu enabled=%d",recs.size(),int(enabled));
    if(!enabled || recs.empty())return;stop=false;next=0;unsigned n=std::min<unsigned>(3,std::max(1u,std::thread::hardware_concurrency()/3));
    for(unsigned i=0;i<n;i++)threads.emplace_back(worker);
}
void shutdown(){{std::lock_guard<std::mutex> l(mu);stop=true;}async_cv.notify_all();for(auto &t:threads)if(t.joinable())t.join();threads.clear();if(async_thread.joinable())async_thread.join();std::lock_guard<std::mutex> l(mu);async_q.clear();async_pending.clear();async_failed.clear();
    for(auto &p:ready)vkDestroyPipeline(cfg.device,p.second,nullptr);ready.clear();for(auto &p:layouts)vkDestroyPipelineLayout(cfg.device,p.second,nullptr);layouts.clear();for(auto s:set_layouts)vkDestroyDescriptorSetLayout(cfg.device,s,nullptr);set_layouts.clear();
    for(auto &p:passes)vkDestroyRenderPass(cfg.device,p.second,nullptr);passes.clear();for(auto &m:modules)vkDestroyShaderModule(cfg.device,m.second,nullptr);modules.clear();ids.clear();id_keys.clear();recs.clear();known.clear();rewritten.clear();}
bool take(const std::string &bytes,VkPipeline &out){std::lock_guard<std::mutex> l(mu);auto it=ready.find(bytes);if(it==ready.end())return false;out=it->second;ready.erase(it);return true;}
}
// Diagnostic (marker gfx-pipe-stats, with the warm-up off): the driver's statistics of each pipeline as it is created, and
// the draws per frame of each pipeline over frames 3000-3059.
PFN_vkGetPipelineExecutablePropertiesKHR pstat_props=nullptr;PFN_vkGetPipelineExecutableStatisticsKHR pstat_stats=nullptr;bool pipe_stats=false,pstat_logged=false;
struct PstatUse{uint64_t draws=0;std::string what;};std::unordered_map<VkPipeline,PstatUse> pstat_use;
std::string pstat_what(){std::string s;for(unsigned st=0;st<6;st++)if(state.shader[st]){if(!s.empty())s+=';';s+=std::to_string(st)+":"+state.shader[st]->key;}return s;}
void pstat_log(VkPipeline pl){
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};pi.pipeline=pl;uint32_t n=0;if(pstat_props(cfg.device,&pi,&n,nullptr)!=VK_SUCCESS)return;
    std::vector<VkPipelineExecutablePropertiesKHR> ex(n,VkPipelineExecutablePropertiesKHR{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});pstat_props(cfg.device,&pi,&n,ex.data());
    for(uint32_t i=0;i<n;i++){VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};ei.pipeline=pl;ei.executableIndex=i;uint32_t m=0;pstat_stats(cfg.device,&ei,&m,nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> st(m,VkPipelineExecutableStatisticKHR{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});pstat_stats(cfg.device,&ei,&m,st.data());
        std::string line;for(auto &x:st){char v[48];switch(x.format){case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:snprintf(v,sizeof v,"%u",x.value.b32);break;case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:snprintf(v,sizeof v,"%lld",(long long)x.value.i64);break;
                case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:snprintf(v,sizeof v,"%llu",(unsigned long long)x.value.u64);break;default:snprintf(v,sizeof v,"%g",x.value.f64);}
            std::string name=x.name;for(auto &c:name)if(c==' ' || c=='=')c='_';line+=" "+name+"="+v;}
        std::string en=ex[i].name;for(auto &c:en)if(c==' ')c='_';
        xbg_logf("NVN PSTAT pipeline=%p what=%s exec=%u stages=%#x name=%s subgroup=%u%s",(void*)pl,pstat_what().c_str(),i,unsigned(ex[i].stages),en.c_str(),ex[i].subgroupSize,line.c_str());}
}
void pipeline_lookup(PipelineLease &out,const XbgPipelineKey &key,bool cacheable,const VkGraphicsPipelineCreateInfo *graphics,const VkComputePipelineCreateInfo *compute,bool may_skip=false){
    static std::string bytes;bytes.clear();   // reused key buffer (graphics thread only)
    if(cacheable){
        bytes.assign(reinterpret_cast<const char*>(key.words),size_t(key.count)*4);
        auto found=pipeline_objects.find(bytes);
        if(found!=pipeline_objects.end()){++pipeline_counts.hits;found->second.last_used=++pipeline_tick;out.pipeline=found->second.pipeline;return;}
        ++pipeline_counts.misses;
    }else ++pipeline_counts.bypass;
    if(graphics && cacheable && warm::take(bytes,out.pipeline)){++warm::hits;}
    // A draw that may be skipped gets no pipeline while its build is queued (warm::async_request); it is not cached.
    else if(graphics && cacheable && may_skip && warm::async_request(*graphics,key,bytes)){out.pipeline=VK_NULL_HANDLE;out.transient=true;++warm::async_skips;return;}
    else if(graphics){uint64_t t0=timing_clock();VkGraphicsPipelineCreateInfo g=*graphics;if(pipe_stats)g.flags|=VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;VK(vkCreateGraphicsPipelines(cfg.device,pipeline_cache,1,&g,nullptr,&out.pipeline));if(pipe_stats)pstat_log(out.pipeline);uint64_t dt=timing_clock()-t0;if(dt>=compile_ns)++pipelines_unsaved;++warm::creates;warm::create_ns+=dt;++hud::pipelines;hud::pipeline_ns+=dt;warm::create_max_ns=std::max(warm::create_max_ns,dt);if(cacheable)warm::record(*graphics,key);}
    else{uint64_t t0=timing_clock();VkComputePipelineCreateInfo cc=*compute;if(pipe_stats)cc.flags|=VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;VK(vkCreateComputePipelines(cfg.device,pipeline_cache,1,&cc,nullptr,&out.pipeline));++pipelines_unsaved;++hud::pipelines;hud::pipeline_ns+=timing_clock()-t0;if(pipe_stats)pstat_log(out.pipeline);}
    if(!cacheable){out.transient=true;return;}
    if(pipeline_objects.size()==pipeline_object_limit){
        // All prior draws/dispatches waited their fence. This lookup occurs
        // before binding the current pipeline; no retained entry is in flight.
        auto oldest=std::min_element(pipeline_objects.begin(),pipeline_objects.end(),[](const auto &a,const auto &b){return a.second.last_used<b.second.last_used;});
        {VkPipeline old=oldest->second.pipeline;retire_handle([old]{vkDestroyPipeline(cfg.device,old,nullptr);});for(auto f=pipeline_fast.begin();f!=pipeline_fast.end();)if(f->second==old)f=pipeline_fast.erase(f);else ++f;pipe_memo_valid=false;}pipeline_objects.erase(oldest);++pipeline_counts.evictions;
    }
    pipeline_objects.emplace(std::string(bytes),PipelineEntry{out.pipeline,++pipeline_tick});
}
void clear_pipeline_objects(){for(auto &p:pipeline_objects)vkDestroyPipeline(cfg.device,p.second.pipeline,nullptr);pipeline_objects.clear();pipeline_fast.clear();pipe_memo_valid=false;}
struct PipelineDefinition {
    std::vector<uint32_t> words;bool valid=false;
    void assign(const XbgPipelineKey &k,bool ok){valid=ok && k.valid;if(valid)words.assign(k.words,k.words+k.count);}
    XbgPipelineKeyView view()const{return {words.data(),uint32_t(words.size()),valid};}
};
// Descriptor set layouts, pipeline layouts and samplers are immutable for a
// given binding list / sampler state, so they are created once and cached.
// Descriptor sets come from per-slot pools that are reset after the slot fence.
struct LayoutEntry {VkDescriptorSetLayout layout{};VkPipelineLayout pipeline_layout{};PipelineDefinition definition;VkDescriptorUpdateTemplate tmpl{};std::vector<uint64_t> tmpl_sig;};
// One descriptor payload slot for update templates (image/buffer info or texel view).
union DescEntry{VkDescriptorImageInfo image;VkDescriptorBufferInfo buffer;VkBufferView view;};

std::map<std::vector<uint32_t>,LayoutEntry> layout_cache;
std::map<std::array<const void*,7>,LayoutEntry*> shader_layouts; // shader set -> layout (bindings are immutable per shader)
std::map<std::array<uint32_t,16>,VkSampler> sampler_cache;
Arena static_arena; // never reset: immutable per-shader c1 constants
std::map<std::pair<float,float>,Buffer> support_static; // support block per viewport size (contents depend on it only)
struct SamplerFastKey{std::array<uint32_t,11> w;bool operator==(const SamplerFastKey &o)const{return w==o.w;}};
struct SamplerFastHash{size_t operator()(const SamplerFastKey &k)const{uint64_t h=1469598103934665603ull;for(uint32_t v:k.w){h^=v;h*=1099511628211ull;}return size_t(h);}};
std::unordered_map<SamplerFastKey,VkSampler,SamplerFastHash> sampler_fast;
void clear_descriptor_caches(){sampler_fast.clear();pipeline_fast.clear();pipe_memo_valid=false;
    for(auto &l:layout_cache){if(l.second.tmpl)vkDestroyDescriptorUpdateTemplate(cfg.device,l.second.tmpl,nullptr);vkDestroyPipelineLayout(cfg.device,l.second.pipeline_layout,nullptr);vkDestroyDescriptorSetLayout(cfg.device,l.second.layout,nullptr);}layout_cache.clear();shader_layouts.clear();
    for(auto &x:sampler_cache)vkDestroySampler(cfg.device,x.second,nullptr);sampler_cache.clear();
    static_arena.release();support_static.clear();
}
VkDescriptorSet allocate_set(VkDescriptorSetLayout layout){
    Slot &s=recording_slot();
    for(;;){
        if(s.pool_next==s.pools.size()){
            std::vector<VkDescriptorPoolSize> sizes;for(uint32_t t=0;t<=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;t++)sizes.push_back({VkDescriptorType(t),t==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||t==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||t==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||t==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER?16384u:2048u});
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};ci.maxSets=2048;ci.poolSizeCount=sizes.size();ci.pPoolSizes=sizes.data();
            VkDescriptorPool pool;VK(vkCreateDescriptorPool(cfg.device,&ci,nullptr,&pool));s.pools.push_back(pool);
        }
        // Sets are allocated 16 at a time per layout and handed out from a per-slot spare list.
        auto &spare=s.spare_sets[layout];auto &used=s.used_sets[layout];if(!spare.empty()){VkDescriptorSet set=spare.back();spare.pop_back();used.push_back(set);return set;}
        VkDescriptorSetLayout layouts[16];for(auto &l:layouts)l=layout;VkDescriptorSet sets[16]{};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=s.pools[s.pool_next];ai.descriptorSetCount=16;ai.pSetLayouts=layouts;
        VkResult r=vkAllocateDescriptorSets(cfg.device,&ai,sets);
        if(r!=VK_SUCCESS){ai.descriptorSetCount=1;r=vkAllocateDescriptorSets(cfg.device,&ai,sets);if(r==VK_SUCCESS){used.push_back(sets[0]);return sets[0];}}
        else{spare.insert(spare.end(),sets+1,sets+16);used.push_back(sets[0]);return sets[0];}
        if(r==VK_ERROR_OUT_OF_POOL_MEMORY || r==VK_ERROR_FRAGMENTED_POOL){++s.pool_next;continue;}
        check(r,"vkAllocateDescriptorSets");
    }
}
struct Descriptors {
    const LayoutEntry *entry=nullptr;VkDescriptorSet set{};VkPipelineLayout pipeline_layout{};std::vector<uint32_t> dynamic_offsets;
    std::vector<Buffer> buffers;std::vector<VkBufferView> buffer_views; // buffers: reserved up front, pointers stay valid
    std::vector<std::pair<std::pair<uint64_t,uint64_t>,Buffer*>> storage; // alias lookup for storage bindings
    std::vector<std::pair<std::pair<uint64_t,uint64_t>,Buffer*>> written; // ranges the GPU may write
    struct InputImage {Image *image;view_t view;bool has_view;std::string label;};
    std::vector<InputImage> capture_inputs;
    ~Descriptors(){for(auto x:buffer_views)vkDestroyBufferView(cfg.device,x,nullptr);}
    Buffer *keep(const Buffer &b){need(buffers.size()<buffers.capacity(),"Descriptor buffer reservation exceeded");buffers.push_back(b);return &buffers.back();}
    bool needs_writeback()const{return !written.empty();}
    void defer_writeback(){const void *slot=&slots[slot_index];for(auto &v:written){if(draw_detail)draw_timing.storage(v.first.second);snapshot_invalidate(v.first.first,v.first.second);pending_writebacks.push_back({v.first.first,v.first.second,*v.second,slot});}}
    void writeback(){for(auto &v:written){if(draw_detail)draw_timing.storage(v.first.second);v.second->invalidate();invalidate_cpu_range(v.first.first,v.first.second);memcpy(reinterpret_cast<void*>(v.first.first),v.second->mapped,v.first.second);}}
    void capture_read_images(){for(auto &i:capture_inputs)capture_image(*i.image,i.has_view?&i.view:nullptr,i.label.c_str());}
};
// Per-draw descriptor records were one make_shared + vector reservations each (≈1800/frame, slow musl malloc). They live
// until the slot's fence like before, then return to a free list with their capacity.
std::vector<std::unique_ptr<Descriptors>> desc_all;std::vector<Descriptors*> desc_free;
Descriptors &acquire_descriptors(){
    Descriptors *d;if(desc_free.empty()){desc_all.push_back(std::make_unique<Descriptors>());d=desc_all.back().get();}else{d=desc_free.back();desc_free.pop_back();}
    recording_slot().desc_used.push_back(d);return *d;
}
void release_descriptors(Slot &s){
    for(Descriptors *d:s.desc_used){for(auto x:d->buffer_views)vkDestroyBufferView(cfg.device,x,nullptr);
        d->entry=nullptr;d->set=VK_NULL_HANDLE;d->pipeline_layout=VK_NULL_HANDLE;d->dynamic_offsets.clear();d->buffers.clear();d->buffer_views.clear();d->storage.clear();d->written.clear();d->capture_inputs.clear();
        desc_free.push_back(d);}
    s.desc_used.clear();
}
// Fast path (iOS: samplers fetched directly instead of re-deriving per draw): raw NVN parameters +
// view format -> VkSampler, before the full conversion/validation below.
VkSampler sampler_slow(const samp_par &p,Image &im,VkFormat viewed,bool integer_border);
VkSampler sampler(const samp_par &p,Image &im,VkFormat viewed,bool integer_border){
    auto bits=[](float f){uint32_t u;memcpy(&u,&f,4);return u;};
    SamplerFastKey k{{uint32_t(p.minf)|uint32_t(p.magf)<<8|uint32_t(p.ws)<<16|uint32_t(p.wt)<<24,uint32_t(p.wr)|uint32_t(p.cmp_mode)<<8|uint32_t(p.cmp_func)<<16|uint32_t(integer_border)<<24,
        bits(p.lod_min),bits(p.lod_max),bits(p.lod_bias),bits(p.aniso),bits(p.border[0]),bits(p.border[1]),bits(p.border[2]),bits(p.border[3]),uint32_t(viewed)}};
    auto it=sampler_fast.find(k);if(it!=sampler_fast.end())return it->second;
    VkSampler out=sampler_slow(p,im,viewed,integer_border);sampler_fast.emplace(k,out);return out;
}
VkSampler sampler_slow(const samp_par &p,Image &im,VkFormat viewed,bool integer_border){
    xbg_nvn_sampler_desc input{p.minf,p.magf,p.ws,p.wt,p.wr,p.cmp_mode,p.cmp_func,p.lod_min,p.lod_max,p.lod_bias,p.aniso,{p.border[0],p.border[1],p.border[2],p.border[3]}};
    xbg_sampler_limits limits{cfg.enabled_core.samplerAnisotropy,sampler_mirror_clamp_enabled,VkBool32(integer_border),properties.limits.maxSamplerAnisotropy,properties.limits.maxSamplerLodBias};
    VkSamplerCreateInfo s{};int result=xbg_sampler_from_nvn(&input,&limits,&s);
    if(result)xbg_fatalf("NVN sampler %s result=%d min=%u mag=%u wrap=%u/%u/%u lod=%g/%g bias=%g aniso=%g compare=%u/%u border=%g,%g,%g,%g texture=%llu format=%#x vk=%u mirrorEnabled=%u",xbg_sampler_error(result),result,p.minf,p.magf,p.ws,p.wt,p.wr,p.lod_min,p.lod_max,p.lod_bias,p.aniso,p.cmp_mode,p.cmp_func,p.border[0],p.border[1],p.border[2],p.border[3],(unsigned long long)im.source.serial,im.source.d.format,unsigned(viewed),sampler_mirror_clamp_enabled);
    const VkFormatProperties &fp=format_properties(viewed);
    if((s.minFilter==VK_FILTER_LINEAR || s.magFilter==VK_FILTER_LINEAR) && !(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))xbg_fatalf("NVN sampler linear filtering unsupported min=%u mag=%u compare=%u/%u format=%#x vk=%u optimalFeatures=%#x texture=%llu decoded=%d imfmt=%u",p.minf,p.magf,p.cmp_mode,p.cmp_func,im.source.d.format,unsigned(viewed),fp.optimalTilingFeatures,(unsigned long long)im.source.serial,int(im.decoded),unsigned(im.fmt));
    need(s.pNext==nullptr,"Unexpected sampler extension chain");
    auto bits=[](float f){uint32_t u;memcpy(&u,&f,4);return u;};
    std::array<uint32_t,16> key{s.flags,uint32_t(s.magFilter),uint32_t(s.minFilter),uint32_t(s.mipmapMode),uint32_t(s.addressModeU),uint32_t(s.addressModeV),uint32_t(s.addressModeW),bits(s.mipLodBias),s.anisotropyEnable,bits(s.maxAnisotropy),s.compareEnable,uint32_t(s.compareOp),bits(s.minLod),bits(s.maxLod),uint32_t(s.borderColor),s.unnormalizedCoordinates};
    auto it=sampler_cache.find(key);if(it!=sampler_cache.end())return it->second;
    VkSampler out;VK(vkCreateSampler(cfg.device,&s,nullptr,&out));sampler_cache.emplace(key,out);return out;
}
void fill_driver(uint8_t *data,unsigned ns,unsigned bank){
    if(bank==0){unsigned base=ns==0?0x110:ns==5?0x310:ns==1?0x510:0;need(base,"Unverified storage-table base for tessellation/geometry stage");for(unsigned r=0;r<16;r++){memcpy(data+base+16*r,&state.sb[ns][r].address,8);uint32_t n=uint32_t(state.sb[ns][r].size);memcpy(data+base+16*r+8,&n,4);}if(ns==5)for(unsigned r=0;r<16;r++){memcpy(data+0x210+16*r,&state.ub[ns][r].address,8);uint32_t n=uint32_t(state.ub[ns][r].size);memcpy(data+0x218+16*r,&n,4);}}
    else{for(unsigned r=0;r<128;r++){memcpy(data+0x168+8*r,&state.tex[ns][r],8);memcpy(data+0x568+8*r,&state.samp[ns][r],8);}for(unsigned r=0;r<9;r++)memcpy(data+0x120+8*r,&state.img[ns][r],8);}
}
// Uniform data is bound with the guest-bound size (rounded to 16 bytes); reads
// past it rely on robustBufferAccess like the reference Ryujinx Vulkan backend.
size_t uniform_range(size_t copy){return std::max<size_t>(16,(copy+15)&~size_t(15));}
Buffer uniform_snapshot_slow(uint64_t address,size_t copy);
Buffer uniform_snapshot_cached(uint64_t address,size_t copy);
Buffer uniform_snapshot(uint64_t address,size_t copy){
    if(g_ui_moff!=0 && address==g_ui_moff_addr && copy>=64){   // 界面下移：投影矩阵另拷一份加纵向偏移，不进快照缓存 | UI shift: a separate copy of the projection matrix with a vertical offset, kept out of the snapshot caches
        Buffer src=uniform_snapshot_cached(address,copy);Buffer b=stream_buffer(uniform_range(copy),src.mapped,copy);
        float m[16],o=float(g_ui_moff);memcpy(m,b.mapped,64);m[1]+=o*m[3];m[5]+=o*m[7];m[9]+=o*m[11];m[13]+=o*m[15];memcpy(b.mapped,m,64);b.flush();return b;}
    return uniform_snapshot_cached(address,copy);
}
Buffer uniform_snapshot_cached(uint64_t address,size_t copy){CPU_SCOPE(Snapshot);
    SnapFront &f=snap_front_for(1,address,copy);if(f.epoch==snapshot_epoch && f.address==address && f.bytes==copy){++perf.snapshot_hits;++snap_front_hits;return f.buffer;}
    Buffer b=uniform_snapshot_slow(address,copy);f.address=address;f.bytes=copy;f.epoch=snapshot_epoch;f.buffer=b;return b;
}
Buffer uniform_snapshot_slow(uint64_t address,size_t copy){
    size_t n=uniform_range(copy);auto key=std::make_pair(address,uint64_t(copy));
    auto it=uniform_snapshots.find(key);if(it!=uniform_snapshots.end()){++perf.snapshot_hits;++snap_map_hits;return it->second;}
    {Buffer view;if(pending_view(address,n,properties.limits.minUniformBufferOffsetAlignment,view)){uniform_snapshots.emplace(key,view);snapshot_maxlen=std::max<uint64_t>(snapshot_maxlen,n);return view;}}
    ++perf.snapshot_misses;perf.snapshot_bytes+=copy;snap_site_bytes[5]+=copy;snap_site_req[5]+=copy;cpu_access(address,copy,2);
    Buffer b=stream_buffer(n,reinterpret_cast<const void*>(address),copy,true);uniform_snapshots.emplace(key,b);snapshot_maxlen=std::max<uint64_t>(snapshot_maxlen,copy);return b;
}
// Descriptor memo (iOS "same inputs -> reuse the parameter block"): within one guest submit, a draw whose shaders and
// bound inputs (buffer ranges, texture/sampler handles, pools, driver generations, viewport) equal an earlier draw's
// reuses that draw's set and dynamic offsets. Snapshots are per-submit too, so the bound data is the same. On a hit
// every input image still gets its pending CPU upload and read barrier. Not used for compute, written storage,
// texel buffers or storage images. files/gfx-no-descmemo disables it.
// Per-submit resolved sampled-image descriptors: (texture handle, sampler handle, pools, binding kind) -> image, view,
// sampler. A hit still performs the pending CPU upload and the read barrier for the image.
struct ImgKey{uint64_t h,sh,tp,to,sp,so,kind;bool operator==(const ImgKey &o)const{return h==o.h&&sh==o.sh&&tp==o.tp&&to==o.to&&sp==o.sp&&so==o.so&&kind==o.kind;}};
struct ImgKeyHash{size_t operator()(const ImgKey &k)const{uint64_t x=k.h*0x9E3779B97F4A7C15ull^k.sh*0xC2B2AE3D27D4EB4Full^k.tp^(k.to<<17)^(k.sp<<7)^k.so^(k.kind<<40);x^=x>>29;return size_t(x*0xBF58476D1CE4E5B9ull);}};
struct ImgEntry{const tex_t *tex;Image *im;VkImageView view;VkSampler sampler;const RegisteredTexture *record;uint64_t revision;};
std::unordered_map<ImgKey,ImgEntry,ImgKeyHash> img_cache;bool img_cache_enabled=true;uint64_t img_cache_hits=0,img_cache_misses=0;
// Recreate an image created without MUTABLE_FORMAT with it, copy every level and layer, and retire the old image and its
// views (queue order keeps earlier recorded uses on the old one). Cached descriptor sets and the image cache hold the old
// views: memo_epoch and img_cache drop them. Field counts on Metal: 0 of 17473 textures needed this (nvn_mtl.m).
void upgrade_mutable(Image &im){
    VkImageCreateInfo ci=im.create;ci.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImage image{};VK(vkCreateImage(cfg.device,&ci,nullptr,&image));VkMemoryRequirements r;vkGetImageMemoryRequirements(cfg.device,image,&r);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;ai.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory memory{};VK(vkAllocateMemory(cfg.device,&ai,nullptr,&memory));VK(vkBindImageMemory(cfg.device,image,memory,0));
    Commands c;   // non-draw: ends an open pass, full barriers before and after
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;b.image=image;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange={im.aspects,0,im.levels,0,im.layers};b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    bool copied=im.layout!=VK_IMAGE_LAYOUT_UNDEFINED;   // nothing defined to copy yet
    if(copied){im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);std::vector<VkImageCopy> regions;
        for(uint32_t l=0;l<im.levels;l++){VkImageCopy rc{};rc.srcSubresource={im.aspects,l,0,im.layers};rc.dstSubresource=rc.srcSubresource;rc.extent={std::max(1u,im.width>>l),std::max(1u,im.height>>l),std::max(1u,im.depth>>l)};regions.push_back(rc);}
        vkCmdCopyImage(c.command,im.image,VK_IMAGE_LAYOUT_GENERAL,image,VK_IMAGE_LAYOUT_GENERAL,uint32_t(regions.size()),regions.data());}
    c.finish();
    VkImage old_image=im.image;VkDeviceMemory old_memory=im.memory;std::vector<VkImageView> old_views;for(auto &v:im.views)old_views.push_back(v.second);im.views.clear();
    std::vector<VkFramebuffer> old_fbs;forget_framebuffers(im,&old_fbs);
    retire_handle([old_image,old_memory,old_views,old_fbs]{for(auto f:old_fbs)vkDestroyFramebuffer(cfg.device,f,nullptr);for(auto v:old_views)vkDestroyImageView(cfg.device,v,nullptr);vkDestroyImage(cfg.device,old_image,nullptr);vkFreeMemory(cfg.device,old_memory,nullptr);});
    im.image=image;im.memory=memory;im.create=ci;im.mutable_format=true;im.layout=VK_IMAGE_LAYOUT_GENERAL;
    ++memo_epoch;img_cache.clear();++mutable_upgrades;last_pass_reset();
    xbg_logf("NVN mutable upgrade serial=%llu fmt=%d %ux%ux%u levels=%u layers=%u copied=%d total=%llu",(unsigned long long)im.source.serial,int(im.fmt),im.width,im.height,im.depth,im.levels,im.layers,int(copied),(unsigned long long)mutable_upgrades);
}
struct DescMemo{const LayoutEntry *entry;VkDescriptorSet set;VkPipelineLayout pipeline_layout;std::vector<uint32_t> dynamic_offsets;std::vector<MemoImage> images;struct Dyn{uint64_t order;VkBuffer buffer;uint32_t offset;bool uniform;uint8_t src=0,ns=0,bank=0;};std::vector<Dyn> dyn;bool persistent=false;std::vector<std::pair<uint64_t,const void*>> tex_ids;struct Stor{uint64_t base,bytes;VkBuffer buffer;VkDeviceSize offset;};std::vector<Stor> stor;
    struct Texel{const RegisteredTexture *record;uint64_t revision,base,bytes,gen;uint32_t format;};std::vector<Texel> texel;uint64_t checked_gen=0;size_t reserve=0;};   // checked_gen: memo_check_gen at its last passed image checks   // texel: resident uniform texel buffers (texel_acquire)
struct DynUbo{uint64_t order,address;size_t copy;};
// Cross-frame memo (Claude C147): the memo lives per frame slot and survives submits. An entry is persistent when its set
// holds no per-frame snapshot except dynamic uniform buffers (re-snapshotted on every hit, only the offset moves) and the
// support / driver parameter blocks (recomputed on every hit, buffer must be the same arena buffer). Entries with storage
// snapshots stay frame-local (cleared at submit, as before). Persistent sets are pinned out of the slot's set recycling;
// Texture registrations use per-record revisions (checked on every hit); sampler changes bump memo_epoch, which drops
// every slot's memo lazily and returns pinned sets to that slot (recycled only after its next fence).
// files/gfx-no-xmemo keeps the old per-submit behaviour.
std::unordered_map<std::vector<uint64_t>,DescMemo,SetKeyHash> desc_memo_slot[2];std::unordered_map<VkDescriptorSet,PinRef> memo_pinned[2];
size_t desc_memo_local[2]={};   // entries that are not persistent, which desc_memo_reset drops
std::atomic<uint64_t> memo_epoch{1};bool dyn_partial_enabled=true;uint64_t key_diff[14]={};uint64_t memo_bump[5]={};uint64_t memo_slot_epoch[2]={1,1};bool xmemo_enabled=true,memo_dyn_ok=false;uint64_t xmemo_hits=0,xmemo_drops=0;
#define desc_memo desc_memo_slot[slot_index]
// Front of the memo lookup: a draw whose key equals the key last found reuses that entry without hashing the key and
// walking its bucket. Any change that can erase an entry or rehash the map (clear, sweep, erase, insert) moves
// memo_mod_gen, which drops the front. A/B target memofront looks every key up.
std::vector<uint64_t> memo_front_key;std::unordered_map<std::vector<uint64_t>,DescMemo,SetKeyHash>::iterator memo_front_it;
uint64_t memo_mod_gen=1,memo_front_gen=0,memo_front_hits=0;unsigned memo_front_slot=~0u;
uint64_t desc_ns[6]={};bool desc_memo_enabled=true;uint64_t desc_memo_hits=0,desc_memo_misses=0,desc_memo_why[6]={};
std::vector<MemoImage> desc_memo_images;std::vector<DescMemo::Dyn> desc_memo_dyn;bool desc_memo_recording=false;
std::vector<DescMemo::Texel> desc_memo_texel;bool desc_memo_texel_fail=false;
void descriptors_build(Descriptors &out,Commands &commands,bool compute);
// Dynamic uniform buffers (Claude C151): Maleoon allows 8 per pipeline layout. Mode 1 = every uniform buffer dynamic (fits);
// mode 2 = only the per-draw ones (guest uniform buffers, driver blocks) dynamic, the support block (depends on the viewport
// only, kept in the static arena) and c1 constants (static) bound at fixed offsets; mode 0 = none dynamic.
#define UBO_DYN(mode,b) ((b).type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && ((mode)==1 || ((mode)==2 && ((b).source==XBG_SOURCE_UNIFORM||(b).source==XBG_SOURCE_DRIVER0||(b).source==XBG_SOURCE_DRIVER2))))
unsigned dyn_mode_for(bool compute,bool *no_ubo=nullptr){unsigned all=0,vary=0;
    for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute)for(auto &b:state.shader[st]->bindings)if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER){all+=b.count;if(b.source==XBG_SOURCE_UNIFORM||b.source==XBG_SOURCE_DRIVER0||b.source==XBG_SOURCE_DRIVER2)vary+=b.count;}
    if(no_ubo)*no_ubo=all==0;unsigned lim=properties.limits.maxDescriptorSetUniformBuffersDynamic;
    return all && all<=lim?1:(vary && vary<=lim && dyn_partial_enabled?2:0);}
void memo_unpin_all(unsigned si){Slot &s=slots[si];for(auto &pn:memo_pinned[si])s.used_sets[pn.second.layout].push_back(pn.first);memo_pinned[si].clear();desc_memo_slot[si].clear();desc_memo_local[si]=0;++memo_mod_gen;}
void memo_check_epoch(){unsigned si=slot_index;if(memo_slot_epoch[si]!=memo_epoch || memo_pinned[si].size()>8192){if(!desc_memo_slot[si].empty())++xmemo_drops;memo_unpin_all(si);memo_slot_epoch[si]=memo_epoch;}}
// End of a submit for the recording slot: frame-local entries go (their snapshots are per submit); persistent ones stay.
void desc_memo_reset(){img_cache.clear();
    if(!xmemo_enabled){memo_unpin_all(slot_index);return;}
    // Sweep only when the slot holds frame-local entries: walking its ~600-800 persistent entries every submission to find
    // none was ~1% of the D2R camp's render thread on the Redmi K50 Ultra.
    if(!desc_memo_local[slot_index])return;
    auto &m=desc_memo_slot[slot_index];for(auto it=m.begin();it!=m.end();){if(it->second.persistent)++it;else it=m.erase(it);}desc_memo_local[slot_index]=0;++memo_mod_gen;}
Buffer &support_block(){size_t n=1392;auto k=std::make_pair(state.viewport.width,state.viewport.height);
    auto it=support_static.find(k);if(it!=support_static.end())return it->second;
    Buffer s=static_arena.alloc(uniform_range(n));xbg_support_defaults(s.mapped,state.viewport.width,state.viewport.height);s.flush();return support_static.emplace(k,s).first->second;}
Buffer &driver_block(unsigned ns,unsigned bank){uint64_t gen=bank?tex_gen[ns]:buf_gen[ns];
    DriverCache &dc=driver_cache[ns][bank];if(dc.slot!=slot_serial || dc.gen!=gen || !dc.slot){Buffer s=stream_buffer(0x1000);fill_driver(static_cast<uint8_t*>(s.mapped),ns,bank?2:0);s.flush();dc.gen=gen;dc.slot=slot_serial;dc.buffer=s;}
    return dc.buffer;}
// Uniform texel buffers held in resident per-slot copies, so descriptor sets that bind them can be memoized across frames
// (D2R camp: 558 of 2188 draws per frame bind one and were rebuilt every time, ~21 us each against ~2.5 us for a memo hit).
// A copy belongs to one slot (refreshed only while that slot records, after its previous submission retired) and is
// refreshed from guest memory when its content hash changes, before its first read in a frame; content that changes again
// within the frame falls back to the per-frame snapshot. A memo entry checks the copy's generation, so an evicted and
// recreated copy is never mistaken for the one its set holds. gfx-no-texel-memo keeps the former per-frame path.
TexelResident *texel_acquire(uint64_t base,uint64_t bytes,VkFormat fmt){
    if(!bytes || base>UINT64_MAX-bytes || bytes>64ull*1024*1024){++texel_fallbacks;return nullptr;}
    // 待写回的数据走原快照路径，不允许驻留命中绕过 GPU 写入或跨槽排空。
    // Pending results use the original snapshot path; resident hits must not bypass GPU writes or drain across slots.
    // 任何重叠都算：没有 read_preflight 时，这里可能还有只覆盖一部分范围的待写回。
    // Any overlap counts: without read_preflight a pending writeback can cover only part of the range here.
    for(auto &w:pending_writebacks)if(xbg_wb_overlaps(base,bytes,w.addr,w.bytes)){++texel_fallbacks;return nullptr;}
    for(auto &r:pending_reports)if(base<r.addr+16 && base+bytes>r.addr){++texel_fallbacks;return nullptr;}
    for(auto *list:{&pending_events,&recording_events})for(auto &e:*list)if(base<e.addr+4 && base+bytes>e.addr){++texel_fallbacks;return nullptr;}
    unsigned si=slot_index;need(slots[si].recording && !slots[si].pending,"Texel resident requires a retired recording slot");auto &map=texel_resident[si];TexelKey key{base,bytes,uint32_t(fmt)};
    auto it=map.find(key);
    if(it!=map.end() && it->second.checked==slot_serial && it->second.check_epoch==texel_write_epoch){it->second.used=slot_serial;return &it->second;}
    uint64_t h=hash_memory(reinterpret_cast<const void*>(uintptr_t(base)),size_t(bytes));
    if(it==map.end()){
        if(map.size()>=8192 || texel_live_bytes[si]+texel_hole_bytes[si]+bytes>64ull*1024*1024){++texel_fallbacks;return nullptr;}
        TexelResident r;r.buffer=texel_arena[si].alloc(size_t(bytes));
        VkBufferViewCreateInfo v{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};v.buffer=r.buffer.buffer;v.offset=r.buffer.offset;v.format=fmt;v.range=bytes;VK(vkCreateBufferView(cfg.device,&v,nullptr,&r.view));
        memcpy(r.buffer.mapped,reinterpret_cast<const void*>(uintptr_t(base)),size_t(bytes));r.buffer.flush();r.hash=h;r.used=slot_serial;r.gen=++texel_gen;++texel_refreshes;
        r.checked=slot_serial;r.check_epoch=texel_write_epoch;texel_lo=std::min(texel_lo,base);texel_hi=std::max(texel_hi,base+bytes);
        texel_live_bytes[si]+=bytes;return &map.emplace(key,r).first->second;}
    TexelResident &r=it->second;
    if(r.hash!=h){if(r.used==slot_serial){++texel_fallbacks;return nullptr;}   // 本帧已读取旧内容 | This frame already reads earlier contents
        memcpy(r.buffer.mapped,reinterpret_cast<const void*>(uintptr_t(base)),size_t(bytes));r.buffer.flush();r.hash=h;++texel_refreshes;}
    r.used=slot_serial;r.checked=slot_serial;r.check_epoch=texel_write_epoch;return &r;
}
void texel_touch(uint64_t address,uint64_t bytes){
    auto &map=texel_resident[slot_index];if(map.size()>64){++texel_write_epoch;return;}   // many copies: invalidate coarsely
    for(auto &e:map)if(address<e.first.base+e.first.bytes && address+bytes>e.first.base){++texel_write_epoch;return;}
}
// While a slot starts recording (its previous submission retired): drop copies unused for ~128 submissions; when the holes they
// leave in the slot's arena pass 8 MB, start the slot's store over (memo entries then fail their generation check).
void texel_evict(unsigned si,bool all){
    need(!slots[si].pending && (!slots[si].recording || !slots[si].ops),"Evict texel residents only after slot retirement");
    auto &map=texel_resident[si];
    for(auto it=map.begin();it!=map.end();){if(all || it->second.used+128<slot_serial){vkDestroyBufferView(cfg.device,it->second.view,nullptr);texel_hole_bytes[si]+=it->first.bytes;texel_live_bytes[si]-=it->first.bytes;it=map.erase(it);++texel_evictions;}else ++it;}
    if(all || texel_hole_bytes[si]>8ull*1024*1024){for(auto &e:map)vkDestroyBufferView(cfg.device,e.second.view,nullptr);map.clear();texel_arena[si].release();texel_hole_bytes[si]=texel_live_bytes[si]=0;}
}
void descriptors(Descriptors &out,Commands &commands,bool compute){
    need(!read_preflight || (pending_writebacks.empty() && pending_reports.empty()),"Unresolved CPU reads at descriptor entry");
    uint64_t tm0=perf_clock();struct MemoClock{uint64_t t0;~MemoClock(){}} mc{tm0};
    static std::vector<uint64_t> key;key.clear();static std::vector<uint8_t> ktag;ktag.clear();
    // ktag (which part of the key each word is) only feeds the keydiff census, so it is filled on timed frames only (one in
    // perf_sample); a miss without tags skips the census. A/B target ktag tags every frame again.
    const bool tags=timing_now()!=nullptr || ab_off("ktag");
    bool ok=desc_memo_enabled && !compute && !capture.active && !provenance_file && g_ui_moff==0;
    if(ok){CPU_SCOPE(Descriptor);
        static std::vector<DynUbo> dyn_ubos;dyn_ubos.clear();
        bool no_ubo=false;const unsigned dmode=dyn_mode_for(false,&no_ubo);memo_dyn_ok=dmode!=0 || no_ubo;
        for(unsigned st=0;st<5;st++){key.push_back(uint64_t(uintptr_t(state.shader[st])));if(tags)ktag.push_back(0);}
        key.push_back(state.texture_pool.pool?uint64_t(uintptr_t(state.texture_pool.pool->mem)):0);key.push_back(uint64_t(state.texture_pool.off));key.push_back(uint64_t(state.texture_pool.n));if(tags)ktag.insert(ktag.end(),3,1);
        key.push_back(state.sampler_pool.pool?uint64_t(uintptr_t(state.sampler_pool.pool->mem)):0);key.push_back(uint64_t(state.sampler_pool.off));if(tags)ktag.insert(ktag.end(),2,1);
        for(unsigned st=0;st<5 && ok;st++)if(state.shader[st]){Shader &shader=*state.shader[st];unsigned ns=nvn_stage[st];
            for(auto &b:shader.bindings){
                // Texel view 属于 slot，退休时销毁；memo 不追踪其快照或生命周期，不能保留旧 set。
                // Slot retirement destroys texel views; memo tracks neither their snapshots nor lifetime, so exclude both kinds.
                if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || (b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && !texel_memo_enabled)){ok=false;++desc_memo_why[0];break;}
                if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE){ok=false;++desc_memo_why[1];break;}
                if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && shader.storage_writes.count(b.binding)){ok=false;++desc_memo_why[2];break;}
                for(unsigned element=0;element<b.count;element++){unsigned slot=b.slot+element;
                    switch(b.source){
                    case XBG_SOURCE_UNIFORM:case XBG_SOURCE_STORAGE:{if(slot>=16){ok=false;break;}auto r=b.source==XBG_SOURCE_UNIFORM?state.ub[ns][slot]:state.sb[ns][slot];
                        if(UBO_DYN(dmode,b) && b.source==XBG_SOURCE_UNIFORM && r.address && r.size){size_t copy=std::min<size_t>(r.size,b.bytes?b.bytes:65536);key.push_back(copy);if(tags)ktag.push_back(2);dyn_ubos.push_back({(uint64_t(b.binding)<<32)|element,r.address,copy});} // address only moves the dynamic offset
                        else{key.push_back(r.address);key.push_back(r.size);if(tags){ktag.push_back(b.source==XBG_SOURCE_UNIFORM?3:10);ktag.push_back(4);}}break;}
                    case XBG_SOURCE_SUPPORT:key.push_back((uint64_t(uint32_t(state.viewport.width))<<32)|uint32_t(state.viewport.height));key.push_back(state.color.alpha_func);if(tags){ktag.push_back(5);ktag.push_back(6);}break;
                    case XBG_SOURCE_C1:break;
                    case XBG_SOURCE_DRIVER0:case XBG_SOURCE_DRIVER2:key.push_back(UBO_DYN(dmode,b)?uint64_t(0xD0D0+b.source):(b.source==XBG_SOURCE_DRIVER0?tex_gen[ns]:buf_gen[ns]));if(tags)ktag.push_back(7);break; // dynamic: block rebuilt from the current state on every hit (driver_block); otherwise its offset is baked into the set
                    case XBG_SOURCE_TEXTURE:case XBG_SOURCE_IMAGE:{if(slot>=(b.source==XBG_SOURCE_IMAGE?16u:128u)){ok=false;break;}key.push_back(b.source==XBG_SOURCE_IMAGE?state.img[ns][slot]:state.tex[ns][slot]);if(tags)ktag.push_back(8);
                        if(b.sampler_slot!=UINT32_MAX){if(b.sampler_slot>=128){ok=false;break;}key.push_back(state.samp[ns][b.sampler_slot]);if(tags)ktag.push_back(9);}break;}
                    default:ok=false;}
                    if(!ok)break;
                }
                if(!ok)break;
            }}
        if(ok){memo_check_epoch();auto hit=desc_memo.end();
            if(memo_front_gen==memo_mod_gen && memo_front_slot==slot_index && !ab_off("memofront") && memo_front_key==key){hit=memo_front_it;++memo_front_hits;}
            else{hit=desc_memo.find(key);if(hit!=desc_memo.end()){memo_front_key=key;memo_front_it=hit;memo_front_gen=memo_mod_gen;memo_front_slot=slot_index;}}
            if(hit==desc_memo.end() && ktag.size()==key.size()){   // diagnostic: which key part differs from the last key of the same shaders
                static std::unordered_map<std::vector<uint64_t>,std::vector<uint64_t>,SetKeyHash> last;std::vector<uint64_t> pre(key.begin(),key.begin()+5);
                auto &prev=last[pre];if(prev.size()==key.size()){bool any=false;for(size_t i=0;i<key.size();i++)if(prev[i]!=key[i]){++key_diff[ktag[i]<12?ktag[i]:11];any=true;}if(!any)++key_diff[11];}else ++key_diff[prev.empty()?12:13];
                prev=key;}
            if(hit!=desc_memo.end()){DescMemo &m=hit->second;
                static std::vector<std::pair<uint64_t,Buffer>> fresh;fresh.clear();
                for(auto &u:dyn_ubos)fresh.push_back({u.order,uniform_snapshot(u.address,u.copy)});
                std::sort(fresh.begin(),fresh.end(),[](const std::pair<uint64_t,Buffer> &a,const std::pair<uint64_t,Buffer> &b){return a.first<b.first;});
                static std::vector<uint32_t> offsets;offsets.clear();bool same=true;size_t fi=0;
                // Room for keep() below (support / driver blocks) and draw()'s vertex/index buffers. The count of the shaders' bindings
                // is stored in the entry when it is made: the shaders are part of the key. A/B target descreserve counts them again.
                if(ab_off("descreserve")){size_t elements=0;for(unsigned st=0;st<5;st++)if(state.shader[st])for(auto &b:state.shader[st]->bindings)elements+=b.count;out.buffers.reserve(elements*2+40);}
                else out.buffers.reserve(m.reserve);
                for(auto &d:m.dyn){if(!d.uniform){
                        if(d.src==XBG_SOURCE_SUPPORT || d.src==XBG_SOURCE_DRIVER0 || d.src==XBG_SOURCE_DRIVER2){
                            Buffer &blk=d.src==XBG_SOURCE_SUPPORT?support_block():driver_block(d.ns,d.bank);if(blk.buffer!=d.buffer){same=false;break;}out.keep(blk);offsets.push_back(uint32_t(blk.offset));}
                        else offsets.push_back(d.offset);continue;}
                    if(fi>=fresh.size() || fresh[fi].first!=d.order || fresh[fi].second.buffer!=d.buffer){same=false;break;}offsets.push_back(uint32_t(fresh[fi].second.offset));++fi;}
                same=same && fi==fresh.size();
                // 每个缓存项均核注册版本（包括 view）和存活图像；仅重建失效项。
                // Every cached entry checks registration revision (including view) and live image identity.
                // Repeated only when memo_check_gen moved since this entry's last pass: re-reading every record was ~12% of
                // descriptors() in the D2R camp (a scattered load per image per draw), and in play no registration changes.
                // A/B target hitgen repeats them on every hit, as before.
                if(same && (m.checked_gen!=memo_check_gen || ab_off("hitgen"))){
                    for(size_t k=0;k<m.images.size();k++){const auto &cached=m.images[k];const tex_t *t=cached.first;
                        if(cached.record->revision!=cached.revision){same=false;break;}
                        if(t->serial!=m.tex_ids[k].first || t->mem!=m.tex_ids[k].second){same=false;break;}
                        if(find_image(t->serial)!=m.images[k].second){same=false;break;}}
                    if(same)m.checked_gen=memo_check_gen;}
                // Resident texel copies: same registration and range, the same copy the set holds (generation), content
                // refreshed if the guest changed it since this slot last recorded.
                if(same)for(auto &tr:m.texel){const tex_t &tt=tr.record->texture;
                    if(tr.record->revision!=tr.revision || uint64_t(uintptr_t(tt.mem))!=tr.base || uint64_t(tt.L.size)!=tr.bytes){same=false;break;}
                    TexelResident *r=texel_acquire(tr.base,tr.bytes,VkFormat(tr.format));if(!r || r->gen!=tr.gen){same=false;break;}++texel_hits;}
                // Read-only storage ranges: refresh the snapshot (write-tracked resident copies only copy dirty pages); the set
                // stays valid only if the range lands in the same buffer at the same offset as when the set was written.
                if(same && m.persistent)for(auto &st:m.stor){snap_site=3;Buffer b=snapshot(st.base,st.bytes,properties.limits.minStorageBufferOffsetAlignment);snap_site=0;
                    if(b.buffer!=st.buffer || b.offset!=st.offset){same=false;break;}}
                if(same){
                // 命中也走既有按 submit 节流的 CPU 内容复核，不强制重传。
                // Hits retain get_image's bounded per-submit CPU checks, without forcing uploads.
                for(auto &ti:m.images){Image &im=get_image(ti.first,true);prepare_read(im,commands.command);}
                out.entry=m.entry;out.set=m.set;out.pipeline_layout=m.pipeline_layout;out.dynamic_offsets=offsets;++desc_memo_hits;if(m.persistent)++xmemo_hits;desc_ns[5]+=perf_clock()-tm0;return;}
                ++desc_memo_why[3];
                if(m.persistent){auto pin=memo_pinned[slot_index].find(m.set);if(pin!=memo_pinned[slot_index].end() && --pin->second.refs==0){slots[slot_index].used_sets[pin->second.layout].push_back(m.set);memo_pinned[slot_index].erase(pin);}}
                if(!m.persistent && desc_memo_local[slot_index])--desc_memo_local[slot_index];desc_memo.erase(hit);++memo_mod_gen;}}
    }
    desc_ns[0]+=perf_clock()-tm0;
    desc_memo_images.clear();desc_memo_dyn.clear();desc_memo_texel.clear();desc_memo_texel_fail=false;desc_memo_recording=ok;
    {uint64_t tb=perf_clock();descriptors_build(out,commands,compute);desc_ns[4]+=perf_clock()-tb;}
    desc_memo_recording=false;
    if(ok && desc_memo_texel_fail){ok=false;++desc_memo_why[0];}
    if(ok && !(out.written.empty() && out.buffer_views.empty() && out.set && out.dynamic_offsets.size()==desc_memo_dyn.size()))++desc_memo_why[4];
    if(ok && out.written.empty() && out.buffer_views.empty() && out.set && out.dynamic_offsets.size()==desc_memo_dyn.size()){++desc_memo_misses;
        std::sort(desc_memo_dyn.begin(),desc_memo_dyn.end(),[](const DescMemo::Dyn &a,const DescMemo::Dyn &b){return a.order<b.order;});
        DescMemo dm{out.entry,out.set,out.pipeline_layout,out.dynamic_offsets,desc_memo_images,desc_memo_dyn};dm.texel=desc_memo_texel;
        {size_t elements=0;for(unsigned st=0;st<5;st++)if(state.shader[st])for(auto &b:state.shader[st]->bindings)elements+=b.count;dm.reserve=elements*2+40;}
        dm.persistent=xmemo_enabled && memo_dyn_ok && out.written.empty();
        for(auto &st:out.storage)dm.stor.push_back({st.first.first,st.first.second,st.second->buffer,st.second->offset});
        for(auto &ti:dm.images)dm.tex_ids.push_back({ti.first->serial,ti.first->mem});
        for(auto &d:dm.dyn)if(!d.uniform && d.src!=XBG_SOURCE_SUPPORT && d.src!=XBG_SOURCE_DRIVER0 && d.src!=XBG_SOURCE_DRIVER2 && d.src!=XBG_SOURCE_C1)dm.persistent=false;
        if(dm.persistent){unsigned si=slot_index;VkDescriptorSetLayout lay=out.entry->layout;
            auto pin=memo_pinned[si].find(out.set);if(pin!=memo_pinned[si].end())++pin->second.refs;
            else{auto &u=slots[si].used_sets[lay];for(size_t i=0;i<u.size();i++)if(u[i]==out.set){u[i]=u.back();u.pop_back();break;}memo_pinned[si].emplace(out.set,PinRef{lay,1});}}
        bool local=!dm.persistent;if(desc_memo.emplace(key,std::move(dm)).second && local)++desc_memo_local[slot_index];++memo_mod_gen;}
}
void descriptors_build(Descriptors &out,Commands &commands,bool compute){CPU_SCOPE(Descriptor);DescriptorRoot descriptor_measurement(&cpu_scope);
    // Uniform buffers use dynamic offsets (when the set fits the device limit) so per-draw constants do not change set contents.
    const unsigned dmode=dyn_mode_for(compute);
    // Aliased NVN storage ranges must refer to the same VkBuffer allocation.
    // Snapshotting each binding separately loses writes observed through aliases.
    {DESC_SCOPE(Setup);
    struct Range{uint64_t begin,end;bool written;};static std::vector<Range> ranges,merged;static std::vector<uint32_t> key;ranges.clear();merged.clear();key.clear();
    {size_t elements=0;for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute)for(auto &b:state.shader[st]->bindings)elements+=b.count;out.buffers.reserve(elements*2+40);}
    std::array<const void*,7> shader_key{};for(unsigned st=0;st<6;st++)if((st==5)==compute)shader_key[st]=state.shader[st];shader_key[6]=reinterpret_cast<const void*>(uintptr_t(compute));
    for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute)for(auto &b:state.shader[st]->bindings){
        need(b.count && b.count<=128 && b.type<=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,"Unsupported descriptor array/type");
        key.push_back(b.binding);key.push_back(UBO_DYN(dmode,b)?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:b.type);key.push_back(b.count);key.push_back(1u<<st);
        if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && (b.source==XBG_SOURCE_STORAGE || b.source==XBG_SOURCE_UNIFORM)){bool written=state.shader[st]->storage_writes.count(b.binding)!=0;for(unsigned i=0;i<b.count;i++){
            need(b.slot+i<16,"Storage descriptor slot out of bounds");auto r=b.source==XBG_SOURCE_STORAGE?state.sb[nvn_stage[st]][b.slot+i]:state.ub[nvn_stage[st]][b.slot+i];
            need(r.address && r.size && r.size<=properties.limits.maxStorageBufferRange && r.address+r.size>=r.address,"Invalid storage descriptor address/range");ranges.push_back({r.address,r.address+r.size,written});
        }}
    }
    std::sort(ranges.begin(),ranges.end(),[](const Range &a,const Range &b){return a.begin<b.begin;});
    for(auto &r:ranges){if(!merged.empty() && r.begin<merged.back().end){merged.back().end=std::max(merged.back().end,r.end);merged.back().written=merged.back().written||r.written;}else merged.push_back(r);}
    for(auto &r:merged){uint64_t base=r.begin,bytes=r.end-base;
        if(r.written){Buffer view,*b;
            if(pending_view(base,bytes,1,view)){b=out.keep(readback_buffer(bytes));VkCommandBuffer cmd=recording_slot().cmd;end_open_pass();VkBufferCopy region{view.offset,b->offset,bytes};vkCmdCopyBuffer(cmd,view.buffer,b->buffer,1,&region);full_barrier(cmd);}
            else{cpu_access(base,bytes,3);b=out.keep(readback_buffer(bytes,reinterpret_cast<void*>(base),bytes));}out.storage.push_back({{base,bytes},b});out.written.push_back({{base,bytes},b});}
        else{snap_site=3;out.storage.push_back({{base,bytes},out.keep(snapshot(base,bytes,properties.limits.minStorageBufferOffsetAlignment))});snap_site=0;}
    }
    LayoutEntry *entry=nullptr;{auto quick=shader_layouts.find(shader_key);if(quick!=shader_layouts.end())entry=quick->second;}
    if(!entry){auto found=layout_cache.find(key);
    if(found==layout_cache.end()){
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for(size_t i=0;i<key.size();i+=4){VkDescriptorSetLayoutBinding l{};l.binding=key[i];l.descriptorType=VkDescriptorType(key[i+1]);l.descriptorCount=key[i+2];l.stageFlags=key[i+3];bindings.push_back(l);}
        LayoutEntry e;VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=bindings.size();li.pBindings=bindings.data();VK(vkCreateDescriptorSetLayout(cfg.device,&li,nullptr,&e.layout));
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pi.setLayoutCount=1;pi.pSetLayouts=&e.layout;VK(vkCreatePipelineLayout(cfg.device,&pi,nullptr,&e.pipeline_layout));
        {XbgPipelineKey k;e.definition.assign(k,xbg_pipeline_layout_key(k,li,pi));}
        found=layout_cache.emplace(key,e).first;
    }
    entry=&found->second;shader_layouts.emplace(shader_key,entry);}
    out.entry=entry;out.pipeline_layout=entry->pipeline_layout;
    }
    static std::vector<VkDescriptorBufferInfo> buffer_infos;static std::vector<VkDescriptorImageInfo> image_infos;static std::vector<VkBufferView> texel_views;static std::vector<VkWriteDescriptorSet> writes;static std::vector<std::pair<uint64_t,uint32_t>> dynamic_list;static std::vector<uint64_t> set_key;
    {size_t elements=0;for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute)for(auto &b:state.shader[st]->bindings)elements+=b.count;
     buffer_infos.clear();image_infos.clear();texel_views.clear();writes.clear();dynamic_list.clear();buffer_infos.reserve(elements);image_infos.reserve(elements);texel_views.reserve(elements);writes.reserve(elements);}
    for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute){Shader &shader=*state.shader[st];unsigned ns=nvn_stage[st];
        for(auto &b:shader.bindings)for(unsigned element=0;element<b.count;element++){DESC_SCOPE(ElementRest);
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=VK_NULL_HANDLE;write.dstBinding=b.binding;write.dstArrayElement=element;write.descriptorCount=1;write.descriptorType=UBO_DYN(dmode,b)?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:VkDescriptorType(b.type);
            if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){DESC_SCOPE(BufferBind);
                Buffer *buffer=nullptr;bool is_storage=b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;uint32_t slot=b.slot+element;size_t n=0,buffer_offset=0;const void *data=nullptr;size_t copy=0;
                if(b.source==XBG_SOURCE_UNIFORM || b.source==XBG_SOURCE_STORAGE){need(slot<16,"Buffer slot out of bounds");auto r=b.source==XBG_SOURCE_UNIFORM?state.ub[ns][slot]:state.sb[ns][slot];need(r.address && r.size,"Required NVN buffer slot is unbound");data=reinterpret_cast<void*>(r.address);copy=r.size;
                    if(is_storage){n=copy;for(auto &v:out.storage)if(r.address>=v.first.first && r.address+r.size<=v.first.first+v.first.second){buffer=v.second;buffer_offset=r.address-v.first.first;break;}need(buffer,"Storage alias allocation not found");need(buffer_offset%properties.limits.minStorageBufferOffsetAlignment==0,"NVN storage offset violates Vulkan alignment");}
                    else{copy=std::min<size_t>(copy,b.bytes?b.bytes:65536);n=uniform_range(copy);buffer=out.keep(uniform_snapshot(r.address,copy));}
                }else if(b.source==XBG_SOURCE_SUPPORT){n=1392;need(state.color.alpha_func==8,"Nontrivial NVN alpha test not translated");
                    Buffer &sb=support_block();data=sb.mapped;copy=n;buffer=out.keep(sb);}
                else if(b.source==XBG_SOURCE_C1){
                    if(!shader.c1_ready){size_t bytes=uniform_range(shader.c1.size());shader.c1_buffer=static_arena.alloc(bytes);memset(shader.c1_buffer.mapped,0,bytes);if(!shader.c1.empty())memcpy(shader.c1_buffer.mapped,shader.c1.data(),shader.c1.size());shader.c1_buffer.flush();shader.c1_ready=true;}
                    data=shader.c1.data();copy=shader.c1.size();n=shader.c1_buffer.size;buffer=&shader.c1_buffer;}
                else if(b.source==XBG_SOURCE_DRIVER0 || b.source==XBG_SOURCE_DRIVER2){n=0x1000;unsigned bank=b.source==XBG_SOURCE_DRIVER0?0:1;uint64_t gen=bank?tex_gen[ns]:buf_gen[ns]+(ns==5?0:0);
                    DriverCache &dc=driver_cache[ns][bank];if(dc.slot!=slot_serial || dc.gen!=gen || !dc.slot){Buffer s=stream_buffer(n);fill_driver(static_cast<uint8_t*>(s.mapped),ns,bank?2:0);s.flush();dc.gen=gen;dc.slot=slot_serial;dc.buffer=s;}
                    data=dc.buffer.mapped;copy=n;buffer=out.keep(dc.buffer);}
                else xbg_fatalf("Unsupported shader buffer source %u",b.source);
                need(n && n<=size_t(is_storage?properties.limits.maxStorageBufferRange:properties.limits.maxUniformBufferRange),"Descriptor range exceeds device limit");
                if(write.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC){buffer_infos.push_back({buffer->buffer,0,n});if(desc_memo_recording)desc_memo_dyn.push_back({(uint64_t(b.binding)<<32)|element,buffer->buffer,uint32_t(buffer->offset+buffer_offset),b.source==XBG_SOURCE_UNIFORM,uint8_t(b.source),uint8_t(ns),uint8_t(b.source==XBG_SOURCE_DRIVER0?0:1)});dynamic_list.push_back({(uint64_t(b.binding)<<32)|element,uint32_t(buffer->offset+buffer_offset)});}
                else buffer_infos.push_back({buffer->buffer,buffer->offset+buffer_offset,n});write.pBufferInfo=&buffer_infos.back();
                if(capture.active){char label[100];snprintf(label,sizeof(label),"st%u-bind%u-el%u-buffer",st,b.binding,element);capture_log("BUFFER label=%s shader=%s nvnStage=%u sourceKind=%u slot=%u guest=%p copied=%zu descriptorBytes=%zu bufferOffset=%zu",label,shader.key.c_str(),ns,b.source,slot,data,copy,n,buffer_offset);capture_data(label,static_cast<const uint8_t*>(buffer->mapped)+buffer_offset,n);}
            }else{
                DESC_SCOPE(ImageLookup);
                bool img_fast=false,img_cacheable=false;ImgKey ikey{};
                if(img_cache_enabled && !capture.active && !provenance_file && b.source==XBG_SOURCE_TEXTURE && (b.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) && b.slot+element<128 && state.texture_pool.pool
                   && (b.sampler_slot==UINT32_MAX || b.sampler_slot<128)){
                    uint64_t handle=state.tex[ns][b.slot+element];int id=int(handle&0xfffff);
                    if(id!=0 && !(state.texture_pool.n>0 && id>=state.texture_pool.n)){
                        uint64_t sh=b.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?(b.sampler_slot!=UINT32_MAX?state.samp[ns][b.sampler_slot]:handle):0;
                        ikey={handle,sh,uint64_t(uintptr_t(state.texture_pool.pool->mem)),uint64_t(state.texture_pool.off),state.sampler_pool.pool?uint64_t(uintptr_t(state.sampler_pool.pool->mem)):0,uint64_t(state.sampler_pool.off),uint64_t(b.type)|(uint64_t((b.flags>>16)&3)<<8)};img_cacheable=true;
                        auto it=img_cache.find(ikey);
                        auto live=it==img_cache.end()?images.end():images.find(it->second.tex->serial);
                        if(it!=img_cache.end() && it->second.record->revision==it->second.revision &&
                            live!=images.end() && live->second.get()==it->second.im){Image *im=&get_image(it->second.tex,true);
                            prepare_read(*im,commands.command);if(desc_memo_recording)desc_memo_images.push_back({it->second.tex,im,it->second.record,it->second.revision});
                            VkDescriptorImageInfo ii{};ii.imageView=it->second.view;ii.sampler=b.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?it->second.sampler:VK_NULL_HANDLE;ii.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
                            image_infos.push_back(ii);write.pImageInfo=&image_infos.back();img_fast=true;++img_cache_hits;}
                        else if(it!=img_cache.end())img_cache.erase(it);
                    }
                }
                if(!img_fast){
                need(b.source==XBG_SOURCE_TEXTURE || b.source==XBG_SOURCE_IMAGE,"Unsupported image source");unsigned slot=b.slot+element;need(slot<(b.source==XBG_SOURCE_IMAGE?16u:128u),"Image binding slot out of range");uint64_t handle=b.source==XBG_SOURCE_IMAGE?state.img[ns][slot]:state.tex[ns][slot];int id=int(handle&0xfffff);
                const RegisteredTexture *record=nullptr;
                if(id==0 || (state.texture_pool.n>0 && id>=state.texture_pool.n))record=&null_texture(b);
                else{PoolKey pk=pool_key(&state.texture_pool);if(!texture_pool_cache || !(pk==texture_pool_cache_key)){auto pool=textures.find(pk);texture_pool_cache=pool==textures.end()?&legacy_textures:&pool->second;texture_pool_cache_key=pk;}auto tex=texture_pool_cache->find(id);if(tex==texture_pool_cache->end())xbg_fatalf("Unregistered texture descriptor id=%d stage=%u slot=%u",id,ns,slot);record=&tex->second;}
                const auto &registered=*record;const tex_t &texture=registered.texture;
                if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){need(texture.d.target==10 && texture.mem && texture.L.size,"Invalid buffer texture");
                    TexelResident *res=nullptr;
                    if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && texel_memo_enabled && !compute){Buffer pv;
                        if(!pending_view(uint64_t(texture.mem),texture.L.size,1,pv))res=texel_acquire(uint64_t(texture.mem),texture.L.size,format(texture.d.format));
                        if(res){if(desc_memo_recording)desc_memo_texel.push_back({record,record->revision,uint64_t(texture.mem),uint64_t(texture.L.size),res->gen,uint32_t(format(texture.d.format))});
                            texel_views.push_back(res->view);write.pTexelBufferView=&texel_views.back();}
                        else desc_memo_texel_fail=true;}   // the set then holds a per-frame view: not memoized
                    if(!res){
                    Buffer *buf=nullptr;uint64_t base=uint64_t(texture.mem),bytes=texture.L.size;
                    if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER){Buffer view;if(pending_view(base,bytes,1,view)){buf=out.keep(readback_buffer(bytes));VkCommandBuffer cmd=recording_slot().cmd;end_open_pass();VkBufferCopy region{view.offset,buf->offset,bytes};vkCmdCopyBuffer(cmd,view.buffer,buf->buffer,1,&region);full_barrier(cmd);}else{cpu_access(base,bytes,8);buf=out.keep(readback_buffer(bytes,texture.mem,bytes));}out.storage.push_back({{base,bytes},buf});out.written.push_back({{base,bytes},buf});}
                    else{snap_site=4;buf=out.keep(snapshot(base,bytes,properties.limits.minTexelBufferOffsetAlignment));snap_site=0;texel_frame_keys.insert((base<<12)^bytes);++texel_binds;}
                    VkBufferViewCreateInfo v{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};v.buffer=buf->buffer;v.offset=buf->offset;v.format=format(texture.d.format);v.range=bytes;VkBufferView bv;{auto &cache=recording_slot().texel_views;std::array<uint64_t,4> key{uint64_t(v.buffer),v.offset,v.range,uint64_t(v.format)};auto f=cache.find(key);if(f!=cache.end())bv=f->second;else{VK(vkCreateBufferView(cfg.device,&v,nullptr,&bv));cache.emplace(key,bv);}}texel_views.push_back(bv);write.pTexelBufferView=&texel_views.back();}}
                else{if(aspect(texture.d.format)!=VK_IMAGE_ASPECT_COLOR_BIT){auto prior=images.find(texture.serial);if(prior==images.end() || (!prior->second->uploaded && !prior->second->gpu_written))xbg_logf("NVN initial depth descriptor shader=%s stage=%u slot=%u id=%d serial=%llu format=%#x",shader.key.c_str(),ns,slot,id,(unsigned long long)texture.serial,texture.d.format);}
                    VkDescriptorImageInfo ii{};
                    Image &im=get_image(&texture,true);provenance_log("BIND shader=%s stage=%u slot=%u id=%d handle=%llu descriptorMem=%p descriptorOff=%lld type=%u",shader.key.c_str(),ns,slot,id,(unsigned long long)handle,state.texture_pool.pool?state.texture_pool.pool->mem:nullptr,(long long)state.texture_pool.off,unsigned(b.type));provenance_image("BIND",&texture,im,registered.has_view?&registered.view:nullptr);DESC_CALL(ImageTransition, prepare_read(im,commands.command));if(desc_memo_recording)desc_memo_images.push_back({&texture,&im,record,record->revision});ii.imageView=DESC_CALL(ImageView, im.view(registered.has_view?&registered.view:nullptr,false,b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE));ii.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
                    if(capture.active){char label[100];snprintf(label,sizeof(label),"st%u-bind%u-el%u-input",st,b.binding,element);out.capture_inputs.push_back({&im,registered.view,registered.has_view,label});capture_log("TEXTURE label=%s shader=%s nvnStage=%u slot=%u handle=%#llx id=%d serial=%llu cachedSerial=%llu image=%p view=%p viewFormat=%u sourceKind=%u",label,shader.key.c_str(),ns,slot,(unsigned long long)handle,id,(unsigned long long)texture.serial,(unsigned long long)im.source.serial,(void*)im.image,(void*)ii.imageView,unsigned(im.view_format(registered.has_view?&registered.view:nullptr)),b.source);}
                    if(b.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b.type==VK_DESCRIPTOR_TYPE_SAMPLER){DESC_SCOPE(SamplerBind);uint64_t sh=handle;if(b.sampler_slot!=UINT32_MAX){need(b.sampler_slot<128,"Sampler slot out of range");sh=state.samp[ns][b.sampler_slot];}int sid=int(sh>>20);samp_par parameters{};if(sid==0){parameters.ws=parameters.wt=parameters.wr=7;parameters.aniso=1;parameters.cmp_func=1;}
                        else{auto sp=samplers.find(pool_key(&state.sampler_pool));auto &table=sp==samplers.end()?legacy_samplers:sp->second;auto sa=table.find(sid);if(sa==table.end())xbg_fatalf("Unregistered sampler id=%d",sid);parameters=sa->second;}ii.sampler=sampler(parameters,im,im.view_format(registered.has_view?&registered.view:nullptr),((b.flags>>16)&3)!=0);if(capture.active)capture_log("SAMPLER st=%u binding=%u sid=%d samplerSlot=%u min=%u mag=%u wrap=%u/%u/%u lod=%g/%g bias=%g",st,b.binding,sid,b.sampler_slot,parameters.minf,parameters.magf,parameters.ws,parameters.wt,parameters.wr,parameters.lod_min,parameters.lod_max,parameters.lod_bias);}
                    if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE){VkFormat viewed=im.view_format(registered.has_view?&registered.view:nullptr);if(!(im.usage&VK_IMAGE_USAGE_STORAGE_BIT))xbg_fatalf("NVN storage image without IMAGE flag serial=%llu format=%#x flags=%#x",(unsigned long long)texture.serial,texture.d.format,texture.d.flags);const VkFormatProperties &fp=format_properties(viewed);need(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,"Format not usable as storage image");
                        if((b.flags&3) && format_feature_flags2){VkFormatProperties3 extra{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};VkFormatProperties2 all{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};all.pNext=&extra;vkGetPhysicalDeviceFormatProperties2(cfg.physical,viewed,&all);if(b.flags&1)need(extra.optimalTilingFeatures&VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT,"Image format lacks formatless-write support");if(b.flags&2)need(extra.optimalTilingFeatures&VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT,"Image format lacks formatless-read support");}
                        else if(b.flags&3){VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(cfg.physical,viewed,&fp);bool listed=storage_without_format_listed(viewed) && (fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
                            if((b.flags&1) && !(listed && cfg.enabled_core.shaderStorageImageWriteWithoutFormat))xbg_fatalf("Image format %d lacks formatless-write support without VK_KHR_format_feature_flags2 (shader=%s listedWithStorageImage=%d shaderStorageImageWriteWithoutFormat=%u)",int(viewed),shader.key.c_str(),int(listed),unsigned(cfg.enabled_core.shaderStorageImageWriteWithoutFormat));
                            if((b.flags&2) && !(listed && cfg.enabled_core.shaderStorageImageReadWithoutFormat))xbg_fatalf("Image format %d lacks formatless-read support without VK_KHR_format_feature_flags2 (shader=%s listedWithStorageImage=%d shaderStorageImageReadWithoutFormat=%u)",int(viewed),shader.key.c_str(),int(listed),unsigned(cfg.enabled_core.shaderStorageImageReadWithoutFormat));}im.gpu_written=true,provenance_write(im,__LINE__);im.last_write=++write_seq;}
                    image_infos.push_back(ii);write.pImageInfo=&image_infos.back();
                    if(img_cacheable){img_cache.emplace(ikey,ImgEntry{&texture,&im,ii.imageView,ii.sampler,record,record->revision});++img_cache_misses;}
                }
                }
            }
            writes.push_back(write);
        }
    }
    // Dynamic offsets are consumed in binding/array-element order.
    std::sort(dynamic_list.begin(),dynamic_list.end());out.dynamic_offsets.clear();for(auto &d:dynamic_list)out.dynamic_offsets.push_back(d.second);
    if(!set_cache_enabled){out.set=allocate_set(out.entry->layout);if(writes.empty())return;
        // Update templates: one driver call with packed payloads instead of parsing VkWriteDescriptorSet per element.
        LayoutEntry &e=*const_cast<LayoutEntry*>(out.entry);static std::vector<uint64_t> sig;sig.clear();
        for(auto &w:writes)sig.push_back((uint64_t(w.dstBinding)<<40)|(uint64_t(w.dstArrayElement)<<8)|uint64_t(w.descriptorType));
        if(template_enabled && !e.tmpl){std::vector<VkDescriptorUpdateTemplateEntry> entries;for(size_t i=0;i<writes.size();i++)entries.push_back({writes[i].dstBinding,writes[i].dstArrayElement,1,writes[i].descriptorType,i*sizeof(DescEntry),sizeof(DescEntry)});
            VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};ti.descriptorUpdateEntryCount=uint32_t(entries.size());ti.pDescriptorUpdateEntries=entries.data();ti.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;ti.descriptorSetLayout=e.layout;
            if(vkCreateDescriptorUpdateTemplate(cfg.device,&ti,nullptr,&e.tmpl)==VK_SUCCESS)e.tmpl_sig=sig;else e.tmpl=VK_NULL_HANDLE;}
        if(template_enabled && e.tmpl && sig==e.tmpl_sig){static std::vector<DescEntry> data;data.resize(writes.size());
            for(size_t i=0;i<writes.size();i++){auto &w=writes[i];if(w.pBufferInfo)data[i].buffer=*w.pBufferInfo;else if(w.pImageInfo)data[i].image=*w.pImageInfo;else data[i].view=*w.pTexelBufferView;}
            vkUpdateDescriptorSetWithTemplate(cfg.device,out.set,e.tmpl,data.data());++template_updates;return;}
        for(auto &w:writes)w.dstSet=out.set;vkUpdateDescriptorSets(cfg.device,uint32_t(writes.size()),writes.data(),0,nullptr);++plain_updates;return;}
    uint64_t tk0=perf_clock();
    set_key.clear();set_key.push_back(uint64_t(out.entry->layout));
    for(auto &w:writes){set_key.push_back((uint64_t(w.dstBinding)<<32)|w.dstArrayElement);set_key.push_back(w.descriptorType);
        if(w.pBufferInfo){set_key.push_back(uint64_t(w.pBufferInfo->buffer));set_key.push_back(w.pBufferInfo->offset);set_key.push_back(w.pBufferInfo->range);}
        else if(w.pImageInfo){set_key.push_back(uint64_t(w.pImageInfo->sampler));set_key.push_back(uint64_t(w.pImageInfo->imageView));set_key.push_back(w.pImageInfo->imageLayout);}
        else if(w.pTexelBufferView)set_key.push_back(uint64_t(*w.pTexelBufferView));}
    Slot &slot=recording_slot();bool use_cache=true;auto hit=use_cache?slot.set_cache.find(set_key):slot.set_cache.end();desc_ns[3]+=perf_clock()-tk0;
    if(hit!=slot.set_cache.end()){out.set=hit->second;++set_cache_hits;return;}
    {uint64_t t0=perf_clock();out.set=allocate_set(out.entry->layout);desc_ns[2]+=perf_clock()-t0;}++set_cache_misses;
    if(!writes.empty()){LayoutEntry &e=*const_cast<LayoutEntry*>(out.entry);static std::vector<uint64_t> sig2;sig2.clear();
        for(auto &w:writes)sig2.push_back((uint64_t(w.dstBinding)<<40)|(uint64_t(w.dstArrayElement)<<8)|uint64_t(w.descriptorType));
        if(!e.tmpl){std::vector<VkDescriptorUpdateTemplateEntry> entries;for(size_t i=0;i<writes.size();i++)entries.push_back({writes[i].dstBinding,writes[i].dstArrayElement,1,writes[i].descriptorType,i*sizeof(DescEntry),sizeof(DescEntry)});
            VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};ti.descriptorUpdateEntryCount=uint32_t(entries.size());ti.pDescriptorUpdateEntries=entries.data();ti.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;ti.descriptorSetLayout=e.layout;
            if(vkCreateDescriptorUpdateTemplate(cfg.device,&ti,nullptr,&e.tmpl)==VK_SUCCESS)e.tmpl_sig=sig2;else e.tmpl=VK_NULL_HANDLE;}
        if(e.tmpl && sig2==e.tmpl_sig){static std::vector<DescEntry> data;data.resize(writes.size());
            for(size_t i=0;i<writes.size();i++){auto &w=writes[i];if(w.pBufferInfo)data[i].buffer=*w.pBufferInfo;else if(w.pImageInfo)data[i].image=*w.pImageInfo;else data[i].view=*w.pTexelBufferView;}
            {uint64_t t0=perf_clock();vkUpdateDescriptorSetWithTemplate(cfg.device,out.set,e.tmpl,data.data());desc_ns[1]+=perf_clock()-t0;}++template_updates;}
        else{for(auto &w:writes)w.dstSet=out.set;vkUpdateDescriptorSets(cfg.device,uint32_t(writes.size()),writes.data(),0,nullptr);++plain_updates;}}
    if(use_cache)slot.set_cache.emplace(set_key,out.set);
}
// Render passes are cached by their full definition; framebuffers are cached
// per frame slot (they reference image views of possibly released images).
struct Pass {
    const PipelineDefinition *definition=nullptr;
    VkRenderPass pass{},pass_noload{};VkFramebuffer framebuffer{};uint32_t width{},height{},color_slots{};VkSampleCountFlagBits samples=VK_SAMPLE_COUNT_1_BIT;ImageList images;
};
// Draws inside the open pass with unchanged render-target state get the same Pass: non-draw ops end the pass, so nothing
// else touched its attachments since. Only the attachments' write marks are refreshed (as prepare_attachment does).
// Reset with every submission and image upgrade. create_pass ran in full for each of ~2200 draws per D2R camp frame.
struct LastPass{decltype(state.rt) rt;Pass pass;bool valid=false;} last_pass;uint64_t pass_fast_hits=0;
void last_pass_reset(){last_pass.valid=false;}
// 保留零值计数用于与旧版本日志比较；任意 guest draw 不再丢弃附件内容。
// Keep zero counters for baseline logs; arbitrary guest draws retain attachment contents.
uint64_t noload_passes=0;bool noload_enabled=false;
// noload: the same pass with every color loadOp DONT_CARE (noload_enabled; first_draw_covers). Only load ops differ, so it is
// compatible with pass and its pipelines and framebuffers.
struct RenderPassEntry {VkRenderPass pass{},noload{};PipelineDefinition definition;};
std::map<std::vector<uint32_t>,RenderPassEntry> render_pass_cache;
void clear_render_pass_cache(){for(auto &r:render_pass_cache){vkDestroyRenderPass(cfg.device,r.second.pass,nullptr);if(r.second.noload)vkDestroyRenderPass(cfg.device,r.second.noload,nullptr);}render_pass_cache.clear();pipeline_fast.clear();pipe_memo_valid=false;}
bool create_pass(Pass &p,Commands &c,int clear_color=-1,bool clear_depth=false,bool soft=false){CPU_SCOPE(Pass);
    const bool plain_draw=c.draw && clear_color<0 && !clear_depth && !soft && !provenance_file && !capture.active && !ab_off("passfast");
    if(plain_draw && last_pass.valid && open_pass.cmd==c.command && open_pass.pass==last_pass.pass.pass && open_pass.framebuffer==last_pass.pass.framebuffer && !memcmp(&state.rt,&last_pass.rt,sizeof state.rt)){
        p=last_pass.pass;for(auto *im:p.images){im->last_write=++write_seq;im->gpu_written=true;}++pass_fast_hits;return true;}
    // 仅显式设置DM_VK_TRACE_ATTACHMENTS=1时输出成功诊断；首次调用读取，错误始终记录。
    // Read once: opt in with DM_VK_TRACE_ATTACHMENTS=1; errors are always logged.
    static const bool trace_attachments=[](){const char *v=std::getenv("DM_VK_TRACE_ATTACHMENTS");return v && std::strcmp(v,"1")==0;}();
    need(state.rt.n>=0 && state.rt.n<=8,"Invalid color target count");
    bool clear_only=clear_color>=0 || clear_depth;
    std::array<const tex_t*,9> targets{};std::array<const view_t*,9> selected{};std::array<xbg_framebuffer_attachment,9> extents{};
    for(int i=0;i<state.rt.n;i++)if(state.rt.color[i] && (!clear_only || clear_color==i)){targets[i]=state.rt.color[i];selected[i]=state.rt.has_cview[i]?&state.rt.cview[i]:nullptr;}
    if(state.rt.depth && (!clear_only || clear_depth)){targets[8]=state.rt.depth;selected[8]=state.rt.has_dview?&state.rt.dview:nullptr;}
    uint64_t trace=clear_only?1:0;
    for(unsigned i=0;i<9;i++)if(targets[i]){
        const tex_t &t=*targets[i];const view_t *v=selected[i];
        need(t.magic==MAGIC('T','E','X',0) && t.serial,"Invalid NVN render attachment");
        extents[i]={1,t.d.width,t.d.height,t.d.levels,t.L.layers,v&&v->set_levels?uint32_t(v->base_level):0,v&&v->set_levels?uint32_t(v->nlevels):t.d.levels,
            v&&v->set_layers?uint32_t(v->min_layer):0,v&&v->set_layers?uint32_t(v->nlayers):t.L.layers,uint32_t(std::max(1,t.d.samples))};
        trace=(trace^t.serial^uint64_t(i)) * 1099511628211ull;trace^=hash_memory(&extents[i],sizeof(extents[i]));
        trace^=uint64_t(v&&v->set_format?v->format:t.d.format)<<32;
    }
    xbg_framebuffer_extent plan{};uint32_t bad=UINT32_MAX;int result=xbg_framebuffer_plan(extents.data(),extents.size(),properties.limits.maxFramebufferWidth,properties.limits.maxFramebufferHeight,&plan,&bad);
    if(result || (trace_attachments && trace!=last_attachment_trace)){
        xbg_logf("NVN framebuffer mode=%s result=%d viewport=%g,%g:%gx%g depthRange=%g/%g scissor=%d,%d:%ux%u selected=%ux%u samples=%u",clear_color>=0?"clear-color":clear_depth?"clear-depth-stencil":"draw",result,state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height,state.viewport.minDepth,state.viewport.maxDepth,state.scissor.offset.x,state.scissor.offset.y,state.scissor.extent.width,state.scissor.extent.height,plan.width,plan.height,plan.samples);
        for(unsigned i=0;i<9;i++)if(targets[i]){const auto &e=extents[i];uint32_t w=e.base_level<32?std::max(1u,e.width>>e.base_level):0,h=e.base_level<32?std::max(1u,e.height>>e.base_level):0;
            xbg_logf("NVN attachment index=%d serial=%llu format=%#x viewFormat=%#x target=%d mip=%u/%u baseLayer=%u layerCount=%u totalLayers=%u base=%ux%u selected=%ux%u samples=%u",i==8?-1:int(i),(unsigned long long)targets[i]->serial,targets[i]->d.format,selected[i]&&selected[i]->set_format?selected[i]->format:targets[i]->d.format,targets[i]->d.target,e.base_level,e.level_count,e.base_layer,e.layer_count,e.layers,e.width,e.height,w,h,e.samples);
        }
        last_attachment_trace=trace;
    }
    if(result && soft)return false;
    if(result)xbg_fatalf("NVN framebuffer %s result=%d attachment=%d",xbg_framebuffer_error(result),result,bad==UINT32_MAX?-1:int(bad));
    p.width=plan.width;p.height=plan.height;p.samples=VkSampleCountFlagBits(plan.samples);
    static std::vector<VkAttachmentDescription> descriptions;descriptions.clear();static std::vector<VkImageView> views;views.clear();   // not re-entrant: graphics thread
    static std::vector<VkAttachmentReference> colors;colors.assign(size_t(state.rt.n),{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL});VkAttachmentReference depth{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
    for(unsigned slot=0;slot<9;slot++)if(targets[slot]){
        Image &im=get_image(targets[slot],false);const view_t *view=selected[slot];
        need((slot==8)==(im.aspects!=VK_IMAGE_ASPECT_COLOR_BIT),"Color/depth attachment role mismatches texture aspects");
        if(c.draw)prepare_attachment(im,c.command);else im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);VkAttachmentDescription a{};a.format=im.view_format(view);a.samples=im.samples;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        uint32_t index=descriptions.size();descriptions.push_back(a);views.push_back(im.view(view,true));p.images.push_back(&im);im.gpu_written=true,provenance_write(im,__LINE__);
        if(slot==8)depth.attachment=index;else colors[slot].attachment=index;
    }
    p.color_slots=colors.size();VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=colors.size();sub.pColorAttachments=colors.data();sub.pDepthStencilAttachment=depth.attachment!=VK_ATTACHMENT_UNUSED?&depth:nullptr;
    VkSubpassDependency dep[2]{};dep[0]={VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0};dep[1]={0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,0};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=descriptions.size();rp.pAttachments=descriptions.data();rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=dep;
    {XbgPipelineKey key;bool valid=xbg_renderpass_key(key,rp);
        if(valid && key.valid){static std::vector<uint32_t> words;words.assign(key.words,key.words+key.count);auto it=render_pass_cache.find(words);
            if(it==render_pass_cache.end()){RenderPassEntry e;VK(vkCreateRenderPass(cfg.device,&rp,nullptr,&e.pass));e.definition.assign(key,true);
                if(noload_enabled && p.samples==VK_SAMPLE_COUNT_1_BIT){std::vector<VkAttachmentDescription> nl(descriptions.begin(),descriptions.end());bool any=false;
                    for(size_t k=0;k<nl.size() && k<p.images.size();k++)if(p.images[k]->aspects==VK_IMAGE_ASPECT_COLOR_BIT){nl[k].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;any=true;}
                    if(any){VkRenderPassCreateInfo r2=rp;r2.pAttachments=nl.data();VK(vkCreateRenderPass(cfg.device,&r2,nullptr,&e.noload));}}
                it=render_pass_cache.emplace(words,std::move(e)).first;}
            p.pass=it->second.pass;p.pass_noload=it->second.noload;p.definition=&it->second.definition;
        }
        else{VK(vkCreateRenderPass(cfg.device,&rp,nullptr,&p.pass));VkRenderPass rpass=p.pass;retire_handle([rpass]{vkDestroyRenderPass(cfg.device,rpass,nullptr);});static PipelineDefinition invalid;p.definition=&invalid;}
    }
    Slot &slot=recording_slot();static std::vector<uint64_t> fb_key;fb_key.assign({uint64_t(p.pass),p.width,p.height});for(auto v:views)fb_key.push_back(uint64_t(v));
    // Cached render passes live until shutdown, so their framebuffers can outlive the frame; they go with the first of their
    // images (forget_framebuffers). A pass outside the cache is retired with this slot, and so are its framebuffers.
    bool persistent=persistent_fb_enabled && p.definition && p.definition->valid;
    auto &cache=persistent?persistent_framebuffers:slot.framebuffers;
    auto cached=cache.find(fb_key);
    if(cached!=cache.end()){p.framebuffer=cached->second;++fb_hits;}
    else{VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=p.pass;fb.attachmentCount=views.size();fb.pAttachments=views.data();fb.width=p.width;fb.height=p.height;fb.layers=1;VK(vkCreateFramebuffer(cfg.device,&fb,nullptr,&p.framebuffer));cache.emplace(fb_key,p.framebuffer);++fb_creates;
        if(persistent)for(auto *im:p.images)im->fb_keys.push_back(fb_key);}
    if(plain_draw && persistent){last_pass.rt=state.rt;last_pass.pass=p;last_pass.valid=true;}else last_pass.valid=false;
    return true;
}
VkRect2D scissor(const Pass &p){VkRect2D r{};need(xbg_clear_rectangle(p.width,p.height,state.scissor.offset.x,state.scissor.offset.y,state.scissor.extent.width,state.scissor.extent.height,&r)==0,"Invalid scissor extent");return r;}
int pass_prof_reserve(VkCommandBuffer cmd){Slot &s=slots[slot_index];if(!s.prof || cmd!=s.cmd)return -1;
    // 开始时预留首尾查询，预算耗尽时整段跳过，不能留下只有开头的假计时。
    // Reserve both queries up front; skip the whole interval when the frame budget is exhausted.
    if(s.prof_used+2>pass_prof_query_limit || pass_prof_queries+2>pass_prof_query_limit){++s.prof_skipped;return -1;}
    Slot::PassRec r;r.q0=s.prof_used++;r.q1=s.prof_used++;r.draws0=r.draws1=uint32_t(pass_prof_draws);r.verts0=r.verts1=pass_prof_verts;pass_prof_queries+=2;
    s.prof_recs.push_back(std::move(r));int index=int(s.prof_recs.size())-1;
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.prof_pool,s.prof_recs[index].q0);return index;
}
void pass_prof_complete(VkCommandBuffer cmd,int index){Slot &s=slots[slot_index];if(!s.prof || cmd!=s.cmd || index<0 || size_t(index)>=s.prof_recs.size())return;
    auto &r=s.prof_recs[index];r.draws1=uint32_t(pass_prof_draws);r.verts1=pass_prof_verts;vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.prof_pool,r.q1);r.complete=true;
}
void pass_prof_end(VkCommandBuffer cmd){Slot &s=slots[slot_index];if(!s.prof || cmd!=s.cmd)return;pass_prof_complete(cmd,s.prof_open);s.prof_open=-1;}
void pass_prof_depth_idle(VkCommandBuffer cmd){Slot &s=slots[slot_index];if(!s.prof || cmd!=s.cmd || s.prof_open<0 || size_t(s.prof_open)>=s.prof_recs.size())return;s.prof_recs[size_t(s.prof_open)].dz=true;}
void pass_prof_begin(const Pass &p,VkCommandBuffer cmd){Slot &s=slots[slot_index];s.prof_open=pass_prof_reserve(cmd);if(s.prof_open<0)return;
    auto &r=s.prof_recs[s.prof_open];r.w=p.width;r.h=p.height;r.att=uint32_t(p.images.size());r.fmt=p.images.empty()?0u:uint32_t(p.images[0]->fmt);
}
void pass_prof_draw(VkCommandBuffer cmd){Slot &s=slots[slot_index];if(!s.prof || cmd!=s.cmd || s.prof_open<0)return;
    std::string keys;for(unsigned st=0;st<5;++st)if(state.shader[st]){if(!keys.empty())keys+=';';keys+=std::to_string(st)+":"+state.shader[st]->key;}
    auto &r=s.prof_recs[s.prof_open];if(r.first.empty())r.first=keys;else if(r.last!=keys)r.mixed=true;r.last=std::move(keys);
}
int pass_prof_compute_begin(VkCommandBuffer cmd,const p_dispatch &p){int index=pass_prof_reserve(cmd);if(index<0)return index;
    auto &r=slots[slot_index].prof_recs[index];r.compute=true;r.first=state.shader[5]->key;r.last=r.first;r.x=uint32_t(p.x);r.y=uint32_t(p.y);r.z=uint32_t(p.z);return index;
}
void begin_pass(Pass &p,Commands &c){pass_prof_begin(p,c.command);VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=p.pass;b.framebuffer=p.framebuffer;b.renderArea.extent={p.width,p.height};vkCmdBeginRenderPass(c.command,&b,VK_SUBPASS_CONTENTS_INLINE);}
void capture_pass(Pass &pass){
    if(!capture.active)return;
    capture_log("PASS width=%u height=%u samples=%u viewport=%g,%g,%g,%g scissor=%d,%d,%u,%u mask=%#x blendEnable=%#x cull=%u front=%u depthTest=%u depthWrite=%u depthFunc=%u stencil=%u",pass.width,pass.height,unsigned(pass.samples),state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height,state.scissor.offset.x,state.scissor.offset.y,state.scissor.extent.width,state.scissor.extent.height,state.mask.mask,state.color.blend_en,state.poly.cull,state.poly.front,unsigned(state.ds.dt),unsigned(state.ds.dw),unsigned(state.ds.df),unsigned(state.ds.st));
    for(int index=0;index<9;index++){
        const tex_t *t=index==8?state.rt.depth:index<state.rt.n?state.rt.color[index]:nullptr;if(!t)continue;
        auto found=images.find(t->serial);if(found==images.end())continue;Image &im=*found->second;
        if(std::find(pass.images.begin(),pass.images.end(),&im)==pass.images.end())continue;
        bool has_view=index==8?state.rt.has_dview:state.rt.has_cview[index];view_t v=has_view?(index==8?state.rt.dview:state.rt.cview[index]):view_t{};
        if(!v.set_levels)v.base_level=0;if(!v.set_layers)v.min_layer=0;v.set_levels=v.set_layers=1;v.nlevels=v.nlayers=1;
        char label[40];snprintf(label,sizeof(label),"rt%d-output",index==8?-1:index);capture_image(im,&v,label);
    }
}
VkPrimitiveTopology topology(int prim){switch(prim){case 0:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;case 1:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;case 3:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;case 4:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;case 5:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;case 6:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;case 10:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;case 11:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;case 12:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;case 13:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;case 14:return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;default:xbg_fatalf("Unsupported draw primitive %d",prim);}}
VkStencilOpState stencil(bool back){VkStencilOpState s{};s.failOp=VkStencilOp((back?state.ds.fail_b:state.ds.fail_f)-1);s.passOp=VkStencilOp((back?state.ds.zpass_b:state.ds.zpass_f)-1);s.depthFailOp=VkStencilOp((back?state.ds.zfail_b:state.ds.zfail_f)-1);s.compareOp=VkCompareOp((back?state.ds.sf_b:state.ds.sf_f)-1);s.compareMask=state.stencil_compare[back];s.writeMask=state.stencil_write[back];s.reference=state.stencil_ref[back];return s;}
// Redundant-bind filter for the draw path (~1800 draws/frame re-bound identical pipeline / set / viewport / scissor /
// vertex buffers). Valid only within one command buffer recording; any other bind site (clear pipeline) and every
// command-buffer begin reset it. files/gfx-no-bindcache disables.
struct BindCache{VkCommandBuffer cmd=VK_NULL_HANDLE;VkPipeline pipe=VK_NULL_HANDLE;VkPipelineLayout layout=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;
    uint32_t noff=UINT32_MAX,off[32]{};bool has_vp=false,has_sc=false;VkViewport vp{};VkRect2D sc{};VkBuffer vb[16]{};VkDeviceSize vbo[16]{};
    VkBuffer ib=VK_NULL_HANDLE;VkDeviceSize ibo=0;VkIndexType it=VK_INDEX_TYPE_MAX_ENUM;};
BindCache bind_cache;bool bind_cache_enabled=true;uint64_t binds_skipped=0;
// Descriptor-set binds per frame (NVN PERF binds): how many, their dynamic offsets, and how many rebind the same set with new
// offsets only.
uint64_t set_binds=0,set_bind_offsets=0,set_binds_offsets_only=0;
// Offset-only rebinds (NVN PERF binds ... changed): how many dynamic offsets differ from the bound ones, and how often each
// offset position does. A push-constant path for a uniform block that changes on every draw would need them few and fixed.
uint64_t offset_changes[10]={},offset_changed_at[32]={};
void bind_cache_reset(){bind_cache=BindCache{};}
// 顶点数量与全 viewport/scissor 不能证明几何覆盖所有像素（局部三角形也满足）。所以只在几何上证明了的时候才不读旧内容：
// 与 Metal 侧（nvn_mtl.m 的 first_draw_covers）同一套条件与同一个纯函数 nvn_cover.h（单测 t_cover.c），位置从这次绘制
// 实际交给 GPU 的顶点 / 索引快照里读。不成立就照旧 LOAD。
// Vertex count and full viewport/scissor do not prove full pixel coverage: a partial triangle qualifies. So the old contents
// are skipped only when the geometry proves it: the conditions of the Metal side (nvn_mtl.m's first_draw_covers) and the same
// pure function nvn_cover.h (unit test t_cover.c), with the positions read from the vertex and index snapshots this draw
// hands to the GPU. Otherwise LOAD as before. Explicit clears keep their separate clear semantics.
// The first draw of a pass covers every pixel of each color attachment, writing it without blending: the vertex position
// is one input as it is (XbgRasterFacts), the fragment shader declares every attachment's output, never discards and does
// not write SampleMask, no depth (except ALWAYS), stencil or depth-bounds test, fill mode, single sample, 2D targets, and
// nvn_cover holds for the viewport, scissor, culling (the NVN cull and front face, as the Metal side passes them) and
// vertices. Should its winding reading ever differ from the GPU's, the visible triangle reads as culled and the pass simply
// loads. noload_why counts the first failing check:
// 0 shaders, 1 attachment state, 2 depth/stencil, 3 raster state (also: covered but culled), 4 primitive/count,
// 5 position format/stream, 6 vertex/index read, 7 geometry.
uint64_t noload_why[8]={};
bool first_draw_covers(const Pass &p,int prim,int first,int count,const p_draw_idx *indexed,const std::vector<std::pair<uint32_t,Buffer*>> &vbufs,const Buffer *ib,VkIndexType itype){
    const Shader *vs=state.shader[0],*fs=state.shader[4];
    if(!vs || !fs || state.shader[1] || state.shader[2] || state.shader[3] || vs->raster.pos_input<0 || vs->raster.pos_input>=16 || fs->discards || fs->raster.sample_mask){++noload_why[0];return false;}
    for(int i=0;i<state.rt.n;i++)if(state.rt.color[i]){
        if(!(fs->raster.outputs>>i&1) || (state.color.blend_en>>i&1) || ((state.mask.mask>>(4*i))&15)!=15 || state.rt.color[i]->d.target==2){++noload_why[1];return false;}}
    if(state.rt.depth && ((state.ds.dt && state.ds.df!=8) || state.ds.st)){++noload_why[2];return false;}
    if(state.bounds.enable){++noload_why[2];return false;}
    if(state.ms.a2c || state.poly.mode!=2 || p.samples!=VK_SAMPLE_COUNT_1_BIT){++noload_why[3];return false;}
    if((prim!=NVN_COVER_TRIANGLES && prim!=NVN_COVER_TRIANGLE_STRIP) || count<3 || count>64){++noload_why[4];return false;}
    va_st a;memcpy(&a,&state.attrib[vs->raster.pos_input],4);
    const VkFormat f=a.fmt?format(a.fmt):VK_FORMAT_UNDEFINED;const int nc=f==VK_FORMAT_R32G32_SFLOAT?2:f==VK_FORMAT_R32G32B32_SFLOAT?3:f==VK_FORMAT_R32G32B32A32_SFLOAT?4:0;
    if(!nc || a.stream>=16 || !state.stream[a.stream].stride || state.stream[a.stream].divisor){++noload_why[5];return false;}
    const Buffer *vb=nullptr;for(auto &b:vbufs)if(b.first==a.stream)vb=b.second;
    if(!vb || !vb->mapped || (indexed && (!ib || !ib->mapped))){++noload_why[6];return false;}
    const uint64_t stride=state.stream[a.stream].stride;const size_t isz=itype==VK_INDEX_TYPE_UINT16?2:4;
    float P[64][4];
    for(int k=0;k<count;k++){
        int64_t vid;
        if(indexed){if(uint64_t(k+1)*isz>ib->size){++noload_why[6];return false;}const uint8_t *q=static_cast<const uint8_t*>(ib->mapped)+size_t(k)*isz;
            uint32_t x=0;if(isz==2){uint16_t h;memcpy(&h,q,2);x=h;}else memcpy(&x,q,4);vid=int64_t(x)+indexed->base_vertex;}
        else vid=int64_t(first)+k;
        if(vid<0){++noload_why[6];return false;}
        const uint64_t off=uint64_t(vid)*stride+a.offset;
        if(off+4u*unsigned(nc)>vb->size){++noload_why[6];return false;}
        float v[4]={0,0,0,1};memcpy(v,static_cast<const uint8_t*>(vb->mapped)+off,4*size_t(nc));
        if(vs->raster.pos_w1)v[3]=1;
        memcpy(P[k],v,sizeof v);
    }
    const VkRect2D sr=scissor(p);const int sc[4]={sr.offset.x,sr.offset.y,int(sr.extent.width),int(sr.extent.height)};
    const float vp[4]={state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height};
    // A failure that culling alone causes counts under raster state (3), the rest under geometry (7).
    if(!nvn_cover(P,count,prim,int(state.poly.cull),state.poly.front==1,1,vp,1,sc,long(p.width),long(p.height))){
        ++noload_why[state.poly.cull && nvn_cover(P,count,prim,NVN_COVER_CULL_NONE,1,1,vp,1,sc,long(p.width),long(p.height))?3:7];return false;}
    return true;
}
// gfx-async-pipelines: may this draw be skipped while its pipeline builds? Only when losing it for a few frames loses a
// translucent effect: every written color target blends with the destination (a destination factor other than ZERO, so
// a ONE / ZERO "blend" that overwrites is not one), no depth or stencil write, no storage buffer or image writes in any
// stage, no capture running, and no samples counted once the game has recorded a samples-passed report. D2R counts samples
// for every draw after its first counter reset but never reports them, so counting alone excluded every draw (fc46144).
bool async_skippable(const std::vector<VkPipelineColorBlendAttachmentState> &att,const VkPipelineDepthStencilStateCreateInfo &ds){
    if(!warm::async_on || (state.count_samples && samples_report_seen) || capture.active || ds.depthWriteEnable)return false;
    if(ds.stencilTestEnable && (ds.front.writeMask || ds.back.writeMask))return false;
    bool writes=false;for(auto &a:att)if(a.colorWriteMask){if(!a.blendEnable || a.dstColorBlendFactor==VK_BLEND_FACTOR_ZERO)return false;writes=true;}
    if(!writes)return false;
    for(unsigned i=0;i<5;i++)if(state.shader[i]){if(!state.shader[i]->storage_writes.empty())return false;for(auto &b:state.shader[i]->bindings)if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)return false;}
    return true;
}
// 盖满整个界面矩形的无贴图块（同 nvn_mtl.m ui_cover_all）：顶点 4 个 × 20 字节，投影后正好铺满界面矩形
// An untextured quad covering the whole UI rectangle (as nvn_mtl.m ui_cover_all): 4 vertices × 20 bytes that, after projection, exactly fill the UI rectangle
bool ui_cover_all(double hf){
    auto v=state.vb[0];auto u=state.ub[0][0];if(!v.address || v.size!=80 || !u.address || u.size<64)return false;
    float m[16],x0=9,y0=9,x1=-9,y1=-9;memcpy(m,reinterpret_cast<const void*>(u.address),64);
    for(int i=0;i<4;i++){float f[2];memcpy(f,reinterpret_cast<const uint8_t*>(v.address)+20*i,8);float nx=(m[0]*f[0]+m[4]*f[1]+m[12]+1)/2,ny=(m[1]*f[0]+m[5]*f[1]+m[13]+1)/2;
        x0=std::min(x0,nx);x1=std::max(x1,nx);y0=std::min(y0,ny);y1=std::max(y1,ny);}
    return x0<0.002f && x1>0.998f && y0<0.002f && std::fabs(y1-float(hf))<0.002f;
}
void draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){preflight_cpu_reads();CPU_SCOPE(Draw);DrawRoot draw_timing_root(&cpu_scope);provenance_draw();
    need(state.rt.n>=0 && state.rt.n<=8,"Invalid draw color target count");
    VkShaderModule modules[5]{};for(unsigned i=0;i<5;i++)if(state.shader[i])modules[i]=state.shader[i]->module;
    uint32_t active_colors=0;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i])active_colors|=1u<<i;
    static std::vector<VkPipelineShaderStageCreateInfo> stages;stages.assign(5,VkPipelineShaderStageCreateInfo{});uint32_t stage_count=0;   // draw(): graphics thread only, not re-entrant
    int stage_result=xbg_graphics_stages(modules,state.rt.depth!=nullptr,active_colors,state.mask.mask,state.ms.a2c,stages.data(),&stage_count);
    if(stage_result)xbg_fatalf("Invalid graphics stage/depth-only state: %d",stage_result);stages.resize(stage_count);need(count>=0 && first>=0,"Negative draw range");if(!count || (indexed && indexed->instances==0))return;provenance_draw();
    const int orig_count=count;std::vector<uint32_t> quad_indices;p_draw_idx quad_indexed{};
    if(prim==7 && !indexed){
        need(instances<=INT32_MAX && first_instance<=INT32_MAX,"Quad instance range exceeds backend limits");
        if(!instances)return;
        uint32_t expanded=0;need(xbg_quad_index_count(count,&expanded)==0,"Quad index expansion exceeds 64MiB bound");
        if(!expanded)return;
        quad_indices.resize(expanded);xbg_draw_call source{7,-1,first,count,int32_t(instances),int32_t(first_instance),0,0},triangles{};
        need(xbg_quad_plan(&source,&triangles,quad_indices.data(),quad_indices.size())==0,"Quad expansion failed");
        quad_indexed={triangles.primitive,triangles.index_type,triangles.count,triangles.base_vertex,triangles.base_instance,triangles.instances,triangles.index_address};
        prim=4;first=0;count=triangles.count;indexed=&quad_indexed;
    }
    if(capture_begin("draw",stats.presents))capture_log("DRAW number=%llu vs=%s ps=%s prim=%d first=%d count=%d indexed=%u instances=%u baseInstance=%u baseVertex=%d",(unsigned long long)stats.draws.load(),state.shader[0]->key.c_str(),state.shader[4]?state.shader[4]->key.c_str():"none",prim,first,count,indexed!=nullptr,indexed?indexed->instances:instances,indexed?indexed->base_instance:first_instance,indexed?indexed->base_vertex:0);
    double ui_uy=0,ui_vs=1;g_ui_moff=0;g_ui_moff_addr=0;
    if(g_ui_rect_fn && orig_count==4 && state.shader[0]->inputs.size()==2 && state.stream[0].stride==20){double uh=1;g_ui_rect_fn(&ui_uy,&uh);
        if(uh>0 && uh<1){if(ui_cover_all(uh))ui_vs=1/uh;else if(ui_uy>0){auto u=state.ub[0][0];if(u.address && u.size>=64 && u.size<=4096){g_ui_moff=2*ui_uy;g_ui_moff_addr=u.address;}}}
        if(ui_vs==1 && g_ui_moff==0)ui_uy=0;}
    Commands c(true);Descriptors &d=acquire_descriptors();descriptors(d,c,false);Pass p;create_pass(p,c);
    static std::vector<VkVertexInputAttributeDescription> attributes;attributes.clear();VkVertexInputBindingDescription binding_slots[16];uint32_t binding_used=0;
    int zero_binding=-1;
    for(size_t input=0;input<state.shader[0]->inputs.size();input++){
        uint32_t loc=state.shader[0]->inputs[input];need(loc<16,"Vertex input location out of bounds");va_st a;memcpy(&a,&state.attrib[loc],4);
        if(!a.fmt){
            if(zero_binding<0)zero_binding=xbg_vertex_zero_binding(state.attrib,state.shader[0]->inputs.data(),state.shader[0]->inputs.size());
            need(zero_binding>=0,"No free binding for constant vertex input");
            uint32_t cls=state.shader[0]->input_classes[input];need(cls<=2,"Unknown vertex scalar type");
            VkFormat f=cls==0?VK_FORMAT_R32G32B32A32_SFLOAT:cls==1?VK_FORMAT_R32G32B32A32_SINT:VK_FORMAT_R32G32B32A32_UINT;
            const VkFormatProperties &fp=format_properties(f);need(fp.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,"Constant vertex format unsupported");
            attributes.push_back({loc,uint32_t(zero_binding),f,0});need(zero_binding<16,"Zero binding out of range");binding_slots[zero_binding]={uint32_t(zero_binding),0,VK_VERTEX_INPUT_RATE_VERTEX};binding_used|=1u<<zero_binding;
            if(capture.active)capture_log("ATTRIBUTE location=%u constant=zero class=%u binding=%d",loc,cls,zero_binding);
            continue;
        }
        need(a.stream<16,"Vertex stream out of bounds");VkFormat fmt=format(a.fmt);const VkFormatProperties &fp=format_properties(fmt);need(fp.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,"Vertex format unsupported");need(state.stream[a.stream].divisor<=1,"Instance divisor >1 unsupported");attributes.push_back({loc,a.stream,fmt,a.offset});binding_slots[a.stream]={a.stream,state.stream[a.stream].stride,state.stream[a.stream].divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};binding_used|=1u<<a.stream;if(capture.active)capture_log("ATTRIBUTE location=%u stream=%u format=%u vk=%u offset=%u stride=%u divisor=%u",loc,a.stream,a.fmt,unsigned(fmt),a.offset,state.stream[a.stream].stride,state.stream[a.stream].divisor);
    }
    static std::vector<VkVertexInputBindingDescription> bindings;bindings.clear();for(uint32_t i=0;i<16;i++)if(binding_used&(1u<<i))bindings.push_back(binding_slots[i]);   // ascending binding order, as the former std::map
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=bindings.size();vi.pVertexBindingDescriptions=bindings.data();vi.vertexAttributeDescriptionCount=attributes.size();vi.pVertexAttributeDescriptions=attributes.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=topology(prim);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=state.poly.mode==2?VK_POLYGON_MODE_FILL:state.poly.mode==1?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_POINT;need(rs.polygonMode==VK_POLYGON_MODE_FILL || cfg.enabled_core.fillModeNonSolid,"Non-solid polygon mode disabled");rs.cullMode=state.poly.cull;rs.frontFace=state.poly.front?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE;rs.lineWidth=1;rs.depthBiasEnable=state.poly.offset_en!=0;rs.depthBiasConstantFactor=state.bias[1]*(state.rt.depth && state.rt.depth->d.format==0x32 && depth16_as_d24?256.0f:1.0f);
    rs.depthBiasSlopeFactor=state.bias[0];rs.depthBiasClamp=state.bias[2];need(!rs.depthBiasClamp || cfg.enabled_core.depthBiasClamp,"Depth bias clamp disabled");   // constant bias units: 2^-16 for D16, 2^-24 for the X8_D24 stand-in; slope and clamp do not depend on the format
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=p.samples;ms.alphaToCoverageEnable=state.ms.a2c;need(!state.ms.sl_en,"Custom sample locations unsupported");need(!state.ms.samples || state.ms.samples==uint32_t(p.samples),"NVN sample count differs from attachments");need(p.samples==VK_SAMPLE_COUNT_1_BIT || state.ms.enable,"Disabled multisampling on multisample target needs coverage emulation");
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=state.ds.dt;ds.depthWriteEnable=state.ds.dw;ds.depthCompareOp=VkCompareOp(state.ds.df-1);ds.stencilTestEnable=state.ds.st;ds.front=stencil(false);ds.back=stencil(true);ds.depthBoundsTestEnable=state.bounds.enable;ds.minDepthBounds=state.bounds.minimum;ds.maxDepthBounds=state.bounds.maximum;need(!ds.depthBoundsTestEnable || cfg.enabled_core.depthBounds,"Depth bounds feature disabled");
    need(!(ds.depthTestEnable || ds.stencilTestEnable) || state.rt.depth,"Depth/stencil state requires attachment");
    static std::vector<VkPipelineColorBlendAttachmentState> attachments;attachments.assign(size_t(state.rt.n),VkPipelineColorBlendAttachmentState{});for(int i=0;i<state.rt.n;i++){const blend_st &b=state.blend[i];xbg_draw2d convert{};need(xbg_2d_blend_from_nvn(&convert,(state.color.blend_en>>i)&1,b.src_rgb,b.dst_rgb,b.src_a,b.dst_a,b.eq_rgb,b.eq_a,(state.mask.mask>>(4*i))&15)==0,"Unsupported blend state");attachments[i]=convert.blend;}
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=attachments.size();blend.pAttachments=attachments.data();memcpy(blend.blendConstants,state.blend_color,16);need(state.color.logic_op==3,"Non-copy logic operation unsupported");
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
    VkPipelineTessellationStateCreateInfo tess{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};tess.patchControlPoints=state.patch_size;need((state.shader[1]!=nullptr)==(state.shader[2]!=nullptr),"Incomplete tessellation shader pair");
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.stageCount=stages.size();ci.pStages=stages.data();ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&blend;ci.pDynamicState=&dy;ci.pTessellationState=state.shader[1]?&tess:nullptr;ci.layout=d.pipeline_layout;ci.renderPass=p.pass;
    PipelineLease pipeline;{CPU_SCOPE(Pipeline);
        static std::vector<uint32_t> fast;fast.clear();bool use_fast=pipeline_fast_enabled && !ab_off("pipefast") && d.entry->definition.valid && p.definition->valid;
        bool memo=use_fast && pipe_memo && !ab_off("pipememo");
        if(memo){pipe_memo_fill(pipe_memo_next,&d.entry->definition,p.definition,p.pass,d.pipeline_layout,prim,p.samples,active_colors);
            if(pipe_memo_valid && !memcmp(&pipe_memo_next,&pipe_memo_key,sizeof pipe_memo_next)){pipeline.pipeline=pipe_memo_pipeline;++pipe_memo_hits;}else ++pipe_memo_misses;}
        if(use_fast && !pipeline.pipeline){uint64_t a=uint64_t(uintptr_t(&d.entry->definition)),b=uint64_t(uintptr_t(p.definition));uint32_t ids[4]={uint32_t(a),uint32_t(a>>32),uint32_t(b),uint32_t(b>>32)};
            static XbgPipelineKey fkey;fkey.count=0;fkey.valid=true;   // 16 KB: one static, graphics thread only
            if(xbg_graphics_pipeline_key(fkey,ci,{ids,2,true},{ids+2,2,true})){fast.assign(fkey.words,fkey.words+fkey.count);auto f=pipeline_fast.find(fast);if(f!=pipeline_fast.end()){pipeline.pipeline=f->second;++pipeline_fast_hits;}}else use_fast=false;}
        if(!pipeline.pipeline){XbgPipelineKey key;bool cacheable=xbg_graphics_pipeline_key(key,ci,d.entry->definition.view(),p.definition->view());pipeline_lookup(pipeline,key,cacheable,&ci,nullptr,async_skippable(attachments,ds));
            if(use_fast && cacheable && !pipeline.transient)pipeline_fast.emplace(fast,pipeline.pipeline);}
        if(memo && use_fast && pipeline.pipeline && !pipeline.transient){pipe_memo_key=pipe_memo_next;pipe_memo_pipeline=pipeline.pipeline;pipe_memo_valid=true;}}
    // gfx-async-pipelines: this draw is skipped while its pipeline builds in the background
    if(!pipeline.pipeline)return;
    if(pipe_stats){uint64_t fr=stats.presents.load();if(fr>=3000 && fr<3060){auto &u=pstat_use[pipeline.pipeline];++u.draws;if(u.what.empty())u.what=pstat_what()+" "+std::to_string(p.width)+"x"+std::to_string(p.height);}
        else if(fr>=3060 && !pstat_logged){pstat_logged=true;std::vector<std::pair<VkPipeline,PstatUse>> v(pstat_use.begin(),pstat_use.end());std::sort(v.begin(),v.end(),[](const auto &a,const auto &b){return a.second.draws>b.second.draws;});
            for(size_t i=0;i<v.size() && i<80;i++)xbg_logf("NVN PSTAT use pipeline=%p draws_per_frame=%.1f what=%s",(void*)v[i].first,v[i].second.draws/60.0,v[i].second.what.c_str());}}
    // Buffer copies precede the draw; original guest layout remains unchanged.
    static std::vector<std::pair<uint32_t,Buffer*>> vertex_buffers;vertex_buffers.clear();
    Buffer *ib=nullptr;VkIndexType index_type=VK_INDEX_TYPE_UINT16;
    {DRAW_SCOPE(VertexPrepare);for(auto &b:bindings){if(int(b.binding)==zero_binding){const uint32_t zero[4]={0};vertex_buffers.push_back({b.binding,d.keep(stream_buffer(sizeof(zero),zero,sizeof(zero)))});continue;}auto r=state.vb[b.binding];need(r.address && r.size,"Unbound vertex buffer");snap_site=1;vertex_buffers.push_back({b.binding,d.keep(snapshot(r.address,r.size))});snap_site=0;}
    if(indexed){need(indexed->itype>=0 && indexed->itype<=2 && indexed->instances>0 && indexed->base_instance>=0,"Invalid indexed draw");size_t bytes=size_t(count)*(1u<<indexed->itype);need(indexed->ibuf,"Unbound index buffer");if(indexed->itype==0){if(draw_detail)draw_timing.index(uint64_t(count));std::vector<uint16_t> expanded(count);const uint8_t *src=reinterpret_cast<const uint8_t*>(indexed->ibuf);for(int i=0;i<count;i++)expanded[i]=src[i];ib=d.keep(stream_buffer(expanded.size()*2,expanded.data(),expanded.size()*2));}else{index_type=indexed->itype==1?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;snap_site=2;ib=indexed==&quad_indexed?d.keep(stream_buffer(bytes,reinterpret_cast<void*>(indexed->ibuf),bytes)):d.keep(snapshot(uint64_t(indexed->ibuf),bytes));snap_site=0;}}
    } // VertexPrepare excludes capture, recording and storage writeback.
    if(capture.active){for(auto &v:vertex_buffers){char label[40];snprintf(label,sizeof(label),"vertex-stream%u",v.first);capture_log("VERTEX label=%s guest=%#llx bytes=%zu",label,(unsigned long long)state.vb[v.first].address,v.second->size);capture_data(label,v.second->mapped,v.second->size);}if(ib){capture_log("INDEX guest=%#llx vkType=%u bytes=%zu",(unsigned long long)indexed->ibuf,unsigned(index_type),ib->size);capture_data("index",ib->mapped,ib->size);}for(auto &input:d.capture_inputs)for(auto *output:p.images)if(input.image==output)capture_log("FEEDBACK input=%s serial=%llu",input.label.c_str(),(unsigned long long)output->source.serial);}
    VkQueryPool occlusion{};uint32_t occlusion_index=0;const bool counting=state.count_samples && !no_occlusion_queries && !ab_off("occ");
    const bool new_run=counting && (!occlusion_batch_enabled || ab_off("query") || !open_occlusion.cmd || open_pass.cmd!=c.command || open_pass.pass!=p.pass || open_pass.framebuffer!=p.framebuffer);
    if(new_run){need(cfg.enabled_core.occlusionQueryPrecise,"Precise samples-passed query disabled");occlusion_query(occlusion,occlusion_index);}   // a new pool ends the pass first
    if(open_pass.cmd!=c.command || open_pass.pass!=p.pass || open_pass.framebuffer!=p.framebuffer){end_open_pass();
        // Proven full overwrite: begin with the color-DONT_CARE twin; open_pass keeps p.pass, so later draws continue this pass.
        if(p.pass_noload && quad_indices.empty() && !ab_off("noload") && ui_uy==0 && ui_vs==1 && first_draw_covers(p,prim,first,count,indexed,vertex_buffers,ib,index_type)){VkRenderPass keep=p.pass;p.pass=p.pass_noload;begin_pass(p,c);p.pass=keep;++noload_passes;}
        else begin_pass(p,c);
        open_pass.cmd=c.command;open_pass.pass=p.pass;open_pass.framebuffer=p.framebuffer;open_pass.images=p.images;open_pass.depth_attached=state.rt.depth!=nullptr;++perf.passes;}
    if(state.ds.dt || state.ds.st || state.bounds.enable)open_pass.depth_used=true;
    if(new_run){end_occlusion_query();begin_occlusion_query(c.command,occlusion,occlusion_index);}
    BindCache &bc=bind_cache;if(!bind_cache_enabled || bc.cmd!=c.command || pipeline.transient){bc=BindCache{};bc.cmd=c.command;}
    if(bc.pipe!=pipeline.pipeline){vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.pipeline);bc.pipe=pipeline.pipeline;}else ++binds_skipped;
    {const uint32_t no=uint32_t(d.dynamic_offsets.size());
     if(bc.layout!=d.pipeline_layout || bc.set!=d.set || bc.noff!=no || no>32 || (no && memcmp(bc.off,d.dynamic_offsets.data(),no*4))){
        ++set_binds;set_bind_offsets+=no;
        if(bc.layout==d.pipeline_layout && bc.set==d.set){++set_binds_offsets_only;
            if(bc.noff==no && no<=32){unsigned changed=0;for(uint32_t k=0;k<no;k++)if(bc.off[k]!=d.dynamic_offsets[k]){++changed;++offset_changed_at[k];}++offset_changes[changed<9?changed:9];}}
        vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,d.pipeline_layout,0,1,&d.set,no,d.dynamic_offsets.data());
        if(no<=32){bc.layout=d.pipeline_layout;bc.set=d.set;bc.noff=no;if(no)memcpy(bc.off,d.dynamic_offsets.data(),no*4);}else bc.noff=UINT32_MAX;}
     else ++binds_skipped;}
    VkViewport viewport{};need(xbg_nvn_viewport(&viewport,state.viewport.x,state.viewport.y,state.viewport.width,state.viewport.height*float(ui_vs),state.viewport.minDepth,state.viewport.maxDepth)==0,"Invalid NVN viewport");
    need(viewport.width<=properties.limits.maxViewportDimensions[0] && -viewport.height<=properties.limits.maxViewportDimensions[1] && viewport.x>=properties.limits.viewportBoundsRange[0] && viewport.x+viewport.width<=properties.limits.viewportBoundsRange[1] && viewport.y>=properties.limits.viewportBoundsRange[0] && viewport.y<=properties.limits.viewportBoundsRange[1] && viewport.y+viewport.height>=properties.limits.viewportBoundsRange[0],"NVN viewport exceeds device bounds");
    if(!bc.has_vp || memcmp(&bc.vp,&viewport,sizeof viewport)){vkCmdSetViewport(c.command,0,1,&viewport);bc.vp=viewport;bc.has_vp=true;}else ++binds_skipped;
    VkRect2D sc=scissor(p);
    if(ui_uy>0 || ui_vs!=1){long so=g_ui_moff!=0?lround(ui_uy*p.height):0;int64_t y0=sc.offset.y<=0?0:sc.offset.y+so,y1=ui_vs!=1?int64_t(p.height):int64_t(sc.offset.y)+sc.extent.height+so;   // 剪裁跟着下移；从顶边起的不动（宿主上提的贴顶控件） | the scissor moves down with it; ones starting at the top edge stay (controls the host lifted to the top)
        if(y1>int64_t(p.height))y1=p.height;if(y1<y0)y1=y0;sc.offset.y=int32_t(y0);sc.extent.height=uint32_t(y1-y0);}
    if(!bc.has_sc || memcmp(&bc.sc,&sc,sizeof sc)){vkCmdSetScissor(c.command,0,1,&sc);bc.sc=sc;bc.has_sc=true;}else ++binds_skipped;
    for(auto &b:vertex_buffers){VkDeviceSize off=b.second->offset;if(b.first<16 && bc.vb[b.first]==b.second->buffer && bc.vbo[b.first]==off){++binds_skipped;continue;}
        vkCmdBindVertexBuffers(c.command,b.first,1,&b.second->buffer,&off);if(b.first<16){bc.vb[b.first]=b.second->buffer;bc.vbo[b.first]=off;}}
    pass_prof_draw(c.command);
    if(indexed){if(bc.ib!=ib->buffer || bc.ibo!=ib->offset || bc.it!=index_type){vkCmdBindIndexBuffer(c.command,ib->buffer,ib->offset,index_type);bc.ib=ib->buffer;bc.ibo=ib->offset;bc.it=index_type;}else ++binds_skipped;vkCmdDrawIndexed(c.command,count,indexed->instances,0,indexed->base_vertex,indexed->base_instance);}else vkCmdDraw(c.command,count,instances,first,first_instance);
    if(!occlusion_batch_enabled)end_occlusion_query();c.finish();++perf.draws;++pass_prof_draws;pass_prof_verts+=uint64_t(uint32_t(count))*instances;
    if(d.needs_writeback() || capture.active){unsigned saved=cur_op;cur_op=62;end_open_pass();full_barrier(c.command);cur_op=saved;VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);}
    {DRAW_SCOPE(Writeback);if(capture.active){if(d.needs_writeback())gpu_sync(perf.sync_writeback);d.writeback();}else d.defer_writeback();}stats.draws.fetch_add(1,std::memory_order_relaxed);   // diagnostics only: a seq_cst add (LDADDAL) waits for every earlier store, ~2.7% of the render thread
    if(capture.active){capture_pass(p);d.capture_read_images();capture.active=false;}
}
// 3D 画面加亮（iOS dm/gfx/nvn/nvn_mtl.m bright_lut 的 Vulkan 版；#112 vickwv fe7b655 在 Android 上做过同样的移植）：游戏每帧用
// 计算着色器 552f45c348a61b86 算色调映射的 32^3 调色表；那次派发之后把表拷一份，用 xbg_bright_lut.comp 按宿主 dm_bright_k() 给的
// 曝光 k 原地重写。k ≈ 1（局外、正午）不动。只碰 3D 世界：UI、文字、血球在色调映射之后才合成。
// 开关 gfx-bright / DM_GFX_BRIGHT（缺省开），A/B 目标 bright；DM_BRIGHT_K=<k>（宿主）固定曝光。
// 3D picture brightening (the Vulkan version of bright_lut in iOS dm/gfx/nvn/nvn_mtl.m; #112, vickwv fe7b655, ported the same idea
// to Android): every frame the game computes tone mapping's 32^3 color table with compute shader 552f45c348a61b86; after that
// dispatch the table is copied and rewritten in place by xbg_bright_lut.comp with the exposure k from the host's dm_bright_k().
// Nothing changes at k ~= 1 (out of game, noon). Only the 3D world changes: UI, text and the orbs are composited after tone mapping.
// Switch gfx-bright / DM_GFX_BRIGHT (on by default), A/B target bright; DM_BRIGHT_K=<k> (host) fixes the exposure.
namespace bright {
bool enabled=true,failed=false;uint64_t applied=0;float last_k=1;
VkDescriptorSetLayout set_layout{};VkPipelineLayout layout{};VkPipeline pipeline{};VkSampler sampler{};VkDescriptorPool pool{};
// 每个帧槽 8 套描述符：一帧里可能派发不止一次，录进命令缓冲的套在提交完成前不能再改
// 8 descriptor sets per frame slot: a frame may dispatch more than once, and a set recorded into a command buffer must not change before that submission completes
constexpr unsigned SETS=8;VkDescriptorSet sets[2][SETS]{};unsigned used[2]{};uint64_t used_serial[2]{};
VkImage tmp{};VkDeviceMemory tmp_memory{};VkImageView tmp_view{};VkFormat tmp_format{};uint32_t tmp_w=0,tmp_h=0,tmp_d=0;bool tmp_general=false;
void shutdown(){
    if(tmp_view)vkDestroyImageView(cfg.device,tmp_view,nullptr);if(tmp)vkDestroyImage(cfg.device,tmp,nullptr);if(tmp_memory)vkFreeMemory(cfg.device,tmp_memory,nullptr);
    if(pipeline)vkDestroyPipeline(cfg.device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(cfg.device,layout,nullptr);if(pool)vkDestroyDescriptorPool(cfg.device,pool,nullptr);
    if(set_layout)vkDestroyDescriptorSetLayout(cfg.device,set_layout,nullptr);if(sampler)vkDestroySampler(cfg.device,sampler,nullptr);
    tmp_view={};tmp={};tmp_memory={};tmp_w=tmp_h=tmp_d=0;tmp_general=false;pipeline={};layout={};pool={};set_layout={};sampler={};for(auto &s:sets)for(auto &x:s)x={};used[0]=used[1]=0;used_serial[0]=used_serial[1]=0;failed=false;
}
bool prepare(){
    if(pipeline)return true;
    if(!cfg.enabled_core.shaderStorageImageWriteWithoutFormat){xbg_logf("NVN bright: off (shaderStorageImageWriteWithoutFormat not enabled)");return false;}
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK(vkCreateSampler(cfg.device,&si,nullptr,&sampler));
    VkDescriptorSetLayoutBinding b[2]{};b[0]={0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};b[1]={1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=2;dl.pBindings=b;VK(vkCreateDescriptorSetLayout(cfg.device,&dl,nullptr,&set_layout));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&set_layout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;
    VK(vkCreatePipelineLayout(cfg.device,&li,nullptr,&layout));
    VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*SETS},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2*SETS}};VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.maxSets=2*SETS;pi.poolSizeCount=2;pi.pPoolSizes=sizes;
    VK(vkCreateDescriptorPool(cfg.device,&pi,nullptr,&pool));
    VkDescriptorSetLayout layouts[2*SETS];for(auto &l:layouts)l=set_layout;VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=2*SETS;ai.pSetLayouts=layouts;
    VK(vkAllocateDescriptorSets(cfg.device,&ai,&sets[0][0]));
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=sizeof(xbg_bright_lut_cs);mi.pCode=xbg_bright_lut_cs;VkShaderModule module{};VK(vkCreateShaderModule(cfg.device,&mi,nullptr,&module));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";ci.layout=layout;
    VkResult r=vkCreateComputePipelines(cfg.device,pipeline_cache,1,&ci,nullptr,&pipeline);vkDestroyShaderModule(cfg.device,module,nullptr);
    if(r!=VK_SUCCESS){pipeline={};xbg_logf("NVN bright: off (compute pipeline result=%d)",int(r));return false;}
    xbg_logf("NVN bright: ready (color table rewrite, exposure from dm_bright_k)");return true;
}
bool prepare_tmp(const Image &lut){
    if(tmp && tmp_format==lut.fmt && tmp_w==lut.width && tmp_h==lut.height && tmp_d==lut.depth)return true;
    if(tmp){VkImage i=tmp;VkImageView v=tmp_view;VkDeviceMemory m=tmp_memory;retire_handle([i,v,m]{vkDestroyImageView(cfg.device,v,nullptr);vkDestroyImage(cfg.device,i,nullptr);vkFreeMemory(cfg.device,m,nullptr);});tmp={};tmp_view={};tmp_memory={};}
    VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(cfg.physical,lut.fmt,&fp);
    if(!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)){xbg_logf("NVN bright: off (color table format %d cannot be sampled linearly)",int(lut.fmt));return false;}
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.imageType=VK_IMAGE_TYPE_3D;ic.format=lut.fmt;ic.extent={lut.width,lut.height,lut.depth};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;
    ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(vkCreateImage(cfg.device,&ic,nullptr,&tmp)!=VK_SUCCESS){tmp={};return false;}
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(cfg.device,tmp,&mr);VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=tmp;vi.viewType=VK_IMAGE_VIEW_TYPE_3D;vi.format=lut.fmt;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkAllocateMemory(cfg.device,&ma,nullptr,&tmp_memory)!=VK_SUCCESS || vkBindImageMemory(cfg.device,tmp,tmp_memory,0)!=VK_SUCCESS || vkCreateImageView(cfg.device,&vi,nullptr,&tmp_view)!=VK_SUCCESS){
        if(tmp_view)vkDestroyImageView(cfg.device,tmp_view,nullptr);vkDestroyImage(cfg.device,tmp,nullptr);if(tmp_memory)vkFreeMemory(cfg.device,tmp_memory,nullptr);tmp={};tmp_view={};tmp_memory={};return false;}
    tmp_format=lut.fmt;tmp_w=lut.width;tmp_h=lut.height;tmp_d=lut.depth;tmp_general=false;
    xbg_logf("NVN bright: color table %ux%ux%u format=%d",tmp_w,tmp_h,tmp_d,int(tmp_format));return true;
}
// 计算着色器在储存图像槽上绑的那张图（调色表）；认不出返回 nullptr
// The image bound at the compute shader's storage image slot (the color table); nullptr when it cannot be identified
Image *bound_storage_image(){
    for(const auto &b:state.shader[5]->bindings)if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b.source==XBG_SOURCE_IMAGE && b.slot<16){
        int id=int(state.img[5][b.slot]&0xfffff);if(!id || (state.texture_pool.n>0 && id>=state.texture_pool.n))return nullptr;
        auto pool=textures.find(pool_key(&state.texture_pool));auto &table=pool==textures.end()?legacy_textures:pool->second;auto tex=table.find(id);if(tex==table.end())return nullptr;
        return &get_image(&tex->second.texture,true);}
    return nullptr;
}
// 在调色表那次派发之后、同一个非绘制命令段里调用（前后都有全屏障）
// Called after the color table's dispatch, inside the same non-draw command span (full barriers before and after)
void after_dispatch(Commands &c){
    if(!enabled || failed || !dm_bright_k || state.shader[5]->key.compare(0,16,"552f45c348a61b86") || ab_off("bright"))return;
    float k=dm_bright_k();last_k=k;if(!(k>1.005f))return;if(k>4.0f)k=4.0f;
    Image *lut=bound_storage_image();
    if(!lut || lut->depth<2 || lut->levels!=1 || lut->aspects!=VK_IMAGE_ASPECT_COLOR_BIT || !(lut->usage&VK_IMAGE_USAGE_STORAGE_BIT) || !(lut->usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)){
        static bool told=false;if(!told){told=true;xbg_logf("NVN bright: color table not recognised (image=%p), not brightening",(void*)lut);}return;}
    if(!prepare() || !prepare_tmp(*lut)){failed=true;return;}
    unsigned si=unsigned(&recording_slot()-slots)&1;if(used_serial[si]!=slot_serial){used_serial[si]=slot_serial;used[si]=0;}
    if(used[si]>=SETS)return;VkDescriptorSet set=sets[si][used[si]++];
    VkCommandBuffer cmd=c.command;
    if(lut->layout!=VK_IMAGE_LAYOUT_GENERAL)lut->transition(cmd,VK_IMAGE_LAYOUT_GENERAL);
    if(!tmp_general){VkImageMemoryBarrier t{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};t.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;t.newLayout=VK_IMAGE_LAYOUT_GENERAL;t.image=tmp;t.srcQueueFamilyIndex=t.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        t.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};t.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&t);tmp_general=true;}
    full_barrier(cmd);
    VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;region.extent={lut->width,lut->height,lut->depth};
    vkCmdCopyImage(cmd,lut->image,VK_IMAGE_LAYOUT_GENERAL,tmp,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    full_barrier(cmd);
    VkDescriptorImageInfo ii[2]{};ii[0]={sampler,tmp_view,VK_IMAGE_LAYOUT_GENERAL};ii[1]={VK_NULL_HANDLE,lut->view(nullptr,false,true),VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};for(int i=0;i<2;i++){w[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w[i].dstSet=set;w[i].dstBinding=uint32_t(i);w[i].descriptorCount=1;w[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[i].pImageInfo=&ii[i];}
    vkUpdateDescriptorSets(cfg.device,2,w,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&k);
    vkCmdDispatch(cmd,(lut->width+3)/4,(lut->height+3)/4,(lut->depth+3)/4);
    full_barrier(cmd);
    ++applied;
}
}
void dispatch(const p_dispatch &p){preflight_cpu_reads();CPU_SCOPE(Dispatch);provenance_log("DISPATCH shader=%s",state.shader[5]?state.shader[5]->key.c_str():"null");need(state.shader[5],"Dispatch has no genuine compute shader");need(p.x>=0 && p.y>=0 && p.z>=0,"Negative compute dispatch");if(!p.x||!p.y||!p.z)return;need(uint32_t(p.x)<=properties.limits.maxComputeWorkGroupCount[0] && uint32_t(p.y)<=properties.limits.maxComputeWorkGroupCount[1] && uint32_t(p.z)<=properties.limits.maxComputeWorkGroupCount[2],"Compute dispatch exceeds device limit");Commands c;Descriptors &d=acquire_descriptors();descriptors(d,c,true);VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=state.shader[5]->module;ci.stage.pName="main";ci.layout=d.pipeline_layout;PipelineLease pipeline;{CPU_SCOPE(Pipeline);XbgPipelineKey key;bool cacheable=xbg_compute_pipeline_key(key,ci,d.entry->definition.view());pipeline_lookup(pipeline,key,cacheable,nullptr,&ci);}vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline.pipeline);vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,d.pipeline_layout,0,1,&d.set,uint32_t(d.dynamic_offsets.size()),d.dynamic_offsets.data());int prof_dispatch=pass_prof_compute_begin(c.command,p);vkCmdDispatch(c.command,p.x,p.y,p.z);pass_prof_complete(c.command,prof_dispatch);bright::after_dispatch(c);VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);c.finish();++perf.dispatches;d.defer_writeback();++stats.dispatches;}

struct MaskedClear {
    VkShaderModule vs{},fs{};VkPipelineLayout layout{};VkPipeline pipeline{};
    ~MaskedClear(){if(pipeline)vkDestroyPipeline(cfg.device,pipeline,nullptr);if(layout)vkDestroyPipelineLayout(cfg.device,layout,nullptr);if(fs)vkDestroyShaderModule(cfg.device,fs,nullptr);if(vs)vkDestroyShaderModule(cfg.device,vs,nullptr);}
    void prepare(const Pass &p,int attachment,int scalar,uint32_t cm,uint32_t sm,uint32_t reference){
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};si.codeSize=sizeof(xbg_clear_vs);si.pCode=xbg_clear_vs;VK(vkCreateShaderModule(cfg.device,&si,nullptr,&vs));
        if(cm){const uint32_t *words=scalar==1?xbg_clear_fs_sint:scalar==2?xbg_clear_fs_uint:xbg_clear_fs_float;size_t bytes=scalar==1?sizeof(xbg_clear_fs_sint):scalar==2?sizeof(xbg_clear_fs_uint):sizeof(xbg_clear_fs_float);std::vector<uint32_t> patched(words,words+bytes/4);patched[xbg_clear_fs_location_word]=uint32_t(attachment);si.codeSize=bytes;si.pCode=patched.data();VK(vkCreateShaderModule(cfg.device,&si,nullptr,&fs));}
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,16};VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.pushConstantRangeCount=cm?1:0;li.pPushConstantRanges=cm?&push:nullptr;VK(vkCreatePipelineLayout(cfg.device,&li,nullptr,&layout));
        VkPipelineShaderStageCreateInfo stages[2]{};for(int i=0;i<(cm?2:1);i++){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=i?fs:vs;stages[i].pName="main";}
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=p.samples;
        VkPipelineColorBlendAttachmentState color{};VkPipelineDepthStencilStateCreateInfo ds{};need(xbg_masked_clear_state(cm,sm,reference,&color,&ds)==0,"Invalid masked clear state");
        // Every other subpass color slot is UNUSED. Identical states across all
        // slots avoid any dependency on the optional independentBlend feature.
        std::vector<VkPipelineColorBlendAttachmentState> colors(p.color_slots,color);VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=colors.size();blend.pAttachments=colors.data();
        VkDynamicState dyn[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dyn;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.stageCount=cm?2:1;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&blend;ci.pDynamicState=&dynamic;ci.layout=layout;ci.renderPass=p.pass;
        VK(vkCreateGraphicsPipelines(cfg.device,pipeline_cache,1,&ci,nullptr,&pipeline));
    }
    void encode(Commands &c,const Pass &p,const VkRect2D &rect,const void *color){
        bind_cache_reset();VkViewport viewport{0,0,float(p.width),float(p.height),0,1};vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdSetViewport(c.command,0,1,&viewport);vkCmdSetScissor(c.command,0,1,&rect);
        if(color)vkCmdPushConstants(c.command,layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,16,color);vkCmdDraw(c.command,3,1,0,0);
    }
};
void clear_attachment(const p_clear_color *color,const p_clear_ds *depth,int scalar=0){preflight_cpu_reads();
    VkClearAttachment a{};uint32_t cm=0,sm=0;bool partial=false;p_clear_color converted{};
    if(color){
        need(color->index>=0 && color->index<state.rt.n && state.rt.color[color->index],"Clear color attachment missing");need((uint32_t(color->mask)&~15u)==0,"Invalid color clear mask");cm=uint32_t(color->mask);
        Image &target=get_image(state.rt.color[color->index],false);const view_t *view=state.rt.has_cview[color->index]?&state.rt.cview[color->index]:nullptr;
        VkFormat viewed=target.view_format(view);static unsigned notes=0;if(notes++<8){uint32_t bits[4];memcpy(bits,color->rgba,16);xbg_logf("NVN clear-value attachment input=%d nvn=%#x view=%#x vk=%u mask=%#x bits=%08x/%08x/%08x/%08x",scalar,target.source.d.format,view&&view->set_format?view->format:target.source.d.format,unsigned(viewed),cm,bits[0],bits[1],bits[2],bits[3]);}VkClearColorValue value{};int converted_scalar=xbg_clear_value(viewed,scalar,color->rgba,&value);
        if(converted_scalar<0)xbg_fatalf("Color clear conversion failed nvn=%#x vk=%u input=%d rc=%d",target.source.d.format,unsigned(viewed),scalar,converted_scalar);
        converted=*color;memcpy(converted.rgba,&value,16);color=&converted;scalar=converted_scalar;
        if(cm)a.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;a.colorAttachment=color->index;memcpy(&a.clearValue.color,color->rgba,16);partial=cm && cm!=15;
    }else{
        need(state.rt.depth,"Clear depth attachment missing");sm=uint32_t(depth->stencil_mask)&255;
        VkImageAspectFlags requested=(depth->depth_mask?VK_IMAGE_ASPECT_DEPTH_BIT:0)|(sm?VK_IMAGE_ASPECT_STENCIL_BIT:0);need((aspect(state.rt.depth->d.format)&requested)==requested,"Clear requests absent depth/stencil aspect");
        if(depth->depth_mask){need(depth->depth>=0 && depth->depth<=1,"Depth clear value outside normalized range");a.aspectMask|=VK_IMAGE_ASPECT_DEPTH_BIT;}
        partial=sm && sm!=255;if(sm && !partial)a.aspectMask|=VK_IMAGE_ASPECT_STENCIL_BIT;a.clearValue.depthStencil={depth->depth,uint32_t(depth->stencil)&255};
    }
    if(!a.aspectMask && !partial)return;
    // Full (unmasked) clears are recorded inside the draw render pass of the
    // current targets, so a clear and the draws after it share one pass.
    if(!partial && a.aspectMask && !capture.trace && !provenance_file){
        const tex_t *t0=color?state.rt.color[color->index]:state.rt.depth;Image &ti=get_image(t0,false);
        const view_t *tv=color?(state.rt.has_cview[color->index]?&state.rt.cview[color->index]:nullptr):(state.rt.has_dview?&state.rt.dview:nullptr);
        uint32_t lvl=tv && tv->set_levels?tv->base_level:0;uint32_t cw=std::max(1u,ti.width>>lvl),ch=std::max(1u,ti.height>>lvl);VkRect2D keep{};
        bool needs_preserve=lvl<ti.levels && xbg_clear_rectangle(cw,ch,state.scissor.offset.x,state.scissor.offset.y,state.scissor.extent.width,state.scissor.extent.height,&keep)==0 && xbg_clear_requires_preserve(ti.levels,ti.layers,cw,ch,&keep,false,ti.aspects,a.aspectMask)!=0;
        if(!needs_preserve || ti.gpu_written || ti.uploaded){
            Commands c(true);Pass p;
            if(create_pass(p,c,-1,false,true)){
                if(open_pass.cmd!=c.command || open_pass.pass!=p.pass || open_pass.framebuffer!=p.framebuffer){end_open_pass();begin_pass(p,c);open_pass.cmd=c.command;open_pass.pass=p.pass;open_pass.framebuffer=p.framebuffer;open_pass.images=p.images;open_pass.depth_attached=state.rt.depth!=nullptr;++perf.passes;}
                if(depth)open_pass.depth_used=true;
                VkClearRect r{scissor(p),0,1};if(r.rect.extent.width && r.rect.extent.height)vkCmdClearAttachments(c.command,1,&a,1,&r);
                ++perf.folded_clears;return;
            }
        }
    }
    if(capture_begin(color?"clear-color":"clear-depth-stencil",stats.presents))capture_log("CLEAR target=%d scalar=%d colorMask=%u stencilMask=%u partial=%u",color?color->index:-1,scalar,cm,sm,unsigned(partial));
    const tex_t *target=color?state.rt.color[color->index]:state.rt.depth;Image &target_image=get_image(target,false);
    const view_t *target_view=color?(state.rt.has_cview[color->index]?&state.rt.cview[color->index]:nullptr):(state.rt.has_dview?&state.rt.dview:nullptr);
    uint32_t clear_level=target_view && target_view->set_levels?target_view->base_level:0;need(clear_level<target_image.levels,"Clear mip outside image");
    uint32_t clear_width=std::max(1u,target_image.width>>clear_level),clear_height=std::max(1u,target_image.height>>clear_level);VkRect2D preserve_rect{};
    need(xbg_clear_rectangle(clear_width,clear_height,state.scissor.offset.x,state.scissor.offset.y,state.scissor.extent.width,state.scissor.extent.height,&preserve_rect)==0,"Invalid clear scissor");
    bool preserve=xbg_clear_requires_preserve(target_image.levels,target_image.layers,clear_width,clear_height,&preserve_rect,partial,target_image.aspects,a.aspectMask)!=0;
    // LOAD preserves actual previous contents only after importing CPU-backed
    // storage on first use; never mark gpu_written before this preservation.
    if(preserve){need(target_image.samples==VK_SAMPLE_COUNT_1_BIT || target_image.gpu_written,"Partial clear of uninitialized CPU multisample storage unsupported");get_image(target,true);}
    // 后端合成的 masked-clear draw 不属于游戏遮挡计数；非 draw 的 Commands 随后也结束 pass。
    // Backend-generated masked-clear draws do not contribute to the game's occlusion count; the non-draw Commands then ends the pass.
    end_occlusion_query();
    Commands c;Pass p;create_pass(p,c,color?color->index:-1,depth!=nullptr);VkClearRect r{scissor(p),0,1};auto masked_object=std::make_shared<MaskedClear>();MaskedClear &masked=*masked_object;retire_object(masked_object);
    bool area=r.rect.extent.width && r.rect.extent.height;
    if(partial && area)masked.prepare(p,color?color->index:-1,scalar,cm,color?0:sm,color?0:uint32_t(depth->stencil));
    begin_pass(p,c);
    if(area){
        if(!color && a.aspectMask)vkCmdClearAttachments(c.command,1,&a,1,&r);
        if(partial)masked.encode(c,p,r.rect,color?color->rgba:nullptr);
        else if(color)vkCmdClearAttachments(c.command,1,&a,1,&r);
    }
    vkCmdEndRenderPass(c.command);pass_prof_end(c.command);c.finish(); // transient pipeline/modules stay alive through this fence
    if(capture.active){capture_pass(p);capture.active=false;}
}
void region_check(const Image &im,uint32_t level,uint32_t layer,const NVNcopyRegion &r){
    need(level<im.levels && layer<im.layers && r.xoffset>=0 && r.yoffset>=0 && r.zoffset>=0 && r.width>0 && r.height>0 && r.depth>0,"Invalid texture copy region");
    bool volume=im.source.d.target==2;uint64_t zlimit=volume?std::max(1u,im.depth>>level):im.layers;
    need(uint64_t(r.xoffset)+r.width<=std::max(1u,im.width>>level) && uint64_t(r.yoffset)+r.height<=std::max(1u,im.height>>level) && uint64_t(volume?0:layer)+r.zoffset+r.depth<=zlimit,"Texture copy region exceeds image");
}
VkImageSubresourceLayers copy_layers(const Image &im,uint32_t level,uint32_t layer,const NVNcopyRegion &r){return {im.aspects,level,im.source.d.target==2?0:layer+uint32_t(r.zoffset),im.source.d.target==2?1:uint32_t(r.depth)};}
VkOffset3D copy_offset(const Image &im,const NVNcopyRegion &r){return {r.xoffset,r.yoffset,im.source.d.target==2?r.zoffset:0};}
VkExtent3D copy_extent(const Image &im,const NVNcopyRegion &r){return {uint32_t(r.width),uint32_t(r.height),im.source.d.target==2?uint32_t(r.depth):1};}
// Buffer->texture copies write the texture's pool storage on Switch. Mirror that
// into guest memory (block-linear) so the texture stays CPU-authoritative: later
// CPU streaming writes into the same storage keep being detected and uploaded.
bool guest_texture_write(const tex_t *t,unsigned level,unsigned layer,const NVNcopyRegion &r,const uint8_t *src,size_t row,size_t image_stride){
    uint32_t row_bytes,rows,slices,bpb;uint64_t base;unsigned th,td;
    if(!t->mem || r.width<=0 || r.height<=0 || r.depth<=0 || r.xoffset<0 || r.yoffset<0 || r.zoffset<0)return false;
    if(nvn_deswizzle_geom(&t->d,level,layer,&row_bytes,&rows,&slices,&base,&th,&td,&bpb))return false;
    int bw=1,bh=1;if(!nvn_fmt_bpb(t->d.format,&bw,&bh) || r.xoffset%bw || r.yoffset%bh)return false;
    uint32_t bx0=r.xoffset/bw,by0=r.yoffset/bh,nbx=(r.width+bw-1)/bw,nby=(r.height+bh-1)/bh;
    if(uint64_t(bx0+nbx)*bpb>row_bytes || by0+nby>rows || row<size_t(nbx)*bpb)return false;
    bool volume=t->d.target==2;if(volume?uint32_t(r.zoffset+r.depth)>slices:layer+uint32_t(r.zoffset+r.depth)>t->L.layers)return false;
    for(int k=0;k<r.depth;k++){
        uint64_t b=base;uint32_t z=0;
        if(volume)z=uint32_t(r.zoffset+k);
        else{uint32_t a,b2,c,d2;uint64_t lb;unsigned h1,h2;if(nvn_deswizzle_geom(&t->d,level,layer+uint32_t(r.zoffset+k),&a,&b2,&c,&lb,&h1,&h2,&d2))return false;b=lb;}
        for(uint32_t y=0;y<nby;y++)for(uint32_t x=0;x<nbx;x++){
            uint64_t at=b+nvn_bl_offset((bx0+x)*bpb,by0+y,z,row_bytes,rows,th,td);if(at+bpb>t->L.size)return false;
            memcpy(t->mem+at,src+size_t(k)*image_stride+size_t(y)*row+size_t(x)*bpb,bpb);
        }
    }
    return true;
}
void copy_texture_buffer(const p_copy_b2t &request,bool to_buffer){preflight_cpu_reads();
    need(request.tex,"Null buffer image texture");
    tex_t stable=*request.tex;p_copy_b2t p=request;p.tex=&stable;
    // 同步必须先于Image映射/引用；深度拆平面、BC解码和颜色复制随后才可读源字节。
    // Synchronize before acquiring any Image mapping/reference; depth-plane splitting, BC decoding and color copying may read source bytes only afterwards.
    if(p.tex->mem && p.tex->L.size)cpu_access(uint64_t(uintptr_t(p.tex->mem)),p.tex->L.size,9);
    if(!to_buffer){
        int bw=1,bh=1;int bpb=nvn_fmt_bpb(p.tex->d.format,&bw,&bh);xbg_depth_format depth;
        if(xbg_depth_format_info(p.tex->d.format,&depth)==0){bw=bh=1;bpb=int(depth.packed_bytes);}
        size_t row,image,bytes;
        need(bpb>0 && nvn_copy_pitch(p.r.width,p.r.height,p.r.depth,bw,bh,bpb,state.copy_stride,state.copy_image_stride,&row,&image,&bytes) && bytes<=256ull*1024*1024,"Invalid buffer image CPU read range");
        cpu_access(p.src,bytes,9);
    }
    Image &im=get_image(p.tex,to_buffer);
    if(p.has_view && p.view.set_format)(void)im.view_format(&p.view);
    if(im.samples!=VK_SAMPLE_COUNT_1_BIT || (im.decoded&&to_buffer))   // 原生BC在下方独立读回 | Native BC reads back in its own branch below
        xbg_fatalf("NVN %s format=%#x samples=%u region=%d,%d,%d:%dx%dx%d needs %s",to_buffer?"CopyTextureToBuffer":"CopyBufferToTexture",p.tex->d.format,uint32_t(im.samples),p.r.xoffset,p.r.yoffset,p.r.zoffset,p.r.width,p.r.height,p.r.depth,im.decoded?"compressed readback encoding":"depth/stencil or multisample packing");
    need((p.flags&~NVN_COPY_FLAGS_ENGINE_2D_BIT)==0,"Buffer/texture copy flags not implemented");
    uint32_t level=p.has_view&&p.view.set_levels?p.view.base_level:0,layer=p.has_view&&p.view.set_layers?p.view.min_layer:0;region_check(im,level,layer,p.r);need(p.src,"Null buffer image copy address");
    bool full_destination=xbg_copy_is_full({im.width,im.height,im.source.d.target==2?im.depth:im.layers,im.levels,level,layer,p.r.xoffset,p.r.yoffset,p.r.zoffset,p.r.width,p.r.height,p.r.depth})!=0;
    if(!to_buffer && !full_destination)get_image(p.tex,true);

    if(im.aspects!=VK_IMAGE_ASPECT_COLOR_BIT){
        xbg_depth_format f;need(xbg_depth_format_info(p.tex->d.format,&f)==0,"Unknown NVN depth copy format");
        size_t row,image,bytes;
        need(nvn_copy_pitch(p.r.width,p.r.height,p.r.depth,1,1,f.packed_bytes,state.copy_stride,state.copy_image_stride,&row,&image,&bytes) && row%f.packed_bytes==0 && bytes<=256ull*1024*1024,"Invalid packed depth copy stride/extent");
        need(p.src<=UINT64_MAX-bytes,"Depth copy address overflow");
        std::vector<uint8_t> planes;std::vector<VkBufferImageCopy> copies;VkBufferImageCopy region{};
        region.imageSubresource=copy_layers(im,level,layer,p.r);region.imageOffset=copy_offset(im,p.r);region.imageExtent=copy_extent(im,p.r);
        append_depth_planes(planes,copies,p.tex->d.format,p.r.width,p.r.height,p.r.depth,
            to_buffer?nullptr:reinterpret_cast<const void*>(p.src),bytes,row,image,region);
        Buffer staging=to_buffer?readback_buffer(planes.size()):stream_buffer(planes.size(),planes.data(),planes.size(),false);for(auto &copy:copies)copy.bufferOffset+=staging.offset;
        Commands command;im.transition(command.command,to_buffer?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if(to_buffer)vkCmdCopyImageToBuffer(command.command,im.image,im.layout,staging.buffer,copies.size(),copies.data());
        else vkCmdCopyBufferToImage(command.command,staging.buffer,im.image,im.layout,copies.size(),copies.data());
        im.transition(command.command,VK_IMAGE_LAYOUT_GENERAL);command.finish();
        if(to_buffer){
            gpu_sync(perf.sync_readback);staging.invalidate();invalidate_cpu_range(p.src,bytes);const uint8_t *dp=nullptr,*sp=nullptr;
            for(auto &r:copies){if(r.imageSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT)dp=static_cast<const uint8_t*>(staging.mapped)+(r.bufferOffset-staging.offset);else sp=static_cast<const uint8_t*>(staging.mapped)+(r.bufferOffset-staging.offset);}
            size_t pixels=size_t(p.r.width)*p.r.height*p.r.depth;
            std::vector<uint8_t> d16;if(dp && p.tex->d.format==0x32 && depth16_as_d24){d16.resize(pixels*2);for(size_t i=0;i<pixels;i++){uint32_t v;memcpy(&v,dp+i*4,4);uint16_t k=uint16_t(d24_to_d16(v));memcpy(&d16[i*2],&k,2);}dp=d16.data();}
            need(xbg_depth_merge(p.tex->d.format,p.r.width,p.r.height,p.r.depth,reinterpret_cast<void*>(p.src),bytes,row,image,dp,pixels*f.depth_bytes,sp,pixels*f.stencil_bytes)==0,"Packed NVN depth readback merge failed");
        }else im.gpu_written=true,provenance_write(im,__LINE__);
        xbg_logf("NVN depth %s serial=%llu format=%#x level=%u layer=%u region=%dx%dx%d planes=%zu",to_buffer?"readback":"upload",(unsigned long long)p.tex->serial,p.tex->d.format,level,layer,p.r.width,p.r.height,p.r.depth,copies.size());return;
    }
    if(!im.decoded && p.tex->d.format>=0x41 && p.tex->d.format<=0x50){ // 原生BC保持块编码 | Native BC retains block encoding
        int bw,bh,bpb=nvn_fmt_bpb(p.tex->d.format,&bw,&bh);need(bw==4 && bh==4 && !(p.r.xoffset%4) && !(p.r.yoffset%4),"BC copy origin must be block aligned");
        need(p.r.width%4==0 || uint32_t(p.r.xoffset+p.r.width)==std::max(1u,im.width>>level),"BC copy width must reach a block or mip edge");
        need(p.r.height%4==0 || uint32_t(p.r.yoffset+p.r.height)==std::max(1u,im.height>>level),"BC copy height must reach a block or mip edge");
        XbgBcCopyRows plan{};need(xbg_bc_copy_rows(p.r.width,p.r.height,p.r.depth,bpb,state.copy_stride,state.copy_image_stride,&plan)==0,"Invalid BC copy row/image stride/extent");
        need(p.src<=UINT64_MAX-plan.guest_bytes,"BC copy address overflow");
        if(!to_buffer && !im.gpu_written && !(p.tex->d.flags&0x10)){cpu_access(p.src,plan.guest_bytes,9);invalidate_cpu_range(uint64_t(uintptr_t(p.tex->mem)),p.tex->L.size);
            if(guest_texture_write(p.tex,level,layer,p.r,reinterpret_cast<const uint8_t*>(p.src),plan.row,plan.image)){im.uploaded=false;im.checked_submission=UINT64_MAX;return;}}
        std::vector<uint8_t> packed;
        if(!to_buffer){
            cpu_access(p.src,plan.guest_bytes,9);
            // 部分覆盖前上传初始全部mip/层；已有GPU内容沿用，不重建或转码。
            // Upload initial mips/layers before partial writes; retain existing GPU contents without recreation or transcoding.
            if(!xbg_copy_is_full({im.width,im.height,im.layers,im.levels,level,layer,p.r.xoffset,p.r.yoffset,p.r.zoffset,p.r.width,p.r.height,p.r.depth}))get_image(p.tex,true);
            packed.resize(plan.packed_bytes);need(xbg_bc_copy_transfer(packed.data(),packed.size(),reinterpret_cast<const void*>(p.src),plan.guest_bytes,plan,false)==0,"BC upload packing failed");
        }
        Buffer b=to_buffer?readback_buffer(plan.packed_bytes):stream_buffer(plan.packed_bytes,packed.data(),plan.packed_bytes,false);
        Commands c;im.transition(c.command,to_buffer?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy r{};r.bufferOffset=b.offset;r.imageSubresource=copy_layers(im,level,layer,p.r);r.imageOffset=copy_offset(im,p.r);r.imageExtent=copy_extent(im,p.r);
        if(to_buffer)vkCmdCopyImageToBuffer(c.command,im.image,im.layout,b.buffer,1,&r);else vkCmdCopyBufferToImage(c.command,b.buffer,im.image,im.layout,1,&r);
        im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();
        if(to_buffer){gpu_sync(perf.sync_readback);b.invalidate();invalidate_cpu_range(p.src,plan.guest_bytes);
            need(xbg_bc_copy_transfer(reinterpret_cast<void*>(p.src),plan.guest_bytes,b.mapped,plan.packed_bytes,plan,true)==0,"BC readback unpacking failed");}
        else im.gpu_written=true,provenance_write(im,__LINE__);
        {static unsigned n=0;if(n++<20)xbg_logf("NVN BC native %s format=%#x level=%u layer=%u region=%dx%dx%d bytes=%zu",to_buffer?"readback":"upload",p.tex->d.format,level,layer,p.r.width,p.r.height,p.r.depth,plan.packed_bytes);}
        return;
    }
    if(im.decoded){
        int bw,bh,bpb=nvn_fmt_bpb(p.tex->d.format,&bw,&bh);need(bw==4 && bh==4 && !(p.r.xoffset%4) && !(p.r.yoffset%4),"BC upload origin must be block aligned");
        need(p.r.width%4==0 || uint32_t(p.r.xoffset+p.r.width)==std::max(1u,im.width>>level),"BC upload width must reach a block or mip edge");
        need(p.r.height%4==0 || uint32_t(p.r.yoffset+p.r.height)==std::max(1u,im.height>>level),"BC upload height must reach a block or mip edge");
        size_t row,image,source_bytes;
        need(nvn_copy_pitch(p.r.width,p.r.height,p.r.depth,bw,bh,bpb,state.copy_stride,state.copy_image_stride,&row,&image,&source_bytes) && row%bpb==0 && source_bytes<=256ull*1024*1024,"Invalid BC copy row stride/extent");
        need(p.src<=UINT64_MAX-source_bytes,"BC copy address overflow");
        size_t bytes=size_t(p.r.width)*p.r.height*p.r.depth*im.bpp;
        if(!im.gpu_written && !(p.tex->d.flags&0x10)){invalidate_cpu_range(uint64_t(uintptr_t(p.tex->mem)),p.tex->L.size);
            if(guest_texture_write(p.tex,level,layer,p.r,reinterpret_cast<const uint8_t*>(p.src),row,image)){im.uploaded=false;im.checked_submission=UINT64_MAX;return;}}
        need(bytes<=256ull*1024*1024,"Decoded BC staging exceeds memory bound");std::vector<uint8_t> decoded(bytes);
        int result=xbg_decode_bc_linear(p.tex->d.format,p.r.width,p.r.height,p.r.depth,reinterpret_cast<const void*>(p.src),source_bytes,row,image,decoded.data(),decoded.size());
        if(result)xbg_fatalf("NVN BC staging decode failed format=%#x level=%u result=%d",p.tex->d.format,level,result);
        Buffer buffer=stream_buffer(bytes,decoded.data(),bytes,false);Commands command;im.transition(command.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};region.bufferOffset=buffer.offset;region.imageSubresource=copy_layers(im,level,layer,p.r);region.imageOffset=copy_offset(im,p.r);region.imageExtent=copy_extent(im,p.r);
        vkCmdCopyBufferToImage(command.command,buffer.buffer,im.image,im.layout,1,&region);im.transition(command.command,VK_IMAGE_LAYOUT_GENERAL);command.finish();im.gpu_written=true,provenance_write(im,__LINE__);
        xbg_logf("NVN BC upload format=%#x level=%u layer=%u region=%dx%dx%d src=%zu decoded=%zu",p.tex->d.format,level,layer,p.r.width,p.r.height,p.r.depth,source_bytes,bytes);return;
    }
    size_t row,image,n;
    need(nvn_copy_pitch(p.r.width,p.r.height,p.r.depth,1,1,im.bpp,state.copy_stride,state.copy_image_stride,&row,&image,&n) && n<=256ull*1024*1024,"Invalid or oversized color copy stride/extent");
    need(p.src<=UINT64_MAX-n,"Color copy address overflow");
    if(!to_buffer && !im.gpu_written && !(p.tex->d.flags&0x10)){invalidate_cpu_range(uint64_t(uintptr_t(p.tex->mem)),p.tex->L.size);
        if(guest_texture_write(p.tex,level,layer,p.r,reinterpret_cast<const uint8_t*>(p.src),row,image)){im.uploaded=false;im.checked_submission=UINT64_MAX;return;}}
    size_t slice_alignment=4;while(slice_alignment%im.bpp)slice_alignment+=4;
    size_t staging_row=size_t(p.r.width)*im.bpp;
    size_t staging_image=((staging_row*size_t(p.r.height)+slice_alignment-1)/slice_alignment)*slice_alignment,staging_bytes=staging_image*size_t(p.r.depth);
    need(staging_bytes<=256ull*1024*1024,"Color staging exceeds memory bound");
    std::vector<uint8_t> upload;
    if(!to_buffer){upload.resize(staging_bytes);
        for(int k=0;k<p.r.depth;k++)for(int y=0;y<p.r.height;y++)
            memcpy(upload.data()+size_t(k)*staging_image+size_t(y)*staging_row,reinterpret_cast<const uint8_t*>(p.src)+size_t(k)*image+size_t(y)*row,size_t(p.r.width)*im.bpp);
    }
    Buffer b=to_buffer?readback_buffer(staging_bytes):stream_buffer(staging_bytes,upload.data(),staging_bytes,false);
    Commands c;im.transition(c.command,to_buffer?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // 每层单独记录，允许字节图距不整除行距。
    // Record each slice separately to allow byte image strides indivisible by row strides.
    std::vector<VkBufferImageCopy> copies;
    for(int k=0;k<p.r.depth;k++){
        VkBufferImageCopy r{};r.bufferOffset=b.offset+size_t(k)*staging_image;r.bufferRowLength=0;
        r.imageSubresource=copy_layers(im,level,layer,p.r);r.imageSubresource.layerCount=1;
        r.imageOffset=copy_offset(im,p.r);r.imageExtent={uint32_t(p.r.width),uint32_t(p.r.height),1};
        if(im.source.d.target==2)r.imageOffset.z+=k;else r.imageSubresource.baseArrayLayer+=k;
        copies.push_back(r);
    }
    if(to_buffer)vkCmdCopyImageToBuffer(c.command,im.image,im.layout,b.buffer,uint32_t(copies.size()),copies.data());else vkCmdCopyBufferToImage(c.command,b.buffer,im.image,im.layout,uint32_t(copies.size()),copies.data());im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();
    if(to_buffer){gpu_sync(perf.sync_readback);b.invalidate();invalidate_cpu_range(p.src,n);
        for(int k=0;k<p.r.depth;k++)for(int y=0;y<p.r.height;y++){
            size_t off=size_t(k)*image+size_t(y)*row;
            memcpy(reinterpret_cast<uint8_t*>(p.src)+off,static_cast<const uint8_t*>(b.mapped)+size_t(k)*staging_image+size_t(y)*staging_row,size_t(p.r.width)*im.bpp);
        }
    }else im.gpu_written=true,provenance_write(im,__LINE__);
}
void copy_texture(const p_copy_t2t &p){preflight_cpu_reads();Image &s=get_image(p.src,true),&d=get_image(p.dst,false);{static unsigned n=0;if(n++<60)xbg_logf("NVN COPYT2T src=%llu fmt=%#x %ux%ux%u L%u lv%u -> dst=%llu fmt=%#x %ux%u L%u lv%u decoded=%d/%d region=%d,%d,%d %dx%dx%d -> %d,%d,%d sv=%d/%u/%u dv=%d/%u/%u",(unsigned long long)p.src->serial,p.src->d.format,s.width,s.height,s.depth,s.layers,s.levels,(unsigned long long)p.dst->serial,p.dst->d.format,d.width,d.height,d.layers,d.levels,int(s.decoded),int(d.decoded),p.sr.xoffset,p.sr.yoffset,p.sr.zoffset,p.sr.width,p.sr.height,p.sr.depth,p.dr.xoffset,p.dr.yoffset,p.dr.zoffset,int(p.has_sv),p.has_sv?p.sv.base_level:0,p.has_sv?p.sv.min_layer:0,int(p.has_dv),p.has_dv?p.dv.base_level:0,p.has_dv?p.dv.min_layer:0);}
    if(p.has_sv && p.sv.set_format)(void)s.view_format(&p.sv);
    if(p.has_dv && p.dv.set_format)(void)d.view_format(&p.dv);
    // 已知ASTC-pack源是ASTC4x4，目的BC3及既有RGBA BC目标从建图起解码，不迁移已有内容。
    // Known ASTC-pack sources use ASTC4x4; BC3 and existing RGBA BC destinations are decoded at creation without migrating contents.
    if(!s.decoded && d.decoded && s.fmt>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK && s.fmt<=VK_FORMAT_ASTC_4x4_SRGB_BLOCK && (d.fmt==VK_FORMAT_R8G8B8A8_UNORM || d.fmt==VK_FORMAT_R8G8B8A8_SRGB)
       && p.sr.width==p.dr.width && p.sr.height==p.dr.height && p.sr.depth==p.dr.depth && s.source.d.target!=2 && d.source.d.target!=2){
        VkFormatProperties sf{},df{};vkGetPhysicalDeviceFormatProperties(cfg.physical,s.fmt,&sf);vkGetPhysicalDeviceFormatProperties(cfg.physical,d.fmt,&df);
        uint32_t sl=p.has_sv&&p.sv.set_levels?p.sv.base_level:0,dl=p.has_dv&&p.dv.set_levels?p.dv.base_level:0,sa=p.has_sv&&p.sv.set_layers?p.sv.min_layer:0,da=p.has_dv&&p.dv.set_layers?p.dv.min_layer:0;
        region_check(s,sl,sa,p.sr);region_check(d,dl,da,p.dr);need(&s!=&d,"Same-image copy needs overlap validation");
        bool full_destination=xbg_copy_is_full({d.width,d.height,d.layers,d.levels,dl,da,p.dr.xoffset,p.dr.yoffset,p.dr.zoffset,p.dr.width,p.dr.height,p.dr.depth})!=0;
        if(!full_destination)get_image(p.dst,true);
        XbgBlitRegion source{s.width,s.height,s.layers,s.levels,sl,sa,p.sr.xoffset,p.sr.yoffset,p.sr.zoffset,p.sr.width,p.sr.height,p.sr.depth};
        XbgBlitRegion destination{d.width,d.height,d.layers,d.levels,dl,da,p.dr.xoffset,p.dr.yoffset,p.dr.zoffset,p.dr.width,p.dr.height,p.dr.depth};
        VkImageBlit blit{};VkFilter filter{};
        need(xbg_astc_bc_plan(s.fmt,d.fmt,source,destination,uint32_t(p.flags),sf.optimalTilingFeatures,df.optimalTilingFeatures,&blit,&filter)==0,"ASTC to decoded-BC copy requires matching sRGB, supported regions/flags and blit features");
        Commands c;s.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);d.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdBlitImage(c.command,s.image,s.layout,d.image,d.layout,1,&blit,filter);
        s.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);d.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();d.gpu_written=true,provenance_write(d,__LINE__);
        {static unsigned n=0;if(n++<8)xbg_logf("NVN ASTC2BC blit src=%llu fmt=%#x -> dst=%llu fmt=%#x %dx%d lv%u/%u layer%u/%u",(unsigned long long)p.src->serial,p.src->d.format,(unsigned long long)p.dst->serial,p.dst->d.format,p.sr.width,p.sr.height,sl,dl,sa,da);}
        return;
    }
    need(!(s.fmt>=VK_FORMAT_ASTC_4x4_UNORM_BLOCK && s.fmt<=VK_FORMAT_ASTC_12x12_SRGB_BLOCK && d.source.d.format>=0x41 && d.source.d.format<=0x50 && !d.decoded),"ASTC blocks into a native BC texture: the ASTC pack needs decoded BC (marker gfx-native-bc = 0)");
    if(s.decoded || d.decoded)need(s.decoded && d.decoded && xbg_bc_view_compatible(s.source.d.format,d.source.d.format),"BC texture copy must retain compression and signedness");
    need((uint32_t(p.flags)&~3u)==0 && s.fmt==d.fmt && s.aspects==d.aspects && s.samples==d.samples,"Texture copy requires matching formats/aspects and supported flags");
    uint32_t sl=p.has_sv&&p.sv.set_levels?p.sv.base_level:0,dl=p.has_dv&&p.dv.set_levels?p.dv.base_level:0,sa=p.has_sv&&p.sv.set_layers?p.sv.min_layer:0,da=p.has_dv&&p.dv.set_layers?p.dv.min_layer:0;
    region_check(s,sl,sa,p.sr);region_check(d,dl,da,p.dr);need(&s!=&d,"Same-image copy needs overlap validation");need((s.source.d.target==2)==(d.source.d.target==2),"Mixed volume/array copy requires slice mapping");
    bool scaled=p.sr.width!=p.dr.width || p.sr.height!=p.dr.height || p.sr.depth!=p.dr.depth;
    VkImageBlit blit{};VkFilter filter{};
    if(scaled){
        need(!s.decoded && !d.decoded && s.source.d.target!=0 && s.source.d.target!=2 && s.source.d.target!=3 && d.source.d.target!=0 && d.source.d.target!=2 && d.source.d.target!=3 && s.aspects==VK_IMAGE_ASPECT_COLOR_BIT && s.samples==VK_SAMPLE_COUNT_1_BIT,"Scaled blit requires native single-sample color 2D/array images");
        need(s.view_format(p.has_sv?&p.sv:nullptr)==s.fmt && d.view_format(p.has_dv?&p.dv:nullptr)==d.fmt,"Scaled blit cannot reinterpret image views");
        VkFormatProperties sf{},df{};vkGetPhysicalDeviceFormatProperties(cfg.physical,s.fmt,&sf);vkGetPhysicalDeviceFormatProperties(cfg.physical,d.fmt,&df);
        XbgBlitRegion src{s.width,s.height,s.layers,s.levels,sl,sa,p.sr.xoffset,p.sr.yoffset,p.sr.zoffset,p.sr.width,p.sr.height,p.sr.depth};
        XbgBlitRegion dst{d.width,d.height,d.layers,d.levels,dl,da,p.dr.xoffset,p.dr.yoffset,p.dr.zoffset,p.dr.width,p.dr.height,p.dr.depth};
        need(xbg_blit_plan(src,dst,uint32_t(p.flags),xbg_clear_color_class(s.source.d.format),sf.optimalTilingFeatures,df.optimalTilingFeatures,&blit,&filter)==0,"Scaled blit region/type/filter/device capability unsupported");
    }
    // Import initial CPU contents before a partial destination overwrite; this
    // also preserves untouched mip levels and array layers for ordinary copies.
    bool full_destination=xbg_copy_is_full({d.width,d.height,d.source.d.target==2?d.depth:d.layers,d.levels,dl,da,p.dr.xoffset,p.dr.yoffset,p.dr.zoffset,p.dr.width,p.dr.height,p.dr.depth})!=0;
    if(!full_destination){need(d.samples==VK_SAMPLE_COUNT_1_BIT || d.gpu_written,"Partial copy into initial CPU multisample storage unsupported");get_image(p.dst,true);}
    if(scaled){Commands c;s.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);d.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdBlitImage(c.command,s.image,s.layout,d.image,d.layout,1,&blit,filter);s.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);d.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();d.gpu_written=true,provenance_write(d,__LINE__);return;}
    Commands c;s.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);d.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkImageCopy r{};r.srcSubresource=copy_layers(s,sl,sa,p.sr);r.dstSubresource=copy_layers(d,dl,da,p.dr);r.srcOffset=copy_offset(s,p.sr);r.dstOffset=copy_offset(d,p.dr);r.extent=copy_extent(s,p.sr);std::vector<VkImageCopy> regions;
    for(VkImageAspectFlags flag: {VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT),VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT)})if(s.aspects&flag){auto one=r;one.srcSubresource.aspectMask=one.dstSubresource.aspectMask=flag;regions.push_back(one);}
    vkCmdCopyImage(c.command,s.image,s.layout,d.image,d.layout,regions.size(),regions.data());s.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);d.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();d.gpu_written=true,provenance_write(d,__LINE__);}
void partial_clear_texture(Image &im,const p_clear_tex &request,uint32_t level,uint32_t base_layer,int scalar){
    need(im.source.d.target!=2,"Partial 3D texture clear requires 2D slice-view support");
    bool color=im.aspects==VK_IMAGE_ASPECT_COLOR_BIT;need(color || request.mask==15,"Depth/stencil texture channel-mask semantics not implemented");
    for(uint32_t slice=0;slice<uint32_t(request.r.depth);slice++){
        Commands c;Pass p;p.width=std::max(1u,im.width>>level);p.height=std::max(1u,im.height>>level);p.samples=im.samples;p.color_slots=color?1:0;
        uint32_t selected_layer=0;VkRect2D rect{};
        need(xbg_texture_clear_slice(p.width,p.height,im.layers,base_layer,request.r.xoffset,request.r.yoffset,request.r.zoffset,request.r.width,request.r.height,request.r.depth,slice,&rect,&selected_layer)==0,"Invalid texture clear slice");
        view_t view=request.view;view.set_levels=view.set_layers=1;view.base_level=level;view.nlevels=1;view.min_layer=selected_layer;view.nlayers=1;
        VkImageView image_view=im.view(&view,true);im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);
        VkAttachmentDescription attachment{};attachment.format=im.view_format(&view);attachment.samples=p.samples;attachment.loadOp=attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_GENERAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=color?1:0;sub.pColorAttachments=color?&ref:nullptr;sub.pDepthStencilAttachment=color?nullptr:&ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;VK(vkCreateRenderPass(cfg.device,&rp,nullptr,&p.pass));
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=p.pass;fb.attachmentCount=1;fb.pAttachments=&image_view;fb.width=p.width;fb.height=p.height;fb.layers=1;VK(vkCreateFramebuffer(cfg.device,&fb,nullptr,&p.framebuffer));
        {VkFramebuffer f=p.framebuffer;VkRenderPass rp_=p.pass;retire_handle([f,rp_]{vkDestroyFramebuffer(cfg.device,f,nullptr);vkDestroyRenderPass(cfg.device,rp_,nullptr);});}
        auto masked_object=std::make_shared<MaskedClear>();MaskedClear &masked=*masked_object;retire_object(masked_object);bool partial=color && request.mask!=15;if(partial)masked.prepare(p,0,scalar,uint32_t(request.mask),0,0);
        begin_pass(p,c);
        if(partial)masked.encode(c,p,rect,&request.c);
        else{VkClearAttachment clear{};clear.aspectMask=im.aspects;if(color)memcpy(&clear.clearValue.color,&request.c,16);else clear.clearValue.depthStencil={im.aspects&VK_IMAGE_ASPECT_DEPTH_BIT?request.c.f[0]:0.0f,request.c.u[im.aspects&VK_IMAGE_ASPECT_DEPTH_BIT?1:0]};VkClearRect region{rect,0,1};vkCmdClearAttachments(c.command,1,&clear,1,&region);}
        vkCmdEndRenderPass(c.command);pass_prof_end(c.command);c.finish();
    }
    im.gpu_written=true,provenance_write(im,__LINE__);
}
void clear_texture(const p_clear_tex &input){preflight_cpu_reads();
    p_clear_tex p=input;
    Image &im=get_image(p.tex,false);need((uint32_t(p.mask)&~15u)==0,"Invalid texture clear mask");if(!p.mask)return;
    uint32_t level=p.has_view&&p.view.set_levels?p.view.base_level:0,layer=p.has_view&&p.view.set_layers?p.view.min_layer:0;
    region_check(im,level,layer,p.r);uint32_t w=std::max(1u,im.width>>level),h=std::max(1u,im.height>>level);
    need(p.is_uint>=0 && p.is_uint<=2,"Invalid texture clear scalar type");int scalar=p.is_uint==2?1:p.is_uint==1?2:0;
    if(im.aspects==VK_IMAGE_ASPECT_COLOR_BIT){VkFormat viewed=im.view_format(p.has_view?&p.view:nullptr);static unsigned notes=0;if(notes++<8)xbg_logf("NVN clear-value texture input=%d nvn=%#x view=%#x vk=%u mip=%u mask=%#x bits=%08x/%08x/%08x/%08x",scalar,im.source.d.format,p.has_view&&p.view.set_format?p.view.format:im.source.d.format,unsigned(viewed),level,unsigned(p.mask),p.c.u[0],p.c.u[1],p.c.u[2],p.c.u[3]);VkClearColorValue value{};int converted=xbg_clear_value(viewed,scalar,&p.c,&value);
        if(converted<0)xbg_fatalf("Texture clear conversion failed nvn=%#x view=%#x vk=%u input=%d rc=%d",im.source.d.format,p.has_view&&p.view.set_format?p.view.format:im.source.d.format,unsigned(viewed),scalar,converted);
        memcpy(&p.c,&value,16);scalar=converted;}

    bool partial=p.mask!=15 || p.r.xoffset || p.r.yoffset || uint32_t(p.r.width)!=w || uint32_t(p.r.height)!=h;
    if(partial)need(im.source.d.target!=2,"Partial 3D texture clear requires 2D slice-view support");
    if(im.aspects&VK_IMAGE_ASPECT_DEPTH_BIT)need(p.c.f[0]>=0 && p.c.f[0]<=1,"Texture depth clear outside normalized range");
    if(partial || im.levels>1 || im.layers>1){need(im.samples==VK_SAMPLE_COUNT_1_BIT || im.gpu_written,"Partial clear of uninitialized CPU multisample storage unsupported");get_image(p.tex,true);}
    if(partial){partial_clear_texture(im,p,level,layer,scalar);return;}
    bool volume=im.source.d.target==2;need(!volume || (p.r.zoffset==0 && uint32_t(p.r.depth)==std::max(1u,im.depth>>level)),"Partial volume clear requires shader clear");
    VkFormat viewed=im.view_format(p.has_view?&p.view:nullptr);Commands c;VkClearValue value{};memcpy(&value.color,&p.c,16);
    if(viewed!=im.fmt){
        // VkClearColorImage uses the image's creation format. The original game
        // clears through integer views, so use an actual view/render-pass clear
        // to preserve raw bit patterns instead of numerically converting them.
        need(!volume && im.aspects==VK_IMAGE_ASPECT_COLOR_BIT,"Reinterpreted volume/depth clear unsupported");
        im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);
        std::vector<VkFramebuffer> framebuffers;VkRenderPass pass{};
        VkAttachmentDescription attachment{};attachment.format=viewed;attachment.samples=im.samples;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_GENERAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;VK(vkCreateRenderPass(cfg.device,&rp,nullptr,&pass));
        for(uint32_t z=0;z<uint32_t(p.r.depth);z++){
            view_t view=p.view;view.set_levels=view.set_layers=1;view.base_level=level;view.nlevels=1;view.min_layer=layer+p.r.zoffset+z;view.nlayers=1;
            VkImageView image_view=im.view(&view,true);VkFramebuffer fb;VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=pass;fi.attachmentCount=1;fi.pAttachments=&image_view;fi.width=w;fi.height=h;fi.layers=1;VK(vkCreateFramebuffer(cfg.device,&fi,nullptr,&fb));framebuffers.push_back(fb);
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=fb;begin.renderArea.extent={w,h};begin.clearValueCount=1;begin.pClearValues=&value;
            if(slots[slot_index].prof){Pass profile;profile.width=w;profile.height=h;profile.images.push_back(&im);pass_prof_begin(profile,c.command);}
            vkCmdBeginRenderPass(c.command,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdEndRenderPass(c.command);pass_prof_end(c.command);
        }
        im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();retire_handle([framebuffers,pass]{for(auto f:framebuffers)vkDestroyFramebuffer(cfg.device,f,nullptr);vkDestroyRenderPass(cfg.device,pass,nullptr);});
    }else{
        im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkImageSubresourceRange range{im.aspects,level,1,volume?0:layer+uint32_t(p.r.zoffset),volume?1:uint32_t(p.r.depth)};
        if(im.aspects==VK_IMAGE_ASPECT_COLOR_BIT)vkCmdClearColorImage(c.command,im.image,im.layout,&value.color,1,&range);
        else{VkClearDepthStencilValue ds{im.aspects&VK_IMAGE_ASPECT_DEPTH_BIT?p.c.f[0]:0.0f,p.c.u[im.aspects&VK_IMAGE_ASPECT_DEPTH_BIT?1:0]};vkCmdClearDepthStencilImage(c.command,im.image,im.layout,&ds,1,&range);}
        im.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();
    }
    im.gpu_written=true,provenance_write(im,__LINE__);
}
// Buffer copy whose source is a pending GPU result: copy on the GPU and keep
// the destination pending too, instead of stalling for the CPU memmove.
// 保守门暂时使 pending-view 链接回退CPU；恢复前须证明源readback跨退役仍存活。
// The conservative gate temporarily falls back to CPU for pending-view chains; restore only after proving source readback lifetime across retirement.
bool gpu_buffer_copy(uint64_t src,uint64_t dst,uint64_t n){preflight_cpu_reads();
    if(!n || (src<dst+n && dst<src+n))return false;
    for(auto &w:pending_writebacks)if(dst<w.addr+w.bytes && dst+n>w.addr)return false;
    Buffer view;if(!pending_view(src,n,1,view))return false;
    invalidate_cpu_range(dst,n);
    Slot &s=recording_slot();end_open_pass();Buffer b=readback_buffer(n);
    VkBufferCopy region{view.offset,b.offset,n};vkCmdCopyBuffer(s.cmd,view.buffer,b.buffer,1,&region);full_barrier(s.cmd);
    pending_writebacks.push_back({dst,n,b,&s});return true;
}
void resolve_texture(const p_copy_t2t &p){preflight_cpu_reads();
    Image &s=get_image(p.src,true),&d=get_image(p.dst,false);
    need(s.samples!=VK_SAMPLE_COUNT_1_BIT && d.samples==VK_SAMPLE_COUNT_1_BIT && s.fmt==d.fmt,"Resolve requires matching MS source / single-sample destination");
    need(s.aspects==VK_IMAGE_ASPECT_COLOR_BIT && d.aspects==VK_IMAGE_ASPECT_COLOR_BIT,"Depth/stencil resolve mode is not yet verified against original NVN");
    uint32_t sl=p.has_sv&&p.sv.set_levels?p.sv.base_level:0,dl=p.has_dv&&p.dv.set_levels?p.dv.base_level:0,sa=p.has_sv&&p.sv.set_layers?p.sv.min_layer:0,da=p.has_dv&&p.dv.set_layers?p.dv.min_layer:0;
    uint32_t sn=p.has_sv&&p.sv.set_layers?p.sv.nlayers:s.layers,dn=p.has_dv&&p.dv.set_layers?p.dv.nlayers:d.layers;
    need(sl<s.levels && dl<d.levels && sa+sn<=s.layers && da+dn<=d.layers && sn==dn,"Resolve view out of range");
    uint32_t w=std::max(1u,s.width>>sl),h=std::max(1u,s.height>>sl);need(w==std::max(1u,d.width>>dl)&&h==std::max(1u,d.height>>dl),"Resolve extents differ");
    Commands c;s.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);d.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkImageResolve r{};r.srcSubresource={s.aspects,sl,sa,sn};r.dstSubresource={d.aspects,dl,da,dn};r.extent={w,h,1};vkCmdResolveImage(c.command,s.image,s.layout,d.image,d.layout,1,&r);s.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);d.transition(c.command,VK_IMAGE_LAYOUT_GENERAL);c.finish();d.gpu_written=true,provenance_write(d,__LINE__);
}

// CPU-visible writes occur only after all preceding synchronous GPU work finished.
// Every byte count and opcode is checked; unknown commands terminate with context.
// Consecutive commands nearly always sit in one range: remember the last one (pools change only under the mutex,
// and clear it; host segments are freed only at shutdown).
uintptr_t cmd_range_lo=0;size_t cmd_range_bytes=0;
size_t command_available(const uint8_t *p){uintptr_t address=uintptr_t(p);if(address-cmd_range_lo<cmd_range_bytes)return cmd_range_bytes-(address-cmd_range_lo);auto it=pools.upper_bound(address);if(it!=pools.begin()){--it;if(address>=it->first && address-it->first<it->second){cmd_range_lo=it->first;cmd_range_bytes=it->second;return it->second-(address-it->first);}}{auto h=host_ranges.upper_bound(address);if(h!=host_ranges.begin()){--h;if(address>=h->first && address-h->first<h->second){cmd_range_lo=h->first;cmd_range_bytes=h->second;return h->second-(address-h->first);}}}xbg_fatalf("NVN command address %#llx is outside registered command pools",(unsigned long long)address);}
void execute_draw_call(const xbg_draw_call &p){
    need(p.index_type>=-1 && p.index_type<=2 && p.first>=0 && p.count>=0 && p.instances>=0 && p.base_instance>=0,"Invalid recorded draw plan");
    if(!p.count || !p.instances)return;
    if(p.index_type<0)draw(p.primitive,p.first,p.count,nullptr,uint32_t(p.instances),uint32_t(p.base_instance));
    else{
        uint64_t bytes=uint64_t(p.count)<<p.index_type;
        need(bytes<=command_available(reinterpret_cast<const uint8_t*>(p.index_address)),"Indirect index read exceeds registered storage");
        p_draw_idx indexed{p.primitive,p.index_type,p.count,p.base_vertex,p.base_instance,p.instances,p.index_address};
        draw(p.primitive,0,p.count,&indexed);
    }
}
void execute_indirect(const xbg_indirect &p,bool single){preflight_cpu_reads();
    need(p.arguments && p.max_count>=0 && p.max_count<=1000000,"Invalid indirect draw request");
    uint32_t count=1;
    if(!single){
        need(p.count_address && !(p.count_address&3),"Invalid indirect count address");
        need(command_available(reinterpret_cast<const uint8_t*>(p.count_address))>=4,"Truncated indirect count storage");
        cpu_access(p.count_address,4,5);memcpy(&count,reinterpret_cast<const void*>(p.count_address),4);count=std::min(count,uint32_t(p.max_count));
    }
    for(uint32_t i=0;i<count;i++){
        uint64_t address=0;need(xbg_draw_indirect_address(p.arguments,i,p.stride,p.index_type,&address)==0,"Invalid indirect argument address/stride");
        size_t bytes=p.index_type<0?16:20;
        need(bytes<=command_available(reinterpret_cast<const uint8_t*>(address)),"Indirect arguments exceed registered storage");cpu_access(address,bytes,5);
        // Read at execution, after preceding synchronous GPU storage writeback,
        // never at command recording. Both single and multi use this path.
        xbg_draw_call plan{};int result=xbg_draw_indirect_plan(&plan,p.primitive,p.index_type,p.index_address,reinterpret_cast<const void*>(address),bytes);
        if(result)xbg_fatalf("Invalid indirect draw parameters: %d",result);
        execute_draw_call(plan);
    }
}
template<class T>T payload(const uint8_t *p,size_t n){need(n>=sizeof(T),"Truncated NVN command payload");T out;memcpy(&out,p,sizeof(out));return out;}
template<class T>T variable_payload(const uint8_t *p,size_t n){T out{};memcpy(&out,p,std::min(n,sizeof(T)));return out;}
// Graphics thread: retire submitted slots whose fence already signalled (non-blocking) so a game
// thread blocked on an older fence is released while the next frame is still being translated.
void poll_retire(){
    bool any=false;for(auto &s:slots)if(s.pending && !s.recording && vkGetFenceStatus(cfg.device,s.fence)==VK_SUCCESS){retire_slot(s);any=true;}
    if(any){{std::lock_guard<std::mutex> l(gfx_job_mutex);}gfx_idle_cv.notify_all();}
}
void execute(const uint8_t *pc,unsigned depth,uint64_t &budget){
    need(depth<64,"NVN command CALL nesting limit exceeded");
    for(;;){need(budget++<10000000,"NVN command count/cycle limit exceeded");
        if(gfx_async && (budget&63)==0)poll_retire();
        // Bound host arena use per frame slot; only at command boundaries.
        {Slot &cur=slots[slot_index];if(cur.recording && cur.stream.used_total+cur.readback.used_total>arena_flush_bytes){SiteScope site(SITE_ARENA);submit_slots(false);}}
        size_t available=command_available(pc);need(available>=8,"Truncated command header");op_hdr h;memcpy(&h,pc,8);need(h.size>=8 && !(h.size&7) && h.size<=available,"NVN command size outside pool");const uint8_t *p=pc+8;size_t n=h.size-8;cur_op=h.op;
        switch(h.op){
        case OP_END:return;
        case OP_BEGIN:break;
        case OP_JUMP:pc=reinterpret_cast<const uint8_t*>(payload<uint64_t>(p,n));continue;
        case OP_CALL:execute(reinterpret_cast<const uint8_t*>(payload<uint64_t>(p,n)),depth+1,budget);break;
        case OP_SIGNAL_EVENT:{need(n>=16,"Truncated event");uint64_t addr;uint32_t mode,v;memcpy(&addr,p,8);memcpy(&mode,p+8,4);memcpy(&v,p+12,4);need(addr,"Null event address");SiteScope site(SITE_EVENT);
            if(defer_events){   // hand the work so far to the GPU without waiting; publish when that submission retires
                Slot &cur=slots[slot_index];
                if(defer_events==2 && cur.recording && cur.ops){recording_events.push_back({addr,mode,v,&cur});++events_deferred;evguard::guard(addr);break;}
                if(cur.recording && cur.ops)submit_slots(false);
                Slot &last=slots[slot_index^1];   // most recent submission (just made, or the previous one)
                if(last.pending){pending_events.push_back({addr,mode,v,&last});++events_deferred;evguard::guard(addr);}else apply_event(addr,mode,v);
                break;}
            complete_all();cpu_access(addr,4,10);snapshot_invalidate(addr,4);auto ptr=reinterpret_cast<uint32_t*>(addr);if(mode==NVN_EVENT_SIGNAL_MODE_ADD)__atomic_fetch_add(ptr,v,__ATOMIC_SEQ_CST);else{need(mode==NVN_EVENT_SIGNAL_MODE_WRITE,"Unknown event mode");__atomic_store_n(ptr,v,__ATOMIC_SEQ_CST);}break;}
        case OP_COPY_B2B:{need(n>=24,"Truncated buffer copy");uint64_t a[3];memcpy(a,p,24);need(a[0]&&a[1]&&a[2]<=256ull*1024*1024,"Invalid buffer copy");if(gpu_buffer_copy(a[0],a[1],a[2]))break;cpu_access(a[0],a[2],11);invalidate_cpu_range(a[1],a[2]);memmove(reinterpret_cast<void*>(a[1]),reinterpret_cast<void*>(a[0]),a[2]);break;}
        case OP_UPDATE_UB:{need(n>=16,"Truncated uniform update");uint64_t a[2];memcpy(a,p,16);need(a[0] && a[1]<=n-16,"Invalid uniform update");invalidate_cpu_range(a[0],a[1]);memcpy(reinterpret_cast<void*>(a[0]),p+16,a[1]);break;}
        case OP_SET_RT:state.rt=payload<p_set_rt>(p,n);break;
        case OP_CLEAR_COLOR:{auto a=payload<p_clear_color>(p,n);clear_attachment(&a,nullptr);break;}
        case OP_COUNT_+9:{auto a=payload<xbg_typed_clear>(p,n);clear_attachment(&a.color,nullptr,a.scalar);break;}
        case OP_CLEAR_DS:{auto a=payload<p_clear_ds>(p,n);clear_attachment(nullptr,&a);break;}
        case OP_VIEWPORTS:{auto a=payload<p_viewports>(p,n);need(a.first==0 && a.n==1,"Multiple viewports unsupported");state.viewport.x=a.v[0][0];state.viewport.y=a.v[0][1];state.viewport.width=a.v[0][2];state.viewport.height=a.v[0][3];break;}
        case OP_SCISSOR:{auto a=payload<p_scissor>(p,n);need(a.w>=0 && a.h>=0,"Invalid scissor dimensions");state.scissor={{a.x,a.y},{uint32_t(a.w),uint32_t(a.h)}};break;}
        case OP_DEPTH_RANGE:{auto a=payload<p_depth_range>(p,n);state.viewport.minDepth=a.n;state.viewport.maxDepth=a.f;break;}
        case OP_BIND_PROGRAM:{CPU_SCOPE(Program);auto a=payload<p_bind_program>(p,n);need(!a.prog || a.prog->nstage<=6,"Invalid bound program");for(unsigned st=0;st<6;st++)if(a.stages&(1u<<nvn_stage[st]))state.shader[st]=nullptr;if(!a.prog)break;const std::array<Shader*,6> &loaded=program_shaders(a.prog);for(unsigned i=0;i<a.prog->nstage;i++){Shader *s=loaded[i];if(a.stages&(1u<<nvn_stage[s->stage]))state.shader[s->stage]=s;}break;}
        case OP_BIND_STATE:{auto a=payload<p_bind_state>(p,n);switch(a.kind){case 0:{blend_st b;memcpy(&b,a.bytes,sizeof(b));state.blend[b.target]=b;break;}case 1:memcpy(&state.color,a.bytes,sizeof(state.color));break;case 2:memcpy(&state.mask,a.bytes,sizeof(state.mask));break;case 3:memcpy(&state.ms,a.bytes,sizeof(state.ms));break;case 4:memcpy(&state.poly,a.bytes,sizeof(state.poly));break;case 5:memcpy(&state.ds,a.bytes,sizeof(state.ds));break;default:xbg_fatalf("Unknown NVN state object %d",a.kind);}break;}
        case OP_BIND_VATTR:{auto a=variable_payload<p_bind_vattr>(p,n);need(a.first==0 && a.n>=0 && a.n<=16 && n>=offsetof(p_bind_vattr,st)+size_t(a.n)*4,"Invalid vertex attribute record");need(xbg_vertex_attributes_replace(state.attrib,reinterpret_cast<const uint32_t*>(a.st),a.n)==0,"Invalid vertex attribute replacement");break;}
        case OP_BIND_VSTREAM:{auto a=variable_payload<p_bind_vstream>(p,n);need(a.first>=0 && a.n>=0 && a.first+a.n<=16 && n>=offsetof(p_bind_vstream,st)+size_t(a.n)*sizeof(vs_st),"Invalid vertex stream record");memcpy(state.stream+a.first,a.st,a.n*sizeof(vs_st));break;}
        case OP_BIND_VBUF:{auto a=variable_payload<p_bind_vbuf>(p,n);need(a.first>=0 && a.n>=0 && a.first+a.n<=16 && n>=offsetof(p_bind_vbuf,r)+size_t(a.n)*sizeof(NVNbufferRange),"Invalid vertex buffer record");memcpy(state.vb+a.first,a.r,a.n*sizeof(NVNbufferRange));break;}
        case OP_BIND_UBO:case OP_BIND_SSBO:{auto a=payload<p_bind_buf>(p,n);need(a.stage>=0 && a.stage<6 && a.index>=0 && a.index<16,"Invalid shader buffer slot");(h.op==OP_BIND_UBO?state.ub:state.sb)[a.stage][a.index]={a.addr,a.size};++buf_gen[a.stage];break;}
        case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:{auto a=variable_payload<p_bind_handles>(p,n);need(a.stage>=0 && a.stage<6 && a.first>=0 && a.n>=0 && a.n<=32 && a.first+a.n<=(h.op==OP_BIND_IMG?16:128) && n>=offsetof(p_bind_handles,h)+size_t(a.n)*8,"Invalid descriptor binding record");uint64_t *dst=h.op==OP_BIND_IMG?state.img[a.stage]:h.op==OP_BIND_SAMP?state.samp[a.stage]:state.tex[a.stage];memcpy(dst+a.first,a.h,a.n*8);++tex_gen[a.stage];break;}
        case OP_STENCIL:{auto a=payload<p_stencil>(p,n);need(a.face>=0 && a.face<=3 && a.which>=0 && a.which<=2,"Invalid stencil state");for(unsigned i=0;i<2;i++)if(a.face&(1<<i)){if(a.which==0)state.stencil_compare[i]=a.value_mask;else if(a.which==1)state.stencil_write[i]=a.mask;else state.stencil_ref[i]=a.ref;}break;}
        case OP_POLY_OFFSET:{auto a=payload<p_poly_offset>(p,n);state.bias[0]=a.factor;state.bias[1]=a.units;state.bias[2]=a.clamp;break;}
        case OP_DRAW:{auto a=payload<p_draw>(p,n);draw(a.prim,a.first,a.count,nullptr);break;}
        case OP_DRAW_IDX:{auto a=payload<p_draw_idx>(p,n);draw(a.prim,0,a.count,&a);break;}
        case OP_DISPATCH:dispatch(payload<p_dispatch>(p,n));break;
        // Commands ends the open pass and records a full barrier. The explicit barrier and finish() recorded the same barrier
        // twice more with no command between them (3 per game barrier, ~130 per D2R fight frame), which Vulkan treats as one.
        // A/B target barrier3 records all three again.
        case OP_BARRIER:{Commands c;if(ab_off("barrier3")){VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);c.finish();}break;}
        case OP_SET_TEX_POOL:{auto a=payload<p_set_pool>(p,n);need(a.pool,"Null texture pool");state.texture_pool=*a.pool;break;}
        case OP_SET_SAMP_POOL:{auto a=payload<p_set_pool>(p,n);need(a.pool,"Null sampler pool");state.sampler_pool=*a.pool;break;}
        case OP_COPY_B2T:copy_texture_buffer(payload<p_copy_b2t>(p,n),false);break;
        case OP_COPY_T2B:copy_texture_buffer(payload<p_copy_b2t>(p,n),true);break;
        case OP_COPY_T2T:copy_texture(payload<p_copy_t2t>(p,n));break;
        case OP_CLEAR_TEX:clear_texture(payload<p_clear_tex>(p,n));break;
        case OP_COPY_STRIDE:state.copy_stride=payload<p_copy_stride>(p,n).stride;need(state.copy_stride>=0,"Negative copy stride");break;
        case OP_COPY_IMAGE_STRIDE:state.copy_image_stride=payload<p_copy_stride>(p,n).stride;need(state.copy_image_stride>=0,"Negative copy image stride");break;
        case OP_BLEND_COLOR:{auto a=payload<p_blend_color>(p,n);memcpy(state.blend_color,a.rgba,16);break;}
        case OP_COUNT_:{uint32_t size=payload<uint32_t>(p,n);need(size && size<=properties.limits.maxTessellationPatchSize,"Invalid tessellation patch size");state.patch_size=size;break;}
        case OP_COUNT_+1:{auto s=payload<sync_t>(p,n);need(s.fenced && s.fence<=submitted+1,"Command wait on unresolved external sync");
            // GPU-side wait on an earlier fence of this same in-order queue: already
            // satisfied by submission order (every operation starts with a barrier).
            break;}
        case OP_COUNT_+2:{uint64_t addr=payload<uint64_t>(p,n);need(addr,"Null indirect dispatch address");cpu_access(addr,12,5);uint32_t xyz[3];memcpy(xyz,reinterpret_cast<void*>(addr),12);p_dispatch a{int32_t(xyz[0]),int32_t(xyz[1]),int32_t(xyz[2]),0};dispatch(a);break;}
        case OP_COUNT_+3:execute_indirect(payload<xbg_indirect>(p,n),false);break;
        case OP_COUNT_+7:execute_draw_call(payload<xbg_draw_call>(p,n));break;
        case OP_COUNT_+8:execute_indirect(payload<xbg_indirect>(p,n),true);break;
        case OP_COUNT_+4:{auto a=payload<xbg_wait_event>(p,n);need(a.address && a.mode<=1,"Invalid event wait");
            for(auto *list:{&pending_events,&recording_events})if(std::any_of(list->begin(),list->end(),[&](const PendingEvent &e){return e.addr==a.address;})){complete_all();break;}
            auto deadline=std::chrono::steady_clock::now()+std::chrono::nanoseconds(cfg.fence_timeout_ns);for(;;){uint32_t value=__atomic_load_n(reinterpret_cast<uint32_t*>(a.address),__ATOMIC_ACQUIRE);if(a.mode==0?value==a.value:int32_t(value-a.value)>=0)break;need(std::chrono::steady_clock::now()<deadline,"NVN event wait timeout");std::this_thread::yield();}break;}
        case OP_COUNT_+5:state.bounds=payload<xbg_depth_bounds>(p,n);break;
        case OP_COUNT_+6:resolve_texture(payload<p_copy_t2t>(p,n));break;
        case OP_RESET_COUNTER:{uint64_t type=payload<uint64_t>(p,n);need(type==NVN_COUNTER_TYPE_SAMPLES_PASSED,"Unsupported pipeline counter reset");state.samples_passed=0;end_occlusion_query();++counter_epoch;state.count_samples=true;++counter_ops[0];break;}   // the open run belongs to the old epoch
        case OP_REPORT_COUNTER:{need(n>=16,"Truncated counter report");uint64_t type,addr;memcpy(&type,p,8);memcpy(&addr,p+8,8);need(addr,"Null counter output");need(type==NVN_COUNTER_TYPE_TIMESTAMP || type==NVN_COUNTER_TYPE_SAMPLES_PASSED,"Unsupported pipeline statistics query");need(timestamp_bits,"Queue family does not support GPU timestamps");++counter_ops[type==NVN_COUNTER_TYPE_TIMESTAMP?1:2];if(type==NVN_COUNTER_TYPE_SAMPLES_PASSED)samples_report_seen=true;
            PendingReport r{addr,type,counter_epoch,epochs[counter_epoch].size(),nullptr,VK_NULL_HANDLE,0,0,false};
            if(type==NVN_COUNTER_TYPE_TIMESTAMP && !heavy_timestamps && !ab_off("timestamps")){
                // A bottom-of-pipe timestamp already waits for all earlier work and may be written inside a render pass: no pass
                // end and no full barriers, so a timestamp report no longer splits a pass on a tiling GPU (4 per D2R frame).
                // Samples reports keep the pass end below: it closes the occlusion run they sum.
                timestamp_query(r.ts,r.ts_index,r.slot);   // a new pool is reset outside the pass first
                Slot &ts=recording_slot();++ts.ops;++perf.ops;vkCmdWriteTimestamp(ts.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.ts,r.ts_index);}
            else{Commands c;timestamp_query(r.ts,r.ts_index,r.slot);vkCmdWriteTimestamp(c.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.ts,r.ts_index);c.finish();}
            pending_reports.push_back(r);break;}
        default:xbg_fatalf("NVN opcode %u at %#llx not implemented",h.op,(unsigned long long)uintptr_t(pc));
        }
        pc+=h.size;
    }
}
PerfCounts perf_counts(){return {pipeline_objects.size(),layout_cache.size(),sampler_cache.size(),render_pass_cache.size(),images.size(),host_segments.size(),host_bytes};}
void destroy_slots(){
    poolreset::drain();for(auto &s:slots)s.reset_queued=false;
    texel_evict(0,true);texel_evict(1,true);
    for(auto &s:slots){s.retire.run();release_descriptors(s);for(auto &v:s.texel_views)vkDestroyBufferView(cfg.device,v.second,nullptr);s.texel_views.clear();for(auto &f:s.framebuffers)vkDestroyFramebuffer(cfg.device,f.second,nullptr);s.framebuffers.clear();for(auto p:s.pools)vkDestroyDescriptorPool(cfg.device,p,nullptr);s.pools.clear();s.pool_next=0;s.stream.release();s.readback.release();s.viewed.clear();s.guard.clear();s.pro_ops.clear();s.pro=VK_NULL_HANDLE;if(s.fence)vkDestroyFence(cfg.device,s.fence,nullptr);if(s.timestamps)vkDestroyQueryPool(cfg.device,s.timestamps,nullptr);if(s.prof_pool)vkDestroyQueryPool(cfg.device,s.prof_pool,nullptr);for(auto q:s.occlusion_pools)vkDestroyQueryPool(cfg.device,q,nullptr);for(auto q:s.timestamp_pools)vkDestroyQueryPool(cfg.device,q,nullptr);if(s.pool)vkDestroyCommandPool(cfg.device,s.pool,nullptr);s=Slot{};}
    slot_index=0;snapshot_reset();
}
} // namespace

// Zero-copy probe (C097): can exportable host-visible Vulkan memory be mmap-ed at an address we choose?
// If yes, guest NVN memory pools can be re-backed by GPU-visible memory in place (iOS: Metal buffers on pool memory).
void zero_copy_probe(){
    for(uint32_t i=0;i<memories.memoryTypeCount;i++)xbg_logf("NVN caps memtype %u flags=%#x heap=%u heapsize=%lluMB",i,memories.memoryTypes[i].propertyFlags,memories.memoryTypes[i].heapIndex,(unsigned long long)(memories.memoryHeaps[memories.memoryTypes[i].heapIndex].size>>20));
    auto getfd=reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(cfg.device,"vkGetMemoryFdKHR"));
    if(!getfd){xbg_logf("NVN caps zerocopy getfd=missing");return;}
    const VkDeviceSize n=4<<20;
    for(uint32_t want:{uint32_t(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT),uint32_t(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)}){
        uint32_t type=UINT32_MAX;for(uint32_t i=0;i<memories.memoryTypeCount;i++)if((memories.memoryTypes[i].propertyFlags&want)==want){type=i;break;}
        if(type==UINT32_MAX){xbg_logf("NVN caps zerocopy flags=%#x type=none",want);continue;}
        for(VkExternalMemoryHandleTypeFlagBits ht:{VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT}){
            VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};ex.handleTypes=ht;
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.pNext=&ex;ai.allocationSize=n;ai.memoryTypeIndex=type;VkDeviceMemory mem{};
            VkResult r=vkAllocateMemory(cfg.device,&ai,nullptr,&mem);if(r!=VK_SUCCESS){xbg_logf("NVN caps zerocopy flags=%#x handle=%#x alloc=%d",want,unsigned(ht),r);continue;}
            VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};gi.memory=mem;gi.handleType=ht;int fd=-1;r=getfd(cfg.device,&gi,&fd);
            int ok=0;void *vk=nullptr;
            if(r==VK_SUCCESS && fd>=0){
                void *at=mmap(nullptr,n,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                void *m=at==MAP_FAILED?MAP_FAILED:mmap(at,n,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0);
                if(m!=MAP_FAILED && vkMapMemory(cfg.device,mem,0,n,0,&vk)==VK_SUCCESS){
                    memset(m,0x5a,n);static_cast<uint8_t*>(m)[12345]=0x77;
                    if(!(want&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){VkMappedMemoryRange rr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};rr.memory=mem;rr.size=VK_WHOLE_SIZE;vkInvalidateMappedMemoryRanges(cfg.device,1,&rr);}
                    ok=static_cast<uint8_t*>(vk)[12345]==0x77 && static_cast<uint8_t*>(vk)[0]==0x5a ? 1 : 2;
                    vkUnmapMemory(cfg.device,mem);
                }
                if(at!=MAP_FAILED)munmap(at,n);close(fd);
            }
            xbg_logf("NVN caps zerocopy flags=%#x type=%u handle=%#x getfd=%d mmap_fixed_shared=%s",want,type,unsigned(ht),r,ok==1?"OK":ok==2?"MISMATCH":"FAIL");
            vkFreeMemory(cfg.device,mem,nullptr);
        }
    }
}
extern "C" int xbg_game_initialize(const xbg_game_config *c,char *error,size_t bytes){
    if(!c || c->size!=sizeof(*c) || !c->physical || !c->device || !c->queue || !c->shader_directory || !c->fence_timeout_ns){if(error&&bytes)snprintf(error,bytes,"Invalid xbg_game_config");return -1;}
    std::lock_guard<std::recursive_mutex> lock(mutex);if(initialized){if(error&&bytes)snprintf(error,bytes,"Game graphics already initialized");return -1;}
#ifdef DM_COMPLETION_WAIT_SELFTEST
    if(const char *test=getenv("DM_GFX_COMPLETION_WAIT_TEST"))if(!strcmp(test,"1")){
        // 隔离测试在资源/线程初始化前只跑一次；失败不准进入游戏，不污染真实 completed。
        // Run isolated tests once before resource/thread initialization; failure blocks the game and never changes the real completed counter.
        static bool tested=false;static int result;static char report[256];
        if(!tested){result=xbg_completion_wait_selftest(report,sizeof report);tested=true;}
        if(c->log)c->log(c->userdata,report);else fprintf(stderr,"[xbg] %s\n",report);
        if(result){if(error&&bytes)snprintf(error,bytes,"%s",report);return -1;}
    }
#endif
    // 沿用宿主保证初始化/关闭时没有活跃等待者的前提；不引入重置通知或新会话世代。
    // Retain the host's quiescent initialize/shutdown requirement; do not add reset notifications or session generations.
    fold_completion_wait_stats();completion_wait_enabled=completion_wait_default;
    if(const char *mode=getenv("DM_GFX_COMPLETION_WAIT")){if(!strcmp(mode,"0"))completion_wait_enabled=false;else if(!strcmp(mode,"1"))completion_wait_enabled=true;}
    {std::string dir=c->shader_directory?c->shader_directory:"";size_t cut=dir.find_last_of('/');FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-legacy-waitall").c_str(),"rb");if(m){legacy_waitall=fgetc(m)=='1';fclose(m);}}
    cfg=*c;xbg_logf("NVN completion_wait enabled=%d legacy_waitall=%d",int(completion_wait_enabled),int(legacy_waitall));vkGetPhysicalDeviceProperties(cfg.physical,&properties);vkGetPhysicalDeviceMemoryProperties(cfg.physical,&memories);{char line[512];int at=snprintf(line,sizeof line,"NVN caps memory types");for(uint32_t i=0;i<memories.memoryTypeCount && at<int(sizeof line)-24;i++)at+=snprintf(line+at,sizeof line-at," %u:%#x/h%u",i,unsigned(memories.memoryTypes[i].propertyFlags),memories.memoryTypes[i].heapIndex);xbg_logf("%s",line);}
    VkPhysicalDeviceProperties2 pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};pp.pNext=&subgroup;vkGetPhysicalDeviceProperties2(cfg.physical,&pp);
    VkPhysicalDeviceFeatures available;vkGetPhysicalDeviceFeatures(cfg.physical,&available);const VkBool32 *want=reinterpret_cast<const VkBool32*>(&cfg.enabled_core),*have=reinterpret_cast<const VkBool32*>(&available);for(size_t i=0;i<sizeof(available)/sizeof(VkBool32);i++)if(want[i]&&!have[i]){if(error&&bytes)snprintf(error,bytes,"Requested enabled feature %zu is unsupported",i);return -1;}
    uint32_t count;vkGetPhysicalDeviceQueueFamilyProperties(cfg.physical,&count,nullptr);std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(cfg.physical,&count,queues.data());if(cfg.queue_family>=count || (queues[cfg.queue_family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))!=(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){if(error&&bytes)snprintf(error,bytes,"Graphics+compute queue family required");return -1;}
    timestamp_bits=queues[cfg.queue_family].timestampValidBits;
    {const auto &l=properties.limits;VkDeviceSize a=256;for(VkDeviceSize x:{l.minUniformBufferOffsetAlignment,l.minStorageBufferOffsetAlignment,l.minTexelBufferOffsetAlignment,l.optimalBufferCopyOffsetAlignment,l.nonCoherentAtomSize,VkDeviceSize(16)})while(a<x)a<<=1;arena_alignment=a;}
    {std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');std::string marker=(cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-sync-mode";FILE *m=fopen(marker.c_str(),"rb");sync_mode=false;if(m){sync_mode=fgetc(m)=='1';fclose(m);}}
     {std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-defer-events").c_str(),"rb");defer_events=0;if(m){int c=fgetc(m);defer_events=c=='1'?1:c=='2'?2:0;fclose(m);}else if(const char *e=getenv("DM_GFX_DEFER_EVENTS")){int v=atoi(e);defer_events=v==1?1:v==2?2:0;}xbg_logf("NVN defer events=%d (marker gfx-defer-events)",int(defer_events));{FILE *eg=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-event-guard").c_str(),"rb");evguard::enabled=eg!=nullptr;if(eg)fclose(eg);if(evguard::enabled)xbg_logf("NVN event guard on (marker gfx-event-guard)");}{FILE *v=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-vcache").c_str(),"rb");vcache::enabled=false;if(v){vcache::enabled=fgetc(v)!='0';fclose(v);}else if(const char *e=getenv("DM_GFX_VCACHE"))vcache::enabled=atoi(e)!=0;xbg_logf("NVN vertex/index cross-frame cache=%d tracker=%d (marker gfx-vcache)",int(vcache::enabled),int(nvn_dirty_since!=nullptr));}
     {FILE *v=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-tex-dirty").c_str(),"rb");tex_dirty=false;if(v){tex_dirty=fgetc(v)!='0';fclose(v);}else if(const char *e=getenv("DM_GFX_TEX_DIRTY"))tex_dirty=atoi(e)!=0;
      xbg_logf("NVN texture checks through the dirty tracker=%d (marker gfx-tex-dirty)",int(tex_dirty));}{FILE *b=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-native-bc").c_str(),"rb");native_bc=false;if(b){native_bc=fgetc(b)!='0';fclose(b);}else if(const char *e=getenv("DM_GFX_NATIVE_BC"))native_bc=atoi(e)!=0;xbg_logf("NVN native BC textures=%d (marker gfx-native-bc)",int(native_bc));}
     {FILE *b=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-noload").c_str(),"rb");noload_enabled=false;if(b){noload_enabled=fgetc(b)!='0';fclose(b);}else if(const char *e=getenv("DM_GFX_NOLOAD"))noload_enabled=atoi(e)!=0;xbg_logf("NVN proven full-overwrite first draws skip the color load=%d (marker gfx-noload; A/B target noload)",int(noload_enabled));}{FILE *q=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-bright").c_str(),"rb");bright::enabled=true;if(q){bright::enabled=fgetc(q)!='0';fclose(q);}else if(const char *e=getenv("DM_GFX_BRIGHT"))bright::enabled=atoi(e)!=0;xbg_logf("NVN 3D brightening=%d (marker gfx-bright; A/B target bright)",int(bright::enabled));}{FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-perf-sample").c_str(),"rb");perf_sample=32;if(m){if(fscanf(m,"%u",&perf_sample)!=1)perf_sample=32;fclose(m);}else if(const char *e=getenv("DM_GFX_PERF_SAMPLE"))perf_sample=unsigned(atoi(e));perf_frame_timed=perf_sample!=0;perf_timed_frames=0;xbg_logf("NVN perf timing every %u frames (marker gfx-perf-sample; 0 = off)",perf_sample);}}
    for(auto &sl:slots)sl=Slot{};slot_index=0;pass_prof_enabled=false;pass_prof_next=300;pass_prof_period=600;pass_prof_until=UINT64_MAX;pass_prof_frame=UINT64_MAX;pass_prof_draws=0;pass_prof_queries=pass_prof_slots=pass_prof_skipped_slots=0;perf=Perf{};for(auto &c:perf_timing_before)c=XbgCpuTiming::Counter{};static_arena.cached=false;support_static.clear();format_property_cache.clear();for(auto &k:format_flat_known)k=false;
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&draw_parameters;vkGetPhysicalDeviceFeatures2(cfg.physical,&features);if(cfg.enabled_shader_draw_parameters && !draw_parameters.shaderDrawParameters){if(error&&bytes)snprintf(error,bytes,"shaderDrawParameters unsupported");return -1;}
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.queueFamilyIndex=cfg.queue_family;cp.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkResult r=vkCreateCommandPool(cfg.device,&cp,nullptr,&command_pool);if(r!=VK_SUCCESS){if(error&&bytes)snprintf(error,bytes,"vkCreateCommandPool=%d",r);return r;}
    VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};std::vector<uint8_t> cache_data=load_pipeline_cache();cache.initialDataSize=cache_data.size();cache.pInitialData=cache_data.empty()?nullptr:cache_data.data();r=vkCreatePipelineCache(cfg.device,&cache,nullptr,&pipeline_cache);if(r!=VK_SUCCESS){vkDestroyCommandPool(cfg.device,command_pool,nullptr);command_pool=VK_NULL_HANDLE;if(error&&bytes)snprintf(error,bytes,"vkCreatePipelineCache=%d",r);return r;}
    {std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');FILE *q=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-quad-derivatives").c_str(),"rb");quad_derivatives=false;
     if(q){quad_derivatives=fgetc(q)!='0';fclose(q);}else if(const char *e=getenv("DM_GFX_QUAD_DERIVATIVES"))quad_derivatives=atoi(e)!=0;xbg_logf("NVN quad shuffles to fine derivatives=%d (marker gfx-quad-derivatives)",int(quad_derivatives));}
    {std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');FILE *q=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-warp32").c_str(),"rb");warp32=false;
     if(q){warp32=fgetc(q)!='0';fclose(q);}else if(const char *e=getenv("DM_GFX_WARP32"))warp32=atoi(e)!=0;xbg_logf("NVN 32-lane warps in wider subgroups=%d subgroup=%u (marker gfx-warp32)",int(warp32),subgroup.subgroupSize);}
    {std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');std::string base=cut==std::string::npos?dir:dir.substr(0,cut);
     auto dm=[&](const char *marker,const char *env){FILE *q=fopen((base+marker).c_str(),"rb");bool on=false;if(q){on=fgetc(q)!='0';fclose(q);}else if(const char *e=getenv(env))on=atoi(e)!=0;return on;};
     ldc_fold=dm("/gfx-ldc-fold","DM_GFX_LDC_FOLD");forward_temps=dm("/gfx-forward-temps","DM_GFX_FORWARD_TEMPS");nan_minmax=dm("/gfx-nan-minmax","DM_GFX_NAN_MINMAX");
     xbg_logf("NVN NaN-ignoring min / max / clamp like Maxwell=%d (marker gfx-nan-minmax)",int(nan_minmax));
     xbg_logf("NVN translator output clean-up: component fold=%d (marker gfx-ldc-fold) forwarded temporaries=%d (marker gfx-forward-temps)",int(ldc_fold),int(forward_temps));
     {FILE *q=fopen((base+"/gfx-shader-reuse").c_str(),"rb");shader_reuse=0;if(q){int c=fgetc(q);shader_reuse=c=='0'?0:c=='2'?2:1;fclose(q);}else if(const char *e=getenv("DM_GFX_SHADER_REUSE"))shader_reuse=atoi(e);
      warm::keep_rewritten=shader_reuse!=0;xbg_logf("NVN shader loads take the warm-up's rewritten SPIR-V=%d (marker gfx-shader-reuse; 2 also rewrites and compares)",shader_reuse);}}
    xbg_module_id_hook=warm::module_id;{std::string lp=warm::list_path();FILE *nw=fopen((lp.substr(0,lp.find_last_of('/'))+"/gfx-no-warm").c_str(),"rb");warm::enabled=!nw;if(nw)fclose(nw);}warm::start();
    wtrack::enabled=false;state=State{};state.color.logic_op=3;state.color.alpha_func=8;for(unsigned i=0;i<8;i++)state.blend[i]={i,2,1,2,1,1,1};stats.reset();timing=XbgCpuTiming(timing_clock);pipeline_counts={};pipeline_before={};pipeline_tick=0;timing_active=false;timing_first=timing_last=0;live_profile_used=false;descriptor_detail=false;descriptor_timing=XbgDescriptorTiming{};descriptor_root=nullptr;draw_detail=false;draw_timing=XbgDrawTiming{};draw_root=nullptr;completed=0;last_attachment_trace=UINT64_MAX;sampler_mirror_clamp_enabled=VK_FALSE;failed=false;error_text[0]=0;initialized=true;
    {std::string dir=cfg.shader_directory;size_t cut=dir.find_last_of('/');FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-sync-submit").c_str(),"rb");bool sync_submit=false;if(m){sync_submit=fgetc(m)=='1';fclose(m);}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-async-pool-reset").c_str(),"rb");poolreset::enabled=false;if(w){poolreset::enabled=fgetc(w)!='0';fclose(w);}else if(const char *e=getenv("DM_GFX_ASYNC_POOL_RESET"))poolreset::enabled=atoi(e)!=0;poolreset::start();xbg_logf("NVN async command pool reset=%d (marker gfx-async-pool-reset)",int(poolreset::enabled));}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-no-bindcache").c_str(),"rb");if(w){bind_cache_enabled=false;fclose(w);}}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-no-imgcache").c_str(),"rb");if(w){img_cache_enabled=false;fclose(w);}}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-pass-profile").c_str(),"rb");if(w){pass_prof_enabled=true;unsigned long long from=0,period=0,count=0;if(fscanf(w,"%llu %llu %llu",&from,&period,&count)>=2){pass_prof_next=from;pass_prof_period=period?period:1;if(count)pass_prof_until=from+count;}fclose(w);}}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-no-descmemo").c_str(),"rb");if(w){desc_memo_enabled=false;fclose(w);}xbg_logf("NVN descmemo enabled=%d",int(desc_memo_enabled));}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-no-dynpartial").c_str(),"rb");if(w){dyn_partial_enabled=false;fclose(w);}}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-no-xmemo").c_str(),"rb");if(w){xmemo_enabled=false;fclose(w);}xbg_logf("NVN cross-frame memo enabled=%d",int(xmemo_enabled));}{auto marker=[&](const char *name){FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+name).c_str(),"rb");int c=-1;if(w){c=fgetc(w);fclose(w);}return c;};persistent_fb_enabled=marker("/gfx-no-persistent-fb")<0;slot_pools_enabled=marker("/gfx-no-slot-pools")<0;readback_cached=marker("/gfx-readback-cached")=='1' || (marker("/gfx-readback-cached")<0 && getenv("DM_GFX_READBACK_CACHED") && *getenv("DM_GFX_READBACK_CACHED")=='1');texel_memo_enabled=marker("/gfx-no-texel-memo")<0;no_occlusion_queries=marker("/gfx-no-occlusion")=='1';samples_report_seen=false;open_occlusion=OpenOcclusion{};occlusion_queries_started=0;occlusion_batch_enabled=marker("/gfx-query-per-draw")!='1' && marker("/gfx-no-query-batch")<0;heavy_timestamps=marker("/gfx-heavy-timestamps")=='1';xbg_logf("NVN persistent framebuffers=%d slot command pools=%d cached readback=%d texel memo=%d (markers gfx-no-persistent-fb, gfx-no-slot-pools, gfx-readback-cached, gfx-no-texel-memo)",int(persistent_fb_enabled),int(slot_pools_enabled),int(readback_cached),int(texel_memo_enabled));}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-wtrack").c_str(),"rb");wtrack::marker=-1;if(w){wtrack::marker=fgetc(w)=='1'?1:0;fclose(w);}wtrack::enabled=wtrack::marker==1;}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-no-wtrack").c_str(),"rb");if(w){wtrack::enabled=false;wtrack::marker=0;fclose(w);}xbg_logf("NVN wtrack enabled=%d (markers gfx-no-wtrack, gfx-wtrack=0)",int(wtrack::enabled));}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-mutable-images").c_str(),"rb");no_mutable_images=true;if(w){no_mutable_images=fgetc(w)!='1';fclose(w);}xbg_logf("NVN mutable-format images=%d (marker gfx-mutable-images)",int(!no_mutable_images));}{FILE *w=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-mutable-test").c_str(),"rb");mutable_test=0;if(w){if(fscanf(w,"%u",&mutable_test)!=1)mutable_test=0;fclose(w);}if(mutable_test)xbg_logf("NVN mutable upgrade test every %u images (marker gfx-mutable-test)",mutable_test);}
     /* Android: no guest memory remapping probe. */
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-pipe-stats").c_str(),"rb");if(m){fclose(m);pstat_props=reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(vkGetDeviceProcAddr(cfg.device,"vkGetPipelineExecutablePropertiesKHR"));
       pstat_stats=reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(vkGetDeviceProcAddr(cfg.device,"vkGetPipelineExecutableStatisticsKHR"));pipe_stats=pstat_props && pstat_stats;}xbg_logf("NVN pipeline statistics=%d (marker gfx-pipe-stats)",int(pipe_stats));}
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-wb-skip").c_str(),"rb");wb_skip=false;if(m){wb_skip=fgetc(m)!='0';fclose(m);}else if(const char *e=getenv("DM_GFX_WB_SKIP"))wb_skip=atoi(e)!=0;
      xbg_logf("NVN superseded storage writebacks skipped=%d pool_flags=%d (marker gfx-wb-skip)",int(wb_skip),int(nvn_pool_flags!=nullptr));}
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-pipe-memo").c_str(),"rb");pipe_memo=false;pipe_memo_valid=false;if(m){pipe_memo=fgetc(m)!='0';fclose(m);}else if(const char *e=getenv("DM_GFX_PIPE_MEMO"))pipe_memo=atoi(e)!=0;
      xbg_logf("NVN pipeline memo for repeated draw state=%d (marker gfx-pipe-memo)",int(pipe_memo));}
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-read-preflight").c_str(),"rb");read_preflight=true;if(m){read_preflight=fgetc(m)!='0';fclose(m);}else if(const char *e=getenv("DM_GFX_READ_PREFLIGHT"))read_preflight=atoi(e)!=0;
      xbg_logf("NVN CPU reads resolved before each operation=%d (marker gfx-read-preflight)",int(read_preflight));}
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-sync-cache-save").c_str(),"rb");sync_cache_save=false;if(m){sync_cache_save=fgetc(m)=='1';fclose(m);}}
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-xview-guard").c_str(),"rb");xview_guard=false;if(m){xview_guard=fgetc(m)!='0';fclose(m);}else if(const char *e=getenv("DM_GFX_XVIEW_GUARD"))xview_guard=atoi(e)!=0;
      xbg_logf("NVN readback buffers the other slot reads are written by the GPU=%d (marker gfx-xview-guard)",int(xview_guard));}
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-upload-prologue").c_str(),"rb");upload_prologue=false;if(m){upload_prologue=fgetc(m)!='0';fclose(m);}else if(const char *e=getenv("DM_GFX_UPLOAD_PROLOGUE"))upload_prologue=atoi(e)!=0;
      xbg_logf("NVN whole uploads of images not yet used in a submission go ahead of it=%d (marker gfx-upload-prologue)",int(upload_prologue));}
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-async-pipelines").c_str(),"rb");warm::async_on=false;if(m){warm::async_on=fgetc(m)!='0';fclose(m);}else if(const char *e=getenv("DM_GFX_ASYNC_PIPELINES"))warm::async_on=atoi(e)!=0;
      xbg_logf("NVN background builds for skippable draws=%d (marker gfx-async-pipelines)",int(warm::async_on));}
     {uint32_t n=0;vkEnumerateDeviceExtensionProperties(cfg.physical,nullptr,&n,nullptr);std::vector<VkExtensionProperties> ext(n);vkEnumerateDeviceExtensionProperties(cfg.physical,nullptr,&n,ext.data());
      std::string list;for(auto &e:ext){list+=e.extensionName;list+=' ';}
      format_feature_flags2=properties.apiVersion>=VK_API_VERSION_1_3;for(auto &e:ext)if(!strcmp(e.extensionName,VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME))format_feature_flags2=true;
      xbg_logf("NVN format-feature-flags2=%d (formatless storage checks %s)",int(format_feature_flags2),format_feature_flags2?"per format":"by the pre-1.3 format list");
      capability_report();
      {const VkFormatProperties &d16=format_properties(VK_FORMAT_D16_UNORM),&d24=format_properties(VK_FORMAT_X8_D24_UNORM_PACK32);
       const VkFormatFeatureFlags d24_needs=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
       bool d16_linear=(d16.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)!=0;depth16_as_d24=!d16_linear && (d24.optimalTilingFeatures&d24_needs)==d24_needs;
       xbg_logf("NVN depth16 stored as %s (D16 linear filtering=%u, X8_D24 features=%#x)",depth16_as_d24?"X8_D24":"D16",unsigned(d16_linear),d24.optimalTilingFeatures);
       xbg_logf("NVN compatibility format_feature_flags2=%d depth16_as_d24=%d",int(format_feature_flags2),int(depth16_as_d24));}
      for(size_t at=0;at<list.size();at+=900)xbg_logf("NVN caps extensions[%zu] %s",at/900,list.substr(at,900).c_str());
      xbg_logf("NVN caps api=%u.%u.%u driver=%u maxUBdyn=%u maxPerStageUB=%u minUBAlign=%llu maxBoundSets=%u",VK_API_VERSION_MAJOR(properties.apiVersion),VK_API_VERSION_MINOR(properties.apiVersion),VK_API_VERSION_PATCH(properties.apiVersion),properties.driverVersion,properties.limits.maxDescriptorSetUniformBuffersDynamic,properties.limits.maxPerStageDescriptorUniformBuffers,(unsigned long long)properties.limits.minUniformBufferOffsetAlignment,properties.limits.maxBoundDescriptorSets);}
     {FILE *m=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-ab-toggle").c_str(),"rb");ab_period=0;if(m){if(fscanf(m,"%d",&ab_period)!=1)ab_period=0;fclose(m);}{FILE *t=fopen(((cut==std::string::npos?dir:dir.substr(0,cut))+"/gfx-ab-target").c_str(),"rb");memset(ab_target,0,sizeof ab_target);if(t){if(fscanf(t,"%15s",ab_target)!=1)ab_target[0]=0;fclose(t);}}xbg_logf("NVN PERF ab_period=%d",ab_period);}
     ++slot_serial;gfx_enqueued=0;gfx_executed=0;gfx_presents_queued=0;gfx_async=false; /* Android game command memory remains guest-owned. */if(gfx_async)gfx_worker=std::thread(gfx_worker_loop);xbg_logf("NVN async translation=%d",int(gfx_async));}
    return 0;
}
// nvnEventGetValue on a deferred event: finish the GPU work the signal covers, then the value is published (apply_ready_events).
extern "C" int xbg_evguard_fault(void *addr,int is_write,void *uc){return evguard::enabled?evguard::fault(addr,is_write,uc):0;}
extern "C" void xbg_game_event_read(const void *addr){
    gfx_drain();
    std::lock_guard<std::recursive_mutex> lock(mutex);if(!initialized || failed)return;
    for(auto *list:{&pending_events,&recording_events})for(auto &e:*list)if(e.addr==uint64_t(reinterpret_cast<uintptr_t>(addr))){++event_reads;complete_all();return;}
}
extern "C" int xbg_game_set_sampler_mirror_clamp_to_edge(VkBool32 enabled){
    if(enabled!=VK_FALSE && enabled!=VK_TRUE)return -1;
    std::lock_guard<std::recursive_mutex> lock(mutex);if(!initialized || failed || stats.submissions || stats.draws || stats.dispatches)return -1;
    if(enabled){
        uint32_t count=0;VkResult result=vkEnumerateDeviceExtensionProperties(cfg.physical,nullptr,&count,nullptr);if(result!=VK_SUCCESS)return result;
        std::vector<VkExtensionProperties> extensions(count);result=vkEnumerateDeviceExtensionProperties(cfg.physical,nullptr,&count,extensions.data());if(result!=VK_SUCCESS)return result;
        bool supported=false;for(const auto &e:extensions)if(!strcmp(e.extensionName,VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME))supported=true;
        if(!supported && properties.apiVersion>=VK_API_VERSION_1_2){VkPhysicalDeviceVulkan12Features core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&core;vkGetPhysicalDeviceFeatures2(cfg.physical,&f);supported=core.samplerMirrorClampToEdge;}
        if(!supported)return -2;
    }
    sampler_mirror_clamp_enabled=enabled;xbg_logf("NVN sampler mirror-clamp-to-edge actualEnabled=%u",enabled);return 0;
}

extern "C" int xbg_game_provenance(const char *path){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(!path){provenance_close("CLOSE");return 0;}
    if(!initialized || provenance_file || capture.trace || timing_active || timing_first || failed || stats.presents.load()>UINT64_MAX-2)return -1;
    FILE *f=fopen(path,"wx");if(!f)return -2;
    provenance.arm(stats.presents.load());provenance_file=f;
    char begin[128];int n=snprintf(begin,sizeof(begin),"BEGIN target=%llu maxDraws=64 maxEvents=2048 maxText=262144\n",(unsigned long long)provenance.target);
    if(n<0 || size_t(n)>=sizeof(begin) || fwrite(begin,1,size_t(n),f)!=size_t(n) || fflush(f)){provenance_close("OPEN_IO_FAIL");return -2;}
    provenance.bytes=uint64_t(n);return 0;
}

extern "C" int xbg_game_capture_frame(const char *directory,uint64_t frame){
    if(!directory || !directory[0] || !frame || strlen(directory)>800)return -1;
    std::lock_guard<std::recursive_mutex> lock(mutex);if(!initialized || failed || stats.submissions || stats.draws || stats.dispatches || capture.trace || timing_first)return -1;
    std::string path=std::string(directory)+"/capture.txt";FILE *trace=fopen(path.c_str(),"wb");if(!trace)return -2;
    capture.directory=directory;capture.trace=trace;capture.frame=frame;capture.bytes=0;capture.events=0;capture.active=false;
    capture_log("CAPTURE version=1 frame=%llu maxEvents=16 maxBytes=%llu semantics=post-fence-raw-no-state-replacement",(unsigned long long)frame,(unsigned long long)capture_limit);
    xbg_logf("NVN one-frame capture frame=%llu directory=%s maxEvents=16 maxBytes=%llu",(unsigned long long)frame,directory,(unsigned long long)capture_limit);return 0;
}
extern "C" int xbg_game_profile_cpu_live(uint32_t frame_count){gfx_drain();
    if(!frame_count || frame_count>60)return -1;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    const uint64_t current=stats.presents.load();
    if(!initialized || failed || live_profile_used || timing_active || timing_first || capture.trace || provenance_file || current>UINT64_MAX-uint64_t(frame_count)-1)return -1;
    // Requested from inside the present callback (a timing scope is open): arm at the next submit instead.
    if(timing.top){deferred_live_profile=frame_count;live_profile_used=true;return 0;}
    timing=XbgCpuTiming(timing_clock);timing_first=current+2;timing_last=timing_first+frame_count-1;live_profile_used=true;
    xbg_logf("NVN CPU_TIMING armed-live first=%llu count=%u maxSeconds=60 clock=steady elapsed-not-thread-cpu once=1",(unsigned long long)timing_first,frame_count);
    return 0;
}
extern "C" int xbg_game_profile_descriptor_live(uint32_t frame_count){gfx_drain();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // Reuse exact once/pending/active/nonempty-scope/provenance/capture checks.
    int result=xbg_game_profile_cpu_live(frame_count);if(result)return result;
    descriptor_timing=XbgDescriptorTiming{};descriptor_detail=true;
    xbg_logf("NVN DESC_TIMING armed v=2 first=%llu count=%u once=1 maxSeconds=60",(unsigned long long)timing_first,frame_count);
    return 0;
}
extern "C" int xbg_game_profile_draw_live(uint32_t frame_count){gfx_drain();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    // Reuse the original live once/pending/active/provenance/capture gates.
    int result=xbg_game_profile_cpu_live(frame_count);if(result)return result;
    draw_timing=XbgDrawTiming{};draw_detail=true;
    xbg_logf("NVN DRAW_TIMING armed v=1 first=%llu count=%u once=1 maxSeconds=60",(unsigned long long)timing_first,frame_count);
    return 0;
}
extern "C" int xbg_game_profile_cpu(uint64_t first_frame,uint32_t frame_count){
    if(!first_frame || !frame_count || frame_count>120 || first_frame>UINT64_MAX-frame_count)return -1;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(!initialized || failed || stats.submissions || stats.draws || stats.dispatches || stats.presents || timing_first || capture.trace)return -1;
    timing=XbgCpuTiming(timing_clock);timing_first=first_frame;timing_last=first_frame+frame_count-1;
    xbg_logf("NVN CPU_TIMING armed first=%llu count=%u maxSeconds=60 clock=steady elapsed-not-thread-cpu",(unsigned long long)first_frame,frame_count);
    if(first_frame==1)timing_start();return 0;
}
extern "C" int xbg_game_shutdown(){gfx_drain();gfx_async_stop();std::lock_guard<std::recursive_mutex> lock(mutex);if(!initialized)return 0;timing_report("shutdown-partial");provenance_close("SHUTDOWN");resolve_pending();submit_slots(true);save_pipeline_cache();pending_writebacks.clear();pending_reports.clear();pending_events.clear();recording_events.clear();epochs.clear();VkResult r=vkQueueWaitIdle(cfg.queue);if(r!=VK_SUCCESS)return r;wtrack::release();destroy_slots();clear_pipeline_objects();cache_save_join();warm::shutdown();bright::shutdown();clear_descriptor_caches();clear_render_pass_cache();images.clear();++images_epoch;for(auto &f:persistent_framebuffers)vkDestroyFramebuffer(cfg.device,f.second,nullptr);persistent_framebuffers.clear();image_index.clear();image_index_dirty=true;program_cache.clear();shaders.clear();textures.clear();texture_pool_cache=nullptr;samplers.clear();null_textures.clear();texture_pool_cache=nullptr;pools.clear();host_segments.clear();host_ranges.clear();host_bytes=0;cmd_range_lo=0;cmd_range_bytes=0;vkDestroyPipelineCache(cfg.device,pipeline_cache,nullptr);pipeline_cache=VK_NULL_HANDLE;vkDestroyCommandPool(cfg.device,command_pool,nullptr);command_pool=VK_NULL_HANDLE;initialized=false;return 0;}
extern "C" void xbg_game_get_stats(xbg_game_stats *out){if(out)*out={stats.submissions.load(),stats.draws.load(),stats.dispatches.load(),stats.presents.load(),stats.shader_loads.load()};}
extern "C" void xbg_game_frame_counters(xbg_frame_counters *out){if(out)*out={hud::pipelines.load(),hud::pipeline_ns.load(),hud::shaders.load(),hud::shader_ns.load(),hud::uploads.load(),hud::reuploads.load(),hud::upload_bytes.load(),hud::upload_ns.load()};}
extern "C" void xbg_game_set_frame(uint64_t frame){hud::frame.store(frame,std::memory_order_relaxed);}
extern "C" double xbg_game_gpu_ms(uint64_t frame){const hud::Gpu &g=hud::gpu[frame%64];if(g.frame.load(std::memory_order_acquire)!=frame)return -1;return double(g.ns.load(std::memory_order_acquire))/1e6;}
static void xbg_legacy_pool_add_now(void *p,size_t n){std::lock_guard<std::recursive_mutex> lock(mutex);need(p&&n,"Invalid NVN pool");pools[uintptr_t(p)]=n;cmd_range_lo=0;cmd_range_bytes=0;auto epoch=provenance.pool(uint64_t(p),true);provenance_log("POOL_ADD mem=%p bytes=%zu epoch=%llu",p,n,epoch);}
static void xbg_legacy_pool_del_now(void *p){std::lock_guard<std::recursive_mutex> lock(mutex);provenance_log("POOL_DEL mem=%p epoch=%llu",p,provenance.epoch(uint64_t(p)));provenance.pool(uint64_t(p),false);{auto pit=pools.find(uintptr_t(p));if(pit!=pools.end())vcache::drop_pool(uintptr_t(p),pit->second);}pools.erase(uintptr_t(p));cmd_range_lo=0;cmd_range_bytes=0;}
static void xbg_register_texture_now(const dpool_t *p,int id,const tex_t *t,const view_t *v){std::lock_guard<std::recursive_mutex> lock(mutex);need(p&&id>=0&&id<p->n&&t,"Invalid texture registration");auto &slot=textures[pool_key(p)][id];if(register_texture_record(slot,*t,v))++memo_bump[0];}
static void xbg_register_sampler_now(const dpool_t *p,int id,const samp_par *s){std::lock_guard<std::recursive_mutex> lock(mutex);need(p&&id>=0&&id<p->n&&s,"Invalid sampler registration");auto &slot=samplers[pool_key(p)][id];if(memcmp(&slot,s,sizeof(*s))){++memo_epoch;++memo_bump[1];}slot=*s;}
static void xbg_legacy_tex_release_now(tex_t *t){std::lock_guard<std::recursive_mutex> lock(mutex);if(t){auto i=images.find(t->serial);if(i!=images.end()){provenance_image("RELEASE",t,*i->second);retire_object(i->second);++memo_bump[2];}images.erase(t->serial);image_index_dirty=true;++images_epoch;++memo_check_gen;}}
static void xbg_legacy_prog_release_now(prog_t *p){/* Immutable shader modules are keyed by original control digest, shared safely. */std::lock_guard<std::recursive_mutex> lock(mutex);if(p && p->serial)program_cache.erase(p->serial);}
static void xbg_texture_invalidate_now(const tex_t *t){std::lock_guard<std::recursive_mutex> lock(mutex);if(t){auto i=images.find(t->serial);if(i!=images.end()){need(!i->second->gpu_written,"CPU invalidation of GPU-written texture needs explicit range merge");i->second->uploaded=false;}}}
extern "C" void *xbg_host_command_memory(cmd_t *owner,size_t n){gfx_drain();std::lock_guard<std::recursive_mutex> lock(mutex);need(n<=64*1024*1024 && host_bytes+n<=256ull*1024*1024,"Host command memory cap reached; finalize handles before reusing");auto p=std::make_unique<uint8_t[]>(n);void *out=p.get();host_ranges[uintptr_t(out)]=n;host_segments.push_back({owner,std::move(p),n});host_bytes+=n;return out;}
extern "C" void xbg_release_command_handle(uint64_t handle){
    // A handle can CALL another handle and game-supplied segments can point to
    // host segments. Retain until shutdown until reference ownership is proven.
    (void)handle;
}
extern "C" void xbg_copy_commands(cmd_t *command,int count,const uint64_t *handles){gfx_drain();
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto available=[](void *,const void *p)->size_t{return command_available(static_cast<const uint8_t*>(p));};
    size_t bytes=0;int result=xbg_command_clone(count,handles,nullptr,256ull*1024*1024,available,nullptr,&bytes);
    if(result)xbg_fatalf("CopyCommands invalid source chain: %d",result);
    std::vector<uint8_t> snapshot(bytes);size_t actual=0;
    result=xbg_command_clone(count,handles,snapshot.data(),snapshot.size(),available,nullptr,&actual);
    need(result==0 && actual==bytes,"CopyCommands source changed while recording");
    for(size_t offset=0;offset<bytes;){op_hdr h;memcpy(&h,snapshot.data()+offset,8);xbg_record_command(command,snapshot.data()+offset,h.size);offset+=h.size;}
}
extern "C" void xbg_invoke_command_callback(cmd_t *c,size_t n){uint64_t args[]={uint64_t(c),NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY,n,uint64_t(c->cb_data)};if(cfg.invoke_guest)cfg.invoke_guest(cfg.userdata,reinterpret_cast<void*>(c->cb),args,4);else c->cb(reinterpret_cast<NVNcommandBuffer*>(c),NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY,n,c->cb_data);}
static void xbg_submit_now(int n,const uint64_t *handles){std::lock_guard<std::recursive_mutex> lock(mutex);
    if(deferred_live_profile && !timing.top){uint64_t current=stats.presents.load();timing=XbgCpuTiming(timing_clock);timing_first=current+2;timing_last=timing_first+deferred_live_profile-1;deferred_live_profile=0;}
   TimingRoot timing_root;CPU_SCOPE(Other);need(initialized && !failed,"Game Vulkan backend not initialized/healthy");uint64_t begin=timing_clock();snapshot_reset();uint64_t budget=0;for(int i=0;i<n;i++)execute(reinterpret_cast<const uint8_t*>(handles[i]),0,budget);++stats.submissions;++submitted;if(sync_mode)complete_all();perf.submit_cpu_ns+=timing_clock()-begin;}
extern "C" uint64_t xbg_legacy_last_fence(){return gfx_async?gfx_enqueued.load():[]{std::lock_guard<std::recursive_mutex> lock(mutex);return submitted;}();}
extern "C" int xbg_legacy_wait(uint64_t fence,uint64_t timeout){
    // 不取锁的提前返回只用于新写法；旧写法（非鸿蒙缺省）先判 failed，与原来逐条相同
    // the lock-free early return is new-path only; the old path (default off HarmonyOS) checks failed first, exactly as before
    if(!legacy_waitall && fence<=completed.load())return 1;
    const bool completion_path=completion_wait_enabled && timeout && !legacy_waitall;
    // 新路径的唯一总预算含初锁、重锁与四轮后的回退；旧路径保留原时限语义。
    // One new-path budget includes initial lock, relock and the post-four-round fallback; preserve the old path's timeout semantics.
    const xbg_completion::Deadline deadline(completion_path?timeout:UINT64_MAX);
    // 只等覆盖 fence 的那一批（同 wait_fence / d2r-harmonyos），且等 GPU 时不持引擎锁：游戏每帧都等刚交的那一帧，旧写法先强交、再等全部在途命令，
    // 全程攥着引擎锁，翻译线程的出图每帧被堵约 16 ms。轮询（timeout 0）只 try_lock，翻译线程正持锁就当没完成，不排队。按提交顺序退槽（写回不越序）。
    // 标记 gfx-legacy-waitall=1 / 0 选旧 / 新写法；非鸿蒙缺省旧写法（与原来逐条相同）。
    // Wait only for the batch covering `fence` (as wait_fence / d2r-harmonyos), without holding the engine lock while the GPU works: the game waits every frame for the frame it just
    // submitted; the old code force-submitted, waited for all in-flight work and held the engine lock throughout, blocking translation's present ~16 ms per frame. A poll (timeout 0)
    // only try_locks: if translation holds the lock it reports not done instead of queueing. Slots retire in submission order (writebacks stay ordered). Marker gfx-legacy-waitall=1 / 0 picks the old / new code; off HarmonyOS the old code (unchanged) is the default.
    std::unique_lock<std::recursive_mutex> lock(mutex,std::defer_lock);
    // 仅对可能阻塞的调用记 trace；保持单次 try_lock 轮询与原退休顺序。
    // Trace potentially blocking calls only; retain single-try-lock polls and the original retirement order.
    if(timeout || legacy_waitall){XTRACE_COUNT("dm-legacy-target", fence);XTRACE_COUNT("dm-legacy-completed-before-lock", completed.load());}
    if(!timeout && !legacy_waitall){if(!lock.try_lock())return 0;}else{
        XTRACE_BEGIN("dm-legacy-lock");
        const auto acquired=completion_path?completion_wait.acquire(lock,fence,deadline):(lock.lock(),xbg_completion::Acquire::EngineLocked);
        XTRACE_END();XTRACE_COUNT("dm-legacy-lock-result", int(acquired));
        if(acquired==xbg_completion::Acquire::Completed){XTRACE_COUNT("dm-legacy-completed", completed.load());return 1;}
        if(acquired==xbg_completion::Acquire::TimedOut)return 0;
    }
    if(timeout || legacy_waitall){XTRACE_COUNT("dm-legacy-submitted", submitted);XTRACE_COUNT("dm-legacy-completed", completed.load());}
    if(failed || fence>submitted)return 0;if(fence<=completed)return 1;
    if(completion_path && deadline.expired())return 0;
    if(!legacy_waitall){
        auto begin=std::chrono::steady_clock::now();
        auto retire_done=[&]{for(int i=0;i<2;i++){if(completion_path && deadline.expired())break;Slot &o=slots[slot_index].pending?slots[slot_index]:slots[slot_index^1];if(!o.pending || vkGetFenceStatus(cfg.device,o.fence)!=VK_SUCCESS)break;SiteScope site(SITE_LEGACY_WAIT);retire_slot(o);}};
        for(int round=0;round<4;round++){
            if(completion_path && deadline.expired())return fence<=completed;
            retire_done();if(failed)return 0;if(fence<=completed)return 1;
            if(completion_path && deadline.expired())return 0;
            Slot &a=slots[slot_index],&b=slots[slot_index^1];Slot &older=a.pending?a:b,&newer=a.pending?b:a;Slot &cur=slots[slot_index];
            // 没有在途槽、录制槽也是空的：已翻译的提交全都做完了（游戏常等一次不含 GPU 命令的提交）| Nothing in flight and an empty recording slot: every translated submit is done (the game often waits on a submit with no GPU commands)
            if(!older.pending && !newer.pending && !(cur.recording && cur.ops)){resolve_known_reports();if(pending_writebacks.empty()&&pending_reports.empty()){publish_completed(std::max(completed.load(),submitted));}if(fence<=completed)return 1;break;}
            if(!timeout)return 0;
            Slot *t=older.pending && older.covers>=fence?&older:newer.pending && newer.covers>=fence?&newer:nullptr;
            if(!t){if(cur.recording && cur.ops){SiteScope site(SITE_LEGACY_WAIT);submit_slots(false);continue;}   // 目标还在录制中的槽：交掉再等 | target still in the recording slot: submit it, then wait
                t=newer.pending?&newer:&older;}   // 目标提交没有 GPU 命令：等最新的在途批次 | the target submit has no GPU commands: wait for the newest in-flight batch
            uint64_t remaining=completion_path?deadline.remaining_ns():timeout;if(!completion_path && timeout!=UINT64_MAX){auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();remaining=uint64_t(elapsed)>=timeout?0:timeout-uint64_t(elapsed);}
            XTRACE_COUNT("dm-legacy-slot", t-slots);XTRACE_COUNT("dm-legacy-covers", t->covers);
            XTRACE_COUNT("dm-legacy-completed", completed.load());
            VkFence f=t->fence;t->waiters.n.fetch_add(1);lock.unlock();
            XTRACE_BEGIN("dm-legacy-wait");uint64_t b0=timing_clock();VkResult r=vkWaitForFences(cfg.device,1,&f,VK_TRUE,remaining);uint64_t b1=timing_clock();XTRACE_END();
            // 先释放 waiter 再等待完成/重锁；此后槽可被另一线程复用，不再读取 t。
            // Release the waiter before completion/relock waits; another thread may now reuse the slot, so never read t again.
            t->waiters.n.fetch_sub(1);
            if(completion_path){
                completion_wait.record_gpu_wait(b1-b0);
                if(r==VK_TIMEOUT)return 0;
                // 设备错误不能被 completed 快路吞掉；原 fatal 状态仍在 engine 下处理。
                // Completion must not hide device errors; handle the original fatal state while holding engine.
                if(r!=VK_SUCCESS){lock.lock();fold_completion_wait_stats();VK(r);}
                XTRACE_BEGIN("dm-legacy-relock");const auto acquired=completion_wait.acquire(lock,fence,deadline);XTRACE_END();
                XTRACE_COUNT("dm-legacy-relock-result", int(acquired));XTRACE_COUNT("dm-legacy-completed", completed.load());
                if(acquired==xbg_completion::Acquire::Completed)return 1;
                if(acquired==xbg_completion::Acquire::TimedOut)return 0;
                fold_completion_wait_stats();
            }else{
                XTRACE_BEGIN("dm-legacy-relock");lock.lock();XTRACE_END();XTRACE_COUNT("dm-legacy-relock-result", 0);
                XTRACE_COUNT("dm-legacy-completed", completed.load());
                block_ns[SITE_LEGACY_WAIT]+=b1-b0;++block_calls[SITE_LEGACY_WAIT];legacy_wait_ns+=b1-b0;
                if(r==VK_TIMEOUT)return 0;VK(r);
            }}
        retire_done();if(fence<=completed)return 1;}
    if(completion_path && deadline.expired())return fence<=completed;
    SiteScope site(SITE_LEGACY_WAIT);uint64_t w0=timing_clock();struct Spent{uint64_t w0;~Spent(){legacy_wait_ns+=timing_clock()-w0;}}spent{w0};
    submit_slots(false);auto begin=std::chrono::steady_clock::now();
    for(auto &slot:slots)if(slot.pending){uint64_t remaining=completion_path?deadline.remaining_ns():timeout;
        if(completion_path && !remaining)return fence<=completed;
        if(!completion_path && timeout!=UINT64_MAX){auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();remaining=uint64_t(elapsed)>=timeout?0:timeout-uint64_t(elapsed);}
        bool busy=vkGetFenceStatus(cfg.device,slot.fence)==VK_NOT_READY;uint64_t b0=busy?timing_clock():0;
        VkResult r=vkWaitForFences(cfg.device,1,&slot.fence,VK_TRUE,remaining);if(busy){block_ns[SITE_LEGACY_WAIT]+=timing_clock()-b0;++block_calls[SITE_LEGACY_WAIT];}
        if(r==VK_TIMEOUT)return 0;VK(r);retire_slot(slot);
    }
    resolve_known_reports();if(pending_writebacks.empty()&&pending_reports.empty()){publish_completed(submitted);}
    return fence<=completed;
}
extern "C" void xbg_legacy_finish(){gfx_drain();std::lock_guard<std::recursive_mutex> lock(mutex);need(initialized,"Queue finish before Vulkan initialization");++perf.why_finish;complete_all();VK(vkQueueWaitIdle(cfg.queue));}
static void xbg_present_texture_now(const win_t *window,const tex_t *texture){std::lock_guard<std::recursive_mutex> lock(mutex);preflight_cpu_reads();{SiteScope site(SITE_PRESENT);cur_op=61;TimingRoot timing_root;CPU_SCOPE(Other);need(initialized&&cfg.present,"Game presentation callback not installed");uint64_t present_begin=timing_clock();Image &im=get_image(texture,false);need(im.gpu_written,"Game tried to present a texture never rendered");Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);c.finish();submit_slots(false);int r;{CPU_SCOPE(Present);r=cfg.present(cfg.userdata,window->native,im.image,im.fmt,im.width,im.height,window->crop,window->interval);}if(r)xbg_fatalf("Game presentation failed: %d",r);++stats.presents;if(pass_prof_enabled && stats.presents.load()==pass_prof_frame)xbg_logf("NVN GPASS boundary frame=%llu slots=%u skipped_slots=%u reserved_queries=%u max_queries=%u max_slots=%u",(unsigned long long)pass_prof_frame,pass_prof_slots,pass_prof_skipped_slots,pass_prof_queries,pass_prof_query_limit,pass_prof_slot_limit);perf.present_cpu_ns+=timing_clock()-present_begin;perf_frame();if(provenance_file && stats.presents.load()>=provenance.target)provenance_close("END");if(capture.trace && stats.presents==capture.frame){capture_log("END frame=%llu events=%u bytes=%llu presentedSerial=%llu image=%p",(unsigned long long)capture.frame,capture.events,(unsigned long long)capture.bytes,(unsigned long long)texture->serial,(void*)im.image);fclose(capture.trace);capture.trace=nullptr;capture.active=false;xbg_logf("NVN one-frame capture complete frame=%llu events=%u bytes=%llu",(unsigned long long)capture.frame,capture.events,(unsigned long long)capture.bytes);}}timing_boundary();}
extern "C" void xbg_legacy_read_texels(const tex_t *t,unsigned level,unsigned layer,const NVNcopyRegion *r,uint8_t *dst,size_t row,size_t image){gfx_drain();std::lock_guard<std::recursive_mutex> lock(mutex);preflight_cpu_reads();need(t&&r&&dst,"Invalid ReadTexels arguments");auto found=images.find(t->serial);if(found==images.end() || !found->second->gpu_written){int bw,bh,bpp=nvn_fmt_bpb(t->d.format,&bw,&bh);need(bpp && r->xoffset==0 && r->yoffset==0 && r->zoffset==0 && r->depth==1 && uint32_t(r->width)==std::max(1u,t->d.width>>level) && uint32_t(r->height)==std::max(1u,t->d.height>>level),"CPU ReadTexels partial region unsupported");need(!(t->d.flags&0x10),"Linear CPU ReadTexels not implemented");need(nvn_deswizzle(&t->d,level,layer,t->mem,dst,row)==0,"ReadTexels deswizzle failed");}
    else{p_copy_b2t p{};p.src=uint64_t(dst);p.tex=t;p.has_view=1;p.view.set_levels=p.view.set_layers=1;p.view.base_level=level;p.view.nlevels=1;p.view.min_layer=layer;p.view.nlayers=1;p.r=*r;int64_t old=state.copy_stride,old_image=state.copy_image_stride;state.copy_stride=row;state.copy_image_stride=image;copy_texture_buffer(p,true);state.copy_stride=old;state.copy_image_stride=old_image;}}

// ---- public entry points (queued in order on the graphics thread) ----
extern "C" void xbg_legacy_pool_add(void *p,size_t n){gfx_enqueue([p,n]{xbg_legacy_pool_add_now(p,n);});}
extern "C" void xbg_legacy_pool_del(void *p){gfx_enqueue([p]{xbg_legacy_pool_del_now(p);});}
extern "C" void xbg_register_texture(const dpool_t *p,int id,const tex_t *t,const view_t *v){
    if(!gfx_async){xbg_register_texture_now(p,id,t,v);return;}
    need(p&&t,"Invalid texture registration");dpool_t pool=*p;tex_t tex=*t;view_t view{};bool has=v!=nullptr;if(v)view=*v;
    gfx_enqueue([pool,id,tex,view,has]{xbg_register_texture_now(&pool,id,&tex,has?&view:nullptr);});}
extern "C" void xbg_register_sampler(const dpool_t *p,int id,const samp_par *s){
    if(!gfx_async){xbg_register_sampler_now(p,id,s);return;}
    need(p&&s,"Invalid sampler registration");dpool_t pool=*p;samp_par sp=*s;gfx_enqueue([pool,id,sp]{xbg_register_sampler_now(&pool,id,&sp);});}
extern "C" void xbg_legacy_tex_release(tex_t *t){if(!t)return;if(!gfx_async){xbg_legacy_tex_release_now(t);return;}tex_t copy=*t;gfx_enqueue([copy]()mutable{xbg_legacy_tex_release_now(&copy);});}
extern "C" void xbg_legacy_prog_release(prog_t *p){if(!p)return;if(!gfx_async){xbg_legacy_prog_release_now(p);return;}prog_t copy=*p;gfx_enqueue([copy]()mutable{xbg_legacy_prog_release_now(&copy);});}
extern "C" void xbg_texture_invalidate(const tex_t *t){if(!t)return;if(!gfx_async){xbg_texture_invalidate_now(t);return;}tex_t copy=*t;gfx_enqueue([copy]{xbg_texture_invalidate_now(&copy);});}
extern "C" void xbg_submit(int n,const uint64_t *handles){
    if(!gfx_async){xbg_submit_now(n,handles);return;}
    std::vector<uint64_t> list(handles,handles+n);++gfx_enqueued;
    gfx_enqueue([list]{xbg_submit_now(int(list.size()),list.data());gfx_executed.store(submitted);});}
extern "C" void xbg_present_texture(const win_t *window,const tex_t *texture){
    if(!gfx_async){xbg_present_texture_now(window,texture);return;}
    // At most two frames queued ahead of the graphics thread.
    {uint64_t t0=uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());std::unique_lock<std::mutex> l(gfx_job_mutex);gfx_idle_cv.wait(l,[]{return gfx_presents_queued.load()<2;});gfx_pace_ns+=uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())-t0;}
    win_t w=*window;tex_t t=*texture;++gfx_presents_queued;
    gfx_enqueue([w,t]{xbg_present_texture_now(&w,&t);{std::lock_guard<std::mutex> l(gfx_job_mutex);--gfx_presents_queued;}gfx_idle_cv.notify_all();});}

extern "C" void xbg_legacy_register_texture(int id,const tex_t *t,const view_t *v){std::lock_guard<std::recursive_mutex> l(mutex);need(id>=0&&t,"Invalid legacy texture registration");if(register_texture_record(legacy_textures[id],*t,v))++memo_bump[0];}
extern "C" void xbg_legacy_register_sampler(int id,const samp_par *s){std::lock_guard<std::recursive_mutex> l(mutex);need(id>=0&&s,"Invalid legacy sampler registration");legacy_samplers[id]=*s;++memo_epoch;++memo_bump[1];}
