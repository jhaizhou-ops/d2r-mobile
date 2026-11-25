#pragma once
// Keep the SDK header out of this C-facing header: it defines namespace-scope const objects.
struct OH_NativeXComponent;
#ifdef __cplusplus
extern "C" {
#endif
// Call only on the ArkUI thread, like the XComponent surface/focus callbacks.
// Surface presence alone does not vote: a successfully started game and foreground focus are also required.
void dm_ohos_frame_rate_surface(struct OH_NativeXComponent *component, int present);
void dm_ohos_frame_rate_focus(int foreground);
void dm_ohos_frame_rate_running(int running);
#ifdef __cplusplus
}
#endif
