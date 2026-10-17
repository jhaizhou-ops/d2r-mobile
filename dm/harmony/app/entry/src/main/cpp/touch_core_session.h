#pragma once
#include "touch_finger_slots.h"
#include "touch-core/ios/Sources/sw_touch_controls.h"

/* Frontend-only event glue. No guest reads, platform calls or runtime ABI.
 * It is not connected to XComponent yet. The caller must validate its runtime
 * context/capability first. Unknown navigation/T3 points are refused outright;
 * they must never fall through to the ordinary UI mouse-click path. */
typedef struct XblTouchCoreSession {
    XblFingerSlots slots;
    sw_tc_state core;
    float width,height;
    double last_event_s;
    uint32_t targets;
    int initialized,game_input_ready;
} XblTouchCoreSession;
enum {XBL_TOUCH_DOWN=0,XBL_TOUCH_MOVE=1,XBL_TOUCH_UP=2,XBL_TOUCH_CANCEL=3};
#ifdef __cplusplus
extern "C" {
#endif
void XblTouchCoreCancel(XblTouchCoreSession *s);
int XblTouchCoreConfigure(XblTouchCoreSession *s,float width,float height,
                         int known_game_input,uint32_t enabled_targets);
int XblTouchCoreEvent(XblTouchCoreSession *s,unsigned phase,int32_t finger_id,
                     float x,float y,double event_time_s);
int XblTouchCoreTick(XblTouchCoreSession *s,double now_s);
#ifdef __cplusplus
}
#endif
