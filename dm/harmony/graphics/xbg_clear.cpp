#include "xbg_clear.h"
#include <string.h>
extern "C" int xbg_clear_vk_class(VkFormat f){
    switch(f){
    case VK_FORMAT_R8_UINT:case VK_FORMAT_R8G8_UINT:case VK_FORMAT_R8G8B8_UINT:case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16_UINT:case VK_FORMAT_R16G16_UINT:case VK_FORMAT_R16G16B16_UINT:case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT:case VK_FORMAT_R32G32_UINT:case VK_FORMAT_R32G32B32_UINT:case VK_FORMAT_R32G32B32A32_UINT:case VK_FORMAT_A2B10G10R10_UINT_PACK32:return 2;
    case VK_FORMAT_R8_SINT:case VK_FORMAT_R8G8_SINT:case VK_FORMAT_R8G8B8_SINT:case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_SINT:case VK_FORMAT_R16G16_SINT:case VK_FORMAT_R16G16B16_SINT:case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT:case VK_FORMAT_R32G32_SINT:case VK_FORMAT_R32G32B32_SINT:case VK_FORMAT_R32G32B32A32_SINT:return 1;
    case VK_FORMAT_R8_UNORM:case VK_FORMAT_R8_SNORM:case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_R8G8_SNORM:case VK_FORMAT_R8G8B8_UNORM:case VK_FORMAT_R8G8B8_SNORM:case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R16_UNORM:case VK_FORMAT_R16_SNORM:case VK_FORMAT_R16_SFLOAT:case VK_FORMAT_R16G16_UNORM:case VK_FORMAT_R16G16_SNORM:case VK_FORMAT_R16G16_SFLOAT:case VK_FORMAT_R16G16B16_UNORM:case VK_FORMAT_R16G16B16_SNORM:case VK_FORMAT_R16G16B16_SFLOAT:case VK_FORMAT_R16G16B16A16_UNORM:case VK_FORMAT_R16G16B16A16_SNORM:case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:case VK_FORMAT_R32G32_SFLOAT:case VK_FORMAT_R32G32B32_SFLOAT:case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_SRGB:case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:case VK_FORMAT_B10G11R11_UFLOAT_PACK32:case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:return 0;
    default:return -1;
    }
}
extern "C" int xbg_clear_value(VkFormat format,int input_scalar,const void *input,VkClearColorValue *output){
    int target=xbg_clear_vk_class(format);if(!input || !output || target<0 || input_scalar<0 || input_scalar>2)return -1;
    VkClearColorValue source{},value{};memcpy(&source,input,16);
    if(target==input_scalar){*output=source;return target;}
    for(unsigned i=0;i<4;i++){
        double x=input_scalar==0?double(source.float32[i]):input_scalar==1?double(source.int32[i]):double(source.uint32[i]);
        if(target==0)value.float32[i]=float(x);
        else { // Only exact cross-class integers: do not invent NVN rounding/saturation.
            if(x!=x)return -2;
            if(target==1){if(x<-2147483648.0 || x>2147483647.0)return -2;int32_t n=int32_t(x);if(double(n)!=x)return -2;value.int32[i]=n;}
            else {if(x<0 || x>4294967295.0)return -2;uint32_t n=uint32_t(x);if(double(n)!=x)return -2;value.uint32[i]=n;}}

    }
    *output=value;return target;
}
extern "C" int xbg_clear_color_class(uint32_t f){
    if(f>=1 && f<=0x30){uint32_t t=(f-1)%12+1;if(t==3 || t==8 || t==11)return 2;if(t==4 || t==9 || t==12)return 1;return 0;}
    switch(f){case 0x3e:return 2;case 0x37:case 0x38:case 0x3d:case 0x3f:case 0x40:case 0x6d:case 0x9a:return 0;default:return -1;}
}
extern "C" int xbg_masked_clear_state(uint32_t cm,uint32_t sm,uint32_t ref,VkPipelineColorBlendAttachmentState *color,VkPipelineDepthStencilStateCreateInfo *depth){
    if(!color || !depth || cm>15 || sm>255 || (cm && sm))return -1;
    VkPipelineColorBlendAttachmentState c{};c.colorWriteMask=cm;
    VkPipelineDepthStencilStateCreateInfo d{};d.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    d.stencilTestEnable=sm?VK_TRUE:VK_FALSE;
    d.front.failOp=d.front.depthFailOp=d.front.passOp=VK_STENCIL_OP_REPLACE;d.front.compareOp=VK_COMPARE_OP_ALWAYS;
    d.front.compareMask=255;d.front.writeMask=sm;d.front.reference=ref&255;d.back=d.front;
    *color=c;*depth=d;return 0;
}
extern "C" int xbg_clear_rectangle(uint32_t width,uint32_t height,int32_t x,int32_t y,uint32_t w,uint32_t h,VkRect2D *out){
    if(!out || width>INT32_MAX || height>INT32_MAX)return -1;
    int64_t left=x<0?0:x,top=y<0?0:y,right=int64_t(x)+w,bottom=int64_t(y)+h;
    if(left>width)left=width;if(top>height)top=height;if(right>width)right=width;if(bottom>height)bottom=height;
    VkRect2D r{{int32_t(left),int32_t(top)},{uint32_t(right>left?right-left:0),uint32_t(bottom>top?bottom-top:0)}};*out=r;return 0;
}
extern "C" int xbg_texture_clear_slice(uint32_t width,uint32_t height,uint32_t layers,uint32_t base,int32_t x,int32_t y,int32_t z,int32_t w,int32_t h,int32_t depth,uint32_t slice,VkRect2D *rect,uint32_t *layer){
    if(!rect || !layer || x<0 || y<0 || z<0 || w<=0 || h<=0 || depth<=0 || slice>=uint32_t(depth))return -1;
    if(uint64_t(x)+w>width || uint64_t(y)+h>height || uint64_t(base)+z+depth>layers)return -2;
    *rect={{x,y},{uint32_t(w),uint32_t(h)}};*layer=base+uint32_t(z)+slice;return 0;
}
extern "C" int xbg_clear_requires_preserve(uint32_t levels,uint32_t layers,uint32_t w,uint32_t h,const VkRect2D *r,uint32_t partial,uint32_t aspects,uint32_t written){
    return !r || partial || levels>1 || layers>1 || r->offset.x || r->offset.y || r->extent.width!=w || r->extent.height!=h || aspects!=written;
}
