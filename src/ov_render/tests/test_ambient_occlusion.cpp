#include "ov/render/ambient_occlusion.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

// Every brightness below is one the real 1.20.1 client printed for the faces
// of the render parity test platform (scripts/render_parity_scenes.txt, `ao`
// lines; docs/provenance/rendu-parite.md § 3).

namespace {

constexpr AoSample kAir{1.0F, 15, 0};
constexpr AoSample kStone{kOccluderShade, 0, 0};

}  // namespace

TEST_CASE("an open corner is at full brightness and full light", "[ao]") {
    const auto corner = smooth_corner(kAir, kAir, kAir, kAir);
    CHECK(corner.brightness == Approx(1.0F));
    CHECK(corner.sky_quarters == 60);
    CHECK(corner.block_quarters == 0);
}

TEST_CASE("each occluder takes a fifth of the corner's brightness", "[ao]") {
    // The floor beside the lone block: 0.8. Beside the wall's end, a side and
    // the diagonal: 0.6.
    CHECK(smooth_corner(kAir, kStone, kAir, kAir).brightness == Approx(0.8F));
    CHECK(smooth_corner(kAir, kAir, kAir, kStone).brightness == Approx(0.8F));
    CHECK(smooth_corner(kAir, kStone, kAir, kStone).brightness == Approx(0.6F));
}

TEST_CASE("an inside corner between single blocks still sees its diagonal", "[ao]") {
    // The two L's of the platform, their tops open to the sky: 0.4 with the
    // diagonal filled, 0.6 without. The old rule hid the diagonal behind any
    // two opaque sides and gave 0.4 to both.
    CHECK(smooth_corner(kAir, kStone, kStone, kStone).brightness == Approx(0.4F));
    CHECK(smooth_corner(kAir, kStone, kStone, kAir).brightness == Approx(0.6F));
}

TEST_CASE("a corner walled two high stands the opposite side in for its diagonal", "[ao]") {
    // The corner of the deep room, walls five high. With the cell on the far
    // side of the corner open, 0.6 and block light 10.25 although all three
    // neighbours are deepslate; with a block put there, 0.4 and 10.00.
    const AoSample centre{1.0F, 0, 10};
    const AoSample open_far{1.0F, 0, 11};
    const auto     open = smooth_corner(centre, kStone, kStone, kStone, &open_far);
    CHECK(open.brightness == Approx(0.6F));
    CHECK(open.block_quarters == 41);

    const auto closed = smooth_corner(centre, kStone, kStone, kStone, &kStone);
    CHECK(closed.brightness == Approx(0.4F));
    CHECK(closed.block_quarters == 40);
}

TEST_CASE("light is the sum of four levels, in quarter levels", "[ao]") {
    const AoSample centre{1.0F, 12, 3};
    const AoSample side{1.0F, 11, 2};
    const AoSample diagonal{1.0F, 10, 1};
    const auto     corner = smooth_corner(centre, side, side, diagonal);
    CHECK(corner.sky_quarters == 12 + 11 + 11 + 10);
    CHECK(corner.block_quarters == 3 + 2 + 2 + 1);
}

TEST_CASE("a block with no light lends the centre's, so an occluder darkens once", "[ao]") {
    // The room: centre at block light 10, the walls at none. The game prints
    // 10.25 for the corner by the wall (one side 11 beyond it) — sides that
    // carry no light count as the centre.
    const AoSample centre{1.0F, 0, 10};
    const AoSample open_side{1.0F, 0, 11};
    const auto     corner = smooth_corner(centre, kStone, open_side, kStone);
    CHECK(corner.block_quarters == 10 + 10 + 11 + 10);
    CHECK(corner.sky_quarters == 0);

    // Sky 0 with some block light is not "no light": it counts as it is.
    const AoSample torchlit{1.0F, 0, 9};
    const auto     lit = smooth_corner(AoSample{1.0F, 12, 4}, torchlit, kAir, kAir);
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
