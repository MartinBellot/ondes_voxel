#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/chunk_generator.hpp"

#include "ov/base/log.hpp"

namespace ov::worldgen {

namespace {

/// Below this the fluid is lava rather than water. Not the sea level and not
/// derived from it: a separate constant in the generator, and the reason the
/// bottom of the world is a lava sea rather than an ocean.
constexpr i32 kLavaLevel = -54;

}  // namespace

ChunkGenerator::ChunkGenerator(const NoiseRouter& router, const BiomeSource& biomes,
                               const registry::BlockRegistry& blocks)
    : router_(&router), biomes_(&biomes), blocks_(&blocks) {
    density_   = router.entry("final_density");
    sea_level_ = router.sea_level();

    const auto resolve = [&](std::string_view name) {
        const auto block = blocks.find_block(name);
        return block ? blocks.default_state(*block) : registry::kAirState;
    };
    stone_ = resolve("minecraft:stone");
    water_ = resolve("minecraft:water");
    lava_  = resolve("minecraft:lava");

    if (density_ == nullptr) {
        OV_LOG_ERROR("worldgen: the router has no final_density; nothing will be generated");
    }
}

bool ChunkGenerator::is_solid(i32 x, i32 y, i32 z) const {
    if (density_ == nullptr) {
        return false;
    }
    // Strictly greater, and the boundary matters: a density of exactly zero is
    // not stone.
    return density_->compute(FunctionContext{x, y, z}) > 0.0;
}

f64 ChunkGenerator::density_at(i32 x, i32 y, i32 z) const {
    return density_ == nullptr ? 0.0 : density_->compute(FunctionContext{x, y, z});
}

registry::BlockStateId ChunkGenerator::fluid_at(i32 y) const {
    if (y < std::min(kLavaLevel, sea_level_)) {
        return y < kLavaLevel ? lava_ : registry::kAirState;
    }
    return y < sea_level_ ? water_ : registry::kAirState;
}

void ChunkGenerator::generate(world::Chunk& chunk) const {
    const auto shape    = chunk.shape();
    const i32  origin_x = chunk.position().x * 16;
    const i32  origin_z = chunk.position().z * 16;

    for (usize local_z = 0; local_z < 16; ++local_z) {
        for (usize local_x = 0; local_x < 16; ++local_x) {
            const i32 world_x = origin_x + static_cast<i32>(local_x);
            const i32 world_z = origin_z + static_cast<i32>(local_z);
            for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                const auto state = is_solid(world_x, y, world_z) ? stone_ : fluid_at(y);
                if (state != registry::kAirState) {
                    chunk.set_block(local_x, y, local_z, state);
                }
            }
        }
    }

    // Biomes on their own 4x4x4 grid, from the same router.
    for (i32 y = shape.min_y; y <= shape.max_y(); y += 4) {
        for (usize local_z = 0; local_z < 16; local_z += 4) {
            for (usize local_x = 0; local_x < 16; local_x += 4) {
                const i32  quart_x = (origin_x + static_cast<i32>(local_x)) >> 2;
                const i32  quart_z = (origin_z + static_cast<i32>(local_z)) >> 2;
                const auto climate = biomes_->sample(*router_, quart_x, y >> 2, quart_z);
                const auto name    = biomes_->biome_at(climate);
                if (const auto index = blocks_->find_biome(name)) {
                    chunk.set_biome(local_x, y, local_z, static_cast<u16>(*index));
                }
            }
        }
    }

    chunk.recompute_heightmaps();
}

}  // namespace ov::worldgen
