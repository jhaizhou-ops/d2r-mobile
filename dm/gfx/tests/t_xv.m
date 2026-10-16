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
// 跨提交附件有效性账本单测：每个「周期」是一次提交。A 每周期先清再画（提交末尾没人再碰它）→ 试用期满后不写回。
// Unit tests for the cross-submit attachment validity ledger: each "cycle" is one submit. A is cleared then drawn every cycle (nobody touches it again at the end of the submit) → not stored once the trial period is over.
static void commit(void) { id<MTLCommandBuffer> cb=g_cb; uint64_t f=mtl_sub_commit(); mtl_wait(f,5000000000ull); (void)cb; }
static prog_t t_pg[2];
// kind：0 = 清 A、画 A（合规周期）；1 = 清 A、画 A、再用 B 采样 A 并读回 B（A 提交内被读 → 照常写回；验证重置后的内容）；2 = 不清 A，直接 B 采样 A（读旧内容 = 违规）
// kind: 0 = clear A, draw A (compliant cycle); 1 = clear A, draw A, then B samples A and B is read back (A is read within the submit → stored as usual; verifies the contents after reset); 2 = do not clear A, B samples A directly (reading old contents = violation)
static tex_t t_mid, t_x; static int t_two;   // t_two：A 的通道多挂一张（换一个通道签名 = 换一个键） | t_two: A's pass has one more attachment (a different pass signature = a different key)
static void cycle(const tex_t *a, const tex_t *b, int aid, int kind, uint8_t px[256]) {
    chain c=chain_new();
    if (kind!=2) { set_rt(&c,a,t_two?&t_x:NULL,0); clear_color(&c,0,0,0,0); if (t_two) clear_color(&c,1,0,0,0); ds_off(&c); bind_prog(&c,&t_pg[0]); blend(&c); draw_cmd(&c);
                   set_rt(&c,&t_mid,NULL,0); clear_color(&c,0,0,0,0); draw_cmd(&c); }   /* A 的通道在命令流里结束（下一个 SetRenderTargets），和游戏里一样；最后这个通道在提交结束时才结束（一律写回） | A's pass ends in the command stream (at the next SetRenderTargets), as in the game; this last pass only ends when the submit ends (always stored) */
    if (kind) { set_rt(&c,b,NULL,0); clear_color(&c,0,0,0,0); ds_off(&c); bind_prog(&c,&t_pg[1]); bind_tex(&c,aid); draw_cmd(&c); }
    emit(&c,OP_END,NULL,0); state_reset(); S.texture_pool = fixture_pool.host; memset(&R,0,sizeof R); vis_clear(); g_vis_on=g_vis_counting=0;
    mtl_sub_begin(); uint64_t hh=(uint64_t)c.p; mtl_sub_index(&hh,1); mtl_exec(c.p);
    if (kind) readback(b,px); else commit();
}
int main(void){@autoreleasepool{setvbuf(stdout,NULL,_IONBF,0);setenv("DM_NVN_TBDR","1",1);
    char path[256]; snprintf(path,sizeof path,"/tmp/t_xv_%d.txt",(int)getpid()); unlink(path); setenv("DM_XV_FILE",path,1);
    mtl_init(); setup_prog(t_pg); t_mid=tex(8); t_x=tex(8); uint8_t ref[256],px[256]; int dc=0,viol=0,dc0,viol0;
    // ① 正例：试用期（XV_TRIAL 个合规周期）内照常写回，之后才不写回；被作废后重新清、画，内容与最初一致
    // ① Positive case: during the trial period (XV_TRIAL compliant cycles) it is stored as usual, and only afterwards is it not stored; after being invalidated it is cleared and drawn again, and the contents match the original
    { tex_t a=tex(8),b=tex(8); int aid=view(&a,0);
      cycle(&a,&b,aid,1,ref);
      xv_test_counts(&dc0,&viol0);
      /* 计数按键（格式 / 尺寸 / 通道签名 / 附件序号）累计，同键的别的贴图也会推进试用期——所以断言看 A 自己在账本里的状态 | Counts accumulate per key (format / size / pass signature / attachment index), and other textures with the same key also advance the trial period - so the assertions look at A's own state in the ledger */
      #define ST(t) (xv_find((t).serial,0,0) ? xv_find((t).serial,0,0)->state : 0)
      cycle(&a,&b,aid,0,NULL);
      EXPECT("首个合规周期：A 在试用期（照常写回）",XV_PROBE,(uint64_t)ST(a)); xv_test_counts(&dc,&viol); EXPECT("首个合规周期：不写回次数 0",0,(uint64_t)(dc-dc0));
      for(int k=0;k<XV_TRIAL+2;k++) cycle(&a,&b,aid,0,NULL);
      EXPECT("试用期满：A 已作废（不写回）",XV_INVALID,(uint64_t)ST(a)); xv_test_counts(&dc,&viol); EXPECT("试用期满后不写回次数 ≥ 1",1,(uint64_t)(dc-dc0>=1)); EXPECT("合规周期无违规",0,(uint64_t)(viol-viol0));
      cycle(&a,&b,aid,1,px); xv_test_counts(&dc0,&viol); EXPECT("作废后重置再用：内容与最初逐字节相同",0,(uint64_t)(memcmp(px,ref,256)!=0)); EXPECT("重置再用无违规",0,(uint64_t)(viol-viol0));
      // ② 违规：先让 A 再次被作废，然后不清就读
      // ② Violation: first let A be invalidated again, then read without clearing
      for(int k=0;k<2;k++) cycle(&a,&b,aid,0,NULL);
      xv_test_counts(&dc0,&viol0); cycle(&a,&b,aid,2,px); xv_test_counts(&dc,&viol); EXPECT("作废后被读：违规 +1",1,(uint64_t)(viol-viol0));
      for(int k=0;k<XV_TRIAL+3;k++) cycle(&a,&b,aid,0,NULL); EXPECT("违规后该键永久不做（A 不再进账本）",0,(uint64_t)ST(a));
      FILE *f=fopen(path,"r"); char line[256]; int nl=0; while(f&&fgets(line,sizeof line,f)) nl++; if(f) fclose(f); EXPECT("违规落盘 1 行",1,(uint64_t)nl);
      int nd=g_xv_ndis; g_xv_init=0; g_xv_ndis=0; xv_init(); EXPECT("重读文件后禁用集合恢复（模拟下次启动）",1,(uint64_t)(g_xv_ndis>=1 && nd>=1)); }
    // ③ 试用期里见读者：不报红、不落盘、该键不做（不同尺寸 = 不同键）
    // ③ A reader seen during the trial period: no failure reported, nothing written to disk, that key is not done (different size = different key)
    { tex_t a=tex(8),b=tex(8); int aid=view(&a,0); t_two=1;
      xv_test_counts(&dc0,&viol0); cycle(&a,&b,aid,0,NULL); cycle(&a,&b,aid,2,px); for(int k=0;k<6;k++) cycle(&a,&b,aid,0,NULL);
      xv_test_counts(&dc,&viol); EXPECT("试用期见读者：不算违规",0,(uint64_t)(viol-viol0)); EXPECT("试用期见读者：A 的键不做（A 不进账本）",0,(uint64_t)ST(a)); }
    unlink(path); printf("== %d 项，失败 %d ==\n",count,fail); return fail!=0; }}
