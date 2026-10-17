#include "xbg_blit.h"
#include <string.h>
extern "C" int xbg_copy_rows_selftest(){
    XbgCopyRows p{};
    if(xbg_copy_rows(3,2,2,4,16,&p) || p.row!=16 || p.tight!=12 || p.rows!=4 || p.bytes!=60 || p.texel_row!=4)return 1;
    unsigned char src[72],dst[72];for(int i=0;i<72;i++){src[i]=(unsigned char)i;dst[i]=0xcd;}
    if(xbg_copy_rows_write(dst,60,src,60,p))return 2;
    for(int i=0;i<72;i++)if(dst[i]!=((i<60 && i%16<12)?src[i]:0xcd))return 3;
    memset(dst,0xcd,sizeof(dst));if(xbg_copy_rows_write(dst,59,src,60,p)!=-1)return 4;
    for(auto v:dst)if(v!=0xcd)return 5;
    if(xbg_copy_rows_write(dst,60,src,59,p)!=-1)return 6;
    auto bad=p;bad.bytes++;if(xbg_copy_rows_write(dst,72,src,72,bad)!=-1)return 7;
    if(xbg_copy_rows(3,1,1,4,0,&p) || p.bytes!=12 || p.rows!=1 || p.texel_row!=3)return 8;
    auto before=p;
    if(xbg_copy_rows(3,2,1,4,8,&p)!=-2 || xbg_copy_rows(3,2,1,4,13,&p)!=-2)return 9;
    if(xbg_copy_rows(0xffffffff,0xffffffff,0xffffffff,16,0,&p)!=-2)return 10;
    if(xbg_copy_rows(1,0,1,4,0,&p)!=-1 || xbg_copy_rows(1,1,1,0,0,&p)!=-1)return 11;
    if(memcmp(&p,&before,sizeof(p)))return 12;
    XbgBlitRegion full{32,16,3,1,0,0,0,0,0,32,16,3};if(!xbg_copy_is_full(full))return 13;
    auto part=full;part.levels=2;if(xbg_copy_is_full(part))return 14;
    part=full;part.base_layer=1;if(xbg_copy_is_full(part))return 15;
    part=full;part.count=2;if(xbg_copy_is_full(part))return 16;
    part=full;part.w=16;if(xbg_copy_is_full(part))return 17;
    part=full;part.z=1;if(xbg_copy_is_full(part))return 18;
    return 0;
}
