// Tool wear, against what a real 1.20.1 server reported.
//
// The maxima were bisected — an item handed over at `Damage: N` either survives
// one more action or does not — and the boundary confirmed three times on each
// side. The costs per action were read straight off `Damage` after one action
// on a pristine tool.
#include "ov/gameplay/durability.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::gameplay;

TEST_CASE("every tier's maximum is the one that was measured",
          "[gameplay][durability]") {
    // Gold is the odd one out, and in the opposite direction from mining: the
    // fastest tier and the shortest-lived, at 32 against wood's 59.
    REQUIRE(max_damage("minecraft:wooden_pickaxe") == 59);
    REQUIRE(max_damage("minecraft:stone_pickaxe") == 131);
    REQUIRE(max_damage("minecraft:iron_pickaxe") == 250);
    REQUIRE(max_damage("minecraft:golden_pickaxe") == 32);
    REQUIRE(max_damage("minecraft:diamond_pickaxe") == 1561);
    REQUIRE(max_damage("minecraft:netherite_pickaxe") == 2031);

    // Every family of a tier shares it. Measured family by family rather than
    // assumed: five families times six materials is thirty rows and every one
    // of them was bisected.
    for (const char* family : {"pickaxe", "shovel", "axe", "hoe", "sword"}) {
        const std::string wooden = std::string{"minecraft:wooden_"} + family;
        const std::string netherite = std::string{"minecraft:netherite_"} + family;
        INFO(family);
        REQUIRE(max_damage(wooden) == 59);
        REQUIRE(max_damage(netherite) == 2031);
    }

    // An item with no durability answers nothing, not zero. Zero would destroy
    // a stack of cobble the first time it was swung.
    REQUIRE_FALSE(max_damage("minecraft:cobblestone").has_value());
    REQUIRE_FALSE(max_damage("").has_value());
}

TEST_CASE("a sword and a tool cost each other's price", "[gameplay][durability]") {
    // The asymmetry is the measurement. A sword costs two to dig and one to
    // hit; every other tool costs one to dig and two to hit. The game charges
    // double for a tool used as something it is not.
    REQUIRE(action_damage("minecraft:diamond_sword", ToolAction::BreakBlock) == 2);
    REQUIRE(action_damage("minecraft:diamond_sword", ToolAction::Attack) == 1);
    REQUIRE(action_damage("minecraft:diamond_pickaxe", ToolAction::BreakBlock) == 1);
    REQUIRE(action_damage("minecraft:diamond_pickaxe", ToolAction::Attack) == 2);
    REQUIRE(action_damage("minecraft:diamond_hoe", ToolAction::Attack) == 2);

    // A block that comes off instantly costs nothing: measured on a torch.
    REQUIRE(action_damage("minecraft:diamond_pickaxe", ToolAction::BreakInstantBlock) == 0);

    // Shears hit for free. Measured, and not an omission.
    REQUIRE(action_damage("minecraft:shears", ToolAction::Attack) == 0);
    REQUIRE(action_damage("minecraft:shears", ToolAction::BreakBlock) == 1);

    // Tilling, lighting, stripping, path-making: one each.
    REQUIRE(action_damage("minecraft:diamond_hoe", ToolAction::UseOnBlock) == 1);

    // An item with no durability costs nothing whatever it is asked to do.
    REQUIRE(action_damage("minecraft:cobblestone", ToolAction::BreakBlock) == 0);
}

TEST_CASE("a tool breaks on the action that reaches its maximum",
          "[gameplay][durability]") {
    math::XoroshiroRandomSource random{1};

    // A golden pickaxe has 32 points. At 30 it survives; at 31 the next point
    // reaches 32 and it is gone.
    const WearResult survives =
        wear("minecraft:golden_pickaxe", 30, ToolAction::BreakBlock, 0, random);
    REQUIRE(survives.damage == 31);
    REQUIRE_FALSE(survives.broke);

    const WearResult gone =
        wear("minecraft:golden_pickaxe", 31, ToolAction::BreakBlock, 0, random);
    REQUIRE(gone.broke);

    // A sword wears by two, so its last survivor is three below the maximum and
    // not two. That is the difference between a wooden sword with 59 uses and
    // one with 58.
    REQUIRE_FALSE(wear("minecraft:wooden_sword", 56, ToolAction::BreakBlock, 0, random).broke);
    REQUIRE(wear("minecraft:wooden_sword", 57, ToolAction::BreakBlock, 0, random).broke);

    // A pristine golden pickaxe has exactly its maximum in uses.
    i32 damage = 0;
    i32 uses   = 0;
    while (true) {
        const WearResult step =
            wear("minecraft:golden_pickaxe", damage, ToolAction::BreakBlock, 0, random);
        ++uses;
        if (step.broke) {
            break;
        }
        damage = step.damage;
        REQUIRE(uses < 100);
    }
    REQUIRE(uses == 32);
}

TEST_CASE("an item with no durability is left alone", "[gameplay][durability]") {
    math::XoroshiroRandomSource random{2};
    const WearResult out = wear("minecraft:cobblestone", 0, ToolAction::Attack, 0, random);
    REQUIRE_FALSE(out.broke);
    REQUIRE(out.applied == 0);
    REQUIRE(out.damage == 0);
}

TEST_CASE("unbreaking makes points miss rather than shrink",
          "[gameplay][durability]") {
    // Not confronted against the server — the campaign for it was not run — so
    // what is checked here is the *shape*: the draws are taken one per point,
    // the chance a point is ignored is level/(level+1), and a hundred thousand
    // draws land near it.
    math::XoroshiroRandomSource random{7};

    i32 landed = 0;
    for (int i = 0; i < 100000; ++i) {
        landed += unbreaking_survives(1, 3, false, random);
    }
    // A quarter of the points should land: 3 in 4 are ignored at level III.
    REQUIRE(landed > 24000);
    REQUIRE(landed < 26000);

    // Armour has a 60% gate in front of the same draw, so far more points land.
    i32 armour = 0;
    for (int i = 0; i < 100000; ++i) {
        armour += unbreaking_survives(1, 3, true, random);
    }
    REQUIRE(armour > landed);

    // Level zero is the identity, and it takes no draws at all.
    REQUIRE(unbreaking_survives(5, 0, false, random) == 5);
}
