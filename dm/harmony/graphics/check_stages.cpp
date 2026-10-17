#include "xbg_stages.h"
#include <string.h>
extern "C" int xbg_stages_selftest(){
    VkShaderModule m[5]={(VkShaderModule)0x111,0,0,0,0};
    VkPipelineShaderStageCreateInfo stages[5]{};uint32_t n=99;
    if(xbg_graphics_stages(m,1,0,~0u,0,stages,&n) || n!=1 || stages[0].stage!=VK_SHADER_STAGE_VERTEX_BIT || stages[0].module!=m[0] || strcmp(stages[0].pName,"main") || stages[0].pNext)return 1;
    if(xbg_graphics_stages(m,1,0x80,0x0fffffff,0,stages,&n) || n!=1)return 2; // inactive bits do not forbid depth-only
    for(unsigned i=0;i<8;i++)for(unsigned component=0;component<4;component++){
        if(xbg_graphics_stages(m,1,1u<<i,1u<<(4*i+component),0,stages,&n)!=-5)return 3;
    }
    n=55;stages[0].module=(VkShaderModule)0x999;
    if(xbg_graphics_stages(m,0,0,0,0,stages,&n)!=-4 || n!=55 || stages[0].module!=(VkShaderModule)0x999)return 4;
    if(xbg_graphics_stages(m,1,0,0,1,stages,&n)!=-6)return 5;
    m[4]=(VkShaderModule)0x555;
    if(xbg_graphics_stages(m,0,255,~0u,1,stages,&n) || n!=2 || stages[1].stage!=VK_SHADER_STAGE_FRAGMENT_BIT || stages[1].module!=m[4])return 6;
    m[1]=(VkShaderModule)0x222;
    if(xbg_graphics_stages(m,1,0,0,0,stages,&n)!=-3)return 7;
    m[2]=(VkShaderModule)0x333;m[3]=(VkShaderModule)0x444;
    if(xbg_graphics_stages(m,1,0,0,0,stages,&n) || n!=5)return 8;
    for(unsigned i=0;i<5;i++)if(stages[i].module!=m[i] || stages[i].stage!=1u<<i)return 9;
    m[0]=0;if(xbg_graphics_stages(m,1,0,0,0,stages,&n)!=-2)return 10;
    return 0;
}
