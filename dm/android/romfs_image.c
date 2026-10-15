#include "../host/dm.h"
#include "romfs_image.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define NONE UINT32_MAX
#define MAX_RECORDS 65536u
#define MAX_TABLE (16u * 1024u * 1024u)
typedef struct {
    uint32_t offset, parent, sibling, child_dir, child_file, hash_next;
    char *name, *path; int visited;
} image_dir;
typedef struct {
    uint32_t offset, parent, sibling, hash_next;
    uint64_t data, size; char *name, *path; int visited;
} image_file;
typedef struct {
    int fd; char image_path[PATH_MAX], root[1024]; struct stat identity;
    image_dir *directories; image_file *files; size_t ndir, nfile, open_files;
    _Atomic int invalid;
} image_context;
typedef struct { int peer; dev_t device; ino_t inode; size_t file; } range_handle;
static pthread_rwlock_t image_lock = PTHREAD_RWLOCK_INITIALIZER;
static image_context *image;
static range_handle *handles[65536];
_Static_assert(sizeof(romfs_image_entry) == 0x310, "DirectoryEntry ABI");

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static int exact_read(int fd, void *destination, size_t length, uint64_t offset) {
    if (offset > INT64_MAX || length > (uint64_t)INT64_MAX - offset) { errno = EINVAL; return -1; }
    size_t done = 0;
    while (done < length) {
        ssize_t n = pread(fd, (char *)destination + done, length - done, (off_t)(offset + done));
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (!n) { errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}
static int same_file(const struct stat *a, const struct stat *b) {
    return S_ISREG(b->st_mode) && a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static int validate_image(image_context *context) {
    struct stat fd_state, path_state;
    if (atomic_load(&context->invalid)) { errno = EIO; return -1; }
    if (fstat(context->fd, &fd_state) || lstat(context->image_path, &path_state) ||
        !same_file(&context->identity, &fd_state) || !same_file(&context->identity, &path_state)) {
        atomic_store(&context->invalid, 1); errno = EIO; return -1;
    }
    return 0;
}
static char *record_name(const uint8_t *p, size_t size, int root) {
    if ((!size && !root) || size > 0x300 || memchr(p, 0, size) || memchr(p, '/', size) || memchr(p, '\\', size)) { errno = EINVAL; return NULL; }
    char *name = malloc(size + 1); if (!name) return NULL;
    memcpy(name, p, size); name[size] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) { free(name); errno = EINVAL; return NULL; }
    return name;
}
static image_dir *directory_at(image_context *context, uint32_t offset) {
    size_t low = 0, high = context->ndir;
    while (low < high) { size_t middle = low + (high - low) / 2;
        if (context->directories[middle].offset < offset) low = middle + 1; else high = middle; }
    if (low < context->ndir && context->directories[low].offset == offset) return &context->directories[low];
    errno = EINVAL; return NULL;
}
static image_file *file_at(image_context *context, uint32_t offset) {
    size_t low = 0, high = context->nfile;
    while (low < high) { size_t middle = low + (high - low) / 2;
        if (context->files[middle].offset < offset) low = middle + 1; else high = middle; }
    if (low < context->nfile && context->files[low].offset == offset) return &context->files[low];
    errno = EINVAL; return NULL;
}
static char *join(const char *parent, const char *name) {
    size_t a = strlen(parent), b = strlen(name);
    if (a + b + (a && b ? 1 : 0) >= 1024) { errno = ENAMETOOLONG; return NULL; }
    char *path = malloc(a + b + 2); if (!path) return NULL;
    snprintf(path, a + b + 2, "%s%s%s", parent, a && b ? "/" : "", name);
    return path;
}
static int walk_directory(image_context *context, image_dir *dir, const char *parent_path, unsigned depth) {
    if (depth > 32 || dir->visited) { errno = ELOOP; return -1; }
    dir->visited = 1; dir->path = join(parent_path, dir->name);
    if (!dir->path) return -1;
    for (uint32_t child = dir->child_file; child != NONE;) {
        image_file *file = file_at(context, child);
        if (!file || file->visited || file->parent != dir->offset) { errno = EINVAL; return -1; }
        file->visited = 1; file->path = join(dir->path, file->name);
        if (!file->path) return -1;
        child = file->sibling;
    }
    for (uint32_t child = dir->child_dir; child != NONE;) {
        image_dir *nested = directory_at(context, child);
        if (!nested || nested->parent != dir->offset || walk_directory(context, nested, dir->path, depth + 1)) { errno = EINVAL; return -1; }
        child = nested->sibling;
    }
    return 0;
}
static void free_context(image_context *context) {
    if (!context) return;
    if (context->fd >= 0) close(context->fd);
    for (size_t i = 0; i < context->ndir; i++) { free(context->directories[i].name); free(context->directories[i].path); }
    for (size_t i = 0; i < context->nfile; i++) { free(context->files[i].name); free(context->files[i].path); }
    free(context->directories); free(context->files); free(context);
}
static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t hash = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) hash = ((hash >> 5) | (hash << 27)) ^ *p;
    return hash;
}
static int validate_hashes(image_context *context, uint64_t offset, uint64_t bytes, int directories) {
    if (!bytes || bytes % 4 || bytes > MAX_TABLE) { errno = EINVAL; return -1; }
    size_t count = directories ? context->ndir : context->nfile, buckets = (size_t)bytes / 4;
    uint8_t *table = malloc((size_t)bytes), *visited = calloc(count ? count : 1, 1);
    if (!table || !visited) { free(table); free(visited); return -1; }
    int result = -1;
    if (exact_read(context->fd, table, (size_t)bytes, offset)) goto done;
    for (size_t bucket = 0; bucket < buckets; bucket++) {
        for (uint32_t current = u32(table + bucket * 4); current != NONE;) {
            uint32_t parent, next; const char *name; size_t index;
            if (directories) {
                image_dir *entry = directory_at(context, current); if (!entry) goto done;
                index = (size_t)(entry - context->directories); parent = entry->parent; next = entry->hash_next; name = entry->name;
            } else {
                image_file *entry = file_at(context, current); if (!entry) goto done;
                index = (size_t)(entry - context->files); parent = entry->parent; next = entry->hash_next; name = entry->name;
            }
            if (visited[index] || name_hash(parent, name) % buckets != bucket) { errno = EINVAL; goto done; }
            visited[index] = 1; current = next;
        }
    }
    for (size_t i = 0; i < count; i++) if (!visited[i]) { errno = EINVAL; goto done; }
    result = 0;
done: { int error = errno; free(table); free(visited); errno = error; return result; }
}
static int parse_image(image_context *context) {
    uint8_t header[80]; if (exact_read(context->fd, header, sizeof header, 0)) return -1;
    uint64_t h[10]; for (size_t i = 0; i < 10; i++) h[i] = u64(header + i * 8);
    uint64_t total = (uint64_t)context->identity.st_size;
    if (h[0] != 80 || h[1] < h[0] || h[3] < h[1] || h[5] < h[3] || h[7] < h[5] || h[9] > total ||
        h[2] > h[3] - h[1] || h[4] > h[5] - h[3] || h[6] > h[7] - h[5] || h[7] > h[9] || h[8] > h[9] - h[7] ||
        h[4] < 24 || h[4] > MAX_TABLE || h[8] > MAX_TABLE || h[2] > MAX_TABLE || h[6] > MAX_TABLE ||
        !h[2] || !h[6] || h[2] % 4 || h[6] % 4 || (h[8] && h[8] < 32)) { errno = EINVAL; return -1; }
    uint8_t *dirs = malloc((size_t)h[4]), *files = malloc(h[8] ? (size_t)h[8] : 1);
    if (!dirs || !files) { free(dirs); free(files); return -1; }
    int rc = -1;
    if (exact_read(context->fd, dirs, (size_t)h[4], h[3]) || exact_read(context->fd, files, (size_t)h[8], h[7])) goto done;
    context->directories = calloc((size_t)h[4] / 24, sizeof *context->directories);
    context->files = calloc(h[8] ? (size_t)h[8] / 32 : 1, sizeof *context->files);
    if (!context->directories || !context->files) goto done;
    for (uint64_t position = 0; position < h[4];) {
        if (h[4] - position < 24 || context->ndir == MAX_RECORDS) { errno = EINVAL; goto done; }
        const uint8_t *p = dirs + position;
        uint32_t length = u32(p + 20); uint64_t step = 24 + (((uint64_t)length + 3) & ~3ull);
        if (step > h[4] - position) { errno = EINVAL; goto done; }
        image_dir *dir = &context->directories[context->ndir++];
        dir->offset = (uint32_t)position; dir->parent = u32(p); dir->sibling = u32(p + 4);
        dir->child_dir = u32(p + 8); dir->child_file = u32(p + 12); dir->hash_next = u32(p + 16);
        dir->name = record_name(p + 24, length, position == 0);
        if (!dir->name) goto done;
        position += step;
    }
    for (uint64_t position = 0; position < h[8];) {
        if (h[8] - position < 32 || context->nfile == MAX_RECORDS) { errno = EINVAL; goto done; }
        const uint8_t *p = files + position;
        uint32_t length = u32(p + 28); uint64_t step = 32 + (((uint64_t)length + 3) & ~3ull);
        if (step > h[8] - position) { errno = EINVAL; goto done; }
        image_file *file = &context->files[context->nfile++];
        file->offset = (uint32_t)position; file->parent = u32(p); file->sibling = u32(p + 4); file->hash_next = u32(p + 24);
        uint64_t relative = u64(p + 8); file->size = u64(p + 16);
        if (relative > total - h[9] || file->size > total - h[9] - relative) { errno = EINVAL; goto done; }
        file->data = h[9] + relative;
        file->name = record_name(p + 32, length, 0); if (!file->name) goto done;
        position += step;
    }
    if (context->directories[0].parent || context->directories[0].sibling != NONE || *context->directories[0].name ||
        walk_directory(context, &context->directories[0], "", 0)) { errno = EINVAL; goto done; }
    for (size_t i = 0; i < context->ndir; i++) {
        image_dir *dir = &context->directories[i];
        if (!dir->visited || (dir->hash_next != NONE && !directory_at(context, dir->hash_next))) { errno = EINVAL; goto done; }
        for (size_t j = 0; j < i; j++) if (!strcmp(dir->path, context->directories[j].path)) { errno = EINVAL; goto done; }
    }
    for (size_t i = 0; i < context->nfile; i++) {
        image_file *file = &context->files[i];
        if (!file->visited || (file->hash_next != NONE && !file_at(context, file->hash_next))) { errno = EINVAL; goto done; }
        for (size_t j = 0; j < i; j++) if (!strcmp(file->path, context->files[j].path)) { errno = EINVAL; goto done; }
        for (size_t j = 0; j < context->ndir; j++) if (!strcmp(file->path, context->directories[j].path)) { errno = EINVAL; goto done; }
    }
    if (validate_hashes(context, h[1], h[2], 1) || validate_hashes(context, h[5], h[6], 0)) goto done;
    rc = validate_image(context);
done: { int error = errno; free(dirs); free(files); errno = error; return rc; }
}
static const char *relative_path(image_context *context, const char *path) {
    size_t length = strlen(context->root);
    if (strncmp(context->root, path, length) || (path[length] && path[length] != '/')) return NULL;
    const char *relative = path + length; if (*relative == '/') relative++;
    for (const char *p = relative; *p;) {
        size_t n = strcspn(p, "/");
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.') || memchr(p, '\\', n)) { errno = EINVAL; return (const char *)-1; }
        p += n; if (*p) p++;
    }
    return relative;
}
static int find_path(image_context *context, const char *path, size_t *index) {
    for (size_t i = 0; i < context->ndir; i++) if (!strcmp(path, context->directories[i].path)) { *index = i; return 0; }
    for (size_t i = 0; i < context->nfile; i++) if (!strcmp(path, context->files[i].path)) { *index = i; return 1; }
    errno = ENOENT; return -1;
}
static int make_directory_tree(int root, const char *relative) {
    char *path = strdup(relative); if (!path) return -1;
    int parent = dup(root); if (parent < 0) { free(path); return -1; }
    char *save;
    int result = 0;
    for (char *part = strtok_r(path, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (mkdirat(parent, part, 0700) && errno != EEXIST) { result = -1; break; }
        int child = openat(parent, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child < 0) { result = -1; break; }
        close(parent); parent = child;
    }
    int error = errno; close(parent); free(path); errno = error; return result;
}
int romfs_image_setup_checked(const char *source, const char *root, uint64_t expected_bytes) {
    if (!source || !root || source[0] != '/' || root[0] != '/' || strlen(source) >= PATH_MAX || strlen(root) >= 1024) { errno = EINVAL; return -1; }
    pthread_rwlock_wrlock(&image_lock);
    if (image) { pthread_rwlock_unlock(&image_lock); errno = EBUSY; return -1; }
    image_context *context = calloc(1, sizeof *context); if (!context) { pthread_rwlock_unlock(&image_lock); return -1; }
    context->fd = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    strlcpy(context->image_path, source, sizeof context->image_path);
    strlcpy(context->root, root, sizeof context->root);
    size_t length = strlen(context->root); while (length > 1 && context->root[length - 1] == '/') context->root[--length] = 0;
    int rc = -1;
    if (context->fd < 0 || fstat(context->fd, &context->identity)) goto done;
    if (!S_ISREG(context->identity.st_mode) || context->identity.st_size < 80 || (expected_bytes && (uint64_t)context->identity.st_size != expected_bytes)) { errno = EINVAL; goto done; }
    if (parse_image(context)) goto done;
    if (mkdir(context->root, 0700) && errno != EEXIST) goto done;
    int root_fd = open(context->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) goto done;
    for (size_t i = 0; i < context->ndir; i++) {
        if (make_directory_tree(root_fd, context->directories[i].path)) { int error = errno; close(root_fd); errno = error; goto done; }
    }
    close(root_fd);
    image = context; context = NULL; rc = 0;
done: { int error = errno; free_context(context); pthread_rwlock_unlock(&image_lock); errno = error; return rc; }
}
int romfs_image_setup(const char *source, const char *root) { return romfs_image_setup_checked(source, root, 0); }
int romfs_image_shutdown(void) {
    pthread_rwlock_wrlock(&image_lock);
    if (image && image->open_files) { pthread_rwlock_unlock(&image_lock); errno = EBUSY; return -1; }
    free_context(image); image = NULL;
    pthread_rwlock_unlock(&image_lock); return 0;
}
int romfs_image_view_root(const char *path) {
    pthread_rwlock_rdlock(&image_lock);
    int result = image && relative_path(image, path) != NULL;
    pthread_rwlock_unlock(&image_lock); return result;
}
int romfs_image_lookup(const char *path, int *type, int64_t *size) {
    pthread_rwlock_rdlock(&image_lock);
    const char *relative = image ? relative_path(image, path) : NULL;
    if (!relative) { pthread_rwlock_unlock(&image_lock); return 0; }
    size_t index; int result = relative == (const char *)-1 || validate_image(image) ? -1 : find_path(image, relative, &index);
    if (result >= 0) { if (type) *type = result; if (size) *size = result ? (int64_t)image->files[index].size : 0; result = 1; }
    pthread_rwlock_unlock(&image_lock); return result;
}
int romfs_image_list(const char *path, romfs_image_entry **out, size_t *count) {
    if (!path || !out || !count) { errno = EINVAL; return -1; }
    *out = NULL; *count = 0;
    pthread_rwlock_rdlock(&image_lock);
    const char *relative = image ? relative_path(image, path) : NULL;
    if (!relative) { pthread_rwlock_unlock(&image_lock); return 0; }
    size_t index;
    if (relative == (const char *)-1 || validate_image(image)) { pthread_rwlock_unlock(&image_lock); return -1; }
    int kind = find_path(image, relative, &index);
    if (kind != 0) { pthread_rwlock_unlock(&image_lock); if (kind == 1) errno = ENOTDIR; return -1; }
    uint32_t parent = image->directories[index].offset;
    size_t n = 0;
    for (size_t i = 1; i < image->ndir; i++) n += image->directories[i].parent == parent;
    for (size_t i = 0; i < image->nfile; i++) n += image->files[i].parent == parent;
    romfs_image_entry *entries = calloc(n ? n : 1, sizeof *entries);
    if (!entries) { pthread_rwlock_unlock(&image_lock); return -1; }
    size_t position = 0;
    for (size_t i = 1; i < image->ndir; i++) if (image->directories[i].parent == parent) strlcpy(entries[position++].name, image->directories[i].name, sizeof entries[0].name);
    for (size_t i = 0; i < image->nfile; i++) if (image->files[i].parent == parent) {
        strlcpy(entries[position].name, image->files[i].name, sizeof entries[0].name);
        entries[position].type = 1; entries[position++].size = (int64_t)image->files[i].size;
    }
    *out = entries; *count = n;
    pthread_rwlock_unlock(&image_lock); return 1;
}
static int handle_valid(int fd, const range_handle *handle) {
    struct stat st;
    if (fstat(fd, &st) || st.st_dev != handle->device || st.st_ino != handle->inode || !S_ISSOCK(st.st_mode)) { errno = EBADF; return -1; }
    return 0;
}
int romfs_image_open(const char *path, int mode, int *out) {
    if (!path || !out) { errno = EINVAL; return -1; }
    pthread_rwlock_wrlock(&image_lock);
    const char *relative = image ? relative_path(image, path) : NULL;
    if (!relative) { pthread_rwlock_unlock(&image_lock); return 0; }
    size_t index;
    if (relative == (const char *)-1 || validate_image(image)) { pthread_rwlock_unlock(&image_lock); return -1; }
    int kind = find_path(image, relative, &index);
    if (kind != 1) { pthread_rwlock_unlock(&image_lock); if (kind == 0) errno = EISDIR; return -1; }
    if (mode != 1) { pthread_rwlock_unlock(&image_lock); errno = EROFS; return -1; }
    int token[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, token)) { pthread_rwlock_unlock(&image_lock); return -1; }
    struct stat st;
    range_handle *handle = malloc(sizeof *handle);
    if (!handle || token[0] >= 65536 || fstat(token[0], &st)) {
        int error = token[0] >= 65536 ? EMFILE : errno; free(handle); close(token[0]); close(token[1]); pthread_rwlock_unlock(&image_lock); errno = error; return -1;
    }
    if (handles[token[0]]) { close(handles[token[0]]->peer); free(handles[token[0]]); image->open_files--; }
    *handle = (range_handle){.peer = token[1], .device = st.st_dev, .inode = st.st_ino, .file = index};
    handles[token[0]] = handle; image->open_files++; *out = token[0];
    pthread_rwlock_unlock(&image_lock); return 1;
}
int romfs_image_note_fd(int fd, const char *path) {
    pthread_rwlock_rdlock(&image_lock);
    int result = fd >= 0 && fd < 65536 && handles[fd] ? (handle_valid(fd, handles[fd]) ? -1 : 1) : 0;
    if (!result && image && relative_path(image, path)) { result = -1; errno = EBADF; }
    pthread_rwlock_unlock(&image_lock); return result;
}
int romfs_image_pread(int fd, void *buffer, size_t length, int64_t offset, ssize_t *out) {
    if (!out) { errno = EINVAL; return -1; }
    pthread_rwlock_rdlock(&image_lock);
    range_handle *handle = fd >= 0 && fd < 65536 ? handles[fd] : NULL;
    if (!handle) { pthread_rwlock_unlock(&image_lock); return 0; }
    int result = -1;
    if (handle_valid(fd, handle) || validate_image(image)) goto done;
    if (offset < 0 || length > SSIZE_MAX || (!buffer && length)) { errno = EINVAL; goto done; }
    image_file *file = &image->files[handle->file];
    if ((uint64_t)offset >= file->size) length = 0;
    else if (length > file->size - (uint64_t)offset) length = (size_t)(file->size - (uint64_t)offset);
    if (length && exact_read(image->fd, buffer, length, file->data + (uint64_t)offset)) { atomic_store(&image->invalid, 1); goto done; }
    if (validate_image(image)) goto done;
    *out = (ssize_t)length; result = 1;
done: pthread_rwlock_unlock(&image_lock); return result;
}
int romfs_image_fd_size(int fd, int64_t *size) {
    if (!size) { errno = EINVAL; return -1; }
    pthread_rwlock_rdlock(&image_lock);
    range_handle *handle = fd >= 0 && fd < 65536 ? handles[fd] : NULL;
    int result = !handle ? 0 : handle_valid(fd, handle) || validate_image(image) ? -1 : 1;
    if (result == 1) *size = (int64_t)image->files[handle->file].size;
    pthread_rwlock_unlock(&image_lock); return result;
}
int romfs_image_close_fd(int fd) {
    pthread_rwlock_wrlock(&image_lock);
    range_handle *handle = fd >= 0 && fd < 65536 ? handles[fd] : NULL;
    if (!handle) { pthread_rwlock_unlock(&image_lock); return 0; }
    if (!handle_valid(fd, handle)) close(fd);
    close(handle->peer); free(handle); handles[fd] = NULL; image->open_files--;
    pthread_rwlock_unlock(&image_lock); return 1;
}
