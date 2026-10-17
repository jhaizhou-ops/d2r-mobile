#include <cstring>
#include "controller_bridge.h"
#include "controller_policy.h"
#include "button_pulse_policy.h"
#include "game_touch.h"
#include <thread>
#include <condition_variable>
#include <chrono>
#include <GameControllerKit/game_pad.h>
#include <GameControllerKit/game_device.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <map>
#include <string>
bool GameDiagnosticsEnabled();

namespace {
std::mutex inputMutex, lifecycleMutex;
bool stopped=false;
XblControllerInputs inputs;
XblInputSink sink=nullptr;
XblInputSamples inputSamples=nullptr;
XblButtonPulses pulses;
std::condition_variable pulseWake;
bool pulseStop=false;
double pulseNow(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct PulseWorker {
    std::thread thread;
    ~PulseWorker(){
        {std::lock_guard<std::mutex> lock(inputMutex);pulseStop=true;}
        pulseWake.notify_all();if(thread.joinable())thread.join();
    }
} pulseWorker;
std::map<std::string,unsigned> slots;
bool monitoring=false;
void *library=nullptr;
#define API(name) decltype(&name) p##name=nullptr
API(OH_GamePad_LeftThumbstick_RegisterAxisInputMonitor);
API(OH_GamePad_RightThumbstick_RegisterAxisInputMonitor);
API(OH_GamePad_LeftThumbstick_UnregisterAxisInputMonitor);
API(OH_GamePad_RightThumbstick_UnregisterAxisInputMonitor);
API(OH_GamePad_AxisEvent_GetDeviceId);
API(OH_GamePad_AxisEvent_GetXAxisValue);
API(OH_GamePad_AxisEvent_GetYAxisValue);
API(OH_GamePad_AxisEvent_GetZAxisValue);
API(OH_GamePad_AxisEvent_GetRZAxisValue);
API(OH_GameDevice_RegisterDeviceMonitor);
API(OH_GameDevice_UnregisterDeviceMonitor);
API(OH_GameDevice_DeviceEvent_GetChangedType);
API(OH_GameDevice_DeviceEvent_GetDeviceInfo);
API(OH_GameDevice_DeviceInfo_GetDeviceId);
API(OH_GameDevice_DestroyDeviceInfo);
API(OH_GamePad_ButtonA_RegisterButtonInputMonitor);
API(OH_GamePad_ButtonA_UnregisterButtonInputMonitor);
API(OH_GamePad_ButtonB_RegisterButtonInputMonitor);
API(OH_GamePad_ButtonB_UnregisterButtonInputMonitor);
API(OH_GamePad_ButtonX_RegisterButtonInputMonitor);
API(OH_GamePad_ButtonX_UnregisterButtonInputMonitor);
API(OH_GamePad_ButtonY_RegisterButtonInputMonitor);
API(OH_GamePad_ButtonY_UnregisterButtonInputMonitor);
API(OH_GamePad_LeftThumbstick_RegisterButtonInputMonitor);
API(OH_GamePad_LeftThumbstick_UnregisterButtonInputMonitor);
API(OH_GamePad_RightThumbstick_RegisterButtonInputMonitor);
API(OH_GamePad_RightThumbstick_UnregisterButtonInputMonitor);
API(OH_GamePad_LeftShoulder_RegisterButtonInputMonitor);
API(OH_GamePad_LeftShoulder_UnregisterButtonInputMonitor);
API(OH_GamePad_RightShoulder_RegisterButtonInputMonitor);
API(OH_GamePad_RightShoulder_UnregisterButtonInputMonitor);
API(OH_GamePad_LeftTrigger_RegisterButtonInputMonitor);
API(OH_GamePad_LeftTrigger_UnregisterButtonInputMonitor);
API(OH_GamePad_RightTrigger_RegisterButtonInputMonitor);
API(OH_GamePad_RightTrigger_UnregisterButtonInputMonitor);
API(OH_GamePad_ButtonMenu_RegisterButtonInputMonitor);
API(OH_GamePad_ButtonMenu_UnregisterButtonInputMonitor);
API(OH_GamePad_Dpad_LeftButton_RegisterButtonInputMonitor);
API(OH_GamePad_Dpad_LeftButton_UnregisterButtonInputMonitor);
API(OH_GamePad_Dpad_UpButton_RegisterButtonInputMonitor);
API(OH_GamePad_Dpad_UpButton_UnregisterButtonInputMonitor);
API(OH_GamePad_Dpad_RightButton_RegisterButtonInputMonitor);
API(OH_GamePad_Dpad_RightButton_UnregisterButtonInputMonitor);
API(OH_GamePad_Dpad_DownButton_RegisterButtonInputMonitor);
API(OH_GamePad_Dpad_DownButton_UnregisterButtonInputMonitor);
API(OH_GamePad_ButtonEvent_GetDeviceId);
API(OH_GamePad_ButtonEvent_GetButtonAction);
API(OH_GamePad_Dpad_RegisterAxisInputMonitor);
API(OH_GamePad_Dpad_UnregisterAxisInputMonitor);
API(OH_GamePad_AxisEvent_GetHatXAxisValue);
API(OH_GamePad_AxisEvent_GetHatYAxisValue);
#undef API
void publish() {
    if(sink){
        const auto f=inputs.merged();sink(f.buttons,f.axes[0],f.axes[1],f.axes[2],f.axes[3]);
        static unsigned receipts=0;
        if(GameDiagnosticsEnabled() && receipts++<256)
            fprintf(stderr,"[frontend-controller-merged] buttons=%u lx=%d ly=%d rx=%d ry=%d foreground=%d\n",
                f.buttons,f.axes[0],f.axes[1],f.axes[2],f.axes[3],inputs.foreground?1:0);
    }
}
unsigned slotFor(const char *id) {
    auto found=slots.find(id);if(found!=slots.end())return found->second;
    for(unsigned i=0;i<8;++i){bool used=false;for(const auto &s:slots)used|=s.second==i;if(!used){slots.emplace(id,i);return i;}}
    return 8;
}
void axis(const GamePad_AxisEvent *event,unsigned side) {
    char *id=nullptr;double x=0,y=0;
    if(!event || pOH_GamePad_AxisEvent_GetDeviceId(event,&id)!=GAME_CONTROLLER_SUCCESS || !id){free(id);return;}
    // OHOS left stick uses X/Y and right stick uses Z/RZ, not Android MotionEvent axes.
    const auto xrc=side?pOH_GamePad_AxisEvent_GetZAxisValue(event,&x):pOH_GamePad_AxisEvent_GetXAxisValue(event,&x);
    const auto yrc=side?pOH_GamePad_AxisEvent_GetRZAxisValue(event,&y):pOH_GamePad_AxisEvent_GetYAxisValue(event,&y);
    {std::lock_guard<std::mutex> lock(inputMutex);
        if(monitoring && inputs.foreground && xrc==GAME_CONTROLLER_SUCCESS && yrc==GAME_CONTROLLER_SUCCESS){inputs.stick(slotFor(id),side,x,y);publish();}}
    free(id); // Official StringUtils::ConvertToCharPtrArray allocates with malloc.
}
void leftAxis(const GamePad_AxisEvent *event){axis(event,0);}
void rightAxis(const GamePad_AxisEvent *event){axis(event,1);}
void dpadAxis(const GamePad_AxisEvent *event) {
    char *id=nullptr;double x=0,y=0;
    if(!event || pOH_GamePad_AxisEvent_GetDeviceId(event,&id)!=GAME_CONTROLLER_SUCCESS || !id){free(id);return;}
    const auto xrc=pOH_GamePad_AxisEvent_GetHatXAxisValue(event,&x);
    const auto yrc=pOH_GamePad_AxisEvent_GetHatYAxisValue(event,&y);
    {std::lock_guard<std::mutex> lock(inputMutex);
        if(monitoring && inputs.foreground && xrc==GAME_CONTROLLER_SUCCESS && yrc==GAME_CONTROLLER_SUCCESS){inputs.dpad(slotFor(id),x,y);publish();}}
    free(id);
}
template<unsigned Mask> void button(const GamePad_ButtonEvent *event) {
    char *id=nullptr;GamePad_Button_ActionType action=UP;
    if(!event || pOH_GamePad_ButtonEvent_GetDeviceId(event,&id)!=GAME_CONTROLLER_SUCCESS || !id){free(id);return;}
    if(pOH_GamePad_ButtonEvent_GetButtonAction(event,&action)==GAME_CONTROLLER_SUCCESS) {
        std::lock_guard<std::mutex> lock(inputMutex);
        if(monitoring && inputs.foreground) {
            const auto found=slots.find(id);
            const unsigned slot=action==DOWN?slotFor(id):(found!=slots.end()?found->second:8);
            if(slot<8){if(action==DOWN)inputs.devices[slot].buttons|=Mask;else inputs.devices[slot].buttons&=~Mask;publish();}
        }
    }
    free(id);
}
void deviceChanged(const GameDevice_DeviceEvent *event) {
    GameDevice_StatusChangedType type=ONLINE;
    if(!event || pOH_GameDevice_DeviceEvent_GetChangedType(event,&type)!=GAME_CONTROLLER_SUCCESS || type!=OFFLINE)return;
    GameDevice_DeviceInfo *info=nullptr;char *id=nullptr;bool identified=false;
    if(pOH_GameDevice_DeviceEvent_GetDeviceInfo(event,&info)==GAME_CONTROLLER_SUCCESS && info) {
        if(pOH_GameDevice_DeviceInfo_GetDeviceId(info,&id)==GAME_CONTROLLER_SUCCESS && id) {
            identified=true;
            std::lock_guard<std::mutex> lock(inputMutex);
            const auto found=slots.find(id);if(found!=slots.end()){inputs.remove(found->second);slots.erase(found);publish();}
        }
        pOH_GameDevice_DestroyDeviceInfo(&info);
    }
    if(!identified) {
        // If the offline event cannot identify its device, release physical inputs conservatively.
        // Keep screen controls intact; stale physical axes must not survive an allocation/API failure.
        std::lock_guard<std::mutex> lock(inputMutex);
        for(unsigned i=0;i<8;++i)inputs.remove(i);slots.clear();publish();
    }
    free(id);
}
void unregisterButtons() {
    if(pOH_GamePad_Dpad_UnregisterAxisInputMonitor)pOH_GamePad_Dpad_UnregisterAxisInputMonitor();
    if(pOH_GamePad_ButtonA_UnregisterButtonInputMonitor)pOH_GamePad_ButtonA_UnregisterButtonInputMonitor();
    if(pOH_GamePad_ButtonB_UnregisterButtonInputMonitor)pOH_GamePad_ButtonB_UnregisterButtonInputMonitor();
    if(pOH_GamePad_ButtonX_UnregisterButtonInputMonitor)pOH_GamePad_ButtonX_UnregisterButtonInputMonitor();
    if(pOH_GamePad_ButtonY_UnregisterButtonInputMonitor)pOH_GamePad_ButtonY_UnregisterButtonInputMonitor();
    if(pOH_GamePad_LeftThumbstick_UnregisterButtonInputMonitor)pOH_GamePad_LeftThumbstick_UnregisterButtonInputMonitor();
    if(pOH_GamePad_RightThumbstick_UnregisterButtonInputMonitor)pOH_GamePad_RightThumbstick_UnregisterButtonInputMonitor();
    if(pOH_GamePad_LeftShoulder_UnregisterButtonInputMonitor)pOH_GamePad_LeftShoulder_UnregisterButtonInputMonitor();
    if(pOH_GamePad_RightShoulder_UnregisterButtonInputMonitor)pOH_GamePad_RightShoulder_UnregisterButtonInputMonitor();
    if(pOH_GamePad_LeftTrigger_UnregisterButtonInputMonitor)pOH_GamePad_LeftTrigger_UnregisterButtonInputMonitor();
    if(pOH_GamePad_RightTrigger_UnregisterButtonInputMonitor)pOH_GamePad_RightTrigger_UnregisterButtonInputMonitor();
    if(pOH_GamePad_ButtonMenu_UnregisterButtonInputMonitor)pOH_GamePad_ButtonMenu_UnregisterButtonInputMonitor();
    if(pOH_GamePad_Dpad_LeftButton_UnregisterButtonInputMonitor)pOH_GamePad_Dpad_LeftButton_UnregisterButtonInputMonitor();
    if(pOH_GamePad_Dpad_UpButton_UnregisterButtonInputMonitor)pOH_GamePad_Dpad_UpButton_UnregisterButtonInputMonitor();
    if(pOH_GamePad_Dpad_RightButton_UnregisterButtonInputMonitor)pOH_GamePad_Dpad_RightButton_UnregisterButtonInputMonitor();
    if(pOH_GamePad_Dpad_DownButton_UnregisterButtonInputMonitor)pOH_GamePad_Dpad_DownButton_UnregisterButtonInputMonitor();
}
bool load() {
    library=dlopen("libohgame_controller.z.so",RTLD_NOW|RTLD_LOCAL);
    if(!library)return false;
    bool ok=true;
#define LOAD(name) p##name=reinterpret_cast<decltype(p##name)>(dlsym(library,#name));ok=ok && p##name
    LOAD(OH_GamePad_LeftThumbstick_RegisterAxisInputMonitor);LOAD(OH_GamePad_RightThumbstick_RegisterAxisInputMonitor);
    LOAD(OH_GamePad_LeftThumbstick_UnregisterAxisInputMonitor);LOAD(OH_GamePad_RightThumbstick_UnregisterAxisInputMonitor);
    LOAD(OH_GamePad_AxisEvent_GetDeviceId);LOAD(OH_GamePad_AxisEvent_GetXAxisValue);LOAD(OH_GamePad_AxisEvent_GetYAxisValue);
    LOAD(OH_GamePad_AxisEvent_GetZAxisValue);LOAD(OH_GamePad_AxisEvent_GetRZAxisValue);
    LOAD(OH_GameDevice_RegisterDeviceMonitor);LOAD(OH_GameDevice_UnregisterDeviceMonitor);
    LOAD(OH_GameDevice_DeviceEvent_GetChangedType);LOAD(OH_GameDevice_DeviceEvent_GetDeviceInfo);
    LOAD(OH_GameDevice_DeviceInfo_GetDeviceId);LOAD(OH_GameDevice_DestroyDeviceInfo);
    LOAD(OH_GamePad_ButtonA_RegisterButtonInputMonitor);LOAD(OH_GamePad_ButtonA_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_ButtonB_RegisterButtonInputMonitor);LOAD(OH_GamePad_ButtonB_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_ButtonX_RegisterButtonInputMonitor);LOAD(OH_GamePad_ButtonX_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_ButtonY_RegisterButtonInputMonitor);LOAD(OH_GamePad_ButtonY_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_LeftThumbstick_RegisterButtonInputMonitor);LOAD(OH_GamePad_LeftThumbstick_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_RightThumbstick_RegisterButtonInputMonitor);LOAD(OH_GamePad_RightThumbstick_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_LeftShoulder_RegisterButtonInputMonitor);LOAD(OH_GamePad_LeftShoulder_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_RightShoulder_RegisterButtonInputMonitor);LOAD(OH_GamePad_RightShoulder_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_LeftTrigger_RegisterButtonInputMonitor);LOAD(OH_GamePad_LeftTrigger_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_RightTrigger_RegisterButtonInputMonitor);LOAD(OH_GamePad_RightTrigger_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_ButtonMenu_RegisterButtonInputMonitor);LOAD(OH_GamePad_ButtonMenu_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_Dpad_LeftButton_RegisterButtonInputMonitor);LOAD(OH_GamePad_Dpad_LeftButton_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_Dpad_UpButton_RegisterButtonInputMonitor);LOAD(OH_GamePad_Dpad_UpButton_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_Dpad_RightButton_RegisterButtonInputMonitor);LOAD(OH_GamePad_Dpad_RightButton_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_Dpad_DownButton_RegisterButtonInputMonitor);LOAD(OH_GamePad_Dpad_DownButton_UnregisterButtonInputMonitor);
    LOAD(OH_GamePad_ButtonEvent_GetDeviceId);LOAD(OH_GamePad_ButtonEvent_GetButtonAction);
    LOAD(OH_GamePad_Dpad_RegisterAxisInputMonitor);LOAD(OH_GamePad_Dpad_UnregisterAxisInputMonitor);
    LOAD(OH_GamePad_AxisEvent_GetHatXAxisValue);LOAD(OH_GamePad_AxisEvent_GetHatYAxisValue);
#undef LOAD
    return ok;
}
}
void AttachControllerRuntime(XblInputSink target,XblInputSamples samples) {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);if(stopped)return;
    {std::lock_guard<std::mutex> lock(inputMutex);sink=target;inputSamples=samples;publish();}
    if(samples&&!pulseWorker.thread.joinable())pulseWorker.thread=std::thread([]{
        std::unique_lock<std::mutex> lock(inputMutex);
        while(!pulseStop){
            pulseWake.wait_for(lock,std::chrono::milliseconds(8));
            if(pulseStop)break;
            // Event routes never acquire inputMutex while holding a guest lock.
            lock.unlock();TickGameTouch();lock.lock();
            if(pulseStop)break;
            if(!inputs.foreground||!inputSamples)continue;
            const unsigned mask=pulses.tick(inputSamples(),pulseNow());
            if(mask!=inputs.virtualPad.buttons){inputs.virtualPad.buttons=mask;publish();}
        }
    });
    fprintf(stderr,"[frontend-input-pulse] realSamples=%d minSamples=3 seenHoldMs=120 maxMs=2000 releaseSamples=2 releaseMaxMs=500\n",samples?1:0);
    if(!load()){fprintf(stderr,"[frontend-controller] optional API21 GameController unavailable; screen controls remain active\n");return;}
    {std::lock_guard<std::mutex> lock(inputMutex);monitoring=true;}
    const int device=pOH_GameDevice_RegisterDeviceMonitor(deviceChanged);
    const int left=pOH_GamePad_LeftThumbstick_RegisterAxisInputMonitor(leftAxis);
    const int right=pOH_GamePad_RightThumbstick_RegisterAxisInputMonitor(rightAxis);
    int buttonError=int(pOH_GamePad_Dpad_RegisterAxisInputMonitor(dpadAxis));
    buttonError |= int(pOH_GamePad_ButtonA_RegisterButtonInputMonitor(button<1>));
    buttonError |= int(pOH_GamePad_ButtonB_RegisterButtonInputMonitor(button<2>));
    buttonError |= int(pOH_GamePad_ButtonX_RegisterButtonInputMonitor(button<4>));
    buttonError |= int(pOH_GamePad_ButtonY_RegisterButtonInputMonitor(button<8>));
    buttonError |= int(pOH_GamePad_LeftThumbstick_RegisterButtonInputMonitor(button<16>));
    buttonError |= int(pOH_GamePad_RightThumbstick_RegisterButtonInputMonitor(button<32>));
    buttonError |= int(pOH_GamePad_LeftShoulder_RegisterButtonInputMonitor(button<64>));
    buttonError |= int(pOH_GamePad_RightShoulder_RegisterButtonInputMonitor(button<128>));
    buttonError |= int(pOH_GamePad_LeftTrigger_RegisterButtonInputMonitor(button<256>));
    buttonError |= int(pOH_GamePad_RightTrigger_RegisterButtonInputMonitor(button<512>));
    buttonError |= int(pOH_GamePad_ButtonMenu_RegisterButtonInputMonitor(button<1024>));
    buttonError |= int(pOH_GamePad_Dpad_LeftButton_RegisterButtonInputMonitor(button<4096>));
    buttonError |= int(pOH_GamePad_Dpad_UpButton_RegisterButtonInputMonitor(button<8192>));
    buttonError |= int(pOH_GamePad_Dpad_RightButton_RegisterButtonInputMonitor(button<16384>));
    buttonError |= int(pOH_GamePad_Dpad_DownButton_RegisterButtonInputMonitor(button<32768>));
    fprintf(stderr,"[frontend-controller] deviceMonitor=%d leftAxis=%d rightAxis=%d buttons=%d\n",device,left,right,buttonError);
    if(device || left || right || buttonError) {
        {std::lock_guard<std::mutex> lock(inputMutex);monitoring=false;for(unsigned i=0;i<8;++i)inputs.remove(i);slots.clear();publish();}
        pOH_GamePad_LeftThumbstick_UnregisterAxisInputMonitor();pOH_GamePad_RightThumbstick_UnregisterAxisInputMonitor();pOH_GameDevice_UnregisterDeviceMonitor();unregisterButtons();
    } // Partial registration must not leave an unreleasable stick.
}
void SetControllerForeground(bool foreground) {
    std::lock_guard<std::mutex> lock(inputMutex);inputs.focus(foreground);if(!foreground){slots.clear();pulses.clear();}publish();
}
void SetVirtualController(unsigned buttons,const int *axes) {
    std::lock_guard<std::mutex> lock(inputMutex);
    inputs.virtualPad={};if(inputs.foreground){
        if(axes[0]||axes[1])pulses.dropQueued();
        if(inputSamples){const auto n=inputSamples();pulses.request(buttons,n,pulseNow());inputs.virtualPad.buttons=pulses.tick(n,pulseNow());}
        else inputs.virtualPad.buttons=buttons;
        for(unsigned i=0;i<4;++i)inputs.virtualPad.axes[i]=axes[i];
    }publish();pulseWake.notify_all();
}
void SetTouchController(unsigned buttons,const int *axes) {
    std::lock_guard<std::mutex> lock(inputMutex);
    XblControllerFrame f{};if(inputs.foreground){f.buttons=buttons;for(unsigned i=0;i<4;++i)f.axes[i]=axes[i];}
    if(f.buttons==inputs.touchPad.buttons&&!memcmp(f.axes,inputs.touchPad.axes,sizeof f.axes))return;
    if(f.axes[0]||f.axes[1])pulses.dropQueued();
    inputs.touchPad=f;publish();
}
void CancelVirtualController() {
    std::lock_guard<std::mutex> lock(inputMutex);pulses.clear();inputs.virtualPad={};publish();
}
void StopControllers() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);stopped=true;
    {std::lock_guard<std::mutex> lock(inputMutex);monitoring=false;pulses.clear();inputs.clear();slots.clear();pulseStop=true;publish();}
    pulseWake.notify_all();if(pulseWorker.thread.joinable())pulseWorker.thread.join();
    // Do not hold inputMutex: unregister may wait for an in-flight callback.
    if(pOH_GamePad_LeftThumbstick_UnregisterAxisInputMonitor)pOH_GamePad_LeftThumbstick_UnregisterAxisInputMonitor();
    if(pOH_GamePad_RightThumbstick_UnregisterAxisInputMonitor)pOH_GamePad_RightThumbstick_UnregisterAxisInputMonitor();
    if(pOH_GameDevice_UnregisterDeviceMonitor)pOH_GameDevice_UnregisterDeviceMonitor();
    unregisterButtons();
    // Keep library mapped: callback lifetime is owned by the platform, no dlclose race.
}
