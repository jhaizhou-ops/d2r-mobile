/* Exact Vulkan command-state reuse within one primary command buffer.
 * The replay backend binds at most two sets, from set zero, with no dynamic
 * offsets. Graphics pipelines always make viewport/scissor/blend dynamic.
 * All state-setting commands must pass through these helpers. Reset after
 * every vkBeginCommandBuffer, including event-wait cuts and reused handles.
 * Resource resolution, uploads and draw/dispatch calls remain outside here.
 */
#ifndef NVN_VK_COMMAND_STATE_H
#define NVN_VK_COMMAND_STATE_H

#include <stdint.h>
#include <string.h>

enum {
    NVN_VK_CMD_PIPELINE, NVN_VK_CMD_DESCRIPTORS, NVN_VK_CMD_VIEWPORT,
    NVN_VK_CMD_SCISSOR, NVN_VK_CMD_BLEND, NVN_VK_CMD_VERTEX,
    NVN_VK_CMD_INDEX, NVN_VK_CMD_STATE_COUNT
};

typedef struct {
    VkPipeline pipeline[2];
    struct {
        VkPipelineLayout layout;
        VkDescriptorSet sets[2];
        uint32_t count;
        int valid;
    } descriptors[2];
    VkViewport viewport;
    VkRect2D scissor;
    float blend[4];
    VkBuffer vertex[16],index;
    VkDeviceSize vertex_offset[16],index_offset;
    VkIndexType index_type;
    uint32_t vertex_valid;
    int viewport_valid,scissor_valid,blend_valid,index_valid;
} NvnVkCommandState;

static inline void nvn_vk_command_state_reset(NvnVkCommandState *state) {
    memset(state,0,sizeof(*state));
}

/* Return one when the Vulkan command was issued, zero when omitted. The
 * disabled path also invalidates its cache entry, so toggling the mode cannot
 * revive state that an uncached command has replaced. */
static inline int nvn_vk_cmd_pipeline(NvnVkCommandState *state,int elide,
        VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline pipeline) {
    unsigned slot=point==VK_PIPELINE_BIND_POINT_COMPUTE;
    if(elide&&state->pipeline[slot]==pipeline)return 0;
    vkCmdBindPipeline(command,point,pipeline);
    state->pipeline[slot]=elide?pipeline:VK_NULL_HANDLE;
    return 1;
}

static inline int nvn_vk_cmd_descriptors(NvnVkCommandState *state,int elide,
        VkCommandBuffer command,VkPipelineBindPoint point,VkPipelineLayout layout,
        uint32_t count,const VkDescriptorSet *sets) {
    unsigned slot=point==VK_PIPELINE_BIND_POINT_COMPUTE;
    /* Use exact layout identity, rather than guessing compatibility. Binding
     * fewer sets conservatively replaces the whole remembered set list. */
    if(elide&&count>=1&&count<=2&&state->descriptors[slot].valid&&
       state->descriptors[slot].layout==layout&&state->descriptors[slot].count==count&&
       !memcmp(state->descriptors[slot].sets,sets,count*sizeof(*sets)))return 0;
    vkCmdBindDescriptorSets(command,point,layout,0,count,sets,0,NULL);
    state->descriptors[slot].valid=0;
    if(elide&&count>=1&&count<=2){
        state->descriptors[slot].layout=layout;state->descriptors[slot].count=count;
        memcpy(state->descriptors[slot].sets,sets,count*sizeof(*sets));
        state->descriptors[slot].valid=1;
    }
    return 1;
}

static inline int nvn_vk_cmd_viewport(NvnVkCommandState *state,int elide,
        VkCommandBuffer command,const VkViewport *viewport) {
    /* Bit comparisons preserve negative zero and avoid treating NaNs as equal
     * through arithmetic. Vulkan's six float fields have no padding. */
    if(elide&&state->viewport_valid&&!memcmp(&state->viewport,viewport,sizeof(*viewport)))return 0;
    vkCmdSetViewport(command,0,1,viewport);
    state->viewport_valid=elide;if(elide)memcpy(&state->viewport,viewport,sizeof(*viewport));
    return 1;
}

static inline int nvn_vk_cmd_scissor(NvnVkCommandState *state,int elide,
        VkCommandBuffer command,const VkRect2D *scissor) {
    if(elide&&state->scissor_valid&&state->scissor.offset.x==scissor->offset.x&&
       state->scissor.offset.y==scissor->offset.y&&state->scissor.extent.width==scissor->extent.width&&
       state->scissor.extent.height==scissor->extent.height)return 0;
    vkCmdSetScissor(command,0,1,scissor);
    state->scissor_valid=elide;if(elide)state->scissor=*scissor;
    return 1;
}

static inline int nvn_vk_cmd_blend(NvnVkCommandState *state,int elide,
        VkCommandBuffer command,const float blend[4]) {
    if(elide&&state->blend_valid&&!memcmp(state->blend,blend,sizeof(state->blend)))return 0;
    vkCmdSetBlendConstants(command,blend);
    state->blend_valid=elide;if(elide)memcpy(state->blend,blend,sizeof(state->blend));
    return 1;
}

static inline int nvn_vk_cmd_vertex(NvnVkCommandState *state,int elide,
        VkCommandBuffer command,uint32_t binding,VkBuffer buffer,VkDeviceSize offset) {
    if(elide&&binding<16&&(state->vertex_valid&(1u<<binding))&&
       state->vertex[binding]==buffer&&state->vertex_offset[binding]==offset)return 0;
    vkCmdBindVertexBuffers(command,binding,1,&buffer,&offset);
    if(binding<16){
        state->vertex_valid&=~(1u<<binding);
        if(elide){state->vertex[binding]=buffer;state->vertex_offset[binding]=offset;state->vertex_valid|=1u<<binding;}
    }
    return 1;
}

static inline int nvn_vk_cmd_index(NvnVkCommandState *state,int elide,
        VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,VkIndexType type) {
    if(elide&&state->index_valid&&state->index==buffer&&state->index_offset==offset&&state->index_type==type)return 0;
    vkCmdBindIndexBuffer(command,buffer,offset,type);
    state->index_valid=elide;
    if(elide){state->index=buffer;state->index_offset=offset;state->index_type=type;}
    return 1;
}

#endif
