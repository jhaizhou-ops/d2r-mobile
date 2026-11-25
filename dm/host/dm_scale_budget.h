// Experimental GPU budget policy. CPU is submission cost, not total game-thread cost.
#ifndef DM_SCALE_BUDGET_H
#define DM_SCALE_BUDGET_H
#include <stdint.h>
typedef struct { uint64_t sample; unsigned good; } dm_scale_budget;
enum { DM_S3B_INVALID, DM_S3B_DUPLICATE, DM_S3B_HOLD, DM_S3B_CPU,
       DM_S3B_FLOOR, DM_S3B_DOWN, DM_S3B_WAIT_UP, DM_S3B_UP, DM_S3B_STABLE };
// Called once per ~2 s decision window. Thresholds are experimental, not tuned claims.
static inline int dm_scale_budget_step(dm_scale_budget *s, uint64_t sample,
    double age_ms, double gpu, double cpu, double budget, int held,
    int level, int max_level, double next_up_ratio) {
    if (!sample || !(age_ms >= 0 && age_ms <= 3000) ||
        !(gpu > 0 && gpu < 1000 && cpu > 0 && cpu < 1000 && budget > 0)) {
        s->good = 0; return DM_S3B_INVALID;
    }
    if (sample == s->sample) return DM_S3B_DUPLICATE;
    s->sample = sample;
    if (held) { s->good = 0; return DM_S3B_HOLD; }
    if (gpu > budget * 1.05) {
        s->good = 0;
        if (gpu <= cpu * 1.10) return DM_S3B_CPU;
        return level < max_level ? DM_S3B_DOWN : DM_S3B_FLOOR;
    }
    // Predict pixel cost at the next higher scale to avoid oscillating at paced 30fps.
    if (level > 0 && next_up_ratio > 1 &&
        gpu * next_up_ratio * next_up_ratio < budget * 0.90) {
        if (++s->good >= 5) { s->good = 0; return DM_S3B_UP; }
        return DM_S3B_WAIT_UP;
    }
    s->good = 0; return DM_S3B_STABLE;
}
#endif
