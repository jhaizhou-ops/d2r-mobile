// 给鸿蒙主程序worker调用；四轮都是真实调用共享库，禁止把expected当执行结果返回。
// 本文件已交叉编译为OBJECT验证接口，但未在设备执行。接入方保存四个完整result。
#include "xblr_runtime.h"
int xblr_example_run(xblr_result *rows, size_t row_count, xblr_layout *layout)
{
    static const uint64_t seeds[4] = { 0, 1, UINT64_MAX, UINT64_C(0x0123456789abcdef) };
    static const uint64_t expected[4] = {
        UINT64_C(0x920157e233c09341), UINT64_C(0x920157e233c09326),
        UINT64_C(0xa291a6b282d1e288), UINT64_C(0x90a2889d44ed29d8) };
    if (!rows || row_count != 4 || !layout) return XBLR_BAD_ARGUMENT;
    int rc = xblr_get_layout(layout, sizeof *layout);
    if (rc != XBLR_OK) return rc;
    xblr_result rejected;
    rc = xblr_run(XBLR_ORIGINAL_NSO, 0, &rejected, sizeof rejected);
    if (rc != XBLR_REAL_NSO_NOT_READY || rejected.actual != 0) return XBLR_RESULT_MISMATCH;
    for (size_t i = 0; i < 4; ++i) {
        rc = xblr_run(XBLR_EMBEDDED_SYNTHETIC, seeds[i], &rows[i], sizeof rows[i]);
        if (rc != XBLR_OK) return rc;
        if (rows[i].actual != expected[i] || rows[i].abi_bad_mask != 0 || rows[i].host_calls != 1 ||
            rows[i].guest_callbacks != 1) return XBLR_RESULT_MISMATCH;
    }
    return XBLR_OK;
}
