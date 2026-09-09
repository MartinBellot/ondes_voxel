// The density function graph, loaded from the real vanilla data.
//
// The values cannot be checked here — the oracle for those is the terrain the
// game itself writes for the same seed. What is checked is that the *whole
// vanilla router loads*, which is a stronger statement than it sounds: it means
// every node type the overworld uses is implemented, every reference resolves,
// every noise is found, and nothing in the graph is circular. A missing node
// type is refused loudly rather than treated as zero, so this test failing is
// how an unimplemented term is discovered instead of shipping terrain that
// looks plausible.

#include "ov/worldgen/density.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>

using namespace ov;
using namespace ov::worldgen;

namespace {

[[nodiscard]] std::filesystem::path data_root() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "generated" /
           "data" / "minecraft";
}

}  // namespace

TEST_CASE("the overworld router loads whole", "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent; run tools/ov_datagen first");
    }

    auto router = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(router.has_value());

    // The world's shape, straight from the settings rather than assumed.
    CHECK(router->sea_level() == 63);
    CHECK(router->min_y() == -64);
    CHECK(router->height() == 384);
    CHECK(router->cell_width() == 4);
    CHECK(router->cell_height() == 8);

    // The six climate functions are the ones the biome source reads. Every one
    // of them has to be present, or a biome would be chosen from a missing
    // dimension.
    // The whole router, all fifteen. This used to assert that exactly one
    // entry was missing — final_density, which reaches old_blended_noise — and
    // that line failed the day the noise landed, which is what it was for.
    for (const auto* name :
         {"temperature", "vegetation", "continents", "erosion", "depth", "ridges",
          "final_density", "initial_density_without_jaggedness", "barrier", "lava", "vein_toggle",
          "vein_ridged", "vein_gap", "fluid_level_floodedness", "fluid_level_spread"}) {
        CAPTURE(name);
        CHECK(router->entry(name) != nullptr);
    }
    CHECK(router->unavailable().empty());
}

TEST_CASE("the terrain density has a shape", "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(router.has_value());
    const DensityFunction* density = router->entry("final_density");
    REQUIRE(density != nullptr);

    // Deep underground is solid and high in the air is not. That is the one
    // thing terrain density means, and a graph that built without error could
    // still be uniformly zero.
    CHECK(density->compute({0, -60, 0}) > 0.0);
    CHECK(density->compute({0, 300, 0}) < 0.0);

    // And it is not a step function of y alone: two columns far apart differ
    // at the same height, or the world would be flat.
    bool varies = false;
    const f64 reference = density->compute({0, 80, 0});
    for (i32 i = 1; i <= 40 && !varies; ++i) {
        varies = density->compute({i * 271, 80, i * 397}) != reference;
    }
    CHECK(varies);
}

TEST_CASE("climate varies with position and repeats with the seed",
          "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent");
    }

    auto first = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(first.has_value());
    const DensityFunction* continents = first->entry("continents");
    REQUIRE(continents != nullptr);

    // Deterministic.
    CHECK(continents->compute({100, 0, 200}) == continents->compute({100, 0, 200}));

    // Not constant: a climate that did not vary would put one biome everywhere,
    // and the failure would look like a working generator.
    bool varies = false;
    const f64 reference = continents->compute({0, 0, 0});
    for (i32 i = 1; i <= 40 && !varies; ++i) {
        varies = continents->compute({i * 512, 0, i * 512}) != reference;
    }
    CHECK(varies);

    // The same seed gives the same world.
    auto again = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(again.has_value());
    CHECK(again->entry("continents")->compute({777, 0, -333}) ==
          continents->compute({777, 0, -333}));

    // A different seed does not.
    auto other = NoiseRouter::load(data_root(), "overworld", 987654321);
    REQUIRE(other.has_value());
    CHECK(other->entry("continents")->compute({777, 0, -333}) !=
          continents->compute({777, 0, -333}));
}

TEST_CASE("flat_cache quantises to quarters", "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", 42);
    REQUIRE(router.has_value());
    const DensityFunction* continents = router->entry("continents");
    REQUIRE(continents != nullptr);

    // Continentalness goes through a flat_cache, so it is constant inside each
    // four-by-four column and steps between them. Treating that cache as an
    // identity gives a smooth field where the game has a stepped one, and every
    // biome boundary moves — so this is worth asserting rather than assuming.
    const f64 base = continents->compute({16, 0, 16});
    CHECK(continents->compute({17, 0, 16}) == base);
    CHECK(continents->compute({16, 0, 19}) == base);
    CHECK(continents->compute({19, 0, 19}) == base);
    CHECK(continents->compute({20, 0, 16}) != base);

    // And negative coordinates floor rather than truncate, or the world would
    // be asymmetric about the origin. -4 and -1 land in the same cell — an
    // arithmetic shift of -1 by two is -1, not 0 — while -5 lands in the one
    // below. Getting this wrong makes the world's negative half a cell out of
    // step with its positive half.
    const f64 negative = continents->compute({-4, 0, -4});
    CHECK(continents->compute({-1, 0, -1}) == negative);
    CHECK(continents->compute({-3, 0, -3}) == negative);
    CHECK(continents->compute({-5, 0, -5}) != negative);
}

TEST_CASE("every node's bounds contain the values it produces", "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(router.has_value());

    // The bounds are not documentation. `min` and `max` skip their second
    // argument when the first is already outside the other's range —
    // `a < b->min_value() ? a : min(a, b)` — so a bound that is too tight makes
    // those nodes return the *wrong number*, quietly, for the positions where
    // the skipped branch would have won. That failure looks like terrain with
    // holes rather than like a crash, which is why it is worth a test.
    for (const auto* name :
         {"temperature", "vegetation", "continents", "erosion", "depth", "ridges",
          "final_density", "initial_density_without_jaggedness", "barrier", "lava", "vein_toggle",
          "vein_ridged", "vein_gap", "fluid_level_floodedness", "fluid_level_spread"}) {
        const DensityFunction* entry = router->entry(name);
        REQUIRE(entry != nullptr);
        const f64 low  = entry->min_value();
        const f64 high = entry->max_value();
        CAPTURE(name, low, high);
        CHECK(low <= high);
        for (i32 x = -768; x <= 768; x += 384) {
            for (i32 z = -768; z <= 768; z += 384) {
                for (i32 y = -60; y <= 300; y += 60) {
                    const f64 value = entry->compute({x, y, z});
                    CAPTURE(x, y, z, value);
                    CHECK(value >= low);
                    CHECK(value <= high);
                }
            }
        }
    }
}

TEST_CASE("the old blended noise is centred on zero", "[worldgen][density]") {
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(router.has_value());
    const DensityFunction* base = router->function("minecraft:overworld/base_3d_noise");
    REQUIRE(base != nullptr);

    // Three stacks of octaves, two of which are a floor and a ceiling and the
    // third a selector between them. The two limits are drawn the same way, so
    // whatever the selector does the result is symmetric about zero and the
    // mean over a wide grid has to be small.
    //
    // This guards the octave stack rather than the terrain: a count or a
    // weighting that slipped would move this mean, and the surface it produces
    // would sit a block or two from the game's without anything else looking
    // wrong. The tolerance is loose on purpose — a regression guard, not a
    // fitted number.
    f64   sum   = 0.0;
    usize count = 0;
    for (i32 x = -1024; x <= 1024; x += 97) {
        for (i32 z = -1024; z <= 1024; z += 89) {
            for (i32 y = 0; y <= 128; y += 32) {
                sum += base->compute({x, y, z});
                ++count;
            }
        }
    }
    REQUIRE(count > 500);
    const f64 mean = sum / static_cast<f64>(count);
    CAPTURE(mean, count);
    CHECK(std::abs(mean) < 0.05);
}

TEST_CASE("the aquifer's fluid level lattice is what the reference world shows",
          "[worldgen][density]") {
    // The aquifer itself is not implemented — the barrier half has no source
    // this project may read, and it is refused rather than guessed. One half of
    // it *is* measured, and this fences the input that measurement rests on.
    //
    // Every fluid level read out of `run/reference-1234567890` sits on
    // `40k + 20 + 3j` for the y band `k = floor(y / 40)`, and the offset `3j`
    // follows `3 * floor(3.2 * spread)` where `spread` is the router's
    // `fluid_level_spread` sampled **at the aquifer grid index**, not at a
    // block position: 142 of 159 cells, against a 32,7 % base rate, while the
    // same noise read at the cell's block centre or block corner, and the other
    // three aquifer noises read at the grid index, all sit at that base rate.
    // See docs/provenance/aquiferes.md.
    //
    // What is checked here is only what the code can check without the
    // reference world: that the entry exists, that it is the small-amplitude
    // field the arithmetic assumes, and that reading it at a grid index rather
    // than a block position is a different question — which is the whole point
    // of the result and the one thing a refactor could silently break.
    if (!std::filesystem::is_directory(data_root() / "worldgen")) {
        SKIP("vanilla worldgen data absent; run tools/ov_datagen first");
    }
    auto router = NoiseRouter::load(data_root(), "overworld", 1234567890);
    REQUIRE(router.has_value());

    const auto* spread = router->entry("fluid_level_spread");
    REQUIRE(spread != nullptr);

    // `3 * floor(3.2 * spread)` only produces the observed offsets — which run
    // from -12 to +6 — if the field stays inside about +/- 1. A field an order
    // of magnitude larger would put the level outside its own cell.
    f64   largest = 0.0;
    usize distinct_from_block_sample = 0;
    usize compared                   = 0;
    for (i32 gx = -60; gx <= 60; ++gx) {
        for (i32 gz = -60; gz <= 60; ++gz) {
            const f64 at_index = spread->compute({gx, 0, gz});
            largest            = std::max(largest, std::abs(at_index));
            // The same cell, addressed in blocks. If these ever agreed, the
            // measurement that separated them would have been reading noise.
            const f64 at_block = spread->compute({gx * 16 + 8, 20, gz * 16 + 8});
            distinct_from_block_sample += at_index != at_block ? 1 : 0;
            ++compared;
        }
    }
    CAPTURE(largest, compared);
    CHECK(largest < 1.5);
    CHECK(distinct_from_block_sample == compared);
}
