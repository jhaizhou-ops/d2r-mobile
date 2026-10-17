// nn::fs：真机经 fsp-srv 服务；宿主直接接 Mac 文件系统。挂载名 → 宿主目录：
//   rom   → $SW_ROMFS（缺省 ~/xbl/data/switch/3.3.94012/Update/RomFS，只读；游戏自己解析其中的存储块）
//   存档  → $SW_SAVE（必须设置，指向标准存档 save_ref 的本局副本；未设报红中止）
//   临时  → $SW_TEMP（缺省 ~/xbl/run/platform/temp）
// 路径形如 "rom:/data/x"。FileHandle / DirectoryHandle 是按值传的 8 字节，这里存宿主 fd / DIR*。
// 结构布局按公开 Horizon ABI：DirectoryEntry = {char name[0x301], pad[3], s8 type, pad[3], s64 size}（0x310 字节）。
#include "hle_ohos_compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>



#define RES_PATH_NOT_FOUND 0x202u      // fs 模块 2，描述 1
#define RES_PATH_EXISTS    0x402u      // fs 模块 2，描述 2
enum { MODE_READ = 1, MODE_WRITE = 2, MODE_APPEND = 4 };
enum { ENT_DIR = 0, ENT_FILE = 1 };
enum { DIRMODE_DIR = 1, DIRMODE_FILE = 2 };

typedef struct { char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "DirectoryEntry 布局");

#define MAX_MOUNT 16
// 存档事务（10-02 HLE 审计第 1–3 条）：真机存档提交前可整体回滚，崩溃 / 被杀后再挂载只见上次 Commit 的版本。
// 宿主：挂载时把已提交目录 commit（= $SW_SAVE）整树 clonefile 成工作目录 <commit>.work（APFS 写时复制，近乎零成本），挂载指向工作目录，
// 游戏的读写都在工作目录；Commit = xblr_clone_tree(work → <commit>.new) 后 xblr_exchange_save(2) 与 commit 原子互换，再删掉换下来的旧版。
// 任何时刻被杀，commit 要么是旧版要么是新版，不会半截 / 新旧混合；下次挂载丢弃残留的 .work / .new。
typedef struct { char name[32]; char dir[1024]; char overlay[4096]; char commit[1024]; int ro; } mount_t;
static mount_t g_mnt[MAX_MOUNT];   // commit 非空 = 事务式存档挂载
static int g_nmnt;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static void path_unlock(pthread_mutex_t **m) { pthread_mutex_unlock(*m); }
#define PATH_LOCK() pthread_mutex_t *path_guard __attribute__((cleanup(path_unlock))) = &g_path_lock; pthread_mutex_lock(path_guard)
static uint8_t g_fmode[65536];              // fd → 打开方式
static uint8_t g_fsave[65536];              // fd → 是否在事务式存档挂载上（存档写注入点只数这些）
static _Atomic int g_save_writes;           // 存档挂载上的写次数（注入点与提交日志共用）
// 写失败按事务拒绝：真机存档空间建档时预留，覆盖写不会失败，游戏也不看 WriteFile / SetFileSize 的返回值，失败后照常 Commit
// （10-02 实测：注入第 1 次写失败后提交进去的 joe.d2s 为 0 字节＝角色丢失）。宿主写失败（盘满等，iOS 上真实可能）时：
// 从上次 Commit 到这次 Commit 之间有任一存档写 / 定长失败，这次 Commit 拒绝——已提交版本不变，工作目录丢弃、从已提交版重新克隆，
// 并通知前端（front_save_failed）；下一个事务（游戏下次整档重写）照常尝试，成功就提交。g_save_poison = 本事务首个失败的 errno（0 = 无）。
static _Atomic int g_save_poison;
static void save_poison(int e) { int z = 0; atomic_compare_exchange_strong(&g_save_poison, &z, e ? e : EIO); }
// 缺省弱实现只记日志（Mac）；iOS 前端写同名强实现弹原生提示。在游戏线程调，前端自己切主线程
__attribute__((weak)) void front_save_failed(int err) { LOG("front_save_failed(errno=%d)：前端未接（缺省弱实现）", err); }
// SW_FSLOG=<文件>：每次 ReadFile 记一行 TSV「启动后秒\t客体路径\t偏移\t长度\t客体缓冲地址」（启动读了哪些字节，离线对清单反查用）
static FILE *g_fslog; static char *g_fdpath[4096]; static pthread_mutex_t g_fslog_lock = PTHREAD_MUTEX_INITIALIZER;
static void fslog_init(void) { static int done; if (done) return; done = 1; const char *e = getenv("SW_FSLOG"); if (e && !(g_fslog = fopen(e, "w"))) die("SW_FSLOG 打不开 %s", e); }

static const char *envdir(const char *var,const char *rel,char *buf,size_t n) {
    (void)rel;(void)buf;(void)n; const char *v=getenv(var); if(!v||!*v)die("missing configured path %s",var); return v;
}

static int rm_tree(const char *p);
static Result mount_ex(const char *name, const char *dir, int ro, int txn) {
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmnt; i++)
        if (!strcmp(g_mnt[i].name, name)) { pthread_mutex_unlock(&g_lock); return RES_OK; }
    if (g_nmnt >= MAX_MOUNT) die("挂载表满");
    strlcpy(g_mnt[g_nmnt].name, name, sizeof g_mnt[0].name);
    g_mnt[g_nmnt].ro = ro; g_mnt[g_nmnt].overlay[0] = 0; g_mnt[g_nmnt].commit[0] = 0;
    if (!strcmp(name, "rom") && getenv("SW_ROMFS_OVERLAY"))
        strlcpy(g_mnt[g_nmnt].overlay, getenv("SW_ROMFS_OVERLAY"), sizeof g_mnt[0].overlay);
    if (txn) {
        char work[1100], neu[1100]; snprintf(work, sizeof work, "%s.work", dir); snprintf(neu, sizeof neu, "%s.new", dir);
        mkdir(dir, 0755);
        if (!access(work, F_OK) && rm_tree(work)) die("存档事务：清不掉上次残留的工作目录 %s", work);
        if (!access(neu, F_OK) && rm_tree(neu)) die("存档事务：清不掉上次残留的 %s", neu);
        if (xblr_clone_tree(dir, work, 0)) die("存档事务：clonefile %s → %s 失败 errno=%d", dir, work, errno);
        strlcpy(g_mnt[g_nmnt].commit, dir, sizeof g_mnt[0].commit);
        atomic_store(&g_save_poison, 0);                                // 工作目录刚从已提交版克隆，完好
        strlcpy(g_mnt[g_nmnt].dir, work, sizeof g_mnt[0].dir);
    } else strlcpy(g_mnt[g_nmnt].dir, dir, sizeof g_mnt[0].dir);
    g_nmnt++;
    pthread_mutex_unlock(&g_lock);
    if (!ro && !txn) mkdir(dir, 0755);
    LOG("fs 挂载 %s: → %s%s%s", name, txn ? g_mnt[g_nmnt - 1].dir : dir, ro ? "（只读）" : txn ? "（事务式存档：工作目录，Commit 原子换入）" : "", g_mnt[g_nmnt - 1].overlay[0] ? "（overlay 优先）" : "");
    return RES_OK;
}
static Result mount(const char *name, const char *dir, int ro) { return mount_ex(name, dir, ro, 0); }

// APFS 缺省不分大小写，Horizon 路径分大小写：每个组件按客体名精确查找。
// 普通宿主名字不变；只有名字在宿主上冲突时才另取物理名，xattr 保存客体名。
// clonefile 保留 xattr，故存档 Commit/重挂不用另设映射表；列举不能泄露物理别名。
#define FS_GUEST_NAME "org.xbl.sw.guest-name"
static int guest_name(const char *parent,const char *physical,char *out,size_t n) {
    return strlcpy(out,xblr_romfs_name(parent,physical),n)<n;
}
static int path_component(char *parent, size_t n, const char *name, int create) {
    DIR *d = opendir(parent); if (!d) return 0;
    struct dirent *e; char physical[0x301] = "", guest[0x301];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!guest_name(parent, e->d_name, guest, sizeof guest)) { closedir(d); return 0; }
        if (!strcmp(guest, name)) { strlcpy(physical, e->d_name, sizeof physical); break; }
    }
    closedir(d);
    if (!physical[0]) {
        if (!create) { errno = ENOENT; return 0; }
        strlcpy(physical, name, sizeof physical);
        char p[1280]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", parent, physical);
        if (!lstat(p, &st)) { // 精确客体名不存在，但 APFS 把物理名匹配到了另一项
            unsigned i = 0;
            do { snprintf(physical, sizeof physical, ".sw-case-%u", ++i);
                 snprintf(p, sizeof p, "%s/%s", parent, physical); } while (!lstat(p, &st));
        }
        if (errno != ENOENT) return 0;
    }
    size_t len = strlen(parent), k = strlen(physical);
    if (len + k + 2 > n) { errno = ENAMETOOLONG; return 0; }
    parent[len] = '/'; memcpy(parent + len + 1, physical, k + 1); return 1;
}
static int root_path(const char *root, const char *rest, char *out, size_t n) {
    strlcpy(out, root, n);
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
        size_t k = end - rest; char part[0x301];
        if (k >= sizeof part) { errno = ENAMETOOLONG; return 0; }
        memcpy(part, rest, k); part[k] = 0;
        if (!strcmp(part, "..")) { errno = ENOENT; return 0; }
        if (strcmp(part, ".") && !path_component(out, n, part, 0)) return 0;
        rest = end;
    }
    return 1;
}
static int rom_overlay_path(const mount_t *m,const char *rest,char *out,size_t n) {
    /* Configuration accepts one root. Permission/IO/path errors are failures,
     * never a reason to silently select stale base assets. */
    if(root_path(m->overlay,rest,out,n)&&!access(out,F_OK))return 1;
    if(errno!=ENOENT)return 0;
    const char *base_rest=xblr_romfs_rest(m->dir,rest);
    if(!base_rest){errno=ENOENT;return 0;}
    return root_path(m->dir,base_rest,out,n)&&!access(out,F_OK);
}
// create 只准最后一段尚不存在；中间目录仍需精确命中。
static int host_path_ex(const char *p, char *out, size_t n, int *ro, int create) {
    const char *c = strchr(p, ':');
    if (!c) { errno = ENOENT; return 0; }
    size_t ln = c - p;
    for (int i = 0; i < g_nmnt; i++) {
        if (strlen(g_mnt[i].name) != ln || strncmp(g_mnt[i].name, p, ln)) continue;
        if (ro) *ro = g_mnt[i].ro;
        const char *rest = c + 1;
        if (g_mnt[i].overlay[0] && !create) return rom_overlay_path(&g_mnt[i], rest, out, n);
        if(g_mnt[i].ro){rest=xblr_romfs_rest(g_mnt[i].dir,rest);if(!rest){errno=ENOENT;return 0;}}
        strlcpy(out, g_mnt[i].dir, n);
        while (*rest) {
            while (*rest == '/') rest++;
            if (!*rest) break;
            const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
            size_t k = end - rest; char part[0x301];
            if (k >= sizeof part) { errno = ENAMETOOLONG; return 0; }
            memcpy(part, rest, k); part[k] = 0;
            if (!strcmp(part, "..")) { errno = ENOENT; return 0; }
            const char *next = end; while (*next == '/') next++;
            if (strcmp(part, ".") && !path_component(out, n, part, create && !*next)) return 0;
            rest = end;
        }
        return 1;
    }
    errno = ENOENT; return 0;
}
static int host_path(const char *p, char *out, size_t n, int *ro) { return host_path_ex(p, out, n, ro, 0); }
static int set_guest_name(const char *hp,const char *path) {
    if(!xblr_fs_guest_name_matches(hp,path)) {errno=ENOTSUP;return -1;} return 0;
}
static Result err_res(const char *op, const char *path) {
    if (errno == ENOENT || errno == ENOTDIR) return RES_PATH_NOT_FOUND;
    if (errno == EEXIST) return RES_PATH_EXISTS;
    LOG("fs %s %s 失败 errno=%d", op, path, errno); return RES_FAIL;
}
// 名字解析与创建/改名/列举同锁，别名与 xattr 对客体一起发布。
#define HP_EX(path, create) PATH_LOCK(); char hp[1024]; int ro = 0; if (!host_path_ex(path, hp, sizeof hp, &ro, create)) return err_res("path", path)
#define HP(path) HP_EX(path, 0)
#define RW() if (ro) { LOG("fs 写只读挂载：%s", hp); return RES_FAIL; }

// ---------- 挂载 ----------
HLE_FORCE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", "fsp-srv；rom 直读宿主目录，缓存只给最小值", Result, h_fs_QueryRomCache, size_t *out) { *out = 0x1000; return RES_OK; }
HLE_FORCE("_ZN2nn2fs8MountRomEPKcPvm", "fsp-srv；rom → 宿主 RomFS 目录", Result, h_fs_MountRom, const char *name, void *cache, size_t sz) {
    (void)cache; (void)sz; char b[1024];
    Result r = mount(name, envdir("SW_ROMFS", "xbl/data/switch/3.3.94012/Update/RomFS", b, sizeof b), 1);
    if (r == RES_OK && getenv("SW_ROMFS_OVERLAY")) {
        PATH_LOCK();
        for (int i = 0; i < g_nmnt; i++) if (!strcmp(g_mnt[i].name, name)) {
            strlcpy(g_mnt[i].overlay, getenv("SW_ROMFS_OVERLAY"), sizeof g_mnt[i].overlay); break;
        }
    }
    return r;
}
HLE_FORCE("_ZN2nn2fs21MountTemporaryStorageEPKc", "fsp-srv；临时区 → 宿主目录", Result, h_fs_MountTemp, const char *name) {
    char b[1024]; return mount(name, envdir("SW_TEMP", "xbl/run/platform/temp", b, sizeof b), 0);
}
HLE_FORCE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", "fsp-srv；存档目录在挂载时建", Result, h_fs_EnsureSave, const void *uid) { (void)uid; return RES_OK; }
// 事务式存档当前的工作目录（游戏读写的那份；宿主自己的旁路文件写在这里才会随 Commit 进已提交版）。没挂载存档时返回 NULL
const char *sw_save_workdir(void) { for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].commit[0]) return g_mnt[i].dir; return NULL; }
// 事务式存档的已提交目录（盘上那份，上次 Commit 的版本）。没挂载存档时返回 NULL
const char *sw_save_commitdir(void) { for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].commit[0]) return g_mnt[i].commit; return NULL; }
// ---- 复用 iOS sw/host/hle_fs.c（上游 7ce77fd）：触摸产品设置缺省（小地图右上角进局自动开、物品名常显、说明条无手柄键位）与大字体兜底 ----
// 「大字体」（设置键 "Low Vision Mode"）兜底：设置文件里它是 1 就改回 0。它把界面换到原版的低视力布局档，我们的触摸界面没为那份档做过；在设置页打开后卡住。
// 设置页里这一项另由界面覆盖层藏掉；这里保证已经打开过的设备重启即恢复。与 settings_seed 同一个开关（iOS 上缺省开，Mac 宿主设 SW_TOUCH_DEFAULTS=1 才开）
static void low_vision_off(const char *save_dir) {
    const char *e = getenv("SW_TOUCH_DEFAULTS"); int on = e ? strcmp(e, "0") != 0 : 1; if (!on) return;
    char path[1200]; snprintf(path, sizeof path, "%s/Settings/Settings.json", save_dir); static char buf[65536]; FILE *f = fopen(path, "rb"); if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); if (n == sizeof buf - 1) return; buf[n] = 0;
    char *k = strstr(buf, "\"Low Vision Mode\""); if (!k) return; char *v = strchr(k + 17, ':'); if (!v) return; v++; while (*v == ' ' || *v == '\t') v++;
    if (*v != '1' || (v[1] >= '0' && v[1] <= '9')) return; *v = '0';
    char tmp[1210]; snprintf(tmp, sizeof tmp, "%s.tmp", path); f = fopen(tmp, "wb"); if (!f) return; int ok = fwrite(buf, 1, n, f) == n; ok = fclose(f) == 0 && ok;
    if (ok && !rename(tmp, path)) LOG("设置兜底：「大字体」（Low Vision Mode）是开着的，已改回关"); else remove(tmp);
}
static void settings_seed(const char *save_dir) {
    const char *e = getenv("SW_TOUCH_DEFAULTS"); int on = e ? strcmp(e, "0") != 0 : 1; if (!on) return;
    static const char *kv[][2] = {{"\"Item Name Display\"", "1"}, {"\"Item Tooltip Hotkey Appender\"", "0"},
                                  {"\"AutoMapMode\"", "2"}, {"\"AutoMap Open On Load\"", "1"}};
    char dir[1100], path[1200]; snprintf(dir, sizeof dir, "%s/Settings", save_dir); snprintf(path, sizeof path, "%s/Settings.json", dir);
    static char buf[65536], out[66000]; size_t n = 0; FILE *f = fopen(path, "rb");   // static：挂存档的游戏线程栈小（两块 64 KB 放栈上直接越栈，实测崩）；挂存档只在一个线程上做
    int first = f == NULL;   // 没有设置文件 = 首次安装
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); if (n == sizeof buf - 1) { LOG("设置缺省：%s 超过 64 KB，不动", path); return; } }
    buf[n] = 0;
    char *brace = strchr(buf, '{'); int had = brace != NULL; size_t o = 0; int added = 0;
    o += (size_t)snprintf(out + o, sizeof out - o, "{");
    for (size_t i = 0; i < sizeof kv / sizeof kv[0]; i++) if (!strstr(buf, kv[i][0])) { o += (size_t)snprintf(out + o, sizeof out - o, "\n    %s: %s,", kv[i][0], kv[i][1]); added++; }
    if (!added) return;
    if (had) { const char *rest = brace + 1; const char *q = rest; while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
        if (*q == '}') { out[o - 1] = 0; o--; }   // 原文件是空对象：去掉补的最后一个逗号
        o += (size_t)snprintf(out + o, sizeof out - o, "%s", rest); }
    else { out[o - 1] = 0; o--; o += (size_t)snprintf(out + o, sizeof out - o, "\n}\n"); }
    mkdir(dir, 0755); char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.seed", path);
    f = fopen(tmp, "wb"); if (!f || fwrite(out, 1, o, f) != o) { if (f) fclose(f); LOG("【红】设置缺省：写 %s 失败", tmp); return; }
    fclose(f); if (rename(tmp, path)) { LOG("【红】设置缺省：改名到 %s 失败", path); return; }
    LOG("设置缺省：%s 补了 %d 个键（显示物品 = 开关式 / 说明条热键 = 关 / 小地图放右、进局自动开%s；已有的键不动）", path, added, first ? " / 首次安装按系统语言写 Language" : "");
}
HLE_FORCE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", "fsp-srv；存档 → 宿主目录（单用户）", Result, h_fs_MountSave, const char *name, const void *uid) {
    (void)uid;
    // 不给缺省目录：静默落到一个没有角色的目录上会让「选角」测量失真（10-01 验证线与总指挥曾被误导）。跑局一律用 save_ref 的新副本
    const char *d = getenv("SW_SAVE");
    if (!d) die("未设 SW_SAVE：请指向标准存档 save_ref 的本局副本（见 run/platform/run.sh）");
    char dd[1024]; strlcpy(dd, d, sizeof dd); size_t L = strlen(dd); while (L > 1 && dd[L - 1] == '/') dd[--L] = 0;   // 去掉结尾斜杠（.work / .new 拼在同级）
    low_vision_off(dd);
    settings_seed(dd);
    return mount_ex(name, dd, 0, 1);
}
void (*g_sw_save_commit_hook)(const char *commit_dir);   // 存档提交成功后调（已提交目录）；iOS 的 iCloud 同步设它，宿主为空
static _Atomic uint64_t g_save_commits;   // 成功提交过几次（前端进后台时等「游戏这次存盘已提交」用）
uint64_t sw_fs_commit_count(void) { return atomic_load(&g_save_commits); }
HLE_FORCE("_ZN2nn2fs6CommitEPKc", "fsp-srv；事务式存档：工作目录整树 clone 后与已提交目录原子互换（renamex_np 2）", Result, h_fs_Commit, const char *name) {
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    int i = 0; while (i < g_nmnt && strcmp(g_mnt[i].name, name)) i++;
    if (i == g_nmnt || !g_mnt[i].commit[0]) { pthread_mutex_unlock(&g_lock); return RES_OK; }   // 非事务挂载：无可提交
    int perr = atomic_exchange(&g_save_poison, 0);
    if (perr) {                                                         // 本事务有写失败：拒绝提交、丢弃工作目录
        char torn[512] = ""; size_t tl = 0; DIR *d0 = opendir(g_mnt[i].dir); struct dirent *e0;   // 记下被截成 0 字节的文件（若照常提交就会进存档的残缺）
        while (d0 && (e0 = readdir(d0))) {
            if (e0->d_name[0] == '.') continue;
            char sub[1300]; snprintf(sub, sizeof sub, "%s/%s", g_mnt[i].dir, e0->d_name); DIR *d1 = opendir(sub); struct dirent *e1;
            while (d1 && (e1 = readdir(d1))) { char f[1600]; struct stat st; snprintf(f, sizeof f, "%s/%s", sub, e1->d_name);
                if (e1->d_name[0] != '.' && !stat(f, &st) && S_ISREG(st.st_mode) && !st.st_size && tl + strlen(e0->d_name) + strlen(e1->d_name) + 3 < sizeof torn)
                    tl += snprintf(torn + tl, sizeof torn - tl, " %s/%s", e0->d_name, e1->d_name); }
            if (d1) closedir(d1);
        }
        if (d0) closedir(d0);
        int nopen = 0; for (int k = 0; k < 65536; k++) nopen += g_fsave[k];   // 仍开着的存档文件：重新克隆后这些句柄指向被删的旧工作目录
        if (rm_tree(g_mnt[i].dir) || xblr_clone_tree(g_mnt[i].commit, g_mnt[i].dir, 0)) die("存档事务：丢弃工作目录 %s 后重新克隆失败 errno=%d", g_mnt[i].dir, errno);
        pthread_mutex_unlock(&g_lock);
        LOG("【红】存档提交拒绝：本事务有存档写失败 errno=%d（工作目录里 0 字节文件：%s）；已提交版本不变，工作目录已从已提交版重新克隆%s", perr, tl ? torn + 1 : "无", nopen ? "；【红】仍有存档文件开着" : "");
        front_save_failed(perr);
        return RES_FAIL;
    }
    char neu[1100]; snprintf(neu, sizeof neu, "%s.new", g_mnt[i].commit);
    Result r = RES_OK;
    if (!access(neu, F_OK)) rm_tree(neu);
    if (xblr_clone_tree(g_mnt[i].dir, neu, 0)) { LOG("【红】存档提交：clonefile %s 失败 errno=%d（已提交版本不变）", neu, errno); r = RES_FAIL; }
    else if (xblr_exchange_save(neu, g_mnt[i].commit, 2)) { LOG("【红】存档提交：原子互换失败 errno=%d（已提交版本不变）", errno); rm_tree(neu); r = RES_FAIL; }
    else rm_tree(neu);                                                  // 换下来的旧版
    static int n; if (r == RES_OK) { LOG("存档提交 #%d：%s 原子换入 %s（此前存档写 %d 次）", ++n, g_mnt[i].dir, g_mnt[i].commit, atomic_load(&g_save_writes)); atomic_fetch_add(&g_save_commits, 1);
        if (g_sw_save_commit_hook) g_sw_save_commit_hook(g_mnt[i].commit); }   // iCloud 同步（cloud_ios.m）：持着锁调，那边只做 clonefile 快照
    pthread_mutex_unlock(&g_lock);
    return r;
}
HLE_FORCE("_ZN2nn2fs7UnmountEPKc", "fsp-srv；挂载表常驻，卸载为空操作", void, h_fs_Unmount, const char *name) { (void)name; }

// ---------- 文件 ----------
// 剔零档案（10-03 场景线）：资源包里被填零的大段（过场视频、旧版音乐）不随包分发——档案文件里把这些零段剔掉，旁边放一张区段表
// 「<档案名>.holes」（文本：首行 `size <原始字节数>`，其后每行 `<零段起点> <零段长度>`，按起点升序、互不重叠）。
// 宿主读这类档案时按表换算偏移：落在零段的字节返回 0，文件大小报原始值；游戏与启动预取看到的仍是原始布局，索引一个字不用改。
// 读入口只有两处：本文件的 ReadFile 与 hle_tact.c 的启动预取，都走 sw_data_pread。没有 .holes 的文件走原路径，零额外开销。
typedef struct { int64_t size; int n; int64_t *log, *len, *phys; char name[40]; } holemap;   // 保留段：原始起点 / 长度 / 在剔零文件里的起点
static _Atomic uint64_t g_hole_reads;   // 读到零段的次数：正常应为 0（零段是砍掉的过场视频 / 旧版音乐，游戏不该来读）；非 0 = 砍错了
static holemap *g_hm[4096];
static holemap *hm_load(const char *hostpath) {
    char hp2[1100]; if (snprintf(hp2, sizeof hp2, "%s.holes", hostpath) >= (int)sizeof hp2) return NULL;
    FILE *f = fopen(hp2, "r"); if (!f) return NULL;
    long long size = -1, o, l; if (fscanf(f, "size %lld", &size) != 1 || size < 0) die("区段表格式不对：%s", hp2);
    holemap *m = calloc(1, sizeof *m); int cap = 64; m->size = size; m->log = malloc(cap * sizeof *m->log); m->len = malloc(cap * sizeof *m->len); m->phys = malloc(cap * sizeof *m->phys);
    int64_t pos = 0, phys = 0;
    while (fscanf(f, "%lld %lld", &o, &l) == 2) {
        if (o < pos || l <= 0 || o > size || l > size-o) die("区段表零段越界或乱序：%s（%lld %lld）", hp2, o, l);
        if (o > pos) { if (m->n == cap) { cap *= 2; m->log = realloc(m->log, cap * sizeof *m->log); m->len = realloc(m->len, cap * sizeof *m->len); m->phys = realloc(m->phys, cap * sizeof *m->phys); }
            m->log[m->n] = pos; m->len[m->n] = o - pos; m->phys[m->n++] = phys; phys += o - pos; }
        pos = o + l;
    }
    fclose(f);
    if (pos < size) { if (m->n == cap) { cap++; m->log = realloc(m->log, cap * sizeof *m->log); m->len = realloc(m->len, cap * sizeof *m->len); m->phys = realloc(m->phys, cap * sizeof *m->phys); }
        m->log[m->n] = pos; m->len[m->n] = size - pos; m->phys[m->n++] = phys; phys += size - pos; }
    struct stat st; if (stat(hostpath, &st) || st.st_size != phys) die("剔零档案长度与区段表不符：%s 实际 %lld，按表应为 %lld", hostpath, (long long)st.st_size, (long long)phys);
    const char *bn = strrchr(hostpath, '/'); strlcpy(m->name, bn ? bn + 1 : hostpath, sizeof m->name);
    { static _Atomic int said[64]; unsigned h = 0; for (const char *c = m->name; *c; c++) h = h * 31 + (unsigned char)*c;   // 每个档案只在首次登记时打一行
      if (!atomic_exchange(&said[h % 64], 1)) LOG("剔零档案 %s：原始 %lld 字节，实存 %lld 字节，保留段 %d 个", m->name, (long long)size, (long long)phys, m->n); }
    return m;
}
void sw_data_note_fd(int fd, const char *hostpath) {   // 打开资源档案后调：有区段表就登记
    if (fd < 0 || fd >= 4096) return;
    g_hm[fd] = hm_load(hostpath);
}
int64_t sw_data_logical_size(const char *hostpath, int64_t phys_size) {   // 列目录 / 取大小用：有区段表报原始大小
    char hp2[1100]; if (snprintf(hp2, sizeof hp2, "%s.holes", hostpath) >= (int)sizeof hp2) return phys_size;
    FILE *f = fopen(hp2, "r"); if (!f) return phys_size; long long size = -1; int ok = fscanf(f, "size %lld", &size) == 1; fclose(f); return ok && size >= 0 ? size : phys_size;
}
ssize_t sw_data_pread(int fd, void *buf, size_t n, off_t off) {   // 语义同 pread（可能短读）；剔零档案按区段表换算
    holemap *m = fd >= 0 && fd < 4096 ? g_hm[fd] : NULL;
    if (!m) return pread(fd, buf, n, off);
    if (off >= m->size) return 0;
    if ((int64_t)n > m->size - off) n = (size_t)(m->size - off);
    int lo = 0, hi = m->n;   // 第一个 结束点 > off 的保留段
    while (lo < hi) { int mid = (lo + hi) / 2; if (m->log[mid] + m->len[mid] <= off) lo = mid + 1; else hi = mid; }
    size_t done = 0;
    while (done < n) {
        int64_t cur = off + (int64_t)done;
        if (lo < m->n && cur >= m->log[lo]) {   // 在保留段里：直接读进目标缓冲
            int64_t in = cur - m->log[lo]; size_t want = n - done; if ((int64_t)want > m->len[lo] - in) want = (size_t)(m->len[lo] - in);
            size_t got = 0; while (got < want) { ssize_t r = pread(fd, (uint8_t *)buf + done + got, want - got, m->phys[lo] + in + (int64_t)got); if (r < 0) { if (errno == EINTR) continue; return -1; } if (r == 0) return (ssize_t)(done + got); got += (size_t)r; }
            done += want; if (in + (int64_t)want == m->len[lo]) lo++;
        } else {                               // 在零段里：填 0 到下一保留段起点
            int64_t end = lo < m->n ? m->log[lo] : m->size; size_t z = n - done; if ((int64_t)z > end - cur) z = (size_t)(end - cur);
            memset((uint8_t *)buf + done, 0, z); done += z;
            // 游戏按 256 KB 对齐的整块读：要的是零段旁边的保留文件时，整块会顺带压到零段边缘（无害，保留文件的字节照给）。
            // 一次读整个落在零段里才是真在读被砍的内容 → 报红（前 20 次逐条，之后每 1000 次一行）；跨边界的只说明前 3 次
            if (z == n) { uint64_t hr = atomic_fetch_add(&g_hole_reads, 1);
                if (hr < 20 || hr % 1000 == 0) LOG("【红】读到剔零区段 %s 偏移 %lld 长 %zu（第 %llu 次；整块都在被砍掉的过场视频 / 旧版音乐里，说明砍错了或有新的读者）", m->name, (long long)off, n, (unsigned long long)hr + 1);
            } else { static _Atomic uint64_t edge; if (atomic_fetch_add(&edge, 1) < 3) LOG("整块读压到剔零区段边缘（无害）：%s 读 %lld 长 %zu，其中 %zu 字节在零段", m->name, (long long)off, n, z); }
        }
    }
    return (ssize_t)done;
}
HLE_FORCE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", "fsp-srv → open(2)", Result, h_fs_OpenFile, uint64_t *out, const char *path, int mode) {
    HP(path);
    if (mode & (MODE_WRITE | MODE_APPEND)) RW();
    int fd = open(hp, (mode & (MODE_WRITE | MODE_APPEND)) ? ((mode & MODE_READ) ? O_RDWR : O_WRONLY) : O_RDONLY);
    if (fd < 0) return err_res("open", hp);
    struct stat st; fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) { close(fd); return RES_PATH_NOT_FOUND; }
    if (fd >= (int)sizeof g_fmode) die("fd 超出表");
    g_fmode[fd] = (uint8_t)mode; *out = (uint64_t)fd;
    if(fd>=4096) {close(fd);errno=EMFILE;return RES_FAIL;} g_hm[fd] = NULL; if (!(mode & (MODE_WRITE | MODE_APPEND))) sw_data_note_fd(fd, hp);
    { const char *c = strchr(path, ':'); size_t ln = c ? (size_t)(c - path) : 0; g_fsave[fd] = 0;
      for (int i = 0; c && i < g_nmnt; i++) if (strlen(g_mnt[i].name) == ln && !strncmp(g_mnt[i].name, path, ln)) g_fsave[fd] = g_mnt[i].commit[0] != 0; }
    fslog_init();
    if (g_fslog && fd < 4096) { pthread_mutex_lock(&g_fslog_lock); free(g_fdpath[fd]); g_fdpath[fd] = strdup(path); pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) LOG("fs open [%.2fs] %s mode=%d → fd %d", (now_ns() - g_t0_ns) / 1e9, path, mode, fd);
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", "fsp-srv → pread(2)", Result, h_fs_ReadFile, size_t *out, uint64_t h, int64_t off, void *buf, size_t sz) {
    size_t got = 0; uint64_t t0 = g_flag.bootprof ? now_ns() : 0;
    sw_guest_write_prepare(buf, sz);   // 目标可能是图形层写保护的贴图池页：内核写只读页返回 EFAULT 而不缺页，先解保护并标脏
    while (got < sz) {
        ssize_t r = sw_data_pread((int)h, (uint8_t *)buf + got, sz - got, off + got);
        if (r < 0) { if (errno == EINTR) continue; return err_res("pread", "fd"); }
        if (r == 0) break;
        got += r;
    }
    *out = got;
    if (g_flag.bootprof) sw_prof_add(PROF_FS_READ, now_ns() - t0, got);
    if (g_fslog) { pthread_mutex_lock(&g_fslog_lock);
        static int hash_on = -1; if (hash_on < 0) hash_on = getenv("SW_FSLOG_HASH") != NULL;   // SW_FSLOG_HASH=1：末尾加一列读到内容的 FNV-1a 64（对比两份资源包读到的字节是否一致）
        uint64_t fh = 0xcbf29ce484222325ull; if (hash_on) for (size_t i = 0; i < got; i++) fh = (fh ^ ((uint8_t *)buf)[i]) * 0x100000001b3ull;
        if (hash_on) fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\t%016llx\n", (now_ns() - g_t0_ns) / 1e9, (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf, (unsigned long long)fh);
        else fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\n", (now_ns() - g_t0_ns) / 1e9, (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf);
        pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) {   // 读取计量：每累计 64 MiB 或每 1000 次打一行
        static uint64_t n, bytes, next_b = 64 << 20; static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&m); n++; bytes += got;
        if (bytes >= next_b || n % 1000 == 0) { LOG("fs 读 累计 %llu 次 %.1f MiB（本次 fd %d 偏移 %#llx 长 %#zx）", n, bytes / 1048576.0, (int)h, off, got); if (bytes >= next_b) next_b += 64 << 20; }
        pthread_mutex_unlock(&m);
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "fsp-srv → pwrite(2)", Result, h_fs_WriteFile, uint64_t h, int64_t off, const void *buf, size_t sz, const void *opt) {
    (void)opt; size_t put = 0;
    if (g_fsave[(int)h & 0xffff]) {                                     // 存档写注入点（确定性回归用；只数存档挂载上的写）
        static int cfg = -1, kill_n, fail_n, fail_e;
        if (cfg < 0) { const char *k = getenv("SW_TEST_KILL_AT"), *f = getenv("SW_TEST_FS_FAIL");
            kill_n = k && !strncmp(k, "write:", 6) ? atoi(k + 6) : 0;
            if (f && !strncmp(f, "write:", 6)) { fail_n = atoi(f + 6); const char *c = strchr(f + 6, ':'); fail_e = c ? atoi(c + 1) : ENOSPC; }
            cfg = 1; }
        int n = atomic_fetch_add(&g_save_writes, 1) + 1;
        if (kill_n && n == kill_n) { LOG("SW_TEST_KILL_AT：存档第 %d 次写，直接 _exit（模拟写到一半被杀）", n); _exit(9); }
        if (fail_n && n == fail_n) { LOG("SW_TEST_FS_FAIL：存档第 %d 次写返回失败 errno=%d", n, fail_e); errno = fail_e; save_poison(fail_e); return err_res("pwrite（注入）", "fd"); }
    }
    while (put < sz) {
        ssize_t r = pwrite((int)h, (const uint8_t *)buf + put, sz - put, off + put);
        if (r < 0) { if (errno == EINTR) continue; if (g_fsave[(int)h & 0xffff]) save_poison(errno); return err_res("pwrite", "fd"); }
        if (r == 0) { errno = ENOSPC; if (g_fsave[(int)h & 0xffff]) save_poison(ENOSPC); return err_res("pwrite（写入 0 字节）", "fd"); }   // 原先在此死循环
        put += r;
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", "fsp-srv → fstat(2)", Result, h_fs_GetFileSize, int64_t *out, uint64_t h) {
    struct stat st; if (fstat((int)h, &st)) return err_res("fstat", "fd");
    *out = (int)h < 4096 && g_hm[(int)h] ? g_hm[(int)h]->size : st.st_size; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", "fsp-srv → ftruncate(2)", Result, h_fs_SetFileSize, uint64_t h, int64_t sz) {
    if (!ftruncate((int)h, sz)) return RES_OK;
    if (g_fsave[(int)h & 0xffff]) save_poison(errno);
    return err_res("ftruncate", "fd");
}
HLE_FORCE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", "fsp-srv；宿主 write 已进页缓存，刷新为空操作", Result, h_fs_FlushFile, uint64_t h) { return fsync((int)h) ? RES_FAIL : RES_OK; }
HLE_FORCE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", "fsp-srv → close(2)", void, h_fs_CloseFile, uint64_t h) { g_fmode[(int)h & 0xffff] = 0; g_fsave[(int)h & 0xffff] = 0;
    if ((int)h >= 0 && (int)h < 4096 && g_hm[(int)h]) { holemap *m = g_hm[(int)h]; g_hm[(int)h] = NULL; free(m->log); free(m->len); free(m->phys); free(m); }
    close((int)h); }
HLE_FORCE("_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", "fsp-srv；打开方式由宿主记录", int, h_fs_GetFileOpenMode, uint64_t h) { return g_fmode[(int)h & 0xffff]; }

// ---------- 路径操作 ----------
HLE_FORCE("_ZN2nn2fs10CreateFileEPKcl", "fsp-srv → open(O_CREAT|O_EXCL)+ftruncate", Result, h_fs_CreateFile, const char *path, int64_t sz) {
    HP_EX(path, 1); RW();
    int fd = open(hp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return err_res("create", hp);
    int rc = ftruncate(fd, sz); int e = errno; close(fd);
    if (!rc && set_guest_name(hp, path)) { rc = -1; e = errno; }
    if (rc) unlink(hp);
    errno = e; return rc ? err_res("create", hp) : RES_OK;
}
HLE_FORCE("_ZN2nn2fs10DeleteFileEPKc", "fsp-srv → unlink(2)", Result, h_fs_DeleteFile, const char *path) { HP(path); RW(); return unlink(hp) ? err_res("unlink", hp) : RES_OK; }
HLE_FORCE("_ZN2nn2fs15CreateDirectoryEPKc", "fsp-srv → mkdir(2)", Result, h_fs_CreateDirectory, const char *path) { HP_EX(path, 1); RW();
    if (mkdir(hp, 0755)) return err_res("mkdir", hp);
    if (set_guest_name(hp, path)) { int e = errno; rmdir(hp); errno = e; return err_res("mkdir name", hp); }
    return RES_OK; }
static int rm_tree(const char *p) {
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; char c[1024]; int rc = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        rc |= e->d_type == DT_DIR ? rm_tree(c) : unlink(c);
    }
    closedir(d);
    return rc | rmdir(p);
}
HLE_FORCE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", "fsp-srv → 递归删除（只在可写挂载内）", Result, h_fs_DeleteDirRec, const char *path) {
    HP(path); RW(); return rm_tree(hp) ? err_res("rmtree", hp) : RES_OK;
}
HLE_FORCE("_ZN2nn2fs10RenameFileEPKcS2_", "fsp-srv → rename(2)", Result, h_fs_RenameFile, const char *from, const char *to) {
    HP(from); RW(); char hp2[1024];
    if (!host_path_ex(to, hp2, sizeof hp2, NULL, 1)) return err_res("rename path", to);
    if (!access(hp2, F_OK)) return RES_PATH_EXISTS;
    if (rename(hp, hp2)) return err_res("rename", hp);
    if (set_guest_name(hp2, to)) { int e = errno; if (rename(hp2, hp)) die("fs 改名元数据失败且回退失败 errno=%d", errno); errno = e; return err_res("rename name", hp2); }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", "fsp-srv → stat(2)", Result, h_fs_GetEntryType, int32_t *out, const char *path) {
    HP(path); struct stat st;
    if (stat(hp, &st)) return err_res("stat", hp);
    *out = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; return RES_OK;
}

// ---------- 目录 ----------
typedef struct { DIR *d; char path[1024]; DIR *od; char opath[1024]; DirectoryEntry *snap; int64_t nsnap, psnap; int mode; } dirh_t;
static void dir_snapshot(dirh_t *h, const mount_t *m, const char *rest) {
    char *roots = strdup(m->overlay);
    char (*names)[0x301] = calloc(2048, sizeof *names);
    const char **all = calloc(65, sizeof *all);
    char *parent = malloc(1024), *guest = malloc(0x301), *rel = malloc(2048), *resolved = malloc(1024);
    if (!roots || !names || !all || !parent || !guest || !rel || !resolved) die("rom overlay 目录快照分配失败");
    int nn = 0, nr = 0;
    all[nr++] = m->dir;
    char *save = NULL;
    for (char *root = strtok_r(roots, ":", &save); root && nr < 65; root = strtok_r(NULL, ":", &save)) all[nr++] = root;
    for (int ri = nr - 1; ri >= 0; ri--) {
        const char *entry_rest=ri==0?xblr_romfs_rest(m->dir,rest):rest;
        if (!entry_rest || !root_path(all[ri], entry_rest, parent, 1024)) continue;
        DIR *x = opendir(parent); if (!x) continue;
        struct dirent *e;
        while ((e = readdir(x))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            { size_t dl = strlen(e->d_name); if (dl > 6 && !strcmp(e->d_name + dl - 6, ".holes")) continue; }   // 区段表不给游戏看
            if (!guest_name(parent, e->d_name, guest, 0x301)) continue;
            int dup = 0; for (int j = 0; j < nn; j++) if (!strcmp(names[j], guest)) { dup = 1; break; }
            if (!dup && nn < 2048) strlcpy(names[nn++], guest, sizeof names[0]);
        }
        closedir(x);
    }
    h->snap = calloc(nn, sizeof *h->snap); h->nsnap = nn;
    if (nn && !h->snap) die("rom overlay 目录快照分配失败");
    for (int j = 0; j < nn; j++) {
        strlcpy(h->snap[j].name, names[j], sizeof h->snap[j].name);
        snprintf(rel, 2048, "%s/%s", rest, names[j]);
        if (!rom_overlay_path(m, rel, resolved, 1024)) continue;
        struct stat st;
        if (!stat(resolved, &st)) { h->snap[j].type = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; h->snap[j].size = S_ISDIR(st.st_mode) ? 0 : sw_data_logical_size(resolved, st.st_size); }
    }
    free(resolved); free(rel); free(guest); free(parent); free(all); free(names); free(roots);
}
HLE_FORCE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", "fsp-srv → opendir(3)", Result, h_fs_OpenDirectory, uint64_t *out, const char *path, int mode) {
    HP(path); DIR *d = opendir(hp);
    if (!d) return err_res("opendir", hp);
    dirh_t *h = calloc(1, sizeof *h);if(!h){closedir(d);return RES_FAIL;} h->d = d; strlcpy(h->path, hp, sizeof h->path); h->mode = mode;
    if(ro&&xblr_romfs_is_root(hp,path)) {
        h->snap=calloc(1,sizeof *h->snap);if(!h->snap){closedir(d);free(h);return RES_FAIL;}
        strcpy(h->snap[0].name,"data");h->snap[0].type=ENT_DIR;h->nsnap=1;*out=(uintptr_t)h;return RES_OK;
    }
    const char *c = strchr(path, ':');
    if (c) for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].overlay[0] && strlen(g_mnt[i].name) == (size_t)(c - path) && !strncmp(g_mnt[i].name, path, (size_t)(c - path))) {
        dir_snapshot(h, &g_mnt[i], c + 1); break;
    }
    *out = (uint64_t)h; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", "fsp-srv → readdir(3)", Result, h_fs_ReadDirectory, int64_t *count, DirectoryEntry *ents, uint64_t hh, int64_t max) {
    PATH_LOCK(); dirh_t *h = (dirh_t *)hh; int64_t n = 0; struct dirent *e;
    if (h->snap) { while (n < max && h->psnap < h->nsnap) { DirectoryEntry *src = &h->snap[h->psnap++]; if (!(h->mode & (src->type == ENT_DIR ? DIRMODE_DIR : DIRMODE_FILE))) continue; ents[n++] = *src; } *count = n; return RES_OK; }
    while (n < max && (e = readdir(h->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        { size_t dl = strlen(e->d_name); if (dl > 6 && !strcmp(e->d_name + dl - 6, ".holes")) continue; }   // 区段表不给游戏看
        char c[1280]; snprintf(c, sizeof c, "%s/%s", h->path, e->d_name);
        struct stat st; if (stat(c, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (!(h->mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        DirectoryEntry *o = &ents[n++]; memset(o, 0, sizeof *o);
        if (!guest_name(h->path, e->d_name, o->name, sizeof o->name)) { *count = n - 1; return err_res("directory name", c); }
        o->type = isdir ? ENT_DIR : ENT_FILE; o->size = isdir ? 0 : sw_data_logical_size(c, st.st_size);
    }
    *count = n; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", "fsp-srv → closedir(3)", void, h_fs_CloseDirectory, uint64_t hh) {
    dirh_t *h = (dirh_t *)hh; closedir(h->d); if (h->od) closedir(h->od); free(h->snap); free(h);
}
