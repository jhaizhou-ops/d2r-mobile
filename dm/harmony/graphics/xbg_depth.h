#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct xbg_depth_format { uint32_t packed_bytes,depth_bytes,stencil_bytes; } xbg_depth_format;
int xbg_depth_format_info(uint32_t nvn_format,xbg_depth_format *out);
/* Tegra/NVN Z24S8 stores stencil/unused in bits0..7 and depth in bits8..31.
 * Vulkan buffer copies use separate depth/stencil planes; D24 depth occupies
 * bits0..23 in each 32-bit word. Z32F_X24S8 stores float then stencil byte.
 * Source layouts: deko3d format_traits.inc; Ryujinx Vulkan FormatConverter.cs.
 * No allocation, float conversion, clear-to-default or source mutation.
 * Zero row/image stride means tight; all sizes include all layers/slices.
 * Extract writes tight planes. Merge leaves packed padding bytes untouched. */
int xbg_depth_extract(uint32_t format,uint32_t width,uint32_t height,uint32_t slices,
    const void *packed,size_t packed_bytes,size_t row_stride,size_t image_stride,
    void *depth,size_t depth_bytes,void *stencil,size_t stencil_bytes);
int xbg_depth_merge(uint32_t format,uint32_t width,uint32_t height,uint32_t slices,
    void *packed,size_t packed_bytes,size_t row_stride,size_t image_stride,
    const void *depth,size_t depth_bytes,const void *stencil,size_t stencil_bytes);
#ifdef __cplusplus
}
#endif
