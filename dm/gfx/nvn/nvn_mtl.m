// NVN 空设备的 Metal 执行层，见 nvn_mtl.h。
// Metal execution layer of the NVN null device, see nvn_mtl.h.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dlfcn.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "nvn_mtl.h"
#include "nvn_copy_pitch.h"
// 编译期开关 DM_NVN_M3=1：图形后端走旧接口（普通命令队列 / 命令缓冲 / 渲染与计算与拷贝编码器，逐槽绑定缓冲，对参数块引用的资源逐个声明），
// 目标 macOS 15 / iOS 18。缺省 0 = Metal 4（macOS 26 / iOS 26）。两支共用同一份回放逻辑，分叉处都在 #if DM_NVN_M3 里
// Compile-time switch DM_NVN_M3=1: the graphics backend uses the older API (ordinary command queue / command buffers / render, compute and copy encoders, buffers bound per slot, each resource referenced by an argument block declared individually),
// target macOS 15 / iOS 18. Default 0 = Metal 4 (macOS 26 / iOS 26). Both branches share the same replay logic, and every divergence is inside #if DM_NVN_M3
#ifndef DM_NVN_M3
#define DM_NVN_M3 0
#endif
#if DM_NVN_M3
#define RENC id<MTLRenderCommandEncoder>
#define CENC id<MTLComputeCommandEncoder>
#define BENC id<MTLBlitCommandEncoder>
#define CBUF id<MTLCommandBuffer>
#define RPD MTLRenderPassDescriptor
#define FB_ON(o, cb) cb
#define M4_ONLY(...)
#define M3_PRESENT(cb, dr) [cb presentDrawable:dr]
#define FB_SEL addCompletedHandler
#define FB_ARG id<MTLCommandBuffer>
#else
#define RENC id<MTL4RenderCommandEncoder>
#define CENC id<MTL4ComputeCommandEncoder>
#define BENC id<MTL4ComputeCommandEncoder>
#define CBUF id<MTL4CommandBuffer>
#define RPD MTL4RenderPassDescriptor
#define FB_ON(o, cb) o
#define M4_ONLY(...) __VA_ARGS__
#define M3_PRESENT(cb, dr) [dr present]
#define FB_SEL addFeedbackHandler
#define FB_ARG id<MTL4CommitFeedback>
#endif
#include "nvn_pace.h"
#include "nvn_obj.h"
#include "../tex/nvn_fmt_mtl.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_deswizzle_gpu.h"
#include "../tex/nvn_bcdec.h"
#include "nvn_builtin.h"
#include "nvn_dirty.h"
#include "nvn_refl.h"
#include "nvn_cover.h"
#include "dmsh_chain.h"   // DMSH_CHAIN：本源码树期望的 B 着色器库翻译链版本 | DMSH_CHAIN: the translation chain version of the B shader library expected by this source tree
#include "nvn_hmap.h"
#include "../../host/dm.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <mach-o/dyld.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <execinfo.h>
#include <os/lock.h>
// 本文件的函数参数一律不在被调方重复持有（调用方 / 全局表已持有，生命期由 defer_obj_release 保证）：热路径每次调用省一对 retain / release
// Function parameters in this file are never retained again in the callee (the caller / global tables already hold them, and lifetime is guaranteed by defer_obj_release): saves a retain / release pair per call on the hot path
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)

#define MLOG(...) do { fprintf(stderr, "[dm] [nvn-mtl] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define MDIE(...) do { MLOG("FATAL " __VA_ARGS__); abort(); } while (0)

static void warm_start(void);
static void warm_claim(void);

static id<MTLDevice> g_dev;
static NSMutableArray *g_l3_gpu_leaks;
static int g_nvn_deswz_gpu;
static id<MTLCommandQueue> g_q;
#if !DM_NVN_M3
static id<MTL4CommandQueue> g_q4;
static dispatch_queue_t g_feedback_q;
#endif
// 命令流队列只许一条：游戏的命令缓冲与出图都提交到它（Metal 4 = g_q4，旧接口 = g_q；Metal 4 构建里的 g_q 只给 CPU 同步等完的回读 / 截图 / 收尾用）。
// WaitSync 空跑即正确靠这个前提（见 hle_nvn.c g_nops 里 WaitSync 的注释）：建命令流队列一律经这里，第二次建即报红中止
// Only one command queue is allowed: the game's command buffers and presents are all submitted to it (Metal 4 = g_q4, legacy API = g_q; in Metal 4 builds g_q is only for CPU-synchronous readback / screenshots / teardown).
// WaitSync being correct as a no-op relies on this (see the WaitSync comment in g_nops of hle_nvn.c): all command queues are created here, and a second creation reports red and aborts
static void stream_queue_once(void) { static int n; if (++n > 1) MDIE("【红】建了第 %d 条命令流队列：WaitSync 空跑的前提是只有一条（见 hle_nvn.c g_nops 里 WaitSync 的注释），加队列先把 WaitSync 改成真等", n); }
static _Atomic uint32_t g_present_qos;
#define ALLOC_MAX 8
static int g_alloc_n, g_alloc_cur;   // 主提交的在途槽（Metal 4：每槽一个命令分配器；旧接口：只用来限制在途提交数） | in-flight slots of the main submission (Metal 4: one command allocator per slot; older API: used only to limit the number of in-flight submissions)
#if !DM_NVN_M3
static id<MTL4CommandAllocator> g_alloc4[ALLOC_MAX];   // 主提交的命令分配器池：开工时挑一个 GPU 已用完的，没有就新建（到上限才等） | Command allocator pool of the main submission: at start pick one the GPU has finished with, create a new one if there is none (wait only at the cap)
static id<MTL4CommandAllocator> g_up_alloc4[3];
static id<MTL4CommandAllocator> g_present_alloc4[3];
static CBUF g_present_cb[3];
static id<MTL4ArgumentTable> g_vt, g_ft, g_ct, g_up_ct;
static id<MTL4ArgumentTable> g_present_args;
#endif
static id<MTLBuffer> g_present_uv[3];
#define MP_SLOTS 9
static id<MTLBuffer> g_mp_buf[3];   // 紫闪探针：每个出图槽一份，5 个画面点 + 4 格调色表 | Magenta probe: one per present slot, 5 picture points + 4 color-table cells
int g_bloom_q = -1;   // 泛光 1/4 起点（非 static：真机同进程交替直接改 0 / 1） | Bloom 1/4 starting point (non-static: real-device same-process alternation sets it to 0 / 1 directly)
#if !DM_NVN_M3
static id<MTL4ArgumentTable> g_bloomq_args;
#endif
static id<MTLBuffer> g_bloomq_const[3];
static id<MTLSamplerState> g_bloomq_samp;
static id<MTLRenderPipelineState> g_bloomq_pso;
static MTLPixelFormat g_bloomq_fmt;
static id<MTLTexture> g_bloomq_view;
static int g_bloomq_i;
static struct { int state; uint64_t s_ser, b0_ser, b1_ser, s_h, samp_h, b1_h; const tex_t *s_tex; float threshold, coeff_g, coeff_b; } g_bloomq;
static uint64_t g_bloomq_last_b0, g_bloomq_last_b1h, g_bloomq_last_b1s;   // 最近一次泛光 1/4 叠加改读：B0 序号 → B1 句柄（跨帧保留，见 2e2d6700a2569795 那支） | Most recent bloom 1/4 overlay read redirection: B0 serial → B1 handle (kept across frames, see the 2e2d6700a2569795 branch)
static uint64_t g_present_alloc_fence[3];
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_link_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_link_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_link_tick_ns, g_link_tick_n;
static uint64_t g_link_dropped;
static int g_pq_inflight;   // 持 g_pq_mu：已提交给 GPU、完成回调还没来的上屏数 | Under g_pq_mu: number of presents committed to the GPU whose completion callback has not arrived yet
int g_dm_present_link;   // 出图节拍模式（DM_PRESENT_LINK 的运行时值，同进程交替测量时直接改它）：0 = 自己取 drawable；1 = 显示节拍 60 Hz；2 = 120 Hz 节拍、游戏隔拍出一帧 | Present pacing mode (runtime value of DM_PRESENT_LINK, modified directly during same-process alternating measurement): 0 = acquire the drawable itself; 1 = display link at 60 Hz; 2 = display link at 120 Hz, the game presents a frame every other display-link tick
static int g_present_latency = 1;
static uint64_t g_pq_done;
static uint64_t g_pq_read;
static id<MTLResidencySet> g_res;
static int g_res_dirty;
static id<MTLSharedEvent> g_ev;
static uint64_t g_fence;                        // 已提交的最大栅栏值（提交线程独占递增） | Highest committed fence value (incremented exclusively by the submit thread)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_done;                         // 已完成的最大栅栏值（完成回调写） | Highest completed fence value (written by the completion callback)
static _Atomic int g_last_commit_error;
static _Atomic int g_up_pending;

static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;   // 驻留集专用锁：res_add 会在持 g_mu 的路径里调用（arena_new），不能共用 g_mu | Dedicated lock for the residency set: res_add is called on paths that hold g_mu (arena_new), so it cannot share g_mu
static NSHashTable *g_res_objects;
#if DM_NVN_M3
// 旧接口按「缓冲对象 + 偏移」绑定，而回放层手里是显卡地址：登记过的缓冲（内存池的零拷贝缓冲、参数区块、暂存块等，几十个）按地址区间反查。
// 弱引用登记（__unsafe_unretained + 撤驻留时摘掉）；最近命中的那一项先试——同一通道里的绑定几乎都落在同一两个缓冲上
// The older API binds by "buffer object + offset", while the replay layer holds GPU addresses: registered buffers (zero-copy buffers of memory pools, argument blocks, staging blocks and so on, a few dozen) are looked up by address range.
// Registered by weak reference (__unsafe_unretained, removed when residency is withdrawn); the most recently hit entry is tried first - bindings within one pass almost always land on the same one or two buffers
enum { M3_BUFS = 512 };
static struct { uint64_t gpu, len; __unsafe_unretained id<MTLBuffer> buf; } g_m3_bufs[M3_BUFS]; static int g_m3_nbufs; static os_unfair_lock g_m3_lk = OS_UNFAIR_LOCK_INIT;
static void m3_buf_add(id<MTLBuffer> b) { os_unfair_lock_lock(&g_m3_lk); if (g_m3_nbufs < M3_BUFS) { g_m3_bufs[g_m3_nbufs].gpu = b.gpuAddress; g_m3_bufs[g_m3_nbufs].len = b.length; g_m3_bufs[g_m3_nbufs++].buf = b; } os_unfair_lock_unlock(&g_m3_lk); }
static void m3_buf_del(id b) { os_unfair_lock_lock(&g_m3_lk); for (int i = 0; i < g_m3_nbufs; i++) if (g_m3_bufs[i].buf == b) { g_m3_bufs[i] = g_m3_bufs[--g_m3_nbufs]; break; } os_unfair_lock_unlock(&g_m3_lk); }
static id<MTLBuffer> m3_buf(uint64_t gpu, NSUInteger *off) {
    static _Thread_local int last; id<MTLBuffer> r = nil; *off = 0; if (!gpu) return nil;
    os_unfair_lock_lock(&g_m3_lk);
    int i = last < g_m3_nbufs && gpu >= g_m3_bufs[last].gpu && gpu - g_m3_bufs[last].gpu < g_m3_bufs[last].len ? last : -1;
    for (int k = 0; i < 0 && k < g_m3_nbufs; k++) if (gpu >= g_m3_bufs[k].gpu && gpu - g_m3_bufs[k].gpu < g_m3_bufs[k].len) i = k;
    if (i >= 0) { last = i; r = g_m3_bufs[i].buf; *off = (NSUInteger)(gpu - g_m3_bufs[i].gpu); }
    os_unfair_lock_unlock(&g_m3_lk);
    return r;
}
#endif
// 测试开关 DM_RES_CENSUS=1（不常开）：记下进过驻留集的全部对象（弱引用），内存读数行那一拍按「还在驻留集里 / 已移出驻留集却还没销毁」各报个数与占用。
// 后一类 = 我们已经不用、但还有人攥着引用（例如客体线程上从不排空的自动释放池）的显卡内存
// Test switch DM_RES_CENSUS=1 (not normally on): records every object that has entered the residency set (weak references); on the tick of the memory readout line it reports the count and size of those "still in the residency set" and those "removed from the residency set but not yet destroyed".
// The latter = GPU memory we no longer use but someone still holds a reference to (for example an autorelease pool on a guest thread that is never drained)
static NSHashTable *g_res_ever; static int g_res_census = -1;
static void res_add(id<MTLResource> r) {
    if (!r) return;
    pthread_mutex_lock(&g_res_mu);
    [g_res addAllocation:(id<MTLAllocation>)r];
    if (!g_res_objects) g_res_objects = [NSHashTable weakObjectsHashTable];
    [g_res_objects addObject:r];
#if DM_NVN_M3
    if ([r conformsToProtocol:@protocol(MTLBuffer)]) m3_buf_add((id<MTLBuffer>)r);
#endif
    if (g_res_census < 0) g_res_census = getenv("DM_RES_CENSUS") != NULL;
    if (g_res_census > 0) { if (!g_res_ever) g_res_ever = [NSHashTable weakObjectsHashTable]; [g_res_ever addObject:r]; }
    g_res_dirty = 1;
    pthread_mutex_unlock(&g_res_mu);
}
static void res_remove(id<MTLResource> r) {
    if (!r) return;
    pthread_mutex_lock(&g_res_mu);
    [g_res removeAllocation:(id<MTLAllocation>)r];
    [g_res_objects removeObject:r];
#if DM_NVN_M3
    m3_buf_del(r);
#endif
    g_res_dirty = 1;
    pthread_mutex_unlock(&g_res_mu);
}
// DM_RES_CHECK=1（诊断）：着色器经参数块访问的贴图 / 缓冲若不在驻留集里，打出前 40 个（贴图按父贴图 / 底层缓冲归到实际分配）
// DM_RES_CHECK=1 (diagnostic): if textures / buffers the shader accesses through the argument block are not in the residency set, print the first 40 (textures are attributed to the actual allocation via parent texture / underlying buffer)
static int g_res_check_on;
static void res_check(id r, const char *what, int reg) {
    static int nbad;
    if (!g_res_check_on || !r || nbad >= 40) return;
    int is_tex = [r conformsToProtocol:@protocol(MTLTexture)], is_buf = [r conformsToProtocol:@protocol(MTLBuffer)];
    if (!is_tex && !is_buf) return;   // 采样器等不是分配 | Samplers etc. are not allocations
    id<MTLResource> a = r; int hops = 0;
    if (is_tex) { id<MTLTexture> t = (id<MTLTexture>)a; while (t.parentTexture) { t = t.parentTexture; hops++; } a = t.buffer ? (id<MTLResource>)t.buffer : (id<MTLResource>)t; }
    pthread_mutex_lock(&g_res_mu); int ok = [g_res_objects containsObject:a]; pthread_mutex_unlock(&g_res_mu);
    if (!ok) { nbad++; id<MTLTexture> t = is_tex ? (id<MTLTexture>)r : nil;
        MLOG("【红】驻留缺失 #%d：%s 槽 %d %s %p（父链 %d 层）分配 %lu B 存储 %lu%s", nbad, what, reg, is_tex ? "贴图" : "缓冲", (__bridge void *)a, hops, (unsigned long)a.allocatedSize, (unsigned long)a.storageMode,
             t ? [NSString stringWithFormat:@" 格式 %lu %lux%lu 类型 %lu 用途 %#lx", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.textureType, (unsigned long)t.usage].UTF8String : ""); }
}
static void res_commit(void) {
    pthread_mutex_lock(&g_res_mu);
    if (g_res_dirty) { [g_res commit]; g_res_dirty = 0; }
    pthread_mutex_unlock(&g_res_mu);
}

@interface NvDeferredRelease : NSObject { @public id object; uint64_t fence; int keep_resident; } @end
@implementation NvDeferredRelease @end
static NSMutableArray<NvDeferredRelease *> *g_deferred;
static pthread_mutex_t g_deferred_mu = PTHREAD_MUTEX_INITIALIZER;
// 延迟释放有两种意思，必须分开（进崔凡克首帧的 GPU 内存故障的根）：
//   defer_obj_release = 这个对象退役了：等用过它的提交完成后放掉引用，并把它移出驻留集；
//   defer_obj_unref   = 只是我手里这一份引用不要了，对象本身可能还在用（贴图号的视图缓存里存的可能就是基贴图本身）：只放引用，不动驻留集。
// 原先只有前一种：视图缓存作废时把「仍是某个序号当前存储」的贴图也移出了驻留集。以前作废只发生在基贴图真被换掉时，碰不到；
// 透明链原地合成对调两张贴图的存储后，两张活着的贴图各被这样移出一次，之后每帧照常当附件、被采样——GPU 访问非驻留内存。
// Deferred release has two meanings that must be kept apart (the root of the GPU memory fault on the first frame on entering Travincal):
//   defer_obj_release = this object is retired: drop the reference after the submissions that used it complete, and remove it from the residency set;
//   defer_obj_unref   = only the reference I hold is no longer wanted, the object itself may still be in use (the view cache of a texture ID may hold the base texture itself): only drop the reference, leave the residency set alone.
// Previously only the former existed: when the view cache was invalidated, a texture that 'is still the current storage of some serial' was also removed from the residency set. Invalidation used to happen only when the base texture was really replaced, so it was never hit;
// after the transparency chain's in-place composite swapped the storage of two textures, each of the two live textures was removed once this way, and afterwards they are used as attachments and sampled every frame as usual - the GPU accesses non-resident memory.
static void defer_obj_release_ex(id obj, int keep_resident) {
    if (!obj) return;
    NvDeferredRelease *d = [NvDeferredRelease new]; d->object = obj; d->fence = g_fence + 1; d->keep_resident = keep_resident;
    pthread_mutex_lock(&g_deferred_mu);
    if (!g_deferred) g_deferred = [NSMutableArray array];
    [g_deferred addObject:d];
    pthread_mutex_unlock(&g_deferred_mu);
}
static void defer_obj_release(id obj) { defer_obj_release_ex(obj, 0); }
static void defer_obj_unref(id obj) { defer_obj_release_ex(obj, 1); }
static void defer_drain(void) {
    NSMutableArray<NvDeferredRelease *> *ready = [NSMutableArray array];
    pthread_mutex_lock(&g_deferred_mu);
    for (NSInteger i = g_deferred.count - 1; i >= 0; i--) {
        NvDeferredRelease *d = g_deferred[(NSUInteger)i];
        if (d->fence <= g_done) { [ready addObject:d]; [g_deferred removeObjectAtIndex:(NSUInteger)i]; }
    }
    pthread_mutex_unlock(&g_deferred_mu);
    for (NvDeferredRelease *d in ready) {
        id obj = d->object;
        if (!d->keep_resident && [obj conformsToProtocol:@protocol(MTLResource)]) {
            pthread_mutex_lock(&g_res_mu);
            int registered = [g_res_objects containsObject:obj];
            pthread_mutex_unlock(&g_res_mu);
            if (registered) res_remove((id<MTLResource>)obj);
        }
        d->object = nil;
    }
}

static pthread_mutex_t g_bf_want_mu = PTHREAD_MUTEX_INITIALIZER;
static char **g_bf_want_keys; static size_t g_bf_want_n, g_bf_want_cap; static int g_bf_want_loaded;
void bf_want_note(const char *key) {
    const char *path = getenv("DM_NVN_BF_WANT");
    if (!path || !key || strlen(key) != 16) return;
    for (int i = 0; i < 16; i++) if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f') || (key[i] >= 'A' && key[i] <= 'F'))) return;
    pthread_mutex_lock(&g_bf_want_mu);
    if (!g_bf_want_loaded) {
        FILE *f = fopen(path, "r"); char line[64];
        if (f) { while (fgets(line, sizeof line, f)) { char *e = strpbrk(line, "\r\n"); if (e) *e = 0; if (strlen(line) != 16) continue;
                if (g_bf_want_n == g_bf_want_cap) { g_bf_want_cap = g_bf_want_cap ? g_bf_want_cap * 2 : 64; g_bf_want_keys = realloc(g_bf_want_keys, g_bf_want_cap * sizeof *g_bf_want_keys); }
                g_bf_want_keys[g_bf_want_n++] = strdup(line); } fclose(f); }
        g_bf_want_loaded = 1;
    }
    for (size_t i = 0; i < g_bf_want_n; i++) if (!strcmp(g_bf_want_keys[i], key)) { pthread_mutex_unlock(&g_bf_want_mu); return; }
    if (g_bf_want_n == g_bf_want_cap) { g_bf_want_cap = g_bf_want_cap ? g_bf_want_cap * 2 : 64; g_bf_want_keys = realloc(g_bf_want_keys, g_bf_want_cap * sizeof *g_bf_want_keys); }
    g_bf_want_keys[g_bf_want_n++] = strdup(key);
    FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", key); fclose(f); }
    pthread_mutex_unlock(&g_bf_want_mu);
}

// ---------- 内存池 ----------
// ---------- Memory pools ----------
typedef struct { uintptr_t lo, hi; uintptr_t base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; const void *key; int res; } pool_ent;   // res：已放进驻留集（按需，见 pool_res） | res: already put in the residency set (on demand, see pool_res)
static pool_ent *g_pools; static int g_npool, g_cap;
static NSMutableArray *g_keep;                  // 持有缓冲引用（pool_ent 里是不持有的指针） | Holds the buffer references (pool_ent has non-owning pointers)
static uint64_t g_pool_adds;

#define C1_BANK_SIZE (4u << 20)
#define C1_DECL_SIZE (64u << 10)
static NSMutableArray *g_c1_banks;
static size_t g_c1_bank_off, g_c1_alloc_bytes, g_c1_data_bytes;
static uint64_t g_c1_count, g_c1_exclusive;
static NSMutableSet<NSString *> *g_c1_full;
static int g_c1_full_valid;
static os_unfair_lock g_c1_lock = OS_UNFAIR_LOCK_INIT;

// DM_UDC_SELFTEST=1（诊断，A19 真机核 DontCare 行为）：私有存储 Depth32Float_Stencil8 / RGBA8（1952x900）先清除写入已知值，
// 再 Load + DontCare、不画，读回逐像素比对，打一行结果
// DM_UDC_SELFTEST=1 (diagnostic, checks DontCare behavior on a real A19 device): private-storage Depth32Float_Stencil8 / RGBA8 (1952x900) are first cleared to write known values,
// then Load + DontCare without drawing, read back and compared pixel by pixel, printing one result line
static void udc_selftest(void) {
    if (!getenv("DM_UDC_SELFTEST")) return;
    id<MTLCommandQueue> q = [g_dev newCommandQueue];
    for (int depth = 1; depth >= 0; depth--) {
        NSUInteger w = 1952, h = 900;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        for (int pass = 0; pass < 2; pass++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            MTLLoadAction la = pass ? MTLLoadActionLoad : MTLLoadActionClear; MTLStoreAction sa = pass ? MTLStoreActionDontCare : MTLStoreActionStore;
            if (depth) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = la; rp.depthAttachment.storeAction = sa; rp.depthAttachment.clearDepth = 0.3125;
                         rp.stencilAttachment.texture = t; rp.stencilAttachment.loadAction = la; rp.stencilAttachment.storeAction = sa; rp.stencilAttachment.clearStencil = 0x5a; }
            else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = la; rp.colorAttachments[0].storeAction = sa; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 0.8); }
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        id<MTLBuffer> out = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0
            destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4 options:depth ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *px = out.contents; NSUInteger bad = 0; for (NSUInteger i = 0; i < w * h; i++) if (px[i] != px[0]) bad++;
        MLOG("UDC自检 %s %s %lux%lu 首值 %08x 与首值不同 %lu / %lu", g_dev.name.UTF8String, depth ? "Depth32F_S8" : "RGBA8", (unsigned long)w, (unsigned long)h, px[0], (unsigned long)bad, (unsigned long)(w * h));
    }
}
void mtl_init(void) {
    if (g_dev) return;
    const char *link = getenv("DM_PRESENT_LINK");
    g_dm_present_link = link ? atoi(link) : 1; if (g_dm_present_link < 0 || g_dm_present_link > 2) g_dm_present_link = 1;   // 缺省 1（真机同进程交替：负载超一拍时长帧 15% → 0、帧间隔变匀；稳 60 时与 0 无差别）；DM_PRESENT_LINK=0 关   // 1 = 60 Hz 节拍每拍出图；2 = 120 Hz 节拍、游戏隔拍出一帧（落后时下一个 8.33 ms 拍就能上屏） | Default 1 (real-device same-process alternation: when load exceeds one display-link period, long frames 15% → 0 and frame intervals even out; no difference from 0 at a steady 60); DM_PRESENT_LINK=0 turns it off   // 1 = present on every tick of a 60 Hz display link; 2 = 120 Hz display link, the game presents a frame every other display-link tick (when behind, it can present on the next 8.33 ms display-link tick)
    const char *latency = getenv("DM_PRESENT_LATENCY");
    if (latency && atoi(latency) > 0) g_present_latency = atoi(latency);
    g_res_check_on = getenv("DM_RES_CHECK") != NULL;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) MDIE("没有 Metal 设备");
    if (!g_dev.hasUnifiedMemory) MDIE("需要统一内存设备（零拷贝内存池）");
    g_q = [g_dev newCommandQueue];
#if DM_NVN_M3
    stream_queue_once();
#endif
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
    rd.initialCapacity = 4096;
    NSError *re = nil;
    g_res = [g_dev newResidencySetWithDescriptor:rd error:&re];
    if (!g_res) MDIE("建立 MTLResidencySet 失败");
    [g_q addResidencySet:g_res];
    { void crash_set_wait_dump(void (*fn)(void)); void mtl_wait_state_dump(void); crash_set_wait_dump(mtl_wait_state_dump); }
#if !DM_NVN_M3
    dispatch_queue_attr_t feedback_attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    g_feedback_q = dispatch_queue_create("dm.nvn.mtl4-feedback", feedback_attr);
    MTL4CommandQueueDescriptor *q4d = [MTL4CommandQueueDescriptor new]; q4d.feedbackQueue = g_feedback_q;
    g_q4 = [g_dev newMTL4CommandQueueWithDescriptor:q4d error:&re];
    if (!g_q4) MDIE("建立 MTL4CommandQueue 失败");
    stream_queue_once();
    [g_q4 addResidencySet:g_res];
#endif
    g_alloc_n = 3;
#if !DM_NVN_M3
    for (int i = 0; i < 3; i++) {
        MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        ad.label = [NSString stringWithFormat:@"nvn-mtl allocator %d", i];
        NSError *ae = nil;
        g_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_alloc4[i]) MDIE("建立 MTL4CommandAllocator[%d] 失败：%s", i, ae.localizedDescription.UTF8String);
        g_up_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_up_alloc4[i]) MDIE("建立上传 MTL4CommandAllocator[%d] 失败：%s", i, ae.localizedDescription.UTF8String);
        MTL4CommandAllocatorDescriptor *pad = [MTL4CommandAllocatorDescriptor new];
        pad.label = [NSString stringWithFormat:@"nvn-mtl present allocator %d", i];
        g_present_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:pad error:&ae];
        if (!g_present_alloc4[i]) MDIE("建立出图 MTL4CommandAllocator[%d] 失败：%s", i, ae.localizedDescription.UTF8String);
        g_present_cb[i] = [g_dev newCommandBuffer];
        if (!g_present_cb[i]) MDIE("建立出图 MTL4CommandBuffer[%d] 失败", i);
    }
    MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new];
    atd.maxBufferBindCount = 31; atd.maxTextureBindCount = 1;
    NSError *ate = nil;
    g_vt = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_vt) MDIE("建立 vertex argument table 失败：%s", ate.localizedDescription.UTF8String);
    g_ft = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_ft) MDIE("建立 fragment argument table 失败：%s", ate.localizedDescription.UTF8String);
    g_ct = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_ct) MDIE("建立 compute argument table 失败：%s", ate.localizedDescription.UTF8String);
    g_up_ct = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_up_ct) MDIE("建立上传 compute argument table 失败：%s", ate.localizedDescription.UTF8String);
    MTL4ArgumentTableDescriptor *pad = [MTL4ArgumentTableDescriptor new];
    pad.maxTextureBindCount = 1; pad.maxSamplerStateBindCount = 1; pad.maxBufferBindCount = 1;
    g_present_args = [g_dev newArgumentTableWithDescriptor:pad error:&ate];
    if (!g_present_args) MDIE("建立出图 argument table 失败：%s", ate.localizedDescription.UTF8String);
    g_bloomq_args = [g_dev newArgumentTableWithDescriptor:pad error:&ate];
    if (!g_bloomq_args) MDIE("建立泛光 1/4 argument table 失败：%s", ate.localizedDescription.UTF8String);
#endif
    for (int i = 0; i < 3; i++) {
        g_present_uv[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_present_uv[i]) MDIE("建立出图 UV 缓冲[%d] 失败", i);
        res_add(g_present_uv[i]);
        g_bloomq_const[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_bloomq_const[i]) MDIE("建立泛光 1/4 常量缓冲[%d] 失败", i);
        res_add(g_bloomq_const[i]);
        g_mp_buf[i] = [g_dev newBufferWithLength:MP_SLOTS * 256 options:MTLResourceStorageModeShared];   // 紫闪探针（见 mp_encode） | Magenta probe (see mp_encode)
        if (!g_mp_buf[i]) MDIE("建立紫闪探针缓冲[%d] 失败", i);
        res_add(g_mp_buf[i]);
    }
    g_ev = [g_dev newSharedEvent];
    g_keep = [NSMutableArray array];
    g_nvn_deswz_gpu = nvn_deswz_gpu_init(g_dev) == 0;
    dm_res_report_register(mtl_res_counts);
    if (!g_nvn_deswz_gpu) MLOG("【红】显卡解排布初始化失败，预排布贴图回退 CPU");
    MLOG("Metal 设备 %s", g_dev.name.UTF8String);
    udc_selftest();
}

void dm_mtl_l3_tick(uint32_t tick) {
    static int leak = -1;
    if (leak < 0) leak = getenv("DM_TEST_LEAK_GPU") != NULL;
    if (leak && tick % 100 == 0) {
        if (!g_l3_gpu_leaks) g_l3_gpu_leaks = [NSMutableArray array];
        id<MTLBuffer> leak = [g_dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
        if (leak) [g_l3_gpu_leaks addObject:leak];
    }
}

uint64_t dm_mtl_l3_sample(uint32_t tick) {
    (void)tick;
    return g_dev ? g_dev.currentAllocatedSize : 0;
}

static _Atomic uint64_t g_reg_gen = 1;   // 内存池 / 贴图 / 采样器登记的变更代号：参数缓冲复用以它为失效条件之一 | Change generation of memory pool / texture / sampler registration: one of the invalidation conditions for argument buffer reuse
// 内存池按需驻留：零拷贝缓冲放进驻留集，iOS 会把整个池读进内存并钉成 wired（不计入 footprint，也不能被换出），游戏从没用过的池也一样——
// 真机 17 Pro Max 实测 512 MB 的渲染目标池一局取址 0 次，放进驻留集整机 wired 高约 530 MB；A14 4 GB 机因此启动几秒就被系统按整机缺页结束（公开仓库 #37）。
// 所以池建好时不放进驻留集，GPU 第一次经 mtl_addr / mtl_gpu_addr 取到它的地址时才放进去，并当场提交驻留集
// （GPU 拿池地址只有这两个口子；当场提交 = 早于用到它的任何命令缓冲提交，不依赖各提交口自己调 res_commit）。调用方持 g_mu
// On-demand residency of memory pools: putting a zero-copy buffer into the residency set makes iOS read the whole pool into memory and pin it as wired (not counted in footprint and never paged out), even a pool the game never uses -
// measured on a 17 Pro Max: the 512 MB render-target pool is addressed 0 times in a run, and putting it into the residency set raises device wired memory by about 530 MB; this is why an A14 4 GB device was ended by the system for running out of memory within seconds of launch (public repository #37).
// So a pool is not put into the residency set when created; it is put in, and the residency set committed on the spot, the first time the GPU gets its address through mtl_addr / mtl_gpu_addr
// (these are the only two ways the GPU gets pool addresses; committing on the spot = earlier than any command buffer commit that uses it, without relying on each commit point calling res_commit). Caller holds g_mu
static uint64_t g_cb_commits, g_pool_res_at;   // 已提交的命令缓冲个数（主提交 + 上传）；最近一次按需驻留时的这个数（0 = 没有待核的） | number of command buffers committed (main + upload); this number at the last on-demand residency (0 = nothing pending to check)
static void pool_res(int i) {
    if (g_pools[i].res) return;
    res_add(g_pools[i].buf); res_commit(); g_pools[i].res = 1; g_pool_res_at = g_cb_commits + 1;
    MLOG("内存池按需驻留：%llu MB 的池第一次被取址，已放进驻留集并提交（此时已提交命令缓冲 %llu 个）",
         (unsigned long long)((g_pools[i].hi - g_pools[i].lo) >> 20), (unsigned long long)g_cb_commits);
}
static void cb_committed(void) {   // 主提交 / 上传提交之后调：核「驻留集提交早于用到它的第一个命令缓冲提交」，每次按需驻留后记一行 | called after the main submission / upload submission: checks that "the residency set commit is earlier than the first command buffer commit that uses it", one line after each on-demand residency
    pthread_mutex_lock(&g_mu); uint64_t n = ++g_cb_commits, at = g_pool_res_at; if (at) g_pool_res_at = 0; pthread_mutex_unlock(&g_mu);
    if (at) MLOG("内存池按需驻留：其后第一个命令缓冲提交是第 %llu 个（驻留集在第 %llu 个之前已提交）", (unsigned long long)n, (unsigned long long)at);
}
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init(); g_reg_gen++;
    uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)mem, hi = lo + size;
    uintptr_t a = lo & ~(pg - 1), b = (hi + pg - 1) & ~(pg - 1);
    id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
    if (!buf) MDIE("内存池 %p+%#llx 建零拷贝缓冲失败", mem, (unsigned long long)size);
    pthread_mutex_lock(&g_mu);
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = realloc(g_pools, g_cap * sizeof *g_pools); }
    g_pools[g_npool++] = (pool_ent){lo, hi, a, buf, buf.gpuAddress, mem, 0};   // 先不进驻留集：第一次被取址时才放进去（pool_res） | not in the residency set yet: put in the first time it is addressed (pool_res)
    g_pool_adds++;
    [g_keep addObject:buf];
    pthread_mutex_unlock(&g_mu);
}

void mtl_pool_del(const void *mem) {
    g_reg_gen++;
    id<MTLBuffer> gone = nil;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++)
        if (g_pools[i].key == mem) { gone = g_pools[i].buf; [g_keep removeObject:gone]; g_pools[i] = g_pools[--g_npool]; break; }
    pthread_mutex_unlock(&g_mu);
    defer_obj_release(gone);   // 退役：等用过它的提交完成后放掉，并移出驻留集（原先只从 g_keep 摘掉——驻留集强持有它，零拷贝缓冲对象永远不销毁） | retire: release after the submissions that used it complete, and remove from the residency set (it used to be only removed from g_keep - the residency set holds it strongly, so the zero-copy buffer object was never destroyed)
}

int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++)
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
            pool_res(i); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; ok = 1; break;
        }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) {
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_npool; i++)
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
            pool_res(i); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; *gpu = g_pools[i].gpu + *off; ok = 1; break;
        }
    pthread_mutex_unlock(&g_mu);
    return ok;
}

// ---------- 提交 ----------
// ---------- Submission ----------
static void defer_reset(void); static void defer_fallback(void);
static void pg_commit(id fb);
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; void *vis; } post_op;   // vis：kind 3 的可见性槽表 | vis: the visibility slot table for kind 3
// ---------- 遮挡查询（NVN_COUNTER_TYPE_SAMPLES_PASSED）----------
// NVN 语义：通过采样数是队列上的累计计数器——ResetCounter 清零，此后每个绘制累加，ReportCounter 在 GPU 完成时写出当时的累计值（+ 时间戳）。
// Metal：每个渲染通道挂可见性结果缓冲；计数期间的绘制用计数模式，每个通道一个新槽（不依赖跨通道累加）；清零时清空「自清零以来的槽表」，
// 报告时把槽表连同所在缓冲（持有）交给完成回调求和写回；报告之后换新槽，后续绘制不计入已报告的值。
// （原先一律写 0：遮挡查询 = 「完全被挡」，篝火 / 光源被剔——选角页每帧 ResetCounter 21 次）
// ---------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------
// NVN semantics: samples passed is a cumulative counter on the queue - ResetCounter zeroes it, every draw afterwards adds to it, and ReportCounter writes out the cumulative value at that moment (+ timestamp) when the GPU completes.
// Metal: every render pass attaches a visibility result buffer; draws during counting use counting mode, with a new slot per pass (no reliance on accumulation across passes); a reset empties the 'slot table since the reset',
// and a report hands the slot table together with its buffer (retained) to the completion callback to sum and write back; after a report a new slot is used, so later draws are not counted into the reported value.
// (Previously always wrote 0: occlusion query = 'fully occluded', campfires / light sources got culled - the character select screen calls ResetCounter 21 times per frame)
typedef struct { void *buf; uint32_t slot; } vis_slot;
typedef struct { int n; vis_slot s[]; } vis_list;
#define VIS_SLOTS 8192
typedef struct { __strong id<MTLBuffer> buf; uint64_t fence; } vis_buf;
static vis_buf g_vis_ring[4]; static int g_vis_ring_n, g_vis_ring_i = -1;
static id<MTLBuffer> g_vis; static uint32_t g_vis_n;   // 本次提交的可见性缓冲与已用槽数 | This submission's visibility buffer and number of slots used
static int vis_in_ring(id<MTLBuffer> b) { for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].buf == b) return 1; return 0; }
static int g_vis_on, g_vis_counting, g_vis_cur = -1; static uint64_t g_vis_cur_pass;
static vis_slot *g_vs; static int g_nvs, g_vscap;      // 自上次清零以来的槽 | Slots since the last reset
static uint64_t g_cnt_report[32], g_cnt_reset[32];       // 各计数器类型的报告 / 清零次数（诊断） | Report / reset counts per counter type (diagnostics)
static void vis_clear(void) { for (int i = 0; i < g_nvs; i++) CFRelease(g_vs[i].buf); g_nvs = 0; g_vis_cur = -1; }
static vis_list *vis_snapshot(void) {
    vis_list *l = malloc(sizeof *l + g_nvs * sizeof(vis_slot)); l->n = g_nvs;
    for (int i = 0; i < g_nvs; i++) { l->s[i] = g_vs[i]; CFRetain(g_vs[i].buf); }
    return l;
}
// 本次提交里已编码、尚未在 GPU 上执行的缓冲写（拷贝 / uniform 内联更新）的目标区间：编码期在 CPU 侧读这些地址读到的是旧值（uv_proof 读 uniform 时避开）
// Target ranges of buffer writes (copies / inline uniform updates) already encoded in this submission but not yet executed on the GPU: reading these addresses on the CPU side at encode time yields old values (uv_proof avoids them when reading uniforms)
#define PEND_MAX 256
static uint64_t g_pend_lo[PEND_MAX], g_pend_hi[PEND_MAX]; static int g_npend, g_pend_over;
static int pend_hits(uint64_t a, uint64_t n) { if (g_pend_over) return 1; for (int i = 0; i < g_npend; i++) if (a < g_pend_hi[i] && a + n > g_pend_lo[i]) return 1; return 0; }
typedef struct { uint64_t ser; uint16_t level, slice; } zskip_zero;
static zskip_zero g_zskip_zero[4096]; static int g_zskip_n, g_zskip_count;
static zskip_zero g_zskip_cand[8]; static uint8_t g_zskip_cand_ok[8];
static void zskip_clear(void);
static int tbdr_on(void);
static id g_cb;
static BENC g_blit;
static NSMutableArray<dispatch_block_t> *g_commit_blocks;
static uint64_t g_alloc_fence[ALLOC_MAX]; static uint64_t g_alloc_waits, g_alloc_grown;
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static void up_flush(void);
// 上传批只属于提交线程（发起 mtl_sub_begin 的线程）：批的编码器不能跨线程用。每个入口断言当前线程 == 提交线程，
// 不符当场打栈中止——让「别的线程碰批」在 Mac 上也确定性暴露，不靠时序（iOS 真机卡主菜单：出图线程经 q_cb 冲刷了提交线程的批）。
// The upload batch belongs only to the submit thread (the thread that called mtl_sub_begin): the batch's encoder cannot be used across threads. Every entry point asserts current thread == submit thread,
// and on mismatch prints the stack and aborts on the spot - so that 'another thread touched the batch' is exposed deterministically on Mac too, not by timing (real iOS device stuck at the main menu: the present thread flushed the submit thread's batch via q_cb).
static pthread_t g_sub_thread; static _Atomic int g_sub_thread_set;
static int on_sub_thread(void) { return atomic_load(&g_sub_thread_set) && pthread_equal(pthread_self(), g_sub_thread); }
static void up_owner_check(const char *where) {
    if (!atomic_load(&g_sub_thread_set) || on_sub_thread()) return;
    char name[64] = ""; pthread_getname_np(pthread_self(), name, sizeof name);
    MLOG("【红】上传批只属于提交线程：%s 在线程「%s」上被调用", where, name);
    void *bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2);
    abort();
}
// 回放 / 上传批之外另建命令缓冲一律经它。提交线程上：先冲刷本批上传，保证随后读写这些贴图的命令排在上传之后；
// 别的线程（出图线程等）另建命令缓冲不碰批：它们不读写本批上传的贴图；需要这些贴图的命令都在本次回放提交之后（批在其前冲刷）
// 旧队列命令缓冲（读回 / 诊断 / 上屏，M4-4、M4-5 迁走后删）：上传已在 Metal 4 队列，两队列之间没有先后保证——
// 冲刷上传后等它完成，旧队列才读得到上传结果（mtl_read_texels 先等后冲刷，读回抢在上传之前，t_nvn 上传内容 4 红）
// Every command buffer created outside the replay / upload batch goes through this. On the submit thread: flush this batch's uploads first, so that commands that then read or write these textures are ordered after the uploads;
// other threads (the present thread etc.) creating a command buffer do not touch the batch: they do not read or write textures uploaded in this batch; commands that need these textures all come after this replay submission (the batch is flushed before it)
// Old-queue command buffers (readback / diagnostics / present; to be deleted once M4-4 and M4-5 are migrated away): uploads are already on the Metal 4 queue, and there is no ordering guarantee between the two queues -
// wait for the upload to complete after flushing it, and only then can the old queue read the upload result (mtl_read_texels waited first and flushed afterwards, the readback beat the upload, 4 failures on t_nvn upload content)
static id<MTLCommandBuffer> q_cb(void) { if (on_sub_thread()) { up_flush(); while (atomic_load(&g_up_pending)) usleep(200); } return [g_q commandBuffer]; }
static post_op *g_post; static int g_npost, g_postcap;
// 每次提交一个自动释放池：提交跑在客体线程上（长驻、无运行循环、从不排空隐式池），Metal 回放里自动释放的对象
// （命令缓冲、编码器、通道描述等）不放池就永远不释放。iOS 真机选角页实测宿主堆每帧涨约 47 个对象、0.22 MiB/s。
// One autorelease pool per submission: submissions run on a guest thread (long-lived, no run loop, never drains the implicit pool), so objects autoreleased in the Metal replay
// (command buffers, encoders, pass descriptors, etc.) are never released unless put in a pool. Measured on the character select screen of a real iOS device: the host heap grows by about 47 objects per frame, 0.22 MiB/s.
void *objc_autoreleasePoolPush(void); void objc_autoreleasePoolPop(void *);
static void *g_sub_pool;
static int g_ruler_truth = -1, g_ruler_pending;
static int tbdr_on(void);

static uint64_t g_sub_serial;   // 第几次提交（并行预编「每次提交只预测一次」用） | Submission index (used by the parallel precompile's 'predict only once per submission')
static void tbdr_latch(void);
void mtl_sub_begin(void) {
    mtl_init(); g_sub_serial++;
    tbdr_latch();
    if (tbdr_on() == 0 && g_ruler_pending) { g_ruler_truth = 0; g_ruler_pending = 0; MLOG("DM_NVN_RULER：切到被测模式"); }
    defer_drain();
    defer_reset();
    warm_claim();
    if (g_cb) MDIE("提交重入");
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    atomic_store(&g_last_commit_error, 0);
    g_sub_pool = objc_autoreleasePoolPush();
#if DM_NVN_M3
    g_cb = [g_q commandBuffer];
#else
    g_cb = [g_dev newCommandBuffer];
#endif
    // 不在帧中间等 GPU（真机热态：固定 3 个轮转时 GPU 跟不上就每帧在这里阻塞，提交线程睡醒后按低频跑，指令数 −18% 而 CPU 时间 +49%）：
    // 挑一个 GPU 已用完的分配器；都在用就新建；到上限才等最老的那个。超前量由游戏取交换链贴图处的等待约束，与旧接口相同。
    // Do not wait for the GPU mid-frame (hot real device: with a fixed rotation of 3, when the GPU cannot keep up every frame blocks here, and the submit thread runs at low frequency after waking, instruction count −18% but CPU time +49%):
    // pick an allocator the GPU has finished with; if all are in use create a new one; only at the cap wait for the oldest one. How far ahead it runs is bounded by the wait where the game acquires the swapchain texture, same as the old interface.
    int alloc_i = -1; uint64_t done = g_done;
    for (int i = 0; i < g_alloc_n; i++) if (g_alloc_fence[i] <= done) { alloc_i = i; break; }
#if DM_NVN_M3
    if (alloc_i < 0 && g_alloc_n < ALLOC_MAX) { alloc_i = g_alloc_n++; g_alloc_grown++; }
#else
    if (alloc_i < 0 && g_alloc_n < ALLOC_MAX) { NSError *ae = nil; MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        g_alloc4[g_alloc_n] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae]; if (!g_alloc4[g_alloc_n]) MDIE("新建 MTL4CommandAllocator 失败：%s", ae.localizedDescription.UTF8String);
        alloc_i = g_alloc_n++; g_alloc_grown++; }
#endif
    if (alloc_i < 0) { alloc_i = 0; for (int i = 1; i < g_alloc_n; i++) if (g_alloc_fence[i] < g_alloc_fence[alloc_i]) alloc_i = i; g_alloc_waits++; mtl_wait(g_alloc_fence[alloc_i], UINT64_MAX); }
    g_alloc_cur = alloc_i;
#if !DM_NVN_M3
    [g_alloc4[alloc_i] reset];
    [g_cb beginCommandBufferWithAllocator:g_alloc4[alloc_i]];
#endif
    g_commit_blocks = [NSMutableArray array];
    g_npost = 0; g_vis = nil; g_vis_n = 0; g_vis_cur = -1; g_npend = 0; g_pend_over = 0;
    zskip_clear(); g_zskip_count = 0;
    int pick = -1;
    for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].fence <= g_done) { pick = i; break; }
    if (pick < 0 && g_vis_ring_n < 4) pick = g_vis_ring_n++;
    if (pick < 0) { uint64_t oldest = UINT64_MAX; for (int i = 0; i < 4; i++) if (g_vis_ring[i].fence < oldest) { oldest = g_vis_ring[i].fence; pick = i; } if (oldest > g_done) mtl_wait(oldest, UINT64_MAX); }
    g_vis_ring_i = pick; g_vis_ring[pick].fence = UINT64_MAX;
    if (!g_vis_ring[pick].buf) { g_vis_ring[pick].buf = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis_ring[pick].buf); }
    if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis);   // 上次提交留下的溢出缓冲（见 g_vis_n >= VIS_SLOTS 那一处）：退役，否则它一直留在驻留集里 | the overflow buffer left by the previous submission (see the place with g_vis_n >= VIS_SLOTS): retire it, otherwise it stays in the residency set forever
    g_vis = g_vis_ring[pick].buf;
    void pg_sub_begin_ext(uint64_t fence); pg_sub_begin_ext(g_fence + 1);
    void arena_begin(void); arena_begin();
}

static void rp_end(void);
extern int g_frz_on; static int g_frz_state;   // 世界冻结：本帧锁存的状态（0 正常 / 1 完整帧 / 2 冻结帧），见 frz_op 上方的说明 | World freeze: the state latched for this frame (0 normal / 1 full frame / 2 frozen frame), see the note above frz_op
// DM_NVN_PASSGPU_SERIAL=1（测量用）：主命令缓冲上每建一个编码器前编「发信号 n+1 → 等信号 n+1」——信号要等此前全部工作完成才发出，
// 后面的编码器因此等前一个彻底跑完才开始；TBDR 上通道不再流水重叠，DM_NVN_PASSGPU 的逐通道起止跨度即该通道独占耗时（整帧总时长偏大，只看相对开销）
// DM_NVN_PASSGPU_SERIAL=1 (for measurement): before creating each encoder on the main command buffer, encode 'signal n+1 → wait for signal n+1' - the signal is only raised once all prior work has completed,
// so the following encoder starts only after the previous one has fully finished; on TBDR passes no longer overlap in the pipeline, and the per-pass start-to-end span of DM_NVN_PASSGPU is that pass's exclusive time (the whole-frame total is inflated; look only at relative cost)
static void ser_gate(void) {
    static int on = -1; static id<MTLEvent> ev; static uint64_t n;
    if (on < 0) { on = getenv("DM_NVN_PASSGPU_SERIAL") != NULL; if (on) { ev = [g_dev newEvent]; MLOG("DM_NVN_PASSGPU_SERIAL：通道串行化（测量用，耗时偏大）"); } }
    if (!on) return;
    (void)ev; (void)n;
}
// Metal 4 保守屏障（第一版）：编码器开头一道消费者屏障（之前全部工作 → 本编码器），结束处一道生产者屏障（本编码器及之前 → 后续编码器）。
// 只放开头不够：t_rt_range「通道 1 → 通道 2 → 拷贝」读回黑色，补上结束处屏障后 19/19。旧队列的编码器（上传 / 诊断 / 上屏）不走这里的屏障。
// Metal 4 conservative barriers (first version): a consumer barrier at the start of an encoder (all prior work → this encoder) and a producer barrier at the end (this encoder and earlier → later encoders).
// Only the one at the start is not enough: t_rt_range 'pass 1 → pass 2 → copy' read back black, and after adding the end barrier it is 19/19. Old-queue encoders (upload / diagnostics / present) do not go through these barriers.
#if DM_NVN_M3
static inline void m4_end(id e) { [e endEncoding]; }   // 旧接口没有显式屏障：依赖关系由 useResource 声明 + 系统自动跟踪 | the older API has no explicit barriers: dependencies come from useResource declarations plus the system's automatic tracking
static inline void m3_rbuf(id<MTLRenderCommandEncoder> e, int vertex, NSUInteger i, uint64_t gpu) {
    NSUInteger off; id<MTLBuffer> b = m3_buf(gpu, &off); if (gpu && !b) { { static int n_; if (!n_++) MLOG("【红】" "旧接口绑定：显卡地址 %#llx 不在任何登记过的缓冲里（槽 %lu）", (unsigned long long)gpu, (unsigned long)i); } return; }
    if (vertex) [e setVertexBuffer:b offset:off atIndex:i]; else [e setFragmentBuffer:b offset:off atIndex:i];
}
#else
static inline void m4_end(id e) {
    if ([e conformsToProtocol:@protocol(MTL4CommandEncoder)]) [(id<MTL4CommandEncoder>)e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [e endEncoding];
}
#endif
#if DM_NVN_M3
static inline RENC enc_r_begin(RPD *d) { return [g_cb renderCommandEncoderWithDescriptor:d]; }
#else
static inline RENC enc_r_begin(RPD *d) { RENC e = [g_cb renderCommandEncoderWithDescriptor:d]; [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; [e setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [e setArgumentTable:g_ft atStages:MTLRenderStageFragment]; return e; }
#endif
static inline void enc_r_end(id e) { m4_end(e); }
static inline void enc_r_pso(RENC e, id<MTLRenderPipelineState> p) { [e setRenderPipelineState:p]; }
static inline void enc_r_dss(RENC e, id<MTLDepthStencilState> s) { [e setDepthStencilState:s]; }
static inline void enc_r_viewport(RENC e, MTLViewport v) { [e setViewport:v]; }
static inline void enc_r_scissor(RENC e, MTLScissorRect r) { [e setScissorRect:r]; }
static inline void enc_r_cull(RENC e, MTLCullMode m) { [e setCullMode:m]; }
static inline void enc_r_winding(RENC e, MTLWinding w) { [e setFrontFacingWinding:w]; }
static inline void enc_r_fill(RENC e, MTLTriangleFillMode m) { [e setTriangleFillMode:m]; }
static inline void enc_r_depth_bias(RENC e, float u, float s, float c) { [e setDepthBias:u slopeScale:s clamp:c]; }
static inline void enc_r_depth_clip(RENC e, BOOL v) { [e setDepthClipMode:v ? MTLDepthClipModeClip : MTLDepthClipModeClamp]; }
static inline void enc_r_stencil_ref(RENC e, uint32_t f, uint32_t b) { (void)b; [e setStencilReferenceValue:f]; }
static inline void enc_r_blend_color(RENC e, float r, float g, float b, float a) { [e setBlendColorRed:r green:g blue:b alpha:a]; }
static inline void enc_r_visibility(RENC e, MTLVisibilityResultMode m, NSUInteger o) { [e setVisibilityResultMode:m offset:o]; }
#if DM_NVN_M3
static inline void enc_r_vbuf(RENC e, NSUInteger i, uint64_t gpu) { m3_rbuf(e, 1, i, gpu); }
static inline void enc_r_args(RENC e, MTLRenderStages s, uint64_t gpu, NSUInteger i) { m3_rbuf(e, s == MTLRenderStageVertex, i, gpu); }
#else
static inline void enc_r_vbuf(RENC e, NSUInteger i, uint64_t gpu) { (void)e; [g_vt setAddress:gpu atIndex:i]; }
static inline void enc_r_args(RENC e, MTLRenderStages s, uint64_t gpu, NSUInteger i) { (void)e; [(s == MTLRenderStageVertex ? g_vt : g_ft) setAddress:gpu atIndex:i]; }
#endif
static inline void enc_r_draw(RENC e, MTLPrimitiveType p, NSUInteger v, NSUInteger n) { [e drawPrimitives:p vertexStart:v vertexCount:n]; }
static inline void enc_r_draw_instanced(RENC e, MTLPrimitiveType p, NSUInteger v, NSUInteger n, NSUInteger i, NSUInteger b) { [e drawPrimitives:p vertexStart:v vertexCount:n instanceCount:i baseInstance:b]; }
#if DM_NVN_M3
static inline void enc_r_draw_indexed(RENC e, MTLPrimitiveType p, NSUInteger n, MTLIndexType t, uint64_t gpu, NSUInteger len, NSUInteger i, NSInteger bv, NSUInteger bi) { (void)len; if (!n) return; NSUInteger off; id<MTLBuffer> b = m3_buf(gpu, &off); if (!b) { { static int n_; if (!n_++) MLOG("【红】" "旧接口绘制：索引缓冲地址 %#llx 不在任何登记过的缓冲里", (unsigned long long)gpu); } return; } [e drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:b indexBufferOffset:off instanceCount:i baseVertex:bv baseInstance:bi]; }
#else
static inline void enc_r_draw_indexed(RENC e, MTLPrimitiveType p, NSUInteger n, MTLIndexType t, uint64_t gpu, NSUInteger len, NSUInteger i, NSInteger bv, NSUInteger bi) { [e drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:gpu indexBufferLength:len instanceCount:i baseVertex:bv baseInstance:bi]; }
#endif
#if DM_NVN_M3
static inline CENC enc_c_begin(MTLComputePassDescriptor *d) { return d ? [g_cb computeCommandEncoderWithDescriptor:d] : [g_cb computeCommandEncoder]; }
#else
static inline CENC enc_c_begin(MTLComputePassDescriptor *d) { (void)d; CENC e = [g_cb computeCommandEncoder]; [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; [e setArgumentTable:g_ct]; return e; }
#endif
static inline id<MTLComputeCommandEncoder> enc_c_begin_concurrent(id<MTLCommandBuffer> cb) { return [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent]; }
static inline void enc_c_end(id e) { m4_end(e); }
static inline void enc_c_pso(id e, id<MTLComputePipelineState> p) { [e setComputePipelineState:p]; }
#if DM_NVN_M3
static inline void enc_c_args(id e, uint64_t gpu, NSUInteger i) { NSUInteger off; id<MTLBuffer> b = m3_buf(gpu, &off); if (gpu && !b) { { static int n_; if (!n_++) MLOG("【红】" "旧接口计算绑定：显卡地址 %#llx 不在任何登记过的缓冲里（槽 %lu）", (unsigned long long)gpu, (unsigned long)i); } return; } [(id<MTLComputeCommandEncoder>)e setBuffer:b offset:off atIndex:i]; }
#else
static inline void enc_c_args(id e, uint64_t gpu, NSUInteger i) { (void)e; [g_ct setAddress:gpu atIndex:i]; }
#endif
static inline void enc_c_dispatch(id e, MTLSize n, MTLSize t) { [e dispatchThreadgroups:n threadsPerThreadgroup:t]; }
#if DM_NVN_M3
static inline id enc_b_begin(id cb) { return [cb blitCommandEncoder]; }
#else
static inline id enc_b_begin(id cb) { return [cb respondsToSelector:@selector(beginCommandBufferWithAllocator:)] ? [cb computeCommandEncoder] : [cb blitCommandEncoder]; }
#endif
static inline void enc_b_end(id e) { m4_end(e); }
static inline void enc_b_copy_buffer(id e, id<MTLBuffer> s, NSUInteger so, id<MTLBuffer> d, NSUInteger doff, NSUInteger n) { [e copyFromBuffer:s sourceOffset:so toBuffer:d destinationOffset:doff size:n]; }
static inline void enc_b_copy_buffer_texture(id e, id<MTLBuffer> s, NSUInteger so, NSUInteger row, NSUInteger img, MTLSize z, id<MTLTexture> d, NSUInteger sl, NSUInteger lv, MTLOrigin o) { [e copyFromBuffer:s sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:z toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o]; }
static inline void enc_b_copy_texture_buffer(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLSize z, id<MTLBuffer> d, NSUInteger doff, NSUInteger row, NSUInteger img) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:o sourceSize:z toBuffer:d destinationOffset:doff destinationBytesPerRow:row destinationBytesPerImage:img]; }
static inline void enc_b_copy_texture(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin so, MTLSize z, id<MTLTexture> d, NSUInteger dl, NSUInteger dv, MTLOrigin doo) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:so sourceSize:z toTexture:d destinationSlice:dl destinationLevel:dv destinationOrigin:doo]; }
static void blit_note(void);   // 逐通道转储：拷贝 / 缓冲更新结束了渲染通道（定义在 PD 之后） | Per-pass dump: a copy / buffer update ended the render pass (defined after PD)
extern int g_blit_bar;
#if DM_NVN_M3
static BENC blit(void) { blit_note(); rp_end(); if (!g_blit) { ser_gate(); g_blit = enc_b_begin(g_cb); } return g_blit; }
#else
// 复用同一个拷贝编码器时，前后两次拷贝之间也要一道屏障：Metal 4 同一编码器内的命令不保证按录制顺序执行，
// 先写后读 / 同区间两次写会乱序（t_nvn 5c 未加时 3 次里 1 次读到旧值）。建编码器时那道屏障只管它与之前的编码器
// When reusing the same blit encoder, a barrier is also needed between two copies: Metal 4 does not guarantee commands within one encoder run in recording order,
// so write-then-read / two writes to the same range can be reordered (without it, t_nvn 5c read a stale value 1 time in 3). The barrier at encoder creation only covers it against earlier encoders
static BENC blit(void) { blit_note(); rp_end();
    if (!g_blit) { ser_gate(); g_blit = enc_b_begin(g_cb); [g_blit barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; }
    else if (g_blit_bar) [g_blit barrierAfterEncoderStages:MTLStageBlit | MTLStageDispatch beforeEncoderStages:MTLStageBlit | MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];   // 拷贝编码器在 Metal 4 下是计算编码器，两种阶段都挡 | With Metal 4 the copy encoder is a compute encoder, so both stages are blocked
    return g_blit; }
#endif

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (g_npend < PEND_MAX) { g_pend_lo[g_npend] = dst; g_pend_hi[g_npend++] = dst + n; } else g_pend_over = 1;
    void *sb, *db; uint64_t so, dof;
    if (!mtl_addr(src, n, &sb, &so) || !mtl_addr(dst, n, &db, &dof))
        MDIE("缓冲拷贝地址不在任何内存池内 src=%#llx dst=%#llx n=%#llx", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    enc_b_copy_buffer(blit(), (__bridge id<MTLBuffer>)sb, so, (__bridge id<MTLBuffer>)db, dof, n);
}

static void post(post_op o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? g_postcap * 2 : 64; g_post = realloc(g_post, g_postcap * sizeof *g_post); }
    g_post[g_npost++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, NULL}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type, NULL}); }
void mtl_counter_stats(uint64_t rep[32], uint64_t rst[32]) { memcpy(rep, g_cnt_report, sizeof g_cnt_report); memcpy(rst, g_cnt_reset, sizeof g_cnt_reset); }

static uint64_t gpu_ticks(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return ((uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec) * 12 / 625; }

// ---------- 启动埋点（DM_BOOTPROF=1）：metallib 载入、管线创建（均在提交线程回放中同步发生）、贴图上传、首帧出图 ----------
// ---------- Boot instrumentation (DM_BOOTPROF=1): metallib loading, pipeline creation (both happen synchronously during replay on the submit thread), texture upload, first-frame present ----------
static uint64_t bp_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static uint64_t g_bp_t0;   // 进程启动（本模块装入）时刻 | Time of process start (when this module is loaded)
__attribute__((constructor)) static void bp_t0(void) { g_bp_t0 = bp_now(); }
static int bootprof(void) { static int v = -1; if (v < 0) v = getenv("DM_BOOTPROF") != NULL; return v; }
static uint64_t g_bp_lib_n, g_bp_lib_ns, g_bp_pso_n, g_bp_pso_ns, g_bp_up_bytes, g_bp_first_present;
static uint64_t g_up_total_ns;   // 上传累计耗时（与逐帧 @upload_ms 同源） | Cumulative upload time (same source as the per-frame @upload_ms)
// ---------- 按帧归集 GPU 时间 ----------
// ---------- Per-frame GPU time aggregation ----------
#define GRING 1024
static struct { _Atomic uint64_t frame, ns, t0, t1; _Atomic int pending; } g_gpu[GRING];   // t0 / t1：该帧最早 GPUStart / 最晚 GPUEnd（mach 绝对时间 ns） | t0 / t1: the frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns)
static _Atomic uint64_t g_cur_frame;
// ---------- 同进程交替测量（DM_NVN_UB，缺省无：不设环境变量时下面只多一次「是否启用」的判断） ----------
// 真机热态下系统持续调功率 / 频率，分段对比（A 段、B 段各跑一分钟）分不清 <10% 的差；在同一局里按相轮流开关、相邻相配对取差，漂移对消。
// DM_NVN_UB="名字=项,项;名字=项"，每 DM_NVN_UB_N 帧（缺省 180，下限 60）一相：偶数相 = 基准（什么都不改），奇数相轮流取一类。一类里的每一项：
//   @set:<全局变量名>=<值>   进相时把这个 int 全局变量设成该值、出相时还原（按符号名找地址；真机上要用的开关须在下面的登记表里加一行）
//   <片元着色器键前缀> 或 *   该相丢掉这些片元着色器的全部绘制（画面会错，只量「这类绘制值多少」；近乎空载的类别要单独成轮）
// 读数：每相丢掉前 45 帧（过渡 + 上屏滞后），其余帧的实际上屏间隔（与 perf.txt 同源）均值与 16.7 / 25 / ≥33 ms 三档帧数，日志一行「UB 上屏」。
// 出表：$DM_ROOT/run/ruler/ub_table.py；真机脚本：$DM_ROOT/run/ruler/ub_alt.sh。
// ---------- Same-process alternating measurement (DM_NVN_UB, off by default: without the environment variable the code below only adds one 'is it enabled' check) ----------
// On a hot real device the system keeps adjusting power / frequency, and segment comparison (segments A and B each run for a minute) cannot resolve differences of <10%; within the same run, toggle by phase in turn and take differences of adjacent phase pairs, so the drift cancels.
// DM_NVN_UB="name=item,item;name=item", one phase every DM_NVN_UB_N frames (default 180, minimum 60): even phases = baseline (nothing changed), odd phases take one category in turn. Each item of a category:
//   @set:<global variable name>=<value>   on entering the phase set this int global variable to the value, on leaving restore it (address found by symbol name; switches to be used on a real device must have a line added to the registration table below)
//   <fragment shader key prefix> or *   the phase drops all draws of these fragment shaders (the picture will be wrong; it only measures 'how much this kind of draw is worth'; a nearly idle category needs a round of its own)
// Readings: each phase discards its first 45 frames (transition + present lag); for the remaining frames, the mean of the actual present interval (same source as perf.txt) and the frame counts in the three buckets 16.7 / 25 / ≥33 ms, one log line 「UB 上屏」(UB present).
// Table output: $DM_ROOT/run/ruler/ub_table.py; real-device script: $DM_ROOT/run/ruler/ub_alt.sh.
static int g_ub_n = -1, g_ub_N = 180; static _Atomic int g_ub_cur = -1; static char g_ub_name[16][16], g_ub_keys[16][16][48]; static int g_ub_nk[16];
static _Atomic int g_ubp_on, g_ubp_c[4]; static _Atomic uint64_t g_ubp_sum_us;
uint64_t (*g_ub_sub_cpu_ns)(void), (*g_ub_sub_cyc)(void);   // hle_nvn.c 登记：翻译线程提交的累计 CPU 纳秒 / 周期（没登记时按 0） | Registered by hle_nvn.c: cumulative CPU nanoseconds / cycles of submissions from the translation thread (0 when not registered)
static uint64_t ub_cpu(void) { return g_ub_sub_cpu_ns ? g_ub_sub_cpu_ns() : 0; }
static uint64_t ub_cyc(void) { return g_ub_sub_cyc ? g_ub_sub_cyc() : 0; }
static uint64_t g_ubx_cpu0, g_ubx_cyc0, g_ubx_f0;   // 本相计数起点（第 45 帧）：按相给出翻译线程每帧 CPU 与周期 | Counter start of this phase (frame 45): gives per-frame CPU and cycles of the translation thread per phase
void mtl_ub_present(double iv) {   // 出图线程每帧调一次：实际上屏间隔 ms（−1 = 被丢弃，0 = 无读数） | Called once per frame by the present thread: actual present interval in ms (−1 = dropped, 0 = no reading)
    if (!atomic_load(&g_ubp_on) || iv == 0) return;
    if (iv < 0) { atomic_fetch_add(&g_ubp_c[3], 1); return; }
    atomic_fetch_add(&g_ubp_c[iv <= 20.8 ? 0 : iv <= 29.2 ? 1 : 2], 1); atomic_fetch_add(&g_ubp_sum_us, (uint64_t)(iv * 1000));
}
// 登记过的运行时开关：名字 → 地址。iOS 发布包的主程序符号不进动态符号表，dlsym 找不到（真机首轮四类全部「找不到全局变量」、整轮白跑），
// 所以真机要量的开关必须在这里加一行；Mac / 单测里没登记的仍按符号名找。
// Registered runtime switches: name → address. Main-program symbols of an iOS release build are not in the dynamic symbol table and dlsym cannot find them (in the first real-device round all four categories were 「找不到全局变量」(global variable not found) and the whole round was wasted),
// so a switch to be measured on a real device must have a line added here; on Mac / in unit tests unregistered ones are still looked up by symbol name.
int g_fx_dedup = 1;
int g_la_cache = 1;   // 每次绘制的反射摘要 / 全零事实表 / 全零集合删除这三处按对象缓存（udc_note_draw、zskip_can_draw）；0 = 每次现算（同进程交替测量对照用）   // 渲染编码器固定功能状态同值不重发（draw 里的 FX_SET）；0 = 每次绘制照发（同进程交替测量对照用） | Per-object caching for the per-draw reflection summary / all-zero fact table / all-zero set removal (udc_note_draw, zskip_can_draw); 0 = computed every time (for in-process alternating measurement comparison)   // Render encoder fixed-function state not resent if unchanged (FX_SET in draw); 0 = sent on every draw (for in-process alternating measurement comparison)
int g_blit_bar = 1;   // Metal 4 复用拷贝编码器时的屏障（blit()）；0 = 不设，只供同进程交替量开销（数据可能乱序） | Barrier when reusing the blit encoder on Metal 4 (blit()); 0 = not set, only for measuring overhead with in-process alternation (data may be reordered)
static int *ub_var(const char *nm) {
    extern int g_bloom_q, g_dm_present_link, g_tbdr_rt, g_blit_bar, g_sdx_attdedup, g_tbdr_sub;
    static const struct { const char *n; int *p; } t[] = { {"g_bloom_q", &g_bloom_q}, {"g_dm_present_link", &g_dm_present_link}, {"g_frz_on", &g_frz_on}, {"g_fx_dedup", &g_fx_dedup}, {"g_la_cache", &g_la_cache}, {"g_tbdr_rt", &g_tbdr_rt}, {"g_blit_bar", &g_blit_bar}, {"g_sdx_attdedup", &g_sdx_attdedup}, {"g_tbdr_sub", &g_tbdr_sub} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(nm, t[i].n)) return t[i].p;
    return dlsym(RTLD_DEFAULT, nm);
}
static void ub_frame(uint64_t f) {
    if (g_ub_n < 0) { g_ub_n = 0; const char *e = getenv("DM_NVN_UB"), *n = getenv("DM_NVN_UB_N"); if (n && atoi(n) >= 60) g_ub_N = atoi(n);
        for (const char *q = e; q && *q && g_ub_n < 16; ) { const char *eq = strchr(q, '='), *end = strchr(q, ';'); if (!end) end = q + strlen(q); if (!eq || eq > end) break;
            int c = g_ub_n++; size_t l = (size_t)(eq - q); if (l > 15) l = 15; memcpy(g_ub_name[c], q, l);
            for (const char *k = eq + 1; k < end && g_ub_nk[c] < 16; ) { size_t kl = 0; while (k + kl < end && k[kl] != ',') kl++; size_t cl = kl > 47 ? 47 : kl; memcpy(g_ub_keys[c][g_ub_nk[c]++], k, cl); k += kl; if (k < end) k++; }
            q = *end ? end + 1 : end; }
        if (g_ub_n) MLOG("DM_NVN_UB：%d 类，每相 %d 帧（同进程交替测量；丢绘制的相画面不对）", g_ub_n, g_ub_N); }
    if (!g_ub_n) return;
    uint64_t ph = f / (uint64_t)g_ub_N, r = f % (uint64_t)g_ub_N;
    if (r == 45) { for (int i = 0; i < 4; i++) atomic_store(&g_ubp_c[i], 0); atomic_store(&g_ubp_sum_us, 0); atomic_store(&g_ubp_on, 1); g_ubx_cpu0 = ub_cpu(); g_ubx_cyc0 = ub_cyc(); g_ubx_f0 = f; }
    if (r != 0) return;
    if (f && atomic_load(&g_ubp_on)) { atomic_store(&g_ubp_on, 0); uint64_t pp = ph - 1; int pc = (pp & 1) ? (int)((pp / 2) % (uint64_t)g_ub_n) : -1;
        int c0 = atomic_load(&g_ubp_c[0]), c1 = atomic_load(&g_ubp_c[1]), c2 = atomic_load(&g_ubp_c[2]), c3 = atomic_load(&g_ubp_c[3]), n_ = c0 + c1 + c2;
        double nf = f > g_ubx_f0 ? (double)(f - g_ubx_f0) : 1;
        MLOG("UB 上屏 %llu %s 帧 %d 均 %.3f ms 2拍 %d 3拍 %d 4拍以上 %d 丢弃 %d 翻译每帧 CPU %.3f ms 周期 %.2f M", (unsigned long long)pp, pc < 0 ? "base" : g_ub_name[pc], n_, n_ ? atomic_load(&g_ubp_sum_us) / 1000.0 / n_ : 0.0, c0, c1, c2, c3,
             (ub_cpu() - g_ubx_cpu0) / 1e6 / nf, (ub_cyc() - g_ubx_cyc0) / 1e6 / nf); }
    int nc = (ph & 1) ? (int)((ph / 2) % (uint64_t)g_ub_n) : -1;
    static int *sp[16]; static int sv[16], ns;   // 上一相设过的变量：先还原 | Variables set by the previous phase: restore first
    for (int i = 0; i < ns; i++) *sp[i] = sv[i]; ns = 0;
    for (int i = 0; nc >= 0 && i < g_ub_nk[nc] && ns < 16; i++) { const char *k = g_ub_keys[nc][i]; if (strncmp(k, "@set:", 5)) continue;
        char nm[48]; const char *eq = strchr(k, '='); if (!eq) continue; size_t l = (size_t)(eq - (k + 5)); if (l >= sizeof nm) continue; memcpy(nm, k + 5, l); nm[l] = 0;
        int *a_ = ub_var(nm); static int bad, ok;
        if (!a_) { if (bad++ < 8) MLOG("【红】DM_NVN_UB：找不到全局变量 %s（真机上要先在 ub_var 的表里登记）", nm); continue; }
        sp[ns] = a_; sv[ns] = *a_; ns++; *a_ = atoi(eq + 1); if (ok++ < 8) MLOG("DM_NVN_UB：%s = %d（原值 %d）", nm, atoi(eq + 1), sv[ns - 1]); }
    atomic_store(&g_ub_cur, nc);
}
static int ub_skip(const char *key) {   // 本相要不要丢掉这个片元着色器的绘制 | Whether this phase should drop draws of this fragment shader
    int uc = atomic_load(&g_ub_cur); if (uc < 0) return 0;
    for (int i = 0; i < g_ub_nk[uc]; i++) { const char *k = g_ub_keys[uc][i]; if (k[0] == '@') continue; if (k[0] == '*' || !strncmp(key, k, strlen(k))) return 1; }
    return 0;
}
void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); if (g_ub_n) ub_frame(f); }
// GPU 计时登记：提交前 gpu_tag_begin(帧)，完成时 gpu_tag_done(帧, GPU 起, GPU 止)（CACurrentMediaTime 基准秒）。
// 旧队列命令缓冲走 gpu_tag_f；Metal 4 主提交在 mtl_sub_commit 的 feedback 里回填（换 Metal 4 时调用点全丢，「计时」行的 GPU 一栏成了空）
// GPU timing registration: gpu_tag_begin(frame) before commit, gpu_tag_done(frame, GPU start, GPU end) on completion (seconds on the CACurrentMediaTime base).
// Old-queue command buffers go through gpu_tag_f; the Metal 4 main submission backfills in the feedback of mtl_sub_commit (when switching to Metal 4 all the call sites were lost and the GPU column of the 「计时」(timing) line became empty)
static void gpu_tag_begin(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) { atomic_store(&g_gpu[k].frame, f); atomic_store(&g_gpu[k].ns, 0); atomic_store(&g_gpu[k].pending, 0); atomic_store(&g_gpu[k].t0, UINT64_MAX); atomic_store(&g_gpu[k].t1, 0); }
    atomic_fetch_add(&g_gpu[k].pending, 1);
}
static void gpu_tag_done(uint64_t f, double g0, double g1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) return;
    double d = g1 - g0;
    if (d > 0) {
        atomic_fetch_add(&g_gpu[k].ns, (uint64_t)(d * 1e9));
        uint64_t s0 = (uint64_t)(g0 * 1e9), s1 = (uint64_t)(g1 * 1e9), v;
        for (v = atomic_load(&g_gpu[k].t0); s0 < v && !atomic_compare_exchange_weak(&g_gpu[k].t0, &v, s0);) {}
        for (v = atomic_load(&g_gpu[k].t1); s1 > v && !atomic_compare_exchange_weak(&g_gpu[k].t1, &v, s1);) {}
    }
    atomic_fetch_sub(&g_gpu[k].pending, 1);
}
static void gpu_tag_f(id<MTLCommandBuffer> cb, uint64_t f) { gpu_tag_begin(f); [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { gpu_tag_done(f, c.GPUStartTime, c.GPUEndTime); }]; }
void gpu_tag(id<MTLCommandBuffer> cb) { gpu_tag_f(cb, atomic_load(&g_cur_frame)); }
double mtl_gpu_ms(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return -1;
    return atomic_load(&g_gpu[k].ns) / 1e6;
}
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return 0;
    *t0 = atomic_load(&g_gpu[k].t0); *t1 = atomic_load(&g_gpu[k].t1);
    return *t1 >= *t0 && *t1;
}
static void rp_flush(void);
static const char *cctx_snap(void);   // 这次提交里有什么（出错时打进红行；定义在 mtl_exec 之前） | What is in this submission (printed into the failure line on error; defined before mtl_exec)
uint64_t mtl_sub_commit(void) {
    defer_fallback();   // 推迟合并通道：提交结束仍有未回放的 P 绘制（正常路径不会到这里） | Deferred merged pass: at the end of the submission there are still P draws not replayed (the normal path never gets here)
    rp_flush();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush();
    uint64_t fence = ++g_fence;
    if (g_vis_ring_i >= 0) g_vis_ring[g_vis_ring_i].fence = fence;
    // 完成后的 CPU 可见效果：拷贝一份操作表进回调
    // CPU-visible effects after completion: copy the op table into the callback
    int n = g_npost; post_op *ops = n ? malloc(n * sizeof *ops) : NULL;
    if (n) memcpy(ops, g_post, n * sizeof *ops);
    NSArray<dispatch_block_t> *commit_blocks = [g_commit_blocks copy];
#if !DM_NVN_M3
    MTL4CommitOptions *o = [MTL4CommitOptions new];
#endif
    [FB_ON(o, g_cb) FB_SEL:^(FB_ARG f) { (void)f; for (dispatch_block_t block in commit_blocks) block(); }];
    uint64_t gpu_f = atomic_load(&g_cur_frame); gpu_tag_begin(gpu_f);
    NSString *cctx = @(cctx_snap());
    [FB_ON(o, g_cb) FB_SEL:^(FB_ARG cb) {
        gpu_tag_done(gpu_f, cb.GPUStartTime, cb.GPUEndTime);
        if (cb.error) { atomic_store(&g_last_commit_error, 1); static int n, nv; int victim = [cb.error.description containsString:@"InnocentVictim"]; if (victim ? nv++ < 1 : n++ < 8) MLOG("【红】命令缓冲错误详情 fence=%llu：%s | GPU %.3f→%.3f", (unsigned long long)fence, cb.error.description.UTF8String, cb.GPUStartTime, cb.GPUEndTime); if (!victim) MLOG("【红】出错那次提交的现场 fence=%llu：%s", (unsigned long long)fence, cctx.UTF8String); MLOG("【红】命令缓冲执行出错：%s", cb.error.localizedDescription.UTF8String); }
        for (int i = 0; i < n; i++) {
            if (ops[i].kind == 1) { if (ops[i].mode == 1) *ops[i].val += ops[i].v; else *ops[i].val = ops[i].v; }
            else if (ops[i].kind == 3) {   // 通过采样数：自清零以来各槽之和 | Samples passed: the sum of all slots since the reset
                vis_list *l = ops[i].vis; uint64_t sum = 0;
                for (int k = 0; k < l->n; k++) { sum += ((const uint64_t *)((__bridge id<MTLBuffer>)l->s[k].buf).contents)[l->s[k].slot]; CFRelease(l->s[k].buf); }
                free(l); uint64_t rec[2] = {sum, gpu_ticks()}; memcpy((void *)ops[i].addr, rec, sizeof rec);
            }
            else { uint64_t t = gpu_ticks(), rec[2] = {ops[i].type == 0 || ops[i].type == 0x10 ? t : 0, t}; memcpy((void *)ops[i].addr, rec, sizeof rec); }
        }
        free(ops);
        pthread_mutex_lock(&g_mu); if (fence > g_done) g_done = fence; pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
        defer_drain();
    }];
    pg_commit(FB_ON(o, g_cb));   // 逐通道 GPU 计时（DM_NVN_PASSGPU）：没开时直接返回 | Per-pass GPU timing (DM_NVN_PASSGPU): returns immediately when off
    void arena_commit(uint64_t fence); arena_commit(fence);
    res_commit();
#if DM_NVN_M3
    [g_cb encodeSignalEvent:g_ev value:fence];
    [g_cb commit];
#else
    [g_cb endCommandBuffer];
    CBUF buffers[] = {g_cb};
    [g_q4 commit:buffers count:1 options:o];
    [g_q4 signalEvent:g_ev value:fence];
#endif
    cb_committed();
    g_alloc_fence[g_alloc_cur] = fence;
    g_cb = nil;
    g_commit_blocks = nil;
    objc_autoreleasePoolPop(g_sub_pool); g_sub_pool = NULL;
    return fence;
}

uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_last_commit_error(void) { return atomic_load(&g_last_commit_error); }

int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += ns / 1000000000ull; dl.tv_nsec = ns % 1000000000ull;
    pthread_mutex_lock(&g_mu);
    int r = 0;
    while (g_done < fence) if ((r = pthread_cond_timedwait(&g_cv, &g_mu, &dl))) break;
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

static void mtl_finish_gpu(void) {
    if (g_fence) mtl_wait(g_fence, UINT64_MAX);
    while (atomic_load(&g_up_pending)) usleep(1000);
}
void mtl_present_flush(void);
void mtl_finish(void) { mtl_present_flush(); mtl_finish_gpu(); }   // 队列语义：已入队的出图也算队列工作 | Queue semantics: already-enqueued presents also count as queue work
static void present_drop_for_suspend(void);
void mtl_drain(void) { present_drop_for_suspend(); mtl_present_flush(); mtl_init(); id<MTLCommandBuffer> cb = q_cb(); [cb commit]; [cb waitUntilCompleted]; mtl_finish_gpu(); }

// ================= 渲染回放 =================
// ================= Render replay =================
#include "../../host/front.h"
static int g_red_once[64];
#define RED_ONCE(k, ...) do { if (!g_red_once[k]++) MLOG("【红】" __VA_ARGS__); } while (0)

// ---------- 贴图 → MTLTexture ----------
// ---------- Texture → MTLTexture ----------
static int fmt_compressed(int f) { return (f >= 0x41 && f <= 0x50) || (f >= 0x79 && f <= 0x94); }
static int fmt_depth(int f) { return f >= 0x31 && f <= 0x36; }
typedef struct { MTLPixelFormat a, b; } pfv_pair;
static const pfv_pair g_pfv_srgb[] = {
    {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB},
    {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB},
    {MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB},
    {MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB},
    {MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB},
    {MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB},
};
static int tex_need_pfv(MTLPixelFormat base, MTLPixelFormat view) {
    if (base == view) return 0;
    for (size_t i = 0; i < sizeof g_pfv_srgb / sizeof *g_pfv_srgb; i++)
        if ((base == g_pfv_srgb[i].a && view == g_pfv_srgb[i].b) || (base == g_pfv_srgb[i].b && view == g_pfv_srgb[i].a)) return 0;
    return 1;
}
// 回放热路径的缓存表一律用 nvn_hmap（C 哈希，键不装箱）；表里存 CFBridgingRetain 过的对象，替换 / 删除时 CFRelease
// Cache tables on the replay hot path always use nvn_hmap (C hash, keys not boxed); the table stores CFBridgingRetain'ed objects, CFRelease'd on replace / delete
static void hm_rel(void *v) { CFRelease(v); }
static __unsafe_unretained id hm_get_id(const nvn_hmap *m, const void *k) { return (__bridge id)nvn_hm_get(m, k); }
static void hm_put_id(nvn_hmap *m, const void *k, id o) { void *old = nvn_hm_put(m, k, (void *)CFBridgingRetain(o)); if (old) CFRelease(old); }
static void hm_del_id(nvn_hmap *m, const void *k) { void *old = nvn_hm_del(m, k); if (old) CFRelease(old); }
static nvn_hmap g_by_serial = {.klen = 8};     // 序号 → MTLTexture / NvPkg / NvProg | Serial → MTLTexture / NvPkg / NvProg
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
// 序号表代号：表项增删、预排布贴图换新时 +1。下游缓存（贴图视图、程序）记下取用时的代号，代号未变即可直接用、不必再查表
// Serial table generation: +1 when entries are added / removed or a packaged texture is replaced. Downstream caches (texture views, programs) record the generation at fetch time; if it is unchanged they can be used directly without another table lookup
static _Atomic uint64_t g_ser_gen = 1;
static __unsafe_unretained id serial_get(uint64_t k) { pthread_mutex_lock(&g_smu); __unsafe_unretained id o = hm_get_id(&g_by_serial, &k); pthread_mutex_unlock(&g_smu); return o; }
static void serial_put(uint64_t k, id o) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) defer_obj_release(old); hm_put_id(&g_by_serial, &k, o); g_ser_gen++; pthread_mutex_unlock(&g_smu); }
static void serial_del(uint64_t k) { pthread_mutex_lock(&g_smu); id old = hm_get_id(&g_by_serial, &k); if (old) defer_obj_release(old); hm_del_id(&g_by_serial, &k); g_ser_gen++; pthread_mutex_unlock(&g_smu); }
// 预排布贴图的宿主记录：当前 MTLTexture、上传戳、上次检查时的全局写代号
// Host record of a packaged texture: the current MTLTexture, the upload stamp, and the global write generation at the last check
@interface NvPkg : NSObject { @public id<MTLTexture> tex; uint64_t stamp, seen, hash; } @end   // hash：DM_NVN_DIRTYCHECK 时上传内容的全量哈希 | hash: full hash of the uploaded content under DM_NVN_DIRTYCHECK
@implementation NvPkg
- (void)dealloc { res_remove(tex); }
@end
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_pkg_miss;
static uint64_t g_pfv_upgrades;
// DM_NVN_DIRTYCHECK=1（诊断，对照真值）：每次使用都全量哈希池内容，变了却没被脏页跟踪检出 = 漏检
// DM_NVN_DIRTYCHECK=1 (diagnostic, compared against ground truth): fully hash the pool content on every use; changed but not detected by dirty-page tracking = a missed detection
static int dirtycheck(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_DIRTYCHECK") != NULL; return v; }
static uint64_t full_hash(const uint8_t *m, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ m[i]) * 1099511628211ull; return h; }
// 显卡不支持 BC 压缩格式（A15 / A16 等）：BC 贴图建成解压后的格式，上传 / 缓冲→贴图拷贝时在 CPU 上解压（nvn_bcdec.c）。
// 游戏贴图里的 BC3 由老机型补充包离线换成了 ASTC（dm/tools/astc_pack.py），走到这里的是剩下的少量 BC。测试开关 DM_TEST_NO_BC=1 在任何机器上强制走这条路
// The GPU does not support BC compressed formats (A15 / A16 etc.): BC textures are created in the decompressed format and decompressed on the CPU at upload / buffer→texture copy time (nvn_bcdec.c).
// BC3 among the game textures was converted to ASTC offline by the Legacy Device Pack (dm/tools/astc_pack.py); what reaches here is the small remainder of BC. The test switch DM_TEST_NO_BC=1 forces this path on any machine
static int no_bc(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_TEST_NO_BC"); v = (e && *e && strcmp(e, "0")) || ![g_dev supportsBCTextureCompression];
        if (v) MLOG("显卡不支持 BC 压缩格式%s：BC 贴图建成解压格式（BC1–5 在 GPU 上解压，BC6H / BC7 / 3D 在 CPU 上）", e && *e && strcmp(e, "0") ? "（测试开关 DM_TEST_NO_BC）" : ""); }
    return v;
}
static int bc_unpack(uint32_t nvn_fmt, uint32_t *pf, int *bpp) { return no_bc() && nvn_bc_unpacked(nvn_fmt, pf, bpp); }
static MTLTextureDescriptor *tex_desc(const tex_t *t, int packaged) {
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf) { RED_ONCE(0, "贴图格式 %#x Metal 无对应（首例）", t->d.format); return nil; }
    int unp = 0; { uint32_t upf; int ubpp; if (bc_unpack(t->d.format, &upf, &ubpp)) { pf = upf; unp = 1; } }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = (MTLPixelFormat)pf;
    d.width = t->d.width ? t->d.width : 1; d.height = t->d.height ? t->d.height : 1; d.depth = 1; d.arrayLength = 1;
    d.mipmapLevelCount = t->d.levels ? t->d.levels : 1;
    switch (t->d.target) {
        case 0: d.textureType = MTLTextureType1D; d.height = 1; break;
        case 2: d.textureType = MTLTextureType3D; d.depth = t->d.depth; break;
        case 3: d.textureType = MTLTextureType1DArray; d.arrayLength = t->d.height; d.height = 1; break;
        case 4: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 5: d.textureType = MTLTextureType2DMultisample; d.sampleCount = t->d.samples; d.mipmapLevelCount = 1; break;
        case 6: d.textureType = MTLTextureType2DMultisampleArray; d.sampleCount = t->d.samples; d.arrayLength = t->d.depth; d.mipmapLevelCount = 1; break;
        case 8: d.textureType = MTLTextureTypeCube; break;
        case 9: d.textureType = MTLTextureTypeCubeArray; d.arrayLength = t->d.depth; break;
        case 10: RED_ONCE(1, "缓冲贴图（TARGET_BUFFER）未实现"); return nil;
        default: d.textureType = MTLTextureType2D; break;
    }
    d.storageMode = MTLStorageModePrivate;   // 渲染目标与素材都放 GPU 私有存储（可用无损压缩） | Render targets and assets both go in GPU private storage (lossless compression available)
    d.usage = MTLTextureUsageShaderRead | (fmt_compressed(t->d.format) || packaged ? 0 : MTLTextureUsageRenderTarget)
            | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0)   // 图像用途：着色器可写（计算核生成 3D 调色表等） | Image usage: shader-writable (compute kernels generating the 3D color lookup table, etc.)
            | (unp ? MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView : 0);   // 无 BC：解压格式贴图由 GPU 解压核写（up_dec）/ 接 ASTC 源的整块拷贝（copy_t2t_astc） | No BC: textures in the decompressed format are written by the GPU decompression kernel (up_dec) / receive whole-block copies from ASTC sources (copy_t2t_astc)
    return d;
}
// 就地升级：原贴图没开 PixelFormatView（缺省不开，保住苹果显卡无损压缩）却要建换格式视图时，按原规格 + PixelFormatView 新建、逐 slice × level 拷过去并替换登记。
// 计数：选角 / 营地 / 野外 / 战斗 4 页 17473 张贴图换格式视图 0 次，所以这里是兜底，不靠预测哪些贴图需要
// In-place upgrade: when the original texture was not created with PixelFormatView (off by default, to keep Apple GPU lossless compression) but a different-format view is needed, create a new one with the original spec + PixelFormatView, copy over every slice × level, and replace the registration.
// Count: across the 4 screens character select / camp / wilderness / combat, 17473 textures needed a different-format view 0 times, so this is a fallback and does not rely on predicting which textures need it
static id<MTLTexture> tex_upgrade_pfv(const tex_t *t, id<MTLTexture> base, MTLPixelFormat view_pf) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = base.textureType; d.pixelFormat = base.pixelFormat; d.width = base.width; d.height = base.height; d.depth = base.depth;   // 保留原格式，只加换格式视图用途 | Keep the original format, only add the format-view usage
    d.arrayLength = base.arrayLength; d.mipmapLevelCount = base.mipmapLevelCount; d.sampleCount = base.sampleCount;
    d.storageMode = base.storageMode; d.cpuCacheMode = base.cpuCacheMode; d.hazardTrackingMode = base.hazardTrackingMode;
    d.resourceOptions = base.resourceOptions; d.usage = base.usage | MTLTextureUsagePixelFormatView;
    id<MTLTexture> upgraded = [g_dev newTextureWithDescriptor:d];
    if (!upgraded) { RED_ONCE(45, "贴图升级失败 ser=%llu 格式 %lu → %lu", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf); return base; }
    res_add(upgraded);
    BENC b = blit();
    NSUInteger slices = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    for (NSUInteger l = 0; l < base.mipmapLevelCount; l++) {
        NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u), z = base.textureType == MTLTextureType3D ? MAX(base.depth >> l, 1u) : 1;
        if (base.textureType == MTLTextureType3D)
            enc_b_copy_texture(b, base, 0, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, z), upgraded, 0, l, MTLOriginMake(0, 0, 0));
        else for (NSUInteger s = 0; s < slices; s++)
            enc_b_copy_texture(b, base, s, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), upgraded, s, l, MTLOriginMake(0, 0, 0));
    }
    id holder = serial_get(t->serial);
    if ([holder isKindOfClass:[NvPkg class]]) { id<MTLTexture> old = ((NvPkg *)holder)->tex; ((NvPkg *)holder)->tex = upgraded; defer_obj_release(old); g_ser_gen++; }
    else serial_put(t->serial, upgraded);
    MLOG("贴图升级为可换格式视图 ser=%llu 基格式 %lu → 视图格式 %lu（升级次数=%llu）", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf, ++g_pfv_upgrades);
    return upgraded;
}
// 预排布贴图上传：先设只读、再取戳、再读池内存解排布进暂存缓冲，blit 进新的 private 贴图。
// 批量：一次回放提交内的上传共用一个上传命令缓冲（计算编码器解排布 + 统一 blit），在回放命令缓冲提交前冲刷；
// 同一队列按提交顺序执行 ⇒ 首次采样前必已上传完。提交之外的上传当场冲刷。暂存区按 32 MB 私有块切分，命令缓冲持有到完成。
// Packaged texture upload: first set read-only, then take the stamp, then read the pool memory and deswizzle into a staging buffer, and blit into a new private texture.
// Batching: uploads within one replay submission share one upload command buffer (compute encoder deswizzle + a unified blit), flushed before the replay command buffer is committed;
// the same queue executes in commit order ⇒ uploads are always finished before the first sample. Uploads outside a submission are flushed on the spot. The staging area is cut from 32 MB private blocks, held by the command buffer until completion.
static _Atomic uint64_t g_upload_ns;   // 提交线程上解排布 + 上传编码的累计耗时（计时埋点） | Cumulative time of deswizzle + upload encoding on the submit thread (timing instrumentation)
static _Atomic uint64_t g_upload_gpu_ns;
static CBUF g_up_cb;
static CENC g_up_ce;
static _Atomic int g_up_alloc_busy[3];
static unsigned g_up_seq;
#define UP_BLOCK_SIZE (32u * 1024u * 1024u)
typedef struct {
    __unsafe_unretained id<MTLBuffer> src;
    size_t off, row, image;
    MTLSize size;
    __unsafe_unretained id<MTLTexture> dst;   // 由 g_up_texs 强引用到冲刷编码完 | Strongly referenced by g_up_texs until the flush is encoded
    NSUInteger slice, level;
} up_blit;
static up_blit *g_up_blits; static int g_nup_blits, g_up_blitcap;
// 无 BC 时 BC1–5 在 GPU 上解压（A16 日志：CPU 解排布 + CPU 解压单帧 300 ms 以上）：GPU 解排布把压缩块写进暂存区，冲刷时过了屏障
// 由计算核 bcd 一线程一块解进贴图（目标是解压格式的 2D 视图，sRGB 底图用 RGBA8Unorm 视图写原值）。BC6H / BC7 / 3D 贴图量极少，仍走 CPU（nvn_bcdec.c）
// Without BC, BC1–5 are decompressed on the GPU (A16 logs: CPU deswizzle + CPU decompression took over 300 ms in a single frame): the GPU deswizzle writes the compressed blocks into the staging area, and at flush time, after the barrier,
// the compute kernel bcd decodes them into the texture one thread per block (the target is a 2D view in the decompressed format; an sRGB base texture is written with raw values through an RGBA8Unorm view). BC6H / BC7 / 3D textures are very few and still go through the CPU (nvn_bcdec.c)
typedef struct { __unsafe_unretained id<MTLBuffer> src, params; size_t off, poff; uint32_t bw, bh; __unsafe_unretained id<MTLTexture> view; } up_dec;   // view：批内由 g_up_texs、之后由 defer_obj_unref 持有到 GPU 跑完 | view: held by g_up_texs within the batch, and afterwards by defer_obj_unref until the GPU finishes
static up_dec *g_up_decs; static int g_nup_decs, g_up_deccap;
static const char k_bcd_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct P { uint off, pitch, w, h, fmt; };\n"   // fmt：1 BC1、2 BC2、3 BC3、4 BC4、5 BC4 有符号、6 BC5、7 BC5 有符号 | fmt: 1 BC1, 2 BC2, 3 BC3, 4 BC4, 5 BC4 signed, 6 BC5, 7 BC5 signed
"static float3 c565(uint c) { return float3((c >> 11) & 31, (c >> 5) & 63, c & 31) / float3(31, 63, 31); }\n"
"static void col(device const uchar *b, thread float4 *o, bool four) {\n"
"  uint c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8), ix = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24); float3 e0 = c565(c0), e1 = c565(c1); float4 p[4];\n"
"  p[0] = float4(e0, 1); p[1] = float4(e1, 1);\n"
"  if (four || c0 > c1) { p[2] = float4((2 * e0 + e1) / 3, 1); p[3] = float4((e0 + 2 * e1) / 3, 1); } else { p[2] = float4((e0 + e1) / 2, 1); p[3] = float4(0); }\n"
"  for (int i = 0; i < 16; i++) o[i] = p[(ix >> (2 * i)) & 3]; }\n"
"static void a8(device const uchar *b, thread float *o, bool sn) {\n"   // BC4 / BC3 alpha / BC5 的单通道块 | Single-channel block of BC4 / BC3 alpha / BC5
"  float a0 = sn ? max(float(as_type<char>(b[0])), -127.0) / 127 : b[0] / 255.0, a1 = sn ? max(float(as_type<char>(b[1])), -127.0) / 127 : b[1] / 255.0;\n"
"  bool eight = sn ? as_type<char>(b[0]) > as_type<char>(b[1]) : b[0] > b[1]; float lo = sn ? -1 : 0; float p[8]; p[0] = a0; p[1] = a1;\n"
"  for (int k = 2; k < 8; k++) p[k] = eight ? ((8 - k) * a0 + (k - 1) * a1) / 7 : (k < 6 ? ((6 - k) * a0 + (k - 1) * a1) / 5 : (k == 6 ? lo : 1));\n"
"  uint lo24 = b[2] | (b[3] << 8) | (b[4] << 16), hi24 = b[5] | (b[6] << 8) | (b[7] << 16);\n"
"  for (int i = 0; i < 16; i++) o[i] = p[i < 8 ? (lo24 >> (3 * i)) & 7 : (hi24 >> (3 * (i - 8))) & 7]; }\n"
"kernel void bcd(device const uchar *src [[buffer(0)]], device const P &p [[buffer(1)]], texture2d<float, access::write> dst [[texture(0)]], uint2 g [[thread_position_in_grid]]) {\n"
"  uint nbx = (p.w + 3) / 4, nby = (p.h + 3) / 4; if (g.x >= nbx || g.y >= nby) return;\n"
"  uint bb = (p.fmt == 1 || p.fmt == 4 || p.fmt == 5) ? 8 : 16; device const uchar *b = src + p.off + g.y * p.pitch + g.x * bb; float4 o[16]; float a[16], c[16];\n"
"  if (p.fmt == 1) col(b, o, false);\n"
"  else if (p.fmt == 2) { col(b + 8, o, true); for (int i = 0; i < 16; i++) o[i].a = ((b[i / 2] >> (4 * (i & 1))) & 15) / 15.0; }\n"
"  else if (p.fmt == 3) { col(b + 8, o, true); a8(b, a, false); for (int i = 0; i < 16; i++) o[i].a = a[i]; }\n"
"  else if (p.fmt == 4 || p.fmt == 5) { a8(b, a, p.fmt == 5); for (int i = 0; i < 16; i++) o[i] = float4(a[i], 0, 0, 1); }\n"
"  else { a8(b, a, p.fmt == 7); a8(b + 8, c, p.fmt == 7); for (int i = 0; i < 16; i++) o[i] = float4(a[i], c[i], 0, 1); }\n"
"  for (int i = 0; i < 16; i++) { uint2 q = g * 4 + uint2(i & 3, i >> 2); if (q.x < p.w && q.y < p.h) dst.write(o[i], q); } }\n";
static int bcd_fmt(uint32_t f) {   // NVN 格式 → bcd 的 fmt；0 = GPU 不解（BC6H / BC7 / 非 BC） | NVN format → bcd fmt; 0 = not decoded on the GPU (BC6H / BC7 / non-BC)
    switch (f) { case 0x41: case 0x42: case 0x45: case 0x46: return 1; case 0x43: case 0x47: return 2; case 0x44: case 0x48: return 3;
                 case 0x49: return 4; case 0x4a: return 5; case 0x4b: return 6; case 0x4c: return 7; default: return 0; }
}
static id<MTLComputePipelineState> bcd_pso(void) {
    static id<MTLComputePipelineState> ps; static int failed;
    if (!ps && !failed) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_bcd_msl) options:nil error:&e]; id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"bcd"] : nil;
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!ps) { failed = 1; MLOG("【红】无 BC：GPU 解压管线建不出 %s（退回 CPU 解压）", e.localizedDescription.UTF8String); } else MLOG("无 BC：BC1–5 改在 GPU 上解压"); }
    return ps;
}
static NSMutableArray *g_up_texs;   // 批内目标贴图：冲刷前同次提交里贴图可能被换掉 / 释放 | Target textures in the batch: before the flush, textures may be replaced / released within the same submission
static id<MTLBuffer> g_up_block;
static size_t g_up_block_off, g_up_block_size;
static NSMutableArray *g_up_keep;


// 上传命令缓冲被系统丢弃（GPU 出错恢复时连带丢，进崔凡克首帧那次每局丢一条）：这批贴图的对象已建好、时间戳已更新，内容却没写进去，
// 游戏那边池内存不再变就永远不会重传。记下这批目标贴图，下次取用到其中哪张就当它脏了重传一次。
// DM_TEST_UP_LOST=<第几批>（测试用）：把那一批上传当成被丢，验证重传路径。
// The upload command buffer was discarded by the system (dropped along during GPU error recovery; the first frame on entering Travincal drops one every run): the objects of this batch of textures are built and the timestamps updated, but the content was never written in,
// and if the pool memory on the game side no longer changes it will never be re-uploaded. Record this batch of target textures, and the next time any of them is fetched treat it as dirty and re-upload once.
// DM_TEST_UP_LOST=<batch index> (for testing): treat that batch of uploads as dropped, to verify the re-upload path.
static NSHashTable *g_up_lost; static os_unfair_lock g_up_lost_lk = OS_UNFAIR_LOCK_INIT; static _Atomic int g_up_lost_n; static _Atomic uint64_t g_up_lost_redo;   // 弱引用表：记录里的贴图若之后再没被取用、被正常释放，不由这张表拖着 | Weak-reference table: if a texture in the record is never fetched again and is released normally, this table does not keep it alive
static int up_lost_take(id tex) {   // 这张贴图的上传被丢过：从记录里取走并返回 1 | This texture's upload was dropped: take it out of the record and return 1
    if (!tex) return 0; os_unfair_lock_lock(&g_up_lost_lk); int hit = [g_up_lost containsObject:tex]; if (hit) { [g_up_lost removeObject:tex]; atomic_store(&g_up_lost_n, (int)g_up_lost.allObjects.count); atomic_fetch_add(&g_up_lost_redo, 1); } os_unfair_lock_unlock(&g_up_lost_lk); return hit;
}
static void up_diag(void);
static void up_flush(void) {
    up_owner_check("up_flush");
    if (!g_up_cb) return;
    if (g_up_ce) {
        m4_end(g_up_ce); g_up_ce = nil;
    }
#if DM_NVN_M3
    // 旧接口：拷贝与计算是两种编码器——先一个拷贝编码器做「暂存块 → 贴图」，再一个计算编码器做无 BC 的 GPU 解压
    // Older API: copy and compute are two kinds of encoder - first a copy encoder does "staging block -> texture", then a compute encoder does the GPU decompression for devices without BC
    BENC b = g_nup_blits ? [g_up_cb blitCommandEncoder] : nil;
    for (int i = 0; b && i < g_nup_blits; i++) {
        up_blit *x = &g_up_blits[i];
        enc_b_copy_buffer_texture(b, x->src, x->off, x->row, x->image, x->size, x->dst, x->slice, x->level, MTLOriginMake(0, 0, 0));
    }
    [b endEncoding];
    CENC dc = g_nup_decs ? [g_up_cb computeCommandEncoder] : nil;
    if (dc) {
        [dc setComputePipelineState:bcd_pso()];
        for (int i = 0; i < g_nup_decs; i++) { up_dec *x = &g_up_decs[i];
            [dc setBuffer:x->src offset:0 atIndex:0]; [dc setBuffer:x->params offset:x->poff atIndex:1]; [dc setTexture:x->view atIndex:0];
            [dc dispatchThreadgroups:MTLSizeMake((x->bw + 7) / 8, (x->bh + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; }
    }
    [dc endEncoding]; g_up_ce = nil;
#else
    CENC b = g_nup_blits || g_nup_decs ? g_up_ce = [g_up_cb computeCommandEncoder] : nil;
    if (b) [b barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    for (int i = 0; b && i < g_nup_blits; i++) {
        up_blit *x = &g_up_blits[i];
        enc_b_copy_buffer_texture(b, x->src, x->off, x->row, x->image, x->size, x->dst, x->slice, x->level, MTLOriginMake(0, 0, 0));
    }
    if (b && g_nup_decs) {   // 无 BC 的 GPU 解压（见 up_dec）：参数块里的偏移相对暂存块起点 | GPU decompression without BC (see up_dec): offsets in the parameter block are relative to the start of the staging block
        [b setArgumentTable:g_up_ct]; [b setComputePipelineState:bcd_pso()];
        for (int i = 0; i < g_nup_decs; i++) { up_dec *x = &g_up_decs[i];
            [g_up_ct setAddress:x->src.gpuAddress atIndex:0]; [g_up_ct setAddress:x->params.gpuAddress + x->poff atIndex:1]; [g_up_ct setTexture:x->view.gpuResourceID atIndex:0];
            [b dispatchThreadgroups:MTLSizeMake((x->bw + 7) / 8, (x->bh + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; }
    }
    if (b) { m4_end(b); g_up_ce = nil; }
#endif
    CBUF cb = g_up_cb;
    unsigned slot = (g_up_seq - 1) % 3;
    NSMutableArray *dsts = [NSMutableArray arrayWithCapacity:(NSUInteger)g_nup_blits]; for (int i = 0; i < g_nup_blits; i++) if (g_up_blits[i].dst) [dsts addObject:g_up_blits[i].dst];
    static int lost_at = -2; static uint64_t batch_n; if (lost_at == -2) { const char *e = getenv("DM_TEST_UP_LOST"); lost_at = e ? atoi(e) : -1; } int fake_lost = dsts.count && lost_at > 0 && (int)++batch_n == lost_at;
#if !DM_NVN_M3
    MTL4CommitOptions *o = [MTL4CommitOptions new];
#endif
    atomic_fetch_add(&g_up_pending, 1);
    [FB_ON(o, cb) FB_SEL:^(FB_ARG f) {
        if (f.error) MLOG("【红】上传命令缓冲执行出错：%s", f.error.description.UTF8String);
        if (f.error || fake_lost) {
            os_unfair_lock_lock(&g_up_lost_lk); if (!g_up_lost) g_up_lost = [NSHashTable weakObjectsHashTable]; for (id d in dsts) [g_up_lost addObject:d]; int n = (int)g_up_lost.allObjects.count; atomic_store(&g_up_lost_n, n); os_unfair_lock_unlock(&g_up_lost_lk);
            pthread_mutex_lock(&g_smu); g_ser_gen++; pthread_mutex_unlock(&g_smu);   // 让各贴图号的视图缓存走一次慢路（缓存命中时不会经过 tex_mtl，看不到这份记录） | Make the view cache of every texture ID take the slow path once (on a cache hit tex_mtl is not passed through and this record is not seen)
            MLOG("%s上传命令缓冲被丢：这一批 %lu 张贴图记为内容未写，下次取用时重传（待重传共 %d 张）", fake_lost ? "DM_TEST_UP_LOST：" : "", (unsigned long)dsts.count, n);
        }
        double d = f.GPUEndTime - f.GPUStartTime;
        if (d > 0) atomic_fetch_add(&g_upload_gpu_ns, (uint64_t)(d * 1e9));
        atomic_store(&g_up_alloc_busy[slot], 0);
        atomic_fetch_sub(&g_up_pending, 1);
    }];
    res_commit();
#if DM_NVN_M3
    [cb commit];
#else
    [cb endCommandBuffer];
    CBUF buffers[] = {cb};
    [g_q4 commit:buffers count:1 options:o];
#endif
    cb_committed();
    g_up_cb = nil; if (g_up_block) { defer_obj_release(g_up_block); g_up_block = nil; } g_up_block_off = g_up_block_size = 0;
    for (id obj in g_up_keep) defer_obj_release(obj);
    [g_up_keep removeAllObjects];
    up_diag();
    g_nup_blits = 0; g_nup_decs = 0;
    [g_up_texs removeAllObjects];
}

static void up_begin(void) {
    up_owner_check("up_begin");
    if (g_up_cb) return;
    unsigned slot = g_up_seq++ % 3;
    while (atomic_load(&g_up_alloc_busy[slot])) usleep(1000);
    atomic_store(&g_up_alloc_busy[slot], 1);
#if DM_NVN_M3
    g_up_cb = [g_q commandBuffer];
    g_up_ce = [g_up_cb computeCommandEncoder];
#else
    [g_up_alloc4[slot] reset];
    g_up_cb = [g_dev newCommandBuffer];
    [g_up_cb beginCommandBufferWithAllocator:g_up_alloc4[slot]];
    g_up_ce = [g_up_cb computeCommandEncoder];
    [g_up_ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [g_up_ce setArgumentTable:g_up_ct];
#endif
    if (!g_up_texs) g_up_texs = [NSMutableArray array];
    if (!g_up_keep) g_up_keep = [NSMutableArray array];
}

// 诊断读数（低频，换块次数有变化且距上一行 ≥ 1 秒才打，一局最多 200 行）：一批上传里暂存块放不下、中途换了几次块；显卡已分配、驻留集对象数、延迟释放队列长度。
// 换块时旧块若没退役，它留在驻留集里永远放不掉，「显卡已分配」会随换块次数一块一块往上涨
// Diagnostic readout (low frequency; printed only when the block switch count changed and ≥ 1 second since the last line, at most 200 lines per game): how many times an upload batch ran out of staging block and switched blocks midway; GPU allocated, residency set object count, deferred-release queue length.
// If the old block is not retired on a switch it stays in the residency set and can never be freed, and "GPU allocated" climbs one block at a time with the switch count
static uint64_t g_up_blocks_new, g_up_swaps, g_up_swap_bytes; static NSHashTable *g_up_old;   // g_up_old：换下的旧块（弱引用），诊断行数它们还活着几块 | g_up_old: old blocks switched out (weak references); the diagnostic line counts how many are still alive
static void up_diag(void) {
    // 换块次数变了打一行；之后过 5 秒再补一行（那一批提交早已跑完、延迟退役也排空了，「显卡已分配」是落定后的数）
    // Print a line when the block switch count changes; then one more line 5 seconds later (that batch's commit is long done and deferred retirement drained, so "GPU allocated" is the settled number)
    static uint64_t said_swaps = ~0ull, last_ns; static int lines, follow; uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    int chg = g_up_swaps != said_swaps, fol = follow && now - last_ns >= 5000000000ull; if ((!chg && !fol) || lines >= 200) return;
    if (chg && last_ns && now - last_ns < 1000000000ull) return; last_ns = now; said_swaps = g_up_swaps; lines++; follow = chg;
    unsigned long alive = 0, inres = 0; uint64_t abytes = 0; NSArray *olds = g_up_old.allObjects;
    pthread_mutex_lock(&g_res_mu); unsigned long nres = (unsigned long)g_res_objects.allObjects.count; for (id<MTLBuffer> o in olds) { alive++; abytes += o.length; inres += [g_res_objects containsObject:o]; } pthread_mutex_unlock(&g_res_mu);
    pthread_mutex_lock(&g_deferred_mu); unsigned long ndef = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    MLOG("上传暂存块%s：累计新建 %llu 块、其中一批内换块 %llu 次（换下的旧块共 %llu MB）；换下的旧块此刻还活着 %lu 块 %llu MB、仍在驻留集 %lu 块；显卡已分配 %llu MB、驻留集 %lu 个对象、延迟释放队列 %lu", fol && !chg ? "（落定后）" : "",
         (unsigned long long)g_up_blocks_new, (unsigned long long)g_up_swaps, (unsigned long long)(g_up_swap_bytes >> 20), alive, (unsigned long long)(abytes >> 20), inres, (unsigned long long)(g_dev.currentAllocatedSize >> 20), nres, ndef);
}
static id<MTLBuffer> up_alloc(size_t n, size_t *off) {
    size_t need = (n + 255u) & ~((size_t)255u);
    if (!g_up_block || g_up_block_off + need > g_up_block_size) {
        // 换块：旧块还被这一批已经登记的拷贝 / 解压命令引用着，不能当场放；交给 g_up_keep，冲刷提交后和别的暂存一起延迟退役（退役时撤出驻留集）。
        // 原先这里直接把 g_up_block 换成新块：旧块没人退役、却一直留在驻留集里，一批上传每多换一次块就漏掉一块（通常 32 MB）显卡内存。
        // 旧块与冲刷时退役的「当前块」是两个对象，不会重复退役
        // Block switch: the old block is still referenced by copy / decompress commands already recorded in this batch and cannot be freed on the spot; hand it to g_up_keep, to be retired later with the other staging after the flush commits (removed from the residency set on retire).
        // Previously g_up_block was simply replaced with the new block here: nobody retired the old block, yet it stayed in the residency set, so each extra block switch in an upload batch leaked one block (usually 32 MB) of GPU memory.
        // The old block and the "current block" retired at flush are two objects, so nothing is retired twice
        g_up_blocks_new++; if (g_up_block) { g_up_swaps++; g_up_swap_bytes += g_up_block_size; if (!g_up_old) g_up_old = [NSHashTable weakObjectsHashTable]; [g_up_old addObject:g_up_block]; static int leak = -1; if (leak < 0) leak = getenv("DM_TEST_UP_LEAK") != NULL;   // 测试开关 DM_TEST_UP_LEAK=1：照修前的做法不退役旧块（对照用，量漏多少） | test switch DM_TEST_UP_LEAK=1: do not retire the old block, as before the fix (for comparison, to measure the leak)
            if (!leak) { if (!g_up_keep) g_up_keep = [NSMutableArray array]; [g_up_keep addObject:g_up_block]; } }
        g_up_block_size = n > UP_BLOCK_SIZE ? need : UP_BLOCK_SIZE;
        g_up_block = [g_dev newBufferWithLength:g_up_block_size options:MTLResourceStorageModePrivate];
        if (!g_up_block) MDIE("建预排布上传暂存块失败 %#zx", g_up_block_size);
        res_add(g_up_block);
        g_up_block_off = 0;
    }
    *off = g_up_block_off;
    g_up_block_off += need;
    return g_up_block;
}

// 解排布参数块：64 字节一格，从 64 KB 共享存储块里顺序切；块满换新，旧块按栅栏延迟释放
// Deswizzle parameter blocks: 64 bytes per cell, cut sequentially from a 64 KB shared-storage block; a full block is replaced by a new one, and the old block is released lazily by fence
static id<MTLBuffer> g_up_pblock; static size_t g_up_pblock_off;
static id<MTLBuffer> up_param_alloc(size_t *off) {
    if (!g_up_pblock || g_up_pblock_off + 64 > g_up_pblock.length) {
        if (g_up_pblock) defer_obj_release(g_up_pblock);
        g_up_pblock = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared];
        if (!g_up_pblock) MDIE("建解排布参数块失败");
        res_add(g_up_pblock); g_up_pblock_off = 0;
    }
    *off = g_up_pblock_off; g_up_pblock_off += 64;
    return g_up_pblock;
}
static void up_blit_add(id<MTLBuffer> src, size_t off, size_t row, size_t image, MTLSize size,
                        id<MTLTexture> dst, NSUInteger slice, NSUInteger level) {
    if (g_nup_blits == g_up_blitcap) {
        g_up_blitcap = g_up_blitcap ? g_up_blitcap * 2 : 256;
        g_up_blits = realloc(g_up_blits, g_up_blitcap * sizeof *g_up_blits);
    }
    g_up_blits[g_nup_blits++] = (up_blit){src, off, row, image, size, dst, slice, level};
}

static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp);
static id<MTLTexture> pkg_upload(const tex_t *t, uint64_t *stamp) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    id<MTLTexture> r = pkg_upload_(t, stamp);
    clock_gettime(CLOCK_MONOTONIC, &b); uint64_t d_ = (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + b.tv_nsec - a.tv_nsec);
    atomic_fetch_add(&g_upload_ns, d_); g_up_total_ns += d_;
    return r;
}
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_upload_ns, 0); }
uint64_t mtl_upload_gpu_ns_take(void) { return atomic_exchange(&g_upload_gpu_ns, 0); }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = g_bp_lib_n; *pso = g_bp_pso_n; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = g_bp_lib_ns; *pso_ns = g_bp_pso_ns; }   // 同步现建着色器库 / 管线的累计耗时（长帧日志按帧取差） | Cumulative time spent synchronously creating shader libraries / pipelines on the spot (the long-frame log takes the per-frame difference)
uint64_t mtl_upload_bytes(void) { return g_bp_up_bytes; }   // 贴图上传累计字节 | Cumulative texture upload bytes
static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp) {
    MTLTextureDescriptor *d = tex_desc(t, 1);
    if (!d) return nil;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(2, "建 MTLTexture 失败 %ux%u 格式 %#x 目标 %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
    res_add(mt);   // 首传与重传都在这里登记驻留（Metal 4 不自动驻留；漏登 = GPU 地址页错误） | Both first upload and re-upload register residency here (Metal 4 does not make resources resident automatically; a missed registration = GPU address page fault)
    nvn_dirty_protect(t->mem, t->L.size);
    *stamp = nvn_dirty_now();
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    unsigned layers = (t->d.target == 4 || t->d.target == 9) ? t->d.depth : t->d.target == 8 ? 6 : t->d.target == 3 ? t->d.height : 1;
    if (t->d.target == 9) layers *= 6;
    uint32_t upf; int ubpp = 0, unp = bc_unpack(t->d.format, &upf, &ubpp);   // 无 BC：解排布后在 CPU 上解压，暂存区按解压后的大小 | No BC: decompress on the CPU after deswizzling, with the staging area sized for the decompressed data
    size_t total = 0;
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
        total += unp ? (size_t)w * ubpp * h * z * layers : (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * z * layers;
    }
    g_bp_up_bytes += total;
    void *mb; uint64_t mo;
    int gdec = unp && t->d.target != 2 && bcd_fmt(t->d.format) && bcd_pso();   // 无 BC 的 BC1–5：GPU 解排布 + GPU 解压 | BC1–5 without BC: GPU deswizzle + GPU decompression
    size_t ctotal = 0;   // 压缩块的暂存大小（GPU 解压时暂存区放的是压缩块） | Staging size of the compressed blocks (with GPU decompression the staging area holds compressed blocks)
    if (gdec) for (unsigned l = 0; l < d.mipmapLevelCount; l++) { uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u); ctotal += (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * layers; }
    int use_gpu = (!unp || gdec) && g_nvn_deswz_gpu && mtl_addr((uint64_t)t->mem, t->L.size, &mb, &mo);
    if (g_nvn_deswz_gpu && !use_gpu && !unp) RED_ONCE(40, "预排布贴图存储不在内存池里，回退 CPU");
    if (use_gpu) {
        // GPU 执行时读内存池，比 CPU 上传时刻快照更接近 Switch 真机；脏页戳仍在编码前取，之后写入仍会触发重传。
        // The GPU reads the memory pool at execution time, which is closer to a real Switch than a snapshot at CPU upload time; the dirty-page stamp is still taken before encoding, and later writes still trigger a re-upload.
        up_begin();
        size_t stg_off;
        id<MTLBuffer> stg = up_alloc(gdec ? ctotal : total ? total : 16, &stg_off);
        size_t off = 0; int enc_ok = g_up_ce != nil, nb0 = g_nup_blits, nd0 = g_nup_decs;
        for (unsigned l = 0; enc_ok && l < d.mipmapLevelCount; l++) {
            uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
            size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
            for (unsigned y = 0; y < layers; y++) {
#if DM_NVN_M3
                if (nvn_deswz_gpu_encode(g_up_ce, (__bridge id<MTLBuffer>)mb, mo, &t->d, l, y, stg, stg_off + off, pitch)) { enc_ok = 0; break; }
#else
                size_t poff; id<MTLBuffer> params = up_param_alloc(&poff);   // Metal 4 无 setBytes：每次解排布的参数块从共享块里切（不逐次新建 GPU 缓冲） | Metal 4 has no setBytes: the parameter block of each deswizzle is cut from a shared block (no new GPU buffer each time)
                if (nvn_deswz_gpu_encode_m4(g_up_ce, g_up_ct, params, poff, (__bridge id<MTLBuffer>)mb, mo, &t->d, l, y, stg, stg_off + off, pitch)) { enc_ok = 0; break; }
#endif
                if (gdec) {   // 解压任务：冲刷时过了屏障再派发（见 up_flush） | Decompression job: dispatched at flush time after the barrier (see up_flush)
                    MTLPixelFormat vf = mt.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm : mt.pixelFormat;
                    id<MTLTexture> v = [mt newTextureViewWithPixelFormat:vf textureType:MTLTextureType2D levels:NSMakeRange(l, 1) slices:NSMakeRange(y, 1)];
                    size_t qoff; id<MTLBuffer> q = up_param_alloc(&qoff);
                    if (!v || !q) { enc_ok = 0; break; }
                    uint32_t *pp = (uint32_t *)((uint8_t *)q.contents + qoff); pp[0] = (uint32_t)(stg_off + off); pp[1] = (uint32_t)pitch; pp[2] = w; pp[3] = h; pp[4] = (uint32_t)bcd_fmt(t->d.format);
                    if (g_nup_decs == g_up_deccap) { g_up_deccap = g_up_deccap ? g_up_deccap * 2 : 256; g_up_decs = realloc(g_up_decs, g_up_deccap * sizeof *g_up_decs); }
                    g_up_decs[g_nup_decs++] = (up_dec){stg, q, 0, qoff, (w + 3) / 4, (h + 3) / 4, v}; [g_up_texs addObject:v]; defer_obj_unref(v);   // 视图：本次提交跑完再放，不撤驻留（驻留的是底图） | View: released after this submission finishes running, residency not revoked (what is resident is the base texture)
                } else
                up_blit_add(stg, stg_off + off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
                off += pitch * rows * z;
            }
        }
        if (enc_ok) {
            [g_up_texs addObject:mt];
            if (!g_cb) up_flush();
            return mt;
        }
        g_nup_blits = nb0; g_nup_decs = nd0;   // 撤回本张已记的拷贝 / 解压：已编的解排布只写暂存区，无害；本张改走下面 CPU 路径 | Roll back the copies / decompressions already recorded for this texture: the already-encoded deswizzle only writes the staging area and is harmless; this texture takes the CPU path below instead
        if (!g_cb) up_flush();
        RED_ONCE(41, "预排布贴图 GPU 解排布不支持格式 %#x 目标 %d，回退 CPU", t->d.format, t->d.target);
    }

    up_begin();
    id<MTLBuffer> stg = [g_dev newBufferWithLength:total ? total : 16 options:MTLResourceStorageModeShared];
    res_add(stg); [g_up_keep addObject:stg];
    size_t off = 0;
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w = MAX(t->d.width >> l, 1u), h = MAX(t->d.height >> l, 1u), z = t->d.target == 2 ? MAX(t->d.depth >> l, 1u) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < layers; y++) {
            if (unp) {   // 无 BC：先解排布到临时区（压缩块），再逐层解压进暂存区 | No BC: first deswizzle into a temporary area (compressed blocks), then decompress layer by layer into the staging area
                uint8_t *cb = malloc(pitch * rows * z); size_t up = (size_t)w * ubpp;
                if (!cb || nvn_deswizzle(&t->d, l, y, t->mem, cb, pitch)) { free(cb); RED_ONCE(9, "解排布失败 格式 %#x 目标 %d", t->d.format, t->d.target); break; }
                for (uint32_t zi = 0; zi < z; zi++) nvn_bc_decode(t->d.format, cb + zi * pitch * rows, pitch, w, h, (uint8_t *)stg.contents + off + zi * up * h, up);
                free(cb);
                up_blit_add(stg, off, up, up * h, MTLSizeMake(w, h, z), mt, y, l);
                off += up * h * z;
                continue;
            }
            if (nvn_deswizzle(&t->d, l, y, t->mem, (uint8_t *)stg.contents + off, pitch)) { RED_ONCE(9, "解排布失败 格式 %#x 目标 %d", t->d.format, t->d.target); break; }
            up_blit_add(stg, off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
            off += pitch * rows * z;
        }
    }
    up_flush();
    return mt;
}
static id<MTLTexture> tex_mtl(const tex_t *ct) {
    tex_t *t = (tex_t *)ct;
    if (!t || !t->serial) return nil;
    int packaged = t->has_layout && t->mem;
    id have = serial_get(t->serial);
    if (packaged) {
        NvPkg *pk = have;
        if (pk) {   // 已上传：自上次检查后有过客体写才查覆盖页 | Already uploaded: check the covered pages only if there has been a guest write since the last check
            uint64_t now = nvn_dirty_now();
            int tracked = nvn_dirty_tracked(t->mem, t->L.size);
            int clean = tracked && (now == pk->seen || !nvn_dirty_since(t->mem, t->L.size, pk->stamp));
            pk->seen = now;
            if (clean && atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && up_lost_take(pk->tex)) { clean = 0; uint64_t k = atomic_load(&g_up_lost_redo); if (k <= 3 || !atomic_load(&g_up_lost_n)) MLOG("上传被丢的贴图 ser=%llu 重传（累计 %llu 张，记录里还剩 %d 张）", (unsigned long long)t->serial, (unsigned long long)k, atomic_load(&g_up_lost_n)); }
            if (clean) {
                if (dirtycheck() && full_hash(t->mem, t->L.size) != pk->hash && !g_pkg_miss++)
                    MLOG("【红】脏页漏检：贴图 ser=%llu 池内容已变但未检出", t->serial);
                return pk->tex;
            }
            uint64_t st; id<MTLTexture> nt = pkg_upload(t, &st);   // 池内容变了（流式素材写进同一存储）：换新贴图，在途绘制仍读旧的 | Pool content changed (streamed assets written into the same storage): switch to a new texture; in-flight draws still read the old one
            if (nt) { id<MTLTexture> old = pk->tex; pk->tex = nt; defer_obj_release(old); res_add(nt); g_ser_gen++; pk->stamp = st; pk->seen = nvn_dirty_now(); g_pkg_reuploads++; if (dirtycheck()) pk->hash = full_hash(t->mem, t->L.size); }
            return pk->tex;
        }
        pk = [NvPkg new];
        pk->tex = pkg_upload(t, &pk->stamp);
        if (!pk->tex) return nil;
        pk->seen = nvn_dirty_now(); g_pkg_uploads++; if (dirtycheck()) pk->hash = full_hash(t->mem, t->L.size);
        serial_put(t->serial, pk);
        return pk->tex;
    }
    if (have) return have;
    if (t->d.target == 10) {   // 缓冲贴图（TARGET_BUFFER）：直接在内存池缓冲上建贴图缓冲视图（零拷贝，GPU 执行时读池里的当前内容，同 NVN 语义） | Buffer texture (TARGET_BUFFER): create the texture-buffer view directly on the memory pool buffer (zero copy; the GPU reads the pool's current content at execution time, same as NVN semantics)
        uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        void *mb; uint64_t mo;
        if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb) { RED_ONCE(36, "缓冲贴图格式 %#x 未映射", t->d.format); return nil; }
        if (!t->mem || !mtl_addr((uint64_t)t->mem, (uint64_t)t->d.width * bpb, &mb, &mo)) { RED_ONCE(37, "缓冲贴图的存储不在内存池里"); return nil; }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
        NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf];
        if (mo % al) { RED_ONCE(38, "缓冲贴图偏移 %#llx 不满足 %lu 字节对齐", (unsigned long long)mo, (unsigned long)al); return nil; }
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:(MTLPixelFormat)pf width:t->d.width resourceOptions:b.resourceOptions
                                                                                         usage:MTLTextureUsageShaderRead | (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE ? MTLTextureUsageShaderWrite : 0)];
        size_t row = ((size_t)t->d.width * bpb + al - 1) / al * al;
        id<MTLTexture> mt = [b newTextureWithDescriptor:d offset:mo bytesPerRow:row];
        if (!mt) { RED_ONCE(39, "缓冲贴图建不出：%u 元素 格式 %#x", t->d.width, t->d.format); return nil; }
        res_add(mt);
        serial_put(t->serial, mt);
        return mt;
    }
    MTLTextureDescriptor *d = tex_desc(t, 0);
    if (!d) return nil;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(2, "建 MTLTexture 失败 %ux%u 格式 %#x 目标 %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
    res_add(mt);
    serial_put(t->serial, mt);
    return mt;
}
// 内存读数行（常开、低频、封顶）：常驻内存、显卡已分配、延迟释放队列、驻留集对象数、在册内存池数 / 峰值 / 跟踪表容量、贴图重传累计、游戏堆总量 / 余量 / 最大可分配块（宿主 dm_guest_heap_stat）、离系统内存上限还有多少（仅 iOS）。
// 用途：定「没有日志的闪退」——系统按内存杀进程前不给任何通知，只有事先留下的读数能说明是哪个数一路往上涨。
// 每 300 帧由帧钩子调一次；常驻、显卡已分配变了 ≥ 64 MB，或游戏堆余量、最大可分配块变了 ≥ 16 MB（主堆平时只剩约 100 MB）才打，否则每 3000 帧打一行；一局封顶 400 行。全是整数（帧钩子在客体线程的小栈上，不用浮点格式）
// Memory readout line (always on, low frequency, capped): resident memory, GPU allocated, deferred release queue, residency set object count, registered memory pool count / peak / tracking table capacity, cumulative texture re-uploads, game heap total / free / largest allocatable block (host dm_guest_heap_stat), and how far from the system memory limit (iOS only).
// Purpose: to pin down "crashes with no log" -- the system gives no notice before killing a process for memory, and only readouts left beforehand can show which number kept climbing.
// Called by the frame hook every 300 frames; printed when resident or GPU allocated changed by >= 64 MB, or game heap free space or largest allocatable block changed by >= 16 MB (the main heap normally has only about 100 MB left), otherwise one line every 3000 frames; capped at 400 lines per run. All integers (the frame hook runs on the small stack of a guest thread, so no floating-point formatting)
static void res_census(unsigned long long frame, unsigned long long rss_mb) {
    @autoreleasepool {
        unsigned long nl[2] = {0, 0}, nr[2] = {0, 0}; unsigned long long bl[2] = {0, 0}, br[2] = {0, 0};   // [0] 贴图 [1] 缓冲；l = 在驻留集里，r = 已退役未销毁 | [0] textures [1] buffers; l = in the residency set, r = retired but not destroyed
        pthread_mutex_lock(&g_res_mu);
        for (id o in g_res_ever.allObjects) { int k = [o conformsToProtocol:@protocol(MTLBuffer)] ? 1 : 0; unsigned long long b = (unsigned long long)[(id<MTLResource>)o allocatedSize];
            if ([g_res_objects containsObject:o]) { nl[k]++; bl[k] += b; } else { nr[k]++; br[k] += b; } }
        pthread_mutex_unlock(&g_res_mu);
        MLOG("驻留普查 至帧 %llu：常驻 %llu MB，显卡已分配 %llu MB；驻留集里 贴图 %lu 个 %llu MB、缓冲 %lu 个 %llu MB；已退役未销毁 贴图 %lu 个 %llu MB、缓冲 %lu 个 %llu MB", frame, rss_mb, (unsigned long long)(g_dev.currentAllocatedSize >> 20),
             nl[0], bl[0] >> 20, nl[1], bl[1] >> 20, nr[0], br[0] >> 20, nr[1], br[1] >> 20);
    }
}
unsigned long long mtl_gpu_allocated_mb(void) { id<MTLDevice> d = g_dev; return d ? (unsigned long long)(d.currentAllocatedSize >> 20) : 0; }   // 显卡已分配（图形层还没起时 0）；任何线程可调 | GPU allocated (0 before the graphics layer starts); callable from any thread
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb, int heap_ok, unsigned long long heap_mb, unsigned long long heap_free_mb, unsigned long long heap_maxblk_kb) {
    if (g_res_census > 0) res_census(frame, rss_mb);
    static unsigned long long last_rss, last_gpu, last_frame, last_hfree, last_hmax; static int lines; unsigned long long gpu = (unsigned long long)(g_dev.currentAllocatedSize >> 20);
    unsigned long long dr = rss_mb > last_rss ? rss_mb - last_rss : last_rss - rss_mb, dg = gpu > last_gpu ? gpu - last_gpu : last_gpu - gpu;
    unsigned long long dh = heap_free_mb > last_hfree ? heap_free_mb - last_hfree : last_hfree - heap_free_mb, hmax_mb = heap_maxblk_kb >> 10, dm = hmax_mb > last_hmax ? hmax_mb - last_hmax : last_hmax - hmax_mb;
    static int all = -1; if (all < 0) all = getenv("DM_MEM_STAT_ALL") != NULL;   // 量测用：每次都打、不封顶 | For measurement: printed every time, uncapped
    if (!all && (lines >= 400 || (lines && dr < 64 && dg < 64 && dh < 16 && dm < 16 && frame - last_frame < 3000))) return; lines++; last_rss = rss_mb; last_gpu = gpu; last_frame = frame; last_hfree = heap_free_mb; last_hmax = hmax_mb;
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    pthread_mutex_lock(&g_res_mu); unsigned long nr = (unsigned long)g_res_objects.count; pthread_mutex_unlock(&g_res_mu);
    int live = 0, peak = 0, cap = 0; nvn_dirty_stat(&live, &peak, &cap);
    char avail[32] = "无";   // os_proc_available_memory：本进程离被系统按内存终止还差多少，只有 iOS 有；Mac 宿主写「无」 | os_proc_available_memory: how far this process is from being terminated by the system for memory, iOS only; the Mac host writes "none"
#if TARGET_OS_IPHONE
    snprintf(avail, sizeof avail, "%llu MB", (unsigned long long)(os_proc_available_memory() >> 20));
#endif
    MLOG("内存 至帧 %llu：常驻 %llu MB，显卡已分配 %llu MB，延迟释放队列 %lu，驻留集 %lu 个，内存池 %d 个（峰值 %d / 跟踪表容量 %d），贴图重传累计 %llu 次；游戏堆 %llu MB，余量 %llu MB，最大可分配块 %llu KB%s；离系统内存上限还有 %s", frame, rss_mb, gpu, nd, nr, live, peak, cap, (unsigned long long)g_pkg_reuploads,
        heap_mb, heap_free_mb, heap_maxblk_kb, heap_ok ? "" : "（未读到）", avail);
}
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_uploads; *reuploads = g_pkg_reuploads; }
uint64_t mtl_pkg_miss(void) { return g_pkg_miss; }
void mtl_tex_release(tex_t *t) { if (t->serial) serial_del(t->serial); }

// ---------- 渲染通道：SetRenderTargets 开新通道；通道里第一条命令之前的清除并进 load 动作 ----------
// ---------- Render passes: SetRenderTargets opens a new pass; clears before the first command in the pass are folded into the load action ----------
static struct {
    p_set_rt rt; int has_rt;
    uint8_t clear_c[8]; MTLClearColor cc[8];
    uint8_t clear_d, clear_s; double cd; uint32_t cs;
    p_scissor sc; int has_sc;
} R;

// ---------- 零源跳过 ----------
// 着色器事实表（静态证明过的键）：「它采样的贴图全 0 且系数有限 ⇒ 输出全 0」。系数 = 片元阶段常量缓冲寄存器 ubo_slot 的第 vec 个向量第 comp 分量。
//   4a819818b8e489c5 ps_outline_blur_p：输出 = (同一张贴图沿行 / 列若干次采样之和) × fp_c7._m0[1].x（fp_c7 = cbuf reg 4），对采样值齐次线性、无常数项
//   （证明出处：dmsh_b/<链版本>/4a819818b8e489c5_b.metal，逐行读过）。以后别的着色器证过了往里加。
// ---------- Zero-source skip ----------
// Shader fact table (statically proven keys): 'the textures it samples are all 0 and the coefficient is finite ⇒ the output is all 0'. Coefficient = component comp of vector vec of fragment-stage constant buffer register ubo_slot.
//   4a819818b8e489c5 ps_outline_blur_p: output = (sum of several samples of the same texture along a row / column) × fp_c7._m0[1].x (fp_c7 = cbuf reg 4), homogeneous linear in the sampled values, no constant term
//   (source of the proof: dmsh_b/<chain version>/4a819818b8e489c5_b.metal, read line by line). Add other shaders here once they are proven.
typedef struct { char key[17]; int ubo_slot, vec, comp; } zskip_fact;
static zskip_fact g_zskip_facts[16] = {{"4a819818b8e489c5", 4, 1, 0}};
static int g_zskip_nf = 1;
void zskip_test_add(const char *key, int ubo_slot, int vec, int comp) {
    if (!key || g_zskip_nf >= 16) return;
    snprintf(g_zskip_facts[g_zskip_nf].key, sizeof g_zskip_facts[0].key, "%s", key);
    g_zskip_facts[g_zskip_nf].ubo_slot = ubo_slot; g_zskip_facts[g_zskip_nf].vec = vec; g_zskip_facts[g_zskip_nf].comp = comp; g_zskip_nf++;
}
int zskip_test_count(void) { return g_zskip_count; }
static int g_zskip_env = -1;   // DM_NVN_ZSKIP 的缓存（−1 = 未读）。本函数每次绘制都调，getenv 在提交路径占过约 8%；单测运行中切开关后把它置回 −1 | Cache of DM_NVN_ZSKIP (−1 = not read). This function is called on every draw, and getenv once took about 8% of the submit path; unit tests set it back to −1 after toggling the switch mid-run
static int zskip_on(void) { if (g_zskip_env < 0) { const char *e = getenv("DM_NVN_ZSKIP"); g_zskip_env = !e || strcmp(e, "0"); } return g_zskip_env && tbdr_on(); }
static uint64_t g_zskip_gen = 1;   // 全零集合只经 zskip_add / zskip_clear 增改：代号没变时，刚删过的序号不会又在集合里 | The all-zero set is changed only via zskip_add / zskip_clear: with the generation unchanged, an index just removed will not be in the set again
static void zskip_clear(void) { g_zskip_n = 0; memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok); g_zskip_gen++; }
static void zskip_del(uint64_t ser) { for (int i = 0; i < g_zskip_n;) if (g_zskip_zero[i].ser == ser) g_zskip_zero[i] = g_zskip_zero[--g_zskip_n]; else i++; }
static int zskip_has(uint64_t ser, int lv, int sl) { for (int i = 0; i < g_zskip_n; i++) if (g_zskip_zero[i].ser == ser && g_zskip_zero[i].level == lv && g_zskip_zero[i].slice == sl) return 1; return 0; }
static void zskip_add(uint64_t ser, int lv, int sl) { if (!ser || zskip_has(ser, lv, sl) || g_zskip_n >= 4096) return; g_zskip_zero[g_zskip_n++] = (zskip_zero){ser, (uint16_t)lv, (uint16_t)sl}; g_zskip_gen++; }
static int zskip_zero_clear(int i) { return R.clear_c[i] && R.cc[i].red == 0 && R.cc[i].green == 0 && R.cc[i].blue == 0 && R.cc[i].alpha == 0; }
static void zskip_commit_clears(void) { for (int i = 0; i < 8; i++) if (g_zskip_cand_ok[i]) zskip_add(g_zskip_cand[i].ser, g_zskip_cand[i].level, g_zskip_cand[i].slice); }
static int zskip_target_zero(const tex_t *t, const view_t *v, int hv) { int lv = hv && v->set_levels ? v->base_level : 0, sl = hv && v->set_layers ? v->min_layer : 0; return t && zskip_has(t->serial, lv, sl); }
#ifdef NVN_RT_RANGE_TEST
static int g_rr_checking;
static void rr_begin(void), rr_finish(int ranged);
static void rr_snap(const p_draw *, const p_draw_idx *);
static void rr_reset(void);
#endif
static RENC g_enc;
// 当前渲染通道的目标信息：rp_begin 时由附件定一次，通道内各绘制共用（管线键、深度模板、视口 / 剪裁都只依赖它）
// Target info of the current render pass: determined once from the attachments at rp_begin and shared by all draws in the pass (pipeline key, depth-stencil, viewport / scissor all depend only on it)
static struct { uint32_t cfmt[8], dfmt, sfmt, samples; int hd, hs; NSUInteger tw, th; uint64_t serial, reg_gen, ser_gen, range_dirty; int range_external, range_depth_nonro; __unsafe_unretained id<MTLTexture> range_color, range_depth; } g_pass;
static id<MTLRenderPipelineState> g_enc_pso; static id<MTLDepthStencilState> g_enc_dss;
// 本编码器上已设的固定功能状态（同值不重发，与 g_enc_pso 同一生命周期：开新渲染编码器时作废）。valid 位：0 视口 1 剪裁 2 剔除 3 朝向 4 填充 5 深度偏移 6 混合色 7 模板参考
// Fixed-function state already set on this encoder (not resent if unchanged; same lifetime as g_enc_pso: invalidated when a new render encoder is opened). valid bits: 0 viewport 1 scissor 2 cull 3 winding 4 fill 5 depth bias 6 blend color 7 stencil reference
static struct { uint32_t valid; MTLViewport vp; MTLScissorRect sc; int cull, wind, fill; float db[3], bc[4]; uint32_t sref; } g_enc_fx;   // 本编码器上已设的（相同不重设） | Already set on this encoder (not set again if identical)
static int g_enc_vis, g_enc_has_vis;   // 本编码器当前的可见性计数模式（0 关 / 槽号 + 1）、是否挂了可见性缓冲 | This encoder's current visibility count mode (0 off / slot number + 1), and whether a visibility buffer is attached
static int g_pso_dirty = 1, g_dss_dirty = 1;   // 回放中改了管线 / 深度模板相关状态 | Pipeline / depth-stencil related state was changed during replay
static uint64_t g_stat_args_enc, g_stat_args_reuse;   // 参数缓冲：新编 / 复用次数 | Argument buffer: new-encode / reuse counts
static uint64_t g_stat_draw, g_stat_pass, g_bw_load, g_bw_store, g_pass_full_rw, g_udc_saved_bytes;   // 带宽账：附件读入 / 写出字节、全量读写通道数 | Bandwidth ledger: attachment load / store bytes, number of full load-store passes
// DM_NVN_PASSDUMP：逐条记通道附件 / 采样 / 出图（贴图按序号），供离线做数据流分析；P / D / R / S 行的通道号一致（= 开通道后的 g_stat_pass）
// DM_NVN_PASSDUMP=帧号 或 起-止（含两端）
// DM_NVN_PASSDUMP: records pass attachments / samples / presents one by one (textures by serial) for offline data-flow analysis; the pass numbers of P / D / R / S lines agree (= g_stat_pass after the pass is opened)
// DM_NVN_PASSDUMP=frame number, or start-end (inclusive at both ends)
static int passdump(void) {
#ifdef NVN_RT_RANGE_TEST
    if (g_rr_checking) return 0;
#endif
    static long a0 = -2, a1; if (a0 == -2) { const char *e = getenv("DM_NVN_PASSDUMP"); a0 = e ? atol(e) : -1; a1 = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : a0; }
    long f = (long)atomic_load(&g_cur_frame); return a0 >= 0 && f >= a0 && f <= a1;
}
#define PD(...) do { if (passdump()) { fprintf(stderr, "[pass] F%llu ", (unsigned long long)atomic_load(&g_cur_frame)); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static void blit_note(void) { if (g_enc) PD("E %llu 拷贝 / 缓冲更新结束渲染通道", g_stat_pass); }
// DM_GFXUSE=<文件>：选角页稳定后（首次 OpenPanel MainMenuButtonRibbon 之后 1 秒起，需 DM_BOOTPROF=1）连续 30 帧里
// GPU 实际读取的资源在客体内存中的区间（贴图存储、UBO / SSBO、顶点 / 索引缓冲），按（种类, 地址, 长度）去重，首见时记一行。
// 供与 inflate 输出区间求交（哪些选角前解压的数据选角页真用到）。列：kind  guest_addr  len  first_frame  tex_id  fmt  w×h
// DM_GFXUSE=<file>: once the character select screen is stable (starting 1 second after the first OpenPanel MainMenuButtonRibbon, requires DM_BOOTPROF=1), over 30 consecutive frames,
// the ranges in guest memory of the resources the GPU actually reads (texture storage, UBO / SSBO, vertex / index buffers), deduplicated by (kind, address, length), one line recorded on first sight.
// For intersecting with the inflate output ranges (which data decompressed before character select is really used by the character select screen). Columns: kind  guest_addr  len  first_frame  tex_id  fmt  w×h
__attribute__((weak)) uint64_t dm_prof_ribbon_ns(void) { return 0; }   // diag.c 提供强定义；未合入时恒 0 → 时间窗不开，结束时报红 | diag.c provides the strong definition; when not merged it is always 0 → the time window does not open, and a failure is reported at the end
__attribute__((weak)) void dm_shot_written(const char *path, int ok) { (void)path; (void)ok; }
__attribute__((weak)) int dm_guest_heap_stat(unsigned long long *t, unsigned long long *f, unsigned long long *m) { (void)t; (void)f; (void)m; return 0; }   // hle_svc.c 提供强定义（游戏主堆读数）；图形单测不链宿主时读不到 | hle_svc.c provides the strong definition (game main heap readout); the graphics unit tests do not link the host and cannot read it
__attribute__((weak)) void dm_guest_frame_tick(void) {}   // hle_os.c 提供强定义（DM_DETCLOCK 确定性客体时钟）；图形单测不链宿主时为空操作   // diag.c 提供强定义（DM_TICK_HOLD=E:SHOT 解冻）；图形单测不链宿主时为空操作 | hle_os.c provides the strong definition (DM_DETCLOCK deterministic guest clock); a no-op when graphics unit tests do not link the host   // diag.c provides the strong definition (DM_TICK_HOLD=E:SHOT unfreeze); a no-op when graphics unit tests do not link the host
__attribute__((weak)) int dm_detclock_enabled(void) { return 0; }   // 同上：宿主 hle_os.c 提供强定义；图形单测不链宿主时确定性时钟视为关闭 | Same as above: the host's hle_os.c provides the strong definition; when graphics unit tests do not link the host, the deterministic clock is treated as off
__attribute__((weak)) uint64_t guest_ns(void) { return 0; }          // 同上：仅在确定性时钟开启时被读，图形单测里不会走到 | Same as above: read only when the deterministic clock is on; never reached in graphics unit tests
enum { USE_OFF, USE_WAIT, USE_REC, USE_DONE };
static int g_use = -1; static FILE *g_use_f; static long g_use_f0, g_use_rows; static nvn_hmap g_use_seen = {.klen = 24};
#define USE_FRAMES 30
static void use_exit(void) {
    if (g_use == USE_WAIT) fprintf(stderr, "[dm] 【红】DM_GFXUSE 时间窗始终没开：dm_prof_ribbon_ns() = %llu（需 DM_BOOTPROF=1 且宿主含面板打点）\n", (unsigned long long)dm_prof_ribbon_ns());
    else if (g_use == USE_REC) fprintf(stderr, "[dm] 【红】DM_GFXUSE 只记了 %ld 帧（不足 %d）即退出，文件未完整\n", (long)atomic_load(&g_cur_frame) - g_use_f0, USE_FRAMES);
    if (g_use_f) fflush(g_use_f);
}
static void use_tick(void) {   // 每次出图调用 | Called on every present
    if (g_use < 0) { const char *e = getenv("DM_GFXUSE"); g_use = USE_OFF;
        if (e) { g_use_f = fopen(e, "w"); if (!g_use_f) { MLOG("【红】DM_GFXUSE 打不开 %s", e); return; }
            fprintf(g_use_f, "kind\tguest_addr\tlen\tfirst_frame\ttex_id\tfmt\twxh\n"); fflush(g_use_f); g_use = USE_WAIT; atexit(use_exit);
            MLOG("[gfxuse] 等面板事件：首次 OpenPanel MainMenuButtonRibbon 后 1 秒起记 %d 帧 → %s", USE_FRAMES, e); } }
    long f = (long)atomic_load(&g_cur_frame);
    if (g_use == USE_WAIT) {
        uint64_t t = dm_prof_ribbon_ns();
        if (t && now_ns() - t >= 1000000000ull) { g_use = USE_REC; g_use_f0 = f; MLOG("[gfxuse] 开始记录：帧 %ld 起 %d 帧", f, USE_FRAMES); }
    } else if (g_use == USE_REC && f - g_use_f0 >= USE_FRAMES) {
        g_use = USE_DONE; fclose(g_use_f); g_use_f = NULL;
        MLOG("[gfxuse] 记录完成：帧 %ld–%ld，去重后 %ld 个区间", g_use_f0, f - 1, g_use_rows);
    }
}
static __attribute__((always_inline)) inline void use_rec(const char *kind, uint64_t addr, uint64_t len, int tid, const tex_t *t) {
    if (g_use != USE_REC || !addr || !len) return;
    uint64_t k[3] = {addr, len, (uint64_t)(uint8_t)kind[0]};
    if (nvn_hm_get(&g_use_seen, k)) return;
    nvn_hm_put(&g_use_seen, k, (void *)1);
    g_use_rows++;
    if (t) fprintf(g_use_f, "%s\t%#llx\t%llu\t%lld\t%d\t%#x\t%ux%u\n", kind, (unsigned long long)addr, (unsigned long long)len, (long long)atomic_load(&g_cur_frame), tid, t->d.format, t->d.width, t->d.height);
    else fprintf(g_use_f, "%s\t%#llx\t%llu\t%lld\t\t\t\n", kind, (unsigned long long)addr, (unsigned long long)len, (long long)atomic_load(&g_cur_frame));
}
static uint64_t att_bytes(id<MTLTexture> t, NSUInteger level) {
    if (!t) return 0;
    NSUInteger w = MAX(t.width >> level, 1), h = MAX(t.height >> level, 1);
    NSUInteger bpp = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? 5 : t.pixelFormat == MTLPixelFormatRGBA16Float || t.pixelFormat == MTLPixelFormatRG32Float ? 8 :
                     t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR8Unorm || t.pixelFormat == MTLPixelFormatStencil8 ? 1 :
                     t.pixelFormat == MTLPixelFormatRG8Unorm || t.pixelFormat == MTLPixelFormatR16Float ? 2 : 4;
    return (uint64_t)w * h * bpp * t.sampleCount;
}
// 渲染附件选中的是某个mip，MTLTexture.width/height仍表示基级。空附件不参与边界。
// The render attachment selects a particular mip, while MTLTexture.width/height still describe the base level. Empty attachments do not take part in the bounds.
static NSUInteger mip_extent(NSUInteger size, NSUInteger level) {
    return level < sizeof(size) * 8 ? MAX(size >> level, 1) : 1;
}
static void pass_dims(RPD *pd, NSUInteger nc, NSUInteger *w, NSUInteger *h) {
    *w = *h = 0;
    for (NSUInteger i = 0; i < nc + 2; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < nc ? pd.colorAttachments[i] : i == nc ? pd.depthAttachment : pd.stencilAttachment;
        if (!a.texture) continue;
        NSUInteger aw = mip_extent(a.texture.width, a.level), ah = mip_extent(a.texture.height, a.level);
        *w = *w ? MIN(*w, aw) : aw; *h = *h ? MIN(*h, ah) : ah;
    }
    if (!*w) *w = *h = 1;
}

// ---------- DM_NVN_PASSGPU=<文件>：逐通道 GPU 时间（阶段边界时间戳计数器） ----------
// 每个渲染通道采 4 个点（顶点起止、片元起止），计算通道采 2 个点；命令缓冲完成后解析，按「帧内第几个通道」累计。
// 每行：frame idx kind gpu_ms inc_ms w h fmt0 dfmt load draws t0_ns t1_ns（kind：R 渲染 / C 计算；load：各颜色附件 + 深度的 L 读入 / C 清除）
// TBDR 上通道间流水重叠（后一通道的顶点阶段早于前一通道片元结束）：gpu_ms = 本通道起止跨度（含排队），不可相加；
// 归因用时间线增量 inc_ms = t1 − max(t0, 此前已完成通道的最晚 t1)，各通道增量之和 = GPU 忙的总时长（命令缓冲按提交序完成）
// ---------- DM_NVN_PASSGPU=<file>: per-pass GPU time (stage-boundary timestamp counters) ----------
// Each render pass samples 4 points (vertex start/end, fragment start/end), a compute pass 2 points; resolved after the command buffer completes and accumulated by 'index of the pass within the frame'.
// Each line: frame idx kind gpu_ms inc_ms w h fmt0 dfmt load draws t0_ns t1_ns (kind: R render / C compute; load: L load / C clear for each color attachment + depth)
// On TBDR passes overlap in the pipeline (the later pass's vertex stage is earlier than the end of the earlier pass's fragment stage): gpu_ms = this pass's start-to-end span (including queueing) and cannot be summed;
// attribution uses the timeline increment inc_ms = t1 − max(t0, latest t1 of the passes completed so far); the sum of the increments of all passes = total time the GPU is busy (command buffers complete in submission order)
typedef struct { uint64_t frame, pass; int idx, kind, w, h, draws, slot; uint32_t cfmt0, dfmt; char load[12]; } pg_pass;   // pass：全局通道号（同 DM_NVN_PASSDUMP 各行的通道号，供两份记录对接） | pass: global pass number (same as the pass number in the DM_NVN_PASSDUMP lines, for joining the two records)
#define PG_SLOTS 4096   // 计数采样缓冲上限 32768 字节 = 4096 个时间戳（本机报错实测） | Counter sample buffer limit 32768 bytes = 4096 timestamps (measured from the error on this machine)
#define PG_RING 8
static int g_pg = -1; static FILE *g_pg_f;
#if DM_NVN_M3
static id<MTLCounterSampleBuffer> g_pg_buf[PG_RING];
#define PG_RSLOTS 4   // 渲染通道采样点：顶点起止、片元起止（阶段边界计数采样） | Render pass sample points: vertex start/end, fragment start/end (stage boundary counter sampling)
#else
static id<MTL4CounterHeap> g_pg_buf[PG_RING]; static double g_pg_ns_tick = 1;   // Metal 4：时间戳计数堆，读数是 GPU 时钟周期，按设备时间戳频率换成纳秒 | Metal 4: timestamp counter heap; readings are GPU clock ticks, converted to nanoseconds by the device timestamp frequency
#define PG_RSLOTS 2   // 渲染通道采样点：开编码器后（顶点阶段之前）、收编码器前（片元阶段之后） | Render pass sample points: after opening the encoder (before the vertex stage) and before ending it (after the fragment stage)
static int g_pg_copen = -1;   // 计算通道：开着的那个的结束采样点 | Compute pass: end sample point of the currently open one
#endif
static int g_pg_b; static uint32_t g_pg_used;
static pg_pass *g_pg_pass; static int g_pg_np, g_pg_cap, g_pg_open = -1;
static uint64_t g_pg_frame = ~0ull; static int g_pg_idx;
static pthread_mutex_t g_pg_mu = PTHREAD_MUTEX_INITIALIZER;
static inline void args_dirty_all(void);
typedef struct { double sum, last; uint64_t n; pg_pass meta; } pg_stat;
static pg_stat g_pg_st[256]; static uint64_t g_pg_rep_frame;
static int pg_on(void) {
    if (g_pg >= 0) return g_pg;
    const char *e = getenv("DM_NVN_PASSGPU"); g_pg = 0;
    if (!e) return 0;
#if DM_NVN_M3
    if (![g_dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) { MLOG("【红】DM_NVN_PASSGPU：本设备不支持阶段边界计数采样"); return 0; }
    id<MTLCounterSet> ts = nil; for (id<MTLCounterSet> c in g_dev.counterSets) if ([c.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = c;
    if (!ts) { MLOG("【红】DM_NVN_PASSGPU：没有时间戳计数器集"); return 0; }
    MTLCounterSampleBufferDescriptor *d = [MTLCounterSampleBufferDescriptor new];
    d.counterSet = ts; d.storageMode = MTLStorageModeShared; d.sampleCount = PG_SLOTS;
    for (int i = 0; i < PG_RING; i++) { NSError *er = nil; g_pg_buf[i] = [g_dev newCounterSampleBufferWithDescriptor:d error:&er]; if (!g_pg_buf[i]) { MLOG("【红】计数采样缓冲建不出：%s", er.localizedDescription.UTF8String); return 0; } }
#else
    MTL4CounterHeapDescriptor *hd = [MTL4CounterHeapDescriptor new]; hd.type = MTL4CounterHeapTypeTimestamp; hd.count = PG_SLOTS;
    for (int i = 0; i < PG_RING; i++) { NSError *er = nil; g_pg_buf[i] = [g_dev newCounterHeapWithDescriptor:hd error:&er]; if (!g_pg_buf[i]) { MLOG("【红】时间戳计数堆建不出：%s", er.localizedDescription.UTF8String ?: "?"); return 0; } }
    { uint64_t fq = [g_dev queryTimestampFrequency]; g_pg_ns_tick = fq ? 1e9 / (double)fq : 1; }
#endif
    if (!(g_pg_f = fopen(e, "w"))) { MLOG("【红】DM_NVN_PASSGPU 打不开 %s", e); return 0; }
    fprintf(g_pg_f, "frame\tidx\tkind\tgpu_ms\tinc_ms\tw\th\tfmt0\tdfmt\tload\tdraws\tt0_ns\tt1_ns\tpass\n");
    MLOG("[passgpu] 逐通道 GPU 计时 → %s", e);
    return g_pg = 1;
}
static void pg_sub_begin(uint64_t fence) {   // 本次提交用环里第 fence % PG_RING 块；先等 PG_RING 次之前用它的提交完成 | This submission uses block fence % PG_RING of the ring; first wait for the submission that used it PG_RING submissions ago to complete
    if (!pg_on()) return;
    if (fence > PG_RING) mtl_wait(fence - PG_RING, UINT64_MAX);
    g_pg_b = (int)(fence % PG_RING); g_pg_used = 0; g_pg_np = 0; g_pg_open = -1;
}
static pg_pass *pg_new(int kind, int nslot) {
    if (!pg_on() || g_pg_used + nslot > PG_SLOTS) return NULL;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_pg_frame) { g_pg_frame = f; g_pg_idx = 0; }
    if (g_pg_np == g_pg_cap) { g_pg_cap = g_pg_cap ? g_pg_cap * 2 : 128; g_pg_pass = realloc(g_pg_pass, g_pg_cap * sizeof *g_pg_pass); }
    pg_pass *p = &g_pg_pass[g_pg_np++]; memset(p, 0, sizeof *p);
    p->frame = f; p->pass = g_stat_pass; p->idx = g_pg_idx++; p->kind = kind; p->slot = (int)g_pg_used; p->draws = -(int)g_stat_draw;
    g_pg_used += nslot;
    return p;
}
// 开渲染通道前记通道信息（附件、尺寸、读入方式）；旧接口在描述符上挂阶段边界采样，Metal 4 在开编码器后 / 收编码器前写时间戳（pg_render_begin / pg_render_finish）。
// 改到 Metal 4时描述符没了采样附件，这里整段被短路、提交时的回读调用也删了，两种接口都没再出过数，这次补齐
// Record the pass info (attachments, size, load actions) before opening a render pass; the older API attaches stage-boundary sampling to the descriptor, while Metal 4 writes timestamps after opening / before ending the encoder (pg_render_begin / pg_render_finish).
// When we moved to Metal 4 the descriptor lost its sampling attachment, this whole block was short-circuited and the readback at submit time was removed too, so neither API produced numbers any more; this restores both
static void pg_render(RPD *pd, NSUInteger nc) {
    pg_pass *p = pg_new(0, PG_RSLOTS); if (!p) return;
#if DM_NVN_M3
    MTLRenderPassSampleBufferAttachmentDescriptor *a = pd.sampleBufferAttachments[0];
    a.sampleBuffer = g_pg_buf[g_pg_b];
    a.startOfVertexSampleIndex = p->slot; a.endOfVertexSampleIndex = p->slot + 1; a.startOfFragmentSampleIndex = p->slot + 2; a.endOfFragmentSampleIndex = p->slot + 3;
#endif
    id<MTLTexture> t0 = nil; int k = 0;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        if (!t0) t0 = t; if (k < 10) p->load[k++] = pd.colorAttachments[i].loadAction == MTLLoadActionClear ? 'C' : 'L'; }
    id<MTLTexture> dt = pd.depthAttachment.texture;
    if (dt) { if (k < 10) { p->load[k++] = '|'; p->load[k++] = pd.depthAttachment.loadAction == MTLLoadActionClear ? 'C' : 'L'; } p->dfmt = (uint32_t)dt.pixelFormat; }
    if (!t0) t0 = dt;
    NSUInteger pw, ph; pass_dims(pd, nc, &pw, &ph);
    p->w = (int)pw; p->h = (int)ph; p->cfmt0 = t0 && t0 != dt ? (uint32_t)t0.pixelFormat : 0;
    g_pg_open = g_pg_np - 1;
}
static void pg_render_begin(RENC e) {   // 开渲染编码器之后：写通道起点（第一个绘制的顶点阶段之前） | After opening the render encoder: write the pass start point (before the vertex stage of the first draw)
#if DM_NVN_M3
    (void)e;
#else
    if (g_pg_open >= 0 && e) [e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageVertex intoHeap:g_pg_buf[g_pg_b] atIndex:(NSUInteger)g_pg_pass[g_pg_open].slot];
#endif
}
static void pg_render_finish(RENC e) {   // 收渲染编码器之前：写通道终点（全部绘制的片元阶段之后） | Before ending the render encoder: write the pass end point (after the fragment stage of all draws)
#if DM_NVN_M3
    (void)e;
#else
    if (g_pg_open >= 0 && e) [e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageFragment intoHeap:g_pg_buf[g_pg_b] atIndex:(NSUInteger)g_pg_pass[g_pg_open].slot + 1];
#endif
}
static id<MTLComputeCommandEncoder> pg_compute(void) {
    pg_pass *p = pg_new(1, 2);
    ser_gate();
    if (!p) { args_dirty_all(); return enc_c_begin(NULL); }
#if !DM_NVN_M3
    p->draws = 0; args_dirty_all();
    CENC ce = enc_c_begin(NULL);   // Metal 4：开编码器后写起点，收编码器前写终点（pg_compute_end） | Metal 4: write the start point after opening the encoder, write the end point before ending it (pg_compute_end)
    [ce writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_pg_buf[g_pg_b] atIndex:(NSUInteger)p->slot]; g_pg_copen = p->slot + 1;
    return (id<MTLComputeCommandEncoder>)ce;
#else
    MTLComputePassDescriptor *cd = [MTLComputePassDescriptor computePassDescriptor];
    cd.sampleBufferAttachments[0].sampleBuffer = g_pg_buf[g_pg_b];
    cd.sampleBufferAttachments[0].startOfEncoderSampleIndex = p->slot; cd.sampleBufferAttachments[0].endOfEncoderSampleIndex = p->slot + 1;
    p->draws = 0;
    args_dirty_all();
    return enc_c_begin(cd);
#endif
}
static void pg_compute_end(id e) {   // 收 pg_compute 开的计算编码器之前调 | Called before ending the compute encoder opened by pg_compute
#if DM_NVN_M3
    (void)e;
#else
    if (g_pg_copen >= 0 && e) { [(CENC)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_pg_buf[g_pg_b] atIndex:(NSUInteger)g_pg_copen]; g_pg_copen = -1; }
#endif
}
static void pg_end_render(void) { if (g_pg_open >= 0) { g_pg_pass[g_pg_open].draws += (int)g_stat_draw; g_pg_open = -1; } }
static void pg_commit(id fb) {   // 把本次提交的通道表交给完成回调解析；fb = FB_ON(o, g_cb)（旧接口是命令缓冲的完成回调，Metal 4 是提交选项的反馈回调） | Hands this submission's pass table to the completion callback for parsing; fb = FB_ON(o, g_cb) (on the legacy API the command buffer completion handler, on Metal 4 the commit options feedback handler)
    if (!g_pg || !g_pg_np) return;
    int n = g_pg_np; pg_pass *ps = malloc(n * sizeof *ps); memcpy(ps, g_pg_pass, n * sizeof *ps);
    __typeof__(g_pg_buf[0]) buf = g_pg_buf[g_pg_b]; uint32_t used = g_pg_used;
#if !DM_NVN_M3
    double ns_tick = g_pg_ns_tick;
#endif
    [fb FB_SEL:^(FB_ARG c) {
        (void)c;
        NSData *d = [buf resolveCounterRange:NSMakeRange(0, used)];
        uint64_t *t = d ? calloc(used ? used : 1, sizeof *t) : NULL;   // 各采样点的时刻（纳秒），0 = 无效 | Time of each sample point (nanoseconds), 0 = invalid
#if DM_NVN_M3
        const MTLCounterResultTimestamp *r = d.bytes;
        for (uint32_t k = 0; t && k < used && (k + 1) * sizeof *r <= d.length; k++) t[k] = r[k].timestamp == MTLCounterErrorValue ? 0 : r[k].timestamp;   // 本机实测与 CPU 同为 ns | Measured on this machine to be ns, same as the CPU
#else
        const MTL4TimestampHeapEntry *r = d.bytes;
        for (uint32_t k = 0; t && k < used && (k + 1) * sizeof *r <= d.length; k++) t[k] = (uint64_t)(r[k].timestamp * ns_tick);
#endif
        pthread_mutex_lock(&g_pg_mu);
        for (int i = 0; t && i < n; i++) {
            pg_pass *p = &ps[i]; uint64_t a, b;
            #define OK(x) ((x) != 0)
#if DM_NVN_M3
            if (p->kind == 0) { a = OK(t[p->slot]) ? t[p->slot] : t[p->slot + 2]; b = OK(t[p->slot + 3]) ? t[p->slot + 3] : t[p->slot + 1]; }
            else { a = t[p->slot]; b = t[p->slot + 1]; }
#else
            a = t[p->slot]; b = t[p->slot + 1];
#endif
            if (!OK(a) || !OK(b) || b < a) continue;
            #undef OK
            static uint64_t prev_end;   // 此前已完成通道的最晚结束时刻（完成回调按提交序串行） | Latest end time of the passes completed so far (completion callbacks are serialized in submission order)
            double ms = (b - a) / 1e6;   // 时间戳与 CPU 同为 ns（本机 sampleTimestamps 实测 1:1） | Timestamps are in ns, same as the CPU (measured 1:1 via sampleTimestamps on this machine)
            double inc = b > prev_end ? (b - (a > prev_end ? a : prev_end)) / 1e6 : 0;
            if (b > prev_end) prev_end = b;
            fprintf(g_pg_f, "%llu\t%d\t%c\t%.4f\t%.4f\t%d\t%d\t%u\t%u\t%s\t%d\t%llu\t%llu\t%llu\n", (unsigned long long)p->frame, p->idx, p->kind ? 'C' : 'R', ms, inc,
                    p->w, p->h, p->cfmt0, p->dfmt, p->load, p->draws, (unsigned long long)a, (unsigned long long)b, (unsigned long long)p->pass);
            if (p->idx < 256 && p->frame > 300) { pg_stat *st = &g_pg_st[p->idx]; st->sum += inc; st->n++; st->last = inc; st->meta = *p; }
            if (p->frame >= g_pg_rep_frame + 300 && p->frame > 300) {   // 每 300 帧报一次前五（第 300 帧起累计） | Report the top five every 300 frames (accumulated from frame 300)
                g_pg_rep_frame = p->frame; double tot = 0; int top[5] = {-1, -1, -1, -1, -1};
                for (int k = 0; k < 256; k++) if (g_pg_st[k].n) { double m = g_pg_st[k].sum / g_pg_st[k].n; tot += m;
                    for (int j = 0; j < 5; j++) if (top[j] < 0 || m > g_pg_st[top[j]].sum / g_pg_st[top[j]].n) { for (int q = 4; q > j; q--) top[q] = top[q - 1]; top[j] = k; break; } }
                char line[1024]; size_t o = 0;
                for (int j = 0; j < 5 && top[j] >= 0; j++) { pg_stat *t = &g_pg_st[top[j]];
                    o += snprintf(line + o, sizeof line - o, " #%d %.2f ms %dx%d fmt%u d%u %s %d 绘制；", top[j], t->sum / t->n, t->meta.w, t->meta.h, t->meta.cfmt0, t->meta.dfmt, t->meta.load, t->meta.draws); }
                MLOG("[passgpu] 至帧 %llu 每帧通道均值合计 %.2f ms；前五：%s", (unsigned long long)p->frame, tot, line);
                fflush(g_pg_f);
            }
        }
        pthread_mutex_unlock(&g_pg_mu);
        free(ps);
    }];
}
void ts_after(void);
// ---------- 写回前瞻：通道结束时按同一次提交的后续命令判各附件写回是否多余 ----------
// TBDR 上每个附件结束时整块写回内存；Switch（Maxwell 直接渲染）没有这笔开销。附件若在被读之前先被整块清除，写回即多余（DontCare）。
// 判定见 mtl_sub_index / sdx_decide；只认同一次提交内能证明的，到提交末尾仍未定论一律写回。
// ---------- Store lookahead: at pass end, decide from the later commands of the same submission whether each attachment's write-back is redundant ----------
// On TBDR every attachment is written back to memory in full when the pass ends; Switch (Maxwell immediate-mode rendering) has no such cost. If an attachment is cleared in full before being read, the write-back is redundant (DontCare).
// For the decision see mtl_sub_index / sdx_decide; only what can be proven within the same submission is accepted, and anything still undecided at the end of the submission is always written back.
typedef struct { uint64_t ser, bytes; uint32_t lv, sl; int idx; uint8_t depth, state, ridx, drawn, why, load, wrote, wwhy; const char *wkey; int wslot; } sd_att;   // depth：0 颜色 1 深度 2 模板（深度模板合一的贴图两部分分开判）   // state 0 未定 1 需要 2 多余；idx：颜色附件序号，深度为 -1 | depth: 0 color 1 depth 2 stencil (the two parts of a combined depth-stencil texture are decided separately)   // state 0 undecided 1 needed 2 redundant; idx: color attachment index, -1 for depth
static sd_att g_sd[9]; static int g_nsd;
// 子项开关 g_tbdr_sub（位掩码，登记进同进程交替测量的变量表）：总开关开着时逐项关掉一项，量每项花的翻译 CPU 与省的 GPU，找净亏的子项。同样每次提交开头锁存
// Sub-item switch g_tbdr_sub (bitmask, registered in the variable table for in-process alternating measurement): with the master switch on, disable one item at a time to measure each item's translation CPU cost and GPU savings, and find net-loss items. Likewise latched at the start of each submit
enum { TB_SDX = 1, TB_HOIST = 2, TB_DEFER = 4, TB_FUSE = 8, TB_DS = 16, TB_UDC = 32, TB_ALL = 63 };   // 写回前瞻（提交预扫描索引 + 跨提交账本）/ 纯清除通道提前 / 同目标续用 / 可读 tile 合并 / 深度模板按需挂 / 没写的附件不写回 | Write-back lookahead (per-submit prescan index + cross-submit ledger) / pure-clear pass hoisting / same-target reuse / readable tile merging / depth-stencil attached on demand / unwritten attachments not written back
int g_tbdr_sub = TB_ALL; static int g_tbdr_sub_eff = TB_ALL;
static int g_udc_ab = -1, g_udc_ab_on;
// 本通道没写的附件（只做深度 / 模板测试、颜色写掩码为 0 等）写回改 DontCare：内容本就在内存里，免一次整块写回。
// 缺省开：Mac 同进程开 / 关截图与空对照同分布；A19 Pro 私有 Depth32F_S8 / RGBA8 DontCare 后读回 0 处不同。
// DM_NVN_UNWRITTEN_DC=0 关闭（等价对照用）
// Attachments this pass did not write (depth / stencil test only, color write mask 0, etc.) get their write-back changed to DontCare: the content is already in memory, saving one full write-back.
// On by default: on Mac, same-process on / off screenshots follow the same distribution as the null control; on A19 Pro, private Depth32F_S8 / RGBA8 read back after DontCare differ in 0 places.
// DM_NVN_UNWRITTEN_DC=0 turns it off (for equivalence comparison)
static int udc_on(void) {
    if (g_udc_ab < 0) { const char *e = getenv("DM_NVN_UNWRITTEN_DC"); g_udc_ab = !(e && !strcmp(e, "0"));
        const char *a = getenv("DM_NVN_UDC_AB"); if (a && !strcmp(a, "1")) { g_udc_ab = 0; g_udc_ab_on = 0; } }
    return (g_udc_ab || g_udc_ab_on) && (g_tbdr_sub_eff & TB_UDC);
}
// 诊断 DM_NVN_TBDR=0：关掉全部 TBDR 优化（写回前瞻、读入可省、同目标续用、纯清除通道前提、深度模板按需挂），回放走最朴素的「全挂、全读、全写」路径。
// 只用于等价对照（同一回放局开 / 关逐字节比画面），不是产品配置
// 运行时总开关 g_tbdr_rt（登记进同进程交替测量的变量表）：真机上交替开 / 关，量这套机制花的翻译 CPU 与省的 GPU / 整机瓦数的净值。
// 只在每次提交开头锁存（g_tbdr_eff），一次提交内不变；锁存值变了就清掉跨提交的前瞻状态（写回核对账本），全零集合本来每次提交开头就清
// Diagnostic DM_NVN_TBDR=0: turns off all TBDR optimizations (writeback lookahead, load elision, same-target continuation, clear-only pass precondition, on-demand depth stencil attachment); replay takes the plainest "attach all, load all, store all" path.
// Only for equivalence comparison (same replay run on / off, comparing frames byte for byte), not a product configuration
// Runtime master switch g_tbdr_rt (registered in the variable table of in-process alternating measurement): toggled on / off on a real device to measure the net of translation CPU spent vs GPU / whole-device watts saved by this mechanism.
// Latched only at the start of each submission (g_tbdr_eff) and constant within a submission; when the latched value changes, the cross-submission lookahead state (writeback check ledger) is cleared; the all-zero set is already cleared at the start of each submission
int g_tbdr_rt = 1; static int g_tbdr_eff = 1;
static int tbdr_env(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_TBDR"), *r = getenv("DM_NVN_RULER"); v = !(e && !strcmp(e, "0")); g_ruler_truth = r && !strcmp(r, "1"); if (!v) MLOG("DM_NVN_TBDR=0：TBDR 优化全关（等价对照用）"); } return v; }
static int tbdr_on(void) { return tbdr_env() && g_tbdr_eff && !g_ruler_truth; }
static int tb_sub(int bit) { return tbdr_on() && (g_tbdr_sub_eff & bit); }
int dm_nvn_ruler_truth(void) { tbdr_on(); return g_ruler_truth; }   // 视觉尺子真值阶段（宿主侧的被测设置据此让开，如 3D 渲染比例） | Visual ruler ground-truth phase (host-side settings under test yield accordingly, e.g. the 3D render scale)
static int rt_range_on(void) { static int v = -1; if (v < 0) { const char *e=getenv("DM_NVN_RT_RANGE"); v=e && !strcmp(e,"1"); } return v; }   // 窄候选，缺省关 | Narrow candidate, off by default
static uint64_t g_sd_dead_bytes, g_sd_n_dead, g_sd_n_all;
// ---------- 跨提交附件有效性账本 ----------
// 条目 = 子资源（贴图序号, 级, 层）+ 状态，跨提交存活：
//   XV_INVALID       内容没写回 / 没生成，不许被读。下一次触碰必须是整张 Clear，或作附件且读入动作 DontCare / Clear；否则违规。
//   XV_PENDING_CLEAR 预留给「纯清除跨提交挂起」（未实现）：只收整张、无剪裁、写掩码全开的清除；已挂起再清 = 换清除值不落地；
//                    作附件且读入 DontCare / Clear → 作废挂起，读入 Load → 改成 Clear 加载；深度模板按平面分开记。
// 违规 = 报红 + 该稳定键（格式 / 宽 / 高 / 级）永久禁用（内存 + DM_XV_FILE 落盘，下次启动读入）+ 同贴图条目全删。兜底只能发现不能挽回（那一帧该附件是旧内容）。
// 用它的第一刀：提交末尾「扫到底未定」的颜色附件不写回（sd_finish）——条件：本次提交里它首次触碰是整张清 / DontCare 读入（每帧先重置再用）、不是窗口贴图、键未禁用。
// 开关 DM_NVN_XV=0 关；tbdr_on()==0（视觉尺子真值模式）不生效。
// 试用期（XV_PROBE）：某个键头 XV_TRIAL 次只「影子记账」——照常写回，但同样跟踪它的下一次触碰；连续 XV_TRIAL 次都是合规触碰才开始真的不写回。
//   试用期里遇到读 / Load：只把该键禁用（本进程内，不落盘、不报红），画面无损—— 实测启动 / 菜单页就有读者（放大目标 36），不该让每次全新安装都错一帧。
// ---------- Cross-submission attachment validity ledger ----------
// Entry = subresource (texture serial, level, layer) + state, surviving across submissions:
//   XV_INVALID       content was not written back / not generated and must not be read. The next touch must be a full Clear, or use as an attachment with load action DontCare / Clear; otherwise it is a violation.
//   XV_PENDING_CLEAR reserved for 'clear-only pending across submissions' (not implemented): accepts only full, unscissored clears with the write mask fully open; clearing again while pending = replace the clear value without landing it;
//                    as an attachment with load DontCare / Clear → the pending clear is voided, with load Load → changed to a Clear load; depth-stencil is recorded per plane.
// Violation = report a failure + that stable key (format / width / height / level) permanently disabled (in memory + persisted to DM_XV_FILE, read at the next launch) + all entries of the same texture deleted. The fallback can only detect, not undo (in that frame the attachment holds old content).
// Its first use: color attachments 'scanned to the end undecided' at the end of a submission are not written back (sd_finish) - conditions: its first touch in this submission is a full clear / DontCare load (reset every frame before use), it is not a window texture, and the key is not disabled.
// Switch DM_NVN_XV=0 turns it off; has no effect when tbdr_on()==0 (visual ruler ground-truth mode).
// Trial period (XV_PROBE): for a key's first XV_TRIAL times it is only 'shadow-accounted' - written back as usual, but its next touch is tracked all the same; only after XV_TRIAL consecutive compliant touches does the write-back really start being skipped.
//   On a read / Load during the trial period: only disable that key (within this process, not persisted, no failure), the picture is intact - in practice there are readers as early as the launch / menu screens (upscale target 36), and not every fresh install should get one wrong frame.
enum { XV_INVALID = 1, XV_PENDING_CLEAR = 2, XV_PROBE = 3 };
#define XV_TRIAL 3
typedef struct { uint64_t ser, tag; uint32_t lv, sl; uint8_t state; } xv_ent;
static xv_ent g_xv[64]; static int g_xv_n, g_xv_block, g_xv_init;
static void tbdr_latch(void) {   // 每次提交开头：锁存 TBDR 运行时开关，变了就清跨提交的写回核对账本 | At the start of each submission: latch the TBDR runtime switch, and clear the cross-submission writeback check ledger if it changed
    if (g_tbdr_sub != g_tbdr_sub_eff) { if ((g_tbdr_sub ^ g_tbdr_sub_eff) & TB_SDX) g_xv_n = 0; g_tbdr_sub_eff = g_tbdr_sub; static int n; if (n++ < 4) MLOG("TBDR 子项开关 → 0x%x（提交 %llu 起）", g_tbdr_sub_eff, (unsigned long long)g_sub_serial); }
    if ((g_tbdr_rt != 0) == g_tbdr_eff) return;
    g_tbdr_eff = g_tbdr_rt != 0; g_xv_n = 0; static int n; if (n++ < 4) MLOG("TBDR 运行时开关 → %d（提交 %llu 起）", g_tbdr_eff, (unsigned long long)g_sub_serial);
}
static uint64_t g_xv_disabled[128]; static int g_xv_ndis;
static struct { uint64_t tag; int good; } g_xv_trial[128]; static int g_xv_ntrial;   // 各键已连续合规的次数 | Number of consecutive compliant touches for each key
static int xv_trial_get(uint64_t tag, int add) { for (int i = 0; i < g_xv_ntrial; i++) if (g_xv_trial[i].tag == tag) return g_xv_trial[i].good += add; if (g_xv_ntrial < 128) { g_xv_trial[g_xv_ntrial].tag = tag; return g_xv_trial[g_xv_ntrial++].good = add; } return 0; }
static uint64_t g_xv_present[128]; static int g_xv_npresent;
static uint64_t g_xv_dc, g_xv_viol;
typedef struct { uint64_t ser; uint32_t lv, sl; uint8_t good; } xv_touch;
static xv_touch g_xv_touch[128]; static int g_xv_ntouch;
static uint64_t xv_tag(const tex_t *t, uint32_t lv) { return ((uint64_t)t->d.format << 40) ^ ((uint64_t)t->d.width << 24) ^ ((uint64_t)t->d.height << 8) ^ lv; }
// 稳定键（跨启动不变，不用贴图序号）：格式 / 宽 / 高 / 级，再混入不写回时所在通道的各颜色附件格式与本附件序号——同格式同尺寸的不同用途贴图（如透光度 27 与放大目标 36）不共用一个键
// Stable key (unchanged across launches, does not use the texture serial): format / width / height / level, then mixed with the formats of the color attachments of the pass where the write-back is skipped and this attachment's index - textures of the same format and size but different purpose (e.g. transmittance 27 and upscale target 36) do not share a key
static uint64_t xv_tag_pass(const tex_t *t, uint32_t lv, int idx, const p_set_rt *rt) { uint64_t h = xv_tag(t, lv) ^ ((uint64_t)(idx + 1) << 60); for (int i = 0; i < rt->n && i < 8; i++) h = (h ^ (rt->color[i] ? (uint64_t)rt->color[i]->d.format + 1 : 0)) * 0x100000001b3ull; return h; }
static void xv_init(void) {
    if (g_xv_init) return; g_xv_init = 1;
    const char *p = getenv("DM_XV_FILE"); if (!p || !*p) return;
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256], why[160]; unsigned long long tag; int ch, n = 0;
    while (fgets(line, sizeof line, f) && g_xv_ndis < 128) if (sscanf(line, "%llx\t%159[^\t]\t%d", &tag, why, &ch) >= 1) g_xv_disabled[g_xv_ndis++] = tag, n++;
    fclose(f); MLOG("读到 %d 条禁用记录", n);
}
static int xv_disabled(uint64_t tag) { xv_init(); for (int i = 0; i < g_xv_ndis; i++) if (g_xv_disabled[i] == tag) return 1; return 0; }
static xv_ent *xv_find(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_n; i++) if (g_xv[i].ser == ser && g_xv[i].lv == lv && g_xv[i].sl == sl) return &g_xv[i]; return NULL; }
static void xv_drop(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_n; i++) if (g_xv[i].ser == ser && g_xv[i].lv == lv && g_xv[i].sl == sl) { g_xv[i] = g_xv[--g_xv_n]; return; } }
static void xv_drop_all_of(uint64_t ser) { for (int i = 0; i < g_xv_n;) if (g_xv[i].ser == ser) g_xv[i] = g_xv[--g_xv_n]; else i++; }
static void xv_set(uint64_t ser, uint32_t lv, uint32_t sl, uint8_t state, uint64_t tag) {
    xv_init(); if (g_xv_block || xv_disabled(tag)) return;
    xv_ent *e = xv_find(ser, lv, sl); if (e) { e->state = state; e->tag = tag; return; }
    if (g_xv_n < 64) g_xv[g_xv_n++] = (xv_ent){ser, tag, lv, sl, state};
}
static void xv_violation(const xv_ent *e, const char *why) {
    xv_init();
    if (e->state == XV_PROBE) {   // 试用期：内容还在，画面无损——只把键禁用（本进程） | Trial period: the content is still there and the picture is intact - only disable the key (this process)
        if (!xv_disabled(e->tag) && g_xv_ndis < 128) g_xv_disabled[g_xv_ndis++] = e->tag;
        PD("V xv 试用期见读者：键=%016llx 贴图=%llu 级=%u 原因=%s → 该键不做", (unsigned long long)e->tag, (unsigned long long)e->ser, e->lv, why);
        xv_drop_all_of(e->ser); return; }
    g_xv_viol++;
    int seen = xv_disabled(e->tag); if (!seen && g_xv_ndis < 128) g_xv_disabled[g_xv_ndis++] = e->tag;
    MLOG("【红】xv 标签=%016llx 贴图=%llu 级=%u 层=%u 原因=%s 通道=%llu", (unsigned long long)e->tag, (unsigned long long)e->ser, e->lv, e->sl, why, (unsigned long long)g_stat_pass);
    const char *p = getenv("DM_XV_FILE"); if (p && *p) { FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%016llx\t%s\t%llu\n", (unsigned long long)e->tag, why, (unsigned long long)g_stat_pass); fclose(f); } }
    xv_drop_all_of(e->ser);
}
static void xv_check(uint64_t ser, uint32_t lv, uint32_t sl, const char *why) { xv_ent *e = xv_find(ser, lv, sl); if (e && (e->state == XV_INVALID || e->state == XV_PROBE)) { xv_ent copy = *e; xv_violation(&copy, why); } }
static void xv_good(xv_ent *e) { if (e->state == XV_PROBE) xv_trial_get(e->tag, 1); xv_drop(e->ser, e->lv, e->sl); }   // 合规触碰：试用期计一次，条目回到有效 | Compliant touch: counts once toward the trial period, and the entry returns to valid
static void xv_touch_add(uint64_t ser, uint32_t lv, uint32_t sl, int good) { for (int i = 0; i < g_xv_ntouch; i++) if (g_xv_touch[i].ser == ser && g_xv_touch[i].lv == lv && g_xv_touch[i].sl == sl) return; if (g_xv_ntouch < 128) g_xv_touch[g_xv_ntouch++] = (xv_touch){ser, lv, sl, (uint8_t)good}; }
static int xv_first_good(uint64_t ser, uint32_t lv, uint32_t sl) { for (int i = 0; i < g_xv_ntouch; i++) if (g_xv_touch[i].ser == ser && g_xv_touch[i].lv == lv && g_xv_touch[i].sl == sl) return g_xv_touch[i].good; return 0; }
static int xv_present(uint64_t ser) { for (int i = 0; i < g_xv_npresent; i++) if (g_xv_present[i] == ser) return 1; return 0; }
static void xv_forward(void);
static int tbdr_on(void);
static int xv_on(void) { static int off = -1; if (off < 0) { const char *e = getenv("DM_NVN_XV"); off = e && !strcmp(e, "0"); } return !off && tbdr_on(); }
void mtl_xv_window_tex(uint64_t serial) { if (!xv_present(serial) && g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = serial; }   // 窗口（交换链）贴图：永不进账本 | Window (swapchain) texture: never enters the ledger
void xv_test_counts(int *dc, int *viol) { if (dc) *dc = (int)g_xv_dc; if (viol) *viol = (int)g_xv_viol; }
static void sd_finish(void);
// 读入可省（全覆盖首绘）：开通道的这次绘制若可证覆盖某颜色附件整块（first_draw_covers），该附件读入改 DontCare——
// 首绘不混合、写满通道，覆盖到的每个像素都被它重写，原内容不可见（Switch 直接渲染无此开销；TBDR 上读入 = 整块从内存读进 tile）
// Load elision (fully covering first draw): if the draw that opens the pass can be proven to cover a color attachment in full (first_draw_covers), that attachment's load becomes DontCare -
// the first draw does not blend and writes all channels, every covered pixel is rewritten by it, and the original content is not visible (Switch immediate-mode rendering has no such cost; on TBDR load = reading the whole thing from memory into the tile)
static const p_draw *g_fd_dw; static const p_draw_idx *g_fd_di;   // 开通道的这次绘制（非索引 / 索引其一）；仅 draw() 开通道期间非空 | The draw that opens the pass (one of non-indexed / indexed); non-null only while draw() is opening a pass
static uint64_t g_fd_n;         // 累计判可省的附件数 | Cumulative number of attachments judged elidable
static int g_fd_why;            // 最近一次不成立的原因（PASSDUMP 的 Q 行：1 无程序 2 着色器 3 附件 4 混合/掩码/光栅 5 深度模板测试 6 顶点数 7 位置属性 8 格式/流 9 越界 10 地址 11 几何） | Reason for the most recent failure (Q line of PASSDUMP: 1 no program 2 shader 3 attachment 4 blend/mask/raster 5 depth-stencil test 6 vertex count 7 position attribute 8 format/stream 9 out of bounds 10 address 11 geometry)
static int first_draw_covers(int i, id<MTLTexture> t, NSUInteger level);
static int pf_is_float(MTLPixelFormat f) {   // 能存 NaN 的浮点颜色格式（读入可省探针的哨兵选择；RGB9E5 存不了 NaN，不算） | Float color formats that can store NaN (sentinel choice for the load-elision probe; RGB9E5 cannot store NaN and does not count)
    switch (f) { case MTLPixelFormatR16Float: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA16Float: case MTLPixelFormatR32Float:
        case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA32Float: case MTLPixelFormatRG11B10Float: return 1; default: return 0; }
}
static int fz_load_covers(int i, id<MTLTexture> t, NSUInteger level);
static NSMutableArray *g_lp;    // DM_NVN_LOADPROBE=1 / 2 / 3（哨兵值两种常数 / NaN——浮点格式下着色器正常输出碰不到 NaN，排除数值巧合）：判可省的附件改清成哨兵色，通道结束回读数剩余哨兵像素（诊断：应为 0） | DM_NVN_LOADPROBE=1 / 2 / 3 (two constant sentinel values / NaN - under float formats normal shader output cannot reach NaN, ruling out numeric coincidence): attachments judged elidable are cleared to the sentinel color instead, and at pass end the remaining sentinel pixels are read back and counted (diagnostic: should be 0)
static void lp_finish(void);
// 同目标续用：游戏对同一组目标再次 SetRenderTargets（中间无拷贝 / 派发 / 清除）时不结束通道——TBDR 上省一次整块写回 + 读入。
// 等价依据：NVN 在两个通道间的 Barrier 要求后者看到前者的全部写入。同一 Metal 渲染编码器内，后续绘制对附件本身的读写（混合、深度 / 模板测试）
// 与先前绘制按提交顺序有序；但「采样」附件贴图不在此列（同一通道内读写同一附件未定义）。故只在后续绘制不采样这些附件时续用：
// 续用期间某次绘制要采样它们，或来了清除，就在它之前结束通道，回到分开的语义。拷贝 / 派发本来就会结束渲染编码器
// Same-target continuation: when the game calls SetRenderTargets again on the same set of targets (with no copy / dispatch / clear in between) the pass is not ended - on TBDR this saves one full write-back + load.
// Basis for equivalence: NVN's Barrier between two passes requires the latter to see all writes of the former. Within one Metal render encoder, later draws' reads and writes of the attachment itself (blend, depth / stencil test)
// are ordered with earlier draws in submission order; but 'sampling' the attachment texture is not covered (reading and writing the same attachment within one pass is undefined). So continuation happens only when later draws do not sample these attachments:
// if during continuation a draw needs to sample them, or a clear arrives, the pass is ended before it, returning to the separate semantics. Copies / dispatches end the render encoder anyway
static int g_enc_mode;          // 当前编码器的开法（rp_begin_ex 参数：0 全部附件 1 只为清除 2 省深度） | How the current encoder was opened (rp_begin_ex argument: 0 all attachments 1 only for clears 2 depth omitted)
static int g_rt_range_active, g_rt_range_pending;
static int g_rt_merged;         // 当前通道已续用过（至少一次同目标 SetRenderTargets 被并入） | The current pass has already been continued (at least one same-target SetRenderTargets was merged in)
static uint64_t g_rt_merge_n;   // 累计续用次数 | Cumulative continuation count
static int g_defer_replaying;
static void fz_end(void);   // 可读 tile 通道合并的收尾（定义在 draw 之前） | Wrap-up of the readable-tile pass merge (defined before draw)
static void defer_fallback(void);
// DM_NVN_ENC_LABEL=1：给每个渲染编码器打标签「p<帧内第几个渲染通道> <宽>x<高> <首次绘制的片元键前 8 位> d<绘制数>」，Instruments（xctrace Game Performance / Metal System Trace）
// 的编码器轨与 GPU 区间表直接带出这个名字，按 shader_map.tsv 就能对回着色器；缺省关（每通道一次字符串分配）
// DM_NVN_ENC_LABEL=1: label every render encoder with 'p<index of the render pass within the frame> <width>x<height> <first 8 digits of the first draw's fragment key> d<draw count>'; in Instruments (xctrace Game Performance / Metal System Trace)
// the encoder track and the GPU interval table carry this name directly, and shader_map.tsv maps it back to the shader; off by default (one string allocation per pass)
static int g_el_w, g_el_h, g_el_draws; static char g_el_key[12];
static int enc_label_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_ENC_LABEL"); v = e && atoi(e); } return v; }
static void ghost_flush(void);
static void rp_end(void) { if (g_enc) {
    ghost_flush();
    if (enc_label_on()) { static uint64_t lf = ~0ull, base; uint64_t f = atomic_load(&g_cur_frame); if (f != lf) { lf = f; base = g_stat_pass - 1; }   // 帧内序号：同一趟每帧同名，表里才能按名归并 | Intra-frame serial: the same pass has the same name every frame, so the table can aggregate by name
        [(id)g_enc setLabel:[NSString stringWithFormat:@"p%02llu %dx%d %s d%d", (unsigned long long)(g_stat_pass - base), g_el_w, g_el_h, g_el_key[0] ? g_el_key : "-", g_el_draws]]; }
#ifdef NVN_RT_RANGE_TEST
    int ranged=g_rt_range_active && !g_rt_range_pending;
#endif
    sd_finish(); pg_render_finish(g_enc); enc_r_end(g_enc); zskip_commit_clears(); g_enc = nil; g_rt_merged = 0; g_rt_range_active = g_rt_range_pending = 0; pg_end_render(); ts_after(); if (g_lp.count) lp_finish(); fz_end();
#ifdef NVN_RT_RANGE_TEST
    rr_finish(ranged);
#endif
} }
void pg_sub_begin_ext(uint64_t fence) { pg_sub_begin(fence); }

// 颜色附件实际挂上的格式：视图换了格式且挂得上就是视图格式，否则底图格式（挂附件与管线预测共用，口径一致）。warn：挂不上时报红
// The format a colour attachment is actually attached with: the view's format if the view changes the format and it can be attached, otherwise the base texture's format (shared by attaching and pipeline prediction, so both agree). warn: log red when it cannot be attached
static MTLPixelFormat rt_color_fmt(id<MTLTexture> base, const view_t *v, int has_v, int warn) {
    if (!base || !has_v || !v->set_format) return base.pixelFormat;
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(v->format, &pf, swz) || !pf) return base.pixelFormat;
    if (pf != (uint32_t)base.pixelFormat && tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) {
        if (warn) RED_ONCE(48, "渲染目标视图格式 %#x 挂不上（底图 %lu 没有换格式用途），按底图格式画（首例）", v->format, (unsigned long)base.pixelFormat);
        return base.pixelFormat; }
    return (MTLPixelFormat)pf;
}
// color：颜色附件的视图换了格式（如 RGBA8 底图经 RGBA8_SRGB 视图写）就挂该格式的 Metal 视图——Switch 上写入与清除按视图格式编码，
// 只挂底图则 sRGB 编码整个丢掉、画面偏暗（公开仓库 #36，seedds 在另一款游戏上实测）；管线颜色格式取挂上的贴图格式（g_pass.cfmt），随之一致。
// 视图只活到本次提交跑完（Metal 4 不持有资源；视图与底图同一份存储，不单独驻留）。深度 / 模板附件不换格式
// color: when the view of a colour attachment changes the format (e.g. an RGBA8 base texture written through an RGBA8_SRGB view), attach a Metal view of that format - on the Switch writes and clears are encoded by the view's format,
// and attaching only the base texture drops the sRGB encoding entirely, making the picture too dark (public repository #36, measured by seedds on another game); the pipeline colour formats take the attached texture's format (g_pass.cfmt), so they follow.
// The view lives only until this submission completes (Metal 4 does not retain resources; the view shares storage with the base texture and is not made resident separately). Depth / stencil attachments do not change format
static void attach(MTLRenderPassAttachmentDescriptor *a, const tex_t *t, const view_t *v, int has_v, int color) {
    id<MTLTexture> base = tex_mtl(t); a.texture = base;
    if (has_v && v->set_levels) a.level = v->base_level;
    if (has_v && v->set_layers) { if (t->d.target == 2) a.depthPlane = v->min_layer; else a.slice = v->min_layer; }
    if (!color || !base) return;
    MTLPixelFormat pf = rt_color_fmt(base, v, has_v, 1);
    if (pf == base.pixelFormat) return;
    id<MTLTexture> vw = [base newTextureViewWithPixelFormat:pf];
    if (!vw) { RED_ONCE(49, "渲染目标视图建不出：格式 %#x（底图 %lu），按底图格式画（首例）", v->format, (unsigned long)base.pixelFormat); return; }
    a.texture = vw; defer_obj_unref(vw);
    { static int once; if (!once++) MLOG("渲染目标经换格式的视图挂上：视图格式 %#x，底图 %lu → 挂 %lu（首例）", v->format, (unsigned long)base.pixelFormat, (unsigned long)pf); }
}
// mode 1（clear_only）：只为落地挂起的清除而开的通道，只挂有清除的附件；mode 2：前瞻证明本通道不用深度模板，不挂深度。
// clear_only：只为落地挂起的清除而开的通道，只挂有清除的附件——其余附件本通道不碰，挂上去在 TBDR 上平白一读一写
// （Mac 选角页：清色通道连带挂上 400x225 深度读入写出）
// mode 1 (clear_only): a pass opened only to land pending clears, attaching only the attachments that have clears; mode 2: the lookahead proves this pass does not use depth-stencil, so depth is not attached.
// clear_only: a pass opened only to land pending clears, attaching only the attachments that have clears - the other attachments are not touched by this pass, and attaching them costs a pointless load and store on TBDR
// (Mac character select screen: the color-clear pass drags along a 400x225 depth load and store)
static RENC rp_begin_ex(int clear_only);
static RENC rp_begin(void) { return rp_begin_ex(0); }
static RENC rp_begin_ex(int clear_only) {
    if (g_enc) return g_enc;
    memset(g_zskip_cand_ok, 0, sizeof g_zskip_cand_ok);
    g_enc_mode = clear_only;
    if (!R.has_rt) { RED_ONCE(3, "绘制前没有设置渲染目标"); return nil; }
    // 附件范围候选的版本在捕获任何附件之前取；建立途中有变化则不续用。
    // The version of the attachment-range candidate is taken before capturing any attachment; if it changes during setup, there is no continuation.
    uint64_t reg0 = 0, ser0 = 0, dirty0 = 0;
    if (rt_range_on()) { reg0 = g_reg_gen; ser0 = g_ser_gen; }
    if (rt_range_on()) dirty0=nvn_dirty_now();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    RPD *pd = [RPD new];
    int any = 0;
    for (int i = 0; i < R.rt.n; i++) {
        if (!R.rt.color[i] || (clear_only == 1 && !R.clear_c[i])) continue;
        attach(pd.colorAttachments[i], R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i], 1);
        if (!pd.colorAttachments[i].texture) continue;
        any = 1;
        pd.colorAttachments[i].loadAction = R.clear_c[i] ? MTLLoadActionClear : MTLLoadActionLoad;
        pd.colorAttachments[i].clearColor = R.cc[i];
        int cov = tbdr_on() && !R.clear_c[i] && (g_fd_dw || g_fd_di) && fz_load_covers(i, pd.colorAttachments[i].texture, pd.colorAttachments[i].level);
        if (!R.clear_c[i] && (g_fd_dw || g_fd_di) && !cov) PD("Q %llu c%d 读入不可省 why=%d", g_stat_pass + 1, i, g_fd_why);
        if (cov) {
            static int probe = -1; if (probe < 0) { probe = getenv("DM_NVN_LOADPROBE") ? atoi(getenv("DM_NVN_LOADPROBE")) : 0; if (probe) MLOG("DM_NVN_LOADPROBE：读入可省的附件改清成哨兵色并回读核对"); }
            if (probe) { pd.colorAttachments[i].loadAction = MTLLoadActionClear; int pm = probe == 1 && pf_is_float(pd.colorAttachments[i].texture.pixelFormat) ? 3 : probe;   // 浮点格式缺省用 NaN 哨兵：固定哨兵值会被真实输出撞上（实测撞上过） | Float formats use a NaN sentinel by default: a fixed sentinel value can be hit by real output (it has been hit in practice)
                pd.colorAttachments[i].clearColor = pm == 3 ? MTLClearColorMake(NAN, NAN, NAN, NAN) : probe == 2 ? MTLClearColorMake(0.777, 0.222, 0.555, 0.888) : MTLClearColorMake(0.123, 0.456, 0.789, 0.321);
                if (!g_lp) g_lp = [NSMutableArray array];
                [g_lp addObject:@[pd.colorAttachments[i].texture, @(pd.colorAttachments[i].level), @(pd.colorAttachments[i].slice), @(g_stat_pass + 1), @(i), @(R.rt.color[i]->serial), @(pm)]]; }
            else pd.colorAttachments[i].loadAction = MTLLoadActionDontCare;
            g_fd_n++;
        }
        xv_touch_add(R.rt.color[i]->serial, (uint32_t)pd.colorAttachments[i].level, (uint32_t)pd.colorAttachments[i].slice,
                     pd.colorAttachments[i].loadAction != MTLLoadActionLoad);
        if (g_xv_n) { xv_ent *xe = xv_find(R.rt.color[i]->serial, (uint32_t)pd.colorAttachments[i].level, (uint32_t)pd.colorAttachments[i].slice);
            if (xe && (xe->state == XV_INVALID || xe->state == XV_PROBE)) { if (pd.colorAttachments[i].loadAction == MTLLoadActionLoad) { xv_ent copy = *xe; xv_violation(&copy, "作附件读入旧内容"); } else xv_good(xe); } }
        pd.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    // clear_only：0 全挂 1 只为清除 2 深度模板都不挂 3 只挂模板 4 只挂深度（3 / 4 由 pass_ds_need 前瞻证明整通道只用那一项）
    // clear_only: 0 attach all 1 only for clears 2 neither depth nor stencil attached 3 stencil only 4 depth only (3 / 4: the pass_ds_need lookahead proves the whole pass uses only that one)
    if (R.rt.depth && clear_only == 3) {   // 只挂模板：合一格式的深度平面不进 tile、不读不写（Metal 允许深度附件空、模板附件挂合一贴图） | Stencil only: the depth plane of the combined format does not enter the tile and is neither read nor written (Metal allows an empty depth attachment with the combined texture on the stencil attachment)
        attach(pd.stencilAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview, 0);
        MTLPixelFormat pf = pd.stencilAttachment.texture.pixelFormat;
        if (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatStencil8) {
            any = 1; pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
        } else pd.stencilAttachment.texture = nil;   // 没有模板平面：模板测试恒过，什么都不挂 | No stencil plane: the stencil test always passes, attach nothing
    } else if (R.rt.depth && clear_only != 2 && !(clear_only == 1 && !R.clear_d && !R.clear_s)) {
        attach(pd.depthAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview, 0);
        if (pd.depthAttachment.texture) {
            any = 1;
            pd.depthAttachment.loadAction = R.clear_d ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.depthAttachment.clearDepth = R.cd; pd.depthAttachment.storeAction = MTLStoreActionStore;
            MTLPixelFormat pf = pd.depthAttachment.texture.pixelFormat;
            if (clear_only != 4 && (pf == MTLPixelFormatDepth32Float_Stencil8 || pf == MTLPixelFormatStencil8)) {   // 4：只挂深度 | 4: depth only
                pd.stencilAttachment.texture = pd.depthAttachment.texture; pd.stencilAttachment.level = pd.depthAttachment.level;
                pd.stencilAttachment.slice = pd.depthAttachment.slice;
                pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
                pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
            }
        }
    }
    if (!any) { RED_ONCE(4, "渲染目标都建不出 MTLTexture"); return nil; }
    for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i] && zskip_zero_clear(i)) {
        g_zskip_cand_ok[i] = 1; g_zskip_cand[i] = (zskip_zero){R.rt.color[i]->serial, (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0), (uint16_t)(R.rt.has_cview[i] && R.rt.cview[i].set_layers ? R.rt.cview[i].min_layer : 0)};
    }
    // 带宽账：Load 的附件计读入，全部附件计写出（当前一律 Store）
    // Bandwidth ledger: attachments with Load count as load, all attachments count as store (currently always Store)
    int nload = 0, natt = 0;
    for (int i = 0; i < R.rt.n; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i];
        if (!c.texture) continue;
        uint64_t b = att_bytes(c.texture, c.level); natt++; g_bw_store += b; if (c.loadAction == MTLLoadActionLoad) { g_bw_load += b; nload++; }
        if (R.rt.has_cview[i] && R.rt.cview[i].set_format) PD("V %llu c%d 视图格式 %#x（基贴图 NVN 格式 %#x）", g_stat_pass + 1, i, R.rt.cview[i].format, R.rt.color[i]->d.format);
        PD("P %llu c%d ser=%llu fmt=%lu %lux%lu ms%lu lv%lu sl%lu %s", g_stat_pass + 1, i, R.rt.color[i]->serial, (unsigned long)c.texture.pixelFormat,
           (unsigned long)c.texture.width, (unsigned long)c.texture.height, (unsigned long)c.texture.sampleCount, (unsigned long)c.level, (unsigned long)c.slice,
           c.loadAction == MTLLoadActionClear ? "clear" : c.loadAction == MTLLoadActionDontCare ? "dontcare" : "load");
    }
    {   // 深度与模板平面分开记（合一格式 5 字节 = 深度 4 + 模板 1；只挂一项时只记那一项） | Depth and stencil planes are counted separately (combined format 5 bytes = depth 4 + stencil 1; when only one is attached only that one is counted)
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t whole = att_bytes(dt ? dt : stt, dt ? pd.depthAttachment.level : pd.stencilAttachment.level);
        int comb = (dt ? dt : stt).pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        uint64_t db = dt ? (comb ? whole / 5 * 4 : whole) : 0, sb = stt && stt != dt ? (comb ? whole / 5 : whole) : stt ? whole - db : 0;
        if (dt) { natt++; g_bw_store += db; if (pd.depthAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += db; nload++; } }
        if (stt) { natt += !dt; g_bw_store += sb; if (pd.stencilAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += sb; nload += !dt; } }
        if (dt) PD("P %llu d ser=%llu fmt=%lu %lux%lu %s", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)dt.pixelFormat,
                   (unsigned long)dt.width, (unsigned long)dt.height, pd.depthAttachment.loadAction == MTLLoadActionClear ? "clear" : "load");
        else if (stt) PD("P %llu s ser=%llu fmt=%lu %lux%lu %s（只挂模板）", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)stt.pixelFormat,
                   (unsigned long)stt.width, (unsigned long)stt.height, pd.stencilAttachment.loadAction == MTLLoadActionClear ? "clear" : "load");
    }
    if (natt && nload == natt) g_pass_full_rw++;
    g_nsd = 0;
    {
        for (int i = 0; i < R.rt.n && i < 8; i++) { MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i]; if (!c.texture) continue;
            g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.color[i]->serial; g_sd[g_nsd].bytes = att_bytes(c.texture, c.level); g_sd[g_nsd].lv = (uint32_t)c.level; g_sd[g_nsd].sl = (uint32_t)(c.texture.textureType == MTLTextureType3D ? c.depthPlane : c.slice); g_sd[g_nsd].idx = i; g_sd[g_nsd].load = c.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_c[i] != 0; g_nsd++;
            c.storeAction = MTLStoreActionUnknown; }
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t db = att_bytes(dt, pd.depthAttachment.level), sb = 0;
        if (stt) { int comb = stt.pixelFormat == MTLPixelFormatDepth32Float_Stencil8; sb = att_bytes(stt, pd.stencilAttachment.level) / (comb ? 5 : 1); if (stt == dt) db -= sb; }   // 合一格式 5 字节 = 深度 4 + 模板 1（只挂模板时也只记 1） | Combined format 5 bytes = depth 4 + stencil 1 (when only stencil is attached, only 1 is counted)
        if (dt) { g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.depth->serial; g_sd[g_nsd].bytes = db; g_sd[g_nsd].lv = (uint32_t)pd.depthAttachment.level; g_sd[g_nsd].sl = (uint32_t)pd.depthAttachment.slice; g_sd[g_nsd].idx = -1; g_sd[g_nsd].depth = 1; g_sd[g_nsd].load = pd.depthAttachment.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_d != 0; g_nsd++;
                  pd.depthAttachment.storeAction = MTLStoreActionUnknown; }
        if (stt) { g_sd[g_nsd] = (sd_att){0}; g_sd[g_nsd].ser = R.rt.depth->serial; g_sd[g_nsd].bytes = sb; g_sd[g_nsd].lv = (uint32_t)pd.stencilAttachment.level; g_sd[g_nsd].sl = (uint32_t)pd.stencilAttachment.slice; g_sd[g_nsd].idx = -1; g_sd[g_nsd].depth = 2; g_sd[g_nsd].load = pd.stencilAttachment.loadAction == MTLLoadActionLoad; g_sd[g_nsd].wrote = R.clear_s != 0; g_nsd++;
                   pd.stencilAttachment.storeAction = MTLStoreActionUnknown; }
    }
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    {   // 通道信息（口径同原先每次绘制现取：颜色取各附件基贴图格式，采样数取最后一个颜色附件、有深度则取深度） | Pass info (same convention as the former per-draw fetch: colors take each attachment's base texture format; sample count takes the last color attachment, or depth if there is depth)
        uint64_t ser = g_pass.serial + 1; memset(&g_pass, 0, sizeof g_pass); g_pass.serial = ser;
        for (int i = 0; i < R.rt.n && i < 8; i++) { id<MTLTexture> t = R.rt.color[i] ? pd.colorAttachments[i].texture : nil;
            if (t) { g_pass.cfmt[i] = (uint32_t)t.pixelFormat; g_pass.samples = (uint32_t)t.sampleCount; } }
        id<MTLTexture> dt = R.rt.depth ? pd.depthAttachment.texture : nil, stt = R.rt.depth ? pd.stencilAttachment.texture : nil;   // 深度 / 模板附件分开取（可能只挂其一） | Depth / stencil attachments are fetched separately (possibly only one of them is attached)
        g_pass.hd = dt && dt.pixelFormat != MTLPixelFormatStencil8; g_pass.hs = stt != nil;
        g_pass.dfmt = g_pass.hd ? (uint32_t)dt.pixelFormat : 0; g_pass.sfmt = stt ? (uint32_t)stt.pixelFormat : 0;
        if (dt || stt) g_pass.samples = (uint32_t)(dt ? dt : stt).sampleCount;
        pass_dims(pd, (NSUInteger)R.rt.n, &g_pass.tw, &g_pass.th);
        if (rt_range_on()) { g_pass.reg_gen = reg0; g_pass.ser_gen = ser0; }
        if (rt_range_on()) { g_pass.range_dirty=dirty0; g_pass.range_color=pd.colorAttachments[0].texture; g_pass.range_depth=pd.depthAttachment.texture; }
        g_enc_pso = nil; g_enc_dss = nil; g_enc_fx.valid = 0;
    }
    if (g_vis_on) {
        // 槽用满（单次提交超过 VIS_SLOTS 个计数通道）：另建一块溢出缓冲接着用。环里的缓冲不许交去退役（它还要轮着用，退役会把它移出驻留集）；
        // 退役的只能是上一块溢出缓冲。溢出缓冲本身在下次提交换回环里的缓冲时退役（mtl_sub_begin）
        // Slots used up (one submission exceeds VIS_SLOTS counting passes): create an overflow buffer and continue with it. Buffers in the ring must not be handed to retirement (they are still used in rotation, and retiring would remove them from the residency set);
        // only the previous overflow buffer may be retired. The overflow buffer itself is retired when the next submission switches back to a ring buffer (mtl_sub_begin)
        if (!g_vis || g_vis_n >= VIS_SLOTS) { if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis); g_vis = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis); g_vis_n = 0; }
        pd.visibilityResultBuffer = g_vis;
    }
    g_enc_vis = 0; g_enc_has_vis = g_vis_on;
    pg_render(pd, (NSUInteger)R.rt.n);
    { void td_note(id pd, NSUInteger nc); td_note(pd, (NSUInteger)R.rt.n);
      void ts_note(id pd, NSUInteger nc); ts_note(pd, (NSUInteger)R.rt.n); }
    ser_gate();
    g_enc = enc_r_begin(pd); pg_render_begin(g_enc);
    args_dirty_all();
    g_stat_pass++;
    if (enc_label_on()) { id<MTLTexture> lt = pd.colorAttachments[0].texture ?: pd.depthAttachment.texture ?: pd.stencilAttachment.texture; g_el_w = (int)lt.width; g_el_h = (int)lt.height; g_el_draws = 0; g_el_key[0] = 0; }
    return g_enc;
}
// 开通道前的空剪裁预检查：第一个有效目标的选中级尺寸是实际pass最小边界的上界。
// Empty-scissor precheck before opening a pass: the size of the selected level of the first valid target is an upper bound on the minimum bounds of the actual pass.
static void rt_dims(NSUInteger *tw, NSUInteger *th) {
    id<MTLTexture> t0 = nil; NSUInteger level = 0;
    for (int i = 0; i < R.rt.n && i < 8 && !t0; i++) if (R.rt.color[i]) {
        t0 = tex_mtl(R.rt.color[i]); level = R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0;
    }
    if (!t0 && R.rt.depth) { t0 = tex_mtl(R.rt.depth); level = R.rt.has_dview && R.rt.dview.set_levels ? R.rt.dview.base_level : 0; }
    *tw = t0 ? mip_extent(t0.width, level) : 1; *th = t0 ? mip_extent(t0.height, level) : 1;
}
// 有挂起的清除但通道里没有绘制：开一次通道把清除落地
// There are pending clears but no draw in the pass: open a pass once to land the clears
static void rp_flush(void) {
    int pend = R.clear_d || R.clear_s;
    for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
    if (pend && !g_enc && g_cb) rp_begin_ex(1);
    rp_end();
}
static int scissor_full(const tex_t *t, const view_t *v, int has_v) {
    NSUInteger level = has_v && v->set_levels ? v->base_level : 0;
    NSUInteger w = mip_extent(t->d.width, level), h = mip_extent(t->d.target == 0 || t->d.target == 3 ? 1 : t->d.height, level);
    return !R.has_sc || (R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h);
}


// ================= 着色器 / 描述符池 / 管线 / 绘制 =================
// ================= Shaders / descriptor pool / pipelines / draws =================
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
// ---------- 着色器映射表：Switch 控制段哈希 → PC metallib（dm/gfx/tools/shader_map.py 产出） ----------
// ---------- Shader mapping table: Switch control section hash → PC metallib (produced by dm/gfx/tools/shader_map.py) ----------
static NSMutableDictionary<NSString *, NSArray *> *g_shmap;
static NSString *g_shmap_path, *g_msl_dir;
static NSString *exe_rel(const char *rel) {
    char buf[4096]; uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n)) return nil;
    char *dir = dirname(buf);
    return [NSString stringWithFormat:@"%s/%s", dir, rel];
}
// 着色器映射表（tools/shader_map.py 出）：运行时只用「Switch 控制段键 → 阶段」；着色器一律是 Switch 原版翻译（tools/dmsh_build.py 离线出 <键>_b.metallib + 反射）
// Shader mapping table (output of tools/shader_map.py): at runtime only 'Switch control section key → stage' is used; shaders are always translations of the Switch originals (tools/dmsh_build.py produces <key>_b.metallib + reflection offline)
static void shmap_load(void) {
    if (g_shmap) return;
    const char *e = getenv("DM_NVN_SHMAP");
    NSString *path = e ? @(e) : exe_rel("../gfx/nvn/shader_map.tsv");
    NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!txt) MDIE("读不到着色器映射表 %s（DM_NVN_SHMAP 可指定）", path.UTF8String);
    g_shmap = [NSMutableDictionary dictionary];
    for (NSString *ln in [txt componentsSeparatedByString:@"\n"]) {
        if (!ln.length || [ln hasPrefix:@"#"] || [ln hasPrefix:@"package"]) continue;
        NSArray *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count > 3) g_shmap[c[3]] = c[2];
    }
    g_shmap_path = path;
    const char *m = getenv("DM_MSL_DIR");
    // 库目录 = <DM_MSL_DIR 或缺省缓存>/b-<翻译链版本>：版本取自本源码树的 dmsh_chain.h（tools/dmsh_chain.py），旧源码树编的宿主只认旧版本的库
    // Library directory = <DM_MSL_DIR or default cache>/b-<translation chain version>: the version comes from this source tree's dmsh_chain.h (tools/dmsh_chain.py); a host built from an old source tree only accepts libraries of the old version
    g_msl_dir = [NSString stringWithFormat:@"%@/b-%s", m ? @(m) : [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work"], DMSH_CHAIN];
    if (![NSFileManager.defaultManager fileExistsAtPath:g_msl_dir]) MLOG("【红】着色器库目录 %s 不存在：本源码树的翻译链版本 %s 还没出包（dm/gfx/tools/dmsh_build.py）", g_msl_dir.UTF8String, DMSH_CHAIN);
    NSString *cp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"c1_full.tsv"];   // 与映射表同目录（Mac：dm/gfx/nvn；iOS：App 内 dm/，run.sh 一起拷） | Same directory as the mapping table (Mac: dm/gfx/nvn; iOS: dm/ inside the App, copied along by run.sh)
    NSString *ct = cp ? [NSString stringWithContentsOfFile:cp encoding:NSUTF8StringEncoding error:nil] : nil;
    NSString *head = ct ? [[ct componentsSeparatedByString:@"\n"] firstObject] : nil;
    NSString *want = [NSString stringWithFormat:@"# chain b-%s", DMSH_CHAIN];
    if (!head || ![head hasPrefix:want]) {
        MLOG("【红】c1_full.tsv 清单过期或缺失，全部 c1 独占 64 KiB");
    } else {
        g_c1_full_valid = 1; g_c1_full = [NSMutableSet set];
        for (NSString *line in [ct componentsSeparatedByString:@"\n"]) if (line.length && ![line hasPrefix:@"#"]) [g_c1_full addObject:[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]];
    }
    MLOG("着色器路线 B（Switch 原版翻译）：映射表 %lu 条（%s），metallib 目录 %s", (unsigned long)g_shmap.count, path.UTF8String, g_msl_dir.UTF8String);
    warm_start();
}

@interface NvShader : NSObject { @public id<MTLFunction> fn; nvn_refl rf; id<MTLArgumentEncoder> ae; NSUInteger arg_len; int stage, range_ro; const char *builtin; uint64_t logged; char key[24]; id<MTLBuffer> c1buf; uint64_t c1_gpu; size_t c1off;
    // 上次编好的参数缓冲
    // The argument buffer encoded last time
    id<MTLBuffer> ab_buf; uint64_t ab_gpu; size_t ab_off; uint64_t ab_pass;
    int8_t zf_state, zf_nf; zskip_fact zf;   // 全零跳过事实表的查表结果缓存（0 未查 / 1 无 / 2 有）；键不变，表长（单测可增补）没变就不重查   // ab_pass：编这块参数的渲染通道序号——只在同一通道内复用（跨通道 / 跨提交的块所在 arena 可能已回收重写） | Cached lookup result in the all-zero skip fact table (0 not looked up / 1 absent / 2 present); with the key unchanged and the table length (unit tests may append) unchanged, no re-lookup   // ab_pass: render pass index where this argument block was encoded; reused only within the same pass (across passes / submissions the arena holding the block may have been recycled and rewritten)
    id<MTLLibrary> bflib; int bflib_tried; NSMutableDictionary<NSString *, id<MTLFunction>> *bffn; } @end   // bflib：可读 tile 版本 <键>_bf（合并 B 第 2 步） | bflib: readable-tile version <key>_bf (merge B step 2)
@implementation NvShader @end
#include "nvn_pso_skey.h"
@interface NvProg : NSObject { @public NvShader *sh[6]; __unsafe_unretained NvShader *fl_sh[6]; uint8_t fl_ok, fl_img, fl_img_w; uint32_t fl_cmask; } @end   // fl_*：按绘制要用的反射摘要（udc_note_draw），按 sh[] 核对后复用 | fl_*: reflection summary used per draw (udc_note_draw), reused after checking against sh[]
@implementation NvProg @end
// 预热项：后台建好、等提交线程领用。vs / fs 强引用（pso 里只是裸指针，后台字典随任务结束释放）
// Warm-up entry: built in the background, waiting to be taken by the submit thread. vs / fs are strong references (in pso they are only raw pointers, and the background dictionary is released when the task ends)
@interface WarmItem : NSObject { @public int kind; NSString *key, *vs_key, *fs_key; NvShader *shader, *vs, *fs; pso_key pso; id<MTLFunction> fsfn; id object; } @end
@implementation WarmItem @end
static NSMutableDictionary<NSString *, NvShader *> *g_shcache; static _Atomic uint32_t g_shcache_n;   // 表只在提交线程上读写；条数另记一份原子数给别的线程读（资源计数） | The tables are read and written only on the submit thread; a separate atomic count is kept for other threads to read (resource counts)

static void c1_store(NvShader *o) {
    size_t len = o->rf.c1_len;
    NSString *guardKey = [NSString stringWithUTF8String:o->key];
    if ([guardKey hasSuffix:@"_b"]) guardKey = [guardKey substringToIndex:guardKey.length - 2];
    os_unfair_lock_lock(&g_c1_lock);   // 预热线程与提交线程都会载入着色器 | Both the warm-up thread and the submit thread load shaders
    if (!g_c1_full_valid || [g_c1_full containsObject:guardKey]) {   // 运行时下标未证有界：独占 64 KiB 全零（同原实现） | Runtime index not proven bounded: exclusive 64 KiB of all zeros (same as the original implementation)
        o->c1buf = [g_dev newBufferWithLength:C1_DECL_SIZE options:MTLResourceStorageModeShared]; o->c1_gpu = o->c1buf.gpuAddress;
        if (!o->c1buf) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 独占缓冲分配失败"); }
        res_add(o->c1buf);
        memset(o->c1buf.contents, 0, C1_DECL_SIZE); memcpy(o->c1buf.contents, o->rf.c1, len);
        g_c1_alloc_bytes += C1_DECL_SIZE; g_c1_data_bytes += len; g_c1_count++; g_c1_exclusive++;
        os_unfair_lock_unlock(&g_c1_lock);
        return;
    }
    size_t need = (len + 255u) & ~((size_t)255u);
    if (!g_c1_banks) g_c1_banks = [NSMutableArray array];
    if (!g_c1_banks.count || g_c1_bank_off + need + 256u + C1_DECL_SIZE > C1_BANK_SIZE) {
        id<MTLBuffer> b = [g_dev newBufferWithLength:C1_BANK_SIZE options:MTLResourceStorageModeShared];
        if (!b) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 缓冲块分配失败"); }
        res_add(b);
        memset(b.contents, 0, C1_BANK_SIZE);
        [g_c1_banks addObject:b];
        g_c1_bank_off = 0;
        g_c1_alloc_bytes += C1_BANK_SIZE;
    }
    id<MTLBuffer> b = g_c1_banks.lastObject;
    size_t off = g_c1_bank_off;
    memcpy((uint8_t *)b.contents + off, o->rf.c1, len);
    g_c1_bank_off += need + 256u;
    g_c1_data_bytes += len;
    g_c1_count++;
    o->c1buf = b;
    o->c1_gpu = b.gpuAddress;
    o->c1off = off;
    os_unfair_lock_unlock(&g_c1_lock);
}

static NSMutableArray<WarmItem *> *g_warm_items;
static os_unfair_lock g_warm_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic int g_warm_ready;
static int g_warm_started;
static uint64_t g_warm_t0;
static _Atomic int g_warm_running;   // 后台预热还没跑完（进局读取画面据此多等一会儿，见 mtl_warm_wait） | Background warm-up has not finished yet (the loading screen on entering a run waits a little longer based on this, see mtl_warm_wait)
static uint64_t g_warm_shaders, g_warm_rps, g_warm_cps, g_warm_skip_fz;
static uint64_t g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps;
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap);
static int rt_range_bindings_ro(NSDictionary *j);
static FILE *g_pso_record; static int g_pso_record_checked;
static FILE *pso_record_file(void) {
    if (g_pso_record_checked) return g_pso_record;
    g_pso_record_checked = 1;
    const char *path = getenv("DM_PSO_RECORD");
    if (path && *path) g_pso_record = fopen(path, "a");
    return g_pso_record;
}
static void pso_record(const char *fmt, ...) {
    FILE *f = pso_record_file();
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fflush(f);
}
// 用户清单：本机运行中提交线程同步新建的着色器 / 管线，下次启动与种子清单一起预热。
// 路径 = DM_PSO_USER，或存档目录（DM_SAVE，iOS 为 Documents/save）的父目录下 pso_user.tsv；测试每局用新存档副本，互不污染
// User manifest: shaders / pipelines created synchronously on the submit thread while running on this machine, warmed up at the next launch together with the seed manifest.
// Path = DM_PSO_USER, or pso_user.tsv under the parent of the save directory (DM_SAVE, Documents/save on iOS); tests use a fresh save copy per run, so they do not pollute each other
static NSString *pso_user_path(void) {
    const char *u = getenv("DM_PSO_USER"), *save = getenv("DM_SAVE");
    if (u && *u) return @(u);
    return save && *save ? [[@(save) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"pso_user.tsv"] : nil;
}
static FILE *g_pso_user; static int g_pso_user_checked;
static NSMutableSet<NSString *> *g_pso_user_seen;
static FILE *pso_user_file(void) {
    if (g_pso_user_checked) return g_pso_user;
    g_pso_user_checked = 1;
    NSString *path = pso_user_path();
    if (path) g_pso_user = fopen(path.fileSystemRepresentation, "a");
    return g_pso_user;
}
static void pso_user_record(const char *fmt, ...) {
    FILE *f = pso_user_file();
    if (!f) return;
    char line[8192]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (!g_pso_user_seen) {
        g_pso_user_seen = [NSMutableSet set];
        NSString *seed = g_shmap_path ? [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"] : nil;
        NSMutableArray<NSString *> *paths = [NSMutableArray array]; if (seed) [paths addObject:seed]; NSString *user = pso_user_path(); if (user) [paths addObject:user];
        for (NSString *path in paths) { NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            for (NSString *old in [txt componentsSeparatedByString:@"\n"]) if (old.length && ![old hasPrefix:@"#"]) [g_pso_user_seen addObject:old]; }
    }
    size_t ll = strlen(line); NSString *s = [[NSString alloc] initWithBytes:line length:ll && line[ll - 1] == '\n' ? ll - 1 : ll encoding:NSUTF8StringEncoding];   // 与清单文件按行（不含换行）比 | Compared with the manifest file line by line (without the newline)
    if (!s || [g_pso_user_seen containsObject:s]) return;
    [g_pso_user_seen addObject:s]; fputs(line, f); fflush(f);
}
// 从 .metallib 文件建库，不常驻占文件描述符：newLibraryWithURL 在库对象活着期间一直开着那个文件（每个游戏着色器一个——iOS 包进局常驻 258 个描述符里 213 个是它，
// 而 iOS 进程缺省软上限 256；别的幕新建着色器还会涨）。这里把文件映射进内存后立刻关掉，用内存数据建库；映射随库的数据对象释放时解除
// Build the library from a .metallib file without permanently occupying a file descriptor: newLibraryWithURL keeps that file open for as long as the library object lives (one per game shader - of the 258 descriptors the iOS app permanently holds in a run, 213 are these,
// while the default soft limit of an iOS process is 256; new shaders created in other acts raise it further). Here the file is mapped into memory and closed immediately, and the library is built from the in-memory data; the mapping is released when the library's data object is freed
static id<MTLLibrary> lib_from_file(NSString *path, NSError **err) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) { if (fd >= 0) close(fd); if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"打不开 %@", path]}]; return nil; }
    size_t len = (size_t)st.st_size; void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"映射失败 %@", path]}]; return nil; }
#ifdef DM_SHELL
    // 壳：使用者的电脑上没有苹果的着色器编译器时（tools/d2rm_pack.py 的不调编译器模式，gfx/tools/nometal_run.py），「库文件」里放的是
    // 着色器源码而不是编好的库（编好的库以 MTLB 开头）。这里就地现编；系统会把编好的结果缓存，同一个着色器第二次启动起不再花这个时间。
    // 真机实测（冷）：平均约 80 毫秒一个、单个最长约 450 毫秒；一局用到几百个。
    // Shell: when the user's computer has no Apple shader compiler (the no-compiler mode of tools/d2rm_pack.py, gfx/tools/nometal_run.py), the "library files" contain
    // shader source rather than compiled libraries (a compiled library starts with MTLB). They are compiled here on the spot; the system caches the compiled result, and from the second launch on the same shader no longer costs this time.
    // Measured on a real device (cold): about 80 ms each on average, about 450 ms for the longest single one; one session uses several hundred.
    if (len < 4 || memcmp(m, "MTLB", 4)) { NSString *src = [[NSString alloc] initWithBytes:m length:len encoding:NSUTF8StringEncoding]; munmap(m, len);
        if (!src) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:EILSEQ userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"既不是编好的库也不是源码 %@", path]}]; return nil; }
        static _Atomic int n; static _Atomic long long us; CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        id<MTLLibrary> l = [g_dev newLibraryWithSource:src options:nil error:err]; long long dt = (long long)((CFAbsoluteTimeGetCurrent() - t0) * 1e6);
        int k = atomic_fetch_add(&n, 1) + 1; long long tot = atomic_fetch_add(&us, dt) + dt;
        if (k == 1 || k % 100 == 0 || dt >= 300000) MLOG("着色器现编：第 %d 个 %.0f ms，累计 %.1f 秒", k, dt / 1e3, tot / 1e6);
        return l; }
#endif
    dispatch_data_t d = dispatch_data_create(m, len, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(m, len); });
    return [g_dev newLibraryWithData:d error:err];
}
static NvShader *shader_load(NSString *pc, int stage) {
    NSError *e = nil;
    NSString *lp = [NSString stringWithFormat:@"%@/%@.metallib", g_msl_dir, pc];
    id<MTLLibrary> lib = lib_from_file(lp, &e);
    NSData *jd = [NSData dataWithContentsOfFile:[NSString stringWithFormat:@"%@/%@.json", g_msl_dir, pc]];
    NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (!lib || !j) { MLOG("【红】加载着色器失败 %s：%s", pc.UTF8String, e ? e.localizedDescription.UTF8String : "无反射 json"); return nil; }
    if (![j[@"chain"] isEqual:@DMSH_CHAIN]) { MLOG("【红】着色器 %s 的翻译链版本 %s 与本宿主期望的 %s 不符", pc.UTF8String, [[j[@"chain"] description] UTF8String], DMSH_CHAIN); return nil; }
    NvShader *o = [NvShader new];
    char err[160];
    if (nvn_refl_from_json(j, &o->rf, err, sizeof err)) { MLOG("【红】着色器 %s 反射转换失败：%s", pc.UTF8String, err); return nil; }
    if (rt_range_on()) o->range_ro=rt_range_bindings_ro(j);
    o->fn = [lib newFunctionWithName:j[@"entry"]]; o->stage = stage; strlcpy(o->key, pc.UTF8String, sizeof o->key);
    if (o->rf.nb) {
        o->ae = [o->fn newArgumentEncoderWithBufferIndex:o->rf.abi];
        int max_aid = -1;
        for (int i = 0; i < o->rf.nb; i++) max_aid = MAX(max_aid, o->rf.b[i].aid);
        o->arg_len = o->ae.encodedLength;
        if (o->arg_len != (NSUInteger)(8 * (max_aid + 1)) || (o->arg_len & 7))
            MDIE("着色器 %s 参数块布局异常：encodedLength=%lu maxAid=%d", o->key, (unsigned long)o->arg_len, max_aid);
    }
    if (o->rf.has_c1) c1_store(o);
    return o;
}

static int stage_of(NSString *zh) {
    if ([zh isEqualToString:@"顶点"]) return 0; if ([zh isEqualToString:@"像素"]) return 1; if ([zh isEqualToString:@"计算"]) return 5;
    if ([zh isEqualToString:@"外壳"]) return 3; if ([zh isEqualToString:@"域"]) return 4; return -1;
}
#define NVN_RT_RANGE_RO_IMPL 1
// 私有续用只接受明确RO的B反射，sbuf缺rw不把默认0当证明；不改变既有反射/翻译链。
// Private continuation accepts only B reflection that is explicitly RO; a missing rw on sbuf does not let the default 0 count as proof; does not change the existing reflection / translation chain.
static int rt_range_bindings_ro(NSDictionary *j) {
    if (![j[@"route"] isEqual:@"B"] || ![j[@"bindings"] isKindOfClass:[NSArray class]] ||
        ![j[@"drv_reads"] isKindOfClass:[NSDictionary class]] || [j[@"drv_reads"] count]) return 0;
    for (NSDictionary *b in j[@"bindings"]) {
        if (![b isKindOfClass:[NSDictionary class]] || ![b[@"kind"] isKindOfClass:[NSString class]] || ![b[@"msl_type"] isKindOfClass:[NSString class]]) return 0;
        NSString *k=b[@"kind"], *t=b[@"msl_type"];
        if (b[@"rw"] && (![b[@"rw"] isKindOfClass:[NSNumber class]] || [b[@"rw"] boolValue])) return 0;
        if ([k isEqual:@"cbuf"] || [k isEqual:@"c1"]) { if (![t hasPrefix:@"constant "]) return 0; }
        else if ([k isEqual:@"sbuf"]) { if (![b[@"rw"] isKindOfClass:[NSNumber class]] || [b[@"rw"] boolValue] || ![t hasPrefix:@"const device "]) return 0; }
        else if ([k isEqual:@"samp"] || [k isEqual:@"sampt"]) { if (![t isEqual:@"sampler"]) return 0; }
        else if ([k isEqual:@"tex"] || [k isEqual:@"tbuf"]) {
            if (![t hasPrefix:@"texture"] || ([t containsString:@"access::"] && ![t containsString:@"access::sample>"] && ![t containsString:@"access::read>"])) return 0; }
        else return 0;
    }
    return 1;
}
static int rt_range_pr_ro(NvProg *p) {
    if (!p || !p->sh[0] || !p->sh[1]) return 0;
    for (int k=0;k<6;k++) if (p->sh[k] && (k>1 || p->sh[k]->stage != k || !p->sh[k]->range_ro || p->sh[k]->rf.drv_read[0] || p->sh[k]->rf.drv_read[2])) return 0;
    return 1;
}
static NvShader *shader_get(NSString *pc, int stage) {
    if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
    NvShader *o = g_shcache[pc];
    if (o) return o;
    uint64_t bt = bp_now(); o = shader_load(pc, stage);
    if (!o) return nil;
    g_shcache[pc] = o; atomic_fetch_add(&g_shcache_n, 1);
    pso_record("S\t%s\t%d\n", pc.UTF8String, stage);
    pso_user_record("S\t%s\t%d\n", pc.UTF8String, stage);
    g_bp_lib_n++; g_bp_lib_ns += bp_now() - bt;
    return o;
}
static id<MTLLibrary> g_builtin_lib;
static NSMutableDictionary<NSString *, NvShader *> *g_builtin;
static NvShader *builtin_get(const char *ctl) {
    for (unsigned i = 0; i < sizeof k_nvn_builtin_map / sizeof *k_nvn_builtin_map; i++) {
        if (strcmp(ctl, k_nvn_builtin_map[i].ctl)) continue;
        if (!g_builtin) g_builtin = [NSMutableDictionary dictionary];
        NvShader *o = g_builtin[@(ctl)];
        if (o) return o;
        if (!g_builtin_lib) {
            NSError *e = nil; g_builtin_lib = [g_dev newLibraryWithSource:@(k_nvn_builtin_msl) options:nil error:&e];
            if (!g_builtin_lib) MDIE("内嵌计算核 MSL 编译失败：%s", e.localizedDescription.UTF8String);
        }
        o = [NvShader new]; o->fn = [g_builtin_lib newFunctionWithName:@(k_nvn_builtin_map[i].fn)]; o->stage = 5; o->builtin = k_nvn_builtin_map[i].fn;
        g_builtin[@(ctl)] = o;
        return o;
    }
    return nil;
}
static __unsafe_unretained NvProg *prog_get(const prog_t *cp) {   // 只在提交线程调用 | Called only on the submit thread
    prog_t *p = (prog_t *)cp;
    static const prog_t *lp; static uint64_t lser, lgen; static NvProg *lpr;   // 上次结果：同一对象、同一序号且序号表未变即直接用 | Last result: same object, same serial and unchanged serial table means it is used directly
    uint64_t gen = g_ser_gen;
    if (p == lp && p->serial == lser && gen == lgen && lpr) return lpr;
    __unsafe_unretained NvProg *have = p->serial ? serial_get(p->serial) : nil;
    if (have) { lp = p; lser = p->serial; lgen = gen; lpr = have; return have; }
    shmap_load();
    NvProg *o = [NvProg new];
    for (unsigned i = 0; i < p->nstage; i++) {
        uint8_t md[32]; CC_SHA256(p->sd[i].control, 0x880, md);
        NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
        NSString *zh = g_shmap[k];
        if (!zh) {   // 不在包里：查 main 本体内嵌的计算核（nvn_builtin.h） | Not in the package: look up the compute kernels embedded in the main body (nvn_builtin.h)
            NvShader *b = builtin_get(k.UTF8String);
            if (b) { o->sh[5] = b; continue; }
            MLOG("【红】着色器控制段 %s 不在映射表里", k.UTF8String); continue;
        }
        int st = stage_of(zh);
        NvShader *sh = st >= 0 ? shader_get([k stringByAppendingString:@"_b"], st) : nil;
        if (sh) o->sh[st] = sh; else MLOG("【红】着色器 %s（%s）加载失败：先跑 dm/gfx/tools/dmsh_build.py 出包", k.UTF8String, zh.UTF8String);
    }
    if (p->serial) serial_put(p->serial, o);
    return o;
}
void mtl_prog_release(prog_t *p) { if (p->serial) serial_del(p->serial); }

// ---------- 描述符池：贴图号 / 采样器号 → 对象 ----------
// 登记时快照整个贴图对象（NVN 语义：描述符已写进池）；view：该号对应的 MTLTexture（视图或基贴图，CFBridgingRetain 持有）及取得时的序号表 / 客体写代号
// ---------- Descriptor pool: texture ID / sampler ID → object ----------
// At registration the whole texture object is snapshotted (NVN semantics: the descriptor has been written into the pool); view: the MTLTexture for that ID (a view or the base texture, held via CFBridgingRetain) and the serial-table / guest-write generations at the time it was obtained
typedef struct { tex_t t; int ok; view_t v; int has_v; void *view; uint64_t view_id, view_rid, view_ser, view_dirty; } treg_t;
static treg_t *g_treg; static int g_ntreg;
// 贴图号 → 序号的无锁副本（回放热路径只要序号：写回前瞻索引、纯清除前提的绑定集合）：贴图号 20 位，固定 1M 项（BSS，只占碰过的页），
// 登记时 release 写、读端 relaxed 原子读，不进 g_rmu（原 h_ser 每次查表加解锁，营地每帧数千次）
// Lock-free copy of texture ID → serial (the replay hot path only needs the serial: store lookahead index, binding set of the clear-only hoist): texture IDs are 20 bits, fixed 1M entries (BSS, only touched pages are used),
// release write at registration, relaxed atomic read on the reader side, without entering g_rmu (the former h_ser locked and unlocked on every lookup, thousands of times per frame in the camp)
static _Atomic uint64_t g_tser[1 << 20];
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static void **g_sview; static uint64_t *g_srid; static int g_nsreg;   // g_sview：采样器号 → MTLSamplerState（持有） | g_sview: sampler ID → MTLSamplerState (retained)
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    int id_ = tid; if (id_ < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id_ >= g_ntreg) { int n = id_ + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    g_reg_gen++;
    if (g_treg[id_].view) { defer_obj_unref((__bridge id)g_treg[id_].view); CFRelease(g_treg[id_].view); g_treg[id_].view = NULL; g_treg[id_].view_id = g_treg[id_].view_rid = 0; }
    g_treg[id_].ok = t != NULL; if (t) g_treg[id_].t = *t; g_treg[id_].has_v = v != NULL; if (v) g_treg[id_].v = *v;
    if (id_ < (1 << 20)) atomic_store_explicit(&g_tser[id_], t ? t->serial : 0, memory_order_release);
    pthread_mutex_unlock(&g_rmu);
}
void mtl_reg_samp(int sid, const samp_par *sp) {
    int id_ = sid; if (id_ < 0) return;
    pthread_mutex_lock(&g_rmu);
    if (id_ >= g_nsreg) { int n = id_ + 256; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); g_sview = realloc(g_sview, n * sizeof *g_sview); g_srid = realloc(g_srid, n * sizeof *g_srid);
        memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg); memset(g_sview + g_nsreg, 0, (n - g_nsreg) * sizeof *g_sview); memset(g_srid + g_nsreg, 0, (n - g_nsreg) * sizeof *g_srid); g_nsreg = n; }
    if (g_sview[id_]) { CFRelease(g_sview[id_]); g_sview[id_] = NULL; g_srid[id_] = 0; }
    g_reg_gen++;
    g_sreg[id_] = *sp; g_sreg_ok[id_] = 1;
    pthread_mutex_unlock(&g_rmu);
}
static MTLTextureType ttype(int target, int fallback) {
    switch (target) { case 0: return MTLTextureType1D; case 1: case 7: return MTLTextureType2D; case 2: return MTLTextureType3D;
        case 3: return MTLTextureType1DArray; case 4: return MTLTextureType2DArray; case 5: return MTLTextureType2DMultisample;
        case 6: return MTLTextureType2DMultisampleArray; case 8: return MTLTextureTypeCube; case 9: return MTLTextureTypeCubeArray; }
    return (MTLTextureType)fallback;
}
// 池外贴图号 = 空槽：游戏自己用 GetSeparateTextureHandle(0xFFFFF) 取句柄绑到不用的槽（实测调用点 main+0x1746820，
// 贴图池容量 15256），从不登记该号。按 D3D / 空槽语义读出 0（与未绑同一套全零哑贴图），不报红；池内未登记的号仍报红
// Out-of-pool texture ID = empty slot: the game itself uses GetSeparateTextureHandle(0xFFFFF) to get a handle and bind it to unused slots (measured call site main+0x1746820,
// texture pool capacity 15256) and never registers that ID. Read 0 per D3D / empty-slot semantics (the same all-zero dummy textures as unbound), no failure; unregistered IDs inside the pool are still reported a failure
static int g_tpool_n; static uint64_t g_stat_tex_oob;
void mtl_set_tex_pool_n(int n) { g_tpool_n = n; }
static id<MTLTexture> tex_by_id(int tid) {   // 调用方持 g_rmu | Caller holds g_rmu
    if (g_tpool_n && tid >= g_tpool_n) { if (!g_stat_tex_oob++) MLOG("池外贴图号 %d（池容量 %d）按空槽处理：读出 0", tid, g_tpool_n); return nil; }
    if (tid < 0 || tid >= g_ntreg || !g_treg[tid].ok) { RED_ONCE(10, "贴图号 %d 未登记", tid); return nil; }
    treg_t *rp = &g_treg[tid];
    uint64_t sg = g_ser_gen, dg = nvn_dirty_now();   // 先取代号再查：期间若有变更，下次必走慢路 | Take the generations first and then look up: if a change happens in between, the next time must take the slow path
    if (rp->view && rp->view_ser == sg && rp->view_dirty == dg) return (__bridge id<MTLTexture>)rp->view;
    id<MTLTexture> v = (__bridge id<MTLTexture>)rp->view;
    if (v) {   // 序号表或客体内存有过变动：核基贴图是否被重传换新，换了则视图作废 | The serial table or guest memory has changed: check whether the base texture was re-uploaded and replaced; if so the view is invalidated
        id<MTLTexture> cur = tex_mtl(&rp->t);
        if (cur == v || v.parentTexture == cur) { rp->view_ser = sg; rp->view_dirty = dg; return v; }
        defer_obj_unref(v); CFRelease(rp->view); rp->view = NULL; rp->view_rid = 0; v = nil;
    }
    treg_t r = *rp;
    id<MTLTexture> base = tex_mtl(&rp->t);
    if (!base) return nil;
    v = base;
    if (r.has_v && (r.v.set_format || r.v.set_levels || r.v.set_layers || r.v.set_target)) {
        uint32_t pf = (uint32_t)base.pixelFormat; uint8_t swz[4];
        if (r.v.set_format && !(nvn_fmt_to_mtl(r.v.format, &pf, swz) && pf)) pf = (uint32_t)base.pixelFormat;
        { uint32_t upf; int ubpp; if (r.v.set_format && bc_unpack(r.v.format, &upf, &ubpp)) pf = upf; }   // 无 BC：BC 视图跟底图一样换成解压后的格式 | No BC: a BC view is switched to the decompressed format just like the base texture
        if (r.v.set_format && tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) {
            base = tex_upgrade_pfv(&rp->t, base, (MTLPixelFormat)pf);
            sg = g_ser_gen;
        }
        NSUInteger l0 = r.v.set_levels ? r.v.base_level : 0, nl = r.v.set_levels && r.v.nlevels ? r.v.nlevels : base.mipmapLevelCount - l0;
        NSUInteger s0 = r.v.set_layers ? r.v.min_layer : 0;
        NSUInteger ns = base.textureType == MTLTextureType3D ? 1 : (r.v.set_layers && r.v.nlayers ? r.v.nlayers : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1) - s0);
        MTLTextureType tt = r.v.set_target ? ttype(r.v.target, base.textureType) : base.textureType;
        if (tt == MTLTextureType2D && ns > 1) ns = 1;
        v = [base newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(l0, nl) slices:NSMakeRange(s0, ns)];
        if (!v) { RED_ONCE(11, "贴图视图建不出 id %d", tid); v = base; }
    }
    rp->view = (void *)CFBridgingRetain(v); rp->view_rid = v.gpuResourceID._impl; rp->view_ser = sg; rp->view_dirty = dg;
    return v;
}
// 热路径取对象：表里有效就直接返回表持有的指针（不经 ARC 的 retain / autorelease），否则走慢路建 / 校验。调用方持 g_rmu。
// Hot-path object fetch: if the table entry is valid, return the table-held pointer directly (without ARC retain / autorelease), otherwise take the slow path to build / validate. Caller holds g_rmu.
static inline __attribute__((always_inline)) void *tex_fast(int tid) {
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == g_ser_gen && g_treg[tid].view_dirty == nvn_dirty_now()) return g_treg[tid].view;
    return (__bridge void *)tex_by_id(tid);
}
static inline __attribute__((always_inline)) uint64_t tex_fast_rid(int tid, void **out) {
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == g_ser_gen && g_treg[tid].view_dirty == nvn_dirty_now()) { if (out) *out = g_treg[tid].view; return g_treg[tid].view_rid; }
    id<MTLTexture> t = tex_by_id(tid); if (out) *out = (__bridge void *)t; return t ? t.gpuResourceID._impl : 0;
}
static MTLSamplerAddressMode wrapm(int w) {
    switch (w) { case 0: case 7: return MTLSamplerAddressModeClampToEdge; case 1: return MTLSamplerAddressModeRepeat;
        case 3: case 2: return MTLSamplerAddressModeMirrorClampToEdge; case 5: case 4: return MTLSamplerAddressModeClampToBorderColor;
        case 6: return MTLSamplerAddressModeMirrorRepeat; }
    return MTLSamplerAddressModeRepeat;
}
static id<MTLSamplerState> samp_by_id(int sid) {   // 调用方持 g_rmu | Caller holds g_rmu
    if (sid < 0 || sid >= g_nsreg || !g_sreg_ok[sid]) { RED_ONCE(12, "采样器号 %d 未登记", sid); return nil; }
    if (g_sview[sid]) return (__bridge id<MTLSamplerState>)g_sview[sid];
    samp_par p = g_sreg[sid];
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = p.magf ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (p.minf == 1 || p.minf == 3 || p.minf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = p.minf <= 1 ? MTLSamplerMipFilterNotMipmapped : (p.minf == 2 || p.minf == 3) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.sAddressMode = wrapm(p.ws); d.tAddressMode = wrapm(p.wt); d.rAddressMode = wrapm(p.wr);
    d.lodMinClamp = p.lod_min < 0 ? 0 : p.lod_min; d.lodMaxClamp = p.lod_max;
    d.maxAnisotropy = p.aniso < 1 ? 1 : p.aniso > 16 ? 16 : (NSUInteger)p.aniso;
    if (p.cmp_mode) d.compareFunction = (MTLCompareFunction)(p.cmp_func - 1);
    d.borderColor = p.border[3] > 0.5f ? (p.border[0] > 0.5f ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack) : MTLSamplerBorderColorTransparentBlack;
    d.supportArgumentBuffers = YES;
    id<MTLSamplerState> s = [g_dev newSamplerStateWithDescriptor:d];
    if (s) { g_sview[sid] = (void *)CFBridgingRetain(s); g_srid[sid] = s.gpuResourceID._impl; }
    return s;
}
static inline __attribute__((always_inline)) void *samp_fast(int sid) {
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid] && g_sview[sid]) return g_sview[sid];
    return (__bridge void *)samp_by_id(sid);
}

// ---------- 参数缓冲区块：每次提交一块，GPU 完成后回收 ----------
// ---------- Argument buffer blocks: one block per submission, reclaimed after the GPU completes ----------
@interface NvArena : NSObject { @public id<MTLBuffer> buf; uint64_t gpu, fence; size_t used; } @end
@implementation NvArena @end
static NSMutableArray<NvArena *> *g_arenas; static NvArena *g_cur_arena; static NSMutableArray<NvArena *> *g_sub_arenas;
static NvArena *arena_new(void) {
    // for-in 循环变量不持有对象：先取强引用再从数组删，否则数组释放后返回的是野指针
    // The for-in loop variable does not retain the object: take a strong reference first and then remove it from the array, otherwise what is returned after the array releases it is a dangling pointer
    for (NvArena *it in g_arenas) if (it->fence && it->fence <= g_done) { NvArena *r = it; [g_arenas removeObject:r]; r->fence = 0; r->used = 0; return r; }
    NvArena *a = [NvArena new]; a->buf = [g_dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared]; a->gpu = a->buf.gpuAddress; res_add(a->buf); return a;
}
void arena_begin(void) { if (!g_arenas) { g_arenas = [NSMutableArray array]; g_sub_arenas = [NSMutableArray array]; } g_cur_arena = nil; }
void arena_commit(uint64_t fence) { for (NvArena *a in g_sub_arenas) { a->fence = fence; [g_arenas addObject:a]; } [g_sub_arenas removeAllObjects]; g_cur_arena = nil; }
// 输出参数写成 __strong：ARC 下不加修饰的 id * 是 __autoreleasing，每次调用（每次绘制两次）都把 arena 缓冲放进自动释放池
// The out-parameter is __strong: an unqualified id * is __autoreleasing under ARC, which put the arena buffer in the autorelease pool at every call (twice per draw)
static size_t arena_alloc(size_t n, __strong id<MTLBuffer> *buf) {
    n = (n + 255) & ~(size_t)255;
    if (!g_cur_arena || g_cur_arena->used + n > g_cur_arena->buf.length) {
        pthread_mutex_lock(&g_mu); g_cur_arena = arena_new(); pthread_mutex_unlock(&g_mu);
        [g_sub_arenas addObject:g_cur_arena];
    }
    size_t off = g_cur_arena->used; g_cur_arena->used += n; *buf = g_cur_arena->buf; return off;
}

// ---------- 绘制状态 ----------
// ---------- Draw state ----------
static struct {
    const prog_t *prog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[16]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16];
    uint64_t tex[6][128], samp[6][128], img[6][32];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; int has_vp;
    float blend_color[4]; int64_t copy_stride, copy_image_stride;
} S;
static void udc_note_draw(NvProg *pr) {
    // 同一目标、全零集合代号没变：上次已把它删掉，不必再扫一遍集合（集合最长 4096 项，每次绘制每个颜色附件扫一次）
    // Same target, all-zero set generation unchanged: it was already removed last time, so no need to scan the set again (the set has up to 4096 entries, scanned once per color attachment per draw)
    static uint64_t del_ser[8], del_gen;
    if (R.has_rt) for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) { uint64_t sr = R.rt.color[i]->serial;
        if (!g_la_cache || del_gen != g_zskip_gen || del_ser[i] != sr) { zskip_del(sr); del_ser[i] = sr; }
        g_zskip_cand_ok[i] = 0; }   // 零源集合：真画了就不再是全零 | Zero-source set: once really drawn it is no longer all zeros
    del_gen = g_zskip_gen;
    // 本程序各阶段有没有图像槽 / 可写图像槽、片元阶段写哪些颜色：只取决于 sh[]，算一次挂在程序上（sh[] 换了就重算）
    // Whether each stage of this program has image slots / writable image slots, and which colors the fragment stage writes: depends only on sh[], computed once and attached to the program (recomputed when sh[] changes)
    if (pr && g_la_cache) {
        int ok = pr->fl_ok; for (int st = 0; ok && st < 6; st++) if (pr->fl_sh[st] != pr->sh[st]) ok = 0;
        if (!ok) { pr->fl_img = pr->fl_img_w = 0; pr->fl_cmask = 0;
            for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; pr->fl_sh[st] = sh; if (!sh) continue; if (st == 1) pr->fl_cmask = sh->rf.color_mask;
                for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG) { pr->fl_img = 1; if (sh->rf.b[i].rw) pr->fl_img_w = 1; } }
            pr->fl_ok = 1; }
    }
    if (pr && g_la_cache) { if (pr->fl_img) zskip_clear(); }
    else if (pr) for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue; for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG) { zskip_clear(); break; } }   // 图像槽可写：保守清空 | Image slots are writable: conservatively clear
    int image_write = 0;
    uint32_t shader_color_mask = 0;
    if (pr && g_la_cache) { image_write = pr->fl_img_w; shader_color_mask = pr->fl_cmask; }
    else if (pr) for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        if (st == 1) shader_color_mask = sh->rf.color_mask;
        for (int i = 0; i < sh->rf.nb; i++) if (sh->rf.b[i].kind == NVN_RB_IMG && sh->rf.b[i].rw) { image_write = 1; break; }
        if (image_write) break;
    }
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i];
        if (image_write) { t->wrote = 1; t->wwhy = 1; }
        else if (t->depth == 0 && t->idx >= 0 && (((S.chmask.mask >> (4 * t->idx)) & 0xf) || (shader_color_mask >> t->idx & 1))) { t->wrote = 1; t->wwhy = (S.chmask.mask >> (4 * t->idx)) & 0xf ? 2 : 3; }
        else if (t->depth == 1 && S.ds.dw) { t->wrote = 1; t->wwhy = 4; }
        else if (t->depth == 2 && S.ds.st && (S.st_wmask[0] || S.st_wmask[1])
                && (S.ds.fail_f != NVN_STENCIL_OP_KEEP || S.ds.zfail_f != NVN_STENCIL_OP_KEEP || S.ds.zpass_f != NVN_STENCIL_OP_KEEP
                    || S.ds.fail_b != NVN_STENCIL_OP_KEEP || S.ds.zfail_b != NVN_STENCIL_OP_KEEP || S.ds.zpass_b != NVN_STENCIL_OP_KEEP)) { t->wrote = 1; t->wwhy = 5; }
    }
}
static void state_reset(void) {
    memset(&S, 0, sizeof S);
    g_pso_dirty = g_dss_dirty = 1;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
}

static MTLVertexFormat vfmt(int f) {
    switch (f) {
        case 0x01: return MTLVertexFormatUCharNormalized; case 0x02: return MTLVertexFormatCharNormalized; case 0x03: return MTLVertexFormatUChar; case 0x04: return MTLVertexFormatChar;
        case 0x05: return MTLVertexFormatHalf; case 0x06: return MTLVertexFormatUShortNormalized; case 0x07: return MTLVertexFormatShortNormalized;
        case 0x08: return MTLVertexFormatUShort; case 0x09: return MTLVertexFormatShort; case 0x0A: return MTLVertexFormatFloat; case 0x0B: return MTLVertexFormatUInt; case 0x0C: return MTLVertexFormatInt;
        case 0x0D: return MTLVertexFormatUChar2Normalized; case 0x0E: return MTLVertexFormatChar2Normalized; case 0x0F: return MTLVertexFormatUChar2; case 0x10: return MTLVertexFormatChar2;
        case 0x11: return MTLVertexFormatHalf2; case 0x12: return MTLVertexFormatUShort2Normalized; case 0x13: return MTLVertexFormatShort2Normalized; case 0x14: return MTLVertexFormatUShort2;
        case 0x15: return MTLVertexFormatShort2; case 0x16: return MTLVertexFormatFloat2; case 0x17: return MTLVertexFormatUInt2; case 0x18: return MTLVertexFormatInt2;
        case 0x19: return MTLVertexFormatUChar3Normalized; case 0x1A: return MTLVertexFormatChar3Normalized; case 0x1B: return MTLVertexFormatUChar3; case 0x1C: return MTLVertexFormatChar3;
        case 0x1D: return MTLVertexFormatHalf3; case 0x1E: return MTLVertexFormatUShort3Normalized; case 0x1F: return MTLVertexFormatShort3Normalized; case 0x20: return MTLVertexFormatUShort3;
        case 0x21: return MTLVertexFormatShort3; case 0x22: return MTLVertexFormatFloat3; case 0x23: return MTLVertexFormatUInt3; case 0x24: return MTLVertexFormatInt3;
        case 0x25: return MTLVertexFormatUChar4Normalized; case 0x26: return MTLVertexFormatChar4Normalized; case 0x27: return MTLVertexFormatUChar4; case 0x28: return MTLVertexFormatChar4;
        case 0x29: return MTLVertexFormatHalf4; case 0x2A: return MTLVertexFormatUShort4Normalized; case 0x2B: return MTLVertexFormatShort4Normalized; case 0x2C: return MTLVertexFormatUShort4;
        case 0x2D: return MTLVertexFormatShort4; case 0x2E: return MTLVertexFormatFloat4; case 0x2F: return MTLVertexFormatUInt4; case 0x30: return MTLVertexFormatInt4;
        case 0x3D: return MTLVertexFormatUInt1010102Normalized; case 0x3F: return MTLVertexFormatFloatRG11B10; case 0x9A: return MTLVertexFormatUChar4Normalized_BGRA;
    }
    return MTLVertexFormatInvalid;
}
static MTLBlendFactor bfac(int f) {
    switch (f) { case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha; case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return MTLBlendFactorDestinationColor; case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 0x10: return MTLBlendFactorSource1Color; case 0x11: return MTLBlendFactorOneMinusSource1Color; case 0x12: return MTLBlendFactorSource1Alpha; case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
        case 0x61: return MTLBlendFactorBlendColor; case 0x62: return MTLBlendFactorOneMinusBlendColor; case 0x63: return MTLBlendFactorBlendAlpha; case 0x64: return MTLBlendFactorOneMinusBlendAlpha; }
    return MTLBlendFactorOne;
}
static MTLBlendOperation bop(int e) { switch (e) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract; case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; } return MTLBlendOperationAdd; }

// 管线：按（程序, 目标格式, 混合 / 写掩码, 顶点布局, 采样数）缓存
// Pipeline: cached by (program, target formats, blend / write mask, vertex layout, sample count)
static const uint8_t *g_pso_lmap; static id<MTLFunction> g_pso_fsfn; static pso_fz_desc g_pso_fz, g_fz_desc;
static os_unfair_lock g_fz_fn_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSString *, id<MTLFunction>> *g_bf_fn_cache;
static NSMutableDictionary<NSString *, id<MTLLibrary>> *g_bf_lib_cache;
static nvn_hmap g_pso = {.klen = sizeof(pso_key)}, g_dss;
static MTLRenderPipelineDescriptor *pso_desc(const pso_key *k, NvShader *vs, NvShader *fs, id<MTLFunction> fsfn, int merged) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs->fn; d.fragmentFunction = fsfn ? fsfn : fs ? fs->fn : nil;
    // 诊断 DM_NVN_HILITE=片元着色器键,…：这些绘制改用纯色片元（第 1–6 个键依次 品红 / 绿 / 青 / 黄 / 蓝 / 白），看几何落在哪里
    // Diagnostic DM_NVN_HILITE=fragment shader key,...: these draws use a solid-color fragment instead (keys 1–6 in order magenta / green / cyan / yellow / blue / white), to see where the geometry lands
    if (fs) { static char keys[6][24]; static int nk = -1; static id<MTLLibrary> hl;
        if (nk < 0) { nk = 0; const char *e = getenv("DM_NVN_HILITE");
            for (const char *q = e; q && *q && nk < 6; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 23) n = 23; memcpy(keys[nk], q, n); keys[nk++][n] = 0; } }
        for (int i = 0; i < nk; i++) if (!strncmp(fs->key, keys[i], strlen(keys[i]))) {
            if (!hl) { NSError *e = nil; hl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };\n"
                "fragment O hl0() { return {float4(1,0,1,1),float4(1,0,1,1),float4(1,0,1,1)}; } fragment O hl1() { return {float4(0,1,0,1),float4(0,1,0,1),float4(0,1,0,1)}; }\n"
                "fragment O hl2() { return {float4(0,1,1,1),float4(0,1,1,1),float4(0,1,1,1)}; } fragment O hl3() { return {float4(1,1,0,1),float4(1,1,0,1),float4(1,1,0,1)}; }\n"
                "fragment O hl4() { return {float4(0,0,1,1),float4(0,0,1,1),float4(0,0,1,1)}; } fragment O hl5() { return {float4(1,1,1,1),float4(1,1,1,1),float4(1,1,1,1)}; }\n" options:nil error:&e];
                if (!hl) MLOG("【红】HILITE 着色器编译失败 %s", e.localizedDescription.UTF8String); }
            if (hl) d.fragmentFunction = [hl newFunctionWithName:[NSString stringWithFormat:@"hl%d", i]];
            break; } }
    d.rasterSampleCount = k->samples ? k->samples : 1;
    d.alphaToCoverageEnabled = k->a2c;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)k->cfmt[i];
        int l = merged ? k->lmap[i] : i;
        uint32_t m = l == 0xff ? 0 : (k->chmask >> (4 * l)) & 0xf;
        c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        if (l != 0xff && k->blend_en >> l & 1) {
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = bfac(k->blend[l].src_rgb); c.destinationRGBBlendFactor = bfac(k->blend[l].dst_rgb);
            c.sourceAlphaBlendFactor = bfac(k->blend[l].src_a); c.destinationAlphaBlendFactor = bfac(k->blend[l].dst_a);
            c.rgbBlendOperation = bop(k->blend[l].eq_rgb); c.alphaBlendOperation = bop(k->blend[l].eq_a);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) {
        int loc = rf->v[v].loc, a = rf->v[v].nvn;   // Metal 槽 loc ← NVN 属性 a | Metal slot loc ← NVN attribute a
        MTLVertexFormat vf = a < 16 ? vfmt(k->attr[a].fmt) : MTLVertexFormatInvalid;
        // a >= S.nattr 时 k.attr[a] 保持 memset 的 0，vfmt(0) 为 Invalid，走同一回退分支且 fmt 非 0 的 RED_ONCE 条件不成立。
        // When a >= S.nattr, k.attr[a] keeps the 0 from memset, vfmt(0) is Invalid, the same fallback branch is taken and the RED_ONCE condition of fmt being non-0 does not hold.
        if (a < 16 && k->attr[a].fmt && vf == MTLVertexFormatInvalid) RED_ONCE(15, "顶点属性格式 %#x 未映射", k->attr[a].fmt);
        if (vf == MTLVertexFormatInvalid) {
            int ct = rf->v[v].ct;
            vd.attributes[loc].format = ct == NVN_CT_UINT ? MTLVertexFormatUInt4 : ct == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4;
            vd.attributes[loc].offset = 0; vd.attributes[loc].bufferIndex = 30;
            vd.layouts[30].stride = 16; vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30].stepRate = 0;
            continue;
        }
        int st = k->attr[a].stream;
        vd.attributes[loc].format = vf; vd.attributes[loc].offset = k->attr[a].offset; vd.attributes[loc].bufferIndex = 16 + st;
        vd.layouts[16 + st].stride = k->stream[st].stride ? k->stream[st].stride : 4;
        vd.layouts[16 + st].stepFunction = k->stream[st].stride == 0 ? MTLVertexStepFunctionConstant : k->stream[st].divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        vd.layouts[16 + st].stepRate = k->stream[st].stride == 0 ? 0 : k->stream[st].divisor ? k->stream[st].divisor : 1;
    }
    d.vertexDescriptor = vd;
    return d;
}
// 管线键：当前状态 S + 通道格式（普通通道；合并通道另带 lmap / 特化函数，由 pso_get 填）
// Pipeline key: current state S + pass formats (normal pass; a merged pass additionally carries lmap / specialized function, filled in by pso_get)
static void pso_key_fill(pso_key *k, NvShader *vs, NvShader *fs, const uint32_t cfmt[8], uint32_t dfmt, uint32_t sfmt, uint32_t samples, int lmap) {
    memset(k, 0, sizeof *k);
    k->vs = (__bridge void *)vs; k->fs = (__bridge void *)fs;
    memcpy(k->cfmt, cfmt, sizeof k->cfmt); k->dfmt = dfmt; k->sfmt = sfmt; k->samples = samples;
    k->blend_en = S.color.blend_en; k->chmask = S.chmask.mask; k->a2c = S.ms.a2c;
    for (int i = 0; i < 8; i++) if (k->cfmt[i] || lmap) k->blend[i] = S.blend[i];
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) { int a = rf->v[v].nvn; if (a < 16 && a < S.nattr) { k->attr[a] = S.attr[a]; int st = S.attr[a].stream; if (st < 16) k->stream[st] = S.stream[st]; } }
}
// ---- 并行预编：首次遇到的管线原先在提交线程一条条同步编，一帧里连着缺 9 条 = 284 ms（真机重场面）。
// 现在：一次提交里第一次缺管线时，顺着余下的命令往前扫一遍（只重放状态，不画），把后面还会缺的普通通道管线一并丢给后台并行编；
// 提交线程走到那次绘制时只等「还没编完的剩余时间」。编出来的管线与同步编的完全相同（同一份描述符）。合并通道 / 特化片元的管线键依赖回放时的规划结果，不预测，照旧同步编。
// ---- Parallel precompile: pipelines met for the first time used to be compiled synchronously one by one on the submit thread; 9 misses in a row in one frame = 284 ms (heavy scene on a real device).
// Now: on the first pipeline miss in a submission, scan ahead once along the remaining commands (replaying state only, no drawing) and kick all the normal-pass pipelines that will also miss later to the background to compile in parallel;
// when the submit thread reaches that draw it only waits for 'the remaining time not yet compiled'. The compiled pipelines are identical to synchronously compiled ones (same descriptor). Pipeline keys of merged passes / specialized fragments depend on the planning result at replay time, are not predicted, and are compiled synchronously as before.
#define PSO_PEND_MAX 384
static struct { pso_key k; id<MTLRenderPipelineState> ps; int used, done; } g_pp[PSO_PEND_MAX];
static pthread_mutex_t g_pp_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pp_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_pp_submit_mark, g_pp_kicked, g_pp_taken;   // 本次提交是否已预测过（按提交序号）；累计丢出 / 领用条数 | Whether this submission has already been predicted (by submission serial); cumulative kicked / taken counts
static void pso_predict_rest(void);
static int pso_pend_find(const pso_key *k) { for (int i = 0; i < PSO_PEND_MAX; i++) if (g_pp[i].used && !memcmp(&g_pp[i].k, k, sizeof *k)) return i; return -1; }
static void pso_pend_kick(const pso_key *k, NvShader *vs, NvShader *fs) {   // 提交线程调：没在缓存、没在表里才丢 | Called by the submit thread: kick only if it is neither in the cache nor in the table
    if (hm_get_id(&g_pso, k)) return;
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k), slot = -1;
    if (at < 0) for (int i = 0; i < PSO_PEND_MAX; i++) if (!g_pp[i].used) { slot = i; break; }
    if (at < 0 && slot >= 0) { g_pp[slot].k = *k; g_pp[slot].ps = nil; g_pp[slot].done = 0; g_pp[slot].used = 1; }
    pthread_mutex_unlock(&g_pp_mu);
    if (at >= 0 || slot < 0) return;
    MTLRenderPipelineDescriptor *d = pso_desc(k, vs, fs, nil, 0); g_pp_kicked++;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *e = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        pthread_mutex_lock(&g_pp_mu); g_pp[slot].ps = ps; g_pp[slot].done = 1; pthread_cond_broadcast(&g_pp_cv); pthread_mutex_unlock(&g_pp_mu);
    });
}
static id<MTLRenderPipelineState> pso_pend_take(const pso_key *k) {   // 提交线程调：表里有就等它编完并取走；没有返回 nil | Called by the submit thread: if it is in the table, wait for it to finish compiling and take it; otherwise return nil
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k); id<MTLRenderPipelineState> ps = nil;
    if (at >= 0) { while (!g_pp[at].done) pthread_cond_wait(&g_pp_cv, &g_pp_mu); ps = g_pp[at].ps; g_pp[at].ps = nil; g_pp[at].used = 0; }
    pthread_mutex_unlock(&g_pp_mu);
    return ps;
}
static __unsafe_unretained id<MTLRenderPipelineState> pso_get(NvProg *pr, MTLRenderPassDescriptor *unused) {
    (void)unused;
    __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs) { RED_ONCE(13, "程序没有顶点着色器（或未映射）"); return nil; }
    pso_key k; pso_key_fill(&k, vs, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, g_pso_lmap != NULL);
    if (g_pso_lmap) { memcpy(k.lmap, g_pso_lmap, 8); k.fsfn = (__bridge void *)g_pso_fsfn; }
    __unsafe_unretained id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
    if (cached) return cached;
    id<MTLRenderPipelineState> ps;
    NSError *e = nil; uint64_t bt = bp_now();
    ps = g_pso_lmap ? nil : pso_pend_take(&k);
    if (ps) g_pp_taken++;
    else {
        if (g_pp_submit_mark != g_sub_serial) { g_pp_submit_mark = g_sub_serial; pso_predict_rest(); }   // 本次提交第一次缺：先把后面的丢出去并行编，再同步编眼前这条 | First miss in this submission: first kick the later ones out to compile in parallel, then synchronously compile the one at hand
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, g_pso_fsfn, g_pso_lmap != NULL);
        ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    }
    g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
    if (!ps) { MLOG("【红】建渲染管线失败：%s", e.localizedDescription.UTF8String); return nil; }
    pso_skey sk; char shex[sizeof sk * 2 + 1]; pso_skey_from(&k, vs, fs, g_pso_lmap != NULL, g_pso_lmap ? &g_pso_fz : NULL, &sk); pso_skey_hex(&sk, shex);
    pso_record("R\t%s\n", shex);
    pso_user_record("R\t%s\n", shex);
    hm_put_id(&g_pso, &k, ps);
    return ps;
}
static NSString *warm_key(const char *s, size_t n) {
    size_t z = strnlen(s, n);
    return [[NSString alloc] initWithBytes:s length:z encoding:NSUTF8StringEncoding];
}
static NvShader *warm_shader(NSMutableDictionary<NSString *, NvShader *> *dict, NSString *key, int stage) {
    NvShader *sh = dict[key];
    if (!sh) { sh = shader_load(key, stage); if (sh) dict[key] = sh; }
    return sh;
}
static void warm_enqueue(WarmItem *item) {
    os_unfair_lock_lock(&g_warm_lock);
    [g_warm_items addObject:item];
    atomic_store_explicit(&g_warm_ready, 1, memory_order_release);
    os_unfair_lock_unlock(&g_warm_lock);
}
static void warm_worker(NSArray<NSString *> *paths) {
    @autoreleasepool {
        NSMutableDictionary<NSString *, NvShader *> *dict = [NSMutableDictionary dictionary];
        NSMutableSet<NSString *> *seen = [NSMutableSet set];
        for (NSString *path in paths) {
            NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            if (!txt) { MLOG("【红】预热清单读取失败：%s", path.UTF8String); continue; }
            for (NSString *line in [txt componentsSeparatedByString:@"\n"]) {
                if (!line.length || [line hasPrefix:@"#"] || [seen containsObject:line]) continue;
                [seen addObject:line];
                NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
                if (!c.count) continue;
                if ([c[0] isEqualToString:@"S"] && c.count >= 3) {
                    NSString *key = c[1]; NvShader *sh = warm_shader(dict, key, c[2].intValue);
                    if (!sh) continue;
                    WarmItem *item = [WarmItem new]; item->kind = 0; item->key = key; item->shader = sh;
                    warm_enqueue(item); g_warm_shaders++;
                } else if ([c[0] isEqualToString:@"R"] && c.count >= 2) {
                    pso_skey sk; if (pso_skey_parse(c[1].UTF8String, &sk)) continue;
                    NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
                    NvShader *vs = vkey.length ? warm_shader(dict, vkey, sk.vs_stage) : nil;
                    NvShader *fs = fkey.length ? warm_shader(dict, fkey, sk.fs_stage) : nil;
                    if (!vs || (fkey.length && !fs)) continue;
                    id<MTLFunction> fsfn = nil;
                    if (sk.fz) {
                        if (sk.fz_desc.kind == 1 && fs) { int fx[32]; for (int i = 0; i < 32; i++) fx[i] = sk.fz_desc.fx[i]; fsfn = bf_fn(fs, fx, sk.fz_desc.fbmask, sk.k.lmap); }
                                                if (!fsfn) { g_warm_skip_fz++; continue; }
                    }
                    pso_key k = sk.k; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs; k.fsfn = (__bridge void *)fsfn;
                    NSError *e = nil; id ps = [g_dev newRenderPipelineStateWithDescriptor:pso_desc(&k, vs, fs, fsfn, sk.merged) error:&e];
                    if (!ps) { MLOG("【红】后台建渲染管线失败：%s", e.localizedDescription.UTF8String); continue; }
                    WarmItem *item = [WarmItem new]; item->kind = 1; item->vs_key = vkey; item->fs_key = fkey; item->vs = vs; item->fs = fs; item->fsfn = fsfn; item->pso = k; item->object = ps;
                    warm_enqueue(item); g_warm_rps++;
                } else if ([c[0] isEqualToString:@"C"] && c.count >= 2) {
                    NSString *key = c[1]; if (!key.length) continue;
                    NvShader *cs = warm_shader(dict, key, 5);
                    if (!cs) continue;
                    NSError *e = nil; id ps = [g_dev newComputePipelineStateWithFunction:cs->fn error:&e];
                    if (!ps) { MLOG("【红】后台建计算管线失败：%s", e.localizedDescription.UTF8String); continue; }
                    WarmItem *item = [WarmItem new]; item->kind = 2; item->key = key; item->shader = cs; item->object = ps;
                    warm_enqueue(item); g_warm_cps++;
                }
            }
        }
        MLOG("管线预热完成：着色器 %llu 个 / 渲染管线 %llu 个（跳过特化片元 %llu）/ 计算管线 %llu 个，后台用时 %.1f ms",
             g_warm_shaders, g_warm_rps, g_warm_skip_fz, g_warm_cps, (bp_now() - g_warm_t0) / 1e6);
    }
    atomic_store(&g_warm_running, 0);
}
// 等后台预热跑完（最多 timeout_ms）：返回实际等了多少毫秒，没在跑返回 0。首装时系统着色器缓存全冷，预热慢于玩家进局的话，
// 没预热到的管线会在提交线程现编 = 局内卡顿；进局读取画面本来就挡着玩家，让它多读一会儿（hle_nvn.c 出图处调）。
// Wait for the background warm-up to finish (at most timeout_ms): returns how many milliseconds were actually waited, 0 if it is not running. On first install the system shader cache is fully cold; if warm-up is slower than the player entering a run,
// pipelines not yet warmed are compiled on the spot on the submit thread = in-run stutter; the loading screen on entering a run blocks the player anyway, so let it load a little longer (called where hle_nvn.c presents).
int mtl_warm_wait(int timeout_ms) {
    if (!atomic_load(&g_warm_running)) return 0;
    uint64_t t0 = bp_now();
    while (atomic_load(&g_warm_running) && (bp_now() - t0) / 1000000ull < (uint64_t)timeout_ms) usleep(2000);
    return (int)((bp_now() - t0) / 1000000ull) + 1;
}
static void warm_start(void) {
    if (g_warm_started++) return;
    const char *e = getenv("DM_PSO_MANIFEST");
    if (e && !strcmp(e, "off")) return;
    NSArray<NSString *> *paths;
    if (e && *e) {
        paths = @[@(e)];
    } else {
        NSString *seed = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"];
        NSMutableArray<NSString *> *all = [NSMutableArray arrayWithObject:seed];
        NSString *user = pso_user_path();
        if (user && [NSFileManager.defaultManager fileExistsAtPath:user]) [all addObject:user];
        if (![NSFileManager.defaultManager fileExistsAtPath:seed]) return;
        paths = all;
    }
    if (!g_warm_items) g_warm_items = [NSMutableArray array];
    g_warm_t0 = bp_now();
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);   // 不是可有可无的后台活：它决定玩家进局前管线是否就绪 | Not optional background work: it decides whether the pipelines are ready before the player enters a run
    dispatch_queue_t q = dispatch_queue_create("dm.nvn.pso-warm", attr);
    atomic_store(&g_warm_running, 1);
    dispatch_async(q, ^{ warm_worker(paths); });
}
static int g_rt_ovr;   // 运行时外推的模板覆盖：0 不覆盖；1 只在单位 / 特效 / 外推洞位（0xD0）上通过、不写模板；3 照常深度测试，通过处写特效位 0x40 | Stencil override for runtime extrapolation: 0 no override; 1 pass only on the unit / effect / extrapolation-hole bits (0xD0), no stencil write; 3 depth test as usual, write the effect bit 0x40 where it passes
static id<MTLDepthStencilState> dss_get(int has_depth, int has_stencil) {
    struct { ds_st ds; uint32_t vm[2], wm[2]; int hd, hs, ovr; } k;
    memset(&k, 0, sizeof k); k.ds = S.ds; memcpy(k.vm, S.st_vmask, 8); memcpy(k.wm, S.st_wmask, 8); k.hd = has_depth; k.hs = has_stencil; k.ovr = has_stencil ? g_rt_ovr : 0;
    if (!g_dss.klen) g_dss.klen = sizeof k;
    id<MTLDepthStencilState> s = hm_get_id(&g_dss, &k);
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (has_depth && S.ds.dt) { d.depthCompareFunction = (MTLCompareFunction)(S.ds.df - 1); d.depthWriteEnabled = S.ds.dw; }
    if (has_stencil && S.ds.st) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new], *b = [MTLStencilDescriptor new];
        f.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_f - 1); f.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_f - 1);
        f.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_f - 1); f.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_f - 1);
        f.readMask = S.st_vmask[0]; f.writeMask = S.st_wmask[0];
        b.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_b - 1); b.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_b - 1);
        b.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_b - 1); b.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_b - 1);
        b.readMask = S.st_vmask[1]; b.writeMask = S.st_wmask[1];
        d.frontFaceStencil = f; d.backFaceStencil = b;
    }
    if (has_stencil && k.ovr) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new];
        if (k.ovr == 1) { f.stencilCompareFunction = MTLCompareFunctionNotEqual; f.readMask = 0xD0; f.writeMask = 0; }   // 参考值 0：(s & 0xD0) != 0 才通过（单位 0x10、特效 0x40、外推洞 / 屏边 0x80） | Reference value 0: passes only if (s & 0xD0) != 0 (unit 0x10, effect 0x40, extrapolation hole / screen edge 0x80)
        else { f.stencilCompareFunction = MTLCompareFunctionAlways; f.depthStencilPassOperation = MTLStencilOperationReplace; f.readMask = 0; f.writeMask = 0x40; }   // 参考值 0x40 | Reference value 0x40
        d.frontFaceStencil = f; d.backFaceStencil = f;
    }
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    hm_put_id(&g_dss, &k, s);
    return s;
}
// 参数缓冲：按 B 反射逐项填（uniform 槽 r、分离贴图 / 采样器槽 r、存储缓冲槽 r、驱动段 c0 / c2、c1、support_buffer）
// 没绑的资源读出 0：给全零哑资源（NVN 的 0..255 号描述符为驱动保留，游戏绑 0 号句柄即「此槽为空」）
// Argument buffer: filled item by item per the B reflection (uniform slot r, separate texture / sampler slot r, storage buffer slot r, driver segments c0 / c2, c1, support_buffer)
// Unbound resources read 0: give all-zero dummy resources (NVN descriptors 0..255 are reserved for the driver; the game binding handle 0 means 'this slot is empty')
static uint64_t g_dummy_tex_rid[8][4], g_dummy_img_rid[8][4], g_dummy_tbuf_rid[4];
static id<MTLTexture> dummy_tex(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    MTLTextureType tt = (MTLTextureType)dtt;
    MTLPixelFormat pf = dpf == NVN_DPF_DEPTH ? MTLPixelFormatDepth32Float : dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA8Unorm;
    id<MTLTexture> t = cache[dtt & 7][dpf & 3];
    if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = tt; d.pixelFormat = pf; d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    t = [g_dev newTextureWithDescriptor:d];   // 共享存储新建即全零 | Shared storage is all zeros when newly created
    if (pf != MTLPixelFormatDepth32Float) { uint8_t z[64] = {0}; NSUInteger faces = tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1;
        for (NSUInteger f = 0; f < faces; f++) [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:f withBytes:z bytesPerRow:16 bytesPerImage:16]; }
    cache[dtt & 7][dpf & 3] = t; g_dummy_tex_rid[dtt & 7][dpf & 3] = t.gpuResourceID._impl; res_add(t);
    return t;
}
// 未绑图像槽的写入汇：与读出 0 的哑贴图分开（写入不得污染它们），同类型同像素类共用一张
// Write sink for unbound image slots: kept separate from the dummy textures that read 0 (writes must not pollute them); one is shared per same type and same pixel class
static id<MTLTexture> dummy_img(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    id<MTLTexture> t = cache[dtt & 7][dpf & 3]; if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = (MTLTextureType)dtt; d.pixelFormat = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA16Float;
    d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [g_dev newTextureWithDescriptor:d]; cache[dtt & 7][dpf & 3] = t; g_dummy_img_rid[dtt & 7][dpf & 3] = t.gpuResourceID._impl; res_add(t); return t;
}
static id<MTLBuffer> dummy_buf(void);
static id<MTLTexture> dummy_tbuf(int dpf) {   // 未绑类型化缓冲：全零（像素类与着色器的 texture_buffer<T> 一致） | Unbound typed buffer: all zeros (pixel class matches the shader's texture_buffer<T>)
    static __strong id<MTLTexture> cache[4];
    id<MTLTexture> t = cache[dpf & 3]; if (t) return t;
    MTLPixelFormat pf = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
    id<MTLBuffer> b = dummy_buf();
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:b.length / 16 resourceOptions:b.resourceOptions usage:MTLTextureUsageShaderRead];
    t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:b.length];
    cache[dpf & 3] = t; g_dummy_tbuf_rid[dpf & 3] = t.gpuResourceID._impl; return t;
}
static id<MTLSamplerState> dummy_samp(void) {
    static id<MTLSamplerState> s; if (!s) { MTLSamplerDescriptor *d = [MTLSamplerDescriptor new]; d.supportArgumentBuffers = YES; s = [g_dev newSamplerStateWithDescriptor:d]; }
    return s;
}
static uint64_t dummy_samp_rid(void) { static uint64_t rid; if (!rid) rid = dummy_samp().gpuResourceID._impl; return rid; }
static id<MTLBuffer> dummy_buf(void) { static id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t dummy_buf_gpu(void) { static uint64_t gpu; if (!gpu) gpu = dummy_buf().gpuAddress; return gpu; }
// useResource 在同一编码器内对同一（资源, 用法, 阶段）是幂等的：记下本渲染编码器已声明过的，重复的跳过（计算编码器每次派发新建，不去重）
// useResource is idempotent for the same (resource, usage, stage) within one encoder: remember what this render encoder has already declared and skip repeats (a compute encoder is created anew for each dispatch, no dedup)
static uint8_t g_args_dirty[6] = {1, 1, 1, 1, 1, 1};
static uint64_t g_args_reg_seen, g_args_dirty_seen;
static inline void args_dirty_all(void) { memset(g_args_dirty, 1, sizeof g_args_dirty); }
static inline void args_sync_dirty(void) {
    uint64_t reg = g_reg_gen, dirty = nvn_dirty_now();
    if (reg != g_args_reg_seen || dirty != g_args_dirty_seen) {
        g_args_reg_seen = reg; g_args_dirty_seen = dirty; args_dirty_all();
    }
}
static void bind_args(id<MTLCommandEncoder> en, int nvn_stage, int metal_vertex, id<MTLBuffer> ab, uint64_t gpu, size_t off, NSUInteger ai) {
    if (nvn_stage == 5) enc_c_args((id<MTLComputeCommandEncoder>)en, gpu, ai);
    else enc_r_args((id<MTLRenderCommandEncoder>)en, metal_vertex ? MTLRenderStageVertex : MTLRenderStageFragment, gpu, ai);
}
// B 路线的静态常量缓冲：Ryujinx support_buffer（布局见 Ryujinx.Graphics.Shader/SupportBuffer.cs：alpha_test u32、is_bgra u32[8]、
// viewport_inverse f4、viewport_size f4、frag_scale_count i32、render_scale f32[73]…，各字段按 16 字节对齐）与全 0 驱动常量段（64 KB = 着色器声明的上限）
// Static constant buffers of route B: the Ryujinx support_buffer (layout in Ryujinx.Graphics.Shader/SupportBuffer.cs: alpha_test u32, is_bgra u32[8],
// viewport_inverse f4, viewport_size f4, frag_scale_count i32, render_scale f32[73]..., each field aligned to 16 bytes) and the all-0 driver constant segment (64 KB = the upper bound declared by shaders)
static id<MTLBuffer> support_buf(void) {
    static __strong id<MTLBuffer> b; if (b) return b;
    b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b);   // 新建即全 0 | All 0 when newly created
    const int render_scale_off = 192;   // MSL 布局实测：alpha_test 0、is_bgra[8] 16、viewport_inverse 144、viewport_size 160、frag_scale_count 176、render_scale float4[73] 192（取 .x） | Measured MSL layout: alpha_test 0, is_bgra[8] 16, viewport_inverse 144, viewport_size 160, frag_scale_count 176, render_scale float4[73] 192 (take .x)
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)b.contents + render_scale_off + 16 * i) = 1.0f;
    return b;
}
static uint64_t support_gpu(void) { static uint64_t gpu; if (!gpu) gpu = support_buf().gpuAddress; return gpu; }
static id<MTLBuffer> zero_bank(void) { static __strong id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t zero_bank_gpu(void) { static uint64_t gpu; if (!gpu) gpu = zero_bank().gpuAddress; return gpu; }
// 界面绘制整体下移（iPad 真全屏，界面矩形不贴画面顶边）：给顶点阶段 cb0 开头的投影矩阵加一个裁剪空间的纵向偏移，换一份改过的拷贝绑上去。
// 不用挪视口的办法：视口顶边就是裁剪体的边，宿主上提回真实画面顶部的贴顶控件（界面坐标 y 为负）会被整块裁掉。
// g_ui_moff 由 draw() 逐次绘制定（0 = 不改）；拷贝放在一块环形缓冲里，同一通道同一地址只拷一次。
// Shift all UI draws down as a whole (iPad true fullscreen, the UI rect does not touch the top edge of the picture): add a clip-space vertical offset to the projection matrix at the start of the vertex stage's cb0, and bind a modified copy instead.
// Moving the viewport is not used: the viewport's top edge is the edge of the clip volume, so top-anchored widgets that the host lifts back to the real top of the picture (UI coordinate y negative) would be clipped away entirely.
// g_ui_moff is set per draw by draw() (0 = no change); the copies live in a ring buffer, and the same address in the same pass is copied only once.
static double g_ui_moff;
static int ui_mat_ok(void) { const p_bind_buf *u = &S.ubo[0][0]; void *mb; uint64_t mo; return u->addr && u->size >= 64 && u->size <= 4096 && !pend_hits(u->addr, u->size) && mtl_addr(u->addr, u->size, &mb, &mo); }
static uint64_t ui_mat_patch(const p_bind_buf *u, void *mb, uint64_t mo) {
    enum { RING = 4 << 20 }; static __strong id<MTLBuffer> ring; static uint64_t ring_gpu, la, ls, lp, lg; static size_t at; static double lo;
    if (!ring) { ring = [g_dev newBufferWithLength:RING options:MTLResourceStorageModeShared]; res_add(ring); ring_gpu = ring.gpuAddress; }
    if (la == u->addr && ls == u->size && lp == g_pass.serial && lo == g_ui_moff) return lg;
    const uint8_t *src = [(__bridge id<MTLBuffer>)mb contents]; if (!src || u->size < 64 || u->size > 4096) return 0;
    size_t n = (u->size + 255) & ~(size_t)255; if (at + n > RING) at = 0;
    uint8_t *dst = (uint8_t *)ring.contents + at; memcpy(dst, src + mo, u->size);
    float m[16], o = (float)g_ui_moff; memcpy(m, dst, 64); m[1] += o * m[3]; m[5] += o * m[7]; m[9] += o * m[11]; m[13] += o * m[15]; memcpy(dst, m, 64);
    la = u->addr; ls = u->size; lp = g_pass.serial; lo = g_ui_moff; lg = ring_gpu + at; at += n;
    return lg;
}
#if DM_NVN_M3
// 旧接口的资源声明去重：同一渲染编码器内对同一资源重复 useResource 是幂等的，而每次新编参数块都会把它引用的资源全声明一遍——
// 营地实测每次绘制声明 9.7 次、其中 9.2 次是本编码器里已经声明过的，每次约 227 条指令。这里记下本通道（g_pass.serial，与渲染编码器一一对应：
// 序号只在开渲染编码器前递增）已按哪些用法声明过哪些资源，已覆盖的跳过。表满（一个通道里超过表长个不同资源）时不记、照常声明。
// 表项不持有资源：声明过的资源被编码器持有到提交完成，同一通道内它的地址不会被别的资源复用。只在提交线程上用
// De-duplicating resource declarations on the older API: repeated useResource for the same resource within one render encoder is idempotent, yet every newly encoded argument block declares all the resources it references once more -
// measured in the camp at 9.7 declarations per draw, of which 9.2 were already declared in this encoder, about 227 instructions per draw. Here it is recorded which resources have been declared with which usages in this pass (g_pass.serial, one-to-one with render encoders:
// the serial only increments before a render encoder is opened), and those already covered are skipped. When the table is full (more distinct resources in one pass than the table holds) nothing is recorded and declarations proceed as usual.
// Table entries do not retain resources: a declared resource is held by the encoder until the submission completes, so within one pass its address is not reused by another resource. Used only on the submission thread
enum { M3_USE_N = 1024 };
static struct { void *p; uint64_t pass; uint32_t u; } g_m3_use[M3_USE_N];
static inline int m3_use_seen(void *p, uint32_t u) {
    for (uint32_t h = (uint32_t)(((uintptr_t)p >> 4) * 0x9E3779B1u) >> 22, k = 0; k < M3_USE_N; k++, h = (h + 1) & (M3_USE_N - 1)) {
        if (g_m3_use[h].pass != g_pass.serial) { g_m3_use[h].p = p; g_m3_use[h].pass = g_pass.serial; g_m3_use[h].u = u; return 0; }
        if (g_m3_use[h].p == p) { if ((g_m3_use[h].u & u) == u) return 1; g_m3_use[h].u |= u; return 0; }
    }
    return 0;
}
#endif
static int encode_args(id<MTLCommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) {
    if (!sh->ae) return 1;
    uint64_t slots[NVN_REFL_MAXB] = {0};
    args_sync_dirty();
    if (!passdump() && !g_args_dirty[nvn_stage] && sh->ab_buf && sh->ab_pass == g_pass.serial && nvn_stage != 5) {
        bind_args(en, nvn_stage, metal_vertex, sh->ab_buf, sh->ab_gpu, sh->ab_off, sh->rf.abi);
        g_stat_args_reuse++;
        return 1;
    }
#if DM_NVN_M3
    // 旧接口：着色器经参数块间接访问的每个资源都要在本编码器上声明用途，系统才知道它和别的编码器（上传拷贝、别的通道）之间的先后依赖；
    // 不声明的话只读贴图会和写它的拷贝乱序（迁 Metal 4 前实测过）。同一编码器内重复声明是幂等的
    // Older API: every resource the shaders reach indirectly through an argument buffer must have its usage declared on this encoder, so the system knows its ordering against other encoders (upload copies, other passes);
    // without the declaration a read-only texture can be reordered against the copy that writes it (measured before the move to Metal 4). Declaring it again on the same encoder is idempotent
    #define USE(r, u) do { id<MTLResource> r_ = (id<MTLResource>)(r); if (r_) { if (nvn_stage == 5) [(id<MTLComputeCommandEncoder>)en useResource:r_ usage:(u)]; else if (!m3_use_seen((__bridge void *)r_, (uint32_t)(u))) [(id<MTLRenderCommandEncoder>)en useResource:r_ usage:(u) stages:MTLRenderStageVertex | MTLRenderStageFragment]; } \
        if (__builtin_expect(g_res_check_on, 0)) res_check((r), sh->key, reg); } while (0)
    #define USE_SAMP(r, u) do { (void)(u); if (__builtin_expect(g_res_check_on, 0)) res_check((r), sh->key, reg); } while (0)   // 采样器不是资源，不用声明 | samplers are not resources and need no declaration
#else
    #define USE(r, u) do { (void)(u); if (__builtin_expect(g_res_check_on, 0)) res_check((r), sh->key, reg); } while (0)
    #define USE_SAMP(r, u) USE(r, u)
#endif
    // DM_NVN_PASSDUMP：每通道每阶段第一次新编时记着色器键（K）与各绑定的实际来源（B：UBO 地址 / 长度 / 前 4 个浮点，贴图号与序号或「哑」）
    // DM_NVN_PASSDUMP: on the first new encode per pass per stage, records the shader key (K) and the actual source of each binding (B: UBO address / length / first 4 floats, texture ID and serial, or 「哑」(dummy))
    static uint64_t bl_pass[6]; static int bl_all = -1; if (bl_all < 0) bl_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;   // 每次新编都记（否则只记每通道每阶段第一次） | Record on every new encode (otherwise only the first time per pass per stage)
    int bl = passdump() && (bl_all || bl_pass[nvn_stage] != g_stat_pass);
    if (bl) { bl_pass[nvn_stage] = g_stat_pass; PD("K %llu st%d key=%s nb=%d", g_stat_pass, nvn_stage, sh->key[0] ? sh->key : sh->builtin ? sh->builtin : "?", sh->rf.nb); }
    pthread_mutex_lock(&g_rmu);   // 贴图 / 采样器登记表可能正被游戏线程扩容或改写 | The texture / sampler registration tables may be being grown or rewritten by the game thread
    for (int i = 0; i < sh->rf.nb; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        int reg = b->reg, aid = b->aid;
        if (b->kind == NVN_RB_CBUF) {
            const p_bind_buf *u = reg < 16 ? &S.ubo[nvn_stage][reg] : NULL;
            void *mb; uint64_t mo, gpu;
            if (!u || !u->addr || !mtl_gpu_addr(u->addr, 1, &mb, &mo, &gpu)) { RED_ONCE(16, "阶段 %d cb%d 没有绑定 uniform 缓冲", nvn_stage, reg); continue; }
            use_rec("buf", u->addr, u->size, 0, NULL);
            if (nvn_stage == 0 && reg == 0 && g_ui_moff != 0) { uint64_t pg = ui_mat_patch(u, mb, mo); if (pg) gpu = pg; }
            if (bl) {
                PD("B %llu st%d cb reg%d addr=%#llx size=%llu pool=%#llx+%#llx f=%g,%g,%g,%g", g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr, (unsigned long long)u->size,
                   (unsigned long long)(u->addr - mo), (unsigned long long)mo, ((float *)u->addr)[0], ((float *)u->addr)[1], ((float *)u->addr)[2], ((float *)u->addr)[3]);
                // DM_NVN_CBDUMP=目录：同时把整块 uniform 原样存盘（F帧_p通道_st阶段_cb槽_地址.bin），供真值侧按客体地址逐字节对
                // DM_NVN_CBDUMP=directory: also saves the whole uniform block to disk as-is (F<frame>_p<pass>_st<stage>_cb<slot>_<address>.bin), for the ground-truth side to compare byte by byte by guest address
                static const char *cbd; static int cbd_init; if (!cbd_init) { cbd = getenv("DM_NVN_CBDUMP"); cbd_init = 1; }
                if (cbd) {
                    char p[1024]; snprintf(p, sizeof p, "%s/F%llu_p%llu_st%d_cb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr);
                    FILE *f = fopen(p, "wb"); if (f) { fwrite((const void *)u->addr, 1, u->size, f); fclose(f); } else RED_ONCE(41, "CBDUMP 写不出 %s", p);
                }
            }
            slots[aid] = gpu;
            USE((__bridge id<MTLBuffer>)mb, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SAMP || b->kind == NVN_RB_SAMPT) {   // SAMPT：合并句柄（贴图号 | 采样器号 << 20）的采样器部分 | SAMPT: the sampler part of a combined handle (texture ID | sampler ID << 20)
            int sid = reg < 128 ? (int)((b->kind == NVN_RB_SAMPT ? S.tex[nvn_stage][reg] : S.samp[nvn_stage][reg]) >> 20) : 0;
            __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
            slots[aid] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
            USE_SAMP(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            if (bl && sid > 0) { int ok = sid < g_nsreg && g_sreg_ok[sid]; samp_par sp = ok ? g_sreg[sid] : (samp_par){0};   // 本段已持 g_rmu | g_rmu is already held in this section
                if (ok) PD("B %llu st%d samp reg%d sid=%d mag=%d min=%d（0 最近点 / 1 双线性；2–5 带 mip）", g_stat_pass, nvn_stage, reg, sid, (int)sp.magf, (int)sp.minf); }
        } else if (b->kind == NVN_RB_TEX) {
            int tid = reg < 128 ? (int)(S.tex[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (t && tid < g_ntreg) use_rec("tex", (uint64_t)g_treg[tid].t.mem, g_treg[tid].t.L.size, tid, &g_treg[tid].t);
            if (t && passdump() && tid < g_ntreg) PD("R %llu st%d ser=%llu", g_stat_pass, nvn_stage, g_treg[tid].t.serial);
            if (bl) PD("B %llu st%d tex reg%d tid=%d %s ser=%llu fmt=%lu %lux%lux%lu view_lv=%lu+%lu view_sl=%lu array=%lu target=%lu", g_stat_pass, nvn_stage, reg, tid, t ? "实" : "哑", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull,
                       (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth,
                       (unsigned long)(t.parentTexture ? t.parentRelativeLevel : 0), (unsigned long)t.mipmapLevelCount,
                       (unsigned long)(t.parentTexture ? t.parentRelativeSlice : 0), (unsigned long)t.arrayLength, (unsigned long)t.textureType);
            if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt & 7][b->dpf & 3]; }
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_IMG) {   // 可写贴图：NVN 图像槽 r（BindImages）↔ D3D u_r | Writable texture: NVN image slot r (BindImages) ↔ D3D u_r
            int tid = reg < 32 ? (int)(S.img[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (passdump()) PD("W %llu st%d reg%d tid=%d %s ser=%llu", g_stat_pass, nvn_stage, reg, tid, t ? "实" : "哑", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull);
            if (t && !(t.usage & MTLTextureUsageShaderWrite)) { RED_ONCE(32, "图像槽绑的贴图没有可写用途（贴图号 %d）", tid); t = nil; }
            if (!t) { t = dummy_img(b->dtt, b->dpf); trid = g_dummy_img_rid[b->dtt & 7][b->dpf & 3]; }   // 未绑：D3D 语义写入丢弃——写进专用哑贴图，不污染读出 0 的那套 | Unbound: D3D semantics discard the write - write into a dedicated dummy texture so the set that reads 0 is not polluted
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead | MTLResourceUsageWrite);
        } else if (b->kind == NVN_RB_TBUF) {   // 类型化缓冲：贴图槽 r 里的缓冲贴图（目标 BUFFER） | Typed buffer: the buffer texture (target BUFFER) in texture slot r
            int tid = reg < 128 ? (int)(S.tex[nvn_stage][reg] & 0xfffff) : 0;
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (t && t.textureType != MTLTextureTypeTextureBuffer) { RED_ONCE(40, "类型化缓冲槽绑的不是缓冲贴图（贴图号 %d 类型 %lu）", tid, (unsigned long)t.textureType); t = nil; }
            if (bl) PD("B %llu st%d tbuf reg%d tid=%d %s ser=%llu", g_stat_pass, nvn_stage, reg, tid, t ? "实" : "哑", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull);
            if (!t) { t = dummy_tbuf(b->dpf); trid = g_dummy_tbuf_rid[b->dpf & 3]; }
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP) {   // B：存储缓冲 / 计算阶段 uniform 是设备指针，按 NVN 槽取地址 | B: storage buffers / compute-stage uniforms are device pointers; the address is taken by NVN slot
            const p_bind_buf *u = reg < 16 ? (b->kind == NVN_RB_UBOP ? &S.ubo[nvn_stage][reg] : &S.ssbo[nvn_stage][reg]) : NULL;
            void *mb; uint64_t mo, gpu;
            if (u && u->addr && mtl_gpu_addr(u->addr, u->size ? u->size : 1, &mb, &mo, &gpu)) { use_rec("buf", u->addr, u->size, 0, NULL); slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, b->rw ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead); }
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead); }
            if (bl) PD("B %llu st%d sbufp reg%d %s addr=%#llx size=%llu %s", g_stat_pass, nvn_stage, reg, u && u->addr ? "实" : "哑", u ? (unsigned long long)u->addr : 0ull, u ? (unsigned long long)u->size : 0ull, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_C1) {      // B：着色器自带常数段，取着色器时建好 | B: the shader's own constant segment, built when the shader is fetched
            id<MTLBuffer> cb = sh->c1buf ? sh->c1buf : zero_bank();   // c1 长 0（全库 600 份）而着色器仍声明：读 0 | c1 has length 0 (600 of them in the whole library) yet the shader still declares it: reads 0
            slots[aid] = (sh->c1buf ? sh->c1_gpu : zero_bank_gpu()) + (sh->c1buf ? sh->c1off : 0);
            USE(cb, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SUPPORT) {   // support_buffer：缩放 1.0、BGRA 互换 0、视口变换未关闭（视口字段不读） | support_buffer: scale 1.0, BGRA swap 0, viewport transform not disabled (viewport fields are not read)
            slots[aid] = support_gpu();
            USE(support_buf(), MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_DRV0 || b->kind == NVN_RB_DRV2) {
            // NVN 驱动常量段：出包时静态统计证明不读的供 0；读的按当前绑定现填——c0 = 存储缓冲表（基址 + 0x10 r：地址 64 位、大小 32 位），
            // c2 = 贴图句柄表（0x168 + 8 r）与采样器句柄表（0x568 + 8 r），值即游戏绑定时给的 NVN 句柄；读到表外的位置出包时已登记、加载时报红
            // NVN driver constant segments: those proven unread by static analysis at packaging time are supplied as 0; those read are filled on the spot from the current bindings - c0 = storage buffer table (base + 0x10 r: address 64 bits, size 32 bits),
            // c2 = texture handle table (0x168 + 8 r) and sampler handle table (0x568 + 8 r), the values being the NVN handles the game gave at bind time; positions read outside the tables are registered at packaging time and reported a failure at load time
            int bank = b->kind == NVN_RB_DRV2 ? 2 : 0;
            if (!sh->rf.drv_read[bank]) { slots[aid] = zero_bank_gpu(); USE(zero_bank(), MTLResourceUsageRead); }
            else {
                id<MTLBuffer> db; size_t doff = arena_alloc(0xA00, &db); uint8_t *d = (uint8_t *)db.contents + doff; memset(d, 0, 0xA00);
                if (bank == 0) {
                    int base = nvn_stage == 0 ? 0x110 : nvn_stage == 5 ? 0x310 : nvn_stage == 1 ? 0x510 : -1;
                    if (base < 0) RED_ONCE(45, "阶段 %d 的着色器读驱动段 c0，存储缓冲表基址未实测", nvn_stage);
                    else for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[nvn_stage][r].addr; uint32_t n = (uint32_t)S.ssbo[nvn_stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
                } else {
                    for (int r = 0; r < 128; r++) { uint32_t th = (uint32_t)S.tex[nvn_stage][r], sh_ = (uint32_t)S.samp[nvn_stage][r]; memcpy(d + 0x168 + 8 * r, &th, 4); memcpy(d + 0x568 + 8 * r, &sh_, 4); }
                }
                slots[aid] = g_cur_arena->gpu + doff;
                USE(db, MTLResourceUsageRead);
            }
        } else RED_ONCE(46, "绑定种类 %d 未实现", b->kind);
    }
    #undef USE
    #undef USE_SAMP
    pthread_mutex_unlock(&g_rmu);
    NSUInteger arg_len = sh->arg_len ? sh->arg_len : sh->ae.encodedLength;
    id<MTLBuffer> ab; size_t off = arena_alloc(arg_len, &ab);
    memcpy((uint8_t *)ab.contents + off, slots, arg_len);
    bind_args(en, nvn_stage, metal_vertex, ab, g_cur_arena->gpu + off, off, sh->rf.abi);
    sh->ab_buf = ab; sh->ab_gpu = g_cur_arena->gpu + off; sh->ab_off = off; sh->ab_pass = g_pass.serial; g_args_dirty[nvn_stage] = 0;
    g_stat_args_enc++;
    return 1;
}
static id<MTLBuffer> g_quad_ib; static uint64_t g_quad_gpu;
static RENC rp_begin(void);
// ---------- 通道前瞻：本通道是否用到深度 / 模板 ----------
// 开通道时从当前绘制的下一条起往后扫，模拟深度模板状态的变化，直到本通道必然结束处（重设目标、拷贝、计算派发——
// 与回放里结束通道的操作一一对应）。期间没有任何绘制开深度测试 / 深度写 / 模板测试，则本通道可不挂深度附件：
// 深度内容一字节不动，TBDR 上省掉整张深度的读入和写出（真机实测：单绘制全屏通道占 GPU 一半，多挂着 D32S8 读入写出）。
// 扫描按真实回放顺序跨子列表返回、跨本提交余下句柄；提交末尾 = 通道必然结束。通道中途清除、嵌套过深等无法判定的情况一律算「用到」。
// ---------- Pass lookahead: does this pass use depth / stencil ----------
// When opening a pass, scan forward from the op after the current draw, simulating the changes of depth-stencil state, up to where this pass must end (target reset, copy, compute dispatch -
// matching one-to-one the ops that end a pass in the replay). If no draw in between enables depth test / depth write / stencil test, this pass may leave the depth attachment unattached:
// not one byte of the depth content is touched, and on TBDR the load and store of the whole depth texture are saved (measured on a real device: a single-draw fullscreen pass takes half the GPU, with an extra D32S8 load and store attached).
// The scan follows real replay order across sub-list returns and across the remaining handles of this submission; end of submission = the pass must end. Undecidable cases such as a mid-pass clear or too-deep nesting always count as 'used'.
static const uint8_t *g_exec_next;   // 回放中当前绘制的下一条操作 | The op following the current draw in the replay
static const uint8_t *g_ret[16]; static int g_ret_n;   // 回放的调用返回栈（OP_CALL 处压入调用方的下一条） | Call return stack of the replay (at OP_CALL the caller's next op is pushed)
static const uint64_t *g_rest; static int g_nrest;      // 本次提交里当前句柄之后还要回放的句柄 | Handles in this submission still to be replayed after the current handle
void mtl_exec_rest(const uint64_t *h, int n) { g_rest = h; g_nrest = n; }
static uint64_t g_stat_ds_omit;      // 因前瞻证明不用而少挂深度模板的通道数（含只挂模板 / 只挂深度） | Number of passes that attach less depth-stencil because the lookahead proved it unused (including stencil-only / depth-only)
static uint64_t g_stat_s_only, g_stat_d_only;   // 其中只挂模板 / 只挂深度的通道数 | Of those, the number of passes that attach only stencil / only depth
// 纯清除通道前提（hoist_try）：已提前执行、回放走到原位置时要跳过的操作（各组的 SetRenderTargets 与其中的清除）。
// 执行循环、写回前瞻（sdx 索引）、深度模板前瞻（ds_scan）都按它跳过——否则前瞻会把原位置的清除当成「再次被清」，误判提前清好的内容多余
// Clear-only pass hoist (hoist_try): ops that were already executed early and must be skipped when replay reaches their original position (each group's SetRenderTargets and the clears in it).
// The exec loop, the store lookahead (sdx index) and the depth-stencil lookahead (ds_scan) all skip by it - otherwise the lookahead would take the clear at the original position as 'cleared again' and wrongly judge the early-cleared content redundant
#define HOIST_MAX 40   // 最多 4 组 ×（1 条设目标 + 8 条清除） | At most 4 groups × (1 set-target op + 8 clears)
static const uint8_t *g_hoist_skip[HOIST_MAX]; static int g_hoist_n; static uint64_t g_hoist_groups;
static int hoist_skipped(const uint8_t *p) { for (int i = 0; i < g_hoist_n; i++) if (g_hoist_skip[i] == p) return 1; return 0; }
typedef struct { const uint8_t *p, *ret[8]; int ret_n, rest_i, bad; } fz_cursor;
static int fz_next(fz_cursor *c, const uint8_t **out, op_hdr *oh, const void **oa) {
    for (int guard = 0; guard < 1000000; guard++) {
        if (!c->p) return 0;
        op_hdr h; memcpy(&h, c->p, sizeof h); const void *a = c->p + 8;
        switch (h.op) {
            case OP_END:
                if (c->ret_n) c->p = c->ret[--c->ret_n];
                else if (c->rest_i < g_nrest) c->p = (const uint8_t *)g_rest[c->rest_i++];
                else { c->p = NULL; return 0; }
                continue;
            case OP_JUMP: memcpy(&c->p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (c->ret_n >= 8) { c->bad = 1; return 0; }
                c->ret[c->ret_n++] = c->p + h.size; c->p = (const uint8_t *)t; continue; }
            default: break;
        }
        const uint8_t *p = c->p; c->p += h.size;
        if (g_hoist_n && hoist_skipped(p)) continue;
        if (out) *out = p; if (oh) *oh = h; if (oa) *oa = a; return 1;
    }
    c->bad = 1; return 0;
}
// 可读 tile 通道合并（合并 B 第 2 步）的状态：前一个通道（阶段 1）与后一个通道（阶段 2）共用一个渲染通道，附件取并集
// State of the readable-tile pass merge (merge B step 2): the earlier pass (phase 1) and the later pass (phase 2) share one render pass, and the attachments are the union
typedef struct {
    int active, phase, n1, nskip;
    int nreuse;
    const uint8_t *rt2_op, *reuse[8], *skip[12];
    p_set_rt rt1, rt2;                 // 两个通道各自的逻辑目标（R.rt 在合并期间是并集） | Logical targets of each of the two passes (R.rt is the union during the merge)
    int8_t p2map[8];                   // 后一个通道的逻辑颜色号 → 物理附件 | Logical color number of the later pass → physical attachment
    uint8_t lmap1[8], lmap2[8];        // 物理附件 → 本阶段绘制的逻辑颜色号（0xff = 不写） | Physical attachment → logical color number of this phase's draws (0xff = not written)
    uint8_t clr2[8], cd2f, cs2f; MTLClearColor cc2[8]; double cd2; uint32_t cs2;   // 后一个通道开头的清除（局内双渲染重画时照原样用） | Clears at the start of the later pass (used as-is when the in-run dual render redraws)
    uint8_t wrote[8];                   // 后一个通道写过的物理附件 | Physical attachments written by the later pass
    uint8_t cov2[8];                     // 后一个通道首绘覆盖的物理附件 | Physical attachments covered by the first draw of the later pass
} fuse_t;
static fuse_t g_fz; static uint64_t g_fz_n, g_fz_draws, g_fz_fallback;
typedef struct { int idx; p_draw dw; p_draw_idx di; p_scissor sc; int has_sc; void *S_; uint8_t pm[8]; } defer_snap;   // pm：组 P 目标下标 → 本次绘制的逻辑颜色号（自身绘制为恒等；被吸收的上游组为复合映射） | pm: group P target index → logical color number of this draw (identity for its own draws; a composed mapping for absorbed upstream groups)
typedef struct {
    int active, at_q, in_p, n, reason, reason_op, logged, reset_between; uint64_t born;   // born：创建序号（回放按它排，不按槽位） | born: creation serial (replay is ordered by it, not by slot)
    p_set_rt p_rt, q_rt;
    const uint8_t *p_end_op, *q_op;
    uint8_t clear_c[8], lmap[8];
    MTLClearColor cc[8];
    uint8_t clear_d, clear_s;
    double cd; uint32_t cs;
    uint64_t p_reads[512]; int nreads, p_dw, p_st;
    defer_snap snap[64];
} defer_t;
static defer_t g_defer_slots[4];
static defer_t *g_defer_cur = &g_defer_slots[0];
#define g_defer (*g_defer_cur)
static int sub_eq(const p_set_rt *a, int ka, const p_set_rt *b, int kb);
static uint64_t g_defer_n, g_defer_draws, g_defer_fallbacks;
static const uint8_t *g_defer_pend;   // 在 P 区间那一组的 P 区间结束操作（没有则 NULL）：执行循环每条操作只比这一个指针 | The P-range end op of the group that is in its P range (NULL if none): the exec loop compares each op against just this one pointer
static int g_defer_nact, g_defer_natq;   // 活动组数、已到 Q 等回放的组数（热路径只看计数，不逐槽扫） | Number of active groups, number of groups that reached Q and await replay (the hot path only looks at the counts, no per-slot scan)
static int g_defer_ab = -1, g_defer_ab_on;
static void draw(const p_draw *dw, const p_draw_idx *di);
static int defer_has_in_p(void) { return g_defer_pend != NULL; }
static int defer_has_active(void) { return g_defer_nact > 0; }
static int defer_slot_free(void) { for (int i = 0; i < 4; i++) if (!g_defer_slots[i].active) return i; return -1; }
static int defer_same_q_depth(const defer_t *a, const p_set_rt *q) {
    if (!a->q_rt.depth || !q->depth) return 1;
    return sub_eq(&a->q_rt, 8, q, 8);
}
static int defer_next(int at_q_only, uint64_t after) {   // 下一个（创建序号最小且 > after）活动组的槽号；没有返回 −1 | Slot number of the next active group (smallest creation serial that is > after); returns −1 if none
    int best = -1; for (int i = 0; i < 4; i++) { const defer_t *d = &g_defer_slots[i]; if (!d->active || (at_q_only && !d->at_q) || d->born <= after) continue; if (best < 0 || d->born < g_defer_slots[best].born) best = i; }
    return best;
}
static void defer_release_one(defer_t *d) { if (d->active) { g_defer_nact--; if (d->at_q) g_defer_natq--; if (d->in_p) g_defer_pend = NULL; } for (int i = 0; i < d->n; i++) free(d->snap[i].S_); memset(d, 0, sizeof *d); }
static void defer_clear_all(void) { for (int i = 0; i < 4; i++) defer_release_one(&g_defer_slots[i]); g_defer_nact = g_defer_natq = 0; g_defer_pend = NULL; }
static int hoist_take(const uint8_t *p) { for (int i = 0; i < g_hoist_n; i++) if (g_hoist_skip[i] == p) { g_hoist_skip[i] = g_hoist_skip[--g_hoist_n]; return 1; } return 0; }
enum { DS_FREE = 0, DS_USED = 1, DS_RET = 2 };
static int g_ds_why; static int g_ds_scanned; static int g_ds_need;   // g_ds_need：扫描中累计的需求，1 深度 2 模板   // 诊断：判「用到」时触发的操作码（绘制则记 1000 + dt*100 + dw*10 + st）、扫过的操作数 | g_ds_need: need accumulated during the scan, 1 depth 2 stencil   // Diagnostics: the opcode that triggered the 'used' verdict (for a draw, records 1000 + dt*100 + dw*10 + st), and the number of ops scanned
static int ds_scan(const uint8_t *p, ds_st *ds, int depth) {
    for (int guard = 0; guard < 1000000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const void *a = p + 8; g_ds_scanned++;
        if (g_hoist_n && hoist_skipped(p)) { p += h.size; continue; }   // 已提前执行的清除通道 | Clear pass already executed early
        switch (h.op) {
            case OP_END: return DS_RET;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); if (depth >= 8) return DS_USED;
                int r = ds_scan((const uint8_t *)t, ds, depth + 1); if (r != DS_RET) return r; break; }
            case OP_SET_RT: case OP_COPY_B2B: case OP_UPDATE_UB: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX: case OP_DISPATCH: return DS_FREE;
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: g_ds_why = h.op; return DS_USED;
            case OP_BIND_STATE: { const p_bind_state *b = a; if (b->kind == 5) memcpy(ds, b->bytes, sizeof *ds); break; }
            case OP_DRAW: case OP_DRAW_IDX:   // 深度测试 / 写入 → 要深度；模板测试 → 要模板；两项都要即可停 | Depth test / write → depth needed; stencil test → stencil needed; can stop once both are needed
                g_ds_need |= (ds->dt || ds->dw ? 1 : 0) | (ds->st ? 2 : 0);
                if (g_ds_need == 3) { g_ds_why = 1000 + ds->dt * 100 + ds->dw * 10 + ds->st; return DS_USED; } break;
            default: break;
        }
        p += h.size;
    }
    return DS_USED;
}
// 写回前瞻的判定（逐条命令，按真实回放顺序）：
//   多余：再次被设为附件后、任何绘制之前被整块清除（颜色：该序号四通道全写；深度：深度清除；模板：模板 8 位全清。深度与模板分开判、分开设写回）
//   需要：有绘制 / 派发时它绑在任一阶段的贴图或图像槽（含扫描开始时已绑着的）；作拷贝源 / 拷贝目标 / 清贴图对象；再次作附件后先画后清（读入）
// 同一贴图的不同级 / 层分开判（降采样链写第 k 级、读第 k−1 级）；采样绑定按整张贴图算读取（保守）
// Store lookahead decision (command by command, in real replay order):
//   Redundant: after being set as an attachment again and before any draw, it is cleared in full (color: all four channels of that index written; depth: depth clear; stencil: all 8 stencil bits cleared. Depth and stencil are decided separately and their write-back is set separately)
//   Needed: at a draw / dispatch it is bound to a texture or image slot of any stage (including ones already bound when the scan starts); used as copy source / copy destination / texture-clear target; drawn before cleared after becoming an attachment again (load)
// Different levels / layers of the same texture are decided separately (a downsample chain writes level k and reads level k−1); a sampler binding counts as a read of the whole texture (conservative)
static const uint8_t *g_exec_cur;   // 回放中当前这条操作（含） | The op currently being replayed (inclusive)
static void sd_need(sd_att *t, int why) { if (!t->state) { t->state = 1; t->why = (uint8_t)why; } }   // why：1 绘制作贴图读 2 作附件先画后清 3 拷贝 4 清贴图 5 扫到末尾未定 6 无起点 / 索引不可用 | why: 1 a draw reads it as a texture 2 as an attachment, drawn before cleared 3 copy 4 texture clear 5 scanned to the end undecided 6 no start point / index unavailable
static int sdx_decide(uint8_t st[9]);   // 写回前瞻判定（索引见 mtl_sub_index） | Store lookahead decision (for the index see mtl_sub_index)
static void sd_finish(void) {
    if (!g_nsd) return;
    if (!tbdr_on()) {   // 优化全关：一律写回 | All optimizations off: always write back
        for (int i = 0; i < g_nsd; i++) { sd_att *t = &g_sd[i];
            if (t->depth == 1) [g_enc setDepthStoreAction:MTLStoreActionStore]; else if (t->depth == 2) [g_enc setStencilStoreAction:MTLStoreActionStore]; else [g_enc setColorStoreAction:MTLStoreActionStore atIndex:(NSUInteger)t->idx]; }
        g_nsd = 0; return; }
    uint8_t nst[9];
    if (sdx_decide(nst)) { for (int i = 0; i < g_nsd; i++) if (nst[i] == 2) g_sd[i].state = 2; else sd_need(&g_sd[i], nst[i] >> 2); }
    else for (int i = 0; i < g_nsd; i++) sd_need(&g_sd[i], 6);   // 列表外结束的通道 / 索引不可用：一律写回 | Pass ended outside the list / index unavailable: always write back
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i]; int udc_dead = udc_on() && t->load && !t->wrote;
        // 与窗口贴图写在同一个通道里的颜色附件不进账本：它是出图链上的一环，不是「每帧先重置再用」的瞬态附件。
        // 局内「放大」通道一次写两张——场景贴图 + 窗口贴图，界面合成并进了同一通道，所以局内没人回读场景贴图；游戏一停画 3D（读取画面）
        // 就改由单独的合成通道回头采它。「要进读取画面」在上一帧提交之后才知道，预判拦不住：iPad 真全屏每个安装第一次换幕必撞一次
        // （帧 N 那张 2732×2048 附件没写回，帧 N+1 起被连采 4 帧；键永久禁用）。iPhone 比例下同尺寸的键在试用期就见到读者、被静默禁用。
        // A color attachment written in the same pass as the window texture does not enter the ledger: it is a link in the present chain, not a transient attachment that is 'reset every frame before use'.
        // The in-run 'upscale' pass writes two textures at once - scene texture + window texture - and the UI composite was merged into the same pass, so in-run nobody reads back the scene texture; as soon as the game stops drawing 3D (loading screen)
        // a separate composite pass samples it afterwards instead. 'About to enter the loading screen' is only known after the previous frame was submitted, so prediction cannot stop it: on iPad true fullscreen every install hits it once on the first act change
        // (the 2732×2048 attachment of frame N was not written back and is sampled for 4 consecutive frames starting at frame N+1; the key is permanently disabled). At the iPhone aspect ratio the same-size key already sees a reader during the trial period and is silently disabled.
        int xv_win = 0; for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k] && xv_present(R.rt.color[k]->serial)) xv_win = 1;
        uint64_t xtag = 0; int xvcand = xv_on() && !xv_win && t->depth == 0 && t->state != 2 && t->why == 5 && xv_first_good(t->ser, t->lv, t->sl) && !xv_present(t->ser)
                                        && !xv_disabled(xtag = xv_tag_pass(R.rt.color[t->idx], t->lv, t->idx, &R.rt));
        int xvdead = xvcand && xv_trial_get(xtag, 0) >= XV_TRIAL;   // 过了试用期才真的不写回 | Only after the trial period is the write-back really skipped
        if (xvcand && !xvdead) xv_set(t->ser, t->lv, t->sl, XV_PROBE, xtag);   // 试用期：照常写回，只跟踪下一次触碰 | Trial period: write back as usual, only track the next touch
        int dead = t->state == 2 || udc_dead || xvdead; MTLStoreAction sa = dead ? MTLStoreActionDontCare : MTLStoreActionStore;
        if (xvdead) { xv_set(t->ser, t->lv, t->sl, XV_INVALID, xtag); g_xv_dc++; }
        if (t->depth == 1) [g_enc setDepthStoreAction:sa]; else if (t->depth == 2) [g_enc setStencilStoreAction:sa]; else [g_enc setColorStoreAction:sa atIndex:(NSUInteger)t->idx];
        g_sd_n_all++; if (dead) { g_sd_n_dead++; g_sd_dead_bytes += t->bytes; g_bw_store -= t->bytes; if (udc_dead) g_udc_saved_bytes += t->bytes; }
        if (!t->state) t->why = 5;
        PD("Z %llu %s ser=%llu lv%u sl%u %s why=%d by=%s slot=%d", g_stat_pass, t->depth == 1 ? "d" : t->depth == 2 ? "s" : "c", (unsigned long long)t->ser, t->lv, t->sl, xvdead ? "dontcare-xv" : dead ? "dontcare" : "store", dead ? 0 : t->why, t->why == 1 && t->wkey ? t->wkey : "-", t->wslot);
        if (udc_on()) PD("U %llu %s ser=%llu lv%u sl%u runtime=%s reason=%s state=%d load=%d", g_stat_pass, t->depth == 1 ? "d" : t->depth == 2 ? "s" : "c", (unsigned long long)t->ser, t->lv, t->sl, t->wrote ? "写过" : "没写", t->wwhy == 1 ? "图像写" : t->wwhy == 2 ? "颜色写掩码" : t->wwhy == 3 ? "片元输出" : t->wwhy == 4 ? "深度写" : t->wwhy == 5 ? "模板写" : "无", t->state, t->load);
    }
    g_nsd = 0;
}
// 按真实回放顺序接着扫：当前列表 → 各层调用方（返回栈自顶向下）→ 本提交余下句柄；提交结束时通道必然结束（commit 前 rp_flush）
// 覆盖证明（全部满足才成立；任何一项认不出按不成立）：
//   着色器：顶点位置 xyz 原样取自一个顶点属性、w 为 1 或同属性 .w（dmsh_build raster_facts）；片元不丢弃、不写 sample_mask、写该颜色号
//   状态：该附件不混合、写掩码满、无 alpha-to-coverage、填充模式、无深度测试（或 ALWAYS）、无模板测试、单采样、非 3D
//   顶点：非索引或 16 / 32 位索引绘制，索引与位置都从 CPU 可见的池内存读出；剪裁 / 视口 / 几何 / 朝向 / 深度裁剪的判定在 nvn_cover.h（纯函数，有单测）
// 读出本次绘制全部顶点的属性 a（非索引或 16 / 32 位索引；顶点号 = first + k 或 索引值 + base_vertex），缺分量按取数规则补 0 / 1。
// 成功返回 0，失败返回原因码（6 顶点数 / 索引类型，7 属性号，8 格式 / 流，9 越界，10 地址）
// Continue scanning in real replay order: current list -> callers at each level (return stack from the top down) -> remaining handles of this submit; the render pass necessarily ends when the submit ends (rp_flush before commit)
// Coverage proof (holds only if all are satisfied; anything unrecognized is treated as not holding):
//   Shader: the vertex position xyz is taken as-is from one vertex attribute, w is 1 or the same attribute's .w (dmsh_build raster_facts); the fragment does not discard, does not write sample_mask, and writes this color index
//   State: this attachment has no blending, a full write mask, no alpha-to-coverage, fill mode, no depth test (or ALWAYS), no stencil test, single sample, not 3D
//   Vertices: non-indexed or 16 / 32-bit indexed draws, with indices and positions both read from CPU-visible pool memory; the scissor / viewport / geometry / winding / depth clipping decisions are in nvn_cover.h (pure functions, with unit tests)
// Read out attribute a of all vertices of this draw (non-indexed or 16 / 32-bit indexed; vertex number = first + k or index value + base_vertex), with missing components filled with 0 / 1 per the fetch rules.
// Returns 0 on success, a reason code on failure (6 vertex count / index type, 7 attribute index, 8 format / stream, 9 out of bounds, 10 address)
static int attr_read(int a, const p_draw *dw, const p_draw_idx *di, float P[64][4]) {
    int n = dw ? dw->count : di->count;
    if (n < 1 || n > 64 || (dw && dw->first < 0) || (di && (di->itype < 1 || di->itype > 2))) return 6;
    if (a < 0 || a >= S.nattr || S.attr[a].stream >= 16) return 7;
    va_st at = S.attr[a]; vs_st sm = S.stream[at.stream]; NVNbufferRange vb = S.vbuf[at.stream];
    MTLVertexFormat vf = vfmt(at.fmt); int nc = vf == MTLVertexFormatFloat2 ? 2 : vf == MTLVertexFormatFloat3 ? 3 : vf == MTLVertexFormatFloat4 ? 4 : 0;
    if (!nc || !sm.stride || sm.divisor || !vb.address) return 8;
    const uint8_t *ib = NULL; int isz = di ? (di->itype == 1 ? 2 : 4) : 0;
    if (di) { void *m; uint64_t o; if (!mtl_addr(di->ibuf, (uint64_t)n * isz, &m, &o) || !(ib = [(__bridge id<MTLBuffer>)m contents])) return 10; ib += o; }
    for (int k = 0; k < n; k++) {
        int64_t vid = dw ? (int64_t)dw->first + k : (isz == 2 ? (int64_t)((const uint16_t *)ib)[k] : (int64_t)((const uint32_t *)ib)[k]) + di->base_vertex;
        uint64_t off = (uint64_t)vid * sm.stride + at.offset;
        if (vid < 0 || off + 4 * nc > vb.size) return 9;
        void *mb; uint64_t mo; const uint8_t *base;
        if (!mtl_addr(vb.address + off, 4 * nc, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 10;
        float v[4] = {0, 0, 0, 1}; memcpy(v, base + mo, 4 * nc); memcpy(P[k], v, sizeof v);
    }
    return 0;
}
static int first_draw_covers(int i, id<MTLTexture> t, NSUInteger level) {
    const p_draw *dw = g_fd_dw; const p_draw_idx *di = g_fd_di;
    int n = dw ? dw->count : di->count, prim = dw ? dw->prim : di->prim;
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr) return g_fd_why = 1, 0;
    NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs || !fs || vs->rf.pos_attr < 0 || fs->rf.may_discard || fs->rf.writes_smask || !(fs->rf.color_mask >> i & 1)) return g_fd_why = 2, 0;
    if (!t || t.sampleCount != 1 || t.textureType == MTLTextureType3D) return g_fd_why = 3, 0;
    if ((S.color.blend_en >> i & 1) || ((S.chmask.mask >> (4 * i)) & 0xf) != 0xf || S.ms.a2c || S.poly.mode != NVN_POLYGON_MODE_FILL) return g_fd_why = 4, 0;
    if (R.rt.depth && ((S.ds.dt && S.ds.df != NVN_DEPTH_FUNC_ALWAYS) || S.ds.st)) return g_fd_why = 5, 0;
    if (n < 3 || n > 64) return g_fd_why = 6, 0;
    float P[64][4]; int why = attr_read(vs->rf.pos_attr, dw, di, P);
    if (why) return g_fd_why = why, 0;
    if (vs->rf.pos_w1) for (int k = 0; k < n; k++) P[k][3] = 1.0f;   // 位置 w 为常数 1（不取属性 .w） | Position w is the constant 1 (the attribute's .w is not taken)
    int sc[4] = {R.sc.x, R.sc.y, R.sc.w, R.sc.h}; float vp[4] = {S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3]};
    g_fd_why = 11;
    return nvn_cover((const float (*)[4])P, n, prim, (int)S.poly.cull, S.poly.front == NVN_FRONT_FACE_CCW,
                     S.has_vp, vp, R.has_sc, sc, (long)MAX(t.width >> level, 1), (long)MAX(t.height >> level, 1));
}

static int state_op(uint32_t op, const void *a) {
    switch (op) {
        case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
        case OP_BIND_PROGRAM: { const prog_t *np = ((const p_bind_program *)a)->prog; if (np != S.prog) { S.prog = np; g_pso_dirty = 1; args_dirty_all(); } break; }
        case OP_BIND_STATE: {
            const p_bind_state *b = a; void *dst = NULL; size_t n = 0;
            blend_st bs;
            switch (b->kind) {
                case 0: memcpy(&bs, b->bytes, sizeof bs); dst = &S.blend[bs.target]; n = sizeof bs; break;
                case 1: dst = &S.color; n = sizeof S.color; break;
                case 2: dst = &S.chmask; n = sizeof S.chmask; break;
                case 3: dst = &S.ms; n = sizeof S.ms; break;
                case 4: dst = &S.poly; n = sizeof S.poly; break;
                case 5: dst = &S.ds; n = sizeof S.ds; break;
            }
            if (dst && memcmp(dst, b->bytes, n)) { memcpy(dst, b->bytes, n); if (b->kind == 5) g_dss_dirty = 1; else g_pso_dirty = 1; }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; if (S.nattr != b->n || memcmp(S.attr, b->st, 4 * b->n)) { memcpy(S.attr, b->st, 4 * b->n); S.nattr = b->n; g_pso_dirty = 1; } break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; if (S.nstream != b->n || memcmp(S.stream, b->st, 8 * b->n)) { memcpy(S.stream, b->st, 8 * b->n); S.nstream = b->n; g_pso_dirty = 1; } break; }
        case OP_BIND_VBUF: { const p_bind_vbuf *b = a; memcpy(&S.vbuf[b->first], b->r, sizeof *b->r * b->n); break; }
        case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16 && memcmp(&S.ubo[b->stage][b->index], b, sizeof *b)) { S.ubo[b->stage][b->index] = *b; g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16 && memcmp(&S.ssbo[b->stage][b->index], b, sizeof *b)) { S.ssbo[b->stage][b->index] = *b; g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage < 6 && b->first + b->n <= 128 && memcmp(&S.tex[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.tex[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage < 6 && b->first + b->n <= 128 && memcmp(&S.samp[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.samp[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; } break; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) { if (memcmp(&S.img[b->stage][b->first], b->h, 8 * b->n)) { memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); g_args_dirty[b->stage] = 1; } } else RED_ONCE(31, "图像槽越界 阶段 %d 起 %d 数 %d", b->stage, b->first, b->n); break; }
        case OP_STENCIL: {
            const p_stencil *b = a;
            if (b->which != 2) g_dss_dirty = 1;
            for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; }
            break;
        }
        case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n; i++) memcpy(S.vp[b->first + i], b->v[i], 16); S.has_vp = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; S.dr[0] = b->n; S.dr[1] = b->f; break; }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
        case OP_COPY_IMAGE_STRIDE: S.copy_image_stride = ((const p_copy_stride *)a)->stride; break;
        default: return 0;
    }
    return 1;
}

static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl);

// 并行预编的预测（见 pso_get 前的说明）：从当前操作之后扫到本次提交结束，只重放状态；遇到绘制就按「那时的状态 + 那个通道的附件格式」算管线键，没建过的丢后台。
// 通道实际挂不挂深度 / 模板要到回放时才定（纯深度 / 纯模板 / 都不挂的省略），所以每种可能的挂法各算一条键——猜多了只是后台白编，不影响结果。
// Prediction for parallel precompilation (see the note before pso_get): scan from after the current operation to the end of this submit, replaying only state; at a draw, compute the pipeline key from "the state at that time + that render pass's attachment formats" and dispatch those not yet built to the background.
// Whether a render pass actually attaches depth / stencil is only decided at replay (depth-only / stencil-only / neither attached are omitted), so one key is computed for each possible attach mode -- guessing too many only means wasted background compiles and does not affect the result.
static void pso_predict_rest(void) {
    if (!g_exec_next) return;
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S);
    int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty;
    uint32_t cf[8]; memcpy(cf, g_pass.cfmt, sizeof cf); uint32_t df = g_pass.dfmt, sf = g_pass.sfmt, samples = g_pass.samples, dfull = df, sfull = sf; int kicked0 = (int)g_pp_kicked, draws = 0;
    fz_cursor cur = {.p = g_exec_next};
    // 先收齐再丢：按「附件原样挂」的键排在前（最可能用到），其余挂法排在后——后台并发有限，先丢的先编完
    // Collect them all first, then dispatch: keys for "attachments attached as-is" go first (most likely to be used), the other attach modes after -- background concurrency is limited, and what is dispatched first is compiled first
    enum { QN = 160 }; static struct { pso_key k; void *vs, *fs; } qa[QN], qb[QN]; int na = 0, nb = 0;
    for (int guard = 0; guard < 200000 && na < QN; guard++) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!fz_next(&cur, &q, &hq, &aq)) break;
        if (hq.op == OP_SET_RT) {
            const p_set_rt *r = aq; memset(cf, 0, sizeof cf); dfull = sfull = 0; samples = 1;
            for (int i = 0; i < r->n && i < 8; i++) { id<MTLTexture> t = r->color[i] ? tex_mtl(r->color[i]) : nil; if (t) { cf[i] = (uint32_t)rt_color_fmt(t, &r->cview[i], r->has_cview[i], 0); samples = (uint32_t)t.sampleCount; } }
            id<MTLTexture> dt = r->depth ? tex_mtl(r->depth) : nil;
            if (dt) { MTLPixelFormat f = dt.pixelFormat; samples = (uint32_t)dt.sampleCount;
                dfull = f != MTLPixelFormatStencil8 ? (uint32_t)f : 0; sfull = (f == MTLPixelFormatStencil8 || f == MTLPixelFormatDepth32Float_Stencil8) ? (uint32_t)f : 0; }
            df = dfull; sf = sfull; continue;
        }
        if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) {
            draws++;
            __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr || !pr->sh[0]) continue;
            __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
            const uint32_t dv[4] = {dfull, 0, 0, dfull}, sv[4] = {sfull, 0, sfull, 0};
            for (int v = 0; v < 4; v++) {
                if (v && dv[v] == dv[0] && sv[v] == sv[0]) continue;
                if (v == 2 && !sfull) continue; if (v == 3 && (!dfull || !sfull)) continue;
                pso_key k; pso_key_fill(&k, vs, fs, cf, dv[v], sv[v], samples, 0);
                if (hm_get_id(&g_pso, &k)) continue;
                int dup = 0; for (int i = 0; i < (v ? nb : na) && !dup; i++) dup = !memcmp(&(v ? qb : qa)[i].k, &k, sizeof k);
                if (dup) continue;
                if (!v && na < QN) { qa[na].k = k; qa[na].vs = (__bridge void *)vs; qa[na].fs = (__bridge void *)fs; na++; }
                else if (v && nb < QN) { qb[nb].k = k; qb[nb].vs = (__bridge void *)vs; qb[nb].fs = (__bridge void *)fs; nb++; }
            }
            continue;
        }
        if (hq.op == OP_CLEAR_COLOR || hq.op == OP_CLEAR_DS) continue;
        state_op(hq.op, aq);
    }
    (void)df; (void)sf;
    for (int i = 0; i < na; i++) pso_pend_kick(&qa[i].k, (__bridge NvShader *)qa[i].vs, (__bridge NvShader *)qa[i].fs);
    for (int i = 0; i < nb; i++) pso_pend_kick(&qb[i].k, (__bridge NvShader *)qb[i].vs, (__bridge NvShader *)qb[i].fs);
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); g_pso_dirty = pso_dirty; g_dss_dirty = dss_dirty;
    static int logged; if (g_pp_kicked > (uint64_t)kicked0 && logged++ < 20) MLOG("并行预编：本次提交余下 %d 次绘制，丢给后台 %d 条管线", draws, (int)g_pp_kicked - kicked0);
}

static void fz_probe_cov2(const p_set_rt *r2, const uint8_t *first, const int8_t *p2map, const uint8_t *clr, int n1, int n) {
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R;
    const p_draw *fdw = g_fd_dw; const p_draw_idx *fdi = g_fd_di; int fdwhy = g_fd_why;
    int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty;
    op_hdr fh; memcpy(&fh, first, sizeof fh);
    g_fd_dw = fh.op == OP_DRAW ? (const p_draw *)(first + 8) : NULL;
    g_fd_di = fh.op == OP_DRAW_IDX ? (const p_draw_idx *)(first + 8) : NULL;
    R.rt = *r2; R.has_rt = 1;
    fz_cursor cur = {.p = g_exec_next};
    for (;;) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!fz_next(&cur, &q, &hq, &aq)) break;
        if (q == first) break;
        if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX || hq.op == OP_SET_RT || hq.op == OP_CLEAR_COLOR || hq.op == OP_CLEAR_DS) continue;
        state_op(hq.op, aq);
    }
    for (int j = n1; j < n && j < 8; j++) {
        int k = -1;
        for (int q = 0; q < 8; q++) if (p2map[q] == j) { k = q; break; }
        if (k < 0 || clr[k] || !r2->color[k]) continue;
        uint64_t ser; int level, slice;
        rt_sub(r2, k, &ser, &level, &slice);
        (void)ser; (void)slice;
        g_fz.cov2[j] = (uint8_t)first_draw_covers(k, tex_mtl(r2->color[k]), (NSUInteger)level);
    }
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave;
    g_fd_dw = fdw; g_fd_di = fdi; g_fd_why = fdwhy;
    g_pso_dirty = pso_dirty; g_dss_dirty = dss_dirty;
}

static int fz_load_covers(int i, id<MTLTexture> t, NSUInteger level) {
    if (g_fz.active && g_fz.phase == 1 && i >= g_fz.n1) return g_fz.cov2[i];
    return first_draw_covers(i, t, level);
}

// 探针收尾：每个被改成清哨兵色的附件回读该级 / 层，与同格式 1x1 哨兵像素逐像素比，数剩余（> 0 即覆盖证明有误，报红）
// Probe wrap-up: for every attachment changed to clear to the sentinel color, read back that level / layer, compare pixel by pixel with a same-format 1x1 sentinel pixel, and count what remains (> 0 means the coverage proof is wrong, report a failure)
static void lp_finish(void) {
    id<MTLCommandBuffer> dcb = q_cb();
    NSArray *ps = g_lp; g_lp = nil;
    for (NSArray *e in ps) {
        id<MTLTexture> t = e[0]; NSUInteger lv = [e[1] unsignedIntegerValue], sl = [e[2] unsignedIntegerValue];
        NSUInteger W = MAX(t.width >> lv, 1), H = MAX(t.height >> lv, 1), bpp = (NSUInteger)(att_bytes(t, lv) / (W * H));
        MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:1 height:1 mipmapped:NO];
        rd.usage = MTLTextureUsageRenderTarget; rd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> ref = [g_dev newTextureWithDescriptor:rd];
        MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor];
        pd.colorAttachments[0].texture = ref; pd.colorAttachments[0].loadAction = MTLLoadActionClear; pd.colorAttachments[0].storeAction = MTLStoreActionStore;
        pd.colorAttachments[0].clearColor = [e[6] intValue] == 3 ? MTLClearColorMake(NAN, NAN, NAN, NAN) : [e[6] intValue] == 2 ? MTLClearColorMake(0.777, 0.222, 0.555, 0.888) : MTLClearColorMake(0.123, 0.456, 0.789, 0.321);
        [[dcb renderCommandEncoderWithDescriptor:pd] endEncoding];
        id<MTLBuffer> bt = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared], br = [g_dev newBufferWithLength:bpp options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [dcb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:sl sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:bt destinationOffset:0 destinationBytesPerRow:W * bpp destinationBytesPerImage:W * H * bpp];
        [b copyFromTexture:ref sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:br destinationOffset:0 destinationBytesPerRow:bpp destinationBytesPerImage:bpp];
    enc_b_end(b);
        unsigned long long pass = [e[3] unsignedLongLongValue], ser = [e[5] unsignedLongLongValue]; int idx = [e[4] intValue];
        [dcb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c;
            const uint8_t *p = bt.contents, *r = br.contents; uint64_t left = 0; char at[96] = ""; size_t ao = 0;
            for (NSUInteger k = 0; k < W * H; k++) if (!memcmp(p + k * bpp, r, bpp)) { if (left++ < 4) ao += snprintf(at + ao, sizeof at - ao, " (%lu,%lu)", (unsigned long)(k % W), (unsigned long)(k / W)); }
            if (left) MLOG("【红】读入可省探针：通道 %llu c%d ser=%llu lv%lu sl%lu %lux%lu 剩哨兵像素 %llu，前几个位置%s", pass, idx, ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)W, (unsigned long)H, (unsigned long long)left, at);
            else MLOG("读入可省探针：通道 %llu c%d ser=%llu lv%lu sl%lu %lux%lu 全覆盖（哨兵剩 0）", pass, idx, ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)W, (unsigned long)H);
        }];
    }
    [dcb commit];
}
static int rt_same(const p_set_rt *a, const p_set_rt *b) {   // 同一组目标：按贴图序号与视图比（不比对象指针：游戏可能按值拷贝贴图对象） | Same set of targets: compare by texture serial and view (not by object pointer: the game may copy texture objects by value)
    if (a->n != b->n || !a->depth != !b->depth || (a->depth && a->depth->serial != b->depth->serial) || a->has_dview != b->has_dview) return 0;
    if (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview)) return 0;
    for (int k = 0; k < a->n && k < 8; k++) {
        if (!a->color[k] != !b->color[k] || (a->color[k] && a->color[k]->serial != b->color[k]->serial) || a->has_cview[k] != b->has_cview[k]) return 0;
        if (a->has_cview[k] && memcmp(&a->cview[k], &b->cview[k], sizeof a->cview[k])) return 0;
    }
    return 1;
}
// 实验RT范围候选：只忽略实际渲染附件未使用的正nlevels差异，其它字段保持逐字相同。
// Experimental RT range candidate: only ignore positive nlevels differences not used by the actual render attachments; other fields stay word-for-word identical.
#define NVN_RT_RANGE_IMPL 1
static int rt_range_shape(const p_set_rt *a, const p_set_rt *b) {
    if (a->n != 1 || b->n != 1 || !a->color[0] || !b->color[0] || !a->has_cview[0] || !b->has_cview[0] ||
        !a->depth != !b->depth || a->has_dview != b->has_dview ||
        (a->has_dview && memcmp(&a->dview,&b->dview,sizeof a->dview))) return 0;
    const tex_t *x=a->color[0], *y=b->color[0];
    if (!x->serial || x->serial != y->serial || x->d.target != NVN_TEXTURE_TARGET_2D || y->d.target != NVN_TEXTURE_TARGET_2D ||
        x->d.samples > 1 || y->d.samples > 1 || (x->has_layout && x->mem) || (y->has_layout && y->mem) ||
        memcmp(&x->d,&y->d,sizeof x->d) || x->mem != y->mem || x->pool != y->pool || x->off != y->off) return 0;
    const view_t *v=&a->cview[0], *w=&b->cview[0]; int levels=x->d.levels;
    if (!v->set_levels || !w->set_levels || v->base_level < 0 || v->base_level >= levels ||
        v->nlevels <= 0 || v->nlevels > levels-v->base_level || w->nlevels <= 0 || w->nlevels > levels-v->base_level ||
        (v->set_layers && (v->min_layer || v->nlayers != 1)) || (v->set_format && v->format != x->d.format) ||
        (v->set_target && v->target != NVN_TEXTURE_TARGET_2D)) return 0;
    view_t cv=*w; cv.nlevels=v->nlevels;
    if (memcmp(v,&cv,sizeof cv)) return 0;
    if (a->depth) { const tex_t *d=a->depth,*e=b->depth;
        if (!d->serial || d->serial != e->serial || d->d.target != NVN_TEXTURE_TARGET_2D || e->d.target != NVN_TEXTURE_TARGET_2D ||
            d->d.samples > 1 || e->d.samples > 1 || (d->has_layout && d->mem) || (e->has_layout && e->mem) ||
            memcmp(&d->d,&e->d,sizeof d->d) || d->mem != e->mem || d->pool != e->pool || d->off != e->off) return 0;
        int dl=a->has_dview && a->dview.set_levels ? a->dview.base_level : 0;
        if (dl < 0 || dl >= d->d.levels || mip_extent(d->d.width,dl) < mip_extent(x->d.width,v->base_level) ||
            mip_extent(d->d.height,dl) < mip_extent(x->d.height,v->base_level)) return 0; }

    return v->nlevels != w->nlevels;
}
#define NVN_RT_RANGE_DEPTH_IMPL 1
static void rt_range_note_depth(void) { if (rt_range_on() && S.ds.dw) g_pass.range_depth_nonro=1; }
static int rt_range_same(const p_set_rt *a, const p_set_rt *b) {
    if (!rt_range_on() || !g_enc || g_fz.active || g_pass.range_external || g_pass.range_depth_nonro || !S.ds.dt || S.ds.dw || S.ds.st || g_enc_mode != 4 || !g_pass.hd || g_pass.hs ||
        (g_vis_on && !g_enc_has_vis) || g_pass.reg_gen != g_reg_gen || g_pass.ser_gen != g_ser_gen || g_pass.range_dirty != nvn_dirty_now() || !rt_range_shape(a,b) || !b->depth) return 0;
    id<MTLTexture> color=tex_mtl(b->color[0]), depth=tex_mtl(b->depth);
    int lv=b->cview[0].base_level;
    return mip_extent(color.width,lv) == g_pass.tw && mip_extent(color.height,lv) == g_pass.th &&
        color == g_pass.range_color && depth == g_pass.range_depth && color.textureType == MTLTextureType2D &&
        depth.textureType == MTLTextureType2D && color.sampleCount == 1 && depth.sampleCount == 1 &&
        g_pass.reg_gen == g_reg_gen && g_pass.ser_gen == g_ser_gen && g_pass.range_dirty == nvn_dirty_now();
}
static int draw_samples_rt(NvProg *pr) {   // 采样附件保持原ser级保守guard，不分级/层 | Sampled attachments keep the original conservative ser-level guard, not split by level/layer
    uint64_t rs[9]; int nr = 0;
    for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k]) rs[nr++] = R.rt.color[k]->serial;
    if (R.rt.depth) rs[nr++] = R.rt.depth->serial;
    if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 && sh->stage < 6 ? S.img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 && sh->stage < 6 ? S.tex[sh->stage][b->reg] : 0; else continue;
            int tid = (int)(h & 0xfffff); if (!tid) continue;
            pthread_mutex_lock(&g_rmu); uint64_t ser = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t.serial : 0; pthread_mutex_unlock(&g_rmu);
            for (int j = 0; j < nr; j++) if (ser && ser == rs[j]) return 1; } }
    return 0;
}
// 整通道对深度 / 模板的需求（位：1 深度 2 模板；3 = 两项都要或证不了）。从开通道的这次绘制起，按真实回放顺序扫到通道结束：
// 挂上的清除与本次绘制的状态先计入；之后的绘制按其深度模板状态累计；中途清除、嵌套过深等证不了的一律按两项都要
// The whole render pass's need for depth / stencil (bits: 1 depth 2 stencil; 3 = both needed or cannot be proven). Starting from the draw that opens the render pass, scan in real replay order to the end of the render pass:
// the attached clears and this draw's state are counted first; later draws accumulate by their depth-stencil state; anything unprovable such as a mid-pass clear or nesting too deep is treated as both needed
static int pass_ds_need(void) {
    int need = (R.clear_d || S.ds.dt || S.ds.dw ? 1 : 0) | (R.clear_s || S.ds.st ? 2 : 0);
    if (!R.rt.depth || need == 3 || !g_exec_next || g_ret_n > 16) {
        PD("S %llu 不前瞻 depth=%d clear_d=%d clear_s=%d dt=%d dw=%d st=%d next=%d ret=%d", g_stat_pass + 1, R.rt.depth != NULL, R.clear_d, R.clear_s, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.st, g_exec_next != NULL, g_ret_n);
        return 3;
    }
    ds_st ds = S.ds; g_ds_why = -1; g_ds_scanned = 0; g_ds_need = need;
    int r = ds_scan(g_exec_next, &ds, 0);
    for (int k = g_ret_n - 1; r == DS_RET && k >= 0; k--) r = ds_scan(g_ret[k], &ds, 0);
    for (int k = 0; r == DS_RET && k < g_nrest; k++) r = ds_scan((const uint8_t *)g_rest[k], &ds, 0);
    int out = r == DS_USED ? 3 : g_ds_need;
    PD("S %llu 前瞻 结果=%d 需求=%d（1 深度 2 模板）原因=%d 扫过 %d 条 返回栈 %d 余句柄 %d", g_stat_pass + 1, r, out, g_ds_why, g_ds_scanned, g_ret_n, g_nrest);
    return out;
}
// 纯清除通道前提：开通道时向后扫，形如「本通道若干绘制 → 若干组（SetRenderTargets + 只有清除）→ 回到与本通道相同的目标」，
// 就把这几组清除提到本通道之前执行、原位置跳过，回到同一目标时由同目标续用接上——本通道的附件不再为夹在中间的清除整块写回再读入
// （选角页：grunge 与粒子都画 28 / 27 第 0 级 + 深度，中间只清第 1、2 级）。
// 等价条件（全部满足才前提，否则照原样）：扫描区间只有绘制、状态与这些清除，没有拷贝 / 派发 / 清贴图 / 子列表；被清的子资源（贴图 + 级 + 层）
// 与本通道附件不相交；被清的贴图既不在当前任一贴图 / 图像槽上、扫描区间里也没被绑上（保守：不论程序采不采样）；清除时的剪裁覆盖整块（同原位置执行的要求）
// Pure-clear render pass hoisting: when a render pass is opened, scan forward, and if the shape is "several draws of this render pass -> several groups (SetRenderTargets + only clears) -> back to the same targets as this render pass",
// then hoist those groups of clears to execute before this render pass and skip them at their original positions, and on returning to the same targets the same-target continuation picks up -- this render pass's attachments are no longer stored whole and loaded again for the clears sandwiched in between
// (character select page: grunge and particles both draw 28 / 27 level 0 + depth, with only levels 1 and 2 cleared in between).
// Equivalence conditions (hoist only if all hold, otherwise leave as-is): the scan range has only draws, state and these clears, no copies / dispatches / texture clears / sub-lists; the cleared subresources (texture + level + layer)
// are disjoint from this render pass's attachments; the cleared textures are neither on any current texture / image slot nor bound within the scan range (conservative: regardless of whether the program samples them); the scissor at the time of the clear covers the whole block (same requirement as executing at the original position)
static uint64_t h_ser(uint64_t h) { int tid = (int)(h & 0xfffff); return tid ? atomic_load_explicit(&g_tser[tid], memory_order_relaxed) : 0; }
// ---------- 写回前瞻索引：提交开始按回放顺序线性扫一遍，按贴图记事件；通道结束时每个附件二分找「之后第一个有效事件」定论 ----------
// 取代原「每个通道结束向后整段扫」（通道数 × 命令数，营地每帧 49 通道）。与原扫描逐附件对拍：画面闸门 17 页 + 营地局约 34 万次附件判定（多余约 4.6 万）0 不一致后删去原扫描。
// 事件（seq = 回放序号）：READ 绘制 / 派发按程序反射读到该贴图；ATT 绘制时它是当前附件；CLEAR 作附件被整块清除；COPY / CLRTEX 作拷贝两端 / 被清贴图。
// 判定（首个有效事件定论）：READ / COPY / CLRTEX → 需要；ATT 且其 SetRenderTargets 在通道结束之后 → 需要（读入）；CLEAR 且同上 → 多余；无事件 → 需要。
// 任何不确定（回放序号与索引对不上、嵌套过深）索引作废，本次提交余下通道一律写回（写回总是正确的，只是少省带宽）。
// ---------- Store lookahead index: at the start of a submit do one linear scan in replay order and record events per texture; when a render pass ends, binary-search each attachment for "the first valid event afterwards" to decide ----------
// Replaces the original "scan the whole remainder forward at every render pass end" (pass count x command count, 49 render passes per frame in camp). Cross-checked per attachment against the original scan: after 0 mismatches over the 17 pages of the screenshot gate + a camp run with about 340,000 attachment decisions (about 46,000 redundant), the original scan was removed.
// Events (seq = replay sequence number): READ a draw / dispatch reads this texture per program reflection; ATT it is a current attachment at a draw; CLEAR cleared as a whole block while an attachment; COPY / CLRTEX either end of a copy / the cleared texture.
// Decision (the first valid event decides): READ / COPY / CLRTEX -> needed; ATT whose SetRenderTargets is after the render pass end -> needed (load); CLEAR with the same -> redundant; no event -> needed.
// On any uncertainty (replay sequence number does not match the index, nesting too deep) the index is invalidated and all remaining render passes of this submit store (storing is always correct, it just saves less bandwidth).
typedef struct { uint32_t seq, rtseq; uint64_t ser; const uint8_t *op; uint16_t lv, sl; uint8_t kind, plane, anyls, why; uint16_t rep; } sdx_ev;   // rep：并进这条的同通道后续 ATT 条数（defer_pre 的步数照原样算）   // plane 0 颜色 8 深度模板（ATT）1 深度 2 模板（CLEAR） | rep: number of later same-pass ATT entries merged into this one (defer_pre step counts unchanged)   // plane 0 color 8 depth-stencil (ATT) 1 depth 2 stencil (CLEAR)
enum { SDX_READ = 1, SDX_NEED, SDX_ATT, SDX_CLEAR };
#define SDX_NONE 0xffffffffu
static sdx_ev *g_sdx, *g_sdx_tmp; static uint32_t g_sdx_n, g_sdx_cap; static int g_sdx_ok;   // 索引有效 | Index valid
// 候选贴图（当过附件的）→ 桶：开放定址；事件发生时尚未当过附件的贴图，对此前结束的任何通道都无关，不记
// Candidate textures (those that have been attachments) -> buckets: open addressing; a texture that has not yet been an attachment when an event occurs is irrelevant to any render pass that ended before then, so it is not recorded
#define SDX_HB 2048
static uint64_t g_sdx_hk[SDX_HB]; static uint16_t g_sdx_hb[SDX_HB]; static uint32_t g_sdx_nb, g_sdx_bcnt[SDX_HB + 1], g_sdx_bst[SDX_HB + 1];
static int sdx_bucket(uint64_t ser, int add) {   // 返回桶号；不在集合（且不加）返回 −1；满了作废索引 | Returns the bucket number; returns -1 if not in the set (and not adding); invalidates the index when full
    if (!ser) return -1;
    for (uint32_t i = (uint32_t)(ser * 0x9E3779B97F4A7C15ull >> 53), n = 0; n < SDX_HB; i = (i + 1) & (SDX_HB - 1), n++) {
        if (g_sdx_hk[i] == ser) return g_sdx_hb[i];
        if (!g_sdx_hk[i]) { if (!add) return -1; if (g_sdx_nb >= SDX_HB / 2) { g_sdx_ok = 0; return -1; } g_sdx_hk[i] = ser; g_sdx_hb[i] = (uint16_t)g_sdx_nb; return (int)g_sdx_nb++; }
    }
    return -1;
}
static const uint8_t **g_sdx_op; static uint32_t g_sdx_nop, g_sdx_opcap;
static uint32_t g_exec_seq;   // 回放中当前操作的序号 | Sequence number of the current operation during replay
static uint64_t g_pp_slot[6][160]; static const prog_t *g_pp_prog; static const p_set_rt *g_pp_rt; static uint32_t g_pp_rtseq;
static uint32_t *g_rtx_seq, *g_drx_seq; static uint32_t g_rtx_n, g_rtx_cap, g_drx_n, g_drx_cap;
// 同一通道（同一个 SetRenderTargets 序号）里每次绘制的「作附件」事件内容相同（同贴图、同平面、同级别层），判定都只认第一条：
// 只记通道里第一次绘制的那几条，后续绘制只在它们的 rep 上加一（g_sdx_attdedup=0 回到每次绘制都记，同进程交替量用）
// Within the same pass (same SetRenderTargets sequence number), every draw's "as attachment" events have identical content (same texture, same plane, same level/layer), and the decision only looks at the first:
// only the entries of the first draw in the pass are recorded, and later draws just increment rep on them (g_sdx_attdedup=0 returns to recording every draw, for in-process alternating measurement)
int g_sdx_attdedup = 1; static uint32_t g_att_rtseq = 0xffffffffu, g_att_idx[9]; static int g_att_n;   // 本次提交的 SET_RT / 绘制序号表（可读 tile 合并的预筛用） | This submit's SET_RT / draw sequence number tables (used by the pre-screen of the readable tile merge)
static void seq_push(uint32_t **v, uint32_t *n, uint32_t *cap, uint32_t x) { if (*n == *cap) { *cap = *cap ? *cap * 2 : 1024; *v = realloc(*v, *cap * sizeof **v); } (*v)[(*n)++] = x; }
static void sdx_ev_add(sdx_ev e) { if (sdx_bucket(e.ser, 0) < 0) return; if (g_sdx_n == g_sdx_cap) { g_sdx_cap = g_sdx_cap ? g_sdx_cap * 2 : 4096; g_sdx = realloc(g_sdx, g_sdx_cap * sizeof *g_sdx); g_sdx_tmp = realloc(g_sdx_tmp, g_sdx_cap * sizeof *g_sdx); } g_sdx[g_sdx_n++] = e; }
static void sdx_lvsl(const view_t *v, int hv, uint16_t *lv, uint16_t *sl) { *lv = (uint16_t)(hv && v->set_levels ? v->base_level : 0); *sl = (uint16_t)(hv && v->set_layers ? v->min_layer : 0); }
static void sdx_walk(const uint8_t *p, int depth) {
    if (depth > 64) { g_sdx_ok = 0; return; }
    for (uint32_t guard = 0; guard < 4000000 && p && g_sdx_ok; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (g_sdx_nop == g_sdx_opcap) { g_sdx_opcap = g_sdx_opcap ? g_sdx_opcap * 2 : 16384; g_sdx_op = realloc(g_sdx_op, g_sdx_opcap * sizeof *g_sdx_op); }
        uint32_t seq = g_sdx_nop; g_sdx_op[g_sdx_nop++] = p;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); sdx_walk((const uint8_t *)t, depth + 1); break; }
            case OP_SET_RT: { const p_set_rt *r = a; g_pp_rt = r; g_pp_rtseq = seq; seq_push(&g_rtx_seq, &g_rtx_n, &g_rtx_cap, seq);
                for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) sdx_bucket(r->color[k]->serial, 1);
                if (r->depth) sdx_bucket(r->depth->serial, 1);
                break; }
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; const p_set_rt *r = g_pp_rt;
                if (!r || (c->mask & 0xf) != 0xf || c->index < 0 || c->index >= r->n || c->index >= 8 || !r->color[c->index]) break;
                uint16_t lv, sl; sdx_lvsl(&r->cview[c->index], r->has_cview[c->index], &lv, &sl); int last = -1;   // 同一子资源挂在多个序号时按最后一个算 | When the same subresource is attached at several indices, the last one counts
                for (int k = 0; k < r->n && k < 8; k++) { uint16_t l2, s2; if (!r->color[k] || r->color[k]->serial != r->color[c->index]->serial) continue; sdx_lvsl(&r->cview[k], r->has_cview[k], &l2, &s2); if (l2 == lv && s2 == sl) last = k; }
                if (last == c->index) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->color[c->index]->serial, p, lv, sl, SDX_CLEAR, 0, 0, 0});
                break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a; const p_set_rt *r = g_pp_rt; if (!r || !r->depth) break;
                uint16_t lv, sl; sdx_lvsl(&r->dview, r->has_dview, &lv, &sl);
                if (c->depth_mask) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_CLEAR, 1, !r->has_dview, 0});
                if ((c->stencil_mask & 0xff) == 0xff) sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_CLEAR, 2, !r->has_dview, 0});
                break; }
            case OP_BIND_TEX: case OP_BIND_IMG: { const p_bind_handles *b = a; int base = h.op == OP_BIND_IMG ? 128 : 0, lim = h.op == OP_BIND_IMG ? 32 : 128;
                if (b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) g_pp_slot[b->stage][base + b->first + k] = h_ser(b->h[k]);
                break; }
            case OP_BIND_PROGRAM: g_pp_prog = ((const p_bind_program *)a)->prog; break;
            case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: {
                if (h.op != OP_DISPATCH) seq_push(&g_drx_seq, &g_drx_n, &g_drx_cap, seq);
                __unsafe_unretained NvProg *pr = g_pp_prog ? prog_get(g_pp_prog) : nil;
                if (!pr) { for (int st = 0; st < 6; st++) for (int k = 0; k < 160; k++) if (g_pp_slot[st][k]) sdx_ev_add((sdx_ev){seq, 0, g_pp_slot[st][k], p, 0, 0, SDX_READ, 0, 0, 1}); }
                else for (int st = 0; st < 6; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || (h.op == OP_DISPATCH) != (st == 5)) continue;
                    for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; int k = b->kind == NVN_RB_IMG ? 128 + b->reg : (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) ? b->reg : -1;
                        if (k < 0 || k >= 160 || sh->stage < 0 || sh->stage >= 6) continue; uint64_t ser = g_pp_slot[sh->stage][k];
                        if (ser) sdx_ev_add((sdx_ev){seq, 0, ser, p, 0, 0, SDX_READ, 0, 0, 1}); } }
                const p_set_rt *r = g_pp_rt;
                if (h.op != OP_DISPATCH && r && g_sdx_attdedup && g_att_rtseq == g_pp_rtseq) {
                    for (int q = 0; q < g_att_n; q++) if (g_sdx[g_att_idx[q]].rep < 0xffff) g_sdx[g_att_idx[q]].rep++;
                } else if (h.op != OP_DISPATCH && r) {
                    uint32_t n0 = g_sdx_n; g_att_rtseq = g_pp_rtseq; g_att_n = 0;
                    for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) { uint16_t lv, sl; sdx_lvsl(&r->cview[k], r->has_cview[k], &lv, &sl); sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->color[k]->serial, p, lv, sl, SDX_ATT, 0, 0, 2}); }
                    if (r->depth) { uint16_t lv, sl; sdx_lvsl(&r->dview, r->has_dview, &lv, &sl); sdx_ev_add((sdx_ev){seq, g_pp_rtseq, r->depth->serial, p, lv, sl, SDX_ATT, 8, !r->has_dview, 2}); }
                    for (uint32_t q = n0; q < g_sdx_n && g_att_n < 9; q++) if (g_sdx[q].kind == SDX_ATT) g_att_idx[g_att_n++] = q;
                }
                break; }
            case OP_COPY_T2T: { const p_copy_t2t *c = a; if (c->src) sdx_ev_add((sdx_ev){seq, 0, c->src->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); if (c->dst) sdx_ev_add((sdx_ev){seq, 0, c->dst->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); break; }
            case OP_COPY_T2B: case OP_COPY_B2T: { const p_copy_b2t *c = a; if (c->tex) sdx_ev_add((sdx_ev){seq, 0, c->tex->serial, p, 0, 0, SDX_NEED, 0, 0, 3}); break; }
            case OP_CLEAR_TEX: { const p_clear_tex *c = a; if (c->tex) sdx_ev_add((sdx_ev){seq, 0, c->tex->serial, p, 0, 0, SDX_NEED, 0, 0, 4}); break; }
            default: break;
        }
        p += h.size;
    }
}
void mtl_sub_index(const uint64_t *h, int n) {   // 提交开始（hle_nvn.c QueueSubmitCommands）：建本次提交的写回前瞻索引 | Start of submit (hle_nvn.c QueueSubmitCommands): build this submit's store lookahead index
    g_xv_ntouch = 0; g_sdx_n = g_sdx_nop = 0; g_rtx_n = g_drx_n = 0; g_exec_seq = 0; g_sdx_ok = tb_sub(TB_SDX); g_att_rtseq = SDX_NONE; g_att_n = 0;
    if (!g_sdx_ok) { g_xv_n = 0; g_xv_block = 1; return; }   // 索引不可用：账本清空、本次提交不新增（没法核下一次触碰） | Index unavailable: clear the ledger and add nothing in this submit (the next touch cannot be verified)
    g_xv_block = 0;
    memset(g_sdx_hk, 0, sizeof g_sdx_hk); g_sdx_nb = 0;
    for (int i = 0; i < g_xv_n; i++) sdx_bucket(g_xv[i].ser, 1);   // 账本里的贴图进候选集：否则它在本次提交里只被采样、不作附件时读事件不入索引，兜底看不见 | Textures in the ledger enter the candidate set: otherwise, when one is only sampled and not used as an attachment in this submit, its read events are not indexed and the fallback cannot see them
    if (R.has_rt) { for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k]) sdx_bucket(R.rt.color[k]->serial, 1); if (R.rt.depth) sdx_bucket(R.rt.depth->serial, 1); }   // 提交开始时的目标（首个绘制可能不经 SetRenderTargets 重开通道） | The targets at the start of the submit (the first draw may reopen the render pass without going through SetRenderTargets)
    g_pp_prog = S.prog; g_pp_rt = NULL; g_pp_rtseq = SDX_NONE;
    for (int st = 0; st < 6; st++) { for (int k = 0; k < 128; k++) g_pp_slot[st][k] = S.tex[st][k] ? h_ser(S.tex[st][k]) : 0;
                                     for (int k = 0; k < 32; k++) g_pp_slot[st][128 + k] = S.img[st][k] ? h_ser(S.img[st][k]) : 0; }
    for (int i = 0; i < n && g_sdx_ok; i++) sdx_walk((const uint8_t *)h[i], 0);
    if (!g_sdx_ok) return;
    // 计数排序到桶：事件本按序号追加，同一序号内 READ 先于 ATT（同一绘制先判读后判附件），稳定散列后桶内有序
    // Counting sort into buckets: events are appended in sequence-number order, and within the same sequence number READ precedes ATT (the same draw is judged for reads first, then attachments), so after a stable scatter each bucket is ordered
    memset(g_sdx_bcnt, 0, (g_sdx_nb + 1) * sizeof *g_sdx_bcnt);
    for (uint32_t i = 0; i < g_sdx_n; i++) g_sdx_bcnt[sdx_bucket(g_sdx[i].ser, 0)]++;
    for (uint32_t b = 0, acc = 0; b <= g_sdx_nb; b++) { g_sdx_bst[b] = acc; acc += b < g_sdx_nb ? g_sdx_bcnt[b] : 0; }
    for (uint32_t b = 0; b < g_sdx_nb; b++) g_sdx_bcnt[b] = g_sdx_bst[b];
    for (uint32_t i = 0; i < g_sdx_n; i++) g_sdx_tmp[g_sdx_bcnt[sdx_bucket(g_sdx[i].ser, 0)]++] = g_sdx[i];
    sdx_ev *t = g_sdx; g_sdx = g_sdx_tmp; g_sdx_tmp = t;
    xv_forward();
}
static void sdx_seq(const uint8_t *p) {   // mtl_exec 每条操作：序号与索引核对（不符即作废，余下一律写回） | Every operation in mtl_exec: check the sequence number against the index (on mismatch invalidate, and store everything for the rest)
    if (!g_sdx_ok) return;
    if (g_exec_seq >= g_sdx_nop || g_sdx_op[g_exec_seq] != p) { RED_ONCE(40, "写回前瞻索引与回放顺序不符（序号 %u / %u），本次提交余下通道一律写回", g_exec_seq, g_sdx_nop); g_sdx_ok = 0; return; }
    g_exec_seq++;
}
static int sdx_decide(uint8_t st[9]) {   // st[i]：2 多余；否则 why << 2（需要的原因，同 sd_need）。返回 0 = 索引不可用 | st[i]: 2 redundant; otherwise why << 2 (the reason it is needed, same as sd_need). Returns 0 = index unavailable
    if (!g_sdx_ok || !g_exec_cur || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 0;
    uint32_t P = g_exec_seq - 1;
    for (int i = 0; i < g_nsd; i++) { const sd_att *t = &g_sd[i]; st[i] = 5 << 2;
        int b = sdx_bucket(t->ser, 0); if (b < 0) continue;   // 不在候选集：本提交里没有任何相关事件 → 扫到末尾未定，需要 | Not in the candidate set: no related event at all in this submit -> undecided after scanning to the end, needed
        uint32_t lo = g_sdx_bst[b], end = g_sdx_bst[b + 1], hi = end;   // 桶内第一个 seq ≥ P | First seq >= P in the bucket
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_sdx[m].seq < P) lo = m + 1; else hi = m; }
        for (uint32_t j = lo; j < end; j++) { const sdx_ev *e = &g_sdx[j];
            if (g_hoist_n && hoist_skipped(e->op)) continue;
            if (e->kind == SDX_READ || e->kind == SDX_NEED) { st[i] = (uint8_t)(e->why << 2); break; }
            if (e->rtseq == SDX_NONE || e->rtseq < P || (g_hoist_n && hoist_skipped(g_sdx_op[e->rtseq]))) continue;   // 该目标在通道结束之前设的：通道结束之后未重设为附件，不算 | This target was set before the render pass ended: not re-set as an attachment after the render pass ended, so it does not count
            int hit = t->depth ? (e->kind == SDX_ATT ? e->plane == 8 : e->plane == t->depth) && (e->anyls || (e->lv == t->lv && e->sl == t->sl))
                               : e->plane == 0 && e->lv == t->lv && e->sl == t->sl;
            if (!hit) continue;
            st[i] = e->kind == SDX_CLEAR ? 2 : (uint8_t)(2 << 2); break; }
    }
    return 1;
}
static void xv_forward(void) {
    if (!g_xv_n) return;
    if (!g_sdx_ok) { g_xv_n = 0; g_xv_block = 1; return; }
    for (int i = 0; i < g_xv_n;) {
        xv_ent *e = &g_xv[i]; int b = sdx_bucket(e->ser, 0), action = 0;
        if (b >= 0) for (uint32_t j = g_sdx_bst[b]; j < g_sdx_bst[b + 1]; j++) { sdx_ev *v = &g_sdx[j];
            if (v->kind == SDX_READ || v->kind == SDX_NEED) { xv_ent copy = *e; xv_violation(&copy, "提交前瞻读/需求"); action = 2; break; }
            if (v->lv != e->lv || v->sl != e->sl) continue;
            if (v->kind == SDX_CLEAR) { xv_good(e); action = 1; break; }
            if (v->kind == SDX_ATT) break;
        }
        if (!action) i++;
    }
}
static void rt_sub(const p_set_rt *r, int k, uint64_t *ser, int *lv, int *sl) {   // 附件 k（0–7 颜色，8 深度）的子资源 | Subresource of attachment k (0–7 color, 8 depth)
    const tex_t *t = k == 8 ? r->depth : r->color[k]; const view_t *v = k == 8 ? &r->dview : &r->cview[k]; int hv = k == 8 ? r->has_dview : r->has_cview[k];
    *ser = t ? t->serial : 0; *lv = hv && v->set_levels ? v->base_level : 0; *sl = hv && v->set_layers ? v->min_layer : 0;
}
static void hoist_try(void) {
    if (g_hoist_n || !g_exec_next) return;
    typedef struct { const uint8_t *rt_op; const p_set_rt *rt; uint8_t clear_c[8]; MTLClearColor cc[8]; uint8_t clear_d, clear_s; double cd; uint32_t cs;
                     p_scissor sc; int has_sc; const uint8_t *ops[8]; int nops; } grp;
    grp G[4]; int ng = 0, in_grp = 0; p_scissor sc = R.sc; int has_sc = R.has_sc;
    // 「被清的贴图不在任一槽上」要的绑定集合：只在真找到可前提的清除组、回到本通道目标时才查表（绝大多数通道走不到那一步）。
    // 扫描区间里绑上的句柄先记下，查表时与当前槽一并换成贴图序号；扫描只读命令，期间当前槽 S 不变，结果与开头就查相同
    // The binding set needed for "the cleared texture is not on any slot": the table is looked up only when a hoistable clear group is really found and we are back to this render pass's targets (the vast majority of render passes never get that far).
    // Handles bound within the scan range are noted first and converted to texture serials together with the current slots at lookup time; the scan only reads commands and the current slots S do not change meanwhile, so the result is the same as checking at the start
    uint64_t bh[512]; int nbh = 0;
    const uint8_t *p = g_exec_next;
    for (int guard = 0; guard < 200000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        switch (h.op) {
            case OP_END: case OP_CALL: case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_DRAW: case OP_DRAW_IDX: if (in_grp) return; break;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_BIND_TEX: case OP_BIND_IMG: { const p_bind_handles *b = a; for (int k = 0; k < b->n && k < 32 && nbh < 512; k++) if (b->h[k]) bh[nbh++] = b->h[k]; break; }
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: {
                if (!in_grp || G[ng - 1].nops >= 8) return;
                grp *g = &G[ng - 1];
                if (h.op == OP_CLEAR_COLOR) { const p_clear_color *c = a;
                    if (c->index < 0 || c->index >= g->rt->n || !g->rt->color[c->index] || c->mask != 0xf) return;
                    const tex_t *t = g->rt->color[c->index];
                    if (has_sc && !(sc.x <= 0 && sc.y <= 0 && sc.x + sc.w >= (int)t->d.width && sc.y + sc.h >= (int)t->d.height)) return;
                    g->clear_c[c->index] = 1; g->cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]); }
                else { const p_clear_ds *c = a; if (!g->rt->depth) return;
                    if (c->depth_mask) { g->clear_d = 1; g->cd = c->depth; } if (c->stencil_mask) { g->clear_s = 1; g->cs = (uint32_t)c->stencil; } }
                g->sc = sc; g->has_sc = has_sc; g->ops[g->nops++] = p; break; }
            case OP_SET_RT: { const p_set_rt *r = a;
                if (rt_same(&R.rt, r)) {   // 回到本通道的目标：前提成立的话执行 | Back to this render pass's targets: execute if hoisting holds
                    if (!ng) return;
                    uint64_t bound[512]; int nb = 0;   // 原「开头即查」的顺序：当前各槽（阶段 × 贴图 128 + 图像 32），再接扫描区间里绑上的 | The order of the original "check at the start": the current slots (stage x textures 128 + images 32), followed by those bound within the scan range
                    for (int st = 0; st < 6; st++) { for (int k = 0; k < 128 && nb < 512; k++) if (S.tex[st][k]) bound[nb++] = h_ser(S.tex[st][k]);
                                                     for (int k = 0; k < 32 && nb < 512; k++) if (S.img[st][k]) bound[nb++] = h_ser(S.img[st][k]); }
                    for (int k = 0; k < nbh && nb < 512; k++) bound[nb++] = h_ser(bh[k]);
                    for (int gi = 0; gi < ng; gi++) {   // 不相交、未被绑 | Disjoint and not bound
                        grp *g = &G[gi]; if (!g->nops) return;
                        for (int k = 0; k < 9; k++) { if (k < 8 ? !g->clear_c[k] : !(g->clear_d || g->clear_s)) continue;
                            uint64_t ser; int lv, sl; rt_sub(g->rt, k, &ser, &lv, &sl); if (!ser) return;
                            for (int j = 0; j < nb; j++) if (bound[j] == ser) return;
                            for (int m = 0; m < 9; m++) { uint64_t s2; int l2, sl2; if (m < 8 && m >= R.rt.n) continue; rt_sub(&R.rt, m, &s2, &l2, &sl2);
                                if (s2 == ser && l2 == lv && sl2 == sl) return; } } }
                    // 先登记全部跳过项再执行：前提通道结束时写回前瞻就往后扫，原位置的清除必须已在跳过集合里
                    // Register all skip entries first, then execute: when a hoisted render pass ends the store lookahead scans forward, and the clears at their original positions must already be in the skip set
                    for (int gi = 0; gi < ng; gi++) { g_hoist_skip[g_hoist_n++] = G[gi].rt_op; for (int k = 0; k < G[gi].nops; k++) g_hoist_skip[g_hoist_n++] = G[gi].ops[k]; }
                    p_set_rt rt0 = R.rt; p_scissor sc0 = R.sc; int hs0 = R.has_sc;   // 本通道的目标、剪裁与挂起的清除：执行完原样恢复 | This render pass's targets, scissor and pending clears: restored as they were after execution
                    uint8_t cc0f[8]; MTLClearColor cc0[8]; uint8_t cd0f = R.clear_d, cs0f = R.clear_s; double cd0 = R.cd; uint32_t cs0 = R.cs;
                    memcpy(cc0f, R.clear_c, 8); memcpy(cc0, R.cc, sizeof cc0);
                    for (int gi = 0; gi < ng; gi++) {   // 执行：临时换上该组目标与清除，开只为清除的通道 | Execute: temporarily swap in that group's targets and clears and open a clear-only render pass
                        grp *g = &G[gi];
                        R.rt = *g->rt; memcpy(R.clear_c, g->clear_c, 8); memcpy(R.cc, g->cc, sizeof R.cc); R.clear_d = g->clear_d; R.clear_s = g->clear_s; R.cd = g->cd; R.cs = g->cs;
                        R.sc = g->sc; R.has_sc = g->has_sc;
                        PD("H %llu 前提纯清除通道（原位置在本通道之后）", g_stat_pass + 1);
                        rp_begin_ex(1); rp_end();
                    }
                    R.rt = rt0; R.sc = sc0; R.has_sc = hs0; memcpy(R.clear_c, cc0f, 8); memcpy(R.cc, cc0, sizeof cc0); R.clear_d = cd0f; R.clear_s = cs0f; R.cd = cd0; R.cs = cs0;
                    if (!g_hoist_groups) MLOG("纯清除通道前提：首例 %d 组（通道 %llu 前）", ng, g_stat_pass + 1);
                    g_hoist_groups += (uint64_t)ng;
                    return;
                }
                if (in_grp && !G[ng - 1].nops) return;   // 上一组没有清除（只设目标）：不认 | The previous group has no clear (only set targets): not recognized
                if (ng >= 4) return;
                memset(&G[ng], 0, sizeof G[ng]); G[ng].rt_op = p; G[ng].rt = r; ng++; in_grp = 1; break; }
            default: break;
        }
        p += h.size;
    }
}
// ---------- 可读 tile 通道合并（合并 B 第 2 步） ----------
// 后一个通道的绘制在本像素采样前一个通道写的颜色附件时，两个通道并成一个渲染通道：前一个通道写的内容留在 tile 里，
// 后一个通道用可读 tile 版本（<键>_bf，函数常量特化）直接读，不为被采样而先落主存再读回。
// 规划（fuse_try，前一个通道开头）只做结构检查；本像素寻址的闭式证明在后一个通道每次绘制时做（fz_p2_draw），证不出就结束合并通道、
// 按后一个通道的原目标重开普通通道（前一个通道的结果照常写回，等价于不合并）。
// ---------- Readable tile render pass merge (merge B step 2) ----------
// When a draw of the later pass samples, at the same pixel, a color attachment written by the earlier pass, the two passes are merged into one render pass: what the earlier pass wrote stays in the tile,
// and the later pass reads it directly with the readable tile version (<key>_bf, function-constant specialization), instead of first landing in main memory and being read back just to be sampled.
// Planning (fuse_try, at the start of the earlier pass) does only structural checks; the closed-form proof of same-pixel addressing is done at every draw of the later pass (fz_p2_draw), and if it cannot be proven the merged render pass is ended and
// a normal render pass is reopened with the later pass's original targets (the earlier pass's result is stored as usual, equivalent to not merging).
static void fz_unskip(void) { for (int i = 0; i < g_fz.nskip; i++) for (int j = 0; j < g_hoist_n; j++) if (g_hoist_skip[j] == g_fz.skip[i]) { g_hoist_skip[j] = g_hoist_skip[--g_hoist_n]; break; } }
static int fz_reuse_op(const uint8_t *p) { for (int i = 0; i < g_fz.nreuse; i++) if (g_fz.reuse[i] == p) return 1; return 0; }
// 诊断 DM_NVN_FUSE_CHECK=1：局内双渲染。后一个通道证得出的绘制逐次留状态快照；合并通道结束后，把后一个通道写过的附件拷一份（合并路径的结果），
// 再关掉全部前瞻、按后一个通道的原目标与原清除用普通路径逐次重画，两份逐字节比（同一帧同一输入，不依赖跨局确定性）。重画结果留在附件上（相同即无影响）
// Diagnostic DM_NVN_FUSE_CHECK=1: in-run double rendering. For the later pass's draws that can be proven, keep a state snapshot per draw; after the merged render pass ends, make a copy of the attachments the later pass wrote (the merge path's result),
// then turn off all lookahead and redraw one by one through the normal path with the later pass's original targets and original clears, and compare the two byte by byte (same frame, same input, no dependence on cross-run determinism). The redraw result is left on the attachment (no effect if identical)
static int fz_check_on(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_FUSE_CHECK") != NULL; return v; }
typedef struct { int idx; p_draw dw; p_draw_idx di; p_scissor sc; int has_sc; void *S_; } fz_snap;
static fz_snap g_fzs[8]; static int g_nfzs, g_fzs_bad, g_fz_checking; static uint64_t g_fzc_same, g_fzc_diff;
static void draw(const p_draw *dw, const p_draw_idx *di);
static void fz_snap_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_nfzs >= 8) { g_fzs_bad = 1; return; }
    fz_snap *z = &g_fzs[g_nfzs++]; z->idx = di != NULL; if (dw) z->dw = *dw; if (di) z->di = *di; z->sc = R.sc; z->has_sc = R.has_sc;
    z->S_ = malloc(sizeof S); memcpy(z->S_, &S, sizeof S);
}
static void fz_check_run(const fuse_t *fz) {
    if (!g_nfzs || g_fzs_bad) { if (g_fzs_bad) MLOG("【红】局内双渲染：后一个通道绘制超过 8 次，未比"); goto out; }
    {
    id<MTLTexture> att[8], scr[8]; uint64_t sers[8]; int na = 0;
    // fz_end 已把 R.rt 从物理并集还原为后通道逻辑目标；wrote 仍按物理槽登记，必须经 p2map 对位。
    // fz_end has already restored R.rt from the physical union to the later pass's logical targets; wrote is still registered by physical slot and must be aligned through p2map.
    for (int j = 0; j < fz->rt2.n && j < 8; j++) if (fz->p2map[j] >= 0 && fz->wrote[fz->p2map[j]] && fz->rt2.color[j]) {
        id<MTLTexture> t = tex_mtl(fz->rt2.color[j]); if (!t) continue;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d]; if (!c) continue;
        enc_b_copy_texture(blit(), t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(t.width, t.height, 1), c, 0, 0, MTLOriginMake(0, 0, 0));
        att[na] = t; scr[na] = c; sers[na] = fz->rt2.color[j]->serial; na++; }
    if (!na) { MLOG("【红】局内双渲染：已留 %d 次绘制快照，但没有可比附件", g_nfzs); goto out; }
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R; const uint8_t *en0 = g_exec_next;
    g_fz_checking = 1; g_exec_next = NULL;   // 重画时关掉全部前瞻（续用 / 前提 / 合并 / 深度模板按需）：普通路径 | Turn off all lookahead during the redraw (continuation / hoisting / merging / depth-stencil on demand): normal path
    R.rt = fz->rt2; memcpy(R.clear_c, fz->clr2, 8); memcpy(R.cc, fz->cc2, sizeof R.cc); R.clear_d = fz->cd2f; R.clear_s = fz->cs2f; R.cd = fz->cd2; R.cs = fz->cs2;
    for (int i = 0; i < g_nfzs; i++) { fz_snap *z = &g_fzs[i]; memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1;
        draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
    rp_end();
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave; g_exec_next = en0; g_fz_checking = 0; g_pso_dirty = g_dss_dirty = 1;
    for (int a = 0; a < na; a++) {
        id<MTLTexture> t = att[a], c = scr[a]; NSUInteger W = t.width, H = t.height, bpp = (NSUInteger)(att_bytes(t, 0) / (W * H));
        id<MTLBuffer> b1 = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared], b2 = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared];
        BENC b = blit();
        enc_b_copy_texture_buffer(b, c, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), b1, 0, W * bpp, W * H * bpp);
        enc_b_copy_texture_buffer(b, t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(W, H, 1), b2, 0, W * bpp, W * H * bpp);
        unsigned long long ser = sers[a], pass = g_stat_pass, frame = atomic_load(&g_cur_frame); NSUInteger nb = W * H * bpp;
        [g_commit_blocks addObject:^{
            const uint8_t *x = b1.contents, *y = b2.contents; uint64_t dpx = 0, maxd = 0;
            for (NSUInteger k = 0; k < nb; k += bpp) if (memcmp(x + k, y + k, bpp)) { dpx++; for (NSUInteger q = 0; q < bpp; q++) { uint64_t dd = (uint64_t)abs((int)x[k + q] - (int)y[k + q]); if (dd > maxd) maxd = dd; } }
            if (dpx) { g_fzc_diff++; MLOG("【红】局内双渲染：帧 %llu 通道 %llu 附件 ser=%llu 合并 vs 普通 不同像素 %llu / %lu（逐字节最大差 %llu）", frame, pass, ser, (unsigned long long)dpx, (unsigned long)(nb / bpp), (unsigned long long)maxd); }
            else { g_fzc_same++; MLOG("局内双渲染：帧 %llu 通道 %llu 附件 ser=%llu 合并 vs 普通 逐字节相同（%lu 像素）", frame, pass, ser, (unsigned long)(nb / bpp)); }
            if ((g_fzc_same + g_fzc_diff) % 300 == 0) MLOG("局内双渲染累计：逐字节相同 %llu 次、不同 %llu 次", (unsigned long long)g_fzc_same, (unsigned long long)g_fzc_diff);
        }];
    }
    }
out:
    for (int i = 0; i < g_nfzs; i++) free(g_fzs[i].S_);
    g_nfzs = 0; g_fzs_bad = 0;
}
static void fz_end(void) {   // 合并通道结束（rp_end 调）：还没进后一个通道就被打断 → 撤回跳过项、目标还原成前一个通道的；否则还原成后一个通道的 | End of a merged render pass (called by rp_end): interrupted before entering the later pass -> retract the skip entries and restore the targets to the earlier pass's; otherwise restore to the later pass's
    if (!g_fz.active) return;
    if (g_fz.phase == 1) { fz_unskip(); R.rt = g_fz.rt1; } else R.rt = g_fz.rt2;
    g_fz.active = 0;
    if (g_fz.phase == 2 && fz_check_on() && !g_fz_checking) { fuse_t fz = g_fz; R.rt = fz.rt2; fz_check_run(&fz); }
    else { for (int i = 0; i < g_nfzs; i++) free(g_fzs[i].S_); g_nfzs = 0; g_fzs_bad = 0; }
}
static int sub_eq(const p_set_rt *a, int ka, const p_set_rt *b, int kb) { uint64_t s1, s2; int l1, l2, x1, x2; rt_sub(a, ka, &s1, &l1, &x1); rt_sub(b, kb, &s2, &l2, &x2); return s1 && s1 == s2 && l1 == l2 && x1 == x2; }
static int prog_samples_ser(const prog_t *pg, uint64_t tex[6][128], uint64_t img[6][32], uint64_t ser) {   // 程序反射声明的贴图 / 图像 / 类型化缓冲槽里有没有绑着该贴图 | Whether this texture is bound in any texture / image / typed buffer slot declared by the program's reflection
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 ? tex[sh->stage][b->reg] : 0; else continue;
            if (h && h_ser(h) == ser) return 1; } }
    return 0;
}
static int g_defer_env_ab, g_defer_env_off;   // 环境开关：每次提交开始（defer_reset）读一次，热路径不调 getenv | Environment switches: read once at the start of each submit (defer_reset); the hot path does not call getenv
static void defer_env(void) { const char *x = getenv("DM_NVN_DEFER_AB"), *e = getenv("DM_NVN_DEFER"); g_defer_env_ab = x && !strcmp(x, "1"); g_defer_env_off = e && !strcmp(e, "0"); }
static int defer_on(void) {
    if (!g_defer_env_ab) return tb_sub(TB_DEFER) && !g_defer_env_off;
    if (g_defer_ab < 0) { g_defer_ab = 0; g_defer_ab_on = 0; }
    return tb_sub(TB_DEFER) && (g_defer_ab || g_defer_ab_on); }
static int defer_handle_sub(uint64_t h, const p_set_rt *r, int k, int cover) {
    int tid = (int)(h & 0xfffff); if (!tid || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    uint64_t ser; int lv, sl; rt_sub(r, k, &ser, &lv, &sl); if (!ser || g_treg[tid].t.serial != ser) return 0;
    if (!g_treg[tid].has_v || !g_treg[tid].v.set_levels) return cover;
    view_t v = g_treg[tid].v; int n = v.nlevels > 0 ? v.nlevels : 1;
    return lv >= v.base_level && lv < v.base_level + n && (!v.set_layers || (sl >= v.min_layer && sl < v.min_layer + (v.nlayers > 0 ? v.nlayers : 1)));
}
static int defer_prog_hits(const prog_t *pg, uint64_t tex[6][128], uint64_t img[6][32], const p_set_rt *r, int color_only, int cover) {
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) return 1;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage < 0 || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h = 0;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 ? tex[sh->stage][b->reg] : 0;
            else continue;
            if (!h) continue;
            for (int k = 0; k < (color_only ? r->n : 9); k++) if ((color_only ? r->color[k] : (k < 8 ? r->color[k] : r->depth)) && defer_handle_sub(h, r, k, cover)) return 1;
        }
    }
    return 0;
}
static void defer_collect_reads(defer_t *d, const prog_t *pg, uint64_t tex[6][128], uint64_t img[6][32]) {
    __unsafe_unretained NvProg *pr = pg ? prog_get(pg) : nil; if (!pr) { d->nreads = 513; return; }
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage < 0 || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h = 0;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? img[sh->stage][b->reg] : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 ? tex[sh->stage][b->reg] : 0;
            else continue;
            if (!h) continue; int seen = 0; for (int j = 0; j < d->nreads; j++) seen |= d->p_reads[j] == h;
            if (!seen && d->nreads < 512) d->p_reads[d->nreads++] = h;
        }
    }
}
static int defer_rt_hits_reads(const p_set_rt *r, int idx) {
    if (idx < 0 || idx >= r->n || !r->color[idx]) return 0;
    for (int i = 0; i < g_defer.nreads; i++) if (defer_handle_sub(g_defer.p_reads[i], r, idx, 1)) return 1;
    return 0;
}
static int defer_q_superset(const p_set_rt *q) {
    memset(g_defer.lmap, 0xff, sizeof g_defer.lmap);
    for (int i = 0; i < g_defer.p_rt.n; i++) if (g_defer.p_rt.color[i]) { int found = 0;
        // 下标须相同：普通片元函数的 color(n) 输出写到物理第 n 个附件，lmap 只改写掩码 / 混合、搬不动输出（能搬的只有带输出映射常量的 _bf 版本）。
        // 下标不同时推迟会把 P 的输出写进 Q 的别的附件。
        // The indices must be the same: a normal fragment function's color(n) output is written to physical attachment n, and lmap only changes the write mask / blend and cannot move the output (only the _bf version with output mapping constants can).
        // With different indices, deferral would write P's output into a different attachment of Q.
        if (i < q->n && q->color[i] && sub_eq(&g_defer.p_rt, i, q, i)) { g_defer.lmap[i] = (uint8_t)i; found = 1; }
        if (!found) return 0;
    }
    return 1;
}
static int defer_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_defer.n >= 64) { g_defer.reason = 20; return 0; }
    defer_snap *z = &g_defer.snap[g_defer.n++]; z->idx = di != NULL; if (dw) z->dw = *dw; if (di) z->di = *di;
    z->sc = R.sc; z->has_sc = R.has_sc; z->S_ = malloc(sizeof S); memcpy(z->S_, &S, sizeof S); for (int k = 0; k < 8; k++) z->pm[k] = (uint8_t)k; g_defer_draws++; return 1;
}
static void defer_release(void) { defer_release_one(&g_defer); }
static void defer_env(void);
static void defer_reset(void) { defer_clear_all(); g_defer_replaying = 0; defer_env(); }
static void defer_apply_p_clear(void) {
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    for (int j = 0; j < g_defer.q_rt.n; j++) { int i = g_defer.lmap[j]; if (i >= 0 && g_defer.clear_c[i]) { R.clear_c[j] = 1; R.cc[j] = g_defer.cc[i]; } }
    if (g_defer.clear_d) { R.clear_d = 1; R.cd = g_defer.cd; } if (g_defer.clear_s) { R.clear_s = 1; R.cs = g_defer.cs; }
}
static int defer_begin_q(const uint8_t *op) {
    int found = 0, first = -1;
    for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && !g_defer_slots[i].at_q && g_defer_slots[i].q_op == op) { found = 1; if (first < 0) first = i; }
    if (!found) return 0;
    rp_flush();
    g_defer_cur = &g_defer_slots[first]; R.rt = g_defer.q_rt; R.has_rt = 1;
    for (int i = 0; i < 4 && !R.rt.depth; i++) { const defer_t *d = &g_defer_slots[i]; if (d->active && !d->at_q && d->q_op == op && d->q_rt.depth) { R.rt.depth = d->q_rt.depth; R.rt.dview = d->q_rt.dview; R.rt.has_dview = d->q_rt.has_dview; } }   // 任一组需要的深度模板都挂上（同 Q 各组深度已核相容） | Attach the depth-stencil needed by any group (the depths of groups with the same Q have been checked compatible)
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    for (int i = 0; i < 4; i++) { defer_t *d = &g_defer_slots[i]; if (!d->active || d->at_q || d->q_op != op) continue;
        for (int j = 0; j < d->q_rt.n; j++) { int k = d->lmap[j]; if (k >= 0 && d->clear_c[k]) { R.clear_c[j] = 1; R.cc[j] = d->cc[k]; } }
        if (d->clear_d) { R.clear_d = 1; R.cd = d->cd; } if (d->clear_s) { R.clear_s = 1; R.cs = d->cs; }
        d->at_q = 1; g_defer_natq++;
    }
    return 1;
}
static void defer_replay(void) {
    if (g_defer_replaying) return;
    int any = 0; for (int i = 0; i < 4; i++) if (g_defer_slots[i].active && g_defer_slots[i].at_q) any = 1;
    if (!any) return;
    typeof(S) save; memcpy(&save, &S, sizeof S); p_scissor sc = R.sc; int has_sc = R.has_sc; int vis = g_vis_counting;
    const uint8_t *en0 = g_exec_next; g_defer_replaying = 1; g_exec_next = NULL;
    for (int gi, after = 0; (gi = defer_next(1, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born;   // 按创建先后：各组 P 区间不交叠且先后排列，组序即原绘制顺序 | In creation order: the groups' P ranges do not overlap and are arranged in sequence, so the group order is the original draw order
        g_defer_cur = d; if (d->reset_between) g_vis_counting = 0;
        { static int once; if (!once++) MLOG("推迟合并通道：首例 通道 %llu，组 %d 回放绘制 %d 次", g_stat_pass, gi, d->n); }   // 多组时首个回放前可能已建多组，不能按 g_defer_n == 1 判 | With multiple groups, several groups may already have been created before the first replay, so it cannot be judged by g_defer_n == 1
        for (int i = 0; i < d->n; i++) { defer_snap *z = &d->snap[i]; static uint8_t lm[8]; for (int j = 0; j < 8; j++) lm[j] = d->lmap[j] == 0xff || d->lmap[j] >= 8 ? 0xff : z->pm[d->lmap[j]];
            memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; g_pso_lmap = lm; draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
        defer_release_one(d);
    }
    g_vis_counting = vis; memcpy(&S, &save, sizeof S); args_dirty_all(); R.sc = sc; R.has_sc = has_sc; g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = g_dss_dirty = 1; g_exec_next = en0; g_defer_replaying = 0; g_defer_cur = &g_defer_slots[0];
}
static void defer_fallback(void) {
    if (g_defer_replaying || !defer_has_active()) return;
    g_defer_fallbacks++; MLOG("【红】推迟合并通道：提交结束仍有未回放组，按各组 P 原目标兜底");
    if (g_enc) { enc_r_end(g_enc); g_enc = nil; }
    typeof(S) save; memcpy(&save, &S, sizeof S); p_set_rt rt = R.rt; g_defer_replaying = 1;
    for (int gi, after = 0; (gi = defer_next(0, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born; g_defer_cur = d; R.rt = d->p_rt; R.has_rt = 1; memcpy(R.clear_c, d->clear_c, 8); memcpy(R.cc, d->cc, sizeof R.cc);
        R.clear_d = d->clear_d; R.clear_s = d->clear_s; R.cd = d->cd; R.cs = d->cs;
        for (int i = 0; i < d->n; i++) { defer_snap *z = &d->snap[i]; memcpy(&S, z->S_, sizeof S); args_dirty_all(); R.sc = z->sc; R.has_sc = z->has_sc; g_pso_dirty = g_dss_dirty = 1; g_pso_lmap = z->pm; draw(z->idx ? NULL : &z->dw, z->idx ? &z->di : NULL); }
        rp_end(); defer_release_one(d);
    }
    memcpy(&S, &save, sizeof S); args_dirty_all(); R.rt = rt; g_defer_replaying = 0; g_defer_cur = &g_defer_slots[0];
}
static const uint8_t *defer_cont(int *ci) {   // 列表结束后的下一段（同一次提交内）：先调用方返回栈，再本次提交余下的句柄；没有了返回 NULL | The next segment after a list ends (within the same submit): first the caller return stack, then the remaining handles of this submit; returns NULL when there are none left
    for (;;) { int k = (*ci)++;
        if (k < g_ret_n) { if (g_ret_n > 16) return NULL; return g_ret[g_ret_n - 1 - k]; }
        k -= g_ret_n; if (k < g_nrest) return (const uint8_t *)g_rest[k];
        return NULL; }
}
// 推迟预筛（写回前瞻索引二分，零扫描）：P 每个颜色子资源在本次绘制之后、首个「作附件 / 被清」事件必须是「另一次 SetRenderTargets 下作附件、
// 且那次目标覆盖 P 全部颜色」；先被清 / 本次提交再没作附件 → 不可能推迟，直接返回。读事件不带级别，交给完整前瞻按级别核。
// Deferral pre-screen (binary search on the store lookahead index, zero scanning): for every color subresource of P, the first "used as attachment / cleared" event after this draw must be "used as attachment under a different SetRenderTargets,
// whose targets cover all of P's colors"; cleared first / never used as attachment again in this submit -> deferral is impossible, return directly. Read events carry no level; handed to the full lookahead to check by level.
static int defer_pre(void) {
    if (!g_sdx_ok || !g_exec_cur || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 0;
    uint32_t P = g_exec_seq - 1;
    for (int k = 0; k < R.rt.n; k++) { if (!R.rt.color[k]) continue;
        uint64_t ser; int lv, sl; rt_sub(&R.rt, k, &ser, &lv, &sl);
        int b = sdx_bucket(ser, 0); if (b < 0) return 0;
        uint32_t lo = g_sdx_bst[b], end = g_sdx_bst[b + 1], hi = end;
        while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_sdx[m].seq <= P) lo = m + 1; else hi = m; }
        // P 本通道的 SetRenderTargets 序号：原先取本次绘制的作附件事件；作附件事件按通道去重后改按 SET_RT 序号表取 P 之前最后一个（同一个值）
        // SetRenderTargets sequence number of P's own pass: previously taken from this draw's as-attachment event; after as-attachment events were deduplicated per pass, it is taken from the SET_RT sequence table as the last one before P (same value)
        uint32_t prt = SDX_NONE;
        if (!g_sdx_attdedup) prt = lo > g_sdx_bst[b] && g_sdx[lo - 1].seq == P ? g_sdx[lo - 1].rtseq : SDX_NONE;
        else { uint32_t a0 = 0, a1 = g_rtx_n; while (a0 < a1) { uint32_t m = (a0 + a1) / 2; if (g_rtx_seq[m] <= P) a0 = m + 1; else a1 = m; } if (a0 > 0) prt = g_rtx_seq[a0 - 1]; }
        int ok = 0, same = 0; uint32_t walk = 0;
        for (uint32_t j = lo; j < end; j++) { const sdx_ev *e = &g_sdx[j];
            walk += 1u + e->rep; if (walk > 64) return 0;   // 事件太多（被大量采样的贴图）：不当候选，保守且免长走（并掉的同通道作附件事件照原样计步） | Too many events (heavily sampled texture): not a candidate; conservative and avoids long walks (merged same-pass as-attachment events still count steps as before)
            if (g_hoist_n && hoist_skipped(e->op)) continue;
            if (e->kind == SDX_READ || e->kind == SDX_NEED) continue;   // 读事件不带级别（读别的级不冲突），交给完整前瞻按级别核 | Read events carry no level (reading another level is not a conflict); handed to the full lookahead to check by level
            if (e->plane != 0 || e->lv != lv || e->sl != sl) continue;
            if (e->kind == SDX_CLEAR) return 0;
            if (e->rtseq == SDX_NONE || e->rtseq >= g_sdx_nop) return 0;
            const uint8_t *rop = g_sdx_op[e->rtseq]; const p_set_rt *r = (const p_set_rt *)(rop + 8);
            if (rt_same(r, &R.rt)) { if (e->rtseq != prt) same = 1; continue; }   // 同目标：紧接的是 P 续段、隔了别的通道的是 Q；预筛不分，走完仍无别的候选时交给完整前瞻 | Same target: one immediately following is a continuation segment of P, one separated by other render passes is Q; the pre-screen does not distinguish them, and if there is still no other candidate at the end it is handed to the full lookahead
            for (int i = 0; i < R.rt.n; i++) { if (!R.rt.color[i]) continue; int f = 0;
                for (int q = 0; q < r->n && !f; q++) if (r->color[q] && sub_eq(&R.rt, i, r, q)) f = 1;
                if (!f) return 0; }
            ok = 1; break; }
        if (!ok && !same) return 0; }
    return 1;
}
static void defer_try(const p_draw *dw, const p_draw_idx *di) {
    if (!defer_on() || defer_has_in_p() || g_defer_replaying || g_fz.active || g_fz_checking || !g_exec_next || !R.rt.n) return;
    int slot = defer_slot_free(); if (slot < 0) return;
    g_defer_cur = &g_defer_slots[slot];
    int ncol = 0; for (int i = 0; i < R.rt.n; i++) ncol += R.rt.color[i] != NULL; if (!ncol) return;   // 只挂深度模板的通道（阴影图等）：Q 的定义是覆盖 P 的颜色，无颜色的 P 不可能推迟——原先预筛对它空转放行、完整前瞻白扫几十次绘制 | Render passes with only depth-stencil attached (shadow maps etc.): Q is defined as covering P's colors, so a P without color can never be deferred -- previously the pre-screen let it through idly and the full lookahead scanned dozens of draws for nothing
    int has_clear = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) has_clear |= R.clear_c[i]; if (!has_clear) return;
    if (!defer_pre()) return;
    memset(&g_defer, 0, sizeof g_defer); g_defer.p_rt = R.rt; memcpy(g_defer.clear_c, R.clear_c, 8); memcpy(g_defer.cc, R.cc, sizeof R.cc);
    g_defer.clear_d = R.clear_d; g_defer.clear_s = R.clear_s; g_defer.cd = R.cd; g_defer.cs = R.cs;
    uint64_t tex[6][128], img[6][32]; memcpy(tex, S.tex, sizeof tex); memcpy(img, S.img, sizeof img); const prog_t *pg = S.prog; ds_st ds = S.ds;
    const uint8_t *p = g_exec_next, *p_end = NULL, *qop = NULL; const p_set_rt *q = NULL; p_set_rt scan_rt = R.rt; int srt_hit = -1, ci = 0, draws = 1, p_has_draw = 1; defer_collect_reads(&g_defer, pg, tex, img);   // 当前这次绘制就是 P 的第一次 | The current draw is P's first
    for (int guard = 0; guard < 200000; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (g_hoist_n && hoist_skipped(p)) { p += h.size; continue; }
        switch (h.op) {
            case OP_END: { const uint8_t *nx = defer_cont(&ci); if (!nx) { g_defer.reason = 1; goto fail; } p = nx; continue; }
            case OP_CALL: g_defer.reason = 2; goto fail;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_BIND_PROGRAM: pg = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_TEX: case OP_BIND_IMG: { const p_bind_handles *b = a; int lim = h.op == OP_BIND_IMG ? 32 : 128;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (h.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } break; }
            case OP_BIND_STATE: { const p_bind_state *b = a; if (b->kind == 5) memcpy(&ds, b->bytes, sizeof ds); break; }
            case OP_DRAW: case OP_DRAW_IDX:
                if (defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { /* 建议在这里识别需要输出映射但缺少 _bf 后调用 bf_want_note。 | Suggestion: here, after identifying that an output mapping is needed but _bf is missing, call bf_want_note. */ g_defer.reason = 7; goto fail; }
                if (p_end) {   // 中间区间的绘制：不得写 P 读的贴图、不得写 P 的深度模板（P 写模板时也不得测模板） | Draws in the intermediate range: must not write textures P reads, must not write P's depth-stencil (and must not test stencil when P writes stencil)
                    if (srt_hit < 0) { srt_hit = 0; for (int k = 0; k < scan_rt.n && !srt_hit; k++) if (scan_rt.color[k] && defer_rt_hits_reads(&scan_rt, k)) srt_hit = 1; }   // 附件只在 SetRenderTargets 变：每段算一次 | Attachments change only at SetRenderTargets: computed once per segment
                    if (srt_hit) { g_defer.reason = 12; goto fail; }
                    if (scan_rt.depth && g_defer.p_rt.depth && scan_rt.depth->serial == g_defer.p_rt.depth->serial && (ds.dw || (g_defer.p_st && ds.st))) { g_defer.reason = 13; goto fail; }
                    break; }
                draws++; if (draws > 64) { g_defer.reason = 20; goto fail; } p_has_draw = 1; defer_collect_reads(&g_defer, pg, tex, img); g_defer.p_dw |= ds.dw; g_defer.p_st |= ds.st && ((S.st_wmask[0] | S.st_wmask[1]) != 0) && (ds.fail_f != NVN_STENCIL_OP_KEEP || ds.zfail_f != NVN_STENCIL_OP_KEEP || ds.zpass_f != NVN_STENCIL_OP_KEEP || ds.fail_b != NVN_STENCIL_OP_KEEP || ds.zfail_b != NVN_STENCIL_OP_KEEP || ds.zpass_b != NVN_STENCIL_OP_KEEP); if (g_defer.p_dw) { g_defer.reason = 8; goto fail; } break;
            case OP_RESET_COUNTER: g_defer.reset_between = 1; break;
            case OP_COPY_T2T: { const p_copy_t2t *c = a;   // 贴图拷贝：两端都不碰 P 的附件、目标不是 P 采样的贴图才放行 | Texture copy: allowed only if neither end touches P's attachments and the destination is not a texture P samples
                uint64_t ss = c->src ? c->src->serial : 0, ds_ = c->dst ? c->dst->serial : 0; int bad = !ss || !ds_;
                for (int k = 0; k < g_defer.p_rt.n && !bad; k++) if (g_defer.p_rt.color[k] && (g_defer.p_rt.color[k]->serial == ss || g_defer.p_rt.color[k]->serial == ds_)) bad = 1;
                if (g_defer.p_rt.depth && (g_defer.p_rt.depth->serial == ss || g_defer.p_rt.depth->serial == ds_)) bad = 1;
                for (int k = 0; k < g_defer.nreads && !bad; k++) if (h_ser(g_defer.p_reads[k]) == ds_) bad = 1;
                if (g_defer.nreads > 512) bad = 1;
                if (bad) { g_defer.reason = 3; g_defer.reason_op = h.op; goto fail; } break; }
            case OP_REPORT_COUNTER: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: g_defer.reason = 3; g_defer.reason_op = h.op; goto fail;
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (!p_end) { g_defer.reason = 16; goto fail; }   // P 续段里的清除：不推迟 | A clear inside P's continuation segment: do not defer
                if (c->index >= 0 && c->index < scan_rt.n && (defer_rt_hits_reads(&scan_rt, c->index) || (scan_rt.color[c->index] && defer_rt_hits_reads(&g_defer.p_rt, c->index)))) { g_defer.reason = 5; goto fail; } break; }
            case OP_CLEAR_DS: if (!p_end) { g_defer.reason = 16; goto fail; } break;
            case OP_SET_RT: { const p_set_rt *r = a;
                if (!p_end && rt_same(r, &g_defer.p_rt)) break;   // 同目标再设一次（游戏常连发）：仍是 P 的续段，绘制一并推迟 | The same target set once more (the game often sends it repeatedly): still a continuation segment of P, and its draws are deferred together
                if (!p_end) p_end = p; int shared = 0; for (int i = 0; i < g_defer.p_rt.n; i++) for (int j = 0; j < r->n; j++) if (g_defer.p_rt.color[i] && r->color[j] && sub_eq(&g_defer.p_rt, i, r, j)) shared = 1;
                if (shared) {   // 空通道（设完目标不画不清就又换目标）不碰任何附件：既不当 Q 也不算冲突 | An empty render pass (targets set, then changed again without drawing or clearing) touches no attachment: it is neither taken as Q nor counted as a conflict
                    const uint8_t *e = p + h.size; int ec = ci, empty = 0;
                    for (int g2 = 0; g2 < 200000; g2++) { op_hdr he; memcpy(&he, e, sizeof he);
                        if (he.op == OP_JUMP) { memcpy(&e, e + 8, 8); continue; }
                        if (he.op == OP_END) { const uint8_t *nx = defer_cont(&ec); if (!nx) break; e = nx; continue; }
                        if (he.op == OP_SET_RT) { empty = 1; break; }
                        if (he.op == OP_DRAW || he.op == OP_DRAW_IDX || he.op == OP_CLEAR_COLOR || he.op == OP_CLEAR_DS || he.op == OP_CALL || he.op == OP_DISPATCH || he.op == OP_CLEAR_TEX || he.op == OP_COPY_T2T || he.op == OP_COPY_B2T || he.op == OP_COPY_T2B) break;
                        e += he.size; }
                    if (empty) break; }
                if (!shared) break; if (!p_has_draw || !defer_q_superset(r)) { g_defer.reason = 6; goto fail; } q = r; qop = p; goto q_found; }
            default: break;
        }
        if (h.op == OP_SET_RT) { scan_rt = *(const p_set_rt *)a; srt_hit = -1; }
        p += h.size;
    }
fail:
    if (g_defer.reason && passdump()) PD("G %llu 不成立 reason=%d op=%d", g_stat_pass + 1, g_defer.reason, g_defer.reason_op); memset(&g_defer, 0, sizeof g_defer); return;
q_found:
    p = qop; op_hdr qh; memcpy(&qh, p, sizeof qh); p += qh.size; int q_body = 0, q_clears = 0;   // 绑定跟踪接着主扫描的状态（不回到 P 开头的 S） | Binding tracking continues from the main scan's state (does not go back to S at the start of P)
    for (int guard = 0; guard < 200000; guard++) { op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8; if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        // Q 整段（到下一个 SetRenderTargets / 列表结束）：任何绘制都不得按覆盖 P 级别的视图采样 P 的颜色附件——推迟后 P 的结果还在 tile 里没落主存，采样会读到旧内容
        // The whole Q segment (up to the next SetRenderTargets / end of list): no draw may sample P's color attachments through a view covering P's level -- after deferral P's result is still in the tile and has not landed in main memory, so sampling would read old content
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) { if (defer_prog_hits(pg, tex, img, &g_defer.p_rt, 1, 1)) { g_defer.reason = 14; goto fail; } q_body = 1; p += h.size; continue; }
        if (q_body && (h.op == OP_SET_RT || h.op == OP_END)) break;
        if (!q_body && h.op == OP_END) { const uint8_t *nx = defer_cont(&ci); if (!nx) { g_defer.reason = 9; goto fail; } p = nx; continue; }
        if (h.op == OP_CALL) { g_defer.reason = 9; g_defer.reason_op = h.op; goto fail; }
        // Q 的 SetRenderTargets 之后、首次绘制与任何清除之前又来一次 SetRenderTargets（游戏重复设目标，前一次是空通道）：新目标仍覆盖 P 就以它为 Q，否则不推迟
        // After Q's SetRenderTargets and before its first draw and any clear, another SetRenderTargets arrives (the game sets targets repeatedly, the previous one being an empty render pass): if the new targets still cover P take it as Q, otherwise do not defer
        if (!q_body && h.op == OP_SET_RT && !q_clears) { const p_set_rt *r2 = a; if (!defer_q_superset(r2)) { g_defer.reason = 15; goto fail; } q = r2; qop = p; p += h.size; continue; }
        if (h.op == OP_CLEAR_COLOR || h.op == OP_CLEAR_DS) q_clears = 1;
        if (!q_body && (h.op == OP_END || h.op == OP_SET_RT || h.op == OP_UPDATE_UB || h.op == OP_COPY_B2B || h.op == OP_DISPATCH)) { g_defer.reason = 9; g_defer.reason_op = h.op; goto fail; }
        if (h.op == OP_BIND_PROGRAM) pg = ((const p_bind_program *)a)->prog; else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG) { const p_bind_handles *b = a; int lim = h.op == OP_BIND_IMG ? 32 : 128; if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (h.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } }
        else if (h.op == OP_CLEAR_COLOR) { const p_clear_color *c = a; if (c->index >= 0 && c->index < q->n && q->color[c->index]) for (int i = 0; i < g_defer.p_rt.n; i++) if (g_defer.p_rt.color[i] && sub_eq(q, c->index, &g_defer.p_rt, i)) { g_defer.reason = 10; goto fail; } }
        p += h.size;
    }
    if (!q || !defer_q_superset(q) || g_defer.n >= 64) { g_defer.reason = 11; goto fail; }
    for (int i = 0; i < 4; i++) if (&g_defer_slots[i] != &g_defer && g_defer_slots[i].active && g_defer_slots[i].q_op == qop && !defer_same_q_depth(&g_defer_slots[i], q)) { g_defer.reason = 17; goto fail; }
    g_defer.q_rt = *q; if (!q->depth && g_defer.p_rt.depth) { g_defer.q_rt.depth = g_defer.p_rt.depth; g_defer.q_rt.dview = g_defer.p_rt.dview; g_defer.q_rt.has_dview = g_defer.p_rt.has_dview; } { int na = 0; for (int i = 0; i < 4; i++) { const defer_t *d = &g_defer_slots[i]; if (d != &g_defer && d->active && d->at_q) na += d->n; }
      if (na + 1 > 64) { g_defer.reason = 20; goto fail; } }
    g_defer.q_op = qop; g_defer.p_end_op = p_end; g_defer.in_p = 1; g_defer.active = 1; g_defer_nact++; g_defer_pend = p_end; g_defer_n++; g_defer.born = g_defer_n;
    // 链式：本通道正是上游组的 Q（它们 at_q、等本通道开通道时回放），本通道又整体推迟——上游组按创建先后并进新组：
    // 上游组的目标空间 = 本通道目标 = 新组 P 目标，快照的映射复合为 pm' = 上游 lmap ∘ pm；上游的清除已在 R 里，下面随新组带走
    // Chained: this render pass is exactly the Q of upstream groups (they are at_q, waiting to be replayed when this render pass opens), and this render pass is itself deferred as a whole -- the upstream groups are merged into the new group in creation order:
    // the target space of an upstream group = this render pass's targets = the new group's P targets, and the snapshot's mapping is composed as pm' = upstream lmap o pm; the upstream clears are already in R and are carried along with the new group below
    for (int gi, after = 0; (gi = defer_next(1, after)) >= 0;) { defer_t *d = &g_defer_slots[gi]; after = d->born; if (d == &g_defer) continue;
        for (int k = 0; k < 8; k++) if (d->lmap[k] != 0xff && d->lmap[k] != k) { g_defer.reason = 18; goto fail; }   // 只并恒等映射的上游组（见 defer_q_superset） | Only merge in upstream groups with an identity mapping (see defer_q_superset)
        for (int i = 0; i < d->n; i++) { defer_snap z = d->snap[i]; uint8_t pm[8]; for (int k = 0; k < 8; k++) pm[k] = d->lmap[k] == 0xff || d->lmap[k] >= 8 ? 0xff : z.pm[d->lmap[k]];
            memcpy(z.pm, pm, 8); g_defer.snap[g_defer.n++] = z; }
        g_defer.reset_between |= d->reset_between; d->n = 0; defer_release_one(d); } if (!defer_take(dw, di)) { defer_release_one(&g_defer); return; }
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0; PD("G %llu 成立 组 %d P→Q draws=%d", g_stat_pass + 1, slot, draws);
}
// 合并的必要条件预筛（按提交索引二分，失败零扫描）：合并要求「后一个通道（下一个目标不同、未被提前的 SET_RT）的首次绘制
// 采样前一个通道的某张颜色附件」，即那次绘制的序号上有该附件的 SDX_READ 事件。不满足就必然不合并，省掉下面逐条扫描；
// 满足才走原规划（决定与不预筛时完全相同）。索引不可用时照旧规划。
// Pre-screen of the necessary condition for merging (binary search on the submit index, zero scanning on failure): merging requires that "the first draw of the later pass (the next SET_RT with a different target that has not been hoisted)
// samples some color attachment of the earlier pass", i.e. there is an SDX_READ event for that attachment at that draw's sequence number. If not satisfied there is certainly no merge, saving the per-operation scan below;
// only if satisfied does it go through the original planning (the decision is exactly the same as without the pre-screen). When the index is unavailable, plan as before.
static int fz_prefilter(void) {
    if (!g_sdx_ok || !g_exec_seq || g_sdx_op[g_exec_seq - 1] != g_exec_cur) return 1;
    uint32_t P = g_exec_seq - 1, lo = 0, hi = g_rtx_n;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (g_rtx_seq[m] <= P) lo = m + 1; else hi = m; }
    for (; lo < g_rtx_n; lo++) { const uint8_t *op = g_sdx_op[g_rtx_seq[lo]]; if (g_hoist_n && hoist_skipped(op)) continue; if (!rt_same(&R.rt, (const p_set_rt *)(op + 8))) break; }
    if (lo >= g_rtx_n) return 0;
    uint32_t r2 = g_rtx_seq[lo], a = 0, b = g_drx_n;
    while (a < b) { uint32_t m = (a + b) / 2; if (g_drx_seq[m] <= r2) a = m + 1; else b = m; }
    if (a >= g_drx_n) return 0;
    uint32_t fd = g_drx_seq[a];
    for (uint32_t k = lo + 1; k < g_rtx_n && g_rtx_seq[k] < fd; k++) if (!(g_hoist_n && hoist_skipped(g_sdx_op[g_rtx_seq[k]]))) return 0;   // 首绘前又换目标：原规划第二步也放弃 | The target changes again before the first draw: step two of the original planning gives up as well
    for (int j = 0; j < R.rt.n && j < 8; j++) { if (!R.rt.color[j]) continue; int bk = sdx_bucket(R.rt.color[j]->serial, 0); if (bk < 0) continue;
        uint32_t e0 = g_sdx_bst[bk], e1 = g_sdx_bst[bk + 1];
        while (e0 < e1) { uint32_t m = (e0 + e1) / 2; if (g_sdx[m].seq < fd) e0 = m + 1; else e1 = m; }
        for (; e0 < g_sdx_bst[bk + 1] && g_sdx[e0].seq == fd; e0++) if (g_sdx[e0].kind == SDX_READ) return 1; }
    return 0;
}
static void fuse_try(void) {
    if (g_fz.active || !g_exec_next || g_hoist_n + 12 > HOIST_MAX || !R.rt.n) return;
    if (!fz_prefilter()) return;
    // 一：前一个通道到下一次 SetRenderTargets，只许绘制与状态
    // One: from the earlier pass to the next SetRenderTargets, only draws and state are allowed
    const uint8_t *p = NULL, *r2op = NULL, *reuse[8]; int nreuse = 0; const p_set_rt *r2 = NULL; p_scissor sc = R.sc; int has_sc = R.has_sc;
    fz_cursor cur = {.p = g_exec_next}; op_hdr h; const void *a;
    for (int guard = 0; guard < 200000 && !r2; guard++) {
        if (!fz_next(&cur, &p, &h, &a)) return;
        switch (h.op) {
            case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH:
            case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_UPDATE_UB: case OP_COPY_B2B: return;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_SET_RT: {
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                if (rt_same(&R.rt, a)) {
                    if (g_enc_mode == 1 || pend || !R.has_rt || !tbdr_on() || nreuse >= 8) return;
                    reuse[nreuse++] = p; continue;
                }
                r2 = a; r2op = p; continue;
            }
            default: break;
        }
    }
    if (!r2 || rt_same(&R.rt, r2)) return;
    // 二：后一个通道开头到首次绘制，只许清除（不清共享附件）与状态
    // Two: from the start of the later pass to its first draw, only clears (not clearing shared attachments) and state are allowed
    const uint8_t *cl[8]; int ncl = 0; uint8_t clr[8] = {0}; MTLClearColor cc[8]; int cd_f = 0, cs_f = 0; double cdv = 0; uint32_t csv = 0;
    if (!fz_next(&cur, &p, &h, &a)) return;   // 接着第一步的游标（已越过后一个通道的 SET_RT，保留跨句柄 / 返回栈位置） | Continue from the cursor of step one (already past the later pass's SET_RT, keeping the cross-handle / return stack position)
    for (int guard = 0; guard < 200000; guard++) {
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) break;
        switch (h.op) {
            case OP_COPY_T2T: case OP_COPY_B2T: case OP_COPY_T2B: case OP_CLEAR_TEX: case OP_DISPATCH: case OP_SET_RT: case OP_UPDATE_UB: case OP_COPY_B2B: return;
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (ncl >= 8 || c->index < 0 || c->index >= r2->n || !r2->color[c->index] || c->mask != 0xf) return;
                const tex_t *t = r2->color[c->index];
                if (has_sc && !(sc.x <= 0 && sc.y <= 0 && sc.x + sc.w >= (int)t->d.width && sc.y + sc.h >= (int)t->d.height)) return;
                clr[c->index] = 1; cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]); cl[ncl++] = p; break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a; if (ncl >= 8 || !r2->depth) return;
                if (c->depth_mask) { cd_f = 1; cdv = c->depth; } if (c->stencil_mask) { cs_f = 1; csv = (uint32_t)c->stencil; } cl[ncl++] = p; break; }
            default: break;
        }
        if (!fz_next(&cur, &p, &h, &a)) return;
    }
    // 三：附件并集（前一个通道的附件保持原序号，后一个通道独有的接在后面）、尺寸一致、深度相容
    // Three: union of attachments (the earlier pass's attachments keep their original indices, those unique to the later pass are appended), consistent sizes, compatible depth
    p_set_rt U = R.rt; int n = R.rt.n; int8_t p2map[8]; memset(p2map, -1, sizeof p2map);
    for (int k = 0; k < r2->n && k < 8; k++) { if (!r2->color[k]) continue;
        int j = -1; for (int m = 0; m < R.rt.n; m++) if (R.rt.color[m] && sub_eq(&R.rt, m, r2, k)) { j = m; break; }
        if (j < 0) { if (n >= 8) return; j = n++; U.color[j] = r2->color[k]; U.cview[j] = r2->cview[k]; U.has_cview[j] = r2->has_cview[k]; }
        else if (clr[k]) return;   // 后一个通道开头要清前一个通道正在画的附件：合并后无处安放这个清除 | The start of the later pass wants to clear an attachment the earlier pass is drawing to: after merging this clear has nowhere to go
        p2map[k] = (int8_t)j; }
    U.n = n;
    { uint64_t s0; int l0, x0; rt_sub(&U, 0, &s0, &l0, &x0); if (!U.color[0]) return;
      uint32_t w0 = MAX(U.color[0]->d.width >> l0, 1u), h0 = MAX(U.color[0]->d.height >> l0, 1u);
      for (int j = 1; j < n; j++) { uint64_t s_; int l_, x_; rt_sub(&U, j, &s_, &l_, &x_); if (!U.color[j] || MAX(U.color[j]->d.width >> l_, 1u) != w0 || MAX(U.color[j]->d.height >> l_, 1u) != h0) return; } }
    int depth_from_r2 = 0;
    if (R.rt.depth && r2->depth && !sub_eq(&R.rt, 8, r2, 8)) return;
    if (!R.rt.depth && r2->depth) { U.depth = r2->depth; U.dview = r2->dview; U.has_dview = r2->has_dview; depth_from_r2 = 1; }
    if ((cd_f || cs_f) && !depth_from_r2) return;   // 深度在前一个通道里就在用：后一个通道开头的深度清除无处安放 | The depth is already in use in the earlier pass: the depth clear at the start of the later pass has nowhere to go
    // 四：前一个通道的绘制不得采样后一个通道带来的附件；深度来自后一个通道时不得开深度模板测试
    // Four: draws of the earlier pass must not sample attachments brought in by the later pass; when the depth comes from the later pass, depth-stencil testing must not be enabled
    static uint64_t tex[6][128], img[6][32]; memcpy(tex, S.tex, sizeof tex); memcpy(img, S.img, sizeof img);
    const prog_t *pg = S.prog; ds_st ds = S.ds;
    cur = (fz_cursor){.p = g_exec_next};
    for (;;) {   // 第一次循环核开通道的这次绘制（当前状态），之后按区间里的绘制逐个核 | The first loop iteration checks the draw that opens the render pass (current state), then checks the draws in the range one by one
        if (depth_from_r2 && (ds.dt || ds.dw || ds.st)) return;
        for (int j = R.rt.n; j < n; j++) if (prog_samples_ser(pg, tex, img, U.color[j]->serial)) return;
        if (depth_from_r2 && prog_samples_ser(pg, tex, img, U.depth->serial)) return;
        for (;;) {
            const uint8_t *q; op_hdr hq; const void *aq;
            if (!fz_next(&cur, &q, &hq, &aq)) return;
            if (q == r2op) goto planned;
            if (hq.op == OP_BIND_PROGRAM) pg = ((const p_bind_program *)aq)->prog;
            else if (hq.op == OP_BIND_STATE) { const p_bind_state *b = aq; if (b->kind == 5) memcpy(&ds, b->bytes, sizeof ds); }
            else if (hq.op == OP_BIND_TEX || hq.op == OP_BIND_IMG) { const p_bind_handles *b = aq; int lim = hq.op == OP_BIND_IMG ? 32 : 128;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (hq.op == OP_BIND_IMG) img[b->stage][b->first + k] = b->h[k]; else tex[b->stage][b->first + k] = b->h[k]; } }
            else if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) break;
        }
    }
planned:
    {   // 五：预筛后一个通道的第一次绘制（静态条件），免得白挂并集附件：跟踪到那次绘制时的程序与贴图绑定 | Five: pre-screen the first draw of the later pass (static conditions), to avoid attaching the union of attachments for nothing: track the program and texture bindings up to that draw
        const prog_t *pg2 = S.prog; static uint64_t t2[6][128], i2[6][32]; memcpy(t2, S.tex, sizeof t2); memcpy(i2, S.img, sizeof i2);
        cur = (fz_cursor){.p = g_exec_next};
        for (;;) { const uint8_t *q; op_hdr hq; const void *aq; if (!fz_next(&cur, &q, &hq, &aq)) return; if (q == p) break;
            if (hq.op == OP_BIND_PROGRAM) pg2 = ((const p_bind_program *)aq)->prog;
            else if (hq.op == OP_BIND_TEX || hq.op == OP_BIND_IMG) { const p_bind_handles *b = aq; int lim = hq.op == OP_BIND_IMG ? 32 : 128;
                if (b->stage >= 0 && b->stage < 6 && b->first >= 0 && b->first + b->n <= lim) for (int k = 0; k < b->n; k++) { if (hq.op == OP_BIND_IMG) i2[b->stage][b->first + k] = b->h[k]; else t2[b->stage][b->first + k] = b->h[k]; } }
        }
        NvProg *pr2 = pg2 ? prog_get(pg2) : nil; NvShader *vs2 = pr2 ? pr2->sh[0] : nil, *fs2 = pr2 ? pr2->sh[1] : nil;
        if (!vs2 || !fs2) return;
        int touches = 0;
        for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr2->sh[st]; if (!sh || sh->stage >= 6) continue;
            for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *bb = &sh->rf.b[i]; uint64_t h;
                if (bb->kind == NVN_RB_IMG) h = bb->reg < 32 ? i2[sh->stage][bb->reg] : 0; else if (bb->kind == NVN_RB_TEX || bb->kind == NVN_RB_TBUF) h = bb->reg < 128 ? t2[sh->stage][bb->reg] : 0; else continue;
                uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
                int j = -1; for (int m = 0; m < R.rt.n; m++) if (R.rt.color[m] && R.rt.color[m]->serial == ser) { j = m; break; }
                if (j < 0) continue;
                if (sh != fs2 || bb->kind != NVN_RB_TEX || (fs2->rf.fc_mixed[bb->reg >> 6] >> (bb->reg & 63) & 1)) return;
                int any = 0;
                for (int c = 0; c < fs2->rf.nfc; c++) if (fs2->rf.fc_reg[c] == bb->reg) { int l = fs2->rf.fc_locn[c];
                    if (l >= 32 || (vs2->rf.uv_attr[l] < 0 && vs2->rf.uv_aff_attr[l] < 0)) return; any = 1; }
                if (!any) return;
                touches = 1; } }
        if (!touches) return;   // 后一个通道不读前一个通道的附件：合并没有收益 | The later pass does not read the earlier pass's attachments: merging has no benefit
        int ident = 1; for (int c = 0; c < 8; c++) if (fs2->rf.color_mask >> c & 1) { if (p2map[c] < 0) return; if (p2map[c] != c) ident = 0; }
        if (!ident && !fs2->rf.nfo) return;
    }
    memset(&g_fz, 0, sizeof g_fz);
    g_fz.active = 1; g_fz.phase = 1; g_fz.n1 = R.rt.n; g_fz.nreuse = nreuse; g_fz.rt1 = R.rt; g_fz.rt2 = *r2; g_fz.rt2_op = r2op; memcpy(g_fz.reuse, reuse, sizeof reuse); memcpy(g_fz.p2map, p2map, 8);
    for (int j = 0; j < 8; j++) g_fz.lmap1[j] = j < R.rt.n ? (uint8_t)j : 0xff;
    fz_probe_cov2(r2, p, p2map, clr, g_fz.n1, n);
    g_fz.skip[g_fz.nskip++] = r2op; for (int i = 0; i < ncl; i++) g_fz.skip[g_fz.nskip++] = cl[i];
    for (int i = 0; i < g_fz.nskip; i++) g_hoist_skip[g_hoist_n++] = g_fz.skip[i];
    R.rt = U;
    memcpy(g_fz.clr2, clr, 8); memcpy(g_fz.cc2, cc, sizeof cc); g_fz.cd2f = (uint8_t)cd_f; g_fz.cs2f = (uint8_t)cs_f; g_fz.cd2 = cdv; g_fz.cs2 = csv;
    for (int k = 0; k < 8; k++) if (clr[k]) { R.clear_c[p2map[k]] = 1; R.cc[p2map[k]] = cc[k]; }
    if (cd_f) { R.clear_d = 1; R.cd = cdv; } if (cs_f) { R.clear_s = 1; R.cs = csv; }
    if (!g_fz_n++) MLOG("可读 tile 通道合并：首例 通道 %llu（物理附件 %d 个）", g_stat_pass + 1, n);
    PD("F %llu 合并下一个通道（可读 tile）：物理附件 %d 个，前一个通道 %d 个", g_stat_pass + 1, n, R.rt.n == n ? g_fz.n1 : g_fz.n1);
}
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap) {   // 可读 tile 版本按常量特化（缓存） | The readable tile version is specialized by constants (cached)
    os_unfair_lock_lock(&g_fz_fn_lock);
    if (!g_bf_fn_cache) { g_bf_fn_cache = [NSMutableDictionary dictionary]; g_bf_lib_cache = [NSMutableDictionary dictionary]; }
    size_t kl = strlen(fs->key); NSString *base = [NSString stringWithFormat:@"%.*s", (int)(kl > 2 ? kl - 2 : kl), fs->key];
    id<MTLLibrary> lib = g_bf_lib_cache[base];
    if (!lib) {
        NSError *e = nil; NSString *path = [NSString stringWithFormat:@"%@/%@_bf.metallib", g_msl_dir, base];
        lib = lib_from_file(path, &e);
        if (!lib) MLOG("【红】可读 tile 版本载入失败 %s：%s", path.UTF8String, e.localizedDescription.UTF8String);
        else g_bf_lib_cache[base] = lib;
    }
    if (!lib) { os_unfair_lock_unlock(&g_fz_fn_lock); return nil; }
    NSMutableString *k = [NSMutableString stringWithFormat:@"%02x", fbmask];
    for (int i = 0; i < fs->rf.nfc; i++) [k appendFormat:@",%d", fx[i]];
    for (int j = 0; j < 8; j++) [k appendFormat:@"/%02x", lmap[j]];
    NSString *cacheKey = [NSString stringWithFormat:@"%@|%@", base, k];
    id<MTLFunction> f = g_bf_fn_cache[cacheKey];
    if (f) { os_unfair_lock_unlock(&g_fz_fn_lock); return f; }
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    for (int i = 0; i < fs->rf.nfc; i++) { int v = fx[i]; [cv setConstantValue:&v type:MTLDataTypeInt atIndex:(NSUInteger)i]; }
    for (int q = 0; q < 8; q++) { bool b = fbmask >> q & 1; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(32 + q)]; }
    for (int j = 0; j < fs->rf.nfo; j++) for (int q = 0; q < 8; q++) { bool b = lmap[q] == fs->rf.fo_color[j]; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:(NSUInteger)(64 + 8 * j + q)]; }
    NSError *e = nil; f = [lib newFunctionWithName:@"main0" constantValues:cv error:&e];
    if (!f) { MLOG("【红】可读 tile 版本特化失败 %s（%s）：%s", fs->key, k.UTF8String, e.localizedDescription.UTF8String); os_unfair_lock_unlock(&g_fz_fn_lock); return nil; }
    g_bf_fn_cache[cacheKey] = f; os_unfair_lock_unlock(&g_fz_fn_lock); return f;
}
static int ubo_float(int slot, uint16_t ref, float *out) {   // uniform 槽的第 vec 个向量第 cmp 分量（CPU 侧读；落在本提交待执行的缓冲写里就不读） | Component cmp of vector vec of the uniform slot (read on the CPU side; not read if it falls within a buffer write pending execution in this submit)
    int sl = ref >> 8, vec = ref >> 2 & 63, cmp = ref & 3;
    if (slot < 0 || slot >= 6 || sl < 0 || sl >= 16) return 0;
    const p_bind_buf *u = &S.ubo[slot][sl]; uint64_t off = (uint64_t)vec * 16 + (uint64_t)cmp * 4;
    if (!u->addr || off + 4 > u->size || pend_hits(u->addr + off, 4)) return 0;
    void *mb; uint64_t mo; const uint8_t *base;
    if (!mtl_addr(u->addr + off, 4, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    memcpy(out, base + mo, 4); return 1;
}
static int zskip_fact_for(const char *key, zskip_fact *out) {
    if (!key) return 0;
    for (int i = 0; i < g_zskip_nf; i++) if (!strncmp(key, g_zskip_facts[i].key, strlen(g_zskip_facts[i].key))) { *out = g_zskip_facts[i]; return 1; }   // 运行时键带 _b 后缀 | Runtime keys carry the _b suffix
    return 0;
}
static int zskip_view_zero(int tid) {
    if (!tid || tid >= g_ntreg || !g_treg[tid].ok) return 0;
    treg_t *r = &g_treg[tid]; const tex_t *t = &r->t; int lv0 = r->has_v && r->v.set_levels ? r->v.base_level : 0;
    int nl = r->has_v && r->v.set_levels && r->v.nlevels > 0 ? r->v.nlevels : 1;
    int sl0 = r->has_v && r->v.set_layers ? r->v.min_layer : 0;
    int ns = r->has_v && r->v.set_layers && r->v.nlayers > 0 ? r->v.nlayers : 1;
    for (int l = 0; l < nl; l++) for (int s = 0; s < ns; s++) if (!zskip_has(t->serial, lv0 + l, sl0 + s)) return 0;
    return 1;
}
// 遮挡查询在计数时，被跳过的绘制本该贡献通过采样数：只有「下一次清零之前没有报告」才无关紧要。
// 从本次绘制之后顺着回放顺序看：先遇到采样数清零 → 无关（返回 1）；先遇到采样数报告、或看不到头（提交结束）→ 有关（返回 0）
// While an occlusion query is counting, a skipped draw should have contributed passed samples: it only does not matter if "there is no report before the next reset".
// Looking forward in replay order from after this draw: sample count reset encountered first -> does not matter (return 1); sample count report encountered first, or the end cannot be seen (submit ends) -> matters (return 0)
static int zskip_vis_dead(void) {
    if (!g_vis_counting) return 1;
    const uint8_t *p = g_exec_next; int ci = 0; if (!p) return 0;
    for (int guard = 0; guard < 200000; guard++) { op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        if (h.op == OP_END) { p = defer_cont(&ci); if (!p) return 0; continue; }
        if (h.op == OP_CALL) return 0;
        if (h.op == OP_RESET_COUNTER || h.op == OP_REPORT_COUNTER) { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) return h.op == OP_RESET_COUNTER; }
        p += h.size; }
    return 0;
}
static int zskip_can_draw(NvProg *pr) {
    // 跳过条件（全部满足）：片元键在事实表、它反射声明的每个贴图槽绑的视图只覆盖已知全零的子资源、系数有限、
    // 不写深度不做模板、没在数遮挡查询、要写的每个颜色附件本来就是 0（本通道整张清 0 的候选，或已知全零）——写 0 盖 0。
    // 不成立原因（PASSDUMP 的 N 行）：1 深度 / 模板 2 遮挡查询计数中且清零前有报告 3 系数读不到或非有限 4 图像槽 5 被采样视图不是已知全零 6 无目标 7 目标不是 0
    // Skip conditions (all must hold): the fragment key is in the fact table, the view bound to every texture slot declared by its reflection covers only subresources known to be all zero, the coefficient is finite,
    // no depth write and no stencil, not counting an occlusion query, and every color attachment to be written is already 0 (a candidate cleared entirely to 0 in this render pass, or known all zero) -- writing 0 over 0.
    // Reasons for not holding (the N line of PASSDUMP): 1 depth / stencil 2 occlusion query counting with a report before the reset 3 coefficient unreadable or non-finite 4 image slot 5 sampled view not known all zero 6 no target 7 target is not 0
    if (!zskip_on() || !pr) return 0;
    NvShader *fs = nil; for (int st = 0; st < 5; st++) if (pr->sh[st] && pr->sh[st]->stage == 1) fs = pr->sh[st];
    zskip_fact f; float coeff; int why = 0;
    if (!fs) return 0;
    if (g_la_cache) { if (!fs->zf_state || fs->zf_nf != g_zskip_nf) { fs->zf_state = zskip_fact_for(fs->key, &fs->zf) ? 2 : 1; fs->zf_nf = (int8_t)g_zskip_nf; } if (fs->zf_state == 1) return 0; f = fs->zf; }   // 键不变，查一次即可 | Key unchanged; one lookup is enough
    else if (!zskip_fact_for(fs->key, &f)) return 0;
    if (S.ds.dw || S.ds.st) why = 1;
    else if (!zskip_vis_dead()) why = 2;
    else if (f.ubo_slot >= 0 && (!ubo_float(1, (uint16_t)((f.ubo_slot << 8) | (f.vec << 2) | f.comp), &coeff) || !isfinite(coeff))) why = 3;   // ubo_slot < 0：该着色器没有系数 | ubo_slot < 0: this shader has no coefficient
    for (int i = 0; !why && i < fs->rf.nb; i++) { nvn_rbind *b = &fs->rf.b[i];
        if (b->kind == NVN_RB_IMG) { why = 4; break; }
        if (b->kind != NVN_RB_TEX && b->kind != NVN_RB_TBUF) continue;
        int tid = b->reg < 128 ? (int)(S.tex[1][b->reg] & 0xfffff) : 0; if (!zskip_view_zero(tid)) why = 5; }
    if (!why && (!R.has_rt || !R.rt.n)) why = 6;
    for (int i = 0; !why && i < R.rt.n && i < 8; i++) { if (!R.rt.color[i]) continue;
        int pending = g_enc ? g_zskip_cand_ok[i] : zskip_zero_clear(i);   // 通道已开：看开通道时记下的候选（还没被画过）；未开：看挂起的清除 | Render pass already open: look at the candidates noted when the render pass was opened (not yet drawn to); not open: look at the pending clears
        if (!pending && (g_enc || R.clear_c[i] || !zskip_target_zero(R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i]))) why = 7; }
    if (why) PD("N %llu 零源不跳过 reason=%d 集合 %d", g_stat_pass + 1, why, g_zskip_n);
    return !why;
}
static int uv_proof(NvShader *vs, int locn, const p_draw *dw, const p_draw_idx *di) {   // 插值输出 locn 在每个像素中心恰为本像素的纹素中心：u = (x + 1) / 2、v = (1 − y) / 2，w = 1，视口与剪裁覆盖整块 | The interpolated output locn at every pixel center is exactly this pixel's texel center: u = (x + 1) / 2, v = (1 - y) / 2, w = 1, viewport and scissor cover the whole block
    if (locn < 0 || locn >= 32 || vs->rf.pos_attr < 0) return 0;
    int direct = vs->rf.uv_attr[locn] >= 0, aff = vs->rf.uv_aff_attr[locn] >= 0;
    if (!direct && !aff) return 0;
    float sc[4] = {1, 0, 1, 0};   // 原样直传 = 缩放 1、偏移 0 | Pass-through as-is = scale 1, offset 0
    if (!direct) for (int q = 0; q < 4; q++) if (!ubo_float(0, vs->rf.uv_aff[locn][q], &sc[q])) return 0;
    NSUInteger W = g_pass.tw, H = g_pass.th;
    if (S.has_vp && !(S.vp[0][0] == 0 && S.vp[0][1] == 0 && S.vp[0][2] == (float)W && S.vp[0][3] == (float)H)) return 0;
    if (R.has_sc && !(R.sc.x <= 0 && R.sc.y <= 0 && R.sc.x + R.sc.w >= (int)W && R.sc.y + R.sc.h >= (int)H)) return 0;
    float P[64][4], Q[64][4]; int n = dw ? dw->count : di->count;
    if (attr_read(vs->rf.pos_attr, dw, di, P) || attr_read(direct ? vs->rf.uv_attr[locn] : vs->rf.uv_aff_attr[locn], dw, di, Q)) return 0;
    for (int k = 0; k < n; k++) { float w = vs->rf.pos_w1 ? 1.0f : P[k][3];
        float u = direct ? Q[k][0] : fmaf(Q[k][0], sc[0], sc[1]), v = direct ? Q[k][1] : fmaf(Q[k][1], sc[2], sc[3]);   // 与着色器同一个 fma | The same fma as the shader
        if (w != 1.0f || u != (P[k][0] + 1.0f) * 0.5f || v != (1.0f - P[k][1]) * 0.5f) { PD("F %llu uv 证明：顶点 %d 位置 %g,%g uv %g,%g（缩放 / 偏移 %g,%g,%g,%g）", g_stat_pass, k, P[k][0], P[k][1], u, v, sc[0], sc[1], sc[2], sc[3]); return 0; } }
    return 1;
}
// 后一个通道的一次绘制：核对并给出特化。返回 0 = 证不出（调用方结束合并通道、按原目标重开）
// One draw of the later pass: verify and produce the specialization. Returns 0 = cannot be proven (the caller ends the merged render pass and reopens with the original targets)
static int fz_p2_draw(NvProg *pr, const p_draw *dw, const p_draw_idx *di, uint8_t lmap[8], __strong id<MTLFunction> *fn) {
    NvShader *vs = pr->sh[0], *fs = pr->sh[1]; *fn = nil;
    if (!vs || !fs) { PD("F %llu 证不出：无顶点 / 片元着色器", g_stat_pass); return 0; }
    if (!g_fz.rt2.depth && R.rt.depth && (S.ds.dt || S.ds.dw || S.ds.st)) { PD("F %llu 证不出：深度来自前一个通道而本次绘制开了深度模板测试", g_stat_pass); return 0; }
    int fx[32]; for (int i = 0; i < 32; i++) fx[i] = -1; uint8_t fbmask = 0;
    for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh || sh->stage >= 6) continue;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? S.img[sh->stage][b->reg] : 0; else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = b->reg < 128 ? S.tex[sh->stage][b->reg] : 0; else continue;
            uint64_t ser = h ? h_ser(h) : 0; if (!ser) continue;
            int j = -1; for (int m = 0; m < g_fz.n1; m++) if (R.rt.color[m] && R.rt.color[m]->serial == ser) { j = m; break; }
            if (j < 0) continue;   // 采样的不是前一个通道的附件 | What is sampled is not an attachment of the earlier pass
            if (sh != fs || b->kind != NVN_RB_TEX) { PD("F %llu 证不出：非片元阶段或非贴图槽读前一个通道的附件", g_stat_pass); return 0; }
            if (fs->rf.fc_mixed[b->reg >> 6] >> (b->reg & 63) & 1) { PD("F %llu 证不出：槽 %d 混用", g_stat_pass, b->reg); return 0; }
            uint64_t s_; int lv, sl; rt_sub(&R.rt, j, &s_, &lv, &sl);
            if (lv || sl || MAX(R.rt.color[j]->d.width, 1u) != g_pass.tw || MAX(R.rt.color[j]->d.height, 1u) != g_pass.th) { PD("F %llu 证不出：附件不是第 0 级第 0 层或尺寸不符", g_stat_pass); return 0; }
            {   // 采样的那个视图也得正好是附件那一张（原先只核附件是第 0 级第 0 层，没核着色器绑的视图——同一张贴图按第 k 级 / 第 k 层 / 换格式的视图采样时， | The sampled view must also be exactly the attachment itself (previously only the attachment being level 0 layer 0 was checked, not the view bound by the shader -- when the same texture is sampled through a view of level k / layer k / a changed format,
                // 证明照样成立，采样会被改成读片上第 0 级 / 原格式，画面就错了）。视图起始级 / 层须为 0、格式须与附件实际格式相同，否则不合并
                // the proof would still hold, the sampling would be changed to read on-tile level 0 / the original format, and the picture would be wrong). The view's starting level / layer must be 0 and its format must equal the attachment's actual format, otherwise do not merge
                int tid_ = (int)(h & 0xfffff); void *tv_ = NULL; if (tid_) tex_fast_rid(tid_, &tv_); __unsafe_unretained id<MTLTexture> tv = (__bridge id<MTLTexture>)tv_;
                uint32_t apf_ = tv.parentTexture ? (uint32_t)tv.parentTexture.pixelFormat : (uint32_t)tv.pixelFormat; uint8_t swz_[4];
                if (R.rt.has_cview[j] && R.rt.cview[j].set_format && !(nvn_fmt_to_mtl(R.rt.cview[j].format, &apf_, swz_) && apf_)) apf_ = 0;   // 附件按换格式视图挂：取那个格式；认不出就不合 | The attachment is attached through a format-changing view: take that format; if unrecognized, do not merge
                MTLPixelFormat apf = (MTLPixelFormat)apf_;
                if (!tv || (tv.parentTexture && (tv.parentRelativeLevel || tv.parentRelativeSlice)) || tv.pixelFormat != apf) {
                    static int lg; if (lg++ < 8) LOG("[nvn-mtl] 片上读合并：槽 %d 采样视图（起始级 %lu 层 %lu 格式 %lu）与附件（第 0 级第 0 层 格式 %lu）不符，不合并（帧 %llu 通道 %llu）", b->reg,
                        tv && tv.parentTexture ? (unsigned long)tv.parentRelativeLevel : 0ul, tv && tv.parentTexture ? (unsigned long)tv.parentRelativeSlice : 0ul, (unsigned long)tv.pixelFormat, (unsigned long)apf,
                        (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass);
                    PD("F %llu 证不出：槽 %d 采样视图不是附件那一张（级 / 层 / 格式不符）", g_stat_pass, b->reg); return 0; } }
            int any = 0;
            for (int c = 0; c < fs->rf.nfc; c++) if (fs->rf.fc_reg[c] == b->reg) {
                if (!uv_proof(vs, fs->rf.fc_locn[c], dw, di)) { PD("F %llu 证不出：槽 %d 的采样坐标不是本像素（插值输出 %d）", g_stat_pass, b->reg, fs->rf.fc_locn[c]); return 0; }
                fx[c] = j; fbmask |= (uint8_t)(1u << j); any = 1; }
            if (!any) { PD("F %llu 证不出：槽 %d 的采样不在可改表里", g_stat_pass, b->reg); return 0; } } }
    memset(lmap, 0xff, 8);
    for (int c = 0; c < 8; c++) if (fs->rf.color_mask >> c & 1) { if (g_fz.p2map[c] < 0) { PD("F %llu 证不出：输出颜色 %d 无对应附件", g_stat_pass, c); return 0; } lmap[g_fz.p2map[c]] = (uint8_t)c; }
    int ident = !fbmask; for (int c = 0; c < 8; c++) if ((fs->rf.color_mask >> c & 1) && g_fz.p2map[c] != c) ident = 0;
    if (ident) { for (int q = 0; q < 8; q++) if (lmap[q] != 0xff) g_fz.wrote[q] = 1; if (fz_check_on()) fz_snap_take(dw, di); return 1; }   // 不读 tile、输出不移位：用原版本（写掩码仍按映射） | Does not read the tile and the output is not shifted: use the original version (the write mask still follows the mapping)
    if (!fs->rf.nfo) { bf_want_note(fs->key); PD("F %llu 证不出：没有可读 tile 版本（输出须移位）", g_stat_pass); return 0; }
    *fn = bf_fn(fs, fx, fbmask, lmap);
    if (!*fn) return 0;
    memset(&g_fz_desc, 0, sizeof g_fz_desc); g_fz_desc.kind = 1; g_fz_desc.fbmask = fbmask; for (int i = 0; i < 32; i++) g_fz_desc.fx[i] = (int8_t)fx[i];
    for (int q = 0; q < 8; q++) if (lmap[q] != 0xff) g_fz.wrote[q] = 1;
    if (fz_check_on()) fz_snap_take(dw, di);
    g_fz_draws++; return 1;
}
static void bloomq_reset(void) { memset(&g_bloomq, 0, sizeof g_bloomq); }
static int bloomq_enabled(void) {
    if (g_bloom_q < 0) { const char *e = getenv("DM_NVN_BLOOM_Q"); g_bloom_q = !e || strcmp(e, "0"); }
    return g_bloom_q;
}
static int bloomq_target(uint64_t *ser, int *lv, int *sl) {
    if (!R.has_rt || R.rt.n != 1 || !R.rt.color[0]) return 0;
    rt_sub(&R.rt, 0, ser, lv, sl); return *ser != 0;
}
static int bloomq_make_pipeline(MTLPixelFormat fmt) {
    if (g_bloomq_pso && g_bloomq_fmt == fmt) return 1;
    static const char *src =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 uv=float2((id<<1)&2,id&2); V o; o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1); o.uv=uv; return o; }\n"
        "struct P { float threshold; float coeff_g; float coeff_b; float dm; float sh; };\n"
        "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant P& p [[buffer(0)]]) { float2 d=float2(1.0/p.dm,1.0/p.sh); float3 c=0; for (int y=-1;y<=1;y+=2) for (int x=-1;x<=1;x+=2) { float3 v=t.sample(s,in.uv+float2(x,y)*d).rgb; float l=v.r*0.2126+v.g*p.coeff_g+v.b*p.coeff_b; if(l>=p.threshold) c+=v/(1.0+l); } return float4(c*0.25,1); }\n";
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&e];
    if (!lib) { MLOG("【红】泛光 1/4 着色器编译失败：%s", e.localizedDescription.UTF8String); return 0; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = fmt;
    g_bloomq_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!g_bloomq_pso) { MLOG("【红】泛光 1/4 管线失败：%s", e.localizedDescription.UTF8String); return 0; }
    g_bloomq_fmt = fmt;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES;
    g_bloomq_samp = [g_dev newSamplerStateWithDescriptor:sd]; return g_bloomq_samp != nil;
}
static int bloomq_capture(void) {
    uint64_t ser; int lv, sl; if (!bloomq_target(&ser, &lv, &sl) || !g_bloomq.s_tex) return 0;
    id<MTLTexture> base = tex_mtl(R.rt.color[0]), src = tex_mtl(g_bloomq.s_tex); if (!base || !src) return 0;
    id<MTLTexture> dst = base; if (lv || sl) { static id<MTLTexture> vb; static int vl, vs; if (vb != base || vl != lv || vs != sl) { defer_obj_release(g_bloomq_view);   /* 旧视图退役（原先直接被覆盖：留在驻留集里，连带钉住旧底图） | retire the old view (it used to be overwritten directly: it stayed in the residency set and pinned the old base texture with it) */ g_bloomq_view = [base newTextureViewWithPixelFormat:base.pixelFormat textureType:base.textureType levels:NSMakeRange(lv, 1) slices:NSMakeRange(sl, 1)]; if (g_bloomq_view) res_add(g_bloomq_view); vb = base; vl = lv; vs = sl; } dst = g_bloomq_view; }
    if (!dst || !bloomq_make_pipeline(dst.pixelFormat)) return 0;
    rp_end();
    RPD *rp = [RPD new]; rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    RENC en = [g_cb renderCommandEncoderWithDescriptor:rp]; if (!en) return 0;
#if DM_NVN_M3
    [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:g_bloomq_samp atIndex:0];
    float *p = g_bloomq_const[g_bloomq_i].contents; p[0] = g_bloomq.threshold; p[1] = g_bloomq.coeff_g; p[2] = g_bloomq.coeff_b; p[3] = src.width; p[4] = src.height; [en setFragmentBuffer:g_bloomq_const[g_bloomq_i] offset:0 atIndex:0];
#else
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice]; [en setArgumentTable:g_bloomq_args atStages:MTLRenderStageFragment];
    [g_bloomq_args setTexture:src.gpuResourceID atIndex:0]; [g_bloomq_args setSamplerState:g_bloomq_samp.gpuResourceID atIndex:0];
    float *p = g_bloomq_const[g_bloomq_i].contents; p[0] = g_bloomq.threshold; p[1] = g_bloomq.coeff_g; p[2] = g_bloomq.coeff_b; p[3] = src.width; p[4] = src.height; [g_bloomq_args setAddress:g_bloomq_const[g_bloomq_i].gpuAddress atIndex:0];
#endif
    [en setViewport:(MTLViewport){0,0,dst.width,dst.height,0,1}]; [en setRenderPipelineState:g_bloomq_pso]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; m4_end(en);
    g_bloomq_i = (g_bloomq_i + 1) % 3; args_dirty_all(); g_pso_dirty = g_dss_dirty = 1; return 1;
}
// ---------- 拖货跟手图：商人货架上的货没有「拿起」态，拖动时由回放层把「画这件货图标的那次绘制」在同一趟末尾平移到手指处再画一遍 ----------
// 界面那一趟每个矩形是一次 4 顶点绘制（顶点 20 字节：位置 3 浮点 + 贴图坐标 2 浮点，位置经顶点常量 0 号的矩阵到裁剪坐标）。
// 前端（front_ios.m）给出手指按下的那一点、货架格网与跟手图中心（归一化，左上原点）；这里在界面趟里找「盖住这一点、四边都压在格线上、面积最小」的矩形 = 那件货
// （只按点认会认错：按下约 0.1 秒后游戏在按下点画一个以它为中心、一格大小的矩形，不压格线——真机上两次把它当成了货）
// （不依赖游戏的选中态：选中要等手柄光标落到货上约 0.1 秒，按下就拖的快拖等不到）。同一矩形上有格底和图标两次绘制：
// 格底用公共贴图（本趟很多矩形都用它），图标每件一张——取「本趟用到次数最少」的那张贴图的那次。认到的矩形回报给前端（货架原位盖暗色用）。
// 界面矩形在真实画面里的纵向位置与高度（都按占画面高的比例：y = 0 贴顶，h = 1 即整个画面）；前端登记，每次绘制现读（切主机 / 掌机档会变）。
// 游戏把界面画在界面贴图的上部 h 那一截（视口是整张贴图，投影矩阵里带着缩放）。
// ---------- Drag-follow image for shop items: items on a vendor's shelf have no "picked up" state, so during a drag the replay layer takes "the draw that draws this item's icon", translates it to the finger at the end of the same pass and draws it once more ----------
// In the UI pass each rectangle is one 4-vertex draw (20-byte vertices: position 3 floats + texture coordinates 2 floats; the position goes to clip coordinates through the matrix in vertex constant 0).
// The front end (front_ios.m) gives the point where the finger pressed, the shelf grid and the center of the drag-follow image (normalized, top-left origin); here, in the UI pass, find the rectangle that "covers this point, has all four edges on grid lines, and has the smallest area" = that item
// (identifying by the point alone gets it wrong: about 0.1 seconds after the press the game draws a one-cell-sized rectangle centered on the press point, not on grid lines -- on a real device it was twice taken for the item)
// (does not depend on the game's selected state: selection has to wait about 0.1 seconds for the controller cursor to land on the item, and a quick drag that starts on press cannot wait for that). The same rectangle has two draws, cell background and icon:
// the cell background uses a common texture (many rectangles in this pass use it), while each item has its own icon texture -- take the draw with the texture "used the fewest times in this pass". The recognized rectangle is reported back to the front end (used to cover the original shelf position with a dark color).
// Vertical position and height of the UI rectangle in the real picture (both as a fraction of the picture height: y = 0 touches the top, h = 1 is the whole picture); registered by the front end and read fresh on every draw (changes when switching between docked / handheld mode).
// The game draws the UI into the upper h portion of the UI texture (the viewport is the whole texture, and the projection matrix carries the scaling).
static void (*g_ui_rect_fn)(double *y, double *h);
void mtl_set_ui_rect_fn(void (*fn)(double *y, double *h)) { g_ui_rect_fn = fn; }
static double ui_hfrac(void) { double y = 0, h = 1; if (g_ui_rect_fn) g_ui_rect_fn(&y, &h); return h > 0 && h < 1 ? h : 1; }
static _Atomic int g_ghost_on; static _Atomic uint64_t g_ghost_pt, g_ghost_at, g_ghost_found, g_ghost_grid;
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy) {
    if (on) { uint64_t gv = 0; for (int i = 0; i < 4; i++) gv |= (uint64_t)(uint16_t)(fminf(fmaxf(grid[i], 0), 1) * 65535 + .5f) << (16 * i); atomic_store(&g_ghost_grid, gv);
              atomic_store(&g_ghost_pt, (uint64_t)(uint32_t)lroundf(fminf(fmaxf(px, 0), 1) * 65535) | (uint64_t)(uint32_t)lroundf(fminf(fmaxf(py, 0), 1) * 65535) << 32);
              atomic_store(&g_ghost_at, (uint64_t)(uint32_t)(int32_t)lroundf(cx * 65535) | (uint64_t)(uint32_t)(int32_t)lroundf(cy * 65535) << 32); }
    else atomic_store(&g_ghost_found, 0);
    atomic_store(&g_ghost_on, on);
}
int mtl_ui_ghost_rect(float r[4]) { uint64_t v = atomic_load(&g_ghost_found); if (!v) return 0; for (int i = 0; i < 4; i++) r[i] = (float)((v >> (16 * i)) & 0xffff) / 65535.f; return 1; }
static struct { uint64_t pass; int n, nt, busy; float area, r[4]; struct { void *S_; p_draw dw; uint64_t tex; } c[4]; uint64_t tid[128]; uint16_t tn[128]; } g_gh;
static uint64_t ghost_tex(void) {   // 这次绘制的片元着色器实际采样的那张贴图（按着色器反射取槽位；S.tex 里别的槽是之前绘制留下的） | The texture actually sampled by this draw's fragment shader (slot taken from shader reflection; the other slots in S.tex are left over from earlier draws)
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; NvShader *f = pr ? pr->sh[1] : nil; if (!f) return 0;
    for (int i = 0; i < f->rf.nb; i++) if ((f->rf.b[i].kind == NVN_RB_TEX || f->rf.b[i].kind == NVN_RB_SAMP) && f->rf.b[i].reg < 128 && S.tex[1][f->rf.b[i].reg]) return S.tex[1][f->rf.b[i].reg];
    return 0; }
// 当前这次界面绘制是不是「不带贴图、正好盖满整个界面矩形」（界面矩形占贴图上部 hf）
// Whether the current UI draw is "untextured and exactly covering the whole UI rectangle" (the UI rectangle occupies the upper hf of the texture)
static int ui_cover_all(double hf) {
    if (S.vbuf[0].size != 80 || ghost_tex()) return 0;
    const p_bind_buf *u = &S.ubo[0][0]; void *vb, *mb; uint64_t vo, mo; const uint8_t *vp_, *mp_;
    if (!u->addr || u->size < 64 || pend_hits(u->addr, 64) || pend_hits(S.vbuf[0].address, 80)) return 0;
    if (!mtl_addr(S.vbuf[0].address, 80, &vb, &vo) || !(vp_ = [(__bridge id<MTLBuffer>)vb contents]) || !mtl_addr(u->addr, 64, &mb, &mo) || !(mp_ = [(__bridge id<MTLBuffer>)mb contents])) return 0;
    float m[16], x0 = 9, y0 = 9, x1 = -9, y1 = -9; memcpy(m, mp_ + mo, 64);
    for (int i = 0; i < 4; i++) { float f[2]; memcpy(f, vp_ + vo + 20 * i, 8); float nx = (m[0] * f[0] + m[4] * f[1] + m[12] + 1) / 2, ny = (m[1] * f[0] + m[5] * f[1] + m[13] + 1) / 2;
        x0 = fminf(x0, nx); x1 = fmaxf(x1, nx); y0 = fminf(y0, ny); y1 = fmaxf(y1, ny); }
    return x0 < 0.002f && x1 > 0.998f && y0 < 0.002f && fabsf(y1 - (float)hf) < 0.002f;
}
static void ghost_note(const p_draw *dw) {
    if (g_gh.busy || dw->count != 4 || S.nattr != 2 || S.stream[0].stride != 20 || S.vbuf[0].size != 80) return;
    if (g_gh.pass != g_stat_pass) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = g_gh.nt = 0; g_gh.pass = g_stat_pass; }
    uint64_t t = ghost_tex(); int k = 0; for (; k < g_gh.nt; k++) if (g_gh.tid[k] == t) break;
    if (k == g_gh.nt && g_gh.nt < 128) { g_gh.tid[k] = t; g_gh.tn[k] = 0; g_gh.nt++; } if (k < g_gh.nt) g_gh.tn[k]++;
    const p_bind_buf *u = &S.ubo[0][0]; void *vb, *mb; uint64_t vo, mo; const uint8_t *vp_, *mp_;
    if (!u->addr || u->size < 64 || pend_hits(u->addr, 64) || pend_hits(S.vbuf[0].address, 80)) return;
    if (!mtl_addr(S.vbuf[0].address, 80, &vb, &vo) || !(vp_ = [(__bridge id<MTLBuffer>)vb contents]) || !mtl_addr(u->addr, 64, &mb, &mo) || !(mp_ = [(__bridge id<MTLBuffer>)mb contents])) return;
    float m[16], x0 = 9, y0 = 9, x1 = -9, y1 = -9; memcpy(m, mp_ + mo, 64);
    for (int i = 0; i < 4; i++) { float f[2]; memcpy(f, vp_ + vo + 20 * i, 8); float nx = (m[0] * f[0] + m[4] * f[1] + m[12] + 1) / 2, ny = (m[1] * f[0] + m[5] * f[1] + m[13] + 1) / 2;
        x0 = fminf(x0, nx); x1 = fmaxf(x1, nx); y0 = fminf(y0, ny); y1 = fmaxf(y1, ny); }
    { float hf = (float)ui_hfrac(); y0 /= hf; y1 /= hf; }   // 前端给的点是界面矩形里的归一化坐标；界面只占贴图上部 hf 那一截时换到同一口径（hf = 1 时不变） | The points given by the front end are normalized coordinates within the UI rectangle; when the UI occupies only the upper hf portion of the texture, convert to the same convention (unchanged when hf = 1)
    uint64_t pv = atomic_load(&g_ghost_pt); float qx = (float)(uint32_t)pv / 65535.f, qy = (float)(pv >> 32) / 65535.f;
    if (qx < x0 || qx > x1 || qy < y0 || qy > y1) return;
    uint64_t gv = atomic_load(&g_ghost_grid); float gx = (float)(gv & 0xffff) / 65535.f, gy = (float)((gv >> 16) & 0xffff) / 65535.f, gw = (float)((gv >> 32) & 0xffff) / 65535.f, gh = (float)(gv >> 48) / 65535.f;
    if (gw <= 0 || gh <= 0) return;
    float w = x1 - x0, h = y1 - y0, area = w * h, c0 = (x0 - gx) / gw, r0 = (y0 - gy) / gh, cn = w / gw, rn = h / gh;   // 左上角在第几格、占几格 | Which cell the top-left corner is in, and how many cells it occupies
    if (fabsf(c0 - roundf(c0)) > 0.15f || fabsf(r0 - roundf(r0)) > 0.15f || fabsf(cn - roundf(cn)) > 0.15f || fabsf(rn - roundf(rn)) > 0.15f || cn < 0.85f || rn < 0.85f || cn > 2.15f || rn > 4.15f) return;   // 一件货最大 2×4 格 | One item is at most 2x4 cells
    if (g_gh.n && area > g_gh.area * 1.03f) return;                       // 已有更小的 | A smaller one already exists
    if (g_gh.n && area < g_gh.area * 0.97f) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0; }   // 找到更小的：前面的作废 | Found a smaller one: the earlier ones are invalidated
    if (g_gh.n == 4) { free(g_gh.c[0].S_); memmove(&g_gh.c[0], &g_gh.c[1], 3 * sizeof g_gh.c[0]); g_gh.n = 3; }
    g_gh.area = area; g_gh.r[0] = x0; g_gh.r[1] = y0; g_gh.r[2] = x1; g_gh.r[3] = y1;
    typeof(g_gh.c[0]) *c = &g_gh.c[g_gh.n++]; c->S_ = malloc(sizeof S); memcpy(c->S_, &S, sizeof S); c->dw = *dw; c->tex = t;
}
static void ghost_flush(void) {   // 渲染通道结束前：把认到的图标绘制平移到手指处再画一遍（挪视口，不改顶点；剪裁放开） | Before the render pass ends: translate the recognized icon draw to the finger and draw it once more (moves the viewport, does not change vertices; the scissor is opened up)
    if (g_gh.busy || !g_gh.n) return;
    if (g_gh.pass != g_stat_pass || !atomic_load(&g_ghost_on)) { for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0; return; }
    int best = -1, bn = 1 << 30; for (int i = 0; i < g_gh.n; i++) { int cnt = 0; for (int k = 0; k < g_gh.nt; k++) if (g_gh.tid[k] == g_gh.c[i].tex) cnt = g_gh.tn[k]; if (cnt < bn) { bn = cnt; best = i; } }
    typeof(g_gh.c[0]) *c = &g_gh.c[best]; uint64_t at = atomic_load(&g_ghost_at); float ax = (float)(int32_t)(uint32_t)at / 65535.f, ay = (float)(int32_t)(uint32_t)(at >> 32) / 65535.f;
    float W = (float)g_pass.tw, H = (float)g_pass.th, ccx = (g_gh.r[0] + g_gh.r[2]) / 2, ccy = (g_gh.r[1] + g_gh.r[3]) / 2;
    { uint64_t v = 0; for (int i = 0; i < 4; i++) v |= (uint64_t)(uint16_t)(fminf(fmaxf(g_gh.r[i], 0), 1) * 65535 + .5f) << (16 * i); atomic_store(&g_ghost_found, v ? v : 1); }
    g_gh.busy = 1; typeof(S) save; memcpy(&save, &S, sizeof S); p_scissor sc = R.sc; int has_sc = R.has_sc;
    memcpy(&S, c->S_, sizeof S); if (!S.has_vp) { S.vp[0][0] = S.vp[0][1] = 0; S.vp[0][2] = W; S.vp[0][3] = H; S.has_vp = 1; }
    S.vp[0][0] += (ax - ccx) * W; S.vp[0][1] += (ay - ccy) * H * (float)ui_hfrac(); R.has_sc = 0;
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1; draw(&c->dw, NULL);
    { static int once; if (!once++) MLOG("拖货跟手图：首例 通道 %llu，候选 %d 个，取贴图本趟用到 %d 次的那次", g_stat_pass, g_gh.n, bn); }
    memcpy(&S, &save, sizeof S); args_dirty_all(); R.sc = sc; R.has_sc = has_sc; g_pso_dirty = g_dss_dirty = 1; g_gh.busy = 0;
    for (int i = 0; i < g_gh.n; i++) free(g_gh.c[i].S_); g_gh.n = 0;
}
static void draw(const p_draw *dw, const p_draw_idx *di) {
    if (!S.prog) { RED_ONCE(18, "绘制前没有绑定程序"); return; }
    __unsafe_unretained NvProg *pr = prog_get(S.prog);
    if (g_ruler_truth == 0) {   // 尺子负对照（DM_NVN_RULER_NEG=<片元键>）：被测模式下丢掉该着色器的绘制，尺子必须报不等价 | Ruler negative control (DM_NVN_RULER_NEG=<fragment key>): in the mode under test drop this shader's draws, and the ruler must report non-equivalence
        static const char *neg; static int ng = -1; if (ng < 0) { neg = getenv("DM_NVN_RULER_NEG"); ng = neg && *neg; }
        if (ng && pr) for (int st = 0; st < 5; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (sh && sh->stage == 1 && !strncmp(sh->key, neg, strlen(neg))) { static int once; if (!once++) MLOG("DM_NVN_RULER_NEG：被测模式丢弃着色器 %s 的绘制", sh->key); return; } }
    }
    if (zskip_can_draw(pr)) {   // 零源跳过：输出恒为 0、写 0 盖 0，这次绘制不改变任何结果（见 zskip_can_draw） | Zero-source skip: the output is always 0, writing 0 over 0, so this draw changes no result (see zskip_can_draw)
        g_zskip_count++; { static int once; if (!once++) MLOG("零源跳过：首例 通道 %llu", g_stat_pass + 1); }
        PD("N %llu 零源跳过", g_stat_pass + 1); return; }
    if (!g_defer_replaying) {
        if (g_defer.active && g_defer.in_p && !g_defer.at_q) { if (defer_take(dw, di)) return; }
    }
    // 什么都画不出的绘制（两面全剔、剪裁为空）在开通道之前就返回：否则通道已开，TBDR 上整张附件白读白写
    // （Mac 选角页：3 个通道只因此类绘制而开，全部读入、零绘制）
    // 量上界用的诊断开关 DM_NVN_SKIP_FS=<片元键>[,<片元键>…]（缺省无）：丢掉这些片元着色器的全部绘制（画面会错，只用来在真机 ABA 里量「这类绘制总共值多少 GPU 时间」）
    // Draws that cannot draw anything (both faces culled, empty scissor) return before the render pass is opened: otherwise the render pass is already open and on TBDR the whole attachment is loaded and stored for nothing
    // (Mac character select page: 3 render passes are opened only because of such draws, all loaded, zero draws)
    // Diagnostic switch for measuring an upper bound DM_NVN_SKIP_FS=<fragment key>[,<fragment key>...] (none by default): drops all draws of these fragment shaders (the picture will be wrong; only used in real-device ABA to measure "how much GPU time this class of draws is worth in total")
    { static char sk[16][20]; static int nsk = -1;
      if (nsk < 0) { nsk = 0; for (const char *q = getenv("DM_NVN_SKIP_FS"); q && *q && nsk < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 16) n = 16; memcpy(sk[nsk], q, n); sk[nsk++][n] = 0; }
                     if (nsk) MLOG("DM_NVN_SKIP_FS：丢弃 %d 个片元着色器的绘制（诊断，画面不对）", nsk); }
      if (g_ub_n > 0 && pr->sh[1] && !g_defer_replaying && ub_skip(pr->sh[1]->key)) return;   // 同进程交替测量：本相丢这一类 | In-process alternating measurement: this phase drops this class
      if (nsk && pr->sh[1] && !g_defer_replaying) for (int i = 0; i < nsk; i++) if (!strncmp(pr->sh[1]->key, sk[i], strlen(sk[i]))) return; }
    // 同类诊断 DM_NVN_SKIP_SMALL=<宽>：丢掉目标宽度（按所挂的级）≤ 该值的全部绘制——量「极小通道整批丢掉」在并行流水下值多少（每通道固定开销）
    // Similar diagnostic DM_NVN_SKIP_SMALL=<width>: drop all draws whose target width (by the attached level) is <= this value -- measures how much "dropping tiny render passes in bulk" is worth under the parallel pipeline (fixed per-pass overhead)
    { static int small = -1; if (small < 0) { const char *e = getenv("DM_NVN_SKIP_SMALL"); small = e ? atoi(e) : 0; if (small) MLOG("DM_NVN_SKIP_SMALL：丢弃目标宽度 ≤ %d 的绘制（诊断，画面不对）", small); }
      if (small > 0 && R.has_rt && !g_defer_replaying) { int k = R.rt.n && R.rt.color[0] ? 0 : R.rt.depth ? 8 : -1;
        if (k >= 0) { uint64_t s_; int lv, sl; rt_sub(&R.rt, k, &s_, &lv, &sl); const tex_t *t = k == 8 ? R.rt.depth : R.rt.color[0]; if ((int)MAX(t->d.width >> lv, 1u) <= small) return; } } }
    // 试验开关 DM_NVN_NOFXAA=1（缺省关，去留按视觉尺子 + 实机看图定）：跳过 FXAA 那次全屏绘制，后面的锐化 / 放大改读 FXAA 的输入（色调映射结果）
    // Experimental switch DM_NVN_NOFXAA=1 (off by default; keep or drop decided by the visual ruler + looking at the picture on a real machine): skips the FXAA full-screen draw, and the later sharpening / upscale reads FXAA's input instead (the tone mapping result)
    { static int nofxaa = -1; static uint64_t fx_from, fx_to; if (nofxaa < 0) { const char *e = getenv("DM_NVN_NOFXAA"); nofxaa = e && !strcmp(e, "1"); if (nofxaa) MLOG("DM_NVN_NOFXAA=1：跳过 FXAA"); }
      if (nofxaa && tbdr_on() && !g_defer_replaying && pr->sh[1]) { NvShader *f = pr->sh[1];
        if (!strncmp(f->key, "ebcfcd1fa0a77dbc", 16) && !g_enc && R.rt.n == 1 && R.rt.color[0] && !R.clear_c[0]) {
            uint64_t h = 0; for (int i = 0; i < f->rf.nb && !h; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < 128) h = S.tex[1][f->rf.b[i].reg];
            if (h) { fx_from = R.rt.color[0]->serial; fx_to = h; PD("N %llu 跳过 FXAA：ser=%llu 的读者改读 ser=%llu", g_stat_pass, (unsigned long long)fx_from, (unsigned long long)h_ser(h)); return; } }
        else if (fx_from && !strncmp(f->key, "52aac1f2306b8fc5", 16)) { for (int r = 0; r < 128; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == fx_from) { S.tex[1][r] = fx_to; g_args_dirty[1] = 1; } fx_from = 0; } } }
    { static uint64_t bloomq_frame; if (bloomq_frame != atomic_load(&g_cur_frame)) { bloomq_frame = atomic_load(&g_cur_frame); bloomq_reset(); }
      if (bloomq_enabled() && tbdr_on() && !g_defer_replaying && pr->sh[1]) {
        NvShader *f = pr->sh[1]; uint64_t tgt = 0; int lv, sl;
        if (!strncmp(f->key, "2938a5b71b8e21b2", 16)) {
            if (g_bloomq.state) bloomq_reset();
            if (bloomq_target(&tgt, &lv, &sl) && R.rt.color[0]) {
                uint64_t h = 0; const tex_t *st = NULL; int cbuf = 0;
                for (int i = 0; i < f->rf.nb; i++) { nvn_rbind *b = &f->rf.b[i];
                    if (b->kind == NVN_RB_TEX && b->reg < 128 && !h) { h = S.tex[1][b->reg]; int tid = (int)(h & 0xfffff); if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) st = &g_treg[tid].t; }
                    if (b->kind == NVN_RB_SAMPT && b->reg < 128) g_bloomq.samp_h = S.tex[1][b->reg] >> 20;
                    if (b->kind == NVN_RB_SAMP && b->reg < 128) g_bloomq.samp_h = S.samp[1][b->reg] >> 20;
                    if (b->kind == NVN_RB_CBUF && !cbuf) cbuf = ubo_float(1, (uint16_t)(b->reg << 8), &g_bloomq.threshold);   // c7[0].x
                }
                if (h && st && cbuf && f->rf.c1_len >= sizeof(float) * 2) {
                    float c1[2]; memcpy(c1, f->rf.c1, sizeof c1); g_bloomq.s_tex = st; g_bloomq.s_h = h; g_bloomq.s_ser = st->serial; g_bloomq.b0_ser = tgt; g_bloomq.coeff_g = c1[0]; g_bloomq.coeff_b = c1[1]; g_bloomq.state = 1;
                    { static int once; if (!once++) MLOG("泛光 1/4 起点：生效（S ser=%llu B0 ser=%llu）", (unsigned long long)g_bloomq.s_ser, (unsigned long long)g_bloomq.b0_ser); } PD("A %llu 泛光 1/4 起点 S=%llu B0=%llu", g_stat_pass, (unsigned long long)g_bloomq.s_ser, (unsigned long long)g_bloomq.b0_ser); R.clear_c[0] = 0;   // B0 整级不要了：挂起的清除也丢掉，否则会为它单开一个只清除的通道 | The whole B0 level is no longer needed: drop the pending clear too, otherwise a clear-only render pass would be opened just for it
                    return;
                }
            }
        } else if (g_bloomq.state == 1 && !strncmp(f->key, "019ce2f82edbda84", 16)) {
            uint64_t src = 0; for (int i = 0; i < f->rf.nb; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < 128) { src = h_ser(S.tex[1][f->rf.b[i].reg]); break; }
            if (src == g_bloomq.b0_ser && bloomq_target(&tgt, &lv, &sl) && bloomq_capture()) { R.clear_c[0] = 0; g_bloomq.b1_ser = tgt; g_bloomq.state = 2; PD("B %llu 泛光 1/4 原生截取 B0=%llu B1=%llu", g_stat_pass, (unsigned long long)g_bloomq.b0_ser, (unsigned long long)g_bloomq.b1_ser); return; }
            RED_ONCE(62, "泛光 1/4 起点：截取已跳过但原生截取没画成，本帧缺泛光"); bloomq_reset();
        } else if (g_bloomq.state == 2 && !strncmp(f->key, "019ce2f82edbda84", 16)) {
            for (int i = 0; i < f->rf.nb; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < 128 && h_ser(S.tex[1][f->rf.b[i].reg]) == g_bloomq.b1_ser) { g_bloomq.b1_h = S.tex[1][f->rf.b[i].reg]; break; }
        } else if (g_bloomq.state == 2 && !strncmp(f->key, "cc264583f2000c81", 16)) {
            if (bloomq_target(&tgt, &lv, &sl) && tgt == g_bloomq.b0_ser) { g_bloomq.state = 3; PD("C %llu 泛光 1/4 跳过 B0 上采样", g_stat_pass); return; }
        } else if (g_bloomq.state == 3 && !strncmp(f->key, "2e2d6700a2569795", 16)) {
            if (g_bloomq.b1_h) { for (int r = 0; r < 128; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == g_bloomq.b0_ser) { S.tex[1][r] = g_bloomq.b1_h; g_args_dirty[1] = 1; } PD("D %llu 泛光 1/4 叠加改读 B1", g_stat_pass); g_bloomq_last_b0 = g_bloomq.b0_ser; g_bloomq_last_b1h = g_bloomq.b1_h; g_bloomq_last_b1s = g_bloomq.b1_ser; bloomq_reset(); }
            else { RED_ONCE(63, "泛光 1/4 状态缺少 B1 句柄"); bloomq_reset(); }
        } else if (!g_bloomq.state && g_bloomq_last_b1h && h_ser(g_bloomq_last_b1h) == g_bloomq_last_b1s && !strncmp(f->key, "2e2d6700a2569795", 16)) {   // B1 句柄仍指同一张贴图（换分辨率重建后不认） | The B1 handle still points to the same texture (not recognized after a rebuild on resolution change)
            // 本帧泛光链没跑（运行时外推的外推帧跳过了它：输入 469 区域外是上一帧 LDR，不能跑）就到了泛光叠加：照最近一次改读 B1，叠上一帧的泛光。
            // 不改读就读 B0——开了泛光 1/4 的话 B0 的上采样一直被跳过、从没写过，读到的是未初始化显存（M4 Pro 上区域里的单位整个发绿，别处多半是 0、缺泛光）
            // The bloom overlay is reached without the bloom chain having run this frame (the extrapolated frame of runtime extrapolation skipped it: outside the region the input 469 is the previous frame's LDR, so it cannot run): redirect the read to B1 as last time, overlaying the previous frame's bloom.
            // Without the redirect it would read B0 -- with bloom 1/4 on, B0's upsample is always skipped and never written, so what is read is uninitialized video memory (on M4 Pro the units in the region turn entirely green; elsewhere it is mostly 0, i.e. missing bloom)
            int hit = 0; for (int r = 0; r < 128; r++) if (S.tex[1][r] && h_ser(S.tex[1][r]) == g_bloomq_last_b0) { S.tex[1][r] = g_bloomq_last_b1h; hit = 1; g_args_dirty[1] = 1; }
            if (hit) { PD("D %llu 泛光链没跑：叠加沿用上一帧 B1", g_stat_pass); static uint64_t n; if (n++ % 600 == 0) MLOG("泛光 1/4：泛光链没跑的帧叠加沿用上一帧 B1（第 %llu 次）", (unsigned long long)n); }
        } else if (g_bloomq.state) { RED_ONCE(61, "泛光 1/4 起点：链中途出现别的绘制，本帧泛光不完整"); bloomq_reset(); }
      }
    }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) return;
    // 续用的通道可能是少挂深度 / 模板开的（开法 2 / 3 / 4）：这次绘制要的平面没挂，就在它之前切开（重开时按新前瞻挂）
    // The continued render pass may have been opened with fewer depth / stencil attachments (open modes 2 / 3 / 4): if the plane this draw needs is not attached, split before it (on reopen, attach per the new lookahead)
    if (g_rt_merged && g_enc_mode != 0 && (((S.ds.dt || S.ds.dw) && !g_pass.hd) || (S.ds.st && !g_pass.hs))) {
        static int once2; if (!once2++) MLOG("同目标续用：续用通道内绘制要的深度 / 模板平面没挂，在此切开（首例 通道 %llu）", g_stat_pass);
        PD("M %llu 续用通道内绘制要的深度 / 模板平面没挂，在此切开", g_stat_pass); rp_end(); }
    if (g_rt_range_active && (!rt_range_pr_ro(pr) || !S.ds.dt || S.ds.dw || S.ds.st)) rp_end();   // 原encoder边界的外部写可见性不删 | The external write visibility at the original encoder boundary is not removed
    if (g_rt_merged && draw_samples_rt(pr)) { static int once; if (!once++) MLOG("同目标续用：续用通道内绘制要采样附件，在此切开（首例 通道 %llu）", g_stat_pass);
        PD("M %llu 续用通道内绘制要采样附件，在此切开", g_stat_pass); rp_end(); }
    static id<MTLFunction> fz_fn;   // 合并通道后一个通道本次绘制的特化片元函数（nil = 用原版本） | Specialized fragment function for this draw of the later pass in a merged render pass (nil = use the original version)
    if (g_fz.active && g_enc && g_fz.phase == 2 && !fz_p2_draw(pr, dw, di, g_fz.lmap2, &fz_fn)) {   // 证不出：结束合并通道（前一个通道的结果照常写回），按后一个通道的原目标重开 | Cannot be proven: end the merged render pass (the earlier pass's result is stored as usual) and reopen with the later pass's original targets
        static int once3; if (!once3++) MLOG("可读 tile 通道合并：后一个通道的绘制证不出，退回分开（首例 通道 %llu）", g_stat_pass);
        g_fz_fallback++; rp_end(); }
    if (R.has_sc && !g_enc) {
        NSUInteger tw0 = 0, th0 = 0; rt_dims(&tw0, &th0);
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw0) x1 = tw0; if (y1 > (long)th0) y1 = th0;
        if (x1 <= x0 || y1 <= y0) return;
    }
#ifdef NVN_RT_RANGE_TEST
    if (!g_enc) rr_begin();
#endif
    int defer_q_draw = !g_enc && !g_defer_replaying && g_defer_natq > 0;   // Q 开通道的这次绘制：通道照常经 hoist_try / fuse_try 开好后，先回放 P 再画本次 | The draw that opens Q's render pass: after the render pass is opened as usual through hoist_try / fuse_try, replay P first and then draw this one
    if (g_frz_state) goto frz_plain;   // 世界冻结的完整帧 / 冻结帧：不前提、不推迟、不合并（跳通道的状态机要看到每一条设目标） | Full frame / frozen frame of world freeze: no hoisting, no deferral, no merging (the pass-skipping state machine must see every set-target)
    if (!g_enc && tb_sub(TB_HOIST)) hoist_try();   // 先把夹在本通道与其续用之间的纯清除通道提前做掉（rp_begin_ex 之前，不占本通道） | First hoist and execute the pure-clear render passes sandwiched between this render pass and its continuation (before rp_begin_ex, not occupying this render pass)
    // 推迟判定在 hoist_try 之后：被提前的纯清除通道在前瞻里跳过，P 的同目标续段才认得出来
    // The deferral decision comes after hoist_try: hoisted pure-clear render passes are skipped in the lookahead, and only then can P's same-target continuation segment be recognized
    if (!g_enc && tbdr_on() && !defer_has_in_p() && !g_defer_replaying && !g_fz.active && !g_fz_checking && g_exec_next) { defer_try(dw, di); if (g_defer_cur->active && g_defer_cur->in_p) return; }
    if (!g_enc && tb_sub(TB_FUSE) && !g_fz_checking) fuse_try();   // 可读 tile 通道合并缺省开；结构 / 本像素证明不成立仍退回普通通道，CHECK 重画时不重入 | Readable tile render pass merge is on by default; when the structure / same-pixel proof does not hold it still falls back to a normal render pass, and it is not re-entered during the CHECK redraw
frz_plain:;
    int need = g_enc || !tb_sub(TB_DS) ? 3 : pass_ds_need(), mode = defer_q_draw ? 0 : need == 0 ? 2 : need == 2 ? 3 : need == 1 ? 4 : 0;   // 2 都不挂 3 只挂模板 4 只挂深度 | 2 attach neither, 3 attach stencil only, 4 attach depth only
    int skip_d = mode != 0;
    if (!g_enc) { g_fd_dw = dw; g_fd_di = di; }
    id<MTLRenderCommandEncoder> en = rp_begin_ex(mode);
    g_fd_dw = NULL; g_fd_di = NULL;
    if (!en) return;
    if (defer_q_draw) defer_replay();
    if (skip_d) { g_stat_ds_omit++; if (mode == 3) g_stat_s_only++; else if (mode == 4) g_stat_d_only++; }
    // 管线只依赖（程序, 通道目标, 混合 / 颜色 / 写掩码 / 多重采样状态, 顶点布局）：这些没变就沿用上次的
    // The pipeline depends only on (program, render pass targets, blend / color / write mask / multisample state, vertex layout): if these did not change, reuse the previous one
    static id<MTLRenderPipelineState> cur_ps; static NvProg *cur_pr; static uint64_t cur_ps_pass;
    if (g_fz.active && !g_defer_replaying) { g_pso_lmap = g_fz.phase == 1 ? g_fz.lmap1 : g_fz.lmap2; g_pso_fsfn = g_fz.phase == 1 ? nil : fz_fn; g_pso_fz = g_fz.phase == 1 ? (pso_fz_desc){0} : g_fz_desc; g_pso_dirty = 1; }   // 可读 tile 合并：按物理 → 逻辑映射与特化取管线 | Readable tile merge: fetch the pipeline by the physical -> logical mapping and the specialization
    if (g_pso_dirty || pr != cur_pr || cur_ps_pass != g_pass.serial) { cur_ps = pso_get(pr, nil); cur_pr = pr; cur_ps_pass = g_pass.serial; g_pso_dirty = !cur_ps; }
    if (g_pso_lmap) { g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = 1; }   // 下一次绘制重新按自身情况取 | The next draw fetches again according to its own situation
    __unsafe_unretained id<MTLRenderPipelineState> ps = cur_ps;
    if (!ps) return;
    if (ps != g_enc_pso) { enc_r_pso(en, ps); g_enc_pso = ps; }
    static id<MTLDepthStencilState> cur_ds; static uint64_t cur_ds_pass;
    if (g_dss_dirty || cur_ds_pass != g_pass.serial) { cur_ds = dss_get(g_pass.hd, g_pass.hs); cur_ds_pass = g_pass.serial; g_dss_dirty = 0; }
    if (cur_ds != g_enc_dss) { enc_r_dss(en, cur_ds); g_enc_dss = cur_ds; }
    #define FX_SET(bit, same, assign, call) do { if (!g_fx_dedup || !(g_enc_fx.valid & (1u << (bit))) || !(same)) { assign; g_enc_fx.valid |= 1u << (bit); call; } } while (0)
    { uint32_t r = g_rt_ovr && g_pass.hs ? (g_rt_ovr == 3 ? 0x40 : 0) : S.st_ref[0]; FX_SET(7, g_enc_fx.sref == r, g_enc_fx.sref = r, enc_r_stencil_ref(en, r, 0)); }
    // 视口 / 剪裁（夹到目标尺寸内）
    // Viewport / scissor (clamped within the target size)
    NSUInteger tw = g_pass.tw, th = g_pass.th;
    // 界面矩形不贴画面顶边时（iPad 真全屏：界面按 16:9 摆在画面底部，见 dm/host/dm_uiscreen.c），界面绘制整体下移：投影矩阵加纵向偏移（ui_mat_patch），剪裁跟着下移。
    // 界面绘制 = 4 顶点、2 个属性、顶点 20 字节，独占一趟、帧末合成（量过：启动读取、选角、建角、进局读取、局内、开面板、全屏地图都不混别的绘制）。
    // 矩阵这次读不到（本提交里还有没落地的写）时退回挪视口：位置一样，只是贴顶上提的控件这一次会被视口顶边裁掉。
    // 从顶边起的剪裁（y ≤ 0）不下移顶边：宿主把贴顶的控件（小地图、地名等）上提回真实画面顶部时，它们画在界面矩形上方，不能被裁掉。
    // 例外：不带贴图、正好盖满整个界面矩形的那一块（开面板时的压暗、转场的黑幕）改成盖满整个画面——视口纵向放大 1/h、不下移，否则界面矩形外的 3D 留一条明暗分界。
    // 没人登记入口或界面矩形就是整个画面（iPhone、Mac 缺省）时 vo = so = 0、vs = 1、不改矩阵，下面与原来逐字相同。
    // When the UI rectangle does not touch the top edge of the picture (iPad true full screen: the UI is laid out at 16:9 at the bottom of the picture, see dm/host/dm_uiscreen.c), UI draws are shifted down as a whole: a vertical offset is added to the projection matrix (ui_mat_patch), and the scissor moves down with it.
    // A UI draw = 4 vertices, 2 attributes, 20-byte vertices, in a pass of its own and composited at end of frame (measured: startup loading, character select, character creation, entering-game loading, in-game, open panel, and full-screen map never mix in other draws).
    // When the matrix cannot be read this time (there are still unlanded writes in this submit), fall back to moving the viewport: same position, except that widgets lifted to the top get clipped by the viewport's top edge this one time.
    // A scissor starting from the top edge (y <= 0) does not have its top edge moved down: when the host lifts top-anchored widgets (minimap, area name etc.) back to the real top of the picture, they are drawn above the UI rectangle and must not be clipped.
    // Exception: the untextured block that exactly covers the whole UI rectangle (the dimming when a panel opens, the black curtain of a transition) is changed to cover the whole picture -- the viewport is scaled vertically by 1/h and not moved down, otherwise the 3D outside the UI rectangle is left with a light/dark dividing line.
    // When nobody registered the entry or the UI rectangle is the whole picture (iPhone, Mac default), vo = so = 0, vs = 1, the matrix is not changed, and what follows is word-for-word the same as before.
    long vo = 0, so = 0; double vs = 1, moff = 0;
    if (g_ui_rect_fn && dw && dw->count == 4 && S.nattr == 2 && S.stream[0].stride == 20) { double uy = 0, uh = 1; g_ui_rect_fn(&uy, &uh);
        if (uh > 0 && uh < 1) { if (ui_cover_all(uh)) vs = 1 / uh; else if (uy > 0) { so = lround(uy * th); if (ui_mat_ok()) moff = 2 * uy; else { vo = so; static int n; if (n++ < 4) MLOG("界面下移：投影矩阵这次读不到，退回挪视口（通道 %llu）", g_stat_pass); } } } }
    if (moff != g_ui_moff) { g_ui_moff = moff; g_args_dirty[0] = 1; }
    { MTLViewport v = S.has_vp ? (MTLViewport){S.vp[0][0], S.vp[0][1] + vo, S.vp[0][2], S.vp[0][3] * vs, S.dr[0], S.dr[1]} : (MTLViewport){0, vo, tw, th * vs, 0, 1};
      FX_SET(0, !memcmp(&g_enc_fx.vp, &v, sizeof v), g_enc_fx.vp = v, enc_r_viewport(en, v)); }
    MTLScissorRect sc = {0, 0, tw, th};
    if (R.has_sc) {
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y + so, x1 = (long)R.sc.x + R.sc.w, y1 = vs != 1 ? (long)th : (long)R.sc.y + R.sc.h + so;
        if (x1 > (long)tw) x1 = tw; if (y1 > (long)th) y1 = th;
        if (x1 <= x0 || y1 <= y0) return;
        sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    FX_SET(1, !memcmp(&g_enc_fx.sc, &sc, sizeof sc), g_enc_fx.sc = sc, enc_r_scissor(en, sc));
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) return;
    { int c = S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone; FX_SET(2, g_enc_fx.cull == c, g_enc_fx.cull = c, enc_r_cull(en, (MTLCullMode)c)); }
    { int w = S.poly.front == NVN_FRONT_FACE_CCW ? MTLWindingCounterClockwise : MTLWindingClockwise; FX_SET(3, g_enc_fx.wind == w, g_enc_fx.wind = w, enc_r_winding(en, (MTLWinding)w)); }
    { int m = S.poly.mode == NVN_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill; FX_SET(4, g_enc_fx.fill == m, g_enc_fx.fill = m, enc_r_fill(en, (MTLTriangleFillMode)m)); }
    { float d[3] = {0, 0, 0}; if (S.poly.offset_en & 4) { d[0] = S.po.units; d[1] = S.po.factor; d[2] = S.po.clamp; }
      FX_SET(5, !memcmp(g_enc_fx.db, d, sizeof d), memcpy(g_enc_fx.db, d, sizeof d), enc_r_depth_bias(en, d[0], d[1], d[2])); }
    FX_SET(6, !memcmp(g_enc_fx.bc, S.blend_color, sizeof g_enc_fx.bc), memcpy(g_enc_fx.bc, S.blend_color, sizeof g_enc_fx.bc), enc_r_blend_color(en, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]));
    #undef FX_SET
    for (int s_ = 0; s_ < 16; s_++) {
        if (!S.vbuf[s_].address) continue;
        void *mb; uint64_t mo;
        uint64_t gpu;
        if (mtl_gpu_addr(S.vbuf[s_].address, 1, &mb, &mo, &gpu)) { enc_r_vbuf(en, 16 + s_, gpu); use_rec("buf", S.vbuf[s_].address, S.vbuf[s_].size, 0, NULL); }
    }
    static id<MTLBuffer> zero; if (!zero) { zero = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(zero); }
    static uint64_t zero_gpu; if (!zero_gpu) zero_gpu = zero.gpuAddress;
    enc_r_vbuf(en, 30, zero_gpu);
    if (pr->sh[0]) encode_args(en, pr->sh[0], 0, 1);
    if (pr->sh[1]) encode_args(en, pr->sh[1], 1, 0);
    // 参数绑定在登记表锁内捕获实际资源；资格判断到绑定期间有版本变化，则在任何GPU draw前切开重试。
    // Parameter binding captures the actual resources under the registry lock; if the version changed between the eligibility check and the binding, split and retry before any GPU draw.
    if (g_rt_range_active && (g_pass.reg_gen != g_reg_gen || g_pass.ser_gen != g_ser_gen || g_pass.range_dirty != nvn_dirty_now())) { rp_end(); draw(dw, di); return; }
    int prim = dw ? dw->prim : di->prim;
    MTLPrimitiveType mp;
    switch (prim) {
        case 0: mp = MTLPrimitiveTypePoint; break; case 1: mp = MTLPrimitiveTypeLine; break; case 3: mp = MTLPrimitiveTypeLineStrip; break;
        case 4: case 7: mp = MTLPrimitiveTypeTriangle; break; case 5: mp = MTLPrimitiveTypeTriangleStrip; break;
        default: RED_ONCE(19, "图元类型 %d 未实现", prim); return;
    }
    if (g_vis_counting) {   // 遮挡查询计数：本通道新槽（报告 / 清零后也换新槽） | Occlusion query counting: a new slot for this render pass (also switch to a new slot after a report / reset)
        if (!g_enc_has_vis) RED_ONCE(34, "遮挡查询计数开始时所在通道没挂可见性缓冲（首次清零发生在通道中途），该通道的采样不计");
        else {
            if (g_vis_cur < 0 || g_vis_cur_pass != g_pass.serial) {
                if (g_vis_n >= VIS_SLOTS) RED_ONCE(35, "可见性槽用尽");
                else { g_vis_cur = (int)g_vis_n++; g_vis_cur_pass = g_pass.serial;
                    if (g_nvs == g_vscap) { g_vscap = g_vscap ? g_vscap * 2 : 64; g_vs = realloc(g_vs, g_vscap * sizeof *g_vs); }
                    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), (uint32_t)g_vis_cur}; }
            }
            if (g_vis_cur >= 0 && g_enc_vis != g_vis_cur + 1) { enc_r_visibility(en, MTLVisibilityResultModeCounting, (NSUInteger)g_vis_cur * 8); g_enc_vis = g_vis_cur + 1; }
        }
    }
    g_stat_draw++;
    { static int tg; if (!tg) { tg = 1; const char *e = getenv("DM_TEST_GHOST"); float v[8];   // 测试开关 DM_TEST_GHOST=px,py,gx,gy,gw,gh,cx,cy（归一化；按下点、格网、跟手图中心）：不经前端直接开跟手图（Mac 宿主截图验证用） | Test switch DM_TEST_GHOST=px,py,gx,gy,gw,gh,cx,cy (normalized; press point, grid, center of the drag-follow image): turns on the drag-follow image directly without going through the front end (for Mac host screenshot verification)
        if (e && sscanf(e, "%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) mtl_ui_ghost(1, v[0], v[1], &v[2], v[6], v[7]); } }
    if (dw && atomic_load(&g_ghost_on)) ghost_note(dw);
    if (enc_label_on() && !g_el_draws++ && pr->sh[1]) { memcpy(g_el_key, pr->sh[1]->key, 8); g_el_key[8] = 0; }
    if (passdump()) {   // 通道首次绘制的特征（判读入是否可省：全屏、无混合、写满通道、不依赖深度）；DM_NVN_PASSDUMP_ALL 时每次绘制都记 | Characteristics of a render pass's first draw (to judge whether the load can be omitted: full screen, no blending, writes all channels, does not depend on depth); with DM_NVN_PASSDUMP_ALL every draw is recorded
        static uint64_t last_pass = ~0ull; static int all = -1; if (all < 0) all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
        if (all) PD("d %llu bv=%d bi=%d first=%d vs=%s fs=%s vc=%d inst=%d blend=%#x b0=%#llx dt=%d dw=%d df=%d cull=%d st=%d sf=%d,%d sop=%d,%d,%d/%d,%d,%d smask=%#x,%#x/%#x,%#x sref=%#x,%#x ib=%#llx it=%d cmask=%#x", g_stat_pass, di ? di->base_vertex : 0, di ? di->base_instance : 0, dw ? dw->first : 0, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-",
                    dw ? dw->count : di->count, dw ? 1 : di->instances, S.color.blend_en, (unsigned long long)*(uint64_t *)&S.blend[0], (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.poly.cull,
                    (int)S.ds.st, (int)S.ds.sf_f, (int)S.ds.sf_b, (int)S.ds.fail_f, (int)S.ds.zfail_f, (int)S.ds.zpass_f,
                    (int)S.ds.fail_b, (int)S.ds.zfail_b, (int)S.ds.zpass_b, S.st_vmask[0], S.st_vmask[1], S.st_wmask[0], S.st_wmask[1], S.st_ref[0], S.st_ref[1],
                    di ? (unsigned long long)di->ibuf : 0ull, di ? di->itype : 0, S.chmask.mask);
        if (all && pr->sh[0]) {   // 顶点阶段各常量缓冲的内容指纹（判某次绘制跨帧是否完全相同：阴影缓存等用）；读不到（本提交待执行的写）记 P | Content fingerprint of each constant buffer of the vertex stage (to judge whether a draw is completely identical across frames: used by the shadow cache etc.); when unreadable (a write pending execution in this submit) record P
            char ub[400]; size_t o = 0; NvShader *v = pr->sh[0];
            for (int i = 0; i < v->rf.nb && o < sizeof ub - 40; i++) if (v->rf.b[i].kind == NVN_RB_CBUF && v->rf.b[i].reg < 16) {
                const p_bind_buf *u = &S.ubo[0][v->rf.b[i].reg]; void *mb; uint64_t mo; const uint8_t *base; uint64_t h = 1469598103934665603ull;
                if (!u->addr || !u->size) { o += snprintf(ub + o, sizeof ub - o, " r%d=-", v->rf.b[i].reg); continue; }
                if (pend_hits(u->addr, u->size) || !mtl_addr(u->addr, u->size, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) { o += snprintf(ub + o, sizeof ub - o, " r%d=P/%llu", v->rf.b[i].reg, (unsigned long long)u->size); continue; }
                uint64_t h64 = 0; for (uint64_t q = 0; q < u->size; q++) { h = (h ^ base[mo + q]) * 1099511628211ull; if (q == 63) h64 = h; }   // h64：前 64 字节（物体缓冲的世界矩阵 vec0–3） | h64: the first 64 bytes (world matrix vec0–3 of the object buffer)
                { static uint64_t hp = ~0ull; static int hn; if (hp != g_stat_pass) { hp = g_stat_pass; hn = 0; }   // 每通道前 6 次绘制另把 ≤ 608 字节的缓冲按十六进制打出来（逐字节比跨帧变了哪几个字段） | For the first 6 draws of each render pass, additionally print buffers of <= 608 bytes in hex (to compare byte by byte which fields changed across frames)
                  if (hn < 12 && u->size <= 608 && v->rf.b[i].reg == 3) { hn++; char hx[1300]; for (uint64_t q = 0; q < u->size; q++) snprintf(hx + 2 * q, 3, "%02x", base[mo + q]); PD("x %llu r%d %s", g_stat_pass, v->rf.b[i].reg, hx); } }
                o += snprintf(ub + o, sizeof ub - o, " r%d=%016llx/%llu/%016llx", v->rf.b[i].reg, (unsigned long long)h, (unsigned long long)u->size, (unsigned long long)h64); }
            PD("u %llu%s", g_stat_pass, ub); }
        if (all) { char vb[512]; size_t o = 0; o += snprintf(vb + o, sizeof vb - o, "nattr=%d", S.nattr);
            for (int i = 0; i < S.nattr && i < 6; i++) { int st = S.attr[i].stream;
                o += snprintf(vb + o, sizeof vb - o, " a%d{fmt=%#x off=%d s=%d stride=%u div=%u vb=%#llx/%llu}", i, S.attr[i].fmt, S.attr[i].offset, st,
                              st < 16 ? S.stream[st].stride : 0, st < 16 ? S.stream[st].divisor : 0, st < 16 ? (unsigned long long)S.vbuf[st].address : 0ull, st < 16 ? (unsigned long long)S.vbuf[st].size : 0ull); }
            PD("v %llu %s", g_stat_pass, vb); }
        if (last_pass != g_stat_pass) {
            last_pass = g_stat_pass;
            PD("D %llu prim=%d vc=%d inst=%d blend=%#x wm=%#x dt=%d dw=%d df=%d st=%d sc=%lu,%lu,%lu,%lu tgt=%lux%lu vp=%.0f,%.0f,%.0f,%.0f", g_stat_pass, prim,
               dw ? dw->count : di->count, dw ? 1 : di->instances, S.color.blend_en, S.chmask.mask, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.ds.st,
               (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height, (unsigned long)tw, (unsigned long)th,
               S.has_vp ? S.vp[0][0] : 0, S.has_vp ? S.vp[0][1] : 0, S.has_vp ? S.vp[0][2] : (float)tw, S.has_vp ? S.vp[0][3] : (float)th);
        }
    }
    if (rt_range_on() && !rt_range_pr_ro(pr)) g_pass.range_external=1;
    rt_range_note_depth();
#ifdef NVN_RT_RANGE_TEST
    rr_snap(dw,di);
#endif
    udc_note_draw(pr);
    // 0 个图元的绘制（游戏会发 count=0）不画任何像素，跳过编码；Metal 规定计数必须非零，校验层否则当场中止（营地 / 野外 / 战斗页实测）
    // A draw with 0 primitives (the game does send count=0) draws no pixels, so skip encoding; Metal requires the count to be non-zero, otherwise the validation layer aborts on the spot (measured on the camp / wilderness / combat pages)
    if (dw && prim == 7) {   // 四边形：共享索引 0,1,2,0,2,3 | Quads: shared indices 0,1,2,0,2,3
        if (!g_quad_ib) {
            uint32_t n = 65536; uint32_t *ix = malloc(n * 6 * 4);
            for (uint32_t q = 0; q < n; q++) { uint32_t b = q * 4; uint32_t v[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; memcpy(ix + q * 6, v, 24); }
            g_quad_ib = [g_dev newBufferWithBytes:ix length:n * 24 options:MTLResourceStorageModeShared]; g_quad_gpu = g_quad_ib.gpuAddress; res_add(g_quad_ib); free(ix);
        }
        if (dw->count >= 4) enc_r_draw_indexed(en, mp, (dw->count / 4) * 6, MTLIndexTypeUInt32, g_quad_gpu, g_quad_ib.length, 1, dw->first, 0);
    } else if (dw) {
        if (dw->count) enc_r_draw(en, mp, dw->first, dw->count);
    } else {
        if (prim == 7) { RED_ONCE(20, "带索引的四边形未实现"); return; }
        if (di->itype == 0) { RED_ONCE(21, "8 位索引未实现"); return; }
        void *mb; uint64_t mo;
        uint64_t gpu;
        if (!mtl_gpu_addr(di->ibuf, 1, &mb, &mo, &gpu)) { RED_ONCE(22, "索引缓冲不在内存池里"); return; }
        use_rec("buf", di->ibuf, (uint64_t)di->count * (di->itype == 1 ? 2 : 4), 0, NULL);
        if (di->count) enc_r_draw_indexed(en, mp, di->count, di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, gpu, ((__bridge id<MTLBuffer>)mb).length - mo,
                di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
    }
    if (g_rt_range_pending) { PD("j rt_level_range pass=%llu vc=%u", g_stat_pass, (unsigned)(dw ? dw->count : di->count)); g_rt_range_pending=0; }
}
static nvn_hmap g_cps = {.klen = sizeof(void *)};
static void warm_claim(void) {
    if (!atomic_exchange_explicit(&g_warm_ready, 0, memory_order_acq_rel)) return;
    os_unfair_lock_lock(&g_warm_lock);
    NSArray<WarmItem *> *items = [g_warm_items copy];
    [g_warm_items removeAllObjects];
    os_unfair_lock_unlock(&g_warm_lock);
    for (WarmItem *item in items) {
        if (item->kind == 0) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            if (!g_shcache[item->key]) { g_shcache[item->key] = item->shader; atomic_fetch_add(&g_shcache_n, 1); g_warm_claim_shaders++; }
        } else if (item->kind == 1) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            NvShader *vs = g_shcache[item->vs_key], *fs = item->fs_key.length ? g_shcache[item->fs_key] : nil;
            if (!vs) { vs = item->vs; g_shcache[item->vs_key] = vs; atomic_fetch_add(&g_shcache_n, 1); }
            if (item->fs_key.length && !fs) { fs = item->fs; g_shcache[item->fs_key] = fs; atomic_fetch_add(&g_shcache_n, 1); }
            item->pso.vs = (__bridge void *)vs; item->pso.fs = (__bridge void *)fs;
            if (!hm_get_id(&g_pso, &item->pso)) { hm_put_id(&g_pso, &item->pso, item->object); g_warm_claim_rps++; }
        } else {
            NvShader *cs = g_shcache[item->key];
            if (!cs) { cs = item->shader; if (!g_shcache) g_shcache = [NSMutableDictionary dictionary]; g_shcache[item->key] = cs; atomic_fetch_add(&g_shcache_n, 1); }
            const void *key = (__bridge const void *)cs;
            if (!hm_get_id(&g_cps, &key)) { hm_put_id(&g_cps, &key, item->object); g_warm_claim_cps++; }
        }
    }
}
// ---------- 紫闪探针 ----------
// A16 视频：重负载下 3D 画面整块变品红 (255,0,254)、界面照常画在上面，0.35 秒一阵。缺省开（DM_MAGENTA_PROBE_OFF=1 关），只在内测包里看日志：
// 每次出图在上屏命令缓冲末尾把游戏最终画面的 5 个点（中心与四个四分之一点）和调色表 4 格拷进共享缓冲，命令缓冲完成时在回调里判，不等 GPU、不占帧。
// 画面 5 点里 ≥3 点是品红（R≥200、G≤60、B≥200）记【红】并带上调色表读数、加亮系数、显示节拍停回调段数；调色表格子是非数 / 无穷 / 品红单独记【红】；每 3600 次出图记一行汇总（射程）。
// 调色表 = 色调映射查的 32³ 表（见下面 3D 画面加亮），由派发 552f45c348a61b86 那里记下。只走 Metal 4 出图路径（iOS / Mac 正式包）
// ---------- Magenta probe ----------
// A user video on an A16 device: under heavy load the whole 3D picture turns magenta (255,0,254) while the UI still draws on top, in bursts of about 0.35 s. On by default (DM_MAGENTA_PROBE_OFF=1 turns it off); read the logs from test builds:
// for each present, at the end of the present command buffer, copy 5 points of the game's final picture (the center and the four quarter points) and 4 cells of the color table into a shared buffer, and judge them in the completion callback — no GPU wait, no frame cost.
// If ≥3 of the 5 picture points are magenta (R≥200, G≤60, B≥200), log a red line with the color-table readings, the brightening factor and the number of display-link callback stalls; a color-table cell that is NaN / infinite / magenta gets its own red line; log a summary every 3600 presents (coverage).
// Color table = the 32³ table looked up by tone mapping (see the 3D brightening section below), recorded at dispatch 552f45c348a61b86. Only on the Metal 4 present path (iOS / Mac release builds)
static os_unfair_lock g_mp_lk = OS_UNFAIR_LOCK_INIT; static id<MTLTexture> g_mp_lut;
static MTLPixelFormat g_mp_fmt[3]; static int g_mp_haslut[3];
static _Atomic uint64_t g_mp_n, g_mp_mag, g_mp_lutbad;
static uint64_t g_stall_t0, g_stall_n;   // 定义在显示节拍那段 | Defined in the display-link section
float dm_bright_k(void);   // 定义在下面 3D 画面加亮（弱定义，宿主 dm_bright.c 给强定义） | Defined below in the 3D brightening section (weak definition; the host dm_bright.c provides the strong one)
static int mp_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_MAGENTA_PROBE_OFF"); v = !(e && *e && strcmp(e, "0")); } return v; }
static int mp_fmt_ok(MTLPixelFormat f) { return f == MTLPixelFormatRGBA8Unorm || f == MTLPixelFormatRGBA8Unorm_sRGB || f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB || f == MTLPixelFormatRGB10A2Unorm || f == MTLPixelFormatRGBA16Float; }
static void mp_encode(CBUF cb, id<MTLTexture> src, int i) {
#if !DM_NVN_M3
    uint64_t t_start = bp_now();   // 成本测量：CPU 耗时起点 | Cost measurement: CPU time start
    g_mp_fmt[i] = 0; g_mp_haslut[i] = 0;
    if (!mp_on() || !g_mp_buf[i] || !src || src.textureType != MTLTextureType2D || !mp_fmt_ok(src.pixelFormat)) return;
    os_unfair_lock_lock(&g_mp_lk); id<MTLTexture> lut = g_mp_lut; os_unfair_lock_unlock(&g_mp_lk);
    if (lut && (lut.pixelFormat != MTLPixelFormatRGBA16Float || lut.width < 2 || lut.height < 2 || lut.depth < 2)) lut = nil;
    NSUInteger w = src.width, h = src.height, bpp = src.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : 4;
    const NSUInteger px[5][2] = {{w / 2, h / 2}, {w / 4, h / 4}, {w * 3 / 4, h / 4}, {w / 4, h * 3 / 4}, {w * 3 / 4, h * 3 / 4}};
    CENC e = [cb computeCommandEncoder];
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    for (int k = 0; k < 5; k++) enc_b_copy_texture_buffer(e, src, 0, 0, MTLOriginMake(px[k][0], px[k][1], 0), MTLSizeMake(1, 1, 1), g_mp_buf[i], k * 256, bpp, bpp);
    if (lut) { NSUInteger lw = lut.width - 1, lh = lut.height - 1, ld = lut.depth - 1; const NSUInteger q[4][3] = {{0, 0, 0}, {lw / 2, lh / 2, ld / 2}, {lw, lh, ld}, {lw / 2, lh / 4, ld * 3 / 4}};
        for (int k = 0; k < 4; k++) enc_b_copy_texture_buffer(e, lut, 0, 0, MTLOriginMake(q[k][0], q[k][1], q[k][2]), MTLSizeMake(1, 1, 1), g_mp_buf[i], (5 + k) * 256, 8, 8);
        g_mp_haslut[i] = 1; }
    m4_end(e);
    g_mp_fmt[i] = src.pixelFormat;
    // 成本测量：每次调用记 CPU 耗时（ns），每 600 次汇总
    // Cost measurement: record CPU time (ns) per call, summarize every 600 calls
    static uint64_t cost_sum, cost_n, cost_max; uint64_t cost_ns = bp_now() - t_start;
    cost_sum += cost_ns; cost_n++; if (cost_ns > cost_max) cost_max = cost_ns;
    if (cost_n % 600 == 0) { MLOG("紫闪探针 CPU 成本：前 %llu 次，平均 %.3f μs / 峰值 %.3f μs", (unsigned long long)cost_n, cost_sum / (double)cost_n / 1000, cost_max / 1000.0); }
#else
    (void)cb; (void)src; (void)i;
#endif
}
static void mp_px(MTLPixelFormat f, const uint8_t *p, int *r, int *g, int *b) {
    if (f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB) { *r = p[2]; *g = p[1]; *b = p[0]; }
    else if (f == MTLPixelFormatRGB10A2Unorm) { uint32_t v; memcpy(&v, p, 4); *r = (v & 1023) >> 2; *g = ((v >> 10) & 1023) >> 2; *b = ((v >> 20) & 1023) >> 2; }
    else if (f == MTLPixelFormatRGBA16Float) { __fp16 hv[4]; memcpy(hv, p, 8); float c[3] = {hv[0], hv[1], hv[2]}; int *o[3] = {r, g, b};
        for (int k = 0; k < 3; k++) { float x = c[k] != c[k] ? 0 : c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k]; *o[k] = (int)(x * 255 + 0.5f); } }
    else { *r = p[0]; *g = p[1]; *b = p[2]; }
}
static void mp_check(int i, uint64_t ticket) {
    MTLPixelFormat f = g_mp_fmt[i]; if (!f) return;
    const uint8_t *m = g_mp_buf[i].contents; char pt[96] = "", lt[160] = "无"; int o = 0, mag = 0, lutbad = 0;
    for (int k = 0; k < 5; k++) { int r, g, b; mp_px(f, m + k * 256, &r, &g, &b); mag += r >= 200 && g <= 60 && b >= 200; o += snprintf(pt + o, sizeof pt - o, " %02x%02x%02x", r, g, b); }
    if (g_mp_haslut[i]) { o = 0; for (int k = 0; k < 4; k++) { __fp16 hv[4]; memcpy(hv, m + (5 + k) * 256, 8); float c[3] = {hv[0], hv[1], hv[2]};
        int bad = 0; for (int j = 0; j < 3; j++) bad |= c[j] != c[j] || isinf(c[j]); bad |= c[0] > 0.8f && c[1] < 0.2f && c[2] > 0.8f; lutbad |= bad;
        o += snprintf(lt + o, sizeof lt - o, "%s(%.3f,%.3f,%.3f)", k ? " " : "", c[0], c[1], c[2]); } }
    uint64_t n = atomic_fetch_add(&g_mp_n, 1) + 1, nm = mag >= 3 ? atomic_fetch_add(&g_mp_mag, 1) + 1 : atomic_load(&g_mp_mag), nl = lutbad ? atomic_fetch_add(&g_mp_lutbad, 1) + 1 : atomic_load(&g_mp_lutbad);
    if (mag >= 3 && (nm <= 30 || nm % 50 == 0)) MLOG("【红】紫闪探针：出图 %llu 画面品红（5 点%s，%d 点品红）；调色表 4 格 %s；加亮 k=%.2f；显示节拍停回调累计 %llu 段，此刻%s停顿中；累计品红 %llu 次",
        (unsigned long long)ticket, pt, mag, lt, dm_bright_k(), (unsigned long long)g_stall_n, g_stall_t0 ? "在" : "不在", (unsigned long long)nm);
    if (lutbad && (nl <= 30 || nl % 50 == 0)) MLOG("【红】紫闪探针：出图 %llu 调色表格子异常（非数 / 无穷 / 品红）%s；画面 5 点%s；累计 %llu 次", (unsigned long long)ticket, lt, pt, (unsigned long long)nl);
    if (n % 3600 == 0 || n == 600) MLOG("紫闪探针 至出图 %llu：取样 %llu 次，画面品红 %llu 次，调色表异常 %llu 次（本次 5 点%s，调色表 %s）", (unsigned long long)ticket, (unsigned long long)n, (unsigned long long)nm, (unsigned long long)nl, pt, lt);
}
// ---------- 3D 画面加亮 ----------
// 色调映射（片元 a1b1aab66933fa8f）= 把合成后的 HDR ×0.008 做 PQ 编码、再查 32³ 调色表（每帧由计算着色器 552f45c348a61b86 按区域重算，只被色调映射读）。
// HDR 先乘曝光 k 等价于把表换成 L'(u) = L(PQ(k · PQ⁻¹(u)))：游戏算完表的那次派发之后，拷一份再按此原地重写（32³ 一次小派发）；表每帧重算，不会越乘越亮。
// 只动 3D 世界：界面、文字、血球在色调映射之后才合；亮部经 PQ 压缩，不会像最后整屏乘系数那样糊成一片白。普通 / 可读 tile 合并 / 冻结各条路径都只是读这张表，一处生效。
// k 每帧由宿主 dm_bright_k() 给（dm/host/dm_bright.c：局外 1、迷宫类 4、原版画面不随昼夜 4、其余按原版环境亮度推，平滑过渡）；k ≈ 1 时不重写。
// ---------- 3D picture brightening ----------
// Tone mapping (fragment a1b1aab66933fa8f) = PQ-encode the composited HDR x0.008, then look up a 32^3 color lookup table (recomputed per region every frame by compute shader 552f45c348a61b86, read only by tone mapping).
// Multiplying HDR by exposure k first is equivalent to replacing the table with L'(u) = L(PQ(k * PQ^-1(u))): after the dispatch in which the game finishes computing the table, make a copy and rewrite it in place accordingly (one small 32^3 dispatch); the table is recomputed every frame, so it does not get brighter with each multiplication.
// Only the 3D world is touched: UI, text and the health orbs are composited after tone mapping; highlights are compressed by PQ and do not blur into a sheet of white as a final whole-screen multiply would. The normal / readable tile merge / freeze paths all just read this table, so it takes effect in one place.
// k is given every frame by the host's dm_bright_k() (dm/host/dm_bright.c: 1 out of game, 4 for maze-type areas, 4 where the original game's picture does not follow day and night, the rest derived from the original game's ambient brightness, with smooth transitions); no rewrite when k ~= 1.
__attribute__((weak)) float dm_bright_k(void) { return 1.0f; }   // 宿主 dm_bright.c 提供强定义；图形单测不链宿主时不加亮 | The host's dm_bright.c provides the strong definition; no brightening when graphics unit tests do not link the host
static const char k_lut_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"constant float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"   // 与色调映射着色器里的 PQ 常数逐位相同 | Bit-identical to the PQ constants in the tone mapping shader
"static float3 pq(float3 v) { float3 p = pow(abs(v), m1); return pow((c1 + c2 * p) / (1.0 + c3 * p), m2); }\n"
"static float3 pq_inv(float3 u) { float3 q = pow(u, 1.0 / m2); return pow(max(q - c1, 0.0) / max(c2 - c3 * q, 1e-6), 1.0 / m1); }\n"
"kernel void lutk(texture3d<float> src [[texture(0)]], texture3d<float, access::write> dst [[texture(1)]], constant float &k [[buffer(0)]], uint3 g [[thread_position_in_grid]]) {\n"
"  uint3 n = uint3(dst.get_width(), dst.get_height(), dst.get_depth()); if (any(g >= n)) return;\n"
"  constexpr sampler ls(filter::linear, address::clamp_to_edge, coord::normalized);\n"
"  float3 u = (float3(g) + 0.5) / float3(n); float3 u2 = clamp(pq(k * pq_inv(u)), 0.0, 1.0);\n"
"  float4 b = src.sample(ls, u2), o = src.read(g);\n"   // b：按曝光 k 提亮后的输出；o：原版这一格 | b: output after brightening by exposure k; o: this cell in the original game
"  float s = smoothstep(0.65, 1.0, dot(b.rgb, float3(0.2126, 0.7152, 0.0722)));\n"   // 高光保护：提亮后越接近白，越收回原值（雪、天光不糊成一片白）；暗处 s = 0 照常提亮 | Highlight protection: after brightening, the closer to white, the more it is pulled back to the original value (snow and skylight do not blur into a sheet of white); in dark areas s = 0 and brightening proceeds as usual
"  dst.write(float4(max(mix(b.rgb, o.rgb, s), o.rgb), b.a), g); }\n";
static void bright_lut(NvShader *cs) {
    static int failed; static id<MTLComputePipelineState> ps; static id<MTLTexture> tmp; static id<MTLBuffer> kb[3]; static int ki;
#if !DM_NVN_M3
    static id<MTL4ArgumentTable> at;
#endif
    if (failed || strncmp(cs->key, "552f45c348a61b86", 16)) return;
    int tid = 0; for (int i = 0; i < cs->rf.nb && !tid; i++) if (cs->rf.b[i].kind == NVN_RB_IMG && cs->rf.b[i].reg < 32) tid = (int)(S.img[5][cs->rf.b[i].reg] & 0xfffff);
    pthread_mutex_lock(&g_rmu); id<MTLTexture> lut = tid ? tex_by_id(tid) : nil; pthread_mutex_unlock(&g_rmu);
    if (lut && lut.textureType == MTLTextureType3D) { os_unfair_lock_lock(&g_mp_lk); g_mp_lut = lut; os_unfair_lock_unlock(&g_mp_lk); }   // 紫闪探针在出图时取这张表的几格 | The magenta probe samples a few cells of this table at present time
    float k = dm_bright_k(); if (!(k > 1.005f)) return;   // k ≈ 1（局外 / 正午）：表保持原版 | k ~= 1 (out of game / noon): the table stays as in the original game
    if (k > 4.0f) k = 4.0f;
    if (!lut || lut.textureType != MTLTextureType3D || !(lut.usage & MTLTextureUsageShaderWrite)) { static int n; if (!n++) MLOG("【红】3D 画面加亮：调色表认不出（贴图号 %d），不加亮", tid); return; }
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_lut_msl) options:nil error:&e]; id<MTLFunction> f = lib ? [lib newFunctionWithName:@"lutk"] : nil;
        ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
#if !DM_NVN_M3
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 2; ad.maxBufferBindCount = 1; at = [g_dev newArgumentTableWithDescriptor:ad error:&e];
#endif
        for (int i = 0; i < 3; i++) { kb[i] = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; if (kb[i]) res_add(kb[i]); }
        if (!ps || M4_ONLY(!at ||) !kb[0] || !kb[1] || !kb[2]) { failed = 1; MLOG("【红】3D 画面加亮：计算管线建不出 %s（退回原版亮度）", e.localizedDescription.UTF8String); return; }
        MLOG("3D 画面加亮：建好（调色表 %lux%lux%lu，曝光每帧由 dm_bright_k 给）", (unsigned long)lut.width, (unsigned long)lut.height, (unsigned long)lut.depth); }
    if (!tmp || tmp.width != lut.width || tmp.height != lut.height || tmp.depth != lut.depth || tmp.pixelFormat != lut.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new]; d.textureType = MTLTextureType3D; d.pixelFormat = lut.pixelFormat; d.width = lut.width; d.height = lut.height; d.depth = lut.depth;
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; if (tmp) defer_obj_release(tmp); tmp = [g_dev newTextureWithDescriptor:d]; if (!tmp) { failed = 1; return; } res_add(tmp); }
    id<MTLBuffer> b0 = kb[ki]; ki = (ki + 1) % 3; *(float *)b0.contents = k;   // 三份轮换：在途的帧还在读上一份 | Three copies in rotation: in-flight frames are still reading the previous copy
    BENC b = blit(); enc_b_copy_texture(b, lut, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(lut.width, lut.height, lut.depth), tmp, 0, 0, MTLOriginMake(0, 0, 0)); enc_b_end(g_blit); g_blit = nil;
    CENC ce = (CENC)enc_c_begin(NULL);
#if DM_NVN_M3
    [ce setTexture:tmp atIndex:0]; [ce setTexture:lut atIndex:1]; [ce setBuffer:b0 offset:0 atIndex:0];
#else
    [ce setArgumentTable:at]; [at setTexture:tmp.gpuResourceID atIndex:0]; [at setTexture:lut.gpuResourceID atIndex:1]; [at setAddress:b0.gpuAddress atIndex:0];
#endif
    enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake((lut.width + 3) / 4, (lut.height + 3) / 4, (lut.depth + 3) / 4), MTLSizeMake(4, 4, 4)); enc_c_end(ce);
    args_dirty_all();
}
static void dispatch(const p_dispatch *dp) {
    zskip_clear();
    if (!S.prog) { RED_ONCE(27, "派发前没有绑定程序"); return; }
    NvShader *cs = prog_get(S.prog)->sh[5];
    if (!cs) { RED_ONCE(28, "派发的程序没有计算着色器（或未映射）"); return; }
    const void *k = (__bridge const void *)cs;
    id<MTLComputePipelineState> ps = hm_get_id(&g_cps, &k);
    if (!ps) {
        NSError *e = nil; uint64_t bt = bp_now(); ps = [g_dev newComputePipelineStateWithFunction:cs->fn error:&e];
        g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
        if (!ps) { MLOG("【红】建计算管线失败：%s", e.localizedDescription.UTF8String); return; }
        pso_record("C\t%s\n", cs->key);
        if (cs->key[0]) pso_user_record("C\t%s\n", cs->key);
        hm_put_id(&g_cps, &k, ps);
    }
    if (cs->builtin) {   // 内嵌计算核：n = uniform 缓冲 0，src = 存储缓冲 0，dst = 存储缓冲 1 | Built-in compute kernel: n = uniform buffer 0, src = storage buffer 0, dst = storage buffer 1
        const p_bind_buf *u = &S.ubo[5][0], *a0 = &S.ssbo[5][0], *a1 = &S.ssbo[5][1];
        void *ub, *sb, *db; uint64_t uo, so, dof;
        if (!mtl_addr(u->addr, 4, &ub, &uo) || !mtl_addr(a0->addr, 8, &sb, &so) || !mtl_addr(a1->addr, 8, &db, &dof)) { RED_ONCE(30, "内嵌计算核 %s 的缓冲没绑全", cs->builtin); return; }
        if (!cs->logged++) {
            module_t *m = module_of((void *)dp->caller);
            MLOG("内嵌计算核 %s 首次派发：调用点 %s+%#llx，线程组 %dx%dx%d，元素数（CPU 侧当前值）%u", cs->builtin, m ? m->name : "?",
                 m ? (unsigned long long)(dp->caller - (uint64_t)m->base) : (unsigned long long)dp->caller, dp->x, dp->y, dp->z, *(uint32_t *)u->addr);
        }
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTLComputeCommandEncoder> ce = pg_compute();
        enc_c_pso(ce, ps);
        enc_c_args(ce, ((__bridge id<MTLBuffer>)ub).gpuAddress + uo, 0); enc_c_args(ce, ((__bridge id<MTLBuffer>)sb).gpuAddress + so, 1); enc_c_args(ce, ((__bridge id<MTLBuffer>)db).gpuAddress + dof, 2);   // 内嵌计算核：冷路径，直接取地址 | Built-in compute kernel: cold path, take the addresses directly
        enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(32, 1, 1));
        pg_compute_end(ce); enc_c_end(ce);
        return;
    }
    const uint16_t *tg = cs->rf.tg;
    if (!tg[0]) { RED_ONCE(29, "计算着色器缺线程组大小反射"); return; }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLComputeCommandEncoder> ce = pg_compute();
    enc_c_pso(ce, ps);
    encode_args(ce, cs, 5, 0);
    enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(tg[0], tg[1], tg[2]));
    pg_compute_end(ce); enc_c_end(ce);
    bright_lut(cs);   // 3D 画面加亮：游戏算完调色表之后重写 | 3D picture brightening: rewrite after the game finishes computing the color lookup table
}
// GPU：贴图 → 缓冲（行距取 SetCopyRowStride，0 = 紧排）
// GPU: texture -> buffer (row stride taken from SetCopyRowStride, 0 = tightly packed)
static void copy_t2b(const p_copy_b2t *c) {
    if (c->tex) xv_check(c->tex->serial, c->has_view && c->view.set_levels ? c->view.base_level : 0, c->has_view && c->view.set_layers ? c->view.min_layer : 0, "贴图读缓冲");
    id<MTLTexture> t = tex_mtl(c->tex);
    if (!t) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row, img, extent;
    if (!nvn_copy_pitch(c->r.width, c->r.height, c->r.depth, bw, bh, bpb,
                        S.copy_stride, S.copy_image_stride, &row, &img, &extent)) {
        MLOG("Invalid texture copy pitch"); return;
    }
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, extent, &mb, &mo)) { RED_ONCE(31, "贴图→缓冲拷贝的目标不在内存池里"); return; }
    BENC b = blit();
    if (is3d)
        enc_b_copy_texture_buffer(b, t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset), MTLSizeMake(c->r.width, c->r.height, nz), (__bridge id<MTLBuffer>)mb, mo, row, img);
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture_buffer(b, t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0), MTLSizeMake(c->r.width, c->r.height, 1), (__bridge id<MTLBuffer>)mb, mo + z * img, row, img);
}
// CPU 读贴图（ReadTexels）：预排布贴图以池内存为真值，解排布直读；其余从 MTLTexture blit 读回（调用方已按 NVN 规矩等过 GPU）
// CPU texture read (ReadTexels): for packaged textures the pool memory is the ground truth, de-layout and read directly; the rest are read back from the MTLTexture by blit (the caller has already waited for the GPU per NVN rules)
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    if (!on_sub_thread()) while (atomic_load(&g_up_pending)) usleep(200);   // 翻译在翻译线程：调用方已排空队列（批已随提交冲刷），这里只等上传在 GPU 上做完（提交线程上由 q_cb 冲刷并等） | Translation runs on the translation thread: the caller has already drained the queue (batches were flushed with the submit), so here we only wait for uploads to finish on the GPU (on the submit thread q_cb flushes and waits)
    xv_check(t->serial, level, layer0, "CPU读回");
    PD("U cpu_read ser=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d dst=%p row=%lu img=%lu", t->serial, level, layer0, r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth, dst, (unsigned long)row, (unsigned long)img);
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    if (t->has_layout && t->mem) {
        uint32_t lw = MAX(t->d.width >> level, 1u), lh = MAX(t->d.height >> level, 1u), lz = is3d ? MAX(t->d.depth >> level, 1u) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        for (int z = 0; z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { RED_ONCE(32, "ReadTexels 解排布失败"); free(lin); return; }
            const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, src + (y0 + y) * lp + x0, wb);
        }
        free(lin);
        return;
    }
    id<MTLTexture> mt = tex_mtl(t);
    if (!mt) { memset(dst, 0, img * nz); return; }
    mtl_finish_gpu();
    id<MTLBuffer> buf = [g_dev newBufferWithLength:wb * hb * nz options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = enc_b_begin(cb);
    for (int z = 0; z < nz; z++)
        [b copyFromTexture:mt sourceSlice:is3d ? 0 : layer0 + r->zoffset + z sourceLevel:level sourceOrigin:MTLOriginMake(r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0)
                sourceSize:MTLSizeMake(r->width, r->height, 1) toBuffer:buf destinationOffset:z * wb * hb destinationBytesPerRow:wb destinationBytesPerImage:wb * hb];
    enc_b_end(b); [cb commit]; [cb waitUntilCompleted];
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, (uint8_t *)buf.contents + z * wb * hb + y * wb, wb);
}
static void copy_b2t(const p_copy_b2t *c) {
    if (c->tex) zskip_del(c->tex->serial);
    id<MTLTexture> t = tex_mtl(c->tex);
    PD("C ser=%llu", c->tex->serial);
    if (!t) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    size_t row, img, extent;
    if (!nvn_copy_pitch(c->r.width, c->r.height, c->r.depth, bw, bh, bpb,
                        S.copy_stride, S.copy_image_stride, &row, &img, &extent)) {
        MLOG("Invalid texture copy pitch"); return;
    }
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, extent, &mb, &mo)) { RED_ONCE(23, "缓冲→贴图拷贝的源不在内存池里"); return; }
    uint32_t upf; int ubpp;
    if (bc_unpack(c->tex->d.format, &upf, &ubpp)) {   // 无 BC：源里的压缩块在 CPU 上解压进临时缓冲，再按像素拷进解压格式的贴图 | No BC: the compressed blocks in the source are decompressed on the CPU into a temporary buffer, then copied pixel-wise into the decompressed-format texture
        const uint8_t *src = [(__bridge id<MTLBuffer>)mb contents];
        if (!src) { RED_ONCE(46, "无 BC 解压：缓冲→贴图拷贝的源缓冲 CPU 读不到"); return; }
        size_t up = (size_t)c->r.width * ubpp, uimg = up * c->r.height;
        id<MTLBuffer> tb = [g_dev newBufferWithLength:uimg * nz ?: 16 options:MTLResourceStorageModeShared];
        for (int z = 0; z < nz; z++) nvn_bc_decode(c->tex->d.format, src + mo + z * img, row, c->r.width, c->r.height, (uint8_t *)tb.contents + z * uimg, up);
        res_add(tb);
        BENC b = blit();
        if (is3d) enc_b_copy_buffer_texture(b, tb, 0, up, uimg, MTLSizeMake(c->r.width, c->r.height, nz), t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset));
        else for (int z = 0; z < nz; z++) enc_b_copy_buffer_texture(b, tb, z * uimg, up, uimg, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
        defer_obj_release(tb);   // 本次提交跑完再放（并撤驻留） | Release after this submit has finished running (and remove from residency)
        return;
    }
    BENC b = blit();
    if (is3d)
        enc_b_copy_buffer_texture(b, (__bridge id<MTLBuffer>)mb, mo, row, img, MTLSizeMake(c->r.width, c->r.height, nz), t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_buffer_texture(b, (__bridge id<MTLBuffer>)mb, mo + z * img, row, img, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
}

// 无 BC + 老机型补充包：游戏把文件来的贴图（已是 ASTC 4x4）整块拷进自己建的 BC3 纹理数组（无 BC 时建成 RGBA8）。Switch 上两种格式块大小相同、
// 拷贝是按原始块搬；这里改成计算核逐像素解出来写进去，sRGB 源先读出线性值再编码回 sRGB 值，结果 = 原样搬块后按目标格式读到的值
// No BC + Legacy Device Pack: the game copies textures from files (already ASTC 4x4) whole into a BC3 texture array it creates itself (created as RGBA8 when there is no BC). On Switch the two formats have the same block size and
// the copy moves raw blocks; here it is changed to a compute kernel that decodes pixel by pixel and writes them in, with sRGB sources first read out as linear values and then encoded back to sRGB values, so the result = the values read in the destination format after moving the blocks as-is
static const char k_a2u_msl[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"static float3 enc(float3 c) { return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308); }\n"
"kernel void a2u(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], constant uint4 *p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= p[1].x || g.y >= p[1].y) return; float4 v = src.read(g + p[0].xy); if (p[1].z) v.rgb = enc(v.rgb); dst.write(v, g + p[0].zw); }\n";
static int copy_t2t_astc(id<MTLTexture> s, id<MTLTexture> d, unsigned sl, unsigned ss, unsigned dl, unsigned ds, int sx, int sy, int dx, int dy, int w, int h) {
    static int failed; static id<MTLComputePipelineState> ps;
#if !DM_NVN_M3
    static id<MTL4ArgumentTable> at;
#endif
    if (failed) return 0;
    if (!ps) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_a2u_msl) options:nil error:&e]; id<MTLFunction> f = lib ? [lib newFunctionWithName:@"a2u"] : nil;
        ps = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
#if !DM_NVN_M3
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 2; ad.maxBufferBindCount = 1; at = [g_dev newArgumentTableWithDescriptor:ad error:&e];
#endif
        if (!ps M4_ONLY(|| !at)) { failed = 1; MLOG("【红】无 BC：ASTC → 解压格式拷贝的计算管线建不出 %s", e.localizedDescription.UTF8String); return 0; } }
    MTLTextureType tt = MTLTextureType2D;
    id<MTLTexture> sv = [s newTextureViewWithPixelFormat:s.pixelFormat textureType:tt levels:NSMakeRange(sl, 1) slices:NSMakeRange(ss, 1)];
    id<MTLTexture> dv = [d newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:tt levels:NSMakeRange(dl, 1) slices:NSMakeRange(ds, 1)];
    id<MTLBuffer> pb = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    if (!sv || !dv || !pb) { RED_ONCE(47, "无 BC：ASTC → 解压格式拷贝建不出视图 / 参数块"); return 0; }
    uint32_t *p = pb.contents; p[0] = sx; p[1] = sy; p[2] = dx; p[3] = dy; p[4] = w; p[5] = h; p[6] = s.pixelFormat == MTLPixelFormatASTC_4x4_sRGB; p[7] = 0;
    res_add(pb);
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    CENC ce = (CENC)enc_c_begin(NULL);
#if DM_NVN_M3
    [ce setTexture:sv atIndex:0]; [ce setTexture:dv atIndex:1]; [ce setBuffer:pb offset:0 atIndex:0];
#else
    [ce setArgumentTable:at];
    [at setTexture:sv.gpuResourceID atIndex:0]; [at setTexture:dv.gpuResourceID atIndex:1]; [at setAddress:pb.gpuAddress atIndex:0];
#endif
    enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake((w + 7) / 8, (h + 7) / 8, 1), MTLSizeMake(8, 8, 1)); enc_c_end(ce);
    args_dirty_all();
    defer_obj_unref(sv); defer_obj_unref(dv); defer_obj_release(pb);   // 本次提交跑完再放（视图不单独驻留，参数块撤驻留） | Release after this submit has finished running (views are not made resident separately; the parameter block is removed from residency)
    return 1;
}
// GPU：贴图 → 贴图（原先误列为空操作，选角页每帧 3 次）。等尺寸、不镜像、同格式 → 逐层 blit；其余语义（缩放带过滤、镜像、换格式）blit 表达不了，报红待实现
// GPU: texture -> texture (previously mislisted as a no-op, 3 times per frame on the character select page). Same size, not mirrored, same format -> blit layer by layer; the other semantics (scaling with filtering, mirroring, format change) cannot be expressed by blit: report a failure, to be implemented
static void copy_t2t(const p_copy_t2t *c) {
    if (c->src) xv_check(c->src->serial, c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, "贴图拷贝源");
    rp_flush();   // 挂起的纯清除通道先落地（零源候选在通道结束时才进集合）；下面 blit() 本来也会结束通道 | Land the pending pure-clear render pass first (zero-source candidates only enter the set when the render pass ends); the blit() below would end the render pass anyway
    if (c->dst) zskip_del(c->dst->serial);
    if (c->src && c->dst && c->sr.xoffset == 0 && c->sr.yoffset == 0 && c->dr.xoffset == 0 && c->dr.yoffset == 0
        && c->sr.width == (int)c->src->d.width && c->sr.height == (int)c->src->d.height && c->dr.width == (int)c->dst->d.width && c->dr.height == (int)c->dst->d.height) {
        int sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
        int ss = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, ds = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
        if (zskip_has(c->src->serial, sl, ss)) zskip_add(c->dst->serial, dl, ds);
    }
    id<MTLTexture> s = tex_mtl(c->src), d = tex_mtl(c->dst);
    PD("T src=%llu dst=%llu %dx%dx%d→%dx%dx%d flags=%#x", c->src->serial, c->dst->serial, c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, c->flags);
    if (!s || !d) return;
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    PD("t src=%llu lv%u sl%u xyz=%d,%d,%d dst=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d", c->src->serial, sl, sz0, c->sr.xoffset, c->sr.yoffset, c->sr.zoffset,
       c->dst->serial, dl, dz0, c->dr.xoffset, c->dr.yoffset, c->dr.zoffset, c->sr.width, c->sr.height, c->sr.depth);
    int s3 = c->src->d.target == 2, d3 = c->dst->d.target == 2, nz = c->sr.depth ? c->sr.depth : 1;
    if (no_bc() && (s.pixelFormat == MTLPixelFormatASTC_4x4_LDR || s.pixelFormat == MTLPixelFormatASTC_4x4_sRGB) && (d.pixelFormat == MTLPixelFormatRGBA8Unorm || d.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB)
        && !s3 && !d3 && c->sr.width == c->dr.width && c->sr.height == c->dr.height && (d.usage & MTLTextureUsageShaderWrite)) {   // 无 BC：ASTC 源 → 解压格式目标（见 copy_t2t_astc） | No BC: ASTC source -> decompressed-format destination (see copy_t2t_astc)
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) ok = copy_t2t_astc(s, d, sl, sz0 + c->sr.zoffset + z, dl, dz0 + c->dr.zoffset + z, c->sr.xoffset, c->sr.yoffset, c->dr.xoffset, c->dr.yoffset, c->sr.width, c->sr.height);
        if (ok) return;
    }
    if (c->sr.width != c->dr.width || c->sr.height != c->dr.height || (c->sr.depth ? c->sr.depth : 1) != (c->dr.depth ? c->dr.depth : 1) || (c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT & ~NVN_COPY_FLAGS_LINEAR_FILTER_BIT)
        || s.pixelFormat != d.pixelFormat || s3 != d3) {
        RED_ONCE(33, "贴图→贴图拷贝未实现的形态：%dx%dx%d→%dx%dx%d 标志 %#x 格式 %lu→%lu 3D %d→%d", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth,
                 c->flags, (unsigned long)s.pixelFormat, (unsigned long)d.pixelFormat, s3, d3);
        return;
    }
    BENC b = blit();
    if (s3)
        enc_b_copy_texture(b, s, 0, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, c->sr.zoffset), MTLSizeMake(c->sr.width, c->sr.height, nz), d, 0, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, c->dr.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture(b, s, sz0 + c->sr.zoffset + z, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, 0), MTLSizeMake(c->sr.width, c->sr.height, 1), d, dz0 + c->dr.zoffset + z, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, 0));
}

// GPU：用给定值清贴图区域（原先误列为空操作，初始化调 5 次；Metal 私有贴图初值未定义，游戏依赖清出的初值）。
// 整级整面、四通道全写 → 每个层 / 深度切片开一个只清不画的通道（硬件按贴图格式换算清除值，含整数、深度）；
// 视图换格式：在该格式的视图上清；部分区域、部分通道、压缩 / 预排布贴图（无渲染目标用途）：报红待实现
// GPU: clear a texture region with the given value (previously mislisted as a no-op, called 5 times at initialization; the initial value of a Metal private texture is undefined, and the game relies on the cleared initial value).
// Whole level and whole face, all four channels written -> open one clear-only render pass per layer / depth slice (the hardware converts the clear value by the texture format, including integer and depth);
// view with changed format: clear on a view of that format; partial region, partial channels, compressed / packaged textures (no render target usage): report a failure, to be implemented
static void clear_tex(const p_clear_tex *c) {
    if (c->tex) zskip_del(c->tex->serial);
    id<MTLTexture> t = tex_mtl(c->tex);
    PD("X ser=%llu 视图格式 %#x 区域 %d,%d,%d %dx%dx%d 掩码 %#x %s %g,%g,%g,%g", c->tex->serial, c->has_view && c->view.set_format ? c->view.format : -1, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth, c->mask,
       c->is_uint ? "ui" : "f", c->is_uint ? (double)c->c.u[0] : c->c.f[0], c->is_uint ? (double)c->c.u[1] : c->c.f[1], c->is_uint ? (double)c->c.u[2] : c->c.f[2], c->is_uint ? (double)c->c.u[3] : c->c.f[3]);
    if (!t) return;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
    int is3d = t.textureType == MTLTextureType3D, nz = c->r.depth ? c->r.depth : 1;
    if (c->has_view && c->view.set_format) {   // 视图换格式：清除值按视图格式解释（游戏 5 次全是 ui 版配整数视图），在同格式视图上清 | View with changed format: the clear value is interpreted in the view format (all 5 of the game's calls are the ui version with an integer view), and the clear is done on a view of the same format
        uint32_t vpf = 0; uint8_t swz[4];
        if (!nvn_fmt_to_mtl(c->view.format, &vpf, swz) || !vpf) { RED_ONCE(42, "清贴图视图格式 %#x 无对应", c->view.format); return; }
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf) && !(t.usage & MTLTextureUsagePixelFormatView)) t = tex_upgrade_pfv(c->tex, t, (MTLPixelFormat)vpf);
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf)) {
            id<MTLTexture> v = [t newTextureViewWithPixelFormat:(MTLPixelFormat)vpf];
            if (!v) { RED_ONCE(43, "清贴图建不出视图：格式 %lu → %u", (unsigned long)t.pixelFormat, vpf); return; }
            t = v;
        }
    }
    MTLPixelFormat pf = t.pixelFormat;
    int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8, stencil = pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatDepth32Float_Stencil8;
    if (c->r.xoffset || c->r.yoffset || (NSUInteger)c->r.width != w || (NSUInteger)c->r.height != h || (c->mask & 0xf) != 0xf
        || !(t.usage & MTLTextureUsageRenderTarget)) {
        RED_ONCE(44, "清贴图未实现的形态：区域 %d,%d %dx%d（本级 %lux%lu）掩码 %#x 可作渲染目标 %d", c->r.xoffset, c->r.yoffset, c->r.width, c->r.height,
                 (unsigned long)w, (unsigned long)h, c->mask, !!(t.usage & MTLTextureUsageRenderTarget));
        return;
    }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0; z < nz; z++) {
        RPD *rp = [RPD new];
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : stencil ? rp.stencilAttachment : rp.colorAttachments[0];
        a.texture = t; a.level = lv; a.loadAction = MTLLoadActionClear; a.storeAction = MTLStoreActionStore;
        if (is3d) a.depthPlane = c->r.zoffset + z; else a.slice = l0 + c->r.zoffset + z;
        if (depth) { rp.depthAttachment.clearDepth = c->c.f[0];
            if (stencil) { rp.stencilAttachment.texture = t; rp.stencilAttachment.level = lv; rp.stencilAttachment.slice = a.slice; rp.stencilAttachment.loadAction = MTLLoadActionClear;
                           rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = c->c.u[1]; } }
        else if (stencil) rp.stencilAttachment.clearStencil = c->c.u[0];
        else rp.colorAttachments[0].clearColor = c->is_uint ? MTLClearColorMake(c->c.u[0], c->c.u[1], c->c.u[2], c->c.u[3]) : MTLClearColorMake(c->c.f[0], c->c.f[1], c->c.f[2], c->c.f[3]);
        ser_gate(); enc_r_end(enc_r_begin(rp));
    }
}

// 一次提交的现场描述（mtl_sub_commit 每次提交取一份，命令缓冲出错时打进红行）：进崔凡克首帧的 GPU 内存故障，红行里只有序号与错误码，看不出那次提交里有什么
// Context description of one submit (mtl_sub_commit takes one per submit and prints it into the failure line when the command buffer errors): for the GPU memory fault on the first frame entering Travincal, the failure line had only the sequence number and error code, and it was impossible to tell what was in that submit
static const char *cctx_snap(void) {
    static char b[320]; static uint64_t p_pass, p_up, p_reup, p_ser; uint64_t up = 0, reup = 0; mtl_pkg_stats(&up, &reup);
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    snprintf(b, sizeof b, "帧 %llu；自上次提交：通道 +%llu、贴图首传 +%llu、重传 +%llu、序号表变动 +%llu；延迟释放队列 %lu 个；世界冻结状态 %d",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)(g_stat_pass - p_pass), (unsigned long long)(up - p_up), (unsigned long long)(reup - p_reup),
             (unsigned long long)(g_ser_gen - p_ser), nd, g_frz_state);
    p_pass = g_stat_pass; p_up = up; p_reup = reup; p_ser = g_ser_gen; return b;
}
// ---------- 世界冻结时不画世界（面板开着时整个游戏可以冻结） ----------
// 「世界已冻结」状态位 g_frz_on 由 mtl_world_frozen() 置（宿主的冻结握手调它；测试时可用 DM_NVN_TEST_FRZ=1 顶替，也可经 DM_NVN_UB 的 @set:g_frz_on 同进程切）。
// 这里不自己判面板开没开。一帧的结构（3.3.94012 局内，逐通道转储）：界面趟 →［界面模板趟］→ 阴影 / 深度 / 光照 / 透明 / 后处理约 40 趟 → 末尾合成趟（写窗口贴图）。
// 置位后的第一帧 = 完整帧：照常全画，只丢界面模板趟的绘制（模板全 0 = 没有被面板挡住的像素）——后处理平时不写被面板挡住的像素，面板缩小会露出旧内容；
// 第二帧起 = 冻结帧：从第一个不带颜色附件的通道起、到写窗口贴图的通道之前，设目标 / 清除 / 绘制 / 贴图间拷贝 / 计算全不执行，场景结果留着完整帧那一份，界面趟与末尾合成趟照画。
// 帧的界线按通道认：写过窗口贴图之后第一条不带窗口贴图的设目标 = 新一帧开始，状态位在这里锁存。认不出结构（一帧里没有不带颜色的通道）= 什么都不跳。
// 状态常量 / 缓冲更新 / 事件 / 计数器 / 贴图上传照常执行（界面趟要用，游戏在等）。
// ---------- Do not draw the world while the world is frozen (the whole game can be frozen while a panel is open) ----------
// The "world frozen" status bit g_frz_on is set by mtl_world_frozen() (the host's freeze handshake calls it; in tests DM_NVN_TEST_FRZ=1 can stand in, or it can be switched in-process via @set:g_frz_on of DM_NVN_UB).
// This code does not judge by itself whether a panel is open. Structure of a frame (3.3.94012 in-game, per-pass dump): UI pass -> [UI stencil pass] -> about 40 passes of shadows / depth / lighting / transparency / post-processing -> final compositing pass (writes the window texture).
// The first frame after the bit is set = full frame: draw everything as usual, dropping only the draws of the UI stencil pass (stencil all 0 = no pixels blocked by the panel) -- post-processing normally does not write pixels blocked by the panel, so a shrinking panel would expose old content;
// from the second frame on = frozen frame: from the first render pass without a color attachment up to before the render pass that writes the window texture, set-target / clear / draw / texture-to-texture copy / compute are all not executed, the scene result stays as the full frame's copy, and the UI pass and the final compositing pass are drawn as usual.
// Frame boundaries are identified by render pass: the first set-target without the window texture after the window texture has been written = start of a new frame, and the status bit is latched here. Unrecognized structure (no render pass without color in a frame) = skip nothing.
// State constants / buffer updates / events / counters / texture uploads execute as usual (the UI pass needs them, the game is waiting).
int g_frz_on = -1;
static int g_frz_phase = 2, g_frz_drop, g_frz_seen_ds; static uint64_t g_frz_full_n, g_frz_skip_n;
// 宿主在游戏线程上写的两个画法开关（冻结位、强制全画）先记成请求，提交入队时锁存进任务（mtl_host_latch），翻译线程执行那次提交前生效（mtl_host_apply）：
// 生效时机与同步翻译时一样落在两次提交之间，不会在翻译途中翻转（回放每条操作都读冻结位；强制全画计数回放里自减，宿主直接写会丢）
// The two drawing switches the host writes on the game thread (freeze bit, force full draw) are first recorded as requests, latched into the job when the submit is enqueued (mtl_host_latch), and take effect before the translation thread runs that submit (mtl_host_apply):
// they take effect between two submits just as with synchronous translation and never flip in the middle of translation (replay reads the freeze bit for every operation; the force-full-draw counter is decremented in replay, and a direct write by the host would lose one)
static _Atomic int g_frz_req = -1, g_force_req;
void mtl_world_frozen(int on) { atomic_store(&g_frz_req, on != 0); }
uint64_t mtl_host_latch(void) { return (uint64_t)(uint32_t)(atomic_load(&g_frz_req) + 1) | (uint64_t)(uint32_t)atomic_exchange(&g_force_req, 0) << 32; }
static int frz_wanted(void) { if (g_frz_on < 0) { const char *e = getenv("DM_NVN_TEST_FRZ"); g_frz_on = e && atoi(e); if (g_frz_on) MLOG("DM_NVN_TEST_FRZ：世界冻结状态位由测试开关顶替（常开）"); } tbdr_on(); return g_frz_on > 0 && tbdr_on() && g_ruler_truth <= 0; }
// ---------- 运行时外推（省电画法；不给玩家开关；线上出过卡顿缺陷，现缺省关，A17 上有不卡的证据再开）----------
// 3D 隔帧：全画帧照常画，并在附件被挪作他用之前拷下不透明 HDR（468）、主深度模板、色调映射后的 468、FXAA 后的 469；
// 外推帧：界面模板趟照跑；阴影图 / 深度预渲染 / 主通道只留会动单位（蒙皮）的绘制；深度预渲染第一个蒙皮绘制前跑一趟外推（nvn_rtex.inc：按 cb3 +464 → +336
// 把上一帧 HDR 与深度搬到本帧，旧单位位置 0x10 / 界面挖空 0x2 当洞补，洞与屏边新露出的像素导出模板 0x80）；半透明照画并写特效位 0x40；
// 合成 / 加泛光 / 加雾 / 色调映射 / FXAA 只在 0xD0 区域里真跑，区域外是上一帧 LDR 结果按整幅平移（正交相机，取整）拷进来；泛光链等全屏项跳过；锐化放大与合界面照跑。
// 退回原画法：版本文件 render_safe = true（dm_update.m）、外推管线建不出（整局）、测试 DM_RT_AB=0。面板开着世界冻结时让给冻结路径。
// 镜头跳变（朝向变或平移超 64 像素）与换 3D 渲染比例前后 4 帧整帧全画。地面上的单位影子沿用上一全画帧。
// ---------- Runtime extrapolation (power-saving rendering method; no switch for the player; it caused a stutter defect in released builds, now off by default, to be re-enabled once there is evidence of no stutter on A17) ----------
// 3D every other frame: a fully drawn frame draws as usual and, before the attachments are repurposed, copies the opaque HDR (468), the main depth-stencil, the post-tone-mapping 468 and the post-FXAA 469;
// extrapolated frame: the UI stencil pass runs as usual; shadow map / depth prepass / main pass keep only the draws of moving units (skinned); before the first skinned draw of the depth prepass run one extrapolation pass (nvn_rtex.inc: by cb3 +464 -> +336
// move the previous frame's HDR and depth to this frame, treat old unit positions 0x10 / UI cutout 0x2 as holes to fill, and export stencil 0x80 for holes and pixels newly exposed at the screen edge); translucents are drawn as usual and write effect bit 0x40;
// compositing / add bloom / add fog / tone mapping / FXAA really run only in the 0xD0 region, and outside the region the previous frame's LDR result is copied in with a whole-image shift (orthographic camera, rounded); full-screen items such as the bloom chain are skipped; sharpening upscale and UI compositing run as usual.
// Fallback to the original rendering method: version file render_safe = true (dm_update.m), the extrapolation pipeline cannot be built (whole run), test DM_RT_AB=0. When a panel is open and the world is frozen, yield to the freeze path.
// On a camera jump (orientation change or translation over 64 pixels) and for 4 frames before and after switching the 3D render scale, whole frames are fully drawn. Unit shadows on the ground keep using the previous fully drawn frame.
static int rt_is_skin(void) {   // 会动单位 = 顶点着色器按骨骼下标读常量的那 10 个（键表取自 Mac 着色器缓存 b-424a072084e6，按源码里 vp_c5 动态下标认） | Moving units = the 10 whose vertex shader reads constants by bone index (the key table is taken from the Mac shader cache b-424a072084e6, identified by the dynamic vp_c5 index in the source)
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil; if (!pr || !pr->sh[0]) return 0;
    static const char *k[] = { "0fa4eb7559f1637a", "1d97d33ab66abcba", "3deaf68fd65b5298", "4dffa355f2fbcdaf", "54c93fa22abff9ee",
                               "64dd7d022f3fae39", "67e2ebc28c830f44", "add86ee8e20f89b9", "b3ae9be5cc670f35", "fbf6dc1873a449e9" };
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!strncmp(pr->sh[0]->key, k[i], 16)) return 1;
    return 0;
}
int g_dm_rt_ab = 0;   // 缺省原画法（新画法在 A17 iPhone 15 Pro 上局内卡、选角页人物重影，先关）；DM_RT_AB=1 测试时开新画法。版本文件 render_safe 只会置 0，不影响 | Default is the original rendering method (the new method stutters in-game on A17 iPhone 15 Pro and ghosts characters on the character select page, so off for now); DM_RT_AB=1 turns on the new method for testing. The version file render_safe can only set it to 0, no effect
static struct { int env, frame, filt, first, hdrp, mainp_seen, skipall, oit_seen, tm_seen, fx_state, comp_seen; uint64_t ldr_ser; float vpc[16], vpp[16]; id<MTLTexture> ldr, pl468, pl469; int ui_seen, main_seen, copied, ex_done, shadow, have_prev, have_ldr, sdx, sdy, cam_checked, force_full, disabled, was_on; uint64_t n_jump; uint64_t ds_ser, hdr_ser, n_ex, n_full, n_noex;
    id<MTLTexture> ds, hdr, pds, phdr; struct { uint64_t key; int skin, other; } sh[16]; int nsh, cur_sh; } g_rt = { .env = -1 };
static int rt_want(void);
static void rt_toggle_poll(void) { static int tg = -1; if (tg < 0) { const char *e = getenv("DM_RT_AB_TOGGLE"); tg = e ? atoi(e) : 0; (void)rt_want(); }   // 首次顺带读 DM_RT_AB（真机上世界冻结位早早置 0，frz_op 不一定被调到）   // 测试：每 tg 秒按墙钟在 A / B 之间来回切 | On first call also read DM_RT_AB (on a real device the world-freeze bit is set to 0 very early, so frz_op is not necessarily called)   // test: switch back and forth between A / B every tg seconds by wall clock
    if (tg > 0) { int v = (int)((time(NULL) / tg) & 1); if (v != g_dm_rt_ab) { g_dm_rt_ab = v; MLOG("DM_RT_AB_TOGGLE：画法切到 %s", v ? "B" : "A"); } } }
void mtl_rt_force_full(int n) { atomic_store(&g_force_req, n); }   // 宿主调：要换 3D 渲染比例等画面整体变化前，之后 n 帧一律全画（随下一次提交生效） | Called by the host: before a change to the whole picture such as switching the 3D render scale, draw everything for the next n frames (takes effect with the next submit)
void mtl_host_apply(uint64_t v) { int frz = (int)(uint32_t)v - 1, force = (int)(v >> 32); if (frz >= 0) g_frz_on = frz; if (force) g_rt.force_full = force; }
// ---------- 省电模式（30 帧）----------
// 开：显示节拍对象改 30 Hz（帧率范围 30–30，ProMotion 屏跟着降刷新率；front_ios.m 的帧率提示一起降），游戏每拍出一帧；运行时外推关（每帧真画）。
// 游戏逻辑本来就按 25 拍 / 秒走、单位位置按时间插值，30 帧下没有重复帧（真机实测：每帧镜头位移的变异系数 30 帧 0.292、60 帧 0.296）。
// 开关在「自定义」页（dm_custom.c eco_fps，缺省关，应用即生效）；测试开关 DM_FPS=30/60 钉住。
// ---------- Power-saving mode (30 frames) ----------
// On: the display link object changes to 30 Hz (frame rate range 30–30, a ProMotion screen lowers its refresh rate accordingly; the frame rate hint in front_ios.m is lowered together), the game presents one frame per display-link tick; runtime extrapolation off (really draws every frame).
// Game logic already runs at 25 ticks / second and unit positions are interpolated by time, so there are no repeated frames at 30 frames (measured on a real device: coefficient of variation of per-frame camera displacement 0.292 at 30 frames, 0.296 at 60 frames).
// The switch is on the 「自定义」(Custom) page (dm_custom.c eco_fps, off by default, takes effect on apply); the test switch DM_FPS=30/60 pins it.
__attribute__((weak)) int dm_fps_setting(void) { return 60; }   // 宿主 dm_custom.c 提供强定义；图形单测不链宿主时恒 60 | The host's dm_custom.c provides the strong definition; always 60 when graphics unit tests do not link the host
int mtl_fps_target(void) { static int env = -2; if (env == -2) { const char *e = getenv("DM_FPS"); env = e ? atoi(e) : -1; if (env != 30 && env != 60) env = -1; else MLOG("DM_FPS=%d：帧数由测试开关钉住", env); }
    return env > 0 ? env : dm_fps_setting() == 30 ? 30 : 60; }
// 外推启用状态一变（省电模式切换、世界冻结接手 / 交回）：先让「跨帧不写回」账本重学（试用期里照常写回），外推晚一帧再真正切换——
// 保证切换前最后一次提交是全量写回的。不然 30 帧时每帧先清空的附件被学成不写回，切回 60 帧的第一个外推帧读到旧内容、闪一帧（测试版实测，xv 自检报红后才撤销）
// Whenever the extrapolation enabled state changes (power-saving mode toggled, world freeze takes over / hands back): first let the "no store across frames" ledger relearn (during the trial period it stores as usual), and the extrapolation really switches one frame later --
// guaranteeing the last submit before the switch is a full store. Otherwise attachments cleared first every frame at 30 frames are learned as no-store, and the first extrapolated frame after switching back to 60 frames reads old content and flashes for one frame (measured on a test build; only reverted after the xv self-check reported a failure)
static void xv_relearn(const char *why) { g_xv_n = 0; g_xv_ntrial = 0; g_xv_ntouch = 0; MLOG("跨帧不写回：%s，账本清空重学（试用期内照常写回）", why); }
static int rt_want_raw(void);
static int rt_want(void) {
    static int eff = -1; static uint64_t pend; int want = rt_want_raw(); uint64_t f = atomic_load(&g_cur_frame);
    if (eff < 0) eff = want;
    if (want == eff) { pend = 0; return eff; }
    if (!pend) { pend = f + 1; xv_relearn(want ? "外推将启用" : "外推将停用"); return eff; }
    if (f > pend) { eff = want; pend = 0; }
    return eff;
}
static int rt_want_raw(void) { static int forced; if (g_rt.env < 0) { const char *e = getenv("DM_RT_AB"); g_rt.env = e ? atoi(e) : 0; forced = e != NULL; if (e) MLOG("DM_RT_AB=%d：画法由环境变量钉住（测试用，自定义页的开关不再起作用）", g_rt.env); }
    if (forced) g_dm_rt_ab = g_rt.env;
    return g_dm_rt_ab == 1 && !g_rt.disabled && g_frz_on <= 0 && tbdr_on() && g_ruler_truth <= 0 && mtl_fps_target() == 60; }   // 面板开着世界冻结时让给冻结那条（更省）；省电模式（30 帧）每帧真画 | When a panel is open and the world is frozen, yield to the freeze path (saves more); power-saving mode (30 frames) really draws every frame
#include "nvn_rtex.inc"
static uint64_t rt_key(const p_set_rt *r) { uint64_t s_; int lv, sl; rt_sub(r, 8, &s_, &lv, &sl); return s_ * 64 + (uint64_t)lv * 8 + (uint64_t)sl; }
static int rt_is_shadow(const p_set_rt *r) { return !r->n && r->depth && r->depth->d.width == r->depth->d.height; }
static id<MTLTexture> rt_cp(id<MTLTexture> src, id<MTLTexture> dst0) {
    id<MTLTexture> dst = dst0;
    if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width height:src.height mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
        if (dst0) defer_obj_release(dst0);   // 换尺寸：旧副本可能还被在途的命令缓冲读着（MTL4 命令缓冲不保留资源），延迟放 | Size change: the old copy may still be read by an in-flight command buffer (MTL4 command buffers do not retain resources), so release it deferred
        dst = [g_dev newTextureWithDescriptor:d]; if (!dst) return nil; res_add(dst); }
    BENC b = blit(); enc_b_copy_texture(b, src, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(src.width, src.height, 1), dst, 0, 0, MTLOriginMake(0, 0, 0)); return dst; }
static void rt_blit_shift(id<MTLTexture> src, id<MTLTexture> dst, int dx, int dy) {   // 整数偏移拷贝：dst(x + dx, y + dy) = src(x, y)；搬不到的屏边一条留给区域重算（外推趟已标 0x80） | Integer-offset copy: dst(x + dx, y + dy) = src(x, y); the strip at the screen edge that cannot be moved is left for the region recompute (the extrapolation pass has already marked it 0x80)
    if (!src || !dst || src.width != dst.width || src.height != dst.height) return;
    int W = (int)src.width, H = (int)src.height, w = W - abs(dx), h = H - abs(dy); if (w <= 0 || h <= 0) return;
    BENC b = blit(); enc_b_copy_texture(b, src, 0, 0, MTLOriginMake(MAX(0, -dx), MAX(0, -dy), 0), MTLSizeMake(w, h, 1), dst, 0, 0, MTLOriginMake(MAX(0, dx), MAX(0, dy), 0)); }
static void rt_copy_prev(void) {   // 全画帧：不透明 HDR 与主深度模板各拷一份（GPU 内按序，在本命令缓冲里） | Fully drawn frame: copy the opaque HDR and the main depth-stencil once each (in order within the GPU, in this command buffer)
    if (g_rt.copied || !g_rt.main_seen || !g_rt.hdr || !g_rt.ds) return; g_rt.copied = 1;   // 本帧没认出主通道（前台 / 读取画面）不拷：上次认的贴图可能已被游戏放掉、不在驻留集里 | Do not copy when the main pass was not recognized this frame (foreground / loading screen): the textures recognized last time may already have been released by the game and not be in the residency set
    if (!rx_setup(g_rt.hdr.pixelFormat, g_rt.ds.pixelFormat)) { if (!g_rt.disabled) { g_rt.disabled = 1; MLOG("【红】运行时外推：外推管线在这台设备上建不出，整局退回原画法"); } return; }   // 建不出（老 GPU 不支持模板导出等）：整局退回 | Cannot be built (old GPU does not support stencil export etc.): fall back for the whole run
    g_rt.phdr = rt_cp(g_rt.hdr, g_rt.phdr); g_rt.pds = rt_cp(g_rt.ds, g_rt.pds); g_rt.have_prev = g_rt.phdr && g_rt.pds; }
static void rt_frame_begin(void) { g_rt.filt = g_rt.ui_seen = g_rt.main_seen = g_rt.copied = g_rt.ex_done = g_rt.mainp_seen = 0; g_rt.cur_sh = -1;
    g_rt.oit_seen = g_rt.tm_seen = g_rt.fx_state = g_rt.comp_seen = g_rt.skipall = g_rt.cam_checked = 0; g_rt_ovr = 0; g_dss_dirty = 1;
    if (g_frz_state == 0) { g_rt.have_prev = g_rt.have_ldr = 0; for (int i = 0; i < g_rt.nsh; i++) g_rt.sh[i].skin = g_rt.sh[i].other = 0; } }   // 全画帧：本帧拷成了才许下一帧外推 | Fully drawn frame: the next frame may extrapolate only if this frame's copies succeeded
static void rt_set_rt(const p_set_rt *r, int win) {   // 每条设目标（全画帧与外推帧都走）：认角色、全画帧找拷贝点、外推帧定本通道是否只留蒙皮 | Every set-target (both fully drawn and extrapolated frames go through here): identify roles, find the copy points in fully drawn frames, and in extrapolated frames decide whether this render pass keeps only skinned draws
    g_rt.cur_sh = -1; g_rt.shadow = 0; g_rt.first = 1; g_rt.hdrp = 0;
    if (win) { if (g_frz_state == 0) { rt_copy_prev(); if (g_rt.fx_state == 2 && g_rt.ldr) { g_rt.pl469 = rt_cp(g_rt.ldr, g_rt.pl469); g_rt.have_ldr = g_rt.pl469 != nil; g_rt.fx_state = 3; } }
        g_rt.filt = 0; g_rt.skipall = 0; if (g_rt_ovr) { g_rt_ovr = 0; g_dss_dirty = 1; } return; }
    id<MTLTexture> c0 = r->n > 0 && r->color[0] ? tex_mtl(r->color[0]) : nil, dt = r->depth ? tex_mtl(r->depth) : nil;
    int hdr_here = 0; for (int i = 0; i < r->n && i < 8; i++) if (r->color[i] && g_rt.hdr_ser && r->color[i]->serial == g_rt.hdr_ser) hdr_here = 1;
    if (g_frz_state == 0) {
        if (!g_rt.main_seen && r->n == 1 && c0 && c0.pixelFormat == MTLPixelFormatRG11B10Float && dt && dt.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 && dt.width >= 500 && dt.width != dt.height) {
            { static uint64_t lh, ld; if (lh != r->color[0]->serial || ld != r->depth->serial) { lh = r->color[0]->serial; ld = r->depth->serial; MLOG("运行时外推：认出主通道 不透明 HDR 序号 %llu %lux%lu、主深度模板序号 %llu %lux%lu（通道 %llu）", (unsigned long long)lh, (unsigned long)c0.width, (unsigned long)c0.height, (unsigned long long)ld, (unsigned long)dt.width, (unsigned long)dt.height, g_stat_pass + 1); } }
            g_rt.main_seen = 1; g_rt.hdr_ser = r->color[0]->serial; g_rt.ds_ser = r->depth->serial; g_rt.hdr = c0; g_rt.ds = dt; }
        else if (g_rt.main_seen && hdr_here && !g_rt.copied) rt_copy_prev();   // 主通道之后再挂不透明 HDR 的第一个设目标（半透明那趟，468 占 0 号槽但不写）：之前拷 HDR 与主深度模板 | The first set-target after the main pass that attaches the opaque HDR again (the translucent pass; 468 occupies slot 0 but is not written): before it, copy the HDR and the main depth-stencil
        else if (g_rt.main_seen && hdr_here && r->n == 1 && r->depth && r->depth->serial == g_rt.ds_ser) g_rt.fx_state = 1;   // 只挂 468 + 主深度 = 色调映射（把 468 挪作 LDR 输出） | Only 468 + main depth attached = tone mapping (repurposes 468 as the LDR output)
        else if (g_rt.fx_state == 1) {   // 色调映射之后第一个通道 = FXAA（读 468、写 469）：之前拷色调映射后的 468（LDR），记下 469 | The first render pass after tone mapping = FXAA (reads 468, writes 469): before it, copy the post-tone-mapping 468 (LDR) and note down 469
            if (r->n >= 1 && r->color[0] && c0) { g_rt.ldr_ser = r->color[0]->serial; g_rt.ldr = c0; g_rt.pl468 = rt_cp(g_rt.hdr, g_rt.pl468); g_rt.fx_state = 2; } else g_rt.fx_state = 0; }
        else if (g_rt.fx_state == 2 && g_rt.ldr) { g_rt.pl469 = rt_cp(g_rt.ldr, g_rt.pl469); g_rt.have_ldr = g_rt.pl468 && g_rt.pl469; g_rt.fx_state = 3; }   // FXAA 之后第一个通道之前：拷 FXAA 后的 469 | Before the first render pass after FXAA: copy the post-FXAA 469
    }
    if (rt_is_shadow(r)) { uint64_t k = rt_key(r); int i; for (i = 0; i < g_rt.nsh && g_rt.sh[i].key != k; i++) {}
        if (i == g_rt.nsh && i < 16) { g_rt.sh[i].key = k; g_rt.sh[i].skin = g_rt.sh[i].other = 0; g_rt.nsh++; } g_rt.cur_sh = i < 16 ? i : -1; g_rt.shadow = 1; }
    int ds_here = r->depth && g_rt.ds_ser && r->depth->serial == g_rt.ds_ser;
    if (ds_here && !r->n && !g_rt.ui_seen) { g_rt.ui_seen = 1; g_rt.filt = 0; return; }   // 本帧第一个带主深度、无颜色的通道 = 界面模板趟：照跑 | The first render pass in this frame with the main depth and no color = the UI stencil pass: run as usual
    g_rt.hdrp = hdr_here && ds_here && r->n == 1 && !g_rt.mainp_seen; if (g_rt.hdrp) g_rt.mainp_seen = 1;   // 本帧第一个「只挂不透明 HDR + 主深度」的通道 = 主通道 | The first render pass in this frame with "only opaque HDR + main depth attached" = the main pass
    g_rt.filt = g_rt.shadow || (ds_here && !r->n) || g_rt.hdrp;
    if (g_frz_state == 2 && g_rt.mainp_seen && !g_rt.hdrp) {   // 外推帧、主通道之后：后处理只在单位 / 特效区域跑（乙方案） | Extrapolated frame, after the main pass: post-processing runs only in the unit / effect region (plan B)
        int c0ldr = r->n >= 1 && r->color[0] && g_rt.ldr_ser && r->color[0]->serial == g_rt.ldr_ser, ovr = 0; g_rt.skipall = 0;
        if (g_rt.fx_state == 2) {}   // FXAA 之后（锐化放大 + 暗角、合界面）：全跑 | After FXAA (sharpening upscale + vignette, UI compositing): run everything
        else if (g_rt.fx_state == 1) g_rt.fx_state = 2;
        else if (!g_rt.oit_seen && ds_here && r->n >= 2) { g_rt.oit_seen = 1; ovr = 3; }   // 半透明（加权混合双目标）：照画，通过处写特效位 0x40 | Translucent (weighted blended, two targets): draw as usual, writing effect bit 0x40 where it passes
        else if (!g_rt.comp_seen) { if (ds_here && c0ldr) { g_rt.comp_seen = 1; ovr = 1; rt_blit_shift(g_rt.pl469, g_rt.ldr, g_rt.sdx, g_rt.sdy); } else g_rt.skipall = 1; }   // 合成之前：上一帧 FXAA 后的 469 整数平移拷进来（区域里随后重算）   // 合成之前的其余各趟（mip 链、511 折射层等）跳过 | Before compositing: the previous frame's post-FXAA 469 is copied in with an integer shift (then recomputed in the region)   // the other passes before compositing (mip chain, 511 refraction layer etc.) are skipped
        else if (ds_here && c0ldr && g_rt.tm_seen) {   // FXAA 本趟（在区域里重算；区域外的 469 合成前已平移拷好） | The FXAA pass itself (recomputed in the region; the 469 outside the region was already shift-copied before compositing)
            g_rt.fx_state = 1; ovr = 1; }
        else if (ds_here) { if (hdr_here && r->n == 1) { g_rt.tm_seen = 1; rt_blit_shift(g_rt.pl468, g_rt.hdr, g_rt.sdx, g_rt.sdy); } ovr = 1; }   // 色调映射之前：上一帧色调映射后的 468（LDR）整数平移拷进来   // 加泛光、加雾、色调映射：只在区域里跑 | Before tone mapping: the previous frame's post-tone-mapping 468 (LDR) is copied in with an integer shift   // add bloom, add fog, tone mapping: run only in the region
        else g_rt.skipall = 1;   // 泛光链等不挂主深度的：跳过（区域里沿用上一帧的泛光） | Those without the main depth attached, such as the bloom chain: skip (the region keeps using the previous frame's bloom)
        if (ovr != g_rt_ovr) { g_rt_ovr = ovr; g_dss_dirty = 1; }
    }
}
static int rt_op(unsigned op) {   // 外推帧里非设目标的操作：返回 1 = 不执行 | Non-set-target operations in an extrapolated frame: returns 1 = not executed
    if (g_frz_state == 2 && g_rt_ovr == 1 && op == OP_CLEAR_COLOR) return 1;   // 区域后处理各趟不清：区域外是平移拷进来的上一帧结果 | The region post-processing passes do not clear: outside the region is the previous frame's result copied in with a shift
    if (g_rt.skipall && (op == OP_DRAW || op == OP_DRAW_IDX || op == OP_CLEAR_COLOR || op == OP_CLEAR_DS || op == OP_COPY_T2T || op == OP_CLEAR_TEX || op == OP_DISPATCH)) return 1;
    if (op == OP_DRAW || op == OP_DRAW_IDX) {
        if (g_frz_state == 2 && !g_rt.cam_checked) {   // 外推帧的第一个绘制（界面模板趟，阴影 / 预渲染之前）：核本帧相机，跳变就整帧改全画 | First draw of an extrapolated frame (UI stencil pass, before shadows / prepass): check this frame's camera, and on a jump switch the whole frame to fully drawn
            g_rt.cam_checked = 1; const p_bind_buf *u = &S.ubo[0][3]; void *mb; uint64_t mo; const uint8_t *base; int ok = 0;
            if (u->addr && u->size >= 528 && mtl_addr(u->addr, u->size, &mb, &mo) && (base = [(__bridge id<MTLBuffer>)mb contents])) {
                const float *cur = (const float *)(base + mo + 336), *prev = (const float *)(base + mo + 464); int dx, dy; float md = 0;
                for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) md = MAX(md, fabsf(cur[r * 4 + c] - prev[r * 4 + c]));   // 前三行（朝向 / 缩放）应不变：正交相机只平移 | The first three rows (orientation / scale) should be unchanged: an orthographic camera only translates
                rt_shift_px(prev, cur, g_rt.hdr.width, g_rt.hdr.height, &dx, &dy);
                ok = md < 1e-4f && abs(dx) <= 64 && abs(dy) <= 64; if (!ok) { static int n; if (n++ < 20) MLOG("运行时外推：镜头跳变（平移 %d,%d 像素、朝向差 %g），本帧改全画", dx, dy, md); } }
            if (!ok) { g_frz_state = 0; g_rt.n_jump++; rt_frame_begin(); g_rt.cam_checked = 1; return 0; }
        }
        if (g_frz_state == 0) { if (g_rt.cur_sh >= 0) { if (rt_is_skin()) g_rt.sh[g_rt.cur_sh].skin++; else g_rt.sh[g_rt.cur_sh].other++; } return 0; }
        if (!g_rt.filt) return 0;
        if (!rt_is_skin()) return 1;
        if (!g_rt.ex_done && !g_rt.shadow) {   // 深度预渲染第一个蒙皮绘制之前：外推趟（读本绘制绑定的 cb3：+336 本帧、+464 上一帧视图投影） | Before the first skinned draw of the depth prepass: the extrapolation pass (reads the cb3 bound by this draw: +336 this frame's, +464 the previous frame's view-projection)
            g_rt.ex_done = 1; const p_bind_buf *u = &S.ubo[0][3]; void *mb; uint64_t mo; const uint8_t *base;
            if (u->addr && u->size >= 528 && mtl_addr(u->addr, u->size, &mb, &mo) && (base = [(__bridge id<MTLBuffer>)mb contents])) {
                float cur[16], prev[16]; memcpy(cur, base + mo + 336, 64); memcpy(prev, base + mo + 464, 64); memcpy(g_rt.vpc, cur, 64); memcpy(g_rt.vpp, prev, 64); rt_shift_px(prev, cur, g_rt.hdr.width, g_rt.hdr.height, &g_rt.sdx, &g_rt.sdy);
                rp_end(); if (rt_ex_encode(g_rt.hdr, g_rt.ds, g_rt.phdr, g_rt.pds, prev, cur, 0x10, 0x2)) g_rt.n_ex++; else g_rt.n_noex++;
            } else g_rt.n_noex++;
        }
        return 0;
    }
    if (g_frz_state == 2 && g_rt.filt) {
        if (op == OP_CLEAR_COLOR || op == OP_CLEAR_DS) {
            if (g_rt.shadow && g_rt.cur_sh >= 0 && !g_rt.sh[g_rt.cur_sh].other) return 0;   // 上一全画帧只有蒙皮的阴影级（人物专用级）：照清照画 | A shadow level that had only skinned draws in the previous fully drawn frame (character-only level): clear and draw as usual
            return 1; }
    }
    return 0;
}
static int frz_op(unsigned op, const void *a) {   // 返回 1 = 这条操作不执行 | Returns 1 = this operation is not executed
    if (op == OP_SET_RT) {
        const p_set_rt *r = a; int win = 0;
        for (int i = 0; i < r->n; i++) { uint64_t s_; int lv, sl; rt_sub(r, i, &s_, &lv, &sl); if (s_ && xv_present(s_)) win = 1; }
        if (g_frz_phase == 2 && !win && rt_want()) {   // 运行时外推：新一帧，全画帧与外推帧交替 | Runtime extrapolation: new frame, fully drawn frames and extrapolated frames alternate
            int was = g_frz_state; g_frz_phase = 0; g_rt.was_on = 1;
            { static int dbg = -1, n; if (dbg < 0) dbg = getenv("DM_RT_DBG") != NULL; if (dbg && n < 4000 && g_rt.ds_ser) { n++; MLOG("[rtdbg] 帧界 通道 %llu 上一段状态 %d 认出主通道 %d 拷了 %d 外推 %d fx %d ldr %d hoist %d fz %d defer %d/%d", g_stat_pass, was, g_rt.main_seen, g_rt.copied, g_rt.ex_done, g_rt.fx_state, g_rt.have_ldr, g_hoist_n, g_fz.active, defer_has_active(), g_defer_replaying); } }
            if (g_rt.force_full > 0) g_rt.force_full--;
            g_frz_state = !was && g_rt.have_prev && g_rt.have_ldr && !g_rt.force_full && !g_hoist_n && !g_fz.active && !defer_has_active() && !g_defer_replaying ? 2 : 0;
            if (g_frz_state == 2) g_rt.frame++; else g_rt.n_full++;
            if (g_frz_state != was && (g_rt.frame + g_rt.n_full) % 600 < 2) MLOG("运行时外推：全画帧 %llu、外推帧 %d（外推趟编了 %llu 次、没编 %llu 次；镜头跳变改全画 %llu 次）", g_rt.n_full, g_rt.frame, g_rt.n_ex, g_rt.n_noex, g_rt.n_jump);
            rt_frame_begin();
        }
        if (g_frz_phase != 2 && rt_want()) { if (win) g_frz_phase = 2; rt_set_rt(r, win); return g_rt.skipall; }
        if (g_frz_phase == 2 && win && rt_want()) { rt_set_rt(r, win); return 0; }
        { int on = rt_want(); if (g_rt.was_on && !on) {   // 新画法刚关（版本文件退回 / 面板冻结接手 / 整局退回）：状态归零，交回原路径 | The new rendering method was just turned off (version file fallback / panel freeze takes over / fallback for the whole run): reset state to zero and hand back to the original path
              g_frz_state = 0; g_frz_phase = 2; g_rt.have_prev = g_rt.have_ldr = 0; if (g_rt_ovr) { g_rt_ovr = 0; g_dss_dirty = 1; } g_rt.skipall = 0;
              MLOG("运行时外推：新画法停用（画法 %d、世界冻结 %d、整局退回 %d）", g_dm_rt_ab, g_frz_on, g_rt.disabled); }
          g_rt.was_on = on; }
        if (g_frz_phase == 2 && !win) {   // 新一帧 | New frame
            int on = frz_wanted(), was = g_frz_state; g_frz_phase = 0; g_frz_seen_ds = 0;
            { static uint64_t fr, p0, k0; if (was == 2 && (++fr == 4 || fr % 600 == 0)) MLOG("世界冻结：第 %llu 个冻结帧开了 %llu 个通道、跳过 %llu 条设目标", fr, g_stat_pass - p0, g_frz_skip_n - k0); if (was != 2) fr = 0; p0 = g_stat_pass; k0 = g_frz_skip_n; }
            if (!on) g_frz_state = 0;
            else if (was) g_frz_state = 2;
            else if (!g_hoist_n && !g_fz.active && !defer_has_active() && !g_defer_replaying) g_frz_state = 1;   // 上一帧留下的前提 / 合并 / 推迟还没走完：下一帧再进 | The premises / merges / deferrals left by the previous frame have not finished yet: enter next frame
            if (g_frz_state != was) MLOG("世界冻结：通道 %llu 起 %s（完整帧累计 %llu，已跳过通道 %llu）", g_stat_pass + 1, g_frz_state == 1 ? "完整帧（不带界面模板）" : g_frz_state == 2 ? "冻结帧（只画界面与末尾合成）" : "恢复正常", g_frz_full_n += g_frz_state == 1, g_frz_skip_n);
        }
        g_frz_drop = 0;
        if (win) { g_frz_phase = 2; return 0; }
        if (!g_frz_state || g_frz_phase == 2) return 0;
        if (g_frz_phase == 0 && !r->n && r->depth && !g_frz_seen_ds) {   // 本帧第一个不带颜色的通道 | The first render pass without color in this frame
            g_frz_seen_ds = 1;
            if (g_frz_state == 2) g_frz_phase = 1;
            else { uint64_t s_; int lv, sl; rt_sub(r, 8, &s_, &lv, &sl); static int neg = -1; if (neg < 0) neg = getenv("DM_NVN_TEST_FRZ_NEG") != NULL;   // 负对照：完整帧照带界面模板画，面板缩小时必须露出旧像素 | Negative control: the full frame is drawn with the UI stencil as usual, and old pixels must be exposed when the panel shrinks
                   g_frz_drop = !neg && MAX(r->depth->d.width >> lv, 1u) != MAX(r->depth->d.height >> lv, 1u); }   // 完整帧：不是正方形（阴影图）的那个就是界面模板趟 | Full frame: the one that is not square (shadow map) is the UI stencil pass
        }
        if (g_frz_phase == 1) { g_frz_skip_n++; return 1; }
        return 0;
    }
    if (rt_want() && g_frz_phase != 2) return rt_op(op);
    if (g_frz_drop) return op == OP_DRAW || op == OP_DRAW_IDX;
    if (g_frz_state == 2 && g_frz_phase == 1) return op == OP_CLEAR_COLOR || op == OP_CLEAR_DS || op == OP_DRAW || op == OP_DRAW_IDX || op == OP_COPY_T2T || op == OP_CLEAR_TEX || op == OP_DISPATCH;
    return 0;
}

void mtl_exec(const uint8_t *p) {
    static int inited; if (!inited) { inited = 1; state_reset(); }
    static int dump_all = -1; if (dump_all < 0) dump_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
    int trace = dump_all && passdump();   // 普通模式不逐操作查帧范围，完整命令位置仅用于指定诊断帧 | Normal mode does not check the frame range per operation; the full command position is only used for the designated diagnostic frames
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        g_exec_cur = p;   // 写回前瞻的扫描起点：结束通道的那条操作（含） | Scan start point of the store lookahead: the operation that ends the render pass (inclusive)
        if (p == g_defer_pend) { for (int gi = 0; gi < 4; gi++) if (g_defer_slots[gi].active && g_defer_slots[gi].p_end_op == p) g_defer_slots[gi].in_p = 0; g_defer_pend = NULL; }
        sdx_seq(p);
        if (h.op == OP_SET_RT) rt_toggle_poll();
        if ((g_frz_on || g_frz_state || g_dm_rt_ab == 1) && frz_op(h.op, p + 8)) { p += h.size; continue; }
        if (trace) PD("o cmd=%p pass=%llu op=%u size=%u", p, g_stat_pass, (unsigned)h.op, (unsigned)h.size);
        if (g_hoist_n && hoist_take(p)) { if (g_fz.active && p == g_fz.rt2_op) { g_fz.phase = 2; g_rt_merged = 0; PD("F %llu 合并通道进入后一个通道的绘制", g_stat_pass); } p += h.size; continue; }   // 已提前执行的纯清除通道 / 已并入的后一个通道开头   // 进入后一个通道：前一个通道的同目标续用到此为止，后一个通道读前一个通道附件由 fz_p2_draw 证本像素，不再按续用规则切开 | A pure-clear render pass already executed early / the start of a later pass already merged in   // Entering the later pass: the same-target continuation of the earlier pass ends here; the later pass reading the earlier pass's attachment is proven to be same-pixel by fz_p2_draw, and is no longer split by the continuation rule
        const void *a = p + 8;
        switch (h.op) {
            case OP_END: g_exec_cur = NULL;
                if (g_hoist_n && !g_ret_n && !g_nrest) {
                    if (g_fz.active) fz_unskip();
                    if (g_hoist_n) { MLOG("【红】纯清除通道前提：提交结束时还有 %d 条已前提的操作没走到，清空（不应发生）", g_hoist_n); g_hoist_n = 0; }
                }
                return;   // 列表外（出图 / CPU 读回等）触发的结束通道不得扫已过期的命令内存：起点为空即全部写回 | An end-of-pass triggered outside a list (present / CPU readback etc.) must not scan expired command memory: a null start point means store everything
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                if (g_ret_n < 16) g_ret[g_ret_n] = p + h.size; g_ret_n++;   // 前瞻用的返回栈：子列表结束后接着看调用方 | Return stack for lookahead: after a sub-list ends, continue looking at the caller
                mtl_exec((const uint8_t *)t); g_ret_n--; break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
                PD("Y signal_event cmd=%p dst=%p mode=%u value=%u", p, val, m, v); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); g_cnt_report[t & 31]++;
                PD("Q report type=%llu cmd=%p dst=%#llx", (unsigned long long)t, p, (unsigned long long)ad);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { post((post_op){3, NULL, 0, 0, ad, (uint32_t)t, vis_snapshot()}); g_vis_cur = -1; }
                else mtl_sub_counter(ad, (uint32_t)t);
                break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); g_cnt_reset[t & 31]++; PD("Q reset type=%llu cmd=%p", (unsigned long long)t, p);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
#ifdef NVN_RT_RANGE_TEST
                    rr_reset();
#endif
                    g_vis_on = 1; g_vis_counting = 1; vis_clear(); }
                break; }
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
                PD("Y copy_b2b cmd=%p src=%#llx dst=%#llx bytes=%llu", p, (unsigned long long)s_, (unsigned long long)d, (unsigned long long)n); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                PD("Y update_ub cmd=%p src=%#llx dst=%#llx bytes=%llu", p, (unsigned long long)((const uint8_t *)a + 16), (unsigned long long)d, (unsigned long long)n); mtl_sub_copy((uint64_t)((const uint8_t *)a + 16), d, n); break; }
            case OP_SET_RT: {
                const p_set_rt *r = a;
                if (defer_begin_q(p)) break;
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                if (g_enc && !pend && R.has_rt && tbdr_on() && rt_range_same(&R.rt,r)) {
                    g_rt_merged = g_rt_range_active = g_rt_range_pending = 1; R.rt = *r; break; }
                if (g_enc && g_fz.active && g_fz.phase == 1 && fz_reuse_op(p) && !pend && g_enc_mode != 1 && R.has_rt && tbdr_on() && rt_same(&g_fz.rt1, r)) {
                    g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("同目标续用：首例 通道 %llu", g_stat_pass); PD("M %llu 同目标续用通道", g_stat_pass); break; }
                // 不续用只为清除开的通道；少挂深度 / 模板的可续用（绘制前核平面）
                // Do not continue a render pass opened only for a clear; one with fewer depth / stencil attachments can be continued (check the planes before drawing)
                if (g_enc && g_enc_mode != 1 && !pend && R.has_rt && tbdr_on() && rt_same(&R.rt, r)) { g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("同目标续用：首例 通道 %llu", g_stat_pass); PD("M %llu 同目标续用通道", g_stat_pass); R.rt = *r; break; }
                rp_flush(); R.rt = *r; R.has_rt = 1; break;
            }
            case OP_CLEAR_COLOR: {
                const p_clear_color *c = a;
                if (g_rt_merged) rp_end();   // 续用的通道里来了清除：在此结束，清除照旧挂到下一个通道开头 | A clear arrived in a continued render pass: end it here, and the clear is attached to the start of the next render pass as usual
                if (c->index < 0 || c->index >= R.rt.n || !R.rt.color[c->index]) { RED_ONCE(5, "清色目标 %d 无效", c->index); break; }
                if (g_enc || c->mask != 0xf || !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index])) { RED_ONCE(6, "通道中途 / 部分掩码 / 部分剪裁的清色未实现（首例 掩码 %#x）", c->mask); break; }
                R.clear_c[c->index] = 1; R.cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                break;
            }
            case OP_CLEAR_DS: {
                const p_clear_ds *c = a;
                if (g_rt_merged) rp_end();
                if (g_enc) { RED_ONCE(7, "通道中途的深度模板清除未实现"); break; }
                if (c->depth_mask) { R.clear_d = 1; R.cd = c->depth; }
                if (c->stencil_mask) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
                break;
            }
            case OP_DRAW: g_exec_next = p + h.size; draw(a, NULL); g_exec_next = NULL; break;   // 前瞻起点 | Lookahead start point
            case OP_DRAW_IDX: g_exec_next = p + h.size; draw(NULL, a); g_exec_next = NULL; break;
            // 状态没变就不置脏（游戏每次绘制前常重发同值的程序 / 状态块 / 顶点格式；脏标志只决定是否重取管线 / 深度模板对象，值相同则对象相同）
            // Do not mark dirty if the state did not change (the game often re-sends the same program / state block / vertex format before each draw; the dirty flag only decides whether to re-fetch the pipeline / depth-stencil object, and equal values mean the same object)
            case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
            case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
            case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BLEND_COLOR: case OP_COPY_STRIDE: case OP_COPY_IMAGE_STRIDE:
                state_op(h.op, a); break;
            case OP_COPY_B2T: copy_b2t(a); break;
            case OP_COPY_T2B: copy_t2b(a); break;
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_DISPATCH: dispatch(a); break;
            case OP_BARRIER: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            default: break;
        }
        p += h.size;
    }
}

// ---------- 出图：交换链贴图画到 CAMetalLayer；可选截图 ----------
// ---------- Present: draw the swapchain texture to the CAMetalLayer; optional screenshot ----------
static id<MTLRenderPipelineState> g_blit_pso; static MTLPixelFormat g_blit_fmt;
static id<MTLSamplerState> g_blit_samp;
static uint64_t g_present_n;
uint64_t mtl_present_count(void) { return g_present_n; }   // 实际出图次数（发热归因 DM_HEAT 算出图帧率） | Actual present count (heat attribution DM_HEAT computes the present frame rate)
void mtl_res_snapshot(mtl_res_count *out) {
    memset(out, 0, sizeof *out);
    out->pso = g_pso.n;
    out->cps = g_cps.n;
    out->dss = g_dss.n;
    out->shaders = atomic_load(&g_shcache_n);   // 资源计数在宿主线程上取，回放在翻译线程上改表 | Resource counts are taken on the host thread while replay changes the tables on the translation thread
    os_unfair_lock_lock(&g_c1_lock);
    out->c1_bufs = (uint32_t)(g_c1_banks.count + g_c1_exclusive); out->c1_alloc = g_c1_alloc_bytes; out->c1_data = g_c1_data_bytes;
    os_unfair_lock_unlock(&g_c1_lock);
    pthread_mutex_lock(&g_smu);
    out->live_tex = g_by_serial.n;
    pthread_mutex_unlock(&g_smu);
    pthread_mutex_lock(&g_mu);
    out->pools = (uint32_t)g_npool;
    out->pool_adds = g_pool_adds;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_rmu);
    out->view_slots = (uint32_t)g_ntreg;
    for (int i = 0; i < g_ntreg; i++) if (g_treg[i].view) out->view_nonnull++;
    for (int i = 0; i < g_nsreg; i++) if (g_sview[i]) out->sview_nonnull++;
    pthread_mutex_unlock(&g_rmu);
}
// 资源计数（长时间稳定性 L3 读）：宿主按游戏拍 / 事件调用（每 250 拍、读取画面已撤、退出），不按墙钟 / 出图数取样。
// 行格式：[nvn] 资源计数 拍 <拍号> <触发>：<名> <数值> …（名不含空格，单空格分隔）；加字段就往这一行后面追加
// Resource counts (read by long-run stability L3): the host calls it per game tick / event (every 250 ticks, loading screen dismissed, exit), not sampled by wall clock / present count.
// Line format: [nvn] 资源计数 拍 <拍号> <触发>：<名> <数值> … ([nvn] resource counts tick <tick number> <trigger>: <name> <value> ...) (names contain no spaces, single-space separated); new fields are appended to the end of this line
void mtl_res_counts(uint32_t tick, const char *why) {
    mtl_res_count c;
    mtl_res_snapshot(&c);
    int dl, du; uint64_t da; nvn_dirty_slot_stats(&dl, &du, &da);
    LOG("[nvn] 资源计数 拍 %u %s：渲染管线 %u 计算管线 %u 着色器 %u 深度模板 %u view非空 %u view槽 %u 存活贴图 %u 内存池存活 %u 内存池累计 %llu 采样器view非空 %u 脏页区间存活 %d 脏页区间累计 %llu c1缓冲 %u c1字节 %llu c1实际 %llu",
        tick, why, c.pso, c.cps, c.shaders, c.dss, c.view_nonnull, c.view_slots, c.live_tex,
        c.pools, (unsigned long long)c.pool_adds, c.sview_nonnull, dl, (unsigned long long)da, c.c1_bufs, (unsigned long long)c.c1_alloc, (unsigned long long)c.c1_data);
}
// 按请求截图（DM_NVN_SHOT_UI 用）：dm_gfx_shot_at(k, 路径) = 在当前出图帧之后第 k 帧截图；每帧出图时检查到期请求
// Screenshot on request (used by DM_NVN_SHOT_UI): dm_gfx_shot_at(k, path) = take a screenshot at the kth frame after the current present frame; due requests are checked at every frame's present
#define SHOTQ_MAX 8
static struct { uint64_t frame; char path[1024]; } g_shotq[SHOTQ_MAX]; static int g_nshotq;
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER;
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]);
static int present_encode_m4(CBUF cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], int uv_i);
// 整窗截图要的游戏画面（前端 window_shot，不再对挂着显示节拍的 Metal 图层做 drawViewHierarchy）：下一次上屏时，出图线程把同一帧
// 按上屏同一套编码（present_encode）画进一张与交换链同尺寸的离屏贴图，GPU 做完后在完成回调里交出 CGImage（回调不阻塞上屏）。g_shotq_mu 护
// Game picture needed by the whole-window screenshot (front end window_shot; no longer does drawViewHierarchy on a Metal layer with the display link attached): at the next present, the present thread draws the same frame
// with the same encoding as the present (present_encode) into an offscreen texture of the same size as the swapchain, and after the GPU finishes hands out a CGImage in the completion callback (the callback does not block the present). Guarded by g_shotq_mu
static void (^g_grab_cb)(CGImageRef img);
void mtl_grab_next_present(void (^cb)(CGImageRef img)) { pthread_mutex_lock(&g_shotq_mu); g_grab_cb = [cb copy]; pthread_mutex_unlock(&g_shotq_mu); }
static void grab_present(id<MTLTexture> src, const int crop[4], CGSize sz) {
    pthread_mutex_lock(&g_shotq_mu); void (^cb)(CGImageRef) = g_grab_cb; g_grab_cb = nil; pthread_mutex_unlock(&g_shotq_mu);
    if (!cb) return;
    size_t w = (size_t)sz.width, h = (size_t)sz.height, row = w * 4;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = w && h ? [g_dev newTextureWithDescriptor:d] : nil; id<MTLBuffer> buf = out ? [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared] : nil;
    id<MTLCommandBuffer> cb2 = buf ? q_cb() : nil;
    if (!cb2 || !present_encode(cb2, src, out, crop)) { MLOG("【红】整窗截图：游戏画面离屏编码失败（%zux%zu）", w, h); cb(NULL); return; }
    id<MTLBlitCommandEncoder> b = [cb2 blitCommandEncoder];
    [b copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding];
    [cb2 addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL; cb(img);
        CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs); }];
    [cb2 commit];
}
// ---------- 测试：连拍（量画面连续性用，缺省不动；触点脚本步骤「<时刻> burst <帧数> <目录名>」触发，目录建在 Documents 下）----------
// 连续 N 帧（上限 BURST_MAX）各拷一块上屏前的游戏画面（屏幕中心、边长 = 画面高 × 0.6 的方块，原始像素）：抓的那一帧只多一次显存拷贝、不等显卡，
// 帧间隔不受影响（Mac 实测 16.5–16.9 ms）；全部抓完后台线程写盘：f000.raw…（宽 × 高 × 4 字节）+ meta.txt（每帧的出图帧号、墙钟、客体时钟、本地玩家逻辑位置）。
// 分析：dm/ios/tools/burst_sad.py <目录>（逐帧画面位移与逻辑位移对照）
// ---------- Test: burst capture (for measuring picture continuity, inactive by default; triggered by the touch-script step "<time> burst <frames> <directory name>", directory created under Documents) ----------
// For N consecutive frames (limit BURST_MAX) copy a block of the game picture before presentation (a square at the screen centre with side = picture height x 0.6, raw pixels): a captured frame costs only one more GPU memory copy and does not wait for the GPU,
// so the frame interval is unaffected (16.5–16.9 ms measured on a Mac); after all frames are captured a background thread writes to disk: f000.raw... (width x height x 4 bytes) + meta.txt (presented frame number, wall clock, guest clock and logical position of the local player for each frame).
// Analysis: dm/ios/tools/burst_sad.py <directory> (per-frame picture displacement compared with logical displacement)
__attribute__((weak)) int dm_diag_player_fixed(uint32_t *x, uint32_t *y) { (void)x; (void)y; return 0; }   // 宿主 diag.c 提供强定义 | the strong definition is provided by the host's diag.c
#define BURST_MAX 512   /* 512 帧约 415 MB（450×450×4 一帧），只在 Mac 上给这么大；真机别超过 128 | 512 frames is about 415 MB (450x450x4 per frame), this much only on a Mac; do not exceed 128 on a device */
static int g_burst_left, g_burst_n; static char g_burst_dir[700];
static id<MTLBuffer> g_burst_buf[BURST_MAX]; static id<MTLCommandBuffer> g_burst_cb[BURST_MAX]; static uint64_t g_burst_fr[BURST_MAX], g_burst_ns[BURST_MAX], g_burst_gns[BURST_MAX]; static uint32_t g_burst_px[BURST_MAX], g_burst_py[BURST_MAX];
static size_t g_burst_w, g_burst_h; static unsigned long g_burst_fmt;
void dm_gfx_burst(int n, const char *dir) { if (g_burst_left > 0 || n <= 0) return; if (n > BURST_MAX) n = BURST_MAX; strlcpy(g_burst_dir, dir, sizeof g_burst_dir); g_burst_n = n; g_burst_left = n; }
static void burst_grab(id<MTLTexture> src) {
    uint64_t guest_ns(void);
    int i = g_burst_n - g_burst_left; size_t dm_ = src.width, sh_ = src.height, c = sh_ * 6 / 10; if (c > dm_) c = dm_;
    g_burst_w = c; g_burst_h = c; g_burst_fmt = (unsigned long)src.pixelFormat;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:c * c * 4 options:MTLResourceStorageModeShared]; id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((dm_ - c) / 2, (sh_ - c) / 2, 0) sourceSize:MTLSizeMake(c, c, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:c * 4 destinationBytesPerImage:c * c * 4];
    [b endEncoding]; [cb commit];
    g_burst_buf[i] = buf; g_burst_cb[i] = cb; g_burst_fr[i] = g_present_n; g_burst_ns[i] = now_ns(); g_burst_gns[i] = guest_ns(); g_burst_px[i] = g_burst_py[i] = 0; dm_diag_player_fixed(&g_burst_px[i], &g_burst_py[i]);
    if (--g_burst_left == 0) { int n = g_burst_n; size_t w = g_burst_w, h = g_burst_h; unsigned long fmt = g_burst_fmt; NSString *dir = [NSString stringWithUTF8String:g_burst_dir];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            NSMutableString *meta = [NSMutableString stringWithFormat:@"w %zu h %zu fmt %lu n %d\n", w, h, fmt, n];
            for (int k = 0; k < n; k++) { [g_burst_cb[k] waitUntilCompleted];
                [[NSData dataWithBytes:g_burst_buf[k].contents length:w * h * 4] writeToFile:[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"f%03d.raw", k]] atomically:NO];
                [meta appendFormat:@"%d frame %llu ns %llu guest_ns %llu px %.4f py %.4f\n", k, (unsigned long long)g_burst_fr[k], (unsigned long long)g_burst_ns[k], (unsigned long long)g_burst_gns[k], g_burst_px[k] / 65536.0, g_burst_py[k] / 65536.0];
                g_burst_buf[k] = nil; g_burst_cb[k] = nil; }
            [meta writeToFile:[dir stringByAppendingPathComponent:@"meta.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            MLOG("测试：连拍 %d 帧写完 %s（%zux%zu 格式 %lu）", n, dir.UTF8String, w, h, fmt); });
    }
}
void dm_gfx_shot_at(long k, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
    if (g_nshotq < SHOTQ_MAX) { g_shotq[g_nshotq].frame = g_present_n + (uint64_t)(k > 0 ? k : 0); strlcpy(g_shotq[g_nshotq].path, path, sizeof g_shotq[0].path); g_nshotq++; }
    else MLOG("【红】截图请求超过 %d 个，丢弃 %s", SHOTQ_MAX, path);
    pthread_mutex_unlock(&g_shotq_mu);
}
// DM_NVN_SHOT_OUT=宽x高（画质度量用）：出图帧截图改截「经 present_encode 放大到该分辨率」的结果，即玩家在屏幕上看到的画面；screen=0（贴图转储）不受影响
// DM_NVN_SHOT_OUT=WIDTHxHEIGHT (for picture quality metrics): the present-frame screenshot instead captures the result "upscaled to this resolution by present_encode", i.e. the picture the player sees on screen; screen=0 (texture dump) is unaffected
static void shot(id<MTLTexture> src, const char *path, int screen, const int crop[4]) {
    static int out_w = -1, out_h;
    if (out_w < 0) {
        out_w = 0;
        const char *e = getenv("DM_NVN_SHOT_OUT"); int w = 0, h = 0; char sep = 0, extra = 0;
        if (e && sscanf(e, "%d%c%d%c", &w, &sep, &h, &extra) == 3 && sep == 'x' && w > 0 && h > 0) { out_w = w; out_h = h; }
    }
    id<MTLTexture> read = src;
    if (out_w && screen) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:out_w height:out_h mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> out = [g_dev newTextureWithDescriptor:d];
        if (!out) { MLOG("【红】截图输出贴图建不出 %dx%d", out_w, out_h); return; }
        id<MTLCommandBuffer> cb = q_cb();
        if (!present_encode(cb, src, out, crop)) return;
        [cb commit]; [cb waitUntilCompleted];
        read = out;
    }
    if (read.pixelFormat != MTLPixelFormatRGBA8Unorm && read.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB &&
        read.pixelFormat != MTLPixelFormatBGRA8Unorm && read.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) { MLOG("【红】截图不支持格式 %lu", (unsigned long)read.pixelFormat); return; }
    size_t w = read.width, h = read.height, row = w * 4;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:read sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int bgra = read.pixelFormat == MTLPixelFormatBGRA8Unorm || read.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGBitmapInfo bi = bgra ? (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little) : (CGBitmapInfo)kCGImageAlphaNoneSkipLast;
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, bi);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
    // 任一步为空（目录不存在 / 不可写、位图上下文建不出）报红返回；原先直接 CFRelease(NULL) 崩在 CFRelease.cold（1952 宽 TEXDUMP 时撞到）
    // If any step yields null (directory does not exist / not writable, bitmap context cannot be created), report a failure and return; previously it went straight to CFRelease(NULL) and crashed in CFRelease.cold (hit during a 1952-wide TEXDUMP)
    int ok = 0;
    if (!ctx || !img || !dst) MLOG("【红】截图 %s 失败：%s", path, !ctx ? "位图上下文建不出" : !img ? "取图失败" : "写不出（目录不存在或不可写）");
    else { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); }
    if (dst) CFRelease(dst); if (url) CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    dm_shot_written(path, ok);   // 先让宿主写「冻结拍」行，再写「成功」行：截图脚本见到「成功」就停宿主，顺序反了满载时日志会缺「冻结拍」行（闸门 require_log 误红） | Let the host write the 「冻结拍」(frozen tick) line first, then write the 「成功」(success) line: the screenshot script stops the host as soon as it sees 「成功」(success), and with the order reversed the log would lack the 「冻结拍」(frozen tick) line under full load (false failure in the gate check's require_log)
    MLOG("截图 %zux%zu 格式 %lu → %s %s", w, h, (unsigned long)read.pixelFormat, path, ok ? "成功" : "失败");
    if (g_ruler_truth && getenv("DM_NVN_RULER") && !strcmp(getenv("DM_NVN_RULER"), "1") && ok) { static int shots; if (++shots == 2) g_ruler_pending = 1; }
    if (g_defer_ab >= 0 && getenv("DM_NVN_DEFER_AB") && !strcmp(getenv("DM_NVN_DEFER_AB"), "1") && ok) { static int shots; if (++shots == 2) { g_defer_ab_on = 1; MLOG("DM_NVN_DEFER_AB：第二张截图写盘完成，打开推迟合并通道"); } }
    if (g_udc_ab >= 0 && getenv("DM_NVN_UDC_AB") && !strcmp(getenv("DM_NVN_UDC_AB"), "1") && ok) {
        static int shots;
        if (++shots == 2) { g_udc_ab_on = 1; MLOG("DM_NVN_UDC_AB：第二张截图写盘完成，打开 DM_NVN_UNWRITTEN_DC"); }
    }
}
// ---------- DM_NVN_TEXDUMP=帧号:路径（路径里 %d 换成「序号*100+mip 级」）：把该帧用过的颜色渲染目标逐张存图 ----------
// 诊断用：在下一帧第一次开通道前等 GPU 做完，再把每张（取该帧用到的 mip 级）全屏采样画进 RGBA8 存 PNG（浮点格式按 [0,1] 截断）
// ---------- DM_NVN_TEXDUMP=frame number:path (%d in the path is replaced with "index*100+mip level"): save each color render target used by that frame as an image ----------
// Diagnostic: before the first render pass of the next frame is opened, wait for the GPU to finish, then draw each one (the mip level used by that frame) by full-screen sampling into RGBA8 and save as PNG (float formats are clamped to [0,1])
static long g_td_frame = -2; static const char *g_td_path; static NSMutableArray *g_td_list; static int g_td_done;
static void td_dump(void);
void td_note(RPD *pd, NSUInteger nc) {
    if (g_td_frame == -2) { const char *e = getenv("DM_NVN_TEXDUMP"), *c = e ? strchr(e, ':') : NULL; g_td_frame = c ? atol(e) : -1; g_td_path = c ? c + 1 : NULL; g_td_list = [NSMutableArray array]; }
    if (g_td_frame < 0 || g_td_done) return;
    long f = (long)atomic_load(&g_cur_frame);
    if (f > g_td_frame) { td_dump(); return; }
    if (f != g_td_frame) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) {
        id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        uint64_t ser = i < (NSUInteger)R.rt.n && R.rt.color[i] ? R.rt.color[i]->serial : 0;
        NSUInteger lv = pd.colorAttachments[i].level;
        BOOL seen = NO; for (NSArray *e in g_td_list) if ([e[1] unsignedLongLongValue] == ser && [e[2] unsignedIntegerValue] == lv) seen = YES;
        if (!seen) [g_td_list addObject:@[t, @(ser), @(lv)]];
    }
}
static void td_dump(void) {
    g_td_done = 1;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    mtl_finish_gpu();
    static id<MTLRenderPipelineState> ps; static id<MTLSamplerState> sp;
    if (!ps) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { float4 c = t.sample(s, in.uv); return float4(c.rgb, 1); }\n";
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src_msl) options:nil error:&e];
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:@"fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sp = [g_dev newSamplerStateWithDescriptor:sd];
        if (!ps) { MLOG("【红】TEXDUMP 管线失败 %s", e.localizedDescription.UTF8String); return; }
    }
    MLOG("[texdump] 帧 %ld 共 %lu 张颜色目标", g_td_frame, (unsigned long)g_td_list.count);
    // DM_NVN_TEXRAW=序号,…：按贴图序号存原始字节（含 3D；文件 路径%d 换成序号、后缀 .raw，首行文本头：宽 高 深 格式 每行字节）
    // DM_NVN_TEXRAW=index,...: save raw bytes by texture index (including 3D; file: %d in the path is replaced with the index, suffix .raw, first-line text header: width height depth format bytes-per-row)
    for (const char *q = getenv("DM_NVN_TEXRAW"); q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
        uint64_t ser = (uint64_t)atoll(q); id o = serial_get(ser);
        id<MTLTexture> t = [o isKindOfClass:[NvPkg class]] ? ((NvPkg *)o)->tex : o;
        if (!t) { MLOG("[texraw] ser=%llu 不在序号表", ser); continue; }
        int bw, bh; NSUInteger w = t.width, h = t.height, dz = t.depth;
        size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : 4; (void)bw; (void)bh;
        size_t row = w * bpp, img = row * h;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:img * dz options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, dz) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:img];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)ser); strlcat(path, ".raw", sizeof path);
        FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "%lu %lu %lu %lu %zu\n", (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, row); fwrite(buf.contents, 1, img * dz, f); fclose(f); }
        MLOG("[texraw] ser=%llu %lux%lux%lu 格式 %lu → %s", ser, (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, path);
    }
    for (NSArray *e in g_td_list) {
        id<MTLTexture> t = e[0]; uint64_t ser = [e[1] unsignedLongLongValue]; NSUInteger lv = [e[2] unsignedIntegerValue];
        if (t.textureType != MTLTextureType2D) { MLOG("[texdump] ser=%llu 类型 %lu 跳过", ser, (unsigned long)t.textureType); continue; }
        if (getenv("DM_NVN_SNAPRAW")) {   // 同时存原始字节（HDR 目标转 8 位图会截断；首行文本头：宽 高 格式 每行字节） | Also save the raw bytes (converting an HDR target to an 8-bit image truncates; first-line text header: width height format bytes-per-row)
            NSUInteger rw = MAX(t.width >> lv, 1u), rh = MAX(t.height >> lv, 1u);
            size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR16Float || t.pixelFormat == MTLPixelFormatR16Unorm ? 2 : 4;
            id<MTLBuffer> rb = [g_dev newBufferWithLength:rw * rh * bpp options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> rc = q_cb(); id<MTLBlitCommandEncoder> bb = [rc blitCommandEncoder];
            [bb copyFromTexture:t sourceSlice:0 sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(rw, rh, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:rw * bpp destinationBytesPerImage:rw * rh * bpp];
            [bb endEncoding]; [rc commit]; [rc waitUntilCompleted];
            char rp_[1024]; snprintf(rp_, sizeof rp_, g_td_path, (int)(ser * 100 + lv)); strlcat(rp_, ".raw", sizeof rp_);
            FILE *f = fopen(rp_, "wb"); if (f) { fprintf(f, "%lu %lu %lu %zu\n", (unsigned long)rw, (unsigned long)rh, (unsigned long)t.pixelFormat, rw * bpp); fwrite(rb.contents, 1, rw * rh * bpp, f); fclose(f); }
        }
        id<MTLTexture> v = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(lv, 1) slices:NSMakeRange(0, 1)];
        NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModePrivate;
        id<MTLTexture> o = [g_dev newTextureWithDescriptor:od];
        id<MTLCommandBuffer> cb = q_cb();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en setFragmentTexture:v atIndex:0]; [en setFragmentSamplerState:sp atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)(ser * 100 + lv));
        MLOG("[texdump] ser=%llu lv=%lu 格式 %lu %lux%lu", ser, (unsigned long)lv, (unsigned long)t.pixelFormat, (unsigned long)w, (unsigned long)h);
        shot(o, path, 0, NULL);
    }
    [g_td_list removeAllObjects];
}
// DM_NVN_TEXSNAP=帧内序号,…（配合 DM_NVN_TEXDUMP 的帧号与路径）：这些通道一结束就把其颜色附件拷一份，与帧末存图一起存
// （同一张目标常在帧内被清掉复用，帧末状态反映不了中间通道的输出）；文件名序号 = 帧内序号*100000 + 贴图序号*100 + mip 级
// DM_NVN_TEXSNAP=in-frame index,... (together with the frame number and path of DM_NVN_TEXDUMP): as soon as these render passes end, copy their color attachments and save them together with the end-of-frame dump
// (the same target is often cleared and reused within a frame, so the end-of-frame state cannot reflect the output of intermediate passes); file name index = in-frame index*100000 + texture index*100 + mip level
static int g_ts_idx[64], g_ts_n = -1, g_ts_on; static uint64_t g_ts_first, g_ts_curf = ~0ull;
static __strong id<MTLTexture> g_ts_att[8]; static uint64_t g_ts_ser[8]; static NSUInteger g_ts_lv[8]; static int g_ts_na;
void ts_note(RPD *pd, NSUInteger nc) {
    if (g_ts_n < 0) { g_ts_n = 0; const char *e = getenv("DM_NVN_TEXSNAP");
        for (const char *q = e; q && *q && g_ts_n < 64; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) g_ts_idx[g_ts_n++] = atoi(q); }
    g_ts_on = 0; if (!g_ts_n || g_td_frame < 0 || g_td_done) return;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_ts_curf) { g_ts_curf = f; g_ts_first = g_stat_pass; }
    if ((long)f != g_td_frame) return;
    int idx = (int)(g_stat_pass - g_ts_first); for (int i = 0; i < g_ts_n; i++) if (g_ts_idx[i] == idx) g_ts_on = idx + 1;
    g_ts_na = 0; if (!g_ts_on) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        g_ts_att[g_ts_na] = t; g_ts_ser[g_ts_na] = R.rt.color[i] ? R.rt.color[i]->serial : 0; g_ts_lv[g_ts_na] = pd.colorAttachments[i].level; g_ts_na++; }
}
void ts_after(void) {
    if (!g_ts_on) return;
    id<MTLCommandBuffer> dcb = q_cb();
    for (int i = 0; i < g_ts_na; i++) {
        id<MTLTexture> t = g_ts_att[i]; if (t.textureType != MTLTextureType2D) continue;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:t.mipmapLevelCount > 1];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; d.mipmapLevelCount = t.mipmapLevelCount;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d];
        id<MTLBlitCommandEncoder> b = [dcb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:g_ts_lv[i] toTexture:c destinationSlice:0 destinationLevel:g_ts_lv[i] sliceCount:1 levelCount:1]; [b endEncoding];
        [g_td_list addObject:@[c, @((uint64_t)(g_ts_on - 1) * 1000 + g_ts_ser[i]), @(g_ts_lv[i])]];
    }
    g_ts_on = 0;
    [dcb commit];
}
// 真实呈现时刻环：出图序号 → presentedTime（秒；−1 = 被系统丢弃未呈现）
// Ring of actual presentation times: present sequence number -> presentedTime (seconds; -1 = dropped by the system, not presented)
#define PRING 1024
typedef struct { uint64_t n; double t; } pres_ent;
static pres_ent g_pres[PRING];
// 取第 n 次出图的实际呈现时刻（未就绪返回 0，被丢弃返回 −1）
// Get the actual presentation time of the nth present (returns 0 if not ready, -1 if dropped)
double mtl_presented_time(uint64_t n) { pres_ent e = g_pres[n % PRING]; return e.n == n ? e.t : 0; }
static _Atomic uint64_t g_drawable_ns;   // 出图里等 nextDrawable 的累计墙钟（计时埋点） | Accumulated wall clock waiting for nextDrawable in present (timing instrumentation)
uint64_t mtl_drawable_ns_take(void) { return atomic_exchange(&g_drawable_ns, 0); }
// 窗口所在屏的刷新率与名字：Mac 前端在主线程上读 AppKit 存一份（front_mac.m screen_note）；没有这份的前端回 0，走下面的退路
// Refresh rate and name of the screen the window is on: the Mac front end reads AppKit on the main thread and stores a copy (front_mac.m screen_note); front ends without this copy return 0 and take the fallback below
__attribute__((weak)) double front_display_refresh(char *name, size_t n) { if (name && n) *name = 0; return 0; }
// 有窗口时屏幕的刷新率（Hz，不取整）；无窗口返回 0。退路：Mac 用主显示器的 CoreGraphics 显示模式，iOS 用 UIScreen 的最高帧率
// Screen refresh rate when there is a window (Hz, not rounded); returns 0 with no window. Fallback: Mac uses the CoreGraphics display mode of the main display, iOS uses the UIScreen max frame rate
double mtl_display_refresh(void) {
    if (!front_metal_layer()) return 0;
    double hz = front_display_refresh(NULL, 0); if (hz > 0) return hz;
#if TARGET_OS_OSX
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    hz = m ? CGDisplayModeGetRefreshRate(m) : 0; if (m) CGDisplayModeRelease(m);
    return hz;
#else
    static double cached;   // iOS 的屏幕最高帧率不会变：只在第一次（出图配置时）读 UIScreen，之后每帧调不碰 UIKit | The iOS screen max frame rate never changes: UIScreen is read only the first time (at present setup); later per-frame calls do not touch UIKit
    if (cached <= 0) { Class c = NSClassFromString(@"UIScreen"); id s = c ? [c valueForKey:@"mainScreen"] : nil; cached = s ? [[s valueForKey:@"maximumFramesPerSecond"] doubleValue] : 0; }
    return cached;
#endif
}
int mtl_display_hz(void) { return (int)(mtl_display_refresh() + 0.5); }
// 原先固定 60：144 Hz 屏上 60 不整除，系统只能 2 拍 / 3 拍交替（13.9 / 20.8 ms），看着一顿一顿。算式见 nvn_pace.h nvn_pace_hz
// Previously fixed at 60: 60 does not evenly divide a 144 Hz screen, so the system can only alternate 2 / 3 refreshes (13.9 / 20.8 ms), which looks stuttery. Formula in nvn_pace.h nvn_pace_hz
double dm_pace_hz(double screen_hz, double floor_hz) { return nvn_pace_hz(screen_hz, floor_hz); }
double mtl_pace_hz(void) { return dm_pace_hz(mtl_display_refresh(), mtl_fps_target() == 30 ? 30 : 60); }
static _Atomic uint64_t g_present_stall_ns;   // 单测用：出图线程每次出图前额外停顿（模拟 drawable 迟迟不可用） | For unit tests: an extra pause before each present on the present thread (simulating a drawable that stays unavailable)
#if !DM_NVN_M3
static CAMetalLayer *g_present_layer_resident;
#endif
void mtl_present_test_stall(uint64_t ns) { g_present_stall_ns = ns; }
static void present_pipeline(MTLPixelFormat format) {
    if (!g_blit_pso || g_blit_fmt != format) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "struct Crop { float4 rect; float4 edge; };\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }\n";
        NSError *e = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src_msl) options:nil error:&e];
        if (!lib) MDIE("出图着色器编译失败 %s", e.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = format;
        g_blit_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!g_blit_pso) MDIE("出图管线失败 %s", e.localizedDescription.UTF8String);
        g_blit_fmt = format;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.supportArgumentBuffers = YES;
        g_blit_samp = [g_dev newSamplerStateWithDescriptor:sd];
    }
}
// 呈现采样的源：交换链贴图是 sRGB 格式（游戏把交换链建成 RGBA8_SRGB）时经它的非 sRGB 视图采样，屏上显示存的字节，与 Switch 扫描输出一致；
// 直接采样会先解成线性，呈现层（BGRA8Unorm）不再编码，整幅偏暗（公开仓库 #36）。截图读的是原始字节，看不出这一处。
// 视图按源贴图缓存 4 张、换掉最久没用的：呈现走自己的队列，不能挂主提交的延迟释放；同时在跑的呈现最多 3 帧，被换掉的那张已经用完
// Source for presentation sampling: when the swapchain texture has an sRGB format (the game created the swapchain as RGBA8_SRGB), sample through its non-sRGB view so the screen shows the stored bytes, as the Switch's scan-out does;
// sampling it directly decodes to linear first, and the present layer (BGRA8Unorm) does not encode again, so the whole picture is too dark (public repository #36). Screenshots read the raw bytes and cannot see this.
// Views are cached per source texture, 4 of them, replacing the least recently used: presentation runs on its own queue and cannot use the deferred release of the main submission; at most 3 presents are in flight, so the replaced one is already done
static id<MTLTexture> present_src(id<MTLTexture> src) {
    MTLPixelFormat raw = MTLPixelFormatInvalid;
    for (size_t i = 0; i < sizeof g_pfv_srgb / sizeof *g_pfv_srgb; i++) if (src.pixelFormat == g_pfv_srgb[i].b) raw = g_pfv_srgb[i].a;
    if (raw == MTLPixelFormatInvalid) return src;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static id<MTLTexture> cs[4], cv[4]; static uint64_t used[4], tick;
    pthread_mutex_lock(&mu);
    int k = -1, old = 0;
    for (int i = 0; i < 4; i++) { if (cs[i] == src) k = i; if (used[i] < used[old]) old = i; }
    if (k < 0) { k = old; cs[k] = src; cv[k] = [src newTextureViewWithPixelFormat:raw];
        if (!cv[k]) RED_ONCE(50, "出图：sRGB 交换链贴图（格式 %lu）的非 sRGB 视图建不出，直接采样（画面会偏暗，首例）", (unsigned long)src.pixelFormat);
        else MLOG("出图：交换链贴图是 sRGB 格式 %lu，经格式 %lu 的视图采样", (unsigned long)src.pixelFormat, (unsigned long)raw); }
    used[k] = ++tick; id<MTLTexture> v = cv[k] ?: src;
    pthread_mutex_unlock(&mu);
    return v;
}
// 出图仅采NVN声明的有效源矩形；与原始交换链读回截图分开。边缘钳到有效区纹素中心，双线性不读外侧黑纹素。
// Present samples only the valid source rectangle declared by NVN; kept separate from the raw swapchain readback screenshot. Edges are clamped to the texel centers of the valid area, so bilinear does not read the black texels outside.
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src0, id<MTLTexture> dst, const int crop[4]) {
    id<MTLTexture> src = present_src(src0);
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("【红】出图裁切越界：%d,%d %dx%d / 原纹理 %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    static uint64_t last_crop[8]; uint64_t meta[8] = {src.width, src.height, cx, cy, cw, ch, dst.width, dst.height};
    if (memcmp(meta, last_crop, sizeof meta)) { memcpy(last_crop, meta, sizeof meta);
        MLOG("出图裁切：原纹理 %lux%lu，有效 %lu,%lu %lux%lu → 目标 %lux%lu", (unsigned long)src.width, (unsigned long)src.height,
             (unsigned long)cx, (unsigned long)cy, (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height); }
    present_pipeline(dst.pixelFormat);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    // 保持游戏画面宽高比居中，余下留黑边（屏幕比例与游戏不同时不拉伸，如 iPhone 约 19.5:9 对游戏 16:9）；
    // 比例差 < 1%（全屏口径下游戏宽度取 8 的倍数带来的零头，如 1952x900 对 2868x1320 差 0.17%）直接铺满，不留几像素的边
    // Keep the game picture's aspect ratio and center it, leaving black bars in the rest (no stretching when the screen ratio differs from the game's, e.g. iPhone about 19.5:9 versus the game's 16:9);
    // when the ratio difference is < 1% (the remainder from the game width being a multiple of 8 in the full-screen setup, e.g. 1952x900 versus 2868x1320 differs by 0.17%) fill directly, leaving no few-pixel border
    double dw = dst.width, dh = dst.height, s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    static double last_vp[4];   // 视口变化时记一行（真机屏幕级验收 screen_check.py 核宽高比用） | Log one line when the viewport changes (used by the real-device screen-level acceptance screen_check.py to check the aspect ratio)
    if (memcmp(last_vp, &vp, sizeof last_vp)) { memcpy(last_vp, &vp, sizeof last_vp);
        MLOG("出图视口 %.0f,%.0f %.0fx%.0f / 画布 %.0fx%.0f（游戏画面 %lux%lu）", vp.originX, vp.originY, vp.width, vp.height, dw, dh, (unsigned long)src.width, (unsigned long)src.height); }
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setViewport:vp];
    [en setRenderPipelineState:g_blit_pso]; [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:g_blit_samp atIndex:0];
    float uv[8] = {(float)cx / src.width, (float)cy / src.height, (float)cw / src.width, (float)ch / src.height,
                   (cx + 0.5f) / src.width, (cy + 0.5f) / src.height, (cx + cw - 0.5f) / src.width, (cy + ch - 0.5f) / src.height};
    [en setFragmentBytes:uv length:sizeof uv atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    return 1;
}
static int present_encode_m4(CBUF cb, id<MTLTexture> src0, id<MTLTexture> dst, const int crop[4], int uv_i) {
    id<MTLTexture> src = present_src(src0);
    NSUInteger cx = 0, cy = 0, cw = src.width, ch = src.height;
    if (crop && (crop[2] || crop[3])) {
        if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
            (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
            MLOG("【红】出图裁切越界：%d,%d %dx%d / 原纹理 %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
        }
        cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3];
    }
    present_pipeline(dst.pixelFormat);
    double dw = dst.width, dh = dst.height, s = fmin(dw / cw, dh / ch);
    MTLViewport vp = {floor((dw - cw * s) / 2), floor((dh - ch * s) / 2), round(cw * s), round(ch * s), 0, 1};
    if (fabs((dw / dh) / ((double)cw / ch) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    RPD *rp = [RPD new];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    RENC en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { MLOG("【红】M4 出图渲染编码器创建失败"); return 0; }
#if DM_NVN_M3
    [en setFragmentTexture:src atIndex:0];
    [en setFragmentSamplerState:g_blit_samp atIndex:0];
#else
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [en setArgumentTable:g_present_args atStages:MTLRenderStageFragment];
    [g_present_args setTexture:src.gpuResourceID atIndex:0];
    [g_present_args setSamplerState:g_blit_samp.gpuResourceID atIndex:0];
#endif
    float *uv = g_present_uv[uv_i].contents;
    uv[0] = (float)cx / src.width; uv[1] = (float)cy / src.height; uv[2] = (float)cw / src.width; uv[3] = (float)ch / src.height;
    uv[4] = (cx + 0.5f) / src.width; uv[5] = (cy + 0.5f) / src.height; uv[6] = (cx + cw - 0.5f) / src.width; uv[7] = (cy + ch - 0.5f) / src.height;
#if DM_NVN_M3
    [en setFragmentBuffer:g_present_uv[uv_i] offset:0 atIndex:0];
#else
    [g_present_args setAddress:g_present_uv[uv_i].gpuAddress atIndex:0];
#endif
    [en setViewport:vp]; [en setRenderPipelineState:g_blit_pso];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    m4_end(en);
    return 1;
}
// 测试 DM_TEST_PRESENT_RGB=<N>（只测试用，查「冷启动整屏品红一两帧」）：进程前 N 次上屏，每次等这一帧画完、交给系统之前，回读 drawable 上 5 个点
// （中心与四个四分之一点）的颜色；颜色组合变了才记一行，任一点是品红（R≥200、G≤60、B≥200）就记「品红」行，第 N 次记汇总。
// 射程：只看我们交出去的那张 drawable；系统合成层之后加上的颜色看不到。每次上屏都同步等 GPU，开着会掉帧
// Test DM_TEST_PRESENT_RGB=<N> (test only, for "one or two full-screen magenta frames at cold start"): for the process's first N presents, wait for each frame to finish and, before handing it to the system, read back the colors of 5 points on the drawable
// (the center and the four quarter points); log a line only when the color combination changes, log a "magenta" line if any point is magenta (R≥200, G≤60, B≥200), and log a summary at the Nth present.
// Coverage: only the drawable we hand over; colors added after the system compositor are not visible. Each present waits for the GPU synchronously, so enabling it drops frames
static long present_rgb_n(void) { static long n = -1; if (n < 0) { const char *e = getenv("DM_TEST_PRESENT_RGB"); n = e ? atol(e) : 0; if (n) MLOG("DM_TEST_PRESENT_RGB：前 %ld 次上屏回读 drawable 取色", n); } return n; }
static void present_rgb(id<MTLTexture> dt, uint64_t ticket) {
    static long k, magenta; static uint32_t last[5]; static uint64_t t0;
    if (k >= present_rgb_n()) return;
    pthread_mutex_lock(&g_pq_mu); while (g_pq_done < ticket) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu);
    NSUInteger w = dt.width, h = dt.height; const NSUInteger px[5][2] = {{w / 2, h / 2}, {w / 4, h / 4}, {w * 3 / 4, h / 4}, {w / 4, h * 3 / 4}, {w * 3 / 4, h * 3 / 4}};
    id<MTLBuffer> buf = [g_dev newBufferWithLength:5 * 256 options:MTLResourceStorageModeShared]; id<MTLCommandBuffer> c2 = q_cb(); id<MTLBlitCommandEncoder> b = [c2 blitCommandEncoder];
    for (int i = 0; i < 5; i++) [b copyFromTexture:dt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(px[i][0], px[i][1], 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:buf destinationOffset:i * 256 destinationBytesPerRow:4 destinationBytesPerImage:4];
    [b endEncoding]; [c2 commit]; [c2 waitUntilCompleted];
    uint64_t now = now_ns(); if (!t0) t0 = now; k++;
    uint32_t c[5]; int mag = 0, chg = 0; char txt[160]; int o = 0;
    for (int i = 0; i < 5; i++) { const uint8_t *q = (const uint8_t *)buf.contents + i * 256;   // BGRA8
        int r = q[2], g = q[1], bl = q[0]; c[i] = (uint32_t)(r << 16 | g << 8 | bl); chg |= c[i] != last[i]; mag |= r >= 200 && g <= 60 && bl >= 200;
        o += snprintf(txt + o, sizeof txt - o, " %02x%02x%02x", r, g, bl); }
    if (dt.pixelFormat != MTLPixelFormatBGRA8Unorm && k == 1) MLOG("DM_TEST_PRESENT_RGB：drawable 格式 %lu 不是 BGRA8，取色按 BGRA 读可能不准", (unsigned long)dt.pixelFormat);
    if (mag) magenta++;
    if (mag) MLOG("上屏取色 第 %ld 次（%.3f s）品红：%s", k, (now - t0) / 1e9, txt);
    else if (chg || k == 1) MLOG("上屏取色 第 %ld 次（%.3f s）变为：%s", k, (now - t0) / 1e9, txt);
    memcpy(last, c, sizeof c);
    if (k == present_rgb_n()) MLOG("上屏取色 汇总：前 %ld 次上屏，品红 %ld 次（drawable %lux%lu）", k, magenta, (unsigned long)w, (unsigned long)h);
}
static double g_link_it_tin;   // 节拍回调正在上屏的那一帧的入队时刻（只在节拍线程读写） | Enqueue time of the frame the display-link callback is presenting (read and written only on the display-link thread)
static int present_now(const tex_t *t, double period, uint64_t frame, uint64_t ticket, const int crop[4], id<CAMetalDrawable> supplied, double target_ts) {
    if (g_present_stall_ns) { struct timespec ts = {0, (long)g_present_stall_ns}; nanosleep(&ts, NULL); }
    g_present_n++;
    if (g_use != USE_OFF && g_use != USE_DONE) use_tick();
    if (bootprof() && (g_present_n == 1 || g_present_n % 120 == 0)) {
        if (g_present_n == 1) g_bp_first_present = bp_now() - g_bp_t0;
        MLOG("[bootprof] t=%.3f s 出图第 %llu 次：metallib 载入 %llu 个 / %.1f ms；管线创建 %llu 个 / %.1f ms（均在提交线程同步）；预热领用 着色器 %llu / 渲染管线 %llu / 计算管线 %llu；"
             "贴图上传 %.1f MB / %llu 张首传 + %llu 次重传 / %.1f ms；首帧出图 t=%.3f s", (bp_now() - g_bp_t0) / 1e9, g_present_n,
             g_bp_lib_n, g_bp_lib_ns / 1e6, g_bp_pso_n, g_bp_pso_ns / 1e6, g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps,
             g_bp_up_bytes / 1048576.0, g_pkg_uploads, g_pkg_reuploads,
             g_up_total_ns / 1e6, g_bp_first_present / 1e9);
    }
    PD("X ser=%llu", t->serial);
    id<MTLTexture> src = tex_mtl(t);
    if (!src) { RED_ONCE(8, "出图贴图建不出 MTLTexture"); return 0; }
    // DM_NVN_REGLUM=x0,y0,x1,y1:文件（归一化坐标）：每次出图回读该区域，记「出图序号、出图时刻 ns、区域平均亮度、隔点灰度小图（十六进制）」（诊断：核动画速度与帧率无关）。
    // 单独的命令缓冲排在本帧渲染之后（同一队列按提交序执行），不等 GPU、不扰动帧节奏
    // DM_NVN_REGLUM=x0,y0,x1,y1:file (normalized coordinates): read back this region on every present and record "present sequence number, present time ns, region mean luminance, subsampled grayscale thumbnail (hex)" (diagnostic: verify that animation speed is independent of frame rate).
    // A separate command buffer is queued after this frame's rendering (the same queue executes in submit order); does not wait for the GPU and does not disturb frame pacing
    static int rl = -1; static float rr[4]; static FILE *rlf; static pthread_mutex_t rlmu = PTHREAD_MUTEX_INITIALIZER;
    if (rl < 0) { const char *e = getenv("DM_NVN_REGLUM"), *c = e ? strchr(e, ':') : NULL;
        rl = c && sscanf(e, "%f,%f,%f,%f", &rr[0], &rr[1], &rr[2], &rr[3]) == 4 && (rlf = fopen(c + 1, "w")) != NULL;
        if (e && !rl) MLOG("【红】DM_NVN_REGLUM 参数或文件无效：%s", e); }
    if (rl && (src.pixelFormat == MTLPixelFormatRGBA8Unorm || src.pixelFormat == MTLPixelFormatBGRA8Unorm || src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB)) {
        NSUInteger x0 = (NSUInteger)(rr[0] * src.width), y0 = (NSUInteger)(rr[1] * src.height), w = (NSUInteger)(rr[2] * src.width) - x0, h = (NSUInteger)(rr[3] * src.height) - y0;
        id<MTLBuffer> rb = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
        [b endEncoding];
        unsigned long long n = g_present_n, tns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; const uint8_t *q = rb.contents; uint64_t sum = 0;
            uint8_t g[4096]; NSUInteger ng = 0;   // 隔点取样的灰度小图（每行 / 每列取 1/4，最多 4096 点），随均值一起记下，供逐帧图像相关分析 | Subsampled grayscale thumbnail (every 4th row / column, at most 4096 points), recorded along with the mean, for per-frame image correlation analysis
            for (NSUInteger y = 0; y < h; y += 4) for (NSUInteger x = 0; x < w && ng < sizeof g; x += 4) { const uint8_t *px = q + 4 * (y * w + x); g[ng++] = (uint8_t)((px[0] + px[1] + px[2]) / 3); }
            for (NSUInteger k = 0; k < w * h; k++) sum += q[4 * k] + q[4 * k + 1] + q[4 * k + 2];
            pthread_mutex_lock(&rlmu); fprintf(rlf, "%llu\t%llu\t%.4f\t", n, tns, sum / (3.0 * w * h));
            for (NSUInteger k = 0; k < ng; k++) fprintf(rlf, "%02x", g[k]);
            fputc('\n', rlf); fflush(rlf); pthread_mutex_unlock(&rlmu); }];
        [cb commit];
    }
    // DM_NVN_SHOT=帧号[,帧号…]:路径（路径里的 %d 换成帧号）
    // DM_NVN_SHOT=frame number[,frame number...]:path (%d in the path is replaced with the frame number)
    static long shots[16]; static int nshot = -1; static const char *shot_path;
    if (nshot < 0) {
        nshot = 0; const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) { shot_path = c + 1; for (const char *q = e; q < c && nshot < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) shots[nshot++] = atol(q); }
    }
    for (int i = 0; i < nshot; i++) if ((long)g_present_n == shots[i]) { char path[1024]; snprintf(path, sizeof path, shot_path, (int)g_present_n); mtl_finish_gpu(); shot(src, path, 1, crop); }
    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) if (g_present_n >= g_shotq[i].frame) { mtl_finish_gpu(); shot(src, g_shotq[i].path, 1, crop); g_shotq[i] = g_shotq[--g_nshotq]; } else i++;
    pthread_mutex_unlock(&g_shotq_mu);

    if (g_burst_left > 0) burst_grab(src);   // 测试：连拍 | test: burst capture
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    if (!layer) return 0;
#if !DM_NVN_M3
    if (g_present_layer_resident != layer) { [g_q4 addResidencySet:layer.residencySet]; g_present_layer_resident = layer; }
#endif
#if TARGET_OS_OSX
    if (!layer.displaySyncEnabled) layer.displaySyncEnabled = YES;   // 窗口模式按显示器垂直同步，防撕裂 | Windowed mode follows the display's vsync to prevent tearing
#endif
    if (present_rgb_n() && layer.framebufferOnly) layer.framebufferOnly = NO;   // 测试取色要回读 drawable | Sampling colors for the test requires reading back the drawable
    present_pipeline(layer.pixelFormat);   // 和原路径一样，首帧编管线在取drawable之前 | Same as the original path: the first frame compiles the pipeline before taking the drawable
    if (g_grab_cb) grab_present(src, crop, layer.drawableSize);   // 整窗截图要的游戏画面（不加锁先看一眼，真取在函数里加锁） | Game picture needed by the whole-window screenshot (peek without the lock first; the real take locks inside the function)
    uint64_t dt0 = now_ns();
    id<CAMetalDrawable> dr = supplied ? supplied : [layer nextDrawable];
    if (!supplied) g_drawable_ns += now_ns() - dt0;
    if (!dr) return 0;
    int alloc_i = (int)(ticket % 3);
    if (g_present_alloc_fence[alloc_i]) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_done < g_present_alloc_fence[alloc_i]) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        pthread_mutex_unlock(&g_pq_mu);
    }
#if DM_NVN_M3
    CBUF cb = [g_q commandBuffer];   // 旧接口：交画面记在命令缓冲上（presentDrawable），这一帧的绘制做完才上屏 | older API: presentation is recorded on the command buffer (presentDrawable), so the frame reaches the screen only after its drawing is done
    if (!present_encode_m4(cb, src, dr.texture, crop, alloc_i)) return 0;
#else
    [g_present_alloc4[alloc_i] reset];
    [g_q4 waitForDrawable:dr];
    CBUF cb = g_present_cb[alloc_i];
    [cb beginCommandBufferWithAllocator:g_present_alloc4[alloc_i]];
    if (!present_encode_m4(cb, src, dr.texture, crop, alloc_i)) { [cb endCommandBuffer]; [g_q4 signalDrawable:dr]; return 0; }
    mp_encode(cb, src, alloc_i);
    [cb endCommandBuffer];
#endif
#if !DM_NVN_M3
    MTL4CommitOptions *o = [MTL4CommitOptions new];
#endif
    [FB_ON(o, cb) FB_SEL:^(FB_ARG f) {
        if (f.error) MLOG("【红】上屏命令缓冲错误：%s", f.error.description.UTF8String);
#if !DM_NVN_M3
        else mp_check(alloc_i, ticket);
#endif
        pthread_mutex_lock(&g_pq_mu); if (ticket > g_pq_done) g_pq_done = ticket; g_pq_inflight--; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    }];
#if !DM_NVN_M3
    CBUF buffers[] = {cb};
#endif
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight++; pthread_mutex_unlock(&g_pq_mu);
#if !DM_NVN_M3
    [g_q4 commit:buffers count:1 options:o];
#endif
    if (target_ts > 0) {
        static double leads[300], e2e[300];   // e2e：游戏入队 → 系统预定上屏（含在队列里等节拍回调的时间），输入延迟里我们这一侧能读到的全程 | e2e: game enqueue -> system-scheduled present (including the time waiting in the queue for the display-link callback), the full span of input latency that our side can read
        static uint64_t lead_n;
        leads[lead_n % 300] = (target_ts - CACurrentMediaTime()) * 1000.0; e2e[lead_n % 300] = g_link_it_tin > 0 ? (target_ts - g_link_it_tin) * 1000.0 : 0;
        lead_n++;
        if (lead_n % 300 == 0) {
            double a[300]; memcpy(a, leads, sizeof a);
            for (int i = 1; i < 300; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
            double b[300]; memcpy(b, e2e, sizeof b);
            for (int i = 1; i < 300; i++) { double v = b[i]; int j = i - 1; while (j >= 0 && b[j] > v) { b[j + 1] = b[j]; j--; } b[j + 1] = v; }
            MLOG("显示节拍 至 %llu：节拍 300、出图 %llu、空节拍 %llu、合并丢弃旧帧 %llu；提交领先目标上屏 p50 %.3f ms / 最小 %.3f ms；游戏入队到目标上屏 p50 %.3f ms / 最小 %.3f / 最大 %.3f ms",
                 (unsigned long long)g_link_tick_n, (unsigned long long)lead_n, (unsigned long long)(g_link_tick_n - lead_n),   // 只数节拍里出的图：节拍停了改自己取 drawable 的那段不算（原先用总出图数，抢救后减成负数） | Count only frames presented via the display link: the period of acquiring drawables ourselves after a stall does not count (previously the total present count was used, which went negative after a rescue)
                 (unsigned long long)g_link_dropped, a[149], a[0], b[149], b[0], b[299]);
        }
    }
#if !DM_NVN_M3
    if (present_rgb_n()) present_rgb(dr.texture, ticket);
    [g_q4 signalDrawable:dr];
#endif
    g_present_alloc_fence[alloc_i] = ticket;
#if TARGET_OS_SIMULATOR   // iOS 模拟器 SDK 没有呈现时刻回调与最短显示时长（只作画面验收用，不计节拍） | The iOS simulator SDK has no presentation-time callback and no minimum display duration (used only for picture acceptance, display-link ticks are not counted)
    (void)period; M3_PRESENT(cb, dr);
#else
    uint64_t pn = ticket;   // 按游戏出图序号（票号）记上屏时刻：hle 的上屏统计按「游戏第 n 次出图」取；显示节拍路径里实际出图次数 ≠ 游戏出图次数（旧帧会被合并丢弃） | Record the present time by the game's present sequence number (ticket number): hle's present statistics are taken by "the game's nth present"; in the display link path the actual present count != the game's present count (old frames get merged and dropped)
    [dr addPresentedHandler:^(id<MTLDrawable> d) {   // 真实呈现时刻（屏幕上真正换上这一帧的时间，CACurrentMediaTime 基准） | Actual presentation time (the time this frame is really put on screen, CACurrentMediaTime base)
        CFTimeInterval tt = d.presentedTime;
        g_pres[pn % PRING] = (pres_ent){pn, tt > 0 ? tt : -1};
    }];
    // 每张至少显示一个出图周期：GPU 完成时刻在 120 Hz 刷新边界附近抖动时，不再出现「25 ms + 8.33 ms」成对的顿挫
    // Each frame is displayed for at least one present period: when the GPU completion time jitters around a 120 Hz refresh boundary, the paired "25 ms + 8.33 ms" stutter no longer appears
#if DM_NVN_M3
    if (target_ts <= 0 && period > 0) [cb presentDrawable:dr afterMinimumDuration:period]; else [cb presentDrawable:dr];
#else
    if (target_ts > 0) [dr present];
    else if (period > 0) [dr presentAfterMinimumDuration:period]; else [dr present];
#endif
#endif
#if DM_NVN_M3
    [cb commit];
#endif
    (void)frame;
    return 1;
}
// ---------- 出图线程 ----------
// NVN 语义：QueuePresentTexture 只把出图排进队列、立即返回（游戏常持锁调用它；iOS 真机：在里面同步等 nextDrawable
// 约 30 ms，锁被占约 90%，装载线程饿死）。取 drawable、blit、present 都在这条线程上按入队顺序做。
// 票号 = 第几次入队（从 1 起）；g_pq_done = 已处理完（blit 已提交到 GPU 队列）的票号。
// 交换链贴图归还：AcquireTexture 拿到某张前等它上一次出图的票号处理完（mtl_present_wait），之后游戏再往里画的命令
// 在 GPU 队列上必排在那次 blit 之后。
// ---------- Present thread ----------
// NVN semantics: QueuePresentTexture only queues the present and returns immediately (the game often calls it while holding a lock; iOS real device: synchronously waiting for nextDrawable inside it
// takes about 30 ms, the lock is held about 90% of the time, and the loading thread starves). Taking the drawable, blit and present are all done on this thread in enqueue order.
// Ticket number = which enqueue it is (starting from 1); g_pq_done = the ticket number that has been processed (blit committed to the GPU queue).
// Swapchain texture return: before AcquireTexture gets a texture it waits for the ticket of that texture's previous present to be processed (mtl_present_wait), after which the commands the game draws into it
// are necessarily ordered after that blit on the GPU queue.
#define PQ 8
typedef struct { tex_t t; double period; uint64_t frame; int crop[4]; double t_in; } pq_item;   // t_in：游戏把这一帧交进出图队列的时刻（CACurrentMediaTime） | t_in: the time the game handed this frame into the present queue (CACurrentMediaTime)
static pq_item g_pq[PQ];
static uint64_t g_pq_in;
static int present_now(const tex_t *, double, uint64_t, uint64_t, const int[4], id<CAMetalDrawable>, double);

@interface NvMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@end

// 前台节拍停了的兜底（两次合入冒烟卡死：前台时节拍不再回调，排队帧没人取，游戏在「取下一张交换链贴图」前等上一次出图处理完，永远等下去）：
// mtl_present_wait 等超过 STALL_NS 且节拍这么久都没回调，就照进后台那样把等着的这张及更早的排队帧丢掉（按掉帧记账），放游戏继续跑；节拍回来自动照常。
// 一段停顿记一行线索、恢复时记一行（都不是红）；同一段停顿连着超过 STALL_RED_NS 才红。全在 g_pq_mu 下
// Fallback for the display link going silent while in the foreground (two smoke-test hangs before merge: the display link stopped calling back while the app was in the foreground, nobody takes the queued frames, and the game waits forever for its previous present to be processed before "acquiring the next swapchain texture"):
// when mtl_present_wait has waited longer than STALL_NS and the display link has not called back for that long, drop the waiting frame and earlier queued frames as when entering the background (accounted as dropped frames) and let the game keep running; when the display link comes back things return to normal automatically.
// One stall logs one hint line, and one line on recovery (neither is a failure); only when the same stall lasts longer than STALL_RED_NS is it a failure. All under g_pq_mu
#define STALL_NS 100000000ull
#define STALL_RED_NS 2000000000ull
static uint64_t g_stall_t0, g_stall_drop, g_stall_n; static int g_stall_red, g_stall_logged;
// 激活中节拍停够 LINK_RESCUE_NS 就不再干等它回来：改成出图线程自己取 drawable 上屏（模式 0，不靠节拍回调），画面接着动；
// 过 g_rescue_back_ns 再换回节拍试一次，又停就把间隔翻倍（上限 60 秒）。两份日志：iPhone 激活中节拍停了再没回来，
// 原兜底只丢帧，画面定住 80 秒（玩家看到「黑屏 / 锁死」）。全在 g_pq_mu 下改；模式切换由 link_sync 在游戏出图线程上做
// When the display link stalls for LINK_RESCUE_NS while active, stop waiting for it: switch to the present thread acquiring a drawable itself (mode 0, no display-link callback), so the picture keeps moving;
// after g_rescue_back_ns, switch back to the display link once more, and if it stalls again double the interval (cap 60 s). Two logs: on iPhone the display link stalled while active and never came back,
// and the old fallback only dropped frames, freezing the picture for 80 s (players saw a "black screen / lockup"). All changes are made under g_pq_mu; mode switching is done by link_sync on the game present thread
#define LINK_RESCUE_NS 500000000ull
static int g_rescue_mode; static uint64_t g_rescue_back_at, g_rescue_back_ns = 3000000000ull, g_rescue_n;
// 显示节拍看门狗只认「激活」状态：App 失去活跃（下拉控制中心 / 通知、来电、系统弹窗）和后台里系统本来就不回调，画面也被盖住——丢帧兜底照旧，
// 但不算卡住、不报红；停顿从「最近一次回调 / 最近一次激活」较晚的那个算起（A16 日志：失活 8–17 秒、后台 17 秒都被报成「前台卡住」）
// The display link watchdog only recognizes the "active" state: when the App loses active status (pulling down Control Center / notifications, incoming call, system popup) and in the background the system does not call back anyway, and the picture is covered too -- the frame-drop fallback stays as before,
// but it is not counted as stuck and no failure is reported; the stall is measured from the later of "the most recent callback / the most recent activation" (A16 logs: 8–17 seconds inactive and 17 seconds in the background were both reported as 「前台卡住」(stuck in foreground))
__attribute__((weak)) int dm_focus_now(void) { return 1; }   // 宿主 hle_oe.c 提供强定义；图形单测不链宿主时视为一直激活 | The host's hle_oe.c provides the strong definition; when graphics unit tests do not link the host it is treated as always active
__attribute__((weak)) uint64_t dm_focus_in_since(void) { return 0; }
@implementation NvMetalDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
    pthread_mutex_lock(&g_pq_mu);
    if (g_stall_t0) { uint64_t since = dm_focus_in_since(), t = now_ns();
        MLOG("显示节拍恢复：这段停了 %.0f ms（其中激活中 %.0f ms），兜底丢了 %llu 帧（第 %llu 段）", (t - g_stall_t0) / 1e6, (t - (g_stall_t0 > since ? g_stall_t0 : since)) / 1e6, (unsigned long long)g_stall_drop, (unsigned long long)g_stall_n); g_stall_t0 = 0; }
    pthread_mutex_unlock(&g_pq_mu);
    pthread_mutex_lock(&g_link_mu);
    g_link_tick_ns = now_ns();
    g_link_tick_n++;
    pthread_cond_broadcast(&g_link_cv);
    pthread_mutex_unlock(&g_link_mu);
    pthread_mutex_lock(&g_pq_mu);
    if (!g_dm_present_link || g_pq_read == g_pq_in) { pthread_mutex_unlock(&g_pq_mu); return; }
    uint64_t first = g_pq_read + 1, ticket = g_pq_in;
    pq_item it = g_pq[(g_pq_in - 1) % PQ]; g_link_it_tin = it.t_in;
    g_pq_read = g_pq_in;
    if (ticket > first) { g_link_dropped += ticket - first; for (uint64_t k = first; k < ticket; k++) g_pres[k % PRING] = (pres_ent){k, -1}; }   // 被合并丢弃的旧帧记「未上屏」，上屏统计如实算掉帧 | Old frames merged and dropped are recorded as "not presented", so the present statistics honestly count them as dropped frames
    if (ticket > 1 && g_pq_done < ticket - 1) g_pq_done = ticket - 1;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    int submitted;
    @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, update.drawable, update.targetPresentationTimestamp); }
    if (!submitted) {
        pthread_mutex_lock(&g_pq_mu);
        if (ticket > g_pq_done) g_pq_done = ticket;
        pthread_cond_broadcast(&g_pq_cv);
        pthread_mutex_unlock(&g_pq_mu);
    }
}
@end

static CAMetalDisplayLink *g_link; static CFRunLoopRef g_link_rl; static int g_link_started;
static int g_link_owns_layer, g_classic_busy;   // 持 g_pq_mu：图层挂着节拍对象期间 nextDrawable 会抛异常，自取 drawable 的出图线程与节拍对象互斥 | Under g_pq_mu: nextDrawable throws while the layer has the display link object attached, so the present thread that takes its own drawable and the display link object are mutually exclusive
static int link_on(void) { return g_dm_present_link && g_link != nil; }
static CAFrameRateRange link_range(int mode) {   // 1 / 3 按屏幕刷新率选整除的那一档（mtl_pace_hz）；2（120 Hz 节拍、测试档）不变 | 1 / 3 pick the evenly dividing tier by screen refresh rate (mtl_pace_hz); 2 (120 Hz pacing, test tier) unchanged
    if (mode == 2) return CAFrameRateRangeMake(60, 120, 120);
    float r = (float)mtl_pace_hz(); return CAFrameRateRangeMake(r, r, r); }
static int g_link_mode = 1;   // 节拍对象当前的模式：1 / 2 同 g_dm_present_link，3 = 省电模式 30 Hz | Current mode of the display link object: 1 / 2 same as g_dm_present_link, 3 = power-saving mode 30 Hz
static void *link_main(void *arg) {   // 显示节拍线程：切到节拍模式时起，切回 0 时退出 | Display link thread: started when switching to display-link mode, exits when switching back to 0
    (void)arg;
    pthread_setname_np("nvn-present-link");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; while (g_classic_busy) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu);   // 等出图线程手上那一帧交完 | Wait for the present thread to finish handing over the frame it holds
    NvMetalDisplayLinkDelegate *delegate = [NvMetalDisplayLinkDelegate new];
    CAMetalDisplayLink *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    link.preferredFrameRateRange = link_range(g_link_mode);
    link.preferredFrameLatency = g_present_latency;
    link.delegate = delegate;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    g_link_rl = CFRunLoopGetCurrent(); g_link = link;
    CFRunLoopRun();   // 切回模式 0 时节拍对象作废、本线程退出（图层还挂着节拍对象时 nextDrawable 取不到图，不能只暂停） | When switching back to mode 0 the display link object is invalidated and this thread exits (nextDrawable cannot get a drawable while the layer still has the display link object attached, so pausing alone is not enough)
    g_link_started = 0;
    return NULL;
}
static void link_sync(void) {   // 游戏出图线程每帧调：把节拍对象的状态对齐到 g_dm_present_link、省电模式与屏幕刷新率（运行中可改） | Called every frame by the game present thread: aligns the pacing object state with g_dm_present_link, power saving mode and screen refresh rate (changeable at runtime)
    static int applied; static double applied_hz;
    if (g_rescue_mode && now_ns() >= g_rescue_back_at) {   // 抢救期满：换回节拍再试（停顿计时从头算，免得刚换回就按旧停顿再抢救） | Rescue period expired: switch back to the display link and retry (stall timing restarts, so we do not rescue again right after switching back based on the old stall)
        pthread_mutex_lock(&g_pq_mu); g_dm_present_link = g_rescue_mode; g_rescue_mode = 0; g_stall_t0 = 0; g_link_tick_ns = now_ns(); pthread_mutex_unlock(&g_pq_mu);
        MLOG("显示节拍：抢救期满，换回节拍模式 %d 再试（帧 %llu）", g_dm_present_link, (unsigned long long)atomic_load(&g_cur_frame)); }
    int want = g_dm_present_link;
    if (want < 0 || want > 2) { g_dm_present_link = applied == 3 ? 1 : applied; return; }
    if (want && mtl_fps_target() == 30) want = 3;   // 省电模式：显示节拍 30 Hz（按屏幕刷新率取整除档） | Power saving mode: display pacing 30 Hz (evenly dividing tier by screen refresh rate)
    if (want == applied && want && want != 2 && g_link) {   // 窗口换了屏 / 屏幕改了刷新率：节拍跟着换到新的整除档 | Window moved to another screen / screen refresh rate changed: pacing switches to the new evenly dividing tier
        double hz = mtl_pace_hz();
        if (hz != applied_hz) { CAMetalDisplayLink *link = g_link; link.preferredFrameRateRange = link_range(want);
            if (applied_hz) MLOG("出图节拍换档 %.2f → %.2f Hz（屏幕刷新率 %.2f Hz，帧 %llu）", applied_hz, hz, mtl_display_refresh(), (unsigned long long)atomic_load(&g_cur_frame));
            applied_hz = hz; }
        return;
    }
    if (want == applied) return;
    g_link_mode = want;
    if (!g_link_started) {
        if (!want) { applied = 0; return; }
        if (!front_metal_layer()) return;
        g_link_started = 1; pthread_t th; if (pthread_create(&th, NULL, link_main, NULL)) MDIE("显示节拍线程起不来"); pthread_detach(th);
        return;   // 节拍对象在新线程里建，下一帧再对齐 | The display link object is created on a new thread; align again next frame
    }
    CAMetalDisplayLink *link = g_link; if (!link) return;
    if (want) link.preferredFrameRateRange = link_range(want);
    else { CFRunLoopRef rl = g_link_rl; g_link = nil; CFRunLoopPerformBlock(rl, kCFRunLoopDefaultMode, ^{ [link invalidate]; pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu); CFRunLoopStop(rl); }); CFRunLoopWakeUp(rl); }
    MLOG("出图节拍模式 %d → %d（帧 %llu）", applied, want, (unsigned long long)atomic_load(&g_cur_frame));
    applied = want;
    pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
// 进后台前（mtl_drain）：显示节拍驱动出图时，排着队还没上屏的帧只能由节拍回调取走，而后台里系统不再回调——
// 原样等「出图队列清空」会永远等下去，进后台的回调不返回，系统把进程杀掉（切到别的 App 后 22 秒内进程消失）。
// 这些帧反正不会被看到：直接丢弃（按掉帧记账），只等已经提交给 GPU 的那一次上屏做完。
// Before going to the background (mtl_drain): when the display link drives presents, frames queued but not yet presented can only be taken by the display-link callback, and in the background the system no longer calls back --
// waiting as-is for "the present queue to empty" would wait forever, the enter-background callback would not return, and the system would kill the process (the process disappears within 22 seconds after switching to another App).
// These frames will not be seen anyway: discard them directly (accounted as dropped frames) and only wait for the one present already submitted to the GPU to finish.
static void present_drop_for_suspend(void) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_link_owns_layer) {
        uint64_t n = g_pq_in - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= g_pq_in; k++) g_pres[k % PRING] = (pres_ent){k, -1};
        g_link_dropped += n; g_pq_read = g_pq_in;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < g_pq_in) g_pq_done = g_pq_in;
        pthread_cond_broadcast(&g_pq_cv);
        if (n) MLOG("进后台：丢弃排队中未上屏的 %llu 帧（显示节拍在后台不回调）", (unsigned long long)n);
    }
    pthread_mutex_unlock(&g_pq_mu);
}
static void *present_main(void *arg) {
    (void)arg;
    pthread_setname_np("nvn-present");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp); atomic_store(&g_present_qos, (uint32_t)qc);
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_read == g_pq_in || g_link_owns_layer) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // 节拍模式下由节拍回调取；空队列看「已取出」；「已完成」要等上屏提交的 feedback，落后于取出 | In display-link mode the display-link callback takes them; an empty queue is judged by "taken"; "done" has to wait for the feedback of the present submit and lags behind taken
        pq_item it = g_pq[g_pq_read % PQ];
        uint64_t ticket = ++g_pq_read; g_classic_busy = 1;
        pthread_mutex_unlock(&g_pq_mu);
        int submitted;
        @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, nil, 0); }
        pthread_mutex_lock(&g_pq_mu); g_classic_busy = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
        if (!submitted) {
            pthread_mutex_lock(&g_pq_mu);
            if (ticket > g_pq_done) g_pq_done = ticket;
            pthread_cond_broadcast(&g_pq_cv);
            pthread_mutex_unlock(&g_pq_mu);
        }
    }
    return NULL;
}
uint32_t mtl_present_qos(void) { return atomic_load(&g_present_qos); }
static void present_start(void) { pthread_t th; if (pthread_create(&th, NULL, present_main, NULL)) MDIE("出图线程起不来"); pthread_detach(th); }
static _Atomic int g_present_discard;
void mtl_link_pause(int on);
// 前端进后台、等游戏存盘那一小段：出图直接丢（显示节拍后台不回调，入队会把游戏线程堵住；画面也没人看）。
// 打开时把已经排着队的帧一并清掉：系统是先停节拍回调、之后才通知进后台，这段空档里游戏已把交换链画满，正卡在「取下一张贴图前等它上一次出图处理完」
// （hle_nvn.c nvnWindowAcquireTexture → mtl_present_wait）。不清的话等存盘那 2.5 秒游戏一拍都走不了，盘没存成（玩家日志里三次同形：排队 3 帧、提交 0 次）
// The short period when the front end goes to the background and waits for the game to save: presents are dropped directly (the display link does not call back in the background, enqueueing would block the game thread; nobody sees the picture anyway).
// When turned on, also clear the frames already queued: the system first stops the display-link callback and only afterwards notifies about going to the background, and in that gap the game has already filled the swapchain and is stuck at "before acquiring the next texture, wait for its previous present to be processed"
// (hle_nvn.c nvnWindowAcquireTexture -> mtl_present_wait). Without clearing, the game cannot advance a single tick during those 2.5 seconds of waiting for the save, and the save is not written (the same pattern three times in player logs: 3 frames queued, 0 submits)
void mtl_present_discard(int on) { atomic_store(&g_present_discard, on != 0); if (on) present_drop_for_suspend(); }
uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {
    if (atomic_load(&g_present_discard)) return 0;   // 票号 0 = 没有待完成的上屏，取交换链贴图处不等 | Ticket number 0 = no pending present; the swapchain texture acquire does not wait
    if (t) { xv_check(t->serial, 0, 0, "出图"); if (g_xv_npresent < 128) g_xv_present[g_xv_npresent++] = t->serial; }
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, present_start);
    link_sync();
    {   // 测试开关 DM_TEST_LINK_STALL=起始秒,时长秒：从第一次出图起算到点暂停显示节拍（模仿前台时节拍不回调），时长到了放开。验兜底用，产品不设 | Test switch DM_TEST_LINK_STALL=start seconds,duration seconds: counting from the first present, pause the display link at that point (imitating the display link not calling back while in the foreground) and release it when the duration is up. For verifying the fallback; not set in the product
        static double t0, at = -1, dur; static int st; double now = CACurrentMediaTime();
        if (at < 0) { at = 0; const char *e = getenv("DM_TEST_LINK_STALL"); if (e && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0) { t0 = now; st = 1; } else at = 0; }
        if (st == 1 && link_on() && now - t0 >= at) { st = 2; mtl_link_pause(1); MLOG("DM_TEST_LINK_STALL：暂停显示节拍 %.1f 秒（帧 %llu）", dur, (unsigned long long)atomic_load(&g_cur_frame)); }
        else if (st == 2 && now - t0 >= at + dur) { st = 3; mtl_link_pause(0); MLOG("DM_TEST_LINK_STALL：放开显示节拍（帧 %llu）", (unsigned long long)atomic_load(&g_cur_frame)); }
    }
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_in - g_pq_done >= PQ) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // 队满才等；正常由 AcquireTexture 先挡住 | Wait only when the queue is full; normally AcquireTexture blocks first
    pq_item *it = &g_pq[g_pq_in % PQ]; *it = (pq_item){*t, period, atomic_load(&g_cur_frame), {0}, CACurrentMediaTime()};
    if (crop) memcpy(it->crop, crop, sizeof it->crop);   // 当帧状态随票号入队，不在出图线程晚读可变window | The current frame's state is enqueued with the ticket number, rather than reading the mutable window late on the present thread
    uint64_t tk = ++g_pq_in;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    return tk;
}
// 丢弃期间不等：标志置上之后才入队的那一帧（出图线程正好走过了标志检查）没人取，等它就又卡住；它留给随后的 mtl_drain 清
// 节拍模式下不无限等：见 STALL_NS 处的说明。只丢「还没被节拍取走」的帧，已在上屏途中的那张等它交给 GPU（同进后台的丢法）
// Do not wait during discard: the frame enqueued only after the flag was set (the present thread had just passed the flag check) has nobody to take it, and waiting for it would stall again; it is left for the following mtl_drain to clear
// Do not wait indefinitely in display-link mode: see the note at STALL_NS. Only frames "not yet taken by the display link" are dropped; the one already on its way to the screen is waited for until handed to the GPU (same dropping as when entering the background)
void mtl_present_wait(uint64_t tk) {
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_done < tk && !atomic_load(&g_present_discard)) {
        if (!g_link_owns_layer) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }
        if (g_pq_read >= tk) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }   // 这张已被节拍取走、正在上屏：等它交给 GPU（GPU 完成回调会叫醒） | This one has already been taken by the display link and is being presented: wait for it to be handed to the GPU (the GPU completion callback will wake us)
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += (long)STALL_NS; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        // 已认定在停顿里（节拍回来时回调先清 g_stall_t0、再记节拍时刻）：不再每帧白等 100 ms，直接丢，游戏按自己的节拍等待超时（约两拍）走
        // Already determined to be in a stall (when the display link comes back the callback first clears g_stall_t0, then records the display-link tick time): no longer wait 100 ms in vain every frame, drop directly, and the game proceeds by its own display-link wait timeout (about two ticks)
        if (!g_stall_t0 && (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT || g_pq_done >= tk)) continue;
        uint64_t now = now_ns(), last = __atomic_load_n(&g_link_tick_ns, __ATOMIC_RELAXED);
        if (g_pq_read >= tk) continue;
        if (now - last < STALL_NS) { pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts); continue; }   // 节拍还在走，只是这张还没轮到：接着等（有上限，不空转） | The display link is still running, it is just not this one's turn yet: keep waiting (bounded, no busy spinning)
        uint64_t n = tk - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= tk; k++) g_pres[k % PRING] = (pres_ent){k, -1};
        g_link_dropped += n; g_pq_read = tk;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < tk) g_pq_done = tk;
        pthread_cond_broadcast(&g_pq_cv);
        if (!g_stall_t0) { g_stall_t0 = last ? last : now; g_stall_drop = 0; g_stall_red = 0; g_stall_logged = 0; g_stall_n++; }
        g_stall_drop += n;
        int active = dm_focus_now() == 1; uint64_t since = dm_focus_in_since(), from = g_stall_t0 > since ? g_stall_t0 : since;
        if (active && !g_stall_logged && now - from >= STALL_NS) { g_stall_logged = 1;
            MLOG("显示节拍 %.0f ms 没回调（激活中）：丢掉排队未上屏的 %llu 帧，游戏继续（第 %llu 段；节拍回来自动照常）", (now - from) / 1e6, (unsigned long long)n, (unsigned long long)g_stall_n); }
        if (active && !g_rescue_mode && g_dm_present_link && now - from >= LINK_RESCUE_NS) {
            g_rescue_mode = g_dm_present_link; g_dm_present_link = 0; g_rescue_n++; g_rescue_back_at = now + g_rescue_back_ns;
            MLOG("显示节拍激活中停了 %.0f ms：改为自己取 drawable 出图（第 %llu 次），%.0f 秒后再换回节拍试", (now - from) / 1e6, (unsigned long long)g_rescue_n, g_rescue_back_ns / 1e9);
            if (g_rescue_back_ns < 60000000000ull) g_rescue_back_ns *= 2;
        }
        if (active && !g_stall_red && now - from >= STALL_RED_NS) { g_stall_red = 1; MLOG("【红】显示节拍激活中连续 %.1f 秒没回调，出图全靠兜底丢帧，画面不动", (now - from) / 1e9); }
    }
    pthread_mutex_unlock(&g_pq_mu);
}
// 等已入队的出图全部处理完（mtl_finish = nvnQueueFinish 等用）：走 mtl_present_wait，节拍停了一样有兜底（真机：停节拍期间游戏调了一次 QueueFinish，
// 原先这里不带超时地等，排队的 3 帧没人取，游戏停住；Mac 上那两局没撞到这个调用）
// Wait until all queued presents have been processed (used by mtl_finish = nvnQueueFinish etc.): goes through mtl_present_wait, so there is still a fallback when the display link stops (real device: the game called QueueFinish once while the display link was stopped,
// and this used to wait here without a timeout; nobody took the 3 queued frames and the game stalled; those two runs on Mac did not hit this call)
void mtl_present_flush(void) { pthread_mutex_lock(&g_pq_mu); uint64_t h = g_pq_in; pthread_mutex_unlock(&g_pq_mu); mtl_present_wait(h); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_pass; *draws = g_stat_draw; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = g_stat_args_enc; *reuse = g_stat_args_reuse; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = 0; *skip = 0; }
uint64_t mtl_ds_omit(void) { return g_stat_ds_omit; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = g_bw_load; *store = g_bw_store; *full = g_pass_full_rw; }
uint64_t mtl_udc_saved_bytes(void) { return g_udc_saved_bytes; }
int mtl_has_window(void) { return front_metal_layer() != NULL; }
int mtl_link_active(void) { return link_on(); }
double mtl_link_tick_age_ms(void) { uint64_t t = __atomic_load_n(&g_link_tick_ns, __ATOMIC_RELAXED); return t ? (now_ns() - t) / 1e6 : -1; }
void mtl_link_pause(int on) { CAMetalDisplayLink *l = g_link; if (l) l.paused = on != 0; }   // 测试用（DM_TEST_BG）：模仿系统在后台停掉节拍回调 | For testing (DM_TEST_BG): imitates the system stopping the display-link callback in the background
int mtl_link_wait_tick(uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    // 只在跑得比显示快时才等节拍：上一帧开工之后若已经过了节拍（这一帧已落后），立刻开工，不再白等到下一个节拍——
    // 否则帧时间略大于一个节拍时会「错过节拍 → 再等一整拍」，帧率直接锁到一半（Mac 有窗口负载下实测成串掉帧）
    // Wait for the display link only when running faster than the display: if a display-link tick has already passed since the previous frame started work (this frame is already behind), start immediately instead of waiting in vain for the next display-link tick --
    // otherwise when the frame time is slightly more than one display-link period it would "miss the tick -> wait another whole tick", locking the frame rate straight to half (measured on Mac under windowed load: runs of dropped frames)
    static uint64_t started_tick;   // 上一帧开工时的节拍号（只有游戏出图线程调本函数） | Display-link tick number when the previous frame started work (only the game's present thread calls this function)
    const uint64_t step = g_dm_present_link == 2 ? 2 : 1;   // 一帧占几拍：60 Hz 节拍 1 拍；120 Hz 节拍 2 拍（游戏仍按 60 帧出） | How many display-link ticks one frame takes: 1 with a 60 Hz display link; 2 with a 120 Hz display link (the game still outputs 60 frames)
    pthread_mutex_lock(&g_link_mu);
    if (g_link_tick_n >= started_tick + step) { started_tick = g_link_tick_n; pthread_mutex_unlock(&g_link_mu); return 1; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ns / 1000000000ull; ts.tv_nsec += timeout_ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_link_tick_n < started_tick + step) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_link_mu); return 0; }
    started_tick = g_link_tick_n;
    pthread_mutex_unlock(&g_link_mu); return 1;
}

static id<CAMetalDrawable> g_prefetched_drawable; // 合成直画 drawable 用 | Used for compositing straight onto the drawable
id<CAMetalDrawable> mtl_present_take_prefetched(void) { return g_prefetched_drawable; }

#ifdef NVN_RT_RANGE_TEST
#include "rt_range_check.inc"
#endif

// 看门狗（无进展）时调用：把图形侧所有阻塞等待点的状态写进日志——谁在等谁一眼可见（换 Metal 4 提交模型后出现过一次「跑到 900 帧后停住、无红行」）
// Called by the watchdog (no progress): writes the state of all blocking wait points on the graphics side to the log -- who is waiting for whom is visible at a glance (after switching to the Metal 4 submit model there was once a case of "ran to 900 frames then stopped, no failure line")
void mtl_wait_state_dump(void) {
    MLOG("等待点状态：主栅栏 已提交 %llu / 已完成 %llu；主分配器栅栏 [%llu %llu %llu]；上传 未完成 %d、分配器占用 [%d %d %d]、序号 %u、有未冲刷批次 %d；出图队列 入 %llu / 完成 %llu；主命令缓冲在编 %d、提交线程已设 %d",
         (unsigned long long)g_fence, (unsigned long long)g_done, (unsigned long long)g_alloc_fence[0], (unsigned long long)g_alloc_fence[1], (unsigned long long)g_alloc_fence[2],
         atomic_load(&g_up_pending), atomic_load(&g_up_alloc_busy[0]), atomic_load(&g_up_alloc_busy[1]), atomic_load(&g_up_alloc_busy[2]), g_up_seq, g_up_cb != nil,
         (unsigned long long)g_pq_in, (unsigned long long)g_pq_done, g_cb != nil, atomic_load(&g_sub_thread_set));
}
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = g_alloc_waits; *grown = g_alloc_grown; *n = g_alloc_n; }   // 主提交分配器池：等过几次、扩过几次、现有几个 | Main submit allocator pool: how many times it waited, how many times it grew, how many exist now
#pragma clang attribute pop
