#include "ov/protocol/breaking.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::net;

// Bytes captured from a real 1.20.1 server by scripts/capture_destroy_stage.py:
// the second player, four blocks away, received this while the first (entity
// 1) held a stone at (1, -60, 3) for 200 ticks. docs/provenance/cassage-bloc.md.
TEST_CASE("Set Block Destroy Stage decodes what the real server sent", "[breaking]") {
    // entity 1, position (1, -60, 3), stage 12 — a stage past 9.
    const std::vector<u8> captured{0x01, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
                                   0x3F, 0xC4, 0x0C};
    const auto packet = parse_block_destroy_stage(captured);
    REQUIRE(packet.has_value());
    CHECK(packet->entity_id == 1);
    CHECK(packet->position == WirePosition{1, -60, 3});
    CHECK(packet->stage == 12);
    CHECK_FALSE(is_drawn_stage(packet->stage));

    // The abort that followed: -1.
    const std::vector<u8> abort{0x01, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x3F, 0xC4, 0xFF};
    const auto removed = parse_block_destroy_stage(abort);
    REQUIRE(removed.has_value());
    CHECK(removed->stage == -1);
}

TEST_CASE("Set Block Destroy Stage round-trips and refuses a short or long body",
          "[breaking]") {
    const BlockDestroyStage sent{300, WirePosition{-12345, 200, 678}, 7};
    const auto              bytes = encode_block_destroy_stage(sent);
    const auto              back  = parse_block_destroy_stage(bytes);
    REQUIRE(back.has_value());
    CHECK(*back == sent);

    std::vector<u8> short_body(bytes.begin(), bytes.end() - 1);
    CHECK_FALSE(parse_block_destroy_stage(short_body).has_value());
    std::vector<u8> long_body = bytes;
    long_body.push_back(0);
    CHECK_FALSE(parse_block_destroy_stage(long_body).has_value());
}

TEST_CASE("only 0 to 9 is a drawn stage", "[breaking]") {
    CHECK_FALSE(is_drawn_stage(-1));
    for (i32 stage = 0; stage <= 9; ++stage) {
        CHECK(is_drawn_stage(stage));
    }
    CHECK_FALSE(is_drawn_stage(10));
    CHECK_FALSE(is_drawn_stage(14));
}

TEST_CASE("the server's stage is floor(count * 10), saturated", "[breaking]") {
    CHECK(server_destroy_stage(0.0F) == 0);
    CHECK(server_destroy_stage(0.0999F) == 0);
    CHECK(server_destroy_stage(0.1F) == 1);
    CHECK(server_destroy_stage(0.95F) == 9);
    CHECK(server_destroy_stage(1.4F) == 14);
    CHECK(server_destroy_stage(1000.0F) == 127);
}

TEST_CASE("Player Action and Swing Arm are laid out as the wire expects", "[breaking]") {
    const auto action = encode_player_action(2, WirePosition{1, -60, 3}, 2, 5);
    // status, position (8 bytes), face, sequence.
    const std::vector<u8> expected{0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
                                   0x3F, 0xC4, 0x02, 0x05};
    CHECK(action == expected);

    CHECK(encode_swing_arm(Hand::Main) == std::vector<u8>{0x00});
    CHECK(encode_swing_arm(Hand::Off) == std::vector<u8>{0x01});
    CHECK(parse_swing_arm(encode_swing_arm(Hand::Off)) == Hand::Off);
}
