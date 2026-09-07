#include "ov/world/paletted_container.hpp"

#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <vector>

using namespace ov;
using namespace ov::world;

TEST_CASE("entries never span two longs", "[world][palette]") {
    // The post-1.16 packing rule, and the one an implementation gets wrong by
    // being clever. Before 1.16 entries did span, so reading a modern chunk
    // with the old rule produces plausible garbage rather than an error.
    REQUIRE(entries_per_long(4) == 16);
    REQUIRE(entries_per_long(5) == 12);  // 60 bits used, 4 wasted
    REQUIRE(entries_per_long(6) == 10);  // 60 used, 4 wasted
    REQUIRE(entries_per_long(8) == 8);
    REQUIRE(entries_per_long(15) == 4);  // 60 used, 4 wasted

    // And the long counts those imply for a full block section.
    REQUIRE(packed_length(4096, 4) == 256);
    REQUIRE(packed_length(4096, 5) == 342);  // not 320, which spanning would give
    REQUIRE(packed_length(4096, 15) == 1024);
    REQUIRE(packed_length(64, 6) == 7);  // a biome section
}

TEST_CASE("a palette width is never below four bits", "[world][palette]") {
    // Vanilla pads up to 4, so a two-entry palette still uses 4-bit indices.
    // Packing them at 1 bit would be smaller and unreadable by the client.
    REQUIRE(bits_for_palette(2) == 4);
    REQUIRE(bits_for_palette(16) == 4);
    REQUIRE(bits_for_palette(17) == 5);
    REQUIRE(bits_for_palette(32) == 5);
    REQUIRE(bits_for_palette(33) == 6);
    REQUIRE(bits_for_palette(256) == 8);
}

TEST_CASE("a new section is single-valued and stores no data", "[world][palette]") {
    // The common case by a wide margin: most sections are entirely air or
    // entirely stone, and storing 4096 identical entries would be waste.
    const auto section = PalettedContainer::blocks(0);

    REQUIRE(section.kind() == PaletteKind::SingleValue);
    REQUIRE(section.bits() == 0);
    REQUIRE(section.data().empty());
    REQUIRE(section.capacity() == 4096);
    REQUIRE(section.get(0) == 0);
    REQUIRE(section.get(4095) == 0);
}

TEST_CASE("setting the same value keeps a section uniform", "[world][palette]") {
    auto section = PalettedContainer::blocks(7);
    section.set(100, 7);

    REQUIRE(section.kind() == PaletteKind::SingleValue);
    REQUIRE(section.data().empty());
}

TEST_CASE("a second distinct value grows to an indirect palette", "[world][palette]") {
    auto section = PalettedContainer::blocks(0);
    section.set(42, 9);

    REQUIRE(section.kind() == PaletteKind::Indirect);
    REQUIRE(section.bits() == 4);
    REQUIRE(section.palette_size() == 2);
    REQUIRE(section.data().size() == 256);

    REQUIRE(section.get(42) == 9);
    REQUIRE(section.get(41) == 0);
    REQUIRE(section.get(43) == 0);
}

TEST_CASE("every index is addressable and independent", "[world][palette]") {
    // Off-by-one in the shift arithmetic shows up as neighbouring entries
    // overwriting each other, which in a world looks like scattered wrong
    // blocks rather than an obvious failure.
    auto section = PalettedContainer::blocks(0);

    for (usize i = 0; i < 4096; ++i) {
        section.set(i, static_cast<u16>(i % 13));
    }
    for (usize i = 0; i < 4096; ++i) {
        REQUIRE(section.get(i) == static_cast<u16>(i % 13));
    }
}

TEST_CASE("the palette widens as distinct values accumulate", "[world][palette]") {
    auto section = PalettedContainer::blocks(0);

    // Fifteen new values, plus the fill: sixteen distinct, the most 4 bits can
    // index.
    for (u16 value = 1; value <= 15; ++value) {
        section.set(value, value);
    }
    REQUIRE(section.palette_size() == 16);
    REQUIRE(section.bits() == 4);

    // The seventeenth forces a widen, and everything already stored has to
    // survive being repacked at the new width.
    section.set(100, 999);
    REQUIRE(section.palette_size() == 17);
    REQUIRE(section.bits() == 5);
    REQUIRE(section.get(100) == 999);

    for (u16 value = 1; value <= 15; ++value) {
        REQUIRE(section.get(value) == value);
    }
}

TEST_CASE("past 256 distinct values the palette is abandoned", "[world][palette]") {
    // At that point an index costs as much as the id it stands for, so vanilla
    // switches to packing registry ids directly.
    auto section = PalettedContainer::blocks(0);

    for (usize i = 0; i < 300; ++i) {
        section.set(i, static_cast<u16>(i + 1));
    }

    REQUIRE(section.kind() == PaletteKind::Direct);
    REQUIRE(section.bits() == kDirectBlockBits);
    REQUIRE(section.palette().empty());
    REQUIRE(section.data().size() == 1024);

    for (usize i = 0; i < 300; ++i) {
        REQUIRE(section.get(i) == static_cast<u16>(i + 1));
    }
    REQUIRE(section.get(500) == 0);
}

TEST_CASE("a direct section holds the largest block state", "[world][palette]") {
    // 24134 is the last state in 1.20.1, and 15 bits must reach it.
    auto section = PalettedContainer::blocks(0);
    for (usize i = 0; i < 300; ++i) {
        section.set(i, static_cast<u16>(i + 1));
    }
    REQUIRE(section.kind() == PaletteKind::Direct);

    section.set(1000, 24134);
    REQUIRE(section.get(1000) == 24134);
}

TEST_CASE("assign picks the tightest representation", "[world][palette]") {
    auto section = PalettedContainer::blocks(0);

    // All the same: back to single-valued, with no data at all.
    section.assign(std::vector<u16>(4096, 5));
    REQUIRE(section.kind() == PaletteKind::SingleValue);
    REQUIRE(section.data().empty());
    REQUIRE(section.get(0) == 5);

    // Three values: a 4-bit palette.
    std::vector<u16> three(4096);
    for (usize i = 0; i < three.size(); ++i) {
        three[i] = static_cast<u16>(i % 3);
    }
    section.assign(three);
    REQUIRE(section.kind() == PaletteKind::Indirect);
    REQUIRE(section.bits() == 4);
    REQUIRE(section.palette_size() == 3);
    for (usize i = 0; i < 4096; ++i) {
        REQUIRE(section.get(i) == static_cast<u16>(i % 3));
    }

    // Too many for a palette: direct.
    std::vector<u16> many(4096);
    std::iota(many.begin(), many.end(), u16{1});
    section.assign(many);
    REQUIRE(section.kind() == PaletteKind::Direct);
    for (usize i = 0; i < 4096; ++i) {
        REQUIRE(section.get(i) == static_cast<u16>(i + 1));
    }
}

TEST_CASE("a biome section is 64 cells at 6 bits", "[world][palette]") {
    auto biomes = PalettedContainer::biomes(0);
    REQUIRE(biomes.capacity() == 64);

    for (usize i = 0; i < 64; ++i) {
        biomes.set(i, static_cast<u16>(i));
    }
    // 64 distinct values still fits an indirect palette at 6 bits.
    REQUIRE(biomes.kind() == PaletteKind::Indirect);
    REQUIRE(biomes.bits() == 6);
    for (usize i = 0; i < 64; ++i) {
        REQUIRE(biomes.get(i) == static_cast<u16>(i));
    }
}

// ── Loading from disk and from the wire ─────────────────────────────────────

TEST_CASE("packed data round-trips through load", "[world][palette]") {
    auto original = PalettedContainer::blocks(0);
    for (usize i = 0; i < 4096; ++i) {
        original.set(i, static_cast<u16>(i % 40));
    }

    auto loaded = PalettedContainer::blocks(0);
    REQUIRE(loaded.load_packed(original.bits(), original.palette(), original.data()));

    for (usize i = 0; i < 4096; ++i) {
        REQUIRE(loaded.get(i) == original.get(i));
    }
    REQUIRE(loaded.bits() == original.bits());
    REQUIRE(loaded.data().size() == original.data().size());
}

TEST_CASE("a single-valued section loads with no data", "[world][palette]") {
    auto                   section = PalettedContainer::blocks(0);
    const std::vector<u16> palette{1234};

    REQUIRE(section.load_packed(0, palette, {}));
    REQUIRE(section.kind() == PaletteKind::SingleValue);
    REQUIRE(section.get(0) == 1234);
    REQUIRE(section.get(4095) == 1234);
}

TEST_CASE("a section that lies about its shape is rejected", "[world][palette][malformed]") {
    // Both the disk and the network are untrusted here. A mismatch between the
    // declared width and the data length would otherwise read past the buffer.
    auto                   section = PalettedContainer::blocks(0);
    const std::vector<u16> palette{0, 1};

    // Says 5 bits, provides the long count for 4.
    REQUIRE_FALSE(section.load_packed(5, palette, std::vector<u64>(256, 0)));
    // Says 4 bits, provides too few longs.
    REQUIRE_FALSE(section.load_packed(4, palette, std::vector<u64>(100, 0)));
    // Single-valued but carrying data.
    REQUIRE_FALSE(section.load_packed(0, palette, std::vector<u64>(1, 0)));
    // Single-valued with more than one palette entry.
    REQUIRE_FALSE(section.load_packed(0, std::vector<u16>{1, 2}, {}));
    // An absurd width.
    REQUIRE_FALSE(section.load_packed(65, palette, std::vector<u64>(1, 0)));
}

TEST_CASE("an index outside the palette is rejected", "[world][palette][malformed]") {
    // The dangerous one: a two-entry palette with data referring to index 5.
    // Without the check that is a read past the end of the palette, on data a
    // peer controls.
    auto                   section = PalettedContainer::blocks(0);
    const std::vector<u16> palette{10, 20};

    std::vector<u64> data(256, 0);
    data[0] = 5;  // first entry indexes past the palette

    REQUIRE_FALSE(section.load_packed(4, palette, data));
}

TEST_CASE("reading outside the section returns nothing rather than reading past",
          "[world][palette][malformed]") {
    auto section = PalettedContainer::blocks(3);
    REQUIRE(section.get(4096) == 0);
    REQUIRE(section.get(999999) == 0);

    // And writing outside is ignored rather than corrupting a neighbour.
    section.set(4096, 9);
    REQUIRE(section.kind() == PaletteKind::SingleValue);
    REQUIRE(section.get(0) == 3);
}
