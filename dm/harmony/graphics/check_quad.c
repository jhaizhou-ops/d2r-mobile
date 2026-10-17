#include "xbg_quad.h"
#include <string.h>
int xbg_quad_selftest(void){
    const uint32_t expected[12]={0,1,2,0,2,3,4,5,6,4,6,7};
    uint32_t indices[14];for(int i=0;i<14;i++)indices[i]=0xdeadbeef;
    xbg_draw_call source={7,-1,17,11,5,9,0,0},out={0};
    if(xbg_quad_plan(&source,&out,indices,12))return 1;
    if(memcmp(indices,expected,sizeof(expected)) || indices[12]!=0xdeadbeef || indices[13]!=0xdeadbeef)return 2;
    if(out.primitive!=4 || out.index_type!=2 || out.first!=0 || out.count!=12 || out.base_vertex!=17 || out.instances!=5 || out.base_instance!=9 || out.index_address!=(uint64_t)(uintptr_t)indices)return 3;
    // Independently expected guest vertex IDs for first=17, not byte offsets.
    const uint32_t vertex_ids[12]={17,18,19,17,19,20,21,22,23,21,23,24};
    for(int i=0;i<12;i++)if(indices[i]+out.base_vertex!=vertex_ids[i])return 4;
    for(int vertices=0;vertices<4;vertices++){
        source.count=vertices;if(xbg_quad_plan(&source,&out,0,0) || out.count)return 5;
    }
    source.count=4;source.instances=0;
    if(xbg_quad_plan(&source,&out,0,0) || out.count)return 6;
    source.instances=1;source.first=0;source.base_instance=0;
    if(xbg_quad_plan(&source,&out,indices,6) || out.count!=6 || out.base_vertex || out.base_instance)return 7;
    out.count=123;indices[0]=456;
    if(xbg_quad_plan(&source,&out,indices,5)!=-3 || out.count!=123 || indices[0]!=456)return 8;
    source.index_type=1;if(xbg_quad_plan(&source,&out,indices,14)!=-1)return 9;
    source.index_type=-1;source.first=-1;if(xbg_quad_plan(&source,&out,indices,14)!=-1)return 10;
    source.first=0;source.count=-1;if(xbg_quad_plan(&source,&out,indices,14)!=-1)return 11;
    uint32_t n=999;
    if(xbg_quad_index_count(INT32_MAX,&n)!=-2 || n!=999)return 12;
    if(xbg_quad_index_count(11184811,&n) || n!=16777212)return 13;
    if(xbg_quad_index_count(11184812,&n)!=-2)return 14;
    if(xbg_quad_index_count(-1,&n)!=-1)return 15;
    return 0;
}
