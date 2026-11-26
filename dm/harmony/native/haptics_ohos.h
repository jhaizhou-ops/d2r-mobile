#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* UI tick consumes the host's high-value-drop event, honoring drop_haptic. */
void dm_ohos_haptics_tick(void);
void dm_ohos_haptics_focus(int active);
int dm_ohos_haptics_last_error(void);
/* Directory (the app files dir) where the three drop waveforms are written for OH_Vibrator_PlayVibrationCustom. */
void dm_ohos_haptics_dir(const char *files);
#ifdef __cplusplus
}
#endif
