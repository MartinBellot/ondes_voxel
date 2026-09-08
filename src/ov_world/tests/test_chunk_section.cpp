#include "ov/world/chunk_section.hpp"
#include "ov/world/light_array.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::world;
using registry::BlockStateId;

namespace {

/// The real air ids, as the registry reports them. Written here rather than
/// looked up so the storage tests do not need a data pack; the lookup itself is
/// covered by the registry tests.
constexpr AirStates kAir{BlockStateId{0}, BlockStateId{12817}, BlockStateId{12818}};

}  // namespace

// ── Index order ─────────────────────────────────────────────────────────────

TEST_CASE("indices run in YZX order", "[world][section]") {
    // The file format and the wire format both use this order. XZY would work
    // perfectly until the first chunk was written, and would then produce a
    // world that is a transposition of itself — every structure mirrored
    // through a diagonal, with nothing reporting an error.
    REQUIRE(section_index(0, 0, 0) == 0);
    REQUIRE(section_index(1, 0, 0) == 1);    // x moves fastest
    REQUIRE(section_index(0, 0, 1) == 16);   // then z
    REQUIRE(section_index(0, 1, 0) == 256);  // then y
    REQUIRE(section_index(15, 15, 15) == 4095);

    // Every position maps to a distinct index, and they fill the space.
    std::vector<bool> seen(4096, false);
    for (usize y = 0; y < 16; ++y) {
        for (usize z = 0; z < 16; ++z) {
            for (usize x = 0; x < 16; ++x) {
                const usize index = section_index(x, y, z);
                REQUIRE(index < 4096);
                REQUIRE_FALSE(seen[index]);
                seen[index] = true;
            }
        }
    }
}

TEST_CASE("biomes are one cell per 4x4x4 cube", "[world][section]") {
    // 64 cells, not 4096. Sixteen blocks share each one, which is why a biome
    // border is visibly blocky at close range in the real game.
    REQUIRE(biome_index(0, 0, 0) == 0);
    REQUIRE(biome_index(3, 3, 3) == 0);  // same cube
    REQUIRE(biome_index(4, 0, 0) == 1);  // next cube along x
    REQUIRE(biome_index(0, 0, 4) == 4);
    REQUIRE(biome_index(0, 4, 0) == 16);
    REQUIRE(biome_index(15, 15, 15) == 63);
}

// ── Blocks and the count the wire needs ─────────────────────────────────────

TEST_CASE("a fresh section is entirely air and counts none", "[world][section]") {
    const ChunkSection section{kAir};

    REQUIRE(section.is_empty());
    REQUIRE(section.non_air_count() == 0);
    REQUIRE(section.get_block(0, 0, 0) == BlockStateId{0});
    REQUIRE(section.blocks().is_single_valued());
}

TEST_CASE("the non-air count follows every placement", "[world][section]") {
    // This number goes on the wire on every chunk send. Recomputing it by
    // walking 4096 entries per section would dominate the packet, so it is
    // maintained incrementally — and an incremental count is exactly the kind
    // that drifts.
    ChunkSection section{kAir};

    section.set_block(1, 2, 3, BlockStateId{100});
    REQUIRE(section.non_air_count() == 1);
    REQUIRE_FALSE(section.is_empty());

    // Replacing one solid block with another does not change the count.
    section.set_block(1, 2, 3, BlockStateId{200});
    REQUIRE(section.non_air_count() == 1);

    // Breaking it does.
    section.set_block(1, 2, 3, BlockStateId{0});
    REQUIRE(section.non_air_count() == 0);
    REQUIRE(section.is_empty());
}

TEST_CASE("cave air and void air count as air", "[world][section]") {
    // The distinction that matters on the wire: a client told a cave is solid
    // renders it solid. All three are air, and only the first is state 0.
    ChunkSection section{kAir};

    section.set_block(0, 0, 0, kAir.cave_air);
    section.set_block(1, 0, 0, kAir.void_air);
    REQUIRE(section.non_air_count() == 0);
    REQUIRE(section.is_empty());

    section.set_block(2, 0, 0, BlockStateId{1});
    REQUIRE(section.non_air_count() == 1);
}

TEST_CASE("the incremental count agrees with a full recount", "[world][section]") {
    ChunkSection section{kAir};

    for (usize i = 0; i < 4096; ++i) {
        const usize x = i % 16;
        const usize y = (i / 256) % 16;
        const usize z = (i / 16) % 16;
        section.set_block(x, y, z, BlockStateId{static_cast<u16>(i % 7)});
    }

    const u16 incremental = section.non_air_count();
    section.recount();
    REQUIRE(section.non_air_count() == incremental);

    // One in seven is state 0 and nothing else here is air. Counted rather
    // than reduced to a formula: 4096/7 rounds to 585 while there are 586
    // multiples of seven below 4096, and my first version of this line got
    // that wrong and accused the implementation.
    usize air = 0;
    for (usize i = 0; i < 4096; ++i) {
        air += (i % 7 == 0) ? 1 : 0;
    }
    REQUIRE(incremental == 4096 - air);
}

TEST_CASE("a fully solid section counts every block", "[world][section]") {
    // 4096 has to survive being stored: a u8 count would wrap to zero and the
    // section would be sent as empty.
    ChunkSection     section{kAir};
    std::vector<u16> solid(4096, 5);
    REQUIRE(section.load_blocks(0, std::vector<u16>{5}, {}));
    REQUIRE(section.non_air_count() == 4096);
    REQUIRE_FALSE(section.is_empty());
}

TEST_CASE("writing outside the section is ignored", "[world][section][malformed]") {
    ChunkSection section{kAir};
    section.set_block(16, 0, 0, BlockStateId{1});
    section.set_block(0, 99, 0, BlockStateId{1});
    REQUIRE(section.non_air_count() == 0);
}

// ── Light ───────────────────────────────────────────────────────────────────

TEST_CASE("a uniform light array stores nothing", "[world][light]") {
    // The reason a loaded world fits in memory. Above the terrain the sky is
    // uniformly full and the block light uniformly zero; below the stone both
    // are zero. Measured on a real chunk: 7 of 24 sections carried a block
    // light array and 2 a sky light array.
    LightArray dark;
    REQUIRE(dark.is_uniform());
    REQUIRE(dark.data().empty());
    REQUIRE(dark.get(0) == 0);
    REQUIRE(dark.get(4095) == 0);

    LightArray full{kMaxLightLevel};
    REQUIRE(full.is_uniform());
    REQUIRE(full.data().empty());
    REQUIRE(full.get(2000) == 15);
}

TEST_CASE("writing the same value keeps an array uniform", "[world][light]") {
    LightArray full{15};
    full.set(100, 15);
    REQUIRE(full.is_uniform());
    REQUIRE(full.data().empty());
}

TEST_CASE("materialising preserves the value that was implicit", "[world][light]") {
    // The trap: allocating a zero-filled buffer for an array that was uniformly
    // *bright* plunges the section into darkness, and only for sections that
    // happen to be edited.
    LightArray full{15};
    full.set(0, 3);

    REQUIRE_FALSE(full.is_uniform());
    REQUIRE(full.data().size() == kLightByteCount);
    REQUIRE(full.get(0) == 3);
    for (usize i = 1; i < kLightCellCount; ++i) {
        REQUIRE(full.get(i) == 15);
    }
}

TEST_CASE("the even cell is the low nibble", "[world][light]") {
    // Reversing this lights the world in a fine checkerboard, which reads as a
    // shader bug rather than a storage one.
    LightArray light;
    light.set(0, 0xA);
    light.set(1, 0x3);

    REQUIRE(light.data()[0] == 0x3A);
    REQUIRE(light.get(0) == 0xA);
    REQUIRE(light.get(1) == 0x3);
}

TEST_CASE("every cell is addressable and independent", "[world][light]") {
    LightArray light;
    for (usize i = 0; i < kLightCellCount; ++i) {
        light.set(i, static_cast<u8>(i % 16));
    }
    for (usize i = 0; i < kLightCellCount; ++i) {
        REQUIRE(light.get(i) == static_cast<u8>(i % 16));
    }
    REQUIRE(light.data().size() == 2048);
}

TEST_CASE("compacting drops storage a section no longer needs", "[world][light]") {
    // A section that was lit and then buried is uniform again. Staying
    // materialised keeps 2 KiB per section for nothing, and there are hundreds
    // of sections loaded.
    LightArray light{15};
    light.set(0, 0);
    REQUIRE_FALSE(light.is_uniform());

    light.set(0, 15);
    light.compact();
    REQUIRE(light.is_uniform());
    REQUIRE(light.uniform_value() == 15);
    REQUIRE(light.data().empty());
    REQUIRE(light.get(0) == 15);
}

TEST_CASE("compacting leaves a genuinely varied array alone", "[world][light]") {
    LightArray light;
    light.set(0, 1);
    light.set(4095, 2);
    light.compact();
    REQUIRE_FALSE(light.is_uniform());
    REQUIRE(light.get(0) == 1);
    REQUIRE(light.get(4095) == 2);
}

TEST_CASE("a light array of the wrong length is refused", "[world][light][malformed]") {
    // Arrives from disk and from the network, both untrusted. A short array
    // would be read past on the first lookup.
    LightArray light;
    REQUIRE_FALSE(light.load(std::vector<u8>(2047, 0)));
    REQUIRE_FALSE(light.load(std::vector<u8>(2049, 0)));
    REQUIRE_FALSE(light.load(std::vector<u8>(1, 0)));

    // An absent array is legitimate: it means uniformly dark.
    REQUIRE(light.load({}));
    REQUIRE(light.is_uniform());

    REQUIRE(light.load(std::vector<u8>(2048, 0xFF)));
    REQUIRE(light.get(0) == 15);
}

TEST_CASE("reading outside a light array returns nothing", "[world][light][malformed]") {
    LightArray light{7};
    REQUIRE(light.get(4096) == 0);
    REQUIRE(light.get(999999) == 0);
    light.set(4096, 1);
    REQUIRE(light.is_uniform());
}
