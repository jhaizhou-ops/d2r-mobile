// Windows 上的内存和线程诊断，依据 GetProcessMemoryInfo / Toolhelp / 线程上下文 API。
// Windows memory and thread diagnostics, based on GetProcessMemoryInfo / Toolhelp / thread context APIs.
#include "dm_windows.h"
#include <mach/mach.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <string.h>
uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
// Windows 不提供此 Apple 退休指令接口；失败必须显式报告，不能把周期数当作指令数。
// Windows has no equivalent of Apple's retired-instruction API; report failure, never substitute cycle counts.
int thread_selfcounts(int type, void *buffer, size_t size) {
    if (type != 1 || !buffer || size < 2 * sizeof(uint64_t)) { errno = EINVAL; return -1; }
    memset(buffer, 0, size);
    static LONG warned;
    if (!InterlockedCompareExchange(&warned, 1, 0))
        fputs("[dm] Retired-instruction counters unavailable on Windows; instruction-count profiling is not supported\n", stderr);
    errno = ENOTSUP;
    return -1;
}
kern_return_t task_info(mach_port_t task, int flavor, task_info_t out, mach_msg_type_number_t *count) {
    (void)task;
    if (flavor != TASK_VM_INFO || *count < TASK_VM_INFO_COUNT) return KERN_FAILURE;
    PROCESS_MEMORY_COUNTERS_EX memory = {.cb = sizeof memory};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory, sizeof memory)) return KERN_FAILURE;
    task_vm_info_data_t *info = (void *)out;
    info->phys_footprint = memory.PrivateUsage; info->resident_size = memory.WorkingSetSize; info->virtual_size = memory.PagefileUsage;
    *count = TASK_VM_INFO_COUNT; return KERN_SUCCESS;
}
kern_return_t thread_suspend(thread_act_t id) {
    if (id == GetCurrentThreadId()) return KERN_FAILURE;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, id); if (!thread) return KERN_FAILURE;
    DWORD result = SuspendThread(thread); CloseHandle(thread); return result == (DWORD)-1 ? KERN_FAILURE : KERN_SUCCESS;
}
kern_return_t thread_resume(thread_act_t id) {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, id); if (!thread) return KERN_FAILURE;
    DWORD result = ResumeThread(thread); CloseHandle(thread); return result == (DWORD)-1 ? KERN_FAILURE : KERN_SUCCESS;
}
kern_return_t thread_get_state(thread_act_t id, int flavor, thread_state_t out, mach_msg_type_number_t *count) {
    if (flavor != ARM_THREAD_STATE64 || *count < ARM_THREAD_STATE64_COUNT) return KERN_FAILURE;
    HANDLE thread = OpenThread(THREAD_GET_CONTEXT, FALSE, id); if (!thread) return KERN_FAILURE;
    CONTEXT context = {.ContextFlags = CONTEXT_FULL};
    BOOL ok = GetThreadContext(thread, &context); CloseHandle(thread); if (!ok) return KERN_FAILURE;
    arm_thread_state64_t *state = (void *)out;
    memcpy(state->__x, context.X, sizeof state->__x);
    state->__fp = context.Fp; state->__lr = context.Lr; state->__sp = context.Sp; state->__pc = context.Pc; state->__cpsr = context.Cpsr;
    *count = ARM_THREAD_STATE64_COUNT; return KERN_SUCCESS;
}
kern_return_t task_threads(mach_port_t task, thread_act_array_t *out, mach_msg_type_number_t *count) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return KERN_FAILURE;
    THREADENTRY32 entry = {.dwSize = sizeof entry};
    size_t capacity = 32, n = 0; thread_act_t *ids = malloc(capacity * sizeof *ids);
    if (!ids) { CloseHandle(snapshot); return KERN_FAILURE; }
    if (!Thread32First(snapshot, &entry)) { free(ids); CloseHandle(snapshot); return KERN_FAILURE; }
    do {
        if (entry.th32OwnerProcessID != task) continue;
        if (n == capacity) {
            capacity *= 2; thread_act_t *larger = realloc(ids, capacity * sizeof *ids);
            if (!larger) { free(ids); CloseHandle(snapshot); return KERN_FAILURE; }
            ids = larger;
        }
        ids[n++] = entry.th32ThreadID;
    } while (Thread32Next(snapshot, &entry));
    DWORD error = GetLastError(); CloseHandle(snapshot);
    if (error != ERROR_NO_MORE_FILES) { free(ids); return KERN_FAILURE; }
    *out = ids; *count = (unsigned)n; return KERN_SUCCESS;
}
static time_value_t file_time(FILETIME time) {
    uint64_t value = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
    return (time_value_t){(int)(value / 10000000), (int)(value % 10000000 / 10)};
}
kern_return_t thread_info(thread_act_t id, int flavor, thread_info_t out, mach_msg_type_number_t *count) {
    if (flavor != THREAD_BASIC_INFO || *count < THREAD_BASIC_INFO_COUNT) return KERN_FAILURE;
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id); if (!thread) return KERN_FAILURE;
    FILETIME creation, exit_, kernel, user;
    BOOL ok = GetThreadTimes(thread, &creation, &exit_, &kernel, &user); CloseHandle(thread); if (!ok) return KERN_FAILURE;
    thread_basic_info_data_t *info = (void *)out; memset(info, 0, sizeof *info);
    info->user_time = file_time(user); info->system_time = file_time(kernel);
    *count = THREAD_BASIC_INFO_COUNT; return KERN_SUCCESS;
}
