// iOS front_ios.m consumes the same host drop event; OHOS uses its phone vibrator.
// Durations approximate three impact tiers; they are not iOS actuator waveforms.
#include "haptics_ohos.h"
#include <sensors/vibrator.h>
#include <stdatomic.h>
#include <hilog/log.h>
extern int dm_drop_haptic_take(void);
static _Atomic int active = 1, last_error;
void dm_ohos_haptics_focus(int value) {
    atomic_store(&active, !!value);
    if (!value) { dm_drop_haptic_take(); OH_Vibrator_Cancel(); }
}
void dm_ohos_haptics_tick(void) {
    int tier = dm_drop_haptic_take();
    if (!atomic_load(&active) || tier < 1 || tier > 3) return;
    const int duration[] = {0, 35, 22, 12};
    Vibrator_Attribute attr = { .vibratorId = 0, .usage = VIBRATOR_USAGE_PHYSICAL_FEEDBACK };
    int rc = OH_Vibrator_PlayVibration(duration[tier], attr);
    int previous = atomic_exchange(&last_error, rc);
    if (rc && rc != previous) OH_LOG_Print(LOG_APP, LOG_WARN, 0xD2E0, "dm-haptics", "vibration failed rc=%{public}d", rc);
}
int dm_ohos_haptics_last_error(void) { return atomic_load(&last_error); }
