/* Android implementation of nn::audio AudioOut, included by hle_audio.c.
 * AAudio owns the render clock. There is no timer/null-sink success fallback.
 * The callback copies the original interleaved PCM16 channel layout unchanged.
 * Explicit mute/background mute still uses a real started AAudio stream.
 * SDK SystemEvent creation stays on the calling guest thread; the ordinary
 * notifier only signals the already-bound host kernel write handle.
 */
#include "audio_aaudio.h"
#include <aaudio/AAudio.h>
#include <errno.h>
#include <limits.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };
typedef struct { void *ptr; uint64_t cap, size; } gbuf;
#define QMAX 64
#define RELMAX QMAX
#define AOMAX 8

typedef struct {
    pthread_mutex_t m, control;
    void *event;
    int ch, muted;
    _Atomic int state, closing;
    gbuf *q[QMAX], *rel[RELMAX];
    _Atomic uint32_t q_head, q_tail, rel_head, rel_tail;
    uint64_t pos;
    AAudioStream *stream;
    sem_t notify_sem;
    pthread_t notify_th;
    _Atomic int notify_run, notify_pending, notify_armed, stream_error, queue_fault;
    uint64_t recovery_window_ns;
    unsigned recovery_attempts;
} aout;

static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_suspended, g_last_error;
static _Atomic uint64_t g_render_calls, g_device_frames, g_game_bytes;
static _Atomic uint64_t g_released, g_release_events, g_underrun, g_nonzero;
static _Atomic uint64_t g_error_callbacks, g_recoveries;

static aout *ao(const void *g) {
    aout *a = objmap_get(g);
    if (!a) die("AudioOut %p is not a host object", g);
    return a;
}

static void wake_notify(aout *a) {
    if (!atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel))
        sem_post(&a->notify_sem);
}
static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    wake_notify(a);
}
static void sink_failure(aout *a, int error) {
    atomic_store_explicit(&a->stream_error, error, memory_order_release);
    atomic_store_explicit(&g_last_error, error, memory_order_release);
    wake_notify(a);
}

/* Single AAudio consumer; append/reclaim are serialized on a->m. */
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    uint64_t missing = 0, copied = 0;
    int released = 0;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) {
            memset(dst, 0, (size_t)n);
            if (atomic_load_explicit(&a->state, memory_order_relaxed) == STATE_STARTED) missing = n;
            break;
        }
        gbuf *b = a->q[qh & (QMAX - 1)];
        if (a->pos > b->size || b->size > b->cap || (b->size && !b->ptr)) {
            memset(dst, 0, (size_t)n); atomic_store_explicit(&a->queue_fault, 1, memory_order_release);
            sink_failure(a, AAUDIO_ERROR_ILLEGAL_ARGUMENT); break;
        }
        uint64_t k = b->size - a->pos;
        if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, (size_t)k);
        dst += k; n -= k; a->pos += k; copied += k;
        if (a->pos == b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) {
                memset(dst, 0, (size_t)n); atomic_store_explicit(&a->queue_fault, 2, memory_order_release);
                sink_failure(a, AAUDIO_ERROR_INTERNAL); break;
            }
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            ++qh;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released = 1;
            atomic_fetch_add_explicit(&g_released, 1, memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(&g_game_bytes, copied, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_underrun, missing / (2u * (unsigned)a->ch), memory_order_relaxed);
    if (released) notify_release(a);
}

static aaudio_data_callback_result_t audio_render(AAudioStream *stream, void *ctx, void *data, int32_t frames) {
    (void)stream;
    aout *a = ctx;
    if (frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    uint64_t size = (uint64_t)(uint32_t)frames * 2u * (unsigned)a->ch;
    atomic_fetch_add_explicit(&g_render_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_device_frames, (uint32_t)frames, memory_order_relaxed);
    pull(a, data, size);
    if (a->muted || atomic_load_explicit(&g_suspended, memory_order_relaxed)) memset(data, 0, (size_t)size);
    else {
        const int16_t *samples = data;
        uint64_t nonzero = 0;
        for (uint64_t i = 0; i < size / 2; ++i) nonzero += samples[i] != 0;
        atomic_fetch_add_explicit(&g_nonzero, nonzero, memory_order_relaxed);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_error(AAudioStream *stream, void *ctx, aaudio_result_t error) {
    (void)stream;
    atomic_fetch_add_explicit(&g_error_callbacks, 1, memory_order_relaxed);
    /* Never stop/close/open or enter guest code from AAudio's callback thread. */
    sink_failure(ctx, error);
}

static int stream_open(aout *a) {
    int result = AAUDIO_ERROR_UNAVAILABLE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        AAudioStreamBuilder *builder = NULL;
        result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK) return result;
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder, RATE);
        AAudioStreamBuilder_setChannelCount(builder, a->ch);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setPerformanceMode(builder, attempt ? AAUDIO_PERFORMANCE_MODE_NONE : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, audio_render, a);
        AAudioStreamBuilder_setErrorCallback(builder, audio_error, a);
        result = AAudioStreamBuilder_openStream(builder, &a->stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK) { a->stream = NULL; continue; }
        if (AAudioStream_getSampleRate(a->stream) != RATE || AAudioStream_getChannelCount(a->stream) != a->ch ||
            AAudioStream_getFormat(a->stream) != AAUDIO_FORMAT_PCM_I16) {
            AAudioStream_close(a->stream); a->stream = NULL; return AAUDIO_ERROR_INVALID_FORMAT;
        }
        int burst = AAudioStream_getFramesPerBurst(a->stream);
        if (burst > 0 && burst <= INT_MAX / 2) AAudioStream_setBufferSizeInFrames(a->stream, burst * 2);
        LOG("AAudio opened PCM16 %d Hz %d channels device=%d burst=%d performance=%d", RATE, a->ch,
            AAudioStream_getDeviceId(a->stream), burst, AAudioStream_getPerformanceMode(a->stream));
        return AAUDIO_OK;
    }
    return result;
}

/* Caller owns control; all stream state changes use this same lock. */
static int wait_stream(aout *a, aaudio_stream_state_t wanted) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t limit = (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec + 1000000000ll;
    for (;;) {
        aaudio_stream_state_t state = AAudioStream_getState(a->stream), next;
        if (state == wanted) return AAUDIO_OK;
        if (state == AAUDIO_STREAM_STATE_DISCONNECTED) return AAUDIO_ERROR_DISCONNECTED;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t left = limit - ((int64_t)now.tv_sec * 1000000000ll + now.tv_nsec);
        if (left <= 0) return AAUDIO_ERROR_TIMEOUT;
        int result = AAudioStream_waitForStateChange(a->stream, state, &next, left);
        if (result != AAUDIO_OK) return result;
    }
}
static int stream_start(aout *a) {
    int result = AAudioStream_requestStart(a->stream);
    return result == AAUDIO_OK ? wait_stream(a, AAUDIO_STREAM_STATE_STARTED) : result;
}
static int stream_pause(aout *a) {
    if (AAudioStream_getState(a->stream) == AAUDIO_STREAM_STATE_PAUSED) return AAUDIO_OK;
    int result = AAudioStream_requestPause(a->stream);
    return result == AAUDIO_OK ? wait_stream(a, AAUDIO_STREAM_STATE_PAUSED) : result;
}

static void *event_notify(void *ctx) {
    aout *a = ctx;
    for (;;) {
        int rc;
        do { rc = sem_wait(&a->notify_sem); } while (rc && errno == EINTR);
        if (rc) die("AAudio notify sem_wait: %d", errno);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        int error = atomic_exchange_explicit(&a->stream_error, 0, memory_order_acq_rel);
        if (error) {
            int fault = atomic_load_explicit(&a->queue_fault, memory_order_acquire);
            if (fault) die("AAudio guest queue invariant fault=%d; device reopen cannot repair guest data", fault);
            pthread_mutex_lock(&a->control);
            if (!atomic_load_explicit(&a->closing, memory_order_acquire)) {
                uint64_t now = now_ns();
                if (now - a->recovery_window_ns > 1000000000ull) { a->recovery_window_ns = now; a->recovery_attempts = 0; }
                if (++a->recovery_attempts > 4) die("AAudio device disconnects repeatedly; error=%d", error);
                LOG("AAudio error %d (%s), reopening actual device", error, AAudio_convertResultToText(error));
                if (a->stream) { AAudioStream_close(a->stream); a->stream = NULL; }
                int result = stream_open(a);
                if (result == AAUDIO_OK && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) result = stream_start(a);
                if (result != AAUDIO_OK) die("AAudio recovery failed %d (%s)", result, AAudio_convertResultToText(result));
                atomic_fetch_add_explicit(&g_recoveries, 1, memory_order_relaxed);
            }
            pthread_mutex_unlock(&a->control);
        }
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event) {
            hle_signal_system_event(a->event);
            atomic_fetch_add_explicit(&g_release_events, 1, memory_order_relaxed);
        }
        atomic_store_explicit(&a->notify_armed, 0, memory_order_release);
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) || atomic_load_explicit(&a->stream_error, memory_order_acquire)) wake_notify(a);
    }
    return NULL;
}

uint64_t dm_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }

HLE_FORCE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", "original PCM output parameter defaults", void, h_au_InitParam, uint8_t *p) { memset(p, 0, 8); }
HLE_FORCE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "Android primary output device", int, h_au_List, char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256); strcpy(info, "DeviceOut"); return 1;
}
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", "actual Android AAudio PCM16 sink; original SDK buffer-release event", Result, h_au_Open, void *g, void *ev, const char *name, const uint8_t *param) {
    int ch = *(const uint16_t *)(param + 4), rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if ((rate && rate != RATE) || ch < 1 || ch > 8) { LOG("AAudio unsupported requested rate=%d channels=%d", rate, ch); return RES_FAIL; }
    aout *a = calloc(1, sizeof *a);
    if (!a) return RES_FAIL;
    pthread_mutex_init(&a->m, NULL); pthread_mutex_init(&a->control, NULL);
    a->ch = ch;
    const char *mute = getenv("DM_AUDIO"); a->muted = mute && !atoi(mute);
    atomic_init(&a->state, STATE_STOPPED); atomic_init(&a->closing, 0);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0); atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->notify_run, 1); atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0); atomic_init(&a->stream_error, 0); atomic_init(&a->queue_fault, 0);
    if (sem_init(&a->notify_sem, 0, 0)) die("AAudio sem_init: %d", errno);
    int result = stream_open(a);
    if (result != AAUDIO_OK) {
        LOG("AAudio open failed %d (%s)", result, AAudio_convertResultToText(result));
        atomic_store(&g_last_error, result); sem_destroy(&a->notify_sem);
        pthread_mutex_destroy(&a->control); pthread_mutex_destroy(&a->m); free(a); return RES_FAIL;
    }
    if (pthread_create(&a->notify_th, NULL, event_notify, a)) die("AAudio notifier pthread_create");
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1;
    for (int i = 0; i < AOMAX; ++i) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("AudioOut simultaneous output limit %d", AOMAX);
    /* This is the only place that executes SDK event creation, on the caller. */
    if (ev) hle_bind_system_event(ev, 1);
    a->event = ev; objmap_put(g, a); g_aos[slot] = a;
    pthread_mutex_unlock(&g_aos_lock);
    LOG("audio::OpenAudioOut %s channels=%d actual AAudio%s", name ? name : "default", ch, a->muted ? " (explicit mute)" : "");
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "start real AAudio stream", Result, h_au_Start, void *g) {
    aout *a = ao(g); pthread_mutex_lock(&a->control);
    if (atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) { pthread_mutex_unlock(&a->control); return RES_OK; }
    /* Publish started before requestStart because callbacks may begin there. */
    atomic_store_explicit(&a->state, STATE_STARTED, memory_order_release);
    int result = stream_start(a);
    if (result != AAUDIO_OK) { atomic_store_explicit(&a->state, STATE_STOPPED, memory_order_release); atomic_store(&g_last_error, result); LOG("AAudio start failed %d (%s)", result, AAudio_convertResultToText(result)); }
    pthread_mutex_unlock(&a->control); return result == AAUDIO_OK ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "stop real AAudio callbacks; retain original queue positions", void, h_au_Stop, void *g) {
    aout *a = ao(g); pthread_mutex_lock(&a->control);
    int was = atomic_exchange_explicit(&a->state, STATE_STOPPED, memory_order_acq_rel);
    if (was == STATE_STARTED) {
        int result = stream_pause(a);
        if (result != AAUDIO_OK && result != AAUDIO_ERROR_DISCONNECTED) die("AAudio pause failed %d (%s)", result, AAudio_convertResultToText(result));
    }
    pthread_mutex_unlock(&a->control);
}
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "close actual AAudio stream and join notifier before freeing object", void, h_au_Close, void *g) {
    aout *a = ao(g);
    pthread_mutex_lock(&g_aos_lock); pthread_mutex_lock(&a->control);
    atomic_store_explicit(&a->closing, 1, memory_order_release); atomic_store_explicit(&a->state, STATE_STOPPED, memory_order_release);
    if (a->stream) { AAudioStream_close(a->stream); a->stream = NULL; }
    atomic_store_explicit(&a->notify_run, 0, memory_order_release); sem_post(&a->notify_sem);
    pthread_mutex_unlock(&a->control);
    pthread_join(a->notify_th, NULL);
    for (int i = 0; i < AOMAX; ++i) if (g_aos[i] == a) g_aos[i] = NULL;
    objmap_del(g); pthread_mutex_unlock(&g_aos_lock);
    sem_destroy(&a->notify_sem); pthread_mutex_destroy(&a->control); pthread_mutex_destroy(&a->m); free(a);
}
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "original logical AudioOut state", int, h_au_State, const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "actual verified 48 kHz stream format", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "unchanged original interleaved channels", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "actual PCM16 stream format", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }
HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "original guest buffer descriptor layout", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) { b->ptr = p; b->cap = cap; b->size = size; }
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "original PCM data pointer", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "original PCM byte size", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "queue actual PCM for AAudio; reject invalid/duplicate/full buffers", unsigned, h_au_Append, void *g, gbuf *b) {
    aout *a = ao(g);
    if (!b || !b->ptr || b->size > b->cap || b->size % (2u * (unsigned)a->ch)) return 0;
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed), qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    uint32_t rt = atomic_load_explicit(&a->rel_tail, memory_order_acquire), rh = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    int ok = (uint64_t)(qt - qh) + (rt - rh) < QMAX;
    for (uint32_t i = qh; ok && i != qt; ++i) if (a->q[i & (QMAX - 1)] == b) ok = 0;
    for (uint32_t i = rh; ok && i != rt; ++i) if (a->rel[i & (RELMAX - 1)] == b) ok = 0;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m); return ok;
}
HLE_FORCE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "retrieve only PCM buffers consumed by actual AAudio callbacks", gbuf *, h_au_Released, void *g) {
    aout *a = ao(g); gbuf *b = NULL; pthread_mutex_lock(&a->m);
    uint32_t rh = atomic_load_explicit(&a->rel_head, memory_order_relaxed), rt = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rh != rt) { b = a->rel[rh & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rh + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m); return b;
}

void dm_audio_suspend(int on) {
    /* Keep the actual device pump alive with zero PCM while backgrounded.
     * Buffer/event progress remains hardware-callback driven, not timed. */
    atomic_store_explicit(&g_suspended, !!on, memory_order_release);
    LOG("AAudio lifecycle %s (real device callback clock)", on ? "muted" : "resumed");
}

int dm_android_audio_snapshot(dm_android_audio_metrics *out) {
    if (!out) return EINVAL;
    memset(out, 0, sizeof *out);
    out->render_callbacks = atomic_load(&g_render_calls); out->device_frames_requested = atomic_load(&g_device_frames);
    out->game_bytes_copied = atomic_load(&g_game_bytes); out->buffers_released = atomic_load(&g_released);
    out->release_events = atomic_load(&g_release_events); out->underrun_frames = atomic_load(&g_underrun); out->nonzero_samples = atomic_load(&g_nonzero);
    out->error_callbacks = atomic_load(&g_error_callbacks); out->stream_recoveries = atomic_load(&g_recoveries);
    out->muted = atomic_load(&g_suspended); out->last_error = atomic_load(&g_last_error);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; ++i) if (g_aos[i]) {
        aout *a = g_aos[i]; ++out->open_outputs; pthread_mutex_lock(&a->control);
        out->started_outputs += atomic_load(&a->state) == STATE_STARTED;
        if (a->stream) {
            int64_t written = AAudioStream_getFramesWritten(a->stream), read = AAudioStream_getFramesRead(a->stream);
            if (written > 0) out->frames_written += (uint64_t)written;
            if (read > 0) out->frames_read += (uint64_t)read;
            int64_t position, timestamp;
            if (AAudioStream_getTimestamp(a->stream, CLOCK_MONOTONIC, &position, &timestamp) == AAUDIO_OK) {
                out->timestamp_valid = 1; out->device_frame_position = position; out->device_timestamp_ns = timestamp;
            }
        }
        pthread_mutex_unlock(&a->control);
    }
    pthread_mutex_unlock(&g_aos_lock); return 0;
}

int dm_android_audio_probe_channels(int channels, dm_android_audio_metrics *out) {
    if (channels < 1 || channels > 8) return EINVAL;
    uint8_t guest[64] = {0}; uint64_t param = (uint64_t)(unsigned)channels << 32;
    enum { FRAMES = RATE / 10, BUFFERS = 3 };
    int16_t *pcm = malloc(FRAMES * (unsigned)channels * BUFFERS * sizeof *pcm);
    if (!pcm) return ENOMEM;
    gbuf buffers[BUFFERS]; int result = 0;
    /* Low-volume triangle wave, real nonzero PCM without an extra math lib. */
    for (int i = 0; i < FRAMES * BUFFERS; ++i) {
        int phase = i % 120, sample = phase < 60 ? phase * 40 - 1200 : 3600 - phase * 40;
        for (int channel = 0; channel < channels; ++channel) pcm[i * channels + channel] = (int16_t)sample;
    }
    dm_android_audio_metrics before, after;
    dm_android_audio_snapshot(&before);
    if (before.open_outputs) { free(pcm); return EBUSY; }
    if (h_au_Open(guest, NULL, "DeviceOut", (const uint8_t *)&param) != RES_OK) { free(pcm); return ENODEV; }
    int audible = !ao(guest)->muted && !before.muted;
    if (h_au_State(guest) != STATE_STOPPED || h_au_Rate(guest) != RATE || h_au_Ch(guest) != channels || h_au_Fmt(guest) != FMT_PCM16) result = EPROTO;
    for (int i = 0; i < BUFFERS && !result; ++i) {
        uint64_t bytes = FRAMES * 2u * (unsigned)channels;
        h_au_SetBuf(&buffers[i], pcm + i * FRAMES * channels, bytes, bytes);
        if (!h_au_Append(guest, &buffers[i]) || h_au_Append(guest, &buffers[i])) result = EPROTO;
    }
    if (!result && h_au_Released(guest)) result = EPROTO;
    if (!result && h_au_Start(guest) != RES_OK) result = EIO;
    uint64_t start = now_ns(); int count = 0;
    while (!result && count < BUFFERS && now_ns() - start < 2000000000ull) {
        gbuf *buffer = h_au_Released(guest);
        if (buffer) { if (buffer != &buffers[count]) result = EPROTO; else ++count; }
        else usleep(2000);
    }
    dm_android_audio_snapshot(&after);
    if (!result && (count != BUFFERS || after.render_callbacks <= before.render_callbacks ||
        after.game_bytes_copied - before.game_bytes_copied != FRAMES * 2u * (unsigned)channels * BUFFERS || !after.frames_written || !after.frames_read)) result = EIO;
    if (!result && audible && after.nonzero_samples <= before.nonzero_samples) result = EIO;
    h_au_Stop(guest);
    if (h_au_State(guest) != STATE_STOPPED) result = EPROTO;
    uint64_t calls = dm_audio_render_calls(); usleep(30000);
    if (dm_audio_render_calls() != calls) result = EPROTO;
    if (!result && h_au_Start(guest) != RES_OK) result = EIO;
    usleep(40000);
    if (!result && dm_audio_render_calls() <= calls) result = EIO;
    dm_android_audio_snapshot(out ? out : &after);
    h_au_Close(guest); free(pcm);
    LOG("AAudio actual-device probe channels=%d result=%d released=%d/3", channels, result, count);
    return result;
}

int dm_android_audio_probe(dm_android_audio_metrics *out) { return dm_android_audio_probe_channels(2, out); }
