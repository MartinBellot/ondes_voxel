#include "ov/world/chunk.hpp"

namespace ov::world {
namespace {

/// Position of a stored heightmap in the chunk's array.
///
/// Only the four persistent kinds have a slot; the `_WG` pair belongs to the
/// generator. An out-of-range type maps to WORLD_SURFACE rather than reading
/// past the array — a caller asking for a generation-time map from a stored
/// chunk is a bug, and one that should not also be memory-unsafe.
[[nodiscard]] usize slot_for(HeightmapType type) noexcept {
    switch (type) {
        case HeightmapType::WorldSurface: return 0;
        case HeightmapType::MotionBlocking: return 1;
        case HeightmapType::MotionBlockingNoLeaves: return 2;
        case HeightmapType::OceanFloor: return 3;
        case HeightmapType::WorldSurfaceWG:
        case HeightmapType::OceanFloorWG: break;
    }
    return 0;
}

}  // namespace

Chunk::Chunk(ChunkPos position, WorldShape shape, AirStates air)
    : position_{position},
      shape_{shape},
      air_{air},
      heightmaps_{Heightmap{shape.min_y, shape.height}, Heightmap{shape.min_y, shape.height},
                  Heightmap{shape.min_y, shape.height}, Heightmap{shape.min_y, shape.height}} {
    sections_.reserve(shape.section_count());
    for (usize i = 0; i < shape.section_count(); ++i) {
        sections_.emplace_back(air);
    }
}

usize Chunk::section_index_for_y(i32 y) const noexcept {
    // Arithmetic shift, not division: y is negative for most of the Overworld,
    // and truncation towards zero would put y = -1 and y = 0 in the same
    // section while y = -16 and y = -17 landed apart.
    return static_cast<usize>((y >> 4) - shape_.min_section());
}

ChunkSection* Chunk::section_for_y(i32 y) noexcept {
    if (!shape_.contains_y(y)) {
        return nullptr;
    }
    return &sections_[section_index_for_y(y)];
}

const ChunkSection* Chunk::section_for_y(i32 y) const noexcept {
    if (!shape_.contains_y(y)) {
        return nullptr;
    }
    return &sections_[section_index_for_y(y)];
}

registry::BlockStateId Chunk::get_block(usize x, i32 y, usize z) const noexcept {
    const ChunkSection* section = section_for_y(y);
    if (section == nullptr || x >= kSectionSize || z >= kSectionSize) {
        return air_.air;
    }
    return section->get_block(x, static_cast<usize>(y & 15), z);
}

i32 Chunk::scan_surface_down(usize x, usize z, i32 from_y) const noexcept {
    for (i32 y = from_y; y >= shape_.min_y; --y) {
        if (!air_.is_air(get_block(x, y, z))) {
            return y;
        }
    }
    return shape_.min_y - 1;  // nothing in the column
}

void Chunk::set_block(usize x, i32 y, usize z, registry::BlockStateId state) {
    ChunkSection* section = section_for_y(y);
    if (section == nullptr || x >= kSectionSize || z >= kSectionSize) {
        return;
    }

    const registry::BlockStateId previous = get_block(x, y, z);
    if (previous == state) {
        return;
    }
    section->set_block(x, static_cast<usize>(y & 15), z, state);

    // WORLD_SURFACE is the only heightmap maintainable today: it needs nothing
    // but air-ness. The other three need the block-state flag table, which the
    // official reports do not carry, so they are left untouched rather than
    // filled with a guess.
    Heightmap& surface = heightmaps_[slot_for(HeightmapType::WorldSurface)];

    if (!air_.is_air(state)) {
        surface.raise_to(x, z, y);
        return;
    }

    // The block became air. If it was not the surface, nothing moves.
    if (surface.first_free(x, z) != y + 1) {
        return;
    }
    const i32 found = scan_surface_down(x, z, y - 1);
    if (found < shape_.min_y) {
        surface.clear_column(x, z);
    } else {
        surface.set_surface(x, z, found);
    }
}

u16 Chunk::get_biome(usize x, i32 y, usize z) const noexcept {
    const ChunkSection* section = section_for_y(y);
    if (section == nullptr || x >= kSectionSize || z >= kSectionSize) {
        return 0;
    }
    return section->get_biome(x, static_cast<usize>(y & 15), z);
}

void Chunk::set_biome(usize x, i32 y, usize z, u16 biome) {
    ChunkSection* section = section_for_y(y);
    if (section == nullptr || x >= kSectionSize || z >= kSectionSize) {
        return;
    }
    section->set_biome(x, static_cast<usize>(y & 15), z, biome);
}

Heightmap& Chunk::heightmap(HeightmapType type) noexcept {
    return heightmaps_[slot_for(type)];
}

const Heightmap& Chunk::heightmap(HeightmapType type) const noexcept {
    return heightmaps_[slot_for(type)];
}

void Chunk::recompute_world_surface() noexcept {
    Heightmap& surface = heightmaps_[slot_for(HeightmapType::WorldSurface)];

    for (usize z = 0; z < kSectionSize; ++z) {
        for (usize x = 0; x < kSectionSize; ++x) {
            const i32 found = scan_surface_down(x, z, shape_.max_y());
            if (found < shape_.min_y) {
                surface.clear_column(x, z);
            } else {
                surface.set_surface(x, z, found);
            }
        }
    }
}

usize Chunk::non_air_count() const noexcept {
    usize total = 0;
    for (const ChunkSection& section : sections_) {
        total += section.non_air_count();
    }
    return total;
}

}  // namespace ov::world
