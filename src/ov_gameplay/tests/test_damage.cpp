// The damage rules, against the server that defines them.
//
// Two kinds of test live here. The first kind pins behaviour that a formula
// predicts — a bigger hit gets through a window a smaller one opened. The
// second kind replays a **measurement**: the thirty fall heights that were
// dropped on a real 1.20.1 server, and the fifteen tick gaps that were fired at
// it through a datapack function. Those cases are the ones worth having,
// because they are the ones that were wrong before they were measured.
#include "ov/gameplay/damage.hpp"

#include "ov/io/file.hpp"
#include "ov/nbt/binary.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

using namespace ov;
using namespace ov::gameplay;

namespace {

constexpr DamageConstants kDefaults{};

/// Fall the way physics.hpp says a body falls, and stop where the game stops
/// counting: the tick that lands does not add its own drop.
///
/// This is the model, not a table. Reproducing thirty measured heights from the
/// same two constants the movement code already uses is a much stronger claim
/// than storing thirty numbers and reading them back.
[[nodiscard]] f32 damage_for_drop(f64 blocks) {
    constexpr f64 kGravity = 0.08;
    constexpr f64 kDrag    = 0.98;
    HealthState   state;
    f64           velocity = 0.0;
    f64           y        = blocks;
    while (true) {
        velocity = (velocity - kGravity) * kDrag;
        if (y + velocity <= 0.0) {
            // This step reaches the floor. On_ground for this tick, and the
            // step itself is never counted.
            return accumulate_fall(state, y + velocity - y, true, kDefaults);
        }
        const f64 before = y;
        y += velocity;
        (void)accumulate_fall(state, y - before, false, kDefaults);
    }
}

}  // namespace

TEST_CASE("the damage type table is the datapack's, entry for entry", "[damage]") {
    REQUIRE(damage_type(DamageKind::Cactus).name == "minecraft:cactus");
    REQUIRE(damage_type(DamageKind::Fall).name == "minecraft:fall");
    REQUIRE(damage_type(DamageKind::LightningBolt).name == "minecraft:lightning_bolt");

    // The alphabetical position of a name is its id in the codec we send, and
    // the client picks a death message by that number. Two of them were read
    // straight off the wire from a real server: cactus arrived as 2, lightning
    // as 23.
    REQUIRE(static_cast<int>(DamageKind::Cactus) == 2);
    REQUIRE(static_cast<int>(DamageKind::LightningBolt) == 23);
    REQUIRE(static_cast<int>(DamageKind::GenericKill) == 17);

    // Every entry is reachable by name, and no two share one.
    for (usize i = 0; i < kDamageKindCount; ++i) {
        const auto kind = static_cast<DamageKind>(i);
        const auto back = damage_kind_from_name(damage_type(kind).name);
        REQUIRE(back.has_value());
        REQUIRE(*back == kind);
    }
    REQUIRE_FALSE(damage_kind_from_name("minecraft:not_a_damage_type").has_value());
}

TEST_CASE("exhaustion and tags come from the data, not from a guess", "[damage]") {
    // Zero for the impersonal, a tenth for anything that is somebody's fault.
    REQUIRE(damage_type(DamageKind::Fall).exhaustion == Catch::Approx(0.0F));
    REQUIRE(damage_type(DamageKind::Starve).exhaustion == Catch::Approx(0.0F));
    REQUIRE(damage_type(DamageKind::Cactus).exhaustion == Catch::Approx(0.1F));
    REQUIRE(damage_type(DamageKind::PlayerAttack).exhaustion == Catch::Approx(0.1F));

    REQUIRE(has(damage_type(DamageKind::Fall).flags, DamageFlags::IsFall));
    REQUIRE(has(damage_type(DamageKind::Fall).flags, DamageFlags::BypassesArmor));
    REQUIRE(has(damage_type(DamageKind::Lava).flags, DamageFlags::IsFire));
    REQUIRE(has(damage_type(DamageKind::Drown).flags, DamageFlags::IsDrowning));

    // Exactly two types ignore the invulnerability window in 1.20.1. Any more
    // and a cactus would be able to kill through it.
    usize bypassing = 0;
    for (usize i = 0; i < kDamageKindCount; ++i) {
        if (has(damage_type(static_cast<DamageKind>(i)).flags,
                DamageFlags::BypassesInvulnerability)) {
            ++bypassing;
        }
    }
    REQUIRE(bypassing == 2);
    REQUIRE(has(damage_type(DamageKind::OutOfWorld).flags, DamageFlags::BypassesInvulnerability));
    REQUIRE(has(damage_type(DamageKind::GenericKill).flags, DamageFlags::BypassesInvulnerability));
}

TEST_CASE("death message keys are the client's own", "[damage]") {
    std::array<char, 64> buffer{};
    const usize          length = death_message_key(DamageKind::Fall, buffer.data(), buffer.size());
    REQUIRE(std::string_view{buffer.data(), length} == "death.attack.fall");

    // The key is not the registry name. minecraft:in_fire is "inFire", and a
    // literal translation of the id produces a sentence the client cannot find.
    const usize in_fire = death_message_key(DamageKind::InFire, buffer.data(), buffer.size());
    REQUIRE(std::string_view{buffer.data(), in_fire} == "death.attack.inFire");

    // A buffer too small is refused rather than truncated.
    std::array<char, 4> tiny{};
    REQUIRE(death_message_key(DamageKind::Fall, tiny.data(), tiny.size()) == 0);
}

TEST_CASE("fall damage reproduces all thirty measured heights", "[damage][parity]") {
    // Measured on a real 1.20.1 server: a cow with a two-hundred-point health
    // bar dropped from y = floor + h, for h = 1..30, and its Health read back.
    // Index 0 is a drop of one block.
    //
    // The dips are the point of the table. A drop of twelve blocks costs eight
    // and not nine, because the tick that lands is worth more than a block at
    // that speed and is never counted. A naive ceil(h - 3) reproduces 23 of
    // these 30; the model below reproduces all 30.
    constexpr std::array<f32, 30> kMeasured{
        0.0F,  0.0F,  0.0F,  1.0F,  2.0F,  3.0F,  4.0F,  5.0F,  6.0F,  7.0F,
        8.0F,  8.0F,  10.0F, 11.0F, 12.0F, 13.0F, 13.0F, 15.0F, 16.0F, 16.0F,
        18.0F, 19.0F, 19.0F, 21.0F, 21.0F, 23.0F, 24.0F, 24.0F, 26.0F, 26.0F,
    };
    for (usize i = 0; i < kMeasured.size(); ++i) {
        const f64 blocks = static_cast<f64>(i + 1);
        INFO("dropped from " << blocks << " blocks");
        REQUIRE(damage_for_drop(blocks) == Catch::Approx(kMeasured[i]));
    }
}

TEST_CASE("fall damage rounds up, and it matters", "[damage]") {
    // A ceiling, not a floor. Measured: fallDistance 10.8065 cost eight points,
    // and floor would have made it seven. Applying floor instead reproduces
    // three of the thirty measured heights.
    REQUIRE(fall_damage(10.8065F, kDefaults) == Catch::Approx(8.0F));
    REQUIRE(fall_damage(3.0F, kDefaults) == Catch::Approx(0.0F));
    REQUIRE(fall_damage(3.0001F, kDefaults) == Catch::Approx(1.0F));
    REQUIRE(fall_damage(0.0F, kDefaults) == Catch::Approx(0.0F));
    REQUIRE(fall_damage(-5.0F, kDefaults) == Catch::Approx(0.0F));
}

TEST_CASE("the invulnerability window is ten ticks wide", "[damage][parity]") {
    // Measured with a datapack function, which is the only way to control the
    // gap to the tick: two four-point hits fired at gaps of 0 to 14. The second
    // one was swallowed through a gap of ten and landed at eleven.
    constexpr std::array<f32, 15> kMeasured{4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F, 4.0F,
                                            4.0F, 4.0F, 4.0F, 8.0F, 8.0F, 8.0F, 8.0F};
    for (usize gap = 0; gap < kMeasured.size(); ++gap) {
        HealthState state{.health = 200.0F, .max_health = 200.0F};
        (void)apply_damage(state, DamageKind::Generic, 4.0F, kDefaults);
        for (usize t = 0; t < gap; ++t) {
            tick_health(state, kDefaults);
        }
        (void)apply_damage(state, DamageKind::Generic, 4.0F, kDefaults);
        INFO("gap of " << gap << " ticks");
        REQUIRE(200.0F - state.health == Catch::Approx(kMeasured[gap]));
    }
}

TEST_CASE("a stronger hit gets through, a weaker one does not", "[damage][parity]") {
    // Both measured in the same tick, as two datapack functions.
    {
        HealthState state{.health = 200.0F, .max_health = 200.0F};
        (void)apply_damage(state, DamageKind::Generic, 4.0F, kDefaults);
        const DamageResult second = apply_damage(state, DamageKind::Generic, 9.0F, kDefaults);
        REQUIRE(second.applied);
        REQUIRE(second.dealt == Catch::Approx(5.0F));
        REQUIRE(200.0F - state.health == Catch::Approx(9.0F));
    }
    {
        HealthState state{.health = 200.0F, .max_health = 200.0F};
        (void)apply_damage(state, DamageKind::Generic, 9.0F, kDefaults);
        const DamageResult second = apply_damage(state, DamageKind::Generic, 4.0F, kDefaults);
        REQUIRE_FALSE(second.applied);
        REQUIRE(second.absorbed);
        REQUIRE(200.0F - state.health == Catch::Approx(9.0F));
    }
}

TEST_CASE("some damage ignores the window entirely", "[damage]") {
    HealthState state{.health = 20.0F};
    (void)apply_damage(state, DamageKind::Generic, 4.0F, kDefaults);
    // out_of_world is in #bypasses_invulnerability, so the void does not wait.
    const DamageResult void_hit = apply_damage(state, DamageKind::OutOfWorld, 4.0F, kDefaults);
    REQUIRE(void_hit.applied);
    REQUIRE(state.health == Catch::Approx(12.0F));
}

TEST_CASE("death is reported once and stays", "[damage]") {
    HealthState state{.health = 3.0F};
    const DamageResult killing = apply_damage(state, DamageKind::Fall, 5.0F, kDefaults);
    REQUIRE(killing.killed);
    REQUIRE(state.dead);
    REQUIRE(state.health == Catch::Approx(0.0F));

    // Nothing happens to something already dead — a corpse hit again must not
    // produce a second death message.
    const DamageResult again = apply_damage(state, DamageKind::Fall, 5.0F, kDefaults);
    REQUIRE_FALSE(again.applied);
    REQUIRE_FALSE(again.killed);
}

TEST_CASE("air runs out, then costs two points every second", "[damage]") {
    HealthState state;
    REQUIRE(state.air == 300);

    f32 total = 0.0F;
    for (int tick = 0; tick < 300; ++tick) {
        total += tick_air(state, true, kDefaults);
    }
    // Three hundred ticks of breath and not one point of damage in them.
    REQUIRE(total == Catch::Approx(0.0F));
    REQUIRE(state.air == 0);

    // Then two points every twenty ticks.
    f32 first_second = 0.0F;
    for (int tick = 0; tick < 20; ++tick) {
        first_second += tick_air(state, true, kDefaults);
    }
    REQUIRE(first_second == Catch::Approx(2.0F));

    // Surfacing refills it, and faster than it drained.
    state.air = 0;
    for (int tick = 0; tick < 75; ++tick) {
        (void)tick_air(state, false, kDefaults);
    }
    REQUIRE(state.air == 300);
}

TEST_CASE("a fall interrupted is two falls", "[damage]") {
    HealthState state;
    // Ten blocks down, a ledge, then ten more. Charged as two ten-block falls
    // and not one twenty-block one, which is what the accumulator is for.
    for (int i = 0; i < 10; ++i) {
        REQUIRE(accumulate_fall(state, -1.0, false, kDefaults) == Catch::Approx(0.0F));
    }
    const f32 first = accumulate_fall(state, 0.0, true, kDefaults);
    REQUIRE(first == Catch::Approx(7.0F));
    REQUIRE(state.fall_distance == Catch::Approx(0.0F));

    for (int i = 0; i < 10; ++i) {
        (void)accumulate_fall(state, -1.0, false, kDefaults);
    }
    REQUIRE(accumulate_fall(state, 0.0, true, kDefaults) == Catch::Approx(7.0F));
}

TEST_CASE("the codec we ship numbers damage types the way this enum does",
          "[damage][parity]") {
    // The damage_type registry is one of the six the *server* sends, so its ids
    // are ours to choose — but the client uses whatever we sent to pick a death
    // message, and the survival code turns a DamageKind into an id by casting.
    // That cast is only correct if the codec lists the names in this order.
    //
    // So the codec is opened and read back. It is built from the vanilla
    // datapack and is gitignored, so a machine without it skips rather than
    // fails; a machine with it proves the cast for all forty-four entries.
    const auto path = std::filesystem::path{OV_SOURCE_DIR} / "data" / "vanilla" / "1.20.1" /
                      "registry_codec.nbt";
    if (!std::filesystem::exists(path)) {
        WARN("registry_codec.nbt missing; run the data generator");
        return;
    }
    const auto bytes = ov::io::read_file(path);
    REQUIRE(bytes.has_value());
    const auto document = ov::nbt::read(*bytes);
    REQUIRE(document.has_value());

    const ov::nbt::Tag* registry = document->root.find("minecraft:damage_type");
    REQUIRE(registry != nullptr);
    const ov::nbt::Tag* value = registry->find("value");
    REQUIRE(value != nullptr);
    REQUIRE(value->list() != nullptr);
    REQUIRE(value->list()->size() == kDamageKindCount);

    usize matched = 0;
    for (const ov::nbt::Tag& entry : *value->list()) {
        const ov::nbt::Tag* entry_name = entry.find("name");
        const ov::nbt::Tag* entry_id   = entry.find("id");
        REQUIRE(entry_name != nullptr);
        REQUIRE(entry_id != nullptr);
        const auto kind = damage_kind_from_name(entry_name->as_string());
        INFO("codec entry " << entry_name->as_string());
        REQUIRE(kind.has_value());
        REQUIRE(static_cast<i64>(*kind) == entry_id->as_i64());
        ++matched;
    }
    REQUIRE(matched == kDamageKindCount);
}
