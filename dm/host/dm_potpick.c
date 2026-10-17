// 药水自动拾取：腰带不满时自动拾取对应的药水。自定义页开关 pot_pick，缺省开。
// 走到地上药水跟前（PICK_R 格内）且原版判定这瓶此刻能进腰带，就替玩家发原版的捡物品包，之后全走原版：
//   · 能不能进腰带 = 原版 main+0x3c1b28(物品)&1：物品类型可进腰带 + 原版腰带找位 0x561de0（同类药那一列有空 / 有空列；满了不捡）。
//     「对应」就是原版这条规则：空列哪种药都放得进，非空列只放同一种（玩家点地上药水时服务端也按它放）。只捡红 / 蓝 / 紫（itemtypes 回复 hpot 77、法力 mpot 78，恢复 rpot 的父类型是 hpot；0x574bc0 含父类型）：解毒、解冻、耐力药不捡，免得占腰带格
//   · 捡 = 客户端发包 main+0x2ceab0(包, 17)：[16][u32 物品 id][u32 x][u32 y][u32 1]——与玩家点地上物品、跑到跟前后原版发的那一包逐字节同（宿主 c2s 实录）
// 手上拿着东西（物品栏 +0x40）、玩家死了时不捡。同一瓶发过一次 2 秒内不再发（服务端没收就是原版不让，别连发）；全局每 10 帧最多一包。
// 「能进腰带才捡、捡了就进腰带」要在服务端处理那个包的那一刻兑现：这里发包时的判断是客户端此刻看到的腰带，会过期——
//   上一瓶的结果还没回来（暂停菜单 / 选项页停拍、卡顿、切后台）就接着发下一瓶，到服务端时腰带已满，原版改放背包，背包也满就被「背包放不下改进方块」规则送进魔盒
//   （宿主实测：同一刻三瓶，一瓶进腰带、两瓶进魔盒）。所以：
//   · 主防线在服务端收包处（diag.c c2s_deliver → dm_potpick_auto）：自动拾取发出的那件，处理那一刻用原版腰带找位 0x561de0 对服务端状态再核一次，放不进就丢包，药留在地上；
//   这里发包的节奏不变（每 10 帧最多一包、同一瓶 2 秒内不重发）；没有自然复现出连发，不另加客户端前置（防的情形服务端那一道已兜住）。
// 实测过、不成立的两条猜想（别再加保护）：① 魔盒面板开着时地上的药照样进腰带（dm_beltauto.c 那个「开着方块不收」是背包 → 腰带那条路，不是捡地上的）；
//   ② 服务端卡顿不会造成连发——服务端拍和客户端帧在同一条线程上，拍入口卡 1.5 秒整帧一起停（帧时间最大 1506 ms），客户端发不出下一包。
//   原版两处腰带找位的规则对红 / 蓝 / 紫一致：客户端判定传 1（空列可放），服务端捡物品传 0（空列要物品表 autobelt 位），这三类药 autobelt 都是 1。
// Potion auto pickup: when the belt is not full, automatically pick up the matching potions. Custom page switch pot_pick, on by default.
// When the player walks up to a potion on the ground (within PICK_R tiles) and the original game judges that this bottle can go into the belt right now, send the original game's pick-up-item packet on the player's behalf; everything after that is the original game:
//   - can it go into the belt = the original game's main+0x3c1b28(item)&1: the item type is belt-eligible + the original belt slot search 0x561de0 (the column of the same potion kind has room / there is an empty column; not picked up when full).
//     "Matching" is exactly this original rule: an empty column takes any potion, a non-empty column only takes the same kind (the server places by it too when the player taps a potion on the ground). Only red / blue / purple are picked up (itemtypes healing hpot 77, mana mpot 78; the parent type of rejuvenation rpot is hpot; 0x574bc0 includes parent types): antidote, thawing and stamina potions are not picked up, so they do not take belt slots
//   - pick up = client packet send main+0x2ceab0(packet, 17): [16][u32 item id][u32 x][u32 y][u32 1] -- byte-for-byte identical to the packet the original game sends after the player taps a ground item and runs up to it (host c2s recording)
// No pickup while something is held in hand (item container +0x40) or while the player is dead. The same bottle is not sent again within 2 seconds of being sent once (if the server did not take it, the original game disallows it; do not burst); globally at most one packet per 10 frames.
// "Pick up only if it can go into the belt, and it goes into the belt once picked up" has to hold at the moment the server handles that packet: the check made here when sending is the belt as the client sees it now, which goes stale --
//   if the next bottle is sent before the previous one's result has come back (pause menu / options page stopping ticks, stutter, backgrounding), the belt is already full when it reaches the server, the original game puts it in the inventory instead, and if the inventory is also full the "does not fit in the inventory, goes to the cube" rule sends it to the Horadric Cube
//   (measured on the host: three bottles at the same moment, one went to the belt, two to the cube). Therefore:
//   - the main line of defense is at the server's packet receive point (diag.c c2s_deliver -> dm_potpick_auto): for an item sent by auto pickup, at the moment of handling re-check against server state with the original belt slot search 0x561de0; if it does not fit, drop the packet and the potion stays on the ground;
//   the sending cadence here is unchanged (at most one packet per 10 frames, the same bottle not resent within 2 seconds); bursts were never reproduced naturally, so no extra client-side precheck is added (the server-side guard already covers the case).
// Two hypotheses that were measured and do not hold (do not add protection for them again): (1) with the cube panel open, potions on the ground still go into the belt (the "not accepted while the cube is open" in dm_beltauto.c is the inventory -> belt path, not ground pickup);
//   (2) server stutter does not cause bursts -- the server tick and the client frame run on the same thread; a 1.5 second stall at tick entry stops the whole frame with it (max frame time 1506 ms), so the client cannot send the next packet.
//   The original game's two belt slot search rules agree for red / blue / purple: the client check passes 1 (empty columns allowed), the server item pickup passes 0 (empty columns require the autobelt bit in the item table), and autobelt is 1 for all three kinds.
#include "hle.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int dm_custom_get(const char *key);
uint64_t dm_guest_frame(void);
#define PICK_R 3   // 格；玩家点药水时原版跑到 2–3 格内就发 0x16（实录：跑到点后隔 2 拍发） | Tiles; when the player taps a potion, the original game sends 0x16 once within 2–3 tiles (recorded: sent 2 ticks after arriving at the spot)

static uint32_t g_tried[8], g_tried_fr[8];
// 自动拾取发出、还没有结果的那几件（任意线程读：服务端收包处问「这个 0x16 是不是自动拾取发的」）。发出 5 秒内算数
// The items sent by auto pickup that have no result yet (read from any thread: the server's packet receive point asks "was this 0x16 sent by auto pickup"). Valid within 5 seconds of sending
#include <stdatomic.h>
static _Atomic uint64_t g_auto[4];   // 高 32 位 = 物品 id，低 32 位 = 发包时的帧号（低 32 位） | High 32 bits = item id, low 32 bits = frame number when the packet was sent (low 32 bits)
static void auto_note(uint32_t id, uint64_t fr) { static unsigned k; atomic_store(&g_auto[k++ % 4], (uint64_t)id << 32 | (uint32_t)fr); }
int dm_potpick_auto(uint32_t id) { uint32_t fr = (uint32_t)dm_guest_frame(); for (int i = 0; i < 4; i++) { uint64_t v = atomic_load(&g_auto[i]); if (v && (uint32_t)(v >> 32) == id && fr - (uint32_t)v < 300) return 1; } return 0; }
uint32_t dm_cur_tick(void);

// 同一刻够得着的有好几瓶时先捡哪瓶：大紫 > 小紫 > 红 / 蓝高档 > 低档；同档按物品链先遇到的。
// 只管先后，能不能进腰带仍是原版那条判定；不给紫药预留空列（空列红红蓝蓝、缺色才用紫的规则不变）
// Which bottle to pick up first when several are within reach at the same moment: large purple > small purple > higher-grade red / blue > lower-grade; within the same grade, whichever comes first in the item chain.
// This only decides the order; whether it can go into the belt is still the original game's check; no empty column is reserved for purple potions (the rule that empty columns fail red blue blue and purple is used only when a color is missing is unchanged)
// 明确的自动拾取白名单，独立于原版类型继承和掉落显示过滤；未知物品返回 0，不自动拾取。
// Explicit auto-pickup allowlist, independent of native type inheritance and drop-label filtering; unknown items return 0 and are not auto-picked.
static int pot_code_rank(uint32_t c) {
    const unsigned char *k = (const unsigned char *)&c;
    if (k[0] == 'r' && k[1] == 'v') return k[2] == 'l' ? 20 : k[2] == 's' ? 19 : 0;
    return (k[0] == 'h' || k[0] == 'm') && k[1] == 'p' && k[2] >= '1' && k[2] <= '5' ? k[2] - '0' : 0;
}
static void pick_send(uint8_t *b, uint64_t it, uint32_t id, int x, int y, int px, int py, uint64_t fr) {
    int slot = 0; for (int i = 0; i < 8; i++) { if (g_tried[i] == id) { slot = i; break; } if (g_tried_fr[i] < g_tried_fr[slot]) slot = i; }
    g_tried[slot] = id; g_tried_fr[slot] = (uint32_t)fr; auto_note(id, fr);
    uint8_t pk[17] = { 0x16 }; uint32_t one = 1; memcpy(pk + 1, &id, 4); memcpy(pk + 5, &x, 4); memcpy(pk + 9, &y, 4); memcpy(pk + 13, &one, 4);
    ((void (*)(const uint8_t *, int))(b + 0x2ceab0))(pk, 17);
    uint32_t c = ((uint32_t (*)(uint64_t))(b + 0x570260))(it);
    LOG("药水自动拾取：物品 id %u（%.3s）类号 %u 在 %d,%d（玩家 %d,%d）→ 发捡起包（帧 %llu 拍 %u）", id, (const char *)&c, *(uint32_t *)(it + 4), x, y, px, py, (unsigned long long)fr, dm_cur_tick());
}

void dm_potpick_step(uint8_t *b) {
    // 掉落过滤（dm_lootfilter.c）借这个每帧入口：按难度切当前配置；测试读数（缺省不做事）
    // The loot filter (dm_lootfilter.c) borrows this per-frame entry: switches the current profile by difficulty; test readout (does nothing by default)
    if (b) { void dm_lootfilter_pump(uint8_t *, uint64_t); void dm_lootfilter_test_dump(uint8_t *); dm_lootfilter_pump(b, ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c))); dm_lootfilter_test_dump(b); }
    if (b) { void dm_autobuff_pump(uint8_t *, uint64_t); void dm_autobuff_test_drop(uint64_t); uint64_t pl = ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)); dm_autobuff_test_drop(pl); dm_autobuff_pump(b, pl); }   // 增益技能自动补（dm_autobuff.c）也借这个每帧入口 | buff auto-recast (dm_autobuff.c) also borrows this per-frame entry
    static int env = -2; if (env == -2) { const char *e = getenv("DM_POT_PICK"); env = e ? e[0] != '0' : -1; }   // 测试强制：DM_POT_PICK=0 关 / 其余开，优先于自定义页 | Test override: DM_POT_PICK=0 off / anything else on; takes priority over the customization page
    if (!b || !(env >= 0 ? env : dm_custom_get("pot_pick"))) return;
    static uint64_t next; uint64_t fr = dm_guest_frame(); if (fr < next) return;
    // 测试开关（造现场 / 单验服务端那一道）：DM_TEST_POTPICK_FROM=<帧> 到这一帧才开始自动拾取；DM_TEST_POTPICK_BURST=1 同一帧把够条件的全发出去
    // ——等于把「客户端判断过期就连发」直接摆出来，看服务端收包处拦不拦得住
    // Test switches (test setup / verifying the server-side guard alone): DM_TEST_POTPICK_FROM=<frame> starts auto pickup only from this frame; DM_TEST_POTPICK_BURST=1 sends everything eligible in the same frame
    // -- which directly stages "the client's judgment is stale so it fires in a burst", to see whether the server's packet receive point blocks it
    static long t_from = -1; static int t_burst; if (t_from < 0) { const char *e = getenv("DM_TEST_POTPICK_FROM"); t_from = e ? atol(e) : 0; t_burst = getenv("DM_TEST_POTPICK_BURST") != NULL; }
    if (fr < (uint64_t)t_from) return;
    uint64_t pl = ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)); if (!pl) return;
    uint32_t pm = *(uint32_t *)(pl + 0xc); if (pm == 0 || pm == 17) return;   // 死亡中 / 已死 | Dying / dead
    uint64_t inv = *(uint64_t *)(pl + 0x90); if (!inv || *(uint32_t *)inv != 0x01020304 || *(uint64_t *)(inv + 0x40)) return;   // 手上有东西 | Something is held in hand
    uint64_t pp = *(uint64_t *)(pl + 0x38); if (!pp) return; int px = *(uint16_t *)(pp + 2), py = *(uint16_t *)(pp + 6);
    uint64_t best = 0; uint32_t bid = 0; int brank = 0, bx = 0, by = 0;
    for (int k = 0; k < 128; k++) for (uint64_t it = *(uint64_t *)(b + 0x2d603a0 + 4 * 0x400 + k * 8); it; it = *(uint64_t *)(it + 0x158)) {
        if (*(uint32_t *)(it + 0xc) != 3) continue;   // 地上 | On the ground
        uint64_t ip = *(uint64_t *)(it + 0x38); if (!ip) continue; int x = *(int32_t *)(ip + 0x10), y = *(int32_t *)(ip + 0x14);
        if (abs(x - px) > PICK_R || abs(y - py) > PICK_R) continue;
        { int dm_lootfilter_hidden(uint8_t *, uint64_t); if (dm_lootfilter_hidden(b, it)) continue; }   // 被原版掉落过滤藏着的（屏上没名字）不自动捡 | items hidden by the game's loot filter (no name on screen) are not auto-picked
        uint32_t code = ((uint32_t (*)(uint64_t))(b + 0x570260))(it);
        int r = pot_code_rank(code); if (!r) continue;
        int (*is)(uint64_t, int) = (int (*)(uint64_t, int))(b + 0x574bc0);
        if (!(is(it, 77) || is(it, 78)) || !(((int (*)(uint64_t))(b + 0x3c1b28))(it) & 1)) continue;   // 回复 hpot 77（恢复药 rpot 的父类型也是它）/ 法力 mpot 78 | Healing hpot 77 (also the parent type of the rejuvenation potion rpot) / mana mpot 78
        uint32_t id = *(uint32_t *)(it + 8); int seen = 0;
        for (int i = 0; i < 8; i++) if (g_tried[i] == id && fr - g_tried_fr[i] < 120) seen = 1;
        if (seen) continue;
        if (t_burst) { pick_send(b, it, id, x, y, px, py, fr); continue; }
        if (r > brank) { brank = r; best = it; bid = id; bx = x; by = y; }
    }
    if (best) { pick_send(b, best, bid, bx, by, px, py, fr); next = fr + 10; }
}
