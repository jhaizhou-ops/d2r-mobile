/* Differential command replay: the Vulkan mocks model only commands that
 * reached the driver. Compare that state with an uncached command stream,
 * rather than inspecting the cache's internal remembered fields. No GPU is
 * used; this proves command-state equivalence, not pixel correctness or FPS.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn/nvn_vk_command_state.h"

#define HANDLE(type,value) ((type)(uintptr_t)(value))
#define REQUIRE(test) do {if(!(test)){fprintf(stderr,"command state check failed at line %d: %s\n",__LINE__,#test);exit(1);}} while(0)

typedef struct {
    VkPipeline graphics_pipeline,compute_pipeline;
    VkPipelineLayout layouts[2];
    VkDescriptorSet sets[2][4];
    VkViewport viewport;
    VkRect2D scissor;
    float constants[4];
    VkBuffer vertices[32],indices;
    VkDeviceSize offsets[32],index_offset;
    VkIndexType index_type;
} DriverState;

static DriverState actual[2];
static unsigned lane;
static uint64_t calls[2],comparisons,resets;

static void called(VkCommandBuffer command) {
    REQUIRE(command==HANDLE(VkCommandBuffer,1));calls[lane]++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline pipeline) {
    called(command);
    if(point==VK_PIPELINE_BIND_POINT_GRAPHICS)actual[lane].graphics_pipeline=pipeline;
    else {REQUIRE(point==VK_PIPELINE_BIND_POINT_COMPUTE);actual[lane].compute_pipeline=pipeline;}
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer command,VkPipelineBindPoint point,
        VkPipelineLayout layout,uint32_t first,uint32_t count,const VkDescriptorSet *sets,
        uint32_t dynamic_count,const uint32_t *dynamic_offsets) {
    called(command);REQUIRE(first==0&&count<=4&&dynamic_count==0&&dynamic_offsets==NULL);
    unsigned slot=point==VK_PIPELINE_BIND_POINT_COMPUTE;
    /* Distinct handles are treated as incompatible in this conservative
     * reference. Identical layouts retain any higher, untouched sets. */
    if(actual[lane].layouts[slot]!=layout)memset(actual[lane].sets[slot],0,sizeof(actual[lane].sets[slot]));
    actual[lane].layouts[slot]=layout;
    memcpy(actual[lane].sets[slot],sets,count*sizeof(*sets));
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer command,uint32_t first,uint32_t count,const VkViewport *viewport) {
    called(command);REQUIRE(first==0&&count==1);memcpy(&actual[lane].viewport,viewport,sizeof(*viewport));
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer command,uint32_t first,uint32_t count,const VkRect2D *scissor) {
    called(command);REQUIRE(first==0&&count==1);actual[lane].scissor=*scissor;
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer command,const float constants[4]) {
    called(command);memcpy(actual[lane].constants,constants,sizeof(actual[lane].constants));
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer command,uint32_t first,uint32_t count,
        const VkBuffer *buffers,const VkDeviceSize *offsets) {
    called(command);REQUIRE(first<32&&count==1);actual[lane].vertices[first]=buffers[0];actual[lane].offsets[first]=offsets[0];
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer command,VkBuffer buffer,VkDeviceSize offset,VkIndexType type) {
    called(command);actual[lane].indices=buffer;actual[lane].index_offset=offset;actual[lane].index_type=type;
}

typedef struct {
    unsigned kind,binding;
    VkPipelineBindPoint point;
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSet sets[4];
    uint32_t count;
    VkViewport viewport;
    VkRect2D scissor;
    float blend[4];
    VkBuffer buffer;
    VkDeviceSize offset;
    VkIndexType index_type;
} Operation;

static void reference(const Operation *op) {
    VkCommandBuffer command=HANDLE(VkCommandBuffer,1);
    switch(op->kind){
    case NVN_VK_CMD_PIPELINE:vkCmdBindPipeline(command,op->point,op->pipeline);break;
    case NVN_VK_CMD_DESCRIPTORS:vkCmdBindDescriptorSets(command,op->point,op->layout,0,op->count,op->sets,0,NULL);break;
    case NVN_VK_CMD_VIEWPORT:vkCmdSetViewport(command,0,1,&op->viewport);break;
    case NVN_VK_CMD_SCISSOR:vkCmdSetScissor(command,0,1,&op->scissor);break;
    case NVN_VK_CMD_BLEND:vkCmdSetBlendConstants(command,op->blend);break;
    case NVN_VK_CMD_VERTEX:vkCmdBindVertexBuffers(command,op->binding,1,&op->buffer,&op->offset);break;
    case NVN_VK_CMD_INDEX:vkCmdBindIndexBuffer(command,op->buffer,op->offset,op->index_type);break;
    default:REQUIRE(0);
    }
}
static int optimized(NvnVkCommandState *state,int elide,const Operation *op) {
    VkCommandBuffer command=HANDLE(VkCommandBuffer,1);
    switch(op->kind){
    case NVN_VK_CMD_PIPELINE:return nvn_vk_cmd_pipeline(state,elide,command,op->point,op->pipeline);
    case NVN_VK_CMD_DESCRIPTORS:return nvn_vk_cmd_descriptors(state,elide,command,op->point,op->layout,op->count,op->sets);
    case NVN_VK_CMD_VIEWPORT:return nvn_vk_cmd_viewport(state,elide,command,&op->viewport);
    case NVN_VK_CMD_SCISSOR:return nvn_vk_cmd_scissor(state,elide,command,&op->scissor);
    case NVN_VK_CMD_BLEND:return nvn_vk_cmd_blend(state,elide,command,op->blend);
    case NVN_VK_CMD_VERTEX:return nvn_vk_cmd_vertex(state,elide,command,op->binding,op->buffer,op->offset);
    case NVN_VK_CMD_INDEX:return nvn_vk_cmd_index(state,elide,command,op->buffer,op->offset,op->index_type);
    default:REQUIRE(0);return -1;
    }
}
static int replay(NvnVkCommandState *state,int elide,const Operation *op) {
    lane=0;reference(op);
    lane=1;uint64_t before=calls[1];int issued=optimized(state,elide,op);
    REQUIRE(calls[1]-before==(uint64_t)issued);
    REQUIRE(!memcmp(&actual[0],&actual[1],sizeof(actual[0])));comparisons++;
    return issued;
}
static void reset(NvnVkCommandState *state) {
    nvn_vk_command_state_reset(state);memset(actual,0,sizeof(actual));resets++;
}
static uint32_t random_next(uint32_t *seed) {
    *seed^=*seed<<13;*seed^=*seed>>17;*seed^=*seed<<5;return *seed;
}
static float float_bits(uint32_t bits) {
    float value;memcpy(&value,&bits,sizeof(value));return value;
}
static Operation defaults(unsigned kind) {
    return (Operation){.kind=kind,.point=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pipeline=HANDLE(VkPipeline,11),.layout=HANDLE(VkPipelineLayout,21),
        .sets={HANDLE(VkDescriptorSet,31),HANDLE(VkDescriptorSet,32),HANDLE(VkDescriptorSet,33)},.count=2,
        .viewport={0,720,1280,-720,0,1},.scissor={{0,0},{1280,720}},.blend={0,0,0,0},
        .buffer=HANDLE(VkBuffer,41),.offset=UINT64_C(0x100000020),.index_type=VK_INDEX_TYPE_UINT16};
}

int main(void) {
    NvnVkCommandState state;
    for(unsigned kind=0;kind<NVN_VK_CMD_STATE_COUNT;kind++){
        reset(&state);Operation op=defaults(kind);
        REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==0);
        /* Same numeric command-buffer/resource handles after pool reset must
         * still issue the first command of the new recording. */
        reset(&state);REQUIRE(replay(&state,1,&op)==1);
        REQUIRE(replay(&state,0,&op)==1);REQUIRE(replay(&state,0,&op)==1);
        REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==0);
    }
    reset(&state);Operation op=defaults(NVN_VK_CMD_PIPELINE);
    REQUIRE(replay(&state,1,&op)==1);op.point=VK_PIPELINE_BIND_POINT_COMPUTE;
    REQUIRE(replay(&state,1,&op)==1);op.point=VK_PIPELINE_BIND_POINT_GRAPHICS;
    REQUIRE(replay(&state,1,&op)==0);op.pipeline=HANDLE(VkPipeline,12);REQUIRE(replay(&state,1,&op)==1);
    op=defaults(NVN_VK_CMD_DESCRIPTORS);REQUIRE(replay(&state,1,&op)==1);
    op.point=VK_PIPELINE_BIND_POINT_COMPUTE;REQUIRE(replay(&state,1,&op)==1);
    op.point=VK_PIPELINE_BIND_POINT_GRAPHICS;REQUIRE(replay(&state,1,&op)==0);
    op.layout=HANDLE(VkPipelineLayout,22);REQUIRE(replay(&state,1,&op)==1);
    op.count=1;REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==0);
    op.count=2;REQUIRE(replay(&state,1,&op)==1);op.sets[1]=HANDLE(VkDescriptorSet,99);REQUIRE(replay(&state,1,&op)==1);
    op.count=3;REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==1);
    op.count=2;REQUIRE(replay(&state,1,&op)==1);
    op=defaults(NVN_VK_CMD_VERTEX);REQUIRE(replay(&state,1,&op)==1);
    op.binding=15;REQUIRE(replay(&state,1,&op)==1);op.binding=0;REQUIRE(replay(&state,1,&op)==0);
    op.offset++;REQUIRE(replay(&state,1,&op)==1);op.buffer=HANDLE(VkBuffer,42);REQUIRE(replay(&state,1,&op)==1);
    op.binding=16;REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==1);
    op=defaults(NVN_VK_CMD_INDEX);REQUIRE(replay(&state,1,&op)==1);
    op.index_type=VK_INDEX_TYPE_UINT32;REQUIRE(replay(&state,1,&op)==1);
    op.offset++;REQUIRE(replay(&state,1,&op)==1);op.buffer=HANDLE(VkBuffer,42);REQUIRE(replay(&state,1,&op)==1);
    for(unsigned field=0;field<6;field++){
        op=defaults(NVN_VK_CMD_VIEWPORT);REQUIRE(replay(&state,1,&op)==1);
        float fields[6];memcpy(fields,&op.viewport,sizeof(fields));fields[field]+=0.5f;memcpy(&op.viewport,fields,sizeof(fields));
        REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==0);
    }
    for(unsigned field=0;field<4;field++){
        op=defaults(NVN_VK_CMD_SCISSOR);REQUIRE(replay(&state,1,&op)==1);
        if(field==0)op.scissor.offset.x++;else if(field==1)op.scissor.offset.y++;
        else if(field==2)op.scissor.extent.width++;else op.scissor.extent.height++;
        REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==0);
        op=defaults(NVN_VK_CMD_BLEND);REQUIRE(replay(&state,1,&op)==1);
        op.blend[field]=float_bits(0x80000000u);REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==0);
        op.blend[field]=float_bits(0x7fc00001u);REQUIRE(replay(&state,1,&op)==1);REQUIRE(replay(&state,1,&op)==0);
        op.blend[field]=float_bits(0x7fc00002u);REQUIRE(replay(&state,1,&op)==1);
    }
    reset(&state);uint64_t repeated_before[2]={calls[0],calls[1]};
    for(unsigned draw=0;draw<256;draw++)for(unsigned kind=0;kind<NVN_VK_CMD_STATE_COUNT;kind++){
        op=defaults(kind);REQUIRE(replay(&state,1,&op)==(draw==0));
    }
    uint64_t repeated_baseline=calls[0]-repeated_before[0],repeated_optimized=calls[1]-repeated_before[1];
    REQUIRE(repeated_baseline==1792&&repeated_optimized==7);
    const uint32_t seeds[4]={0x1ce47u,0x51be32u,0x8fc381u,0xe6019u};
    for(unsigned trial=0;trial<4;trial++){
        reset(&state);uint32_t seed=seeds[trial];int elide=1;Operation history[2][NVN_VK_CMD_STATE_COUNT];
        for(unsigned point=0;point<2;point++)for(unsigned kind=0;kind<NVN_VK_CMD_STATE_COUNT;kind++){
            history[point][kind]=defaults(kind);history[point][kind].point=point?VK_PIPELINE_BIND_POINT_COMPUTE:VK_PIPELINE_BIND_POINT_GRAPHICS;
        }
        for(unsigned step=0;step<50000;step++){
            uint32_t value=random_next(&seed);
            if(value%173==0){reset(&state);continue;}
            if(value%211==0)elide=!elide;
            unsigned kind=random_next(&seed)%NVN_VK_CMD_STATE_COUNT,point=random_next(&seed)%2;
            op=history[point][kind];
            if(value%4==0){
                uint32_t change=random_next(&seed);
                switch(kind){
                case NVN_VK_CMD_PIPELINE:op.pipeline=HANDLE(VkPipeline,11+change%5);break;
                case NVN_VK_CMD_DESCRIPTORS:
                    op.layout=HANDLE(VkPipelineLayout,21+change%5);op.count=1+random_next(&seed)%2;
                    op.sets[random_next(&seed)%op.count]=HANDLE(VkDescriptorSet,31+random_next(&seed)%10);break;
                case NVN_VK_CMD_VIEWPORT:{
                    float fields[6];memcpy(fields,&op.viewport,sizeof(fields));
                    fields[change%6]=float_bits(random_next(&seed));memcpy(&op.viewport,fields,sizeof(fields));break;}
                case NVN_VK_CMD_SCISSOR:
                    op.scissor=(VkRect2D){{(int32_t)change,(int32_t)random_next(&seed)},{random_next(&seed),random_next(&seed)}};break;
                case NVN_VK_CMD_BLEND:op.blend[change%4]=float_bits(random_next(&seed));break;
                case NVN_VK_CMD_VERTEX:op.binding=change%16; /* fall through */
                case NVN_VK_CMD_INDEX:
                    op.buffer=HANDLE(VkBuffer,41+change%5);op.offset=((uint64_t)random_next(&seed)<<32)|random_next(&seed);
                    op.index_type=change%2?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;break;
                }
                history[point][kind]=op;
            }
            int issued=replay(&state,elide,&op);if(!elide)REQUIRE(issued==1);
        }
    }
    printf("{\"passed\":true,\"random_steps\":200000,\"state_comparisons\":%llu,\"resets\":%llu,\"reference_commands\":%llu,\"cached_commands\":%llu,\"repeated_draws\":256,\"repeated_reference_commands\":%llu,\"repeated_cached_commands\":%llu,\"gpu_executed\":false}\n",
        (unsigned long long)comparisons,(unsigned long long)resets,(unsigned long long)calls[0],(unsigned long long)calls[1],
        (unsigned long long)repeated_baseline,(unsigned long long)repeated_optimized);
    return 0;
}
