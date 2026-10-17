#pragma once
#include <napi/native_api.h>
#include <cstdint>
napi_value RunGpuChecks(napi_env env, napi_callback_info info);
napi_value SendSurfaceTouch(napi_env env, napi_callback_info info);
napi_value RunRealShader(napi_env env, napi_callback_info info);
napi_value RunReal2D(napi_env env, napi_callback_info info);
napi_value PresentGpuTest(napi_env env, napi_callback_info info);
void BindGpuSurface(napi_env env, napi_value exports);
struct NativeWindow;
using OHNativeWindow = NativeWindow;
OHNativeWindow *AcquireGameSurface();
bool IsGameSurfaceCurrent(OHNativeWindow *window);
struct GameSurfaceSnapshot { OHNativeWindow *window; uint64_t width,height,generation; };
bool WaitForGameSurface(GameSurfaceSnapshot &snapshot);
bool WaitForGameSurfaceRefresh(uint64_t generation,GameSurfaceSnapshot &snapshot);
void SetGameSurfaceForeground(bool foreground);
void CancelGameSurfaceWait();
bool IsGameSurfaceWaiting();
