// The measured sound tables, read back through the public accessors.
//
// What they assert is what the real 1.20.1 server was heard doing
// (scripts/measure_block_sounds.py, scripts/measure_sound_events.py,
// docs/provenance/son.md), not what a list says: stone's set, the audience of a
// door against a lever's, a cow's volume against a zombie's.
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>

using namespace ov;
using namespace ov::registry;
using Catch::Matchers::WithinAbs;
using Sounds = BlockRegistry::BlockSounds;

namespace {

[[nodiscard]] std::filesystem::path pack_path() {
    return std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" / "registry.ovpack";
}

const BlockRegistry& blocks() {
    static const auto loaded = BlockRegistry::load(pack_path());
    REQUIRE(loaded.has_value());
    return *loaded;
}

const Registries& registries() {
    static const auto loaded = Registries::load(pack_path());
    REQUIRE(loaded.has_value());
    return *loaded;
}

std::string_view sound_name(i32 id) {
    const auto registry = registries().find("minecraft:sound_event");
    REQUIRE(registry.has_value());
    return registries().entry_of(*registry, id);
}

Sounds sounds_of(std::string_view block) {
    const auto id = blocks().find_block(block);
    REQUIRE(id.has_value());
    const auto sounds = blocks().sounds(*id);
    REQUIRE(sounds.has_value());
    return *sounds;
}

i32 entity_type(std::string_view name) {
    const auto registry = registries().find("minecraft:entity_type");
    REQUIRE(registry.has_value());
    const auto id = registries().protocol_id(*registry, name);
    REQUIRE(id.has_value());
    return static_cast<i32>(*id);
}

}  // namespace

TEST_CASE("block sounds: stone is the stone set, three gestures heard, two inferred",
          "[registry][sound]") {
    const Sounds stone = sounds_of("minecraft:stone");
    CHECK(sound_name(stone.event(Sounds::Place)) == "minecraft:block.stone.place");
    CHECK(sound_name(stone.event(Sounds::Step)) == "minecraft:block.stone.step");
    CHECK(sound_name(stone.event(Sounds::Break)) == "minecraft:block.stone.break");
    CHECK(sound_name(stone.event(Sounds::Hit)) == "minecraft:block.stone.hit");
    CHECK(sound_name(stone.event(Sounds::Fall)) == "minecraft:block.stone.fall");
    CHECK(stone.volume == 1.0F);
    CHECK(stone.pitch == 1.0F);
    CHECK((stone.measured & Sounds::kPlaceHeard) != 0);
    CHECK((stone.measured & Sounds::kStepHeard) != 0);
    CHECK((stone.measured & Sounds::kBreakHitInferred) != 0);
    CHECK(stone.event(Sounds::Open) == -1);
}

TEST_CASE("block sounds: air sounds of nothing", "[registry][sound]") {
    CHECK_FALSE(blocks().sounds(blocks().block_of(kAirState)).has_value());
}

TEST_CASE("block sounds: a door leaves out its opener, a lever does not", "[registry][sound]") {
    const Sounds door = sounds_of("minecraft:oak_door");
    CHECK(sound_name(door.event(Sounds::Open)) == "minecraft:block.wooden_door.open");
    CHECK(sound_name(door.event(Sounds::Close)) == "minecraft:block.wooden_door.close");
    CHECK(door.open_audience == Sounds::Audience::Others);
    CHECK(door.close_audience == Sounds::Audience::Others);
    CHECK(door.open_pitch_lo >= 0.9F);
    CHECK(door.open_pitch_hi <= 1.0F);

    const Sounds lever = sounds_of("minecraft:lever");
    CHECK(sound_name(lever.event(Sounds::Open)) == "minecraft:block.lever.click");
    CHECK(lever.open_audience == Sounds::Audience::Everyone);
    CHECK_THAT(lever.open_volume, WithinAbs(0.3, 1e-6));
    CHECK_THAT(lever.open_pitch_lo, WithinAbs(0.6, 1e-6));
    CHECK_THAT(lever.close_pitch_lo, WithinAbs(0.5, 1e-6));

    // Pressed by a hand: the others. Released by a tick: everyone.
    const Sounds button = sounds_of("minecraft:stone_button");
    CHECK(sound_name(button.event(Sounds::Open)) == "minecraft:block.stone_button.click_on");
    CHECK(sound_name(button.event(Sounds::Close)) == "minecraft:block.stone_button.click_off");
    CHECK(button.open_audience == Sounds::Audience::Others);
    CHECK(button.close_audience == Sounds::Audience::Everyone);
}

TEST_CASE("entity sounds: a cow is neutral at 0.4, a zombie hostile at 1", "[registry][sound]") {
    const auto cow = registries().entity_sounds(entity_type("minecraft:cow"));
    REQUIRE(cow.has_value());
    CHECK(sound_name(cow->hurt) == "minecraft:entity.cow.hurt");
    CHECK(sound_name(cow->death) == "minecraft:entity.cow.death");
    CHECK(sound_name(cow->ambient) == "minecraft:entity.cow.ambient");
    CHECK(cow->category == 6);
    CHECK_THAT(cow->volume, WithinAbs(0.4, 1e-6));
    CHECK(cow->pitch_lo >= 0.8F);
    CHECK(cow->pitch_hi <= 1.2F);

    const auto zombie = registries().entity_sounds(entity_type("minecraft:zombie"));
    REQUIRE(zombie.has_value());
    CHECK(sound_name(zombie->hurt) == "minecraft:entity.zombie.hurt");
    CHECK(zombie->category == 5);
    CHECK(zombie->volume == 1.0F);
}
