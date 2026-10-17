// NVN 贴图存储布局（Tegra X1 块线性），见 nvn_tex.c。
#pragma once
#include <stdint.h>

typedef struct {
    int target, format, flags, samples;   // NVNtextureTarget / NVNformat / NVNtextureFlags / 采样数（0 或 1 = 单采样）
    uint32_t width, height, depth, levels, stride;
    int pkg_block_h;                      // 预排布贴图布局参数第 0 字节（块高 log2）+ 1；0 = 按自动算法
} nvn_texdesc;

typedef struct {
    uint64_t size, layer_size;            // 总存储字节、每层字节
    uint32_t align, layers, bpb;          // 对齐、层数、每块字节
    unsigned tileH, tileD;                // 块高 / 块深（log2 GOB 数）
} nvn_texlayout;

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);   // 每块字节（0 = 未知格式），块宽高
// 第 level 级实际使用的块高 / 块深（log2 GOB 数）：贴图级块高按该级行数（块行）收缩，解排布逐级用它
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);
