#pragma once
#include "touch_core_session.h"
#include "touch_core_pulse.h"
#include "touch_interaction_policy.h"

/* Frontend values-only adapter. Caller serializes events/ticks/cancel and
 * publishes core.buttons/axes directly as a separate controller source;
 * never feed these outputs through the ordinary button pulse queue twice. */
typedef struct XblGameTouchMapper {
    XblTouchCoreSession session;
    XblCorePulse pulse;
    XblTouchContext context;
    unsigned world_seen;
    uint64_t world_request;
    /* Only visible frontend-owned controls may capture their target circles.
     * Zero is the initial WORLD-only candidate until touch HUD is drawn. */
    uint32_t drawn_targets;
    int ready;
} XblGameTouchMapper;
typedef int (*XblWorldTapRequest)(float,float,uint64_t*);
#ifdef __cplusplus
extern "C" {
#endif
void XblGameTouchCancel(XblGameTouchMapper *m);
int XblGameTouchObserve(XblGameTouchMapper *m,const xblr_interaction_snapshot *snapshot,
                       float logical_width,float logical_height);
int XblGameTouchEvent(XblGameTouchMapper *m,unsigned phase,int32_t id,
                     float normalized_x,float normalized_y,double timestamp_s,
                     XblWorldTapRequest request);
int XblGameTouchTick(XblGameTouchMapper *m,uint64_t samples,double now_s);
#ifdef __cplusplus
}
#endif
