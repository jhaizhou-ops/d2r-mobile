// Win32 Vulkan 前端初始化，在 nnMain 前调用；资源和设备由前端持有。
// Win32 Vulkan initialization, called before nnMain; the frontend owns resources and device.
#pragma once
#include <windows.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int dm_windows_vk_initialize(HWND window, const char *shaders, char *error, size_t size);
#ifdef __cplusplus
}
#endif
