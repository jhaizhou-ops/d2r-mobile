#include "xbg_quad.h"
int xbg_quad_index_count(int32_t vertices,uint32_t *indices){
    if(vertices<0 || !indices)return -1;
    uint64_t n=(uint64_t)(vertices/4)*6;
    if(n>16u*1024*1024)return -2;
    *indices=(uint32_t)n;return 0;
}
int xbg_quad_plan(const xbg_draw_call *s,xbg_draw_call *out,uint32_t *indices,size_t capacity){
    if(!s || !out || s->primitive!=7 || s->index_type!=-1 || s->first<0 || s->count<0 || s->instances<0 || s->base_instance<0)return -1;
    uint32_t n=0;int r=xbg_quad_index_count(s->count,&n);if(r)return r;
    if(!s->instances)n=0;
    if(n>capacity || (n && !indices))return -3;
    xbg_draw_call p={4,2,0,(int32_t)n,s->instances,s->base_instance,s->first,(uint64_t)(uintptr_t)indices};
    for(uint32_t q=0;q<n/6;q++){
        uint32_t b=q*4,*v=indices+q*6;
        v[0]=b;v[1]=b+1;v[2]=b+2;v[3]=b;v[4]=b+2;v[5]=b+3;
    }
    *out=p;return 0;
}
