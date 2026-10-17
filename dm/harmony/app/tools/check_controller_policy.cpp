#include "../entry/src/main/cpp/controller_policy.h"
extern "C" { int _fltused=0; }
int main() {
    if(XblControllerAxis(0.15)!=0 || XblControllerAxis(1)!=32767 || XblControllerAxis(-2)!=-32767)return 1;
    XblControllerInputs input;
    input.stick(0,0,1,-1);input.stick(0,1,-1,1);
    auto f=input.merged();if(f.axes[0]!=32767 || f.axes[1]!=32767 || f.axes[2]!=-32767 || f.axes[3]!=-32767)return 2;
    input.virtualPad.buttons=1;input.virtualPad.axes[0]=100;
    f=input.merged();if(f.buttons!=1 || f.axes[0]!=100 || f.axes[1]!=0 || f.axes[2]!=-32767)return 3;
    input.virtualPad={};f=input.merged();if(f.axes[0]!=32767)return 4;
    input.stick(1,0,-1,0);input.remove(0);f=input.merged();
    if(f.axes[0]!=-32767 || f.axes[2]!=0)return 5;
    input.focus(false);input.stick(1,0,1,1);f=input.merged();if(f.buttons || f.axes[0] || f.axes[1])return 6;
    input.focus(true);f=input.merged();if(f.axes[0] || f.axes[2])return 7;
    input.stick(8,0,1,1);input.stick(0,2,1,1);if(input.merged().axes[0])return 8;
    input.dpad(0,-1,-1);input.devices[0].buttons=1;
    if(input.merged().buttons!=(1|4096|8192))return 10;
    input.dpad(0,0,0);if(input.merged().buttons!=1)return 11;
    input.dpad(1,1,1);input.remove(1);if(input.merged().buttons!=1)return 12;
    volatile double zero=0;const double nan=zero/zero;
    if(XblControllerAxis(nan)!=0)return 9;
#ifdef XBL_CONTROLLER_NEGATIVE
    if(XblControllerAxis(0.1)==0)return 19;
#endif
    return 0;
}
