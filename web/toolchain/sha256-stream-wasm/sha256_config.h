/* 仅启用固定 mbedTLS SHA-256 实现；不启用平台、TLS、替代后端或自检 I/O。
 * Enable only pinned mbedTLS SHA-256; no platform, TLS, alternate backend or self-test I/O. */
#ifndef D2R_SHA256_CONFIG_H
#define D2R_SHA256_CONFIG_H
#define MBEDTLS_SHA256_C
#endif
