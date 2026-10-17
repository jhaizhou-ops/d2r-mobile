#ifndef XBLR_RUNTIME_H
#define XBLR_RUNTIME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef _WIN32
#define XBLR_API __declspec(dllexport)
#else
#define XBLR_API __attribute__((visibility("default")))
#endif
#define XBLR_ABI_VERSION 1u
enum xblr_status {
    XBLR_OK = 0, XBLR_BAD_ARGUMENT = 1, XBLR_BAD_FORMAT = 2,
    XBLR_REAL_NSO_NOT_READY = 3, XBLR_BUSY = 4, XBLR_LAYOUT_MISMATCH = 5,
    XBLR_ABI_MISMATCH = 6, XBLR_RESULT_MISMATCH = 7, XBLR_VERSION_MISMATCH = 8
};
enum xblr_input_kind { XBLR_UNKNOWN = 0, XBLR_ORIGINAL_NSO = 1, XBLR_EMBEDDED_SYNTHETIC = 2 };
enum xblr_module_role { XBLR_MAIN = 1, XBLR_SDK = 2 };
typedef struct xblr_layout {
    uint32_t abi_version, struct_bytes;
    uint32_t input_kind, reserved;
    uint64_t image_base, text_offset, text_bytes, ro_offset, ro_bytes, data_offset, data_bytes;
    uint64_t entry_offset, callback_offset;
} xblr_layout;
typedef struct xblr_input_info {
    uint32_t abi_version, struct_bytes, input_kind, role;
    uint32_t nso_flags, header_only, main_id_prefix_matches, execution_allowed;
    uint8_t build_id[32];
    uint64_t file_bytes, image_bytes;
} xblr_input_info;
typedef struct xblr_result {
    uint32_t abi_version, struct_bytes, input_kind, status;
    uint64_t seed, actual, expected, abi_bad_mask;
    uint64_t host_calls, guest_callbacks, virtual_x18_before, virtual_x18_after;
    uint64_t tls_before, tls_after, host_observed_tls, tlr_value;
    uint64_t guest_before_host, guest_after_host, embedded_runs, embedded_last;
} xblr_result;
/* Output sizes are explicit: callers must pass sizeof(the exact v1 structure).
 * No API accepts a function pointer, executable byte buffer, or original NSO entry.
 * Run only on a worker thread. Calls are synchronous and bounded; concurrent run returns BUSY.
 */
XBLR_API int xblr_get_layout(xblr_layout *out, size_t out_bytes);
/* Read-only header preflight. A valid NSO still returns REAL_NSO_NOT_READY.
 * Not decompression, SHA verification, rewriting, linking, or permission to execute.
 */
XBLR_API int xblr_probe_input(const void *bytes, size_t length, uint32_t role,
                            xblr_input_info *out, size_t out_bytes);
XBLR_API int xblr_run(uint32_t input_kind, uint64_t seed, xblr_result *out, size_t out_bytes);
#ifdef __cplusplus
}
#endif
#endif
