// 共享仓库页数可调（上限 99）与多出页的旁路备份。
// 页数：原版页数 = 3 + 0x1d2ce0() + 2·(模式字节 [main+0x2127c48] == 3)，0x1d2ce0 只有这 7 个调用点（界面页码 0x394ea8 / 0x3988d8 / 0x3996d4、服务端建页表 0x55ed70 与遍历 0x55ee58、
//   读档补空页 0x6fdf44、共享金币上限 0xc55a2c），全是这个式子。改写器把 0x1d2ce0 入口换成 svc #0x91; ret（patch_nso.py 共享仓库页数门），这里答「多出几页」：
//   页数 = max(stash_pages.txt 的 shared_pages（缺省 5 = 原版）, 存档里共享仓库档已有的页数)，夹在 5..99；本进程第一次问时定下、之后不变（服务端按它分配页表，局中变了会错位）。
//   取 max：调小也不会让游戏读档时丢掉已有的页（页数少的构建读多页的档，多出的页大概率被丢弃）。
// 备份：共享仓库档 <名>.d2i 按 55 AA 55 AA 分段，段头 +0x14 是段类型：0 = 物品页，1 / 2 = 原版别的段（实测 1 在第 5 页后、2 在最后；游戏按「前 5 页、类型 1、其余页、类型 2」写）。
//   每次提交前（写盘线程）把工作目录里页数多于 5 的档整份抄一份旁路 <名>.swshared（同目录，随提交原子换入；iCloud 按 ModernSharedStash* 整个目录同步，它跟着走）。
//   挂存档前（hle_fs h_fs_MountSave）：档的物品页比备份少 = 被页数少的程序（旧版 / 没这段代码的设备）存过 → 前 k 页用现在的档（那个程序可能动过），
//   第 k+1 页起用备份的（那个程序看不见这些页，动不了，备份就是真值，含页上的金币），接在类型 2 段前面；改前那份留成 <名>.d2i.before-restore。
// Adjustable shared stash page count (cap 99) and a side backup of the extra pages.
// Count: original count = 3 + 0x1d2ce0() + 2·(mode byte [main+0x2127c48] == 3); 0x1d2ce0 has exactly these 7 call sites (UI page number 0x394ea8 / 0x3988d8 / 0x3996d4, server page table 0x55ed70 and its walk 0x55ee58,
//   loading fills empty pages 0x6fdf44, shared gold cap 0xc55a2c), all with this expression. The rewriter replaces the entry of 0x1d2ce0 with svc #0x91; ret (patch_nso.py shared stash page count gate), and this answers 'how many extra pages':
//   count = max(shared_pages in stash_pages.txt (default 5 = original), pages the shared stash file in the save already has), clamped to 5..99; fixed at the first question in this process and never changed afterwards (the server allocates its page table by it; a change mid-run would misalign).
//   Taking the max: lowering the setting never makes the game drop existing pages on load (a build with fewer pages reading a file with more most likely drops the extra pages).
// Backup: the shared stash file <name>.d2i is split into segments by 55 AA 55 AA, segment header +0x14 is the segment type: 0 = item page, 1 / 2 = other original segments (measured: 1 after page 5, 2 last; the game writes 'first 5 pages, type 1, the other pages, type 2').
//   Before every commit (disk-write thread) a file in the working directory with more than 5 pages is copied whole to the side file <name>.swshared (same directory, swapped in atomically with the commit; iCloud syncs the whole ModernSharedStash* directory, so it goes along).
//   Before mounting the save (hle_fs h_fs_MountSave): the file has fewer item pages than the backup = it was saved by a program with fewer pages (an old build / a device without this code) -> the first k pages come from the current file (that program may have changed them),
//   pages k+1 on come from the backup (that program cannot see them, so cannot change them; the backup is the truth, including the gold on those pages), inserted before the type 2 segment; the file before the change is kept as <name>.d2i.before-restore.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "dm.h"

#define SP_MIN 5
#define SP_MAX 99
#define SP_DEF 5
int dm_custom_num(const char *file, const char *key, int def);   // dm_custom.c
static uint8_t *main_base(void) { static uint8_t *b; if (!b) for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) b = (uint8_t *)g_mod[i].base; return b; }
static const char *const k_names[4] = { "ModernSharedStashSoftCoreV2", "ModernSharedStashHardCoreV2", "SharedStashSoftCoreV2", "SharedStashHardCoreV2" };

typedef struct { size_t off, len; uint8_t type; } sp_seg;
// 分段；返回段数，-1 = 不是整齐的段序列（不动它） | Split into segments; returns the count, -1 = not a clean segment sequence (left alone)
int dm_sp_segments(const uint8_t *d, size_t n, sp_seg *s, int max) {
    size_t off = 0; int k = 0;
    while (off < n) { if (k >= max || off + 0x40 > n || memcmp(d + off, "\x55\xaa\x55\xaa", 4)) return -1;
        uint32_t len; memcpy(&len, d + off + 16, 4); if (len < 0x40 || off + len > n) return -1;
        s[k].off = off; s[k].len = len; s[k].type = d[off + 0x14]; k++; off += len; }
    return k;
}
int dm_sp_pages(const sp_seg *s, int k) { int n = 0; for (int i = 0; i < k; i++) n += s[i].type == 0; return n; }
// 拼：cur 的段到第一个类型 2 段之前为止 + bak 的第 have+1 页起的物品页 + cur 从类型 2 段起的；写进 out（调用方给够大），返回字节数
// Splice: cur's segments up to (not including) the first type 2 segment + bak's item pages from page have+1 on + cur's segments from the type 2 segment on; written into out (the caller gives enough room), returns the byte count
size_t dm_sp_splice(const uint8_t *cur, const sp_seg *cs, int ck, const uint8_t *bak, const sp_seg *bs, int bk, uint8_t *out) {
    int cut = ck; for (int i = 0; i < ck; i++) if (cs[i].type == 2) { cut = i; break; }
    size_t o = 0; int have = dm_sp_pages(cs, ck), pg = 0;
    for (int i = 0; i < cut; i++) { memcpy(out + o, cur + cs[i].off, cs[i].len); o += cs[i].len; }
    for (int i = 0; i < bk; i++) if (bs[i].type == 0 && pg++ >= have) { memcpy(out + o, bak + bs[i].off, bs[i].len); o += bs[i].len; }
    for (int i = cut; i < ck; i++) { memcpy(out + o, cur + cs[i].off, cs[i].len); o += cs[i].len; }
    return o;
}
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); uint8_t *d = NULL; *n = 0; if (!f) return NULL;
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET); d = L > 0 && L < (16 << 20) ? malloc((size_t)L) : NULL; *n = d ? fread(d, 1, (size_t)L, f) : 0; fclose(f);
    if (!*n) { free(d); d = NULL; } return d; }
static int spit(const char *p, const uint8_t *d, size_t n) {   // 临时文件再改名 | Temp file then rename
    char t[1300]; snprintf(t, sizeof t, "%s.tmp", p); FILE *f = fopen(t, "wb"); if (!f) return -1;
    int ok = fwrite(d, 1, n, f) == n; ok = fclose(f) == 0 && ok; if (!ok || rename(t, p)) { remove(t); return -1; } return 0; }
static void paths(const char *dir, const char *name, char *d2i, char *bak, size_t n) {   // 档在 <名>/<名>.d2i 或根目录 <名>.d2i（同 dm_save_blobs） | The file is <name>/<name>.d2i or <name>.d2i in the root (same as dm_save_blobs)
    struct stat st; snprintf(d2i, n, "%s/%s/%s.d2i", dir, name, name); snprintf(bak, n, "%s/%s/%s.swshared", dir, name, name);
    if (stat(d2i, &st)) { snprintf(d2i, n, "%s/%s.d2i", dir, name); snprintf(bak, n, "%s/%s.swshared", dir, name); } }

// ---------- 页数 | Page count ----------
static _Atomic int g_count;   // 0 = 还没定 | 0 = not decided yet
static int file_pages(const char *dir) {   // 存档目录里各共享仓库档的最多物品页数 | Most item pages among the shared stash files in the save directory
    int best = 0; char d2i[1200], bak[1200]; static sp_seg s[256];
    for (int k = 0; dir && k < 4; k++) { paths(dir, k_names[k], d2i, bak, sizeof d2i); size_t n; uint8_t *d = slurp(d2i, &n); if (!d) continue;
        int c = dm_sp_segments(d, n, s, 256); if (c > 0 && dm_sp_pages(s, c) > best) best = dm_sp_pages(s, c); free(d); }
    return best; }
int dm_shared_pages(void) {   // 本进程的共享仓库页数（第一次调时定下） | This process's shared stash page count (decided at the first call)
    int c = atomic_load(&g_count); if (c) return c;
    int want = dm_custom_num("stash_pages.txt", "shared_pages", SP_DEF); const char *dir = getenv("DM_SAVE"); int have = file_pages(dir);
    c = want > have ? want : have; c = c < SP_MIN ? SP_MIN : c > SP_MAX ? SP_MAX : c;
    int exp = 0; if (atomic_compare_exchange_strong(&g_count, &exp, c)) LOG("共享仓库：页数 %d（设定 shared_pages=%d，存档里已有 %d 页，夹在 %d–%d），本次运行不再变", c, want, have, SP_MIN, SP_MAX);
    return atomic_load(&g_count);
}
// svc #0x91（0x1d2ce0 入口）：答「多出几页」= 页数 − 3 − 2·(模式 == 3)，写 x0 | svc #0x91 (entry of 0x1d2ce0): answers 'how many extra pages' = count - 3 - 2·(mode == 3), written to x0
void svc_shared_pages(uint64_t *regs) {
    uint8_t *B = main_base(); int mode3 = B && B[0x2127c48] == 3;
    regs[0] = (uint64_t)(uint32_t)(dm_shared_pages() - 3 - 2 * mode3);
}

// ---------- 备份与恢复 | Backup and restore ----------
static void sp_restore(const char *save_dir) {   // 挂存档前（hle_fs） | Before mounting the save (hle_fs)
    static sp_seg cs[256], bs[256]; char d2i[1200], bak[1200], old[1300];
    for (int k = 0; k < 4; k++) { paths(save_dir, k_names[k], d2i, bak, sizeof d2i); size_t cn, bn; uint8_t *cur = slurp(d2i, &cn), *bk = cur ? slurp(bak, &bn) : NULL;
        int ck = cur ? dm_sp_segments(cur, cn, cs, 256) : -1, bkn = bk ? dm_sp_segments(bk, bn, bs, 256) : -1;
        if (ck > 0 && bkn > 0 && dm_sp_pages(bs, bkn) > dm_sp_pages(cs, ck)) {
            uint8_t *out = malloc(cn + bn); size_t on = out ? dm_sp_splice(cur, cs, ck, bk, bs, bkn, out) : 0;
            snprintf(old, sizeof old, "%s.before-restore", d2i);
            if (on && spit(old, cur, cn) == 0 && spit(d2i, out, on) == 0)
                LOG("【红】共享仓库：%s 只有 %d 页、备份有 %d 页（被页数少的程序存过）→ 第 %d 页起从备份补回（原档留成 %s）", d2i, dm_sp_pages(cs, ck), dm_sp_pages(bs, bkn), dm_sp_pages(cs, ck) + 1, old);
            else LOG("【红】共享仓库：%s 少页，从备份补回失败，原档不动", d2i);
            free(out); }
        else if (ck < 0 && cur) LOG("【红】共享仓库：%s 分段不整齐，不核对备份", d2i);
        free(cur); free(bk); }
}
static void sp_backup(const char *workdir) {   // 提交前（写盘线程）：页数多于 5 的档抄一份旁路 | Before commit (disk-write thread): copy a file with more than 5 pages to the side file
    static sp_seg s[256]; char d2i[1200], bak[1200];
    for (int k = 0; k < 4; k++) { paths(workdir, k_names[k], d2i, bak, sizeof d2i); size_t n, bn = 0; uint8_t *d = slurp(d2i, &n); if (!d) continue;
        int c = dm_sp_segments(d, n, s, 256);
        if (c > 0 && dm_sp_pages(s, c) > SP_MIN) { uint8_t *b = slurp(bak, &bn);
            if (!b || bn != n || memcmp(b, d, n)) { if (spit(bak, d, n)) LOG("【红】共享仓库：写备份 %s 失败", bak); }
            free(b); }
        free(d); }
}
__attribute__((constructor)) static void sp_install(void) {
    extern void (*g_dm_mount_stash_hook)(const char *), (*g_dm_pre_commit_stash_hook)(const char *);
    g_dm_mount_stash_hook = sp_restore; g_dm_pre_commit_stash_hook = sp_backup;
}
