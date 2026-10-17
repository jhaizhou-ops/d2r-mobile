#include "xbg_draw_plan.h"
#include <string.h>

int xbg_vertex_buffer_range(int32_t first,int32_t count){
    return first>=0 && first<=16 && count>=0 && count<=16-first ? 0:-1;
}
int xbg_draw_arrays_plan(xbg_draw_call *out,int32_t primitive,int32_t first,
    int32_t count,int32_t base_instance,int32_t instances){
    if(!out || first<0 || count<0 || base_instance<0 || instances<0)return -1;
    xbg_draw_call p={primitive,-1,first,count,instances,base_instance,0,0};*out=p;return 0;
}
int xbg_draw_indirect_plan(xbg_draw_call *out,int32_t primitive,int32_t index_type,
    uint64_t index_base,const void *arguments,size_t argument_bytes){
    if(!out || !arguments || index_type< -1 || index_type>2)return -1;
    size_t bytes=index_type<0?16:20;if(argument_bytes<bytes)return -2;
    uint32_t v[5]={0};memcpy(v,arguments,bytes);
    if(v[0]>INT32_MAX || v[1]>INT32_MAX || v[index_type<0?3:4]>INT32_MAX || (index_type<0 && v[2]>INT32_MAX))return -3;
    xbg_draw_call p={primitive,index_type,index_type<0?(int32_t)v[2]:0,(int32_t)v[0],(int32_t)v[1],(int32_t)v[index_type<0?3:4],0,0};
    if(index_type>=0){
        memcpy(&p.base_vertex,&v[3],4);
        uint64_t offset=(uint64_t)v[2]<<index_type,bytes_to_read=(uint64_t)v[0]<<index_type;
        if(offset>UINT64_MAX-index_base || bytes_to_read>UINT64_MAX-(index_base+offset))return -4;
        p.index_address=index_base+offset;
        if(p.count && p.instances && (!index_base || (index_base&((1u<<index_type)-1))))return -5;
    }
    *out=p;return 0;
}
int xbg_draw_indirect_address(uint64_t base,uint32_t element,int64_t stride,
    int32_t index_type,uint64_t *out){
    if(!out || !base || (base&3) || index_type< -1 || index_type>2)return -1;
    uint64_t bytes=index_type<0?16:20;
    if(stride==0)stride=(int64_t)bytes;
    if(stride<(int64_t)bytes || (stride&3))return -2;
    if(element && (uint64_t)stride>(UINT64_MAX-base)/element)return -3;
    uint64_t address=base+(uint64_t)element*(uint64_t)stride;
    if(bytes>UINT64_MAX-address)return -3;
    *out=address;return 0;
}
