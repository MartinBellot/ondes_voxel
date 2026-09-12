// Entity motion, against the fall the game itself performed.
//
// scripts/measure_entity_fall.py drops a mob from y = 300 on a real 1.20.1
// server and samples its `Motion` tag forty times on the way down. The curve
// below is what that fit produced; what is checked here is that our step
// reproduces it, and that the box a mob is stopped by is its own measured one.
#include "ov/gameplay/entity_physics.hpp"

#include "ov/registry/registries.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <tuple>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

[[nodiscard]] const registry::BlockRegistry* blocks() {
    static const auto loaded = registry::BlockRegistry::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

[[nodiscard]] const registry::Registries* registries() {
    static const auto loaded = registry::Registries::load(pack_path());
    return loaded ? &*loaded : nullptr;
}

/// A floor at y = 0 and nothing else.
struct Floor {
    registry::BlockStateId stone{};

    static registry::BlockStateId look_up(void* context, i32 /*x*/, i32 y, i32 /*z*/) {
        const auto* self = static_cast<const Floor*>(context);
        return y < 0 ? self->stone : registry::BlockStateId{0};
    }
};

[[nodiscard]] entity::EntityState mob_at(f64 y, f32 width = 0.6F, f32 height = 1.95F) {
    entity::EntityState state;
    state.position = Vec3d{0.5, y, 0.5};
    state.width    = width;
    state.height   = height;
    return state;
}

}  // namespace

TEST_CASE("a falling mob follows the curve the game's Motion tag traced",
          "[gameplay][entity]") {
    if (blocks() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Floor          floor{blocks()->default_state(blocks()->find_block("minecraft:stone").value())};
    CollisionWorld world{*blocks(), &Floor::look_up, &floor};

    entity::EntityState state = mob_at(200.0);
    const EntityMotionConstants constants;

    // v(n+1) = (v(n) - g)·d, closed form -g·d·(1 - dⁿ)/(1 - d). Both sides of
    // the comparison come from the same two numbers, but the point is the
    // *order*: gravity then drag. Applying drag first passes a test written
    // against itself and fails against the game.
    f64 expected = 0.0;
    for (int tick = 0; tick < 60; ++tick) {
        expected = (expected - constants.gravity) * constants.vertical_drag;
        state    = step_entity(state, constants, world);
        REQUIRE(state.velocity.y == Catch::Approx(expected).epsilon(1e-12));
        REQUIRE_FALSE(state.on_ground);
    }
    // Sixty ticks in the closed form says -2.7536, which is well inside the
    // range the game's own samples covered on the way down.
    CHECK(state.velocity.y == Catch::Approx(-2.75359).epsilon(1e-5));
}

TEST_CASE("the terminal speed is the one a long fall converges on", "[gameplay][entity]") {
    if (blocks() == nullptr) {
        return;
    }
    Floor          floor{blocks()->default_state(blocks()->find_block("minecraft:stone").value())};
    CollisionWorld world{*blocks(), &Floor::look_up, &floor};

    entity::EntityState         state = mob_at(100000.0);
    const EntityMotionConstants constants;
    for (int tick = 0; tick < 2000; ++tick) {
        state = step_entity(state, constants, world);
    }
    // -g·d/(1-d) = -3.92. The measurement's fastest observed sample was -3.58,
    // still on its way here.
    // With the game's float drag, 0.98F, the limit sits a hair past -3.92.
    const f64 terminal =
        -constants.gravity * constants.vertical_drag / (1.0 - constants.vertical_drag);
    CHECK(state.velocity.y == Catch::Approx(terminal).epsilon(1e-9));
    CHECK(state.velocity.y == Catch::Approx(-3.92).epsilon(1e-5));

    // A dropped stack falls at half the gravity, and so converges on half the
    // speed. Measured separately, and the fit was exact.
    entity::EntityState item = mob_at(100000.0, 0.25F, 0.25F);
    for (int tick = 0; tick < 2000; ++tick) {
        item = step_entity(item, item_motion(), world);
    }
    CHECK(item.velocity.y == Catch::Approx(-1.96).epsilon(1e-9));
}

TEST_CASE("a mob lands on the floor and stays on it", "[gameplay][entity]") {
    if (blocks() == nullptr) {
        return;
    }
    Floor          floor{blocks()->default_state(blocks()->find_block("minecraft:stone").value())};
    CollisionWorld world{*blocks(), &Floor::look_up, &floor};

    entity::EntityState         state = mob_at(10.0);
    const EntityMotionConstants constants;
    for (int tick = 0; tick < 200; ++tick) {
        state = step_entity(state, constants, world);
    }
    CHECK(state.position.y == Catch::Approx(0.0).margin(1e-9));
    CHECK(state.on_ground);
    CHECK(state.velocity.y == 0.0);

    // And it stays grounded rather than flickering. Gravity is applied every
    // tick whether the mob is standing or not, so "on the ground" has to mean
    // "the fall was cut short", not "the velocity is zero".
    for (int tick = 0; tick < 20; ++tick) {
        state = step_entity(state, constants, world);
        CHECK(state.on_ground);
        CHECK(state.position.y == Catch::Approx(0.0).margin(1e-9));
    }
}

TEST_CASE("the box is the entity's own measured one", "[gameplay][entity]") {
    if (registries() == nullptr) {
        return;
    }
    // A chicken is 0.4 wide and 0.7 tall; an enderman 0.6 by 2.9. Both come
    // from the measured table rather than from a constant here, so a mob that
    // changes size changes what stops it.
    const auto types    = registries()->find("minecraft:entity_type").value();
    const auto chicken  = registries()->protocol_id(types, "minecraft:chicken").value();
    const auto enderman = registries()->protocol_id(types, "minecraft:enderman").value();

    entity::EntityState bird;
    bird.position = Vec3d{0.0, 64.0, 0.0};
    const auto bird_info = registries()->entity_type(chicken).value();
    bird.width           = bird_info.width;
    bird.height          = bird_info.height;

    const AABB box = entity_box(bird);
    CHECK(box.min.x == Catch::Approx(-0.2).epsilon(1e-6));
    CHECK(box.max.x == Catch::Approx(0.2).epsilon(1e-6));
    CHECK(box.min.y == 64.0);
    CHECK(box.max.y == Catch::Approx(64.7).epsilon(1e-6));

    entity::EntityState tall;
    tall.position          = Vec3d{0.0, 64.0, 0.0};
    const auto tall_info   = registries()->entity_type(enderman).value();
    tall.height            = tall_info.height;
    tall.width             = tall_info.width;
    CHECK(entity_box(tall).max.y == Catch::Approx(66.9).epsilon(1e-6));
}
