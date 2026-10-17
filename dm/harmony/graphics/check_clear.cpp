#include "xbg_clear.h"
#include <string.h>
extern "C" int xbg_clear_selftest(){
    const uint32_t old[4]={11,22,33,44},value[4]={101,202,303,404};
    for(uint32_t mask=0;mask<16;mask++){
        VkPipelineColorBlendAttachmentState c{};VkPipelineDepthStencilStateCreateInfo d{};
        if(xbg_masked_clear_state(mask,0,0,&c,&d) || c.blendEnable || c.colorWriteMask!=mask || d.depthTestEnable || d.depthWriteEnable || d.stencilTestEnable)return 1;
        uint32_t pixel[4];memcpy(pixel,old,sizeof(pixel));for(unsigned component=0;component<4;component++)if(c.colorWriteMask&(1u<<component))pixel[component]=value[component];
        for(unsigned component=0;component<4;component++)if(pixel[component]!=((mask>>component)&1?value[component]:old[component]))return 2;
    }
    for(uint32_t mask=0;mask<256;mask++){
        VkPipelineColorBlendAttachmentState c{};VkPipelineDepthStencilStateCreateInfo d{};
        if(xbg_masked_clear_state(0,mask,0x1a5,&c,&d) || c.colorWriteMask || d.depthTestEnable || d.depthWriteEnable || bool(d.stencilTestEnable)!=bool(mask))return 3;
        VkStencilOpState faces[2]={d.front,d.back};for(auto s:faces){
            if(s.compareOp!=VK_COMPARE_OP_ALWAYS || s.passOp!=VK_STENCIL_OP_REPLACE || s.failOp!=VK_STENCIL_OP_REPLACE || s.depthFailOp!=VK_STENCIL_OP_REPLACE || s.reference!=0xa5 || s.writeMask!=mask)return 4;
            uint32_t actual=(0x5a&~s.writeMask)|(s.reference&s.writeMask),expected=0x5a^mask;
            if(actual!=expected)return 5;
        }
    }
    VkPipelineColorBlendAttachmentState c{};c.colorWriteMask=123;VkPipelineDepthStencilStateCreateInfo d{};
    if(xbg_masked_clear_state(16,0,0,&c,&d)!=-1 || c.colorWriteMask!=123 || xbg_masked_clear_state(1,1,0,&c,&d)!=-1 || xbg_masked_clear_state(0,256,0,&c,&d)!=-1)return 6;
    VkRect2D rect{};
    if(xbg_clear_rectangle(80,60,-5,-9,10,12,&rect) || rect.offset.x || rect.offset.y || rect.extent.width!=5 || rect.extent.height!=3)return 7;
    if(xbg_clear_rectangle(80,60,75,59,UINT32_MAX,UINT32_MAX,&rect) || rect.offset.x!=75 || rect.offset.y!=59 || rect.extent.width!=5 || rect.extent.height!=1)return 8;
    if(xbg_clear_rectangle(80,60,90,80,10,10,&rect) || rect.offset.x!=80 || rect.offset.y!=60 || rect.extent.width || rect.extent.height)return 9;
    if(xbg_clear_rectangle(80,60,INT32_MIN,INT32_MIN,1,1,&rect) || rect.extent.width || rect.extent.height)return 10;
    if(xbg_clear_rectangle(UINT32_MAX,60,0,0,1,1,&rect)!=-1)return 11;
    const uint32_t unsigned_formats[]={3,8,11,15,20,23,27,32,35,39,44,47,0x3e};
    const uint32_t signed_formats[]={4,9,12,16,21,24,28,33,36,40,45,48};
    for(auto f:unsigned_formats)if(xbg_clear_color_class(f)!=2)return 12;
    for(auto f:signed_formats)if(xbg_clear_color_class(f)!=1)return 13;
    const uint32_t floats[]={1,2,5,10,0x37,0x3d,0x9a};for(auto f:floats)if(xbg_clear_color_class(f)!=0)return 14;
    const uint32_t invalid[]={0,0x31,0x36,0xffff};for(auto f:invalid)if(xbg_clear_color_class(f)!=-1)return 15;
    uint32_t layer=99;
    for(uint32_t i=0;i<3;i++){
        if(xbg_texture_clear_slice(64,32,10,3,5,6,2,7,8,3,i,&rect,&layer) || layer!=5+i || rect.offset.x!=5 || rect.offset.y!=6 || rect.extent.width!=7 || rect.extent.height!=8)return 16;
    }
    layer=99;rect.offset.x=88;
    if(xbg_texture_clear_slice(64,32,10,UINT32_MAX,0,0,1,1,1,1,0,&rect,&layer)!=-2 || layer!=99 || rect.offset.x!=88)return 17;
    if(xbg_texture_clear_slice(64,32,10,0,63,0,0,2,1,1,0,&rect,&layer)!=-2 || xbg_texture_clear_slice(64,32,10,0,-1,0,0,1,1,1,0,&rect,&layer)!=-1 || xbg_texture_clear_slice(64,32,10,0,0,0,0,1,1,1,1,&rect,&layer)!=-1)return 18;
    rect={{0,0},{64,32}};
    if(xbg_clear_requires_preserve(1,1,64,32,&rect,0,1,1))return 19;
    if(!xbg_clear_requires_preserve(1,1,64,32,&rect,1,1,1) || !xbg_clear_requires_preserve(2,1,64,32,&rect,0,1,1) || !xbg_clear_requires_preserve(1,2,64,32,&rect,0,1,1) || !xbg_clear_requires_preserve(1,1,64,32,&rect,0,6,2))return 20;
    rect.extent.width=63;if(!xbg_clear_requires_preserve(1,1,64,32,&rect,0,1,1))return 21;
    return 0;
}
