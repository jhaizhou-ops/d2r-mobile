#include "../dm_scale_budget.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
// Real production helper, explicit behavioral expectations (no copied algorithm).
int test_scale_budget(void) {
    dm_scale_budget s = {0}; int d;
    CHECK(dm_scale_budget_step(&s,1,0,30,19,1000.0/60,0,0,3,1)==DM_S3B_DOWN);
    CHECK(dm_scale_budget_step(&s,2,0,30,19,1000.0/30,0,0,3,1)==DM_S3B_STABLE);
    CHECK(dm_scale_budget_step(&s,3,0,20,19,1000.0/60,0,1,3,1.1)==DM_S3B_CPU);
    CHECK(dm_scale_budget_step(&s,4,0,30,19,1000.0/60,0,3,3,1.1)==DM_S3B_FLOOR);
    CHECK(dm_scale_budget_step(&s,5,0,30,19,1000.0/60,1,0,3,1)==DM_S3B_HOLD);
    CHECK(dm_scale_budget_step(&s,6,3001,30,19,1000.0/60,0,0,3,1)==DM_S3B_INVALID);
    CHECK(dm_scale_budget_step(&s,7,0,-1,19,1000.0/60,0,0,3,1)==DM_S3B_INVALID);
    CHECK(dm_scale_budget_step(&s,8,0,30,-1,1000.0/60,0,0,3,1)==DM_S3B_INVALID);
    // 20ms GPU at paced30 must NOT recover scale when the requested target is60.
    CHECK(dm_scale_budget_step(&s,9,0,20,10,1000.0/60,0,1,3,1.1)==DM_S3B_DOWN);
    CHECK(dm_scale_budget_step(&s,10,0,14,10,1000.0/60,0,1,3,1.1)==DM_S3B_STABLE);
    s.good=0;
    for (uint64_t i=11;i<15;i++) CHECK(dm_scale_budget_step(&s,i,0,10,10,1000.0/60,0,1,3,1.1)==DM_S3B_WAIT_UP);
    CHECK(s.good==4);
    CHECK(dm_scale_budget_step(&s,14,0,10,10,1000.0/60,0,1,3,1.1)==DM_S3B_DUPLICATE);
    CHECK(s.good==4);
    CHECK(dm_scale_budget_step(&s,15,0,10,10,1000.0/60,0,1,3,1.1)==DM_S3B_UP);
    CHECK(s.good==0);
    CHECK(dm_scale_budget_step(&s,16,0,10,10,1000.0/60,0,1,3,1.1)==DM_S3B_WAIT_UP);
    CHECK(dm_scale_budget_step(&s,17,-1,10,10,1000.0/60,0,1,3,1.1)==DM_S3B_INVALID);
    CHECK(s.good==0);
    return 0;
}
