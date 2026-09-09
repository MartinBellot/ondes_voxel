// The verbs, on the wire.
//
// The bytes below are what a real 1.20.1 client sends; the shapes were verified
// against a real 1.20.1 *server*, which is the harder direction — a wrongly
// numbered Interact is not refused, it is ignored, and a whole measurement
// campaign then reports that a diamond sword does no damage.
#include "ov/protocol/interaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace ov;
using namespace ov::net;

namespace {

std::string hex(const std::vector<u8>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string           out;
    out.reserve(bytes.size() * 2);
    for (const u8 byte : bytes) {
        out.push_back(kDigits[byte >> 4U]);
        out.push_back(kDigits[byte & 0x0FU]);
    }
    return out;
}

std::vector<u8> bytes(std::initializer_list<int> values) {
    std::vector<u8> out;
    out.reserve(values.size());
    for (const int value : values) {
        out.push_back(static_cast<u8>(value));
    }
    return out;
}

}  // namespace

TEST_CASE("an attack carries no hand", "[protocol][interaction]") {
    // entity 42, type 1 (attack), not sneaking. Three bytes, and the absent
    // hand is the whole point: reading one here would eat the sneak flag.
    const auto packet = bytes({42, 1, 0});
    const auto parsed = parse_interact(packet);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->entity_id == 42);
    REQUIRE(parsed->kind == InteractKind::Attack);
    REQUIRE_FALSE(parsed->hand.has_value());
    REQUIRE_FALSE(parsed->sneaking);
}

TEST_CASE("a plain interaction carries a hand and a sneak flag",
          "[protocol][interaction]") {
    const auto packet = bytes({7, 0, 1, 1});
    const auto parsed = parse_interact(packet);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind == InteractKind::Interact);
    REQUIRE(parsed->hand == Hand::Off);
    REQUIRE(parsed->sneaking);
}

TEST_CASE("interact-at carries three floats before the hand",
          "[protocol][interaction]") {
    // 0.0f, 1.0f, 0.5f as big-endian floats.
    const auto packet = bytes({9, 2, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x00,
                               0x00, 0x00, 0, 0});
    const auto parsed = parse_interact(packet);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->kind == InteractKind::InteractAt);
    REQUIRE(parsed->target_y == 1.0F);
    REQUIRE(parsed->target_z == 0.5F);
    REQUIRE(parsed->hand == Hand::Main);
}

TEST_CASE("a truncated interact is refused, not guessed", "[protocol][interaction]") {
    REQUIRE_FALSE(parse_interact(bytes({42, 1})).has_value());
    REQUIRE_FALSE(parse_interact(bytes({42})).has_value());
    REQUIRE_FALSE(parse_interact({}).has_value());
    // Type 3 does not exist in 763.
    REQUIRE_FALSE(parse_interact(bytes({42, 3, 0})).has_value());
}

TEST_CASE("use item is a hand and a sequence", "[protocol][interaction]") {
    const auto parsed = parse_use_item(bytes({0, 0x80, 0x02}));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->hand == Hand::Main);
    REQUIRE(parsed->sequence == 256);
}

TEST_CASE("swing arm is one varint", "[protocol][interaction]") {
    REQUIRE(parse_swing_arm(bytes({0})) == Hand::Main);
    REQUIRE(parse_swing_arm(bytes({1})) == Hand::Off);
    REQUIRE_FALSE(parse_swing_arm(bytes({2})).has_value());
}

TEST_CASE("set cooldown is two varints", "[protocol][interaction]") {
    REQUIRE(hex(encode_set_cooldown(908, 20)) == "8c0714");
}

TEST_CASE("block action carries the block type last", "[protocol][interaction]") {
    // A chest at the origin opening: action 1, parameter 1.
    REQUIRE(hex(encode_block_action(WirePosition{0, 0, 0}, 1, 1, 130)) ==
            "0000000000000000" "0101" "8201");
}
