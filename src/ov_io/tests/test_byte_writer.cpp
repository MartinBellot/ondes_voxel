#include "ov/io/byte_reader.hpp"
#include "ov/io/byte_writer.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ov;
using namespace ov::io;

TEST_CASE("integers are written big-endian", "[io][writer]") {
    ByteWriter writer;
    writer.write_u16(0x0102);
    writer.write_u32(0x03040506);

    const auto bytes = writer.data();
    REQUIRE(bytes.size() == 6);
    REQUIRE(bytes[0] == 0x01);
    REQUIRE(bytes[1] == 0x02);
    REQUIRE(bytes[2] == 0x03);
    REQUIRE(bytes[5] == 0x06);
}

TEST_CASE("every scalar survives a write-then-read round trip", "[io][writer]") {
    ByteWriter writer;
    writer.write_i8(-42);
    writer.write_i16(-12345);
    writer.write_i32(-123456789);
    writer.write_i64(-1234567890123456789LL);
    writer.write_f32(2.5f);
    writer.write_f64(-0.125);
    writer.write_u64(0xFFFFFFFFFFFFFFFFULL);

    const auto buffer = writer.take();
    ByteReader reader{std::span<const u8>{buffer.data(), buffer.size()}};

    REQUIRE(reader.read_i8().value() == -42);
    REQUIRE(reader.read_i16().value() == -12345);
    REQUIRE(reader.read_i32().value() == -123456789);
    REQUIRE(reader.read_i64().value() == -1234567890123456789LL);
    REQUIRE(reader.read_f32().value() == 2.5f);
    REQUIRE(reader.read_f64().value() == -0.125);
    REQUIRE(reader.read_u64().value() == 0xFFFFFFFFFFFFFFFFULL);
    REQUIRE(reader.exhausted());
}

TEST_CASE("take leaves the writer empty", "[io][writer]") {
    ByteWriter writer;
    writer.write_u32(1);
    REQUIRE(writer.size() == 4);

    const auto taken = writer.take();
    REQUIRE(taken.size() == 4);
    REQUIRE(writer.empty());
}

TEST_CASE("patch_u32 rewrites a length prefix in place", "[io][writer]") {
    // Length-prefixed framing writes a placeholder, encodes the body, then
    // comes back to fill in the size. Both the region file header and the
    // packet framing need this.
    ByteWriter writer;
    writer.write_u32(0);  // placeholder
    writer.write_bytes(std::string_view{"hello"});
    writer.patch_u32(0, static_cast<u32>(writer.size() - 4));

    const auto buffer = writer.take();
    ByteReader reader{std::span<const u8>{buffer.data(), buffer.size()}};
    REQUIRE(reader.read_u32().value() == 5);
    REQUIRE(reader.remaining() == 5);
}

TEST_CASE("float bit patterns are preserved exactly", "[io][writer]") {
    // Entity positions are doubles on the wire, and a rounding difference here
    // becomes a position desync that is very hard to attribute back to I/O.
    ByteWriter writer;
    writer.write_f32(1.0f);
    writer.write_f64(3.141592653589793);

    const auto bytes = writer.data();
    REQUIRE(bytes[0] == 0x3F);
    REQUIRE(bytes[1] == 0x80);
    REQUIRE(bytes[4] == 0x40);
    REQUIRE(bytes[5] == 0x09);
    REQUIRE(bytes[6] == 0x21);
    REQUIRE(bytes[7] == 0xFB);
}

TEST_CASE("clear resets without releasing capacity", "[io][writer]") {
    ByteWriter writer{256};
    writer.write_u64(1);
    writer.clear();
    REQUIRE(writer.empty());
    REQUIRE(writer.size() == 0);
}
