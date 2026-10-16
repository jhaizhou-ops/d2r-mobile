/* First Android Vulkan replay implementation for the original NVN recorder.
 * This file preserves the recorder's mtl_* C interface to avoid guest changes.
 * Unsupported semantics abort with a named boundary; no draw/dispatch is dropped.
 * The initial queue is deliberately synchronous. Guest-visible writes/events
 * are published only after a real fence has completed.
 */
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include "nvn_obj.h"
#include "../../android/nvn_platform_clock.h"
#include "nvn_mtl.h"
#include "nvn_dirty.h"
#include "nvn_dirty_summary.h"
#include "nvn_bright_spv.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_bcdec.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include <math.h>
#include <dlfcn.h>
#include <unistd.h>
#include "../../android/nvn_platform_backend.h"

void dm_guest_write_prepare(void *address,size_t length);

#define LIMIT_POOLS 512
#define LIMIT_OBJECTS 4096
#define LIMIT_BINDINGS 64
#define LIMIT_WRITES 4096
#define LIMIT_PSOS 2048
#define PSO_TABLE_SIZE 4096
#define DRIVER_TABLE_BYTES 0x968
#define DRIVER_TABLE_SLOTS 8192
#ifndef TEMP_POOL_COUNT
#define TEMP_POOL_COUNT 512
#endif
#ifndef TEMP_POOL_BYTES
#define TEMP_POOL_BYTES (128ULL*1024*1024)
#endif
#define CHECK(x) do { VkResult r_=(x); if(r_!=VK_SUCCESS) die(#x,(long long)r_); } while(0)
static VkResult cpu_work_wait_fences(VkDevice device,uint32_t count,const VkFence *fences,VkBool32 all,uint64_t timeout) {
    uint64_t token=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_FENCE);
    VkResult result=vkWaitForFences(device,count,fences,all,timeout);dm_android_nvn_work_wait_end(token);return result;
}
static VkResult cpu_work_queue_idle(VkQueue queue) {
    uint64_t token=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_QUEUE_IDLE);
    VkResult result=vkQueueWaitIdle(queue);dm_android_nvn_work_wait_end(token);return result;
}
static _Noreturn void die(const char *where,long long value) {
    fprintf(stderr,"[nvn-vulkan] STOP %s (%lld)\n",where,value); fflush(stderr); abort();
}
#include "nvn_vk_dispatch.h"
/* Any command that can write memory or change image state marks the command buffer dirty;
 * all_barrier() omits a full barrier only when nothing was recorded since the previous one. */
enum { HZ_DRAW=1,HZ_ATTACH,HZ_TRANSFER,HZ_COMPUTE };
static void hz_note(int kind);
#undef vkCmdCopyImage
#define vkCmdCopyImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&1)?(void)0:dm_vkCmdCopyImage(__VA_ARGS__))
#undef vkCmdCopyImageToBuffer
#define vkCmdCopyImageToBuffer(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&16)?(void)0:dm_vkCmdCopyImageToBuffer(__VA_ARGS__))
#undef vkCmdCopyBufferToImage
#define vkCmdCopyBufferToImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&8)?(void)0:dm_vkCmdCopyBufferToImage(__VA_ARGS__))
#undef vkCmdCopyBuffer
#define vkCmdCopyBuffer(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&4)?(void)0:dm_vkCmdCopyBuffer(__VA_ARGS__))
#undef vkCmdClearAttachments
#define vkCmdClearAttachments(...) (S.command_dirty=1,hz_note(HZ_ATTACH),dm_vkCmdClearAttachments(__VA_ARGS__))
#undef vkCmdBlitImage
#define vkCmdBlitImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&32)?(void)0:dm_vkCmdBlitImage(__VA_ARGS__))
#undef vkCmdUpdateBuffer
#define vkCmdUpdateBuffer(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&64)?(void)0:dm_vkCmdUpdateBuffer(__VA_ARGS__))
#undef vkCmdBeginRenderPass
#define vkCmdBeginRenderPass(...) (S.command_dirty=1,dm_vkCmdBeginRenderPass(__VA_ARGS__))
#undef vkCmdEndRenderPass
#define vkCmdEndRenderPass(...) (S.command_dirty=1,dm_vkCmdEndRenderPass(__VA_ARGS__))
#undef vkCmdDraw
#define vkCmdDraw(...) (S.command_dirty=1,hz_note(HZ_DRAW),dm_vkCmdDraw(__VA_ARGS__))
#undef vkCmdDrawIndexed
#define vkCmdDrawIndexed(...) (S.command_dirty=1,hz_note(HZ_DRAW),dm_vkCmdDrawIndexed(__VA_ARGS__))
#undef vkCmdDispatch
#define vkCmdDispatch(...) (S.command_dirty=1,hz_note(HZ_COMPUTE),dm_vkCmdDispatch(__VA_ARGS__))
#undef vkCmdClearColorImage
#define vkCmdClearColorImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&2)?(void)0:dm_vkCmdClearColorImage(__VA_ARGS__))
#undef vkCmdClearDepthStencilImage
#define vkCmdClearDepthStencilImage(...) (S.command_dirty=1,hz_note(HZ_TRANSFER),(G.diag_skip_xfer&2)?(void)0:dm_vkCmdClearDepthStencilImage(__VA_ARGS__))
#undef vkCmdPipelineBarrier
#define vkCmdPipelineBarrier(...) (S.command_dirty=1,dm_vkCmdPipelineBarrier(__VA_ARGS__))
#include "nvn_vk_storage_bias.h"
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *map; uint64_t size,allocation_size;VkBufferUsageFlags usage;int cached;uint64_t dirty; } VBuffer;
enum { TEMP_OTHER,TEMP_DRIVER,TEMP_ALIGNED,TEMP_COMPARISON,TEMP_ROLES };
typedef struct { const uint8_t *guest; uint64_t size,serial; VBuffer v; } VPool;
typedef struct { uint64_t address,size,pool_serial,stamp,age; } PoolUpload;
typedef struct { uint64_t epoch,ssbo,tex;VBuffer *table; } DriverCacheEntry;
#define UPLOAD_BUCKETS 4096
#define UPLOAD_WAYS 4
typedef struct { uint32_t set,binding,type,kind; int32_t slot,sampler; uint32_t count,flags; } RBinding;
typedef struct { uint32_t magic,version,stage,nb,na,c1n,ncap,local[3],fout,reserved; } RHeader;
#include "nvn_vk_binding_plan.h"
typedef struct {
    char key[17]; RHeader h; RBinding b[LIMIT_BINDINGS]; uint32_t attr[32][2],caps[32];
    VBuffer c1; VkShaderModule module; VkDescriptorSetLayout layout; uint64_t fetch_bindings,dref_bindings,large_bindings_seen;
    uint64_t image_bindings,sampled_bindings,side_effect_bindings;   // 逐 draw 预筛：图像/采样/副作用绑定掩码 | per-draw prefilter masks: image / sampled / side-effect bindings
    uint64_t dyn_bindings; uint32_t dyn_count;   // kind-2 uniform bindings switched to DYNAMIC descriptors | 改成动态描述符的 kind2 统一缓冲绑定
    SBInfo storage_bias;
    BindingPlan *plan;
} VShader;
typedef struct { uint64_t serial,incarnation; tex_t source; VkImage image; VkDeviceMemory memory;
    VkImageView view; VkFormat format; VkImageLayout layout; uint32_t width,height,depth,bpp; int cpu_owned; uint64_t upload_stamp,version;
    uint8_t *cpu_snapshot;
    VkImage sample_copy;VkDeviceMemory sample_memory;VkImageView sample_view;VkImageLayout sample_layout;
    VkImage shadow;VkDeviceMemory shadow_memory;VkImageView shadow_view;VkFormat shadow_format;VkImageLayout shadow_layout;uint64_t shadow_version;uint64_t other_version,sample_other;uint32_t dirty_levels,sc_valid,sc_level,sc_levels,sc_layer,sc_layers;
    uint8_t bc_block;   // 压缩直传时 = BC 块字节数（8/16）；0 = 非压缩 | compressed passthrough: BC block bytes (8/16); 0 = not compressed
    VkImageLayout lvl[16];int mixed; /* per-mip layouts, valid only while mixed (then layout is stale) */ } VTexture;
static void tex_dirty(VTexture *t){t->version++;t->other_version++;}
static void tex_attach_dirty(VTexture *t,unsigned level){t->version++;t->dirty_levels|=1u<<(level&31);}
typedef struct { int live; tex_t texture; view_t view; uint8_t has_view; uint64_t revision;TextureRef resident; } TSlot;
typedef struct { int live; samp_par source; VkSampler sampler; } SSlot;
typedef struct { volatile uint32_t *addr; uint32_t mode,value; } Event;
typedef struct { uint32_t kind,query; uint64_t address; } CounterOp;
typedef struct { Range range; VBuffer *buffer; int written; } AlignedBinding;
typedef struct { Range range;VBuffer *buffer;int write_count; } ReadSnapshot;
typedef struct { uint32_t hash;VBuffer *buffer; } DriverTable;
typedef struct { uint64_t serial; unsigned roles; } ImageUse;
typedef struct { const uint8_t *entry;uint64_t hash; } ShaderEntry;
typedef struct { uint64_t hash;VShader *shader;VkDescriptorSet set;uint32_t nb,off; } DescEntry;
typedef struct { uint64_t key,draws;uint32_t w,h,n,passes;double ms; } PassAcc;
typedef struct { const void *control;uint32_t stage,valid;VShader *shader;uint8_t bytes[0x880]; } ControlCache;
#define DESC_ENTRIES 8192
#define DESC_TABLE 16384
#define DESC_ARENA (8u<<20)
#define CONTROL_CACHE 4096
typedef struct { VBuffer *buffer;uint64_t bytes,offset;char path[1400]; } TraceCapture;
/* Only one subpass and single-sample attachments are supported here. Keep
 * the complete render-pass description: reuse never depends on image handles,
 * framebuffer size or Vulkan render-pass handle allocation. */
typedef struct {
    uint32_t count,color_count,has_depth,dependency_count;
    VkAttachmentDescription attachments[9];
    VkAttachmentReference color[8],depth;
    VkSubpassDependency dependencies[3];
} PassKey;

/* Submission-owned resources are immutable and remain live until its fence.
 * Include the complete view and framebuffer description in reuse keys. */
typedef struct {
    VkImage image;VkImageViewCreateFlags flags;VkImageViewType type;VkFormat format;
    VkComponentMapping components;VkImageSubresourceRange range;
} ImageViewKey;
typedef struct { PassKey pass;VkImageView views[9];uint32_t width,height; } FramebufferKey;
typedef struct { ImageViewKey key; VkImageView view; uint64_t gen; } RtViewEntry;
typedef struct { FramebufferKey key; VkFramebuffer fb; uint64_t gen; } RtFbEntry;
typedef struct { VkImageView view; VkFramebuffer fb; } RtDead;
#define VIEW_TABLE_SIZE 8192
typedef struct {
    uint32_t compute,shader[2],flags,topology,cull,front;
    uint32_t attribute_count,binding_count,color_count;
    VkVertexInputAttributeDescription attributes[32];
    VkVertexInputBindingDescription bindings[16];
    uint32_t depth_test,depth_write,depth_compare,stencil_test;
    VkStencilOpState stencil[2];
    VkPipelineColorBlendAttachmentState blend[8];
    uint32_t storage_bias[2][LIMIT_BINDINGS];
    PassKey pass;
} PsoKey;
typedef struct { VkPipeline pipeline; VkPipelineLayout layout; } Pso;
typedef struct { PsoKey key;Pso pso;uint64_t hash; } CachedPso;
typedef struct {
    const prog_t *program; p_set_rt rt; va_st attributes[16]; vs_st streams[16]; NVNbufferRange vbuf[16];
    Range uniform[6][16],storage[6][16]; uint64_t tex[6][128],samp[6][128],img[6][32];
    color_st color; chmask_st mask; blend_st blend[8]; poly_st polygon; ds_st ds; ms_st ms;
    VkViewport viewport; VkRect2D scissor; float blend_color[4]; uint64_t copy_stride;
    const dpool_t *texture_pool,*sampler_pool; Range scratch; uint32_t stencil_compare[2],stencil_write[2],stencil_ref[2];
} ReplayState;
#include "nvn_vk_writeback_plan.h"
#include "nvn_vk_command_state.h"
#include "nvn_vk_submission.h"
#include "nvn_vk_command_stream.h"
static struct {
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; VkQueue queue; uint32_t family; VkSampler fetch_sampler;
    VkPhysicalDeviceMemoryProperties memory; VkPhysicalDeviceProperties properties;
    VkPipelineCache pipeline_cache;
    VkSubmission submission_slot,submission_slot2;VkSubmission *current_submission;VkSemaphore swap_finished[16];int async_enabled,completing,async_drain_after_submit;uint64_t recording_epoch;
    VBuffer support,zero; VPool pools[LIMIT_POOLS]; int npool;
    PoolUpload pool_uploads[UPLOAD_BUCKETS][UPLOAD_WAYS];uint64_t upload_age;
    int disable_dirty_uploads,enable_writeback_prepare,enable_writeback_coalesce,event_trace;
    uint64_t profile_pool_uploaded_bytes,profile_pool_reused_ranges,profile_pool_reused_bytes;
    VShader shaders[LIMIT_OBJECTS]; int nshader; VTexture textures[LIMIT_OBJECTS]; int ntexture;
    uint32_t texture_used,texture_free_count,texture_free[LIMIT_OBJECTS],texture_table[2*LIMIT_OBJECTS];
    uint64_t texture_incarnation,registry_revision,pool_revision,binding_revision[6],ssbo_revision[6],texbind_revision[6],driver_revision[6][2];DriverCacheEntry driver_cache[6][2];
    int disable_binding_plans,plan_precise_state,enable_command_cache;
    uint64_t profile_cmd_issued[NVN_VK_CMD_STATE_COUNT],profile_cmd_skipped[NVN_VK_CMD_STATE_COUNT];
    NvnDirtyQueryStats profile_dirty_base;
    uint64_t profile_plan_hits,profile_plan_misses,profile_plan_fallbacks,profile_plan_buffers,profile_plan_images,profile_driver_revision_hits;
    TSlot *tslot; size_t texture_slots; SSlot sslot[LIMIT_OBJECTS]; int texture_pool_n;

    VBuffer *retired_buffers[TEMP_POOL_COUNT];uint64_t retired_age[TEMP_POOL_COUNT],retired_bytes,retire_clock;
    unsigned nretired;int disable_temp_pool;

    int disable_driver_table_reuse;

    uint32_t timestamp_bits;int precise_occlusion,disable_query_batch,enable_query_reset_range;
    uint64_t profile_query_reset_calls,profile_query_reset_slots;
    uint64_t samples_passed;

    CachedPso psos[LIMIT_PSOS];uint32_t pso_table[PSO_TABLE_SIZE],npso;
    uint32_t last_pso_slot;int enable_pso_last;
    int disable_pso_reuse;

    int disable_resource_cache;
    uint64_t profile_view_hits,profile_view_misses,profile_framebuffer_hits,profile_framebuffer_misses;
    int active,feedback_layout,shader_draw_parameters;
    ReplayState state; VkPhysicalDeviceSubgroupProperties subgroup;
    uint8_t *shader_index; size_t index_bytes; char shader_root[1024]; ANativeWindow *window;
    ShaderEntry *shader_entries;size_t shader_entry_capacity;
    VkSurfaceKHR surface; VkSwapchainKHR swapchain; VkImage *swap_images; VkImageLayout *swap_layouts;
    uint32_t nswap; VkFormat swap_format; VkExtent2D swap_extent; int display_hz;
    _Atomic uint64_t fence,completed,present_count; uint64_t draws,pass_count,uploads,pool_adds,frame;
    int trace_frame,trace_ui;_Atomic int window_origin;
    uint64_t profile_pipeline_ns,profile_wait_ns,profile_decode_ns,profile_submits,profile_start_ns,profile_draws,profile_frames;
    uint64_t profile_submit_candidates,profile_candidate_wait_ns,profile_writeback_bytes,profile_events,profile_reports;
    uint64_t profile_writeback_raw_bytes,profile_writeback_raw_ranges,profile_writeback_copy_ranges,profile_writeback_plan_cpu_ns;
    uint64_t profile_write_submits,profile_event_submits,profile_report_submits,profile_trace_submits,profile_cpu_readback_submits,profile_event_wait_submits;

    uint64_t profile_desc_alloc_ns,profile_desc_update_ns,profile_prep_ns,profile_lookup_ns,profile_pool_ns,profile_bind_ns,profile_render_ns,profile_present_ns,profile_state_ns,profile_vertex_ns,profile_pso_ns,profile_tail_ns;unsigned trace_meshes;
    uint64_t profile_bind_hits,profile_bind_full_calls,profile_bind_plan_ns,profile_bind_full_ns;   // bind_shader 细分：计划命中/全路径次数与各自耗时 | bind_shader split: plan hits / full-path calls and their times
    uint64_t profile_plan_rev,profile_plan_epoch,profile_plan_state,profile_plan_reg,profile_plan_pool;   // 计划失效原因 | plan-invalidation reasons
    uint64_t profile_plan_buf_none,profile_plan_buf_align,profile_plan_buf_addr,profile_plan_buf_pool,profile_plan_buf_off,profile_plan_buf_dynbig;   // 缓冲侧失效原因 | buffer-side miss reasons
    uint64_t profile_plan_img_check,profile_plan_img_layout,profile_plan_img_samp;   // 图像侧失效原因 | image-side miss reasons
    uint64_t profile_tail_bind_ns,profile_tail_state_ns,profile_tail_draw_ns;   // tail 子桶：管线+描述符 / 状态+查询 / 绘制调用 | tail sub-buckets
    uint64_t profile_rp_flush_ns,profile_rp_attach_ns,profile_rp_key_ns,profile_rp_begin_ns;   // render_begin 子桶：冲刷 / 附件 / 键+创建 / 编码 | render_begin sub-buckets
    uint64_t profile_rp_chunk_ns,profile_rp_scan_ns,profile_rp_create_ns,profile_tail_idx_ns;   // 细分：分块提交 / 键构建+扫描 / 创建对象 / 索引读
    uint64_t profile_chunk_end_ns,profile_chunk_submit_ns,profile_chunk_begin_ns;   // 分块提交三分：vkEndCommandBuffer / vkQueueSubmit / 新缓冲开始+状态重置
    uint64_t profile_query_breaks,profile_query_preresets,profile_pass_splits;
    uint64_t profile_counter_resets,profile_counter_reports,profile_counter_ops;
    uint64_t profile_full_clears,profile_scissor_clears;
    uint64_t profile_read_memo_hits,profile_read_memo_misses;
    uint64_t pass_end_draws[8192];
    int diag_barrier,diag_loadstore,disable_barrier_elision,disable_sample_copy_cache,barrier_stats,pass_timing;
    uint32_t npacc;

    PassAcc pacc[128];char skip_ps[17];struct{char key[17];uint32_t draws;}hot[64];uint32_t nhot;int disable_desc_cache,disable_control_cache,diag_no_occlusion,diag_img_usage,barrier_pass_elide,pass_side_effects,diag_no_pass_deps,precise_sync,draw_store,xition_vertex;uint64_t profile_wait_elided,profile_hz_barriers,hz_count[8];int hz_diag,chunk_draws,chunk_log,profile_detail,seg_index,diag_skip_draws,diag_skip_xfer,diag_skip_class,diag_shape,enable_narrow_access,diag_fixed_desc,disable_wait_elision,want_cached,wb_stage_enabled,wb_stage_type,disable_setrt_skip,disable_subres,disable_pass_reuse,disable_cpu_uniform_snapshot,query_pre_reset,pass_split_draws,diag_imprecise_occlusion,read_memo_enabled;uint64_t setrt_total,setrt_same;char pass_desc[96],last_ps[20],last_uses[100];int last_count,last_barrier_bits;uint64_t pass_draws0;uint64_t gts_busy[4],gts_span[4],gts_gap[4],gts_chunks[4],pres_acq_ns,pres_fence_ns,pres_idle_ns,pres_submit_ns,seg_begin_ns,seg_record_ns[4],seg_wait_ns[4],seg_tail_ns[4],seg_draws[4],seg_chunks[4];uint64_t profile_chunks;uint32_t barrier_bits[256],barrier_kept[256],barrier_nopass,barrier_sidefx;uint32_t barrier_sites[4096],pass_end_sites[8192];
    ControlCache *control_cache;
    uint64_t profile_desc_hits,profile_desc_misses,profile_control_hits,profile_control_misses,profile_control_evict,profile_control_changed,profile_barriers_skipped,profile_sample_copy_hits,profile_sample_copy_misses;
    uint64_t profile_passes,profile_barriers,profile_uniform_updates,profile_cpu_snapshots,profile_gpu_copies,profile_snapshot_reuses;char trace_shader[17];
    uint64_t profile_pso_hits,profile_pso_misses;
    uint64_t profile_pso_lookups,profile_pso_hash_calls,profile_pso_hash_ns,profile_pso_lookup_ns;
    uint64_t profile_pso_last_hits,profile_pso_last_misses,profile_pso_table_hits,profile_pso_probes,profile_pso_max_probe;
    uint64_t profile_replay_cpu_ns,profile_begin_cpu_ns,profile_commit_prepare_cpu_ns,profile_commit_wait_cpu_ns,profile_retire_cpu_ns,profile_writeback_cpu_ns,profile_present_cpu_ns;
    uint64_t profile_replay_calls,profile_begin_calls,profile_commit_calls,profile_writeback_calls,profile_present_calls;
    uint64_t profile_submission_creates,profile_submission_reuses,profile_submission_publications;
    uint64_t profile_command_records,profile_command_bytes,profile_command_lease_hits,profile_command_resolves;
    uint64_t profile_writeback_global_faults,profile_writeback_global_preps,profile_writeback_global_mp_calls,profile_writeback_global_mp_pages;
    uint64_t profile_driver_tables,profile_driver_table_reuses,profile_buffer_allocs,profile_buffer_bytes;
    uint64_t profile_temp_reuses,profile_temp_clear_ns,profile_buffer_allocate_ns,profile_temp_bytes[TEMP_ROLES];
    uint64_t profile_storage_bias_bindings,profile_storage_bias_bytes,profile_occlusion_queries;
    unsigned ntrace_textures;uint64_t trace_texture_serials[24];
    /* 持久渲染通道缓存（PassKey 纯值数据、对象不可变、与设备同寿命）| persistent render-pass cache */
    struct { PassKey key; VkRenderPass pass; } rp_cache[1024];
    uint64_t profile_rpc_hits,profile_rpc_creates,profile_rpc_overflow;
    /* 持久帧缓冲 + 附件视图缓存：纹理释放→G.rt_gen 代际失效；被替换的对象进退休列表，
     * 在提交围栏之后（completion）统一销毁。| persistent framebuffer + attachment view cache:
     * texture release bumps G.rt_gen; replaced objects go to a retirement list destroyed only
     * after the submission fence (completion). */
    RtViewEntry rt_views[1024]; RtFbEntry rt_fbs[1024];
    RtDead rt_dead[1024]; int nrt_dead; uint64_t rt_gen;
    uint64_t profile_rtv_hits,profile_rtv_creates,profile_rtv_stale,profile_rtf_hits,profile_rtf_creates,profile_rtf_stale;
    int fb_cache_enabled;
    pthread_mutex_t lock;
    void *transfer_memory; size_t transfer_size; int graphics_live;
} G={.lock=PTHREAD_MUTEX_INITIALIZER};
static void rt_dead_flush(void);
/* Recording helpers access the selected slot; completion receives its owner explicitly. */
#define S (*G.current_submission)
static void command_state_profile(const char *tag,uint64_t frame,uint64_t frames) {
    uint64_t issued=0,skipped=0;
    for(unsigned i=0;i<NVN_VK_CMD_STATE_COUNT;i++){issued+=G.profile_cmd_issued[i];skipped+=G.profile_cmd_skipped[i];}
    fprintf(stderr,"[nvn-vulkan] %s frame=%llu frames=%llu enabled=%d trace_bypass=%d issued=%llu skipped=%llu pipeline_issued=%llu pipeline_skipped=%llu descriptors_issued=%llu descriptors_skipped=%llu viewport_issued=%llu viewport_skipped=%llu scissor_issued=%llu scissor_skipped=%llu blend_issued=%llu blend_skipped=%llu vertex_issued=%llu vertex_skipped=%llu index_issued=%llu index_skipped=%llu\n",
        tag,(unsigned long long)frame,(unsigned long long)frames,G.enable_command_cache,G.trace_frame,
        (unsigned long long)issued,(unsigned long long)skipped,
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_PIPELINE],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_PIPELINE],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_DESCRIPTORS],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_DESCRIPTORS],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_VIEWPORT],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_VIEWPORT],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_SCISSOR],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_SCISSOR],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_BLEND],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_BLEND],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_VERTEX],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_VERTEX],
        (unsigned long long)G.profile_cmd_issued[NVN_VK_CMD_INDEX],(unsigned long long)G.profile_cmd_skipped[NVN_VK_CMD_INDEX]);
}
static void dirty_query_profile(const char *tag,uint64_t frame,uint64_t frames,int window) {
    NvnDirtyQueryStats stats,current;int enabled,recording;
    nvn_dirty_query_stats(&current,&enabled,&recording);stats=current;
    if(window){
        stats.calls-=G.profile_dirty_base.calls;stats.page_reads-=G.profile_dirty_base.page_reads;
        stats.block_reads-=G.profile_dirty_base.block_reads;stats.skipped_pages-=G.profile_dirty_base.skipped_pages;
        stats.range_fast_paths-=G.profile_dirty_base.range_fast_paths;G.profile_dirty_base=current;
        if(frames<60)return;
    }
    fprintf(stderr,"[nvn-vulkan] %s frame=%llu frames=%llu enabled=%d recording=%d scope=process calls=%llu page_reads=%llu block_reads=%llu skipped_pages=%llu range_fast_paths=%llu block_pages=%u\n",
        tag,(unsigned long long)frame,(unsigned long long)frames,enabled,recording,(unsigned long long)stats.calls,
        (unsigned long long)stats.page_reads,(unsigned long long)stats.block_reads,(unsigned long long)stats.skipped_pages,
        (unsigned long long)stats.range_fast_paths,NVN_DIRTY_BLOCK_PAGES);
}
static pthread_once_t init_once=PTHREAD_ONCE_INIT;
/* Thread CPU clocks must stay on their originating thread. Replay is inclusive
 * of event-wait commit/begin cuts; present includes its own sub_begin, and
 * writeback is a retire subset. These phase totals must not be summed blindly. */
static _Thread_local unsigned replay_cpu_depth,present_cpu_depth;
static _Thread_local uint64_t replay_cpu_started,present_cpu_started;
static char g_pso_cache_path[1024];   // PSO 缓存落盘路径（DM_FRAME_DIR/pso-cache.bin） | pipeline-cache blob path (DM_FRAME_DIR/pso-cache.bin)
static _Atomic unsigned long g_pso_create_count, g_pso_slow_count, g_pso_slow_total_ns;   // 冷建统计：g_pso2 未命中才会走到 vkCreate*Pipelines | cold-creation statistics: vkCreate*Pipelines is only reached on a g_pso2 miss
static int g_pso_cache_dirty,g_pso_cache_presents;
static int g_bc_passthrough;          // 设备支持 textureCompressionBC：压缩纹理直传 GPU | device supports textureCompressionBC: upload compressed textures as-is
static VkDebugUtilsMessengerEXT validation_messenger;
static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;
    static struct {int32_t id;unsigned count;} seen[128];static unsigned count;
    unsigned i=0;while(i<count && seen[i].id!=data->messageIdNumber)i++;
    if(i==count){if(count==128)return VK_FALSE;seen[count++].id=data->messageIdNumber;}
    if(seen[i].count++<10)fprintf(stderr,"[nvn-vulkan] VALIDATION severity=%#x draw=%llu id=%s message=%s\n",severity,(unsigned long long)G.draws,data->pMessageIdName?data->pMessageIdName:"unknown",data->pMessage);
    return VK_FALSE;
}

static inline uint64_t prof_now(void) { return G.profile_detail?dm_android_nvn_clock_ns():0; }
static uint32_t memory_type(uint32_t mask,VkMemoryPropertyFlags properties) {
    for(uint32_t i=0;i<G.memory.memoryTypeCount;i++) if((mask&(1u<<i)) &&
        (G.memory.memoryTypes[i].propertyFlags&properties)==properties) return i;
    die("memory type",properties);
}
static VBuffer buffer_create(uint64_t size,VkBufferUsageFlags usage,int zero) {
    uint64_t started=prof_now();VBuffer b={.size=size,.usage=usage}; if(!size || size>SIZE_MAX) die("buffer size",size);
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=usage,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE}; CHECK(vkCreateBuffer(G.device,&info,NULL,&b.b));
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(G.device,b.b,&r);
    b.allocation_size=r.size;
    VkMemoryAllocateInfo a={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,
        .memoryTypeIndex=memory_type(r.memoryTypeBits,G.want_cached?(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT):(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))};
    b.cached=G.want_cached&&!(G.memory.memoryTypes[a.memoryTypeIndex].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(G.device,&a,NULL,&b.m)); CHECK(vkBindBufferMemory(G.device,b.b,b.m,0));
    void *mapped=NULL; CHECK(vkMapMemory(G.device,b.m,0,size,0,&mapped)); b.map=mapped;if(zero)memset(b.map,0,size);
    G.profile_buffer_allocs++;G.profile_buffer_bytes+=size;G.profile_buffer_allocate_ns+=prof_now()-started;return b;
}
static VBuffer buffer_make(uint64_t size,VkBufferUsageFlags usage) { return buffer_create(size,usage,1); }
static void buffer_free(VBuffer *b) {
    if(b->map) vkUnmapMemory(G.device,b->m); if(b->b) vkDestroyBuffer(G.device,b->b,NULL);
    if(b->m) vkFreeMemory(G.device,b->m,NULL); memset(b,0,sizeof(*b));
}
static VBuffer *retired_take(unsigned index) {
    VBuffer *b=G.retired_buffers[index];G.retired_bytes-=b->allocation_size;
    unsigned last=--G.nretired;G.retired_buffers[index]=G.retired_buffers[last];G.retired_age[index]=G.retired_age[last];return b;
}
static void temporaries_retire(VkSubmission *sub) {
    /* Called only after fence completion and CPU writeback/capture. Entries
     * taken for a new submission are removed from this pool, so no live
     * command or another binding can receive the same mutable allocation. */
    for(int i=0;i<sub->ntemporary;i++){
        VBuffer *b=sub->temporaries[i];
        if(G.disable_temp_pool||b->allocation_size>TEMP_POOL_BYTES){buffer_free(b);free(b);continue;}
        while(G.nretired&&(G.nretired==TEMP_POOL_COUNT||G.retired_bytes>TEMP_POOL_BYTES-b->allocation_size)){
            unsigned oldest=0;for(unsigned j=1;j<G.nretired;j++)if(G.retired_age[j]<G.retired_age[oldest])oldest=j;
            VBuffer *old=retired_take(oldest);buffer_free(old);free(old);
        }
        unsigned slot=G.nretired++;G.retired_buffers[slot]=b;G.retired_age[slot]=++G.retire_clock;G.retired_bytes+=b->allocation_size;
    }
    sub->ntemporary=0;
}
static VBuffer *temporary_named(uint64_t size,VkBufferUsageFlags usage,unsigned role) {
    if(role>=TEMP_ROLES)die("temporary buffer role",role);G.profile_temp_bytes[role]+=size;
    if((size_t)S.ntemporary==S.temporary_capacity){size_t count=S.temporary_capacity?S.temporary_capacity*2:256;
        VBuffer **entries=realloc(S.temporaries,count*sizeof(*entries));if(!entries)die("temporary buffer registry allocation",count);S.temporaries=entries;S.temporary_capacity=count;}
    VBuffer *b=NULL;
    if(!G.disable_temp_pool)for(unsigned i=0;i<G.nretired;i++)if(G.retired_buffers[i]->size==size&&G.retired_buffers[i]->usage==usage){
        b=retired_take(i);G.profile_temp_reuses++;uint64_t started=prof_now();
        /* Preserve the original zero-initialized semantics, including bytes
         * outside a partial upload and the tails of driver constant banks. */
        /* Bytes at or beyond b->dirty are still zero from the previous use. */
        memset(b->map,0,b->dirty<size?b->dirty:size);G.profile_temp_clear_ns+=prof_now()-started;break;
    }
    if(!b){b=malloc(sizeof(*b));if(!b)die("temporary buffer allocation",size);*b=buffer_make(size,usage);}
    b->dirty=role==TEMP_DRIVER?DRIVER_TABLE_BYTES:size;
    S.temporaries[S.ntemporary++]=b;return b;
}
static VBuffer *temporary(uint64_t size,VkBufferUsageFlags usage) {return temporary_named(size,usage,TEMP_OTHER);}
static void trace_capture_queue(VBuffer *buffer,uint64_t bytes,const char *name) {
    const char *root=getenv("DM_FRAME_DIR");if(!root)return;
    if(S.ntrace_captures==512)die("GPU trace capture capacity",512);
    TraceCapture *c=&S.trace_captures[S.ntrace_captures++];c->buffer=buffer;c->bytes=bytes;c->offset=0;
    snprintf(c->path,sizeof(c->path),"%s/%s.bin",root,name);
}
static void trace_capture_complete(VkSubmission *sub) {
    for(unsigned i=0;i<sub->ntrace_captures;i++){TraceCapture *c=&sub->trace_captures[i];FILE *out=fopen(c->path,"wb");if(!out)die("GPU trace output",0);
        if(fwrite(c->buffer->map+c->offset,1,c->bytes,out)!=c->bytes)die("GPU trace write",c->bytes);fclose(out);
        fprintf(stderr,"[nvn-vulkan] GPU_TRACE_CAPTURE bytes=%llu path=%s\n",(unsigned long long)c->bytes,c->path);
    }
    sub->ntrace_captures=0;
}
void dm_vk_set_shader_root(const char *path) {
    if(!path || strlen(path)>=sizeof(G.shader_root) || G.nshader) die("shader root change",0);
    strcpy(G.shader_root,path);
}
void mtl_set_window_origin_mode(int mode) {
    if(mode!=NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT&&mode!=NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT)die("NVN window origin mode",mode);
    atomic_store(&G.window_origin,mode);
    fprintf(stderr,"[nvn-vulkan] WINDOW_ORIGIN mode=%d negative_viewport=%d\n",mode,mode==NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
}
static void backend_init(void) {
    void *loader=NULL;
    const char *private_driver=getenv("DM_PRIVATE_VULKAN");
    if(private_driver && !strcmp(private_driver,"turnip")) {
        Dl_info info;char directory[1024];
        if(!dladdr((void *)backend_init,&info) || !info.dli_fname || strlen(info.dli_fname)>=sizeof(directory))die("private Vulkan native library directory",0);
        strcpy(directory,info.dli_fname);char *slash=strrchr(directory,'/');if(!slash)die("private Vulkan native path",0);slash[1]=0;
        char tool_path[1100];snprintf(tool_path,sizeof tool_path,"%slibadrenotools.so",directory);
        void *tool=dlopen(tool_path,RTLD_NOW|RTLD_LOCAL);if(!tool)die("private Vulkan adrenotools load",0);
        void *(*open_driver)(int,int,const char *,const char *,const char *,const char *,const char *,void **)=dlsym(tool,"adrenotools_open_libvulkan");
        if(!open_driver)die("private Vulkan driver entry",0);
        loader=open_driver(RTLD_NOW|RTLD_LOCAL,1,NULL,directory,directory,"libd2r_turnip.so",NULL,NULL);
        if(!loader)die("private Vulkan loader failed",0);
    }else loader=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
    if(!loader)die("Vulkan loader unavailable",0);
    dm_vk_get=(PFN_vkGetInstanceProcAddr)dlsym(loader,"vkGetInstanceProcAddr");if(!dm_vk_get)die("Vulkan proc address unavailable",0);
    dm_vkCreateInstance=(PFN_vkCreateInstance)dm_vk_get(NULL,"vkCreateInstance");if(!dm_vkCreateInstance)die("Vulkan create instance unavailable",0);
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="D2R Android",.apiVersion=VK_API_VERSION_1_1};
    int validation=getenv("DM_VULKAN_VALIDATION")!=NULL;
    const char *ie[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char *layers[]={"VK_LAYER_KHRONOS_validation"};
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,.pfnUserCallback=validation_message};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,
        .enabledExtensionCount=validation?3u:2u,.ppEnabledExtensionNames=ie,.enabledLayerCount=validation?1u:0u,.ppEnabledLayerNames=validation?layers:NULL,.pNext=validation?&debug:NULL}; CHECK(vkCreateInstance(&ii,NULL,&G.instance));
    if(validation){PFN_vkCreateDebugUtilsMessengerEXT create=(PFN_vkCreateDebugUtilsMessengerEXT)dm_vk_get(G.instance,"vkCreateDebugUtilsMessengerEXT");if(!create)die("validation debug messenger unavailable",0);CHECK(create(G.instance,&debug,NULL,&validation_messenger));fprintf(stderr,"[nvn-vulkan] VALIDATION_ENABLED\n");}
    dm_vk_resolve(G.instance);
    uint32_t n=0; CHECK(vkEnumeratePhysicalDevices(G.instance,&n,NULL)); if(!n) die("Vulkan physical device",0);
    VkPhysicalDevice *physical=calloc(n,sizeof(*physical)); if(!physical) die("allocate physical list",n);
    CHECK(vkEnumeratePhysicalDevices(G.instance,&n,physical)); G.physical=physical[0]; free(physical);
    vkGetPhysicalDeviceProperties(G.physical,&G.properties); vkGetPhysicalDeviceMemoryProperties(G.physical,&G.memory);
    for(uint32_t i=0;i<G.memory.memoryTypeCount;i++)fprintf(stderr,"[nvn-vulkan] MEMORY_TYPE %u flags=%#x heap=%u size=%llu\n",i,G.memory.memoryTypes[i].propertyFlags,G.memory.memoryTypes[i].heapIndex,(unsigned long long)G.memory.memoryHeaps[G.memory.memoryTypes[i].heapIndex].size);
    G.subgroup=(VkPhysicalDeviceSubgroupProperties){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceDriverProperties driver_properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,.pNext=&G.subgroup};
    VkPhysicalDeviceProperties2 properties2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver_properties};
    vkGetPhysicalDeviceProperties2(G.physical,&properties2);
    if(private_driver && !strcmp(private_driver,"turnip") && driver_properties.driverID!=VK_DRIVER_ID_MESA_TURNIP)die("requested private driver was not loaded",driver_properties.driverID);
    fprintf(stderr,"[nvn-vulkan] driver id=%u name=%s info=%s\n",driver_properties.driverID,driver_properties.driverName,driver_properties.driverInfo);
    fprintf(stderr,"[nvn-vulkan] subgroup size=%u stages=%#x operations=%#x\n",G.subgroup.subgroupSize,G.subgroup.supportedStages,G.subgroup.supportedOperations);
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,NULL);
    VkQueueFamilyProperties *families=calloc(n,sizeof(*families)); if(!families) die("queue list",n);
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,families); G.family=UINT32_MAX;
    for(uint32_t i=0;i<n;i++) {
        if((families[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==
           (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) { G.family=i; break; }
    }
    free(families);
    if(G.family==UINT32_MAX) die("graphics/compute queue",0);
    float priority=1; VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
       .queueFamilyIndex=G.family,.queueCount=1,.pQueuePriorities=&priority};
    const char *de[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME};
    PFN_vkEnumerateDeviceExtensionProperties enumerate_extensions=(PFN_vkEnumerateDeviceExtensionProperties)dm_vk_get(G.instance,"vkEnumerateDeviceExtensionProperties");
    uint32_t extension_count=0;CHECK(enumerate_extensions(G.physical,NULL,&extension_count,NULL));
    VkExtensionProperties *extension_properties=calloc(extension_count,sizeof(*extension_properties));if(!extension_properties)die("device extension allocation",extension_count);
    CHECK(enumerate_extensions(G.physical,NULL,&extension_count,extension_properties));int has_feedback=0;
    for(uint32_t i=0;i<extension_count;i++)has_feedback|=!strcmp(extension_properties[i].extensionName,de[1]);free(extension_properties);
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,.pNext=has_feedback?&feedback:NULL};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&draw_parameters};
    PFN_vkGetPhysicalDeviceFeatures2 get_features=(PFN_vkGetPhysicalDeviceFeatures2)dm_vk_get(G.instance,"vkGetPhysicalDeviceFeatures2");get_features(G.physical,&features);
    G.feedback_layout=feedback.attachmentFeedbackLoopLayout;
    G.shader_draw_parameters=draw_parameters.shaderDrawParameters;
    draw_parameters.pNext=G.feedback_layout?&feedback:NULL;
    VkPhysicalDeviceFeatures available,enabled={0}; vkGetPhysicalDeviceFeatures(G.physical,&available);
    enabled.shaderStorageImageReadWithoutFormat=available.shaderStorageImageReadWithoutFormat;
    enabled.shaderStorageImageWriteWithoutFormat=available.shaderStorageImageWriteWithoutFormat;
    enabled.shaderImageGatherExtended=available.shaderImageGatherExtended;
    enabled.samplerAnisotropy=available.samplerAnisotropy;
    enabled.imageCubeArray=available.imageCubeArray;
    enabled.occlusionQueryPrecise=available.occlusionQueryPrecise;
    if(!available.independentBlend)die("NVN independent attachment blending unavailable",0);
    enabled.independentBlend=VK_TRUE;
    enabled.fragmentStoresAndAtomics=available.fragmentStoresAndAtomics;
    enabled.vertexPipelineStoresAndAtomics=available.vertexPipelineStoresAndAtomics;
    enabled.textureCompressionBC=available.textureCompressionBC;   // Adreno 原生支持 BC：开启后纹理压缩直传，免 CPU 解码（"初次遇到怪卡一下"的根源）
    g_bc_passthrough=enabled.textureCompressionBC;
    G.precise_occlusion=enabled.occlusionQueryPrecise;
    fprintf(stderr,"[nvn-vulkan] textureCompressionBC=%d (BC 直传=%s)\n",available.textureCompressionBC,g_bc_passthrough?"开":"关");
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,NULL);
    families=calloc(n,sizeof(*families));if(!families)die("timestamp queue properties",n);
    vkGetPhysicalDeviceQueueFamilyProperties(G.physical,&n,families);G.timestamp_bits=families[G.family].timestampValidBits;free(families);
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,
       .enabledExtensionCount=G.feedback_layout?2u:1u,.ppEnabledExtensionNames=de,.pEnabledFeatures=&enabled,.pNext=&draw_parameters}; CHECK(vkCreateDevice(G.physical,&di,NULL,&G.device));
    fprintf(stderr,"[nvn-vulkan] shader_draw_parameters=%d\n",G.shader_draw_parameters);
    fprintf(stderr,"[nvn-vulkan] attachment_feedback_layout=%d independent_blend=%u fragment_stores=%u\n",G.feedback_layout,enabled.independentBlend,enabled.fragmentStoresAndAtomics);
    vkGetDeviceQueue(G.device,G.family,0,&G.queue);
    /* The backend/device live for the process lifetime. Keep driver compilation
     * results across submissions while preserving every pipeline's full state.
     * An empty cache belongs only to this device; no cross-device data is loaded. */
    if(!getenv("DM_DISABLE_PIPELINE_CACHE")){
        VkPipelineCacheCreateInfo cache={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        /* 跨会话持久化：从 DM_FRAME_DIR/pso-cache.bin 加载上一次的驱动编译结果。
         * 后果：遇到新怪/新特效时的 300~600ms 现场编译只在第一次发生，
         * 之后每次启动直接命中。（驱动会自行校验 blob；非法则视为空缓存。） */
        void *blob=NULL; size_t blob_size=0;
        const char *dir=getenv("DM_FRAME_DIR");
        if(dir){
            snprintf(g_pso_cache_path,sizeof g_pso_cache_path,"%s/pso-cache.bin",dir);
            FILE *f=fopen(g_pso_cache_path,"rb");
            if(f){
                fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
                if(n>0 && (unsigned long)n<(256ul<<20)){
                    blob=malloc((size_t)n);
                    if(blob && fread(blob,1,(size_t)n,f)!=(size_t)n){free(blob);blob=NULL;}
                    else if(blob){blob_size=(size_t)n;cache.pInitialData=blob;cache.initialDataSize=blob_size;}
                }
                fclose(f);
            }
            if(blob) fprintf(stderr,"[nvn-vulkan] PSO_CACHE load %zu bytes from %s\n",blob_size,g_pso_cache_path);
        }
        VkResult cr=vkCreatePipelineCache(G.device,&cache,NULL,&G.pipeline_cache);
        free(blob);
        if(cr!=VK_SUCCESS){
            VkPipelineCacheCreateInfo empty={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            fprintf(stderr,"[nvn-vulkan] PSO_CACHE load rejected (%d); starting empty\n",(int)cr);
            CHECK(vkCreatePipelineCache(G.device,&empty,NULL,&G.pipeline_cache));
        }
    }
    fprintf(stderr,"[nvn-vulkan] driver_pipeline_cache=%d\n",G.pipeline_cache!=VK_NULL_HANDLE);
    G.disable_pso_reuse=getenv("DM_DISABLE_PSO_REUSE")!=NULL;
    {const char *last=getenv("DM_PSO_LAST_CACHE");G.enable_pso_last=last&&atoi(last)!=0;G.last_pso_slot=UINT32_MAX;}
    G.disable_driver_table_reuse=getenv("DM_DISABLE_DRIVER_TABLE_REUSE")!=NULL;
    G.disable_temp_pool=getenv("DM_DISABLE_TEMP_POOL")!=NULL;
    G.disable_query_batch=getenv("DM_DISABLE_QUERY_BATCH")!=NULL;G.diag_no_occlusion=getenv("DM_DIAG_NO_OCCLUSION")!=NULL;G.barrier_pass_elide=getenv("DM_BARRIER_PASS_ELIDE")!=NULL;G.diag_no_pass_deps=getenv("DM_DIAG_NO_PASS_DEPS")!=NULL;G.precise_sync=getenv("DM_DISABLE_PRECISE_SYNC")==NULL;{const char *hd=getenv("DM_HZ_DIAG");G.hz_diag=hd?atoi(hd):0;}{const char *cd=getenv("DM_CHUNK_DRAWS");G.chunk_draws=cd?atoi(cd):48;G.chunk_log=getenv("DM_RESOURCE_STATS")!=NULL;G.profile_detail=getenv("DM_PROFILE_DETAIL")!=NULL;G.diag_skip_draws=getenv("DM_DIAG_SKIP_DRAWS")!=NULL;{const char *sh=getenv("DM_DIAG_SHAPE");G.diag_shape=sh?atoi(sh):0;}G.disable_setrt_skip=getenv("DM_DISABLE_SETRT_SKIP")!=NULL;G.disable_subres=getenv("DM_DISABLE_SUBRES")!=NULL;G.enable_narrow_access=getenv("DM_NARROW_ACCESS")!=NULL;G.diag_fixed_desc=getenv("DM_DIAG_FIXED_DESC")!=NULL;G.disable_wait_elision=getenv("DM_DISABLE_WAIT_ELISION")!=NULL;G.async_enabled=getenv("DM_DISABLE_ASYNC_COMMIT")==NULL;G.async_drain_after_submit=getenv("DM_ASYNC_DRAIN")!=NULL;{const char *sc=getenv("DM_DIAG_SKIP_CLASS");G.diag_skip_class=sc?atoi(sc):0;}G.wb_stage_enabled=getenv("DM_DISABLE_WB_STAGE")==NULL;G.disable_pass_reuse=getenv("DM_DISABLE_PASS_REUSE")!=NULL;G.disable_cpu_uniform_snapshot=getenv("DM_DISABLE_CPU_UNIFORM_SNAPSHOT")!=NULL;{const char *sx=getenv("DM_DIAG_SKIP_XFER");G.diag_skip_xfer=sx?atoi(sx):0;}}fprintf(stderr,"[nvn-vulkan] PRECISE_SYNC=%d hz_diag=%d\n",G.precise_sync,G.hz_diag);{const char *iu=getenv("DM_DIAG_IMG_USAGE");G.diag_img_usage=iu?atoi(iu):0;}
    {const char *range=getenv("DM_QUERY_RESET_RANGE");G.enable_query_reset_range=range&&atoi(range)!=0;}
    G.query_pre_reset=getenv("DM_QUERY_PRERESET")!=NULL;   /* 默认关：实测 CPU 省 2.2ms 但 GPU 亏 2-4ms（更少更长的通道对 GPU 不利）| default off: measured CPU win but larger GPU loss (fewer/longer passes hurt this GPU) */
    {const char *ps=getenv("DM_PASS_SPLIT");G.pass_split_draws=ps?atoi(ps):0;}
    G.fb_cache_enabled=getenv("DM_DISABLE_FB_CACHE")==NULL;
    G.read_memo_enabled=getenv("DM_DISABLE_READ_MEMO")==NULL;
    G.disable_desc_cache=getenv("DM_DISABLE_DESC_CACHE")!=NULL;G.disable_control_cache=getenv("DM_DISABLE_CONTROL_CACHE")!=NULL;G.disable_barrier_elision=getenv("DM_DISABLE_BARRIER_ELISION")!=NULL;G.disable_sample_copy_cache=getenv("DM_DISABLE_SAMPLE_COPY_CACHE")!=NULL;G.pass_timing=getenv("DM_PASS_TIMING")!=NULL;{const char *sk=getenv("DM_SKIP_PS");if(sk){strncpy(G.skip_ps,sk,16);G.skip_ps[16]=0;fprintf(stderr,"[nvn-vulkan] DIAG_SKIP_PS key=%s (timing diagnostic only)\n",G.skip_ps);}}
    G.disable_resource_cache=getenv("DM_DISABLE_RESOURCE_CACHE")!=NULL;
    G.disable_binding_plans=getenv("DM_DISABLE_BINDING_PLANS")!=NULL;G.plan_precise_state=getenv("DM_PLAN_PRECISE_STATE")!=NULL;
    {const char *cache=getenv("DM_COMMAND_STATE_CACHE");G.enable_command_cache=cache&&atoi(cache)!=0;}
    G.disable_dirty_uploads=getenv("DM_DISABLE_DIRTY_UPLOADS")!=NULL;
    {const char *prepare=getenv("DM_WRITEBACK_PREPARE");G.enable_writeback_prepare=prepare&&atoi(prepare)!=0;}
    {const char *coalesce=getenv("DM_WRITEBACK_COALESCE");G.enable_writeback_coalesce=coalesce&&atoi(coalesce)!=0;}
    G.event_trace=getenv("DM_EVENT_TRACE")!=NULL;
    G.current_submission=&G.submission_slot;
    S.desc_entries=calloc(DESC_ENTRIES,sizeof(*S.desc_entries));S.desc_arena=malloc(DESC_ARENA);G.control_cache=calloc(CONTROL_CACHE,sizeof(*G.control_cache));
    if(!S.desc_entries||!S.desc_arena||!G.control_cache)die("descriptor/control cache allocation",0);
    {const char *d=getenv("DM_DIAG_BARRIER");G.diag_barrier=d?atoi(d):0;G.barrier_stats=getenv("DM_BARRIER_STATS")!=NULL;
     const char *ls=getenv("DM_DIAG_LOADSTORE");G.diag_loadstore=ls?atoi(ls):0;
     if(G.diag_loadstore)fprintf(stderr,"[nvn-vulkan] DIAG_LOADSTORE mode=%d (timing diagnostic only; render output is intentionally incorrect)\n",G.diag_loadstore);
     fprintf(stderr,"[nvn-vulkan] DIAG_SWITCHES profile_detail=%d resource_stats=%d disable_resource_cache=%d diag_loadstore=%d query_batch=%d command_cache=%d\n",G.profile_detail,getenv("DM_RESOURCE_STATS")!=NULL,G.disable_resource_cache,G.diag_loadstore,!G.disable_query_batch,G.enable_command_cache);
     if(G.diag_barrier)fprintf(stderr,"[nvn-vulkan] DIAG_BARRIER mode=%d (timing diagnostic only; GPU synchronization is intentionally incorrect)\n",G.diag_barrier);}
    VkCommandPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=G.family,
       .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT}; CHECK(vkCreateCommandPool(G.device,&ci,NULL,&S.command_pool));CHECK(vkCreateCommandPool(G.device,&ci,NULL,&G.submission_slot2.command_pool));
    G.support=buffer_make(1392,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); G.zero=buffer_make(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    for(int i=0;i<73;i++) { float one=1; memcpy(G.support.map+192+16*i,&one,4); }
    G.state.mask.mask=UINT32_MAX; G.state.polygon.mode=NVN_POLYGON_MODE_FILL;
    G.state.color.logic_op=NVN_LOGIC_OP_COPY;G.state.color.alpha_func=NVN_ALPHA_FUNC_ALWAYS;
    G.state.ds.df=NVN_DEPTH_FUNC_LESS; G.state.viewport=(VkViewport){0,0,1,1,0,1};
    G.state.ds.sf_f=G.state.ds.sf_b=NVN_STENCIL_FUNC_ALWAYS;
    G.state.ds.fail_f=G.state.ds.fail_b=G.state.ds.zfail_f=G.state.ds.zfail_b=G.state.ds.zpass_f=G.state.ds.zpass_b=NVN_STENCIL_OP_KEEP;
    G.state.stencil_compare[0]=G.state.stencil_compare[1]=G.state.stencil_write[0]=G.state.stencil_write[1]=UINT32_MAX;
    fprintf(stderr,"[nvn-vulkan] initialized %s\n",G.properties.deviceName);
    G.profile_start_ns=dm_android_nvn_clock_ns();
}
void mtl_init(void) { if(pthread_once(&init_once,backend_init))die("Vulkan initialize once",0); }
void *dm_vk_native_window(void) {pthread_mutex_lock(&G.lock);void *w=G.window;pthread_mutex_unlock(&G.lock);return w;}
int dm_vk_graphics_live(void) {pthread_mutex_lock(&G.lock);int live=G.graphics_live;pthread_mutex_unlock(&G.lock);return live;}
void dm_vk_graphics_begin(void *memory,size_t size) {
    if(!memory || !size || size>UINTPTR_MAX-(uintptr_t)memory)die("nv::InitializeGraphics transfer-memory bounds",size);
    mtl_init();pthread_mutex_lock(&G.lock);
    if(G.graphics_live)die("nv::InitializeGraphics duplicate session",size);
    G.transfer_memory=memory;G.transfer_size=size;G.graphics_live=1;
    fprintf(stderr,"[nvn-vulkan] graphics session registered transfer-memory=%p bytes=%zu; Vulkan device initialized\n",memory,size);
    pthread_mutex_unlock(&G.lock);
}
static void async_drain_locked(void);
void dm_vk_graphics_end(void) {
    mtl_init();pthread_mutex_lock(&G.lock);async_drain_locked();
    if(!G.graphics_live || G.active || G.npool)die("nv::FinalizeGraphics live session/pools/submission",G.npool);
    CHECK(vkDeviceWaitIdle(G.device));G.graphics_live=0;G.transfer_memory=NULL;G.transfer_size=0;
    fprintf(stderr,"[nvn-vulkan] graphics session ended after Vulkan device idle; host device retained\n");pthread_mutex_unlock(&G.lock);
}
void mtl_pool_add(const void *mem,uint64_t size) {
    mtl_init(); pthread_mutex_lock(&G.lock);
    if(!mem || G.npool==LIMIT_POOLS || size>UINTPTR_MAX-(uintptr_t)mem) die("memory pool bounds",size);
    for(int i=0;i<G.npool;i++) if(G.pools[i].guest==mem) die("duplicate memory pool",(long long)(uintptr_t)mem);
    VPool *pool=&G.pools[G.npool++]; pool->guest=mem;
    *pool=(VPool){.guest=mem,.size=size,.serial=++G.pool_adds};G.pool_revision++; pthread_mutex_unlock(&G.lock);
}
static void pool_materialize(VPool *pool) {
    if(pool->v.b)return;
    /* pool_read supplies every CPU-owned range before a GPU read, and queued
     * writes supply their own bytes. Touching the entire backing with memset
     * would commit hundreds of MiB that the game has not used. */
    pool->v=buffer_create(pool->size,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,0);
    uint64_t logical=0,resident=0;for(int i=0;i<G.npool;i++){logical+=G.pools[i].size;resident+=G.pools[i].v.size;}
    fprintf(stderr,"[nvn-vulkan] POOL_MATERIALIZE bytes=%llu resident=%llu logical=%llu pools=%d\n",(unsigned long long)pool->size,(unsigned long long)resident,(unsigned long long)logical,G.npool);
}
static VPool *pool_find(uint64_t addr,uint64_t size,uint64_t *offset) {
    if(size>UINT64_MAX-addr) return NULL;
    /* 最近命中池优先：热路径上同一批小池（流式/常量池）被反复查找，先试上次命中的索引。
     * 池数组会被删除操作压实 → 索引可能指向别的池，但范围检查保证返回的仍是"包含该地址的池"
     * （池区间互不重叠），找不到再走全表扫描。 | Last-hit pool first: the hot path hammers a few
     * streaming/constant pools; the cached index may be stale after compaction, but the range check
     * still guarantees the returned pool contains the address (pool ranges never overlap). */
    static _Thread_local int last=-1;
    if(last>=0&&last<G.npool){VPool *p=&G.pools[last];uint64_t base=(uint64_t)(uintptr_t)p->guest;
        if(addr>=base && addr-base<=p->size && size<=p->size-(addr-base)) {
            if(offset) *offset=addr-base; return p; } }
    for(int i=0;i<G.npool;i++) { uint64_t base=(uint64_t)(uintptr_t)G.pools[i].guest;
        if(addr>=base && addr-base<=G.pools[i].size && size<=G.pools[i].size-(addr-base)) {
            if(offset) *offset=addr-base; last=i; return &G.pools[i]; } }
    return NULL;
}
int mtl_addr(uint64_t addr,uint64_t len,void **buf,uint64_t *off) {
    VPool *p=pool_find(addr,len,off); if(!p) return 0; *buf=p; return 1;
}
int mtl_gpu_addr(uint64_t addr,uint64_t len,void **buf,uint64_t *off,uint64_t *gpu) {
    if(!mtl_addr(addr,len,buf,off)) return 0; *gpu=addr; return 1; /* NVN virtual address, resolved through descriptors. */
}
/* 翻译线程可能在执行提交（G.active）：宿主线程要释放/改注册资源时等到这次提交发布完。
 * 等待期间放开 G.lock —— 翻译线程与异步发布者需要它推进（等待窗口只有发布间隙，极短）。
 * 调用时必须已持有 G.lock；返回时仍持有。 | The translation thread may be running a submission
 * (G.active): host-side resource release/re-registration waits until it is published. The lock is
 * released while waiting because the translation thread and the async publisher need it to make
 * progress (only the publish gap is ever waited on). Must be called with G.lock held; returns
 * with it held. */
static void wait_submission_idle_locked(void) {
    while (G.active) {
        pthread_mutex_unlock(&G.lock);
        struct timespec pause = {0, 200000};
        nanosleep(&pause, NULL);
        pthread_mutex_lock(&G.lock);
    }
    async_drain_locked();
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&G.lock);wait_submission_idle_locked(); for(int i=0;i<G.npool;i++) if(G.pools[i].guest==mem) {
        buffer_free(&G.pools[i].v); G.pools[i]=G.pools[--G.npool];G.pool_revision++; pthread_mutex_unlock(&G.lock); return; }
    pthread_mutex_unlock(&G.lock); die("unknown memory pool delete",(long long)(uintptr_t)mem);
}
static int overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn) { return a<b+bn && b<a+an; }
static int fully_gpu_owned(Range range) {
    uint64_t cursor=range.addr,end=range.addr+range.size;
    while(cursor<end){uint64_t next=cursor;
        for(int i=0;i<S.nwrites;i++){Range w=S.writes[i];if(w.addr<=cursor&&w.addr+w.size>next)next=w.addr+w.size;}
        if(next==cursor)return 0;cursor=next<end?next:end;
    }
    return 1;
}
static int cached_snapshot(Range range,VBuffer **buffer) {
    *buffer=NULL;if(!S.read_snapshots_allowed)return -1;
    for(int i=0;i<S.nread_snapshots;i++){
        ReadSnapshot *s=&S.read_snapshots[i];if(s->range.addr!=range.addr||s->range.size!=range.size)continue;
        int changed=0;for(int j=s->write_count;j<S.nwrites;j++)if(overlap(range.addr,range.size,S.writes[j].addr,S.writes[j].size)){changed=1;break;}
        if(!changed){s->write_count=S.nwrites;*buffer=s->buffer;G.profile_snapshot_reuses++;}
        return i;
    }
    /* pool_read skips these entirely GPU-owned bytes, so host uploads cannot
     * change them. All later GPU/queued writes are tracked by mark_write. */
    if(S.nread_snapshots==LIMIT_WRITES || !fully_gpu_owned(range))return -1;
    int i=S.nread_snapshots++;S.read_snapshots[i]=(ReadSnapshot){.range=range};return i;
}
static void pool_upload_guest_span(VPool *p,uint64_t base,uint64_t base_off,uint64_t addr,uint64_t len) {
    G.profile_pool_uploaded_bytes+=nvn_vk_pool_copy_guest_span(p->v.map,base,base_off,addr,len,S.writes,S.nwrites);
}
static VPool *pool_read_resolved(VPool *p,uint64_t addr,uint64_t len,uint64_t *off) {
    /* 热路径备忘录：同一 draw 内（乃至相邻 draw）反复读同一区间——统一缓冲/顶点/索引。
     * 命中且此后无新写入（脏检查仍是权威）时直接复用，省掉哈希桶查找。
     * 池序号参与匹配：池被释放后地址复用不会误命中。 */
    static _Thread_local struct { VPool *p; uint64_t serial,addr,len,off,stamp; } memo;
    if(memo.p==p && memo.serial==p->serial && memo.addr==addr && memo.len==len &&
       (!len || !nvn_dirty_since((const void *)(uintptr_t)addr,len,memo.stamp))){
        *off=memo.off;G.profile_pool_reused_ranges++;G.profile_pool_reused_bytes+=len;
        return p;
    }
    uint64_t started=prof_now();
    pool_materialize(p);
    /* Cache exact read ranges. On a dirty cache hit, copy only changed page
     * intersections with this exact range; bytes outside it may belong to a
     * different range referenced by already-recorded draws. Page generations
     * conservatively invalidate overlapping CPU changes, while pool serials
     * prevent address reuse from inheriting old uploads. */
    PoolUpload *upload=NULL;uint64_t stamp=0,upload_stamp=0;int partial_upload=0;
    if(len&&!G.disable_dirty_uploads&&nvn_dirty_tracked((const void *)(uintptr_t)addr,len)){
        uint64_t hash=addr^(addr>>19)^(len*0x9e3779b97f4a7c15ULL)^p->serial;
        PoolUpload *bucket=G.pool_uploads[hash&(UPLOAD_BUCKETS-1)];upload=&bucket[0];
        for(unsigned i=0;i<UPLOAD_WAYS;i++){
            PoolUpload *entry=&bucket[i];
            if(entry->pool_serial==p->serial&&entry->address==addr&&entry->size==len){
                upload=entry;upload_stamp=entry->stamp;
                if(!nvn_dirty_since((const void *)(uintptr_t)addr,len,entry->stamp)){
                    entry->age=++G.upload_age;G.profile_pool_reused_ranges++;G.profile_pool_reused_bytes+=len;
                    memo.p=p;memo.serial=p->serial;memo.addr=addr;memo.len=len;memo.off=*off;memo.stamp=entry->stamp;
                    G.profile_pool_ns+=prof_now()-started;return p;
                }
                partial_upload=1;
                break;
            }
            if(entry->age<upload->age)upload=entry;
        }
        stamp=nvn_dirty_now();nvn_dirty_protect((const void *)(uintptr_t)addr,len);
    }
    /* A GPU-written range in this command stream must retain its queued value. */
    if(partial_upload) {
        size_t cursor=0,range_offset=0,range_len=0;
        while(nvn_dirty_next_range((const void *)(uintptr_t)addr,(size_t)len,upload_stamp,&cursor,&range_offset,&range_len)) {
            if(!range_len || range_offset>len || range_len>len-range_offset)die("invalid dirty upload span",addr);
            pool_upload_guest_span(p,addr,*off,addr+range_offset,range_len);
        }
    } else {
        pool_upload_guest_span(p,addr,*off,addr,len);
    }
    if(upload)*upload=(PoolUpload){addr,len,p->serial,stamp,++G.upload_age};
    memo.p=p;memo.serial=p->serial;memo.addr=addr;memo.len=len;memo.off=*off;memo.stamp=stamp;
    G.profile_pool_ns+=prof_now()-started;return p;
}
static VPool *pool_read(uint64_t addr,uint64_t len,uint64_t *off) {
    VPool *p=pool_find(addr,len,off);if(!p)die("unmapped NVN buffer range",addr);
    return pool_read_resolved(p,addr,len,off);
}
/* draw 热路径专用读出备忘（索引/顶点流）：pool_read 的内置备忘是单条且被缓冲绑定穿插冲刷，
 * 这里开一层 8 槽直接映射表，跳过 pool_find 扫描与上传桶查找；**保留脏检查**（内容新鲜语义不变：
 * 命中条件含 dirty_since，任何 CPU 写入都会落回 pool_read 走上传）。 */
typedef struct { uint64_t addr,size,serial,stamp; VPool *p; uint64_t off; } ReadMemo;
static _Thread_local ReadMemo g_read_memo[8];
static VPool *pool_read_memo(uint64_t addr,uint64_t size,uint64_t *offset) {
    if(!G.read_memo_enabled)return pool_read(addr,size,offset);
    ReadMemo *m=&g_read_memo[(addr>>6)&7];
    uint64_t stamp=nvn_dirty_now();
    if(m->addr==addr&&m->size==size&&m->p&&m->p->serial==m->serial&&
       !nvn_dirty_since((const void *)(uintptr_t)addr,size,m->stamp)){
        *offset=m->off;G.profile_read_memo_hits++;return m->p; }
    uint64_t off=0;VPool *p=pool_read(addr,size,&off);
    m->addr=addr;m->size=size;m->serial=p->serial;m->stamp=stamp;m->p=p;m->off=off;
    G.profile_read_memo_misses++;*offset=off;return p;
}
static void mark_write(uint64_t addr,uint64_t len) {
    if(!pool_find(addr,len,NULL)) die("unmapped write range",addr);
    if(S.nwrites==LIMIT_WRITES) die("write range limit",S.nwrites);
    S.write_cpu[S.nwrites]=NULL;S.writes[S.nwrites++]=(Range){addr,len};
}
static void counter_op(uint32_t kind,uint32_t query,uint64_t address);
static void occlusion_end(void) {
    if(S.active_occlusion!=UINT32_MAX){
        vkCmdEndQuery(S.command,S.occlusion,S.active_occlusion);
        counter_op(1,S.active_occlusion,0);S.active_occlusion=UINT32_MAX;
    }
}
static void chunk_ts_write(int end);
static void pass_end_at(int line) { if(S.in_pass) { G.pass_end_sites[line&8191]++;G.pass_end_draws[line&8191]+=G.draws-G.pass_draws0;if(G.profile_detail&&G.profile_frames==30)fprintf(stderr,"[nvn-vulkan] PASSLOG seg=%u rt=%s ps=%.6s n=%d bb=%#x uses=%s draws=%llu end_line=%d\n",G.seg_index,G.pass_desc,G.last_ps,G.last_count,G.last_barrier_bits,G.last_uses,(unsigned long long)(G.draws-G.pass_draws0),line);occlusion_end();vkCmdEndRenderPass(S.command); S.in_pass=0;S.pass_attach_n=0;
    if(S.pt_open){S.pt_open=0;vkCmdWriteTimestamp(S.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,S.pass_ts,S.pt[S.npt].q0+1);S.pt[S.npt].q1=S.pt[S.npt].q0+1;S.pt[S.npt].draws=G.draws-S.pt_draws0;S.npt++;} } }
#define pass_end() pass_end_at(__LINE__)
static void query_reset(VkQueryPool pool,uint32_t first,uint32_t count) {
    if(S.in_pass || S.active_occlusion!=UINT32_MAX)die("query reset inside render pass/query",first);
    vkCmdResetQueryPool(S.command,pool,first,count);
    G.profile_query_reset_calls++;G.profile_query_reset_slots+=count;
}
static void query_reserve(VkQueryPool pool,uint32_t *reset_count,uint32_t first,uint32_t count,uint32_t capacity) {
    if(!G.enable_query_reset_range)return;
    if(first>=capacity || !count || count>capacity-first)die("query reserve bounds",first);
    uint32_t required=first+count;
    if(required<=*reset_count)return;
    /* Each submission allocates query indices monotonically. Reset a fresh
     * prefix in 32-slot blocks before its first use, outside any render pass.
     * Never reset a previously used query: retirement still needs its result.
     * Command-buffer reuse starts with an empty prefix after the real fence. */
    {int was=S.in_pass;if(was)G.profile_query_breaks++;}
    pass_end();
    uint32_t end=(required+31u)&~31u;if(end>capacity)end=capacity;
    query_reset(pool,*reset_count,end-*reset_count);*reset_count=end;
}
static void pass_timing_collect(VkSubmission *sub) {
    if(!sub->pass_ts)return;
    if(sub->npt){uint64_t ticks[1024];CHECK(vkGetQueryPoolResults(G.device,sub->pass_ts,0,sub->npt*2,sizeof(ticks[0])*sub->npt*2,ticks,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
        uint64_t mask=G.timestamp_bits>=64?UINT64_MAX:((1ULL<<G.timestamp_bits)-1);
        double total_ms=0,max_ms=0;
        for(uint32_t i=0;i<sub->npt;i++){uint64_t a=ticks[sub->pt[i].q0],b=ticks[sub->pt[i].q1];
            double ms=(double)((b-a)&mask)*G.properties.limits.timestampPeriod/1e6;uint32_t k=0;
            total_ms+=ms;if(ms>max_ms)max_ms=ms;
            while(k<G.npacc&&!(G.pacc[k].key==sub->pt[i].key&&G.pacc[k].w==sub->pt[i].w&&G.pacc[k].h==sub->pt[i].h&&G.pacc[k].n==sub->pt[i].n))k++;
            if(k==G.npacc){if(k==128)continue;G.pacc[k]=(PassAcc){.key=sub->pt[i].key,.w=sub->pt[i].w,.h=sub->pt[i].h,.n=sub->pt[i].n};G.npacc++;}
            G.pacc[k].ms+=ms;G.pacc[k].passes++;G.pacc[k].draws+=sub->pt[i].draws;}
        if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] PASS_QUERY_STATS passes=%u chunks=%u total_ms=%.6f max_ms=%.6f\n",sub->npt,sub->chunk_index+1,total_ms,max_ms);
    }
    sub->npt=0;sub->pt_open=0; /* Query storage belongs to the completed context; keep it for reuse. */
}
enum { HZ_GV,HZ_GF,HZ_T,HZ_CS };
#define HZ_ATT_STAGES (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
#define HZ_ATT_ACCESS (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
#define HZ_SHADER_STAGES (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
#define HZ_COPY_SRC (G.precise_sync?((G.hz_diag&4)?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:(HZ_SHADER_STAGES|VK_PIPELINE_STAGE_TRANSFER_BIT)):VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
#define HZ_COPY_DST (G.precise_sync?((G.hz_diag&4)?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|(G.xition_vertex?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:0))):VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
static void hz_add(unsigned classes,VkPipelineStageFlags stages,VkAccessFlags access) {
    for(unsigned c=0;c<4;c++)if(classes&(1u<<c)){S.pend[c].stages|=stages;S.pend[c].access|=access;}
}
static void hz_note(int kind) {
    if(!G.precise_sync)return;
    switch(kind) {
    case HZ_DRAW:
        hz_add(1u<<HZ_GF|1u<<HZ_T|1u<<HZ_CS,HZ_ATT_STAGES,HZ_ATT_ACCESS);
        hz_add(1u<<HZ_T|1u<<HZ_CS,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|HZ_SHADER_STAGES,0);
        if(G.draw_store)hz_add(15u,HZ_SHADER_STAGES,VK_ACCESS_SHADER_WRITE_BIT);
        break;
    case HZ_ATTACH:hz_add(1u<<HZ_GF|1u<<HZ_T|1u<<HZ_CS,HZ_ATT_STAGES,HZ_ATT_ACCESS);break;
    case HZ_TRANSFER:hz_add(15u,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);break;
    case HZ_COMPUTE:hz_add(15u,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);break;
    }
}
static void hz_sync(unsigned cls,VkPipelineStageFlags dst_stages,VkAccessFlags dst_access) {
    if(!S.pend[cls].stages)return;
    if(G.hz_diag&2){S.pend[cls].stages=S.pend[cls].access=0;return;}
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=S.pend[cls].access,.dstAccessMask=dst_access};
    vkCmdPipelineBarrier(S.command,S.pend[cls].stages,dst_stages,0,1,&b,0,NULL,0,NULL);
    S.pend[cls].stages=S.pend[cls].access=0;G.profile_hz_barriers++;G.hz_count[2+(cls==HZ_CS)]++;
}
/* Before a render pass: attachment-only pending writes only gate fragment-side work, so the next pass's
 * vertex/tiler stages can overlap the previous pass's fragment work. */
static void hz_pass_sync(void) {
    VkPipelineStageFlags src=S.pend[HZ_GV].stages|S.pend[HZ_GF].stages;
    if(!src)return;
    if(G.hz_diag&8){S.pend[HZ_GV].stages=S.pend[HZ_GV].access=S.pend[HZ_GF].stages=S.pend[HZ_GF].access=0;return;}
    VkAccessFlags access=S.pend[HZ_GV].access|S.pend[HZ_GF].access;
    int wide=S.pend[HZ_GV].stages!=0;
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=access,
        .dstAccessMask=wide?(VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT):
            (VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)};
    vkCmdPipelineBarrier(S.command,src,wide?VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT:(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|HZ_ATT_STAGES),0,1,&b,0,NULL,0,NULL);
    S.pend[HZ_GV].stages=S.pend[HZ_GV].access=S.pend[HZ_GF].stages=S.pend[HZ_GF].access=0;G.profile_hz_barriers++;G.hz_count[wide?1:0]++;
}
static void all_barrier_at(int line,int soft) {
    G.profile_barriers++;G.barrier_sites[line&4095]++;
    if(soft&&G.diag_barrier==2)return;
    if(G.precise_sync){
        pass_end_at(line);
        if(!soft)hz_sync(HZ_T,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        return;
    }
    pass_end(); VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
       .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    if(G.diag_barrier==1)return;
    if(!S.command_dirty&&!G.disable_barrier_elision){G.profile_barriers_skipped++;return;}
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,NULL,0,NULL);
    S.command_dirty=0;
}
#define all_barrier() all_barrier_at(__LINE__,0)
#define all_barrier_soft() all_barrier_at(__LINE__,1)
static uint64_t pso_key_hash(const PsoKey *key) {
    uint64_t started=prof_now();
    /* Four independent multiply-rotate lanes over 8-byte words; every key byte is hashed. */
    const uint8_t *bytes=(const uint8_t *)key;uint64_t h0=0x9e3779b97f4a7c15ULL,h1=0xc2b2ae3d27d4eb4fULL,h2=0x165667b19e3779f9ULL,h3=0x27d4eb2f165667c5ULL;size_t i=0;
    for(;i+32<=sizeof(*key);i+=32){uint64_t w[4];memcpy(w,bytes+i,32);
        h0=(h0^w[0])*0xff51afd7ed558ccdULL;h0^=h0>>32;h1=(h1^w[1])*0xc4ceb9fe1a85ec53ULL;h1^=h1>>32;
        h2=(h2^w[2])*0xff51afd7ed558ccdULL;h2^=h2>>32;h3=(h3^w[3])*0xc4ceb9fe1a85ec53ULL;h3^=h3>>32;}
    for(;i+8<=sizeof(*key);i+=8){uint64_t w;memcpy(&w,bytes+i,8);h0=(h0^w)*0xff51afd7ed558ccdULL;h0^=h0>>32;}
    for(;i<sizeof(*key);i++){h1=(h1^bytes[i])*1099511628211ULL;}
    uint64_t h=h0^(h1*31)^(h2*0x9e3779b97f4a7c15ULL)^(h3<<7);h^=h>>29;h*=0xbf58476d1ce4e5b9ULL;h^=h>>32;
    G.profile_pso_hash_calls++;G.profile_pso_hash_ns+=prof_now()-started;return h;
}
static void pso_probe_stats(uint64_t probes) {
    G.profile_pso_probes+=probes;if(probes>G.profile_pso_max_probe)G.profile_pso_max_probe=probes;
}
static Pso *pso_cached(const PsoKey *key,uint64_t hash) {
    uint64_t probes=0;
    if(!G.disable_pso_reuse){
        uint32_t slot=(uint32_t)hash&(PSO_TABLE_SIZE-1);
        while(G.pso_table[slot]){
            uint32_t index=G.pso_table[slot]-1;CachedPso *p=&G.psos[index];probes++;
            if(p->hash==hash&&!memcmp(&p->key,key,sizeof(*key))){
                G.profile_pso_hits++;G.profile_pso_table_hits++;G.last_pso_slot=index;pso_probe_stats(probes);return &p->pso;}
            slot=(slot+1)&(PSO_TABLE_SIZE-1);
        }
    }
    G.last_pso_slot=UINT32_MAX;G.profile_pso_misses++;pso_probe_stats(probes);return NULL;
}
static Pso *pso_lookup(const PsoKey *key,uint64_t *hash) {
    uint64_t started=prof_now();G.profile_pso_lookups++;*hash=0;Pso *cached=NULL;
    /* Only permanent append-only cache slots survive submission retirement.
     * Compare every byte of the existing key; no pipeline state is omitted. */
    if(G.enable_pso_last&&!G.disable_pso_reuse){
        if(G.last_pso_slot<G.npso&&!memcmp(&G.psos[G.last_pso_slot].key,key,sizeof(*key))){
            cached=&G.psos[G.last_pso_slot].pso;G.profile_pso_hits++;G.profile_pso_last_hits++;
        }else G.profile_pso_last_misses++;
    }
    if(!cached){*hash=pso_key_hash(key);cached=pso_cached(key,*hash);}
    G.profile_pso_lookup_ns+=prof_now()-started;return cached;
}
static Pso pso_remember(const PsoKey *key,uint64_t hash,Pso pso) {
    /* Cached objects live with the device, like their shader modules/layouts.
     * Once full, keep the original fence-owned temporary lifetime. Never evict
     * an object referenced by commands in the current submission. */
    if(!G.disable_pso_reuse&&G.npso<LIMIT_PSOS){
        uint32_t index=G.npso++,slot=(uint32_t)hash&(PSO_TABLE_SIZE-1);
        G.psos[index]=(CachedPso){.key=*key,.pso=pso,.hash=hash};
        while(G.pso_table[slot])slot=(slot+1)&(PSO_TABLE_SIZE-1);
        G.pso_table[slot]=index+1;G.last_pso_slot=index;
    }else{
        G.last_pso_slot=UINT32_MAX;
        if(S.npipeline==LIMIT_WRITES)die("temporary pipeline limit",S.npipeline);
        S.pipelines[S.npipeline]=pso.pipeline;S.layouts[S.npipeline++]=pso.layout;
    }
    return pso;
}
static void submission_pools_create(VkSubmission *sub) {
    VkCommandBufferAllocateInfo a={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=sub->command_pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};CHECK(vkAllocateCommandBuffers(G.device,&a,&sub->command));sub->command_primary=sub->command;sub->chunk_cb[0]=sub->command;sub->chunk_alloc=1;
    VkQueryPoolCreateInfo q={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_OCCLUSION,.queryCount=LIMIT_WRITES};
    CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->occlusion));q.queryType=VK_QUERY_TYPE_TIMESTAMP;
    CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->timestamps));
    if(G.pass_timing){q.queryCount=1024;CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->pass_ts));}
    if(G.profile_detail){q.queryCount=128;CHECK(vkCreateQueryPool(G.device,&q,NULL,&sub->chunk_ts));}
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16384},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,16384},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8192},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,16384},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,4096},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,16384},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,4096},{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,4096}};
    VkDescriptorPoolCreateInfo d={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=8192,
        .poolSizeCount=7,.pPoolSizes=sizes};CHECK(vkCreateDescriptorPool(G.device,&d,NULL,&sub->descriptors));
    VkFenceCreateInfo f={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(vkCreateFence(G.device,&f,NULL,&sub->gpu_fence));
    G.profile_submission_creates++;
}
static void submission_gpu_done(VkSubmission *sub) {
    if(sub->phase!=SUBMISSION_INFLIGHT || !pthread_equal(sub->recorder,pthread_self()))die("submission completion ownership",sub->epoch);
    /* Wait success, not a software ticket, establishes safe query/readback access. */
    CHECK(vkGetFenceStatus(G.device,sub->gpu_fence));sub->phase=SUBMISSION_GPU_DONE;
}
static void submission_publish(VkSubmission *sub) {
    if(sub->phase!=SUBMISSION_GPU_DONE || !pthread_equal(sub->recorder,pthread_self()))die("submission publication ownership",sub->epoch);
    /* Called after original writeback, counter and event operations, in that order. */
    atomic_store(&G.completed,sub->ticket);sub->phase=SUBMISSION_PUBLISHED;G.profile_submission_publications++;
}
static void submission_released(VkSubmission *sub) {
    if(sub->phase!=SUBMISSION_PUBLISHED || sub->ntemporary || sub->ntrace_captures)die("submission retirement before publication",sub->epoch);
    sub->phase=SUBMISSION_RETIRED;G.active=0;
}
/* Segment-end copy of GPU-written pool ranges into a HOST_CACHED staging buffer. Reading the write-combined
 * pool mirror with the CPU is very slow; the cached copy is read after the fence with one invalidate. */
static int wb_stage_has_cached_type(void) {
    for(uint32_t i=0;i<G.memory.memoryTypeCount;i++){VkMemoryPropertyFlags f=G.memory.memoryTypes[i].propertyFlags;
        if((f&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)&&(f&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)&&!(f&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))return 1;}
    return 0;
}
static void wb_stage_record(void) {
    S.wb_staged=0;
    if(!G.wb_stage_enabled||G.enable_writeback_coalesce||!S.nwrites||!wb_stage_has_cached_type())return;
    uint64_t total=0;for(int i=0;i<S.nwrites;i++)total+=S.writes[i].size;
    if(!total)return;
    if(S.wb_stage.size<total){
        if(S.wb_stage.b)buffer_free(&S.wb_stage);
        uint64_t want=total+(total>>1);if(want<(1ULL<<20))want=1ULL<<20;
        G.want_cached=1;S.wb_stage=buffer_create(want,VK_BUFFER_USAGE_TRANSFER_DST_BIT,0);G.want_cached=0;
    }
    all_barrier();
    VkBufferCopy regions[64];uint32_t nregions=0;VkBuffer source=VK_NULL_HANDLE;uint64_t cursor=0;
    for(int i=0;i<S.nwrites;i++){
        Range r=S.writes[i];uint64_t off=0;VPool *p=pool_find(r.addr,r.size,&off);
        if(!p||!p->v.b)die("writeback stage pool lifetime",r.addr);
        if(nregions&&(source!=p->v.b||nregions==64)){vkCmdCopyBuffer(S.command,source,S.wb_stage.b,nregions,regions);nregions=0;}
        source=p->v.b;regions[nregions++]=(VkBufferCopy){off,cursor,r.size};cursor+=r.size;
    }
    if(nregions)vkCmdCopyBuffer(S.command,source,S.wb_stage.b,nregions,regions);
    S.wb_stage_total=total;S.wb_staged=1;
}
static void wb_stage_invalidate(VkSubmission *sub) {
    uint64_t atom=G.properties.limits.nonCoherentAtomSize;if(!atom)atom=1;
    uint64_t size=(sub->wb_stage_total+atom-1)/atom*atom;
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=sub->wb_stage.m,.offset=0,.size=size<sub->wb_stage.allocation_size?size:VK_WHOLE_SIZE};
    CHECK(vkInvalidateMappedMemoryRanges(G.device,1,&range));
}
static void submission_complete_effects(VkSubmission *sub) {
    if(sub->phase!=SUBMISSION_GPU_DONE || !(G.active||G.completing) || !pthread_equal(sub->recorder,pthread_self()))
        die("guest effects before owned GPU completion",sub->epoch);
    pass_timing_collect(sub);
    trace_capture_complete(sub);
    uint64_t wb_cpu=0,wb_faults=0,wb_preps=0,wb_mp_calls=0,wb_mp_pages=0,wb_mp_ns=0;
    if(sub->nwrites){nvn_dirty_stats(&wb_faults,&wb_preps);nvn_dirty_mp_stats(&wb_mp_calls,&wb_mp_pages,&wb_mp_ns);wb_cpu=dm_android_nvn_thread_cpu_ns();}
    size_t copy_count=(size_t)sub->nwrites;uint64_t raw_bytes=0,copied_bytes=0;
    if(G.enable_writeback_coalesce && sub->nwrites) {
        uint64_t plan_cpu=dm_android_nvn_thread_cpu_ns();
        for(int i=0;i<sub->nwrites;i++) {
            Range r=sub->writes[i];VPool *p=pool_find(r.addr,r.size,NULL);
            if(!p || !p->v.map)die("writeback plan pool lifetime",r.addr);
            sub->writeback_plan[i]=(NvnVkWritebackSpan){r.addr,r.size,p->serial};raw_bytes+=r.size;
        }
        copy_count=nvn_vk_writeback_plan(sub->writeback_plan,(size_t)sub->nwrites);
        if(copy_count==SIZE_MAX)die("invalid writeback plan",sub->epoch);
        G.profile_writeback_plan_cpu_ns+=dm_android_nvn_thread_cpu_ns()-plan_cpu;
    }
    uint64_t stage_cursor=0;
    if(sub->wb_staged&&sub->nwrites)wb_stage_invalidate(sub);
    for(size_t i=0;i<copy_count;i++) {
       Range r=G.enable_writeback_coalesce?(Range){sub->writeback_plan[i].addr,sub->writeback_plan[i].size}:sub->writes[i];
       uint64_t off=0;VPool *p=pool_find(r.addr,r.size,&off);
       if(!p || !p->v.map || (G.enable_writeback_coalesce && p->serial!=sub->writeback_plan[i].pool_serial))die("writeback pool lifetime",r.addr);
       /* The real fence has completed; prepare preserves dirty generations
        * while avoiding a write fault on each protected guest page. */
       if(G.enable_writeback_prepare)dm_guest_write_prepare((void *)(uintptr_t)r.addr,r.size);
       if(sub->wb_staged){memcpy((void *)(uintptr_t)r.addr,sub->wb_stage.map+stage_cursor,r.size);stage_cursor+=r.size;}
       else memcpy((void *)(uintptr_t)r.addr,p->v.map+off,r.size);copied_bytes+=r.size;
       if(!G.enable_writeback_coalesce)raw_bytes+=r.size;
    }
    G.profile_writeback_bytes+=copied_bytes;G.profile_writeback_raw_bytes+=raw_bytes;
    G.profile_writeback_raw_ranges+=sub->nwrites;G.profile_writeback_copy_ranges+=copy_count;
    if(getenv("DM_RESOURCE_STATS") && sub->nwrites)fprintf(stderr,"[nvn-vulkan] WRITEBACK_STATS epoch=%llu enabled=%u raw_ranges=%d copied_ranges=%zu raw_bytes=%llu copied_bytes=%llu\n",(unsigned long long)sub->epoch,G.enable_writeback_coalesce,sub->nwrites,copy_count,(unsigned long long)raw_bytes,(unsigned long long)copied_bytes);
    if(sub->nwrites){
        G.profile_writeback_cpu_ns+=dm_android_nvn_thread_cpu_ns()-wb_cpu;G.profile_writeback_calls++;
        uint64_t faults=0,preps=0,mp_calls=0,mp_pages=0,mp_ns=0;nvn_dirty_stats(&faults,&preps);nvn_dirty_mp_stats(&mp_calls,&mp_pages,&mp_ns);
        /* Existing dirty counters are process-wide: concurrent guest writes
         * can contribute to these observed deltas during the copy loop. */
        G.profile_writeback_global_faults+=faults-wb_faults;G.profile_writeback_global_preps+=preps-wb_preps;
        G.profile_writeback_global_mp_calls+=mp_calls-wb_mp_calls;G.profile_writeback_global_mp_pages+=mp_pages-wb_mp_pages;
    }
    uint64_t occ[LIMIT_WRITES],times[LIMIT_WRITES];
    if(sub->nocc)CHECK(vkGetQueryPoolResults(G.device,sub->occlusion,0,sub->nocc,sub->nocc*8,occ,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    if(G.present_count<2 || G.trace_frame)for(uint32_t i=0;i<sub->nocc;++i)fprintf(stderr,"[nvn-vulkan] DRAW_SAMPLES query=%u samples=%llu\n",i,(unsigned long long)occ[i]);
    if(sub->ntime)CHECK(vkGetQueryPoolResults(G.device,sub->timestamps,0,sub->ntime,sub->ntime*8,times,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    for(int i=0;i<sub->ncounter;++i) {CounterOp *op=&sub->counter_ops[i];
        if(op->kind==0)G.samples_passed=0;
        else if(op->kind==1)G.samples_passed+=occ[op->query];
        else {uint64_t ticks=times[op->query];if(G.timestamp_bits<64)ticks&=(1ULL<<G.timestamp_bits)-1;
            uint64_t stamp=(uint64_t)((long double)ticks*G.properties.limits.timestampPeriod*12/625);
            uint64_t result[2]={op->kind==2?G.samples_passed:stamp,stamp},off=0;
            VPool *p=pool_find(op->address,16,&off);if(!p)die("counter report pool lifetime",op->address);
            if(p->v.map)memcpy(p->v.map+off,result,16);memcpy((void *)(uintptr_t)op->address,result,16);}
    }
    for(int i=0;i<sub->nevents;i++) { Event *e=&sub->events[i]; if(e->mode) atomic_fetch_add((_Atomic uint32_t *)e->addr,e->value);
       else atomic_store((_Atomic uint32_t *)e->addr,e->value); }
    submission_publish(sub);
}
static pthread_cond_t g_done_cv=PTHREAD_COND_INITIALIZER,g_job_cv=PTHREAD_COND_INITIALIZER;
typedef struct { VkSubmission *ctx;int kind; } AsyncJob;
static AsyncJob g_jobs[8];static int g_jhead,g_jcount,g_worker_up;
static int ctx_busy(const VkSubmission *c) { return c->phase==SUBMISSION_INFLIGHT||c->phase==SUBMISSION_GPU_DONE||c->phase==SUBMISSION_PUBLISHED; }
/* Caller holds G.lock; the condition wait releases it so the completion worker can publish. */
static void async_drain_locked(void) { while(g_jcount)pthread_cond_wait(&g_done_cv,&G.lock); }
static void sub_begin_ctx(VkSubmission *ctx) {
    mtl_init();uint64_t begin_cpu=dm_android_nvn_thread_cpu_ns();pthread_mutex_lock(&G.lock);
    if(ctx_busy(ctx)){uint64_t w0=dm_android_nvn_clock_ns();while(ctx_busy(ctx))pthread_cond_wait(&g_done_cv,&G.lock);G.profile_wait_ns+=dm_android_nvn_clock_ns()-w0;}
    G.current_submission=ctx;
    if(G.active || (S.phase!=SUBMISSION_UNUSED && S.phase!=SUBMISSION_RETIRED))die("nested/unretired submission",S.epoch);
    if(S.phase==SUBMISSION_UNUSED)submission_pools_create(&S);
    else {
        /* Executable buffers release references before descriptor sets are reset.
         * The slot's fence and publication both completed; no in-flight object is reset. */
        CHECK(vkGetFenceStatus(G.device,S.gpu_fence));
        CHECK(vkResetCommandPool(G.device,S.command_pool,0));CHECK(vkResetDescriptorPool(G.device,S.descriptors,0));
        CHECK(vkResetFences(G.device,1,&S.gpu_fence));G.profile_submission_reuses++;
    }
    S.command=S.command_primary;S.chunk_index=0;S.chunk_mark=G.draws;G.seg_begin_ns=dm_android_nvn_clock_ns();
    S.phase=SUBMISSION_RECORDING;S.epoch=++G.recording_epoch;S.ticket=0;S.recorder=pthread_self();G.active=1;
    S.nwrites=S.nevents=S.ntemporary=S.npipeline=S.npass=S.nread_snapshots=S.ndirect_reads=S.novf=0;S.in_pass=0;
    S.ndriver_tables=0;memset(S.driver_table_slots,0,sizeof S.driver_table_slots);memset(S.last_driver_table,0,sizeof S.last_driver_table);
    S.nocc=S.ntime=0;S.ncounter=0;S.nview=0;S.nbuffer_view=0;memset(S.view_table,0,sizeof S.view_table);
    S.occlusion_reset_count=S.timestamp_reset_count=S.pass_reset_count=0;
    S.ndesc=0;S.desc_arena_used=0;memset(S.desc_table,0,sizeof S.desc_table);S.command_dirty=1;memset(S.pend,0,sizeof S.pend);
    S.submission_cpu_readback=S.submission_event_wait=0;S.active_occlusion=UINT32_MAX;
    VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(S.command,&b));
    nvn_vk_command_state_reset(&S.command_state);
    if(S.chunk_ts){vkCmdResetQueryPool(S.command,S.chunk_ts,0,128);S.nchunk_ts=0;chunk_ts_write(0);}
    if(!G.enable_query_reset_range){query_reset(S.occlusion,0,LIMIT_WRITES);query_reset(S.timestamps,0,LIMIT_WRITES);S.occlusion_reset_count=S.timestamp_reset_count=LIMIT_WRITES;}
    if(S.pass_ts){if(!G.enable_query_reset_range){query_reset(S.pass_ts,0,1024);S.pass_reset_count=1024;}S.npt=0;S.pt_open=0;}
    G.profile_begin_cpu_ns+=dm_android_nvn_thread_cpu_ns()-begin_cpu;G.profile_begin_calls++;
}
void mtl_sub_begin(void) { sub_begin_ctx(&G.submission_slot); }
void mtl_sub_copy(uint64_t src,uint64_t dst,uint64_t len) {
    all_barrier(); uint64_t so=0,doff=0; VPool *s=pool_read(src,len,&so),*d=pool_find(dst,len,&doff);
    if(!d || overlap(src,len,dst,len)) die("buffer copy bounds/overlap",dst);pool_materialize(d);
    VkBufferCopy copy={so,doff,len}; vkCmdCopyBuffer(S.command,s->v.b,d->v.b,1,&copy); mark_write(dst,len);
}
void mtl_sub_event(volatile uint32_t *addr,uint32_t mode,uint32_t value) {
    if(mode>1 || S.nevents==LIMIT_WRITES) die("event mode/count",mode);
    S.events[S.nevents++]=(Event){addr,mode,value};
}
static void counter_op(uint32_t kind,uint32_t query,uint64_t address) {
    if(S.ncounter==LIMIT_WRITES)die("counter operation limit",S.ncounter);
    S.counter_ops[S.ncounter++]=(CounterOp){kind,query,address};G.profile_counter_ops++;
}
void mtl_sub_counter(uint64_t addr,uint32_t type) {
    if(type!=NVN_COUNTER_TYPE_SAMPLES_PASSED && type!=NVN_COUNTER_TYPE_TIMESTAMP && type!=NVN_COUNTER_TYPE_TIMESTAMP_TOP)
        die("GPU counter type implementation pending",type);
    if(!pool_find(addr,16,NULL) || addr%8)die("GPU counter report range/alignment",addr);
    if(!G.timestamp_bits || S.ntime==LIMIT_WRITES)die("GPU timestamp support/count",S.ntime);
    pass_end();uint32_t q=S.ntime++;
    query_reserve(S.timestamps,&S.timestamp_reset_count,q,1,LIMIT_WRITES);
    vkCmdWriteTimestamp(S.command,type==NVN_COUNTER_TYPE_TIMESTAMP_TOP?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,S.timestamps,q);
    counter_op(type==NVN_COUNTER_TYPE_SAMPLES_PASSED?2:3,q,addr);
}
typedef struct { unsigned reports;int candidate,traced;uint64_t wait_cpu,wait_start; } CommitInfo;
static uint64_t commit_after_gpu(CommitInfo ci,uint64_t waited);
static CommitInfo commit_submit(void) {
    uint64_t prepare_cpu=dm_android_nvn_thread_cpu_ns();
    if(!G.active) die("commit without begin",0); pass_end();
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] QUERY_RESET_STATS enabled=%d occ_reset=%u time_reset=%u pass_reset=%u occ_used=%u time_used=%u pass_used=%u\n",G.enable_query_reset_range,S.occlusion_reset_count,S.timestamp_reset_count,S.pass_reset_count,S.nocc,S.ntime,S.npt*2);
    /* Observed candidates only: resource lifetime and mapped-pool hazards have
     * not been solved. Internal counter reset/add operations are not reports. */
    unsigned reports=0;for(int i=0;i<S.ncounter;i++)if(S.counter_ops[i].kind==2 || S.counter_ops[i].kind==3)reports++;G.profile_counter_reports+=reports;
    int traced=G.trace_frame || S.ntrace_captures;
    int candidate=!S.nwrites && !S.nevents && !reports && !traced && !S.submission_cpu_readback && !S.submission_event_wait;
    wb_stage_record();
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    if(G.chunk_draws&&S.chunk_index&&G.chunk_log)fprintf(stderr,"[nvn-vulkan] CHUNKED_SUBMIT chunks=%u\n",S.chunk_index+1);
    chunk_ts_write(1);CHECK(vkEndCommandBuffer(S.command));VkFence fence=S.gpu_fence;VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&S.command};
    if(S.phase!=SUBMISSION_RECORDING)die("submit phase",S.phase);
    CHECK(vkQueueSubmit(G.queue,1,&si,fence));S.ticket=++G.fence;S.phase=SUBMISSION_INFLIGHT;
    uint64_t wait_cpu=dm_android_nvn_thread_cpu_ns();G.profile_commit_prepare_cpu_ns+=wait_cpu-prepare_cpu;G.profile_commit_calls++;
    CommitInfo ci={reports,candidate,traced,wait_cpu,dm_android_nvn_clock_ns()};
    /* Snapshot recorder-owned counters before another context begins or present
     * resets the frame's segment index. Async completion must not read them. */
    S.profile_segment=G.seg_index<3?G.seg_index:3;
    G.seg_record_ns[S.profile_segment]+=ci.wait_start-G.seg_begin_ns;
    G.seg_draws[S.profile_segment]+=G.draws-S.chunk_mark;
    G.seg_chunks[S.profile_segment]+=S.chunk_index+1;G.seg_index++;
    return ci;
}
uint64_t mtl_sub_commit(void) {
    CommitInfo ci=commit_submit();VkFence fence=S.gpu_fence;
    uint64_t wait_start=ci.wait_start;CHECK(cpu_work_wait_fences(G.device,1,&fence,VK_TRUE,UINT64_MAX));uint64_t waited=dm_android_nvn_clock_ns()-wait_start;
    uint64_t ticket=commit_after_gpu(ci,waited);
    pthread_mutex_unlock(&G.lock);return ticket;
}
static uint64_t commit_after_gpu(CommitInfo ci,uint64_t waited) {
    uint64_t wait_cpu=ci.wait_cpu;unsigned reports=ci.reports;int candidate=ci.candidate,traced=ci.traced;
    if(S.chunk_ts&&S.nchunk_ts>=2){unsigned si=S.profile_segment;uint64_t ts[128];
        if(vkGetQueryPoolResults(G.device,S.chunk_ts,0,S.nchunk_ts&~1u,sizeof ts,ts,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)==VK_SUCCESS){
            uint64_t mask=G.timestamp_bits>=64?~0ULL:((1ULL<<G.timestamp_bits)-1);double period=G.properties.limits.timestampPeriod;uint64_t busy=0,gap=0;
            if(G.profile_frames==30&&si==0)for(uint32_t i=0;i+1<(S.nchunk_ts&~1u);i+=2)fprintf(stderr,"[nvn-vulkan] CHUNK_TIME i=%u dur_us=%.0f gap_us=%.0f passes=%llu draws=%llu\n",i/2,(double)(((ts[i+1]-ts[i])&mask)*period/1000.0),i?(double)(((ts[i]-ts[i-1])&mask)*period/1000.0):0.0,(unsigned long long)(S.chunk_pass0[i+1]-S.chunk_pass0[i]),(unsigned long long)(S.chunk_draw0[i+1]-S.chunk_draw0[i]));
            for(uint32_t i=0;i+1<(S.nchunk_ts&~1u);i+=2){busy+=(ts[i+1]-ts[i])&mask;if(i>=2){uint64_t g=(ts[i]-ts[i-1])&mask;if(g<(1ULL<<40))gap+=g;}}
            G.gts_busy[si]+=(uint64_t)(busy*period);G.gts_gap[si]+=(uint64_t)(gap*period);G.gts_span[si]+=(uint64_t)(((ts[(S.nchunk_ts&~1u)-1]-ts[0])&mask)*period);G.gts_chunks[si]+=S.nchunk_ts/2;}}
    G.seg_wait_ns[S.profile_segment]+=waited;
    submission_gpu_done(&S);
    uint64_t retire_cpu=dm_android_nvn_thread_cpu_ns();G.profile_commit_wait_cpu_ns+=retire_cpu-wait_cpu;
    G.profile_wait_ns+=waited;G.profile_submits++;
    if(candidate){G.profile_submit_candidates++;G.profile_candidate_wait_ns+=waited;}
    G.profile_write_submits+=S.nwrites!=0;G.profile_event_submits+=S.nevents!=0;G.profile_report_submits+=reports!=0;
    G.profile_trace_submits+=traced!=0;G.profile_cpu_readback_submits+=S.submission_cpu_readback!=0;G.profile_event_wait_submits+=S.submission_event_wait!=0;
    G.profile_events+=S.nevents;G.profile_reports+=reports;
    submission_complete_effects(&S);
    for(int i=0;i<S.npipeline;i++) { vkDestroyPipeline(G.device,S.pipelines[i],NULL); vkDestroyPipelineLayout(G.device,S.layouts[i],NULL); }
    for(int i=0;i<S.npass;i++)vkDestroyFramebuffer(G.device,S.framebuffers[i],NULL);
    /* 通道对象由 G.rp_cache 持有（会话级寿命）；仅溢出创建的随 epoch 退休
     * | passes live in G.rp_cache (session lifetime); only overflow ones retire with the epoch */
    for(int i=0;i<S.novf;i++)vkDestroyRenderPass(G.device,S.overflow_passes[i],NULL);
    temporaries_retire(&S);
    for(int i=0;i<S.nview;++i)vkDestroyImageView(G.device,S.temporary_views[i],NULL);
    for(int i=0;i<S.nbuffer_view;++i)vkDestroyBufferView(G.device,S.buffer_views[i],NULL);
    rt_dead_flush();
    if(getenv("DM_PSO_STATS"))fprintf(stderr,"[nvn-vulkan] PSO_STATS cached=%u hits=%llu misses=%llu\n",G.npso,(unsigned long long)G.profile_pso_hits,(unsigned long long)G.profile_pso_misses);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] VIEW_CACHE_STATS view_hits=%llu view_misses=%llu framebuffer_hits=%llu framebuffer_misses=%llu\n",(unsigned long long)G.profile_view_hits,(unsigned long long)G.profile_view_misses,(unsigned long long)G.profile_framebuffer_hits,(unsigned long long)G.profile_framebuffer_misses);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] POOL_UPLOAD_STATS uploaded_bytes=%llu reused_ranges=%llu reused_bytes=%llu\n",(unsigned long long)G.profile_pool_uploaded_bytes,(unsigned long long)G.profile_pool_reused_ranges,(unsigned long long)G.profile_pool_reused_bytes);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] RESOURCE_STATS driver_tables=%llu driver_table_reuses=%llu buffer_allocs=%llu buffer_bytes=%llu\n",(unsigned long long)G.profile_driver_tables,(unsigned long long)G.profile_driver_table_reuses,(unsigned long long)G.profile_buffer_allocs,(unsigned long long)G.profile_buffer_bytes);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] TEMP_STATS reuses=%llu cached=%u cached_bytes=%llu aligned_bytes=%llu comparison_bytes=%llu driver_bytes=%llu other_bytes=%llu\n",(unsigned long long)G.profile_temp_reuses,G.nretired,(unsigned long long)G.retired_bytes,(unsigned long long)G.profile_temp_bytes[TEMP_ALIGNED],(unsigned long long)G.profile_temp_bytes[TEMP_COMPARISON],(unsigned long long)G.profile_temp_bytes[TEMP_DRIVER],(unsigned long long)G.profile_temp_bytes[TEMP_OTHER]);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] STORAGE_BIAS_STATS bindings=%llu avoided_snapshot_bytes=%llu\n",(unsigned long long)G.profile_storage_bias_bindings,(unsigned long long)G.profile_storage_bias_bytes);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] QUERY_STATS queries=%u counter_ops=%d\n",S.nocc,S.ncounter);
    if(getenv("DM_RESOURCE_STATS"))command_state_profile("COMMAND_STATE_STATS",G.present_count+1,G.profile_frames);
    if(getenv("DM_RESOURCE_STATS"))dirty_query_profile("DIRTY_QUERY_STATS",G.present_count+1,G.profile_frames,0);
    if(getenv("DM_RESOURCE_STATS"))fprintf(stderr,"[nvn-vulkan] SUBMISSION_POOL_STATS epoch=%llu ticket=%llu phase=%u creates=%llu reuses=%llu command=%llx descriptors=%llx query=%llx fence=%llx gpu_complete=1 effects_published=1\n",
        (unsigned long long)S.epoch,(unsigned long long)S.ticket,(unsigned)S.phase,(unsigned long long)G.profile_submission_creates,(unsigned long long)G.profile_submission_reuses,
        (unsigned long long)(uintptr_t)S.command,(unsigned long long)(uintptr_t)S.descriptors,(unsigned long long)(uintptr_t)S.timestamps,(unsigned long long)(uintptr_t)S.gpu_fence);
    uint64_t ticket=S.ticket;submission_released(&S);G.profile_retire_cpu_ns+=dm_android_nvn_thread_cpu_ns()-retire_cpu;return ticket;
}
static void present_after_gpu(void) {
    submission_gpu_done(&S);pass_timing_collect(&S);trace_capture_complete(&S);
    vkDestroySemaphore(G.device,S.present_available,NULL);S.present_available=VK_NULL_HANDLE;
    for(int i=0;i<S.nview;++i)vkDestroyImageView(G.device,S.temporary_views[i],NULL);
    temporaries_retire(&S);
    rt_dead_flush();
    if(S.nwrites || S.nevents || S.ncounter)die("present has unpublished guest effects",S.epoch);
    submission_publish(&S);submission_released(&S);
}
/* Completion worker: publishes finished submissions strictly in submission order while the guest thread records
 * the next frame. It runs the same completion code as the synchronous path with G.lock held. */
static void *async_worker(void *arg) {
    (void)arg;
    for(;;) {
        pthread_mutex_lock(&G.lock);
        while(!g_jcount)pthread_cond_wait(&g_job_cv,&G.lock);
        AsyncJob job=g_jobs[g_jhead];
        pthread_mutex_unlock(&G.lock);
        VkFence fence=job.ctx->gpu_fence;
        CHECK(vkWaitForFences(G.device,1,&fence,VK_TRUE,UINT64_MAX));
        pthread_mutex_lock(&G.lock);
        VkSubmission *saved=G.current_submission;int saved_active=G.active;G.current_submission=job.ctx;G.completing=1;job.ctx->recorder=pthread_self();
        if(job.kind==0){CommitInfo ci;memcpy(&ci,&job.ctx->commit_info,sizeof ci);ci.wait_cpu=dm_android_nvn_thread_cpu_ns();commit_after_gpu(ci,0);}
        else present_after_gpu();
        G.completing=0;G.current_submission=saved;G.active=saved_active;
        g_jhead=(g_jhead+1)%8;g_jcount--;
        pthread_cond_broadcast(&g_done_cv);
        pthread_mutex_unlock(&G.lock);
    }
}
static void async_enqueue_locked(VkSubmission *ctx,int kind) {
    if(g_jcount==8)die("async job queue overflow",g_jcount);
    if(!g_worker_up){pthread_t thread;if(pthread_create(&thread,NULL,async_worker,NULL))die("async worker creation",0);pthread_detach(thread);g_worker_up=1;}
    g_jobs[(g_jhead+g_jcount)%8]=(AsyncJob){ctx,kind};g_jcount++;
    pthread_cond_signal(&g_job_cv);
}
/* Submit the recorded segment and return without waiting; the worker waits for the fence and publishes effects. */
void mtl_sub_commit_async(void) {
    if(!G.async_enabled){mtl_sub_commit();return;}
    CommitInfo ci=commit_submit();
    memcpy(&S.commit_info,&ci,sizeof ci);G.active=0;
    async_enqueue_locked(&S,0);
    if(G.async_drain_after_submit)async_drain_locked();
    pthread_mutex_unlock(&G.lock);
}
int mtl_async_enabled(void) { mtl_init();return G.async_enabled; }
uint64_t mtl_last_fence(void) { return G.fence; }
int mtl_last_commit_error(void) { return 0; } /* Errors terminate at CHECK before completion is published. */
int mtl_wait(uint64_t fence,uint64_t timeout) {
    if(fence<=atomic_load(&G.completed))return 1;
    if(!timeout)return 0;
    uint64_t start=dm_android_nvn_clock_ns();
    while(fence>atomic_load(&G.completed)){
        if(timeout!=UINT64_MAX&&dm_android_nvn_clock_ns()-start>=timeout)return 0;
        struct timespec pause={0,100000};nanosleep(&pause,NULL);
    }
    return 1;
}
static void pso_cache_save(void) {
    if(!G.pipeline_cache || !g_pso_cache_path[0]) return;
    size_t n=0;
    if(vkGetPipelineCacheData(G.device,G.pipeline_cache,&n,NULL)!=VK_SUCCESS || !n) return;
    void *buf=malloc(n); if(!buf) return;
    size_t got=n;
    if(vkGetPipelineCacheData(G.device,G.pipeline_cache,&got,buf)==VK_SUCCESS && got){
        char tmp[sizeof g_pso_cache_path+8];
        snprintf(tmp,sizeof tmp,"%s.tmp",g_pso_cache_path);
        FILE *f=fopen(tmp,"wb");
        if(f){ size_t w=fwrite(buf,1,got,f); int ok=!fclose(f) && w==got;
            if(ok)rename(tmp,g_pso_cache_path);
            fprintf(stderr,"[nvn-vulkan] PSO_CACHE save %zu bytes%s\n",got,ok?"":" (写入失败)"); }
    }
    free(buf);g_pso_cache_dirty=0;
}
/* 冷建统计（上游建议 §2 的尺子）：只有 g_pso2 未命中才走 vkCreate*Pipelines；耗时 ≥2ms 判"冷"（驱动缓存的
 * 热命中通常 <1ms）。逐条记慢建 + 累计计数，RENDER_PROFILE 行带出——决定要不要做启动预热 / 后台编译。
 * Cold-creation accounting (the ruler for upstream advice §2): vkCreate*Pipelines is only reached on a
 * g_pso2 miss; ≥2 ms counts as cold (a driver-cache hit is normally <1 ms). Slow builds are logged
 * individually plus cumulative counters, carried on the RENDER_PROFILE line -- this decides whether
 * startup pre-warm / background compilation is worth building. */
static void pso_create_account(const char *kind,uint64_t ns,uint64_t key,uint64_t frame) {
    atomic_fetch_add(&g_pso_create_count,1);
    if(ns<2000000ull)return;
    atomic_fetch_add(&g_pso_slow_count,1);atomic_fetch_add(&g_pso_slow_total_ns,(unsigned long)ns);
    fprintf(stderr,"[nvn-vulkan] PSO_COLD kind=%s ms=%.1f frame=%llu key=%016llx\n",kind,(double)ns/1e6,(unsigned long long)frame,(unsigned long long)key);
}
void mtl_pso_cache_flush(void) { mtl_init(); pthread_mutex_lock(&G.lock); if(g_pso_cache_dirty)pso_cache_save(); pthread_mutex_unlock(&G.lock); }
void mtl_finish(void) { mtl_init(); pthread_mutex_lock(&G.lock); async_drain_locked(); CHECK(cpu_work_queue_idle(G.queue)); pso_cache_save(); pthread_mutex_unlock(&G.lock); }
void mtl_drain(void) { mtl_finish(); }

static VkFormat format(int nvn) {
    switch(nvn) {
    case NVN_FORMAT_R8:return VK_FORMAT_R8_UNORM; case NVN_FORMAT_RG8:return VK_FORMAT_R8G8_UNORM;
    case NVN_FORMAT_R16:return VK_FORMAT_R16_UNORM;case NVN_FORMAT_RG16:return VK_FORMAT_R16G16_UNORM;case NVN_FORMAT_RGBA16:return VK_FORMAT_R16G16B16A16_UNORM;
    case NVN_FORMAT_R8SN:return VK_FORMAT_R8_SNORM;case NVN_FORMAT_RG8SN:return VK_FORMAT_R8G8_SNORM;case NVN_FORMAT_RGBA8SN:return VK_FORMAT_R8G8B8A8_SNORM;
    case NVN_FORMAT_R16SN:return VK_FORMAT_R16_SNORM;case NVN_FORMAT_RG16SN:return VK_FORMAT_R16G16_SNORM;case NVN_FORMAT_RGBA16SN:return VK_FORMAT_R16G16B16A16_SNORM;
    case NVN_FORMAT_R8UI:return VK_FORMAT_R8_UINT;case NVN_FORMAT_RG8UI:return VK_FORMAT_R8G8_UINT;case NVN_FORMAT_RGBA8UI:return VK_FORMAT_R8G8B8A8_UINT;
    case NVN_FORMAT_R32UI:return VK_FORMAT_R32_UINT;case NVN_FORMAT_RG32UI:return VK_FORMAT_R32G32_UINT;case NVN_FORMAT_RGBA32UI:return VK_FORMAT_R32G32B32A32_UINT;
    case NVN_FORMAT_R16F:return VK_FORMAT_R16_SFLOAT; case NVN_FORMAT_RG16F:return VK_FORMAT_R16G16_SFLOAT;
    case NVN_FORMAT_RGBA16F:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case NVN_FORMAT_R11G11B10F:return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case NVN_FORMAT_R32F:return VK_FORMAT_R32_SFLOAT; case NVN_FORMAT_RG32F:return VK_FORMAT_R32G32_SFLOAT;
    case NVN_FORMAT_RGB32F:return VK_FORMAT_R32G32B32_SFLOAT; case NVN_FORMAT_RGBA32F:return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NVN_FORMAT_RGBA8:return VK_FORMAT_R8G8B8A8_UNORM; case NVN_FORMAT_RGBA8_SRGB:return VK_FORMAT_R8G8B8A8_SRGB;
    case NVN_FORMAT_RGBA8_UI2F:return VK_FORMAT_R8G8B8A8_USCALED;case NVN_FORMAT_RGBA8_I2F:return VK_FORMAT_R8G8B8A8_SSCALED;
    case NVN_FORMAT_DEPTH16:return VK_FORMAT_D16_UNORM; case NVN_FORMAT_DEPTH32F:return VK_FORMAT_D32_SFLOAT;
    case NVN_FORMAT_DEPTH24_STENCIL8:return VK_FORMAT_D24_UNORM_S8_UINT;
    case NVN_FORMAT_DEPTH32F_STENCIL8:return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:die("NVN pixel/vertex format",nvn);
    }
}
static VkImageAspectFlags aspect(VkFormat f) {
    if(f==VK_FORMAT_D16_UNORM || f==VK_FORMAT_D32_SFLOAT) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if(f==VK_FORMAT_D24_UNORM_S8_UINT || f==VK_FORMAT_D32_SFLOAT_S8_UINT) return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}
/* Access types implied by a layout. Read-only layouts carry no source access: earlier writes were made visible
 * to every read type when the image entered that layout. */
static VkAccessFlags layout_access(VkImageLayout layout,int as_source) {
    const VkAccessFlags reads=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    switch(layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:return 0;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:return as_source?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:return as_source?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:return as_source?VK_ACCESS_TRANSFER_WRITE_BIT:(VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT);
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:return as_source?0:reads;
    default:return VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    }
}
static void texture_barrier_levels(VTexture *t,VkImageLayout old,VkImageLayout layout,uint32_t base,uint32_t count) {
    pass_end();
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
       .srcAccessMask=old==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,
       .dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,.oldLayout=old,.newLayout=layout,
       .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->image,
       .subresourceRange={aspect(t->format),base,count,0,t->source.L.layers}};
    if(G.precise_sync){
        VkPipelineStageFlags src=old==VK_IMAGE_LAYOUT_UNDEFINED?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
            (HZ_ATT_STAGES|HZ_SHADER_STAGES|VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        VkPipelineStageFlags dst=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        switch(layout) {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:dst=HZ_ATT_STAGES;break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            dst=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            if(G.xition_vertex)dst|=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:dst=VK_PIPELINE_STAGE_TRANSFER_BIT;break;
        default:break;
        }
        if(G.enable_narrow_access){b.srcAccessMask=layout_access(old,1);b.dstAccessMask=layout_access(layout,0);}
        if(G.hz_diag&1){src=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;dst=VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;}
        vkCmdPipelineBarrier(S.command,src,dst,0,0,NULL,0,NULL,1,&b);
        G.profile_hz_barriers++;G.hz_count[4+(layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL?0:(dst==HZ_ATT_STAGES?1:(dst==VK_PIPELINE_STAGE_TRANSFER_BIT?2:3)))]++;
    }else
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
}
/* Whole-image transitions unify any per-mip layouts first; per-mip transitions let a mip chain be sampled and rendered
 * in place without the sampling copy that devices lacking feedback layouts would otherwise need. */
static void texture_transition(VTexture *t,VkImageLayout layout) {
    if(!t->mixed){if(t->layout==layout)return;texture_barrier_levels(t,t->layout,layout,0,t->source.d.levels);t->layout=layout;return;}
    uint32_t n=t->source.d.levels;
    for(uint32_t i=0;i<n;){if(t->lvl[i]==layout){i++;continue;}uint32_t j=i+1;while(j<n&&t->lvl[j]==t->lvl[i])j++;texture_barrier_levels(t,t->lvl[i],layout,i,j-i);i=j;}
    t->mixed=0;t->layout=layout;
}
static int texture_range_ok(const VTexture *t) {
    return !G.disable_subres&&!G.feedback_layout&&t->source.d.levels>1&&t->source.d.levels<=16&&t->source.L.layers==1&&
        t->source.d.target==NVN_TEXTURE_TARGET_2D&&aspect(t->format)==VK_IMAGE_ASPECT_COLOR_BIT;
}
static void texture_transition_levels(VTexture *t,uint32_t base,uint32_t count,VkImageLayout layout) {
    uint32_t n=t->source.d.levels;
    if(base==0&&count>=n){texture_transition(t,layout);return;}
    if(!t->mixed){if(t->layout==layout)return;for(uint32_t i=0;i<n;i++)t->lvl[i]=t->layout;t->mixed=1;}
    for(uint32_t i=base;i<base+count;){if(t->lvl[i]==layout){i++;continue;}VkImageLayout old=t->lvl[i];uint32_t j=i+1;while(j<base+count&&t->lvl[j]==old)j++;
        texture_barrier_levels(t,old,layout,i,j-i);for(uint32_t k=i;k<j;k++)t->lvl[k]=layout;i=j;}
    uint32_t k=1;while(k<n&&t->lvl[k]==t->lvl[0])k++;
    if(k==n){t->mixed=0;t->layout=t->lvl[0];}
}
/* GPU transfers and CPU streaming can update different compression blocks of
 * one NVN allocation. Compare actual block bytes to the last CPU snapshot so
 * page-level dirtiness never overwrites unrelated GPU-owned contents. */
static void refresh_compressed_cpu(VTexture *t,const tex_t *source) {
    uint64_t started=dm_android_nvn_clock_ns(),stamp=nvn_dirty_now();nvn_dirty_protect(source->mem,source->L.size);
    uint8_t *snapshot=malloc(source->L.size);if(!snapshot)die("streamed BC snapshot allocation",source->L.size);
    memcpy(snapshot,source->mem,source->L.size);
    unsigned changed_regions=0;
    for(unsigned layer=0;layer<source->L.layers;layer++)for(unsigned level=0;level<source->d.levels;level++){
        unsigned width=t->width>>level,height=t->height>>level,depth=t->depth>>level;if(!width)width=1;if(!height)height=1;if(!depth)depth=1;
        int bw,bh;int block=nvn_fmt_bpb(source->d.format,&bw,&bh);unsigned columns=(width+bw-1)/bw,rows=(height+bh-1)/bh;
        uint64_t row=(uint64_t)columns*block,linear_bytes=row*rows*depth;
        uint8_t *old=malloc(linear_bytes),*next=malloc(linear_bytes);if(!old||!next)die("streamed BC block comparison allocation",linear_bytes);
        if(nvn_deswizzle(&source->d,level,layer,t->cpu_snapshot,old,row)||nvn_deswizzle(&source->d,level,layer,snapshot,next,row))die("streamed BC block layout",source->serial);
        if(!memcmp(old,next,linear_bytes)){free(old);free(next);continue;}
        VkBufferImageCopy *regions=NULL;unsigned count=0,capacity=0;
        if(t->bc_block){
            /* 压缩直传：staging = 整层压缩块流；区域按“块”表述缓冲区坐标
             * （压缩格式的 bufferRowLength/bufferImageHeight 以块计）。 */
            uint64_t compressed=(uint64_t)row*rows*depth;
            VBuffer *staging=temporary(compressed,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            memcpy(staging->map,next,compressed);
            for(unsigned z=0;z<depth;z++)for(unsigned y=0;y<rows;y++){
                unsigned x=0;while(x<columns){uint64_t offset=((uint64_t)z*rows+y)*row+(uint64_t)x*block;
                    if(!memcmp(old+offset,next+offset,block)){x++;continue;}
                    unsigned first=x++;while(x<columns&&memcmp(old+((uint64_t)z*rows+y)*row+(uint64_t)x*block,next+((uint64_t)z*rows+y)*row+(uint64_t)x*block,block))x++;
                    if(count==capacity){capacity=capacity?capacity*2:64;VkBufferImageCopy *larger=realloc(regions,(size_t)capacity*sizeof(*regions));if(!larger)die("streamed BC copy regions",capacity);regions=larger;}
                    unsigned px=first*bw,py=y*bh,w=(x-first)*bw,h=bh;if(w>width-px)w=width-px;if(h>height-py)h=height-py;
                    regions[count++]=(VkBufferImageCopy){.bufferOffset=offset,.bufferRowLength=columns,.bufferImageHeight=rows,
                        .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1},.imageOffset={px,py,z},.imageExtent={w,h,1}};
                }
            }
            all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(S.command,staging->b,t->image,t->layout,count,regions);changed_regions+=count;
            free(regions);free(old);free(next);
        } else {
        uint64_t plane=((uint64_t)width*height*t->bpp+3)&~3ULL;
        VBuffer *staging=temporary(plane*depth,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for(unsigned z=0;z<depth;z++)if(nvn_bc_decode(source->d.format,next+z*row*rows,row,width,height,staging->map+z*plane,(size_t)width*t->bpp))die("streamed BC block decode",source->d.format);
        VkBufferImageCopy *regions=NULL;unsigned count=0,capacity=0;
        for(unsigned z=0;z<depth;z++)for(unsigned y=0;y<rows;y++){
            unsigned x=0;while(x<columns){uint64_t offset=((uint64_t)z*rows+y)*row+(uint64_t)x*block;
                if(!memcmp(old+offset,next+offset,block)){x++;continue;}
                unsigned first=x++;while(x<columns&&memcmp(old+((uint64_t)z*rows+y)*row+(uint64_t)x*block,next+((uint64_t)z*rows+y)*row+(uint64_t)x*block,block))x++;
                if(count==capacity){capacity=capacity?capacity*2:64;VkBufferImageCopy *larger=realloc(regions,(size_t)capacity*sizeof(*regions));if(!larger)die("streamed BC copy regions",capacity);regions=larger;}
                unsigned px=first*bw,py=y*bh,w=(x-first)*bw,h=bh;if(w>width-px)w=width-px;if(h>height-py)h=height-py;
                regions[count++]=(VkBufferImageCopy){.bufferOffset=z*plane+((uint64_t)py*width+px)*t->bpp,.bufferRowLength=width,.bufferImageHeight=height,
                    .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1},.imageOffset={px,py,z},.imageExtent={w,h,1}};
            }
        }
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(S.command,staging->b,t->image,t->layout,count,regions);changed_regions+=count;
        free(regions);free(old);free(next);
        }
    }
    free(t->cpu_snapshot);t->cpu_snapshot=snapshot;t->upload_stamp=stamp;
    if(changed_regions){tex_dirty(t);G.uploads++;fprintf(stderr,"[nvn-vulkan] BC_STREAM_REFRESH serial=%llu changed_regions=%u gpu_blocks_preserved=1\n",(unsigned long long)t->serial,changed_regions);}
    {uint64_t refresh_ms=(dm_android_nvn_clock_ns()-started)/1000000ull;
     if(refresh_ms>=20) fprintf(stderr,"[nvn-vulkan] TEXTURE_REFRESH_SLOW serial=%llu %ux%u bytes=%llu fmt=%#x bc=%u changed=%u ms=%llu\n",
         (unsigned long long)t->serial,t->source.d.width,t->source.d.height,(unsigned long long)t->source.L.size,t->source.d.format,t->bc_block,changed_regions,(unsigned long long)refresh_ms);}
    G.profile_decode_ns+=dm_android_nvn_clock_ns()-started;
}
/* Resident slots never move. Both the descriptor registry and shader plans
 * retain slot+incarnation references; deleting/reusing a slot cannot inherit
 * an old image. The serial table is used only to acquire a new reference. */
static uint32_t texture_table_slot(uint64_t serial,int *found) {
    uint64_t hash=serial;hash^=hash>>30;hash*=0xbf58476d1ce4e5b9ULL;hash^=hash>>27;
    uint32_t first=UINT32_MAX,slot=(uint32_t)hash&(2*LIMIT_OBJECTS-1);
    for(unsigned n=0;n<2*LIMIT_OBJECTS;n++,slot=(slot+1)&(2*LIMIT_OBJECTS-1)){
        uint32_t entry=G.texture_table[slot];
        if(!entry){*found=0;return first==UINT32_MAX?slot:first;}
        if(entry==UINT32_MAX){if(first==UINT32_MAX)first=slot;continue;}
        if(G.textures[entry-1].serial==serial){*found=1;return slot;}
    }
    if(first!=UINT32_MAX){*found=0;return first;}
    die("texture registry capacity",serial);return 0;
}
static TextureRef texture_reference(VTexture *t) {
    return (TextureRef){(uint32_t)(t-G.textures),t->incarnation};
}
static VTexture *texture_reference_get(TextureRef ref) {
    if(!ref.incarnation||ref.slot>=G.texture_used)return NULL;
    VTexture *t=&G.textures[ref.slot];return t->serial&&t->incarnation==ref.incarnation?t:NULL;
}
/* NVN 压缩格式 → Vulkan BC 格式（Adreno 原生支持）。与 nvn_bc_unpacked 的解码语义
 * 保持一致：0x41/0x42（含 sRGB 变体）都按带 1-bit alpha 的 BC1 处理。
 * NVN compressed formats -> Vulkan BC formats (Adreno supports natively). Semantics match
 * nvn_bc_unpacked's decode: 0x41/0x42 (and sRGB variants) both behave as BC1 with 1-bit alpha. */
static int nvn_bc_vk_format(uint32_t f,VkFormat *out,uint8_t *block) {
    switch(f) {
    case 0x41: case 0x42: *out=VK_FORMAT_BC1_RGBA_UNORM_BLOCK;*block=8;return 1;
    case 0x45: case 0x46: *out=VK_FORMAT_BC1_RGBA_SRGB_BLOCK;*block=8;return 1;
    case 0x43: *out=VK_FORMAT_BC2_UNORM_BLOCK;*block=16;return 1;
    case 0x47: *out=VK_FORMAT_BC2_SRGB_BLOCK;*block=16;return 1;
    case 0x44: *out=VK_FORMAT_BC3_UNORM_BLOCK;*block=16;return 1;
    case 0x48: *out=VK_FORMAT_BC3_SRGB_BLOCK;*block=16;return 1;
    case 0x49: *out=VK_FORMAT_BC4_UNORM_BLOCK;*block=8;return 1;
    case 0x4a: *out=VK_FORMAT_BC4_SNORM_BLOCK;*block=8;return 1;
    case 0x4b: *out=VK_FORMAT_BC5_UNORM_BLOCK;*block=16;return 1;
    case 0x4c: *out=VK_FORMAT_BC5_SNORM_BLOCK;*block=16;return 1;
    case 0x4d: *out=VK_FORMAT_BC7_UNORM_BLOCK;*block=16;return 1;
    case 0x4e: *out=VK_FORMAT_BC7_SRGB_BLOCK;*block=16;return 1;
    case 0x4f: *out=VK_FORMAT_BC6H_SFLOAT_BLOCK;*block=16;return 1;
    case 0x50: *out=VK_FORMAT_BC6H_UFLOAT_BLOCK;*block=16;return 1;
    default: return 0;
    }
}
static VTexture *texture_acquire(const tex_t *source,int upload,TextureRef ref) {
    if(!source || !source->serial) die("texture serial",0);
    VTexture *t=texture_reference_get(ref);int found=0;uint32_t registry_slot=0;
    if(t&&t->serial!=source->serial)die("texture reference serial",source->serial);
    if(!t){registry_slot=texture_table_slot(source->serial,&found);if(found)t=&G.textures[G.texture_table[registry_slot]-1];}
    if(t) {
        uint32_t decoded_format;int decoded_bpp;
        if(upload&&!t->cpu_owned&&t->cpu_snapshot&&nvn_bc_unpacked(source->d.format,&decoded_format,&decoded_bpp)&&nvn_dirty_since(source->mem,source->L.size,t->upload_stamp)){
            refresh_compressed_cpu(t,source);return t;
        }
        // GPU render targets remain resident. Only CPU-backed sampled images
        // use dirty generations to refresh streamed contents at the same serial.
        if(!upload || !t->cpu_owned || !nvn_dirty_since(source->mem,source->L.size,t->upload_stamp))return t;
        goto upload_existing;
    }
    int one=source->d.target==NVN_TEXTURE_TARGET_1D || source->d.target==NVN_TEXTURE_TARGET_1D_ARRAY;
    int layered=source->d.target==NVN_TEXTURE_TARGET_2D_ARRAY || source->d.target==NVN_TEXTURE_TARGET_1D_ARRAY;
    int volume=source->d.target==NVN_TEXTURE_TARGET_3D;
    int cube=source->d.target==NVN_TEXTURE_TARGET_CUBEMAP || source->d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    if(G.ntexture==LIMIT_OBJECTS || (source->d.target!=NVN_TEXTURE_TARGET_2D && !one && !layered && !cube && !volume) || source->d.samples>1)
        die("texture target/layers/MSAA",source->d.target);
    uint32_t resident_slot=G.texture_free_count?G.texture_free[--G.texture_free_count]:G.texture_used++;
    if(resident_slot>=LIMIT_OBJECTS)die("texture registry slot",resident_slot);
    t=&G.textures[resident_slot];memset(t,0,sizeof(*t));t->serial=source->serial;t->incarnation=++G.texture_incarnation;
    G.ntexture++;G.texture_table[registry_slot]=resident_slot+1;t->source=*source;t->version=1;
    t->width=source->d.width; t->height=one?1:source->d.height;t->depth=volume?source->d.depth:1;t->layout=VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t metal_format=0; int decompressed_bpp=0;
    int bc=nvn_bc_unpacked(source->d.format,&metal_format,&decompressed_bpp);
    t->bc_block=0;
    if(bc&&g_bc_passthrough&&nvn_bc_vk_format(source->d.format,&t->format,&t->bc_block)) {
        t->bpp=decompressed_bpp;   // 仅历史兼容：非 BC 路径才会用 bpp 定尺寸 | compat only: non-BC paths size by bpp
    } else if(bc) {
        switch(metal_format) { case 10:t->format=VK_FORMAT_R8_UNORM;break;case 12:t->format=VK_FORMAT_R8_SNORM;break;
          case 30:t->format=VK_FORMAT_R8G8_UNORM;break;case 32:t->format=VK_FORMAT_R8G8_SNORM;break;
          case 70:t->format=VK_FORMAT_R8G8B8A8_UNORM;break;case 71:t->format=VK_FORMAT_R8G8B8A8_SRGB;break;
          case 115:t->format=VK_FORMAT_R16G16B16A16_SFLOAT;break;default:die("BC decoded format",metal_format); }
        t->bpp=decompressed_bpp;
    } else { t->format=format(source->d.format); t->bpp=source->L.bpb; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(G.physical,t->format,&fp);
    VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(aspect(t->format)&VK_IMAGE_ASPECT_DEPTH_BIT) usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int attachment_image=(usage&(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))!=0;
    if((fp.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)&&!(G.diag_img_usage>=1&&attachment_image)) usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    if(G.feedback_layout && attachment_image && G.diag_img_usage<2)usage|=VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkImageCreateInfo ii={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=volume?VK_IMAGE_TYPE_3D:one?VK_IMAGE_TYPE_1D:VK_IMAGE_TYPE_2D,.format=t->format,
      .flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
      .extent={t->width,t->height,t->depth},.mipLevels=source->d.levels,.arrayLayers=source->L.layers,.samples=VK_SAMPLE_COUNT_1_BIT,
      .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateImage(G.device,&ii,NULL,&t->image)); VkMemoryRequirements r; vkGetImageMemoryRequirements(G.device,t->image,&r);
    VkMemoryAllocateInfo a={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,
      .memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(G.device,&a,NULL,&t->memory)); CHECK(vkBindImageMemory(G.device,t->image,t->memory,0));
    VkImageViewType view_type=volume?VK_IMAGE_VIEW_TYPE_3D:one?(layered?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D):cube?(source->L.layers==6?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY):layered?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,.viewType=view_type,
       .format=t->format,.subresourceRange={aspect(t->format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,source->d.levels,0,source->L.layers}}; CHECK(vkCreateImageView(G.device,&vi,NULL,&t->view));
upload_existing:
    if(upload) {
        uint32_t decoded_format=0;int decoded_bpp=0;
        int upload_bc=nvn_bc_unpacked(source->d.format,&decoded_format,&decoded_bpp);
        if(!source->mem || !pool_find((uint64_t)(uintptr_t)source->mem,source->L.size,NULL)) die("packaged texture storage",source->serial);
        // Protect before reading and stamp before protection: any later CPU
        // write is newer than this snapshot, even if it races the upload.
        uint64_t stamp=nvn_dirty_now();nvn_dirty_protect(source->mem,source->L.size);
        const uint8_t *upload_memory=source->mem;
        if(upload_bc){uint8_t *snapshot=malloc(source->L.size);if(!snapshot)die("BC CPU baseline snapshot allocation",source->L.size);
            memcpy(snapshot,source->mem,source->L.size);free(t->cpu_snapshot);t->cpu_snapshot=snapshot;upload_memory=snapshot;}
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        uint64_t decode_start=dm_android_nvn_clock_ns();
        /* 先把所有（层，级）的字节尺寸算好，合成一个暂存缓冲与一次拷贝命令批次：
           多层图集（例如 252 层 × 7 级）因此从上千次暂存分配+拷贝命令降到个位数。
           | Size every (layer, level) upload first, stage them into one buffer and issue
           a single copy-command batch: multi-layer atlases (e.g. 252 layers x 7 levels)
           drop from ~1e3 staging allocations and copy calls to a handful. */
        {
        VkImageAspectFlags upload_aspect=aspect(t->format);
        if(upload_aspect&VK_IMAGE_ASPECT_STENCIL_BIT)die("packaged depth/stencil upload requires separate aspect conversion",source->d.format);
        size_t total=0,scratch_max=0;unsigned nregions=0;
        for(unsigned layer=0;layer<source->L.layers;layer++)for(unsigned level=0;level<source->d.levels;level++) {
            uint32_t w=t->width>>level,h=t->height>>level,z=t->depth>>level;if(!w)w=1;if(!h)h=1;if(!z)z=1;
            int bw=1,bh=1;int bb=nvn_fmt_bpb(source->d.format,&bw,&bh);if(!bb)die("texture block size",source->d.format);
            uint64_t row=(uint64_t)((w+bw-1)/bw)*bb,rows=(h+bh-1)/bh;
            total+=t->bc_block?(size_t)(row*rows*z):(size_t)w*h*z*t->bpp;
            if(row*rows*z>scratch_max)scratch_max=(size_t)(row*rows*z);
            nregions++;
        }
        VBuffer *staging=temporary((uint64_t)total,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        uint8_t *scratch=malloc(scratch_max?scratch_max:1);if(!scratch)die("texture upload scratch allocation",scratch_max);
        VkBufferImageCopy *regions=calloc(nregions,sizeof(*regions));if(!regions)die("texture upload copy regions",nregions);
        size_t staged=0;unsigned r=0;
        for(unsigned layer=0;layer<source->L.layers;layer++)for(unsigned level=0;level<source->d.levels;level++) {
            uint32_t w=t->width>>level,h=t->height>>level,z=t->depth>>level;if(!w)w=1;if(!h)h=1;if(!z)z=1;
            int bw=1,bh=1;int bb=nvn_fmt_bpb(source->d.format,&bw,&bh);if(!bb)die("texture block size",source->d.format);
            uint64_t row=(uint64_t)((w+bw-1)/bw)*bb,rows=(h+bh-1)/bh;
            if(source->d.flags&NVN_TEXTURE_FLAGS_LINEAR) { free(scratch);free(regions); die("linear texture upload layout",source->d.flags); }
            if(nvn_deswizzle(&source->d,level,layer,upload_memory,scratch,row)){free(scratch);free(regions);die("Tegra texture deswizzle",level);}
            if(t->bc_block) {
                size_t compressed=(size_t)row*rows*z;
                memcpy(staging->map+staged,scratch,compressed);
                regions[r++]=(VkBufferImageCopy){.bufferOffset=staged,.imageSubresource={upload_aspect,level,layer,1},.imageExtent={w,h,z}};
                staged+=compressed;
            } else {
                if(upload_bc) {for(unsigned slice=0;slice<z;++slice)if(nvn_bc_decode(source->d.format,scratch+slice*row*rows,row,w,h,staging->map+staged+(size_t)slice*w*h*t->bpp,(size_t)w*t->bpp)){free(scratch);free(regions);die("BC decode",source->d.format);} }
                else memcpy(staging->map+staged,scratch,(size_t)w*h*z*t->bpp);
                regions[r++]=(VkBufferImageCopy){.bufferOffset=staged,.imageSubresource={upload_aspect,level,layer,1},.imageExtent={w,h,z}};
                staged+=(size_t)w*h*z*t->bpp;
            }
        }
        free(scratch);
        vkCmdCopyBufferToImage(S.command,staging->b,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,r,regions);
        free(regions);
        }
        G.profile_decode_ns+=dm_android_nvn_clock_ns()-decode_start;
        {uint64_t decode_ms=(dm_android_nvn_clock_ns()-decode_start)/1000000ull;
         if(decode_ms>=20) fprintf(stderr,"[nvn-vulkan] TEXTURE_UPLOAD_SLOW serial=%llu %ux%u levels=%u layers=%u bytes=%llu fmt=%#x bc=%u ms=%llu\n",
             (unsigned long long)source->serial,source->d.width,source->d.height,source->d.levels,source->L.layers,(unsigned long long)source->L.size,source->d.format,t->bc_block,(unsigned long long)decode_ms);}
        t->cpu_owned=1;t->upload_stamp=stamp;tex_dirty(t); G.uploads++;
    }
    return t;
}
static VTexture *texture_get(const tex_t *source,int upload) {
    return texture_acquire(source,upload,(TextureRef){0});
}
static VTexture *slot_texture(TSlot *slot,int upload) {
    VTexture *t=texture_acquire(&slot->texture,upload,slot->resident);slot->resident=texture_reference(t);return t;
}
void mtl_tex_release(tex_t *source) {
    pthread_mutex_lock(&G.lock);wait_submission_idle_locked();int found=0;uint32_t slot=texture_table_slot(source->serial,&found);
    if(found){VTexture *t=&G.textures[G.texture_table[slot]-1];
       free(t->cpu_snapshot);
       if(t->shadow){vkDestroyImageView(G.device,t->shadow_view,NULL);vkDestroyImage(G.device,t->shadow,NULL);vkFreeMemory(G.device,t->shadow_memory,NULL);}
       if(t->sample_copy){vkDestroyImageView(G.device,t->sample_view,NULL);vkDestroyImage(G.device,t->sample_copy,NULL);vkFreeMemory(G.device,t->sample_memory,NULL);}
       vkDestroyImageView(G.device,t->view,NULL); vkDestroyImage(G.device,t->image,NULL); vkFreeMemory(G.device,t->memory,NULL);
       /* 纹理图像销毁：持久帧缓冲/附件视图的代际失效点 | image death invalidates persistent fb/view caches */
       G.rt_gen++;
       G.texture_free[G.texture_free_count++]=(uint32_t)(t-G.textures);t->serial=0;G.ntexture--;G.texture_table[slot]=UINT32_MAX;G.registry_revision++;
    }
    pthread_mutex_unlock(&G.lock);
}
void mtl_reg_tex(int id,const tex_t *t,const view_t *v) {
    mtl_init();pthread_mutex_lock(&G.lock);
    if(id<0 || id>=G.texture_pool_n || (size_t)id>=G.texture_slots)die("texture descriptor id",id); G.tslot[id].live=t!=NULL;
    if(t) G.tslot[id].texture=*t; G.tslot[id].has_view=v!=NULL; if(v)G.tslot[id].view=*v;
    G.tslot[id].resident=(TextureRef){0};G.tslot[id].revision=++G.registry_revision;pthread_mutex_unlock(&G.lock);
}
static VkSamplerAddressMode wrap(unsigned w) { switch(w) {
    case NVN_WRAP_MODE_REPEAT:return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_EDGE:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;default:die("sampler wrap mode",w); } }
void mtl_reg_samp(int id,const samp_par *p) {
    mtl_init();pthread_mutex_lock(&G.lock); if(id<0 || id>=LIMIT_OBJECTS || !p)die("sampler descriptor id",id); SSlot *s=&G.sslot[id];
    async_drain_locked();if(s->live) { wait_submission_idle_locked(); vkDestroySampler(G.device,s->sampler,NULL); }
    s->source=*p;
    VkSamplerCreateInfo i={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
       .magFilter=p->magf==NVN_MAG_FILTER_NEAREST?VK_FILTER_NEAREST:VK_FILTER_LINEAR,
       .minFilter=(p->minf==NVN_MIN_FILTER_NEAREST || p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST || p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR)?VK_FILTER_NEAREST:VK_FILTER_LINEAR,
       .mipmapMode=(p->minf==NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf==NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR)?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST,
       .addressModeU=wrap(p->ws),.addressModeV=wrap(p->wt),.addressModeW=wrap(p->wr),.minLod=p->lod_min,.maxLod=p->lod_max,
       .mipLodBias=p->lod_bias,.anisotropyEnable=p->aniso>1,.maxAnisotropy=p->aniso>1?p->aniso:1,
       .compareEnable=p->cmp_mode==NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE,.compareOp=(VkCompareOp)(p->cmp_func-1),
       .borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    if(p->border[0]==0 && p->border[1]==0 && p->border[2]==0 && p->border[3]==1)
        i.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if(p->border[0]==1 && p->border[1]==1 && p->border[2]==1 && p->border[3]==1)
        i.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if(p->border[0] || p->border[1] || p->border[2] || p->border[3]) {
        fprintf(stderr,"[nvn-vulkan] unsupported sampler border rgba=%g,%g,%g,%g wrap=%u,%u,%u\n",
            p->border[0],p->border[1],p->border[2],p->border[3],p->ws,p->wt,p->wr);
        die("custom sampler border color",id);
    }
    CHECK(vkCreateSampler(G.device,&i,NULL,&s->sampler)); s->live=1;G.registry_revision++;pthread_mutex_unlock(&G.lock);
}
void mtl_set_tex_pool_n(int n) {
    mtl_init();pthread_mutex_lock(&G.lock);
    /* NVN handles encode a 20-bit texture descriptor ID. Descriptor slots are
     * CPU metadata, independent of the number of resident Vulkan images. */
    if(n<0 || n>(1<<20))die("texture pool capacity",n);
    if((size_t)n>G.texture_slots) {
        TSlot *slots=realloc(G.tslot,(size_t)n*sizeof(*slots));
        if(!slots)die("texture descriptor allocation",n);
        memset(slots+G.texture_slots,0,((size_t)n-G.texture_slots)*sizeof(*slots));
        G.tslot=slots;G.texture_slots=(size_t)n;
    }
    G.texture_pool_n=n;G.registry_revision++;
    fprintf(stderr,"[nvn-vulkan] texture descriptor slots=%d resident image limit=%d\n",n,LIMIT_OBJECTS);
    pthread_mutex_unlock(&G.lock);
}

static uint8_t *read_file(const char *name,size_t *size) {
    char path[1200]; if(!G.shader_root[0] || snprintf(path,sizeof(path),"%s/%s",G.shader_root,name)>=(int)sizeof(path))die("shader cache path",0);
    FILE *f=fopen(path,"rb"); if(!f)die(path,0); if(fseek(f,0,SEEK_END))die("shader cache seek",0);
    long n=ftell(f); if(n<0 || n>64*1024*1024 || fseek(f,0,SEEK_SET))die("shader cache file size",n);
    uint8_t *bytes=malloc(n?n:1); if(!bytes || fread(bytes,1,n,f)!=(size_t)n)die("shader cache read",n);
    fclose(f);*size=n;return bytes;
}
static uint8_t *buffer_image_module(uint8_t *bytes,size_t *size,VShader *shader) {
    uint32_t *w=(uint32_t *)bytes;unsigned bound=w[3];if(bound>1000000)die("buffer image id bound",bound);
    uint32_t *definition=calloc(bound,4),*variable=calloc(bound,4),*loaded=calloc(bound,4),*pointers=calloc(bound,4);
    if(!definition||!variable||!loaded||!pointers)die("buffer image audit allocation",bound);
    for(size_t o=5;o<*size/4;) {unsigned n=w[o]>>16,op=w[o]&65535;if(!n||n>*size/4-o)die("buffer image instruction",o);
        if((op==25||op==27||op==32)&&w[o+1]<bound)definition[w[o+1]]=o;o+=n;}
    unsigned next=bound,count=0;
    for(size_t o=5;o<*size/4;o+=w[o]>>16)if((w[o]&65535)==59&&w[o+1]<bound) {
        uint32_t p=definition[w[o+1]];
        if(!p||(w[p]&65535)!=32||w[p+2]!=0)continue;
        uint32_t s=w[p+3]<bound?definition[w[p+3]]:0;
        if(!s||(w[s]&65535)!=27)continue;
        uint32_t im=w[s+2]<bound?definition[w[s+2]]:0;
        if(!im||(w[im]&65535)!=25||w[im+3]!=5)continue;
        unsigned id=w[o+2],image=w[im+1];if(id>=bound)die("buffer image variable id",id);
        variable[id]=image;if(!pointers[image])pointers[image]=next++;
        int binding=-1;
        for(size_t d=5;d<*size/4;d+=w[d]>>16)if((w[d]&65535)==71&&w[d]>>16==4&&w[d+1]==id&&w[d+2]==33)binding=w[d+3];
        int found=0;for(unsigned i=0;i<shader->h.nb;i++)if((int)shader->b[i].binding==binding){if(shader->b[i].kind!=4||shader->b[i].type!=1)die("buffer image metadata",binding);shader->b[i].kind=10;shader->b[i].type=4;found=1;}
        if(!found)die("buffer image descriptor binding",binding);count++;
    }
    if(!count){free(definition);free(variable);free(loaded);free(pointers);return bytes;}
    uint32_t *out=malloc(*size+4*(next-bound)*4);if(!out)die("buffer image module allocation",*size);memcpy(out,w,20);out[3]=next;size_t at=5;unsigned extracts=0;
    for(size_t o=5;o<*size/4;o+=w[o]>>16) {
        unsigned n=w[o]>>16,op=w[o]&65535;memcpy(out+at,w+o,n*4);
        if(op==25&&w[o+1]<bound&&pointers[w[o+1]]){at+=n;uint32_t ptr[]={4u<<16|32,pointers[w[o+1]],0,w[o+1]};memcpy(out+at,ptr,16);at+=4;continue;}
        if(op==59&&w[o+2]<bound&&variable[w[o+2]])out[at+1]=pointers[variable[w[o+2]]];
        if(op==61&&w[o+3]<bound&&variable[w[o+3]]){out[at+1]=variable[w[o+3]];if(w[o+2]>=bound)die("buffer image load id",w[o+2]);loaded[w[o+2]]=out[at+1];}
        if(op==100&&w[o+3]<bound&&loaded[w[o+3]]){if(n!=4||w[o+1]!=loaded[w[o+3]])die("buffer image extraction",w[o+2]);out[at]=4u<<16|83;extracts++;}
        if(((op>=87&&op<=94)||op==96||op==97||op==105)&&n>3&&w[o+3]<bound&&loaded[w[o+3]])die("buffer image cannot use sampler",op);
        at+=n;
    }
    if(!extracts)die("buffer image had no image extraction",count);
    fprintf(stderr,"[nvn-vulkan] samplerless buffer adaptation shader=%s descriptors=%u extractions=%u\n",shader->key,count,extracts);
    free(definition);free(variable);free(loaded);free(pointers);free(bytes);*size=at*4;return (uint8_t *)out;
}
static uint64_t control_hash(const void *control,uint32_t stage) {
    const uint8_t *bytes=control;uint64_t hash=14695981039346656037ULL^stage;
    for(unsigned i=0;i<0x880;i++){hash^=bytes[i];hash*=1099511628211ULL;}return hash;
}
static BindingPlan *binding_plan_compile(VShader *s) {
    size_t bytes=sizeof(BindingPlan)+(size_t)s->h.nb*sizeof(BindingResource);
    BindingPlan *plan=calloc(1,bytes);if(!plan)die("shader binding plan allocation",bytes);
    plan->eligible=1;
    for(uint32_t i=0;i<s->h.nb;i++){
        const RBinding *b=&s->b[i];
        if(b->kind==2||b->kind==3||b->kind==9||b->kind==10)plan->buffers[plan->nbuffer++]=(uint8_t)i;
        else if(b->kind==4||b->kind==6)plan->images[plan->nimage++]=(uint8_t)i;
        else if(b->kind==8)plan->drivers[plan->ndriver++]=(uint8_t)i;
        if(b->kind==1||b->kind==7)continue;
        if(b->kind==2&&b->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&b->slot>=0&&b->slot<16)continue;
        if(b->kind==4&&(b->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||b->type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)&&b->slot>=0&&b->slot<128&&!(s->dref_bindings&(1ull<<i)))continue;
        plan->eligible=0;
    }
    return plan;
}
static const uint8_t *shader_entry(const void *control,uint32_t stage) {
    if(!control)die("null shader control",stage);
    if(!G.shader_index) { G.shader_index=read_file("shader_index.bin",&G.index_bytes);
        if(G.index_bytes<12)die("shader index header",G.index_bytes); uint32_t *h=(uint32_t *)G.shader_index;
        if(h[0]!=0x494b5644 || h[1]!=1 || G.index_bytes!=12+(size_t)h[2]*(20+0x880))die("shader index schema",h[0]); }
    if(!G.shader_entries){
        uint32_t count=((uint32_t *)G.shader_index)[2];if(count>1000000)die("shader control index count",count);
        size_t capacity=16;while(capacity<(size_t)count*2)capacity*=2;
        G.shader_entries=calloc(capacity,sizeof(*G.shader_entries));if(!G.shader_entries)die("shader control index allocation",capacity);
        G.shader_entry_capacity=capacity;const uint8_t *entry=G.shader_index+12;
        for(uint32_t i=0;i<count;i++,entry+=20+0x880){uint32_t st;memcpy(&st,entry+16,4);uint64_t hash=control_hash(entry+20,st);size_t slot=hash&(capacity-1);
            while(G.shader_entries[slot].entry){const uint8_t *old=G.shader_entries[slot].entry;
                if(G.shader_entries[slot].hash==hash&&!memcmp(old+16,entry+16,4+0x880))break;
                slot=(slot+1)&(capacity-1);}
            /* Preserve the first matching verified entry, including duplicates. */
            if(!G.shader_entries[slot].entry)G.shader_entries[slot]=(ShaderEntry){entry,hash};
        }
        fprintf(stderr,"[nvn-vulkan] SHADER_CONTROL_INDEX entries=%u slots=%zu bytes=%zu full_control_verified=1\n",count,capacity,capacity*sizeof(*G.shader_entries));
    }
    uint64_t hash=control_hash(control,stage);size_t slot=hash&(G.shader_entry_capacity-1);
    while(G.shader_entries[slot].entry){ShaderEntry *candidate=&G.shader_entries[slot];uint32_t st;memcpy(&st,candidate->entry+16,4);
        if(candidate->hash==hash&&st==stage&&!memcmp(candidate->entry+20,control,0x880))return candidate->entry;
        slot=(slot+1)&(G.shader_entry_capacity-1);}
    return NULL;
}
static VShader *shader_get(const void *control,uint32_t stage) {
    const uint8_t *entry=shader_entry(control,stage);if(!entry)die("original shader not in verified Vulkan cache",stage);
    char key[17]={0};memcpy(key,entry,16);
    for(int i=0;i<G.nshader;i++)if(!strcmp(G.shaders[i].key,key))return &G.shaders[i];
    if(G.nshader==LIMIT_OBJECTS)die("shader object limit",G.nshader); VShader *s=&G.shaders[G.nshader++]; strcpy(s->key,key);
    char name[40];snprintf(name,sizeof(name),"%s.vkmeta",key);size_t size=0;uint8_t *bytes=read_file(name,&size);
    if(size<sizeof(RHeader))die("shader metadata header",size);memcpy(&s->h,bytes,sizeof(RHeader));
    if(s->h.magic!=0x4d4b5644 || s->h.version!=1 || s->h.stage!=stage || s->h.nb>LIMIT_BINDINGS || s->h.na>32 || s->h.ncap>32 || s->h.c1n>4096)
       die("shader metadata bounds",stage);
    size_t expected=sizeof(RHeader)+s->h.nb*sizeof(RBinding)+s->h.na*8+s->h.ncap*4+s->h.c1n;
    if(size!=expected)die("shader metadata size",size);uint8_t *p=bytes+sizeof(RHeader);
    memcpy(s->b,p,s->h.nb*sizeof(RBinding));p+=s->h.nb*sizeof(RBinding);memcpy(s->attr,p,s->h.na*8);p+=s->h.na*8;
    memcpy(s->caps,p,s->h.ncap*4);p+=s->h.ncap*4;
    for(uint32_t i=0;i<s->h.ncap;i++) {
       unsigned cap=s->caps[i];VkSubgroupFeatureFlags feature=0;
       if(cap>=61&&cap<=68){const VkSubgroupFeatureFlags features[]={VK_SUBGROUP_FEATURE_BASIC_BIT,VK_SUBGROUP_FEATURE_VOTE_BIT,VK_SUBGROUP_FEATURE_ARITHMETIC_BIT,VK_SUBGROUP_FEATURE_BALLOT_BIT,VK_SUBGROUP_FEATURE_SHUFFLE_BIT,VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT,VK_SUBGROUP_FEATURE_CLUSTERED_BIT,VK_SUBGROUP_FEATURE_QUAD_BIT};feature=features[cap-61];}
       if(feature && (G.subgroup.supportedStages&(stage==5?VK_SHADER_STAGE_COMPUTE_BIT:stage==1?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT)) && (G.subgroup.supportedOperations&feature))continue;
       if(cap==51 && stage==1)continue; /* DerivativeControl is core fragment functionality. */
       if(cap==4427 && stage==0 && G.shader_draw_parameters)continue; /* BaseVertex/BaseInstance/DrawIndex use actual Vulkan draw arguments. */
       if(cap!=1 && cap!=46 && cap!=47 && cap!=50 && cap!=55 && cap!=56){fprintf(stderr,"[nvn-vulkan] shader feature key=%s stage=%u cap=%u\n",key,stage,cap);die("shader feature needs device/semantic adaptation",cap);}
    }
    if(s->h.c1n){s->c1=buffer_make(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);memcpy(s->c1.map,p,s->h.c1n);}free(bytes);
    if(s->h.reserved) {
        /* Metadata records the exact uncompressed size. zlib validates gzip
         * CRC32 and ISIZE; reject trailing/concatenated streams and short data.
         * The IR is exactly the offline-validated original SPIR-V, unchanged.
         */
        uint32_t expected_size=s->h.reserved;
        if(expected_size<20 || expected_size%4 || expected_size>16*1024*1024)die("compressed SPIR-V declared length",expected_size);
        snprintf(name,sizeof(name),"%s.spv.gz",key);uint8_t *compressed=read_file(name,&size);
        if(size<18 || size>UINT32_MAX || compressed[0]!=0x1f || compressed[1]!=0x8b || compressed[2]!=8)die("shader gzip header",size);
        bytes=malloc(expected_size);if(!bytes)die("SPIR-V inflate allocation",expected_size);
        z_stream stream={.next_in=compressed,.avail_in=(uInt)size,.next_out=bytes,.avail_out=expected_size};
        int initialized=inflateInit2(&stream,15+16);if(initialized!=Z_OK)die("shader gzip initialize",initialized);
        int result=inflate(&stream,Z_FINISH);int complete=result==Z_STREAM_END && stream.total_out==expected_size && stream.avail_in==0;
        int ended=inflateEnd(&stream);free(compressed);
        if(!complete || ended!=Z_OK)die("shader gzip CRC/length/trailing-data validation",result);
        size=expected_size;
    } else {snprintf(name,sizeof(name),"%s.spv",key);bytes=read_file(name,&size);}
    if(size%4 || size<20 || ((uint32_t *)bytes)[0]!=0x07230203)die("SPIR-V module",size);
    bytes=buffer_image_module(bytes,&size,s);
    uint32_t *words=(uint32_t *)bytes;int fetch=0,sampling=0;
    for(size_t offset=5;offset<size/4;) {
        unsigned length=words[offset]>>16,op=words[offset]&0xffff;
        if(!length || length>size/4-offset)die("SPIR-V instruction bounds",offset);
        if(op==95 || op==313)fetch=1; /* OpImageFetch / OpImageSparseFetch */
        if((op>=87&&op<=94) || op==96 || op==97 || op==105 || (op>=305&&op<=312) || op==314 || op==315)sampling=1;
        offset+=length;
    }
    (void)fetch;(void)sampling;
    unsigned resource_bound=words[3];if(resource_bound>1000000)die("SPIR-V resource id bound",resource_bound);
    uint64_t *resource=calloc(resource_bound,sizeof(*resource));if(!resource)die("SPIR-V resource audit allocation",resource_bound);
    for(size_t o=5;o<size/4;o+=words[o]>>16)if((words[o]&65535)==71&&words[o]>>16==4&&words[o+2]==33&&words[o+1]<resource_bound)
        for(unsigned b=0;b<s->h.nb;b++)if(s->b[b].binding==words[o+3])resource[words[o+1]]=1ull<<b;
    uint64_t extracted=0,used_sampler=0;
    for(size_t o=5;o<size/4;o+=words[o]>>16){unsigned op=words[o]&65535,n=words[o]>>16;
        if((op==61||op==83||op==100)&&n>=4&&words[o+2]<resource_bound&&words[o+3]<resource_bound){resource[words[o+2]]=resource[words[o+3]];if(op==100)extracted|=resource[words[o+3]];}
        if(op==86&&n>=5&&words[o+2]<resource_bound&&words[o+3]<resource_bound)resource[words[o+2]]=resource[words[o+3]];
        if((op==89||op==90||op==93||op==94||op==97||op==307||op==308||op==311||op==312||op==315)&&n>=4&&words[o+3]<resource_bound)s->dref_bindings|=resource[words[o+3]];
        if(((op>=87&&op<=94)||op==96||op==97||op==105||(op>=305&&op<=312)||op==314||op==315)&&n>=4&&words[o+3]<resource_bound)used_sampler|=resource[words[o+3]];
    }
    s->fetch_bindings=extracted&~used_sampler;free(resource);
    /* 逐 draw 预筛掩码：加载期算一次，热路径只遍历相关绑定。 | Per-draw prefilter masks: computed once at load, hot paths iterate only the relevant bindings. */
    s->image_bindings=s->sampled_bindings=s->side_effect_bindings=0;
    for(uint32_t i=0;i<s->h.nb;i++){
        const RBinding *b=&s->b[i];
        if(b->kind==4||b->kind==6)s->image_bindings|=1ull<<i;
        if(b->kind==4)s->sampled_bindings|=1ull<<i;
        if(((b->kind==3||b->kind==9)&&b->flags)||b->kind==6||b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)s->side_effect_bindings|=1ull<<i;
    }
    /* DRIVER resources are the original binding tables, not zero constants.
     * Verify every direct access is a statically known table field before
     * constructing them from live NVN bindings. Reject unknown/dynamic reads.
     */
    unsigned bound=words[3];if(bound>1000000)die("SPIR-V id bound",bound);
    uint32_t *constants=calloc(bound,sizeof(*constants));uint8_t *known=calloc(bound,1);
    if(!constants || !known)die("SPIR-V driver audit allocation",bound);
    for(size_t o=5;o<size/4;o+=words[o]>>16)if((words[o]&0xffff)==43 && words[o]>>16==4 && words[o+2]<bound){constants[words[o+2]]=words[o+3];known[words[o+2]]=1;}
    for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==8) {
        RBinding *b=&s->b[i];uint32_t variable=0;
        if(b->slot!=0 && b->slot!=2)die("driver constant bank",b->slot);
        for(size_t o=5;o<size/4;o+=words[o]>>16)if((words[o]&0xffff)==71 && words[o]>>16==4 && words[o+2]==33 && words[o+3]==b->binding)variable=words[o+1];
        if(!variable)die("driver constant variable",b->binding);
        for(size_t o=5;o<size/4;o+=words[o]>>16)if(((words[o]&0xffff)==65 || (words[o]&0xffff)==66) && words[o+3]==variable) {
            if(words[o]>>16!=7)die("driver access shape",b->binding);
            uint32_t a=words[o+4],v=words[o+5],c=words[o+6];
            if(a>=bound||v>=bound||c>=bound||!known[a]||!known[v]||!known[c]||constants[a]||constants[c]>3)die("dynamic driver constant access",b->binding);
            uint64_t offset=(uint64_t)constants[v]*16+constants[c]*4;int valid=0;
            if(b->slot==0){unsigned base=stage==5?0x310:stage==1?0x510:0x110;
                valid=offset>=base && offset<base+256 && (offset-base)%16<12;
                if(stage==5 && offset>=0x210 && offset<0x310 && (offset-0x210)%16<12)valid=1;
            }else valid=(offset>=0x120&&offset<0x168&&(offset-0x120)%8==0)||(offset>=0x168&&offset<0x568&&(offset-0x168)%8==0)||(offset>=0x568&&offset<0x968&&(offset-0x568)%8==0);
            if(!valid)die("driver constant access outside verified binding tables",offset);
        }
    }
    free(constants);free(known);
    if(!getenv("DM_DISABLE_STORAGE_BIAS")){
        SBResource resources[LIMIT_BINDINGS];
        for(unsigned i=0;i<s->h.nb;i++)resources[i]=(SBResource){s->b[i].binding,(s->b[i].kind==3||s->b[i].kind==9)&&!s->b[i].flags&&s->b[i].type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        bytes=dm_storage_bias_apply(bytes,&size,resources,s->h.nb,&s->storage_bias);if(!bytes)die("storage index compensation module bounds",0);
        if(s->storage_bias.mask)fprintf(stderr,"[nvn-vulkan] STORAGE_BIAS_MODULE shader=%s mask=%#llx bytes=%zu\n",s->key,(unsigned long long)s->storage_bias.mask,size);
    }
    const char *dump=getenv("DM_ADAPTED_SHADER_DIR");
    if(dump&&s->storage_bias.mask){char path[1200];if(snprintf(path,sizeof(path),"%s/%s.spv",dump,s->key)>=(int)sizeof path)die("adapted shader dump path",0);
        FILE *out=fopen(path,"wb");if(!out||fwrite(bytes,1,size,out)!=size)die("adapted shader dump",size);fclose(out);}
    VkShaderModuleCreateInfo mi={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=(uint32_t *)bytes};
    CHECK(vkCreateShaderModule(G.device,&mi,NULL,&s->module));free(bytes);
    fprintf(stderr,"[nvn-vulkan] original shader key=%s stage=%u loaded\n",s->key,stage);
    /* kind2 统一缓冲的偏移逐 draw 变化：改成 DYNAMIC 描述符，让描述符内容（池缓冲 + 整段）
     * 跨 draw 保持不变，偏移走绑定调用。动态偏移按 set 布局的绑定顺序消费，因此只在绑定号
     * 升序时启用。 | kind-2 uniform offsets change per draw: switch them to DYNAMIC descriptors so
     * the descriptor content (pool buffer + whole range) stays put across draws and the offset
     * travels in the bind call. Dynamic offsets are consumed in the set layout's binding order, so
     * only enable when the binding numbers are ascending. */
    s->dyn_bindings=0;s->dyn_count=0;{
        int ordered=1;uint32_t prev=0;
        for(uint32_t i=0;i<s->h.nb;i++){if(i&&s->b[i].binding<prev)ordered=0;prev=s->b[i].binding;}
        for(uint32_t i=0;ordered&&i<s->h.nb;i++)
            if(s->b[i].kind==2&&s->b[i].type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&s->b[i].slot>=0&&s->b[i].slot<16){
                s->dyn_bindings|=1ull<<i;s->dyn_count++; }
    }
    VkDescriptorSetLayoutBinding bindings[LIMIT_BINDINGS];for(uint32_t i=0;i<s->h.nb;i++){
       if(s->b[i].count!=1 || s->b[i].set!=(stage==1?1u:0u))die("shader descriptor set/count",s->b[i].set);
       VkDescriptorType type=(VkDescriptorType)s->b[i].type;
       if((s->dyn_bindings>>i)&1)type=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
       bindings[i]=(VkDescriptorSetLayoutBinding){s->b[i].binding,type,1,stage==0?VK_SHADER_STAGE_VERTEX_BIT:stage==1?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_COMPUTE_BIT,NULL}; }
    VkDescriptorSetLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=s->h.nb,.pBindings=bindings};
    CHECK(vkCreateDescriptorSetLayout(G.device,&li,NULL,&s->layout));s->plan=binding_plan_compile(s);return s;
}
void mtl_prog_release(prog_t *p) { (void)p; /* Programs own only immutable shared shader-cache entries; no per-program GPU allocation. */ }
static void aligned_writeback(void) {
    for(int i=0;i<S.naligned;i++)if(S.aligned[i].written){AlignedBinding *b=&S.aligned[i];uint64_t offset=0;
        VPool *p=pool_find(b->range.addr,b->range.size,&offset);if(!p)die("aligned binding pool lifetime",b->range.addr);
        all_barrier();VkBufferCopy copy={0,offset,b->range.size};vkCmdCopyBuffer(S.command,b->buffer->b,p->v.b,1,&copy);
    }
    S.naligned=0;
}
static int depth_read_only(void) {
    const ds_st *d=&G.state.ds;if(d->dw)return 0;
    if(!d->st || (!G.state.stencil_write[0]&&!G.state.stencil_write[1]))return 1;
    return d->fail_f==NVN_STENCIL_OP_KEEP&&d->zfail_f==NVN_STENCIL_OP_KEEP&&d->zpass_f==NVN_STENCIL_OP_KEEP&&d->fail_b==NVN_STENCIL_OP_KEEP&&d->zfail_b==NVN_STENCIL_OP_KEEP&&d->zpass_b==NVN_STENCIL_OP_KEEP;
}
static void collect_image_uses(VShader *s) {
    if(!s)return;
    for(unsigned i=0;i<s->h.nb;i++) {RBinding *b=&s->b[i];if(b->kind!=4&&b->kind!=6)continue;
        if(b->slot<0 || b->slot>=(b->kind==6?32:128))die("image-use shader slot",b->slot);
        uint64_t handle=b->kind==6?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot];unsigned tid=handle&0xfffff;
        if(tid>=G.texture_slots||!G.tslot[tid].live)die("image-use texture descriptor",tid);
        uint64_t serial=G.tslot[tid].texture.serial;unsigned j=0;while(j<S.nimage_uses&&S.image_uses[j].serial!=serial)j++;
        if(j==S.nimage_uses){if(j==2*LIMIT_BINDINGS)die("image-use capacity",j);S.image_uses[S.nimage_uses++]=(ImageUse){serial,0};}
        S.image_uses[j].roles|=b->kind==6?2u:1u;
    }
}
static int storage_image_use(uint64_t serial) {
    for(unsigned i=0;i<S.nimage_uses;i++)if(S.image_uses[i].serial==serial)return (S.image_uses[i].roles&2)!=0;
    return 0;
}
/* Disjoint mip/layer reads are legal NVN rendering, not attachment feedback.
 * The current backend tracks layouts for an entire image. On drivers without
 * feedback layouts, isolate those sampled subresources with a bit-exact GPU
 * copy so layout transitions cannot invalidate the attachment or descriptor. */
static int disjoint_sampling(VTexture *t,TSlot *slot,unsigned stage) {
    if(G.feedback_layout || stage==5 || aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)return 0;
    view_t *v=&slot->view;
    unsigned level=slot->has_view&&v->set_levels?v->base_level:0;
    unsigned levels=slot->has_view&&v->set_levels?v->nlevels:t->source.d.levels;
    unsigned layer=slot->has_view&&v->set_layers?v->min_layer:0;
    unsigned layers=slot->has_view&&v->set_layers?v->nlayers:t->source.L.layers;
    if(level>=t->source.d.levels || !levels || levels>t->source.d.levels-level ||
       layer>=t->source.L.layers || !layers || layers>t->source.L.layers-layer)die("sample copy view bounds",t->serial);
    int found=0;
    for(int i=0;i<G.state.rt.n;i++)if(G.state.rt.color[i]&&G.state.rt.color[i]->serial==t->serial){
        view_t *a=&G.state.rt.cview[i];unsigned al=G.state.rt.has_cview[i]&&a->set_levels?a->base_level:0;
        unsigned ar=G.state.rt.has_cview[i]&&a->set_layers?a->min_layer:0;
        if(al>=level && al-level<levels && ar>=layer && ar-layer<layers)return 0;
        found=1;
    }
    if(found && storage_image_use(t->serial))die("disjoint sample copy with storage writes",t->serial);
    return found;
}
static void prepare_sampling_copies(VShader *s) {
    if(!s || G.feedback_layout || s->h.stage==5)return;
    for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==4 && !(s->dref_bindings&(1ull<<i))){
        unsigned tid=G.state.tex[s->h.stage][s->b[i].slot]&0xfffff;TSlot *slot=&G.tslot[tid];
        VTexture *t=slot_texture(slot,1);if(!disjoint_sampling(t,slot,s->h.stage)||texture_range_ok(t))continue;
        view_t *sv=&slot->view;unsigned slevel=slot->has_view&&sv->set_levels?sv->base_level:0,slevels=slot->has_view&&sv->set_levels?sv->nlevels:t->source.d.levels;
        unsigned slayer=slot->has_view&&sv->set_layers?sv->min_layer:0,slayers=slot->has_view&&sv->set_layers?sv->nlayers:t->source.L.layers;
        uint32_t smask=(slevels>=32?~0u:((1u<<slevels)-1u))<<slevel;
        if(!G.disable_sample_copy_cache&&t->sample_copy&&t->sc_valid&&t->sample_other==t->other_version&&!(t->dirty_levels&smask)&&
           slevel>=t->sc_level&&slevel+slevels<=t->sc_level+t->sc_levels&&slayer>=t->sc_layer&&slayer+slayers<=t->sc_layer+t->sc_layers){G.profile_sample_copy_hits++;continue;}
        G.profile_sample_copy_misses++;
        int cube=t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP||t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
        if(t->source.d.target!=NVN_TEXTURE_TARGET_2D && t->source.d.target!=NVN_TEXTURE_TARGET_2D_ARRAY && !cube)die("disjoint sample copy dimensionality",t->source.d.target);
        if(!t->sample_copy){
            VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
                .imageType=VK_IMAGE_TYPE_2D,.format=t->format,.extent={t->width,t->height,1},.mipLevels=t->source.d.levels,
                .arrayLayers=t->source.L.layers,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
                .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
            CHECK(vkCreateImage(G.device,&ci,NULL,&t->sample_copy));VkMemoryRequirements r;vkGetImageMemoryRequirements(G.device,t->sample_copy,&r);
            VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            CHECK(vkAllocateMemory(G.device,&ai,NULL,&t->sample_memory));CHECK(vkBindImageMemory(G.device,t->sample_copy,t->sample_memory,0));
            VkImageViewType type=cube?(t->source.L.layers==6?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY):t->source.d.target==NVN_TEXTURE_TARGET_2D_ARRAY?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
            VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->sample_copy,.viewType=type,.format=t->format,
                .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t->source.d.levels,0,t->source.L.layers}};
            CHECK(vkCreateImageView(G.device,&vi,NULL,&t->sample_view));t->sample_layout=VK_IMAGE_LAYOUT_UNDEFINED;
            fprintf(stderr,"[nvn-vulkan] DISJOINT_SAMPLE_COPY serial=%llu levels=%u layers=%u\n",(unsigned long long)t->serial,t->source.d.levels,t->source.L.layers);
        }
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=t->sample_layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout=t->sample_layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->sample_copy,
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t->source.d.levels,0,t->source.L.layers}};
        vkCmdPipelineBarrier(S.command,HZ_COPY_SRC,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        view_t *v=&slot->view;unsigned level=slot->has_view&&v->set_levels?v->base_level:0,levels=slot->has_view&&v->set_levels?v->nlevels:t->source.d.levels;
        unsigned layer=slot->has_view&&v->set_layers?v->min_layer:0,layers=slot->has_view&&v->set_layers?v->nlayers:t->source.L.layers;
        for(unsigned l=level;l<level+levels;l++){
            unsigned w=t->width>>l,h=t->height>>l;if(!w)w=1;if(!h)h=1;
            VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,layer,layers},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,layer,layers},.extent={w,h,1}};
            vkCmdCopyImage(S.command,t->image,t->layout,t->sample_copy,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        }
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,HZ_COPY_DST,0,0,NULL,0,NULL,1,&barrier);t->sample_layout=barrier.newLayout;
        t->sample_other=t->other_version;t->dirty_levels&=~smask;t->sc_valid=1;t->sc_level=slevel;t->sc_levels=slevels;t->sc_layer=slayer;t->sc_layers=slayers;
    }
}
static VkImageLayout sampled_layout(VTexture *t,unsigned stage) {
    if(storage_image_use(t->serial))return VK_IMAGE_LAYOUT_GENERAL;
    if(stage!=5){
        if(G.state.rt.depth && G.state.rt.depth->serial==t->serial){S.depth_sampled=1;
            if(depth_read_only())return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            if(!G.feedback_layout)die("NVN writable depth feedback requires device support",t->serial);
            S.feedback_flags|=VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
            return VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        }
        for(int i=0;i<G.state.rt.n;i++)if(G.state.rt.color[i]&&G.state.rt.color[i]->serial==t->serial){
            if(!G.feedback_layout)die("NVN color feedback requires device support",t->serial);
            S.feedback_flags|=VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
            return VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        }
    }
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}
/* NVN permits comparison sampling of normalized color. Vulkan requires a
 * depth image: transfer the identical R16/D16 or R32F/D32F element bits through
 * a GPU buffer, retaining the original color attachment/storage image. */
static void comparison_image(VTexture *t) {
    if(t->depth!=1 || t->source.d.target==NVN_TEXTURE_TARGET_1D || t->source.d.target==NVN_TEXTURE_TARGET_1D_ARRAY)
        die("color comparison image dimensionality",t->source.d.target);
    VkFormat f=t->format==VK_FORMAT_R16_UNORM?VK_FORMAT_D16_UNORM:t->format==VK_FORMAT_R32_SFLOAT?VK_FORMAT_D32_SFLOAT:VK_FORMAT_UNDEFINED;
    if(f==VK_FORMAT_UNDEFINED)die("color comparison image format",t->format);
    if(!t->shadow){
        int cube=t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP||t->source.d.target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
        VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0,
            .imageType=VK_IMAGE_TYPE_2D,.format=f,.extent={t->width,t->height,1},.mipLevels=t->source.d.levels,
            .arrayLayers=t->source.L.layers,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        CHECK(vkCreateImage(G.device,&ci,NULL,&t->shadow));VkMemoryRequirements r;vkGetImageMemoryRequirements(G.device,t->shadow,&r);
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=r.size,.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(G.device,&ai,NULL,&t->shadow_memory));CHECK(vkBindImageMemory(G.device,t->shadow,t->shadow_memory,0));
        VkImageViewType type=cube?(t->source.L.layers==6?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY):t->source.d.target==NVN_TEXTURE_TARGET_2D_ARRAY?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
        VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->shadow,.viewType=type,.format=f,
            .subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,t->source.d.levels,0,t->source.L.layers}};
        CHECK(vkCreateImageView(G.device,&vi,NULL,&t->shadow_view));t->shadow_format=f;t->shadow_layout=VK_IMAGE_LAYOUT_UNDEFINED;
        fprintf(stderr,"[nvn-vulkan] COLOR_COMPARISON_ALIAS serial=%llu color=%u depth=%u size=%ux%u levels=%u layers=%u\n",(unsigned long long)t->serial,t->format,f,t->width,t->height,t->source.d.levels,t->source.L.layers);
    }
    if(t->shadow_version==t->version)return;
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask=t->shadow_layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_SHADER_READ_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout=t->shadow_layout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=t->shadow,.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,t->source.d.levels,0,t->source.L.layers}};
    vkCmdPipelineBarrier(S.command,HZ_COPY_SRC,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    for(unsigned level=0;level<t->source.d.levels;level++){
        unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
        uint64_t bytes=(uint64_t)width*height*t->source.L.layers*(f==VK_FORMAT_D16_UNORM?2:4);
        VBuffer *bridge=temporary_named((bytes+3)&~3ULL,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,TEMP_COMPARISON);
        VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,t->source.L.layers},.imageExtent={width,height,1}};
        vkCmdCopyImageToBuffer(S.command,t->image,t->layout,bridge->b,1,&region);
        VkBufferMemoryBarrier bb={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=bridge->b,.offset=0,.size=VK_WHOLE_SIZE};
        vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,1,&bb,0,NULL);
        region.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(S.command,bridge->b,t->shadow,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    }
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,HZ_COPY_DST,0,0,NULL,0,NULL,1,&barrier);
    t->shadow_layout=barrier.newLayout;t->shadow_version=t->version;
}
static void prepare_comparison_images(VShader *s) {
    if(!s)return;
    for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==4&&(s->dref_bindings&(1ull<<i))){
        unsigned tid=G.state.tex[s->h.stage][s->b[i].slot]&0xfffff;
        VTexture *t=slot_texture(&G.tslot[tid],1);
        if(aspect(t->format)==VK_IMAGE_ASPECT_COLOR_BIT)comparison_image(t);
    }
}
static void trace_texture(VTexture *t,const VShader *s,const RBinding *binding) {
    if(t->bc_block)return;   // 压缩纹理诊断捕获按线性尺寸读取，直接跳过 | compressed textures: skip the linear-sized diagnostic capture
    const char *trace_shader=G.trace_ui?"9a650561e5c40791":G.trace_shader[0]?G.trace_shader:"5c1ca4e7c3e4657c";
    if(!G.trace_frame||s->h.stage!=1||strcmp(s->key,trace_shader)||G.ntrace_textures==24||t->depth!=1)return;
    for(unsigned i=0;i<G.ntrace_textures;i++)if(G.trace_texture_serials[i]==t->serial)return;
    uint64_t bytes=(uint64_t)t->width*t->height*t->source.L.layers*t->bpp;
    if(bytes>32*1024*1024)return;
    G.trace_texture_serials[G.ntrace_textures++]=t->serial;
    all_barrier();VkImageLayout layout=t->layout;texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageAspectFlags flags=aspect(t->format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
    unsigned bpp=flags==VK_IMAGE_ASPECT_DEPTH_BIT?(t->format==VK_FORMAT_D16_UNORM?2:4):t->bpp;
    for(unsigned level=0;level<t->source.d.levels;level++){
        unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
        bytes=(uint64_t)width*height*t->source.L.layers*bpp;
        VBuffer *capture=temporary((bytes+3)&~3ULL,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy region={.imageSubresource={flags,level,0,t->source.L.layers},.imageExtent={width,height,1}};
        vkCmdCopyImageToBuffer(S.command,t->image,t->layout,capture->b,1,&region);
        char name[160];snprintf(name,sizeof(name),"texture-%04llu-%llu-mip%u",(unsigned long long)G.present_count+1,(unsigned long long)t->serial,level);
        trace_capture_queue(capture,bytes,name);
        fprintf(stderr,"[nvn-vulkan] GPU_TEXTURE_CAPTURE name=%s draw=%llu serial=%llu slot=%d binding=%u format=%u nvn_format=%u extent=%u,%u layers=%u levels=%u mip=%u bpp=%u cpu_owned=%d\n",name,(unsigned long long)G.draws,(unsigned long long)t->serial,binding->slot,binding->binding,t->format,t->source.d.format,width,height,t->source.L.layers,t->source.d.levels,level,bpp,t->cpu_owned);
        /* Compare streamed CPU backing with the actual GPU image. This is
         * diagnostic data only; never substitute it for GPU-owned pixels. */
        uint32_t decoded_format;int decoded_bpp;
        if(t->source.L.layers==1&&nvn_bc_unpacked(t->source.d.format,&decoded_format,&decoded_bpp)&&t->source.mem){
            int bw,bh;unsigned block=nvn_fmt_bpb(t->source.d.format,&bw,&bh);uint64_t row=(uint64_t)((width+bw-1)/bw)*block,rows=(height+bh-1)/bh;
            uint8_t *linear=malloc(row*rows),*decoded=malloc((size_t)width*height*decoded_bpp);if(!linear||!decoded)die("CPU texture diagnostic allocation",bytes);
            if(nvn_deswizzle(&t->source.d,level,0,t->source.mem,linear,row)||nvn_bc_decode(t->source.d.format,linear,row,width,height,decoded,(size_t)width*decoded_bpp))die("CPU texture diagnostic decode",t->serial);
            char path[1400];snprintf(path,sizeof(path),"%s/%s-cpu.bin",getenv("DM_FRAME_DIR"),name);FILE *out=fopen(path,"wb");if(!out)die("CPU texture diagnostic output",0);
            uint64_t cpu_bytes=(uint64_t)width*height*decoded_bpp;if(fwrite(decoded,1,cpu_bytes,out)!=cpu_bytes)die("CPU texture diagnostic write",cpu_bytes);fclose(out);free(linear);free(decoded);
            fprintf(stderr,"[nvn-vulkan] CPU_TEXTURE_CAPTURE bytes=%llu path=%s\n",(unsigned long long)cpu_bytes,path);
        }
    }
    texture_transition(t,layout);
}
static VBuffer *driver_table_build(VShader *s,const RBinding *b) {
    if(s->h.stage>=6||b->type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||(b->slot!=0&&b->slot!=2))die("driver table bank/stage/type",b->slot);
    uint8_t bytes[DRIVER_TABLE_BYTES];memset(bytes,0,sizeof bytes);
    if(b->slot==0){
        unsigned base=s->h.stage==5?0x310:s->h.stage==1?0x510:0x110;
        for(unsigned j=0;j<16;j++){
            Range r=G.state.storage[s->h.stage][j];if(r.size>UINT32_MAX)die("driver buffer size",r.size);uint32_t n=r.size;
            memcpy(bytes+base+j*16,&r.addr,8);memcpy(bytes+base+j*16+8,&n,4);
            if(s->h.stage==5){r=G.state.uniform[5][j];if(r.size>UINT32_MAX)die("driver uniform size",r.size);n=r.size;
                memcpy(bytes+0x210+j*16,&r.addr,8);memcpy(bytes+0x218+j*16,&n,4);}
        }
    }else{
        for(unsigned j=0;j<128;j++){uint32_t t=G.state.tex[s->h.stage][j],sp=G.state.samp[s->h.stage][j];
            memcpy(bytes+0x168+j*8,&t,4);memcpy(bytes+0x568+j*8,&sp,4);}
        for(unsigned j=0;j<9;j++){uint32_t image=G.state.img[s->h.stage][j];memcpy(bytes+0x120+j*8,&image,4);}
    }
    VBuffer **last=&S.last_driver_table[s->h.stage][b->slot/2];uint32_t hash=0,slot=0;
    if(!G.disable_driver_table_reuse){
        if(*last&&!memcmp((*last)->map,bytes,sizeof bytes)){G.profile_driver_table_reuses++;return *last;}
        hash=(uint32_t)crc32(0,bytes,sizeof bytes);slot=hash&(DRIVER_TABLE_SLOTS-1);
        while(S.driver_table_slots[slot]){
            DriverTable *entry=&S.driver_tables[S.driver_table_slots[slot]-1];
            if(entry->hash==hash&&!memcmp(entry->buffer->map,bytes,sizeof bytes)){
                G.profile_driver_table_reuses++;*last=entry->buffer;return *last;
            }
            slot=(slot+1)&(DRIVER_TABLE_SLOTS-1);
        }
    }
    /* Preserve all 64KB, including zeros beyond the populated words. The
     * uniform descriptor is immutable and cannot be written by guest shaders.
     * Its existing temporary registry owns it until the submission fence. */
    VBuffer *table=temporary_named(65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,TEMP_DRIVER);memcpy(table->map,bytes,sizeof bytes);G.profile_driver_tables++;
    if(!G.disable_driver_table_reuse&&S.ndriver_tables<LIMIT_WRITES){
        uint32_t index=S.ndriver_tables++;S.driver_tables[index]=(DriverTable){hash,table};S.driver_table_slots[slot]=index+1;*last=table;
    }
    return table;
}
static VBuffer *driver_table(VShader *s,const RBinding *b) {
    if(s->h.stage>=6||b->type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||(b->slot!=0&&b->slot!=2))die("driver table bank/stage/type",b->slot);
    /* The table content depends only on this stage's storage (bank 0) or texture/sampler/image (bank 2) bindings. */
    unsigned bank=b->slot/2,stage=s->h.stage;
    DriverCacheEntry *dc=&G.driver_cache[stage][bank];
    uint64_t revision_ssbo=G.ssbo_revision[stage],revision_tex=G.texbind_revision[stage];
    if(!G.disable_driver_table_reuse&&dc->table&&dc->epoch==S.epoch&&(bank==0?dc->ssbo==revision_ssbo:dc->tex==revision_tex)){G.profile_driver_table_reuses++;return dc->table;}
    VBuffer *table=driver_table_build(s,b);
    dc->epoch=S.epoch;dc->ssbo=revision_ssbo;dc->tex=revision_tex;dc->table=table;
    return table;
}
static void direct_storage_read(Range r) {
    int entry=-1,from=0;
    for(int i=0;i<S.ndirect_reads;i++)if(S.direct_reads[i].addr==r.addr&&S.direct_reads[i].size==r.size){entry=i;from=S.direct_read_write_count[i];break;}
    int dependency=0;for(int i=from;i<S.nwrites;i++)if(overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size)){dependency=1;break;}
    if(dependency)all_barrier_soft();
    if(entry<0&&S.ndirect_reads<LIMIT_WRITES){entry=S.ndirect_reads++;S.direct_reads[entry]=r;}
    if(entry>=0)S.direct_read_write_count[entry]=S.nwrites;
}
static VkSpecializationInfo storage_specialization(VShader *s,VkSpecializationMapEntry entries[LIMIT_BINDINGS]) {
    uint32_t count=0;
    for(unsigned i=0;i<s->h.nb;i++)if(s->storage_bias.mask&(1ull<<i))entries[count++]=(VkSpecializationMapEntry){s->storage_bias.spec_id[i],i*4,4};
    return (VkSpecializationInfo){count,entries,sizeof(S.storage_bias[s->h.stage]),S.storage_bias[s->h.stage]};
}
static VkImageView submission_image_view(const VkImageViewCreateInfo *info) {
    ImageViewKey key;memset(&key,0,sizeof key);
    key.image=info->image;key.flags=info->flags;key.type=info->viewType;key.format=info->format;
    key.components=info->components;key.range=info->subresourceRange;
    uint32_t slot=0;int cacheable=!G.disable_resource_cache&&!info->pNext;
    if(cacheable){
        uint64_t hash=14695981039346656037ULL;const uint8_t *bytes=(const uint8_t *)&key;
        for(size_t i=0;i<sizeof key;i++)hash=(hash^bytes[i])*1099511628211ULL;
        for(slot=(uint32_t)hash&(VIEW_TABLE_SIZE-1);S.view_table[slot];slot=(slot+1)&(VIEW_TABLE_SIZE-1)){
            uint32_t index=S.view_table[slot]-1;
            if(!memcmp(&S.view_keys[index],&key,sizeof key)){G.profile_view_hits++;return S.temporary_views[index];}
        }
    }
    if(S.nview==LIMIT_WRITES)die("submission image view count",S.nview);
    uint32_t index=S.nview++;CHECK(vkCreateImageView(G.device,info,NULL,&S.temporary_views[index]));
    G.profile_view_misses++;
    if(cacheable){S.view_keys[index]=key;S.view_table[slot]=index+1;}
    return S.temporary_views[index];
}
static void binding_plan_remember(VShader *s,VkDescriptorSet set,const BindingResource *resources,int safe) {
    BindingPlan *plan=s->plan;
    if(!plan||!safe||G.disable_binding_plans||G.disable_desc_cache||G.trace_frame||!set){if(plan)plan->resolved=0;return;}
    if(s->h.nb)memcpy(plan->resources,resources,(size_t)s->h.nb*sizeof(*resources));
    plan->epoch=S.epoch;plan->state_revision=G.binding_revision[s->h.stage];
    plan->registry_revision=G.registry_revision;plan->pool_revision=G.pool_revision;
    plan->set=set;plan->resolved=1;
}
static VkDescriptorSet binding_plan_try(VShader *s) {
    BindingPlan *plan=s->plan;
    if(!plan||G.disable_binding_plans||G.disable_desc_cache||G.trace_frame||!plan->eligible){G.profile_plan_fallbacks++;return VK_NULL_HANDLE;}
    { int why=!plan->resolved||!plan->set?0:plan->epoch!=S.epoch?1:
              (!G.plan_precise_state&&plan->state_revision!=G.binding_revision[s->h.stage])?2:
              plan->registry_revision!=G.registry_revision?3:plan->pool_revision!=G.pool_revision?4:-1;
      if(why>=0){
        if(why==0)G.profile_plan_rev++;
        else if(why==1)G.profile_plan_epoch++;
        else if(why==2)G.profile_plan_state++;
        else if(why==3)G.profile_plan_reg++;
        else G.profile_plan_pool++;
        G.profile_plan_misses++;return VK_NULL_HANDLE;} }
    uint32_t live_dyn[LIMIT_BINDINGS];memset(live_dyn,0,sizeof(live_dyn));
    for(uint32_t n=0;n<plan->nbuffer;n++){
        unsigned i=plan->buffers[n];const RBinding *b=&s->b[i];
        Range r=G.state.uniform[s->h.stage][b->slot];uint64_t offset=0;
        if(!r.addr||!r.size){G.profile_plan_buf_none++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        VPool *pool=pool_read(r.addr,r.size,&offset);const BindingBuffer *cached=&plan->resources[i].buffer;
        int dynamic=(s->dyn_bindings>>i)&1;   // 动态绑定的偏移不算计划失效条件：它随绑定调用走 | A dynamic binding's offset is not a plan-invalidating condition: it rides the bind call
        if(offset%G.properties.limits.minUniformBufferOffsetAlignment){G.profile_plan_buf_align++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        if(r.addr!=cached->range.addr||r.size!=cached->range.size){G.profile_plan_buf_addr++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        if(pool->serial!=cached->pool_serial){G.profile_plan_buf_pool++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        if(!dynamic&&offset!=cached->offset){G.profile_plan_buf_off++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        if(dynamic&&offset>0xffffffffu){G.profile_plan_buf_dynbig++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        if(dynamic)live_dyn[i]=(uint32_t)offset;
        G.profile_plan_buffers++;
    }
    for(uint32_t n=0;n<plan->nimage;n++){
        unsigned i=plan->images[n];const RBinding *b=&s->b[i];
        uint64_t handle=G.state.tex[s->h.stage][b->slot];unsigned tid=(unsigned)handle&0xfffff;
        if(tid>=(unsigned)G.texture_pool_n||tid>=G.texture_slots||!G.tslot[tid].live){G.profile_plan_misses++;return VK_NULL_HANDLE;}
        TSlot *slot=&G.tslot[tid];VTexture *t=slot_texture(slot,1);const BindingImage *cached=&plan->resources[i].image;
        if(t->source.d.target!=NVN_TEXTURE_TARGET_2D||
           (slot->has_view&&(slot->view.set_levels||slot->view.set_layers||slot->view.set_format||slot->view.set_target))||
           cached->descriptor_id!=tid||cached->texture.incarnation!=t->incarnation||cached->texture.slot!=(uint32_t)(t-G.textures)||
           disjoint_sampling(t,slot,s->h.stage)){G.profile_plan_img_check++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        VkImageLayout layout=sampled_layout(t,s->h.stage);texture_transition(t,layout);
        if(layout!=cached->layout||t->view==VK_NULL_HANDLE){G.profile_plan_img_layout++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        if(b->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER){
            unsigned sid=(unsigned)((b->sampler>=0?G.state.samp[s->h.stage][b->sampler]:handle)>>20)&0xfff;VkSampler sampler;
            if(sid>=LIMIT_OBJECTS||!G.sslot[sid].live){if(!(s->fetch_bindings&(1ull<<i))||sid||!G.fetch_sampler){G.profile_plan_img_samp++;G.profile_plan_misses++;return VK_NULL_HANDLE;}sampler=G.fetch_sampler;}
            else sampler=G.sslot[sid].sampler;
            /* 采样器对象身份逐绑定比对（精确模式下闸门不含 state_revision，这里是唯一防线）。
             * mtl_reg_samp 换对象会 bump registry_revision（另一道闸门），对象销毁后句柄复用也由此兜住。
             * Per-binding sampler identity compare (the only line of defence once the precise mode drops
             * the state_revision gate). Sampler re-registration bumps registry_revision (a separate gate),
             * which also covers driver handle reuse after a destroy. */
            if(sampler!=cached->sampler){G.profile_plan_img_samp++;G.profile_plan_misses++;return VK_NULL_HANDLE;}
        }
        G.profile_plan_images++;
    }
    if(s->dyn_count){uint32_t k=0;for(uint32_t i=0;i<s->h.nb;i++)if((s->dyn_bindings>>i)&1)S.dyn_offset[s->h.stage][k++]=live_dyn[i];S.dyn_count[s->h.stage]=k;}
    G.profile_plan_hits++;return plan->set;
}
static VkDescriptorSet bind_shader(VShader *s) {
    memset(S.storage_bias[s->h.stage],0,sizeof S.storage_bias[s->h.stage]);
    S.dyn_count[s->h.stage]=0;
    uint64_t bs_plan=prof_now();VkDescriptorSet planned=binding_plan_try(s);G.profile_bind_plan_ns+=prof_now()-bs_plan;
    if(planned){G.profile_bind_hits++;return planned;}
    uint64_t bs_full=prof_now();G.profile_bind_full_calls++;
    VkWriteDescriptorSet w[LIMIT_BINDINGS];VkDescriptorBufferInfo bi[LIMIT_BINDINGS];VkDescriptorImageInfo ii[LIMIT_BINDINGS];
    uint32_t live_dyn[LIMIT_BINDINGS];memset(live_dyn,0,sizeof(live_dyn));
    BindingResource plan_resources[LIMIT_BINDINGS];memset(plan_resources,0,sizeof(plan_resources));
    int plan_safe=s->plan&&s->plan->eligible&&!G.disable_binding_plans&&!G.disable_desc_cache&&!G.trace_frame;
    memset(w,0,s->h.nb*sizeof(*w));memset(bi,0,s->h.nb*sizeof(*bi));memset(ii,0,s->h.nb*sizeof(*ii));
    int cacheable=!G.disable_desc_cache&&!G.trace_frame;VkDescriptorSet set=VK_NULL_HANDLE;
    for(uint32_t i=0;i<s->h.nb;i++) { RBinding *b=&s->b[i]; w[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
       .dstBinding=b->binding,.descriptorCount=1,.descriptorType=(VkDescriptorType)b->type};
       if(b->kind==1)bi[i]=(VkDescriptorBufferInfo){G.support.b,0,1392};
       else if(b->kind==7) { if(!s->c1.b)die("missing shader c1",i);bi[i]=(VkDescriptorBufferInfo){s->c1.b,0,65536}; }
       else if(b->kind==10) {
           if(b->slot<0 || b->slot>=128)die("texel buffer slot",b->slot);
           uint32_t tid=(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot])&0xfffff;
           if(tid>=G.texture_slots||!G.tslot[tid].live)die("unbound texel buffer",tid);
           TSlot *slot=&G.tslot[tid];tex_t *t=&slot->texture;
           if(t->d.target!=NVN_TEXTURE_TARGET_BUFFER||t->d.levels!=1||(slot->has_view&&(slot->view.set_layers||slot->view.set_levels)))die("texel buffer target/view",tid);
           int nvn=slot->has_view&&slot->view.set_format?slot->view.format:t->d.format;
           VkFormat f=format(nvn);VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(G.physical,f,&properties);
           VkFormatFeatureFlags required=b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER?VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT:VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
           if(!(properties.bufferFeatures&required))die("texel buffer format feature",nvn);
           uint64_t offset=0;VPool *p=pool_read((uint64_t)(uintptr_t)t->mem,t->L.size,&offset);VkBuffer buffer=p->v.b;
           if(offset%G.properties.limits.minTexelBufferOffsetAlignment){VBuffer *aligned=temporary((t->L.size+3)&~3ULL,VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
               if(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER||offset%4||t->L.size%4)die("writable/word-unaligned texel buffer",offset);
               all_barrier();VkBufferCopy copy={offset,0,t->L.size};vkCmdCopyBuffer(S.command,p->v.b,aligned->b,1,&copy);all_barrier();buffer=aligned->b;offset=0;}
           if(S.nbuffer_view==LIMIT_WRITES)die("texel buffer view count",S.nbuffer_view);
           VkBufferViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=buffer,.format=f,.offset=offset,.range=t->L.size};
           CHECK(vkCreateBufferView(G.device,&vi,NULL,&S.buffer_views[S.nbuffer_view]));w[i].pTexelBufferView=&S.buffer_views[S.nbuffer_view++];
           if(b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)mark_write((uint64_t)(uintptr_t)t->mem,t->L.size);
           cacheable=0;continue;
       }
       else if(b->kind==8) {
           VBuffer *table=driver_table(s,b);
           bi[i]=(VkDescriptorBufferInfo){table->b,0,65536};
       }
       else if(b->kind==2 || b->kind==3 || b->kind==9) {
           if(b->slot<0 || b->slot>=16)die("shader buffer slot",b->slot);
           Range r=(b->kind==2||b->kind==9)?G.state.uniform[s->h.stage][b->slot]:G.state.storage[s->h.stage][b->slot];
           if(!r.addr || !r.size)die("unbound original shader buffer",b->slot);
           uint64_t offset=0;VPool *p=pool_read(r.addr,r.size,&offset);
           uint64_t alignment=b->kind==2?G.properties.limits.minUniformBufferOffsetAlignment:G.properties.limits.minStorageBufferOffsetAlignment;
           uint64_t prefix=offset%alignment;
           if(plan_safe){if(b->kind!=2||prefix)plan_safe=0;else plan_resources[i].buffer=(BindingBuffer){.range=r,.pool_serial=p->serial,.offset=offset};}
           if(prefix&&S.read_snapshots_allowed&&(s->storage_bias.mask&(1ull<<i))&&prefix%4==0&&prefix/4<=UINT32_MAX&&r.size%4==0&&prefix<=G.properties.limits.maxStorageBufferRange&&r.size<=G.properties.limits.maxStorageBufferRange-prefix){
               /* Keep the complete guest range in its original pool. Shader
                * indices compensate for the aligned descriptor prefix. Any
                * queued producer write is made visible before the first read
                * after it; later readonly draws need no additional copy. */
               direct_storage_read(r);S.storage_bias[s->h.stage][i]=(uint32_t)(prefix/4);
               bi[i]=(VkDescriptorBufferInfo){p->v.b,offset-prefix,r.size+prefix};G.profile_storage_bias_bindings++;G.profile_storage_bias_bytes+=r.size;
           }else if(prefix||(((s->dyn_bindings>>i)&1)&&offset>0xffffffffu)){
               if(r.size>=1024*1024&&!(s->large_bindings_seen&(1ull<<i))){
                   s->large_bindings_seen|=1ull<<i;
                   fprintf(stderr,"[nvn-vulkan] UNALIGNED_LARGE_BIND shader=%s stage=%u binding=%u kind=%u slot=%d bytes=%llu prefix=%llu alignment=%llu writable=%u\n",s->key,s->h.stage,b->binding,b->kind,b->slot,(unsigned long long)r.size,(unsigned long long)(offset%alignment),(unsigned long long)alignment,b->flags);
               }
               AlignedBinding *aligned=NULL;
               for(int j=0;j<S.naligned;j++)if(overlap(r.addr,r.size,S.aligned[j].range.addr,S.aligned[j].range.size)){
                   if(r.addr!=S.aligned[j].range.addr || r.size!=S.aligned[j].range.size)die("overlapping unaligned shader ranges",r.addr);
                   aligned=&S.aligned[j];break;
               }
               if(!aligned){if(S.naligned==2*LIMIT_BINDINGS)die("aligned binding count",S.naligned);
                   if(offset%4 || r.size%4)die("unaligned copy requires word granularity",offset);
                   VBuffer *temp=NULL;int snapshot_index=cached_snapshot(r,&temp);
                   if(!temp){
                   temp=temporary_named((r.size+3)&~3ULL,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,TEMP_ALIGNED);
                   int cpu_only=(b->kind==2||((b->kind==3||b->kind==9)&&!b->flags))&&!G.disable_cpu_uniform_snapshot;
                   for(int j=0;j<S.nwrites&&cpu_only;j++)if(!S.write_cpu[j]&&overlap(r.addr,r.size,S.writes[j].addr,S.writes[j].size))cpu_only=0;
                   if(cpu_only){
                       G.profile_cpu_snapshots++;
                       /* Unwritten bytes come from CPU backing; every queued
                        * overlapping write has an exact CPU payload. Overlay
                        * those immutable command payloads in recording order
                        * to snapshot the value this draw would read on GPU. */
                       memcpy(temp->map,p->v.map+offset,r.size);
                       for(int j=0;j<S.nwrites;j++)if(S.write_cpu[j]&&overlap(r.addr,r.size,S.writes[j].addr,S.writes[j].size)){
                           Range w=S.writes[j];uint64_t lo=r.addr>w.addr?r.addr:w.addr,hi=r.addr+r.size<w.addr+w.size?r.addr+r.size:w.addr+w.size;
                           memcpy(temp->map+(lo-r.addr),S.write_cpu[j]+(lo-w.addr),hi-lo);
                       }
                   }else{
                       G.profile_gpu_copies++;
                       all_barrier();VkBufferCopy copy={offset,0,r.size};vkCmdCopyBuffer(S.command,p->v.b,temp->b,1,&copy);all_barrier();
                   }
                   if(snapshot_index>=0){S.read_snapshots[snapshot_index].buffer=temp;S.read_snapshots[snapshot_index].write_count=S.nwrites;}
                   }
                   aligned=&S.aligned[S.naligned++];*aligned=(AlignedBinding){r,temp,0};
               }
               if(b->kind==3&&b->flags)aligned->written=1;
               bi[i]=(VkDescriptorBufferInfo){aligned->buffer->b,0,r.size};
           }else bi[i]=(VkDescriptorBufferInfo){p->v.b,offset,r.size};
           if(b->kind==3 && b->flags)mark_write(r.addr,r.size);
           if((s->dyn_bindings>>i)&1){
               /* 动态描述符：内容固定为（缓冲, 0, 整段），逐 draw 偏移走绑定调用；
                  快照/临时路径的偏移本来就是 0，保持其范围。 | Dynamic descriptor: fix the content to
                  (buffer, 0, whole range); the per-draw offset travels in the bind call. Temp snapshot
                  paths already carry offset zero, keep their range. */
               live_dyn[i]=(uint32_t)bi[i].offset;bi[i].offset=0;bi[i].range=VK_WHOLE_SIZE;
           }
       } else if(b->kind==4 || b->kind==6) {
           if(b->slot<0 || b->slot>=(b->kind==6?32:128))die("texture shader slot",b->slot);
           uint64_t handle=b->kind==6?G.state.img[s->h.stage][b->slot]:G.state.tex[s->h.stage][b->slot];
           unsigned tid=(unsigned)handle&0xfffff;if(tid>=(unsigned)G.texture_pool_n || tid>=G.texture_slots || !G.tslot[tid].live)die("unbound texture descriptor",tid);
           TSlot *slot=&G.tslot[tid];
           VTexture *t=slot_texture(slot,1);
           trace_texture(t,s,b);
           int alias=b->kind==4&&(s->dref_bindings&(1ull<<i))&&aspect(t->format)==VK_IMAGE_ASPECT_COLOR_BIT;
           int sampled_copy=b->kind==4&&!alias&&disjoint_sampling(t,slot,s->h.stage),ranged=0;
           if(sampled_copy&&texture_range_ok(t))sampled_copy=0,ranged=1;
           if(!G.feedback_layout && !sampled_copy && !ranged && b->kind==4 && s->h.stage!=5){
               for(int rt=0;rt<G.state.rt.n;rt++)if(G.state.rt.color[rt]&&G.state.rt.color[rt]->serial==t->serial){
                   view_t *v=&slot->view,*a=&G.state.rt.cview[rt];
                   fprintf(stderr,"[nvn-vulkan] COLOR_FEEDBACK_VIEW shader=%s binding=%u serial=%llu sampled_levels=%d,%d flags=%u sampled_layers=%d,%d flags=%u attachment_level=%d flags=%u attachment_layer=%d flags=%u\n",s->key,b->binding,(unsigned long long)t->serial,v->base_level,v->nlevels,slot->has_view&&v->set_levels,v->min_layer,v->nlayers,slot->has_view&&v->set_layers,a->base_level,G.state.rt.has_cview[rt]&&a->set_levels,a->min_layer,G.state.rt.has_cview[rt]&&a->set_layers);
               }
           }
           if(ranged){view_t *rv=&slot->view;unsigned rl=slot->has_view&&rv->set_levels?rv->base_level:0,rn=slot->has_view&&rv->set_levels?rv->nlevels:t->source.d.levels;
               texture_transition_levels(t,rl,rn,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
           else if(!alias&&!sampled_copy)texture_transition(t,storage_image_use(t->serial)?VK_IMAGE_LAYOUT_GENERAL:sampled_layout(t,s->h.stage));
           if(alias&&!t->shadow_view){fprintf(stderr,"[nvn-vulkan] missing comparison view shader=%s binding=%u slot=%d serial=%llu stage=%u\n",s->key,b->binding,b->slot,(unsigned long long)t->serial,s->h.stage);die("comparison image preparation",t->serial);}
           if(sampled_copy&&!t->sample_view)die("disjoint sample copy preparation",t->serial);
           VkImage selected_image=alias?t->shadow:sampled_copy?t->sample_copy:t->image;VkFormat selected_format=alias?t->shadow_format:t->format;
           ii[i].imageView=alias?t->shadow_view:sampled_copy?t->sample_view:t->view;ii[i].imageLayout=alias?t->shadow_layout:sampled_copy?t->sample_layout:ranged?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:t->layout;
           if(slot->has_view) {
               view_t *v=&slot->view;
               uint32_t level=v->set_levels?v->base_level:0,levels=v->set_levels?v->nlevels:t->source.d.levels;
               uint32_t layer=v->set_layers?v->min_layer:0,layers=v->set_layers?v->nlayers:t->source.L.layers;
               int target=v->set_target?v->target:t->source.d.target;
               if(level>=t->source.d.levels || !levels || levels>t->source.d.levels-level ||
                  layer>=t->source.L.layers || !layers || layers>t->source.L.layers-layer ||
                  (v->set_format && v->format!=t->source.d.format) ||
                  (target!=NVN_TEXTURE_TARGET_2D && target!=NVN_TEXTURE_TARGET_3D && target!=NVN_TEXTURE_TARGET_2D_ARRAY && target!=NVN_TEXTURE_TARGET_CUBEMAP && target!=NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) ||
                  (target==NVN_TEXTURE_TARGET_2D && layers!=1)) {
                   fprintf(stderr,"[nvn-vulkan] texture view id=%u flags=%u,%u,%u,%u levels=%d,%d layers=%d,%d format=%d/%d target=%d/%d\n",
                       tid,v->set_levels,v->set_layers,v->set_format,v->set_target,v->base_level,v->nlevels,v->min_layer,v->nlayers,
                       v->format,t->source.d.format,v->target,t->source.d.target);
                   die("texture view format/target/layer/range adaptation",tid);
               }
               if(level || levels!=t->source.d.levels || layer || layers!=t->source.L.layers || target!=t->source.d.target) {
                   VkImageViewType view_type=target==NVN_TEXTURE_TARGET_3D?VK_IMAGE_VIEW_TYPE_3D:target==NVN_TEXTURE_TARGET_2D?VK_IMAGE_VIEW_TYPE_2D:target==NVN_TEXTURE_TARGET_2D_ARRAY?VK_IMAGE_VIEW_TYPE_2D_ARRAY:
                       target==NVN_TEXTURE_TARGET_CUBEMAP?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
                   VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=selected_image,.viewType=view_type,
                       .format=selected_format,.subresourceRange={aspect(selected_format)&VK_IMAGE_ASPECT_DEPTH_BIT?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,level,levels,layer,layers}};
                   ii[i].imageView=submission_image_view(&vi);
               }
           }
           if(b->kind==4 && b->type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) { unsigned sid=(unsigned)((b->sampler>=0?G.state.samp[s->h.stage][b->sampler]:handle)>>20)&0xfff;
               if(sid>=LIMIT_OBJECTS || !G.sslot[sid].live){
                   /* Maxwell texel fetch requires no sampler. The translator
                    * still declares a combined descriptor; Vulkan requires a
                    * valid sampler there even though OpImage discards it. */
                   if(!(s->fetch_bindings&(1ull<<i)) || sid){fprintf(stderr,"[nvn-vulkan] unbound sampler shader=%s stage=%u slot=%d sampler=%d handle=%#llx separate=%#llx fetch_bindings=%#llx\n",s->key,s->h.stage,b->slot,b->sampler,(unsigned long long)handle,(unsigned long long)(b->sampler>=0?G.state.samp[s->h.stage][b->sampler]:0),(unsigned long long)s->fetch_bindings);die("unbound sampler descriptor",sid);}
                   if(!G.fetch_sampler){VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=VK_LOD_CLAMP_NONE};CHECK(vkCreateSampler(G.device,&ci,NULL,&G.fetch_sampler));}
                   ii[i].sampler=G.fetch_sampler;
               }else {ii[i].sampler=G.sslot[sid].sampler;
                   if(G.trace_frame&&s->h.stage==1&&!strcmp(s->key,"5c1ca4e7c3e4657c")){
                       samp_par *p=&G.sslot[sid].source;
                       fprintf(stderr,"[nvn-vulkan] TEXTURE_SAMPLER draw=%llu binding=%u slot=%d serial=%llu sampler=%u minf=%u magf=%u lod=%g,%g bias=%g compare=%u,%u view_levels=%d,%d set_levels=%u\n",(unsigned long long)G.draws,b->binding,b->slot,(unsigned long long)t->serial,sid,p->minf,p->magf,p->lod_min,p->lod_max,p->lod_bias,p->cmp_mode,p->cmp_func,slot->view.base_level,slot->view.nlevels,slot->has_view&&slot->view.set_levels);
                   }
               } }
           if(ranged&&ii[i].imageView==t->view)die("ranged sampling without a subresource view",t->serial);
           if(b->kind==6){t->cpu_owned=0;tex_dirty(t);}
           if(plan_safe){
               int custom_view=slot->has_view&&(slot->view.set_levels||slot->view.set_layers||slot->view.set_format||slot->view.set_target);
               if(b->kind!=4||alias||sampled_copy||ranged||custom_view||t->source.d.target!=NVN_TEXTURE_TARGET_2D||ii[i].imageView!=t->view)
                   plan_safe=0;
               else plan_resources[i].image=(BindingImage){.texture=texture_reference(t),.descriptor_id=tid,.layout=ii[i].imageLayout,.sampler=ii[i].sampler};
           }
           w[i].pImageInfo=&ii[i];continue;
       } else die("Vulkan shader resource kind",b->kind);
       w[i].pBufferInfo=&bi[i];
    }
    if(s->dyn_count){uint32_t k=0;for(uint32_t i=0;i<s->h.nb;i++)if((s->dyn_bindings>>i)&1)S.dyn_offset[s->h.stage][k++]=live_dyn[i];S.dyn_count[s->h.stage]=k;}
    uint32_t nb=s->h.nb;size_t half=(size_t)nb*sizeof(VkDescriptorBufferInfo);uint32_t slot=0;uint64_t hash=0;
    if(cacheable){
        hash=14695981039346656037ULL^(uint64_t)(uintptr_t)s;const uint8_t *k0=(const uint8_t *)bi,*k1=(const uint8_t *)ii;
        const uint64_t *q0=(const uint64_t *)k0,*q1=(const uint64_t *)k1;size_t words=half/8;
        for(size_t j=0;j<words;j++){hash=(hash^q0[j])*1099511628211ULL;hash^=hash>>29;}
        for(size_t j=0;j<words;j++){hash=(hash^q1[j])*1099511628211ULL;hash^=hash>>29;}
        for(slot=(uint32_t)hash&(DESC_TABLE-1);S.desc_table[slot];slot=(slot+1)&(DESC_TABLE-1)){
            const DescEntry *e=&S.desc_entries[S.desc_table[slot]-1];
            if(e->hash==hash&&e->shader==s&&!memcmp(S.desc_arena+e->off,bi,half)&&!memcmp(S.desc_arena+e->off+half,ii,half)){
                G.profile_desc_hits++;binding_plan_remember(s,e->set,plan_resources,plan_safe);return e->set;}
        }
        G.profile_desc_misses++;
    }
    VkDescriptorSetAllocateInfo a={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=S.descriptors,
      .descriptorSetCount=1,.pSetLayouts=&s->layout};uint64_t alloc_started=prof_now();CHECK(vkAllocateDescriptorSets(G.device,&a,&set));G.profile_desc_alloc_ns+=prof_now()-alloc_started;
    for(uint32_t i=0;i<nb;i++)w[i].dstSet=set;
    uint64_t update_started=prof_now();vkUpdateDescriptorSets(G.device,s->h.nb,w,0,NULL);G.profile_desc_update_ns+=prof_now()-update_started;
    if(cacheable&&S.ndesc<DESC_ENTRIES&&S.desc_arena_used+2*half<=DESC_ARENA&&S.ndesc*2<DESC_TABLE){
        DescEntry *e=&S.desc_entries[S.ndesc];*e=(DescEntry){hash,s,set,nb,(uint32_t)S.desc_arena_used};
        memcpy(S.desc_arena+S.desc_arena_used,bi,half);memcpy(S.desc_arena+S.desc_arena_used+half,ii,half);S.desc_arena_used+=2*half;
        S.desc_table[slot]=++S.ndesc;
    }
    if(G.trace_frame&&G.trace_ui&&s->h.stage==1&&!strcmp(s->key,"62ddcb3faa41a92a")){
        for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==2||s->b[i].kind==9){uint64_t bytes=bi[i].range;
            if(bytes%4)die("UI effective uniform capture alignment",bytes);
            VBuffer *capture=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);all_barrier();VkBufferCopy copy={bi[i].offset,0,bytes};
            vkCmdCopyBuffer(S.command,bi[i].buffer,capture->b,1,&copy);char name[160];snprintf(name,sizeof(name),"ui-effective-%04llu-%llu-ps-ubo%d",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,s->b[i].slot);trace_capture_queue(capture,bytes,name);
        }
    }
    binding_plan_remember(s,set,plan_resources,plan_safe);G.profile_bind_full_ns+=prof_now()-bs_full;return set;
}
static VShader *program_stage_optional_impl(int stage) {
    const prog_t *p=G.state.program;if(!p)die("draw/dispatch without program",stage);
    for(uint32_t i=0;i<p->nstage;i++) { const void *control=p->sd[i].control;if(!control)die("shader data control",i);
       uint64_t mix=((uint64_t)(uintptr_t)control>>4^(uint64_t)stage*0x9e3779b97f4a7c15ULL)*0xbf58476d1ce4e5b9ULL;
       ControlCache *c=&G.control_cache[(mix>>40)&(CONTROL_CACHE-2)];
       if(!(c->valid&&c->control==control&&c->stage==(uint32_t)stage)){
           if(c[1].valid&&c[1].control==control&&c[1].stage==(uint32_t)stage)c++;
           else if(c->valid&&(!c[1].valid||(mix&1)))c++;
       }
       if(!G.disable_control_cache&&c->valid&&c->control==control&&c->stage==(uint32_t)stage){
           if(!memcmp(c->bytes,control,0x880)){G.profile_control_hits++;if(c->shader)return c->shader;continue;}
           G.profile_control_changed++;
       }else if(c->valid)G.profile_control_evict++;
       G.profile_control_misses++;
       VShader *found=shader_entry(control,stage)?shader_get(control,stage):NULL;
       c->control=control;c->stage=stage;c->valid=1;c->shader=found;memcpy(c->bytes,control,0x880);
       if(found)return found; }
    return NULL;
}
static VShader *program_stage_optional(int stage) {
    uint64_t started=prof_now();VShader *s=program_stage_optional_impl(stage);
    G.profile_lookup_ns+=prof_now()-started;return s;
}
static VShader *program_stage(int stage) {
    VShader *s=program_stage_optional(stage);if(!s)die("requested program stage missing",stage);return s;
}

static VkBlendFactor blend_factor(unsigned f) {
    switch(f) {
    case NVN_BLEND_FUNC_ZERO:return VK_BLEND_FACTOR_ZERO;case NVN_BLEND_FUNC_ONE:return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR:return VK_BLEND_FACTOR_SRC_COLOR;case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA:return VK_BLEND_FACTOR_SRC_ALPHA;case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA:return VK_BLEND_FACTOR_DST_ALPHA;case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR:return VK_BLEND_FACTOR_DST_COLOR;case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_COLOR:return VK_BLEND_FACTOR_CONSTANT_COLOR;case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA:return VK_BLEND_FACTOR_CONSTANT_ALPHA;case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:die("blend factor",f); }
}
static VkBlendOp blend_op(unsigned op) {
    switch(op) {case NVN_BLEND_EQUATION_ADD:return VK_BLEND_OP_ADD;case NVN_BLEND_EQUATION_SUB:return VK_BLEND_OP_SUBTRACT;
       case NVN_BLEND_EQUATION_REVERSE_SUB:return VK_BLEND_OP_REVERSE_SUBTRACT;case NVN_BLEND_EQUATION_MIN:return VK_BLEND_OP_MIN;
       case NVN_BLEND_EQUATION_MAX:return VK_BLEND_OP_MAX;default:die("blend equation",op); }
}
static VkPrimitiveTopology topology(int p) {
    switch(p) {case NVN_DRAW_PRIMITIVE_POINTS:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;case NVN_DRAW_PRIMITIVE_LINE_STRIP:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;default:die("draw primitive",p); }
}
static VkImageSubresourceRange attachment_range(const tex_t *source,const view_t *v) {
    unsigned level=v&&v->set_levels?v->base_level:0,layer=v&&v->set_layers?v->min_layer:0;
    if(level>=source->d.levels || layer>=source->L.layers || (v&&v->set_levels&&v->nlevels<1) ||
       (v&&v->set_layers&&v->nlayers!=1) || (v&&v->set_format&&v->format!=source->d.format) || source->d.target==NVN_TEXTURE_TARGET_3D)
        die("attachment view format/layer/mip",level);
    return (VkImageSubresourceRange){aspect(format(source->d.format)),level,1,layer,1};
}
/* Submit the commands recorded so far so the GPU starts while the CPU keeps recording this segment.
 * Completion, writeback, events and counters are still published only after the segment's final fence. */
static void chunk_ts_write(int end) {
    if(!S.chunk_ts||S.nchunk_ts>=128)return;
    S.chunk_pass0[S.nchunk_ts]=G.pass_count;S.chunk_draw0[S.nchunk_ts]=G.draws;
    vkCmdWriteTimestamp(S.command,end?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,S.chunk_ts,S.nchunk_ts++);
}
static void sub_chunk_flush(void) {
    /* Pass queries belong to the submission, not a command buffer. Fresh query
     * prefixes are reset before first use and all chunks retire at the final
     * fence; profiling therefore need not disable normal early submission. */
    if(S.in_pass||S.active_occlusion!=UINT32_MAX||G.draws-S.chunk_mark<(uint64_t)G.chunk_draws||S.chunk_index+1>=48)return;
    chunk_ts_write(1);
    uint64_t c_t0=prof_now();CHECK(vkEndCommandBuffer(S.command));G.profile_chunk_end_ns+=prof_now()-c_t0;
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&S.command};
    uint64_t c_t1=prof_now();CHECK(vkQueueSubmit(G.queue,1,&si,VK_NULL_HANDLE));G.profile_chunk_submit_ns+=prof_now()-c_t1;
    S.chunk_index++;
    if(S.chunk_index>=S.chunk_alloc){
        VkCommandBufferAllocateInfo a={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=S.command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        CHECK(vkAllocateCommandBuffers(G.device,&a,&S.chunk_cb[S.chunk_alloc]));S.chunk_alloc++;
    }
    uint64_t c_t2=prof_now();
    S.command=S.chunk_cb[S.chunk_index];
    VkCommandBufferBeginInfo b={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(S.command,&b));chunk_ts_write(0);
    nvn_vk_command_state_reset(&S.command_state);S.command_dirty=1;S.chunk_mark=G.draws;G.profile_chunks++;
    G.profile_chunk_begin_ns+=prof_now()-c_t2;
}
/* 持久渲染通道缓存：PassKey 是纯值数据（附件/子通道/依赖描述，无句柄），且 VkRenderPass 描述后不可变，
 * 因此对象可与设备同寿命。省去每 epoch 重建通道对象的成本；表满/探测链过长时退化为"创建不缓存"（对象
 * 记入 S.overflow_passes 随 epoch 退休，行为与今天一致）。
 * Persistent render-pass cache: PassKey is pure value data (attachment/subpass/dependency
 * descriptions, no handles), and render-pass objects are immutable after creation, so they can
 * live as long as the device. Saves the per-epoch recreate churn; a full table or long probe
 * chain degrades to "create without caching" (the object retires with the epoch like today). */
static VkRenderPass render_pass_get(const PassKey *key,const VkRenderPassCreateInfo *ri) {
    uint64_t h=14695981039346656037ULL;const uint8_t *b=(const uint8_t *)key;
    for(size_t i=0;i<sizeof *key;i++)h=(h^b[i])*1099511628211ULL;
    uint32_t slot=(uint32_t)h&1023;
    for(uint32_t p=0;p<16;p++){uint32_t i=(slot+p)&1023;
        if(G.rp_cache[i].pass==VK_NULL_HANDLE){
            CHECK(vkCreateRenderPass(G.device,ri,NULL,&G.rp_cache[i].pass));
            G.rp_cache[i].key=*key;G.profile_rpc_creates++;return G.rp_cache[i].pass;}
        if(!memcmp(&G.rp_cache[i].key,key,sizeof *key)){G.profile_rpc_hits++;return G.rp_cache[i].pass;}}
    if(S.novf==16)die("render pass overflow cache",S.novf);
    CHECK(vkCreateRenderPass(G.device,ri,NULL,&S.overflow_passes[S.novf++]));G.profile_rpc_overflow++;return S.overflow_passes[S.novf-1];
}
/* 附件视图持久缓存：键 = 图像句柄 + 视图参数；纹理释放推进 G.rt_gen，过期槽位收回时旧视图进退休列表。
 * 表满不缓存（会话级；实际键空间 ~百级，1024 槽绰绰有余）。
 * Persistent attachment-view cache keyed by image handle + view parameters; a texture release
 * bumps G.rt_gen and stale slots retire their old view. A full table degrades to "create without
 * caching" (the real key space is in the hundreds; 1024 slots are plenty). */
static VkImageView attachment_view(const VkImageViewCreateInfo *info) {
    ImageViewKey key;memset(&key,0,sizeof key);
    key.image=info->image;key.flags=info->flags;key.type=info->viewType;key.format=info->format;
    key.components=info->components;key.range=info->subresourceRange;
    uint64_t h=14695981039346656037ULL;const uint8_t *b=(const uint8_t *)&key;
    for(size_t i=0;i<sizeof key;i++)h=(h^b[i])*1099511628211ULL;
    uint32_t slot=(uint32_t)h&1023;
    for(uint32_t p=0;p<16;p++){uint32_t i=(slot+p)&1023;
        if(G.rt_views[i].view==VK_NULL_HANDLE||G.rt_views[i].gen!=G.rt_gen){
            if(G.rt_views[i].view!=VK_NULL_HANDLE){
                if(G.nrt_dead==1024)die("rt dead list full",G.nrt_dead);
                G.rt_dead[G.nrt_dead++]=(RtDead){G.rt_views[i].view,VK_NULL_HANDLE};G.profile_rtv_stale++;}
            CHECK(vkCreateImageView(G.device,info,NULL,&G.rt_views[i].view));
            G.rt_views[i].key=key;G.rt_views[i].gen=G.rt_gen;G.profile_rtv_creates++;return G.rt_views[i].view;}
        if(!memcmp(&G.rt_views[i].key,&key,sizeof key)){G.profile_rtv_hits++;return G.rt_views[i].view;}}
    VkImageView view;CHECK(vkCreateImageView(G.device,info,NULL,&view));G.profile_rtv_creates++;return view;
}
static VkFramebuffer framebuffer_get(const FramebufferKey *key,VkRenderPass rp,uint32_t n,uint32_t width,uint32_t height,const VkImageView *views) {
    uint64_t h=14695981039346656037ULL;const uint8_t *b=(const uint8_t *)key;
    for(size_t i=0;i<sizeof *key;i++)h=(h^b[i])*1099511628211ULL;
    uint32_t slot=(uint32_t)h&1023;
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=rp,.attachmentCount=n,
        .pAttachments=views,.width=width,.height=height,.layers=1};
    for(uint32_t p=0;p<16;p++){uint32_t i=(slot+p)&1023;
        if(G.rt_fbs[i].fb==VK_NULL_HANDLE||G.rt_fbs[i].gen!=G.rt_gen){
            if(G.rt_fbs[i].fb!=VK_NULL_HANDLE){
                if(G.nrt_dead==1024)die("rt dead list full",G.nrt_dead);
                G.rt_dead[G.nrt_dead++]=(RtDead){VK_NULL_HANDLE,G.rt_fbs[i].fb};G.profile_rtf_stale++;}
            CHECK(vkCreateFramebuffer(G.device,&fi,NULL,&G.rt_fbs[i].fb));
            G.rt_fbs[i].key=*key;G.rt_fbs[i].gen=G.rt_gen;G.profile_rtf_creates++;return G.rt_fbs[i].fb;}
        if(!memcmp(&G.rt_fbs[i].key,key,sizeof *key)){G.profile_rtf_hits++;return G.rt_fbs[i].fb;}}
    VkFramebuffer fb;CHECK(vkCreateFramebuffer(G.device,&fi,NULL,&fb));G.profile_rtf_creates++;return fb;
}
static void rt_dead_flush(void) {
    /* 只在提交围栏之后调用：被替换的通道/视图对象可能仍被已提交的命令缓冲引用。
     * Only call after the submission fence: replaced objects may still be referenced by
     * already-submitted command buffers. */
    for(int i=0;i<G.nrt_dead;i++){
        if(G.rt_dead[i].fb)vkDestroyFramebuffer(G.device,G.rt_dead[i].fb,NULL);
        if(G.rt_dead[i].view)vkDestroyImageView(G.device,G.rt_dead[i].view,NULL);}
    G.nrt_dead=0;
}
static VkRenderPass render_begin(void) {
    p_set_rt *rt=&G.state.rt;if(rt->n<0 || rt->n>8 || (!rt->n&&!rt->depth))die("render targets",rt->n);
    int read_only=S.depth_sampled&&depth_read_only();
    /* OP_SET_RT and every transfer/layout barrier already end the pass.
     * Keep ordinary consecutive draws in the same attachment configuration.
     * Feedback passes retain the original explicit pass boundaries. */
    if(S.in_pass && !G.disable_pass_reuse && !S.feedback_flags && !S.pass_feedback_flags && read_only==S.pass_depth_read_only){
        /* 附件指针/层级在 pass 期间不会变（任何 RT 变更都会结束 pass），直接按缓存标脏，
           省掉每 draw 每附件的纹理表查找。 | Attachment pointers/levels cannot change during
           a pass (any RT change ends it); mark dirty from the cached list and skip the
           per-draw per-attachment texture-table lookups. */
        for(int k=0;k<S.pass_attach_n;k++){
            VTexture *t=(VTexture *)S.pass_attach_t[k];
            t->cpu_owned=0;tex_attach_dirty(t,S.pass_attach_level[k]);
        }
        return S.active_pass_rp;
    }
    uint64_t rp_t0=prof_now();
    pass_end();G.pass_side_effects=0;S.pass_attach_n=0;
    G.profile_rp_flush_ns+=prof_now()-rp_t0;uint64_t rp_t0b=prof_now();
    if(G.chunk_draws)sub_chunk_flush();
    /* 预重置遮挡查询前缀：帧首的第一次通道转换处（通道已关、无活动查询）一次性重置向前的全部槽位
       ——本帧内查询索引单调增长且远小于池容量，此后不再需要中途跨块重置，也就不再有"打断已积累
       多绘制量的通道"的 pass_end（实测 ~34 次/帧、每次打断的通道平均已有 ~28 个 draw；块只有
       32 槽，而每帧查询数 ~1100+）。上一个提交的查询结果在其 commit（fence+退休）后已消费，因此
       重置那些"上一帧用过"的槽位是安全的。懒路径保留兜底。 | Pre-reset the occlusion query prefix
       once per frame at its first pass transition (pass closed, no active query): reset all forward
       slots in a single command. Query indices are monotonic within a frame and far below the pool
       capacity, so no mid-pass block crossings are needed afterwards -- removing the pass breaks
       (~34 per frame, each breaking a pass that had already accumulated ~28 draws; blocks are only
       32 slots while a frame uses ~1100+ queries). The previous submission's query results were
       consumed at its commit (fence + retirement), so re-resetting those slots is safe. The lazy
       path stays as a fallback. */
    if(G.query_pre_reset&&G.enable_query_reset_range&&S.active_occlusion==UINT32_MAX&&S.occlusion_reset_count<LIMIT_WRITES){
        query_reset(S.occlusion,S.occlusion_reset_count,LIMIT_WRITES-S.occlusion_reset_count);S.occlusion_reset_count=LIMIT_WRITES;G.profile_query_preresets++; }
    G.profile_rp_chunk_ns+=prof_now()-rp_t0b;uint64_t rp_t1=prof_now();
    VkAttachmentDescription attachments[9];VkAttachmentReference color[8],depth;VkImageView views[9];uint32_t n=0,width=0,height=0;
    memset(attachments,0,sizeof(attachments));
    for(int i=0;i<rt->n;i++)color[i]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    for(int i=0;i<rt->n+(rt->depth?1:0);i++) {
        int is_depth=i==rt->n;const tex_t *source=is_depth?rt->depth:rt->color[i];
        /* NVN color slots may be unbound. Pack only real image attachments,
         * while keeping subpass references at their original output locations. */
        if(!source)continue;
        const view_t *v=is_depth?(rt->has_dview?&rt->dview:NULL):(rt->has_cview[i]?&rt->cview[i]:NULL);
        VkImageSubresourceRange range=attachment_range(source,v);
        VTexture *t=texture_get(source,0);uint32_t tw=t->width>>range.baseMipLevel,th=t->height>>range.baseMipLevel;if(!tw)tw=1;if(!th)th=1;
        if(G.present_count<1 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] RT_ATTACHMENT index=%d depth=%d serial=%llu extent=%u,%u base_extent=%u,%u level=%u layer=%u count=%d nvn_format=%u gpu_format=%u\n",i,is_depth,(unsigned long long)source->serial,tw,th,source->d.width,source->d.height,range.baseMipLevel,range.baseArrayLayer,rt->n,source->d.format,t->format);
        /* NVN permits differently sized attachments. Vulkan framebuffer extent
         * must fit every attachment; its render area is their intersection. */
        if(!width){width=tw;height=th;}else {if(tw<width)width=tw;if(th<height)height=th;}
        t->cpu_owned=0;tex_attach_dirty(t,range.baseMipLevel);
        if(S.pass_attach_n<9){S.pass_attach_t[S.pass_attach_n]=t;S.pass_attach_level[S.pass_attach_n]=range.baseMipLevel;S.pass_attach_n++;}
        VkImageLayout attachment_layout=is_depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if(is_depth&&S.depth_sampled&&depth_read_only())attachment_layout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        else if(S.feedback_flags&(is_depth?VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT:VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT))attachment_layout=VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        VkImageLayout layout;
        if(!is_depth&&texture_range_ok(t)&&range.levelCount<t->source.d.levels){texture_transition_levels(t,range.baseMipLevel,range.levelCount,attachment_layout);layout=attachment_layout;}
        else{texture_transition(t,attachment_layout);layout=t->layout;}
        attachments[n]=(VkAttachmentDescription){.format=t->format,.samples=VK_SAMPLE_COUNT_1_BIT,
           .loadOp=G.diag_loadstore>=2?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,
           .storeOp=G.diag_loadstore>=1?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE,
           .stencilLoadOp=G.diag_loadstore>=2?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,
           .stencilStoreOp=G.diag_loadstore>=1?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE,
           .initialLayout=layout,.finalLayout=layout};
        VkImageViewCreateInfo view={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=t->image,
            .viewType=VK_IMAGE_VIEW_TYPE_2D,.format=t->format,.subresourceRange=range};
        views[n]=G.fb_cache_enabled?attachment_view(&view):submission_image_view(&view);
        if(is_depth)depth=(VkAttachmentReference){n,layout};else color[i]=(VkAttachmentReference){n,layout};n++;
    }
    if(!n)die("render targets have no bound attachment",rt->n);
    G.profile_rp_attach_ns+=prof_now()-rp_t1;uint64_t rp_t2=prof_now();
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=rt->n,.pColorAttachments=color,
        .pDepthStencilAttachment=rt->depth?&depth:NULL};
    VkSubpassDependency deps[3]={
      {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       .dstStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
      {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,.dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
       .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT},
      {.srcSubpass=0,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
       .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
       .dstAccessMask=VK_ACCESS_SHADER_READ_BIT,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT|VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT}};
    VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=n,.pAttachments=attachments,
       .subpassCount=1,.pSubpasses=&sub,.dependencyCount=(G.diag_no_pass_deps||(G.precise_sync&&!S.feedback_flags))?0u:S.feedback_flags?3u:2u,.pDependencies=deps};
    uint64_t rp_k0=prof_now();memset(&S.pass_key,0,sizeof(S.pass_key));
    S.pass_key.count=n;S.pass_key.color_count=rt->n;S.pass_key.has_depth=rt->depth!=NULL;S.pass_key.dependency_count=ri.dependencyCount;
    memcpy(S.pass_key.attachments,attachments,n*sizeof(*attachments));
    memcpy(S.pass_key.color,color,rt->n*sizeof(*color));if(rt->depth)S.pass_key.depth=depth;
    memcpy(S.pass_key.dependencies,deps,ri.dependencyCount*sizeof(*deps));
    FramebufferKey key;memset(&key,0,sizeof key);key.pass=S.pass_key;key.width=width;key.height=height;memcpy(key.views,views,n*sizeof(*views));
    int index=0;
    if(!G.disable_resource_cache){
        for(;index<S.npass;index++)if(!memcmp(&S.framebuffer_keys[index],&key,sizeof key))break;
    }else index=S.npass;
    G.profile_rp_scan_ns+=prof_now()-rp_k0;uint64_t rp_k1=prof_now();
    VkRenderPass rp;VkFramebuffer fb;
    if(G.fb_cache_enabled){
        /* 持久帧缓冲路径：键（通道描述 + 视图句柄 + 尺寸）命中即复用，跨 epoch 有效。
         * Persistent framebuffer path: the key (pass description + view handles + extent) is
         * looked up across epochs, so steady-state frames stop recreating every framebuffer. */
        rp=render_pass_get(&S.pass_key,&ri);
        fb=framebuffer_get(&key,rp,n,width,height,views);
    }else{
        if(index==S.npass){
            if(S.npass==LIMIT_WRITES)die("render pass limit",S.npass);S.npass++;
            S.passes[index]=render_pass_get(&S.pass_key,&ri);
            VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=S.passes[index],.attachmentCount=n,
               .pAttachments=views,.width=width,.height=height,.layers=1};CHECK(vkCreateFramebuffer(G.device,&fi,NULL,&S.framebuffers[index]));
            S.framebuffer_keys[index]=key;G.profile_framebuffer_misses++;
        }else G.profile_framebuffer_hits++;
        rp=S.passes[index];fb=S.framebuffers[index];
    }
    G.profile_rp_create_ns+=prof_now()-rp_k1;
    G.profile_rp_key_ns+=prof_now()-rp_t2;uint64_t rp_t3=prof_now();
    S.active_pass_rp=rp;G.pass_draws0=G.draws;{const tex_t *f0=rt->n>0?rt->color[0]:rt->depth;snprintf(G.pass_desc,sizeof G.pass_desc,"%ux%u c%d d%d s%llu",f0?f0->d.width:0,f0?f0->d.height:0,rt->n,rt->depth!=NULL,(unsigned long long)(f0?f0->serial:0));}
    VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,.framebuffer=fb,
       .renderArea={{0,0},{(G.diag_shape&4)&&width>16?16:width,(G.diag_shape&4)&&height>16?16:height}}};
    if(S.pass_ts&&S.npt<500){uint32_t q=S.npt*2;query_reserve(S.pass_ts,&S.pass_reset_count,q,2,1024);vkCmdWriteTimestamp(S.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,S.pass_ts,q);
        S.pt[S.npt].q0=q;S.pt[S.npt].w=width;S.pt[S.npt].h=height;S.pt[S.npt].n=rt->n;
        S.pt[S.npt].key=(rt->color[0]?rt->color[0]->serial:0)*1315423911ULL^(rt->depth?rt->depth->serial:0);S.pt_draws0=G.draws;S.pt_open=1;}
    if(G.precise_sync)hz_pass_sync();
    vkCmdBeginRenderPass(S.command,&begin,VK_SUBPASS_CONTENTS_INLINE);S.in_pass=1;G.pass_count++;
    G.profile_rp_begin_ns+=prof_now()-rp_t3;
    S.pass_feedback_flags=S.feedback_flags;S.pass_depth_read_only=read_only;
    return rp;
}
static void trace_mesh_range(const char *name,uint64_t address,uint64_t bytes) {
    const char *root=getenv("DM_FRAME_DIR");if(!root||!bytes)return;
    if(bytes>16*1024*1024||!pool_find(address,bytes,NULL))die("mesh diagnostic range",bytes);
    char path[1400];snprintf(path,sizeof(path),"%s/mesh-%04llu-%llu-%s.bin",root,(unsigned long long)G.present_count+1,(unsigned long long)G.draws,name);
    FILE *out=fopen(path,"wb");if(!out)die("mesh diagnostic output",0);
    if(fwrite((const void *)(uintptr_t)address,1,bytes,out)!=bytes)die("mesh diagnostic write",bytes);fclose(out);
    fprintf(stderr,"[nvn-vulkan] MESH_GUEST_RANGE draw=%llu name=%s bytes=%llu path=%s\n",(unsigned long long)G.draws,name,(unsigned long long)bytes,path);
    uint64_t prefix=address&3ULL,span=(prefix+bytes+3)&~3ULL;
    uint64_t offset=0;VPool *pool=pool_read(address-prefix,span,&offset);VBuffer *capture=temporary(span,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    all_barrier();VkBufferCopy copy={offset,0,span};vkCmdCopyBuffer(S.command,pool->v.b,capture->b,1,&copy);
    char gpu_name[160];snprintf(gpu_name,sizeof(gpu_name),"mesh-%04llu-%llu-%s-gpu",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,name);trace_capture_queue(capture,bytes,gpu_name);
    S.trace_captures[S.ntrace_captures-1].offset=prefix;
}
static void command_bind_profile(unsigned kind,int issued) {
    if(issued)G.profile_cmd_issued[kind]++;else G.profile_cmd_skipped[kind]++;
}
static void draw(const p_draw *a,const p_draw_idx *idx) {
    if(!a&&!idx)die("draw parameters",0);
    /* 有意通道分割器（A/B 实验，passsplit.txt）：限制单通道 draw 数。预重置实验证明通道长度
     * 对这块 GPU 影响巨大（~100 draw 长通道 15.8fps vs ~32 draw 25fps），本档把"查询打断"
     * 的副产品分割变成可调参数。 | Intentional pass splitter (A/B, passsplit.txt): cap the draws
     * per pass. The pre-reset experiment showed pass length matters a lot on this GPU (~100-draw
     * passes 15.8fps vs ~32-draw 25fps); this turns the accidental query-break splitter into a knob. */
    if(G.pass_split_draws && S.in_pass && G.draws-G.pass_draws0>=(uint64_t)G.pass_split_draws){G.profile_pass_splits++;pass_end();}
    uint64_t dr_state=prof_now();
    int upper_left=atomic_load(&G.window_origin)==NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
    if(G.state.ms.samples>1 || G.state.ms.a2c || G.state.ms.cov_mod || G.state.ms.sl_en)die("multisample semantics",G.state.ms.samples);
    if(G.state.color.logic_op!=NVN_LOGIC_OP_COPY || G.state.color.alpha_func!=NVN_ALPHA_FUNC_ALWAYS)die("logic/alpha test adaptation",G.state.color.logic_op);
    if(G.state.polygon.mode!=NVN_POLYGON_MODE_FILL || G.state.polygon.offset_en)die("polygon mode/offset",G.state.polygon.mode);
    /* A zero-color pass may still run fragment discard, depth export or
     * storage side effects. Only a program with no fragment stage omits it. */
    VShader *vs=program_stage(0),*ps=program_stage_optional(1);
    if(G.pass_timing&&ps&&G.state.rt.n==1&&G.state.rt.color[0]&&G.state.rt.color[0]->d.width==1280&&G.state.rt.color[0]->d.height==720){
        uint32_t k=0;while(k<G.nhot&&strcmp(G.hot[k].key,ps->key))k++;
        if(k==G.nhot&&k<64){strcpy(G.hot[k].key,ps->key);G.hot[k].draws=0;G.nhot++;}
        if(k<G.nhot)G.hot[k].draws++;}
    if(G.skip_ps[0]&&ps&&!strcmp(ps->key,G.skip_ps))return;
    S.read_snapshots_allowed=1;
    for(unsigned stage=0;stage<2;stage++){VShader *s=stage?ps:vs;if(!s)continue;
        for(unsigned i=0;i<s->h.nb;i++){RBinding *b=&s->b[i];if(((b->kind==3||b->kind==9)&&b->flags)||b->kind==6||b->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)S.read_snapshots_allowed=0;}
    }
    if(!S.read_snapshots_allowed)G.pass_side_effects=1;
    G.draw_store=!S.read_snapshots_allowed;
    if(G.profile_detail){snprintf(G.last_ps,sizeof G.last_ps,"%s",ps?ps->key:"none");G.last_count=a?a->count:idx->count;}
    if(G.precise_sync&&G.draw_store){pass_end();hz_add(1u<<HZ_GV|1u<<HZ_GF,HZ_ATT_STAGES|HZ_SHADER_STAGES,HZ_ATT_ACCESS|VK_ACCESS_SHADER_WRITE_BIT);}
    if(G.trace_frame&&!G.trace_ui&&idx&&idx->count>=100&&G.trace_meshes<4&&!strcmp(vs->key,"54f730a6c0efc118")){
        G.trace_meshes++;for(unsigned i=0;i<16;i++)if(G.state.vbuf[i].size){char name[32];snprintf(name,sizeof(name),"vbuf%u",i);trace_mesh_range(name,G.state.vbuf[i].address,G.state.vbuf[i].size);}
        for(unsigned i=0;i<vs->h.nb;i++)if(vs->b[i].kind==2||vs->b[i].kind==9){int slot=vs->b[i].slot;char name[32];snprintf(name,sizeof(name),"ubo%d",slot);trace_mesh_range(name,G.state.uniform[0][slot].addr,G.state.uniform[0][slot].size);}
        trace_mesh_range("indices",idx->ibuf,(uint64_t)idx->count*(idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?2:4));
        fprintf(stderr,"[nvn-vulkan] MESH_INDEX draw=%llu type=%u base_vertex=%d base_instance=%u count=%d\n",(unsigned long long)G.draws,idx->itype,idx->base_vertex,idx->base_instance,idx->count);
    }
    if(G.trace_frame&&G.trace_ui&&ps&&!strcmp(ps->key,"62ddcb3faa41a92a")){
        for(unsigned stage=0;stage<2;stage++){VShader *s=stage?ps:vs;
            for(unsigned i=0;i<s->h.nb;i++)if(s->b[i].kind==2||s->b[i].kind==9){unsigned slot=s->b[i].slot;char name[48];snprintf(name,sizeof(name),"ui-%s-ubo%u",stage?"ps":"vs",slot);Range r=G.state.uniform[stage][slot];trace_mesh_range(name,r.addr,r.size);}}
        NVNbufferRange v=G.state.vbuf[0];trace_mesh_range("ui-vbuf0",v.address,v.size);
        blend_st b=G.state.blend[0];fprintf(stderr,"[nvn-vulkan] UI_BLEND draw=%llu enable=%u src=%u,%u dst=%u,%u equation=%u,%u channels=%#x\n",(unsigned long long)G.draws,G.state.color.blend_en,b.src_rgb,b.src_a,b.dst_rgb,b.dst_a,b.eq_rgb,b.eq_a,G.state.mask.mask);
    }
    if(G.state.rt.n&&!ps)die("color draw program fragment stage missing",1);
    if(G.draws<9 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_TRACE draw=%llu vs=%s ps=%s count=%d instances=%d rt=%llu viewport=%g,%g,%g,%g scissor=%d,%d,%u,%u cull=%u front=%u depth=%u,%u,%u blend=%u mask=%#x\n",
        (unsigned long long)G.draws,vs->key,ps?ps->key:"none",a?a->count:idx->count,a?1:idx->instances,
        (unsigned long long)(G.state.rt.n&&G.state.rt.color[0]?G.state.rt.color[0]->serial:0),G.state.viewport.x,G.state.viewport.y,G.state.viewport.width,G.state.viewport.height,
        G.state.scissor.offset.x,G.state.scissor.offset.y,G.state.scissor.extent.width,G.state.scissor.extent.height,
        G.state.polygon.cull,G.state.polygon.front,G.state.ds.dt,G.state.ds.dw,G.state.ds.df,G.state.color.blend_en,G.state.mask.mask);
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_DEPTH_STATE draw=%llu range=%g,%g stencil=%u func=%u,%u masks=%#x,%#x write=%#x,%#x ref=%#x,%#x ops=%u,%u,%u/%u,%u,%u\n",
        (unsigned long long)G.draws,G.state.viewport.minDepth,G.state.viewport.maxDepth,G.state.ds.st,G.state.ds.sf_f,G.state.ds.sf_b,
        G.state.stencil_compare[0],G.state.stencil_compare[1],G.state.stencil_write[0],G.state.stencil_write[1],G.state.stencil_ref[0],G.state.stencil_ref[1],
        G.state.ds.fail_f,G.state.ds.zfail_f,G.state.ds.zpass_f,G.state.ds.fail_b,G.state.ds.zfail_b,G.state.ds.zpass_b);
    S.feedback_flags=0;S.depth_sampled=0;S.nimage_uses=0;collect_image_uses(vs);collect_image_uses(ps);
    G.xition_vertex=0;for(unsigned i=0;i<vs->h.nb;i++)if(vs->b[i].kind==4||vs->b[i].kind==6)G.xition_vertex=1;
    G.profile_state_ns+=prof_now()-dr_state;
    uint64_t bind_started=prof_now();
    prepare_sampling_copies(vs);prepare_sampling_copies(ps);prepare_comparison_images(vs);prepare_comparison_images(ps);
    G.profile_prep_ns+=prof_now()-bind_started;
    VkDescriptorSet descriptor_sets[2]={bind_shader(vs),ps?bind_shader(ps):VK_NULL_HANDLE};
    if(G.diag_fixed_desc){static VkDescriptorSet last_sets[2];static VShader *last_vs,*last_ps;static uint64_t last_epoch;
        if(last_epoch==S.epoch&&last_vs==vs&&last_ps==ps){descriptor_sets[0]=last_sets[0];descriptor_sets[1]=last_sets[1];}
        else{last_epoch=S.epoch;last_vs=vs;last_ps=ps;last_sets[0]=descriptor_sets[0];last_sets[1]=descriptor_sets[1];}}
    G.profile_bind_ns+=prof_now()-bind_started;
    if(G.profile_detail){int n=0;G.last_uses[0]=0;for(unsigned u=0;u<S.nimage_uses&&n<90;u++)n+=snprintf(G.last_uses+n,sizeof G.last_uses-n,"%llu%s ",(unsigned long long)S.image_uses[u].serial,(S.image_uses[u].roles&2)?"w":"");}
    G.xition_vertex=0;
    VkDescriptorSetLayout set_layouts[2]={vs->layout,ps?ps->layout:VK_NULL_HANDLE};
    if(S.active_occlusion==UINT32_MAX)query_reserve(S.occlusion,&S.occlusion_reset_count,S.nocc,1,LIMIT_WRITES);
    uint64_t render_started=prof_now();VkRenderPass renderpass=render_begin();G.profile_render_ns+=prof_now()-render_started;
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=ps?2u:1u,.pSetLayouts=set_layouts};
    uint64_t vertex_started=prof_now();
    VkVertexInputAttributeDescription attributes[32];VkVertexInputBindingDescription bindings[16];uint32_t nbindings=0;uint8_t used[16]={0};
    for(uint32_t i=0;i<vs->h.na;i++) { unsigned location=vs->attr[i][0];if(location>=16)die("vertex attribute count",location);
        va_st v=G.state.attributes[location];if(v.stream>=16)die("vertex stream",v.stream);
        if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] VERTEX_INPUT draw=%llu location=%u shader_type=%u stream=%u format=%u offset=%u stride=%u divisor=%u address=%#llx size=%llu\n",(unsigned long long)G.draws,location,vs->attr[i][1],v.stream,v.fmt,v.offset,G.state.streams[v.stream].stride,G.state.streams[v.stream].divisor,(unsigned long long)G.state.vbuf[v.stream].address,(unsigned long long)G.state.vbuf[v.stream].size);
        attributes[i]=(VkVertexInputAttributeDescription){location,v.stream,format(v.fmt),v.offset};used[v.stream]=1; }
    for(unsigned i=0;i<16;i++)if(used[i]) { vs_st v=G.state.streams[i];if(v.divisor>1)die("vertex divisor",v.divisor);
        bindings[nbindings++]=(VkVertexInputBindingDescription){i,v.stride,v.divisor?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};
        NVNbufferRange r=G.state.vbuf[i];if(!r.address || !r.size)die("unbound vertex stream",i);uint64_t offset=0;VPool *p=pool_read_memo(r.address,r.size,&offset);
        command_bind_profile(NVN_VK_CMD_VERTEX,nvn_vk_cmd_vertex(&S.command_state,G.enable_command_cache&&!G.trace_frame,S.command,i,p->v.b,offset)); }
    G.profile_vertex_ns+=prof_now()-vertex_started;
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=nbindings,
        .pVertexBindingDescriptions=bindings,.vertexAttributeDescriptionCount=vs->h.na,.pVertexAttributeDescriptions=attributes};
    VkPipelineInputAssemblyStateCreateInfo ai={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology=topology(a?a->prim:idx->prim)};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,
       .cullMode=G.state.polygon.cull==NVN_FACE_FRONT?VK_CULL_MODE_FRONT_BIT:G.state.polygon.cull==NVN_FACE_BACK?VK_CULL_MODE_BACK_BIT:
          G.state.polygon.cull==NVN_FACE_FRONT_AND_BACK?VK_CULL_MODE_FRONT_AND_BACK:VK_CULL_MODE_NONE,
       // An upper-left NVN window retains NDC y upwards; a negative Vulkan
       // viewport restores that mapping. Facing follows the same inversion.
       .frontFace=(G.state.polygon.front==NVN_FRONT_FACE_CCW)==upper_left?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=G.state.ds.dt,
        .depthWriteEnable=G.state.ds.dw,.depthCompareOp=(VkCompareOp)(G.state.ds.df-1),.stencilTestEnable=G.state.ds.st};
    ds.front=(VkStencilOpState){.failOp=(VkStencilOp)(G.state.ds.fail_f-1),.passOp=(VkStencilOp)(G.state.ds.zpass_f-1),
       .depthFailOp=(VkStencilOp)(G.state.ds.zfail_f-1),.compareOp=(VkCompareOp)(G.state.ds.sf_f-1),
       .compareMask=G.state.stencil_compare[0],.writeMask=G.state.stencil_write[0],.reference=G.state.stencil_ref[0]};
    ds.back=(VkStencilOpState){.failOp=(VkStencilOp)(G.state.ds.fail_b-1),.passOp=(VkStencilOp)(G.state.ds.zpass_b-1),
       .depthFailOp=(VkStencilOp)(G.state.ds.zfail_b-1),.compareOp=(VkCompareOp)(G.state.ds.sf_b-1),
       .compareMask=G.state.stencil_compare[1],.writeMask=G.state.stencil_write[1],.reference=G.state.stencil_ref[1]};
    VkPipelineColorBlendAttachmentState blends[8];memset(blends,0,sizeof(blends));
    for(int i=0;i<G.state.rt.n;i++) {blend_st b=G.state.blend[i];
        if(!G.state.rt.color[i])continue;
        blends[i].colorWriteMask=(G.state.mask.mask>>(4*i))&15;
        if(G.state.color.blend_en&(1u<<i)) {blends[i].blendEnable=1;blends[i].srcColorBlendFactor=blend_factor(b.src_rgb);
           blends[i].dstColorBlendFactor=blend_factor(b.dst_rgb);blends[i].srcAlphaBlendFactor=blend_factor(b.src_a);
           blends[i].dstAlphaBlendFactor=blend_factor(b.dst_a);blends[i].colorBlendOp=blend_op(b.eq_rgb);blends[i].alphaBlendOp=blend_op(b.eq_a);} }
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=G.state.rt.n,.pAttachments=blends};
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=3,.pDynamicStates=dynamic};
    VkPipelineShaderStageCreateInfo stages[2]={
       {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs->module,.pName="main"},
       {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=ps?ps->module:VK_NULL_HANDLE,.pName="main"}};
    VkSpecializationMapEntry bias_maps[2][LIMIT_BINDINGS];VkSpecializationInfo bias[2];
    bias[0]=storage_specialization(vs,bias_maps[0]);if(bias[0].mapEntryCount)stages[0].pSpecializationInfo=&bias[0];
    if(ps){bias[1]=storage_specialization(ps,bias_maps[1]);if(bias[1].mapEntryCount)stages[1].pSpecializationInfo=&bias[1];}
    VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.flags=S.feedback_flags,.stageCount=ps?2u:1u,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ai,.pViewportState=&vp,.pRasterizationState=&raster,.pMultisampleState=&ms,
        .pDepthStencilState=&ds,.pColorBlendState=&cb,.pDynamicState=&dy,.renderPass=renderpass};
    uint64_t pso_started=prof_now();
    PsoKey key;memset(&key,0,sizeof(key));
    key.shader[0]=(uint32_t)(vs-G.shaders)+1;key.shader[1]=ps?(uint32_t)(ps-G.shaders)+1:0;
    memcpy(key.storage_bias[0],S.storage_bias[0],sizeof key.storage_bias[0]);if(ps)memcpy(key.storage_bias[1],S.storage_bias[1],sizeof key.storage_bias[1]);
    key.flags=gp.flags;key.topology=ai.topology;key.cull=raster.cullMode;key.front=raster.frontFace;
    key.attribute_count=vs->h.na;key.binding_count=nbindings;key.color_count=cb.attachmentCount;
    memcpy(key.attributes,attributes,vs->h.na*sizeof(*attributes));memcpy(key.bindings,bindings,nbindings*sizeof(*bindings));
    key.depth_test=ds.depthTestEnable;key.depth_write=ds.depthWriteEnable;key.depth_compare=ds.depthCompareOp;key.stencil_test=ds.stencilTestEnable;
    key.stencil[0]=ds.front;key.stencil[1]=ds.back;memcpy(key.blend,blends,cb.attachmentCount*sizeof(*blends));key.pass=S.pass_key;
    uint64_t hash=0;Pso pso,*cached=pso_lookup(&key,&hash);
    if(cached)pso=*cached;
    else{
        uint64_t pipeline_start=dm_android_nvn_clock_ns();CHECK(vkCreatePipelineLayout(G.device,&li,NULL,&pso.layout));gp.layout=pso.layout;
        CHECK(vkCreateGraphicsPipelines(G.device,G.pipeline_cache,1,&gp,NULL,&pso.pipeline));{uint64_t pns=dm_android_nvn_clock_ns()-pipeline_start;G.profile_pipeline_ns+=pns;g_pso_cache_dirty=1;pso_create_account("gfx",pns,hash,G.present_count+1);}
        pso=pso_remember(&key,hash,pso);
    }
    G.profile_pso_ns+=prof_now()-pso_started;
    uint64_t tail_started=prof_now();
    int cache_commands=G.enable_command_cache&&!G.trace_frame;
    command_bind_profile(NVN_VK_CMD_PIPELINE,nvn_vk_cmd_pipeline(&S.command_state,cache_commands,S.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pso.pipeline));
    {uint32_t draw_dyn[2*LIMIT_BINDINGS];uint32_t ndyn=S.dyn_count[vs->h.stage];
     memcpy(draw_dyn,S.dyn_offset[vs->h.stage],ndyn*sizeof(*draw_dyn));
     if(ps){memcpy(draw_dyn+ndyn,S.dyn_offset[ps->h.stage],S.dyn_count[ps->h.stage]*sizeof(*draw_dyn));ndyn+=S.dyn_count[ps->h.stage];}
     command_bind_profile(NVN_VK_CMD_DESCRIPTORS,nvn_vk_cmd_descriptors(&S.command_state,cache_commands,S.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pso.layout,ps?2:1,descriptor_sets,ndyn,draw_dyn));}
    G.profile_tail_bind_ns+=prof_now()-tail_started;uint64_t tail_s2=prof_now();
    if(G.state.viewport.width<=0 || G.state.viewport.height<=0)die("viewport bounds",0);
    VkViewport viewport=G.state.viewport;if(upper_left){viewport.y+=viewport.height;viewport.height=-viewport.height;}
    command_bind_profile(NVN_VK_CMD_VIEWPORT,nvn_vk_cmd_viewport(&S.command_state,cache_commands,S.command,&viewport));
    command_bind_profile(NVN_VK_CMD_SCISSOR,nvn_vk_cmd_scissor(&S.command_state,cache_commands,S.command,(G.diag_shape&1)?&(VkRect2D){{0,0},{1,1}}:&G.state.scissor));
    command_bind_profile(NVN_VK_CMD_BLEND,nvn_vk_cmd_blend(&S.command_state,cache_commands,S.command,G.state.blend_color));
    /* Precise sample counts add across drawing commands. Keep one query open
     * through consecutive draws, closing it at pass/counter boundaries. This
     * preserves every NVN counter value without serializing each draw on tile
     * GPUs. Frame traces retain the original per-draw query diagnostics. */
    if(S.active_occlusion==UINT32_MAX&&!G.diag_no_occlusion){
        if(!G.precise_occlusion || S.nocc==LIMIT_WRITES)die("precise occlusion query support/count",S.nocc);
        S.active_occlusion=S.nocc++;G.profile_occlusion_queries++;
        vkCmdBeginQuery(S.command,S.occlusion,S.active_occlusion,G.diag_imprecise_occlusion?0:VK_QUERY_CONTROL_PRECISE_BIT);
    }
    G.profile_tail_state_ns+=prof_now()-tail_s2;uint64_t tail_s3=prof_now();
    uint32_t query=S.active_occlusion;
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] DRAW_QUERY draw=%llu query=%u\n",(unsigned long long)G.draws,query);
    int skip_class=0;
    if(G.diag_skip_class){const tex_t *first=G.state.rt.n>0?G.state.rt.color[0]:G.state.rt.depth;unsigned w=first?first->d.width:0,h=first?first->d.height:0;
        int cls=(w==2100&&h==2100)?1:(w==960&&h==540)?(G.state.rt.n==0?2:G.state.rt.n==1?4:8):16;skip_class=(G.diag_skip_class&cls)!=0;}
    if(G.diag_skip_draws||skip_class){}
    else if(a) {if(a->count<0 || a->first<0)die("draw arrays bounds",a->count);vkCmdDraw(S.command,(G.diag_shape&2)&&a->count>3?3:a->count,1,a->first,0);}
    else { if(idx->count<0 || idx->instances<0 || (idx->itype!=NVN_INDEX_TYPE_UNSIGNED_SHORT && idx->itype!=NVN_INDEX_TYPE_UNSIGNED_INT))die("index type/bounds",idx->itype);
        uint64_t idx_t0=prof_now();
        uint64_t offset=0,size=(uint64_t)idx->count*(idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?2:4);VPool *p=pool_read_memo(idx->ibuf,size,&offset);
        command_bind_profile(NVN_VK_CMD_INDEX,nvn_vk_cmd_index(&S.command_state,cache_commands,S.command,p->v.b,offset,idx->itype==NVN_INDEX_TYPE_UNSIGNED_SHORT?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32));
        G.profile_tail_idx_ns+=prof_now()-idx_t0;
        vkCmdDrawIndexed(S.command,(G.diag_shape&2)&&idx->count>3?3:idx->count,idx->instances,0,idx->base_vertex,idx->base_instance); }
    if(G.disable_query_batch||G.trace_frame)occlusion_end();
    G.profile_tail_draw_ns+=prof_now()-tail_s3;
    if(G.trace_frame&&G.trace_ui&&ps&&!strcmp(ps->key,"62ddcb3faa41a92a")&&G.state.rt.n==1){
        VTexture *t=texture_get(G.state.rt.color[0],0);if(t->bpp!=4||aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)die("UI result capture format",t->format);
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);uint64_t bytes=(uint64_t)t->width*t->height*4;
        VBuffer *capture=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
        vkCmdCopyImageToBuffer(S.command,t->image,t->layout,capture->b,1,&copy);char name[160];snprintf(name,sizeof(name),"ui-result-%04llu-%llu-%ux%u",(unsigned long long)G.present_count+1,(unsigned long long)G.draws,t->width,t->height);trace_capture_queue(capture,bytes,name);
    }
    G.profile_tail_ns+=prof_now()-tail_started;
    G.draws++;aligned_writeback();
}
/* ── 3D 画面加亮（调色表重写；与 nvn_mtl.m 的 bright_lut 同构）─────────────────────────────
 * 游戏的色调映射用一张 32³ 调色表（每帧由计算着色器 552f45c348a61b86 重算；本文件里键相同）。
 * 在它算完之后把表复制到临时图并原地重写 L'(u) = L(PQ(k·PQ⁻¹(u)))，加高光保护——等价于给 3D
 * 世界乘曝光 k；UI / 文字在色调映射之后合成，不受影响。k 由宿主 dm_bright_k()（dm_bright.c）
 * 每帧给；k≈1 什么都不做。认表失败只报一次红并不加亮（安全降级）。
 * 裸 vkCmd* 会打乱命令状态去重缓存：写完重置它，让后续绑定全部重发。
 * 3D picture brightening (palette-table rewrite; isomorphic to nvn_mtl.m's bright_lut): the game's
 * tone mapping uses a 32^3 palette table recomputed every frame by compute shader 552f45c348a61b86
 * (same key in this file). Right after that dispatch the table is copied to a temp image and
 * rewritten in place as L'(u) = L(PQ(k * PQ^-1(u))) with highlight protection -- equivalent to
 * multiplying the 3D world by exposure k; UI / text are composited after tone mapping and are
 * unaffected. k comes from the host's dm_bright_k() (dm_bright.c) every frame; k ~= 1 does nothing.
 * A failed table identification logs once and does not brighten (safe fallback). Raw vkCmd* calls
 * would desync the command-state dedupe cache: reset it afterwards so later binds re-emit. */
__attribute__((weak)) float dm_bright_k(void);
typedef struct {
    int failed, ready; VkFormat fmt; VkImageLayout tmp_layout;
    VkPipeline pipeline; VkPipelineLayout play; VkDescriptorSetLayout slay;
    VkDescriptorPool pool; VkDescriptorSet set; VkSampler sampler;
    VkImage tmp; VkImageView tmp_view; VkDeviceMemory tmp_mem;
} BrightLut;
static BrightLut g_bright;
static void bright_tmp_transition(VkImageLayout layout) {
    if (g_bright.tmp_layout == layout) return;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = g_bright.tmp_layout, .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .image = g_bright.tmp, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(S.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    g_bright.tmp_layout = layout;
}
static void bright_lut_after_dispatch(VShader *s) {
    if (g_bright.failed || strncmp(s->key, "552f45c348a61b86", 16)) return;
    float k = dm_bright_k ? dm_bright_k() : 1.0f;
    if (!(k > 1.005f)) return;
    if (k > 4.0f) k = 4.0f;
    VTexture *t = NULL;
    for (unsigned i = 0; i < s->h.nb && !t; i++) {
        RBinding *b = &s->b[i];
        if (b->kind != 6 || b->slot < 0 || b->slot >= 32) continue;
        unsigned id = (unsigned)G.state.img[s->h.stage][b->slot] & 0xfffff;
        if (id >= (unsigned)G.texture_pool_n || id >= G.texture_slots || !G.tslot[id].live) continue;
        VTexture *c = slot_texture(&G.tslot[id], 1);
        if (c && c->source.d.target == NVN_TEXTURE_TARGET_3D && c->width == 32 && c->height == 32 && c->source.d.depth == 32) t = c;
    }
    if (!t) { g_bright.failed = 1; fprintf(stderr, "[nvn-vulkan] 【红】3D 画面加亮：调色表认不出，不加亮\n"); return; }
    int variant = t->format == VK_FORMAT_R8G8B8A8_UNORM ? 0 : t->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 1 : -1;
    if (variant < 0) { g_bright.failed = 1; fprintf(stderr, "[nvn-vulkan] 【红】3D 画面加亮：调色表格式 %d 不认识（只要 rgba8/rgba16f），不加亮\n", (int)t->format); return; }
    if (!g_bright.ready || g_bright.fmt != t->format) {
        if (g_bright.ready) { g_bright.failed = 1; fprintf(stderr, "[nvn-vulkan] 【红】3D 画面加亮：调色表格式变了，不再加亮\n"); return; }
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D, .format = t->format,
            .extent = {32, 32, 32}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        CHECK(vkCreateImage(G.device, &ii, NULL, &g_bright.tmp)); VkMemoryRequirements r; vkGetImageMemoryRequirements(G.device, g_bright.tmp, &r);
        VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
            .memoryTypeIndex = memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(G.device, &ma, NULL, &g_bright.tmp_mem)); CHECK(vkBindImageMemory(G.device, g_bright.tmp, g_bright.tmp_mem, 0));
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_bright.tmp, .viewType = VK_IMAGE_VIEW_TYPE_3D,
            .format = t->format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(G.device, &vi, NULL, &g_bright.tmp_view));
        VkSamplerCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
        CHECK(vkCreateSampler(G.device, &sc, NULL, &g_bright.sampler));
        VkDescriptorSetLayoutBinding db[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = db};
        CHECK(vkCreateDescriptorSetLayout(G.device, &dl, NULL, &g_bright.slay));
        VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_bright.slay,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        CHECK(vkCreatePipelineLayout(G.device, &pl, NULL, &g_bright.play));
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
        VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps};
        CHECK(vkCreateDescriptorPool(G.device, &dp, NULL, &g_bright.pool));
        VkDescriptorSetAllocateInfo da = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_bright.pool,
            .descriptorSetCount = 1, .pSetLayouts = &g_bright.slay};
        CHECK(vkAllocateDescriptorSets(G.device, &da, &g_bright.set));
        const uint32_t *spv = variant ? k_lutk_spv_rgba16f : k_lutk_spv_rgba8;
        size_t spv_bytes = variant ? sizeof k_lutk_spv_rgba16f : sizeof k_lutk_spv_rgba8;
        VkShaderModuleCreateInfo sm = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = spv_bytes, .pCode = spv};
        VkShaderModule mod; CHECK(vkCreateShaderModule(G.device, &sm, NULL, &mod));
        VkComputePipelineCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"},
            .layout = g_bright.play};
        CHECK(vkCreateComputePipelines(G.device, G.pipeline_cache, 1, &cp, NULL, &g_bright.pipeline));
        vkDestroyShaderModule(G.device, mod, NULL);
        g_bright.fmt = t->format; g_pso_cache_dirty = 1;
        VkDescriptorImageInfo src = {g_bright.sampler, g_bright.tmp_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo dst = {VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_bright.set, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &src},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_bright.set, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dst}};
        vkUpdateDescriptorSets(G.device, 2, w, 0, NULL);
        g_bright.ready = 1; g_bright.tmp_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        fprintf(stderr, "[nvn-vulkan] 3D 画面加亮：建好（调色表 32x32x32 格式 %d）\n", (int)t->format);
    }
    texture_transition(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    bright_tmp_transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {32, 32, 32}};
    vkCmdCopyImage(S.command, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_bright.tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    bright_tmp_transition(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture_transition(t, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(S.command, VK_PIPELINE_BIND_POINT_COMPUTE, g_bright.pipeline);
    vkCmdBindDescriptorSets(S.command, VK_PIPELINE_BIND_POINT_COMPUTE, g_bright.play, 0, 1, &g_bright.set, 0, NULL);
    vkCmdPushConstants(S.command, g_bright.play, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &k);
    vkCmdDispatch(S.command, 8, 8, 8);
    nvn_vk_command_state_reset(&S.command_state); S.command_dirty = 1;   // 裸绑定已改状态：去重缓存重置 | raw binds changed the state: reset the dedupe cache
}
static void dispatch(const p_dispatch *p) {
    S.read_snapshots_allowed=0;
    pass_end();if(p->x<0 || p->y<0 || p->z<0)die("dispatch dimensions",p->x);VShader *s=program_stage(5);
    S.nimage_uses=0;collect_image_uses(s);prepare_comparison_images(s);
    VkDescriptorSet set=bind_shader(s);VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&s->layout};
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=s->module,.pName="main"}};
    VkSpecializationMapEntry bias_map[LIMIT_BINDINGS];VkSpecializationInfo bias=storage_specialization(s,bias_map);if(bias.mapEntryCount)ci.stage.pSpecializationInfo=&bias;
    PsoKey key;memset(&key,0,sizeof(key));key.compute=1;key.shader[0]=(uint32_t)(s-G.shaders)+1;
    memcpy(key.storage_bias[0],S.storage_bias[5],sizeof key.storage_bias[0]);
    uint64_t hash=0;Pso pso,*cached=pso_lookup(&key,&hash);
    if(cached)pso=*cached;
    else{
        uint64_t pipeline_start=dm_android_nvn_clock_ns();CHECK(vkCreatePipelineLayout(G.device,&li,NULL,&pso.layout));ci.layout=pso.layout;
        CHECK(vkCreateComputePipelines(G.device,G.pipeline_cache,1,&ci,NULL,&pso.pipeline));{uint64_t pns=dm_android_nvn_clock_ns()-pipeline_start;G.profile_pipeline_ns+=pns;g_pso_cache_dirty=1;pso_create_account("compute",pns,hash,G.present_count+1);}
        pso=pso_remember(&key,hash,pso);
    }
    int cache_commands=G.enable_command_cache&&!G.trace_frame;
    command_bind_profile(NVN_VK_CMD_PIPELINE,nvn_vk_cmd_pipeline(&S.command_state,cache_commands,S.command,VK_PIPELINE_BIND_POINT_COMPUTE,pso.pipeline));
    command_bind_profile(NVN_VK_CMD_DESCRIPTORS,nvn_vk_cmd_descriptors(&S.command_state,cache_commands,S.command,VK_PIPELINE_BIND_POINT_COMPUTE,pso.layout,1,&set,S.dyn_count[s->h.stage],S.dyn_offset[s->h.stage]));
    if(G.precise_sync)hz_sync(HZ_CS,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_UNIFORM_READ_BIT);
    vkCmdDispatch(S.command,p->x,p->y,p->z);
    bright_lut_after_dispatch(s);
    aligned_writeback();
}

static void copy_b2t(const p_copy_b2t *p) {
    if(p->flags || p->r.zoffset<0 || p->r.depth<1 || p->r.xoffset<0 || p->r.yoffset<0 || p->r.width<0 || p->r.height<0)
       die("buffer-to-texture flags/view/region",p->flags);
    unsigned level=p->has_view&&p->view.set_levels?(unsigned)p->view.base_level:0;
    int volume=p->tex->d.target==NVN_TEXTURE_TARGET_3D;
    unsigned layer=(p->has_view&&p->view.set_layers?(unsigned)p->view.min_layer:0)+(unsigned)p->r.zoffset;
    unsigned available=volume?(p->tex->d.depth>>level):p->tex->L.layers;if(!available)available=1;
    if(layer>=available || (unsigned)p->r.depth>available-layer) {
        fprintf(stderr,"[nvn-vulkan] b2t target=%d extent=%u,%u,%u layers=%u region=%d,%d,%d,%d,%d,%d view_flags=%u,%u levels=%d,%d layers=%d,%d\n",
            p->tex->d.target,p->tex->d.width,p->tex->d.height,p->tex->d.depth,p->tex->L.layers,
            p->r.xoffset,p->r.yoffset,p->r.zoffset,p->r.width,p->r.height,p->r.depth,p->view.set_levels,p->view.set_layers,
            p->view.base_level,p->view.nlevels,p->view.min_layer,p->view.nlayers);
        die("buffer-to-texture layer range",layer);
    }
    if(level>=p->tex->d.levels || (p->has_view &&
       ((p->view.set_format&&p->view.format!=p->tex->d.format) ||
        (p->view.set_target&&p->view.target!=p->tex->d.target))))die("buffer-to-texture view range/format/target",level);
    uint32_t pf;int bb;int bc=nvn_bc_unpacked(p->tex->d.format,&pf,&bb);
    /* A compressed texture can already contain streamed CPU mip data when a
     * transfer updates only its tail. Preserve the other levels from its NVN
     * backing on first materialization, just as copy_t2t does. */
    VTexture *t=texture_get(p->tex,bc);uint64_t row=G.state.copy_stride?G.state.copy_stride:(uint64_t)p->r.width*t->bpp;
    unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
    if(bc) {
        int bw,bh;int block=nvn_fmt_bpb(p->tex->d.format,&bw,&bh);
        row=G.state.copy_stride?G.state.copy_stride:(uint64_t)((p->r.width+bw-1)/bw)*block;
        uint64_t rows=(p->r.height+bh-1)/bh,size=row*rows*p->r.depth;
        if(!block || row<(uint64_t)((p->r.width+bw-1)/bw)*block ||
           (uint64_t)p->r.xoffset+p->r.width>width || (uint64_t)p->r.yoffset+p->r.height>height)
            die("compressed buffer-to-texture range/stride",row);
        for(int i=0;i<S.nwrites;++i)if(overlap(p->src,size,S.writes[i].addr,S.writes[i].size))
            die("compressed decode requires completed GPU source write",p->src);
        if(!pool_find(p->src,size,NULL))die("compressed decode source range",p->src);
        /* No queued GPU write overlaps this range (checked above), so the
         * compressed source bytes are authoritative in guest CPU memory. */
        const uint8_t *compressed=(const void *)(uintptr_t)p->src;
        uint64_t image_bytes=(uint64_t)p->r.width*p->r.height*t->bpp;
        VBuffer *staging=temporary(image_bytes*p->r.depth,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for(int z=0;z<p->r.depth;++z)if(nvn_bc_decode(p->tex->d.format,compressed+z*row*rows,row,p->r.width,p->r.height,staging->map+z*image_bytes,(size_t)p->r.width*t->bpp))
            die("buffer-to-texture BC decode",p->tex->d.format);
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c={.imageSubresource={aspect(t->format),level,volume?0:layer,volume?1:p->r.depth},
            .imageOffset={p->r.xoffset,p->r.yoffset,volume?p->r.zoffset:0},.imageExtent={p->r.width,p->r.height,volume?p->r.depth:1}};
        vkCmdCopyBufferToImage(S.command,staging->b,t->image,t->layout,1,&c);t->cpu_owned=0;tex_dirty(t);return;
    }
    if(row%t->bpp || row<(uint64_t)p->r.width*t->bpp || (uint64_t)p->r.xoffset+p->r.width>width || (uint64_t)p->r.yoffset+p->r.height>height)
       die("buffer-to-texture extent/stride",row);
    uint64_t offset=0;VPool *src=pool_read(p->src,row*p->r.height*p->r.depth,&offset);all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c={.bufferOffset=offset,.bufferRowLength=(uint32_t)(row/t->bpp),.imageSubresource={aspect(t->format),level,volume?0:layer,volume?1:p->r.depth},
       .imageOffset={p->r.xoffset,p->r.yoffset,volume?p->r.zoffset:0},.imageExtent={p->r.width,p->r.height,volume?p->r.depth:1}};
    vkCmdCopyBufferToImage(S.command,src->v.b,t->image,t->layout,1,&c);t->cpu_owned=0;tex_dirty(t);
}
static void clear_color(const p_clear_color *p) {
    if(p->mask!=15 || p->index<0 || p->index>=G.state.rt.n)die("clear-color target/mask/view",p->mask);
    VTexture *t=texture_get(G.state.rt.color[p->index],0);VkRect2D s=G.state.scissor;
    VkImageSubresourceRange range=attachment_range(&t->source,G.state.rt.has_cview[p->index]?&G.state.rt.cview[p->index]:NULL);
    unsigned w=t->width>>range.baseMipLevel,h=t->height>>range.baseMipLevel;if(!w)w=1;if(!h)h=1;
    if(s.offset.x || s.offset.y || (s.extent.width && s.extent.width!=w) || (s.extent.height && s.extent.height!=h)) {
        if(s.offset.x<0 || s.offset.y<0)die("clear scissor negative",p->index);
        if((unsigned)s.offset.x>=w || (unsigned)s.offset.y>=h)return;
        if(s.extent.width>w-s.offset.x)s.extent.width=w-s.offset.x;if(s.extent.height>h-s.offset.y)s.extent.height=h-s.offset.y;
        render_begin();VkClearAttachment a={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.colorAttachment=p->index};memcpy(a.clearValue.color.float32,p->rgba,16);
        VkClearRect r={s,0,1};vkCmdClearAttachments(S.command,1,&a,1,&r);t->cpu_owned=0;tex_dirty(t);G.profile_scissor_clears++;return;
    }
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearColorValue color;memcpy(color.float32,p->rgba,sizeof(p->rgba));
    vkCmdClearColorImage(S.command,t->image,t->layout,&color,1,&range);t->cpu_owned=0;tex_dirty(t);G.profile_full_clears++;
}
static void clear_ds(const p_clear_ds *p) {
    if(G.trace_frame)fprintf(stderr,"[nvn-vulkan] CLEAR_DS depth=%g depth_mask=%#x stencil=%#x stencil_mask=%#x serial=%llu\n",p->depth,p->depth_mask,p->stencil,p->stencil_mask,
        (unsigned long long)(G.state.rt.depth?G.state.rt.depth->serial:0));
    if(!G.state.rt.depth || (p->stencil_mask && p->stencil_mask!=255 && p->stencil_mask!=-1))die("depth/stencil clear target/mask",p->stencil_mask);
    VTexture *t=texture_get(G.state.rt.depth,0);VkImageAspectFlags flags=0;
    if(p->depth_mask)flags|=VK_IMAGE_ASPECT_DEPTH_BIT;if(p->stencil_mask)flags|=VK_IMAGE_ASPECT_STENCIL_BIT;
    flags&=aspect(t->format);if(!flags)return;
    VkImageSubresourceRange range=attachment_range(&t->source,G.state.rt.has_dview?&G.state.rt.dview:NULL);range.aspectMask=flags;
    unsigned w=t->width>>range.baseMipLevel,h=t->height>>range.baseMipLevel;if(!w)w=1;if(!h)h=1;
    VkRect2D s=G.state.scissor;
    if(s.offset.x || s.offset.y || (s.extent.width && s.extent.width!=w) || (s.extent.height && s.extent.height!=h)){
        if(s.offset.x<0 || s.offset.y<0)die("depth clear negative scissor",0);
        if((unsigned)s.offset.x>=w || (unsigned)s.offset.y>=h)return;
        if(s.extent.width>w-s.offset.x)s.extent.width=w-s.offset.x;if(s.extent.height>h-s.offset.y)s.extent.height=h-s.offset.y;
        render_begin();VkClearAttachment a={.aspectMask=flags,.clearValue.depthStencil={p->depth,(uint32_t)p->stencil}};
        VkClearRect r={s,0,1};vkCmdClearAttachments(S.command,1,&a,1,&r);t->cpu_owned=0;tex_dirty(t);G.profile_scissor_clears++;return;
    }
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);VkClearDepthStencilValue value={p->depth,(uint32_t)p->stencil};
    vkCmdClearDepthStencilImage(S.command,t->image,t->layout,&value,1,&range);t->cpu_owned=0;tex_dirty(t);G.profile_full_clears++;
}
static void clear_tex(const p_clear_tex *p) {
    const view_t *v=p->has_view?&p->view:NULL;
    unsigned level=v&&v->set_levels?v->base_level:0,layer=v&&v->set_layers?v->min_layer:0;
    int nvn=v&&v->set_format?v->format:p->tex->d.format;
    VTexture *t=texture_get(p->tex,0);
    fprintf(stderr,"[nvn-vulkan] CLEAR_TEX serial=%llu basefmt=%d viewfmt=%d region=%d,%d,%d,%d,%d,%d uint=%u\n",(unsigned long long)p->tex->serial,p->tex->d.format,nvn,p->r.xoffset,p->r.yoffset,p->r.zoffset,p->r.width,p->r.height,p->r.depth,p->is_uint);
    unsigned w=t->width>>level,h=t->height>>level;if(!w)w=1;if(!h)h=1;
    int volume=p->tex->d.target==NVN_TEXTURE_TARGET_3D;
    unsigned z=volume?(t->depth>>level):p->tex->L.layers;if(!z)z=1;
    int bw,bh,bpp=nvn_fmt_bpb(nvn,&bw,&bh);
    if(p->mask!=15 || level>=p->tex->d.levels || p->r.xoffset<0 || p->r.yoffset<0 || p->r.zoffset<0 ||
       p->r.width<0 || p->r.height<0 || p->r.depth<1 || bw!=1 || bh!=1 || bpp>16 || bpp!=(int)t->bpp ||
       (v&&v->set_target&&v->target!=p->tex->d.target) || (uint64_t)p->r.xoffset+p->r.width>w ||
       (uint64_t)p->r.yoffset+p->r.height>h || (uint64_t)(volume?0:layer)+p->r.zoffset+p->r.depth>z)
        die("texture clear view/region/mask",nvn);
    if(!p->r.width || !p->r.height)return;
    uint8_t pixel[16]={0};int channels=0,unorm=0,integer=0,floating=0;
    switch(nvn) {
        case NVN_FORMAT_R8:channels=1;unorm=1;break;case NVN_FORMAT_RG8:channels=2;unorm=1;break;case NVN_FORMAT_RGBA8:case NVN_FORMAT_RGBA8_SRGB:channels=4;unorm=1;break;
        case NVN_FORMAT_R8UI:channels=1;integer=1;break;case NVN_FORMAT_RG8UI:channels=2;integer=1;break;case NVN_FORMAT_RGBA8UI:channels=4;integer=1;break;
        case NVN_FORMAT_R32UI:channels=1;integer=4;break;case NVN_FORMAT_RG32UI:channels=2;integer=4;break;case NVN_FORMAT_RGBA32UI:channels=4;integer=4;break;
        case NVN_FORMAT_R32F:channels=1;floating=1;break;case NVN_FORMAT_RG32F:channels=2;floating=1;break;case NVN_FORMAT_RGBA32F:channels=4;floating=1;break;
        default:die("texture clear value format",nvn);
    }
    for(int i=0;i<channels;i++) {
        double value=p->is_uint?(double)p->c.u[i]:(double)p->c.f[i];
        if(unorm) {
            value=fmax(0,fmin(1,value));
            if(nvn==NVN_FORMAT_RGBA8_SRGB && i<3)value=value<=0.0031308?12.92*value:1.055*pow(value,1.0/2.4)-0.055;
            pixel[i]=(uint8_t)floor(value*255+0.5);
        }else if(integer==1)pixel[i]=(uint8_t)fmax(0,fmin(255,value));
        else if(integer==4) {uint32_t u=p->is_uint?p->c.u[i]:(uint32_t)fmax(0,fmin(4294967295.0,value));memcpy(pixel+i*4,&u,4);}
        else if(floating) {float f=(float)value;memcpy(pixel+i*4,&f,4);}
    }
    uint64_t bytes=(uint64_t)p->r.width*p->r.height*p->r.depth*bpp;
    VBuffer *staging=temporary(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for(uint64_t i=0;i<bytes;i+=bpp)memcpy(staging->map+i,pixel,bpp);
    all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c={.imageSubresource={aspect(t->format),level,volume?0:layer+p->r.zoffset,volume?1:p->r.depth},
        .imageOffset={p->r.xoffset,p->r.yoffset,volume?p->r.zoffset:0},.imageExtent={p->r.width,p->r.height,volume?p->r.depth:1}};
    vkCmdCopyBufferToImage(S.command,staging->b,t->image,t->layout,1,&c);
    t->cpu_owned=0;tex_dirty(t); /* GPU now owns the image; sampled access must preserve it. */
}
static VkImageSubresourceLayers copy_layers(const tex_t *t,VkFormat gpu_format,const view_t *v,const NVNcopyRegion *r) {
    unsigned level=v&&v->set_levels?v->base_level:0,base=v&&v->set_layers?v->min_layer:0;
    int volume=t->d.target==NVN_TEXTURE_TARGET_3D;
    if(level>=t->d.levels || (v&&v->set_levels&&v->nlevels<1) || (v&&v->set_format&&v->format!=t->d.format) ||
       (v&&v->set_target&&v->target!=t->d.target) || r->xoffset<0 || r->yoffset<0 || r->zoffset<0 || r->width<1 || r->height<1 || r->depth<1)
        die("texture copy view/region",t->serial);
    unsigned w=t->d.width>>level,h=(t->d.target==NVN_TEXTURE_TARGET_1D || t->d.target==NVN_TEXTURE_TARGET_1D_ARRAY)?1:t->d.height>>level;
    unsigned z=volume?t->d.depth>>level:t->L.layers;if(!w)w=1;if(!h)h=1;if(!z)z=1;
    if((uint64_t)r->xoffset+r->width>w || (uint64_t)r->yoffset+r->height>h || (uint64_t)(volume?0:base)+r->zoffset+r->depth>z)
        die("texture copy extent",t->serial);
    /* BC images have already been expanded into the resident GPU format.
     * Their guest format is not a Vulkan image format in this backend. */
    return (VkImageSubresourceLayers){aspect(gpu_format),level,volume?0:base+r->zoffset,volume?1:r->depth};
}
static void copy_t2t(const p_copy_t2t *p) {
    if(p->flags&~(NVN_COPY_FLAGS_ENGINE_2D_BIT|NVN_COPY_FLAGS_LINEAR_FILTER_BIT))die("texture copy flags",p->flags);
    uint32_t decoded_format;int decoded_bpp;
    int bc_destination=nvn_bc_unpacked(p->dst->d.format,&decoded_format,&decoded_bpp);
    /* A partial BC update must retain the other compressed blocks already
     * present in guest storage, even when the destination is first used here. */
    VTexture *s=texture_get(p->src,1),*d=texture_get(p->dst,bc_destination);
    int sv=p->src->d.target==NVN_TEXTURE_TARGET_3D,dv=p->dst->d.target==NVN_TEXTURE_TARGET_3D;
    if(s==d || sv!=dv || s->format!=d->format || p->src->d.format!=p->dst->d.format)die("texture copy alias/dimension/format",p->dst->serial);
    VkImageSubresourceLayers sl=copy_layers(p->src,s->format,p->has_sv?&p->sv:NULL,&p->sr),dl=copy_layers(p->dst,d->format,p->has_dv?&p->dv:NULL,&p->dr);
    fprintf(stderr,"[nvn-vulkan] COPY_T2T src=%llu dst=%llu region=%d,%d,%d -> %d,%d,%d flags=%#x src_level=%u dst_level=%u src_layer=%u dst_layer=%u\n",(unsigned long long)p->src->serial,(unsigned long long)p->dst->serial,p->sr.width,p->sr.height,p->sr.depth,p->dr.width,p->dr.height,p->dr.depth,p->flags,sl.mipLevel,dl.mipLevel,sl.baseArrayLayer,dl.baseArrayLayer);
    texture_transition(s,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);texture_transition(d,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);all_barrier();
    VkOffset3D so={p->sr.xoffset,p->sr.yoffset,sv?p->sr.zoffset:0},doff={p->dr.xoffset,p->dr.yoffset,dv?p->dr.zoffset:0};
    if(p->sr.width==p->dr.width&&p->sr.height==p->dr.height&&p->sr.depth==p->dr.depth) {
        VkImageCopy c={.srcSubresource=sl,.srcOffset=so,.dstSubresource=dl,.dstOffset=doff,.extent={p->sr.width,p->sr.height,sv?p->sr.depth:1}};
        vkCmdCopyImage(S.command,s->image,s->layout,d->image,d->layout,1,&c);
    }else {
        VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(G.physical,s->format,&fp);
        if(aspect(s->format)!=VK_IMAGE_ASPECT_COLOR_BIT || !(p->flags&NVN_COPY_FLAGS_ENGINE_2D_BIT) || (!sv&&p->sr.depth!=p->dr.depth) ||
           !(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))die("texture scaled copy feature/flags",p->flags);
        VkFilter filter=p->flags&NVN_COPY_FLAGS_LINEAR_FILTER_BIT?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        if(filter==VK_FILTER_LINEAR&&!(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))die("texture blit linear feature",s->format);
        VkImageBlit c={.srcSubresource=sl,.srcOffsets={so,{so.x+p->sr.width,so.y+p->sr.height,so.z+(sv?p->sr.depth:1)}},
            .dstSubresource=dl,.dstOffsets={doff,{doff.x+p->dr.width,doff.y+p->dr.height,doff.z+(dv?p->dr.depth:1)}}};
        vkCmdBlitImage(S.command,s->image,s->layout,d->image,d->layout,1,&c,filter);
    }
    d->cpu_owned=0;tex_dirty(d);all_barrier();
}
static void payload_check(const op_hdr *h,size_t bytes) { if(h->size<8+bytes)die("command payload truncated",h->op); }
static int command_memory_readable(CommandMemoryCursor *cursor,const uint8_t *pointer,uint64_t bytes) {
    uint64_t address=(uint64_t)(uintptr_t)pointer;
    if(command_cursor_contains(cursor,S.epoch,address,bytes)){G.profile_command_lease_hits++;return 1;}
    G.profile_command_resolves++;VPool *pool=pool_find(address,bytes,NULL);
    if(!pool){cursor->valid=0;return 0;}
    *cursor=(CommandMemoryCursor){(uint64_t)(uintptr_t)pool->guest,pool->size,pool->serial,S.epoch,1};
    return 1;
}
static void execute(const uint8_t *p,unsigned depth) {
    if(depth>32)die("nested command recursion",depth);
    CommandMemoryCursor cursor={0};
    for(unsigned guard=0;guard<1000000;guard++) {
        if(!command_memory_readable(&cursor,p,8))die("command memory outside pools",(long long)(uintptr_t)p);
        op_hdr h;memcpy(&h,p,sizeof(h));if(h.size<8 || h.size%8 || !command_memory_readable(&cursor,p,h.size))die("command record size",h.size);
        G.profile_command_records++;G.profile_command_bytes+=h.size;
        const uint8_t *a=p+8;
        switch(h.op) {
        case OP_END:return;
        case OP_BEGIN:break; /* Structural record; GPU binding state persists across command handles. */
        case OP_JUMP:payload_check(&h,8);memcpy(&p,a,8);continue;
        case OP_CALL:{const uint8_t *next;payload_check(&h,8);memcpy(&next,a,8);execute(next,depth+1);break;}
        case OP_SIGNAL_EVENT:{Event e;payload_check(&h,16);memcpy(&e.addr,a,8);memcpy(&e.mode,a+8,4);memcpy(&e.value,a+12,4);
            if(G.event_trace)fprintf(stderr,"[nvn-event] SIGNAL_REPLAY frame=%llu record=%p address=%p mode=%u value=%u\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)e.addr,e.mode,e.value);
            mtl_sub_event(e.addr,e.mode,e.value);break;}
        case OP_REPORT_COUNTER:{uint64_t type,address;payload_check(&h,16);memcpy(&type,a,8);memcpy(&address,a+8,8);mtl_sub_counter(address,type);break;}
        case OP_RESET_COUNTER:{uint64_t type;payload_check(&h,8);memcpy(&type,a,8);
            if(type!=NVN_COUNTER_TYPE_SAMPLES_PASSED)die("GPU counter reset type implementation pending",type);
            occlusion_end();G.profile_counter_resets++;counter_op(0,0,0);break;}
        case OP_COPY_B2B:{uint64_t src,dst,len;payload_check(&h,24);memcpy(&src,a,8);memcpy(&dst,a+8,8);memcpy(&len,a+16,8);mtl_sub_copy(src,dst,len);break;}
        case OP_UPDATE_UB:{uint64_t dst,len;payload_check(&h,16);memcpy(&dst,a,8);memcpy(&len,a+8,8);if(len>h.size-24)die("uniform update payload",len);
            G.profile_uniform_updates++;
            all_barrier();uint64_t off=0;VPool *pool=pool_find(dst,len,&off);if(!pool || off%4 || len%4 || len>65536)die("uniform update bounds/alignment",len);
            pool_materialize(pool);vkCmdUpdateBuffer(S.command,pool->v.b,off,len,a+16);mark_write(dst,len);S.write_cpu[S.nwrites-1]=a+16;break;}
        case OP_SET_RT:payload_check(&h,sizeof(p_set_rt));G.setrt_total++;
            if(!memcmp(&G.state.rt,a,sizeof(p_set_rt))){G.setrt_same++;if(!G.disable_setrt_skip)break;}
            pass_end();memcpy(&G.state.rt,a,sizeof(p_set_rt));break;
        case OP_CLEAR_COLOR:payload_check(&h,sizeof(p_clear_color));clear_color((const p_clear_color *)a);break;
        case OP_CLEAR_DS:payload_check(&h,sizeof(p_clear_ds));clear_ds((const p_clear_ds *)a);break;
        case OP_VIEWPORTS:{payload_check(&h,sizeof(p_viewports));const p_viewports *v=(const void *)a;if(v->first || v->n!=1)die("multiple viewports",v->n);
            /* NVN SetViewports records only x/y/w/h; depth range has its own
             * command and survives viewport changes. */
            G.state.viewport.x=v->v[0][0];G.state.viewport.y=v->v[0][1];
            G.state.viewport.width=v->v[0][2];G.state.viewport.height=v->v[0][3];break;}
        case OP_SCISSOR:{payload_check(&h,sizeof(p_scissor));const p_scissor *s=(const void *)a;if(s->x<0||s->y<0||s->w<0||s->h<0)die("scissor bounds",s->x);
            G.state.scissor=(VkRect2D){{s->x,s->y},{s->w,s->h}};break;}
        case OP_DEPTH_RANGE:{payload_check(&h,sizeof(p_depth_range));const p_depth_range *d=(const void *)a;G.state.viewport.minDepth=d->n;G.state.viewport.maxDepth=d->f;break;}
        case OP_BIND_PROGRAM:payload_check(&h,sizeof(p_bind_program));G.state.program=((const p_bind_program *)a)->prog;break;
        case OP_BIND_STATE:{payload_check(&h,sizeof(p_bind_state));const p_bind_state *b=(const void *)a;switch(b->kind){
            case 0:{blend_st s;memcpy(&s,b->bytes,sizeof(s));if(s.target>=8)die("blend target",s.target);G.state.blend[s.target]=s;break;}
            case 1:memcpy(&G.state.color,b->bytes,sizeof(color_st));break;case 2:memcpy(&G.state.mask,b->bytes,sizeof(chmask_st));break;
            case 3:memcpy(&G.state.ms,b->bytes,sizeof(ms_st));break;case 4:memcpy(&G.state.polygon,b->bytes,sizeof(poly_st));break;
            case 5:memcpy(&G.state.ds,b->bytes,sizeof(ds_st));break;default:die("state kind",b->kind);}break;}
        case OP_BIND_VATTR:{payload_check(&h,offsetof(p_bind_vattr,st));const p_bind_vattr *b=(const void *)a;if(b->first<0||b->n<0||b->first>16||b->n>16-b->first)die("vertex attribute range",b->n);
            payload_check(&h,offsetof(p_bind_vattr,st)+(size_t)b->n*sizeof(va_st));
            memcpy(G.state.attributes+b->first,b->st,b->n*sizeof(va_st));break;}
        case OP_BIND_VSTREAM:{payload_check(&h,offsetof(p_bind_vstream,st));const p_bind_vstream *b=(const void *)a;if(b->first<0||b->n<0||b->first>16||b->n>16-b->first)die("vertex stream range",b->n);
            payload_check(&h,offsetof(p_bind_vstream,st)+(size_t)b->n*sizeof(vs_st));
            memcpy(G.state.streams+b->first,b->st,b->n*sizeof(vs_st));break;}
        case OP_BIND_VBUF:{payload_check(&h,offsetof(p_bind_vbuf,r));const p_bind_vbuf *b=(const void *)a;if(b->first<0||b->n<0||b->first>16||b->n>16-b->first)die("vertex buffer range",b->n);
            payload_check(&h,offsetof(p_bind_vbuf,r)+(size_t)b->n*sizeof(NVNbufferRange));
            memcpy(G.state.vbuf+b->first,b->r,b->n*sizeof(NVNbufferRange));break;}
        case OP_BIND_UBO:case OP_BIND_SSBO:{payload_check(&h,sizeof(p_bind_buf));const p_bind_buf *b=(const void *)a;if(b->stage<0||b->stage>=6||b->index<0||b->index>=16)die("buffer stage/index",b->index);
            Range *dest=h.op==OP_BIND_UBO?G.state.uniform[b->stage]:G.state.storage[b->stage];Range next={b->addr,b->size};
            if(memcmp(&dest[b->index],&next,sizeof(next))){dest[b->index]=next;G.binding_revision[b->stage]++;if(h.op==OP_BIND_SSBO||b->stage==5)G.ssbo_revision[b->stage]++;}break;}
        case OP_STENCIL:{payload_check(&h,sizeof(p_stencil));const p_stencil *s=(const void *)a;if(s->face<0||s->face>3||s->which<0||s->which>2)die("stencil dynamic parameters",s->face);
            for(int i=0;i<2;i++) {
                if(s->face&(1<<i)){if(s->which==0)G.state.stencil_compare[i]=s->value_mask;
                    else if(s->which==1)G.state.stencil_write[i]=s->mask;else G.state.stencil_ref[i]=s->ref;}
            }
            break;}
        case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:{payload_check(&h,offsetof(p_bind_handles,h));const p_bind_handles *b=(const void *)a;
            int limit=h.op==OP_BIND_IMG?32:128;if(b->stage<0||b->stage>=6||b->first<0||b->n<0||b->n>32||b->first>limit||b->n>limit-b->first)die("texture/sampler binding range",b->n);
            payload_check(&h,offsetof(p_bind_handles,h)+(size_t)b->n*sizeof(uint64_t));
            uint64_t *dest=h.op==OP_BIND_TEX?G.state.tex[b->stage]:h.op==OP_BIND_SAMP?G.state.samp[b->stage]:G.state.img[b->stage];
            size_t bytes=(size_t)b->n*sizeof(uint64_t);if(bytes&&memcmp(dest+b->first,b->h,bytes)){memcpy(dest+b->first,b->h,bytes);G.binding_revision[b->stage]++;G.texbind_revision[b->stage]++;}break;}
        case OP_DRAW:payload_check(&h,sizeof(p_draw));draw((const p_draw *)a,NULL);break;
        case OP_DRAW_IDX:payload_check(&h,sizeof(p_draw_idx));draw(NULL,(const p_draw_idx *)a);break;
        case OP_DISPATCH:payload_check(&h,sizeof(p_dispatch));dispatch((const p_dispatch *)a);break;
        case OP_BARRIER:{payload_check(&h,4);int32_t bits;memcpy(&bits,a,4);G.barrier_bits[bits&255]++;G.last_barrier_bits=bits;
            /* A pass with no shader-written memory has nothing for a barrier to order: attachment hazards are covered by
             * the render-pass external dependencies and layout transitions at the pass boundaries. */
            if(G.barrier_pass_elide&&S.in_pass&&!G.pass_side_effects){G.barrier_kept[bits&255]++;break;}
            if(!S.in_pass)G.barrier_nopass++;else G.barrier_sidefx++;
            all_barrier_soft();break;} /* Full barrier is stronger than every supported NVN barrier bit. */
        case OP_POLY_OFFSET:{payload_check(&h,sizeof(p_poly_offset));const p_poly_offset *v=(const void *)a;
            if(v->factor || v->units || v->clamp)die("nonzero polygon offset requires Vulkan pipeline bias",0);
            break;}
        case OP_WAIT_EVENT:{uint64_t address;uint32_t mode,value;payload_check(&h,16);
            memcpy(&address,a,8);memcpy(&mode,a+8,4);memcpy(&value,a+12,4);
            if(!pool_find(address,4,NULL) || address%4 || mode>1)die("event wait storage/mode",address);
            uint32_t before_cut=atomic_load((_Atomic uint32_t *)(uintptr_t)address),projected=before_cut;unsigned prior_same=0,pending_cpu_writes=0,pending_counter_reports=0;uint64_t pending_write_bytes=0;
            if(G.event_trace){
                for(int i=0;i<S.nwrites;i++){pending_write_bytes+=S.writes[i].size;pending_cpu_writes+=S.write_cpu[i]!=NULL;}
                for(int i=0;i<S.ncounter;i++)pending_counter_reports+=S.counter_ops[i].kind>=2;
                fprintf(stderr,"[nvn-event] WAIT_CUT_BEGIN frame=%llu record=%p address=%p mode=%u value=%u observed_before_cut=%u pending_signal_count=%d pending_write_ranges=%d pending_write_bytes=%llu pending_cpu_writes=%u pending_counter_ops=%d pending_counter_reports=%u pending_readback=%u\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)(uintptr_t)address,mode,value,before_cut,S.nevents,S.nwrites,(unsigned long long)pending_write_bytes,pending_cpu_writes,S.ncounter,pending_counter_reports,S.submission_cpu_readback);
                for(int i=0;i<S.nevents;i++)if((uint64_t)(uintptr_t)S.events[i].addr==address){
                    Event *event=&S.events[i];prior_same++;if(event->mode)projected+=event->value;else projected=event->value;
                    fprintf(stderr,"[nvn-event] WAIT_PRIOR_SIGNAL frame=%llu wait_record=%p address=%p index=%d mode=%u value=%u projected=%u\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)(uintptr_t)address,i,event->mode,event->value,projected);
                }
                int projected_ready=mode==0?projected==value:(int32_t)(projected-value)>=0;
                fprintf(stderr,"[nvn-event] WAIT_PROJECTED frame=%llu record=%p address=%p prior_same=%u projected=%u predicate_ready=%u\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)(uintptr_t)address,prior_same,projected,projected_ready);
            }
            /* The GPU queue is in order: a wait whose predicate is already satisfied by the current value plus the
             * signals recorded earlier in this replay needs neither a submission nor a CPU poll. The event values are
             * still published in recorded order when the segment completes. */
            if(!G.disable_wait_elision){
                uint32_t value_now=before_cut;
                for(int i=0;i<S.nevents;i++)if((uint64_t)(uintptr_t)S.events[i].addr==address){Event *event=&S.events[i];if(event->mode)value_now+=event->value;else value_now=event->value;}
                if(mode==0?value_now==value:(int32_t)(value_now-value)>=0){
                    if(G.event_trace)fprintf(stderr,"[nvn-event] WAIT_ELIDED address=%p mode=%u value=%u projected=%u\n",(void *)(uintptr_t)address,mode,value,value_now);
                    all_barrier_soft();G.profile_wait_elided++;break;
                }
            }
            /* Submit preceding work and publish only its actual GPU results.
             * This also handles a signal earlier in the same command handle. */
            S.submission_event_wait=1;
            mtl_sub_commit();
            uint32_t after_cut=atomic_load((_Atomic uint32_t *)(uintptr_t)address);
            uint64_t start=dm_android_nvn_clock_ns();
            uint64_t work_wait=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_EVENT);
            uint64_t polls=0;
            for(;;){uint32_t observed=atomic_load((_Atomic uint32_t *)(uintptr_t)address);
                if(mode==0 ? observed==value : (int32_t)(observed-value)>=0)break;
                if(G.event_trace)polls++;
                if(dm_android_nvn_clock_ns()-start>30000000000ULL)die("event wait exceeded diagnostic deadline",address);
                struct timespec pause={0,100000};nanosleep(&pause,NULL);
            }
            uint64_t poll_ns=dm_android_nvn_clock_ns()-start;uint32_t after_wait=atomic_load((_Atomic uint32_t *)(uintptr_t)address);
            dm_android_nvn_work_wait_end(work_wait);
            if(G.event_trace)fprintf(stderr,"[nvn-event] WAIT_CUT_END frame=%llu record=%p address=%p mode=%u value=%u before_cut=%u after_publish=%u after_wait=%u prior_same_address=%u poll_ns=%llu polls=%llu\n",(unsigned long long)G.present_count+1,(const void *)p,(void *)(uintptr_t)address,mode,value,before_cut,after_cut,after_wait,prior_same,(unsigned long long)poll_ns,(unsigned long long)polls);
            mtl_sub_begin();break;}
        case OP_WAIT_SYNC:case OP_FENCE_SYNC:{payload_check(&h,sizeof(p_sync));const p_sync *v=(const void *)a;
            if(!v->sync || v->sync->magic!=MAGIC('S','Y','N','C'))die("sync lifetime",0);
            if(h.op==OP_WAIT_SYNC){
                if(!v->sync->fenced || v->sync->fence>G.fence+1)die("wait on unsubmitted sync",v->sync->fence);
                /* Earlier submits finish synchronously. A fence recorded in this
                 * submit is ordered by a full GPU memory/execution barrier. */
                all_barrier_soft();
            }else{
                if(v->condition<0 || v->condition>1 || v->flags&~1)die("fence condition/flags",v->flags);
                all_barrier_soft();v->sync->fence=G.fence+1;v->sync->fenced=1;
                /* CPU completion remains false until vkWaitForFences finishes. */
            }break;}
        case OP_COPY_B2T:payload_check(&h,sizeof(p_copy_b2t));copy_b2t((const p_copy_b2t *)a);break;
        case OP_CLEAR_TEX:payload_check(&h,sizeof(p_clear_tex));clear_tex((const p_clear_tex *)a);break;
        case OP_COPY_T2T:payload_check(&h,sizeof(p_copy_t2t));copy_t2t((const p_copy_t2t *)a);break;
        case OP_COPY_STRIDE:payload_check(&h,sizeof(p_copy_stride));G.state.copy_stride=((const p_copy_stride *)a)->stride;break;
        case OP_BLEND_COLOR:payload_check(&h,sizeof(p_blend_color));memcpy(G.state.blend_color,((const p_blend_color *)a)->rgba,16);break;
        case OP_SHADER_SCRATCH:payload_check(&h,sizeof(p_bind_buf));
            {const p_bind_buf *r=(const p_bind_buf *)a;
             if(!pool_find(r->addr,r->size,NULL))die("shader scratch pool lifetime/range",r->addr);
             /* Maxwell LDL/STL are translated to SPIR-V Private variables.
              * Vulkan's compiler owns the physical per-invocation allocation;
              * the NVN scratch range has no application-visible payload. */
             G.state.scratch=(Range){r->addr,r->size};}break;
        case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:payload_check(&h,sizeof(p_set_pool));
            if(!((const p_set_pool *)a)->pool)die("null descriptor pool",h.op);
            {const dpool_t **current=h.op==OP_SET_TEX_POOL?&G.state.texture_pool:&G.state.sampler_pool;
             if(*current && *current!=((const p_set_pool *)a)->pool)die("multiple descriptor pools need qualified registration",h.op);
             if(!*current){*current=((const p_set_pool *)a)->pool;for(unsigned stage=0;stage<6;stage++){G.binding_revision[stage]++;G.ssbo_revision[stage]++;G.texbind_revision[stage]++;}}}break;
        default:die("unimplemented NVN command opcode",h.op);
        }
        p+=h.size;
    }
    die("command iteration limit",depth);
}
void mtl_exec(const uint8_t *handle) {
    if(!G.active)die("execute without submission",0);
    if(replay_cpu_depth++==0)replay_cpu_started=dm_android_nvn_thread_cpu_ns();
    execute(handle,0);
    if(--replay_cpu_depth==0){G.profile_replay_cpu_ns+=dm_android_nvn_thread_cpu_ns()-replay_cpu_started;G.profile_replay_calls++;}
}
void mtl_exec_rest(const uint64_t *handles,int n) { (void)handles;(void)n; } /* Optimization-only lookahead; replay order is unchanged. */
void mtl_sub_index(const uint64_t *handles,int n) { (void)handles;(void)n; } /* No attachment store-elision optimizer in this backend. */

static void swapchain_destroy_locked(void);
void dm_vk_set_window(void *native) {
    mtl_init();pthread_mutex_lock(&G.lock);async_drain_locked();CHECK(cpu_work_queue_idle(G.queue));
    /* Focus/resume notifications may publish the same BufferQueue again.
     * Retain its Vulkan surface instead of disconnecting and reconnecting the
     * producer. Invalidate only the swapchain if this window changed size. */
    if(native && G.window==native){
        if(G.swapchain && ((uint32_t)ANativeWindow_getWidth(G.window)!=G.swap_extent.width ||
                          (uint32_t)ANativeWindow_getHeight(G.window)!=G.swap_extent.height)){
            swapchain_destroy_locked();
        }
        pthread_mutex_unlock(&G.lock);return;
    }
    swapchain_destroy_locked();if(G.surface)vkDestroySurfaceKHR(G.instance,G.surface,NULL);
    if(G.window)ANativeWindow_release(G.window);G.window=native;G.swapchain=VK_NULL_HANDLE;G.surface=VK_NULL_HANDLE;G.nswap=0;
    if(G.window){ANativeWindow_acquire(G.window);VkAndroidSurfaceCreateInfoKHR i={.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,.window=G.window};
        PFN_vkCreateAndroidSurfaceKHR create=(PFN_vkCreateAndroidSurfaceKHR)dm_vk_get(G.instance,"vkCreateAndroidSurfaceKHR");
        if(!create)die("Android Vulkan surface entry",0);CHECK(create(G.instance,&i,NULL,&G.surface)); }
    pthread_mutex_unlock(&G.lock);
}
static void swapchain_make(void) {
    if(G.swapchain)return;if(!G.surface)die("present without Android Surface",0);
    VkBool32 supported=0;CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(G.physical,G.family,G.surface,&supported));if(!supported)die("queue surface support",0);
    VkSurfaceCapabilitiesKHR caps;CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(G.physical,G.surface,&caps));
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))die("swapchain transfer usage",caps.supportedUsageFlags);
    uint32_t n=0;CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(G.physical,G.surface,&n,NULL));if(!n)die("surface formats",0);
    VkSurfaceFormatKHR *formats=calloc(n,sizeof(*formats));if(!formats)die("surface format allocation",n);
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(G.physical,G.surface,&n,formats));VkSurfaceFormatKHR chosen=formats[0];
    for(uint32_t i=0;i<n;i++)if(formats[i].format==VK_FORMAT_R8G8B8A8_UNORM || formats[i].format==VK_FORMAT_B8G8R8A8_UNORM){chosen=formats[i];break;}free(formats);
    G.swap_format=chosen.format;G.swap_extent=caps.currentExtent;
    if(G.swap_extent.width==UINT32_MAX)G.swap_extent=(VkExtent2D){(uint32_t)ANativeWindow_getWidth(G.window),(uint32_t)ANativeWindow_getHeight(G.window)};
    uint32_t count=caps.minImageCount+1;if(caps.maxImageCount && count>caps.maxImageCount)count=caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;if(!(caps.supportedCompositeAlpha&alpha))alpha=VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if(!(caps.supportedCompositeAlpha&alpha))die("surface composite alpha",caps.supportedCompositeAlpha);
    VkSwapchainCreateInfoKHR i={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=G.surface,.minImageCount=count,
      .imageFormat=chosen.format,.imageColorSpace=chosen.colorSpace,.imageExtent=G.swap_extent,.imageArrayLayers=1,
      .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR?VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:caps.currentTransform,
      .compositeAlpha=alpha,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};CHECK(vkCreateSwapchainKHR(G.device,&i,NULL,&G.swapchain));
    CHECK(vkGetSwapchainImagesKHR(G.device,G.swapchain,&G.nswap,NULL));if(!G.nswap)die("swapchain has no images",0);
    free(G.swap_images);free(G.swap_layouts);
    G.swap_images=calloc(G.nswap,sizeof(*G.swap_images));G.swap_layouts=calloc(G.nswap,sizeof(*G.swap_layouts));
    if(!G.swap_images || !G.swap_layouts)die("swapchain image allocation",G.nswap);
    CHECK(vkGetSwapchainImagesKHR(G.device,G.swapchain,&G.nswap,G.swap_images));
    if(G.nswap>16)die("swapchain image count",G.nswap);
    VkSemaphoreCreateInfo sem_info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(uint32_t k=0;k<G.nswap;k++)CHECK(vkCreateSemaphore(G.device,&sem_info,NULL,&G.swap_finished[k]));
}
static void swapchain_destroy_locked(void) {
    for(uint32_t k=0;k<G.nswap&&k<16;k++)if(G.swap_finished[k]){vkDestroySemaphore(G.device,G.swap_finished[k],NULL);G.swap_finished[k]=VK_NULL_HANDLE;}
    if(G.swapchain)vkDestroySwapchainKHR(G.device,G.swapchain,NULL);G.swapchain=VK_NULL_HANDLE;G.nswap=0;
}
uint64_t mtl_present(const tex_t *source,double period,const int crop[4]) {
    if(present_cpu_depth++==0)present_cpu_started=dm_android_nvn_thread_cpu_ns();
    uint64_t present_started=dm_android_nvn_clock_ns();G.seg_index=0;
    (void)period;if(!G.window)die("original game present requires Surface",0);sub_begin_ctx(&G.submission_slot2);swapchain_make();
    VTexture *t=texture_get(source,0);if(aspect(t->format)!=VK_IMAGE_ASPECT_COLOR_BIT)die("present depth texture",t->format);
    if(G.present_count<2 || G.trace_frame)fprintf(stderr,"[nvn-vulkan] PRESENT_FORMAT frame=%llu source_format=%u swap_format=%u source_extent=%u,%u swap_extent=%u,%u\n",(unsigned long long)G.present_count+1,t->format,G.swap_format,t->width,t->height,G.swap_extent.width,G.swap_extent.height);
    const char *capture_root=getenv("DM_FRAME_DIR");VBuffer capture={0};
    int capture_frame=capture_root && (G.trace_frame || (getenv("DM_CAPTURE_FRAMES") &&
        (G.present_count<4 || (G.present_count%120==0 && G.present_count<1000))));
    if(capture_frame && t->bpp==4){capture=buffer_make((uint64_t)t->width*t->height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);VkBufferImageCopy c={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={t->width,t->height,1}};
        vkCmdCopyImageToBuffer(S.command,t->image,t->layout,capture.b,1,&c);}
    VkSemaphoreCreateInfo sem_info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore available;
    CHECK(vkCreateSemaphore(G.device,&sem_info,NULL,&available));
    uint64_t acquire_wait=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_ACQUIRE);
    uint32_t index=0;VkResult acquired=vkAcquireNextImageKHR(G.device,G.swapchain,UINT64_MAX,available,VK_NULL_HANDLE,&index);
    dm_android_nvn_work_wait_end(acquire_wait);G.pres_acq_ns+=dm_android_nvn_clock_ns()-present_started;
    if(acquired!=VK_SUCCESS && acquired!=VK_SUBOPTIMAL_KHR)die("swapchain acquisition",acquired);
    VkSemaphore finished=G.swap_finished[index];
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=G.swap_layouts[index],
       .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
       .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=G.swap_images[index],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
    int x=0,y=0,w=t->width,h=t->height;if(crop && crop[2]>0 && crop[3]>0){x=crop[0];y=crop[1];w=crop[2];h=crop[3];}
    if(x<0||y<0||w<=0||h<=0||(uint64_t)x+w>t->width||(uint64_t)y+h>t->height)die("present crop bounds",w);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{x,y,0},{x+w,y+h,1}},
      .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int)G.swap_extent.width,(int)G.swap_extent.height,1}}};
    VkFormatProperties src_format,dst_format;vkGetPhysicalDeviceFormatProperties(G.physical,t->format,&src_format);
    vkGetPhysicalDeviceFormatProperties(G.physical,G.swap_format,&dst_format);
    if(!(src_format.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(dst_format.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))die("present blit formats",t->format);
    vkCmdBlitImage(S.command,t->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,G.swap_images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,
       src_format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
    vkCmdPipelineBarrier(S.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
    G.swap_layouts[index]=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;CHECK(vkEndCommandBuffer(S.command));
    VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,
      .pWaitSemaphores=&available,.pWaitDstStageMask=&wait_stage,.commandBufferCount=1,.pCommandBuffers=&S.command,
      .signalSemaphoreCount=1,.pSignalSemaphores=&finished};
    VkFence fence=S.gpu_fence;
    CHECK(vkQueueSubmit(G.queue,1,&si,fence));S.ticket=++G.fence;S.phase=SUBMISSION_INFLIGHT;VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,
      .pWaitSemaphores=&finished,.swapchainCount=1,.pSwapchains=&G.swapchain,.pImageIndices=&index};
    uint64_t present_wait=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_PRESENT);
    VkResult presented=vkQueuePresentKHR(G.queue,&pi);dm_android_nvn_work_wait_end(present_wait);
    if(presented!=VK_SUCCESS && presented!=VK_SUBOPTIMAL_KHR)die("queue present",presented);
    int async_present=G.async_enabled&&!capture.b&&!G.trace_frame&&!S.ntrace_captures;
    if(async_present)S.present_available=available;
    else{
    uint64_t pres_t0=dm_android_nvn_clock_ns();CHECK(cpu_work_wait_fences(G.device,1,&fence,VK_TRUE,UINT64_MAX));uint64_t pres_t1=dm_android_nvn_clock_ns();G.pres_fence_ns+=pres_t1-pres_t0;G.pres_idle_ns+=dm_android_nvn_clock_ns()-pres_t1;
    submission_gpu_done(&S);
    pass_timing_collect(&S);
    trace_capture_complete(&S);
    if(capture.b){char file[1200];snprintf(file,sizeof file,"%s/frame-%04llu.ppm",capture_root,(unsigned long long)G.present_count+1);
        FILE *out=fopen(file,"wb");if(!out)die("frame readback output",0);fprintf(out,"P6\n%u %u\n255\n",t->width,t->height);
        size_t row_bytes=(size_t)t->width*3;uint8_t *rgb_row=malloc(row_bytes);if(!rgb_row)die("frame readback row allocation",row_bytes);
        unsigned nonblack=0;unsigned red=t->format==VK_FORMAT_B8G8R8A8_UNORM?2:0,blue=2-red;
        for(unsigned y=0;y<t->height;y++){
            const uint8_t *pixels=capture.map+(uint64_t)y*t->width*4;
            for(unsigned x=0;x<t->width;x++){const uint8_t *pixel=pixels+(size_t)x*4;uint8_t *rgb=rgb_row+(size_t)x*3;
                rgb[0]=pixel[red];rgb[1]=pixel[1];rgb[2]=pixel[blue];if(rgb[0]||rgb[1]||rgb[2])nonblack++;}
            if(fwrite(rgb_row,1,row_bytes,out)!=row_bytes)die("frame readback write",row_bytes);
        }free(rgb_row);
        fclose(out);fprintf(stderr,"[nvn-vulkan] PRESENT_READBACK frame=%llu extent=%u,%u nonblack=%u draws=%llu passes=%llu\n",(unsigned long long)G.present_count+1,t->width,t->height,nonblack,(unsigned long long)G.draws,(unsigned long long)G.pass_count);buffer_free(&capture);
    }
    vkDestroySemaphore(G.device,available,NULL);
    for(int i=0;i<S.nview;++i)vkDestroyImageView(G.device,S.temporary_views[i],NULL);
    temporaries_retire(&S);
    }
    G.profile_frames++;
    G.profile_present_ns+=dm_android_nvn_clock_ns()-present_started;
    if(--present_cpu_depth==0){G.profile_present_cpu_ns+=dm_android_nvn_thread_cpu_ns()-present_cpu_started;G.profile_present_calls++;}
    if(G.trace_frame || G.profile_frames>=60 || G.present_count<2){uint64_t now=dm_android_nvn_clock_ns();
        double elapsed_ms=(now-G.profile_start_ns)/1e6;
        double fps=elapsed_ms>0?1000.0*G.profile_frames/elapsed_ms:0;
        fprintf(stderr,"[nvn-vulkan] RENDER_PROFILE frame=%llu frames=%llu fps=%.3f elapsed_ms=%.3f pipeline_ms=%.3f gpu_wait_ms=%.3f upload_decode_ms=%.3f submits=%llu draws=%llu pso_new=%lu pso_slow=%lu pso_slow_ms=%.1f\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,fps,elapsed_ms,G.profile_pipeline_ns/1e6,G.profile_wait_ns/1e6,G.profile_decode_ns/1e6,(unsigned long long)G.profile_submits,(unsigned long long)(G.draws-G.profile_draws),(unsigned long)atomic_load(&g_pso_create_count),(unsigned long)atomic_load(&g_pso_slow_count),(double)atomic_load(&g_pso_slow_total_ns)/1e6);
        fprintf(stderr,"[nvn-vulkan] RENDER_SUBMIT_CLASS_PROFILE frame=%llu total_submits=%llu observed_eligible_candidates=%llu candidate_fence_wait_ms=%.3f pool_writeback_bytes=%llu events=%llu counter_reports=%llu write_submits=%llu event_submits=%llu report_submits=%llu trace_submits=%llu cpu_readback_submits=%llu event_wait_submits=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_submits,(unsigned long long)G.profile_submit_candidates,G.profile_candidate_wait_ns/1e6,(unsigned long long)G.profile_writeback_bytes,(unsigned long long)G.profile_events,(unsigned long long)G.profile_reports,(unsigned long long)G.profile_write_submits,(unsigned long long)G.profile_event_submits,(unsigned long long)G.profile_report_submits,(unsigned long long)G.profile_trace_submits,(unsigned long long)G.profile_cpu_readback_submits,(unsigned long long)G.profile_event_wait_submits);
        fprintf(stderr,"[nvn-vulkan] RENDER_CPU_PROFILE frame=%llu lookup_ms=%.3f pool_ms=%.3f bind_ms=%.3f renderpass_ms=%.3f present_ms=%.3f desc_alloc_ms=%.3f desc_update_ms=%.3f prep_ms=%.3f state_ms=%.3f vertex_ms=%.3f pso_ms=%.3f tail_ms=%.3f\n",(unsigned long long)G.present_count+1,G.profile_lookup_ns/1e6,G.profile_pool_ns/1e6,G.profile_bind_ns/1e6,G.profile_render_ns/1e6,G.profile_present_ns/1e6,G.profile_desc_alloc_ns/1e6,G.profile_desc_update_ns/1e6,G.profile_prep_ns/1e6,G.profile_state_ns/1e6,G.profile_vertex_ns/1e6,G.profile_pso_ns/1e6,G.profile_tail_ns/1e6);
        fprintf(stderr,"[nvn-vulkan] RENDER_BIND_SPLIT frame=%llu plan_hits=%llu plan_ms=%.3f full_calls=%llu full_ms=%.3f\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_bind_hits,G.profile_bind_plan_ns/1e6,(unsigned long long)G.profile_bind_full_calls,G.profile_bind_full_ns/1e6);
        fprintf(stderr,"[nvn-vulkan] RENDER_PLAN_REASON frame=%llu rev=%llu epoch=%llu state=%llu registry=%llu pool=%llu buf_none=%llu buf_align=%llu buf_addr=%llu buf_pool=%llu buf_off=%llu buf_dynbig=%llu img_check=%llu img_layout=%llu img_samp=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_plan_rev,(unsigned long long)G.profile_plan_epoch,(unsigned long long)G.profile_plan_state,(unsigned long long)G.profile_plan_reg,(unsigned long long)G.profile_plan_pool,(unsigned long long)G.profile_plan_buf_none,(unsigned long long)G.profile_plan_buf_align,(unsigned long long)G.profile_plan_buf_addr,(unsigned long long)G.profile_plan_buf_pool,(unsigned long long)G.profile_plan_buf_off,(unsigned long long)G.profile_plan_buf_dynbig,(unsigned long long)G.profile_plan_img_check,(unsigned long long)G.profile_plan_img_layout,(unsigned long long)G.profile_plan_img_samp);
        fprintf(stderr,"[nvn-vulkan] RENDER_TAIL_SPLIT frame=%llu bind_ms=%.3f state_ms=%.3f draw_ms=%.3f idx_ms=%.3f\n",(unsigned long long)G.present_count+1,G.profile_tail_bind_ns/1e6,G.profile_tail_state_ns/1e6,G.profile_tail_draw_ns/1e6,G.profile_tail_idx_ns/1e6);
        fprintf(stderr,"[nvn-vulkan] RENDER_RP_SPLIT frame=%llu flush_ms=%.3f chunk_ms=%.3f cend_ms=%.3f csub_ms=%.3f cbeg_ms=%.3f attach_ms=%.3f key_ms=%.3f scan_ms=%.3f create_ms=%.3f begin_ms=%.3f\n",(unsigned long long)G.present_count+1,G.profile_rp_flush_ns/1e6,G.profile_rp_chunk_ns/1e6,G.profile_chunk_end_ns/1e6,G.profile_chunk_submit_ns/1e6,G.profile_chunk_begin_ns/1e6,G.profile_rp_attach_ns/1e6,G.profile_rp_key_ns/1e6,G.profile_rp_scan_ns/1e6,G.profile_rp_create_ns/1e6,G.profile_rp_begin_ns/1e6);
        if(G.profile_detail){for(int rank=0;rank<8;rank++){int best=-1;uint64_t bestc=0;
            for(int i=0;i<8192;i++)if(G.pass_end_sites[i]>bestc){bestc=G.pass_end_sites[i];best=i;}
            if(best<0||!bestc)break;
            fprintf(stderr,"[nvn-vulkan] PASS_END_SITE rank=%d line=%d count=%llu draws=%llu\n",rank,best,(unsigned long long)G.pass_end_sites[best],(unsigned long long)G.pass_end_draws[best]);
            G.pass_end_sites[best]=0;G.pass_end_draws[best]=0;}
            memset(G.pass_end_sites,0,sizeof G.pass_end_sites);memset(G.pass_end_draws,0,sizeof G.pass_end_draws);}
        fprintf(stderr,"[nvn-vulkan] RENDER_WRITEBACK_PROFILE frame=%llu enabled=%u raw_ranges=%llu copied_ranges=%llu raw_bytes=%llu copied_bytes=%llu plan_thread_cpu_ms=%.3f\n",(unsigned long long)G.present_count+1,G.enable_writeback_coalesce,(unsigned long long)G.profile_writeback_raw_ranges,(unsigned long long)G.profile_writeback_copy_ranges,(unsigned long long)G.profile_writeback_raw_bytes,(unsigned long long)G.profile_writeback_bytes,G.profile_writeback_plan_cpu_ns/1e6);
        fprintf(stderr,"[nvn-vulkan] RENDER_PSO_PROFILE frame=%llu cached=%u hits=%llu misses=%llu\n",(unsigned long long)G.present_count+1,G.npso,(unsigned long long)G.profile_pso_hits,(unsigned long long)G.profile_pso_misses);
        if(G.profile_frames>=60)fprintf(stderr,"[nvn-vulkan] RENDER_PSO_HASH_PROFILE frame=%llu frames=%llu key_bytes=%zu hash_mode=byte_fnv1a last_cache=%d lookups=%llu hash_calls=%llu hash_ms=%.3f lookup_ms=%.3f last_hits=%llu last_misses=%llu table_hits=%llu misses=%llu table_probes=%llu max_probe=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,sizeof(PsoKey),G.enable_pso_last,(unsigned long long)G.profile_pso_lookups,(unsigned long long)G.profile_pso_hash_calls,G.profile_pso_hash_ns/1e6,G.profile_pso_lookup_ns/1e6,(unsigned long long)G.profile_pso_last_hits,(unsigned long long)G.profile_pso_last_misses,(unsigned long long)G.profile_pso_table_hits,(unsigned long long)G.profile_pso_misses,(unsigned long long)G.profile_pso_probes,(unsigned long long)G.profile_pso_max_probe);
        if(G.profile_frames>=60)fprintf(stderr,"[nvn-vulkan] RENDER_PHASE_CPU_PROFILE frame=%llu frames=%llu clock=thread_cpu writeback_prepare=%d replay_inclusive_ms=%.3f begin_ms=%.3f commit_prepare_ms=%.3f commit_wait_ms=%.3f retire_ms=%.3f writeback_subset_ms=%.3f present_body_inclusive_ms=%.3f replay_calls=%llu begin_calls=%llu commit_calls=%llu writeback_calls=%llu present_calls=%llu wb_global_faults=%llu wb_global_preps=%llu wb_global_mp_calls=%llu wb_global_mp_pages=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,G.enable_writeback_prepare,G.profile_replay_cpu_ns/1e6,G.profile_begin_cpu_ns/1e6,G.profile_commit_prepare_cpu_ns/1e6,G.profile_commit_wait_cpu_ns/1e6,G.profile_retire_cpu_ns/1e6,G.profile_writeback_cpu_ns/1e6,G.profile_present_cpu_ns/1e6,(unsigned long long)G.profile_replay_calls,(unsigned long long)G.profile_begin_calls,(unsigned long long)G.profile_commit_calls,(unsigned long long)G.profile_writeback_calls,(unsigned long long)G.profile_present_calls,(unsigned long long)G.profile_writeback_global_faults,(unsigned long long)G.profile_writeback_global_preps,(unsigned long long)G.profile_writeback_global_mp_calls,(unsigned long long)G.profile_writeback_global_mp_pages);
        if(G.profile_frames>=60)fprintf(stderr,"[nvn-vulkan] RENDER_SUBMISSION_PROFILE frame=%llu contexts=1 epoch=%llu creates=%llu reuses=%llu publications=%llu context_bytes=%zu async=0\n",(unsigned long long)G.present_count+1,(unsigned long long)S.epoch,(unsigned long long)G.profile_submission_creates,(unsigned long long)G.profile_submission_reuses,(unsigned long long)G.profile_submission_publications,sizeof(VkSubmission)+DESC_ARENA+DESC_ENTRIES*sizeof(DescEntry));
        if(G.profile_frames>=60)fprintf(stderr,"[nvn-vulkan] RENDER_COMMAND_PROFILE frame=%llu records=%llu bytes=%llu lease_hits=%llu pool_resolves=%llu epoch_bound=1\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_command_records,(unsigned long long)G.profile_command_bytes,(unsigned long long)G.profile_command_lease_hits,(unsigned long long)G.profile_command_resolves);
        if(G.profile_frames>=60)command_state_profile("RENDER_COMMAND_STATE_PROFILE",G.present_count+1,G.profile_frames);
        dirty_query_profile("RENDER_DIRTY_QUERY_PROFILE",G.present_count+1,G.profile_frames,1);
        memset(G.profile_cmd_issued,0,sizeof G.profile_cmd_issued);memset(G.profile_cmd_skipped,0,sizeof G.profile_cmd_skipped);
        G.profile_command_records=G.profile_command_bytes=G.profile_command_lease_hits=G.profile_command_resolves=0;
        G.profile_submission_creates=G.profile_submission_reuses=G.profile_submission_publications=0;
        G.profile_pso_hits=G.profile_pso_misses=0;
        G.profile_pso_lookups=G.profile_pso_hash_calls=G.profile_pso_hash_ns=G.profile_pso_lookup_ns=0;
        G.profile_pso_last_hits=G.profile_pso_last_misses=G.profile_pso_table_hits=G.profile_pso_probes=G.profile_pso_max_probe=0;
        G.profile_replay_cpu_ns=G.profile_begin_cpu_ns=G.profile_commit_prepare_cpu_ns=G.profile_commit_wait_cpu_ns=G.profile_retire_cpu_ns=G.profile_writeback_cpu_ns=G.profile_present_cpu_ns=0;
        G.profile_replay_calls=G.profile_begin_calls=G.profile_commit_calls=G.profile_writeback_calls=G.profile_present_calls=0;
        G.profile_writeback_global_faults=G.profile_writeback_global_preps=G.profile_writeback_global_mp_calls=G.profile_writeback_global_mp_pages=0;
        fprintf(stderr,"[nvn-vulkan] CONTROL_CACHE_REASON evict=%llu changed=%llu""\n",(unsigned long long)G.profile_control_evict,(unsigned long long)G.profile_control_changed);G.profile_control_evict=G.profile_control_changed=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_CACHE_PROFILE frame=%llu desc_hits=%llu desc_misses=%llu control_hits=%llu control_misses=%llu barriers_skipped=%llu sample_copy_hits=%llu sample_copy_misses=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_desc_hits,(unsigned long long)G.profile_desc_misses,(unsigned long long)G.profile_control_hits,(unsigned long long)G.profile_control_misses,(unsigned long long)G.profile_barriers_skipped,(unsigned long long)G.profile_sample_copy_hits,(unsigned long long)G.profile_sample_copy_misses);
        G.profile_desc_hits=G.profile_desc_misses=G.profile_control_hits=G.profile_control_misses=G.profile_barriers_skipped=G.profile_sample_copy_hits=G.profile_sample_copy_misses=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_BINDING_PLAN_PROFILE frame=%llu hits=%llu misses=%llu fallbacks=%llu prepared_buffers=%llu prepared_images=%llu enabled=%u\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_plan_hits,(unsigned long long)G.profile_plan_misses,(unsigned long long)G.profile_plan_fallbacks,(unsigned long long)G.profile_plan_buffers,(unsigned long long)G.profile_plan_images,!G.disable_binding_plans);
        G.profile_plan_hits=G.profile_plan_misses=G.profile_plan_fallbacks=G.profile_plan_buffers=G.profile_plan_images=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_VIEW_CACHE_PROFILE frame=%llu view_hits=%llu view_misses=%llu framebuffer_hits=%llu framebuffer_misses=%llu rp_hits=%llu rp_creates=%llu rp_overflow=%llu rtv_hits=%llu rtv_creates=%llu rtv_stale=%llu rtf_hits=%llu rtf_creates=%llu rtf_stale=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_view_hits,(unsigned long long)G.profile_view_misses,(unsigned long long)G.profile_framebuffer_hits,(unsigned long long)G.profile_framebuffer_misses,(unsigned long long)G.profile_rpc_hits,(unsigned long long)G.profile_rpc_creates,(unsigned long long)G.profile_rpc_overflow,(unsigned long long)G.profile_rtv_hits,(unsigned long long)G.profile_rtv_creates,(unsigned long long)G.profile_rtv_stale,(unsigned long long)G.profile_rtf_hits,(unsigned long long)G.profile_rtf_creates,(unsigned long long)G.profile_rtf_stale);
        G.profile_view_hits=G.profile_view_misses=G.profile_framebuffer_hits=G.profile_framebuffer_misses=G.profile_rpc_hits=G.profile_rpc_creates=G.profile_rpc_overflow=G.profile_rtv_hits=G.profile_rtv_creates=G.profile_rtv_stale=G.profile_rtf_hits=G.profile_rtf_creates=G.profile_rtf_stale=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_POOL_UPLOAD_PROFILE frame=%llu pools=%d uploaded_bytes=%llu reused_ranges=%llu reused_bytes=%llu read_memo_hits=%llu read_memo_misses=%llu\n",(unsigned long long)G.present_count+1,G.npool,(unsigned long long)G.profile_pool_uploaded_bytes,(unsigned long long)G.profile_pool_reused_ranges,(unsigned long long)G.profile_pool_reused_bytes,(unsigned long long)G.profile_read_memo_hits,(unsigned long long)G.profile_read_memo_misses);
        G.profile_pool_uploaded_bytes=G.profile_pool_reused_ranges=G.profile_pool_reused_bytes=0;G.profile_read_memo_hits=G.profile_read_memo_misses=0;
        if(G.pass_timing){for(uint32_t a=0;a<G.npacc;a++)for(uint32_t b=a+1;b<G.npacc;b++)if(G.pacc[b].ms>G.pacc[a].ms){PassAcc tmp=G.pacc[a];G.pacc[a]=G.pacc[b];G.pacc[b]=tmp;}
            double total=0;for(uint32_t a=0;a<G.npacc;a++)total+=G.pacc[a].ms;fprintf(stderr,"[nvn-vulkan] PASS_TIME_TOTAL frame=%llu groups=%u gpu_ms=%.3f\n",(unsigned long long)G.present_count+1,G.npacc,total);
            for(uint32_t a=0;a<G.npacc&&a<12;a++)fprintf(stderr,"[nvn-vulkan] PASS_TIME rank=%u rt_key=%llx size=%ux%u colors=%u passes=%u draws=%llu ms=%.3f\n",a,(unsigned long long)G.pacc[a].key,G.pacc[a].w,G.pacc[a].h,G.pacc[a].n,G.pacc[a].passes,(unsigned long long)G.pacc[a].draws,G.pacc[a].ms);
            for(uint32_t a=0;a<G.nhot;a++)for(uint32_t b=a+1;b<G.nhot;b++)if(G.hot[b].draws>G.hot[a].draws){char tk[17];uint32_t td=G.hot[a].draws;memcpy(tk,G.hot[a].key,17);memcpy(G.hot[a].key,G.hot[b].key,17);G.hot[a].draws=G.hot[b].draws;memcpy(G.hot[b].key,tk,17);G.hot[b].draws=td;}
            for(uint32_t a=0;a<G.nhot&&a<12;a++)fprintf(stderr,"[nvn-vulkan] HOT_PS rank=%u key=%s draws=%u\n",a,G.hot[a].key,G.hot[a].draws);
            G.nhot=0;G.npacc=0;}
        fprintf(stderr,"[nvn-vulkan] RENDER_RESOURCE_PROFILE frame=%llu driver_tables=%llu driver_table_reuses=%llu buffer_allocs=%llu buffer_bytes=%llu full_clears=%llu scissor_clears=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_driver_tables,(unsigned long long)G.profile_driver_table_reuses,(unsigned long long)G.profile_buffer_allocs,(unsigned long long)G.profile_buffer_bytes,(unsigned long long)G.profile_full_clears,(unsigned long long)G.profile_scissor_clears);
        G.profile_driver_tables=G.profile_driver_table_reuses=G.profile_buffer_allocs=G.profile_buffer_bytes=G.profile_full_clears=G.profile_scissor_clears=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_TEMP_PROFILE frame=%llu reuses=%llu cached=%u cached_bytes=%llu clear_ms=%.3f allocate_ms=%.3f aligned_bytes=%llu comparison_bytes=%llu driver_bytes=%llu other_bytes=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_temp_reuses,G.nretired,(unsigned long long)G.retired_bytes,G.profile_temp_clear_ns/1e6,G.profile_buffer_allocate_ns/1e6,(unsigned long long)G.profile_temp_bytes[TEMP_ALIGNED],(unsigned long long)G.profile_temp_bytes[TEMP_COMPARISON],(unsigned long long)G.profile_temp_bytes[TEMP_DRIVER],(unsigned long long)G.profile_temp_bytes[TEMP_OTHER]);
        G.profile_temp_reuses=G.profile_temp_clear_ns=G.profile_buffer_allocate_ns=0;memset(G.profile_temp_bytes,0,sizeof G.profile_temp_bytes);
        fprintf(stderr,"[nvn-vulkan] RENDER_STORAGE_BIAS_PROFILE frame=%llu bindings=%llu avoided_snapshot_bytes=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_storage_bias_bindings,(unsigned long long)G.profile_storage_bias_bytes);
        G.profile_storage_bias_bindings=G.profile_storage_bias_bytes=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_QUERY_PROFILE frame=%llu queries=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_occlusion_queries);
        G.profile_occlusion_queries=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_QUERY_RESET_PROFILE frame=%llu frames=%llu enabled=%d calls=%llu slots=%llu pre_resets=%llu reserve_breaks=%llu pass_splits=%llu counter_resets=%llu counter_reports=%llu counter_ops=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)G.profile_frames,G.enable_query_reset_range,(unsigned long long)G.profile_query_reset_calls,(unsigned long long)G.profile_query_reset_slots,(unsigned long long)G.profile_query_preresets,(unsigned long long)G.profile_query_breaks,(unsigned long long)G.profile_pass_splits,(unsigned long long)G.profile_counter_resets,(unsigned long long)G.profile_counter_reports,(unsigned long long)G.profile_counter_ops);
        { /* 频率/GPU 忙时监控：每个剖面窗口读一次（诊断用，判断 fps 波动与 GPU 负载）。
             gpubusy = "busy total" 累计计数（kgsl）；窗口 delta 直接给出 GPU 忙时。
             | frequency / GPU-busy monitor, read once per profile window: gpubusy is kgsl's
             cumulative busy/total counter; the per-window delta yields GPU busy time. */
          unsigned cpu7=0;FILE *f;
          f=fopen("/sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq","r");if(f){if(fscanf(f,"%u",&cpu7)!=1)cpu7=0;fclose(f);}
          unsigned long long busy=0,total=0;
          f=fopen("/sys/class/kgsl/kgsl-3d0/gpubusy","r");if(f){if(fscanf(f,"%llu %llu",&busy,&total)!=2){busy=total=0;}fclose(f);}
          /* gpubusy 是即时占用率读数（busy/total 为滚动窗口内的时间对），不是累计计数。 */
          double util=(total)?100.0*(double)busy/(double)total:-1.0;
          fprintf(stderr,"[nvn-vulkan] SYS_FREQ frame=%llu cpu7_khz=%u gpu_busy_raw=%llu gpu_total_raw=%llu gpu_util_pct=%.1f\n",(unsigned long long)G.present_count+1,cpu7,busy,total,util); }
        G.profile_query_reset_calls=G.profile_query_reset_slots=G.profile_query_preresets=G.profile_query_breaks=G.profile_pass_splits=0;
        G.profile_counter_resets=G.profile_counter_reports=G.profile_counter_ops=0;
        fprintf(stderr,"[nvn-vulkan] RENDER_SYNC_PROFILE frame=%llu passes=%llu all_barriers=%llu uniform_updates=%llu aligned_cpu_snapshots=%llu aligned_gpu_copies=%llu aligned_snapshot_reuses=%llu\n",(unsigned long long)G.present_count+1,(unsigned long long)(G.pass_count-G.profile_passes),(unsigned long long)G.profile_barriers,(unsigned long long)G.profile_uniform_updates,(unsigned long long)G.profile_cpu_snapshots,(unsigned long long)G.profile_gpu_copies,(unsigned long long)G.profile_snapshot_reuses);
        fprintf(stderr,"[nvn-vulkan] PRESENT_PARTS acquire_ms=%.1f fence_ms=%.1f idle_ms=%.1f\n",G.pres_acq_ns/1e6,G.pres_fence_ns/1e6,G.pres_idle_ns/1e6);G.pres_acq_ns=G.pres_fence_ns=G.pres_idle_ns=0;
        for(int q=0;q<4;q++){fprintf(stderr,"[nvn-vulkan] SEG%d record_ms=%.1f wait_ms=%.1f last_chunk_draws=%llu chunks=%llu gpu_busy_ms=%.1f gpu_gap_ms=%.1f gpu_span_ms=%.1f\n",q,G.seg_record_ns[q]/1e6,G.seg_wait_ns[q]/1e6,(unsigned long long)G.seg_draws[q],(unsigned long long)G.seg_chunks[q],G.gts_busy[q]/1e6,G.gts_gap[q]/1e6,G.gts_span[q]/1e6);G.gts_busy[q]=G.gts_gap[q]=G.gts_span[q]=G.gts_chunks[q]=0;G.seg_record_ns[q]=G.seg_wait_ns[q]=G.seg_draws[q]=G.seg_chunks[q]=0;}
        if(G.precise_sync||G.barrier_stats)fprintf(stderr,"[nvn-vulkan] HZ_SYNC barriers=%llu chunks=%llu wait_elided=%llu\n",(unsigned long long)G.profile_hz_barriers,(unsigned long long)G.profile_chunks,(unsigned long long)G.profile_wait_elided);G.profile_chunks=0;G.profile_wait_elided=0;fprintf(stderr,"[nvn-vulkan] HZ_KINDS pass_frag=%llu pass_wide=%llu xfer=%llu cs=%llu tr_shaderread=%llu tr_attach=%llu tr_transfer=%llu tr_other=%llu\n",(unsigned long long)G.hz_count[0],(unsigned long long)G.hz_count[1],(unsigned long long)G.hz_count[2],(unsigned long long)G.hz_count[3],(unsigned long long)G.hz_count[4],(unsigned long long)G.hz_count[5],(unsigned long long)G.hz_count[6],(unsigned long long)G.hz_count[7]);memset(G.hz_count,0,sizeof G.hz_count);G.profile_hz_barriers=0;
        if(G.barrier_stats){fprintf(stderr,"[nvn-vulkan] BARRIER_BITS nopass=%u sidefx=%u",G.barrier_nopass,G.barrier_sidefx);G.barrier_nopass=G.barrier_sidefx=0;for(int i=0;i<256;i++)if(G.barrier_bits[i]){fprintf(stderr," %#x=%u(kept %u)",i,G.barrier_bits[i],G.barrier_kept[i]);G.barrier_bits[i]=G.barrier_kept[i]=0;}fprintf(stderr,"\n");fprintf(stderr,"[nvn-vulkan] BARRIER_SITES");for(int i=0;i<4096;i++)if(G.barrier_sites[i])fprintf(stderr," L%d=%u",i,G.barrier_sites[i]);fprintf(stderr,"\n");}
        if(G.barrier_stats){fprintf(stderr,"[nvn-vulkan] SETRT total=%llu same=%llu\n",(unsigned long long)G.setrt_total,(unsigned long long)G.setrt_same);G.setrt_total=G.setrt_same=0;}
        if(G.barrier_stats){fprintf(stderr,"[nvn-vulkan] PASS_END_SITES");for(int i=0;i<4096;i++)if(G.pass_end_sites[i])fprintf(stderr," L%d=%u",i,G.pass_end_sites[i]);fprintf(stderr,"\n");}
        memset(G.pass_end_sites,0,sizeof G.pass_end_sites);
        memset(G.barrier_sites,0,sizeof G.barrier_sites);
        G.profile_passes=G.pass_count;G.profile_barriers=G.profile_uniform_updates=G.profile_cpu_snapshots=G.profile_gpu_copies=G.profile_snapshot_reuses=0;
        G.profile_lookup_ns=G.profile_pool_ns=G.profile_bind_ns=G.profile_render_ns=G.profile_present_ns=G.profile_desc_alloc_ns=G.profile_desc_update_ns=G.profile_prep_ns=G.profile_state_ns=G.profile_vertex_ns=G.profile_pso_ns=G.profile_tail_ns=0;
        G.profile_bind_hits=G.profile_bind_full_calls=G.profile_bind_plan_ns=G.profile_bind_full_ns=0;
        G.profile_plan_rev=G.profile_plan_epoch=G.profile_plan_state=G.profile_plan_reg=G.profile_plan_pool=0;
        G.profile_plan_buf_none=G.profile_plan_buf_align=G.profile_plan_buf_addr=G.profile_plan_buf_pool=G.profile_plan_buf_off=G.profile_plan_buf_dynbig=0;
        G.profile_plan_img_check=G.profile_plan_img_layout=G.profile_plan_img_samp=0;
        G.profile_tail_bind_ns=G.profile_tail_state_ns=G.profile_tail_draw_ns=G.profile_tail_idx_ns=0;
        G.profile_chunk_end_ns=G.profile_chunk_submit_ns=G.profile_chunk_begin_ns=0;
        G.profile_rp_flush_ns=G.profile_rp_chunk_ns=G.profile_rp_attach_ns=G.profile_rp_key_ns=G.profile_rp_scan_ns=G.profile_rp_create_ns=G.profile_rp_begin_ns=0;
        /* §1 带宽 A/B：运行时可翻转的 load/store（文件内容 = 0/1/2/3，语义同 DM_DIAG_LOADSTORE）。
         * 每个 profile 窗口（~1 秒）读一次；切换会改变后续渲染通道的键（新通道对象，旧的留在
         * 缓存里到会话结束）。文件不存在 = 保持当前模式。 | §1 bandwidth A/B: runtime load/store
         * override (file content = 0/1/2/3, same meaning as DM_DIAG_LOADSTORE), read once per
         * profile window (~1 s). A switch changes the render-pass keys from the next pass on (new
         * objects are created; old ones stay for the session). File absent: keep the current mode. */
        { static const char *lsdir; if(!lsdir)lsdir=getenv("DM_FRAME_DIR"); char lsf[1300];
          if(lsdir&&*lsdir&&snprintf(lsf,sizeof lsf,"%s/loadstore.txt",lsdir)<(int)sizeof lsf){FILE *fp=fopen(lsf,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&m>=0&&m<=3&&m!=G.diag_loadstore){G.diag_loadstore=m;
                fprintf(stderr,"[nvn-vulkan] DIAG_LOADSTORE 运行中切换 → %d（%s）\n",m,m?"内容将不正确（诊断）":"恢复正常");}fclose(fp);}}}
        /* 分块提交粒度运行时切换：chunkd.txt = 每多少绘制冲刷一次（0 = 关，整帧一个命令缓冲）。
         * 每个窗口（~1 秒）读一次，下一帧起生效。 | Runtime chunk-submit granularity: chunkd.txt =
         * draws between flushes (0 = off, one command buffer per frame). Read once per window. */
        { static const char *cddir; if(!cddir)cddir=getenv("DM_FRAME_DIR"); char cdf[1300];
          if(cddir&&*cddir&&snprintf(cdf,sizeof cdf,"%s/chunkd.txt",cddir)<(int)sizeof cdf){FILE *fp=fopen(cdf,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&m>=0&&m<=1000&&m!=G.chunk_draws){G.chunk_draws=m;
                fprintf(stderr,"[nvn-vulkan] CHUNK_DRAWS 运行中切换 → %d\n",m);}fclose(fp);}}}
        /* 绑定计划校验模式运行时切换：planstate.txt = 1 精确（逐绑定校验，含采样器身份；不做 state 粗闸门）
         * / 0 粗闸门（原行为）。A/B 用；每窗口读一次。 | Binding-plan validation mode: planstate.txt = 1
         * precise (per-binding validation incl. sampler identity; no coarse state gate) / 0 coarse (previous
         * behaviour). For in-process A/B; read once per window. */
        { static const char *psdir; if(!psdir)psdir=getenv("DM_FRAME_DIR"); char psf[1300];
          if(psdir&&*psdir&&snprintf(psf,sizeof psf,"%s/planstate.txt",psdir)<(int)sizeof psf){FILE *fp=fopen(psf,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&(m==0||m==1)&&m!=G.plan_precise_state){G.plan_precise_state=m;
                fprintf(stderr,"[nvn-vulkan] PLAN_STATE 运行中切换 → %d（%s）\n",m,m?"精确逐绑定校验":"粗 state 闸门");}fclose(fp);}}}
        /* 查询前缀预重置运行时 A/B：prereset.txt = 1 开 / 0 关（默认 0，见上）。 | Occlusion query
         * prefix pre-reset runtime A/B: prereset.txt = 1 on / 0 off (default 0, see above). */
        { static const char *qrdir; if(!qrdir)qrdir=getenv("DM_FRAME_DIR"); char qrf[1300];
          if(qrdir&&*qrdir&&snprintf(qrf,sizeof qrf,"%s/prereset.txt",qrdir)<(int)sizeof qrf){FILE *fp=fopen(qrf,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&(m==0||m==1)&&m!=G.query_pre_reset){G.query_pre_reset=m;
                fprintf(stderr,"[nvn-vulkan] QUERY_PRERESET 运行中切换 → %d\n",m);}fclose(fp);}}}
        /* 遮挡查询成本诊断（A/B，仅统计用途）：nooccl.txt=1 完全不发查询；imprecise.txt=1 去掉
         * PRECISE 位。两者都会让游戏读到错误的遮挡计数（纯诊断，不得用于游玩）。 | Occlusion query
         * cost diagnostics (A/B, measurement only): nooccl.txt=1 disables queries entirely;
         * imprecise.txt=1 drops the PRECISE bit. Both feed wrong counts to the game (diagnostic only). */
        { static const char *nodir; if(!nodir)nodir=getenv("DM_FRAME_DIR"); char nof[1300];
          if(nodir&&*nodir&&snprintf(nof,sizeof nof,"%s/nooccl.txt",nodir)<(int)sizeof nof){FILE *fp=fopen(nof,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&(m==0||m==1)&&m!=G.diag_no_occlusion){G.diag_no_occlusion=m;
                fprintf(stderr,"[nvn-vulkan] DIAG_NO_OCCLUSION 运行中切换 → %d\n",m);}fclose(fp);}}}
        { static const char *imdir; if(!imdir)imdir=getenv("DM_FRAME_DIR"); char imf[1300];
          if(imdir&&*imdir&&snprintf(imf,sizeof imf,"%s/imprecise.txt",imdir)<(int)sizeof imf){FILE *fp=fopen(imf,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&(m==0||m==1)&&m!=G.diag_imprecise_occlusion){G.diag_imprecise_occlusion=m;
                fprintf(stderr,"[nvn-vulkan] DIAG_IMPRECISE_OCCLUSION 运行中切换 → %d\n",m);}fclose(fp);}}}
        /* 读出备忘运行时 A/B：readmemo.txt = 1 开（默认）/ 0 关。 | Read-memo runtime A/B:
         * readmemo.txt = 1 on (default) / 0 off. */
        { static const char *rmdir; if(!rmdir)rmdir=getenv("DM_FRAME_DIR"); char rmf[1300];
          if(rmdir&&*rmdir&&snprintf(rmf,sizeof rmf,"%s/readmemo.txt",rmdir)<(int)sizeof rmf){FILE *fp=fopen(rmf,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&(m==0||m==1)&&m!=G.read_memo_enabled){G.read_memo_enabled=m;
                fprintf(stderr,"[nvn-vulkan] READ_MEMO 运行中切换 → %d\n",m);}fclose(fp);}}}
        /* 有意通道分割器运行时切换：passsplit.txt = 单通道 draw 上限（0=关）。 | Intentional
         * pass splitter: passsplit.txt = draws per pass cap (0 = off). */
        { static const char *psdir; if(!psdir)psdir=getenv("DM_FRAME_DIR"); char psf[1300];
          if(psdir&&*psdir&&snprintf(psf,sizeof psf,"%s/passsplit.txt",psdir)<(int)sizeof psf){FILE *fp=fopen(psf,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&m>=0&&m<=4096&&m!=G.pass_split_draws){G.pass_split_draws=m;
                fprintf(stderr,"[nvn-vulkan] PASS_SPLIT 运行中切换 → %d\n",m);}fclose(fp);}}}
        /* 持久帧缓冲缓存运行时 A/B：fbcache.txt = 1 开（默认）/ 0 关。 | Persistent framebuffer
         * cache runtime A/B: fbcache.txt = 1 on (default) / 0 off. */
        { static const char *fcdir; if(!fcdir)fcdir=getenv("DM_FRAME_DIR"); char fcf[1300];
          if(fcdir&&*fcdir&&snprintf(fcf,sizeof fcf,"%s/fbcache.txt",fcdir)<(int)sizeof fcf){FILE *fp=fopen(fcf,"r");
            if(fp){int m=-1;if(fscanf(fp,"%d",&m)==1&&(m==0||m==1)&&m!=G.fb_cache_enabled){G.fb_cache_enabled=m;
                fprintf(stderr,"[nvn-vulkan] FB_CACHE 运行中切换 → %d\n",m);}fclose(fp);}}}
        G.profile_submit_candidates=G.profile_candidate_wait_ns=G.profile_writeback_bytes=G.profile_events=G.profile_reports=0;
        G.profile_writeback_raw_bytes=G.profile_writeback_raw_ranges=G.profile_writeback_copy_ranges=G.profile_writeback_plan_cpu_ns=0;
        G.profile_write_submits=G.profile_event_submits=G.profile_report_submits=G.profile_trace_submits=G.profile_cpu_readback_submits=G.profile_event_wait_submits=0;
        G.profile_start_ns=now;G.profile_pipeline_ns=G.profile_wait_ns=G.profile_decode_ns=G.profile_submits=G.profile_frames=0;G.profile_draws=G.draws;}
    G.trace_frame=G.trace_ui=0;
    if(capture_root){char request[1200];snprintf(request,sizeof request,"%s/trace-ui-request",capture_root);
        if(!access(request,F_OK)){if(unlink(request))die("UI trace request consume",0);G.trace_frame=G.trace_ui=1;G.trace_meshes=G.ntrace_textures=0;
            fprintf(stderr,"[nvn-vulkan] TRACE_NEXT_FRAME frame=%llu ui=1\n",(unsigned long long)G.present_count+2);}
        snprintf(request,sizeof request,"%s/trace-request",capture_root);
        if(!access(request,F_OK)){if(unlink(request))die("trace request consume",0);G.trace_frame=1;G.trace_meshes=G.ntrace_textures=0;
            G.trace_shader[0]=0;char selector[1200];snprintf(selector,sizeof selector,"%s/texture-trace-shader",capture_root);FILE *selection=fopen(selector,"rb");
            if(selection){char key[18]={0};size_t bytes=fread(key,1,sizeof(key),selection);fclose(selection);
                if((bytes!=16 && !(bytes==17&&key[16]=='\n')) || strspn(key,"0123456789abcdef")!=16)die("texture trace shader selector",bytes);
                memcpy(G.trace_shader,key,16);G.trace_shader[16]=0;
            }
            fprintf(stderr,"[nvn-vulkan] TRACE_NEXT_FRAME frame=%llu\n",(unsigned long long)G.present_count+2);}}
    if(async_present){G.active=0;async_enqueue_locked(&S,1);uint64_t pticket=++G.present_count;if(g_pso_cache_dirty&&++g_pso_cache_presents>=300){g_pso_cache_presents=0;pso_cache_save();}pthread_mutex_unlock(&G.lock);return pticket;}
    if(S.nwrites || S.nevents || S.ncounter)die("present has unpublished guest effects",S.epoch);
    submission_publish(&S);submission_released(&S);
    uint64_t ticket=++G.present_count;if(g_pso_cache_dirty&&++g_pso_cache_presents>=300){g_pso_cache_presents=0;pso_cache_save();}pthread_mutex_unlock(&G.lock);return ticket;
}
void mtl_present_wait(uint64_t ticket) { if(ticket>G.present_count)die("unknown present ticket",ticket); }
void mtl_present_flush(void) { mtl_finish(); }
int mtl_has_window(void) { return G.window!=NULL; }
void dm_vk_set_display_hz(int hz) { if(hz<0 || hz>1000)die("Android display refresh rate",hz);G.display_hz=hz; }
int mtl_display_hz(void) { return G.window?G.display_hz:0; } /* 0 means the front end has not supplied a measured display mode. */
double mtl_presented_time(uint64_t ticket) { (void)ticket;return 0; } /* Actual scanout timing is not available yet. */
uint64_t mtl_present_count(void) { return G.present_count; }
void mtl_read_texels(const tex_t *source,unsigned level,unsigned layer,const NVNcopyRegion *region,uint8_t *dst,size_t row,size_t img) {
    int volume=source->d.target==NVN_TEXTURE_TARGET_3D;
    unsigned depth=volume?source->d.depth>>level:1;if(!depth)depth=1;
    if(G.active || !region || region->zoffset<0 || (unsigned)region->zoffset>=depth || region->depth!=1 || level>=source->d.levels || layer>=source->L.layers)die("CPU texture read mip/layer/inflight",level);
    uint32_t decoded_format;int decoded_bpp;if(nvn_bc_unpacked(source->d.format,&decoded_format,&decoded_bpp))die("BC readback needs original-format encoding",source->d.format);
    mtl_sub_begin();S.submission_cpu_readback=1;VTexture *t=texture_get(source,1);unsigned width=t->width>>level,height=t->height>>level;if(!width)width=1;if(!height)height=1;
    if(region->xoffset<0||region->yoffset<0||region->width<0||region->height<0||
      (uint64_t)region->xoffset+region->width>width||(uint64_t)region->yoffset+region->height>height)die("CPU texture read bounds",region->width);
    size_t pitch=(size_t)region->width*t->bpp;if(row<pitch || (img && img<row*(size_t)region->height))die("CPU texture read stride",row);
    VBuffer readback=buffer_make(pitch*region->height,VK_BUFFER_USAGE_TRANSFER_DST_BIT);texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy={.imageSubresource={aspect(t->format),level,layer,1},.imageOffset={region->xoffset,region->yoffset,region->zoffset},.imageExtent={region->width,region->height,1}};
    vkCmdCopyImageToBuffer(S.command,t->image,t->layout,readback.b,1,&copy);mtl_sub_commit();
    for(int y=0;y<region->height;y++)memcpy(dst+y*row,readback.map+y*pitch,pitch);buffer_free(&readback);
}
#ifdef DM_VK_COPY_TEST
void dm_vk_copy_test_pool_bytes(uint64_t *logical,uint64_t *resident) {
    *logical=*resident=0;for(int i=0;i<G.npool;i++){*logical+=G.pools[i].size;*resident+=G.pools[i].v.size;}
}
/* Independent device probe only: inspect expanded BC pixels without promising
 * compressed guest-format CPU readback or introducing a lossy BC encoder. */
void dm_vk_copy_test_read(const tex_t *source,unsigned level,unsigned layer,uint8_t *dst) {
    if(G.active || level>=source->d.levels || layer>=source->L.layers)die("copy probe subresource",level);
    mtl_sub_begin();S.submission_cpu_readback=1;VTexture *t=texture_get(source,1);
    unsigned w=t->width>>level,h=t->height>>level;if(!w)w=1;if(!h)h=1;
    VBuffer readback=buffer_make((uint64_t)w*h*t->bpp,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c={.imageSubresource={aspect(t->format),level,layer,1},.imageExtent={w,h,1}};
    vkCmdCopyImageToBuffer(S.command,t->image,t->layout,readback.b,1,&c);mtl_sub_commit();
    memcpy(dst,readback.map,(size_t)w*h*t->bpp);buffer_free(&readback);
}
#endif
void mtl_stats(uint64_t *passes,uint64_t *draws) { *passes=G.pass_count;*draws=G.draws; }
void mtl_pkg_stats(uint64_t *uploads,uint64_t *reuploads) { *uploads=G.uploads;*reuploads=0; }
void mtl_set_frame(uint64_t frame) { G.frame=frame; }
double mtl_gpu_ms(uint64_t frame) { (void)frame;return -1; }
int mtl_gpu_span(uint64_t frame,uint64_t *a,uint64_t *b) { (void)frame;(void)a;(void)b;return 0; }
int mtl_warm_wait(int timeout) { (void)timeout;return 0; } /* No asynchronous pipeline warm-up job exists. */
int mtl_link_active(void) { return 0; } /* No Android frame-link service has been installed. */
int mtl_link_wait_tick(uint64_t timeout) { (void)timeout;return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; } /* No cross-frame store-elision ledger exists. */
void mtl_ub_present(double ms) { (void)ms; } /* Disabled diagnostic experiment. */
uint64_t mtl_upload_bytes(void) { uint64_t n=0;for(int i=0;i<G.ntexture;i++)if(G.textures[i].cpu_owned)n+=(uint64_t)G.textures[i].width*G.textures[i].height*G.textures[i].bpp;return n; }
