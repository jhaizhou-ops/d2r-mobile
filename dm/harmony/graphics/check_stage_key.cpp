#include "xbg_stages.h"
#include "xbg_pipeline_key.h"
/* Key-only fixtures: these are not complete runnable Vulkan pipelines. */
static XbgPipelineKey depth,fragment,repeated;
extern "C" int xbg_stage_key_selftest(){
    depth={};fragment={};repeated={};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    p.pVertexInputState=&vi;p.pInputAssemblyState=&ia;p.pViewportState=&vp;p.pRasterizationState=&rs;p.pMultisampleState=&ms;p.pDepthStencilState=&ds;p.pColorBlendState=&blend;p.pDynamicState=&dynamic;
    VkShaderModule modules[5]={(VkShaderModule)0x111,0,0,0,0};VkPipelineShaderStageCreateInfo stages[5]{};p.pStages=stages;
    XbgPipelineKeyView definition{nullptr,0,true};
    if(xbg_graphics_stages(modules,1,0,0,0,stages,&p.stageCount) || !xbg_graphics_pipeline_key(depth,p,definition,definition))return 1;
    modules[4]=(VkShaderModule)0x555;
    if(xbg_graphics_stages(modules,1,0,0,0,stages,&p.stageCount) || !xbg_graphics_pipeline_key(fragment,p,definition,definition) || depth.same(fragment))return 2;
    modules[4]=VK_NULL_HANDLE;
    if(xbg_graphics_stages(modules,1,0,0,0,stages,&p.stageCount) || !xbg_graphics_pipeline_key(repeated,p,definition,definition) || !depth.same(repeated))return 3;
    return 0;
}
