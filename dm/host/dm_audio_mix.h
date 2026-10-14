// 将交错 PCM16 的六声道 L/R/C/LFE/LS/RS 混为立体声，保留中置对话；依据已验证的 iOS 音频实验，非游戏内容。
// Downmix interleaved PCM16 L/R/C/LFE/LS/RS to stereo, retaining center dialogue; based on the verified iOS audio experiment, with no game content.
#pragma once
#include <stdint.h>
static inline int16_t dm_audio_clip(int32_t v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}
static inline void dm_audio_mix_frame(const int16_t *s, int ch, int16_t *d) {
    if (ch == 1) { d[0] = d[1] = s[0]; return; }
    if (ch == 2) { d[0] = s[0]; d[1] = s[1]; return; }
    // 总体留 1/2 余量；中置/环绕 -3 dB，低频 -6 dB。乘加用 64 位，避免削波前先整数溢出。
    // Overall 1/2 headroom; center/surround -3 dB, LFE -6 dB. Accumulate in 64 bits to avoid overflow before clipping.
    d[0] = dm_audio_clip(((int64_t)s[0] * 32768 + (int64_t)s[2] * 23170 +
                       (int64_t)s[4] * 23170 + (int64_t)s[3] * 16384) / 65536);
    d[1] = dm_audio_clip(((int64_t)s[1] * 32768 + (int64_t)s[2] * 23170 +
                       (int64_t)s[5] * 23170 + (int64_t)s[3] * 16384) / 65536);
}
