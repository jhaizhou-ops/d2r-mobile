// 时间 ABI 转换，依据 Horizon LP64 布局与 UCRT 的 64 位时间 API。
// Time ABI conversion, based on Horizon LP64 layouts and UCRT 64-bit time APIs.
#include "guest_libc.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Static_assert(sizeof(dm_guest_tm) == 56, "guest tm must be 56 bytes");
static struct tm to_host(const dm_guest_tm *value) {
    return (struct tm){.tm_sec = value->tm_sec, .tm_min = value->tm_min, .tm_hour = value->tm_hour, .tm_mday = value->tm_mday,
        .tm_mon = value->tm_mon, .tm_year = value->tm_year, .tm_wday = value->tm_wday, .tm_yday = value->tm_yday, .tm_isdst = value->tm_isdst};
}
static void to_guest(const struct tm *value, int64_t seconds, int local, dm_guest_tm *out) {
    *out = (dm_guest_tm){value->tm_sec, value->tm_min, value->tm_hour, value->tm_mday, value->tm_mon, value->tm_year,
        value->tm_wday, value->tm_yday, value->tm_isdst, 0, 0, "UTC"};
    if (local) {
        struct tm utc = *value;
        out->tm_gmtoff = _mkgmtime64(&utc) - seconds;
        _tzset(); out->tm_zone = _tzname[value->tm_isdst > 0 ? 1 : 0];
    }
}
int64_t dm_guest_mktime(dm_guest_tm *value) {
    struct tm host = to_host(value);
    int64_t seconds = _mktime64(&host); to_guest(&host, seconds, 1, value); return seconds;
}
int64_t dm_guest_timegm(dm_guest_tm *value) {
    struct tm host = to_host(value);
    int64_t seconds = _mkgmtime64(&host); to_guest(&host, seconds, 0, value); return seconds;
}
dm_guest_tm *dm_guest_gmtime_r(const int64_t *seconds, dm_guest_tm *out) {
    struct tm host;
    if (_gmtime64_s(&host, seconds)) return NULL;
    to_guest(&host, *seconds, 0, out); return out;
}
dm_guest_tm *dm_guest_localtime_r(const int64_t *seconds, dm_guest_tm *out) {
    struct tm host;
    if (_localtime64_s(&host, seconds)) return NULL;
    to_guest(&host, *seconds, 1, out); return out;
}
size_t dm_guest_strftime(char *out, size_t size, const char *format, const dm_guest_tm *value) {
    struct tm host = to_host(value);
    const char *zone = value->tm_zone ? value->tm_zone : "";
    size_t n = strlen(format), z = strlen(zone);
    if (z > SIZE_MAX / 2 - 32 || n > SIZE_MAX / (z * 2 + 32) - 1) return 0;
    char *expanded = malloc((n + 1) * (z * 2 + 32));
    if (!expanded) return 0;
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (format[i] == '%' && format[i + 1] == '%') {
            expanded[used++] = '%'; expanded[used++] = '%'; i++;
        } else if (format[i] == '%' && format[i + 1] == 'Z') {
            for (size_t j = 0; j < z; j++) { if (zone[j] == '%') expanded[used++] = '%'; expanded[used++] = zone[j]; }
            i++;
        } else if (format[i] == '%' && format[i + 1] == 'z') {
            int64_t offset = value->tm_gmtoff;
            if (offset <= -86400 || offset >= 86400) { free(expanded); return 0; }
            expanded[used++] = offset < 0 ? '-' : '+';
            if (offset < 0) offset = -offset;
            used += (size_t)sprintf(expanded + used, "%02d%02d", (int)(offset / 3600), (int)(offset % 3600 / 60)); i++;
        } else expanded[used++] = format[i];
    }
    expanded[used] = 0;
    size_t result = strftime(out, size, expanded, &host);
    free(expanded); return result;
}
