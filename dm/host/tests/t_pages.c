// 个人仓库 2–10 页（dm_pages.c）：侧文件读 5 页的版本 1 / 2 与 10 页的版本 3、后几页空时照旧写版本 2（逐字节相同）、页数多于 PGN 只读、
// 待定翻页记录进局落定、提交前判定表、翻页目标累加与夹边。只碰文件，不碰游戏内存（玩家物品栏 +0x90 = 0：格是空的）。
// Personal stash pages 2-10 (dm_pages.c): reading the 5-page side file versions 1 / 2 and the 10-page version 3, writing version 2 as before (byte-identical) while the later pages are empty, read-only when there are more pages than PGN,
// settling a pending turn record on entry, the pre-commit verdict table, turn target accumulation and clamping. Files only, no game memory (player inventory +0x90 = 0: the grid is empty).
#include "../dm_pages.c"
static char g_wd[256];
const char *dm_save_workdir(void) { return g_wd; }
int dm_request_save(void) { return 1; }
uint64_t dm_fs_commit_count(void) { return 0; }
const char *dm_stash_name(uint8_t *b) { (void)b; return ""; }
uint64_t dm_seq_next(void) { return 0; }
uint64_t dm_fs_d2s_wseq(void) { return 0; }
uint32_t dm_hud_held(void) { return 0; }
void dm_game_notice(uint8_t *b, const char *t) { (void)b; (void)t; }
const char *dm_str(uint8_t *b, const char *k) { (void)b; return k; }
void dm_test_clock_shift(int64_t d) { (void)d; }
static int g_set = 10;   // stash_pages.txt 的 personal_pages | personal_pages in stash_pages.txt
int dm_custom_num(const char *file, const char *key, int def) { (void)file; (void)key; (void)def; return g_set; }
void (*g_dm_pre_commit_hook[5])(const char *);
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (c) printf("  ✓ "); else { fails++; printf("  ✗ "); } printf(__VA_ARGS__); printf("\n"); } while (0)
// 造一件：20 字节位流，字节随 id 变（第 3 字节 0x20 位不置 = 不是简单物品） | Make an item: a 20-byte bit stream whose bytes depend on id (bit 0x20 of byte 3 clear = not a simple item)
static void mk(uint8_t *o, int id) { for (int i = 0; i < 20; i++) o[i] = (uint8_t)(id * 31 + i * 7 + 1); o[2] = 0; }
typedef struct { uint8_t d[8192]; size_t n; } bufw;
static void put(bufw *w, const void *p, size_t n) { memcpy(w->d + w->n, p, n); w->n += n; }
static void put_page(bufw *w, int n, int id0) { uint16_t c = (uint16_t)n; put(w, &c, 2); for (int i = 0; i < n; i++) { uint8_t e[4] = { (uint8_t)i, 1, 20, 0 }, it[20]; mk(it, id0 + i); put(w, e, 4); put(w, it, 20); } }
static void hdr(bufw *w, int ver, int cur, int pend, int cnt) { uint8_t h[8] = { 'S', 'W', 'P', 'G', (uint8_t)ver, (uint8_t)cur, (uint8_t)pend, (uint8_t)cnt }; w->n = 0; put(w, h, 8); }
static void spath(char *p, size_t n) { snprintf(p, n, "%s/%s/%s.swpage", g_wd, g_pchar, g_pchar); }
static void wfile(const bufw *w) { char p[600]; spath(p, sizeof p); FILE *f = fopen(p, "wb"); fwrite(w->d, 1, w->n, f); fclose(f); }
static size_t rfile(uint8_t *o, size_t cap) { char p[600]; spath(p, sizeof p); FILE *f = fopen(p, "rb"); if (!f) return 0; size_t n = fread(o, 1, cap, f); fclose(f); return n; }
static uint8_t player[0x100];   // +0x90 = 0：没有物品栏（格空） | +0x90 = 0: no inventory (empty grid)
static void load(void) { pages_load(NULL, (uint64_t)(uintptr_t)player); }
static int total(void) { int t = 0; for (int i = 0; i < PGN_MAX; i++) t += g_pp[i].n; return t; }
int main(void) {
    snprintf(g_wd, sizeof g_wd, "/tmp/t_pages_%d", (int)getpid()); strlcpy(g_pchar, "Hero", sizeof g_pchar);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/Hero", g_wd, g_wd); if (system(cmd)) return 2;
    bufw w, w2; static uint8_t got[8192]; size_t gn;
    // 版本 2、5 页、第 2 页 17 件（同 Vertruid 现在的样子） | Version 2, 5 pages, 17 items on page 2 (like Vertruid now)
    hdr(&w, 2, 0, 0, 0); put_page(&w, 0, 0); put_page(&w, 17, 100); put_page(&w, 0, 0); put_page(&w, 0, 0); put_page(&w, 0, 0); wfile(&w);
    load();
    CHECK(!g_pro && g_pcur == 0 && g_pp[1].n == 17 && total() == 17, "版本 2 的 5 页读进 10 页：第 2 页 17 件，一件不少");
    CHECK(dm_pages_count() == 10, "页数 10（界面写「n/10」）");
    gn = rfile(got, sizeof got); CHECK(gn == w.n && !memcmp(got, w.d, w.n), "只读进来、不翻：侧文件不动");
    CHECK(pages_save(g_wd) == 0 && (gn = rfile(got, sizeof got)) == w.n && !memcmp(got, w.d, w.n), "后几页都空：写回版本 2，与 5 页的程序逐字节相同");
    // 第 7 页放一件 → 版本 3；再读回同一份 | One item on page 7 -> version 3; read back the same
    { uint8_t it[20]; mk(it, 900); pitem t = { 3, 4, 20, it }; pitem_dup(&g_pp[6], &t); }
    CHECK(pages_save(g_wd) == 0 && (gn = rfile(got, sizeof got)) > 8 && got[4] == 3 && got[7] == 10, "第 7 页有件：写版本 3，头里页数 10");
    load(); CHECK(g_pp[6].n == 1 && g_pp[6].it[0].x == 3 && g_pp[6].it[0].y == 4 && g_pp[1].n == 17 && total() == 18, "版本 3 读回：第 7 页那件在原格，第 2 页 17 件还在");
    ppage_free(&g_pp[6]);
    CHECK(pages_save(g_wd) == 0 && (gn = rfile(got, sizeof got)) == w.n && !memcmp(got, w.d, w.n), "第 7 页清空：又写回版本 2（旧程序读得懂）");
    g_pcur = 7; CHECK(pages_save(g_wd) == 0 && (gn = rfile(got, sizeof got)) > 8 && got[4] == 3 && got[5] == 7, "当前页在第 8 页（后几页都空也一样）：写版本 3");
    load(); CHECK(g_pcur == 7 && g_pp[1].n == 17, "读回当前页第 8 页");
    // 版本 1 | Version 1
    hdr(&w2, 1, 2, 0, 0); put_page(&w2, 1, 10); put_page(&w2, 2, 20); put_page(&w2, 0, 0); put_page(&w2, 3, 30); put_page(&w2, 1, 40); wfile(&w2);
    load(); CHECK(!g_pro && g_pcur == 2 && g_pp[0].n == 1 && g_pp[1].n == 2 && g_pp[3].n == 3 && g_pp[4].n == 1 && total() == 7, "版本 1（5 页）照读");
    // 页数多于 PGN_MAX：只读 | More pages than PGN_MAX: read-only
    hdr(&w2, 3, 0, 0, 70); for (int i = 0; i < 70; i++) put_page(&w2, i == 69, 500); wfile(&w2);
    load(); CHECK(g_pro && total() == 0, "70 页的文件（上限 64）：只读，不读进来");
    CHECK(pages_save(g_wd) == -1 && (gn = rfile(got, sizeof got)) == w2.n && !memcmp(got, w2.d, w2.n), "只读时不写：70 页的文件原样");
    g_pload = 1; g_pdirty = 1; pages_pre_commit(g_wd); gn = rfile(got, sizeof got); CHECK(gn == w2.n && !memcmp(got, w2.d, w2.n), "只读时提交前钩子也不写");
    g_pload = 0;
    // 坏文件 | Bad files
    hdr(&w2, 3, 0, 0, 0); put_page(&w2, 0, 0); wfile(&w2); load(); CHECK(total() == 0 && !g_pro, "版本 3 页数 0：坏文件");
    hdr(&w2, 2, 5, 0, 0); for (int i = 0; i < 5; i++) put_page(&w2, 0, 0); wfile(&w2); load(); CHECK(g_pcur == 0, "版本 2 当前页 ≥ 5：坏文件");
    hdr(&w2, 2, 0, 0, 0); put_page(&w2, 2, 60); put_page(&w2, 3, 70); w2.n -= 5; wfile(&w2); load(); CHECK(g_pp[0].n == 2 && g_pp[1].n == 2, "截断：读好的件留着（第 2 页读到第 2 件）");
    // 待定翻页记录（版本 2，第 1 → 2 页），本角色档不在：格（空）既不是 A 也不是 B → 翻页后 | Pending record (version 2, page 1 -> 2), no character file: the (empty) grid is neither A nor B -> after
    hdr(&w2, 2, 1, 1, 0); put_page(&w2, 1, 200); put_page(&w2, 0, 0); for (int i = 0; i < 3; i++) put_page(&w2, 0, 0);
    { uint8_t ft[2] = { 0, 1 }; uint64_t h0 = 77; put(&w2, ft, 2); put(&w2, &h0, 8); put_page(&w2, 1, 200); put_page(&w2, 2, 300); put_page(&w2, 0, 0); } wfile(&w2);
    load(); CHECK(g_pcur == 1 && g_pdirty && !g_ppend && g_pA.n == 0 && g_pp[0].n == 1, "待定记录落定：翻页后，页槽照读，下次提交写回");
    hdr(&w2, 2, 1, 1, 0); for (int i = 0; i < 5; i++) put_page(&w2, 0, 0); { uint8_t ft[2] = { 0, 7 }; uint64_t h0 = 77; put(&w2, ft, 2); put(&w2, &h0, 8); put_page(&w2, 0, 0); put_page(&w2, 0, 0); put_page(&w2, 0, 0); } wfile(&w2);
    load(); CHECK(!g_ppend && g_pA.n == 0 && g_pcur == 1, "版本 2 的待定记录里第 8 页：坏记录，不落定");
    // 提交前钩子：角色档里是目标页 B → 翻页后、写定稿（不带待定） | Pre-commit hook: the character file holds target page B -> after, final write (no pending record)
    { char p[600]; uint8_t a[20], bb[20], d2s[200]; mk(a, 1); mk(bb, 2); memset(d2s, 0xee, sizeof d2s); memcpy(d2s + 50, bb, 20);
      snprintf(p, sizeof p, "%s/Hero/Hero.d2s", g_wd); FILE *f = fopen(p, "wb"); fwrite(d2s, 1, sizeof d2s, f); fclose(f);
      pages_clear(); g_pload = 1; g_pcur = 1; g_ppend = 1; g_pfrom = 0; g_pto = 1; g_pH0 = dm_fnv64(d2s, sizeof d2s);
      pitem ta = { 0, 0, 20, a }, tb = { 0, 0, 20, bb }; pitem_dup(&g_pA, &ta); pitem_dup(&g_pB, &tb); pitem_dup(&g_pp[0], &ta);
      pages_pre_commit(g_wd); gn = rfile(got, sizeof got);
      CHECK(!g_ppend && g_pA.n == 0 && gn > 8 && got[4] == 2 && got[5] == 1 && got[6] == 0, "格里是目标页：翻页后，侧文件写定稿（版本 2、当前第 2 页、不带待定）");
      // 角色档里是搬出页 A、指纹 = 翻页时那份 → 等，带待定记录写、再请存盘 | The character file holds turned-away page A, fingerprint = the one at the turn -> wait, write with the pending record, request another save
      memset(d2s, 0xee, sizeof d2s); memcpy(d2s + 50, a, 20); f = fopen(p, "wb"); fwrite(d2s, 1, sizeof d2s, f); fclose(f);
      pages_clear(); g_pload = 1; g_pcur = 1; g_ppend = 1; g_pfrom = 0; g_pto = 1; g_pH0 = dm_fnv64(d2s, sizeof d2s); pitem_dup(&g_pA, &ta); pitem_dup(&g_pB, &tb); pitem_dup(&g_pp[0], &ta);
      pages_pre_commit(g_wd); gn = rfile(got, sizeof got);
      CHECK(g_ppend && g_pmore && gn > 8 && got[6] == 1, "格里是搬出页：不落定，侧文件带待定记录，再请一次存盘");
      // 待定的是第 1 → 8 页：带待定记录也写版本 3 | Pending turn 1 -> 8: written as version 3 even with the pending record
      g_pto = 7; g_pcur = 7; pages_pre_commit(g_wd); gn = rfile(got, sizeof got); CHECK(gn > 8 && got[4] == 3 && got[6] == 1, "待定翻页到第 8 页：版本 3");
      load(); CHECK(g_pcur == 7 && !g_ppend && g_pdirty && g_pp[0].n == 1, "版本 3 的待定记录（第 1 → 8 页）进局落定：格（空）既不是 A 也不是 B → 翻页后，当前第 8 页"); }
    // 设定页数（stash_pages.txt personal_pages） | Configured page count (personal_pages in stash_pages.txt)
    hdr(&w2, 3, 0, 0, 10); for (int i = 0; i < 10; i++) put_page(&w2, i == 8 ? 2 : 0, 600); wfile(&w2);
    g_set = 6; load(); CHECK(g_pgn == 9 && dm_pages_count() == 9 && g_pp[8].n == 2, "设 6 页、第 9 页有件：这次用 9 页，第 9 页的件还在");
    ppage_free(&g_pp[8]); CHECK(pages_save(g_wd) == 0 && (gn = rfile(got, sizeof got)) > 8 && got[4] == 2, "清空第 9 页后存：第 6 页起都空 → 写回版本 2");
    load(); CHECK(g_pgn == 6, "第 9 页清空后再进局：缩到设定的 6 页");
    g_set = 15; load(); CHECK(g_pgn == 15 && dm_pages_count() == 15, "设 15 页：15 页");
    { uint8_t it[20]; mk(it, 950); pitem t = { 1, 1, 20, it }; pitem_dup(&g_pp[13], &t); }
    CHECK(pages_save(g_wd) == 0 && (gn = rfile(got, sizeof got)) > 8 && got[4] == 3 && got[7] == 15, "第 14 页有件：版本 3、头里 15 页");
    g_set = 10; load(); CHECK(g_pgn == 14 && g_pp[13].n == 1, "再设回 10 页：第 14 页有件 → 用 14 页，件还在");
    CHECK(pg_next_target(13, -1, 1, g_pgn) == 0, "用 14 页时第 14 页按 R 回第 1 页");
    g_set = 200; pages_clear(); CHECK(g_pgn == PGN_MAX, "设定超过上限：夹到 64");
    g_set = 0; pages_clear(); CHECK(g_pgn == 1, "设定 0：夹到 1");
    g_set = 10;
    // 判定表 | Verdict table
    { const char *why; uint64_t H = 5;
      CHECK(pg_verdict(0, 1, 1, 0, 0, 1, H, &why) == PGV_UNREAD, "读不到角色档：等");
      CHECK(pg_verdict(9, 0, 0, 0, 0, H, H, &why) == PGV_AFTER, "两页都空：翻页后");
      CHECK(pg_verdict(9, 3, 2, 3, 0, 6, H, &why) == PGV_AFTER, "指纹变了：翻页后（哪怕格里还是搬出页）");
      CHECK(pg_verdict(9, 3, 2, 0, 2, H, H, &why) == PGV_AFTER, "格里是目标页：翻页后");
      CHECK(pg_verdict(9, 3, 2, 3, 0, H, H, &why) == PGV_WAIT_A, "格里是搬出页、指纹没变：等");
      CHECK(pg_verdict(9, 0, 2, 0, 0, H, H, &why) == PGV_WAIT_EMPTY, "从空页翻来、同秒存出逐字节相同的档（指纹 = 翻页时那份）：等，不判翻页前");
      CHECK(pg_verdict(9, 3, 2, 1, 1, H, H, &why) == PGV_AFTER, "格里两样都不是：翻页后");
      CHECK(pg_verdict(9, 3, 2, 3, 0, H, 0, &why) == PGV_WAIT_A, "翻页时没记指纹：只按格判"); }
    // 翻页目标 | Turn target
    CHECK(pg_next_target(1, -1, 1, 10) == 2, "第 2 页按 R：第 3 页");
    CHECK(pg_next_target(1, pg_next_target(1, pg_next_target(1, -1, 1, 10), 1, 10), 1, 10) == 4, "待定时连按 R R R：目标第 5 页（证明后一次翻过去）");
    CHECK(pg_next_target(1, pg_next_target(1, -1, 1, 10), -1, 10) == 1, "R 再 L：目标回到当前页（不翻）");
    CHECK(pg_next_target(0, -1, -1, 10) == 9 && pg_next_target(9, -1, 1, 10) == 0, "回绕（同原版共享页）：第 1 页按 L 到第 10 页、第 10 页按 R 回第 1 页");
    CHECK(pg_next_target(8, -1, 5, 10) == 3 && pg_next_target(1, -1, -13, 10) == 8, "连按跨过头尾也回绕");
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_wd); (void)!system(cmd);
    printf("t_pages：%d 项，%d 项失败\n", checks, fails);
    return fails ? 1 : 0;
}
