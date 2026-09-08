#include "ov/world/chunk.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::world;
using registry::BlockStateId;

namespace {

constexpr AirStates    kAir{BlockStateId{0}, BlockStateId{12817}, BlockStateId{12818}};
constexpr BlockStateId kStone{1};

[[nodiscard]] Chunk make_chunk() {
    return Chunk{ChunkPos{0, 0}, WorldShape::overworld(), kAir};
}

}  // namespace

TEST_CASE("the overworld is 24 sections from -64", "[world][chunk]") {
    const auto shape = WorldShape::overworld();
    REQUIRE(shape.min_y == -64);
    REQUIRE(shape.max_y() == 319);
    REQUIRE(shape.section_count() == 24);
    REQUIRE(shape.min_section() == -4);

    // The Nether and the End start at zero, which is why the shape is passed
    // in. Code that hard-codes -64 works until the first Nether chunk.
    REQUIRE(WorldShape::nether().min_y == 0);
    REQUIRE(WorldShape::nether().section_count() == 16);
    REQUIRE(WorldShape::nether().max_y() == 255);
}

TEST_CASE("sections are picked with an arithmetic shift, not a division", "[world][chunk]") {
    // Most of the Overworld has negative y. Truncation towards zero would put
    // y = -1 and y = 0 in the same section while y = -16 and y = -17 landed
    // apart — a seam sixteen blocks thick at the world's origin.
    Chunk chunk = make_chunk();

    REQUIRE(chunk.section_for_y(-64) != nullptr);
    REQUIRE(chunk.section_for_y(319) != nullptr);
    REQUIRE(chunk.section_for_y(-65) == nullptr);
    REQUIRE(chunk.section_for_y(320) == nullptr);

    // -1 and 0 must be different sections; -16 and -1 the same.
    REQUIRE(chunk.section_for_y(-1) != chunk.section_for_y(0));
    REQUIRE(chunk.section_for_y(-16) == chunk.section_for_y(-1));
    REQUIRE(chunk.section_for_y(0) == chunk.section_for_y(15));
    REQUIRE(chunk.section_for_y(0) != chunk.section_for_y(16));
}

TEST_CASE("a block written at world y comes back from world y", "[world][chunk]") {
    Chunk chunk = make_chunk();

    chunk.set_block(3, -64, 7, kStone);
    chunk.set_block(3, 0, 7, BlockStateId{2});
    chunk.set_block(3, 319, 7, BlockStateId{3});

    REQUIRE(chunk.get_block(3, -64, 7) == kStone);
    REQUIRE(chunk.get_block(3, 0, 7) == BlockStateId{2});
    REQUIRE(chunk.get_block(3, 319, 7) == BlockStateId{3});
    REQUIRE(chunk.get_block(3, -63, 7) == kAir.air);
    REQUIRE(chunk.non_air_count() == 3);
}

TEST_CASE("blocks outside the column are ignored", "[world][chunk][malformed]") {
    Chunk chunk = make_chunk();

    chunk.set_block(16, 0, 0, kStone);
    chunk.set_block(0, 400, 0, kStone);
    chunk.set_block(0, -100, 0, kStone);
    REQUIRE(chunk.non_air_count() == 0);
    REQUIRE(chunk.get_block(0, 400, 0) == kAir.air);
}

// ── WORLD_SURFACE, maintained rather than recomputed ────────────────────────

TEST_CASE("placing a block raises the surface", "[world][chunk][heightmap]") {
    Chunk            chunk   = make_chunk();
    const Heightmap& surface = chunk.heightmap(HeightmapType::WorldSurface);

    REQUIRE(surface.first_free(2, 2) == -64);

    chunk.set_block(2, 70, 2, kStone);
    REQUIRE(surface.first_free(2, 2) == 71);

    // A block below the surface does not move it.
    chunk.set_block(2, 30, 2, kStone);
    REQUIRE(surface.first_free(2, 2) == 71);

    // One above does.
    chunk.set_block(2, 100, 2, kStone);
    REQUIRE(surface.first_free(2, 2) == 101);
}

TEST_CASE("breaking the surface block scans back down", "[world][chunk][heightmap]") {
    // The expensive direction, and the one an implementation forgets: raising
    // is a comparison, lowering is a search. Forgetting it leaves the heightmap
    // pointing at a block that is no longer there, and rain falls onto nothing.
    Chunk            chunk   = make_chunk();
    const Heightmap& surface = chunk.heightmap(HeightmapType::WorldSurface);

    chunk.set_block(4, 20, 4, kStone);
    chunk.set_block(4, 60, 4, kStone);
    chunk.set_block(4, 61, 4, kStone);
    REQUIRE(surface.first_free(4, 4) == 62);

    chunk.set_block(4, 61, 4, kAir.air);
    REQUIRE(surface.first_free(4, 4) == 61);

    chunk.set_block(4, 60, 4, kAir.air);
    REQUIRE(surface.first_free(4, 4) == 21);

    // Emptying the column returns it to "nothing here".
    chunk.set_block(4, 20, 4, kAir.air);
    REQUIRE(surface.first_free(4, 4) == -64);
}

TEST_CASE("breaking a block under the surface leaves it alone", "[world][chunk][heightmap]") {
    // Mining a tunnel must not trigger a column scan on every block, and must
    // not move a surface that has not moved.
    Chunk            chunk   = make_chunk();
    const Heightmap& surface = chunk.heightmap(HeightmapType::WorldSurface);

    chunk.set_block(5, 10, 5, kStone);
    chunk.set_block(5, 80, 5, kStone);
    REQUIRE(surface.first_free(5, 5) == 81);

    chunk.set_block(5, 10, 5, kAir.air);
    REQUIRE(surface.first_free(5, 5) == 81);
}

TEST_CASE("cave air breaks the surface like real air", "[world][chunk][heightmap]") {
    Chunk            chunk   = make_chunk();
    const Heightmap& surface = chunk.heightmap(HeightmapType::WorldSurface);

    chunk.set_block(6, 50, 6, kStone);
    REQUIRE(surface.first_free(6, 6) == 51);

    chunk.set_block(6, 50, 6, kAir.cave_air);
    REQUIRE(surface.first_free(6, 6) == -64);
}

TEST_CASE("the incremental surface agrees with a full rebuild", "[world][chunk][heightmap]") {
    // The claim the incremental path is making. A maintained heightmap is
    // exactly the kind of state that drifts a block at a time until something
    // visible breaks a week later.
    Chunk chunk = make_chunk();

    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            const i32 top = static_cast<i32>((x * 7 + z * 11) % 120) - 40;
            for (i32 y = -64; y <= top; ++y) {
                chunk.set_block(x, y, z, kStone);
            }
        }
    }
    // Then carve some of it back out, which is where drift creeps in.
    for (usize x = 0; x < 16; ++x) {
        chunk.set_block(x, static_cast<i32>((x * 7) % 120) - 40, x, kAir.air);
    }

    std::vector<i32> incremental;
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            incremental.push_back(chunk.heightmap(HeightmapType::WorldSurface).first_free(x, z));
        }
    }

    chunk.recompute_world_surface();
    usize i = 0;
    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            INFO("column " << x << "," << z);
            REQUIRE(chunk.heightmap(HeightmapType::WorldSurface).first_free(x, z) ==
                    incremental[i++]);
        }
    }
}

TEST_CASE("biomes are stored per section and read by world y", "[world][chunk]") {
    Chunk chunk = make_chunk();

    chunk.set_biome(0, 0, 0, 42);
    REQUIRE(chunk.get_biome(0, 0, 0) == 42);
    REQUIRE(chunk.get_biome(3, 3, 3) == 42);   // same 4x4x4 cell
    REQUIRE(chunk.get_biome(4, 0, 0) == 0);    // next cell
    REQUIRE(chunk.get_biome(0, -64, 0) == 0);  // different section
}
