#pragma once

#include <stdint.h>

#define DM_MAC_NAV_LEFT  (1ull << 46)
#define DM_MAC_NAV_UP    (1ull << 47)
#define DM_MAC_NAV_RIGHT (1ull << 48)
#define DM_MAC_NAV_DOWN  (1ull << 49)
#define DM_MAC_POTION_1  (1ull << 50)
#define DM_MAC_POTION_2  (1ull << 51)
#define DM_MAC_POTION_3  (1ull << 52)
#define DM_MAC_POTION_4  (1ull << 53)
#define DM_MAC_ZOOM (1ull << 54)
#define DM_MAC_NAV_MASK (15ull << 46)
#define DM_MAC_POTION_MASK (15ull << 50)

int dm_touch_host_enabled(void);
void dm_touch_host_game_start(uint64_t now);
void dm_touch_host_step(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);

int dm_touch_host_world_active(void);   // 游戏线程：当前是否允许消费 Mac 鼠标世界指令；脚本不受此限制 | Game thread: whether Mac mouse world commands may be consumed now; scripts are unaffected

// dm_touch_host_live op3 is PR95 hover outside PC mode; use the explicit mouse API for non-PC right down.
// AppKit publishes coordinates; only the game thread computes aim.
void dm_touch_host_pc_mouse(int button,int phase,float nx,float ny,int side); // side: primary=0, secondary=1, no world action=-1.
void dm_touch_host_pc_mouse_mods(int button,int phase,float nx,float ny,int side,int modifiers);
void dm_touch_host_pointer(float nx,float ny,int valid);
int dm_touch_host_world_pointer(float *nx,float *ny); // Current physical PC pointer, classified in UI space and converted to render space once.
void dm_touch_host_aim(uint64_t skills, float nx, float ny);
