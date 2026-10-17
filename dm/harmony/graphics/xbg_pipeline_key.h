#pragma once
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

// Exact, pointer/padding-free keys for the backend's pipeline definitions.
// Unknown extension/specialization/immutable-sampler state bypasses the cache.
// No hash-only equality: the cache compares all serialized bytes.
struct XbgPipelineKeyView {const uint32_t *words;uint32_t count;bool valid;};
struct XbgPipelineKey {
    static constexpr uint32_t Capacity=4096;
    uint32_t words[Capacity];
    uint32_t count=0;
    bool valid=true;
    void u(uint32_t n){if(count<Capacity)words[count++]=n;else valid=false;}
    void q(uint64_t n){u(uint32_t(n));u(uint32_t(n>>32));}
    void f(float n){uint32_t bits;memcpy(&bits,&n,4);u(bits);}
    XbgPipelineKeyView view()const{return {words,count,valid};}
    void piece(XbgPipelineKeyView p){u(p.count);if(!p.valid)valid=false;for(uint32_t i=0;i<p.count;i++)u(p.words[i]);}
    bool same(const XbgPipelineKey &p)const{return valid && p.valid && count==p.count && !memcmp(words,p.words,size_t(count)*4);}
};

inline bool xbg_pipeline_layout_key(XbgPipelineKey &k,const VkDescriptorSetLayoutCreateInfo &s,const VkPipelineLayoutCreateInfo &p){
    if(s.pNext || p.pNext || p.setLayoutCount!=1 || (s.bindingCount && !s.pBindings) || (p.pushConstantRangeCount && !p.pPushConstantRanges))return false;
    k.u(s.flags);k.u(s.bindingCount);
    for(uint32_t i=0;i<s.bindingCount;i++){const auto &b=s.pBindings[i];if(b.pImmutableSamplers)return false;k.u(b.binding);k.u(b.descriptorType);k.u(b.descriptorCount);k.u(b.stageFlags);}
    k.u(p.flags);k.u(p.setLayoutCount);k.u(p.pushConstantRangeCount);
    for(uint32_t i=0;i<p.pushConstantRangeCount;i++){const auto &r=p.pPushConstantRanges[i];k.u(r.stageFlags);k.u(r.offset);k.u(r.size);}
    return k.valid;
}
inline void xbg_pipeline_ref_key(XbgPipelineKey &k,const VkAttachmentReference &r){k.u(r.attachment);k.u(r.layout);}
inline bool xbg_renderpass_key(XbgPipelineKey &k,const VkRenderPassCreateInfo &p){
    if(p.pNext || p.subpassCount!=1 || !p.pSubpasses || (p.attachmentCount && !p.pAttachments) || (p.dependencyCount && !p.pDependencies))return false;
    k.u(p.flags);k.u(p.attachmentCount);
    for(uint32_t i=0;i<p.attachmentCount;i++){const auto &a=p.pAttachments[i];k.u(a.flags);k.u(a.format);k.u(a.samples);k.u(a.loadOp);k.u(a.storeOp);k.u(a.stencilLoadOp);k.u(a.stencilStoreOp);k.u(a.initialLayout);k.u(a.finalLayout);}
    const auto &s=p.pSubpasses[0];
    if(s.inputAttachmentCount || s.pResolveAttachments || s.preserveAttachmentCount || (s.colorAttachmentCount && !s.pColorAttachments))return false;
    k.u(s.flags);k.u(s.pipelineBindPoint);k.u(s.colorAttachmentCount);
    for(uint32_t i=0;i<s.colorAttachmentCount;i++)xbg_pipeline_ref_key(k,s.pColorAttachments[i]);
    k.u(s.pDepthStencilAttachment!=nullptr);if(s.pDepthStencilAttachment)xbg_pipeline_ref_key(k,*s.pDepthStencilAttachment);
    k.u(p.dependencyCount);for(uint32_t i=0;i<p.dependencyCount;i++){const auto &d=p.pDependencies[i];k.u(d.srcSubpass);k.u(d.dstSubpass);k.u(d.srcStageMask);k.u(d.dstStageMask);k.u(d.srcAccessMask);k.u(d.dstAccessMask);k.u(d.dependencyFlags);}
    return k.valid;
}
// Optional stable shader-module identity (warm-up list); null keeps the raw handle.
inline uint64_t (*xbg_module_id_hook)(VkShaderModule)=nullptr;
inline bool xbg_stage_key(XbgPipelineKey &k,const VkPipelineShaderStageCreateInfo &s){
    if(s.pNext || s.pSpecializationInfo || !s.pName || strcmp(s.pName,"main"))return false;
    k.u(s.flags);k.u(s.stage);k.q(xbg_module_id_hook?xbg_module_id_hook(s.module):(uint64_t)s.module);return k.valid;
}
inline void xbg_stencil_key(XbgPipelineKey &k,const VkStencilOpState &s){k.u(s.failOp);k.u(s.passOp);k.u(s.depthFailOp);k.u(s.compareOp);k.u(s.compareMask);k.u(s.writeMask);k.u(s.reference);}
inline bool xbg_graphics_pipeline_key(XbgPipelineKey &k,const VkGraphicsPipelineCreateInfo &p,XbgPipelineKeyView layout,XbgPipelineKeyView pass){
    if(p.pNext || p.basePipelineHandle || (p.flags&VK_PIPELINE_CREATE_DERIVATIVE_BIT) || !p.pStages || !p.pVertexInputState || !p.pInputAssemblyState || !p.pViewportState || !p.pRasterizationState || !p.pMultisampleState || !p.pDepthStencilState || !p.pColorBlendState || !p.pDynamicState)return false;
    k.u(1);k.piece(layout);k.piece(pass);k.u(p.flags);k.u(p.subpass);k.u(p.stageCount);
    for(uint32_t i=0;i<p.stageCount;i++)if(!xbg_stage_key(k,p.pStages[i]))return false;
    const auto &v=*p.pVertexInputState;const auto &a=*p.pInputAssemblyState;const auto &vp=*p.pViewportState;const auto &r=*p.pRasterizationState;const auto &m=*p.pMultisampleState;const auto &d=*p.pDepthStencilState;const auto &b=*p.pColorBlendState;const auto &dy=*p.pDynamicState;
    if(v.pNext || a.pNext || vp.pNext || r.pNext || m.pNext || d.pNext || b.pNext || dy.pNext || (p.pTessellationState && p.pTessellationState->pNext))return false;
    if((v.vertexBindingDescriptionCount && !v.pVertexBindingDescriptions) || (v.vertexAttributeDescriptionCount && !v.pVertexAttributeDescriptions) || (b.attachmentCount && !b.pAttachments) || (dy.dynamicStateCount && !dy.pDynamicStates))return false;
    k.u(v.flags);k.u(v.vertexBindingDescriptionCount);for(uint32_t i=0;i<v.vertexBindingDescriptionCount;i++){const auto &x=v.pVertexBindingDescriptions[i];k.u(x.binding);k.u(x.stride);k.u(x.inputRate);}
    k.u(v.vertexAttributeDescriptionCount);for(uint32_t i=0;i<v.vertexAttributeDescriptionCount;i++){const auto &x=v.pVertexAttributeDescriptions[i];k.u(x.location);k.u(x.binding);k.u(x.format);k.u(x.offset);}
    k.u(a.flags);k.u(a.topology);k.u(a.primitiveRestartEnable);
    k.u(p.pTessellationState!=nullptr);if(p.pTessellationState){k.u(p.pTessellationState->flags);k.u(p.pTessellationState->patchControlPoints);}
    k.u(vp.flags);k.u(vp.viewportCount);k.u(vp.scissorCount);k.u(vp.pViewports!=nullptr);k.u(vp.pScissors!=nullptr);
    if(vp.pViewports)for(uint32_t i=0;i<vp.viewportCount;i++){const auto &x=vp.pViewports[i];k.f(x.x);k.f(x.y);k.f(x.width);k.f(x.height);k.f(x.minDepth);k.f(x.maxDepth);}
    if(vp.pScissors)for(uint32_t i=0;i<vp.scissorCount;i++){const auto &x=vp.pScissors[i];k.u(x.offset.x);k.u(x.offset.y);k.u(x.extent.width);k.u(x.extent.height);}
    k.u(r.flags);k.u(r.depthClampEnable);k.u(r.rasterizerDiscardEnable);k.u(r.polygonMode);k.u(r.cullMode);k.u(r.frontFace);k.u(r.depthBiasEnable);k.f(r.depthBiasConstantFactor);k.f(r.depthBiasClamp);k.f(r.depthBiasSlopeFactor);k.f(r.lineWidth);
    k.u(m.flags);k.u(m.rasterizationSamples);k.u(m.sampleShadingEnable);k.f(m.minSampleShading);k.u(m.alphaToCoverageEnable);k.u(m.alphaToOneEnable);k.u(m.pSampleMask!=nullptr);
    if(m.pSampleMask){if(m.rasterizationSamples>VK_SAMPLE_COUNT_64_BIT)return false;for(uint32_t i=0;i<(uint32_t(m.rasterizationSamples)+31)/32;i++)k.u(m.pSampleMask[i]);}
    k.u(d.flags);k.u(d.depthTestEnable);k.u(d.depthWriteEnable);k.u(d.depthCompareOp);k.u(d.depthBoundsTestEnable);k.u(d.stencilTestEnable);xbg_stencil_key(k,d.front);xbg_stencil_key(k,d.back);k.f(d.minDepthBounds);k.f(d.maxDepthBounds);
    k.u(b.flags);k.u(b.logicOpEnable);k.u(b.logicOp);k.u(b.attachmentCount);
    for(uint32_t i=0;i<b.attachmentCount;i++){const auto &x=b.pAttachments[i];k.u(x.blendEnable);k.u(x.srcColorBlendFactor);k.u(x.dstColorBlendFactor);k.u(x.colorBlendOp);k.u(x.srcAlphaBlendFactor);k.u(x.dstAlphaBlendFactor);k.u(x.alphaBlendOp);k.u(x.colorWriteMask);}
    for(float x:b.blendConstants)k.f(x);k.u(dy.flags);k.u(dy.dynamicStateCount);for(uint32_t i=0;i<dy.dynamicStateCount;i++)k.u(dy.pDynamicStates[i]);
    return k.valid;
}
inline bool xbg_compute_pipeline_key(XbgPipelineKey &k,const VkComputePipelineCreateInfo &p,XbgPipelineKeyView layout){
    if(p.pNext || p.basePipelineHandle || (p.flags&VK_PIPELINE_CREATE_DERIVATIVE_BIT))return false;
    k.u(2);k.piece(layout);k.u(p.flags);return xbg_stage_key(k,p.stage) && k.valid;
}
