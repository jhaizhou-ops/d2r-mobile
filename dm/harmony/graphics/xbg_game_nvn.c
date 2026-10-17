/* Portable original-NVN interface. Semantic implementation is derived from
 * gfx/nvn/hle_nvn.c; platform Metal/profiling/no-op fallbacks are not included. */
#include "xbg_game_internal.h"
#include "../compat-r32/gfx/nvn/nvn_protos.h"
#include <stdatomic.h>
#include <stddef.h>
#define LOG xbg_logf
#define die xbg_fatalf
#define OLOG(...) ((void)0)
#define NVN(name,ret,...) static ret h_##name(__VA_ARGS__); \
 static const NVNT_##name chk_##name __attribute__((unused))=h_##name; \
 static ret h_##name(__VA_ARGS__)
#define NVN_NOPROTO(name,ret,...) static ret h_##name(__VA_ARGS__)
uint64_t nvn_new_serial(void) { static _Atomic uint64_t n; return ++n; }
// ---------- 布尔参数：Switch 调用方不扩展，只看低 8 位 ----------
#define B(x) ((x) & 0xff)


// ---------- 设备 ----------
NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = f; }
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d; memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0); o->flags = ((const dev_builder_t *)b)->flags;
    OLOG("DeviceInitialize %p flags=%#x", (void *)d, o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { ((dev_t_ *)d)->magic = 0; }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d; return (PFNNVNGENERICFUNCPTRPROC)xbg_nvn_get_proc_address(name);
}

// 设备属性：Tegra X1 实测值（nnheaders nvn_DeviceConstantsNX.h，在 SMO 上逐项调 nvnDeviceGetInteger 得到）
static const int g_devinfo[] = {
    [0x00] = 53, [0x01] = 311,   // API 版本：游戏读了不比较（main 0x173cbd8），此值未核
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    if(xbg_device_integer(what,out)) return;
    unsigned w = (unsigned)what;
    // 0x5F 分离采样器绑定数、0x60/0x61 在 SMO 实测表里没有：0x5F 按采样器池上限给 4096 未核，0x60/0x61 报红给 0
    if (w >= sizeof g_devinfo / sizeof *g_devinfo) die("nvnDeviceGetInteger unknown property %u", w);
    *out = g_devinfo[w];
    OLOG("DeviceGetInteger %#x -> %d", w, *out);
}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (NVNimageHandle)id; }
// 分离贴图 / 采样器句柄：值 = 描述符号（采样器号放 20 位以上，与组合句柄的位布局一致）
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d; o->dbg_cb = B(enable) ? (void *)cb : NULL; o->dbg_data = data;
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) { if(m!=NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT)die("Lower-left NVN viewport convention unsupported"); ((dev_t_ *)d)->origin = m; LOG("[nvn] 窗口原点 %s", m ? "左上（同 D3D / Metal）" : "左下（GL）"); }
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) { if(m!=NVN_DEPTH_MODE_NEAR_IS_ZERO)die("Negative-one NVN depth convention unsupported"); ((dev_t_ *)d)->depth_mode = m; LOG("[nvn] 深度模式 %s", m ? "近=0（同 D3D / Metal）" : "近=-w（GL）"); }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; xbg_release_command_handle(h); }

// ---------- 内存池 ----------
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) { memset(b, 0, sizeof(pool_builder_t)); }
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { ((pool_builder_t *)b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { ((pool_builder_t *)b)->mem = m; ((pool_builder_t *)b)->size = n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { ((pool_builder_t *)b)->flags = f; }
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s = (const pool_builder_t *)b; pool_t *o = (pool_t *)p;
    o->magic = MAGIC('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = s->mem; o->size = s->size;
    if (o->mem) { xbg_legacy_pool_add(o->mem, o->size);  }
    if (!o->mem && !(o->flags & 0x200)) LOG("【红】MemoryPoolInitialize 非虚拟池没有存储 flags=%#x size=%#llx", o->flags, o->size);
    OLOG("MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, o->size, o->flags);
    return 1;
}
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) { if (((pool_t *)p)->mem) {  xbg_legacy_pool_del(((pool_t *)p)->mem); } ((pool_t *)p)->magic = 0; }
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { return (NVNbufferAddress)((const pool_t *)p)->mem; }

// ---------- 描述符池 ----------
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *tp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)tp = (dpool_t){(const pool_t *)p, off, n}; OLOG("TexturePool n=%d", n);  return 1;
}
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *tp) { (void)tp; }
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) { xbg_register_texture((const dpool_t *)tp, id, (const tex_t *)t, (const view_t *)v); }
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *sp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)sp = (dpool_t){(const pool_t *)p, off, n}; OLOG("SamplerPool n=%d", n); return 1;
}
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *sp) { (void)sp; }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *sp, int id, const NVNsampler *s) { xbg_register_sampler((const dpool_t *)sp, id, &((const samp_t *)s)->p); }

// ---------- 贴图 ----------
#define TB(b) ((tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = f; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
static uint64_t g_layout_logged;
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("[nvn] 预排布贴图 %ux%ux%u 级%u 格式%#x 目标%d 布局字节 %02x%02x%02x%02x%02x%02x%02x%02x → 块高 log2=%u（取布局第 0 字节）尺寸 %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) LOG("【红】TextureBuilderGetStorageSize 未知格式 %#x", ((const tex_builder_t *)b)->d.format);
    log_layout((const tex_builder_t *)b, &L);
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L); return L.align;
}
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = (const tex_builder_t *)b; tex_t *o = TX(t);
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8);
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    o->serial = nvn_new_serial();
    OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, o->L.size);
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture *t) { xbg_legacy_tex_release(TX(t)); TX(t)->magic = 0; }
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->L.size; }
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem;
}
// 头文件无原型；main 0x174899c 一类调用点把返回值与 1 比，不等则置某状态位 0x100。含义未核，先给 1
NVN_NOPROTO(nvnTextureGetRawStorageClass, int, const NVNtexture *t) { (void)t; return 1; }
#define VW(v) ((view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = t; }
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) {
    const view_t *o = (const view_t *)v; *min = o->min_layer; *n = o->nlayers; return o->set_layers;
}
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t); const view_t *w = (const view_t *)v;
    if (!w) return 0;
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * o->L.layer_size;
    return (ptrdiff_t)off;
}
// CPU 读贴图（NVN ReadTexels）：预排布贴图以池内存为真值（解排布直读），GPU 写出的贴图从 MTLTexture 读回（nvn_mtl.m）
static void read_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    xbg_legacy_read_texels(o, level, layer0, r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    read_texels(CTX(t), (const view_t *)v, r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    read_texels(CTX(t), (const view_t *)v, r, dst, row, img);
}
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)v; (void)r; xbg_texture_invalidate(CTX(t)); }

// ---------- 采样器 ----------
#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;   // GL 默认
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f; o->lod_max = 1000.f; o->aniso = 1.f;
}
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { ((samp_builder_t *)b)->dev = d; }
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { SB(b)->minf = mi; SB(b)->magf = ma; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->ws = s; SB(b)->wt = t; SB(b)->wr = r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float lo, float hi) { SB(b)->lod_min = lo; SB(b)->lod_max = hi; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->cmp_mode = m; SB(b)->cmp_func = f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->border, c, 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->aniso = v; }
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { (void)b; if (v != 0) die("nvnSamplerBuilderSetLodSnap nonzero unsupported"); }
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    samp_t *o = (samp_t *)s; o->magic = MAGIC('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p; return 1;
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }

// ---------- 程序 ----------
NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d; memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC('P', 'R', 'O', 'G'); return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram *p) { xbg_legacy_prog_release((prog_t *)p); ((prog_t *)p)->magic = 0; }
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { LOG("【红】ProgramSetShaders 阶段数 %d", n); return 0; }
    xbg_legacy_prog_release(o); o->nstage = n; memcpy(o->sd, sd, n * sizeof *sd); o->serial = nvn_new_serial();
    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    return 1;
}

// ---------- 同步 / 事件 ----------
NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { (void)d; *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0}; return 1; }
NVN(nvnSyncFinalize, void, NVNsync *s) { ((sync_t *)s)->magic = 0; }
// 围栏 = 「此前提交到队列的命令全部完成」：记下最近一次提交的栅栏值
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) { (void)q; (void)c; (void)f; ((sync_t *)s)->fenced = 1; ((sync_t *)s)->fence = xbg_legacy_last_fence(); }
// 没围栏过的同步对象按真机行为等到超时；已围栏的只在 GPU 真正完成到该栅栏值后放行
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    const sync_t *o = (const sync_t *)s;
    if (!o->fenced) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    if (xbg_legacy_wait(o->fence, 0)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    return xbg_legacy_wait(o->fence, timeout) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { *(ev_builder_t *)b = (ev_builder_t){(const pool_t *)p, off}; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t *s = (const ev_builder_t *)b;
    if (!s->pool || !s->pool->mem) { LOG("【红】EventInitialize 没有存储"); return 0; }
    *(ev_t *)e = (ev_t){MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t *)(s->pool->mem + s->off)}; return 1;
}
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return *((const ev_t *)e)->val; }
static void ev_signal(const ev_t *e, int mode, uint32_t v) { if (mode == NVN_EVENT_SIGNAL_MODE_ADD) __atomic_fetch_add(e->val,v,__ATOMIC_SEQ_CST); else __atomic_store_n(e->val,v,__ATOMIC_SEQ_CST); }
NVN(nvnEventSignal, void, NVNevent *e, NVNeventSignalMode m, uint32_t v) { ev_signal((const ev_t *)e, m, v); }

// ---------- 队列 ----------
#define QB(b) ((queue_builder_t *)(b))
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
    QB(b)->cmd_sz = 65536; QB(b)->comp_sz = 262144; QB(b)->ctl_sz = 16384; QB(b)->flush = 4096;   // 设备默认值
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_sz = n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_sz = n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_sz = n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = n; }
// 队列内存：命令 + 计算 + 控制三块之和按 4 KiB 取整（宿主不使用，只需游戏按此大小分配）
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    return (o->cmd_sz + o->comp_sz + o->ctl_sz + 4095) & ~(size_t)4095;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_sz = n; }
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    *(queue_t *)q = (queue_t){MAGIC('Q', 'U', 'E', 'U'), ((const queue_builder_t *)b)->dev, 0};
    OLOG("QueueInitialize %p", (void *)q); return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) { ((queue_t *)q)->magic = 0; }
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    (void)q; if (info) memset(info, 0, sizeof *info); return xbg_backend_failed() ? NVN_QUEUE_GET_ERROR_RESULT_GPU_ERROR_UNKNOWN : NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnQueueFlush, void, NVNqueue *q) { (void)q; }
NVN(nvnQueueFinish, void, NVNqueue *q) { (void)q; xbg_legacy_finish(); }

// ---------- 命令缓冲：录进游戏的命令内存，提交时执行 CPU 可见副作用 ----------
#define CB(c) ((cmd_t *)(c))
#define OP_RESERVE 24                             // 段尾永远留够一条 JUMP / END

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    memset(c, 0, sizeof(cmd_t)); CB(c)->magic = MAGIC('C', 'M', 'D', 'B'); CB(c)->dev = d; return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) { CB(c)->magic = 0; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CB(c)->cb = cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *d) { CB(c)->cb_data = d; }
static uint8_t *align8(uint8_t *p) { return (uint8_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7); }
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) { op_hdr h={op,size}; memcpy(p,&h,8); }
static void cmd_new_segment(cmd_t *o,uint8_t *mem,size_t n) {
    uint8_t *start=align8(mem),*end=mem+n;
    if (n<64 || start>end-OP_RESERVE) die("NVN invalid command memory size");
    if (o->recording && o->cur) { put_hdr(o->cur,OP_JUMP,16); memcpy(o->cur+8,&start,8); }
    o->cur=start; o->end=end;
}
NVN(nvnCommandBufferAddCommandMemory,void,NVNcommandBuffer *c,const NVNmemoryPool *p,ptrdiff_t off,size_t n) {
    const pool_t *pl=(const pool_t *)p;
    if (!pl || !pl->mem || n<64) { size_t bytes=n>1048576?n:1048576; cmd_new_segment(CB(c),xbg_host_command_memory(CB(c),bytes),bytes); return; }
    if (off<0 || (uint64_t)off>pl->size || n>pl->size-(uint64_t)off) die("NVN command memory exceeds pool");
    cmd_new_segment(CB(c),pl->mem+off,n);
}
NVN(nvnCommandBufferAddControlMemory,void,NVNcommandBuffer *c,void *m,size_t n) { (void)c; (void)m; (void)n; /* no separate control allocation in this recording format */ }
// 取一条记录的空间；不够则按 NVN 约定回调游戏补命令内存
static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) {
    size = (size + 7) & ~7u;
    for (int tries = 0; !o->cur || o->cur + size + OP_RESERVE > o->end; tries++) {
        if (!o->cb || tries == 2) die("NVN 命令内存耗尽且无法补充（cb=%p）", (void *)o->cb);
        xbg_invoke_command_callback(o, size + OP_RESERVE);
    }
    uint8_t *p = o->cur; o->cur += size; put_hdr(p, 0, size); return p;
}
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (o->recording) LOG("【红】BeginRecording 重入");
    o->recording = 1;
    uint8_t *p = cmd_alloc(o, 8); put_hdr(p, OP_BEGIN, 8); o->begin = p;
}
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    uint8_t *p = cmd_alloc(o, 8); put_hdr(p, OP_END, 8);
    o->recording = 0;
    return (NVNcommandHandle)o->begin;
}
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->recording != 0; }
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    xbg_copy_commands(CB(c),n,(const uint64_t *)h);
}
void xbg_record_command(cmd_t *c,const void *record,uint32_t bytes) {
    if(!c || !c->recording || bytes<8 || (bytes&7))die("Invalid cloned command record");
    memcpy(cmd_alloc(c,bytes),record,bytes);
}
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    const ev_t *ev = (const ev_t *)e; volatile uint32_t *val = ev->val; uint32_t mm = m;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) {
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_RESET_COUNTER, 16); uint64_t tt = t; memcpy(p + 8, &tt, 8);
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_REPORT_COUNTER, 24); uint64_t tt = t; memcpy(p + 8, &tt, 8); memcpy(p + 16, &a, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src, NVNbufferAddress dst, size_t n, int flags) {
    (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 32); put_hdr(p, OP_COPY_B2B, 32); uint64_t nn = n;
    memcpy(p + 8, &src, 8); memcpy(p + 16, &dst, 8); memcpy(p + 24, &nn, 8);
}
// uniform 更新：数据在录制时拷进命令内存（NVN 语义：录制后调用方可改原数据），提交时写进缓冲
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t bufsz, ptrdiff_t off, size_t n, const void *data) {
    if(off<0 || (uint64_t)off>bufsz || n>bufsz-(uint64_t)off || n>0x1000000)die("NVN uniform update outside bound range");
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, OP_UPDATE_UB, (sz + 7) & ~7u);
    uint64_t dst = buf + off, nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8); memcpy(p + 24, data, n);
}

// ---------- 渲染命令：参数按 nvn_obj.h 的载荷格式录进命令内存，回放见 nvn_mtl.m ----------
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
// 带数组的绑定记录按实际个数分配：前缀字段 + n 个元素（回放端只读前 n 个）。原先按最大个数定长记（绑顶点缓冲 272 字节、绑贴图 / 采样器 280 字节，
// 实际常只有 1–2 个），空尾占每帧命令字节的四到五成；88 级法师的高级属性页每帧录 12.5 MB，把游戏自己的命令内存池（约每帧 105 段 × 128 KB）用空（10-04 实测）
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) { *has = v != NULL; if (v) *dst = *(const view_t *)v; }
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv,
    const NVNtexture *dt, const NVNtextureView *dv) {
    if (n < 0 || n > 8) { LOG("【红】SetRenderTargets 数量 %d", n); n = n < 0 ? 0 : 8; }
    p_set_rt *r = REC(c, OP_SET_RT, p_set_rt); r->n = n;
    for (int i = 0; i < n; i++) { r->color[i] = ct ? (const tex_t *)ct[i] : NULL; copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL); }
    r->depth = (const tex_t *)dt; copy_view(&r->dview, &r->has_dview, dv);
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; memcpy(r->rgba, rgba, 16);
}
NVN(nvnCommandBufferClearColori, void, NVNcommandBuffer *c, int index, const int *rgba, int mask) {
    if(!rgba)die("Null signed clear color");xbg_typed_clear *r=(xbg_typed_clear*)rec(c,OP_COUNT_+9,sizeof(*r));r->color.index=index;r->color.mask=mask;r->scalar=1;memcpy(r->color.rgba,rgba,16);
}
NVN(nvnCommandBufferClearColorui, void, NVNcommandBuffer *c, int index, const uint32_t *rgba, int mask) {
    if(!rgba)die("Null unsigned clear color");xbg_typed_clear *r=(xbg_typed_clear*)rec(c,OP_COUNT_+9,sizeof(*r));r->color.index=index;r->color.mask=mask;r->scalar=2;memcpy(r->color.rgba,rgba,16);
}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    p_clear_ds *r = REC(c, OP_CLEAR_DS, p_clear_ds); *r = (p_clear_ds){depth, (int)B(dmask), stencil, smask};
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0 || first + n > 16) { LOG("【红】SetViewports first=%d n=%d", first, n); return; }
    p_viewports *r = REC(c, OP_VIEWPORTS, p_viewports); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) { *REC(c, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) { *REC(c, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    *REC(c, OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){(const prog_t *)p, stages};
}
static void bind_state(NVNcommandBuffer *c, int kind, const void *st, size_t n) { p_bind_state *r = REC(c, OP_BIND_STATE, p_bind_state); r->kind = kind; memcpy(r->bytes, st, n); }
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *st) { bind_state(c, 0, st, sizeof(blend_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *st) { bind_state(c, 1, st, sizeof(color_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *st) { bind_state(c, 2, st, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *st) { bind_state(c, 3, st, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *st) { bind_state(c, 4, st, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *st) { bind_state(c, 5, st, sizeof(ds_st)); }
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *st) {
    if (n < 0 || n > 16 || (n && !st)) die("Invalid BindVertexAttribState count/source");
    p_bind_vattr *r = RECN(c, OP_BIND_VATTR, p_bind_vattr, st, n); r->first = 0; r->n = n; if(n)memcpy(r->st, st, 4 * n);
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *st) {
    if (n < 0 || n > 16) { LOG("【红】BindVertexStreamState n=%d", n); return; }
    p_bind_vstream *r = RECN(c, OP_BIND_VSTREAM, p_bind_vstream, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 8 * n);
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *rg) {
    if (xbg_vertex_buffer_range(first,n) || (n && !rg)) die("NVN vertex buffer range first=%d count=%d invalid",first,n);
    if (!n) return;
    p_bind_vbuf *r = RECN(c, OP_BIND_VBUF, p_bind_vbuf, r, n); r->first = first; r->n = n; memcpy(r->r, rg, sizeof *rg * n);
}
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int index, NVNbufferAddress address, size_t bytes) {
    NVNbufferRange range={address,bytes};h_nvnCommandBufferBindVertexBuffers(c,index,1,&range);
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
static void bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {
    if (n < 0 || n > 32) { LOG("【红】绑定句柄数 %d", n); return; }
    p_bind_handles *r = RECN(c, op, p_bind_handles, h, n); r->stage = st; r->first = first; r->n = n; memcpy(r->h, h, 8 * n);
}
// 图像（可写贴图）：句柄 = 贴图池号（GetImageHandle 原样返回；游戏不调 RegisterImage，图像与贴图共用登记）
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) {
    bind_handles(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) {
    bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) {
    bind_handles(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateSamplerHandle h) {
    bind_handles(c, OP_BIND_SAMP, st, i, 1, &h.value);
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, m, 0, 0, 0}; }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, m, 0, 1}; }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, 0, v, 2}; }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float fa, float un, float cl) {
    *REC(c, OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){fa, un, cl};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n) { *REC(c, OP_DRAW, p_draw) = (p_draw){pr, first, n}; }
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive, int first, int count, int base_instance, int instances) {
    xbg_draw_call plan;if(xbg_draw_arrays_plan(&plan,primitive,first,count,base_instance,instances))die("Invalid DrawArraysInstanced arguments");
    *(xbg_draw_call *)rec(c,OP_COUNT_+7,sizeof(plan))=plan;
}
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib,
    int bv, int bi, int inst) {
    *REC(c, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){pr, it, n, bv, bi, inst, ib};
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    // 跳板是尾跳（br），这里的返回地址就是游戏里的调用点
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){x, y, z, (uint64_t)__builtin_return_address(0)};
}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) { *(int32_t *)rec(c, OP_BARRIER, 8) = bits; }
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { REC(c, OP_SET_TEX_POOL, p_set_pool)->pool = (const dpool_t *)p; }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { REC(c, OP_SET_SAMP_POOL, p_set_pool)->pool = (const dpool_t *)p; }

NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v,
    const NVNcopyRegion *r, int flags) {
    p_copy_b2t *o = REC(c, OP_COPY_B2T, p_copy_b2t); o->src = src; o->tex = (const tex_t *)t; copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r,
    NVNbufferAddress dst, int flags) {
    p_copy_b2t *o = REC(c, OP_COPY_T2B, p_copy_b2t); o->src = dst; o->tex = (const tex_t *)t; copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
// 用给定值清贴图区域（浮点 / 无符号整数两版；mask = 通道写掩码）
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *rgba, int mask) {
    p_clear_tex *o = REC(c, OP_CLEAR_TEX, p_clear_tex); o->tex = (const tex_t *)t; copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 0; memcpy(o->c.f, rgba, 16);
}
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *rgba, int mask) {
    p_clear_tex *o = REC(c, OP_CLEAR_TEX, p_clear_tex); o->tex = (const tex_t *)t; copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 1; memcpy(o->c.u, rgba, 16);
}
NVN(nvnCommandBufferClearTexturei, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const int *rgba, int mask) {
    if(!r || !rgba)die("Null signed texture clear region/value");p_clear_tex *o=REC(c,OP_CLEAR_TEX,p_clear_tex);o->tex=(const tex_t*)t;copy_view(&o->view,&o->has_view,v);o->r=*r;o->mask=mask;o->is_uint=2;memcpy(&o->c,rgba,16);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNcopyRegion *sr,
    const NVNtexture *d, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *o = REC(c, OP_COPY_T2T, p_copy_t2t); o->src = (const tex_t *)s; o->dst = (const tex_t *)d;
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv); o->sr = *sr; o->dr = *dr; o->flags = flags;
}
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t st) { REC(c, OP_COPY_STRIDE, p_copy_stride)->stride = st; }
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(REC(c, OP_BLEND_COLOR, p_blend_color)->rgba, v, 16); }

// ---------- 状态对象：就地打包（nvn_obj.h） ----------
#define ST(T, x) ((T *)(x))
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) { (void)s; (void)start; (void)v; if (n) die("NVN custom sample locations unsupported"); }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG("【红】顶点属性偏移 %td 超出打包范围", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }

// ---------- 窗口 / 出图 ----------
#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) { NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1; }
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0 || n > 4) { LOG("【红】WindowBuilderSetTextures 数量 %d", n); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    memset(o, 0, sizeof *o); o->magic = MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    LOG("[nvn] WindowInitialize 交换链 %d 张 间隔 %d 首张 %dx%d 格式 %#x", o->ntex, o->interval,
        o->ntex ? (int)CTX(o->tex[0])->d.width : 0, o->ntex ? (int)CTX(o->tex[0])->d.height : 0, o->ntex ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int v) { if (((win_t *)w)->interval != v) LOG("[nvn] 游戏设出图间隔 %d → %d", ((win_t *)w)->interval, v); ((win_t *)w)->interval = v; }
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    win_t *o = (win_t *)w; int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) LOG("[nvn] WindowCrop %d,%d %dx%d", x, y, ww, hh);
    memcpy(o->crop, v, sizeof v);
}

NVN(nvnQueueSubmitCommands,void,NVNqueue *q,int n,const NVNcommandHandle *h) {
    if (n<0 || (n && !h)) die("NVN invalid submit list");
    ++((queue_t *)q)->submits; xbg_submit(n,(const uint64_t *)h);
}
NVN(nvnWindowAcquireTexture,NVNwindowAcquireTextureResult,NVNwindow *w,NVNsync *s,int *idx) {
    win_t *o=(win_t *)w; if (o->ntex<1 || o->ntex>4 || !idx) die("NVN invalid window acquire");
    *idx=o->next++%o->ntex;
    sync_t *ss=(sync_t *)s; ss->fenced=1; ss->fence=xbg_legacy_last_fence();
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN(nvnQueuePresentTexture,void,NVNqueue *q,NVNwindow *w,int idx) {
    (void)q; win_t *o=(win_t *)w;
    if (idx<0 || idx>=o->ntex) die("NVN invalid present index");
    xbg_present_texture(o,(const tex_t *)o->tex[idx]); ++o->presents;
}
NVN(nvnWindowFinalize,void,NVNwindow *w) { ((win_t *)w)->magic=0; }
NVN(nvnCommandBufferPushDebugGroupDynamic,void,NVNcommandBuffer *c,uint32_t domain,const char *text) { (void)c; (void)domain; (void)text; }
NVN(nvnCommandBufferPopDebugGroup,void,NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic,void,NVNcommandBuffer *c,uint32_t domain,const char *text) { (void)c; (void)domain; (void)text; }

NVN(nvnCommandBufferSetPatchSize,void,NVNcommandBuffer *c,int n) { *(uint32_t *)rec(c,OP_COUNT_,4)=n; }
NVN(nvnCommandBufferWaitSync,void,NVNcommandBuffer *c,const NVNsync *s) { memcpy(rec(c,OP_COUNT_+1,sizeof(sync_t)),s,sizeof(sync_t)); }
NVN(nvnQueueWaitSync,NVNboolean,NVNqueue *q,const NVNsync *s) { (void)q; const sync_t *o=(const sync_t *)s; return o->fenced && xbg_legacy_wait(o->fence,0); }
NVN(nvnCommandBufferDispatchComputeIndirect,void,NVNcommandBuffer *c,NVNbufferAddress a) { *(uint64_t *)rec(c,OP_COUNT_+2,8)=a; }
NVN(nvnCommandBufferDrawArraysIndirect,void,NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNbufferAddress arguments) {
    *(xbg_indirect *)rec(c,OP_COUNT_+8,sizeof(xbg_indirect))=(xbg_indirect){arguments,0,0,primitive,-1,1,0};
}
NVN(nvnCommandBufferDrawElementsIndirect,void,NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNindexType type,NVNbufferAddress indices,NVNbufferAddress arguments) {
    *(xbg_indirect *)rec(c,OP_COUNT_+8,sizeof(xbg_indirect))=(xbg_indirect){arguments,0,indices,primitive,type,1,0};
}
NVN(nvnCommandBufferMultiDrawArraysIndirectCount,void,NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNbufferAddress args,NVNbufferAddress count,int maximum,ptrdiff_t stride) { if(stride<0 || (uint64_t)stride>INT32_MAX)die("Indirect stride out of backend range"); *(xbg_indirect *)rec(c,OP_COUNT_+3,sizeof(xbg_indirect))=(xbg_indirect){args,count,0,primitive,-1,maximum,stride}; }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount,void,NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNindexType type,NVNbufferAddress indices,NVNbufferAddress args,NVNbufferAddress count,int maximum,ptrdiff_t stride) { if(stride<0 || (uint64_t)stride>INT32_MAX)die("Indirect stride out of backend range"); *(xbg_indirect *)rec(c,OP_COUNT_+3,sizeof(xbg_indirect))=(xbg_indirect){args,count,indices,primitive,type,maximum,stride}; }
NVN(nvnCommandBufferWaitEvent,void,NVNcommandBuffer *c,const NVNevent *e,NVNeventWaitMode mode,uint32_t value) { *(xbg_wait_event *)rec(c,OP_COUNT_+4,sizeof(xbg_wait_event))=(xbg_wait_event){(uint64_t)((const ev_t *)e)->val,mode,value}; }
NVN(nvnCommandBufferSetDepthBounds,void,NVNcommandBuffer *c,uint32_t enabled,float minimum,float maximum) { *(xbg_depth_bounds *)rec(c,OP_COUNT_+5,sizeof(xbg_depth_bounds))=(xbg_depth_bounds){B(enabled),minimum,maximum}; }
NVN(nvnCommandBufferSetShaderScratchMemory,void,NVNcommandBuffer *c,const NVNmemoryPool *p,ptrdiff_t offset,size_t size) { (void)c; const pool_t *pool=(const pool_t *)p; if(size && (!pool || offset<0 || (uint64_t)offset>pool->size || size>pool->size-(uint64_t)offset)) die("NVN shader scratch memory outside pool"); /* Vulkan owns shader spill/private memory; the Tegra backing pool is not addressed by translated shaders. */ }
NVN(nvnCommandBufferDrawElements,void,NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNindexType type,int count,NVNbufferAddress indices) { h_nvnCommandBufferDrawElementsInstanced(c,primitive,type,count,indices,0,0,1); }
NVN(nvnCommandBufferDrawElementsBaseVertex,void,NVNcommandBuffer *c,NVNdrawPrimitive primitive,NVNindexType type,int count,NVNbufferAddress indices,int base) { h_nvnCommandBufferDrawElementsInstanced(c,primitive,type,count,indices,base,0,1); }
NVN(nvnDeviceGetTextureHandle,NVNtextureHandle,const NVNdevice *d,int texture,int sampler) { (void)d; return (uint64_t)(uint32_t)texture | (uint64_t)(uint32_t)sampler<<20; }
static void unsupported_nvnDeviceBuilderGetFlags(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceBuilderGetFlags"); }
static void unsupported_nvnDeviceSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceSetDebugLabel"); }
static void unsupported_nvnDeviceGetCurrentTimestampInNanoseconds(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceGetCurrentTimestampInNanoseconds"); }
static void unsupported_nvnDeviceSetIntermediateShaderCache(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceSetIntermediateShaderCache"); }
static void unsupported_nvnDeviceGetTexelFetchHandle(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceGetTexelFetchHandle"); }
static void unsupported_nvnDeviceGenerateDebugDomainId(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceGenerateDebugDomainId"); }
static void unsupported_nvnDeviceRegisterFastClearColor(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceRegisterFastClearColor"); }
static void unsupported_nvnDeviceRegisterFastClearColori(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceRegisterFastClearColori"); }
static void unsupported_nvnDeviceRegisterFastClearColorui(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceRegisterFastClearColorui"); }
static void unsupported_nvnDeviceRegisterFastClearDepth(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceRegisterFastClearDepth"); }
static void unsupported_nvnDeviceGetWindowOriginMode(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceGetWindowOriginMode"); }
static void unsupported_nvnDeviceGetDepthMode(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceGetDepthMode"); }
static void unsupported_nvnDeviceGetTimestampInNanoseconds(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceGetTimestampInNanoseconds"); }
static void unsupported_nvnDeviceApplyDeferredFinalizes(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceApplyDeferredFinalizes"); }
static void unsupported_nvnDeviceWalkDebugDatabase(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceWalkDebugDatabase"); }
static void unsupported_nvnDeviceIsExternalDebuggerAttached(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceIsExternalDebuggerAttached"); }
static void unsupported_nvnQueueGetTotalCommandMemoryUsed(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueGetTotalCommandMemoryUsed"); }
static void unsupported_nvnQueueGetTotalControlMemoryUsed(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueGetTotalControlMemoryUsed"); }
static void unsupported_nvnQueueGetTotalComputeMemoryUsed(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueGetTotalComputeMemoryUsed"); }
static void unsupported_nvnQueueResetMemoryUsageCounts(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueResetMemoryUsageCounts"); }
static void unsupported_nvnQueueBuilderSetQueuePriority(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderSetQueuePriority"); }
static void unsupported_nvnQueueBuilderGetQueuePriority(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetQueuePriority"); }
static void unsupported_nvnQueueBuilderGetDevice(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetDevice"); }
static void unsupported_nvnQueueBuilderGetFlags(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetFlags"); }
static void unsupported_nvnQueueBuilderGetCommandMemorySize(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetCommandMemorySize"); }
static void unsupported_nvnQueueBuilderGetComputeMemorySize(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetComputeMemorySize"); }
static void unsupported_nvnQueueBuilderGetControlMemorySize(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetControlMemorySize"); }
static void unsupported_nvnQueueBuilderGetCommandFlushThreshold(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetCommandFlushThreshold"); }
static void unsupported_nvnQueueBuilderGetMemorySize(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetMemorySize"); }
static void unsupported_nvnQueueBuilderGetMemory(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueBuilderGetMemory"); }
static void unsupported_nvnQueueSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueSetDebugLabel"); }
static void unsupported_nvnQueueAcquireTexture(void) { xbg_fatalf("NVN API called but not implemented: nvnQueueAcquireTexture"); }
static void unsupported_nvnWindowBuilderSetNumActiveTextures(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowBuilderSetNumActiveTextures"); }
static void unsupported_nvnWindowBuilderGetDevice(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowBuilderGetDevice"); }
static void unsupported_nvnWindowBuilderGetNumTextures(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowBuilderGetNumTextures"); }
static void unsupported_nvnWindowBuilderGetTexture(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowBuilderGetTexture"); }
static void unsupported_nvnWindowBuilderGetNativeWindow(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowBuilderGetNativeWindow"); }
static void unsupported_nvnWindowBuilderGetPresentInterval(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowBuilderGetPresentInterval"); }
static void unsupported_nvnWindowBuilderGetNumActiveTextures(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowBuilderGetNumActiveTextures"); }
static void unsupported_nvnWindowSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowSetDebugLabel"); }
static void unsupported_nvnWindowGetNativeWindow(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowGetNativeWindow"); }
static void unsupported_nvnWindowGetPresentInterval(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowGetPresentInterval"); }
static void unsupported_nvnWindowGetCrop(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowGetCrop"); }
static void unsupported_nvnWindowSetNumActiveTextures(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowSetNumActiveTextures"); }
static void unsupported_nvnWindowGetNumActiveTextures(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowGetNumActiveTextures"); }
static void unsupported_nvnWindowGetNumTextures(void) { xbg_fatalf("NVN API called but not implemented: nvnWindowGetNumTextures"); }
static void unsupported_nvnProgramSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnProgramSetDebugLabel"); }
static void unsupported_nvnProgramSetShadersExt(void) { xbg_fatalf("NVN API called but not implemented: nvnProgramSetShadersExt"); }
static void unsupported_nvnProgramSetSampleShading(void) { xbg_fatalf("NVN API called but not implemented: nvnProgramSetSampleShading"); }
static void unsupported_nvnMemoryPoolBuilderGetDevice(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolBuilderGetDevice"); }
static void unsupported_nvnMemoryPoolBuilderGetMemory(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolBuilderGetMemory"); }
static void unsupported_nvnMemoryPoolBuilderGetSize(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolBuilderGetSize"); }
static void unsupported_nvnMemoryPoolBuilderGetFlags(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolBuilderGetFlags"); }
static void unsupported_nvnMemoryPoolSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolSetDebugLabel"); }
static void unsupported_nvnMemoryPoolMap(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolMap"); }
static void unsupported_nvnMemoryPoolInvalidateMappedRange(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolInvalidateMappedRange"); }
static void unsupported_nvnMemoryPoolMapVirtual(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolMapVirtual"); }
static void unsupported_nvnMemoryPoolGetSize(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolGetSize"); }
static void unsupported_nvnMemoryPoolGetFlags(void) { xbg_fatalf("NVN API called but not implemented: nvnMemoryPoolGetFlags"); }
static void unsupported_nvnTexturePoolSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnTexturePoolSetDebugLabel"); }
static void unsupported_nvnTexturePoolRegisterImage(void) { xbg_fatalf("NVN API called but not implemented: nvnTexturePoolRegisterImage"); }
static void unsupported_nvnTexturePoolGetMemoryPool(void) { xbg_fatalf("NVN API called but not implemented: nvnTexturePoolGetMemoryPool"); }
static void unsupported_nvnTexturePoolGetMemoryOffset(void) { xbg_fatalf("NVN API called but not implemented: nvnTexturePoolGetMemoryOffset"); }
static void unsupported_nvnTexturePoolGetSize(void) { xbg_fatalf("NVN API called but not implemented: nvnTexturePoolGetSize"); }
static void unsupported_nvnSamplerPoolSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerPoolSetDebugLabel"); }
static void unsupported_nvnSamplerPoolRegisterSamplerBuilder(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerPoolRegisterSamplerBuilder"); }
static void unsupported_nvnSamplerPoolGetMemoryPool(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerPoolGetMemoryPool"); }
static void unsupported_nvnSamplerPoolGetMemoryOffset(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerPoolGetMemoryOffset"); }
static void unsupported_nvnSamplerPoolGetSize(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerPoolGetSize"); }
static void unsupported_nvnBufferBuilderSetDevice(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferBuilderSetDevice"); }
static void unsupported_nvnBufferBuilderSetDefaults(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferBuilderSetDefaults"); }
static void unsupported_nvnBufferBuilderSetStorage(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferBuilderSetStorage"); }
static void unsupported_nvnBufferBuilderGetDevice(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferBuilderGetDevice"); }
static void unsupported_nvnBufferBuilderGetMemoryPool(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferBuilderGetMemoryPool"); }
static void unsupported_nvnBufferBuilderGetMemoryOffset(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferBuilderGetMemoryOffset"); }
static void unsupported_nvnBufferBuilderGetSize(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferBuilderGetSize"); }
static void unsupported_nvnBufferInitialize(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferInitialize"); }
static void unsupported_nvnBufferSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferSetDebugLabel"); }
static void unsupported_nvnBufferFinalize(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferFinalize"); }
static void unsupported_nvnBufferMap(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferMap"); }
static void unsupported_nvnBufferGetAddress(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferGetAddress"); }
static void unsupported_nvnBufferFlushMappedRange(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferFlushMappedRange"); }
static void unsupported_nvnBufferInvalidateMappedRange(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferInvalidateMappedRange"); }
static void unsupported_nvnBufferGetMemoryPool(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferGetMemoryPool"); }
static void unsupported_nvnBufferGetMemoryOffset(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferGetMemoryOffset"); }
static void unsupported_nvnBufferGetSize(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferGetSize"); }
static void unsupported_nvnBufferGetDebugID(void) { xbg_fatalf("NVN API called but not implemented: nvnBufferGetDebugID"); }
static void unsupported_nvnTextureBuilderSetSize2D(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderSetSize2D"); }
static void unsupported_nvnTextureBuilderSetSize3D(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderSetSize3D"); }
static void unsupported_nvnTextureBuilderSetSwizzle(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderSetSwizzle"); }
static void unsupported_nvnTextureBuilderSetDepthStencilMode(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderSetDepthStencilMode"); }
static void unsupported_nvnTextureBuilderSetPackagedTextureData(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderSetPackagedTextureData"); }
static void unsupported_nvnTextureBuilderSetGLTextureName(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderSetGLTextureName"); }
static void unsupported_nvnTextureBuilderGetStorageClass(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetStorageClass"); }
static void unsupported_nvnTextureBuilderGetDevice(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetDevice"); }
static void unsupported_nvnTextureBuilderGetTarget(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetTarget"); }
static void unsupported_nvnTextureBuilderGetWidth(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetWidth"); }
static void unsupported_nvnTextureBuilderGetHeight(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetHeight"); }
static void unsupported_nvnTextureBuilderGetDepth(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetDepth"); }
static void unsupported_nvnTextureBuilderGetLevels(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetLevels"); }
static void unsupported_nvnTextureBuilderGetFormat(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetFormat"); }
static void unsupported_nvnTextureBuilderGetSamples(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetSamples"); }
static void unsupported_nvnTextureBuilderGetSwizzle(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetSwizzle"); }
static void unsupported_nvnTextureBuilderGetDepthStencilMode(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetDepthStencilMode"); }
static void unsupported_nvnTextureBuilderGetPackagedTextureData(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetPackagedTextureData"); }
static void unsupported_nvnTextureBuilderGetPackagedTextureLayout(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetPackagedTextureLayout"); }
static void unsupported_nvnTextureBuilderGetStride(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetStride"); }
static void unsupported_nvnTextureBuilderGetSparseTileLayout(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetSparseTileLayout"); }
static void unsupported_nvnTextureBuilderGetGLTextureName(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetGLTextureName"); }
static void unsupported_nvnTextureBuilderGetZCullStorageSize(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetZCullStorageSize"); }
static void unsupported_nvnTextureBuilderGetMemoryPool(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetMemoryPool"); }
static void unsupported_nvnTextureBuilderGetMemoryOffset(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetMemoryOffset"); }
static void unsupported_nvnTextureBuilderGetRawStorageClass(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureBuilderGetRawStorageClass"); }
static void unsupported_nvnTextureViewSetSwizzle(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureViewSetSwizzle"); }
static void unsupported_nvnTextureViewSetDepthStencilMode(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureViewSetDepthStencilMode"); }
static void unsupported_nvnTextureViewGetLevels(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureViewGetLevels"); }
static void unsupported_nvnTextureViewGetFormat(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureViewGetFormat"); }
static void unsupported_nvnTextureViewGetSwizzle(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureViewGetSwizzle"); }
static void unsupported_nvnTextureViewGetDepthStencilMode(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureViewGetDepthStencilMode"); }
static void unsupported_nvnTextureViewGetTarget(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureViewGetTarget"); }
static void unsupported_nvnTextureViewCompare(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureViewCompare"); }
static void unsupported_nvnTextureGetZCullStorageSize(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetZCullStorageSize"); }
static void unsupported_nvnTextureSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureSetDebugLabel"); }
static void unsupported_nvnTextureGetStorageClass(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetStorageClass"); }
static void unsupported_nvnTextureGetTarget(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetTarget"); }
static void unsupported_nvnTextureGetLevels(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetLevels"); }
static void unsupported_nvnTextureGetFormat(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetFormat"); }
static void unsupported_nvnTextureGetSamples(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetSamples"); }
static void unsupported_nvnTextureGetSwizzle(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetSwizzle"); }
static void unsupported_nvnTextureGetDepthStencilMode(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetDepthStencilMode"); }
static void unsupported_nvnTextureGetStride(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetStride"); }
static void unsupported_nvnTextureGetTextureAddress(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetTextureAddress"); }
static void unsupported_nvnTextureGetSparseTileLayout(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetSparseTileLayout"); }
static void unsupported_nvnTextureWriteTexels(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureWriteTexels"); }
static void unsupported_nvnTextureWriteTexelsStrided(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureWriteTexelsStrided"); }
static void unsupported_nvnTextureFlushTexels(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureFlushTexels"); }
static void unsupported_nvnTextureGetMemoryPool(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetMemoryPool"); }
static void unsupported_nvnTextureGetMemoryOffset(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetMemoryOffset"); }
static void unsupported_nvnTextureGetDebugID(void) { xbg_fatalf("NVN API called but not implemented: nvnTextureGetDebugID"); }
static void unsupported_nvnSamplerBuilderSetBorderColori(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderSetBorderColori"); }
static void unsupported_nvnSamplerBuilderSetBorderColorui(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderSetBorderColorui"); }
static void unsupported_nvnSamplerBuilderSetReductionFilter(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderSetReductionFilter"); }
static void unsupported_nvnSamplerBuilderGetDevice(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetDevice"); }
static void unsupported_nvnSamplerBuilderGetLodClamp(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetLodClamp"); }
static void unsupported_nvnSamplerBuilderGetLodBias(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetLodBias"); }
static void unsupported_nvnSamplerBuilderGetCompare(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetCompare"); }
static void unsupported_nvnSamplerBuilderGetBorderColor(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetBorderColor"); }
static void unsupported_nvnSamplerBuilderGetBorderColori(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetBorderColori"); }
static void unsupported_nvnSamplerBuilderGetBorderColorui(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetBorderColorui"); }
static void unsupported_nvnSamplerBuilderGetMaxAnisotropy(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetMaxAnisotropy"); }
static void unsupported_nvnSamplerBuilderGetReductionFilter(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetReductionFilter"); }
static void unsupported_nvnSamplerBuilderGetLodSnap(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerBuilderGetLodSnap"); }
static void unsupported_nvnSamplerSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerSetDebugLabel"); }
static void unsupported_nvnSamplerGetMinMagFilter(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetMinMagFilter"); }
static void unsupported_nvnSamplerGetWrapMode(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetWrapMode"); }
static void unsupported_nvnSamplerGetLodClamp(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetLodClamp"); }
static void unsupported_nvnSamplerGetLodBias(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetLodBias"); }
static void unsupported_nvnSamplerGetCompare(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetCompare"); }
static void unsupported_nvnSamplerGetBorderColor(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetBorderColor"); }
static void unsupported_nvnSamplerGetBorderColori(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetBorderColori"); }
static void unsupported_nvnSamplerGetBorderColorui(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetBorderColorui"); }
static void unsupported_nvnSamplerGetMaxAnisotropy(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetMaxAnisotropy"); }
static void unsupported_nvnSamplerGetReductionFilter(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetReductionFilter"); }
static void unsupported_nvnSamplerCompare(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerCompare"); }
static void unsupported_nvnSamplerGetDebugID(void) { xbg_fatalf("NVN API called but not implemented: nvnSamplerGetDebugID"); }
static void unsupported_nvnBlendStateSetAdvancedMode(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateSetAdvancedMode"); }
static void unsupported_nvnBlendStateSetAdvancedOverlap(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateSetAdvancedOverlap"); }
static void unsupported_nvnBlendStateSetAdvancedPremultipliedSrc(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateSetAdvancedPremultipliedSrc"); }
static void unsupported_nvnBlendStateSetAdvancedNormalizedDst(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateSetAdvancedNormalizedDst"); }
static void unsupported_nvnBlendStateGetBlendTarget(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateGetBlendTarget"); }
static void unsupported_nvnBlendStateGetBlendFunc(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateGetBlendFunc"); }
static void unsupported_nvnBlendStateGetBlendEquation(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateGetBlendEquation"); }
static void unsupported_nvnBlendStateGetAdvancedMode(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateGetAdvancedMode"); }
static void unsupported_nvnBlendStateGetAdvancedOverlap(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateGetAdvancedOverlap"); }
static void unsupported_nvnBlendStateGetAdvancedPremultipliedSrc(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateGetAdvancedPremultipliedSrc"); }
static void unsupported_nvnBlendStateGetAdvancedNormalizedDst(void) { xbg_fatalf("NVN API called but not implemented: nvnBlendStateGetAdvancedNormalizedDst"); }
static void unsupported_nvnColorStateSetLogicOp(void) { xbg_fatalf("NVN API called but not implemented: nvnColorStateSetLogicOp"); }
static void unsupported_nvnColorStateSetAlphaTest(void) { xbg_fatalf("NVN API called but not implemented: nvnColorStateSetAlphaTest"); }
static void unsupported_nvnColorStateGetBlendEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnColorStateGetBlendEnable"); }
static void unsupported_nvnColorStateGetLogicOp(void) { xbg_fatalf("NVN API called but not implemented: nvnColorStateGetLogicOp"); }
static void unsupported_nvnColorStateGetAlphaTest(void) { xbg_fatalf("NVN API called but not implemented: nvnColorStateGetAlphaTest"); }
static void unsupported_nvnChannelMaskStateGetChannelMask(void) { xbg_fatalf("NVN API called but not implemented: nvnChannelMaskStateGetChannelMask"); }
static void unsupported_nvnMultisampleStateSetAlphaToCoverageDither(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateSetAlphaToCoverageDither"); }
static void unsupported_nvnMultisampleStateGetMultisampleEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetMultisampleEnable"); }
static void unsupported_nvnMultisampleStateGetSamples(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetSamples"); }
static void unsupported_nvnMultisampleStateGetAlphaToCoverageEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetAlphaToCoverageEnable"); }
static void unsupported_nvnMultisampleStateGetAlphaToCoverageDither(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetAlphaToCoverageDither"); }
static void unsupported_nvnMultisampleStateSetRasterSamples(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateSetRasterSamples"); }
static void unsupported_nvnMultisampleStateGetRasterSamples(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetRasterSamples"); }
static void unsupported_nvnMultisampleStateSetCoverageModulationMode(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateSetCoverageModulationMode"); }
static void unsupported_nvnMultisampleStateGetCoverageModulationMode(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetCoverageModulationMode"); }
static void unsupported_nvnMultisampleStateSetCoverageToColorEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateSetCoverageToColorEnable"); }
static void unsupported_nvnMultisampleStateGetCoverageToColorEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetCoverageToColorEnable"); }
static void unsupported_nvnMultisampleStateSetCoverageToColorOutput(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateSetCoverageToColorOutput"); }
static void unsupported_nvnMultisampleStateGetCoverageToColorOutput(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetCoverageToColorOutput"); }
static void unsupported_nvnMultisampleStateGetSampleLocationsEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetSampleLocationsEnable"); }
static void unsupported_nvnMultisampleStateGetSampleLocationsGrid(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetSampleLocationsGrid"); }
static void unsupported_nvnMultisampleStateSetSampleLocationsGridEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateSetSampleLocationsGridEnable"); }
static void unsupported_nvnMultisampleStateGetSampleLocationsGridEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnMultisampleStateGetSampleLocationsGridEnable"); }
static void unsupported_nvnPolygonStateGetCullFace(void) { xbg_fatalf("NVN API called but not implemented: nvnPolygonStateGetCullFace"); }
static void unsupported_nvnPolygonStateGetFrontFace(void) { xbg_fatalf("NVN API called but not implemented: nvnPolygonStateGetFrontFace"); }
static void unsupported_nvnPolygonStateGetPolygonMode(void) { xbg_fatalf("NVN API called but not implemented: nvnPolygonStateGetPolygonMode"); }
static void unsupported_nvnPolygonStateGetPolygonOffsetEnables(void) { xbg_fatalf("NVN API called but not implemented: nvnPolygonStateGetPolygonOffsetEnables"); }
static void unsupported_nvnDepthStencilStateGetDepthTestEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnDepthStencilStateGetDepthTestEnable"); }
static void unsupported_nvnDepthStencilStateGetDepthWriteEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnDepthStencilStateGetDepthWriteEnable"); }
static void unsupported_nvnDepthStencilStateGetDepthFunc(void) { xbg_fatalf("NVN API called but not implemented: nvnDepthStencilStateGetDepthFunc"); }
static void unsupported_nvnDepthStencilStateGetStencilTestEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnDepthStencilStateGetStencilTestEnable"); }
static void unsupported_nvnDepthStencilStateGetStencilFunc(void) { xbg_fatalf("NVN API called but not implemented: nvnDepthStencilStateGetStencilFunc"); }
static void unsupported_nvnDepthStencilStateGetStencilOp(void) { xbg_fatalf("NVN API called but not implemented: nvnDepthStencilStateGetStencilOp"); }
static void unsupported_nvnVertexAttribStateGetFormat(void) { xbg_fatalf("NVN API called but not implemented: nvnVertexAttribStateGetFormat"); }
static void unsupported_nvnVertexAttribStateGetStreamIndex(void) { xbg_fatalf("NVN API called but not implemented: nvnVertexAttribStateGetStreamIndex"); }
static void unsupported_nvnVertexStreamStateGetStride(void) { xbg_fatalf("NVN API called but not implemented: nvnVertexStreamStateGetStride"); }
static void unsupported_nvnVertexStreamStateGetDivisor(void) { xbg_fatalf("NVN API called but not implemented: nvnVertexStreamStateGetDivisor"); }
static void unsupported_nvnCommandBufferSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetDebugLabel"); }
static void unsupported_nvnCommandBufferSetCommandMemoryCallbackEnabled(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetCommandMemoryCallbackEnabled"); }
static void unsupported_nvnCommandBufferGetCommandMemorySize(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetCommandMemorySize"); }
static void unsupported_nvnCommandBufferGetCommandMemoryUsed(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetCommandMemoryUsed"); }
static void unsupported_nvnCommandBufferGetCommandMemoryFree(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetCommandMemoryFree"); }
static void unsupported_nvnCommandBufferGetControlMemorySize(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetControlMemorySize"); }
static void unsupported_nvnCommandBufferGetControlMemoryUsed(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetControlMemoryUsed"); }
static void unsupported_nvnCommandBufferGetControlMemoryFree(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetControlMemoryFree"); }
static void unsupported_nvnCommandBufferCallCommands(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferCallCommands"); }
static void unsupported_nvnCommandBufferBindTransformFeedbackBuffer(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferBindTransformFeedbackBuffer"); }
static void unsupported_nvnCommandBufferBindTransformFeedbackBuffers(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferBindTransformFeedbackBuffers"); }
static void unsupported_nvnCommandBufferBindTexture(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferBindTexture"); }
static void unsupported_nvnCommandBufferBindTextures(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferBindTextures"); }
static void unsupported_nvnCommandBufferBindImage(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferBindImage"); }
static void unsupported_nvnCommandBufferSetInnerTessellationLevels(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetInnerTessellationLevels"); }
static void unsupported_nvnCommandBufferSetOuterTessellationLevels(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetOuterTessellationLevels"); }
static void unsupported_nvnCommandBufferSetPrimitiveRestart(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetPrimitiveRestart"); }
static void unsupported_nvnCommandBufferBeginTransformFeedback(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferBeginTransformFeedback"); }
static void unsupported_nvnCommandBufferEndTransformFeedback(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferEndTransformFeedback"); }
static void unsupported_nvnCommandBufferPauseTransformFeedback(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferPauseTransformFeedback"); }
static void unsupported_nvnCommandBufferResumeTransformFeedback(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferResumeTransformFeedback"); }
static void unsupported_nvnCommandBufferDrawTransformFeedback(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferDrawTransformFeedback"); }
static void unsupported_nvnCommandBufferSetViewport(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetViewport"); }
static void unsupported_nvnCommandBufferSetViewportSwizzles(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetViewportSwizzles"); }
static void unsupported_nvnCommandBufferSetScissors(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetScissors"); }
static void unsupported_nvnCommandBufferSetDepthRanges(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetDepthRanges"); }
static void unsupported_nvnCommandBufferSetTiledCacheAction(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetTiledCacheAction"); }
static void unsupported_nvnCommandBufferSetTiledCacheTileSize(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetTiledCacheTileSize"); }
static void unsupported_nvnCommandBufferBindSeparateTexture(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferBindSeparateTexture"); }
static void unsupported_nvnCommandBufferSetPointSize(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetPointSize"); }
static void unsupported_nvnCommandBufferSetLineWidth(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetLineWidth"); }
static void unsupported_nvnCommandBufferSetAlphaRef(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetAlphaRef"); }
static void unsupported_nvnCommandBufferSetSampleMask(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetSampleMask"); }
static void unsupported_nvnCommandBufferSetRasterizerDiscard(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetRasterizerDiscard"); }
static void unsupported_nvnCommandBufferSetDepthClamp(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetDepthClamp"); }
static void unsupported_nvnCommandBufferSetConservativeRasterEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetConservativeRasterEnable"); }
static void unsupported_nvnCommandBufferSetConservativeRasterDilate(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetConservativeRasterDilate"); }
static void unsupported_nvnCommandBufferSetSubpixelPrecisionBias(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetSubpixelPrecisionBias"); }
static void unsupported_nvnCommandBufferClearBuffer(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferClearBuffer"); }
static void unsupported_nvnCommandBufferReportValue(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferReportValue"); }
static void unsupported_nvnCommandBufferSetRenderEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetRenderEnable"); }
static void unsupported_nvnCommandBufferSetRenderEnableConditional(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetRenderEnableConditional"); }
static void unsupported_nvnCommandBufferDiscardColor(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferDiscardColor"); }
static void unsupported_nvnCommandBufferDiscardDepthStencil(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferDiscardDepthStencil"); }
static void unsupported_nvnCommandBufferDownsample(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferDownsample"); }
static void unsupported_nvnCommandBufferTiledDownsample(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferTiledDownsample"); }
NVN(nvnCommandBufferDownsampleTextureView,void,NVNcommandBuffer *c,const NVNtexture *src,const NVNtextureView *sv,const NVNtexture *dst,const NVNtextureView *dv) {
 p_copy_t2t *p=(p_copy_t2t *)rec(c,OP_COUNT_+6,sizeof(p_copy_t2t));p->src=(const tex_t *)src;p->dst=(const tex_t *)dst;copy_view(&p->sv,&p->has_sv,sv);copy_view(&p->dv,&p->has_dv,dv);
}
static void unsupported_nvnCommandBufferTiledDownsampleTextureView(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferTiledDownsampleTextureView"); }
static void unsupported_nvnCommandBufferFenceSync(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferFenceSync"); }
static void unsupported_nvnCommandBufferSaveZCullData(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSaveZCullData"); }
static void unsupported_nvnCommandBufferRestoreZCullData(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferRestoreZCullData"); }
static void unsupported_nvnCommandBufferSetCopyImageStride(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetCopyImageStride"); }
static void unsupported_nvnCommandBufferGetCopyRowStride(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetCopyRowStride"); }
static void unsupported_nvnCommandBufferGetCopyImageStride(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetCopyImageStride"); }
static void unsupported_nvnCommandBufferDrawTexture(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferDrawTexture"); }
static void unsupported_nvnProgramSetSubroutineLinkage(void) { xbg_fatalf("NVN API called but not implemented: nvnProgramSetSubroutineLinkage"); }
static void unsupported_nvnCommandBufferSetProgramSubroutines(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetProgramSubroutines"); }
static void unsupported_nvnCommandBufferBindCoverageModulationTable(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferBindCoverageModulationTable"); }
static void unsupported_nvnCommandBufferResolveDepthBuffer(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferResolveDepthBuffer"); }
static void unsupported_nvnCommandBufferSetColorReductionEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetColorReductionEnable"); }
static void unsupported_nvnCommandBufferSetColorReductionThresholds(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetColorReductionThresholds"); }
static void unsupported_nvnCommandBufferPushDebugGroupStatic(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferPushDebugGroupStatic"); }
static void unsupported_nvnCommandBufferPushDebugGroup(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferPushDebugGroup"); }
static void unsupported_nvnCommandBufferPopDebugGroupId(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferPopDebugGroupId"); }
static void unsupported_nvnCommandBufferInsertDebugMarkerStatic(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferInsertDebugMarkerStatic"); }
static void unsupported_nvnCommandBufferInsertDebugMarker(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferInsertDebugMarker"); }
static void unsupported_nvnCommandBufferGetMemoryCallback(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetMemoryCallback"); }
static void unsupported_nvnCommandBufferGetMemoryCallbackData(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferGetMemoryCallbackData"); }
static void unsupported_nvnSyncSetDebugLabel(void) { xbg_fatalf("NVN API called but not implemented: nvnSyncSetDebugLabel"); }
static void unsupported_nvnSyncInitializeFromFencedGLSync(void) { xbg_fatalf("NVN API called but not implemented: nvnSyncInitializeFromFencedGLSync"); }
static void unsupported_nvnSyncCreateGLSync(void) { xbg_fatalf("NVN API called but not implemented: nvnSyncCreateGLSync"); }
static void unsupported_nvnEventBuilderGetStorage(void) { xbg_fatalf("NVN API called but not implemented: nvnEventBuilderGetStorage"); }
static void unsupported_nvnEventBuilderGetMemoryPool(void) { xbg_fatalf("NVN API called but not implemented: nvnEventBuilderGetMemoryPool"); }
static void unsupported_nvnEventBuilderGetMemoryOffset(void) { xbg_fatalf("NVN API called but not implemented: nvnEventBuilderGetMemoryOffset"); }
static void unsupported_nvnEventGetMemoryPool(void) { xbg_fatalf("NVN API called but not implemented: nvnEventGetMemoryPool"); }
static void unsupported_nvnEventGetMemoryOffset(void) { xbg_fatalf("NVN API called but not implemented: nvnEventGetMemoryOffset"); }
static void unsupported_nvnCommandBufferSetStencilCullCriteria(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferSetStencilCullCriteria"); }
static void unsupported_nvnCommandBufferInsertCheckpointStatic(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferInsertCheckpointStatic"); }
static void unsupported_nvnCommandBufferInsertCheckpointDynamic(void) { xbg_fatalf("NVN API called but not implemented: nvnCommandBufferInsertCheckpointDynamic"); }
static void unsupported_nvnDeviceSetAutomaticCheckpointsEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceSetAutomaticCheckpointsEnable"); }
static void unsupported_nvnDeviceSetAutomaticCheckpointsSamplingInterval(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceSetAutomaticCheckpointsSamplingInterval"); }
static void unsupported_nvnDeviceSetAutomaticCheckpointsByDebugGroup(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceSetAutomaticCheckpointsByDebugGroup"); }
static void unsupported_nvnDeviceSetCheckpointCallStacksEnable(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceSetCheckpointCallStacksEnable"); }
static void unsupported_nvnDeviceSetCheckpointMaxCallStacksDepth(void) { xbg_fatalf("NVN API called but not implemented: nvnDeviceSetCheckpointMaxCallStacksDepth"); }
void *xbg_nvn_get_proc_address(const char *name) {
 if (!name) { xbg_logf("NVN null proc name"); return NULL; }
 if (!strcmp(name,"nvnDeviceBuilderSetDefaults")) return (void *)h_nvnDeviceBuilderSetDefaults;
 if (!strcmp(name,"nvnDeviceBuilderSetFlags")) return (void *)h_nvnDeviceBuilderSetFlags;
 if (!strcmp(name,"nvnDeviceBuilderGetFlags")) return (void *)unsupported_nvnDeviceBuilderGetFlags;
 if (!strcmp(name,"nvnDeviceInitialize")) return (void *)h_nvnDeviceInitialize;
 if (!strcmp(name,"nvnDeviceFinalize")) return (void *)h_nvnDeviceFinalize;
 if (!strcmp(name,"nvnDeviceSetDebugLabel")) return (void *)unsupported_nvnDeviceSetDebugLabel;
 if (!strcmp(name,"nvnDeviceGetProcAddress")) return (void *)h_nvnDeviceGetProcAddress;
 if (!strcmp(name,"nvnDeviceGetInteger")) return (void *)h_nvnDeviceGetInteger;
 if (!strcmp(name,"nvnDeviceGetCurrentTimestampInNanoseconds")) return (void *)unsupported_nvnDeviceGetCurrentTimestampInNanoseconds;
 if (!strcmp(name,"nvnDeviceSetIntermediateShaderCache")) return (void *)unsupported_nvnDeviceSetIntermediateShaderCache;
 if (!strcmp(name,"nvnDeviceGetTextureHandle")) return (void *)h_nvnDeviceGetTextureHandle;
 if (!strcmp(name,"nvnDeviceGetTexelFetchHandle")) return (void *)unsupported_nvnDeviceGetTexelFetchHandle;
 if (!strcmp(name,"nvnDeviceGetImageHandle")) return (void *)h_nvnDeviceGetImageHandle;
 if (!strcmp(name,"nvnDeviceInstallDebugCallback")) return (void *)h_nvnDeviceInstallDebugCallback;
 if (!strcmp(name,"nvnDeviceGenerateDebugDomainId")) return (void *)unsupported_nvnDeviceGenerateDebugDomainId;
 if (!strcmp(name,"nvnDeviceSetWindowOriginMode")) return (void *)h_nvnDeviceSetWindowOriginMode;
 if (!strcmp(name,"nvnDeviceSetDepthMode")) return (void *)h_nvnDeviceSetDepthMode;
 if (!strcmp(name,"nvnDeviceRegisterFastClearColor")) return (void *)unsupported_nvnDeviceRegisterFastClearColor;
 if (!strcmp(name,"nvnDeviceRegisterFastClearColori")) return (void *)unsupported_nvnDeviceRegisterFastClearColori;
 if (!strcmp(name,"nvnDeviceRegisterFastClearColorui")) return (void *)unsupported_nvnDeviceRegisterFastClearColorui;
 if (!strcmp(name,"nvnDeviceRegisterFastClearDepth")) return (void *)unsupported_nvnDeviceRegisterFastClearDepth;
 if (!strcmp(name,"nvnDeviceGetWindowOriginMode")) return (void *)unsupported_nvnDeviceGetWindowOriginMode;
 if (!strcmp(name,"nvnDeviceGetDepthMode")) return (void *)unsupported_nvnDeviceGetDepthMode;
 if (!strcmp(name,"nvnDeviceGetTimestampInNanoseconds")) return (void *)unsupported_nvnDeviceGetTimestampInNanoseconds;
 if (!strcmp(name,"nvnDeviceApplyDeferredFinalizes")) return (void *)unsupported_nvnDeviceApplyDeferredFinalizes;
 if (!strcmp(name,"nvnDeviceFinalizeCommandHandle")) return (void *)h_nvnDeviceFinalizeCommandHandle;
 if (!strcmp(name,"nvnDeviceWalkDebugDatabase")) return (void *)unsupported_nvnDeviceWalkDebugDatabase;
 if (!strcmp(name,"nvnDeviceGetSeparateTextureHandle")) return (void *)h_nvnDeviceGetSeparateTextureHandle;
 if (!strcmp(name,"nvnDeviceGetSeparateSamplerHandle")) return (void *)h_nvnDeviceGetSeparateSamplerHandle;
 if (!strcmp(name,"nvnDeviceIsExternalDebuggerAttached")) return (void *)unsupported_nvnDeviceIsExternalDebuggerAttached;
 if (!strcmp(name,"nvnQueueGetError")) return (void *)h_nvnQueueGetError;
 if (!strcmp(name,"nvnQueueGetTotalCommandMemoryUsed")) return (void *)unsupported_nvnQueueGetTotalCommandMemoryUsed;
 if (!strcmp(name,"nvnQueueGetTotalControlMemoryUsed")) return (void *)unsupported_nvnQueueGetTotalControlMemoryUsed;
 if (!strcmp(name,"nvnQueueGetTotalComputeMemoryUsed")) return (void *)unsupported_nvnQueueGetTotalComputeMemoryUsed;
 if (!strcmp(name,"nvnQueueResetMemoryUsageCounts")) return (void *)unsupported_nvnQueueResetMemoryUsageCounts;
 if (!strcmp(name,"nvnQueueBuilderSetDevice")) return (void *)h_nvnQueueBuilderSetDevice;
 if (!strcmp(name,"nvnQueueBuilderSetDefaults")) return (void *)h_nvnQueueBuilderSetDefaults;
 if (!strcmp(name,"nvnQueueBuilderSetFlags")) return (void *)h_nvnQueueBuilderSetFlags;
 if (!strcmp(name,"nvnQueueBuilderSetCommandMemorySize")) return (void *)h_nvnQueueBuilderSetCommandMemorySize;
 if (!strcmp(name,"nvnQueueBuilderSetComputeMemorySize")) return (void *)h_nvnQueueBuilderSetComputeMemorySize;
 if (!strcmp(name,"nvnQueueBuilderSetControlMemorySize")) return (void *)h_nvnQueueBuilderSetControlMemorySize;
 if (!strcmp(name,"nvnQueueBuilderGetQueueMemorySize")) return (void *)h_nvnQueueBuilderGetQueueMemorySize;
 if (!strcmp(name,"nvnQueueBuilderSetQueueMemory")) return (void *)h_nvnQueueBuilderSetQueueMemory;
 if (!strcmp(name,"nvnQueueBuilderSetCommandFlushThreshold")) return (void *)h_nvnQueueBuilderSetCommandFlushThreshold;
 if (!strcmp(name,"nvnQueueBuilderSetQueuePriority")) return (void *)unsupported_nvnQueueBuilderSetQueuePriority;
 if (!strcmp(name,"nvnQueueBuilderGetQueuePriority")) return (void *)unsupported_nvnQueueBuilderGetQueuePriority;
 if (!strcmp(name,"nvnQueueBuilderGetDevice")) return (void *)unsupported_nvnQueueBuilderGetDevice;
 if (!strcmp(name,"nvnQueueBuilderGetFlags")) return (void *)unsupported_nvnQueueBuilderGetFlags;
 if (!strcmp(name,"nvnQueueBuilderGetCommandMemorySize")) return (void *)unsupported_nvnQueueBuilderGetCommandMemorySize;
 if (!strcmp(name,"nvnQueueBuilderGetComputeMemorySize")) return (void *)unsupported_nvnQueueBuilderGetComputeMemorySize;
 if (!strcmp(name,"nvnQueueBuilderGetControlMemorySize")) return (void *)unsupported_nvnQueueBuilderGetControlMemorySize;
 if (!strcmp(name,"nvnQueueBuilderGetCommandFlushThreshold")) return (void *)unsupported_nvnQueueBuilderGetCommandFlushThreshold;
 if (!strcmp(name,"nvnQueueBuilderGetMemorySize")) return (void *)unsupported_nvnQueueBuilderGetMemorySize;
 if (!strcmp(name,"nvnQueueBuilderGetMemory")) return (void *)unsupported_nvnQueueBuilderGetMemory;
 if (!strcmp(name,"nvnQueueInitialize")) return (void *)h_nvnQueueInitialize;
 if (!strcmp(name,"nvnQueueFinalize")) return (void *)h_nvnQueueFinalize;
 if (!strcmp(name,"nvnQueueSetDebugLabel")) return (void *)unsupported_nvnQueueSetDebugLabel;
 if (!strcmp(name,"nvnQueueSubmitCommands")) return (void *)h_nvnQueueSubmitCommands;
 if (!strcmp(name,"nvnQueueFlush")) return (void *)h_nvnQueueFlush;
 if (!strcmp(name,"nvnQueueFinish")) return (void *)h_nvnQueueFinish;
 if (!strcmp(name,"nvnQueuePresentTexture")) return (void *)h_nvnQueuePresentTexture;
 if (!strcmp(name,"nvnQueueAcquireTexture")) return (void *)unsupported_nvnQueueAcquireTexture;
 if (!strcmp(name,"nvnWindowBuilderSetDevice")) return (void *)h_nvnWindowBuilderSetDevice;
 if (!strcmp(name,"nvnWindowBuilderSetDefaults")) return (void *)h_nvnWindowBuilderSetDefaults;
 if (!strcmp(name,"nvnWindowBuilderSetNativeWindow")) return (void *)h_nvnWindowBuilderSetNativeWindow;
 if (!strcmp(name,"nvnWindowBuilderSetTextures")) return (void *)h_nvnWindowBuilderSetTextures;
 if (!strcmp(name,"nvnWindowBuilderSetPresentInterval")) return (void *)h_nvnWindowBuilderSetPresentInterval;
 if (!strcmp(name,"nvnWindowBuilderSetNumActiveTextures")) return (void *)unsupported_nvnWindowBuilderSetNumActiveTextures;
 if (!strcmp(name,"nvnWindowBuilderGetDevice")) return (void *)unsupported_nvnWindowBuilderGetDevice;
 if (!strcmp(name,"nvnWindowBuilderGetNumTextures")) return (void *)unsupported_nvnWindowBuilderGetNumTextures;
 if (!strcmp(name,"nvnWindowBuilderGetTexture")) return (void *)unsupported_nvnWindowBuilderGetTexture;
 if (!strcmp(name,"nvnWindowBuilderGetNativeWindow")) return (void *)unsupported_nvnWindowBuilderGetNativeWindow;
 if (!strcmp(name,"nvnWindowBuilderGetPresentInterval")) return (void *)unsupported_nvnWindowBuilderGetPresentInterval;
 if (!strcmp(name,"nvnWindowBuilderGetNumActiveTextures")) return (void *)unsupported_nvnWindowBuilderGetNumActiveTextures;
 if (!strcmp(name,"nvnWindowInitialize")) return (void *)h_nvnWindowInitialize;
 if (!strcmp(name,"nvnWindowFinalize")) return (void *)h_nvnWindowFinalize;
 if (!strcmp(name,"nvnWindowSetDebugLabel")) return (void *)unsupported_nvnWindowSetDebugLabel;
 if (!strcmp(name,"nvnWindowAcquireTexture")) return (void *)h_nvnWindowAcquireTexture;
 if (!strcmp(name,"nvnWindowGetNativeWindow")) return (void *)unsupported_nvnWindowGetNativeWindow;
 if (!strcmp(name,"nvnWindowGetPresentInterval")) return (void *)unsupported_nvnWindowGetPresentInterval;
 if (!strcmp(name,"nvnWindowSetPresentInterval")) return (void *)h_nvnWindowSetPresentInterval;
 if (!strcmp(name,"nvnWindowSetCrop")) return (void *)h_nvnWindowSetCrop;
 if (!strcmp(name,"nvnWindowGetCrop")) return (void *)unsupported_nvnWindowGetCrop;
 if (!strcmp(name,"nvnWindowSetNumActiveTextures")) return (void *)unsupported_nvnWindowSetNumActiveTextures;
 if (!strcmp(name,"nvnWindowGetNumActiveTextures")) return (void *)unsupported_nvnWindowGetNumActiveTextures;
 if (!strcmp(name,"nvnWindowGetNumTextures")) return (void *)unsupported_nvnWindowGetNumTextures;
 if (!strcmp(name,"nvnProgramInitialize")) return (void *)h_nvnProgramInitialize;
 if (!strcmp(name,"nvnProgramFinalize")) return (void *)h_nvnProgramFinalize;
 if (!strcmp(name,"nvnProgramSetDebugLabel")) return (void *)unsupported_nvnProgramSetDebugLabel;
 if (!strcmp(name,"nvnProgramSetShaders")) return (void *)h_nvnProgramSetShaders;
 if (!strcmp(name,"nvnProgramSetShadersExt")) return (void *)unsupported_nvnProgramSetShadersExt;
 if (!strcmp(name,"nvnProgramSetSampleShading")) return (void *)unsupported_nvnProgramSetSampleShading;
 if (!strcmp(name,"nvnMemoryPoolBuilderSetDevice")) return (void *)h_nvnMemoryPoolBuilderSetDevice;
 if (!strcmp(name,"nvnMemoryPoolBuilderSetDefaults")) return (void *)h_nvnMemoryPoolBuilderSetDefaults;
 if (!strcmp(name,"nvnMemoryPoolBuilderSetStorage")) return (void *)h_nvnMemoryPoolBuilderSetStorage;
 if (!strcmp(name,"nvnMemoryPoolBuilderSetFlags")) return (void *)h_nvnMemoryPoolBuilderSetFlags;
 if (!strcmp(name,"nvnMemoryPoolBuilderGetDevice")) return (void *)unsupported_nvnMemoryPoolBuilderGetDevice;
 if (!strcmp(name,"nvnMemoryPoolBuilderGetMemory")) return (void *)unsupported_nvnMemoryPoolBuilderGetMemory;
 if (!strcmp(name,"nvnMemoryPoolBuilderGetSize")) return (void *)unsupported_nvnMemoryPoolBuilderGetSize;
 if (!strcmp(name,"nvnMemoryPoolBuilderGetFlags")) return (void *)unsupported_nvnMemoryPoolBuilderGetFlags;
 if (!strcmp(name,"nvnMemoryPoolInitialize")) return (void *)h_nvnMemoryPoolInitialize;
 if (!strcmp(name,"nvnMemoryPoolSetDebugLabel")) return (void *)unsupported_nvnMemoryPoolSetDebugLabel;
 if (!strcmp(name,"nvnMemoryPoolFinalize")) return (void *)h_nvnMemoryPoolFinalize;
 if (!strcmp(name,"nvnMemoryPoolMap")) return (void *)unsupported_nvnMemoryPoolMap;
 if (!strcmp(name,"nvnMemoryPoolFlushMappedRange")) return (void *)h_nvnMemoryPoolFlushMappedRange;
 if (!strcmp(name,"nvnMemoryPoolInvalidateMappedRange")) return (void *)unsupported_nvnMemoryPoolInvalidateMappedRange;
 if (!strcmp(name,"nvnMemoryPoolGetBufferAddress")) return (void *)h_nvnMemoryPoolGetBufferAddress;
 if (!strcmp(name,"nvnMemoryPoolMapVirtual")) return (void *)unsupported_nvnMemoryPoolMapVirtual;
 if (!strcmp(name,"nvnMemoryPoolGetSize")) return (void *)unsupported_nvnMemoryPoolGetSize;
 if (!strcmp(name,"nvnMemoryPoolGetFlags")) return (void *)unsupported_nvnMemoryPoolGetFlags;
 if (!strcmp(name,"nvnTexturePoolInitialize")) return (void *)h_nvnTexturePoolInitialize;
 if (!strcmp(name,"nvnTexturePoolSetDebugLabel")) return (void *)unsupported_nvnTexturePoolSetDebugLabel;
 if (!strcmp(name,"nvnTexturePoolFinalize")) return (void *)h_nvnTexturePoolFinalize;
 if (!strcmp(name,"nvnTexturePoolRegisterTexture")) return (void *)h_nvnTexturePoolRegisterTexture;
 if (!strcmp(name,"nvnTexturePoolRegisterImage")) return (void *)unsupported_nvnTexturePoolRegisterImage;
 if (!strcmp(name,"nvnTexturePoolGetMemoryPool")) return (void *)unsupported_nvnTexturePoolGetMemoryPool;
 if (!strcmp(name,"nvnTexturePoolGetMemoryOffset")) return (void *)unsupported_nvnTexturePoolGetMemoryOffset;
 if (!strcmp(name,"nvnTexturePoolGetSize")) return (void *)unsupported_nvnTexturePoolGetSize;
 if (!strcmp(name,"nvnSamplerPoolInitialize")) return (void *)h_nvnSamplerPoolInitialize;
 if (!strcmp(name,"nvnSamplerPoolSetDebugLabel")) return (void *)unsupported_nvnSamplerPoolSetDebugLabel;
 if (!strcmp(name,"nvnSamplerPoolFinalize")) return (void *)h_nvnSamplerPoolFinalize;
 if (!strcmp(name,"nvnSamplerPoolRegisterSampler")) return (void *)h_nvnSamplerPoolRegisterSampler;
 if (!strcmp(name,"nvnSamplerPoolRegisterSamplerBuilder")) return (void *)unsupported_nvnSamplerPoolRegisterSamplerBuilder;
 if (!strcmp(name,"nvnSamplerPoolGetMemoryPool")) return (void *)unsupported_nvnSamplerPoolGetMemoryPool;
 if (!strcmp(name,"nvnSamplerPoolGetMemoryOffset")) return (void *)unsupported_nvnSamplerPoolGetMemoryOffset;
 if (!strcmp(name,"nvnSamplerPoolGetSize")) return (void *)unsupported_nvnSamplerPoolGetSize;
 if (!strcmp(name,"nvnBufferBuilderSetDevice")) return (void *)unsupported_nvnBufferBuilderSetDevice;
 if (!strcmp(name,"nvnBufferBuilderSetDefaults")) return (void *)unsupported_nvnBufferBuilderSetDefaults;
 if (!strcmp(name,"nvnBufferBuilderSetStorage")) return (void *)unsupported_nvnBufferBuilderSetStorage;
 if (!strcmp(name,"nvnBufferBuilderGetDevice")) return (void *)unsupported_nvnBufferBuilderGetDevice;
 if (!strcmp(name,"nvnBufferBuilderGetMemoryPool")) return (void *)unsupported_nvnBufferBuilderGetMemoryPool;
 if (!strcmp(name,"nvnBufferBuilderGetMemoryOffset")) return (void *)unsupported_nvnBufferBuilderGetMemoryOffset;
 if (!strcmp(name,"nvnBufferBuilderGetSize")) return (void *)unsupported_nvnBufferBuilderGetSize;
 if (!strcmp(name,"nvnBufferInitialize")) return (void *)unsupported_nvnBufferInitialize;
 if (!strcmp(name,"nvnBufferSetDebugLabel")) return (void *)unsupported_nvnBufferSetDebugLabel;
 if (!strcmp(name,"nvnBufferFinalize")) return (void *)unsupported_nvnBufferFinalize;
 if (!strcmp(name,"nvnBufferMap")) return (void *)unsupported_nvnBufferMap;
 if (!strcmp(name,"nvnBufferGetAddress")) return (void *)unsupported_nvnBufferGetAddress;
 if (!strcmp(name,"nvnBufferFlushMappedRange")) return (void *)unsupported_nvnBufferFlushMappedRange;
 if (!strcmp(name,"nvnBufferInvalidateMappedRange")) return (void *)unsupported_nvnBufferInvalidateMappedRange;
 if (!strcmp(name,"nvnBufferGetMemoryPool")) return (void *)unsupported_nvnBufferGetMemoryPool;
 if (!strcmp(name,"nvnBufferGetMemoryOffset")) return (void *)unsupported_nvnBufferGetMemoryOffset;
 if (!strcmp(name,"nvnBufferGetSize")) return (void *)unsupported_nvnBufferGetSize;
 if (!strcmp(name,"nvnBufferGetDebugID")) return (void *)unsupported_nvnBufferGetDebugID;
 if (!strcmp(name,"nvnTextureBuilderSetDevice")) return (void *)h_nvnTextureBuilderSetDevice;
 if (!strcmp(name,"nvnTextureBuilderSetDefaults")) return (void *)h_nvnTextureBuilderSetDefaults;
 if (!strcmp(name,"nvnTextureBuilderSetFlags")) return (void *)h_nvnTextureBuilderSetFlags;
 if (!strcmp(name,"nvnTextureBuilderSetTarget")) return (void *)h_nvnTextureBuilderSetTarget;
 if (!strcmp(name,"nvnTextureBuilderSetWidth")) return (void *)h_nvnTextureBuilderSetWidth;
 if (!strcmp(name,"nvnTextureBuilderSetHeight")) return (void *)h_nvnTextureBuilderSetHeight;
 if (!strcmp(name,"nvnTextureBuilderSetDepth")) return (void *)h_nvnTextureBuilderSetDepth;
 if (!strcmp(name,"nvnTextureBuilderSetSize1D")) return (void *)h_nvnTextureBuilderSetSize1D;
 if (!strcmp(name,"nvnTextureBuilderSetSize2D")) return (void *)unsupported_nvnTextureBuilderSetSize2D;
 if (!strcmp(name,"nvnTextureBuilderSetSize3D")) return (void *)unsupported_nvnTextureBuilderSetSize3D;
 if (!strcmp(name,"nvnTextureBuilderSetLevels")) return (void *)h_nvnTextureBuilderSetLevels;
 if (!strcmp(name,"nvnTextureBuilderSetFormat")) return (void *)h_nvnTextureBuilderSetFormat;
 if (!strcmp(name,"nvnTextureBuilderSetSamples")) return (void *)h_nvnTextureBuilderSetSamples;
 if (!strcmp(name,"nvnTextureBuilderSetSwizzle")) return (void *)unsupported_nvnTextureBuilderSetSwizzle;
 if (!strcmp(name,"nvnTextureBuilderSetDepthStencilMode")) return (void *)unsupported_nvnTextureBuilderSetDepthStencilMode;
 if (!strcmp(name,"nvnTextureBuilderGetStorageSize")) return (void *)h_nvnTextureBuilderGetStorageSize;
 if (!strcmp(name,"nvnTextureBuilderGetStorageAlignment")) return (void *)h_nvnTextureBuilderGetStorageAlignment;
 if (!strcmp(name,"nvnTextureBuilderSetStorage")) return (void *)h_nvnTextureBuilderSetStorage;
 if (!strcmp(name,"nvnTextureBuilderSetPackagedTextureData")) return (void *)unsupported_nvnTextureBuilderSetPackagedTextureData;
 if (!strcmp(name,"nvnTextureBuilderSetPackagedTextureLayout")) return (void *)h_nvnTextureBuilderSetPackagedTextureLayout;
 if (!strcmp(name,"nvnTextureBuilderSetStride")) return (void *)h_nvnTextureBuilderSetStride;
 if (!strcmp(name,"nvnTextureBuilderSetGLTextureName")) return (void *)unsupported_nvnTextureBuilderSetGLTextureName;
 if (!strcmp(name,"nvnTextureBuilderGetStorageClass")) return (void *)unsupported_nvnTextureBuilderGetStorageClass;
 if (!strcmp(name,"nvnTextureBuilderGetDevice")) return (void *)unsupported_nvnTextureBuilderGetDevice;
 if (!strcmp(name,"nvnTextureBuilderGetFlags")) return (void *)h_nvnTextureBuilderGetFlags;
 if (!strcmp(name,"nvnTextureBuilderGetTarget")) return (void *)unsupported_nvnTextureBuilderGetTarget;
 if (!strcmp(name,"nvnTextureBuilderGetWidth")) return (void *)unsupported_nvnTextureBuilderGetWidth;
 if (!strcmp(name,"nvnTextureBuilderGetHeight")) return (void *)unsupported_nvnTextureBuilderGetHeight;
 if (!strcmp(name,"nvnTextureBuilderGetDepth")) return (void *)unsupported_nvnTextureBuilderGetDepth;
 if (!strcmp(name,"nvnTextureBuilderGetLevels")) return (void *)unsupported_nvnTextureBuilderGetLevels;
 if (!strcmp(name,"nvnTextureBuilderGetFormat")) return (void *)unsupported_nvnTextureBuilderGetFormat;
 if (!strcmp(name,"nvnTextureBuilderGetSamples")) return (void *)unsupported_nvnTextureBuilderGetSamples;
 if (!strcmp(name,"nvnTextureBuilderGetSwizzle")) return (void *)unsupported_nvnTextureBuilderGetSwizzle;
 if (!strcmp(name,"nvnTextureBuilderGetDepthStencilMode")) return (void *)unsupported_nvnTextureBuilderGetDepthStencilMode;
 if (!strcmp(name,"nvnTextureBuilderGetPackagedTextureData")) return (void *)unsupported_nvnTextureBuilderGetPackagedTextureData;
 if (!strcmp(name,"nvnTextureBuilderGetPackagedTextureLayout")) return (void *)unsupported_nvnTextureBuilderGetPackagedTextureLayout;
 if (!strcmp(name,"nvnTextureBuilderGetStride")) return (void *)unsupported_nvnTextureBuilderGetStride;
 if (!strcmp(name,"nvnTextureBuilderGetSparseTileLayout")) return (void *)unsupported_nvnTextureBuilderGetSparseTileLayout;
 if (!strcmp(name,"nvnTextureBuilderGetGLTextureName")) return (void *)unsupported_nvnTextureBuilderGetGLTextureName;
 if (!strcmp(name,"nvnTextureBuilderGetZCullStorageSize")) return (void *)unsupported_nvnTextureBuilderGetZCullStorageSize;
 if (!strcmp(name,"nvnTextureBuilderGetMemoryPool")) return (void *)unsupported_nvnTextureBuilderGetMemoryPool;
 if (!strcmp(name,"nvnTextureBuilderGetMemoryOffset")) return (void *)unsupported_nvnTextureBuilderGetMemoryOffset;
 if (!strcmp(name,"nvnTextureBuilderGetRawStorageClass")) return (void *)unsupported_nvnTextureBuilderGetRawStorageClass;
 if (!strcmp(name,"nvnTextureViewSetDefaults")) return (void *)h_nvnTextureViewSetDefaults;
 if (!strcmp(name,"nvnTextureViewSetLevels")) return (void *)h_nvnTextureViewSetLevels;
 if (!strcmp(name,"nvnTextureViewSetLayers")) return (void *)h_nvnTextureViewSetLayers;
 if (!strcmp(name,"nvnTextureViewSetFormat")) return (void *)h_nvnTextureViewSetFormat;
 if (!strcmp(name,"nvnTextureViewSetSwizzle")) return (void *)unsupported_nvnTextureViewSetSwizzle;
 if (!strcmp(name,"nvnTextureViewSetDepthStencilMode")) return (void *)unsupported_nvnTextureViewSetDepthStencilMode;
 if (!strcmp(name,"nvnTextureViewSetTarget")) return (void *)h_nvnTextureViewSetTarget;
 if (!strcmp(name,"nvnTextureViewGetLevels")) return (void *)unsupported_nvnTextureViewGetLevels;
 if (!strcmp(name,"nvnTextureViewGetLayers")) return (void *)h_nvnTextureViewGetLayers;
 if (!strcmp(name,"nvnTextureViewGetFormat")) return (void *)unsupported_nvnTextureViewGetFormat;
 if (!strcmp(name,"nvnTextureViewGetSwizzle")) return (void *)unsupported_nvnTextureViewGetSwizzle;
 if (!strcmp(name,"nvnTextureViewGetDepthStencilMode")) return (void *)unsupported_nvnTextureViewGetDepthStencilMode;
 if (!strcmp(name,"nvnTextureViewGetTarget")) return (void *)unsupported_nvnTextureViewGetTarget;
 if (!strcmp(name,"nvnTextureViewCompare")) return (void *)unsupported_nvnTextureViewCompare;
 if (!strcmp(name,"nvnTextureInitialize")) return (void *)h_nvnTextureInitialize;
 if (!strcmp(name,"nvnTextureGetZCullStorageSize")) return (void *)unsupported_nvnTextureGetZCullStorageSize;
 if (!strcmp(name,"nvnTextureFinalize")) return (void *)h_nvnTextureFinalize;
 if (!strcmp(name,"nvnTextureSetDebugLabel")) return (void *)unsupported_nvnTextureSetDebugLabel;
 if (!strcmp(name,"nvnTextureGetStorageClass")) return (void *)unsupported_nvnTextureGetStorageClass;
 if (!strcmp(name,"nvnTextureGetViewOffset")) return (void *)h_nvnTextureGetViewOffset;
 if (!strcmp(name,"nvnTextureGetFlags")) return (void *)h_nvnTextureGetFlags;
 if (!strcmp(name,"nvnTextureGetTarget")) return (void *)unsupported_nvnTextureGetTarget;
 if (!strcmp(name,"nvnTextureGetWidth")) return (void *)h_nvnTextureGetWidth;
 if (!strcmp(name,"nvnTextureGetHeight")) return (void *)h_nvnTextureGetHeight;
 if (!strcmp(name,"nvnTextureGetDepth")) return (void *)h_nvnTextureGetDepth;
 if (!strcmp(name,"nvnTextureGetLevels")) return (void *)unsupported_nvnTextureGetLevels;
 if (!strcmp(name,"nvnTextureGetFormat")) return (void *)unsupported_nvnTextureGetFormat;
 if (!strcmp(name,"nvnTextureGetSamples")) return (void *)unsupported_nvnTextureGetSamples;
 if (!strcmp(name,"nvnTextureGetSwizzle")) return (void *)unsupported_nvnTextureGetSwizzle;
 if (!strcmp(name,"nvnTextureGetDepthStencilMode")) return (void *)unsupported_nvnTextureGetDepthStencilMode;
 if (!strcmp(name,"nvnTextureGetStride")) return (void *)unsupported_nvnTextureGetStride;
 if (!strcmp(name,"nvnTextureGetTextureAddress")) return (void *)unsupported_nvnTextureGetTextureAddress;
 if (!strcmp(name,"nvnTextureGetSparseTileLayout")) return (void *)unsupported_nvnTextureGetSparseTileLayout;
 if (!strcmp(name,"nvnTextureWriteTexels")) return (void *)unsupported_nvnTextureWriteTexels;
 if (!strcmp(name,"nvnTextureWriteTexelsStrided")) return (void *)unsupported_nvnTextureWriteTexelsStrided;
 if (!strcmp(name,"nvnTextureReadTexels")) return (void *)h_nvnTextureReadTexels;
 if (!strcmp(name,"nvnTextureReadTexelsStrided")) return (void *)h_nvnTextureReadTexelsStrided;
 if (!strcmp(name,"nvnTextureFlushTexels")) return (void *)unsupported_nvnTextureFlushTexels;
 if (!strcmp(name,"nvnTextureInvalidateTexels")) return (void *)h_nvnTextureInvalidateTexels;
 if (!strcmp(name,"nvnTextureGetMemoryPool")) return (void *)unsupported_nvnTextureGetMemoryPool;
 if (!strcmp(name,"nvnTextureGetMemoryOffset")) return (void *)unsupported_nvnTextureGetMemoryOffset;
 if (!strcmp(name,"nvnTextureGetStorageSize")) return (void *)h_nvnTextureGetStorageSize;
 if (!strcmp(name,"nvnTextureCompare")) return (void *)h_nvnTextureCompare;
 if (!strcmp(name,"nvnTextureGetDebugID")) return (void *)unsupported_nvnTextureGetDebugID;
 if (!strcmp(name,"nvnTextureGetRawStorageClass")) return (void *)h_nvnTextureGetRawStorageClass;
 if (!strcmp(name,"nvnSamplerBuilderSetDevice")) return (void *)h_nvnSamplerBuilderSetDevice;
 if (!strcmp(name,"nvnSamplerBuilderSetDefaults")) return (void *)h_nvnSamplerBuilderSetDefaults;
 if (!strcmp(name,"nvnSamplerBuilderSetMinMagFilter")) return (void *)h_nvnSamplerBuilderSetMinMagFilter;
 if (!strcmp(name,"nvnSamplerBuilderSetWrapMode")) return (void *)h_nvnSamplerBuilderSetWrapMode;
 if (!strcmp(name,"nvnSamplerBuilderSetLodClamp")) return (void *)h_nvnSamplerBuilderSetLodClamp;
 if (!strcmp(name,"nvnSamplerBuilderSetLodBias")) return (void *)h_nvnSamplerBuilderSetLodBias;
 if (!strcmp(name,"nvnSamplerBuilderSetCompare")) return (void *)h_nvnSamplerBuilderSetCompare;
 if (!strcmp(name,"nvnSamplerBuilderSetBorderColor")) return (void *)h_nvnSamplerBuilderSetBorderColor;
 if (!strcmp(name,"nvnSamplerBuilderSetBorderColori")) return (void *)unsupported_nvnSamplerBuilderSetBorderColori;
 if (!strcmp(name,"nvnSamplerBuilderSetBorderColorui")) return (void *)unsupported_nvnSamplerBuilderSetBorderColorui;
 if (!strcmp(name,"nvnSamplerBuilderSetMaxAnisotropy")) return (void *)h_nvnSamplerBuilderSetMaxAnisotropy;
 if (!strcmp(name,"nvnSamplerBuilderSetReductionFilter")) return (void *)unsupported_nvnSamplerBuilderSetReductionFilter;
 if (!strcmp(name,"nvnSamplerBuilderSetLodSnap")) return (void *)h_nvnSamplerBuilderSetLodSnap;
 if (!strcmp(name,"nvnSamplerBuilderGetDevice")) return (void *)unsupported_nvnSamplerBuilderGetDevice;
 if (!strcmp(name,"nvnSamplerBuilderGetMinMagFilter")) return (void *)h_nvnSamplerBuilderGetMinMagFilter;
 if (!strcmp(name,"nvnSamplerBuilderGetWrapMode")) return (void *)h_nvnSamplerBuilderGetWrapMode;
 if (!strcmp(name,"nvnSamplerBuilderGetLodClamp")) return (void *)unsupported_nvnSamplerBuilderGetLodClamp;
 if (!strcmp(name,"nvnSamplerBuilderGetLodBias")) return (void *)unsupported_nvnSamplerBuilderGetLodBias;
 if (!strcmp(name,"nvnSamplerBuilderGetCompare")) return (void *)unsupported_nvnSamplerBuilderGetCompare;
 if (!strcmp(name,"nvnSamplerBuilderGetBorderColor")) return (void *)unsupported_nvnSamplerBuilderGetBorderColor;
 if (!strcmp(name,"nvnSamplerBuilderGetBorderColori")) return (void *)unsupported_nvnSamplerBuilderGetBorderColori;
 if (!strcmp(name,"nvnSamplerBuilderGetBorderColorui")) return (void *)unsupported_nvnSamplerBuilderGetBorderColorui;
 if (!strcmp(name,"nvnSamplerBuilderGetMaxAnisotropy")) return (void *)unsupported_nvnSamplerBuilderGetMaxAnisotropy;
 if (!strcmp(name,"nvnSamplerBuilderGetReductionFilter")) return (void *)unsupported_nvnSamplerBuilderGetReductionFilter;
 if (!strcmp(name,"nvnSamplerBuilderGetLodSnap")) return (void *)unsupported_nvnSamplerBuilderGetLodSnap;
 if (!strcmp(name,"nvnSamplerInitialize")) return (void *)h_nvnSamplerInitialize;
 if (!strcmp(name,"nvnSamplerFinalize")) return (void *)h_nvnSamplerFinalize;
 if (!strcmp(name,"nvnSamplerSetDebugLabel")) return (void *)unsupported_nvnSamplerSetDebugLabel;
 if (!strcmp(name,"nvnSamplerGetMinMagFilter")) return (void *)unsupported_nvnSamplerGetMinMagFilter;
 if (!strcmp(name,"nvnSamplerGetWrapMode")) return (void *)unsupported_nvnSamplerGetWrapMode;
 if (!strcmp(name,"nvnSamplerGetLodClamp")) return (void *)unsupported_nvnSamplerGetLodClamp;
 if (!strcmp(name,"nvnSamplerGetLodBias")) return (void *)unsupported_nvnSamplerGetLodBias;
 if (!strcmp(name,"nvnSamplerGetCompare")) return (void *)unsupported_nvnSamplerGetCompare;
 if (!strcmp(name,"nvnSamplerGetBorderColor")) return (void *)unsupported_nvnSamplerGetBorderColor;
 if (!strcmp(name,"nvnSamplerGetBorderColori")) return (void *)unsupported_nvnSamplerGetBorderColori;
 if (!strcmp(name,"nvnSamplerGetBorderColorui")) return (void *)unsupported_nvnSamplerGetBorderColorui;
 if (!strcmp(name,"nvnSamplerGetMaxAnisotropy")) return (void *)unsupported_nvnSamplerGetMaxAnisotropy;
 if (!strcmp(name,"nvnSamplerGetReductionFilter")) return (void *)unsupported_nvnSamplerGetReductionFilter;
 if (!strcmp(name,"nvnSamplerCompare")) return (void *)unsupported_nvnSamplerCompare;
 if (!strcmp(name,"nvnSamplerGetDebugID")) return (void *)unsupported_nvnSamplerGetDebugID;
 if (!strcmp(name,"nvnBlendStateSetDefaults")) return (void *)h_nvnBlendStateSetDefaults;
 if (!strcmp(name,"nvnBlendStateSetBlendTarget")) return (void *)h_nvnBlendStateSetBlendTarget;
 if (!strcmp(name,"nvnBlendStateSetBlendFunc")) return (void *)h_nvnBlendStateSetBlendFunc;
 if (!strcmp(name,"nvnBlendStateSetBlendEquation")) return (void *)h_nvnBlendStateSetBlendEquation;
 if (!strcmp(name,"nvnBlendStateSetAdvancedMode")) return (void *)unsupported_nvnBlendStateSetAdvancedMode;
 if (!strcmp(name,"nvnBlendStateSetAdvancedOverlap")) return (void *)unsupported_nvnBlendStateSetAdvancedOverlap;
 if (!strcmp(name,"nvnBlendStateSetAdvancedPremultipliedSrc")) return (void *)unsupported_nvnBlendStateSetAdvancedPremultipliedSrc;
 if (!strcmp(name,"nvnBlendStateSetAdvancedNormalizedDst")) return (void *)unsupported_nvnBlendStateSetAdvancedNormalizedDst;
 if (!strcmp(name,"nvnBlendStateGetBlendTarget")) return (void *)unsupported_nvnBlendStateGetBlendTarget;
 if (!strcmp(name,"nvnBlendStateGetBlendFunc")) return (void *)unsupported_nvnBlendStateGetBlendFunc;
 if (!strcmp(name,"nvnBlendStateGetBlendEquation")) return (void *)unsupported_nvnBlendStateGetBlendEquation;
 if (!strcmp(name,"nvnBlendStateGetAdvancedMode")) return (void *)unsupported_nvnBlendStateGetAdvancedMode;
 if (!strcmp(name,"nvnBlendStateGetAdvancedOverlap")) return (void *)unsupported_nvnBlendStateGetAdvancedOverlap;
 if (!strcmp(name,"nvnBlendStateGetAdvancedPremultipliedSrc")) return (void *)unsupported_nvnBlendStateGetAdvancedPremultipliedSrc;
 if (!strcmp(name,"nvnBlendStateGetAdvancedNormalizedDst")) return (void *)unsupported_nvnBlendStateGetAdvancedNormalizedDst;
 if (!strcmp(name,"nvnColorStateSetDefaults")) return (void *)h_nvnColorStateSetDefaults;
 if (!strcmp(name,"nvnColorStateSetBlendEnable")) return (void *)h_nvnColorStateSetBlendEnable;
 if (!strcmp(name,"nvnColorStateSetLogicOp")) return (void *)unsupported_nvnColorStateSetLogicOp;
 if (!strcmp(name,"nvnColorStateSetAlphaTest")) return (void *)unsupported_nvnColorStateSetAlphaTest;
 if (!strcmp(name,"nvnColorStateGetBlendEnable")) return (void *)unsupported_nvnColorStateGetBlendEnable;
 if (!strcmp(name,"nvnColorStateGetLogicOp")) return (void *)unsupported_nvnColorStateGetLogicOp;
 if (!strcmp(name,"nvnColorStateGetAlphaTest")) return (void *)unsupported_nvnColorStateGetAlphaTest;
 if (!strcmp(name,"nvnChannelMaskStateSetDefaults")) return (void *)h_nvnChannelMaskStateSetDefaults;
 if (!strcmp(name,"nvnChannelMaskStateSetChannelMask")) return (void *)h_nvnChannelMaskStateSetChannelMask;
 if (!strcmp(name,"nvnChannelMaskStateGetChannelMask")) return (void *)unsupported_nvnChannelMaskStateGetChannelMask;
 if (!strcmp(name,"nvnMultisampleStateSetDefaults")) return (void *)h_nvnMultisampleStateSetDefaults;
 if (!strcmp(name,"nvnMultisampleStateSetMultisampleEnable")) return (void *)h_nvnMultisampleStateSetMultisampleEnable;
 if (!strcmp(name,"nvnMultisampleStateSetSamples")) return (void *)h_nvnMultisampleStateSetSamples;
 if (!strcmp(name,"nvnMultisampleStateSetAlphaToCoverageEnable")) return (void *)h_nvnMultisampleStateSetAlphaToCoverageEnable;
 if (!strcmp(name,"nvnMultisampleStateSetAlphaToCoverageDither")) return (void *)unsupported_nvnMultisampleStateSetAlphaToCoverageDither;
 if (!strcmp(name,"nvnMultisampleStateGetMultisampleEnable")) return (void *)unsupported_nvnMultisampleStateGetMultisampleEnable;
 if (!strcmp(name,"nvnMultisampleStateGetSamples")) return (void *)unsupported_nvnMultisampleStateGetSamples;
 if (!strcmp(name,"nvnMultisampleStateGetAlphaToCoverageEnable")) return (void *)unsupported_nvnMultisampleStateGetAlphaToCoverageEnable;
 if (!strcmp(name,"nvnMultisampleStateGetAlphaToCoverageDither")) return (void *)unsupported_nvnMultisampleStateGetAlphaToCoverageDither;
 if (!strcmp(name,"nvnMultisampleStateSetRasterSamples")) return (void *)unsupported_nvnMultisampleStateSetRasterSamples;
 if (!strcmp(name,"nvnMultisampleStateGetRasterSamples")) return (void *)unsupported_nvnMultisampleStateGetRasterSamples;
 if (!strcmp(name,"nvnMultisampleStateSetCoverageModulationMode")) return (void *)unsupported_nvnMultisampleStateSetCoverageModulationMode;
 if (!strcmp(name,"nvnMultisampleStateGetCoverageModulationMode")) return (void *)unsupported_nvnMultisampleStateGetCoverageModulationMode;
 if (!strcmp(name,"nvnMultisampleStateSetCoverageToColorEnable")) return (void *)unsupported_nvnMultisampleStateSetCoverageToColorEnable;
 if (!strcmp(name,"nvnMultisampleStateGetCoverageToColorEnable")) return (void *)unsupported_nvnMultisampleStateGetCoverageToColorEnable;
 if (!strcmp(name,"nvnMultisampleStateSetCoverageToColorOutput")) return (void *)unsupported_nvnMultisampleStateSetCoverageToColorOutput;
 if (!strcmp(name,"nvnMultisampleStateGetCoverageToColorOutput")) return (void *)unsupported_nvnMultisampleStateGetCoverageToColorOutput;
 if (!strcmp(name,"nvnMultisampleStateSetSampleLocationsEnable")) return (void *)h_nvnMultisampleStateSetSampleLocationsEnable;
 if (!strcmp(name,"nvnMultisampleStateGetSampleLocationsEnable")) return (void *)unsupported_nvnMultisampleStateGetSampleLocationsEnable;
 if (!strcmp(name,"nvnMultisampleStateGetSampleLocationsGrid")) return (void *)unsupported_nvnMultisampleStateGetSampleLocationsGrid;
 if (!strcmp(name,"nvnMultisampleStateSetSampleLocationsGridEnable")) return (void *)unsupported_nvnMultisampleStateSetSampleLocationsGridEnable;
 if (!strcmp(name,"nvnMultisampleStateGetSampleLocationsGridEnable")) return (void *)unsupported_nvnMultisampleStateGetSampleLocationsGridEnable;
 if (!strcmp(name,"nvnMultisampleStateSetSampleLocations")) return (void *)h_nvnMultisampleStateSetSampleLocations;
 if (!strcmp(name,"nvnPolygonStateSetDefaults")) return (void *)h_nvnPolygonStateSetDefaults;
 if (!strcmp(name,"nvnPolygonStateSetCullFace")) return (void *)h_nvnPolygonStateSetCullFace;
 if (!strcmp(name,"nvnPolygonStateSetFrontFace")) return (void *)h_nvnPolygonStateSetFrontFace;
 if (!strcmp(name,"nvnPolygonStateSetPolygonMode")) return (void *)h_nvnPolygonStateSetPolygonMode;
 if (!strcmp(name,"nvnPolygonStateSetPolygonOffsetEnables")) return (void *)h_nvnPolygonStateSetPolygonOffsetEnables;
 if (!strcmp(name,"nvnPolygonStateGetCullFace")) return (void *)unsupported_nvnPolygonStateGetCullFace;
 if (!strcmp(name,"nvnPolygonStateGetFrontFace")) return (void *)unsupported_nvnPolygonStateGetFrontFace;
 if (!strcmp(name,"nvnPolygonStateGetPolygonMode")) return (void *)unsupported_nvnPolygonStateGetPolygonMode;
 if (!strcmp(name,"nvnPolygonStateGetPolygonOffsetEnables")) return (void *)unsupported_nvnPolygonStateGetPolygonOffsetEnables;
 if (!strcmp(name,"nvnDepthStencilStateSetDefaults")) return (void *)h_nvnDepthStencilStateSetDefaults;
 if (!strcmp(name,"nvnDepthStencilStateSetDepthTestEnable")) return (void *)h_nvnDepthStencilStateSetDepthTestEnable;
 if (!strcmp(name,"nvnDepthStencilStateSetDepthWriteEnable")) return (void *)h_nvnDepthStencilStateSetDepthWriteEnable;
 if (!strcmp(name,"nvnDepthStencilStateSetDepthFunc")) return (void *)h_nvnDepthStencilStateSetDepthFunc;
 if (!strcmp(name,"nvnDepthStencilStateSetStencilTestEnable")) return (void *)h_nvnDepthStencilStateSetStencilTestEnable;
 if (!strcmp(name,"nvnDepthStencilStateSetStencilFunc")) return (void *)h_nvnDepthStencilStateSetStencilFunc;
 if (!strcmp(name,"nvnDepthStencilStateSetStencilOp")) return (void *)h_nvnDepthStencilStateSetStencilOp;
 if (!strcmp(name,"nvnDepthStencilStateGetDepthTestEnable")) return (void *)unsupported_nvnDepthStencilStateGetDepthTestEnable;
 if (!strcmp(name,"nvnDepthStencilStateGetDepthWriteEnable")) return (void *)unsupported_nvnDepthStencilStateGetDepthWriteEnable;
 if (!strcmp(name,"nvnDepthStencilStateGetDepthFunc")) return (void *)unsupported_nvnDepthStencilStateGetDepthFunc;
 if (!strcmp(name,"nvnDepthStencilStateGetStencilTestEnable")) return (void *)unsupported_nvnDepthStencilStateGetStencilTestEnable;
 if (!strcmp(name,"nvnDepthStencilStateGetStencilFunc")) return (void *)unsupported_nvnDepthStencilStateGetStencilFunc;
 if (!strcmp(name,"nvnDepthStencilStateGetStencilOp")) return (void *)unsupported_nvnDepthStencilStateGetStencilOp;
 if (!strcmp(name,"nvnVertexAttribStateSetDefaults")) return (void *)h_nvnVertexAttribStateSetDefaults;
 if (!strcmp(name,"nvnVertexAttribStateSetFormat")) return (void *)h_nvnVertexAttribStateSetFormat;
 if (!strcmp(name,"nvnVertexAttribStateSetStreamIndex")) return (void *)h_nvnVertexAttribStateSetStreamIndex;
 if (!strcmp(name,"nvnVertexAttribStateGetFormat")) return (void *)unsupported_nvnVertexAttribStateGetFormat;
 if (!strcmp(name,"nvnVertexAttribStateGetStreamIndex")) return (void *)unsupported_nvnVertexAttribStateGetStreamIndex;
 if (!strcmp(name,"nvnVertexStreamStateSetDefaults")) return (void *)h_nvnVertexStreamStateSetDefaults;
 if (!strcmp(name,"nvnVertexStreamStateSetStride")) return (void *)h_nvnVertexStreamStateSetStride;
 if (!strcmp(name,"nvnVertexStreamStateSetDivisor")) return (void *)h_nvnVertexStreamStateSetDivisor;
 if (!strcmp(name,"nvnVertexStreamStateGetStride")) return (void *)unsupported_nvnVertexStreamStateGetStride;
 if (!strcmp(name,"nvnVertexStreamStateGetDivisor")) return (void *)unsupported_nvnVertexStreamStateGetDivisor;
 if (!strcmp(name,"nvnCommandBufferInitialize")) return (void *)h_nvnCommandBufferInitialize;
 if (!strcmp(name,"nvnCommandBufferFinalize")) return (void *)h_nvnCommandBufferFinalize;
 if (!strcmp(name,"nvnCommandBufferSetDebugLabel")) return (void *)unsupported_nvnCommandBufferSetDebugLabel;
 if (!strcmp(name,"nvnCommandBufferSetMemoryCallback")) return (void *)h_nvnCommandBufferSetMemoryCallback;
 if (!strcmp(name,"nvnCommandBufferSetMemoryCallbackData")) return (void *)h_nvnCommandBufferSetMemoryCallbackData;
 if (!strcmp(name,"nvnCommandBufferSetCommandMemoryCallbackEnabled")) return (void *)unsupported_nvnCommandBufferSetCommandMemoryCallbackEnabled;
 if (!strcmp(name,"nvnCommandBufferAddCommandMemory")) return (void *)h_nvnCommandBufferAddCommandMemory;
 if (!strcmp(name,"nvnCommandBufferAddControlMemory")) return (void *)h_nvnCommandBufferAddControlMemory;
 if (!strcmp(name,"nvnCommandBufferGetCommandMemorySize")) return (void *)unsupported_nvnCommandBufferGetCommandMemorySize;
 if (!strcmp(name,"nvnCommandBufferGetCommandMemoryUsed")) return (void *)unsupported_nvnCommandBufferGetCommandMemoryUsed;
 if (!strcmp(name,"nvnCommandBufferGetCommandMemoryFree")) return (void *)unsupported_nvnCommandBufferGetCommandMemoryFree;
 if (!strcmp(name,"nvnCommandBufferGetControlMemorySize")) return (void *)unsupported_nvnCommandBufferGetControlMemorySize;
 if (!strcmp(name,"nvnCommandBufferGetControlMemoryUsed")) return (void *)unsupported_nvnCommandBufferGetControlMemoryUsed;
 if (!strcmp(name,"nvnCommandBufferGetControlMemoryFree")) return (void *)unsupported_nvnCommandBufferGetControlMemoryFree;
 if (!strcmp(name,"nvnCommandBufferBeginRecording")) return (void *)h_nvnCommandBufferBeginRecording;
 if (!strcmp(name,"nvnCommandBufferEndRecording")) return (void *)h_nvnCommandBufferEndRecording;
 if (!strcmp(name,"nvnCommandBufferCallCommands")) return (void *)unsupported_nvnCommandBufferCallCommands;
 if (!strcmp(name,"nvnCommandBufferCopyCommands")) return (void *)h_nvnCommandBufferCopyCommands;
 if (!strcmp(name,"nvnCommandBufferBindBlendState")) return (void *)h_nvnCommandBufferBindBlendState;
 if (!strcmp(name,"nvnCommandBufferBindChannelMaskState")) return (void *)h_nvnCommandBufferBindChannelMaskState;
 if (!strcmp(name,"nvnCommandBufferBindColorState")) return (void *)h_nvnCommandBufferBindColorState;
 if (!strcmp(name,"nvnCommandBufferBindMultisampleState")) return (void *)h_nvnCommandBufferBindMultisampleState;
 if (!strcmp(name,"nvnCommandBufferBindPolygonState")) return (void *)h_nvnCommandBufferBindPolygonState;
 if (!strcmp(name,"nvnCommandBufferBindDepthStencilState")) return (void *)h_nvnCommandBufferBindDepthStencilState;
 if (!strcmp(name,"nvnCommandBufferBindVertexAttribState")) return (void *)h_nvnCommandBufferBindVertexAttribState;
 if (!strcmp(name,"nvnCommandBufferBindVertexStreamState")) return (void *)h_nvnCommandBufferBindVertexStreamState;
 if (!strcmp(name,"nvnCommandBufferBindProgram")) return (void *)h_nvnCommandBufferBindProgram;
 if (!strcmp(name,"nvnCommandBufferBindVertexBuffer")) return (void *)h_nvnCommandBufferBindVertexBuffer;
 if (!strcmp(name,"nvnCommandBufferBindVertexBuffers")) return (void *)h_nvnCommandBufferBindVertexBuffers;
 if (!strcmp(name,"nvnCommandBufferBindUniformBuffer")) return (void *)h_nvnCommandBufferBindUniformBuffer;
 if (!strcmp(name,"nvnCommandBufferBindUniformBuffers")) return (void *)h_nvnCommandBufferBindUniformBuffers;
 if (!strcmp(name,"nvnCommandBufferBindTransformFeedbackBuffer")) return (void *)unsupported_nvnCommandBufferBindTransformFeedbackBuffer;
 if (!strcmp(name,"nvnCommandBufferBindTransformFeedbackBuffers")) return (void *)unsupported_nvnCommandBufferBindTransformFeedbackBuffers;
 if (!strcmp(name,"nvnCommandBufferBindStorageBuffer")) return (void *)h_nvnCommandBufferBindStorageBuffer;
 if (!strcmp(name,"nvnCommandBufferBindStorageBuffers")) return (void *)h_nvnCommandBufferBindStorageBuffers;
 if (!strcmp(name,"nvnCommandBufferBindTexture")) return (void *)unsupported_nvnCommandBufferBindTexture;
 if (!strcmp(name,"nvnCommandBufferBindTextures")) return (void *)unsupported_nvnCommandBufferBindTextures;
 if (!strcmp(name,"nvnCommandBufferBindImage")) return (void *)unsupported_nvnCommandBufferBindImage;
 if (!strcmp(name,"nvnCommandBufferBindImages")) return (void *)h_nvnCommandBufferBindImages;
 if (!strcmp(name,"nvnCommandBufferSetPatchSize")) return (void *)h_nvnCommandBufferSetPatchSize;
 if (!strcmp(name,"nvnCommandBufferSetInnerTessellationLevels")) return (void *)unsupported_nvnCommandBufferSetInnerTessellationLevels;
 if (!strcmp(name,"nvnCommandBufferSetOuterTessellationLevels")) return (void *)unsupported_nvnCommandBufferSetOuterTessellationLevels;
 if (!strcmp(name,"nvnCommandBufferSetPrimitiveRestart")) return (void *)unsupported_nvnCommandBufferSetPrimitiveRestart;
 if (!strcmp(name,"nvnCommandBufferBeginTransformFeedback")) return (void *)unsupported_nvnCommandBufferBeginTransformFeedback;
 if (!strcmp(name,"nvnCommandBufferEndTransformFeedback")) return (void *)unsupported_nvnCommandBufferEndTransformFeedback;
 if (!strcmp(name,"nvnCommandBufferPauseTransformFeedback")) return (void *)unsupported_nvnCommandBufferPauseTransformFeedback;
 if (!strcmp(name,"nvnCommandBufferResumeTransformFeedback")) return (void *)unsupported_nvnCommandBufferResumeTransformFeedback;
 if (!strcmp(name,"nvnCommandBufferDrawTransformFeedback")) return (void *)unsupported_nvnCommandBufferDrawTransformFeedback;
 if (!strcmp(name,"nvnCommandBufferDrawArrays")) return (void *)h_nvnCommandBufferDrawArrays;
 if (!strcmp(name,"nvnCommandBufferDrawElements")) return (void *)h_nvnCommandBufferDrawElements;
 if (!strcmp(name,"nvnCommandBufferDrawElementsBaseVertex")) return (void *)h_nvnCommandBufferDrawElementsBaseVertex;
 if (!strcmp(name,"nvnCommandBufferDrawArraysInstanced")) return (void *)h_nvnCommandBufferDrawArraysInstanced;
 if (!strcmp(name,"nvnCommandBufferDrawElementsInstanced")) return (void *)h_nvnCommandBufferDrawElementsInstanced;
 if (!strcmp(name,"nvnCommandBufferDrawArraysIndirect")) return (void *)h_nvnCommandBufferDrawArraysIndirect;
 if (!strcmp(name,"nvnCommandBufferDrawElementsIndirect")) return (void *)h_nvnCommandBufferDrawElementsIndirect;
 if (!strcmp(name,"nvnCommandBufferMultiDrawArraysIndirectCount")) return (void *)h_nvnCommandBufferMultiDrawArraysIndirectCount;
 if (!strcmp(name,"nvnCommandBufferMultiDrawElementsIndirectCount")) return (void *)h_nvnCommandBufferMultiDrawElementsIndirectCount;
 if (!strcmp(name,"nvnCommandBufferClearColor")) return (void *)h_nvnCommandBufferClearColor;
 if (!strcmp(name,"nvnCommandBufferClearColori")) return (void *)h_nvnCommandBufferClearColori;
 if (!strcmp(name,"nvnCommandBufferClearColorui")) return (void *)h_nvnCommandBufferClearColorui;
 if (!strcmp(name,"nvnCommandBufferClearDepthStencil")) return (void *)h_nvnCommandBufferClearDepthStencil;
 if (!strcmp(name,"nvnCommandBufferDispatchCompute")) return (void *)h_nvnCommandBufferDispatchCompute;
 if (!strcmp(name,"nvnCommandBufferDispatchComputeIndirect")) return (void *)h_nvnCommandBufferDispatchComputeIndirect;
 if (!strcmp(name,"nvnCommandBufferSetViewport")) return (void *)unsupported_nvnCommandBufferSetViewport;
 if (!strcmp(name,"nvnCommandBufferSetViewports")) return (void *)h_nvnCommandBufferSetViewports;
 if (!strcmp(name,"nvnCommandBufferSetViewportSwizzles")) return (void *)unsupported_nvnCommandBufferSetViewportSwizzles;
 if (!strcmp(name,"nvnCommandBufferSetScissor")) return (void *)h_nvnCommandBufferSetScissor;
 if (!strcmp(name,"nvnCommandBufferSetScissors")) return (void *)unsupported_nvnCommandBufferSetScissors;
 if (!strcmp(name,"nvnCommandBufferSetDepthRange")) return (void *)h_nvnCommandBufferSetDepthRange;
 if (!strcmp(name,"nvnCommandBufferSetDepthBounds")) return (void *)h_nvnCommandBufferSetDepthBounds;
 if (!strcmp(name,"nvnCommandBufferSetDepthRanges")) return (void *)unsupported_nvnCommandBufferSetDepthRanges;
 if (!strcmp(name,"nvnCommandBufferSetTiledCacheAction")) return (void *)unsupported_nvnCommandBufferSetTiledCacheAction;
 if (!strcmp(name,"nvnCommandBufferSetTiledCacheTileSize")) return (void *)unsupported_nvnCommandBufferSetTiledCacheTileSize;
 if (!strcmp(name,"nvnCommandBufferBindSeparateTexture")) return (void *)unsupported_nvnCommandBufferBindSeparateTexture;
 if (!strcmp(name,"nvnCommandBufferBindSeparateSampler")) return (void *)h_nvnCommandBufferBindSeparateSampler;
 if (!strcmp(name,"nvnCommandBufferBindSeparateTextures")) return (void *)h_nvnCommandBufferBindSeparateTextures;
 if (!strcmp(name,"nvnCommandBufferBindSeparateSamplers")) return (void *)h_nvnCommandBufferBindSeparateSamplers;
 if (!strcmp(name,"nvnCommandBufferSetStencilValueMask")) return (void *)h_nvnCommandBufferSetStencilValueMask;
 if (!strcmp(name,"nvnCommandBufferSetStencilMask")) return (void *)h_nvnCommandBufferSetStencilMask;
 if (!strcmp(name,"nvnCommandBufferSetStencilRef")) return (void *)h_nvnCommandBufferSetStencilRef;
 if (!strcmp(name,"nvnCommandBufferSetBlendColor")) return (void *)h_nvnCommandBufferSetBlendColor;
 if (!strcmp(name,"nvnCommandBufferSetPointSize")) return (void *)unsupported_nvnCommandBufferSetPointSize;
 if (!strcmp(name,"nvnCommandBufferSetLineWidth")) return (void *)unsupported_nvnCommandBufferSetLineWidth;
 if (!strcmp(name,"nvnCommandBufferSetPolygonOffsetClamp")) return (void *)h_nvnCommandBufferSetPolygonOffsetClamp;
 if (!strcmp(name,"nvnCommandBufferSetAlphaRef")) return (void *)unsupported_nvnCommandBufferSetAlphaRef;
 if (!strcmp(name,"nvnCommandBufferSetSampleMask")) return (void *)unsupported_nvnCommandBufferSetSampleMask;
 if (!strcmp(name,"nvnCommandBufferSetRasterizerDiscard")) return (void *)unsupported_nvnCommandBufferSetRasterizerDiscard;
 if (!strcmp(name,"nvnCommandBufferSetDepthClamp")) return (void *)unsupported_nvnCommandBufferSetDepthClamp;
 if (!strcmp(name,"nvnCommandBufferSetConservativeRasterEnable")) return (void *)unsupported_nvnCommandBufferSetConservativeRasterEnable;
 if (!strcmp(name,"nvnCommandBufferSetConservativeRasterDilate")) return (void *)unsupported_nvnCommandBufferSetConservativeRasterDilate;
 if (!strcmp(name,"nvnCommandBufferSetSubpixelPrecisionBias")) return (void *)unsupported_nvnCommandBufferSetSubpixelPrecisionBias;
 if (!strcmp(name,"nvnCommandBufferCopyBufferToTexture")) return (void *)h_nvnCommandBufferCopyBufferToTexture;
 if (!strcmp(name,"nvnCommandBufferCopyTextureToBuffer")) return (void *)h_nvnCommandBufferCopyTextureToBuffer;
 if (!strcmp(name,"nvnCommandBufferCopyTextureToTexture")) return (void *)h_nvnCommandBufferCopyTextureToTexture;
 if (!strcmp(name,"nvnCommandBufferCopyBufferToBuffer")) return (void *)h_nvnCommandBufferCopyBufferToBuffer;
 if (!strcmp(name,"nvnCommandBufferClearBuffer")) return (void *)unsupported_nvnCommandBufferClearBuffer;
 if (!strcmp(name,"nvnCommandBufferClearTexture")) return (void *)h_nvnCommandBufferClearTexture;
 if (!strcmp(name,"nvnCommandBufferClearTexturei")) return (void *)h_nvnCommandBufferClearTexturei;
 if (!strcmp(name,"nvnCommandBufferClearTextureui")) return (void *)h_nvnCommandBufferClearTextureui;
 if (!strcmp(name,"nvnCommandBufferUpdateUniformBuffer")) return (void *)h_nvnCommandBufferUpdateUniformBuffer;
 if (!strcmp(name,"nvnCommandBufferReportCounter")) return (void *)h_nvnCommandBufferReportCounter;
 if (!strcmp(name,"nvnCommandBufferResetCounter")) return (void *)h_nvnCommandBufferResetCounter;
 if (!strcmp(name,"nvnCommandBufferReportValue")) return (void *)unsupported_nvnCommandBufferReportValue;
 if (!strcmp(name,"nvnCommandBufferSetRenderEnable")) return (void *)unsupported_nvnCommandBufferSetRenderEnable;
 if (!strcmp(name,"nvnCommandBufferSetRenderEnableConditional")) return (void *)unsupported_nvnCommandBufferSetRenderEnableConditional;
 if (!strcmp(name,"nvnCommandBufferSetRenderTargets")) return (void *)h_nvnCommandBufferSetRenderTargets;
 if (!strcmp(name,"nvnCommandBufferDiscardColor")) return (void *)unsupported_nvnCommandBufferDiscardColor;
 if (!strcmp(name,"nvnCommandBufferDiscardDepthStencil")) return (void *)unsupported_nvnCommandBufferDiscardDepthStencil;
 if (!strcmp(name,"nvnCommandBufferDownsample")) return (void *)unsupported_nvnCommandBufferDownsample;
 if (!strcmp(name,"nvnCommandBufferTiledDownsample")) return (void *)unsupported_nvnCommandBufferTiledDownsample;
 if (!strcmp(name,"nvnCommandBufferDownsampleTextureView")) return (void *)h_nvnCommandBufferDownsampleTextureView;
 if (!strcmp(name,"nvnCommandBufferTiledDownsampleTextureView")) return (void *)unsupported_nvnCommandBufferTiledDownsampleTextureView;
 if (!strcmp(name,"nvnCommandBufferBarrier")) return (void *)h_nvnCommandBufferBarrier;
 if (!strcmp(name,"nvnCommandBufferWaitSync")) return (void *)h_nvnCommandBufferWaitSync;
 if (!strcmp(name,"nvnCommandBufferFenceSync")) return (void *)unsupported_nvnCommandBufferFenceSync;
 if (!strcmp(name,"nvnCommandBufferSetTexturePool")) return (void *)h_nvnCommandBufferSetTexturePool;
 if (!strcmp(name,"nvnCommandBufferSetSamplerPool")) return (void *)h_nvnCommandBufferSetSamplerPool;
 if (!strcmp(name,"nvnCommandBufferSetShaderScratchMemory")) return (void *)h_nvnCommandBufferSetShaderScratchMemory;
 if (!strcmp(name,"nvnCommandBufferSaveZCullData")) return (void *)unsupported_nvnCommandBufferSaveZCullData;
 if (!strcmp(name,"nvnCommandBufferRestoreZCullData")) return (void *)unsupported_nvnCommandBufferRestoreZCullData;
 if (!strcmp(name,"nvnCommandBufferSetCopyRowStride")) return (void *)h_nvnCommandBufferSetCopyRowStride;
 if (!strcmp(name,"nvnCommandBufferSetCopyImageStride")) return (void *)unsupported_nvnCommandBufferSetCopyImageStride;
 if (!strcmp(name,"nvnCommandBufferGetCopyRowStride")) return (void *)unsupported_nvnCommandBufferGetCopyRowStride;
 if (!strcmp(name,"nvnCommandBufferGetCopyImageStride")) return (void *)unsupported_nvnCommandBufferGetCopyImageStride;
 if (!strcmp(name,"nvnCommandBufferDrawTexture")) return (void *)unsupported_nvnCommandBufferDrawTexture;
 if (!strcmp(name,"nvnProgramSetSubroutineLinkage")) return (void *)unsupported_nvnProgramSetSubroutineLinkage;
 if (!strcmp(name,"nvnCommandBufferSetProgramSubroutines")) return (void *)unsupported_nvnCommandBufferSetProgramSubroutines;
 if (!strcmp(name,"nvnCommandBufferBindCoverageModulationTable")) return (void *)unsupported_nvnCommandBufferBindCoverageModulationTable;
 if (!strcmp(name,"nvnCommandBufferResolveDepthBuffer")) return (void *)unsupported_nvnCommandBufferResolveDepthBuffer;
 if (!strcmp(name,"nvnCommandBufferSetColorReductionEnable")) return (void *)unsupported_nvnCommandBufferSetColorReductionEnable;
 if (!strcmp(name,"nvnCommandBufferSetColorReductionThresholds")) return (void *)unsupported_nvnCommandBufferSetColorReductionThresholds;
 if (!strcmp(name,"nvnCommandBufferPushDebugGroupStatic")) return (void *)unsupported_nvnCommandBufferPushDebugGroupStatic;
 if (!strcmp(name,"nvnCommandBufferPushDebugGroupDynamic")) return (void *)h_nvnCommandBufferPushDebugGroupDynamic;
 if (!strcmp(name,"nvnCommandBufferPushDebugGroup")) return (void *)unsupported_nvnCommandBufferPushDebugGroup;
 if (!strcmp(name,"nvnCommandBufferPopDebugGroup")) return (void *)h_nvnCommandBufferPopDebugGroup;
 if (!strcmp(name,"nvnCommandBufferPopDebugGroupId")) return (void *)unsupported_nvnCommandBufferPopDebugGroupId;
 if (!strcmp(name,"nvnCommandBufferInsertDebugMarkerStatic")) return (void *)unsupported_nvnCommandBufferInsertDebugMarkerStatic;
 if (!strcmp(name,"nvnCommandBufferInsertDebugMarkerDynamic")) return (void *)h_nvnCommandBufferInsertDebugMarkerDynamic;
 if (!strcmp(name,"nvnCommandBufferInsertDebugMarker")) return (void *)unsupported_nvnCommandBufferInsertDebugMarker;
 if (!strcmp(name,"nvnCommandBufferGetMemoryCallback")) return (void *)unsupported_nvnCommandBufferGetMemoryCallback;
 if (!strcmp(name,"nvnCommandBufferGetMemoryCallbackData")) return (void *)unsupported_nvnCommandBufferGetMemoryCallbackData;
 if (!strcmp(name,"nvnCommandBufferIsRecording")) return (void *)h_nvnCommandBufferIsRecording;
 if (!strcmp(name,"nvnSyncInitialize")) return (void *)h_nvnSyncInitialize;
 if (!strcmp(name,"nvnSyncFinalize")) return (void *)h_nvnSyncFinalize;
 if (!strcmp(name,"nvnSyncSetDebugLabel")) return (void *)unsupported_nvnSyncSetDebugLabel;
 if (!strcmp(name,"nvnQueueFenceSync")) return (void *)h_nvnQueueFenceSync;
 if (!strcmp(name,"nvnSyncWait")) return (void *)h_nvnSyncWait;
 if (!strcmp(name,"nvnQueueWaitSync")) return (void *)h_nvnQueueWaitSync;
 if (!strcmp(name,"nvnSyncInitializeFromFencedGLSync")) return (void *)unsupported_nvnSyncInitializeFromFencedGLSync;
 if (!strcmp(name,"nvnSyncCreateGLSync")) return (void *)unsupported_nvnSyncCreateGLSync;
 if (!strcmp(name,"nvnEventBuilderSetDefaults")) return (void *)h_nvnEventBuilderSetDefaults;
 if (!strcmp(name,"nvnEventBuilderSetStorage")) return (void *)h_nvnEventBuilderSetStorage;
 if (!strcmp(name,"nvnEventBuilderGetStorage")) return (void *)unsupported_nvnEventBuilderGetStorage;
 if (!strcmp(name,"nvnEventBuilderGetMemoryPool")) return (void *)unsupported_nvnEventBuilderGetMemoryPool;
 if (!strcmp(name,"nvnEventBuilderGetMemoryOffset")) return (void *)unsupported_nvnEventBuilderGetMemoryOffset;
 if (!strcmp(name,"nvnEventInitialize")) return (void *)h_nvnEventInitialize;
 if (!strcmp(name,"nvnEventFinalize")) return (void *)h_nvnEventFinalize;
 if (!strcmp(name,"nvnEventGetValue")) return (void *)h_nvnEventGetValue;
 if (!strcmp(name,"nvnEventSignal")) return (void *)h_nvnEventSignal;
 if (!strcmp(name,"nvnEventGetMemoryPool")) return (void *)unsupported_nvnEventGetMemoryPool;
 if (!strcmp(name,"nvnEventGetMemoryOffset")) return (void *)unsupported_nvnEventGetMemoryOffset;
 if (!strcmp(name,"nvnCommandBufferWaitEvent")) return (void *)h_nvnCommandBufferWaitEvent;
 if (!strcmp(name,"nvnCommandBufferSignalEvent")) return (void *)h_nvnCommandBufferSignalEvent;
 if (!strcmp(name,"nvnCommandBufferSetStencilCullCriteria")) return (void *)unsupported_nvnCommandBufferSetStencilCullCriteria;
 if (!strcmp(name,"nvnCommandBufferInsertCheckpointStatic")) return (void *)unsupported_nvnCommandBufferInsertCheckpointStatic;
 if (!strcmp(name,"nvnCommandBufferInsertCheckpointDynamic")) return (void *)unsupported_nvnCommandBufferInsertCheckpointDynamic;
 if (!strcmp(name,"nvnDeviceSetAutomaticCheckpointsEnable")) return (void *)unsupported_nvnDeviceSetAutomaticCheckpointsEnable;
 if (!strcmp(name,"nvnDeviceSetAutomaticCheckpointsSamplingInterval")) return (void *)unsupported_nvnDeviceSetAutomaticCheckpointsSamplingInterval;
 if (!strcmp(name,"nvnDeviceSetAutomaticCheckpointsByDebugGroup")) return (void *)unsupported_nvnDeviceSetAutomaticCheckpointsByDebugGroup;
 if (!strcmp(name,"nvnDeviceSetCheckpointCallStacksEnable")) return (void *)unsupported_nvnDeviceSetCheckpointCallStacksEnable;
 if (!strcmp(name,"nvnDeviceSetCheckpointMaxCallStacksDepth")) return (void *)unsupported_nvnDeviceSetCheckpointMaxCallStacksDepth;
 xbg_logf("NVN unknown proc name: %s",name); return NULL;
}
