// 回收不能破坏最近使用或被 GPU 修改的贴图；关闭开关必须保留原行为。
// Eviction must protect recent/GPU-written textures; disabling it must preserve baseline behavior.
#include <assert.h>
#include <stdio.h>
#include "../../gfx/nvn/nvn_pkg_cache.h"
#include "../../gfx/nvn/nvn_astc_copy.h"
#define MB(x) ((uint64_t)(x) << 20)
int main(void) {
    assert(!nvn_pkg_cache_pressure(0, MB(160)));
    assert(nvn_pkg_cache_pressure(0, MB(159)));
    assert(nvn_pkg_cache_pressure(1, MB(200))); // 恢复途中不反复开关 | No oscillation during recovery.
    assert(!nvn_pkg_cache_pressure(1, MB(224)));
    assert(nvn_pkg_cache_critical(MB(95)));
    assert(!nvn_pkg_cache_critical(MB(96)));
    assert(!nvn_pkg_cache_cold(599, 0, 0, 0));
    assert(nvn_pkg_cache_cold(600, 0, 0, 0));
    assert(!nvn_pkg_cache_cold(119, 0, 0, 1));
    assert(nvn_pkg_cache_cold(120, 0, 0, 1));
    assert(!nvn_pkg_cache_cold(1000, 999, 0, 1));
    assert(!nvn_pkg_cache_cold(1000, 0, 1, 0));
    assert(!nvn_pkg_cache_cold(1000, 0, 1, 1));
    assert(!nvn_pkg_cache_cold(0, UINT64_MAX, 0, 1));
    assert(nvn_pkg_cache_target(MB(900), 0, 0, 1) == 0);
    assert(nvn_pkg_cache_target(MB(384), MB(256), MB(384), 0) == 0);
    assert(nvn_pkg_cache_target(MB(400), MB(500), MB(384), 0) == MB(16));
    assert(nvn_pkg_cache_target(MB(900), MB(500), MB(384), 0) == MB(32));
    assert(nvn_pkg_cache_target(MB(200), MB(150), MB(384), 1) == MB(16));
    assert(nvn_pkg_cache_target(MB(200), MB(200), MB(384), 1) == MB(16));
    assert(nvn_pkg_cache_target(MB(200), MB(200), MB(384), 0) == 0);
    assert(nvn_pkg_cache_target(MB(400), MB(1), MB(384), 1) == MB(64));
    for (unsigned total = 0; total < 4096; total += 17)
        for (unsigned available = 0; available < 1024; available += 13) {
            uint64_t target = nvn_pkg_cache_target(MB(total), MB(available), MB(384), 1);
            assert(target <= MB(available < 96 ? 64 : 32));
        }
    assert(nvn_astc_copy_axis(0, 0, 256, 256, 256));
    assert(nvn_astc_copy_axis(0, 0, 2, 2, 2)); // 小 mip 仍可直接拷贝 | Small mips remain valid.
    assert(nvn_astc_copy_axis(4, 8, 3, 7, 11));
    assert(!nvn_astc_copy_axis(4, 8, 3, 7, 12)); // 目标尾块未到边缘 | Destination tail is not at its edge.
    assert(!nvn_astc_copy_axis(1, 0, 4, 8, 8));
    assert(!nvn_astc_copy_axis(0, 2, 4, 8, 8));
    assert(!nvn_astc_copy_axis(-4, 0, 4, 8, 8));
    assert(!nvn_astc_copy_axis(0, 0, 0, 8, 8));
    assert(!nvn_astc_copy_axis(0, 0, 9, 8, 8));
    assert(!nvn_astc_copy_axis(0x7ffffffc, 0, 8, 0x7fffffff, 8));
    puts("pkg_cache: hysteresis, hot/GPU protection, batch limits and ASTC block bounds passed");
}
