// nn::vi（显示层）：真机经 vi 服务开显示、建图层、取原生窗口句柄；句柄交给 NVN 的窗口 builder。
// 宿主：返回宿主静态对象；接 Metal 时原生窗口对应前端的 CAMetalLayer（front_metal_layer()）。
// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"

#ifdef __ANDROID__
#include "../android/nvn_platform_backend.h"
#include <android/native_window.h>
static struct { uint32_t magic; void *native; } android_display,android_layer;
static void *android_vi_window(void) {
    ANativeWindow *w=dm_android_native_window();
    if(!w || dm_vk_native_window()!=w || ANativeWindow_getWidth(w)<=0 || ANativeWindow_getHeight(w)<=0)
        die("Android vi requires a live front-end Surface bound to Vulkan");
    return w;
}
HLE_FORCE("_ZN2nn2vi10InitializeEv", "Android display initialization validates the actual bound front-end Surface", void,h_vi_Init,void) {
    void *w=android_vi_window();android_display.magic=0x50534944;android_display.native=w;
    android_layer.magic=0;android_layer.native=NULL;
    LOG("[vi-android] initialized actual Surface %p",w);
}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "Android default display is backed by the current front-end Surface", Result,h_vi_OpenDisp,void **out) {
    if(!out || android_display.magic!=0x50534944 || android_display.native!=android_vi_window())return RES_FAIL;
    *out=&android_display;return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "Android layer validates the opened display and bound Surface", Result,h_vi_CreateLayer,void **out,void *display) {
    if(!out || display!=&android_display || android_display.magic!=0x50534944 || android_display.native!=android_vi_window())return RES_FAIL;
    android_layer.magic=0x5259414c;android_layer.native=android_display.native;*out=&android_layer;return RES_OK;
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "Android native window is the actual acquired ANativeWindow used by the Vulkan Surface", Result,h_vi_GetNative,void **out,void *layer) {
    if(!out || layer!=&android_layer || android_layer.magic!=0x5259414c || android_layer.native!=android_vi_window())return RES_FAIL;
    *out=android_layer.native;return RES_OK;
}
#else

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "显示服务初始化；宿主无 vi 服务", void, h_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "开默认显示；宿主给静态对象", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "建图层；宿主给静态对象", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "原生窗口句柄，交给 NVN 窗口 builder；宿主给静态对象", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
#endif
