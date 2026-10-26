// 桌面宿主共用的鼠标 / 触点状态机；保留历史文件名，Windows 构建显式纳入。
// Shared desktop-host mouse / touch state machine; retained historical filename, explicitly included by the Windows build.
#pragma once

#include <stdint.h>

int dm_touch_host_enabled(void);
void dm_touch_host_game_start(uint64_t now);
void dm_touch_host_step(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void dm_touch_host_live_enable(void);
enum { DM_TOUCH_HOST_CANCEL = 4 };
void dm_touch_host_live(int op, float nx, float ny);
int dm_touch_host_is_live(void);

int dm_touch_host_world_active(void);   // 游戏线程：当前是否允许消费桌面鼠标世界指令；脚本不受此限制 | Game thread: whether desktop mouse world commands may be consumed now; scripts are unaffected
