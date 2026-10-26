// 直接按 WASAPI 剩余帧数补充，不再等 WinMM 成批归还短缓冲。
// Refill WASAPI's actual padding directly, rather than wait for batched WinMM buffer returns.
#define COBJMACROS
#include "audio.h"
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { RATE = 48000, FRAMES = 480, QUEUE_FRAMES = 3 * FRAMES };
enum { CMD_NONE, CMD_START, CMD_STOP, CMD_CLOSE };
struct dm_audio_device {
    HANDLE event, done;
    CRITICAL_SECTION api_lock;
    pthread_t thread;
    _Atomic int command;
    int thread_created, opened, playing, failed, timer_active, result;
    IAudioClient *client;
    IAudioRenderClient *output;
    UINT32 capacity, target;
    dm_audio_render render;
    void *context;
    unsigned channels;
    LARGE_INTEGER frequency;
    uint64_t refills, frames, empty, last_refill, max_gap, max_render, max_submit;
};
static uint64_t audio_ticks(void) {
    LARGE_INTEGER value; QueryPerformanceCounter(&value); return (uint64_t)value.QuadPart;
}
static void audio_error(const char *operation, HRESULT result) {
    fprintf(stderr, "[dm] audio: WASAPI %s failed (0x%08lx)\n", operation, (unsigned long)result);
}
static void audio_signal(HANDLE event) {
    if (!SetEvent(event)) { fprintf(stderr, "[dm] audio: event signal failed (%lu)\n", GetLastError()); abort(); }
}
static void audio_wait(HANDLE event) {
    if (WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0) {
        fprintf(stderr, "[dm] audio: event wait failed (%lu)\n", GetLastError()); abort();
    }
}
static void audio_report(dm_audev device) {
    if (!device->refills) return;
    double milliseconds = 1000.0 / (double)device->frequency.QuadPart;
    fprintf(stderr, "[dm] audio: WASAPI cumulative refills=%llu frames=%llu empty=%llu max_gap_ms=%.2f max_render_ms=%.2f max_submit_ms=%.2f queue_ms=%.2f\n",
        (unsigned long long)device->refills, (unsigned long long)device->frames, (unsigned long long)device->empty,
        device->max_gap * milliseconds, device->max_render * milliseconds, device->max_submit * milliseconds,
        device->target * 1000.0 / RATE);
}
static void timer_stop(dm_audev device) {
    if (!device->timer_active) return;
    MMRESULT result = timeEndPeriod(1);
    if (result) fprintf(stderr, "[dm] audio: timeEndPeriod failed (%u)\n", result);
    else device->timer_active = 0;
}
static HRESULT audio_open_stream(dm_audev device) {
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *endpoint = NULL;
    HRESULT result = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
        &IID_IMMDeviceEnumerator, (void **)&enumerator);
    const char *operation = "create enumerator";
    if (FAILED(result)) goto done;
    operation = "default endpoint";
    result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &endpoint);
    if (FAILED(result)) goto done;
    operation = "activate";
    result = IMMDevice_Activate(endpoint, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&device->client);
    if (FAILED(result)) goto done;
    WAVEFORMATEX format = {.wFormatTag = WAVE_FORMAT_PCM, .nChannels = (WORD)device->channels,
        .nSamplesPerSec = RATE, .nAvgBytesPerSec = RATE * device->channels * 2,
        .nBlockAlign = (WORD)(device->channels * 2), .wBitsPerSample = 16};
    operation = "initialize";
    result = IAudioClient_Initialize(device->client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format, NULL);
    if (FAILED(result)) goto done;
    operation = "buffer size";
    result = IAudioClient_GetBufferSize(device->client, &device->capacity);
    if (SUCCEEDED(result) && !device->capacity) result = E_UNEXPECTED;
    if (FAILED(result)) goto done;
    device->target = device->capacity < QUEUE_FRAMES ? device->capacity : QUEUE_FRAMES;
    operation = "render service";
    result = IAudioClient_GetService(device->client, &IID_IAudioRenderClient, (void **)&device->output);
    if (FAILED(result)) goto done;
    operation = "event handle";
    result = IAudioClient_SetEventHandle(device->client, device->event);
    if (SUCCEEDED(result)) fprintf(stderr, "[dm] audio: WASAPI shared PCM16/%u/%u, buffer_frames=%u queue_ms=%.2f\n",
        RATE, device->channels, device->capacity, device->target * 1000.0 / RATE);
done:
    if (endpoint) IMMDevice_Release(endpoint);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(result)) audio_error(operation, result);
    return result;
}
static HRESULT audio_fill(dm_audev device) {
    UINT32 padding;
    HRESULT result = IAudioClient_GetCurrentPadding(device->client, &padding);
    if (FAILED(result)) { audio_error("padding", result); return result; }
    if (padding > device->capacity) { audio_error("invalid padding", E_UNEXPECTED); return E_UNEXPECTED; }
    if (padding >= device->target) return S_OK;
    if (!padding && device->last_refill) device->empty++;
    UINT32 frames = device->target - padding;
    BYTE *data;
    uint64_t before = audio_ticks();
    result = IAudioRenderClient_GetBuffer(device->output, frames, &data);
    if (FAILED(result)) { audio_error("get buffer", result); return result; }
    uint64_t begin = audio_ticks();
    if (device->last_refill && begin - device->last_refill > device->max_gap) device->max_gap = begin - device->last_refill;
    device->render(device->context, data, frames * device->channels * 2);
    uint64_t elapsed = audio_ticks() - begin;
    if (elapsed > device->max_render) device->max_render = elapsed;
    result = IAudioRenderClient_ReleaseBuffer(device->output, frames, 0);
    uint64_t submitted = audio_ticks() - before;
    if (submitted > device->max_submit) device->max_submit = submitted;
    if (FAILED(result)) { audio_error("release buffer", result); return result; }
    device->last_refill = begin; device->refills++; device->frames += frames;
    return S_OK;
}
static void audio_stop_stream(dm_audev device) {
    HRESULT result = IAudioClient_Stop(device->client);
    if (FAILED(result)) { audio_error("stop", result); device->failed = 1; }
    result = IAudioClient_Reset(device->client);
    if (FAILED(result)) { audio_error("reset", result); device->failed = 1; }
}
static void *audio_main(void *argument) {
    dm_audev device = argument;
    HRESULT result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com = SUCCEEDED(result);
    HANDLE mmcss = NULL;
    if (!com) audio_error("COM initialization", result);
    else result = audio_open_stream(device);
    device->opened = SUCCEEDED(result);
    device->result = device->opened ? 0 : -1;
    if (device->opened) {
        DWORD task = 0;
        mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
        if (!mmcss) fprintf(stderr, "[dm] audio: MMCSS Audio registration failed (%lu); playback may underrun\n", GetLastError());
        else if (!AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH))
            fprintf(stderr, "[dm] audio: MMCSS priority failed (%lu)\n", GetLastError());
        else fprintf(stderr, "[dm] audio: WASAPI MMCSS Audio/high\n");
    }
    audio_signal(device->done);
    ULONGLONG next_frame = 0;
    int16_t scratch[FRAMES * 2];
    while (device->opened) {
        // 设备事件是主节拍；有界后备检查也能发现漏通知或设备失效，不让客体永久停播。
        // Device events drive playback; a bounded fallback detects missed notifications or device loss.
        DWORD wait = WaitForSingleObject(device->event, device->playing ? 10 : INFINITE);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            fprintf(stderr, "[dm] audio: playback event wait failed (%lu)\n", GetLastError()); abort();
        }
        int command = atomic_exchange_explicit(&device->command, CMD_NONE, memory_order_acq_rel);
        if (command == CMD_CLOSE) {
            if (device->playing) { audio_stop_stream(device); audio_report(device); }
            timer_stop(device);
            audio_signal(device->done);
            break;
        }
        if (command == CMD_STOP) {
            if (device->playing) { audio_stop_stream(device); device->playing = 0; audio_report(device); }
            timer_stop(device);
            device->result = 0; audio_signal(device->done);
            continue;
        }
        if (command == CMD_START) {
            device->result = 0;
            if (device->failed) device->result = -1;
            else if (!device->playing) {
                if (!device->timer_active) {
                    MMRESULT timer = timeBeginPeriod(1);
                    if (timer) fprintf(stderr, "[dm] audio: timeBeginPeriod failed (%u)\n", timer);
                    else device->timer_active = 1;
                }
                device->last_refill = 0;
                // 所有 COM 调用及预填充都在同一线程；先填再启动，不以空队列起播。
                // Own COM and prime on this thread; start only after the initial PCM is queued.
                result = audio_fill(device);
                if (SUCCEEDED(result)) result = IAudioClient_Start(device->client);
                if (FAILED(result)) {
                    audio_error("start/prime", result); audio_stop_stream(device);
                    timer_stop(device); device->result = -1;
                }
                else device->playing = 1;
            }
            audio_signal(device->done);
            continue;
        }
        if (!device->playing) continue;
        if (device->failed) {
            ULONGLONG now = GetTickCount64();
            if (now >= next_frame) {
                device->render(device->context, scratch, FRAMES * device->channels * 2);
                next_frame = now + 10;
            }
        }
        else if (FAILED(audio_fill(device))) {
            fprintf(stderr, "[dm] audio: WASAPI output lost; using a real-time silent sink\n");
            device->failed = 1; audio_stop_stream(device); next_frame = GetTickCount64() + 10;
        }
    }
    timer_stop(device);
    if (device->output) IAudioRenderClient_Release(device->output);
    if (device->client) IAudioClient_Release(device->client);
    if (mmcss && !AvRevertMmThreadCharacteristics(mmcss))
        fprintf(stderr, "[dm] audio: MMCSS cleanup failed (%lu)\n", GetLastError());
    if (com) CoUninitialize();
    return NULL;
}
static int audio_command(dm_audev device, int command) {
    EnterCriticalSection(&device->api_lock);
    atomic_store_explicit(&device->command, command, memory_order_release);
    audio_signal(device->event); audio_wait(device->done);
    int result = device->result;
    LeaveCriticalSection(&device->api_lock);
    return result;
}
dm_audev dm_audio_open(unsigned channels, dm_audio_render render, void *context) {
    if ((channels != 1 && channels != 2) || !render) { fprintf(stderr, "[dm] audio: invalid WASAPI format\n"); return NULL; }
    dm_audev device = calloc(1, sizeof *device);
    if (!device) { fprintf(stderr, "[dm] audio: device allocation failed\n"); return NULL; }
    device->channels = channels; device->render = render; device->context = context;
    atomic_init(&device->command, CMD_NONE);
    InitializeCriticalSection(&device->api_lock);
    if (!QueryPerformanceFrequency(&device->frequency)) { fprintf(stderr, "[dm] audio: performance clock unavailable\n"); goto fail; }
    device->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    device->done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!device->event || !device->done) { fprintf(stderr, "[dm] audio: event creation failed (%lu)\n", GetLastError()); goto fail; }
    int error = pthread_create(&device->thread, NULL, audio_main, device);
    if (error) { fprintf(stderr, "[dm] audio: thread creation failed (%d)\n", error); goto fail; }
    device->thread_created = 1; audio_wait(device->done);
    if (!device->opened) goto fail;
    return device;
fail:
    dm_audio_close(device);
    return NULL;
}
int dm_audio_start(dm_audev device) { return audio_command(device, CMD_START); }
void dm_audio_stop(dm_audev device) { audio_command(device, CMD_STOP); }
void dm_audio_close(dm_audev device) {
    if (!device) return;
    if (device->thread_created) {
        if (device->opened) audio_command(device, CMD_CLOSE);
        pthread_join(device->thread, NULL);
    }
    if (device->event) CloseHandle(device->event);
    if (device->done) CloseHandle(device->done);
    DeleteCriticalSection(&device->api_lock); free(device);
}
