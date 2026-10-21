// 刷怪积分牌：按角色、按难度记名单上首领的击杀次数、首杀时刻、最快交战用时。
//   计数：服务端怪物表（G+0x2230 + 类型 1 × 0x400）里名单上的怪，活着时见过、模式变 0（死亡中）/ 12（已死）那一拍算死一次。
//     认法：关底首领、血鸦等按类号（单位 +4）；超级独特按怪物数据（单位 +0x10）+0x2a 的超级独特编号——刷超级独特 0x69b310 写的
//     （strh w22,[x8,#0x2a]），类型标志 +0x1a 位 1 = 超级独特。
//     每局一张「见过的单位 id」表：同一只一局只计一次；第一次见到就是尸体的不计；首领连战本关刷出的（dm_bossrush_owns）不计、不提示。
//     高庭理事会（伊斯梅尔 / 吉勒布 / 托克）与三位先祖（塔力克 / 马道克 / 科里克）各算一行，同一局三只都计过死的那一拍加 1。
//   最快用时：交战用时 = 第一次掉血那一拍 → 死亡那一拍（游戏拍，25 拍 / 秒）；回满血从头算；一组的按「第一只掉血 → 最后一只死」。
//   提示（原版系统提示框 + 聊天栏，dm_game_notice）：客户端单位表里第一次出现这只（一组算一次）→「<名>已累计击杀 N 次！」（N = 当前难度、不含这一次）；
//     死后 →「<名>第 N 次成功击杀！」。名字一律取原版字符串（0x7f5630），旧译名开着时跟着换。
//   存：<存档工作目录>/<角色名>/<角色名>.sw_kills（文本，见 save()），每次计数当场写进工作目录，随游戏下一次存档提交原子换入、随角色目录同步 iCloud；
//     没存档就退出时这段不算，同原版「没存就丢进度」。原版存档一字节不动。
// 只在服务端拍里调（diag.c，首领连战之后）。
// Kill board: per character and per difficulty, records kill count, first-kill time and fastest engagement time for the listed bosses.
//   Counting: a listed monster in the server monster table (G+0x2230 + type 1 × 0x400) that was seen alive counts as one death on the tick its mode changes to 0 (dying) / 12 (dead).
//     Identification: act bosses, Blood Raven etc. by class id (unit +4); super uniques by the super unique number at +0x2a of the monster data (unit +0x10) -- written by the super unique spawn 0x69b310
//     (strh w22,[x8,#0x2a]); type flags +0x1a bit 1 = super unique.
//     One "seen unit ids" table per run: the same unit is counted only once per run; one first seen as a corpse is not counted; those spawned by the current boss rush stage (dm_bossrush_owns) are not counted and not announced.
//     The High Council (Ismail / Geleb / Toorc) and the three Ancients (Talic / Madawc / Korlic) each count as one row; +1 on the tick when all three have been counted dead in the same run.
//   Fastest time: engagement time = the tick of the first life loss -> the death tick (game ticks, 25 ticks / second); starts over on returning to full life; for a group it is "first one loses life -> last one dies".
//   Notices (original game system notice box + chat bar, dm_game_notice): the first time this unit appears in the client unit table (once per group) -> 「<名>已累计击杀 N 次！」(<name> killed N times so far!) (N = current difficulty, not counting this one);
//     after death -> 「<名>第 N 次成功击杀！」(<name> killed successfully for the Nth time!). Names always come from the original game strings (0x7f5630) and follow along when the old translated names are enabled.
//   Storage: <save working dir>/<character name>/<character name>.sw_kills (text, see save()); each count is written to the working directory immediately, swapped in atomically with the game's next save commit, and synced to iCloud with the character directory;
//     on exit without saving this part does not count, same as the original game's "progress is lost if not saved". The original save is not changed by a single byte.
// Called only in the server tick (diag.c, after boss rush).
#include "dm_bossrush.h"
#include "dm_input.h"
#include "dm_cursor.h"
#include "dm.h"
#include "dm_rift.h"
#include "hle.h"
#include "dm_i18n.h"
#include "dm_widget.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

const char *dm_save_workdir(void);

enum { K_CLS, K_SU, K_GRP };
typedef struct { const char *key, *str; uint8_t act, kind; int16_t id, id2; } kb_row;   // key = 侧文件里的键（不改）；str = 原版字符串键；id2 = 另一种认法（类号行的超级独特编号） | key = the key in the sidecar file (never changed); str = original game string key; id2 = the other way to identify (super unique number for class-id rows)
static const kb_row k_rows[] = {
    {"andariel", "Andariel", 1, K_CLS, 156, -1}, {"countess", "The Countess", 1, K_SU, 6, -1}, {"bloodraven", "Bloodraven", 1, K_CLS, 267, -1},
    {"cowking", "The Cow King", 1, K_SU, 39, -1}, {"bishibosh", "Bishibosh", 1, K_SU, 0, -1}, {"corpsefire", "Corpsefire", 1, K_SU, 40, -1},
    {"smith", "The Smith", 1, K_SU, 20, -1}, {"griswold", "Griswold", 1, K_SU, 5, -1}, {"rakanishu", "Rakanishu", 1, K_SU, 3, -1},
    {"coldcrow", "Coldcrow", 1, K_SU, 2, -1}, {"treehead", "Treehead WoodFist", 1, K_SU, 4, -1},
    {"duriel", "Duriel", 2, K_CLS, 211, -1}, {"summoner", "Summoner", 2, K_CLS, 250, 18}, {"radament", "Radament", 2, K_CLS, 229, 10},
    {"kaa", "Ancient Kaa the Soulless", 2, K_SU, 19, -1}, {"coldworm", "Coldworm the Burrower", 2, K_SU, 15, -1}, {"fangskin", "Fangskin", 2, K_SU, 12, -1},
    {"bloodwitch", "Bloodwitch the Wild", 2, K_SU, 11, -1}, {"beetleburst", "Beetleburst", 2, K_SU, 13, -1}, {"darkelder", "Dark Elder", 2, K_SU, 17, -1},
    {"fireeye", "Fire Eye", 2, K_SU, 16, -1},
    {"mephisto", "Mephisto", 3, K_CLS, 242, -1}, {"council", "Council Member", 3, K_GRP, 0, -1},
    {"bremm", "Bremm Sparkfist", 3, K_SU, 28, -1}, {"wyand", "Wyand Voidfinger", 3, K_SU, 30, -1}, {"maffer", "Maffer Dragonhand", 3, K_SU, 31, -1},
    {"sszark", "Web Mage the Burning", 3, K_SU, 21, -1}, {"endugu", "Witch Doctor Endugu", 3, K_SU, 22, -1}, {"stormtree", "Stormtree", 3, K_SU, 23, -1},
    {"sarina", "Sarina the Battlemaid", 3, K_SU, 24, -1}, {"icehawk", "Icehawk Riftwing", 3, K_SU, 25, -1}, {"wingeddeath", "Winged Death", 3, K_SU, 32, -1},
    {"tormentor", "The Tormentor", 3, K_SU, 33, -1}, {"taintbreeder", "Taintbreeder", 3, K_SU, 34, -1}, {"riftwraith", "Riftwraith the Cannibal", 3, K_SU, 35, -1},
    {"diablo", "Diablo", 4, K_CLS, 243, -1}, {"vizier", "Grand Vizier of Chaos", 4, K_SU, 38, -1}, {"deseis", "Lord De Seis", 4, K_SU, 37, -1},
    {"infector", "Infector of Souls", 4, K_SU, 36, -1}, {"izual", "izual", 4, K_CLS, 256, -1}, {"hephasto", "The Feature Creep", 4, K_SU, 41, -1},
    {"baal", "Baal Crab", 5, K_CLS, 544, -1}, {"nihlathak", "Nihlathak", 5, K_SU, 60, -1}, {"shenk", "Siege Boss", 5, K_SU, 42, -1},
    {"eldritch", "Megaflow Rectifier", 5, K_SU, 49, -1}, {"pindleskin", "Pindleskin", 5, K_SU, 52, -1}, {"dacfarren", "Dac Farren", 5, K_SU, 48, -1},
    {"threshsocket", "Threash Socket", 5, K_SU, 51, -1}, {"snapchip", "Snapchip Shatter", 5, K_SU, 53, -1}, {"frozenstein", "Frozenstein", 5, K_SU, 59, -1},
    {"ancients", NULL, 5, K_GRP, 1, -1},
    {"colenzo", "Baal Subject 1", 5, K_SU, 61, -1}, {"achmel", "Baal Subject 2", 5, K_SU, 62, -1}, {"bartuc", "Baal Subject 3", 5, K_SU, 63, -1},
    {"ventar", "Baal Subject 4", 5, K_SU, 64, -1}, {"lister", "Baal Subject 5", 5, K_SU, 65, -1},
    {"bonesaw", "Bonesaw Breaker", 5, K_SU, 47, -1}, {"axedweller", "Axe Dweller", 5, K_SU, 46, -1}, {"eyeback", "Eyeback Unleashed", 5, K_SU, 50, -1},
    {"sharptooth", "Sharp Tooth Sayer", 5, K_SU, 56, -1}, {"vinvear", "Vinvear Molech", 5, K_SU, 55, -1}, {"magma", "Magma Torquer", 5, K_SU, 57, -1},
    {"hellsbelle", "Anodized Elite", 5, K_SU, 54, -1}, {"blazeripper", "Blaze Ripper", 5, K_SU, 58, -1},
};
enum { NROW = sizeof k_rows / sizeof *k_rows };
static const int16_t k_grp_su[2][3] = { {26, 27, 29}, {43, 44, 45} };   // 高庭理事会、三位先祖 | High Council, the three Ancients
static const char *k_grp_str[2][3] = { {"Ismail Vilehand", "Geleb Flamefinger", "Toorc Icefist"}, {"Ancient Barbarian 1", "Ancient Barbarian 2", "Ancient Barbarian 3"} };

static struct { int32_t n; int64_t first; int32_t best, last; int64_t sum; int32_t nt, uq, st; } S[NROW][3];
// 每行每难度：次数、首杀时刻（Unix 秒）、最快 / 最近一次交战拍数（0 = 没有）、量到用时的那几次的总拍数与次数（平均 = sum / nt）、死后那一批掉落里的暗金 / 套装件数
// Per row per difficulty: count, first-kill time (Unix seconds), fastest / latest engagement in ticks (0 = none), total ticks and count of the kills where a time was measured (average = sum / nt), number of unique / set items in the batch of drops after death
static char g_name[64];                                                 // 已载入的是哪个角色（空 = 还没载） | Which character is loaded (empty = not loaded yet)
static struct { uint32_t id; int16_t row; int8_t grp, gm; uint8_t announced, dead, skip; uint32_t eng; } E[128];   // 本局见过的名单单位 | Listed units seen this run
static int nE;
static struct { uint8_t announced, mask; uint32_t eng; int uq, st; } GS[2];   // uq / st：三只没死齐前各自那批掉落先记这里，死齐那一拍再算进牌子          // 本局两组的状态：提示过没有、哪几只计过死、第一只掉血的拍 | uq / st: until all three are dead, each one's batch of drops is recorded here first, then added to the board on the tick the last one dies          // State of the two groups this run: whether announced, which members have been counted dead, the tick the first one lost life
static uint64_t g_G; static uint32_t g_last_t;
// 掉落：死亡那一拍记下「此前最大的物品 id」和尸体位置，25 拍后数尸体 20 格内 id 更大、在地上（模式 3 / 5）的暗金（品质 7）/ 套装（品质 5）件数——同 dm_bossrush.c 首领掉落读数。
// 掉落在死亡那一拍里同步生成，所以门槛取的是上一拍（名单上有活着的单位时每拍更新）
// Drops: on the death tick, record "the largest item id so far" and the corpse position; 25 ticks later count the unique (quality 7) / set (quality 5) items within 20 tiles of the corpse that have a larger id and are on the ground (mode 3 / 5) -- same as the boss drop readout in dm_bossrush.c.
// Drops are generated synchronously within the death tick, so the threshold is taken from the previous tick (updated every tick while a listed unit is alive)
static struct { int16_t row; int8_t d, grp; uint32_t t_die, max_item; int x, y; } P[16]; static int nP; static uint32_t g_maxit;
static uint8_t *B;

static uint64_t rd(uint64_t a) { return a ? *(volatile uint64_t *)(uintptr_t)a : 0; }
static const char *str_of(const char *key) { struct { const char *k; uint64_t n; } s = { key, strlen(key) }; const char *r = dm_untag(((const char *(*)(void *))(B + 0x7f5630))(&s)); return r && *r ? r : key; }   // 跳过西语串首的性别标记 | Skip the gender marker at the start of Spanish strings
static void row_name(int r, char *out, size_t cap) {
    const kb_row *k = &k_rows[r];
    if (k->kind == K_GRP && k->id == 1) { snprintf(out, cap, dm_str(B, "dmKbJoin3"), str_of(k_grp_str[1][0]), str_of(k_grp_str[1][1]), str_of(k_grp_str[1][2])); return; }
    strlcpy(out, str_of(k->str), cap);
}
static int64_t now_s(void) { const char *e = getenv("DM_FIXED_TIME"); return e ? strtoll(e, 0, 0) : (int64_t)time(NULL); }   // 确定性对拍时首杀时刻也取定值 | In deterministic tick-by-tick comparison the first-kill time also takes a fixed value

static int file_path(char *out, size_t cap) { const char *w = dm_save_workdir(); if (!w || !g_name[0]) return 0; snprintf(out, cap, "%s/%s/%s.sw_kills", w, g_name, g_name); return 1; }
static void load(void) {
    memset(S, 0, sizeof S); char p[1200], ln[256]; if (!file_path(p, sizeof p)) return; FILE *f = fopen(p, "r"); if (!f) { LOG("刷怪积分牌：「%s」还没有记录（%s）", g_name, p); return; } int n = 0;
    while (fgets(ln, sizeof ln, f)) { char key[40]; int d, c, b, la = 0, nt = 0, uq = 0, st = 0; long long t, sum = 0;   // 第 1 版只有前 5 列，后几列缺省 0 | Version 1 has only the first 5 columns; the later columns default to 0
        if (ln[0] == '#' || sscanf(ln, "%39s %d %d %lld %d %d %lld %d %d %d", key, &d, &c, &t, &b, &la, &sum, &nt, &uq, &st) < 5 || d < 0 || d > 2) continue;
        for (int r = 0; r < NROW; r++) if (!strcmp(k_rows[r].key, key)) { S[r][d].n = c; S[r][d].first = t; S[r][d].best = b; S[r][d].last = la; S[r][d].sum = sum; S[r][d].nt = nt; S[r][d].uq = uq; S[r][d].st = st; n++; } }
    fclose(f); LOG("刷怪积分牌：载入「%s」%d 条", g_name, n);
}
static void save(void) {   // 整份重写：先写同目录的临时文件再改名，存档提交克隆目录时看到的要么是旧的要么是新的 | Rewrite the whole file: write a temp file in the same directory first, then rename, so that when the save commit clones the directory it sees either the old one or the new one
    char p[1200], t[1220]; if (!file_path(p, sizeof p)) return; snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w"); if (!f) { LOG("【红】刷怪积分牌：写 %s 打不开", t); return; }
    fprintf(f, "# dm_kills 2\n# 键 难度(0 普通/1 噩梦/2 地狱) 次数 首杀时刻(Unix 秒) 最快 最近(交战拍数，25 拍/秒，0 = 无) 量到用时的总拍数 次数 暗金件数 套装件数\n");
    for (int r = 0; r < NROW; r++) for (int d = 0; d < 3; d++) if (S[r][d].n || S[r][d].uq || S[r][d].st)
        fprintf(f, "%s %d %d %lld %d %d %lld %d %d %d\n", k_rows[r].key, d, S[r][d].n, (long long)S[r][d].first, S[r][d].best, S[r][d].last, (long long)S[r][d].sum, S[r][d].nt, S[r][d].uq, S[r][d].st);
    if (fclose(f) || rename(t, p)) { remove(t); LOG("【红】刷怪积分牌：写 %s 失败", p); }
}

// 每拍对服务端每只怪都要查一次名单：开局把名单按类号 / 超级独特号建成直查表（同一号取最前那一行，与逐行找的结果一致），每只怪 O(1)
// Every tick each server monster is checked against the list: at game start the list is built into direct lookup tables by class id / superunique id (the first row wins for a given id, matching a row-by-row search), O(1) per monster
enum { KLUT_CLS = 1024, KLUT_SU = 256 };
static int8_t k_cls_row[KLUT_CLS], k_su_row[KLUT_SU], k_su_grp[KLUT_SU];   // 存行号 + 1 / 组号 × 3 + 成员 + 1；0 = 不在名单 | Stores row number + 1 / group number x 3 + member + 1; 0 = not on the list
static void lut_init(void) {
    static int done; if (done) return; done = 1;
    for (int r = NROW - 1; r >= 0; r--) {   // 倒着填，前面的行覆盖后面的 = 取最前那一行 | Fill backwards so earlier rows overwrite later ones = take the first matching row
        if (k_rows[r].kind == K_CLS && k_rows[r].id >= 0 && k_rows[r].id < KLUT_CLS) k_cls_row[k_rows[r].id] = (int8_t)(r + 1);
        int su = k_rows[r].kind == K_SU ? k_rows[r].id : k_rows[r].kind == K_CLS ? k_rows[r].id2 : -1;
        if (su >= 0 && su < KLUT_SU) k_su_row[su] = (int8_t)(r + 1); }
    for (int g = 1; g >= 0; g--) for (int m = 2; m >= 0; m--) k_su_grp[k_grp_su[g][m]] = (int8_t)(g * 3 + m + 1);
}
static int row_of(uint64_t u, int *grp, int *gm) {   // 名单行号；一组的成员给 grp / gm（行号是组那一行）；不在名单 = -1 | Row number in the list; for a group member, gives grp / gm (the row number is the group's row); not in the list = -1
    *grp = -1; int cls = (int)*(uint32_t *)(uintptr_t)(u + 4);
    if (cls >= 0 && cls < KLUT_CLS && k_cls_row[cls]) return k_cls_row[cls] - 1;
    uint64_t md = rd(u + 0x10); if (!md || !(*(uint8_t *)(uintptr_t)(md + 0x1a) & 2)) return -1;
    int su = *(uint16_t *)(uintptr_t)(md + 0x2a); if (su >= KLUT_SU) return -1;
    if (k_su_row[su]) return k_su_row[su] - 1;
    if (k_su_grp[su]) { int g = (k_su_grp[su] - 1) / 3; *grp = g; *gm = (k_su_grp[su] - 1) % 3; for (int r = 0; r < NROW; r++) if (k_rows[r].kind == K_GRP && k_rows[r].id == g) return r; }
    return -1;
}
static int client_has(uint32_t id) {   // 客户端单位表（main+0x2d603a0 + 类型 1 × 0x400，128 桶按 id 低 7 位，下一个 +0x158）里有这只 = 玩家这边已出现。只读、链长封顶（同首领掉落读数） | Present in the client unit table (main+0x2d603a0 + type 1 × 0x400, 128 buckets by the low 7 bits of id, next +0x158) = already appeared on the player's side. Read-only, chain length capped (same as the boss drop readout)
    int k = 0; for (uint64_t c = rd((uint64_t)(uintptr_t)(B + 0x2d603a0 + 1 * 0x400 + (id & 0x7f) * 8)); c && k < 64; c = rd(c + 0x158), k++) if (*(uint32_t *)(uintptr_t)(c + 8) == id) return 1;
    return 0;
}
static void counted(int r, int d, uint32_t eng, uint32_t t) {
    S[r][d].n++; if (S[r][d].n == 1) S[r][d].first = now_s();
    int dt = eng && t >= eng ? (int)(t - eng) : 0; if (dt > 0 && (!S[r][d].best || dt < S[r][d].best)) S[r][d].best = dt;
    if (dt > 0) { S[r][d].last = dt; S[r][d].sum += dt; S[r][d].nt++; }   // 最近 / 平均：只算量到交战用时的那几次（同「最快」的口径） | Latest / average: only count the kills where an engagement time was measured (same basis as 「最快」(Fastest))
    char nm[160], line[200]; row_name(r, nm, sizeof nm); snprintf(line, sizeof line, dm_str(B, "dmKbKilledNth"), nm, S[r][d].n); dm_game_notice(B, line);
    LOG("刷怪积分牌：拍 %u %s 难度 %d 第 %d 次（交战 %d 拍，最快 %d 拍）", t, k_rows[r].key, d, S[r][d].n, dt, S[r][d].best); save();
}

void dm_kills_tick(uint64_t G, uint64_t player, uint8_t *main_base) {
    B = main_base; if (!G || !player || !B) return;
    lut_init();
    uint32_t t = *(volatile uint32_t *)(uintptr_t)(G + 0x170);
    if (G != g_G || t < g_last_t) { g_G = G; nE = 0; nP = 0; g_maxit = 0; memset(GS, 0, sizeof GS); g_name[0] = 0; }   // 新的一局（认法同首领连战：G 变了或拍号倒退） | A new run (identified the same way as boss rush: G changed or the tick number went backwards)
    g_last_t = t;
    if (!g_name[0]) { uint64_t pd = rd(player + 0x10); if (!pd) return; char nm[64]; strlcpy(nm, (const char *)(uintptr_t)pd, sizeof nm); if (!nm[0] || strchr(nm, '/')) return; strlcpy(g_name, nm, sizeof g_name); load(); }
    int d = *(uint8_t *)(uintptr_t)(G + 0x104); if (d > 2) return;
    // 测试开关 DM_TEST_SLAY=<拍>（造现场用，配 DM_TEST_SPAWN 刷名单上的怪、按键脚本去打）：到拍把名单上活着的单位生命设成 1 点（同时算开打），玩家一下就打死
    // Test switch DM_TEST_SLAY=<tick> (for test setup; combine with DM_TEST_SPAWN to spawn listed monsters and a key script to fight them): at that tick, set the life of living listed units to 1 point (also counts as the fight starting) so the player kills them in one hit
    { static int at = -1; if (at < 0) { const char *e = getenv("DM_TEST_SLAY"); at = e ? atoi(e) : 0; }
      if (at > 0 && t >= (uint32_t)at) { at = 0; int nk = 0;
          for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2230 + 1 * 0x400 + 8 * b); u; u = rd(u + 0x158)) { int gg, gm; uint32_t m = *(uint32_t *)(uintptr_t)(u + 0xc);
              if (row_of(u, &gg, &gm) < 0 || m == 0 || m == 12) continue; nk++; ((void (*)(uint64_t, int, int, int))(B + 0x5bc1f0))(u, 6, 256, 0); }
          LOG("测试 DM_TEST_SLAY：拍 %u 名单上 %d 只生命设成 1 点", t, nk); } }
    // 测试开关 DM_TEST_PICK_UQ=<拍>（查编年史登记用）：到拍把玩家 20 格内地上的暗金（品质 7）与套装（品质 5）件用真实捡物品包 0x16 捡进背包（同 diag.c DM_TEST_GIVE_INV），
    // 5 拍后再用服务端鉴定 0x78acf0(游戏, 玩家, 物品)（同免费鉴定）鉴定这几件
    // Test switch DM_TEST_PICK_UQ=<tick> (for checking chronicle registration): at that tick, pick up the unique (quality 7) and set (quality 5) items on the ground within 20 tiles of the player into the inventory using the real pick-up-item packet 0x16 (same as DM_TEST_GIVE_INV in diag.c),
    // then 5 ticks later identify these items with the server-side identify 0x78acf0(game, player, item) (same as free identify)
    { static int at = -1; static uint32_t ids[8]; static int n; if (at < 0) { const char *e = getenv("DM_TEST_PICK_UQ"); at = e ? atoi(e) : 0; }
      if (at > 0 && t == (uint32_t)at) { uint64_t pp = rd(player + 0x38); int px = pp ? *(uint16_t *)(uintptr_t)(pp + 2) : 0, py = pp ? *(uint16_t *)(uintptr_t)(pp + 6) : 0;
          for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2e30 + 8 * b); u; u = rd(u + 0x158)) { uint64_t dd = rd(u + 0x10), ip = rd(u + 0x38);
              if (*(uint32_t *)(uintptr_t)u != 4 || *(uint32_t *)(uintptr_t)(u + 0xc) != 3 || !dd || !ip || (*(uint32_t *)(uintptr_t)dd != 7 && *(uint32_t *)(uintptr_t)dd != 5) || n >= 8) continue;
              if (abs(*(int32_t *)(uintptr_t)(ip + 0x10) - px) > 20 || abs(*(int32_t *)(uintptr_t)(ip + 0x14) - py) > 20) continue;
              uint8_t pk[17] = { 0x16 }; uint32_t one = 1; memcpy(pk + 1, (void *)(uintptr_t)(u + 8), 4); memcpy(pk + 5, (void *)(uintptr_t)(ip + 0x10), 4); memcpy(pk + 9, (void *)(uintptr_t)(ip + 0x14), 4); memcpy(pk + 13, &one, 4);
              ids[n++] = *(uint32_t *)(uintptr_t)(u + 8);
              *(uint32_t *)(uintptr_t)(G + 0x19050) = 1; ((void (*)(uint64_t, uint64_t, uint8_t *, uint32_t))(B + 0x6e5b40))(G, player, pk, 17); *(uint32_t *)(uintptr_t)(G + 0x19050) = 0;
              LOG("测试 DM_TEST_PICK_UQ：拍 %u 捡暗金 / 套装 id %u（类号 %u）→ 模式 %u", t, ids[n - 1], *(uint32_t *)(uintptr_t)(u + 4), *(uint32_t *)(uintptr_t)(u + 0xc)); } }
      if (at > 0 && t == (uint32_t)at + 5) for (int i = 0; i < n; i++) { uint64_t dm_diag_srv_item(uint32_t); uint64_t si = dm_diag_srv_item(ids[i]);
          int r = si ? ((int (*)(uint64_t, uint64_t, uint64_t))(B + 0x78acf0))(G, player, si) : -1; LOG("测试 DM_TEST_PICK_UQ：鉴定 id %u → %d", ids[i], r); } }
    int alive_any = 0;
    for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2230 + 1 * 0x400 + 8 * b); u; u = rd(u + 0x158)) {
        int grp, gm = 0, r = row_of(u, &grp, &gm); if (r < 0) continue;
        uint32_t id = *(uint32_t *)(uintptr_t)(u + 8), m = *(uint32_t *)(uintptr_t)(u + 0xc); int dead = m == 0 || m == 12;
        int e = 0; while (e < nE && E[e].id != id) e++;
        if (e == nE) { if (nE == (int)(sizeof E / sizeof *E)) { memmove(E, E + 1, sizeof E - sizeof *E); nE--; e--; }   // 表满丢最老的 | When the table is full, drop the oldest
            memset(&E[e], 0, sizeof *E); E[e].id = id; E[e].row = (int16_t)r; E[e].grp = (int8_t)grp; E[e].gm = (int8_t)gm; nE++;
            E[e].skip = dead || dm_bossrush_owns(id); E[e].dead = (uint8_t)dead;
            LOG("刷怪积分牌：拍 %u 见到 %s（单位 id %u，模式 %u%s）", t, k_rows[r].key, id, m, E[e].skip ? "，不计" : ""); }
        if (E[e].skip || E[e].dead) continue;
        alive_any = 1;
        if (!dead) {
            int hp = ((int (*)(uint64_t, int, int))(B + 0x5b9290))(u, 6, 0), mx = ((int (*)(uint64_t, int, int))(B + 0x5b9290))(u, 7, 0);
            uint32_t *eng = grp >= 0 ? &GS[grp].eng : &E[e].eng;
            if (hp < mx) { if (!*eng) *eng = t; } else if (grp < 0 || !GS[grp].mask) *eng = 0;   // 回满血重新起算（一组里已有死的就不清） | Back at full life, timing starts over (not cleared if a member of the group is already dead)
            if (!(grp >= 0 ? GS[grp].announced : E[e].announced) && client_has(id)) {
                if (grp >= 0) GS[grp].announced = 1; else E[e].announced = 1;
                char nm[160], line[200]; row_name(r, nm, sizeof nm); snprintf(line, sizeof line, dm_str(B, "dmKbKilledTotal"), nm, S[r][d].n); if (S[r][d].n > 0) dm_game_notice(B, line);   // 一次都没杀过时不报（「已累计击杀 0 次」读着像出错） | Not reported when it has never been killed (「已累计击杀 0 次」(killed 0 times so far) reads like an error)
                LOG("刷怪积分牌：拍 %u 提示出现 %s（难度 %d 已 %d 次）", t, k_rows[r].key, d, S[r][d].n); }
            continue; }
        E[e].dead = 1; if (dm_rift_swept(id)) { E[e].skip = 1; LOG("刷怪积分牌：拍 %u %s（单位 id %u）是秘境结算后清掉的，不计", t, k_rows[r].key, id); continue; }
        if (nP < (int)(sizeof P / sizeof *P)) { uint64_t pa = rd(u + 0x38); P[nP].row = (int16_t)r; P[nP].d = (int8_t)d; P[nP].grp = (int8_t)grp; P[nP].t_die = t; P[nP].max_item = g_maxit;
            P[nP].x = pa ? *(uint16_t *)(uintptr_t)(pa + 2) : 0; P[nP].y = pa ? *(uint16_t *)(uintptr_t)(pa + 6) : 0; nP++; }
        // 测试开关 DM_TEST_KILL_DROP=<物品码>:<品质>[,…]（造现场用：验掉落件数）：名单上的怪死的那一拍，让它按原版「在某单位旁边掉一件」main+0x7c0b20(游戏, 单位, 码, 品质)
        // 再掉这几件（同 diag.c DM_TEST_GIVE，来源换成死掉的首领）。品质 7 暗金、5 套装；期望掉落读数 = 给的件数
        // Test switch DM_TEST_KILL_DROP=<item code>:<quality>[,...] (for test setup: verifying drop counts): on the tick a listed monster dies, have it use the original game's "drop one item next to a unit" main+0x7c0b20(game, unit, code, quality)
        // to drop these items as well (same as DM_TEST_GIVE in diag.c, with the source changed to the dead boss). Quality 7 unique, 5 set; expected drop readout = number of items given
        { const char *e = getenv("DM_TEST_KILL_DROP"); if (e) { char buf[128]; strlcpy(buf, e, sizeof buf);
            for (char *sv, *tk = strtok_r(buf, ",", &sv); tk; tk = strtok_r(NULL, ",", &sv)) { char code[8] = ""; int q = 2; if (sscanf(tk, "%4[^:]:%d", code, &q) < 1) continue;
                uint32_t c = 0x20202020; memcpy(&c, code, strlen(code) > 4 ? 4 : strlen(code)); uint64_t it = ((uint64_t (*)(uint64_t, uint64_t, uint32_t, int))(B + 0x7c0b20))(G, u, c, q);
                LOG("测试 DM_TEST_KILL_DROP：%s 死时补掉「%s」品质 %d → %s", k_rows[r].key, code, q, it ? "成" : "败"); } } }
        if (grp < 0) { counted(r, d, E[e].eng, t); continue; }
        GS[grp].mask |= (uint8_t)(1 << gm); LOG("刷怪积分牌：拍 %u %s 第 %d 只死（%x）", t, k_rows[r].key, gm + 1, GS[grp].mask);
        if (GS[grp].mask == 7) { S[r][d].uq += GS[grp].uq; S[r][d].st += GS[grp].st; GS[grp].uq = GS[grp].st = 0; counted(r, d, GS[grp].eng, t); }
    }
    for (int i = 0; i < nP; i++) { if (t - P[i].t_die < 25) continue;
        int uq = 0, st = 0;
        for (int b = 0; b < 128; b++) for (uint64_t u = rd(G + 0x2e30 + 8 * b); u; u = rd(u + 0x158)) { uint64_t dd = rd(u + 0x10), pa = rd(u + 0x38); uint32_t m = *(uint32_t *)(uintptr_t)(u + 0xc);
            if (*(uint32_t *)(uintptr_t)u != 4 || *(uint32_t *)(uintptr_t)(u + 8) <= P[i].max_item || (m != 3 && m != 5) || !dd || !pa) continue;
            if (abs(*(int32_t *)(uintptr_t)(pa + 0x10) - P[i].x) > 20 || abs(*(int32_t *)(uintptr_t)(pa + 0x14) - P[i].y) > 20) continue;
            uint32_t q = *(uint32_t *)(uintptr_t)dd; uq += q == 7; st += q == 5; }
        int r = P[i].row, dg = P[i].d, g = P[i].grp;
        if (g >= 0 && GS[g].mask != 7) { GS[g].uq += uq; GS[g].st += st; } else { S[r][dg].uq += uq; S[r][dg].st += st; if (uq || st) save(); }
        LOG("刷怪积分牌：拍 %u %s 死后那一批掉落：暗金 %d、套装 %d", t, k_rows[r].key, uq, st);
        P[i--] = P[--nP]; }
    if (alive_any) { uint32_t mx = 0; for (int b = 0; b < 128; b++) for (uint64_t v = rd(G + 0x2e30 + 8 * b); v; v = rd(v + 0x158)) { uint32_t i = *(uint32_t *)(uintptr_t)(v + 8); if (i > mx) mx = i; } g_maxit = mx; }
}

// ---- 编年史第四页签「首领」（布局 touch_layout.py _chronicle_killboard：控件 TouchKillBoard，每行 TouchKB_<键>）。全在游戏主线程
// 页签切换：编年史每次切子页发 ChroniclePanelMessage:ToggleChildPanel:<子页>（打开编年史时先发一次 TouchKillBoard 再发暗金页），这里按最后一次是不是 TouchKillBoard
// 显示 / 隐藏积分牌；在消息回调里当场改，不等下一帧（不闪）。点击：勾选框、五幕、下拉都发 ClosePanel:TouchKB_*（游戏不认，空操作）
// ---- Chronicle fourth tab 「首领」(Bosses) (layout _chronicle_killboard in touch_layout.py: widget TouchKillBoard, each row TouchKB_<key>). All on the game main thread
// Tab switching: every time the chronicle switches sub-page it sends ChroniclePanelMessage:ToggleChildPanel:<sub-page> (when the chronicle opens it first sends TouchKillBoard once, then the uniques page); here, depending on whether the last one was TouchKillBoard,
// show / hide the kill board; changed right in the message callback, without waiting for the next frame (no flicker). Clicks: the checkboxes, the five acts and the dropdown all send ClosePanel:TouchKB_* (not recognized by the game, a no-op)
enum { KB_ROW_H = 188 };   // 行高，同 touch_layout.py KB_ROW_H | Row height, same as KB_ROW_H in touch_layout.py
static _Atomic int g_kb_on, g_kb_act, g_kb_dirty = 1;
static uint64_t kb_find(uint64_t w, const char *name, int dep) {   // 按名字找控件：名字 +8、子控件数组 +0x58、个数 +0x60（同 dm_custom.c） | Find a widget by name: name +8, child widget array +0x58, count +0x60 (same as dm_custom.c)
    if (!w || dep > 12) return 0; const char *nm = *(const char **)(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = kb_find(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
static uint8_t *kb_base(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static uint64_t kb_board(void) { uint8_t *b = kb_base(); uint64_t root = b ? *(uint64_t *)(b + 0x2f05458) : 0, cp = root ? dm_ui_top(root, "ChroniclePanel") : 0; uint64_t bd = cp ? kb_find(cp, "TouchKillBoard", 1) : 0; return bd ? kb_find(bd, "TouchKB_Body", 0) : 0; }   // 里层（缺省在屏外） | Inner layer (off-screen by default)
static void kb_show(uint64_t w, int on) { const char *nm = (const char *)*(uint64_t *)(w + 8); if (!nm || strncmp(nm, "Touch", 5)) { LOG("【红】积分牌：要改可见的控件「%s」不是我们的，不写", nm ? nm : "?"); return; } *(uint8_t *)(w + 0x50) = (uint8_t)on; *(uint8_t *)(w + 0x51) = (uint8_t)on; }   // +0x50 可见、+0x51 收点击（同 dm_cursor.c 技能树页签） | +0x50 visible, +0x51 receives clicks (same as the skill tree tabs in dm_cursor.c)
static void kb_rect(uint64_t w, int x, int y, int h);
static void kb_body(uint64_t w, int on) { kb_rect(w, on ? 0 : 99999, 0, -1); }   // 积分牌整页挪进 / 挪出屏幕（布局里缺省在屏外，见 touch_layout.py _kb_board） | Move the whole kill board page on / off screen (off-screen by default in the layout, see _kb_board in touch_layout.py)
void dm_kills_on_uimsg(const char *cat, const char *act, const char *arg) {
    if (!strcmp(cat, "DropdownListWidgetMessage")) { atomic_store(&g_kb_dirty, 1); return; }   // 下拉选了一项：重排（选了哪项由 kb_sort 读） | A dropdown item was selected: re-sort (which item was selected is read by kb_sort)
    if (!strcmp(cat, "ChroniclePanelMessage") && !strcmp(act, "ToggleChildPanel")) { int on = !strcmp(arg, "TouchKillBoard"); atomic_store(&g_kb_on, on); atomic_store(&g_kb_dirty, 1);
        uint64_t w = kb_board(); if (w) kb_body(w, on); return; }
    if (strcmp(cat, "PanelManager") || strcmp(act, "ClosePanel") || strncmp(arg, "TouchKB_", 8)) return;
    if (!strncmp(arg + 8, "Act", 3)) atomic_store(&g_kb_act, atoi(arg + 11));
    atomic_store(&g_kb_dirty, 1);
}
static void kb_text(uint8_t *b, uint64_t w, const char *s) {   // 同 dm_update.c set_text：字没变不写 | Same as set_text in dm_update.c: not written if the text is unchanged
    if (!w || *(uint64_t *)w - (uint64_t)(uintptr_t)b != 0x20ba4c8) return;
    size_t n = strlen(s); if (*(uint64_t *)(w + 0x90) == n && !memcmp(*(const char **)(w + 0x88), s, n)) return;
    ((void *(*)(uint64_t, const char *, uint64_t))(b + 0x136e5c))(w + 0x88, s, n); ((void (*)(uint64_t))(b + 0xadc434))(w);
}
static void kb_rect(uint64_t w, int x, int y, int h) {   // 矩形 +0x70 x / +0x74 y / +0x7c 高；改完照原版补一次虚表 +0x10 重算（同 dm_update.c place） | Rectangle +0x70 x / +0x74 y / +0x7c height; after changing, recompute once via vtable +0x10 as the original game does (same as place in dm_update.c)
    int32_t *r = (int32_t *)(w + 0x70); if (r[0] == x && r[1] == y && (h < 0 || r[3] == h)) return; r[0] = x; r[1] = y; if (h >= 0) r[3] = h; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x10))(w);
}
DM_WIDGET_RED_DEF
static void kb_toggle(uint64_t w, int on, int on_frame) {   // 勾选状态 +0xa88，四个帧号 +0x6e8..，此刻的帧 +0x610（dm_custom.c 差分） | Checked state +0xa88, four frame numbers from +0x6e8.., current frame +0x610 (diffed in dm_custom.c)
    if (!DM_WIDGET_OK(kb_base(), w, DM_VT_TOGGLE, "刷怪积分牌")) return;   // 写勾选框专有字段前核类 | check the class before writing checkbox-only fields
    int32_t f[4]; for (int i = 0; i < 4; i++) f[i] = on ? on_frame : 0;
    if (on_frame == 4) { static const int32_t fr[2][4] = { { 0, 1, 3, 2 }, { 4, 5, 6, -1 } }; memcpy(f, fr[on != 0], sizeof f); }
    *(uint8_t *)(w + 0xa88) = on != 0; memcpy((void *)(w + 0x6e8), f, sizeof f); *(int32_t *)(w + 0x610) = f[0];
}
// 排序下拉选了哪项：下拉控件（DropdownListWidget）不发我们能收的消息（onUpdateMessage 带的是选项号，换成 ClosePanel 会让游戏取空面板名崩），
// 按它显示的字认（与选项键 dmKbSortCount / dmKbSortFast 此刻的本地化串比，换语言照样认得；英文版前是逐字比中文）——下拉控件自带一个文字控件嵌在 +0x718（虚表 = TextBox main+0x20ba4c8，串 +0x88 / 长 +0x90，同 kb_text；宿主选项前后逐字节差分）
// Which item the sort dropdown has selected: the dropdown widget (DropdownListWidget) sends no message we can receive (onUpdateMessage carries the
// option index, and replacing it with ClosePanel makes the game crash fetching an empty panel name),
// so it is identified by the text it shows (compared with the current localized strings of the option keys dmKbSortCount / dmKbSortFast, so it still
// works after a language change; before the English version this compared the Chinese text character by character). The dropdown widget has its own text
// widget embedded at +0x718 (vtable = TextBox main+0x20ba4c8, string +0x88 / length +0x90, same as kb_text; found by a byte-by-byte diff on the host
// before and after choosing an option)
static int kb_sort(uint8_t *b, uint64_t dd) {
    uint64_t t = dd && *(uint64_t *)(dd + 0x718) - (uint64_t)(uintptr_t)b == 0x20ba4c8 ? dd + 0x718 : 0; if (!t) { static int once; if (!once++) LOG("【红】刷怪积分牌：排序下拉里找不到文字控件，按幕顺序"); return 0; }
    const char *s = *(const char **)(t + 0x88); size_t n = *(uint64_t *)(t + 0x90); if (!s || n > 64) return 0;
    const char *c = dm_str(b, "dmKbSortCount"), *f = dm_str(b, "dmKbSortFast");
    return n == strlen(c) && !memcmp(s, c, n) ? 1 : n == strlen(f) && !memcmp(s, f, n) ? 2 : 0;
}
static void mmss(char *o, size_t cap, int ticks) { int s = (ticks + 12) / 25; snprintf(o, cap, "%d:%02d", s / 60, s % 60); }   // 游戏拍 → 分:秒（25 拍 / 秒，四舍五入） | Game ticks -> min:sec (25 ticks / second, rounded)
static void fmt_thousands(char *o, size_t cap, long v) { char t[32]; int n = snprintf(t, sizeof t, "%ld", v), k = 0; size_t j = 0;
    for (int i = 0; i < n && j + 2 < cap; i++) { if (i && (n - i) % 3 == 0) o[j++] = ','; o[j++] = t[i]; k++; } o[j] = 0; (void)k; }
void dm_kills_ui_pump(void) {
    uint8_t *b = kb_base(); uint64_t bw = b ? kb_board() : 0; if (!bw) return;
    // 编年史页签条右端的触屏关闭 X（布局 touch_layout.py _chronicle_touch 的 TouchCloseBox，两种模式共用这份布局）：手柄模式挪到屏外（手柄按 B 关），触屏模式放回布局里的位置
    // The touch close X at the right end of the Chronicle tab bar (TouchCloseBox in layout touch_layout.py _chronicle_touch; both modes share this layout):
    // moved off-screen in controller mode (the controller closes with B), put back at its layout position in touch mode
    { int dm_padmode_current(void); uint64_t root = *(uint64_t *)(b + 0x2f05458), cp = root ? dm_ui_top(root, "ChroniclePanel") : 0, cx = cp ? kb_find(cp, "TouchCloseBox", 1) : 0; if (cx) kb_rect(cx, dm_padmode_current() ? 99999 : -998, 256, -1); }
    int on = atomic_load(&g_kb_on); kb_body(bw, on); if (!on) return;   // 打开编年史时那两条消息比面板挂上界面树早、回调里够不着它：这里每帧对齐一次 | When the chronicle opens, those two messages arrive before the panel is attached to the UI tree, so the callback cannot reach it: align once per frame here
    if (!B) B = b;
    // 手柄模式：原版编年史面板的焦点不认识这一页的控件——十字键上下 = 列表滚动、左右 = 上 / 下一幕、X = 难度筛选轮换（全部 → 普通 → 噩梦 → 地狱）、Y = 排序轮换
    // Controller mode: the original Chronicle panel's focus does not know this page's widgets. D-pad up/down = scroll the list, left/right = previous / next
    // act,
    // X = cycle the difficulty filter (All → Normal → Nightmare → Hell), Y = cycle the sort order
    static int pad_scroll;
    { int dm_padmode_current(void); uint64_t dm_padmode_page_keys(uint64_t);
      if (dm_padmode_current()) { int rift = atomic_load(&g_kb_act) == 5;   // 第六项「秘境」：左右能换到它；它没有难度勾选和排序——X / Y 照旧由我们接管（不漏给原版编年史面板）但不处理，提示条不提 | Sixth item "Rift": left/right can switch to it; it has no difficulty checkboxes or sorting, so X / Y are still taken over by us (not leaked to the original Chronicle panel) but not handled, and the button legend does not mention them
          uint64_t ev = dm_padmode_page_keys(DM_BTN_UP | DM_BTN_DOWN | DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_X | DM_BTN_Y); if (rift) ev &= ~(uint64_t)(DM_BTN_X | DM_BTN_Y);
          if (!rift) { void dm_padmode_legend(int, const char *); dm_padmode_legend(2, dm_str(b, "dmPadLegDiff")); dm_padmode_legend(1, dm_str(b, "dmPadLegSort")); }   // 按键提示走原版底部提示条（dm_padmode.c）：X = 2、Y = 1；上下左右原版本来不提示 | Button hints go through the original bottom button legend (dm_padmode.c): X = 2, Y = 1; the original does not hint up/down/left/right in the first place
          if (ev & (DM_BTN_LEFT | DM_BTN_RIGHT)) { int a = atomic_load(&g_kb_act) + ((ev & DM_BTN_RIGHT) ? 1 : -1); if (a >= 0 && a <= 5) { atomic_store(&g_kb_act, a); atomic_store(&g_kb_dirty, 1); if (a < 5) LOG("刷怪积分牌：手柄换到第 %d 幕", a + 1); else LOG("刷怪积分牌：手柄换到秘境项"); } }
          if (ev & DM_BTN_UP) pad_scroll = 1; else if (ev & DM_BTN_DOWN) pad_scroll = -1;
          if (ev & DM_BTN_X) { uint64_t c[3]; int cur = 0; for (int d = 0; d < 3; d++) { char nm[16]; snprintf(nm, sizeof nm, "TouchKB_D%d", d); c[d] = kb_find(bw, nm, 0); if (dm_widget_is(b, c[d], DM_VT_TOGGLE) && *(uint8_t *)(c[d] + 0xa88)) cur = d + 1; }
              int to = (cur + 1) % 4; for (int d = 0; d < 3; d++) if (c[d]) kb_toggle(c[d], to == d + 1, 4); LOG("刷怪积分牌：手柄难度筛选 → %s", to == 0 ? "全部" : to == 1 ? "普通" : to == 2 ? "噩梦" : "地狱"); }
          if (ev & DM_BTN_Y) { const char *const nm[3] = { dm_str(b, "dmKbSortAct"), dm_str(b, "dmKbSortCount"), dm_str(b, "dmKbSortFast") }; uint64_t dd = kb_find(bw, "TouchKB_Sort", 0); int to = (kb_sort(b, dd) + 1) % 3;
              if (dd) { kb_text(b, dd + 0x718, nm[to]); atomic_store(&g_kb_dirty, 1); LOG("刷怪积分牌：手柄排序 → %s", nm[to]); } } } }
    static dm_wref last_bw; static int frame; if (bw != dm_wref_get(&last_bw)) { dm_wref_set(&last_bw, bw); atomic_store(&g_kb_dirty, 1); }
    int dsel[3], nsel = 0; for (int d = 0; d < 3; d++) { char nm[16]; snprintf(nm, sizeof nm, "TouchKB_D%d", d); uint64_t c = kb_find(bw, nm, 0); dsel[d] = dm_widget_is(b, c, DM_VT_TOGGLE) && *(uint8_t *)(c + 0xa88); nsel += dsel[d]; }
    static int dlast = -1; int dm = dsel[0] | dsel[1] << 1 | dsel[2] << 2; if (dm != dlast) { dlast = dm; atomic_store(&g_kb_dirty, 1); }
    if (!atomic_exchange(&g_kb_dirty, 0) && ++frame % 30) return;   // 计数在服务端拍里变：开着时每半秒重写一次（字没变不写） | Counts change in the server tick: while open, rewrite every half second (not written if the text is unchanged)
    int act = atomic_load(&g_kb_act); if (act < 0 || act > 5) act = 0;   // 5 = 秘境（秘境测试分支） | 5 = rift (rift test branch)
    for (int i = 0; i < 6; i++) { char nm[20]; snprintf(nm, sizeof nm, "TouchKB_Act%d", i); uint64_t a = kb_find(bw, nm, 0); if (a) kb_toggle(a, i == act, 1); }
    int sort = kb_sort(b, kb_find(bw, "TouchKB_Sort", 0));
    // 每行的合计：都不勾 = 三个难度合计；勾了就只算勾上的
    // Per-row total: none checked = total of all three difficulties; if any are checked, count only the checked ones
    struct { int r, n, best, last, uq, st; int64_t first, sum; int nt; } v[NROW]; int nv = 0; long total = 0;
    for (int r = 0; r < NROW; r++) { int n = 0, best = 0; int64_t first = 0;
        int last = 0, uq = 0, st = 0, nt = 0; int64_t sum = 0, lastf = 0;
        for (int d = 0; d < 3; d++) { if (nsel && !dsel[d]) continue; n += S[r][d].n; if (S[r][d].best && (!best || S[r][d].best < best)) best = S[r][d].best; if (S[r][d].first && (!first || S[r][d].first < first)) first = S[r][d].first;
            uq += S[r][d].uq; st += S[r][d].st; sum += S[r][d].sum; nt += S[r][d].nt;
            if (S[r][d].last && S[r][d].first >= lastf) { lastf = S[r][d].first; last = S[r][d].last; } }   // 几个难度合看时「最近」没有各自的时刻：取首杀最晚那个难度的（近似，单难度时准） | When several difficulties are viewed together, 「最近」(Latest) has no time of its own: take it from the difficulty whose first kill is the latest (approximate; exact for a single difficulty)
        total += n; if (k_rows[r].act == act + 1) { v[nv].r = r; v[nv].n = n; v[nv].best = best; v[nv].first = first; v[nv].last = last; v[nv].uq = uq; v[nv].st = st; v[nv].sum = sum; v[nv].nt = nt; nv++; } }
    if (sort) for (int i = 1; i < nv; i++) for (int j = i; j > 0; j--) {   // 排序：1 按次数多到少，2 按最快（没有的排后）；相同的保持名单顺序 | Sort: 1 by count descending, 2 by fastest (those without one go last); ties keep the list order
        int dm = sort == 1 ? v[j].n > v[j - 1].n : (v[j].best && (!v[j - 1].best || v[j].best < v[j - 1].best)); if (!dm) break; typeof(v[0]) t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    uint64_t list = 0;
    for (int i = 0; i < 6; i++) { char nm[20]; snprintf(nm, sizeof nm, "TouchKB_Page%d", i); uint64_t pg = kb_find(bw, nm, 0); if (!pg) continue;
        kb_rect(pg, i == act ? 0 : 99999, 0, -1); if (i == act) list = kb_find(pg, "TouchKB_List", 3); }   // 每幕一页（各自的滚动区），只把选中那幕挪进屏幕 | One page per act (each with its own scroll area); only the selected act is moved on-screen
    if (!list) return;
    if (pad_scroll) {   // 滚动走触屏同一条路：在列表里的一点发滚轮（一行约 3 格）。点位现找：视口上撒格点，取列表命中的那些点的中间一个（不靠写死的几何，各档位通用） | Scrolling takes the same path as touch: send a wheel event at a point inside the list (one row is about 3 notches). The point is found on the spot: scatter grid points over the viewport and take the middle one of those that hit the list (no hard-coded geometry, works for every profile)
        int W = *(int32_t *)(b + 0x211da38), H = *(int32_t *)(b + 0x211da3c), hx[256], hy[256], nh = 0;
        for (int gy = 1; gy < 16 && W > 0 && H > 0; gy++) for (int gx = 1; gx < 16; gx++) { int x = W * gx / 16, y = H * gy / 16;
            if (*(uint8_t *)(list + 0x50) && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)list + 0x48))(list, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32)) & 1)) { hx[nh] = x; hy[nh] = y; nh++; } }
        if (nh) { void dm_ui_ptr(int, float, float, int); float nx = (float)hx[nh / 2] / (float)W, ny = (float)hy[nh / 2] / (float)H; dm_ui_ptr(1, nx, ny, 0); dm_ui_ptr(7, nx, ny, pad_scroll * 360);   /* 先给位置（滚轮事件自己不带坐标，dm_cursor.c dm_ui_ptr），再发滚轮 | Give the position first (a wheel event carries no coordinates of its own, dm_cursor.c dm_ui_ptr), then send the wheel event */ LOG("刷怪积分牌：手柄滚动 %+d（点 %d,%d）", pad_scroll, hx[nh / 2], hy[nh / 2]); }
        else LOG("【红】刷怪积分牌：手柄滚动时没找到列表的落点");
        pad_scroll = 0; }
    { uint64_t ds = kb_find(bw, "TouchKB_DiffStack", 0), so = kb_find(bw, "TouchKB_Sort", 0);   // 难度勾选与排序只对首领有意义：选到秘境时藏起来，切回幕次再显示 | The difficulty checkboxes and sorting only make sense for bosses: hide them when the rift is selected, show them again when switching back to an act
      if (ds) kb_show(ds, act != 5); if (so) kb_show(so, act != 5); }   // 用可见位（挪矩形会让下拉自带的字丢掉） | Use the visible bit (moving the rectangle would lose the dropdown's built-in text)
    if (act == 5) {   // 秘境：两行总览（行样式同首领行）+ 限时内通关过的层（层号高的在上），没记录的层挪出屏 | Rift: two summary rows (same row style as boss rows) + levels cleared within the time limit (higher level numbers on top); levels with no record are moved off-screen
        dm_rift_stats_t r; int have = dm_rift_stats(&r); char t[160];
        int nms = 0; for (int i = 0; i < 10; i++) nms += have && (r.ms >> i & 1);
        uint64_t w = kb_find(list, "TouchKB_RiftSum", 0);
        if (w) { kb_rect(w, 0, 0, -1); kb_text(b, kb_find(w, "Name", 0), dm_str(b, "dmKbRiftName"));
            if (have) snprintf(t, sizeof t, dm_str(b, "dmKbRiftBest"), r.best); else snprintf(t, sizeof t, "%s", dm_str(b, "dmKbRiftNever")); kb_text(b, kb_find(w, "Kills", 0), t);
            if (have) snprintf(t, sizeof t, dm_str(b, "dmKbRiftClears"), r.clears, r.deaths); else t[0] = 0; kb_text(b, kb_find(w, "First", 0), t);
            if (have) snprintf(t, sizeof t, dm_str(b, "dmKbRiftUnlock"), r.unlock); else t[0] = 0; kb_text(b, kb_find(w, "Drops", 0), t); kb_text(b, kb_find(w, "Times", 0), ""); }
        w = kb_find(list, "TouchKB_RiftSum2", 0);
        if (w) { kb_rect(w, have ? 0 : 99999, KB_ROW_H, -1); kb_text(b, kb_find(w, "Name", 0), dm_str(b, "dmKbRiftMs"));
            snprintf(t, sizeof t, "%d / 10", nms); kb_text(b, kb_find(w, "Kills", 0), t);
            { int nx = 0; while (nx < 10 && (r.ms >> nx & 1)) nx++; if (nx < 10) snprintf(t, sizeof t, dm_str(b, "dmKbRiftMsNext"), (nx + 1) * 10); else snprintf(t, sizeof t, "%s", dm_str(b, "dmKbRiftMsAll")); } kb_text(b, kb_find(w, "First", 0), t);   // 「可选到第几层」放在上一行（原「已解锁到第 N 层 · 下次第 N 层」读不通） | 「可选到第几层」(selectable up to which level) goes on the line above (the former 「已解锁到第 N 层 · 下次第 N 层」(Unlocked to level N, next level N) did not read well)
            kb_text(b, kb_find(w, "Drops", 0), ""); kb_text(b, kb_find(w, "Times", 0), ""); }
        int row = 2;
        for (int tier = 100; tier >= 1; tier--) { char nm[24]; snprintf(nm, sizeof nm, "TouchKB_RiftT%d", tier); uint64_t rw = kb_find(list, nm, 0); if (!rw) continue;
            int f = have ? r.fast[tier] : 0; if (!f) { kb_rect(rw, 99999, 0, -1); continue; }
            kb_rect(rw, 0, KB_ROW_H * row++, -1); snprintf(t, sizeof t, dm_str(b, "dmRiftTierN"), tier); kb_text(b, kb_find(rw, "Name", 0), t);
            snprintf(t, sizeof t, dm_str(b, "dmKbRiftFast"), f / 60, f % 60); kb_text(b, kb_find(rw, "Kills", 0), t);
            int two = tier % 10 == 0 && (r.ms >> (tier / 10 - 1) & 1); kb_text(b, kb_find(rw, "First", 0), two ? dm_str(b, "dmKbRiftMsGot") : "");
            kb_rect(kb_find(rw, "Name", 0), 180, two ? 8 : 64, -1); kb_rect(kb_find(rw, "Kills", 0), 760, two ? 8 : 64, -1); }   // 只有一行字的条目垂直居中（行高 188、字高 60） | Entries with only one line of text are vertically centered (row height 188, text height 60)
        if (have) snprintf(t, sizeof t, dm_str(b, "dmKbRiftTotal"), r.best); else t[0] = 0; kb_text(b, kb_find(bw, "TouchKB_Total", 0), t); return; }
    for (int i = 0; i < nv; i++) { int r = v[i].r; char nm[40]; snprintf(nm, sizeof nm, "TouchKB_%s", k_rows[r].key); uint64_t w = kb_find(list, nm, 0); if (!w) continue;
        kb_rect(w, 0, KB_ROW_H * i, -1);
        char t[160]; row_name(r, t, sizeof t); kb_text(b, kb_find(w, "Name", 0), t);
        snprintf(t, sizeof t, dm_str(b, "dmKbKills"), v[i].n); kb_text(b, kb_find(w, "Kills", 0), t);
        if (v[i].first) { time_t tt = (time_t)v[i].first; struct tm lt; localtime_r(&tt, &lt); strftime(t, sizeof t, dm_str(b, "dmKbFirst"), &lt); } else t[0] = 0; kb_text(b, kb_find(w, "First", 0), t);
        if (v[i].uq || v[i].st) snprintf(t, sizeof t, dm_str(b, "dmKbDrops"), v[i].uq, v[i].st); else t[0] = 0; kb_text(b, kb_find(w, "Drops", 0), t);
        if (v[i].best) { char a[16], c[16], e[16]; mmss(a, sizeof a, v[i].best); mmss(c, sizeof c, v[i].last); mmss(e, sizeof e, v[i].nt ? (int)(v[i].sum / v[i].nt) : 0);
            snprintf(t, sizeof t, dm_str(b, "dmKbTimes"), a, c, e); } else t[0] = 0; kb_text(b, kb_find(w, "Times", 0), t); }
    char num[32], t[64]; fmt_thousands(num, sizeof num, total); snprintf(t, sizeof t, dm_str(b, "dmKbTotal"), num); kb_text(b, kb_find(bw, "TouchKB_Total", 0), t);
}
