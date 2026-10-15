/* Host-only queue/event stress harness. AAudio API stubs abort if invoked;
 * this cannot pass as a real device or original-SDK audio test. */
#include "../host/hle_audio.c"
#include <sched.h>

enum { NBUF = 100000, CHANNELS = 6, FRAME_BYTES = CHANNELS * 2, MAX_SIZE = 37 * FRAME_BYTES };
static struct { const void *key; void *value; } objects[8];
uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
void *objmap_get(const void *key) { for (int i = 0; i < 8; ++i) if (objects[i].key == key) return objects[i].value; return NULL; }
void objmap_put(const void *key, void *value) { for (int i = 0; i < 8; ++i) if (!objects[i].key) { objects[i].key = key; objects[i].value = value; return; } abort(); }
void *objmap_del(const void *key) { for (int i = 0; i < 8; ++i) if (objects[i].key == key) { objects[i].key = NULL; return objects[i].value; } return NULL; }
static _Atomic int signals, final_pull, final_signal;
void hle_bind_system_event(void *event, int autoclear) { (void)event; (void)autoclear; abort(); }
void hle_signal_system_event(void *event) {
    if (event != (void *)0xcafe) abort();
    atomic_fetch_add(&signals, 1);
    if (atomic_load(&final_pull)) atomic_store(&final_signal, 1);
}
static uint32_t random_next(uint32_t *state) { *state = *state * 1664525u + 1013904223u; return *state; }
typedef struct {
    void *guest; aout *output; gbuf *buffers;
    uint8_t (*data)[MAX_SIZE]; uint32_t *sizes; uint64_t bytes;
    _Atomic int failed, reclaimed;
} test;
static uint64_t available(aout *a) {
    uint32_t head = atomic_load(&a->q_head), tail = atomic_load_explicit(&a->q_tail, memory_order_acquire);
    if (head == tail) return 0;
    uint64_t size = a->q[head & (QMAX - 1)]->size - a->pos;
    for (uint32_t i = head + 1; i != tail; ++i) size += a->q[i & (QMAX - 1)]->size;
    return size;
}
static void *produce(void *ctx) {
    test *t = ctx;
    for (uint32_t i = 0; i < NBUF; ++i) {
        h_au_SetBuf(&t->buffers[i], t->data[i], MAX_SIZE, t->sizes[i]);
        while (!h_au_Append(t->guest, &t->buffers[i])) sched_yield();
    }
    return NULL;
}
static void *consume(void *ctx) {
    test *t = ctx; uint32_t random = 7; uint64_t done = 0;
    uint8_t scratch[FRAME_BYTES * 64];
    while (done < t->bytes) {
        uint64_t bytes = available(t->output);
        if (!bytes) { sched_yield(); continue; }
        uint64_t size = (1 + random_next(&random) % 64) * FRAME_BYTES;
        if (size > bytes) size = bytes;
        if (done + size == t->bytes) atomic_store(&final_pull, 1);
        pull(t->output, scratch, size);
        for (uint64_t i = 0; i < size; ++i) if (scratch[i] != (uint8_t)(done + i)) atomic_store(&t->failed, 1);
        done += size;
    }
    return NULL;
}
static void *reclaim(void *ctx) {
    test *t = ctx;
    for (int i = 0; i < NBUF;) {
        gbuf *b = h_au_Released(t->guest);
        if (!b) { sched_yield(); continue; }
        if (b != &t->buffers[i]) atomic_store(&t->failed, 1);
        atomic_store(&t->reclaimed, ++i);
    }
    return NULL;
}
static void initialize(aout *a, void *guest) {
    memset(a, 0, sizeof *a); a->ch = CHANNELS; a->event = (void *)0xcafe;
    pthread_mutex_init(&a->m, NULL); pthread_mutex_init(&a->control, NULL); sem_init(&a->notify_sem, 0, 0);
    atomic_init(&a->state, STATE_STARTED); atomic_init(&a->notify_run, 1);
    objmap_put(guest, a); g_aos[0] = a;
    pthread_create(&a->notify_th, NULL, event_notify, a);
}

int main(void) {
    uint8_t guest[64], scratch[FRAME_BYTES * 2], payload[FRAME_BYTES * 2];
    aout *output = calloc(1, sizeof *output);
    initialize(output, guest);
    memset(payload, 0x5a, sizeof payload);
    gbuf buffer, invalid;
    h_au_SetBuf(&buffer, payload, sizeof payload, sizeof payload);
    h_au_SetBuf(&invalid, payload, sizeof payload, sizeof payload - 1);
    if (h_au_Append(guest, &invalid) || !h_au_Append(guest, &buffer) || h_au_Append(guest, &buffer)) return 2;
    pull(output, scratch, FRAME_BYTES);
    if (h_au_Released(guest) || memcmp(scratch, payload, FRAME_BYTES) || output->pos != FRAME_BYTES) return 3;
    atomic_store(&output->state, STATE_STOPPED);
    memset(scratch, 1, sizeof scratch); pull(output, scratch, FRAME_BYTES);
    for (unsigned i = 0; i < FRAME_BYTES; ++i) if (scratch[i]) return 4;
    if (output->pos != FRAME_BYTES || h_au_Released(guest)) return 5;
    atomic_store(&output->state, STATE_STARTED);
    pull(output, scratch, FRAME_BYTES);
    if (h_au_Append(guest, &buffer) || h_au_Released(guest) != &buffer) return 6;
    /* Released + pending share the original in-flight limit. */
    gbuf full[QMAX + 1];
    for (int i = 0; i < QMAX + 1; ++i) h_au_SetBuf(&full[i], payload, sizeof payload, FRAME_BYTES);
    for (int i = 0; i < QMAX; ++i) if (!h_au_Append(guest, &full[i])) return 7;
    if (h_au_Append(guest, &full[QMAX])) return 8;
    for (int i = 0; i < QMAX; ++i) pull(output, scratch, FRAME_BYTES);
    if (h_au_Append(guest, &full[QMAX])) return 9;
    for (int i = 0; i < QMAX; ++i) if (h_au_Released(guest) != &full[i]) return 10;
    /* Lifecycle mute still advances the same render callback queue; resume
     * restores PCM. No Android device call occurs in this software test. */
    if (!h_au_Append(guest, &buffer)) return 12;
    dm_audio_suspend(1); memset(scratch, 1, sizeof scratch);
    audio_render(NULL, output, scratch, 1);
    for (unsigned i = 0; i < FRAME_BYTES; ++i) if (scratch[i]) return 13;
    if (h_au_Released(guest) || output->pos != FRAME_BYTES) return 14;
    dm_audio_suspend(0); audio_render(NULL, output, scratch, 1);
    if (memcmp(scratch, payload, FRAME_BYTES) || h_au_Released(guest) != &buffer) return 15;
    test t = {.guest = guest, .output = output};
    t.buffers = calloc(NBUF, sizeof *t.buffers); t.data = calloc(NBUF, sizeof *t.data); t.sizes = calloc(NBUF, sizeof *t.sizes);
    if (!t.buffers || !t.data || !t.sizes) return 11;
    uint32_t random = 1; uint64_t cursor = 0;
    for (int i = 0; i < NBUF; ++i) {
        t.sizes[i] = (1 + random_next(&random) % 37) * FRAME_BYTES;
        for (uint32_t j = 0; j < t.sizes[i]; ++j) t.data[i][j] = (uint8_t)cursor++;
        t.bytes += t.sizes[i];
    }
    pthread_t producer, consumer, reader;
    pthread_create(&producer, NULL, produce, &t); pthread_create(&consumer, NULL, consume, &t); pthread_create(&reader, NULL, reclaim, &t);
    pthread_join(producer, NULL); pthread_join(consumer, NULL); pthread_join(reader, NULL);
    uint64_t deadline = now_ns() + 1000000000ull;
    while (!atomic_load(&final_signal) && now_ns() < deadline) usleep(1000);
    int failed = atomic_load(&t.failed), reclaimed = atomic_load(&t.reclaimed), last_event = atomic_load(&final_signal);
    h_au_Close(guest);
    printf("{\"passed\":%s,\"buffers\":%d,\"bytes\":%llu,\"reclaimed\":%d,\"events\":%d,\"final_event\":%d,\"real_device_test\":false}\n",
        (!failed && reclaimed == NBUF && last_event) ? "true" : "false", NBUF, (unsigned long long)t.bytes, reclaimed, atomic_load(&signals), last_event);
    free(t.sizes); free(t.data); free(t.buffers);
    return failed || reclaimed != NBUF || !last_event;
}
