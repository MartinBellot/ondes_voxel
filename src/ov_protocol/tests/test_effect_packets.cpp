// The effect packets, against bytes a real 1.20.1 server sent.
//
// Every payload below was captured by scripts/measure_effects.py (campaigns
// `packets` and `player_meta`): a probe joined the vanilla jar, effects were
// given to it from the console, and the packets that arrived starting with its
// entity id and the effect id just named were written down. What is frozen
// here is what the game produced.
#include "ov/protocol/effect_packets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::net;

namespace {

[[nodiscard]] std::string hex(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out;
    for (const u8 byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0xF]);
    }
    return out;
}

[[nodiscard]] std::vector<u8> bytes(std::string_view text) {
    std::vector<u8> out;
    for (usize i = 0; i + 1 < text.size(); i += 2) {
        const auto nibble = [](char c) -> u8 {
            return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        out.push_back(static_cast<u8>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    }
    return out;
}

// `effect give <bot> minecraft:darkness 10 0`, entity 35.
constexpr std::string_view kDarkness =
    "232100c80106010a0000"
    "050015666163746f725f70726576696f75735f6672616d6500000000"
    "05000c666163746f725f737461727400000000"
    "03000c7469636b735f61637469766500000000"
    "05000d666163746f725f7461726765743f800000"
    "05000e666163746f725f63757272656e7400000000"
    "0100146861645f6566666563745f6c6173745f7469636b00"
    "03001070616464696e675f6475726174696f6e00000016"
    "00";

}  // namespace

TEST_CASE("entity effect: speed II for thirty seconds", "[protocol][effects]") {
    // `effect give <bot> minecraft:speed 30 1`: id 1 (1-based), amplifier 1,
    // 600 ticks, particles and icon, no factor data.
    EntityEffect packet{.entity_id = 35, .effect_id = 1, .amplifier = 1, .duration = 600,
                        .flags = effect_flags::kVisible | effect_flags::kShowIcon};
    REQUIRE(hex(encode_entity_effect(packet)) == "230101d8040600");
    const auto decoded = decode_entity_effect(bytes("230101d8040600"));
    REQUIRE(decoded);
    CHECK(*decoded == packet);
}

TEST_CASE("entity effect: infinite, with particles hidden, hides the icon too",
          "[protocol][effects]") {
    // `effect give <bot> minecraft:haste infinite 0 true`: the duration is the
    // five-byte varint of -1, and the flags byte is 0 — hiding the particles
    // from the command hid the icon as well.
    EntityEffect packet{.entity_id = 35, .effect_id = 3, .amplifier = 0, .duration = -1,
                        .flags = 0};
    REQUIRE(hex(encode_entity_effect(packet)) == "230300ffffffff0f0000");
    const auto decoded = decode_entity_effect(bytes("230300ffffffff0f0000"));
    REQUIRE(decoded);
    CHECK(decoded->duration == -1);
    CHECK(decoded->flags == 0);
}

TEST_CASE("entity effect: darkness carries its factor data as named network NBT",
          "[protocol][effects]") {
    EntityEffect packet{.entity_id = 35, .effect_id = 33, .amplifier = 0, .duration = 200,
                        .flags  = effect_flags::kVisible | effect_flags::kShowIcon,
                        .factor = EffectFactorData{}};
    REQUIRE(hex(encode_entity_effect(packet)) == kDarkness);
    const auto decoded = decode_entity_effect(bytes(kDarkness));
    REQUIRE(decoded);
    REQUIRE(decoded->factor);
    CHECK(decoded->factor->padding_duration == 22);
    CHECK(decoded->factor->factor_target == 1.0F);
    CHECK(*decoded == packet);
}

TEST_CASE("remove entity effect is two varints", "[protocol][effects]") {
    REQUIRE(hex(encode_remove_entity_effect(35, 1)) == "2301");
    REQUIRE(hex(encode_remove_entity_effect(35, 33)) == "2321");
    const auto decoded = decode_remove_entity_effect(bytes("2305"));
    REQUIRE(decoded);
    CHECK(decoded->entity_id == 35);
    CHECK(decoded->effect_id == 5);
}

TEST_CASE("update attributes carries the base and the modifier, not the total",
          "[protocol][effects]") {
    // Speed II on the bot: movement speed, base 0.1 as the rig had set it,
    // one modifier 91aeaa56-… of 0.4000000059604645 (the float 0.2 times
    // two), operation 2. The client multiplies it out itself.
    const WireModifier speed[] = {
        {Uuid{0x91AEAA56376B4498ULL, 0x935B2F7F68070635ULL}, 0.4000000059604645, 2}};
    const AttributeProperty properties[] = {{"minecraft:generic.movement_speed", 0.1, speed}};
    const std::string_view expected =
        "2301206d696e6563726166743a67656e657269632e6d6f76656d656e745f7370656564"
        "3fb999999999999a0191aeaa56376b4498935b2f7f680706353fd99999a000000002";
    REQUIRE(hex(encode_update_attributes_full(35, properties)) == expected);

    const auto decoded = decode_update_attributes(bytes(expected));
    REQUIRE(decoded);
    REQUIRE(decoded->properties.size() == 1);
    CHECK(decoded->properties[0].name == "minecraft:generic.movement_speed");
    REQUIRE(decoded->properties[0].modifiers.size() == 1);
    CHECK(decoded->properties[0].modifiers[0] == speed[0]);
}

TEST_CASE("update attributes: health boost, then its removal", "[protocol][effects]") {
    // Captured in `player_meta`: entity 24, max health base 20 with the
    // 5d6f0ba2-… modifier of 8.0 (health boost II), operation 0; then the same
    // attribute with no modifier once the effect was cleared.
    const WireModifier boost[] = {
        {Uuid{0x5D6F0BA2118646ACULL, 0xB896C61C5CEE99CCULL}, 8.0, 0}};
    const AttributeProperty on[] = {{"minecraft:generic.max_health", 20.0, boost}};
    CHECK(hex(encode_update_attributes_full(24, on)) ==
          "18011c6d696e6563726166743a67656e657269632e6d61785f6865616c7468"
          "4034000000000000015d6f0ba2118646acb896c61c5cee99cc402000000000000000");
    const AttributeProperty off[] = {{"minecraft:generic.max_health", 20.0, {}}};
    CHECK(hex(encode_update_attributes_full(24, off)) ==
          "18011c6d696e6563726166743a67656e657269632e6d61785f6865616c7468"
          "403400000000000000");
}

TEST_CASE("the decoders refuse what they cannot read", "[protocol][effects]") {
    CHECK(!decode_entity_effect(bytes("2301")));
    CHECK(!decode_entity_effect(bytes("230101d804060000")));  // trailing byte
    CHECK(!decode_remove_entity_effect(bytes("230101")));
    CHECK(!decode_update_attributes(bytes("2301")));
}
