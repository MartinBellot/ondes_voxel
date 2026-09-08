#include "ov/world/heightmap.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::world;

namespace {

/// The overworld: floor at -64, 384 blocks tall.
constexpr i32 kMinY   = -64;
constexpr u32 kHeight = 384;

}  // namespace

TEST_CASE("a 384-block world needs nine bits", "[world][heightmap]") {
    // 385 possibilities, not 384: a column can be full to the very top. Sizing
    // for 384 makes the top of the world unrepresentable, which is invisible
    // until someone builds there.
    REQUIRE(bits_for_height(384) == 9);
    REQUIRE(bits_for_height(256) == 9);  // 257 values
    REQUIRE(bits_for_height(255) == 8);
    REQUIRE(bits_for_height(511) == 9);
    REQUIRE(bits_for_height(512) == 10);
}

TEST_CASE("the packing is the palette rule again", "[world][heightmap]") {
    // Nine bits, seven to a long, and an entry never spans two longs. 37 longs
    // for 256 columns — not 36, which is what spanning would give.
    const Heightmap map{kMinY, kHeight};
    REQUIRE(map.bits() == 9);
    REQUIRE(map.data().size() == 37);
    REQUIRE(entries_per_long(9) == 7);
}

TEST_CASE("an empty column reports the world floor", "[world][heightmap]") {
    // Zero stored means nothing in the column, and the first free space is the
    // floor itself. That is what keeps "empty" and "full to the floor"
    // distinguishable.
    const Heightmap map{kMinY, kHeight};
    REQUIRE(map.first_free(0, 0) == kMinY);
    REQUIRE(map.first_free(15, 15) == kMinY);
}

TEST_CASE("what is stored is the free space, not the surface block", "[world][heightmap]") {
    // Measured against 4577024 columns of a real world: stored == top + 1 - minY
    // in every one of them. The other convention makes rain fall a block into
    // the ground.
    Heightmap map{kMinY, kHeight};

    map.set_surface(3, 7, 69);
    REQUIRE(map.first_free(3, 7) == 70);
    REQUIRE(map.data()[(7 * 16 + 3) / 7] != 0);

    // A block on the floor itself stores 1, not 0 — otherwise it would be
    // indistinguishable from an empty column.
    map.set_surface(0, 0, kMinY);
    REQUIRE(map.first_free(0, 0) == kMinY + 1);
}

TEST_CASE("the very top of the world is representable", "[world][heightmap]") {
    Heightmap map{kMinY, kHeight};
    const i32 top = kMinY + static_cast<i32>(kHeight) - 1;  // 319

    map.set_surface(1, 1, top);
    REQUIRE(map.first_free(1, 1) == top + 1);
    REQUIRE(map.first_free(1, 1) == 320);
}

TEST_CASE("every column is addressable and independent", "[world][heightmap]") {
    // Off-by-one in the shift arithmetic shows up as neighbouring columns
    // overwriting each other, which in a world looks like patchy rain rather
    // than an obvious failure.
    Heightmap map{kMinY, kHeight};

    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            map.set_surface(x, z, static_cast<i32>(z * 16 + x) - 64);
        }
    }
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            REQUIRE(map.first_free(x, z) == static_cast<i32>(z * 16 + x) - 63);
        }
    }
}

TEST_CASE("raising only ever moves the surface up", "[world][heightmap]") {
    // The common path when a player builds. Lowering needs a rescan of the
    // column, which is why breaking a block is the expensive direction.
    Heightmap map{kMinY, kHeight};

    map.set_surface(5, 5, 60);
    map.raise_to(5, 5, 40);
    REQUIRE(map.first_free(5, 5) == 61);

    map.raise_to(5, 5, 80);
    REQUIRE(map.first_free(5, 5) == 81);
}

TEST_CASE("clearing a column returns it to empty", "[world][heightmap]") {
    Heightmap map{kMinY, kHeight};
    map.set_surface(2, 2, 100);
    map.clear_column(2, 2);
    REQUIRE(map.first_free(2, 2) == kMinY);
}

TEST_CASE("packed longs round-trip", "[world][heightmap]") {
    Heightmap original{kMinY, kHeight};
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            original.set_surface(x, z, static_cast<i32>((x * 13 + z * 7) % 300) - 64);
        }
    }

    Heightmap loaded{kMinY, kHeight};
    REQUIRE(loaded.load(original.data()));
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            REQUIRE(loaded.first_free(x, z) == original.first_free(x, z));
        }
    }
}

TEST_CASE("the format names are exact", "[world][heightmap]") {
    // A typo costs a heightmap in silence: the client receives a compound with
    // a key it does not recognise and renders nothing.
    REQUIRE(to_string(HeightmapType::WorldSurface) == "WORLD_SURFACE");
    REQUIRE(to_string(HeightmapType::MotionBlocking) == "MOTION_BLOCKING");
    REQUIRE(to_string(HeightmapType::MotionBlockingNoLeaves) == "MOTION_BLOCKING_NO_LEAVES");
    REQUIRE(to_string(HeightmapType::OceanFloor) == "OCEAN_FLOOR");

    REQUIRE(heightmap_type_from("MOTION_BLOCKING") == HeightmapType::MotionBlocking);
    REQUIRE_FALSE(heightmap_type_from("motion_blocking").has_value());
    REQUIRE_FALSE(heightmap_type_from("NOT_A_HEIGHTMAP").has_value());

    // Only two of the six go to the client.
    REQUIRE(is_sent_to_client(HeightmapType::WorldSurface));
    REQUIRE(is_sent_to_client(HeightmapType::MotionBlocking));
    REQUIRE_FALSE(is_sent_to_client(HeightmapType::OceanFloor));
    REQUIRE_FALSE(is_sent_to_client(HeightmapType::WorldSurfaceWG));
}

TEST_CASE("a heightmap of the wrong length is refused", "[world][heightmap][malformed]") {
    // Arrives from disk and from the network, both untrusted.
    Heightmap map{kMinY, kHeight};
    REQUIRE_FALSE(map.load(std::vector<u64>(36, 0)));
    REQUIRE_FALSE(map.load(std::vector<u64>(38, 0)));
    REQUIRE_FALSE(map.load({}));
    REQUIRE(map.load(std::vector<u64>(37, 0)));
}

TEST_CASE("a surface outside the world is clamped, not lost", "[world][heightmap][malformed]") {
    // A caller passing an impossible y is a bug, but it is not worth losing a
    // chunk over — and wrapping would put the surface underground.
    Heightmap map{kMinY, kHeight};

    map.set_surface(0, 0, 100000);
    REQUIRE(map.first_free(0, 0) == kMinY + static_cast<i32>(kHeight));

    map.set_surface(1, 0, -100000);
    REQUIRE(map.first_free(1, 0) == kMinY);

    map.set_surface(20, 20, 50);  // outside the chunk entirely
    REQUIRE(map.first_free(0, 0) == kMinY + static_cast<i32>(kHeight));
}
