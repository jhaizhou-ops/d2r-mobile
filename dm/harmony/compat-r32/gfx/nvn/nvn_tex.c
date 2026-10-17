// NVN 贴图的存储尺寸 / 对齐 / 各级偏移（Tegra X1 块线性布局）。
// 算法移植自 deko3d source/dk_image.cpp（zlib 许可，Copyright (C) 2018-2020 fincs）：
//   一个 GOB = 64 字节 × 8 行 = 512 字节；块高（tileH，单位 GOB）按「高度×1.5」挑；各级按块对齐累加。
// 游戏的预排布贴图（SetPackagedTextureLayout）带 8 字节布局参数：第 0 字节 = 块高 log2（10-01 真游戏 16 例对照得出，
// 其中 4 例与自动算法不同），有它时以它为准。
#include "nvn_tex.h"

typedef struct { uint8_t bpb, bw, bh; } fmt_t;   // 每块字节、块宽、块高（非压缩格式块宽高 = 1）

static fmt_t fmt_of(int f) {
    static const uint8_t astc[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    if (f >= 0x79 && f <= 0x94) { int k = (f - 0x79) % 14; return (fmt_t){16, astc[k][0], astc[k][1]}; }
    if (f >= 0x41 && f <= 0x50) {                         // DXT / RGTC / BPTC：4×4 块
        int b8 = (f <= 0x42) || (f == 0x45 || f == 0x46) || (f == 0x49 || f == 0x4A);
        return (fmt_t){b8 ? 8 : 16, 4, 4};
    }
    static const struct { uint8_t lo, hi, bpb; } r[] = {
        {0x01,0x04,1},{0x05,0x09,2},{0x0A,0x0C,4},{0x0D,0x10,2},{0x11,0x15,4},{0x16,0x18,8},
        {0x19,0x1C,3},{0x1D,0x21,6},{0x22,0x24,12},{0x25,0x28,4},{0x29,0x2D,8},{0x2E,0x30,16},
        {0x31,0x31,1},{0x32,0x32,2},{0x33,0x35,4},{0x36,0x36,8},{0x37,0x38,4},{0x39,0x3C,2},{0x3D,0x40,4},
        {0x51,0x52,1},{0x53,0x54,2},{0x55,0x56,4},{0x57,0x58,2},{0x59,0x5A,4},{0x5B,0x5C,8},
        {0x5D,0x5E,3},{0x5F,0x60,6},{0x61,0x62,12},{0x63,0x64,4},{0x65,0x66,8},{0x67,0x68,16},
        {0x69,0x6C,4},{0x6D,0x70,4},{0x71,0x75,8},{0x76,0x78,16},{0x95,0x98,2},{0x99,0x9C,4},
    };
    for (unsigned i = 0; i < sizeof r / sizeof *r; i++)
        if (f >= r[i].lo && f <= r[i].hi) return (fmt_t){r[i].bpb, 1, 1};
    return (fmt_t){0, 1, 1};
}

int nvn_fmt_bpb(int f, int *bw, int *bh) { fmt_t t = fmt_of(f); if (bw) *bw = t.bw; if (bh) *bh = t.bh; return t.bpb; }

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static uint32_t blk(uint32_t s, uint32_t b) { return (s + b - 1) / b; }
static unsigned pick_tile(uint32_t gobs) { return gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0; }
static unsigned adj_tile(unsigned shift, uint32_t unit, uint32_t dim) {
    if (!shift) return 0;
    uint32_t x = unit << (shift - 1);
    if (x >= dim) while (--shift) { x >>= 1; if (x < dim) break; }
    return shift;
}

// target：NVNtextureTarget；dims 维数、是否分层由 target 决定
static void shape(const nvn_texdesc *d, unsigned *dpl, int *layered, uint32_t *w, uint32_t *h, uint32_t *z) {
    *w = d->width; *h = d->height; *z = d->depth; *layered = 0;
    switch (d->target) {
        case 0: *dpl = 1; *h = 1; *z = 1; break;                       // 1D
        case 3: *dpl = 1; *z = d->height; *h = 1; *layered = 1; break; // 1D_ARRAY（层数在 height）
        case 2: *dpl = 3; break;                                        // 3D
        case 4: case 6: *dpl = 2; *layered = 1; break;                  // 2D_ARRAY / MS_ARRAY
        case 8: *dpl = 2; *z = 6; *layered = 1; break;                  // CUBEMAP
        case 9: *dpl = 2; *z = d->depth * 6; *layered = 1; break;       // CUBEMAP_ARRAY
        default: *dpl = 2; *z = 1; break;                               // 2D / MS / RECTANGLE
    }
    if (*z == 0) *z = 1;
}

static void samples_xy(int s, uint32_t *sx, uint32_t *sy) {
    *sx = *sy = 1;
    if (s == 2) *sx = 2; else if (s == 4) *sx = *sy = 2; else if (s == 8) { *sx = 4; *sy = 2; }
}

static uint64_t level_offset(const nvn_texdesc *d, unsigned level, unsigned dpl, uint32_t w, uint32_t h, uint32_t z,
                             unsigned tileH, unsigned tileD, fmt_t t) {
    uint32_t sx, sy; samples_xy(d->samples, &sx, &sy);
    uint64_t off = 0;
    for (unsigned i = 0; i < level; i++) {
        uint32_t lw = blk(mip(w * sx, i), t.bw);
        uint32_t lh = dpl >= 2 ? blk(mip(h * sy, i), t.bh) : 1;
        uint32_t ld = dpl >= 3 ? mip(z, i) : 1;
        uint32_t lwb = lw * t.bpb;
        unsigned th = adj_tile(tileH, 8, lh), td = adj_tile(tileD, 1, ld);
        uint32_t wg = (lwb + 63) / 64, hg = (lh + 7) / 8;
        uint32_t wt = wg, ht = (hg + (1u << th) - 1) >> th, dt = (ld + (1u << td) - 1) >> td;
        off += (uint64_t)wt * ht * dt << (9 + th + td);
    }
    return off;
}

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    uint32_t sx, sy; samples_xy(d->samples, &sx, &sy);
    o->bpb = t.bpb; o->tileH = o->tileD = 0; o->layers = layered ? z : 1;
    if (!t.bpb) { o->size = 0; o->align = 512; o->layer_size = 0; return; }
    if (d->target == 10) {                                            // BUFFER
        o->layer_size = o->size = (uint64_t)t.bpb * d->width;
        o->align = t.bpb == 12 ? 4 : t.bpb; return;
    }
    if (d->flags & 0x10) {                                            // LINEAR：行距 = stride 或按 32 对齐
        o->align = (d->flags & 0x40) ? 128 : 32;
        uint32_t stride = d->stride ? d->stride : (t.bpb * blk(w, t.bw) + o->align - 1) & ~(o->align - 1);
        o->layer_size = o->size = (uint64_t)stride * blk(h, t.bh);
        return;
    }
    unsigned tileH = 0, tileD = 0;
    if (dpl == 2) { uint32_t ah = blk(h * sy, t.bh); tileH = d->pkg_block_h ? (unsigned)d->pkg_block_h - 1 : pick_tile((ah + ah / 2 + 7) / 8); }
    else if (dpl == 3) tileD = pick_tile(z + z / 2);
    o->tileH = tileH; o->tileD = tileD;
    unsigned levels = d->levels ? d->levels : 1;
    uint64_t ls = level_offset(d, levels, dpl, w, h, z, tileH, tileD, t);
    if (layered) {                                                    // 层尺寸按块对齐
        uint32_t hh = blk(h, t.bh); unsigned th = tileH, td = tileD;
        while (th && hh <= (8u << (th - 1))) th--;
        while (td && z <= (1u << (td - 1))) td--;
        unsigned sh = th + td + 9; uint64_t g = ls >> sh;
        if (ls != g << sh) ls = (g + 1) << sh;
    }
    o->layer_size = ls;
    o->size = layered ? ls * z : ls;
    o->align = 512;
}

uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || (d->flags & 0x10) || d->target == 10) return 0;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    return level_offset(d, level, dpl, w, h, z, L.tileH, L.tileD, t);
}

void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl; int layered; uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    uint32_t sx, sy; samples_xy(d->samples, &sx, &sy);
    nvn_texlayout L; nvn_tex_layout(d, &L);
    uint32_t lh = dpl >= 2 ? blk(mip(h * sy, level), t.bh) : 1;
    uint32_t ld = dpl >= 3 ? mip(z, level) : 1;
    *tileH = adj_tile(L.tileH, 8, lh);
    *tileD = adj_tile(L.tileD, 1, ld);
}
