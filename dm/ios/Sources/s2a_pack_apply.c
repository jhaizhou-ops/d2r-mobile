// S2Arm 资源包约定（S2Arm 启动器推资源用；S2Arm 源码 runtime/src/pack_apply.c、tools/push_pack.py 是同一份约定的另一头）：
//   启动器把游戏资源按 ≤1 GB 一批打成 pax tar 推进 Documents/资源包/.incoming/，再推 index.tsv（路径 \t 字节数 \t SHA-256）与 manifest.json（index_sha256），
//   然后带环境变量 S2A_PACK_APPLY_ONLY=1 起一次 App。App 只做：按名字序解开每个 tar（解完即删）→ 按 index.tsv 逐个核文件在、大小对
//   → 存 index.applied.tsv、写 .applied = index_sha256 → 往 Documents/unpack.log 追加完成行 → 退出。不起游戏。
//   日志行的写法是约定的一部分，推送端按它判进度 / 完成 / 失败：「进度：解开 <包>（k/n）…」「… 解开 N 个包 … .applied = <sha>…」「解包失败：<原因>」。
//   解开后游戏资源在 Documents/资源包/data/<档名>，main.m romfs_locate 的第②条（「D2R Mobile」下 4 层以内直接放着档案的目录）照常认到，不用另配。
// The S2Arm resource pack convention (used by the S2Arm launcher to push resources; S2Arm's runtime/src/pack_apply.c and tools/push_pack.py are the other end of the same convention):
//   the launcher packs the game resources into pax tars of at most 1 GB, pushes them into Documents/资源包/.incoming/, then pushes index.tsv (path \t bytes \t SHA-256) and manifest.json (index_sha256),
//   and launches the App once with the environment variable S2A_PACK_APPLY_ONLY=1. The App only: unpacks every tar in name order (deleting each when done) → checks every file in index.tsv exists with the right size
//   → saves index.applied.tsv and writes .applied = index_sha256 → appends the completion line to Documents/unpack.log → exits. It does not start the game.
//   The wording of the log lines is part of the convention; the pusher reads progress / completion / failure from them.
//   Once unpacked, the game resources are at Documents/资源包/data/<archive name>, which rule ② of romfs_locate in main.m (a directory within 4 levels under "D2R Mobile" that directly holds archives) finds as usual.
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char g_why[1024];

static void logf_(const char *logp, const char *fmt, ...) {
    FILE *f = fopen(logp, "a"); if (!f) return;
    char ts[32]; time_t t = time(NULL); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "%s ", ts); va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fflush(f); fsync(fileno(f)); fclose(f);
}

static int mkparents(const char *path) {   // 建 path 的各级父目录（path 是文件路径） | create every parent directory of path (a file path)
    char p[4096]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; if (mkdir(p, 0755) && errno != EEXIST) return -1; *s = '/'; }
    return 0;
}

static int safe_rel(const char *p) {   // 相对资源包根、不出界：不许绝对路径、不许 .. 段 | relative to the pack root and staying inside it: no absolute path, no .. segment
    if (!*p || *p == '/') return 0;
    for (const char *s = p; *s; ) { const char *e = strchr(s, '/'); size_t n = e ? (size_t)(e - s) : strlen(s); if (n == 2 && !strncmp(s, "..", 2)) return 0; s += n; if (*s == '/') s++; }
    return 1;
}

static uint64_t octal(const char *s, int n) { uint64_t v = 0; for (int i = 0; i < n && s[i]; i++) if (s[i] >= '0' && s[i] <= '7') v = v * 8 + (uint64_t)(s[i] - '0'); return v; }

// 解一个 pax / ustar tar 到 root：只认普通文件、目录、pax 扩展头（path / size）；别的条目跳过。0 成功；-1 读不了；-2 截短 / 头坏；-3 路径不安全；-4 写失败
// Unpack one pax / ustar tar into root: only regular files, directories and pax extended headers (path / size) are recognized; other entries are skipped. 0 ok; -1 unreadable; -2 truncated / bad header; -3 unsafe path; -4 write failure
static int untar(const char *tar, const char *root, unsigned *nf, uint64_t *nb) {
    FILE *f = fopen(tar, "rb"); if (!f) { snprintf(g_why, sizeof g_why, "打不开 %s：%s", tar, strerror(errno)); return -1; }
    unsigned char h[512]; char pax_path[4096] = ""; uint64_t pax_size = UINT64_MAX; int rc = 0; static char buf[1 << 20];
    for (;;) {
        if (fread(h, 1, 512, f) != 512) { snprintf(g_why, sizeof g_why, "%s 截短（头不全）", tar); rc = -2; break; }
        int zero = 1; for (int i = 0; i < 512; i++) if (h[i]) { zero = 0; break; }
        if (zero) break;   // 结尾的全零块 | the all-zero end block
        char type = (char)h[156]; uint64_t size = pax_size != UINT64_MAX ? pax_size : octal((char *)h + 124, 12);
        char name[4096];
        if (pax_path[0]) snprintf(name, sizeof name, "%s", pax_path);
        else if (h[345]) snprintf(name, sizeof name, "%.155s/%.100s", (char *)h + 345, (char *)h);
        else snprintf(name, sizeof name, "%.100s", (char *)h);
        if (type == 'x' || type == 'g') {   // pax 扩展头：读出 path / size 给下一个条目（g = 全局，读过即弃） | pax extended header: path / size for the next entry (g = global, read and dropped)
            if (size > sizeof buf) { snprintf(g_why, sizeof g_why, "%s pax 头过大", tar); rc = -2; break; }
            size_t pad = (size_t)((size + 511) & ~511ULL); if (fread(buf, 1, pad, f) != pad) { snprintf(g_why, sizeof g_why, "%s 截短（pax 头）", tar); rc = -2; break; }
            if (type == 'g') continue;
            for (size_t i = 0; i < size; ) {
                size_t len = (size_t)strtoull(buf + i, NULL, 10); if (!len || i + len > size) break;
                char *kv = memchr(buf + i, ' ', len); if (kv) { kv++; size_t kl = len - (size_t)(kv - (buf + i)) - 1;
                    if (!strncmp(kv, "path=", 5)) snprintf(pax_path, sizeof pax_path, "%.*s", (int)(kl - 5), kv + 5);
                    else if (!strncmp(kv, "size=", 5)) pax_size = strtoull(kv + 5, NULL, 10); }
                i += len;
            }
            continue;
        }
        pax_path[0] = 0; pax_size = UINT64_MAX;
        while (strlen(name) > 2 && !strncmp(name, "./", 2)) memmove(name, name + 2, strlen(name) - 1);
        size_t L = strlen(name); if (L && name[L - 1] == '/') name[--L] = 0;
        uint64_t pad = (size + 511) & ~511ULL;
        if (type == '5') { if (!safe_rel(name)) { snprintf(g_why, sizeof g_why, "%s 里有不安全的路径 %s", tar, name); rc = -3; break; }
            char d[4096]; snprintf(d, sizeof d, "%s/%s/x", root, name); if (mkparents(d)) { snprintf(g_why, sizeof g_why, "建不了目录 %s：%s", name, strerror(errno)); rc = -4; break; }
            if (pad && fseeko(f, (off_t)pad, SEEK_CUR)) { rc = -2; break; } continue; }
        if (type != '0' && type != 0) { if (pad && fseeko(f, (off_t)pad, SEEK_CUR)) { rc = -2; break; } continue; }   // 别的条目类型：跳过 | other entry types: skipped
        if (!safe_rel(name)) { snprintf(g_why, sizeof g_why, "%s 里有不安全的路径 %s", tar, name); rc = -3; break; }
        char out[4096], part[4200]; snprintf(out, sizeof out, "%s/%s", root, name); snprintf(part, sizeof part, "%s.s2a-part", out);
        if (mkparents(out)) { snprintf(g_why, sizeof g_why, "建不了 %s 的目录：%s", name, strerror(errno)); rc = -4; break; }
        FILE *o = fopen(part, "wb"); if (!o) { snprintf(g_why, sizeof g_why, "写不了 %s：%s", name, strerror(errno)); rc = -4; break; }
        uint64_t left = size; while (left) { size_t n = left > sizeof buf ? sizeof buf : (size_t)left;
            if (fread(buf, 1, n, f) != n) { rc = -2; snprintf(g_why, sizeof g_why, "%s 截短（%s 的内容不全）", tar, name); break; }
            if (fwrite(buf, 1, n, o) != n) { rc = -4; snprintf(g_why, sizeof g_why, "写 %s 失败：%s", name, strerror(errno)); break; } left -= n; }
        if (fclose(o) && !rc) { rc = -4; snprintf(g_why, sizeof g_why, "写 %s 失败：%s", name, strerror(errno)); }
        if (rc) { unlink(part); break; }
        if (rename(part, out)) { snprintf(g_why, sizeof g_why, "改名 %s 失败：%s", name, strerror(errno)); unlink(part); rc = -4; break; }
        if (pad > size && fseeko(f, (off_t)(pad - size), SEEK_CUR)) { rc = -2; break; }
        (*nf)++; *nb += size;
    }
    fclose(f); return rc;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int read_file(const char *p, char **out) {
    FILE *f = fopen(p, "rb"); if (!f) return -1; fseeko(f, 0, SEEK_END); long n = (long)ftello(f); fseeko(f, 0, SEEK_SET);
    *out = malloc((size_t)n + 1); size_t got = fread(*out, 1, (size_t)n, f); fclose(f); (*out)[got] = 0; return 0;
}

static int write_text(const char *dir, const char *name, const char *text) {   // 原子写：先写临时文件再改名 | atomic write: temporary file, then rename
    char p[4096], t[4200]; snprintf(p, sizeof p, "%s/%s", dir, name); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb"); if (!f) return -1; fputs(text, f); fflush(f); fsync(fileno(f)); fclose(f); return rename(t, p);
}

// 按 S2Arm 资源包约定解开 Documents/资源包/.incoming 里的推送包；返回 0 = 解开且核过（或没有要解的），非 0 = 失败（原因已写进 unpack.log）
// Unpack the pushed tars in Documents/资源包/.incoming per the S2Arm resource pack convention; returns 0 = unpacked and checked (or nothing to unpack), non-zero = failure (reason written to unpack.log)
int dm_s2a_pack_apply(const char *docs) {
    char root[2048], inc[2100], logp[2100]; snprintf(root, sizeof root, "%s/资源包", docs); snprintf(inc, sizeof inc, "%s/.incoming", root); snprintf(logp, sizeof logp, "%s/unpack.log", docs);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    char **names = NULL; int nn = 0, cap = 0; DIR *d = opendir(inc);
    if (d) { struct dirent *e; while ((e = readdir(d))) { size_t L = strlen(e->d_name); if (L > 4 && !strcmp(e->d_name + L - 4, ".tar")) {
        if (nn == cap) { cap = cap ? cap * 2 : 16; names = realloc(names, sizeof *names * (size_t)cap); } names[nn++] = strdup(e->d_name); } } closedir(d); }
    if (nn) qsort(names, (size_t)nn, sizeof *names, by_name);
    unsigned nf = 0, ntar = 0; uint64_t nb = 0; int rc = 0;
    for (int i = 0; i < nn && !rc; i++) {
        char t[4200]; snprintf(t, sizeof t, "%s/%s", inc, names[i]); unsigned f0 = nf; uint64_t b0 = nb;
        rc = untar(t, root, &nf, &nb);
        if (rc == -2) {   // 截短的推送包永远解不开：删掉它与它后面所有待解包和推送记录，推送端下次按 index.applied.tsv 补推 | a truncated push never unpacks: delete it, every later pending tar and the push record, so the pusher re-pushes per index.applied.tsv
            for (int j = i; j < nn; j++) { char u[4200]; snprintf(u, sizeof u, "%s/%s", inc, names[j]); unlink(u); }
            char pp[2100]; snprintf(pp, sizeof pp, "%s/.pushed", root); unlink(pp);
        } else if (!rc) { unlink(t); ntar++; logf_(logp, "进度：解开 %s（%d/%d）%u 个文件 %llu MB", names[i], i + 1, nn, nf - f0, (unsigned long long)((nb - b0) >> 20)); }
    }
    for (int i = 0; i < nn; i++) free(names[i]); free(names);
    char *index = NULL, *manifest = NULL, sha[80] = "";
    if (!rc) {
        char p[2200]; snprintf(p, sizeof p, "%s/index.tsv", root);
        if (read_file(p, &index)) { snprintf(g_why, sizeof g_why, "资源包里没有 index.tsv"); rc = -5; }
        snprintf(p, sizeof p, "%s/manifest.json", root);
        if (!rc && read_file(p, &manifest)) { snprintf(g_why, sizeof g_why, "资源包里没有 manifest.json"); rc = -5; }
        const char *k = manifest ? strstr(manifest, "\"index_sha256\"") : NULL;
        if (!rc && k && (k = strchr(k + 14, '"'))) { k++; size_t n = strcspn(k, "\""); snprintf(sha, sizeof sha, "%.*s", (int)(n < 79 ? n : 79), k); }
        if (!rc && !sha[0]) { snprintf(g_why, sizeof g_why, "manifest.json 里没有 index_sha256"); rc = -5; }
    }
    for (char *l = index; !rc && l && *l; ) {   // 逐个核：文件在、大小对（-8，同 S2Arm） | check each: present, right size (-8, as in S2Arm)
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char *t1 = strchr(l, '\t'); if (t1) { *t1 = 0; char f[4200]; struct stat st; unsigned long long want = strtoull(t1 + 1, NULL, 10); snprintf(f, sizeof f, "%s/%s", root, l);
            if (stat(f, &st) || (unsigned long long)st.st_size != want) { snprintf(g_why, sizeof g_why, "%s %s（应有 %llu 字节）", l, stat(f, &st) ? "不在" : "大小不对", want); rc = -8; }
            *t1 = '\t'; }
        if (e) { *e = '\n'; l = e + 1; } else break;
    }
    if (!rc && index && (write_text(root, "index.applied.tsv", index) || write_text(root, ".applied", sha))) { snprintf(g_why, sizeof g_why, "写 .applied 失败：%s", strerror(errno)); rc = -4; }
    if (!rc) { char tp[2100]; snprintf(tp, sizeof tp, "%s/.transfer", root); unlink(tp); snprintf(tp, sizeof tp, "%s/.partial", root); unlink(tp); }
    free(index); free(manifest);
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1); double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (rc) logf_(logp, "解包失败：%s（返回 -%d）", g_why, -rc);
    else logf_(logp, "解开 %u 个包 %u 个文件 %llu MB，.applied = %s（%.1f 秒）", ntar, nf, (unsigned long long)(nb >> 20), sha, secs);
    return rc;
}
