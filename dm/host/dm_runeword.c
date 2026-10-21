// 符文之语助手（长按匹配的带孔底材打开清单；符文齐且此刻可取用才出「一键合成」，点了直接镶；不弹任何提示框）。
// 地址为 3.3.94012 原映像偏移。全部在游戏主线程（取手柄处每帧 / 界面消息回调）上跑。
//
// 原版事实（读码 + Mac 宿主实测转储）：
//   数据表 = [main+0x2d64a18 + 版本×16]，版本 = 物品 +0x1bd（3 = 资料片）。符文之语表 +0x1880（条数 +0x1888，每条 0xe8）：+0 名字键（"Runeword133"）、
//   +0x44 complete、+0x4a…+0x54 允许类型 itype1–6（i16）、+0x56/+0x58/+0x5a 禁止类型 etype1–3、+0x5c…+0x70 Rune1–6（物品类号，<1 = 没有）、+0xe4 天梯禁止位（位 2 = 非天梯禁止）。
//   原版匹配 main+0x576310(物品, 输出) 拿已镶满的物品对表：品质（数据 +0）≥ 4 不行；物品表行 +0x13a 非 0 不行；先 etype 命中任一个不行，再 itype 命中任一个才行；
//   类型判定都用 main+0x574bc0(物品, 类型)。这里对空孔底材按同一套规则挑候选（孔数 = 符文个数），成不成型仍由服务端镶最后一颗时的原版 0x64d080 判。
//   物品表 = 数据表 +0x15c0（行数 +0x15c8，每行 0x1c0），+0x80 物品码。物品单位：+4 类号、+8 id、+0xc 模式（0 存放 / 1 穿着 / 2 腰带 / 4 光标上 / 6 镶在孔里）、
//   +0x10 数据（+0 品质、+0x55 页：0 背包 / 3 魔盒 / 4 储藏箱（+0x5c 不是页）、+0x9c 高级仓库叠数、+0xb0 下一件）、+0x38 位置（+0x10 x、+0x14 y）、+0x90 物品自己的容器（+0x10 第一件 = 孔里第一颗）。
//   孔数 = 属性 194（main+0x5b9290(物品, 0xc2, 0)）。客户端玩家 = main+0x45f9d0([main+0x2d20e4c])，其 +0x90 容器 +0x10 起是全部物品（含储藏箱、魔盒）。箱子开着 = 字节 [main+0x2d5ff60]。
//   取本地化串 main+0x7f5630(&{键, 长度})（普通 / 旧译名两套由游戏按设置选）。客户端发包 main+0x2ceab0(包, 长度)。
//   镶嵌包照真镶嵌局（fm_ipadfull_k）的客户端包：0x19 [物品 id][u32 x][u32 y][u32 页]（17 字节，拿到光标）→ 0x28 [符文 id][底材 id][u32 装备位 0][u32 页][u16 x][u16 y]（21 字节，镶进去）。
// Runeword helper (long-pressing a matching socketed base item opens the list; 「一键合成」(Craft) is shown only when the runes are complete and accessible right now, and tapping it sockets directly; no prompt box of any kind pops up).
// Addresses are offsets in the 3.3.94012 original image. Everything runs on the game main thread (every frame at the controller poll / UI message callbacks).
// Original game facts (code reading + dumps measured on the Mac host):
//   Data table = [main+0x2d64a18 + version×16], version = item +0x1bd (3 = expansion). Runeword table +0x1880 (count +0x1888, 0xe8 each): +0 name key ("Runeword133"),
//   +0x44 complete, +0x4a...+0x54 allowed types itype1–6 (i16), +0x56/+0x58/+0x5a excluded types etype1–3, +0x5c...+0x70 Rune1–6 (item class id, <1 = none), +0xe4 ladder restriction bits (bit 2 = forbidden in non-ladder).
//   The original game's match main+0x576310(item, out) checks a fully socketed item against the table: quality (data +0) ≥ 4 fails; item table row +0x13a nonzero fails; first, hitting any etype fails, then hitting any itype passes;
//   all type checks use main+0x574bc0(item, type). Here candidates for an empty-socket base item are picked by the same rules (socket count = number of runes); whether it forms is still decided by the original game's 0x64d080 when the server sockets the last rune.
//   Item table = data table +0x15c0 (row count +0x15c8, 0x1c0 per row), +0x80 item code. Item unit: +4 class id, +8 id, +0xc mode (0 stored / 1 equipped / 2 belt / 4 on cursor / 6 in a socket),
//   +0x10 data (+0 quality, +0x55 page: 0 inventory / 3 cube / 4 stash (+0x5c is not the page), +0x9c advanced stash stack count, +0xb0 next item), +0x38 position (+0x10 x, +0x14 y), +0x90 the item's own container (+0x10 first item = first rune in the sockets).
//   Socket count = stat 194 (main+0x5b9290(item, 0xc2, 0)). Client player = main+0x45f9d0([main+0x2d20e4c]); its +0x90 container, from +0x10, holds all items (including stash and cube). Stash open = byte [main+0x2d5ff60].
//   Get localized string main+0x7f5630(&{key, length}) (the game picks between the normal / legacy translation sets by setting). Client send packet main+0x2ceab0(packet, length).
//   Socket packets follow the client packets of a real socketing run (fm_ipadfull_k): 0x19 [item id][u32 x][u32 y][u32 page] (17 bytes, pick up to the cursor) → 0x28 [rune id][base id][u32 equipment slot 0][u32 page][u16 x][u16 y] (21 bytes, socket it).
#include "hle.h"
#include "dm_rwlv_core.h"
#include "dm_i18n.h"
#include "dm_actions.h"
#include "dm_cursor.h"
#include "dm_input.h"
#include "dm_cursor_item.h"
#include "dm_textbox_core.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NROW 15                       // 清单最多几行（3.3 表里一种底材最多匹配 15 个：3 孔身体护甲 / 斗篷） | Maximum rows in the list (in the 3.3 table one kind of base item matches at most 15: 3-socket body armor / cloak)
enum { PG_INV = 0, PG_CUBE = 3, PG_STASH = 4 };
// 物品所在页 = 物品数据 +0x55（原版客户端拼 0x28 与服务端格子快照都读它，读码）。+0x5c 是拿起时服务端回写的另一个字节，放进储藏箱后仍是 0——
// 原先按 +0x5c 判，储藏箱里的底材被当成背包里的、镶嵌包页号填 0，服务端在背包里找不到底材不收（底材在仓库只把符文取进了背包没镶上）
// Item's page = item data +0x55 (both the original client when building 0x28 and the server grid snapshot read it; from reading the code). +0x5c is another byte the server writes back on pickup, and it is still 0 after the item is put in the stash -
// it used to be judged by +0x5c, so a base item in the stash was taken for one in the inventory, the socket packet's page field was filled with 0, and the server could not find the base item in the inventory and rejected it (with the base item in the stash, the runes were only taken into the inventory and not socketed)
#define PAGE(d) (*(uint8_t *)((d) + 0x55))

static uint8_t *rb(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static uint64_t unit_item(uint8_t *b, uint32_t id) { return ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + 4 * 0x400), id & 0x7f, id, 4); }
static uint64_t player(uint8_t *b) { return ((uint64_t (*)(uint32_t))(b + 0x45f9d0))(*(uint32_t *)(b + 0x2d20e4c)); }
static uint64_t tables(uint8_t *b, uint64_t it) { uint8_t v = *(uint8_t *)(it + 0x1bd); return v >= 1 && v <= 3 ? *(uint64_t *)(b + 0x2d64a18 + v * 16) : 0; }
static int sockets(uint8_t *b, uint64_t it) {   // 孔数 = 属性 194。测试开关 DM_TEST_RW_SOCKETS=<n>：一律当 n 孔（只为看清单排版，白装孔数随机；带这个开关别点一键合成） | Socket count = stat 194. Test switch DM_TEST_RW_SOCKETS=<n>: always treat as n sockets (only for viewing the list layout, since white items get random socket counts; do not tap one-tap crafting with this switch on)
    static int fake = -1; if (fake < 0) { const char *e = getenv("DM_TEST_RW_SOCKETS"); fake = e ? atoi(e) : 0; }
    return fake > 0 ? fake : ((int (*)(uint64_t, int, int))(b + 0x5b9290))(it, 0xc2, 0); }
static int filled(uint64_t it) { uint64_t c = *(uint64_t *)(it + 0x90); return c && *(uint64_t *)(c + 0x10); }
static int nfilled(uint64_t it) { uint64_t c = *(uint64_t *)(it + 0x90); int n = 0; for (uint64_t s = c ? *(uint64_t *)(c + 0x10) : 0; s && n < 8; n++) { uint64_t d = *(uint64_t *)(s + 0x10); s = d ? *(uint64_t *)(d + 0xb0) : 0; } return n; }
static int is_type(uint8_t *b, uint64_t it, int t) { return ((int (*)(uint64_t, int))(b + 0x574bc0))(it, t); }
static const char *loc_str(uint8_t *b, const char *key) { struct { const char *p; uint64_t n; } k = { key, strlen(key) }; const char *s = ((const char *(*)(void *))(b + 0x7f5630))(&k); return s && *s ? s : key; }
// 本地化串的文字框版：物品注释把带颜色码、带点评行的注释版写进了原版字符串键（名字前几行是点评，游戏倒着画），普通文字框不认颜色码。
// 改写规则只有一份：文字框对照表（dm_textbox_core.c，门 14 在文字框写字入口按表改写，注释关着时没有表、串里也没有码）。
// 这里同一张表先换好再用（清单名字也要进日志）；不在表里的原样用
// Text box version of localized strings: item annotations wrote the annotated version, with color codes and comment lines, into the original game's string keys (the lines before the name are comments; the game draws them in reverse), and ordinary text boxes do not understand color codes.
// There is only one copy of the rewrite rule: the text box mapping table (dm_textbox_core.c; gate 14 rewrites by the table at the text box set-text entry; with annotations off there is no table and no codes in the strings).
// Here the same table is applied first, then the result is used (list names also go into the log); strings not in the table are used as is
static void loc_tb(uint8_t *b, const char *key, char *out, size_t n) {
    dm_textbox_ensure_loaded();   // 表由门 14 第一次触发时读；门没挂（DM_TEXTBOX_OFF / rules 产物）或清单比任何文字框先取名字时，这里先读 | The table is read when gate 14 first fires; when the gate is not hooked (DM_TEXTBOX_OFF / rules build output) or the list fetches names before any text box does, read it here first
    const char *s = loc_str(b, key); char *o = NULL; size_t on = 0;
    if (dm_textbox_rewrite(s, strlen(s), &o, &on) && o) { snprintf(out, n, "%s", o); free(o); } else snprintf(out, n, "%s", s);
    { const char *u = dm_untag(out); if (u != out) memmove(out, u, strlen(u) + 1); }   // 串首的语法性别标记（西语）去掉 | Strip the grammatical gender marker at the start of the string (Spanish)
}
#define CC "\xee\x81\xbe"   // 颜色码前缀（游戏串里的 ÿc），后接颜色号：0 白 1 红 4 金 5 灰 | Color code prefix (ÿc in game strings), followed by the color number: 0 white 1 red 4 gold 5 gray
static void send_pkt(uint8_t *b, const uint8_t *p, int n) { ((void (*)(const uint8_t *, int))(b + 0x2ceab0))(p, n); }
static int grid_xy(uint64_t it, int *x, int *y) { uint64_t pp = *(uint64_t *)(it + 0x38); if (!pp) return 0; *x = *(int32_t *)(pp + 0x10); *y = *(int32_t *)(pp + 0x14); return 1; }

// ---------- 符文之语属性说明（原版编年史的生成路径） ----------
// 原版编年史收录符文之语，说明文字由 main+0xafa85c 生成：
//   ① main+0x367890(类型 2 = 符文之语, runes 记录 +0x46 的编号, 1, 1) 造一件临时预览物品（按 itype1 挑底材、按 Rune1–6 镶好），经 x8 返回 {删除器, …, +0x28 物品单位}；
//   ② 属性列表 = runes 记录 +0x74 起 7 条 {prop, param, min, max}（16 字节，prop < 0 止）+ 每颗符文的镶嵌加成（物品表 +0x15c0 行 +0xf8 宝石号 →
//      宝石表 +0x1678 行 +0x30 / +0x60 / +0x90 三组按 main+0x5757a0(物品) 挑武器 / 盔甲 / 盾牌，每组 3 条）；
//   ③ main+0x374f00(玩家, 玩家, 预览物品, 0, 1, 0, 属性列表, &{条数, 1}) 经 x8 返回整段说明（UTF-8，{指针, 长度, 容量}，容量最高位为 0 时要用游戏分配器释放）；
//      范围按原版写法（「(3-5)」，串 ChronicleModifierRange），行序同原版说明框（倒序，文字框用 newlineHandling reversed 显示）；
//   ④ 照 main+0xafaee8–0xafaf74 销毁预览物品。只许游戏主线程调
// ---------- Runeword property description (the original game's chronicle generation path) ----------
// The original game's chronicle records runewords; the description text is generated by main+0xafa85c:
//   ① main+0x367890(type 2 = runeword, the index at runes record +0x46, 1, 1) builds a temporary preview item (base item picked by itype1, socketed per Rune1–6), returning via x8 {deleter, ..., +0x28 item unit};
//   ② property list = 7 entries of {prop, param, min, max} starting at runes record +0x74 (16 bytes, stops at prop < 0) + each rune's socket bonus (item table +0x15c0 row +0xf8 gem id →
//      gem table +0x1678 row +0x30 / +0x60 / +0x90, three groups chosen as weapon / armor / shield by main+0x5757a0(item), 3 entries per group);
//   ③ main+0x374f00(player, player, preview item, 0, 1, 0, property list, &{count, 1}) returns the whole description via x8 (UTF-8, {pointer, length, capacity}; when the top bit of capacity is 0 it must be freed with the game allocator);
//      ranges are written the original game's way ("(3–5)", string ChronicleModifierRange), line order same as the original game's tooltip (reversed; the text box displays it with newlineHandling reversed);
//   ④ destroy the preview item following main+0xafaee8–0xafaf74. May only be called on the game main thread
typedef struct { char *p; uint64_t len, cap; } gstr_t;
typedef struct { uint64_t d[5]; uint64_t item; } preview_t;
static void g_free(uint8_t *b, void *p) { if (!(*(volatile uint8_t *)(b + 0x3211700) & 1)) return; uint64_t a = *(uint64_t *)(b + 0x32116f8); ((void (*)(uint64_t, void *))(*(uint64_t *)(*(uint64_t *)a + 0x28)))(a, p); }
static void preview_drop(uint8_t *b, preview_t *h) {
    uint64_t *obj = (h->d[0] & 1) ? (uint64_t *)(h->d[0] & ~1ull) : (uint64_t *)h;
    if (h->item) { uint64_t it = h->item; ((void (*)(void *, uint64_t *))(*(uint64_t *)(*obj + 0x10)))(obj, &it); h->item = 0; }
    obj = (h->d[0] & 1) ? (uint64_t *)(h->d[0] & ~1ull) : (uint64_t *)h;
    if (obj) { ((void (*)(void *))(*(uint64_t *)(*obj)))(obj); if (h->d[0] & 1) g_free(b, obj); }
    h->d[0] = 1;
}
static int rw_desc(uint8_t *b, int rec, char *out, size_t n) {
    out[0] = 0; uint8_t ver = *(uint8_t *)(b + 0x2127c48); if (ver < 1 || ver > 3) return 0;
    uint64_t t = *(uint64_t *)(b + 0x2d64a18 + ver * 16), pl = player(b); if (!t || !pl) return 0;
    uint64_t rr = *(uint64_t *)(t + 0x1880); uint32_t rn = *(uint32_t *)(t + 0x1888); if (!rr || rec < 0 || (uint32_t)rec >= rn) return 0;
    const uint8_t *e = (const uint8_t *)(rr + (uint64_t)rec * 0xe8);
    preview_t h = ((preview_t (*)(int, int, int, int))(b + 0x367890))(2, *(uint16_t *)(e + 0x46), 1, 1);
    if (!h.item) { preview_drop(b, &h); return 0; }
    uint8_t props[16 * (7 + 18)]; uint64_t np = 0;
    for (int k = 0; k < 7 && *(int16_t *)(e + 0x74 + 16 * k) >= 0; k++) memcpy(props + 16 * np++, e + 0x74 + 16 * k, 16);
    uint64_t ir = *(uint64_t *)(t + 0x15c0), gt = *(uint64_t *)(t + 0x1678), gn = *(uint64_t *)(t + 0x1680); uint32_t in = *(uint32_t *)(t + 0x15c8);
    int grp = ((int (*)(uint64_t))(b + 0x5757a0))(h.item);
    for (int k = 0; k < 6 && grp >= 0 && grp < 3 && ir && gt; k++) { int32_t cls = *(int32_t *)(e + 0x5c + 4 * k); if (cls < 0 || (uint32_t)cls >= in) continue;
        int32_t gi = *(int32_t *)(ir + (uint64_t)cls * 0x1c0 + 0xf8); if (gi < 0 || (uint64_t)gi >= gn) continue;
        memcpy(props + 16 * np, (const void *)(gt + (uint64_t)gi * 0xc0 + 0x30 + 0x30 * grp), 0x30); np += 3; }
    struct { uint64_t n; uint8_t has; } cnt = { np, 1 };
    gstr_t sres = ((gstr_t (*)(uint64_t, uint64_t, uint64_t, int, int, int, void *, void *))(b + 0x374f00))(pl, pl, h.item, 0, 1, 0, props, &cnt);
    size_t k = sres.p && sres.len < n ? sres.len : (sres.p ? n - 1 : 0); if (k) memcpy(out, sres.p, k); out[k] = 0;
    if (sres.p && !(sres.cap >> 63)) g_free(b, sres.p);
    preview_drop(b, &h);
    return (int)k;
}

// ---------- 候选与清点 ----------
// ---------- Candidates and counting ----------
typedef struct { int rec, nr, cls[6], lv, ok, miss, hint; char name[64]; } cand_t;   // hint：符文齐、但底材在共享仓库（原版不能直接镶）→ 按钮那格出灰字 | hint: runes complete, but the base item is in the shared stash (the original game cannot socket it directly) → the button cell shows gray text
typedef struct { int inv, stash, shared, stack, cube; } have_t;
// 玩家能动的全部物品挂在三个单位上：玩家自己（背包 / 魔盒 / 个人储藏箱 / 身上）+ 共享仓库与高级仓库（宝石 / 材料 / 符文页）的两个单位
// main+0x652650 / 0x652780(客户端玩家)（同 dm_actions.c 取高级仓库叠数最多那件的遍历）；容器 +0x90 开头是 0x01020304
// All items the player can move hang on three units: the player itself (inventory / cube / personal stash / equipped) + the two units of the shared stash and the advanced stash (gem / material / rune pages)
// main+0x652650 / 0x652780(client player) (same traversal as dm_actions.c uses to take the largest stack in the advanced stash); the container at +0x90 starts with 0x01020304
static uint32_t shared_unit_id(uint8_t *b); static uint64_t unit_any(uint8_t *b, uint32_t id, int type);
// 共享仓库：按客户端拼 0x46 时的取法找当前页的单位（玩家类单位，mini 实测：id 2、物品就在它身上；main+0x652780 那个在这一局是空的）；找不到再退回 0x652780
// Shared stash: find the current page's unit the way the client does when building 0x46 (a player-class unit; measured on the second Mac: id 2, the items are right on it; the one at main+0x652780 is empty in this run); if not found, fall back to 0x652780
static int owners(uint8_t *b, uint64_t pl, uint64_t us[3]) { us[0] = pl; us[1] = ((uint64_t (*)(uint64_t))(b + 0x652650))(pl); uint32_t sid = shared_unit_id(b); us[2] = sid ? unit_any(b, sid, 0) : 0;
    if (!us[2]) us[2] = ((uint64_t (*)(uint64_t))(b + 0x652780))(pl); if (us[2] == us[1] || us[2] == pl) us[2] = 0; return 3; }
// 共享仓库当前页的单位 id：照客户端拼 0x46 时的取法（main+0x3b589c–0x3b5938）——界面状态表 [main+0x2f05450]（{桶数, 桶数组}，按固定键取），
// 键 0x8d4d18a675312bd4 的那一项 +0x10 → +0x10 → u32。没有返回 0
// Unit id of the shared stash's current page: fetched the way the client does when building 0x46 (main+0x3b589c-0x3b5938) - UI state table [main+0x2f05450] ({bucket count, bucket array}, looked up by a fixed key),
// the entry with key 0x8d4d18a675312bd4, +0x10 → +0x10 → u32. Returns 0 if absent
static uint32_t shared_unit_id(uint8_t *b) {
    uint64_t m = *(uint64_t *)(b + 0x2f05450), bk = m ? *(uint64_t *)(m + 8) : 0, n = m ? *(uint64_t *)m : 0; if (!bk || !n) return 0;
    const uint64_t key = 0x8d4d18a675312bd4ull; uint64_t node = *(uint64_t *)(bk + 8 * ((n - 1) & 0x1f864eb9001295cdull));
    for (int g = 0; node && g < 64; g++, node = *(uint64_t *)node) if (*(uint64_t *)(node + 8) == key) { uint64_t a1 = *(uint64_t *)(node + 0x10), a2 = a1 ? *(uint64_t *)(a1 + 0x10) : 0; return a2 ? *(uint32_t *)a2 : 0; }
    return 0;
}
static uint64_t unit_any(uint8_t *b, uint32_t id, int type) { return ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0 + type * 0x400), id & 0x7f, id, (uint32_t)type); }
static uint64_t first_item(uint64_t u) { uint64_t c = u ? *(uint64_t *)(u + 0x90) : 0; return c && *(uint32_t *)c == 0x01020304 ? *(uint64_t *)(c + 0x10) : 0; }
static struct { uint32_t base; int n; cand_t c[NROW]; int open; } g_list;   // 打开着的清单 | The currently open list
static int rune_lv(uint8_t *b, uint64_t t, int cls, int lvoff) { uint64_t ir = *(uint64_t *)(t + 0x15c0); uint32_t in = *(uint32_t *)(t + 0x15c8); return ir && cls >= 0 && (uint32_t)cls < in && lvoff >= 0 ? *(uint8_t *)(ir + (uint64_t)cls * 0x1c0 + lvoff) : 0; }
static void rune_code(uint64_t t, int cls, char out[4]) { uint64_t ir = *(uint64_t *)(t + 0x15c0); uint32_t in = *(uint32_t *)(t + 0x15c8); memset(out, 0, 4); if (ir && cls >= 0 && (uint32_t)cls < in) memcpy(out, (const void *)(ir + (uint64_t)cls * 0x1c0 + 0x80), 3); }
static int g_lvoff = -1;   // 物品表行里「需求等级」（levelreq）的偏移：首次用到时按已知物品的原版值在行里找（dm_rwlv_core.c），找不到就不显示等级 | offset of "required level" (levelreq) in an item-table row: found on first use by the original values of known items (dm_rwlv_core.c); when not found no level is shown

// 这件物品能当符文之语底材吗（品质、孔、物品表标志；位置另判）
// Can this item serve as a runeword base item (quality, sockets, item table flag; location is checked separately)
static int base_ok(uint8_t *b, uint64_t it, uint64_t t) {
    uint64_t d = *(uint64_t *)(it + 0x10); if (!d || *(uint32_t *)it != 4) return 0;
    if (*(uint32_t *)d >= 4) return 0;                                         // 魔法 / 套装 / 稀有 / 暗金 / 手工 / 淬炼（0x576310 同判） | Magic / set / rare / unique / crafted / tempered (same check as 0x576310)
    uint64_t ir = *(uint64_t *)(t + 0x15c0); uint32_t cls = *(uint32_t *)(it + 4); if (!ir || cls >= *(uint32_t *)(t + 0x15c8) || *(uint8_t *)(ir + (uint64_t)cls * 0x1c0 + 0x13a)) return 0;
    return sockets(b, it) > 0 && !filled(it);
}
static void count_have(uint8_t *b, uint64_t pl, int cls, have_t *h) {
    memset(h, 0, sizeof *h); uint64_t us[3]; owners(b, pl, us);
    for (int o = 0; o < 3; o++) for (uint64_t it = first_item(us[o]); it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        if ((int)*(uint32_t *)(it + 4) == cls && *(uint32_t *)(it + 0xc) == 0) { int pg = PAGE(d), st = *(uint8_t *)(d + 0x9c);
            if (st) h->stack += st; else if (o) h->shared++; else if (pg == PG_INV) h->inv++; else if (pg == PG_CUBE) h->cube++; else h->stash++; }
        it = *(uint64_t *)(d + 0xb0); }
}
static int find_lvoff(uint64_t t) {   // 锚点与找法见 dm_rwlv_core.c；找不到（或不唯一）返回 -1，清单不显示等级 | anchors and the search are in dm_rwlv_core.c; returns -1 when not found (or not unique), and the list then shows no level
    uint64_t ir = *(uint64_t *)(t + 0x15c0); uint32_t in = *(uint32_t *)(t + 0x15c8); int o = ir ? dm_rw_find_lvoff((const uint8_t *)ir, in, 0x1c0) : -1;
    // 测试开关 DM_TEST_RW_LVDUMP=<文件>：把整张物品表逐行写成「物品码<TAB>读到的需求等级」，给 dm/ios/tools/rw_lvcheck.py 与原版 weapons / armor / misc 三张表全量核
    // Test switch DM_TEST_RW_LVDUMP=<file>: write the whole item table line by line as "item code<TAB>required level read", for dm/ios/tools/rw_lvcheck.py to check in full against the original weapons / armor / misc tables
    const char *e = getenv("DM_TEST_RW_LVDUMP"); FILE *f = e && o >= 0 ? fopen(e, "w") : NULL;
    if (f) { for (uint32_t i = 0; i < in; i++) { const uint8_t *r = (const uint8_t *)(ir + (uint64_t)i * 0x1c0); char c[5] = {0}; memcpy(c, r + 0x80, 4); for (int k = 0; k < 4; k++) if (c[k] == ' ') c[k] = 0; fprintf(f, "%s\t%u\n", c, r[o]); } fclose(f); LOG("符文之语：DM_TEST_RW_LVDUMP 写了 %u 行 → %s", in, e); }
    return o;
}
static int cand_cmp(const void *a, const void *b2) { const cand_t *x = a, *y = b2; if (x->ok != y->ok) return y->ok - x->ok; if (x->miss != y->miss) return x->miss - y->miss; return x->lv - y->lv; }
// 列出这件底材能做的全部符文之语，并按此刻能取用的符文定 ok（能出按钮）
// List every runeword this base item can make, and set ok (button shown) according to the runes accessible right now
static int base_where(uint8_t *b, uint64_t pl, uint64_t it, int *page, int *x, int *y, int *slot);
static int build_list(uint8_t *b, uint64_t it) {
    uint64_t t = tables(b, it), pl = player(b); if (!t || !pl || !base_ok(b, it, t)) return 0;
    if (g_lvoff == -1) { g_lvoff = find_lvoff(t); if (g_lvoff < 0) LOG("【红】符文之语：物品表里没找到「需求等级」那一列（锚点物品对不上），清单不显示等级"); else LOG("符文之语：物品表「需求等级」偏移 %#x", g_lvoff); if (g_lvoff < 0) g_lvoff = -2; }
    int ns = sockets(b, it), stash_open = *(uint8_t *)(b + 0x2d5ff60) != 0, bp = 0, bx = 0, by = 0, bsl = 0, bw = base_where(b, pl, it, &bp, &bx, &by, &bsl);
    int can_put = bw && !(bw == 1 && bp == PG_STASH && !stash_open);   // 底材在能镶的位置（储藏箱里的要箱子开着）；共享仓库里的照列、不出按钮 | Base item is in a socketable location (one in the stash needs the stash open); one in the shared stash is still listed, without a button
    uint64_t rr = *(uint64_t *)(t + 0x1880), rn = *(uint64_t *)(t + 0x1888); g_list.n = 0;
    for (uint64_t r = 0; rr && r < rn && g_list.n < NROW; r++) { const uint8_t *e = (const uint8_t *)(rr + r * 0xe8);
        if (!e[0x44] || (e[0xe4] & 2)) continue;
        int nr = 0, cls[6]; for (int k = 0; k < 6; k++) { int v = *(const int32_t *)(e + 0x5c + 4 * k); if (v >= 1) cls[nr++] = v; }
        if (nr != ns) continue;
        int bad = 0; for (int k = 0; k < 3 && !bad; k++) { int ty = *(const int16_t *)(e + 0x56 + 2 * k); if (ty && is_type(b, it, ty)) bad = 1; }
        int good = 0; for (int k = 0; k < 6 && !good && !bad; k++) { int ty = *(const int16_t *)(e + 0x4a + 2 * k); if (ty && is_type(b, it, ty)) good = 1; }
        if (bad || !good) continue;
        cand_t *c = &g_list.c[g_list.n++]; memset(c, 0, sizeof *c); c->rec = (int)r; c->nr = nr; memcpy(c->cls, cls, sizeof cls); c->ok = can_put; int runes_ok = 1;
        int base_req = rune_lv(b, t, *(int32_t *)(it + 4), g_lvoff), rreq[6];   // 需要等级 = 底材与各符文需求等级的最大值（原版做成以后的说明同此；物品单位 +4 = 物品类号） | required level = the maximum of the required levels of the base item and each rune (the same as the original's description once made; item unit +4 = item class number)
        char key[64]; memcpy(key, e, 63); key[63] = 0; loc_tb(b, key, c->name, sizeof c->name);
        for (int k = 0; k < nr; k++) rreq[k] = rune_lv(b, t, cls[k], g_lvoff);
        c->lv = dm_rw_need_level(base_req, rreq, nr);
        for (int k = 0; k < nr; k++) {
            int need = 0; for (int j = 0; j < nr; j++) need += cls[j] == cls[k]; have_t h; count_have(b, pl, cls[k], &h);
            int usable = h.inv + (stash_open ? h.stash + h.shared + h.stack : 0), total = h.inv + h.stash + h.shared + h.stack + h.cube;
            if (usable < need) { c->ok = 0; runes_ok = 0; }
            if (total < need) { int dup = 0; for (int j = 0; j < k; j++) dup |= cls[j] == cls[k]; if (!dup) c->miss += need - total; } }
        c->hint = runes_ok && !can_put && !bw; }   // 符文都在手边、只是底材在共享仓库：按钮那格出灰字「先移到个人仓库或背包」 | Runes are all at hand, only the base item is in the shared stash: the button cell shows the gray text 「先移到个人仓库或背包」(Move out of shared stash first)
    qsort(g_list.c, g_list.n, sizeof *g_list.c, cand_cmp);
    return g_list.n;
}

// ---------- 界面：物品栏面板里的清单控件（布局 touch_layout.py _RW；名字 TouchRw*） ----------
// ---------- UI: list widgets in the inventory panel (layout touch_layout.py _RW; names TouchRw*) ----------
static uint64_t find_w(uint64_t w, const char *name, int dep) {   // 按名字找控件：名字 +8、子控件 +0x58、个数 +0x60（同 dm_update.c） | Find a widget by name: name +8, children +0x58, count +0x60 (same as dm_update.c)
    if (!w || dep > 6) return 0; const char *nm = *(const char **)(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 512) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = find_w(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
static void set_text(uint8_t *b, uint64_t w, const char *s) {   // 同 dm_update.c：TextBoxWidget（虚表 main+0x20ba4c8）字符串赋值 0x136e5c + 刷新 0xadc434 | Same as dm_update.c: TextBoxWidget (vtable main+0x20ba4c8) string assign 0x136e5c + refresh 0xadc434
    if (!w || *(uint64_t *)w - (uint64_t)(uintptr_t)b != 0x20ba4c8) return;
    size_t n = strlen(s); if (*(uint64_t *)(w + 0x90) == n && !memcmp(*(const char **)(w + 0x88), s, n)) return;
    ((void *(*)(uint64_t, const char *, uint64_t))(b + 0x136e5c))(w + 0x88, s, n); ((void (*)(uint64_t))(b + 0xadc434))(w);
}
static void place(uint64_t w, int x, int y) { if (!w) return; int32_t *r = (int32_t *)(w + 0x70); if (r[0] == x && r[1] == y) return; r[0] = x; r[1] = y; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x10))(w); }
#define OFF 99999
// 面板坐标：物品栏面板（PlayerInventoryExpansionLayout）里 1 单位 ≈ 0.7 屏幕像素（按项链格、背包格反推），面板宽约 1408、高约 1486。
// 清单几何（物品栏面板坐标，面板 1408x1486）：底板 x 262、宽 1125，从 y 60 往下；标题占 RW_HEAD，每个符文之语名字行占 RW_L1、符文串每行 RW_LH、行间留 RW_RGAP；底板最低到 RW_YMAX。
// 一页放不下时分页（不截掉超长的部分，改成像仓库页一样换页）：底部一排翻页控件占 RW_NAV_H，照搬仓库页（左右箭头 + 中间「第 x/y 页」）
// Panel coordinates: in the inventory panel (PlayerInventoryExpansionLayout) 1 unit ≈ 0.7 screen pixels (derived back from the amulet slot and inventory cells); the panel is about 1408 wide and 1486 high.
// List geometry (inventory panel coordinates, panel 1408x1486): backplate x 262, width 1125, going down from y 60; the title takes RW_HEAD, each runeword name line takes RW_L1, each rune string line RW_LH, with RW_RGAP between rows; the backplate goes down to RW_YMAX at most.
// When one page is not enough, paginate (do not cut off the overflow; switch pages like the stash page instead): a row of paging widgets at the bottom takes RW_NAV_H, copied from the stash page (left / right arrows + 「第 x/y 页」(Page x/y) in the middle)
enum { RW_X = 262, RW_W = 1125, RW_Y0 = 60, RW_HEAD = 80, RW_L1 = 56, RW_LH = 48, RW_RGAP = 28, RW_PAD = 36, RW_GAP = 22, RW_NAV_H = 80, RW_PG_W = 230, RW_YMAX = 1470, RW_PER_PAGE = 8 };   // 每页最多 8 个，每两个之间一条分隔线 | At most 8 per page, with one separator between every two
static int g_inv_open;   // 物品栏面板开着：按界面消息 OpenPanel / ClosePanel PlayerInventoryExpansionLayout 记（面板控件的可见位在没打开时也可能是 1，真机实测靠不住） | Inventory panel open: tracked by the UI messages OpenPanel / ClosePanel PlayerInventoryExpansionLayout (the panel widget's visible bit can also be 1 when it is not open; measured on a real device, unreliable)
static uint64_t inv_panel(uint8_t *b) { if (!g_inv_open) return 0; uint64_t root = *(uint64_t *)(b + 0x2f05458); return root ? find_w(root, "PlayerInventoryExpansionLayout", 0) : 0; }
// 打开时把控件指针缓存下来（面板活着期间不变），每帧重排只读宽度、写位置。整组带界面代号 ui_gen（dm_cursor.h）：代号变了且物品栏面板已不在界面树上 = 重建过，整组作废，
// 清单当作已关、不碰旧控件（w_stale；切布局 / 切语言 / 面板卸载都会重建，旧控件已释放）
// Cache the widget pointers on open (unchanged while the panel is alive); each per-frame re-layout only reads widths and writes positions. The whole group carries the UI generation ui_gen (dm_cursor.h): generation changed and the inventory panel no longer in the UI tree = rebuilt, the whole group is invalidated,
// the list is treated as closed and old widgets are not touched (w_stale; layout switch / language switch / panel unload all rebuild, and old widgets are freed)
static struct { uint64_t panel, bg, title, prev, next, pg, sep[RW_PER_PAGE - 1], desc, descs, back, backtxt, dgo, sel, name[NROW], lv[NROW], rw[NROW][6], rr[NROW][6], go[NROW], hint[NROW];
    int page, pages, shown_pg, shown_pages, first, last, bot, det, psel, rowy[NROW], red[NROW][6], rx[NROW][6], ry[NROW][6], rh[NROW], pstart[NROW + 1]; uint8_t *b; uint32_t ui_gen; } W;   // 当前页（从 0 起）显示第 first..last-1 个 | The current page (counting from 0) shows entries first..last-1
static int g_check_at;   // 打开后第几帧做排版自检（等 fitToText 量完） | Which frame after opening to run the layout self-check (wait for fitToText to finish measuring)
static int text_w(uint64_t w) { int x = w ? *(int32_t *)(w + 0x78) : 0; return x > 0 && x < 2000 ? x : 0; }   // fitToText 每帧把量出的字宽写回 +0x78（dm_update.c 同法） | fitToText writes the measured text width back to +0x78 every frame (same approach as dm_update.c)
static int text_h(uint64_t w) { return w ? *(int32_t *)(w + 0x7c) : 0; }
// 按钮实际大小 = 矩形宽高（+0x78 / +0x7c，游戏按按钮图填的 470x118）× 缩放（+0x80 浮点，布局里的 0.75）；真机截图量得 352x88 对得上。
// 布局写的 width 408 不作数（第一版按 408×0.75 = 306 摆，按钮右端伸出底板约 45 单位，自检也没抓到）
// Actual button size = rectangle width and height (+0x78 / +0x7c, 470x118 filled in by the game from the button image) × scale (+0x80 float, 0.75 in the layout); 352x88 measured on real device screenshots matches.
// The width 408 written in the layout does not count (the first version placed it by 408×0.75 = 306, the button's right end stuck out of the backplate by about 45 units, and the self-check did not catch it either)
static int real_w(uint64_t w) { return w ? (int)(*(int32_t *)(w + 0x78) * *(float *)(w + 0x80)) : 0; }
static int real_h(uint64_t w) { return w ? (int)(*(int32_t *)(w + 0x7c) * *(float *)(w + 0x80)) : 0; }
static int btn_w(void) { return real_w(W.go[0]); }
static int btn_h(void) { return real_h(W.go[0]); }
static void ui_cache(uint8_t *b) {
    uint64_t p = inv_panel(b); memset(&W, 0, sizeof W); W.ui_gen = dm_ui_gen(); W.panel = p; if (!p) return; char nm[40];
    W.bg = find_w(p, "TouchRwBg", 0); W.title = find_w(p, "TouchRwTitle", 0); W.prev = find_w(p, "TouchRwPgPrev", 0); W.next = find_w(p, "TouchRwPgNext", 0); W.pg = find_w(p, "TouchRwPage", 0); W.shown_pg = -1;
    for (int k = 0; k < RW_PER_PAGE - 1; k++) { snprintf(nm, sizeof nm, "TouchRwSep%d", k); W.sep[k] = find_w(p, nm, 0); }
    W.desc = find_w(p, "TouchRwDesc", 0); W.descs = find_w(p, "TouchRwDescS", 0); W.back = find_w(p, "TouchRwBack", 0); W.backtxt = find_w(p, "TouchRwBackTxt", 0); W.dgo = find_w(p, "TouchRwDetGo", 0); W.sel = find_w(p, "TouchRwSel", 0); W.det = -1;
    for (int i = 0; i < NROW; i++) { snprintf(nm, sizeof nm, "TouchRwName%d", i); W.name[i] = find_w(p, nm, 0); snprintf(nm, sizeof nm, "TouchRwLv%d", i); W.lv[i] = find_w(p, nm, 0);
        snprintf(nm, sizeof nm, "TouchRwGo%d", i); W.go[i] = find_w(p, nm, 0); snprintf(nm, sizeof nm, "TouchRwHint%d", i); W.hint[i] = find_w(p, nm, 0);
        for (int j = 0; j < 6; j++) { snprintf(nm, sizeof nm, "TouchRwR%d_%dw", i, j); W.rw[i][j] = find_w(p, nm, 0); snprintf(nm, sizeof nm, "TouchRwR%d_%dr", i, j); W.rr[i][j] = find_w(p, nm, 0); } }
}
static void ui_hide_all(void) {
    place(W.bg, OFF, OFF); place(W.title, OFF, OFF); place(W.prev, OFF, OFF); place(W.next, OFF, OFF); place(W.pg, OFF, OFF); for (int k = 0; k < RW_PER_PAGE - 1; k++) place(W.sep[k], OFF, OFF);
    place(W.desc, OFF, OFF); place(W.descs, OFF, OFF); place(W.back, OFF, OFF); place(W.backtxt, OFF, OFF); place(W.dgo, OFF, OFF); place(W.sel, OFF, OFF);
    for (int i = 0; i < NROW; i++) { place(W.name[i], OFF, OFF); place(W.lv[i], OFF, OFF); place(W.go[i], OFF, OFF); place(W.hint[i], OFF, OFF); for (int j = 0; j < 6; j++) { place(W.rw[i][j], OFF, OFF); place(W.rr[i][j], OFF, OFF); } }
}
static int det_page(void) { return W.det >= 0; }   // 详情开着（页式：清单底板里换成详情页） | Detail view open (paged style: the detail page replaces the list inside the list backplate)
static int box_h(void) { return W.bot - RW_Y0 + (!det_page() && W.pages > 1 ? RW_NAV_H : 0) + 10; }
// 页码字：原版仓库页的串 SharedStashPage（「第%u/%u页」），两个 %u 换成页号 / 页数
// Page number text: the original game's stash page string SharedStashPage (「第%u/%u页」(Page %u/%u)), with the two %u replaced by page number / page count
static void set_page_text(void) {
    if (!W.b || !W.pg || (W.shown_pg == W.page && W.shown_pages == W.pages)) return;
    char f[64], s[96]; loc_tb(W.b, "SharedStashPage", f, sizeof f); int v[2] = { W.page + 1, W.pages }, k = 0; size_t o = 0;
    for (const char *q = f; *q && o + 12 < sizeof s; ) { if (q[0] == '%' && q[1] == 'u' && k < 2) { o += snprintf(s + o, sizeof s - o, "%d", v[k++]); q += 2; } else s[o++] = *q++; }
    s[o] = 0; set_text(W.b, W.pg, s); W.shown_pg = W.page; W.shown_pages = W.pages;
}
// 按游戏量出的字宽摆位置，清单开着期间每帧重排（第一帧字宽还没量出来时按 0 摆，下一帧就对了）：
// 符文串一枚一个框往右排，排不下（可做的行还要让出按钮那一列）就折到下一行；每行高度按折出的行数算。
// 分页：全部放得下就一页、不出翻页控件；放不下就留出底部翻页那一排，从上往下按页装满（排序已是能做的在前，能做的落在前几页）
// Positions are laid out by the glyph widths the game measures, re-laid out every frame while the list is open (on the first frame, before widths are measured, they are placed as 0; the next frame is correct):
// rune strings are laid out to the right one box per rune, wrapping to the next line when they do not fit (craftable rows also leave room for the button column); each row's height is computed from the number of wrapped lines.
// Paging: if everything fits, one page and no paging widgets; if not, reserve the paging row at the bottom and fill pages from top to bottom (sorting already puts craftable ones first, so they land on the first pages)
static void det_layout(void);
static void ui_layout(void) {
    if (!W.panel) return;
    if (det_page()) { place(W.sel, OFF, OFF); det_layout(); return; }
    int x0 = RW_X + RW_PAD + 12, right = RW_X + RW_W - RW_PAD, top = RW_Y0 + RW_HEAD, n = g_list.n < NROW ? g_list.n : NROW, all = 0;
    for (int i = 0; i < n; i++) { cand_t *c = &g_list.c[i]; int lim = right - (c->ok ? btn_w() + RW_GAP : c->hint ? text_w(W.hint[i]) + RW_GAP : 0), x = x0, ly = RW_L1, lines = 1;   // 行内纵坐标先按相对行顶记 | In-row y coordinates are first recorded relative to the row top
        for (int j = 0; j < c->nr; j++) { int w = text_w(W.red[i][j] ? W.rr[i][j] : W.rw[i][j]);
            if (x > x0 && x + w > lim) { x = x0; ly += RW_LH; lines++; }
            W.rx[i][j] = x; W.ry[i][j] = ly; x += w + RW_GAP; }
        int h = RW_L1 + lines * RW_LH; if (c->ok && h < btn_h()) h = btn_h(); W.rh[i] = h + RW_RGAP; all += W.rh[i]; }
    W.pages = 0; W.pstart[0] = 0;
    if (top + all <= RW_YMAX && n <= RW_PER_PAGE) { W.pages = 1; W.pstart[1] = n; }
    else for (int i = 0, y = top; i < n; i++) { if ((y + W.rh[i] > RW_YMAX - RW_NAV_H || i - W.pstart[W.pages] >= RW_PER_PAGE) && i > W.pstart[W.pages]) { W.pstart[++W.pages] = i; y = top; } y += W.rh[i]; if (i == n - 1) W.pstart[++W.pages] = n; }
    if (W.pages < 1) { W.pages = 1; W.pstart[1] = n; }
    if (W.page >= W.pages) W.page = W.pages - 1; if (W.page < 0) W.page = 0;
    W.first = W.pstart[W.page]; W.last = W.pstart[W.page + 1];
    int y = top; for (int i = W.first; i < W.last; i++) y += W.rh[i]; W.bot = y;
    place(W.bg, RW_X, RW_Y0); { int32_t *r = (int32_t *)(W.bg + 0x70); int h = box_h(); if (W.bg && r[3] != h) { r[3] = h; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)W.bg + 0x10))(W.bg); } }
    place(W.title, RW_X, RW_Y0 + 14);
    y = top;
    for (int i = 0; i < NROW; i++) {
        int vis = i >= W.first && i < W.last, ok = vis && g_list.c[i].ok;
        place(W.name[i], vis ? RW_X + RW_PAD : OFF, vis ? y : OFF);
        place(W.lv[i], vis ? RW_X + RW_PAD + text_w(W.name[i]) + 28 : OFF, vis ? y + 4 : OFF);
        place(W.go[i], ok ? right + 12 - btn_w() : OFF, ok ? y + (W.rh[i] - RW_RGAP - btn_h()) / 2 : OFF);
        { int hv = vis && g_list.c[i].hint; place(W.hint[i], hv ? right - text_w(W.hint[i]) : OFF, hv ? y + (W.rh[i] - RW_RGAP - text_h(W.hint[i])) / 2 : OFF); }
        for (int j = 0; j < 6; j++) { int on = vis && j < g_list.c[i].nr; uint64_t a = on && W.red[i][j] ? W.rr[i][j] : W.rw[i][j], o = a == W.rw[i][j] ? W.rr[i][j] : W.rw[i][j];
            place(o, OFF, OFF); place(a, on ? W.rx[i][j] : OFF, on ? y + W.ry[i][j] : OFF); }
        if (vis) { W.rowy[i] = y; y += W.rh[i]; } }
    // 手柄模式的选中框（原版手柄选中框，框住十字键选中的那一行）：触屏模式不出
    // Selection frame in controller mode (the original controller selection frame, framing the row selected with the D-pad): not shown in touch mode
    { int dm_padmode_current(void); int i = W.psel, on = dm_padmode_current() && i >= W.first && i < W.last; if (W.sel) { int32_t *r = (int32_t *)(W.sel + 0x70); int w = RW_W - 2 * RW_PAD + 48, h = W.rh[i < NROW ? i : 0] - RW_RGAP + 28;
        place(W.sel, on ? RW_X + RW_PAD - 24 : OFF, on ? W.rowy[i] - 14 : OFF); if (on && (r[2] != w || r[3] != h)) { r[2] = w; r[3] = h; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)W.sel + 0x10))(W.sel); } } }
    // 分隔线：每两个之间一条，竖直落在行间空当正中（原版暂停菜单列表的分隔线图，布局里调暗）
    // Separators: one between every two entries, vertically centered in the gap between rows (the original game's pause menu list separator image, dimmed in the layout)
    for (int k = 0, yy = top; k < RW_PER_PAGE - 1; k++) { int i = W.first + k, on = i + 1 < W.last; if (on) yy += W.rh[i];
        place(W.sep[k], on ? RW_X + (RW_W - real_w(W.sep[k])) / 2 : OFF, on ? yy - RW_RGAP / 2 - real_h(W.sep[k]) / 2 : OFF); }
    // 翻页一排：同仓库页 StashNavigation 的摆法（箭头 64、页码框 230、各隔 2），整排在底板里居中
    // Paging row: laid out like the stash page's StashNavigation (arrows 64, page number box 230, 2 apart each), the whole row centered in the backplate
    int nav = W.pages > 1, cx = RW_X + RW_W / 2, ny = W.bot + (RW_NAV_H - 64) / 2;
    place(W.pg, nav ? cx - RW_PG_W / 2 : OFF, nav ? ny : OFF);
    place(W.prev, nav ? cx - RW_PG_W / 2 - 2 - real_w(W.prev) : OFF, nav ? ny : OFF);
    place(W.next, nav ? cx + RW_PG_W / 2 + 2 : OFF, nav ? ny : OFF);
    if (nav) set_page_text();
    det_layout();
}
// 详情：清单行 / 分隔线 / 翻页全收起，底板里左上角「← 返回」、中间原版说明文字、能做时下面一颗「一键合成」。没开详情时全收起
// Detail view: list rows / separators / paging are all hidden; inside the backplate, 「← 返回」(← Back) at the top left, the original game's description text in the middle, and a 「一键合成」(Craft) button below when craftable. When the detail view is not open, everything is hidden
static uint64_t det_desc_pick(int room) {   // 32 号放得下（高、宽都算）用 32 号，否则 22 号 | Use size 32 if it fits (both height and width), otherwise size 22
    return text_h(W.desc) > 0 && text_h(W.desc) <= room && text_w(W.desc) <= RW_W - 24 ? W.desc : W.descs; }
static void det_layout(void) {
    if (W.det < 0) { place(W.desc, OFF, OFF); place(W.descs, OFF, OFF); place(W.back, OFF, OFF); place(W.backtxt, OFF, OFF); place(W.dgo, OFF, OFF); return; }
    int ok = g_list.c[W.det].ok, bw = real_w(W.dgo), bh = real_h(W.dgo), room = RW_YMAX - (RW_Y0 + 100) - (ok ? bh + 24 : 0) - 14;
    uint64_t dt = det_desc_pick(room), du = dt == W.desc ? W.descs : W.desc; place(du, OFF, OFF);
    int dw = text_w(dt), dh = text_h(dt);
    for (int i = 0; i < NROW; i++) { place(W.name[i], OFF, OFF); place(W.lv[i], OFF, OFF); place(W.go[i], OFF, OFF); place(W.hint[i], OFF, OFF); for (int j = 0; j < 6; j++) { place(W.rw[i][j], OFF, OFF); place(W.rr[i][j], OFF, OFF); } }
    for (int k = 0; k < RW_PER_PAGE - 1; k++) place(W.sep[k], OFF, OFF);
    place(W.prev, OFF, OFF); place(W.next, OFF, OFF); place(W.pg, OFF, OFF); place(W.title, OFF, OFF);
    place(W.back, RW_X + RW_PAD, RW_Y0 + 12); place(W.backtxt, RW_X + RW_PAD + real_w(W.back) + 8, RW_Y0 + 12);
    int y = RW_Y0 + 12 + 64 + 24; place(dt, RW_X + (RW_W - dw) / 2, y); y += dh;
    if (ok) { y += 24; place(W.dgo, RW_X + (RW_W - bw) / 2, y); y += bh; } else place(W.dgo, OFF, OFF);
    W.bot = y + 14;
    place(W.bg, RW_X, RW_Y0); { int32_t *r = (int32_t *)(W.bg + 0x70); int h = box_h(); if (W.bg && r[3] != h) { r[3] = h; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)W.bg + 0x10))(W.bg); } }
}
// 排版自检：所有挪到屏上的清单控件（底板除外）都在底板内、彼此不重叠（按钮也算），符文串不伸进按钮那一列。结果记一行（真机 / 回归判据）
// 文字按墨迹矩形判：fitToText 量出的高（+0x7c）是整行框，字只占上半截（真机截图逐行量：36 号字框 81、墨迹在框顶下 9 起 44 高；
// 30 号框 69、墨迹 9 起 36 高），拿整行框判会把肉眼不挨着的上下两行判成重叠。取框顶下 1/9 起、高 3/5（两种字号都比实测墨迹略大，偏严）
// Layout self-check: all list widgets moved on screen (except the backplate) are inside the backplate and do not overlap each other (buttons included), and rune strings do not extend into the button column. The result is logged as one line (real device / regression criterion)
// Text is judged by its ink rectangle: the height measured by fitToText (+0x7c) is the full line box, and the glyphs occupy only the upper part (measured line by line on real device screenshots: size 36 box 81, ink starts 9 below the box top and is 44 high;
// size 30 box 69, ink starts at 9 and is 36 high); judging by the full line box would flag two vertically adjacent lines that visibly do not touch as overlapping. Take from 1/9 below the box top, height 3/5 (slightly larger than the measured ink for both font sizes, on the strict side)
static int det_check(int log);
static int ui_check(int log) {
    if (!W.panel) return -1;
    if (W.det >= 0) return det_check(log); int bx = RW_X, by = RW_Y0, bw = RW_W, bh = box_h(), n = 0, out = 0, ovl = 0; struct { int x, y, w, h; const char *nm; } r[NROW * 10 + 8 + RW_PER_PAGE];
    #define ADD(wd, ww, hh, dy) do { if ((wd) && ((int32_t *)((wd) + 0x70))[0] != OFF) { r[n].x = ((int32_t *)((wd) + 0x70))[0]; r[n].y = ((int32_t *)((wd) + 0x70))[1] + (dy); r[n].w = (ww); r[n].h = (hh); r[n].nm = *(const char **)((wd) + 8); n++; } } while (0)
    #define TXT(wd) ADD(wd, text_w(wd), text_h(wd) * 3 / 5, text_h(wd) / 9)
    ADD(W.title, RW_W, 56, 0); ADD(W.pg, RW_PG_W, 64, 0); ADD(W.prev, real_w(W.prev), real_h(W.prev), 0); ADD(W.next, real_w(W.next), real_h(W.next), 0);
    for (int k = 0; k < RW_PER_PAGE - 1; k++) ADD(W.sep[k], real_w(W.sep[k]), real_h(W.sep[k]), 0);
    if (log && W.sep[0]) { int32_t *q = (int32_t *)(W.sep[0] + 0x70); LOG("符文之语：分隔线控件矩形 %d,%d %dx%d 缩放 %.2f", q[0], q[1], q[2], q[3], *(float *)(W.sep[0] + 0x80)); }
    for (int i = 0; i < NROW; i++) { TXT(W.name[i]); TXT(W.lv[i]); ADD(W.go[i], btn_w(), btn_h(), 0); TXT(W.hint[i]);
        for (int j = 0; j < 6; j++) { TXT(W.rw[i][j]); TXT(W.rr[i][j]); } }
    #undef TXT
    #undef ADD
    const char *bad = "";
    for (int a = 0; a < n && log; a++) if (r[a].x < bx || r[a].y < by || r[a].x + r[a].w > bx + bw || r[a].y + r[a].h > by + bh) { LOG("符文之语：排版自检 首个出底板 %s (%d,%d %dx%d)", r[a].nm, r[a].x, r[a].y, r[a].w, r[a].h); break; }
    for (int a = 0; a < n; a++) { if (r[a].x < bx || r[a].y < by || r[a].x + r[a].w > bx + bw || r[a].y + r[a].h > by + bh) { out++; bad = r[a].nm; }
        for (int c = a + 1; c < n; c++) if (r[a].x < r[c].x + r[c].w && r[c].x < r[a].x + r[a].w && r[a].y < r[c].y + r[c].h && r[c].y < r[a].y + r[a].h) {
            if (!ovl++ && log) LOG("符文之语：排版自检 首个重叠 %s (%d,%d %dx%d) × %s (%d,%d %dx%d)", r[a].nm, r[a].x, r[a].y, r[a].w, r[a].h, r[c].nm, r[c].x, r[c].y, r[c].w, r[c].h); if (!*bad) bad = r[a].nm; } }
    if (log) LOG("%s符文之语：排版自检 第 %d/%d 页 %d 个控件，出底板 %d、重叠 %d%s%s（底板 %d,%d %dx%d，面板 %dx%d）", out || ovl ? "【红】" : "", W.page + 1, W.pages, n, out, ovl, *bad ? "，例：" : "", bad, bx, by, bw, bh,
                 ((int32_t *)(W.panel + 0x70))[2], ((int32_t *)(W.panel + 0x70))[3]);
    return out + ovl;
}
// 详情的排版自检：说明文字、「← 返回」、按钮都在底板里，彼此不重叠。说明文字按整个文字框算（多行，不取墨迹）
// Layout self-check for the detail view: description text, 「← 返回」(← Back) and the button are all inside the backplate and do not overlap each other. The description text is measured by its whole text box (multi-line, not by ink)
static int det_check(int log) {
    uint64_t box = W.bg; if (!box) return -1; int32_t *br = (int32_t *)(box + 0x70);
    int bx = br[0], by = br[1], bw = br[2], bh = box_h(), n = 0, out = 0, ovl = 0; struct { int x, y, w, h; const char *nm; } r[7];
    #define ADD(wd, ww, hh) do { if ((wd) && ((int32_t *)((wd) + 0x70))[0] != OFF) { r[n].x = ((int32_t *)((wd) + 0x70))[0]; r[n].y = ((int32_t *)((wd) + 0x70))[1]; r[n].w = (ww); r[n].h = (hh); r[n].nm = *(const char **)((wd) + 8); n++; } } while (0)
    ADD(W.desc, text_w(W.desc), text_h(W.desc)); ADD(W.descs, text_w(W.descs), text_h(W.descs)); ADD(W.dgo, real_w(W.dgo), real_h(W.dgo)); ADD(W.back, real_w(W.back), real_h(W.back)); ADD(W.backtxt, text_w(W.backtxt), text_h(W.backtxt));
    #undef ADD
    const char *bad = "";
    for (int a = 0; a < n; a++) { if (r[a].x < bx || r[a].y < by || r[a].x + r[a].w > bx + bw || r[a].y + r[a].h > by + bh) { out++; bad = r[a].nm; if (log) LOG("符文之语：详情自检 出框 %s (%d,%d %dx%d)", r[a].nm, r[a].x, r[a].y, r[a].w, r[a].h); }
        for (int c = a + 1; c < n; c++) if (r[a].x < r[c].x + r[c].w && r[c].x < r[a].x + r[a].w && r[a].y < r[c].y + r[c].h && r[c].y < r[a].y + r[a].h) { ovl++; if (!*bad) bad = r[a].nm; } }
    if (log) LOG("%s符文之语：详情自检「%s」%d 个控件，出框 %d、重叠 %d%s%s（框 %d,%d %dx%d，说明用 %s 号 %dx%d）", out || ovl ? "【红】" : "", g_list.c[W.det].name, n, out, ovl,
                 *bad ? "，例：" : "", bad, bx, by, bw, bh, ((int32_t *)(W.desc + 0x70))[0] != OFF ? "32" : "22", ((int32_t *)(W.desc + 0x70))[0] != OFF ? text_w(W.desc) : text_w(W.descs), ((int32_t *)(W.desc + 0x70))[0] != OFF ? text_h(W.desc) : text_h(W.descs));
    return out + ovl;
}
// 详情说明文字：原版生成的整段（rw_desc，行序倒排），在符文那一行下面插三行原版串：底材名（游戏物品名）、「需要等级: %d」（ItemStats1p）、「镶孔 (%i)」（Socketable），
// 次序同原版说明框（名字 / 底材 / 需求 / 镶孔 / 属性）。倒排串里「符文行」是以金色码开头、紧跟单引号的那一行
// Detail description text: the whole block generated by the original game (rw_desc, line order reversed), with three original-game strings inserted below the rune line: base item name (the game's item name), 「需要等级: %d」(Required Level: %d) (ItemStats1p), 「镶孔 (%i)」(Socketed (%i)) (Socketable),
// in the same order as the original game's tooltip (name / base item / requirement / sockets / properties). In the reversed string, the "rune line" is the line that starts with the gold color code immediately followed by a single quote
static void det_text(uint8_t *b, int i, char *out, size_t n) {
    static char d[4096]; out[0] = 0; if (!rw_desc(b, g_list.c[i].rec, d, sizeof d)) return;
    uint64_t it = unit_item(b, g_list.base); char bn[96] = "", lv[64] = "", so[64] = "", ins[300];
    if (it) { uint32_t code = ((uint32_t (*)(uint64_t))(b + 0x570260))(it); char ck[5] = {0}; memcpy(ck, &code, 4); for (int k = 3; k >= 0 && ck[k] == ' '; k--) ck[k] = 0; loc_tb(b, ck, bn, sizeof bn); }
    char f[64]; loc_tb(b, "ItemStats1p", f, sizeof f); { char *q = strstr(f, "%d"); if (q && g_list.c[i].lv) { *q = 0; snprintf(lv, sizeof lv, "%s%d%s", f, g_list.c[i].lv, q + 2); } }
    loc_tb(b, "Socketable", f, sizeof f); { char *q = strstr(f, "%i"); if (q && it) { *q = 0; snprintf(so, sizeof so, "%s%d%s", f, sockets(b, it), q + 2); } }
    snprintf(ins, sizeof ins, "\n" CC "0%s%s%s%s%s", so, *lv ? "\n" CC "0" : "", lv, *bn ? "\n" CC "0" : "", bn);   // 倒排：先写镶孔（显示在最下） | Reversed order: write the sockets line first (displayed at the bottom)
    char *rl = strstr(d, "\n" CC "4'"); size_t at = rl ? (size_t)(rl - d) : strlen(d);
    snprintf(out, n, "%.*s%s%s", (int)at, d, ins, d + at);
}
static void det_open(uint8_t *b, int i) {
    static char t[4600]; if (i < 0 || i >= g_list.n || i >= NROW) return;
    det_text(b, i, t, sizeof t); if (!t[0]) { LOG("【红】符文之语：「%s」的原版说明没生成出来，不开详情", g_list.c[i].name); return; }
    W.det = i; set_text(b, W.desc, t); set_text(b, W.descs, t); ui_layout(); g_check_at = 6;
    LOG("符文之语：详情「%s」（%zu 字节）", g_list.c[i].name, strlen(t));
}
static void ui_fill(uint8_t *b, uint64_t it) {
    uint64_t t = tables(b, it), pl = player(b); if (!W.panel || !t || !pl) return; char s[256];
    uint32_t code = ((uint32_t (*)(uint64_t))(b + 0x570260))(it); char ck[5] = {0}; memcpy(ck, &code, 4); for (int k = 3; k >= 0 && ck[k] == ' '; k--) ck[k] = 0;
    char bn[96]; loc_tb(b, ck, bn, sizeof bn); snprintf(s, sizeof s, dm_str(b, "dmRwTitle"), bn, sockets(b, it)); set_text(b, W.title, s);
    W.b = b;   // 分几页、每页哪几个由 ui_layout 按量出的宽高算 | How many pages and which entries go on each page are computed by ui_layout from the measured widths and heights
    for (int i = 0; i < g_list.n && i < NROW; i++) { cand_t *c = &g_list.c[i];
        set_text(b, W.name[i], c->name); if (c->lv) snprintf(s, sizeof s, dm_str(b, "dmRwLevel"), c->lv); else s[0] = 0; set_text(b, W.lv[i], s);
        for (int j = 0; j < c->nr; j++) {
            char rc[4]; rune_code(t, c->cls[j], rc); int num = atoi(rc + 1);
            char rk[8]; snprintf(rk, sizeof rk, "%sL", rc);   // 配方短名键 rNNL：物品注释开着时是「贝#30 」，关着是「贝」；rNN 那个是多行（高号符文最后一行是红叶加的装饰横线，截图里画成长线） | Recipe short-name key rNNL: 「贝#30 」(Ber#30 ) with item annotations on, 「贝」(Ber) with them off; the rNN one is multi-line (for high runes the last line is a decorative horizontal rule added by the Hongye community mod, drawn as a long line in screenshots)
            char rn[64]; loc_tb(b, rk, rn, sizeof rn); char *hs = strchr(rn, '#'); if (hs) *hs = 0; for (int k = (int)strlen(rn) - 1; k >= 0 && rn[k] == ' '; k--) rn[k] = 0;
            have_t h; count_have(b, pl, c->cls[j], &h); int seen = 0; for (int q = 0; q < j; q++) seen += c->cls[q] == c->cls[j];
            int total = h.inv + h.stash + h.shared + h.stack + h.cube; W.red[i][j] = total <= seen;   // 同号要两颗时：有一颗就第一颗白、第二颗红 | When two of the same rune are needed: with only one owned, the first is white and the second red
            snprintf(s, sizeof s, "#%d %s", num, rn); set_text(b, W.rw[i][j], s); set_text(b, W.rr[i][j], s); } }
}
static int w_stale(void) { return W.panel && W.ui_gen != dm_ui_gen() && !dm_ui_gen_revalidate(&W.ui_gen, W.panel); }   // 缓存的是重建前的控件（代号变了、物品栏面板也不在树上了） | The cache holds widgets from before the rebuild (generation changed and the inventory panel is no longer in the tree)
static void ui_show(uint8_t *b, int on) { if (on) { ui_cache(b); ui_layout(); } else { if (!W.panel) ui_cache(b); if (!w_stale()) ui_hide_all(); memset(&W, 0, sizeof W); } }   // 旧控件已释放：不碰，新建出来的清单控件缺省就在屏外 | Old widgets are freed: don't touch them; newly created list widgets are off screen by default
static void list_close(uint8_t *b, const char *why) { if (!g_list.open) return; g_list.open = 0; ui_show(b, 0); LOG("符文之语：清单关闭（%s）", why); }
// 手柄 / 触屏切换（dm_padmode.c，游戏主线程，在让游戏重载布局之前调）：整套界面马上要销毁重建，W 里缓存的控件指针全部作废。
// 清单开着的话这里直接当它关了——只清自己的状态，不碰控件（重建出来的清单控件缺省就在屏外）。不这样做，重建时游戏发的 ClosePanel 会走到 list_close → ui_show，
// 经已销毁控件的虚表调用，崩（Mac 上 iOS 包实跑：触屏开着清单时推手柄摇杆，CRASH lr=list_close）
// Controller / touch switch (dm_padmode.c, game main thread, called before the game is made to reload the layout): the whole UI is about to
// be destroyed and rebuilt, and every widget pointer cached in W becomes invalid.
// If the list is open, it is simply treated as closed here -- only our own state is cleared and no widget is touched (the rebuilt list widgets
// are off screen by default). Otherwise the ClosePanel the game sends during the rebuild would reach list_close -> ui_show,
// make a call through the vtable of a destroyed widget, and crash (real run of the iOS app on Mac: pushing the controller stick while the
// list is open in touch mode, CRASH lr=list_close)
void dm_runeword_ui_rebuild(void) {
    int was = g_list.open; g_list.open = 0; uint8_t *b = W.b; memset(&W, 0, sizeof W); W.b = b; g_check_at = 0;
    if (was) LOG("符文之语：界面要重建（手柄 / 触屏切换）→ 清单当作已关，缓存的控件指针作废");
}

// ---------- 一键合成：每颗符文从它所在的地方直接拿到光标、镶进底材所在的地方（原版玩家的走法，不经背包中转；读码） ----------
// 拿：背包 / 个人储藏箱格 = 0x19 [id][x][y][页]；符文页成叠 = 原版「取 1 件到光标」main+0x39d724（发 0x63 目的地 0）；共享仓库格 = 0x46 [id][仓库单位 id][x | y<<16]。
// 镶：底材在背包 / 魔盒 / 个人储藏箱 = 0x28 [符文][底材][0][页][u16 x][u16 y]；穿在身上主装备位 1–10 = 0x28 [符文][底材][装备位][0xff][u32 装备位]。
// 共享仓库里的底材原版不能镶（服务端只在玩家自己物品的格子快照里按位置找底材），清单照列、不出按钮。
// 原子性：一切能失败的条件在拿第一颗之前核完（每颗符文的来源、底材位置、箱子开着、光标空、面板开着）；合成进行中交互层吃掉所有点击（dm_runeword_list_open 在合成中也答 1），
// 不让人在半途关箱子 / 挪东西；某颗拿起后没进孔（服务端不收），手上那颗按原路放回原格（0x18），成叠 / 共享仓库来的收进背包。镶进孔的原版就取不出来
// ---------- One-tap crafting: each rune is taken from where it is straight to the cursor and socketed into the base item where it is (the way a player does it in the original game, no detour through the inventory; from reading the code) ----------
// Pick up: inventory / personal stash cell = 0x19 [id][x][y][page]; rune page stack = the original game's "take 1 to the cursor" main+0x39d724 (sends 0x63, destination 0); shared stash cell = 0x46 [id][stash unit id][x | y<<16].
// Socket: base item in inventory / cube / personal stash = 0x28 [rune][base][0][page][u16 x][u16 y]; equipped in main equipment slot 1–10 = 0x28 [rune][base][equipment slot][0xff][u32 equipment slot].
// The original game cannot socket a base item that is in the shared stash (the server only looks up the base item by position in the grid snapshot of the player's own items); the list still shows it, without a button.
// Atomicity: every condition that can fail is checked before picking up the first rune (each rune's source, base item location, stash open, cursor empty, panel open); while crafting is in progress the interaction layer swallows all taps (dm_runeword_list_open also answers 1 during crafting),
// so nobody can close the stash / move things midway; if a rune was picked up but did not enter the socket (the server rejected it), the one in hand goes back the same way to its original cell (0x18), and one from a stack / the shared stash goes into the inventory. In the original game, ones already socketed cannot be taken out
enum { SRC_GRID = 1, SRC_STACK, SRC_SHARED };
typedef struct { int kind, x, y, page; uint32_t id, unit; } src_t;
static struct { int st, rec, nr, k, wait, cls[6], bkind, bpage, bx, by, bslot; uint32_t base, rune; src_t src[6]; } g_job;   // st：0 无 / 2 拿 / 3 等到光标上 / 4 等进孔 | st: 0 none / 2 pick up / 3 waiting to reach the cursor / 4 waiting to enter the socket
static uint64_t held_item(uint8_t *b, uint64_t pl) { uint64_t inv = pl ? *(uint64_t *)(pl + 0x90) : 0, h = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x40) : 0; (void)b; return h && *(uint32_t *)h == 4 ? h : 0; }
// 底材在哪、能不能镶：1 = 格子（玩家自己物品，页 0 / 3 / 4），2 = 身上主装备位 1–10，0 = 不能（共享仓库 / 副武器位 / 别处）
// Where the base item is and whether it can be socketed: 1 = grid cell (the player's own item, page 0 / 3 / 4), 2 = equipped in main equipment slot 1–10, 0 = cannot (shared stash / secondary weapon slot / elsewhere)
static int base_where(uint8_t *b, uint64_t pl, uint64_t it, int *page, int *x, int *y, int *slot) {
    uint64_t d = it ? *(uint64_t *)(it + 0x10) : 0; if (!d || !pl) return 0; int own = 0;
    for (uint64_t q = first_item(pl); q && !own; ) { uint64_t qd = *(uint64_t *)(q + 0x10); if (!qd) break; own = q == it; q = *(uint64_t *)(qd + 0xb0); }
    if (!own) return 0;
    uint32_t mode = *(uint32_t *)(it + 0xc);
    if (mode == 0) { int pg = PAGE(d); if (pg != PG_INV && pg != PG_CUBE && pg != PG_STASH) return 0; *page = pg; grid_xy(it, x, y); return 1; }
    if (mode == 1) { int sl = *(uint8_t *)(d + 0x54); if (sl < 1 || sl > 10) return 0; *slot = sl; return 2; }
    return 0;
}
// 第 cls 类符文挑一个来源（跳过已经排给前几颗的）：背包格 → 个人储藏箱格 → 符文页成叠 → 共享仓库格；箱子关着只认背包。魔盒里的不取
// Pick a source for a rune of class cls (skipping ones already assigned to earlier runes): inventory cell → personal stash cell → rune page stack → shared stash cell; with the stash closed only the inventory counts. Ones in the cube are not taken
static int pick_src(uint8_t *b, uint64_t pl, int cls, int stash_open, int k, src_t *out) {
    uint64_t us[3]; owners(b, pl, us);
    for (int pass = 0; pass < 4; pass++) for (int o = 0; o < 3; o++) for (uint64_t it = first_item(us[o]); it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        if ((int)*(uint32_t *)(it + 4) == cls && *(uint32_t *)(it + 0xc) == 0) {
            int pg = PAGE(d), st = *(uint8_t *)(d + 0x9c); uint32_t id = *(uint32_t *)(it + 8);
            int kind = st ? SRC_STACK : o ? SRC_SHARED : pg == PG_INV ? SRC_GRID : pg == PG_STASH ? SRC_GRID : 0, rank = st ? 2 : o ? 3 : pg == PG_INV ? 0 : 1;
            if (kind && rank == pass && (rank == 0 || stash_open)) {
                int used = 0; for (int j = 0; j < k; j++) used += g_job.src[j].id == id;
                if (used < (st ? st : 1)) { out->kind = kind; out->id = id; out->unit = o ? *(uint32_t *)(us[o] + 8) : 0; out->page = pg; grid_xy(it, &out->x, &out->y); return 1; } } }
        it = *(uint64_t *)(d + 0xb0); }
    return 0;
}
// 一键合成结束（成型 / 没成型 / 中途停）那一刻：宿主自己读回底材——孔数、孔里几颗、孔里各是什么、物品标志（0x04000000 = 符文之语），记一行「成型读回」，
// 计数加一。回归判据认这一行；触点脚本「await rw」等这个计数，等到就收局（判据按事件不按秒，DM_TEST_RW_LIST 是按进程秒的，await 一跳就撞）
// The moment one-tap crafting ends (formed / not formed / stopped midway): the host itself reads back the base item - socket count, how many are in the sockets, what each one is, item flags (0x04000000 = runeword), logs one 「成型读回」(formed readback) line,
// and increments the counter. The regression criterion keys on this line; the touch point script's "await rw" waits on this counter and ends the run once it arrives (the criterion goes by event, not by seconds; DM_TEST_RW_LIST goes by process seconds and collides as soon as await skips)
static _Atomic int g_job_done;
int dm_runeword_done_count(void) { return atomic_load(&g_job_done); }
static void job_readback(uint8_t *b, const char *how) {
    uint64_t it = b ? unit_item(b, g_job.base) : 0, d = it ? *(uint64_t *)(it + 0x10) : 0; char rs[64] = ""; size_t o = 0; int n = 0;
    uint64_t c = it ? *(uint64_t *)(it + 0x90) : 0;
    for (uint64_t x = c ? *(uint64_t *)(c + 0x10) : 0; x && n < 8; n++) { uint64_t xd = *(uint64_t *)(x + 0x10); uint32_t cd = ((uint32_t (*)(uint64_t))(b + 0x570260))(x);
        o += snprintf(rs + o, sizeof rs - o, "%s%.3s", n ? " " : "", (const char *)&cd); if (o >= sizeof rs) o = sizeof rs - 1; x = xd ? *(uint64_t *)(xd + 0xb0) : 0; }
    uint32_t code = it ? ((uint32_t (*)(uint64_t))(b + 0x570260))(it) : 0;
    LOG("符文之语：成型读回（%s）底材 id %u「%.3s」%d 孔、孔里 %d 颗（%s）标志 %#x", how, g_job.base, (const char *)&code, it ? sockets(b, it) : -1, n, rs, d ? *(uint32_t *)(d + 0x18) : 0);
    atomic_fetch_add(&g_job_done, 1);
}
static void job_stop(const char *why, int red) {
    uint8_t *b = rb(); uint64_t pl = b ? player(b) : 0, r = b && g_job.rune ? unit_item(b, g_job.rune) : 0, bs = b ? unit_item(b, g_job.base) : 0, h = held_item(b, pl);
    // 手上还拿着这次拿起的符文（没进孔）：放回原处。格子来的原格放回（0x18）；成叠 / 共享仓库来的收进背包（原版没有放回成叠 / 共享格的单包）
    // Still holding the rune picked up this time (it did not enter the socket): put it back where it came from. One from a grid cell goes back to its original cell (0x18); one from a stack / the shared stash goes into the inventory (the original game has no single packet for putting back into a stack / shared cell)
    if (h && g_job.k < g_job.nr && *(uint32_t *)(h + 8) == g_job.rune) { src_t *sr = &g_job.src[g_job.k];
        if (sr->kind == SRC_GRID) { uint8_t p[17] = {0x18}; uint32_t v[4] = { g_job.rune, (uint32_t)sr->x, (uint32_t)sr->y, (uint32_t)sr->page }; memcpy(p + 1, v, 16); send_pkt(b, p, 17); LOG("符文之语：手上的符文（id %u）放回原格 %d,%d 页 %d", g_job.rune, sr->x, sr->y, sr->page); }
        else { dm_held_stow(); LOG("符文之语：手上的符文（id %u）收进背包（来源是%s）", g_job.rune, sr->kind == SRC_STACK ? "符文页成叠" : "共享仓库"); } }
    LOG("%s符文之语：一键合成停（%s；已镶 %d / %d；符文 id %u %s 模式 %d；底材孔里 %d 颗）", red ? "【红】" : "", why, g_job.k, g_job.nr, g_job.rune, r ? "在" : "不在", r ? (int)*(uint32_t *)(r + 0xc) : -1, bs ? nfilled(bs) : -1);
    g_job.st = 0; job_readback(b, "中途停"); }
static int item_panel_open(uint8_t *b) { return inv_panel(b) || *(uint8_t *)(b + 0x2d5ff60); }   // 物品栏或储藏箱开着：光标上拿着东西而没有物品面板时，客户端会自己发 0x17 把它丢到地上（Mac 宿主实测） | Inventory panel or stash open: when something is on the cursor with no item panel, the client sends 0x17 by itself and drops it on the ground (measured on the Mac host)
static void job_start(uint8_t *b, int row) {
    if (g_job.st || row < 0 || row >= g_list.n || !g_list.c[row].ok) return;
    uint64_t it = unit_item(b, g_list.base), pl = player(b), t = it ? tables(b, it) : 0; int hw, hh;
    // 预检（一切能失败的条件都在拿第一颗之前核完）：底材还在、孔仍全空、仍匹配（重算清单）、这一行仍可做、底材在能镶的位置、光标空、物品面板开着、每颗符文都排到了来源
    // Pre-check (every condition that can fail is checked before picking up the first rune): base item still there, sockets still all empty, still matches (list recomputed), this row still craftable, base item in a socketable location, cursor empty, item panel open, every rune has a source assigned
    if (!it || !pl || !t || !base_ok(b, it, t) || dm_held_size(&hw, &hh) || !item_panel_open(b)) { list_close(b, "预检没过"); LOG("符文之语：预检没过（底材 %s、光标%s），不动", it ? "在" : "不在", dm_held_size(&hw, &hh) ? "上有东西" : "空"); return; }
    cand_t c = g_list.c[row]; build_list(b, it); int still = 0; for (int i = 0; i < g_list.n; i++) if (g_list.c[i].rec == c.rec && g_list.c[i].ok) still = 1;
    if (!still) { list_close(b, "预检：符文已不齐"); return; }
    memset(&g_job, 0, sizeof g_job); g_job.rec = c.rec; g_job.nr = c.nr; memcpy(g_job.cls, c.cls, sizeof c.cls); g_job.base = g_list.base;
    g_job.bkind = base_where(b, pl, it, &g_job.bpage, &g_job.bx, &g_job.by, &g_job.bslot);
    int stash_open = *(uint8_t *)(b + 0x2d5ff60) != 0;
    if (!g_job.bkind || (g_job.bkind == 1 && g_job.bpage == PG_STASH && !stash_open)) { list_close(b, "预检：底材位置不能镶"); LOG("符文之语：预检没过（底材位置不能镶），不动"); return; }
    for (int k = 0; k < c.nr; k++) if (!pick_src(b, pl, c.cls[k], stash_open, k, &g_job.src[k])) { list_close(b, "预检：符文来源不齐"); LOG("符文之语：预检没过（第 %d 颗没有可取的来源），不动", k + 1); memset(&g_job, 0, sizeof g_job); return; }
    g_job.st = 2; list_close(b, "开始合成");
    LOG("符文之语：一键合成「%s」开始（底材 id %u %s，%d 颗）", c.name, g_job.base, g_job.bkind == 2 ? "身上" : g_job.bpage == PG_STASH ? "储藏箱" : g_job.bpage == PG_CUBE ? "魔盒" : "背包", c.nr);
}
static void job_pump(uint8_t *b) {
    if (!g_job.st) return; uint64_t pl = player(b), base = unit_item(b, g_job.base); if (!pl || !base) { job_stop("底材或玩家不见了", 1); return; }
    // 测试开关 DM_TEST_RW_FAIL=<n>：拿第 n 颗（从 1 起）之前当作「来源没了」停下（验原子性：第 1 颗就停 = 什么都不动）
    // Test switch DM_TEST_RW_FAIL=<n>: before picking up rune n (counting from 1), act as if "the source is gone" and stop (verifies atomicity: stopping at rune 1 = nothing is touched)
    static int fail_at = -2; if (fail_at == -2) { const char *e = getenv("DM_TEST_RW_FAIL"); fail_at = e ? atoi(e) : 0; }
    switch (g_job.st) {
    case 2: {   // 拿：第 k 颗从它的来源直接拿到光标 | Pick up: take rune k from its source straight to the cursor
        if (g_job.k >= g_job.nr) { uint64_t d = *(uint64_t *)(base + 0x10); uint32_t fl = d ? *(uint32_t *)(d + 0x18) : 0;   // 物品标志位 0x04000000 = 符文之语（原版成型 0x64d080 置） | Item flag bit 0x04000000 = runeword (set by the original game's forming routine 0x64d080)
            if (fl & 0x04000000) LOG("符文之语：一键合成成型（底材 id %u，%d 颗，标志 %#x）", g_job.base, g_job.nr, fl); else LOG("【红】符文之语：%d 颗都镶进去了但没成型（标志 %#x）", g_job.nr, fl);
            g_job.st = 0; job_readback(b, fl & 0x04000000 ? "成型" : "没成型"); return; }
        if (held_item(b, pl)) { job_stop("光标上有别的东西", 1); return; }
        if (!item_panel_open(b)) { job_stop("物品面板关了", 1); return; }
        if (fail_at == g_job.k + 1) { job_stop("测试开关 DM_TEST_RW_FAIL：当作来源没了", 0); return; }
        src_t *sr = &g_job.src[g_job.k]; uint64_t r = unit_item(b, sr->id);
        if (!r || *(uint32_t *)(r + 0xc) != 0) { job_stop("这颗符文的来源不见了", 1); return; }
        if (sr->kind == SRC_GRID) { uint8_t p[17] = {0x19}; uint32_t v[4] = { sr->id, (uint32_t)sr->x, (uint32_t)sr->y, (uint32_t)sr->page }; memcpy(p + 1, v, 16); send_pkt(b, p, 17); }
        else if (sr->kind == SRC_SHARED) { uint8_t p[13] = {0x46}; uint32_t v[3] = { sr->id, sr->unit, (uint32_t)sr->x | (uint32_t)sr->y << 16 }; memcpy(p + 1, v, 12); send_pkt(b, p, 13); }
        else { static uint64_t fake[0x620 / 8]; fake[0x608 / 8] = r; ((void (*)(void *))(b + 0x39d724))(fake); }   // 原版「成叠取 1 件到光标」（发 0x63 子命令 1、目的地 0） | The original game's "take 1 from a stack to the cursor" (sends 0x63 subcommand 1, destination 0)
        g_job.rune = 0; g_job.st = 3; g_job.wait = 120;
        LOG("符文之语：第 %d 颗：从%s拿起（id %u%s）", g_job.k + 1, sr->kind == SRC_GRID ? (sr->page == PG_STASH ? "储藏箱格" : "背包格") : sr->kind == SRC_STACK ? "符文页成叠" : "共享仓库格", sr->id, sr->kind == SRC_STACK ? "，取 1 件" : ""); return; }
    case 3: {   // 等符文到光标上（成叠取出来的是新的一件，按手上那件认），到了就镶（0x28） | Wait for the rune to reach the cursor (one taken from a stack is a new item, identified by the item in hand); once there, socket it (0x28)
        uint64_t h = held_item(b, pl); if (!h || (int)*(uint32_t *)(h + 4) != g_job.cls[g_job.k]) { if (--g_job.wait <= 0) job_stop("符文 2 秒没到光标上", 1); return; }
        g_job.rune = *(uint32_t *)(h + 8);
        uint8_t p[21] = {0x28}; uint32_t v[4]; uint32_t w5;
        if (g_job.bkind == 2) { v[0] = g_job.rune; v[1] = g_job.base; v[2] = (uint32_t)g_job.bslot; v[3] = 0xff; w5 = (uint32_t)g_job.bslot; }
        else { v[0] = g_job.rune; v[1] = g_job.base; v[2] = 0; v[3] = (uint32_t)g_job.bpage; w5 = (uint32_t)(uint16_t)g_job.bx | (uint32_t)(uint16_t)g_job.by << 16; }
        memcpy(p + 1, v, 16); memcpy(p + 17, &w5, 4); send_pkt(b, p, 21);
        g_job.st = 4; g_job.wait = 120;
        if (g_job.bkind == 2) LOG("符文之语：第 %d 颗：镶进身上装备位 %d 的底材（id %u）", g_job.k + 1, g_job.bslot, g_job.base);
        else LOG("符文之语：第 %d 颗：镶进底材（id %u，页 %d，格 %d,%d）", g_job.k + 1, g_job.base, g_job.bpage, g_job.bx, g_job.by); return; }
    case 4: {   // 等进孔（符文模式 6、底材孔里多了一颗），再拿下一颗 | Wait for it to enter the socket (rune mode 6, one more rune in the base item's sockets), then pick up the next one
        uint64_t r = unit_item(b, g_job.rune);
        if (r && *(uint32_t *)(r + 0xc) == 6 && nfilled(base) >= g_job.k + 1) { g_job.k++; g_job.st = 2; return; }
        if (--g_job.wait <= 0) job_stop("符文 2 秒没进孔", 1); return; }
    }
}

// ---------- 对外 ----------
// ---------- Public ----------
int dm_runeword_long_press(const dm_cursor_item_info *ci) {   // 长按物品分支调：是匹配底材就打开清单，返回 1 | Called from the long-press-item branch: if it is a matching base item, open the list and return 1
    uint8_t *b = rb(); if (!b || !ci || g_job.st) return 0;
    if (ci->container != DM_ITEM_AT_INVENTORY && ci->container != DM_ITEM_AT_STASH && ci->container != DM_ITEM_AT_EQUIP) return 0;   // 身上穿着的也算（原版能往身上的装备镶） | Equipped items count too (the original game can socket into equipped gear)
    uint64_t it = unit_item(b, ci->id); if (!it || *(uint32_t *)(it + 0xc) > 1) return 0;
    if (!build_list(b, it)) return 0;
    g_list.base = ci->id; g_list.open = 1; ui_show(b, 1); ui_fill(b, it); ui_layout(); g_check_at = 6;
    LOG("符文之语：长按 %s（id %u）→ 清单 %d 行（首行「%s」需要等级 %d %s）", ci->code, ci->id, g_list.n, g_list.c[0].name, g_list.c[0].lv, g_list.c[0].ok ? "可做" : "不可做");
    if (getenv("DM_TEST_RW_DESC") && !strcmp(getenv("DM_TEST_RW_DESC"), "all")) {   // 测试开关 =all：表里每个 complete 的符文之语都生成一遍原版说明，记行数（定详情字号用） | Test switch =all: generate the original game's description once for every complete runeword in the table and log the line counts (used to pick the detail font size)
        uint8_t ver = *(uint8_t *)(b + 0x2127c48); uint64_t t = ver >= 1 && ver <= 3 ? *(uint64_t *)(b + 0x2d64a18 + ver * 16) : 0, rr = t ? *(uint64_t *)(t + 0x1880) : 0; uint32_t rn = t ? *(uint32_t *)(t + 0x1888) : 0;
        int mx = 0, mxr = -1, cnt = 0, fail = 0; static char d[4096];
        for (uint32_t r = 0; rr && r < rn; r++) { if (!*(uint8_t *)(rr + (uint64_t)r * 0xe8 + 0x44)) continue; cnt++;
            if (!rw_desc(b, (int)r, d, sizeof d)) { fail++; continue; } int nl = 1; for (char *q = d; *q; q++) nl += *q == '\n';
            if (nl > mx) { mx = nl; mxr = (int)r; } }
        LOG("符文之语：全表说明 %d 条（生成失败 %d），最多 %d 行（记录 %d）", cnt, fail, mx, mxr);
        if (mxr >= 0 && rw_desc(b, mxr, d, sizeof d)) { for (char *q = d; *q; q++) if (*q == '\n') *q = '|'; LOG("符文之语：最长那条：%s", d); } }
    else if (getenv("DM_TEST_RW_DESC")) for (int i = 0; i < g_list.n; i++) {   // 测试开关：把每个候选的原版说明逐个记下来（换行记成 ⏎，原版行序是倒的） | Test switch: log the original game's description of each candidate one by one (newlines logged as ⏎; the original game's line order is reversed)
        static char d[4096]; int k = rw_desc(b, g_list.c[i].rec, d, sizeof d); for (char *q = d; *q; q++) if (*q == '\n') *q = '|';
        LOG("符文之语：说明「%s」%d 字节：%s", g_list.c[i].name, k, d); }
    return 1;
}
void dm_runeword_list_close(void) { uint8_t *b = rb(); if (b) list_close(b, "点了清单外"); }
int dm_runeword_list_open(void) { return g_list.open || g_job.st; }   // 合成进行中也答「开着」：交互层吃掉这期间所有点击，不让人半途关箱子 / 挪东西（一键合成的原子性） | Also answers "open" while crafting is in progress: the interaction layer swallows all taps during that time so nobody can close the stash / move things midway (atomicity of one-tap crafting)
// 清单按钮：「一键合成」TouchRwGo<行号>、翻页 TouchRwPgPrev / TouchRwPgNext。触摸下由交互层吃掉点击后直接调（dm_cursor.c ui_classify，不发鼠标点击，
// 免得落到按钮底下的物品格上）；按钮自己的点击消息（PanelManager:ClosePanel:<名字>，鼠标 / 手柄确认键点到时游戏发）也走这里
// List buttons: 「一键合成」(Craft) TouchRwGo<row number>, paging TouchRwPgPrev / TouchRwPgNext. Under touch, the interaction layer swallows the tap and calls directly (ui_classify in dm_cursor.c, no mouse click is sent,
// so it does not land on the item cell beneath the button); the button's own click message (PanelManager:ClosePanel:<name>, sent by the game when hit by mouse / controller confirm button) also comes here
void dm_runeword_button(const char *name) {
    uint8_t *b = rb(); if (!b || !name || !g_list.open) return;
    if (W.det >= 0) {   // 详情开着：只认「← 返回」与一键合成，点别处不动 | Detail open: only 「← 返回」(<- Back) and Craft are recognized; taps elsewhere do nothing
        if (!strcmp(name, "TouchRwDetGo")) { job_start(b, W.det); return; }
        if (!strncmp(name, "TouchRwBack", 11)) { LOG("符文之语：收起详情「%s」（点了 %s）", g_list.c[W.det].name, name); W.det = -1; ui_layout(); g_check_at = 6; }
        return; }
    if (!strncmp(name, "TouchRwName", 11) || !strncmp(name, "TouchRwLv", 9)) { det_open(b, atoi(name + (name[7] == 'N' ? 11 : 9))); return; }   // 点名字（连同后面的需要等级，手指好点中）= 看属性 | Tapping the name (together with the required level after it, easier for a finger to hit) = view stats
    if (!strncmp(name, "TouchRwGo", 9)) { job_start(b, atoi(name + 9)); return; }
    if (!strncmp(name, "TouchRwPg", 9)) {   // 到头不绕回，同仓库页 | No wrap-around at the ends, same as the stash pages
        int d = !strcmp(name + 9, "Next") ? 1 : -1, to = W.page + d;
        if (to >= 0 && to < W.pages) { W.page = to; ui_layout(); g_check_at = 6; }
        LOG("符文之语：翻页 %s → 第 %d/%d 页（第 %d–%d 个）", name + 9, W.page + 1, W.pages, W.first + 1, W.last); }
}
// 手柄模式（「符文之语助手挂原版对比键」）：物品栏里焦点在匹配的带孔底材上、按住原版对比键（手柄动作键序号 2，原版按着时物品格控件 +0x632 = 1，
// 见 dm_actions.c 装备对比）满 PAD_HOLD 帧 → 打开同一份清单。清单开着时十字键 / A / B / L / R 归清单（不交给原版，免得背包光标跟着动、B 把物品栏关了）：
//   上下 = 选行（跨页自动翻）、L / R = 翻页、A = 看这一个的详情（详情里 A = 一键合成，能做时）、B = 详情里回清单 / 清单里关清单。清单关了以后，当时按着的键松开前仍不交给原版
// Controller mode ("the runeword helper hangs on the original compare button"): in the inventory, with focus on a matching socketed base item, holding the
// original compare button (controller action index 2; while it is held the original item-cell widget has +0x632 = 1,
// see equipment comparison in dm_actions.c) for PAD_HOLD frames → opens the same list. While the list is open the D-pad / A / B / L / R belong to the
// list (not passed to the original, so the inventory cursor does not move along and B does not close the inventory):
//   up/down = select a row (pages flip automatically), L / R = flip pages, A = view this entry's details (in details A = one-tap craft, when possible),
//   B = back to the list from details / close the list from the list. After the list closes, buttons held at that moment stay withheld from the original
//   until released
enum { PAD_HOLD = 15 };
static _Atomic uint64_t g_pad_ev;
uint64_t dm_runeword_pad_filter(uint64_t bt) {   // 取手柄处（hle_hid.c fill）：手柄模式且清单开着时吃掉这几个键，按下沿记给 dm_runeword_pump | Controller read (hle_hid.c fill): in controller mode with the list open, swallow these buttons and record their press edges for dm_runeword_pump
    int dm_padmode_current(void); static uint64_t prev, eat; const uint64_t M = DM_BTN_UP | DM_BTN_DOWN | DM_BTN_LEFT | DM_BTN_RIGHT | DM_BTN_A | DM_BTN_B | DM_BTN_L | DM_BTN_R;
    uint64_t down = bt & ~prev; prev = bt;
    if (dm_padmode_current() && (g_list.open || g_job.st)) { atomic_fetch_or(&g_pad_ev, down & M); eat |= bt & M; return bt & ~M; }
    eat &= bt; return bt & ~eat;
}
static void pad_sel(int to) {   // 选中第 to 个（跨页时翻到它那一页） | Select the to-th entry (flip to its page when it is on another page)
    if (to < 0 || to >= g_list.n || to >= NROW) return; W.psel = to;
    for (int p = 0; p < W.pages; p++) if (to >= W.pstart[p] && to < W.pstart[p + 1]) W.page = p;
    ui_layout(); g_check_at = 6;
}
static void pad_pump(uint8_t *b) {   // 游戏主线程（dm_runeword_pump） | Game main thread (dm_runeword_pump)
    int dm_padmode_current(void); if (!dm_padmode_current()) { atomic_store(&g_pad_ev, 0); return; }
    static uint64_t t0; static int fired, parked, px, py;
    // 清单开着时原版光标还停在底材上，说明框（按着对比键时还有对比框）会盖住清单：打开时把光标送到屏幕左缘的停靠点（同触屏长按那条路），清单关了放回原处
    // While the list is open the original cursor still rests on the base item, and the tooltip box (plus the comparison box while the compare button is
    // held) would cover the list: on opening, send the cursor to the park point at the left edge of the screen (same path as a touch long-press), and put it
    // back when the list closes
    if (!g_list.open && !g_job.st && parked) { parked = 0; dm_ui_hover_now(px, py); }
    if (!g_list.open && !g_job.st) {   // 按住对比键打开 | Opened by holding the compare button
        uint64_t root = *(uint64_t *)(b + 0x2f05458), fm = root ? *(uint64_t *)(root + 0xd0) : 0; int held = 0;
        for (int i = 0; fm && i < 2 && !held; i++) { uint64_t w = *(uint64_t *)(fm + (i ? 0x178 : 0x190)); held = w && *(uint64_t *)w - (uint64_t)(uintptr_t)b == 0x2089ab8 && *(uint8_t *)(w + 0x632); }
        uint64_t f = dm_guest_frame(); if (!held || !g_inv_open) { t0 = 0; fired = 0; }
        else if (!fired) { if (!t0) t0 = f; else if (f - t0 >= PAD_HOLD) { fired = 1; dm_cursor_item_info ci; if (dm_cursor_item(&ci)) { if (dm_runeword_long_press(&ci)) { W.psel = W.first; ui_layout(); LOG("符文之语：手柄按住对比键 %d 帧 → 打开清单", PAD_HOLD);
                px = *(int32_t *)(b + 0x2d128f8); py = *(int32_t *)(b + 0x2d128fc); parked = 1; dm_ui_hover_now(*(int32_t *)(b + 0x211da38) * 2 / 100, *(int32_t *)(b + 0x211da3c) / 2); } } } }
        atomic_store(&g_pad_ev, 0); return; }
    uint64_t ev = atomic_exchange(&g_pad_ev, 0); if (!ev || g_job.st) return;
    if (W.det >= 0) {   // 详情页 | Detail page
        if (ev & DM_BTN_A) { if (g_list.c[W.det].ok) dm_runeword_button("TouchRwDetGo"); }
        else if (ev & DM_BTN_B) dm_runeword_button("TouchRwBack");
        return; }
    if (ev & DM_BTN_B) { list_close(b, "手柄 B"); return; }
    if (ev & DM_BTN_A) { det_open(b, W.psel); LOG("符文之语：手柄 A → 详情「%s」", g_list.c[W.psel].name); return; }
    if (ev & DM_BTN_DOWN) pad_sel(W.psel + 1); else if (ev & DM_BTN_UP) pad_sel(W.psel - 1);
    else if (ev & (DM_BTN_L | DM_BTN_R)) { int to = W.page + ((ev & DM_BTN_R) ? 1 : -1); if (to >= 0 && to < W.pages) pad_sel(W.pstart[to]); }
}
void dm_runeword_on_uimsg(const char *cat, const char *act, const char *arg) {   // 界面消息回调（diag.c 转来，游戏主线程） | UI message callback (forwarded from diag.c, game main thread)
    uint8_t *b = rb(); if (!b || strcmp(cat, "PanelManager")) return;
    if (!strcmp(act, "ClosePanel") && (!strncmp(arg, "TouchRwGo", 9) || !strncmp(arg, "TouchRwPg", 9) || !strncmp(arg, "TouchRwBack", 11) || !strcmp(arg, "TouchRwDetGo"))) { dm_runeword_button(arg); return; }
    if (!strcmp(arg, "PlayerInventoryExpansionLayout")) { if (!strcmp(act, "OpenPanel") || !strcmp(act, "OpenExclusivePanel")) g_inv_open = 1; else if (!strcmp(act, "ClosePanel") || !strcmp(act, "UnloadPanel")) g_inv_open = 0; }
    if (!strcmp(act, "ClosePanel") && (!strcmp(arg, "PlayerInventoryExpansionLayout") || !strcmp(arg, "BankExpansionLayout") || !strcmp(arg, "UISwitcher"))) list_close(b, "面板关了");
}
void dm_runeword_pump(void) {   // 取手柄处每帧调（游戏主线程，dm_actions_pump 转来） | Called every frame at the controller sampling point (game main thread, forwarded from dm_actions_pump)
    uint8_t *b = rb(); if (!b) return;
    if (g_list.open && dm_ui_unstable()) return;   // 界面正在重建：这一帧不排版、不找控件 | The UI is rebuilding: no layout and no widget lookup this frame
    if (w_stale()) { int was = g_list.open; g_list.open = 0; memset(&W, 0, sizeof W); W.b = b; g_check_at = 0; if (was) LOG("符文之语：界面重建过（界面代号变了）→ 清单当作已关，缓存的控件作废"); }
    if (g_list.open) { uint64_t it = unit_item(b, g_list.base); if (!inv_panel(b) || !it || *(uint32_t *)(it + 0xc) > 1) list_close(b, "底材挪走了或面板没了"); }   // 先核面板与底材，再用缓存排版 | Check the panel and base item first, then use the cached layout
    if (g_list.open) { ui_layout(); if (g_check_at > 0 && --g_check_at == 0) ui_check(1); }
    job_pump(b);
    pad_pump(b);
    // 测试开关 DM_TEST_RW_OPEN=<秒>:<物品码>[@inv|@stash|@shared]：进程起来后到这一秒（且物品栏开着）对第一件这个码、能开清单的物品按长按打开清单（不经触摸，宿主 / 真机脚本造现场用）。
    // @ 后缀只认放在那里的：inv = 背包，stash = 个人储藏箱，shared = 共享仓库（另外两个物品单位），equip = 穿在身上；不写 = 玩家自己的都算
    // Test switch DM_TEST_RW_OPEN=<seconds>:<item code>[@inv|@stash|@shared]: at that second after the process starts (and with the inventory panel open), open the list as a long press would on the first item with this code that can open the list (bypasses touch; for test setup in host / real device scripts).
    // The @ suffix restricts it to items located there: inv = inventory, stash = personal stash, shared = shared stash (the other two item units), equip = equipped; omitted = anything of the player's own counts
    static long at = -2, fr; static char code[32], where[16]; if (at == -2) { const char *e = getenv("DM_TEST_RW_OPEN"); at = -1; if (e) { at = atol(e); const char *c = strchr(e, ':'); if (c) strlcpy(code, c + 1, sizeof code); char *w = strchr(code, '@'); if (w) { strlcpy(where, w + 1, sizeof where); *w = 0; } } }
    // 测试开关 DM_TEST_RW_GO=<秒>：到这一秒点清单第一行的「一键合成」（同按钮消息）
    // Test switch DM_TEST_RW_GO=<seconds>: at that second, tap 「一键合成」(Craft) on the first row of the list (same as the button message)
    static long go = -2; if (go == -2) { const char *e = getenv("DM_TEST_RW_GO"); go = e ? atol(e) : -1; }
    fr++; static double t0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec / 1e9; if (!t0) t0 = now; double sec = now - t0;
    static long open_fr; if (g_inv_open && !open_fr) open_fr = fr; if (!g_inv_open) open_fr = 0;
    if (at > 0 && sec >= at && open_fr && fr - open_fr > 30) { at = -1; uint64_t pl = player(b), us[3] = {0}; int done = 0; if (pl) owners(b, pl, us);
        for (int o = 0; o < 3 && !done; o++) for (uint64_t it = first_item(us[o]); it && !done; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
            int pg = PAGE(d), here = !where[0] ? !o : !strcmp(where, "inv") ? !o && pg == PG_INV : !strcmp(where, "stash") ? !o && pg == PG_STASH : !strcmp(where, "shared") ? o > 0 : !strcmp(where, "equip") ? !o && *(uint32_t *)(it + 0xc) == 1 : 0;
            uint32_t cd = ((uint32_t (*)(uint64_t))(b + 0x570260))(it); if (here && *(uint32_t *)(it + 0xc) <= 1 && !memcmp(&cd, code, strlen(code) < 4 ? strlen(code) : 4)) {
                dm_cursor_item_info ci; memset(&ci, 0, sizeof ci); ci.id = *(uint32_t *)(it + 8); memcpy(ci.code, code, 3); ci.container = *(uint32_t *)(it + 0xc) == 1 ? DM_ITEM_AT_EQUIP : o || pg == PG_STASH ? DM_ITEM_AT_STASH : DM_ITEM_AT_INVENTORY;
                int ok = dm_runeword_long_press(&ci); LOG("测试 DM_TEST_RW_OPEN：第 %.1f 秒对 %s（id %u，单位 %d 页 %d，孔 %d）长按 → %s", sec, code, ci.id, o, pg, sockets(b, it), ok ? "开了清单" : "不是匹配底材"); done = ok; }
            it = *(uint64_t *)(d + 0xb0); }
        if (!done) { LOG("测试 DM_TEST_RW_OPEN：第 %.1f 秒没有能开清单的 %s%s%s；同码物品：", sec, code, where[0] ? "@" : "", where);
            { uint32_t sid = shared_unit_id(b); LOG("测试 DM_TEST_RW_OPEN：共享仓库单位 id %u；另两个单位 id %u / %u", sid, us[1] ? *(uint32_t *)(us[1] + 8) : 0, us[2] ? *(uint32_t *)(us[2] + 8) : 0);
              for (int ty = 0; ty < 6 && sid; ty++) { uint64_t u = unit_any(b, sid, ty); if (!u) continue; int ni = 0; for (uint64_t q = first_item(u); q && ni < 200; ni++) { uint64_t qd = *(uint64_t *)(q + 0x10); q = qd ? *(uint64_t *)(qd + 0xb0) : 0; }
                  LOG("测试 DM_TEST_RW_OPEN：  按类型 %d 找到单位 %p，物品 %d 件", ty, (void *)u, ni); } }
            for (int o = 0; o < 3; o++) for (uint64_t it = first_item(us[o]); it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; uint32_t cd = ((uint32_t (*)(uint64_t))(b + 0x570260))(it);
                if (!memcmp(&cd, code, strlen(code) < 4 ? strlen(code) : 4)) LOG("测试 DM_TEST_RW_OPEN：  id %u 单位 %d 页 %u 模式 %u 孔 %d", *(uint32_t *)(it + 8), o, PAGE(d), *(uint32_t *)(it + 0xc), sockets(b, it));
                it = *(uint64_t *)(d + 0xb0); } } }
    // 测试开关 DM_TEST_RW_LIST=<秒>：到这一秒把玩家三个单位里带符文之语位（物品标志 0x04000000）的物品列一遍（存档读回验证用，只读）
    // Test switch DM_TEST_RW_LIST=<seconds>: at that second, list the items in the player's three units that have the runeword bit (item flag 0x04000000) (for verifying save read-back; read-only)
    static double ls = -2; if (ls == -2) { const char *e = getenv("DM_TEST_RW_LIST"); ls = e ? atof(e) : -1; }
    if (ls > 0 && sec >= ls) { ls = -1; uint64_t pl = player(b), us[3]; int n = 0; if (pl) { owners(b, pl, us);
        for (int o = 0; o < 3; o++) for (uint64_t it = first_item(us[o]); it; ) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
            if (*(uint32_t *)(d + 0x18) & 0x04000000) { uint32_t cd = ((uint32_t (*)(uint64_t))(b + 0x570260))(it); LOG("测试 DM_TEST_RW_LIST：符文之语物品 id %u「%.4s」品质 %u 单位 %d 页 %u 模式 %u 孔里 %d 颗 标志 %#x", *(uint32_t *)(it + 8), (const char *)&cd, *(uint32_t *)d, o, PAGE(d), *(uint32_t *)(it + 0xc), nfilled(it), *(uint32_t *)(d + 0x18)); n++;
                struct { uint64_t n; uint8_t has; } c0 = { 0, 0 };   // 原版给真物品出说明的调法（main+0x37a4d8 等 6 处：不带属性列表）——与详情文字逐条对照用 | How the original game produces the description for a real item (main+0x37a4d8 etc., 6 sites: without a stat list) -- used for comparing against the detail text entry by entry
                gstr_t tip = ((gstr_t (*)(uint64_t, uint64_t, uint64_t, int, int, int, void *, void *))(b + 0x374f00))(pl, pl, it, 0, 1, 0, NULL, &c0);
                if (tip.p) { static char tb[4096]; size_t k = tip.len < sizeof tb - 1 ? tip.len : sizeof tb - 1; memcpy(tb, tip.p, k); tb[k] = 0; for (char *q = tb; *q; q++) if (*q == '\n') *q = '|';
                    LOG("测试 DM_TEST_RW_LIST：原版说明 %s", tb); if (!(tip.cap >> 63)) g_free(b, tip.p); } }
            it = *(uint64_t *)(d + 0xb0); } }
        LOG("测试 DM_TEST_RW_LIST：第 %.1f 秒共 %d 件符文之语物品", sec, n); }
    if (go > 0 && sec >= go) { go = -1; LOG("测试 DM_TEST_RW_GO：第 %.1f 秒点第一行「一键合成」", sec); job_start(b, 0); }
}
