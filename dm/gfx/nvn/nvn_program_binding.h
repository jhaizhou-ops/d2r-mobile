#pragma once
#include "nvn_obj.h"

#define NVN_BINDING_MAGIC MAGIC('B', 'I', 'N', 'D')
// 仅宿主使用的不可变状态，不写进客体 NVNprogram 存储。
// Host-only immutable state, never written into guest NVNprogram storage.
typedef struct nvn_program_binding {
    prog_t program;
    const prog_t *source[6];
    uint64_t source_serial[6];
    struct nvn_program_binding *next;
} nvn_program_binding;

const prog_t *nvn_program_bind(const prog_t *current, const prog_t *source, unsigned mask);
