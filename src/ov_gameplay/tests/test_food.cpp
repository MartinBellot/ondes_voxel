// Hunger, against the server that defines it.
//
// The interesting cases are the ones where the three counters interact, because
// that is where an implementation with only the visible bar looks right and is
// wrong: saturation is spent before food, exhaustion wraps once per tick and
// not in a loop, and regeneration is what makes a well-fed player hungry.
#include "ov/gameplay/food.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
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
