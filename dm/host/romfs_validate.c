#include "romfs_validate.h"
#include "dm_i18n.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int count_files(const char *root) {
    DIR *d = opendir(root); if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[4096]; snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        struct stat st; if (stat(p, &st)) { closedir(d); return -1; }
        n += S_ISDIR(st.st_mode) ? count_files(p) : 1;
        if (n < 0) { closedir(d); return -1; }
    }
    closedir(d); return n;
}

// 基底资源要核的只有档案文件（起产品 RomFS 只带简中：06–16 共 11 种语言的配音档案已删；开发机推的完整一份照样通过）。
// 配置文件（.build.config）不在这里：游戏读的是 App 里覆盖层自带的那份（出包时生成、缺了出包失败，见 ios/run.sh），基底里有没有、对不对都不影响；
// 密钥（build_key.txt）与各张清单（*.psv）是提取工具的产物，游戏不读。它们在任何一条路上都不是进不了游戏的原因，也不出现在给玩家的话里。
// Only the archive files of the base resources need checking (the product RomFS now ships Simplified Chinese only: the voice archives of the 11
// languages 06 to 16 have been removed; a full copy pushed from a development machine still passes).
// The config file (.build.config) is not checked here: the game reads the copy bundled in the App's overlay (generated at packaging time, packaging
// fails if it is missing, see ios/run.sh), so whether the base has one, or a correct one, makes no difference;
// the key (build_key.txt) and the manifests (*.psv) are products of the extraction tool and the game does not read them. On no path are they the reason
// the game cannot be entered, and they never appear in messages shown to the player.
static const char *need[] = {
    "00-00000001.data", "00-00000002.data", "00-00000003.data", "00-00000004.data", "00-00000005.data", "00-00000006.data",
    "01-00000001.data", "01-00000002.data", "02-00000001.data", "02-00000002.data", "03-00000001.data", "03-00000002.data",
    "04-00000001.data", "04-00000002.data", "05-00000001.data", "05-00000002.data", "17-00000001.data", "17-00000002.data",
    "07-00000001.data", "07-00000002.data", "13-00000001.data", "13-00000002.data"};
// 字节数也核：玩家从「文件」拖进来可能只拷了一半。本构建各文件的原始字节数固定（也就认出了是不是这个版本的资源）；带「.holes」区段表的剔零档案
// （hle_fs.c）= 原始字节数减去表里各零段长度，表首行的 size 须等于原始字节数。
// Byte counts are checked too: a player dragging files in from the Files app may have copied only half. The original byte count of each file is fixed
// for this build (which also tells whether the resources are of this version); for a zero-stripped archive with a ".holes" extent table
// (hle_fs.c), size = original byte count minus the lengths of the zero-filled ranges in the table, and the size on the table's first line must equal the
// original byte count.
static const long long size[] = {
    1073741824, 1073741821, 1026342790, 3664822, 391990840, 3794407,
    344377972, 37189352, 368762692, 23206186, 251620752, 55071917,
    118831405, 8915855, 478931036, 92048235, 204495617, 4496514,
    227505280, 7036462, 206331662, 4579633};
#define NEED_N (sizeof need / sizeof *need)
static int need_required(size_t i) {
    if (i < 18) return 1;
    int language = dm_voice_index();
    return (language == 2 && i < 20) || (language == 10 && i >= 20);
}
static int need_count(void) {
    int language = dm_voice_index();
    return language == 2 || language == 10 ? 20 : 18;
}
// 第 i 个必需文件此刻的状态：0 齐了，1 没有，2 字节数不对（have / want 写出现有与应有），3 区段表不对。dir = 直接放着档案文件的那个目录
// Current state of the i-th required file: 0 complete, 1 missing, 2 wrong byte count (have / want receive the actual and expected sizes), 3 wrong extent
// table. dir = the directory that directly contains the archive files
static int need_state(const char *dir, size_t i, long long *have, long long *want) {
    char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, need[i]); struct stat st; *have = 0; *want = size[i];
    if (stat(p, &st) || !S_ISREG(st.st_mode)) return 1;
    char hp[4200]; snprintf(hp, sizeof hp, "%s.holes", p); FILE *hf = fopen(hp, "r");
    if (hf) { long long lsz = -1, o, l; if (fscanf(hf, "size %lld", &lsz) != 1 || lsz != size[i]) { fclose(hf); return 3; }
        while (fscanf(hf, "%lld %lld", &o, &l) == 2) *want -= l; fclose(hf); }
    *have = (long long)st.st_size; return *have == *want ? 0 : 2;
}
static void human(long long b, char *o, size_t n) { if (b < 1000000) snprintf(o, n, dm_ui("dmRvBytes"), b); else if (b < 1000000000) snprintf(o, n, "%.0f MB", b / 1048576.0); else snprintf(o, n, "%.2f GB", b / 1073741824.0); }
// 给玩家看的完整清单（资源界面用）：已有几个 / 共几个，缺的、没拷完的逐个列出（带应有大小），不是只报第一个。返回缺 / 不全的个数
// The full list shown to the player (for the resources screen): how many present / how many in total, with each missing or incompletely copied file listed one by one (with its expected size), not just the first one. Returns the number missing / incomplete
int dm_romfs_report(const char *dir, char *out, size_t out_n) {
    int bad = 0, ok = 0; char miss[1200] = "", part[800] = ""; long long have, want;
    for (size_t i = 0; i < NEED_N; i++) { if (!need_required(i)) continue; int st = need_state(dir, i, &have, &want); char a[40], b[40]; human(want, a, sizeof a); human(have, b, sizeof b);
        if (!st) { ok++; continue; } bad++;
        if (st == 1) { if (strlen(miss) < sizeof miss - 80) snprintf(miss + strlen(miss), sizeof miss - strlen(miss), dm_ui("dmRvItem"), *miss ? dm_ui("dmRvSep") : "", need[i], a); }
        else if (strlen(part) < sizeof part - 100) snprintf(part + strlen(part), sizeof part - strlen(part), dm_ui("dmRvItem2"), *part ? dm_ui("dmRvSep") : "", need[i], st == 3 ? dm_ui("dmRvHolesBad") : b, a); }
    if (!bad) { snprintf(out, out_n, dm_ui("dmRvAllOk"), ok); return 0; }
    snprintf(out, out_n, dm_ui("dmRvReport"), ok, need_count(), *miss ? dm_ui("dmRvMissing") : "", miss, *miss ? dm_ui("dmRvPeriod") : "", *part ? dm_ui("dmRvPartial") : "", part, *part ? dm_ui("dmRvPeriod") : "");
    return bad;
}
int dm_romfs_validate(const char *dir, char *why, size_t why_n) {
    for (size_t i = 0; i < NEED_N; i++) { if (!need_required(i)) continue; long long have, want; int st = need_state(dir, i, &have, &want);
        if (st == 1) { snprintf(why, why_n, dm_ui("dmRvNoFile"), need[i]); return 0; }
        if (st == 3) { snprintf(why, why_n, dm_ui("dmRvHoles"), need[i]); return 0; }
        if (st == 2) { snprintf(why, why_n, dm_ui("dmRvShort"), need[i], have, want); return 0; }
    }
    snprintf(why, why_n, "Archivos verificados: %d; archivos en el directorio: %d", need_count(), count_files(dir));
    return 1;
}
