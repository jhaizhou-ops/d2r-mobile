#include "../host/dm.h"
#include "services_platform.h"
#include "services_sdk.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1u
#endif

static pthread_mutex_t platform_lock = PTHREAD_MUTEX_INITIALIZER;
static dm_android_platform_config configuration;
static dm_android_window_state window;
static dm_android_platform_status status;
static char profile_directory[PATH_MAX];
static int content_indices[64], aoc_initialized, error_fd = -1, account_initialized;
static unsigned network_initialized[3], network_submissions, network_cancellations;
static struct { char name[64]; int value; } settings[32];
static unsigned settings_count;
static struct { const void *address; int kind, ready; } objects[256];
static struct { uint64_t token; } users[32];
static uint64_t next_user_token = 1;
static struct profile {
    char magic[8];
    uint64_t uid[2];
    uint8_t device_uuid[16];
    char nickname[33];
    uint8_t padding[7];
} profile;
_Static_assert(sizeof profile == 80, "Persistent offline profile ABI");
static int platform_error(const char *stage, int error) {
    LOG("ANDROID_PLATFORM_BIND failed stage=%s error=%d uid=%lu", stage, error, (unsigned long)getuid());
    return error;
}
static int profile_error(const char *operation, const char *path, int error) {
    LOG("ANDROID_PROFILE syscall=%s path=%s errno=%d process_uid=%lu", operation, path, error, (unsigned long)getuid());
    return error;
}

static int full_io(int fd, void *buffer, size_t size, int writing) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = writing ? write(fd, (char *)buffer + done, size - done) : read(fd, (char *)buffer + done, size - done);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (!n) { errno = writing ? ENOSPC : EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}
static int entropy(void *buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t n = getrandom((char *)buffer + done, size - done, 0);
        if (n < 0) { if (errno == EINTR) continue; return errno; }
        if (!n) return EIO;
        done += (size_t)n;
    }
    return 0;
}
static int load_profile(void) { // caller holds platform_lock
    int created_directory = mkdir(profile_directory, 0700) == 0;
    if (!created_directory && errno != EEXIST) return profile_error("mkdir", profile_directory, errno);
    if (created_directory) {
        char *parent_path = strdup(profile_directory);
        if (!parent_path) return profile_error("strdup-parent", profile_directory, ENOMEM);
        char *slash = strrchr(parent_path, '/');
        if (!slash) { free(parent_path); return profile_error("parent-path", profile_directory, EINVAL); }
        if (slash == parent_path) slash[1] = 0; else *slash = 0;
        int parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        free(parent_path);
        if (parent < 0) return profile_error("open-parent", profile_directory, errno);
        int rc = fsync(parent), error = errno;
        close(parent);
        if (rc) return profile_error("fsync-parent", profile_directory, error);
    }
    int dir = open(profile_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return profile_error("open-directory", profile_directory, errno);
    struct stat st;
    if (fstat(dir, &st)) { int error = errno; close(dir); return profile_error("fstat-directory", profile_directory, error); }
    LOG("ANDROID_PROFILE directory=%s owner=%lu mode=%#o expected_uid=%lu", profile_directory, (unsigned long)st.st_uid, st.st_mode & 07777, (unsigned long)getuid());
    if (st.st_uid != getuid()) { close(dir); return profile_error("directory-owner", profile_directory, EACCES); }
    int fd = openat(dir, "offline-profile.bin", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) {
        memset(&profile, 0, sizeof profile);
        memcpy(profile.magic, "DMUSR001", 8);
        int rc = entropy(profile.uid, sizeof profile.uid);
        if (!rc) rc = entropy(profile.device_uuid, sizeof profile.device_uuid);
        if (rc) { close(dir); return profile_error("getrandom-profile-identity", profile_directory, rc); }
        profile.device_uuid[6] = (profile.device_uuid[6] & 15u) | 64u;
        profile.device_uuid[8] = (profile.device_uuid[8] & 63u) | 128u;
        const char *nick = getenv("DM_NICK");
        nick = nick && *nick ? nick : "Player";
        if (!dm_android_platform_utf8_valid(nick)) { close(dir); return profile_error("nickname-utf8", profile_directory, EINVAL); }
        size_t nickname_length = strlen(nick);
        if (nickname_length > 32) {
            nickname_length = 32;
            while (nickname_length && (((unsigned char)nick[nickname_length] & 0xc0u) == 0x80u)) nickname_length--;
        }
        memcpy(profile.nickname, nick, nickname_length);
        uint32_t nonce;
        rc = entropy(&nonce, sizeof nonce);
        if (rc) { close(dir); return profile_error("getrandom-temporary-name", profile_directory, rc); }
        char temporary[64]; snprintf(temporary, sizeof temporary, "offline-profile.%d.%08x.new", getpid(), nonce);
        int temporary_fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (temporary_fd < 0) { int error = errno; close(dir); return profile_error("openat-temporary", temporary, error); }
        rc = full_io(temporary_fd, &profile, sizeof profile, 1);
        int error = errno;
        if (rc) profile_error("write-profile", temporary, error);
        if (!rc && fsync(temporary_fd)) { rc = -1; error = errno; profile_error("fsync-profile", temporary, error); }
        if (close(temporary_fd) && !rc) { rc = -1; error = errno; profile_error("close-profile", temporary, error); }
        int published = 0;
        if (!rc) {
            // Android's policy can deny linkat even in an owned directory. Use
            // an atomic rename that never replaces an existing profile. On a
            // concurrent EEXIST winner, read that winner after directory sync.
            if (!syscall(__NR_renameat2, dir, temporary, dir, "offline-profile.bin", RENAME_NOREPLACE)) published = 1;
            else if (errno != EEXIST) { rc = -1; error = errno; profile_error("renameat2-publish-noreplace", temporary, error); }
        }
        if (!published && unlinkat(dir, temporary, 0)) profile_error("unlinkat-temporary", temporary, errno);
        if (!rc && fsync(dir)) { rc = -1; error = errno; profile_error("fsync-profile-directory", profile_directory, error); }
        if (rc) { close(dir); return error; }
        fd = openat(dir, "offline-profile.bin", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    }
    if (fd < 0) { int error = errno; close(dir); return profile_error("openat-profile", profile_directory, error); }
    int rc = fstat(fd, &st);
    int error = errno;
    if (rc) profile_error("fstat-profile", profile_directory, error);
    if (!rc && (!S_ISREG(st.st_mode) || st.st_size != sizeof profile)) { rc = -1; error = EINVAL; profile_error("profile-type-or-size", profile_directory, error); }
    if (!rc && full_io(fd, &profile, sizeof profile, 0)) { rc = -1; error = errno; profile_error("read-profile", profile_directory, error); }
    close(fd); close(dir);
    if (rc) return error ? error : EIO;
    if (memcmp(profile.magic, "DMUSR001", 8) || !(profile.uid[0] | profile.uid[1]) || !memchr(profile.nickname, 0, sizeof profile.nickname) || !dm_android_platform_utf8_valid(profile.nickname)) return profile_error("profile-validation", profile_directory, EINVAL);
    return 0;
}

int dm_android_platform_bind(const dm_android_platform_config *config) {
    if (!config || !config->profile_directory || config->profile_directory[0] != '/' ||
        strlen(config->profile_directory) >= sizeof profile_directory ||
        (config->mode != DM_ANDROID_HOST_SHELL && config->mode != DM_ANDROID_HOST_ACTIVITY) ||
        config->add_on_content_count > 64 || (config->add_on_content_count && !config->add_on_content_indices) ||
        (config->prefetch_present && !config->stop_prefetch) || (config->audio_present && !config->pause_audio) ||
        (config->graphics_present && !config->suspend_and_drain_graphics) ||
        (config->locale && (!*config->locale || strlen(config->locale) >= sizeof status.locale ||
                            !config->locale_source || !*config->locale_source))) return platform_error("configuration", EINVAL);
    pthread_mutex_lock(&platform_lock);
    if (status.bound) { pthread_mutex_unlock(&platform_lock); return platform_error("already-bound", EALREADY); }
    configuration = *config;
    strlcpy(profile_directory, config->profile_directory, sizeof profile_directory);
    configuration.profile_directory = profile_directory;
    for (size_t i = 0; i < config->add_on_content_count; i++) {
        if (config->add_on_content_indices[i] < 0 || (i && config->add_on_content_indices[i] <= config->add_on_content_indices[i - 1])) {
            pthread_mutex_unlock(&platform_lock); return platform_error("content-list-order", EINVAL);
        }
        content_indices[i] = config->add_on_content_indices[i];
    }
    configuration.add_on_content_indices = content_indices;
    status.core_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
    long pages = sysconf(_SC_PHYS_PAGES), page_size = sysconf(_SC_PAGESIZE);
    if (status.core_count <= 0 || pages <= 0 || page_size <= 0) { pthread_mutex_unlock(&platform_lock); return platform_error("device-memory-cpu-query", EIO); }
    status.physical_memory_bytes = (uint64_t)pages * (uint64_t)page_size;
    char property[PROP_VALUE_MAX] = {0};
    if (!__system_property_get("ro.product.model", property)) { pthread_mutex_unlock(&platform_lock); return platform_error("device-model-property", ENODATA); }
    strlcpy(status.device_model, property, sizeof status.device_model);
    if (config->locale) {
        strlcpy(status.locale, config->locale, sizeof status.locale);
        strlcpy(status.locale_source, config->locale_source, sizeof status.locale_source);
    } else {
        if (!__system_property_get("persist.sys.locale", property)) {
            pthread_mutex_unlock(&platform_lock); return platform_error("current-locale-property-requires-Configuration-or-Settings", ENODATA);
        }
        strlcpy(status.locale, property, sizeof status.locale);
        strlcpy(status.locale_source, "persist.sys.locale", sizeof status.locale_source);
    }
    configuration.locale = status.locale; configuration.locale_source = status.locale_source;
    int profile_error = load_profile();
    if (profile_error) { pthread_mutex_unlock(&platform_lock); return platform_error("persistent-profile", profile_error); }
    window.focus = 3; window.thermal_status = -1;
    status.bound = 1;
    pthread_mutex_unlock(&platform_lock);
    return 0;
}
int dm_android_platform_update_window(const dm_android_window_state *state) {
    if (!state || state->focus < 1 || state->focus > 3 || (state->surface_available && (!state->width || !state->height || state->density <= 0 || state->refresh_hz <= 0))) return EINVAL;
    pthread_mutex_lock(&platform_lock);
    if (!status.bound || configuration.mode != DM_ANDROID_HOST_ACTIVITY) { pthread_mutex_unlock(&platform_lock); return ENODEV; }
    uint64_t generation = window.generation + 1;
    window = *state; window.generation = generation;
    status.surface_available = state->surface_available != 0;
    pthread_mutex_unlock(&platform_lock);
    void dm_focus_set(int); dm_focus_set(state->focus);
    return 0;
}
void dm_android_platform_get_window(dm_android_window_state *out) {
    if (!out) die("Android window status output missing");
    pthread_mutex_lock(&platform_lock); *out = window; pthread_mutex_unlock(&platform_lock);
}
void dm_android_platform_get_status(dm_android_platform_status *out) {
    if (!out) die("Android platform status output missing");
    pthread_mutex_lock(&platform_lock);
    *out = status;
    out->account_initialized = account_initialized;
    out->content_initialized = aoc_initialized;
    out->error_recorder_ready = error_fd >= 0;
    pthread_mutex_unlock(&platform_lock);
}
int dm_android_platform_oe_init(void *context) {
    (void)context;
    pthread_mutex_lock(&platform_lock);
    int rc = !status.bound ? ENODEV : 0;
    int focus = window.focus;
    if (!rc) status.oe_initialized = 1;
    pthread_mutex_unlock(&platform_lock);
    if (!rc) { void dm_focus_set(int); dm_focus_set(focus); }
    return rc;
}
int dm_android_platform_aoc_init(void *context) {
    (void)context;
    pthread_mutex_lock(&platform_lock);
    int rc = !status.bound ? ENODEV : !configuration.content_manifest_verified ? EAGAIN : 0;
    if (!rc) aoc_initialized = 1;
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_error_init(void *context) {
    (void)context;
    pthread_mutex_lock(&platform_lock);
    if (!status.bound) { pthread_mutex_unlock(&platform_lock); return ENODEV; }
    int dir = open(profile_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) { int error = errno; pthread_mutex_unlock(&platform_lock); return error; }
    error_fd = openat(dir, "guest-errors.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    int error = error_fd < 0 ? errno : 0;
    if (!error && (fsync(error_fd) || fsync(dir))) {
        error = errno; close(error_fd); error_fd = -1;
    }
    close(dir); pthread_mutex_unlock(&platform_lock); return error;
}
int dm_android_platform_parental_init(void *context) {
    (void)context;
    pthread_mutex_lock(&platform_lock);
    int rc = !status.bound ? ENODEV : 0;
    if (!rc) { status.parental_policy_initialized = 1; status.online_requests_enabled = 0; }
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_abort_init(void *context) {
    (void)context;
    extern int dm_android_diagnostics_ready(void) __attribute__((weak));
    return dm_android_diagnostics_ready && dm_android_diagnostics_ready() ? 0 : ENODEV;
}
int dm_android_platform_register_services(void) {
    pthread_mutex_lock(&platform_lock);
    if (!status.bound) { pthread_mutex_unlock(&platform_lock); return ENODEV; }
    pthread_mutex_unlock(&platform_lock);
    dm_android_service_register(DM_ANDROID_SERVICE_OE, dm_android_platform_oe_init, NULL);
    dm_android_service_register(DM_ANDROID_SERVICE_AOC, dm_android_platform_aoc_init, NULL);
    dm_android_service_register(DM_ANDROID_SERVICE_ERROR_CONTEXT, dm_android_platform_error_init, NULL);
    dm_android_service_register(DM_ANDROID_SERVICE_PARENTAL_CONTROL, dm_android_platform_parental_init, NULL);
    dm_android_service_register(DM_ANDROID_SERVICE_ABORT_OBSERVER, dm_android_platform_abort_init, NULL);
    return 0;
}
int dm_android_platform_account_init(void) {
    pthread_mutex_lock(&platform_lock);
    int rc = !status.bound ? ENODEV : account_initialized ? 0 : load_profile();
    if (!rc) account_initialized = 1;
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_account_uid(uint64_t out[2]) {
    pthread_mutex_lock(&platform_lock);
    int rc = !account_initialized || !out ? EINVAL : 0;
    if (!rc) memcpy(out, profile.uid, sizeof profile.uid);
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_account_open(const uint64_t uid[2], uint64_t handle[3]) {
    pthread_mutex_lock(&platform_lock);
    int rc = EINVAL;
    if (account_initialized && uid && handle && !memcmp(uid, profile.uid, sizeof profile.uid)) {
        rc = EMFILE;
        for (size_t i = 0; i < sizeof users / sizeof *users; i++) if (!users[i].token) {
            users[i].token = next_user_token++;
            if (!next_user_token) die("Android offline user token exhausted");
            handle[0] = profile.uid[0]; handle[1] = profile.uid[1]; handle[2] = users[i].token;
            rc = 0; break;
        }
    }
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_account_close(uint64_t handle[3]) {
    if (!handle) return EINVAL;
    pthread_mutex_lock(&platform_lock);
    int rc = EINVAL;
    for (size_t i = 0; i < sizeof users / sizeof *users; i++) if (users[i].token && users[i].token == handle[2] && !memcmp(handle, profile.uid, sizeof profile.uid)) {
        users[i].token = 0; memset(handle, 0, 24); rc = 0; break;
    }
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_account_handle_uid(const uint64_t handle[3], uint64_t out[2]) {
    if (!handle || !out) return EINVAL;
    pthread_mutex_lock(&platform_lock);
    int rc = EINVAL;
    if (!memcmp(handle, profile.uid, sizeof profile.uid))
        for (size_t i = 0; i < sizeof users / sizeof *users; i++) if (users[i].token && users[i].token == handle[2]) {
            memcpy(out, profile.uid, sizeof profile.uid); rc = 0; break;
        }
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_account_open_count(void) {
    pthread_mutex_lock(&platform_lock);
    int count = 0;
    for (size_t i = 0; i < sizeof users / sizeof *users; i++) count += users[i].token != 0;
    pthread_mutex_unlock(&platform_lock); return count;
}
int dm_android_platform_account_nickname(const uint64_t uid[2], char out[33]) {
    pthread_mutex_lock(&platform_lock);
    int rc = !account_initialized || !uid || !out || memcmp(uid, profile.uid, sizeof profile.uid) ? EINVAL : 0;
    if (!rc) memcpy(out, profile.nickname, 33);
    pthread_mutex_unlock(&platform_lock); return rc;
}
void dm_android_platform_device_uuid(uint8_t out[16]) {
    if (dm_android_platform_account_init()) die("Android persistent device identity unavailable");
    pthread_mutex_lock(&platform_lock); memcpy(out, profile.device_uuid, 16); pthread_mutex_unlock(&platform_lock);
}
uint64_t dm_android_platform_language_code(void) {
    char code[8] = {0};
    pthread_mutex_lock(&platform_lock);
    if (!status.bound) die("Android locale backend unavailable");
    const char *configured = getenv("DM_LANG");
    const char *locale = configured ? configured : status.locale;
    if (!strncmp(locale, "zh", 2)) strlcpy(code, strstr(locale, "TW") || strstr(locale, "HK") || strstr(locale, "Hant") ? "zh-Hant" : "zh-Hans", sizeof code);
    else if (!strncmp(locale, "pt", 2) && strstr(locale, "BR")) strlcpy(code, "pt-BR", sizeof code);
    else if (!strncmp(locale, "es", 2) && strstr(locale, "MX")) strlcpy(code, "es-419", sizeof code);
    else if (!strncmp(locale, "en", 2) && strstr(locale, "GB")) strlcpy(code, "en-GB", sizeof code);
    else { size_t length = strcspn(locale, "-_"); if (length >= sizeof code) length = sizeof code - 1; memcpy(code, locale, length); }
    pthread_mutex_unlock(&platform_lock);
    uint64_t result; memcpy(&result, code, sizeof result); return result;
}
int dm_android_platform_aoc_count(void) {
    pthread_mutex_lock(&platform_lock);
    if (!aoc_initialized) die("Android content manifest not initialized");
    int count = (int)configuration.add_on_content_count;
    pthread_mutex_unlock(&platform_lock); return count;
}
int dm_android_platform_aoc_list(int *out, int offset, int capacity) {
    if (offset < 0 || capacity < 0 || (capacity && !out)) die("Invalid Android add-on content list range");
    pthread_mutex_lock(&platform_lock);
    if (!aoc_initialized) die("Android content manifest not initialized");
    int count = 0;
    for (int i = offset; i < (int)configuration.add_on_content_count && count < capacity; i++) out[count++] = content_indices[i];
    pthread_mutex_unlock(&platform_lock); return count;
}
void dm_android_platform_record_error(uint32_t result, const char *context) {
    pthread_mutex_lock(&platform_lock);
    if (error_fd < 0) die("Android error recorder is not initialized");
    char line[256]; int length = snprintf(line, sizeof line, "monotonic_ns=%llu result=%#x context=%s\n", (unsigned long long)now_ns(), result, context ? context : "unknown");
    if (length <= 0 || full_io(error_fd, line, (size_t)length < sizeof line ? (size_t)length : sizeof line - 1, 1) || fsync(error_fd)) die("Android error record write failed errno=%d", errno);
    pthread_mutex_unlock(&platform_lock);
}
int dm_android_platform_network_init(int subsystem) {
    if (subsystem < 0 || subsystem > 2) return EINVAL;
    pthread_mutex_lock(&platform_lock);
    int rc = !status.bound ? ENODEV : 0;
    if (!rc) { network_initialized[subsystem]++; status.offline_policy_initialized = 1; status.online_requests_enabled = 0; }
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_network_finalize(int subsystem) {
    if (subsystem < 0 || subsystem > 2) return EINVAL;
    pthread_mutex_lock(&platform_lock);
    int rc = !network_initialized[subsystem] ? EINVAL : 0;
    if (!rc) network_initialized[subsystem]--;
    status.offline_policy_initialized = (network_initialized[0] || network_initialized[1] || network_initialized[2]) != 0;
    pthread_mutex_unlock(&platform_lock); return rc;
}
void dm_android_platform_network_request(int submit) {
    pthread_mutex_lock(&platform_lock);
    if (!network_initialized[1]) die("Android offline network manager is not initialized");
    if (submit) network_submissions++; else network_cancellations++;
    pthread_mutex_unlock(&platform_lock);
}
int dm_android_platform_network_ready(void) {
    pthread_mutex_lock(&platform_lock);
    int ready = status.offline_policy_initialized;
    pthread_mutex_unlock(&platform_lock); return ready;
}
void dm_android_platform_setting(const char *name, int value) {
    pthread_mutex_lock(&platform_lock);
    if (!status.bound) die("Android platform configuration not bound");
    unsigned i;
    for (i = 0; i < settings_count; i++) if (!strcmp(settings[i].name, name)) break;
    if (i == settings_count) {
        if (settings_count == sizeof settings / sizeof *settings) die("Android platform settings table full");
        strlcpy(settings[settings_count++].name, name, sizeof settings[0].name);
    }
    settings[i].value = value;
    pthread_mutex_unlock(&platform_lock);
}
int dm_android_platform_setting_get(const char *name, int *value) {
    if (!name || !value) return EINVAL;
    pthread_mutex_lock(&platform_lock);
    int rc = ENOENT;
    for (unsigned i = 0; i < settings_count; i++) if (!strcmp(settings[i].name, name)) { *value = settings[i].value; rc = 0; break; }
    pthread_mutex_unlock(&platform_lock); return rc;
}
int dm_android_platform_shutdown(void) {
    pthread_mutex_lock(&platform_lock);
    if (!status.bound) { pthread_mutex_unlock(&platform_lock); return ENODEV; }
    dm_android_platform_config callbacks = configuration;
    pthread_mutex_unlock(&platform_lock);
    int rc;
    if (callbacks.prefetch_present && (rc = callbacks.stop_prefetch())) return rc;
    if (callbacks.audio_present && (rc = callbacks.pause_audio(1))) return rc;
    if (callbacks.graphics_present && (rc = callbacks.suspend_and_drain_graphics(1))) return rc;
    if (callbacks.flush_diagnostics && (rc = callbacks.flush_diagnostics())) return rc;
    pthread_mutex_lock(&platform_lock);
    rc = error_fd >= 0 && fsync(error_fd) ? errno : 0;
    pthread_mutex_unlock(&platform_lock); return rc;
}
void dm_android_platform_object_create(void *object, int kind) {
    if (!object || kind <= 0) die("Invalid Android service object");
    pthread_mutex_lock(&platform_lock);
    if (!status.bound) die("Android service object created before platform bind");
    int slot = -1;
    for (size_t i = 0; i < sizeof objects / sizeof *objects; i++) {
        if (objects[i].address == object) die("Android service object constructed twice");
        if (!objects[i].address && slot < 0) slot = (int)i;
    }
    if (slot < 0) die("Android service object table full");
    objects[slot].address = object; objects[slot].kind = kind;
    pthread_mutex_unlock(&platform_lock);
}
void dm_android_platform_object_destroy(void *object, int kind) {
    pthread_mutex_lock(&platform_lock);
    for (size_t i = 0; i < sizeof objects / sizeof *objects; i++) if (objects[i].address == object && objects[i].kind == kind) {
        memset(&objects[i], 0, sizeof objects[i]); pthread_mutex_unlock(&platform_lock); return;
    }
    die("Android service object destructor without matching constructor");
}
int dm_android_platform_object_exists(const void *object, int kind) {
    pthread_mutex_lock(&platform_lock);
    int found = 0;
    for (size_t i = 0; i < sizeof objects / sizeof *objects; i++) found |= objects[i].address == object && objects[i].kind == kind;
    pthread_mutex_unlock(&platform_lock); return found;
}
void dm_android_platform_object_ready(void *object, int kind) {
    pthread_mutex_lock(&platform_lock);
    for (size_t i = 0; i < sizeof objects / sizeof *objects; i++) if (objects[i].address == object && objects[i].kind == kind) {
        objects[i].ready = 1; pthread_mutex_unlock(&platform_lock); return;
    }
    die("Android service object initialization without a constructor");
}
int dm_android_platform_object_is_ready(const void *object, int kind) {
    pthread_mutex_lock(&platform_lock);
    int found = 0;
    for (size_t i = 0; i < sizeof objects / sizeof *objects; i++) found |= objects[i].address == object && objects[i].kind == kind && objects[i].ready;
    pthread_mutex_unlock(&platform_lock); return found;
}
int dm_android_platform_utf8_valid(const char *text) {
    if (!text) return 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t code; unsigned continuation;
        if (*p < 0x80) { p++; continue; }
        if (*p >= 0xc2 && *p <= 0xdf) { code = *p & 0x1f; continuation = 1; }
        else if (*p >= 0xe0 && *p <= 0xef) { code = *p & 0x0f; continuation = 2; }
        else if (*p >= 0xf0 && *p <= 0xf4) { code = *p & 7; continuation = 3; }
        else return 0;
        p++;
        for (unsigned i = 0; i < continuation; i++) {
            if (!*p || (*p & 0xc0) != 0x80) return 0;
            code = (code << 6) | (*p++ & 0x3f);
        }
        if ((continuation == 1 && code < 0x80) || (continuation == 2 && code < 0x800) ||
            (continuation == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
    }
    return 1;
}

int dm_android_platform_verify(const char *result_json) {
    unsigned checks = 0, failures = 0;
#define VERIFY(expression) do { ++checks; if (!(expression)) { ++failures; LOG("PLATFORM_SERVICE_VERIFY failed: %s", #expression); } } while (0)
    dm_android_platform_status current;
    dm_android_platform_get_status(&current);
    VERIFY(current.bound && current.core_count > 0 && current.physical_memory_bytes > 0 && *current.device_model && *current.locale);
    dm_android_window_state current_window;
    dm_android_platform_get_window(&current_window);
    if (configuration.mode == DM_ANDROID_HOST_SHELL) VERIFY(!current_window.surface_available && current_window.focus == 3 && !current_window.width && !current_window.height);
    VERIFY(!dm_android_platform_account_init());
    uint64_t uid[2] = {0}, returned[2] = {0}, user_handle[3] = {0};
    VERIFY(!dm_android_platform_account_uid(uid) && (uid[0] || uid[1]));
    VERIFY(!dm_android_platform_account_open(uid, user_handle));
    VERIFY(!dm_android_platform_account_handle_uid(user_handle, returned) && !memcmp(uid, returned, sizeof uid));
    uint64_t invalid[3]; memcpy(invalid, user_handle, sizeof invalid); invalid[0] ^= 1;
    VERIFY(dm_android_platform_account_handle_uid(invalid, returned) != 0);
    VERIFY(!dm_android_platform_account_close(user_handle));
    VERIFY(dm_android_platform_account_handle_uid(user_handle, returned) != 0);
    char nickname[33];
    VERIFY(!dm_android_platform_account_nickname(uid, nickname) && dm_android_platform_utf8_valid(nickname));
    struct profile disk = {0};
    int dir = open(profile_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int fd = dir >= 0 ? openat(dir, "offline-profile.bin", O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
    VERIFY(fd >= 0 && !full_io(fd, &disk, sizeof disk, 0) && !memcmp(disk.uid, uid, sizeof uid));
    uint8_t identity[16]; dm_android_platform_device_uuid(identity);
    VERIFY(fd >= 0 && !memcmp(disk.device_uuid, identity, sizeof identity));
    if (fd >= 0) close(fd);
    if (dir >= 0) close(dir);
    VERIFY(dm_android_platform_language_code() != 0);
    VERIFY(!dm_android_platform_network_init(1));
    dm_android_platform_network_request(1); dm_android_platform_network_request(0);
    dm_android_platform_get_status(&current);
    VERIFY(current.offline_policy_initialized && !current.online_requests_enabled);
    VERIFY(!dm_android_platform_network_finalize(1));
    dm_android_platform_setting("probe_setting", 7); int setting = 0;
    VERIFY(!dm_android_platform_setting_get("probe_setting", &setting) && setting == 7);
    char object;
    dm_android_platform_object_create(&object, 99);
    VERIFY(dm_android_platform_object_exists(&object, 99) && !dm_android_platform_object_is_ready(&object, 99));
    dm_android_platform_object_ready(&object, 99);
    VERIFY(dm_android_platform_object_is_ready(&object, 99));
    dm_android_platform_object_destroy(&object, 99);
    VERIFY(!dm_android_platform_object_exists(&object, 99));
    VERIFY(dm_android_platform_utf8_valid("Player") && dm_android_platform_utf8_valid("\xe7\x8e\xa9\xe5\xae\xb6") && !dm_android_platform_utf8_valid("\xc0\xaf") && !dm_android_platform_utf8_valid("\xed\xa0\x80"));
    FILE *output = fopen(result_json, "w"); if (!output) die("Android platform service verification result open");
    fprintf(output, "{\"passed\":%s,\"checks\":%u,\"failures\":%u,\"shell_mode\":%s,\"surface_available\":%s,\"focus\":%d,\"core_count\":%d,\"physical_memory_bytes\":%llu,\"offline_policy_only\":true,\"original_sdk_execution\":false,\"full_game_verified\":false}\n",
            failures ? "false" : "true", checks, failures, configuration.mode == DM_ANDROID_HOST_SHELL ? "true" : "false",
            current_window.surface_available ? "true" : "false", current_window.focus, current.core_count,
            (unsigned long long)current.physical_memory_bytes);
    if (fclose(output)) die("Android platform service verification result close");
    return failures ? 1 : 0;
#undef VERIFY
}
