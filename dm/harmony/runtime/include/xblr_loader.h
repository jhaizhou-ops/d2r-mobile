#ifndef XBLR_LOADER_H
#define XBLR_LOADER_H
#include "xblr_runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { XBLR_IMAGE_ORIGINAL_UNREWRITTEN = 1, XBLR_IMAGE_APPLE_PATCHED = 2,
       XBLR_IMAGE_HARMONY_REWRITTEN = 3 };
enum { XBLR_LOAD_OK=0, XBLR_LOAD_ARGUMENT=20, XBLR_LOAD_FORMAT=21, XBLR_LOAD_MEMORY=22,
       XBLR_LOAD_MISSING_IMPORT=23, XBLR_LOAD_UNSUPPORTED_RELOCATION=24,
       XBLR_LOAD_READONLY=25, XBLR_LOAD_APPLE_ABI=26, XBLR_LOAD_NEEDS_HARMONY_REWRITE=27 };
/* Content verified offline against full NSO/segment SHA; this API parses/binds,
 * never treats this descriptor as permission to execute an original instruction.
 * The caller owns base throughout module lifetime. readonly=0 only for private RW staging.
 */
typedef struct xblr_image_desc {
    char name[16];
    const uint8_t *base;
    uint64_t size, text_bytes, ro_offset, ro_bytes, data_offset, data_bytes, bss_bytes;
    uint32_t source_kind, readonly;
    uint8_t source_sha256[32], build_id[32];
} xblr_image_desc;
typedef struct xblr_module xblr_module;
typedef struct xblr_module_info {
    uint64_t image_bytes, symbol_count, defined_symbols, undefined_symbols, relocation_count;
    uint64_t mod0_offset, dynamic_offset, init_offset, init_array_offset, init_array_bytes;
    uint32_t source_kind, execution_allowed;
} xblr_module_info;
typedef struct xblr_hle_binding { const char *name; uint64_t address; uint32_t force; } xblr_hle_binding;
typedef struct xblr_link_report {
    uint64_t relocations, relative, absolute, global_data, jump_slot;
    uint64_t module_bindings, hle_bindings, weak_zero, missing_imports, unsupported, invalid_targets, writes_applied;
} xblr_link_report;
typedef void (*xblr_missing_import_fn)(void *user, const char *module, const char *symbol, uint64_t offset, uint32_t type);
XBLR_API int xblr_bind_image(const xblr_image_desc *image, xblr_module **out, xblr_module_info *info);
XBLR_API void xblr_release_module(xblr_module *module);
XBLR_API int xblr_module_symbol(const xblr_module *module, const char *name, uint64_t *address);
/* Both modules must be bound before resolution. force entries override defined
 * symbols, then own/module definitions, then ordinary HLE. Missing strong imports
 * abort the whole transaction. Only data/BSS is writable, never code or RO.
 * apply=0 produces actual binding counts without writes. apply=1 precomputes all
 * patches and commits only when every resolution and writable target is valid.
 */
XBLR_API int xblr_link_modules(xblr_module *const *modules, size_t module_count,
    const xblr_hle_binding *hle, size_t hle_count, int apply,
    xblr_missing_import_fn missing, void *user, xblr_link_report *report);
/* There is deliberately no call-entry function for unrevised original/Apple code. */
XBLR_API int xblr_module_execution_status(const xblr_module *module);
typedef struct xblr_originals_report {
    uint32_t abi_version, struct_bytes, bind_status, link_status, execution_status, reserved;
    xblr_module_info modules[2];
    xblr_link_report link;
    uint64_t nnmain_offset;
    char first_missing_symbol[160];
} xblr_originals_report;
/* Available in the optional libxblr_originals.so target containing verified,
 * READ-ONLY, NON-EXECUTABLE original module images. No game code is called.
 */
XBLR_API int xblr_prepare_originals(xblr_originals_report *out, size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
