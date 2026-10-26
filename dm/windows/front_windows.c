// Win32 原生窗口、键鼠 / XInput 与 Unicode 输入框，复用 front.h 与 Switch 按键契约。
// Native Win32 window, keyboard / mouse / XInput and Unicode input dialog, sharing front.h and Switch button contracts.
#include "../host/dm.h"
#include "../host/front.h"
#include "../host/dm_input.h"
#include "../host/dm_i18n.h"
#include "../host/touch_mac.h"
#include "../host/dm_uiscreen.h"
#include "input.h"
#include "vulkan_present.h"
#include <windowsx.h>
#include <xinput.h>
#include <stdatomic.h>
enum { TEXT_REQUEST = WM_APP + 1, GUEST_FINISHED, SAVE_FAILED };
static HWND window;
static _Atomic int focused;
static unsigned char keys[256];
static uint64_t buttons;
static int wheel, quitting, pointer_down, tracking_mouse;
static void (*guest_entry)(void);
typedef struct {
    const char *title, *sub, *guide, *initial;
    char *out; int capacity, limit, result;
} text_request;
static wchar_t *ui_wide(const char *key) { return dm_windows_wide(dm_ui(key)); }
void *front_metal_layer(void) { return window; }
uint64_t front_game_res(int docked) {
    unsigned w, h; char end;
    const char *value = getenv("DM_GAME_RES");
    if (value && sscanf(value, "%ux%u%c", &w, &h, &end) == 2 && w >= 320 && w <= 4096 && h >= 180 && h <= 4096)
        return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}
double front_display_refresh(char *name, size_t size) {
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info = {.cbSize = sizeof info};
    DEVMODEW mode = {.dmSize = sizeof mode};
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info) || !EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        if (name && size) *name = 0; return 0;
    }
    if (name && size && !WideCharToMultiByte(CP_UTF8, 0, info.szDevice, -1, name, (int)size, NULL, NULL)) *name = 0;
    return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
}
static void push_keys(void) {
    dm_input_set(buttons, ((int)keys['D'] - keys['A']) * DM_STICK_MAX,
        ((int)keys['W'] - keys['S']) * DM_STICK_MAX, 0, wheel * DM_STICK_MAX);
}
static void keyboard(unsigned key, int down) {
    if (key >= 256) return;
    keys[key] = (unsigned char)down;
    buttons = 0;
    for (unsigned i = 0; i < 256; i++) if (keys[i]) { int bit = dm_windows_key_bit(i); if (bit >= 0) buttons |= 1ull << bit; }
    if (down && (dm_windows_key_bit(key) >= 0 || key == 'W' || key == 'A' || key == 'S' || key == 'D')) dm_padmode_request_glyph(DM_GLYPH_KBD);
    push_keys();
}
static int32_t deadzone(SHORT axis, int zone) {
    int value = axis == -32768 ? -32767 : axis;
    return abs(value) <= zone ? 0 : (int32_t)((int64_t)(value > 0 ? value - zone : value + zone) * 32767 / (32767 - zone));
}
static void poll_pad(void) {
    if (!atomic_load(&focused) || !IsWindowEnabled(window)) { dm_pad_set(0, 0, 0, 0, 0); return; }
    XINPUT_STATE state;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) if (XInputGetState(i, &state) == ERROR_SUCCESS) {
        WORD p = state.Gamepad.wButtons;
        static const WORD masks[] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
            XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
            0, 0, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_UP,
            XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN};
        uint64_t b = 0; for (unsigned k = 0; k < 16; k++) if (p & masks[k]) b |= 1ull << k;
        if (state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= DM_BTN_ZL;
        if (state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= DM_BTN_ZR;
        int32_t lx = deadzone(state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE), ly = deadzone(state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        int32_t rx = deadzone(state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE), ry = deadzone(state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        dm_pad_set(b, lx, ly, rx, ry);
        if (b || lx || ly || rx || ry) dm_padmode_request_glyph(DM_GLYPH_XBOX);
        return;
    }
    dm_pad_set(0, 0, 0, 0, 0);
}
static int mouse_live(HWND target, LPARAM position, int operation) {
    RECT rect; GetClientRect(target, &rect);
    int width = 0, height = 0;
    if (!dm_screen_real(&width, &height) || width <= 0 || height <= 0) {
        uint64_t resolution = front_game_res(1); width = (int)(uint32_t)resolution; height = (int)(resolution >> 32);
    }
    float x, y;
    if (!dm_windows_pointer((float)rect.right, (float)rect.bottom, (float)width / height,
                            (float)GET_X_LPARAM(position), (float)GET_Y_LPARAM(position), &x, &y)) return 0;
    int inside = x >= 0 && x <= 1 && y >= 0 && y <= 1;
    if (operation == 0 && !inside) return 0;
    dm_touch_host_live(operation, x, dm_ui_ny(y)); return inside;
}
static void mouse_release(int cancelled) {
    if (!pointer_down) return;
    pointer_down = 0;
    dm_touch_host_live(cancelled ? DM_TOUCH_HOST_CANCEL : 2, 0, 0);
}
static void release_input(HWND target) {
    memset(keys, 0, sizeof keys); buttons = wheel = 0; push_keys(); dm_pad_set(0, 0, 0, 0, 0);
    // 只取消仍按着的手势；已抬起排队的点击 / 放置必须完成，不能因随后失焦作废。
    // Cancel only a held gesture; a queued click/drop already released must survive a later focus change.
    mouse_release(1); dm_touch_host_live(3, -1, -1);
    if (GetCapture() == target) ReleaseCapture();
}
static HWND dialog_control(HWND dialog, const wchar_t *class_, const wchar_t *text, DWORD style, DWORD extra, int id,
                           int x, int y, int width, int height) {
    RECT rect = {x, y, x + width, y + height};
    if (!MapDialogRect(dialog, &rect)) die("MapDialogRect failed: %lu", GetLastError());
    HWND control = CreateWindowExW(extra, class_, text, WS_CHILD | WS_VISIBLE | style,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, dialog, (HMENU)(intptr_t)id, NULL, NULL);
    if (!control) die("CreateWindowExW dialog control failed: %lu", GetLastError());
    return control;
}
static INT_PTR CALLBACK text_dialog(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    text_request *request = (void *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        request = (void *)lparam; SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        wchar_t *title = dm_windows_wide(request->title), *initial = dm_windows_wide(request->initial);
        wchar_t *sub = dm_windows_wide(request->sub), *guide = dm_windows_wide(request->guide);
        wchar_t *ok = ui_wide("dmAppOK"), *cancel = ui_wide("dmCancel");
        if (!title || !initial || !sub || !guide || !ok || !cancel) die("text dialog UTF-8 conversion failed");
        SetWindowTextW(dialog, title);
        dialog_control(dialog, L"STATIC", sub, 0, 0, 0, 8, 6, 224, 16);
        dialog_control(dialog, L"STATIC", guide, 0, 0, 0, 8, 24, 224, 24);
        HWND edit = dialog_control(dialog, L"EDIT", initial, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 100, 8, 50, 224, 16);
        dialog_control(dialog, L"BUTTON", ok, WS_TABSTOP | BS_DEFPUSHBUTTON, 0, IDOK, 120, 78, 54, 18);
        dialog_control(dialog, L"BUTTON", cancel, WS_TABSTOP, 0, IDCANCEL, 178, 78, 54, 18);
        SendMessageW(edit, EM_SETLIMITTEXT, request->limit > 0 ? (WPARAM)request->limit : 32767, 0);
        SetFocus(edit); SendMessageW(edit, EM_SETSEL, 0, -1);
        free(title); free(initial); free(sub); free(guide); free(ok); free(cancel);
        return FALSE;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == IDOK) {
        HWND edit = GetDlgItem(dialog, 100); int n = GetWindowTextLengthW(edit);
        wchar_t *text = calloc((size_t)n + 1, sizeof *text); if (!text) die("text input allocation failed");
        GetWindowTextW(edit, text, n + 1);
        int result = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, request->out, request->capacity, NULL, NULL); free(text);
        if (!result) { LOG("text input exceeds UTF-8 buffer or contains an incomplete character"); MessageBeep(MB_ICONWARNING); return TRUE; }
        request->result = 1; EndDialog(dialog, IDOK); return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL)) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
static void *close_guest(void *unused) {
    (void)unused;
    int dm_save_wait(double max_s, uint64_t *commits, int *froze, double *ms);
    uint64_t commits = 0; int froze = 0; double ms = 0;
    int asked = dm_save_wait(3, &commits, &froze, &ms);
    LOG("Windows close: save request=%d commits=%llu froze=%d elapsed=%.1f ms", asked, (unsigned long long)commits, froze, ms);
    dm_host_shutdown("WM_CLOSE"); return NULL;
}
static LRESULT CALLBACK window_proc(HWND target, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CLOSE:
        if (!quitting) {
            quitting = 1; pthread_t thread;
            int error = pthread_create(&thread, NULL, close_guest, NULL);
            if (error) { quitting = 0; LOG("Windows close thread failed: %d", error); } else pthread_detach(thread);
        }
        return 0;
    case WM_ACTIVATEAPP:
        atomic_store(&focused, !!wparam);
        if (!wparam) release_input(target);
        return 0;
    case WM_KEYDOWN: case WM_KEYUP: keyboard((unsigned)wparam, message == WM_KEYDOWN); return 0;
    case WM_LBUTTONDOWN: if (mouse_live(target, lparam, 0)) { pointer_down = 1; SetCapture(target); } return 0;
    case WM_MOUSEMOVE:
        if (!tracking_mouse) { TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, target, 0};
            tracking_mouse = TrackMouseEvent(&track); if (!tracking_mouse) LOG("TrackMouseEvent failed: %lu", GetLastError()); }
        mouse_live(target, lparam, pointer_down ? 1 : 3); return 0;
    case WM_MOUSELEAVE: tracking_mouse = 0; dm_touch_host_live(3, -1, -1); return 0;
    case WM_LBUTTONUP:
        if (pointer_down) mouse_release(!mouse_live(target, lparam, 1));
        ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: mouse_release(1); return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: keyboard(VK_RBUTTON, message == WM_RBUTTONDOWN); return 0;
    case WM_MOUSEWHEEL: wheel = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1; push_keys(); SetTimer(target, 2, 120, NULL); return 0;
    case WM_TIMER: if (wparam == 1) poll_pad(); else if (wparam == 2) { KillTimer(target, 2); wheel = 0; push_keys(); } return 0;
    case TEXT_REQUEST: {
        text_request *request = (void *)lparam;
        atomic_store(&focused, 0); release_input(target);
        struct { DLGTEMPLATE dialog; WORD menu, class_, title; } template_ = {
            {WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME, 0, 0, 0, 0, 240, 110}, 0, 0, 0};
        INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(NULL), &template_.dialog, target, text_dialog, lparam);
        release_input(target); atomic_store(&focused, GetForegroundWindow() == target);
        if (result == -1) { LOG("DialogBoxIndirectParamW failed: %lu", GetLastError()); request->result = -1; }
        return 0;
    }
    case SAVE_FAILED: {
        wchar_t *text = ui_wide((int)wparam == ENOSPC ? "dmSaveNoSpace" : "dmSaveFailed");
        if (!text) die("save warning UTF-8 conversion failed");
        MessageBoxW(target, text, L"D2R Mobile", MB_OK | MB_ICONERROR); free(text); return 0;
    }
    case GUEST_FINISHED: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(target, message, wparam, lparam);
}
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int capacity) {
    if (!window) return -1;
    text_request request = {title ? title : "", sub ? sub : "", guide ? guide : "", initial ? initial : "", out, capacity, max_chars, 0};
    SendMessageW(window, TEXT_REQUEST, 0, (LPARAM)&request); return request.result;
}
void front_save_failed(int error) {
    LOG("Windows save failed: errno=%d", error);
    if (window && !PostMessageW(window, SAVE_FAILED, (WPARAM)error, 0)) LOG("Windows save warning delivery failed: %lu", GetLastError());
}
static void *guest_main(void *unused) {
    (void)unused; void crash_thread_init(void); crash_thread_init();
    guest_entry(); PostMessageW(window, GUEST_FINISHED, 0, 0); return NULL;
}
void front_run(void (*guest)(void)) {
    const char *enabled = getenv("DM_WINDOW");
    if (enabled && !strcmp(enabled, "0")) { die("Windows Vulkan host requires a window"); }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls = {.lpfnWndProc = window_proc, .hInstance = GetModuleHandleW(NULL),
        .hCursor = LoadCursorW(NULL, IDC_ARROW), .lpszClassName = L"D2RMobileARM64"};
    if (!RegisterClassW(&cls)) die("RegisterClassW failed: %lu", GetLastError());
    RECT rect = {0, 0, 1280, 720}; AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    window = CreateWindowW(cls.lpszClassName, L"D2R Mobile", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, cls.hInstance, NULL);
    if (!window) die("CreateWindowW failed: %lu", GetLastError());
    ShowWindow(window, SW_SHOW); atomic_store(&focused, 1);
    const char *shaders = getenv("DM_VK_SHADER_DIR"); char error[1024];
    if (!shaders || !*shaders) die("Set DM_VK_SHADER_DIR to genuine Vulkan .xgs shader bundles");
    if (dm_windows_vk_initialize(window, shaders, error, sizeof error)) die("%s", error);
    dm_touch_host_live_enable();
    guest_entry = guest; pthread_attr_t attr; pthread_attr_init(&attr);
    if (pthread_attr_setstacksize(&attr, 16u << 20)) die("Windows guest stack size failed");
    pthread_t thread; int result = pthread_create(&thread, &attr, guest_main, NULL); pthread_attr_destroy(&attr);
    if (result) die("Windows guest thread failed: %d", result);
    SetTimer(window, 1, 8, NULL);
    MSG message; int status;
    while ((status = GetMessageW(&message, NULL, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (status < 0) die("GetMessageW failed: %lu", GetLastError());
    pthread_join(thread, NULL);
    dm_host_shutdown("nnMain returned");
}
