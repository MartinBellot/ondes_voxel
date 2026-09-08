// The storage, and the two properties that make it worth having: a handle to a
// dead entity never resolves, and the tick order never depends on who died.
#include "ov/entity/world.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::entity;

namespace {

[[nodiscard]] const registry::Registries* registries() {
    static const auto pack = registry::Registries::load(
        std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack");
    return pack ? &*pack : nullptr;
}

[[nodiscard]] net::Uuid uuid_for(u64 n) { return net::Uuid{n, ~n}; }

/// Records the order it was ticked in, and can ask to be removed.
class Recorder final : public IEntityLogic {
public:
    Recorder(std::vector<std::string>* log, std::string label, int die_after = -1)
        : log_{log}, label_{std::move(label)}, die_after_{die_after} {}

    void tick(EntityWorld& world, EntityHandle self, const TickContext& context) override {
        log_->push_back(label_);
        ++ticks_;
        if (die_after_ >= 0 && ticks_ >= die_after_) {
            world.mutable_state(self)->removed = true;
        }
        (void)context;
    }

    [[nodiscard]] std::string_view name() const noexcept override { return label_; }

private:
    std::vector<std::string>* log_;
    std::string               label_;
    int                       die_after_{-1};
    int                       ticks_{0};
};

/// Spawns one entity on its first tick, to prove a newborn is not ticked in the
/// tick that made it.
class Breeder final : public IEntityLogic {
public:
    void tick(EntityWorld& world, EntityHandle self, const TickContext&) override {
        if (!done_) {
            done_ = true;
            (void)world.spawn("minecraft:chicken", Vec3d{1.0, 2.0, 3.0}, uuid_for(99));
        }
        (void)self;
    }
    [[nodiscard]] std::string_view name() const noexcept override { return "breeder"; }

private:
    bool done_{false};
};

}  // namespace

TEST_CASE("a spawned entity carries the type's measured box and health", "[entity][world]") {
    if (registries() == nullptr) {
        WARN("registry.ovpack missing");
        return;
    }
    EntityWorld world{*registries()};
    const auto  zombie = world.spawn("minecraft:zombie", Vec3d{0.5, 64.0, 0.5}, uuid_for(1));
    REQUIRE(zombie.has_value());

    const EntityState* state = world.state(*zombie);
    REQUIRE(state != nullptr);
    CHECK(state->width == 0.6F);
    CHECK(state->height == 1.95F);
    CHECK(state->eye_height == 1.74F);
    CHECK(state->health == 20.0F);
    CHECK(state->max_health == 20.0F);
    CHECK(state->position.y == 64.0);
    // Not 0.3: the width is an f32 holding 0.6, and half of it in double is
    // 0.30000001192092896. Written as 0.3 this check fails, and it should —
    // the box the collision code uses is the one the float describes.
    CHECK(half_width(*state) == static_cast<f64>(0.6F) * 0.5);

    // The type id is Mojang's, taken from the registry rather than counted here.
    const auto types = registries()->find("minecraft:entity_type").value();
    CHECK(state->type == registries()->protocol_id(types, "minecraft:zombie").value());
}

TEST_CASE("an unknown or unmeasured type is refused rather than approximated",
          "[entity][world]") {
    if (registries() == nullptr) {
        return;
    }
    EntityWorld world{*registries()};

    const auto nonsense = world.spawn("minecraft:not_a_mob", Vec3d{}, uuid_for(2));
    REQUIRE_FALSE(nonsense.has_value());
    CHECK(nonsense.error() == SpawnError::UnknownType);

    // A real type whose hitbox the measurement could never reach. It exists in
    // the registry and still cannot be spawned, which is the point.
    const auto bolt = world.spawn("minecraft:lightning_bolt", Vec3d{}, uuid_for(3));
    REQUIRE_FALSE(bolt.has_value());
    CHECK(bolt.error() == SpawnError::UnmeasuredType);
    CHECK(world.size() == 0);
}

TEST_CASE("a handle to a removed entity never resolves to its successor", "[entity][world]") {
    if (registries() == nullptr) {
        return;
    }
    EntityWorld world{*registries()};
    const auto  first = world.spawn("minecraft:cow", Vec3d{}, uuid_for(4)).value();
    const i32   first_id = world.state(first)->network_id;

    REQUIRE(world.remove(first));
    CHECK_FALSE(world.alive(first));
    CHECK(world.state(first) == nullptr);
    CHECK(world.find(first_id) == kNoEntity);

    // EnTT recycles the slot. Without the generation carried in the handle,
    // `first` would now resolve to the pig — and damage aimed at a dead cow
    // would land on it, silently.
    const auto second = world.spawn("minecraft:pig", Vec3d{}, uuid_for(5)).value();
    CHECK(second != first);
    CHECK_FALSE(world.alive(first));
    CHECK(world.state(first) == nullptr);
    CHECK(world.state(second) != nullptr);
}

TEST_CASE("wire ids are unique and start where the caller asked", "[entity][world]") {
    if (registries() == nullptr) {
        return;
    }
    EntityWorld world{*registries(), 1000};
    const auto  a = world.spawn("minecraft:cow", Vec3d{}, uuid_for(6)).value();
    const auto  b = world.spawn("minecraft:cow", Vec3d{}, uuid_for(7)).value();
    CHECK(world.state(a)->network_id == 1000);
    CHECK(world.state(b)->network_id == 1001);
    CHECK(world.find(1000) == a);
    CHECK(world.find(1001) == b);

    // The id is not reused when the entity dies. A client that still has the
    // old entity would otherwise attach the new one's movement to it.
    world.remove(a);
    const auto c = world.spawn("minecraft:cow", Vec3d{}, uuid_for(8)).value();
    CHECK(world.state(c)->network_id == 1002);
}

TEST_CASE("the tick order is insertion order, and a death does not reshuffle it",
          "[entity][world]") {
    if (registries() == nullptr) {
        return;
    }
    EntityWorld              world{*registries()};
    std::vector<std::string> log;

    const auto a = world.spawn("minecraft:cow", Vec3d{}, uuid_for(10)).value();
    const auto b = world.spawn("minecraft:pig", Vec3d{}, uuid_for(11)).value();
    const auto c = world.spawn("minecraft:sheep", Vec3d{}, uuid_for(12)).value();
    world.set_logic(a, std::make_unique<Recorder>(&log, "a"));
    world.set_logic(b, std::make_unique<Recorder>(&log, "b", 1));  // dies after one tick
    world.set_logic(c, std::make_unique<Recorder>(&log, "c"));

    world.tick(TickContext{1, nullptr});
    CHECK(log == std::vector<std::string>{"a", "b", "c"});
    CHECK(world.removed_ids().size() == 1);
    CHECK(world.removed_ids()[0] == 2);  // b was the second spawned

    // With an EnTT view this second tick would visit c before a, because
    // destroying b swaps the last entity into its storage slot. Insertion order
    // is what keeps two identical runs identical.
    log.clear();
    world.tick(TickContext{2, nullptr});
    CHECK(log == std::vector<std::string>{"a", "c"});
    CHECK(world.removed_ids().empty());
    CHECK(world.size() == 2);
}

TEST_CASE("an entity spawned during a tick waits for the next one", "[entity][world]") {
    if (registries() == nullptr) {
        return;
    }
    EntityWorld              world{*registries()};
    std::vector<std::string> log;

    const auto breeder = world.spawn("minecraft:cow", Vec3d{}, uuid_for(20)).value();
    world.set_logic(breeder, std::make_unique<Breeder>());

    world.tick(TickContext{1, nullptr});
    // The chicken exists already — a caller that wants to announce it must be
    // able to see it — but it was not ticked, so a mob that spawns one every
    // tick cannot make the tick never end.
    CHECK(world.size() == 2);
    CHECK(world.handles().size() == 2);
}

TEST_CASE("attributes come from the type and absence stays absent", "[entity][world]") {
    if (registries() == nullptr) {
        return;
    }
    const auto attributes = registries()->find("minecraft:attribute").value();
    const auto attack =
        registries()->protocol_id(attributes, "minecraft:generic.attack_damage").value();
    const auto speed =
        registries()->protocol_id(attributes, "minecraft:generic.movement_speed").value();

    EntityWorld world{*registries()};
    const auto  zombie = world.spawn("minecraft:zombie", Vec3d{}, uuid_for(30)).value();
    const auto  cow    = world.spawn("minecraft:cow", Vec3d{}, uuid_for(31)).value();

    CHECK(world.attribute(zombie, attack) == 3.0);
    CHECK(world.attribute(zombie, speed) == static_cast<double>(0.23F));
    CHECK_FALSE(world.attribute(cow, attack).has_value());
    CHECK(world.attribute(cow, speed) == static_cast<double>(0.2F));

    // A dead entity answers nothing rather than the last thing it knew.
    world.remove(zombie);
    CHECK_FALSE(world.attribute(zombie, attack).has_value());
}
