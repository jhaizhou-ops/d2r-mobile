// 只读NSO头门。真实模块无执行入口，不把头部识别当版本/内容真实性验收。
#include "xblr_runtime.h"
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
int xblr_probe_input(const void *bytes, size_t length, uint32_t role,
                     xblr_input_info *out, size_t out_bytes)
{
    if (!out || out_bytes != sizeof *out || (!bytes && length) || (role != XBLR_MAIN && role != XBLR_SDK))
        return XBLR_BAD_ARGUMENT;
    *out = (xblr_input_info){ .abi_version = XBLR_ABI_VERSION, .struct_bytes = sizeof *out,
        .role = role, .header_only = 1, .file_bytes = length };
    if (length < 0x100) return XBLR_BAD_FORMAT;
    const uint8_t *p = bytes;
    if (p[0] != 'N' || p[1] != 'S' || p[2] != 'O' || p[3] != '0') return XBLR_BAD_FORMAT;
    out->input_kind = XBLR_ORIGINAL_NSO;
    out->nso_flags = le32(p + 0xc);
    if (le32(p + 4) != 0 || (out->nso_flags & ~63u)) return XBLR_BAD_FORMAT;
    for (unsigned i = 0; i < 32; ++i) out->build_id[i] = p[0x40 + i];
    uint64_t start[3], end[3], file_start[3], file_end[3];
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned h = 0x10 + i * 0x10;
        uint32_t memory_size = le32(p + h + 8);
        uint32_t stored = (out->nso_flags & (1u << i)) ? le32(p + 0x60 + i * 4) : memory_size;
        file_start[i] = le32(p + h); file_end[i] = file_start[i] + stored;
        start[i] = le32(p + h + 4); end[i] = start[i] + memory_size;
        if (!memory_size || !stored || file_start[i] < 0x100 || file_end[i] > length || end[i] > UINT32_MAX)
            return XBLR_BAD_FORMAT;
        for (unsigned j = 0; j < i; ++j)
            if ((start[i] < end[j] && start[j] < end[i]) ||
                (file_start[i] < file_end[j] && file_start[j] < file_end[i])) return XBLR_BAD_FORMAT;
        if (end[i] > out->image_bytes) out->image_bytes = end[i];
    }
    // This initial contract supports the ordinary text->RO->data/bss NSO layout only.
    if (start[0] != 0 || end[0] > start[1] || end[1] > start[2]) return XBLR_BAD_FORMAT;
    out->image_bytes = end[2] + le32(p + 0x3c);
    if (out->image_bytes > UINT32_MAX) return XBLR_BAD_FORMAT;
    static const uint8_t main_prefix[16] = {
        0xdb,0x05,0x07,0xf3,0x6b,0x84,0x20,0x0a,0xc0,0x5f,0x75,0xd7,0x6e,0xd5,0x02,0x6f };
    uint32_t different = 0;
    for (unsigned i = 0; i < 16; ++i) different |= out->build_id[i] ^ main_prefix[i];
    out->main_id_prefix_matches = !different;
    if (role == XBLR_MAIN && different) return XBLR_VERSION_MISMATCH;
    return XBLR_REAL_NSO_NOT_READY;
}
