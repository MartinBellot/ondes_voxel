// Sound packets, frozen against bytes the real 1.20.1 server sent to a probe
// (scripts/capture_sound_packets.py, docs/provenance/son.md). Each vanilla
// payload is parsed, checked field by field against what the gesture chose,
// and re-encoded to the same bytes.
#include "ov/protocol/chat.hpp"
#include "ov/protocol/sound.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace ov;
using namespace ov::net;

namespace {

std::vector<u8> hex(std::string_view text) {
    std::vector<u8> out;
    for (usize i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<u8>(std::stoi(std::string{text.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

}  // namespace

TEST_CASE("Sound Effect: a stone placed, as vanilla sent it to another player", "[sound]") {
    // place.stone: block.stone.place (1269) at (1.5, -59.5, 2.5), block, 1.0, 0.8.
    const auto bytes  = hex("f609040000000cfffffe24000000143f8000003f4ccccd3f94f78943554861");
    const auto parsed = parse_sound_effect(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sound.sound_id == 1269);
    CHECK(parsed->category == sound_category::kBlock);
    CHECK(parsed->x == 12);
    CHECK(parsed->y == -476);
    CHECK(parsed->z == 20);
    CHECK(parsed->position().y == -59.5);
    CHECK(parsed->volume == 1.0F);
    CHECK(parsed->pitch == 0.8F);
    CHECK(encode_sound_effect(*parsed) == bytes);
}

TEST_CASE("Sound Effect: an unregistered name travels inline", "[sound]") {
    // /playsound minecraft:ondes.custom hostile Ear 10.5 -59.25 20.125 2.0 0.5
    const auto bytes = hex("00166d696e6563726166743a6f6e6465732e637573746f6d000500000054fffffe26"
                           "000000a1400000003f0000006d9a45527ff12cbb");
    const auto parsed = parse_sound_effect(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->sound.sound_id == -1);
    CHECK(parsed->sound.name == "minecraft:ondes.custom");
    CHECK_FALSE(parsed->sound.fixed_range.has_value());
    CHECK(parsed->category == sound_category::kHostile);
    CHECK(parsed->x == sound_coordinate(10.5));
    CHECK(parsed->y == sound_coordinate(-59.25));
    CHECK(parsed->z == sound_coordinate(20.125));
    CHECK(parsed->volume == 2.0F);
    CHECK(parsed->pitch == 0.5F);
    CHECK(encode_sound_effect(*parsed) == bytes);
}

TEST_CASE("sound coordinates truncate toward zero, as the walk capture shows", "[sound]") {
    // Footsteps heard at -0.75 and 0.875 from walkers at -0.7736 and 0.9528:
    // floor would give -0.875, rounding 1.0.
    CHECK(sound_coordinate(-2.5 + 0.2158 * 8) == -6);
    CHECK(sound_coordinate(-2.5 + 0.2158 * 16) == 7);
    CHECK(sound_coordinate(1.5) == 12);
    CHECK(sound_coordinate(-59.5) == -476);
}

TEST_CASE("Stop Sound: the four forms /stopsound sends", "[sound]") {
    const auto all = parse_stop_sound(hex("00"));
    REQUIRE(all.has_value());
    CHECK_FALSE(all->category.has_value());
    CHECK(all->sound.empty());

    const auto source = parse_stop_sound(hex("0104"));
    REQUIRE(source.has_value());
    CHECK(source->category == sound_category::kBlock);

    const auto sound_bytes = hex("021b6d696e6563726166743a626c6f636b2e73746f6e652e706c616365");
    const auto sound       = parse_stop_sound(sound_bytes);
    REQUIRE(sound.has_value());
    CHECK_FALSE(sound->category.has_value());
    CHECK(sound->sound == "minecraft:block.stone.place");

    const auto both_bytes = hex("03031b6d696e6563726166743a626c6f636b2e73746f6e652e706c616365");
    const auto both       = parse_stop_sound(both_bytes);
    REQUIRE(both.has_value());
    CHECK(both->category == sound_category::kWeather);
    CHECK(both->sound == "minecraft:block.stone.place");

    CHECK(encode_stop_sound(*all) == hex("00"));
    CHECK(encode_stop_sound(*source) == hex("0104"));
    CHECK(encode_stop_sound(*sound) == sound_bytes);
    CHECK(encode_stop_sound(*both) == both_bytes);
    CHECK_FALSE(parse_stop_sound(hex("04")).has_value());
}

TEST_CASE("World Event: the block break another player is told about", "[sound]") {
    // break.stone.creative: 2001 at (1, -60, 2), state 1 (stone), not global.
    const auto bytes  = hex("000007d10000004000002fc40000000100");
    const auto parsed = parse_world_event(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->event == kWorldEventBlockBreak);
    CHECK(parsed->x == 1);
    CHECK(parsed->y == -60);
    CHECK(parsed->z == 2);
    CHECK(parsed->data == 1);
    CHECK_FALSE(parsed->global);
    CHECK(encode_world_event(2001, WirePosition{1, -60, 2}, 1, false) == bytes);
}

TEST_CASE("Entity Sound Effect round-trips (archived layout, not captured)", "[sound]") {
    EntitySoundEffect sound;
    sound.sound.sound_id = 313;
    sound.category       = sound_category::kNeutral;
    sound.entity_id      = 42;
    sound.volume         = 0.4F;
    sound.pitch          = 1.1F;
    sound.seed           = -7;
    const auto parsed    = parse_entity_sound_effect(encode_entity_sound_effect(sound));
    REQUIRE(parsed.has_value());
    CHECK(parsed->sound.sound_id == 313);
    CHECK(parsed->entity_id == 42);
    CHECK(parsed->seed == -7);
}
