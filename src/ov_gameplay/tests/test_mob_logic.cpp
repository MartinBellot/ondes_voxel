// Behaviour driven through the entity world, rather than applied by hand.
//
// The point of IEntityLogic is that a zombie and a dropped stack differ in what
// they do and not in who calls them. These tests exercise that path: the world
// ticks, the logic falls, and an entity with no logic at all does nothing.
#include "ov/gameplay/mob_logic.hpp"

#include "ov/registry/registries.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

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

struct Floor {
    registry::BlockStateId stone{};

    static registry::BlockStateId look_up(void* context, i32 /*x*/, i32 y, i32 /*z*/) {
        return y < 0 ? static_cast<const Floor*>(context)->stone : registry::BlockStateId{0};
    }
};

}  // namespace

TEST_CASE("a mob given the falling behaviour falls when the world ticks",
          "[gameplay][entity][logic]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Floor          floor{blocks()->default_state(blocks()->find_block("minecraft:stone").value())};
    CollisionWorld collisions{*blocks(), &Floor::look_up, &floor};

    entity::EntityWorld world{*registries()};
    const auto zombie = world.spawn("minecraft:zombie", Vec3d{0.5, 10.0, 0.5}, net::Uuid{}).value();
    const auto stack  = world.spawn("minecraft:item", Vec3d{4.5, 10.0, 0.5}, net::Uuid{}).value();

    world.set_logic(zombie, std::make_unique<FallingMob>());
    // The stack gets its own constants: half the gravity, measured separately
    // and exactly. Same behaviour object, different numbers — which is the
    // whole reason the constants are a parameter and not a constant.
    world.set_logic(stack, std::make_unique<FallingMob>(item_motion()));

    MobContext context{&collisions};
    for (i64 tick = 0; tick < 5; ++tick) {
        world.tick(entity::TickContext{tick, &context});
    }

    const f64 mob_y  = world.state(zombie)->position.y;
    const f64 item_y = world.state(stack)->position.y;
    CHECK(mob_y < 10.0);
    CHECK(item_y < 10.0);
    // Half the gravity is half the distance, tick for tick: the same closed
    // form with g halved. Not "roughly slower" — exactly half.
    CHECK(10.0 - item_y == Catch::Approx((10.0 - mob_y) * 0.5).epsilon(1e-12));

    for (i64 tick = 5; tick < 200; ++tick) {
        world.tick(entity::TickContext{tick, &context});
    }
    CHECK(world.state(zombie)->position.y == Catch::Approx(0.0).margin(1e-9));
    CHECK(world.state(zombie)->on_ground);
}

TEST_CASE("an entity with no behaviour does not move", "[gameplay][entity][logic]") {
    if (blocks() == nullptr || registries() == nullptr) {
        return;
    }
    Floor          floor{blocks()->default_state(blocks()->find_block("minecraft:stone").value())};
    CollisionWorld collisions{*blocks(), &Floor::look_up, &floor};

    entity::EntityWorld world{*registries()};
    const auto painting =
        world.spawn("minecraft:painting", Vec3d{0.5, 10.0, 0.5}, net::Uuid{}).value();

    MobContext context{&collisions};
    for (i64 tick = 0; tick < 40; ++tick) {
        world.tick(entity::TickContext{tick, &context});
    }
    // Still at ten. A painting has no logic component at all, which costs one
    // null pointer and no virtual call — and is what keeps "everything falls"
    // from being the default.
    CHECK(world.state(painting)->position.y == 10.0);
}

TEST_CASE("behaviour with no world in the context refuses to move the entity",
          "[gameplay][entity][logic]") {
    if (registries() == nullptr) {
        return;
    }
    entity::EntityWorld world{*registries()};
    const auto cow = world.spawn("minecraft:cow", Vec3d{0.5, 10.0, 0.5}, net::Uuid{}).value();
    world.set_logic(cow, std::make_unique<FallingMob>());

    // A caller that forgot to point the context at anything gets a mob that
    // does not move, rather than one that falls through the floor because there
    // was nothing to stop it.
    for (i64 tick = 0; tick < 40; ++tick) {
        world.tick(entity::TickContext{tick, nullptr});
    }
    CHECK(world.state(cow)->position.y == 10.0);
}
