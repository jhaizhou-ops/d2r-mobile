// nn::fs：真机经 fsp-srv 服务；宿主直接接 Mac 文件系统。挂载名 → 宿主目录：
//   rom   → $DM_ROMFS（缺省 $DM_ROOT/data/switch/3.3.94012/Update/RomFS，只读；游戏自己解析其中的存储块）
//   存档  → $DM_SAVE（必须设置，指向标准存档 save_ref 的本局副本；未设报红中止）
//   临时  → $DM_TEMP（缺省 $DM_ROOT/run/platform/temp）
// 路径形如 "rom:/data/x"。FileHandle / DirectoryHandle 是按值传的 8 字节，这里存宿主 fd / DIR*。
// 结构布局按公开 Horizon ABI：DirectoryEntry = {char name[0x301], pad[3], s8 type, pad[3], s64 size}（0x310 字节）。
// nn::fs: on the real device it goes through the fsp-srv service; the host connects directly to the Mac file system. Mount name → host directory:
//   rom   → $DM_ROMFS (default $DM_ROOT/data/switch/3.3.94012/Update/RomFS, read-only; the game parses the storage blocks in it itself)
//   save  → $DM_SAVE (must be set, pointing to this run's copy of the standard save save_ref; if unset, report a failure and abort)
//   temp  → $DM_TEMP (default $DM_ROOT/run/platform/temp)
// Paths look like "rom:/data/x". FileHandle / DirectoryHandle are 8 bytes passed by value, here they store the host fd / DIR*.
// Struct layout follows the public Horizon ABI: DirectoryEntry = {char name[0x301], pad[3], s8 type, pad[3], s64 size} (0x310 bytes).
#include "hle.h"
#include "front.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>

#define RES_PATH_NOT_FOUND 0x202u      // fs 模块 2，描述 1 | fs module 2, description 1
#define RES_PATH_EXISTS    0x402u      // fs 模块 2，描述 2 | fs module 2, description 2
enum { MODE_READ = 1, MODE_WRITE = 2, MODE_APPEND = 4 };
enum { ENT_DIR = 0, ENT_FILE = 1 };
enum { DIRMODE_DIR = 1, DIRMODE_FILE = 2 };

typedef struct { char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "DirectoryEntry 布局");

#define MAX_MOUNT 16
// 存档事务（HLE 审计第 1–3 条）：真机存档提交前可整体回滚，崩溃 / 被杀后再挂载只见上次 Commit 的版本。
// 宿主：挂载时把已提交目录 commit（= $DM_SAVE）整树 clonefile 成工作目录 <commit>.work（APFS 写时复制，近乎零成本），挂载指向工作目录，
// 游戏的读写都在工作目录；Commit = clonefile(work → <commit>.new) 后 renamex_np(RENAME_SWAP) 与 commit 原子互换，再删掉换下来的旧版。
// 任何时刻被杀，commit 要么是旧版要么是新版，不会半截 / 新旧混合；下次挂载丢弃残留的 .work / .new。
// Save transactions (HLE audit items 1–3): on the real device a save can be rolled back as a whole before commit, and after a crash / kill a remount sees only the version from the last Commit.
// Host: at mount, the committed directory commit (= $DM_SAVE) is clonefile'd as a whole tree into the working directory <commit>.work (APFS copy-on-write, nearly zero cost), and the mount points at the working directory,
// all game reads and writes happen in the working directory; Commit = clonefile(work → <commit>.new), then renamex_np(RENAME_SWAP) swaps it atomically with commit, then the swapped-out old version is deleted.
// Killed at any moment, commit is either the old version or the new version, never half-written / a mix of old and new; the next mount discards leftover .work / .new.
typedef struct { char name[32]; char dir[1024]; char overlay[4096]; char commit[1024]; int ro; } mount_t;
static mount_t g_mnt[MAX_MOUNT];   // commit 非空 = 事务式存档挂载 | commit non-empty = transactional save mount
static int g_nmnt;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static void path_unlock(pthread_mutex_t **m) { pthread_mutex_unlock(*m); }
#define PATH_LOCK() pthread_mutex_t *path_guard __attribute__((cleanup(path_unlock))) = &g_path_lock; pthread_mutex_lock(path_guard)
static uint8_t g_fmode[65536];              // fd → 打开方式 | fd → open mode
// 角色档写完的先后（个人仓库翻页 / iCloud 并回判「这份角色档是不是某一刻之后序列化的」用）：写方式打开的 *.d2s 关闭、或改名成 *.d2s 时取一个全局单调序号。
// 只当「写完」的先后用，不当「序列化」的先后用——角色档由另一条线程写，序列化在前、写盘在后中间可以夹着服务端拍上的动作（dm_vault.c 文件头）
// Order in which character files finished writing (used by personal stash page turns / iCloud merge to tell whether a character file was serialized after a given moment): a global monotonic number is taken when a *.d2s opened for writing is closed, or when a file is renamed to *.d2s.
// Use it only for the order of finished writes, not the order of serialization: the character file is written by another thread, and actions on the server tick can fall between serialization and the write (see the header of dm_vault.c)
static uint8_t g_fd2s[65536]; static _Atomic uint64_t g_seq, g_d2s_wseq;
uint64_t dm_seq_next(void) { return atomic_fetch_add(&g_seq, 1) + 1; }
uint64_t dm_fs_d2s_wseq(void) { return atomic_load(&g_d2s_wseq); }
static int is_d2s(const char *p) { size_t n = strlen(p); return n > 4 && !strcmp(p + n - 4, ".d2s"); }
static uint8_t g_fsave[65536];              // fd → 是否在事务式存档挂载上（存档写注入点只数这些） | fd → whether it is on a transactional save mount (the save write injection point only counts these)
static _Atomic int g_save_writes;           // 存档挂载上的写次数（注入点与提交日志共用） | number of writes on the save mount (shared by the injection point and the commit log)
static _Atomic uint64_t g_txn_first_ns;     // 本事务第一次存档写的时刻（提交日志报耗时用；提交时清 0） | time of this transaction's first save write (used by the commit log to report timing; cleared to 0 at commit)
// 写失败按事务拒绝：真机存档空间建档时预留，覆盖写不会失败，游戏也不看 WriteFile / SetFileSize 的返回值，失败后照常 Commit
// （注入第 1 次写失败后提交进去的 joe.d2s 为 0 字节＝角色丢失）。宿主写失败（盘满等，iOS 上真实可能）时：
// 从上次 Commit 到这次 Commit 之间有任一存档写 / 定长失败，这次 Commit 拒绝——已提交版本不变，工作目录丢弃、从已提交版重新克隆，
// 并通知前端（front_save_failed）；下一个事务（游戏下次整档重写）照常尝试，成功就提交。g_save_poison = 本事务首个失败的 errno（0 = 无）。
// Write failures are refused per transaction: on the real device save space is reserved when the save is created, overwrites cannot fail, and the game does not check the return values of WriteFile / SetFileSize either, it Commits as usual after a failure
// (after injecting a failure on the 1st write, the committed joe.d2s is 0 bytes = character lost). When a host write fails (disk full etc., really possible on iOS):
// if any save write / set-size failed between the last Commit and this Commit, this Commit is refused -- the committed version is unchanged, the working directory is discarded and re-cloned from the committed version,
// and the front end is notified (front_save_failed); the next transaction (the game's next full save rewrite) is attempted as usual and committed if it succeeds. g_save_poison = errno of the first failure in this transaction (0 = none).
static _Atomic int g_save_poison;
static void save_poison(int e) { int z = 0; atomic_compare_exchange_strong(&g_save_poison, &z, e ? e : EIO); }
void dm_fs_save_poison(int e) { save_poison(e); }   // 提交前钩子用：旁路文件（编年史 / 百宝囊）没写成也算本事务写失败——照常提交的话，物品已不在角色档里、旁路文件里又没有 | for the pre-commit hook: a side file (Chronicle / pouch) that failed to write also counts as a write failure of this transaction -- if the commit went ahead as usual, the items would no longer be in the character save and would not be in the side file either
// 缺省弱实现只记日志（Mac）；iOS 前端写同名强实现弹原生提示。在游戏线程调，前端自己切主线程
// The default weak implementation only logs (Mac); the iOS front end provides a strong implementation of the same name that shows a native alert. Called on the game thread, the front end switches to the main thread itself
__attribute__((weak)) void front_save_failed(int err) { LOG("front_save_failed(errno=%d)：前端未接（缺省弱实现）", err); }
// DM_FSLOG=<文件>：每次 ReadFile 记一行 TSV「启动后秒\t客体路径\t偏移\t长度\t客体缓冲地址」（启动读了哪些字节，离线对清单反查用）
// DM_FSLOG=<file>: each ReadFile logs one TSV line "seconds since boot\tguest path\toffset\tlength\tguest buffer address" (which bytes boot read, for offline reverse lookup against the manifest)
static FILE *g_fslog; static char *g_fdpath[4096]; static pthread_mutex_t g_fslog_lock = PTHREAD_MUTEX_INITIALIZER;
static void fslog_init(void) { static int done; if (done) return; done = 1; const char *e = getenv("DM_FSLOG"); if (e && !(g_fslog = fopen(e, "w"))) die("DM_FSLOG 打不开 %s", e); }

static const char *envdir(const char *var, const char *rel, char *buf, size_t n) {
    const char *v = getenv(var);
    if (v) return v;
    snprintf(buf, n, "%s%s/%s", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", rel); return buf;
}

// 测试注入 DM_TEST_FS_FAILAT=<点>[:<次数>][,…]：存档事务里写失败收场的几步（mountclone 挂载时克隆工作目录、reclone 提交被拒后重新克隆、
// guestname 改名后写客体名、renameback 改名回退）前 <次数> 次（缺省 1）按 ENOSPC 失败，验「不退出、已提交版本不变、下次提交照常」（tests/t_fs.c）
// Test injection DM_TEST_FS_FAILAT=<point>[:<count>][,...]: the steps of a save transaction that end in a failed write (mountclone = clone the work directory at mount, reclone = reclone after a refused commit,
// guestname = write the guest name after a rename, renameback = rename rollback) fail with ENOSPC for the first <count> times (default 1), to check "no exit, committed version unchanged, next commit works" (tests/t_fs.c)
static int dm_fs_test_fail(const char *pt) {
    static char spec[256]; static int cnt[8]; const char *e = getenv("DM_TEST_FS_FAILAT"); if (!e || !e[0]) return 0;
    if (strcmp(spec, e)) { strlcpy(spec, e, sizeof spec); memset(cnt, 0, sizeof cnt); }   // 换了一组注入就从头数 | A new set of injections starts counting from zero
    size_t L = strlen(pt); int k = 0;
    for (const char *q = spec; *q && k < 8; k++) { const char *end = strchr(q, ','); size_t n = end ? (size_t)(end - q) : strlen(q);
        if (n >= L && !strncmp(q, pt, L) && (n == L || q[L] == ':')) { int lim = n > L ? atoi(q + L + 1) : 1; if (cnt[k] < lim) { cnt[k]++; errno = ENOSPC; LOG("DM_TEST_FS_FAILAT：%s 第 %d 次按 ENOSPC 失败", pt, cnt[k]); return 1; } return 0; }
        q = end ? end + 1 : q + n; }
    return 0;
}
static int in_txn_dir(const char *hp) { for (int i = 0; i < g_nmnt; i++) { size_t n = strlen(g_mnt[i].dir); if (g_mnt[i].commit[0] && !strncmp(hp, g_mnt[i].dir, n) && (hp[n] == '/' || !hp[n])) return 1; } return 0; }
static int rm_tree(const char *p);
static Result mount_ex(const char *name, const char *dir, int ro, int txn) {
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmnt; i++)
        if (!strcmp(g_mnt[i].name, name)) { pthread_mutex_unlock(&g_lock); return RES_OK; }
    if (g_nmnt >= MAX_MOUNT) die("挂载表满");
    strlcpy(g_mnt[g_nmnt].name, name, sizeof g_mnt[0].name);
    g_mnt[g_nmnt].ro = ro; g_mnt[g_nmnt].overlay[0] = 0; g_mnt[g_nmnt].commit[0] = 0;
    if (!strcmp(name, "rom") && getenv("DM_ROMFS_OVERLAY"))
        strlcpy(g_mnt[g_nmnt].overlay, getenv("DM_ROMFS_OVERLAY"), sizeof g_mnt[0].overlay);
    int txn_err = 0;   // 事务目录建不起来（清不掉残留 / 克隆失败，盘满时真实可能）：不退出，改成只读挂载已提交目录——角色照常读得到，写一律失败、提交为空操作，已提交版本一个字节不动 | The transaction directory cannot be set up (leftovers cannot be cleared / clone failed, which really happens on a full disk): do not exit; mount the committed directory read-only instead. Characters still load, every write fails, commits are no-ops, and not one byte of the committed version changes
    if (txn) {
        char work[1100], neu[1100]; snprintf(work, sizeof work, "%s.work", dir); snprintf(neu, sizeof neu, "%s.new", dir);
        mkdir(dir, 0755);
        const char *why = NULL;
        if (!access(work, F_OK) && rm_tree(work)) why = "清不掉上次残留的工作目录";
        else if (!access(neu, F_OK) && rm_tree(neu)) why = "清不掉上次残留的 .new";
        else if (dm_fs_test_fail("mountclone") || clonefile(dir, work, 0)) why = "克隆工作目录失败";
        if (why) { txn_err = errno ? errno : EIO; g_mnt[g_nmnt].ro = 1; strlcpy(g_mnt[g_nmnt].dir, dir, sizeof g_mnt[0].dir);
            LOG("【红】存档事务：%s（%s，errno=%d）——改为只读挂载已提交目录 %s：本局存档写不进去，已提交版本不变", why, work, txn_err, dir); }
        else {
            strlcpy(g_mnt[g_nmnt].commit, dir, sizeof g_mnt[0].commit);
            atomic_store(&g_save_poison, 0);                                // 工作目录刚从已提交版克隆，完好 | the working directory was just cloned from the committed version, intact
            strlcpy(g_mnt[g_nmnt].dir, work, sizeof g_mnt[0].dir); }
    } else strlcpy(g_mnt[g_nmnt].dir, dir, sizeof g_mnt[0].dir);
    g_nmnt++;
    pthread_mutex_unlock(&g_lock);
    if (txn_err) front_save_failed(txn_err);
    if (!ro && !txn) mkdir(dir, 0755);
    LOG("fs 挂载 %s: → %s%s%s", name, txn ? g_mnt[g_nmnt - 1].dir : dir, ro || txn_err ? "（只读）" : txn ? "（事务式存档：工作目录，Commit 原子换入）" : "", g_mnt[g_nmnt - 1].overlay[0] ? "（overlay 优先）" : "");
    return RES_OK;
}
static Result mount(const char *name, const char *dir, int ro) { return mount_ex(name, dir, ro, 0); }

// APFS 缺省不分大小写，Horizon 路径分大小写：每个组件按客体名精确查找。
// 普通宿主名字不变；只有名字在宿主上冲突时才另取物理名，xattr 保存客体名。
// clonefile 保留 xattr，故存档 Commit/重挂不用另设映射表；列举不能泄露物理别名。
// APFS is case-insensitive by default, Horizon paths are case-sensitive: every component is looked up exactly by guest name.
// Ordinary host names are unchanged; only when a name collides on the host is a separate physical name chosen, with the guest name stored in an xattr.
// clonefile preserves xattr, so save Commit/remount needs no separate mapping table; enumeration must not leak physical aliases.
#define FS_GUEST_NAME "org.d2rmobile.guest-name"
static int guest_name(const char *parent, const char *physical, char *out, size_t n) {
    char p[1280]; snprintf(p, sizeof p, "%s/%s", parent, physical);
    ssize_t k = getxattr(p, FS_GUEST_NAME, out, n - 1, 0, XATTR_NOFOLLOW);
    if (k >= 0) { out[k] = 0; return 1; }
    if (errno != ENOATTR) return 0;
    strlcpy(out, physical, n); return 1;
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
        if (!lstat(p, &st)) { // 精确客体名不存在，但 APFS 把物理名匹配到了另一项 | the exact guest name does not exist, but APFS matched the physical name to another entry
            unsigned i = 0;
            do { snprintf(physical, sizeof physical, ".dm-case-%u", ++i);
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
// 覆盖层里宿主自己读的文件（标记文件、旁路表、数据档）统一从这里找。DM_ROMFS_OVERLAY 可以有多层，冒号分隔、前面的优先——老机型是两层
// 「包内 romfs_overlay_astc:补充包目录」；把整串当成一个目录去拼文件名，老机型上就一个也找不到。
// Files in the overlay that the host itself reads (marker files, sidecar tables, data archives) are all looked up here. DM_ROMFS_OVERLAY may have
// several layers, colon-separated, earlier ones taking priority; old devices have two layers,
// "in-bundle romfs_overlay_astc:Legacy Device Pack directory"; treating the whole string as one directory when building file names finds nothing at all
// on old devices.
int dm_overlay_file(const char *rel, char *out, size_t n) {
    const char *ov = getenv("DM_ROMFS_OVERLAY");
    for (const char *p = ov ? ov : ""; *p;) {
        size_t l = strcspn(p, ":");
        if (l && snprintf(out, n, "%.*s/%s", (int)l, p, rel) < (int)n && !access(out, R_OK)) return 1;
        p += l; if (*p == ':') p++;
    }
    if (n) out[0] = 0;
    return 0;
}
static int rom_overlay_path(const mount_t *m, const char *rest, char *out, size_t n) {
    char *roots = strdup(m->overlay);
    if (!roots) die("rom overlay 路径分配失败");
    char *save = NULL;
    for (char *root = strtok_r(roots, ":", &save); root; root = strtok_r(NULL, ":", &save)) {
        char candidate[1024];
        if (root_path(root, rest, candidate, sizeof candidate) && !access(candidate, F_OK)) {
            strlcpy(out, candidate, n); free(roots); return 1;
        }
    }
    int found = root_path(m->dir, rest, out, n) && !access(out, F_OK);
    free(roots);
    if (found) return 1;
    errno = ENOENT; return 0;
}
// create 只准最后一段尚不存在；中间目录仍需精确命中。
// create only allows the last component to not exist yet; intermediate directories still need an exact match.
static int host_path_ex(const char *p, char *out, size_t n, int *ro, int create) {
    const char *c = strchr(p, ':');
    if (!c) { errno = ENOENT; return 0; }
    size_t ln = c - p;
    for (int i = 0; i < g_nmnt; i++) {
        if (strlen(g_mnt[i].name) != ln || strncmp(g_mnt[i].name, p, ln)) continue;
        if (ro) *ro = g_mnt[i].ro;
        const char *rest = c + 1;
        if (g_mnt[i].overlay[0] && !create) return rom_overlay_path(&g_mnt[i], rest, out, n);
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
static int set_guest_name(const char *hp, const char *path) {
    const char *end = path + strlen(path); while (end > path && end[-1] == '/') end--;
    const char *start = end; while (start > path && start[-1] != '/' && start[-1] != ':') start--;
    char guest[0x301]; size_t k = end - start;
    if (k >= sizeof guest) { errno = ENAMETOOLONG; return -1; }
    memcpy(guest, start, k); guest[k] = 0;
    const char *physical = strrchr(hp, '/') + 1;
    if (strcmp(physical, guest)) return setxattr(hp, FS_GUEST_NAME, guest, strlen(guest), 0, XATTR_NOFOLLOW);
    if (!removexattr(hp, FS_GUEST_NAME, XATTR_NOFOLLOW) || errno == ENOATTR) return 0;
    return -1;
}
static Result err_res(const char *op, const char *path) {
    if (errno == ENOENT || errno == ENOTDIR) return RES_PATH_NOT_FOUND;
    if (errno == EEXIST) return RES_PATH_EXISTS;
    LOG("fs %s %s 失败 errno=%d", op, path, errno); return RES_FAIL;
}
// 名字解析与创建/改名/列举同锁，别名与 xattr 对客体一起发布。
// Name resolution shares one lock with create/rename/enumerate, aliases and xattr are published to the guest together.
#define HP_EX(path, create) PATH_LOCK(); char hp[1024]; int ro = 0; if (!host_path_ex(path, hp, sizeof hp, &ro, create)) return err_res("path", path)
#define HP(path) HP_EX(path, 0)
#define RW() if (ro) { LOG("fs 写只读挂载：%s", hp); return RES_FAIL; }
// 建目录 / 建文件：目标已存在就照可写挂载的语义回「已存在」——游戏只认得这一种失败（原版存档盘不会写失败），回通用失败它会拿空句柄继续走（iOS 只读回退局在 CloseDirectory 崩）
// Create directory / create file: if the target exists, answer "already exists" as a writable mount would. That is the only failure the game understands (the original save storage never fails to write); with a generic failure it carries on with a null handle (the iOS read-only fallback crashed in CloseDirectory)
#define RW_CREATE() if (ro) { if (!access(hp, F_OK)) return RES_PATH_EXISTS; LOG("fs 写只读挂载：%s", hp); return RES_FAIL; }

// ---------- 挂载 ----------
// ---------- Mount ----------
HLE_FORCE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", "fsp-srv；rom 直读宿主目录，缓存只给最小值", Result, h_fs_QueryRomCache, size_t *out) { *out = 0x1000; return RES_OK; }
HLE_FORCE("_ZN2nn2fs8MountRomEPKcPvm", "fsp-srv；rom → 宿主 RomFS 目录", Result, h_fs_MountRom, const char *name, void *cache, size_t sz) {
    (void)cache; (void)sz; char b[1024];
    Result r = mount(name, envdir("DM_ROMFS", "data/switch/3.3.94012/Update/RomFS", b, sizeof b), 1);
    if (r == RES_OK && getenv("DM_ROMFS_OVERLAY")) {
        PATH_LOCK();
        for (int i = 0; i < g_nmnt; i++) if (!strcmp(g_mnt[i].name, name)) {
            strlcpy(g_mnt[i].overlay, getenv("DM_ROMFS_OVERLAY"), sizeof g_mnt[i].overlay); break;
        }
    }
    return r;
}
HLE_FORCE("_ZN2nn2fs21MountTemporaryStorageEPKc", "fsp-srv；临时区 → 宿主目录", Result, h_fs_MountTemp, const char *name) {
    char b[1024]; return mount(name, envdir("DM_TEMP", "run/platform/temp", b, sizeof b), 0);
}
HLE_FORCE("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", "fsp-srv；存档目录在挂载时建", Result, h_fs_EnsureSave, const void *uid) { (void)uid; return RES_OK; }
// 触摸产品的设置缺省（掉落物名字默认常显、设置里可关）：存档里的 Settings/Settings.json 只记玩家改过的键，没记的走游戏缺省。
// 挂存档前给「玩家还没碰过」的两个键补上触摸产品的缺省；已有的键一律不动（玩家在选项页改过就以玩家为准）：
//   "Item Name Display": 1 —— 选项页「显示物品」= 开关式（游戏缺省 0 = 长按；触摸没有可按住的键）。配套：前端在本进程首次进到局内常态时替玩家点一次「显示物品」键（front_ios.m），
//       开关式下点一次即一直显示，退出重进下一局仍保持；玩家想关就把这一项改回「长按」。
//   "Item Tooltip Hotkey Appender": 0 —— 物品说明条不带手柄键位提示（游戏缺省 1）。
//   小地图（放右上角：迷你、半透明）：
//   "AutoMapMode": 2 = 微缩地图放右（0 全屏 / 1 放左 / 2 放右，游戏缺省也是 2，这里写明）；透明度（AutoMapFade）不补、用游戏缺省 0 = 不淡化——选「全部」（3）时小地图的线条淡到几乎只剩文字，地图本来就是叠在画面上的线条，不淡化也透得过去；
//   "AutoMap Open On Load": 1 = 进局自动开启（游戏缺省 0）。点小地图 = 地图开关键，见 front_ios.m。
// iOS 包缺省做；Mac 宿主缺省不做（闸门对 Switch 真值比图），DM_TOUCH_DEFAULTS=1 开 / =0 关。
// 共享仓库预置：存档目录里没有 ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i 时写入预置文件（stash_seed.h），已有的一个字节不动。
// 在挂存档、克隆出工作目录之前做，所以游戏第一次读到的就是它。iOS 上缺省开、DM_STASH_SEED=0 关掉；Mac 宿主缺省关、DM_STASH_SEED=1 打开。
// 只覆盖 DLC 类型（标准 + 专家）：毁灭之王类型用的 SharedStash{Soft,Hard}CoreV2.d2i 不预置——触摸建角页建不出那类角色
// 预置文件 stash_seed.h 是游戏自己生成的一份存档的字节，公开仓库不带它：头文件不在就不预置（游戏第一次用到共享仓库时照原版自己生成一份空的）
// Settings defaults for the touch product (dropped item names shown permanently by default, can be turned off in settings): Settings/Settings.json in the save only records keys the player has changed, unrecorded ones use the game default.
// Before mounting the save, fill in the touch product defaults for the two keys "the player has not touched yet"; existing keys are never changed (if the player changed it in the options page, the player wins):
//   "Item Name Display": 1 -- options page 「显示物品」(Show Items) = toggle style (game default 0 = hold; touch has no key that can be held). Companion: the front end taps the 「显示物品」(Show Items) key once for the player the first time this process reaches the normal in-run state (front_ios.m),
//       in toggle style one tap keeps it shown, and it persists into the next run after exit and re-entry; a player who wants it off changes this item back to 「长按」(Hold).
//   "Item Tooltip Hotkey Appender": 0 -- item tooltips carry no controller key hints (game default 1).
//   Minimap (placed top right: mini, semi-transparent):
//   "AutoMapMode": 2 = mini map on the right (0 full screen / 1 left / 2 right, the game default is also 2, stated here explicitly); opacity (AutoMapFade) is not filled in, the game default 0 = no fade is used -- with 「全部」(All) (3) the minimap lines fade until almost only the text remains; the map is lines overlaid on the picture anyway, so it shows through without fading;
//   "AutoMap Open On Load": 1 = open automatically on entering a run (game default 0). Tapping the minimap = the map toggle key, see front_ios.m.
// Done by default in the iOS app; not done by default on the Mac host (gate checks compare images against Switch ground truth), DM_TOUCH_DEFAULTS=1 on / =0 off.
// Shared stash seed: when the save directory has no ModernSharedStashSoftCoreV2/ModernSharedStashSoftCoreV2.d2i, write the seed file (stash_seed.h); an existing one is not changed by a single byte.
// Done before mounting the save and cloning the working directory, so it is what the game reads the first time. On by default on iOS, DM_STASH_SEED=0 turns it off; off by default on the Mac host, DM_STASH_SEED=1 turns it on.
// Only covers the DLC type (standard + hardcore): SharedStash{Soft,Hard}CoreV2.d2i used by the Lord of Destruction type is not seeded -- the touch character creation page cannot create that kind of character
// The seed file stash_seed.h holds the bytes of a save the game itself generated, and the public repository does not ship it: without the header nothing is seeded (the game then creates an empty shared stash of its own the first time one is needed, as the original does)
#if __has_include("stash_seed.h")
#include "stash_seed.h"
static void stash_seed_one(const char *save_dir, const char *name) {
    char dir[1100], path[1200], tmp[1210]; snprintf(dir, sizeof dir, "%s/%s", save_dir, name); snprintf(path, sizeof path, "%s/%s.d2i", dir, name);
    struct stat st; if (!stat(path, &st)) return;
    if (mkdir(dir, 0755) && errno != EEXIST) { LOG("共享仓库预置：建目录 %s 失败（%s），不预置", dir, strerror(errno)); return; }
    snprintf(tmp, sizeof tmp, "%s.tmp", path); FILE *f = fopen(tmp, "wb"); if (!f) { LOG("共享仓库预置：写 %s 失败（%s），不预置", tmp, strerror(errno)); return; }
    int ok = fwrite(g_stash_seed, 1, sizeof g_stash_seed, f) == sizeof g_stash_seed; ok = fclose(f) == 0 && ok;
    if (ok && !rename(tmp, path)) LOG("共享仓库预置：%s 不存在，已写入预置文件（%zu 字节：250 万金币 + 2 枚赦免之证）", path, sizeof g_stash_seed); else { remove(tmp); LOG("共享仓库预置：写入失败，不预置"); }
}
// 标准级与专家级各一份，各判各的（游戏按角色类型选文件名，main+0x2ce1e8：DLC 类型标准 = ModernSharedStashSoftCoreV2、专家 = ModernSharedStashHardCoreV2；文件格式相同）
// One copy each for standard and hardcore, each judged on its own (the game picks the file name by character type, main+0x2ce1e8: DLC type standard = ModernSharedStashSoftCoreV2, hardcore = ModernSharedStashHardCoreV2; same file format)
static void stash_seed(const char *save_dir) {
    const char *e = getenv("DM_STASH_SEED"); int on = e ? strcmp(e, "0") != 0 : TARGET_OS_IPHONE; if (!on) return;   // iOS 上缺省开；Mac 宿主缺省关（对拍 / 存档事务这些闸门逐字节比存档目录，多出一个文件就红），要验时设 DM_STASH_SEED=1 | on by default on iOS; off by default on the Mac host (gate checks such as lockstep comparison / save transaction compare the save directory byte by byte, one extra file turns them red), set DM_STASH_SEED=1 to verify
    stash_seed_one(save_dir, "ModernSharedStashSoftCoreV2"); stash_seed_one(save_dir, "ModernSharedStashHardCoreV2");
}
#else
static void stash_seed(const char *save_dir) { (void)save_dir; }
#endif
// 「大字体」（设置键 "Low Vision Mode"）兜底：设置文件里它是 1 就改回 0。它把界面换到原版的低视力布局档，我们的触摸界面没为那份档做过；在设置页打开后卡住。
// 设置页里这一项另由界面覆盖层藏掉；这里保证已经打开过的设备重启即恢复。与 settings_seed 同一个开关（iOS 上缺省开，Mac 宿主设 DM_TOUCH_DEFAULTS=1 才开）
// Fallback for 「大字体」(Large Font) (settings key "Low Vision Mode"): if it is 1 in the settings file, change it back to 0. It switches the UI to the original game's low-vision layout profile, which our touch UI was never built for; it hangs after being turned on in the settings page.
// This item in the settings page is separately hidden by the UI overlay; this here guarantees that devices where it was already turned on recover on restart. Same switch as settings_seed (on by default on iOS, on the Mac host only with DM_TOUCH_DEFAULTS=1)
static void low_vision_off(const char *save_dir) {
    const char *e = getenv("DM_TOUCH_DEFAULTS"); int on = e ? strcmp(e, "0") != 0 : TARGET_OS_IPHONE; if (!on) return;
    char path[1200]; snprintf(path, sizeof path, "%s/Settings/Settings.json", save_dir); static char buf[65536]; FILE *f = fopen(path, "rb"); if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); if (n == sizeof buf - 1) return; buf[n] = 0;
    char *k = strstr(buf, "\"Low Vision Mode\""); if (!k) return; char *v = strchr(k + 17, ':'); if (!v) return; v++; while (*v == ' ' || *v == '\t') v++;
    if (*v != '1' || (v[1] >= '0' && v[1] <= '9')) return; *v = '0';
    char tmp[1210]; snprintf(tmp, sizeof tmp, "%s.tmp", path); f = fopen(tmp, "wb"); if (!f) return; int ok = fwrite(buf, 1, n, f) == n; ok = fclose(f) == 0 && ok;
    if (ok && !rename(tmp, path)) LOG("设置兜底：「大字体」（Low Vision Mode）是开着的，已改回关"); else remove(tmp);
}
static void settings_seed(const char *save_dir) {
    const char *e = getenv("DM_TOUCH_DEFAULTS"); int on = e ? strcmp(e, "0") != 0 : TARGET_OS_IPHONE; if (!on) return;
    static const char *kv[][2] = {{"\"Item Name Display\"", "1"}, {"\"Item Tooltip Hotkey Appender\"", "0"},
                                  {"\"AutoMapMode\"", "2"}, {"\"AutoMap Open On Load\"", "1"}};
    char dir[1100], path[1200]; snprintf(dir, sizeof dir, "%s/Settings", save_dir); snprintf(path, sizeof path, "%s/Settings.json", dir);
    static char buf[65536], out[66000]; size_t n = 0; FILE *f = fopen(path, "rb");   // static：挂存档的游戏线程栈小（两块 64 KB 放栈上直接越栈，实测崩）；挂存档只在一个线程上做 | static: the game thread that mounts the save has a small stack (two 64 KB blocks on the stack overflow it outright, measured crash); mounting the save is done on one thread only
    int first = f == NULL;   // 没有设置文件 = 首次安装 | no settings file = first install
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); if (n == sizeof buf - 1) { LOG("设置缺省：%s 超过 64 KB，不动", path); return; } }
    buf[n] = 0;
    char *brace = strchr(buf, '{'); int had = brace != NULL; size_t o = 0; int added = 0;
    o += (size_t)snprintf(out + o, sizeof out - o, "{");
    for (size_t i = 0; i < sizeof kv / sizeof kv[0]; i++) if (!strstr(buf, kv[i][0])) { o += (size_t)snprintf(out + o, sizeof out - o, "\n    %s: %s,", kv[i][0], kv[i][1]); added++; }
    // 首次安装按系统语言定原版的文字语言和语音语言（dm_sys_lang()，见 dm_i18n.c；只在没有设置文件时）。已有设置文件不补这两个键（老玩家照旧）；之后由自定义页的语言开关改（settings_lang_apply）
    // On first install the original game's text and voice languages are set from the system language (dm_sys_lang(), see dm_i18n.c; only when there is no settings file). An existing settings file does not get these two keys added (existing players stay as they are); afterwards they are changed by the language switch on the Custom page (settings_lang_apply)
    if (first) { int dm_sys_lang(void); o += (size_t)snprintf(out + o, sizeof out - o, "\n    \"Language\": %d,\n    \"VoiceLanguage\": %d,", dm_sys_lang(), dm_sys_lang()); added += 2; }
    if (!added) return;
    if (had) { const char *rest = brace + 1; const char *q = rest; while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
        if (*q == '}') { out[o - 1] = 0; o--; }   // 原文件是空对象：去掉补的最后一个逗号 | the original file is an empty object: remove the last comma that was added
        o += (size_t)snprintf(out + o, sizeof out - o, "%s", rest); }
    else { out[o - 1] = 0; o--; o += (size_t)snprintf(out + o, sizeof out - o, "\n}\n"); }
    mkdir(dir, 0755); char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.seed", path);
    f = fopen(tmp, "wb"); if (!f || fwrite(out, 1, o, f) != o) { if (f) fclose(f); LOG("【红】设置缺省：写 %s 失败", tmp); return; }
    fclose(f); if (rename(tmp, path)) { LOG("【红】设置缺省：改名到 %s 失败", path); return; }
    LOG("设置缺省：%s 补了 %d 个键（显示物品 = 开关式 / 说明条热键 = 关 / 小地图放右、进局自动开%s；已有的键不动）", path, added, first ? " / 首次安装按系统语言写 Language" : "");
}
// 语言开关（自定义页标题行那一格，dm_custom.c 的 lang_en）：玩家点过「应用」后设置文件里有明确的值，每次挂存档（游戏读设置之前）按它把原版设置文件的
// Language（文字）和 VoiceLanguage（语音，语音跟着切）改成 0 英文 / 8 简中。只在「与开关不符」时改：开关 = 英文而文字不是 0；开关 = 中文而文字是 0。
// 所以开关关着时玩家若在原版里选了繁中等别的语言，不会被改回简中。两个键缺哪个补哪个。没点过应用（-1）= 一个字不动
// Language switch (the cell in the title row of the Custom page, lang_en in dm_custom.c): once the player has tapped 「应用」(Apply) the settings file holds an explicit value, and on every save mount (before the game reads its settings) the original game's settings file has its
// Language (text) and VoiceLanguage (voice; the voice switches along with it) changed to 0 English / 8 Simplified Chinese accordingly. It is changed only when it "does not match the switch": switch = English but text is not 0; switch = Chinese but text is 0.
// So with the switch off, if the player has chosen another language such as Traditional Chinese in the original game, it is not changed back to Simplified Chinese. Whichever of the two keys is missing is added. Apply never tapped (-1) = nothing is touched
static int json_int_set(char *buf, size_t cap, const char *key, int val) {   // 把 "key": N 的 N 换成 val（没有这个键就插在第一个 { 之后）；返回 1 = 改了 | Replace the N of "key": N with val (if the key is absent, insert it after the first {); returns 1 = changed
    char *k = strstr(buf, key); char num[16]; snprintf(num, sizeof num, "%d", val);
    if (!k) { char *b = strchr(buf, '{'); char ins[96]; int m = snprintf(ins, sizeof ins, "\n    %s: %s,", key, num); if (!b || strlen(buf) + (size_t)m + 1 >= cap) return 0;
        memmove(b + 1 + m, b + 1, strlen(b + 1) + 1); memcpy(b + 1, ins, (size_t)m); return 1; }
    char *v = strchr(k + strlen(key), ':'); if (!v) return 0; v++; while (*v == ' ' || *v == '\t') v++; char *e = v; if (*e == '-') e++; while (*e >= '0' && *e <= '9') e++;
    if (e == v || atoi(v) == val) return 0; size_t nl = strlen(num); if (strlen(buf) + nl + 1 >= cap) return 0;
    memmove(v + nl, e, strlen(e) + 1); memcpy(v, num, nl); return 1; }
static void settings_lang_apply(const char *save_dir) {
    int dm_custom_lang_explicit(void); int en = dm_custom_lang_explicit(); if (en < 0) return;
    char path[1200]; snprintf(path, sizeof path, "%s/Settings/Settings.json", save_dir); static char buf[66000]; FILE *f = fopen(path, "rb"); if (!f) return;
    size_t n = fread(buf, 1, 65535, f); fclose(f); if (n == 65535) return; buf[n] = 0;
    const char *k = strstr(buf, "\"Language\""), *c = k ? strchr(k + 10, ':') : NULL; int cur = c ? atoi(c + 1) : 8;   // 没写 = 游戏缺省（宿主报简中） | Not written = game default (the host reports Simplified Chinese)
    if ((en && cur == 0) || (!en && cur != 0)) { const char *vk = strstr(buf, "\"VoiceLanguage\""), *vc = vk ? strchr(vk + 15, ':') : NULL; int vcur = vc ? atoi(vc + 1) : cur;
        if ((en && vcur == 0) || (!en && vcur != 0)) return; }   // 文字、语音都已与开关相符 | Text and voice already match the switch
    int want = en ? 0 : 8, ch = json_int_set(buf, sizeof buf, "\"Language\"", want) | json_int_set(buf, sizeof buf, "\"VoiceLanguage\"", want); if (!ch) return;
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.lang", path); f = fopen(tmp, "wb"); size_t m = strlen(buf);
    if (!f || fwrite(buf, 1, m, f) != m) { if (f) fclose(f); LOG("【红】语言开关：写 %s 失败", tmp); return; }
    fclose(f); if (rename(tmp, path)) { LOG("【红】语言开关：改名到 %s 失败", path); return; }
    LOG("语言开关：设置文件的 Language / VoiceLanguage 改成 %d（%s；原文字语言 %d）", want, en ? "英文" : "简中", cur);
}
// 事务式存档当前的工作目录（游戏读写的那份；宿主自己的旁路文件写在这里才会随 Commit 进已提交版）。没挂载存档时返回 NULL
// The current working directory of the transactional save (the copy the game reads and writes; the host's own side files must be written here to enter the committed version with Commit). Returns NULL when no save is mounted
const char *dm_save_workdir(void) { for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].commit[0]) return g_mnt[i].dir; return NULL; }
// 事务式存档的已提交目录（盘上那份，上次 Commit 的版本）。没挂载存档时返回 NULL
// The committed directory of the transactional save (the copy on disk, the version from the last Commit). Returns NULL when no save is mounted
const char *dm_save_commitdir(void) { for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].commit[0]) return g_mnt[i].commit; return NULL; }
void (*g_dm_mount_seed_hook)(const char *save_dir);   // 挂存档前调（存档目录）：往存档里补宿主要的文件 | called before mounting the save (save dir): adds the files the host needs to the save
HLE_FORCE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", "fsp-srv；存档 → 宿主目录（单用户）", Result, h_fs_MountSave, const char *name, const void *uid) {
    (void)uid;
    // 不给缺省目录：静默落到一个没有角色的目录上会让「选角」测量失真（曾因此误判过）。跑局一律用 save_ref 的新副本
    // No default directory: silently landing on a directory with no characters distorts the "character select" measurement (this once caused a misjudgment). Runs always use a fresh copy of save_ref
    const char *d = getenv("DM_SAVE");
    if (!d) die("未设 DM_SAVE：请指向一份存档目录的可写副本");
    char dd[1024]; strlcpy(dd, d, sizeof dd); size_t L = strlen(dd); while (L > 1 && dd[L - 1] == '/') dd[--L] = 0;   // 去掉结尾斜杠（.work / .new 拼在同级） | strip trailing slashes (.work / .new are appended at the same level)
    stash_seed(dd);
    low_vision_off(dd);
    settings_seed(dd); settings_lang_apply(dd);
    // 掉落过滤的规则文件（原版启动时载入，dm_lootfilter.c 用构造函数登记这个钩子；只链 hle_fs.c 的单测里它是空的）。与 settings_seed 同一个开关：
    // iOS 上缺省写；Mac 宿主的对拍闸门（存档要和真机真值逐文件相同）不写
    // Rule files for the loot filter (the game loads them at startup; dm_lootfilter.c registers this hook from a constructor; it is null in unit tests that link only hle_fs.c). Same switch as settings_seed:
    // written by default on iOS; not written under the Mac host's parity gate (saves must match the device ground truth file by file)
    { const char *e = getenv("DM_TOUCH_DEFAULTS"); if (g_dm_mount_seed_hook && (e ? strcmp(e, "0") != 0 : TARGET_OS_IPHONE)) g_dm_mount_seed_hook(dd); }
    return mount_ex(name, dd, 0, 1);
}
void (*g_dm_pre_commit_hook[5])(const char *workdir);   // 提交前调（工作目录）：宿主旁路文件按工作目录里的存档内容核对后再写（[0] 个人仓库多页 dm_pages.c、[2] 百宝囊 dm_pouch.c、[1] 编年史仓库 dm_vault.c、[3] 秘境进度 dm_rift.c、[4] iCloud 收件箱并回 dm_cloudmerge.c） | Called before commit (working directory): host side files are checked against the save content in the working directory before being written ([0] personal stash multi-page dm_pages.c, [2] pouch dm_pouch.c, [1] Chronicle stash dm_vault.c, [3] Rift progress dm_rift.c, [4] iCloud inbox merge dm_cloudmerge.c)
void (*g_dm_save_commit_hook)(const char *commit_dir);   // 存档提交成功后调（已提交目录）；iOS 的 iCloud 同步设它，宿主为空 | called after a save commit succeeds (committed directory); the iOS iCloud sync sets it, NULL on the host
static _Atomic uint64_t g_save_commits;   // 成功提交过几次（前端进后台时等「游戏这次存盘已提交」用） | number of successful commits (used by the front end when going to background to wait for "the game's current save has been committed")
uint64_t dm_fs_commit_count(void) { return atomic_load(&g_save_commits); }
HLE_FORCE("_ZN2nn2fs6CommitEPKc", "fsp-srv；事务式存档：工作目录整树 clone 后与已提交目录原子互换（renamex_np RENAME_SWAP）", Result, h_fs_Commit, const char *name) {
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    int i = 0; while (i < g_nmnt && strcmp(g_mnt[i].name, name)) i++;
    if (i == g_nmnt || !g_mnt[i].commit[0]) { pthread_mutex_unlock(&g_lock); return RES_OK; }   // 非事务挂载：无可提交 | non-transactional mount: nothing to commit
    uint64_t tc0 = clock_gettime_nsec_np(CLOCK_MONOTONIC), tw0 = atomic_exchange(&g_txn_first_ns, 0);
    for (int k = 0; k < 5; k++) if (g_dm_pre_commit_hook[k]) g_dm_pre_commit_hook[k](g_mnt[i].dir);
    uint64_t tc1 = clock_gettime_nsec_np(CLOCK_MONOTONIC);   // 写进工作目录的旁路文件随这次提交一起原子换入 | side files written into the working directory are swapped in atomically together with this commit
    int perr = atomic_exchange(&g_save_poison, 0);
    if (perr) {                                                         // 本事务有写失败：拒绝提交、丢弃工作目录 | a write failed in this transaction: refuse the commit, discard the working directory
        char torn[512] = ""; size_t tl = 0; DIR *d0 = opendir(g_mnt[i].dir); struct dirent *e0;   // 记下被截成 0 字节的文件（若照常提交就会进存档的残缺） | record files truncated to 0 bytes (the damage that would enter the save if committed as usual)
        while (d0 && (e0 = readdir(d0))) {
            if (e0->d_name[0] == '.') continue;
            char sub[1300]; snprintf(sub, sizeof sub, "%s/%s", g_mnt[i].dir, e0->d_name); DIR *d1 = opendir(sub); struct dirent *e1;
            while (d1 && (e1 = readdir(d1))) { char f[1600]; struct stat st; snprintf(f, sizeof f, "%s/%s", sub, e1->d_name);
                if (e1->d_name[0] != '.' && !stat(f, &st) && S_ISREG(st.st_mode) && !st.st_size && tl + strlen(e0->d_name) + strlen(e1->d_name) + 3 < sizeof torn)
                    tl += snprintf(torn + tl, sizeof torn - tl, " %s/%s", e0->d_name, e1->d_name); }
            if (d1) closedir(d1);
        }
        if (d0) closedir(d0);
        int nopen = 0; for (int k = 0; k < 65536; k++) nopen += g_fsave[k];   // 仍开着的存档文件：重新克隆后这些句柄指向被删的旧工作目录 | save files still open: after re-cloning, these handles point into the deleted old working directory
        // 先把已提交版克隆到同级临时目录，成了再与被污染的工作目录原子互换、删掉换下来的；任一步失败都不退出：工作目录留着、「已污染」标记放回去
        // （下次提交照样拒绝、再试一次重新克隆），已提交版本不变（原先先删工作目录再克隆，克隆失败就 die，盘满时进程直接退出、工作目录也没了）
        // First clone the committed version into a sibling temporary directory; once that succeeds, atomically swap it with the tainted work directory and delete the swapped-out one. If any step fails, do not exit: the work directory stays and the "tainted" mark is put back
        // (the next commit is refused again and recloning is retried); the committed version is unchanged (before, the work directory was deleted first and then recloned, and a failed clone called die, so with a full disk the process exited and the work directory was gone)
        char re[1100]; snprintf(re, sizeof re, "%s.re", g_mnt[i].dir); int rerr = 0;
        if (!access(re, F_OK) && rm_tree(re)) rerr = errno ? errno : EIO;
        else if (dm_fs_test_fail("reclone") || clonefile(g_mnt[i].commit, re, 0)) { rerr = errno ? errno : EIO; rm_tree(re); }
        else if (renamex_np(re, g_mnt[i].dir, RENAME_SWAP)) { rerr = errno ? errno : EIO; rm_tree(re); }
        else rm_tree(re);                                               // 换下来的被污染工作目录 | The swapped-out tainted work directory
        if (rerr) { save_poison(perr); LOG("【红】存档事务：重新克隆工作目录失败 errno=%d——工作目录不动、保持已污染（下次提交照样拒绝并重试），已提交版本不变", rerr); }
        pthread_mutex_unlock(&g_lock);
        LOG("【红】存档提交拒绝：本事务有存档写失败 errno=%d（工作目录里 0 字节文件：%s）；已提交版本不变，工作目录%s%s", perr, tl ? torn + 1 : "无", rerr ? "没能重新克隆（见上一行）" : "已从已提交版重新克隆", nopen ? "；【红】仍有存档文件开着" : "");
        front_save_failed(perr);
        return RES_FAIL;
    }
    char neu[1100]; snprintf(neu, sizeof neu, "%s.new", g_mnt[i].commit);
    Result r = RES_OK;
    if (!access(neu, F_OK)) rm_tree(neu);
    if (clonefile(g_mnt[i].dir, neu, 0)) { LOG("【红】存档提交：clonefile %s 失败 errno=%d（已提交版本不变）", neu, errno); r = RES_FAIL; }
    else if (renamex_np(neu, g_mnt[i].commit, RENAME_SWAP)) { LOG("【红】存档提交：原子互换失败 errno=%d（已提交版本不变）", errno); rm_tree(neu); r = RES_FAIL; }
    else rm_tree(neu);                                                  // 换下来的旧版 | the old version that was swapped out
    // 耗时读数（整数微秒，不带浮点）：提交前钩子、克隆 + 原子互换各多久；从本事务第一次存档写到提交完一共多久（游戏写文件 + 提交，0 = 本事务没有存档写）
    // Timing readouts (integer microseconds, no floating point): how long the pre-commit hook and the clone + atomic swap each took; how long in total from this transaction's first save write to the end of the commit (the game writing files + the commit; 0 = no save write in this transaction)
    static int n; if (r == RES_OK) { uint64_t tc2 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        LOG("存档提交 #%d：%s 原子换入 %s（此前存档写 %d 次）（钩子 %llu 微秒、克隆换入 %llu 微秒、自本事务首次写起 %llu 微秒）", ++n, g_mnt[i].dir, g_mnt[i].commit, atomic_load(&g_save_writes),
            (unsigned long long)((tc1 - tc0) / 1000), (unsigned long long)((tc2 - tc1) / 1000), (unsigned long long)(tw0 ? (tc2 - tw0) / 1000 : 0)); atomic_fetch_add(&g_save_commits, 1);
        if (g_dm_save_commit_hook) g_dm_save_commit_hook(g_mnt[i].commit); }   // iCloud 同步（cloud_ios.m）：持着锁调，那边只做 clonefile 快照 | iCloud sync (cloud_ios.m): called with the lock held, that side only takes a clonefile snapshot
    pthread_mutex_unlock(&g_lock);
    return r;
}
HLE_FORCE("_ZN2nn2fs7UnmountEPKc", "fsp-srv；挂载表常驻，卸载为空操作", void, h_fs_Unmount, const char *name) { (void)name; }

// ---------- 文件 ----------
// 剔零档案：资源包里被填零的大段（过场视频、旧版音乐）不随包分发——档案文件里把这些零段剔掉，旁边放一张区段表
// 「<档案名>.holes」（文本：首行 `size <原始字节数>`，其后每行 `<零段起点> <零段长度>`，按起点升序、互不重叠）。
// 宿主读这类档案时按表换算偏移：落在零段的字节返回 0，文件大小报原始值；游戏与启动预取看到的仍是原始布局，索引一个字不用改。
// 读入口只有两处：本文件的 ReadFile 与 hle_tact.c 的启动预取，都走 dm_data_pread。没有 .holes 的文件走原路径，零额外开销。
// ---------- Files ----------
// Zero-stripped archives: large zero-filled ranges in the resource pack (cutscene videos, old-version music) are not distributed with the package -- these zero-filled ranges are stripped from the archive file, and an extent table is placed beside it,
// "<archive name>.holes" (text: first line `size <original byte count>`, then one line `<zero-filled range start> <zero-filled range length>` each, ascending by start, non-overlapping).
// When reading such archives the host translates offsets through the table: bytes falling in a zero-filled range return 0 and the file size is reported as the original value; the game and the startup prefetch still see the original layout, and not a single word of the index needs changing.
// There are only two read entry points: ReadFile in this file and the startup prefetch in hle_tact.c, both going through dm_data_pread. Files without .holes take the original path with zero extra overhead.
typedef struct { int64_t size; int n; int64_t *log, *len, *phys; char name[40]; } holemap;   // 保留段：原始起点 / 长度 / 在剔零文件里的起点 | Kept segment: original start / length / start in the zero-stripped file
static _Atomic uint64_t g_hole_reads;   // 读到零段的次数：正常应为 0（零段是砍掉的过场视频 / 旧版音乐，游戏不该来读）；非 0 = 砍错了 | Number of reads that hit a zero-filled range: should normally be 0 (zero-filled ranges are stripped cutscene videos / old-version music the game should not come to read); non-zero = stripped wrongly
static holemap *g_hm[4096];
static holemap *hm_load(const char *hostpath) {
    char hp2[1100]; if (snprintf(hp2, sizeof hp2, "%s.holes", hostpath) >= (int)sizeof hp2) return NULL;
    FILE *f = fopen(hp2, "r"); if (!f) return NULL;
    long long size = -1, o, l; if (fscanf(f, "size %lld", &size) != 1 || size < 0) die("区段表格式不对：%s", hp2);
    holemap *m = calloc(1, sizeof *m); int cap = 64; m->size = size; m->log = malloc(cap * sizeof *m->log); m->len = malloc(cap * sizeof *m->len); m->phys = malloc(cap * sizeof *m->phys);
    int64_t pos = 0, phys = 0;
    while (fscanf(f, "%lld %lld", &o, &l) == 2) {
        if (o < pos || l <= 0 || o + l > size) die("区段表零段越界或乱序：%s（%lld %lld）", hp2, o, l);
        if (o > pos) { if (m->n == cap) { cap *= 2; m->log = realloc(m->log, cap * sizeof *m->log); m->len = realloc(m->len, cap * sizeof *m->len); m->phys = realloc(m->phys, cap * sizeof *m->phys); }
            m->log[m->n] = pos; m->len[m->n] = o - pos; m->phys[m->n++] = phys; phys += o - pos; }
        pos = o + l;
    }
    fclose(f);
    if (pos < size) { if (m->n == cap) { cap++; m->log = realloc(m->log, cap * sizeof *m->log); m->len = realloc(m->len, cap * sizeof *m->len); m->phys = realloc(m->phys, cap * sizeof *m->phys); }
        m->log[m->n] = pos; m->len[m->n] = size - pos; m->phys[m->n++] = phys; phys += size - pos; }
    struct stat st; if (stat(hostpath, &st) || st.st_size != phys) die("剔零档案长度与区段表不符：%s 实际 %lld，按表应为 %lld", hostpath, (long long)st.st_size, (long long)phys);
    const char *bn = strrchr(hostpath, '/'); strlcpy(m->name, bn ? bn + 1 : hostpath, sizeof m->name);
    { static _Atomic int said[64]; unsigned h = 0; for (const char *c = m->name; *c; c++) h = h * 31 + (unsigned char)*c;   // 每个档案只在首次登记时打一行 | Print a line for each archive only on first registration
      if (!atomic_exchange(&said[h % 64], 1)) LOG("剔零档案 %s：原始 %lld 字节，实存 %lld 字节，保留段 %d 个", m->name, (long long)size, (long long)phys, m->n); }
    return m;
}
void dm_data_note_fd(int fd, const char *hostpath) {   // 打开资源档案后调：有区段表就登记 | Called after opening a resource archive: register it if there is an extent table
    if (fd < 0 || fd >= 4096) return;
    g_hm[fd] = hm_load(hostpath);
}
int64_t dm_data_logical_size(const char *hostpath, int64_t phys_size) {   // 列目录 / 取大小用：有区段表报原始大小 | For directory listing / size queries: report the original size when there is an extent table
    char hp2[1100]; if (snprintf(hp2, sizeof hp2, "%s.holes", hostpath) >= (int)sizeof hp2) return phys_size;
    FILE *f = fopen(hp2, "r"); if (!f) return phys_size; long long size = -1; int ok = fscanf(f, "size %lld", &size) == 1; fclose(f); return ok && size >= 0 ? size : phys_size;
}
ssize_t dm_data_pread(int fd, void *buf, size_t n, off_t off) {   // 语义同 pread（可能短读）；剔零档案按区段表换算 | Same semantics as pread (may short-read); zero-stripped archives are translated through the extent table
    holemap *m = fd >= 0 && fd < 4096 ? g_hm[fd] : NULL;
    if (!m) return pread(fd, buf, n, off);
    if (off >= m->size) return 0;
    if ((int64_t)n > m->size - off) n = (size_t)(m->size - off);
    int lo = 0, hi = m->n;   // 第一个 结束点 > off 的保留段 | The first kept segment whose end point > off
    while (lo < hi) { int mid = (lo + hi) / 2; if (m->log[mid] + m->len[mid] <= off) lo = mid + 1; else hi = mid; }
    size_t done = 0;
    while (done < n) {
        int64_t cur = off + (int64_t)done;
        if (lo < m->n && cur >= m->log[lo]) {   // 在保留段里：直接读进目标缓冲 | In a kept segment: read directly into the destination buffer
            int64_t in = cur - m->log[lo]; size_t want = n - done; if ((int64_t)want > m->len[lo] - in) want = (size_t)(m->len[lo] - in);
            size_t got = 0; while (got < want) { ssize_t r = pread(fd, (uint8_t *)buf + done + got, want - got, m->phys[lo] + in + (int64_t)got); if (r < 0) { if (errno == EINTR) continue; return -1; } if (r == 0) return (ssize_t)(done + got); got += (size_t)r; }
            done += want; if (in + (int64_t)want == m->len[lo]) lo++;
        } else {                               // 在零段里：填 0 到下一保留段起点 | In a zero-filled range: fill 0 up to the start of the next kept segment
            int64_t end = lo < m->n ? m->log[lo] : m->size; size_t z = n - done; if ((int64_t)z > end - cur) z = (size_t)(end - cur);
            memset((uint8_t *)buf + done, 0, z); done += z;
            // 游戏按 256 KB 对齐的整块读：要的是零段旁边的保留文件时，整块会顺带压到零段边缘（无害，保留文件的字节照给）。
            // 一次读整个落在零段里才是真在读被砍的内容 → 报红（前 20 次逐条，之后每 1000 次一行）；跨边界的只说明前 3 次
            // The game reads whole blocks aligned to 256 KB: when what it wants is a kept file next to a zero-filled range, the whole block incidentally overlaps the edge of the zero-filled range (harmless, the kept file's bytes are returned as usual).
            // Only a read that lies entirely inside a zero-filled range is really reading stripped content → report a failure (the first 20 one by one, then one line per 1000); boundary-crossing reads are only noted the first 3 times
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
    g_fmode[fd] = (uint8_t)mode; *out = (uint64_t)fd; g_fd2s[fd] = (mode & (MODE_WRITE | MODE_APPEND)) && is_d2s(path);
    g_hm[fd] = NULL; if (!(mode & (MODE_WRITE | MODE_APPEND))) dm_data_note_fd(fd, hp);
    { const char *c = strchr(path, ':'); size_t ln = c ? (size_t)(c - path) : 0; g_fsave[fd] = 0;
      for (int i = 0; c && i < g_nmnt; i++) if (strlen(g_mnt[i].name) == ln && !strncmp(g_mnt[i].name, path, ln)) g_fsave[fd] = g_mnt[i].commit[0] != 0; }
    fslog_init();
    if (g_fslog && fd < 4096) { pthread_mutex_lock(&g_fslog_lock); free(g_fdpath[fd]); g_fdpath[fd] = strdup(path); pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) LOG("fs open [%.2fs] %s mode=%d → fd %d", (now_ns() - g_t0_ns) / 1e9, path, mode, fd);
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", "fsp-srv → pread(2)", Result, h_fs_ReadFile, size_t *out, uint64_t h, int64_t off, void *buf, size_t sz) {
    if (!h) { *out = 0; return RES_FAIL; }   // 文件句柄就是 fd，0 是标准输入、不会是游戏打开的文件：打开失败后照用的空句柄 | A file handle is an fd; 0 is standard input and never a file the game opened: it is the null handle the game keeps using after a failed open
    size_t got = 0; uint64_t t0 = g_flag.bootprof ? now_ns() : 0;
    dm_guest_write_prepare(buf, sz);   // 目标可能是图形层写保护的贴图池页：内核写只读页返回 EFAULT 而不缺页，先解保护并标脏 | The destination may be a texture pool page write-protected by the graphics layer: a kernel write to a read-only page returns EFAULT instead of page-faulting, so unprotect and mark dirty first
    while (got < sz) {
        ssize_t r = dm_data_pread((int)h, (uint8_t *)buf + got, sz - got, off + got);
        if (r < 0) { if (errno == EINTR) continue; return err_res("pread", "fd"); }
        if (r == 0) break;
        got += r;
    }
    *out = got;
    if (g_flag.bootprof) dm_prof_add(PROF_FS_READ, now_ns() - t0, got);
    if (g_fslog) { pthread_mutex_lock(&g_fslog_lock);
        static int hash_on = -1; if (hash_on < 0) hash_on = getenv("DM_FSLOG_HASH") != NULL;   // DM_FSLOG_HASH=1：末尾加一列读到内容的 FNV-1a 64（对比两份资源包读到的字节是否一致） | DM_FSLOG_HASH=1: append a column with the FNV-1a 64 of the content read (to compare whether the bytes read from two resource packs match)
        uint64_t fh = 0xcbf29ce484222325ull; if (hash_on) for (size_t i = 0; i < got; i++) fh = (fh ^ ((uint8_t *)buf)[i]) * 0x100000001b3ull;
        if (hash_on) fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\t%016llx\n", (now_ns() - g_t0_ns) / 1e9, (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf, (unsigned long long)fh);
        else fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\n", (now_ns() - g_t0_ns) / 1e9, (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf);
        pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) {   // 读取计量：每累计 64 MiB 或每 1000 次打一行 | Read metering: print a line every 64 MiB accumulated or every 1000 reads
        static uint64_t n, bytes, next_b = 64 << 20; static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&m); n++; bytes += got;
        if (bytes >= next_b || n % 1000 == 0) { LOG("fs 读 累计 %llu 次 %.1f MiB（本次 fd %d 偏移 %#llx 长 %#zx）", n, bytes / 1048576.0, (int)h, off, got); if (bytes >= next_b) next_b += 64 << 20; }
        pthread_mutex_unlock(&m);
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "fsp-srv → pwrite(2)", Result, h_fs_WriteFile, uint64_t h, int64_t off, const void *buf, size_t sz, const void *opt) {
    if (!h) return RES_FAIL;
    (void)opt; size_t put = 0;
    if (g_fsave[(int)h & 0xffff]) {                                     // 存档写注入点（确定性回归用；只数存档挂载上的写） | Save write injection point (for deterministic regression; only writes on the save mount are counted)
        static int cfg = -1, kill_n, fail_n, fail_e;
        if (cfg < 0) { const char *k = getenv("DM_TEST_KILL_AT"), *f = getenv("DM_TEST_FS_FAIL");
            kill_n = k && !strncmp(k, "write:", 6) ? atoi(k + 6) : 0;
            if (f && !strncmp(f, "write:", 6)) { fail_n = atoi(f + 6); const char *c = strchr(f + 6, ':'); fail_e = c ? atoi(c + 1) : ENOSPC; }
            cfg = 1; }
        int n = atomic_fetch_add(&g_save_writes, 1) + 1;
        { uint64_t z = 0; atomic_compare_exchange_strong(&g_txn_first_ns, &z, clock_gettime_nsec_np(CLOCK_MONOTONIC)); }
        if (kill_n && n == kill_n) { LOG("DM_TEST_KILL_AT：存档第 %d 次写，直接 _exit（模拟写到一半被杀）", n); _exit(9); }
        if (fail_n && n == fail_n) { LOG("DM_TEST_FS_FAIL：存档第 %d 次写返回失败 errno=%d", n, fail_e); errno = fail_e; save_poison(fail_e); return err_res("pwrite（注入）", "fd"); }
    }
    while (put < sz) {
        ssize_t r = pwrite((int)h, (const uint8_t *)buf + put, sz - put, off + put);
        if (r < 0) { if (errno == EINTR) continue; if (g_fsave[(int)h & 0xffff]) save_poison(errno); return err_res("pwrite", "fd"); }
        if (r == 0) { errno = ENOSPC; if (g_fsave[(int)h & 0xffff]) save_poison(ENOSPC); return err_res("pwrite（写入 0 字节）", "fd"); }   // 原先在此死循环 | This used to loop forever here
        put += r;
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", "fsp-srv → fstat(2)", Result, h_fs_GetFileSize, int64_t *out, uint64_t h) {
    if (!h) return RES_FAIL;
    struct stat st; if (fstat((int)h, &st)) return err_res("fstat", "fd");
    *out = (int)h < 4096 && g_hm[(int)h] ? g_hm[(int)h]->size : st.st_size; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", "fsp-srv → ftruncate(2)", Result, h_fs_SetFileSize, uint64_t h, int64_t sz) {
    if (!h) return RES_FAIL;
    if (!ftruncate((int)h, sz)) return RES_OK;
    if (g_fsave[(int)h & 0xffff]) save_poison(errno);
    return err_res("ftruncate", "fd");
}
HLE_FORCE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", "fsp-srv；宿主 write 已进页缓存，刷新为空操作", Result, h_fs_FlushFile, uint64_t h) { (void)h; return RES_OK; }
HLE_FORCE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", "fsp-srv → close(2)", void, h_fs_CloseFile, uint64_t h) {
    if (!h) return;  g_fmode[(int)h & 0xffff] = 0; g_fsave[(int)h & 0xffff] = 0;
    int d2s = g_fd2s[(int)h & 0xffff]; g_fd2s[(int)h & 0xffff] = 0;
    if ((int)h >= 0 && (int)h < 4096 && g_hm[(int)h]) { holemap *m = g_hm[(int)h]; g_hm[(int)h] = NULL; free(m->log); free(m->len); free(m->phys); free(m); }
    close((int)h); if (d2s) atomic_store(&g_d2s_wseq, dm_seq_next()); }   // 先关再取号：取号时内容已落到文件 | Close first, then take the number: by then the content is in the file
HLE_FORCE("_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", "fsp-srv；打开方式由宿主记录", int, h_fs_GetFileOpenMode, uint64_t h) { return g_fmode[(int)h & 0xffff]; }

// ---------- 路径操作 ----------
// ---------- Path operations ----------
HLE_FORCE("_ZN2nn2fs10CreateFileEPKcl", "fsp-srv → open(O_CREAT|O_EXCL)+ftruncate", Result, h_fs_CreateFile, const char *path, int64_t sz) {
    HP_EX(path, 1); RW_CREATE();
    int fd = open(hp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return err_res("create", hp);
    int rc = ftruncate(fd, sz); int e = errno; close(fd);
    if (!rc && set_guest_name(hp, path)) { rc = -1; e = errno; }
    if (rc) unlink(hp);
    errno = e; return rc ? err_res("create", hp) : RES_OK;
}
HLE_FORCE("_ZN2nn2fs10DeleteFileEPKc", "fsp-srv → unlink(2)", Result, h_fs_DeleteFile, const char *path) { HP(path); RW(); return unlink(hp) ? err_res("unlink", hp) : RES_OK; }
HLE_FORCE("_ZN2nn2fs15CreateDirectoryEPKc", "fsp-srv → mkdir(2)", Result, h_fs_CreateDirectory, const char *path) { HP_EX(path, 1); RW_CREATE();
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
    if (dm_fs_test_fail("guestname") || set_guest_name(hp2, to)) { int e = errno ? errno : EIO;
        if (dm_fs_test_fail("renameback") || rename(hp2, hp)) {   // 回退也失败：文件留在新路径、客体名元数据不对；不退出——在事务存档里就让本事务提交被拒（已提交版本不变，重新克隆即复原） | Rollback also failed: the file stays at the new path with wrong guest-name metadata; do not exit. In a transactional save, this transaction's commit is refused (the committed version is unchanged and recloning restores it)
            LOG("【红】fs 改名元数据失败且回退失败 errno=%d：%s 留在新路径%s", errno, hp2, in_txn_dir(hp2) ? "，本事务存档提交将被拒" : ""); if (in_txn_dir(hp2)) save_poison(e); }
        errno = e; return err_res("rename name", hp2); }
    if (is_d2s(to)) atomic_store(&g_d2s_wseq, dm_seq_next());
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", "fsp-srv → stat(2)", Result, h_fs_GetEntryType, int32_t *out, const char *path) {
    HP(path); struct stat st;
    if (stat(hp, &st)) return err_res("stat", hp);
    *out = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; return RES_OK;
}

// ---------- 目录 ----------
// ---------- Directories ----------
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
        if (!root_path(all[ri], rest, parent, 1024)) continue;
        DIR *x = opendir(parent); if (!x) continue;
        struct dirent *e;
        while ((e = readdir(x))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            { size_t dl = strlen(e->d_name); if (dl > 6 && !strcmp(e->d_name + dl - 6, ".holes")) continue; }   // 区段表不给游戏看 | The extent table is not shown to the game
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
        if (!stat(resolved, &st)) { h->snap[j].type = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; h->snap[j].size = S_ISDIR(st.st_mode) ? 0 : dm_data_logical_size(resolved, st.st_size); }
    }
    free(resolved); free(rel); free(guest); free(parent); free(all); free(names); free(roots);
}
HLE_FORCE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", "fsp-srv → opendir(3)", Result, h_fs_OpenDirectory, uint64_t *out, const char *path, int mode) {
    HP(path); DIR *d = opendir(hp);
    if (!d) return err_res("opendir", hp);
    dirh_t *h = calloc(1, sizeof *h); h->d = d; strlcpy(h->path, hp, sizeof h->path); h->mode = mode;
    const char *c = strchr(path, ':');
    if (c) for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].overlay[0] && strlen(g_mnt[i].name) == (size_t)(c - path) && !strncmp(g_mnt[i].name, path, (size_t)(c - path))) {
        dir_snapshot(h, &g_mnt[i], c + 1); break;
    }
    *out = (uint64_t)h; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", "fsp-srv → readdir(3)", Result, h_fs_ReadDirectory, int64_t *count, DirectoryEntry *ents, uint64_t hh, int64_t max) {
    if (!hh) { *count = 0; return RES_FAIL; }
    PATH_LOCK(); dirh_t *h = (dirh_t *)hh; int64_t n = 0; struct dirent *e;
    if (h->snap) { while (n < max && h->psnap < h->nsnap) { DirectoryEntry *src = &h->snap[h->psnap++]; if (!(h->mode & (src->type == ENT_DIR ? DIRMODE_DIR : DIRMODE_FILE))) continue; ents[n++] = *src; } *count = n; return RES_OK; }
    while (n < max && (e = readdir(h->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        { size_t dl = strlen(e->d_name); if (dl > 6 && !strcmp(e->d_name + dl - 6, ".holes")) continue; }   // 区段表不给游戏看 | The extent table is not shown to the game
        char c[1280]; snprintf(c, sizeof c, "%s/%s", h->path, e->d_name);
        struct stat st; if (stat(c, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (!(h->mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        DirectoryEntry *o = &ents[n++]; memset(o, 0, sizeof *o);
        if (!guest_name(h->path, e->d_name, o->name, sizeof o->name)) { *count = n - 1; return err_res("directory name", c); }
        o->type = isdir ? ENT_DIR : ENT_FILE; o->size = isdir ? 0 : dm_data_logical_size(c, st.st_size);
    }
    *count = n; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", "fsp-srv → closedir(3)", void, h_fs_CloseDirectory, uint64_t hh) {
    if (!hh) return;   // 游戏在打开失败后也会关（句柄是 0）：原版无事可做 | The game also closes after a failed open (handle 0): nothing to do in the original either
    dirh_t *h = (dirh_t *)hh; closedir(h->d); if (h->od) closedir(h->od); free(h->snap); free(h);
}
