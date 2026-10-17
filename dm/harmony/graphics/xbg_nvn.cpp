#include "xbg_vulkan.h"
#include <string.h>

static bool finite_float(float v){uint32_t bits;memcpy(&bits,&v,4);return (bits&0x7f800000u)!=0x7f800000u;}
extern "C" int xbg_nvn_viewport(VkViewport *out,float x,float y,float width,float height,float minimum,float maximum){
    if(!out || !finite_float(x) || !finite_float(y) || !finite_float(width) || !finite_float(height) ||
       !finite_float(minimum) || !finite_float(maximum) || width<=0 || height<=0 ||
       minimum<0 || minimum>1 || maximum<0 || maximum>1 || !finite_float(x+width) || !finite_float(y+height))return -1;
    // The original shader chain explicitly sets YNegateEnabled=false and feeds
    // the same SPIR-V to Metal without --flip-vert-y. Match Metal's viewport:
    // screenY=y+(1-ndcY)*height/2. Flipping only frontFace leaves the image upside
    // down; disabling culling would also lose front/back stencil semantics.
    VkViewport v{x,y+height,width,-height,minimum,maximum};*out=v;return 0;
}

static int address(uint32_t n,VkSamplerAddressMode &v) {
    switch(n) {case 1:v=VK_SAMPLER_ADDRESS_MODE_REPEAT;return 0;case 6:v=VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;return 0;
        case 5:v=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;return 0;
        case 7:v=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;return 0;default:return -1;}
}
extern "C" int xbg_2d_sampler_from_nvn(xbg_draw2d *d,uint32_t min,uint32_t mag,uint32_t s,uint32_t t,float lo,float hi,float bias) {
    if(!d || min>5 || mag>1)return -1;VkSamplerAddressMode u,v;
    if(address(s,u) || address(t,v))return -2;
    d->min_filter=(min&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST;d->mag_filter=mag?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    d->mipmap_mode=min>=4?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
    d->address_u=u;d->address_v=v;d->min_lod=min<2?0:lo;d->max_lod=min<2?0.25f:hi;d->lod_bias=bias;return 0;
}
static int factor(uint32_t n,VkBlendFactor &f) {
    switch(n) {
    case 1:f=VK_BLEND_FACTOR_ZERO;break;case 2:f=VK_BLEND_FACTOR_ONE;break;
    case 3:f=VK_BLEND_FACTOR_SRC_COLOR;break;case 4:f=VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;break;
    case 5:f=VK_BLEND_FACTOR_SRC_ALPHA;break;case 6:f=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;break;
    case 7:f=VK_BLEND_FACTOR_DST_ALPHA;break;case 8:f=VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;break;
    case 9:f=VK_BLEND_FACTOR_DST_COLOR;break;case 10:f=VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;break;
    case 11:f=VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;break;
    case 0x61:f=VK_BLEND_FACTOR_CONSTANT_COLOR;break;case 0x62:f=VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;break;
    case 0x63:f=VK_BLEND_FACTOR_CONSTANT_ALPHA;break;case 0x64:f=VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;break;
    default:return -1;
    }return 0;
}
extern "C" int xbg_2d_blend_from_nvn(xbg_draw2d *d,uint32_t enabled,uint32_t sr,uint32_t dr,uint32_t sa,uint32_t da,uint32_t er,uint32_t ea,uint32_t mask) {
    if(!d || er<1 || er>5 || ea<1 || ea>5 || (mask&~15u))return -1;
    VkPipelineColorBlendAttachmentState b{};
    if(factor(sr,b.srcColorBlendFactor) || factor(dr,b.dstColorBlendFactor) || factor(sa,b.srcAlphaBlendFactor) || factor(da,b.dstAlphaBlendFactor))return -2;
    static const VkBlendOp eq[]={VK_BLEND_OP_ADD,VK_BLEND_OP_SUBTRACT,VK_BLEND_OP_REVERSE_SUBTRACT,VK_BLEND_OP_MIN,VK_BLEND_OP_MAX};
    b.colorBlendOp=eq[er-1];b.alphaBlendOp=eq[ea-1];b.blendEnable=enabled?VK_TRUE:VK_FALSE;b.colorWriteMask=mask;d->blend=b;return 0;
}
static int format(uint32_t f,uint32_t &components,uint32_t &bytes,bool &unorm) {
    unorm=false;
    switch(f) {case 0x0a:components=1;break;case 0x16:components=2;break;case 0x22:components=3;break;case 0x2e:components=4;break;
        case 0x25:components=4;bytes=4;unorm=true;return 0;default:return -1;}
    bytes=components*4;return 0;
}
extern "C" int xbg_2d_vertices_from_nvn(const xbg_nvn_attribute a[3],uint32_t first,uint32_t count,xbg_vertex2d *out,size_t bytes) {
    if(!a || !out || !count || count>1048576 || bytes<(size_t)count*sizeof(*out))return -1;
    uint32_t comp[3],size[3];bool unorm[3];
    for(unsigned k=0;k<3;k++) {
        if(!a[k].buffer || format(a[k].nvn_format,comp[k],size[k],unorm[k]))return -2;
        uint64_t last=(uint64_t)first+count-1;
        if(last>UINT32_MAX)return -3;
        uint64_t end=last*a[k].stride+a[k].offset+size[k];if(end>a[k].buffer_bytes)return -3;
    }
    for(uint32_t i=0;i<count;i++) {
        float v[3][4]={{0,0,0,1},{0,0,0,1},{0,0,0,1}};
        for(unsigned k=0;k<3;k++) {
            const uint8_t *p=(const uint8_t*)a[k].buffer+((size_t)first+i)*a[k].stride+a[k].offset;
            for(uint32_t c=0;c<comp[k];c++) {if(unorm[k])v[k][c]=p[c]/255.0f;else memcpy(&v[k][c],p+c*4,4);}
        }
        memcpy(out[i].position,v[0],8);memcpy(out[i].uv,v[1],8);memcpy(out[i].color,v[2],16);
    }return 0;
}
