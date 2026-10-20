/* Private replay submission ownership, included after the backend's types.
 * Device resources and guest queue state outlive this context. Every object
 * below belongs to one recorded submission and is reused only after its real
 * GPU fence and ordered CPU publication. The first scheduler has one slot;
 * asynchronous issue still requires immutable pool inputs/output versions.
 */
#ifndef NVN_VK_SUBMISSION_H
#define NVN_VK_SUBMISSION_H

typedef enum {
    SUBMISSION_UNUSED, SUBMISSION_RECORDING, SUBMISSION_INFLIGHT,
    SUBMISSION_GPU_DONE, SUBMISSION_PUBLISHED, SUBMISSION_RETIRED
} SubmissionPhase;

typedef struct {
    SubmissionPhase phase;
    uint64_t epoch,ticket;
    pthread_t recorder;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkCommandBuffer command_primary,chunk_cb[48];
    uint32_t chunk_alloc,chunk_index;
    uint64_t chunk_mark;
    unsigned profile_segment; /* Captured at submit, before asynchronous completion. */
    VkDescriptorPool descriptors;
    VkFence gpu_fence;
    VkQueryPool occlusion,timestamps,pass_ts,chunk_ts;
    uint32_t nchunk_ts;
    uint64_t chunk_pass0[130],chunk_draw0[130];
    uint32_t nocc,ntime,active_occlusion;
    uint32_t occlusion_reset_count,timestamp_reset_count,pass_reset_count;
    int command_dirty;
    VkSemaphore present_available;
    struct { unsigned reports;int candidate,traced;uint64_t wait_cpu,wait_start; } commit_info;
    VBuffer wb_stage;
    uint64_t wb_stage_total;
    int wb_staged;
    struct { uint32_t stages,access; } pend[4];
    NvnVkCommandState command_state;

    Range writes[LIMIT_WRITES];
    const uint8_t *write_cpu[LIMIT_WRITES];
    int nwrites;
    NvnVkWritebackSpan writeback_plan[LIMIT_WRITES];
    Event events[LIMIT_WRITES];
    int nevents;
    CounterOp counter_ops[LIMIT_WRITES];
    int ncounter;
    int submission_cpu_readback,submission_event_wait;

    VBuffer **temporaries;
    size_t temporary_capacity;
    int ntemporary;
    AlignedBinding aligned[2*LIMIT_BINDINGS];
    int naligned;
    ReadSnapshot read_snapshots[LIMIT_WRITES];
    int nread_snapshots,read_snapshots_allowed;
    DriverTable driver_tables[LIMIT_WRITES];
    uint32_t driver_table_slots[DRIVER_TABLE_SLOTS],ndriver_tables;
    VBuffer *last_driver_table[6][2];
    uint64_t driver_table_revision[6][2];
    ImageUse image_uses[2*LIMIT_BINDINGS];
    unsigned nimage_uses;
    uint32_t storage_bias[6][LIMIT_BINDINGS];
    uint32_t dyn_offset[6][LIMIT_BINDINGS];   // 本 draw 每个 stage 的动态偏移（按 set 布局绑定顺序） | this draw's dynamic offsets per stage (in set layout binding order)
    uint32_t dyn_count[6];
    Range direct_reads[LIMIT_WRITES];
    int direct_read_write_count[LIMIT_WRITES],ndirect_reads;

    VkImageView temporary_views[LIMIT_WRITES];
    int nview;
    ImageViewKey view_keys[LIMIT_WRITES];
    uint32_t view_table[VIEW_TABLE_SIZE];
    VkBufferView buffer_views[LIMIT_WRITES];
    int nbuffer_view;
    VkPipeline pipelines[LIMIT_WRITES];
    VkPipelineLayout layouts[LIMIT_WRITES];
    int npipeline;
    PassKey pass_key;
    void *pass_attach_t[9];uint32_t pass_attach_level[9];int pass_attach_n;   // pass 内附件缓存：快路径标脏不用再查纹理表 | attachment cache: fast-path dirty marking skips texture-table lookups
    VkRenderPass passes[LIMIT_WRITES];
    VkFramebuffer framebuffers[LIMIT_WRITES];
    VkRenderPass overflow_passes[16];
    VkRenderPass active_pass_rp;
    int npass,novf;
    FramebufferKey framebuffer_keys[LIMIT_WRITES];
    int active_pass,in_pass,depth_sampled,pass_depth_read_only;
    VkPipelineCreateFlags pass_feedback_flags,feedback_flags;

    DescEntry *desc_entries;
    uint8_t *desc_arena;
    uint32_t desc_table[DESC_TABLE],ndesc;
    size_t desc_arena_used;
    uint32_t npt,pt_open;
    uint64_t pt_draws0;
    struct {uint32_t q0,q1,w,h,n;uint64_t key,draws;} pt[512];
    TraceCapture trace_captures[512];
    unsigned ntrace_captures;
    struct { void *buf; void *tex; uint32_t w,h,pass,slot; uint64_t serial; } sentinel[16];
    unsigned nsentinel;
} VkSubmission;

#endif
