#include "ov/math/vec.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

using namespace ov;
using Catch::Approx;

TEST_CASE("Vec3 is a trivial aggregate", "[math][vec]") {
    // Vectors sit inside entity components and vertex data. They must stay
    // memcpy-able and free of hidden padding.
    STATIC_REQUIRE(std::is_trivially_copyable_v<Vec3d>);
    STATIC_REQUIRE(sizeof(Vec3f) == 3 * sizeof(f32));
    STATIC_REQUIRE(sizeof(Vec3i) == 3 * sizeof(i32));
}

TEST_CASE("Vec3 arithmetic", "[math][vec]") {
    constexpr Vec3d a{1.0, 2.0, 3.0};
    constexpr Vec3d b{4.0, 5.0, 6.0};

    STATIC_REQUIRE(a + b == Vec3d{5.0, 7.0, 9.0});
    STATIC_REQUIRE(b - a == Vec3d{3.0, 3.0, 3.0});
    STATIC_REQUIRE(a * 2.0 == Vec3d{2.0, 4.0, 6.0});
    STATIC_REQUIRE(-a == Vec3d{-1.0, -2.0, -3.0});
    STATIC_REQUIRE(a.dot(b) == 32.0);
    STATIC_REQUIRE(a.cross(b) == Vec3d{-3.0, 6.0, -3.0});

    Vec3d c = a;
    c += b;
    REQUIRE(c == Vec3d{5.0, 7.0, 9.0});
    c -= b;
    REQUIRE(c == a);
    c *= 3.0;
    REQUIRE(c == Vec3d{3.0, 6.0, 9.0});
}

TEST_CASE("length and normalization", "[math][vec]") {
    constexpr Vec3d v{3.0, 4.0, 0.0};
    STATIC_REQUIRE(v.length_squared() == 25.0);
    REQUIRE(v.length() == Approx(5.0));

    const Vec3d n = v.normalized();
    REQUIRE(n.length() == Approx(1.0));
    REQUIRE(n.x == Approx(0.6));
    REQUIRE(n.y == Approx(0.8));

    // Normalizing a zero vector must not produce NaN: entity velocity is zero
    // most of the time, and a NaN there propagates into the position and
    // eventually into a saved chunk.
    const Vec3d zero = Vec3d{}.normalized();
    REQUIRE(zero == Vec3d{0.0, 0.0, 0.0});
}

TEST_CASE("Direction numbering matches the wire protocol", "[math][vec]") {
    // These values are sent verbatim in the block-placement packet and indexed
    // by block models. They are protocol, not an internal choice.
    STATIC_REQUIRE(static_cast<u8>(Direction::Down) == 0);
    STATIC_REQUIRE(static_cast<u8>(Direction::Up) == 1);
    STATIC_REQUIRE(static_cast<u8>(Direction::North) == 2);
    STATIC_REQUIRE(static_cast<u8>(Direction::South) == 3);
    STATIC_REQUIRE(static_cast<u8>(Direction::West) == 4);
    STATIC_REQUIRE(static_cast<u8>(Direction::East) == 5);
}

TEST_CASE("direction offsets point the right way", "[math][vec]") {
    STATIC_REQUIRE(direction_offset(Direction::Down) == Vec3i{0, -1, 0});
    STATIC_REQUIRE(direction_offset(Direction::Up) == Vec3i{0, 1, 0});
    // North is -Z and South is +Z. Getting this backwards mirrors every
    // structure that is placed in the world.
    STATIC_REQUIRE(direction_offset(Direction::North) == Vec3i{0, 0, -1});
    STATIC_REQUIRE(direction_offset(Direction::South) == Vec3i{0, 0, 1});
    STATIC_REQUIRE(direction_offset(Direction::West) == Vec3i{-1, 0, 0});
    STATIC_REQUIRE(direction_offset(Direction::East) == Vec3i{1, 0, 0});
}

TEST_CASE("direction names round-trip and match the vanilla spelling",
          "[math][vec]") {
    // These strings are parsed out of blockstate files and written into NBT.
    // They are data format, not debug output.
    REQUIRE(direction_name(Direction::Down) == "down");
    REQUIRE(direction_name(Direction::North) == "north");
    REQUIRE(direction_name(Direction::East) == "east");

    for (u8 i = 0; i < kDirectionCount; ++i) {
        const auto d = static_cast<Direction>(i);
        REQUIRE(direction_from_name(direction_name(d)) == d);
    }

    REQUIRE_FALSE(direction_from_name("upwards").has_value());
    REQUIRE_FALSE(direction_from_name("").has_value());
    REQUIRE_FALSE(direction_from_name("Up").has_value());  // case-sensitive, like vanilla
}

TEST_CASE("opposite() is consistent with the offsets it claims to mirror",
          "[math][vec]") {
    // opposite() flips the low bit, which is only correct because the enum is
    // laid out in opposing pairs. This test is what keeps that from being
    // folklore that someone breaks by reordering the enum.
    for (u8 i = 0; i < kDirectionCount; ++i) {
        const auto d = static_cast<Direction>(i);
        REQUIRE(opposite(opposite(d)) == d);
        REQUIRE(direction_offset(opposite(d)) == -direction_offset(d));
    }
}
