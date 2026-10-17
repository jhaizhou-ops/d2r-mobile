// 仅连接公开 SDK 的设备信息查询；没有链接或模拟游戏宿主。
#include "cloud_sync_bridge.h"
#include <napi/native_api.h>
#include <deviceinfo.h>
#include <climits>
#include "platform_checks.h"
#include "gpu_checks.h"
#include "runtime_bridge.h"

#include "game_keyboard.h"
#if !defined(__aarch64__)
#error "The bootstrap currently targets arm64-v8a only."
#endif

namespace {
bool SetText(napi_env env, napi_value object, const char *key, const char *text)
{
    napi_value value;
    if (text == nullptr) {
        napi_throw_error(env, nullptr, "Device info query returned null");
        return false;
    }
    return napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &value) == napi_ok &&
           napi_set_named_property(env, object, key, value) == napi_ok;
}
bool SetInt(napi_env env, napi_value object, const char *key, int number)
{
    napi_value value;
    return napi_create_int32(env, number, &value) == napi_ok &&
           napi_set_named_property(env, object, key, value) == napi_ok;
}
napi_value GetHostReport(napi_env env, napi_callback_info)
{
#if defined(XBL_GAME_GRAPHICS) && defined(XBL_RUNTIME_EXECUTION)
    const char *hostState="game_integration_in_progress";
#else
    const char *hostState="native_checks_only";
#endif
    napi_value result;
    if (napi_create_object(env, &result) != napi_ok ||
        !SetText(env, result, "hostState", hostState) ||
        !SetText(env, result, "deviceModel", OH_GetProductModel()) ||
        !SetText(env, result, "deviceType", OH_GetDeviceType()) ||
        !SetText(env, result, "osVersion", OH_GetDisplayVersion()) ||
        !SetText(env, result, "deviceAbi", OH_GetAbiList()) ||
        !SetInt(env, result, "runtimeApi", OH_GetSdkApiVersion()) ||
        !SetText(env, result, "compiledAbi", "arm64-v8a") ||
        !SetText(env, result, "compiler", __clang_version__) ||
        !SetInt(env, result, "pointerBits", sizeof(void *) * CHAR_BIT)) {
        bool pending = false;
        napi_is_exception_pending(env, &pending);
        if (!pending) napi_throw_error(env, nullptr, "Failed to create native host report");
        return nullptr;
    }
    return result;
}
napi_value Init(napi_env env, napi_value exports)
{
    napi_property_descriptor properties[] = {
        {"getHostReport", nullptr, GetHostReport, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runPlatformChecks", nullptr, RunPlatformChecks, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runGpuChecks", nullptr, RunGpuChecks, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"presentGpuTest", nullptr, PresentGpuTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runRealShader", nullptr, RunRealShader, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runReal2D", nullptr, RunReal2D, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runRuntimeStartup", nullptr, RunRuntimeStartup, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runGame", nullptr, RunGame, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runtimeProgress", nullptr, RuntimeProgress, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGameFocus", nullptr, SetGameFocus, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGameUiTouch", nullptr, SetGameUiTouch, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGameTouch", nullptr, SetGameTouch, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getGameKeyboard", nullptr, GetGameKeyboard, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"answerGameKeyboard", nullptr, AnswerGameKeyboard, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cancelGameButtons", nullptr, CancelGameButtons, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGameButtons", nullptr, SetGameButtons, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getPresentedFrames", nullptr, GetPresentedFrames, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getScaleMode", nullptr, GetScaleMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getTouchHud", nullptr, GetTouchHudNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cloudSync", nullptr, CloudSync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setTouchControls", nullptr, SetTouchControlsNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setScaleMode", nullptr, SetScaleMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setScale3d", nullptr, SetScale3dNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendSurfaceTouch", nullptr, SendSurfaceTouch, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setInputReceiptBudget", nullptr, SetInputReceiptBudget, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cancelGamePresentation", nullptr, CancelGamePresentation, nullptr, nullptr, nullptr, napi_default, nullptr}
    };
    if (napi_define_properties(env, exports, sizeof(properties)/sizeof(properties[0]), properties) != napi_ok) {
        napi_throw_error(env, nullptr, "Failed to register getHostReport");
        return nullptr;
    }
    BindGpuSurface(env, exports);
    return exports;
}
napi_module module = {1, 0, nullptr, Init, "xblhost", nullptr, {0}};
}
extern "C" __attribute__((constructor)) void RegisterXblHost()
{
    napi_module_register(&module);
}
