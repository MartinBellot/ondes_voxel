#include "ov/render/ambient_occlusion.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::render;
using Catch::Approx;

TEST_CASE("an open corner is not occluded at all", "[ao]") {
    STATIC_REQUIRE(ao_level(CornerNeighbours{}) == 3);
}

TEST_CASE("occluders are counted", "[ao]") {
    STATIC_REQUIRE(ao_level(CornerNeighbours{.side1 = true}) == 2);
    STATIC_REQUIRE(ao_level(CornerNeighbours{.corner = true}) == 2);
    STATIC_REQUIRE(ao_level(CornerNeighbours{.side1 = true, .corner = true}) == 1);
}

TEST_CASE("two solid sides darken the corner fully, whatever the diagonal does", "[ao]") {
    // The one rule that is not simple counting, and the one that decides
    // whether an inside corner has a crease or looks inflated: past two solid
    // sides the diagonal block cannot be seen at all, so it changes nothing.
    STATIC_REQUIRE(ao_level(CornerNeighbours{.side1 = true, .side2 = true}) == 0);
    STATIC_REQUIRE(ao_level(CornerNeighbours{.side1 = true, .side2 = true, .corner = true}) == 0);
}

TEST_CASE("the brightness steps end at full", "[ao]") {
    CHECK(ao_brightness(3) == Approx(1.0F));
    CHECK(ao_brightness(0) == Approx(0.2F));
    CHECK(ao_brightness(1) < ao_brightness(2));
    CHECK(ao_brightness(2) < ao_brightness(3));
}

TEST_CASE("smooth light averages the neighbours that can carry light", "[ao]") {
    const CornerLight light{.self = 12, .side1 = 12, .side2 = 12, .corner = 12};
    CHECK(smooth_light(light, CornerNeighbours{}) == 12);
}

TEST_CASE("a solid neighbour is skipped rather than averaged in as darkness", "[ao]") {
    // A solid block has light 0. Counting it would darken the vertex twice —
    // once through ambient occlusion and once through the average — and the
    // result is the black seam under every overhang.
    const CornerLight light{.self = 12, .side1 = 0, .side2 = 12, .corner = 12};

    CHECK(smooth_light(light, CornerNeighbours{}) == 9);
    CHECK(smooth_light(light, CornerNeighbours{.side1 = true}) == 12);
}

TEST_CASE("the corner is skipped when both sides are solid, as in ao_level", "[ao]") {
    // The two functions have to agree about what is visible, or a vertex is
    // fully occluded and brightly lit at the same time.
    const CornerLight      light{.self = 12, .side1 = 0, .side2 = 0, .corner = 15};
    const CornerNeighbours closed{.side1 = true, .side2 = true};

    CHECK(ao_level(closed) == 0);
    CHECK(smooth_light(light, closed) == 12);
}

TEST_CASE("directional shading matches the game's own faces", "[ao]") {
    CHECK(face_shade(Direction::Up) == Approx(1.0F));
    CHECK(face_shade(Direction::Down) == Approx(0.5F));
    CHECK(face_shade(Direction::North) == Approx(face_shade(Direction::South)));
    CHECK(face_shade(Direction::West) == Approx(face_shade(Direction::East)));
    CHECK(face_shade(Direction::North) > face_shade(Direction::East));
}
