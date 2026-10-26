// Switch LP64 的时间结构，独立于 Windows CRT 的 struct tm。
// Switch LP64 time structures, independent of the Windows CRT struct tm.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct {
    int32_t tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst, padding;
    int64_t tm_gmtoff;
    const char *tm_zone;
} dm_guest_tm;
int64_t dm_guest_mktime(dm_guest_tm *value);
int64_t dm_guest_timegm(dm_guest_tm *value);
dm_guest_tm *dm_guest_gmtime_r(const int64_t *seconds, dm_guest_tm *out);
dm_guest_tm *dm_guest_localtime_r(const int64_t *seconds, dm_guest_tm *out);
size_t dm_guest_strftime(char *out, size_t size, const char *format, const dm_guest_tm *value);
