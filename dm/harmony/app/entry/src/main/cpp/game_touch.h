#pragma once
#include <stdint.h>
void AttachGameTouchRuntime(void *handle);
void CancelGameTouch();
void SetGameTouchForeground(bool foreground);
void TickGameTouch();
void SetGameTouchSurface(uint64_t width,uint64_t height); // surface size before the first touch (HUD needs it)
int GetTouchHud(double *out,int n);
void SetTouchControlsEnabled(bool on);
void SetTouchHudV4(bool on);   // full iOS touch overlay in use (bottom bar v4 drawn by the game)
/* 1 permits existing frontend-only UI handler; 0 consumes/rejects. */
int RouteGameTouch(unsigned phase,int32_t id,float x,float y,
                   uint64_t surface_width,uint64_t surface_height,int64_t event_ns);
