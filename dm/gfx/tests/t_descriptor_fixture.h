#include <assert.h>
// 合成测试显式绑定一个池，代替以前隐式共用的全局登记表。
// Synthetic fixtures explicitly bind a pool instead of using the former implicit global registry.
static dpool_t fixture_pool = {.n = 65536};
static void fixture_reg_tex(int id, const tex_t *tex, const view_t *view) {
    if (!fixture_pool.host) assert(mtl_dpool_init(&fixture_pool, 0));
    mtl_reg_tex(&fixture_pool, id, tex, view);
}
