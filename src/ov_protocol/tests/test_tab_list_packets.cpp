// The tab list's packets and Open Horse Screen.
//
// The expected bytes are written out by hand from the field tables of the
// frozen protocol page (oldid=2773082), not produced by our own encoder: a
// round trip alone passes when both halves agree on the same mistake.

#include "ov/protocol/tab_list.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::net;

namespace {

using Bytes = std::vector<u8>;

Bytes with_trailing_byte(Bytes bytes) {
    bytes.push_back(0x00);
    return bytes;
}

Bytes without_last_byte(Bytes bytes) {
    bytes.pop_back();
    return bytes;
}

void append(Bytes& to, std::initializer_list<u8> bytes) {
    to.insert(to.end(), bytes);
}

/// 00..0F then 10..1F: the two halves of the UUID the tests use.
void append_uuid(Bytes& to) {
    for (u8 i = 0; i < 16; ++i) {
        to.push_back(i);
    }
}

const Uuid kUuid{0x0001020304050607ULL, 0x08090A0B0C0D0E0FULL};

}  // namespace

TEST_CASE("Player Info Update: what the vanilla server sends on join", "[protocol][hud]") {
    // Actions 0x3D (add, game mode, listed, latency, display name — the mask a
    // vanilla server sends a joining offline player, no chat session), one
    // entry: "Al", no properties, survival, listed, 7 ms, no display name.
    Bytes bytes;
    append(bytes, {0x3D, 0x01});
    append_uuid(bytes);
    append(bytes, {0x02, 'A', 'l', 0x00});  // name, zero properties
    append(bytes, {0x00});                  // game mode 0
    append(bytes, {0x01});                  // listed
    append(bytes, {0x07});                  // latency 7
    append(bytes, {0x00});                  // no display name

    const auto parsed = parse_player_info_update(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->actions == 0x3D);
    REQUIRE(parsed->entries.size() == 1);
    const PlayerInfoEntry& entry = parsed->entries[0];
    CHECK(entry.uuid == kUuid);
    CHECK(entry.name == "Al");
    CHECK(entry.properties.empty());
    CHECK_FALSE(entry.chat.has_value());
    CHECK(entry.game_mode == 0);
    CHECK(entry.listed);
    CHECK(entry.latency == 7);
    CHECK_FALSE(entry.display_name_json.has_value());
    CHECK(encode_player_info_update(*parsed) == bytes);

    CHECK_FALSE(parse_player_info_update(with_trailing_byte(bytes)).has_value());
    CHECK_FALSE(parse_player_info_update(without_last_byte(bytes)).has_value());
}

TEST_CASE("Player Info Update: the fields of one action only", "[protocol][hud]") {
    // Update Latency alone: UUID, then a VarInt — 300 is AC 02.
    Bytes bytes;
    append(bytes, {0x10, 0x01});
    append_uuid(bytes);
    append(bytes, {0xAC, 0x02});
    const auto parsed = parse_player_info_update(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->entries.at(0).latency == 300);
    CHECK(parsed->entries.at(0).name.empty());

    // Update Game Mode alone, to spectator.
    Bytes mode;
    append(mode, {0x04, 0x01});
    append_uuid(mode);
    append(mode, {0x03});
    REQUIRE(parse_player_info_update(mode).has_value());
    CHECK(parse_player_info_update(mode)->entries.at(0).game_mode == 3);
}

TEST_CASE("Player Info Update: a signed property and a chat session", "[protocol][hud]") {
    Bytes bytes;
    append(bytes, {0x03, 0x01});  // add player | initialize chat
    append_uuid(bytes);
    append(bytes, {0x01, 'x', 0x01});                      // name "x", one property
    append(bytes, {0x01, 't', 0x01, 'v', 0x01, 0x01, 's'});  // "t" = "v", signed "s"
    append(bytes, {0x01});                                 // has signature data
    append_uuid(bytes);                                    // session id
    append(bytes, {0, 0, 0, 0, 0, 0, 0, 0x2A});            // expires 42
    append(bytes, {0x02, 0xAA, 0xBB});                     // key
    append(bytes, {0x01, 0xCC});                           // key signature
    const auto parsed = parse_player_info_update(bytes);
    REQUIRE(parsed.has_value());
    const PlayerInfoEntry& entry = parsed->entries.at(0);
    REQUIRE(entry.properties.size() == 1);
    CHECK(entry.properties[0].signature == std::optional<std::string>{"s"});
    REQUIRE(entry.chat.has_value());
    CHECK(entry.chat->expires_ms == 42);
    CHECK(entry.chat->public_key == Bytes{0xAA, 0xBB});
    CHECK(entry.chat->key_signature == Bytes{0xCC});
    CHECK(encode_player_info_update(*parsed) == bytes);
}

TEST_CASE("Player Info Update refuses what cannot be sized", "[protocol][hud]") {
    // A mask with a bit no action owns.
    Bytes unknown{0x40, 0x00};
    CHECK_FALSE(parse_player_info_update(unknown).has_value());
    // A count of players the bytes left cannot hold.
    Bytes lying{0x10, 0x7F};
    CHECK_FALSE(parse_player_info_update(lying).has_value());
    // A name longer than sixteen.
    Bytes long_name;
    append(long_name, {0x01, 0x01});
    append_uuid(long_name);
    long_name.push_back(17);
    for (int i = 0; i < 17; ++i) {
        long_name.push_back('a');
    }
    long_name.push_back(0x00);
    CHECK_FALSE(parse_player_info_update(long_name).has_value());
}

TEST_CASE("Player Info Remove is a count and the UUIDs", "[protocol][hud]") {
    Bytes bytes{0x01};
    append_uuid(bytes);
    const auto parsed = parse_player_info_remove(bytes);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == std::vector<Uuid>{kUuid});
    CHECK(encode_player_info_remove_many(*parsed) == bytes);
    CHECK_FALSE(parse_player_info_remove(without_last_byte(bytes)).has_value());
    CHECK_FALSE(parse_player_info_remove(with_trailing_byte(bytes)).has_value());
}

TEST_CASE("Set Tab List Header And Footer is two strings", "[protocol][hud]") {
    const std::string header = R"({"text":"H"})";
    Bytes             bytes;
    bytes.push_back(static_cast<u8>(header.size()));
    bytes.insert(bytes.end(), header.begin(), header.end());
    append(bytes, {0x02, '"', '"'});
    const auto parsed = parse_tab_list_header_footer(bytes);
    REQUIRE(parsed.has_value());
    CHECK(parsed->header_json == header);
    CHECK(parsed->footer_json == "\"\"");
    CHECK(encode_tab_list_header_footer(*parsed) == bytes);
    CHECK_FALSE(parse_tab_list_header_footer(without_last_byte(bytes)).has_value());
}

TEST_CASE("Open Horse Screen: a byte, a VarInt, then an Int", "[protocol][hud]") {
    // Window 3, 17 slots (a donkey with a chest), entity 0x01020304.
    const Bytes bytes{0x03, 0x11, 0x01, 0x02, 0x03, 0x04};
    const auto  parsed = parse_open_horse_screen(bytes);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == OpenHorseScreen{3, 17, 0x01020304});
    CHECK(encode_open_horse_screen(*parsed) == bytes);
    CHECK_FALSE(parse_open_horse_screen(without_last_byte(bytes)).has_value());
    CHECK_FALSE(parse_open_horse_screen(with_trailing_byte(bytes)).has_value());
}
