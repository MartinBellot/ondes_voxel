// Behaviour driven through the entity world, rather than applied by hand.
//
// The point of IEntityLogic is that a zombie and a dropped stack differ in what
// they do and not in who calls them. These tests exercise that path: the world
// ticks, the logic falls, and an entity with no logic at all does nothing.
#include "ov/gameplay/mob_logic.hpp"

#include "ov/registry/registries.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

// ── A mob with a brain ──────────────────────────────────────────────────────

namespace {

/// Flat stone below y = 0, air above, and a LevelView the goals can read.
class WalkLevel final : public world::LevelView {
public:
    explicit WalkLevel(const registry::BlockRegistry& registry) : registry_{&registry} {
        air_   = registry.default_state(registry.find_block("minecraft:air").value());
        stone_ = registry.default_state(registry.find_block("minecraft:stone").value());
    }

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        return pos.y < 0 ? stone_ : air_;
    }
    [[nodiscard]] bool                   is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::overworld(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *registry_; }

private:
    const registry::BlockRegistry* registry_;
    registry::BlockStateId         air_{};
    registry::BlockStateId         stone_{};
};

}  // namespace

TEST_CASE("every shipped species has a kind, and only those", "[gameplay][entity][logic]") {
    CHECK(mob_kinds().size() == 20);  // ── mobs-2 ── eight of M2, twelve new
    for (const MobKind& kind : mob_kinds()) {
        CHECK(mob_kind(kind.type_name) == &kind);
        // The two tables agree with each other: a species this module knows how
        // to run is one the spawner knows how to place.
        CHECK(category_of(kind.type_name) == kind.category);
    }
    // Refused by name rather than given a plausible default.
    CHECK(mob_kind("minecraft:ender_dragon") == nullptr);
}

TEST_CASE("a mob that is walking covers its measured speed each tick",
          "[gameplay][entity][logic][parity]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Floor          floor{blocks()->default_state(blocks()->find_block("minecraft:stone").value())};
    CollisionWorld collisions{*blocks(), &Floor::look_up, &floor};
    WalkLevel      level{*blocks()};

    entity::EntityWorld world{*registries()};
    const auto zombie = world.spawn("minecraft:zombie", Vec3d{0.5, 0.0, 0.5}, net::Uuid{}).value();

    const MobKind* kind = mob_kind("minecraft:zombie");
    REQUIRE(kind != nullptr);
    world.set_logic(zombie, std::make_unique<Mob>(*kind, 0.6F, 1.95F, 1));

    MobContext context{&collisions, &level, false};
    f64        travelled = 0.0;
    i32        moving    = 0;
    Vec3d      previous  = world.state(zombie)->position;
    for (i64 tick = 0; tick < 600; ++tick) {
        world.tick(entity::TickContext{tick, &context});
        const Vec3d now  = world.state(zombie)->position;
        const f64   dx   = now.x - previous.x;
        const f64   dz   = now.z - previous.z;
        const f64   step = std::sqrt(dx * dx + dz * dz);
        if (step > 1e-6) {
            travelled += step;
            ++moving;
        }
        previous = now;
    }
    // It must actually have gone somewhere: the goals are what choose to walk,
    // and a mob that never strolls in six hundred ticks has a brain that is not
    // running at all.
    REQUIRE(moving > 20);
    const f64 per_tick = travelled / static_cast<f64>(moving);
    // Measured on a real 1.20.1 server: a chasing zombie covers 0.11419 blocks
    // a tick. Ours is driven at the same number, and this checks that the drag
    // the physics step applies afterwards has been accounted for — without that
    // compensation this reads about 0.062, which is 45 % low and looks entirely
    // plausible in a screenshot.
    CHECK(per_tick > 0.10);
    CHECK(per_tick < 0.13);
}

TEST_CASE("a mob with a level decides things; one without only falls",
          "[gameplay][entity][logic]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Floor          floor{blocks()->default_state(blocks()->find_block("minecraft:stone").value())};
    CollisionWorld collisions{*blocks(), &Floor::look_up, &floor};

    entity::EntityWorld world{*registries()};
    const auto cow = world.spawn("minecraft:cow", Vec3d{0.5, 6.0, 0.5}, net::Uuid{}).value();
    world.set_logic(cow, std::make_unique<Mob>(*mob_kind("minecraft:cow"), 0.9F, 1.4F, 7));

    // No level in the context: it falls to the floor and stays put. Every
    // decision a goal makes needs blocks, so with no blocks there are no
    // decisions — stated rather than silently half-running.
    MobContext context{&collisions, nullptr, false};
    for (i64 tick = 0; tick < 200; ++tick) {
        world.tick(entity::TickContext{tick, &context});
    }
    const Vec3d resting = world.state(cow)->position;
    CHECK(resting.y == 0.0);
    CHECK(resting.x == 0.5);
    CHECK(resting.z == 0.5);
}
