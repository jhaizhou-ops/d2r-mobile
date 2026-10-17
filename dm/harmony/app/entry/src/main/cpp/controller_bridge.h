#pragma once
#include <cstdint>
using XblInputSink=void(*)(uint64_t,int32_t,int32_t,int32_t,int32_t);
using XblInputSamples=uint64_t(*)();
void AttachControllerRuntime(XblInputSink sink,XblInputSamples samples);
void CancelVirtualController();
void SetControllerForeground(bool foreground);
void SetVirtualController(unsigned buttons,const int *axes);
void SetTouchController(unsigned buttons,const int *axes); // touch core output, published as-is
void StopControllers();
