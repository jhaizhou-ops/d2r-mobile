#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* UI tick consumes the host's high-value-drop event, honoring drop_haptic. */
void dm_ohos_haptics_tick(void);
void dm_ohos_haptics_focus(int active);
int dm_ohos_haptics_last_error(void);
#ifdef __cplusplus
}
#endif
