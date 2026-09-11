#include "ov/render/ambient_occlusion.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

namespace {

constexpr AoSample kAir{1.0F, true, 15, 0};
constexpr AoSample kStone{kOccluderShade, false, 0, 0};

}  // namespace

TEST_CASE("an open corner is at full brightness and full light", "[ao]") {
    const auto corner = smooth_corner(kAir, kAir, kAir, kAir);
    CHECK(corner.brightness == Approx(1.0F));
    CHECK(corner.sky_quarters == 60);
    CHECK(corner.block_quarters == 0);
}

TEST_CASE("each occluder takes a fifth of the corner's brightness", "[ao]") {
    // The mean of four shade brightnesses, 0.2 for a full cube: one occluder
    // is 0.8, a side and the diagonal 0.6. The game's own AmbientOcclusionFace
    // prints exactly these on the test platform of the render parity scenes.
    CHECK(smooth_corner(kAir, kStone, kAir, kAir).brightness == Approx(0.8F));
    CHECK(smooth_corner(kAir, kAir, kAir, kStone).brightness == Approx(0.8F));
    CHECK(smooth_corner(kAir, kStone, kAir, kStone).brightness == Approx(0.6F));
}

TEST_CASE("two opaque sides hide the diagonal: an inside corner is 0.4 either way", "[ao]") {
    // Past two opaque sides the diagonal cannot be seen, so it is replaced by
    // a side — and 0.4 whether it is air or stone, the crease of an inside
    // corner. The old four-step table put this at 0.2, half the game's.
    CHECK(smooth_corner(kAir, kStone, kStone, kAir).brightness == Approx(0.4F));
    CHECK(smooth_corner(kAir, kStone, kStone, kStone).brightness == Approx(0.4F));
}

TEST_CASE("a see-through full cube darkens but does not hide", "[ao]") {
    // Glass fills its cube, so its shade brightness is 0.2 like stone's; but
    // sight passes it, so two panes side by side still show the diagonal.
    constexpr AoSample kGlass{kOccluderShade, true, 15, 0};
    CHECK(smooth_corner(kAir, kGlass, kGlass, kAir).brightness == Approx(0.6F));
}

TEST_CASE("light is the sum of four levels, in quarter levels", "[ao]") {
    const AoSample centre{1.0F, true, 12, 3};
    const AoSample side{1.0F, true, 11, 2};
    const AoSample diagonal{1.0F, true, 10, 1};
    const auto     corner = smooth_corner(centre, side, side, diagonal);
    CHECK(corner.sky_quarters == 12 + 11 + 11 + 10);
    CHECK(corner.block_quarters == 3 + 2 + 2 + 1);
}

TEST_CASE("a block with no light lends the centre's, so an occluder darkens once", "[ao]") {
    // A solid block stores no light. Averaged in as zero it would darken the
    // corner a second time, on top of its shade; the game counts the centre's
    // light in its place — both channels, and only when both are zero.
    const AoSample centre{1.0F, true, 12, 4};
    const auto     corner = smooth_corner(centre, kStone, kAir, kAir);
    CHECK(corner.sky_quarters == 12 + 12 + 15 + 15);
    CHECK(corner.block_quarters == 4 + 4 + 0 + 0);

    // Sky 0 with some block light is not "no light": it counts as it is.
    const AoSample torchlit{1.0F, true, 0, 9};
    const auto     lit = smooth_corner(centre, torchlit, kAir, kAir);
    CHECK(lit.sky_quarters == 12 + 0 + 15 + 15);
    CHECK(lit.block_quarters == 4 + 9 + 0 + 0);
}

TEST_CASE("directional shading matches the game's own faces", "[ao]") {
    CHECK(face_shade(Direction::Up) == Approx(1.0F));
    CHECK(face_shade(Direction::Down) == Approx(0.5F));
    CHECK(face_shade(Direction::North) == Approx(0.8F));
    CHECK(face_shade(Direction::South) == Approx(0.8F));
    CHECK(face_shade(Direction::West) == Approx(0.6F));
    CHECK(face_shade(Direction::East) == Approx(0.6F));
}
