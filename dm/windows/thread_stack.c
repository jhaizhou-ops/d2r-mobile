// SDK 给的客体栈不能交给 Windows CRT 展开；通过显式栈桥返回宿主后退出线程。
// Windows CRT must not unwind SDK-owned stacks; return through an explicit stack bridge before exiting the thread.
#include "dm_windows.h"
typedef struct { uint64_t registers[24]; } saved_stack;
static _Thread_local saved_stack *active;
extern void dm_windows_guest_enter(uint64_t entry, uint64_t argument, uint64_t sp, saved_stack *saved, uint64_t limit);
extern void dm_windows_guest_leave(saved_stack *saved) __attribute__((noreturn));
static int stack_writable(uint64_t limit, uint64_t sp) {
    while (limit < sp) {
        MEMORY_BASIC_INFORMATION memory;
        if (!VirtualQuery((void *)(uintptr_t)limit, &memory, sizeof memory) || memory.State != MEM_COMMIT ||
            (memory.Protect & PAGE_GUARD)) return 0;
        DWORD protection = memory.Protect & 0xff;
        if (protection != PAGE_READWRITE && protection != PAGE_EXECUTE_READWRITE) return 0;
        size_t available = memory.RegionSize - (limit - (uintptr_t)memory.BaseAddress);
        if (available >= sp - limit) return 1;
        limit += available;
    }
    return 0;
}
void dm_thread_enter(uint64_t entry, uint64_t argument, uint64_t sp, uint64_t limit) {
    ULONG_PTR low, high; GetCurrentThreadStackLimits(&low, &high);
    uint8_t *teb = dm_windows_tsd_base();
    if (*(uintptr_t *)(teb + 0x1478) != low || *(uintptr_t *)(teb + 8) != high) {
        fprintf(stderr, "[dm] unsupported Windows TEB stack layout\n"); abort();
    }
    if (active || sp & 15 || !stack_writable(limit, sp)) {
        fprintf(stderr, "[dm] invalid or nested Windows guest stack\n"); abort();
    }
    saved_stack saved; active = &saved;
    dm_windows_guest_enter(entry, argument, sp, &saved, limit);
    active = NULL;
    pthread_exit(NULL);
}
void dm_windows_thread_exit(void) {
    if (active) dm_windows_guest_leave(active);
    pthread_exit(NULL);
    __builtin_unreachable();
}
