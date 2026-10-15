#include "../host/hle.h"
#include "services_fs.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static void *force(const char *name) {
    extern const hle_force_ent __start_dm_force[], __stop_dm_force[];
    for (const hle_force_ent *entry = __start_dm_force; entry < __stop_dm_force; entry++)
        if (!strcmp(entry->name, name)) return entry->fn;
    die("Android filesystem probe entry missing: %s", name);
}
typedef Result (*query_fn)(size_t *);
typedef Result (*rom_fn)(const char *, void *, size_t);
typedef Result (*path_fn)(const char *);
typedef Result (*save_fn)(const char *, const void *);
typedef Result (*ensure_fn)(const void *);
typedef void (*unmount_fn)(const char *);
typedef Result (*open_fn)(uint64_t *, const char *, int);
typedef Result (*read_fn)(size_t *, uint64_t, int64_t, void *, size_t);
typedef Result (*write_fn)(uint64_t, int64_t, const void *, size_t, const void *);
typedef Result (*size_fn)(int64_t *, uint64_t);
typedef Result (*set_size_fn)(uint64_t, int64_t);
typedef Result (*file_fn)(uint64_t);
typedef void (*close_fn)(uint64_t);
typedef Result (*create_fn)(const char *, int64_t);
typedef Result (*rename_fn)(const char *, const char *);
typedef struct { char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } probe_entry;
typedef Result (*read_directory_fn)(int64_t *, probe_entry *, uint64_t, int64_t);

static int host_put(const char *path, const char *contents) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t length = strlen(contents), sent = 0;
    while (sent < length) {
        ssize_t n = write(fd, contents + sent, length - sent);
        if (n < 0) { if (errno == EINTR) continue; int error = errno; close(fd); errno = error; return -1; }
        if (!n) { close(fd); errno = ENOSPC; return -1; }
        sent += (size_t)n;
    }
    int rc = fsync(fd), error = errno;
    if (close(fd) && !rc) { rc = -1; error = errno; }
    errno = error; return rc;
}
static int host_equals(const char *path, const char *contents) {
    char buffer[128];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buffer, sizeof buffer);
    close(fd);
    return n == (ssize_t)strlen(contents) && !memcmp(buffer, contents, (size_t)n);
}

int dm_android_fs_verify(const char *fixture_parent, const char *result_json) {
    query_fn query = force("_ZN2nn2fs22QueryMountRomCacheSizeEPm");
    rom_fn mount_rom = force("_ZN2nn2fs8MountRomEPKcPvm");
    path_fn mount_temp = force("_ZN2nn2fs21MountTemporaryStorageEPKc");
    save_fn mount_save = force("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
    ensure_fn ensure = force("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE");
    path_fn commit = force("_ZN2nn2fs6CommitEPKc");
    unmount_fn unmount = force("_ZN2nn2fs7UnmountEPKc");
    open_fn open_file = force("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    read_fn read_file = force("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
    write_fn write_file = force("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
    size_fn file_size = force("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
    set_size_fn set_size = force("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl");
    file_fn flush = force("_ZN2nn2fs9FlushFileENS0_10FileHandleE");
    close_fn close_file = force("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    create_fn create_file = force("_ZN2nn2fs10CreateFileEPKcl");
    path_fn create_dir = force("_ZN2nn2fs15CreateDirectoryEPKc");
    rename_fn rename_file = force("_ZN2nn2fs10RenameFileEPKcS2_");
    open_fn open_dir = force("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
    read_directory_fn read_dir = force("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl");
    close_fn close_dir = force("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
    unsigned checks = 0, failures = 0;
    char *root = malloc(PATH_MAX), *rom = malloc(PATH_MAX), *save = malloc(PATH_MAX), *temporary = malloc(PATH_MAX), *path = malloc(PATH_MAX);
    if (!root || !rom || !save || !temporary || !path) die("Android filesystem fixture allocation");
    snprintf(root, PATH_MAX, "%s/fs-fixture-%d", fixture_parent, getpid());
    if (mkdir(root, 0700)) die("Android filesystem probe requires a fresh fixture directory: %s", root);
    snprintf(rom, PATH_MAX, "%s/rom", root);
    snprintf(save, PATH_MAX, "%s/save", root);
    snprintf(temporary, PATH_MAX, "%s/temporary", root);
#define CHECK(expression) do { ++checks; if (!(expression)) { ++failures; LOG("FS_PROBE_FAILED line=%d expression=%s errno=%d", __LINE__, #expression, errno); goto result; } } while (0)
    CHECK(!dm_android_fs_ensure_directory(rom) && !dm_android_fs_ensure_directory(save) && !dm_android_fs_ensure_directory(temporary));
    snprintf(path, PATH_MAX, "%s/sdk-fixture.txt", rom);
    CHECK(!host_put(path, "android-filesystem-fixture-v1"));
    snprintf(path, PATH_MAX, "%s/old.bin", save);
    CHECK(!host_put(path, "old committed fixture"));
    snprintf(path, PATH_MAX, "%s/.dm-case-1", save);
    CHECK(!host_put(path, "named attribute fixture"));
    CHECK(!lsetxattr(path, DM_ANDROID_GUEST_NAME_XATTR, "logicalAlias.bin", strlen("logicalAlias.bin"), 0));
    CHECK(!setenv("DM_ROMFS", rom, 1) && !setenv("DM_SAVE", save, 1) && !setenv("DM_TEMP", temporary, 1));
    CHECK(!unsetenv("DM_ROMFS_OVERLAY") && !unsetenv("DM_TEST_KILL_AT") && !unsetenv("DM_TEST_FS_FAIL"));
    CHECK(!dm_android_fs_initialize(NULL));
    size_t cache_size = 0;
    CHECK(!query(&cache_size) && cache_size >= 0x1000);
    void *cache = NULL;
    CHECK(!posix_memalign(&cache, 0x1000, cache_size));
    CHECK(!mount_rom("fsprobe_rom", cache, cache_size));
    uint64_t handle = 0;
    int64_t size = 0;
    char buffer[128] = {0}; size_t received = 0;
    CHECK(!open_file(&handle, "fsprobe_rom:/sdk-fixture.txt", 1));
    CHECK(!file_size(&size, handle) && size == (int64_t)strlen("android-filesystem-fixture-v1"));
    CHECK(!read_file(&received, handle, 8, buffer, 10) && received == 10 && !memcmp(buffer, "filesystem", 10));
    CHECK(!read_file(&received, handle, size + 32, buffer, sizeof buffer) && !received);
    close_file(handle);
    CHECK(open_file(&handle, "fsprobe_rom:/sdk-fixture.txt", 2) != 0);
    CHECK(open_file(&handle, "fsprobe_rom:/../save/old.bin", 1) != 0);
    uint64_t directory = 0; int64_t entries = 0; probe_entry entry;
    CHECK(!open_dir(&directory, "fsprobe_rom:/", 2));
    CHECK(!read_dir(&entries, &entry, directory, 1) && entries == 1 && !strcmp(entry.name, "sdk-fixture.txt"));
    close_dir(directory);
    CHECK(!mount_temp("fsprobe_temp"));
    CHECK(!create_dir("fsprobe_temp:/nested"));
    CHECK(!create_file("fsprobe_temp:/nested/data.bin", 0));
    CHECK(!open_file(&handle, "fsprobe_temp:/nested/data.bin", 3));
    CHECK(!set_size(handle, 17));
    CHECK(!write_file(handle, 0, "temporary fixture", 17, NULL));
    CHECK(!flush(handle));
    CHECK(commit("fsprobe_temp") == 0x327202u);
    close_file(handle);
    CHECK(!commit("fsprobe_temp"));
    CHECK(!rename_file("fsprobe_temp:/nested/data.bin", "fsprobe_temp:/nested/renamed.bin"));
    CHECK(!open_file(&handle, "fsprobe_temp:/nested/renamed.bin", 1));
    CHECK(!read_file(&received, handle, 0, buffer, sizeof buffer) && received == 17 && !memcmp(buffer, "temporary fixture", 17));
    close_file(handle);
    CHECK(!create_file("fsprobe_temp:/CaseFixture.bin", 1));
    CHECK(!create_file("fsprobe_temp:/casefixture.bin", 2));
    CHECK(!open_file(&handle, "fsprobe_temp:/CaseFixture.bin", 1) && !file_size(&size, handle) && size == 1);
    close_file(handle);
    CHECK(!open_file(&handle, "fsprobe_temp:/casefixture.bin", 1) && !file_size(&size, handle) && size == 2);
    close_file(handle);
    CHECK(!ensure(NULL) && !mount_save("fsprobe_save", NULL));
    CHECK(!open_file(&handle, "fsprobe_save:/logicalAlias.bin", 1));
    CHECK(!read_file(&received, handle, 0, buffer, sizeof buffer) && received == strlen("named attribute fixture") && !memcmp(buffer, "named attribute fixture", received));
    close_file(handle);
    CHECK(!open_file(&handle, "fsprobe_save:/old.bin", 2));
    CHECK(!set_size(handle, strlen("new committed fixture")));
    CHECK(!write_file(handle, 0, "new committed fixture", strlen("new committed fixture"), NULL));
    CHECK(!flush(handle) && commit("fsprobe_save") == 0x327202u);
    close_file(handle);
    snprintf(path, PATH_MAX, "%s/old.bin", save);
    CHECK(host_equals(path, "old committed fixture"));
    CHECK(!commit("fsprobe_save"));
    CHECK(host_equals(path, "new committed fixture"));
    unmount("fsprobe_save");
    CHECK(!mount_save("fsprobe_save", NULL));
    CHECK(!open_file(&handle, "fsprobe_save:/old.bin", 2));
    CHECK(!set_size(handle, strlen("uncommitted fixture")));
    CHECK(!write_file(handle, 0, "uncommitted fixture", strlen("uncommitted fixture"), NULL));
    close_file(handle);
    unmount("fsprobe_save");
    CHECK(!mount_save("fsprobe_save", NULL));
    CHECK(!open_file(&handle, "fsprobe_save:/old.bin", 1));
    CHECK(!read_file(&received, handle, 0, buffer, sizeof buffer) && received == strlen("new committed fixture") && !memcmp(buffer, "new committed fixture", received));
    // Real EBADF on a read-only save descriptor must poison the transaction.
    CHECK(write_file(handle, 0, "rejected fixture", strlen("rejected fixture"), NULL) != 0);
    close_file(handle);
    CHECK(commit("fsprobe_save") != 0);
    CHECK(host_equals(path, "new committed fixture"));
    CHECK(!open_file(&handle, "fsprobe_save:/old.bin", 2));
    CHECK(!set_size(handle, strlen("recovered fixture")));
    CHECK(!write_file(handle, 0, "recovered fixture", strlen("recovered fixture"), NULL));
    close_file(handle);
    CHECK(!commit("fsprobe_save") && host_equals(path, "recovered fixture"));
    snprintf(path, PATH_MAX, "%s.work/forbidden-link", save);
    CHECK(!symlink(rom, path));
    CHECK(commit("fsprobe_save") != 0); // Real snapshot refusal, not a fake write hook.
    CHECK(!unlink(path));
    snprintf(path, PATH_MAX, "%s/old.bin", save);
    CHECK(host_equals(path, "recovered fixture"));
    CHECK(!commit("fsprobe_save"));
    unmount("fsprobe_save");
    CHECK(!mount_save("fsprobe_save", NULL));
    CHECK(!open_file(&handle, "fsprobe_save:/logicalAlias.bin", 1));
    close_file(handle);
    unmount("fsprobe_save"); unmount("fsprobe_temp"); unmount("fsprobe_rom");
    free(cache);
result: {
        FILE *output = fopen(result_json, "w"); if (!output) die("Android filesystem result open");
        fprintf(output, "{\"passed\":%s,\"mode\":\"filesystem-fixture\",\"checks\":%u,\"failures\":%u,\"game_resources_used\":false,\"original_sdk_execution\":false,\"full_game_verified\":false,\"fixture\":\"%s\"}\n",
                failures ? "false" : "true", checks, failures, root);
        if (fclose(output)) die("Android filesystem result close");
        free(root); free(rom); free(save); free(temporary); free(path);
        return failures ? 1 : 0;
    }
#undef CHECK
}
