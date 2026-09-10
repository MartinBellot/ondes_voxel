// The aquifer's invariants — the parts that are facts rather than degrees of
// agreement.
//
// What the aquifer is *worth* is not settled here and cannot be: the oracle is
// the game's own blocks inside its own carving masks, and `ov_parity --aquifer`
// reads it (docs/provenance/aquiferes.md § 10). What a unit test can hold is
// the shape of the thing: the global rule, the cell a centre may land in, the
// two answers that never depend on the noise, and — the one generation leans on
// for multi-threading — that the answer does not depend on the order the
// questions are asked in.

#include "ov/registry/block_states.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/aquifer.hpp"
#include "ov/worldgen/chunk_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <vector>

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

TEST_CASE("the aquifer is built from the overworld router", "[worldgen][aquifer]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    CHECK(router->seed() == kSeed);

    const Aquifer aquifer{*router, router->seed()};
    CHECK(aquifer.enabled());
    CHECK(aquifer.missing().empty());
    CHECK(aquifer.sea_level() == 63);
    CHECK(aquifer.dry_level() == -65);
}

TEST_CASE("the global fluid rule", "[worldgen][aquifer]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    const Aquifer aquifer{*router, kSeed};

    // Below min(-54, sea level): lava, whose surface is at -54.
    CHECK(aquifer.global_fluid(-55) == FluidStatus{-54, AquiferFluid::Lava});
    CHECK(aquifer.global_fluid(-64) == FluidStatus{-54, AquiferFluid::Lava});
    // Otherwise the default fluid up to the sea level.
    CHECK(aquifer.global_fluid(-54) == FluidStatus{63, AquiferFluid::Water});
    CHECK(aquifer.global_fluid(0) == FluidStatus{63, AquiferFluid::Water});
    // The level is exclusive: water at 62, air at 63.
    CHECK(aquifer.global_fluid(62).at(62) == AquiferFluid::Water);
    CHECK(aquifer.global_fluid(63).at(63) == AquiferFluid::None);
    CHECK(aquifer.global_fluid(200).at(200) == AquiferFluid::None);
}

TEST_CASE("a centre stays inside its cell", "[worldgen][aquifer]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    const Aquifer aquifer{*router, kSeed};

    // 0-9 on x and z, 0-8 on y, inside a 16 x 12 x 16 cell. And every offset
    // actually occurs: a centre stuck at one corner would pass the bound.
    std::array<bool, 10> seen_x{};
    std::array<bool, 9>  seen_y{};
    for (i32 gx = -6; gx <= 6; ++gx) {
        for (i32 gy = -6; gy <= 6; ++gy) {
            for (i32 gz = -6; gz <= 6; ++gz) {
                const auto c = aquifer.centre(gx, gy, gz);
                REQUIRE(c[0] >= gx * 16);
                REQUIRE(c[0] <= gx * 16 + 9);
                REQUIRE(c[1] >= gy * 12);
                REQUIRE(c[1] <= gy * 12 + 8);
                REQUIRE(c[2] >= gz * 16);
                REQUIRE(c[2] <= gz * 16 + 9);
                seen_x[static_cast<usize>(c[0] - gx * 16)] = true;
                seen_y[static_cast<usize>(c[1] - gy * 12)] = true;
            }
        }
    }
    for (const bool seen : seen_x) {
        CHECK(seen);
    }
    for (const bool seen : seen_y) {
        CHECK(seen);
    }
}

TEST_CASE("a different positional name moves the centres", "[worldgen][aquifer]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    const Aquifer right{*router, kSeed};
    AquiferTuning witness_tuning;
    witness_tuning.random_name = "minecraft:witness";
    const Aquifer witness{*router, kSeed, witness_tuning};

    // The witness `ov_parity --aquifer` runs has to be a real witness: if the
    // name did not reach the factory, both would agree and prove nothing.
    usize differing = 0;
    for (i32 g = -20; g < 20; ++g) {
        if (right.centre(g, 1, -g) != witness.centre(g, 1, -g)) {
            ++differing;
        }
    }
    CHECK(differing > 30);
}

TEST_CASE("the two answers that never depend on the noise", "[worldgen][aquifer]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    const Aquifer  aquifer{*router, kSeed};
    AquiferSampler sampler{aquifer};

    for (i32 x = -40; x <= 40; x += 13) {
        for (i32 z = -40; z <= 40; z += 17) {
            // Positive density is solid, at any height.
            CHECK(sampler.compute(x, 10, z, 0.25).substance == Substance::Solid);
            CHECK(sampler.compute(x, 150, z, 1e-9).substance == Substance::Solid);
            // The lava layer exists regardless of aquifers, and is never woken.
            const auto deep = sampler.compute(x, -60, z, -1.0);
            CHECK(deep.substance == Substance::Lava);
            CHECK_FALSE(deep.schedule);
        }
    }
}

TEST_CASE("the answer does not depend on the order of the questions", "[worldgen][aquifer]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    const Aquifer aquifer{*router, kSeed};

    // The sampler memoises statuses and surfaces. If a memoised value ever
    // depended on what was asked before it, two workers walking chunks in
    // different orders would build different worlds — which is exactly what
    // ov_gendet checks end to end, and this checks at the source.
    std::vector<std::array<i32, 3>> positions;
    for (i32 y = -50; y <= 90; y += 7) {
        for (i32 x = 100; x < 132; x += 5) {
            for (i32 z = -300; z < -268; z += 6) {
                positions.push_back({x, y, z});
            }
        }
    }
    AquiferSampler             forward{aquifer};
    std::vector<AquiferAnswer> forward_answers;
    forward_answers.reserve(positions.size());
    for (const auto& p : positions) {
        forward_answers.push_back(forward.compute(p[0], p[1], p[2], -0.05));
    }
    AquiferSampler backward{aquifer};
    usize          fluids = 0;
    for (usize i = positions.size(); i-- > 0;) {
        const auto& p      = positions[i];
        const auto  answer = backward.compute(p[0], p[1], p[2], -0.05);
        REQUIRE(answer.substance == forward_answers[i].substance);
        REQUIRE(answer.schedule == forward_answers[i].schedule);
        if (answer.substance == Substance::Water || answer.substance == Substance::Lava) {
            ++fluids;
        }
    }
    // A sample with no fluid in it would pass trivially.
    CHECK(fluids > 0);
}

TEST_CASE("the wake-up rule follows the gap between the two nearest centres",
          "[worldgen][aquifer]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    const Aquifer shipped{*router, kSeed};
    AquiferTuning never_tuning;
    never_tuning.schedule_gap = 0;
    const Aquifer never{*router, kSeed, never_tuning};

    AquiferSampler shipped_sampler{shipped};
    AquiferSampler never_sampler{never};
    usize          woken       = 0;
    usize          fluids      = 0;
    usize          disagreeing = 0;
    for (i32 y = -50; y <= 60; y += 3) {
        for (i32 x = -64; x < 64; x += 5) {
            for (i32 z = -64; z < 64; z += 7) {
                const auto a = shipped_sampler.compute(x, y, z, -0.05);
                const auto b = never_sampler.compute(x, y, z, -0.05);
                // The gap only decides the wake-up, never the block.
                REQUIRE(a.substance == b.substance);
                const bool fluid =
                    a.substance == Substance::Water || a.substance == Substance::Lava;
                fluids += fluid ? 1 : 0;
                woken += a.schedule ? 1 : 0;
                // Air and stone are never woken.
                if (!fluid) {
                    REQUIRE_FALSE(a.schedule);
                }
                // With a gap of zero only the water over the lava layer is.
                if (b.schedule) {
                    REQUIRE(y == -54);
                }
                disagreeing += a.schedule != b.schedule ? 1 : 0;
            }
        }
    }
    CHECK(fluids > 0);
    CHECK(woken > 0);
    CHECK(woken < fluids);
    CHECK(disagreeing > 0);
}

TEST_CASE("the generator asks the aquifer, and gives the same chunk twice",
          "[worldgen][aquifer]") {
    if (!data_present()) {
        SKIP("vanilla data absent; run tools/ov_datagen first");
    }
    auto blocks = registry::BlockRegistry::load(registry_pack());
    REQUIRE(blocks.has_value());
    auto router = NoiseRouter::load(data_root(), "overworld", kSeed);
    REQUIRE(router.has_value());
    auto biomes = BiomeSource::load(reports_root(), "overworld");
    REQUIRE(biomes.has_value());

    ChunkGenerator generator{*router, *biomes, *blocks};
    REQUIRE(generator.aquifer() != nullptr);
    CHECK(generator.aquifer_active());

    const auto             air   = world::AirStates::from(*blocks);
    const auto             shape = world::WorldShape::overworld();
    const ov::ChunkPos     position{7, -3};
    world::Chunk           first{position, shape, air, &*blocks};
    world::Chunk           second{position, shape, air, &*blocks};
    std::vector<BlockPos>  updates_first;
    std::vector<BlockPos>  updates_second;
    generator.generate_noise(first, &updates_first);
    generator.generate_noise(second, &updates_second);

    world::Chunk without{position, shape, air, &*blocks};
    generator.set_aquifer_enabled(false);
    CHECK_FALSE(generator.aquifer_active());
    generator.generate_noise(without);

    usize differing_from_global = 0;
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                REQUIRE(first.get_block(x, y, z) == second.get_block(x, y, z));
                if (first.get_block(x, y, z) != without.get_block(x, y, z)) {
                    ++differing_from_global;
                }
            }
        }
    }
    CHECK(updates_first.size() == updates_second.size());
    // Switching the aquifer off has to change something, or the switch — and
    // every before/after measured with it — is a no-op.
    CHECK(differing_from_global > 0);
}
