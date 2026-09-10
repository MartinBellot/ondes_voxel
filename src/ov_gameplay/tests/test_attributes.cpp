// Attribute arithmetic, against totals a real 1.20.1 server printed.
//
// Every number below was read off `attribute … get` by
// scripts/measure_effects.py (campaigns `bounds`, `ops` and `order`), and is
// compared **exactly**: the console prints Java's Double.toString, which
// round-trips, so an off-by-one-ulp total is a failure here, not a tolerance.
#include "ov/gameplay/attributes.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::gameplay;

namespace {

[[nodiscard]] net::Uuid low(u64 value) { return net::Uuid{0, value}; }

/// A zombie's speed: the double nearest the float 0.23, measured.
constexpr f64 kZombieSpeed = 0.23000000417232513;

}  // namespace

TEST_CASE("the thirteen attributes are the registry's, in its order", "[gameplay][attributes]") {
    CHECK(attribute_info(Attribute::MaxHealth).name == "minecraft:generic.max_health");
    CHECK(attribute_info(Attribute::HorseJumpStrength).name == "minecraft:horse.jump_strength");
    for (usize i = 0; i < kAttributeCount; ++i) {
        const auto attribute = static_cast<Attribute>(i);
        CHECK(attribute_from_name(attribute_info(attribute).name) == attribute);
    }
    CHECK(!attribute_from_name("minecraft:generic.nonsense"));
}

TEST_CASE("the clamps, as measured with a billion either way", "[gameplay][attributes][parity]") {
    struct Row {
        Attribute attribute;
        f64       min;
        f64       max;
    };
    const Row rows[] = {
        {Attribute::MaxHealth, 1.0, 1024.0},           {Attribute::FollowRange, 0.0, 2048.0},
        {Attribute::KnockbackResistance, 0.0, 1.0},    {Attribute::MovementSpeed, 0.0, 1024.0},
        {Attribute::FlyingSpeed, 0.0, 1024.0},         {Attribute::AttackDamage, 0.0, 2048.0},
        {Attribute::AttackKnockback, 0.0, 5.0},        {Attribute::AttackSpeed, 0.0, 1024.0},
        {Attribute::Armor, 0.0, 30.0},                 {Attribute::ArmorToughness, 0.0, 20.0},
        {Attribute::Luck, -1024.0, 1024.0},            {Attribute::ZombieSpawnReinforcements, 0.0, 1.0},
        {Attribute::HorseJumpStrength, 0.0, 2.0},
    };
    for (const Row& row : rows) {
        AttributeInstance instance{row.attribute, 1'000'000'000.0};
        CHECK(instance.value() == row.max);
        // Stored as given: `base get` answered the billion.
        CHECK(instance.base() == 1'000'000'000.0);
        instance.set_base(-1'000'000'000.0);
        CHECK(instance.value() == row.min);
    }
}

TEST_CASE("the three operations, step by step, bit for bit", "[gameplay][attributes][parity]") {
    // `attribute @e[…] minecraft:generic.movement_speed modifier add …` on a
    // zombie, the total read after each one.
    AttributeInstance speed{Attribute::MovementSpeed, kZombieSpeed};
    const auto        add = [&](u64 id, f64 amount, AttributeOperation operation) {
        REQUIRE(speed.add_modifier({low(id), "ovprobe", amount, operation}));
        return speed.value();
    };
    CHECK(add(0xA001, 0.1, AttributeOperation::Addition) == 0.3300000041723251);
    CHECK(add(0xA002, 0.5, AttributeOperation::MultiplyBase) == 0.49500000625848767);
    CHECK(add(0xA003, 0.25, AttributeOperation::MultiplyTotal) == 0.6187500078231096);
    CHECK(add(0xA004, -0.5, AttributeOperation::MultiplyTotal) == 0.3093750039115548);
    CHECK(add(0xA005, 0.07, AttributeOperation::Addition) == 0.3750000039115548);
    CHECK(add(0xA006, -0.3, AttributeOperation::MultiplyBase) == 0.30000000312924385);

    // "Modifier … is already present on attribute Speed": the game refuses
    // the same UUID twice, and so does this.
    const auto again = speed.add_modifier({low(0xA001), "ovprobe", 0.3, AttributeOperation::Addition});
    REQUIRE(!again);
    CHECK(again.error() == ModifierError::AlreadyPresent);
}

TEST_CASE("inside one operation, hash-bucket order, not insertion order",
          "[gameplay][attributes][parity]") {
    // base + 0.1 + 0.07 and base + 0.07 + 0.1 differ in the last bit. …a009
    // sits in Java bucket 9 and …a002 in bucket 2. Inserting …a009 first and
    // reading the bucket-order bits back is what separates the two rules.
    CHECK(java_hash_bucket(low(0xA009), 16) == 9);
    CHECK(java_hash_bucket(low(0xA002), 16) == 2);

    struct Case {
        u64                first;
        f64                first_amount;
        u64                second;
        f64                second_amount;
        AttributeOperation operation;
        f64                measured;
    };
    const Case cases[] = {
        {0xA009, 0.1, 0xA002, 0.07, AttributeOperation::Addition, 0.4000000041723252},
        {0xA002, 0.07, 0xA009, 0.1, AttributeOperation::Addition, 0.4000000041723252},
        {0xA009, 0.07, 0xA002, 0.1, AttributeOperation::Addition, 0.4000000041723251},
        {0xB009, 0.1, 0xB002, 0.07, AttributeOperation::MultiplyTotal, 0.2707100049108267},
        {0xB002, 0.07, 0xB009, 0.1, AttributeOperation::MultiplyTotal, 0.2707100049108267},
    };
    for (const Case& c : cases) {
        AttributeInstance speed{Attribute::MovementSpeed, kZombieSpeed};
        REQUIRE(speed.add_modifier({low(c.first), "ovorder", c.first_amount, c.operation}));
        REQUIRE(speed.add_modifier({low(c.second), "ovorder", c.second_amount, c.operation}));
        CHECK(speed.value() == c.measured);
    }
}

TEST_CASE("clamps reached through modifiers", "[gameplay][attributes][parity]") {
    // The same zombie: max health 20 + 5000, knockback resistance 0 + 5,
    // armour 2 + 100. Measured 1024, 1 and 30.
    AttributeInstance health{Attribute::MaxHealth, 20.0};
    REQUIRE(health.add_modifier({low(0xB001), "ovclamp", 5000.0, AttributeOperation::Addition}));
    CHECK(health.value() == 1024.0);
    AttributeInstance knockback{Attribute::KnockbackResistance, 0.0};
    REQUIRE(knockback.add_modifier({low(0xB001), "ovclamp", 5.0, AttributeOperation::Addition}));
    CHECK(knockback.value() == 1.0);
    AttributeInstance armor{Attribute::Armor, 2.0};
    REQUIRE(armor.add_modifier({low(0xB001), "ovclamp", 100.0, AttributeOperation::Addition}));
    CHECK(armor.value() == 30.0);
    REQUIRE(armor.remove_modifier(low(0xB001)));
    CHECK(armor.value() == 2.0);
    CHECK(!armor.remove_modifier(low(0xB001)));
}

TEST_CASE("the player's own bases", "[gameplay][attributes][parity]") {
    // `attribute <bot> … base get` before anything touched them.
    const AttributeMap player = AttributeMap::player();
    CHECK(player.value(Attribute::MaxHealth) == 20.0);
    CHECK(player.value(Attribute::MovementSpeed) == 0.10000000149011612);
    CHECK(player.value(Attribute::AttackDamage) == 1.0);
    CHECK(player.value(Attribute::AttackSpeed) == 4.0);
    CHECK(player.value(Attribute::Luck) == 0.0);
    // A player has no follow range; absent is not zero.
    CHECK(!player.value(Attribute::FollowRange));
}

TEST_CASE("dirty tracks what the client has not been told", "[gameplay][attributes]") {
    AttributeInstance speed{Attribute::MovementSpeed, 0.1};
    CHECK(speed.dirty());
    speed.clear_dirty();
    speed.set_base(0.1);
    CHECK(!speed.dirty());
    REQUIRE(speed.add_modifier({low(1), "x", 0.2, AttributeOperation::MultiplyTotal}));
    CHECK(speed.dirty());
}

TEST_CASE("a full attribute refuses and names it", "[gameplay][attributes]") {
    AttributeInstance speed{Attribute::MovementSpeed, 0.1};
    for (u64 i = 0; i < AttributeInstance::kCapacity; ++i) {
        REQUIRE(speed.add_modifier({low(i + 1), "x", 0.01, AttributeOperation::Addition}));
    }
    const auto full = speed.add_modifier({low(99), "x", 0.01, AttributeOperation::Addition});
    REQUIRE(!full);
    CHECK(full.error() == ModifierError::Full);
}
