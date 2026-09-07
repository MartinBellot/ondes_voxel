#include "ov/math/block_pos.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace ov;

TEST_CASE("floor_div rounds towards negative infinity", "[math][pos]") {
    // C++ integer division truncates towards zero: -1 / 16 == 0. But the block
    // at y = -1 is in section -1. Truncating here writes blocks into the wrong
    // section and the wrong region file, silently.
    REQUIRE(floor_div(0, 16) == 0);
    REQUIRE(floor_div(15, 16) == 0);
    REQUIRE(floor_div(16, 16) == 1);
    REQUIRE(floor_div(-1, 16) == -1);
    REQUIRE(floor_div(-16, 16) == -1);
    REQUIRE(floor_div(-17, 16) == -2);

    // The bottom of the overworld since 1.18.
    REQUIRE(floor_div(-64, 16) == -4);
    REQUIRE(floor_div(-49, 16) == -4);
    REQUIRE(floor_div(-65, 16) == -5);
}

TEST_CASE("floor_mod is never negative", "[math][pos]") {
    REQUIRE(floor_mod(0, 16) == 0);
    REQUIRE(floor_mod(15, 16) == 15);
    REQUIRE(floor_mod(16, 16) == 0);
    REQUIRE(floor_mod(-1, 16) == 15);
    REQUIRE(floor_mod(-16, 16) == 0);
    REQUIRE(floor_mod(-64, 16) == 0);

    // The identity that makes the pair usable: value == div * q + mod.
    for (i32 v = -100; v <= 100; ++v) {
        REQUIRE(floor_div(v, 16) * 16 + floor_mod(v, 16) == v);
    }
}

TEST_CASE("a block maps to its containing chunk and section", "[math][pos]") {
    REQUIRE(BlockPos{0, 0, 0}.to_chunk() == ChunkPos{0, 0});
    REQUIRE(BlockPos{15, 0, 15}.to_chunk() == ChunkPos{0, 0});
    REQUIRE(BlockPos{16, 0, 0}.to_chunk() == ChunkPos{1, 0});
    REQUIRE(BlockPos{-1, 0, -1}.to_chunk() == ChunkPos{-1, -1});
    REQUIRE(BlockPos{-16, 0, -16}.to_chunk() == ChunkPos{-1, -1});
    REQUIRE(BlockPos{-17, 0, -17}.to_chunk() == ChunkPos{-2, -2});

    // Overworld build limits since 1.18: y from -64 to 319, sections -4 to 19.
    REQUIRE(BlockPos{0, -64, 0}.to_section() == SectionPos{0, -4, 0});
    REQUIRE(BlockPos{0, -1, 0}.to_section() == SectionPos{0, -1, 0});
    REQUIRE(BlockPos{0, 0, 0}.to_section() == SectionPos{0, 0, 0});
    REQUIRE(BlockPos{0, 319, 0}.to_section() == SectionPos{0, 19, 0});
}

TEST_CASE("local coordinates stay inside the section", "[math][pos]") {
    const BlockPos p{-1, -1, -1};
    REQUIRE(p.local_x() == 15);
    REQUIRE(p.local_y() == 15);
    REQUIRE(p.local_z() == 15);

    for (i32 v : {-100, -64, -17, -1, 0, 1, 15, 16, 100}) {
        const BlockPos q{v, v, v};
        REQUIRE(q.local_x() >= 0);
        REQUIRE(q.local_x() < kSectionSize);
        REQUIRE(q.local_y() >= 0);
        REQUIRE(q.local_y() < kSectionSize);
    }
}

TEST_CASE("section_index is YZX-ordered and covers 0..4095 exactly once", "[math][pos]") {
    // YZX is not a preference: it is the layout of both the Anvil block array
    // and the network chunk packet. A different order means transposing on
    // every chunk read and write.
    REQUIRE(BlockPos{0, 0, 0}.section_index() == 0);
    REQUIRE(BlockPos{1, 0, 0}.section_index() == 1);
    REQUIRE(BlockPos{0, 0, 1}.section_index() == 16);
    REQUIRE(BlockPos{0, 1, 0}.section_index() == 256);
    REQUIRE(BlockPos{15, 15, 15}.section_index() == 4095);

    std::vector<bool> seen(static_cast<std::size_t>(kSectionVolume), false);
    for (i32 y = 0; y < kSectionSize; ++y) {
        for (i32 z = 0; z < kSectionSize; ++z) {
            for (i32 x = 0; x < kSectionSize; ++x) {
                const i32 index = BlockPos{x, y, z}.section_index();
                REQUIRE(index >= 0);
                REQUIRE(index < kSectionVolume);
                REQUIRE_FALSE(seen[static_cast<std::size_t>(index)]);
                seen[static_cast<std::size_t>(index)] = true;
            }
        }
    }
}

TEST_CASE("section_index ignores which section the block is in", "[math][pos]") {
    // The index is local, so a block and the same block one chunk over share it.
    REQUIRE(BlockPos{0, 0, 0}.section_index() == BlockPos{16, 16, 16}.section_index());
    REQUIRE(BlockPos{0, 0, 0}.section_index() == BlockPos{-16, -16, -16}.section_index());
}

TEST_CASE("chunks map to region files and header slots", "[math][pos]") {
    // Regions are 32x32 chunks; the header table is indexed z-major.
    REQUIRE(ChunkPos{0, 0}.region_x() == 0);
    REQUIRE(ChunkPos{31, 31}.region_x() == 0);
    REQUIRE(ChunkPos{32, 0}.region_x() == 1);
    REQUIRE(ChunkPos{-1, -1}.region_x() == -1);
    REQUIRE(ChunkPos{-32, -32}.region_x() == -1);
    REQUIRE(ChunkPos{-33, 0}.region_x() == -2);

    REQUIRE(ChunkPos{0, 0}.region_slot() == 0);
    REQUIRE(ChunkPos{1, 0}.region_slot() == 1);
    REQUIRE(ChunkPos{0, 1}.region_slot() == 32);
    REQUIRE(ChunkPos{31, 31}.region_slot() == 1023);
    REQUIRE(ChunkPos{-1, -1}.region_slot() == 1023);

    // Every slot in a region is hit exactly once.
    std::vector<bool> seen(1024, false);
    for (i32 z = 0; z < 32; ++z) {
        for (i32 x = 0; x < 32; ++x) {
            const i32 slot = ChunkPos{x, z}.region_slot();
            REQUIRE_FALSE(seen[static_cast<std::size_t>(slot)]);
            seen[static_cast<std::size_t>(slot)] = true;
        }
    }
}

TEST_CASE("ChunkPos packing round-trips, negatives included", "[math][pos]") {
    // -1875000 is the world border in chunks, so these are real extremes.
    for (const ChunkPos p : {ChunkPos{0, 0}, ChunkPos{1, -1}, ChunkPos{-1875000, 1875000},
                             ChunkPos{-1, -1}, ChunkPos{INT32_MAX, INT32_MIN}}) {
        REQUIRE(ChunkPos::from_packed(p.packed()) == p);
    }

    // Distinct chunks must not collide: packing is the hash-map key for the
    // entire chunk store.
    REQUIRE(ChunkPos{1, 0}.packed() != ChunkPos{0, 1}.packed());
    REQUIRE(ChunkPos{-1, 0}.packed() != ChunkPos{0, -1}.packed());
}

TEST_CASE("view distance uses Chebyshev distance, not Euclidean", "[math][pos]") {
    // Render distance 12 means a 25x25 square of columns, not a circle.
    const ChunkPos origin{0, 0};
    REQUIRE(origin.chebyshev_distance(ChunkPos{12, 0}) == 12);
    REQUIRE(origin.chebyshev_distance(ChunkPos{12, 12}) == 12);
    REQUIRE(origin.chebyshev_distance(ChunkPos{-12, 12}) == 12);
    REQUIRE(origin.chebyshev_distance(ChunkPos{13, 0}) == 13);

    i32 within = 0;
    for (i32 z = -20; z <= 20; ++z) {
        for (i32 x = -20; x <= 20; ++x) {
            if (origin.chebyshev_distance(ChunkPos{x, z}) <= 12) {
                ++within;
            }
        }
    }
    REQUIRE(within == 25 * 25);
}

TEST_CASE("ChunkPos works as an unordered_map key", "[math][pos]") {
    std::unordered_map<ChunkPos, int> chunks;
    chunks[ChunkPos{0, 0}]  = 1;
    chunks[ChunkPos{-1, 5}] = 2;
    chunks[ChunkPos{0, 0}]  = 3;

    REQUIRE(chunks.size() == 2);
    REQUIRE(chunks.at(ChunkPos{0, 0}) == 3);
    REQUIRE(chunks.at(ChunkPos{-1, 5}) == 2);
}

TEST_CASE("SectionPos exposes its world-space corner", "[math][pos]") {
    REQUIRE(SectionPos{0, -4, 0}.min_block() == BlockPos{0, -64, 0});
    REQUIRE(SectionPos{1, 0, -1}.min_block() == BlockPos{16, 0, -16});
    REQUIRE(SectionPos{2, 3, 4}.to_chunk() == ChunkPos{2, 4});
}
