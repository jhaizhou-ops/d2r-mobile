#pragma once
#include <array>
#include <cstdint>
#include <vector>

// A set belongs to one shader interface, command slot and snapshot lifetime.
// Compare lifetime guards before following any image/registration pointers.
struct XbgWholeSetStamp {
    std::array<uint64_t, 6> buffers{}, textures{};
    std::array<const void*, 5> shaders{};
    std::array<uint64_t, 6> pools{};
    uint64_t frame{}, submit{}, arena{}, writes{}, samplers{}, registrations{}, images{};
    float width{}, height{};
    uint32_t alpha{};
    bool operator==(const XbgWholeSetStamp &b) const {
        return frame == b.frame && submit == b.submit && arena == b.arena &&
               writes == b.writes && samplers == b.samplers &&
               registrations == b.registrations && images == b.images &&
               shaders == b.shaders && buffers == b.buffers && textures == b.textures &&
               pools == b.pools && width == b.width && height == b.height && alpha == b.alpha;
    }
};

struct XbgWholeSetRevision {
    const uint64_t *live;
    uint64_t saved;
};

inline bool xbg_whole_set_valid(const XbgWholeSetStamp &saved, const XbgWholeSetStamp &now,
                                const std::vector<XbgWholeSetRevision> &refs) {
    if (!(saved == now)) return false;
    for (const auto &r : refs) {
        if (!r.live || *r.live != r.saved) return false;
    }
    return true;
}
