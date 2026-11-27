#pragma once

#include <stdint.h>
#include <TargetConditionals.h>

// 真鼠标（指针模式）代码：Mac App 与鸿蒙（平板 / 电脑，front 在手柄版界面下把鼠标交给 dm_touch_host_live）都编；iOS / 安卓不编
// Real-mouse (pointer mode) code: built for the Mac App and HarmonyOS (tablet / PC: the front end hands the mouse to dm_touch_host_live while the
// controller layout is up); not built for iOS / Android
#if TARGET_OS_OSX || defined(__OHOS__)
#define DM_MOUSE_LIVE 1
#else
#define DM_MOUSE_LIVE 0
#endif
void dm_touch_host_live_enable(void);
void dm_touch_host_live(int op, float nx, float ny);
int dm_touch_host_is_live(void);

int dm_touch_host_enabled(void);
void dm_touch_host_game_start(uint64_t now);
void dm_touch_host_step(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);

int dm_touch_host_world_active(void);   // 游戏线程：当前是否允许消费 Mac 鼠标世界指令；脚本不受此限制 | Game thread: whether Mac mouse world commands may be consumed now; scripts are unaffected
