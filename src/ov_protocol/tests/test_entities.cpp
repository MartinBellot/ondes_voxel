// The three packets a dropped stack needs, checked against bytes a real 1.20.1
// server sent.
//
// The ids and the field order were captured rather than read off a summary: the
// archived protocol page gave the wrong number for every packet this project
// has checked against it. What is asserted below is the exact payload the game
// produced for a known entity, so a field that moves or a varint that grows
// fails here rather than as an item that renders as nothing.
#include "ov/protocol/play.hpp"

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

TEST_CASE("a dropped stack spawns the way the game spawns one", "[protocol][entity]") {
    // Captured: `summon item 3 -60 0 {Item:{id:"minecraft:diamond",Count:5b}}`,
    // which the server announced as entity 4 of type 54 at 3.5, -60, 0.5.
    const Uuid uuid{0x91a8fed51c1a415dULL, 0xbc5321e3c744e2e4ULL};
    const auto encoded = encode_spawn_entity(4, uuid, kItemEntityType, 3.5, -60.0, 0.5);

    REQUIRE(hex(encoded) ==
            "04"                                // entity id
            "91a8fed51c1a415dbc5321e3c744e2e4"  // uuid
            "36"                                // type 54, minecraft:item
            "400c000000000000"                  // x = 3.5
            "c04e000000000000"                  // y = -60
            "3fe0000000000000"                  // z = 0.5
            "000000"                            // pitch, yaw, head yaw
            "00"                                // data
            "000000000000");                    // velocity
    REQUIRE(encoded.size() == 52);
}

TEST_CASE("the metadata says what the stack holds", "[protocol][entity]") {
    // Same capture: five diamonds, item id 764.
    REQUIRE(hex(encode_item_metadata(4, 764, 5)) ==
            "04"    // entity id
            "08"    // index 8 — the stack an item entity carries
            "07"    // type 7 — a slot
            "01"    // present
            "fc05"  // item 764
            "05"    // count
            "00"    // no nbt
            "ff");  // end of metadata
}

TEST_CASE("the pickup names both entities and the count", "[protocol][entity]") {
    // Captured when the player walked onto it: entity 7 taken by entity 6.
    REQUIRE(hex(encode_take_item(7, 6, 5)) == "070605");
}

TEST_CASE("item is entity type 54", "[protocol][entity]") {
    // Hard-coded by the client and never sent to it, like every other id in
    // this registry: a difference of one spawns the wrong thing entirely.
    REQUIRE(kItemEntityType == 54);
}
