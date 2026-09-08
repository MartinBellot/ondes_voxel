// Movement in water and lava, checked against published speeds.
//
// A terminal speed is the cleanest possible test of a drag: under a constant
// acceleration `a` and a per-tick drag `d`, a velocity settles at
// a * 0.98 / (1 - d), and the game's own published figures are in metres per
// second, which is twenty times that. If the drag, the acceleration or the
// input scale is wrong by anything at all, the settled speed misses.
//
// Six of these are published. All six are reproduced below to within a
// hundredth of a metre per second, which is finer than the published figures
// are quoted.

#include "ov/gameplay/physics.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <optional>

using namespace ov;
using namespace ov::gameplay;
using Catch::Approx;

namespace {

/// The pack, loaded once. Generated locally and never committed, so every test
/// here skips rather than fails when it is absent.
[[nodiscard]] const std::optional<registry::BlockRegistry>& pack() {
    static const auto loaded = [] {
        const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                          "registry.ovpack";
        std::optional<registry::BlockRegistry> out;
        if (auto blocks = registry::BlockRegistry::load(path)) {
            out = std::move(*blocks);
        }
        return out;
    }();
    return loaded;
}

/// A world with nothing solid in it. Every movement is allowed, so what is
/// measured is the physics and not the collision.
[[nodiscard]] registry::BlockStateId nothing(void*, i32, i32, i32) {
    return registry::kAirState;
}

/// Water everywhere, a full block deep.
[[nodiscard]] FluidSample all_water(void*, i32, i32, i32) {
    return FluidSample{Fluid::Water, 1.0};
}

/// Lava everywhere.
[[nodiscard]] FluidSample all_lava(void*, i32, i32, i32) {
    return FluidSample{Fluid::Lava, 1.0};
}

[[nodiscard]] FluidSample nothing_wet(void*, i32, i32, i32) {
    return FluidSample{};
}

/// Blocks per tick, as the game reports metres per second.
[[nodiscard]] f64 metres_per_second(f64 blocks_per_tick) {
    return blocks_per_tick * 20.0;
}

/// Run until it stops changing, and report the distance covered in one tick.
///
/// The distance and not the stored velocity: the game's published speeds are
/// how far a player travels, which is the velocity at the moment of the move —
/// after the tick's acceleration and before its drag. Reading the velocity
/// afterwards instead reports the same movement multiplied by the drag, which
/// is a fifth low and looks like a plausible constant being wrong.
[[nodiscard]] f64 settled_horizontal_speed(const FluidWorld& fluids, bool sprint) {
    const CollisionWorld world{*pack(), nothing, nullptr};

    MotionConstants constants;
    MoveInput       input;
    input.forward = 1.0F;
    input.yaw     = 0.0F;
    input.sprint  = sprint;

    MotionState state;
    state.position = Vec3d{0.5, 64.0, 0.5};
    for (int tick = 0; tick < 400; ++tick) {
        state = step(state, input, constants, world, &fluids);
    }
    const Vec3d before = state.position;
    state              = step(state, input, constants, world, &fluids);
    return std::hypot(state.position.x - before.x, state.position.z - before.z);
}

}  // namespace

TEST_CASE("swimming settles at the published speed", "[gameplay][fluid]") {
    if (!pack()) {
        SKIP("no registry pack");
    }
    FluidWorld water{all_water, nullptr};

    // Published: 1.97 m/s swimming, 3.918 m/s sprint-swimming. The constants
    // give 1.960 and 3.920 — the second to four significant figures, and the
    // first inside the precision the figure is quoted at.
    CHECK(metres_per_second(settled_horizontal_speed(water, false)) == Approx(1.96).margin(0.01));
    CHECK(metres_per_second(settled_horizontal_speed(water, true)) == Approx(3.92).margin(0.01));
}

TEST_CASE("sinking settles at the published speed", "[gameplay][fluid]") {
    if (!pack()) {
        SKIP("no registry pack");
    }
    const CollisionWorld world{*pack(), nothing, nullptr};
    MotionConstants      constants;
    MoveInput            idle;

    // Lava is the one with a published number: 0.8 m/s with fire resistance,
    // and 0.02 / (1 - 0.5) is exactly 0.04 blocks a tick.
    {
        FluidWorld  lava{all_lava, nullptr};
        MotionState state;
        state.position = Vec3d{0.5, 64.0, 0.5};
        for (int tick = 0; tick < 400; ++tick) {
            state = step(state, idle, constants, world, &lava);
        }
        CHECK(metres_per_second(-state.velocity.y) == Approx(0.8).margin(0.001));
    }

    // Water has no published sink rate. The constants give half a metre a
    // second, which is written down here so that a change to them shows up as
    // a failure rather than as a slightly different game.
    {
        FluidWorld  water{all_water, nullptr};
        MotionState state;
        state.position = Vec3d{0.5, 64.0, 0.5};
        for (int tick = 0; tick < 400; ++tick) {
            state = step(state, idle, constants, world, &water);
        }
        CHECK(metres_per_second(-state.velocity.y) == Approx(0.5).margin(0.001));
    }
}

TEST_CASE("a fluid changes the movement, not just its speed", "[gameplay][fluid]") {
    if (!pack()) {
        SKIP("no registry pack");
    }
    const CollisionWorld world{*pack(), nothing, nullptr};
    MotionConstants      constants;

    MotionState start;
    start.position = Vec3d{0.5, 64.0, 0.5};

    // Holding jump in water is a stroke upward, and it beats gravity: a player
    // in water rises. In air the same input does nothing at all while falling.
    MoveInput jump;
    jump.jump = true;

    FluidWorld  water{all_water, nullptr};
    MotionState swimming = start;
    for (int tick = 0; tick < 40; ++tick) {
        swimming = step(swimming, jump, constants, world, &water);
    }
    CHECK(swimming.position.y > start.position.y);

    FluidWorld  dry{nothing_wet, nullptr};
    MotionState falling = start;
    for (int tick = 0; tick < 40; ++tick) {
        falling = step(falling, jump, constants, world, &dry);
    }
    CHECK(falling.position.y < start.position.y);

    // And sneaking sinks faster than doing nothing.
    MoveInput sneak;
    sneak.sneak = true;
    MotionState sinking = start;
    MotionState drifting = start;
    for (int tick = 0; tick < 40; ++tick) {
        sinking  = step(sinking, sneak, constants, world, &water);
        drifting = step(drifting, MoveInput{}, constants, world, &water);
    }
    CHECK(sinking.position.y < drifting.position.y);
}

TEST_CASE("a puddle is not a swimming pool", "[gameplay][fluid]") {
    if (!pack()) {
        SKIP("no registry pack");
    }
    // On the ground with shallow water, a jump is still a jump. Without the
    // threshold, wading through ankle-deep water would turn into swimming and
    // the player would never leave the ground.
    const CollisionWorld world{*pack(), nothing, nullptr};
    MotionConstants      constants;

    // Water in one layer only. A lookup that answers water at every height is
    // an ocean, not a puddle, and the depth it reports is the whole column.
    const auto shallow = [](void*, i32, i32 y, i32) {
        return y == 64 ? FluidSample{Fluid::Water, 0.25} : FluidSample{};
    };
    FluidWorld puddle{shallow, nullptr};

    MotionState state;
    state.position  = Vec3d{0.5, 64.0, 0.5};
    state.on_ground = true;

    MoveInput jump;
    jump.jump = true;

    const MotionState after = step(state, jump, constants, world, &puddle);
    // A real jump, not a 0.04 stroke: the difference is a factor of ten.
    CHECK(after.velocity.y > 0.3);
}
