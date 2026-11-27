// iOS front_ios.m consumes the same host drop event; OHOS uses its phone vibrator.
// Three tiers get three waveforms (custom vibration sequences, OH_Vibrator_PlayVibrationCustom): heavy = thump + resonant swell +
// second thump, medium = thump + short swell, light = one crisp tap. Where custom sequences are unsupported or fail, the former
// durations (35 / 22 / 12 ms) are used.
#include "haptics_ohos.h"
#include <sensors/vibrator.h>
#include <stdatomic.h>
#include <hilog/log.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
extern int dm_drop_haptic_take(void);
static _Atomic int active = 1, last_error, custom_ok = 1;
static char dir[1100];
static const char *const pattern[4] = {NULL,
    "{\"MetaData\":{\"Version\":1.0,\"ChannelNumber\":1,\"Description\":\"dm drop heavy\"},\"Channels\":[{\"Parameters\":{\"Index\":0},\"Pattern\":["
    "{\"Event\":{\"Type\":\"transient\",\"StartTime\":0,\"Parameters\":{\"Frequency\":40,\"Intensity\":100}}},"
    "{\"Event\":{\"Type\":\"continuous\",\"StartTime\":25,\"Duration\":90,\"Parameters\":{\"Frequency\":55,\"Intensity\":85,\"Curve\":["
    "{\"Time\":0,\"Frequency\":0,\"Intensity\":1},{\"Time\":30,\"Frequency\":0,\"Intensity\":1},{\"Time\":60,\"Frequency\":-10,\"Intensity\":0.6},{\"Time\":90,\"Frequency\":-20,\"Intensity\":0}]}}},"
    "{\"Event\":{\"Type\":\"transient\",\"StartTime\":140,\"Parameters\":{\"Frequency\":40,\"Intensity\":100}}}]}]}",
    "{\"MetaData\":{\"Version\":1.0,\"ChannelNumber\":1,\"Description\":\"dm drop medium\"},\"Channels\":[{\"Parameters\":{\"Index\":0},\"Pattern\":["
    "{\"Event\":{\"Type\":\"transient\",\"StartTime\":0,\"Parameters\":{\"Frequency\":50,\"Intensity\":90}}},"
    "{\"Event\":{\"Type\":\"continuous\",\"StartTime\":20,\"Duration\":40,\"Parameters\":{\"Frequency\":55,\"Intensity\":55,\"Curve\":["
    "{\"Time\":0,\"Frequency\":0,\"Intensity\":1},{\"Time\":15,\"Frequency\":0,\"Intensity\":0.8},{\"Time\":30,\"Frequency\":0,\"Intensity\":0.4},{\"Time\":40,\"Frequency\":0,\"Intensity\":0}]}}}]}]}",
    "{\"MetaData\":{\"Version\":1.0,\"ChannelNumber\":1,\"Description\":\"dm drop light\"},\"Channels\":[{\"Parameters\":{\"Index\":0},\"Pattern\":["
    "{\"Event\":{\"Type\":\"transient\",\"StartTime\":0,\"Parameters\":{\"Frequency\":70,\"Intensity\":60}}}]}]}",
};
void dm_ohos_haptics_dir(const char *files) {
    snprintf(dir, sizeof dir, "%s/dm", files ? files : "");
}
static int play_custom(int tier) {
    if (!dir[0] || !atomic_load(&custom_ok)) return -1;
    char path[1200]; snprintf(path, sizeof path, "%s/haptic-drop-%d.json", dir, tier);
    struct stat st; size_t n = strlen(pattern[tier]);
    if (stat(path, &st) || (size_t)st.st_size != n) {
        FILE *f = fopen(path, "wb"); if (!f) return -1;
        int ok = fwrite(pattern[tier], 1, n, f) == n; ok &= fclose(f) == 0; if (!ok) return -1;
    }
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    Vibrator_FileDescription d = { .fd = fd, .offset = 0, .length = (int64_t)n };
    Vibrator_Attribute attr = { .vibratorId = 0, .usage = VIBRATOR_USAGE_PHYSICAL_FEEDBACK };
    int rc = OH_Vibrator_PlayVibrationCustom(d, attr);
    close(fd);
    if (rc) { atomic_store(&custom_ok, 0); OH_LOG_Print(LOG_APP, LOG_WARN, 0xD2E0, "dm-haptics", "custom vibration rc=%{public}d, using durations", rc); }
    else { static _Atomic int told; if (!atomic_exchange(&told, 1)) fprintf(stderr, "震动：掉落三档用自定义波形（第 %d 档先到）\n", tier); }
    return rc;
}
void dm_ohos_haptics_focus(int value) {
    atomic_store(&active, !!value);
    if (!value) { dm_drop_haptic_take(); OH_Vibrator_Cancel(); }
}
void dm_ohos_haptics_tick(void) {
    int tier = dm_drop_haptic_take();
    if (!atomic_load(&active) || tier < 1 || tier > 3) return;
    if (play_custom(tier) == 0) { atomic_store(&last_error, 0); return; }
    const int duration[] = {0, 35, 22, 12};
    Vibrator_Attribute attr = { .vibratorId = 0, .usage = VIBRATOR_USAGE_PHYSICAL_FEEDBACK };
    int rc = OH_Vibrator_PlayVibration(duration[tier], attr);
    int previous = atomic_exchange(&last_error, rc);
    if (rc && rc != previous) OH_LOG_Print(LOG_APP, LOG_WARN, 0xD2E0, "dm-haptics", "vibration failed rc=%{public}d", rc);
}
int dm_ohos_haptics_last_error(void) { return atomic_load(&last_error); }
