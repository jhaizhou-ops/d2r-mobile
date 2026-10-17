#ifndef XBLR_EXECUTION_H
#define XBLR_EXECUTION_H
#include "xblr_loader.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { XBLR_EXEC_READY=0, XBLR_EXEC_ARGUMENT=40, XBLR_EXEC_PAGE_SIZE=41,
       XBLR_EXEC_TLS=42, XBLR_EXEC_UNSUPPORTED_SVC=43, XBLR_EXEC_GUEST_BREAK=44,
       XBLR_EXEC_GATE_LIMIT=45, XBLR_EXEC_BAD_BRANCH=46, XBLR_EXEC_BUSY=47,
       XBLR_EXEC_MISSING_SYMBOL=48, XBLR_EXEC_HOST_FAILURE=49 };
enum { XBLR_STAGE_LIBC0=1, XBLR_STAGE_OS=2, XBLR_STAGE_LIBC1=3,
       XBLR_STAGE_SDK=4, XBLR_STAGE_STARTUP=5, XBLR_STAGE_LIBC2=6,
       XBLR_STAGE_INITS=7, XBLR_STAGE_ARGUMENTS=8, XBLR_STAGE_NNMAIN=9 };
typedef struct xblr_execution_report {
    uint32_t abi_version, struct_bytes, status, stage_entered, stage_completed, page_bytes;
    uint64_t gates, max_gates, original_pc, guest_sp, svc_number, svc_args[6];
    uint64_t relocations_applied, symbol_address, result_x0;
    char symbol[128];
} xblr_execution_report;
/* Invoke from a dedicated worker, at most once per process. Stages after OS
 * initialization can start guest workers and access configured game resources.
 * Stops by longjmp on an explicitly unsupported SVC, with original PC/args.
 * max_stage=2 first exercises the REAL nnosInitialize, not only ret callbacks.
 * max_gates bounds gate traffic; it is not a wall-clock timeout for native loops.
 */
XBLR_API int xblr_execute_startup(uint32_t max_stage,uint64_t max_gates,
    xblr_execution_report *report,size_t report_bytes);
/* Thread-safe snapshot for the UI while nnMain owns its dedicated worker.
 * Stage 9 entered alone is not proof of a frame; combine with graphics presents.
 * active=0 on a worker failure even if the main worker has not unwound yet. */
typedef struct xblr_execution_progress {
    uint32_t abi_version,struct_bytes,started,active,status,stage_entered,stage_completed,reserved;
} xblr_execution_progress;
XBLR_API int xblr_get_execution_progress(xblr_execution_progress *progress,size_t bytes);
/* App-owned existing directories, configured before execution. RomFS is read-only;
 * save/temp must be separate from RomFS and each other. No host path defaults. */
XBLR_API int xblr_configure_game_paths(const char *romfs,const char *save,const char *temporary);
/* Before startup, after game paths: one canonical read-only overlay directory.
 * NULL clears the previous overlay. 40 invalid preserves previous setting;
 * 47 execution started, 49 environment update failed. No colon search list.
 * Must be disjoint from RomFS/save/temp and save .work/.new siblings. */
XBLR_API int xblr_configure_game_overlay(const char *absolute_root);
XBLR_API uint64_t xblr_invoke_guest(void *function,const uint64_t *arguments,uint32_t count);
/* Register graphics resolver before execution; invoked only by real guest NVN calls. */
typedef void *(*xblr_nvn_resolver)(const char *name);
XBLR_API int xblr_set_nvn_resolver(xblr_nvn_resolver resolver);
XBLR_API uint64_t xblr_invoke_guest_callback(void *userdata,void *function,const uint64_t *arguments,uint32_t count);
XBLR_API void xblr_graphics_fatal(void *userdata,const char *message);
/* Original Npad button bits, signed sticks [-32767,32767], touch positions [0,1]. */
typedef struct xblr_touch {uint32_t id;float x,y;} xblr_touch;
XBLR_API void xblr_set_input(uint64_t buttons,int32_t lx,int32_t ly,int32_t rx,int32_t ry);
/* Process-lifetime count of completed FullKey/Handheld getter fills, including
 * disconnected/unsupported queries. Thread-safe; no reset, logging or latching.
 * Input publishing and receipt budget changes do not advance this count.
 * A count change witnesses a getter, not consumption of a particular button. */
XBLR_API uint64_t xblr_get_input_samples(void);
/* Queue one UI mouse tap. phase 0=down,1=move,2=up,3=cancel; normalized
 * rendered-content coordinates. 0=accepted,40=invalid,47=in-flight/busy.
 * Dispatch occurs on the main guest thread after two hover frames, followed
 * by mouse Down and next-frame Up. This is not item dragging or a HID touch. */
XBLR_API int xblr_ui_touch(uint32_t phase,float nx,float ny);
/* Default off, 0 disables, 1..4 arms bounded receipts; lifetime max 4 taps,
 * max 12 lines each. >4 is invalid and preserves state. */
XBLR_API int xblr_set_ui_touch_receipt_budget(uint32_t taps);
/* Optional actual GetNpadState return receipts in the native runtime log.
 * Default off. 0 disables; 1..256 resets a bounded generation, then auto-stops.
 * Records first/changed state per id/style, plus connected reads at most once
 * per second. Host set_input alone never emits a consumed receipt. */
XBLR_API int xblr_set_input_receipt_budget(uint32_t record_limit);
XBLR_API int xblr_set_touches(const xblr_touch *touches,uint32_t count);
/* 1 foreground, 2 unfocused, 3 background. Guest focus is updated even if the
 * audio transition reports XBLR_EXEC_HOST_FAILURE; native error is logged. */
XBLR_API int xblr_set_focus(int focus);
/* Called on the dedicated guest worker. Frontend must marshal UI work itself,
 * then return 1 confirmed / 0 user cancelled / negative error; UTF-8, NUL-ended.
 * No handler is an explicit unsupported-service failure, never fake confirmation. */
typedef int (*xblr_keyboard_handler)(void *userdata,const char *title,const char *subtitle,
    const char *guide,const char *initial,int maximum_utf16_units,char *output,int output_bytes);
XBLR_API int xblr_set_keyboard_handler(xblr_keyboard_handler handler,void *userdata);
XBLR_API uint64_t xblr_audio_render_calls(void);
/* Optional pull-only diagnostic. No timer, logging or guest memory reads.
 * Caller supplies exactly sizeof(snapshot); up to 64 thread rows are copied.
 * BUSY=47 means the thread lock was held: retry on a later polling interval.
 * svc_count/last_svc/last_lr are individually atomic observations, not a
 * transactional CPU sample. wait_kind=0 does not prove CPU execution.
 * wait_kind: 0 none, 1 mutex, 2 condition, 3 address, 4 sync objects.
 * wait_address is populated only for 1..3; wait_key only for 2.
 * ABI1: row 80 bytes, snapshot 5144 bytes, threads offset 24. */
typedef struct xblr_thread_observation {
    uint64_t id,entry,svc_count,last_lr,wait_address,wait_key,wait_sequence;
    uint32_t handle,started,exited,wait_kind,last_svc,reserved;
} xblr_thread_observation;
typedef struct xblr_thread_snapshot {
    uint32_t abi_version,struct_bytes,count,truncated;
    uint64_t monotonic_ns;
    xblr_thread_observation threads[64];
} xblr_thread_snapshot;
XBLR_API int xblr_get_thread_snapshot(xblr_thread_snapshot *snapshot,size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
