#include "../host/dm.h"
#include <time.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <limits.h>
#include "services_platform.h"
#include "services_sdk.h"
#include "services_fs.h"

struct dm_flags g_flag;
uint64_t g_t0_ns;
void guest_thread_init(void);
void crash_thread_init(void);
uint32_t svc_thread_main_init(void);
uint64_t dm_android_svc_count(int);
void thr_counts(int *, int *, int *);
void dm_android_loader_keys_init(void);
void gate_register(uint64_t, const char *, int, uint64_t);
uint64_t dm_android_gate_check(void);
extern char dm_android_gate_check_return;
int dm_android_fs_verify(const char *, const char *);
int dm_android_diagnostics_flush(void);
static void fixture_platform_context(const char *base) {
    char profile[PATH_MAX], rom[PATH_MAX], save[PATH_MAX], temporary[PATH_MAX];
    if (!base || base[0]!='/' || strlen(base)>PATH_MAX-64) die("Invalid Android service fixture directory");
    snprintf(profile,sizeof profile,"%s/profile",base); snprintf(rom,sizeof rom,"%s/romfs-fixture",base);
    snprintf(save,sizeof save,"%s/save-fixture",base); snprintf(temporary,sizeof temporary,"%s/temp-fixture",base);
    if (dm_android_fs_ensure_directory(profile) || dm_android_fs_ensure_directory(rom) ||
        dm_android_fs_ensure_directory(save) || dm_android_fs_ensure_directory(temporary)) die("Android service fixture setup");
    setenv("DM_ROMFS",rom,1); setenv("DM_SAVE",save,1); setenv("DM_TEMP",temporary,1);
    dm_android_platform_config config = {.mode=DM_ANDROID_HOST_SHELL, .profile_directory=profile,
        // This test's selected content set is empty, with no game resources/DLC.
        .content_manifest_verified=1, .flush_diagnostics=dm_android_diagnostics_flush};
    int rc=dm_android_platform_bind(&config);
    if (rc) die("Android service fixture bind rc=%d",rc);
    rc=dm_android_platform_register_services();
    if (rc) die("Android service fixture registration rc=%d",rc);
    dm_android_service_register(DM_ANDROID_SERVICE_FS,dm_android_fs_initialize,NULL);
}
void *dm_guest_thread_spawn(void (*)(void *), void *, int, size_t);
void dm_guest_thread_join(void *);
static void (*event_wait)(void *);
static _Atomic int worker_started, worker_done;
static _Atomic uint64_t worker_wait_ns;
static _Atomic unsigned tiny_stack_returns;
static void tiny_stack_worker(void *arg) {
    (void)arg;atomic_fetch_add(&tiny_stack_returns,1);
}
static void native_clock_check(uint64_t *, uint64_t *);
static void event_worker(void *arg) {
    uint64_t a, b; native_clock_check(&a, &b);
    uint64_t start = now_ns();
    atomic_store(&worker_started, 1);
    event_wait(arg);
    atomic_store(&worker_wait_ns, now_ns() - start);
    atomic_store(&worker_done, 1);
}
static uintptr_t host_x18(void) { uintptr_t v; __asm__ volatile("mov %0,x18" : "=r"(v)); return v; }
static void *platform_worker(void *arg) {
    uintptr_t before = (uintptr_t)__builtin_thread_pointer(), x18 = host_x18();
    crash_thread_init(); guest_thread_init();
    dm_android_slot_set(g_key18, arg);
    if (dm_android_slots()[1] != (uintptr_t)arg || (uintptr_t)__builtin_thread_pointer() != before || host_x18() != x18)
        die("Android worker guest slots damaged host context");
    if (dm_android_gate_check() == 0x7777) die("Android gate did not restore modified x19");
    return arg;
}
static int platform_check(const char *path) {
    uintptr_t before = (uintptr_t)__builtin_thread_pointer(), x18 = host_x18();
    dm_android_loader_keys_init(); guest_thread_init();
    errno = E2BIG;
    dm_android_slot_set(g_key18, (void *)0x1234);
    if (errno != E2BIG || dm_android_slots()[1] != 0x1234 || (uintptr_t)__builtin_thread_pointer() != before || host_x18() != x18)
        die("Android guest slots damaged host context or errno");
    gate_register((uintptr_t)&dm_android_gate_check_return, "cntpct", 19, 0);
    uint64_t tick = dm_android_gate_check();
    if (!tick || tick == 0x7777) die("Android gate result incorrect");
    for (uintptr_t i = 1; i <= 3; ++i) {
        pthread_t thread; void *result = NULL;
        if (pthread_create(&thread, NULL, platform_worker, (void *)i) || pthread_join(thread, &result) || result != (void *)i)
            die("Android guest slot worker failed");
        if (dm_android_slots()[1] != 0x1234) die("Android guest slot isolation failed");
    }
    FILE *out = fopen(path, "w"); if (!out) die("Android platform result open");
    fprintf(out,"{\"passed\":true,\"mode\":\"platform\",\"host_tp_preserved\":true,\"host_x18_preserved\":true,\"errno_preserved\":true,\"tls_slot\":2,\"guest_threads_checked\":3,\"gate_x19_restored\":true,\"original_cpu_execution\":false,\"full_game_verified\":false}\n");
    if (fclose(out)) die("Android platform result close");
    dm_android_slots_detach();
    LOG("ANDROID_PLATFORM_CHECK passed");
    return 0;
}
static void *required(const char *name) {
    void *p = guest_sym(name); if (!p) die("Android required original symbol missing: %s", name); return p;
}
static void native_clock_check(uint64_t *first, uint64_t *last) {
    uint64_t (*tick)(void) = required("_ZN2nn2os13GetSystemTickEv");
    *first = tick();
    struct timespec sleep = {.tv_nsec = 2000000}; nanosleep(&sleep, NULL);
    *last = tick();
    if (*last <= *first || *last - *first < 10000) die("Original SDK tick did not advance");
    void *(*current_thread)(void) = required("_ZN2nn2os16GetCurrentThreadEv");
    uintptr_t *s = dm_android_slots();
    void *expected = *(void **)((uint8_t *)s[3] + 0x1f8);
    if (current_thread() != expected) die("Original SDK TLR current thread mismatch");
}
static void original_os_services_check(void) {
    for(unsigned i=0;i<32;++i) {
        void *thread=dm_guest_thread_spawn(tiny_stack_worker,NULL,16,4096);
        dm_guest_thread_join(thread);
    }
    if(atomic_load(&tiny_stack_returns)!=32)die("4 KiB original SDK thread teardown failed");
    LOG("ANDROID_TINY_STACK_EXIT original_sdk_threads=32 stack_bytes=4096 passed=true");
    uint32_t (*create)(void *,int,int) = required("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    void (*signal_event)(void *) = required("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    void (*clear_event)(void *) = required("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE");
    void (*destroy_event)(void *) = required("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    int (*try_wait)(void *) = required("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE");
    event_wait = required("_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE");
    int h0,l0,o0; thr_counts(&h0,&l0,&o0);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        unsigned char event[256] __attribute__((aligned(16))) = {0};
        if (create(event,0,1)) die("Original SDK CreateSystemEvent failed");
        if (try_wait(event) & 1) die("Original SDK fresh event is signaled");
        atomic_store(&worker_started,0); atomic_store(&worker_done,0); atomic_store(&worker_wait_ns,0);
        uint64_t wait_count = dm_android_svc_count(0x18);
        void *thread = dm_guest_thread_spawn(event_worker,event,16,64*1024);
        uint64_t deadline = now_ns() + 2000000000;
        struct timespec small = {.tv_nsec=1000000};
        while (!atomic_load(&worker_started) || dm_android_svc_count(0x18) <= wait_count) {
            if (now_ns() > deadline) die("Original SDK worker did not enter event wait");
            nanosleep(&small,NULL);
        }
        struct timespec delay = {.tv_nsec=20000000}; nanosleep(&delay,NULL);
        if (atomic_load(&worker_done)) die("Original SDK wait returned before event signal");
        signal_event(event); dm_guest_thread_join(thread);
        if (!atomic_load(&worker_done) || atomic_load(&worker_wait_ns) < 10000000) die("Original SDK event wait did not complete correctly");
        if (!(try_wait(event)&1)) die("Original SDK manual event lost signal");
        clear_event(event);
        if (try_wait(event)&1) die("Original SDK event clear failed");
        destroy_event(event);
        int h1,l1,o1; thr_counts(&h1,&l1,&o1);
        if (h0!=h1 || l0!=l1 || o0!=o1) die("Original SDK thread/event resources leaked");
    }
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--platform-services-check")) {
        g_t0_ns=now_ns(); crash_thread_init(); fixture_platform_context(argv[2]);
        return dm_android_platform_verify(argv[3]);
    }
    if (argc == 4 && !strcmp(argv[1], "--filesystem-check")) {
        g_t0_ns = now_ns(); crash_thread_init(); return dm_android_fs_verify(argv[2], argv[3]);
    }
    if (argc == 3 && !strcmp(argv[1], "--platform-check")) {
        g_t0_ns = now_ns(); crash_thread_init(); return platform_check(argv[2]);
    }
    if (argc != 4 && argc != 5) { fprintf(stderr, "usage: dm_android_host_probe PATCH_DIR RESULT_JSON load|os-init|os-services|sdk-startup [FIXTURE_BASE]\n"); return 2; }
    if (strcmp(argv[3], "load") && strcmp(argv[3], "os-init") && strcmp(argv[3], "os-services") && strcmp(argv[3],"sdk-startup")) return 2;
    if (!strcmp(argv[3],"sdk-startup") && argc!=5) return 2;
    g_flag.imports = getenv("DM_IMPORTS")!=NULL; g_t0_ns = now_ns(); crash_thread_init();
    const char *names[] = {"main", "sdk", "subsdk0"};
    load_modules(argv[1], names, 3);
    guest_thread_init();
    if (!strcmp(argv[3],"sdk-startup")) fixture_platform_context(argv[4]);
    uint64_t tick0, tick1; native_clock_check(&tick0, &tick1);
    int initialized = 0;
    if (strcmp(argv[3],"load")) {
        uint32_t handle = svc_thread_main_init();
        ((void (*)(void))required("_init_libc0"))();
        LOG("ANDROID_ORIGINAL nnosInitialize start handle=%u", handle);
        ((void (*)(uint64_t,uint64_t))required("nnosInitialize"))(handle,0);
        ((void (*)(void))required("_init_libc1"))();
        initialized = 1;
        LOG("ANDROID_ORIGINAL nnosInitialize returned");
        native_clock_check(&tick0, &tick1);
    }
    unsigned service_cycles = 0;
    if (!strcmp(argv[3], "os-services")) { original_os_services_check(); service_cycles = 3; }
    unsigned startup_stages = 0;
    if (!strcmp(argv[3],"sdk-startup")) {
        LOG("ANDROID_ORIGINAL nninitInitializeSdkModule start");
        ((void (*)(void))required("nninitInitializeSdkModule"))(); ++startup_stages;
        LOG("ANDROID_ORIGINAL nninitStartup start");
        ((void (*)(void))required("nninitStartup"))(); ++startup_stages;
        ((void (*)(void))required("_init_libc2"))(); ++startup_stages;
        native_clock_check(&tick0,&tick1);
    }
    FILE *out = fopen(argv[2], "w"); if (!out) die("Android result open");
    fprintf(out,"{\"passed\":true,\"mode\":\"%s\",\"modules\":%d,\"original_cpu_execution\":true,\"original_sdk_tls_checked\":true,\"tick0\":%" PRIu64 ",\"tick1\":%" PRIu64 ",\"nnos_initialized\":%s,\"get_info_calls\":%" PRIu64 ",\"original_thread_event_cycles\":%u,\"sdk_startup_stages\":%u,\"fixture_context\":%s,\"full_game_verified\":false}\n",argv[3],g_nmod,tick0,tick1,initialized?"true":"false",dm_android_svc_count(0x29),service_cycles,startup_stages,startup_stages?"true":"false");
    if (fclose(out)) die("Android result close");
    LOG("ANDROID_HOST_PROBE passed mode=%s", argv[3]);
    return 0;
}
