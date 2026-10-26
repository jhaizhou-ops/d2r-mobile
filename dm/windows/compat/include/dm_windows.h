// Windows ARM64 宿主适配；依据 Win32 API 与 Windows ARM64 ABI，客体键不用 winpthreads 私有布局。
// Windows ARM64 host adaptation, based on Win32 and the Windows ARM64 ABI; guest keys avoid winpthreads internals.
#pragma once
#ifndef _WIN32
#error "This header is only for Windows builds"
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#undef near
#undef far
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <direct.h>
#include <sys/time.h>
#ifdef __cplusplus
extern "C" {
#endif
int dm_windows_guest_key_create(pthread_key_t *key);
void *dm_windows_getspecific(pthread_key_t key);
int dm_windows_setspecific(pthread_key_t key, const void *value);
uint8_t *dm_windows_tsd_base(void);
uint64_t dm_windows_slot_offset(pthread_key_t key);
int dm_windows_tsd_check(pthread_key_t key);
wchar_t *dm_windows_wide(const char *text);
int dm_windows_errno(DWORD error);
uint64_t clock_gettime_nsec_np(clockid_t clock);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
ssize_t pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset);
int dm_windows_open(const char *path, int flags, ...);
FILE *dm_windows_fopen(const char *path, const char *mode);
int dm_windows_access(const char *path, int mode);
int dm_windows_mkdir(const char *path, int mode);
int dm_windows_unlink(const char *path);
int dm_windows_rmdir(const char *path);
int dm_windows_rename(const char *from, const char *to);
int dm_windows_ftruncate(int fd, off_t size);
int dm_windows_setenv(const char *key, const char *value, int overwrite);
int dm_windows_unsetenv(const char *key);
int getdtablesize(void);
int getpagesize(void);
void arc4random_buf(void *buffer, size_t size);
uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t bound);
void *memmem(const void *buffer, size_t size, const void *needle, size_t length);
char *strcasestr(const char *text, const char *needle);
ssize_t getline(char **buffer, size_t *capacity, FILE *file);
struct tm *localtime_r(const time_t *value, struct tm *out);
int utimes(const char *path, const struct timeval times[2]);
size_t pthread_get_stacksize_np(pthread_t thread);
void *pthread_get_stackaddr_np(pthread_t thread);
void dm_thread_enter(uint64_t entry, uint64_t argument, uint64_t sp, uint64_t limit);
void dm_windows_thread_exit(void) __attribute__((noreturn));
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
#ifndef ENODATA
#define ENODATA 120
#endif
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef F_OK
#define F_OK 0
#define R_OK 4
#define W_OK 2
#endif
#ifndef DM_WINDOWS_IMPL
#define pthread_getspecific dm_windows_getspecific
#define pthread_setspecific dm_windows_setspecific
#define open dm_windows_open
#define fopen dm_windows_fopen
#define access dm_windows_access
#define mkdir dm_windows_mkdir
#define unlink dm_windows_unlink
#define rmdir dm_windows_rmdir
#define rename dm_windows_rename
#define ftruncate dm_windows_ftruncate
#define setenv dm_windows_setenv
#define unsetenv dm_windows_unsetenv
#endif
#ifdef __cplusplus
}
#endif
