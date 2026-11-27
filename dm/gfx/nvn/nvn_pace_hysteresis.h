// Pure state for the opt-in OHOS ordinary downshift experiment.
#pragma once
typedef struct { unsigned bad_windows; } nvn_pace_hysteresis;
static inline void nvn_pace_hysteresis_reset(nvn_pace_hysteresis *s) { s->bad_windows = 0; }
static inline int nvn_pace_hysteresis_down(nvn_pace_hysteresis *s, unsigned late) {
    if (late < 6) { nvn_pace_hysteresis_reset(s); return 0; }
    if (late >= 30 || s->bad_windows) { nvn_pace_hysteresis_reset(s); return 1; }
    s->bad_windows = 1; return 0;
}

// HarmonyOS probe-up policy (H-13 B, on by default; DM_PACE_PROBE_GATE=0 / experiment bit 131072 restores the old policy).
// A probe to the faster tier only starts when the CPU side already has headroom for it: both the game thread's CPU per frame and the
// translation thread's CPU per frame at most 87% of the faster tier's period (14.5 ms for 60 fps). GPU time is not used: after a step down
// the GPU clock drops and its time stretches, so a GPU gate would lock the tier (see pace_adapt_tick). To avoid a CPU-clock lock-in the
// same way, a probe is still allowed once the stable period reaches the backoff cap. A failed probe doubles the next interval up to the cap.
// 鸿蒙试升策略：CPU 侧已有余量才试升（游戏线程与翻译线程每帧 CPU 都不超过快一档周期的 87%，60 档即 14.5 ms）；不看 GPU（降档后 GPU 降频、耗时变长，会锁死）。
// 为免 CPU 降频同样锁死，稳定时长到退避上限时照样试一次。试升失败间隔翻倍，到上限为止。
#define NVN_PACE_PROBE_CAP_NS 120000000000ull
static inline int nvn_pace_probe_allowed(double faster_period_ms, double game_ms, double xlate_ms, unsigned long long stable_ns, unsigned long long cap_ns) {
    if (stable_ns >= cap_ns) return 1;
    double lim = faster_period_ms * 0.87;
    return faster_period_ms > 0 && game_ms >= 0 && game_ms <= lim && (xlate_ms < 0 || xlate_ms <= lim);
}
static inline unsigned long long nvn_pace_backoff_next(unsigned long long b, unsigned long long cap) { return b > cap / 2 ? cap : b * 2; }
