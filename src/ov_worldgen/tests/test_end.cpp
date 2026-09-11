// The End's generation: the island noise, the biome rule, the spikes.
//
// The whole-world figures against an End the real 1.20.1 server generated are
// in docs/provenance/end.md (`tools/ov_endparity`); what is here is the part
// that fits in a unit test, and the shape facts the rule is made of.

#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/end.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <set>
#include <string_view>
#include <utility>

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

TEST_CASE("the main island is a cone round the origin", "[worldgen][end]") {
    const EndIslands islands{kSeed};
    // Within 64 of the origin (eighth-scale) no outer island is consulted: the
    // height is 100 - 8 x distance, clamped to [-100, 80].
    CHECK(islands.height_value(0, 0) == 80.0F);
    CHECK(islands.height_value(2, 0) == 80.0F);   // 100 - 16 = 84, clamped
    CHECK(islands.height_value(5, 0) == 60.0F);   // 100 - 40
    CHECK(islands.height_value(0, -13) == -4.0F);
    CHECK(islands.height_value(30, 40) == -100.0F);  // 100 - 400, clamped

    // The density is (height - 8) / 128, read at block / 8 — truncating, so
    // blocks -7..7 all read the origin's column.
    CHECK(islands.density(0, 0) == (80.0 - 8.0) / 128.0);
    CHECK(islands.density(-7, 7) == islands.density(0, 0));
    CHECK(islands.density(40, 0) == (60.0 - 8.0) / 128.0);
    CHECK(islands.density(-40, 0) == (60.0 - 8.0) / 128.0);

    // Bounds: every value lies in the declared range — the router prunes on
    // them (briefing, pitfall 3).
    for (i32 x = -2000; x <= 2000; x += 97) {
        for (i32 z = -2000; z <= 2000; z += 89) {
            const f64 value = islands.density(x * 8, z * 8);
            CHECK(value >= EndIslands::kMinValue);
            CHECK(value <= EndIslands::kMaxValue);
        }
    }
}

TEST_CASE("the End's biome rule has four thresholds", "[worldgen][end]") {
    CHECK(kEndBiomes[end_biome_for_erosion(0.26)] == "minecraft:end_highlands");
    CHECK(kEndBiomes[end_biome_for_erosion(0.25)] == "minecraft:end_midlands");
    CHECK(kEndBiomes[end_biome_for_erosion(-0.0625)] == "minecraft:end_midlands");
    CHECK(kEndBiomes[end_biome_for_erosion(-0.07)] == "minecraft:end_barrens");
    CHECK(kEndBiomes[end_biome_for_erosion(-0.21875)] == "minecraft:end_barrens");
    CHECK(kEndBiomes[end_biome_for_erosion(-0.22)] == "minecraft:small_end_islands");
    CHECK(kEndBiomes[end_biome_for_erosion(-0.84375)] == "minecraft:small_end_islands");
}

TEST_CASE("the End's biome source is a rule, not a table", "[worldgen][end]") {
    if (!data_present()) {
        SKIP("vanilla data absent");
    }
    auto source = BiomeSource::load(reports_root(), "end");
    REQUIRE(source.has_value());
    CHECK(source->is_end_rule());
    CHECK(source->biome_count() == 5);
    // In the source's order: the decorator's tie-break reads it.
    REQUIRE(source->entry_count() == 5);
    CHECK(source->entry_biome(0) == "minecraft:the_end");
    CHECK(source->entry_biome(3) == "minecraft:small_end_islands");

    auto router = NoiseRouter::load(data_root(), "end", kSeed);
    REQUIRE(router.has_value());
    CHECK(router->unavailable().empty());

    // Every cell within 64 chunks of the origin is the_end, whatever the noise.
    CHECK(source->biome_at(source->sample(*router, 0, 10, 0)) == "minecraft:the_end");
    CHECK(source->biome_at(source->sample(*router, 255, 0, 0)) == "minecraft:the_end");  // chunk 63
    CHECK(source->biome_at(source->sample(*router, -260, 0, 0)) != "");
    // One chunk answers once: every cell of a chunk, every y.
    const auto a = source->biome_at(source->sample(*router, 400, 0, 100));
    const auto b = source->biome_at(source->sample(*router, 403, 60, 103));
    CHECK(a == b);

    // Far out, all four island biomes turn up.
    std::set<std::string_view> seen;
    for (i32 qx = -2000; qx <= 2000; qx += 36) {
        for (i32 qz = 400; qz <= 2400; qz += 36) {
            seen.insert(source->biome_at(source->sample(*router, qx, 0, qz)));
        }
    }
    CHECK(seen.contains("minecraft:end_highlands"));
    CHECK(seen.contains("minecraft:end_midlands"));
    CHECK(seen.contains("minecraft:end_barrens"));
    CHECK(seen.contains("minecraft:small_end_islands"));
    CHECK_FALSE(seen.contains("minecraft:the_end"));
}

TEST_CASE("the ten spikes stand on a circle of radius 42", "[worldgen][end]") {
    const auto spikes = end_spikes(kSeed);
    // floor(42 cos), floor(42 sin) of 2(-pi + i pi / 10): the positions are the
    // same in every world; sin(-pi) is -1.2e-16, which floors to -1.
    constexpr std::array<std::pair<i32, i32>, 10> kCentres{
        {{42, 0}, {33, 24}, {12, 39}, {-13, 39}, {-34, 24}, {-42, -1}, {-34, -25}, {-13, -40},
         {12, -40}, {33, -25}}};
    std::set<i32> heights;
    usize         caged = 0;
    for (usize i = 0; i < spikes.size(); ++i) {
        CHECK(spikes[i].centre_x == kCentres[i].first);
        CHECK(spikes[i].centre_z == kCentres[i].second);
        // Index k in the shuffle: radius 2 + k / 3, height 76 + 3 k.
        CHECK((spikes[i].height - 76) % 3 == 0);
        const i32 k = (spikes[i].height - 76) / 3;
        CHECK(spikes[i].radius == 2 + k / 3);
        CHECK(spikes[i].guarded == (k == 1 || k == 2));
        heights.insert(spikes[i].height);
        caged += spikes[i].guarded ? 1 : 0;
    }
    // A permutation: ten different heights from 76 to 103, two cages.
    CHECK(heights.size() == 10);
    CHECK(*heights.begin() == 76);
    CHECK(*heights.rbegin() == 103);
    CHECK(caged == 2);
    // A pure function of the seed.
    CHECK(end_spikes(kSeed)[3].height == spikes[3].height);
}

TEST_CASE("an End chunk is end stone on air, 128 blocks of noise in 256", "[worldgen][end]") {
    if (!data_present()) {
        SKIP("vanilla data absent");
    }
    auto blocks  = registry::BlockRegistry::load(registry_pack());
    auto router  = NoiseRouter::load(data_root(), "end", kSeed);
    auto source  = BiomeSource::load(reports_root(), "end");
    auto surface = SurfaceSystem::load(data_root(), "end", kSeed, *blocks);
    REQUIRE(blocks.has_value());
    REQUIRE(router.has_value());
    REQUIRE(source.has_value());
    REQUIRE(surface.has_value());
    ChunkGenerator generator{*router, *source, *blocks};
    generator.set_surface_system(&*surface);

    world::Chunk chunk{ChunkPos{0, 0}, world::WorldShape::the_end(),
                       world::AirStates::from(*blocks), &*blocks};
    generator.generate(chunk);
    const auto end_stone = blocks->default_state(*blocks->find_block("minecraft:end_stone"));
    usize      stone     = 0;
    for (i32 y = 0; y < 256; ++y) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                const auto state = chunk.get_block(x, y, z);
                if (state == end_stone) {
                    ++stone;
                    CHECK(y < 128);
                } else {
                    CHECK(state == registry::kAirState);
                }
            }
        }
    }
    // The main island is under the origin.
    CHECK(stone > 16 * 16 * 20);
    CHECK(blocks->biome_name(chunk.get_biome(0, 64, 0)) == "minecraft:the_end");
}
