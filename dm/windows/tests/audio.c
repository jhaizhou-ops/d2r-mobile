// 真实工作线程 + 模拟 COM 音频端点，不打开扬声器。
// Real worker with a simulated COM audio endpoint; no speakers are opened.
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include "dm_windows.h"
#include <assert.h>
#include <stdatomic.h>

static HRESULT WINAPI test_com_init(LPVOID, DWORD);
static void WINAPI test_com_end(void);
static HRESULT WINAPI test_create(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
static MMRESULT WINAPI test_begin(UINT);
static MMRESULT WINAPI test_end(UINT);
static HANDLE WINAPI test_mmcss(LPCWSTR, LPDWORD);
static BOOL WINAPI test_priority(HANDLE, AVRT_PRIORITY);
static BOOL WINAPI test_revert(HANDLE);
#define CoInitializeEx test_com_init
#define CoUninitialize test_com_end
#define CoCreateInstance test_create
#define timeBeginPeriod test_begin
#define timeEndPeriod test_end
#define AvSetMmThreadCharacteristicsW test_mmcss
#define AvSetMmThreadPriority test_priority
#define AvRevertMmThreadCharacteristics test_revert
#include "../audio.c"

enum { FAIL_COM = 1, FAIL_CREATE, FAIL_ENDPOINT, FAIL_ACTIVATE, FAIL_INITIALIZE,
    FAIL_SIZE, FAIL_SERVICE, FAIL_EVENT, FAIL_START, FAIL_PADDING, FAIL_BUFFER, FAIL_RELEASE, FAIL_INVALID_PADDING };
static struct {
    CRITICAL_SECTION lock;
    DWORD owner;
    HANDLE event;
    UINT32 capacity, padding, requested;
    unsigned channels, created, released, com_ended, registered, prioritized, reverted;
    unsigned generated, checked, begun, ended, timer_live;
    int fail_open, fail_mmcss, fail_priority, fail_begin, fail_end, started;
    _Atomic int fail_runtime;
    _Atomic unsigned writes, renders;
    int16_t samples[QUEUE_FRAMES * 2];
} mock;
static IMMDeviceEnumerator enumerator;
static IMMDevice endpoint;
static IAudioClient client;
static IAudioRenderClient output;
static void owner(void) { assert(mock.owner == GetCurrentThreadId()); }
static HRESULT WINAPI test_com_init(LPVOID reserved, DWORD flags) {
    assert(!reserved && flags == COINIT_MULTITHREADED);
    mock.owner = GetCurrentThreadId();
    return mock.fail_open == FAIL_COM ? E_FAIL : S_OK;
}
static void WINAPI test_com_end(void) { owner(); assert(mock.created == mock.released); mock.com_ended++; }
static HRESULT WINAPI test_create(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID *object) {
    owner(); assert(IsEqualGUID(clsid, &CLSID_MMDeviceEnumerator) && !outer && context == CLSCTX_ALL);
    assert(IsEqualGUID(iid, &IID_IMMDeviceEnumerator));
    if (mock.fail_open == FAIL_CREATE) return E_FAIL;
    *object = &enumerator; mock.created++; return S_OK;
}
static ULONG WINAPI release_enumerator(IMMDeviceEnumerator *self) { owner(); assert(self == &enumerator); mock.released++; return 0; }
static ULONG WINAPI release_endpoint(IMMDevice *self) { owner(); assert(self == &endpoint); mock.released++; return 0; }
static ULONG WINAPI release_client(IAudioClient *self) { owner(); assert(self == &client); mock.released++; return 0; }
static ULONG WINAPI release_output(IAudioRenderClient *self) { owner(); assert(self == &output); mock.released++; return 0; }
static HRESULT WINAPI get_endpoint(IMMDeviceEnumerator *self, EDataFlow flow, ERole role, IMMDevice **device) {
    owner(); assert(self == &enumerator && flow == eRender && role == eConsole);
    if (mock.fail_open == FAIL_ENDPOINT) return E_FAIL;
    *device = &endpoint; mock.created++; return S_OK;
}
static HRESULT WINAPI activate(IMMDevice *self, REFIID iid, DWORD context, PROPVARIANT *params, void **object) {
    owner(); assert(self == &endpoint && IsEqualGUID(iid, &IID_IAudioClient) && context == CLSCTX_ALL && !params);
    if (mock.fail_open == FAIL_ACTIVATE) return E_FAIL;
    *object = &client; mock.created++; return S_OK;
}
static HRESULT WINAPI initialize(IAudioClient *self, AUDCLNT_SHAREMODE mode, DWORD flags,
        REFERENCE_TIME duration, REFERENCE_TIME period, const WAVEFORMATEX *format, LPCGUID session) {
    owner(); assert(self == &client && mode == AUDCLNT_SHAREMODE_SHARED && !duration && !period && !session);
    assert(flags == (AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY));
    assert(format->wFormatTag == WAVE_FORMAT_PCM && format->nSamplesPerSec == RATE && format->wBitsPerSample == 16);
    assert(format->nChannels == mock.channels && format->nBlockAlign == mock.channels * 2);
    assert(format->nAvgBytesPerSec == RATE * mock.channels * 2 && !format->cbSize);
    return mock.fail_open == FAIL_INITIALIZE ? E_FAIL : S_OK;
}
static HRESULT WINAPI get_size(IAudioClient *self, UINT32 *frames) {
    owner(); assert(self == &client);
    *frames = mock.capacity; return mock.fail_open == FAIL_SIZE ? E_FAIL : S_OK;
}
static HRESULT WINAPI get_service(IAudioClient *self, REFIID iid, void **object) {
    owner(); assert(self == &client && IsEqualGUID(iid, &IID_IAudioRenderClient));
    if (mock.fail_open == FAIL_SERVICE) return E_FAIL;
    *object = &output; mock.created++; return S_OK;
}
static HRESULT WINAPI set_event(IAudioClient *self, HANDLE event) {
    owner(); assert(self == &client && event); mock.event = event;
    return mock.fail_open == FAIL_EVENT ? E_FAIL : S_OK;
}
static HRESULT WINAPI get_padding(IAudioClient *self, UINT32 *frames) {
    owner(); assert(self == &client);
    if (atomic_load(&mock.fail_runtime) == FAIL_PADDING) return AUDCLNT_E_DEVICE_INVALIDATED;
    if (atomic_load(&mock.fail_runtime) == FAIL_INVALID_PADDING) { *frames = mock.capacity + 1; return S_OK; }
    EnterCriticalSection(&mock.lock); *frames = mock.padding; LeaveCriticalSection(&mock.lock);
    return S_OK;
}
static HRESULT WINAPI get_buffer(IAudioRenderClient *self, UINT32 frames, BYTE **data) {
    owner(); assert(self == &output && frames && frames <= QUEUE_FRAMES && !mock.requested);
    if (atomic_load(&mock.fail_runtime) == FAIL_BUFFER) return AUDCLNT_E_DEVICE_INVALIDATED;
    EnterCriticalSection(&mock.lock); assert(frames <= mock.capacity - mock.padding); LeaveCriticalSection(&mock.lock);
    mock.requested = frames; *data = (BYTE *)mock.samples; return S_OK;
}
static HRESULT WINAPI release_buffer(IAudioRenderClient *self, UINT32 frames, DWORD flags) {
    owner(); assert(self == &output && frames == mock.requested && !flags);
    for (unsigned i = 0; i < frames * mock.channels; i++) assert(mock.samples[i] == (int16_t)mock.checked++);
    mock.requested = 0;
    if (atomic_load(&mock.fail_runtime) == FAIL_RELEASE) return AUDCLNT_E_DEVICE_INVALIDATED;
    EnterCriticalSection(&mock.lock); mock.padding += frames; LeaveCriticalSection(&mock.lock);
    atomic_fetch_add(&mock.writes, 1); return S_OK;
}
static HRESULT WINAPI start(IAudioClient *self) {
    owner(); assert(self == &client && !mock.started && mock.padding && atomic_load(&mock.writes));
    if (mock.fail_open == FAIL_START) return E_FAIL;
    mock.started = 1; return S_OK;
}
static HRESULT WINAPI stop(IAudioClient *self) { owner(); assert(self == &client); mock.started = 0; return S_OK; }
static HRESULT WINAPI reset(IAudioClient *self) {
    owner(); assert(self == &client && !mock.started && !mock.requested);
    EnterCriticalSection(&mock.lock); mock.padding = 0; LeaveCriticalSection(&mock.lock); return S_OK;
}
static MMRESULT WINAPI test_begin(UINT period) {
    owner(); assert(period == 1 && !mock.timer_live);
    if (mock.fail_begin) return TIMERR_NOCANDO;
    mock.begun++; mock.timer_live = 1; return TIMERR_NOERROR;
}
static MMRESULT WINAPI test_end(UINT period) {
    owner(); assert(period == 1 && mock.timer_live);
    if (mock.fail_end) { mock.fail_end = 0; return TIMERR_NOCANDO; }
    mock.ended++; mock.timer_live = 0; return TIMERR_NOERROR;
}
static HANDLE WINAPI test_mmcss(LPCWSTR name, LPDWORD task) {
    owner(); assert(!wcscmp(name, L"Audio") && !*task); mock.registered++;
    return mock.fail_mmcss ? NULL : (HANDLE)(uintptr_t)2;
}
static BOOL WINAPI test_priority(HANDLE task, AVRT_PRIORITY priority) {
    owner(); assert(task == (HANDLE)(uintptr_t)2 && priority == AVRT_PRIORITY_HIGH); mock.prioritized++;
    return !mock.fail_priority;
}
static BOOL WINAPI test_revert(HANDLE task) { owner(); assert(task == (HANDLE)(uintptr_t)2); mock.reverted++; return TRUE; }
static void test_render(void *context, void *data, uint32_t bytes) {
    owner(); assert(context == &mock && bytes && bytes % (mock.channels * 2) == 0 && bytes <= QUEUE_FRAMES * mock.channels * 2);
    int16_t *samples = data;
    for (unsigned i = 0; i < bytes / 2; i++) samples[i] = (int16_t)mock.generated++;
    atomic_fetch_add(&mock.renders, 1);
}
static void prepare(unsigned channels) {
    memset(&mock, 0, sizeof mock); InitializeCriticalSection(&mock.lock);
    mock.channels = channels; mock.capacity = QUEUE_FRAMES + FRAMES;
}
static void finish(void) {
    assert(mock.created == mock.released && mock.begun == mock.ended && !mock.timer_live && !mock.requested);
    assert(mock.com_ended == (mock.fail_open != FAIL_COM));
    assert(mock.reverted == (mock.registered && !mock.fail_mmcss));
    DeleteCriticalSection(&mock.lock);
}
static void consume(unsigned frames, int notify) {
    EnterCriticalSection(&mock.lock); assert(mock.padding >= frames); mock.padding -= frames; LeaveCriticalSection(&mock.lock);
    if (notify) audio_signal(mock.event);
}
static void wait_count(_Atomic unsigned *counter, unsigned count) {
    ULONGLONG deadline = GetTickCount64() + 3000;
    while (atomic_load(counter) < count) { assert(GetTickCount64() < deadline); Sleep(1); }
}
int main(void) {
    IMMDeviceEnumeratorVtbl ev = {.Release = release_enumerator, .GetDefaultAudioEndpoint = get_endpoint};
    IMMDeviceVtbl dv = {.Release = release_endpoint, .Activate = activate};
    IAudioClientVtbl cv = {.Release = release_client, .Initialize = initialize, .GetBufferSize = get_size,
        .GetService = get_service, .SetEventHandle = set_event, .GetCurrentPadding = get_padding,
        .Start = start, .Stop = stop, .Reset = reset};
    IAudioRenderClientVtbl ov = {.Release = release_output, .GetBuffer = get_buffer, .ReleaseBuffer = release_buffer};
    enumerator.lpVtbl = &ev; endpoint.lpVtbl = &dv; client.lpVtbl = &cv; output.lpVtbl = &ov;
    for (unsigned channels = 1; channels <= 2; channels++) {
        prepare(channels);
        dm_audev device = dm_audio_open(channels, test_render, &mock); assert(device);
        assert(!dm_audio_start(device) && mock.started && mock.padding == QUEUE_FRAMES);
        assert(!dm_audio_start(device) && atomic_load(&mock.writes) == 1 && mock.begun == 1);
        consume(120, 1); wait_count(&mock.writes, 2);
        consume(FRAMES, 0); wait_count(&mock.writes, 3);
        consume(QUEUE_FRAMES, 1); wait_count(&mock.writes, 4);
        dm_audio_stop(device);
        assert(device->frames == 2 * QUEUE_FRAMES + FRAMES + 120 && device->empty == 1);
        assert(!mock.started && mock.begun == mock.ended);
        unsigned renders = atomic_load(&mock.renders); Sleep(25); assert(atomic_load(&mock.renders) == renders);
        assert(!dm_audio_start(device) && mock.padding == QUEUE_FRAMES);
        dm_audio_close(device);
        assert(mock.registered == 1 && mock.prioritized == 1); finish();
    }
    for (int failure = FAIL_COM; failure <= FAIL_START; failure++) {
        prepare(2); mock.fail_open = failure;
        dm_audev device = dm_audio_open(2, test_render, &mock);
        if (failure == FAIL_START) { assert(device && dm_audio_start(device) == -1 && !device->playing); dm_audio_close(device); }
        else assert(!device);
        finish();
    }
    for (int failure = FAIL_PADDING; failure <= FAIL_INVALID_PADDING; failure++) {
        prepare(2);
        dm_audev device = dm_audio_open(2, test_render, &mock); assert(device && !dm_audio_start(device));
        atomic_store(&mock.fail_runtime, failure); unsigned renders = atomic_load(&mock.renders);
        consume(FRAMES, 1); wait_count(&mock.renders, renders + 3);
        dm_audio_stop(device);
        assert(device->failed && !mock.started && dm_audio_start(device) == -1);
        dm_audio_close(device); finish();
    }
    for (int failure = 0; failure < 4; failure++) {
        prepare(2);
        mock.fail_mmcss = failure == 0; mock.fail_priority = failure == 1;
        mock.fail_begin = failure == 2; mock.fail_end = failure == 3;
        dm_audev device = dm_audio_open(2, test_render, &mock); assert(device && !dm_audio_start(device));
        dm_audio_stop(device); dm_audio_close(device); finish();
    }
    prepare(2); mock.capacity = 0;
    assert(!dm_audio_open(2, test_render, &mock)); finish();
    prepare(1); mock.capacity = FRAMES * 2;
    dm_audev device = dm_audio_open(1, test_render, &mock); assert(device && !dm_audio_start(device));
    assert(device->target == FRAMES * 2 && mock.padding == FRAMES * 2);
    consume(120, 0); wait_count(&mock.writes, 2);
    dm_audio_close(device); finish();
    puts("Windows audio: WASAPI padding/PCM/30ms target, priming, COM/MMCSS ownership, suspend/resume, errors and timer cleanup passed");
    return 0;
}
