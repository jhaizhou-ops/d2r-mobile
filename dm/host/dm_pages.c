// 个人仓库 2–5 页。
// 角色存档里的个人仓库格（玩家物品栏页 4）放「当前页」；另外 4 页的物品位流放侧文件 <角色>/<角色>.swpage（存档工作目录里的角色目录，随存档提交原子换入；
// iCloud 同步按角色整个目录打包，侧文件跟着走，dm_cloudsync.c）。翻页在服务端拍里做（diag.c hud_tick 调 dm_pages_tick），全用原版函数：
//   搬出 = 写位流 main+0x581b60 → 从物品栏摘下 0x55e430 → 通知客户端 0x642f70(…, 0x20, 4, 0, 页, y, x) → 释放 0x658be0；
//   放回 = 读位流建物品 0x5793a0 + 0x6564e0（孔里的照原版复制物品 0x65a840 逐个建、0x64d080 镶回）→ 原版读档放物品 0x6f1940（按物品自带的页 / 格放进属主物品栏）
//          → 通知客户端 0x642f70(…, 1, 2, 0, 页, 0, x | y << 16)（同收包 0x18 的处理 0x6d0050）。
// 界面在 dm_cursor.c（个人页左右箭头、页码）。实测与坑：设计文档「个人仓库 2–5 页：实现与实测」。
// Personal stash pages 2–5.
// The personal stash grid in the character save (player item container page 4) holds the "current page"; the item bitstreams of the other 4 pages go in the sidecar file <character>/<character>.swpage (in the character directory inside the save working directory, swapped in atomically with the save commit;
// iCloud sync packs the whole directory per character, so the sidecar file goes along, dm_cloudsync.c). Page flips are done in the server tick (hud_tick in diag.c calls dm_pages_tick), using only original game functions:
//   move out = write bitstream main+0x581b60 -> remove from the item container 0x55e430 -> notify the client 0x642f70(..., 0x20, 4, 0, page, y, x) -> free 0x658be0;
//   put back = read bitstream and build item 0x5793a0 + 0x6564e0 (socketed ones are built one by one following the original game's item copy 0x65a840 and socketed back with 0x64d080) -> the original game's load-time item placement 0x6f1940 (puts it into the owner's item container by the item's own page / cell)
//          -> notify the client 0x642f70(..., 1, 2, 0, page, 0, x | y << 16) (same as the handler 0x6d0050 for received packet 0x18).
// UI is in dm_cursor.c (left / right arrows on the personal page, page number). Measurements and pitfalls: design doc 「个人仓库 2–5 页：实现与实测」(Personal stash pages 2–5: implementation and measurements).
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <dirent.h>
#include "dm.h"

#define VBYTES 1024   // 单件位流上限（含孔里的） | Upper limit of a single item's bitstream (including socketed ones)
const char *dm_save_workdir(void);   // hle_fs.c：事务式存档的工作目录 | hle_fs.c: the working directory of the transactional save
int dm_request_save(void);           // diag.c：请游戏走一次它自己的定时存盘 | diag.c: ask the game to run one of its own timed saves
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;   // 服务端拍（翻页）与写盘线程（提交前钩子）都碰页数据 | Both the server tick (page flip) and the disk-write thread (pre-commit hook) touch the page data

int dm_item_write(uint8_t *b, uint64_t it, uint8_t *buf) {   // 原版写位流（模板 main+0x65a840）：写入器 {缓冲, 容量位, 字节, 余位, 错误}，上下文 = GOT 虚表 + 0x10 | The original game's bitstream write (template main+0x65a840): writer {buffer, capacity in bits, byte, remaining bits, error}, context = GOT vtable + 0x10
    struct { uint64_t buf, cap, n, bits; uint32_t err, pad; uint8_t rest[0x60]; } w; uint64_t ctx[2] = { *(uint64_t *)(b + 0x211feb8) + 0x10, 0 };
    memset(&w, 0, sizeof w); memset(buf, 0, VBYTES); w.buf = (uint64_t)buf; w.cap = VBYTES * 8;
    ((void (*)(uint64_t, void *, int, int, int, void *))(b + 0x581b60))(it, &w, 1, 1, 0, ctx);
    int len = w.err ? 0 : (int)w.n + (w.bits != 0); return len > 0 && len <= VBYTES ? len : 0;
}
// 读位流建物品（含孔里镶的，照原版复制物品 0x65a840：母件后面紧跟孔里的件数 = 信息块 +0x14），清「新物品」位。room = 落在哪个房间（0 = 不进房间，放进物品栏用）
// Read a bitstream and build the item (including socketed ones, following the original game's item copy 0x65a840: the parent item is immediately followed by the socketed items, count = info block +0x14), clearing the "new item" bit. room = which room it lands in (0 = not placed in a room, used when putting it into an item container)
uint64_t dm_item_make_n(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out, int *bytes_out);
uint64_t dm_item_make(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out) { return dm_item_make_n(b, G, room, buf, len, socks_out, NULL); }
// 同上，另给出这件（连同孔里的）在位流里占了多少字节（bytes_out）：从一页里连续排着的物品逐件拆开用（dm_cloudmerge.c）
// Same as above, and also reports how many bytes this item (with its socketed items) takes in the bitstream (bytes_out): used to split items packed consecutively in a page one by one (dm_cloudmerge.c)
uint64_t dm_item_make_n(uint8_t *b, uint64_t G, uint64_t room, const uint8_t *buf, int len, int *socks_out, int *bytes_out) {
    uint8_t info[0x100]; int used = 0; memset(info, 0, sizeof info);
    ((void (*)(int, const void *, int64_t, int, void *, int))(b + 0x5793a0))(*(uint8_t *)(G + 0x106), buf, len, 1, info, 0x69);
    uint64_t nu = ((uint64_t (*)(uint64_t, uint64_t, const void *, int, int, void *, int *, int))(b + 0x6564e0))(G, room, buf, len, 1, info, &used, 0x69);
    if (!nu) return 0;
    { uint64_t nd = *(uint64_t *)(nu + 0x10); if (nd) *(uint32_t *)(nd + 0x18) &= ~0x2000u; }
    int socks = *(int32_t *)(info + 0x14), off = used, rem = len - used, okn = 0;
    for (int s = 0; s < socks && rem > 0; s++) { uint8_t ci[0x100]; int cu = 0; memset(ci, 0, sizeof ci);
        ((void (*)(int, const void *, int64_t, int, void *, int))(b + 0x5793a0))(*(uint8_t *)(G + 0x106), buf + off, rem, 1, ci, 0x69);
        uint64_t ch = ((uint64_t (*)(uint64_t, uint64_t, const void *, int, int, void *, int *, int))(b + 0x6564e0))(G, 0, buf + off, rem, 1, ci, &cu, 0x69);
        if (!ch || cu <= 0) { LOG("【红】建物品：孔里第 %d 件建不出来", s + 1); break; }
        ((void (*)(uint64_t, int))(b + 0x5ca8a0))(ch, 4);
        if (!((int (*)(uint64_t, uint64_t, uint32_t, uint32_t, int))(b + 0x64d080))(G, nu, *(uint32_t *)(ch + 8), *(uint32_t *)(nu + 8), 2)) LOG("【红】建物品：孔里第 %d 件镶不回去", s + 1); else okn++;
        { uint64_t cd = *(uint64_t *)(ch + 0x10); if (cd) *(uint32_t *)(cd + 0x18) &= ~0x2000u; }
        off += cu; rem -= cu; }
    if (socks_out) *socks_out = okn; if (bytes_out) *bytes_out = off; return nu;
}
static uint64_t pf_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void *dm_memfind(const uint8_t *h, size_t hn, const uint8_t *n, size_t nn) { if (!nn || hn < nn) return NULL; for (size_t i = 0; i + nn <= hn; i++) if (h[i] == n[0] && !memcmp(h + i, n, nn)) return (void *)(h + i); return NULL; }
// 工作目录里的原版存档：各角色档 <角色>/<角色>.d2s 与这一类共享仓库档（<名>/<名>.d2i 或 <名>.d2i），整份读进内存，供提交前核对「这件物品在不在原版存档里」
// （编年史仓库 dm_vault.c、个人仓库多页共用）。stash 为空就只读角色档
// The original saves in the working directory: each character save <character>/<character>.d2s and the shared stash saves of this kind (<name>/<name>.d2i or <name>.d2i), read whole into memory for the pre-commit check "is this item in the original save"
// (shared by the chronicle vault dm_vault.c and the multi-page personal stash). If stash is empty, only character saves are read
int dm_save_blobs(const char *wd, const char *stash, dm_blob *out, int max) {
    int k = 0; DIR *dd = opendir(wd); struct dirent *e; char p[1400];
    #define BLOB(path) do { FILE *f_; if (k < max && (f_ = fopen(path, "rb"))) { fseek(f_, 0, SEEK_END); long L_ = ftell(f_); fseek(f_, 0, SEEK_SET); \
        uint8_t *m_ = L_ > 0 && L_ < (16 << 20) ? malloc((size_t)L_) : NULL; size_t n_ = m_ ? fread(m_, 1, (size_t)L_, f_) : 0; fclose(f_); if (m_ && n_) { out[k].d = m_; out[k].n = n_; k++; } else free(m_); } } while (0)
    while (dd && (e = readdir(dd))) { if (e->d_name[0] == '.') continue; snprintf(p, sizeof p, "%s/%s/%s.d2s", wd, e->d_name, e->d_name); BLOB(p); }
    if (dd) closedir(dd);
    if (stash && stash[0]) { snprintf(p, sizeof p, "%s/%s/%s.d2i", wd, stash, stash); BLOB(p); snprintf(p, sizeof p, "%s/%s.d2i", wd, stash); BLOB(p); }
    #undef BLOB
    return k;
}
// 这件（位流）在不在这些存档里：比位流第 8 字节起最多 48 字节（跳过标志位与位置字段，物品挪了地方也认得）。只对有自己身份的物品可靠：
// 简单物品（位流标志位 0x200000，如同种符文、药水）没有唯一编号，两颗同种符文字节一样——调用方别拿它判「同一件」
// Whether this item (bitstream) is in these saves: compare up to 48 bytes of the bitstream starting from byte 8 (skipping the flag bits and position fields, so an item is still recognized after being moved). Reliable only for items with their own identity:
// simple items (bitstream flag bit 0x200000, e.g. runes or potions of the same kind) have no unique id, and two runes of the same kind have identical bytes -- callers must not use it to decide "the same item"
int dm_blobs_have(const dm_blob *bl, int nb, const uint8_t *bits, int len) { int n = len - 8 > 48 ? 48 : len - 8; if (n <= 8) return 0;
    for (int i = 0; i < nb; i++) if (dm_memfind(bl[i].d, bl[i].n, bits + 8, (size_t)n)) return 1; return 0; }
void dm_blobs_free(dm_blob *bl, int nb) { for (int i = 0; i < nb; i++) free(bl[i].d); }
int dm_bits_simple(const uint8_t *bits, int len) { return len >= 4 && (bits[2] & 0x20); }   // 位流标志位 0x200000 = 简单物品（没有唯一编号） | Bitstream flag bit 0x200000 = simple item (no unique id)
// ---------- 个人仓库 2–5 页 ----------
// 角色存档里的个人仓库格（玩家物品栏页 4）放「当前页」；另外 4 页的物品位流放侧文件 <角色>/<角色>.swpage（存档工作目录里，随存档提交原子换入）。
// 翻页（服务端拍）：当前格整页搬出（写位流 → 摘下 0x55e430 → 通知 → 释放）存进当前页的槽，再把目标页逐件建出来（含孔里的）按原格放回（读档放物品 0x6f1940），
// 页码改成目标页，请游戏存盘。同原版共享页：第 1 页往左、第 5 页往右不动。
// 落盘一致性（不变式：同一件物品只在一处——侧文件的页里，或原版存档里）：翻完一次、落盘前不接受下一次翻页，待定的只有「翻页前 / 后」两态。
// 翻页时留两份快照：搬出页 A（翻页前格里的）、目标页 B（放进格里的），每件带位置的完整位流；待定期间每次提交侧文件都带上这两份（待定记录）。
// 提交前钩子先看事实：这次要提交的本角色档已不是翻页时那份（整份指纹 ≠ g_pH0，见 g_pH0 的注释）→ 翻页后序列化的 → 翻页后，不管格里是什么
//   （从空页翻来、第一次存盘前就把件全拿走，格和翻页前一样也判得出）。拿不到这条事实时再看格：
//   格 = B → 翻页后；格 = A → 翻页前，带待定记录写、再请一次存盘；两份都等判翻页后；都不等 = 翻页后又动过格 → 翻页后。
//   位置在位流里、一格只放得下一件，别处有同字节的复制品不影响（丢整页的病根）。
// 进局读到待定记录：按格里实际的件同一规则落定（pages_resolve）。
// 进局读侧文件时页里某件（有自己身份的）字节在原版存档里也有一份，只记不删（两件同字节是可能的，删了就是丢）
// ---------- Personal stash pages 2–5 ----------
// The personal stash grid in the character file (player inventory page 4) holds the 'current page'; the item bit streams of the other 4 pages live in the side file <character>/<character>.swpage (in the save working directory, swapped in atomically with each save commit).
// Page turn (server tick): the whole current grid is moved out (write bit stream -> remove 0x55e430 -> notify -> free) into the current page's slot, then the target page is built item by item (including socketed items) and put back in the original cells (load-and-place 0x6f1940),
// the page number is set to the target page and the game is asked to save. Like the original shared pages: page 1 does not go left, page 5 does not go right.
// On-disk consistency (invariant: an item is in exactly one place - a page in the side file, or the original save): after a turn, no further turn is accepted until it is on disk; the pending state is only 'before / after the turn'.
// At a turn two snapshots are kept: turned-away page A (grid before the turn) and target page B (placed into the grid), each item as the full bit stream with its position; while pending, every commit writes both with the side file (the pending record).
// The pre-commit hook looks at the fact first: this character file about to be committed is no longer the copy from the turn (whole-file fingerprint != g_pH0, see the g_pH0 comment) -> serialized after the turn -> after, whatever the grid holds
//   (also correct when turning from an empty page and taking everything out before the first save, where the grid looks the same as before). Without the fact, look at the grid:
//   grid = B -> after; grid = A -> before, write with the pending record and request another save; both equal -> after; neither = the grid changed after the turn -> after.
//   The position is in the bit stream and a cell holds only one item, so same-byte copies elsewhere do not matter (the root cause of losing a whole page).
// On entering the game with a pending record: settle it by the actual items in the grid with the same rule (pages_resolve).
// When reading the side file on entry, an item (with its own identity) whose bytes also exist in the original save is logged, not removed (two items with the same bytes are possible; removing one loses it)
#define PGN 5
typedef struct { uint8_t x, y; uint16_t len; uint8_t *bits; } pitem;
typedef struct { int n; pitem *it; } ppage;
static ppage g_pp[PGN]; static int g_pcur, g_pload; static char g_pchar[64];
static int g_ppend, g_pdirty;   // 待定翻页：[0] 搬出的、[1] 搬进的；g_pdirty = 进局去重改了页，下次提交要写 | Pending flip: [0] moved out, [1] moved in; g_pdirty = dedup on entering the run changed a page, to be written on the next commit
static uint64_t g_pmade; static int g_pmore; static char g_pstash[64];   // 翻页时已提交过几次、翻页后 [G+0x5800] 的值、之后看到过真的序列化、钩子要再请一次存盘、共享仓库名 | Number of commits already made at flip time, the value of [G+0x5800] after the flip, a real serialization has been seen since, the hook needs to request another save, shared stash name
uint64_t dm_fs_commit_count(void); const char *dm_stash_name(uint8_t *b);
static _Atomic int g_preq, g_pcur_pub;
// 待定翻页记录（翻完、侧文件还没按「翻页后」落定之前）：每次提交都随侧文件写下去，进局时拿仓库格的实际内容和两份整页快照比、落定（pages_resolve）。
// 原先待定期间不写侧文件：翻页后第一次存盘就把「格已搬空」的角色档提交了，侧文件要等第二次存盘（节流 3 秒）才写，期间被杀，搬出的那页哪里都没有（个人仓库清空）。
// A = 从仓库格搬出的（翻页前格里的）；B = 放进仓库格的（翻页后格里的）；S0 = 翻页前「搬出页」槽里原有的（放不回的残留，通常空）。翻页前的页槽：[from] = S0，[to] = B + 现在 [to] 里放不回的
// Pending page turn record (after a turn, until the side file is settled as 'after the turn'): written with the side file at every commit; on entering the game the actual grid content is compared with the two whole-page snapshots to settle it (pages_resolve).
// Previously the side file was not written while pending: the first save after the turn committed the character file with the grid already emptied, the side file waited for the second save (3-second throttle), and if killed in between the turned-away page was nowhere (personal stash emptied).
// A = moved out of the grid (grid before the turn); B = placed into the grid (grid after the turn); S0 = what the turned-away page slot held before (leftovers that could not be placed, usually empty). Page slots before the turn: [from] = S0, [to] = B + whatever in [to] could not be placed now
static int g_pfrom, g_pto; static uint64_t g_pH0; static ppage g_pA, g_pB, g_pS0;
// 「这份角色档是翻页之后序列化的」怎么证（事实，不计数、不看墙钟）：
//   翻页时若没有「已序列化、还没写完」的角色档（g_ser_infl = 0），翻页前的内容只可能是工作目录里现在这份 d2s，记下它的整份指纹 g_pH0；
//   之后提交的角色档指纹 ≠ g_pH0，就只能是翻页之后序列化的。g_pH0 = 0 表示翻页时拿不到这条事实（顺延等不到），只按格判。
//   不用「我们 fs 层写 d2s 的先后序号」：角色档由另一条线程写，序列化在翻页前、写盘在翻页后的交错真实存在（dm_vault.c 文件头「定时存盘的序列化与写盘之间夹着存取」；
// after2 实测档头时刻 20:04:28、写盘 20:04:31），写序号只证「写在后」，证不了「序列化在后」，满页翻空页会复制整页。
//   也不用档头时刻对墙钟：玩家改系统时间 / 网络对时往回拨会颠倒先后。
// g_ser_infl：服务端拍看到序列化（[G+0x5800] 变成真时刻；与翻页同在服务端拍上，先后确定），且这一拍窗口里角色档还没写完 → 在途；之后看到角色档写完（hle_fs 的序号更大）→ 不在途。
//   假「在途」只多等，安全；假「不在途」要「上一次存盘的角色档恰好在这一拍才写完、这一次的还没写」——存盘间隔秒级、写盘毫秒级，不会同拍。
// g_ser_sec：最近一次看到序列化那一拍的用户时钟秒；同一秒里翻页前刚存过就顺延到下一秒（只防「翻页后同秒再存、内容逐字节相同」，用相等判、改时钟不会长卡）
// How 'this character file was serialized after the page turn' is proven (a fact; no counting, no wall clock):
//   If at the turn no character file is serialized but not yet written (g_ser_infl = 0), the pre-turn content can only be the d2s now in the working directory; record its whole-file fingerprint g_pH0.
//   A later committed character file whose fingerprint != g_pH0 can only have been serialized after the turn. g_pH0 = 0 means the fact was not available at the turn (deferral timed out); judge by the grid only.
//   Not the order of our fs-layer d2s writes: the character file is written by another thread, and serialization before the turn with the write after it really happens (header of dm_vault.c: 'between serialization and write of the timed save, a store/take can happen';
// measured: header time 20:04:28, written 20:04:31). A write order only proves 'written later', not 'serialized later'; turning a full page to an empty one would duplicate the page.
//   Not the header time against the wall clock either: the player changing the system time / network time sync going back would reverse the order.
// g_ser_infl: the server tick sees a serialization ([G+0x5800] became a real time; on the same server tick as page turns, so the order is fixed) and the character file has not finished writing within this tick's window -> in flight; once a finished write is seen (a larger hle_fs number) -> not in flight.
//   A false 'in flight' only waits longer, which is safe; a false 'not in flight' would need the previous save's character file to finish exactly in this tick while this one is not yet written - saves are seconds apart and writes take milliseconds, so not in the same tick.
// g_ser_sec: user clock second of the tick where the last serialization was seen; if a save happened earlier in the same second, defer the turn to the next second (only guards against a byte-identical save later in the same second; compared for equality, so clock changes cannot stall it for long)
static int g_ser_infl; static uint64_t g_ser_seq; static int64_t g_ser_sec = -1; static uint32_t g_ser_tq, g_tq_now;
int64_t dm_user_clock(void); int dm_user_clock_fixed(void); uint64_t dm_seq_next(void); uint64_t dm_fs_d2s_wseq(void);   // hle_time.c / hle_fs.c
// 给 iCloud 并回：0 = 没有在途的存盘（放件时可记指纹）；1 = 在途、50 拍内（等）；2 = 在途超过 50 拍还没见写完（不等了，这次拿不到「放好后序列化」的事实，只认在存档里找到）
// For the iCloud merge: 0 = no save in flight (a fingerprint can be taken at placement); 1 = in flight, within 50 ticks (wait); 2 = in flight for more than 50 ticks with no finished write seen (stop waiting; the 'serialized after placing' fact is not available this time, only 'found in the saves' counts)
int dm_ser_state(void) { return !g_ser_infl ? 0 : g_tq_now - g_ser_tq < 50 ? 1 : 2; }
uint64_t dm_fnv64(const uint8_t *d, size_t n);
uint64_t dm_fnv64(const uint8_t *d, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; } return h ? h : 1; }
static uint8_t *d2s_read(const char *wd, size_t *n) {   // 工作目录里本角色档整份；读不到返回 NULL | This character's whole file in the working directory; NULL if it cannot be read
    char cp[1400]; snprintf(cp, sizeof cp, "%s/%s/%s.d2s", wd, g_pchar, g_pchar); FILE *f = fopen(cp, "rb"); uint8_t *d = NULL; *n = 0; if (!f) return NULL;
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); d = L > 0 && L < (16 << 20) ? malloc((size_t)L) : NULL; *n = d ? fread(d, 1, (size_t)L, f) : 0; fclose(f);
    if (!*n) { free(d); d = NULL; } return d; }
int dm_pages_cur(void) { return atomic_load(&g_pcur_pub); }   // 界面读：当前第几页（0 起） | UI read: the current page number (0-based)
int dm_pages_count(void) { return PGN; }
void dm_pages_request_flip(int dir) { atomic_store(&g_preq, dir < 0 ? -1 : 1); }
static void ppage_free(ppage *p) { for (int i = 0; i < p->n; i++) free(p->it[i].bits); free(p->it); p->n = 0; p->it = NULL; }
static void pitem_dup(ppage *p, const pitem *it) { uint8_t *c = malloc(it->len); pitem *m = realloc(p->it, sizeof(pitem) * (size_t)(p->n + 1)); if (m) p->it = m; if (!c || !m) { free(c); return; } memcpy(c, it->bits, it->len); p->it[p->n] = *it; p->it[p->n++].bits = c; }
static void pend_clear(void) { ppage_free(&g_pA); ppage_free(&g_pB); ppage_free(&g_pS0); }
static void pages_clear(void) { pend_clear(); for (int i = 0; i < PGN; i++) ppage_free(&g_pp[i]); g_pcur = 0; g_ppend = 0; g_pdirty = 0; atomic_store(&g_pcur_pub, 0); }
static void pages_path(char *p, size_t n, const char *wd) { snprintf(p, n, "%s/%s/%s.swpage", wd, g_pchar, g_pchar); }
static int ppage_read(FILE *f, ppage *p) {   // 一页：件数 u16，每件 x、y、长度 u16、位流；读坏返回 −1（读好的件留着） | One page: item count u16, then per item x, y, length u16, bit stream; returns -1 on a bad read (items read so far are kept)
    uint8_t c[2]; if (fread(c, 1, 2, f) != 2) return -1; int n = *(uint16_t *)c; if (!n) return 0;
    if (!(p->it = calloc((size_t)n, sizeof(pitem)))) return -1;
    for (int i = 0; i < n; i++) { uint8_t e[4]; if (fread(e, 1, 4, f) != 4) return -1; pitem *it = &p->it[i]; it->x = e[0]; it->y = e[1]; it->len = *(uint16_t *)(e + 2);
        if (!it->len || it->len > VBYTES || !(it->bits = malloc(it->len)) || fread(it->bits, 1, it->len, f) != it->len) { free(it->bits); it->bits = NULL; return -1; } p->n++; }
    return 0;
}
static int ppage_write(FILE *f, const ppage *p) { uint16_t n = (uint16_t)p->n; int ok = fwrite(&n, 1, 2, f) == 2;
    for (int i = 0; i < n && ok; i++) { const pitem *it = &p->it[i]; uint8_t e[4] = { it->x, it->y, 0, 0 }; *(uint16_t *)(e + 2) = it->len; ok = fwrite(e, 1, 4, f) == 4 && fwrite(it->bits, 1, it->len, f) == it->len; }
    return ok; }
static int pitem_same(const pitem *a, const pitem *b) { return a->x == b->x && a->y == b->y && a->len == b->len && !memcmp(a->bits, b->bits, a->len); }   // 同一件：同一格、完整位流相同（同提交前核对） | Same item: same cell and identical full bit stream (same as the pre-commit check)
static int ppage_overlap(const ppage *g, const ppage *s) {   // 两份里能一一配上的件数（每件只配一次） | Number of items that pair up one-to-one between the two (each item paired once)
    int used[256] = { 0 }, k = 0; if (s->n > 256) return 0;
    for (int i = 0; i < g->n; i++) for (int j = 0; j < s->n; j++) if (!used[j] && pitem_same(&g->it[i], &s->it[j])) { used[j] = 1; k++; break; }
    return k; }
// 进局落定待定翻页：同提交前核对——角色档已不是翻页时那份（指纹 ≠ g_pH0）→ 翻页后；否则仓库格（这次读进来的角色档）= A 且 ≠ B 才是翻页前，其余翻页后。
// 两种落定都让每件只在一处：翻页后 = 已读的页槽原样；翻页前 = [from] 换回 S0、B 并回 [to]
// Settle a pending page turn on entering the game: same as the pre-commit check - the character file is no longer the copy from the turn (fingerprint != g_pH0) -> after; otherwise only grid (the character file just read) = A and != B means before, anything else after.
// Both outcomes keep every item in exactly one place: after = the page slots as read; before = [from] gets S0 back and B is merged back into [to]
static void pages_resolve(uint8_t *b, uint64_t player) {
    ppage g = { 0, NULL }; static uint8_t buf[VBYTES];
    uint64_t inv = *(uint64_t *)(player + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0;
    for (int k = 0; it && k < 2048 && g.n < 256; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break;
        if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 4) { uint64_t pp = *(uint64_t *)(it + 0x38); int len = dm_item_write(b, it, buf);
            if (len) { pitem t = { (uint8_t)(pp ? *(int32_t *)(pp + 0x10) : 0), (uint8_t)(pp ? *(int32_t *)(pp + 0x14) : 0), (uint16_t)len, buf }; pitem_dup(&g, &t); } }
        it = *(uint64_t *)(d + 0xb0); }
    int na = ppage_overlap(&g, &g_pA), nb = ppage_overlap(&g, &g_pB), eqa = na == g.n && na == g_pA.n, eqb = nb == g.n && nb == g_pB.n;
    const char *wd = dm_save_workdir(); size_t dn = 0; uint8_t *d = wd ? d2s_read(wd, &dn) : NULL; uint64_t h = d ? dm_fnv64(d, dn) : 0; free(d);
    int after = g_pH0 && h && h != g_pH0, pre = !after && eqa && !eqb;   // 同提交前核对：角色档不是翻页时那份 → 翻页后；否则格 = 搬出页才是翻页前 | Same as the pre-commit check: the character file is not the copy from the turn -> after; otherwise only grid = turned-away page means before
    if (pre) { ppage_free(&g_pp[g_pfrom]); g_pp[g_pfrom] = g_pS0; g_pS0 = (ppage){ 0, NULL };
        for (int i = 0; i < g_pB.n; i++) pitem_dup(&g_pp[g_pto], &g_pB.it[i]); g_pcur = g_pfrom; }
    else g_pcur = g_pto;
    LOG("个人仓库：落定上次没证明完的翻页（第 %d → %d 页）：格里 %d 件，配上搬出页 %d / %d 件、配上搬进页 %d / %d 件，角色档%s → 存档是翻页%s的，当前第 %d 页",
        g_pfrom + 1, g_pto + 1, g.n, na, g_pA.n, nb, g_pB.n, after ? "已不是翻页时那份" : g_pH0 ? "还是翻页时那份" : "（翻页时没记指纹）", pre ? "前" : "后", g_pcur + 1);
    ppage_free(&g); pend_clear(); g_pdirty = 1;   // 落定后的页槽下次提交写回（不再带待定记录） | Settled page slots are written back at the next commit (without the pending record)
}
static void pages_load(uint8_t *b, uint64_t player) {   // 进局读一次（工作目录里的那份 = 上次提交的） | Read once on entering a run (the copy in the working directory = the last committed one)
    pages_clear(); const char *wd = dm_save_workdir(); if (!wd || !g_pchar[0]) return;
    char p[1200]; pages_path(p, sizeof p, wd); FILE *f = fopen(p, "rb"); if (!f) { LOG("个人仓库：%s 还没有翻过页（只有第 1 页）", g_pchar); return; }
    uint8_t h[8]; int bad = 0, tot = 0, pend = 0;   // 头：SWPG、版本（1 / 2）、当前页、有没有待定翻页记录（版本 2） | Header: SWPG, version (1 / 2), current page, whether a pending page turn record follows (version 2)
    if (fread(h, 1, 8, f) != 8 || memcmp(h, "SWPG", 4) || (h[4] != 1 && h[4] != 2) || h[5] >= PGN) bad = 1;
    else { g_pcur = h[5]; pend = h[4] == 2 && h[6];
        for (int pg = 0; pg < PGN && !bad; pg++) { if (ppage_read(f, &g_pp[pg])) bad = 1; tot += g_pp[pg].n; }
        if (!bad && pend) { uint8_t ft[2]; if (fread(ft, 1, 2, f) != 2 || ft[0] >= PGN || ft[1] >= PGN || fread(&g_pH0, 8, 1, f) != 1 || ppage_read(f, &g_pA) || ppage_read(f, &g_pB) || ppage_read(f, &g_pS0)) bad = 1; else { g_pfrom = ft[0]; g_pto = ft[1]; } } }
    fclose(f);
    if (bad) { LOG("【红】个人仓库：%s 坏了（读到 %d 件），只用读好的部分", p, tot); if (pend) { LOG("【红】个人仓库：待定翻页记录没读全，不落定（按已读的页槽）"); pend_clear(); pend = 0; } }
    if (pend) pages_resolve(b, player);
    { dm_blob bl[64]; int nb = dm_save_blobs(wd, g_pstash, bl, 64), drop = 0;   // 去重：页里有自己身份的那件，字节已在原版存档里 = 复制，从页里去掉（下次提交写回） | Dedup: an item in a page that has its own identity and whose bytes are already in the original save = a duplicate; remove it from the page (written back on the next commit)
      for (int pg = 0; pg < PGN; pg++) for (int i = 0; i < g_pp[pg].n; ) { pitem *it = &g_pp[pg].it[i];
          // 只记不删：原先当成复制从侧文件删掉，可字节相同的另一份也可能是别处还回来的复制品（iCloud 并回没划掉、每次进局再还一份），
          // 删掉的反而是玩家放进个人仓库的那份原件（个人仓库清空）。复制的来源已在源头堵上（翻页待定记录落定、并回清单按全部存档划掉），这里只留读数
          // Log only, do not remove: these used to be removed from the side file as duplicates, but the other same-byte copy may itself be a duplicate returned from elsewhere (the iCloud merge not crossing it off and returning another copy on every entry),
          // so what got removed was the original the player had put in the personal stash (personal stash emptied). The sources of duplication are closed at the root (pending page turns settled, merge list crossed off against all saves); only the reading is kept here
          if (!dm_bits_simple(it->bits, it->len) && dm_blobs_have(bl, nb, it->bits, it->len)) { LOG("【红】个人仓库：第 %d 页格 %d,%d 那件的字节在原版存档里也有一份（不删，只记）", pg + 1, it->x, it->y); drop++; } i++; }
      dm_blobs_free(bl, nb); (void)drop; }   // 只记不删：页槽没改，不因此回写（落定翻页设的 g_pdirty 保留） | Log only, do not remove: the page slots did not change, so no write-back for this (g_pdirty set by settling a turn is kept)
    atomic_store(&g_pcur_pub, g_pcur); tot = 0; for (int pg = 0; pg < PGN; pg++) tot += g_pp[pg].n;   // 落定后重数 | Recount after settling
    LOG("个人仓库：%s 当前第 %d 页，另外几页共 %d 件", g_pchar, g_pcur + 1, tot);
}
static int pages_save(const char *wd) {   // 写进工作目录（临时文件再改名）；只在提交前钩子里调 | Write into the working directory (temp file then rename); called only in the pre-commit hook
    char p[1200], t[1210]; pages_path(p, sizeof p, wd); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb"); if (!f) return -1;
    uint8_t h[8] = { 'S', 'W', 'P', 'G', 2, (uint8_t)g_pcur, (uint8_t)(g_ppend ? 1 : 0), 0 }; int ok = fwrite(h, 1, 8, f) == 8;
    for (int pg = 0; pg < PGN && ok; pg++) ok = ppage_write(f, &g_pp[pg]);
    if (ok && g_ppend) { uint8_t ft[2] = { (uint8_t)g_pfrom, (uint8_t)g_pto }; ok = fwrite(ft, 1, 2, f) == 2 && fwrite(&g_pH0, 8, 1, f) == 1 && ppage_write(f, &g_pA) && ppage_write(f, &g_pB) && ppage_write(f, &g_pS0); }
    ok = fclose(f) == 0 && ok; if (!ok || rename(t, p)) { remove(t); LOG("【红】个人仓库：写 %s 失败", p); return -1; }
    return 0;
}
static void pages_flip(uint8_t *b, uint64_t G, uint64_t player, int dir) {
    if (g_ppend) { LOG("个人仓库：上一次翻页还没落盘，这次不翻"); return; }
    int tgt = g_pcur + dir; if (tgt < 0 || tgt >= PGN) return;   // 不回绕（同原版共享页） | No wrap-around (same as the original game's shared pages)
    uint64_t t0 = pf_now(); pend_clear();
    uint64_t inv = *(uint64_t *)(player + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0, list[256]; int n = 0;
    for (int k = 0; it && k < 2048 && n < 256; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 4) list[n++] = it; it = *(uint64_t *)(d + 0xb0); }
    ppage out = { 0, n ? calloc((size_t)n, sizeof(pitem)) : NULL }; static uint8_t buf[VBYTES]; int keep = 0;
    // 先全部写出位流：有一件写不出就整次不翻（不动任何物品）
    // Write out all bitstreams first: if any one item cannot be written, the whole flip is cancelled (no item is touched)
    for (int i = 0; i < n; i++) { uint64_t x = list[i], pp = *(uint64_t *)(x + 0x38); int len = dm_item_write(b, x, buf);
        if (!len || !(out.it[i].bits = malloc((size_t)len))) { LOG("【红】个人仓库：第 %d 件写不出位流，这次不翻", i + 1); out.n = i; ppage_free(&out); return; }
        memcpy(out.it[i].bits, buf, (size_t)len); out.it[i].len = (uint16_t)len; out.it[i].x = (uint8_t)(pp ? *(int32_t *)(pp + 0x10) : 0); out.it[i].y = (uint8_t)(pp ? *(int32_t *)(pp + 0x14) : 0); out.n = i + 1; }
    for (int i = 0; i < n; i++) { uint64_t x = list[i], d = *(uint64_t *)(x + 0x10);
        ((void (*)(uint64_t))(b + 0x55e430))(x); ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, int))(b + 0x642f70))(G, player, x, 0x20, 4, 0, *(uint8_t *)(d + 0x55), out.it[i].y, out.it[i].x); ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, x); }
    ppage left = { 0, g_pp[tgt].n ? calloc((size_t)g_pp[tgt].n, sizeof(pitem)) : NULL };
    for (int i = 0; i < g_pp[tgt].n; i++) { pitem *pi = &g_pp[tgt].it[i]; uint64_t nu = dm_item_make(b, G, 0, pi->bits, pi->len, NULL), nd = nu ? *(uint64_t *)(nu + 0x10) : 0, ip = nu ? *(uint64_t *)(nu + 0x38) : 0;
        if (nd && ip) { *(uint8_t *)(nd + 0x55) = 4; *(int32_t *)(ip + 0x10) = pi->x; *(int32_t *)(ip + 0x14) = pi->y; uint64_t one = nu, ox = 0; ((int (*)(uint64_t, uint64_t, uint64_t *, uint64_t *))(b + 0x6f1940))(G, player, &one, &ox); }
        if (nd && *(uint64_t *)(nd + 0xa0)) { ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, uint64_t))(b + 0x642f70))(G, player, nu, 1, 2, 0, 4, 0, (uint64_t)pi->x | (uint64_t)pi->y << 16); pitem_dup(&g_pB, pi); free(pi->bits); keep++; }
        else { if (nu) ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, nu); left.it[left.n++] = *pi; LOG("【红】个人仓库：第 %d 页第 %d 件放不回格 %d,%d，留在侧文件里", tgt + 1, i + 1, pi->x, pi->y); } }
    free(g_pp[tgt].it); g_pp[tgt] = left;   // 放不回的（不该有）留在目标页槽里，下次翻走时和格里的一起存 | Items that cannot be put back (should not happen) stay in the target page slot and are saved together with those in the grid on the next flip away
    for (int i = 0; i < out.n; i++) pitem_dup(&g_pA, &out.it[i]);   // 格里搬出的（并进残留之前） | Moved out of the grid (before merging leftovers)
    for (int i = 0; i < g_pp[g_pcur].n; i++) pitem_dup(&g_pS0, &g_pp[g_pcur].it[i]);
    if (g_pp[g_pcur].n) { LOG("【红】个人仓库：第 %d 页槽里有 %d 件放不回的，并进这次搬出的", g_pcur + 1, g_pp[g_pcur].n); pitem *m = realloc(out.it, sizeof(pitem) * (size_t)(out.n + g_pp[g_pcur].n)); if (m) { out.it = m; memcpy(out.it + out.n, g_pp[g_pcur].it, sizeof(pitem) * (size_t)g_pp[g_pcur].n); out.n += g_pp[g_pcur].n; free(g_pp[g_pcur].it); g_pp[g_pcur].it = NULL; g_pp[g_pcur].n = 0; } }
    g_pp[g_pcur] = out; int from = g_pcur; g_pfrom = from; g_pto = tgt; { const char *wd = dm_save_workdir(); size_t dn = 0; uint8_t *d = !g_ser_infl && wd ? d2s_read(wd, &dn) : NULL; g_pH0 = d ? dm_fnv64(d, dn) : 0; free(d); } g_pcur = tgt; g_ppend = 1; atomic_store(&g_pcur_pub, g_pcur);
    ((void (*)(uint64_t, int))(b + 0x5cadb0))(player, 1);
    int sv = getenv("DM_TEST_PG_NOSAVE") ? -9 : dm_request_save();   // 测试开关 DM_TEST_PG_NOSAVE=1：翻完不请存盘（配合杀进程验「翻页后、落盘前」掉电） | Test switch DM_TEST_PG_NOSAVE=1: do not request a save after flipping (combine with killing the process to verify power loss "after the page flip, before persisting")
    g_pmade = dm_fs_commit_count(); g_pmore = 0;
    LOG("个人仓库：第 %d → %d 页，搬出 %d 件、放回 %d 件，用时 %.2f 毫秒；请游戏存盘 → %d", from + 1, tgt + 1, n, keep, (pf_now() - t0) / 1e6, sv);
    if (getenv("DM_TEST_PG_TAKE")) {   // 测试开关 DM_TEST_PG_TAKE=1：翻完当拍（第一次存盘前）把格里的件全部拿走（摘下 → 通知 → 释放，同卖掉），验「翻页后又把东西全拿走」不被当成翻页前 | Test switch DM_TEST_PG_TAKE=1: in the tick of the turn (before the first save) take every item out of the grid (remove -> notify -> free, same as selling), to check that 'took everything out after the turn' is not mistaken for 'before the turn'
        uint64_t iv = *(uint64_t *)(player + 0x90), x = iv && *(uint32_t *)iv == 0x01020304 ? *(uint64_t *)(iv + 0x10) : 0, tk[256]; int m = 0;
        for (int k = 0; x && k < 2048 && m < 256; k++) { uint64_t d = *(uint64_t *)(x + 0x10); if (!d) break; if (*(uint32_t *)(x + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 4) tk[m++] = x; x = *(uint64_t *)(d + 0xb0); }
        for (int i = 0; i < m; i++) { uint64_t y = tk[i], d = *(uint64_t *)(y + 0x10), pp = *(uint64_t *)(y + 0x38);
            ((void (*)(uint64_t))(b + 0x55e430))(y); ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, int))(b + 0x642f70))(G, player, y, 0x20, 4, 0, *(uint8_t *)(d + 0x55), pp ? *(int32_t *)(pp + 0x14) : 0, pp ? *(int32_t *)(pp + 0x10) : 0); ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, y); }
        LOG("测试 DM_TEST_PG_TAKE：翻完当拍从格里拿走 %d 件", m); }
}
static void pages_pre_commit(const char *wd) {   // 提交前钩子（持 g_pmu 调），规则见文件头「落盘一致性」 | Pre-commit hook (called holding g_pmu); for the rules see "On-disk consistency" in the file header
    { static int kill = -1; if (kill < 0) { const char *e = getenv("DM_TEST_PG_KILL"); kill = e ? atoi(e) : 0; }   // 测试开关 DM_TEST_PG_KILL=<n>：本局翻过页后，到翻页后第 n 次提交前直接结束进程（验任意时刻被杀：重进后不丢不重） | Test switch DM_TEST_PG_KILL=<n>: after a page turn in this session, exit the process right before the n-th commit after the turn (checks being killed at any moment: nothing lost or duplicated after re-entering)
      if (kill && g_pmade && dm_fs_commit_count() + 1 == g_pmade + (uint64_t)kill) { LOG("DM_TEST_PG_KILL：翻页后第 %d 次提交前结束进程（%s）", kill, g_ppend ? "翻页待定" : "翻页已落定"); fflush(stderr); _exit(9); } }
    if (!g_pload || !g_pchar[0] || (!g_ppend && !g_pdirty)) return;
    if (!g_ppend) { if (pages_save(wd) == 0) { g_pdirty = 0; LOG("个人仓库：进局后改过的页槽（去重 / 落定上次的翻页）随这次提交落盘"); } return; }
    // 只看这次要提交的本角色档里的个人仓库格：搬出页快照（A）与目标页快照（B）每件带位置的完整位流（简单物品也算）是否都在。
    // 位置在位流里，同一格只放得下一件，所以别处（共享仓库 / 别的角色 / 背包）有同字节的复制品不影响判断。
    // 格 = B → 翻页后；格 = A → 翻页前；两份都等（内容相同）判哪边都对，取翻页后；都不等 = 翻页后玩家又动过格 → 翻页后。
    // （旧判据在所有存档里找搬出件的前 48 字节，iCloud 并回复制出的件让「搬出的 3 件还在」，翻页永远证明不了，保存退出再进局整页 12 件丢了）
    // Look only at the personal stash grid in this character's file about to be committed: are all items of the turned-away page snapshot (A) and the target page snapshot (B) present, each as the full bit stream with its position (simple items included)?
    // The position is in the bit stream and a grid cell holds only one item, so same-byte copies elsewhere (shared stash / other characters / inventory) do not affect the result.
    // Grid = B -> after the turn; grid = A -> before the turn; both equal (same content) is correct either way, take after; neither = the player changed the grid after the turn -> after.
    // (The old rule searched all saves for the first 48 bytes of the moved-out items; copies made by the iCloud merge made '3 moved-out items still present', the turn could never be proven, and after Save and Exit a whole page of 12 items was lost)
    uint64_t cno = dm_fs_commit_count() + 1; const char *why = NULL; char cp[1400]; snprintf(cp, sizeof cp, "%s/%s/%s.d2s", wd, g_pchar, g_pchar);
    uint8_t *d = NULL; size_t dn = 0; FILE *f = fopen(cp, "rb");
    if (f) { fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); d = L > 0 && L < (16 << 20) ? malloc((size_t)L) : NULL; dn = d ? fread(d, 1, (size_t)L, f) : 0; fclose(f); }
    int na = 0, nb = 0;
    for (int i = 0; d && i < g_pA.n; i++) na += dm_memfind(d, dn, g_pA.it[i].bits, g_pA.it[i].len) != NULL;
    for (int i = 0; d && i < g_pB.n; i++) nb += dm_memfind(d, dn, g_pB.it[i].bits, g_pB.it[i].len) != NULL;
    uint64_t h = d ? dm_fnv64(d, dn) : 0; free(d);
    int isA = na == g_pA.n, isB = nb == g_pB.n;   // 快照里的件都在格里（快照为空时：格里一件快照里的都没有） | All items of the snapshot are in the grid (for an empty snapshot: none of its items are in the grid)
    if (!dn) { g_pmore = 1; LOG("【红】个人仓库：提交前核对 #%llu —— 读不到本角色档 %s，等下一次", (unsigned long long)cno, cp); }
    else if (!g_pA.n && !g_pB.n) why = "两页都空，不涉及物品";
    else if (g_pH0 && h != g_pH0) why = "角色档已不是翻页时那份（翻页时没有在途的存盘，之后的只能是翻页后序列化的）";   // 事实，不看格：翻页后又把件全拿走也判得出 | A fact, independent of the grid: also correct when everything was taken out after the turn
    else if (isB && g_pB.n) why = "格里是目标页";
    else if (isA && g_pA.n) { g_pmore = 1; LOG("个人仓库：提交前核对 #%llu —— 格里是搬出页（%d / %d 件都在）→ 角色档是翻页前的，等翻页后的存档", (unsigned long long)cno, na, g_pA.n); }
    else if (!g_pA.n && !nb) { g_pmore = 1; LOG("个人仓库：提交前核对 #%llu —— 从空页翻来、格里还没有目标页的件（0 / %d）→ 像是翻页前的，等翻页后的存档", (unsigned long long)cno, g_pB.n); }
    else why = "格里既不是搬出页也不是目标页（翻页后又动过格）";
    if (why) { g_ppend = 0;   // 先清待定再写：这次写的是不带待定记录的定稿 | Clear the pending state before writing: this write is the final version without the pending record
        if (pages_save(wd) == 0) { pend_clear(); g_pdirty = 0; g_pmore = 0; LOG("个人仓库：提交前核对 #%llu —— %s → 翻页后，侧文件随这次提交落盘（当前第 %d 页）", (unsigned long long)cno, why, g_pcur + 1); }
        else g_ppend = 1; }
    else if (pages_save(wd) == 0) LOG("个人仓库：提交前核对 #%llu —— 还没证明，侧文件带待定翻页记录随这次提交落盘（第 %d → %d 页，搬出 %d 件、搬进 %d 件）", (unsigned long long)cno, g_pfrom + 1, g_pto + 1, g_pA.n, g_pB.n);
}
// 测试开关 DM_TEST_PG_SOCK=<拍>（造带孔物品现场）：背包里找一件能镶的装备 + 一颗宝石，用原版镶嵌 0x64d080 镶上，再把母件搬进个人仓库格 (5,5)（写位流 → 摘下 → 释放 → 建物品（含孔里的）→ 读档放物品）
// Test switch DM_TEST_PG_SOCK=<tick> (sets up a socketed-item test case): find a socketable piece of equipment + a gem in the inventory, socket it with the original game's socketing 0x64d080, then move the parent item to personal stash cell (5,5) (write bitstream -> remove -> free -> build item (including socketed ones) -> load-time item placement)
static int item_kids(uint64_t it) { uint64_t inv = *(uint64_t *)(it + 0x90); int n = 0; if (!inv || *(uint32_t *)inv != 0x01020304) return 0; for (uint64_t c = *(uint64_t *)(inv + 0x10); c && n < 16; ) { uint64_t d = *(uint64_t *)(c + 0x10); if (!d) break; n++; c = *(uint64_t *)(d + 0xb0); } return n; }
static void pg_sock_test(uint64_t G, uint64_t player, uint8_t *b, uint32_t tq) {
    static uint32_t at; static int got; if (!got) { got = 1; const char *e = getenv("DM_TEST_PG_SOCK"); at = e ? (uint32_t)atoi(e) : 0; }
    if (!at || tq < at) return; at = 0;
    uint64_t inv = *(uint64_t *)(player + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0, gems[64], gear[64]; int ng = 0, nq = 0;
    for (int k = 0; it && k < 2048; k++) { uint64_t d = *(uint64_t *)(it + 0x10); if (!d) break; uint32_t c = ((uint32_t (*)(uint64_t))(b + 0x570260))(it);
        if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 0) { if (((char *)&c)[0] == 'g' && ng < 64) gems[ng++] = it; else if (nq < 64) gear[nq++] = it; } it = *(uint64_t *)(d + 0xb0); }
    for (int q = 0; q < nq && ng; q++) { uint64_t g = gems[ng - 1], gd = *(uint64_t *)(g + 0x10), gp = *(uint64_t *)(g + 0x38); int gx = gp ? *(int32_t *)(gp + 0x10) : 0, gy = gp ? *(int32_t *)(gp + 0x14) : 0;
        ((void (*)(uint64_t))(b + 0x55e430))(g); ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, int))(b + 0x642f70))(G, player, g, 0x20, 4, 0, *(uint8_t *)(gd + 0x55), gy, gx);
        ((void (*)(uint64_t, int))(b + 0x5ca8a0))(g, 4);
        if (((int (*)(uint64_t, uint64_t, uint32_t, uint32_t, int))(b + 0x64d080))(G, gear[q], *(uint32_t *)(g + 8), *(uint32_t *)(gear[q] + 8), 2)) {
            ng--; uint64_t x = gear[q], pp = *(uint64_t *)(x + 0x38), d = *(uint64_t *)(x + 0x10); static uint8_t buf[VBYTES]; int len = dm_item_write(b, x, buf); uint32_t c = ((uint32_t (*)(uint64_t))(b + 0x570260))(x);
            LOG("镶孔测试：%.3s 镶上一颗宝石（孔里现 %d 件），位流 %d 字节，搬进个人仓库 (5,5)", (const char *)&c, item_kids(x), len);
            ((void (*)(uint64_t))(b + 0x55e430))(x); ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, int))(b + 0x642f70))(G, player, x, 0x20, 4, 0, *(uint8_t *)(d + 0x55), pp ? *(int32_t *)(pp + 0x14) : 0, pp ? *(int32_t *)(pp + 0x10) : 0); ((void (*)(uint64_t, uint64_t))(b + 0x658be0))(G, x);
            int sk = 0; uint64_t nu = len ? dm_item_make(b, G, 0, buf, len, &sk) : 0, nd = nu ? *(uint64_t *)(nu + 0x10) : 0, ip = nu ? *(uint64_t *)(nu + 0x38) : 0;
            if (nd && ip) { *(uint8_t *)(nd + 0x55) = 4; *(int32_t *)(ip + 0x10) = 5; *(int32_t *)(ip + 0x14) = 5; uint64_t one = nu, ox = 0; ((int (*)(uint64_t, uint64_t, uint64_t *, uint64_t *))(b + 0x6f1940))(G, player, &one, &ox);
                ((void (*)(uint64_t, uint64_t, uint64_t, int, int, int, int, int, uint64_t))(b + 0x642f70))(G, player, nu, 1, 2, 0, 4, 0, 5 | 5 << 16); }
            LOG("镶孔测试：重建后孔里 %d 件，进个人仓库 %s", nu ? item_kids(nu) : -1, nd && *(uint64_t *)(nd + 0xa0) ? "成" : "没成"); return; }
        ((void (*)(uint64_t, int))(b + 0x5ca8a0))(g, 0); }   // 镶不上：这件没孔，换下一件（宝石已摘下，测试用，不放回） | Cannot socket: this item has no sockets, try the next one (the gem is already removed; test only, not put back)
    LOG("镶孔测试：背包里没有能镶的装备（%d 件装备、%d 颗宝石）", nq, ng);
}
void dm_pages_tick(uint64_t G, uint64_t player, uint8_t *b) {   // 服务端拍（diag.c hud_tick）；不在局内 player 为 0 | Server tick (hud_tick in diag.c); player is 0 when not in a run
    static uint64_t lp; if (!player) { lp = 0; g_pload = 0; return; }
    uint32_t tq = *(uint32_t *)(G + 0x170);
    if (player != lp) { lp = player; uint64_t pd = *(uint64_t *)(player + 0x10); pthread_mutex_lock(&g_pmu); strlcpy(g_pchar, pd ? (const char *)pd : "", sizeof g_pchar);
        { const char *sn = dm_stash_name(b); strlcpy(g_pstash, sn ? sn : "", sizeof g_pstash); } pages_load(b, player); g_pload = 1; pthread_mutex_unlock(&g_pmu); }
    { pthread_mutex_lock(&g_pmu);
      { static uint64_t pv, tick_seq; uint64_t v = *(uint64_t *)(G + 0x5800), w = dm_fs_d2s_wseq();   // [G+0x5800] 变成真时刻 = 游戏序列化了（请存盘时我们拨成 1） | [G+0x5800] became a real time = the game serialized (we set it to 1 when requesting a save)
        if (v != pv) { if (v > 1 && pv) { g_ser_sec = dm_user_clock(); g_ser_infl = !(w > tick_seq); g_ser_seq = dm_seq_next(); g_ser_tq = tq; } pv = v; }   // 从 0 变过来是进局开始计时，不是序列化 | A change from 0 is the game starting its timer on entry, not a serialization
        if (g_ser_infl && w > g_ser_seq) g_ser_infl = 0;   // 看到序列化之后角色档写完了 | The character file finished writing after the serialization was seen
        tick_seq = dm_seq_next(); g_tq_now = tq; }
      int more = g_pmore; g_pmore = 0; pthread_mutex_unlock(&g_pmu); if (more && !getenv("DM_TEST_PG_NOSAVE")) { int r = dm_request_save(); LOG("个人仓库：翻页还没证明，再请一次存盘 → %d", r); } }
    pg_sock_test(G, player, b, tq);
    int dir = atomic_exchange(&g_preq, 0);
    const char *e = getenv("DM_TEST_PG_GO");   // 测试开关 DM_TEST_PG_GO=<拍>:<方向>[,<拍>:<方向>…]：到拍翻页（+1 下一页 / -1 上一页） | Test switch DM_TEST_PG_GO=<tick>:<direction>[,<tick>:<direction>...]: flip the page at that tick (+1 next page / -1 previous page)
    if (e) { char buf[128]; strlcpy(buf, e, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) { int at = 0, dv = 0; if (sscanf(t, "%d:%d", &at, &dv) == 2 && (uint32_t)at == tq) dir = dv; } }
    { const char *ce = getenv("DM_TEST_CLOCK_AT");   // 测试开关 DM_TEST_CLOCK_AT=<拍>:<秒>[,…]：到拍把游戏用户时钟拨若干秒（负数往回拨），验改系统时间不丢不重 | Test switch DM_TEST_CLOCK_AT=<tick>:<seconds>[,...]: at the tick, shift the game's user clock by that many seconds (negative = back), to check that changing the system time neither loses nor duplicates items
      if (ce) { void dm_test_clock_shift(int64_t d); char buf[128]; strlcpy(buf, ce, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) { int at = 0; long long dv = 0; if (sscanf(t, "%d:%lld", &at, &dv) == 2 && (uint32_t)at == tq) dm_test_clock_shift(dv); } } }
    const char *lg = getenv("DM_TEST_PG_LOG");   // 测试开关 DM_TEST_PG_LOG=<拍>[,<拍>…]：到拍把个人仓库格里每件的物品码 / 格 / 孔里件数打出来（回归比对用） | Test switch DM_TEST_PG_LOG=<tick>[,<tick>...]: at that tick, print the item code / cell / socketed item count of every item in the personal stash grid (for regression comparison)
    if (lg) { char buf[128]; strlcpy(buf, lg, sizeof buf); for (char *sv, *t = strtok_r(buf, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) if ((uint32_t)atoi(t) == tq) {
        uint64_t inv = *(uint64_t *)(player + 0x90), it = inv && *(uint32_t *)inv == 0x01020304 ? *(uint64_t *)(inv + 0x10) : 0; int n = 0;
        for (int k = 0; it && k < 2048; k++) { uint64_t d = *(uint64_t *)(it + 0x10), pp = *(uint64_t *)(it + 0x38); if (!d) break;
            if (*(uint32_t *)(it + 0xc) == 0 && *(uint8_t *)(d + 0x55) == 4) { uint32_t c = ((uint32_t (*)(uint64_t))(b + 0x570260))(it); n++; LOG("个人仓库读数：拍 %u %.3s 格 %d,%d 孔里 %d 件", tq, (const char *)&c, pp ? *(int32_t *)(pp + 0x10) : -1, pp ? *(int32_t *)(pp + 0x14) : -1, item_kids(it)); }
            it = *(uint64_t *)(d + 0xb0); }
        LOG("个人仓库读数：拍 %u 第 %d 页，格里 %d 件", tq, g_pcur + 1, n); } }
    // 翻页顺延（至多 50 拍）：有在途的存盘（已序列化、角色档还没写完）就等它写完，翻页时才能记下唯一的「翻页前那份」指纹（通常几毫秒）；
    // 这一秒里刚存过盘也等到下一秒（固定时钟局不等），免得翻页后同一秒再存出逐字节相同的档、指纹分不开
    // Page turn deferral (at most 50 ticks): if a save is in flight (serialized, character file not yet written), wait for the write so the turn can record the unique pre-turn fingerprint (usually a few milliseconds);
    // if a save already happened in this second, also wait for the next second (not with a fixed clock), so a save later in the same second cannot produce a byte-identical file whose fingerprint cannot be told apart
    { static int hold; static uint32_t hq; if (dir) { hold = dir; hq = tq; }
      if (hold) { if ((dm_ser_state() == 1 || (g_ser_sec == dm_user_clock() && !dm_user_clock_fixed())) && tq - hq < 50) dir = 0; else { if (tq != hq) LOG("个人仓库：翻页顺延了 %u 拍（等存盘写完 / 这一秒刚存过盘）%s", tq - hq, g_ser_infl ? "，到上限存盘还在写：这次照翻、只按格判" : ""); dir = hold; hold = 0; } } }
    if (dir) { pthread_mutex_lock(&g_pmu); pages_flip(b, G, player, dir); pthread_mutex_unlock(&g_pmu); }
}
static void pages_hook(const char *wd) { pthread_mutex_lock(&g_pmu); pages_pre_commit(wd); pthread_mutex_unlock(&g_pmu); }
__attribute__((constructor)) static void pages_hook_install(void) { extern void (*g_dm_pre_commit_hook[5])(const char *); g_dm_pre_commit_hook[0] = pages_hook; }
