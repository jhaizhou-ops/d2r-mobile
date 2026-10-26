// Win32 运行时适配：内存、UTF-8 文件、线程槽与异常，依据公开 Windows API。
// Win32 runtime adaptation: memory, UTF-8 files, thread slots and exceptions, based on public Windows APIs.
#define DM_WINDOWS_IMPL
#include "dm_windows.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/xattr.h>
#include <sys/clonefile.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <direct.h>
#include <bcrypt.h>
#include <ktmw32.h>

#define GUEST_KEY 0x80000000u
#define TEB_TLS_SLOTS 0x1480

int dm_windows_errno(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: errno = ENOMEM; break;
    case ERROR_DIR_NOT_EMPTY: errno = ENOTEMPTY; break;
    case ERROR_NOT_SUPPORTED: case ERROR_CALL_NOT_IMPLEMENTED: errno = ENOTSUP; break;
    case ERROR_INVALID_HANDLE: errno = EBADF; break;
    case ERROR_INVALID_PARAMETER: errno = EINVAL; break;
    default: errno = EIO; break;
    }
    return -1;
}
wchar_t *dm_windows_wide(const char *text) {
    if (!text) { errno = EINVAL; return NULL; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!n) { errno = EILSEQ; return NULL; }
    wchar_t *wide = malloc((size_t)n * sizeof *wide);
    if (!wide) { errno = ENOMEM; return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n);
    return wide;
}
int dm_windows_guest_key_create(pthread_key_t *key) {
    DWORD k = TlsAlloc();
    if (k == TLS_OUT_OF_INDEXES) return EAGAIN;
    if (k >= 64) { TlsFree(k); return EAGAIN; }
    *key = GUEST_KEY | k;
    return 0;
}
void *dm_windows_getspecific(pthread_key_t key) {
    return key & GUEST_KEY ? TlsGetValue(key & ~GUEST_KEY) : pthread_getspecific(key);
}
int dm_windows_setspecific(pthread_key_t key, const void *value) {
    return key & GUEST_KEY ? (TlsSetValue(key & ~GUEST_KEY, (void *)value) ? 0 : EINVAL) : pthread_setspecific(key, value);
}
uint8_t *dm_windows_tsd_base(void) {
    uint8_t *teb;
    __asm__ volatile("mov %0, x18" : "=r"(teb));
    return teb;
}
uint64_t dm_windows_slot_offset(pthread_key_t key) {
    return TEB_TLS_SLOTS + (uint64_t)(key & ~GUEST_KEY) * sizeof(void *);
}
int dm_windows_tsd_check(pthread_key_t key) {
    const void *magic = (void *)(uintptr_t)0x123456789abcdef0ull;
    void *old = dm_windows_getspecific(key);
    int error = dm_windows_setspecific(key, magic);
    int matches = !error && *(void **)(dm_windows_tsd_base() + dm_windows_slot_offset(key)) == magic;
    dm_windows_setspecific(key, old);
    return matches;
}
uint64_t clock_gettime_nsec_np(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts)) { fprintf(stderr, "[dm] clock_gettime failed: %d\n", errno); abort(); }
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) { size_t k = n < size - 1 ? n : size - 1; memcpy(dst, src, k); dst[k] = 0; }
    return n;
}
size_t strlcat(char *dst, const char *src, size_t size) {
    size_t n = strnlen(dst, size);
    return n + strlcpy(dst + n, src, size - n);
}
int getpagesize(void) { SYSTEM_INFO info; GetSystemInfo(&info); return (int)info.dwPageSize; }
void arc4random_buf(void *buffer, size_t size) {
    while (size) {
        ULONG n = size > ULONG_MAX ? ULONG_MAX : (ULONG)size;
        NTSTATUS status = BCryptGenRandom(NULL, buffer, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) { fprintf(stderr, "[dm] BCryptGenRandom failed: %#lx\n", (unsigned long)status); abort(); }
        buffer = (uint8_t *)buffer + n; size -= n;
    }
}
uint32_t arc4random(void) { uint32_t value; arc4random_buf(&value, sizeof value); return value; }
uint32_t arc4random_uniform(uint32_t bound) {
    if (bound < 2) return 0;
    uint32_t value, threshold = -bound % bound;
    do { value = arc4random(); } while (value < threshold);
    return value % bound;
}
void *memmem(const void *buffer, size_t size, const void *needle, size_t length) {
    if (!length) return (void *)buffer;
    if (length > size) return NULL;
    const uint8_t *p = buffer;
    for (size_t i = 0; i <= size - length; i++) if (!memcmp(p + i, needle, length)) return (void *)(p + i);
    return NULL;
}
char *strcasestr(const char *text, const char *needle) {
    size_t length = strlen(needle);
    if (!length) return (char *)text;
    for (; *text; text++) if (!_strnicmp(text, needle, length)) return (char *)text;
    return NULL;
}
ssize_t getline(char **buffer, size_t *capacity, FILE *file) {
    if (!buffer || !capacity || !file) { errno = EINVAL; return -1; }
    size_t used = 0;
    for (;;) {
        int c = fgetc(file);
        if (c == EOF && (!used || ferror(file))) return -1;
        if (!*buffer || used + 1 >= *capacity) {
            if (used > SIZE_MAX / 2 - 128) { errno = EOVERFLOW; return -1; }
            size_t next = (used + 128) * 2;
            char *allocation = realloc(*buffer, next);
            if (!allocation) { errno = ENOMEM; return -1; }
            *buffer = allocation; *capacity = next;
        }
        if (c == EOF) break;
        (*buffer)[used++] = (char)c;
        if (c == '\n') break;
    }
    (*buffer)[used] = 0;
    return (ssize_t)used;
}
struct tm *localtime_r(const time_t *value, struct tm *out) { return _localtime64_s(out, value) ? NULL : out; }
int utimes(const char *path, const struct timeval times[2]) {
    FILETIME value[2];
    if (times) for (unsigned i = 0; i < 2; i++) {
        int64_t seconds = times[i].tv_sec;
        if (times[i].tv_usec < 0 || times[i].tv_usec >= 1000000 || seconds < -11644473600ll) { errno = EINVAL; return -1; }
        uint64_t ticks = ((uint64_t)seconds + 11644473600ull) * 10000000 + times[i].tv_usec * 10;
        value[i] = (FILETIME){(DWORD)ticks, (DWORD)(ticks >> 32)};
    } else { GetSystemTimeAsFileTime(&value[0]); value[1] = value[0]; }
    wchar_t *wide = dm_windows_wide(path); if (!wide) return -1;
    HANDLE file = CreateFileW(wide, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD error = GetLastError(); free(wide);
    if (file == INVALID_HANDLE_VALUE) return dm_windows_errno(error);
    int result = SetFileTime(file, NULL, &value[0], &value[1]) ? 0 : dm_windows_errno(GetLastError());
    CloseHandle(file); return result;
}
static DWORD page_protection(int protection) {
    if (protection & PROT_EXEC) return protection & PROT_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return protection & PROT_WRITE ? PAGE_READWRITE : protection & PROT_READ ? PAGE_READONLY : PAGE_NOACCESS;
}
void *mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    if (!size || offset < 0 || (flags & (MAP_SHARED | MAP_FIXED))) { errno = ENOTSUP; return MAP_FAILED; }
    DWORD allocation = MEM_RESERVE | (protection == PROT_NONE ? 0 : MEM_COMMIT);
    void *memory = VirtualAlloc(address, size, allocation, page_protection(protection));
    if (!memory) { dm_windows_errno(GetLastError()); return MAP_FAILED; }
    if (!(flags & MAP_ANON)) {
        if (fd < 0 || protection == PROT_NONE) { VirtualFree(memory, 0, MEM_RELEASE); errno = EINVAL; return MAP_FAILED; }
        DWORD previous;
        if (!VirtualProtect(memory, size, PAGE_READWRITE, &previous)) { VirtualFree(memory, 0, MEM_RELEASE); errno = EIO; return MAP_FAILED; }
        size_t done = 0;
        while (done < size) {
            ssize_t n = pread(fd, (uint8_t *)memory + done, size - done, offset + (off_t)done);
            if (n < 0) { VirtualFree(memory, 0, MEM_RELEASE); return MAP_FAILED; }
            if (!n) break;
            done += (size_t)n;
        }
        if (!VirtualProtect(memory, size, page_protection(protection), &previous)) { VirtualFree(memory, 0, MEM_RELEASE); errno = EIO; return MAP_FAILED; }
    }
    if (protection & PROT_EXEC) FlushInstructionCache(GetCurrentProcess(), memory, size);
    return memory;
}
int mprotect(void *address, size_t size, int protection) {
    if (!size) return 0;
    if (protection != PROT_NONE && !VirtualAlloc(address, size, MEM_COMMIT, page_protection(protection))) return dm_windows_errno(GetLastError());
    DWORD previous;
    if (!VirtualProtect(address, size, page_protection(protection), &previous)) return dm_windows_errno(GetLastError());
    if (protection & PROT_EXEC) FlushInstructionCache(GetCurrentProcess(), address, size);
    return 0;
}
int munmap(void *address, size_t size) {
    (void)size;
    return VirtualFree(address, 0, MEM_RELEASE) ? 0 : dm_windows_errno(GetLastError());
}
int madvise(void *address, size_t size, int advice) {
    if (advice != MADV_FREE) { errno = ENOTSUP; return -1; }
    return VirtualAlloc(address, size, MEM_RESET, PAGE_READWRITE) ? 0 : dm_windows_errno(GetLastError());
}
// CRT 对无效描述符默认终止进程；POSIX 接口必须按线程局部处理器转换成 EBADF。
// The CRT normally terminates on invalid descriptors; POSIX interfaces translate them to EBADF with a thread-local handler.
// LLVM-MinGW 尚未声明此公开 UCRT 导出，但 ARM64 导入库包含它。
// LLVM-MinGW omits this public UCRT declaration, but the ARM64 import library contains it.
__declspec(dllimport) _invalid_parameter_handler __cdecl _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler handler);
static void invalid_descriptor(const wchar_t *expression, const wchar_t *function, const wchar_t *file, unsigned line, uintptr_t reserved) {
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
}
static HANDLE descriptor_handle(int fd) {
    if (fd < 0) { errno = EBADF; return INVALID_HANDLE_VALUE; }
    _invalid_parameter_handler previous = _set_thread_local_invalid_parameter_handler(invalid_descriptor);
    intptr_t handle = _get_osfhandle(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    if (handle == -1 || handle == -2) { errno = EBADF; return INVALID_HANDLE_VALUE; }
    return (HANDLE)handle;
}
static ssize_t positioned_io(int fd, void *buffer, size_t size, off_t offset, int write_) {
    if (offset < 0) { errno = EINVAL; return -1; }
    HANDLE original = descriptor_handle(fd);
    if (original == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    HANDLE file = ReOpenFile(original, write_ ? GENERIC_WRITE : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
    if (file == INVALID_HANDLE_VALUE) return dm_windows_errno(GetLastError());
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)(uint64_t)offset; ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) { DWORD error = GetLastError(); CloseHandle(file); return dm_windows_errno(error); }
    DWORD count = size > INT_MAX ? INT_MAX : (DWORD)size, done = 0;
    BOOL ok = write_ ? WriteFile(file, buffer, count, &done, &ov) : ReadFile(file, buffer, count, &done, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (error == ERROR_IO_PENDING) { ok = GetOverlappedResult(file, &ov, &done, TRUE); error = ok ? ERROR_SUCCESS : GetLastError(); }
    CloseHandle(ov.hEvent); CloseHandle(file);
    if (error == ERROR_HANDLE_EOF) return 0;
    return ok ? (ssize_t)done : dm_windows_errno(error);
}
ssize_t pread(int fd, void *buffer, size_t size, off_t offset) { return positioned_io(fd, buffer, size, offset, 0); }
ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset) { return positioned_io(fd, (void *)buffer, size, offset, 1); }
int dm_windows_open(const char *path, int flags, ...) {
    wchar_t *wide = dm_windows_wide(path); if (!wide) return -1;
    DWORD access_ = flags & O_RDWR ? GENERIC_READ | GENERIC_WRITE : flags & O_WRONLY ? GENERIC_WRITE : GENERIC_READ;
    DWORD creation = flags & O_CREAT ? (flags & O_EXCL ? CREATE_NEW : flags & O_TRUNC ? CREATE_ALWAYS : OPEN_ALWAYS) :
        flags & O_TRUNC ? TRUNCATE_EXISTING : OPEN_EXISTING;
    HANDLE file = CreateFileW(wide, access_, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, creation,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD error = GetLastError(); free(wide);
    if (file == INVALID_HANDLE_VALUE) return dm_windows_errno(error);
    int fd = _open_osfhandle((intptr_t)file, (flags & (O_RDONLY | O_WRONLY | O_RDWR | O_APPEND)) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) CloseHandle(file);
    return fd;
}
FILE *dm_windows_fopen(const char *path, const char *mode) {
    wchar_t *p = dm_windows_wide(path), *m = dm_windows_wide(mode);
    FILE *file = p && m ? _wfopen(p, m) : NULL;
    free(p); free(m); return file;
}
int dm_windows_access(const char *path, int mode) {
    wchar_t *p = dm_windows_wide(path); if (!p) return -1;
    int result = _waccess(p, mode); free(p); return result;
}
int dm_windows_mkdir(const char *path, int mode) {
    (void)mode; wchar_t *p = dm_windows_wide(path); if (!p) return -1;
    int result = _wmkdir(p); free(p); return result;
}
int dm_windows_unlink(const char *path) {
    wchar_t *p = dm_windows_wide(path); if (!p) return -1;
    int result = DeleteFileW(p) ? 0 : dm_windows_errno(GetLastError()); free(p); return result;
}
int dm_windows_rmdir(const char *path) {
    wchar_t *p = dm_windows_wide(path); if (!p) return -1;
    int result = RemoveDirectoryW(p) ? 0 : dm_windows_errno(GetLastError()); free(p); return result;
}
int dm_windows_rename(const char *from, const char *to) {
    wchar_t *a = dm_windows_wide(from), *b = dm_windows_wide(to);
    int result = a && b ? (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : dm_windows_errno(GetLastError())) : -1;
    free(a); free(b); return result;
}
int dm_windows_ftruncate(int fd, off_t size) { int error = _chsize_s(fd, size); if (error) { errno = error; return -1; } return 0; }
int dm_windows_setenv(const char *key, const char *value, int overwrite) {
    if (!overwrite && getenv(key)) return 0;
    int error = _putenv_s(key, value); if (error) { errno = error; return -1; } return 0;
}
int dm_windows_unsetenv(const char *key) {
    int error = _putenv_s(key, ""); if (error) { errno = error; return -1; } return 0;
}
int getdtablesize(void) { return _getmaxstdio(); }
size_t pthread_get_stacksize_np(pthread_t thread) {
    if (thread != pthread_self()) { fprintf(stderr, "[dm] stack size requested for another thread\n"); return 0; }
    ULONG_PTR low, high; GetCurrentThreadStackLimits(&low, &high); return high - low;
}
void *pthread_get_stackaddr_np(pthread_t thread) {
    if (thread != pthread_self()) { fprintf(stderr, "[dm] stack address requested for another thread\n"); return NULL; }
    ULONG_PTR low, high; GetCurrentThreadStackLimits(&low, &high); return (void *)high;
}
static int stat_handle(HANDLE file, struct dm_windows_stat *out) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info)) return dm_windows_errno(GetLastError());
    memset(out, 0, sizeof *out);
    out->st_dev = info.dwVolumeSerialNumber;
    out->st_ino = ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    out->st_nlink = info.nNumberOfLinks;
    out->st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? _S_IFDIR : _S_IFREG) | 0600;
    out->st_size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    FILETIME times[] = {info.ftLastAccessTime, info.ftLastWriteTime, info.ftCreationTime};
    struct timespec *dest[] = {&out->st_atimespec, &out->st_mtimespec, &out->st_ctimespec};
    for (int i = 0; i < 3; i++) {
        uint64_t t = ((uint64_t)times[i].dwHighDateTime << 32) | times[i].dwLowDateTime;
        int64_t ns100 = (int64_t)t - 116444736000000000ll;
        dest[i]->tv_sec = ns100 / 10000000; dest[i]->tv_nsec = (long)(ns100 % 10000000) * 100;
    }
    out->st_atime = out->st_atimespec.tv_sec; out->st_mtime = out->st_mtimespec.tv_sec; out->st_ctime = out->st_ctimespec.tv_sec;
    return 0;
}
int dm_windows_stat(const char *path, struct dm_windows_stat *out) {
    wchar_t *p = dm_windows_wide(path); if (!p) return -1;
    HANDLE file = CreateFileW(p, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD error = GetLastError(); free(p);
    if (file == INVALID_HANDLE_VALUE) return dm_windows_errno(error);
    int result = stat_handle(file, out); CloseHandle(file); return result;
}
int dm_windows_fstat(int fd, struct dm_windows_stat *out) {
    HANDLE file = descriptor_handle(fd);
    if (file == INVALID_HANDLE_VALUE) return -1;
    DWORD kind = GetFileType(file);
    if (kind == FILE_TYPE_CHAR || kind == FILE_TYPE_PIPE) {
        memset(out, 0, sizeof *out); out->st_mode = kind == FILE_TYPE_PIPE ? _S_IFIFO : _S_IFCHR; out->st_nlink = 1;
        return 0;
    }
    return stat_handle(file, out);
}
int lstat(const char *path, struct dm_windows_stat *out) {
    int result = dm_windows_stat(path, out);
    if (!result) {
        wchar_t *wide = dm_windows_wide(path); if (!wide) return -1;
        DWORD attributes = GetFileAttributesW(wide); free(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES && attributes & FILE_ATTRIBUTE_REPARSE_POINT) out->st_mode = 0xa000;
    }
    return result;
}
DIR *opendir(const char *path) {
    size_t n = strlen(path); char *pattern = malloc(n + 3);
    if (!pattern) { errno = ENOMEM; return NULL; }
    snprintf(pattern, n + 3, "%s\\*", path);
    wchar_t *wide = dm_windows_wide(pattern); free(pattern); if (!wide) return NULL;
    DIR *directory = calloc(1, sizeof *directory); if (!directory) { free(wide); errno = ENOMEM; return NULL; }
    directory->find = FindFirstFileW(wide, &directory->data); DWORD error = GetLastError(); free(wide);
    if (directory->find == INVALID_HANDLE_VALUE) { free(directory); dm_windows_errno(error); return NULL; }
    directory->first = 1; return directory;
}
struct dirent *readdir(DIR *directory) {
    if (!directory->first && !FindNextFileW(directory->find, &directory->data)) {
        DWORD error = GetLastError(); if (error != ERROR_NO_MORE_FILES) dm_windows_errno(error);
        return NULL;
    }
    directory->first = 0;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory->data.cFileName, -1,
            directory->entry.d_name, sizeof directory->entry.d_name, NULL, NULL)) { errno = EILSEQ; return NULL; }
    DWORD a = directory->data.dwFileAttributes;
    directory->entry.d_type = a & FILE_ATTRIBUTE_REPARSE_POINT ? DT_LNK : a & FILE_ATTRIBUTE_DIRECTORY ? DT_DIR : DT_REG;
    return &directory->entry;
}
int closedir(DIR *directory) {
    int result = FindClose(directory->find) ? 0 : dm_windows_errno(GetLastError());
    free(directory); return result;
}
static char *stream_path(const char *path, const char *name) {
    if (strcmp(name, "user.org.d2rmobile.guest-name")) { errno = ENOTSUP; return NULL; }
    size_t n = strlen(path) + 40; char *out = malloc(n);
    if (out) snprintf(out, n, "%s:dm-guest-name", path); else errno = ENOMEM;
    return out;
}
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size) {
    char *stream = stream_path(path, name); if (!stream) return -1;
    int fd = dm_windows_open(stream, O_RDONLY); free(stream);
    if (fd < 0) { if (errno == ENOENT) errno = ENODATA; return -1; }
    struct dm_windows_stat info;
    if (dm_windows_fstat(fd, &info)) { _close(fd); return -1; }
    if (!size) { _close(fd); return info.st_size; }
    if ((uint64_t)info.st_size > size) { _close(fd); errno = ERANGE; return -1; }
    ssize_t n = pread(fd, value, size, 0); _close(fd); return n;
}
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags) {
    if (flags) { errno = ENOTSUP; return -1; }
    char *stream = stream_path(path, name); if (!stream) return -1;
    int fd = dm_windows_open(stream, O_WRONLY | O_CREAT | O_TRUNC); free(stream); if (fd < 0) return -1;
    ssize_t n = pwrite(fd, value, size, 0); _close(fd);
    if (n != (ssize_t)size) { if (n >= 0) errno = EIO; return -1; }
    return 0;
}
int lremovexattr(const char *path, const char *name) {
    char *stream = stream_path(path, name); if (!stream) return -1;
    int result = dm_windows_unlink(stream); free(stream); if (result && errno == ENOENT) errno = ENODATA; return result;
}
int clonefile(const char *from, const char *to, int flags) {
    if (flags) { errno = ENOTSUP; return -1; }
    wchar_t *a = dm_windows_wide(from), *b = dm_windows_wide(to);
    if (!a || !b) { free(a); free(b); return -1; }
    DWORD attributes = GetFileAttributesW(a);
    if (attributes == INVALID_FILE_ATTRIBUTES) { DWORD e = GetLastError(); free(a); free(b); return dm_windows_errno(e); }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { free(a); free(b); errno = ENOTSUP; return -1; }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        int result = CopyFileW(a, b, TRUE) ? 0 : dm_windows_errno(GetLastError());
        free(a); free(b); return result;
    }
    free(a); free(b);
    if (dm_windows_mkdir(to, 0700)) return -1;
    DIR *directory = opendir(from); if (!directory) return -1;
    struct dirent *entry; int result = 0;
    for (;;) {
        errno = 0; entry = readdir(directory);
        if (!entry) { if (errno) result = -1; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char src[2048], dst[2048];
        if (snprintf(src, sizeof src, "%s/%s", from, entry->d_name) >= (int)sizeof src ||
            snprintf(dst, sizeof dst, "%s/%s", to, entry->d_name) >= (int)sizeof dst) { errno = ENAMETOOLONG; result = -1; break; }
        if (clonefile(src, dst, 0)) { result = -1; break; }
    }
    int saved = errno; closedir(directory); errno = saved;
    if (!result) {
        char name[1024]; ssize_t n = lgetxattr(from, "user.org.d2rmobile.guest-name", name, sizeof name);
        if (n >= 0) result = lsetxattr(to, "user.org.d2rmobile.guest-name", name, (size_t)n, 0);
        else if (errno != ENODATA) result = -1;
    }
    return result;
}
int renamex_np(const char *from, const char *to, unsigned flags) {
    if (flags != RENAME_SWAP) { errno = ENOTSUP; return -1; }
    wchar_t *a = dm_windows_wide(from), *b = dm_windows_wide(to);
    if (!a || !b) { free(a); free(b); return -1; }
    HANDLE transaction = CreateTransaction(NULL, NULL, 0, 0, 0, 0, NULL);
    if (transaction == INVALID_HANDLE_VALUE) { free(a); free(b); return dm_windows_errno(GetLastError()); }
    size_t n = wcslen(a) + 80; wchar_t *temporary = calloc(n, sizeof *temporary);
    if (!temporary) { CloseHandle(transaction); free(a); free(b); errno = ENOMEM; return -1; }
    swprintf(temporary, n, L"%ls.swap-%lu-%llu", a, GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    BOOL ok = MoveFileTransactedW(a, temporary, NULL, NULL, 0, transaction) &&
        MoveFileTransactedW(b, a, NULL, NULL, 0, transaction) &&
        MoveFileTransactedW(temporary, b, NULL, NULL, 0, transaction) && CommitTransaction(transaction);
    DWORD error = GetLastError();
    if (!ok) {
        BOOL rolled_back = RollbackTransaction(transaction);
        DWORD rollback_error = rolled_back ? ERROR_SUCCESS : GetLastError();
        fprintf(stderr, "[dm] atomic directory swap failed (Win32 %lu), rollback=%d (%lu); transactional NTFS is required\n",
            error, (int)rolled_back, rollback_error);
    }
    CloseHandle(transaction); free(a); free(b); free(temporary);
    return ok ? 0 : dm_windows_errno(error);
}
static _Thread_local char dl_message[256];
void *dlopen(const char *path, int flags) {
    (void)flags;
    wchar_t *wide = dm_windows_wide(path); if (!wide) return NULL;
    HMODULE module = LoadLibraryW(wide); DWORD error = GetLastError(); free(wide);
    if (!module) snprintf(dl_message, sizeof dl_message, "LoadLibrary failed: %lu", error);
    return module;
}
void *dlsym(void *library, const char *symbol) {
    HMODULE module = library ? library : GetModuleHandleW(NULL);
    void *result = (void *)GetProcAddress(module, symbol);
    if (!result) snprintf(dl_message, sizeof dl_message, "GetProcAddress(%s) failed: %lu", symbol, GetLastError());
    return result;
}
int dlclose(void *library) { return FreeLibrary(library) ? 0 : -1; }
char *dlerror(void) { return dl_message[0] ? dl_message : NULL; }
int dladdr(const void *address, Dl_info *info) {
    MEMORY_BASIC_INFORMATION memory;
    memset(info, 0, sizeof *info);
    if (!VirtualQuery(address, &memory, sizeof memory)) return 0;
    info->dli_fbase = memory.AllocationBase;
    return 1;
}
static struct sigaction handlers[32];
static LONG CALLBACK exception_dispatch(EXCEPTION_POINTERS *exception) {
    DWORD code = exception->ExceptionRecord->ExceptionCode;
    int sig = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ? SIGSEGV :
        code == EXCEPTION_ILLEGAL_INSTRUCTION ? SIGILL : code == EXCEPTION_BREAKPOINT ? SIGTRAP : 0;
    if (!sig || !handlers[sig].sa_sigaction) return EXCEPTION_CONTINUE_SEARCH;
    ucontext_t context = {exception->ContextRecord, 0};
    siginfo_t info = {exception->ExceptionRecord->ExceptionAddress};
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        info.si_addr = (void *)exception->ExceptionRecord->ExceptionInformation[1];
        context.esr = exception->ExceptionRecord->ExceptionInformation[0] == 1 ? 1ull << 6 : 0;
    }
    handlers[sig].sa_sigaction(sig, &info, &context);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static INIT_ONCE exception_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK exception_init(PINIT_ONCE once, PVOID argument, PVOID *context) {
    (void)once; (void)argument; (void)context;
    return AddVectoredExceptionHandler(1, exception_dispatch) != NULL;
}
int sigaction(int sig, const struct sigaction *action, struct sigaction *previous) {
    if (sig < 1 || sig >= 32) { errno = EINVAL; return -1; }
    if (!InitOnceExecuteOnce(&exception_once, exception_init, NULL, NULL)) return dm_windows_errno(GetLastError());
    if (previous) *previous = handlers[sig];
    if (action) {
        handlers[sig] = *action;
        if (!(action->sa_flags & SA_SIGINFO) && signal(sig, action->sa_handler) == SIG_ERR) return -1;
    }
    return 0;
}
int sigemptyset(dm_sigset_t *set) { *set = 0; return 0; }
int sigaltstack(const stack_t *stack, stack_t *previous) {
    (void)stack; (void)previous; errno = ENOTSUP; return -1;
}
static VOID CALLBACK alarm_callback(PVOID argument, BOOLEAN fired) { (void)argument; (void)fired; _exit(128 + SIGALRM); }
unsigned alarm(unsigned seconds) {
    static HANDLE timer;
    if (timer) { DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE); timer = NULL; }
    if (seconds && !CreateTimerQueueTimer(&timer, NULL, alarm_callback, NULL, seconds * 1000, 0, WT_EXECUTEONLYONCE)) {
        fprintf(stderr, "[dm] alarm timer failed: %lu\n", GetLastError()); abort();
    }
    return 0;
}
