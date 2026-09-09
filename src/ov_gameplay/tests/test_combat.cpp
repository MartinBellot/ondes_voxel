// Melee, against the numbers a real 1.20.1 server produced.
//
// The tables below are measurements, not expectations: every one was read off
// the server by scripts/measure_combat.py and is reproduced here so that a
// change to the rules fails a test rather than a play session.
#include "ov/gameplay/combat.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace ov;
using namespace ov::gameplay;
using Catch::Approx;

namespace {

/// The gauge, measured on a diamond sword — 7 damage, attack speed 1.6, so a
/// cooldown of 12.5 ticks. One row per tick from a swing on the same tick as
/// the last one up to a full charge.
///
/// The staircase was sampled every 25 ms, which is half a tick, so both sides
/// of each riser were seen. Twelve distinct values came back and these are
/// they.
struct GaugeRow {
    i32 ticks;
    f32 damage;
};

constexpr GaugeRow kDiamondSwordGauge[] = {
    {0, 1.4089F},  {1, 1.4806F}, {2, 1.6240F},  {3, 1.8391F},  {4, 2.1257F},  {5, 2.4841F},
    {6, 2.9143F},  {7, 3.4160F}, {8, 3.9894F},  {9, 4.6346F},  {10, 5.3514F}, {11, 6.1398F},
    {12, 7.0000F}, {13, 7.0000F},
};

/// The same for a diamond axe — 9 damage, attack speed 1.0, cooldown 20 ticks.
/// Only the ends and a middle point, because the sword already pins the shape
/// and what this checks is that the *cooldown* moves with the weapon.
constexpr GaugeRow kDiamondAxeGauge[] = {
    {0, 1.8045F}, {1, 1.8405F}, {2, 1.9125F}, {19, 8.6445F}, {20, 9.0000F},
};

AttackerState resting() {
    AttackerState attacker;
    attacker.strength_ticker = 100;
    return attacker;
}

}  // namespace

TEST_CASE("the weapon table is the one the server reported", "[gameplay][combat]") {
    // Three rows that are each a trap. Gold is wood's damage while being the
    // fastest mining tier; the axes' speeds are not one series; and a hoe hits
    // for exactly what a bare hand does at every tier.
    REQUIRE(weapon_for("minecraft:golden_sword").attack_damage == 4.0F);
    REQUIRE(weapon_for("minecraft:wooden_sword").attack_damage == 4.0F);
    REQUIRE(weapon_for("minecraft:iron_axe").attack_speed == 0.9F);
    REQUIRE(weapon_for("minecraft:stone_axe").attack_speed == 0.8F);
    REQUIRE(weapon_for("minecraft:diamond_axe").attack_speed == 1.0F);
    REQUIRE(weapon_for("minecraft:netherite_hoe").attack_damage == 1.0F);
    REQUIRE(weapon_for("minecraft:netherite_hoe").attack_speed == 4.0F);

    // Anything with no modifiers is the bare hand, which is itself measured:
    // a stick, a pair of shears and an empty hand all answered 1.0 and 4.0.
    REQUIRE(weapon_for("minecraft:stick").attack_damage == 1.0F);
    REQUIRE(weapon_for("").attack_speed == 4.0F);
    REQUIRE(weapon_for("minecraft:cobblestone").attack_damage == 1.0F);

    REQUIRE(weapon_table().size() == 31);
}

TEST_CASE("the cooldown is twenty over the attack speed", "[gameplay][combat]") {
    REQUIRE(attack_cooldown_ticks(1.6F) == Approx(12.5));
    REQUIRE(attack_cooldown_ticks(1.0F) == Approx(20.0));
    REQUIRE(attack_cooldown_ticks(4.0F) == Approx(5.0));
}

TEST_CASE("the attack gauge reproduces every measured step", "[gameplay][combat]") {
    Weapon sword{"minecraft:diamond_sword"};
    for (const GaugeRow& row : kDiamondSwordGauge) {
        AttackerState attacker;
        attacker.strength_ticker = row.ticks;
        const AttackOutcome out  = resolve_attack(sword, attacker, CombatConstants{});
        INFO("diamond sword at " << row.ticks << " ticks");
        // Three ten-thousandths, which is what the server's own printed float
        // supports: a health of 1022.5911 read back as a difference of 1.4089
        // where the exact value is 1.40896. Loose enough for that, and an
        // order of magnitude tighter than any wrong formula — dropping the
        // half-tick offset moves this row by 0.009.
        REQUIRE(out.damage == Approx(row.damage).margin(3.0e-4));
    }

    Weapon axe{"minecraft:diamond_axe"};
    for (const GaugeRow& row : kDiamondAxeGauge) {
        AttackerState attacker;
        attacker.strength_ticker = row.ticks;
        const AttackOutcome out  = resolve_attack(axe, attacker, CombatConstants{});
        INFO("diamond axe at " << row.ticks << " ticks");
        REQUIRE(out.damage == Approx(row.damage).margin(5.0e-5));
    }
}

TEST_CASE("a swing on the same tick is not a fifth of the damage", "[gameplay][combat]") {
    // The half-tick offset is the whole of this test. Without it a diamond
    // sword at ticker zero does exactly 1.4, which is wrong by a thousandth and
    // wrong in a way no play session would ever show.
    Weapon        sword{"minecraft:diamond_sword"};
    AttackerState attacker;
    attacker.strength_ticker = 0;
    const AttackOutcome out  = resolve_attack(sword, attacker, CombatConstants{});
    REQUIRE(out.damage != Approx(1.4).margin(1.0e-3));
    REQUIRE(out.damage == Approx(1.4089).margin(3.0e-4));
}

TEST_CASE("a fully charged hit removes the attack damage attribute exactly",
          "[gameplay][combat]") {
    // The cross-check the campaign made: attribute and hit agreed on all
    // thirty-four items. Four of them here, spread across the tiers.
    for (const char* item : {"minecraft:netherite_axe", "minecraft:diamond_sword",
                             "minecraft:golden_shovel", "minecraft:trident"}) {
        const AttackOutcome out = resolve_attack(Weapon{item}, resting(), CombatConstants{});
        INFO(item);
        REQUIRE(out.damage == Approx(weapon_for(item).attack_damage));
    }
}

TEST_CASE("a critical needs all six of its conditions", "[gameplay][combat]") {
    const CombatConstants constants;
    Weapon                sword{"minecraft:diamond_sword"};

    AttackerState falling  = resting();
    falling.on_ground      = false;
    falling.fall_distance  = 1.0F;
    const AttackOutcome hit = resolve_attack(sword, falling, constants);
    REQUIRE(hit.critical);
    REQUIRE(hit.damage == Approx(10.5));  // 7 * 1.5

    // Each disqualifier alone is enough.
    for (int which = 0; which < 6; ++which) {
        AttackerState broken = falling;
        switch (which) {
            case 0: broken.on_ground = true; break;
            case 1: broken.fall_distance = 0.0F; break;
            case 2: broken.in_water = true; break;
            case 3: broken.on_climbable = true; break;
            case 4: broken.blind = true; break;
            case 5: broken.sprinting = true; break;
            default: break;
        }
        INFO("disqualifier " << which);
        REQUIRE_FALSE(resolve_attack(sword, broken, constants).critical);
    }

    // An uncharged swing is never critical however far the attacker is falling.
    AttackerState early    = falling;
    early.strength_ticker  = 0;
    REQUIRE_FALSE(resolve_attack(sword, early, constants).critical);
}

TEST_CASE("the sweep is a sword's, charged, standing and not critical",
          "[gameplay][combat]") {
    const CombatConstants constants;

    // Measured: a charged diamond sword put 7 into the mob it hit and exactly
    // 1.0 into the one beside it; a diamond axe put 9 in and nothing beside.
    const AttackOutcome sword = resolve_attack(Weapon{"minecraft:diamond_sword"}, resting(),
                                               constants);
    REQUIRE(sword.sweeping);
    REQUIRE(sword.sweep_damage == Approx(1.0));

    REQUIRE_FALSE(resolve_attack(Weapon{"minecraft:diamond_axe"}, resting(), constants).sweeping);
    REQUIRE_FALSE(resolve_attack(Weapon{""}, resting(), constants).sweeping);

    AttackerState uncharged   = resting();
    uncharged.strength_ticker = 0;
    REQUIRE_FALSE(
        resolve_attack(Weapon{"minecraft:diamond_sword"}, uncharged, constants).sweeping);

    AttackerState sprinting = resting();
    sprinting.sprinting     = true;
    REQUIRE_FALSE(
        resolve_attack(Weapon{"minecraft:diamond_sword"}, sprinting, constants).sweeping);

    AttackerState airborne = resting();
    airborne.on_ground     = false;
    airborne.fall_distance = 2.0F;
    REQUIRE_FALSE(resolve_attack(Weapon{"minecraft:diamond_sword"}, airborne, constants).sweeping);

    AttackerState running               = resting();
    running.moving_faster_than_walking  = true;
    REQUIRE_FALSE(resolve_attack(Weapon{"minecraft:diamond_sword"}, running, constants).sweeping);

    // Sweeping Edge adds level/(level+1) of the hit's damage on top of the one.
    Weapon edged{"minecraft:diamond_sword"};
    edged.sweeping_edge     = 3;
    const AttackOutcome out = resolve_attack(edged, resting(), constants);
    REQUIRE(out.sweep_damage == Approx(1.0 + 0.75 * 7.0));
}

TEST_CASE("knockback is two impulses in two directions", "[gameplay][combat]") {
    const CombatConstants constants;

    // A plain hit: 0.4 along the line from attacker to victim, nothing else.
    // Measured as Set Entity Velocity (0.4, 0.4, 0.0) on a sheep three blocks
    // along +x from a bot at yaw 0.
    const AttackOutcome plain = resolve_attack(Weapon{"minecraft:diamond_sword"}, resting(),
                                               constants);
    REQUIRE(plain.knockback_strength == Approx(0.4));
    REQUIRE(plain.directed_knockback == Approx(0.0));

    const Vec3d after = apply_knockback(Vec3d{0.0, 0.0, 0.0}, true, plain.knockback_strength,
                                        1.0, 0.0, 0.0F, constants);
    REQUIRE(after.x == Approx(-0.4));
    REQUIRE(after.y == Approx(0.4));
    REQUIRE(after.z == Approx(0.0));

    // Sprinting adds a second impulse of 0.5 along the attacker's *facing*.
    AttackerState sprinting = resting();
    sprinting.sprinting     = true;
    const AttackOutcome dash = resolve_attack(Weapon{"minecraft:diamond_sword"}, sprinting,
                                              constants);
    REQUIRE(dash.sprint_knockback);
    REQUIRE(dash.knockback_strength == Approx(0.4));
    REQUIRE(dash.directed_knockback == Approx(0.5));

    // The measured sequence, in order: the hit's impulse away from the attacker
    // along +x, then the sprint's along the attacker's yaw of 0, which is +z.
    //
    // Both calls take the vector the game passes: for the hit, the attacker's
    // position minus the victim's; for the sprint, `(sin(yaw), -cos(yaw))`.
    // `apply_knockback` subtracts them, so a facing of (0, -1) pushes toward
    // +z — negating it here would push the victim through the attacker.
    Vec3d       velocity  = Vec3d{0.0, 0.0, 0.0};
    velocity              = apply_knockback(velocity, true, dash.knockback_strength, -1.0, 0.0,
                                            0.0F, constants);
    const Vec3d direction = knockback_direction(0.0F);
    velocity = apply_knockback(velocity, true, dash.directed_knockback, direction.x, direction.z,
                               0.0F, constants);
    REQUIRE(velocity.x == Approx(0.2));
    REQUIRE(velocity.y == Approx(0.4));
    REQUIRE(velocity.z == Approx(0.5));

    // Knockback II without sprinting reaches 1.0, measured.
    Weapon knocker{"minecraft:diamond_sword"};
    knocker.knockback = 2;
    REQUIRE(resolve_attack(knocker, resting(), constants).directed_knockback == Approx(1.0));
}

TEST_CASE("knockback resistance and a zero direction are both refused",
          "[gameplay][combat]") {
    const CombatConstants constants;
    const Vec3d           start{0.5, 0.25, -0.5};

    // Full resistance leaves the velocity exactly as it was.
    const Vec3d resisted = apply_knockback(start, true, 0.4F, 1.0, 0.0, 1.0F, constants);
    REQUIRE(resisted.x == Approx(start.x));
    REQUIRE(resisted.z == Approx(start.z));

    // Two entities in the same place: nothing, rather than a NaN that would
    // make the victim's position unrecoverable.
    const Vec3d nowhere = apply_knockback(start, true, 0.4F, 0.0, 0.0, 0.0F, constants);
    REQUIRE(nowhere.x == Approx(start.x));
    REQUIRE_FALSE(std::isnan(nowhere.x));
}

TEST_CASE("yaw zero points at plus z", "[gameplay][combat]") {
    const Vec3d south = knockback_direction(0.0F);
    REQUIRE(south.x == Approx(0.0).margin(1.0e-9));
    REQUIRE(south.z == Approx(-1.0));
    const Vec3d west = knockback_direction(90.0F);
    REQUIRE(west.x == Approx(1.0));
    REQUIRE(west.z == Approx(0.0).margin(1.0e-9));
}

TEST_CASE("a tool swung at a mob costs twice what a sword costs",
          "[gameplay][combat]") {
    // Measured on a real server: a diamond sword came back at Damage 1 after
    // one hit, a diamond pickaxe, axe, shovel and hoe at 2, and shears at 0.
    REQUIRE(resolve_attack(Weapon{"minecraft:diamond_sword"}, resting(), {}).item_damage == 1);
    REQUIRE(resolve_attack(Weapon{"minecraft:diamond_pickaxe"}, resting(), {}).item_damage == 2);
    REQUIRE(resolve_attack(Weapon{""}, resting(), {}).item_damage == 0);
}

TEST_CASE("sharpness is scaled by the charge but not by the critical",
          "[gameplay][combat]") {
    const CombatConstants constants;
    Weapon                sharp{"minecraft:diamond_sword"};
    sharp.sharpness = 5;
    REQUIRE(sharpness_bonus(5) == Approx(3.0));

    // Charged and on the ground: 7 + 3.
    REQUIRE(resolve_attack(sharp, resting(), constants).damage == Approx(10.0));

    // Charged, falling: the 7 becomes 10.5 and the 3 stays 3.
    AttackerState falling = resting();
    falling.on_ground     = false;
    falling.fall_distance = 1.0F;
    REQUIRE(resolve_attack(sharp, falling, constants).damage == Approx(13.5));

    // Uncharged: the weapon keeps a fifth, the enchantment keeps almost none.
    AttackerState early   = resting();
    early.strength_ticker = 0;
    const AttackOutcome spam = resolve_attack(sharp, early, constants);
    REQUIRE(spam.damage < 1.7F);
}

TEST_CASE("weakness cannot heal the target", "[gameplay][combat]") {
    AttackerState weak = resting();
    weak.weakness      = 0;  // Weakness I, -4
    const AttackOutcome out = resolve_attack(Weapon{""}, weak, CombatConstants{});
    REQUIRE(out.damage == Approx(0.0));
}

TEST_CASE("the gauge ticks up and an attack resets it", "[gameplay][combat]") {
    AttackerState attacker;
    attacker.strength_ticker = 0;
    tick_attack_strength(attacker);
    tick_attack_strength(attacker);
    REQUIRE(attacker.strength_ticker == 2);
    REQUIRE(attack_strength_scale(attacker.strength_ticker, 12.5F) == Approx(0.2));
    // And it saturates rather than overflowing after a long idle.
    for (int i = 0; i < 5000; ++i) {
        tick_attack_strength(attacker);
    }
    REQUIRE(attacker.strength_ticker == 1000);
    REQUIRE(attack_strength_scale(attacker.strength_ticker, 12.5F) == Approx(1.0));
}
