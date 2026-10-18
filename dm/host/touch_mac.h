#pragma once

#include <stdint.h>

int dm_touch_host_enabled(void);
void dm_touch_host_game_start(uint64_t now);
void dm_touch_host_step(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);

int dm_touch_host_world_active(void);   // 游戏线程：当前是否允许消费 Mac 鼠标世界指令；脚本不受此限制 | Game thread: whether Mac mouse world commands may be consumed now; scripts are unaffected
