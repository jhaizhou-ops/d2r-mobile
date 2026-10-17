#pragma once
#include "touch-core/ios/Sources/sw_touch_controls.h"

/* Serialized frontend policy, derived from original front_ios.m pulseTick.
 * Not wired to OHOS yet. Call after publishing each new core pulse. A getter
 * sample is transport evidence only, never proof of a completed game action.
 * cursor_pending < 0 means unavailable: pointer pulses cannot pass as delivered.
 */
typedef struct XblCorePulse {
    uint64_t epoch, samples, last_samples;
    double began, first_seen, first_free, last_now;
    int tracking, seen, freed, gap, initialized;
} XblCorePulse;
enum { XBL_PULSE_IDLE=0, XBL_PULSE_RELEASED=1, XBL_PULSE_RESUMED=2,
       XBL_PULSE_TIMEOUT=3, XBL_PULSE_INVALID=40 };
#ifdef __cplusplus
extern "C" {
#endif
void XblCorePulseClear(XblCorePulse *p);
int XblCorePulsePoll(XblCorePulse *p, sw_tc_state *core,
                     uint64_t actual_guest_samples, double now_s, int cursor_pending);
#ifdef __cplusplus
}
#endif
