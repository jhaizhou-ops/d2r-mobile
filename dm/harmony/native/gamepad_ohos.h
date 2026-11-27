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
/* PC (2in1) boot: front end starts in controller mode with keyboard key caps. */
void dm_ohos_gamepad_boot_pad(void);
/* Mouse extras: kind 0 = right button (value 1 down / 0 up) = B; kind 1 = wheel (value +1 up / -1 down) = right stick for 0.12 s. */
void dm_ohos_mouse(int kind, int value);
/* Pointer mode: Return / Esc as the Mac App (context-dependent A or + / B or +). */
void dm_ohos_gamepad_live_keys(int on);
#ifdef __cplusplus
}
#endif
