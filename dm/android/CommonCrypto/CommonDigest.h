/* Android 垫片：CommonCrypto/CommonDigest.h 的最小面（dm_padmode 用 SHA-1；diag 用 MD5）。
 * 只提供本移植用到的函数与常量；实现在 ../commoncrypto_shim.c。 */
#ifndef DM_ANDROID_COMMON_DIGEST_SHIM_H
#define DM_ANDROID_COMMON_DIGEST_SHIM_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t CC_LONG;
#define CC_SHA1_DIGEST_LENGTH 20
#define CC_MD5_DIGEST_LENGTH 16
unsigned char *CC_SHA1(const void *data, CC_LONG len, unsigned char *md);
unsigned char *CC_MD5(const void *data, CC_LONG len, unsigned char *md);
#endif
