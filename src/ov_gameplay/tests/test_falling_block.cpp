// Falling blocks, against what a real 1.20.1 server did.
//
// Every number frozen here was read by scripts/measure_tnt_gravity.py sand and
// powder on the vanilla jar — see docs/provenance/tnt-et-gravite.md. The
// trajectory is compared tick for tick with the falling entity's own `Time`
// and `Motion` tags, which is why it can be compared to twelve decimals.
#include "tnt_gravity_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace ov;
using namespace ov::gameplay;
using namespace ov::test;
using Catch::Approx;

#define REQUIRE_PACK()                                         \
    if (pack_blocks() == nullptr || pack_registries() == nullptr) { \
        WARN("registry.ovpack missing");                       \
        return;                                                \
    }

TEST_CASE("the blocks that fall, and the ones that do not", "[gameplay][falling]") {
    REQUIRE_PACK();
    const FallingBlocks rules{*pack_blocks(), *pack_registries()};
    for (const char* name : {"minecraft:sand", "minecraft:red_sand", "minecraft:gravel",
                             "minecraft:anvil", "minecraft:chipped_anvil",
                             "minecraft:damaged_anvil", "minecraft:dragon_egg",
                             "minecraft:suspicious_sand", "minecraft:suspicious_gravel",
                             "minecraft:white_concrete_powder", "minecraft:black_concrete_powder"}) {
        INFO(name);
        REQUIRE(rules.falls(state_of(name)));
    }
    for (const char* name : {"minecraft:stone", "minecraft:dirt", "minecraft:sandstone",
                             "minecraft:white_concrete", "minecraft:tnt"}) {
        INFO(name);
        REQUIRE_FALSE(rules.falls(state_of(name)));
    }

    // Sixteen powders, each into its own colour.
    usize powders = 0;
    for (usize i = 0; i < pack_blocks()->block_count(); ++i) {
        const registry::BlockId block{static_cast<u16>(i)};
        const std::string_view  name = pack_blocks()->block_name(block);
        if (!name.ends_with("_concrete_powder")) {
            continue;
        }
        ++powders;
        const auto concrete = rules.hardened(pack_blocks()->default_state(block));
        REQUIRE(concrete.has_value());
        REQUIRE(std::string{pack_blocks()->block_name(pack_blocks()->block_of(*concrete))} + "_powder" ==
                std::string{name});
    }
    REQUIRE(powders == 16);
    REQUIRE_FALSE(rules.hardened(state_of("minecraft:sand")).has_value());

    // What a falling block may fall into, and what it may land in.
    REQUIRE(rules.is_free(state_of("minecraft:air")));
    REQUIRE(rules.is_free(state_of("minecraft:water")));
    REQUIRE(rules.is_free(state_of("minecraft:lava")));
    REQUIRE(rules.is_free(state_of("minecraft:fire")));
    REQUIRE(rules.is_free(state_of("minecraft:grass")));
    REQUIRE_FALSE(rules.is_free(state_of("minecraft:torch")));
    REQUIRE_FALSE(rules.is_free(state_of("minecraft:stone")));
    REQUIRE(rules.replaceable(state_of("minecraft:water")));
    REQUIRE_FALSE(rules.replaceable(state_of("minecraft:torch")));
}

TEST_CASE("a falling block falls the way the Motion tag says", "[gameplay][falling][parity]") {
    REQUIRE_PACK();
    // Measured: sand set at y = -45 in open air, sampled by its own `Time`.
    struct Sample {
        i32 time;
        f64 y;
        f64 vy;
    };
    constexpr std::array<Sample, 7> kVanilla{{
        {1, -45.04, -0.0392},
        {2, -45.1192, -0.07761599999999999},
        {3, -45.236816, -0.11526368},
        {6, -45.812553324671995, -0.22374893350655997},
        {16, -49.93217661806458, -0.5413564676387079},
        {24, -55.34647297788967, -0.7530705404422061},
        {29, -59.5484319382437, -0.8690313612351256},
    }};

    TestLevel      level{*pack_blocks(), -100};
    CollisionWorld collisions{*pack_blocks(), &TestLevel::look_up, &level};
    entity::EntityState state;
    state.width    = 0.98F;
    state.height   = 0.98F;
    state.position = Vec3d{28.5, -45.0, 0.5};
    usize next     = 0;
    for (i32 time = 1; time <= 29; ++time) {
        state = step_block_entity(state, BlockEntityMotion{}, collisions);
        if (next < kVanilla.size() && kVanilla[next].time == time) {
            INFO("time " << time);
            REQUIRE(state.position.y == Approx(kVanilla[next].y).margin(1e-12));
            REQUIRE(state.velocity.y == Approx(kVanilla[next].vy).margin(1e-12));
            ++next;
        }
    }
    REQUIRE(next == kVanilla.size());
}

TEST_CASE("a column falls two ticks apart and lands whole", "[gameplay][falling][parity]") {
    REQUIRE_PACK();
    // Measured: four columns of six sand held by one stone at y = -51; the
    // stone removed; the blocks were born 2, 4, 6, 8, 10, 12 ticks after the
    // removal (read as 1, 3, 5… through `gametime - Time`, which is one tick
    // short by construction — see tnt-et-gravite.md) and ended as a column of
    // six on the floor, from -60 up.
    const FallingBlocks rules{*pack_blocks(), *pack_registries()};
    TestLevel           level{*pack_blocks(), -61};
    entity::EntityWorld world{*pack_registries()};
    Simulation          sim{level, rules, world};

    const auto sand = state_of("minecraft:sand");
    level.place(BlockPos{0, -51, 0}, state_of("minecraft:stone"));
    for (i32 k = 0; k < 6; ++k) {
        level.place(BlockPos{0, -50 + k, 0}, sand);
    }
    level.set_block(BlockPos{0, -51, 0}, registry::kAirState);  // tick 0
    sim.settle();
    for (int t = 0; t < 120; ++t) {
        sim.step();
    }

    REQUIRE(sim.births.size() == 6);
    for (usize k = 0; k < 6; ++k) {
        INFO("block " << k);
        REQUIRE(sim.births[k].tick == static_cast<i64>(2 * (k + 1)));
        REQUIRE(sim.births[k].pos.y == -50 + static_cast<i32>(k));
    }
    for (i32 y = -60; y <= -55; ++y) {
        INFO("y " << y);
        REQUIRE(level.block_at(BlockPos{0, y, 0}) == sand);
    }
    REQUIRE(level.block_at(BlockPos{0, -54, 0}) == registry::kAirState);
    REQUIRE(std::ranges::all_of(sim.landings, [](Landing l) { return l == Landing::Placed; }));
}

TEST_CASE("nothing falls until something next to it changes", "[gameplay][falling]") {
    REQUIRE_PACK();
    // Vanilla does not re-evaluate a loaded world, and neither does this: a
    // sand block floating in a save stays there. Only a change wakes it.
    const FallingBlocks rules{*pack_blocks(), *pack_registries()};
    TestLevel           level{*pack_blocks(), -61};
    entity::EntityWorld world{*pack_registries()};
    Simulation          sim{level, rules, world};
    level.place(BlockPos{5, -40, 5}, state_of("minecraft:sand"));
    for (int t = 0; t < 40; ++t) {
        sim.step();
    }
    REQUIRE(sim.births.empty());

    // A neighbour placed beside it — and the sand goes.
    level.set_block(BlockPos{6, -40, 5}, state_of("minecraft:stone"));
    for (int t = 0; t < 60; ++t) {
        sim.step();
    }
    REQUIRE(sim.births.size() == 1);
    REQUIRE(level.block_at(BlockPos{5, -60, 5}) == state_of("minecraft:sand"));
}

TEST_CASE("sand onto a torch breaks into its item", "[gameplay][falling][parity]") {
    REQUIRE_PACK();
    // Measured: the torch stayed, and one item lay on the ground.
    const FallingBlocks rules{*pack_blocks(), *pack_registries()};
    TestLevel           level{*pack_blocks(), -61};
    entity::EntityWorld world{*pack_registries()};
    Simulation          sim{level, rules, world};
    const auto torch = state_of("minecraft:torch");
    level.place(BlockPos{24, -60, 0}, torch);
    level.set_block(BlockPos{24, -50, 0}, state_of("minecraft:sand"));
    for (int t = 0; t < 80; ++t) {
        sim.step();
    }
    REQUIRE(sim.landings.size() == 1);
    REQUIRE(sim.landings[0] == Landing::Dropped);
    REQUIRE(level.block_at(BlockPos{24, -60, 0}) == torch);
    REQUIRE(rules.item_of(state_of("minecraft:sand")) == "minecraft:sand");
}

TEST_CASE("sand sinks through a pool, powder hardens at its surface", "[gameplay][falling][parity]") {
    REQUIRE_PACK();
    // Measured: a pool four deep over glass. Sand went to the floor (-60) and
    // left the water above it; red concrete powder hardened in the top water
    // cell (-57) and left the three below it.
    const FallingBlocks rules{*pack_blocks(), *pack_registries()};
    TestLevel           level{*pack_blocks(), -61};
    entity::EntityWorld world{*pack_registries()};
    Simulation          sim{level, rules, world};
    const auto water = state_of("minecraft:water");
    const auto glass = state_of("minecraft:glass");
    for (const i32 px : {28, 32}) {
        for (i32 y = -61; y <= -57; ++y) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                for (i32 dz = -1; dz <= 1; ++dz) {
                    level.place(BlockPos{px + dx, y, dz}, glass);
                }
            }
        }
        for (i32 y = -60; y <= -57; ++y) {
            level.place(BlockPos{px, y, 0}, water);
        }
    }
    level.set_block(BlockPos{28, -45, 0}, state_of("minecraft:sand"));
    level.set_block(BlockPos{32, -45, 0}, state_of("minecraft:red_concrete_powder"));
    for (int t = 0; t < 120; ++t) {
        sim.step();
    }
    REQUIRE(level.block_at(BlockPos{28, -60, 0}) == state_of("minecraft:sand"));
    for (i32 y = -59; y <= -57; ++y) {
        REQUIRE(level.block_at(BlockPos{28, y, 0}) == water);
    }
    REQUIRE(level.block_at(BlockPos{32, -57, 0}) == state_of("minecraft:red_concrete"));
    for (i32 y = -60; y <= -58; ++y) {
        REQUIRE(level.block_at(BlockPos{32, y, 0}) == water);
    }
}

TEST_CASE("powder hardens against five faces of water and falls into the sixth",
          "[gameplay][falling][parity]") {
    REQUIRE_PACK();
    // Measured one face at a time, the water placed last: above, north,
    // south, west and east each gave white concrete in place; with the water
    // below, the powder fell into it and hardened there, one cell down.
    const FallingBlocks rules{*pack_blocks(), *pack_registries()};
    const auto powder   = state_of("minecraft:white_concrete_powder");
    const auto concrete = state_of("minecraft:white_concrete");
    const auto water    = state_of("minecraft:water");
    const auto stone    = state_of("minecraft:stone");

    for (u8 face = 0; face < kDirectionCount; ++face) {
        const auto d = static_cast<Direction>(face);
        INFO("face " << static_cast<int>(face));
        TestLevel           level{*pack_blocks(), -61};
        entity::EntityWorld world{*pack_registries()};
        Simulation          sim{level, rules, world};
        const BlockPos      at{0, -45, 0};
        level.place(at, powder);
        // Walls round the powder and the water cell, a floor under both.
        const BlockPos wet = at.offset(d);
        for (const BlockPos cell : {at, wet}) {
            for (u8 side = 0; side < kDirectionCount; ++side) {
                const BlockPos n = cell.offset(static_cast<Direction>(side));
                if (n != at && n != wet) {
                    level.place(n, stone);
                }
            }
        }
        level.set_block(wet, water);
        for (int t = 0; t < 40; ++t) {
            sim.step();
        }
        if (d == Direction::Down) {
            REQUIRE(level.block_at(at) == registry::kAirState);
            REQUIRE(level.block_at(wet) == concrete);
        } else {
            REQUIRE(level.block_at(at) == concrete);
        }
    }

    // The control: no water anywhere, and it stays powder.
    TestLevel level{*pack_blocks(), -61};
    level.place(BlockPos{0, -45, 0}, powder);
    level.place(BlockPos{0, -46, 0}, stone);
    REQUIRE_FALSE(rules.neighbour_changed(level, BlockPos{0, -45, 0}));
    REQUIRE(level.block_at(BlockPos{0, -45, 0}) == powder);
}

TEST_CASE("a suspicious block that cannot land leaves nothing", "[gameplay][falling]") {
    REQUIRE_PACK();
    const FallingBlocks rules{*pack_blocks(), *pack_registries()};
    TestLevel           level{*pack_blocks(), -61};
    level.place(BlockPos{0, -60, 0}, state_of("minecraft:torch"));
    REQUIRE(rules.land(level, BlockPos{0, -60, 0}, state_of("minecraft:suspicious_sand"), false) ==
            Landing::Vanished);
    REQUIRE(rules.land(level, BlockPos{0, -60, 0}, state_of("minecraft:gravel"), false) ==
            Landing::Dropped);
    REQUIRE_FALSE(FallingBlocks::unimplemented().empty());
}
