#include "ov/registry/block_states.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::registry;

namespace {

/// Where tools/ov_datagen writes the pack. Generated, never committed — it is
/// derived from Mojang's own reports.
[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

/// Load once for the whole file; it is 131 KB and immutable.
[[nodiscard]] const BlockRegistry* loaded_registry() {
    static const auto loaded = BlockRegistry::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

}  // namespace

TEST_CASE("the registry pack loads", "[registry][blockstates]") {
    if (loaded_registry() == nullptr) {
        // Skipped rather than failed: the pack is generated from a server jar
        // the user supplies, and a fresh clone will not have one.
        WARN(
            "registry.ovpack not present — run tools/ov_datagen/datagen.py "
            "then tools/ov_datagen/ovpack.py");
        SUCCEED();
        return;
    }

    REQUIRE(loaded_registry()->block_count() == 1003);
    REQUIRE(loaded_registry()->state_count() == 24135);
}

TEST_CASE("air is state 0", "[registry][blockstates]") {
    if (loaded_registry() == nullptr) {
        SUCCEED();
        return;
    }
    // Relied on everywhere: a zeroed chunk section is an empty one, so
    // memset(0) has to mean air rather than something solid.
    const auto air = loaded_registry()->find_block("minecraft:air");
    REQUIRE(air.has_value());
    REQUIRE(loaded_registry()->first_state(*air) == kAirState);
    REQUIRE(loaded_registry()->default_state(*air) == kAirState);
    REQUIRE(loaded_registry()->block_of(kAirState) == *air);
}

TEST_CASE("known blocks have the state counts they should", "[registry][blockstates]") {
    if (loaded_registry() == nullptr) {
        SUCCEED();
        return;
    }
    const auto* reg = loaded_registry();

    const auto door = reg->find_block("minecraft:oak_door");
    REQUIRE(door.has_value());
    // facing(4) x half(2) x hinge(2) x open(2) x powered(2)
    REQUIRE(reg->state_count(*door) == 64);
    REQUIRE(reg->properties(*door).size() == 5);

    const auto wire = reg->find_block("minecraft:redstone_wire");
    REQUIRE(wire.has_value());
    REQUIRE(reg->state_count(*wire) == 1296);  // the most of any block

    const auto stone = reg->find_block("minecraft:stone");
    REQUIRE(stone.has_value());
    REQUIRE(reg->state_count(*stone) == 1);
    REQUIRE(reg->properties(*stone).empty());

    REQUIRE_FALSE(reg->find_block("minecraft:not_a_block").has_value());
}

TEST_CASE("every state decomposes and recomposes to itself", "[registry][blockstates]") {
    if (loaded_registry() == nullptr) {
        SUCCEED();
        return;
    }
    const auto* reg = loaded_registry();

    // The invariant the whole design rests on: a state is a mixed-radix number
    // over its block's properties. Reading every digit out and writing every
    // digit back has to return the same state, for all 24135 of them. If the
    // strides are wrong for even one block, this finds it.
    usize checked = 0;

    for (u16 state_value = 0; state_value < reg->state_count(); ++state_value) {
        const BlockStateId state{state_value};
        const BlockId      block = reg->block_of(state);

        // The state must fall inside its block's declared range, and the range
        // must be contiguous — the arithmetic assumes both.
        const u16 base = reg->first_state(block).value();
        REQUIRE(state_value >= base);
        REQUIRE(state_value < base + reg->state_count(block));

        BlockStateId rebuilt{base};
        for (const PropertyView& property : reg->properties(block)) {
            rebuilt = reg->with_property(rebuilt, property, reg->property_index(state, property));
        }
        REQUIRE(rebuilt == state);
        ++checked;
    }

    REQUIRE(checked == 24135);
}

TEST_CASE("the state ranges tile the whole space exactly once", "[registry][blockstates]") {
    if (loaded_registry() == nullptr) {
        SUCCEED();
        return;
    }
    const auto* reg = loaded_registry();

    // No gaps and no overlaps. A gap means a state belongs to nothing; an
    // overlap means two blocks claim the same id, and the client would show
    // one of them.
    std::vector<bool> seen(reg->state_count(), false);

    for (u16 i = 0; i < reg->block_count(); ++i) {
        const BlockId block = BlockId{i};
        const u16     base  = reg->first_state(block).value();
        const u16     count = reg->state_count(block);
        REQUIRE(count > 0);

        for (u16 offset = 0; offset < count; ++offset) {
            const usize index = base + offset;
            REQUIRE(index < seen.size());
            REQUIRE_FALSE(seen[index]);
            seen[index] = true;
        }
    }

    REQUIRE(std::ranges::all_of(seen, [](bool v) { return v; }));
}

TEST_CASE("changing a property costs one multiply and one add", "[registry][blockstates]") {
    if (loaded_registry() == nullptr) {
        SUCCEED();
        return;
    }
    const auto* reg = loaded_registry();

    const auto door = reg->find_block("minecraft:oak_door");
    REQUIRE(door.has_value());

    const auto open = reg->find_property(*door, "open");
    REQUIRE(open.has_value());
    REQUIRE(open->values.size() == 2);
    // Vanilla lists boolean properties true first.
    REQUIRE(open->values[0] == "true");
    REQUIRE(open->values[1] == "false");

    const std::vector<std::pair<std::string_view, std::string_view>> shut{{"open", "false"}};
    const BlockStateId closed = reg->state_for(*door, shut).value_or(kAirState);

    REQUIRE(reg->property_value(closed, *open) == "false");

    const BlockStateId opened = reg->with_property(closed, *open, 0);
    REQUIRE(reg->property_value(opened, *open) == "true");
    // The two differ by exactly one stride.
    REQUIRE(closed.value() - opened.value() == open->stride);

    // And the block is unchanged, which is what keeps this from wandering into
    // another block's range.
    REQUIRE(reg->block_of(opened) == *door);
}

TEST_CASE("a state can be built from a name and properties", "[registry][blockstates]") {
    if (loaded_registry() == nullptr) {
        SUCCEED();
        return;
    }
    const auto* reg = loaded_registry();

    // This is how an Anvil palette entry and a blockstate file are read.
    const auto door = reg->find_block("minecraft:oak_door");
    REQUIRE(door.has_value());

    const std::vector<std::pair<std::string_view, std::string_view>> props{{"facing", "east"},
                                                                           {"half", "lower"},
                                                                           {"hinge", "right"},
                                                                           {"open", "true"},
                                                                           {"powered", "false"}};

    const auto state = reg->state_for(*door, props);
    REQUIRE(state.has_value());
    REQUIRE(reg->block_of(*state) == *door);

    for (const auto& [name, value] : props) {
        const auto property = reg->find_property(*door, name);
        REQUIRE(property.has_value());
        REQUIRE(reg->property_value(*state, *property) == value);
    }

    // An unknown property or value is rejected rather than guessed at: a
    // palette entry we cannot represent must not silently become a different
    // block.
    const std::vector<std::pair<std::string_view, std::string_view>> bad_name{{"nope", "true"}};
    REQUIRE_FALSE(reg->state_for(*door, bad_name).has_value());

    const std::vector<std::pair<std::string_view, std::string_view>> bad_value{
        {"facing", "upwards"}};
    REQUIRE_FALSE(reg->state_for(*door, bad_value).has_value());
}

TEST_CASE("the four blocks whose file order lies are handled", "[registry][blockstates]") {
    if (loaded_registry() == nullptr) {
        SUCCEED();
        return;
    }
    const auto* reg = loaded_registry();

    // chest, trapped_chest, moving_piston and piston_head list their properties
    // in blocks.json in an order that is NOT the one the state ids are computed
    // in. Trusting the file gives wrong ids for these four and no others, which
    // nothing would catch until a client showed the wrong block.
    //
    // The strides prove which order the data actually uses: for a chest,
    // facing(4) is more significant than type(3), which is more significant
    // than waterlogged(2).
    const auto chest = reg->find_block("minecraft:chest");
    REQUIRE(chest.has_value());

    const auto facing      = reg->find_property(*chest, "facing");
    const auto type        = reg->find_property(*chest, "type");
    const auto waterlogged = reg->find_property(*chest, "waterlogged");
    REQUIRE(facing.has_value());
    REQUIRE(type.has_value());
    REQUIRE(waterlogged.has_value());

    REQUIRE(waterlogged->stride == 1);
    REQUIRE(type->stride == 2);    // above waterlogged's 2 values
    REQUIRE(facing->stride == 6);  // above type's 3 x waterlogged's 2
    REQUIRE(reg->state_count(*chest) == 24);
}

TEST_CASE("an out-of-range state does not read out of bounds",
          "[registry][blockstates][malformed]") {
    if (loaded_registry() == nullptr) {
        SUCCEED();
        return;
    }
    const auto* reg = loaded_registry();

    // Chunk palettes come from disk and from the network, so a state id can be
    // anything at all.
    REQUIRE_FALSE(reg->is_valid(BlockStateId{65535}));
    REQUIRE(reg->block_of(BlockStateId{65535}) == BlockId{0});
    REQUIRE(reg->is_valid(BlockStateId{24134}));
    REQUIRE_FALSE(reg->is_valid(BlockStateId{24135}));
}

TEST_CASE("a corrupt pack is rejected", "[registry][blockstates][malformed]") {
    REQUIRE(BlockRegistry::from_bytes({}).error() == RegistryError::Corrupt);
    REQUIRE(BlockRegistry::from_bytes(std::vector<u8>(10, 0)).error() == RegistryError::Corrupt);

    // Right size, wrong magic.
    std::vector<u8> wrong_magic(200, 0);
    wrong_magic[0] = 'N';
    REQUIRE(BlockRegistry::from_bytes(wrong_magic).error() == RegistryError::Corrupt);

    // Right magic, wrong version — a stale cache read as current would produce
    // plausible, wrong ids.
    std::vector<u8> wrong_version(200, 0);
    wrong_version[0] = 'O';
    wrong_version[1] = 'V';
    wrong_version[2] = 'P';
    wrong_version[3] = 'K';
    wrong_version[4] = 99;
    REQUIRE(BlockRegistry::from_bytes(wrong_version).error() == RegistryError::VersionMismatch);
}
