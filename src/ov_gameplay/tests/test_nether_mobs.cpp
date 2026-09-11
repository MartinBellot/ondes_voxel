// ── nether-2 ── The Nether's mobs: the rules that are numbers and clocks.
//
// What the real server was measured to do is in docs/provenance/nether-2.md
// § 3 (scripts/measure_nether_mobs.py); these tests pin the rules those numbers
// were compared against, so a change to one is a failing test and not a silent
// drift.

#include "ov/gameplay/mob_logic.hpp"
#include "ov/gameplay/nether_mobs.hpp"
#include "ov/gameplay/spawn_rules.hpp"
#include "ov/gameplay/spawning.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::gameplay;

TEST_CASE("a barter draws its entry by weight, then its count, then its level",
          "[gameplay][nether]") {
    BarterTable table{{
        {"minecraft:obsidian", 40, 1, 1, "", false},
        {"minecraft:string", 20, 3, 9, "", false},
        {"minecraft:book", 5, 1, 1, "", true},
        {"minecraft:potion", 8, 1, 1, "minecraft:fire_resistance", false},
    }};
    REQUIRE(table.total_weight() == 73);
    math::LegacyRandomSource random{1234};
    std::map<std::string, i32> seen;
    constexpr i32              kDraws = 73'000;
    for (i32 i = 0; i < kDraws; ++i) {
        const BarterDrop drop = table.draw(random);
        ++seen[std::string{drop.item}];
        if (drop.item == "minecraft:string") {
            CHECK(drop.count >= 3);
            CHECK(drop.count <= 9);
        } else {
            CHECK(drop.count == 1);
        }
        if (drop.item == "minecraft:book") {
            CHECK(drop.soul_speed_level >= 1);
            CHECK(drop.soul_speed_level <= 3);
        } else {
            CHECK(drop.soul_speed_level == 0);
        }
        if (drop.item == "minecraft:potion") {
            CHECK(drop.potion == "minecraft:fire_resistance");
        }
    }
    // Within 4 % of weight / 73 each (73 000 draws: the standard error of the
    // smallest share is under 1 %).
    const auto share = [&](const char* item) { return static_cast<f64>(seen[item]) / kDraws; };
    CHECK(std::abs(share("minecraft:obsidian") - 40.0 / 73.0) < 0.04 * 40.0 / 73.0);
    CHECK(std::abs(share("minecraft:string") - 20.0 / 73.0) < 0.04 * 20.0 / 73.0);
    CHECK(std::abs(share("minecraft:book") - 5.0 / 73.0) < 0.06 * 5.0 / 73.0);
    CHECK(std::abs(share("minecraft:potion") - 8.0 / 73.0) < 0.05 * 8.0 / 73.0);
}

TEST_CASE("a blaze charges for three seconds, fires three shots six ticks apart, rests",
          "[gameplay][nether]") {
    BlazeVolley       volley;
    std::vector<i32>  shots;
    std::vector<bool> charged;
    for (i32 tick = 1; tick <= 400; ++tick) {
        if (volley.tick(true)) {
            shots.push_back(tick);
        }
        charged.push_back(volley.charged);
    }
    REQUIRE(shots.size() >= 6);
    CHECK(shots[0] == 61);
    CHECK(shots[1] == 67);
    CHECK(shots[2] == 73);
    // Rest 100, charge 60 again: the next volley 178 ticks after the first.
    CHECK(shots[3] == 61 + 178);
    CHECK(shots[4] == 67 + 178);
    CHECK(shots[5] == 73 + 178);
    CHECK(charged[0]);        // lit from the first engaged tick
    CHECK_FALSE(charged[80]); // out, resting

    // Out of sight: no shot, and the flame goes out.
    BlazeVolley idle;
    for (i32 tick = 0; tick < 200; ++tick) {
        CHECK_FALSE(idle.tick(false));
    }
    CHECK_FALSE(idle.charged);
}

TEST_CASE("a ghast fires every three seconds while it sees its target", "[gameplay][nether]") {
    GhastCharge      charge;
    std::vector<i32> shots;
    bool             attacking_before_first = false;
    for (i32 tick = 1; tick <= 200; ++tick) {
        if (charge.tick(true)) {
            shots.push_back(tick);
        }
        if (tick == 15) {
            attacking_before_first = charge.attacking();
        }
    }
    REQUIRE(shots.size() >= 3);
    CHECK(shots[0] == 20);
    CHECK(shots[1] - shots[0] == 60);
    CHECK(shots[2] - shots[1] == 60);
    CHECK(attacking_before_first);  // the open mouth, from 11
}

TEST_CASE("a fireball is pushed by a constant acceleration and slowed by 5 %",
          "[gameplay][nether]") {
    const Vec3d power = fireball_power(Vec3d{3.0, 0.0, 4.0});
    CHECK(std::abs(power.x - 0.06) < 1e-12);
    CHECK(std::abs(power.z - 0.08) < 1e-12);
    CHECK(fireball_power(Vec3d{}).x == 0.0);

    Vec3d position{};
    Vec3d velocity{};
    for (i32 i = 0; i < 400; ++i) {
        step_fireball(position, velocity, power, false);
    }
    // Terminal speed: v = (v + a)·0.95 → v = 0.95a / 0.05 = 19a = 1.9 b/t.
    const f64 speed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    CHECK(std::abs(speed - 1.9) < 1e-3);
}

TEST_CASE("the Nether's mobs are known: categories, predicates, species",
          "[gameplay][nether]") {
    CHECK(category_of("minecraft:piglin") == MobCategory::Monster);
    CHECK(category_of("minecraft:zombified_piglin") == MobCategory::Monster);
    CHECK(category_of("minecraft:ghast") == MobCategory::Monster);
    CHECK(category_of("minecraft:magma_cube") == MobCategory::Monster);
    CHECK(category_of("minecraft:strider") == MobCategory::Creature);

    CHECK(spawn_rule_of("minecraft:piglin", false).rule == SpawnRule::NetherFloor);
    CHECK(spawn_rule_of("minecraft:hoglin", false).rule == SpawnRule::NetherFloor);
    CHECK(spawn_rule_of("minecraft:zombified_piglin", false).rule == SpawnRule::NetherFloor);
    CHECK(spawn_rule_of("minecraft:ghast", false).rule == SpawnRule::Ghast);
    CHECK(spawn_rule_of("minecraft:magma_cube", false).rule == SpawnRule::Anywhere);
    CHECK(spawn_rule_of("minecraft:strider", true).rule == SpawnRule::Strider);
    CHECK(spawn_rule_of("minecraft:enderman", false).rule == SpawnRule::Monster);

    for (const char* name : {"minecraft:piglin", "minecraft:piglin_brute", "minecraft:hoglin",
                             "minecraft:zoglin", "minecraft:zombified_piglin",
                             "minecraft:wither_skeleton", "minecraft:blaze",
                             "minecraft:magma_cube", "minecraft:strider"}) {
        INFO(name);
        const MobKind* kind = mob_kind(name);
        REQUIRE(kind != nullptr);
        CHECK(kind->movement_speed > 0.0);
    }
    // The ghast floats: it is not a walker in the species table.
    CHECK(mob_kind("minecraft:ghast") == nullptr);
    CHECK(mob_kind("minecraft:strider")->category == MobCategory::Creature);
    CHECK(mob_kind("minecraft:zombified_piglin")->hostile);  // hunts once angered
}
