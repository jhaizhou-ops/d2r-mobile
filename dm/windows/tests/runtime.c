// 无游戏资源的 Windows ARM64 验证：真实 TLS、内存权限、UTF-8 / 大文件与原子存档。
// Game-independent Windows ARM64 checks: real TLS, page permissions, UTF-8 / large files and atomic saves.
#include "dm_windows.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include "../../host/dm_sect.h"
#include "../../host/dm.h"
#include "../guest_libc.h"
#include "../../host/dm_alloc.h"
#include "rewrite_blocks.h"
typedef struct { const char *name; void *value; } registration;
DM_SECT(swhle) static const registration item = {"windows-runtime", (void *)1};
static pthread_key_t key;
pthread_key_t g_key18;
static pthread_key_t tls_key, tlr_key;
static pthread_key_t owned;
static void *guest_memory;
static int destroyed;
static uint8_t *fault_page;
static int fault_count;
struct dm_flags g_flag;
module_t g_mod[MAX_MOD];
int g_nmod;
static _Thread_local ULONG_PTR host_stack_low, host_stack_high, host_committed_low;
extern uint64_t svc_dispatch(int number, uint64_t *registers, uint64_t origin);
extern void svc_guest_range(uint8_t **base, size_t *size);
void hlecount_svc(int number) { assert(number == 0x04 || number == 0x05 || number == 0x29); }
int svc_thread(int number, uint64_t *registers) { (void)registers; hlecount_svc(number); return 0; }
int svc_host_call(int number, uint64_t *registers) { (void)number; (void)registers; abort(); }
uint64_t guest_ns(void) { abort(); }
uint64_t dm_entropy64(void) { abort(); }
module_t *module_of(const void *address) { (void)address; abort(); }
void dm_backtrace(uint64_t fp) { (void)fp; abort(); }
static uint64_t memory_svc(int number, uint64_t first, uint64_t second, uint64_t third) {
    uint64_t registers[32] = {first, second, third};
    svc_dispatch(number, registers, 0);
    assert(registers[0] == 0);
    return registers[1];
}
static void fault_handler(int number, siginfo_t *info, void *raw) {
    ucontext_t *context = raw;
    assert(number == SIGSEGV && info->si_addr == fault_page && (context->esr & (1ull << 6)));
    fault_count++;
    assert(!mprotect(fault_page, 4096, PROT_READ | PROT_WRITE));
}
uint64_t gate_dispatch(uint64_t address, uint64_t *registers) {
    assert(address == 123 && registers[18] == 456);
    registers[18] = 789; return 321;
}
extern uint64_t dm_windows_gate_dispatch(uint64_t address, uint64_t *registers);
extern uint64_t mach_absolute_time(void);
extern int thread_selfcounts(int type, void *buffer, size_t size);
static uint64_t execute_rewrite(const uint32_t *code, size_t size, uint64_t argument) {
    uint8_t *image = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(image != MAP_FAILED);
    memcpy(image + 0x1000, code, size);
    *(uint32_t *)(image + 0x104) = 0xd65f03c0;
    uint64_t *page = (void *)(image + 0x4000);
    page[0] = dm_windows_slot_offset(g_key18); page[1] = dm_windows_slot_offset(tls_key); page[2] = dm_windows_slot_offset(tlr_key);
    assert(!mprotect(image, 0x2000, PROT_READ | PROT_EXEC));
    void *teb = dm_windows_tsd_base();
    uint64_t result = ((uint64_t (*)(uint64_t))(image + 0x1000))(argument);
    assert(dm_windows_tsd_base() == teb);
    assert(!munmap(image, 65536));
    return result;
}
#define EXECUTE(name, value) execute_rewrite(rewrite_##name, sizeof rewrite_##name, value)
void dm_thread_returned(void) { fputs("unexpected guest return\n", stderr); abort(); }
static void destructor(void *value) {
    assert(value == (void *)42);
    uintptr_t marker = (uintptr_t)&value;
    assert(marker < (uintptr_t)guest_memory || marker >= (uintptr_t)guest_memory + 65536);
    ULONG_PTR low, high; GetCurrentThreadStackLimits(&low, &high);
    assert(low == host_stack_low && high == host_stack_high);
    assert((uintptr_t)((NT_TIB *)dm_windows_tsd_base())->StackLimit == host_committed_low);
    destroyed = 1;
}
extern void dm_windows_test_stack_exception(void);
__attribute__((noinline)) static void guest_stack_fault(void) {
    volatile uint8_t frame[16384];
    for (size_t i = 0; i < sizeof frame; i++) frame[i] = (uint8_t)i;
    *(volatile uint8_t *)fault_page = 78;
    dm_windows_test_stack_exception();
    ULONG_PTR low, high; GetCurrentThreadStackLimits(&low, &high);
    assert(low <= (uintptr_t)frame && (uintptr_t)frame + sizeof frame <= high);
    for (size_t i = 0; i < sizeof frame; i++) assert(frame[i] == (uint8_t)i);
}
static void guest_stack_entry(uint64_t value) {
    assert(value == 123);
    assert((uintptr_t)&value >= (uintptr_t)guest_memory && (uintptr_t)&value < (uintptr_t)guest_memory + 65536);
    assert(pthread_get_stackaddr_np(pthread_self()) == (uint8_t *)guest_memory + 65536 - 32);
    assert(pthread_get_stacksize_np(pthread_self()) == 65536 - 32);
    assert(((NT_TIB *)dm_windows_tsd_base())->StackLimit == guest_memory);
    guest_stack_fault();
    dm_windows_thread_exit();
}
static void *stack_worker(void *unused) {
    (void)unused;
    assert(!pthread_setspecific(owned, (void *)42));
    GetCurrentThreadStackLimits(&host_stack_low, &host_stack_high);
    host_committed_low = (uintptr_t)((NT_TIB *)dm_windows_tsd_base())->StackLimit;
    uint64_t sp = (uintptr_t)guest_memory + 65536 - 32, limit;
    assert(svc_guest_stack_limit(sp, &limit) && limit == (uintptr_t)guest_memory);
    dm_thread_enter((uintptr_t)guest_stack_entry, 123, sp, limit);
    abort();
}
static void *worker(void *argument) {
    assert(dm_windows_tsd_check(key));
    assert(!pthread_getspecific(key));
    assert(!pthread_setspecific(key, argument));
    assert(*(void **)(dm_windows_tsd_base() + dm_windows_slot_offset(key)) == argument);
    return NULL;
}
static void write_file(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(pwrite(fd, text, strlen(text), 0) == (ssize_t)strlen(text));
    assert(!_commit(fd));
    close(fd);
}
int main(void) {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    assert(sizeof(void *) == 8 && sizeof(long) == 4 && sizeof(off_t) == 8);
    uint64_t before = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    uint64_t absolute = mach_absolute_time();
    assert(absolute >= before && absolute <= clock_gettime_nsec_np(CLOCK_MONOTONIC));
    uint64_t counts[2] = {1, 1};
    errno = 0;
    assert(thread_selfcounts(1, counts, sizeof counts) == -1 && errno == ENOTSUP);
    assert(counts[0] == 0 && counts[1] == 0);
    assert(thread_selfcounts(1, counts, sizeof counts - 1) == -1 && errno == EINVAL);
    assert(thread_selfcounts(0, counts, sizeof counts) == -1 && errno == EINVAL);
    assert(thread_selfcounts(1, NULL, sizeof counts) == -1 && errno == EINVAL);
    assert(!dm_windows_guest_key_create(&key));
    assert(dm_windows_tsd_check(key));
    assert(!pthread_setspecific(key, (void *)7));
    pthread_t thread; assert(!pthread_create(&thread, NULL, worker, (void *)9)); assert(!pthread_join(thread, NULL));
    assert(pthread_getspecific(key) == (void *)7);
    puts("TLS: TEB slots and thread isolation passed");
    assert(!dm_windows_guest_key_create(&g_key18) && !dm_windows_guest_key_create(&tls_key) && !dm_windows_guest_key_create(&tlr_key));
    assert(!pthread_setspecific(tls_key, (void *)0x123456789abcdef0ull));
    assert(!pthread_setspecific(tlr_key, (void *)0xfedcba9876543210ull));
    assert(EXECUTE(read_tls, 0) == 0x123456789abcdef0ull);
    assert(EXECUTE(read_tlr, 0) == 0xfedcba9876543210ull);
    EXECUTE(read_x18, 0);
    assert(pthread_getspecific(g_key18) == (void *)0x123456789abcdef0ull);
    EXECUTE(write_tls, 456);
    assert(pthread_getspecific(tls_key) == (void *)456);
    EXECUTE(write_x18, 0);
    assert(pthread_getspecific(tls_key) == pthread_getspecific(g_key18));
    EXECUTE(read_zero, 0);
    assert(!pthread_setspecific(g_key18, (void *)456));
    uint64_t registers[32] = {[18] = (uintptr_t)dm_windows_tsd_base()};
    assert(dm_windows_gate_dispatch(123, registers) == 321);
    assert(registers[18] == (uintptr_t)dm_windows_tsd_base() && pthread_getspecific(g_key18) == (void *)789);
    void *aligned = dm_aligned_alloc(4096, 8192); assert(aligned && !((uintptr_t)aligned & 4095)); dm_aligned_free(aligned);
    puts("Rewriter: emitted ARM64 code executed natively, preserving TEB and guest x18 across TLS reads/writes and gates");
    void *source = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(source != MAP_FAILED && !pthread_key_create(&owned, destructor));
    memset(source, 37, 65536);
    uint64_t stack_region = memory_svc(0x29, 0, 14, 0);
    guest_memory = (void *)(uintptr_t)(stack_region + 0x11000);
    memory_svc(0x04, (uintptr_t)guest_memory, (uintptr_t)source, 65536);
    assert(*(uint8_t *)guest_memory == 37);
    uint64_t neighbor = (uintptr_t)guest_memory + 65536, limit;
    memory_svc(0x04, neighbor, (uintptr_t)source, 4096);
    assert(!svc_guest_stack_limit(0, &limit));
    assert(!svc_guest_stack_limit((uintptr_t)guest_memory, &limit));
    assert(!svc_guest_stack_limit((uintptr_t)guest_memory - 1, &limit));
    assert(!svc_guest_stack_limit(neighbor + 4097, &limit));
    assert(svc_guest_stack_limit(neighbor, &limit) && limit == (uintptr_t)guest_memory);
    assert(svc_guest_stack_limit(neighbor + 4096, &limit) && limit == neighbor);
    fault_page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(fault_page != MAP_FAILED);
    struct sigaction action = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO}, previous;
    assert(!sigaction(SIGSEGV, &action, &previous));
    for (int i = 0; i < 32; i++) {
        DWORD old;
        assert(VirtualProtect((uint8_t *)guest_memory + 61440, 4096,
            PAGE_READWRITE | (i & 1 ? PAGE_NOCACHE : 0), &old));
        assert(!mprotect(fault_page, 4096, PROT_READ));
        fault_count = destroyed = 0;
        assert(!pthread_create(&thread, NULL, stack_worker, NULL));
        assert(!pthread_join(thread, NULL) && destroyed);
        assert(fault_count == 1 && *fault_page == 78);
    }
    assert(!sigaction(SIGSEGV, &previous, NULL));
    assert(!munmap(fault_page, 4096));
    memory_svc(0x05, (uintptr_t)guest_memory, (uintptr_t)source, 65536);
    assert(!svc_guest_stack_limit(neighbor, &limit));
    assert(svc_guest_stack_limit(neighbor + 4096, &limit) && limit == neighbor);
    memory_svc(0x05, neighbor, (uintptr_t)source, 4096);
    assert(!svc_guest_stack_limit(neighbor + 4096, &limit));
    assert(!munmap(source, 65536));
    uint8_t *guest_base; size_t guest_size; svc_guest_range(&guest_base, &guest_size);
    assert(!munmap(guest_base, guest_size));
    puts("Stacks: exact SVC mapping bounds, 32 deep-stack write-fault/C++ recoveries, restored host TEB and pthread destructors passed");
    unsigned char *memory = mmap(NULL, 5ull << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    assert(!mprotect(memory + (4ull << 30), 4096, PROT_READ | PROT_WRITE));
    memory[4ull << 30] = 77;
    assert(!mprotect(memory + (4ull << 30), 4096, PROT_READ));
    MEMORY_BASIC_INFORMATION info; assert(VirtualQuery(memory + (4ull << 30), &info, sizeof info));
    assert(info.Protect == PAGE_READONLY && memory[4ull << 30] == 77);
    fault_page = memory + (4ull << 30);
    fault_count = 0;
    assert(!sigaction(SIGSEGV, &action, &previous));
    *(volatile uint8_t *)fault_page = 78;
    assert(fault_count == 1 && *fault_page == 78);
    assert(!sigaction(SIGSEGV, &previous, NULL));
    assert(!munmap(memory, 5ull << 30));
    puts("Memory: 5 GiB reservation, protection and write-fault recovery passed");
    const registration *records; unsigned long bytes;
    DM_SECT_GET(swhle, const registration, records, bytes);
    assert(bytes == sizeof(registration));
    assert(!strcmp(records->name, "windows-runtime") && records->value == (void *)1);
    puts("COFF: registration section boundaries passed");
    char root[256]; snprintf(root, sizeof root, "dm-runtime-%lu-%llu", GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    assert(!mkdir(root, 0700));
    char file[512]; snprintf(file, sizeof file, "%s/\xe6\xb5\x8b\xe8\xaf\x95.bin", root);
    write_file(file, "old");
    int fd = open(file, O_RDWR); assert(fd >= 0);
    assert(lseek(fd, 1, SEEK_SET) == 1);
    assert(pwrite(fd, "wide", 4, (off_t)5 << 30) == 4);
    assert(lseek(fd, 0, SEEK_CUR) == 1);
    char buffer[16] = {0}; assert(pread(fd, buffer, 4, (off_t)5 << 30) == 4 && !strcmp(buffer, "wide"));
    assert(lseek(fd, 0, SEEK_CUR) == 1);
    struct stat stat_; assert(!fstat(fd, &stat_) && stat_.st_size == ((int64_t)5 << 30) + 4);
    assert(!ftruncate(fd, 3)); close(fd);
    assert(fstat(fd, &stat_) == -1 && errno == EBADF);
    assert(fstat(8191, &stat_) == -1 && errno == EBADF);
    assert(pread(-1, buffer, 1, 0) == -1 && errno == EBADF);
    const char *attribute = "user.org.d2rmobile.guest-name";
    assert(!lsetxattr(root, attribute, "Root", 4, 0));
    assert(!lsetxattr(file, attribute, "Case", 4, 0));
    assert(lgetxattr(file, attribute, buffer, sizeof buffer) == 4 && !memcmp(buffer, "Case", 4));
    puts("Files: UTF-8, 64-bit offsets and NTFS guest-name metadata passed");
    char copied[300]; snprintf(copied, sizeof copied, "%s-copy", root); assert(!clonefile(root, copied, 0));
    assert(lgetxattr(copied, attribute, buffer, sizeof buffer) == 4 && !memcmp(buffer, "Root", 4));
    char other[512]; snprintf(other, sizeof other, "%s/\xe6\xb5\x8b\xe8\xaf\x95.bin", copied);
    assert(lgetxattr(other, attribute, buffer, sizeof buffer) == 4 && !memcmp(buffer, "Case", 4));
    fd = open(other, O_WRONLY); assert(fd >= 0); assert(pwrite(fd, "new", 3, 0) == 3); close(fd);
    assert(!renamex_np(copied, root, RENAME_SWAP));
    fd = open(file, O_RDONLY); assert(fd >= 0); assert(pread(fd, buffer, 3, 0) == 3 && !memcmp(buffer, "new", 3)); close(fd);
    fd = open(other, O_RDONLY); assert(fd >= 0); assert(pread(fd, buffer, 3, 0) == 3 && !memcmp(buffer, "old", 3)); close(fd);
    assert(renamex_np("missing-directory", root, RENAME_SWAP) == -1);
    fd = open(file, O_RDONLY); assert(fd >= 0); assert(pread(fd, buffer, 3, 0) == 3 && !memcmp(buffer, "new", 3)); close(fd);
    assert(!unlink(file) && !rmdir(root)); assert(!unlink(other) && !rmdir(copied));
    puts("Saves: atomic NTFS directory swap, copied metadata and failed-swap preservation passed");
    int64_t seconds = 2147483648ll;
    struct { dm_guest_tm time; uint64_t canary; } calendar = {.canary = 0x123456789abcdef0ull};
    assert(dm_guest_gmtime_r(&seconds, &calendar.time) == &calendar.time);
    assert(calendar.time.tm_year == 138 && calendar.time.tm_mon == 0 && calendar.time.tm_mday == 19);
    assert(calendar.time.tm_gmtoff == 0 && !strcmp(calendar.time.tm_zone, "UTC") && calendar.canary == 0x123456789abcdef0ull);
    assert(dm_guest_timegm(&calendar.time) == seconds);
    char formatted[128];
    calendar.time.tm_gmtoff = 19800; calendar.time.tm_zone = "test%zone";
    assert(dm_guest_strftime(formatted, sizeof formatted, "%Y %z %Z %%z", &calendar.time));
    assert(!strcmp(formatted, "2038 +0530 test%zone %z"));
    puts("ABI: LP64 time layout, post-2038 dates and guest timezone formatting passed");
    return 0;
}
