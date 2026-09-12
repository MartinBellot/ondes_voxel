#include "ov/world/chunk.hpp"

#include <array>

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

Chunk::Chunk(ChunkPos position, WorldShape shape, AirStates air,
             const registry::BlockRegistry* blocks)
    : position_{position},
      shape_{shape},
      air_{air},
      blocks_{blocks},
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

ChunkSection* Chunk::section_for_y(i32 y) {
    if (!shape_.contains_y(y)) {
        return nullptr;
    }
    // Unshare now rather than on the first write: the relight pass takes
    // `blocks()` by reference and then writes the light, and if the copy
    // happened on that write the reference would point into storage only a
    // snapshot owns — freed by the network thread whenever it is done.
    ChunkSection& section = sections_[section_index_for_y(y)];
    section.unshare();
    return &section;
}

std::shared_ptr<const Chunk> Chunk::snapshot() const {
    // Copying the sections marks them shared on both sides (ChunkSection's
    // copy constructor); nothing else is needed to keep the tick off them.
    return std::make_shared<const Chunk>(*this);
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

    // A block entity outliving its block is a chest that cannot be opened and
    // cannot be removed. Any change to the block drops it; whoever placed the
    // new block adds one back if it needs one.
    remove_block_entity(x, y, z);

    update_heightmap(HeightmapType::WorldSurface, x, y, z, state);
    if (blocks_ != nullptr) {
        update_heightmap(HeightmapType::MotionBlocking, x, y, z, state);
        update_heightmap(HeightmapType::MotionBlockingNoLeaves, x, y, z, state);
        update_heightmap(HeightmapType::OceanFloor, x, y, z, state);
    }
}

bool Chunk::counts_for(HeightmapType type, registry::BlockStateId state) const noexcept {
    if (type == HeightmapType::WorldSurface) {
        return !air_.is_air(state);
    }
    if (blocks_ == nullptr) {
        return false;
    }
    const registry::BlockId block = blocks_->block_of(state);
    const bool              solid = blocks_->blocks_motion(block);

    switch (type) {
        case HeightmapType::OceanFloor: return solid;
        case HeightmapType::MotionBlocking: return solid || blocks_->holds_fluid(state);
        case HeightmapType::MotionBlockingNoLeaves:
            return (solid || blocks_->holds_fluid(state)) && !blocks_->is_leaves(block);
        default: return false;
    }
}

i32 Chunk::scan_down(HeightmapType type, usize x, usize z, i32 from_y) const noexcept {
    for (i32 y = from_y; y >= shape_.min_y; --y) {
        if (counts_for(type, get_block(x, y, z))) {
            return y;
        }
    }
    return shape_.min_y - 1;  // nothing in the column counts
}

void Chunk::update_heightmap(HeightmapType type, usize x, i32 y, usize z,
                             registry::BlockStateId state) noexcept {
    Heightmap& map = heightmaps_[slot_for(type)];

    if (counts_for(type, state)) {
        map.raise_to(x, z, y);
        return;
    }

    // The block stopped counting. If it was not the top, nothing moves — which
    // is the common case, and the reason breaking is the expensive direction.
    if (map.first_free(x, z) != y + 1) {
        return;
    }
    const i32 found = scan_down(type, x, z, y - 1);
    if (found < shape_.min_y) {
        map.clear_column(x, z);
    } else {
        map.set_surface(x, z, found);
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

void Chunk::fill_biome(u16 biome) {
    for (ChunkSection& section : sections_) {
        section.fill_biome(biome);
    }
}

Heightmap& Chunk::heightmap(HeightmapType type) noexcept {
    return heightmaps_[slot_for(type)];
}

const Heightmap& Chunk::heightmap(HeightmapType type) const noexcept {
    return heightmaps_[slot_for(type)];
}

void Chunk::recompute_heightmaps() noexcept {
    constexpr std::array kTypes = {HeightmapType::WorldSurface, HeightmapType::MotionBlocking,
                                   HeightmapType::MotionBlockingNoLeaves,
                                   HeightmapType::OceanFloor};

    for (const HeightmapType type : kTypes) {
        if (type != HeightmapType::WorldSurface && blocks_ == nullptr) {
            continue;
        }
        Heightmap& map = heightmaps_[slot_for(type)];
        for (usize z = 0; z < kSectionSize; ++z) {
            for (usize x = 0; x < kSectionSize; ++x) {
                const i32 found = scan_down(type, x, z, shape_.max_y());
                if (found < shape_.min_y) {
                    map.clear_column(x, z);
                } else {
                    map.set_surface(x, z, found);
                }
            }
        }
    }
}

BlockEntity* Chunk::block_entity_at(usize x, i32 y, usize z) noexcept {
    for (BlockEntity& entity : block_entities_) {
        if (entity.x == x && entity.y == y && entity.z == z) {
            return &entity;
        }
    }
    return nullptr;
}

const BlockEntity* Chunk::block_entity_at(usize x, i32 y, usize z) const noexcept {
    for (const BlockEntity& entity : block_entities_) {
        if (entity.x == x && entity.y == y && entity.z == z) {
            return &entity;
        }
    }
    return nullptr;
}

void Chunk::set_block_entity(BlockEntity entity) {
    if (BlockEntity* existing = block_entity_at(entity.x, entity.y, entity.z)) {
        *existing = std::move(entity);
        return;
    }
    block_entities_.push_back(std::move(entity));
}

void Chunk::remove_block_entity(usize x, i32 y, usize z) {
    std::erase_if(block_entities_, [&](const BlockEntity& entity) {
        return entity.x == x && entity.y == y && entity.z == z;
    });
}

usize Chunk::non_air_count() const noexcept {
    usize total = 0;
    for (const ChunkSection& section : sections_) {
        total += section.non_air_count();
    }
    return total;
}

}  // namespace ov::world
