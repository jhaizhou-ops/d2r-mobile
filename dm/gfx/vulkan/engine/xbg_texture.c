#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include "xbg_texture.h"
#include <string.h>
#include "../../tex/nvn_deswizzle.h"
/* Unmodified, pinned MIT decoder; provenance/license in third_party/bcdec/. */
#define BCDECDEF static inline
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec/bcdec.h"
/* Upstream precise signed BC4/5 casts endpoints to plain char. AArch64's
 * default differs from x86; require -fsigned-char rather than silently
 * interpreting negative endpoints as positive on the phone. */
_Static_assert((char)-1 < 0,"bcdec signed formats require -fsigned-char");

int xbg_bc_8bit_channels=0;
int xbg_bc_format_info(uint32_t fmt,xbg_bc_format *out) {
    if(!out || fmt<0x41 || fmt>0x50)return -1;
    out->pixels=XBG_RGBA8_UNORM;out->bytes_per_pixel=4;out->bytes_per_block=16;
    if(fmt==0x41 || fmt==0x42 || fmt==0x45 || fmt==0x46 || fmt==0x49 || fmt==0x4a)out->bytes_per_block=8;
    if((fmt>=0x45 && fmt<=0x48) || fmt==0x4e)out->pixels=XBG_RGBA8_SRGB;
    if(fmt==0x49){out->pixels=XBG_R16_UNORM;out->bytes_per_pixel=2;}
    if(fmt==0x4a){out->pixels=XBG_R16_SNORM;out->bytes_per_pixel=2;}
    if(fmt==0x4b)out->pixels=XBG_RG16_UNORM;
    if(fmt==0x4c)out->pixels=XBG_RG16_SNORM;
    if(fmt==0x4f || fmt==0x50){out->pixels=XBG_RGBA16_SFLOAT;out->bytes_per_pixel=8;}
    /* 8 位通道回退：设备不能线性过滤 16 位 R/RG（Mali）。SNORM 仍保留 16 位。
     * 8-bit channel fallback: the device cannot filter 16-bit R/RG (Mali). Signed formats keep 16 bits. */
    if(xbg_bc_8bit_channels){
        if(fmt==0x49){out->pixels=XBG_R8_UNORM;out->bytes_per_pixel=1;}
        if(fmt==0x4b){out->pixels=XBG_RG8_UNORM;out->bytes_per_pixel=2;}
    }
    return 0;
}
static uint32_t bc_linear_format(uint32_t f) {
    return f>=0x45 && f<=0x48?f-4:f==0x4e?0x4d:f;
}
int xbg_bc_view_compatible(uint32_t source,uint32_t view) {
    return source>=0x41 && source<=0x50 && view>=0x41 && view<=0x50 &&
        bc_linear_format(source)==bc_linear_format(view);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint32_t mip(uint32_t n, uint32_t l) { n >>= l; return n ? n : 1; }
static void zero(void *p, size_t n) { uint8_t *b=p; for(size_t i=0;i<n;i++) b[i]=0; }
static int map(uint32_t code, uint32_t *nvn, uint32_t *pixels, uint32_t *bpp) {
    *bpp=4; *pixels=XBG_RGBA8_UNORM;
    switch(code) {
    case 0x1f: *nvn=0x25; break;
    case 0x39: *nvn=0x42; break;
    case 0x3a: *nvn=0x46; *pixels=XBG_RGBA8_SRGB; break;
    case 0x3d: *nvn=0x44; break;
    case 0x3e: *nvn=0x48; *pixels=XBG_RGBA8_SRGB; break;
    case 0x3f: *nvn=0x49; *pixels=XBG_R16_UNORM; *bpp=2; break;
    case 0x41: *nvn=0x4b; if(xbg_bc_8bit_channels){*pixels=XBG_RG8_UNORM;*bpp=2;}else *pixels=XBG_RG16_UNORM; break;
    default: return -1;
    }
    return 0;
}
static nvn_texdesc desc(const xbg_texture *t) {
    nvn_texdesc d={1,(int)t->nvn_format,0,0,t->width,t->height,1,t->levels,0,
                    (int)t->block_height_log2+1}; return d;
}
int xbg_nvn_texture_layout(const xbg_nvn_texture_desc *d,size_t n,xbg_texture *t) {
    if(!d || !t || !d->width || !d->height || d->width>16384 || d->height>16384 ||
       !d->levels || d->levels>15 || (d->block_height_log2!=UINT32_MAX && d->block_height_log2>5)) return -1;
    zero(t,sizeof(*t));t->width=d->width;t->height=d->height;t->levels=d->levels;t->nvn_format=d->format;
    uint32_t max=d->width>d->height?d->width:d->height,possible=1;
    while(max>1){max>>=1;possible++;}if(d->levels>possible)return -1;
    t->pixels=XBG_RGBA8_UNORM;t->bytes_per_pixel=4;t->block_linear=1;
    if(d->format!=0x25) {
        xbg_bc_format info;if(xbg_bc_format_info(d->format,&info))return -2;
        t->pixels=info.pixels;t->bytes_per_pixel=info.bytes_per_pixel;
    }
    nvn_texdesc nd={1,(int)d->format,0,0,d->width,d->height,1,d->levels,0,
        d->block_height_log2==UINT32_MAX?0:(int)d->block_height_log2+1};
    nvn_texlayout layout;nvn_tex_layout(&nd,&layout);
    if(layout.size>n || layout.size>UINT32_MAX) return -3;
    t->block_height_log2=layout.tileH;
    for(uint32_t l=0;l<t->levels;l++) {
        t->offsets[l]=nvn_tex_level_offset(&nd,l);
        uint64_t next=l+1<t->levels?nvn_tex_level_offset(&nd,l+1):layout.size;
        t->sizes[l]=(uint32_t)(next-t->offsets[l]);
    }
    return 0;
}
int xbg_texture_parse(const void *source, size_t n, xbg_texture *t) {
    if(!source || !t || n<36) return -1;
    const uint8_t *b=source; zero(t,sizeof(*t));
    int sprite=b[0]=='S' && b[1]=='p' && b[2]=='A' && b[3]=='1';
    if(!sprite && (b[0]!='<' || b[1]!='D' || b[2]!='E' || b[3]!='(')) return -2;
    if(sprite && (n<40 || b[5] || !((unsigned)b[20]|(unsigned)b[21]<<8) || le32(b+36)!=4)) return -2;
    t->source_code=b[4];
    if(map(t->source_code,&t->nvn_format,&t->pixels,&t->bytes_per_pixel)) return -3;
    t->width=le32(b+8); t->height=le32(b+12); t->levels=sprite?1:le32(b+28);
    if(!t->width || !t->height || t->width>16384 || t->height>16384 ||
       !t->levels || t->levels>15) return -4;
    uint32_t max=t->width>t->height?t->width:t->height, possible=1;
    while(max>1) {max>>=1;possible++;} if(t->levels>possible) return -4;
    uint32_t marker=le32(b+(sprite?24:20));
    if(marker!=0 && marker!=0x07000000u) return -5;
    t->block_linear=marker!=0; t->block_height_log2=b[sprite?23:19];
    if(t->block_linear && t->block_height_log2>5) return -5;
    t->header_bytes=sprite?40:36+8*t->levels; if(n<t->header_bytes) return -6;
    uint64_t total=0;
    for(uint32_t l=0;l<t->levels;l++) {
        t->offsets[l]=total; t->sizes[l]=le32(b+(sprite?32:36+l*8)); total+=t->sizes[l];
        if(!t->sizes[l] || total>n-t->header_bytes) return -6;
    }
    if(total!=n-t->header_bytes) return -6;
    nvn_texdesc d=desc(t); nvn_texlayout layout; nvn_tex_layout(&d,&layout);
    int bw,bh; int bpb=nvn_fmt_bpb(d.format,&bw,&bh);
    if(t->block_linear && layout.size!=total) return -7;
    for(uint32_t l=0;l<t->levels;l++) {
        uint64_t sz=(uint64_t)((mip(t->width,l)+bw-1)/bw)*
            ((mip(t->height,l)+bh-1)/bh)*(unsigned)bpb;
        if(t->block_linear) {
            uint64_t expected=nvn_tex_level_offset(&d,l);
            uint64_t next=l+1<t->levels?nvn_tex_level_offset(&d,l+1):layout.size;
            if(t->offsets[l]!=expected || t->sizes[l]!=next-expected) return -7;
        } else if(t->sizes[l]!=sz) return -7;
    } return 0;
}
size_t xbg_texture_mip_bytes(const xbg_texture *t, uint32_t l) {
    if(!t || l>=t->levels || l>=16 || !t->width || !t->height || t->width>16384 || t->height>16384 ||
       (t->bytes_per_pixel!=2 && t->bytes_per_pixel!=4 && t->bytes_per_pixel!=8)) return 0;
    return (size_t)mip(t->width,l)*mip(t->height,l)*t->bytes_per_pixel;
}
static void color(const uint8_t *b, uint8_t p[16][4], int four, int opaque) {
    unsigned c0=b[0]|b[1]<<8, c1=b[2]|b[3]<<8; unsigned v[4][4];
    for(unsigned k=0;k<2;k++) {
        unsigned c=k?c1:c0, r=(c>>11)&31, g=(c>>5)&63, bl=c&31;
        v[k][0]=(r<<3)|(r>>2); v[k][1]=(g<<2)|(g>>4); v[k][2]=(bl<<3)|(bl>>2); v[k][3]=255;
    }
    for(unsigned j=0;j<3;j++) {
        v[2][j]=(four || c0>c1)?(2*v[0][j]+v[1][j])/3:(v[0][j]+v[1][j])/2;
        v[3][j]=(four || c0>c1)?(v[0][j]+2*v[1][j])/3:0;
    }
    v[2][3]=255; v[3][3]=(four || c0>c1 || opaque)?255:0;
    uint32_t bits=le32(b+4);
    for(unsigned i=0;i<16;i++) for(unsigned j=0;j<4;j++) p[i][j]=(uint8_t)v[(bits>>(i*2))&3][j];
}
static void channel(const uint8_t *b, uint16_t p[16]) {
    /* R16 keeps the 1/5 and 1/7 interpolants instead of rounding them to R8. */
    uint32_t v[8]={b[0]*257u,b[1]*257u};
    for(unsigned k=2;k<8;k++) v[k]=b[0]>b[1]?
        ((8-k)*v[0]+(k-1)*v[1]+3)/7:k<6?((6-k)*v[0]+(k-1)*v[1]+2)/5:k==6?0:65535;
    uint64_t bits=0; for(unsigned k=0;k<6;k++) bits|=(uint64_t)b[k+2]<<(k*8);
    for(unsigned i=0;i<16;i++) p[i]=(uint16_t)v[(bits>>(i*3))&7];
}
int xbg_decode_block(uint32_t fmt,const void *block,size_t bn,void *out,size_t on) {
    if(!block || !out) return -1;
    xbg_bc_format info;if(xbg_bc_format_info(fmt,&info))return -2;
    if(bn<info.bytes_per_block || on<16*info.bytes_per_pixel)return -1;
    const uint8_t *b=block; uint8_t *dst=out; uint8_t rgba[16][4]; uint16_t r[16],g[16];
    if(fmt==0x4a || fmt>=0x4c) {
        /* bcdec uses typed block reads. NVN buffer offsets and caller output
         * need not be aligned, so decode only through aligned local storage. */
        uint64_t aligned[2]={0,0};memcpy(aligned,block,info.bytes_per_block);
        if(fmt==0x4a || fmt==0x4c) {
            float values[32];unsigned channels=fmt==0x4a?1:2;
            if(channels==1)bcdec_bc4_float(aligned,values,4,1);
            else bcdec_bc5_float(aligned,values,8,1);
            for(unsigned i=0;i<16*channels;i++) {
                /* Round to nearest SNORM16 (max error 0.5/32767); never lose
                 * the sign, and normalize both signed endpoint -128/-127. */
                float scaled=values[i]*32767.0f;
                int16_t value=(int16_t)(scaled+(scaled<0?-0.5f:0.5f));
                memcpy(dst+i*2,&value,2);
            }
        } else if(fmt==0x4d || fmt==0x4e) {
            uint32_t pixels[16];bcdec_bc7(aligned,pixels,16);memcpy(dst,pixels,64);
        } else {
            uint16_t rgb[48],pixels[64];bcdec_bc6h_half(aligned,rgb,12,fmt==0x4f);
            for(unsigned i=0;i<16;i++) {for(unsigned k=0;k<3;k++)pixels[i*4+k]=rgb[i*3+k];pixels[i*4+3]=0x3c00;}
            memcpy(dst,pixels,sizeof(pixels));
        }
        return 0;
    }
    if(fmt==0x49 || fmt==0x4b) {
        unsigned channels=fmt==0x49?1:2;
        const int eight=xbg_bc_8bit_channels && (fmt==0x49 || fmt==0x4b);
        if(bn<channels*8 || on<channels*16*(eight?1:2)) return -1;
        channel(b,r); if(channels==2) channel(b+8,g);
        if(eight){ for(unsigned i=0;i<16;i++) {dst[i*channels]=(uint8_t)(r[i]>>8);if(channels==2) dst[i*2+1]=(uint8_t)(g[i]>>8);} return 0; }
        for(unsigned i=0;i<16;i++) {
            dst[i*channels*2]=(uint8_t)r[i]; dst[i*channels*2+1]=(uint8_t)(r[i]>>8);
            if(channels==2) {dst[i*4+2]=(uint8_t)g[i];dst[i*4+3]=(uint8_t)(g[i]>>8);}
        } return 0;
    }
    if(fmt<0x41 || fmt>0x48 || on<64) return -2;
    unsigned base=fmt>=0x45?fmt-4:fmt;
    unsigned bytes=base<=0x42?8:16; if(bn<bytes) return -1;
    color(b+(bytes==16?8:0),rgba,bytes==16,base==0x41);
    if(base==0x43) {
        uint64_t a=0;for(unsigned k=0;k<8;k++) a|=(uint64_t)b[k]<<(8*k);
        for(unsigned i=0;i<16;i++) rgba[i][3]=(uint8_t)(((a>>(i*4))&15)*17);
    } else if(base==0x44) {
        channel(b,r);for(unsigned i=0;i<16;i++) rgba[i][3]=(uint8_t)((r[i]+128)/257);
    }
    for(unsigned i=0;i<16;i++) for(unsigned j=0;j<4;j++) dst[i*4+j]=rgba[i][j];
    return 0;
}
/* BC 软解并行：马良 GPU 不支持 BC（textureCompressionBC=0），游戏的 BC 贴图全在翻译线程上逐块软解成 RGBA。
 * 选角 / 进局装载时翻译线程 ~80% 忙在 xbg_decode_block，游戏线程等它传完贴图；进局后新贴图流入也在帧里解。
 * 块与块互不依赖，大图（>= 4096 块）按块行分给常驻工作线程和调用线程一起解，输出逐字节相同。DM_BC_THREADS=工作线程数（缺省 0 = 串行：Mate90 实测逐张唤醒同步比解码本身贵，4 线程时选角装载里 BC 软解 9.1 s 对串行 4.9 s）。
 * Parallel BC software decode: the Maleoon GPU has no BC (textureCompressionBC=0), so the game's BC textures are decoded block by block into RGBA on the
 * translation thread. While loading the menu / a run the translation thread is ~80% busy in xbg_decode_block and the game thread waits for the uploads;
 * after entering, newly streamed textures are decoded in-frame too. Blocks are independent: large images (>= 4096 blocks) are split by block rows between
 * resident worker threads and the calling thread; the output is byte-identical. DM_BC_THREADS = worker count (default 0 = serial: on Mate90 waking and synchronizing per image cost more than the decode; with 4 threads BC decode took 9.1 s of the menu load vs 4.9 s serial). */
/* 整块快路径：BC1–BC3 与旧路径同一套整数算式（调色板按块算一次、打包成 32 位像素），直接按行距写进目标，省掉 128 字节中转和逐像素拷贝；
 * BC7 直接用 bcdec 按行距解进目标。只处理完整的 4x4 内部块、每像素 4 字节；边缘块和其他格式走原路径。DM_BC_FASTVERIFY=1 逐张与原路径逐字节比对。
 * Full-block fast path: BC1–BC3 use the same integer arithmetic as the old path (palette computed once per block, packed as 32-bit pixels) and write
 * straight into the destination at its row pitch, skipping the 128-byte staging and per-pixel copies; BC7 is decoded by bcdec straight into the destination.
 * Only full interior 4x4 blocks at 4 bytes per pixel; edge blocks and other formats take the old path. DM_BC_FASTVERIFY=1 compares every image byte for byte. */
static int bc_fast_block(uint32_t fmt,const uint8_t *b,uint8_t *d,size_t pitch) {
    if(fmt==0x4d || fmt==0x4e){uint64_t aligned[2];memcpy(aligned,b,16);bcdec_bc7(aligned,d,(int)pitch);return 1;}
    if(fmt<0x41 || fmt>0x48)return 0;
    unsigned base=fmt>=0x45?fmt-4:fmt; const uint8_t *cb=base<=0x42?b:b+8; int four=base>0x42, opaque=base==0x41;
    unsigned c0=cb[0]|cb[1]<<8, c1=cb[2]|cb[3]<<8, v[4][4];
    for(unsigned k=0;k<2;k++){unsigned c=k?c1:c0,r=(c>>11)&31,g=(c>>5)&63,bl=c&31;v[k][0]=(r<<3)|(r>>2);v[k][1]=(g<<2)|(g>>4);v[k][2]=(bl<<3)|(bl>>2);v[k][3]=255;}
    for(unsigned j=0;j<3;j++){v[2][j]=(four || c0>c1)?(2*v[0][j]+v[1][j])/3:(v[0][j]+v[1][j])/2;v[3][j]=(four || c0>c1)?(v[0][j]+2*v[1][j])/3:0;}
    v[2][3]=255; v[3][3]=(four || c0>c1 || opaque)?255:0;
    uint32_t pal[4]; for(unsigned k=0;k<4;k++)pal[k]=v[k][0]|v[k][1]<<8|v[k][2]<<16|v[k][3]<<24;
    uint32_t bits=(uint32_t)cb[4]|(uint32_t)cb[5]<<8|(uint32_t)cb[6]<<16|(uint32_t)cb[7]<<24;
    uint32_t alpha[16]; int has_alpha=base>=0x43;
    if(base==0x43){uint64_t a=0;for(unsigned k=0;k<8;k++)a|=(uint64_t)b[k]<<(8*k);for(unsigned i=0;i<16;i++)alpha[i]=(uint32_t)(((a>>(i*4))&15)*17);}
    else if(base==0x44){
        uint32_t a16[8]={b[0]*257u,b[1]*257u},a8[8];
        for(unsigned k=2;k<8;k++)a16[k]=b[0]>b[1]?((8-k)*a16[0]+(k-1)*a16[1]+3)/7:k<6?((6-k)*a16[0]+(k-1)*a16[1]+2)/5:k==6?0:65535;
        for(unsigned k=0;k<8;k++)a8[k]=(uint8_t)((a16[k]+128)/257);
        uint64_t ab=0;for(unsigned k=0;k<6;k++)ab|=(uint64_t)b[k+2]<<(k*8);
        for(unsigned i=0;i<16;i++)alpha[i]=a8[(ab>>(i*3))&7];
    }
    for(unsigned y=0;y<4;y++){uint8_t *row=d+y*pitch;
        for(unsigned x=0;x<4;x++){unsigned i=y*4+x;uint32_t px=pal[(bits>>(i*2))&3];if(has_alpha)px=(px&0x00ffffffu)|alpha[i]<<24;memcpy(row+x*4,&px,4);}}
    return 1;
}
typedef struct {
    uint32_t fmt,w,h,depth; const uint8_t *src; size_t row,image,blocks,rows,units; unsigned bpp,block_bytes; uint8_t *dst;
    atomic_size_t next; atomic_int err; atomic_uint active;
} bc_job;
static int bc_rows(bc_job *j,size_t unit) {   /* 一个单位 = 一层里的 4 个块行 | one unit = 4 block rows of one layer */
    size_t per=(j->rows+3)/4,z=unit/per,by0=(unit%per)*4; uint8_t pixels[128];
    for(size_t by=by0;by<by0+4 && by<j->rows;by++)for(size_t bx=0;bx<j->blocks;bx++){
        int r=xbg_decode_block(j->fmt,j->src+z*j->image+by*j->row+bx*j->block_bytes,j->block_bytes,pixels,sizeof(pixels));if(r)return r;
        for(unsigned y=0;y<4 && by*4+y<j->h;y++)for(unsigned x=0;x<4 && bx*4+x<j->w;x++)
            memcpy(j->dst+(((size_t)z*j->h+by*4+y)*j->w+bx*4+x)*j->bpp,pixels+(y*4+x)*j->bpp,j->bpp);
    }
    return 0;
}
static void bc_run(bc_job *j) {
    for(;;){size_t u=atomic_fetch_add(&j->next,1);if(u>=j->units)return;int r=bc_rows(j,u);if(r)atomic_store(&j->err,r);}
}
static pthread_mutex_t bc_mu=PTHREAD_MUTEX_INITIALIZER,bc_submit=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t bc_cv=PTHREAD_COND_INITIALIZER,bc_done=PTHREAD_COND_INITIALIZER;
static bc_job *bc_cur;static unsigned bc_gen,bc_threads=0;static int bc_inited;
static void *bc_worker(void *a) {
    (void)a; pthread_setname_np(pthread_self(),"xbg-bcdec"); unsigned seen=0;
    for(;;){pthread_mutex_lock(&bc_mu);while(bc_gen==seen)pthread_cond_wait(&bc_cv,&bc_mu);seen=bc_gen;bc_job *j=bc_cur;if(j)atomic_fetch_add(&j->active,1);pthread_mutex_unlock(&bc_mu);
        if(!j)continue;bc_run(j);pthread_mutex_lock(&bc_mu);if(atomic_fetch_sub(&j->active,1)==1)pthread_cond_broadcast(&bc_done);pthread_mutex_unlock(&bc_mu);}
    return NULL;
}
static unsigned bc_pool(void) {   /* 首次用时建线程；返回工作线程数 | create threads on first use; returns the worker count */
    pthread_mutex_lock(&bc_mu);
    if(!bc_inited){bc_inited=1;const char *e=getenv("DM_BC_THREADS");unsigned n=e?(unsigned)atoi(e):0;if(n>8)n=8;
        for(unsigned i=0;i<n;i++){pthread_t t;if(pthread_create(&t,NULL,bc_worker,NULL))break;pthread_detach(t);bc_threads++;}}
    unsigned n=bc_threads;pthread_mutex_unlock(&bc_mu);return n;
}
/* 启动剖析计数（宿主 diag.c，DM_BOOTPROF=1 才累加；图形单测里没有宿主 → 弱符号为空）| boot-profile counter (host diag.c, accumulates only with DM_BOOTPROF=1; no host in graphics unit tests → weak symbol is null) */
void dm_prof_add(int id,uint64_t ns,uint64_t bytes) __attribute__((weak));
static int bc_decode(uint32_t fmt,uint32_t w,uint32_t h,uint32_t depth,const void *source,size_t source_bytes,size_t row,size_t image,void *output,size_t out_bytes);
int xbg_decode_bc_linear(uint32_t fmt,uint32_t w,uint32_t h,uint32_t depth,
    const void *source,size_t source_bytes,size_t row,size_t image,void *output,size_t out_bytes) {
    struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);int r=bc_decode(fmt,w,h,depth,source,source_bytes,row,image,output,out_bytes);clock_gettime(CLOCK_MONOTONIC,&t1);
    if(dm_prof_add)dm_prof_add(10,(uint64_t)(t1.tv_sec-t0.tv_sec)*1000000000ull+(uint64_t)(t1.tv_nsec-t0.tv_nsec),out_bytes);   /* 10 = PROF_GFX_BCDEC（dm.h）| 10 = PROF_GFX_BCDEC (dm.h) */
    return r;
}
static int bc_decode(uint32_t fmt,uint32_t w,uint32_t h,uint32_t depth,
    const void *source,size_t source_bytes,size_t row,size_t image,void *output,size_t out_bytes) {
    if(!source || !output || !w || !h || !depth)return -1;
    xbg_bc_format info;if(xbg_bc_format_info(fmt,&info))return -2;
    unsigned bpp=info.bytes_per_pixel,block_bytes=info.bytes_per_block;
    if((size_t)w>SIZE_MAX/h || (size_t)w*h>SIZE_MAX/depth || (size_t)w*h*depth>SIZE_MAX/bpp)return -1;
    size_t out_size=(size_t)w*h*depth*bpp;
    if(out_size>out_bytes)return -1;
    size_t blocks=((size_t)w+3)/4,rows=((size_t)h+3)/4,minimum=blocks*block_bytes;
    if(!row)row=minimum;
    if(row<minimum || row%block_bytes || row>SIZE_MAX/rows)return -1;
    if(!image)image=row*rows;
    if(image<row*rows || image>SIZE_MAX/depth)return -1;
    size_t needed=image*(depth-1)+row*(rows-1)+minimum;
    if(needed>source_bytes)return -1;
    if(blocks*rows*depth>=4096 && bc_pool()){
        bc_job j;memset(&j,0,sizeof j);j.fmt=fmt;j.w=w;j.h=h;j.depth=depth;j.src=source;j.row=row;j.image=image;j.blocks=blocks;j.rows=rows;
        j.units=((rows+3)/4)*depth;j.bpp=bpp;j.block_bytes=block_bytes;j.dst=output;atomic_init(&j.next,0);atomic_init(&j.err,0);atomic_init(&j.active,0);
        pthread_mutex_lock(&bc_submit);   /* 一次一张：工作线程只认 bc_cur | one image at a time: workers only see bc_cur */
        pthread_mutex_lock(&bc_mu);bc_cur=&j;bc_gen++;pthread_cond_broadcast(&bc_cv);pthread_mutex_unlock(&bc_mu);
        bc_run(&j);
        pthread_mutex_lock(&bc_mu);bc_cur=NULL;while(atomic_load(&j.active))pthread_cond_wait(&bc_done,&bc_mu);pthread_mutex_unlock(&bc_mu);
        pthread_mutex_unlock(&bc_submit);
        {static int verify=-1;if(verify<0){const char *e=getenv("DM_BC_VERIFY");verify=e&&*e=='1';}   /* 校验：再串行解一遍逐字节比 | verification: decode serially again and compare byte for byte */
         if(verify && !atomic_load(&j.err)){uint8_t *ref=malloc(out_size);if(ref){bc_job k=j;k.dst=ref;int bad=0;for(size_t u=0;u<k.units;u++)if(bc_rows(&k,u)){bad=-1;break;}
             static unsigned long checked,mismatched;checked++;if(!bad && memcmp(ref,output,out_size))mismatched++;
             if(checked%64==1 || mismatched)fprintf(stderr,"[xbg] BC parallel verify: %lu images, %lu mismatched\n",checked,mismatched);free(ref);}}}
        return atomic_load(&j.err);
    }
    uint8_t pixels[128];const uint8_t *src=source;uint8_t *dst=output;
    static int fast=-1,fverify=0;if(fast<0){const char *e=getenv("DM_BC_FAST");fast=!(e&&*e=='0');e=getenv("DM_BC_FASTVERIFY");fverify=e&&*e=='1';}
    uint8_t *ref=NULL;if(fast && fverify && bpp==4)ref=malloc(out_size);
    for(int pass=ref?1:0;pass>=0;pass--){uint8_t *out=pass?ref:dst;int use_fast=fast && bpp==4 && !pass;   /* pass 1 = 原路径参考 | pass 1 = old-path reference */
    for(uint32_t z=0;z<depth;z++)for(size_t by=0;by<rows;by++)for(size_t bx=0;bx<blocks;bx++){
        const uint8_t *blk=src+z*image+by*row+bx*block_bytes;
        if(use_fast && bx*4+4<=w && by*4+4<=h && bc_fast_block(fmt,blk,out+(((size_t)z*h+by*4)*w+bx*4)*4,(size_t)w*4))continue;
        int r=xbg_decode_block(fmt,blk,block_bytes,pixels,sizeof(pixels));if(r){free(ref);return r;}
        for(unsigned y=0;y<4 && by*4+y<h;y++)for(unsigned x=0;x<4 && bx*4+x<w;x++)
            memcpy(out+(((size_t)z*h+by*4+y)*w+bx*4+x)*bpp,pixels+(y*4+x)*bpp,bpp);
    }}
    if(ref){static unsigned long checked,mismatched;checked++;if(memcmp(ref,dst,out_size))mismatched++;
        if(checked%256==1 || mismatched)fprintf(stderr,"[xbg] BC fast verify: %lu images, %lu mismatched (fmt %#x)\n",checked,mismatched,fmt);free(ref);}
    return 0;
}
static int decode_verified(const void *source,const xbg_texture *t,uint32_t l,void *out,size_t on) {
    if(l>=t->levels || on<xbg_texture_mip_bytes(t,l)) return -1;
    int bw,bh; unsigned bpb=(unsigned)nvn_fmt_bpb(t->nvn_format,&bw,&bh);
    uint32_t w=mip(t->width,l),h=mip(t->height,l),blocks=(w+bw-1)/bw,rows=(h+bh-1)/bh;
    nvn_texdesc d=desc(t); unsigned th,td;nvn_tex_level_tiles(&d,l,&th,&td);
    const uint8_t *src=(const uint8_t *)source+t->header_bytes+t->offsets[l]; uint8_t *dst=out;
    for(uint32_t y=0;y<rows;y++) for(uint32_t x=0;x<blocks;x++) {
        uint64_t at=t->block_linear?nvn_bl_offset(x*bpb,y,0,blocks*bpb,rows,th,td):((uint64_t)y*blocks+x)*bpb;
        if(at+bpb>t->sizes[l]) return -3;
        uint8_t pixel[128];
        if(t->nvn_format==0x25) {for(unsigned k=0;k<4;k++) pixel[k]=src[at+k];}
        else if(xbg_decode_block(t->nvn_format,src+at,bpb,pixel,sizeof(pixel))) return -4;
        for(unsigned py=0;py<(unsigned)bh;py++) for(unsigned px=0;px<(unsigned)bw;px++) {
            uint32_t dx=x*bw+px,dy=y*bh+py;if(dx>=w || dy>=h) continue;
            for(unsigned k=0;k<t->bytes_per_pixel;k++) dst[((size_t)dy*w+dx)*t->bytes_per_pixel+k]=pixel[(py*bw+px)*t->bytes_per_pixel+k];
        }
    }
    return 0;
}
int xbg_texture_decode_mip(const void *source,size_t n,const xbg_texture *t,uint32_t l,void *out,size_t on) {
    if(!source || !t || !out) return -1;
    /* Re-parse untrusted external descriptors: bounds cannot be bypassed by a stale struct. */
    xbg_texture actual; if(xbg_texture_parse(source,n,&actual)) return -2;
    const uint8_t *ta=(const uint8_t *)t,*tb=(const uint8_t *)&actual;
    for(size_t i=0;i<sizeof(actual);i++) if(ta[i]!=tb[i]) return -2;
    return decode_verified(source,t,l,out,on);
}
int xbg_nvn_texture_decode_mip(const void *source,size_t n,const xbg_nvn_texture_desc *d,uint32_t l,void *out,size_t on) {
    if(!source || !d || !out)return -1;xbg_texture t;
    if(xbg_nvn_texture_layout(d,n,&t))return -2;
    return decode_verified(source,&t,l,out,on);
}
