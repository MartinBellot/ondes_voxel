#include "ov/world/chunk.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

using namespace ov;
using namespace ov::world;
using registry::BlockStateId;

namespace {

constexpr AirStates    kAir{BlockStateId{0}, BlockStateId{12817}, BlockStateId{12818}};
constexpr BlockStateId kStone{1};

[[nodiscard]] Chunk make_chunk() {
    // No registry: this fixture only exercises WORLD_SURFACE and block storage.
    return Chunk{ChunkPos{0, 0}, WorldShape::overworld(), kAir, nullptr};
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

    chunk.recompute_heightmaps();
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

// ── The four heightmaps ─────────────────────────────────────────────────────
//
// These need the registry: MOTION_BLOCKING and OCEAN_FLOOR are the measured
// flags, and without them a chunk keeps only WORLD_SURFACE.

namespace {

[[nodiscard]] const registry::BlockRegistry* pack() {
    static const auto loaded = registry::BlockRegistry::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] BlockStateId state_of(std::string_view name) {
    const auto block = pack()->find_block(name);
    REQUIRE(block.has_value());
    return pack()->default_state(*block);
}

}  // namespace

TEST_CASE("the four heightmaps follow their own predicates", "[world][chunk][heightmap]") {
    if (pack() == nullptr) {
        SKIP("no registry pack — run tools/ov_datagen/ovpack.py");
    }
    Chunk chunk{ChunkPos{0, 0}, WorldShape::overworld(), AirStates::from(*pack()), pack()};

    const auto first_free = [&](HeightmapType type) {
        return chunk.heightmap(type).first_free(0, 0);
    };

    chunk.set_block(0, -60, 0, state_of("minecraft:stone"));
    REQUIRE(first_free(HeightmapType::WorldSurface) == -59);
    REQUIRE(first_free(HeightmapType::MotionBlocking) == -59);
    REQUIRE(first_free(HeightmapType::OceanFloor) == -59);

    // A flower is not air, and stops nothing. Only WORLD_SURFACE rises — the
    // measurement that says so covers 187 such blocks.
    chunk.set_block(0, -59, 0, state_of("minecraft:poppy"));
    REQUIRE(first_free(HeightmapType::WorldSurface) == -58);
    REQUIRE(first_free(HeightmapType::MotionBlocking) == -59);
    REQUIRE(first_free(HeightmapType::OceanFloor) == -59);

    // Water is the case the two motion maps exist to separate: it stops no
    // movement but it is a fluid, so MOTION_BLOCKING takes it and OCEAN_FLOOR
    // does not. Getting this backwards puts the ocean's surface where its floor
    // should be.
    chunk.set_block(0, -58, 0, state_of("minecraft:water"));
    REQUIRE(first_free(HeightmapType::WorldSurface) == -57);
    REQUIRE(first_free(HeightmapType::MotionBlocking) == -57);
    REQUIRE(first_free(HeightmapType::OceanFloor) == -59);

    // Leaves stop movement, and MOTION_BLOCKING_NO_LEAVES is the one map that
    // skips them: it stays on the water below rather than following the leaves
    // up. OCEAN_FLOOR does follow them, because leaves stop movement — which is
    // exactly the pair of answers the measurement separated.
    chunk.set_block(0, -57, 0, state_of("minecraft:oak_leaves"));
    REQUIRE(first_free(HeightmapType::MotionBlocking) == -56);
    REQUIRE(first_free(HeightmapType::MotionBlockingNoLeaves) == -57);
    REQUIRE(first_free(HeightmapType::OceanFloor) == -56);
}

TEST_CASE("breaking the top block lowers every map that counted it", "[world][chunk][heightmap]") {
    if (pack() == nullptr) {
        SKIP("no registry pack");
    }
    Chunk chunk{ChunkPos{0, 0}, WorldShape::overworld(), AirStates::from(*pack()), pack()};
    const BlockStateId air   = AirStates::from(*pack()).air;
    const BlockStateId stone = state_of("minecraft:stone");

    chunk.set_block(0, -60, 0, stone);
    chunk.set_block(0, -50, 0, stone);
    REQUIRE(chunk.heightmap(HeightmapType::OceanFloor).first_free(0, 0) == -49);

    // Removing the upper one has to scan back down, not simply decrement: the
    // next block that counts is ten below, not one.
    chunk.set_block(0, -50, 0, air);
    REQUIRE(chunk.heightmap(HeightmapType::WorldSurface).first_free(0, 0) == -59);
    REQUIRE(chunk.heightmap(HeightmapType::MotionBlocking).first_free(0, 0) == -59);
    REQUIRE(chunk.heightmap(HeightmapType::OceanFloor).first_free(0, 0) == -59);

    // And an empty column reads as the world's floor, not as zero.
    chunk.set_block(0, -60, 0, air);
    REQUIRE(chunk.heightmap(HeightmapType::OceanFloor).first_free(0, 0) == -64);
}

TEST_CASE("recomputing agrees with maintaining", "[world][chunk][heightmap]") {
    if (pack() == nullptr) {
        SKIP("no registry pack");
    }
    Chunk chunk{ChunkPos{0, 0}, WorldShape::overworld(), AirStates::from(*pack()), pack()};

    const std::array names = std::to_array<std::string_view>(
        {"minecraft:stone", "minecraft:water", "minecraft:oak_leaves", "minecraft:poppy",
         "minecraft:glass", "minecraft:oak_slab", "minecraft:snow", "minecraft:cobweb"});

    for (usize z = 0; z < 16; ++z) {
        for (usize x = 0; x < 16; ++x) {
            for (usize i = 0; i < names.size(); ++i) {
                chunk.set_block(x, -60 + static_cast<i32>(i), z,
                                state_of(names[(x + z + i) % names.size()]));
            }
        }
    }

    std::array<std::vector<i32>, 4> incremental;
    constexpr std::array kTypes = {HeightmapType::WorldSurface, HeightmapType::MotionBlocking,
                                   HeightmapType::MotionBlockingNoLeaves,
                                   HeightmapType::OceanFloor};
    for (usize i = 0; i < kTypes.size(); ++i) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                incremental[i].push_back(chunk.heightmap(kTypes[i]).first_free(x, z));
            }
        }
    }

    // The two paths are written separately — one raises and scans down on every
    // edit, the other sweeps whole columns — so agreeing is a real check rather
    // than a tautology.
    chunk.recompute_heightmaps();
    for (usize i = 0; i < kTypes.size(); ++i) {
        usize index = 0;
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                REQUIRE(chunk.heightmap(kTypes[i]).first_free(x, z) == incremental[i][index++]);
            }
        }
    }
}
