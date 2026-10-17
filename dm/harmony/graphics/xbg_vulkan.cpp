#include "xbg_vulkan.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace {
struct Failure { VkResult result; std::string message; };
void require(bool yes,const char *message) { if(!yes) throw Failure{VK_ERROR_INITIALIZATION_FAILED,message}; }
void vkcheck(VkResult result,const char *call) {
    if(result!=VK_SUCCESS) throw Failure{result,std::string(call)+": "+std::to_string(result)};
}
void message(char *out,size_t n,const std::string &s) { if(out && n) std::snprintf(out,n,"%s",s.c_str()); }
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; VkDeviceSize size{},allocation{}; bool coherent{}; };
struct Image { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; VkFormat format{}; uint32_t width{},height{},levels{}; };
uint32_t memory_type(VkPhysicalDevice physical,uint32_t bits,VkMemoryPropertyFlags need,VkMemoryPropertyFlags prefer) {
    VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(physical,&p);
    for(unsigned pass=0;pass<2;pass++) for(uint32_t i=0;i<p.memoryTypeCount;i++)
        if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&need)==need &&
            (pass || (p.memoryTypes[i].propertyFlags&prefer)==prefer)) return i;
    throw Failure{VK_ERROR_FEATURE_NOT_PRESENT,"no compatible Vulkan memory type"};
}
void free_buffer(VkDevice d,Buffer &b) {
    if(b.buffer) vkDestroyBuffer(d,b.buffer,nullptr); if(b.memory) vkFreeMemory(d,b.memory,nullptr); b={};
}
void free_image(VkDevice d,Image &i) {
    if(i.view) vkDestroyImageView(d,i.view,nullptr); if(i.image) vkDestroyImage(d,i.image,nullptr);
    if(i.memory) vkFreeMemory(d,i.memory,nullptr); i={};
}
VkFormat pixel_format(uint32_t p) {
    switch(p) {case XBG_RGBA8_UNORM:return VK_FORMAT_R8G8B8A8_UNORM;case XBG_RGBA8_SRGB:return VK_FORMAT_R8G8B8A8_SRGB;
        case XBG_R16_UNORM:return VK_FORMAT_R16_UNORM;case XBG_RG16_UNORM:return VK_FORMAT_R16G16_UNORM;
        case XBG_R16_SNORM:return VK_FORMAT_R16_SNORM;case XBG_RG16_SNORM:return VK_FORMAT_R16G16_SNORM;
        case XBG_RGBA16_SFLOAT:return VK_FORMAT_R16G16B16A16_SFLOAT;default:return VK_FORMAT_UNDEFINED;}
}
void inspect(const uint32_t *w,size_t bytes,VkShaderStageFlags stage,xbg_vk_requirements &r) {
    require(w && bytes>=20 && bytes%4==0 && w[0]==0x07230203,"invalid SPIR-V header");
    bool entry=false,subgroup=false;
    for(size_t i=5;i<bytes/4;) {
        uint32_t n=w[i]>>16,op=w[i]&65535; require(n && i+n<=bytes/4,"truncated SPIR-V instruction");
        if(op==15) {require(n>=4,"invalid entry point");require(w[i+1]==(stage==VK_SHADER_STAGE_VERTEX_BIT?0u:4u),"wrong 2D shader stage");entry=true;}
        if(op>=333 && op<=366) subgroup=true;
        require(op!=10,"unexpected SPIR-V extension in fixed 2D pipeline");
        if(op==17) {
            require(n==2,"invalid capability");
            switch(w[i+1]) {
            case 1: case 46: case 47: case 50: break; /* core image/query/buffer */
            case 25:r.core.shaderImageGatherExtended=VK_TRUE;break;
            case 32:r.core.shaderClipDistance=VK_TRUE;break;
            case 33:r.core.shaderCullDistance=VK_TRUE;break;
            case 61:r.subgroup_operations|=VK_SUBGROUP_FEATURE_BASIC_BIT;break;
            case 62:r.subgroup_operations|=VK_SUBGROUP_FEATURE_VOTE_BIT;break;
            case 63:r.subgroup_operations|=VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;break;
            case 64:r.subgroup_operations|=VK_SUBGROUP_FEATURE_BALLOT_BIT;break;
            case 65:r.subgroup_operations|=VK_SUBGROUP_FEATURE_SHUFFLE_BIT;break;
            case 66:r.subgroup_operations|=VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;break;
            case 4427:r.shader_draw_parameters=VK_TRUE;break;
            default:throw Failure{VK_ERROR_FEATURE_NOT_PRESENT,"unhandled 2D SPIR-V capability "+std::to_string(w[i+1])};
            }
        } i+=n;
    }
    require(entry,"missing SPIR-V entry point"); if(subgroup) r.subgroup_stages|=stage;
}
void core_available(const VkPhysicalDeviceFeatures &need,const VkPhysicalDeviceFeatures &have) {
    require(!need.shaderImageGatherExtended || have.shaderImageGatherExtended,"shaderImageGatherExtended not supported/enabled");
    require(!need.shaderClipDistance || have.shaderClipDistance,"shaderClipDistance not supported/enabled");
    require(!need.shaderCullDistance || have.shaderCullDistance,"shaderCullDistance not supported/enabled");
}
}

struct xbg_vk_texture { Image image; xbg_vk_renderer *owner{}; bool uploaded{}; };
struct xbg_vk_renderer {
    xbg_vk_info info{}; xbg_vk_requirements requirements{}; std::string error;
    VkCommandPool pool{}; VkCommandBuffer command{}; VkFence fence{}; bool pending{};
    VkShaderModule vs{},ps{}; VkDescriptorSetLayout set_layout{}; VkPipelineLayout layout{};
    VkDescriptorPool descriptor_pool{}; VkRenderPass render_pass{}; VkFramebuffer framebuffer{};
    VkPipeline pipeline{}; VkSampler sampler{};
    std::vector<Buffer> transient; std::vector<xbg_vk_texture*> textures;
    Image target; Buffer readback; bool output_ready{}; xbg_vk_texture *upload_pending{};
    void cleanup_transient() {
        for(auto &b:transient) free_buffer(info.device,b); transient.clear();
        if(pipeline) vkDestroyPipeline(info.device,pipeline,nullptr);pipeline={};
        if(framebuffer) vkDestroyFramebuffer(info.device,framebuffer,nullptr);framebuffer={};
        if(render_pass) vkDestroyRenderPass(info.device,render_pass,nullptr);render_pass={};
        if(descriptor_pool) vkDestroyDescriptorPool(info.device,descriptor_pool,nullptr);descriptor_pool={};
        if(sampler) vkDestroySampler(info.device,sampler,nullptr);sampler={};
    }
    Buffer make_buffer(VkDeviceSize bytes,VkBufferUsageFlags usage) {
        Buffer b{};b.size=bytes;
        try {
            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            vkcheck(vkCreateBuffer(info.device,&ci,nullptr,&b.buffer),"vkCreateBuffer");
            VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(info.device,b.buffer,&mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;
            ai.memoryTypeIndex=memory_type(info.physical,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(info.physical,&mp);
            b.coherent=(mp.memoryTypes[ai.memoryTypeIndex].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;b.allocation=mr.size;
            vkcheck(vkAllocateMemory(info.device,&ai,nullptr,&b.memory),"vkAllocateMemory(buffer)");
            vkcheck(vkBindBufferMemory(info.device,b.buffer,b.memory,0),"vkBindBufferMemory");return b;
        } catch(...) {free_buffer(info.device,b);throw;}
    }
    void write(const Buffer &b,const void *data,size_t bytes,bool zero_tail=false) {
        require(bytes<=b.size && (data || !bytes),"invalid buffer write");void *p=nullptr;
        vkcheck(vkMapMemory(info.device,b.memory,0,VK_WHOLE_SIZE,0,&p),"vkMapMemory(write)");
        if(zero_tail) std::memset(p,0,(size_t)b.size);if(bytes) std::memcpy(p,data,bytes);
        VkResult result=VK_SUCCESS;
        if(!b.coherent) {VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};r.memory=b.memory;r.size=VK_WHOLE_SIZE;result=vkFlushMappedMemoryRanges(info.device,1,&r);}
        vkUnmapMemory(info.device,b.memory);vkcheck(result,"vkFlushMappedMemoryRanges");
    }
    Image make_image(uint32_t w,uint32_t h,uint32_t levels,VkFormat format,VkImageUsageFlags usage) {
        Image image{};image.width=w;image.height=h;image.levels=levels;image.format=format;
        try {
            VkImageFormatProperties props{};
            vkcheck(vkGetPhysicalDeviceImageFormatProperties(info.physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&props),"vkGetPhysicalDeviceImageFormatProperties");
            require(w && h && w<=props.maxExtent.width && h<=props.maxExtent.height && levels<=props.maxMipLevels,"image exceeds device limits");
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={w,h,1};ci.mipLevels=levels;ci.arrayLayers=1;
            ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            vkcheck(vkCreateImage(info.device,&ci,nullptr,&image.image),"vkCreateImage");
            VkMemoryRequirements mr{};vkGetImageMemoryRequirements(info.device,image.image,&mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=memory_type(info.physical,mr.memoryTypeBits,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkcheck(vkAllocateMemory(info.device,&ai,nullptr,&image.memory),"vkAllocateMemory(image)");
            vkcheck(vkBindImageMemory(info.device,image.image,image.memory,0),"vkBindImageMemory");
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
            vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,1};
            vkcheck(vkCreateImageView(info.device,&vi,nullptr,&image.view),"vkCreateImageView");return image;
        } catch(...) {free_image(info.device,image);throw;}
    }
    void begin() {
        require(!pending,"GPU submission still pending; retry xbg_vk_wait");cleanup_transient();
        vkcheck(vkResetCommandPool(info.device,pool,0),"vkResetCommandPool");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkcheck(vkBeginCommandBuffer(command,&bi),"vkBeginCommandBuffer");
    }
    void image_barrier(const Image &i,VkImageLayout before,VkImageLayout after,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkAccessFlags sa,VkAccessFlags da) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=sa;b.dstAccessMask=da;b.oldLayout=before;b.newLayout=after;
        b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=i.image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,i.levels,0,1};
        vkCmdPipelineBarrier(command,src,dst,0,0,nullptr,0,nullptr,1,&b);
    }
    int submit() {
        vkcheck(vkEndCommandBuffer(command),"vkEndCommandBuffer");vkcheck(vkResetFences(info.device,1,&fence),"vkResetFences");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&command;
        vkcheck(vkQueueSubmit(info.queue,1,&si,fence),"vkQueueSubmit");pending=true;return xbg_vk_wait(this);
    }
};

extern "C" int xbg_vk_query_2d(VkPhysicalDevice physical,const uint32_t *vs,size_t vb,const uint32_t *ps,size_t pb,xbg_vk_requirements *out,char *err,size_t en) {
    try {
        require(physical && out,"null physical device or output");*out={};inspect(vs,vb,VK_SHADER_STAGE_VERTEX_BIT,*out);inspect(ps,pb,VK_SHADER_STAGE_FRAGMENT_BIT,*out);
        out->subgroup.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
        VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&out->subgroup;vkGetPhysicalDeviceProperties2(physical,&p);out->properties=p.properties;
        require(p.properties.apiVersion>=VK_API_VERSION_1_1,"Vulkan 1.1 required for supplied SPIR-V 1.3");
        VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f.pNext=&draw;vkGetPhysicalDeviceFeatures2(physical,&f);
        core_available(out->core,f.features);require(!out->shader_draw_parameters || draw.shaderDrawParameters,"shaderDrawParameters not supported");
        require((out->subgroup.supportedOperations&out->subgroup_operations)==out->subgroup_operations,"required subgroup operations not supported");
        require((out->subgroup.supportedStages&out->subgroup_stages)==out->subgroup_stages,"executed subgroup stage not supported");
        const auto &l=p.properties.limits;
        require(l.maxUniformBufferRange>=65536 && l.maxBoundDescriptorSets>=1 && l.maxPerStageDescriptorUniformBuffers>=2 &&
            l.maxDescriptorSetUniformBuffers>=4 && l.maxPerStageDescriptorSamplers>=1 && l.maxPerStageDescriptorSampledImages>=1 &&
            l.maxDescriptorSetSamplers>=1 && l.maxDescriptorSetSampledImages>=1 && l.maxVertexInputAttributes>=3 && l.maxVertexInputBindings>=1,
            "real 2D shader descriptor/vertex limits unsupported");
        message(err,en,"");return VK_SUCCESS;
    } catch(const Failure &f) {message(err,en,f.message);return f.result;} catch(...) {message(err,en,"allocation failure");return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
extern "C" int xbg_vk_create(const xbg_vk_info *info,xbg_vk_renderer **out,char *err,size_t en) {
    xbg_vk_renderer *r=nullptr;if(out)*out=nullptr;
    try {
        require(info && out && info->device && info->queue && info->fence_timeout_ns,"invalid renderer creation arguments");
        r=new xbg_vk_renderer;r->info=*info;r->transient.reserve(8);
        char failure[256];int result=xbg_vk_query_2d(info->physical,info->vs_words,info->vs_bytes,info->ps_words,info->ps_bytes,&r->requirements,failure,sizeof(failure));
        if(result) throw Failure{(VkResult)result,failure};core_available(r->requirements.core,info->enabled_core);
        require(!r->requirements.shader_draw_parameters || info->enabled_shader_draw_parameters,"shaderDrawParameters must be enabled at device creation");
        uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(info->physical,&count,nullptr);std::vector<VkQueueFamilyProperties> q(count);
        vkGetPhysicalDeviceQueueFamilyProperties(info->physical,&count,q.data());require(info->queue_family<count && (q[info->queue_family].queueFlags&VK_QUEUE_GRAPHICS_BIT),"queue family lacks graphics");
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=info->queue_family;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkcheck(vkCreateCommandPool(info->device,&pool,nullptr,&r->pool),"vkCreateCommandPool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=r->pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
        vkcheck(vkAllocateCommandBuffers(info->device,&ca,&r->command),"vkAllocateCommandBuffers");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkcheck(vkCreateFence(info->device,&fc,nullptr,&r->fence),"vkCreateFence");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=info->vs_bytes;sm.pCode=info->vs_words;
        vkcheck(vkCreateShaderModule(info->device,&sm,nullptr,&r->vs),"vkCreateShaderModule(real VS)");sm.codeSize=info->ps_bytes;sm.pCode=info->ps_words;
        vkcheck(vkCreateShaderModule(info->device,&sm,nullptr,&r->ps),"vkCreateShaderModule(real PS)");
        VkDescriptorSetLayoutBinding bindings[]={
            {0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
            {4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {6,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},
            {10,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {16,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=5;dl.pBindings=bindings;
        vkcheck(vkCreateDescriptorSetLayout(info->device,&dl,nullptr,&r->set_layout),"vkCreateDescriptorSetLayout");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&r->set_layout;
        vkcheck(vkCreatePipelineLayout(info->device,&pl,nullptr,&r->layout),"vkCreatePipelineLayout");
        *out=r;message(err,en,"");return VK_SUCCESS;
    } catch(const Failure &f) {message(err,en,f.message);if(r)xbg_vk_destroy(r);return f.result;}
      catch(...) {message(err,en,"allocation failure");if(r)xbg_vk_destroy(r);return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
extern "C" int xbg_vk_wait(xbg_vk_renderer *r) {
    if(!r)return VK_ERROR_INITIALIZATION_FAILED;if(!r->pending)return VK_SUCCESS;
    VkResult result=vkWaitForFences(r->info.device,1,&r->fence,VK_TRUE,r->info.fence_timeout_ns);
    if(result==VK_SUCCESS) {r->pending=false;if(r->upload_pending){r->upload_pending->uploaded=true;r->upload_pending=nullptr;}}
    else r->error="vkWaitForFences: "+std::to_string(result)+"; resources retained";
    return result;
}
extern "C" int xbg_vk_destroy(xbg_vk_renderer *r) {
    if(!r)return VK_SUCCESS;int result=xbg_vk_wait(r);if(result && result!=VK_ERROR_DEVICE_LOST)return result;
    VkDevice d=r->info.device;r->cleanup_transient();for(auto *t:r->textures){free_image(d,t->image);delete t;}
    free_image(d,r->target);free_buffer(d,r->readback);
    if(r->layout)vkDestroyPipelineLayout(d,r->layout,nullptr);if(r->set_layout)vkDestroyDescriptorSetLayout(d,r->set_layout,nullptr);
    if(r->vs)vkDestroyShaderModule(d,r->vs,nullptr);if(r->ps)vkDestroyShaderModule(d,r->ps,nullptr);
    if(r->fence)vkDestroyFence(d,r->fence,nullptr);if(r->pool)vkDestroyCommandPool(d,r->pool,nullptr);delete r;return VK_SUCCESS;
}
extern "C" const char *xbg_vk_error(const xbg_vk_renderer *r) {return r?r->error.c_str():"null renderer";}

static int upload_texture(xbg_vk_renderer *r,const xbg_nvn_texture_desc *nvn,const void *data,size_t bytes,xbg_vk_texture **out) {
    if(out)*out=nullptr;if(!r)return VK_ERROR_INITIALIZATION_FAILED;xbg_vk_texture *texture=nullptr;
    try {
        require(out && data,"null texture source/output");xbg_texture t{};
        require((nvn?xbg_nvn_texture_layout(nvn,bytes,&t):xbg_texture_parse(data,bytes,&t))==0,"invalid/unsupported original texture storage");
        size_t total=0;std::vector<VkBufferImageCopy> regions;
        for(uint32_t l=0;l<t.levels;l++) {
            total=(total+3)&~size_t(3);VkBufferImageCopy c{};c.bufferOffset=total;c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,0,1};
            c.imageExtent={std::max(t.width>>l,1u),std::max(t.height>>l,1u),1};regions.push_back(c);total+=xbg_texture_mip_bytes(&t,l);
        }
        require(total<=256u*1024u*1024u,"decoded texture exceeds 256 MiB allocation bound");std::vector<uint8_t> decoded(total);
        for(uint32_t l=0;l<t.levels;l++) {
            void *dst=decoded.data()+regions[l].bufferOffset;size_t size=xbg_texture_mip_bytes(&t,l);
            require(!(nvn?xbg_nvn_texture_decode_mip(data,bytes,nvn,l,dst,size):xbg_texture_decode_mip(data,bytes,&t,l,dst,size)),"texture decode failed");
        }
        r->begin();r->transient.push_back(r->make_buffer(total,VK_BUFFER_USAGE_TRANSFER_SRC_BIT));r->write(r->transient.back(),decoded.data(),total);
        texture=new xbg_vk_texture;texture->owner=r;
        texture->image=r->make_image(t.width,t.height,t.levels,pixel_format(t.pixels),VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        r->textures.push_back(texture);r->upload_pending=texture;*out=texture;
        r->image_barrier(texture->image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(r->command,r->transient.back().buffer,texture->image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,(uint32_t)regions.size(),regions.data());
        r->image_barrier(texture->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        return r->submit();
    } catch(const Failure &f) {r->error=f.message;if(texture && !*out){free_image(r->info.device,texture->image);delete texture;}return f.result;}
      catch(...) {r->error="texture allocation failure";if(texture && (!out || !*out)){free_image(r->info.device,texture->image);delete texture;}return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
extern "C" int xbg_vk_upload_texture(xbg_vk_renderer *r,const void *data,size_t bytes,xbg_vk_texture **out) {
    return upload_texture(r,nullptr,data,bytes,out);
}
extern "C" int xbg_vk_upload_nvn_texture(xbg_vk_renderer *r,const xbg_nvn_texture_desc *desc,const void *data,size_t bytes,xbg_vk_texture **out) {
    if(!desc)return VK_ERROR_INITIALIZATION_FAILED;return upload_texture(r,desc,data,bytes,out);
}
extern "C" int xbg_vk_release_texture(xbg_vk_renderer *r,xbg_vk_texture *t) {
    if(!r || !t || t->owner!=r)return VK_ERROR_INITIALIZATION_FAILED;if(r->pending)return VK_NOT_READY;
    auto it=std::find(r->textures.begin(),r->textures.end(),t);if(it==r->textures.end())return VK_ERROR_INITIALIZATION_FAILED;
    free_image(r->info.device,t->image);r->textures.erase(it);if(r->upload_pending==t)r->upload_pending=nullptr;delete t;return VK_SUCCESS;
}

extern "C" int xbg_vk_draw_2d(xbg_vk_renderer *r,const xbg_draw2d *d) {
    if(!r)return VK_ERROR_INITIALIZATION_FAILED;
    try {
        require(d && d->vertices && d->vertex_count && d->vertex_count<=1048576,"invalid vertex stream");
        require(d->texture10 && d->texture10->owner==r && d->texture10->uploaded,"texture upload not complete");
        require(d->vertex_uniform0 && d->vertex_uniform_bytes>=64 && d->vertex_uniform_bytes<=65536 &&
            d->fragment_uniform0 && d->fragment_uniform_bytes>=32 && d->fragment_uniform_bytes<=65536 && d->vertex_support && d->fragment_support,"incomplete real shader constants");
        require(d->index_count<=3145728 && (!d->index_count || d->indices),"invalid index stream");
        require((d->index_count?d->index_count:d->vertex_count)%3==0,"triangle-list count not divisible by 3");
        for(uint32_t i=0;i<d->index_count;i++)require(d->indices[i]<d->vertex_count,"index exceeds vertex stream");
        require(d->target_format==VK_FORMAT_R8G8B8A8_UNORM || d->target_format==VK_FORMAT_R8G8B8A8_SRGB,"unsupported 2D target format");
        require(d->width && d->height && d->width<=8192 && d->height<=8192 && (uint64_t)d->width*d->height*4<=256u*1024u*1024u,"target extent exceeds bounds");
        const auto &limits=r->requirements.properties.limits;
        require(d->width<=limits.maxViewportDimensions[0] && d->height<=limits.maxViewportDimensions[1] &&
            d->width<=limits.maxFramebufferWidth && d->height<=limits.maxFramebufferHeight,"target exceeds viewport/framebuffer limits");
        require(d->scissor.offset.x>=0 && d->scissor.offset.y>=0 && (uint64_t)d->scissor.offset.x+d->scissor.extent.width<=d->width &&
            (uint64_t)d->scissor.offset.y+d->scissor.extent.height<=d->height,"scissor exceeds target");
        require((d->min_filter==VK_FILTER_NEAREST || d->min_filter==VK_FILTER_LINEAR) &&
            (d->mag_filter==VK_FILTER_NEAREST || d->mag_filter==VK_FILTER_LINEAR) &&
            (d->mipmap_mode==VK_SAMPLER_MIPMAP_MODE_NEAREST || d->mipmap_mode==VK_SAMPLER_MIPMAP_MODE_LINEAR),"unsupported filter");
        require(d->address_u>=VK_SAMPLER_ADDRESS_MODE_REPEAT && d->address_u<=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
            d->address_v>=VK_SAMPLER_ADDRESS_MODE_REPEAT && d->address_v<=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,"unsupported address mode");
        VkBlendFactor factors[]={d->blend.srcColorBlendFactor,d->blend.dstColorBlendFactor,d->blend.srcAlphaBlendFactor,d->blend.dstAlphaBlendFactor};
        for(auto f:factors)require(f>=VK_BLEND_FACTOR_ZERO && f<VK_BLEND_FACTOR_SRC1_COLOR,"unsupported blend factor; real 2D PS has one color output");
        require(d->blend.colorBlendOp>=VK_BLEND_OP_ADD && d->blend.colorBlendOp<=VK_BLEND_OP_MAX &&
            d->blend.alphaBlendOp>=VK_BLEND_OP_ADD && d->blend.alphaBlendOp<=VK_BLEND_OP_MAX && !(d->blend.colorWriteMask&~15u),"unsupported blend operation/mask");
        require(std::isfinite(d->min_lod) && std::isfinite(d->max_lod) && std::isfinite(d->lod_bias) && d->min_lod>=0 && d->max_lod>=d->min_lod &&
            std::abs(d->lod_bias)<=r->requirements.properties.limits.maxSamplerLodBias,"invalid sampler LOD");
        float gamma=0;std::memcpy(&gamma,(const uint8_t*)d->fragment_uniform0+20,4);require(std::isfinite(gamma) && gamma>0,"PS gamma must be positive");
        VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(r->info.physical,d->texture10->image.format,&fp);
        require((fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
            ((d->min_filter!=VK_FILTER_LINEAR && d->mag_filter!=VK_FILTER_LINEAR && d->mipmap_mode!=VK_SAMPLER_MIPMAP_MODE_LINEAR) ||
             (fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)),"texture format sampling unsupported");
        vkGetPhysicalDeviceFormatProperties(r->info.physical,d->target_format,&fp);
        require((fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && (!d->blend.blendEnable || (fp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)),"target color/blend format unsupported");
        bool same=r->target.image && r->target.width==d->width && r->target.height==d->height && r->target.format==d->target_format;
        require(!d->load_previous || (same && r->output_ready),"cannot load an absent previous target");r->begin();r->output_ready=false;
        if(!same) {free_image(r->info.device,r->target);free_buffer(r->info.device,r->readback);
            r->target=r->make_image(d->width,d->height,1,d->target_format,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
            r->readback=r->make_buffer((VkDeviceSize)d->width*d->height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);}
        const void *buffers[]={d->vertex_support,d->fragment_support,d->vertex_uniform0,d->fragment_uniform0,d->vertices,d->indices};
        size_t sizes[]={1392,1392,d->vertex_uniform_bytes,d->fragment_uniform_bytes,(size_t)d->vertex_count*sizeof(xbg_vertex2d),(size_t)d->index_count*2};
        for(unsigned i=0;i<6;i++) {
            if(i==5 && !d->index_count)break;size_t allocation=(i==2 || i==3)?65536:sizes[i];
            VkBufferUsageFlags usage=i<4?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:i==4?VK_BUFFER_USAGE_VERTEX_BUFFER_BIT:VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            r->transient.push_back(r->make_buffer(allocation,usage));r->write(r->transient.back(),buffers[i],sizes[i],i==2 || i==3);
        }
        VkSamplerCreateInfo sa{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sa.magFilter=d->mag_filter;sa.minFilter=d->min_filter;sa.mipmapMode=d->mipmap_mode;
        sa.addressModeU=d->address_u;sa.addressModeV=d->address_v;sa.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sa.minLod=d->min_lod;sa.maxLod=d->max_lod;sa.mipLodBias=d->lod_bias;
        vkcheck(vkCreateSampler(r->info.device,&sa,nullptr,&r->sampler),"vkCreateSampler");
        VkDescriptorPoolSize pool_sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,4},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=pool_sizes;
        vkcheck(vkCreateDescriptorPool(r->info.device,&dp,nullptr,&r->descriptor_pool),"vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=r->descriptor_pool;da.descriptorSetCount=1;da.pSetLayouts=&r->set_layout;
        VkDescriptorSet set{};vkcheck(vkAllocateDescriptorSets(r->info.device,&da,&set),"vkAllocateDescriptorSets");
        VkDescriptorBufferInfo db[4]{};VkWriteDescriptorSet writes[5]{};uint32_t bindings[]={0,4,6,16};
        for(unsigned i=0;i<4;i++) {db[i]={r->transient[i].buffer,0,r->transient[i].size};writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet=set;writes[i].dstBinding=bindings[i];writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;writes[i].pBufferInfo=&db[i];}
        VkDescriptorImageInfo di{r->sampler,d->texture10->image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[4].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[4].dstSet=set;writes[4].dstBinding=10;writes[4].descriptorCount=1;writes[4].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[4].pImageInfo=&di;
        vkUpdateDescriptorSets(r->info.device,5,writes,0,nullptr);
        VkAttachmentDescription attachment{};attachment.format=d->target_format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=d->load_previous?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=d->load_previous?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        VkSubpassDependency deps[2]{};deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;deps[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;deps[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;deps[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;deps[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=deps;
        vkcheck(vkCreateRenderPass(r->info.device,&rp,nullptr,&r->render_pass),"vkCreateRenderPass");
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=r->render_pass;fb.attachmentCount=1;fb.pAttachments=&r->target.view;fb.width=d->width;fb.height=d->height;fb.layers=1;
        vkcheck(vkCreateFramebuffer(r->info.device,&fb,nullptr,&r->framebuffer),"vkCreateFramebuffer");
        VkPipelineShaderStageCreateInfo stages[2]{};for(unsigned i=0;i<2;i++) {stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=i?r->ps:r->vs;stages[i].pName="main";}
        VkVertexInputBindingDescription vb{0,sizeof(xbg_vertex2d),VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attr[]={{0,0,VK_FORMAT_R32G32_SFLOAT,offsetof(xbg_vertex2d,position)},{1,0,VK_FORMAT_R32G32_SFLOAT,offsetof(xbg_vertex2d,uv)},{2,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(xbg_vertex2d,color)}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&vb;vi.vertexAttributeDescriptionCount=3;vi.pVertexAttributeDescriptions=attr;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,(float)d->width,(float)d->height,0,1};VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&d->scissor;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&d->blend;std::memcpy(cb.blendConstants,d->blend_constants,sizeof(cb.blendConstants));
        VkGraphicsPipelineCreateInfo pc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pc.stageCount=2;pc.pStages=stages;pc.pVertexInputState=&vi;pc.pInputAssemblyState=&ia;
        pc.pViewportState=&vp;pc.pRasterizationState=&rs;pc.pMultisampleState=&ms;pc.pColorBlendState=&cb;pc.layout=r->layout;pc.renderPass=r->render_pass;
        vkcheck(vkCreateGraphicsPipelines(r->info.device,VK_NULL_HANDLE,1,&pc,nullptr,&r->pipeline),"vkCreateGraphicsPipelines(real 2D)");
        VkClearValue clear{};clear.color=d->clear;VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=r->render_pass;begin.framebuffer=r->framebuffer;begin.renderArea={{0,0},{d->width,d->height}};begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(r->command,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(r->command,VK_PIPELINE_BIND_POINT_GRAPHICS,r->pipeline);
        vkCmdBindDescriptorSets(r->command,VK_PIPELINE_BIND_POINT_GRAPHICS,r->layout,0,1,&set,0,nullptr);VkDeviceSize offset=0;vkCmdBindVertexBuffers(r->command,0,1,&r->transient[4].buffer,&offset);
        if(d->index_count) {vkCmdBindIndexBuffer(r->command,r->transient[5].buffer,0,VK_INDEX_TYPE_UINT16);vkCmdDrawIndexed(r->command,d->index_count,1,0,0,0);}
        else vkCmdDraw(r->command,d->vertex_count,1,0,0);vkCmdEndRenderPass(r->command);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={d->width,d->height,1};
        vkCmdCopyImageToBuffer(r->command,r->target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r->readback.buffer,1,&copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=r->readback.buffer;host.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(r->command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        r->output_ready=true;int result=r->submit();if(result!=VK_SUCCESS && result!=VK_TIMEOUT)r->output_ready=false;return result;
    } catch(const Failure &f) {r->error=f.message;r->output_ready=false;return f.result;}
      catch(...) {r->error="draw allocation failure";r->output_ready=false;return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
extern "C" int xbg_vk_readback(xbg_vk_renderer *r,void *rgba,size_t bytes) {
    if(!r)return VK_ERROR_INITIALIZATION_FAILED;
    try {
        require(!r->pending && r->output_ready && rgba && bytes>=r->readback.size,"readback not ready or destination too small");void *p=nullptr;
        vkcheck(vkMapMemory(r->info.device,r->readback.memory,0,VK_WHOLE_SIZE,0,&p),"vkMapMemory(readback)");VkResult result=VK_SUCCESS;
        if(!r->readback.coherent) {VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};mr.memory=r->readback.memory;mr.size=VK_WHOLE_SIZE;result=vkInvalidateMappedMemoryRanges(r->info.device,1,&mr);}
        if(result==VK_SUCCESS)std::memcpy(rgba,p,(size_t)r->readback.size);vkUnmapMemory(r->info.device,r->readback.memory);vkcheck(result,"vkInvalidateMappedMemoryRanges");return VK_SUCCESS;
    }catch(const Failure &f){r->error=f.message;return f.result;}
}
extern "C" VkImage xbg_vk_output_image(xbg_vk_renderer *r,uint32_t *w,uint32_t *h) {
    if(!r || r->pending || !r->output_ready)return VK_NULL_HANDLE;if(w)*w=r->target.width;if(h)*h=r->target.height;return r->target.image;
}
extern "C" int xbg_vk_replay_texture(xbg_vk_renderer *r,xbg_vk_texture *texture) {
    if(!r || !texture || texture->owner!=r)return VK_ERROR_INITIALIZATION_FAILED;
    float w=(float)texture->image.width,h=(float)texture->image.height;
    xbg_vertex2d vertices[]={{{0,0},{0,0},{1,1,1,1}},{{w,0},{1,0},{1,1,1,1}},{{w,h},{1,1},{1,1,1,1}},
        {{0,0},{0,0},{1,1,1,1}},{{w,h},{1,1},{1,1,1,1}},{{0,h},{0,1},{1,1,1,1}}};
    float matrix[16],fragment[8];uint8_t support[1392];xbg_2d_projection(matrix,w,h);xbg_2d_fragment_defaults(fragment);xbg_support_defaults(support,w,h);
    xbg_draw2d draw{};draw.vertices=vertices;draw.vertex_count=6;draw.vertex_uniform0=matrix;draw.vertex_uniform_bytes=sizeof(matrix);
    draw.fragment_uniform0=fragment;draw.fragment_uniform_bytes=sizeof(fragment);draw.vertex_support=draw.fragment_support=support;draw.texture10=texture;
    draw.min_filter=draw.mag_filter=VK_FILTER_NEAREST;draw.mipmap_mode=VK_SAMPLER_MIPMAP_MODE_NEAREST;draw.address_u=draw.address_v=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    draw.blend.colorWriteMask=15;draw.scissor={{0,0},{texture->image.width,texture->image.height}};
    draw.width=texture->image.width;draw.height=texture->image.height;
    draw.target_format=texture->image.format==VK_FORMAT_R8G8B8A8_SRGB?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;
    return xbg_vk_draw_2d(r,&draw);
}
extern "C" void xbg_2d_projection(float m[16],float w,float h) {
    std::memset(m,0,64);m[0]=2/w;m[3]=-1;m[5]=2/h;m[7]=-1;m[10]=1;m[15]=1;
}
extern "C" void xbg_2d_fragment_defaults(float c[8]) {
    std::memset(c,0,32);c[3]=1;c[5]=2.2f;c[6]=1;
}
extern "C" void xbg_support_defaults(void *bytes,float w,float h) {
    uint8_t *b=(uint8_t*)bytes;std::memset(b,0,1392);float inv[]={1/w,1/h,0,0},size[]={w,h,0,0},one=1;
    std::memcpy(b+144,inv,16);std::memcpy(b+160,size,16);for(unsigned i=0;i<73;i++)std::memcpy(b+192+16*i,&one,4);
}
