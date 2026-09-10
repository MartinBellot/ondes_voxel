#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/chunk_generator.hpp"

#include "ov/base/log.hpp"

#include <cstdlib>
#include <string_view>

namespace ov::worldgen {

namespace {

/// Below this the fluid is lava rather than water. Not the sea level and not
/// derived from it: a separate constant in the generator, and the reason the
/// bottom of the world is a lava sea rather than an ocean.
constexpr i32 kLavaLevel = -54;

/// The tag that says what a carver is allowed to cut through.
///
/// A tag and not a list: it resolves to every stone of the overworld, every
/// dirt including grass, every sand and sandstone, the terracottas, the iron
/// and copper ores, gravel, calcite, snow, packed ice and water. Writing that
/// out by hand is how a list goes stale.
constexpr std::string_view kReplaceablesTag = "minecraft:overworld_carver_replaceables";

constexpr std::string_view kBlockRegistry = "minecraft:block";

}  // namespace

std::string_view to_string(CarverAttachError error) noexcept {
    switch (error) {
        case CarverAttachError::NoBlockRegistry:
            return "the pack has no minecraft:block registry";
        case CarverAttachError::NoReplaceablesTag:
            return "the pack has no minecraft:overworld_carver_replaceables tag";
        case CarverAttachError::UnknownMember:
            return "the tag names a block the block registry does not have";
    }
    return "unknown";
}

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
    dirt_  = resolve("minecraft:dirt");

    if (const auto grass = blocks.find_block("minecraft:grass_block")) {
        grass_block_     = *grass;
        has_grass_block_ = true;
    }

    if (const char* setting = std::getenv("OV_CARVE_BEFORE_SURFACE"); setting != nullptr) {
        carve_before_surface_ = std::string_view(setting) != "0";
        if (carve_before_surface_) {
            OV_LOG_WARN(
                "worldgen: OV_CARVE_BEFORE_SURFACE is set; the carvers run before the surface "
                "rules and cut stone only. This is the pre-reordering behaviour, kept so the "
                "two orders can be measured against each other, and it is not the game's");
        }
    }

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

std::string_view ChunkGenerator::biome_name_at(i32 x, i32 y, i32 z) const {
    // Quart positions: the biome grid is one cell per four blocks on each axis.
    // An arithmetic shift and not a division, because `-1 / 4` is 0 in C++ and
    // the block at x = -1 belongs to quart -1.
    const auto climate = biomes_->sample(*router_, x >> 2, y >> 2, z >> 2);
    return biomes_->biome_at(climate);
}

registry::BlockStateId ChunkGenerator::fluid_at(i32 y) const {
    if (y < std::min(kLavaLevel, sea_level_)) {
        return y < kLavaLevel ? lava_ : registry::kAirState;
    }
    return y < sea_level_ ? water_ : registry::kAirState;
}

bool ChunkGenerator::is_carver_replaceable(registry::BlockId block) const noexcept {
    const auto index = static_cast<usize>(block.value());
    return index < replaceable_.size() && replaceable_[index];
}

std::expected<void, CarverAttachError> ChunkGenerator::set_carvers(
    const CarverStage* carvers, const registry::Registries& registries) {
    const auto block_registry = registries.find(kBlockRegistry);
    if (!block_registry) {
        OV_LOG_ERROR("worldgen: {}", to_string(CarverAttachError::NoBlockRegistry));
        return std::unexpected(CarverAttachError::NoBlockRegistry);
    }
    const auto tag = registries.find_tag(*block_registry, kReplaceablesTag);
    if (!tag) {
        OV_LOG_ERROR("worldgen: {}", to_string(CarverAttachError::NoReplaceablesTag));
        return std::unexpected(CarverAttachError::NoReplaceablesTag);
    }

    // Resolved through names rather than by assuming that a block registry
    // index and a wire id are the same number. They are, in 1.20.1 — but the
    // assumption is invisible while it holds and silent when it stops, and this
    // runs once per generator rather than once per block.
    std::vector<bool> table(blocks_->block_count(), false);
    usize             members = 0;
    for (const registry::ProtocolId id : registries.tag_members(*tag)) {
        const std::string_view name = registries.entry_of(*block_registry, id);
        if (name.empty()) {
            OV_LOG_ERROR("worldgen: {} lists wire id {}, which minecraft:block does not have",
                         kReplaceablesTag, id);
            return std::unexpected(CarverAttachError::UnknownMember);
        }
        const auto block = blocks_->find_block(name);
        if (!block) {
            OV_LOG_ERROR("worldgen: {} lists '{}', which the block registry does not have",
                         kReplaceablesTag, name);
            return std::unexpected(CarverAttachError::UnknownMember);
        }
        table[static_cast<usize>(block->value())] = true;
        ++members;
    }

    replaceable_ = std::move(table);
    carvers_     = carvers;

    // Emptying a carved cell that holds a fluid is what the tag literally says
    // — `minecraft:water` is a member. The reasoning against it was that the
    // game asks the aquifer, we have none, and draining carved water would
    // empty sea beds the game kept full. The measurement said otherwise, which
    // is why it is on: our noise fills every non-solid cell below the sea level
    // with water, so a dry cave under dry land comes out flooded, and cutting
    // the fluid fixes far more of those than it breaks under the sea. Set to 0
    // to put it back and re-measure once the aquifer exists.
    if (const char* setting = std::getenv("OV_CARVE_FLUIDS"); setting != nullptr) {
        carve_fluids_ = std::string_view(setting) != "0";
    }

    OV_LOG_INFO("worldgen: carvers attached, {} replaceable blocks, fluids {}", members,
                carve_fluids_ ? "carved" : "left alone");
    return {};
}

void ChunkGenerator::apply_carving(world::Chunk& chunk, const CarvingMask& mask,
                                   i32 lava_level) const {
    const auto shape = chunk.shape();

    for (usize local_z = 0; local_z < 16; ++local_z) {
        for (usize local_x = 0; local_x < 16; ++local_x) {
            // Upwards, and the direction is load-bearing. The grass rule looks
            // one block up, so a cell has to be cut before the block above it
            // is asked about; and when that block above is itself carved, the
            // dirt this pass just wrote there is reached later in the same
            // sweep and removed, which is the right answer rather than a
            // coincidence.
            for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                if (!mask.get(static_cast<i32>(local_x), y, static_cast<i32>(local_z))) {
                    continue;
                }
                const auto state = chunk.get_block(local_x, y, local_z);
                if (state == registry::kAirState) {
                    continue;
                }
                if (!carve_fluids_ && blocks_->holds_fluid(state)) {
                    continue;
                }
                if (!is_carver_replaceable(blocks_->block_of(state))) {
                    continue;
                }

                chunk.set_block(local_x, y, local_z,
                                y <= lava_level ? lava_ : registry::kAirState);

                // The grass above a cell that has just been cut becomes dirt.
                // Without it a cave eating into a hillside from underneath
                // leaves a ceiling of grass, which the game never has: grass is
                // a surface block and the surface is no longer there.
                if (!has_grass_block_ || y >= shape.max_y()) {
                    continue;
                }
                const auto above = chunk.get_block(local_x, y + 1, local_z);
                if (above != registry::kAirState && blocks_->block_of(above) == grass_block_) {
                    chunk.set_block(local_x, y + 1, local_z, dirt_);
                }
            }
        }
    }
}

void ChunkGenerator::generate_noise(world::Chunk& chunk) const {
    const auto shape    = chunk.shape();
    const i32  origin_x = chunk.position().x * 16;
    const i32  origin_z = chunk.position().z * 16;

    // Stone, water, lava, air. Nothing is cut here: the carvers run after the
    // surface rules, which is the game's order and the reason this file was
    // rewritten. See the header.
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

    // The order this file used to have, kept only so that the two can be
    // compared on one sample. Carving here cuts stone and nothing else, because
    // before the surface rules run there is nothing else to cut — which is
    // precisely the thing that was wrong with it.
    if (carve_before_surface_ && carvers_ != nullptr) {
        const CarvingMask mask = carvers_->carve(chunk.position().x, chunk.position().z);
        const i32         lava_level = carvers_->context().lava_level();
        for (usize local_z = 0; local_z < 16; ++local_z) {
            for (usize local_x = 0; local_x < 16; ++local_x) {
                for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                    if (!mask.get(static_cast<i32>(local_x), y, static_cast<i32>(local_z))) {
                        continue;
                    }
                    if (chunk.get_block(local_x, y, local_z) != stone_) {
                        continue;
                    }
                    chunk.set_block(local_x, y, local_z,
                                    y <= lava_level ? lava_ : registry::kAirState);
                }
            }
        }
    }
}

void ChunkGenerator::generate_biomes(world::Chunk& chunk) const {
    const auto shape    = chunk.shape();
    const i32  origin_x = chunk.position().x * 16;
    const i32  origin_z = chunk.position().z * 16;

    // On their own 4x4x4 grid, from the same router.
    //
    // The nesting is not free to choose. The climate search keeps a one-entry
    // cache that decides ties, so the order the cells are asked about is part
    // of the answer: section by section from the bottom, then x, then y, then
    // z. Asked cell by cell with no cache at all, 2197 of 7821312 cells came
    // out differently from the real game at seed 1234567890; asked in this
    // order, none do.
    BiomeSearchCache cache;
    for (i32 section_y = shape.min_y; section_y <= shape.max_y(); section_y += 16) {
        for (usize local_x = 0; local_x < 16; local_x += 4) {
            for (i32 y = section_y; y < section_y + 16; y += 4) {
                for (usize local_z = 0; local_z < 16; local_z += 4) {
                    const i32  quart_x = (origin_x + static_cast<i32>(local_x)) >> 2;
                    const i32  quart_z = (origin_z + static_cast<i32>(local_z)) >> 2;
                    const auto climate = biomes_->sample(*router_, quart_x, y >> 2, quart_z);
                    const auto name    = biomes_->biome_at(climate, cache);
                    if (const auto index = blocks_->find_biome(name)) {
                        chunk.set_biome(local_x, y, local_z, static_cast<u16>(*index));
                    }
                }
            }
        }
    }
}

void ChunkGenerator::generate_surface(world::Chunk& chunk) const {
    // After the biomes, because the rules ask which biome a block is in on
    // almost every line — and before the carvers, because the rules count
    // `stone_depth` down from the top of a column and a cave that had already
    // eaten that top would have them counting from its ceiling.
    if (surface_ != nullptr) {
        surface_->build(chunk, *router_, *biomes_, *blocks_);
    }
}

void ChunkGenerator::generate_carvers(world::Chunk& chunk) const {
    // The mask is a record of what the carvers considered, taken for the whole
    // chunk at once; it depends on nothing but the seed, the chunk and the
    // world's height, and it is bit-for-bit the game's. Applying it is the
    // separate decision: lava below the carvers' own lava level, air above it,
    // and one day the aquifer's water in between.
    if (carvers_ != nullptr && !carve_before_surface_) {
        const CarvingMask mask = carvers_->carve(chunk.position().x, chunk.position().z);
        apply_carving(chunk, mask, carvers_->context().lava_level());
    }
}

void ChunkGenerator::generate(world::Chunk& chunk) const {
    generate_noise(chunk);
    generate_biomes(chunk);
    generate_surface(chunk);
    generate_carvers(chunk);

    // Last, and after the carvers rather than before them: a carved cell can be
    // the very block a heightmap was pointing at.
    chunk.recompute_heightmaps();
}

}  // namespace ov::worldgen
