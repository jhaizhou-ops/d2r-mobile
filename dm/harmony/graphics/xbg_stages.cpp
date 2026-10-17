#include "xbg_stages.h"
#include <string.h>
extern "C" int xbg_graphics_stages(const VkShaderModule m[5],uint32_t depth,uint32_t colors,uint32_t mask,uint32_t a2c,VkPipelineShaderStageCreateInfo out[5],uint32_t *count){
    if(!m || !out || !count || colors>255)return -1;
    if(!m[0])return -2;
    if(bool(m[1])!=bool(m[2]))return -3;
    if(!m[4]){
        if(!depth)return -4;
        for(uint32_t i=0;i<8;i++)if((colors&(1u<<i)) && ((mask>>(4*i))&15))return -5;
        if(a2c)return -6;
    }
    VkPipelineShaderStageCreateInfo stages[5]{};uint32_t n=0;
    for(uint32_t i=0;i<5;i++)if(m[i]){auto &s=stages[n++];s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.stage=VkShaderStageFlagBits(1u<<i);s.module=m[i];s.pName="main";}
    memcpy(out,stages,n*sizeof(*out));*count=n;return 0;
}
