// OHOS GameControllerKit -> existing host pad channel. iOS dm_gamepad.m policy:
// independent devices, explicit-input mode switching, 25%/15% stick hysteresis.
#include "gamepad_ohos.h"
#include "../../host/dm_input.h"
#include "../../host/dm_padmap.h"
#include <GameControllerKit/game_pad.h>
#include <GameControllerKit/game_device.h>
#include <hilog/log.h>
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

extern void dm_padmode_request(int pad);
extern int dm_front_navigation(void), dm_freetp_zr_free(int zl);
extern void dm_tp_request(void);
#define PAD_LOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0xD2E0, "dm-pad", __VA_ARGS__)
#define SLOTS 8
static struct pad { char *id; uint64_t buttons, hat; int32_t ax[4]; int xbox, ps; } pads[SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int running, focused = 1, pad_mode, need_center, stick_on[2], glyph, zr_mine;
static uint64_t previous_buttons, previous_zr;
/* Keyboard (Bluetooth / USB, XComponent key events): one more pad, same bits as the Mac app (front_mac.m key_bit):
   J/Space A, K/Esc B, U X, I Y, F/G stick clicks, Q/E L/R, Z/C ZL/ZR, Enter/=/keypad+ Plus, Tab Minus, arrows d-pad, WASD left stick. */
static uint64_t kb_buttons; static int kb_w, kb_a, kb_s, kb_d;
static int button_regs, axis_regs, device_reg;
/* Mouse extras (as the Mac app front_mac.m): right button = B (back / close panel), wheel = right stick up / down for 0.12 s (the controller
   UI scrolls lists with the right stick). Neither switches layouts: they are merged after the "new input -> controller mode" check. */
static uint64_t mouse_buttons; static int wheel_dir, wheel_gen;

static int slot(const char *id, int create) {
    int empty = -1;
    if (!id || !*id) return -1;
    for (int i = 0; i < SLOTS; ++i) {
        if (pads[i].id && !strcmp(pads[i].id, id)) return i;
        if (!pads[i].id && empty < 0) empty = i;
    }
    if (create && empty >= 0) { pads[empty].id = strdup(id); return pads[empty].id ? empty : -1; }
    return -1;
}
static void mode(int value) {
    if (pad_mode == value) return;
    pad_mode = value; dm_padmode_request(value);
    PAD_LOG("mode=%{public}d", value);
}
static int32_t axis(double value) {
    if (!isfinite(value)) return 0;
    if (value > 1) value = 1;
    if (value < -1) value = -1;
    return (int32_t)lround(value * DM_STICK_MAX);
}
/* lock held. Keep publication ordered with focus/drop and other callbacks. */
static void apply(void) {
    uint64_t b = 0; int32_t a[4] = {0}; double best[2] = {0};
    for (int i = 0; i < SLOTS; ++i) if (pads[i].id) {
        b |= pads[i].buttons | pads[i].hat;
        for (int k = 0; k < 2; ++k) {
            double m = hypot(pads[i].ax[2*k], pads[i].ax[2*k+1]);
            if (m > best[k]) { best[k] = m; a[2*k] = pads[i].ax[2*k]; a[2*k+1] = pads[i].ax[2*k+1]; }
        }
    }
    b |= kb_buttons;
    { int32_t kx = (kb_d - kb_a) * DM_STICK_MAX, ky = (kb_w - kb_s) * DM_STICK_MAX; double m = hypot(kx, ky);
      if (m > best[0]) { best[0] = m > DM_STICK_MAX ? DM_STICK_MAX : m; a[0] = kx; a[1] = ky; } }
    if (!focused || !running) { dm_pad_set(0, 0, 0, 0, 0); return; }
    for (int k = 0; k < 2; ++k) stick_on[k] = best[k] / DM_STICK_MAX >= (stick_on[k] ? .15 : .25);
    if (!stick_on[0] && !stick_on[1]) need_center = 0;
    if ((b & ~previous_buttons) || ((stick_on[0] || stick_on[1]) && !need_center)) {
        dm_padmode_request_glyph(glyph); mode(1);
    }
    previous_buttons = b;
    for (int k = 0; k < 2; ++k) if (!pad_mode || !stick_on[k]) a[2*k] = a[2*k+1] = 0;
    b |= mouse_buttons;
    if (wheel_dir && pad_mode) { a[2] = 0; a[3] = wheel_dir * DM_STICK_MAX; }
    if ((b & DM_BTN_ZR) && !(previous_zr & DM_BTN_ZR)) {
        zr_mine = !dm_front_navigation() && dm_freetp_zr_free((b & DM_BTN_ZL) != 0);
        if (zr_mine) dm_tp_request();
    }
    previous_zr = b;
    if (zr_mine) b &= ~(uint64_t)DM_BTN_ZR;
    if (!(previous_zr & DM_BTN_ZR)) zr_mine = 0;
    dm_pad_set(b, a[0], a[1], a[2], a[3]);
}
static void button(const GamePad_ButtonEvent *event, uint64_t bit) {
    char *id = NULL; GamePad_Button_ActionType action;
    if (OH_GamePad_ButtonEvent_GetDeviceId(event, &id) || !id) return;
    if (OH_GamePad_ButtonEvent_GetButtonAction(event, &action)) { free(id); return; }
    pthread_mutex_lock(&lock);
    int i = running && focused ? slot(id, 1) : -1;
    if (i >= 0) {
        // SDK names face buttons by A/B/X/Y. Xbox uses existing iOS X/Y translation.
        if (pads[i].xbox) { if (bit == DM_BTN_X) bit = DM_BTN_Y; else if (bit == DM_BTN_Y) bit = DM_BTN_X; }
        if (action == DOWN) { if (!(pads[i].buttons & bit)) glyph = pads[i].xbox ? DM_GLYPH_XBOX : pads[i].ps ? DM_GLYPH_PS : DM_GLYPH_PAD; pads[i].buttons |= bit; }
        else if (action == UP) pads[i].buttons &= ~bit;
        apply();
    }
    pthread_mutex_unlock(&lock); free(id);
}
#define BUTTONS(X) \
 X(LeftShoulder, DM_BTN_L) X(RightShoulder, DM_BTN_R) \
 X(LeftTrigger, DM_BTN_ZL) X(RightTrigger, DM_BTN_ZR) \
 X(ButtonMenu, DM_BTN_PLUS) X(ButtonA, DM_BTN_A) X(ButtonB, DM_BTN_B) \
 X(ButtonX, DM_BTN_X) X(ButtonY, DM_BTN_Y) \
 X(Dpad_LeftButton, DM_BTN_LEFT) X(Dpad_RightButton, DM_BTN_RIGHT) \
 X(Dpad_UpButton, DM_BTN_UP) X(Dpad_DownButton, DM_BTN_DOWN) \
 X(LeftThumbstick, DM_BTN_LS) X(RightThumbstick, DM_BTN_RS)
#define CB(name, bit) static void cb_##name(const GamePad_ButtonEvent *e) { button(e, bit); }
BUTTONS(CB)
#undef CB
static struct {
    GameController_ErrorCode (*reg)(GamePad_ButtonInputMonitorCallback);
    GameController_ErrorCode (*unreg)(void);
    GamePad_ButtonInputMonitorCallback callback;
} button_api[] = {
#define API(name, bit) { OH_GamePad_##name##_RegisterButtonInputMonitor, OH_GamePad_##name##_UnregisterButtonInputMonitor, cb_##name },
BUTTONS(API)
#undef API
};
static void axes(const GamePad_AxisEvent *e) {
    char *id = NULL; GamePad_AxisSourceType type; double x = 0, y = 0; int rc = 0;
    if (OH_GamePad_AxisEvent_GetAxisSourceType(e, &type)) return;
    if (type == LEFT_THUMBSTICK) { rc = OH_GamePad_AxisEvent_GetXAxisValue(e, &x); rc |= OH_GamePad_AxisEvent_GetYAxisValue(e, &y); }
    else if (type == RIGHT_THUMBSTICK) { rc = OH_GamePad_AxisEvent_GetZAxisValue(e, &x); rc |= OH_GamePad_AxisEvent_GetRZAxisValue(e, &y); }
    else if (type == DPAD) { rc = OH_GamePad_AxisEvent_GetHatXAxisValue(e, &x); rc |= OH_GamePad_AxisEvent_GetHatYAxisValue(e, &y); }
    else return;
    if (rc || OH_GamePad_AxisEvent_GetDeviceId(e, &id) || !id) return;
    pthread_mutex_lock(&lock);
    int i = running && focused ? slot(id, 1) : -1;
    if (i >= 0) {
        if (type == DPAD) pads[i].hat = (x < -.5 ? DM_BTN_LEFT : x > .5 ? DM_BTN_RIGHT : 0) | (y < -.5 ? DM_BTN_UP : y > .5 ? DM_BTN_DOWN : 0);
        else {
            int k = type == LEFT_THUMBSTICK ? 0 : 2;
            // OHOS screen axes grow downwards; the host/Nintendo stick grows upwards.
            pads[i].ax[k] = axis(x); pads[i].ax[k+1] = axis(-y);
        }
        if (hypot(x, y) >= .25) glyph = pads[i].xbox ? DM_GLYPH_XBOX : pads[i].ps ? DM_GLYPH_PS : DM_GLYPH_PAD;
        apply();
    }
    pthread_mutex_unlock(&lock); free(id);
}
static struct {
    GameController_ErrorCode (*reg)(GamePad_AxisInputMonitorCallback);
    GameController_ErrorCode (*unreg)(void);
} axis_api[] = {
#define AX(name) { OH_GamePad_##name##_RegisterAxisInputMonitor, OH_GamePad_##name##_UnregisterAxisInputMonitor }
    AX(LeftThumbstick), AX(RightThumbstick), AX(Dpad)
#undef AX
};
static void device_info(const GameDevice_DeviceInfo *info, int online) {
    char *id = NULL, *name = NULL;
    if (OH_GameDevice_DeviceInfo_GetDeviceId(info, &id) || !id) return;
    OH_GameDevice_DeviceInfo_GetName(info, &name);
    pthread_mutex_lock(&lock);
    int i = running ? slot(id, online) : -1;
    if (i >= 0) {
        if (online) {
            pads[i].xbox = name && strcasestr(name, "xbox") != NULL;
            // Only explicit model names: a generic Wireless Controller is ambiguous.
            pads[i].ps = !pads[i].xbox && name && (strcasestr(name, "DualSense") || strcasestr(name, "DualShock"));
            PAD_LOG("connected %{public}s xbox=%{public}d ps=%{public}d", name ? name : "gamepad", pads[i].xbox, pads[i].ps);
        } else {
            free(pads[i].id); memset(&pads[i], 0, sizeof pads[i]); apply();
            int any = 0; for (int k = 0; k < SLOTS; ++k) any |= pads[k].id != NULL;
            if (!any) mode(0);
        }
    }
    pthread_mutex_unlock(&lock); free(id); free(name);
}
static void device_event(const GameDevice_DeviceEvent *event) {
    GameDevice_StatusChangedType changed; GameDevice_DeviceInfo *info = NULL;
    if (OH_GameDevice_DeviceEvent_GetChangedType(event, &changed) || OH_GameDevice_DeviceEvent_GetDeviceInfo(event, &info)) return;
    if (info) { device_info(info, changed == ONLINE); OH_GameDevice_DestroyDeviceInfo(&info); }
}
void dm_ohos_gamepad_focus(int active) {
    pthread_mutex_lock(&lock); focused = !!active;
    if (!active) {
        for (int i = 0; i < SLOTS; ++i) { pads[i].buttons = pads[i].hat = 0; memset(pads[i].ax, 0, sizeof pads[i].ax); }
        previous_buttons = previous_zr = 0; zr_mine = 0; stick_on[0] = stick_on[1] = 0; need_center = 1;
        dm_pad_set(0, 0, 0, 0, 0);
    }
    pthread_mutex_unlock(&lock);
}
void dm_ohos_gamepad_touched(void) {
    pthread_mutex_lock(&lock);
    if (pad_mode) { need_center = 1; mode(0); apply(); }
    pthread_mutex_unlock(&lock);
}
int dm_ohos_keyboard_key(int code, int down) {
    uint64_t bit = 0; int *stick = NULL;
    switch (code) {
    case 2026: case 2050: bit = DM_BTN_A; break;            /* J, Space */
    case 2027: case 2070: bit = DM_BTN_B; break;            /* K, Esc */
    case 2037: bit = DM_BTN_X; break; case 2025: bit = DM_BTN_Y; break;   /* U, I */
    case 2022: bit = DM_BTN_LS; break; case 2023: bit = DM_BTN_RS; break; /* F, G */
    case 2033: bit = DM_BTN_L; break; case 2021: bit = DM_BTN_R; break;   /* Q, E */
    case 2042: bit = DM_BTN_ZL; break; case 2019: bit = DM_BTN_ZR; break; /* Z, C */
    case 2054: case 2058: case 2116: bit = DM_BTN_PLUS; break;  /* Enter, =, keypad + */
    case 2049: bit = DM_BTN_MINUS; break;                   /* Tab */
    case 2014: bit = DM_BTN_LEFT; break; case 2012: bit = DM_BTN_UP; break; case 2015: bit = DM_BTN_RIGHT; break; case 2013: bit = DM_BTN_DOWN; break;
    case 2039: stick = &kb_w; break; case 2017: stick = &kb_a; break; case 2035: stick = &kb_s; break; case 2020: stick = &kb_d; break;  /* W A S D */
    default: return 0;
    }
    pthread_mutex_lock(&lock);
    if (down) glyph = DM_GLYPH_KBD;
    if (stick) *stick = !!down; else kb_buttons = down ? (kb_buttons | bit) : (kb_buttons & ~bit);
    apply(); pthread_mutex_unlock(&lock);
    return 1;
}
/* PC (2in1): the host boots in the controller layout (DM_PAD_BOOT); the front end starts there too, with keyboard key caps. */
void dm_ohos_gamepad_boot_pad(void) { pthread_mutex_lock(&lock); pad_mode = 1; glyph = DM_GLYPH_KBD; pthread_mutex_unlock(&lock); }
static void *wheel_off(void *p) {
    int g = (int)(intptr_t)p; usleep(120000);
    pthread_mutex_lock(&lock); if (g == wheel_gen) { wheel_dir = 0; apply(); } pthread_mutex_unlock(&lock); return NULL;
}
void dm_ohos_mouse(int kind, int value) {
    pthread_mutex_lock(&lock);
    if (kind == 0) mouse_buttons = value ? DM_BTN_B : 0;
    else if (kind == 1 && value) { wheel_dir = value > 0 ? 1 : -1; int g = ++wheel_gen; pthread_t t;
        if (!pthread_create(&t, NULL, wheel_off, (void *)(intptr_t)g)) pthread_detach(t); else wheel_dir = 0; }
    apply(); pthread_mutex_unlock(&lock);
    static int told; if (!told) { told = 1; PAD_LOG("mouse kind=%{public}d value=%{public}d", kind, value); fprintf(stderr, "鼠标：%s\n", kind ? "滚轮 = 右摇杆上下" : "右键 = B（返回）"); }
}
int dm_ohos_gamepad_mode(void) { pthread_mutex_lock(&lock); int m = pad_mode; pthread_mutex_unlock(&lock); return m; }
int dm_ohos_gamepad_count(void) { pthread_mutex_lock(&lock); int n = 0; for (int i = 0; i < SLOTS; ++i) n += pads[i].id != NULL; pthread_mutex_unlock(&lock); return n; }
void dm_ohos_gamepad_stop(void) {
    pthread_mutex_lock(&lock); running = 0; dm_pad_set(0, 0, 0, 0, 0); pthread_mutex_unlock(&lock);
    for (int i = 0; i < button_regs; ++i) button_api[i].unreg(); button_regs = 0;
    for (int i = 0; i < axis_regs; ++i) axis_api[i].unreg(); axis_regs = 0;
    if (device_reg) OH_GameDevice_UnregisterDeviceMonitor(); device_reg = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SLOTS; ++i) { free(pads[i].id); memset(&pads[i], 0, sizeof pads[i]); }
    previous_buttons = previous_zr = 0; zr_mine = 0; need_center = 0; stick_on[0] = stick_on[1] = 0; mode(0);
    pthread_mutex_unlock(&lock);
}
int dm_ohos_gamepad_start(void) {
    pthread_mutex_lock(&lock); if (running) { pthread_mutex_unlock(&lock); return 0; } running = 1; pthread_mutex_unlock(&lock);
    int rc = OH_GameDevice_RegisterDeviceMonitor(device_event);
    if (rc) goto fail;
    device_reg = 1;
    for (int i = 0; i < (int)(sizeof button_api / sizeof button_api[0]); ++i) { rc = button_api[i].reg(button_api[i].callback); if (rc) goto fail; ++button_regs; }
    for (int i = 0; i < (int)(sizeof axis_api / sizeof axis_api[0]); ++i) { rc = axis_api[i].reg(axes); if (rc) goto fail; ++axis_regs; }
    GameDevice_AllDeviceInfos *all = NULL;
    if (!OH_GameDevice_GetAllDeviceInfos(&all) && all) {
        int32_t count = 0; OH_GameDevice_AllDeviceInfos_GetCount(all, &count);
        for (int32_t i = 0; i < count; ++i) {
            GameDevice_DeviceInfo *info = NULL;
            if (!OH_GameDevice_AllDeviceInfos_GetDeviceInfo(all, i, &info) && info) { device_info(info, 1); OH_GameDevice_DestroyDeviceInfo(&info); }
        }
        OH_GameDevice_DestroyAllDeviceInfos(&all);
    }
    PAD_LOG("monitors ready; Select/Home are not remapped"); return 0;
fail:
    PAD_LOG("monitor registration failed rc=%{public}d", rc); dm_ohos_gamepad_stop(); return rc;
}
