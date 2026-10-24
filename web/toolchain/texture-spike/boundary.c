/* 有界二维桥接，原始 NVN/bcdec 源码保持不变。 */
/* Bounded 2D bridge; original NVN/bcdec sources remain unmodified. */
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include "nvn_bcdec.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT (64u * 1024u * 1024u)
_Static_assert(sizeof(nvn_texdesc) == 40, "NVN descriptor ABI changed");
_Static_assert(offsetof(nvn_texdesc, pkg_block_h) == 36, "NVN descriptor ABI changed");
/* 结果字依次为存储字节数、紧凑压缩字节数、宽、高、Metal
 * 像素格式、每像素字节数、压缩行字节数及压缩行数。 */
/* Result words: storage bytes, tight compressed bytes, width, height,
 * Metal pixel format, bytes/pixel, compressed row bytes, compressed rows. */
int texture_info(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t *out) {
    if (!d || !out)
        return -1;
    if (d->format < 0x41 || d->format > 0x50)
        return -2;
    if (d->target != 1 || d->depth != 1 || layer || d->samples < 0 || d->samples > 1 ||
        (d->flags != 0 && d->flags != 0x10 && d->flags != 0x50))
        return -3;
    if (!d->width || !d->height || d->width > 8192 || d->height > 8192 || !d->levels ||
        d->levels > 14 || level >= d->levels || d->pkg_block_h < 0 || d->pkg_block_h > 6)
        return -1;
    unsigned max_levels = 1, max_dim = d->width > d->height ? d->width : d->height;
    while (max_dim >>= 1)
        max_levels++;
    if (d->levels > max_levels)
        return -1;
    if ((d->flags & 0x10) && (d->levels != 1 || d->pkg_block_h))
        return -3;
    if (!(d->flags & 0x10) && d->stride)
        return -3;
    uint32_t w = d->width >> level, h = d->height >> level, pf;
    if (!w)
        w = 1;
    if (!h)
        h = 1;
    int bw, bh, bpp;
    uint32_t bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!nvn_bc_unpacked(d->format, &pf, &bpp))
        return -2;
    uint32_t row = ((w + 3) / 4) * bpb, rows = (h + 3) / 4;
    if ((d->flags & 0x10) && d->stride && (d->stride < row || d->stride > LIMIT))
        return -1;
    nvn_texlayout layout;
    nvn_tex_layout(d, &layout);
    if (!layout.size || layout.size > LIMIT || (uint64_t)w * h * bpp > LIMIT)
        return -4;
    uint32_t result[8] = {(uint32_t)layout.size, row * rows, w, h, pf, (uint32_t)bpp, row, rows};
    memcpy(out, result, sizeof result);
    return 0;
}

/* Pillow 10.2 DDS/DXT5 先扩展 565 端点，再截断插值的 8 位通道；bcdec 在 565
 * 域内插值，有效块也会得到不同 RGB 字节。原生美术固定 PNG 需要 Pillow 的结果。 */
/* Pillow 10.2 DDS/DXT5 expands 565 endpoints first, then truncates interpolated
 * 8-bit channels. bcdec interpolates in the 565 domain, giving different RGB
 * bytes for valid blocks. Native-art's pinned PNGs require Pillow's result. */
static void pillow_bc3(const uint8_t *linear, uint32_t pitch, uint32_t width, uint32_t height,
                       uint8_t *dst) {
    for (uint32_t by = 0; by < (height + 3) / 4; by++) {
        for (uint32_t bx = 0; bx < (width + 3) / 4; bx++) {
            const uint8_t *block = linear + by * pitch + bx * 16;
            const uint16_t c0 = (uint16_t)block[8] | ((uint16_t)block[9] << 8);
            const uint16_t c1 = (uint16_t)block[10] | ((uint16_t)block[11] << 8);
            uint8_t color[4][3];
            const uint16_t endpoints[2] = {c0, c1};
            for (int i = 0; i < 2; i++) {
                const uint16_t c = endpoints[i];
                const uint8_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
                color[i][0] = (r << 3) | (r >> 2);
                color[i][1] = (g << 2) | (g >> 4);
                color[i][2] = (b << 3) | (b >> 2);
            }
            for (int channel = 0; channel < 3; channel++) {
                color[2][channel] = (2 * color[0][channel] + color[1][channel]) / 3;
                color[3][channel] = (color[0][channel] + 2 * color[1][channel]) / 3;
            }
            uint8_t alpha[8] = {block[0], block[1]};
            if (alpha[0] > alpha[1]) {
                for (int i = 2; i < 8; i++)
                    alpha[i] = ((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7;
            } else {
                for (int i = 2; i < 6; i++)
                    alpha[i] = ((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5;
                alpha[6] = 0;
                alpha[7] = 255;
            }
            uint64_t alpha_indices = 0;
            uint32_t color_indices = 0;
            for (int i = 0; i < 6; i++)
                alpha_indices |= (uint64_t)block[2 + i] << (8 * i);
            for (int i = 0; i < 4; i++)
                color_indices |= (uint32_t)block[12 + i] << (8 * i);
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; y++) {
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; x++) {
                    const uint32_t index = y * 4 + x;
                    uint8_t *pixel = dst + 4 * ((by * 4 + y) * width + bx * 4 + x);
                    const uint8_t *rgb = color[(color_indices >> (2 * index)) & 3];
                    pixel[0] = rgb[0];
                    pixel[1] = rgb[1];
                    pixel[2] = rgb[2];
                    pixel[3] = alpha[(alpha_indices >> (3 * index)) & 7];
                }
            }
        }
    }
}

static int texture_decode_impl(const nvn_texdesc *d, unsigned level, unsigned layer,
                               const uint8_t *src, uint32_t src_size, uint8_t *dst,
                               uint32_t dst_size, int pillow) {
    uint32_t m[8];
    int status = texture_info(d, level, layer, m);
    if (status)
        return status;
    if (pillow && (d->format != 0x44 || level || layer || d->levels != 1))
        return -3;
    if (!src || !dst || src_size != m[0] || dst_size != m[2] * m[3] * m[5])
        return -5;
    uint8_t *linear = malloc(m[1]);
    if (!linear)
        return -4;
    if (d->flags & 0x10) {
        uint32_t align = (d->flags & 0x40) ? 128 : 32;
        uint32_t pitch = d->stride ? d->stride : (m[6] + align - 1) & ~(align - 1);
        for (uint32_t y = 0; y < m[7]; y++)
            memcpy(linear + y * m[6], src + y * pitch, m[6]);
        status = 0;
    } else {
        status = nvn_deswizzle(d, level, layer, src, linear, m[6]);
    }
    if (!status) {
        if (pillow)
            pillow_bc3(linear, m[6], m[2], m[3], dst);
        else
            status = nvn_bc_decode(d->format, linear, m[6], m[2], m[3], dst, m[2] * m[5]);
    }
    free(linear);
    return status;
}

int texture_decode(const nvn_texdesc *d, unsigned level, unsigned layer, const uint8_t *src,
                   uint32_t src_size, uint8_t *dst, uint32_t dst_size) {
    return texture_decode_impl(d, level, layer, src, src_size, dst, dst_size, 0);
}

int texture_decode_pillow_bc3(const nvn_texdesc *d, unsigned level, unsigned layer,
                              const uint8_t *src, uint32_t src_size, uint8_t *dst,
                              uint32_t dst_size) {
    return texture_decode_impl(d, level, layer, src, src_size, dst, dst_size, 1);
}

/* 编码边界刻意只支持完整单层 BC3 二维纹理。 */
/* Encoding boundary intentionally supports only complete single-level BC3 2D. */
int texture_pack_bc3(const nvn_texdesc *d, const uint8_t *linear, uint32_t linear_size,
                     uint8_t *dst, uint32_t dst_size) {
    uint32_t m[8];
    int status = texture_info(d, 0, 0, m);
    if (status)
        return status;
    if (d->format != 0x44 || d->levels != 1 || d->flags || d->stride || d->samples)
        return -3;
    if ((uint64_t)d->width * d->height > 4u * 1024u * 1024u)
        return -4;
    if (!linear || !dst || linear_size != m[1] || dst_size != m[0])
        return -5;
    /* 原生打包使用新建清零的 ctypes 缓冲区，未使用的 GOB 字节必须匹配。 */
    /* Native pack uses a fresh zeroed ctypes buffer; unused GOB bytes must match. */
    memset(dst, 0, dst_size);
    return nvn_swizzle(d, 0, 0, linear, m[6], dst);
}
