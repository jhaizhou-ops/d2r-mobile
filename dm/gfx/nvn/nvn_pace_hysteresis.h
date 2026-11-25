// Pure state for the opt-in OHOS ordinary downshift experiment.
#pragma once
typedef struct { unsigned bad_windows; } nvn_pace_hysteresis;
static inline void nvn_pace_hysteresis_reset(nvn_pace_hysteresis *s) { s->bad_windows = 0; }
static inline int nvn_pace_hysteresis_down(nvn_pace_hysteresis *s, unsigned late) {
    if (late < 6) { nvn_pace_hysteresis_reset(s); return 0; }
    if (late >= 30 || s->bad_windows) { nvn_pace_hysteresis_reset(s); return 1; }
    s->bad_windows = 1; return 0;
}
