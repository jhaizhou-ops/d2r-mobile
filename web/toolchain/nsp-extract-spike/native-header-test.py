#!/usr/bin/env python3
"""用合成头与加密桩验证固定版本 NCA 头路径，不使用真实密钥或游戏。
Test pinned NCA header control flow using synthetic bytes and crypto stubs.
Usage: python3 native-header-test.py /path/to/unmodified/pinned/hactool
Requires cc. This tests format dispatch and initialized reads, not cryptography.
"""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="d2r-synthetic-header-") as directory:
    work = Path(directory)
    for name in ["extkeys.c", "utils.h", "nca.c"]:
        shutil.copyfile(source / name, work / name)
    subprocess.run(
        [sys.executable, str(here / "prepare-source.py"), str(work)], check=True
    )
    (work / "fixture.c").write_text(r"""
#include <assert.h>
#include "nca.h"
#include "rsa.h"
#include "sha.h"
static nca_header_t decrypted;
static aes_ctx_t dummy;
static unsigned legacy_calls, section_decrypts, freed;
aes_ctx_t *new_aes_ctx(const void *key, unsigned int size, aes_mode_t mode) {
    (void)key; (void)size;
    if (mode == AES_MODE_ECB) legacy_calls++;
    return &dummy;
}
void free_aes_ctx(aes_ctx_t *ctx) { (void)ctx; freed++; }
void aes_decrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t n) {
    (void)ctx; memcpy(dst, src, n);
}
void aes_xts_decrypt(aes_ctx_t *ctx, void *dst, const void *src, size_t n,
                     size_t sector, size_t sector_size) {
    (void)ctx; (void)sector; (void)sector_size;
    if (n == 0x400 || n == 0xc00) memcpy(dst, &decrypted, n);
    else { assert(n == 0x200); section_decrypts++; memcpy(dst, src, n); }
}
const unsigned char *pki_get_beta_nca0_modulus(void) { legacy_calls++; return NULL; }
const unsigned char *pki_get_beta_nca0_exponent(void) { return NULL; }
const unsigned char *pki_get_beta_nca0_label_hash(void) { return NULL; }
int rsa2048_oaep_decrypt_verify(void *out, size_t max_out_len,
    const unsigned char *signature, const unsigned char *modulus,
    const unsigned char *exponent, size_t exponent_len,
    const unsigned char *label_hash, size_t *out_len) {
    (void)out; (void)max_out_len; (void)signature; (void)modulus;
    (void)exponent; (void)exponent_len; (void)label_hash; (void)out_len;
    return 0;
}
void sha256_hash_buffer(unsigned char *hash, const void *buffer, size_t len) {
    (void)buffer; (void)len; memset(hash, 0, 32);
}
static void check(uint32_t magic) {
    hactool_ctx_t tool = {0};
    nca_ctx_t ctx = {0};
    ctx.tool_ctx = &tool;
    ctx.file = tmpfile(); assert(ctx.file);
    nca_header_t wire = {0};
    memset(&wire, 0x55, 0x400); /* Not an already-decrypted magic. */
    memset(&wire.fs_headers[0], 0x55, sizeof wire.fs_headers[0]);
    memset(&wire.fs_headers[1], 0x66, sizeof wire.fs_headers[1]);
    /* Section 2 has only a later reserved byte set; section 3 is absent. */
    wire.fs_headers[2]._0x148[1] = 1;
    assert(fwrite(&wire, 1, sizeof wire, ctx.file) == sizeof wire);
    memset(&decrypted, 0, sizeof decrypted);
    decrypted.magic = magic;
    decrypted.kaek_ind = 255; /* Legacy path must never use this index. */
    legacy_calls = section_decrypts = freed = 0;
    int result = nca_decrypt_header(&ctx);
    assert(legacy_calls == 0 && freed == 1);
    if (magic == MAGIC_NCA2) {
        assert(result == 1 && section_decrypts == 3);
        assert(!memcmp(ctx.header.fs_headers, wire.fs_headers, sizeof wire.fs_headers));
    } else if (magic == MAGIC_NCA3) {
        assert(result == 1 && section_decrypts == 0);
    } else {
        assert(result == 0 && section_decrypts == 0);
    }
    fclose(ctx.file);
}
int main(void) {
    check(MAGIC_NCA0);
    check(0);
    check(MAGIC_NCA2);
    check(MAGIC_NCA3);
    puts("PASS synthetic native NCA format rejection and initialized NCA2 section detection");
}
""")
    # 对支持自动清零的编译器，让原先的未初始化输出缺陷确定重现；回归同时按普通方式运行。
    # Make the former uninitialized-output bug deterministic on compilers that
    # support automatic zero initialization; the regression also runs normally.
    for automatic_init in [[], ["-ftrivial-auto-var-init=zero"]]:
        subprocess.run(
            [
                "cc",
                "-std=gnu11",
                "-O1",
                *automatic_init,
                "-ffunction-sections",
                "-fdata-sections",
                "-D_FILE_OFFSET_BITS=64",
                "-I" + str(source),
                "-I" + str(source / "mbedtls/include"),
                str(work / "fixture.c"),
                str(work / "nca.c"),
                "-Wl,--gc-sections",
                "-o",
                str(work / "fixture"),
            ],
            check=True,
        )
        subprocess.run([str(work / "fixture")], check=True)
