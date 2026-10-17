#pragma once
#include "xbg_draw_plan.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bounded 64MiB temporary index expansion. No allocation in these helpers. */
int xbg_quad_index_count(int32_t vertices,uint32_t *indices);
/* Only non-indexed QUADS. indices capacity is measured in uint32 elements. */
int xbg_quad_plan(const xbg_draw_call *source,xbg_draw_call *triangles,uint32_t *indices,size_t capacity);
#ifdef __cplusplus
}
#endif
