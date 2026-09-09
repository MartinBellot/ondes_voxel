// The goal selector, and enough of the goals to prove it is one.
//
// The thing worth testing here is not any individual behaviour — it is the
// arbitration. A bag of goals with priorities and control flags behaves quite
// differently from a state machine at exactly the moments that matter: two
// goals that need different controls run together, two that need the same one
// do not, and a lower-priority goal cannot take a control a higher-priority one
// already holds.
#include "ov/gameplay/goals.hpp"

#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

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

/// Flat stone under y = 0, air above.
class FlatLevel final : public world::LevelView {
public:
    explicit FlatLevel(const registry::BlockRegistry& registry) : registry_{&registry} {
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

/// A goal that records what happened to it and can be told what to answer.
class ScriptedGoal final : public Goal {
public:
    ScriptedGoal(std::string label, GoalFlag flags) : label_{std::move(label)}, flags_{flags} {}

    bool usable{false};
    i32  starts{0};
    i32  stops{0};
    i32  ticks{0};

    [[nodiscard]] bool     can_use(GoalContext&) override { return usable; }
    void                   start(GoalContext&) override { ++starts; }
    void                   stop(GoalContext&) override { ++stops; }
    void                   tick(GoalContext&) override { ++ticks; }
    [[nodiscard]] GoalFlag flags() const noexcept override { return flags_; }
    [[nodiscard]] std::string_view name() const noexcept override { return label_; }

private:
    std::string label_;
    GoalFlag    flags_;
};

struct Rig {
    FlatLevel                level;
    entity::EntityWorld      entities;
    math::LegacyRandomSource random{12345};
    MobBrain                 brain{512};

    explicit Rig(const registry::BlockRegistry& b, const registry::Registries& r)
        : level{b}, entities{r} {}

    [[nodiscard]] GoalContext context(entity::EntityHandle self, i64 tick) {
        GoalContext out;
        out.level    = &level;
        out.entities = &entities;
        out.self     = self;
        out.brain    = &brain;
        out.tick     = tick;
        out.random   = &random;
        return out;
    }
};

}  // namespace

TEST_CASE("two goals wanting different controls run together", "[gameplay][goals]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Rig        rig{*blocks(), *registries()};
    const auto self = rig.entities.spawn("minecraft:cow", Vec3d{0.5, 0.0, 0.5}, net::Uuid{}).value();

    auto  mover_owned  = std::make_unique<ScriptedGoal>("mover", GoalFlag::Move);
    auto  looker_owned = std::make_unique<ScriptedGoal>("looker", GoalFlag::Look);
    auto* mover        = mover_owned.get();
    auto* looker       = looker_owned.get();

    GoalSelector selector;
    selector.add(1, std::move(mover_owned));
    selector.add(2, std::move(looker_owned));

    mover->usable  = true;
    looker->usable = true;
    GoalContext context = rig.context(self, 0);
    selector.tick(context);

    CHECK(selector.is_running("mover"));
    CHECK(selector.is_running("looker"));
    CHECK(mover->starts == 1);
    CHECK(looker->starts == 1);
}

TEST_CASE("two goals wanting the same control do not, and priority decides which",
          "[gameplay][goals]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Rig        rig{*blocks(), *registries()};
    const auto self = rig.entities.spawn("minecraft:cow", Vec3d{0.5, 0.0, 0.5}, net::Uuid{}).value();

    auto  urgent_owned = std::make_unique<ScriptedGoal>("urgent", GoalFlag::Move);
    auto  idle_owned   = std::make_unique<ScriptedGoal>("idle", GoalFlag::Move);
    auto* urgent       = urgent_owned.get();
    auto* idle         = idle_owned.get();

    GoalSelector selector;
    // Added in the wrong order on purpose: priority must decide, not insertion.
    selector.add(5, std::move(idle_owned));
    selector.add(1, std::move(urgent_owned));

    urgent->usable = true;
    idle->usable   = true;
    GoalContext context = rig.context(self, 0);
    selector.tick(context);

    CHECK(selector.is_running("urgent"));
    CHECK_FALSE(selector.is_running("idle"));

    // The urgent one goes away; the idle one may now have the control — but not
    // in the same tick it was released, because a running goal is only stopped
    // in step one and starts happen in step two of the *next* pass.
    urgent->usable = false;
    selector.tick(context);
    CHECK_FALSE(selector.is_running("urgent"));
    CHECK(selector.is_running("idle"));
    CHECK(urgent->stops == 1);
}

TEST_CASE("a higher-priority goal takes a control from a running lower-priority one",
          "[gameplay][goals]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Rig        rig{*blocks(), *registries()};
    const auto self = rig.entities.spawn("minecraft:cow", Vec3d{0.5, 0.0, 0.5}, net::Uuid{}).value();

    auto  urgent_owned = std::make_unique<ScriptedGoal>("urgent", GoalFlag::Move);
    auto  idle_owned   = std::make_unique<ScriptedGoal>("idle", GoalFlag::Move);
    auto* urgent       = urgent_owned.get();
    auto* idle         = idle_owned.get();

    GoalSelector selector;
    selector.add(1, std::move(urgent_owned));
    selector.add(5, std::move(idle_owned));

    idle->usable        = true;
    GoalContext context = rig.context(self, 0);
    selector.tick(context);
    REQUIRE(selector.is_running("idle"));

    // The urgent goal becomes usable while `idle` is still perfectly happy to
    // continue. It takes the control anyway, and `idle` is stopped — which is
    // the only thing that makes a priority number mean anything. A selector
    // that waited for the control to be released would let a chicken finish
    // its stroll before panicking.
    urgent->usable = true;
    selector.tick(context);
    CHECK(selector.is_running("urgent"));
    CHECK_FALSE(selector.is_running("idle"));
    CHECK(idle->stops == 1);
}

namespace {

/// A goal that refuses to be interrupted, like one mid-swing.
class StubbornGoal final : public Goal {
public:
    bool usable{false};

    [[nodiscard]] bool     can_use(GoalContext&) override { return usable; }
    [[nodiscard]] GoalFlag flags() const noexcept override { return GoalFlag::Move; }
    [[nodiscard]] bool     interruptible() const noexcept override { return false; }
    [[nodiscard]] std::string_view name() const noexcept override { return "stubborn"; }
};

}  // namespace

TEST_CASE("a goal that says it must finish is not evicted", "[gameplay][goals]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Rig        rig{*blocks(), *registries()};
    const auto self = rig.entities.spawn("minecraft:cow", Vec3d{0.5, 0.0, 0.5}, net::Uuid{}).value();

    auto  urgent_owned   = std::make_unique<ScriptedGoal>("urgent", GoalFlag::Move);
    auto  stubborn_owned = std::make_unique<StubbornGoal>();
    auto* urgent         = urgent_owned.get();
    auto* stubborn       = stubborn_owned.get();

    GoalSelector selector;
    selector.add(1, std::move(urgent_owned));
    selector.add(5, std::move(stubborn_owned));

    stubborn->usable    = true;
    GoalContext context = rig.context(self, 0);
    selector.tick(context);
    REQUIRE(selector.is_running("stubborn"));

    urgent->usable = true;
    selector.tick(context);
    CHECK(selector.is_running("stubborn"));
    CHECK_FALSE(selector.is_running("urgent"));
}

TEST_CASE("a mob told to move walks towards the block it was given", "[gameplay][goals]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Rig        rig{*blocks(), *registries()};
    const auto self =
        rig.entities.spawn("minecraft:zombie", Vec3d{0.5, 0.0, 0.5}, net::Uuid{}).value();
    rig.brain.size = MobSize::from_box(0.6F, 1.95F);

    GoalContext context = rig.context(self, 0);
    REQUIRE(move_to(context, BlockPos{8, 0, 0}, 1.0));
    REQUIRE_FALSE(rig.brain.follower.done());

    Vec3d waypoint{};
    REQUIRE(rig.brain.follower.next_waypoint(Vec3d{0.5, 0.0, 0.5}, 0.6F, waypoint));
    // The first waypoint the mob has not already reached is the second node,
    // since it is standing on the first.
    CHECK(waypoint.x > 0.5);
    CHECK(waypoint.z == 0.5);
}

TEST_CASE("the nearest entity is the nearest, and ties go to the older one",
          "[gameplay][goals]") {
    if (registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    entity::EntityWorld world{*registries()};
    const auto self = world.spawn("minecraft:zombie", Vec3d{0.0, 0.0, 0.0}, net::Uuid{}).value();
    const auto near_ = world.spawn("minecraft:cow", Vec3d{3.0, 0.0, 0.0}, net::Uuid{}).value();
    const auto far   = world.spawn("minecraft:cow", Vec3d{9.0, 0.0, 0.0}, net::Uuid{}).value();
    (void)far;

    CHECK(nearest_entity(world, self, -1, 16.0) == near_);

    const auto cow_type = world.state(near_)->type;
    CHECK(nearest_entity(world, self, cow_type, 16.0) == near_);
    // Out of range is nothing at all, not the closest thing out of range.
    CHECK(nearest_entity(world, self, cow_type, 2.0) == entity::kNoEntity);

    // A tie: the earlier-spawned one wins, so the choice never depends on
    // storage order.
    const auto tie_a = world.spawn("minecraft:pig", Vec3d{0.0, 0.0, 5.0}, net::Uuid{}).value();
    const auto tie_b = world.spawn("minecraft:pig", Vec3d{0.0, 0.0, -5.0}, net::Uuid{}).value();
    (void)tie_b;
    const auto pig_type = world.state(tie_a)->type;
    CHECK(nearest_entity(world, self, pig_type, 16.0) == tie_a);
}

TEST_CASE("a panicking animal takes the move control from the wanderer",
          "[gameplay][goals]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    Rig        rig{*blocks(), *registries()};
    const auto self = rig.entities.spawn("minecraft:cow", Vec3d{0.5, 0.0, 0.5}, net::Uuid{}).value();
    rig.brain.size  = MobSize::from_box(0.9F, 1.4F);

    auto  panic_owned = std::make_unique<PanicGoal>();
    auto* panic       = panic_owned.get();
    GoalSelector selector;
    selector.add(1, std::move(panic_owned));
    selector.add(6, std::make_unique<RandomStrollGoal>());

    GoalContext context = rig.context(self, 0);
    // Nothing has frightened it: it may wander, and eventually will.
    bool wandered = false;
    for (i64 tick = 0; tick < 2000 && !wandered; ++tick) {
        context.tick = tick;
        selector.tick(context);
        wandered = selector.is_running("stroll");
    }
    CHECK(wandered);

    panic->frighten(60);
    for (i64 tick = 2000; tick < 2010; ++tick) {
        context.tick = tick;
        selector.tick(context);
    }
    CHECK(selector.is_running("panic"));
    CHECK_FALSE(selector.is_running("stroll"));
}

TEST_CASE("a target behind a wall is not acquired, and one in the open is",
          "[gameplay][goals]") {
    if (blocks() == nullptr || registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    // The rule the maze oracle ran into head-first: vanilla's target selector
    // is gated on line of sight, and a wall denies it.
    struct Walled {
        registry::BlockStateId stone{};
        registry::BlockStateId air{};
        bool                   wall{true};

        static registry::BlockStateId look_up(void* context, i32 x, i32 y, i32 z) {
            const auto* self = static_cast<const Walled*>(context);
            if (y < 0) {
                return self->stone;
            }
            if (self->wall && x == 3 && y >= 0 && y <= 3 && z >= -8 && z <= 8) {
                return self->stone;
            }
            return self->air;
        }
    };
    Walled walled{blocks()->default_state(blocks()->find_block("minecraft:stone").value()),
                  blocks()->default_state(blocks()->find_block("minecraft:air").value()), true};
    CollisionWorld collisions{*blocks(), &Walled::look_up, &walled};

    entity::EntityWorld world{*registries()};
    const auto zombie = world.spawn("minecraft:zombie", Vec3d{0.5, 0.0, 0.5}, net::Uuid{}).value();
    const auto cow    = world.spawn("minecraft:cow", Vec3d{6.5, 0.0, 0.5}, net::Uuid{}).value();

    CHECK_FALSE(has_line_of_sight(collisions, *world.state(zombie), *world.state(cow)));
    walled.wall = false;
    CHECK(has_line_of_sight(collisions, *world.state(zombie), *world.state(cow)));
}
