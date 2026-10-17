#pragma once
#include <vulkan/vulkan.h>
struct XbgBlitRegion {
    uint32_t width,height,layers,levels,level,base_layer;
    int32_t x,y,z,w,h,count;
};
/* Same-format single-sample color 2D/array only; caller verifies image identity,
 * creation format, type and samples. Unknown NVN flags fail unchanged. */
int xbg_blit_plan(const XbgBlitRegion &src,const XbgBlitRegion &dst,uint32_t flags,
    int scalar,VkFormatFeatureFlags source_features,VkFormatFeatureFlags dest_features,
    VkImageBlit *region,VkFilter *filter);
struct XbgCopyRows { uint64_t row,tight,rows,bytes; uint32_t texel_row; };
int xbg_copy_rows(uint32_t width,uint32_t height,uint32_t slices,uint32_t bytes_per_pixel,
    uint64_t stride,XbgCopyRows *out);
int xbg_copy_rows_write(void *dst,uint64_t dst_bytes,const void *src,uint64_t src_bytes,const XbgCopyRows &plan);

int xbg_copy_is_full(const XbgBlitRegion &destination);
