#pragma once
struct XblControllerFrame { unsigned buttons=0; int axes[4]{}; };
inline int XblControllerAxis(double value) {
    // Per-axis dead zone. Positive screen Y becomes negative guest Y at the adapter.
    if (!(value>=-1.0e6 && value<=1.0e6)) return 0;
    const double magnitude=value<0?-value:value;
    if(magnitude<=0.15) return 0;
    const double scaled=((magnitude<1?magnitude:1)-0.15)/0.85*32767;
    return (value<0?-1:1)*int(scaled+0.5);
}
struct XblControllerInputs {
    bool foreground=true;
    XblControllerFrame virtualPad{}, touchPad{}, devices[8]{};  // touchPad: iOS touch core (game_touch.cpp), raw — no pulse policy
    unsigned hats[8]{};
    void clear() { virtualPad={};touchPad={};for(auto &device:devices)device={};for(auto &hat:hats)hat=0; }
    void focus(bool enabled) { foreground=enabled;if(!enabled)clear(); }
    void remove(unsigned slot) {if(slot<8){devices[slot]={};hats[slot]=0;}}
    void dpad(unsigned slot,double x,double y) {
        if(!foreground || slot>=8)return;
        hats[slot]=(x < -0.5 ? 4096u : (x > 0.5 ? 16384u : 0u)) |
            (y < -0.5 ? 8192u : (y > 0.5 ? 32768u : 0u));
    }
    void stick(unsigned slot,unsigned side,double x,double y) {
        if(!foreground || slot>=8 || side>1)return;
        devices[slot].axes[side*2]=XblControllerAxis(x);
        devices[slot].axes[side*2+1]=XblControllerAxis(-y);
    }
    XblControllerFrame merged() const {
        XblControllerFrame result{};if(!foreground)return result;
        result=virtualPad;result.buttons|=touchPad.buttons;
        for(unsigned i=0;i<4;i+=2)if(touchPad.axes[i]||touchPad.axes[i+1]){result.axes[i]=touchPad.axes[i];result.axes[i+1]=touchPad.axes[i+1];}
        for(const auto &device:devices)result.buttons|=device.buttons;
        for(const auto hat:hats)result.buttons|=hat;
        for(unsigned side=0;side<2;++side) {
            const unsigned i=side*2;
            if(result.axes[i] || result.axes[i+1])continue;
            for(const auto &device:devices)if(device.axes[i] || device.axes[i+1]) {
                result.axes[i]=device.axes[i];result.axes[i+1]=device.axes[i+1];break;
            }
        }
        return result;
    }
};
