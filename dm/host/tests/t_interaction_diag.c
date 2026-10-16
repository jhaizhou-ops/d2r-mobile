// 物件交互追踪回归：包含生产实现，以自造单位测试采样时间、同图/跨图、覆盖及离局；不调用游戏入口。
// Object-interaction trace regression: includes production code with authored units to test checkpoints, same/different levels, supersession and exit; calls no game entry.
// Object-interaction trace regression: includes production code with authored units to test checkpoints, same/different levels, supersession and exit; calls no game entry.
#include "../dm_world.c"
void dm_hud_get(dm_hud_info *h) { memset(h, 0, sizeof *h); h->valid = 1; h->area = 1; }
static int mouse_world_active = 1;
int dm_touch_host_world_active(void) { return mouse_world_active; }
static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
int main(void) {
    _Alignas(8) unsigned char pl[512] = {0}, target[512] = {0}, path[128] = {0}, op[64] = {0}, room[64] = {0}, r2[160] = {0}, level[512] = {0};
    uint64_t p = (uint64_t)(uintptr_t)pl, u = (uint64_t)(uintptr_t)target;
    *(uint32_t *)(pl + 8) = 10; *(uint64_t *)(pl + 0x38) = (uint64_t)(uintptr_t)path;
    *(uint64_t *)(path + 0x20) = (uint64_t)(uintptr_t)room; *(uint64_t *)(room + 0x18) = (uint64_t)(uintptr_t)r2; *(uint64_t *)(r2 + 0x90) = (uint64_t)(uintptr_t)level;
    *(int *)(level + 0x1f8) = 40; *(uint32_t *)target = 2; *(uint32_t *)(target + 4) = 59; *(uint32_t *)(target + 8) = 20; *(uint64_t *)(target + 0x38) = (uint64_t)(uintptr_t)op;
    CHECK(w_interaction_begin(p, u, 100) == 1); CHECK(g_it.active && g_it.last_lv == 40);
    w_interaction_poll(p, 102); CHECK(g_it.stage == 0);
    w_interaction_poll(p, 103); CHECK(g_it.stage == 1 && g_it.active);
    w_interaction_poll(p, 129); CHECK(g_it.stage == 1);
    w_interaction_poll(p, 130); CHECK(g_it.stage == 2);
    *(int *)(level + 0x1f8) = 47; w_interaction_poll(p, 131); CHECK(g_it.last_lv == 47 && g_it.active);
    w_interaction_poll(p, 220); CHECK(!g_it.active && g_it.stage == 3);
    CHECK(w_interaction_begin(p, u, 230) == 2); CHECK(w_interaction_begin(p, u, 231) == 3);
    w_interaction_poll(0, 232); CHECK(!g_it.active);
    w_interaction_begin(p, u, 240); *(uint32_t *)(pl + 8) = 11; w_interaction_poll(p, 241); CHECK(!g_it.active);
    w_interaction_begin(p, u, 250); w_interaction_poll(p, 1); CHECK(!g_it.active);
    *(uint32_t *)target = 4; CHECK(w_interaction_begin(p, u, 260) == 0 && !g_it.active);
    CHECK(*(int *)(level + 0x1f8) == 47 && *(uint32_t *)(target + 8) == 20);
#if TARGET_OS_OSX
    // 用生产队列验证取消：下一轮世界泵先于鼠标 hook，不能只清鼠标本地状态。
    // Verify cancellation on production queues: the next world pump precedes the mouse hook, so clearing only mouse-local state is insufficient.
    dm_world_click(.7f, .4f); dm_world_walk(.8f, .5f); w_mouse_cancel_pending();
    CHECK(atomic_load(&g_tap) != 0 && atomic_load(&g_walk) != 0);
    mouse_world_active = 0; w_mouse_cancel_pending();
    CHECK(atomic_load(&g_tap) == 0 && atomic_load(&g_walk) == 0 && atomic_load(&g_tap_mouse) == 0);
    unsigned seq = dm_world_tap(.2f, .3f); w_mouse_cancel_pending();
    CHECK((unsigned)(atomic_load(&g_tap) >> 32) == seq);
    atomic_store(&g_tap, 0); mouse_world_active = 1;
#endif
    printf("t_interaction_diag: %d checks, %d failures\n", checks, fails); return fails != 0;
}
