#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "xbg_texture.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct xbg_vk_renderer xbg_vk_renderer;
typedef struct xbg_vk_texture xbg_vk_texture;
typedef struct xbg_vk_info {
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;                  /* caller serializes all use of this queue */
    uint32_t queue_family;          /* graphics, same family as queue */
    VkPhysicalDeviceFeatures enabled_core; /* exact features used at vkCreateDevice */
    VkBool32 enabled_shader_draw_parameters;
    const uint32_t *vs_words; size_t vs_bytes; /* real 96757bb36a024b91 */
    const uint32_t *ps_words; size_t ps_bytes; /* real 6b53604427784bff */
    uint64_t fence_timeout_ns;      /* nonzero; recommended 5 seconds */
} xbg_vk_info;
typedef struct xbg_vk_requirements {
    VkPhysicalDeviceFeatures core; /* merge into vkCreateDevice enabled features */
    VkBool32 shader_draw_parameters; /* chain VkPhysicalDeviceShaderDrawParametersFeatures */
    VkSubgroupFeatureFlags subgroup_operations;
    VkShaderStageFlags subgroup_stages;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceSubgroupProperties subgroup;
} xbg_vk_requirements;
/* Queries the actual device. Reads capabilities/opcodes without changing shaders.
 * Rejects unknown capabilities rather than silently enabling a subset. */
int xbg_vk_query_2d(VkPhysicalDevice physical,const uint32_t *vs,size_t vs_bytes,
    const uint32_t *ps,size_t ps_bytes,xbg_vk_requirements *requirements,char *error,size_t error_bytes);
int xbg_vk_create(const xbg_vk_info *info,xbg_vk_renderer **out,char *error,size_t error_bytes);
/* May return VK_TIMEOUT with GPU resources still alive. Caller retries wait; it
 * must not destroy or reuse them while a submission is pending. */
int xbg_vk_wait(xbg_vk_renderer *renderer);
int xbg_vk_destroy(xbg_vk_renderer *renderer);

typedef struct xbg_vertex2d {
    float position[2];              /* original vs reads only location0.xy */
    float uv[2];                    /* location1.xy */
    float color[4];                 /* location2.rgba */
} xbg_vertex2d;
typedef struct xbg_draw2d {
    const xbg_vertex2d *vertices;
    uint32_t vertex_count;
    const uint16_t *indices; uint32_t index_count; /* optional triangle-list */
    const void *vertex_uniform0; size_t vertex_uniform_bytes; /* NVN VS uniform0 */
    const void *fragment_uniform0; size_t fragment_uniform_bytes; /* NVN PS uniform0 */
    const void *vertex_support; const void *fragment_support; /* each 1392 bytes */
    xbg_vk_texture *texture10;      /* NVN PS texture10, sampler0 */
    VkFilter min_filter,mag_filter; /* each NEAREST/LINEAR, queried against format */
    VkSamplerMipmapMode mipmap_mode;
    VkSamplerAddressMode address_u,address_v;
    float min_lod,max_lod,lod_bias;
    VkPipelineColorBlendAttachmentState blend; /* exact NVN translated state */
    float blend_constants[4];
    VkRect2D scissor;
    VkClearColorValue clear;
    uint32_t width,height;
    VkFormat target_format;         /* RGBA8 UNORM or SRGB, chosen explicitly */
    VkBool32 load_previous;         /* preserve previous target for subsequent NVN draws */
} xbg_draw2d;
int xbg_vk_upload_texture(xbg_vk_renderer *renderer,const void *original_texture,
    size_t bytes,xbg_vk_texture **out);
int xbg_vk_upload_nvn_texture(xbg_vk_renderer *renderer,const xbg_nvn_texture_desc *description,
    const void *guest_storage,size_t bytes,xbg_vk_texture **out);
int xbg_vk_release_texture(xbg_vk_renderer *renderer,xbg_vk_texture *texture);
int xbg_vk_draw_2d(xbg_vk_renderer *renderer,const xbg_draw2d *draw);
/* Exact-size full-atlas replay using the genuine 2D shaders. Explicitly generated
 * projection/gamma defaults, no game-state capture claim. Useful first integration
 * entry point: NEAREST, no blend, identity RGB/alpha, no color-block shader. */
int xbg_vk_replay_texture(xbg_vk_renderer *renderer,xbg_vk_texture *texture);
int xbg_vk_readback(xbg_vk_renderer *renderer,void *rgba,size_t bytes);
/* After a successful draw/wait: TRANSFER_SRC_OPTIMAL, queue_family owned.
 * Caller may copy/blit/sample only with proper transitions, and must restore the
 * layout before another backend call. Keep texture alive until external use ends. */
VkImage xbg_vk_output_image(xbg_vk_renderer *renderer,uint32_t *width,uint32_t *height);
const char *xbg_vk_error(const xbg_vk_renderer *renderer);

/* Explicit replay defaults, derived from original SPIR-V. These are not captured
 * game state: runtime integration passes real NVN constants instead.
 * Projection maps pixel coordinates directly to a positive-height Vulkan viewport.
 * PS has gamma/contrast at byte20/24; zero gamma is invalid. */
void xbg_2d_projection(float matrix16[16],float width,float height);
void xbg_2d_fragment_defaults(float constants8[8]);
void xbg_support_defaults(void *bytes1392,float width,float height);
/* Original NVN/Metal upper-left framebuffer convention with the supplied
 * unmodified game shaders. Vulkan 1.1 negative viewport height preserves both
 * screen position and front-face orientation. Scissor and depth stay unchanged.
 * Returns -1 without writing output for nonfinite/invalid bounds/depth. */
int xbg_nvn_viewport(VkViewport *out,float x,float y,float width,float height,
    float minimum_depth,float maximum_depth);
/* NVN enum translation, using baseline gfx/nvn/include/nvn.h numeric ABI.
 * Unsupported legacy clamp/mirror-clamp modes return an error, never approximate.
 * Non-mip NVN min filters use maxLod .25 to retain independent min/mag filters.
 * Border mode uses transparent black in the standalone 2D path. */
int xbg_2d_sampler_from_nvn(xbg_draw2d *draw,uint32_t min_filter,uint32_t mag_filter,
    uint32_t wrap_s,uint32_t wrap_t,float min_lod,float max_lod,float lod_bias);
int xbg_2d_blend_from_nvn(xbg_draw2d *draw,uint32_t enabled,uint32_t src_rgb,uint32_t dst_rgb,
    uint32_t src_alpha,uint32_t dst_alpha,uint32_t equation_rgb,uint32_t equation_alpha,uint32_t rgba_mask);
typedef struct xbg_nvn_attribute {
    const void *buffer; size_t buffer_bytes;
    uint32_t nvn_format,offset,stride;
} xbg_nvn_attribute;
/* Converts bound NVN location0/1/2 streams into the real 2D vertex layout.
 * Supports R/RG/RGB/RGBA32F and RGBA8_UNORM; bounded reads, no guest-pointer casts.
 * Guest-memory validation/mapping remains the runtime owner's responsibility. */
int xbg_2d_vertices_from_nvn(const xbg_nvn_attribute attributes[3],uint32_t first,uint32_t count,
    xbg_vertex2d *out,size_t out_bytes);
#ifdef __cplusplus
}
#endif
