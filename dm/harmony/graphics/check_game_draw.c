/* Real production NVN entrypoints/recorder + pure execution planner; no GPU. */
#include "xbg_game_internal.h"
#include "../compat-r32/gfx/nvn/nvn_protos.h"
#define FN(n) ((NVNT_##n)xbg_nvn_get_proc_address(#n))
static unsigned char memory[4096];
static int record(const uint8_t **p,uint32_t expected,void *out,size_t bytes){
    op_hdr h;memcpy(&h,*p,8);if(h.op!=expected || h.size<8+bytes)return -1;
    if(bytes)memcpy(out,*p+8,bytes);*p+=h.size;return 0;
}
int xbg_game_draw_selftest(void){
    NVNmemoryPoolBuilder pb={0};NVNmemoryPool pool={0};NVNcommandBuffer c={0};
    FN(nvnMemoryPoolBuilderSetDefaults)(&pb);FN(nvnMemoryPoolBuilderSetStorage)(&pb,memory,sizeof(memory));
    if(!FN(nvnMemoryPoolInitialize)(&pool,&pb) || !FN(nvnCommandBufferInitialize)(&c,0))return 1;
    FN(nvnCommandBufferAddCommandMemory)(&c,&pool,0,sizeof(memory));FN(nvnCommandBufferBeginRecording)(&c);
    FN(nvnCommandBufferBindVertexBuffer)(&c,15,0x12345000,384);
    FN(nvnCommandBufferBindVertexBuffer)(&c,0,0,0); // explicit unbind is legal
    FN(nvnCommandBufferBindVertexBuffers)(&c,16,0,0); // empty range emits nothing
    FN(nvnCommandBufferDrawArraysInstanced)(&c,4,7,11,3,5);
    FN(nvnCommandBufferDrawArraysInstanced)(&c,4,9,0,2,7);
    uint32_t arrays[4]={11,5,7,3},indices[5]={12,6,9,(uint32_t)-4,2},limit=2;
    FN(nvnCommandBufferDrawArraysIndirect)(&c,4,(uint64_t)arrays);
    FN(nvnCommandBufferDrawElementsIndirect)(&c,4,1,0x10000,(uint64_t)indices);
    FN(nvnCommandBufferMultiDrawArraysIndirectCount)(&c,4,(uint64_t)arrays,(uint64_t)&limit,2,32);
    const uint8_t *p=(const uint8_t *)FN(nvnCommandBufferEndRecording)(&c);
    if(record(&p,OP_BEGIN,0,0))return 2;
    p_bind_vbuf vb={0};if(record(&p,OP_BIND_VBUF,&vb,offsetof(p_bind_vbuf,r)+sizeof(NVNbufferRange)))return 3;
    if(vb.first!=15 || vb.n!=1 || vb.r[0].address!=0x12345000 || vb.r[0].size!=384)return 4;
    if(record(&p,OP_BIND_VBUF,&vb,offsetof(p_bind_vbuf,r)+sizeof(NVNbufferRange)) || vb.first!=0 || vb.n!=1 || vb.r[0].address || vb.r[0].size)return 5;
    xbg_draw_call call={0};if(record(&p,OP_COUNT_+7,&call,sizeof(call)))return 6;
    if(call.first!=7 || call.count!=11 || call.base_instance!=3 || call.instances!=5 || call.index_type!=-1)return 7;
    if(record(&p,OP_COUNT_+7,&call,sizeof(call)) || call.first!=9 || call.count!=0 || call.instances!=7)return 8;
    xbg_indirect indirect={0};if(record(&p,OP_COUNT_+8,&indirect,sizeof(indirect)))return 9;
    if(indirect.arguments!=(uint64_t)arrays || indirect.count_address || indirect.index_type!=-1 || indirect.max_count!=1)return 10;
    // Parameters change after recording: execution planning must see new data.
    arrays[0]=17;arrays[3]=8;
    if(xbg_draw_indirect_plan(&call,indirect.primitive,indirect.index_type,indirect.index_address,(const void*)indirect.arguments,16) || call.count!=17 || call.base_instance!=8 || call.first!=7 || call.instances!=5)return 11;
    if(record(&p,OP_COUNT_+8,&indirect,sizeof(indirect)) || indirect.index_address!=0x10000 || indirect.arguments!=(uint64_t)indices || indirect.index_type!=1)return 12;
    if(xbg_draw_indirect_plan(&call,indirect.primitive,indirect.index_type,indirect.index_address,(const void*)indirect.arguments,20) || call.count!=12 || call.instances!=6 || call.index_address!=0x10012 || call.base_vertex!=-4 || call.base_instance!=2)return 13;
    if(record(&p,OP_COUNT_+3,&indirect,sizeof(indirect)) || indirect.count_address!=(uint64_t)&limit || indirect.stride!=32 || indirect.max_count!=2)return 14;
    if(record(&p,OP_END,0,0))return 15;
    if(xbg_vertex_buffer_range(-1,1)==0 || xbg_vertex_buffer_range(16,1)==0 || xbg_vertex_buffer_range(INT32_MAX,1)==0 || xbg_vertex_buffer_range(1,INT32_MAX)==0)return 16;
    return 0;
}

int xbg_game_vertex_record_selftest(void){
    NVNmemoryPoolBuilder pb={0};NVNmemoryPool pool={0};NVNcommandBuffer c={0};
    FN(nvnMemoryPoolBuilderSetDefaults)(&pb);FN(nvnMemoryPoolBuilderSetStorage)(&pb,memory,sizeof(memory));
    if(!FN(nvnMemoryPoolInitialize)(&pool,&pb) || !FN(nvnCommandBufferInitialize)(&c,0))return 1;
    FN(nvnCommandBufferAddCommandMemory)(&c,&pool,0,sizeof(memory));FN(nvnCommandBufferBeginRecording)(&c);
    NVNvertexAttribState attrs[16];uint32_t expected[16];
    for(int i=0;i<16;i++){expected[i]=1u|((uint32_t)i<<27);memcpy(&attrs[i],expected+i,4);}
    FN(nvnCommandBufferBindVertexAttribState)(&c,16,attrs);
    FN(nvnCommandBufferBindVertexAttribState)(&c,2,attrs);
    FN(nvnCommandBufferBindVertexAttribState)(&c,0,0);
    const uint8_t *p=(const uint8_t*)FN(nvnCommandBufferEndRecording)(&c);
    if(record(&p,OP_BEGIN,0,0))return 2;
    uint32_t state[16]={0};const int counts[3]={16,2,0};
    for(int step=0;step<3;step++){
        int n=counts[step];p_bind_vattr a={0};
        if(record(&p,OP_BIND_VATTR,&a,offsetof(p_bind_vattr,st)+(size_t)n*4) || a.first || a.n!=n)return 3;
        if(xbg_vertex_attributes_replace(state,(const uint32_t*)a.st,n))return 4;
        for(int i=0;i<16;i++)if(state[i]!=(i<n?expected[i]:0))return 5;
    }
    return record(&p,OP_END,0,0)?6:0;
}

int xbg_game_clear_record_selftest(void){
    NVNmemoryPoolBuilder pb={0};NVNmemoryPool pool={0};NVNcommandBuffer c={0};
    FN(nvnMemoryPoolBuilderSetDefaults)(&pb);FN(nvnMemoryPoolBuilderSetStorage)(&pb,memory,sizeof(memory));
    if(!FN(nvnMemoryPoolInitialize)(&pool,&pb) || !FN(nvnCommandBufferInitialize)(&c,0))return 1;
    FN(nvnCommandBufferAddCommandMemory)(&c,&pool,0,sizeof(memory));FN(nvnCommandBufferBeginRecording)(&c);
    int signed_color[4]={-1,INT32_MIN,123,INT32_MAX};uint32_t unsigned_color[4]={0xffffffff,0x80000000,987,0};
    FN(nvnCommandBufferClearColori)(&c,7,signed_color,5);
    FN(nvnCommandBufferClearColorui)(&c,3,unsigned_color,10);
    FN(nvnCommandBufferClearDepthStencil)(&c,0.25f,1,0xa5,0x33);
    NVNtexture texture={0};NVNcopyRegion region={5,6,2,7,8,3};
    FN(nvnCommandBufferClearTexturei)(&c,&texture,0,&region,signed_color,5);
    const uint8_t *p=(const uint8_t*)FN(nvnCommandBufferEndRecording)(&c);
    if(record(&p,OP_BEGIN,0,0))return 2;
    xbg_typed_clear a={0};
    if(record(&p,OP_COUNT_+9,&a,sizeof(a)) || a.scalar!=1 || a.color.index!=7 || a.color.mask!=5 || memcmp(a.color.rgba,signed_color,16))return 3;
    if(record(&p,OP_COUNT_+9,&a,sizeof(a)) || a.scalar!=2 || a.color.index!=3 || a.color.mask!=10 || memcmp(a.color.rgba,unsigned_color,16))return 4;
    p_clear_ds ds={0};if(record(&p,OP_CLEAR_DS,&ds,sizeof(ds)) || ds.depth!=0.25f || ds.depth_mask!=1 || ds.stencil!=0xa5 || ds.stencil_mask!=0x33)return 5;
    p_clear_tex tex={0};if(record(&p,OP_CLEAR_TEX,&tex,sizeof(tex)) || tex.tex!=(const tex_t*)&texture || tex.is_uint!=2 || tex.mask!=5 || memcmp(&tex.c,signed_color,16) || memcmp(&tex.r,&region,sizeof(region)))return 7;
    return record(&p,OP_END,0,0)?6:0;
}
