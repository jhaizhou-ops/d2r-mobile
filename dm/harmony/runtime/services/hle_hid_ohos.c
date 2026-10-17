#include "hle_ohos_compat.h"
#include <stdatomic.h>
enum {NPAD_HANDHELD=0x20,STYLE_FULLKEY=1,STYLE_HANDHELD=2,ATTR_CONNECTED=1};
typedef struct {int64_t sampling;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,pad;} NpadState;
_Static_assert(sizeof(NpadState)==0x28,"original NpadState ABI");
static pthread_mutex_t input_lock=PTHREAD_MUTEX_INITIALIZER;
static NpadState input;
static uint64_t input_samples;
uint64_t xblr_get_input_samples(void) {
    pthread_mutex_lock(&input_lock);uint64_t n=input_samples;pthread_mutex_unlock(&input_lock);return n;
}
static _Atomic uint32_t supported_styles=3;
static uint64_t supported_ids=1ull|(1ull<<NPAD_HANDHELD);
#include "input_receipt_ohos.inc"
static uint32_t style_of(uint32_t id) {
    if(id>NPAD_HANDHELD)return 0;
    pthread_mutex_lock(&input_lock);int supported=(supported_ids>>id)&1;pthread_mutex_unlock(&input_lock);
    if(!supported||id!=(sw_opmode()==0?NPAD_HANDHELD:0))return 0;
    return (id==NPAD_HANDHELD?STYLE_HANDHELD:STYLE_FULLKEY)&supported_styles;
}
static int32_t axis(int32_t v) {return v < -32767 ? -32767 : v > 32767 ? 32767 : v;}
void xblr_set_input(uint64_t buttons,int32_t lx,int32_t ly,int32_t rx,int32_t ry) {
    pthread_mutex_lock(&input_lock);input.buttons=buttons;input.lx=axis(lx);input.ly=axis(ly);input.rx=axis(rx);input.ry=axis(ry);pthread_mutex_unlock(&input_lock);xblr_world_note_stick(lx,ly);
}
static void fill(NpadState *s,uint32_t id,uint32_t style) {
    xblr_pump_ui_touch();
    uint64_t auto_buttons=xblr_pump_ui_frontend();
    uint32_t connected=style_of(id)&style;
    pthread_mutex_lock(&input_lock);*s=input;s->sampling=++input.sampling;
    if(connected){s->attributes=ATTR_CONNECTED;s->buttons|=auto_buttons;}
    else {int64_t sampling=s->sampling;memset(s,0,sizeof *s);s->sampling=sampling;}
    input_receipt_return(s,id,style);
    ++input_samples;
    pthread_mutex_unlock(&input_lock);
}
HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "连 hid 服务；宿主从 sw_input 取输入", void, h_hid_InitNpad, void) {}
HLE_FORCE("_ZN2nn3hid21InitializeTouchScreenEv", "连 hid 服务；触屏输入由前端另接", void, h_hid_InitTouch, void) {}
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "支持的手柄号；宿主固定", void, h_hid_SetIds, const uint32_t *ids, uint64_t n) { if(n>10)die("unsupported Npad id count");uint64_t set=0;for(uint64_t i=0;i<n;i++){if(ids[i]>NPAD_HANDHELD)die("unsupported Npad id");set|=1ull<<ids[i];}pthread_mutex_lock(&input_lock);supported_ids=set;pthread_mutex_unlock(&input_lock); }
// BitFlagSet<32> 按值传在 w 寄存器
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "支持的手柄样式；宿主固定", void, h_hid_SetStyles, uint32_t styles) { supported_styles=styles; }
HLE_FORCE("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", "掌机手柄激活方式；宿主固定", void, h_hid_SetHhMode, int m) { (void)m; }
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "手柄样式查询；跟 SW_OPMODE 走", uint32_t, h_hid_GetStyle, const uint32_t *id) { return style_of(*id); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", "全键手柄状态；取 sw_input", void, h_hid_GetFullKey, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", "掌机手柄状态；取 sw_input", void, h_hid_GetHandheld, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_HANDHELD); }

// 震动：设备数 0，游戏不会再初始化 / 发送
HLE_FORCE("_ZN2nn3hid20IsVibrationPermittedEv", "震动许可；宿主不支持（返回 bool）", unsigned, h_hid_VibPermit, void) { return 0; }
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
          "震动设备；宿主 0 个", int, h_hid_GetVibHandles, void *out, int max, const uint32_t *id, uint32_t style) { (void)out; (void)max; (void)id; (void)style; return 0; }
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "震动；宿主不支持", void, h_hid_InitVib, const void *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "震动；宿主不支持", void, h_hid_SendVib, const void *h, const void *v) { (void)h; (void)v; }

// 触屏：nn::hid::TouchScreenState<16> = { int64 sampling; int32 count; int32 _r; TouchState touches[16]（各 0x28） }
// 内容由 hle_touch.c 按前端推来的触点填（布局与短按锁存见该文件）；游戏只看第 0 份，返回写入的状态份数 1
typedef struct { int64_t sampling; int32_t count, _r; uint8_t touches[16][0x28]; } TouchState16;
_Static_assert(sizeof(TouchState16) == 0x290, "TouchScreenState<16>");
int touch_fill(void *out, uint64_t now_ns);
HLE_FORCE("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", "触屏状态：前端触点（hle_touch.c）", int, h_hid_GetTouch, TouchState16 *out, int n) {
    if (n < 1) return 0;
    touch_fill(out, guest_ns());
    return 1;
}
