#!/usr/bin/env python3
"""用合成 PFS0 验证生产 C 目录边界，不读取游戏或密钥。
Test production C directory bounds with synthetic PFS0, never game/key inputs.
Usage: python3 native-directory-test.py /path/to/pinned/hactool
Requires cc; crypto/section functions are unreachable traps, not crypto tests.
需要 cc；加密／分区函数仅为不可达陷阱，并非加密测试。
"""

from pathlib import Path
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="d2r-synthetic-directory-") as directory:
    work = Path(directory)
    (work / "fixture.c").write_text(r"""
#include <assert.h>
#include "boundary.c"
/* 仅原始 PFS0 路径可达；意外进入加密路径立即失败。
 * Only raw PFS0 is exercised; accidentally entering crypto fails immediately. */
void free_aes_ctx(aes_ctx_t *ctx) { (void)ctx; abort(); }
void aes_setiv(aes_ctx_t *ctx, const void *iv, size_t n) {
    (void)ctx; (void)iv; (void)n; abort();
}
void aes_decrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t n) {
    (void)ctx; (void)dst; (void)src; (void)n; abort();
}
void nca_section_fseek(nca_section_ctx_t *ctx, uint64_t at) {
    (void)ctx; (void)at; abort();
}
size_t nca_section_fread(nca_section_ctx_t *ctx, void *out, size_t n) {
    (void)ctx; (void)out; (void)n; abort();
}
static void fixture(const char *path, size_t length, unsigned padding,
                    unsigned offset, int terminated, int expected) {
    pfs0_header_t head = {0};
    pfs0_file_entry_t entry = {0};
    head.magic = MAGIC_PFS0;
    head.num_files = 1;
    head.string_table_size = padding + length + terminated;
    entry.string_table_offset = offset;
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(&head, 1, sizeof head, file) == sizeof head);
    assert(fwrite(&entry, 1, sizeof entry, file) == sizeof entry);
    for (unsigned i = 0; i < padding; i++) fputc(0, file);
    for (size_t i = 0; i < length; i++) fputc('a', file);
    if (terminated) fputc(0, file);
    assert(!fclose(file));
    assert(nsp_open_pfs0(path) == expected);
    if (expected) {
        assert(nsp_entry_count() == -1);
        assert(!raw && !directory);
    } else {
        unsigned char info[272];
        assert(nsp_entry_count() == 1);
        assert(nsp_entry_info(0, info, sizeof info) == (int)(17 + length));
    }
    nsp_close();
}
int main(int argc, char **argv) {
    assert(argc == 2);
    fixture(argv[1], 255, 0, 0, 1, 0);
    fixture(argv[1], 256, 0, 0, 1, -2);
    fixture(argv[1], 4096, 0, 0, 1, -2);
    fixture(argv[1], 255, 0, 0, 0, -2);
    fixture(argv[1], 3, 0, 1, 1, -2);
    fixture(argv[1], 3, 2, 2, 1, 0);
    puts("PASS synthetic native PFS0 name bounds, padding and failure cleanup");
}
""")
    subprocess.run(
        [
            "cc",
            "-std=gnu11",
            "-O2",
            "-ffunction-sections",
            "-fdata-sections",
            "-D_FILE_OFFSET_BITS=64",
            "-I" + str(here),
            "-I" + str(source),
            "-I" + str(source / "mbedtls/include"),
            str(work / "fixture.c"),
            "-Wl,--gc-sections",
            "-o",
            str(work / "fixture"),
        ],
        check=True,
    )
    subprocess.run([str(work / "fixture"), str(work / "synthetic.pfs0")], check=True)
