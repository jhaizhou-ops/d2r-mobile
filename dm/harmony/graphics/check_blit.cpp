#include "xbg_blit.h"
#include <string.h>
extern "C" int xbg_blit_selftest(){
    const VkFormatFeatureFlags sf=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,df=VK_FORMAT_FEATURE_BLIT_DST_BIT;
    XbgBlitRegion s{128,64,12,4,1,2,3,4,1,16,12,3},d{128,128,10,4,2,4,2,3,2,24,20,3};VkImageBlit r{};VkFilter f{};
    if(xbg_blit_plan(s,d,0,0,sf,df,&r,&f) || f!=VK_FILTER_NEAREST)return 1;
    if(r.srcSubresource.mipLevel!=1 || r.srcSubresource.baseArrayLayer!=3 || r.srcSubresource.layerCount!=3 || r.dstSubresource.mipLevel!=2 || r.dstSubresource.baseArrayLayer!=6 || r.dstSubresource.layerCount!=3)return 2;
    if(r.srcOffsets[0].x!=3 || r.srcOffsets[0].y!=4 || r.srcOffsets[1].x!=19 || r.srcOffsets[1].y!=16 || r.dstOffsets[1].x!=26 || r.dstOffsets[1].y!=23 || r.srcOffsets[1].z!=1 || r.dstOffsets[1].z!=1)return 3;
    for(uint32_t flags=1;flags<=3;flags++)if(xbg_blit_plan(s,d,flags,0,sf,df,&r,&f) || f!=((flags&1)?VK_FILTER_LINEAR:VK_FILTER_NEAREST))return 4;
    for(int scalar=1;scalar<=2;scalar++)if(xbg_blit_plan(s,d,0,scalar,sf,df,&r,&f))return 5;
    VkImageBlit prior=r;VkFilter prior_f=f;
    if(xbg_blit_plan(s,d,1,2,sf,df,&r,&f)!=-3)return 6;
    if(xbg_blit_plan(s,d,1,0,VK_FORMAT_FEATURE_BLIT_SRC_BIT,df,&r,&f)!=-3)return 7;
    if(xbg_blit_plan(s,d,0,0,0,df,&r,&f)!=-2 || xbg_blit_plan(s,d,0,0,sf,0,&r,&f)!=-2)return 8;
    const uint32_t rejected[]={4u,8u,16u,0x80000000u};for(uint32_t flags:rejected)if(xbg_blit_plan(s,d,flags,0,sf,df,&r,&f)!=-1)return 9;
    auto bad=s;bad.count=2;if(xbg_blit_plan(bad,d,0,0,sf,df,&r,&f)!=-1)return 10;
    bad=s;bad.base_layer=10;if(xbg_blit_plan(bad,d,0,0,sf,df,&r,&f)!=-1)return 11;
    bad=s;bad.x=60;if(xbg_blit_plan(bad,d,0,0,sf,df,&r,&f)!=-1)return 12;
    bad=s;bad.level=32;bad.levels=33;if(xbg_blit_plan(bad,d,0,0,sf,df,&r,&f)!=-1)return 13;
    bad=s;bad.width=0xffffffff;bad.level=0;bad.x=0x7ffffffe;bad.w=4;if(xbg_blit_plan(bad,d,0,0,sf,df,&r,&f)!=-1)return 14;
    bad=s;bad.base_layer=0xffffffff;if(xbg_blit_plan(bad,d,0,0,sf,df,&r,&f)!=-1)return 15;
    if(memcmp(&r,&prior,sizeof(r)) || f!=prior_f)return 16;
    bad=s;bad.x=-1;if(xbg_blit_plan(bad,d,0,0,sf,df,&r,&f)!=-1)return 17;
    bad=s;bad.w=0;if(xbg_blit_plan(bad,d,0,0,sf,df,&r,&f)!=-1)return 18;
    return 0;
}
