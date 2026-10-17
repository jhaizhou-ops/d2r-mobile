#pragma once
void CancelGameKeyboard(bool runtimeStarted=true);
#include <napi/native_api.h>
int RegisterGameKeyboard(void *runtime);
napi_value GetGameKeyboard(napi_env env,napi_callback_info info);
napi_value AnswerGameKeyboard(napi_env env,napi_callback_info info);
