// 技能钮上的剩余弹药（箭矢 / 弩矢 / 投掷武器数量）。
//
// 数据源（2026-11-13 真机实测）：物品自己的属性 stat 70（quantity）——箭袋在装备格，属性表只有一项 70/0=85，
// 射 3 箭读到 85 → 82；与 diag.c 读回城书数量同一口径。投掷武器（标枪 / 飞斧）的数量也在武器自己的 stat 70 上。
// 装备格：[背包+0x20] → +0x18 的格子数组，组 I 武器 = 4、副手 = 5，组 II = 11 / 12（腰带 = 8，dm_beltauto.c 同口径）。
//
// 「这个技能吃不吃弹药」走技能号名单，不走技能表的 itypea1：
// itypea1 只说「这个技能需要什么装备」，而魔法箭（Magic Arrow）和多重箭的 itypea1 都是弓——
// 魔法箭不耗箭是游戏执行代码里的特例，数据表上分不出来（用户在真机上确认：多重箭、引导箭吃箭，魔法箭、女武神不吃）。
// 名单只列吃弹药的技能；不认识的技能号一律不显示（宁可没有数字，也不给错的数字）。
//
// Ammo left on the skill buttons (arrows / bolts / throwing weapon count).
//
// Data source (measured on the device 2026-11-13): the item's own stat 70 (quantity) -- the quiver's stat table holds
// exactly one entry 70/0=85 and three shots read 85 -> 82, the same convention diag.c uses for the town-portal tome.
// A throwing weapon (javelin / throwing axe) carries its count on its own stat 70 as well.
// Equipment cells: [inventory+0x20] -> the cell array at +0x18; set I weapon = 4, off hand = 5, set II = 11 / 12 (belt = 8, same as dm_beltauto.c).
//
// "Does this skill consume ammo" goes by a skill-id list, not by the skill table's itypea1:
// itypea1 only says which gear a skill requires, and both Magic Arrow and Multiple Shot require a bow -- Magic Arrow not
// burning arrows is a special case in the game's execution code, invisible in the data table (confirmed on the device:
// Multiple Shot and Guided Arrow burn ammo, Magic Arrow and Valkyrie do not).
// The list holds only the skills that do consume it; an unknown skill id shows nothing (better no number than a wrong one).
#include "hle.h"
#include "dm_ammo.h"
#include "dm_actions.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

int dm_custom_get(const char *key);

// 客体地址是否可信：够高、8 字节对齐。宿主这边每帧直接解引用客体地址（同 dm_actions.c 的写法），
// 所以进解引用之前先过这一道，免得一次坏值把游戏打崩（2026-11-13 已因裸传垃圾指针崩过两次）。
// Whether a guest address is trustworthy: high enough and 8-byte aligned. The host dereferences guest
// addresses directly every frame (same style as dm_actions.c), so everything goes through this first --
// a single bad value used to crash the game (two crashes on 2026-11-13 came from passing garbage pointers on).
static int ok_unit(uint64_t u) { return u > 0x100000 && (u & 7) == 0; }

static uint32_t ammo_code(uint8_t *b, uint64_t it) { return ((uint32_t (*)(uint64_t))(b + 0x570260))(it); }   // 物品码 | item code
static int ammo_stat(uint8_t *b, uint64_t it, int id) { return ((int (*)(uint64_t, int, int))(b + 0x5b9290))(it, id, 0); }   // 属性（stat 70 = quantity） | stat (70 = quantity)
static int ammo_is(uint8_t *b, uint64_t it, int type) { return ((int (*)(uint64_t, int))(b + 0x574bc0))(it, type); }   // 是某类型（含父类型） | is of a type (parent types included)

// 技能号取自 kolbot 的 skills.txt（d2bs/kolbot/sdk/skills.txt），并用真机实测的四个锚点核对过：
// 6 = 魔法箭、12 = 多重箭、22 = 引导箭、32 = 女武神 全部吻合，说明 D2R 3.x 没有改动这套 id。
// 吃箭矢的弓技能（10 个里除魔法箭外全吃）：7 火焰箭 / 11 冰箭 / 12 多重箭 / 16 爆炸箭 / 21 冰箭(寒冰)
//   / 22 引导箭 / 26 炮轰 Strafe / 27 献祭箭 / 31 冻结箭；0 = 普通攻击，拿弓时也吃箭。
// 魔法箭 6 是唯一不耗箭的弓技能，故意不在名单里。
// Skill ids come from kolbot's skills.txt (d2bs/kolbot/sdk/skills.txt) and were checked against four anchors measured
// on the device: 6 = Magic Arrow, 12 = Multiple Shot, 22 = Guided Arrow, 32 = Valkyrie all match, so D2R 3.x keeps this id set.
// Bow skills that consume arrows (nine of the ten bow skills do; Magic Arrow is the exception):
//   7 Fire Arrow / 11 Cold Arrow / 12 Multiple Shot / 16 Exploding Arrow / 21 Ice Arrow / 22 Guided Arrow
//   / 26 Strafe / 27 Immolation Arrow / 31 Freezing Arrow; 0 = normal attack, which also burns arrows with a bow.
// Magic Arrow 6 is the only bow skill that burns nothing and is deliberately absent.
static const int k_bow[] = { 0, 7, 11, 12, 16, 21, 22, 26, 27, 31 };
// 吃标枪数量的投掷技能：2 投掷 Throw（通用投掷，实测在槽 7）/ 15 毒枪 / 20 闪电球 / 25 瘟疫标枪 / 35 闪电烈怒。
// 不吃数量的两种情况：① 近战标枪技能（10 刺击 Jab / 14 猛击 Power Strike / 19 猛刺 Impale / 24 充能一击 Charged Strike /
// 30 贯穿 Fend / 34 闪电击 Lightning Strike）；② 投掷武器的普通攻击——用户实测标枪普攻是近身戳刺，不减数量，所以 0 不在表里。
// Throwing skills that consume javelin count: 2 Throw (the generic throw, measured on the device in slot 7) / 15 Poison Javelin /
// 20 Lightning Bolt / 25 Plague Javelin / 35 Lightning Fury.
// Two cases that do not: (1) the melee javelin skills (10 Jab / 14 Power Strike / 19 Impale / 24 Charged Strike / 30 Fend /
// 34 Lightning Strike); (2) a throwing weapon's normal attack -- measured on the device: a javelin's normal attack is a
// close-range stab that consumes nothing, so 0 is absent from this list.
static const int k_throw[] = { 2, 15, 20, 25, 35 };

static int in_list(const int *l, unsigned n, int v) { for (unsigned i = 0; i < n; i++) if (l[i] == v) return 1; return 0; }

static uint64_t eq_cell(uint64_t inv, int i) {   // 装备格 i（页 0 的格子数组，每格 8 字节） | equipment cell i (page 0's cell array, 8 bytes per cell)
    uint64_t pages = 0, eq = 0, it = 0;
    if (!ok_unit(inv)) return 0;
    pages = *(uint64_t *)(uintptr_t)(inv + 0x20);
    if (!ok_unit(pages)) return 0;
    eq = *(uint64_t *)(uintptr_t)(pages + 0x18);
    if (!ok_unit(eq)) return 0;
    it = *(uint64_t *)(uintptr_t)(eq + 8 * (uint64_t)i);
    return ok_unit(it) ? it : 0;
}

// 切武器读数：武器组或弹药源一变就打一行，把两组四个装备格的东西都报出来——
// 「当前武器组」那个字段（服务端 [[玩家+0x10]+0xf0]+0x5a0）如果不随换武器更新，就只能靠这一行看出来。
// Weapon-swap readout: one line whenever the weapon set or the ammo source changes, listing all four equipment cells
// of both sets -- if the "current weapon set" field (server-side [[player+0x10]+0xf0]+0x5a0) does not follow a weapon
// swap, this line is how that becomes visible.
static void ammo_diag(uint8_t *b, uint64_t inv, const dm_ammo_src *o) {
    static int lw = -2, lk = -2, lq = -2;
    int wsv = dm_swapskill_wset();
    if (wsv == lw && o->kind == lk && o->qty == lq) return;
    lw = wsv; lk = o->kind; lq = o->qty;
    uint32_t c[4] = {0, 0, 0, 0}; const int cell[4] = {4, 5, 11, 12};
    for (int i = 0; i < 4; i++) { uint64_t e = eq_cell(inv, cell[i]); if (e) c[i] = ammo_code(b, e); }
    LOG("[ammo] 切武器读数 ws=%d｜格4=%.4s 格5=%.4s 格11=%.4s 格12=%.4s｜选中 kind%d qty%d 组%d",
        wsv, (char *)&c[0], (char *)&c[1], (char *)&c[2], (char *)&c[3], o->kind, o->qty, o->set);
}

static int dm_ammo_source_raw(uint8_t *b, uint64_t pl, dm_ammo_src *o);   // 真正的读取（dm_ammo_source 在它外面加防抖缓存） | the real read (dm_ammo_source wraps it with a debounce cache)

int dm_ammo_source(uint8_t *b, uint64_t pl, dm_ammo_src *o) {
    static dm_ammo_src last; static int miss;   // 游戏更新背包的那几帧页指针会短暂失效，不能一读不到就判「没有弹药」——
    // 实测这一下会让技能钮的数字每帧闪成 0（日志里 k10 q44 出现后 16 ms 就变回 k0 q0）。连续 30 帧都读不到才真的清空。
    // While the game is rebuilding the inventory the page pointer is briefly invalid; treating one failed read as "no ammo"
    // made the button digit flicker to 0 every frame (the log showed k10 q44 followed by k0 q0 16 ms later). Only 30
    // consecutive misses clear it for real.
    if (dm_ammo_source_raw(b, pl, o)) { last = *o; miss = 0; return 1; }
    if (++miss < 30 && last.item) { *o = last; return 1; }
    memset(&last, 0, sizeof last); memset(o, 0, sizeof *o);
    return 0;
}

// 箭袋总数 = 副手那袋 + 背包（页 0、模式 0）里**同样码**的所有箭袋。
// 游戏会自己换箭袋（副手打空后从背包再上一袋），所以显示总数才有意义。
// 箭和弩矢分开算（用户 2026-11-13 定：一般一个角色只用一种弓，分流派，不会混着用）。
// Total quiver count = the quiver in the off hand + every quiver of the SAME code in the backpack (page 0, mode 0).
// The game refills the off hand by itself when a quiver runs out, so the total is what matters.
// Arrows and bolts are counted separately (the user's call 2026-11-13: one character uses one kind of bow, a single build).
static int quiver_total(uint8_t *b, uint64_t inv, uint64_t worn, uint32_t want) {
    int total = 0;
    if (worn) { int q = ammo_stat(b, worn, 70); if (q > 0) total += q; }
    if (!ok_unit(inv)) return total;
    uint64_t it = *(uint64_t *)(uintptr_t)(inv + 0x10);   // 物品链头（同 dm_beltauto.c / diag.c cube_items） | item chain head (same as dm_beltauto.c / diag.c cube_items)
    for (int k = 0; it && k < 512; k++) {
        if (!ok_unit(it)) break;
        uint64_t d = *(uint64_t *)(uintptr_t)(it + 0x10);   // 数据块 | data block
        if (!ok_unit(d)) break;
        // 只算背包里的（模式 0 = 存放、页 0 = 背包）；副手那袋模式 1，本来就不在这条分支里，再用 it != worn 兜一道
        // Backpack only (mode 0 = stored, page 0 = backpack); the worn one is mode 1 and already excluded, it != worn is a second guard
        if (it != worn && *(uint32_t *)(uintptr_t)(it + 0xc) == 0 && *(uint8_t *)(uintptr_t)(d + 0x55) == 0) {
            if ((ammo_code(b, it) & 0xffffu) == want) { int q = ammo_stat(b, it, 70); if (q > 0) total += q; }
        }
        it = *(uint64_t *)(uintptr_t)(d + 0xb0);
    }
    return total;
}

static int dm_ammo_source_raw(uint8_t *b, uint64_t pl, dm_ammo_src *o) {
    memset(o, 0, sizeof *o);
    if (!b || !ok_unit(pl)) return 0;
    uint64_t inv = *(uint64_t *)(uintptr_t)(pl + 0x90);
    if (!ok_unit(inv) || *(uint32_t *)(uintptr_t)inv != 0x01020304u) return 0;   // 背包容器签名 | inventory container signature
    // 照探针（diag.c 的 ammo_probe）的读法：固定扫装备格 4/5/11/12，直接读 stat 70，不依赖任何武器组标志
    // （ws 那个字段实测在 -1/0/1 之间乱跳，拿它选格必然出错）。
    // 日志实测：装备格会随切换交换——拿标枪时「格4=ssp 格5=lrg 格11=am7 格12=aqv」，拿弓时「格4=am7 格5=aqv 格11=ssp 格12=lrg」，
    // 所以格 4 就是当前主手。顺序：格4 自带数量 = 投掷武器；否则格5 是箭袋 = 弓/弩；再退到格11 / 格12。
    // Same read as the probe (diag.c's ammo_probe): scan equipment cells 4/5/11/12 and read stat 70 directly, without any
    // weapon-set flag (the ws field was measured jumping between -1/0/1, so picking cells by it must fail).
    // Measured: the cells swap on every weapon change -- javelin gives "cell4=ssp cell5=lrg cell11=am7 cell12=aqv", bow gives
    // "cell4=am7 cell5=aqv cell11=ssp cell12=lrg" -- so cell 4 is the current main hand. Order: cell 4 carrying a count means a
    // throwing weapon; otherwise cell 5 being a quiver means bow/crossbow; then fall back to cells 11 and 12.
    const int wcell[2] = { 4, 11 }, ocell[2] = { 5, 12 };
    for (int i = 0; i < 2; i++) {
        uint64_t w = eq_cell(inv, wcell[i]), sh = eq_cell(inv, ocell[i]);
        if (w) {   // 主手自带数量 = 投掷武器（箭袋不在主手位；书 / 药水 / 钥匙不进装备格） | a main hand carrying a count is a throwing weapon
            int q = ammo_stat(b, w, 70);
            if (q > 0) { o->item = w; o->weapon = w; o->qty = q; o->kind = 2; o->set = i; ammo_diag(b, inv, o); return 1; }
        }
        if (sh) {   // 副手是箭袋（码 aq / cq 开头，不会误认） | the off hand is a quiver (code starts with aq / cq, cannot misfire)
            uint32_t lc = ammo_code(b, sh) & 0xffffu;
            if (lc == 0x7161u || lc == 0x7163u) {
                int q = quiver_total(b, inv, sh, lc);   // 副手 + 背包里同码的箭袋 | the worn quiver plus the same code in the backpack
                o->item = sh; o->weapon = w; o->qty = q; o->kind = 1; o->set = i;
                ammo_diag(b, inv, o); return 1;
            }
        }
    }
    ammo_diag(b, inv, o); return 0;
}

int dm_ammo_for_skill(uint8_t *b, uint64_t pl, const dm_ammo_src *src, int skill) {
    (void)pl;
    if (!b || !src || !src->item || skill < 0) return -1;
    if (src->kind == 1) return in_list(k_bow, sizeof k_bow / sizeof *k_bow, skill) ? src->qty : -1;       // 箭袋：只认弓技能名单 | quiver: only the bow skill list
    if (src->kind == 2) return in_list(k_throw, sizeof k_throw / sizeof *k_throw, skill) ? src->qty : -1;   // 投掷：只认投掷技能名单 | throwing: only the throwing skill list
    return -1;
}

// 服务端发布：弹药源必须用**服务端玩家**读——客户端那份背包结构里没有装备页数组（dm_actions 的 swapskill_pump 拿的是客户端玩家），
// 实测客户端读同一套偏移大部分帧读不到，偶尔成功一次就闪一下，技能钮上的数字因此一直出不来。
// 服务端玩家由 diag.c 的 hud_tick 每拍给一份（与 dm_swapskill_srv_wset 同一个来源），这里缓存起来给前端用。
// Server-side publish: the ammo source has to be read from the SERVER player -- the client copy of the inventory has no
// equipment-page array (swapskill_pump works on the client player), and reading the same offsets off the client failed on
// most frames with an occasional success, which is why the digits never settled. hud_tick in diag.c publishes it every tick
// (same source as dm_swapskill_srv_wset) and it is cached here for the front end.
static dm_ammo_src g_pub; static int g_pub_valid;

void dm_ammo_publish(uint8_t *b, uint64_t srv_player) {
    dm_ammo_src a;
    if (dm_ammo_source(b, srv_player, &a)) { g_pub = a; g_pub_valid = 1; }
}

int dm_ammo_get(dm_ammo_src *o) {
    if (!o) return 0;
    if (!g_pub_valid) { memset(o, 0, sizeof *o); return 0; }
    *o = g_pub;
    return 1;
}
