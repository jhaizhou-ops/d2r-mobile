// 编年史仓库：只收已鉴定的暗金和套装，每一件（类型 + 表里的行号）在仓库里一格。
// 这里是服务端那一半：存入 / 取出都在服务端拍里做（diag.c hud_tick 调 dm_vault_tick），全用原版函数、只发原版包（读码，宿主实测）：
//   存入 = 写位流 main+0x581b60 → 从物品栏摘下 0x55e430 → 通知客户端 0x642f70(…, 0x20, 4, 0, 页, y, x)（照原版用掉背包药水 0x6d3544）→ 释放 0x658be0；
//   取出 = 读位流建物品 0x5793a0 + 0x6564e0（孔里的照原版复制物品 0x65a840 逐个建、0x64d080 镶回，dm_pages.c dm_item_make）→ 原版在格区找空位 0x55f620 确认背包放得下
//         → 原版读档放物品 0x6f1940 放进背包 → 通知客户端 0x642f70(…, 1, 2, 0, 0, 0, x | y << 16)（同收包 0x18 的处理 0x6d0050）。只进背包、放不下不取。
// 侧文件 <共享仓库名>.swvault 放存档工作目录根（游戏按角色模式选共享仓库名：main+0x2ce1e8，模式字节 main+0x2127c48、角色状态 main+0x21278d5 位 2 = 专家）；
// 名字带共享仓库前缀，iCloud 同步把它归进共享仓库那个单位（dm_cloudsync.c is_stash）。
// 落盘：存取只改内存里的仓库、记一条「待落盘」（存入 / 取出 + 那件的位流），请游戏马上存一次盘；侧文件在提交前钩子里写（hle_fs.c Commit 开头）。
// 不变式：同一件物品只在一处——要么在侧文件里，要么在原版存档（角色档 .d2s / 共享仓库 .d2i）里。侧文件写的是「盘上状态」g_d，待落盘的存取按发生次序、
// 在原版存档里得到证明后才并进 g_d（物品在存档里按字节对齐，我们写的位流与存档里的逐字节相同）：
//   存入：工作目录里所有角色档和共享仓库档都已没有那件 → 并进；
//   取出：任一角色档或共享仓库档里已有那件 → 并进（原先只看当前角色的 .d2s：取出后、证明前放进共享仓库或卖掉，就永远对不上，侧文件不更新，重进后那件又回到编年史 = 复制）；
//   取出后第二次提交起、且已过 10 秒仍哪都没有 → 那件已离开（卖掉 / 丢下 / 用掉），也并进（不无限期挂着）。依据：存取后马上请存盘，下一拍就序列化；
//   序列化在存取之前、提交在存取之后的那一次至多一次（定时存盘的序列化与写盘之间夹着存取，角色档由另一条线程写）。
// 前一条没证明，后面的都等（次序：交换时旧件先取出、新件后存入）。侧文件与角色档在同一次提交里原子换入；提交前被杀，重启时工作目录从已提交版重新克隆，两边一起回到上一次。
// 进局读侧文件时再兜一道：侧文件里某件的字节已出现在原版存档里（已取出），就从编年史里去掉（只会去重，不会凭空少）。
// 格式（小端）：'SWVT' u16 版本 1 u16 条数；每条 u8 类型（0 套装件 / 1 暗金）u8 保留 u16 行号 u16 位流长 + 位流（游戏自己写存档时那件物品的字节，含孔里的）。
// Chronicle vault: accepts only identified unique and set items, one cell in the vault per item (type + row number in the table).
// This is the server half: store / withdraw are both done in the server tick (diag.c hud_tick calls dm_vault_tick), using only original-game functions and sending only original-game packets (from reading the code, measured on the host):
//   store = write bitstream main+0x581b60 → detach from the inventory structure 0x55e430 → notify the client 0x642f70(..., 0x20, 4, 0, page, y, x) (following the original game's use of an inventory potion 0x6d3544) → free 0x658be0;
//   withdraw = read bitstream and build the item 0x5793a0 + 0x6564e0 (socketed ones are built one by one following the original game's item copy 0x65a840 and socketed back with 0x64d080, dm_pages.c dm_item_make) → the original game's free-spot search in a grid region 0x55f620 confirms the inventory has room
//         → the original game's load-time item placement 0x6f1940 puts it into the inventory → notify the client 0x642f70(..., 1, 2, 0, 0, 0, x | y << 16) (same as the handler 0x6d0050 for received packet 0x18). Inventory only; if it does not fit, no withdraw.
// The sidecar file <shared stash name>.swvault sits at the root of the save working directory (the game picks the shared stash name by character mode: main+0x2ce1e8, mode byte main+0x2127c48, character status main+0x21278d5 bit 2 = hardcore);
// the name carries the shared stash prefix, so iCloud sync files it under the shared stash unit (dm_cloudsync.c is_stash).
// Flushing: a store / withdraw only changes the in-memory vault and records one "pending flush" entry (store / withdraw + that item's bitstream), then asks the game to save at once; the sidecar file is written in the pre-commit hook (start of hle_fs.c Commit).
// Invariant: any one item exists in exactly one place - either in the sidecar file or in the original saves (character save .d2s / shared stash .d2i). The sidecar file holds the "on-disk state" g_d; pending store / withdraw operations are merged into g_d in the order they happened,
// only after being proven in the original saves (items are byte-aligned in a save, and the bitstream we write is byte-for-byte identical to the one in the save):
//   store: none of the character saves and shared stash saves in the working directory has that item any more → merge;
//   withdraw: any character save or shared stash save already has that item → merge (previously only the current character's .d2s was checked: if after withdraw and before proof it was put into the shared stash or sold, it could never be matched, the sidecar file was not updated, and after re-entering the item was back in the chronicle = duplication);
//   from the second commit after a withdraw on, with 10 seconds passed and it still nowhere → that item has left (sold / dropped / used up), merge as well (do not leave it hanging indefinitely). Basis: a save is requested right after a store / withdraw and serialization happens on the next tick;
//   there is at most one commit whose serialization was before the store / withdraw and whose commit was after it (the store / withdraw falls between a timed save's serialization and its disk write, with the character save written by another thread).
// If an earlier entry is not proven, all later ones wait (order: in a swap the old item is withdrawn first, the new item stored after). The sidecar file and the character save are swapped in atomically in the same commit; if killed before commit, on restart the working directory is re-cloned from the committed version and both sides go back to the previous state together.
// One more safety net when reading the sidecar file on join: if an item's bytes in the sidecar file already appear in the original saves (already withdrawn), it is removed from the chronicle (this only deduplicates, never loses items out of nowhere).
// Format (little-endian): 'SWVT' u16 version 1 u16 entry count; per entry u8 type (0 set item / 1 unique) u8 reserved u16 row number u16 bitstream length + bitstream (the bytes of that item as the game itself writes it in a save, including socketed ones).
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <pthread.h>
#include "dm.h"

#define VMAX 1024
#define VBYTES 1024   // 同 dm_pages.c：单件位流上限（含孔里的） | Same as dm_pages.c: upper limit of a single item's bitstream (including socketed ones)
typedef struct { uint8_t kind; uint16_t idx, len; uint8_t *data; } ventry;
static ventry g_v[VMAX]; static int g_nv, g_loaded; static char g_vname[64];   // g_v = 内存里的仓库（界面看的） | g_v = the in-memory vault (what the UI sees)
static ventry g_d[VMAX]; static int g_nd, g_ddirty;   // g_d = 盘上状态（侧文件写它）；g_ddirty = 进局去重改了 g_d，下次提交要写 | g_d = on-disk state (what the sidecar file writes); g_ddirty = join-time dedup changed g_d, to be written on the next commit
static pthread_mutex_t g_vmu = PTHREAD_MUTEX_INITIALIZER;   // 服务端拍（存取）与写盘线程（提交前钩子）都碰仓库 | Both the server tick (store / withdraw) and the disk write thread (pre-commit hook) touch the vault
// 待落盘的一次存取：位流整份留着（并进 g_d 用），指纹 = 位流跳过前 8 字节的前 48 字节：前 4 字节是标志位（取出时清「新物品」位会变），接着是位置字段（方式 / 装备位 / x / y / 页，取出后可能穿在身上）
// One pending store / withdraw: the whole bitstream is kept (for merging into g_d); fingerprint = 48 bytes of the bitstream after skipping the first 8 bytes: the first 4 bytes are flag bits (clearing the "new item" bit on withdraw changes them), followed by the position fields (mode / equipment slot / x / y / page; after withdraw it may be equipped)
typedef struct { int put; uint8_t kind; uint16_t idx, len; uint8_t *data; int n; uint64_t made, t; } vpend;   // made = 记下时已成功提交过几次；t = 记下时的单调时钟（秒） | made = number of successful commits when recorded; t = monotonic clock when recorded (seconds)
enum { VPEND = 64 };
static vpend g_pend[VPEND]; static int g_npend, g_pend_cap = VPEND;   // g_pend_cap：测试开关 DM_TEST_VAULT_PEND=<条数> 可调小（造「队列满」） | g_pend_cap: can be lowered with the test switch DM_TEST_VAULT_PEND=<entries> (to produce "queue full")
uint64_t dm_fs_commit_count(void); void dm_fs_save_poison(int e);
static _Atomic int g_vretry;   // 侧文件上次没写成：服务端拍据此每 10 秒请一次存盘，直到写成 | the side file failed to write last time: based on this the server tick requests a save every 10 seconds until the write succeeds
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static void pend_add(int put, int kind, int idx, const uint8_t *bits, int len) {
    if (g_npend >= VPEND || len <= 16) { LOG("【红】编年史仓库：待落盘已满 %d 条，这次存取不记（不该到这里：服务端拍在队列快满时先不做存取，见 dm_vault_tick）", VPEND); return; }
    vpend *p = &g_pend[g_npend]; p->data = malloc((size_t)len); if (!p->data) return; memcpy(p->data, bits, (size_t)len);
    p->put = put; p->kind = (uint8_t)kind; p->idx = (uint16_t)idx; p->len = (uint16_t)len; p->n = len - 8 > 48 ? 48 : len - 8; p->made = dm_fs_commit_count(); p->t = (uint64_t)now_s(); g_npend++; }
static _Atomic uint32_t g_req_put; static _Atomic uint32_t g_req_take;   // 存入：物品 id；取出：1 + 类型 << 16 | 行号 | Store: item id; withdraw: 1 + type << 16 | row number
static _Atomic int g_full_snd;   // 取出时背包放不下 / 存入不收：主线程播一次原版错误音（dm_vault_take_full_sound，dm_vault_ui.c 播） | Inventory has no room on withdraw / store rejected: the main thread plays the original game's error sound once (dm_vault_take_full_sound, played by dm_vault_ui.c)
int dm_vault_take_full_sound(void) { return atomic_exchange(&g_full_snd, 0); }   // 界面取一次「该播错误音」（取出放不下 / 存入不收） | The UI takes "should play the error sound" once (withdraw does not fit / store rejected)
void dm_vault_request_deposit(uint32_t item_id) { atomic_store(&g_req_put, item_id); }
int dm_vault_put_pending(void) { return atomic_load(&g_req_put) != 0; }   // 上一件存入还没被服务端拍取走（存放按钮一件一件排队用，dm_pouch.c） | The previous deposit has not been picked up by the server tick yet (used by the Deposit button to queue items one at a time, dm_pouch.c)
int dm_vault_ready(void) { return g_loaded; }   // 这类角色有编年史仓库（有共享仓库）且已读入 | This kind of character has a chronicle vault (has a shared stash) and it has been loaded
void dm_vault_request_withdraw(int kind, int idx) { atomic_store(&g_req_take, 1u << 24 | (uint32_t)(kind & 0xff) << 16 | (uint32_t)(idx & 0xffff)); }
static _Atomic uint64_t g_take_at;   // 拖回背包的落点：0 = 没指定（自己找空位）；否则 1 << 63 | 物品图中心在背包格里的位置（× 100）x << 32 | y << 16 | 列数 << 8 | 行数 | Drop point for dragging back to the inventory: 0 = not specified (find a free spot itself); otherwise 1 << 63 | item image center position in inventory cells (× 100) x << 32 | y << 16 | column count << 8 | row count
void dm_vault_request_withdraw_at(int kind, int idx, int fx100, int fy100, int cols, int rows) {
    atomic_store(&g_take_at, 1ull << 63 | (uint64_t)(uint16_t)fx100 << 32 | (uint64_t)(uint16_t)fy100 << 16 | (uint64_t)(cols & 0xff) << 8 | (uint64_t)(rows & 0xff)); dm_vault_request_withdraw(kind, idx); }
static _Atomic int g_vgen;   // 仓库内容一变就加一（界面 dm_vault_ui.c 据此重画） | Incremented whenever the vault contents change (the UI in dm_vault_ui.c redraws based on it)
int dm_vault_gen(void) { return atomic_load(&g_vgen); }

const char *dm_save_workdir(void); int dm_request_save(void); uint64_t dm_diag_srv_item(uint32_t id);
int dm_item_write(uint8_t *b, uint64_t it, uint8_t *buf); uint64_t dm_item_make(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out);   // dm_pages.c
void *dm_memfind(const uint8_t *h, size_t hn, const uint8_t *n, size_t nn);

static const char *vault_name(uint8_t *b) {   // 同游戏选共享仓库名：DLC 类型（3）= ModernSharedStash*、毁灭之王类型（2）= SharedStash*；状态位 2 = 专家 | Same as the game's choice of shared stash name: DLC type (3) = ModernSharedStash*, Lord of Destruction type (2) = SharedStash*; status bit 2 = hardcore
    uint8_t mode = *(uint8_t *)(b + 0x2127c48); int hc = (*(uint16_t *)(b + 0x21278d5) >> 2) & 1;
    if (mode == 3) return hc ? "ModernSharedStashHardCoreV2" : "ModernSharedStashSoftCoreV2";
    if (mode == 2) return hc ? "SharedStashHardCoreV2" : "SharedStashSoftCoreV2";
    return NULL;   // 经典类型：没有共享仓库，也不开编年史仓库 | Classic type: no shared stash, and no chronicle vault either
}
const char *dm_stash_name(uint8_t *b) { return vault_name(b); }   // 个人仓库多页（dm_pages.c）核对时也要认共享仓库档 | Personal stash multi-page (dm_pages.c) verification also needs to recognize the shared stash save
static void vault_clear(void) { for (int i = 0; i < g_nv; i++) free(g_v[i].data); g_nv = 0; for (int i = 0; i < g_nd; i++) free(g_d[i].data); g_nd = 0; g_ddirty = 0;
    for (int i = 0; i < g_npend; i++) free(g_pend[i].data); g_npend = 0; }
int dm_save_blobs(const char *wd, const char *stash, dm_blob *out, int max); int dm_blobs_have(const dm_blob *bl, int nb, const uint8_t *bits, int len); void dm_blobs_free(dm_blob *bl, int nb);   // dm_pages.c
static int dfind(int kind, int idx) { for (int i = 0; i < g_nd; i++) if (g_d[i].kind == kind && g_d[i].idx == idx) return i; return -1; }
static int vault_load(const char *name) {   // 进局读一次（工作目录里的那份 = 上次提交的） | Read once on entering a run (the copy in the working directory = the last committed one)
    vault_clear(); const char *wd = dm_save_workdir(); if (!wd) return -1;
    char p[1200]; snprintf(p, sizeof p, "%s/%s.swvault", wd, name); FILE *f = fopen(p, "rb");
    if (!f) { LOG("编年史仓库：%s 还没有侧文件（空仓库）", name); return 0; }
    uint8_t h[8]; int n = 0, bad = 0;
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "SWVT", 4) || *(uint16_t *)(h + 4) != 1) bad = 1;
    else { n = *(uint16_t *)(h + 6);
        for (int i = 0; i < n && !bad; i++) { uint8_t e[6]; if (fread(e, 1, 6, f) != 6) { bad = 1; break; } uint16_t len = *(uint16_t *)(e + 4);
            if (!len || len > VBYTES || g_nv >= VMAX) { bad = 1; break; } ventry *v = &g_v[g_nv]; v->kind = e[0]; v->idx = *(uint16_t *)(e + 2); v->len = len; v->data = malloc(len);
            if (!v->data || fread(v->data, 1, len, f) != len) { free(v->data); bad = 1; break; } g_nv++; } }
    fclose(f);
    if (bad) LOG("【红】编年史仓库：%s 读到第 %d 条坏了，只用前面读好的 %d 条（文件不动）", p, g_nv, g_nv);
    // 盘上状态 = 读到的；再按原版存档去重：字节已在任一角色档 / 共享仓库档里的那件已经取出过，编年史里去掉（这次提交时写回侧文件）
    // On-disk state = what was read; then deduplicate against the original saves: an item whose bytes are already in any character save / shared stash save has already been withdrawn and is removed from the chronicle (written back to the sidecar file on this commit)
    dm_blob bl[64]; int nb = dm_save_blobs(wd, name, bl, 64), drop = 0;
    for (int i = 0; i < g_nv; ) { if (dm_blobs_have(bl, nb, g_v[i].data, g_v[i].len)) { LOG("【红】编年史仓库：%s 第 %d 件（%s %d）的字节已在原版存档里 → 从编年史去掉（防复制）", name, i, g_v[i].kind ? "暗金" : "套装件", g_v[i].idx); free(g_v[i].data); g_v[i] = g_v[--g_nv]; drop++; } else i++; }
    dm_blobs_free(bl, nb);
    for (int i = 0; i < g_nv; i++) { g_d[i] = g_v[i]; g_d[i].data = malloc(g_v[i].len); if (g_d[i].data) memcpy(g_d[i].data, g_v[i].data, g_v[i].len); else g_d[i].len = 0; } g_nd = g_nv; g_ddirty = drop > 0;
    if (bad) return -2;
    LOG("编年史仓库：%s 读入 %d 件（对原版存档去重 %d 件，比对 %d 份存档）", name, g_nv, drop, nb); return g_nv;
}
static int vault_save(const char *wd, const char *name) {   // 写进工作目录（临时文件再改名），随这次提交落盘；只在提交前钩子里调 | Write into the working directory (temp file, then rename), flushed to disk with this commit; called only in the pre-commit hook
    if (!wd) return -1;
    { static int nf = -1; if (nf < 0) { const char *e = getenv("DM_TEST_VAULT_SAVE_FAIL"); nf = e ? atoi(e) : 0; } if (nf > 0) { nf--; LOG("【红】编年史仓库：测试 DM_TEST_VAULT_SAVE_FAIL 这次当作写失败"); return -1; } }   // 测试开关：前 N 次写当作失败（验拒绝提交 + 重试） | test switch: the first N writes are treated as failures (verifies refusing the commit + retry)
    char p[1200], t[1210]; snprintf(p, sizeof p, "%s/%s.swvault", wd, name); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb"); if (!f) return -1;
    uint8_t h[8] = { 'S', 'W', 'V', 'T', 1, 0, 0, 0 }; int nw = 0; for (int i = 0; i < g_nd; i++) nw += g_d[i].len > 0; *(uint16_t *)(h + 6) = (uint16_t)nw; int ok = fwrite(h, 1, 8, f) == 8;   // 写盘上状态 g_d | Write the on-disk state g_d
    for (int i = 0; i < g_nd && ok; i++) { if (!g_d[i].len) continue; uint8_t e[6] = { g_d[i].kind, 0 }; *(uint16_t *)(e + 2) = g_d[i].idx; *(uint16_t *)(e + 4) = g_d[i].len;
        ok = fwrite(e, 1, 6, f) == 6 && fwrite(g_d[i].data, 1, g_d[i].len, f) == g_d[i].len; }
    ok = fclose(f) == 0 && ok; if (!ok || rename(t, p)) { remove(t); LOG("【红】编年史仓库：写 %s 失败", p); return -1; }
    return 0;
}
static int vfind(int kind, int idx) { for (int i = 0; i < g_nv; i++) if (g_v[i].kind == kind && g_v[i].idx == idx) return i; return -1; }
// 存着那件的说明（第 5 步）：界面单击存着的格子时请求；服务端拍里用位流临时建一件（不放进任何物品栏），交原版说明 main+0x374f00(玩家, 玩家, 物品, 0, 1, 0, 空, &{0, 0})
// （给真物品出说明时属性表传空），拷出串后释放临时物品。结果经 g_desc_ready 交回界面（游戏主线程取）
// Description of the stored item (step 5): requested when the UI single-taps a stored cell; in the server tick a temporary item is built from the bitstream (not put into any inventory structure) and passed to the original game's description main+0x374f00(player, player, item, 0, 1, 0, NULL, &{0, 0})
// (the property table is passed as NULL when producing a description for a real item), and the temporary item is freed after the string is copied out. The result is handed back to the UI via g_desc_ready (fetched on the game main thread)
static _Atomic int g_desc_req, g_desc_ready; static char g_desc[4096];
void dm_vault_request_desc(int kind, int idx) { atomic_store(&g_desc_ready, 0); atomic_store(&g_desc_req, 1 << 24 | (kind & 0xff) << 16 | (idx & 0xffff)); }
int dm_vault_take_desc(char *out, size_t n) { if (!atomic_exchange(&g_desc_ready, 0)) return 0; strlcpy(out, g_desc, n); return 1; }
static void vault_desc(uint8_t *b, uint64_t G, uint64_t player, uint32_t req) {
    int k = vfind((int)(req >> 16 & 0xff), (int)(req & 0xffff)); if (k < 0) return;
    uint64_t nu = dm_item_make(b, G, 0, g_v[k].data, g_v[k].len, NULL); if (!nu) { LOG("【红】编年史仓库：说明——位流建不出物品"); return; }
    typedef struct { char *p; uint64_t len, cap; } gstr_t; struct { uint64_t n; uint8_t has; } cnt = { 0, 0 };
    gstr_t r = ((gstr_t (*)(uint64_t, uint64_t, uint64_t, int, int, int, void *, void *))(b + 0x374f00))(player, player, nu, 0, 1, 0, NULL, &cnt);
    size_t n = r.p && r.len < sizeof g_desc ? r.len : (r.p ? sizeof g_desc - 1 : 0); if (n) memcpy(g_desc, r.p, n); g_desc[n] = 0;
    if (r.p && !(r.cap >> 63) && (*(volatile uint8_t *)(b + 0x3211700) & 1)) { uint64_t a = *(uint64_t *)(b + 0x32116f8); ((void (*)(uint64_t, void *))(*(uint64_t *)(*(uint64_t *)a + 0x28)))(a, r.p); }   // 游戏分配器释放（同 dm_runeword.c g_free） | Free with the game allocator (same as dm_runeword.c g_free)
    ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, nu);
    if (n) atomic_store(&g_desc_ready, 1);
}
int dm_vault_get_bits(int kind, int idx, uint8_t *out, int cap) {   // 拷出某件的位流（主线程比较用；找不到或放不下返回 0）
    pthread_mutex_lock(&g_vmu);
    int i = vfind(kind, idx), n = 0;
    if (i >= 0 && g_v[i].len > 0 && (int)g_v[i].len <= cap) { memcpy(out, g_v[i].data, g_v[i].len); n = (int)g_v[i].len; }
    pthread_mutex_unlock(&g_vmu);
    return n; }
int dm_vault_has(int kind, int idx) { pthread_mutex_lock(&g_vmu); int r = g_loaded && vfind(kind, idx) >= 0; pthread_mutex_unlock(&g_vmu); return r; }   // 界面读（游戏主线程）：这一格存着没有 | UI read (game main thread): whether this cell holds a stored item

// 任务物品（王者之杖、卡林的连枷、毒蛇护身符……）：游戏自己的物品表记录（步长 0x1c0，[版本表+0x15c0]，下标 [物品+4]，同 dm_cursor_item.c）+0x13a = weapons / armor / misc.txt 的 quest 列。
// 宿主实测：只有 0x13a 一处同时对上 msf 10 / hst 10 / hfh 25 / qf1 17 / qf2 17 / vip 10 / rin 0，全表非 0 的 26 条与原版表逐条一致。
// 存进编年史会卡任务进度：存放与手动存入都不收；已经存进去的照常能取出
// Quest items (Staff of Kings, Khalim's Flail, Amulet of the Viper...): the game's own item table record (stride 0x1c0, [version table+0x15c0], index [item+4], same as dm_cursor_item.c) +0x13a = the quest column of weapons / armor / misc.txt.
// Measured on the host: 0x13a is the only place that simultaneously matches msf 10 / hst 10 / hfh 25 / qf1 17 / qf2 17 / vip 10 / rin 0, and the 26 nonzero entries in the whole table match the original table one by one.
// Storing them in the chronicle would block quest progress: neither Deposit nor manual storing accepts them; ones already stored can still be withdrawn as usual
int dm_item_quest(uint8_t *b, uint64_t it) {
    uint8_t ver = *(uint8_t *)(it + 0x1bd); if (ver < 1 || ver > 3) return 0;
    uint64_t t = *(uint64_t *)(b + 0x2d64a18 + (uint64_t)ver * 16); uint64_t recs = t ? *(uint64_t *)(t + 0x15c0) : 0; uint32_t cls = *(uint32_t *)(it + 4);
    return recs && cls < *(uint32_t *)(t + 0x15c8) && *(uint8_t *)(recs + (uint64_t)cls * 0x1c0 + 0x13a) != 0;
}
static int item_key(uint8_t *b, uint64_t it, int *kind, int *idx) {   // 已鉴定的暗金 / 套装件：类型（1 暗金 / 0 套装）与表里的行号（物品数据 +0x34，同编年史登记 main+0x6bd760）；任务物品不收 | Identified unique / set item: type (1 unique / 0 set) and row number in the table (item data +0x34, same as the chronicle registration main+0x6bd760); quest items are not accepted
    uint64_t d = *(uint64_t *)(it + 0x10); if (*(uint32_t *)it != 4 || !d) return 0;
    uint32_t q = *(uint32_t *)d; if (q != 7 && q != 5) return 0;
    if (!(*(uint32_t *)(d + 0x18) & 0x10)) return 0;   // 未鉴定的不收 | Unidentified items are not accepted
    if (dm_item_quest(b, it)) return 0;
    *kind = q == 7; *idx = (int)*(uint32_t *)(d + 0x34); return 1;
}
// 存入一件：摘下 → 通知 → 释放（物品此刻必须在玩家自己的某个物品栏里：背包 / 储藏箱 / 方块，不收身上穿着的）
// Store one item: detach → notify → free (the item must at this moment be in one of the player's own item grids: inventory / stash / cube; equipped items are not accepted)
static int deposit(uint8_t *b, uint64_t G, uint64_t player, uint32_t id, const char *name) {
    uint64_t it = dm_diag_srv_item(id); int kind, idx; if (!it || !item_key(b, it, &kind, &idx)) return -1;
    uint64_t d = *(uint64_t *)(it + 0x10), pp = *(uint64_t *)(it + 0x38); if (!d || !*(uint64_t *)(d + 0xa0) || *(uint32_t *)(it + 0xc) != 0) return -2;   // 方式 0 = 在容器里（穿着 = 1、腰带 = 2 …） | Mode 0 = in a container (equipped = 1, belt = 2 ...)
    static uint8_t buf[VBYTES]; int len = dm_item_write(b, it, buf); if (!len) return -3;
    int old = vfind(kind, idx); uint8_t *keep = malloc((size_t)len); if (!keep) return -3; memcpy(keep, buf, (size_t)len);
    int page = *(uint8_t *)(d + 0x55), x = pp ? *(int32_t *)(pp + 0x10) : 0, y = pp ? *(int32_t *)(pp + 0x14) : 0;
    ((void (*)(uint64_t))(b + 0x55e430))(it);
    ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, int))(b + 0x642f70))(G, player, it, 0x20, 4, 0, page, y, x);
    ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, it);
    pend_add(1, kind, idx, keep, len);
    if (old >= 0) { free(g_v[old].data); g_v[old].data = keep; g_v[old].len = (uint16_t)len; }   // 交换的旧件已在调用处先取出 | The old item of a swap has already been withdrawn at the call site
    else { if (g_nv >= VMAX) { free(keep); return -4; } g_v[g_nv++] = (ventry){ (uint8_t)kind, (uint16_t)idx, (uint16_t)len, keep }; }
    LOG("编年史仓库：存入 %s %d（物品 id %u，原在页 %d 位置 %d,%d，位流 %d 字节）%s", kind ? "暗金" : "套装件", idx, id, page, x, y, len, old >= 0 ? "，替换旧件" : ""); (void)name;
    return 1;
}
// 取出一格：建物品（孔里的逐个建、镶回）→ 背包找空位，放不下就释放、仓库不动 → 放进背包并通知客户端；核实进了背包才从仓库里删
// 拖回背包指定落点：物品图中心 (fx, fy)（格）→ 左上角 = 四舍五入(中心 − 宽高 / 2)；越出背包或压到背包里别的物品（服务端物品栏、页 0、存放中，按各自左上角与宽高）就不放。
// 宽高 = 物品表行 +0x11e / +0x11f（同原版找空位 0x55f620 读的那两个字节）
// Withdraw one cell: build the item (socketed ones are built one by one and socketed back) → find a free spot in the inventory, and if it does not fit, free it and leave the vault untouched → put it into the inventory and notify the client; delete from the vault only after verifying it is in the inventory
// Dragged back to a specified drop point in the inventory: item image center (fx, fy) (cells) → top-left corner = round(center − size / 2); if it goes outside the inventory or overlaps another item in the inventory (server inventory structure, page 0, stored, by each one's top-left corner and size), it is not placed.
// Size = item table row +0x11e / +0x11f (the same two bytes the original game's free-spot search 0x55f620 reads)
static int bag_fits(uint8_t *b, uint64_t player, uint64_t nu, uint64_t at, uint32_t *ox, uint32_t *oy) {
    uint8_t ver = *(uint8_t *)(nu + 0x1bd); uint64_t t = ver >= 1 && ver <= 3 ? *(uint64_t *)(b + 0x2d64a18 + ver * 16) : 0; if (!t) return 0;
    uint64_t ir = *(uint64_t *)(t + 0x15c0); uint32_t in = *(uint32_t *)(t + 0x15c8);
#define ISZ(it, o) (*(uint32_t *)((it) + 4) < in ? *(uint8_t *)(ir + (uint64_t)*(uint32_t *)((it) + 4) * 0x1c0 + (o)) : 1)
    int w = ISZ(nu, 0x11e), h = ISZ(nu, 0x11f), cols = (int)(at >> 8 & 0xff), rows = (int)(at & 0xff);
    int x = (int)lround((int16_t)(at >> 32 & 0xffff) / 100.0 - w / 2.0), y = (int)lround((int16_t)(at >> 16 & 0xffff) / 100.0 - h / 2.0);
    if (x < 0 || y < 0 || x + w > cols || y + h > rows) { LOG("编年史仓库：拖回背包落点 (%d,%d) 大小 %d×%d 出了背包 %d×%d", x, y, w, h, cols, rows); return 0; }
    uint64_t inv = *(uint64_t *)(player + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0;
    for (int k = 0; it && k < 2048; k++) { uint64_t d = *(uint64_t *)(it + 0x10), pp = *(uint64_t *)(it + 0x38); if (!d) break;
        if (it != nu && *(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0 && pp) { int ix = *(int32_t *)(pp + 0x10), iy = *(int32_t *)(pp + 0x14), iw = ISZ(it, 0x11e), ih = ISZ(it, 0x11f);
            if (x < ix + iw && ix < x + w && y < iy + ih && iy < y + h) { LOG("编年史仓库：拖回背包落点 (%d,%d) 压到背包里 (%d,%d) 的物品", x, y, ix, iy); return 0; } }
        it = *(uint64_t *)(d + 0xb0); }
#undef ISZ
    *ox = (uint32_t)x; *oy = (uint32_t)y; return 1;
}
static int withdraw(uint8_t *b, uint64_t G, uint64_t player, int kind, int idx) {
    int k = vfind(kind, idx); if (k < 0) return -5;
    uint64_t pp = *(uint64_t *)(player + 0x38), room = pp ? *(uint64_t *)(pp + 0x20) : 0; if (!room) return -6;
    uint8_t *buf = g_v[k].data; int len = g_v[k].len; int socks = 0;
    uint64_t nu = dm_item_make(b, G, room, buf, len, &socks); if (!nu) return -7;
    // 只进背包（与仓库页规则一致）：原版「在格区里找空位」main+0x55f620(物品栏, 物品, 格区号, &x, &y, 0)，格区号 = 表 main+0x1dae684[玩家类 × 8 + 4]
    // （同 dm_actions.c 取多件、原版「放回背包」0x3bdb10）。放不下：物品释放、仓库不动，交主线程播原版错误音（同原版物品栏放不下）
    // Inventory only (consistent with the stash page rules): the original game's "find a free spot in a grid region" main+0x55f620(inventory structure, item, grid region id, &x, &y, 0), grid region id = table main+0x1dae684[player class × 8 + 4]
    // (same as taking multiple items in dm_actions.c and the original game's "put back to inventory" 0x3bdb10). If it does not fit: the item is freed, the vault is untouched, and the main thread is asked to play the original game's error sound (same as when the original inventory is full)
    { uint64_t inv = *(uint64_t *)(player + 0x90); uint32_t cls = *(uint32_t *)(player + 4), fx = 0, fy = 0;
      uint64_t at = atomic_exchange(&g_take_at, 0);   // 拖回背包：放在指定落点，放不下就不取（不改找别的空位） | Dragged back to the inventory: place at the specified drop point, and do not withdraw if it does not fit (no falling back to another free spot)
      if (at ? !bag_fits(b, player, nu, at, &fx, &fy) : (!inv || cls > 7 || !((int (*)(uint64_t, uint64_t, int, uint32_t *, uint32_t *, int))(b + 0x55f620))(inv, nu, *(int32_t *)(b + 0x1dae684 + cls * 8 + 4), &fx, &fy, 0))) {
          ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, nu); atomic_store(&g_full_snd, 1); LOG("编年史仓库：取出 %s %d —— 背包放不下，不取（仓库不动）", kind ? "暗金" : "套装件", idx); return -8; }
      uint64_t nd = *(uint64_t *)(nu + 0x10), ip = *(uint64_t *)(nu + 0x38); if (!nd || !ip) { ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, nu); return -9; }
      *(uint8_t *)(nd + 0x55) = 0; *(int32_t *)(ip + 0x10) = (int32_t)fx; *(int32_t *)(ip + 0x14) = (int32_t)fy;   // 背包页、找到的格 | Inventory page, the cell found
      uint64_t one = nu, ox = 0; ((int (*)(uint64_t, uint64_t, uint64_t *, uint64_t *))(b + 0x6f1940))(G, player, &one, &ox);   // 原版读档放物品：按物品自带的页 / 格放进属主物品栏（个人仓库翻页同一个） | The original game's load-time item placement: puts the item into the owner's inventory structure by the page / cell the item carries (the same one personal stash paging uses)
      if (!*(uint64_t *)(nd + 0xa0)) { ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, nu); LOG("【红】编年史仓库：取出 %s %d —— 放进背包没成（格 %u,%u），仓库不动", kind ? "暗金" : "套装件", idx, fx, fy); return -10; }
      ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, uint64_t))(b + 0x642f70))(G, player, nu, 1, 2, 0, 0, 0, (uint64_t)(fx & 0xffff) | (uint64_t)fy << 16);   // 通知客户端，同收包 0x18 的处理 0x6d0050 | Notify the client, same as the handler 0x6d0050 for received packet 0x18
    }
    uint64_t ip = *(uint64_t *)(nu + 0x38);
    pend_add(0, kind, idx, g_v[k].data, g_v[k].len);
    free(g_v[k].data); g_v[k] = g_v[--g_nv];
    LOG("编年史仓库：取出 %s %d → 背包（新 id %u，位置 %d,%d，孔里 %d 件）", kind ? "暗金" : "套装件", idx, *(uint32_t *)(nu + 8), ip ? *(int32_t *)(ip + 0x10) : -1, ip ? *(int32_t *)(ip + 0x14) : -1, socks);
    return 1;
}
// 测试开关 DM_TEST_ITEM_DUMP=<拍>（救档核对用）：到拍把服务端玩家身上全部物品与物品栏 +0x68 挂的附属单位（类 0 = 共享仓库五页、类 1 = 高级仓库）的物品逐件打出来：
// 所在 / 方式 / 页 / 格 / 物品码 / 品质（附属单位按 0x45f5f0(main+0x2d603a0, id & 0x7f, id, 0) 取，同 0x652650）
// Test switch DM_TEST_ITEM_DUMP=<tick> (for save-rescue verification): at that tick, print one by one every item on the server player and on the attached units hanging at inventory structure +0x68 (class 0 = the five shared stash pages, class 1 = advanced stash):
// location / mode / page / cell / item code / quality (attached units are fetched via 0x45f5f0(main+0x2d603a0, id & 0x7f, id, 0), same as 0x652650)
static void item_dump_list(uint8_t *b, uint64_t owner, const char *who) {
    uint64_t inv = owner ? *(uint64_t *)(owner + 0x90) : 0, it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; int n = 0;
    for (int k = 0; it && k < 4096; k++) { uint64_t d = *(uint64_t *)(it + 0x10), pp = *(uint64_t *)(it + 0x38); if (!d) break; uint32_t c = ((uint32_t (*)(uint64_t))(b + 0x570260))(it);
        LOG("物品清点 %s：%.4s 品质 %u 方式 %u 页 %u 格 %d,%d", who, (const char *)&c, *(uint32_t *)d, *(uint32_t *)(it + 0xc), *(uint8_t *)(d + 0x55), pp ? *(int32_t *)(pp + 0x10) : -1, pp ? *(int32_t *)(pp + 0x14) : -1); n++; it = *(uint64_t *)(d + 0xb0); }
    LOG("物品清点 %s：共 %d 件", who, n);
}
static void item_dump(uint8_t *b, uint64_t player) {
    item_dump_list(b, player, "角色");
    uint64_t inv = *(uint64_t *)(player + 0x90); int k = 0;
    for (uint64_t en = inv ? *(uint64_t *)(inv + 0x68) : 0; en && k < 32; en = *(uint64_t *)(en + 0x10), k++) { uint32_t uid = *(uint32_t *)en; char who[48];
        uint64_t u = ((uint64_t (*)(uint64_t, uint32_t, uint32_t, uint32_t))(b + 0x45f5f0))((uint64_t)(uintptr_t)(b + 0x2d603a0), uid & 0x7f, uid, 0);
        snprintf(who, sizeof who, "附属单位 %u（类 %u）", uid, *(uint8_t *)(en + 0xa)); item_dump_list(b, u, who); }
}
void dm_vault_tick(uint64_t G, uint64_t player, uint8_t *b) {   // 服务端拍（diag.c hud_tick）；不在局内 player 为 0 | Server tick (hud_tick in diag.c); player is 0 when not in a run
    static uint64_t lastp;
    if (!player) { lastp = 0; g_loaded = 0; return; }
    { static int done; const char *e = getenv("DM_TEST_ITEM_DUMP"); if (e && !done && *(uint32_t *)(G + 0x170) >= (uint32_t)atoi(e)) { done = 1; item_dump(b, player); } }
    if (player != lastp) { lastp = player; pthread_mutex_lock(&g_vmu); const char *nm = vault_name(b); g_loaded = nm != NULL; if (nm) { strlcpy(g_vname, nm, sizeof g_vname); vault_load(nm); } else vault_clear(); pthread_mutex_unlock(&g_vmu); atomic_fetch_add(&g_vgen, 1); }   // 进局（换了玩家单位）读一次 | Read once on entering a run (the player unit changed)
    { uint32_t dq = atomic_exchange(&g_desc_req, 0); if (dq) { pthread_mutex_lock(&g_vmu); vault_desc(b, G, player, dq); pthread_mutex_unlock(&g_vmu); } }
    { static uint32_t last; uint32_t t = *(uint32_t *)(G + 0x170); if (atomic_load(&g_vretry) && t - last >= 250) { last = t; LOG("编年史仓库：侧文件上次没写成，再请一次存盘 → %d", dm_request_save()); } }
    // 待落盘队列快满（一次交换要记取出 + 存入两条）：这一拍先不做存取，请求原样留着（存放按钮本来就等上一件被取走，dm_vault_put_pending），请游戏存盘；
    // 提交时队首得到证明、队列腾出来，下一拍再做。原先满了照做存取、只是不记：那件已从物品栏摘下，侧文件却永远不会写它，重进后消失
    // The pending-write queue is nearly full (one swap has to record two entries, withdraw + deposit): this tick does no deposit / withdraw yet, the request is kept as is (the deposit button waits for the previous piece to be taken away anyway, dm_vault_put_pending), and the game is asked to save;
    // at commit the head of the queue gets proven and the queue frees up, and it is done on the next tick. Previously when full the deposit / withdraw went ahead and was just not recorded: that piece had been taken off the inventory, yet the side file would never write it, and it vanished after re-entering
    { static int cap = -1; if (cap < 0) { const char *e = getenv("DM_TEST_VAULT_PEND"); cap = e ? atoi(e) : 0; if (cap >= 2 && cap <= VPEND) g_pend_cap = cap; }
      if (atomic_load(&g_req_put) || atomic_load(&g_req_take)) { pthread_mutex_lock(&g_vmu); int np = g_npend; pthread_mutex_unlock(&g_vmu);
          if (np > g_pend_cap - 2) { static uint32_t last; uint32_t t = *(uint32_t *)(G + 0x170); if (t - last >= 25) { last = t; LOG("编年史仓库：待落盘 %d 条（上限 %d），这次存取先等落盘，请游戏存盘 → %d", np, g_pend_cap, dm_request_save()); } return; } } }
    uint32_t put = atomic_exchange(&g_req_put, 0), take = atomic_exchange(&g_req_take, 0); if (!put && !take) return;
    pthread_mutex_lock(&g_vmu);
    if (!g_loaded) { pthread_mutex_unlock(&g_vmu); LOG("编年史仓库：这类角色没有共享仓库，不开编年史仓库"); return; }
    int r = 0;
    if (take) r = withdraw(b, G, player, (int)(take >> 16 & 0xff), (int)(take & 0xffff));
    if (put) { uint64_t it = dm_diag_srv_item(put); int kind, idx, swap = it && item_key(b, it, &kind, &idx) && vfind(kind, idx) >= 0;
        if (swap) { int w = withdraw(b, G, player, kind, idx); if (w < 0) { r = w; LOG("编年史仓库：存入物品 %u —— 同一格的旧件取不出来（%d），这次不存", put, w); goto done; } }   // 交换：旧件先回背包 | Swap: the old item goes back to the inventory first
        r = deposit(b, G, player, put, g_vname); if (r < 0 && it) { uint64_t d = *(uint64_t *)(it + 0x10); LOG("编年史仓库：不收的那件 类型 %u 品质 %u 标志 %#x 方式 %u", *(uint32_t *)it, d ? *(uint32_t *)d : 0, d ? *(uint32_t *)(d + 0x18) : 0, *(uint32_t *)(it + 0xc)); }
        if (r < 0) atomic_store(&g_full_snd, 1);   // 不收：同取不出，播原版错误音 | Rejected: same as a failed withdraw, play the original game's error sound
        if (r < 0) LOG("编年史仓库：存入物品 %u 不收（%d：不是已鉴定的暗金 / 套装件、是任务物品、不在容器里或写不出位流）", put, r); }
done:
    atomic_fetch_add(&g_vgen, 1);
    int np = g_npend; pthread_mutex_unlock(&g_vmu);
    if (np) { int s = getenv("DM_TEST_VAULT_NOSAVE") ? -9 : dm_request_save(); LOG("编年史仓库：仓库现有 %d 件、待落盘 %d 条，请游戏存盘 → %d", g_nv, np, s); }   // 测试开关 DM_TEST_VAULT_NOSAVE=1：存取后不请存盘（配合杀进程验「存取后、落盘前」掉电） | Test switch DM_TEST_VAULT_NOSAVE=1: do not request a save after a store / withdraw (used with killing the process to verify power loss "after store / withdraw, before flush")
}
// 提交前钩子（写盘线程，持着存档锁）：待落盘的存取按次序在原版存档里证明，证明了的并进盘上状态 g_d，有变化就写侧文件（规则见文件头）
// Pre-commit hook (disk write thread, holding the save lock): pending store / withdraw operations are proven in order against the original save, the proven ones are merged into the on-disk state g_d, and the sidecar file is written if anything changed (rules in the file header)
static void vault_pre_commit(const char *wd) {
    pthread_mutex_lock(&g_vmu);
    if ((g_npend || g_ddirty) && g_loaded) {
        dm_blob bl[64]; int nb = dm_save_blobs(wd, g_vname, bl, 64), done = 0; uint64_t cno = dm_fs_commit_count() + 1; double t = now_s();
        while (g_npend > 0) { vpend *p = &g_pend[0]; int have = dm_blobs_have(bl, nb, p->data, p->len), ok = 0; const char *why = "";
            int late = cno >= p->made + 2 && t - (double)p->t >= 10;   // 存取后第二次提交起、且过了 10 秒：这次提交的原版存档一定是存取之后序列化的 | From the second commit after the store / withdraw on, and more than 10 seconds later: the original save in this commit was definitely serialized after the store / withdraw
            if (p->put) { if (!have) { ok = 1; why = "存入：原版存档里已没有那件"; } else if (late && cno >= p->made + 3) { ok = 2; why = "【红】存入：存入后好几次提交原版存档里那件还在（存入没生效？），不并进、编年史里也去掉"; } }
            else { if (have) { ok = 1; why = "取出：原版存档里已有那件"; } else if (late) { ok = 1; why = "取出：之后的存档里哪都没有那件（卖掉 / 丢下 / 用掉）"; } }
            if (!ok) { LOG("编年史仓库：提交前核对 —— 第 1 条待落盘（%s %s %d）还没证明（这次提交 #%llu，记下于 #%llu 之后），这次及后面的都等", p->put ? "存入" : "取出", p->kind ? "暗金" : "套装件", p->idx, (unsigned long long)cno, (unsigned long long)p->made); break; }
            int di = dfind(p->kind, p->idx);
            if (ok == 1 && p->put) { if (di >= 0) { free(g_d[di].data); g_d[di].data = p->data; g_d[di].len = p->len; } else if (g_nd < VMAX) g_d[g_nd++] = (ventry){ p->kind, p->idx, p->len, p->data }; else free(p->data); p->data = NULL; }
            else if (ok == 1) { if (di >= 0 && g_d[di].len >= 16 && !memcmp(g_d[di].data + 8, p->data + 8, (size_t)p->n)) { free(g_d[di].data); g_d[di] = g_d[--g_nd]; } }
            else { int vi = vfind(p->kind, p->idx); if (vi >= 0 && g_v[vi].len == p->len && !memcmp(g_v[vi].data, p->data, p->len)) { free(g_v[vi].data); g_v[vi] = g_v[--g_nv]; atomic_fetch_add(&g_vgen, 1); } }
            LOG("编年史仓库：提交前核对 #%llu —— %s（%s %d）→ 并进盘上状态", (unsigned long long)cno, why, p->kind ? "暗金" : "套装件", p->idx);
            free(p->data); memmove(&g_pend[0], &g_pend[1], (size_t)(g_npend - 1) * sizeof g_pend[0]); g_npend--; done++; }
        dm_blobs_free(bl, nb);
        // 侧文件没写成：这次提交不许照常进行（并进盘上状态的那几件已不在原版存档里，侧文件里又没有，提交后被杀就是丢件）。
        // 走存档事务自己的写失败口子：这次提交被拒绝、已提交版不变（同存档写失败，前端有提示）；盘上状态留在内存里标脏，服务端拍每 10 秒请一次存盘，写成为止
        // The side file failed to write: this commit must not go ahead as usual (the pieces merged into the on-disk state are no longer in the original save and are not in the side file either; being killed after the commit means lost items).
        // Take the save transaction's own write-failure path: this commit is refused and the committed version is unchanged (same as a save write failure, the front end shows a notice); the on-disk state stays in memory marked dirty, and the server tick requests a save every 10 seconds until the write succeeds
        if (done || g_ddirty) { if (vault_save(wd, g_vname) == 0) { LOG("编年史仓库：侧文件随这次提交落盘（盘上 %d 件、内存 %d 件、还有 %d 条待证明）", g_nd, g_nv, g_npend); g_ddirty = 0; atomic_store(&g_vretry, 0); }
            else { g_ddirty = 1; atomic_store(&g_vretry, 1); dm_fs_save_poison(EIO); LOG("【红】编年史仓库：侧文件没写成 → 这次存档提交拒绝，之后重试（盘上状态 %d 件留在内存）", g_nd); } }
    }
    pthread_mutex_unlock(&g_vmu);
}
__attribute__((constructor)) static void vault_hook_install(void) { extern void (*g_dm_pre_commit_hook[5])(const char *); g_dm_pre_commit_hook[1] = vault_pre_commit; }
