// 百宝囊与「存放」（百宝囊是扩展背包——野外的符文 / 宝石收进囊里不占背包格，回城开储藏箱时自动存进高级仓库的宝石页 / 符文页）。
//   为什么不在野外直接「移到仓库」：原版「移到仓库」0x63（main+0x6d0d10）要求玩家 50 以内有储藏箱物件（0x650d10 找物件 267），野外整包拒绝（线上 6 件全留背包）；
//   不改补丁、不绕原版限制。
//   收：从地上进了背包的符文 / 宝石（自动捡的：18 号以内符文、全部宝石，PICK_R 格内；玩家自己捡的也算），服务端拍里收进囊：写位流 0x581b60 → 摘下 0x55e430 →
//       通知客户端 0x642f70(…, 0x20, 4, 0, 页, y, x) → 释放 0x658be0（同编年史存入）。人在储藏箱旁（0x650d10(游戏, 玩家, 267, 50)）就直接走原版 0x63 进高级仓库，不进囊。
//       从仓库取出来的（没在地上出现过）不收。
//   放：开着储藏箱时，囊里的逐件建回背包（读位流建物品 0x5793a0 + 0x6564e0 → 原版找空位 0x55f620 → 读档放物品 0x6f1940 → 通知 0x642f70(…, 1, 2, 0, 0, 0, x | y << 16)，
//       同编年史取出；背包满了就停，下一批再放），等这一件落盘证明后，服务端代发原版 0x63 进宝石页 / 符文页、叠堆；2 秒还在背包（页满 / 叠满）就收回囊里。
//   存放按钮：背包里已鉴定的暗金 / 套装件 → 编年史仓库（dm_vault.c 存入，一件一件排队）；符文 / 宝石 → 储藏箱旁进高级仓库，别处进百宝囊。别的不动。
//          做完出一条原版系统提示（聊天栏 + 提示框，dm_game_notice，服务端线程发）：「存放：编年史 N 件，储藏箱 M 件，百宝囊 K 件」，没东西可存也提示。
// 落盘（不变式：同一件只在一处——侧文件 <角色>/<角色>.swpouch 里，或原版角色档里；侧文件跟着角色目录走 iCloud，dm_cloudsync.c）：
//   符文 / 宝石是简单物品，同种的字节一样，不能像编年史那样只认「在不在」；但位流前 8 字节带着位置（方式 / x / y / 页），背包同一格同一时刻只有一件，
//   所以按整串位流（含背包格位）只在本角色档里找：
//   收进囊：这次要提交的角色档里已没有那一串 → 并进盘上状态；放到背包：角色档里已有放好的那一串 → 从盘上状态去掉，之后才发 0x63（0x63 之后的事全在原版存档里）。
//   都在提交前钩子里判，侧文件与角色档同一次提交原子换入：杀进程只会回到某次提交的一致状态，不丢、不复制。没证明的留着等下一次存盘（服务端拍每 2 秒再请一次）。
//   开箱放之前先等收进囊的都证明完（同一件不会同时挂着收和放）。
// 界面：物品栏面板里我们自己的控件（布局 dm/ios/tools/pouch_layout.py）：TouchPouch / TouchStore 两个按钮发 ClosePanel:TouchPouch / TouchStore（空操作，这里当信号收）。
// 百宝囊框的数字 = 囊里件数，点它出清单（读侧文件来的内容，重进游戏还在；开箱搬完清零）。
// 只写自己的控件（TouchPouch* / TouchStore）；「自定义」页开关 gem_pick（自动收符文宝石 + 百宝囊框）、store_btn（存放按钮），缺省开，关了整块挪到屏外。
// 线程：dm_pouch_step 在游戏主线程（dm_world.c）；界面消息回调 dm_pouch_on_uimsg（diag.c）；收 / 放 / 系统提示在服务端拍 dm_pouch_srv_tick（diag.c hud_tick）；
// 落盘在写盘线程的提交前钩子 pouch_hook（hle_fs.c Commit 开头，[2]）。
// The pouch and 「存放」(Deposit) (the pouch is an extended inventory -- runes / gems found in the wilderness are stowed into the pouch without taking inventory cells, and are automatically stored into the gem page / rune page of the advanced stash when the stash is opened back in town).
//   Why not "move to stash" directly in the wilderness: the original game's "move to stash" 0x63 (main+0x6d0d10) requires a stash object within 50 of the player (0x650d10 looks for object 267) and rejects the whole packet in the wilderness (in the released build all 6 items stayed in the inventory);
//   no patch changes, no bypassing the original game's restriction.
//   Stow: runes / gems that entered the inventory from the ground (auto-picked: runes up to No. 18, all gems, within PICK_R tiles; ones the player picks up themselves count too) are stowed into the pouch in the server tick: write bitstream 0x581b60 -> remove 0x55e430 ->
//       notify the client 0x642f70(..., 0x20, 4, 0, page, y, x) -> free 0x658be0 (same as chronicle store). If the player is next to the stash (0x650d10(game, player, 267, 50)), go straight through the original 0x63 into the advanced stash, not into the pouch.
//       Items taken out of the stash (never seen on the ground) are not stowed.
//   Release: while the stash is open, the pouch items are built back into the inventory one by one (read bitstream and build item 0x5793a0 + 0x6564e0 -> original free-spot search 0x55f620 -> load-time item placement 0x6f1940 -> notify 0x642f70(..., 1, 2, 0, 0, 0, x | y << 16),
//       same as chronicle retrieve; stop when the inventory is full and continue in the next batch); once this item is proven persisted, the server sends the original 0x63 on the player's behalf into the gem page / rune page, stacking; if still in the inventory after 2 seconds (page full / stack full), take it back into the pouch.
//   Deposit button: identified unique / set items in the inventory -> chronicle vault (dm_vault.c store, queued one at a time); runes / gems -> advanced stash when next to the stash, pouch elsewhere. Nothing else is touched.
//          When done, show one original game system notice (chat bar + notice box, dm_game_notice, sent from the server thread): 「存放：编年史 N 件，储藏箱 M 件，百宝囊 K 件」(Deposited: Chronicle N, Stash M, Pouch K); a notice is shown even when there is nothing to deposit.
// Persistence (invariant: the same item exists in only one place -- in the sidecar file <character>/<character>.swpouch, or in the original character save; the sidecar file goes to iCloud with the character directory, dm_cloudsync.c):
//   runes / gems are simple items and same-kind bytes are identical, so "present or not" alone cannot be used as with the chronicle; but the first 8 bytes of the bitstream carry the position (placement / x / y / page), and one inventory cell holds only one item at a time,
//   so search for the whole bitstream (including the inventory cell position) in this character's save only:
//   stow into the pouch: the character save about to be committed no longer contains that bitstream -> merge into the on-disk state; release to the inventory: the character save already contains the placed bitstream -> remove from the on-disk state, and only then send 0x63 (everything after 0x63 lives in the original save).
//   All decided in the pre-commit hook; the sidecar file and the character save are swapped in atomically in the same commit: killing the process only returns to the consistent state of some commit, nothing lost, nothing duplicated. Unproven entries wait for the next save (the server tick requests one again every 2 seconds).
//   Before releasing on stash open, wait until everything stowed into the pouch is proven (the same item is never pending both stow and release).
// UI: our own widgets in the inventory panel (layout dm/ios/tools/pouch_layout.py): the two buttons TouchPouch / TouchStore send ClosePanel:TouchPouch / TouchStore (a no-op, received here as a signal).
// The number on the pouch box = item count in the pouch; tapping it shows the list (contents read from the sidecar file, still there after re-entering the game; reset to zero once the stash-open transfer finishes).
// Writes only our own widgets (TouchPouch* / TouchStore); 「自定义」(Custom) page switches gem_pick (auto stow runes and gems + pouch box) and store_btn (Deposit button), on by default; when off the whole block is moved off-screen.
// Threads: dm_pouch_step on the game main thread (dm_world.c); UI message callback dm_pouch_on_uimsg (diag.c); stow / release / system notices in the server tick dm_pouch_srv_tick (hud_tick in diag.c);
// persistence in the pre-commit hook pouch_hook on the disk-write thread (start of Commit in hle_fs.c, [2]).
#include "dm_widget.h"
#include "hle.h"
#include "dm_i18n.h"
#include <stdatomic.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

int dm_custom_get(const char *key);
uint64_t dm_guest_frame(void);
int dm_ui_panel_state(void);
void dm_game_notice(uint8_t *main_base, const char *text);
const char *dm_save_workdir(void); uint64_t dm_fs_commit_count(void); uint64_t dm_diag_srv_item(uint32_t id); void *dm_memfind(const uint8_t *h, size_t hn, const uint8_t *n, size_t nn);
int dm_item_write(uint8_t *b, uint64_t it, uint8_t *buf); uint64_t dm_item_make(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out);
int dm_request_save(void); void dm_vault_request_deposit(uint32_t item_id); int dm_vault_put_pending(void); int dm_vault_ready(void); int dm_vault_has(int kind, int idx); int dm_item_quest(uint8_t *b, uint64_t it);
#define PICK_R 3          // 格（同药水自动拾取） | Tiles (same as potion auto pickup)
#define T_RUNE 75         // 原版物品类型号：rune（0x574bc0(物品, 类型) 含父类型；宿主逐号探：一号符文认 0 52 53 75） | Original game item type number: rune (0x574bc0(item, type) includes parent types; probed number by number on the host: the No. 1 rune matches 0 52 53 75)
#define T_GEM 20          // gem：各色宝石与骷髅的父类型（宿主实测：红宝石 gsr、碎紫 gcv、碎骷髅 skc 都认 20） | gem: the parent type of gems of every color and skulls (measured on the host: ruby gsr, chipped amethyst gcv, chipped skull skc all match 20)
#define T_CHARM 13        // charm：小 / 中 / 大护身符（scha 83 / mcha 84 / lcha 85）与碎裂护身符（csch 108）的父类型（ItemTypes.txt 行号，同法算出 gem 20、rune 75 与宿主实探一致） | charm: the parent type of small / large / grand charms (scha 83 / mcha 84 / lcha 85) and the cracked charm (csch 108) (ItemTypes.txt row number; gem 20 and rune 75 computed the same way match what was probed on the host)

static uint8_t *B;
static uint64_t cli_player(void) { return ((uint64_t (*)(uint32_t))(B + 0x45f9d0))(*(uint32_t *)(B + 0x2d20e4c)); }
static int is_rune_gem(uint64_t it) { int (*is)(uint64_t, int) = (int (*)(uint64_t, int))(B + 0x574bc0); return is(it, T_RUNE) || is(it, T_GEM); }
// 材料页（原版高级仓库第三页，每种一个固定格）收的东西里，一键存放管这些：三把钥匙、三件器官、四种精华、赦免之证、世界之石碎片、五种超级先祖召唤材料（ua1–ua5）。
// 不收两种回复药剂 rvs / rvl（腰带和自动补药的货）。只在点「存放」时收：不自动捡、捡进背包也不自动收（符文 / 宝石才那样）
// Of the things the materials page (the third page of the original advanced stash, one fixed cell per kind) accepts, one-tap deposit handles these: the three keys, the three organs, the four essences, Token of Absolution, Worldstone Shard, and the five uber ancient summoning materials (ua1–ua5).
// The two rejuvenation potions rvs / rvl are not taken (they are stock for the belt and for auto-refill). Taken only when 「存放」(Deposit) is tapped: no auto-pickup, and no auto-deposit on entering the inventory either (only runes / gems work that way)
static int is_mat(uint64_t it) { static const char k[][4] = { "pk1", "pk2", "pk3", "dhn", "bey", "mbr", "tes", "ceh", "bet", "fed", "toa", "xa1", "xa2", "xa3", "xa4", "xa5", "ua1", "ua2", "ua3", "ua4", "ua5" };
    uint32_t c = ((uint32_t (*)(uint64_t))(B + 0x570260))(it) & 0xffffff; for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (!memcmp(&c, k[i], 3)) return 1; return 0; }
static int is_charm(uint64_t it) { return ((int (*)(uint64_t, int))(B + 0x574bc0))(it, T_CHARM); }
static uint32_t item_code(uint64_t it) { return ((uint32_t (*)(uint64_t))(B + 0x570260))(it); }
#define AUTO_RUNE_MAX 18   // 自动捡的符文只到 18 号（19 号 Lem 起让玩家手动捡，有仪式感）；宝石全捡 | Auto-picked runes only go up to No. 18 (from No. 19 Lem on, the player picks them up manually, for a sense of ceremony); all gems are picked up
static int auto_ok(uint64_t it) {   // 自动拾取挑哪些：宝石全要；符文按物品码 rNN 的号 | Which ones auto pickup takes: all gems; runes by the number in the item code rNN
    if (!is_rune_gem(it)) return 0;
    if (!((int (*)(uint64_t, int))(B + 0x574bc0))(it, T_RUNE)) return 1;
    uint32_t c = item_code(it); const char *k = (const char *)&c;
    return k[0] == 'r' && k[1] >= '0' && k[1] <= '9' && k[2] >= '0' && k[2] <= '9' && (k[1] - '0') * 10 + (k[2] - '0') <= AUTO_RUNE_MAX;
}
// 玩家背包里有没有这件（客户端：物品栏 +0x10 起的链表，数据 +0xb0 = 下一件；方式 +0xc = 0 在容器里，页 +0x55 = 0 背包）
// Whether the player's inventory contains this item (client: the linked list starting at item container +0x10, data +0xb0 = next item; placement +0xc = 0 in a container, page +0x55 = 0 inventory)
static uint64_t bag_item(uint64_t pl, uint32_t id) {
    uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0;
    for (int k = 0; it && k < 2048; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; if (*(uint32_t *)(it + 8) == id && *(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0) return it; it = *(uint64_t *)(d + 0xb0); }
    return 0;
}
// ---- 百宝囊存储（服务端拍 + 提交前钩子；规则见文件头「落盘」）
// ---- Pouch storage (server tick + pre-commit hook; for the rules see "Persistence" in the file header)
#define UMAX 1024
#define UBYTES 64          // 符文 / 宝石是简单物品，位流很短（宿主实测：十几字节） | Runes / gems are simple items with very short bitstreams (measured on the host: a dozen or so bytes)
typedef struct { uint32_t code; uint16_t len; uint8_t bits[UBYTES]; } uitem;
typedef struct { int put; uitem it; uint16_t plen; uint8_t pb[UBYTES]; uint32_t nid; } upend;   // put：收进囊（证据 pb = 背包里那一串，要「没有」）；否则放到背包（pb = 放好后那一串，要「有」，nid = 新物品 id） | put: stow into the pouch (evidence pb = that bitstream in the inventory, must be "absent"); otherwise release to the inventory (pb = the bitstream after placement, must be "present", nid = new item id)
static uitem g_u[UMAX], g_ud[UMAX]; static int g_nu, g_nud, g_uload; static char g_uchar[64];   // g_u = 囊里（界面看的）；g_ud = 盘上状态（侧文件写它） | g_u = in the pouch (what the UI sees); g_ud = on-disk state (what the sidecar file writes)
static upend g_up[256]; static int g_nup, g_udirty;   // g_udirty：盘上状态 g_ud 变了但侧文件还没写成（上次写失败），下次提交再写 | g_udirty: the on-disk state g_ud changed but the side file has not been written successfully yet (the last write failed); it is written again at the next commit
static uint32_t g_ready[64]; static int g_nready;   // 已证明放进背包、等发 0x63 的新物品 id（钩子写、服务端拍取） | New item ids proven to be in the inventory and waiting for 0x63 to be sent (written by the hook, taken by the server tick)
static pthread_mutex_t g_umu = PTHREAD_MUTEX_INITIALIZER;   // 服务端拍与写盘线程（提交前钩子）都碰；界面读清单也拿它 | Touched by both the server tick and the disk-write thread (pre-commit hook); the UI also takes it when reading the list
static _Atomic int g_ucount, g_ugen;   // 囊里件数 / 内容一变就加一（界面读） | Item count in the pouch / incremented by one whenever the contents change (read by the UI)
static void pouch_path(char *p, size_t n, const char *wd) { snprintf(p, n, "%s/%s/%s.swpouch", wd, g_uchar, g_uchar); }
static void pouch_pub(void) { atomic_store(&g_ucount, g_nu); atomic_fetch_add(&g_ugen, 1); }
static void pouch_load(void) {   // 进局读一次（工作目录里的那份 = 上次提交的）。格式（小端）：'SWPU' u8 版本 1 u8 0 u16 条数；每条 u32 物品码 u16 位流长 + 位流 | Read once on entering a run (the copy in the working directory = the last committed one). Format (little-endian): 'SWPU' u8 version 1 u8 0 u16 entry count; each entry u32 item code u16 bitstream length + bitstream
    g_nu = g_nud = g_nup = g_nready = 0; const char *wd = dm_save_workdir(); if (!wd || !g_uchar[0]) { pouch_pub(); return; }
    char p[1200]; pouch_path(p, sizeof p, wd); FILE *f = fopen(p, "rb");
    if (!f) { LOG("百宝囊：%s 还没有侧文件（空）", g_uchar); pouch_pub(); return; }
    uint8_t h[8]; int bad = 0;
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "SWPU", 4) || h[4] != 1) bad = 1;
    else for (int i = 0, n = *(uint16_t *)(h + 6); i < n; i++) { uint8_t e[6]; if (fread(e, 1, 6, f) != 6) { bad = 1; break; } uint16_t len = *(uint16_t *)(e + 4);
        if (!len || len > UBYTES || g_nu >= UMAX) { bad = 1; break; } uitem *u = &g_u[g_nu]; memcpy(&u->code, e, 4); u->len = len; if (fread(u->bits, 1, len, f) != len) { bad = 1; break; } g_nu++; }
    fclose(f); if (bad) LOG("【红】百宝囊：%s 读到第 %d 条坏了，只用前面读好的（文件不动）", p, g_nu);
    memcpy(g_ud, g_u, sizeof(uitem) * (size_t)g_nu); g_nud = g_nu; pouch_pub(); LOG("百宝囊：%s 读入 %d 件", g_uchar, g_nu);
}
static int pouch_save(const char *wd) {   // 写盘上状态 g_ud（临时文件再改名），只在提交前钩子里调 | Write the on-disk state g_ud (temp file then rename); called only in the pre-commit hook
    { static int nf = -1; if (nf < 0) { const char *e = getenv("DM_TEST_POUCH_SAVE_FAIL"); nf = e ? atoi(e) : 0; } if (nf > 0) { nf--; LOG("【红】百宝囊：测试 DM_TEST_POUCH_SAVE_FAIL 这次当作写失败"); return -1; } }   // 测试开关：前 N 次写当作失败 | test switch: the first N writes are treated as failures
    char p[1200], t[1210]; pouch_path(p, sizeof p, wd); snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "wb"); if (!f) return -1;
    uint8_t h[8] = { 'S', 'W', 'P', 'U', 1, 0, 0, 0 }; *(uint16_t *)(h + 6) = (uint16_t)g_nud; int ok = fwrite(h, 1, 8, f) == 8;
    for (int i = 0; i < g_nud && ok; i++) { uint8_t e[6]; memcpy(e, &g_ud[i].code, 4); *(uint16_t *)(e + 4) = g_ud[i].len; ok = fwrite(e, 1, 6, f) == 6 && fwrite(g_ud[i].bits, 1, g_ud[i].len, f) == g_ud[i].len; }
    ok = fclose(f) == 0 && ok; if (!ok || rename(t, p)) { remove(t); LOG("【红】百宝囊：写 %s 失败", p); return -1; }
    return 0;
}
static int char_blob(const char *wd, dm_blob *o) {   // 本角色档 <角色>/<角色>.d2s 整份读进内存（只比本角色：别的角色同一格放着同种符文，字节会一样） | Read this character's save <character>/<character>.d2s whole into memory (compare against this character only: another character with the same kind of rune in the same cell would have identical bytes)
    char p[1200]; snprintf(p, sizeof p, "%s/%s/%s.d2s", wd, g_uchar, g_uchar); FILE *f = fopen(p, "rb"); o->d = NULL; o->n = 0; if (!f) return 0;
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); o->d = L > 0 && L < (16 << 20) ? malloc((size_t)L) : NULL; o->n = o->d ? fread(o->d, 1, (size_t)L, f) : 0; fclose(f);
    if (!o->n) { free(o->d); o->d = NULL; } return o->n > 0;
}
static void pouch_hook(const char *wd) {   // 提交前钩子（写盘线程，持存档锁） | Pre-commit hook (disk-write thread, holding the save lock)
    pthread_mutex_lock(&g_umu);
    if (g_uload && (g_nup || g_udirty)) {
        dm_blob bl; int hb = char_blob(wd, &bl), done = 0; uint64_t cno = dm_fs_commit_count() + 1;
        if (!hb) LOG("【红】百宝囊：提交前核对 #%llu —— 读不到 %s 的角色档，这次不判", (unsigned long long)cno, g_uchar);
        for (int i = 0; hb && i < g_nup; ) { upend *p = &g_up[i]; int have = dm_memfind(bl.d, bl.n, p->pb, p->plen) != NULL;
            if (p->put == have) { i++; continue; }
            if (p->put) { if (g_nud < UMAX) g_ud[g_nud++] = p->it; else LOG("【红】百宝囊：盘上状态已满 %d 件", UMAX); }
            else { int k = -1; for (int j = 0; j < g_nud && k < 0; j++) if (g_ud[j].len == p->it.len && !memcmp(g_ud[j].bits, p->it.bits, p->it.len)) k = j;
                if (k >= 0) g_ud[k] = g_ud[--g_nud]; else LOG("【红】百宝囊：放到背包的 %.3s 在盘上状态里找不到", (const char *)&p->it.code);
                if (g_nready < 64) g_ready[g_nready++] = p->nid; }
            LOG("百宝囊：提交前核对 #%llu —— %.3s %s → 并进盘上状态", (unsigned long long)cno, (const char *)&p->it.code, p->put ? "收进囊：角色档里已没有背包那一串" : "放到背包：角色档里已有放好的那一串");
            g_up[i] = g_up[--g_nup]; done++; }
        free(bl.d);
        // 侧文件没写成：同编年史（dm_vault.c vault_pre_commit）——这次提交拒绝、盘上状态标脏，服务端拍接着请存盘，写成为止
        // The side file failed to write: same as the Chronicle (dm_vault.c vault_pre_commit) -- this commit is refused, the on-disk state is marked dirty, and the server tick keeps requesting a save until the write succeeds
        if (done || g_udirty) { if (pouch_save(wd) == 0) { LOG("百宝囊：侧文件随这次提交落盘（盘上 %d 件、囊里 %d 件、还有 %d 条待证明）", g_nud, g_nu, g_nup); g_udirty = 0; }
            else { void dm_fs_save_poison(int e); g_udirty = 1; dm_fs_save_poison(EIO); LOG("【红】百宝囊：侧文件没写成 → 这次存档提交拒绝，之后重试（盘上状态 %d 件留在内存）", g_nud); } }
    }
    pthread_mutex_unlock(&g_umu);
}
__attribute__((constructor)) static void pouch_hook_install(void) { extern void (*g_dm_pre_commit_hook[5])(const char *); g_dm_pre_commit_hook[2] = pouch_hook; }

// 收进囊：背包里的这件符文 / 宝石（服务端物品）→ 写位流 → 摘下 → 通知 → 释放；记一条「收」（持 g_umu 调）
// Stow into the pouch: this rune / gem in the inventory (server item) -> write bitstream -> remove -> notify -> free; record one "stow" entry (called holding g_umu)
static int pouch_put(uint8_t *b, uint64_t G, uint64_t player, uint32_t id) {
    uint64_t it = bag_item(player, id), pp = it ? *(uint64_t *)(it + 0x38) : 0;   // 沿玩家自己的物品栏链表找（服务端玩家同一布局）：只收背包（页 0、方式 0）里的符文 / 宝石 | Search along the player's own item container linked list (the server player has the same layout): only stow runes / gems that are in the inventory (page 0, placement 0)
    if (!it || !pp || !(is_rune_gem(it) || is_mat(it))) return -1;
    if (g_nu >= UMAX || g_nup >= 256) { LOG("【红】百宝囊：囊里 %d 件 / 待证明 %d 条已满，这件留在背包", g_nu, g_nup); return -2; }
    uint8_t buf[1024]; int len = dm_item_write(b, it, buf); if (!len || len > UBYTES) { LOG("【红】百宝囊：物品 id %u 位流 %d 字节，不收", id, len); return -3; }
    uitem u = { item_code(it), (uint16_t)len, { 0 } }; memcpy(u.bits, buf, (size_t)len); int x = *(int32_t *)(pp + 0x10), y = *(int32_t *)(pp + 0x14);
    ((void (*)(uint64_t))(b + 0x55e430))(it);
    ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, int))(b + 0x642f70))(G, player, it, 0x20, 4, 0, 0, y, x);
    ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, it);
    g_u[g_nu++] = u; upend *p = &g_up[g_nup++]; memset(p, 0, sizeof *p); p->put = 1; p->it = u; p->plen = (uint16_t)len; memcpy(p->pb, buf, (size_t)len);
    pouch_pub(); LOG("百宝囊：收进 %.3s（物品 id %u，背包格 %d,%d，位流 %d 字节）→ 囊里 %d 件", (const char *)&u.code, id, x, y, len, g_nu);
    return 1;
}
// 放到背包：囊里最后一件建出来、放进背包空位；记一条「放」。背包放不下返回 0（持 g_umu 调）
// Release to the inventory: build the last item in the pouch and put it in a free inventory spot; record one "release" entry. Returns 0 if the inventory has no room (called holding g_umu)
static int pouch_take(uint8_t *b, uint64_t G, uint64_t player) {
    if (!g_nu || g_nup >= 256) return 0;
    uitem u = g_u[g_nu - 1]; uint64_t pp = *(uint64_t *)(player + 0x38), room = pp ? *(uint64_t *)(pp + 0x20) : 0; if (!room) return 0;
    uint64_t nu = dm_item_make(b, G, room, u.bits, u.len, NULL); if (!nu) { LOG("【红】百宝囊：%.3s 位流建不出物品，留在囊里", (const char *)&u.code); return 0; }
    uint64_t inv = *(uint64_t *)(player + 0x90); uint32_t cls = *(uint32_t *)(player + 4), fx = 0, fy = 0;   // 原版在格区找空位（同编年史取出） | The original game finds a free spot in the grid (same as chronicle retrieve)
    if (!inv || cls > 7 || !((int (*)(uint64_t, uint64_t, int, uint32_t *, uint32_t *, int))(b + 0x55f620))(inv, nu, *(int32_t *)(b + 0x1dae684 + cls * 8 + 4), &fx, &fy, 0)) {
        ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, nu); return 0; }
    uint64_t nd = *(uint64_t *)(nu + 0x10), ip = *(uint64_t *)(nu + 0x38); if (!nd || !ip) { ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, nu); return 0; }
    *(uint8_t *)(nd + 0x55) = 0; *(int32_t *)(ip + 0x10) = (int32_t)fx; *(int32_t *)(ip + 0x14) = (int32_t)fy;
    uint64_t one = nu, ox = 0; ((int (*)(uint64_t, uint64_t, uint64_t *, uint64_t *))(b + 0x6f1940))(G, player, &one, &ox);
    if (!*(uint64_t *)(nd + 0xa0)) { ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, nu); LOG("【红】百宝囊：%.3s 放进背包没成（格 %u,%u），留在囊里", (const char *)&u.code, fx, fy); return 0; }
    ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, uint64_t))(b + 0x642f70))(G, player, nu, 1, 2, 0, 0, 0, (uint64_t)(fx & 0xffff) | (uint64_t)fy << 16);
    uint8_t buf[1024]; int len = dm_item_write(b, nu, buf);
    upend *p = &g_up[g_nup++]; memset(p, 0, sizeof *p); p->put = 0; p->it = u; p->nid = *(uint32_t *)(nu + 8);
    if (len > 0 && len <= UBYTES) { p->plen = (uint16_t)len; memcpy(p->pb, buf, (size_t)len); } else { p->plen = u.len; memcpy(p->pb, u.bits, u.len); LOG("【红】百宝囊：放好后的位流 %d 字节，用囊里那串作证据", len); }
    g_nu--; pouch_pub(); LOG("百宝囊：%.3s 放到背包格 %u,%u（新 id %u）→ 囊里还剩 %d 件，等落盘证明后进高级仓库", (const char *)&u.code, fx, fy, p->nid, g_nu);
    return 1;
}
static uint32_t adv_stash_unit(uint64_t pl) {   // 玩家自己的高级仓库单位 id：物品栏 +0x68 附属单位表（+0 单位 id、+0xa 类，+0x10 下一个），类 1 = 高级仓库（同 dm_vault.c item_dump；客户端 / 服务端玩家同一布局） | The player's own advanced stash unit id: the attached unit table at item container +0x68 (+0 unit id, +0xa class, +0x10 next), class 1 = advanced stash (same as item_dump in dm_vault.c; client / server players share the same layout)
    uint64_t inv = *(uint64_t *)(pl + 0x90); int k = 0;
    for (uint64_t en = inv ? *(uint64_t *)(inv + 0x68) : 0; en && k < 32; en = *(uint64_t *)(en + 0x10), k++) if (*(uint8_t *)(en + 0xa) == 1) return *(uint32_t *)en;
    return 0;
}
static void move_to_stash(uint8_t *b, uint64_t G, uint64_t player, uint32_t id, uint32_t unit) {   // 原版「移到仓库」0x63：服务端代发，照收包函数 0x632510 的做法调用期间 [G+0x19050] = 1（同 diag.c DM_TEST_GIVE_INV） | The original game's "move to stash" 0x63: sent by the server on the player's behalf; following what the packet receive function 0x632510 does, [G+0x19050] = 1 during the call (same as DM_TEST_GIVE_INV in diag.c)
    uint8_t pk[25] = { 0x63 }; memcpy(pk + 5, &id, 4); memcpy(pk + 9, &unit, 4);
    *(uint32_t *)(G + 0x19050) = 1; ((void (*)(uint64_t, uint64_t, uint8_t *, uint32_t))(b + 0x6e5b40))(G, player, pk, 25); *(uint32_t *)(G + 0x19050) = 0;
}

// ---- 仓库页里新落下的符文 / 宝石 / 材料自动转进高级仓库（珍宝匣）：玩家把它们拖进个人页 / 共享页的格子、或按住确认键移到当前页，落下后由这里代发原版「移到仓库」。
// 不管是哪个入口放进去的都认（只看结果：这件现在在个人页的格子里）。开箱那一刻已经在个人页里的不动（玩家以前自己摆的）；发了 2 秒没进去（那一格叠满）的留在原处、记下不再发。
// 只管个人页（玩家物品栏里方式 0、页 4 的物品）。共享页不管：原版「移到仓库」不收来源在共享页的物品（实测：发了 2 秒还在原格），
// 共享页的物品属于另一个仓库单位、不在玩家自己的物品栏里；要转得先把它挪回玩家名下，没有现成的安全做法。unit = 0（箱子没开 / 不在旁）时清状态
// ---- Runes / gems / materials newly dropped into a stash page are moved into the advanced stash tabs (Trove) automatically: the player drags them into a personal / shared tab cell, or holds the confirm button to move them to the current tab, and once they land this sends the game's "move to stash" for them.
// Whichever entry put them there counts (only the result matters: the item is now in a personal tab cell). Items already in the personal tab when the stash was opened are left alone (the player placed them earlier); items not gone 2 seconds after sending (that stack is full) stay put and are marked not to send again.
// Personal tab only (items in the player inventory with mode 0, page 4). The shared tab is not handled: the game's "move to stash" does not accept items whose source is the shared tab (measured: still in its cell 2 seconds after sending),
// shared tab items belong to a separate stash unit and are not in the player's own inventory; moving them would first require putting them back under the player, for which there is no safe existing way. State is cleared when unit = 0 (stash not open / not nearby)
static int stash_sweep(uint8_t *b, uint64_t G, uint64_t player, uint32_t unit, uint32_t tq, int *saved) {
    static uint32_t old[768]; static int nold, was_open; typedef struct { uint32_t id, t; } dm_t; static dm_t dm[16]; static int nsw;
    if (!unit) { was_open = 0; nsw = 0; return 0; }
    uint32_t cand[256]; int nc = 0; uint64_t inv = *(uint64_t *)(player + 0x90);
    for (uint64_t it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0, k = 0; it && k < 2048 && nc < 256; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 4 && (is_rune_gem(it) || is_mat(it))) cand[nc++] = *(uint32_t *)(it + 8); it = *(uint64_t *)(d + 0xb0); }
    // 测试开关 DM_TEST_STACK99=<物品码>（造「那一格叠满」的现场）：开箱时把高级仓库里这种物品那一叠的叠数（物品数据 +0x9c）写成 99
    // Test switch DM_TEST_STACK99=<item code> (to stage "that stack is full"): on stash open write 99 into the stack count (item data +0x9c) of this item's stack in the advanced stash tabs
    if (!was_open) { const char *e = getenv("DM_TEST_STACK99");
        for (uint64_t en = e && inv ? *(uint64_t *)(inv + 0x68) : 0, n = 0; en && n < 16; en = *(uint64_t *)(en + 0x10), n++) { if (*(uint8_t *)(en + 0xa) != 1) continue;
            uint32_t i = *(uint32_t *)en; uint64_t u = *(uint64_t *)(G + 0x2230 + 8 * (i & 0x7f)); while (u && *(uint32_t *)(u + 8) != i) u = *(uint64_t *)(u + 0x158);
            uint64_t iv = u ? *(uint64_t *)(u + 0x90) : 0; if (!iv || *(uint32_t *)iv != 0x01020304) continue;
            for (uint64_t it = *(uint64_t *)(iv + 0x10), k = 0; it && k < 2048; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; uint32_t c = item_code(it);
                if (!memcmp(&c, e, strlen(e))) { LOG("测试 DM_TEST_STACK99：高级仓库里 %s 的叠数 %u → 99", e, *(uint8_t *)(d + 0x9c)); *(uint8_t *)(d + 0x9c) = 99; } it = *(uint64_t *)(d + 0xb0); } } }
    if (!was_open) { was_open = 1; nold = 0; for (int i = 0; i < nc && nold < 768; i++) old[nold++] = cand[i]; if (nc) LOG("珍宝匣：开箱时个人页里已有符文 / 宝石 / 材料 %d 件（不动）", nc); return 0; }
    int full = 0;
    for (int i = 0; i < nsw; ) { int still = 0; for (int k = 0; k < nc; k++) still |= cand[k] == dm[i].id;   // 发过的：不在仓库页了 = 进了高级仓库 | already sent: no longer in the stash page = it went into the advanced stash tabs
        if (!still) { LOG("珍宝匣：物品 id %u 已从仓库页转进高级仓库", dm[i].id); *saved = 1; dm[i] = dm[--nsw]; continue; }
        if (tq - dm[i].t >= 50) { if (nold < 768) old[nold++] = dm[i].id; LOG("珍宝匣：物品 id %u 发了移到仓库 2 秒还在仓库页（那一格叠满 / 不收）→ 留在原处，不再发", dm[i].id); full = 1; dm[i] = dm[--nsw]; continue; }
        i++; }
    for (int k = 0; k < nc && nsw < 16; k++) { int skip = 0; for (int i = 0; i < nold && !skip; i++) skip = old[i] == cand[k]; for (int i = 0; i < nsw && !skip; i++) skip = dm[i].id == cand[k];
        if (skip) continue; move_to_stash(b, G, player, cand[k], unit); dm[nsw++] = (dm_t){ cand[k], tq }; LOG("珍宝匣：仓库页里新落下的物品 id %u → 原版移到仓库（高级仓库单位 %u）", cand[k], unit); break; }   // 一拍发一件 | send one item per tick
    return full;
}

// ---- 拖进共享页：在服务端收包处拦（diag.c 的命令分派观测门，分发之前）。落进共享页之后就搬不动了——原版「移到仓库」不收来源在共享页的物品（实测），
// 所以趁物品还在玩家手上时改道：客户端发来「放进共享页格」0x45 [物品 id][共享页单位 id][x u16][y u16]（13 字节）时，这件是符文 / 宝石 / 材料、人在箱子旁、
// 高级仓库里它那一格没叠满 → 把这条包换成「放回背包原来那一格」0x18 [物品 id][x][y][页 0]（17 字节；原格取自刚才那条「从背包拿起」0x19，一定空着），
// 下一拍再对它代发原版「移到仓库」（背包来源，线上一直在用的那条路）。物品从没进过共享页单位。叠满 → 不改道，照玩家的意思落在他拖到的共享页格，提示一次。
// 三种包的字段是这台机器上拖放时实录的（DM_C2S_LOG）。和服务端拍同一个线程（都在服务端），所以待发清单不用加锁
// ---- Drag into the shared tab: intercepted where the server receives the packet (the command dispatch observation gate in diag.c, before dispatch). Once in the shared tab it cannot be moved: the game's "move to stash" does not accept items whose source is the shared tab (measured),
// so reroute while the item is still on the player's cursor: when the client sends "place into shared tab cell" 0x45 [item id][shared tab unit id][x u16][y u16] (13 bytes) and the item is a rune / gem / material, the player is at the stash, and
// its stack in the advanced stash tabs is not full → replace the packet with "put back into the original inventory cell" 0x18 [item id][x][y][page 0] (17 bytes; the cell comes from the preceding "pick up from inventory" 0x19 and is guaranteed empty),
// then on the next tick send the game's "move to stash" for it (inventory source, the path already used in production). The item never enters the shared tab unit. Stack full → no reroute; it lands in the shared tab cell the player dragged it to, with one notice.
// The fields of the three packets were recorded on this machine during drag and drop (DM_C2S_LOG). Same thread as the server tick (both on the server), so the pending list needs no lock
static uint32_t g_pick_id, g_pick_x, g_pick_y; static uint32_t g_c2s_q[8]; static int g_c2s_nq, g_c2s_full;
static int adv_stack_full(uint64_t G, uint64_t player, uint32_t code) {   // 高级仓库里这种物品那一叠是不是已经 99 | is the stack of this item in the advanced stash tabs already at 99
    uint64_t inv = *(uint64_t *)(player + 0x90);
    for (uint64_t en = inv ? *(uint64_t *)(inv + 0x68) : 0, n = 0; en && n < 16; en = *(uint64_t *)(en + 0x10), n++) { if (*(uint8_t *)(en + 0xa) != 1) continue;
        uint32_t i = *(uint32_t *)en; uint64_t u = *(uint64_t *)(G + 0x2230 + 8 * (i & 0x7f)); while (u && *(uint32_t *)(u + 8) != i) u = *(uint64_t *)(u + 0x158);
        uint64_t iv = u ? *(uint64_t *)(u + 0x90) : 0; if (!iv || *(uint32_t *)iv != 0x01020304) continue;
        for (uint64_t it = *(uint64_t *)(iv + 0x10), k = 0; it && k < 2048; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
            if (item_code(it) == code && *(uint8_t *)(d + 0x9c) >= 99) return 1; it = *(uint64_t *)(d + 0xb0); } }
    return 0;
}
void dm_pouch_c2s(uint64_t *regs) {   // regs：x0 游戏、x1 玩家、x2 包、x3 长度（可改） | regs: x0 game, x1 player, x2 packet, x3 length (writable)
    uint64_t G = regs[0], player = regs[1]; const uint8_t *pk = (const uint8_t *)(uintptr_t)regs[2]; uint32_t len = (uint32_t)regs[3]; if (!B || !G || !player || !pk) return;
    if (len == 17 && pk[0] == 0x19) { uint32_t pg; memcpy(&g_pick_id, pk + 1, 4); memcpy(&g_pick_x, pk + 5, 4); memcpy(&g_pick_y, pk + 9, 4); memcpy(&pg, pk + 13, 4); if (pg) g_pick_id = 0; return; }   // 只记从背包（页 0）拿起的 | only record pickups from the inventory (page 0)
    // 手柄「按住确认键移到当前页」（触屏双击不可成叠物品也是它）：物品还在背包里、不经手上——个人页 0x54 [物品 id]…（21 字节）、共享页 0x55 [物品 id][共享页单位]…（29 字节），实录。
    // 三类物品走这条时直接把整条包换成原版「移到仓库」0x63（背包来源）；叠满就不换，照原样落到玩家选的那一页（共享页的在这里提示，个人页的由 stash_sweep 发一次没进去后提示）
    // Gamepad "hold the confirm button to move to the current page" (a touch double-tap on a non-stackable item is the same thing): the item stays in the backpack and never goes onto the cursor -- personal page 0x54 [item id]... (21 bytes), shared page 0x55 [item id][shared page unit]... (29 bytes), as recorded.
    // When one of the three item kinds takes this path, the whole packet is replaced with the original "move to stash" 0x63 (backpack source); when the stack is full it is not replaced and lands on the page the player chose as usual (the shared page case shows a hint here, the personal page case is hinted after stash_sweep sends once and it does not go in)
    if ((len == 21 && pk[0] == 0x54) || (len == 29 && pk[0] == 0x55)) { uint32_t id; memcpy(&id, pk + 1, 4);
        uint64_t dm_diag_srv_item(uint32_t); uint64_t it = dm_diag_srv_item(id); if (!it || !bag_item(player, id) || !(is_rune_gem(it) || is_mat(it))) return;
        uint32_t unit = adv_stash_unit(player); int near = ((int (*)(uint64_t, uint64_t, int, int))(B + 0x650d10))(G, player, 0x10b, 0x32) & 1; if (!unit || !near) return;
        uint32_t code = item_code(it);
        if (adv_stack_full(G, player, code)) { if (pk[0] == 0x55) g_c2s_full = 1; LOG("珍宝匣：%.3s（id %u）按住确认键移到%s页，高级仓库那一格已叠满 → 照原样移过去", (const char *)&code, id, pk[0] == 0x55 ? "共享" : "个人"); return; }
        static uint8_t st[25]; memset(st, 0, sizeof st); st[0] = 0x63; memcpy(st + 5, &id, 4); memcpy(st + 9, &unit, 4); regs[2] = (uint64_t)(uintptr_t)st; regs[3] = 25;
        LOG("珍宝匣：%.3s（id %u）按住确认键移到%s页 → 改成原版移到仓库（高级仓库单位 %u）", (const char *)&code, id, pk[0] == 0x55 ? "共享" : "个人", unit); return; }
    if (len != 13 || pk[0] != 0x45) return;
    uint32_t id; memcpy(&id, pk + 1, 4); if (!id || id != g_pick_id) return;   // 不是刚从背包拿起的那件（比如从别的页拖过来的）：不管 | not the item just picked up from the inventory (e.g. dragged from another page): ignore
    uint64_t dm_diag_srv_item(uint32_t); uint64_t it = dm_diag_srv_item(id); if (!it || !(is_rune_gem(it) || is_mat(it))) return;
    uint32_t unit = adv_stash_unit(player); int near = ((int (*)(uint64_t, uint64_t, int, int))(B + 0x650d10))(G, player, 0x10b, 0x32) & 1; if (!unit || !near || g_c2s_nq >= 8) return;
    uint32_t code = item_code(it);
    if (adv_stack_full(G, player, code)) { g_c2s_full = 1; LOG("珍宝匣：%.3s（id %u）拖进共享页，高级仓库那一格已叠满 → 照原样落在共享页那格，提示一次", (const char *)&code, id); return; }
    static uint8_t alt[17]; uint32_t zero = 0; alt[0] = 0x18; memcpy(alt + 1, &id, 4); memcpy(alt + 5, &g_pick_x, 4); memcpy(alt + 9, &g_pick_y, 4); memcpy(alt + 13, &zero, 4);
    regs[2] = (uint64_t)(uintptr_t)alt; regs[3] = 17; g_c2s_q[g_c2s_nq++] = id; g_pick_id = 0;
    LOG("珍宝匣：%.3s（id %u）拖进共享页 → 改成放回背包格 %u,%u，下一拍代发原版移到仓库", (const char *)&code, id, g_pick_x, g_pick_y);
}

// ---- 主线程 → 服务端拍：要收的物品 id（单写单读环）、储藏箱开着、在不在储藏箱旁
// ---- Main thread -> server tick: item ids to stow (single-writer single-reader ring), stash open, whether next to the stash
static _Atomic uint32_t g_absq[64]; static _Atomic unsigned g_absw, g_absr;
static _Atomic int g_stash_open, g_near_pub;
static void absorb_push(uint32_t id) { unsigned w = atomic_load(&g_absw); if (w - atomic_load(&g_absr) >= 64) return; atomic_store(&g_absq[w % 64], id); atomic_store(&g_absw, w + 1); }

// ---- 自动收：地上的符文 / 宝石记下 id → 自动捡（18 号以内符文、全部宝石，PICK_R 格内）→ 凡是记过的 id 进了背包就交服务端收
// ---- Auto stow: record the ids of runes / gems on the ground -> auto pick up (runes up to No. 18, all gems, within PICK_R tiles) -> any recorded id that enters the inventory is handed to the server to stow
typedef struct { uint32_t id, code; uint64_t fr; int st; } pick;   // st 1 = 发了捡起包，2 = 交服务端收了 | st 1 = pick-up packet sent, 2 = handed to the server to stow
static pick g_pk[16];
static uint32_t g_gnd[128]; static int g_ngnd;   // 在地上见过的符文 / 宝石 id（环形）：玩家自己捡起来的也认；从仓库取出来的不在这里，不收 | Ids of runes / gems seen on the ground (ring buffer): those the player picks up themselves are recognized too; those taken out of the stash are not in here and are not stowed
static int gnd_has(uint32_t id) { for (int i = 0; i < 128; i++) if (g_gnd[i] == id) return 1; return 0; }
static void gnd_add(uint32_t id) { if (!gnd_has(id)) g_gnd[g_ngnd++ % 128] = id; }
static void gnd_del(uint32_t id) { for (int i = 0; i < 128; i++) if (g_gnd[i] == id) g_gnd[i] = 0; }
static void auto_pick(uint64_t pl, uint64_t fr) {
    if (!adv_stash_unit(pl)) return;   // 没有高级仓库的角色（毁灭之王类型只有 3 页共享、经典类型都没有，宿主逐个载入存档实测）：不捡、不收 | Characters without an advanced stash (the Lord of Destruction type has only 3 shared pages, the classic type has none; measured on the host by loading saves one by one): no pickup, no stow
    for (int i = 0; i < 16; i++) { pick *p = &g_pk[i]; if (!p->st) continue; uint64_t it = bag_item(pl, p->id);
        if (p->st == 1) { if (!it && fr - p->fr > 120) { p->st = 0; LOG("百宝囊：%.3s（id %u）2 秒没捡进背包（背包满？），不再试", (const char *)&p->code, p->id); } }
        else if (!it) p->st = 0;
        else if (fr - p->fr > 120) { p->st = 0; LOG("【红】百宝囊：%.3s（id %u）交服务端 2 秒还在背包", (const char *)&p->code, p->id); } }
    static uint64_t next; if (fr < next) return; next = fr + 10;
    {   // 背包里有在地上见过的符文 / 宝石 → 交服务端收（储藏箱旁直接进高级仓库，别处进囊） | The inventory contains a rune / gem that was seen on the ground -> hand it to the server to stow (next to the stash straight into the advanced stash, elsewhere into the pouch)
        uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0;
        for (int k = 0; it && k < 2048; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; uint32_t id = *(uint32_t *)(it + 8);
            if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && gnd_has(id) && is_rune_gem(it)) {
                int slot = -1; for (int i = 0; i < 16; i++) { if (g_pk[i].st == 2 && g_pk[i].id == id) { slot = -2; break; } if (slot == -1 && (!g_pk[i].st || g_pk[i].id == id)) slot = i; }
                if (slot >= 0) { int mine = g_pk[slot].st == 1 && g_pk[slot].id == id; g_pk[slot] = (pick){ id, item_code(it), fr, 2 }; gnd_del(id); absorb_push(id);
                    LOG("百宝囊：%.3s（id %u）%s进了背包 → 交服务端收（%s）", (const char *)&g_pk[slot].code, id, mine ? "" : "玩家自己捡", atomic_load(&g_near_pub) ? "储藏箱旁：进高级仓库" : "进百宝囊"); return; } }
            it = *(uint64_t *)(d + 0xb0); } }
    uint64_t pp = *(uint64_t *)(pl + 0x38); if (!pp) return; int px = *(uint16_t *)(pp + 2), py = *(uint16_t *)(pp + 6);
    // 测试开关 DM_TEST_PICK=<帧>:<物品码>（造现场用）：到帧替「玩家自己」捡地上这种物品一件——客户端发原版捡起包 0x16，同玩家点地面；不进 g_pk，验「玩家自己捡的也收」
    // Test switch DM_TEST_PICK=<frame>:<item code> (for test setup): at that frame, pick up one item of this kind from the ground on behalf of "the player themselves" -- the client sends the original pick-up packet 0x16, same as the player tapping the ground; it does not enter g_pk, verifying "items the player picks up themselves are stowed too"
    { static int at = -1; static char code[5]; if (at < 0) { const char *e = getenv("DM_TEST_PICK"); at = e && sscanf(e, "%d:%4s", &at, code) == 2 ? at : 0; }
      if (at > 0 && fr >= (uint64_t)at) for (int k = 0; k < 128 && at > 0; k++) for (uint64_t it = *(uint64_t *)(B + 0x2d603a0 + 4 * 0x400 + k * 8); it; it = *(uint64_t *)(it + 0x158)) {
          uint32_t c = item_code(it); uint64_t ip = *(uint64_t *)(it + 0x38); if (*(uint32_t *)(it + 0xc) != 3 || !ip || memcmp(&c, code, strlen(code))) continue;
          uint32_t id = *(uint32_t *)(it + 8), one = 1; int x = *(int32_t *)(ip + 0x10), y = *(int32_t *)(ip + 0x14); uint8_t pk[17] = { 0x16 }; memcpy(pk + 1, &id, 4); memcpy(pk + 5, &x, 4); memcpy(pk + 9, &y, 4); memcpy(pk + 13, &one, 4);
          ((void (*)(const uint8_t *, int))(B + 0x2ceab0))(pk, 17); LOG("测试 DM_TEST_PICK：帧 %llu 替玩家捡地上 %s（id %u）", (unsigned long long)fr, code, id); at = 0; break; } }
    uint32_t want = 0, wcode = 0; int wx = 0, wy = 0;
    for (int k = 0; k < 128; k++) for (uint64_t it = *(uint64_t *)(B + 0x2d603a0 + 4 * 0x400 + k * 8); it; it = *(uint64_t *)(it + 0x158)) {
        if (*(uint32_t *)(it + 0xc) != 3 || !is_rune_gem(it)) continue;   // 地上的符文 / 宝石：都记下 | Runes / gems on the ground: record them all
        uint32_t id = *(uint32_t *)(it + 8); gnd_add(id);
        uint64_t ip = *(uint64_t *)(it + 0x38); if (want || !ip) continue; int x = *(int32_t *)(ip + 0x10), y = *(int32_t *)(ip + 0x14);
        if (abs(x - px) > PICK_R || abs(y - py) > PICK_R || !auto_ok(it)) continue;
        { int dm_lootfilter_hidden(uint8_t *, uint64_t); if (dm_lootfilter_hidden(B, it)) continue; }   // 被原版掉落过滤藏着的不自动捡（现行规则不藏符文宝石，守的是以后改规则） | items hidden by the game's loot filter are not auto-picked (current rules do not hide runes or gems; this guards future rule changes)
        int seen = 0; for (int i = 0; i < 16; i++) if (g_pk[i].id == id && (g_pk[i].st || fr - g_pk[i].fr < 120)) seen = 1;
        if (!seen) { want = id; wx = x; wy = y; wcode = item_code(it); }
    }
    if (!want) return;
    int slot = -1; for (int i = 0; i < 16 && slot < 0; i++) if (!g_pk[i].st) slot = i; if (slot < 0) return;
    g_pk[slot] = (pick){ want, wcode, fr, 1 };
    uint8_t pk[17] = { 0x16 }; uint32_t one = 1; memcpy(pk + 1, &want, 4); memcpy(pk + 5, &wx, 4); memcpy(pk + 9, &wy, 4); memcpy(pk + 13, &one, 4);
    ((void (*)(const uint8_t *, int))(B + 0x2ceab0))(pk, 17);
    LOG("百宝囊：地上 %.3s（id %u）在 %d,%d（玩家 %d,%d）→ 发捡起包", (const char *)&wcode, want, wx, wy, px, py);
}

// ---- 存放：一件一件做
// ---- Deposit: done one item at a time
typedef struct { uint32_t id, code; uint8_t to_vault, page; } sitem;   // page：这件原来在哪一页（0 背包 / 3 魔盒） | page: which page this item was originally on (0 inventory / 3 Horadric Cube)
static sitem g_st[128]; static int g_nst, g_ist = -1, g_sv, g_sg, g_sp, g_sleft, g_snear; static uint64_t g_sfr;
static _Atomic int g_store_req, g_pouch_tap;
static char g_notice[160]; static _Atomic int g_notice_req;
// 这件还在不在原来那一页（客户端物品栏；方式 0 = 在容器里）
// Whether this item is still on its original page (client inventory; mode 0 = in a container)
static uint64_t page_item(uint64_t pl, uint32_t id, int page) {
    uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0;
    for (int k = 0; it && k < 2048; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; if (*(uint32_t *)(it + 8) == id && *(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == page) return it; it = *(uint64_t *)(d + 0xb0); }
    return 0;
}
static void store_begin(uint64_t pl) {
    int ncube = 0;
    g_nst = 0; g_sv = g_sg = g_sp = g_sleft = 0; int vault = dm_vault_ready(), adv = adv_stash_unit(pl) != 0; g_snear = atomic_load(&g_near_pub);
    uint64_t inv = *(uint64_t *)(pl + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0;
    for (int k = 0; it && k < 2048 && g_nst < 128; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        // 来源页：背包（页 0）全套规则；魔盒（页 3）只搬已鉴定的暗金 / 套装进编年史——魔盒里的符文 / 宝石不动：
        // 常是正要合成的材料，而且原版「移到仓库」不收来源在魔盒里的物品
        // Source page: the inventory (page 0) gets the full rule set; the Horadric Cube (page 3) only moves identified unique / set items into the Chronicle.
        // Runes / gems in the Cube are left alone:
        // they are usually materials about to be combined, and the original "move to stash" does not accept items whose source is the Cube
        int page = *(uint32_t *)(it + 0xc) == 0 ? *(uint8_t *)(d + 0x55) : -1;
        if (*(uint32_t *)d == 7 || *(uint32_t *)d == 5) LOG("存放：暗金 / 套装件 id %u 方式 %u 页 %u 已鉴定 %d（收的是方式 0、背包页 0 或魔盒页 3、已鉴定的）", *(uint32_t *)(it + 8), *(uint32_t *)(it + 0xc), *(uint8_t *)(d + 0x55), (*(uint32_t *)(d + 0x18) & 0x10) != 0);
        if (page == 0 || page == 3) { uint32_t q = *(uint32_t *)d, id = *(uint32_t *)(it + 8);
            if (is_rune_gem(it) || is_mat(it)) { if (adv && page == 0) g_st[g_nst++] = (sitem){ id, item_code(it), 0, 0 }; }   // 没有高级仓库：符文宝石不动、也不算「留在背包」 | No advanced stash: runes and gems are not touched and do not count as "left in the inventory" either
            else if ((q == 7 || q == 5) && (*(uint32_t *)(d + 0x18) & 0x10) && !is_charm(it) && !dm_item_quest(B, it)) {   // 已鉴定的暗金 / 套装件（同 dm_vault.c item_key）；护身符（常驻背包）与任务物品（编年史不收）不搬、也不算「留在背包」 | Identified unique / set items (same as item_key in dm_vault.c); charms (kept permanently in the inventory) and quest items (not accepted by the chronicle) are not moved and do not count as "left in the inventory" either
                uint32_t key = (uint32_t)(q == 7) << 16 | (*(uint32_t *)(d + 0x34) & 0xffff); int dup = 0;   // 这一批里已有同一款（背包一件、魔盒又一件）：只存头一件，后一件不动——不然后一件会把前一件从编年史换回背包 | The same item is already in this batch (one in the inventory, another in the Horadric Cube): store only the first and leave the second alone, otherwise the second would swap the first back from the Chronicle into the inventory
                for (int i = 0; i < g_nst; i++) if (g_st[i].to_vault && g_st[i].code == key) dup = 1;
                if (vault && !dup && !dm_vault_has(q == 7, (int)*(uint32_t *)(d + 0x34))) { g_st[g_nst++] = (sitem){ id, key, 1, (uint8_t)page }; ncube += page == 3; } else if (page == 0) g_sleft++; } }
        it = *(uint64_t *)(d + 0xb0); }
    g_ist = 0; g_sfr = 0; LOG("存放：背包里 %d 件、魔盒里 %d 件要存（编年史里已有同件 / 没有编年史仓库的 %d 件不动；符文宝石%s）", g_nst - ncube, ncube, g_sleft, g_snear ? "在储藏箱旁，进高级仓库" : "进百宝囊");
}
static void store_step(uint64_t pl, uint64_t fr) {
    if (g_ist < 0) return;
    if (g_ist < g_nst) { sitem *s = &g_st[g_ist]; uint64_t it = page_item(pl, s->id, s->page);
        if (!g_sfr) { if (!it) { g_ist++; return; }   // 已经不在背包（玩家自己挪了） | No longer in the inventory (the player moved it)
            if (s->to_vault) { if (dm_vault_put_pending()) return; dm_vault_request_deposit(s->id); } else absorb_push(s->id);
            g_sfr = fr; return; }
        if (!it) { if (s->to_vault) g_sv++; else if (g_snear) g_sg++; else g_sp++; g_ist++; g_sfr = 0; return; }
        if (fr - g_sfr > (!s->to_vault && g_snear ? 240 : 120)) {   // 储藏箱旁：服务端等 2 秒没进仓库才收回囊里，这边多等一会儿，别抢在它前面判「留在背包」 | next to the stash: the server waits 2 seconds and only takes it back into the pouch if it has not entered the stash, so wait a little longer here and do not judge "stays in the inventory" ahead of it
            g_sleft++; LOG("【红】存放：物品 id %u 2 秒还在%s（%s不收）", s->id, s->page == 3 ? "魔盒" : "背包", s->to_vault ? "编年史" : g_snear ? "储藏箱" : "百宝囊"); g_ist++; g_sfr = 0; }
        return; }
    {   // 「存放：编年史 N 件，储藏箱 M 件，百宝囊 K 件」，为 0 的那类不写；一件没存也提示。「存放」「编年史」「储藏箱」是原版字符串（strGoldDeposit / Chronicle / 储藏箱标题） | 「存放：编年史 N 件，储藏箱 M 件，百宝囊 K 件」(Deposited: Chronicle N, Stash M, Pouch K); categories that are 0 are omitted; a notice is shown even if nothing was deposited. 「存放」(Deposit), 「编年史」(Chronicle) and 「储藏箱」(Stash) are original game strings (strGoldDeposit / Chronicle / the stash title)
        int k = snprintf(g_notice, sizeof g_notice, "%s", dm_str(B, "dmStoreHead")), any = 0;
        if (g_sv) { k += snprintf(g_notice + k, sizeof g_notice - k, dm_str(B, "dmStoreVault"), g_sv); any = 1; }
        if (g_sg) { k += snprintf(g_notice + k, sizeof g_notice - k, dm_str(B, "dmStoreStash"), any ? dm_str(B, "dmStoreSep") : "", g_sg); any = 1; }
        if (g_sp) { k += snprintf(g_notice + k, sizeof g_notice - k, dm_str(B, "dmStorePouch"), any ? dm_str(B, "dmStoreSep") : "", g_sp); any = 1; }
        if (!any) k += snprintf(g_notice + k, sizeof g_notice - k, "%s", dm_str(B, "dmStoreNone"));
        else if (g_sleft) snprintf(g_notice + k, sizeof g_notice - k, dm_str(B, "dmStoreLeft"), g_sleft); }
    int sv = g_sv + g_sg ? dm_request_save() : 0;   // 存放完请游戏存一次盘（同编年史存取）；进囊的由服务端拍自己请 | After depositing, ask the game to save once (same as chronicle store / retrieve); for items going into the pouch the server tick requests it itself
    atomic_store(&g_notice_req, 1); LOG("存放：完成——编年史 %d、储藏箱 %d、百宝囊 %d、留在背包 %d → 提示「%s」；请存盘 → %d", g_sv, g_sg, g_sp, g_sleft, g_notice, sv); g_ist = -1;
}

// ---- 界面（只写我们自己的控件）
// ---- UI (writes only our own widgets)
static uint64_t find_w(uint64_t w, const char *name, int dep) {
    if (!w || dep > 14) return 0; const char *nm = (const char *)*(uint64_t *)(w + 8); if (nm && !strcmp(nm, name)) return w;
    uint64_t ch = *(uint64_t *)(w + 0x58), n = *(uint64_t *)(w + 0x60); if (!ch || n > 256) return 0;
    for (uint64_t i = 0; i < n; i++) { uint64_t r = find_w(*(uint64_t *)(ch + 8 * i), name, dep + 1); if (r) return r; }
    return 0;
}
static int ours(uint64_t w) { const char *nm = w ? (const char *)*(uint64_t *)(w + 8) : NULL; return nm && (!strncmp(nm, "TouchPouch", 10) || !strcmp(nm, "TouchStore")); }
static void w_move(uint64_t w, int x, int y) { if (!ours(w)) return; int32_t *r = (int32_t *)(w + 0x70); if (r[0] == x && r[1] == y) return; r[0] = x; r[1] = y; ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w + 0x10))(w); }
static void w_text(uint64_t w, const char *s) {   // 文字控件改字（虚表 main+0x20ba4c8，串 +0x88 / 长 +0x90，改完 0xadc434 重排；同 dm_vault_ui.c vu_text） | Change a text widget's text (vtable main+0x20ba4c8, string +0x88 / length +0x90, re-layout with 0xadc434 after changing; same as vu_text in dm_vault_ui.c)
    if (!ours(w) || *(uint64_t *)w - (uint64_t)(uintptr_t)B != 0x20ba4c8) return; size_t n = strlen(s);
    if (*(uint64_t *)(w + 0x90) == n && !memcmp(*(const char **)(w + 0x88), s, n)) return;
    ((void (*)(uint64_t, const char *, uint64_t))(B + 0x136e5c))(w + 0x88, s, n); ((void (*)(uint64_t))(B + 0xadc434))(w); }
static const char *str_of(uint32_t code) { char k[5] = { 0 }; memcpy(k, &code, 4); for (int i = 3; i >= 0 && k[i] == ' '; i--) k[i] = 0;   // 物品名：原版字符串（键 = 物品码，同 dm_kills.c str_of） | Item name: original game string (key = item code, same as str_of in dm_kills.c)
    struct { const char *k; uint64_t n; } s = { k, strlen(k) }; const char *r = dm_untag(((const char *(*)(void *))(B + 0x7f5630))(&s)); return r && *r ? r : "?"; }   /* 跳过西语串首的性别标记 | Skip the gender marker at the start of Spanish strings */
static int g_tip;
static void tip_show(uint64_t root, int on) {
    uint64_t tb = find_w(root, "TouchPouchTip", 1), bg = find_w(root, "TouchPouchTipBg", 1), box = find_w(root, "TouchPouch", 1); if (!tb || !bg || !box) return;
    if (!on) { w_move(tb, 99999, 0); w_move(bg, 99999, 0); g_tip = 0; return; }
    uint32_t cc[64]; int cn[64], nc = 0;   // 囊里内容按物品码合并（锁里只拷，锁外查名字） | Pouch contents merged by item code (only copy inside the lock, look up names outside it)
    pthread_mutex_lock(&g_umu); for (int i = 0; i < g_nu; i++) { int j = 0; while (j < nc && cc[j] != g_u[i].code) j++; if (j == nc) { if (nc == 64) continue; cc[nc] = g_u[i].code; cn[nc++] = 0; } cn[j]++; } pthread_mutex_unlock(&g_umu);
    char s[1024]; int k = snprintf(s, sizeof s, "%s", dm_str(B, "dmPouchTitle"));
    if (!nc) k += snprintf(s + k, sizeof s - k, "\n%s", dm_str(B, "dmPouchEmpty"));
    for (int i = 0; i < nc && k < (int)sizeof s - 64; i++) k += snprintf(s + k, sizeof s - k, "\n%s ×%d", str_of(cc[i]), cn[i]);
    w_text(tb, s); g_tip = 1;
    int32_t *br = (int32_t *)(box + 0x70), w = *(int32_t *)(tb + 0x78), h = *(int32_t *)(tb + 0x7c); if (w <= 0 || h <= 0) { w = 500; h = 300; }
    int x = br[0] + br[2] / 2 - w / 2, y = br[1] - h - 70; if (x < 40) x = 40;
    w_move(tb, x, y); int32_t *r = (int32_t *)(bg + 0x70); r[2] = w + 80; r[3] = h + 60; w_move(bg, x - 40, y - 30);
}
// 交互层（游戏主线程，按下那一刻的命中查询，dm_cursor.c）：清单开着时点哪都先收清单、这一下不做别的（同编年史说明框）；点百宝囊框本身照常（再点一次收起）
// Interaction layer (game main thread, the hit query at the moment of press, dm_cursor.c): while the list is open, a tap anywhere first closes the list and does nothing else (same as the chronicle info box); tapping the pouch box itself works as usual (tapping again closes it)
int dm_pouch_tip_gate(int x, int y) {
    if (!g_tip || !B) return 0; uint64_t root = *(uint64_t *)(B + 0x2f05458), box = find_w(root, "TouchPouch", 1);
    if (box && (((int (*)(uint64_t, uint64_t))*(uint64_t *)(*(uint64_t *)box + 0x48))(box, (uint64_t)(uint32_t)x | ((uint64_t)(uint32_t)y << 32)) & 1)) return 0;
    tip_show(root, 0); LOG("百宝囊：点了清单外 → 收起清单，这一下不做别的"); return 1;
}
static void ui_pump(void) {
    // 百宝囊的控件全部插在物品栏布局里（pouch_layout.py）：先取物品栏面板（界面根的直接子控件，dm_widget.h），再在面板里找；物品栏没开就不扫整棵界面树
    // All of the pouch widgets are inserted into the inventory layout (pouch_layout.py): first get the inventory panel (a direct child of the UI root, dm_widget.h), then search inside it; when the inventory is not open, the whole UI tree is not scanned
    uint64_t root = *(uint64_t *)(B + 0x2f05458), inv = root ? dm_ui_top(root, "PlayerInventoryExpansionLayout") : 0, boxw = inv ? find_w(inv, "TouchPouchBox", 3) : 0;
    if (dm_ui_top_chk_on()) { uint64_t old = root ? find_w(root, "TouchPouchBox", 2) : 0; if (old != boxw) { static int k; if (k++ < 20) LOG("【红】百宝囊对拍：TouchPouchBox 新找法 %#llx、旧找法 %#llx", (unsigned long long)boxw, (unsigned long long)old); } }
    if (!boxw) return;
    int gp = dm_custom_get("gem_pick"), sb = dm_custom_get("store_btn");
    w_move(find_w(boxw, "TouchPouch", 1), gp ? 222 : 99999, 1294); w_move(find_w(boxw, "TouchPouchIcon", 1), gp ? 246 : 99999, 1310); w_move(find_w(boxw, "TouchPouchCount", 1), gp ? 350 : 99999, 1294);
    w_move(find_w(boxw, "TouchStore", 1), sb ? 916 : 99999, 1323);
    static int shown = -1; int gen = atomic_load(&g_ugen); if (gen != shown) { char n[16]; snprintf(n, sizeof n, "%d", atomic_load(&g_ucount)); w_text(find_w(boxw, "TouchPouchCount", 1), n); shown = gen; if (g_tip) tip_show(root, 1); }
    if (atomic_exchange(&g_pouch_tap, 0)) tip_show(root, !g_tip);
    if (g_tip) tip_show(root, 1);   // 文字控件量尺寸要晚一两帧（fitToText）：开着时每帧对齐 | Text widgets measure their size a frame or two late (fitToText): align every frame while open
}

void dm_pouch_on_uimsg(const char *cat, const char *act, const char *arg) {   // 界面消息回调（diag.c）：我们的两个按钮发 ClosePanel:TouchPouch / TouchStore | UI message callback (diag.c): our two buttons send ClosePanel:TouchPouch / TouchStore
    if (!cat || !act || !arg || strcmp(cat, "PanelManager") || strcmp(act, "ClosePanel")) return;
    if (!strcmp(arg, "TouchPouch")) atomic_store(&g_pouch_tap, 1);
    else if (!strcmp(arg, "TouchStore") || !strcmp(arg, "TouchPadStore")) atomic_store(&g_store_req, 1);   // TouchPadStore = 手柄版暂停菜单的「存放」行（touch_layout.py pad_pause） | TouchPadStore = the "Deposit" row of the controller pause menu (touch_layout.py pad_pause)
}
void dm_pouch_step(uint8_t *b) {   // 游戏主线程每帧（dm_world.c） | every frame on the game main thread (dm_world.c)
    if (!b) return; B = b; uint64_t fr = dm_guest_frame();
    atomic_store(&g_stash_open, (dm_ui_panel_state() & 1) != 0);   // 储藏箱开着：服务端拍把囊里的放进高级仓库 | Stash open: the server tick moves what is in the pouch into the advanced stash
    ui_pump();
    uint64_t pl = cli_player(); if (!pl) { memset(g_pk, 0, sizeof g_pk); memset(g_gnd, 0, sizeof g_gnd); g_ist = -1; return; }
    uint32_t pm = *(uint32_t *)(pl + 0xc); if (pm == 0 || pm == 17) return;   // 死亡中 / 已死 | Dying / dead
    uint64_t inv = *(uint64_t *)(pl + 0x90); if (!inv || *(uint32_t *)inv != 0x01020304) return;
    { static long t_at = -1; if (t_at < 0) { const char *e = getenv("DM_TEST_STORE"); t_at = e ? atol(e) : 0; }   // 测试开关 DM_TEST_STORE=<帧>（宿主没有我们的布局、点不到存放按钮）：到帧当作点了一次「存放」 | Test switch DM_TEST_STORE=<frame> (the host does not have our layout, so the Deposit button cannot be tapped): at that frame behave as if "Deposit" was tapped once
      if (t_at > 0 && fr >= (uint64_t)t_at) { t_at = 0; atomic_store(&g_store_req, 1); LOG("测试 DM_TEST_STORE：帧 %llu 当作点了「存放」", (unsigned long long)fr); } }
    if (atomic_exchange(&g_store_req, 0)) { if (!dm_custom_get("store_btn")) LOG("存放：按钮已在自定义页关掉，不做"); else if (g_ist >= 0) LOG("存放：上一次还没做完"); else store_begin(pl); }
    if (*(uint64_t *)(inv + 0x40)) return;   // 手上拿着东西：不捡、不存（同药水自动拾取） | Something is held in hand: no pickup, no storing (same as potion auto pickup)
    store_step(pl, fr);
    if (dm_custom_get("gem_pick") && g_ist < 0) auto_pick(pl, fr);
}
void dm_pouch_srv_tick(uint8_t *b, uint64_t G, uint64_t player) {   // 服务端拍（diag.c hud_tick；不在局内 player 为 0）：收 / 放 / 系统提示（系统提示只许服务端线程发） | Server tick (hud_tick in diag.c; player is 0 when not in a run): stow / release / system notices (system notices may only be sent from the server thread)
    static uint64_t lp; if (!player) { lp = 0; pthread_mutex_lock(&g_umu); g_uload = 0; pthread_mutex_unlock(&g_umu); return; }
    B = b; uint32_t tq = *(uint32_t *)(G + 0x170);
    if (player != lp) { lp = player; uint64_t pd = *(uint64_t *)(player + 0x10); pthread_mutex_lock(&g_umu); strlcpy(g_uchar, pd ? (const char *)pd : "", sizeof g_uchar); pouch_load(); g_uload = 1; pthread_mutex_unlock(&g_umu); }   // 进局（换了玩家单位）读一次 | Read once on entering a run (the player unit changed)
    int near = ((int (*)(uint64_t, uint64_t, int, int))(b + 0x650d10))(G, player, 0x10b, 0x32) & 1; atomic_store(&g_near_pub, near);   // 原版「移到仓库」用的同一个判断：50 以内有储藏箱物件 267 | The same check the original game's "move to stash" uses: a stash object 267 within 50
    uint32_t unit = adv_stash_unit(player); int go63 = near && unit;
    typedef struct { uint32_t id, t; } watch; static watch wl[64]; static int nw;   // 发了 0x63、等它离开背包的 | Items for which 0x63 was sent, waiting for them to leave the inventory
    int full_notice = 0;
    int saved = 0;
    pthread_mutex_lock(&g_umu);
    for (unsigned r = atomic_load(&g_absr); r != atomic_load(&g_absw); r++) { uint32_t id = atomic_load(&g_absq[r % 64]); atomic_store(&g_absr, r + 1);   // 收：储藏箱旁直接进高级仓库，别处进囊 | Stow: next to the stash it goes straight into the advanced stash, elsewhere into the pouch
        if (go63) { move_to_stash(b, G, player, id, unit); if (nw < 64) wl[nw++] = (watch){ id, tq }; LOG("百宝囊：物品 id %u 在储藏箱旁 → 原版移到仓库（高级仓库单位 %u）", id, unit); }
        else if (!unit) LOG("百宝囊：物品 id %u —— 这个角色没有高级仓库，不收", id);
        else if (pouch_put(b, G, player, id) > 0) saved = 1; }
    // 测试开关 DM_TEST_HOLDY=<拍>:<物品码>:<54|55>（手柄光标不好用脚本摆位，用它造「按住确认键移到当前页」）：到拍对背包里第一件这种物品代发那条包（字段照实录的填：个人页 0x54、共享页 0x55），走收包分发、会经过上面的改道
    // Test switch DM_TEST_HOLDY=<tick>:<item code>:<54|55> (the gamepad cursor is hard to position from a script, so this creates "hold the confirm button to move to the current page"): at that tick, send that packet on behalf of the first such item in the backpack (fields filled as recorded: personal page 0x54, shared page 0x55), going through packet dispatch and therefore through the rerouting above
    { static int at = -1, kind; static char code[5]; if (at < 0) { const char *e = getenv("DM_TEST_HOLDY"); if (!e || sscanf(e, "%d:%4[^:]:%x", &at, code, &kind) != 3) at = 0; }
      if (at > 0 && tq >= (uint32_t)at) { at = 0; uint64_t inv = *(uint64_t *)(player + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0, f = 0;
          for (int k = 0; it && k < 2048 && !f; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; uint32_t c = item_code(it); if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && !memcmp(&c, code, strlen(code))) f = it; else it = *(uint64_t *)(d + 0xb0); }
          if (f) { uint64_t pp = *(uint64_t *)(f + 0x38); uint32_t id = *(uint32_t *)(f + 8), sh = 0; uint16_t x = pp ? (uint16_t)*(int32_t *)(pp + 0x10) : 0, y = pp ? (uint16_t)*(int32_t *)(pp + 0x14) : 0; uint8_t pk[29]; memset(pk, 0, sizeof pk);
              for (uint64_t en = inv ? *(uint64_t *)(inv + 0x68) : 0, n = 0; en && n < 16 && !sh; en = *(uint64_t *)(en + 0x10), n++) if (*(uint8_t *)(en + 9) && !*(uint8_t *)(en + 0xa)) sh = *(uint32_t *)en;
              int ln; pk[0] = (uint8_t)kind; memcpy(pk + 1, &id, 4);
              if (kind == 0x55) { memcpy(pk + 5, &sh, 4); memcpy(pk + 13, &x, 2); memcpy(pk + 15, &y, 2); pk[17] = 4; pk[21] = 0x0d; pk[23] = 9; ln = 29; }
              else { memcpy(pk + 9, &x, 2); memcpy(pk + 11, &y, 2); pk[13] = 4; pk[17] = 0x0d; pk[19] = 9; ln = 21; }
              LOG("测试 DM_TEST_HOLDY：拍 %u 对背包里的 %s（id %u，格 %u,%u）代发 0x%02x", tq, code, id, x, y, kind);
              *(uint32_t *)(G + 0x19050) = 1; ((void (*)(uint64_t, uint64_t, uint8_t *, uint32_t))(b + 0x6e5b40))(G, player, pk, (uint32_t)ln); *(uint32_t *)(G + 0x19050) = 0; } } }
    for (int i = 0; i < g_c2s_nq; i++) { uint32_t id = g_c2s_q[i];   // 拖进共享页被改道放回背包的（dm_pouch_c2s）：人还在箱子旁就发原版移到仓库 | items dragged into the shared tab and rerouted back to the inventory (dm_pouch_c2s): if still at the stash, send the game's move-to-stash
        if (go63 && bag_item(player, id)) { move_to_stash(b, G, player, id, unit); if (nw < 64) wl[nw++] = (watch){ id, tq }; LOG("珍宝匣：物品 id %u 已回到背包 → 原版移到仓库（高级仓库单位 %u）", id, unit); }
        else LOG("珍宝匣：物品 id %u 改道后%s，留在背包", id, go63 ? "不在背包里" : "人已离开储藏箱"); }
    g_c2s_nq = 0; if (g_c2s_full) { g_c2s_full = 0; full_notice = 1; }
    for (int i = 0; i < g_nready; i++) {   // 已证明放进背包的 → 0x63；证明到时人已离开储藏箱就收回囊里 | Those proven to be in the inventory -> 0x63; if the player has left the stash by the time the proof arrives, take it back into the pouch
        if (go63) { move_to_stash(b, G, player, g_ready[i], unit); if (nw < 64) wl[nw++] = (watch){ g_ready[i], tq }; }
        else if (pouch_put(b, G, player, g_ready[i]) > 0) { saved = 1; LOG("百宝囊：物品 id %u 落盘证明时已离开储藏箱，收回囊里", g_ready[i]); } }
    g_nready = 0;
    for (int i = 0; i < nw; ) {   // 0x63 之后：离开了背包 = 进了高级仓库（叠进去的那件已释放）；2 秒还在背包（页满 / 叠满）→ 收回囊里 | After 0x63: left the inventory = entered the advanced stash (the item that was stacked in has been freed); still in the inventory after 2 seconds (page full / stack full) -> take it back into the pouch
        if (!bag_item(player, wl[i].id)) { wl[i] = wl[--nw]; saved = 1; continue; }   // 进了仓库就马上请存盘：叠堆落盘前被杀，物品会回到放进背包那一刻（不丢，但留在背包） | Request a save as soon as it has entered the stash: if killed before the stack is persisted, the item goes back to the moment it was put in the inventory (not lost, but left in the inventory)
        if (tq - wl[i].t >= 50) {   // 那一格叠满（99）/ 放不下：留在背包、提示一次，不收回囊里——原先收回囊里，箱子开着时会「放回背包 → 再发 → 再失败」每秒循环一次 | that stack is full (99) / no room: leave it in the inventory and notify once, do not take it back into the Pouch. It used to be taken back, which with the stash open looped "back to inventory → send again → fail again" once a second
            LOG("百宝囊：物品 id %u 移到仓库 2 秒还在背包（那一格叠满 / 放不下）→ 留在背包，提示一次", wl[i].id); full_notice = 1; wl[i] = wl[--nw]; continue; }
        i++; }
    {   // 放：储藏箱开着、人在旁边、囊里有、收进囊的都已证明（同一件不会同时挂着收和放）；每 25 拍一批，背包放满就等下一批 | Release: stash open, player next to it, pouch not empty, everything stowed into the pouch already proven (the same item is never pending both stow and release); one batch every 25 ticks; when the inventory is full, wait for the next batch
        static uint32_t last; int puts = 0; for (int j = 0; j < g_nup; j++) puts += g_up[j].put;
        // 手上（服务端光标）拿着东西时不放：往背包里塞东西会占掉手上那件拿起时的原格，玩家松手放不回原位；卡死那一局就是拿着魔盒拖的途中
        // 放进来 5 件、正好落在魔盒的原格上，之后服务端光标还挂着魔盒、客户端手上已空。等放下了再放（下一批 25 拍后再看）
        // Do not deposit while the hand (server cursor) is holding something: stuffing items into the backpack takes the original slot of the held item, and the player cannot drop it back in place; the hang run was exactly this: mid-drag of the Horadric Cube,
        // 5 items were deposited, landing right on the Cube's original slot, after which the server cursor still held the Cube while the client hand was empty. Wait until it is put down (check again with the next batch, 25 ticks later)
        uint64_t pinv = *(uint64_t *)(player + 0x90), phand = pinv && *(uint32_t *)pinv == 0x01020304 ? *(uint64_t *)(pinv + 0x40) : 0;
        static uint64_t lh; if (phand && phand != lh && atomic_load(&g_stash_open) && g_nu) LOG("百宝囊：手上拿着东西 → 先不往背包放，等放下"); lh = phand;
        if (atomic_load(&g_stash_open) && go63 && g_nu && !puts && nw == 0 && !phand && tq - last >= 25) { last = tq; int n = 0; while (pouch_take(b, G, player)) n++;
            if (n) { saved = 1; LOG("百宝囊：开着储藏箱 → 放 %d 件到背包，囊里还剩 %d 件", n, g_nu); } } }
    full_notice |= stash_sweep(b, G, player, go63 && atomic_load(&g_stash_open) ? unit : 0, tq, &saved);
    { static int said_open; if (!atomic_load(&g_stash_open)) said_open = 0;   // 一次开箱只提示一次 | notify only once per stash open
      if (full_notice && !said_open) { said_open = 1; snprintf(g_notice, sizeof g_notice, "%s", dm_str(b, "dmStoreFull")); atomic_store(&g_notice_req, 1); } }
    int pend = g_nup + g_udirty; pthread_mutex_unlock(&g_umu);   // 侧文件上次没写成也算「还没落盘」，接着请存盘 | the side file failing to write last time also counts as "not on disk yet", so keep requesting a save
    { static uint32_t lastreq; if (saved || (pend && tq - lastreq >= 50)) { lastreq = tq; int r = getenv("DM_TEST_POUCH_NOSAVE") ? -9 : dm_request_save(); if (!saved) LOG("百宝囊：还有 %d 条没证明，再请一次存盘 → %d", pend, r); } }   // 测试开关 DM_TEST_POUCH_NOSAVE=1：不请存盘（配合杀进程验落盘前掉电） | Test switch DM_TEST_POUCH_NOSAVE=1: do not request a save (combine with killing the process to verify power loss before persisting)
    if (atomic_exchange(&g_notice_req, 0)) dm_game_notice(b, g_notice);
    {   // 测试开关 DM_TEST_NOTICE=<帧数>（复现「提示框在屏上时开面板」用，产品不设）：每隔这么多客体帧发一条原版系统提示 | test switch DM_TEST_NOTICE=<frames> (for reproducing "opening a panel while a notification is on screen"; not set in the product): sends one original system notification every this many guest frames
        static int per = -1; static uint64_t last; if (per < 0) { const char *e = getenv("DM_TEST_NOTICE"); per = e ? atoi(e) : 0; }
        uint64_t dm_guest_frame(void); uint64_t f = dm_guest_frame(); if (per > 0 && f - last >= (uint64_t)per) { last = f; dm_game_notice(b, "TEST NOTICE 0123456789"); LOG("测试 DM_TEST_NOTICE：帧 %llu 发一条系统提示", (unsigned long long)f); } }
}

