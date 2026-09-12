// The reader against the writer: every metadata type the server can send comes
// back with the value it went in with, and nothing after it moves.
//
// The failure this guards against is not a wrong value, it is a *shifted* one:
// the metadata list has no length per field, so a value read one byte too
// short makes every later index decode from the middle of the one before. A
// test that only checked the first field would pass on exactly that bug, so
// every case below puts a sentinel field after the one under test.
#include "ov/io/byte_reader.hpp"
#include "ov/protocol/entity.hpp"
#include "ov/protocol/entity_metadata.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::net;

namespace {

/// The writer's body, walked back by the reader, with a sentinel at index 30.
[[nodiscard]] ParsedMetadata round_trip(MetadataWriter& fields) {
    fields.varint_value(30, 424242);
    const std::vector<u8> body = fields.take();
    io::ByteReader        reader(body);
    ParsedMetadata        parsed = parse_entity_metadata(reader);
    REQUIRE(parsed.complete);
    REQUIRE(reader.exhausted());
    REQUIRE_FALSE(parsed.values.empty());
    CHECK(parsed.values.back().index == 30);
    CHECK(parsed.values.back().integer == 424242);
    return parsed;
}

}  // namespace

TEST_CASE("every value type round-trips and leaves the next field in place",
          "[protocol][entity][metadata]") {
    MetadataWriter fields;
    fields.byte_value(0, static_cast<i8>(0x21))
        .boolean_value(3, true)
        .float_value(9, 17.5F)
        .varint_value(16, -3)
        .string_value(4, "hello")
        .optional_component_value(2, std::string_view{R"({"text":"Dinnerbone"})"})
        .optional_component_value(5, std::nullopt)
        .rotations_value(16, 1.0F, -2.0F, 3.5F)
        .villager_data_value(18, 2, 11, 5)
        .optional_unsigned_int_value(19, 7)
        .optional_unsigned_int_value(20, std::nullopt)
        .block_state_value(11, 1234)
        .pose_value(6, 2);
    const ParsedMetadata parsed = round_trip(fields);
    REQUIRE(parsed.values.size() == 14);

    CHECK(parsed.values[0].integer == 0x21);
    CHECK(parsed.values[1].integer == 1);
    CHECK(parsed.values[2].real == Catch::Approx(17.5F));
    CHECK(parsed.values[3].integer == -3);
    CHECK(parsed.values[4].text == "hello");
    CHECK(parsed.values[5].present);
    CHECK(parsed.values[5].text == R"({"text":"Dinnerbone"})");
    CHECK_FALSE(parsed.values[6].present);
    CHECK(parsed.values[7].vector[1] == Catch::Approx(-2.0F));
    CHECK(parsed.values[8].villager == std::array<i32, 3>{2, 11, 5});
    // The wire's "value + 1" is undone: a real zero and an absence differ.
    CHECK(parsed.values[9].present);
    CHECK(parsed.values[9].integer == 7);
    CHECK_FALSE(parsed.values[10].present);
    CHECK(parsed.values[11].integer == 1234);
    CHECK(parsed.values[12].type == MetadataType::Pose);
    CHECK(parsed.values[12].integer == 2);
}

TEST_CASE("an item stack in metadata keeps its item and count", "[protocol][entity][metadata]") {
    MetadataWriter fields;
    ItemStack      stack;
    stack.item_id = 812;
    stack.count   = 3;
    fields.item_value(metadata::kItemStack, stack);
    const ParsedMetadata parsed = round_trip(fields);
    REQUIRE(parsed.values[0].stack.has_value());
    CHECK(parsed.values[0].stack->item_id == 812);
    CHECK(parsed.values[0].stack->count == 3);
}

TEST_CASE("a compound tag stops the walk and keeps what came before",
          "[protocol][entity][metadata]") {
    // Index 9, type 16 (CompoundTag), then bytes this reader cannot size.
    const std::vector<u8> body{0x08, 0x01, 0x05, 0x09, 0x10, 0x0A, 0x00, 0x00, 0xFF};
    io::ByteReader        reader(body);
    const ParsedMetadata  parsed = parse_entity_metadata(reader);
    CHECK_FALSE(parsed.complete);
    REQUIRE(parsed.values.size() == 1);
    CHECK(parsed.values[0].index == 8);
    CHECK(parsed.values[0].integer == 5);
}

TEST_CASE("equipment carries a continuation bit on every entry but the last",
          "[protocol][entity][metadata]") {
    ItemStack sword;
    sword.item_id = 829;
    sword.count   = 1;
    ItemStack helmet;
    helmet.item_id = 790;
    helmet.count   = 1;
    const std::vector<EquipmentEntry> entries{{EquipmentSlot::MainHand, sword},
                                              {EquipmentSlot::Head, helmet}};
    const std::vector<u8> payload = encode_entity_equipment(41, entries);
    // Entity 41, then slot 0 with the top bit set.
    REQUIRE(payload.size() > 2);
    CHECK(payload[0] == 41);
    CHECK(payload[1] == 0x80);

    const auto parsed = parse_entity_equipment(payload);
    REQUIRE(parsed.has_value());
    CHECK(parsed->entity_id == 41);
    REQUIRE(parsed->entries.size() == 2);
    CHECK(parsed->entries[0].slot == EquipmentSlot::MainHand);
    CHECK(parsed->entries[0].stack.item_id == 829);
    CHECK(parsed->entries[1].slot == EquipmentSlot::Head);
    CHECK(parsed->entries[1].stack.item_id == 790);
}

TEST_CASE("passengers, entity events and hurt animations read what the server writes",
          "[protocol][entity][metadata]") {
    const auto passengers = parse_set_passengers(encode_set_passengers(Passengers{7, {3, 12}}));
    REQUIRE(passengers.has_value());
    CHECK(passengers->vehicle_id == 7);
    CHECK(passengers->riders == std::vector<i32>{3, 12});

    // A dismount is the same packet with nobody in it.
    const auto empty = parse_set_passengers(encode_set_passengers(Passengers{7, {}}));
    REQUIRE(empty.has_value());
    CHECK(empty->riders.empty());

    const auto event = parse_entity_event(encode_entity_event(300, entity_status::kDeath));
    REQUIRE(event.has_value());
    CHECK(event->entity_id == 300);
    CHECK(event->status == entity_status::kDeath);

    const auto hurt = parse_hurt_animation(encode_hurt_animation(5, 90.0F));
    REQUIRE(hurt.has_value());
    CHECK(hurt->entity_id == 5);
    CHECK(hurt->yaw == Catch::Approx(90.0F));

    const auto victim = parse_damage_event_victim(encode_damage_event(9, 2, std::nullopt, 4));
    REQUIRE(victim.has_value());
    CHECK(*victim == 9);
}
