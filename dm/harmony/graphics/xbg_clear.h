#pragma once
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* NVN color numeric class: 0 float/normalized, 1 signed, 2 unsigned. */
int xbg_clear_color_class(uint32_t nvn_format);
/* Actual Vulkan view format, including decompressed BC storage. */
int xbg_clear_vk_class(VkFormat format);
int xbg_clear_value(VkFormat format,int input_scalar,const void *input,VkClearColorValue *output);
/* State for a shader clear, never inherited from the guest draw state. */
int xbg_masked_clear_state(uint32_t color_mask,uint32_t stencil_mask,uint32_t stencil,
    VkPipelineColorBlendAttachmentState *color,VkPipelineDepthStencilStateCreateInfo *depth);
int xbg_clear_rectangle(uint32_t width,uint32_t height,int32_t x,int32_t y,uint32_t w,uint32_t h,VkRect2D *out);
int xbg_texture_clear_slice(uint32_t width,uint32_t height,uint32_t layers,uint32_t base_layer,
    int32_t x,int32_t y,int32_t z,int32_t w,int32_t h,int32_t depth,uint32_t slice,VkRect2D *rect,uint32_t *layer);
int xbg_clear_requires_preserve(uint32_t levels,uint32_t layers,uint32_t width,uint32_t height,
    const VkRect2D *rect,uint32_t partial_mask,uint32_t aspects,uint32_t written_aspects);
#ifdef __cplusplus
}
#endif
