#include "relight.hpp"

#include "ov/world/chunk_section.hpp"
#include "ov/world/heightmap.hpp"
#include "ov/world/light_array.hpp"
#include "ov/world/paletted_container.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace ov::server {

bool stops_sky_light(const registry::BlockRegistry* blocks, registry::BlockStateId state) {
    if (blocks == nullptr) {
        return false;
    }
    return blocks->blocks_sky_light(blocks->block_of(state));
}

i32 sky_floor(const registry::BlockRegistry* blocks, const world::Chunk& chunk, usize x, usize z,
              i32 top) {
    for (i32 y = top; y >= chunk.shape().min_y; --y) {
        if (stops_sky_light(blocks, chunk.get_block(x, y, z))) {
            return y + 1;
        }
    }
    return chunk.shape().min_y;
}

void relight_blocks(world::Chunk& chunk, const registry::BlockRegistry& blocks) {
    const auto shape = chunk.shape();

    struct Cell {
        u8  x;
        i32 y;
        u8  z;
    };

    const auto light_at = [&](usize x, i32 y, usize z) -> u8 {
        const world::ChunkSection* section = chunk.section_for_y(y);
        return section == nullptr ? 0
                                  : section->block_light().get(
                                        world::section_index(x, static_cast<usize>(y & 15), z));
    };
    const auto set_light = [&](usize x, i32 y, usize z, u8 value) {
        world::ChunkSection* section = chunk.section_for_y(y);
        if (section != nullptr) {
            section->block_light().set(world::section_index(x, static_cast<usize>(y & 15), z),
                                       value);
        }
    };

    std::vector<Cell> frontier;

    // Clear first, then seed — a section at a time. Relighting without
    // clearing leaves the light of a torch that was broken.
    //
    // A section whose palette names no light-emitting state cannot seed
    // anything, and the palette says so in a few lookups instead of 4096 cell
    // reads. Only a direct (palette-less) section has to be read cell by cell.
    // A palette may still list a state no cell uses any more — it never
    // shrinks — which only ever makes the check scan a section it could have
    // skipped, never skip one it had to scan.
    for (usize i = 0; i < shape.section_count(); ++i) {
        const i32            bottom  = shape.min_y + static_cast<i32>(i) * 16;
        world::ChunkSection* section = chunk.section_for_y(bottom);
        if (section == nullptr) {
            continue;
        }
        section->block_light() = world::LightArray{0};

        const world::PalettedContainer& container = section->blocks();
        bool                            may_emit  = true;
        if (container.kind() != world::PaletteKind::Direct) {
            may_emit = std::ranges::any_of(container.palette(), [&](u16 value) {
                return blocks.light_emission(registry::BlockStateId{value}) > 0;
            });
        }
        if (!may_emit) {
            continue;
        }
        for (usize local_y = 0; local_y < world::kSectionSize; ++local_y) {
            for (usize z = 0; z < 16; ++z) {
                for (usize x = 0; x < 16; ++x) {
                    const u8 emission = blocks.light_emission(section->get_block(x, local_y, z));
                    if (emission > 0) {
                        const i32 y = bottom + static_cast<i32>(local_y);
                        section->block_light().set(world::section_index(x, local_y, z), emission);
                        frontier.push_back(Cell{static_cast<u8>(x), y, static_cast<u8>(z)});
                    }
                }
            }
        }
    }

    for (usize head = 0; head < frontier.size(); ++head) {
        const Cell cell    = frontier[head];
        const u8   current = light_at(cell.x, cell.y, cell.z);
        if (current <= 1) {
            continue;
        }
        const u8 spread = static_cast<u8>(current - 1);

        const std::array<Cell, 6> neighbours{{
            {static_cast<u8>(cell.x - 1), cell.y, cell.z},
            {static_cast<u8>(cell.x + 1), cell.y, cell.z},
            {cell.x, cell.y, static_cast<u8>(cell.z - 1)},
            {cell.x, cell.y, static_cast<u8>(cell.z + 1)},
            {cell.x, cell.y - 1, cell.z},
            {cell.x, cell.y + 1, cell.z},
        }};

        for (const Cell& next : neighbours) {
            if (next.x >= 16 || next.z >= 16 || next.y < shape.min_y || next.y > shape.max_y()) {
                continue;
            }
            if (stops_sky_light(&blocks, chunk.get_block(next.x, next.y, next.z))) {
                continue;
            }
            if (light_at(next.x, next.y, next.z) >= spread) {
                continue;
            }
            set_light(next.x, next.y, next.z, spread);
            frontier.push_back(next);
        }
    }

    for (usize i = 0; i < shape.section_count(); ++i) {
        world::ChunkSection* section = chunk.section_for_y(shape.min_y + static_cast<i32>(i) * 16);
        if (section != nullptr) {
            section->block_light().compact();
        }
    }
}

void relight_chunk(world::Chunk& chunk, const registry::BlockRegistry* blocks) {
    const auto  shape   = chunk.shape();
    const auto& surface = chunk.heightmap(world::HeightmapType::WorldSurface);

    // Only the occupied band needs work. Everything above the tallest column is
    // open sky and everything below the world is nothing.
    i32 highest = shape.min_y;
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            highest = std::max(highest, surface.first_free(x, z));
        }
    }
    const i32 top = std::min(highest + 1, shape.max_y());

    const auto light_at = [&](usize x, i32 y, usize z) -> u8 {
        const world::ChunkSection* section = chunk.section_for_y(y);
        return section == nullptr ? 0
                                  : section->sky_light().get(
                                        world::section_index(x, static_cast<usize>(y & 15), z));
    };
    const auto set_light = [&](usize x, i32 y, usize z, u8 value) {
        world::ChunkSection* section = chunk.section_for_y(y);
        if (section != nullptr) {
            section->sky_light().set(world::section_index(x, static_cast<usize>(y & 15), z), value);
        }
    };

    struct Cell {
        u8  x;
        i32 y;
        u8  z;
    };

    std::vector<Cell> frontier;

    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            const i32 first_free = sky_floor(blocks, chunk, x, z, top);
            for (usize i = 0; i < shape.section_count(); ++i) {
                const i32            bottom  = shape.min_y + static_cast<i32>(i) * 16;
                world::ChunkSection* section = chunk.section_for_y(bottom);
                if (section == nullptr) {
                    continue;
                }
                for (usize local_y = 0; local_y < 16; ++local_y) {
                    const i32 y = bottom + static_cast<i32>(local_y);
                    section->sky_light().set(world::section_index(x, local_y, z),
                                             y >= first_free ? world::kMaxLightLevel : 0);
                }
            }
            // Every directly lit cell in the band is a source, not just the
            // lowest one of each column: the boundary between two columns of
            // different heights is exactly where a build casts its shadow.
            for (i32 y = std::max(first_free, shape.min_y); y <= top; ++y) {
                frontier.push_back(Cell{static_cast<u8>(x), y, static_cast<u8>(z)});
            }
        }
    }

    for (usize head = 0; head < frontier.size(); ++head) {
        const Cell cell    = frontier[head];
        const u8   current = light_at(cell.x, cell.y, cell.z);
        if (current <= 1) {
            continue;
        }
        const u8 spread = static_cast<u8>(current - 1);

        const std::array<Cell, 6> neighbours{{
            {static_cast<u8>(cell.x - 1), cell.y, cell.z},
            {static_cast<u8>(cell.x + 1), cell.y, cell.z},
            {cell.x, cell.y, static_cast<u8>(cell.z - 1)},
            {cell.x, cell.y, static_cast<u8>(cell.z + 1)},
            {cell.x, cell.y - 1, cell.z},
            {cell.x, cell.y + 1, cell.z},
        }};

        for (const Cell& next : neighbours) {
            // Unsigned wrap makes an x of -1 become 255, so one comparison
            // covers both edges.
            if (next.x >= 16 || next.z >= 16 || next.y < shape.min_y || next.y > top) {
                continue;
            }
            if (stops_sky_light(blocks, chunk.get_block(next.x, next.y, next.z))) {
                continue;
            }
            if (light_at(next.x, next.y, next.z) >= spread) {
                continue;
            }
            set_light(next.x, next.y, next.z, spread);
            frontier.push_back(next);
        }
    }

    for (usize i = 0; i < shape.section_count(); ++i) {
        world::ChunkSection* section = chunk.section_for_y(shape.min_y + static_cast<i32>(i) * 16);
        if (section != nullptr) {
            section->sky_light().compact();
        }
    }
}

void relight_neighbourhood(const ChunkLookup& lookup, i32 centre_x, i32 centre_z,
                           const registry::BlockRegistry* blocks) {
    const auto shape = world::WorldShape::overworld();

    // The 3x3, as a grid rather than a list: a cell's chunk is two shifts and
    // an index, where the list cost a linear search on every read and write.
    constexpr i32                 kSpan = 48;  // three chunks, in blocks
    std::array<world::Chunk*, 9> grid{};
    bool                          any = false;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            world::Chunk* chunk = lookup(centre_x + dx, centre_z + dz);
            grid[static_cast<usize>((dz + 1) * 3 + (dx + 1))] = chunk;
            any |= chunk != nullptr;
        }
    }
    if (!any) {
        return;
    }
    const i32 origin_x = (centre_x - 1) * 16;
    const i32 origin_z = (centre_z - 1) * 16;

    // World coordinates throughout: the whole point is that the fill does not
    // know where the borders are.
    const auto chunk_for = [&](i32 x, i32 z) -> world::Chunk* {
        const i32 gx = x - origin_x;
        const i32 gz = z - origin_z;
        if (gx < 0 || gx >= kSpan || gz < 0 || gz >= kSpan) {
            return nullptr;
        }
        return grid[static_cast<usize>((gz >> 4) * 3 + (gx >> 4))];
    };

    i32 top = shape.min_y;
    for (world::Chunk* chunk : grid) {
        if (chunk == nullptr) {
            continue;
        }
        const auto& surface = chunk->heightmap(world::HeightmapType::WorldSurface);
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                top = std::max(top, surface.first_free(x, z));
            }
        }
    }
    top = std::min(top + 1, shape.max_y());

    const auto light_at = [&](i32 x, i32 y, i32 z) -> u8 {
        world::Chunk* chunk = chunk_for(x, z);
        if (chunk == nullptr) {
            return 0;
        }
        const world::ChunkSection* section = chunk->section_for_y(y);
        return section == nullptr ? 0
                                  : section->sky_light().get(world::section_index(
                                        static_cast<usize>(x & 15), static_cast<usize>(y & 15),
                                        static_cast<usize>(z & 15)));
    };
    const auto set_light = [&](i32 x, i32 y, i32 z, u8 value) {
        world::Chunk* chunk = chunk_for(x, z);
        if (chunk == nullptr) {
            return;
        }
        world::ChunkSection* section = chunk->section_for_y(y);
        if (section != nullptr) {
            section->sky_light().set(
                world::section_index(static_cast<usize>(x & 15), static_cast<usize>(y & 15),
                                     static_cast<usize>(z & 15)),
                value);
        }
    };

    // Where full sunlight stops, per column of the 48x48. A column whose chunk
    // is not loaded is marked so: it neither takes light nor gives any.
    constexpr i32                            kUnloaded = std::numeric_limits<i32>::max();
    std::array<i32, static_cast<usize>(kSpan * kSpan)> floors{};
    floors.fill(kUnloaded);
    const auto floor_at = [&](i32 gx, i32 gz) -> i32 {
        if (gx < 0 || gx >= kSpan || gz < 0 || gz >= kSpan) {
            return kUnloaded;
        }
        return floors[static_cast<usize>(gz * kSpan + gx)];
    };

    for (i32 gz = 0; gz < kSpan; ++gz) {
        for (i32 gx = 0; gx < kSpan; ++gx) {
            const world::Chunk* chunk = grid[static_cast<usize>((gz >> 4) * 3 + (gx >> 4))];
            if (chunk == nullptr) {
                continue;
            }
            floors[static_cast<usize>(gz * kSpan + gx)] = sky_floor(
                blocks, *chunk, static_cast<usize>(gx & 15), static_cast<usize>(gz & 15), top);
        }
    }

    // Direct sunlight over the band, a section at a time. Cells above `top`
    // keep what they held — the previous pass did the same, and the fill never
    // reaches them.
    for (i32 gz = 0; gz < kSpan; ++gz) {
        for (i32 gx = 0; gx < kSpan; ++gx) {
            world::Chunk* chunk = grid[static_cast<usize>((gz >> 4) * 3 + (gx >> 4))];
            if (chunk == nullptr) {
                continue;
            }
            const i32   first_free = floor_at(gx, gz);
            const usize lx         = static_cast<usize>(gx & 15);
            const usize lz         = static_cast<usize>(gz & 15);
            for (i32 bottom = shape.min_y; bottom <= top; bottom += 16) {
                world::ChunkSection* section = chunk->section_for_y(bottom);
                if (section == nullptr) {
                    continue;
                }
                world::LightArray& sky = section->sky_light();
                for (i32 local_y = 0; local_y < 16 && bottom + local_y <= top; ++local_y) {
                    const i32 y = bottom + local_y;
                    sky.set(world::section_index(lx, static_cast<usize>(local_y), lz),
                            y >= first_free ? world::kMaxLightLevel : 0);
                }
            }
        }
    }

    struct Cell {
        i32 x;
        i32 y;
        i32 z;
    };

    // The seeds: lit cells that have an unlit neighbour, and nothing else. A
    // cell at 15 beside cells at 15 gives nothing to anyone, and seeding all of
    // them was a few hundred thousand pushes a pass — the bulk of its cost.
    // The unlit neighbours of a lit cell can only be sideways, in a column
    // whose sunlight stops higher up, or straight below the column's own
    // floor. The fill reaches the same fixed point from these as from every
    // lit cell, because every cell that could raise a neighbour is still here.
    std::vector<Cell> frontier;
    for (i32 gz = 0; gz < kSpan; ++gz) {
        for (i32 gx = 0; gx < kSpan; ++gx) {
            const i32 first_free = floor_at(gx, gz);
            if (first_free == kUnloaded) {
                continue;
            }
            i32 shadow = std::numeric_limits<i32>::min();
            for (const auto& [dx, dz] : std::array<std::pair<i32, i32>, 4>{
                     {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}}) {
                const i32 other = floor_at(gx + dx, gz + dz);
                if (other != kUnloaded) {
                    shadow = std::max(shadow, other);
                }
            }
            const i32 x    = origin_x + gx;
            const i32 z    = origin_z + gz;
            const i32 from = std::max(first_free, shape.min_y);
            const i32 to   = std::min(top, shadow - 1);
            for (i32 y = from; y <= to; ++y) {
                frontier.push_back(Cell{x, y, z});
            }
            if (first_free > shape.min_y && first_free <= top && first_free > to) {
                frontier.push_back(Cell{x, first_free, z});
            }
        }
    }

    for (usize head = 0; head < frontier.size(); ++head) {
        const Cell cell    = frontier[head];
        const u8   current = light_at(cell.x, cell.y, cell.z);
        if (current <= 1) {
            continue;
        }
        const u8 spread = static_cast<u8>(current - 1);

        const std::array<Cell, 6> neighbours{{
            {cell.x - 1, cell.y, cell.z},
            {cell.x + 1, cell.y, cell.z},
            {cell.x, cell.y, cell.z - 1},
            {cell.x, cell.y, cell.z + 1},
            {cell.x, cell.y - 1, cell.z},
            {cell.x, cell.y + 1, cell.z},
        }};

        for (const Cell& next : neighbours) {
            if (next.y < shape.min_y || next.y > top) {
                continue;
            }
            world::Chunk* chunk = chunk_for(next.x, next.z);
            if (chunk == nullptr) {
                continue;  // outside the loaded neighbourhood
            }
            if (stops_sky_light(blocks,
                                chunk->get_block(static_cast<usize>(next.x & 15), next.y,
                                                 static_cast<usize>(next.z & 15)))) {
                continue;
            }
            if (light_at(next.x, next.y, next.z) >= spread) {
                continue;
            }
            set_light(next.x, next.y, next.z, spread);
            frontier.push_back(next);
        }
    }

    for (world::Chunk* chunk : grid) {
        if (chunk == nullptr) {
            continue;
        }
        for (usize i = 0; i < shape.section_count(); ++i) {
            world::ChunkSection* section =
                chunk->section_for_y(shape.min_y + static_cast<i32>(i) * 16);
            if (section != nullptr) {
                section->sky_light().compact();
            }
        }
    }
}

void relight_after_edit(const ChunkLookup& lookup, i32 centre_x, i32 centre_z,
                        const registry::BlockRegistry* blocks) {
    relight_neighbourhood(lookup, centre_x, centre_z, blocks);
    if (blocks == nullptr) {
        return;
    }
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (world::Chunk* found = lookup(centre_x + dx, centre_z + dz); found != nullptr) {
                relight_blocks(*found, *blocks);
            }
        }
    }
}

}  // namespace ov::server
