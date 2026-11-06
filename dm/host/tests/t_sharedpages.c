// 共享仓库页数可调与旁路备份（dm_sharedpages.c）：分段、按「前 5 页、类型 1、其余页、类型 2」拼回被页数少的程序存过的档、备份只在多于 5 页且变了时写、
// 页数 = 设定与存档已有页数取大并夹在 5..99、本进程定一次、svc #0x90 答「多出几页」（模式 3 再减 2）。只碰临时目录里的文件。
// Adjustable shared stash page count and side backup (dm_sharedpages.c): segmenting, splicing a file saved by a program with fewer pages back in the order 'first 5 pages, type 1, the other pages, type 2', the backup written only with more than 5 pages and when changed,
// count = the larger of the setting and the pages the save already has, clamped to 5..99, decided once per process, svc #0x90 answers 'how many extra pages' (2 less in mode 3). Only files in a temporary directory are touched.
#include "../dm_sharedpages.c"
#include <unistd.h>
module_t g_mod[MAX_MOD]; int g_nmod;
void (*g_dm_mount_stash_hook)(const char *), (*g_dm_pre_commit_stash_hook)(const char *);
static int g_set = 30;
int dm_custom_num(const char *file, const char *key, int def) { (void)file; (void)key; (void)def; return g_set; }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (c) printf("  ✓ "); else { fails++; printf("  ✗ "); } printf(__VA_ARGS__); printf("\n"); } while (0)
typedef struct { uint8_t d[65536]; size_t n; } bufw;
// 一段：头 0x40（55 AA 55 AA、2、0x69、金币、长度、+0x14 类型），身体 body 字节（标记 tag 填满，好认出是哪一段） | One segment: 0x40 header (55 AA 55 AA, 2, 0x69, gold, length, +0x14 type), then body bytes (filled with tag, to tell which segment it is)
static void seg(bufw *w, int type, uint32_t gold, int body, uint8_t tag) {
    uint8_t h[0x40] = { 0x55, 0xaa, 0x55, 0xaa, 2, 0, 0, 0, 0x69 }; uint32_t len = 0x40 + (uint32_t)body; memcpy(h + 12, &gold, 4); memcpy(h + 16, &len, 4); h[0x14] = (uint8_t)type;
    memcpy(w->d + w->n, h, 0x40); memset(w->d + w->n + 0x40, tag, (size_t)body); w->n += len; }
// 游戏写法：前 5 页、类型 1、其余页、类型 2；第 i 页标记 = base + i | The game's layout: first 5 pages, type 1, the other pages, type 2; tag of page i = base + i
static void stash(bufw *w, int pages, uint8_t base) { w->n = 0; for (int i = 0; i < pages; i++) { if (i == 5) seg(w, 1, 0, 30, 0xE1); seg(w, 0, i == 6 ? 777 : 0, 10 + i, (uint8_t)(base + i)); } if (pages <= 5) seg(w, 1, 0, 30, 0xE1); seg(w, 2, 0, 50, 0xE2); }
static char g_root[256];
static void wf(const char *rel, const bufw *w) { char p[600]; snprintf(p, sizeof p, "%s/%s", g_root, rel); FILE *f = fopen(p, "wb"); fwrite(w->d, 1, w->n, f); fclose(f); }
static size_t rf(const char *rel, uint8_t *o, size_t cap) { char p[600]; snprintf(p, sizeof p, "%s/%s", g_root, rel); FILE *f = fopen(p, "rb"); if (!f) return 0; size_t n = fread(o, 1, cap, f); fclose(f); return n; }
static int exists(const char *rel) { char p[600]; struct stat st; snprintf(p, sizeof p, "%s/%s", g_root, rel); return !stat(p, &st); }
#define D2I "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i"
#define BAK "ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.swshared"
int main(void) {
    snprintf(g_root, sizeof g_root, "/tmp/t_sharedpages_%d", (int)getpid()); char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/ModernSharedStashSoftCoreV2", g_root, g_root); if (system(cmd)) return 2;
    static bufw full, old; static sp_seg s[256], t[256]; static uint8_t rb[65536];
    stash(&full, 20, 0x10);
    int k = dm_sp_segments(full.d, full.n, s, 256);
    CHECK(k == 22 && dm_sp_pages(s, k) == 20 && s[5].type == 1 && s[21].type == 2, "20 页的档：22 段、20 个物品页，类型 1 在第 6 段、类型 2 在最后（同实测）");
    { bufw bad = full; bad.d[0x40 + 10 + 2] ^= 0xff; CHECK(dm_sp_segments(bad.d, bad.n, s, 256) == -1, "段头坏了：-1（不动它）"); }
    // 被 5 页的程序存过：前 5 页内容变了（标记 0x40 起）、只剩 5 页 | Saved by a 5-page program: pages 1-5 changed (tags from 0x40), only 5 pages left
    stash(&old, 5, 0x40); wf(D2I, &old); wf(BAK, &full);
    sp_restore(g_root); size_t n = rf(D2I, rb, sizeof rb); int rk = dm_sp_segments(rb, n, t, 256);
    CHECK(rk == 22 && dm_sp_pages(t, rk) == 20, "补回：22 段、20 页");
    { int ok = 1, pg = 0; for (int i = 0; i < rk; i++) if (t[i].type == 0) { uint8_t want = pg < 5 ? (uint8_t)(0x40 + pg) : (uint8_t)(0x10 + pg); if (rb[t[i].off + 0x40] != want) ok = 0; pg++; }
      CHECK(ok, "第 1–5 页用现在的档（5 页的程序动过的），第 6–20 页用备份的"); }
    { uint32_t g7; int pg = 0; for (int i = 0; i < rk; i++) if (t[i].type == 0 && pg++ == 6) memcpy(&g7, rb + t[i].off + 12, 4); CHECK(g7 == 777, "第 7 页上的金币跟着补回"); }
    CHECK(t[5].type == 1 && t[rk - 1].type == 2 && rb[t[5].off + 0x40] == 0xE1 && rb[t[rk - 1].off + 0x40] == 0xE2, "类型 1 在第 5 页后、类型 2 在最后（同游戏写法）");
    { static uint8_t ob[65536]; size_t on = rf(D2I ".before-restore", ob, sizeof ob); CHECK(on == old.n && !memcmp(ob, old.d, old.n), "补回前那份留成 .before-restore"); }
    // 已经齐了：不动 | Already complete: untouched
    wf(D2I, &full); sp_restore(g_root); n = rf(D2I, rb, sizeof rb); CHECK(n == full.n && !memcmp(rb, full.d, n), "档的页数不比备份少：不动");
    // 档比备份多页（30 页的档、20 页的备份）：不动 | File with more pages than the backup: untouched
    { static bufw p30; stash(&p30, 30, 0x60); wf(D2I, &p30); sp_restore(g_root); n = rf(D2I, rb, sizeof rb); CHECK(n == p30.n && !memcmp(rb, p30.d, n), "档比备份多页：不动"); }
    // 备份 | Backup
    snprintf(cmd, sizeof cmd, "rm -f %s/%s", g_root, BAK); (void)!system(cmd);
    wf(D2I, &old); sp_backup(g_root); CHECK(!exists(BAK), "5 页的档：不写备份");
    wf(D2I, &full); sp_backup(g_root); n = rf(BAK, rb, sizeof rb); CHECK(n == full.n && !memcmp(rb, full.d, n), "20 页的档：备份 = 整份档");
    { struct stat a, b; char p[600]; snprintf(p, sizeof p, "%s/%s", g_root, BAK); stat(p, &a); sleep(1); sp_backup(g_root); stat(p, &b);
      CHECK(a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec, "档没变：备份不重写"); }
    // 页数 | Page count
    setenv("DM_SAVE", g_root, 1); wf(D2I, &full);
    g_set = 30; atomic_store(&g_count, 0); CHECK(dm_shared_pages() == 30, "设 30、存档 20 页：30");
    g_set = 10; CHECK(dm_shared_pages() == 30, "本进程已定：改设定不变");
    atomic_store(&g_count, 0); CHECK(dm_shared_pages() == 20, "设 10、存档 20 页：20（不丢已有的页）");
    g_set = 500; atomic_store(&g_count, 0); CHECK(dm_shared_pages() == 99, "设 500：夹到 99");
    g_set = 1; snprintf(cmd, sizeof cmd, "rm -f %s/%s", g_root, D2I); (void)!system(cmd); atomic_store(&g_count, 0); CHECK(dm_shared_pages() == 5, "设 1、没有档：夹到 5");
    // svc #0x90：多出几页 | svc #0x90: extra pages
    { uint8_t *mb = calloc(1, 0x2127c49); g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = mb; uint64_t r[32] = { 0 };
      g_set = 30; atomic_store(&g_count, 0); mb[0x2127c48] = 3; svc_shared_pages(r); CHECK(r[0] == 25, "模式 3、30 页：答 25（3 + 25 + 2 = 30）");
      mb[0x2127c48] = 0; svc_shared_pages(r); CHECK(r[0] == 27, "别的模式、30 页：答 27（3 + 27 = 30）"); free(mb); }
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root); (void)!system(cmd);
    printf("t_sharedpages：%d 项，%d 项失败\n", checks, fails);
    return fails ? 1 : 0;
}
