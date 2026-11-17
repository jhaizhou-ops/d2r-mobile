/* Read-only overlay contract validator. Hashes stream through 64 KiB; no game
 * archive is loaded as a whole. Paths are opened relative to the overlay dirfd
 * with O_NOFOLLOW at EVERY component. The manifest is never auto-generated.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "overlay_validate.h"
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define OV_MANIFEST "overlay.manifest"
#define OV_MAGIC "DM_OVERLAY_V1 "
#define OV_PATH_MAX 1024
#define OV_DEPTH_MAX 32
#define OV_FILES_MAX 4096
#define OV_NODES_MAX (OV_FILES_MAX * 2)
#define OV_MANIFEST_MAX (4u * 1024u * 1024u)
#define OV_FILE_MAX (UINT64_C(8) * 1024 * 1024 * 1024)
#define OV_TOTAL_MAX (UINT64_C(16) * 1024 * 1024 * 1024)
#define OV_LINE_MAX (OV_PATH_MAX + 66)

typedef struct { char *path; unsigned char hash[CC_SHA256_DIGEST_LENGTH]; } ov_entry;
typedef struct {
    ov_entry *entries;
    size_t count, capacity;
    size_t scanned_nodes;
    uint64_t total_bytes;
    char *reason;
    size_t reason_capacity;
} ov_check;

static dm_overlay_result ov_fail(ov_check *v, dm_overlay_result code, const char *fmt, ...) {
    if (v->reason && v->reason_capacity) {
        va_list args; va_start(args, fmt);
        vsnprintf(v->reason, v->reason_capacity, fmt, args);
        va_end(args);
    }
    return code;
}

static int ov_hex(const char *text, size_t length, unsigned char digest[32]) {
    if (length != 64) return 0;
    for (size_t i = 0; i < 32; ++i) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; ++j) {
            unsigned char c = (unsigned char)text[2 * i + j];
            unsigned n;
            if (c >= '0' && c <= '9') n = c - '0';
            else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
            else return 0;
            value = value * 16 + n;
        }
        digest[i] = (unsigned char)value;
    }
    return 1;
}

static int ov_reserved(const char *part, size_t length) {
    size_t stem = 0;
    while (stem < length && part[stem] != '.') ++stem;
    if (stem == 3 && (!strncasecmp(part, "CON", 3) || !strncasecmp(part, "PRN", 3)
                     || !strncasecmp(part, "AUX", 3) || !strncasecmp(part, "NUL", 3))) return 1;
    return stem == 4 && part[3] >= '1' && part[3] <= '9'
        && (!strncasecmp(part, "COM", 3) || !strncasecmp(part, "LPT", 3));
}

static int ov_path(const char *path) {
    size_t length = strlen(path), start = 0, depth = 0;
    if (!length || length > OV_PATH_MAX) return 0;
    for (size_t i = 0; i <= length; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c == '/' || c == 0) {
            size_t n = i - start;
            if (!n || ++depth > OV_DEPTH_MAX || path[i - 1] == '.' || ov_reserved(path + start, n)) return 0;
            start = i + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+' || c == '.')) return 0;
    }
    return 1;
}

static int ov_is_data(const char *path) {
    size_t n = strlen(path);
    return n >= 10 && !strncmp(path, "data/", 5) && !strchr(path + 5, '/') && !strcmp(path + n - 5, ".data");
}

static int ov_required(const char *path) {
    if (!strcmp(path, "ctrl_touch")) return 1;
    if (!strcmp(path, "touch_strings.tsv")) return 2;
    if (!strcmp(path, "data/.build.config")) return 4;
    return ov_is_data(path) ? 8 : 0;
}

/* Bounded lines, strict LF+ASCII. EOF mid-line is invalid, not accepted as a
 * complete last row. A NUL cannot hide a second path from strlen/strcmp.
 */
static int ov_line(FILE *stream, char line[OV_LINE_MAX], size_t *total) {
    size_t used = 0;
    for (;;) {
        int c = fgetc(stream);
        if (c == EOF) return ferror(stream) || used ? -1 : 0;
        if (++*total > OV_MANIFEST_MAX) return -1;
        if (c == '\n') { line[used] = 0; return 1; }
        if (c < 32 || c > 126 || used + 1 >= OV_LINE_MAX) return -1;
        line[used++] = (char)c;
    }
}

static int ov_fold_compare(const void *left, const void *right) {
    const ov_entry *const *a = left, *const *b = right;
    return strcasecmp((*a)->path, (*b)->path);
}

static dm_overlay_result ov_read_manifest(ov_check *v, int fd, const char *expected) {
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0
            || (uint64_t)info.st_size > OV_MANIFEST_MAX) {
        close(fd); return ov_fail(v, DM_OVERLAY_INVALID, "manifest: not a bounded regular file");
    }
    FILE *stream = fdopen(fd, "rb");
    if (!stream) { close(fd); return ov_fail(v, DM_OVERLAY_INVALID, "manifest: cannot read"); }
    char line[OV_LINE_MAX]; size_t total = 0; int line_number = 1, required = 0, result;
    dm_overlay_result status = DM_OVERLAY_INVALID;
    if (ov_line(stream, line, &total) != 1 || strlen(line) != strlen(OV_MAGIC) + 64
            || strncmp(line, OV_MAGIC, strlen(OV_MAGIC)) || strcmp(line + strlen(OV_MAGIC), expected)) {
        ov_fail(v, status, "manifest: source fingerprint mismatch or malformed header"); goto done;
    }
    while ((result = ov_line(stream, line, &total)) == 1) {
        ++line_number;
        unsigned char hash[32]; size_t length = strlen(line);
        if (length < 66 || line[64] != ' ' || !ov_hex(line, 64, hash) || !ov_path(line + 65)) {
            ov_fail(v, status, "manifest: malformed line %d", line_number); goto done;
        }
        const char *path = line + 65;
        if (!strcasecmp(path, OV_MANIFEST) || (v->count && strcmp(v->entries[v->count - 1].path, path) >= 0)) {
            ov_fail(v, status, "manifest: duplicate, excluded or unordered path: %s", path); goto done;
        }
        if (v->count == OV_FILES_MAX) { ov_fail(v, status, "manifest: too many files"); goto done; }
        if (v->count == v->capacity) {
            size_t cap = v->capacity ? v->capacity * 2 : 32;
            ov_entry *entries = realloc(v->entries, cap * sizeof *entries);
            if (!entries) { ov_fail(v, status, "manifest: allocation failed"); goto done; }
            v->entries = entries; v->capacity = cap;
        }
        char *copy = strdup(path);
        if (!copy) { ov_fail(v, status, "manifest: allocation failed"); goto done; }
        v->entries[v->count].path = copy;
        memcpy(v->entries[v->count].hash, hash, sizeof hash);
        ++v->count; required |= ov_required(path);
    }
    if (result < 0) { ov_fail(v, status, "manifest: damaged or oversized line %d", line_number + 1); goto done; }
    if (required != 15) { ov_fail(v, status, "manifest: missing required marker, strings, config or data archive"); goto done; }
    ov_entry **folded = malloc(v->count * sizeof *folded);
    if (!folded) { ov_fail(v, status, "manifest: allocation failed"); goto done; }
    for (size_t i = 0; i < v->count; ++i) folded[i] = &v->entries[i];
    qsort(folded, v->count, sizeof *folded, ov_fold_compare);
    for (size_t i = 1; i < v->count; ++i) {
        if (!strcasecmp(folded[i - 1]->path, folded[i]->path)) {
            ov_fail(v, status, "manifest: case-ambiguous path: %s", folded[i]->path);
            free(folded); goto done;
        }
    }
    free(folded); status = DM_OVERLAY_OK;
done:
    fclose(stream);
    return status;
}

static int ov_open_file(int rootfd, const char *path) {
    char copy[OV_PATH_MAX + 1]; strcpy(copy, path);
    int directory = dup(rootfd);
    if (directory < 0) return -1;
    char *part = copy;
    for (;;) {
        char *slash = strchr(part, '/');
        if (!slash) {
            int fd = openat(directory, part, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            close(directory); return fd;
        }
        *slash = 0;
        int next = openat(directory, part, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
        close(directory);
        if (next < 0) return -1;
        directory = next; part = slash + 1;
    }
}

static dm_overlay_result ov_hash_file(ov_check *v, int rootfd, const ov_entry *entry) {
    int fd = ov_open_file(rootfd, entry->path);
    if (fd < 0) return ov_fail(v, DM_OVERLAY_INVALID, "payload: missing, unreadable or linked: %s", entry->path);
    struct stat before, after;
    dm_overlay_result status = DM_OVERLAY_INVALID;
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0
            || (uint64_t)before.st_size > OV_FILE_MAX
            || (ov_required(entry->path) && !before.st_size)
            || (uint64_t)before.st_size > OV_TOTAL_MAX - v->total_bytes) {
        ov_fail(v, status, "payload: invalid type, empty required file or byte limit: %s", entry->path); goto done;
    }
    unsigned char buffer[64 * 1024], digest[32]; CC_SHA256_CTX hash;
    uint64_t bytes = 0; ssize_t n;
    if (!CC_SHA256_Init(&hash)) { ov_fail(v, status, "payload: digest initialization failed"); goto done; }
    while ((n = read(fd, buffer, sizeof buffer)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; ov_fail(v, status, "payload: read failed: %s", entry->path); goto done; }
        bytes += (uint64_t)n;
        if (bytes > (uint64_t)before.st_size || !CC_SHA256_Update(&hash, buffer, (CC_LONG)n)) {
            ov_fail(v, status, "payload: changed or digest failed: %s", entry->path); goto done;
        }
    }
    if (fstat(fd, &after) || bytes != (uint64_t)before.st_size || before.st_size != after.st_size
            || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
            || !CC_SHA256_Final(digest, &hash) || memcmp(digest, entry->hash, sizeof digest)) {
        ov_fail(v, status, "payload: digest mismatch or changed: %s", entry->path); goto done;
    }
    v->total_bytes += bytes; status = DM_OVERLAY_OK;
done:
    close(fd); return status;
}

static int ov_has_entry(const ov_check *v, const char *path) {
    size_t lo = 0, hi = v->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(path, v->entries[mid].path);
        if (!cmp) return 1;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

dm_overlay_result dm_overlay_require_touch_art(const char *directory, char *reason, size_t reason_capacity) {
    ov_check v={.reason=reason,.reason_capacity=reason_capacity};
    int rootfd=open(directory,O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);
    if (rootfd<0) return ov_fail(&v,DM_OVERLAY_INVALID,"touch art: unreadable overlay");
    /* 原版图集和HUD只接受外部导入；旧版只含CASC的overlay不能静默失去技能图标和数值。
       Native atlases and HUD are externally imported; old CASC-only overlays must not silently lose icons and numbers. */
    static const char *const names[]={
        "touchart/dm_native_skill_0.png","touchart/dm_native_skill_1.png","touchart/dm_native_skill_2.png",
        "touchart/dm_native_skill_3.png","touchart/dm_native_skill_4.png","touchart/dm_native_skill_5.png",
        "touchart/dm_native_skill_6.png","touchart/dm_native_skill_7.png","touchart/dm_native_skill_8.png",
        "touchart/dm_mobile_socket.png",
        "hud_v4_ui/sil_dm_native_skill_0.png","hud_v4_ui/sil_dm_native_skill_1.png","hud_v4_ui/sil_dm_native_skill_2.png",
        "hud_v4_ui/sil_dm_native_skill_3.png","hud_v4_ui/sil_dm_native_skill_4.png","hud_v4_ui/sil_dm_native_skill_5.png",
        "hud_v4_ui/sil_dm_native_skill_6.png","hud_v4_ui/sil_dm_native_skill_7.png","hud_v4_ui/sil_dm_native_skill_8.png",
        "hud_v4_ui/ui.json","hud_v4_ui/base_full.png","hud_v4_ui/lvl_0.png","hud_v4_ui/lvl_1.png",
        "hud_v4_ui/lvl_d0.png","hud_v4_ui/lvl_d1.png","hud_v4_ui/slot.png",
        "hud_v4_ui/diff_1.png","hud_v4_ui/diff_2.png","hud_v4_ui/diff_3.png","hud_v4_ui/diff_4.png",
        "hud_v4_ui/diff_5.png","hud_v4_ui/diff_6.png","hud_v4_ui/diff_7.png","hud_v4_ui/diff_8.png"};
    dm_overlay_result status=DM_OVERLAY_OK;
    for (size_t i=0;i<sizeof names/sizeof names[0];++i) {
        int fd=ov_open_file(rootfd,names[i]); struct stat info;
        if (fd<0 || fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_size<=0) {
            if(fd>=0)close(fd);
            status=ov_fail(&v,DM_OVERLAY_INVALID,"touch art: missing or invalid: %s",names[i]); break;
        }
        close(fd);
    }
    close(rootfd);return status;
}

static dm_overlay_result ov_inventory(ov_check *v, int dirfd_value, const char *prefix, unsigned depth, size_t *seen) {
    if (depth > OV_DEPTH_MAX) return ov_fail(v, DM_OVERLAY_INVALID, "overlay: directory depth exceeded");
    int copy = dup(dirfd_value);
    if (copy < 0) return ov_fail(v, DM_OVERLAY_INVALID, "overlay: cannot duplicate directory");
    DIR *directory = fdopendir(copy);
    if (!directory) { close(copy); return ov_fail(v, DM_OVERLAY_INVALID, "overlay: cannot enumerate directory"); }
    dm_overlay_result status = DM_OVERLAY_OK;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(directory);
        if (!item) {
            if (errno) status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: directory read failed");
            break;
        }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (++v->scanned_nodes > OV_NODES_MAX) { status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: directory entry limit exceeded"); break; }
        char relative[OV_PATH_MAX + 1];
        int length = snprintf(relative, sizeof relative, "%s%s", prefix, item->d_name);
        struct stat info;
        if (length < 0 || (size_t)length >= sizeof relative || !ov_path(relative)) {
            status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: unsafe or overlong path"); break;
        }
        if (!strcasecmp(relative, OV_MANIFEST) && strcmp(relative, OV_MANIFEST)) {
            status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: case-ambiguous manifest path"); break;
        }
        if (fstatat(dirfd_value, item->d_name, &info, AT_SYMLINK_NOFOLLOW)) {
            status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: cannot inspect: %s", relative); break;
        }
        if (S_ISDIR(info.st_mode)) {
            if ((size_t)length + 1 >= sizeof relative) { status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: path too long"); break; }
            int child = openat(dirfd_value, item->d_name, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
            if (child < 0) { status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: linked or unreadable directory: %s", relative); break; }
            relative[length] = '/'; relative[length + 1] = 0;
            status = ov_inventory(v, child, relative, depth + 1, seen);
            close(child);
            if (status != DM_OVERLAY_OK) break;
        } else if (!S_ISREG(info.st_mode)) {
            status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: link or special file: %s", relative); break;
        } else if (strcmp(relative, OV_MANIFEST)) {
            if (!ov_has_entry(v, relative) || ++*seen > OV_FILES_MAX) {
                status = ov_fail(v, DM_OVERLAY_INVALID, "overlay: unlisted file: %s", relative); break;
            }
        }
    }
    closedir(directory); return status;
}

dm_overlay_result dm_overlay_validate(const char *directory, const char *expected_source_sha256,
                                      char *reason, size_t reason_capacity) {
    ov_check v = {.reason = reason, .reason_capacity = reason_capacity};
    unsigned char expected[32];
    if (reason && reason_capacity) reason[0] = 0;
    if (!directory || !expected_source_sha256 || !ov_hex(expected_source_sha256, strlen(expected_source_sha256), expected))
        return ov_fail(&v, DM_OVERLAY_INVALID, "overlay: invalid validation arguments");
    struct stat root_info;
    if (lstat(directory, &root_info))
        return ov_fail(&v, errno == ENOENT || errno == ENOTDIR ? DM_OVERLAY_MISSING : DM_OVERLAY_INVALID,
                       "overlay: missing or unreadable directory");
    if (S_ISLNK(root_info.st_mode)) return ov_fail(&v, DM_OVERLAY_INVALID, "overlay: linked root directory");
    int rootfd = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (rootfd < 0) return ov_fail(&v, errno == ENOENT || errno == ENOTDIR ? DM_OVERLAY_MISSING : DM_OVERLAY_INVALID,
                                    "overlay: missing, linked or unreadable directory");
    int fd = openat(rootfd, OV_MANIFEST, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        dm_overlay_result missing = errno == ENOENT ? DM_OVERLAY_MISSING : DM_OVERLAY_INVALID;
        close(rootfd); return ov_fail(&v, missing, "overlay: missing, linked or unreadable manifest");
    }
    dm_overlay_result status = ov_read_manifest(&v, fd, expected_source_sha256);
    if (status == DM_OVERLAY_OK) {
        for (size_t i = 0; i < v.count; ++i) {
            status = ov_hash_file(&v, rootfd, &v.entries[i]);
            if (status != DM_OVERLAY_OK) break;
        }
    }
    if (status == DM_OVERLAY_OK) {
        size_t seen = 0;
        status = ov_inventory(&v, rootfd, "", 0, &seen);
        if (status == DM_OVERLAY_OK && seen != v.count) status = ov_fail(&v, DM_OVERLAY_INVALID, "overlay: manifest file count mismatch");
    }
    close(rootfd);
    for (size_t i = 0; i < v.count; ++i) free(v.entries[i].path);
    free(v.entries);
    return status;
}

int dm_overlay_variant(const char *overlay, const char *source_sha256, int (*enabled)(const char *),
                       char *out, size_t n) {
    char vdir[1300]; snprintf(vdir, sizeof vdir, "%s_variants", overlay);
    DIR *d = opendir(vdir); if (!d) return 0;
    char names[16][64]; int nv = 0; struct dirent *e;
    while ((e = readdir(d)) && nv < 16) if (e->d_name[0] != '.' && strlen(e->d_name) < sizeof names[0]) strcpy(names[nv++], e->d_name);
    closedir(d);
    // 变体名里出现过的键（去重）里开着几个；某份变体的键全开、个数又相等 = 键集合正好相等
    char keys[16][64]; int nk = 0, on = 0;
    for (int i = 0; i < nv; i++) { char t[64], *sp = NULL; strcpy(t, names[i]);
        for (char *k = strtok_r(t, "+", &sp); k; k = strtok_r(NULL, "+", &sp)) {
            int seen = 0; for (int j = 0; j < nk; j++) seen |= !strcmp(keys[j], k);
            if (!seen && nk < 16) { strcpy(keys[nk++], k); on += enabled(k) != 0; } } }
    if (!on) return 0;
    for (int i = 0; i < nv; i++) { char t[64], *sp = NULL; int cnt = 0, all = 1; strcpy(t, names[i]);
        for (char *k = strtok_r(t, "+", &sp); k; k = strtok_r(NULL, "+", &sp)) { cnt++; all &= enabled(k) != 0; }
        if (!all || cnt != on) continue;
        char path[1400], reason[256]; snprintf(path, sizeof path, "%s/%s", vdir, names[i]);
        dm_overlay_result r = dm_overlay_validate(path, source_sha256, reason, sizeof reason);
        if (r != DM_OVERLAY_OK) return 0;
        if (snprintf(out, n, "%s:%s", path, overlay) >= (int)n) return 0;
        return 1;
    }
    return 0;
}
