#include "../vulkan/engine/xbg_whole_set.h"
#include <cassert>
#include <cstdio>

int main() {
    XbgWholeSetStamp saved;
    uint64_t ubo = 7, image = 8;
    std::vector<XbgWholeSetRevision> refs{{&ubo, ubo}, {&image, image}};
    auto valid = [&](const XbgWholeSetStamp &next) {
        return xbg_whole_set_valid(saved, next, refs);
    };
    assert(valid(saved));
    auto changed = saved;
    ++changed.textures[3]; assert(!valid(changed));
    changed = saved; ++changed.buffers[1]; assert(!valid(changed)); // offset-only BindUBO increments buf_gen
    ++ubo; assert(!valid(saved)); --ubo;
    ++image; assert(!valid(saved)); --image;
    changed = saved; changed.shaders[1] = &ubo; assert(!valid(changed)); // shader interface/layout
    changed = saved; ++changed.frame; assert(!valid(changed));
    changed = saved; ++changed.submit; assert(!valid(changed));
    changed = saved; ++changed.arena; assert(!valid(changed));
    changed = saved; ++changed.writes; assert(!valid(changed));
    changed = saved; ++changed.pools[1]; assert(!valid(changed));
    changed = saved; ++changed.samplers; assert(!valid(changed));
    changed = saved; ++changed.registrations; assert(!valid(changed));
    changed = saved; ++changed.images; assert(!valid(changed));
    changed = saved; ++changed.width; assert(!valid(changed));
    changed = saved; ++changed.alpha; assert(!valid(changed));
    refs.push_back({nullptr, 0}); assert(!valid(saved));
    // Lifetime mismatch must avoid even reading a stale revision pointer.
    refs = {{reinterpret_cast<const uint64_t *>(1), 0}};
    changed = saved; ++changed.arena; assert(!valid(changed));
    puts("PASS whole-set binding, offset, UBO/image revision, layout, frame, submit, arena, write, pool and pointer lifetime");
}
