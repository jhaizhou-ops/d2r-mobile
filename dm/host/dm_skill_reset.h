// 免费技能重置的纯逻辑：存档未用技能点为 8 位，确认必须绑定当前玩家与打开的技能树。
// Pure logic for free skill resets: unspent points use 8 save bits; confirmation belongs to the current player and open tree.
#pragma once
#include <stdint.h>
#include <stddef.h>
static inline int dm_skill_reset_total(int unspent, const int *levels, size_t count) {
    if (unspent < 0 || unspent > 255) return -1;
    for (size_t i = 0; i < count; i++) {
        if (levels[i] < 0 || levels[i] > 255 - unspent) return -1;
        unspent += levels[i];
    }
    return unspent;
}
typedef struct { uint64_t player; int armed; } dm_skill_reset_flow;
// 事件：0 刷新、1 请求、2 确认、3 取消；成功确认只消费一次。
// Events: 0 refresh, 1 request, 2 confirm, 3 cancel; a successful confirmation is consumed once.
static inline int dm_skill_reset_event(dm_skill_reset_flow *f, int event, uint64_t player, int open, int allowed) {
    if (!player || !open || !allowed || (f->armed && f->player != player)) {
        f->armed = 0; f->player = 0; return 0;
    }
    if (event == 1) { f->player = player; f->armed = 1; }
    if (event == 3) f->armed = 0;
    if (event != 2 || !f->armed) return 0;
    f->armed = 0;
    return 1;
}
