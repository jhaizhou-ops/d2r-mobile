#pragma once
#include <stdint.h>

/* Host diagnostics only. These counters never manufacture guest progress. */
typedef struct dm_android_audio_metrics {
    uint64_t render_callbacks, device_frames_requested, game_bytes_copied;
    uint64_t buffers_released, release_events, underrun_frames, nonzero_samples;
    uint64_t error_callbacks, stream_recoveries, frames_written, frames_read;
    int open_outputs, started_outputs, muted, last_error, timestamp_valid;
    int64_t device_frame_position, device_timestamp_ns;
} dm_android_audio_metrics;

/* AAudio stream inspection is serialized with stop/close/recovery. */
int dm_android_audio_snapshot(dm_android_audio_metrics *out);

/* Real PCM sink exercise. Call only from an initialized host/guest context.
 * It uses no test SystemEvent and does not claim original-SDK event coverage.
 * Root owns whether/when this is connected to the APK. */
int dm_android_audio_probe(dm_android_audio_metrics *out);
int dm_android_audio_probe_channels(int channels, dm_android_audio_metrics *out);
