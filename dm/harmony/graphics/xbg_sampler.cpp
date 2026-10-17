#include "xbg_sampler.h"
#include <string.h>

static bool finite(float x){uint32_t bits;memcpy(&bits,&x,4);return (bits&0x7f800000u)!=0x7f800000u;}
static int wrap(uint32_t n,VkBool32 mirror,VkSamplerAddressMode &out){
    switch(n){
    case 1:out=VK_SAMPLER_ADDRESS_MODE_REPEAT;return 0;
    case 3:if(!mirror)return -3;out=VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;return 0;
    case 5:out=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;return 0;
    case 6:out=VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;return 0;
    case 7:out=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;return 0;
    default:return -2; // GL_CLAMP and MIRROR_CLAMP[_TO_BORDER] need shader support.
    }
}
extern "C" int xbg_sampler_from_nvn(const xbg_nvn_sampler_desc *p,const xbg_sampler_limits *limits,VkSamplerCreateInfo *out){
    if(!p || !limits || !out)return -1;
    if(p->min_filter>5 || p->mag_filter>1)return -1;
    VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s.minFilter=(p->min_filter&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;s.magFilter=p->mag_filter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    s.mipmapMode=p->min_filter>=4?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
    int r;if((r=wrap(p->wrap_s,limits->mirror_clamp_to_edge_enabled,s.addressModeU)) ||
             (r=wrap(p->wrap_t,limits->mirror_clamp_to_edge_enabled,s.addressModeV)) ||
             (r=wrap(p->wrap_r,limits->mirror_clamp_to_edge_enabled,s.addressModeW)))return r;
    if(!finite(p->min_lod) || !finite(p->max_lod) || !finite(p->lod_bias) || p->min_lod>p->max_lod ||
       !finite(limits->max_lod_bias) || limits->max_lod_bias<0 || p->lod_bias>limits->max_lod_bias || p->lod_bias < -limits->max_lod_bias)return -4;
    // Vulkan's filter mapping requires maxLod .25 for non-mip minification.
    // Zero would always select magFilter, silently changing min!=mag samplers.
    s.minLod=p->min_filter<2?0:(p->min_lod<0?0:p->min_lod);
    s.maxLod=p->min_filter<2?0.25f:(p->max_lod<0?0:p->max_lod);s.mipLodBias=p->lod_bias;
    if(!finite(p->anisotropy) || p->anisotropy<1 || !finite(limits->max_anisotropy) ||
       p->anisotropy>limits->max_anisotropy || (p->anisotropy>1 && !limits->anisotropy_enabled))return -5;
    s.anisotropyEnable=p->anisotropy>1;s.maxAnisotropy=p->anisotropy;
    if(p->compare_mode>1 || p->compare_func<1 || p->compare_func>8)return -6;
    s.compareEnable=p->compare_mode;s.compareOp=VkCompareOp(p->compare_func-1);
    s.borderColor=limits->integer_border?VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if(p->wrap_s==5 || p->wrap_t==5 || p->wrap_r==5){
        const float *b=p->border;
        if(b[0]==0 && b[1]==0 && b[2]==0 && b[3]==0){}
        else if(b[0]==0 && b[1]==0 && b[2]==0 && b[3]==1)s.borderColor=limits->integer_border?VK_BORDER_COLOR_INT_OPAQUE_BLACK:VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if(b[0]==1 && b[1]==1 && b[2]==1 && b[3]==1)s.borderColor=limits->integer_border?VK_BORDER_COLOR_INT_OPAQUE_WHITE:VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else return -7; // VK_EXT_custom_border_color requires separate enabled contract.
    }
    *out=s;return 0;
}
extern "C" const char *xbg_sampler_error(int result){
    switch(result){case 0:return "OK";case -1:return "invalid min/mag filter";case -2:return "legacy clamp/mirror border needs exact shader handling";
    case -3:return "mirror-clamp-to-edge feature/extension not enabled";case -4:return "invalid LOD range/bias";
    case -5:return "anisotropy unsupported/disabled/out of range";case -6:return "invalid compare mode/function";
    case -7:return "noncanonical border needs enabled custom-border-color";default:return "invalid sampler conversion";}
}
