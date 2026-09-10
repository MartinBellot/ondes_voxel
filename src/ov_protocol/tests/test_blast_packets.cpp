// The Explosion packet, against bytes a real 1.20.1 server sent.
//
// scripts/measure_tnt_gravity.py capture summoned `tnt {Fuse:0}` at
// (13.5, -60, 5.5) next to a probe client and wrote down the packet that
// opened with that centre. It came back as id 0x1D — not the 0x1E the archived
// wiki gives — and it carried 827 records for a charge that broke a few dozen
// blocks, because the list holds every cell a ray reached, air included.
#include "ov/protocol/blast.hpp"

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

}  // namespace

TEST_CASE("the explosion packet opens the way the captured one does", "[protocol][blast]") {
    REQUIRE(clientbound::kExplosion == 0x1D);

    Explosion explosion;
    explosion.x      = 13.5;
    // -60 raised by a sixteenth of the TNT's 0.98F height, widened from float:
    // the exact double the server sent.
    explosion.y      = -60.0 + static_cast<f64>(0.98F) * 0.0625;
    explosion.z      = 5.5;
    explosion.radius = 4.0F;
    // The first three of the 827 records, in the order they arrived.
    explosion.records = {{-4, 0, -4}, {3, 1, 5}, {3, -1, 0}};
    const std::vector<u8> bytes = encode_explosion(explosion);

    // Captured prefix: x, y, z, radius, then the count (here 3, there 827 =
    // 0xbb 0x06) and the first record fc 00 fc.
    const std::string text = hex(bytes);
    REQUIRE(text.substr(0, 56) ==
            "402b000000000000"    // x = 13.5
            "c04df828f5c00000"    // y = -59.93874999880791
            "4016000000000000"    // z = 5.5
            "40800000");          // radius 4
    REQUIRE(text.substr(56, 2) == "03");
    REQUIRE(text.substr(58, 18) == "fc00fc" "030105" "03ff00");
    REQUIRE(bytes.size() == 8 * 3 + 4 + 1 + 3 * 3 + 12);
}

TEST_CASE("an explosion survives a round trip", "[protocol][blast]") {
    Explosion explosion;
    explosion.x           = -3.25;
    explosion.y           = 64.06125;
    explosion.z           = 1000.5;
    explosion.radius      = 3.0F;
    explosion.knockback_x = -0.5545357F;
    explosion.knockback_y = 0.2881275F;
    explosion.knockback_z = 0.0F;
    REQUIRE(explosion.add_record(BlockPos{-4, 64, 1000}));
    REQUIRE(explosion.add_record(BlockPos{-3, 63, 1001}));
    // 200 blocks away does not fit a signed byte and is refused, not wrapped.
    REQUIRE_FALSE(explosion.add_record(BlockPos{196, 64, 1000}));

    const auto parsed = parse_explosion(encode_explosion(explosion));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->x == explosion.x);
    REQUIRE(parsed->y == explosion.y);
    REQUIRE(parsed->z == explosion.z);
    REQUIRE(parsed->radius == explosion.radius);
    REQUIRE(parsed->records == explosion.records);
    // floor(-3.25) is -4, so the block at x = -4 is offset 0, not -1.
    REQUIRE(parsed->records[0] == std::array<i8, 3>{0, 0, 0});
    REQUIRE(parsed->records[1] == std::array<i8, 3>{1, -1, 1});
    REQUIRE(parsed->knockback_x == explosion.knockback_x);
    REQUIRE(parsed->knockback_y == explosion.knockback_y);
}

TEST_CASE("a truncated explosion is refused rather than half read", "[protocol][blast]") {
    Explosion explosion;
    explosion.radius = 4.0F;
    explosion.records.push_back({1, 2, 3});
    std::vector<u8> bytes = encode_explosion(explosion);
    bytes.pop_back();
    REQUIRE_FALSE(parse_explosion(bytes).has_value());

    // A count claiming more records than the payload holds.
    std::vector<u8> lying = encode_explosion(explosion);
    lying[28] = 0x7F;
    REQUIRE_FALSE(parse_explosion(lying).has_value());
}

TEST_CASE("the measured metadata indices", "[protocol][blast]") {
    REQUIRE(metadata::kTntFuse == 8);
    REQUIRE(metadata::kFallingBlockStart == 8);
    REQUIRE(metadata::kCreeperSwellDir == 16);
    REQUIRE(metadata::kCreeperPowered == 17);
    REQUIRE(metadata::kCreeperIgnited == 18);
    REQUIRE(kDefaultTntFuse == 80);
}
