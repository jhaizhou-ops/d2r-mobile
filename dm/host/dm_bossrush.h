#pragma once
// 首领连战：哈洛加斯的门 → 亚瑞特之巅打 5 关 → 门进世界之石大殿，强制恐怖地带打 5 关 → 地狱再加 2 关。实现与依据见 dm_bossrush.c
// Boss rush: the door in Harrogath → 5 stages at the Arreat Summit → a door into the Worldstone Chamber, 5 stages with the terror zone forced → 2 more stages in Hell. For implementation and rationale see dm_bossrush.c
#include <stdint.h>
void dm_bossrush_tick(uint64_t game, uint64_t player, uint8_t *main_base);   // 服务端每拍调（diag.c 读玩家状态处） | called every server tick (where diag.c reads the player state)
void dm_bossrush_c2s(uint64_t game, uint64_t player, const uint8_t *pkt, uint32_t len);   // 客户端命令包分派入口（观测门 OBS_C2S）每包调 | called per packet at the client command packet dispatch entry (observation gate OBS_C2S)
void dm_game_notice(uint8_t *main_base, const char *text);   // 原版系统提示（聊天栏一行 + 系统提示框），服务端线程调 | original-game system notification (one chat bar line + the system notification box), called from the server thread
int dm_bossrush_owns(uint32_t id);                            // 这个怪物单位是首领连战本关刷出来的 | this monster unit was spawned by the current boss rush stage
void dm_kills_tick(uint64_t G, uint64_t player, uint8_t *main_base);   // 刷怪积分牌（dm_kills.c），服务端拍里调 | kill board (dm_kills.c), called in the server tick
void dm_door_label(uint8_t *main_base, uint64_t game, int cls, const char *key, int in_town);   // 我们建的门的名牌：城里把这一类物件的串键换成 key（挂进原版串表），离城写回 | name label of the doors we create: in town the string key of this object class is replaced with key (attached to the original game's string table), and written back on leaving town
