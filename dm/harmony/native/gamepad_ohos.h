#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Call start/stop on the app UI thread; callbacks may arrive on service threads. */
int dm_ohos_gamepad_start(void);
void dm_ohos_gamepad_stop(void);
void dm_ohos_gamepad_focus(int active);
void dm_ohos_gamepad_touched(void);
int dm_ohos_gamepad_mode(void);
int dm_ohos_gamepad_count(void);
/* XComponent key event (OH_NativeXComponent_KeyCode, down 1 / up 0); returns 1 when the key is a game key (consumed). */
int dm_ohos_keyboard_key(int code, int down);
#ifdef __cplusplus
}
#endif
