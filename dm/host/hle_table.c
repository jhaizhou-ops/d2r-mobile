// libc 直通表：与 AAPCS64 一致、无结构布局依赖的 C 库函数，直接接苹果 libc。进 swhle 段。
// libc passthrough table: C library functions consistent with AAPCS64 and with no struct layout dependency, connected directly to Apple libc. Goes into the swhle section.
#include "dm.h"
#include <math.h>
#include <ctype.h>
#include <time.h>
#include "dm_sect.h"
typedef struct { const char *name; void *fn; } hle_ent;
#define L(n) DM_SECT(swhle) static const hle_ent hle_lib_##n = { #n, (void *)n };
L(memcpy) L(memset) L(memmove) L(memcmp) L(memchr) L(strlen) L(strnlen) L(strcmp) L(strncmp)
L(strcpy) L(strncpy) L(strcat) L(strncat) L(strchr) L(strrchr) L(strstr) L(strspn) L(strcspn)
L(strpbrk) L(strcasecmp) L(strncasecmp) L(atoi) L(atof)
#ifdef _WIN32
// 客体是 LP64，Windows 是 LLP64：long 的转换不能直接透传。
// The guest is LP64 and Windows is LLP64: long conversions cannot pass through.
DM_SECT(swhle) static const hle_ent hle_lib_strtol = {"strtol", (void *)strtoll};
DM_SECT(swhle) static const hle_ent hle_lib_strtoul = {"strtoul", (void *)strtoull};
DM_SECT(swhle) static const hle_ent hle_lib_lroundf = {"lroundf", (void *)llroundf};
#else
L(strtol) L(strtoul) L(lroundf)
#endif
L(strtoll) L(strtoull)
L(strtod) L(strtof) L(sqrtf) L(cbrtf) L(expf) L(acosf) L(cosf) L(sinf) L(atan2f) L(fmodf) L(cos)
L(sin) L(powf) L(atanf) L(tanf) L(asinf) L(log10) L(pow) L(logf) L(ldexpf) L(log10f) L(log)
L(ldexp) L(frexp) L(atan2) L(modff) L(exp2f) L(exp2) L(log2f) L(log2)
#ifdef _WIN32
#include "../windows/guest_libc.h"
#define T(n) DM_SECT(swhle) static const hle_ent hle_lib_##n = {#n, (void *)dm_guest_##n};
T(mktime) T(timegm) T(gmtime_r) T(localtime_r) T(strftime)
#undef T
#else
L(mktime) L(timegm) L(gmtime_r) L(localtime_r) L(strftime)
#endif
