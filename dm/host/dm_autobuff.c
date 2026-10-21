// 增益技能自动补（玩家说的「猴戏」）：自己放过的自身增益技能掉了（到期 / 被打光），人物站着的那一拍替玩家再放一次。第一笔只管自己学的技能，不换武器。
// 名单（原版技能表 skills.txt 筛的：非被动、非光环、给自己上一个状态、不要目标、没有冷却、不是变形 / 诅咒 / 召唤）见 k_ab：
//   技能号、它挂在自己身上的状态号（states.txt 行号）、互斥组（同组的互相顶替，只补玩家最后放的那个；0 = 不互斥）、城里能不能放。
// 怎么知道掉了：读客户端玩家的状态位图（[单位+0x88] 属性表 +0xb30 起，同自动喝药读药效）。剩余时间读不到，所以是「掉了再补」。
// 补谁：不用绑键、不用先手动放一次——角色学了（或当前在手的装备给了：非职业技能建的也是来源 -1 的普通节点）、有效等级 > 0、身上没有，就补。
//   有负面作用的不自动放：见 k_skip（一张带理由的排除表，技能号、名字、理由）。互斥组：本局见过哪个在身上（玩家手动放的）就补哪个，没见过补等级最高的。
// 怎么放：服务端拍里走原版右键施放那一步 main+0x6e77f0(游戏, 玩家, x, y, 1 = 右手, 技能节点)（同免费回城 dm_freetp.c）：法力检查与扣除、施法动作、
//   城里规则都是原版自己的；不占技能栏、不改玩家的左右键绑定（调用期间把右键技能节点临时指过去，调完放回）。
// 什么时候放（都是为了不打扰玩家）：人物站立那一拍（客户端与服务端模式都是 1）——走路会被施法打断，攻击 / 施法中原版自己拒；不在城里（喊叫类在城里
//   调用会被拒、还会把人重置成站立）；没死；手上没拿东西；法力够（宿主先自己比，不够不调——原版在法力不足时会出语音）；一拍只做一件事。
// 节流：发出后 2 秒内状态没出现算一次失败，同一技能隔 3 秒再试（DM_AUTOBUFF_RETRY，拍），连败 3 次本局不再补它并记红。
// 开关：「自定义」设置 auto_buff（缺省开）。在 iOS 和 Mac App 生效；Mac 命令行对拍回放不自动施法；DM_AUTOBUFF=1 / 0 强制。
// 第二笔：副手那套武器给的增益（典型：战争召唤给的战斗指挥 / 战斗体制）。装备给的非职业技能（属性 97，层 = 技能号）只在那套武器在手时才有节点，
//   所以要「换到副手 → 把该补的全放完 → 换回」。换走期间主手与盾的属性全没了，条件比第一笔严：站立、不在城里、没死、手上没拿东西、
//   副手组技能的连招没在进行、周围 DM_AUTOBUFF_SAFE 格（缺省 40）内没有活着的敌对怪。换武器调游戏自己的 main+0x41db90(当前组)（同 dm_actions.c）。
//   任何情况下不把玩家留在副手：换过去后最多等 6 秒（DM_AUTOBUFF_SWAPMAX，拍），到点不管放没放完都换回；换回一直重试到成（每 8 拍一次，死了等复活）。
//   同一个技能本局经换武器补失败 2 次就不再为它换；两轮之间至少隔 10 秒。
// Automatic self-buff recast: when a self-buff you cast yourself drops (expired / dispelled), recast it for the player on a tick where the character is standing. Part 1 only covers skills the character learned and does not swap weapons.
// The list (filtered from the game's skills.txt: not passive, not an aura, puts a state on yourself, needs no target, no cooldown, not a shapeshift / curse / summon) is in k_ab:
//   skill id, the state it puts on yourself (row in states.txt), mutual-exclusion group (skills in one group replace each other; only the one the player cast last is recast; 0 = none), and whether it can be cast in town.
// How we know it dropped: read the client player's state bitmap ([unit+0x88] stat list from +0xb30, same as auto-drink reading potion effects). The remaining time is not readable, so it is "recast after it drops".
// What gets recast: no key binding needed and no manual first cast - if the character has the skill (learned, or granted by the gear in hand: non-class skills from gear are ordinary nodes with source -1), its effective level > 0 and the state is not on, it is recast.
//   Skills with downsides are never auto-cast: see k_skip (an exclusion table with reasons: skill id, name, reason). Mutual-exclusion groups: recast whichever one was seen on the character this game (cast by the player); if none was seen, the highest-level one.
// How it is cast: in the server tick, through the game's own right-hand cast step main+0x6e77f0(game, player, x, y, 1 = right hand, skill node) (same as free town portal, dm_freetp.c): mana check and cost, cast animation
//   and town rules are all the game's own; it takes no skill slot and does not change the player's left / right bindings (the right-hand skill node is pointed at it during the call and restored afterwards).
// When it is cast (all so as not to disturb the player): on a tick when the character is standing (client and server mode both 1), since walking would be interrupted by the cast and the game itself refuses during attack / cast; not in town (warcries called in town
//   are refused and also reset the character to standing); not dead; nothing on the cursor; enough Mana (the host compares first and does not call if short: the game plays a voice line on insufficient Mana); one thing per tick.
// Throttle: if the state has not appeared within 2 seconds of the request it counts as one failure; the same skill is retried 3 seconds later (DM_AUTOBUFF_RETRY, ticks); after 3 failures in a row it is not recast again this game and a failure is logged.
// Switch: "Custom" setting auto_buff (default on). Active in iOS and the Mac App; Mac command-line parity replays do not auto-cast; DM_AUTOBUFF=1 / 0 forces it.
// Part 2: buffs granted by the off-hand weapon set (typical: Battle Command / Battle Orders from Call to Arms). A gear-granted non-class skill (stat 97, layer = skill id) only has a node while that set is in hand,
//   so it takes "swap to off-hand -> cast everything due -> swap back". While swapped away all main-hand and shield stats are gone, so the conditions are stricter than part 1: standing, not in town, not dead, nothing on the cursor,
//   no off-hand-set skill combo in progress, no living hostile monster within DM_AUTOBUFF_SAFE tiles (default 40). The weapon swap calls the game's own main+0x41db90(current set) (same as dm_actions.c).
//   The player is never left on the off-hand: after swapping, wait at most 6 seconds (DM_AUTOBUFF_SWAPMAX, ticks), then swap back whether or not everything was cast; swapping back is retried until it succeeds (every 8 ticks; if dead, wait for resurrection).
//   A skill that fails twice this game through a weapon swap is no longer swapped for; at least 10 seconds between two rounds.
#include "dm.h"
#include "dm_custom.h"
#include "dm_hud.h"
#include "dm_unit.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int16_t skill, state; int8_t grp, intown; const char *nm; } ab_def;
static const ab_def k_ab[] = {
    {  40,  10, 1, 1, "寒冰装甲" }, {  50,  88, 1, 1, "碎冰甲" }, {  60,  20, 1, 1, "寒冰护甲" }, {  52,  16, 0, 1, "魔法附加" }, {  58,  30, 0, 1, "能量护盾" },
    {  68,  14, 0, 1, "骨甲" }, { 117, 101, 0, 1, "圣盾" },
    { 155,  51, 0, 0, "战斗指挥" }, { 149,  32, 0, 0, "战斗体制" }, { 138,  26, 0, 0, "大叫" },   // 次序有讲究：战斗指挥给全技能 +1，先放它，后两个吃到这 1 级（玩家手动也是这个顺序）；循环按表序挑 | order matters: Battle Command gives +1 to all skills, so cast it first and the next two get that level (players do it in this order by hand too); the loop picks in table order
    { 235, 151, 0, 1, "飓风装甲" }, { 258, 157, 2, 1, "速度爆发" }, { 267, 159, 2, 1, "能量消解" }, { 278,  31, 0, 1, "毒牙" },
    { 385, 198, 6, 1, "Hex Bane" }, { 386, 200, 6, 1, "Hex Siphon" }, { 389, 202, 6, 1, "Hex Purge" }, { 387, 206, 0, 1, "Psychic Ward" },
    {  57,  38, 0, 0, "雷云风暴" }, { 277, 158, 0, 0, "刀刃之盾" },
};
// 不自动放的（有负面作用 / 待定）：技能号、名字、理由。名单里没有的技能本来就不会放，这张表管的是「在 k_ab 里、或将来加进去也不许放」的
// Not auto-cast (has downsides / undecided): skill id, name, reason. Skills not on the list are never cast anyway; this table covers those that "are in k_ab, or must not be cast even if added later"
static const struct { int16_t skill; const char *nm, *why; } k_skip[] = {
    { 381, "吞噬 Consume", "有负面作用，产品方点名不自动放" },
};
// Hex Bane 385 / Hex Siphon 386：技能表里的负值（防御、命中、伤害 -33%）落在敌人的减益上，不在自己身上——真局读过自己的属性：放之后防御、伤害百分比不变，命中百分比 +83。所以不在排除表里。
// 装备前提（技能表 itypea1 / etypea1 列）：当前在手的那套（部位 4 / 5）里要有这类装备，且不能是排除的那类。原版在前提不满足时不报错、改放一下普通攻击，
// 所以宿主要先自己判，不满足就不调（否则等于替玩家空挥一刀）。类型号 = 物品类型表行号：51 任意盾、45 武器、69 拳套类
// Hex Bane 385 / Hex Siphon 386: the negative values in the skill table (Defense, Attack Rating, damage -33%) land on the enemy debuff, not on the caster. Own stats were read in a real game: after casting, Defense and damage percent are unchanged, Attack Rating percent +83. So they are not in the exclusion table.
// Gear prerequisite (skill table columns itypea1 / etypea1): the set currently in hand (body locations 4 / 5) must contain that item type and must not be of the excluded type. When the prerequisite is not met the game raises no error and casts a normal attack instead,
// so the host must check first and not call if unmet (otherwise it swings at nothing on the player's behalf). Type id = item type table row: 51 any shield, 45 weapon, 69 hand-to-hand class
static const struct { int16_t skill, need, excl; } k_equip[] = { { 117, 51, 0 }, { 385, 45, 69 }, { 386, 45, 69 }, { 389, 45, 69 } };
static uint32_t g_hand_sig;   // 当前在手两格的物品 id 之和：变了 = 换了武器组 / 穿脱了手上的装备 | sum of the item ids in the two held slots: changed = weapon set swapped / held gear put on or taken off
static int equip_ok(uint8_t *b, uint64_t pl, int skill) {
    int need = 0, excl = 0; for (unsigned i = 0; i < sizeof k_equip / sizeof *k_equip; i++) if (k_equip[i].skill == skill) { need = k_equip[i].need; excl = k_equip[i].excl; }
    uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; int ok = need == 0; uint32_t sig = 0;
    for (int k = 0; it && k < 512; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; int loc = *(uint8_t *)(d + 0x54);
        if (*(uint32_t *)(it + 0xc) == 1 && (loc == 4 || loc == 5)) { sig += *(uint32_t *)(it + 8) * (uint32_t)(loc == 4 ? 3 : 7);
            int (*is)(uint64_t, int) = (int (*)(uint64_t, int))(b + 0x574bc0); if (need && is(it, need) && !(excl && is(it, excl))) ok = 1; }
        it = *(uint64_t *)(d + 0xb0); }
    g_hand_sig = sig; return ok;
}
// 副手那套（部位 11 / 12）的装备给不给这个技能：物品属性 97（非职业技能），层 = 技能号
// Does gear in the off-hand set (body locations 11 / 12) grant this skill: item stat 97 (non-class skill), layer = skill id
static int swap_gives(uint8_t *b, uint64_t pl, int skill) {
    uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0;
    for (int k = 0; it && k < 512; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; int loc = *(uint8_t *)(d + 0x54);
        if (*(uint32_t *)(it + 0xc) == 1 && (loc == 11 || loc == 12) && ((int (*)(uint64_t, int, int))(b + 0x5b9290))(it, 97, skill) > 0) return 1;
        it = *(uint64_t *)(d + 0xb0); }
    return 0;
}
// 周围 r 格内有没有活着的敌对怪（客户端单位表；敌对 = 原版分类 main+0x2f7b7c(0x165, 玩家, 单位) == 1，同秘境认击杀的读法）。返回最近那只的距离，没有 = -1
// Is there a living hostile monster within r tiles (client unit table; hostile = the game's classification main+0x2f7b7c(0x165, player, unit) == 1, the same reading Greater Rifts use to credit kills). Returns the distance to the nearest one, none = -1
static int enemy_near(uint8_t *b, uint64_t pl, int r) {
    uint64_t pp = *(uint64_t *)(pl + 0x38); if (!pp) return 0; int px = *(uint16_t *)(pp + 2), py = *(uint16_t *)(pp + 6), best = -1;
    for (int k = 0; k < 128; k++) for (uint64_t u = *(uint64_t *)(b + 0x2d603a0 + 1 * 0x400 + k * 8); u; u = *(uint64_t *)(u + 0x158)) {
        uint32_t m = *(uint32_t *)(u + 0xc); uint64_t up = *(uint64_t *)(u + 0x38); if (m == 0 || m == 12 || !up) continue;
        int dx = abs(*(uint16_t *)(up + 2) - px), dy = abs(*(uint16_t *)(up + 6) - py), d = dx > dy ? dx : dy; if (d > r) continue;
        if (((int (*)(int, uint64_t, uint64_t))(b + 0x2f7b7c))(0x165, pl, u) != 1) continue;
        if (best < 0 || d < best) best = d; }
    return best;
}
static int lr_skill(uint64_t pl, int right) { uint64_t sl = *(uint64_t *)(pl + 0x100), n = sl ? *(uint64_t *)(sl + (right ? 0x10 : 8)) : 0; return n && *(uint64_t *)n ? *(int16_t *)*(uint64_t *)n : -1; }
static int skipped(int skill) { if (getenv("DM_TEST_BUFF_NOSKIP")) return 0;   /* 测试开关：排除表不生效（验「待验」那几项落在谁身上） | Test switch: exclusion table disabled (to see who the "to be verified" entries land on) */ for (unsigned i = 0; i < sizeof k_skip / sizeof *k_skip; i++) if (k_skip[i].skill == skill) return 1; return 0; }
#define NAB (int)(sizeof k_ab / sizeof *k_ab)
uint32_t dm_cur_tick(void);

static int enabled(void) { static int env = -2; if (env == -2) { const char *e = getenv("DM_AUTOBUFF"); env = e ? e[0] != '0' : -1; }
    const char *app = getenv("DM_APP");
    return env >= 0 ? env : ((TARGET_OS_IPHONE || (app && app[0] && app[0] != '0')) && dm_custom_get("auto_buff")); }
static int cfg(const char *nm, int def) { const char *e = getenv(nm); return e && atoi(e) > 0 ? atoi(e) : def; }
static int has_state(uint64_t u, int st) {
    uint64_t sl = *(uint64_t *)(u + 0x88); if (!sl || !(*(uint32_t *)(sl + 0x1c) >> 31)) return 0;
    return (*(uint32_t *)(sl + 0xb30 + 4 * (uint64_t)(st >> 5)) >> (st & 31)) & 1;
}
// 技能链表里「自己学的」这个技能的节点（来源物品 id = -1）；没有 = 0。客户端 / 服务端玩家同一布局
// The node of this skill "learned by the character itself" in the skill list (source item id = -1); none = 0. Same layout for client / server player
static void stat_line(uint8_t *b, uint64_t u, char *o, size_t n) { int (*gs)(uint64_t, int, int) = (void *)(b + 0x5b9290);   // 测试读数用：防御 / 防御百分比两条 / 命中 / 命中百分比 / 伤害百分比 | for test readout: Defense / two Defense percent stats / Attack Rating / Attack Rating percent / damage percent
    snprintf(o, n, "防御 31=%d、16=%d、171=%d，命中 19=%d、119=%d，伤害%% 25=%d", gs(u, 31, 0), gs(u, 16, 0), gs(u, 171, 0), gs(u, 19, 0), gs(u, 119, 0), gs(u, 25, 0)); }
static uint64_t own_node(uint64_t pl, int skill) {
    uint64_t sl = *(uint64_t *)(pl + 0x100);
    for (uint64_t n = sl ? *(uint64_t *)sl : 0, k = 0; n && k < 512; n = *(uint64_t *)(n + 8), k++) { uint64_t rec = *(uint64_t *)n; if (rec && *(int16_t *)rec == skill && *(int32_t *)(n + 0x44) == -1) return n; }
    return 0;
}
// 这个节点放一次要的法力（×256，原版预检 0x5b1580 的算法：((等级 − 1) × 每级 + 基础) << 位移，再和最低法力取大）
// Mana needed to cast this node once (×256; algorithm of the game's precheck 0x5b1580: ((level − 1) × per level + base) << shift, then the larger of that and minimum Mana)
static int eff_level(uint8_t *b, uint64_t pl, uint64_t node) { return *(int32_t *)(node + 0x38) + ((int (*)(uint64_t, uint64_t, int))(b + 0x5b1710))(pl, node, 0); }   // 基础等级 + 装备加成（原版换武器后重设技能时的同一算法，0x6df160） | base level + gear bonus (same algorithm the game uses to reset skills after a weapon swap, 0x6df160)
static int mana_cost(uint64_t node, int lvl) {
    uint64_t rec = *(uint64_t *)node; int sh = *(uint8_t *)(rec + 0x228), base = *(int16_t *)(rec + 0x22a), per = *(int16_t *)(rec + 0x22c), mn = *(int16_t *)(rec + 0x224);
    int c = ((lvl - 1) * per + base) << sh; if (c < 0) c = 0; if ((mn << 8) > c) c = mn << 8; return c;
}

static uint8_t g_fail[NAB], g_stop[NAB]; static int g_last[8]; static uint32_t g_try[NAB];   // 连败次数 / 本局不再补 / 互斥组本局见到在身上的那个（-1 = 没见过）/ 上次发出的拍 | consecutive failures / no more recasts this game / mutex-group member seen active this game (-1 = none seen) / tick of the last request
static _Atomic int g_req = -1, g_busy, g_unusable = -1;
enum { W_IDLE, W_OUT, W_CAST, W_BACK };
static int w_ph, w_from, w_tries, w_list[6], w_n, w_life0, w_lr0[2]; static uint32_t w_t0, w_start, w_next; static uint8_t w_fail[NAB];   // 换武器补的状态机（见文件头第二笔） | state machine for weapon-swap recast (see part 2 in the file header)
int dm_swapskill_wset(void); int dm_swapskill_busy(void); static int g_wait = -1, g_mana0; static uint32_t g_wait_until;   // 交服务端拍放的那个；服务端那一拍没法放（还在动作里）；在等它出现的那个、请求时的法力 | the one handed to the server tick to cast; the server could not cast on that tick (still mid-action); the one we are waiting to see appear, Mana at request time

// 测试开关 DM_TEST_BUFF_LEARN=<技能号>[:<等级>][,…]（造现场用：手头的测试角色都没学名单里的技能）：给这个单位的技能链挂上这些技能并把基础等级设到这个数
// （原版不扣技能点的加技能 main+0x7bf010，同免费回城挂 411）。客户端玩家与服务端玩家各调一次
// Test switch DM_TEST_BUFF_LEARN=<skill id>[:<level>][,...] (for staging a scene: none of the test characters at hand has learned the listed skills): attach these skills to this unit's skill chain and set their base level to this number
// (the game's add-skill that spends no skill points, main+0x7bf010, same as free town portal attaching 411). Called once each for the client player and the server player
static void test_learn(uint8_t *b, uint64_t unit, const char *side) {
    const char *e = getenv("DM_TEST_BUFF_LEARN"); if (!e || !unit) return; char bf[160]; strlcpy(bf, e, sizeof bf);
    for (char *sv, *k = strtok_r(bf, ",", &sv); k; k = strtok_r(NULL, ",", &sv)) { int sk = 0, lv = 1; if (sscanf(k, "%d:%d", &sk, &lv) < 1 || sk <= 0) continue;
        uint64_t n = own_node(unit, sk); if (!n) n = ((uint64_t (*)(uint64_t, int))(b + 0x7bf010))(unit, sk);
        if (n && *(int32_t *)(n + 0x38) < lv) { *(int32_t *)(n + 0x38) = lv; LOG("测试 DM_TEST_BUFF_LEARN：%s玩家技能 %d 基础等级设成 %d", side, sk, lv); } }
}
// 主线程每帧调（dm_potpick.c 的每帧入口带过来；pl = 客户端玩家，不在局内为 0）
// Called every frame on the main thread (via the per-frame entry in dm_potpick.c; pl = client player, 0 when not in a game)
void dm_autobuff_pump(uint8_t *b, uint64_t pl) {
    static uint64_t lp; static uint32_t lt;
    if (!pl || !enabled()) { lp = 0; return; }
    uint32_t t = dm_cur_tick(); if (!t) return;
    { static uint64_t tl; if (tl != pl && t > 40) { tl = pl; test_learn(b, pl, "客户端"); } }
    // 测试开关 DM_TEST_EQUIP=<拍>:<物品码>（造现场用：给主手那套装一件带生命的盾，验换武器前后的生命）：到拍把背包里这种物品交给现成的「原版装备」（dm_actions.c dm_item_equip，走游戏自己的自动装备）
    // Test switch DM_TEST_EQUIP=<tick>:<item code> (for staging a scene: equip a shield with Life on the main-hand set to verify Life across a weapon swap): at that tick hand this item from the inventory to the existing "native equip" (dm_actions.c dm_item_equip, the game's own auto-equip)
    { static int at = -1; static char code[5]; if (at < 0) { const char *e = getenv("DM_TEST_EQUIP"); if (!e || sscanf(e, "%d:%4s", &at, code) != 2) at = 0; }
      if (at > 0 && t >= (uint32_t)at) { at = 0; uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; uint32_t iid = 0;
          for (int k = 0; it && k < 512 && !iid; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; uint32_t c = ((uint32_t (*)(uint64_t))(b + 0x570260))(it);
              if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && !memcmp(&c, code, strlen(code))) iid = *(uint32_t *)(it + 8); it = *(uint64_t *)(d + 0xb0); }
          void dm_item_equip(uint32_t); if (iid) dm_item_equip(iid); int (*gs)(uint64_t, int, int) = (void *)(b + 0x5b9290);
          LOG("测试 DM_TEST_EQUIP：拍 %u 背包里的 %s id %u → 交原版装备；此刻生命 %d / 上限 %d", t, code, iid, gs(pl, 6, 0) >> 8, gs(pl, 7, 0) >> 8); } }
    if (pl != lp || t < lt) {   // 进局 / 换局 | entering / switching games
        lp = pl; memset(g_fail, 0, sizeof g_fail); memset(g_stop, 0, sizeof g_stop); memset(g_try, 0, sizeof g_try); for (int i = 0; i < 8; i++) g_last[i] = -1;
        g_wait = -1; atomic_store(&g_req, -1); w_ph = W_IDLE; w_next = 0; memset(w_fail, 0, sizeof w_fail); }
    { static uint64_t dumped; if (dumped != pl && t > 60 && getenv("DM_TEST_BUFF_DUMP")) { dumped = pl; char bf[900]; int o = 0; bf[0] = 0;   // 测试读数：名单里每个技能在客户端技能链上的节点、基础等级、有效等级 | test readout: for each listed skill, its node on the client skill chain, base level, effective level
        for (int i = 0; i < NAB && o < 860; i++) { uint64_t n = own_node(pl, k_ab[i].skill); if (n) o += snprintf(bf + o, sizeof bf - o, " %s(%d)基础%d有效%d", k_ab[i].nm, k_ab[i].skill, *(int32_t *)(n + 0x38), eff_level(b, pl, n)); }
        uint64_t sl = *(uint64_t *)(pl + 0x100); int nn = 0; for (uint64_t n = sl ? *(uint64_t *)sl : 0; n && nn < 512; n = *(uint64_t *)(n + 8)) nn++;
        LOG("增益自动补读数：模式 %u、关卡 %d、技能链 %d 个节点，名单里有：%s", *(uint32_t *)(pl + 0xc), dm_unit_level(pl), nn, o ? bf : " 无"); } }
    if (t == lt) return; lt = t;   // 一拍看一次 | check once per tick
    int has[NAB];
    for (int i = 0; i < NAB; i++) { has[i] = has_state(pl, k_ab[i].state); if (has[i]) { if (k_ab[i].grp) g_last[(int)k_ab[i].grp] = i; g_fail[i] = 0; } }
    { int u = atomic_exchange(&g_unusable, -1); if (u >= 0) { g_stop[u] = 1; if (g_wait == u) g_wait = -1; LOG("增益自动补：「%s」此刻用不了（原版没进施法：缺装备条件等），本局不再补它", k_ab[u].nm); } }
    if (g_wait >= 0 && atomic_exchange(&g_busy, 0)) { g_try[g_wait] = 0; g_wait = -1; return; }   // 服务端那一拍人还在动作里（客户端的模式比服务端晚一两拍）：不算失败，下个站立拍再请 | on that server tick the character was still mid-action (client mode lags the server by a tick or two): not a failure, request again on the next standing tick
    if (g_wait >= 0) {   // 发出去的那个：出现了 = 成；2 秒没出现 = 这次没放成 | the one requested: appeared = success; not there after 2 seconds = this cast failed
        if (has[g_wait]) { uint64_t sl = *(uint64_t *)(pl + 0x100), rn = sl ? *(uint64_t *)(sl + 0x10) : 0; LOG("增益自动补：拍 %u「%s」已补上（法力 %d → %d；客户端右键技能 %d、左键技能 %d）", t, k_ab[g_wait].nm, g_mana0 >> 8, ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 8, 0) >> 8, rn && *(uint64_t *)rn ? *(int16_t *)*(uint64_t *)rn : -1, sl && *(uint64_t *)(sl + 8) && *(uint64_t *)*(uint64_t *)(sl + 8) ? *(int16_t *)*(uint64_t *)*(uint64_t *)(sl + 8) : -1); g_wait = -1; }
        else if (t >= g_wait_until) { int i = g_wait; g_wait = -1; g_fail[i]++;
            if (g_fail[i] >= 3) { g_stop[i] = 1; LOG("【红】增益自动补：「%s」连着 3 次没补上，本局不再补它", k_ab[i].nm); } else LOG("增益自动补：拍 %u「%s」这次没补上（第 %d 次），隔一会儿再试", t, k_ab[i].nm, g_fail[i]); }
        return; }
    uint32_t mode = *(uint32_t *)(pl + 0xc); int wset = dm_swapskill_wset(), dead = mode == 0 || mode == 17; void (*swap0)(int) = (void (*)(int))(b + 0x41db90);
    static int swapmax = -1, safe, swaplife; if (swapmax < 0) { swapmax = cfg("DM_AUTOBUFF_SWAPMAX", 150); safe = cfg("DM_AUTOBUFF_SAFE", 40); swaplife = cfg("DM_AUTOBUFF_SWAPLIFE", 50); }
    if (w_ph == W_OUT) {   // 等换过去 | waiting for the swap to land
        if (wset >= 0 && wset != w_from) { w_ph = W_CAST; w_start = t; LOG("增益自动补：拍 %u 已换到副手组（第 %d 组），开始补（此刻生命 %d / 上限 %d）", t, wset, ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 6, 0) >> 8, ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 7, 0) >> 8); }
        else if (dead || t - w_start > 60) { w_ph = W_IDLE; w_next = t + 250; LOG("增益自动补：拍 %u 换武器没换过去（%s），这一轮作罢", t, dead ? "人死了" : "2 秒多没变"); return; }
        else if (t - w_t0 >= 8 && w_tries < 4 && (mode == 1 || mode == 5)) { w_tries++; w_t0 = t; swap0(w_from); return; }
        else return; }
    if (w_ph == W_CAST) {   // 在副手组：该补的都补上了、或到点了 → 换回。补放本身走下面第一笔的循环（这套在手时节点就在技能链上） | on the off-hand set: everything due has been recast, or time is up → swap back. The casts themselves go through the part 1 loop below (with this set in hand the nodes are on the skill chain)
        int left = 0; for (int k = 0; k < w_n; k++) if (!has[w_list[k]] && !g_stop[w_list[k]]) left++;
        if (!left || dead || t - w_start > (uint32_t)swapmax) { for (int k = 0; k < w_n; k++) if (!has[w_list[k]]) w_fail[w_list[k]]++;
            LOG("增益自动补：拍 %u 副手组上%s → 换回第 %d 组", t, !left ? "该补的都补上了" : dead ? "人死了" : "到点了还没补完", w_from); w_ph = W_BACK; w_t0 = 0; w_tries = 0; g_wait = -1; } }
    if (w_ph == W_BACK) {   // 换回：一直重试到成 | swap back: retry until it succeeds
        if (wset == w_from) { w_ph = W_IDLE; w_next = t + 250; int l1 = ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 6, 0) >> 8, mx = ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 7, 0) >> 8;
            LOG("增益自动补：拍 %u 已换回第 %d 组（生命 %d → %d / 上限 %d；左键技能 %d → %d、右键技能 %d → %d）", t, w_from, w_life0, l1, mx, w_lr0[0], lr_skill(pl, 0), w_lr0[1], lr_skill(pl, 1)); }
        else { if (!dead && (mode == 1 || mode == 5 || t - w_t0 >= 25) && (!w_t0 || t - w_t0 >= 8)) { w_t0 = t; w_tries++; swap0(wset);
                   if (w_tries % 10 == 0) LOG("【红】增益自动补：换回第 %d 组已试 %d 次还没成（模式 %u），继续试", w_from, w_tries, mode); }
               return; } }
    if (mode != 1 && mode != 5) return;   // 只在站立那一拍（1 野外站立、5 城镇站立——刚出城还没迈步时服务端仍是 5，实测）；城里另由关卡号挡 | only on a standing tick (1 = standing outside, 5 = standing in town; just out of town before the first step the server still says 5, measured); town is blocked separately by level id
    if (dm_hud_area_is_town(dm_unit_level(pl)) || ((uint64_t (*)(void))(b + 0x2aa7c0))()) return;   // 城里不放；手上拿着东西不放 | no cast in town; no cast with an item on the cursor
    static int retry = -1; if (retry < 0) retry = cfg("DM_AUTOBUFF_RETRY", 75);
    { static uint32_t last_sig; equip_ok(b, pl, 0); if (g_hand_sig != last_sig) { last_sig = g_hand_sig; memset(g_stop, 0, sizeof g_stop); memset(g_fail, 0, sizeof g_fail); } }   // 手上的装备变了：之前判「用不了 / 连败」的重新算 | held gear changed: re-evaluate those previously judged "unusable / failing repeatedly"
    int mana = ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 8, 0);
    for (int i = 0; i < NAB; i++) { int g = k_ab[i].grp;
        if (has[i] || g_stop[i] || skipped(k_ab[i].skill) || (g_try[i] && t - g_try[i] < (uint32_t)retry)) continue;
        uint64_t node = own_node(pl, k_ab[i].skill); int lvl = node ? eff_level(b, pl, node) : 0; if (lvl <= 0) continue;   // 没学也没有装备给 | not learned and not granted by gear
        if (!equip_ok(b, pl, k_ab[i].skill)) continue;   // 装备前提不满足（圣盾没拿盾等） | gear prerequisite not met (Holy Shield with no shield, etc.)
        if (g) { int other = 0, best = i, bl = lvl;   // 互斥组：同组别的还在身上就不去顶它；本局见过哪个补哪个；没见过补等级最高的 | mutex group: do not replace another member still active; recast whichever was seen this game; if none was seen, the highest-level one
            for (int j = 0; j < NAB; j++) { if (j == i || k_ab[j].grp != g || skipped(k_ab[j].skill)) continue; if (has[j]) other = 1;
                uint64_t n2 = own_node(pl, k_ab[j].skill); int l2 = n2 ? eff_level(b, pl, n2) : 0; if (l2 > bl) { bl = l2; best = j; } }
            if (other || (g_last[g] >= 0 ? g_last[g] != i : best != i)) continue; }
        int cost = mana_cost(node, lvl); if (mana < cost) { static uint32_t said; if (!said || t - said > 250) { said = t; LOG("增益自动补：「%s」不在身上，法力 %d 不够 %d，先不补", k_ab[i].nm, mana >> 8, cost >> 8); } continue; }
        g_try[i] = t; g_wait = i; g_wait_until = t + 50; g_mana0 = mana; atomic_store(&g_busy, 0); atomic_store(&g_req, i);
        { uint64_t sl = *(uint64_t *)(pl + 0x100), rn = sl ? *(uint64_t *)(sl + 0x10) : 0; LOG("增益自动补：拍 %u「%s」（技能 %d，等级 %d）不在身上 → 请服务端补放（法力 %d，要 %d；客户端右键技能 %d）", t, k_ab[i].nm, k_ab[i].skill, lvl, mana >> 8, cost >> 8, rn && *(uint64_t *)rn ? *(int16_t *)*(uint64_t *)rn : -1); }
        return; }
    // 第二笔：当前这套补不了、副手那套给的 → 条件都满足就起一轮「换过去 → 补 → 换回」
    // Part 2: cannot be recast with the current set but granted by the off-hand set → if all conditions hold, start one round of "swap over → recast → swap back"
    if (w_ph != W_IDLE || g_wait >= 0 || t < w_next || wset < 0 || dm_swapskill_busy()) return;
    w_n = 0; for (int i = 0; i < NAB && w_n < 6; i++) { if (has[i] || g_stop[i] || w_fail[i] >= 2 || skipped(k_ab[i].skill)) continue;
        uint64_t node = own_node(pl, k_ab[i].skill); if (node && eff_level(b, pl, node) > 0) continue;   // 当前这套就能放的归第一笔 | anything the current set can cast belongs to part 1
        if (swap_gives(b, pl, k_ab[i].skill)) w_list[w_n++] = i; }
    if (!w_n) return;
    { int lf = ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 6, 0) >> 8, mx = ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 7, 0) >> 8;   // 换走期间主手 / 盾加的生命上限没了、当前生命会被截到新上限，换回不回补：血量不到上限的一半（DM_AUTOBUFF_SWAPLIFE，百分数）时不换 | while swapped away the max Life from main hand / shield is gone, current Life is clamped to the new max and not restored on swap back: do not swap when Life is below half of max (DM_AUTOBUFF_SWAPLIFE, percent)
      if (mx > 0 && (long long)lf * 100 < (long long)mx * swaplife) return; }
    int en = enemy_near(b, pl, safe); if (en >= 0) { static uint32_t said; if (!said || t - said > 250) { said = t; LOG("增益自动补：副手组的增益要补，但 %d 格内有敌对怪（最近 %d 格），先不换武器", safe, en); } return; }
    w_ph = W_OUT; w_from = wset; w_t0 = w_start = t; w_tries = 1; w_life0 = ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 6, 0) >> 8; w_lr0[0] = lr_skill(pl, 0); w_lr0[1] = lr_skill(pl, 1);
    { char bf[120]; int o = 0; for (int k = 0; k < w_n; k++) o += snprintf(bf + o, sizeof bf - o, " %s", k_ab[w_list[k]].nm); LOG("增益自动补：拍 %u 副手组给的增益不在身上（%s）→ 换武器（当前第 %d 组，生命 %d / 上限 %d，左键技能 %d、右键技能 %d）", t, bf, wset, w_life0, ((int (*)(uint64_t, int, int))(b + 0x5b9290))(pl, 7, 0) >> 8, w_lr0[0], w_lr0[1]); }
    swap0(wset);
}
// 服务端拍（diag.c）：放主线程点名的那个技能
// Server tick (diag.c): cast the skill named by the main thread
void dm_autobuff_srv_tick(uint8_t *b, uint64_t G, uint64_t player) {
    // 测试开关 DM_TEST_BUFF_DROP=<拍>:<状态号>（造现场用）：到拍把服务端玩家这个状态位清掉、客户端那边由 dm_autobuff_test_drop 同拍清，模拟「增益掉了」。只清位图，不走原版的状态移除
    // Test switch DM_TEST_BUFF_DROP=<tick>:<state id> (for staging a scene): at that tick clear this state bit on the server player; the client side is cleared on the same tick by dm_autobuff_test_drop, simulating "the buff dropped". Only the bitmap is cleared; the game's state removal is not run
    { static int at = -1, st; if (at < 0) { const char *e = getenv("DM_TEST_BUFF_DROP"); if (!e || sscanf(e, "%d:%d", &at, &st) != 2) at = 0; }
      if (at > 0 && player && *(uint32_t *)(G + 0x170) >= (uint32_t)at) { at = 0; uint64_t sl = *(uint64_t *)(player + 0x88); if (sl && *(uint32_t *)(sl + 0x1c) >> 31) *(uint32_t *)(sl + 0xb30 + 4 * (uint64_t)(st >> 5)) &= ~(1u << (st & 31)); LOG("测试 DM_TEST_BUFF_DROP：服务端清掉状态 %d", st); } }
    { static uint64_t tl; if (tl != player && player && *(uint32_t *)(G + 0x170) > 40) { tl = player; test_learn(b, player, "服务端"); } }
    static uint32_t chk_at; static int chk_m, chk_i;   // 测试读数（DM_TEST_BUFF_DUMP）：施放后第 30 拍再读一次法力与几条属性 | test readout (DM_TEST_BUFF_DUMP): read Mana and a few stats again on tick 30 after the cast
    if (chk_at && player && *(uint32_t *)(G + 0x170) >= chk_at) { chk_at = 0; char sb[200]; stat_line(b, player, sb, sizeof sb); LOG("增益自动补读数：「%s」施放后 30 拍，服务端法力 %d → %d，模式 %u；%s", k_ab[chk_i].nm, chk_m >> 8, ((int (*)(uint64_t, int, int))(b + 0x5b9290))(player, 8, 0) >> 8, *(uint32_t *)(player + 0xc), sb); }
    int i = atomic_exchange(&g_req, -1); if (i < 0 || !player) return;
    uint32_t pm = *(uint32_t *)(player + 0xc); uint64_t node = own_node(player, k_ab[i].skill), sl = *(uint64_t *)(player + 0x100);
    if ((pm != 1 && pm != 5) || !node || !sl) { atomic_store(&g_busy, 1); return; }   // 人还在动作里 / 节点没了：这一拍不放，告诉主线程别记失败 | character still mid-action / node gone: no cast this tick, tell the main thread not to count a failure
    uint64_t pp = *(uint64_t *)(player + 0x38); int x = pp ? *(uint16_t *)(pp + 2) : 0, y = pp ? *(uint16_t *)(pp + 6) : 0, m0 = ((int (*)(uint64_t, int, int))(b + 0x5b9290))(player, 8, 0);
    uint64_t old_r = *(uint64_t *)(sl + 0x10); *(uint64_t *)(sl + 0x10) = node;
    int r = ((int (*)(uint64_t, uint64_t, int, int, int, uint64_t))(b + 0x6e77f0))(G, player, x, y, 1, node);
    uint64_t mid_r = *(uint64_t *)(sl + 0x10); *(uint64_t *)(sl + 0x10) = old_r;
    if (*(uint32_t *)(player + 0xc) != 10) atomic_store(&g_unusable, i);   // 原版在技能此刻用不了时（如圣盾没拿盾）会改放一下普通攻击：认出来，本局不再替放 | when a skill is unusable right now (e.g. Holy Shield with no shield) the game casts a normal attack instead: detect that and stop recasting it this game
    int m1 = ((int (*)(uint64_t, int, int))(b + 0x5b9290))(player, 8, 0); if (getenv("DM_TEST_BUFF_DUMP")) { char sb[200]; stat_line(b, player, sb, sizeof sb); LOG("增益自动补读数：放「%s」之前服务端玩家 %s", k_ab[i].nm, sb); chk_at = *(uint32_t *)(G + 0x170) + 30; chk_m = m0; chk_i = i; }
    LOG("增益自动补：服务端放「%s」（技能 %d，基础等级 %d）→ 返回 %d，模式 %u → %u，法力 %d → %d；右键技能节点 %#llx（技能 %d）调用后是 %#llx，已放回", k_ab[i].nm, k_ab[i].skill, *(int32_t *)(node + 0x38), r, pm, *(uint32_t *)(player + 0xc), m0 >> 8, m1 >> 8, (unsigned long long)old_r, old_r && *(uint64_t *)old_r ? *(int16_t *)*(uint64_t *)old_r : -1, (unsigned long long)mid_r);
}
// 测试开关 DM_TEST_BUFF_DROP 的客户端那一半（主线程，dm_autobuff_pump 之前调）
// Client half of the test switch DM_TEST_BUFF_DROP (main thread, called before dm_autobuff_pump)
void dm_autobuff_test_drop(uint64_t pl) {
    static int at = -1, st; if (at < 0) { const char *e = getenv("DM_TEST_BUFF_DROP"); if (!e || sscanf(e, "%d:%d", &at, &st) != 2) at = 0; }
    if (at > 0 && pl && dm_cur_tick() >= (uint32_t)at) { at = 0; uint64_t sl = *(uint64_t *)(pl + 0x88); if (sl && *(uint32_t *)(sl + 0x1c) >> 31) *(uint32_t *)(sl + 0xb30 + 4 * (uint64_t)(st >> 5)) &= ~(1u << (st & 31)); LOG("测试 DM_TEST_BUFF_DROP：客户端清掉状态 %d", st); }
}
