// The order of the generation stages, and what the carvers are allowed to cut.
//
// What a unit test can settle here is the part that is a *fact* rather than a
// degree of agreement: that the replaceables tag resolves out of the pack
// rather than out of a list somebody typed, that it holds the blocks the
// surface rules put on top of a column and not merely stone, that a carved cell
// really is emptied whatever it was made of, and that grass left over a cut
// becomes dirt.
//
// What the ordering is *worth* is not settled here and cannot be: the oracle
// for that is a world the real game wrote. tools/ov_caveedge runs both stage
// orders over the same chunks of run/reference-1234567890 and reports which one
// puts the game's block on the skin of a cave. See
// docs/provenance/ordre-des-etages.md for the numbers.

#include "ov/registry/registries.hpp"
#include "ov/worldgen/chunk_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path data_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated" /
           "data" / "minecraft";
}

[[nodiscard]] std::filesystem::path reports_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated";
}

[[nodiscard]] std::filesystem::path registry_pack() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
           "registry.ovpack";
}

constexpr i64 kSeed = 1234567890;

[[nodiscard]] bool data_present() {
    return std::filesystem::is_directory(data_root() / "worldgen") &&
           std::filesystem::is_regular_file(registry_pack());
}

}  // namespace

TEST_CASE("the carvers may replace what the surface rules put down", "[worldgen][carve]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto registries = registry::Registries::load(registry_pack());
    REQUIRE(registries.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());

    ChunkGenerator generator{*router, *biomes, *blocks};

    // Before the tag is resolved nothing is replaceable. The point is that the
    // set comes from the pack and is empty until it does, rather than starting
    // life as a built-in guess.
    const auto stone = blocks->find_block("minecraft:stone");
    REQUIRE(stone.has_value());
    CHECK_FALSE(generator.is_carver_replaceable(*stone));

    const CarvingContext context{router->min_y(), router->height()};
    const CarverStage    carvers{kSeed, context};
    auto                 attached = generator.set_carvers(&carvers, *registries);
    REQUIRE(attached.has_value());

    // The blocks the noise stage puts down.
    CHECK(generator.is_carver_replaceable(*stone));
    for (const std::string_view name :
         {"minecraft:deepslate", "minecraft:water", "minecraft:granite"}) {
        const auto block = blocks->find_block(name);
        REQUIRE(block.has_value());
        CHECK(generator.is_carver_replaceable(*block));
    }

    // And the blocks the *surface rules* put down, which is the whole reason
    // the order was changed: carving only stone was correct only for as long
    // as the surface had not run yet.
    for (const std::string_view name : {"minecraft:grass_block", "minecraft:dirt",
                                        "minecraft:sand", "minecraft:gravel",
                                        "minecraft:sandstone", "minecraft:terracotta",
                                        "minecraft:podzol", "minecraft:snow"}) {
        const auto block = blocks->find_block(name);
        REQUIRE(block.has_value());
        INFO(name);
        CHECK(generator.is_carver_replaceable(*block));
    }

    // What the tag deliberately leaves out. Bedrock is the one that matters:
    // a carver that cut it would open the bottom of the world.
    for (const std::string_view name :
         {"minecraft:bedrock", "minecraft:lava", "minecraft:obsidian"}) {
        const auto block = blocks->find_block(name);
        REQUIRE(block.has_value());
        INFO(name);
        CHECK_FALSE(generator.is_carver_replaceable(*block));
    }
}

TEST_CASE("a generated chunk is carved after its surface, not before",
          "[worldgen][carve][order]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto registries = registry::Registries::load(registry_pack());
    REQUIRE(registries.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());
    auto surface = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(surface.has_value());

    const CarvingContext context{router->min_y(), router->height()};
    const CarverStage    carvers{kSeed, context};

    ChunkGenerator generator{*router, *biomes, *blocks};
    generator.set_surface_system(&*surface);
    REQUIRE(generator.set_carvers(&carvers, *registries).has_value());

    const auto shape = world::WorldShape::overworld();
    const auto air   = world::AirStates::from(*blocks);

    // A chunk that is actually carved. Picked by asking the mask rather than
    // by hoping: most chunks are carved, but a test that silently landed on one
    // that is not would pass while checking nothing.
    ov::ChunkPos position{0, 0};
    CarvingMask  mask{context.min_y, context.height};
    bool         found = false;
    for (i32 chunk_x = 0; chunk_x < 8 && !found; ++chunk_x) {
        for (i32 chunk_z = 0; chunk_z < 8 && !found; ++chunk_z) {
            carvers.carve_into(chunk_x, chunk_z, mask);
            if (!mask.empty()) {
                position = ov::ChunkPos{chunk_x, chunk_z};
                found    = true;
            }
        }
    }
    REQUIRE(found);
    carvers.carve_into(position.x, position.z, mask);

    world::Chunk chunk{position, shape, air, &*blocks};
    generator.generate(chunk);

    const auto grass_block = blocks->find_block("minecraft:grass_block");
    REQUIRE(grass_block.has_value());

    usize carved_cells   = 0;
    usize still_solid    = 0;
    usize grass_on_a_cut = 0;
    for (i32 local_z = 0; local_z < 16; ++local_z) {
        for (i32 local_x = 0; local_x < 16; ++local_x) {
            const auto ax = static_cast<usize>(local_x);
            const auto az = static_cast<usize>(local_z);
            for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                if (!mask.get(local_x, y, local_z)) {
                    continue;
                }
                ++carved_cells;
                const auto state = chunk.get_block(ax, y, az);
                // A carved cell holds air, lava, or something the tag does not
                // let a carver touch. What it must never hold is a block the
                // tag lists — that would mean the mask was applied to a terrain
                // that no longer existed by the time it mattered.
                if (state != registry::kAirState &&
                    generator.is_carver_replaceable(blocks->block_of(state)) &&
                    !blocks->holds_fluid(state)) {
                    ++still_solid;
                }
                // Grass sitting directly on a cut. The rule this file exists
                // for: the surface is no longer under it, so it is not grass.
                if (y < shape.max_y()) {
                    const auto above = chunk.get_block(ax, y + 1, az);
                    if (above != registry::kAirState &&
                        blocks->block_of(above) == *grass_block) {
                        ++grass_on_a_cut;
                    }
                }
            }
        }
    }

    CHECK(carved_cells > 0);
    CHECK(still_solid == 0);
    CHECK(grass_on_a_cut == 0);
}

TEST_CASE("the two stage orders really do produce different chunks",
          "[worldgen][carve][order]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto registries = registry::Registries::load(registry_pack());
    REQUIRE(registries.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());
    auto surface = SurfaceSystem::load(data_root(), "overworld", kSeed, *blocks);
    REQUIRE(surface.has_value());

    const CarvingContext context{router->min_y(), router->height()};
    const CarverStage    carvers{kSeed, context};

    const auto shape = world::WorldShape::overworld();
    const auto air   = world::AirStates::from(*blocks);

    // The legacy order is a measuring instrument and this is what guards it: if
    // it ever stopped differing from the real one, every before/after number in
    // docs/provenance/ordre-des-etages.md would silently become a comparison of
    // something with itself.
    usize differing = 0;
    for (i32 chunk_x = 0; chunk_x < 4 && differing == 0; ++chunk_x) {
        for (i32 chunk_z = 0; chunk_z < 4 && differing == 0; ++chunk_z) {
            const ov::ChunkPos position{chunk_x, chunk_z};

            world::Chunk modern{position, shape, air, &*blocks};
            world::Chunk legacy{position, shape, air, &*blocks};

            ChunkGenerator generator{*router, *biomes, *blocks};
            generator.set_surface_system(&*surface);
            REQUIRE(generator.set_carvers(&carvers, *registries).has_value());

            generator.set_carve_before_surface(false);
            generator.generate(modern);
            generator.set_carve_before_surface(true);
            generator.generate(legacy);

            for (i32 local_z = 0; local_z < 16; ++local_z) {
                for (i32 local_x = 0; local_x < 16; ++local_x) {
                    const auto ax = static_cast<usize>(local_x);
                    const auto az = static_cast<usize>(local_z);
                    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                        if (modern.get_block(ax, y, az) != legacy.get_block(ax, y, az)) {
                            ++differing;
                        }
                    }
                }
            }
        }
    }
    CHECK(differing > 0);
}
