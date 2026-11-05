// 有界 ASTC 实验；不是游戏转换管线。
// Bounded ASTC experiment; not the game conversion pipeline.
// 基于 Arm astc-encoder 5.0.0 API 和原生 astc_pack.py 参数。
// Based on the Arm astc-encoder 5.0.0 API and native astc_pack.py settings.
#include "astcenc.h"
#include <cstddef>
#include <cstdint>

extern "C" {
// 版本 2 增加有界预设参数；旧二进制不得静默忽略 fast。
// Version 2 adds the bounded preset argument; old binaries must not silently ignore fast.
uint32_t astc_encoder_abi_version() { return 2; }
// 原始块字节，不含 .astc 容器头。零表示尺寸无效。
// Raw block bytes, without an .astc container header. Zero means invalid dimensions.
uint32_t astc_output_size(uint32_t width, uint32_t height) {
    if (!width || !height || width > 4096 || height > 4096 ||
        static_cast<uint64_t>(width) * height > 1048576)
        return 0;
    return ((width + 3) / 4) * ((height + 3) / 4) * 16;
}

// 调用者拥有有效、不重叠的缓冲区；配置 0 = 线性，1 = sRGB。
// Caller owns valid, nonoverlapping buffers; profile 0 = linear, 1 = sRGB.
// 预设 0 = thorough（默认），1 = fast；其他值拒绝。
// Preset 0 = thorough (default), 1 = fast; all other values are rejected.
// 成功返回 0，输入无效返回 -1，否则返回 100 + 上游错误码。
// Returns 0 on success, -1 on invalid input, or 100 + upstream error.
int astc_encode_rgba(const uint8_t *rgba, uint32_t rgba_size, uint32_t width, uint32_t height,
                     uint32_t profile, uint8_t *output, uint32_t output_size, uint32_t preset) {
    const uint32_t required = astc_output_size(width, height);
    if (!required || !rgba || !output || profile > 1 || preset > 1 || output_size != required ||
        rgba_size != static_cast<uint64_t>(width) * height * 4)
        return -1;
    astcenc_config config;
    auto status = astcenc_config_init(profile ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1,
                                      preset ? ASTCENC_PRE_FAST : ASTCENC_PRE_THOROUGH, 0, &config);
    if (status != ASTCENC_SUCCESS)
        return 100 + status;
    astcenc_context *context = nullptr;
    status = astcenc_context_alloc(&config, 1, &context);
    if (status != ASTCENC_SUCCESS)
        return 100 + status;
    void *slice = const_cast<uint8_t *>(rgba);
    astcenc_image image{width, height, 1, ASTCENC_TYPE_U8, &slice};
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    status = astcenc_compress_image(context, &image, &swizzle, output, output_size, 0);
    astcenc_context_free(context);
    return status == ASTCENC_SUCCESS ? 0 : 100 + status;
}
}
