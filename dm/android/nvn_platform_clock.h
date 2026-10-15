#pragma once
#include <stdint.h>
uint64_t dm_android_nvn_clock_ns(void);
uint64_t dm_android_nvn_thread_cpu_ns(void);
void dm_android_nvn_schedule_thread(const char *role);
void dm_android_nvn_sleep_until(uint64_t deadline_ns);
void dm_android_nvn_presented(uint64_t frame);
/* Per-frame CPU workload accounting. Wait hooks only read local clocks/TLS;
 * hint service calls happen outside the Vulkan lock at acquire/present edges. */
enum dm_android_nvn_wait_kind {
    DM_NVN_WAIT_FENCE, DM_NVN_WAIT_QUEUE_IDLE, DM_NVN_WAIT_ACQUIRE,
    DM_NVN_WAIT_PRESENT, DM_NVN_WAIT_EVENT, DM_NVN_WAIT_KIND_COUNT
};
void dm_android_nvn_work_begin(void);
void dm_android_nvn_work_check_thread(void);
void dm_android_nvn_work_end(uint64_t frame);
void dm_android_nvn_work_reset(int foreground);
uint64_t dm_android_nvn_work_wait_begin(enum dm_android_nvn_wait_kind kind);
void dm_android_nvn_work_wait_end(uint64_t token);
