#include "ov/protocol/types.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::net;
using Catch::Approx;

namespace {
TypeResult<std::string> decode_string(const std::vector<u8>& bytes, u32 max = kMaxStringLength) {
    io::ByteReader reader{std::span<const u8>{bytes}};
    return read_string(reader, max);
}
}  // namespace

TEST_CASE("a string is a VarInt byte count then UTF-8", "[protocol][types]") {
    io::ByteWriter writer;
    write_string(writer, "Ondes VOXEL");
    const auto bytes = writer.take();

    REQUIRE(bytes[0] == 11);
    REQUIRE(bytes.size() == 12);
    REQUIRE(decode_string(bytes).value() == "Ondes VOXEL");
}

TEST_CASE("strings round-trip, including non-ASCII", "[protocol][types]") {
    const std::vector<std::string> texts{"", "stone", "minecraft:oak_door", "Café ☃",
                                         std::string(1000, 'x')};
    for (const auto& text : texts) {
        io::ByteWriter writer;
        write_string(writer, text);
        const auto bytes = writer.take();
        REQUIRE(bytes.size() == string_size(text));
        REQUIRE(decode_string(bytes).value() == text);
    }
}

TEST_CASE("the byte prefix counts bytes, not characters", "[protocol][types]") {
    // Easy to get backwards, and it desynchronises the stream when it is.
    const std::string snowman = "☃";  // three UTF-8 bytes, one character
    io::ByteWriter    writer;
    write_string(writer, snowman);
    const auto bytes = writer.take();
    REQUIRE(bytes[0] == 3);
}

TEST_CASE("a string past its limit is rejected", "[protocol][types]") {
    // Fields declare their own maximum — a chat message is 256 — and the limit
    // is what stops a length prefix from becoming an allocation.
    io::ByteWriter writer;
    write_string(writer, std::string(300, 'x'));
    const auto bytes = writer.take();

    REQUIRE(decode_string(bytes, 256).has_value());  // 300 bytes is under 256*3
    REQUIRE(decode_string(bytes, 10).error() == TypeError::StringTooLong);
}

TEST_CASE("a huge declared length does not allocate", "[protocol][types]") {
    // The shape of the attack: a few bytes claiming a two-gigabyte string. It
    // must fail on the bound, before anything is reserved.
    const std::vector<u8> hostile{0xFF, 0xFF, 0xFF, 0xFF, 0x07, 'a'};
    const auto            result = decode_string(hostile);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == TypeError::StringTooLong);
}

TEST_CASE("a negative length is rejected", "[protocol][types]") {
    const std::vector<u8> negative{0xFF, 0xFF, 0xFF, 0xFF, 0x0F};  // -1
    REQUIRE(decode_string(negative).error() == TypeError::BadVarInt);
}

TEST_CASE("a truncated string is rejected", "[protocol][types]") {
    const std::vector<u8> truncated{10, 'a', 'b'};  // claims 10, has 2
    REQUIRE(decode_string(truncated).error() == TypeError::UnexpectedEnd);
}

TEST_CASE("invalid UTF-8 is rejected", "[protocol][types]") {
    // A client sends strings before it is authenticated. Letting malformed
    // UTF-8 through means it surfaces later in a log line, a JSON document or
    // a filename, far from where it entered.
    const std::vector<std::vector<u8>> malformed{
        {2, 0xC3, 0x28},              // bad continuation
        {1, 0x80},                    // continuation byte alone
        {4, 0xF0, 0x82, 0x82, 0xAC},  // overlong encoding of U+20AC
        {3, 0xED, 0xA0, 0x80},        // UTF-16 surrogate, not valid UTF-8
        {2, 0xC0, 0x80},              // overlong NUL
    };
    for (const auto& bytes : malformed) {
        REQUIRE(decode_string(bytes).error() == TypeError::BadUtf8);
    }
}

TEST_CASE("valid multi-byte UTF-8 is accepted", "[protocol][types]") {
    const std::vector<std::string> texts{"é", "☃", "\xF0\x9F\x98\x80", "日本語"};
    for (const auto& text : texts) {
        io::ByteWriter writer;
        write_string(writer, text);
        REQUIRE(decode_string(writer.take()).value() == text);
    }
}

TEST_CASE("a UUID is two big-endian halves", "[protocol][types]") {
    const Uuid uuid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

    io::ByteWriter writer;
    write_uuid(writer, uuid);
    const auto bytes = writer.take();

    REQUIRE(bytes.size() == 16);
    REQUIRE(bytes[0] == 0x01);
    REQUIRE(bytes[7] == 0xEF);
    REQUIRE(bytes[8] == 0xFE);

    io::ByteReader reader{std::span<const u8>{bytes}};
    REQUIRE(read_uuid(reader).value() == uuid);
}

TEST_CASE("a UUID formats and parses in canonical form", "[protocol][types]") {
    const Uuid uuid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    REQUIRE(uuid.to_string() == "01234567-89ab-cdef-fedc-ba9876543210");

    REQUIRE(Uuid::parse("01234567-89ab-cdef-fedc-ba9876543210").value() == uuid);
    // The unhyphenated form appears in launcher files.
    REQUIRE(Uuid::parse("0123456789abcdeffedcba9876543210").value() == uuid);

    REQUIRE_FALSE(Uuid::parse("too-short").has_value());
    REQUIRE_FALSE(Uuid::parse("").has_value());
}

TEST_CASE("an angle is 1/256 of a turn", "[protocol][types]") {
    io::ByteWriter writer;
    write_angle(writer, 0.0f);
    write_angle(writer, 90.0f);
    write_angle(writer, 180.0f);
    // Beyond a full turn must wrap rather than overflow the byte.
    write_angle(writer, 450.0f);
    const auto bytes = writer.take();

    REQUIRE(bytes[0] == 0);
    REQUIRE(bytes[1] == 64);
    REQUIRE(bytes[2] == 128);
    REQUIRE(bytes[3] == 64);

    io::ByteReader reader{std::span<const u8>{bytes}};
    REQUIRE(read_angle(reader).value() == Approx(0.0f));
    REQUIRE(read_angle(reader).value() == Approx(90.0f));
    REQUIRE(read_angle(reader).value() == Approx(180.0f));
}

TEST_CASE("errors have readable names", "[protocol][types]") {
    REQUIRE(to_string(TypeError::BadUtf8) == "string is not valid UTF-8");
    REQUIRE(to_string(TypeError::StringTooLong) == "string exceeds its limit");
}
