#include "../host/hle.h"
#include <time.h>

/* Android has no Tegra video device. The original MediaCodec start reply can
 * precede its actual driver error callback. Wait for this decoder's own error
 * or input callback before reporting successful startup to the original game.
 * All packet/frame handling, callback events and destruction remain original.
 * Layout is verified against build91738 DecoderImpl's constructor and callback.
 */
HLE_FORCE("_ZN5movie7Decoder5StartEv",
          "Wait for the original decoder's real asynchronous startup outcome",
          int32_t, android_movie_start, void *self) {
    int32_t (*start)(void *)=guest_sym("_ZN5movie7Decoder5StartEv");
    int32_t (*last_error)(void *)=guest_sym("_ZN5movie7Decoder12GetLastErrorEv");
    void (*lock)(void *)=guest_sym("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE");
    void (*unlock)(void *)=guest_sym("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
    if(!start || !last_error || !lock || !unlock)die("Android movie startup missing original SDK functions");
    uint8_t *impl=*(uint8_t **)((uint8_t *)self+8);
    if(!impl) return start(self);
    lock(impl+0x60);
    uintptr_t prior_capacity=*(uintptr_t *)(impl+0xb0);
    unlock(impl+0x60);
    int32_t result=start(self);
    LOG("MOVIE_START original decoder=%p impl=%p result=%#x",self,impl,(unsigned)result);
    if(result) return result;
    uint64_t deadline=now_ns()+5000000000ull;
    for(;;) {
        result=last_error(self);
        if(result) {
            LOG("MOVIE_START actual_async_error decoder=%p result=%#x",self,(unsigned)result);
            return result;
        }
        lock(impl+0x60);
        uintptr_t begin=*(uintptr_t *)(impl+0xa0),capacity=*(uintptr_t *)(impl+0xb0);
        unlock(impl+0x60);
        /* Capacity persists after input indices are consumed by the game's
         * decoder thread. Only a fresh allocation proves a callback this start.
         */
        if(!prior_capacity && begin && capacity>begin) {
            LOG("MOVIE_START actual_input_ready decoder=%p",self);
            return 0;
        }
        if(now_ns()>=deadline)die("Android movie startup has no verified new input callback or original error decoder=%p prior_capacity=%p",self,(void *)prior_capacity);
        struct timespec interval={.tv_nsec=1000000};
        nanosleep(&interval,NULL);
    }
}
