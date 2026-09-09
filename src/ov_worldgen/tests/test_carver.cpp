// What can be checked about a carver without a reference world.
//
// The thing that actually matters — whether our caves are the game's caves —
// has one oracle and it is a locally generated world, which no unit test may
// depend on. `tools/ov_carveparity` asks that question and its answer is
// written down in docs/provenance/carvers.md.
//
// What is left here is everything that would still be broken if the caves
// happened to look right: the trigonometry table the angles come out of, the
// bit layout the mask reaches disk in, and the two structural properties that
// catch the mistakes an implementation actually makes — carving a chunk twice
// must give the same mask, and a chunk must be carved by its neighbours and not
// only by itself.

#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/carving_mask.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace ov;
using namespace ov::worldgen;
using Catch::Approx;

TEST_CASE("the mask packs cells the way a region file stores them", "[worldgen][carver]") {
    CarvingMask mask{-64, 384};
    CHECK(mask.count() == 0);
    CHECK(mask.to_long_array().empty());

    // index = x | z << 4 | (y - min_y) << 8, low bit of a word first.
    mask.set(1, -64, 0);
    const auto words = mask.to_long_array();
    REQUIRE(words.size() == 1);
    CHECK(static_cast<u64>(words[0]) == 0x2ULL);

    mask.set(0, -63, 0);
    CHECK(mask.get(0, -63, 0));
    CHECK_FALSE(mask.get(0, -62, 0));
    // (y - min_y) << 8 puts the second cell 256 bits along, in word 4.
    const auto grown = mask.to_long_array();
    REQUIRE(grown.size() == 5);
    CHECK(static_cast<u64>(grown[4]) == 0x1ULL);

    CHECK(mask.count() == 2);
}

TEST_CASE("a mask survives the round trip through a long array", "[worldgen][carver]") {
    CarvingMask mask{-64, 384};
    mask.set(3, 12, 9);
    mask.set(15, -20, 15);
    mask.set(0, 200, 7);
    const auto words    = mask.to_long_array();
    const auto restored = CarvingMask::from_long_array(words, -64, 384);
    CHECK(restored.count() == mask.count());
    CHECK(restored.get(3, 12, 9));
    CHECK(restored.get(15, -20, 15));
    CHECK(restored.get(0, 200, 7));
    CHECK_FALSE(restored.get(3, 13, 9));
}

TEST_CASE("out-of-range heights are outside the mask, not an error", "[worldgen][carver]") {
    CarvingMask mask{-64, 384};
    mask.set(0, -65, 0);
    mask.set(0, 320, 0);
    CHECK(mask.count() == 0);
    CHECK_FALSE(mask.get(0, -65, 0));
    CHECK_FALSE(mask.get(0, 320, 0));
}

TEST_CASE("the angles come from the game's table, not from libm", "[worldgen][carver]") {
    // The table has 65536 steps, so it agrees with the real sine to about
    // 1e-4 and no further. Both facts matter: close enough that a wrong
    // implementation looks right, far enough that it is not.
    f32 worst = 0.0F;
    for (int i = 0; i < 2000; ++i) {
        const auto angle = static_cast<f32>(i) * 0.0031F;
        worst            = std::max(worst, std::abs(mth_sin(angle) - std::sin(angle)));
    }
    CHECK(worst > 1.0e-6F);
    CHECK(worst < 1.0e-4F);

    // The quarter-turn index is exact, which is what makes a carver room's
    // radius exactly 1.5 + its thickness.
    constexpr f32 half_pi = 3.14159265358979323846F / 2.0F;
    CHECK(mth_sin(half_pi) == 1.0F);
    CHECK(mth_sin(0.0F) == 0.0F);
    CHECK(mth_cos(0.0F) == 1.0F);

    // Negative angles wrap rather than trap: the index is masked, not clamped.
    CHECK(mth_sin(-half_pi) == Approx(-1.0).margin(1.0e-4));
}

TEST_CASE("large feature seeding depends on both draws and both coordinates",
          "[worldgen][carver]") {
    // Two longs are taken from the base seed before the chunk coordinates are
    // mixed in. Dropping either one still gives a per-chunk seed, so the only
    // thing that catches it is a reference; what a test can catch is the
    // weaker property that no coordinate is ignored.
    auto at = [](i64 seed, i32 x, i32 z) {
        auto random = large_feature_random(seed, x, z);
        return random.next_long();
    };
    CHECK(at(1234567890, 0, 0) != at(1234567890, 1, 0));
    CHECK(at(1234567890, 0, 0) != at(1234567890, 0, 1));
    CHECK(at(1234567890, 3, 5) != at(1234567891, 3, 5));
    CHECK(at(1234567890, 3, 5) == at(1234567890, 3, 5));

    // x and z are multiplied by different draws, so swapping them is not a
    // symmetry. A single-draw version would make it one.
    CHECK(at(1234567890, 3, 5) != at(1234567890, 5, 3));
}

TEST_CASE("carving a chunk is a pure function of seed and position", "[worldgen][carver]") {
    const CarvingContext context{-64, 384};
    const CarverStage    stage{1234567890, context};

    const auto first  = stage.carve(12, -7);
    const auto second = stage.carve(12, -7);
    CHECK(first.count() == second.count());
    CHECK(first.to_long_array() == second.to_long_array());

    const CarverStage other{1234567891, context};
    // A different seed gives a different world. Not merely a different count —
    // the same count with different cells would also be a pass here, which is
    // why the array is compared and not the total.
    CHECK(other.carve(12, -7).to_long_array() != first.to_long_array());
}

TEST_CASE("a chunk is carved by its neighbours, not only by itself", "[worldgen][carver]") {
    // The whole reason the driver sweeps 17x17 chunks: a tunnel runs up to 112
    // blocks, so most of what is carved out of a chunk started somewhere else.
    // If the neighbourhood loop were dropped, chunks would still get caves and
    // every one of them would stop dead at the chunk border.
    const CarvingContext context{-64, 384};
    const CarverStage    stage{1234567890, context};

    // Take a run of chunks and count how many have any carved cell at all.
    // With only self-starts, at most 22 % of chunks could be non-empty
    // (0.15 + 0.07 + 0.01 probabilities, minus the ones whose nested draw
    // yields no origin). With the neighbourhood, nearly all of them are.
    int non_empty = 0;
    for (i32 x = 0; x < 8; ++x) {
        for (i32 z = 0; z < 8; ++z) {
            if (!stage.carve(x, z).empty()) {
                ++non_empty;
            }
        }
    }
    CHECK(non_empty > 40);
}

TEST_CASE("carved cells stay inside the heights the carvers are allowed", "[worldgen][carver]") {
    // The lowest row of an ellipsoid's bounding box is never carved, so
    // nothing may land on y = min_y or y = min_y + 1; and the top eight blocks
    // of the world are left alone.
    const CarvingContext context{-64, 384};
    const CarverStage    stage{1234567890, context};

    i32 lowest  = 1000;
    i32 highest = -1000;
    for (i32 x = 0; x < 4; ++x) {
        for (i32 z = 0; z < 4; ++z) {
            const auto mask = stage.carve(x, z);
            for (i32 local_x = 0; local_x < 16; ++local_x) {
                for (i32 local_z = 0; local_z < 16; ++local_z) {
                    for (i32 y = -64; y < 320; ++y) {
                        if (mask.get(local_x, y, local_z)) {
                            lowest  = std::min(lowest, y);
                            highest = std::max(highest, y);
                        }
                    }
                }
            }
        }
    }
    REQUIRE(lowest < 1000);
    CHECK(lowest >= -62);
    CHECK(highest <= 312);
}
