// iCloud 存档同步的核心（按角色名同步，后写的覆盖先写的，共享仓库也一样）。
// 这里只做「本地存档目录 ↔ 一个云端目录」的文件对比与搬运，不碰 iCloud 接口：iOS 侧（cloud_ios.m）给出 ubiquity 容器里的目录、确认包已下载好，再调 dm_cloud_sync。
// 宿主单测（tests/t_cloudsync.c）用本地目录当云端。
//
// 同步单位：每个角色一个（存档目录下的 <名>/，里面有 <名>.d2s），共享仓库一个（名为 _stash：根目录的 SharedStash* 文件与 ModernSharedStash* 目录；
// 角色名首字必须是字母，不会和它撞）。Settings/ 等其余东西不同步（按键、画面设置跟设备走）。
// 云端每个单位一个包 <单位>.pack：该单位全部文件打成一个文件，整个替换，不会出现新旧文件混在一起。删除标记 = 不带文件、带「已删」标志的包。
// 时刻：单位内文件修改时间的最大值（毫秒），即游戏最后一次写它的时刻；下载时把每个文件的修改时间照包里还原，两边量出来的时刻一致。
// 本地另记每个单位「上次同步时的内容摘要和时刻」（状态文件），据此分出本地改了 / 云端改了：只一边改了就照那边；两边都改了，时刻大的赢。
// 被覆盖或被删的那一份不丢：整份改名挪进备份目录 <备份>/<单位>-<时刻>/，每个单位只留最近 3 份；挪不进去就这次不覆盖、不删。
// allow_down = 0（游戏运行中，存档提交后调）：只上传，不改本地；需要拿云端版本的等下次启动。删除标记只在这时写（见下面「本地没了」）。
// 后改（出过编年史与编年史仓库被整份换成空的：新设备首次同步时本机刚建的空共享仓库按「后写赢」盖了云端）：
//   · 首次同步（这一单位没有状态记录）且两边内容不同：一律以云端为准（本地那份照常整份进备份）；运行中（allow_down = 0）不传不改，等下次启动下载。云端没有才上传本地。
//   · 共享仓库（_stash）再加一道：要被覆盖的一方「明显更多」就不覆盖——任一共享仓库档（.d2i）的编年史段变短（编年史只增不减；新装的空仓库编年史最短）、
//     或要盖上去的是没东西的新档（0 件且编年史还是新建大小）而另一份有物品，或编年史仓库侧文件（.swvault）有件而另一份根本没有这个文件（新装 / 旧版本）。
//     物品变少、编年史仓库取空都是正常玩法，不算（只有从没记过编年史的仓库被取空会被当成新档）。
// 共享仓库分叉（iCloud 离线冲突第 1 条第一笔，不丢、不永久拦）：首次同步两边不同、或上面那道保险判出「会丢东西」时，不再永久拦住——
//   启动时一律以云端为现行版（换下的本机那份照常整份进备份），本机那份有东西（物品 / 金币 / 编年史 / 编年史仓库任一项，预置与新建空档不算）就另存进
//   待合并收件箱 <存档>/_cloudinbox/<时刻>-<序号>/（在存档目录里：局内并回时与角色档、共享仓库同一次存盘提交原子换入，被杀不会复制也不会丢；
//   下划线开头不算角色、也不是共享仓库文件，iCloud 不同步它——收件箱是这台设备自己的）：mine.pack = 本机那份；base.pack = 上次同步成功时的共享仓库（状态文件旁的 .stash_base，摘要对得上才带），
//   以后在局内并回：云端是基准的后代（编年史只增不减，基准的编年史条目云端都有）时按「云端 + 本机新增 − 本机取走」，分不清来历时按同一件取一份的并集
//   （单机游戏里少数情况下多出一件比丢一件好）。收不进收件箱就这次不动。
//   运行中（allow_down = 0）遇到分叉不动、等下次启动。新收进一份就进局提示一次（dm_cloud_conflict_take）。
//   每次共享仓库同步成功（上传 / 下载 / 一致）都把那一刻的内容写进 <状态文件>.stash_base，给下次分叉当基准。
//   iCloud 自己的同名文件冲突版本（cloud_ios.m 读出来）也走收件箱：dm_cloud_inbox_add。
// Core of iCloud save sync (synced by character name, the later write overwrites the earlier one, and the shared stash works the same way).
// This only does the file comparison and moving for "local save directory ↔ one cloud directory" and does not touch the iCloud API: the iOS side (cloud_ios.m) provides the directory in the ubiquity container, confirms the packs have finished downloading, and then calls dm_cloud_sync.
// The host unit test (tests/t_cloudsync.c) uses a local directory as the cloud.
// Sync units: one per character (<name>/ under the save directory, containing <name>.d2s), and one for the shared stash (named _stash: the SharedStash* files and ModernSharedStash* directories in the root;
// a character name must start with a letter, so it cannot collide with it). Everything else such as Settings/ is not synced (key bindings and display settings follow the device).
// One pack <unit>.pack per unit in the cloud: all of the unit's files packed into one file and replaced as a whole, so old and new files never get mixed together. Deletion marker = a pack with no files and the "deleted" flag.
// Timestamp: the maximum modification time (milliseconds) of the files in the unit, i.e. the moment the game last wrote it; on download each file's modification time is restored as in the pack, so the timestamp measured on both sides agrees.
// Locally, "the content digest and timestamp at the last sync" is also recorded per unit (state file), which separates changed locally / changed in the cloud: if only one side changed, follow that side; if both changed, the larger timestamp wins.
// The copy that is overwritten or deleted is not lost: the whole copy is renamed into the backup directory <backup>/<unit>-<timestamp>/, keeping only the most recent 3 copies per unit; if it cannot be moved in, no overwrite or delete happens this time.
// allow_down = 0 (game running, called after a save commit): upload only, no local changes; anything needing the cloud version waits for the next launch. Deletion markers are written only at this time (see "Missing locally" below).
// Later change (the chronicle and the chronicle vault were once replaced wholesale with empty ones: at a new device's first sync, the empty shared stash just created locally overwrote the cloud under "later write wins"):
//   - First sync (this unit has no state record) and the two sides differ: the cloud is always authoritative (the local copy goes into the backup whole as usual); while running (allow_down = 0) nothing is uploaded or changed, and the download waits for the next launch. Local is uploaded only if the cloud has nothing.
//   - The shared stash (_stash) gets one more check: do not overwrite if the side to be overwritten "clearly has more" -- the chronicle segment of any shared stash save (.d2i) gets shorter (the chronicle only grows, never shrinks; a freshly installed empty stash has the shortest chronicle),
//     or what would overwrite is an empty new save (0 items and the chronicle still at the freshly-created size) while the other copy has items, or the chronicle vault side file (.swvault) has items while the other copy does not have this file at all (fresh install / old version).
//     Fewer items and an emptied chronicle vault are both normal play and do not count (only a stash that never recorded any chronicle being emptied is treated as a new save).
// Shared stash fork (iCloud offline conflict item 1, first step: nothing lost, nothing blocked forever): when the two sides differ at first sync, or the check above finds that "something would be lost", it is no longer blocked forever -
//   at launch the cloud is always taken as the current version (the local copy it replaces goes into the backup whole as usual), and if the local copy has anything (items / gold / chronicle / chronicle vault, any of them; the preset and freshly created empty saves do not count) it is also stored in
//   the to-merge inbox <save>/_cloudinbox/<timestamp>-<seq>/ (inside the save directory: when merged back in a run it is swapped in atomically in the same save commit as the character saves and the shared stash, so being killed neither duplicates nor loses anything;
//   names starting with an underscore are not characters and not shared stash files, and iCloud does not sync it - the inbox belongs to this device): mine.pack = the local copy; base.pack = the shared stash at the last successful sync (.stash_base next to the state file, included only if its digest matches),
//   to be merged back in a run later: when the cloud is a descendant of the base (the chronicle only grows; the cloud has every chronicle entry of the base) as "cloud + added locally - taken out locally"; when the origin cannot be told apart, as a union taking one of each identical item
//   (in a single-player game, one extra item in rare cases is better than one lost). If it cannot be stored in the inbox, nothing is touched this time.
//   While running (allow_down = 0) a fork is left alone until the next launch. Each newly stored copy gives one notice after entering a run (dm_cloud_conflict_take).
//   Every successful shared stash sync (upload / download / identical) writes the content at that moment to <state file>.stash_base, as the base for the next fork.
//   iCloud's own same-name file conflict versions (read out by cloud_ios.m) also go to the inbox: dm_cloud_inbox_add.
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "dm.h"

#define MAXF 64
typedef struct { char path[256]; uint64_t mt; uint32_t size; uint8_t *data; } cs_file;   // path：相对存档目录 | path: relative to the save directory
typedef struct { int n, tomb; uint64_t stamp, hash; cs_file f[MAXF]; } cs_unit;
static const char k_magic[8] = "SWPK1\n\0\0";

static void unit_free(cs_unit *u) { for (int i = 0; i < u->n; i++) free(u->f[i].data); u->n = 0; }
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static int cmp_file(const void *a, const void *b) { return strcmp(((const cs_file *)a)->path, ((const cs_file *)b)->path); }
static void unit_finish(cs_unit *u) {   // 按路径排序后算摘要（路径 + 内容，不含修改时间）与时刻 | after sorting by path, compute the digest (path + contents, excluding modification time) and the timestamp
    qsort(u->f, (size_t)u->n, sizeof *u->f, cmp_file); u->hash = 0xcbf29ce484222325ull; u->stamp = 0;
    for (int i = 0; i < u->n; i++) { u->hash = fnv(u->hash, u->f[i].path, strlen(u->f[i].path) + 1); u->hash = fnv(u->hash, u->f[i].data, u->f[i].size); if (u->f[i].mt > u->stamp) u->stamp = u->f[i].mt; }
}
static int read_file(const char *full, const char *rel, cs_unit *u) {
    struct stat st; if (u->n >= MAXF || stat(full, &st) || !S_ISREG(st.st_mode) || st.st_size > (16 << 20)) return -1;
    FILE *f = fopen(full, "rb"); if (!f) return -1;
    cs_file *x = &u->f[u->n]; x->data = malloc((size_t)st.st_size + 1); x->size = (uint32_t)st.st_size;
    int ok = x->data && fread(x->data, 1, x->size, f) == x->size; fclose(f); if (!ok) { free(x->data); return -1; }
    strlcpy(x->path, rel, sizeof x->path); x->mt = (uint64_t)st.st_mtimespec.tv_sec * 1000 + (uint64_t)st.st_mtimespec.tv_nsec / 1000000; u->n++; return 0;
}
static int read_dir(const char *root, const char *rel, cs_unit *u) {   // rel 目录下的普通文件（一层，存档里的目录都是一层） | regular files under the rel directory (one level; directories in the save are all one level)
    char d[1300]; snprintf(d, sizeof d, "%s/%s", root, rel); DIR *dp = opendir(d); struct dirent *e; if (!dp) return -1; int r = 0;
    while ((e = readdir(dp))) { if (e->d_name[0] == '.') continue; char full[1600], rp[600]; snprintf(full, sizeof full, "%s/%s", d, e->d_name); snprintf(rp, sizeof rp, "%s/%s", rel, e->d_name); struct stat st;
        if (!stat(full, &st) && S_ISREG(st.st_mode) && read_file(full, rp, u)) r = -1; }
    closedir(dp); return r;
}
// ---- 共享仓库的内容量（只认结构，不解物品）：.d2i 按 55 AA 55 AA 分段，段内 +64 是 'JM' 的是物品页（+66 物品数），其余段是编年史；.swvault 头 'SWVT'、+6 件数
// ---- Content volume of the shared stash (structure only, items are not decoded): .d2i is split into segments by 55 AA 55 AA; a segment with 'JM' at +64 is an item page (+66 item count), the other segments are the chronicle; .swvault has the header 'SWVT', +6 item count
typedef struct { int kind, items, chron, vault; long long gold; } cs_rich;
// 游戏新建的空共享仓库：物品 0 件、编年史段 148 字节（三份新建档、rf3 的专家仓库都是这个值）。「0 件且编年史还是新建大小」= 没东西的新档，
// 不许盖掉有物品的一份（rf3：云端专家仓库 2 件、编年史 148，本机新建的空档同样 148，只比编年史拦不住）；编年史长过新建的仓库被玩家取空照常同步
// An empty shared stash newly created by the game: 0 items, chronicle segment 148 bytes (three freshly created saves and rf3's hardcore stash all have this value). "0 items and the chronicle still at the freshly-created size" = an empty new save,
// which may not overwrite a copy that has items (rf3: cloud hardcore stash 2 items, chronicle 148, and the locally created empty save is also 148, so comparing the chronicle alone cannot block it); a stash whose chronicle is longer than freshly-created and that was emptied by the player syncs as usual
enum { FRESH_CHRON = 148 };
// 共享仓库预置（hle_fs.c stash_seed：新设备没有共享仓库档时写的「250 万金币 + 2 枚赦免之证」，标准 / 专家同一份字节）：逐字节相同就当「没东西的新档」——
// 它被盖不算丢（rf3：启动同步跑在写预置之前，本机还没有专家仓库档，云端那份只是另一台的预置，误判「2 件 → 0 件」弹了提示），它要盖有东西的一份照拦
// Shared stash seed (hle_fs.c stash_seed: the "2.5 million gold + 2 Tokens of Absolution" written when a new device has no shared stash save, the same bytes for standard / hardcore): if byte-for-byte identical, treat it as "an empty new save" --
// it being overwritten does not count as a loss (rf3: the launch sync ran before the seed was written, the local machine had no hardcore stash save yet, and the cloud copy was merely another device's seed, misjudged as "2 items → 0 items" and a notification popped up); it overwriting a copy that has things in it is still blocked
#if __has_include("stash_seed.h")   // 公开仓库不带这份预置（见 hle_fs.c）：没有它就没有「预置档」这一类要认 | the public repository does not ship this seed (see hle_fs.c): without it there is no "seed save" category to recognize
#include "stash_seed.h"
#define CS_HAVE_STASH_SEED 1
#endif
   // kind：1 = .d2i，2 = .swvault，0 = 不管
   // kind: 1 = .d2i, 2 = .swvault, 0 = ignored
static cs_rich rich_of(const cs_file *f) {
    cs_rich r = {0}; size_t L = strlen(f->path);
    if (L > 4 && !strcmp(f->path + L - 4, ".d2i")) { r.kind = 1;
#ifdef CS_HAVE_STASH_SEED
        if (f->size == sizeof g_stash_seed && !memcmp(f->data, g_stash_seed, sizeof g_stash_seed)) { r.chron = FRESH_CHRON; return r; }   // 预置：0 件、新建大小 | seed: 0 items, freshly-created size
#endif
        for (uint32_t o = 0; o + 68 <= f->size && !memcmp(f->data + o, "\x55\xaa\x55\xaa", 4); ) { uint32_t ln; memcpy(&ln, f->data + o + 16, 4); if (ln < 68 || o + ln > f->size) break;
            if (!memcmp(f->data + o + 64, "JM", 2)) { uint16_t c; uint32_t g; memcpy(&c, f->data + o + 66, 2); memcpy(&g, f->data + o + 12, 4); r.items += c; r.gold += g; } else r.chron += (int)ln; o += ln; } }
    else if (L > 8 && !strcmp(f->path + L - 8, ".swvault")) { r.kind = 2; if (f->size >= 8 && !memcmp(f->data, "SWVT", 4)) { uint16_t c; memcpy(&c, f->data + 6, 2); r.vault = c; } }
    return r;
}
// ---- 编年史段并集（iCloud 离线冲突第三笔）：编年史只增不减，两份共享仓库档里找到过的条目都该留着。
// 段格式（游戏写段函数 main+0x4cb6cc、读段 0x4cc5cc，读码）：段头 64 字节后 +64 魔数 C0 ED EA C0、+68 u16 版本 1、+70 / +72 / +74 u16 三组条数 A / B / C、
// +76 8 字节 0、+84 起 (A + B + C) 条记录按组排、每条 10 字节（u32 表内序号、u16、u32 时刻 = 1970 年起的分钟数），段长 = 84 + 10 × 条数 + 64（末尾 64 字节游戏不写、是零）。
// 并法：src 里有、dst 同一组里没有同一个序号的条目追加到 dst 那一组末尾（组内次序游戏不认，它读进哈希表）；两边都有的取更早的时刻。返回加了几条，-1 = 格式不认（不动）
// ---- Chronicle segment union (iCloud offline conflict part three): the Chronicle only grows, so entries found in either shared stash file should all be kept.
// Segment format (game segment write function main+0x4cb6cc, segment read 0x4cc5cc, from reading the code): after the 64-byte segment header, +64 magic C0 ED EA C0, +68 u16 version 1, +70 / +72 / +74 u16 counts of three groups A / B / C,
// +76 8 bytes of 0, from +84 (A + B + C) records ordered by group, 10 bytes each (u32 table index, u16, u32 time = minutes since 1970); segment length = 84 + 10 × count + 64 (the last 64 bytes are not written by the game and are zero).
// Merge: entries in src with no same index in the same group of dst are appended to the end of that group in dst (the game ignores in-group order; it reads into a hash table); for entries in both, the earlier time is taken. Returns the number added, -1 = unrecognized format (untouched)
static int chron_seg(const uint8_t *d, uint32_t n, uint32_t *at, uint32_t *len) {   // 找编年史段：1 = 找到 | Find the Chronicle segment: 1 = found
    for (uint32_t o = 0; o + 84 <= n && !memcmp(d + o, "\x55\xaa\x55\xaa", 4); ) { uint32_t ln; memcpy(&ln, d + o + 16, 4); if (ln < 68 || o + ln > n) return 0;
        if (!memcmp(d + o + 64, "\xc0\xed\xea\xc0", 4)) { uint16_t v, a, b, c; memcpy(&v, d + o + 68, 2); memcpy(&a, d + o + 70, 2); memcpy(&b, d + o + 72, 2); memcpy(&c, d + o + 74, 2);
            if (v != 1 || ln != 84u + 10u * (uint32_t)(a + b + c) + 64u) return 0; *at = o; *len = ln; return 1; }
        o += ln; }
    return 0;
}
static int chron_union(cs_file *dst, const cs_file *src) {
    uint32_t da, dl, sa, sl; if (!chron_seg(dst->data, dst->size, &da, &dl) || !chron_seg(src->data, src->size, &sa, &sl)) return -1;
    uint16_t dc[3], sc[3]; memcpy(dc, dst->data + da + 70, 6); memcpy(sc, src->data + sa + 70, 6);
    uint32_t dn = dc[0] + dc[1] + dc[2], sn = sc[0] + sc[1] + sc[2];
    uint8_t *rec = malloc(10u * (dn + sn) + 1); if (!rec) return -1;
    uint32_t out[3] = {0}, w = 0, add = 0; const uint8_t *dr = dst->data + da + 84, *sr = src->data + sa + 84;
    for (int g = 0, d0 = 0, s0 = 0; g < 3; d0 += dc[g], s0 += sc[g], g++) {
        for (int i = 0; i < dc[g]; i++) { memcpy(rec + 10 * w, dr + 10 * (d0 + i), 10); uint32_t id, t; memcpy(&id, rec + 10 * w, 4); memcpy(&t, rec + 10 * w + 6, 4);
            for (int j = 0; j < sc[g]; j++) { uint32_t sid, st; memcpy(&sid, sr + 10 * (s0 + j), 4); memcpy(&st, sr + 10 * (s0 + j) + 6, 4); if (sid == id && st < t) { t = st; memcpy(rec + 10 * w + 6, &t, 4); } }
            w++; out[g]++; }
        for (int j = 0; j < sc[g]; j++) { uint32_t sid; memcpy(&sid, sr + 10 * (s0 + j), 4); int have = 0;
            for (int i = 0; i < dc[g] && !have; i++) { uint32_t id; memcpy(&id, dr + 10 * (d0 + i), 4); have = id == sid; }
            if (!have) { memcpy(rec + 10 * w, sr + 10 * (s0 + j), 10); w++; out[g]++; add++; } } }
    uint32_t nl = 84 + 10 * w + 64, nsz = dst->size - dl + nl; uint8_t *nd = malloc(nsz + 1); if (!nd) { free(rec); return -1; }
    memcpy(nd, dst->data, da + 84); memcpy(nd + da + 84, rec, 10 * w); memcpy(nd + da + 84 + 10 * w, dst->data + da + dl - 64, 64);
    memcpy(nd + da + nl, dst->data + da + dl, dst->size - da - dl);
    memcpy(nd + da + 16, &nl, 4); for (int g = 0; g < 3; g++) { uint16_t c = (uint16_t)out[g]; memcpy(nd + da + 70 + 2 * g, &c, 2); }
    free(rec); free(dst->data); dst->data = nd; dst->size = nsz; return (int)add;
}
static int chron_union_unit(cs_unit *dst, const cs_unit *src, const char *who) {   // 同路径的 .d2i 逐个并；返回一共加了几条 | Merge .d2i files with the same path one by one; returns the total number of records added
    int tot = 0; for (int i = 0; i < dst->n; i++) { size_t L = strlen(dst->f[i].path); if (L < 5 || strcmp(dst->f[i].path + L - 4, ".d2i")) continue;
        for (int j = 0; j < src->n; j++) if (!strcmp(src->f[j].path, dst->f[i].path)) { int a = chron_union(&dst->f[i], &src->f[j]);
            if (a > 0) { tot += a; LOG("iCloud 同步：%s 的编年史并进%s多出的 %d 条", dst->f[i].path, who, a); } } }
    return tot;
}
// 用 w 换掉 x 会不会丢东西：会就返回原因（写日志用），不会返回 NULL
// Whether replacing x with w would lose anything: if so return the reason (for the log), otherwise return NULL
static const char *stash_shrinks(const cs_unit *w, const cs_unit *x, char *why, size_t cap) {
    for (int i = 0; i < x->n; i++) { cs_rich rx = rich_of(&x->f[i]); if (!rx.kind || (rx.kind == 1 && !rx.items && rx.chron <= FRESH_CHRON)) continue;   // 被盖的是没东西的新档 / 预置：没什么可丢 | what gets overwritten is an empty new save / the seed: nothing to lose
        const cs_file *wf = NULL; for (int j = 0; j < w->n; j++) if (!strcmp(w->f[j].path, x->f[i].path)) wf = &w->f[j];
        cs_rich rw = wf ? rich_of(wf) : (cs_rich){0};
        if (rx.kind == 1 && (rw.chron < rx.chron || (rx.items > 0 && rw.items == 0 && rw.chron <= FRESH_CHRON))) { snprintf(why, cap, "%s：编年史段 %d → %d 字节、物品 %d → %d 件", x->f[i].path, rx.chron, rw.chron, rx.items, rw.items); return why; }
        if (rx.kind == 2 && rx.vault > 0 && !wf) { snprintf(why, cap, "%s：编年史仓库 %d 件 → 没有侧文件", x->f[i].path, rx.vault); return why; } }
    return NULL;
}
static _Atomic int g_conflict;
int dm_cloud_conflict_take(void) { return __atomic_exchange_n((int *)&g_conflict, 0, __ATOMIC_SEQ_CST); }   // 进局后取一次：这次启动有共享仓库收进了待合并收件箱就弹系统提示（diag.c） | taken once after entering a run: if a shared stash was put in the to-merge inbox during this launch, show the system notice (diag.c)
static int stash_has_stuff(const cs_unit *u) {   // 共享仓库这份有没有东西值得留：任一 .d2i 有物品 / 金币 / 编年史长过新建，或编年史仓库有件（预置与新建空档不算） | whether this shared stash copy has anything worth keeping: any .d2i with items / gold / a chronicle longer than freshly created, or the chronicle vault has items (the preset and freshly created empty saves do not count)
    for (int i = 0; i < u->n; i++) { cs_rich r = rich_of(&u->f[i]); if ((r.kind == 1 && (r.items || r.gold || r.chron > FRESH_CHRON)) || (r.kind == 2 && r.vault)) return 1; }
    return 0;
}
// 同名角色（iCloud 离线冲突第 3 条）：首次同步两边都有同名角色、内容不同 → 照旧以云端为准、本机那份整份进备份，另外记下名字，进局后提示一次（diag.c）。
// 两份角色档没法合并（等级、任务、身上装备各是一条独立的历史）；改名留两份要改 .d2s 头与校验和，不做。
// Same-name character (iCloud offline conflict item 3): at first sync both sides have a character with the same name and different content -> as before the cloud is authoritative and the local copy goes into the backup whole; the name is also recorded and one notice is shown after entering a run (diag.c).
// Two character saves cannot be merged (level, quests and equipment are each an independent history); keeping both under different names would need changes to the .d2s header and checksum, not done.
static char g_char_conf[256]; static pthread_mutex_t g_cc_mu = PTHREAD_MUTEX_INITIALIZER;
static void char_conf_add(const char *nm) { pthread_mutex_lock(&g_cc_mu); size_t l = strlen(g_char_conf); if (l + strlen(nm) + 3 < sizeof g_char_conf) snprintf(g_char_conf + l, sizeof g_char_conf - l, "%s%s", l ? "、" : "", nm); pthread_mutex_unlock(&g_cc_mu); }
int dm_cloud_char_conflict_take(char *out, size_t n) { pthread_mutex_lock(&g_cc_mu); int r = g_char_conf[0] != 0; if (r) strlcpy(out, g_char_conf, n); g_char_conf[0] = 0; pthread_mutex_unlock(&g_cc_mu); return r; }

static int is_stash(const char *nm) { return !strncmp(nm, "SharedStash", 11) || !strncmp(nm, "ModernSharedStash", 17); }
static int is_char_dir(const char *save, const char *nm) { char p[1600]; struct stat st; snprintf(p, sizeof p, "%s/%s/%s.d2s", save, nm, nm); return nm[0] != '.' && nm[0] != '_' && !stat(p, &st) && S_ISREG(st.st_mode); }
// 读本地单位：返回 1 = 有，0 = 没有，-1 = 读失败（这一单位本次不动）
// Read a local unit: returns 1 = present, 0 = absent, -1 = read failed (this unit is left alone this time)
static int local_read(const char *save, const char *unit, cs_unit *u) {
    memset(u, 0, sizeof *u);
    if (strcmp(unit, "_stash")) { if (!is_char_dir(save, unit)) return 0; if (read_dir(save, unit, u)) { unit_free(u); return -1; } unit_finish(u); return 1; }
    DIR *dp = opendir(save); struct dirent *e; int r = 0; if (!dp) return -1;
    while ((e = readdir(dp))) { if (!is_stash(e->d_name)) continue; char full[1300]; struct stat st; snprintf(full, sizeof full, "%s/%s", save, e->d_name);
        if (stat(full, &st)) continue; if (S_ISDIR(st.st_mode) ? read_dir(save, e->d_name, u) : read_file(full, e->d_name, u)) r = -1; }
    closedir(dp); if (r) { unit_free(u); return -1; } if (!u->n) return 0; unit_finish(u); return 1;
}
// ---- 包：magic 8 | 时刻 u64 | 标志 u32（1 = 已删）| 文件数 u32 | 每个文件：路径长 u16、路径、修改时间 u64、长度 u32、内容
// ---- Pack: magic 8 | timestamp u64 | flags u32 (1 = deleted) | file count u32 | per file: path length u16, path, modification time u64, length u32, contents
static int pack_write(const char *path, const cs_unit *u) {
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    uint32_t fl = (uint32_t)u->tomb, n = (uint32_t)u->n; int ok = fwrite(k_magic, 8, 1, f) == 1 && fwrite(&u->stamp, 8, 1, f) == 1 && fwrite(&fl, 4, 1, f) == 1 && fwrite(&n, 4, 1, f) == 1;
    for (int i = 0; ok && i < u->n; i++) { uint16_t pl = (uint16_t)strlen(u->f[i].path);
        ok = fwrite(&pl, 2, 1, f) == 1 && fwrite(u->f[i].path, pl, 1, f) == 1 && fwrite(&u->f[i].mt, 8, 1, f) == 1 && fwrite(&u->f[i].size, 4, 1, f) == 1 && (!u->f[i].size || fwrite(u->f[i].data, u->f[i].size, 1, f) == 1); }
    ok = fclose(f) == 0 && ok; if (ok) ok = rename(tmp, path) == 0; if (!ok) unlink(tmp); return ok ? 0 : -1;   // 先写临时文件再换名：云端看到的总是整包 | write a temporary file first and then rename: what the cloud sees is always a whole pack
}
#if defined(__OHOS__)
// 把手动导入的共享仓库交给现有的局内合并流程。同一份内容只进入收件箱一次。
int dm_cloud_import_stash(const char *save, const char *source, const char *name) {
    if (!*save || !*source || !is_stash(name) || strchr(name, '/') || strstr(name, "..")) return -1;
    cs_unit unit = {0};
    if (read_dir(source, name, &unit) || !unit.n) { unit_free(&unit); return -1; }
    unit_finish(&unit);
    char inbox[1100], entry[1150], staging[1150], pack[1200];
    snprintf(inbox, sizeof inbox, "%s/_cloudinbox", save);
    if (mkdir(inbox, 0700) && errno != EEXIST) { unit_free(&unit); return -1; }
    snprintf(entry, sizeof entry, "%s/ohos-%016llx", inbox, (unsigned long long)unit.hash);
    snprintf(pack, sizeof pack, "%s/mine.pack", entry);
    if (!access(pack, R_OK)) { unit_free(&unit); return 0; }
    snprintf(staging, sizeof staging, "%s/.ohos-%016llx.part", inbox, (unsigned long long)unit.hash);
    if (mkdir(staging, 0700) && errno != EEXIST) { unit_free(&unit); return -1; }
    snprintf(pack, sizeof pack, "%s/mine.pack", staging);
    int ok = !pack_write(pack, &unit) && !rename(staging, entry);
    unit_free(&unit);
    return ok ? 1 : -1;
}
#endif
static int pack_read(const char *path, cs_unit *u) {   // 1 = 读到，0 = 没有这个包，-1 = 坏包 | 1 = read, 0 = no such pack, -1 = bad pack
    memset(u, 0, sizeof *u); FILE *f = fopen(path, "rb"); if (!f) return errno == ENOENT ? 0 : -1;
    char mg[8]; uint32_t fl = 0, n = 0; int ok = fread(mg, 8, 1, f) == 1 && !memcmp(mg, k_magic, 8) && fread(&u->stamp, 8, 1, f) == 1 && fread(&fl, 4, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && n <= MAXF;
    for (uint32_t i = 0; ok && i < n; i++) { cs_file *x = &u->f[i]; uint16_t pl = 0;
        ok = fread(&pl, 2, 1, f) == 1 && pl < sizeof x->path && fread(x->path, pl, 1, f) == 1 && fread(&x->mt, 8, 1, f) == 1 && fread(&x->size, 4, 1, f) == 1 && x->size <= (16u << 20);
        if (ok) { x->path[pl] = 0; x->data = malloc(x->size + 1); ok = x->data && (!x->size || fread(x->data, x->size, 1, f) == 1); u->n = (int)i + 1; }
        if (ok) ok = !strstr(x->path, "..") && x->path[0] != '/'; }
    fclose(f); if (!ok) { unit_free(u); return -1; }
    uint64_t st = u->stamp; u->tomb = fl & 1; unit_finish(u); u->stamp = st; return 1;   // 时刻以包头为准（删除标记没有文件） | the timestamp follows the pack header (a deletion marker has no files)
}
// 局内并回收件箱用（dm_cloudmerge.c）：读一个包，逐个文件交给 cb（rel = 包里的相对路径）。返回包里文件数，没有 / 坏包返回 -1
// For the in-game inbox merge (dm_cloudmerge.c): reads a pack and hands each file to cb (rel = relative path in the pack). Returns the number of files in the pack; -1 for missing / corrupt pack
int dm_cloud_pack_each(const char *path, void (*cb)(const char *rel, const uint8_t *d, uint32_t n, void *ctx), void *ctx) {
    cs_unit *u = malloc(sizeof *u); if (!u) return -1; int r = pack_read(path, u), n = -1;
    if (r == 1) { n = u->n; for (int i = 0; i < u->n; i++) cb(u->f[i].path, u->f[i].data, u->f[i].size, ctx); unit_free(u); }
    free(u); return n;
}
// ---- 本地写入 / 移除 / 备份
// ---- Local write / remove / backup
static int rm_rf(const char *p) {
    struct stat st; if (lstat(p, &st)) return errno == ENOENT ? 0 : -1; if (!S_ISDIR(st.st_mode)) return unlink(p);
    DIR *d = opendir(p); struct dirent *e; int r = 0; while (d && (e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char c[1600]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); r |= rm_rf(c); }
    if (d) closedir(d); return r | rmdir(p);
}
static int write_files(const char *root, const cs_unit *u) {   // 把单位的文件写到 root 下（目录按需建），修改时间照包里还原 | write the unit's files under root (creating directories as needed), restoring modification times as in the pack
    for (int i = 0; i < u->n; i++) { char full[1600]; snprintf(full, sizeof full, "%s/%s", root, u->f[i].path);
        char *sl = strrchr(full, '/'); if (sl && sl > full + strlen(root)) { *sl = 0; mkdir(full, 0755); *sl = '/'; }
        FILE *f = fopen(full, "wb"); if (!f) return -1; int ok = !u->f[i].size || fwrite(u->f[i].data, u->f[i].size, 1, f) == 1; ok = fclose(f) == 0 && ok; if (!ok) return -1;
        struct timeval tv[2]; tv[0].tv_sec = tv[1].tv_sec = (time_t)(u->f[i].mt / 1000); tv[0].tv_usec = tv[1].tv_usec = (int)(u->f[i].mt % 1000) * 1000; utimes(full, tv); }
    return 0;
}
// ---- 本地被换下 / 被删的那份：整份「改名挪进」备份目录 <备份>/<单位>-<时刻>/（同一卷上改名是原子的，不复制、不会写一半）。
// 挪不进去（建不了目录 / 改名失败）就返回 -1，调用方不覆盖、不删——宁可这次不同步，也不丢玩家存档（排查会不会删玩家本地存档时查出：原先备份写失败仍照样覆盖）
// ---- The local copy that was swapped out / deleted: the whole copy is "renamed into" the backup directory <backup>/<unit>-<timestamp>/ (a rename on the same volume is atomic, no copying, never half-written).
// If it cannot be moved in (cannot create the directory / rename failed), return -1 and the caller does not overwrite or delete -- better not to sync this time than to lose the player's save (found while investigating whether the player's local save could get deleted: previously, a failed backup write still overwrote anyway)
static int bak_dir(const char *bak, const char *unit, uint64_t stamp, char *d, size_t n) {
    mkdir(bak, 0755); snprintf(d, n, "%s/%s-%llu", bak, unit, (unsigned long long)stamp);
    rm_rf(d); return mkdir(d, 0755);   // 同名 = 同一时刻的同一份，换掉 | same name = the same copy at the same timestamp, replace it
}
static void bak_prune(const char *bak, const char *unit) {   // 每个单位只留最近 3 份：名字是 <单位>-<时刻>，时刻小的先删 | keep only the most recent 3 copies per unit: the name is <unit>-<timestamp>, and smaller timestamps are deleted first
    char pre[300]; snprintf(pre, sizeof pre, "%s-", unit); uint64_t ts[64]; int n = 0; DIR *dp = opendir(bak); struct dirent *e;
    while (dp && (e = readdir(dp)) && n < 64) if (!strncmp(e->d_name, pre, strlen(pre))) { char *end; uint64_t t = strtoull(e->d_name + strlen(pre), &end, 10); if (!*end) ts[n++] = t; }
    if (dp) closedir(dp);
    while (n > 3) { int lo = 0; for (int i = 1; i < n; i++) if (ts[i] < ts[lo]) lo = i; char p[1300]; snprintf(p, sizeof p, "%s/%s%llu", bak, pre, (unsigned long long)ts[lo]); rm_rf(p); ts[lo] = ts[--n]; }
}
static int top_names(const cs_unit *u, char (*nm)[256], int cap) {   // 单位在存档根下占的顶层名字（角色 = 一个目录；共享仓库 = 几个文件 / 目录） | top-level names the unit occupies under the save root (character = one directory; shared stash = several files / directories)
    int n = 0; for (int i = 0; i < u->n; i++) { char x[256]; strlcpy(x, u->f[i].path, sizeof x); char *sl = strchr(x, '/'); if (sl) *sl = 0;
        int dup = 0; for (int k = 0; k < n; k++) dup |= !strcmp(nm[k], x); if (!dup && n < cap) strlcpy(nm[n++], x, 256); }
    return n;
}
static int move_to_bak(const char *save, const char *bak, const char *unit, const cs_unit *cur, const char *why, char *d, size_t dn) {
    char nm[16][256]; int n = top_names(cur, nm, 16), k = 0;
    if (bak_dir(bak, unit, cur->stamp, d, dn)) { LOG("【红】iCloud 同步：%s 建不了备份目录 %s errno=%d，本次不动本地", unit, d, errno); return -1; }
    for (; k < n; k++) { char a[1600], b[1600]; snprintf(a, sizeof a, "%s/%s", save, nm[k]); snprintf(b, sizeof b, "%s/%s", d, nm[k]); if (rename(a, b)) break; }
    if (k < n) { int er = errno; while (k-- > 0) { char a[1600], b[1600]; snprintf(a, sizeof a, "%s/%s", save, nm[k]); snprintf(b, sizeof b, "%s/%s", d, nm[k]); rename(b, a); }
        rm_rf(d); LOG("【红】iCloud 同步：%s 挪进备份失败 errno=%d，已挪回，本次不动本地", unit, er); return -1; }
    LOG("iCloud 同步：%s 的%s那份挪进备份 %s", unit, why, d); bak_prune(bak, unit); return 0;
}
static int local_apply(const char *save, const char *bak, const char *unit, const cs_unit *cur, const cs_unit *cl) {   // 本地单位换成云端那份 | replace the local unit with the cloud copy
    char tmp[1300], d[1300]; snprintf(tmp, sizeof tmp, "%s/.cloudin", save); rm_rf(tmp); mkdir(tmp, 0755);
    if (write_files(tmp, cl)) { rm_rf(tmp); return -1; }                                     // 先把云端那份完整写到旁边 | first write the cloud copy out completely alongside
    if (cur && move_to_bak(save, bak, unit, cur, "本机（被云端后写覆盖）", d, sizeof d)) { rm_rf(tmp); return -1; }   // 本地那份整份挪进备份，挪不动就不换 | move the whole local copy into the backup; if it cannot be moved, do not swap
    char nm[16][256]; int n = top_names(cl, nm, 16), k = 0;
    for (; k < n; k++) { char a[1600], b[1600]; snprintf(a, sizeof a, "%s/%s", tmp, nm[k]); snprintf(b, sizeof b, "%s/%s", save, nm[k]); if (rename(a, b)) break; }
    if (k < n) {   // 换到一半失败：撤掉已换进的，把备份里那份挪回来 | failed halfway through the swap: remove what was already swapped in and move the copy in the backup back
        int er = errno; while (k-- > 0) { char b[1600]; snprintf(b, sizeof b, "%s/%s", save, nm[k]); rm_rf(b); }
        if (cur) { char on[16][256]; int m = top_names(cur, on, 16); for (int i = 0; i < m; i++) { char a[1600], b[1600]; snprintf(a, sizeof a, "%s/%s", d, on[i]); snprintf(b, sizeof b, "%s/%s", save, on[i]); rename(a, b); } }
        rm_rf(tmp); errno = er; return -1; }
    rm_rf(tmp); return 0;
}
static int backup_copy(const char *bak, const char *unit, const cs_unit *u, const char *why) {   // 云端那份（本机后写赢、要覆盖它时）：写一份副本进备份 | the cloud copy (when the local side wins as the later write and is about to overwrite it): write a copy into the backup
    char d[1300]; if (bak_dir(bak, unit, u->stamp, d, sizeof d) || write_files(d, u)) { LOG("【红】iCloud 同步：%s 的%s那份写备份失败 errno=%d，本次不上传", unit, why, errno); rm_rf(d); return -1; }
    LOG("iCloud 同步：%s 的%s那份写进备份 %s", unit, why, d); bak_prune(bak, unit); return 0;
}
// ---- 状态文件：每行「单位\t摘要\t时刻\t已删」
// ---- State file: each line is "unit\tdigest\ttimestamp\tdeleted"
typedef struct { char unit[64]; uint64_t hash, stamp; int tomb; } cs_state;
static int state_load(const char *p, cs_state *s, int cap) {
    FILE *f = fopen(p, "r"); int n = 0; char ln[256]; if (!f) return 0;
    while (n < cap && fgets(ln, sizeof ln, f)) { unsigned long long h, t; int tb; char nm[64]; if (sscanf(ln, "%63[^\t]\t%llx\t%llu\t%d", nm, &h, &t, &tb) == 4) { strlcpy(s[n].unit, nm, sizeof s[n].unit); s[n].hash = h; s[n].stamp = t; s[n].tomb = tb; n++; } }
    fclose(f); return n;
}
static void state_save(const char *p, const cs_state *s, int n) {
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", p); FILE *f = fopen(tmp, "w"); if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\t%llx\t%llu\t%d\n", s[i].unit, (unsigned long long)s[i].hash, (unsigned long long)s[i].stamp, s[i].tomb);
    if (fclose(f) == 0) rename(tmp, p); else unlink(tmp);
}
static cs_state *state_get(cs_state *s, int *n, int cap, const char *unit, int add) {
    for (int i = 0; i < *n; i++) if (!strcmp(s[i].unit, unit)) return &s[i];
    if (!add || *n >= cap) return NULL; memset(&s[*n], 0, sizeof *s); strlcpy(s[*n].unit, unit, sizeof s->unit); return &s[(*n)++];
}
static void add_name(char (*names)[64], int *n, const char *nm) { for (int i = 0; i < *n; i++) if (!strcmp(names[i], nm)) return; if (*n < 256 && strlen(nm) < 64) strlcpy(names[(*n)++], nm, 64); }

// ---- 待合并收件箱与基准（见文件头「共享仓库分叉」）
// ---- To-merge inbox and base (see "Shared stash fork" in the file header)
static void base_path(const char *statef, char *o, size_t n) { snprintf(o, n, "%s.stash_base", statef); }
static int base_missing(const char *statef) { char bp[1300]; struct stat st; base_path(statef, bp, sizeof bp); return stat(bp, &st) != 0; }   // 升级上来的设备第一次补写 | first write-out on a device upgraded from an older version
static void base_put(const char *statef, const cs_unit *u) {   // 共享仓库同步成功那一刻的内容，下次分叉时当基准 | the content at the moment of a successful shared stash sync, used as the base at the next fork
    char bp[1300]; base_path(statef, bp, sizeof bp); cs_unit t = *u; t.tomb = 0;
    if (pack_write(bp, &t)) LOG("【红】iCloud 同步：共享仓库基准写不进 %s errno=%d", bp, errno); }
// 收件箱新条目：<存档>/_cloudinbox/<时刻>-<序号>/mine.pack（+ base.pack：摘要等于 base_hash 时才带；base_hash = 0 不带）。返回 0 = 收好了
// New inbox entry: <save>/_cloudinbox/<timestamp>-<seq>/mine.pack (+ base.pack: included only when its digest equals base_hash; not included when base_hash = 0). Returns 0 = stored
static int inbox_put(const char *save, const char *statef, const cs_unit *u, uint64_t base_hash, const char *why, const char *who) {
    char ib[1300], d[1400], f[1500]; snprintf(ib, sizeof ib, "%s/_cloudinbox", save); mkdir(ib, 0755);
    int k = 0; do snprintf(d, sizeof d, "%s/%llu-%d", ib, (unsigned long long)u->stamp, k++); while (mkdir(d, 0755) && errno == EEXIST && k < 100);
    snprintf(f, sizeof f, "%s/mine.pack", d); cs_unit t = *u; t.tomb = 0;
    if (pack_write(f, &t)) { LOG("【红】iCloud 同步：共享仓库收件箱写不进 %s errno=%d", f, errno); rm_rf(d); return -1; }
    int nb = 0; if (base_hash) { char bp[1300]; base_path(statef, bp, sizeof bp); cs_unit *B = calloc(1, sizeof *B);
        if (B && pack_read(bp, B) == 1 && B->hash == base_hash) { snprintf(f, sizeof f, "%s/base.pack", d); if (pack_write(f, B)) { LOG("【红】iCloud 同步：收件箱基准写不进 %s errno=%d", f, errno); unit_free(B); free(B); rm_rf(d); return -1; } nb = 1; }
        if (B) { unit_free(B); free(B); } }
    LOG("iCloud 同步：_stash %s，%s那份收进待合并收件箱 %s（%d 个文件，%s）", why, who, d, u->n, nb ? "带上次同步的基准" : "没有基准：两份当独立来历");
    __atomic_store_n((int *)&g_conflict, 1, __ATOMIC_SEQ_CST); return 0;
}
// iCloud 冲突版本（cloud_ios.m）：把一份共享仓库包收进收件箱，基准用本机上次同步的那份。返回 0 = 收好了（或没东西不用收），-1 = 坏包 / 写不进
// iCloud conflict version (cloud_ios.m): store one shared stash pack in the inbox, with the copy from this device's last sync as the base. Returns 0 = stored (or nothing worth storing), -1 = bad pack / cannot write
int dm_cloud_inbox_add(const char *save, const char *statef, const char *pack) {
    cs_unit *u = calloc(1, sizeof *u); if (!u) return -1; int r = pack_read(pack, u), ret = -1;
    if (r == 1 && !u->tomb) { if (!stash_has_stuff(u)) ret = 0; else { char bp[1300]; base_path(statef, bp, sizeof bp); cs_unit *B = calloc(1, sizeof *B); uint64_t bh = B && pack_read(bp, B) == 1 ? B->hash : 0; if (B) { unit_free(B); free(B); }
        ret = inbox_put(save, statef, u, bh, "iCloud 冲突版本", "冲突版本"); } }
    else if (r == 1) ret = 0; else LOG("【红】iCloud 同步：冲突版本 %s 读不了", pack);
    unit_free(u); free(u); return ret;
}
// 角色包的 iCloud 冲突版本（cloud_ios.m）：包头时刻（游戏最后写它的时刻）、整份进备份、记名进局提示
// iCloud conflict version of a character pack (cloud_ios.m): the pack header timestamp (the moment the game last wrote it), the whole copy goes into the backup, the name is recorded for a notice after entering a run
uint64_t dm_cloud_pack_stamp(const char *pack) { cs_unit *u = calloc(1, sizeof *u); uint64_t t = u && pack_read(pack, u) == 1 ? u->stamp : 0; if (u) { unit_free(u); free(u); } return t; }
int dm_cloud_bak_pack(const char *bak, const char *unit, const char *pack, const char *why) {
    cs_unit *u = calloc(1, sizeof *u); int r = u && pack_read(pack, u) == 1 ? (u->tomb ? 0 : backup_copy(bak, unit, u, why)) : -1; if (u) { unit_free(u); free(u); } return r; }
void dm_cloud_char_conflict_note(const char *unit) { char_conf_add(unit); }

// 返回本次动作数（上传 + 下载 + 删除），-1 = 参数错。ready(单位) 返回 0 = 云端那份还没下载好（iCloud 占位），这一单位本次跳过。now_ms = 写删除标记用的时刻
// Returns the number of actions this time (uploads + downloads + deletions), -1 = bad arguments. ready(unit) returning 0 = the cloud copy has not finished downloading yet (iCloud placeholder), and this unit is skipped this time. now_ms = the timestamp used when writing a deletion marker
int dm_cloud_sync(const char *save, const char *cloud, const char *bak, const char *statef, int allow_down, uint64_t now_ms, int (*ready)(const char *unit)) {
    if (!save || !cloud || !bak || !statef) return -1;
    static char names[256][64]; int nn = 0; static cs_state st[256]; int ns = state_load(statef, st, 256), acts = 0;
    DIR *dp = opendir(save); struct dirent *e; while (dp && (e = readdir(dp))) { if (is_char_dir(save, e->d_name)) add_name(names, &nn, e->d_name); else if (is_stash(e->d_name)) add_name(names, &nn, "_stash"); } if (dp) closedir(dp);
    dp = opendir(cloud); while (dp && (e = readdir(dp))) { size_t l = strlen(e->d_name); if (e->d_name[0] != '.' && l > 5 && !strcmp(e->d_name + l - 5, ".pack")) { char nm[64]; if (l - 5 < sizeof nm) { memcpy(nm, e->d_name, l - 5); nm[l - 5] = 0; add_name(names, &nn, nm); } } } if (dp) closedir(dp);
    for (int i = 0; i < ns; i++) add_name(names, &nn, st[i].unit);
    for (int k = 0; k < nn; k++) {
        const char *un = names[k]; if (ready && !ready(un)) { LOG("iCloud 同步：%s 云端那份还没下载好，本次跳过", un); continue; }
        char cp[1300]; snprintf(cp, sizeof cp, "%s/%s.pack", cloud, un);
        cs_unit *L = calloc(1, sizeof *L), *C = calloc(1, sizeof *C); int hl = local_read(save, un, L), hc = pack_read(cp, C);
        cs_state *S = state_get(st, &ns, 256, un, 0);
        if (hl < 0 || hc < 0) { LOG("【红】iCloud 同步：%s %s读不了，本次跳过", un, hl < 0 ? "本地" : "云端包"); goto next; }
        int lchg = hl ? (!S || S->tomb || L->hash != S->hash) : (S && !S->tomb);   // 本地自上次同步后改过（没有了也算改：被删了） | changed locally since the last sync (being gone also counts as a change: it was deleted)
        int cchg = hc && (!S || C->stamp != S->stamp || C->tomb != S->tomb);         // 云端没有包 = 没有消息，不算改 | no pack in the cloud = no news, does not count as a change
        cs_state *W = NULL; enum { NONE, UP, DOWN, DEL_LOCAL, TOMB } act = NONE;
        if (hl && hc && !C->tomb && L->hash == C->hash) act = NONE;                  // 内容一样：只记状态 | same contents: only record the state
        else if (!S && hl && hc && !C->tomb) { if (!allow_down) goto next; act = DOWN; }   // 首次同步、两边不同：以云端为准（运行中先不动、状态不记，等下次启动下载） | first sync, the two sides differ: the cloud is authoritative (while running, leave it alone and record no state, and download at the next launch)
        else if (hl && !hc) act = UP;
        else if (!hl && hc) { if (C->tomb) act = NONE; else if (lchg && !cchg && !allow_down) act = TOMB; else act = DOWN; }
        // 本地没了：只有游戏运行中（存档提交后，allow_down = 0）发现的才算玩家删的（游戏删角色一定经过一次提交）→ 记删除标记；
        // 启动时发现本地缺了（存档目录被清空 / 换掉等游戏以外的原因）一律当本地丢了、从云端拿回来——不然一次误清空会把删除传到所有设备（Mac 上实测撞到）
        // Missing locally: only when found while the game is running (after a save commit, allow_down = 0) does it count as deleted by the player (a character deletion in the game always goes through a commit) → record a deletion marker;
        // when found missing locally at launch (the save directory was emptied / replaced, or other causes outside the game), always treat it as lost locally and fetch it back from the cloud -- otherwise one accidental wipe would propagate the deletion to all devices (hit in measurement on a Mac)
        else if (!hl && !hc) { if (lchg && S) act = NONE; }
        else if (!lchg && cchg) act = C->tomb ? DEL_LOCAL : DOWN;
        else if (lchg && !cchg) act = UP;
        else act = C->stamp > L->stamp ? (C->tomb ? DEL_LOCAL : DOWN) : UP;            // 两边都改了（或从没同步过且内容不同）：后写的赢 | both sides changed (or never synced and the contents differ): the later write wins
        if ((act == DOWN || act == DEL_LOCAL) && !allow_down) { act = NONE; goto next; }   // 运行中不改本地：等下次启动，状态不动 | do not change local data while running: wait for the next launch, state untouched
        if (!strcmp(un, "_stash") && hl && hc && !C->tomb && L->hash != C->hash) { char why[400] = "";   // 共享仓库分叉（见文件头）：云端为现行版，本机那份有东西就进收件箱 | shared stash fork (see the file header): the cloud is the current version; the local copy goes into the inbox if it has anything
            int fork = 0;
            if (!S) { snprintf(why, sizeof why, "首次同步两边不同"); fork = 1; }
            else if (act == DOWN) fork = stash_shrinks(C, L, why, sizeof why) != NULL;
            else if (act == UP) fork = stash_shrinks(L, C, why, sizeof why) != NULL;
            if (fork) {
                if (!allow_down) { LOG("iCloud 同步：_stash 分叉（%s），运行中不动，等下次启动收进收件箱", why); goto next; }
                if (stash_has_stuff(L) && inbox_put(save, statef, L, S ? S->hash : 0, why, "本机")) goto next;   // 收不进收件箱：这次不动 | cannot store it in the inbox: leave everything this time
                act = DOWN; } }
        if (act == UP) {
            if (hc && !C->tomb && C->hash != L->hash && cchg && backup_copy(bak, un, C, "云端（被本机后写覆盖）")) goto next;
            if (!strcmp(un, "_stash") && allow_down && hc && !C->tomb && C->hash != L->hash && chron_union_unit(L, C, "云端那份") > 0) {   // 编年史并集：启动时（游戏还没读档）先写回本地再上传；运行中不动本地 | Chronicle union: at startup (game has not loaded saves yet) write back locally first, then upload; at runtime leave local untouched
                uint64_t st0 = L->stamp; if (write_files(save, L)) { LOG("【红】iCloud 同步：%s 并好的编年史写不回本地 errno=%d，本次不上传", un, errno); goto next; } unit_finish(L); L->stamp = st0; }
            L->tomb = 0; if (pack_write(cp, L)) { LOG("【红】iCloud 同步：%s 上传失败 errno=%d", un, errno); goto next; }
            LOG("iCloud 同步：%s 上传（时刻 %llu，%d 个文件）", un, (unsigned long long)L->stamp, L->n); acts++;
            W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = L->hash; W->stamp = L->stamp; W->tomb = 0; }
            if (!strcmp(un, "_stash")) base_put(statef, L);
        } else if (act == DOWN) {
            uint64_t ch = C->hash; cs_unit *O = NULL;   // O = 云端原样（并编年史之前），基准要记它（下次分叉判「云端是不是基准的后代」用） | O = the cloud as is (before Chronicle merge); the baseline must record it (used on the next fork to decide "is the cloud a descendant of the baseline")
            if (!strcmp(un, "_stash") && hl && L->hash != C->hash && (O = malloc(sizeof *O))) { *O = *C; for (int i = 0; i < O->n; i++) { O->f[i].data = malloc(C->f[i].size + 1); memcpy(O->f[i].data, C->f[i].data, C->f[i].size); }
                if (chron_union_unit(C, L, "本机那份") > 0) { uint64_t st0 = C->stamp; unit_finish(C); C->stamp = st0; } }   // 编年史并集：本机那份找到过的条目并进要换进来的云端那份 | Chronicle union: entries found in the local copy are merged into the cloud copy being swapped in
            if (local_apply(save, bak, un, hl ? L : NULL, C)) { if (O) { unit_free(O); free(O); } LOG("【红】iCloud 同步：%s 下载后换进本地失败 errno=%d（本地不变）", un, errno); goto next; }
            LOG("iCloud 同步：%s 下载（时刻 %llu，%d 个文件）", un, (unsigned long long)C->stamp, C->n); acts++;
            if (!S && hl && strcmp(un, "_stash")) { char_conf_add(un); LOG("iCloud 同步：%s 两台设备各有一份同名角色，用云端那份，本机那份已进备份；进局提示", un); }
            W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = ch; W->stamp = C->stamp; W->tomb = 0; }   // 记云端原样的摘要：并过编年史的本地 ≠ 它 → 下次同步当本地改过、传上去 | Record the digest of the cloud as is: a local copy with the Chronicle merged ≠ it → on next sync it is treated as locally modified and uploaded
            if (!strcmp(un, "_stash")) base_put(statef, O ? O : C);
            if (O) { unit_free(O); free(O); }
        } else if (act == DEL_LOCAL) {
            { char d[1300]; if (move_to_bak(save, bak, un, L, "本机（另一台删了这个角色）", d, sizeof d)) goto next; }   // 删 = 整份挪进备份，挪不动就不删 | delete = move the whole copy into the backup; if it cannot be moved, do not delete
            LOG("iCloud 同步：%s 另一台删了，本机也删（备份已留）", un); acts++;
            W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = 0; W->stamp = C->stamp; W->tomb = 1; }
        } else if (act == TOMB) {
            cs_unit t = { .tomb = 1, .stamp = now_ms }; if (pack_write(cp, &t)) { LOG("【红】iCloud 同步：%s 写删除标记失败 errno=%d", un, errno); goto next; }
            LOG("iCloud 同步：%s 本机删了，云端记删除标记", un); acts++;
            W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = 0; W->stamp = now_ms; W->tomb = 1; }
        } else if (hc) { W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = C->tomb ? 0 : C->hash; W->stamp = C->stamp; W->tomb = C->tomb; }   // 一致：记成已同步 | identical: record as synced
            if (!strcmp(un, "_stash") && !C->tomb && (!S || S->hash != C->hash || base_missing(statef))) base_put(statef, C); }
    next:
        unit_free(L); unit_free(C); free(L); free(C);
    }
    state_save(statef, st, ns);
    return acts;
}
