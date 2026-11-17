#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable result values. JNI may map MISSING to -9 and INVALID to -10.
 * Missing directory/manifest is distinct from an installed but invalid overlay.
 * Validation is read-only: no repairs, process exit, save or resource writes.
 */
typedef enum {
    DM_OVERLAY_OK = 0,
    DM_OVERLAY_MISSING = 1,
    DM_OVERLAY_INVALID = 2
} dm_overlay_result;

dm_overlay_result dm_overlay_validate(const char *directory,
                                      const char *expected_source_sha256,
                                      char *reason, size_t reason_capacity);
/* 完整manifest验证通过后检查安卓绘制所需的固定资源；只读，不补写文件。
 * After full manifest verification, require Android's fixed drawing assets; read-only, never repairs files. */
dm_overlay_result dm_overlay_require_touch_art(const char *directory, char *reason, size_t reason_capacity);

/* 成功时返回经校验的「变体:基础覆盖层」路径。 | On success, returns the validated variant:base overlay path. */
int dm_overlay_variant(const char *overlay, const char *source_sha256, int (*enabled)(const char *),
                       char *out, size_t n);

#ifdef __cplusplus
}
#endif
