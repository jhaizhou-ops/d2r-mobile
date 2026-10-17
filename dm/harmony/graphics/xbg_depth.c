#include "xbg_depth.h"
#include <string.h>

int xbg_depth_format_info(uint32_t f,xbg_depth_format *o) {
    if(!o || f<0x31 || f>0x36)return -1;
    o->packed_bytes=f==0x31?1:f==0x32?2:f==0x36?8:4;
    o->depth_bytes=f==0x31?0:f==0x32?2:4;
    o->stencil_bytes=(f==0x31 || f==0x35 || f==0x36)?1:0;
    return 0;
}
static int transfer(uint32_t f,uint32_t w,uint32_t h,uint32_t z,void *packed,size_t pn,
    size_t row,size_t image,void *depth,size_t dn,void *stencil,size_t sn,int merge) {
    xbg_depth_format info;if(xbg_depth_format_info(f,&info))return -2;
    if(!packed || !w || !h || !z || (info.depth_bytes && !depth) || (info.stencil_bytes && !stencil))return -1;
    if((size_t)w>SIZE_MAX/h || (size_t)w*h>SIZE_MAX/z)return -1;
    size_t count=(size_t)w*h*z;
    if(count>SIZE_MAX/4 || count*info.depth_bytes>dn || count*info.stencil_bytes>sn)return -1;
    if((size_t)w>SIZE_MAX/info.packed_bytes)return -1;
    size_t tight=(size_t)w*info.packed_bytes;if(!row)row=tight;
    if(row<tight || row%info.packed_bytes || row>SIZE_MAX/h)return -1;
    if(!image)image=row*h;
    if(image<row*h || image>SIZE_MAX/z || image*(z-1)+row*(h-1)+tight>pn)return -1;
    uint8_t *p=packed,*d=depth,*s=stencil;
    for(uint32_t iz=0;iz<z;iz++)for(uint32_t iy=0;iy<h;iy++)for(uint32_t ix=0;ix<w;ix++) {
        size_t index=((size_t)iz*h+iy)*w+ix;uint8_t *pixel=p+iz*image+iy*row+ix*info.packed_bytes;
        if(info.depth_bytes) {
            uint8_t *dp=d+index*info.depth_bytes;
            if(f==0x33 || f==0x35) {
                if(merge)memcpy(pixel+1,dp,3);
                else {memcpy(dp,pixel+1,3);dp[3]=0;}
            } else if(merge)memcpy(pixel,dp,info.depth_bytes);
            else memcpy(dp,pixel,info.depth_bytes);
        }
        if(info.stencil_bytes) {
            uint8_t *sp=pixel+(f==0x36?4:0);
            if(merge)*sp=s[index];else s[index]=*sp;
        }
    }
    return 0;
}
int xbg_depth_extract(uint32_t f,uint32_t w,uint32_t h,uint32_t z,const void *p,size_t pn,
    size_t row,size_t image,void *d,size_t dn,void *s,size_t sn) {
    return transfer(f,w,h,z,(void*)p,pn,row,image,d,dn,s,sn,0);
}
int xbg_depth_merge(uint32_t f,uint32_t w,uint32_t h,uint32_t z,void *p,size_t pn,
    size_t row,size_t image,const void *d,size_t dn,const void *s,size_t sn) {
    return transfer(f,w,h,z,p,pn,row,image,(void*)d,dn,(void*)s,sn,1);
}
