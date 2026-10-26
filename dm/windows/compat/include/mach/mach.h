// 公共宿主的 Mach 查询接口由 Win32 实现；线程端口用线程 ID，不泄漏 HANDLE。
// Shared host Mach queries implemented with Win32; thread ports are thread IDs, not leaked HANDLEs.
#pragma once
#include <windows.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
typedef int kern_return_t;
typedef unsigned mach_port_t;
typedef mach_port_t thread_act_t;
typedef thread_act_t *thread_act_array_t;
typedef unsigned mach_msg_type_number_t;
typedef uintptr_t vm_address_t;
typedef size_t vm_size_t;
typedef int *task_info_t;
typedef int *thread_info_t;
typedef unsigned *thread_state_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
static inline mach_port_t mach_task_self(void) { return GetCurrentProcessId(); }
static inline mach_port_t mach_thread_self(void) { return GetCurrentThreadId(); }
static inline mach_port_t pthread_mach_thread_np(pthread_t thread) { return GetThreadId(pthread_gethandle(thread)); }
static inline kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) { (void)task; (void)name; return KERN_SUCCESS; }
static inline kern_return_t vm_deallocate(mach_port_t task, vm_address_t address, vm_size_t size) { (void)task; (void)size; free((void *)address); return KERN_SUCCESS; }
static inline kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t address, vm_size_t size, vm_address_t to, vm_size_t *got) {
    (void)task; SIZE_T read = 0;
    BOOL ok = ReadProcessMemory(GetCurrentProcess(), (void *)address, (void *)to, size, &read);
    if (got) *got = read;
    return ok && read == size ? KERN_SUCCESS : KERN_FAILURE;
}
#define TASK_VM_INFO 22
typedef struct { uint64_t phys_footprint, resident_size, virtual_size; } task_vm_info_data_t;
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
typedef struct { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(uint32_t)))
#define __darwin_arm_thread_state64_get_pc(s) ((s).__pc)
#define __darwin_arm_thread_state64_get_lr(s) ((s).__lr)
#define __darwin_arm_thread_state64_get_sp(s) ((s).__sp)
#define __darwin_arm_thread_state64_get_fp(s) ((s).__fp)
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(int)))
#ifdef __cplusplus
extern "C" {
#endif
kern_return_t task_info(mach_port_t task, int flavor, task_info_t out, mach_msg_type_number_t *count);
kern_return_t thread_suspend(thread_act_t thread);
kern_return_t thread_resume(thread_act_t thread);
kern_return_t thread_get_state(thread_act_t thread, int flavor, thread_state_t out, mach_msg_type_number_t *count);
kern_return_t task_threads(mach_port_t task, thread_act_array_t *out, mach_msg_type_number_t *count);
kern_return_t thread_info(thread_act_t thread, int flavor, thread_info_t out, mach_msg_type_number_t *count);
#ifdef __cplusplus
}
#endif
