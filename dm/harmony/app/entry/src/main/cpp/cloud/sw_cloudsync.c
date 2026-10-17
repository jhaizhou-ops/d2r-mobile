// iCloud 存档同步的核心（按角色名同步，后写的覆盖先写的，共享仓库也一样）。
// 这里只做「本地存档目录 ↔ 一个云端目录」的文件对比与搬运，不碰 iCloud 接口：iOS 侧（cloud_ios.m）给出 ubiquity 容器里的目录、确认包已下载好，再调 sw_cloud_sync。
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
//     物品变少、编年史仓库取空都是正常玩法，不拦（只有从没记过编年史的仓库被取空会被当成新档拦下，宁可不同步也不丢）。拦下时本地与云端都不动、状态不记，日志记红，进局后弹系统提示（sw_cloud_conflict_take）。
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
#include "sw.h"

#define MAXF 64
typedef struct { char path[256]; uint64_t mt; uint32_t size; uint8_t *data; } cs_file;   // path：相对存档目录
typedef struct { int n, tomb; uint64_t stamp, hash; cs_file f[MAXF]; } cs_unit;
static const char k_magic[8] = "SWPK1\n\0\0";

static void unit_free(cs_unit *u) { for (int i = 0; i < u->n; i++) free(u->f[i].data); u->n = 0; }
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static int cmp_file(const void *a, const void *b) { return strcmp(((const cs_file *)a)->path, ((const cs_file *)b)->path); }
static void unit_finish(cs_unit *u) {   // 按路径排序后算摘要（路径 + 内容，不含修改时间）与时刻
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
static int read_dir(const char *root, const char *rel, cs_unit *u) {   // rel 目录下的普通文件（一层，存档里的目录都是一层）
    char d[1300]; snprintf(d, sizeof d, "%s/%s", root, rel); DIR *dp = opendir(d); struct dirent *e; if (!dp) return -1; int r = 0;
    while ((e = readdir(dp))) { if (e->d_name[0] == '.') continue; char full[1600], rp[600]; snprintf(full, sizeof full, "%s/%s", d, e->d_name); snprintf(rp, sizeof rp, "%s/%s", rel, e->d_name); struct stat st;
        if (!stat(full, &st) && S_ISREG(st.st_mode) && read_file(full, rp, u)) r = -1; }
    closedir(dp); return r;
}
// ---- 共享仓库的内容量（只认结构，不解物品）：.d2i 按 55 AA 55 AA 分段，段内 +64 是 'JM' 的是物品页（+66 物品数），其余段是编年史；.swvault 头 'SWVT'、+6 件数
typedef struct { int kind, items, chron, vault; } cs_rich;
// 游戏新建的空共享仓库：物品 0 件、编年史段 148 字节（三份新建档、rf3 的专家仓库都是这个值）。「0 件且编年史还是新建大小」= 没东西的新档，
// 不许盖掉有物品的一份（rf3：云端专家仓库 2 件、编年史 148，本机新建的空档同样 148，只比编年史拦不住）；编年史长过新建的仓库被玩家取空照常同步
enum { FRESH_CHRON = 148 };
// 共享仓库预置（hle_fs.c stash_seed：新设备没有共享仓库档时写的「250 万金币 + 2 枚赦免之证」，标准 / 专家同一份字节）：逐字节相同就当「没东西的新档」——
// 它被盖不算丢（rf3：启动同步跑在写预置之前，本机还没有专家仓库档，云端那份只是另一台的预置，误判「2 件 → 0 件」弹了提示），它要盖有东西的一份照拦
#if __has_include("stash_seed.h")   // 公开仓库不带这份预置（见 hle_fs.c）：没有它就没有「预置档」这一类要认
#include "stash_seed.h"
#define CS_HAVE_STASH_SEED 1
#endif
   // kind：1 = .d2i，2 = .swvault，0 = 不管
static cs_rich rich_of(const cs_file *f) {
    cs_rich r = {0}; size_t L = strlen(f->path);
    if (L > 4 && !strcmp(f->path + L - 4, ".d2i")) { r.kind = 1;
#ifdef CS_HAVE_STASH_SEED
        if (f->size == sizeof g_stash_seed && !memcmp(f->data, g_stash_seed, sizeof g_stash_seed)) { r.chron = FRESH_CHRON; return r; }   // 预置：0 件、新建大小
#endif
        for (uint32_t o = 0; o + 68 <= f->size && !memcmp(f->data + o, "\x55\xaa\x55\xaa", 4); ) { uint32_t ln; memcpy(&ln, f->data + o + 16, 4); if (ln < 68 || o + ln > f->size) break;
            if (!memcmp(f->data + o + 64, "JM", 2)) { uint16_t c; memcpy(&c, f->data + o + 66, 2); r.items += c; } else r.chron += (int)ln; o += ln; } }
    else if (L > 8 && !strcmp(f->path + L - 8, ".swvault")) { r.kind = 2; if (f->size >= 8 && !memcmp(f->data, "SWVT", 4)) { uint16_t c; memcpy(&c, f->data + 6, 2); r.vault = c; } }
    return r;
}
// 用 w 换掉 x 会不会丢东西：会就返回原因（写日志用），不会返回 NULL
static const char *stash_shrinks(const cs_unit *w, const cs_unit *x, char *why, size_t cap) {
    for (int i = 0; i < x->n; i++) { cs_rich rx = rich_of(&x->f[i]); if (!rx.kind || (rx.kind == 1 && !rx.items && rx.chron <= FRESH_CHRON)) continue;   // 被盖的是没东西的新档 / 预置：没什么可丢
        const cs_file *wf = NULL; for (int j = 0; j < w->n; j++) if (!strcmp(w->f[j].path, x->f[i].path)) wf = &w->f[j];
        cs_rich rw = wf ? rich_of(wf) : (cs_rich){0};
        if (rx.kind == 1 && (rw.chron < rx.chron || (rx.items > 0 && rw.items == 0 && rw.chron <= FRESH_CHRON))) { snprintf(why, cap, "%s：编年史段 %d → %d 字节、物品 %d → %d 件", x->f[i].path, rx.chron, rw.chron, rx.items, rw.items); return why; }
        if (rx.kind == 2 && rx.vault > 0 && !wf) { snprintf(why, cap, "%s：编年史仓库 %d 件 → 没有侧文件", x->f[i].path, rx.vault); return why; } }
    return NULL;
}
static _Atomic int g_conflict;
int sw_cloud_conflict_take(void) { return __atomic_exchange_n((int *)&g_conflict, 0, __ATOMIC_SEQ_CST); }   // 进局后取一次：有拦下的共享仓库覆盖就弹系统提示（diag.c）

static int is_stash(const char *nm) { return !strncmp(nm, "SharedStash", 11) || !strncmp(nm, "ModernSharedStash", 17); }
static int is_char_dir(const char *save, const char *nm) { char p[1600]; struct stat st; snprintf(p, sizeof p, "%s/%s/%s.d2s", save, nm, nm); return nm[0] != '.' && nm[0] != '_' && !stat(p, &st) && S_ISREG(st.st_mode); }
// 读本地单位：返回 1 = 有，0 = 没有，-1 = 读失败（这一单位本次不动）
static int local_read(const char *save, const char *unit, cs_unit *u) {
    memset(u, 0, sizeof *u);
    if (strcmp(unit, "_stash")) { if (!is_char_dir(save, unit)) return 0; if (read_dir(save, unit, u)) { unit_free(u); return -1; } unit_finish(u); return 1; }
    DIR *dp = opendir(save); struct dirent *e; int r = 0; if (!dp) return -1;
    while ((e = readdir(dp))) { if (!is_stash(e->d_name)) continue; char full[1300]; struct stat st; snprintf(full, sizeof full, "%s/%s", save, e->d_name);
        if (stat(full, &st)) continue; if (S_ISDIR(st.st_mode) ? read_dir(save, e->d_name, u) : read_file(full, e->d_name, u)) r = -1; }
    closedir(dp); if (r) { unit_free(u); return -1; } if (!u->n) return 0; unit_finish(u); return 1;
}
// ---- 包：magic 8 | 时刻 u64 | 标志 u32（1 = 已删）| 文件数 u32 | 每个文件：路径长 u16、路径、修改时间 u64、长度 u32、内容
static int pack_write(const char *path, const cs_unit *u) {
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    uint32_t fl = (uint32_t)u->tomb, n = (uint32_t)u->n; int ok = fwrite(k_magic, 8, 1, f) == 1 && fwrite(&u->stamp, 8, 1, f) == 1 && fwrite(&fl, 4, 1, f) == 1 && fwrite(&n, 4, 1, f) == 1;
    for (int i = 0; ok && i < u->n; i++) { uint16_t pl = (uint16_t)strlen(u->f[i].path);
        ok = fwrite(&pl, 2, 1, f) == 1 && fwrite(u->f[i].path, pl, 1, f) == 1 && fwrite(&u->f[i].mt, 8, 1, f) == 1 && fwrite(&u->f[i].size, 4, 1, f) == 1 && (!u->f[i].size || fwrite(u->f[i].data, u->f[i].size, 1, f) == 1); }
    ok = fclose(f) == 0 && ok; if (ok) ok = rename(tmp, path) == 0; if (!ok) unlink(tmp); return ok ? 0 : -1;   // 先写临时文件再换名：云端看到的总是整包
}
static int pack_read(const char *path, cs_unit *u) {   // 1 = 读到，0 = 没有这个包，-1 = 坏包
    memset(u, 0, sizeof *u); FILE *f = fopen(path, "rb"); if (!f) return errno == ENOENT ? 0 : -1;
    char mg[8]; uint32_t fl = 0, n = 0; int ok = fread(mg, 8, 1, f) == 1 && !memcmp(mg, k_magic, 8) && fread(&u->stamp, 8, 1, f) == 1 && fread(&fl, 4, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && n <= MAXF;
    for (uint32_t i = 0; ok && i < n; i++) { cs_file *x = &u->f[i]; uint16_t pl = 0;
        ok = fread(&pl, 2, 1, f) == 1 && pl < sizeof x->path && fread(x->path, pl, 1, f) == 1 && fread(&x->mt, 8, 1, f) == 1 && fread(&x->size, 4, 1, f) == 1 && x->size <= (16u << 20);
        if (ok) { x->path[pl] = 0; x->data = malloc(x->size + 1); ok = x->data && (!x->size || fread(x->data, x->size, 1, f) == 1); u->n = (int)i + 1; }
        if (ok) ok = !strstr(x->path, "..") && x->path[0] != '/'; }
    fclose(f); if (!ok) { unit_free(u); return -1; }
    uint64_t st = u->stamp; u->tomb = fl & 1; unit_finish(u); u->stamp = st; return 1;   // 时刻以包头为准（删除标记没有文件）
}
// ---- 本地写入 / 移除 / 备份
static int rm_rf(const char *p) {
    struct stat st; if (lstat(p, &st)) return errno == ENOENT ? 0 : -1; if (!S_ISDIR(st.st_mode)) return unlink(p);
    DIR *d = opendir(p); struct dirent *e; int r = 0; while (d && (e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char c[1600]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); r |= rm_rf(c); }
    if (d) closedir(d); return r | rmdir(p);
}
static int write_files(const char *root, const cs_unit *u) {   // 把单位的文件写到 root 下（目录按需建），修改时间照包里还原
    for (int i = 0; i < u->n; i++) { char full[1600]; snprintf(full, sizeof full, "%s/%s", root, u->f[i].path);
        char *sl = strrchr(full, '/'); if (sl && sl > full + strlen(root)) { *sl = 0; mkdir(full, 0755); *sl = '/'; }
        FILE *f = fopen(full, "wb"); if (!f) return -1; int ok = !u->f[i].size || fwrite(u->f[i].data, u->f[i].size, 1, f) == 1; ok = fclose(f) == 0 && ok; if (!ok) return -1;
        struct timeval tv[2]; tv[0].tv_sec = tv[1].tv_sec = (time_t)(u->f[i].mt / 1000); tv[0].tv_usec = tv[1].tv_usec = (int)(u->f[i].mt % 1000) * 1000; utimes(full, tv); }
    return 0;
}
// ---- 本地被换下 / 被删的那份：整份「改名挪进」备份目录 <备份>/<单位>-<时刻>/（同一卷上改名是原子的，不复制、不会写一半）。
// 挪不进去（建不了目录 / 改名失败）就返回 -1，调用方不覆盖、不删——宁可这次不同步，也不丢玩家存档（排查会不会删玩家本地存档时查出：原先备份写失败仍照样覆盖）
static int bak_dir(const char *bak, const char *unit, uint64_t stamp, char *d, size_t n) {
    mkdir(bak, 0755); snprintf(d, n, "%s/%s-%llu", bak, unit, (unsigned long long)stamp);
    rm_rf(d); return mkdir(d, 0755);   // 同名 = 同一时刻的同一份，换掉
}
static void bak_prune(const char *bak, const char *unit) {   // 每个单位只留最近 3 份：名字是 <单位>-<时刻>，时刻小的先删
    char pre[300]; snprintf(pre, sizeof pre, "%s-", unit); uint64_t ts[64]; int n = 0; DIR *dp = opendir(bak); struct dirent *e;
    while (dp && (e = readdir(dp)) && n < 64) if (!strncmp(e->d_name, pre, strlen(pre))) { char *end; uint64_t t = strtoull(e->d_name + strlen(pre), &end, 10); if (!*end) ts[n++] = t; }
    if (dp) closedir(dp);
    while (n > 3) { int lo = 0; for (int i = 1; i < n; i++) if (ts[i] < ts[lo]) lo = i; char p[1300]; snprintf(p, sizeof p, "%s/%s%llu", bak, pre, (unsigned long long)ts[lo]); rm_rf(p); ts[lo] = ts[--n]; }
}
static int top_names(const cs_unit *u, char (*nm)[256], int cap) {   // 单位在存档根下占的顶层名字（角色 = 一个目录；共享仓库 = 几个文件 / 目录）
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
static int local_apply(const char *save, const char *bak, const char *unit, const cs_unit *cur, const cs_unit *cl) {   // 本地单位换成云端那份
    char tmp[1300], d[1300]; snprintf(tmp, sizeof tmp, "%s/.cloudin", save); rm_rf(tmp); mkdir(tmp, 0755);
    if (write_files(tmp, cl)) { rm_rf(tmp); return -1; }                                     // 先把云端那份完整写到旁边
    if (cur && move_to_bak(save, bak, unit, cur, "本机（被云端后写覆盖）", d, sizeof d)) { rm_rf(tmp); return -1; }   // 本地那份整份挪进备份，挪不动就不换
    char nm[16][256]; int n = top_names(cl, nm, 16), k = 0;
    for (; k < n; k++) { char a[1600], b[1600]; snprintf(a, sizeof a, "%s/%s", tmp, nm[k]); snprintf(b, sizeof b, "%s/%s", save, nm[k]); if (rename(a, b)) break; }
    if (k < n) {   // 换到一半失败：撤掉已换进的，把备份里那份挪回来
        int er = errno; while (k-- > 0) { char b[1600]; snprintf(b, sizeof b, "%s/%s", save, nm[k]); rm_rf(b); }
        if (cur) { char on[16][256]; int m = top_names(cur, on, 16); for (int i = 0; i < m; i++) { char a[1600], b[1600]; snprintf(a, sizeof a, "%s/%s", d, on[i]); snprintf(b, sizeof b, "%s/%s", save, on[i]); rename(a, b); } }
        rm_rf(tmp); errno = er; return -1; }
    rm_rf(tmp); return 0;
}
static int backup_copy(const char *bak, const char *unit, const cs_unit *u, const char *why) {   // 云端那份（本机后写赢、要覆盖它时）：写一份副本进备份
    char d[1300]; if (bak_dir(bak, unit, u->stamp, d, sizeof d) || write_files(d, u)) { LOG("【红】iCloud 同步：%s 的%s那份写备份失败 errno=%d，本次不上传", unit, why, errno); rm_rf(d); return -1; }
    LOG("iCloud 同步：%s 的%s那份写进备份 %s", unit, why, d); bak_prune(bak, unit); return 0;
}
// ---- 状态文件：每行「单位\t摘要\t时刻\t已删」
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

// 返回本次动作数（上传 + 下载 + 删除），-1 = 参数错。ready(单位) 返回 0 = 云端那份还没下载好（iCloud 占位），这一单位本次跳过。now_ms = 写删除标记用的时刻
int sw_cloud_sync(const char *save, const char *cloud, const char *bak, const char *statef, int allow_down, uint64_t now_ms, int (*ready)(const char *unit)) {
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
        int lchg = hl ? (!S || S->tomb || L->hash != S->hash) : (S && !S->tomb);   // 本地自上次同步后改过（没有了也算改：被删了）
        int cchg = hc && (!S || C->stamp != S->stamp || C->tomb != S->tomb);         // 云端没有包 = 没有消息，不算改
        cs_state *W = NULL; enum { NONE, UP, DOWN, DEL_LOCAL, TOMB } act = NONE;
        if (hl && hc && !C->tomb && L->hash == C->hash) act = NONE;                  // 内容一样：只记状态
        else if (!S && hl && hc && !C->tomb) { if (!allow_down) goto next; act = DOWN; }   // 首次同步、两边不同：以云端为准（运行中先不动、状态不记，等下次启动下载）
        else if (hl && !hc) act = UP;
        else if (!hl && hc) { if (C->tomb) act = NONE; else if (lchg && !cchg && !allow_down) act = TOMB; else act = DOWN; }
        // 本地没了：只有游戏运行中（存档提交后，allow_down = 0）发现的才算玩家删的（游戏删角色一定经过一次提交）→ 记删除标记；
        // 启动时发现本地缺了（存档目录被清空 / 换掉等游戏以外的原因）一律当本地丢了、从云端拿回来——不然一次误清空会把删除传到所有设备（Mac 上实测撞到）
        else if (!hl && !hc) { if (lchg && S) act = NONE; }
        else if (!lchg && cchg) act = C->tomb ? DEL_LOCAL : DOWN;
        else if (lchg && !cchg) act = UP;
        else act = C->stamp > L->stamp ? (C->tomb ? DEL_LOCAL : DOWN) : UP;            // 两边都改了（或从没同步过且内容不同）：后写的赢
        if ((act == DOWN || act == DEL_LOCAL) && !allow_down) { act = NONE; goto next; }   // 运行中不改本地：等下次启动，状态不动
        if (!strcmp(un, "_stash") && ((act == DOWN && hl) || (act == UP && hc && !C->tomb))) { char why[400];   // 共享仓库：要被覆盖的一方明显更多就不覆盖（见文件头）
            if (act == DOWN ? stash_shrinks(C, L, why, sizeof why) : stash_shrinks(L, C, why, sizeof why)) {
                LOG("【红】iCloud 同步：_stash %s会丢东西（%s），不覆盖、两边都不动", act == DOWN ? "下载云端那份" : "上传本机那份", why);
                __atomic_store_n((int *)&g_conflict, 1, __ATOMIC_SEQ_CST); act = NONE; goto next; } }
        if (act == UP) {
            if (hc && !C->tomb && C->hash != L->hash && cchg && backup_copy(bak, un, C, "云端（被本机后写覆盖）")) goto next;
            L->tomb = 0; if (pack_write(cp, L)) { LOG("【红】iCloud 同步：%s 上传失败 errno=%d", un, errno); goto next; }
            LOG("iCloud 同步：%s 上传（时刻 %llu，%d 个文件）", un, (unsigned long long)L->stamp, L->n); acts++;
            W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = L->hash; W->stamp = L->stamp; W->tomb = 0; }
        } else if (act == DOWN) {
            if (local_apply(save, bak, un, hl ? L : NULL, C)) { LOG("【红】iCloud 同步：%s 下载后换进本地失败 errno=%d（本地不变）", un, errno); goto next; }
            LOG("iCloud 同步：%s 下载（时刻 %llu，%d 个文件）", un, (unsigned long long)C->stamp, C->n); acts++;
            W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = C->hash; W->stamp = C->stamp; W->tomb = 0; }
        } else if (act == DEL_LOCAL) {
            { char d[1300]; if (move_to_bak(save, bak, un, L, "本机（另一台删了这个角色）", d, sizeof d)) goto next; }   // 删 = 整份挪进备份，挪不动就不删
            LOG("iCloud 同步：%s 另一台删了，本机也删（备份已留）", un); acts++;
            W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = 0; W->stamp = C->stamp; W->tomb = 1; }
        } else if (act == TOMB) {
            cs_unit t = { .tomb = 1, .stamp = now_ms }; if (pack_write(cp, &t)) { LOG("【红】iCloud 同步：%s 写删除标记失败 errno=%d", un, errno); goto next; }
            LOG("iCloud 同步：%s 本机删了，云端记删除标记", un); acts++;
            W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = 0; W->stamp = now_ms; W->tomb = 1; }
        } else if (hc) { W = state_get(st, &ns, 256, un, 1); if (W) { W->hash = C->tomb ? 0 : C->hash; W->stamp = C->stamp; W->tomb = C->tomb; } }   // 一致：记成已同步
    next:
        unit_free(L); unit_free(C); free(L); free(C);
    }
    state_save(statef, st, ns);
    return acts;
}
