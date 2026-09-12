// Blocks that change movement: ice, slime, honey, soul sand, ladders, cobwebs,
// bubble columns. The figures are the ones scripts/measure_block_motion.py read
// off a real 1.20.1 server (docs/provenance/physique-blocs.md).
#include "ov/gameplay/block_motion.hpp"
#include "ov/gameplay/entity_physics.hpp"
#include "ov/gameplay/physics.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string_view>
#include <utility>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] registry::BlockStateId state(const std::string& name) {
    return blocks()->default_state(blocks()->find_block(name).value());
}

/// A sparse world: a floor of one block at y = -1, plus whatever is placed.
struct Scene {
    explicit Scene(registry::BlockStateId ground) : floor{ground} {}

    registry::BlockStateId                                      floor{};
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> placed{};

    static registry::BlockStateId look_up(void* context, i32 x, i32 y, i32 z) {
        const auto* self = static_cast<const Scene*>(context);
        if (const auto found = self->placed.find({x, y, z}); found != self->placed.end()) {
            return found->second;
        }
        return y == -1 ? self->floor : registry::kAirState;
    }
};

[[nodiscard]] f64 settled_walk(const CollisionWorld& world, bool sprint = false) {
    MotionState     state{Vec3d{0.5, 0.0, 0.5}, Vec3d{}, true};
    MoveInput       input;
    input.forward = 1.0F;
    input.sprint  = sprint;
    const MotionConstants constants;
    f64                   last = 0.0;
    for (int tick = 0; tick < 600; ++tick) {
        const Vec3d before = state.position;
        state              = step(state, input, constants, world);
        last = std::hypot(state.position.x - before.x, state.position.z - before.z);
    }
    return last;
}

}  // namespace

TEST_CASE("the table reads per-block friction, factors and the climbable tag", "[gameplay][blocks]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    CHECK(table.of(state("minecraft:stone")).friction == 0.6F);
    CHECK(table.of(state("minecraft:ice")).friction == 0.98F);
    CHECK(table.of(state("minecraft:packed_ice")).friction == 0.98F);
    CHECK(table.of(state("minecraft:blue_ice")).friction == 0.989F);
    CHECK(table.of(state("minecraft:slime_block")).friction == 0.8F);
    CHECK(table.of(state("minecraft:soul_sand")).speed_factor == 0.4F);
    CHECK(table.of(state("minecraft:honey_block")).speed_factor == 0.4F);
    CHECK(table.of(state("minecraft:honey_block")).jump_factor == 0.5F);
    CHECK(table.of(state("minecraft:soul_soil")).speed_factor == 1.0F);
    for (const char* name : {"minecraft:ladder", "minecraft:vine", "minecraft:scaffolding",
                             "minecraft:twisting_vines_plant", "minecraft:cave_vines"}) {
        CHECK(table.of(state(name)).climbable);
    }
    CHECK_FALSE(table.of(state("minecraft:stone")).climbable);

    const auto column = blocks()->find_block("minecraft:bubble_column").value();
    const auto drag   = blocks()->find_property(column, "drag").value();
    const auto down   = blocks()->with_property(blocks()->default_state(column), drag,
                                                static_cast<u16>(0));
    const auto up     = blocks()->with_property(blocks()->default_state(column), drag,
                                                static_cast<u16>(1));
    // Which index is "true" is the registry's business; one of each must come out.
    CHECK(table.bubble(down) != table.bubble(up));
    CHECK(table.bubble(state("minecraft:water")) == BubbleColumn::None);
}

TEST_CASE("without a table every block is ordinary ground, as before", "[gameplay][blocks]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    Scene                scene{state("minecraft:ice")};
    const CollisionWorld plain{*blocks(), &Scene::look_up, &scene};
    CHECK(std::abs(settled_walk(plain) - 0.21578) < 0.0005);
}

TEST_CASE("walking on ice follows the cubed-slipperiness law", "[gameplay][blocks]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    // Cruise = 0.1·0.98·(0.6/f)³ / (1 − 0.91·f): barely slower on ice, and a
    // little faster on blue ice, though both take far longer to get there.
    for (const auto& [name, friction] : std::initializer_list<std::pair<const char*, f64>>{
             {"minecraft:stone", 0.6}, {"minecraft:ice", static_cast<f64>(0.98F)},
             {"minecraft:blue_ice", static_cast<f64>(0.989F)}}) {
        Scene                scene{state(name)};
        const CollisionWorld world{*blocks(), &Scene::look_up, &scene, &table};
        const f64            ratio    = 0.6 / friction;
        const f64            expected = 0.1 * 0.98 * ratio * ratio * ratio / (1.0 - 0.91 * friction);
        INFO(name);
        CHECK(settled_walk(world) == Catch::Approx(expected).epsilon(1e-6));
    }
}

TEST_CASE("soul sand slows a walk by its speed factor", "[gameplay][blocks]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    Scene                  scene{state("minecraft:soul_sand")};
    const CollisionWorld   world{*blocks(), &Scene::look_up, &scene, &table};
    // The factor multiplies the velocity after each move, so the push of the
    // tick itself is untouched and cruise is a / (1 − 0.4·0.546) — 58 % of a
    // normal walk, not 40 %.
    const f64 expected = 0.1 * 0.98 / (1.0 - 0.4 * 0.546);
    CHECK(settled_walk(world) == Catch::Approx(expected).epsilon(1e-3));
}

TEST_CASE("a ladder clamps the fall, holds a sneaking player and climbs by collision",
          "[gameplay][blocks]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    Scene                  scene{state("minecraft:stone")};
    const std::array<std::pair<std::string_view, std::string_view>, 1> facing{
        {{"facing", "west"}}};
    const auto ladder =
        blocks()->state_for(blocks()->find_block("minecraft:ladder").value(), facing);
    for (i32 y = 0; y < 40; ++y) {
        scene.placed[{1, y, 0}] = state("minecraft:stone");
        scene.placed[{0, y, 0}] = ladder.value_or(state("minecraft:ladder"));
    }
    const CollisionWorld  world{*blocks(), &Scene::look_up, &scene, &table};
    const MotionConstants constants;

    // Falling: every move is clamped to 0.15.
    MotionState falling{Vec3d{0.5, 30.0, 0.5}, Vec3d{}, false};
    for (int tick = 0; tick < 20; ++tick) {
        falling = step(falling, MoveInput{}, constants, world);
    }
    const f64 before = falling.position.y;
    falling          = step(falling, MoveInput{}, constants, world);
    CHECK(before - falling.position.y == Catch::Approx(static_cast<f64>(0.15F)).epsilon(1e-9));

    // Sneaking: held in place.
    MoveInput sneak;
    sneak.sneak     = true;
    const f64 held  = falling.position.y;
    for (int tick = 0; tick < 10; ++tick) {
        falling = step(falling, sneak, constants, world);
    }
    CHECK(falling.position.y == Catch::Approx(held).margin(1e-9));

    // Walking into the wall: 0.2 up, which is (0.2 − 0.08)·0.98 = 0.1176 a
    // tick once moving — 2.35 m/s, the published climbing speed.
    MotionState climbing{Vec3d{0.5, 0.0, 0.5}, Vec3d{}, true};
    MoveInput   east;
    east.forward = 1.0F;
    east.yaw     = -90.0F;  // facing +X, into the wall
    for (int tick = 0; tick < 30; ++tick) {
        climbing = step(climbing, east, constants, world);
    }
    const f64 low = climbing.position.y;
    climbing      = step(climbing, east, constants, world);
    CHECK(climbing.position.y - low == Catch::Approx(0.1176).epsilon(1e-6));
}

TEST_CASE("slime reflects a landing and sneaking cancels it", "[gameplay][blocks]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    Scene                  scene{state("minecraft:slime_block")};
    const CollisionWorld   world{*blocks(), &Scene::look_up, &scene, &table};
    const MotionConstants  constants;

    MotionState state{Vec3d{0.5, 10.0, 0.5}, Vec3d{}, false};
    f64         peak_after = 0.0;
    bool        bounced    = false;
    for (int tick = 0; tick < 120; ++tick) {
        state = step(state, MoveInput{}, constants, world);
        if (state.velocity.y > 0.0) {
            bounced = true;
        }
        if (bounced) {
            peak_after = std::max(peak_after, state.position.y);
        }
    }
    CHECK(bounced);
    CHECK(peak_after > 5.0);  // most of the ten blocks back

    MoveInput   sneak;
    sneak.sneak = true;
    MotionState careful{Vec3d{0.5, 10.0, 0.5}, Vec3d{}, false};
    for (int tick = 0; tick < 120; ++tick) {
        careful = step(careful, sneak, constants, world);
        CHECK(careful.velocity.y <= 0.0);
    }
    CHECK(careful.on_ground);
}

TEST_CASE("a cobweb scales each move and discards the velocity", "[gameplay][blocks]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    Scene                  scene{state("minecraft:stone")};
    for (i32 y = 0; y < 20; ++y) {
        scene.placed[{0, y, 0}] = state("minecraft:cobweb");
    }
    const CollisionWorld  world{*blocks(), &Scene::look_up, &scene, &table};
    const MotionConstants constants;
    MotionState           state{Vec3d{0.5, 10.0, 0.5}, Vec3d{}, false};
    state = step(state, MoveInput{}, constants, world);
    // Inside the web from the first tick: gravity rebuilds -0.0784 each tick
    // and only a twentieth of it (0.05F) is moved.
    for (int tick = 0; tick < 10; ++tick) {
        const f64 before = state.position.y;
        state            = step(state, MoveInput{}, constants, world);
        CHECK(before - state.position.y ==
              Catch::Approx(0.0784 * static_cast<f64>(0.05F)).epsilon(1e-6));
    }
}

// ── Against the real server's trace ─────────────────────────────────────────
//
// The numbers below are Motion values an armor stand stored, tick after tick,
// on a real 1.20.1 server (scripts/measure_block_motion.py). They are compared
// to the last representable digit the game printed.

TEST_CASE("bubble columns act once per block, as the traced armor stand shows",
          "[gameplay][blocks][parity]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    const auto             column = blocks()->find_block("minecraft:bubble_column").value();
    const MotionConstants  constants;

    struct Case {
        const char*          drag;
        const char*          bottom;
        f64                  start;
        std::array<f64, 8>   motion;
    };
    // The superflat's floor is at -61 in the trace and at 0 here: the column
    // runs from 1 to 10 with air above, the stand starts at 3 (up) or 9 (down).
    const std::array<Case, 2> cases{{
        {"false", "minecraft:soul_sand", 3.0,
         {0.0910000014, 0.2118000044, 0.3084400082, 0.3857520124, 0.4476016166, 0.4970813008,
          0.5366650487, 0.5550000083}},
        {"true", "minecraft:magma_block", 9.0,
         {-0.0530000007, -0.1194000023, -0.1725200043, -0.2150160066, -0.2490128089,
          -0.2690000039, -0.2450000036, -0.2450000036}},
    }};
    for (const Case& c : cases) {
        const std::array<std::pair<std::string_view, std::string_view>, 1> drag{{{"drag", c.drag}}};
        Scene scene{state(c.bottom)};
        scene.placed[{0, 0, 0}] = state(c.bottom);
        for (i32 y = 1; y <= 10; ++y) {
            scene.placed[{0, y, 0}] = blocks()->state_for(column, drag).value();
        }
        const CollisionWorld world{*blocks(), &Scene::look_up, &scene, &table};

        // The living water tick, by hand: move with the stored velocity, let
        // every column block the box overlaps act, then drag and gravity.
        Vec3d position{0.5, c.start, 0.5};
        Vec3d velocity{};
        for (usize tick = 0; tick < c.motion.size(); ++tick) {
            position.y += velocity.y;
            apply_inside_effects(world, AABB::from_entity(position, 0.5, 1.975), position, 0.5,
                                 false, velocity, constants.effects);
            velocity.y = velocity.y * constants.water_vertical_drag - constants.water_gravity;
            INFO(c.drag << " tick " << tick);
            CHECK(velocity.y == Catch::Approx(c.motion[tick]).margin(1e-10));
        }
    }
}

TEST_CASE("a mob keeps exactly float(f x 0.91F) of its speed per tick on the ground",
          "[gameplay][blocks][parity]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    // Traced ratios of successive on-ground Motion.x, armor stand launched at 0.8.
    for (const auto& [floor, ratio] : std::initializer_list<std::pair<const char*, f64>>{
             {"minecraft:stone", 0.546000063419},
             {"minecraft:ice", 0.891800045967},
             {"minecraft:packed_ice", 0.891800045967},
             {"minecraft:blue_ice", 0.899990022182},
             {"minecraft:soul_sand", 0.218400028622},
             {"minecraft:honey_block", 0.218400028622},
             {"minecraft:soul_soil", 0.546000063419}}) {
        Scene                scene{state(floor)};
        const CollisionWorld world{*blocks(), &Scene::look_up, &scene, &table};
        entity::EntityState  mob;
        mob.position = Vec3d{0.5, 0.0, 0.5};
        mob.width    = 0.5F;
        mob.height   = 1.975F;
        // Settle first: soul sand's top is an eighth below the cell, honey's
        // a sixteenth, and a mob hovering above them is in the air.
        for (int tick = 0; tick < 5; ++tick) {
            mob = step_entity(mob, EntityMotionConstants{}, world);
        }
        REQUIRE(mob.on_ground);
        mob.velocity.x = 0.8;
        const entity::EntityState next = step_entity(mob, EntityMotionConstants{}, world);
        const entity::EntityState after = step_entity(next, EntityMotionConstants{}, world);
        INFO(floor);
        CHECK(after.velocity.x / next.velocity.x == Catch::Approx(ratio).margin(1e-11));
    }
}

TEST_CASE("a mob on a ladder falls at 0.15F and climbs 0.1176 when walking into the wall",
          "[gameplay][blocks][parity]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    Scene                  scene{state("minecraft:stone")};
    const std::array<std::pair<std::string_view, std::string_view>, 1> facing{
        {{"facing", "west"}}};
    const auto ladder =
        blocks()->state_for(blocks()->find_block("minecraft:ladder").value(), facing).value();
    for (i32 y = 0; y < 40; ++y) {
        scene.placed[{1, y, 0}] = state("minecraft:stone");
        scene.placed[{0, y, 0}] = ladder;
    }
    const CollisionWorld world{*blocks(), &Scene::look_up, &scene, &table};

    entity::EntityState mob;
    mob.position = Vec3d{0.5, 30.0, 0.5};
    mob.width    = 0.5F;
    mob.height   = 1.975F;
    for (int tick = 0; tick < 5; ++tick) {
        mob = step_entity(mob, EntityMotionConstants{}, world);
    }
    // Traced: dy = -0.150000006 every tick once the clamp is reached.
    const f64 before = mob.position.y;
    mob              = step_entity(mob, EntityMotionConstants{}, world);
    CHECK(mob.position.y - before == Catch::Approx(-0.150000006).margin(1e-9));

    // Pushed into the wall at 0.1 a tick, as the trace's `pushx` stand was:
    // dy = 0.1176000023 = (0.2 - 0.08) x 0.98F, every tick.
    entity::EntityState climber;
    climber.position  = Vec3d{0.5, 0.0, 0.5};
    climber.on_ground = true;
    climber.width     = 0.5F;
    climber.height    = 1.975F;
    f64 dy            = 0.0;
    for (int tick = 0; tick < 10; ++tick) {
        climber.velocity.x = 0.1;
        const f64 low      = climber.position.y;
        climber            = step_entity(climber, EntityMotionConstants{}, world);
        dy                 = climber.position.y - low;
    }
    CHECK(dy == Catch::Approx(0.1176000023).margin(1e-9));
}

TEST_CASE("a mob slides further on ice than on stone", "[gameplay][blocks]") {
    if (blocks() == nullptr) {
        SKIP("no registry pack");
    }
    const BlockMotionTable table{*blocks()};
    const auto             slide = [&](const char* floor) {
        Scene                scene{state(floor)};
        const CollisionWorld world{*blocks(), &Scene::look_up, &scene, &table};
        entity::EntityState  mob;
        mob.position  = Vec3d{0.5, 0.0, 0.5};
        mob.velocity  = Vec3d{0.8, 0.0, 0.0};
        mob.on_ground = true;
        mob.width     = 0.5F;
        mob.height    = 1.975F;
        for (int tick = 0; tick < 200; ++tick) {
            mob = step_entity(mob, EntityMotionConstants{}, world);
        }
        return mob.position.x - 0.5;
    };
    const f64 stone = slide("minecraft:stone");
    const f64 ice   = slide("minecraft:ice");
    // Geometric: 0.8·k/(1−k) with k the per-tick keep, 0.546 against 0.8918.
    CHECK(stone == Catch::Approx(0.8 * 0.546 / (1.0 - 0.546)).epsilon(0.02));
    CHECK(ice > 5.0 * stone);
}
