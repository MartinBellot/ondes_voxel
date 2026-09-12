#define OV_LOG_CATEGORY "worldgen"

#include "terrain_cache.hpp"

#include "ov/math/block_pos.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace ov::server {

namespace {

struct Item {
    world::Chunk                  chunk;
    std::vector<std::string_view> starts;
    u64                           used{0};
};

}  // namespace

struct SharedTerrainCache::Impl {
    usize capacity{0};

    mutable std::mutex              mutex;
    std::unordered_map<u64, Item>   items;
    u64                             clock{0};
    u64                             hits{0};
    u64                             misses{0};
    u64                             stored{0};
    u64                             evicted{0};
    std::vector<std::pair<u64, u64>> ages;  ///< eviction scratch: (used, key)

    /// Forget the least recently used eighth. Called with the lock held, when
    /// full: an eighth at a time so that a full cache does not pay a scan per
    /// offer.
    void evict() {
        ages.clear();
        ages.reserve(items.size());
        for (const auto& [key, item] : items) {
            ages.emplace_back(item.used, key);
        }
        const usize drop = std::max<usize>(1, items.size() / 8);
        std::ranges::nth_element(ages, ages.begin() + static_cast<std::ptrdiff_t>(drop - 1));
        for (usize i = 0; i < drop; ++i) {
            items.erase(ages[i].second);
        }
        evicted += drop;
    }
};

SharedTerrainCache::SharedTerrainCache(usize capacity) : impl_(std::make_unique<Impl>()) {
    impl_->capacity = capacity;
}

SharedTerrainCache::~SharedTerrainCache() = default;

bool SharedTerrainCache::fetch(i32 chunk_x, i32 chunk_z, world::Chunk& chunk,
                               std::vector<std::string_view>& starts) {
    const u64              key = ChunkPos{chunk_x, chunk_z}.packed();
    const std::scoped_lock lock{impl_->mutex};
    const auto             found = impl_->items.find(key);
    if (found == impl_->items.end()) {
        ++impl_->misses;
        return false;
    }
    // Copied under the lock: see the header.
    chunk              = found->second.chunk;
    starts             = found->second.starts;
    found->second.used = ++impl_->clock;
    ++impl_->hits;
    return true;
}

void SharedTerrainCache::offer(i32 chunk_x, i32 chunk_z, const world::Chunk& chunk,
                               const std::vector<std::string_view>& starts) {
    if (impl_->capacity == 0) {
        return;
    }
    const u64              key = ChunkPos{chunk_x, chunk_z}.packed();
    const std::scoped_lock lock{impl_->mutex};
    if (impl_->items.contains(key)) {
        return;  // another worker carved the same chunk at the same time: same chunk
    }
    if (impl_->items.size() >= impl_->capacity) {
        impl_->evict();
    }
    impl_->items.emplace(key, Item{chunk, starts, ++impl_->clock});
    ++impl_->stored;
}

usize SharedTerrainCache::capacity() const noexcept { return impl_->capacity; }

std::string SharedTerrainCache::report() const {
    const std::scoped_lock lock{impl_->mutex};
    const u64              asked = impl_->hits + impl_->misses;
    return fmt::format(
        "terrain cache: {} hits, {} misses ({:.1f}% of carved terrain copied rather than "
        "generated), {} stored, {} evicted, {} of {} held",
        impl_->hits, impl_->misses,
        asked == 0 ? 0.0 : 100.0 * static_cast<double>(impl_->hits) / static_cast<double>(asked),
        impl_->stored, impl_->evicted, impl_->items.size(), impl_->capacity);
}

}  // namespace ov::server
