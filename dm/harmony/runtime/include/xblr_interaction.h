#ifndef XBLR_INTERACTION_H
#define XBLR_INTERACTION_H
#include "xblr_execution.h"
#ifdef __cplusplus
extern "C" {
#endif
/* ABI 1: 96 bytes. Fields are host-published observations, never guest pointers.
 * Each validity bit is independent; zero is unknown, not false/inactive.
 * T0 INPUT/UI_TAP/CANCEL retain xblr_set_input/xblr_ui_touch entry points.
 * T1 cursor/panels and T2 world target requests require future capability bits;
 * T3 inventory/drag/shop/skill assignment is deliberately not advertised. */
enum {XBLR_INTERACTION_VIEWPORT=1,XBLR_INTERACTION_UI_RECT=2,
      XBLR_INTERACTION_MODE=4,XBLR_INTERACTION_TARGETS=8};
enum {XBLR_INTERACTION_INPUT=1,XBLR_INTERACTION_UI_TAP=2,
      XBLR_INTERACTION_CANCEL=4,XBLR_INTERACTION_CURSOR=8,
      XBLR_INTERACTION_WORLD_TAP=16};
enum {XBLR_INTERACTION_FRONTEND=0,XBLR_INTERACTION_GAME=1};
/* navigation: 0 game, 1 UI/loading, 2 death modal, 3 NPC dialogue only.
 * MODE validity covers both mode and navigation. It does not imply that
 * world/drag/panel business requests are implemented: check capabilities. */
typedef struct xblr_interaction_snapshot {
    uint32_t abi_version,struct_bytes;
    uint64_t generation,present_frame;
    uint32_t valid,capabilities,mode,navigation;
    uint32_t viewport_width,viewport_height;
    float ui_x,ui_y,ui_width,ui_height;
    uint32_t enabled_targets,reserved;
    uint64_t completed_request;
    int32_t request_result;
    uint32_t reserved_tail[3];
} xblr_interaction_snapshot;
/* Exactly sizeof(snapshot). 0 coherent copy, 40 invalid, 47 lock contended.
 * No guest read on caller thread. Before first main getter valid=0.
 * Rect is normalized rendered-content coordinates only when UI_RECT is set.
 * enabled_targets bits match SW_TC_MAIN..SW_TC_MAP, meaningful only if valid.
 */
XBLR_API int xblr_get_interaction_snapshot(xblr_interaction_snapshot *out,size_t bytes);
/* Candidate T2 API: requires WORLD_TAP and valid&15==15, game/navigation=0.
 * 0 accepted queue only; 40 bad argument; 47 unsupported/not ready.
 * Caller output remains unchanged on failure. A newer request supersedes the
 * previous world action on the next main pump; no guest call on caller thread.
 * completed_request is the most recent terminal request, not a history queue.
 * result -2 cancelled, -1 unsafe/unavailable, 0 empty, 1 item, 2 NPC, 3 object,
 * 4 exit, 5 corpse means native action was issued, NOT completed by the game.
 * WORLD_TAP remains disabled until the candidate integration is validated. */
XBLR_API int xblr_world_tap(float nx,float ny,uint64_t *request);
/* Idempotent asynchronous cancel; pending press releases on the main pump. */
XBLR_API void xblr_cancel_world(void);
#ifdef __cplusplus
}
#endif
#endif
