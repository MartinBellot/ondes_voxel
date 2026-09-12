// The four scoreboard packets, against the bytes a real 1.20.1 server sent.
//
// Every hexadecimal string below is **a capture**: a payload the jar sent a
// probe client during scripts/capture_scoreboard.py, after the command named
// beside it. The encoder must produce it byte for byte, and the parser must
// read it back to the same fields.
#include "ov/protocol/scoreboard_packets.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace ov;
using namespace ov::net::scoreboard;

namespace {

std::vector<u8> hex(std::string_view text) {
    std::vector<u8> out;
    for (usize i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<u8>(std::stoi(std::string{text.substr(i, 2)}, nullptr, 16)));
    }
    return out;
}

const std::string kEmpty = R"({"text":""})";

}  // namespace

TEST_CASE("Update Score is the jar's, byte for byte", "[protocol][scoreboard]") {
    // players set @s d 5 — the second of its two packets
    const ScoreUpdate five{"ovprobe", ScoreAction::Change, "d", 5};
    CHECK(encode_update_score(five) == hex("076f7670726f626500016405"));
    CHECK(parse_update_score(hex("076f7670726f626500016405")) == five);
    // players set a d -7: a negative VarInt is five bytes
    CHECK(encode_update_score({"a", ScoreAction::Change, "d", -7}) == hex("0161000164f9ffffff0f"));
    // players add a d 1 on 2147483647: the wrap
    CHECK(encode_update_score({"a", ScoreAction::Change, "d", std::numeric_limits<i32>::min()}) ==
          hex("01610001648080808008"));
    // players reset fake d, fake's only score: a removal with no objective
    const ScoreUpdate gone{"fake", ScoreAction::Remove, "", 0};
    CHECK(encode_update_score(gone) == hex("0466616b650100"));
    CHECK(parse_update_score(hex("0466616b650100")) == gone);
}

TEST_CASE("Display Objective is the jar's", "[protocol][scoreboard]") {
    CHECK(encode_display_objective({kSlotSidebar, "d"}) == hex("010164"));
    CHECK(encode_display_objective({kSlotList, "hp"}) == hex("00026870"));
    CHECK(encode_display_objective({kSlotBelowName, "d2"}) == hex("02026432"));
    // sidebar.team.red: 3 + red's 12
    CHECK(encode_display_objective({15, "d3"}) == hex("0f026433"));
    CHECK(parse_display_objective(hex("0f026433")) == DisplayObjective{15, "d3"});
}

TEST_CASE("Update Objectives is the jar's", "[protocol][scoreboard]") {
    const ObjectiveUpdate d{"d", ObjectiveMode::Create, R"({"text":"d"})", 0};
    CHECK(encode_update_objectives(d) == hex("0164000c7b2274657874223a2264227d00"));
    CHECK(parse_update_objectives(hex("0164000c7b2274657874223a2264227d00")) == d);
    // health's hearts
    CHECK(encode_update_objectives({"hp", ObjectiveMode::Create, R"({"text":"hp"})", 1}) ==
          hex("026870000d7b2274657874223a226870227d01"));
    // setdisplay sidebar.team.red, cleared: the objective leaves
    CHECK(encode_update_objectives({"d3", ObjectiveMode::Remove, {}, 0}) == hex("02643301"));
    CHECK(parse_update_objectives(hex("02643301")) == ObjectiveUpdate{"d3", ObjectiveMode::Remove, {}, 0});
}

TEST_CASE("Update Teams, every mode, is the jar's", "[protocol][scoreboard]") {
    // team add red: Create with the defaults
    TeamUpdate create;
    create.name                    = "red";
    create.mode                    = TeamMode::Create;
    create.parameters.display_json = R"({"text":"red"})";
    create.parameters.prefix_json  = kEmpty;
    create.parameters.suffix_json  = kEmpty;
    const auto create_bytes =
        hex("03726564000e7b2274657874223a22726564227d0306616c7761797306616c77617973150b7b2274657874223a22"
            "227d0b7b2274657874223a22227d00");
    CHECK(encode_update_teams(create) == create_bytes);
    CHECK(parse_update_teams(create_bytes) == create);

    // team modify blue color blue: an Update
    TeamUpdate blue;
    blue.name                    = "blue";
    blue.mode                    = TeamMode::Update;
    blue.parameters.display_json = R"({"text":"Blue Team"})";
    blue.parameters.color        = 9;
    blue.parameters.prefix_json  = kEmpty;
    blue.parameters.suffix_json  = kEmpty;
    CHECK(encode_update_teams(blue) ==
          hex("04626c756502147b2274657874223a22426c7565205465616d227d0306616c7761797306616c77617973090b7b"
              "2274657874223a22227d0b7b2274657874223a22227d"));

    // what a newcomer is sent of red, every option changed and one member
    TeamUpdate red;
    red.name                          = "red";
    red.mode                          = TeamMode::Create;
    red.parameters.display_json       = R"({"color":"dark_red","text":"Rouge"})";
    red.parameters.flags              = 0;
    red.parameters.nametag_visibility = "hideForOtherTeams";
    red.parameters.collision_rule     = "pushOwnTeam";
    red.parameters.color              = 12;
    red.parameters.prefix_json        = R"({"text":"[R] "})";
    red.parameters.suffix_json        = R"({"text":" !"})";
    red.entities                      = {"ovprobe"};
    const auto red_bytes = hex(
        "0372656400237b22636f6c6f72223a226461726b5f726564222c2274657874223a22526f756765227d001168696465"
        "466f724f746865725465616d730b707573684f776e5465616d0c0f7b2274657874223a225b525d20227d0d7b227465"
        "7874223a222021227d01076f7670726f6265");
    CHECK(encode_update_teams(red) == red_bytes);
    CHECK(parse_update_teams(red_bytes) == red);

    // team join red / leave: one name each
    CHECK(encode_update_teams({"red", TeamMode::AddEntities, {}, {"ovprobe"}}) ==
          hex("037265640301076f7670726f6265"));
    CHECK(encode_update_teams({"red", TeamMode::RemoveEntities, {}, {"ovprobe"}}) ==
          hex("037265640401076f7670726f6265"));
    // team remove green
    CHECK(encode_update_teams({"green", TeamMode::Remove, {}, {}}) == hex("05677265656e01"));
}

TEST_CASE("the scoreboard parsers refuse what is not a packet", "[protocol][scoreboard][hostile]") {
    // Each captured payload, with the parser that reads it: every truncation is
    // refused, and so is one byte too many.
    const auto refuses_every_cut = [](const std::vector<u8>& whole, auto parse) {
        usize accepted = 0;
        for (usize n = 0; n < whole.size(); ++n) {
            if (parse(std::span<const u8>{whole.data(), n}).has_value()) {
                ++accepted;
            }
        }
        std::vector<u8> longer = whole;
        longer.push_back(0);
        if (parse(std::span<const u8>{longer}).has_value()) {
            ++accepted;
        }
        return accepted == 0 && parse(std::span<const u8>{whole}).has_value();
    };
    CHECK(refuses_every_cut(hex("076f7670726f626500016405"),
                            [](std::span<const u8> b) { return parse_update_score(b); }));
    CHECK(refuses_every_cut(hex("0466616b650100"),
                            [](std::span<const u8> b) { return parse_update_score(b); }));
    CHECK(refuses_every_cut(hex("0f026433"),
                            [](std::span<const u8> b) { return parse_display_objective(b); }));
    CHECK(refuses_every_cut(hex("0164000c7b2274657874223a2264227d00"),
                            [](std::span<const u8> b) { return parse_update_objectives(b); }));
    CHECK(refuses_every_cut(hex("037265640301076f7670726f6265"),
                            [](std::span<const u8> b) { return parse_update_teams(b); }));
    CHECK(refuses_every_cut(
        hex("03726564000e7b2274657874223a22726564227d0306616c7761797306616c77617973150b7b2274657874223a"
            "22227d0b7b2274657874223a22227d00"),
        [](std::span<const u8> b) { return parse_update_teams(b); }));
    // A mode that does not exist, and a member count larger than the payload.
    CHECK_FALSE(parse_update_teams(hex("03726564090000")).has_value());
    CHECK_FALSE(parse_update_teams(hex("0372656403ffffffff07")).has_value());
    CHECK_FALSE(parse_update_objectives(hex("01640300")).has_value());
    CHECK_FALSE(parse_update_score(hex("01610201640000")).has_value());
}
