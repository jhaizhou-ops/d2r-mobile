#include "../host/hle.h"
/* Both original SDK decoder classes implement the same ten virtual methods
 * and nn::codec packet framing. Execute the SDK's bundled libopus software
 * implementation on ARM64 instead of the unavailable Horizon hardware IPC. */
HLE_FORCE("_ZN2nn5codec19HardwareOpusDecoderC1Ei", "Android executes original SDK OpusDecoder/libopus with matching decoder virtual interface", void,android_opus_constructor,void *self,int option) {
    (void)option;
    void (*construct)(void *)=guest_sym("_ZN2nn5codec11OpusDecoderC1Ev");
    if(!self || !construct)die("Android original software Opus constructor unavailable");
    memset(self,0,0x40);construct(self);
    LOG("[opus-android] original SDK software OpusDecoder constructed object=%p",self);
}
