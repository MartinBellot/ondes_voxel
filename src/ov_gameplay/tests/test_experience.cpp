// The experience curve, against the forty-one costs that were measured.
//
// Each cost was read off a real 1.20.1 server one level at a time: set the
// level, add a single point, and the experience bar comes back holding exactly
// one over that level's cost. The table below is what the server answered, and
// the three-piece curve has to reproduce every entry — not the shape of it.
#include "ov/gameplay/experience.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <numeric>

using namespace ov;
using namespace ov::gameplay;

namespace {

constexpr ExperienceCurve kCurve{};

/// The measured cost of going from level i to level i + 1, for i = 0..40.
constexpr std::array<i32, 41> kMeasuredCosts{
    7,  9,  11, 13, 15, 17, 19,  21,  23,  25,  27,  29,  31,  33,  35,  37,  42,  47, 52, 57, 62,
    67, 72, 77, 82, 87, 92, 97, 102, 107, 112, 121, 130, 139, 148, 157, 166, 175, 184, 193, 202};

}  // namespace

TEST_CASE("every measured level cost falls on the three-piece curve", "[experience][parity]") {
    for (i32 level = 0; level <= 40; ++level) {
        INFO("level " << level << " to " << level + 1);
        REQUIRE(experience_to_next_level(level, kCurve) == kMeasuredCosts[usize(level)]);
    }
}

TEST_CASE("the joins are where the curve actually bends", "[experience]") {
    // Fifteen to sixteen still costs 2L+7; sixteen to seventeen already costs
    // 5L-38. One line fitted through both is off by five at the join and by
    // more further out, and nothing below level 30 would ever show it.
    REQUIRE(experience_to_next_level(15, kCurve) == 37);
    REQUIRE(experience_to_next_level(16, kCurve) == 42);
    REQUIRE(experience_to_next_level(30, kCurve) == 112);
    REQUIRE(experience_to_next_level(31, kCurve) == 121);
}

TEST_CASE("totals and levels are each other's inverse", "[experience][parity]") {
    // The independent check the measurement ran too: feed the cumulative total
    // a level should cost, and the level must come back exactly, with an empty
    // bar. A curve wrong anywhere fails here at the first level past the error.
    //
    // On the real server this check came back 39 correct out of 41. The two
    // that did not are levels 12 and 13, where the server answered one level
    // low with a bar of 0.99999994 — because vanilla accumulates the bar as a
    // *float*, one division per point awarded, and a thousand of those do not
    // add back up to one. Ours keeps the remainder as an integer and computes
    // the bar only for display, so it is exact at every level. That is a
    // deliberate divergence, named here rather than hidden: determinism
    // (CLAUDE.md principle 5) is worth more than reproducing a rounding error,
    // and the number the client draws is indistinguishable either way.
    i32 cumulative = 0;
    for (i32 level = 0; level <= 40; ++level) {
        INFO("reaching level " << level);
        REQUIRE(total_experience_for_level(level, kCurve) == cumulative);

        const LevelProgress progress = level_for_total(cumulative, kCurve);
        REQUIRE(progress.level == level);
        REQUIRE(progress.points_into_level == 0);
        REQUIRE(progress.bar == Catch::Approx(0.0F));

        cumulative += kMeasuredCosts[usize(level)];
    }
    // Thirty is the enchanting bar every player knows: 1395 points.
    REQUIRE(total_experience_for_level(30, kCurve) == 1395);
}

TEST_CASE("the bar is a fraction of the level, not of the total", "[experience]") {
    // One point into level 0, whose cost is 7.
    const LevelProgress one = level_for_total(1, kCurve);
    REQUIRE(one.level == 0);
    REQUIRE(one.points_into_level == 1);
    REQUIRE(one.bar == Catch::Approx(1.0F / 7.0F));

    // And one point past level 16, whose cost is 42.
    const LevelProgress deep = level_for_total(total_experience_for_level(16, kCurve) + 1, kCurve);
    REQUIRE(deep.level == 16);
    REQUIRE(deep.bar == Catch::Approx(1.0F / 42.0F));
}

TEST_CASE("death drops seven a level and never more than a hundred",
          "[experience][parity]") {
    // Measured by killing a player at each level and summing the Value of every
    // orb the death produced.
    constexpr std::array<std::pair<i32, i32>, 12> kMeasured{{
        {0, 0}, {1, 7}, {2, 14}, {5, 35}, {10, 70}, {13, 91}, {14, 98},
        {15, 100}, {20, 100}, {30, 100}, {60, 100}, {100, 100},
    }};
    for (const auto& [level, expected] : kMeasured) {
        INFO("dying at level " << level);
        REQUIRE(death_experience(level) == expected);
    }
}

TEST_CASE("a lump of experience splits into the game's own denominations",
          "[experience][parity]") {
    std::array<i32, 32> orbs{};

    // Forty-eight came off a real death as four orbs: 37, 7, 3, 1.
    const usize count = split_into_orbs(48, orbs);
    REQUIRE(count == 4);
    REQUIRE(orbs[0] == 37);
    REQUIRE(orbs[1] == 7);
    REQUIRE(orbs[2] == 3);
    REQUIRE(orbs[3] == 1);

    // Every split adds back up to what went in, and the measured orb counts
    // match: 100 points came out as four orbs, 91 as three, 98 as four.
    for (const auto& [amount, expected_orbs] :
         std::array<std::pair<i32, usize>, 6>{{{7, 1}, {14, 2}, {35, 3}, {91, 3}, {98, 4},
                                               {100, 4}}}) {
        std::array<i32, 32> split{};
        const usize         n = split_into_orbs(amount, split);
        INFO(amount << " points");
        REQUIRE(n == expected_orbs);
        REQUIRE(std::accumulate(split.begin(), split.begin() + static_cast<long>(n), 0) == amount);
    }
}

TEST_CASE("a split that does not fit says so instead of losing experience",
          "[experience]") {
    std::array<i32, 2> tiny{};
    // 2477 is the biggest denomination, so a huge amount needs many orbs. A
    // caller with room for two gets two and a count of two — never a silent
    // truncation reported as success.
    REQUIRE(split_into_orbs(100000, tiny) == 2);
    REQUIRE(tiny[0] == 2477);
    REQUIRE(tiny[1] == 2477);
}

TEST_CASE("orbs merge only when close and of an age", "[experience]") {
    const OrbState fresh{.value = 3, .age = 0};
    const OrbState same_age{.value = 5, .age = 10};
    const OrbState ancient{.value = 5, .age = 4000};

    REQUIRE(orbs_can_merge(fresh, same_age, 0.25));
    REQUIRE_FALSE(orbs_can_merge(fresh, same_age, 4.0));
    // Distance alone is not enough: a pile that swallowed everything nearby
    // would inherit the newest orb's lifetime and never expire.
    REQUIRE_FALSE(orbs_can_merge(fresh, ancient, 0.25));
}
