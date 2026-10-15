/* Shader plans describe binding identity, not resource contents. Resolved
 * descriptors belong to a real submission epoch. Reuse still prepares every
 * referenced CPU/GPU range and image layout before recording a draw. */
#ifndef NVN_VK_BINDING_PLAN_H
#define NVN_VK_BINDING_PLAN_H

#include "nvn_vk_pool_upload.h"

typedef struct { uint32_t slot; uint64_t incarnation; } TextureRef;
typedef PoolUploadRange Range;

typedef struct {
    Range range;
    uint64_t pool_serial,offset;
    uint32_t pool_slot,bias;
} BindingBuffer;
typedef struct {
    TextureRef texture;
    uint32_t descriptor_id;
    VkImageLayout layout;
} BindingImage;
typedef union { BindingBuffer buffer; BindingImage image; } BindingResource;

typedef struct {
    uint32_t eligible,nbuffer,nimage,ndriver;
    uint8_t buffers[LIMIT_BINDINGS],images[LIMIT_BINDINGS],drivers[LIMIT_BINDINGS];
    uint64_t epoch,state_revision,registry_revision,pool_revision;
    VkDescriptorSet set;
    uint32_t resolved;
    BindingResource resources[];
} BindingPlan;

#endif
