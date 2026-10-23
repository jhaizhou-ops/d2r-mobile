/* BC4/BC5 的 8 位通道回退（xbg_bc_8bit_channels）：设备不能线性过滤 16 位 R/RG 时，解码目标从
 * R16/R16G16 换成 R8/R8G8。这里核两件事：8 位输出必须等于 16 位输出的高字节（只降精度、不改值）；
 * 有符号 BC4/BC5（0x4a/0x4c）与其余 BC 格式不因开关改变像素格式。
 * BC4/BC5 8-bit channel fallback (xbg_bc_8bit_channels): when the device cannot linearly filter 16-bit
 * R/RG, the decode target switches from R16/R16G16 to R8/R8G8. Two things are checked here: the 8-bit
 * output must equal the high byte of the 16-bit output (lower precision only, same values), and the
 * signed BC4/BC5 (0x4a/0x4c) plus every other BC format keep their pixel format regardless of the switch.
 * 依据 xbg_texture.c 的 xbg_bc_format_info() 与 xbg_decode_block()；不需要游戏文件。 */
/* Based on xbg_bc_format_info() and xbg_decode_block() in xbg_texture.c; no game files needed. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "xbg_texture.h"

/* 只为链接：本测试不走块线性布局路径（nvn_bl_offset 在 nvn/hle_nvn.c） | Link stub only: this test does not exercise the block-linear path */
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned tileH, unsigned tileD) {
    (void)y; (void)z; (void)row_bytes; (void)rows; (void)tileH; (void)tileD;
    return (uint64_t)x;
}

static int fails;

static void check(int ok, const char *what, uint32_t fmt) {
    if (!ok) { printf("FAIL %s (format %#x)\n", what, fmt); ++fails; }
}

/* 同一批固定字节喂两条路径：块内容不必有意义，比较的是两条解码路径的关系
 * The same fixed bytes feed both paths: the block need not be meaningful, what is compared is the relation between the two paths */
static void run(uint32_t fmt, const char *name, int eight) {
    uint8_t block[16];
    for (int i = 0; i < 16; i++) block[i] = (uint8_t)(i * 37 + 11);

    xbg_bc_format f16, f8;
    xbg_bc_8bit_channels = 0; check(xbg_bc_format_info(fmt, &f16) == 0, "format_info", fmt);
    xbg_bc_8bit_channels = 1; check(xbg_bc_format_info(fmt, &f8) == 0, "format_info 8bit", fmt);

    uint8_t out16[16 * 8], out8[16 * 8];
    memset(out16, 0xAA, sizeof out16);
    memset(out8, 0xAA, sizeof out8);
    xbg_bc_8bit_channels = 0; int r16 = xbg_decode_block(fmt, block, f16.bytes_per_block, out16, 16 * f16.bytes_per_pixel);
    xbg_bc_8bit_channels = 1; int r8 = xbg_decode_block(fmt, block, f16.bytes_per_block, out8, 16 * f8.bytes_per_pixel);
    xbg_bc_8bit_channels = 0;

    check(r16 == 0 && r8 == 0, "decode returns 0", fmt);
    if (r16 || r8) { printf("%s: decode failed r16=%d r8=%d\n", name, r16, r8); return; }

    if (!eight) {
        /* 有符号格式保持 16 位：开关不能改变它们 | Signed formats stay 16-bit: the switch must not change them */
        check(f8.pixels == f16.pixels && f8.bytes_per_pixel == f16.bytes_per_pixel, "signed format unchanged", fmt);
        printf("%s: unchanged, %u bytes per pixel\n", name, f16.bytes_per_pixel);
        return;
    }
    unsigned nch = f16.bytes_per_pixel / 2;   /* 16 位通道数：BC4 = 1、BC5 = 2 | 16-bit channels: BC4 = 1, BC5 = 2 */
    check(f8.bytes_per_pixel == nch, "8bit is one byte per channel", fmt);
    for (unsigned i = 0; i < 16; i++)
        for (unsigned c = 0; c < nch; c++)
            check(out8[i * f8.bytes_per_pixel + c] == out16[i * f16.bytes_per_pixel + c * 2 + 1], "8bit equals the high byte", fmt);
    printf("%s: %u -> %u bytes per pixel, high bytes match\n", name, f16.bytes_per_pixel, f8.bytes_per_pixel);
}

int main(void) {
    run(0x49, "BC4 UNORM (0x49)", 1);
    run(0x4a, "BC4 SNORM (0x4a)", 0);
    run(0x4b, "BC5 UNORM (0x4b)", 1);
    run(0x4c, "BC5 SNORM (0x4c)", 0);
    /* 其余 BC1..BC7 / BC6H：开关不得改变像素格式 | Every other BC1..BC7 / BC6H format must be unchanged by the switch */
    for (uint32_t fmt = 0x41; fmt <= 0x50; fmt++) {
        if (fmt == 0x49 || fmt == 0x4b) continue;
        xbg_bc_format a, b;
        xbg_bc_8bit_channels = 0; xbg_bc_format_info(fmt, &a);
        xbg_bc_8bit_channels = 1; xbg_bc_format_info(fmt, &b);
        xbg_bc_8bit_channels = 0;
        check(a.pixels == b.pixels && a.bytes_per_pixel == b.bytes_per_pixel, "switch must not touch this format", fmt);
    }
    printf(fails ? "BC8 FAIL %d\n" : "BC8 OK\n", fails);
    return fails != 0;
}
