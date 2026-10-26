// Win32 键位与留边视口坐标换算；桌面回车 / Esc 的上下文键语义复用 touch_mac.c。
// Win32 bindings and letterboxed viewport coordinates; contextual Return / Esc semantics reuse touch_mac.c.
#pragma once
#include <windows.h>

static inline int dm_windows_key_bit(unsigned key) {
    switch (key) {
    case 'J': case VK_SPACE: return 0; case 'K': case VK_RBUTTON: return 1;
    case 'U': return 2; case 'I': return 3; case 'F': return 4; case 'G': return 5;
    case 'Q': return 6; case 'E': return 7; case 'Z': return 8; case 'C': return 9;
    case VK_OEM_PLUS: case VK_ADD: return 10; case VK_TAB: return 11;
    case VK_LEFT: return 12; case VK_UP: return 13; case VK_RIGHT: return 14; case VK_DOWN: return 15;
    case VK_RETURN: return 40; case VK_ESCAPE: return 41;
    default: return -1;
    }
}
static inline int dm_windows_pointer(float width, float height, float aspect, float x, float y, float *nx, float *ny) {
    if (width <= 0 || height <= 0 || aspect <= 0) return 0;
    float w = width, h = width / aspect;
    if (h > height) { h = height; w = height * aspect; }
    *nx = (x - (width - w) / 2) / w;
    *ny = (y - (height - h) / 2) / h;
    return 1;
}
