// NVN 宿主侧对象布局与命令记录格式：hle_nvn.c（接口层，写）与 nvn_mtl.m（Metal 回放层，读）共用。
// 对象就地存在游戏分配的 NVN 对象内存里，大小不超过 nvn.h 的保留大小（FITS 静态断言）。
// 状态对象（Blend / Color / … 4–0x18 字节）是游戏按值拷贝的小结构，布局用位域就地打包。
// NVN host-side object layout and command record format: shared by hle_nvn.c (interface layer, writes) and nvn_mtl.m (Metal replay layer, reads).
// Objects are stored in place in the NVN object memory allocated by the game, with size not exceeding the reserved size in nvn.h (FITS static assertion).
// State objects (Blend / Color / ... 4–0x18 bytes) are small structs the game copies by value; their layout is packed in place with bit-fields.
#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn.h"
#include "nvn_tex.h"

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " 超出 " #H " 的保留大小")

typedef struct { uint32_t flags; } dev_builder_t;                       FITS(dev_builder_t, NVNdeviceBuilder);
typedef struct { uint32_t magic, flags, origin, depth_mode; void *dbg_cb, *dbg_data; } dev_t_;  FITS(dev_t_, NVNdevice);

typedef struct { NVNdevice *dev; uint32_t flags; uint64_t cmd_sz, comp_sz, ctl_sz, flush; void *mem; uint64_t mem_sz; } queue_builder_t;
FITS(queue_builder_t, NVNqueueBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits; } queue_t;                 FITS(queue_t, NVNqueue);

typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;  FITS(pool_builder_t, NVNmemoryPoolBuilder);
typedef struct { uint32_t magic, flags; uint8_t *mem; uint64_t size; } pool_t;                FITS(pool_t, NVNmemoryPool);

typedef struct { const pool_t *pool; int64_t off; int32_t n; } dpool_t;                       // 贴图 / 采样器描述符池 | Texture / sampler descriptor pool
FITS(dpool_t, NVNtexturePool); FITS(dpool_t, NVNsamplerPool);

typedef struct {
    NVNdevice *dev; nvn_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
} tex_builder_t;                                                                             FITS(tex_builder_t, NVNtextureBuilder);
typedef struct {
    uint32_t magic; nvn_texdesc d; nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off; uint8_t has_layout, layout[8];
    uint64_t serial;                     // 初始化序号：回放层按它在宿主表里找 MTLTexture（游戏可能按值拷贝对象，不能存对象指针） | Initialization serial: the replay layer uses it to find the MTLTexture in the host table (the game may copy the object by value, so object pointers cannot be stored)
    const void *snap;                    // 本序号的不可变副本（hle_nvn.c snap_tex）：命令载荷存它，不存游戏对象地址 | This serial's immutable snapshot (hle_nvn.c snap_tex): command payloads store it, not the game object's address
} tex_t;                                                                                     FITS(tex_t, NVNtexture);
typedef struct { uint8_t set_levels, set_layers, set_format, set_target; int32_t base_level, nlevels, min_layer, nlayers, format, target; } view_t;
FITS(view_t, NVNtextureView);

// 采样器参数（builder 与对象共用一份）
// Sampler parameters (one copy shared by builder and object)
typedef struct { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func; float lod_min, lod_max, lod_bias, aniso, border[4]; } samp_par;   // 枚举值都 < 256 | Enum values are all < 256
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                                FITS(samp_builder_t, NVNsamplerBuilder);
typedef struct { uint32_t magic; samp_par p; } samp_t;                                        FITS(samp_t, NVNsampler);

typedef struct { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; const void *snap; } prog_t;     FITS(prog_t, NVNprogram);   // serial：回放层解析结果的表键；snap：本序号的不可变副本（同 tex_t） | serial: table key for the replay layer's resolved results; snap: this serial's immutable snapshot (same as tex_t)

typedef struct { uint32_t magic, fenced; uint64_t fence; } sync_t;                            FITS(sync_t, NVNsync);
typedef struct { const pool_t *pool; int64_t off; } ev_builder_t;                             FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; } ev_t;                              FITS(ev_t, NVNevent);

typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval; NVNtexture *tex[4]; } win_builder_t;
FITS(win_builder_t, NVNwindowBuilder);
// avail[i]：交换链第 i 张可再次获取的时刻（ns，单调时钟）= 它的显示时刻 + 一个出图周期；pace：本窗口的出图节拍器目标时刻
// avail[i]: time at which swapchain image i can be acquired again (ns, monotonic clock) = its display time + one present period; pace: target time of this window's present pacer
typedef struct { uint32_t magic; void *native; int32_t ntex, interval, next, crop[4]; NVNtexture *tex[4]; uint64_t presents, avail[4], pace, ticket[4]; } win_t;   // ticket[i]：第 i 张最近一次出图的票号 | ticket[i]: ticket of the most recent present of image i
FITS(win_t, NVNwindow);

// 命令缓冲：命令写进游戏给的命令内存（AddCommandMemory），句柄 = 段首地址；用完回调游戏补内存
// Command buffer: commands are written into the command memory given by the game (AddCommandMemory), handle = segment start address; when it runs out, the game is called back for more memory
typedef struct {
    uint32_t magic, recording;
    uint8_t *cur, *end;                  // 当前命令内存段的写指针 / 段尾 | Write pointer / end of the current command memory segment
    uint8_t *begin;                      // 本次录制的 BEGIN 记录 | BEGIN record of this recording
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    NVNdevice *dev;
} cmd_t;                                                                                     FITS(cmd_t, NVNcommandBuffer);

// ---------- 状态对象（默认值按 NVN / GL 默认） ----------
// ---------- State objects (defaults follow the NVN / GL defaults) ----------
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;            FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;            /* 每目标 4 位：r=1 g=2 b=4 a=8，置位 = 写 | 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ FITS(chmask_st, NVNchannelMaskState);
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3; } blend_st;
FITS(blend_st, NVNblendState);
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } ms_st;
FITS(ms_st, NVNmultisampleState);
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly_st;           FITS(poly_st, NVNpolygonState);
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} ds_st;                                                                                     FITS(ds_st, NVNdepthStencilState);
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } va_st;                         FITS(va_st, NVNvertexAttribState);
typedef struct { uint32_t stride, divisor; } vs_st;                                          FITS(vs_st, NVNvertexStreamState);

// ---------- 命令记录：{op, size（含头，8 对齐）} + 载荷 ----------
// ---------- Command records: {op, size (including header, aligned to 8)} + payload ----------
typedef struct { uint32_t op, size; } op_hdr;
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,                 // CPU 可见副作用 | CPU-visible side effects
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX,
    /* ==== ANDROID-BEGIN（非上游新增区，见 docs/android-isolation-analysis.md）====
     * 规矩：新的 op 一律追加在本区（保持在 OP_COUNT_ 之前、彼此相对顺序不变）；
     * 上游来合并时，他们的新 op 应插在 OP_CLEAR_TEX 之后、本区之前。严禁往枚举中间插——
     * 这些编号是"主程序 + 改写后的游戏模块"共用的暗号，两边必须同树一起重新生成（上游 #68 教训）。 */
    OP_SHADER_SCRATCH, OP_WAIT_SYNC, OP_FENCE_SYNC, OP_WAIT_EVENT,
    /* ==== ANDROID-END ==== */
    OP_COUNT_
};
// 载荷（紧跟 op_hdr）。贴图 / 程序指针都指向录制时按序号留下的不可变副本（snap_tex / snap_prog），游戏之后改写或重建对象不影响已录的命令
// Payload (right after op_hdr). Texture / program pointers point to the immutable snapshots kept by serial at record time (snap_tex / snap_prog); later rewrites or rebuilds of the object by the game do not affect recorded commands
typedef struct { int32_t n; const tex_t *color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t *depth; view_t dview; uint8_t has_dview; } p_set_rt;
typedef struct { int32_t index, mask; float rgba[4]; } p_clear_color;
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } p_clear_ds;
typedef struct { int32_t first, n; float v[16][6]; } p_viewports;          // x, y, w, h, 近, 远（近远由 DEPTH_RANGE 覆盖） | x, y, w, h, near, far (near and far are overridden by DEPTH_RANGE)
typedef struct { int32_t x, y, w, h; } p_scissor;
typedef struct { float n, f; } p_depth_range;
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
typedef struct { int32_t kind; uint8_t bytes[0x18]; } p_bind_state;      // kind：0 blend 1 color 2 chmask 3 ms 4 poly 5 ds
typedef struct { int32_t first, n; uint32_t st[16]; } p_bind_vattr;
typedef struct { int32_t first, n; vs_st st[16]; } p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } p_bind_vbuf;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;  // UBO / SSBO
typedef struct { int32_t stage, first, n; uint64_t h[32]; } p_bind_handles; // 分离贴图 / 采样器句柄 | Separate texture / sampler handles
typedef struct { int32_t face, value_mask, mask, ref, which; } p_stencil;  // which：0 值掩码 1 写掩码 2 参考值 | which: 0 value mask 1 write mask 2 reference value
typedef struct { float factor, units, clamp; } p_poly_offset;
typedef struct { int32_t prim, first, count; } p_draw;
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf; } p_draw_idx;
typedef struct { int32_t x, y, z; uint64_t caller; } p_dispatch;   // caller：游戏调用点返回地址（定位派发者） | caller: return address of the game call site (to locate the dispatcher)
typedef struct { const dpool_t *pool; } p_set_pool;
typedef struct { uint64_t src; const tex_t *tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; } p_copy_b2t;
typedef struct { const tex_t *src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } p_copy_t2t;   // T2B 复用：src 存目标缓冲地址 | Reused for T2B: src stores the destination buffer address
typedef struct { const tex_t *tex; view_t view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; } p_clear_tex;
typedef struct { int64_t stride; } p_copy_stride;
typedef struct { float rgba[4]; } p_blend_color;
/* ==== ANDROID-BEGIN（非上游新增区）==== */
typedef struct { sync_t *sync; int32_t condition, flags; } p_sync;
/* ==== ANDROID-END ==== */

// Metal 回放层入口（nvn_mtl.m）：把一组命令句柄回放进当前提交
// Entry point of the Metal replay layer (nvn_mtl.m): replays a set of command handles into the current submission
void mtl_exec(const uint8_t *handle);
void mtl_exec_rest(const uint64_t *handles, int n);   // 告知本次提交里当前句柄之后还有哪些句柄（通道前瞻跨句柄用） | Tells which handles in this submission still follow the current handle (used by the pass lookahead across handles)
// 出图：把交换链贴图画到前端 CAMetalLayer（无窗口时只计数）
// Present: draw the swapchain texture to the front-end CAMetalLayer (only counts when there is no window)
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]);   // 出图入队、立即返回票号；period_s：该张至少显示的时长（= 出图周期，0 = 不限） | Enqueue a present and return the ticket immediately; period_s: minimum time this image is displayed (= present period, 0 = unlimited)
void mtl_present_wait(uint64_t ticket);                    // 等该票号的出图处理完（blit 已提交到 GPU 队列） | Wait until the present with this ticket has been processed (blit committed to the GPU queue)
void mtl_present_flush(void);                              // 等已入队的出图全部处理完 | Wait until all enqueued presents have been processed
void mtl_present_test_stall(uint64_t ns);                  // 单测：出图线程每次出图前额外停顿 ns | Unit test: extra pause in ns before each present on the present thread
int mtl_display_hz(void);                                   // 有窗口时屏幕最高刷新率（Hz），无窗口 0 | Maximum refresh rate of the screen (Hz) when there is a window, 0 without a window
double mtl_display_refresh(void);                           // 同上，不取整（窗口所在屏） | Same as above, not rounded (screen the window is on)
double mtl_pace_hz(void);                                   // 出图帧率：刷新率的整数分之一里不低于目标（60 / 省电 30）的最小一档 | Present frame rate: the smallest tier among integer fractions of the refresh rate that is not below the target (60 / power saving 30)
double dm_pace_hz(double screen_hz, double floor_hz);       // 上面的纯算式（单测用） | Pure formula of the above (for unit tests)
void mtl_set_tex_pool_n(int n);                            // 贴图描述符池容量：池外的贴图号按「空槽」处理（读出 0） | Texture descriptor pool capacity: texture IDs outside the pool are treated as 'empty slots' (read 0)
// 计时埋点（Mac / iOS 共用）：提交按帧号打标；GPU 时间 = 该帧全部命令缓冲 GPUEnd−GPUStart 之和，全部完成后才可读（否则 −1）
// Timing instrumentation (shared by Mac / iOS): submissions are tagged with the frame number; GPU time = sum of GPUEnd−GPUStart over all of that frame's command buffers, readable only after all complete (otherwise −1)
void mtl_set_frame(uint64_t f);
uint64_t mtl_host_latch(void);   // 游戏线程提交入队时锁存宿主写的画法开关（冻结位 / 强制全画），随任务带到翻译线程 | When the game thread enqueues a submit it latches the host-written drawing switches (freeze bit / force full draw), carried with the job to the translation thread
void mtl_host_apply(uint64_t v);   // 翻译线程执行那次提交前生效 | Takes effect before the translation thread runs that submit
void mtl_ub_present(double iv_ms);   // 同进程交替测量（DM_NVN_UB）：出图线程每帧报实际上屏间隔 | Same-process alternating measurement (DM_NVN_UB): the present thread reports the actual present interval every frame
double mtl_gpu_ms(uint64_t f);
void mtl_sub_index(const uint64_t *h, int n);   // 提交开始：按回放顺序建写回前瞻索引（nvn_mtl.m） | Start of submission: build the store lookahead index in replay order (nvn_mtl.m)
void xv_test_counts(int *dc, int *viol);
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1);   // 该帧最早 GPUStart / 最晚 GPUEnd（mach 绝对时间 ns）；未全部完成或无 GPU 时间返回 0 | The frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns); returns 0 if not all complete or there is no GPU time
void mtl_stats(uint64_t *passes, uint64_t *draws);
void mtl_args_stats(uint64_t *enc, uint64_t *reuse);
void mtl_use_stats(uint64_t *call, uint64_t *skip);
uint64_t mtl_ds_omit(void);   // 前瞻证明不用深度模板、未挂深度附件的通道累计数 | Cumulative number of passes with no depth attachment because the lookahead proved depth-stencil unused
uint64_t mtl_drawable_ns_take(void);   // 取走并清零：出图里等 nextDrawable 的累计 ns   // useResource 实发 / 同编码器去重跳过累计次数   // 参数缓冲新编 / 复用累计次数 | Take and zero: cumulative ns spent waiting for nextDrawable in present   // Cumulative useResource calls actually issued / skipped by same-encoder dedup   // Cumulative argument buffer new-encode / reuse counts
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes);   // 附件读入 / 写出累计字节、全量读写通道累计数 | Cumulative attachment load / store bytes, cumulative number of full load-store passes
uint64_t mtl_udc_saved_bytes(void);                                      // DM_NVN_UNWRITTEN_DC 实际省下的写回字节 | Write-back bytes actually saved by DM_NVN_UNWRITTEN_DC
int mtl_has_window(void);
int mtl_warm_wait(int timeout_ms);
int mtl_link_active(void);
int mtl_link_wait_tick(uint64_t timeout_ns);
double mtl_presented_time(uint64_t n);
   // 第 n 次出图（从 1 计）的实际呈现时刻（秒）；0 = 未就绪，−1 = 被丢弃
   // Actual presentation time (seconds) of the n-th present (counted from 1); 0 = not ready, −1 = dropped
// 贴图释放（Finalize 时）
// Texture release (at Finalize)
void mtl_tex_release(tex_t *t);
#ifdef __ANDROID__
void mtl_set_window_origin_mode(int mode);
#endif
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
uint64_t mtl_pkg_miss(void);
uint64_t mtl_upload_ns_take(void);   // 取走并清零：上次以来提交线程上预排布贴图上传耗时   // DM_NVN_DIRTYCHECK 诊断：漏检次数   // 预排布贴图：首传次数、因池内容变化而重传次数 | Take and zero: time spent uploading packaged textures on the submit thread since last time   // DM_NVN_DIRTYCHECK diagnostic: number of missed detections   // Packaged textures: number of first uploads, number of re-uploads due to pool content changes
uint64_t mtl_upload_gpu_ns_take(void);   // 取走并清零：上传命令缓冲的显卡执行耗时（GPUEndTime − GPUStartTime 累加） | Take and zero: GPU execution time of upload command buffers (sum of GPUEndTime − GPUStartTime)
void mtl_create_stats(uint64_t *lib, uint64_t *pso);   // 累计：提交线程上同步载入的着色器库数、同步新建的渲染 + 计算管线数（预热领用的不算） | Cumulative: number of shader libraries loaded synchronously on the submit thread, number of render + compute pipelines created synchronously (those taken from warm-up do not count)
// CPU 读贴图：level / layer0 起，按 row / img 字节步长写进 dst
// CPU texture read: starting at level / layer0, written into dst with row / img byte strides
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
// 描述符池登记（贴图号 / 采样器号 → 对象快照）、程序释放
// Descriptor pool registration (texture ID / sampler ID → object snapshot), program release
void mtl_reg_tex(int id, const tex_t *t, const view_t *v);
void mtl_reg_samp(int id, const samp_par *p);
void mtl_prog_release(prog_t *p);
// 新序号（> 0，全局递增）
// New serial (> 0, globally increasing)
uint64_t nvn_new_serial(void);
