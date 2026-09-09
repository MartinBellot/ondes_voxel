// The survival packets, against bytes a real 1.20.1 server actually sent.
//
// Every expectation below is a capture, not a reading of a specification. The
// hex strings are what arrived on a socket when a probe client joined the
// vanilla jar and one thing at a time was changed from the console; the test
// asks whether our encoder produces the same bytes for the same state.
//
// Two of them are here because a specification would have got them wrong:
// Combat Death carries no killer entity id, and there is no Hurt Animation
// packet at all. See survival.hpp.
#include "ov/protocol/survival.hpp"

#include "ov/protocol/entity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::net;

namespace {

[[nodiscard]] std::string hex(std::span<const u8> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        char buffer[3];
        std::snprintf(buffer, sizeof(buffer), "%02x", byte);
        out += buffer;
    }
    return out;
}

}  // namespace

TEST_CASE("Set Health is what the vanilla server sent", "[survival][parity]") {
    // Captured after taking 3 points of cactus damage from full, with the bar
    // at twenty and saturation at five: 41880000 is 17.0f, 14 is the varint 20,
    // 40a00000 is 5.0f.
    REQUIRE(hex(encode_set_health(17.0F, 20, 5.0F)) == "418800001440a00000");

    // And at the moment of death, which is the one that puts up the screen.
    REQUIRE(hex(encode_set_health(0.0F, 20, 20.0F)) == "000000001441a00000");
}

TEST_CASE("Set Experience is a bar, a level and a total", "[survival][parity]") {
    // Captured immediately after a respawn: everything zero.
    REQUIRE(hex(encode_set_experience(0.0F, 0, 0)) == "000000000000");

    // The bar is a fraction of the *level*, not of the total. Level 7 costs 21
    // points, so one point in is 1/21.
    const auto payload = encode_set_experience(1.0F / 21.0F, 7, 197);
    REQUIRE(payload.size() == 4 + 1 + 2);
    REQUIRE(payload[4] == 7);
}

TEST_CASE("Damage Event names the type, and its encoder is entity.hpp's",
          "[survival][parity]") {
    // Captured twice, from a real server, for the same player (entity 23):
    // cactus and lightning. The only difference between the two windows was
    // this packet's second field, which is how the packet was identified at
    // all — Damage Event and Hurt Animation are the same shape, so nothing
    // about the *shape* could have told them apart.
    //
    // The encoder lives in entity.hpp and is not duplicated here; what this
    // asserts is that the damage type ids the survival system uses are the ones
    // that arrived, and that they are in the alphabetical order a datapack
    // registry is loaded in: cactus is the third name of forty-four, lightning
    // the twenty-fourth, generic_kill the eighteenth.
    REQUIRE(hex(encode_damage_event(23, 2, std::nullopt, std::nullopt)) == "1702000000");
    REQUIRE(hex(encode_damage_event(23, 23, std::nullopt, std::nullopt)) == "1717000000");
    REQUIRE(hex(encode_damage_event(23, 17, std::nullopt, std::nullopt)) == "1711000000");
}

TEST_CASE("a 1.20.1 server sends no hurt animation with a damage event",
          "[survival][parity]") {
    // Recorded rather than assumed. Two windows were captured around two hits
    // of different damage types; the packets in them were Damage Event, Set
    // Entity Metadata (health), Set Entity Velocity (knockback) and Set Health.
    // No Hurt Animation, either time. The encoder for one exists in entity.hpp
    // because the packet exists in the protocol — but the survival system must
    // not send it, or the client flinches twice.
    REQUIRE(clientbound::kHurtAnimation != clientbound::kDamageEvent);
}

TEST_CASE("Combat Death has no killer entity id", "[survival][parity]") {
    // The measurement that made this test exist. A real server's Combat Death
    // for a player killed by /kill was 315 bytes: one varint of player id, then
    // a 312-byte chat component with a two-byte length. There is no room for a
    // four-byte entity id, and decoding it as though there were leaves the
    // string 116 bytes long and the packet 193 bytes over.
    const std::string message =
        R"({"translate":"death.attack.genericKill","with":[{"text":"ovsurvive"}]})";
    const auto payload = encode_combat_death(23, message);
    REQUIRE(payload[0] == 23);
    // Length of the component follows immediately: a two-byte varint for 70.
    REQUIRE(payload[1] == message.size());
    REQUIRE(payload.size() == 1 + 1 + message.size());
}

TEST_CASE("a death message names its key and its victim", "[survival]") {
    REQUIRE(death_message_json("death.attack.fall", "Steve") ==
            R"({"translate":"death.attack.fall","with":[{"text":"Steve"}]})");

    // With a killer, the two-argument form of the same sentence.
    REQUIRE(death_message_json("death.attack.player", "Steve", "Alex") ==
            R"({"translate":"death.attack.player","with":[{"text":"Steve"},{"text":"Alex"}]})");

    // A name with a quote in it cannot break the component. Players cannot have
    // one, but a renamed mob can, and a malformed component disconnects the
    // client at the exact moment it was about to draw a death screen.
    REQUIRE(death_message_json("death.attack.mob", R"(Bo"b)") ==
            R"({"translate":"death.attack.mob","with":[{"text":"Bo\"b"}]})");
}

TEST_CASE("Respawn carries every field the real one did", "[survival][parity]") {
    // Decoded from a capture, field by field: two dimension strings, the hashed
    // seed, game mode and previous game mode, three flags, the kept-data byte,
    // an optional death location, and the portal cooldown. 83 bytes.
    Respawn respawn;
    respawn.hashed_seed     = 5382741407664558541LL;
    respawn.game_mode       = 0;
    respawn.is_flat         = true;
    respawn.data_kept       = 0;
    respawn.death_dimension = std::string_view{"minecraft:overworld"};
    respawn.death_position  = WirePosition{0, -60, 4};

    const auto payload = encode_respawn(respawn);
    // 1 + 19 twice, 8 seed, 1 + 1 modes, 3 flags, 1 kept, 1 has-location,
    // 1 + 19 dimension, 8 position, 1 cooldown.
    REQUIRE(payload.size() == 83);
    REQUIRE(payload[0] == 19);

    // Without a death location it is twenty-eight bytes shorter, and the
    // has-location byte is the last flag before the cooldown.
    Respawn plain = respawn;
    plain.death_dimension.reset();
    REQUIRE(encode_respawn(plain).size() == 83 - 28);
}

TEST_CASE("an experience orb carries a value, not a type", "[survival][parity]") {
    // Captured from a real death: entity 24 at the player's feet, holding 37 —
    // the largest denomination that fits in the 48 points that death dropped.
    const auto payload = encode_spawn_experience_orb(24, 4.5, -60.0, -8.5, 37);
    REQUIRE(payload.size() == 1 + 24 + 2);
    REQUIRE(payload[0] == 24);
    REQUIRE(payload[payload.size() - 2] == 0);
    REQUIRE(payload[payload.size() - 1] == 37);
}

TEST_CASE("Client Command is parsed, and an unknown action is refused",
          "[survival]") {
    const std::vector<u8> respawn{0x00};
    const auto            parsed = parse_client_command(respawn);
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == ClientCommand::PerformRespawn);

    const std::vector<u8> stats{0x01};
    REQUIRE(parse_client_command(stats) == ClientCommand::RequestStats);

    // A third action is named as unknown rather than treated as a respawn,
    // which would put a live player back at spawn for pressing a button we
    // misread.
    const std::vector<u8> unknown{0x07};
    REQUIRE_FALSE(parse_client_command(unknown).has_value());
    REQUIRE_FALSE(parse_client_command({}).has_value());
}

TEST_CASE("the ids are the ones that were measured, not the ones published",
          "[survival][parity]") {
    // Derived on a live server by changing one thing at a time. Kept as a test
    // so that a well-meaning correction against a summary fails here.
    REQUIRE(clientbound::kDamageEvent == 0x18);
    REQUIRE(clientbound::kSpawnExperienceOrb == 0x02);
    REQUIRE(clientbound::kCombatDeath == 0x38);
    REQUIRE(clientbound::kRespawn == 0x41);
    REQUIRE(clientbound::kSetExperience == 0x56);
    REQUIRE(clientbound::kSetHealth == 0x57);
    REQUIRE(serverbound::kClientCommand == 0x07);
}
