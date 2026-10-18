#pragma once
#include <stddef.h>
#include <stdint.h>

// 两个方向共享字节间距；零表示紧排，在访问内存池前拒绝无效或溢出的范围。
// Both copy directions use the same recorded byte strides. Zero selects tight
static inline int nvn_copy_pitch(int width, int height, int depth, int bw, int bh,
                                int bpb, int64_t row_stride, int64_t image_stride,
                                size_t *row, size_t *image, size_t *extent) {
    if (width <= 0 || height <= 0 || depth < 0 || bw <= 0 || bh <= 0 || bpb <= 0 ||
        row_stride < 0 || image_stride < 0) return 0;
    size_t blocks = ((size_t)width + bw - 1) / bw;
    size_t rows = ((size_t)height + bh - 1) / bh;
    if (blocks > SIZE_MAX / (size_t)bpb) return 0;
    size_t tight = blocks * bpb;
    *row = row_stride ? (size_t)row_stride : tight;
    if (*row < tight || *row > SIZE_MAX / rows) return 0;
    *image = image_stride ? (size_t)image_stride : *row * rows;
    if (*image < *row * rows) return 0;
    size_t layers = depth ? (size_t)depth : 1;
    if (*image > SIZE_MAX / layers) return 0;
    *extent = *image * layers;
    return 1;
}
