#include "ov/render/position_random.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace ov;
using namespace ov::render;

// The expected values come from the JDK: java.util.Random for the generator,
// and the position hash under test, run once in a standalone program
// (.scratch/seedvec/SeedVectors.java at the time of writing). They check the
// C++ reproduces the JDK's arithmetic bit for bit — wrapping, sign, shift. That
// the hash is the game's own is checked separately, against its captures.

TEST_CASE("the position seed wraps as the JDK's arithmetic does", "[position_random]") {
    CHECK(position_seed(0, 0, 0) == 0);
    CHECK(position_seed(1, 64, -1) == 51075559105168LL);
    CHECK(position_seed(48074, 90, 24) == 73931932241280LL);
    CHECK(position_seed(-123456, -40, 987654) == -24984754662129LL);
    CHECK(position_seed(7, 300, 7) == -541718005453LL);
}

TEST_CASE("the weighted pick is the JDK generator's", "[position_random]") {
    constexpr std::array<i32, 4> kFour{1, 1, 1, 1};
    constexpr std::array<i32, 3> kWeighted{3, 1, 6};

    CHECK(pick_weighted(position_seed(0, 0, 0), kFour) == 0);
    CHECK(pick_weighted(position_seed(1, 64, -1), kFour) == 2);
    CHECK(pick_weighted(position_seed(48074, 90, 24), kFour) == 0);
    CHECK(pick_weighted(position_seed(-123456, -40, 987654), kFour) == 2);
    CHECK(pick_weighted(position_seed(7, 300, 7), kFour) == 2);

    CHECK(pick_weighted(position_seed(1, 64, -1), kWeighted) == 2);
    CHECK(pick_weighted(position_seed(48074, 90, 24), kWeighted) == 2);
    CHECK(pick_weighted(position_seed(-123456, -40, 987654), kWeighted) == 0);
}

TEST_CASE("a plant's offset is the game's, to the printed digit", "[position_random]") {
    // BlockState.getOffset of minecraft:grass, printed by the oracle's
    // `variants` directive over the plains (22 of 22 reproduced; three here).
    const auto check = [](i32 x, i32 z, f32 ex, f32 ey, f32 ez) {
        const Vec3f o = block_offset(x, z, OffsetType::XYZ, 0.25F);
        CHECK(o.x == Catch::Approx(ex).margin(1e-6));
        CHECK(o.y == Catch::Approx(ey).margin(1e-6));
        CHECK(o.z == Catch::Approx(ez).margin(1e-6));
    };
    check(48048, 23, 0.083333343F, -0.160000002F, 0.25F);
    check(48049, 20, -0.149999999F, -0.026666665F, -0.016666666F);
    check(48049, 25, 0.216666669F, -0.013333333F, -0.18333333F);
    CHECK(block_offset(48049, 25, OffsetType::XZ, 0.25F).y == 0.0F);
    CHECK(block_offset(48049, 25, OffsetType::None, 0.25F).x == 0.0F);
}

TEST_CASE("an empty or single list always gives the first", "[position_random]") {
    CHECK(pick_weighted(12345, std::span<const i32>{}) == 0);
    constexpr std::array<i32, 1> kOne{5};
    CHECK(pick_weighted(12345, kOne) == 0);
}
