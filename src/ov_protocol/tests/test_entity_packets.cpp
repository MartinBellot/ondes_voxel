// The entity packets, against bytes a real 1.20.1 server sent.
//
// Everything asserted below was captured by scripts/capture_entity_packets.py:
// a probe client written from the spec joined the vanilla jar, mobs were
// summoned next to it from the console, and the payloads were written down.
// What is frozen here is what the game produced, so a field that moves or a
// varint that grows fails here rather than as a mob that renders as nothing.
#include "ov/protocol/entity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::net;

namespace {

[[nodiscard]] std::string hex(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out;
    out.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0xF]);
    }
    return out;
}

}  // namespace

TEST_CASE("a mob spawns the way the game spawns one", "[protocol][entity]") {
    // Captured: `summon minecraft:zombie 11.5 -60.0 6.5 {…}`, announced as
    // entity 2 of type 118 with no rotation and no velocity.
    SpawnEntity spawn;
    spawn.entity_id = 2;
    spawn.uuid      = Uuid{0xcbef872dc9d642eaULL, 0x98290466dfb92b80ULL};
    spawn.type      = 118;
    spawn.x         = 11.5;
    spawn.y         = -60.0;
    spawn.z         = 6.5;

    REQUIRE(hex(encode_spawn_entity(spawn)) ==
            "02"                                // entity id
            "cbef872dc9d642ea98290466dfb92b80"  // uuid
            "76"                                // type 118, minecraft:zombie
            "4027000000000000"                  // x = 11.5
            "c04e000000000000"                  // y = -60
            "401a000000000000"                  // z = 6.5
            "000000"                            // pitch, yaw, head yaw
            "00"                                // data
            "000000000000");                    // velocity
}

TEST_CASE("the metadata a summoned zombie arrives with", "[protocol][entity]") {
    // Captured verbatim from the same spawn: silent because it was summoned
    // that way, no AI for the same reason, and twenty health.
    MetadataWriter fields;
    fields.boolean_value(metadata::kSilent, true)
        .byte_value(metadata::kMobFlags, metadata::kMobFlagNoAi)
        .float_value(metadata::kHealth, 20.0F);

    REQUIRE(hex(encode_entity_metadata(2, fields.take())) ==
            "02"        // entity id
            "040801"    // index 4, type 8 (boolean), silent
            "0f0001"    // index 15, type 0 (byte), mob flags: no AI
            "090341a00000"  // index 9, type 3 (float), health 20
            "ff");          // end of metadata
}

TEST_CASE("a cow arrives with its own health, and an armour stand with no mob flags",
          "[protocol][entity]") {
    // Two more captures, kept because they are the ones that would catch an
    // index written for the wrong class: an armour stand is not a Mob and sends
    // no index 15 at all.
    MetadataWriter cow;
    cow.boolean_value(metadata::kSilent, true)
        .byte_value(metadata::kMobFlags, metadata::kMobFlagNoAi)
        .float_value(metadata::kHealth, 10.0F);
    REQUIRE(hex(encode_entity_metadata(4, cow.take())) ==
            "04040801"
            "0f0001"
            "090341200000"
            "ff");

    MetadataWriter stand;
    stand.boolean_value(metadata::kSilent, true).float_value(metadata::kHealth, 20.0F);
    REQUIRE(hex(encode_entity_metadata(6, stand.take())) ==
            "06040801"
            "090341a00000"
            "ff");
}

TEST_CASE("the measured metadata indices", "[protocol][entity]") {
    // Each of these was found by setting one NBT field on an otherwise
    // identical zombie and reading which index moved. They are asserted as
    // numbers because a wrong index is not an error — it is a mob that renders
    // with someone else's property.
    CHECK(metadata::kSharedFlags == 0);
    CHECK(metadata::kAir == 1);
    CHECK(metadata::kCustomName == 2);
    CHECK(metadata::kCustomNameVisible == 3);
    CHECK(metadata::kSilent == 4);
    CHECK(metadata::kNoGravity == 5);
    CHECK(metadata::kTicksFrozen == 7);
    CHECK(metadata::kItemStack == 8);
    CHECK(metadata::kHealth == 9);
    CHECK(metadata::kMobFlags == 15);
    CHECK(metadata::kSharedFlagGlowing == 0x40);
    CHECK(metadata::kMobFlagNoAi == 0x01);
    CHECK(metadata::kMobFlagLeftHanded == 0x02);

    // Glowing arrives as the byte 64 at index 0 — captured.
    MetadataWriter glowing;
    glowing.byte_value(metadata::kSharedFlags, metadata::kSharedFlagGlowing);
    REQUIRE(hex(encode_entity_metadata(55, glowing.take())) == "37000040ff");

    // A custom name is an optional component carrying the raw JSON.
    MetadataWriter named;
    named.optional_component_value(metadata::kCustomName, R"({"text":"Bob"})")
        .boolean_value(metadata::kCustomNameVisible, true);
    REQUIRE(hex(encode_entity_metadata(60, named.take())) ==
            "3c"
            "0206"                                        // index 2, optional component
            "01"                                          // present
            "0e" "7b2274657874223a22426f62227d"           // 14 bytes of JSON
            "030801"                                      // index 3, boolean, true
            "ff");
}

TEST_CASE("a movement delta is in 4096ths of a block", "[protocol][entity]") {
    // Measured rather than assumed. A zombie teleported by three quarters of a
    // block produced dX = 3072, and one moved -1.125 produced -4608. Both are
    // exactly the block distance times 4096.
    REQUIRE(hex(encode_entity_position(10, 0.75, 0.0, 0.0, true)) ==
            "0a" "0c00" "0000" "0000" "01");
    REQUIRE(hex(encode_entity_position(10, -1.125, 0.0, 0.0, true)) ==
            "0a" "ee00" "0000" "0000" "01");

    // The bound is what makes this packet safe to use. Eight blocks scales to
    // 32768, one past an i16, and would send the entity eight blocks the other
    // way — so it does not fit and the caller has to teleport instead.
    CHECK(fits_in_delta(7.999, 0.0, 0.0));
    CHECK_FALSE(fits_in_delta(8.0, 0.0, 0.0));
    CHECK_FALSE(fits_in_delta(0.0, 0.0, -8.0));
    CHECK(fits_in_delta(0.0, 0.0, 0.0));
}

TEST_CASE("position and rotation put yaw before pitch", "[protocol][entity]") {
    // Unlike Spawn Entity, which writes pitch first. The two packets genuinely
    // disagree; both orders came from captures.
    REQUIRE(hex(encode_entity_position_rotation(7, 0.0, 0.0, 0.0, 0.0F, -90.0F, false)) ==
            "07" "0000" "0000" "0000" "00" "c0" "00");
}

TEST_CASE("damage arrives unattributed with both source fields zero", "[protocol][entity]") {
    // Captured from `damage @e[…] 3 minecraft:generic`: entity 10, damage type
    // 16, no cause and no direct source, no position.
    REQUIRE(hex(encode_damage_event(10, 16, std::nullopt, std::nullopt)) == "0a10000000");

    // And the health that followed it: twenty less three.
    MetadataWriter health;
    health.float_value(metadata::kHealth, 17.0F);
    REQUIRE(hex(encode_entity_metadata(10, health.take())) == "0a090341880000ff");

    // A named cause travels as id + 1, because zero already means absent.
    REQUIRE(hex(encode_damage_event(10, 16, 5, 5)) == "0a100606" "00");
}

TEST_CASE("update attributes carries the base value the game reports",
          "[protocol][entity]") {
    // Captured: a zombie arrives with minecraft:generic.movement_speed and the
    // f64 0x3fcd70a3e0000000, which is 0.23000000417232513 — exactly what
    // `attribute … base get` prints for the type, and exactly the double
    // nearest the float 0.23f. This is the one place the two independent
    // measurements meet, and they agree bit for bit.
    const AttributeValue attributes[] = {
        {"minecraft:generic.movement_speed", static_cast<f64>(0.23F)}};
    REQUIRE(hex(encode_update_attributes(2, attributes)) ==
            "02"
            "01"  // one attribute
            "20" "6d696e6563726166743a67656e657269632e6d6f76656d656e745f7370656564"
            "3fcd70a3e0000000"
            "00");  // no modifiers
}

TEST_CASE("entity event writes a fixed int, not a varint", "[protocol][entity]") {
    // The only entity packet in the protocol that does. A varint here would be
    // one byte for a small id and the client would read the status out of the
    // wrong place.
    REQUIRE(hex(encode_entity_event(2, 3)) == "0000000203");
}

TEST_CASE("removals are batched", "[protocol][entity]") {
    const i32 ids[] = {2, 4, 300};
    REQUIRE(hex(encode_remove_entities(ids)) == "03" "02" "04" "ac02");
    REQUIRE(hex(encode_remove_entities({})) == "00");
}

TEST_CASE("velocity saturates rather than wrapping", "[protocol][entity]") {
    // A velocity past about four blocks a tick cannot be said in an i16 of
    // 1/8000ths. Wrapping would send the entity the other way, which looks like
    // broken physics rather than a broken encoder.
    REQUIRE(hex(encode_entity_velocity(6, 0.0, -0.0783875, 0.0)) == "060000fd8d0000");
    REQUIRE(hex(encode_entity_velocity(1, 100.0, -100.0, 0.0)) == "017fff80000000");
}
