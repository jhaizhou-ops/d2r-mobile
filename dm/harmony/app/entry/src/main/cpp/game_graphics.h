#pragma once
#include <cstdint>
#include <string>
using GuestInvoke = uint64_t (*)(void *,void *,const uint64_t *,uint32_t);
using GuestFatal = void (*)(void *,const char *);
bool InitializeGameGraphics(const std::string &files,GuestInvoke invoke,GuestFatal fatal,std::string &report);
void *ResolveGameGraphics(const char *name);
std::string GameGraphicsStats();
uint64_t GamePresentedFrames();
bool GameSurfacePaused();
