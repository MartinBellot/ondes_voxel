#include "ov/world/heightmap.hpp"

#include <algorithm>
#include <array>

namespace ov::world {
namespace {

struct Named {
    HeightmapType    type;
    std::string_view name;
};

/// The names the file format and the chunk packet use, verbatim. A typo here
/// costs a heightmap silently: the client would receive a compound with a key
/// it does not know and render nothing.
constexpr std::array<Named, 6> kNames{{
    {HeightmapType::WorldSurface, "WORLD_SURFACE"},
    {HeightmapType::MotionBlocking, "MOTION_BLOCKING"},
    {HeightmapType::MotionBlockingNoLeaves, "MOTION_BLOCKING_NO_LEAVES"},
    {HeightmapType::OceanFloor, "OCEAN_FLOOR"},
    {HeightmapType::WorldSurfaceWG, "WORLD_SURFACE_WG"},
    {HeightmapType::OceanFloorWG, "OCEAN_FLOOR_WG"},
}};

[[nodiscard]] constexpr usize column_index(usize x, usize z) noexcept {
    return z * 16 + x;
}

}  // namespace

std::string_view to_string(HeightmapType type) noexcept {
    for (const Named& entry : kNames) {
        if (entry.type == type) {
            return entry.name;
        }
    }
    return {};
}

std::optional<HeightmapType> heightmap_type_from(std::string_view name) noexcept {
    for (const Named& entry : kNames) {
        if (entry.name == name) {
            return entry.type;
        }
    }
    return std::nullopt;
}

bool is_sent_to_client(HeightmapType type) noexcept {
    return type == HeightmapType::WorldSurface || type == HeightmapType::MotionBlocking;
}

Heightmap::Heightmap(i32 min_y, u32 height)
    : min_y_{min_y}, height_{height}, bits_{bits_for_height(height)} {
    data_.assign(packed_length(kColumnCount, bits_), 0);
}

u16 Heightmap::raw(usize index) const noexcept {
    // The same packing as a block palette: entries never span two longs, so the
    // division is by entries-per-long rather than by a bit count.
    const u32   per  = entries_per_long(bits_);
    const usize word = index / per;
    if (word >= data_.size()) {
        return 0;
    }
    const u32 shift = static_cast<u32>(index % per) * bits_;
    return static_cast<u16>((data_[word] >> shift) & ((u64{1} << bits_) - 1));
}

void Heightmap::set_raw(usize index, u16 value) noexcept {
    const u32   per  = entries_per_long(bits_);
    const usize word = index / per;
    if (word >= data_.size()) {
        return;
    }
    const u32 shift = static_cast<u32>(index % per) * bits_;
    const u64 mask  = ((u64{1} << bits_) - 1) << shift;
    data_[word]     = (data_[word] & ~mask) | ((static_cast<u64>(value) << shift) & mask);
}

i32 Heightmap::first_free(usize x, usize z) const noexcept {
    if (x >= 16 || z >= 16) {
        return min_y_;
    }
    return min_y_ + static_cast<i32>(raw(column_index(x, z)));
}

void Heightmap::set_surface(usize x, usize z, i32 block_y) noexcept {
    if (x >= 16 || z >= 16) {
        return;
    }
    // Stored is the free space above the block, so one more than the offset of
    // the block itself.
    const i64 offset  = static_cast<i64>(block_y) - min_y_ + 1;
    const i64 clamped = std::clamp<i64>(offset, 0, static_cast<i64>(height_));
    set_raw(column_index(x, z), static_cast<u16>(clamped));
}

void Heightmap::clear_column(usize x, usize z) noexcept {
    if (x >= 16 || z >= 16) {
        return;
    }
    set_raw(column_index(x, z), 0);
}

void Heightmap::raise_to(usize x, usize z, i32 block_y) noexcept {
    if (x >= 16 || z >= 16) {
        return;
    }
    if (block_y + 1 > first_free(x, z)) {
        set_surface(x, z, block_y);
    }
}

bool Heightmap::load(std::span<const u64> longs) {
    if (longs.size() != packed_length(kColumnCount, bits_)) {
        return false;
    }
    data_.assign(longs.begin(), longs.end());
    return true;
}

}  // namespace ov::world
