// ASTC 4x4 块拷贝：不足一块的尾部只允许同时落在源和目标的 mip 边界。
// ASTC 4x4 block copies: a partial final block must reach both source and destination mip edges.
#pragma once
#include <stdint.h>
static inline int nvn_astc_copy_axis(int source_origin, int destination_origin, int count,
                                     uint32_t source_extent, uint32_t destination_extent) {
    if (source_origin < 0 || destination_origin < 0 || count <= 0 || source_origin % 4 || destination_origin % 4) return 0;
    uint32_t s = (uint32_t)source_origin, d = (uint32_t)destination_origin, n = (uint32_t)count;
    if (s > source_extent || d > destination_extent || n > source_extent - s || n > destination_extent - d) return 0;
    return !(n % 4) || (n == source_extent - s && n == destination_extent - d);
}
