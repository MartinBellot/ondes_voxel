#define OV_LOG_CATEGORY "voxel"

#include "world_source.hpp"

#include "ov/base/log.hpp"
#include "ov/nbt/region.hpp"
#include "ov/world/chunk_section.hpp"
#include "ov/world/chunk_storage.hpp"

#include <algorithm>
#include <utility>

namespace ov::demo {

namespace {

[[nodiscard]] constexpr i32 region_of(i32 chunk) noexcept {
    return chunk >> 5;
}

[[nodiscard]] constexpr u32 inside_region(i32 chunk) noexcept {
    return static_cast<u32>(chunk & 31);
}

}  // namespace

const world::Chunk* LoadedWorld::at(i32 x, i32 z) const {
    const auto it = chunks.find({x, z});
    return it == chunks.end() ? nullptr : it->second.get();
}

render::ChunkNeighbours LoadedWorld::neighbours(i32 x, i32 z) const {
    render::ChunkNeighbours around{};
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            around[static_cast<usize>((dz + 1) * 3 + (dx + 1))] = at(x + dx, z + dz);
        }
    }
    return around;
}

Vec3f LoadedWorld::suggested_camera(const registry::BlockRegistry& blocks) const {
    if (chunks.empty()) {
        return Vec3f{0.0F, 80.0F, 0.0F};
    }

    // Middle of what was loaded, then walk down the column to the first thing
    // that is not air. Landing inside a mountain and reporting "nothing
    // renders" is a waste of a run.
    i32 sum_x = 0;
    i32 sum_z = 0;
    for (const auto& [position, chunk] : chunks) {
        sum_x += position.first;
        sum_z += position.second;
    }
    const i32 centre_x = (sum_x / static_cast<i32>(chunks.size())) * 16 + 8;
    const i32 centre_z = (sum_z / static_cast<i32>(chunks.size())) * 16 + 8;

    const world::Chunk* chunk = at(centre_x >> 4, centre_z >> 4);
    if (chunk == nullptr) {
        return Vec3f{static_cast<f32>(centre_x), 80.0F, static_cast<f32>(centre_z)};
    }

    const auto shape   = chunk->shape();
    i32        surface = shape.min_y;
    for (i32 y = shape.max_y(); y >= shape.min_y; --y) {
        const auto state = chunk->get_block(static_cast<usize>(centre_x & 15), y,
                                            static_cast<usize>(centre_z & 15));
        if (state != registry::kAirState && !blocks.is_air(blocks.block_of(state))) {
            surface = y;
            break;
        }
    }
    return Vec3f{static_cast<f32>(centre_x) + 0.5F, static_cast<f32>(surface) + 3.0F,
                 static_cast<f32>(centre_z) + 0.5F};
}

std::optional<LoadedWorld> load_world(const std::filesystem::path&   world_directory,
                                      const registry::BlockRegistry& blocks, i32 centre_x,
                                      i32 centre_z, i32 radius) {
    const auto region_directory = world_directory / "region";
    if (!std::filesystem::is_directory(region_directory)) {
        OV_LOG_ERROR("{} has no region/ directory", world_directory.string());
        return std::nullopt;
    }

    world::ChunkCodecContext context;
    context.blocks = &blocks;
    // Without this, cave_air and void_air are not recognised as air and a whole
    // cave system meshes as solid blocks of nothing.
    context.air = world::AirStates::from(blocks);

    LoadedWorld world;
    // Region files are a few megabytes each and a radius spans at most four of
    // them, so each is opened once and kept for the whole sweep rather than
    // re-read per chunk.
    std::map<std::pair<i32, i32>, std::optional<nbt::RegionFile>> regions;

    for (i32 z = centre_z - radius; z <= centre_z + radius; ++z) {
        for (i32 x = centre_x - radius; x <= centre_x + radius; ++x) {
            const std::pair<i32, i32> region_key{region_of(x), region_of(z)};

            auto region = regions.find(region_key);
            if (region == regions.end()) {
                const auto path =
                    region_directory / ("r." + std::to_string(region_key.first) + "." +
                                        std::to_string(region_key.second) + ".mca");
                auto opened = nbt::RegionFile::open(path);
                region = regions
                             .emplace(region_key,
                                      opened ? std::optional<nbt::RegionFile>(std::move(*opened))
                                             : std::nullopt)
                             .first;
                if (!region->second) {
                    OV_LOG_DEBUG("no region file {}", path.filename().string());
                }
            }
            if (!region->second) {
                continue;
            }

            const u32 local_x = inside_region(x);
            const u32 local_z = inside_region(z);
            if (!region->second->has_chunk(local_x, local_z)) {
                continue;
            }

            auto document = region->second->read_chunk(local_x, local_z);
            if (!document) {
                ++world.chunks_failed;
                continue;
            }
            auto chunk = world::from_nbt(*document, context);
            if (!chunk) {
                // A chunk this version cannot read is skipped, not guessed at.
                // See the note on from_nbt: half-read is worse than absent.
                ++world.chunks_failed;
                continue;
            }

            for (const auto& section : chunk->sections()) {
                if (!section.sky_light().is_absent() || !section.block_light().is_absent()) {
                    world.light_stored = true;
                    break;
                }
            }

            ++world.chunks_read;
            world.chunks.emplace(std::pair<i32, i32>{x, z},
                                 std::make_unique<world::Chunk>(std::move(*chunk)));
        }
    }

    if (world.chunks.empty()) {
        OV_LOG_ERROR("no chunks around ({}, {}) in {}", centre_x, centre_z,
                     region_directory.string());
        return std::nullopt;
    }
    return world;
}

}  // namespace ov::demo
