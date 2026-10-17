#pragma once
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* modules: VS,TCS,TES,GS,FS. Returns an actual Vulkan stage list.
 * Missing FS is supported only with depth/stencil, no active color writes,
 * and no alpha-to-coverage (the absent color/alpha outputs are undefined).
 * Output is untouched on failure. active_colors is a bit per attachment. */
int xbg_graphics_stages(const VkShaderModule modules[5],uint32_t has_depth,
    uint32_t active_colors,uint32_t channel_mask,uint32_t alpha_to_coverage,
    VkPipelineShaderStageCreateInfo stages[5],uint32_t *count);
#ifdef __cplusplus
}
#endif
