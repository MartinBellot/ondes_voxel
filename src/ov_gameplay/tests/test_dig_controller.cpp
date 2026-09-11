#include "ov/gameplay/dig_controller.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::gameplay;

namespace {

constexpr BlockPos kStone{1, -60, 3};
constexpr BlockPos kOther{2, -60, 3};

[[nodiscard]] DigTarget at(BlockPos pos, u8 face = 2) {
    return DigTarget{pos, face, registry::BlockStateId{1}, true};
}

[[nodiscard]] DigInput holding(BlockPos pos, f32 rate, bool pressed = false) {
    DigInput input;
    input.pressed           = pressed;
    input.held              = true;
    input.target            = at(pos);
    input.progress_per_tick = rate;
    return input;
}

/// Press on `pos` and hold until a Finish is sent; returns the tick (1 = the
/// press's own) on which it went.
[[nodiscard]] i32 ticks_to_finish(DigController& dig, BlockPos pos, f32 rate) {
    for (i32 tick = 1; tick < 10000; ++tick) {
        const DigOutcome out = dig.tick(holding(pos, rate, tick == 1));
        for (u8 i = 0; i < out.action_count; ++i) {
            if (out.actions[i].status == DigStatus::Finish) {
                return tick;
            }
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("a press starts, and the press's own tick already counts", "[dig]") {
    DigController    dig;
    const DigOutcome out = dig.tick(holding(kStone, 1.0F / 150.0F, true));
    REQUIRE(out.action_count == 1);
    CHECK(out.actions[0].status == DigStatus::Start);
    CHECK(out.actions[0].pos == kStone);
    CHECK(out.actions[0].face == 2);
    CHECK(out.swing);
    CHECK(out.hit_sound);  // the fourth-tick rule starts at zero
    CHECK(out.crack_particle);
    CHECK(dig.progress() > 0.0F);
    CHECK(dig.stage() == -1);
}

TEST_CASE("the Finish goes on the tick the count reaches one", "[dig]") {
    // The rate is written exactly as BreakRules computes it, speed / hardness
    // / divisor in f32, because the count is a sum of f32 and the tick it
    // crosses one on depends on the rounding. Stone by hand is 1/150 a tick,
    // yet 150 of them sum to 0.99999: the claim goes on tick 151. The server
    // times the same block at 150 with ceil(1 / rate) and accepts the claim a
    // tick later through its 0.7 threshold — both are what 1.20.1 does.
    DigController dig;
    CHECK(ticks_to_finish(dig, kStone, 1.0F / 1.5F / 100.0F) == 151);
    DigController dirt;
    CHECK(ticks_to_finish(dirt, kStone, 1.0F / 0.5F / 30.0F) == 15);
    DigController planks;
    CHECK(ticks_to_finish(planks, kStone, 1.0F / 2.0F / 30.0F) == 61);
}

TEST_CASE("the stage is floor(count * 10) - 1", "[dig]") {
    DigController dig;
    std::vector<i32> stages;
    for (i32 tick = 1; tick <= 20; ++tick) {
        (void)dig.tick(holding(kStone, 0.05F, tick == 1));
        stages.push_back(dig.stage());
    }
    // 0.05 a tick: 0.1 after two ticks, 0.95 after nineteen, broken at twenty.
    CHECK(stages[0] == -1);
    CHECK(stages[1] == 0);
    CHECK(stages[3] == 1);
    CHECK(stages[17] == 8);
    CHECK(stages[18] == 8);  // 0.95 -> floor(9.5) - 1 = 8, f32 permitting
    CHECK(stages[19] == -1);  // broken: nothing left to crack
    for (const i32 stage : stages) {
        CHECK(stage >= -1);
        CHECK(stage <= 9);
    }
}

TEST_CASE("the hit sound plays every fourth tick of the count", "[dig]") {
    DigController    dig;
    std::vector<i32> heard;
    for (i32 tick = 1; tick <= 13; ++tick) {
        if (dig.tick(holding(kStone, 0.001F, tick == 1)).hit_sound) {
            heard.push_back(tick);
        }
    }
    CHECK(heard == std::vector<i32>{1, 5, 9, 13});
}

TEST_CASE("breaking a block leaves five ticks before the next one starts", "[dig]") {
    DigController dig;
    REQUIRE(ticks_to_finish(dig, kStone, 0.5F) == 2);
    // Still holding, now aimed at the next block.
    std::vector<i32> starts;
    for (i32 tick = 3; tick <= 12; ++tick) {
        const DigOutcome out = dig.tick(holding(kOther, 0.5F));
        CHECK(out.swing);  // the arm keeps going through the delay
        for (u8 i = 0; i < out.action_count; ++i) {
            if (out.actions[i].status == DigStatus::Start) {
                starts.push_back(tick);
            }
        }
    }
    // Broken on tick 2; ticks 3..7 are the delay; the next start is tick 8 —
    // six ticks after the break, the gap the wiki gives.
    CHECK(starts == std::vector<i32>{8});
}

TEST_CASE("an instant block breaks on the press and leaves no delay", "[dig]") {
    DigController    dig;
    const DigOutcome out = dig.tick(holding(kStone, 1.0F, true));
    REQUIRE(out.action_count == 1);
    CHECK(out.actions[0].status == DigStatus::Start);
    REQUIRE(out.broken.has_value());
    CHECK(*out.broken == kStone);
    CHECK_FALSE(out.crack_particle);
    CHECK(dig.delay() == 0);
    // The next one the very next tick.
    const DigOutcome next = dig.tick(holding(kOther, 1.0F));
    REQUIRE(next.broken.has_value());
    CHECK(*next.broken == kOther);
}

TEST_CASE("letting go aborts, and so does looking at nothing", "[dig]") {
    DigController dig;
    (void)dig.tick(holding(kStone, 0.01F, true));
    DigInput released;
    const DigOutcome out = dig.tick(released);
    REQUIRE(out.action_count == 1);
    CHECK(out.actions[0].status == DigStatus::Abort);
    CHECK(out.actions[0].pos == kStone);
    CHECK_FALSE(dig.digging().has_value());
    CHECK(dig.stage() == -1);

    DigController away;
    (void)away.tick(holding(kStone, 0.01F, true));
    DigInput sky = holding(kStone, 0.01F);
    sky.target.reset();
    const DigOutcome gone = away.tick(sky);
    REQUIRE(gone.action_count == 1);
    CHECK(gone.actions[0].status == DigStatus::Abort);

    // Releasing with nothing under way sends nothing.
    DigController idle;
    CHECK(idle.tick(released).action_count == 0);
}

TEST_CASE("turning to another block aborts the first and starts the second", "[dig]") {
    DigController dig;
    (void)dig.tick(holding(kStone, 0.01F, true));
    (void)dig.tick(holding(kStone, 0.01F));
    const DigOutcome out = dig.tick(holding(kOther, 0.01F));
    REQUIRE(out.action_count == 2);
    CHECK(out.actions[0].status == DigStatus::Abort);
    CHECK(out.actions[0].pos == kStone);
    CHECK(out.actions[1].status == DigStatus::Start);
    CHECK(out.actions[1].pos == kOther);
    CHECK(dig.progress() == 0.0F);  // started, not yet counted
}

TEST_CASE("changing the held item restarts the same block", "[dig]") {
    DigController dig;
    DigInput      first = holding(kStone, 0.01F, true);
    first.held_item     = 10;
    (void)dig.tick(first);
    DigInput swapped  = holding(kStone, 0.01F);
    swapped.held_item = 11;
    const DigOutcome out = dig.tick(swapped);
    REQUIRE(out.action_count == 2);
    CHECK(out.actions[0].status == DigStatus::Abort);
    CHECK(out.actions[1].status == DigStatus::Start);
}

TEST_CASE("creative breaks on the press and every sixth tick while held", "[dig]") {
    // The block the press broke is air for the hold of the same tick, which
    // therefore spends none of the delay: the gap after the press is the same
    // six as after every other break.
    DigController    dig;
    std::vector<i32> broken;
    for (i32 tick = 1; tick <= 18; ++tick) {
        DigInput input = holding(tick < 7 ? kStone : kOther, 0.0F, tick == 1);
        input.creative = true;
        if (dig.tick(input).broken) {
            broken.push_back(tick);
        }
    }
    CHECK(broken == std::vector<i32>{1, 7, 13});

    DigController sword;
    DigInput      input             = holding(kStone, 0.0F, true);
    input.creative                  = true;
    input.can_attack_in_creative    = false;
    const DigOutcome out            = sword.tick(input);
    CHECK(out.action_count == 0);
    CHECK_FALSE(out.broken.has_value());
}

TEST_CASE("water, lava and air are not dug", "[dig]") {
    DigController dig;
    DigInput      input = holding(kStone, 0.5F, true);
    input.target->diggable = false;
    const DigOutcome out = dig.tick(input);
    CHECK(out.action_count == 0);
    CHECK(out.swing);  // the press still swings
}
