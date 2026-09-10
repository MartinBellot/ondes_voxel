// The dispenser's table against what a real 1.20.1 server did with each item.
//
// The dropper is the control in the measurement and it is the control here too:
// it has no per-item behaviour at all, and a test that says so is what stops
// the dispenser's table from leaking into it.
//
// See docs/provenance/redstone.md §13.
#include "ov/gameplay/dispenser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>

using namespace ov;
using namespace ov::gameplay;

namespace {

struct Loaded {
    std::optional<registry::Registries> registries;
    std::optional<Dispenser>            dispenser;
};

[[nodiscard]] const Loaded& loaded() {
    static const Loaded state = [] {
        const auto path =
            std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
        Loaded out;
        auto   regs = registry::Registries::load(path);
        if (regs) {
            out.registries = std::move(*regs);
            out.dispenser.emplace(*out.registries);
        }
        return out;
    }();
    return state;
}

#define REQUIRE_REGISTRY()                                                \
    if (!loaded().dispenser.has_value()) {                                \
        SKIP("registry.ovpack not built");                                \
    }                                                                     \
    [[maybe_unused]] const Dispenser& dispenser = *loaded().dispenser;    \
    const registry::Registries& registries = *loaded().registries;        \
    const auto items = registries.find("minecraft:item");                 \
    REQUIRE(items.has_value());                                           \
    const auto id = [&](std::string_view name) {                          \
        const auto found = registries.protocol_id(*items, name);          \
        REQUIRE(found.has_value());                                       \
        return *found;                                                    \
    };                                                                    \
    (void)id

}  // namespace

TEST_CASE("a bucket is replaced, not consumed", "[dispenser]") {
    REQUIRE_REGISTRY();
    // Measured: the cell came back holding `minecraft:bucket` with water on the
    // target square. A rule that consumed the bucket would look right for one
    // shot and destroy a bucket on every one after.
    const DispenseAction water = dispenser.action_for(id("minecraft:water_bucket"));
    CHECK(water.kind == DispenseKind::PlaceFluid);
    CHECK(water.block == "minecraft:water");
    CHECK(water.replacement == "minecraft:bucket");
    CHECK_FALSE(water.damages);

    const DispenseAction lava = dispenser.action_for(id("minecraft:lava_bucket"));
    CHECK(lava.kind == DispenseKind::PlaceFluid);
    CHECK(lava.block == "minecraft:lava");
    CHECK(lava.replacement == "minecraft:bucket");
}

TEST_CASE("a flint and steel is damaged, not spent", "[dispenser]") {
    REQUIRE_REGISTRY();
    // Measured: it came back with `tag: {Damage: 1}` and fire on the target
    // square. It is neither consumed nor replaced.
    const DispenseAction action = dispenser.action_for(id("minecraft:flint_and_steel"));
    CHECK(action.kind == DispenseKind::Ignite);
    CHECK(action.block == "minecraft:fire");
    CHECK(action.damages);
    CHECK(action.replacement.empty());
}

TEST_CASE("dispensed tnt is primed", "[dispenser]") {
    REQUIRE_REGISTRY();
    // The measurement's own dispenser did not survive its cell: reading it back
    // answered "the target block is not a block entity". A dropped stick of tnt
    // would have left the machine standing.
    const DispenseAction action = dispenser.action_for(id("minecraft:tnt"));
    CHECK(action.kind == DispenseKind::PrimeTnt);
    CHECK(action.entity == "minecraft:tnt");
}

TEST_CASE("the projectiles are projectiles and not items on the floor",
          "[dispenser]") {
    REQUIRE_REGISTRY();
    struct Case {
        std::string_view item;
        std::string_view entity;
    };
    // Snowball and firework rocket are the two the campaign nearly got wrong.
    // A thrown snowball and a dropped snowball are both called "Snowball", and
    // the display name says they are the same thing. Asking whether the entity
    // is a `minecraft:item` says they are not.
    for (const Case& c : std::array<Case, 6>{
             {{"minecraft:arrow", "minecraft:arrow"},
              {"minecraft:egg", "minecraft:egg"},
              {"minecraft:fire_charge", "minecraft:small_fireball"},
              {"minecraft:splash_potion", "minecraft:potion"},
              {"minecraft:snowball", "minecraft:snowball"},
              {"minecraft:firework_rocket", "minecraft:firework_rocket"}}}) {
        const DispenseAction action = dispenser.action_for(id(c.item));
        INFO(c.item);
        CHECK(action.kind == DispenseKind::Projectile);
        CHECK(action.entity == c.entity);
    }
}

TEST_CASE("a spawn egg spawns what it names, by construction", "[dispenser]") {
    REQUIRE_REGISTRY();
    // Eighty spawn eggs tabulated by hand would be eighty chances to mistype
    // one. Measured on a pig, derived for the rest.
    const DispenseAction pig = dispenser.action_for(id("minecraft:pig_spawn_egg"));
    CHECK(pig.kind == DispenseKind::SpawnMob);
    CHECK(pig.entity == "minecraft:pig");

    const DispenseAction creeper = dispenser.action_for(id("minecraft:creeper_spawn_egg"));
    CHECK(creeper.kind == DispenseKind::SpawnMob);
    CHECK(creeper.entity == "minecraft:creeper");
}

TEST_CASE("an ordinary block is ejected, which is not a behaviour",
          "[dispenser]") {
    REQUIRE_REGISTRY();
    // Measured: the dispenser and the dropper agreed, and what appeared was a
    // plain `minecraft:item`. Agreement with the dropper is the definition of
    // "fell through to the default".
    // The ender pearl is the one worth naming: a dispenser really does not
    // throw it, and every guess says it should.
    for (const std::string_view name : {"minecraft:cobblestone", "minecraft:ender_pearl",
                                        "minecraft:leather_helmet", "minecraft:stone"}) {
        INFO(name);
        CHECK(dispenser.action_for(id(name)).kind == DispenseKind::Eject);
    }
}

TEST_CASE("a dropper has no per-item behaviour at all", "[dispenser]") {
    REQUIRE_REGISTRY();
    // The control in the measurement, and the control here. If this ever stops
    // being true it will be because the dispenser's table leaked.
    CHECK(Dispenser::dropper_action().kind == DispenseKind::Eject);
    CHECK(Dispenser::dropper_action().entity.empty());
}

TEST_CASE("what is not measured is refused by name", "[dispenser]") {
    REQUIRE_REGISTRY();
    // This project refuses an unimplemented case rather than treating it as the
    // default, and here the default is plausible for every one of them: a
    // shulker box on the floor instead of placed looks like a dispenser that
    // works.
    CHECK_FALSE(dispenser.unhandled().empty());
    for (const std::string_view name : dispenser.unhandled()) {
        INFO(name);
        const DispenseAction action = dispenser.action_for(id(name));
        CHECK(action.kind == DispenseKind::Refused);
        CHECK_FALSE(action.refusal.empty());
    }
    // Bone meal and shears were in the measurement and came back having
    // produced no entity at all while leaving the machine empty: the item did
    // something, and this table does not know what. Refused, loudly.
    CHECK(dispenser.action_for(id("minecraft:bone_meal")).kind == DispenseKind::Refused);
    CHECK(dispenser.action_for(id("minecraft:shears")).kind == DispenseKind::Refused);
    // A boat and a glass bottle came back as plain ejected items — but only
    // because the target square was air. Both depend on what is in front of
    // them, and recording the air case as the whole rule is recording half of
    // one.
    CHECK(dispenser.action_for(id("minecraft:oak_boat")).kind == DispenseKind::Refused);
    CHECK(dispenser.action_for(id("minecraft:glass_bottle")).kind == DispenseKind::Refused);
}

TEST_CASE("an item id off the end of the registry is refused, not ejected",
          "[dispenser]") {
    REQUIRE_REGISTRY();
    CHECK(dispenser.action_for(registry::ProtocolId{1 << 20}).kind == DispenseKind::Refused);
    CHECK(dispenser.action_for(registry::ProtocolId{-1}).kind == DispenseKind::Refused);
}

TEST_CASE("a machine fires its first non-empty slot", "[dispenser]") {
    REQUIRE_REGISTRY();
    SimpleContainer machine{9, 64};
    CHECK(Dispenser::slot_to_fire(machine) == -1);
    machine.set_slot(4, SlotStack{registry::ProtocolId{7}, 1, 0});
    CHECK(Dispenser::slot_to_fire(machine) == 4);
    machine.set_slot(1, SlotStack{registry::ProtocolId{8}, 1, 0});
    // Vanilla draws a random one; this takes the first, and says so in the
    // header rather than pretending the two agree.
    CHECK(Dispenser::slot_to_fire(machine) == 1);
}
