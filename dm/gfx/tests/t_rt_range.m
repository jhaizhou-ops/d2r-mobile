// 实际RT单级选择不变，仅view范围4→1；同纹理同CB原guard参考与候选，手写常量shader不冒游戏shader。
// The actual RT single-level selection is unchanged, only the view range goes 4→1; same texture and same CB for the original-guard reference and the candidate; hand-written constant shaders do not pass themselves off as game shaders.
#include "../nvn/nvn_mtl.m"
#include "t_descriptor_fixture.h"
void *front_metal_layer(void){return NULL;}
module_t *module_of(const void *p){(void)p;return NULL;}
module_t g_mod[MAX_MOD];int g_nmod;
void svc_thread_dump(void){}
uint64_t now_ns(void){return bp_now();}
static int cases,bad,enabled,small_depth,force_invalid,sample_attachment,both_flags;
#define E(n,w,a) do{uint64_t actual=(a),want=(w);cases++;printf("%s 期望%llu → 真结果%llu\n",n,(unsigned long long)want,(unsigned long long)actual);bad+=want!=actual;}while(0)
typedef struct{uint8_t b[8192];size_t n;} code;
static void emit(code *c,int op,const void *p,size_t n){size_t z=(8+n+7)&~7ull;op_hdr h={op,(uint32_t)z};memcpy(c->b+c->n,&h,8);if(n)memcpy(c->b+c->n+8,p,n);c->n+=z;}
static void st(code *c,int k,const void *v,size_t n){p_bind_state b={.kind=k};memcpy(b.bytes,v,n);emit(c,OP_BIND_STATE,&b,sizeof b);}
static tex_t color={.serial=52000},depth={.serial=52001};static prog_t pg[2]={{.serial=52002},{.serial=52003}};
static p_set_rt rt(int levels){p_set_rt r={.n=1,.depth=&depth,.has_dview=1};r.color[0]=&color;r.has_cview[0]=1;
 r.cview[0]=(view_t){.set_levels=1,.set_layers=1,.set_format=1,.set_target=1,.nlevels=levels,.nlayers=1,.format=NVN_FORMAT_RGBA8,.target=NVN_TEXTURE_TARGET_2D};
 r.dview=r.cview[0];r.dview.nlevels=1;r.dview.base_level=small_depth?1:0;r.dview.format=NVN_FORMAT_DEPTH32F_STENCIL8;return r;}
static void seed(void){RPD *p=[RPD new];p.colorAttachments[0].texture=tex_mtl(&color);p.colorAttachments[0].loadAction=MTLLoadActionClear;p.colorAttachments[0].storeAction=MTLStoreActionStore;p.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
 p.depthAttachment.texture=p.stencilAttachment.texture=tex_mtl(&depth);p.depthAttachment.loadAction=p.stencilAttachment.loadAction=MTLLoadActionClear;p.depthAttachment.storeAction=p.stencilAttachment.storeAction=MTLStoreActionStore;p.depthAttachment.clearDepth=.75;p.stencilAttachment.clearStencil=0x2d;enc_r_end(enc_r_begin(p));
 if(sample_attachment){RPD *q=[RPD new];q.colorAttachments[0].texture=tex_mtl(&color);q.colorAttachments[0].level=1;q.colorAttachments[0].loadAction=MTLLoadActionClear;q.colorAttachments[0].storeAction=MTLStoreActionStore;q.colorAttachments[0].clearColor=MTLClearColorMake(0,1,0,0);enc_r_end(enc_r_begin(q));}
 if(small_depth){p.colorAttachments[0].texture=nil;p.depthAttachment.level=p.stencilAttachment.level=1;enc_r_end(enc_r_begin(p));}}
static void rr_read_planes(id<MTLBuffer> __strong *b){id<MTLTexture> t=tex_mtl(&color),d=tex_mtl(&depth);[blit()copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)sourceSize:MTLSizeMake(8,8,1)toBuffer:b[0]destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
 [blit()copyFromTexture:d sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)sourceSize:MTLSizeMake(8,8,1)toBuffer:b[1]destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256 options:MTLBlitOptionDepthFromDepthStencil];
 [blit()copyFromTexture:d sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)sourceSize:MTLSizeMake(8,8,1)toBuffer:b[2]destinationOffset:0 destinationBytesPerRow:8 destinationBytesPerImage:64 options:MTLBlitOptionStencilFromDepthStencil];}
int main(int argc,char **argv){@autoreleasepool{enabled=1;for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--disabled"))enabled=0;else if(!strcmp(argv[i],"--small-depth"))small_depth=1;else if(!strcmp(argv[i],"--force-invalid"))force_invalid=1;else if(!strcmp(argv[i],"--sample-attachment"))sample_attachment=1;else if(!strcmp(argv[i],"--both-flags"))both_flags=1;}if(enabled)setenv("DM_NVN_RT_RANGE","1",1);else unsetenv("DM_NVN_RT_RANGE");if(both_flags)setenv("DM_NVN_VIEW_CONTINUE","1",1);else unsetenv("DM_NVN_VIEW_CONTINUE");mtl_init();NSError *err=nil;
 id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;vertex float4 v(uint n[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n],.5,1);}\nfragment float4 r(){return float4(1,0,0,1);}\nfragment float4 g(){return float4(0,1,0,0);}struct A{texture2d<float> t[[id(0)]];};fragment float4 s(constant A&a[[buffer(0)]]){return a.t.read(uint2(0));}\n"options:nil error:&err];if(!lib)abort();
 for(int i=0;i<2;i++){NvProg *p=[NvProg new];p->sh[0]=[NvShader new];p->sh[0]->fn=[lib newFunctionWithName:@"v"];p->sh[1]=[NvShader new];p->sh[1]->fn=[lib newFunctionWithName:i?(sample_attachment?@"s":@"g"):@"r"];p->sh[1]->stage=1;p->sh[1]->rf.color_mask=1;if(i&&sample_attachment){p->sh[1]->rf.nb=1;p->sh[1]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_TEX,.dtt=MTLTextureType2D};p->sh[1]->ae=[p->sh[1]->fn newArgumentEncoderWithBufferIndex:0];}
#ifdef NVN_RT_RANGE_RO_IMPL
 p->sh[0]->range_ro=p->sh[1]->range_ro=1; // 本测试手写constant shader，已直接查无外部写 | this test hand-writes constant shaders and has directly verified there are no external writes
#endif
 serial_put(pg[i].serial,p);}
 color.d.width=color.d.height=depth.d.width=depth.d.height=8;color.d.depth=color.d.samples=depth.d.depth=depth.d.samples=1;color.d.levels=4;depth.d.levels=small_depth?2:1;color.d.target=depth.d.target=NVN_TEXTURE_TARGET_2D;color.d.format=NVN_FORMAT_RGBA8;depth.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;
 (void)tex_mtl(&color);(void)tex_mtl(&depth);if(sample_attachment){view_t v={.set_levels=1,.base_level=1,.nlevels=1};fixture_reg_tex(1000,&color,&v);}
 uint16_t *ix;posix_memalign((void **)&ix,getpagesize(),getpagesize());for(int i=0;i<120;i++)ix[i]=i%3;mtl_pool_add(ix,getpagesize());
 code first={0},second={0};p_set_rt a=rt(4),b=rt(1);emit(&first,OP_SET_RT,&a,sizeof a);ds_st ds={.dt=1,.df=NVN_DEPTH_FUNC_LESS};st(&first,5,&ds,sizeof ds);
 color_st col={0,NVN_LOGIC_OP_COPY,NVN_ALPHA_FUNC_ALWAYS};st(&first,1,&col,sizeof col);p_bind_program pr={&pg[0],3};emit(&first,OP_BIND_PROGRAM,&pr,sizeof pr);uint64_t reset=NVN_COUNTER_TYPE_SAMPLES_PASSED;emit(&first,OP_RESET_COUNTER,&reset,8);p_draw dw={NVN_DRAW_PRIMITIVE_TRIANGLES,0,3};emit(&first,OP_DRAW,&dw,sizeof dw);emit(&first,OP_END,NULL,0);
 emit(&second,OP_RESET_COUNTER,&reset,8);emit(&second,OP_SET_RT,&b,sizeof b);if(small_depth){ds_st off={.df=NVN_DEPTH_FUNC_ALWAYS};st(&second,5,&off,sizeof off);}pr.prog=&pg[1];emit(&second,OP_BIND_PROGRAM,&pr,sizeof pr);if(sample_attachment){p_bind_handles h={.stage=1,.first=0,.n=1};h.h[0]=1000;emit(&second,OP_BIND_TEX,&h,sizeof h);}col.blend_en=1;st(&second,1,&col,sizeof col);blend_st blend={0,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO,NVN_BLEND_EQUATION_ADD,NVN_BLEND_EQUATION_ADD};st(&second,0,&blend,sizeof blend);p_draw_idx di={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=120,.itype=1,.ibuf=(uint64_t)ix,.instances=1};emit(&second,OP_DRAW_IDX,&di,sizeof di);emit(&second,OP_END,NULL,0);
 id<MTLBuffer> buffers[2][3];for(int m=0;m<2;m++)for(int j=0;j<3;j++)buffers[m][j]=[g_dev newBufferWithLength:j==2?64:256 options:MTLResourceStorageModeShared];uint64_t passes[2],bytes[2],q[2][2]={{0}};
 mtl_sub_begin();
 for(int m=0;m<2;m++){state_reset(); S.texture_pool = fixture_pool.host;memset(&R,0,sizeof R);vis_clear();g_vis_on=g_vis_counting=0;seed();uint64_t p=g_stat_pass,l=g_bw_load,s=g_bw_store,draws=g_stat_draw;mtl_exec(first.b);E("首绘按深度只读模式",4,g_enc_mode);if(!m)rp_end();else if(force_invalid)R.rt.cview[0].nlevels=1; // 负控：错误地假设附件范围/目标始终等价 | negative control: wrongly assume the attachment range/target is always equivalent
 mtl_exec(second.b);rp_end();passes[m]=g_stat_pass-p;bytes[m]=g_bw_load-l+g_bw_store-s;E("真实draw次数不变",2,g_stat_draw-draws);post((post_op){3,NULL,0,0,(uint64_t)q[m],(uint32_t)reset,vis_snapshot()});rr_read_planes(buffers[m]);if(g_blit){[g_blit endEncoding];g_blit=nil;}}
 int done=mtl_wait(mtl_sub_commit(),5000000000ull);E("同纹理同CB完成",1,done);if(!done)abort();E("提交反馈无错误",0,mtl_last_commit_error());
 uint8_t yellow[4]={255,255,0,0},green[4]={0,255,0,0}; // alpha由第二draw ONE/ ZERO给0；整数闭式RGB黄色。 | alpha is given 0 by the second draw's ONE/ ZERO; integer closed-form RGB yellow.
 for(int m=0;m<2;m++){unsigned bc=0,bd=0,bs=0;for(int i=0;i<64;i++){bc+=memcmp((uint8_t *)buffers[m][0].contents+4*i,small_depth && (i%8>=4||i/8>=4)?green:yellow,4)!=0;bd+=((float *)buffers[m][1].contents)[i]!=.75f;bs+=((uint8_t *)buffers[m][2].contents)[i]!=0x2d;}E("各64纹素手算黄色",0,bc);E("depth .75种子保持",0,bd);E("stencil 0x2d种子保持",0,bs);E("Reset后120索引40三角采样数",2560,q[m][0]);}
 E("三平面原候选逐字节",0,memcmp(buffers[0][0].contents,buffers[1][0].contents,256)||memcmp(buffers[0][1].contents,buffers[1][1].contents,256)||memcmp(buffers[0][2].contents,buffers[1][2].contents,64));
 E("原guard两pass",2,passes[0]);E("候选一pass/默认两pass",enabled&&!small_depth&&!sample_attachment?1:2,passes[1]);{ // DontCare（本通道没写不写回）：两个通道的深度状态都只开测试不开写，每个通道省一次深度模板写回 = 8×8×5 字节（Depth32F_S8 附件模型 5 B/像素） | DontCare (not stored if this pass did not write it): both passes' depth state enables only test, not write, so each pass saves one depth-stencil store = 8×8×5 bytes (Depth32F_S8 attachment model, 5 B/pixel)
   const char *u=getenv("DM_NVN_UNWRITTEN_DC"); int udc=!(u&&!strcmp(u,"0")); unsigned dsb=udc&&!small_depth&&!sample_attachment?8*8*5:0;
   E(udc?"原附件模型2048−2×320（DontCare）":"原附件模型2048",(small_depth?1152:2048)-2*dsb,bytes[0]);
   E(udc?"候选附件模型1024−320（DontCare）":"候选附件模型1024/默认2048",(small_depth?1152:enabled&&!sample_attachment?1024:2048)-(enabled&&!sample_attachment?1:2)*dsb,bytes[1]); }
 const char *dir=getenv("DM_GFX_FIXTURE_OUT");if(dir){char p[1024];for(int m=0;m<2;m++)for(int j=0;j<3;j++){snprintf(p,sizeof p,"%s/%s_%s.raw",dir,m?"candidate":"reference",j==0?"color":j==1?"depth":"stencil");FILE *f=fopen(p,"wb");if(!f||fwrite(buffers[m][j].contents,1,j==2?64:256,f)!=(j==2?64:256))abort();fclose(f);}}
 mtl_pool_del(ix);free(ix);printf("RT_RANGE_GPU cases=%d failed=%d\n",cases,bad);return bad!=0;
}}
