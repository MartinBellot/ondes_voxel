#include "ov/protocol/varint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::net;

namespace {

std::vector<u8> encode(i32 value) {
    io::ByteWriter writer;
    write_varint(writer, value);
    return writer.take();
}

std::vector<u8> encode_long(i64 value) {
    io::ByteWriter writer;
    write_varlong(writer, value);
    return writer.take();
}

VarIntResult<i32> decode(const std::vector<u8>& bytes) {
    io::ByteReader reader{std::span<const u8>{bytes}};
    return read_varint(reader);
}

VarIntResult<i64> decode_long(const std::vector<u8>& bytes) {
    io::ByteReader reader{std::span<const u8>{bytes}};
    return read_varlong(reader);
}

}  // namespace

TEST_CASE("VarInt matches the values in the protocol specification", "[protocol][varint]") {
    // The exact table from the wire format documentation. Pinning to someone
    // else's numbers, not to our own encoder — a round-trip test alone passes
    // even when both halves are wrong in the same way.
    REQUIRE(encode(0) == std::vector<u8>{0x00});
    REQUIRE(encode(1) == std::vector<u8>{0x01});
    REQUIRE(encode(2) == std::vector<u8>{0x02});
    REQUIRE(encode(127) == std::vector<u8>{0x7F});
    REQUIRE(encode(128) == std::vector<u8>{0x80, 0x01});
    REQUIRE(encode(255) == std::vector<u8>{0xFF, 0x01});
    REQUIRE(encode(25565) == std::vector<u8>{0xDD, 0xC7, 0x01});
    REQUIRE(encode(2097151) == std::vector<u8>{0xFF, 0xFF, 0x7F});
    REQUIRE(encode(2147483647) == std::vector<u8>{0xFF, 0xFF, 0xFF, 0xFF, 0x07});
    REQUIRE(encode(-1) == std::vector<u8>{0xFF, 0xFF, 0xFF, 0xFF, 0x0F});
    REQUIRE(encode(-2147483648) == std::vector<u8>{0x80, 0x80, 0x80, 0x80, 0x08});
}

TEST_CASE("VarLong matches the specification", "[protocol][varint]") {
    REQUIRE(encode_long(0) == std::vector<u8>{0x00});
    REQUIRE(encode_long(127) == std::vector<u8>{0x7F});
    REQUIRE(encode_long(128) == std::vector<u8>{0x80, 0x01});
    REQUIRE(encode_long(2147483647) == std::vector<u8>{0xFF, 0xFF, 0xFF, 0xFF, 0x07});
    REQUIRE(encode_long(9223372036854775807LL) ==
            std::vector<u8>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F});
    REQUIRE(encode_long(-1) ==
            std::vector<u8>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01});
    REQUIRE(encode_long(-2147483648LL) ==
            std::vector<u8>{0x80, 0x80, 0x80, 0x80, 0xF8, 0xFF, 0xFF, 0xFF, 0xFF, 0x01});
}

TEST_CASE("a negative VarInt uses all five bytes", "[protocol][varint]") {
    // Two's complement, so the high bits are all set. An encoder that stops
    // when the value reaches zero emits nothing at all for a negative number,
    // and the whole stream shifts.
    REQUIRE(encode(-1).size() == 5);
    REQUIRE(encode(-100).size() == 5);
    REQUIRE(varint_size(-1) == 5);
    REQUIRE(encode_long(-1).size() == 10);
    REQUIRE(varlong_size(-1) == 10);
}

TEST_CASE("every VarInt round-trips", "[protocol][varint]") {
    const std::vector<i32> values{0,
                                  1,
                                  -1,
                                  127,
                                  128,
                                  -128,
                                  255,
                                  256,
                                  25565,
                                  2097151,
                                  2097152,
                                  -2097152,
                                  65535,
                                  123456,
                                  -123456,
                                  2147483647,
                                  -2147483647 - 1};
    for (const i32 value : values) {
        const auto bytes = encode(value);
        REQUIRE(bytes.size() == varint_size(value));
        const auto decoded = decode(bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(*decoded == value);
    }
}

TEST_CASE("every VarLong round-trips", "[protocol][varint]") {
    const std::vector<i64> values{0,
                                  1,
                                  -1,
                                  127,
                                  128,
                                  2147483647,
                                  -2147483648LL,
                                  9223372036854775807LL,
                                  -9223372036854775807LL - 1};
    for (const i64 value : values) {
        const auto bytes = encode_long(value);
        REQUIRE(bytes.size() == varlong_size(value));
        const auto decoded = decode_long(bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(*decoded == value);
    }
}

TEST_CASE("size is computable without encoding", "[protocol][varint]") {
    // A length-prefixed packet has to know its own length before writing it,
    // so these two must never disagree.
    for (i32 value = -100000; value < 100000; value += 997) {
        REQUIRE(varint_size(value) == encode(value).size());
    }
    REQUIRE(varint_size(0) == 1);
    REQUIRE(varint_size(127) == 1);
    REQUIRE(varint_size(128) == 2);
    REQUIRE(varint_size(2147483647) == 5);
}

// ── Malformed input ─────────────────────────────────────────────────────────
// The first thing a hostile peer sends is a VarInt, before any authentication.

TEST_CASE("a truncated VarInt is rejected", "[protocol][varint][malformed]") {
    // Continuation bit set with nothing following.
    REQUIRE(decode({0x80}).error() == VarIntError::UnexpectedEnd);
    REQUIRE(decode({0xFF, 0xFF}).error() == VarIntError::UnexpectedEnd);
    REQUIRE(decode({}).error() == VarIntError::UnexpectedEnd);
}

TEST_CASE("an over-long VarInt is rejected", "[protocol][varint][malformed]") {
    // Six bytes of continuation. Without the bound this reads for as long as
    // the peer keeps sending, on unauthenticated input.
    REQUIRE(decode({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}).error() == VarIntError::TooLarge);
    REQUIRE(decode({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00}).error() ==
            VarIntError::TooLarge);
}

TEST_CASE("an over-long VarLong is rejected", "[protocol][varint][malformed]") {
    const std::vector<u8> eleven_bytes(11, 0xFF);
    REQUIRE(decode_long(eleven_bytes).error() == VarIntError::TooLarge);
}

TEST_CASE("a five-byte VarInt is accepted, a sixth is not", "[protocol][varint][malformed]") {
    // The boundary itself: -1 is exactly five bytes and must still decode.
    REQUIRE(decode({0xFF, 0xFF, 0xFF, 0xFF, 0x0F}).value() == -1);
    REQUIRE(decode({0xFF, 0xFF, 0xFF, 0xFF, 0x8F, 0x01}).error() == VarIntError::TooLarge);
}

TEST_CASE("reading stops after the value, leaving the rest", "[protocol][varint]") {
    // A packet is a VarInt length followed by a body; the reader must be
    // positioned exactly at the body afterwards.
    const std::vector<u8> stream{0xDD, 0xC7, 0x01, 'r', 'e', 's', 't'};
    io::ByteReader        reader{std::span<const u8>{stream}};

    REQUIRE(read_varint(reader).value() == 25565);
    REQUIRE(reader.remaining() == 4);
    REQUIRE(reader.read_u8().value() == 'r');
}

TEST_CASE("consecutive VarInts decode independently", "[protocol][varint]") {
    io::ByteWriter writer;
    write_varint(writer, 1);
    write_varint(writer, -1);
    write_varint(writer, 25565);
    write_varint(writer, 0);
    const auto bytes = writer.take();

    io::ByteReader reader{std::span<const u8>{bytes}};
    REQUIRE(read_varint(reader).value() == 1);
    REQUIRE(read_varint(reader).value() == -1);
    REQUIRE(read_varint(reader).value() == 25565);
    REQUIRE(read_varint(reader).value() == 0);
    REQUIRE(reader.exhausted());
}

TEST_CASE("errors have readable names", "[protocol][varint]") {
    REQUIRE(to_string(VarIntError::TooLarge) == "VarInt is too big");
    REQUIRE(to_string(VarIntError::UnexpectedEnd) == "unexpected end of data");
}
