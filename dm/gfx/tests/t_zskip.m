#include "../nvn/nvn_mtl.m"
#include "t_descriptor_fixture.h"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w=(want), g=(got); count++; printf("%s 期望 %llu → 真结果 %llu %s\n", name, (unsigned long long)w, (unsigned long long)g, w==g?"✓":"✗"); fail += w != g; } while (0)
typedef struct { uint8_t *p; size_t n; } chain;
static chain chain_new(void) { chain c={0}; if (posix_memalign((void **)&c.p,getpagesize(),65536)) abort(); memset(c.p,0,65536); mtl_pool_add(c.p,65536); return c; }
static void emit(chain *c, unsigned op, const void *a, size_t n) { size_t z=(8+n+7)&~7ull; op_hdr h={op,(uint32_t)z}; memcpy(c->p+c->n,&h,8); if(n) memcpy(c->p+c->n+8,a,n); c->n+=z; }
static tex_t tex(unsigned w) { static uint64_t s=9000; tex_t t={.serial=s++}; t.d.width=t.d.height=w; t.d.depth=t.d.samples=1; t.d.levels=2; t.d.target=NVN_TEXTURE_TARGET_2D; t.d.format=NVN_FORMAT_RGBA8; return t; }
static int view(const tex_t *t, int lv) { static int id=1200; view_t v={.set_levels=1,.base_level=lv,.nlevels=1}; fixture_reg_tex(id,t,&v); return id++; }
static void set_rt(chain *c, const tex_t *a, const tex_t *b, int alv) { p_set_rt r={.n=b?2:1}; r.color[0]=a; r.color[1]=b; for(int i=0;i<r.n;i++){r.has_cview[i]=1;r.cview[i].set_levels=1;r.cview[i].base_level=i?0:alv;r.cview[i].nlevels=1;} emit(c,OP_SET_RT,&r,sizeof r); p_viewports v={.n=1};v.v[0][2]=v.v[0][3]=8;emit(c,OP_VIEWPORTS,&v,sizeof v);p_scissor sc={0,0,8,8};emit(c,OP_SCISSOR,&sc,sizeof sc); }
static void clear_color(chain *c, int i, float r, float g, float b) { p_clear_color x={.index=i,.mask=15,.rgba={r,g,b,1}}; emit(c,OP_CLEAR_COLOR,&x,sizeof x); }
static void bind_prog(chain *c, prog_t *p) { p_bind_program b={p,3}; emit(c,OP_BIND_PROGRAM,&b,sizeof b); }
static void bind_tex(chain *c, int id) { p_bind_handles b={.stage=1,.first=0,.n=1};b.h[0]=id;emit(c,OP_BIND_TEX,&b,sizeof b); }
static void draw_cmd(chain *c) { p_draw d={NVN_DRAW_PRIMITIVE_TRIANGLES,0,3}; emit(c,OP_DRAW,&d,sizeof d); }
static void blend(chain *c) { color_st col={1,NVN_LOGIC_OP_COPY,NVN_ALPHA_FUNC_ALWAYS}; p_bind_state x={.kind=1};memcpy(x.bytes,&col,sizeof col);emit(c,OP_BIND_STATE,&x,sizeof x); blend_st b={0,NVN_BLEND_FUNC_SRC_ALPHA,NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO,NVN_BLEND_EQUATION_ADD,NVN_BLEND_EQUATION_ADD};x.kind=0;memcpy(x.bytes,&b,sizeof b);emit(c,OP_BIND_STATE,&x,sizeof x); }
static void ds_off(chain *c) { ds_st d={0,0,NVN_DEPTH_FUNC_ALWAYS,0,NVN_STENCIL_FUNC_ALWAYS,NVN_STENCIL_FUNC_ALWAYS,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP,NVN_STENCIL_OP_KEEP}; p_bind_state x={.kind=5}; memcpy(x.bytes,&d,sizeof d); emit(c,OP_BIND_STATE,&x,sizeof x); }
static void readback(const tex_t *t, uint8_t out[256]) { id<MTLBuffer> b=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; [blit() copyFromTexture:tex_mtl(t) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256]; uint64_t f=mtl_sub_commit(); EXPECT("提交完成",1,mtl_wait(f,5000000000ull)); EXPECT("提交反馈无错误",0,mtl_last_commit_error()); memcpy(out,b.contents,256); }
static NvProg *g_tkeep[256]; static int g_tnkeep;   /* 测试里的程序对象常驻：释放后新对象可能落在同一地址，管线缓存按对象地址认会取到旧管线（生产里程序对象不释放） | Program objects in the tests are permanent: after a free, a new object may land at the same address, and the pipeline cache, keyed by object address, would return the old pipeline (in production program objects are never freed) */
static void setup_prog(prog_t *pg) { NSError *e=nil; id<MTLLibrary> l=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; vertex float4 v(uint n[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n],0,1);}\nfragment float4 red(){return float4(1,0,0,0.5);}\nstruct A{texture2d<float> t[[id(0)]];}; fragment float4 sample(constant A&a[[buffer(0)]]){return a.t.read(uint2(0));}\n" options:nil error:&e]; if(!l){fprintf(stderr,"%s\n",e.localizedDescription.UTF8String);abort();} for(int i=0;i<2;i++)pg[i].serial=8000+i; for(int i=0;i<2;i++){NvProg *p=[NvProg new];p->sh[0]=[NvShader new];p->sh[0]->fn=[l newFunctionWithName:@"v"];p->sh[1]=[NvShader new];p->sh[1]->stage=1;p->sh[1]->fn=[l newFunctionWithName:i?@"sample":@"red"];if(i){p->sh[1]->rf.nb=1;p->sh[1]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.reg=0,.dtt=2};p->sh[1]->ae=[p->sh[1]->fn newArgumentEncoderWithBufferIndex:0];}if(g_tnkeep<256)g_tkeep[g_tnkeep++]=p;serial_put(pg[i].serial,p);} }
// 零源跳过单测（生产 mtl_exec + 手写着色器）：sample 着色器输出 = 被采样贴图的 (0,0) 纹素——对采样值齐次线性，测试里注入事实表
// Zero-source skip unit tests (production mtl_exec + hand-written shaders): the sample shader's output = texel (0,0) of the sampled texture - homogeneous linear in the sampled value; the fact table is injected in the test
static void copy_cmd(chain *c, const tex_t *s, const tex_t *d) { p_copy_t2t x={.src=s,.dst=d}; x.sr=(NVNcopyRegion){0,0,0,8,8,1}; x.dr=x.sr; emit(c,OP_COPY_T2T,&x,sizeof x); }
// mode：0 正例（A 清 0 → B 清 0 后采样 A）；1 传播（A 清 0 → 整张拷 A→C → B 采样 C）；2 A 清成非 0；3 A 清 0 后被画过；4 B 的清除值非 0
// mode: 0 positive case (A cleared to 0 → B cleared to 0 then samples A); 1 propagation (A cleared to 0 → whole-texture copy A→C → B samples C); 2 A cleared to non-zero; 3 A drawn to after being cleared to 0; 4 B's clear value is non-zero
static int run(int mode, int enabled, uint8_t px[256]) {
    tex_t a=tex(8),b=tex(8),c2=tex(8); int aid=view(&a,0), cid=view(&c2,0); chain c=chain_new(); static prog_t pg[2]; memset(pg,0,sizeof pg); setup_prog(pg);
    strcpy(g_tkeep[g_tnkeep-1]->sh[1]->key, "feedfacefeedface_b");
    float av = mode==2 ? 0.5f : 0.0f;
    set_rt(&c,&a,NULL,0); clear_color(&c,0,av,av,av); c.n -= 0;
    { p_clear_color *last=(p_clear_color *)(c.p + c.n - ((8+sizeof(p_clear_color)+7)&~7ull) + 8); last->rgba[3] = av; }   /* clear_color 帮手把 alpha 固定成 1：这里改成与 rgb 一致（0 = 全零） | The clear_color helper fixes alpha at 1: here it is changed to match rgb (0 = all zeros) */
    if (mode==3) { ds_off(&c); bind_prog(&c,&pg[0]); draw_cmd(&c); }
    if (mode==1) copy_cmd(&c,&a,&c2);
    float bv = mode==4 ? 0.25f : 0.0f;
    set_rt(&c,&b,NULL,0); clear_color(&c,0,bv,bv,bv);
    { p_clear_color *last=(p_clear_color *)(c.p + c.n - ((8+sizeof(p_clear_color)+7)&~7ull) + 8); last->rgba[3] = bv; }
    ds_off(&c); bind_prog(&c,&pg[1]); bind_tex(&c,mode==1?cid:aid); draw_cmd(&c); emit(&c,OP_END,NULL,0);
    state_reset(); S.texture_pool = fixture_pool.host; memset(&R,0,sizeof R); vis_clear(); g_vis_on=g_vis_counting=0; setenv("DM_NVN_ZSKIP",enabled?"1":"0",1); g_zskip_env=-1;
    mtl_sub_begin(); uint64_t hh=(uint64_t)c.p; mtl_sub_index(&hh,1); mtl_exec(c.p); int n=zskip_test_count(); readback(&b,px); return n;
}
int main(void){@autoreleasepool{setvbuf(stdout,NULL,_IONBF,0);setenv("DM_NVN_TBDR","1",1);mtl_init();uint8_t on[256],off[256];
    run(0,0,off); int n=run(0,1,on); EXPECT("不在事实表：不跳过",0,n); EXPECT("不在事实表：开关逐字节相同",0,(uint64_t)(memcmp(on,off,256)!=0));
    zskip_test_add("feedfacefeedface",-1,0,0);
    static const char *nm[5]={"正例（A 清 0）","传播（A 清 0 → 拷到 C）","反例 A 清成非 0","反例 A 清 0 后被画过","反例 B 清除值非 0"}; static const int want[5]={1,1,0,0,0};
    for(int m=0;m<5;m++){ run(m,0,off); n=run(m,1,on); char t1[96],t2[96]; snprintf(t1,sizeof t1,"%s：跳过次数",nm[m]); snprintf(t2,sizeof t2,"%s：开关逐字节相同",nm[m]);
        EXPECT(t1,want[m],n); EXPECT(t2,0,(uint64_t)(memcmp(on,off,256)!=0)); if(m==2) EXPECT("反例 A 非 0：B 读回非 0",1,on[0]!=0); if(m==0) EXPECT("正例：B 读回全 0",0,on[0]|on[1]|on[2]|on[3]); }
    printf("== %d 项，失败 %d ==\n",count,fail); return fail!=0; }}
