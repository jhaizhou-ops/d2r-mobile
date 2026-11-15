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
 * Extract and merge do no allocation, float conversion, clear-to-default or source mutation.
 * Zero row/image stride means tight; all sizes include all layers/slices.
 * Extract writes tight planes. Merge leaves packed padding bytes untouched. */
int xbg_depth_extract(uint32_t format,uint32_t width,uint32_t height,uint32_t slices,
    const void *packed,size_t packed_bytes,size_t row_stride,size_t image_stride,
    void *depth,size_t depth_bytes,void *stencil,size_t stencil_bytes);
int xbg_depth_merge(uint32_t format,uint32_t width,uint32_t height,uint32_t slices,
    void *packed,size_t packed_bytes,size_t row_stride,size_t image_stride,
    const void *depth,size_t depth_bytes,const void *stencil,size_t stencil_bytes);
/* 没有 D24 格式的设备（三星 Xclipse）把 24 位定点深度存成 32 位浮点。这两个函数做单个像素的互转：
 * 定点值 v（低 24 位）变成最接近 v/(2^24-1) 的 float 位模式；float 变回最接近的定点值，NaN 和负数为 0，大于 1 为 2^24-1。
 * 定点 -> 浮点 -> 定点对全部 2^24 个值无损。
 * Devices without D24 formats (Samsung Xclipse) store 24-bit unorm depth as 32-bit float. These convert one pixel:
 * the unorm value v (low 24 bits) becomes the bit pattern of the float nearest v/(2^24-1); a float becomes the nearest unorm value,
 * with NaN and negatives giving 0 and values above 1 giving 2^24-1. Unorm -> float -> unorm is exact for all 2^24 values. */
uint32_t xbg_depth_unorm24_to_float(uint32_t unorm24);
uint32_t xbg_depth_float_to_unorm24(uint32_t float_bits);
#ifdef __cplusplus
}
#endif
