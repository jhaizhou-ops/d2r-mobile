#include "../host/dm.h"
#include "services_fs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1u << 1)
#endif

static int open_parent(const char *path, char *leaf, size_t capacity) {
    if (!path || path[0] != '/') { errno = EINVAL; return -1; }
    char *parent = strdup(path);
    if (!parent) return -1;
    size_t length = strlen(parent);
    while (length > 1 && parent[length - 1] == '/') parent[--length] = 0;
    char *slash = strrchr(parent, '/');
    if (!slash || !slash[1] || !strcmp(slash + 1, ".") || !strcmp(slash + 1, "..") ||
        strlen(slash + 1) >= capacity) {
        free(parent); errno = EINVAL; return -1;
    }
    strlcpy(leaf, slash + 1, capacity);
    if (slash == parent) slash[1] = 0; else *slash = 0;
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int error = errno;
    free(parent); errno = error;
    return fd;
}

static int remove_at(int parent, const char *name, unsigned depth) {
    if (depth > 32) { errno = ELOOP; return -1; }
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) return -1;
    if (!S_ISDIR(st.st_mode)) return unlinkat(parent, name, 0);
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    DIR *dir = fdopendir(fd);
    if (!dir) { int error = errno; close(fd); errno = error; return -1; }
    struct dirent *entry;
    int rc = 0;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) rc = -1; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (remove_at(fd, entry->d_name, depth + 1)) { rc = -1; break; }
    }
    int error = errno;
    if (closedir(dir) && !rc) { rc = -1; error = errno; }
    if (!rc && unlinkat(parent, name, AT_REMOVEDIR)) { rc = -1; error = errno; }
    errno = error; return rc;
}

int dm_android_fs_remove_tree(const char *path) {
    char leaf[NAME_MAX + 1];
    int parent = open_parent(path, leaf, sizeof leaf);
    if (parent < 0) return -1;
    int rc = remove_at(parent, leaf, 0);
    int error = errno;
    close(parent); errno = error;
    return rc;
}

static int copy_guest_name(int source, int destination) {
    ssize_t size = fgetxattr(source, DM_ANDROID_GUEST_NAME_XATTR, NULL, 0);
    if (size < 0) {
        if (errno == ENODATA || errno == ENOTSUP || errno == EOPNOTSUPP) return 0;
        return -1;
    }
    if (size > 0x300) { errno = EINVAL; return -1; }
    char name[0x301];
    if (fgetxattr(source, DM_ANDROID_GUEST_NAME_XATTR, name, sizeof name) != size) return -1;
    return fsetxattr(destination, DM_ANDROID_GUEST_NAME_XATTR, name, (size_t)size, 0);
}

static int copy_file(int source_parent, int destination_parent, const char *name,
                     uint8_t *buffer, size_t capacity) {
    int source = openat(source_parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) return -1;
    struct stat st;
    int destination = -1, rc = -1;
    if (fstat(source, &st) || !S_ISREG(st.st_mode)) {
        if (!errno) errno = EINVAL;
        goto done;
    }
    destination = openat(destination_parent, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (destination < 0) goto done;
    for (;;) {
        ssize_t received = read(source, buffer, capacity);
        if (received < 0) { if (errno == EINTR) continue; goto done; }
        if (!received) break;
        size_t sent = 0;
        while (sent < (size_t)received) {
            ssize_t put = write(destination, buffer + sent, (size_t)received - sent);
            if (put < 0) { if (errno == EINTR) continue; goto done; }
            if (!put) { errno = ENOSPC; goto done; }
            sent += (size_t)put;
        }
    }
    if (copy_guest_name(source, destination) || fchmod(destination, st.st_mode & 0777)) goto done;
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (futimens(destination, times) || fsync(destination)) goto done;
    rc = 0;
done: {
        int error = errno;
        if (destination >= 0 && close(destination) && !rc) { rc = -1; error = errno; }
        close(source); errno = error;
        return rc;
    }
}

static int copy_directory(int source, int destination, uint8_t *buffer, size_t capacity, unsigned depth) {
    if (depth > 32) { errno = ELOOP; return -1; }
    int iteration_fd = dup(source);
    if (iteration_fd < 0) return -1;
    DIR *dir = fdopendir(iteration_fd);
    if (!dir) { int error = errno; close(iteration_fd); errno = error; return -1; }
    int rc = -1;
    struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) goto done; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(source, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) goto done;
        if (S_ISDIR(st.st_mode)) {
            if (mkdirat(destination, entry->d_name, 0700)) goto done;
            int src = openat(source, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            int dst = openat(destination, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            int error = errno;
            if (src < 0 || dst < 0) {
                if (src >= 0) close(src);
                if (dst >= 0) close(dst);
                errno = error; goto done;
            }
            int child = copy_directory(src, dst, buffer, capacity, depth + 1);
            error = errno;
            close(src);
            if (close(dst) && !child) { child = -1; error = errno; }
            errno = error;
            if (child) goto done;
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file(source, destination, entry->d_name, buffer, capacity)) goto done;
        } else { errno = EINVAL; goto done; } // No links/devices can escape the owned tree.
    }
    struct stat st;
    if (fstat(source, &st) || copy_guest_name(source, destination) || fchmod(destination, st.st_mode & 0777)) goto done;
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (futimens(destination, times) || fsync(destination)) goto done;
    rc = 0;
done: {
        int error = errno;
        if (closedir(dir) && !rc) { rc = -1; error = errno; }
        errno = error; return rc;
    }
}

int dm_android_fs_clone_tree(const char *source, const char *destination) {
    char leaf[NAME_MAX + 1];
    int parent = open_parent(destination, leaf, sizeof leaf);
    if (parent < 0) return -1;
    int source_fd = open(source, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0) { int error = errno; close(parent); errno = error; return -1; }
    int rc = -1, destination_fd = -1, created = 0;
    uint8_t *buffer = malloc(64 * 1024);
    if (!buffer) goto done;
    if (mkdirat(parent, leaf, 0700)) goto done;
    created = 1;
    destination_fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (destination_fd < 0) goto done;
    if (copy_directory(source_fd, destination_fd, buffer, 64 * 1024, 0) || fsync(parent)) goto done;
    rc = 0;
done: {
        int error = errno;
        free(buffer);
        if (destination_fd >= 0 && close(destination_fd) && !rc) { rc = -1; error = errno; }
        close(source_fd);
        if (rc && created && remove_at(parent, leaf, 0))
            LOG("Android save snapshot cleanup failed: %s (%d)", destination, errno);
        close(parent); errno = error;
        return rc;
    }
}

int dm_android_fs_commit_snapshot(const char *work, const char *committed) {
    char *snapshot = malloc(strlen(committed) + 5);
    if (!snapshot) return -1;
    sprintf(snapshot, "%s.new", committed);
    struct stat st;
    if (!lstat(snapshot, &st)) {
        if (dm_android_fs_remove_tree(snapshot)) { free(snapshot); return -1; }
    } else if (errno != ENOENT) { free(snapshot); return -1; }
    if (dm_android_fs_clone_tree(work, snapshot)) { free(snapshot); return -1; }
    char old_leaf[NAME_MAX + 1], new_leaf[NAME_MAX + 1];
    int parent = open_parent(committed, old_leaf, sizeof old_leaf);
    if (parent < 0) { free(snapshot); return -1; }
    if (snprintf(new_leaf, sizeof new_leaf, "%s.new", old_leaf) >= (int)sizeof new_leaf) {
        close(parent); free(snapshot); errno = ENAMETOOLONG; return -1;
    }
    int rc = -1;
    if (fstatat(parent, old_leaf, &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode)) { errno = EINVAL; goto done; }
    if (syscall(__NR_renameat2, parent, new_leaf, parent, old_leaf, RENAME_EXCHANGE)) goto done;
    if (fsync(parent)) {
        int error = errno;
        // Preserve the old committed generation if durability cannot be proven.
        // Catastrophic rollback failure leaves both complete trees for recovery.
        if (syscall(__NR_renameat2, parent, new_leaf, parent, old_leaf, RENAME_EXCHANGE))
            die("Android save atomic rollback failed; preserve %s and %s (errno=%d)", committed, snapshot, errno);
        if (fsync(parent))
            die("Android save rollback durability failed; preserve %s and %s (errno=%d)", committed, snapshot, errno);
        errno = error; goto done;
    }
    rc = 0;
    if (remove_at(parent, new_leaf, 0) || fsync(parent))
        LOG("Android save committed, old generation cleanup deferred: %s (%d)", snapshot, errno);
done: {
        int error = errno;
        close(parent); free(snapshot); errno = error;
        return rc;
    }
}

int dm_android_fs_ensure_directory(const char *path) {
    if (!path || path[0] != '/') { errno = EINVAL; return -1; }
    /* TEST PATCH (not in dodola's tree): 0775 instead of 0700 so the diagnostics stay readable
       from adb shell; storage permissions only, no behavioural change. */
    if (mkdir(path, 0775) && errno != EEXIST) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = fsync(fd), error = errno;
    close(fd);
    if (!rc) {
        char leaf[NAME_MAX + 1];
        int parent = open_parent(path, leaf, sizeof leaf);
        if (parent < 0) { rc = -1; error = errno; }
        else {
            rc = fsync(parent); error = errno;
            close(parent);
        }
    }
    errno = error;
    return rc;
}

static int sync_directory(int fd, unsigned depth) {
    if (depth > 32) { errno = ELOOP; return -1; }
    int iteration_fd = dup(fd);
    if (iteration_fd < 0) return -1;
    DIR *dir = fdopendir(iteration_fd);
    if (!dir) { int error = errno; close(iteration_fd); errno = error; return -1; }
    int rc = -1;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) goto done; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW)) goto done;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) { errno = EINVAL; goto done; }
        int child = openat(fd, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (S_ISDIR(st.st_mode) ? O_DIRECTORY : 0));
        if (child < 0) goto done;
        int child_rc = S_ISDIR(st.st_mode) ? sync_directory(child, depth + 1) : fsync(child);
        int error = errno;
        close(child); errno = error;
        if (child_rc) goto done;
    }
    rc = fsync(fd);
done: {
        int error = errno;
        closedir(dir); errno = error;
        return rc;
    }
}

int dm_android_fs_sync_tree(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = sync_directory(fd, 0), error = errno;
    close(fd); errno = error;
    return rc;
}

int dm_android_fs_initialize(void *context) {
    (void)context;
    const char *rom = getenv("DM_ROMFS"), *save = getenv("DM_SAVE"), *temporary = getenv("DM_TEMP");
    if (!rom || !save || !temporary) return EINVAL;
    int rom_fd = open(rom, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (rom_fd < 0) return errno;
    close(rom_fd);
    if (dm_android_fs_ensure_directory(save) || dm_android_fs_ensure_directory(temporary)) return errno;
    return 0;
}
