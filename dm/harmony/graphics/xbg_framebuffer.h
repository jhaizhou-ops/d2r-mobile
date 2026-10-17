#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct xbg_framebuffer_attachment {
    uint32_t active,width,height,levels,layers;
    uint32_t base_level,level_count,base_layer,layer_count,samples;
} xbg_framebuffer_attachment;
typedef struct xbg_framebuffer_extent { uint32_t width,height,layers,samples; } xbg_framebuffer_extent;
/* Nonempty attachment intersection after selecting each NVN mip/slice.
 * Layered rendering and mixed sample counts require separately enabled support
 * and are rejected. On failure, output is unchanged and bad_index identifies
 * the invalid attachment (UINT32_MAX for an empty set/global extent limit). */
int xbg_framebuffer_plan(const xbg_framebuffer_attachment *attachments,uint32_t count,
    uint32_t max_width,uint32_t max_height,xbg_framebuffer_extent *out,uint32_t *bad_index);
const char *xbg_framebuffer_error(int result);
#ifdef __cplusplus
}
#endif
