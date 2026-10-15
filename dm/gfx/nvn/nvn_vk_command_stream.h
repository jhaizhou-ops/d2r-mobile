/* One command handle's validated guest-memory lease. This caches containment,
 * never opcodes or their bytes: payload loads still observe current guest data.
 * A submit cut invalidates the lease because pool lifetime can change while the
 * device lock is released. Recursive handles each have their own cursor.
 */
#ifndef NVN_VK_COMMAND_STREAM_H
#define NVN_VK_COMMAND_STREAM_H
#include <stdint.h>

typedef struct {
    uint64_t base,size,pool_serial,submission_epoch;
    int valid;
} CommandMemoryCursor;

static inline int command_cursor_contains(const CommandMemoryCursor *cursor,
                                         uint64_t epoch,uint64_t address,uint64_t bytes) {
    return cursor->valid && cursor->submission_epoch==epoch &&
        address>=cursor->base && address-cursor->base<=cursor->size &&
        bytes<=cursor->size-(address-cursor->base) && bytes<=UINT64_MAX-address;
}
#endif
