#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct xbg_nvn_sampler_desc {
    uint32_t min_filter,mag_filter,wrap_s,wrap_t,wrap_r,compare_mode,compare_func;
    float min_lod,max_lod,lod_bias,anisotropy,border[4];
} xbg_nvn_sampler_desc;
typedef struct xbg_sampler_limits {
    VkBool32 anisotropy_enabled,mirror_clamp_to_edge_enabled,integer_border;
    float max_anisotropy,max_lod_bias;
} xbg_sampler_limits;
/* Pure conversion of actual NVN state. No Vulkan call. Unknown legacy clamp
 * modes and noncanonical borders fail instead of being approximated.
 * Returns 0 on success and leaves output untouched on failure. */
int xbg_sampler_from_nvn(const xbg_nvn_sampler_desc *nvn,const xbg_sampler_limits *limits,VkSamplerCreateInfo *out);
const char *xbg_sampler_error(int result);
#ifdef __cplusplus
}
#endif
