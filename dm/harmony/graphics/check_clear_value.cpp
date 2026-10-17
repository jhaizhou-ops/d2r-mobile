#include "xbg_clear.h"
#include <string.h>
extern "C" int xbg_clear_value_selftest(){
    const VkFormat decoded[]={VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_R16_UNORM,VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16B16A16_SFLOAT};
    VkClearColorValue out{};float f[]={-0.5f,0.25f,1.0f,2.0f};
    for(auto fmt:decoded)if(xbg_clear_value(fmt,0,f,&out)!=0 || memcmp(f,&out,16))return 1;
    const uint32_t u[]={0,1,0x80000000u,0xffffffffu};
    for(auto fmt:decoded)if(xbg_clear_value(fmt,2,u,&out)!=0 || out.float32[0]!=0 || out.float32[1]!=1 || out.float32[2]!=2147483648.0f || out.float32[3]!=4294967296.0f)return 2;
    const int32_t i[]={-2147483647-1,-1,0,2147483647};
    if(xbg_clear_value(VK_FORMAT_R32G32B32A32_SINT,1,i,&out)!=1 || memcmp(i,&out,16))return 3;
    if(xbg_clear_value(VK_FORMAT_R32G32B32A32_UINT,2,u,&out)!=2 || memcmp(u,&out,16))return 4;
    if(xbg_clear_value(VK_FORMAT_R8G8B8A8_SNORM,1,i,&out)!=0 || out.float32[1]!=-1 || out.float32[0]!=-2147483648.0f)return 5;
    float ints[]={0,3,16,16777216};
    if(xbg_clear_value(VK_FORMAT_R32_SINT,0,ints,&out)!=1 || out.int32[1]!=3 || out.int32[3]!=16777216)return 6;
    if(xbg_clear_value(VK_FORMAT_R32_UINT,0,ints,&out)!=2 || out.uint32[1]!=3 || out.uint32[3]!=16777216)return 7;
    if(xbg_clear_value(VK_FORMAT_R32_SINT,2,u,&out)!=-2)return 8;
    if(xbg_clear_value(VK_FORMAT_R32_UINT,1,i,&out)!=-2)return 9;
    out.uint32[0]=0xdeadbeef;auto before=out;
    if(xbg_clear_value(VK_FORMAT_D32_SFLOAT,0,f,&out)!=-1 || memcmp(&out,&before,16))return 10;
    uint32_t nan[]={0x7fc00000,0,0,0};
    if(xbg_clear_value(VK_FORMAT_R32_UINT,0,nan,&out)!=-2 || memcmp(&out,&before,16))return 11;
    if(xbg_clear_value(VK_FORMAT_R32_SFLOAT,0,nan,&out)!=0 || memcmp(nan,&out,16))return 12;
    // Creation UNORM with UINT reinterpretation must choose the VIEW class.
    if(xbg_clear_value(VK_FORMAT_R8G8B8A8_UINT,2,u,&out)!=2 || memcmp(u,&out,16))return 13;
    float fractional[]={0.5f,0,0,0};before=out;
    if(xbg_clear_value(VK_FORMAT_R32_UINT,0,fractional,&out)!=-2 || memcmp(&out,&before,16))return 14;
    float huge[]={1e30f,0,0,0};if(xbg_clear_value(VK_FORMAT_R32_SINT,0,huge,&out)!=-2 || memcmp(&out,&before,16))return 15;
    return 0;
}
