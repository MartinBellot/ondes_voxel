// The Nether's generation, pinned to the real game where it can be.
//
// Two columns below were read out of a Nether the vanilla 1.20.1 server
// generated at seed 1234567890 (`scripts/reference_nether.sh`), from chunks it
// stopped at `minecraft:carvers` — noise, surface and carvers and nothing else,
// which is exactly what `ChunkGenerator::generate()` produces. They are not our
// own output copied back: the whole-world figures are in
// docs/provenance/nether.md (`ov_netherparity`), and these two are the part
// that fits in a unit test.

#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/noise.hpp"
#include "ov/worldgen/random_factory.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

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
           std::filesystem::is_directory(reports_root() / "reports") &&
           std::filesystem::is_regular_file(registry_pack());
}

}  // namespace

TEST_CASE("a legacy positional factory is not the Xoroshiro one", "[worldgen][nether]") {
    const auto legacy    = PositionalRandomFactory::for_world(kSeed, true);
    const auto xoroshiro = PositionalRandomFactory::for_world(kSeed, false);
    CHECK(legacy.legacy());
    CHECK_FALSE(xoroshiro.legacy());

    // Positional draws are pure functions of the position, on both.
    CHECK(legacy.next_double_at(10, 0, -7) == legacy.next_double_at(10, 0, -7));
    CHECK(legacy.next_double_at(10, 0, -7) != xoroshiro.next_double_at(10, 0, -7));

    // `for_world` is `new LegacyRandomSource(seed).forkPositional()`: one long.
    math::LegacyRandomSource source{kSeed};
    const math::LegacyPositionalFactory by_hand{static_cast<u64>(source.next_long())};
    auto a = by_hand.at(3, 4, 5);
    CHECK(static_cast<f64>(a.next_double()) == legacy.next_double_at(3, 4, 5));

    // A named fork is `fromHashOf(name).forkPositional()`, again one long.
    auto named = by_hand.from_hash_of("minecraft:bedrock_floor");
    const math::LegacyPositionalFactory forked{static_cast<u64>(named.next_long())};
    auto b = forked.at(0, 2, 0);
    CHECK(b.next_float() == legacy.fork_named("minecraft:bedrock_floor").next_float_at(0, 2, 0));
}

TEST_CASE("the old initialisation spends an octave's draws even when it skips it",
          "[worldgen][nether]") {
    // Octaves -7 and -6: the old scheme builds octave 0 first (and throws it
    // away, it is out of range), skips -1..-5 at 262 draws each, and only then
    // builds the two it keeps. So the generator's state afterwards is exactly
    // three ImprovedNoise constructions and five skips past the start — which
    // is what makes the second stack of the NormalNoise a different field.
    constexpr std::array<f64, 2> kAmplitudes{1.0, 1.0};
    math::LegacyRandomSource     one{42};
    math::LegacyRandomSource     two{42};
    const NormalNoise a = NormalNoise::create_legacy_nether_biome(one, -7, kAmplitudes);
    const NormalNoise b = NormalNoise::create_legacy_nether_biome(two, -7, kAmplitudes);
    CHECK(a.value(12.0, 0.0, -30.0) == b.value(12.0, 0.0, -30.0));
    CHECK(one.next_long() == two.next_long());

    // And it is not the new initialisation from the same generator.
    math::LegacyRandomSource three{42};
    const NormalNoise        c = NormalNoise::create(three, -7, kAmplitudes);
    CHECK(c.value(12.0, 0.0, -30.0) != a.value(12.0, 0.0, -30.0));
}

TEST_CASE("the Nether's carver is nether_cave, with its own shape", "[worldgen][nether]") {
    const CarverStage stage = CarverStage::nether(kSeed);
    CHECK(stage.preset() == CarverPreset::Nether);
    CHECK(stage.context().min_y == 0);
    CHECK(stage.context().height == 256);
    CHECK(stage.context().depth() == 128);
    // Measured: the game's carved cells are lava at y <= 31 and cave air
    // above, in every one of 166 chunks — not the JSON's `above_bottom 10`.
    CHECK(stage.context().lava_level() == 31);
    CHECK(stage.replaceables_tag() == "minecraft:nether_carver_replaceables");
    CHECK(stage.carved_air() == "minecraft:cave_air");

    const CaveCarverConfig config = nether_cave_config(stage.context());
    CHECK(config.probability == 0.2F);
    CHECK(config.y_min == 0);
    CHECK(config.y_max == 126);  // below_top 1 of a 128-deep generator
    CHECK(config.nether);

    // A mask never reaches the top margin of the *generator*: 128 - 1 - 7.
    for (i32 cx = -3; cx <= 3; ++cx) {
        const CarvingMask mask = stage.carve(cx, 0);
        for (i32 y = 121; y < 256; ++y) {
            for (i32 z = 0; z < 16; ++z) {
                for (i32 x = 0; x < 16; ++x) {
                    REQUIRE_FALSE(mask.get(x, y, z));
                }
            }
        }
    }
}

TEST_CASE("the Nether generates the game's columns", "[worldgen][nether][parity]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto registries = registry::Registries::load(registry_pack());
    REQUIRE(registries.has_value());
    auto router = NoiseRouter::load(data_root(), "nether", kSeed);
    REQUIRE(router.has_value());
    CHECK(router->legacy_random_source());
    CHECK(router->default_block() == "minecraft:netherrack");
    CHECK(router->default_fluid() == "minecraft:lava");
    // ── carvers-3 ── The Nether has no aquifer: the lava sea fills every
    // empty cell under y 32, carved or not.
    CHECK_FALSE(router->aquifers_enabled());
    CHECK(router->sea_level() == 32);
    CHECK(router->height() == 128);
    auto biomes = BiomeSource::load(reports_root(), "nether");
    REQUIRE(biomes.has_value());
    auto surface = SurfaceSystem::load(data_root(), "nether", kSeed, *blocks);
    REQUIRE(surface.has_value());
    const CarverStage carvers = CarverStage::nether(kSeed);

    ChunkGenerator generator{*router, *biomes, *blocks};
    generator.set_surface_system(&*surface);
    REQUIRE(generator.set_carvers(&carvers, *registries).has_value());
    CHECK(generator.gen_depth() == 128);

    // `#` solid, `~` lava, `.` air or cave air, y = 0 on the left. Column
    // (3, 5) of chunks (10, 0) and (10, 1), as the game wrote them.
    struct Column {
        i32              chunk_z;
        std::string_view profile;
        i32              nylium_y;
    };
    // clang-format off
    constexpr std::array<Column, 2> kColumns{{
        {0, "#################~~~#####################........................................................................###############", 40},
        {1, "#############~###########################.......................................##########.............................#########", 40},
    }};
    // clang-format on
    for (const Column& column : kColumns) {
        REQUIRE(column.profile.size() == 128);
    }

    const auto air = world::AirStates::from(*blocks);
    for (const Column& expected : kColumns) {
        world::Chunk chunk{ChunkPos{10, expected.chunk_z}, world::WorldShape::nether(), air,
                           &*blocks};
        generator.generate(chunk);
        std::string profile;
        for (i32 y = 0; y < 128; ++y) {
            const auto state = chunk.get_block(3, y, 5);
            const std::string_view name =
                state == registry::kAirState ? "minecraft:air"
                                             : blocks->block_name(blocks->block_of(state));
            profile += (name == "minecraft:lava")                                ? '~'
                       : (name == "minecraft:air" || name == "minecraft:cave_air") ? '.'
                                                                                   : '#';
        }
        CHECK(profile == expected.profile);
        const auto nylium = chunk.get_block(3, expected.nylium_y, 5);
        CHECK(blocks->block_name(blocks->block_of(nylium)) == "minecraft:crimson_nylium");
        CHECK(blocks->biome_name(chunk.get_biome(8, 0, 4)) == "minecraft:crimson_forest");
        // Nothing above the noise's own 128 blocks.
        for (i32 y = 128; y < 256; ++y) {
            REQUIRE(chunk.get_block(3, y, 5) == registry::kAirState);
        }
    }
}
