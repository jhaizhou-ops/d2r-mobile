#ifndef DM_AMMO_H
#define DM_AMMO_H

#include <stdint.h>

// 技能钮上的剩余弹药（箭矢 / 弩矢 / 投掷武器数量） | Remaining ammo on the skill buttons (arrows / bolts / throwing weapon count)
//
// 数据源（2026-11-13 真机实测）：物品自己的属性 stat 70（quantity）。箭袋在装备格 5（组 I 副手），
// 投掷武器就是主手武器本身；射 3 箭 85 → 82 已验证。
// Data source (measured on the device 2026-11-13): the item's own stat 70 (quantity). The quiver sits in
// equipment cell 5 (set I off hand) and a throwing weapon carries its own count; three shots took 85 -> 82.

typedef struct {
    uint64_t item;    // 提供数量的那件物品（箭袋 / 投掷武器） | the item that carries the count (quiver / throwing weapon)
    uint64_t weapon;  // 当前武器组的主手武器（判定技能要什么装备时用） | the main-hand weapon of the current set (used to test what the skill requires)
    int qty;          // 剩余数量（stat 70） | remaining count (stat 70)
    int kind;         // 0 = 没有弹药来源，1 = 箭袋（弓 / 弩），2 = 投掷武器自身 | 0 = no ammo source, 1 = quiver (bow / crossbow), 2 = the throwing weapon itself
    int set;          // 来源组：0 = 当前武器组，1 = 另一组 | source set: 0 = the current weapon set, 1 = the other one
} dm_ammo_src;

// 每帧算一次：当前武器组的弹药来源。返回 1 = 有来源 | Once per frame: the ammo source of the current weapon set. Returns 1 when there is one
int dm_ammo_source(uint8_t *b, uint64_t pl, dm_ammo_src *out);

// 这个技能吃不吃弹药：>= 0 = 吃（返回剩余数量），-1 = 不吃 / 不该显示 | Whether this skill consumes ammo: >= 0 = yes (remaining count), -1 = no / do not show
int dm_ammo_for_skill(uint8_t *b, uint64_t pl, const dm_ammo_src *src, int skill);

// 服务端发布 / 读取（diag.c 的 hud_tick 每拍发布；前端用它，不信客户端那份背包） | server publish / read (hud_tick in diag.c publishes every tick; the front end uses this instead of the client's inventory copy)
void dm_ammo_publish(uint8_t *b, uint64_t srv_player);
int dm_ammo_get(dm_ammo_src *o);

#endif
