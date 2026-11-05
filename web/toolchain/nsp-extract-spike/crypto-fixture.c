/* 使用公开 NIST SP 800-38A AES-128 CTR 合成向量及 XTS
 * 往返测试。不读取用户密钥、包或游戏数据，链接未改动的上游 aes.c。 */
/* Synthetic public NIST SP 800-38A AES-128 CTR vector + XTS round-trip.
 * No user keys, packages, or game data are read. Links upstream aes.c unchanged.
 */
#include <stdio.h>
#include <string.h>
#include "aes.h"
int main(void) {
    unsigned char key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                             0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    unsigned char ctr[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                             0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
    unsigned char plain[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
                               0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    unsigned char expected[16] = {0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26,
                                  0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce};
    unsigned char out[16];
    aes_ctx_t *ctx = new_aes_ctx(key, sizeof key, AES_MODE_CTR);
    aes_setiv(ctx, ctr, sizeof ctr);
    aes_encrypt(ctx, out, plain, sizeof plain);
    if (memcmp(out, expected, sizeof out))
        return 1;
    free_aes_ctx(ctx);
    unsigned char xts_key[32], sectors[1024], encrypted[1024], decrypted[1024];
    for (unsigned i = 0; i < 32; i++)
        xts_key[i] = (unsigned char)i;
    for (unsigned i = 0; i < 1024; i++)
        sectors[i] = (unsigned char)(i * 7);
    ctx = new_aes_ctx(xts_key, sizeof xts_key, AES_MODE_XTS);
    aes_xts_encrypt(ctx, encrypted, sectors, sizeof sectors, 0x123, 512);
    aes_xts_decrypt(ctx, decrypted, encrypted, sizeof encrypted, 0x123, 512);
    if (memcmp(sectors, decrypted, sizeof sectors) || !memcmp(sectors, encrypted, sizeof sectors))
        return 2;
    free_aes_ctx(ctx);
    puts("PASS synthetic AES-CTR known-answer and XTS two-sector round-trip (not NCA validation)");
    return 0;
}
