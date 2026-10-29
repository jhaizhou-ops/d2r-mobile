// 共用 Core Animation 秘境任务栏；前端提供当前游戏可见矩形。
// Shared Core Animation rift HUD; each front end supplies its visible game rectangle.
#pragma once
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
void dm_rift_hud_draw(NSView *host, CGRect world_rect, BOOL hide);
#else
#import <UIKit/UIKit.h>
void dm_rift_hud_draw(UIView *host, CGRect world_rect, BOOL hide);
#endif
