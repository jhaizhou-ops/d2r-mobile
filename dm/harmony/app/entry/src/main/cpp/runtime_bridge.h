#pragma once
#include <napi/native_api.h>
bool GameDiagnosticsEnabled();
napi_value RunRuntimeStartup(napi_env env,napi_callback_info info);
napi_value RunGame(napi_env env,napi_callback_info info);
napi_value RuntimeProgress(napi_env env,napi_callback_info info);
napi_value SetGameFocus(napi_env env,napi_callback_info info);
napi_value SetGameTouch(napi_env env,napi_callback_info info);
napi_value SetGameButtons(napi_env env,napi_callback_info info);
napi_value GetPresentedFrames(napi_env env,napi_callback_info info);
napi_value GetScaleMode(napi_env env,napi_callback_info info);
napi_value GetTouchHudNapi(napi_env env,napi_callback_info info);
napi_value SetTouchControlsNapi(napi_env env,napi_callback_info info);
napi_value SetScaleMode(napi_env env,napi_callback_info info);
napi_value SetScale3dNapi(napi_env env,napi_callback_info info);
napi_value SetInputReceiptBudget(napi_env env,napi_callback_info info);
napi_value CancelGamePresentation(napi_env env,napi_callback_info info);

napi_value CancelGameButtons(napi_env env,napi_callback_info info);

napi_value SetGameUiTouch(napi_env env,napi_callback_info info);

int DispatchGameUiTouch(unsigned phase,float x,float y);
