#include "xbg_pipeline_key.h"

struct Fixture {
    VkDescriptorSetLayoutBinding binding[2]={{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout layout_handle=(VkDescriptorSetLayout)0x100;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,4};
    VkAttachmentDescription attachment[2]{};
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_GENERAL},depth{1,VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub{};
    VkSubpassDependency dep{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT,0};
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    VkVertexInputAttributeDescription attr{0,0,VK_FORMAT_R32G32_SFLOAT,0};
    VkVertexInputBindingDescription stream{0,8,VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineTessellationStateCreateInfo tess{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    VkViewport viewport{0,900,1600,-900,0,1};VkRect2D scissor{{0,0},{1600,900}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkSampleMask sample_mask=~0u;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend_attachment{};
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dynamic[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    Fixture(){
        layout.bindingCount=2;layout.pBindings=binding;pl.setLayoutCount=1;pl.pSetLayouts=&layout_handle;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        attachment[0].format=VK_FORMAT_R8G8B8A8_UNORM;attachment[1].format=VK_FORMAT_D32_SFLOAT;
        attachment[0].samples=attachment[1].samples=VK_SAMPLE_COUNT_1_BIT;
        sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;sub.pDepthStencilAttachment=&depth;
        pass.attachmentCount=2;pass.pAttachments=attachment;pass.subpassCount=1;pass.pSubpasses=&sub;pass.dependencyCount=1;pass.pDependencies=&dep;
        for(int i=0;i<2;i++){stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=(VkShaderModule)(uintptr_t)(100+i);stages[i].pName="main";}
        vi.vertexBindingDescriptionCount=vi.vertexAttributeDescriptionCount=1;vi.pVertexBindingDescriptions=&stream;vi.pVertexAttributeDescriptions=&attr;
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;tess.patchControlPoints=3;
        vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        rs.lineWidth=1;rs.cullMode=VK_CULL_MODE_BACK_BIT;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;ms.pSampleMask=&sample_mask;
        blend.attachmentCount=1;blend.pAttachments=&blend_attachment;dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        p.stageCount=2;p.pStages=stages;p.pVertexInputState=&vi;p.pInputAssemblyState=&ia;p.pTessellationState=&tess;p.pViewportState=&vp;p.pRasterizationState=&rs;p.pMultisampleState=&ms;p.pDepthStencilState=&ds;p.pColorBlendState=&blend;p.pDynamicState=&dy;p.layout=(VkPipelineLayout)0x200;p.renderPass=(VkRenderPass)0x300;
    }
    bool key(XbgPipelineKey &out){XbgPipelineKey l,r;return xbg_pipeline_layout_key(l,layout,pl) && xbg_renderpass_key(r,pass) && xbg_graphics_pipeline_key(out,p,l.view(),r.view());}
};
static uint32_t checks;
extern "C" uint32_t xbg_pipeline_key_checks(){return checks;}
extern "C" int xbg_pipeline_key_selftest(){
    checks=0;Fixture f,copy;XbgPipelineKey baseline,equal;
    if(!f.key(baseline) || !copy.key(equal) || !baseline.same(equal))return 1;
    // Independently allocated arrays, recreated Vulkan layout/pass handles and
    // non-pointer padding must not turn an identical definition into a miss.
    copy.layout_handle=(VkDescriptorSetLayout)0x999;copy.p.layout=(VkPipelineLayout)0x998;copy.p.renderPass=(VkRenderPass)0x997;
    XbgPipelineKey handles;if(!copy.key(handles) || !baseline.same(handles))return 2;
    checks+=2;
#define CHANGE(field) {uint32_t old,bits;static_assert(sizeof(field)==4,"scalar");memcpy(&old,&(field),4);bits=old^1;memcpy(&(field),&bits,4);XbgPipelineKey changed;bool ok=f.key(changed);memcpy(&(field),&old,4);++checks;if(!ok || baseline.same(changed))return 100+int(checks);}
    CHANGE(f.binding[0].binding);CHANGE(f.binding[0].descriptorType);CHANGE(f.binding[0].descriptorCount);CHANGE(f.binding[0].stageFlags);
    CHANGE(f.layout.flags);CHANGE(f.pl.flags);CHANGE(f.push.stageFlags);CHANGE(f.push.offset);CHANGE(f.push.size);
    {auto old=f.layout.bindingCount;f.layout.bindingCount=1;XbgPipelineKey k;++checks;if(!f.key(k) || baseline.same(k))return 11;f.layout.bindingCount=old;}
    CHANGE(f.pass.flags);CHANGE(f.sub.flags);CHANGE(f.sub.pipelineBindPoint);CHANGE(f.color.attachment);CHANGE(f.color.layout);CHANGE(f.depth.attachment);CHANGE(f.depth.layout);
    CHANGE(f.attachment[0].flags);CHANGE(f.attachment[0].format);CHANGE(f.attachment[0].samples);CHANGE(f.attachment[0].loadOp);CHANGE(f.attachment[0].storeOp);CHANGE(f.attachment[0].stencilLoadOp);CHANGE(f.attachment[0].stencilStoreOp);CHANGE(f.attachment[0].initialLayout);CHANGE(f.attachment[0].finalLayout);CHANGE(f.attachment[1].format);
    CHANGE(f.dep.srcSubpass);CHANGE(f.dep.dstSubpass);CHANGE(f.dep.srcStageMask);CHANGE(f.dep.dstStageMask);CHANGE(f.dep.srcAccessMask);CHANGE(f.dep.dstAccessMask);CHANGE(f.dep.dependencyFlags);
    CHANGE(f.p.flags);CHANGE(f.p.subpass);CHANGE(f.stages[0].flags);CHANGE(f.stages[0].stage);CHANGE(f.stages[1].stage);
    CHANGE(f.vi.flags);CHANGE(f.vi.vertexBindingDescriptionCount);CHANGE(f.vi.vertexAttributeDescriptionCount);CHANGE(f.stream.binding);CHANGE(f.stream.stride);CHANGE(f.stream.inputRate);CHANGE(f.attr.location);CHANGE(f.attr.binding);CHANGE(f.attr.format);CHANGE(f.attr.offset);
    CHANGE(f.ia.flags);CHANGE(f.ia.topology);CHANGE(f.ia.primitiveRestartEnable);CHANGE(f.tess.flags);CHANGE(f.tess.patchControlPoints);
    CHANGE(f.vp.flags);CHANGE(f.vp.viewportCount);CHANGE(f.vp.scissorCount);CHANGE(f.viewport.x);CHANGE(f.viewport.y);CHANGE(f.viewport.width);CHANGE(f.viewport.height);CHANGE(f.viewport.minDepth);CHANGE(f.viewport.maxDepth);CHANGE(f.scissor.offset.x);CHANGE(f.scissor.offset.y);CHANGE(f.scissor.extent.width);CHANGE(f.scissor.extent.height);
    CHANGE(f.rs.flags);CHANGE(f.rs.depthClampEnable);CHANGE(f.rs.rasterizerDiscardEnable);CHANGE(f.rs.polygonMode);CHANGE(f.rs.cullMode);CHANGE(f.rs.frontFace);CHANGE(f.rs.depthBiasEnable);CHANGE(f.rs.depthBiasConstantFactor);CHANGE(f.rs.depthBiasClamp);CHANGE(f.rs.depthBiasSlopeFactor);CHANGE(f.rs.lineWidth);
    CHANGE(f.ms.flags);CHANGE(f.ms.rasterizationSamples);CHANGE(f.ms.sampleShadingEnable);CHANGE(f.ms.minSampleShading);CHANGE(f.ms.alphaToCoverageEnable);CHANGE(f.ms.alphaToOneEnable);CHANGE(f.sample_mask);
    CHANGE(f.ds.flags);CHANGE(f.ds.depthTestEnable);CHANGE(f.ds.depthWriteEnable);CHANGE(f.ds.depthCompareOp);CHANGE(f.ds.depthBoundsTestEnable);CHANGE(f.ds.stencilTestEnable);CHANGE(f.ds.minDepthBounds);CHANGE(f.ds.maxDepthBounds);
#define STENCIL(which) CHANGE(f.ds.which.failOp);CHANGE(f.ds.which.passOp);CHANGE(f.ds.which.depthFailOp);CHANGE(f.ds.which.compareOp);CHANGE(f.ds.which.compareMask);CHANGE(f.ds.which.writeMask);CHANGE(f.ds.which.reference)
    STENCIL(front);STENCIL(back);
    CHANGE(f.blend.flags);CHANGE(f.blend.logicOpEnable);CHANGE(f.blend.logicOp);CHANGE(f.blend_attachment.blendEnable);CHANGE(f.blend_attachment.srcColorBlendFactor);CHANGE(f.blend_attachment.dstColorBlendFactor);CHANGE(f.blend_attachment.colorBlendOp);CHANGE(f.blend_attachment.srcAlphaBlendFactor);CHANGE(f.blend_attachment.dstAlphaBlendFactor);CHANGE(f.blend_attachment.alphaBlendOp);CHANGE(f.blend_attachment.colorWriteMask);CHANGE(f.blend.blendConstants[0]);CHANGE(f.blend.blendConstants[1]);CHANGE(f.blend.blendConstants[2]);CHANGE(f.blend.blendConstants[3]);CHANGE(f.dy.flags);CHANGE(f.dynamic[0]);CHANGE(f.dynamic[1]);
#undef STENCIL
#undef CHANGE
    f.stages[0].module=(VkShaderModule)999;{XbgPipelineKey k;++checks;if(!f.key(k) || baseline.same(k))return 3;}f.stages[0].module=(VkShaderModule)100;
    f.vp.pViewports=nullptr;f.vp.pScissors=nullptr;XbgPipelineKey dynamic_base;if(!f.key(dynamic_base))return 4;
    f.viewport.height=999;f.scissor.extent.width=999;{XbgPipelineKey k;++checks;if(!f.key(k) || !dynamic_base.same(k))return 5;}
    // Unsupported extension data is a bypass, never a false cache hit.
#define REJECT(field,value) {auto old=field;field=value;XbgPipelineKey k;++checks;bool accepted=f.key(k);field=old;if(accepted)return 300+int(checks);}
    REJECT(f.layout.pNext,&f);REJECT(f.pl.pNext,&f);REJECT(f.pass.pNext,&f);REJECT(f.stages[0].pNext,&f);REJECT(f.p.pNext,&f);REJECT(f.vi.pNext,&f);REJECT(f.ia.pNext,&f);REJECT(f.vp.pNext,&f);REJECT(f.rs.pNext,&f);REJECT(f.ms.pNext,&f);REJECT(f.ds.pNext,&f);REJECT(f.blend.pNext,&f);REJECT(f.dy.pNext,&f);REJECT(f.tess.pNext,&f);
    VkSpecializationInfo spec{};REJECT(f.stages[0].pSpecializationInfo,&spec);REJECT(f.stages[0].pName,"different");VkSampler immutable{};REJECT(f.binding[0].pImmutableSamplers,&immutable);
    REJECT(f.sub.pResolveAttachments,&f.color);REJECT(f.sub.inputAttachmentCount,1u);REJECT(f.sub.preserveAttachmentCount,1u);REJECT(f.p.basePipelineHandle,(VkPipeline)1);REJECT(f.p.flags,VK_PIPELINE_CREATE_DERIVATIVE_BIT);
#undef REJECT
    XbgPipelineKey layout; if(!xbg_pipeline_layout_key(layout,f.layout,f.pl))return 6;
    VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};compute.stage=f.stages[0];compute.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    XbgPipelineKey ca,cb;compute.layout=(VkPipelineLayout)1;if(!xbg_compute_pipeline_key(ca,compute,layout.view()))return 7;compute.layout=(VkPipelineLayout)2;
    ++checks;if(!xbg_compute_pipeline_key(cb,compute,layout.view()) || !ca.same(cb) || ca.same(dynamic_base))return 8;
    compute.stage.module=(VkShaderModule)333;{XbgPipelineKey k;++checks;if(!xbg_compute_pipeline_key(k,compute,layout.view()) || ca.same(k))return 9;}
    XbgPipelineKey overflow;for(uint32_t i=0;i<=XbgPipelineKey::Capacity;i++)overflow.u(i);++checks;if(overflow.valid || overflow.count!=XbgPipelineKey::Capacity)return 10;
    return 0;
}
