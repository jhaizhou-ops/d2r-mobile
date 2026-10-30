// 重置前的点数容量与确认流程：不允许溢出或跨角色确认。
// Refund capacity and confirmation flow: reject overflow and cross-character confirmation.
#include "../dm_skill_reset.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    int levels[] = {20, 40, 0, 3};
    assert(dm_skill_reset_total(12, levels, 4) == 75);
    assert(dm_skill_reset_total(192, levels, 4) == 255);
    assert(dm_skill_reset_total(193, levels, 4) == -1);
    assert(dm_skill_reset_total(-1, levels, 4) == -1);
    levels[0] = -1;
    assert(dm_skill_reset_total(0, levels, 4) == -1);
    levels[0] = 0; levels[1] = 0; levels[3] = 0;
    assert(dm_skill_reset_total(75, levels, 4) == 75);
    dm_skill_reset_flow f = {0};
    assert(!dm_skill_reset_event(&f, 2, 100, 1, 1));
    assert(!dm_skill_reset_event(&f, 1, 100, 1, 1));
    assert(!dm_skill_reset_event(&f, 2, 101, 1, 1));
    assert(!dm_skill_reset_event(&f, 1, 100, 1, 1));
    assert(!dm_skill_reset_event(&f, 3, 100, 1, 1));
    assert(!dm_skill_reset_event(&f, 2, 100, 1, 1));
    for (int i = 0; i < 3; i++) {
        assert(!dm_skill_reset_event(&f, 1, 100, 1, 1));
        assert(dm_skill_reset_event(&f, 2, 100, 1, 1));
        assert(!dm_skill_reset_event(&f, 2, 100, 1, 1));
    }
    dm_skill_reset_event(&f, 1, 100, 1, 1);
    assert(!dm_skill_reset_event(&f, 2, 100, 0, 1));
    assert(!dm_skill_reset_event(&f, 2, 100, 1, 1));
    dm_skill_reset_event(&f, 1, 100, 1, 1);
    assert(!dm_skill_reset_event(&f, 2, 100, 1, 0));
    puts("skill reset OK");
}
