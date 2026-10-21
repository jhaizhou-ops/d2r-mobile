// 首领连战。缺省开，没有自定义开关。
//   哈洛加斯 109 凯恩旁广场的门（秘法圣殿门的外观，物件 298）→ 亚瑞特之巅 120 第 1–5 关：安达莉尔 156、督瑞尔 211、墨菲斯托 242、迪亚波罗 243、巴尔 544，
//   直接刷出，死了刷下一关 → 第 5 关死后在玩家身边开门 → 世界之石大殿 132 第 6–10 关：同五位，期间强制恐怖地带 Act5-WorldstoneKeep（原版地带机制）
//   → 只在地狱：第 11 关暗黑迪亚波罗 333（掉毁灭走它自己的宝藏表）、第 12 关三只超级首领 704 / 705 / 709（火炬走原版 0x718650 的三死标记）。
// 资格：当前难度任务字 40（毁灭前夕）位 0 = 已打过巴尔，读法见 eligible()。每局一次；死了在本局接着打；退出本局作废。
// 读码依据：
//   门：建门 0x7bfde0(游戏, 玩家, 房间, x, y, 目标关卡, 0, 物件号, 0, 1)（第 10 个参数非 0 跳过「红门只许去奶牛关 / 混乱之地」）。
//       298 的操作函数（操作函数表 0x20a3460 第 34 项 0x6afec0）写死只在 74 / 54 关换关，别处点了什么都不做；原版野外的红门点了只回城。
//       所以门只当外观：玩家点门（客户端命令 0x41 [单位 id][类型 2]，经命令分派入口观测门 OBS_C2S 看到）且走到门前，由这里调原版换关
//       0x634000(游戏, 玩家, 关卡, 11)——原版毁灭王座进世界之石大殿就是这一句（0x755d38），类型 11 = 按传送门落点。
//   首领：刷怪 0x7ce630(游戏, 房间, 1, 类号, x, y, 1, 0, 0x10)；人从门进来先挪进擂台（落点朝关卡中心 18 格，避开入口的出口砖 / 物件），首领刷在擂台再往里 10 格，
//       0x7160d0(游戏, 房间, 坐标, 3, 0x400, &房间, 4, 100) 找空地。本关记账：死亡只认模式 0 / 12；人离开后单位被释放就按原刷点、原生命接回。
//   提示：每关开打 / 前 5 关完成 / 全部完成，同时出系统提示框（升级 / 切难度那个框）与聊天区一行。
//   提示：原版聊天区「[游戏]」行 0xab2ab0(聊天管理 0x2e3d7d0, 文字, 4, 0, 发言者模板, 空, 空, 空, 空, 0)——语音字幕写进聊天区也是这一句（0x3601f4），
//       通道 4 显示「[游戏]」前缀；发言者模板照字幕那处从 0x2d52c88 复制。文字按游戏的串布局 {指针, 长度, 容量 | 位 63 = 不归它释放} 传。
//   恐怖地带：游戏只在两处把当前地带套到局上——玩家进局 0x5d8750 与主循环轮换 0x5d5e30，都是「地带函数 0x58f1e0(0) 取记录 → 0x636a60(游戏, 记录, 1)」。
//       这里让恐怖地带那几处取时钟（hle_time.c）返回一个落在 WorldstoneKeep 档的时刻，再照这两处的做法调一次 0x636a60 套上；离开 132 / 第 10 关死 /
//       打完时撤掉强制、按真实时钟再套一次，局外与连战后的地带照常轮换。
// Boss rush. On by default, with no custom settings switch.
//   The door in the square beside Cain in Harrogath 109 (with the look of the Arcane Sanctuary portal, object 298) → Arreat Summit 120, stages 1–5: Andariel 156, Duriel 211, Mephisto 242, Diablo 243, Baal 544,
//   spawned directly, the next stage spawning on death → after the stage 5 death a door opens beside the player → Worldstone Chamber 132, stages 6–10: the same five, with the terror zone Act5-WorldstoneKeep forced meanwhile (the original game's zone mechanism)
//   → Hell only: stage 11 Uber Diablo 333 (the Annihilus drop goes through its own treasure table), stage 12 three uber bosses 704 / 705 / 709 (the Torch goes through the three-deaths flag of the original game's 0x718650).
// Eligibility: current-difficulty quest word 40 (Eve of Destruction) bit 0 = Baal already beaten; see eligible() for how it is read. Once per run; after dying, continue within this run; exiting this run voids it.
// Code-reading basis:
//   Door: door creation 0x7bfde0(game, player, room, x, y, target level, 0, object id, 0, 1) (a non-zero 10th argument skips "red portals may only go to the Cow Level / Land of Chaos").
//       The operate function of 298 (entry 34 of the operate function table 0x20a3460, 0x6afec0) is hard-coded to change level only in levels 74 / 54 and does nothing when tapped elsewhere; the original game's red portals in the wilderness only return to town when tapped.
//       So the door serves only as a look: when the player taps the door (client command 0x41 [unit id][type 2], seen through the command dispatch entry observation gate OBS_C2S) and walks up to it, the original game's level change is called from here,
//       0x634000(game, player, level, 11) -- this is exactly the call the original game uses to go from the Throne of Destruction into the Worldstone Chamber (0x755d38); type 11 = by portal landing point.
//   Bosses: monster spawn 0x7ce630(game, room, 1, class id, x, y, 1, 0, 0x10); a player coming in through the door is first moved into the arena (18 cells from the landing point toward the level center, avoiding the exit tile / object at the entrance), and the boss spawns 10 cells further in from the arena,
//       0x7160d0(game, room, coordinates, 3, 0x400, &room, 4, 100) finds open ground. Bookkeeping for this stage: death is recognized only by mode 0 / 12; if the unit is freed after the player leaves, it is reattached at the original spawn point with the original life.
//   Notices: at the start of each stage / completion of the first 5 stages / full completion, both the system notification box (the level-up / difficulty-switch box) and one chat area line are shown.
//   Notices: the original game's chat area 「[游戏]」([Game]) line 0xab2ab0(chat manager 0x2e3d7d0, text, 4, 0, speaker template, null, null, null, null, 0) -- voice subtitles written to the chat area also use this call (0x3601f4),
//       channel 4 shows the 「[游戏]」([Game]) prefix; the speaker template is copied from 0x2d52c88 as the subtitle code does. The text is passed in the game's string layout {pointer, length, capacity | bit 63 = not freed by it}.
//   Terror zone: the game applies the current zone to a run in only two places -- player joining the run 0x5d8750 and the main loop rotation 0x5d5e30, both being "zone function 0x58f1e0(0) fetches the record → 0x636a60(game, record, 1)".
//       Here the terror zone clock reads (hle_time.c) are made to return a moment falling in the WorldstoneKeep slot, and then 0x636a60 is called once the way these two places do, to apply it; on leaving 132 / the stage 10 death /
//       on finishing, the forcing is removed and it is applied once more by the real clock, so zones outside the run and after the boss rush rotate as usual.
#include "dm_bossrush.h"
#include "dm_unit.h"
#include "hle.h"
#include "dm.h"
#include "dm_i18n.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern _Atomic int64_t g_dm_tz_time;            // hle_time.c：非 0 时恐怖地带那几处取时钟返回它 | hle_time.c: when non-zero, the terror zone clock reads return it
extern _Atomic uint64_t g_dm_tz_live_ns;        // hle_time.c：强制时刻的有效期（单调时钟纳秒），本局停拍后自动失效 | hle_time.c: validity deadline of the forced time (monotonic clock nanoseconds); expires automatically once this run stops ticking

enum { DOOR_OBJ = 298, LV_TOWN = 109, LV_SUMMIT = 120, LV_CHAMBER = 132, NOTHING_CLS = 817 };   // 817：place_nothing 建图时取成的「怪」类号，什么都不刷（宿主读出） | 817: the "monster" class id that place_nothing is taken as during map building; spawns nothing (read out on the host)
static const struct { int lv, n, cls[3]; const char *name; } k_stage[12] = {   // name = 中英表的键（dm/i18n/ui_text.tsv） | name = key in the Chinese-English string table (dm/i18n/ui_text.tsv)
    {LV_SUMMIT, 1, {156}, "dmBrAndariel"}, {LV_SUMMIT, 1, {211}, "dmBrDuriel"}, {LV_SUMMIT, 1, {242}, "dmBrMephisto"}, {LV_SUMMIT, 1, {243}, "dmBrDiablo"}, {LV_SUMMIT, 1, {544}, "dmBrBaal"},
    {LV_CHAMBER, 1, {156}, "dmBrAndariel"}, {LV_CHAMBER, 1, {211}, "dmBrDuriel"}, {LV_CHAMBER, 1, {242}, "dmBrMephisto"}, {LV_CHAMBER, 1, {243}, "dmBrDiablo"}, {LV_CHAMBER, 1, {544}, "dmBrBaal"},
    {LV_CHAMBER, 1, {333}, "dmBrDiabloClone"}, {LV_CHAMBER, 3, {704, 705, 709}, "dmBrUbers"},
};

static struct {
    uint64_t G, player; uint32_t last_t; int stage, total, done, eligible;       // stage = 正在打 / 下一个要打的关（0 起） | stage = the stage being fought / the next one to fight (0-based)
    uint32_t boss[3]; int nboss, boss_xy[2]; uint32_t next_at;      // 本关刷出的首领单位 id（nboss > 0 = 本关已刷过）；下一关最早刷出的拍 | unit ids of the bosses spawned by this stage (nboss > 0 = this stage has already spawned); the earliest tick the next stage spawns
    int boss_hp[3], boss_dead[3], spawn_xy[3][2];                    // 记账：每只最后读到的生命（×256）、是否见过它死（模式 0 / 12）、刷点 | bookkeeping: each one's last read life (×256), whether it was seen dying (mode 0 / 12), spawn point
    int lv_prev; uint32_t in_since; int arena_lv, arena_xy[2], tp_due;  // 进关卡的拍；擂台（落点朝关卡中心 18 格）；进门后要不要把人挪进擂台 | the tick of entering the level; the arena (18 cells from the landing point toward the level center); whether the player needs to be moved into the arena after entering through the door
    uint32_t door_at; int table_off; uint8_t table_ver;   // 第 5 关门要开的拍；132 原生巴尔那条预设记录（已取成不刷，打完按它改回）；预设表第 33 条是否改着 | the tick at which the stage 5 door is to open; the preset record of the 132 native Baal (already taken as not spawned, changed back per it when finished); whether entry 33 of the preset table is currently modified
    uint32_t door_town, door_summit; int town_xy[2], summit_xy[2];   // 两扇门的 id 与位置（位置只记日志） | ids and positions of the two doors (positions are only logged)
    uint32_t pend_door, pend_at; int town_tried;                        // 玩家点了哪扇门、在哪一拍 | which door the player tapped, and at which tick
    int tz_on; int64_t tz_time; uint32_t loot_at;
} R;

static uint8_t *B;
static uint64_t rd(uint64_t a) { return a ? *(volatile uint64_t *)(uintptr_t)a : 0; }
static uint32_t tick_of(uint64_t G) { return *(volatile uint32_t *)(uintptr_t)(G + 0x170); }
static int unit_level(uint64_t u) { return dm_unit_level(u); }   // 路径 → 房间 → 关卡 → 关卡号 | path → room → level → level id
static uint64_t unit_room(uint64_t u) { uint64_t p = rd(u + 0x38); return !p ? 0 : *(uint32_t *)(uintptr_t)u == 2 ? rd(p) : rd(p + 0x20); }   // 物件的静态路径房间在 +0，玩家 / 怪物在 +0x20 | an object's static path has the room at +0, players / monsters at +0x20
static int unit_xy(uint64_t u, int *x, int *y) {   // 同 dm_world.c：物件是路径 +0x10 / +0x14，玩家 / 怪物是路径头的 16 位 x / y | same as dm_world.c: objects use path +0x10 / +0x14, players / monsters use the 16-bit x / y in the path header
    uint64_t p = rd(u + 0x38); if (!p) return 0; uint32_t t = *(uint32_t *)(uintptr_t)u;
    if (t == 2) { *x = *(int32_t *)(uintptr_t)(p + 0x10); *y = *(int32_t *)(uintptr_t)(p + 0x14); } else { *x = *(uint16_t *)(uintptr_t)(p + 2); *y = *(uint16_t *)(uintptr_t)(p + 6); }
    return 1;
}
static uint64_t find_unit(uint64_t G, int type, uint32_t id, int cls) {   // 服务端单位表 G+0x2230 + 类型 × 0x400，128 桶，下一个 +0x158；按 id 或（id = 0 时）按类号找 | server unit table G+0x2230 + type × 0x400, 128 buckets, next at +0x158; find by id or (when id = 0) by class id
    for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2230 + type * 0x400 + 8 * b); u; u = rd(u + 0x158)) {
        uint32_t c = *(uint32_t *)(uintptr_t)(u + 4), i = *(uint32_t *)(uintptr_t)(u + 8);
        if (id ? i == id : (int)c == cls) return u; }
    return 0;
}

// 原版系统提示：聊天栏一行「[游戏] …」+ 系统提示框（首领连战、刷怪积分牌 dm_kills.c 共用）。只许服务端线程调（同首领连战）
// Original-game system notification: one chat bar line 「[游戏] …」([Game] ...) + the system notification box (shared by the boss rush and the kill board dm_kills.c). May only be called from the server thread (same as the boss rush)
void dm_game_notice(uint8_t *main_base, const char *text) {
    B = main_base; static char line[200]; strlcpy(line, text, sizeof line);
    static uint64_t txt[3], who[6], o1[8], o2[8], o3[8], o4[8];
    size_t n = strlen(line); txt[0] = (uint64_t)(uintptr_t)line; txt[1] = n; txt[2] = 0x8000000000000000ull | n;
    const uint8_t *g = B + 0x2d52c88; memset(who, 0, sizeof who); *(uint32_t *)who = *(const uint32_t *)g; who[1] = *(const uint64_t *)(g + 8); who[2] = *(const uint64_t *)(g + 16); who[3] = 0x8000000000000000ull | who[2];
    memset(o1, 0, sizeof o1); memset(o2, 0, sizeof o2); memset(o3, 0, sizeof o3); memset(o4, 0, sizeof o4);
    ((void (*)(uint64_t, void *, int, int, void *, void *, void *, void *, uint64_t, uint64_t))(B + 0xab2ab0))((uint64_t)(uintptr_t)(B + 0x2e3d7d0), txt, 4, 0, who, o1, o2, o3, (uint64_t)(uintptr_t)o4, 0);
    // 系统提示框（切难度生效时那个框）：界面消息 {NotificationMessage, SystemNotification}（FNV-1a 64）交给 0x3d8b74(文字, 消息)——
    // 升级 / 任务更新的提示框也走这一句（0xab5c40、0x306d54）；文字是 UTF-8 串，同 0x7f5630 取出的本地化串
    // System notification box (the box shown when a difficulty switch takes effect): the UI message {NotificationMessage, SystemNotification} (FNV-1a 64) is handed to 0x3d8b74(text, message) --
    // the level-up / quest-update notification boxes also go through this call (0xab5c40, 0x306d54); the text is a UTF-8 string, same as the localized string fetched by 0x7f5630
    { static uint64_t msg[4]; msg[0] = 0xd106c9eabf113da5ull; msg[1] = 0x8fa6af1ab5b75b23ull; msg[2] = 0; msg[3] = 1;
      ((void (*)(const char *, void *))(B + 0x3d8b74))(line, msg); }
}
// 我们建的门的名牌（秘境门「大秘境」、首领连战门「Boss 连战」，不改原版字符串表里通用的「传送门」）。
//   名牌：客户端取物件表记录（数据表根 +0x1548，每条 0x168）+0x42 的串键（342 / 298 都是 "portal"）→ 0x7f5630 取本地化串（0x442bc0 内 0x4447f8）。
//   取串查的表：[main+0x2d71098] 为 0 时 main+0x2d71030（当前语言），否则 main+0x2d70ff0（英文）；表 {+0 桶数（2 的幂）, +8 桶数组}，桶里直接是首节点，
//   节点 {+0 下一个, +8 键, +0x10 键长, +0x20 串, +0x28 串长}，哈希 0x1b6e1e0(&出, 键, 长, 0)（出参 16 字节，取前 8）；先查「键_switch」再查「键」，都没有给「丢失字符串」（宿主读出）。
//   做法：两张表各挂一个我们的键（当前语言表按语言挂中文或英文，英文表挂英文）（节点用原版分配器 [main+0x32116f8] 的 +0x10 分配：切语言时 0x7f5b60 用同一分配器的 +0x28 逐个释放节点，
//   表重装后键没了就再挂）；人在哈洛加斯时把门那一类的串键换成我们的键，离城写回（这两类原版都不在哈洛加斯出现，城里只有我们的门）。
// Name labels of the doors we create (rift door 「大秘境」(Greater Rift), boss rush door 「Boss 连战」(Boss Rush); the generic 「传送门」(Portal) in the original game's string table is not changed).
//   Name label: the client takes the string key at +0x42 of the object table record (data table root +0x1548, 0x168 per entry) (342 / 298 are both "portal") → 0x7f5630 fetches the localized string (0x4447f8 inside 0x442bc0).
//   The table the string lookup uses: main+0x2d71030 (current language) when [main+0x2d71098] is 0, otherwise main+0x2d70ff0 (English); table {+0 bucket count (power of 2), +8 bucket array}, the bucket directly holds the first node,
//   node {+0 next, +8 key, +0x10 key length, +0x20 string, +0x28 string length}, hash 0x1b6e1e0(&out, key, length, 0) (out parameter is 16 bytes, take the first 8); "key_switch" is looked up first, then "key", and if neither exists 「丢失字符串」(missing string) is given (read out on the host).
//   Approach: attach one key of ours to each of the two tables (the current-language table gets Chinese or English depending on the language, the English table gets English) (nodes are allocated with +0x10 of the original game's allocator [main+0x32116f8]: on a language switch 0x7f5b60 frees the nodes one by one with +0x28 of the same allocator,
//   and if the key is gone after the table is reloaded, attach it again); while the player is in Harrogath the string key of the door's class is replaced with our key, and written back on leaving town (in the original game neither class appears in Harrogath, so the only ones in town are our doors).
static int loc_has(uint64_t map, const char *key, size_t n, uint64_t *bucket) {
    uint64_t cnt = rd(map), bk = rd(map + 8); if (!cnt || !bk || (cnt & (cnt - 1))) return -1;
    uint64_t h[4] = {0}; ((void (*)(uint64_t *, const char *, uint64_t, int))(B + 0x1b6e1e0))(h, key, n, 0); *bucket = bk + 8 * (h[0] & (cnt - 1));   // 出参 16 字节（原版放 sp+0x50，下一格 +0x60 才是存的寄存器），只给 8 字节会写坏栈 | the out parameter is 16 bytes (the original game puts it at sp+0x50, and only the next slot +0x60 holds saved registers); giving it only 8 bytes corrupts the stack
    for (uint64_t nd = rd(*bucket); nd; nd = rd(nd)) if (rd(nd + 0x10) == n && !memcmp((const void *)(uintptr_t)rd(nd + 8), key, n)) return 1;
    return 0;
}
static void loc_add(uint64_t map, const char *key, const char *val) {
    size_t n = strlen(key); uint64_t bucket; if (loc_has(map, key, n, &bucket)) return;
    uint64_t al = rd((uint64_t)(uintptr_t)(B + 0x32116f8)); if (!al) return;
    uint64_t *nd = ((uint64_t *(*)(uint64_t, uint64_t, int))(uintptr_t)rd(rd(al) + 0x10))(al, 0x30, 16); if (!nd) return;
    nd[1] = (uint64_t)(uintptr_t)key; nd[2] = n; nd[3] = 0; nd[4] = (uint64_t)(uintptr_t)val; nd[5] = strlen(val); nd[0] = rd(bucket);
    __atomic_store_n((uint64_t *)(uintptr_t)bucket, (uint64_t)(uintptr_t)nd, __ATOMIC_RELEASE);
    LOG("门名牌：串表挂上键 %s → %s", key, val);
}
void dm_door_label(uint8_t *main_base, uint64_t G, int cls, const char *key, int in_town) {
    B = main_base; static struct { int cls; char orig[0x40]; } sv[4]; static int nsv;
    uint64_t T = rd((uint64_t)(uintptr_t)(B + 0x2d64a18 + *(uint8_t *)(uintptr_t)(G + 0x106) * 16)); if (!T || (uint64_t)cls >= rd(T + 0x1550)) return;
    char *k = (char *)(uintptr_t)(rd(T + 0x1548) + (uint64_t)cls * 0x168 + 0x42); int i = 0; while (i < nsv && sv[i].cls != cls) i++;
    if (i == nsv) { if (nsv == 4) return; sv[i].cls = cls; memcpy(sv[i].orig, k, sizeof sv[i].orig); nsv++; }
    if (in_town) { uint32_t li = *(volatile uint32_t *)(B + 0x20ae4e0);   // 当前语言号（main+0x20ae4e0，同 diag.c SetLanguage 记录）：文字从多语表取（简中 / 繁中用中文，esES / esMX 用西语，其余英文） | Current language number (main+0x20ae4e0, same as the SetLanguage record in diag.c): text taken from the multilingual table (Chinese for Simplified / Traditional Chinese, Spanish for esES / esMX, English otherwise)
        loc_add((uint64_t)(uintptr_t)(B + 0x2d71030), key, dm_ui_group(key, dm_lang_group_of((int)li))); loc_add((uint64_t)(uintptr_t)(B + 0x2d70ff0), key, dm_ui_group(key, DM_LG_EN));
        if (strcmp(k, key)) { memset(k, 0, sizeof sv[i].orig); strlcpy(k, key, sizeof sv[i].orig); LOG("门名牌：物件 %d 串键换成 %s", cls, key); } }
    else if (!strcmp(k, key)) { memcpy(k, sv[i].orig, sizeof sv[i].orig); LOG("门名牌：物件 %d 串键写回 %s", cls, sv[i].orig); }   // 只撤自己挂的那个键：同一类物件两处用（298 = 城里的首领连战门、秘境图里的回城门），各管各的 | only remove the key we attached ourselves: the same object class is used in two places (298 = the boss rush door in town, the return-to-town door in rift maps), each managing its own
}
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[160], full[180]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    snprintf(full, sizeof full, dm_str(B, "dmBrPrefix"), line); dm_game_notice(B, full); LOG("首领连战：提示「%s」", full);
}
int dm_bossrush_owns(uint32_t id) { for (int i = 0; i < R.nboss && i < 3; i++) if (R.boss[i] == id) return 1; return 0; }   // 本关刷出的首领（刷怪积分牌不计、不提示） | bosses spawned by this stage (not counted or announced by the kill board)

// 在 (x, y) 附近找空地：返回房间，坐标写回
// Find open ground near (x, y): returns the room, coordinates are written back
static uint64_t free_spot(uint64_t G, uint64_t room, int32_t pos[2]) {
    uint64_t out = 0; ((void (*)(uint64_t, uint64_t, int32_t *, int, int, uint64_t *, int, int))(B + 0x7160d0))(G, room, pos, 3, 0x400, &out, 4, 100); return out;
}
static uint64_t level_of(uint64_t u) { return dm_unit_level_ptr(u); }   // 逐步判空（dm_unit.h） | null-checked at every step (dm_unit.h)
// 关卡的房间链：关卡 +0x10 起，下一个 +0x48，范围 +0x60 / +0x64 / +0x68 / +0x6c（×5 = 格），已载入的房间 +0x58（同 dm_world.c 关卡信息）
// The level's room chain: starts at level +0x10, next at +0x48, extent +0x60 / +0x64 / +0x68 / +0x6c (×5 = cells), loaded rooms +0x58 (same as the level info in dm_world.c)
static void level_box(uint64_t lv, int *x0, int *y0, int *x1, int *y1) {
    *x0 = *y0 = 1 << 30; *x1 = *y1 = 0; int n = 0;
    for (uint64_t r = dm_level_rooms(lv); r && n < 2000; r = rd(r + 0x48), n++) { int x = *(int32_t *)(uintptr_t)(r + 0x60) * 5, y = *(int32_t *)(uintptr_t)(r + 0x64) * 5, w = *(int32_t *)(uintptr_t)(r + 0x68) * 5, h = *(int32_t *)(uintptr_t)(r + 0x6c) * 5;
        if (x < *x0) *x0 = x; if (y < *y0) *y0 = y; if (x + w > *x1) *x1 = x + w; if (y + h > *y1) *y1 = y + h; }
}
static uint64_t room_at(uint64_t lv, int px, int py) {   // 含这一格的已载入房间，没有返回 0 | the loaded room containing this cell; returns 0 if none
    int n = 0; for (uint64_t r = dm_level_rooms(lv); r && n < 2000; r = rd(r + 0x48), n++) { int x = *(int32_t *)(uintptr_t)(r + 0x60) * 5, y = *(int32_t *)(uintptr_t)(r + 0x64) * 5, w = *(int32_t *)(uintptr_t)(r + 0x68) * 5, h = *(int32_t *)(uintptr_t)(r + 0x6c) * 5;
        if (px >= x && px < x + w && py >= y && py < y + h) return rd(r + 0x58); }
    return 0;
}
// 这一格本身是空地：找空地函数不挪它（挪了 ≤ 1 格也算），且离别的物件 keep 格以上（门不压在别的可点东西上）。
// 只看物件表（G+0x2a30）：服务端单位表的槽不按类型号排（物品在槽 3 = G+0x2e30），槽 5 不是出口砖表，读它会碰到别的结构（实测崩）
// This cell itself is open ground: the find-open-ground function does not move it (being moved ≤ 1 cell also counts), and it is at least keep cells from other objects (the door does not sit on top of other tappable things).
// Only the object table (G+0x2a30) is checked: the server unit table's slots are not ordered by type number (items are in slot 3 = G+0x2e30), slot 5 is not the exit tile table, and reading it runs into another structure (crashed in measurement)
static uint64_t clear_spot(uint64_t G, uint64_t lv, int x, int y, int keep) {
    uint64_t rs = room_at(lv, x, y); if (!rs) return 0; int32_t pos[2] = {x, y}; uint64_t rr = free_spot(G, rs, pos);
    if (!rr || abs(pos[0] - x) > 1 || abs(pos[1] - y) > 1) return 0;
    for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2a30 + 8 * b); u; u = rd(u + 0x158)) { int ox, oy;
        if (unit_xy(u, &ox, &oy) && abs(ox - x) < keep && abs(oy - y) < keep) return 0; }
    return rr;
}
static int teleport(uint64_t G, uint64_t player, int x, int y) {   // 传到本关卡里的 (x, y)：目标所在的已载入房间交给公共的定点传送（dm_unit.h；目标房间没载入返回 0） | Teleport to (x, y) in the current level: the loaded room containing the target is handed to the shared fixed-point teleport (dm_unit.h; returns 0 if the target room is not loaded)
    return dm_teleport(B, G, player, room_at(level_of(player), x, y), x, y);
}
static int door_ids(uint64_t G, uint32_t *ids, int max) {   // 当前所有 298 物件的 id | ids of all current 298 objects
    int n = 0; for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2230 + 2 * 0x400 + 8 * b); u; u = rd(u + 0x158))
        if (*(uint32_t *)(uintptr_t)(u + 4) == DOOR_OBJ && n < max) ids[n++] = *(uint32_t *)(uintptr_t)(u + 8);
    return n;
}
// 建门。建门函数不返回单位，也会在目标关卡落点配一扇对门（同城镇传送门）：建之前记下已有的 298，建完取新出现、离落点最近的那扇
// Create the door. The door-creation function does not return the unit, and it also places a paired return door at the target level's landing point (same as a town portal): record the existing 298s before creating, and after creating take the newly appeared one closest to the landing point
static uint32_t make_door(uint64_t G, uint64_t player, uint64_t room, int32_t pos[2], int target, int xy[2]) {
    uint64_t rr = free_spot(G, room, pos); if (!rr) return 0;
    uint32_t old[32]; int nold = door_ids(G, old, 32);
    int ok = ((int (*)(uint64_t, uint64_t, uint64_t, int, int, int, uint64_t, int, uint64_t, uint64_t))(B + 0x7bfde0))(G, player, rr, pos[0], pos[1], target, 0, DOOR_OBJ, 0, 1);
    if (!ok) return 0;
    uint32_t best = 0; int bd = 1 << 30;
    for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2230 + 2 * 0x400 + 8 * b); u; u = rd(u + 0x158)) {
        int x, y, seen = 0; uint32_t id = *(uint32_t *)(uintptr_t)(u + 8); if (*(uint32_t *)(uintptr_t)(u + 4) != DOOR_OBJ || !unit_xy(u, &x, &y)) continue;
        for (int i = 0; i < nold; i++) seen |= old[i] == id; if (seen) continue;
        int d = abs(x - pos[0]) + abs(y - pos[1]); if (d < bd) { bd = d; best = id; xy[0] = x; xy[1] = y; } }
    return best;
}

static int eligible(uint64_t G, uint64_t player) {   // 当前难度已打过巴尔：任务字 40 位 0 | Baal already beaten on the current difficulty: quest word 40 bit 0
    // 玩家数据 +0x40 + 难度 × 8 → 记录 → +0 是该难度的任务块，与 .d2s 里每难度那 96 字节逐字节相同（两份存档对过），字 40 在 +0x50
    // Player data +0x40 + difficulty × 8 → record → +0 is that difficulty's quest block, byte-for-byte identical to the per-difficulty 96 bytes in the .d2s (checked against two saves); word 40 is at +0x50
    uint8_t diff = *(uint8_t *)(uintptr_t)(G + 0x104); uint64_t q = rd(rd(rd(player + 0x10) + 0x40 + 8 * diff));
    return q && (*(uint8_t *)(uintptr_t)(q + 0x50) & 1);
}

static void tz_live(void) { atomic_store(&g_dm_tz_live_ns, (uint64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC) + 1000000000ull); }   // 每拍续 1 秒：本局停拍（退出 / 崩溃）后强制自动失效 | extended by 1 second every tick: the forcing expires automatically once this run stops ticking (exit / crash)
static void tz_set(uint64_t G, int on) {
    uint64_t (*zone)(uint64_t) = (uint64_t (*)(uint64_t))(B + 0x58f1e0); void (*apply)(uint64_t, uint64_t, int) = (void (*)(uint64_t, uint64_t, int))(B + 0x636a60);
    if (on) { tz_live();
        if (!R.tz_time) { int64_t now = time(NULL);   // 往后逐档（30 分钟一档）找落在 WorldstoneKeep 的时刻：该档记录的关卡表（+0x20 起每 0x40 两格）含 132 | search forward slot by slot (30 minutes per slot) for a moment that falls on WorldstoneKeep: that slot's record's level table (two entries every 0x40 from +0x20) contains 132
            for (int j = 0; j < 4000 && !R.tz_time; j++) { atomic_store(&g_dm_tz_time, now + j * 1800 + 60); uint64_t rc = zone(0); int hit = 0;
                for (int q = 0; rc && q < 16; q++) hit |= *(int32_t *)(uintptr_t)(rc + 0x20 + 0x40 * (q / 2) + 4 * (q % 2)) == LV_CHAMBER;
                if (hit) R.tz_time = now + j * 1800 + 60; }
            atomic_store(&g_dm_tz_time, 0);
            if (!R.tz_time) { LOG("【红】首领连战：往后 4000 档没找到 WorldstoneKeep 当值的时刻，第 6–10 关不强制地带"); return; } }
        atomic_store(&g_dm_tz_time, R.tz_time);
    } else atomic_store(&g_dm_tz_time, 0);
    uint64_t rc = zone(0); if (rc) apply(G, rc, 1);
    R.tz_on = on; LOG("首领连战：恐怖地带%s（记录 %#llx%s）", on ? "强制为 WorldstoneKeep" : "恢复真实时钟", (unsigned long long)rc, rc ? "，已套用" : "，无记录");
}

// 132 原生巴尔（连战只借大殿这张图，不刷它自己的巴尔）。机制（宿主读码 + 实测）：
//   ① 132 建图时按怪物预设表（数据表根 [main+0x2d64a18 + 版本 × 16] 的 +0x1060 = 第五幕，每条 4 字节 {幕, 类型 0 地点 / 1 怪 / 2 超级独特, u16 下标}）
//      取第 33 条 {5, 1, 544}，写进那间房的预设单位记录（房间 +0x98 → +0x10 起，下一个 +0x10；{+0 模式, +4 类号, +8 / +0xc 房内坐标, +0x20 单位类型}）；
//   ② 房间激活时放怪（0x7ce930 → 0x7ce800 → 0x7ce630）；原版同一局里巴尔只能打一次，杀了之后离开再回来不再出现（宿主对照实测）。
// 连战第 5、10 关本身就是巴尔，打完连战这一局本来就不该再有巴尔（照原版）。所以第 5 关巴尔一死（开去 132 的门之前），把表的第 33 条改成 place_nothing
// （第 51 条同值 {5, 0, 0}，建图时取成「怪 817」，什么都不刷）；132 一建好、人第一拍在大殿时立刻改回，别的图、以后的建图都不受影响。
// 不进连战的局不碰这张表，原生巴尔照常在
// 132 native Baal (the boss rush only borrows the chamber map and does not spawn its own Baal). Mechanism (host code reading + measurement):
//   (1) When 132 is built, per the monster preset table (+0x1060 of the data table root [main+0x2d64a18 + version × 16] = Act 5, 4 bytes per entry {act, type 0 place / 1 monster / 2 super unique, u16 index})
//      entry 33 {5, 1, 544} is taken and written into that room's preset unit record (room +0x98 → starting at +0x10, next at +0x10; {+0 mode, +4 class id, +8 / +0xc in-room coordinates, +0x20 unit type});
//   (2) monsters are placed when the room is activated (0x7ce930 → 0x7ce800 → 0x7ce630); in the original game Baal can be fought only once in the same run, and after being killed he does not appear again on leaving and coming back (host comparison measurement).
// Boss rush stages 5 and 10 are Baal themselves, so a run that finished the boss rush should not have another Baal anyway (as in the original game). So as soon as the stage 5 Baal dies (before opening the door to 132), entry 33 of the table is changed to place_nothing
// (entry 51 has the same value {5, 0, 0}, which map building takes as "monster 817", spawning nothing); as soon as 132 is built and on the first tick the player is in the chamber, it is changed back immediately, so other maps and later map building are unaffected.
// Runs that do not enter the boss rush do not touch this table, and the native Baal is there as usual
static uint8_t *preset_slot(uint8_t ver) { uint64_t T = rd((uint64_t)(uintptr_t)(B + 0x2d64a18 + ver * 16)), a5 = T ? rd(T + 0x1060) : 0; return a5 ? (uint8_t *)(uintptr_t)(a5 + 4 * 33) : NULL; }
static void baal_table(uint64_t G, int off) {
    if (off == R.table_off) return; uint8_t ver = off ? *(uint8_t *)(uintptr_t)(G + 0x106) : R.table_ver; uint8_t *r = preset_slot(ver);
    if (!r || r[0] != 5 || *(uint16_t *)(r + 2) != (off ? 544 : 0)) { LOG("【红】首领连战：怪物预设表第五幕第 33 条不是预期值（%s），不动", off ? "巴尔 544" : "place_nothing"); return; }
    r[1] = off ? 0 : 1; *(uint16_t *)(r + 2) = off ? 0 : 544; R.table_off = off; R.table_ver = ver;
    LOG("首领连战：怪物预设表第五幕第 33 条%s", off ? "改成 place_nothing（132 建图时不取巴尔）" : "改回巴尔 544");
}
static uint64_t chamber_rec(uint64_t lv132, int type, int cls) {   // 132 里站位 (15135,5920) 的那条预设记录（类型 / 类号符合才算） | the preset record positioned at (15135,5920) in 132 (only counts if type / class id match)
    for (uint64_t r = dm_level_rooms(lv132), m = 0; r && m < 400; r = rd(r + 0x48), m++) { int rx = *(int32_t *)(uintptr_t)(r + 0x60) * 5, ry = *(int32_t *)(uintptr_t)(r + 0x64) * 5;
        uint64_t pl = rd(r + 0x98); if (!pl) continue;   // 没有预设单位的房间这里是空 | this is null for rooms without preset units
        for (uint64_t u = rd(pl + 0x10), k = 0; u && k < 256; u = rd(u + 0x10), k++)
            if (*(int32_t *)(uintptr_t)u == type && *(int32_t *)(uintptr_t)(u + 4) == cls && rx + *(int32_t *)(uintptr_t)(u + 8) == 15135 && ry + *(int32_t *)(uintptr_t)(u + 0xc) == 5920) return u; }
    return 0;
}
static void reset(uint64_t G) {
    if (R.tz_on) atomic_store(&g_dm_tz_time, 0);
    if (R.table_off) { uint8_t *r = preset_slot(R.table_ver); if (r && r[0] == 5 && *(uint16_t *)(r + 2) == 0) { r[1] = 1; *(uint16_t *)(r + 2) = 544; LOG("首领连战：换局，怪物预设表第五幕第 33 条改回巴尔 544"); } }
    memset(&R, 0, sizeof R); R.G = G; R.eligible = -1;
}

void dm_bossrush_c2s(uint64_t G, uint64_t player, const uint8_t *pkt, uint32_t len) {
    (void)player;
    if (G != R.G || len < 9 || pkt[0] != 0x41) return;   // 0x41 [单位 id][单位类型]：点了一个单位（实测点门：41 0f000000 02000000 ffffffff） | 0x41 [unit id][unit type]: a unit was tapped (measured when tapping the door: 41 0f000000 02000000 ffffffff)
    uint32_t id, ty; memcpy(&id, pkt + 1, 4); memcpy(&ty, pkt + 5, 4);
    uint64_t u = ty == 2 ? find_unit(G, 2, id, 0) : 0;
    R.pend_door = u && *(uint32_t *)(uintptr_t)(u + 4) == DOOR_OBJ ? id : 0; R.pend_at = tick_of(G);
}

// 首领掉落读数（出现过地狱安达莉尔一件没掉的报告，日志里没有击杀与掉落读数定不了性）：不论在不在连战，五个幕首领（类号见 k_boss）
// 活着时每 5 拍盯住一只、记下当时最大的物品 id；它的模式变成 0 / 12（死亡中 / 已死）时记下尸体位置，25 拍后数尸体 20 格内 id 更大、在地上（模式 3）或正在落地（5）的物品。
// 宝藏格从怪物表读（[数据表+0xf90] + 类号 × 0x1fc，+0xc0 + 难度 × 0x10 那一块：+0 普通、+6 任务），极乐世界开着时读到的就是改过的值
// Boss drop readout (there was a report of Hell Andariel dropping nothing at all, and without kill and drop readouts in the log it could not be characterized): whether or not in the boss rush, for the five act bosses (class ids in k_boss)
// watch one every 5 ticks while alive and record the largest item id at that time; when its mode becomes 0 / 12 (dying / dead) record the corpse position, and 25 ticks later count the items within 20 cells of the corpse with a larger id that are on the ground (mode 3) or landing (5).
// The treasure class slot is read from the monster table ([data table+0xf90] + class id × 0x1fc, the block at +0xc0 + difficulty × 0x10: +0 normal, +6 quest); with Elysium mode on, what is read is the modified value
static const int k_boss[] = { 156, 211, 242, 243, 544 };
static void boss_drop_watch(uint64_t G, uint32_t t) {
    static uint64_t g; static uint32_t id, t_die, max_item; static int cls, x, y;
    if (G != g) { g = G; id = 0; t_die = 0; }
    if (t_die) { if (t - t_die < 25) return;
        int n = 0, nc = 0; char lst[200] = "";   // nc = 客户端单位表（main+0x2d603a0 + 类型 4 × 0x400，128 桶按 id 低 7 位）里也有的件数：服务端有、客户端没有 = 玩家看不见 | nc = number of items also present in the client unit table (main+0x2d603a0 + type 4 × 0x400, 128 buckets by the low 7 bits of the id): on the server but not on the client = the player cannot see it
        for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2e30 + 8 * b); u; u = rd(u + 0x158)) { uint32_t m = *(uint32_t *)(uintptr_t)(u + 0xc); int ix = 0, iy = 0;
            if (*(uint32_t *)(uintptr_t)(u + 8) <= max_item || (m != 3 && m != 5)) continue; uint64_t p = rd(u + 0x38); if (!p) continue; ix = *(int32_t *)(uintptr_t)(p + 0x10); iy = *(int32_t *)(uintptr_t)(p + 0x14);
            if (abs(ix - x) > 20 || abs(iy - y) > 20) continue; n++; uint32_t iid = *(uint32_t *)(uintptr_t)(u + 8);
            int k = 0;   // 客户端表归客户端线程管，这里只读、链长封顶（客体堆释放后仍可读，不会缺页） | the client table is managed by the client thread; here it is read-only and the chain length is capped (guest heap memory is still readable after being freed, no page fault)
            for (uint64_t c = rd((uint64_t)(uintptr_t)(B + 0x2d603a0 + 4 * 0x400 + (iid & 0x7f) * 8)); c && k < 64; c = rd(c + 0x158), k++) if (*(uint32_t *)(uintptr_t)(c + 8) == iid) { nc++; break; } size_t L = strlen(lst); if (L < 180) snprintf(lst + L, sizeof lst - L, " %u", *(uint32_t *)(uintptr_t)(u + 4)); }
        uint8_t d = *(uint8_t *)(uintptr_t)(G + 0x104), ver = *(uint8_t *)(uintptr_t)(G + 0x106); uint64_t T = rd((uint64_t)(uintptr_t)(B + 0x2d64a18 + ver * 16)), mt = T ? rd(T + 0xf90) : 0;
        uint16_t *blk = mt ? (uint16_t *)(uintptr_t)(mt + (uint64_t)cls * 0x1fc + 0xc0 + d * 0x10) : NULL;
        extern _Atomic int g_dm_l_presses; int lp = atomic_load(&g_dm_l_presses);
        LOG("首领掉落读数：类号 %d 拍 %u 死于 %d,%d（难度 %u，宝藏格 普通 %u / 任务 %u），死后 25 拍尸体 20 格内新落地 %d 件%s%s；客户端有其中 %d 件；「显示物品」键本进程交给游戏 %d 次（%s）",
            cls, t_die, x, y, d, blk ? blk[0] : 0, blk ? blk[3] : 0, n, n ? "：类号" : "【零掉落】", lst, nc, lp, lp % 2 ? "单数 = 名字常显" : "双数 = 名字没开，或开了又被关");
        id = 0; t_die = 0; return; }
    if (t % 5) return;
    uint64_t u = 0;
    if (id) { u = find_unit(G, 1, id, 0); if (!u) { id = 0; return; } }   // 人走远单位被释放：下次见到再盯 | the player walked far away and the unit was freed: watch it again the next time it is seen
    else { for (unsigned i = 0; i < sizeof k_boss / sizeof *k_boss && !u; i++) { u = find_unit(G, 1, 0, k_boss[i]); if (u) { uint32_t m = *(uint32_t *)(uintptr_t)(u + 0xc); if (m == 0 || m == 12) u = 0; } }
        if (!u) return; id = *(uint32_t *)(uintptr_t)(u + 8); cls = (int)*(uint32_t *)(uintptr_t)(u + 4); }
    uint32_t m = *(uint32_t *)(uintptr_t)(u + 0xc); if (m == 0 || m == 12) { unit_xy(u, &x, &y); t_die = t; return; }
    // 掉落在死亡那一拍里同步生成，看到死时已经有了：所以门槛（最大物品 id）在它活着的每次巡检时更新
    // Drops are generated synchronously within the tick of death and already exist by the time the death is seen: so the threshold (largest item id) is updated at every patrol check while it is alive
    max_item = 0; for (int b = 0; b < 128; b++) for (uint64_t v = rd(G + 0x2e30 + 8 * b); v; v = rd(v + 0x158)) { uint32_t i = *(uint32_t *)(uintptr_t)(v + 8); if (i > max_item) max_item = i; }
}

void dm_bossrush_tick(uint64_t G, uint64_t player, uint8_t *main_base) {
    B = main_base; if (!G || !player || !B) return;
    // 认「新的一局」：退出再进同一进程时新局常复用同一块 G（br4 复现），只比 G 认不出。
    // 每局拍号从 0 起、本函数每拍都被调，所以拍号倒退 = 换了局；玩家单位换了也算
    // Recognizing "a new run": when exiting and re-entering within the same process, the new run often reuses the same G block (br4 reproduction), so comparing G alone cannot tell.
    // Each run's tick number starts from 0 and this function is called every tick, so the tick number going backward = the run changed; the player unit changing also counts
    uint32_t t = tick_of(G);
    if (G != R.G || player != R.player || t < R.last_t) { reset(G); R.player = player; }
    R.last_t = t;
    int lv = unit_level(player); uint64_t room = unit_room(player); if (!lv || !room) return;
    boss_drop_watch(G, t);
    // 测试开关 DM_TEST_WARP_LV=<拍>:<关卡号>（造现场用，亮度对比截图）：到这一拍照原版王座门的做法 0x634000(游戏, 玩家, 关卡, 11) 把人送进这个关卡，一局一次
    // Test switch DM_TEST_WARP_LV=<tick>:<level id> (for setting up a scenario, brightness comparison screenshots): at this tick send the player into this level the way the original game's Throne door does, 0x634000(game, player, level, 11), once per run
    { static int at = -1, wl; if (at < 0) { const char *e = getenv("DM_TEST_WARP_LV"); at = 0; if (e && sscanf(e, "%d:%d", &at, &wl) != 2) at = 0; }
      if (at > 0 && t >= (uint32_t)at && lv != wl) { at = 0; LOG("测试 DM_TEST_WARP_LV：拍 %u 从关卡 %d 进 %d", t, lv, wl); ((void (*)(uint64_t, uint64_t, int, int))(B + 0x634000))(G, player, wl, 11); return; } }
    if (R.eligible < 0) { R.eligible = eligible(G, player); R.total = *(uint8_t *)(uintptr_t)(G + 0x104) == 2 ? 12 : 10;
        LOG("首领连战：本局%s资格（难度 %u，共 %d 关）", R.eligible ? "有" : "没有", *(uint8_t *)(uintptr_t)(G + 0x104), R.total); }
    // 测试开关 DM_TEST_SPAWN=<拍>:<类号>:<dx>,<dy>（造现场用）：到拍在玩家格 + (dx, dy) 附近找空地刷一只怪（同连战刷首领：0x7160d0 找空地、0x7ce630 刷）。
    // 首领顶部血条复现用（远处 / 画面边上的首领）
    // Test switch DM_TEST_SPAWN=<tick>:<class id>:<dx>,<dy> (for setting up a scenario): at the tick, find open ground near player cell + (dx, dy) and spawn one monster (same as boss rush boss spawning: 0x7160d0 finds open ground, 0x7ce630 spawns).
    // Used to reproduce the boss top health bar (bosses far away / at the edge of the screen)
    { static int st = -1, at, cls, dx, dy; if (st < 0) { const char *e = getenv("DM_TEST_SPAWN"); st = e && sscanf(e, "%d:%d:%d,%d", &at, &cls, &dx, &dy) == 4 ? 0 : 1; }
      if (!st && t >= (uint32_t)at) { st = 1; int x = 0, y = 0; unit_xy(player, &x, &y); int32_t pos[2] = { x + dx, y + dy }; uint64_t rr = free_spot(G, room, pos);
          uint64_t u = rr ? ((uint64_t (*)(uint64_t, uint64_t, int, int, int, int, int, int, uint64_t))(B + 0x7ce630))(G, rr, 1, cls, pos[0], pos[1], 1, 0, 0x10) : 0;
          LOG("DM_TEST_SPAWN：拍 %u 在 %d,%d（玩家 %d,%d）刷类号 %d → %s", t, pos[0], pos[1], x, y, cls, u ? "成" : "败"); } }
    if (!R.eligible) return;
    dm_door_label(B, G, DOOR_OBJ, "dmDoorBossRush", lv == LV_TOWN);

    // 门：哈洛加斯中间那片空地（真机上让人站到那块地，读得 (5076,5080) = 关卡原点 + (76, 80)）。从这一格起一圈圈找
    // 「本身是空地、3 格内没有物件」的格；那片房间没载入（人还在北边）就等，载入了才开
    // Door: the open ground in the middle of Harrogath (on a real device the player was made to stand on that ground, reading (5076,5080) = level origin + (76, 80)). Starting from this cell, search ring by ring for
    // a cell that "is itself open ground, with no objects within 3 cells"; if that group of rooms is not loaded (the player is still in the north), wait, and open it only once loaded
    if (lv != LV_TOWN) R.town_tried = 0;
    if (lv == LV_TOWN && !R.door_town && !R.town_tried) { uint64_t lvt = level_of(player); int x0, y0, x1, y1; level_box(lvt, &x0, &y0, &x1, &y1);
        int cx = x0 + 76, cy = y0 + 80; if (room_at(lvt, cx, cy)) { R.town_tried = 1; uint64_t rr = 0; int32_t pos[2] = {cx, cy};
            for (int r = 0; r <= 5 && !rr; r++) for (int dx = -r; dx <= r && !rr; dx++) for (int dy = -r; dy <= r && !rr; dy++) { if (abs(dx) != r && abs(dy) != r) continue; pos[0] = cx + dx; pos[1] = cy + dy; rr = clear_spot(G, lvt, pos[0], pos[1], 3); }
            if (rr) R.door_town = make_door(G, player, rr, pos, LV_SUMMIT, R.town_xy);
            LOG("首领连战：哈洛加斯开门 %s（物件 id %u 在 %d,%d；目标 %d,%d）", R.door_town ? "成功" : "失败", R.door_town, R.town_xy[0], R.town_xy[1], cx, cy); } }

    // 点了门且走到门前（4 格内）→ 换关。哈洛加斯的门：去当前关所在的图；第 5 关后开的门：去 132；配出来的对门（120 / 132 落点那扇）：回哈洛加斯
    // Tapped the door and walked up to it (within 4 cells) → change level. The Harrogath door: goes to the map of the current stage; the door opened after stage 5: goes to 132; the paired return door (the one at the 120 / 132 landing point): back to Harrogath
    if (R.pend_door) { uint64_t du = find_unit(G, 2, R.pend_door, 0); int px, py, dx, dy;
        int mine = R.pend_door == R.door_town || R.pend_door == R.door_summit, back = !mine && (lv == LV_SUMMIT || lv == LV_CHAMBER);
        if (!du || (!mine && !back) || t - R.pend_at > 250) R.pend_door = 0;
        else if (unit_xy(player, &px, &py) && unit_xy(du, &dx, &dy) && abs(px - dx) <= 4 && abs(py - dy) <= 4) {
            int to = back ? LV_TOWN : R.pend_door == R.door_summit ? LV_CHAMBER : R.stage < 5 ? LV_SUMMIT : LV_CHAMBER; R.pend_door = 0;
            if (R.done && !back) say("%s", dm_str(B, "dmBrDoneThisGame"));
            else { LOG("首领连战：拍 %u 进门 → %d", t, to); R.tp_due = to != LV_TOWN;  ((void (*)(uint64_t, uint64_t, int, int))(B + 0x634000))(G, player, to, 11); return; } } }

    // 擂台：门的落点（类型 11 = 传送门落点）就在关卡入口，旁边是出口砖与入口物件（120：出口砖 c79 (10057,12666)、物件 c564 (10057,12667)，
    // 离落点 6 格）。首领追过来站在入口上，手指点首领、按主攻击键都被判成点出口砖 / 物件（走过去交互），打不着——看着像「放不了技能」。
    // 所以人从我们的门（换关落在入口）进到连战关卡时，用游戏自己的传送把人挪到朝关卡中心 18 格处，首领再往里刷
    // Arena: the door's landing point (type 11 = portal landing point) is right at the level entrance, with the exit tile and the entrance object next to it (120: exit tile c79 (10057,12666), object c564 (10057,12667),
    // 6 cells from the landing point). The boss chases over and stands on the entrance, and tapping the boss with a finger or pressing the main attack button is both judged as tapping the exit tile / object (walk over and interact), so it cannot be hit -- it looks like "can't cast skills".
    // So when the player enters the boss rush level through our door (the level change lands at the entrance), use the game's own teleport to move the player to 18 cells toward the level center, and the boss spawns further in
    if (lv != R.lv_prev) { R.lv_prev = lv; R.in_since = t; if (lv != LV_SUMMIT && lv != LV_CHAMBER) R.tp_due = 0;
        // 测试开关 DM_TEST_BR_LOSE=1：人离开连战关卡时把本关没死的首领 id 换成不存在的值，模拟「人走后单位被游戏释放」，验接回那一支（真首领仍在，会多一只）
        // Test switch DM_TEST_BR_LOSE=1: when the player leaves the boss rush level, replace the ids of this stage's bosses that are not dead with nonexistent values, simulating "units freed by the game after the player leaves", to verify the reattach branch (the real boss is still there, so there will be one extra)
        static int lose = -1; if (lose < 0) lose = getenv("DM_TEST_BR_LOSE") != NULL;
        if (lose && R.nboss && lv != k_stage[R.stage].lv) for (int i = 0; i < R.nboss; i++) if (!R.boss_dead[i]) { R.boss[i] = 0x7ffffff0u + i; LOG("首领连战：测试 DM_TEST_BR_LOSE：第 %d 关首领 id 换成不存在的值", R.stage + 1); } }
    // 进图第一拍就挪（读图界面还盖着，玩家看不到第二次位移）
    // Move on the very first tick after entering the map (the loading screen still covers it, so the player does not see the second displacement)
    if (R.tp_due && lv == R.lv_prev && (lv == LV_SUMMIT || lv == LV_CHAMBER)) { R.tp_due = 0; int px, py, x0, y0, x1, y1; unit_xy(player, &px, &py); level_box(level_of(player), &x0, &y0, &x1, &y1);
        double vx = (x0 + x1) / 2.0 - px, vy = (y0 + y1) / 2.0 - py, vl = vx * vx + vy * vy > 1 ? __builtin_sqrt(vx * vx + vy * vy) : 1;
        int ok = 0; for (int d = 18; d >= 10 && !ok; d -= 4) ok = teleport(G, player, px + (int)(d * vx / vl), py + (int)(d * vy / vl));
        int nx, ny; unit_xy(player, &nx, &ny); R.arena_lv = lv; R.arena_xy[0] = nx; R.arena_xy[1] = ny;
        LOG("首领连战：拍 %u 进关卡 %d 落在入口 %d,%d → 挪进擂台 %s，现在 %d,%d", t, lv, px, py, ok ? "成" : "不成", nx, ny); }

    // 测试开关 DM_TEST_BR_WARP132=<拍>：人在城里、连战没开始或已打完时，到这一拍照原版王座 → 大殿那扇门的做法（op 70，0x755d38：0x634000(游戏, 玩家, 132, 11)）进 132，
    // 验「不进连战的局原生巴尔照常在」
    // Test switch DM_TEST_BR_WARP132=<tick>: when the player is in town and the boss rush has not started or is already finished, at this tick enter 132 the way the original game's Throne → chamber door does (op 70, 0x755d38: 0x634000(game, player, 132, 11)),
    // to verify "in a run that does not enter the boss rush, the native Baal is there as usual"
    { static int at = -1; if (at < 0) { const char *e = getenv("DM_TEST_BR_WARP132"); at = e ? atoi(e) : 0; }
      if (at > 0 && (R.done || (!R.stage && !R.nboss)) && lv == LV_TOWN && t >= (uint32_t)at) { at = 0; LOG("首领连战：测试 DM_TEST_BR_WARP132：拍 %u 照原版王座门进 132", t); ((void (*)(uint64_t, uint64_t, int, int))(B + 0x634000))(G, player, LV_CHAMBER, 11); return; } }
    // 第 6–10 关在大殿里时强制地带，其余时候按真实时钟
    // Force the zone while stages 6–10 are in the chamber; the rest of the time follow the real clock
    static int tz_off = -1; if (tz_off < 0) { const char *e = getenv("DM_BOSSRUSH_TZ"); tz_off = e && e[0] == '0'; }   // 测试开关 DM_BOSSRUSH_TZ=0：不强制地带（对照局用） | test switch DM_BOSSRUSH_TZ=0: do not force the zone (for comparison runs)
    int want = !tz_off && !R.done && R.stage >= 5 && R.stage < 10 && lv == LV_CHAMBER;
    if (want != R.tz_on) tz_set(G, want);
    // 连战进行中（进过 120 起、没打完）：132 一建出来就把原生巴尔那条预设改成不刷；打完改回。换局不用管：预设记录属于上一局的建图数据，随局释放
    // 132 原生巴尔：人第一拍在大殿时 132 已建好（巴尔那条预设已取成 {怪, 817} 不刷），预设表立刻改回，别的图、以后的建图不受影响
    // Boss rush in progress (from having entered 120, not yet finished): as soon as 132 is built, change the native Baal preset to not spawn; change it back when finished. Switching runs needs no handling: the preset record belongs to the previous run's map-building data and is freed with the run
    // 132 native Baal: on the first tick the player is in the chamber, 132 is already built (the Baal preset has already been taken as {monster, 817}, not spawned), so the preset table is changed back immediately; other maps and later map building are not affected
    if (lv == LV_CHAMBER && R.table_off) { uint64_t u = chamber_rec(level_of(player), 1, NOTHING_CLS); baal_table(G, 0);
        LOG("首领连战：132 已建，原生巴尔预设%s", u ? "已取成不刷" : "【红】没取成不刷（站位 (15135,5920) 找不到 {怪, 817} 记录）"); }
    if (R.tz_on) tz_live();

    if (R.loot_at && (int32_t)(t - R.loot_at) >= 0) { R.loot_at = 0; int n1 = 0, n2 = 0, all = 0;   // 物品表 G+0x2e30（同 diag.c dm_diag_srv_item），码 0x570260，品质 = 数据块 +0，模式 3 = 在地上 | item table G+0x2e30 (same as diag.c dm_diag_srv_item), code 0x570260, quality = data block +0, mode 3 = on the ground
        for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2e30 + 8 * b); u; u = rd(u + 0x158)) { uint64_t d = rd(u + 0x10);
            if (*(uint32_t *)(uintptr_t)u != 4 || *(uint32_t *)(uintptr_t)(u + 0xc) != 3 || !d) continue; all++; if (*(uint32_t *)(uintptr_t)d != 7) continue;
            uint32_t c = ((uint32_t (*)(uint64_t))(B + 0x570260))(u); n1 += !memcmp(&c, "cm1 ", 4); n2 += !memcmp(&c, "cm2 ", 4); }
        LOG("首领连战：拍 %u 本局地上物品 %d 件，其中暗金小型神符 %d 件、暗金大型神符 %d 件", t, all, n1, n2); }
    // 本关首领记账：每拍不论人在哪都看一遍。见到模式 0 / 12 = 死了，只认这一条；找不到（人离开后关卡房间卸掉，单位随之释放——
    // 原来「找不到 = 重刷」就是这条路：走门、回城卷轴离开 120 都会让首领从服务端单位表里消失，回来就满血重刷一只）不算死，人回到本关卡 2 秒后还找不到，
    // 才在原刷点按记下的生命补一只（「接回」），死过的绝不再刷
    // 第 5 关门：开在最后一只首领倒下处旁 2–3 格（那圈里离掉落都不到 3 格才放到 4–5 格），同一圈 16 个方向里挑「离最近的地上掉落最远」的空地，
    // 门和它的名字标签不压在掉落堆里（宿主截图：只看 2 格内有没有掉落时，门照样夹在前几关的掉落名中间）
    // Bookkeeping for this stage's bosses: checked once every tick no matter where the player is. Seeing mode 0 / 12 = dead, and only this counts; not found (after the player leaves, the level's rooms are unloaded and the units are freed with them --
    // the old "not found = respawn" was exactly this path: leaving 120 through the door or by a town portal scroll both make the boss vanish from the server unit table, and coming back respawned one at full life) does not count as dead; only if it is still not found 2 seconds after the player returns to this level
    // is one spawned at the original spawn point with the recorded life ("reattach"); ones that have died are never spawned again
    // Stage 5 door: opened 2–3 cells beside where the last boss fell (only placed at 4–5 cells if everything in that ring is less than 3 cells from the drops), choosing the open ground "farthest from the nearest ground drop" among the 16 directions of the same ring,
    // so the door and its name label are not buried in the drop pile (host screenshot: when only checking whether there are drops within 2 cells, the door still ended up sandwiched among the drop names of the earlier stages)
    if (R.door_at && (int32_t)(t - R.door_at) >= 0 && lv == LV_SUMMIT) { R.door_at = 0; uint64_t lvp = level_of(player), best_rr = 0; int32_t best[2] = {0, 0}; int bx = R.boss_xy[0], by = R.boss_xy[1], bd = -1;
        static int lx[256], ly[256]; int nl = 0;
        for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2e30 + 8 * b); u && nl < 256; u = rd(u + 0x158)) { uint64_t ip = rd(u + 0x38);
            if (*(uint32_t *)(uintptr_t)(u + 0xc) != 3 || !ip) continue; lx[nl] = *(int32_t *)(uintptr_t)(ip + 0x10); ly[nl++] = *(int32_t *)(uintptr_t)(ip + 0x14); }
        static const int8_t dir[16][2] = {{2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {-1, 2}, {-2, 2}, {-2, 1}, {-2, 0}, {-2, -1}, {-2, -2}, {-1, -2}, {0, -2}, {1, -2}, {2, -2}, {2, -1}};
        // 先看 2–3 格那圈，里面有离掉落 ≥ 3 格的就用它；没有再看 4–5 格那圈
        // Look at the 2–3 cell ring first and use it if it has a spot ≥ 3 cells from the drops; only if not, look at the 4–5 cell ring
        for (int r = 1; r <= 2 && bd < 3; r++) for (int d = 0; d < 16; d++) { int32_t pos[2] = {bx + dir[d][0] * r + (r == 2 ? dir[d][0] / 2 : 0), by + dir[d][1] * r + (r == 2 ? dir[d][1] / 2 : 0)};   // 半径约 2–3 格与 4–5 格两圈 | two rings, at radius about 2–3 cells and 4–5 cells
            int dmin = 99; for (int i = 0; i < nl; i++) { int dd = abs(lx[i] - pos[0]) > abs(ly[i] - pos[1]) ? abs(lx[i] - pos[0]) : abs(ly[i] - pos[1]); if (dd < dmin) dmin = dd; }
            if (dmin <= bd) continue; uint64_t rr = clear_spot(G, lvp, pos[0], pos[1], 3); if (rr) { bd = dmin; best_rr = rr; best[0] = pos[0]; best[1] = pos[1]; } }
        if (best_rr) R.door_summit = make_door(G, player, best_rr, best, LV_CHAMBER, R.summit_xy);
        LOG("首领连战：亚瑞特之巅开门 %s（物件 id %u 在 %d,%d；巴尔倒在 %d,%d；地上掉落 %d 件，最近的离门 %d 格）", R.door_summit ? "成功" : "失败", R.door_summit, R.summit_xy[0], R.summit_xy[1], bx, by, nl, bd);
        say("%s", dm_str(B, "dmBrToChamber")); }
    if (R.nboss) { int alive = 0, missing = 0;
        for (int i = 0; i < R.nboss; i++) { if (R.boss_dead[i]) continue; uint64_t u = find_unit(G, 1, R.boss[i], 0);
            if (!u) { missing++; continue; } uint32_t md = *(uint32_t *)(uintptr_t)(u + 0xc);
            if (md == 0 || md == 12) { R.boss_dead[i] = 1; LOG("首领连战：拍 %u 第 %d 关类号 %d 死亡", t, R.stage + 1, k_stage[R.stage].cls[i]); continue; }
            alive++; R.boss_hp[i] = ((int (*)(uint64_t, int, int))(B + 0x5b9290))(u, 6, 0); unit_xy(u, &R.boss_xy[0], &R.boss_xy[1]);
            if (t % 125 == 0) LOG("首领连战：拍 %u 第 %d 关类号 %d 生命 %d/%d", t, R.stage + 1, k_stage[R.stage].cls[i], R.boss_hp[i] >> 8, ((int (*)(uint64_t, int, int))(B + 0x5b9290))(u, 7, 0) >> 8); }   // 每 5 秒一行：回归判「打得动」看它降 | one line every 5 seconds: regression judges "can be damaged" by watching it go down
        if (!alive && !missing) {
            LOG("首领连战：拍 %u 第 %d 关打完", t, R.stage + 1); R.nboss = 0; R.stage++; R.next_at = t + 75;
            if (R.stage >= 11) R.loot_at = t + 50;   // 地狱第 11 / 12 关：2 秒后看地上有没有暗金小 / 大型神符（毁灭 / 地狱火炬），给回归核对 | Hell stages 11 / 12: after 2 seconds check whether there is a unique small / large charm (Annihilus / Hellfire Torch) on the ground, for regression to check against
            if (R.stage == R.total) { R.done = 1; if (R.tz_on) tz_set(G, 0); say("%s", dm_str(B, "dmBrAllDone")); return; }
            if (R.stage == 5) { R.door_at = t + 50; baal_table(G, 1); }   // 门等掉落都落地（2 秒）再开，好避开掉落堆 | the door opens only after the drops have all landed (2 seconds), to avoid the drop pile
            return; }
        if (missing && lv == k_stage[R.stage].lv && t - R.in_since >= 50) for (int i = 0; i < R.nboss; i++) {
            if (R.boss_dead[i] || find_unit(G, 1, R.boss[i], 0)) continue;
            int32_t pos[2] = {R.spawn_xy[i][0], R.spawn_xy[i][1]}; uint64_t rs = room_at(level_of(player), pos[0], pos[1]), rr = rs ? free_spot(G, rs, pos) : 0;
            uint64_t u = rr ? ((uint64_t (*)(uint64_t, uint64_t, int, int, int, int, int, int, uint64_t))(B + 0x7ce630))(G, rr, 1, k_stage[R.stage].cls[i], pos[0], pos[1], 1, 0, 0x10) : 0;
            if (!u) continue;
            if (R.boss_hp[i] > 0) ((void (*)(uint64_t, int, int, int))(B + 0x5bc1f0))(u, 6, R.boss_hp[i], 0);
            LOG("首领连战：拍 %u 第 %d 关类号 %d 不在单位表里（人离开过关卡），按原刷点接回：生命 %d → 新单位 id %u", t, R.stage + 1, k_stage[R.stage].cls[i], R.boss_hp[i] >> 8, *(uint32_t *)(uintptr_t)(u + 8));
            R.boss[i] = *(uint32_t *)(uintptr_t)(u + 8); }
        return; }
    if (R.done || lv != k_stage[R.stage].lv) { R.next_at = 0; return; }
    { uint32_t pm = *(uint32_t *)(uintptr_t)(player + 0xc); if (pm == 0 || pm == 17) { R.next_at = 0; return; } }   // 人倒下（0 正在死 / 17 已死）时不刷下一关，复活回来再刷 | do not spawn the next stage while the player is down (0 dying / 17 dead); spawn after they revive and come back
    if (!R.next_at) R.next_at = t + 25;   // 进关卡后 1 秒再刷 | spawn 1 second after entering the level
    if ((int32_t)(t - R.next_at) < 0) return;
    // 刷点（别都刷在同一点，否则下一只被上一只的掉落挡住）：擂台再朝关卡中心 10 格为圆心，半径 8 格的五个固定点按关号轮换，
    // 顺序隔一个跳（0°、144°、288°、72°、216°），相邻两关相距约 15 格；多只时绕本关点左右错开 6 格。该格所在房间没载入就退回玩家旁 8 格
    // Spawn points (do not spawn them all at the same point, otherwise the next one is blocked by the previous one's drops): centered 10 cells further from the arena toward the level center, five fixed points at radius 8 cells are rotated by stage number,
    // in skip-one order (0°, 144°, 288°, 72°, 216°), so two adjacent stages are about 15 cells apart; with several bosses they are staggered 6 cells left and right around this stage's point. If the room containing that cell is not loaded, fall back to 8 cells beside the player
    int px, py, x0, y0, x1, y1; unit_xy(player, &px, &py); uint64_t lvp = level_of(player); level_box(lvp, &x0, &y0, &x1, &y1);
    int ax = R.arena_lv == lv ? R.arena_xy[0] : px, ay = R.arena_lv == lv ? R.arena_xy[1] : py;
    double vx = (x0 + x1) / 2.0 - ax, vy = (y0 + y1) / 2.0 - ay, vl = vx * vx + vy * vy > 1 ? __builtin_sqrt(vx * vx + vy * vy) : 1;
    static const float ring[5][2] = {{8, 0}, {-6.47f, 4.70f}, {2.47f, -7.61f}, {2.47f, 7.61f}, {-6.47f, -4.70f}};   // 半径 8 格，0° / 144° / 288° / 72° / 216° | radius 8 cells, 0° / 144° / 288° / 72° / 216°
    float rx = ring[R.stage % 5][0], ry = ring[R.stage % 5][1];
    for (int i = 0; i < k_stage[R.stage].n; i++) {
        int32_t pos[2] = {ax + (int)(10 * vx / vl + rx) + (int)(6 * (i - (k_stage[R.stage].n - 1) / 2.0) * -vy / vl), ay + (int)(10 * vy / vl + ry) + (int)(6 * (i - (k_stage[R.stage].n - 1) / 2.0) * vx / vl)};
        uint64_t rs = room_at(lvp, pos[0], pos[1]); if (!rs) { rs = room; pos[0] = px + 8; pos[1] = py; }
        uint64_t rr = free_spot(G, rs, pos);
        uint64_t u = rr ? ((uint64_t (*)(uint64_t, uint64_t, int, int, int, int, int, int, uint64_t))(B + 0x7ce630))(G, rr, 1, k_stage[R.stage].cls[i], pos[0], pos[1], 1, 0, 0x10) : 0;
        if (u) { int k = R.nboss++; R.boss[k] = *(uint32_t *)(uintptr_t)(u + 8); R.boss_dead[k] = 0; R.boss_hp[k] = 0; R.spawn_xy[k][0] = pos[0]; R.spawn_xy[k][1] = pos[1]; }
        LOG("首领连战：拍 %u 第 %d 关刷类号 %d 在 %d,%d → %s（等级 %d）", t, R.stage + 1, k_stage[R.stage].cls[i], pos[0], pos[1], u ? "成功" : "失败",
            u ? ((int (*)(uint64_t, int, int))(B + 0x5b9290))(u, 12, 0) : 0); }
    if (!R.nboss) { R.next_at = t + 25; return; }
    char st[160]; snprintf(st, sizeof st, dm_str(B, "dmBrStage"), R.stage + 1, R.total, dm_str(B, k_stage[R.stage].name), R.stage >= 5 && R.stage < 10 ? dm_str(B, "dmBrTerror") : "");
    say("%s", st);
}
