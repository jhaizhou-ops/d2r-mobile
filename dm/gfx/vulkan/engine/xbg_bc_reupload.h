#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace h12bc {

// Recording is serialized by the engine mutex. Once soft-BC activity stops,
// the next upload discards the burst working set (including scratch buffers).
constexpr uint64_t idle_ns = 10'000'000'000ULL;
constexpr size_t cache_budget = 64ULL << 20;
inline bool burst_active(uint64_t now, uint64_t last) {
    return last && now - last < idle_ns;
}

inline bool can_skip(bool enabled, bool uploaded_hash_valid, bool same_layout,
                     uint64_t previous, uint64_t current) {
    return enabled && uploaded_hash_valid && same_layout && previous && previous == current;
}

// Grow-only scratch within a burst. The baseline still uses its own zero-filled
// vectors. Only initialized texel bytes are ever referenced by Vulkan copies.
class Scratch {
    std::unique_ptr<uint8_t[]> bytes_;
    size_t used_ = 0, capacity_ = 0;
public:
    void reset() { used_ = 0; }
    void resize(size_t size) {
        if (size > capacity_) {
            auto replacement = std::unique_ptr<uint8_t[]>(new uint8_t[size]);
            if (used_) std::memcpy(replacement.get(), bytes_.get(), used_);
            bytes_ = std::move(replacement);
            capacity_ = size;
        }
        used_ = size;
    }
    size_t size() const { return used_; }
    uint8_t *data() { return bytes_.get(); }
};

class UploadBuffer {
    Scratch *scratch_;
    std::vector<uint8_t> local_;
public:
    explicit UploadBuffer(Scratch *scratch):scratch_(scratch) { if(scratch_)scratch_->reset(); }
    void resize(size_t n) { if(scratch_)scratch_->resize(n);else local_.resize(n); }
    size_t size() const { return scratch_?scratch_->size():local_.size(); }
    uint8_t *data() { return scratch_?scratch_->data():local_.data(); }
};

// Decode a 2D BC subresource. The preceding compressed and decoded bytes
// belong to exactly this image layout, layer and mip.
template<class DecodeBlock>
bool patch_blocks(uint32_t width, uint32_t height, size_t bpp, size_t block_bytes,
                  const uint8_t *previous, const uint8_t *next,
                  uint8_t *output, DecodeBlock decode) {
    const size_t columns = (size_t(width) + 3) / 4, rows = (size_t(height) + 3) / 4;
    uint8_t pixels[128];
    for (size_t y = 0; y < rows; ++y) for (size_t x = 0; x < columns; ++x) {
        const size_t offset = (y * columns + x) * block_bytes;
        if (!std::memcmp(previous + offset, next + offset, block_bytes)) continue;
        if (!decode(next + offset, pixels, sizeof pixels)) return false;
        const size_t copy_width = std::min<size_t>(4, width - 4*x);
        for (size_t line = 0; line < 4 && 4*y + line < height; ++line) {
            std::memcpy(output + ((4*y + line)*width + 4*x)*bpp,
                        pixels + line*4*bpp, copy_width*bpp);
        }
    }
    return true;
}

struct Surface {
    std::vector<uint8_t> source, decoded;
};
struct ImageHistory {
    uint64_t used = 0;
    uint32_t format, width, height, levels, layers, bpp;
    std::vector<Surface> surfaces;
    size_t payload = 0;
};
class Histories {
    std::map<uint64_t, unsigned> seen_;
    std::map<uint64_t, ImageHistory> images_;
    uint64_t stamp_ = 0;
    size_t payload_ = 0;
public:
    ImageHistory *acquire(uint64_t serial, bool reupload, size_t guest_size,
                          uint32_t format, uint32_t width, uint32_t height,
                          uint32_t layers, uint32_t levels, uint32_t bpp, size_t block_size) {
        if (!reupload || guest_size < (16U << 10) || guest_size > (2U << 20))
            return nullptr;
        if (seen_.size() >= 4096 && !seen_.count(serial)) seen_.clear();
        auto &seen = seen_[serial];
        if (seen < 3 && ++seen < 3) return nullptr;
        auto old = images_.find(serial);
        if (old != images_.end()) {
            auto &img = old->second;
            if (img.format == format && img.width == width && img.height == height &&
                img.layers == layers && img.levels == levels && img.bpp == bpp) {
                img.used = ++stamp_;
                return &img;
            }
            payload_ -= img.payload;
            images_.erase(old);
        }
        // Compute the exact reservation before allocation, including all mips.
        // Inputs here have passed Vulkan image creation; guard bounds as well.
        if (!width || !height || !layers || !levels || levels > 16 ||
            (bpp != 2 && bpp != 4 && bpp != 8) || (block_size != 8 && block_size != 16))
            return nullptr;
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < levels; ++i) {
            const uint64_t w = std::max(1U, width >> i), h = std::max(1U, height >> i);
            if (w > cache_budget / bpp / h) return nullptr;
            const uint64_t per_layer = w*h*bpp + ((w+3)/4)*((h+3)/4)*block_size;
            if (per_layer > (cache_budget-bytes)/layers) return nullptr;
            bytes += per_layer*layers;
        }
        while (!images_.empty() && (images_.size() >= 16 || bytes > cache_budget-payload_)) {
            auto lru = std::min_element(images_.begin(), images_.end(),
                [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
            payload_ -= lru->second.payload;
            images_.erase(lru);
        }
        auto &img = images_[serial];
        img = {++stamp_, format, width, height, levels, layers, bpp, {}, size_t(bytes)};
        img.surfaces.resize(size_t(layers)*levels);
        payload_ += img.payload;
        return &img;
    }
};
}
