// Hunger, against the server that defines it.
//
// The interesting cases are the ones where the three counters interact, because
// that is where an implementation with only the visible bar looks right and is
// wrong: saturation is spent before food, exhaustion wraps once per tick and
// not in a loop, and regeneration is what makes a well-fed player hungry.
#include "ov/gameplay/food.hpp"

#include <simdjson.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

using namespace ov;
using namespace ov::gameplay;

namespace {

constexpr FoodConstants kFood{};

/// Run `ticks` ticks of a full player and return the health gained.
[[nodiscard]] f32 heal_over(FoodState& state, f32 health, i32 ticks,
                            Difficulty difficulty = Difficulty::Normal) {
    f32 total = 0.0F;
    for (i32 i = 0; i < ticks; ++i) {
        const FoodTick step = tick_food(state, health + total, 20.0F, difficulty, true, kFood);
        total += step.heal;
    }
    return total;
}

}  // namespace

TEST_CASE("starvation stops at a different place on every difficulty",
          "[food][parity]") {
    // Measured by starving a real player from twelve health on each difficulty
    // and watching where the health settled: peaceful never starved at all and
    // refilled the bar, easy stopped at ten, normal at one, hard at zero.
    REQUIRE(starvation_floor(Difficulty::Easy) == Catch::Approx(10.0F));
    REQUIRE(starvation_floor(Difficulty::Normal) == Catch::Approx(1.0F));
    REQUIRE(starvation_floor(Difficulty::Hard) == Catch::Approx(0.0F));
    // Peaceful's floor is the health bar itself: it never takes a point.
    REQUIRE(starvation_floor(Difficulty::Peaceful) == Catch::Approx(20.0F));
}

TEST_CASE("starvation costs one point every four seconds", "[food]") {
    FoodState state{.food = 0, .saturation = 0.0F};
    f32       damage = 0.0F;
    for (i32 tick = 0; tick < 80; ++tick) {
        damage += tick_food(state, 20.0F, 20.0F, Difficulty::Normal, true, kFood).damage;
    }
    REQUIRE(damage == Catch::Approx(1.0F));

    // And it stops at the floor rather than at zero, on normal.
    FoodState low{.food = 0, .saturation = 0.0F};
    f32       none = 0.0F;
    for (i32 tick = 0; tick < 400; ++tick) {
        none += tick_food(low, 1.0F, 20.0F, Difficulty::Normal, true, kFood).damage;
    }
    REQUIRE(none == Catch::Approx(0.0F));

    // On hard it does not stop.
    FoodState hard{.food = 0, .saturation = 0.0F};
    f32       fatal = 0.0F;
    for (i32 tick = 0; tick < 80; ++tick) {
        fatal += tick_food(hard, 1.0F, 20.0F, Difficulty::Hard, true, kFood).damage;
    }
    REQUIRE(fatal == Catch::Approx(1.0F));
}

TEST_CASE("exhaustion is spent from saturation first, then from food", "[food]") {
    FoodState state{.food = 20, .saturation = 3.0F};

    // Four sprinted blocks is 0.4 exhaustion; a hundred of them is ten, which
    // is two whole points and a remainder.
    for (i32 block = 0; block < 100; ++block) {
        (void)add_exhaustion(state, kFood.sprint_per_block, kFood);
        (void)tick_food(state, 20.0F, 20.0F, Difficulty::Normal, true, kFood);
    }
    // Two wraps: saturation 3 -> 1. The visible bar has not moved.
    REQUIRE(state.saturation == Catch::Approx(1.0F));
    REQUIRE(state.food == 20);
    REQUIRE(state.exhaustion == Catch::Approx(2.0F));

    // Seventy more blocks. The counter is at 2.0, so the twenty-first block
    // spends the last point of saturation and the sixty-second takes the first
    // haunch — which is the point of the test: the visible bar does not move
    // until the hidden pool is empty, and then it moves every forty blocks.
    for (i32 block = 0; block < 70; ++block) {
        (void)add_exhaustion(state, kFood.sprint_per_block, kFood);
        (void)tick_food(state, 20.0F, 20.0F, Difficulty::Normal, true, kFood);
    }
    REQUIRE(state.saturation == Catch::Approx(0.0F));
    REQUIRE(state.food == 19);
}

TEST_CASE("forty sprinted blocks cost one haunch from an empty saturation",
          "[food][parity]") {
    // How far do you sprint to lose half a haunch of the visible bar? One food
    // point is 4.0 exhaustion at a tenth of a point per block, so forty blocks
    // — and the bar draws half-haunches, so half of one is twenty blocks and a
    // whole haunch is eighty.
    //
    // The loop runs forty-one times because the wrap is strict: the block that
    // brings the counter to exactly four does not spend it, the next one does.
    // The arithmetic answer is forty; the observable one is on the forty-first
    // block, and a player counting blocks would report the latter.
    FoodState state{.food = 20, .saturation = 0.0F};
    for (i32 block = 0; block < 41; ++block) {
        (void)add_exhaustion(state, kFood.sprint_per_block, kFood);
        (void)tick_food(state, 20.0F, 20.0F, Difficulty::Normal, true, kFood);
    }
    REQUIRE(state.food == 19);
    REQUIRE(kFood.exhaustion_per_point / kFood.sprint_per_block == Catch::Approx(40.0F));
}

TEST_CASE("exhaustion wraps once a tick, not in a loop", "[food]") {
    // Twenty points charged in one go — a very long jump, or a lump of damage —
    // must not empty five haunches on the next tick. Vanilla spends one point
    // per tick whatever the counter holds, which is why a single expensive
    // action never visibly costs more than half a haunch.
    FoodState state{.food = 20, .saturation = 0.0F};
    (void)add_exhaustion(state, 20.0F, kFood);
    (void)tick_food(state, 20.0F, 20.0F, Difficulty::Normal, true, kFood);
    REQUIRE(state.food == 19);
    REQUIRE(state.exhaustion == Catch::Approx(16.0F));
}

TEST_CASE("saturated regeneration is fast and expensive", "[food][parity]") {
    // Measured: a player at twenty food with saturation left went from ten
    // health to twenty inside the first sampling window, which is the fast
    // branch — one point every ten ticks. The slow branch would have taken
    // forty seconds for the same ten points.
    FoodState state{.food = 20, .saturation = 20.0F};
    REQUIRE(heal_over(state, 10.0F, 100) == Catch::Approx(10.0F));

    // And it charges six exhaustion per heal, which is a point and a half of
    // saturation each time: healing ten hearts eats most of a full bar.
    REQUIRE(state.saturation < 20.0F);
}

TEST_CASE("regeneration needs eighteen food and nothing less", "[food][parity]") {
    // Measured: at seventeen food and no saturation, a player at ten health
    // gained nothing over twenty-two seconds.
    FoodState seventeen{.food = 17, .saturation = 0.0F};
    REQUIRE(heal_over(seventeen, 10.0F, 20 * 30) == Catch::Approx(0.0F));

    // At eighteen it heals, on the slow branch: one point every eighty ticks.
    FoodState eighteen{.food = 18, .saturation = 0.0F};
    REQUIRE(heal_over(eighteen, 10.0F, 80) == Catch::Approx(1.0F));
}

TEST_CASE("a player at full health does not regenerate or get hungry for it",
          "[food]") {
    FoodState state{.food = 20, .saturation = 20.0F};
    const f32 gained = heal_over(state, 20.0F, 200);
    REQUIRE(gained == Catch::Approx(0.0F));
    REQUIRE(state.saturation == Catch::Approx(20.0F));
    REQUIRE(state.exhaustion == Catch::Approx(0.0F));
}

TEST_CASE("natural regeneration off means no healing at all", "[food]") {
    FoodState state{.food = 20, .saturation = 20.0F};
    f32       total = 0.0F;
    for (i32 tick = 0; tick < 400; ++tick) {
        total += tick_food(state, 10.0F, 20.0F, Difficulty::Normal, false, kFood).heal;
    }
    REQUIRE(total == Catch::Approx(0.0F));
}

TEST_CASE("saturation is capped by the food level it was eaten at", "[food]") {
    // The ordering that makes eating on an empty stomach worse than eating half
    // full. A steak is nutrition 8 with a modifier of 0.8, so 12.8 saturation —
    // but the ceiling is whatever the bar reached.
    const FoodValue steak{"minecraft:cooked_beef", 8, 0.8F, false};

    FoodState empty{.food = 0, .saturation = 0.0F};
    eat(empty, steak);
    REQUIRE(empty.food == 8);
    REQUIRE(empty.saturation == Catch::Approx(8.0F));  // clipped from 12.8

    FoodState half{.food = 10, .saturation = 0.0F};
    eat(half, steak);
    REQUIRE(half.food == 18);
    REQUIRE(half.saturation == Catch::Approx(12.8F));  // room for all of it
}

TEST_CASE("a full bar refuses everything but the always-edible", "[food]") {
    const FoodState full{.food = 20, .saturation = 5.0F};
    REQUIRE_FALSE(can_eat(full, FoodValue{"minecraft:bread", 5, 0.6F, false}));
    REQUIRE(can_eat(full, FoodValue{"minecraft:golden_apple", 4, 1.2F, true}));

    const FoodState hungry{.food = 19, .saturation = 0.0F};
    REQUIRE(can_eat(hungry, FoodValue{"minecraft:bread", 5, 0.6F, false}));
}

TEST_CASE("an item that is not food says so", "[food]") {
    REQUIRE_FALSE(food_for("minecraft:stone").has_value());
    REQUIRE_FALSE(food_for("").has_value());
}

TEST_CASE("every food in the table is the one the server fed the probe",
          "[food][parity]") {
    // The table in food.cpp is the output of scripts/measure_survival.py, and
    // this reads the measurement back to ask whether it survived the trip into
    // C++ unchanged. It is not "does the number look right" — it is "did any
    // number change on the way", which is the part a test can falsify.
    //
    // The measurement is gitignored, so a machine without it skips. A machine
    // with it checks every entry both ways: nothing measured is missing from
    // the table, and nothing in the table was invented.
    const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                      "normalized" / "survival.json";
    if (!std::filesystem::exists(path)) {
        WARN("survival.json missing; run scripts/measure_survival.py");
        return;
    }

    simdjson::dom::parser parser;
    auto                  document = parser.load(path.string());
    if (document.error() != simdjson::SUCCESS) {
        WARN("survival.json could not be parsed");
        return;
    }
    simdjson::dom::object measured;
    if ((*document)["food"].get(measured) != simdjson::SUCCESS) {
        WARN("survival.json has no food campaign yet");
        return;
    }

    usize checked = 0;
    usize missing = 0;
    for (const auto [name, value] : measured) {
        const std::string item{name};
        int64_t           nutrition = 0;
        double            saturation = 0.0;
        bool              clamped = false;
        if (value["nutrition"].get(nutrition) != simdjson::SUCCESS ||
            value["saturation"].get(saturation) != simdjson::SUCCESS) {
            continue;
        }
        (void)value["saturation_clamped"].get(clamped);
        if (nutrition <= 0) {
            // Something that moved saturation and not the bar, or an item that
            // was refused. Not food, and not silently folded into the table.
            continue;
        }

        const auto entry = food_for(item);
        INFO("measured food " << item);
        if (!entry) {
            ++missing;
            continue;
        }
        REQUIRE(entry->nutrition == static_cast<i32>(nutrition));
        if (!clamped) {
            // modifier = saturation / (2 * nutrition), which is the game's own
            // definition of it. A clamped measurement only bounds the modifier
            // from below, so it is checked as a bound rather than as a value.
            const f32 expected =
                static_cast<f32>(saturation) / (2.0F * static_cast<f32>(nutrition));
            REQUIRE(std::abs(entry->saturation_modifier - expected) < 1e-3F);
        } else {
            REQUIRE(entry->saturation_modifier * 2.0F * static_cast<f32>(nutrition) >=
                    static_cast<f32>(saturation) - 1e-3F);
        }
        ++checked;
    }

    INFO(checked << " foods checked, " << missing << " measured but absent from the table");
    REQUIRE(missing == 0);
    // And nothing in the table that the server never confirmed.
    for (const FoodValue& entry : food_table()) {
        INFO("table entry " << entry.name);
        REQUIRE(measured[entry.name].error() == simdjson::SUCCESS);
    }
    REQUIRE(checked + missing == food_table().size());
}

TEST_CASE("the food table is the forty items the sweep found, and no others",
          "[food][parity]") {
    // Three hundred and ninety-nine candidates were fed to a real player one at
    // a time; forty of them moved the bar. The count is worth asserting on its
    // own: an item quietly added to this table would be an item the server
    // never confirmed is food.
    REQUIRE(food_table().size() == 40);

    // Five saturation modifiers occur, and only five. Every food in 1.20.1 uses
    // one of them, which is a fact about the game rather than about the table —
    // and a sixth value appearing here would mean a measurement went wrong.
    std::array<f32, 5> known{0.1F, 0.3F, 0.6F, 0.8F, 1.2F};
    for (const FoodValue& entry : food_table()) {
        INFO(entry.name);
        const bool recognised =
            std::any_of(known.begin(), known.end(), [&](f32 value) {
                return std::abs(entry.saturation_modifier - value) < 1e-3F;
            });
        REQUIRE(recognised);
        REQUIRE(entry.nutrition >= 1);
        REQUIRE(entry.nutrition <= 10);
    }
}

TEST_CASE("the foods that can be eaten on a full bar are the five measured ones",
          "[food][parity]") {
    // Measured by a second pass: the player is put at twenty food with a pool
    // that still has room, and handed each of the forty foods. An item that is
    // refused adds nothing; an always-edible one adds saturation. Forty of forty
    // came back conclusive, and five of them were eaten.
    const std::array<std::string_view, 5> expected{
        "minecraft:golden_apple", "minecraft:enchanted_golden_apple",
        "minecraft:chorus_fruit", "minecraft:suspicious_stew", "minecraft:honey_bottle"};
    usize found = 0;
    for (const FoodValue& entry : food_table()) {
        if (!entry.always_edible) {
            continue;
        }
        ++found;
        INFO(entry.name << " is marked always edible");
        REQUIRE(std::find(expected.begin(), expected.end(), entry.name) != expected.end());
    }
    REQUIRE(found == expected.size());

    // And the rule they exist for: a full bar refuses everything else.
    const FoodState full{.food = 20, .saturation = 5.0F};
    REQUIRE(can_eat(full, *food_for("minecraft:golden_apple")));
    REQUIRE_FALSE(can_eat(full, *food_for("minecraft:apple")));
}

TEST_CASE("the foods a player remembers are the ones the server gave back",
          "[food][parity]") {
    // Spelled out rather than left to the count, because a reader wants to see
    // them: bread is five and 0.6, a cooked porkchop is eight and 0.8, rotten
    // flesh is four points of food for almost no saturation, and a golden
    // carrot has the best saturation in the game.
    struct Expected {
        std::string_view name;
        i32              nutrition;
        f32              modifier;
    };
    const std::array<Expected, 8> measured{{
        {"minecraft:bread", 5, 0.6F},
        {"minecraft:cooked_porkchop", 8, 0.8F},
        {"minecraft:cooked_beef", 8, 0.8F},
        {"minecraft:golden_carrot", 6, 1.2F},
        {"minecraft:rotten_flesh", 4, 0.1F},
        {"minecraft:rabbit_stew", 10, 0.6F},
        {"minecraft:pufferfish", 1, 0.1F},
        {"minecraft:honey_bottle", 6, 0.1F},
    }};
    for (const Expected& want : measured) {
        const auto entry = food_for(want.name);
        INFO(want.name);
        REQUIRE(entry.has_value());
        REQUIRE(entry->nutrition == want.nutrition);
        REQUIRE(std::abs(entry->saturation_modifier - want.modifier) < 1e-3F);
    }
    // Rabbit stew is the largest nutrition in the game, which is what makes ten
    // the right baseline for the measurement: from there nothing can reach the
    // ceiling of twenty and be recorded short.
    i32 largest = 0;
    for (const FoodValue& entry : food_table()) {
        largest = std::max(largest, entry.nutrition);
    }
    REQUIRE(largest == 10);
}

TEST_CASE("the exhaustion an action costs is the one that was counted",
          "[food][parity]") {
    // Normalised by the server's own statistics rather than by packets sent:
    // sprint_one_cm, mined:stone and custom:jump. A probe driven from Python
    // does not get every position packet processed, and dividing by what was
    // *sent* gave 0.065 a block for sprinting — a number the game does not have.
    //
    //   sprinting   5.45081 exhaustion over 52.15 counted blocks -> 0.1045
    //   breaking    0.125   over 25 blocks the server agreed were mined -> 0.005
    //   jumping     1.0     over 20 jumps the server counted -> 0.05
    //
    // The two exact ones are pinned. Sprinting is within five per cent of a
    // tenth and is not pinned tighter than that: the counter it is divided by
    // is itself rounded to the centimetre.
    //
    // The block-breaking figure rests on a *single* successful run. Digging
    // from the probe is intermittent — twenty-five of twenty-five once, zero of
    // twenty-five twice afterwards, with the pickaxe held and the block
    // present. See docs/provenance/survie.md § 12; the number is a measurement
    // and not a guess, but it has been taken once.
    REQUIRE(kFood.break_block == Catch::Approx(0.005F));
    REQUIRE(kFood.jump == Catch::Approx(0.05F));
    REQUIRE(std::abs(kFood.sprint_per_block - 0.1045F) < 0.006F);
}
