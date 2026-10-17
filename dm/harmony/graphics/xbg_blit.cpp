#include "xbg_blit.h"
#include <stdint.h>
#include <string.h>
static bool valid(const XbgBlitRegion &r){
    if(!r.width || !r.height || r.level>=r.levels || r.level>=32 || r.x<0 || r.y<0 || r.z<0 || r.w<=0 || r.h<=0 || r.count<=0)return false;
    uint32_t w=r.width>>r.level,h=r.height>>r.level;if(!w)w=1;if(!h)h=1;
    return uint64_t(r.x)+r.w<=w && uint64_t(r.y)+r.h<=h &&
        uint64_t(r.x)+r.w<=INT32_MAX && uint64_t(r.y)+r.h<=INT32_MAX &&
        uint64_t(r.base_layer)+r.z+r.count<=r.layers;
}
int xbg_blit_plan(const XbgBlitRegion &s,const XbgBlitRegion &d,uint32_t flags,int scalar,
    VkFormatFeatureFlags sf,VkFormatFeatureFlags df,VkImageBlit *out,VkFilter *filter){
    if(!out || !filter || (flags&~3u) || scalar<0 || scalar>2 || !valid(s) || !valid(d) || s.count!=d.count)return -1;
    if(!(sf&VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(df&VK_FORMAT_FEATURE_BLIT_DST_BIT))return -2;
    bool linear=(flags&1)!=0;
    if(linear && (scalar!=0 || !(sf&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)))return -3;
    VkImageBlit r{};
    r.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,s.level,s.base_layer+uint32_t(s.z),uint32_t(s.count)};
    r.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,d.level,d.base_layer+uint32_t(d.z),uint32_t(d.count)};
    r.srcOffsets[0]={s.x,s.y,0};r.srcOffsets[1]={s.x+s.w,s.y+s.h,1};
    r.dstOffsets[0]={d.x,d.y,0};r.dstOffsets[1]={d.x+d.w,d.y+d.h,1};
    *out=r;*filter=linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;return 0;
}
int xbg_copy_rows(uint32_t w,uint32_t h,uint32_t slices,uint32_t bpp,uint64_t stride,XbgCopyRows *out){
    if(!out || !w || !h || !slices || !bpp || bpp>16)return -1;
    uint64_t tight=uint64_t(w)*bpp,row=stride?stride:tight,rows=uint64_t(h)*slices;
    if(row<tight || row%bpp || row/bpp>UINT32_MAX || row>256ull*1024*1024 || rows>256ull*1024*1024/row)return -2;
    *out={row,tight,rows,(rows-1)*row+tight,uint32_t(row/bpp)};return 0;
}
int xbg_copy_rows_write(void *dst,uint64_t dst_bytes,const void *src,uint64_t src_bytes,const XbgCopyRows &p){
    if(!dst || !src || !p.rows || !p.tight || p.row<p.tight || p.row>256ull*1024*1024 || p.rows>256ull*1024*1024/p.row || p.bytes!=(p.rows-1)*p.row+p.tight || dst_bytes<p.bytes || src_bytes<p.bytes)return -1;
    for(uint64_t r=0;r<p.rows;r++)memcpy(static_cast<uint8_t*>(dst)+r*p.row,static_cast<const uint8_t*>(src)+r*p.row,size_t(p.tight));
    return 0;
}

int xbg_copy_is_full(const XbgBlitRegion &r){
    return r.levels==1 && r.level==0 && r.base_layer==0 && r.x==0 && r.y==0 && r.z==0 && r.w>0 && r.h>0 && r.count>0 && uint32_t(r.w)==r.width && uint32_t(r.h)==r.height && uint32_t(r.count)==r.layers;
}
