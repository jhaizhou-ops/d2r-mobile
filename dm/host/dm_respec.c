// 阿卡拉免费无限洗点：走原版第一幕阿卡拉的「重置技能与属性」，只把「这一难度还能洗」的任务位一直留着。
// 原版服务端收「找 NPC 洗点」请求（main+0x6db3f0，包长 5，+1 = NPC 单位号）：在 NPC 洗点表（main+0x2097fa8，每项 0x1c 字节、43 项）里按 NPC 类号找项，
// +5 ≠ 0 = 这个 NPC 能洗，+0x10 = 看哪个任务（阿卡拉 148 → 42）；再看玩家这一难度的任务标志（[[玩家+0x10]+难度×8+0x40]，难度 = 游戏 +0x104）里
// 第 (任务×16−15)>>3 字节（阿卡拉 = 0x52）的位 1「可以洗」：在就调原版整套洗点 main+0x786070（技能 0x7c0f90 + 属性 0x6ee090 …），再调 main+0x718be0
// 把这一位换成位 0「已用掉」——所以原版每个难度只能洗一次。这里每拍把位 1 置回去、位 0 清掉：判断、洗点、扣次数全是原版的，只是次数永远用不完；不花钱、不要道具。
// 客户端菜单（main+0x42da84）看的是客户端自己那份任务标志（[main+0x2d5ec18] 指向的那份，同一字节 0x52）：位 0「已用掉」在就不出「重置」，位 1 在才出。
// 服务端洗完、存档读进来时都可能把位 0 带给客户端，所以两份每拍都对一遍。
// Free unlimited respec at Akara: the original Act I Akara "reset skills and attributes", with the quest bit "this difficulty can still reset" kept set.
// The original server handles the "respec at NPC" request (main+0x6db3f0, packet length 5, +1 = NPC unit id): it looks the NPC class up in the NPC respec table
// (main+0x2097fa8, 0x1c bytes per entry, 43 entries), +5 != 0 = this NPC can reset, +0x10 = which quest (Akara 148 -> 42); then it reads bit 1 "can reset" of byte
// (quest*16-15)>>3 (Akara = 0x52) of the player's quest flags for this difficulty ([[player+0x10]+difficulty*8+0x40], difficulty = game +0x104): when set it calls
// the original full respec main+0x786070 (skills 0x7c0f90 + attributes 0x6ee090 ...), then main+0x718be0 turns the bit into bit 0 "used" -- which is why the
// original allows one reset per difficulty. Here bit 1 is set again every tick: the check, the reset and the bookkeeping are all the original game's, the reset
// simply never runs out (bit 1 set, bit 0 cleared); no gold or item is taken.
// The client menu (main+0x42da84) reads the client's own copy of the quest flags (the one [main+0x2d5ec18] points to, same byte 0x52): with bit 0 "used" set it does
// not offer the reset, with bit 1 set it does. A finished reset or a loaded save can hand bit 0 to the client, so both copies are checked every tick.
#include "dm.h"
#include <stdint.h>
enum { RESPEC_BYTE = 0x52, RESPEC_USED = 1, RESPEC_CAN = 2 };
static uint8_t *quest_flags(uint64_t player, unsigned diff) {
    if (!player || *(uint32_t *)player != 0 || diff > 2) return NULL;   // 只认玩家类单位 | player units only
    uint64_t pd = *(uint64_t *)(player + 0x10), q = pd ? *(uint64_t *)(pd + 0x40 + 8 * (uint64_t)diff) : 0;
    return q ? *(uint8_t **)q : NULL;
}
static int keep(uint8_t *f, int client, unsigned diff) {   // 位 1 置上、位 0 清掉；改了返回 1 | set bit 1, clear bit 0; returns 1 when changed
    uint8_t was = f[RESPEC_BYTE], now = (uint8_t)((was | RESPEC_CAN) & ~RESPEC_USED); if (now == was) return 0;
    f[RESPEC_BYTE] = now;
    static int n; if (n++ < 16) LOG("洗点：%s难度 %u 阿卡拉洗点位 %#x → %#x（原版每难度一次，这里留着可以再洗）", client ? "客户端" : "服务端", diff, was, now);
    return 1;
}
void dm_respec_server_step(uint8_t *b, uint64_t G, uint64_t player) {   // 服务端拍里调（diag.c） | Called in the server tick (diag.c)
    if (!b || !G) return;
    unsigned diff = *(uint8_t *)(G + 0x104); uint8_t *f = quest_flags(player, diff); if (!f) return;
    keep(f, 0, diff);
    uint64_t c = *(uint64_t *)(b + 0x2d5ec18); uint8_t *cf = c ? *(uint8_t **)c : NULL; if (cf) keep(cf, 1, diff);
}
