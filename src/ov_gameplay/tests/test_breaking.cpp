#include "ov/gameplay/breaking.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string_view>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

struct Loaded {
    std::optional<registry::BlockRegistry> blocks;
    std::optional<registry::Registries>    registries;
    std::optional<BreakRules>              rules;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        Loaded out;
        auto   blocks = registry::BlockRegistry::load(pack_path());
        auto   regs   = registry::Registries::load(pack_path());
        if (blocks && regs) {
            out.blocks     = std::move(*blocks);
            out.registries = std::move(*regs);
            out.rules.emplace(*out.blocks, *out.registries);
        }
        return out;
    }();
    return state;
}

[[nodiscard]] registry::BlockStateId block(std::string_view name) {
    const auto id = loaded().blocks->find_block(name);
    REQUIRE(id.has_value());
    return loaded().blocks->default_state(*id);
}

[[nodiscard]] Held holding(std::string_view name, u8 efficiency = 0) {
    const auto items = loaded().registries->find("minecraft:item");
    REQUIRE(items.has_value());
    const auto id = loaded().registries->protocol_id(*items, name);
    REQUIRE(id.has_value());
    return Held{*id, efficiency};
}

}  // namespace

// Every number below was counted on a real 1.20.1 server, not derived from the
// formula it is checking. The server does not break a block by itself: it waits
// for the client to claim it is done and then checks the claim, and a claim made
// immediately falls under the 0.7 threshold, at which point the server uses its
// own clock and breaks the block on the tick vanilla would.

TEST_CASE("break times match the ones counted on a real server", "[gameplay][breaking]") {
    if (!loaded().rules) {
        SKIP("no registry pack");
    }
    const BreakRules& rules = *loaded().rules;
    const Stance      standing{};

    REQUIRE(rules.break_ticks(block("minecraft:dirt"), Held{}, standing) == 15);
    REQUIRE(rules.break_ticks(block("minecraft:glass"), Held{}, standing) == 9);
    REQUIRE(rules.break_ticks(block("minecraft:white_wool"), Held{}, standing) == 24);
    REQUIRE(rules.break_ticks(block("minecraft:oak_leaves"), Held{}, standing) == 6);
    REQUIRE(rules.break_ticks(block("minecraft:oak_planks"), Held{}, standing) == 60);

    // Stone demands a pickaxe: bare-handed it is the hundred divisor, five
    // times the thirty a proper tool gets.
    REQUIRE(rules.break_ticks(block("minecraft:stone"), Held{}, standing) == 150);
    REQUIRE(rules.break_ticks(block("minecraft:stone"), holding("minecraft:wooden_pickaxe"),
                              standing) == 23);

    // The result that no reasoning produces: a netherite shovel outranks every
    // tier there is and still cannot harvest stone, so it is as slow as no tool
    // at all. The family gates harvesting, not the tier alone.
    REQUIRE(rules.break_ticks(block("minecraft:stone"), holding("minecraft:netherite_shovel"),
                              standing) == 150);

    // Iron ore adds the other half of the rule: right family, tier too low.
    REQUIRE(rules.break_ticks(block("minecraft:iron_ore"), holding("minecraft:wooden_pickaxe"),
                              standing) == 150);
    REQUIRE(rules.break_ticks(block("minecraft:iron_ore"), holding("minecraft:stone_pickaxe"),
                              standing) == 23);

    REQUIRE(rules.break_ticks(block("minecraft:obsidian"), holding("minecraft:iron_pickaxe"),
                              standing) == 834);
    REQUIRE(rules.break_ticks(block("minecraft:obsidian"), holding("minecraft:netherite_pickaxe"),
                              standing) == 167);

    // Gold is the odd tier: fastest of them all and yet no better than wood at
    // deciding what drops. Both halves were measured — 417 ticks on obsidian
    // leaves 12 as the only possible speed, and 25 on iron ore is the
    // hundred divisor, so its harvest level is wood's.
    REQUIRE(rules.break_ticks(block("minecraft:obsidian"), holding("minecraft:golden_pickaxe"),
                              standing) == 417);
    REQUIRE(rules.break_ticks(block("minecraft:iron_ore"), holding("minecraft:golden_pickaxe"),
                              standing) == 25);

    // Bedrock never breaks, and that is the server's own answer: it was mined
    // for eighteen seconds and did nothing.
    REQUIRE(rules.break_ticks(block("minecraft:bedrock"), Held{}, standing) == -1);
}

TEST_CASE("efficiency adds level squared plus one", "[gameplay][breaking]") {
    if (!loaded().rules) {
        SKIP("no registry pack");
    }
    const BreakRules& rules = *loaded().rules;
    const Stance      standing{};
    const auto        obsidian = block("minecraft:obsidian");

    // 9 + 2, 9 + 10, 9 + 26 — counted at levels I, III and V.
    REQUIRE(rules.break_ticks(obsidian, holding("minecraft:netherite_pickaxe", 1), standing) ==
            137);
    REQUIRE(rules.break_ticks(obsidian, holding("minecraft:netherite_pickaxe", 3), standing) == 79);
    REQUIRE(rules.break_ticks(obsidian, holding("minecraft:netherite_pickaxe", 5), standing) == 43);

    // And it does nothing to a tool that was not helping in the first place.
    REQUIRE(rules.break_ticks(block("minecraft:stone"), holding("minecraft:netherite_shovel", 5),
                              standing) == 150);
}

TEST_CASE("harvesting needs the right family and a sufficient tier", "[gameplay][breaking]") {
    if (!loaded().rules) {
        SKIP("no registry pack");
    }
    const BreakRules& rules = *loaded().rules;

    REQUIRE(rules.can_harvest(block("minecraft:dirt"), Held{}));
    REQUIRE_FALSE(rules.can_harvest(block("minecraft:stone"), Held{}));
    REQUIRE(rules.can_harvest(block("minecraft:stone"), holding("minecraft:wooden_pickaxe")));
    REQUIRE_FALSE(
        rules.can_harvest(block("minecraft:stone"), holding("minecraft:netherite_shovel")));
    REQUIRE_FALSE(
        rules.can_harvest(block("minecraft:iron_ore"), holding("minecraft:wooden_pickaxe")));
    REQUIRE(rules.can_harvest(block("minecraft:iron_ore"), holding("minecraft:stone_pickaxe")));
    REQUIRE_FALSE(
        rules.can_harvest(block("minecraft:obsidian"), holding("minecraft:iron_pickaxe")));
    REQUIRE(rules.can_harvest(block("minecraft:obsidian"), holding("minecraft:diamond_pickaxe")));
}

TEST_CASE("standing still matters as much as the tool", "[gameplay][breaking]") {
    if (!loaded().rules) {
        SKIP("no registry pack");
    }
    const BreakRules& rules = *loaded().rules;
    const auto        stone = block("minecraft:stone");
    const Held        pick  = holding("minecraft:wooden_pickaxe");

    // Five times slower off the ground, five times again with a flooded head —
    // the two divisors compound, which is why mining while swimming upwards is
    // twenty-five times slower and feels broken.
    REQUIRE(rules.break_ticks(stone, pick, Stance{}) == 23);
    REQUIRE(rules.break_ticks(stone, pick, Stance{.on_ground = false}) == 113);
    REQUIRE(rules.break_ticks(stone, pick, Stance{.head_in_water = true}) == 113);
    REQUIRE(rules.break_ticks(stone, pick, Stance{.head_in_water = true, .aqua_affinity = true}) ==
            23);
    REQUIRE(rules.break_ticks(stone, pick, Stance{.on_ground = false, .head_in_water = true}) ==
            563);
}

// ── Under effects ───────────────────────────────────────────────────────────
//
// Counted on the real server by scripts/measure_effects.py (campaign
// `breaking`), each case twice, with the method above. Conduit power is not a
// field of its own: measured, it digs exactly like haste, and conduit II with
// haste I digs like haste II — the larger of the two amplifiers. The server
// passes `max(haste, conduit)` in `Stance::haste`.
//
// Two of the thirty-two counts were a tick short on one of their two attempts
// (stone with a wooden pickaxe under haste II: 16 and 17; dirt by hand: 14 and
// 15). That is the ±1 of the miner's own start, the jitter PROVENANCE.md
// describes for the hardness sweep; the other attempt is the one asserted.
TEST_CASE("breaking under haste, conduit power and mining fatigue",
          "[gameplay][breaking][effects][parity]") {
    if (!loaded().rules) {
        SKIP("registry.ovpack not generated");
    }
    const BreakRules& rules = *loaded().rules;
    struct Case {
        std::string_view block;
        std::string_view tool;
        u8               efficiency;
        i8               haste;
        i8               fatigue;
        i32              ticks;
    };
    const Case cases[] = {
        {"minecraft:stone", "", 0, -1, -1, 150},
        {"minecraft:stone", "", 0, 0, -1, 125},
        {"minecraft:stone", "", 0, 1, -1, 108},
        {"minecraft:stone", "", 0, 2, -1, 94},
        {"minecraft:stone", "", 0, 4, -1, 75},
        {"minecraft:stone", "", 0, 0, -1, 125},  // conduit power I
        {"minecraft:stone", "", 0, 1, -1, 108},  // conduit power II + haste I
        {"minecraft:stone", "minecraft:wooden_pickaxe", 0, 1, -1, 17},
        {"minecraft:obsidian", "minecraft:netherite_pickaxe", 0, 1, -1, 120},
        {"minecraft:dirt", "", 0, -1, -1, 15},
        {"minecraft:dirt", "", 0, -1, 0, 50},
        {"minecraft:dirt", "", 0, -1, 1, 167},
        {"minecraft:dirt", "minecraft:netherite_shovel", 0, -1, 2, 618},
        {"minecraft:dirt", "minecraft:netherite_shovel", 5, -1, 3, 530},
        {"minecraft:dirt", "", 0, 1, 0, 36},
        {"minecraft:oak_planks", "", 0, 2, -1, 38},
    };
    usize agree = 0;
    for (const Case& c : cases) {
        const Held   held = c.tool.empty() ? Held{} : holding(c.tool, c.efficiency);
        const Stance stance{.on_ground = true, .haste = c.haste, .mining_fatigue = c.fatigue};
        const i32    ticks = rules.break_ticks(block(c.block), held, stance);
        CHECK(ticks == c.ticks);
        agree += ticks == c.ticks ? 1 : 0;
    }
    CHECK(agree == 16);
}
