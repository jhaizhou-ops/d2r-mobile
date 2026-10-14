// 不用游戏文件或声卡：核对中置、环绕、单/双声道、正负饱和；期望值独立手算。
// No game files or audio device: check center, surround, mono/stereo and positive/negative saturation against independently calculated expectations.
#include "../dm_audio_mix.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    int16_t s[6] = {0}, d[2];
    s[0] = 10000; dm_audio_mix_frame(s,6,d); assert(d[0] == 5000 && d[1] == 0);
    s[0] = 0; s[1] = -10000; dm_audio_mix_frame(s,6,d); assert(d[0] == 0 && d[1] == -5000);
    s[1] = 0; s[3] = 10000; dm_audio_mix_frame(s,6,d); assert(d[0] == 2500 && d[1] == 2500);
    s[3] = 0;
    s[2] = 10000; dm_audio_mix_frame(s,6,d); assert(d[0] == 3535 && d[1] == 3535);
    s[2] = 0; s[4] = 10000; dm_audio_mix_frame(s,6,d); assert(d[0] == 3535 && d[1] == 0);
    s[4] = 0; s[5] = -10000; dm_audio_mix_frame(s,6,d); assert(d[0] == 0 && d[1] == -3535);
    s[0] = -32768; s[1] = 32767; dm_audio_mix_frame(s,2,d); assert(d[0] == -32768 && d[1] == 32767);
    dm_audio_mix_frame(s,1,d); assert(d[0] == -32768 && d[1] == -32768);
    for(int i=0;i<6;i++) s[i]=32767;
    dm_audio_mix_frame(s,6,d); assert(d[0] == 32767 && d[1] == 32767);
    for(int i=0;i<6;i++) s[i]=-32768;
    dm_audio_mix_frame(s,6,d); assert(d[0] == -32768 && d[1] == -32768);
    puts("Center, surround, mono, stereo and clipping cases passed");
}
