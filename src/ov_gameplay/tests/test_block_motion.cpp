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
