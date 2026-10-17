/* CommonCrypto 垫片实现：SHA-1 与 MD5（公有领域风格的紧凑实现）。
 * 用于 dm_padmode 的字符串旁路表校验（SHA-1 前 4 字节十六进制）与诊断（MD5）。 */
#include "CommonCrypto/CommonDigest.h"
#include <string.h>

/* ---------------- SHA-1 ---------------- */
typedef struct { uint32_t h[5]; uint64_t n; uint8_t buf[64]; size_t fill; } sha1_ctx;
static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static void sha1_block(sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t k, t;
        if (i < 20) { k = 0x5A827999; t = (b & d) | (~b & e); }
        else if (i < 40) { k = 0x6ED9EBA1; t = b ^ d ^ e; }
        else if (i < 60) { k = 0x8F1BBCDC; t = (b & d) | (b & e) | (d & e); }
        else { k = 0xCA62C1D6; t = b ^ d ^ e; }
        uint32_t tmp = rol32(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol32(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}
unsigned char *CC_SHA1(const void *data, CC_LONG len, unsigned char *md) {
    sha1_ctx c = {{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u}, 0, {0}, 0};
    const uint8_t *p = data; size_t n = len;
    while (n) {
        size_t take = 64 - c.fill; if (take > n) take = n;
        memcpy(c.buf + c.fill, p, take); c.fill += take; p += take; n -= take; c.n += take;
        if (c.fill == 64) { sha1_block(&c, c.buf); c.fill = 0; }
    }
    uint64_t bits = c.n * 8; uint8_t tail[72]; size_t tl = 0; tail[tl++] = 0x80;
    while ((c.fill + tl) % 64 != 56) tail[tl++] = 0;
    for (int i = 7; i >= 0; i--) tail[tl++] = (uint8_t)(bits >> (i * 8));
    { const uint8_t *q = tail; size_t m = tl; while (m) { size_t take = 64 - c.fill; if (take > m) take = m;
        memcpy(c.buf + c.fill, q, take); c.fill += take; q += take; m -= take; if (c.fill == 64) { sha1_block(&c, c.buf); c.fill = 0; } } }
    for (int i = 0; i < 5; i++) { md[i * 4] = (uint8_t)(c.h[i] >> 24); md[i * 4 + 1] = (uint8_t)(c.h[i] >> 16); md[i * 4 + 2] = (uint8_t)(c.h[i] >> 8); md[i * 4 + 3] = (uint8_t)c.h[i]; }
    return md;
}

/* ---------------- MD5 ---------------- */
typedef struct { uint32_t h[4]; uint64_t n; uint8_t buf[64]; size_t fill; } md5_ctx;
static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
static const int md5_r[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
static void md5_block(md5_ctx *c, const uint8_t *p) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (b & d) | (~b & e); g = i; }
        else if (i < 32) { f = (e & b) | (~e & d); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ d ^ e; g = (3 * i + 5) % 16; }
        else { f = d ^ (b | ~e); g = (7 * i) % 16; }
        uint32_t tmp = e; e = d; d = b; b = b + rol32(a + f + md5_k[i] + m[g], md5_r[i]); a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e;
}
unsigned char *CC_MD5(const void *data, CC_LONG len, unsigned char *md) {
    md5_ctx c = {{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u}, 0, {0}, 0};
    const uint8_t *p = data; size_t n = len;
    while (n) {
        size_t take = 64 - c.fill; if (take > n) take = n;
        memcpy(c.buf + c.fill, p, take); c.fill += take; p += take; n -= take; c.n += take;
        if (c.fill == 64) { md5_block(&c, c.buf); c.fill = 0; }
    }
    uint64_t bits = c.n * 8; uint8_t tail[72]; size_t tl = 0; tail[tl++] = 0x80;
    while ((c.fill + tl) % 64 != 56) tail[tl++] = 0;
    for (int i = 0; i < 8; i++) tail[tl++] = (uint8_t)(bits >> (i * 8));
    { const uint8_t *q = tail; size_t m = tl; while (m) { size_t take = 64 - c.fill; if (take > m) take = m;
        memcpy(c.buf + c.fill, q, take); c.fill += take; q += take; m -= take; if (c.fill == 64) { md5_block(&c, c.buf); c.fill = 0; } } }
    for (int i = 0; i < 4; i++) { md[i * 4] = (uint8_t)c.h[i]; md[i * 4 + 1] = (uint8_t)(c.h[i] >> 8); md[i * 4 + 2] = (uint8_t)(c.h[i] >> 16); md[i * 4 + 3] = (uint8_t)(c.h[i] >> 24); }
    return md;
}
