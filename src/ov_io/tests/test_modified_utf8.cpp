#include "ov/io/modified_utf8.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::io;

namespace {

std::span<const u8> bytes_of(std::string_view s) {
    return {reinterpret_cast<const u8*>(s.data()), s.size()};
}

std::string round_trip(std::string_view text) {
    const std::string encoded = encode_modified_utf8(text);
    auto              decoded = decode_modified_utf8(bytes_of(encoded));
    REQUIRE(decoded.has_value());
    return *decoded;
}

}  // namespace

TEST_CASE("ASCII is identical in both encodings", "[io][utf8]") {
    // The common case by a wide margin: resource locations, property names,
    // block ids. It must not pay for the conversion.
    for (std::string_view text : {"minecraft:stone", "facing", "", "waterlogged"}) {
        REQUIRE(is_modified_utf8_identical(text));
        REQUIRE(encode_modified_utf8(text) == text);
        REQUIRE(modified_utf8_length(text) == text.size());
        REQUIRE(round_trip(text) == text);
    }
}

TEST_CASE("null is encoded as C0 80, never as a bare zero", "[io][utf8]") {
    // This is the whole reason the encoding is "modified": a string may contain
    // a null without terminating anything.
    const std::string text{'a', '\0', 'b'};
    const std::string encoded = encode_modified_utf8(text);

    REQUIRE(encoded.size() == 4);
    REQUIRE(static_cast<u8>(encoded[0]) == 'a');
    REQUIRE(static_cast<u8>(encoded[1]) == 0xC0);
    REQUIRE(static_cast<u8>(encoded[2]) == 0x80);
    REQUIRE(static_cast<u8>(encoded[3]) == 'b');

    REQUIRE_FALSE(is_modified_utf8_identical(text));
    REQUIRE(modified_utf8_length(text) == 4);
    REQUIRE(round_trip(text) == text);
}

TEST_CASE("a bare null byte is rejected on decode", "[io][utf8]") {
    const std::vector<u8> malformed{'a', 0x00, 'b'};
    const auto            result = decode_modified_utf8({malformed.data(), malformed.size()});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ReadError::MalformedEncoding);
}

TEST_CASE("two- and three-byte sequences match plain UTF-8", "[io][utf8]") {
    for (std::string_view text : {"café", "日本語", "Ω≈ç√", "über"}) {
        REQUIRE(is_modified_utf8_identical(text));
        REQUIRE(encode_modified_utf8(text) == text);
        REQUIRE(round_trip(text) == text);
    }
}

TEST_CASE("astral characters become surrogate pairs, not four-byte sequences", "[io][utf8]") {
    // U+1F600 GRINNING FACE. Standard UTF-8 uses four bytes; modified UTF-8
    // writes a surrogate pair as two three-byte sequences, six bytes total.
    // This is the case that silently corrupts an emoji on a sign if the
    // encoding is treated as ordinary UTF-8.
    const std::string emoji = "\xF0\x9F\x98\x80";
    REQUIRE(emoji.size() == 4);
    REQUIRE_FALSE(is_modified_utf8_identical(emoji));

    const std::string encoded = encode_modified_utf8(emoji);
    REQUIRE(encoded.size() == 6);
    REQUIRE(modified_utf8_length(emoji) == 6);

    // ED A0 BD ED B8 80 — high surrogate D83D, low surrogate DE00.
    REQUIRE(static_cast<u8>(encoded[0]) == 0xED);
    REQUIRE(static_cast<u8>(encoded[1]) == 0xA0);
    REQUIRE(static_cast<u8>(encoded[2]) == 0xBD);
    REQUIRE(static_cast<u8>(encoded[3]) == 0xED);
    REQUIRE(static_cast<u8>(encoded[4]) == 0xB8);
    REQUIRE(static_cast<u8>(encoded[5]) == 0x80);

    // And it comes back as the original four-byte UTF-8.
    REQUIRE(round_trip(emoji) == emoji);
}

TEST_CASE("mixed text round-trips", "[io][utf8]") {
    const std::string sign = "Café \xF0\x9F\x98\x80 日本 ok";
    REQUIRE(round_trip(sign) == sign);
}

TEST_CASE("a four-byte sequence is rejected on decode", "[io][utf8]") {
    // Valid standard UTF-8, but illegal in modified UTF-8 where astral
    // characters must arrive as surrogate pairs. Accepting it would mean
    // silently tolerating a non-vanilla encoder.
    const std::vector<u8> four_byte{0xF0, 0x9F, 0x98, 0x80};
    const auto            result = decode_modified_utf8({four_byte.data(), four_byte.size()});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == ReadError::MalformedEncoding);
}

TEST_CASE("truncated sequences are rejected", "[io][utf8]") {
    const std::vector<std::vector<u8>> truncated{
        {0xC3},              // two-byte sequence cut short
        {0xE6, 0x97},        // three-byte sequence cut short
        {0xC3, 0x28},        // bad continuation byte
        {0xE6, 0x97, 0x28},  // bad second continuation
    };
    for (const auto& input : truncated) {
        const auto result = decode_modified_utf8({input.data(), input.size()});
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error() == ReadError::MalformedEncoding);
    }
}

TEST_CASE("an unpaired surrogate degrades instead of failing", "[io][utf8]") {
    // A lone high surrogate has no standard-UTF-8 representation. Substituting
    // U+FFFD keeps one malformed sign from failing an entire chunk load, which
    // is the behaviour that matters when reading someone else's world.
    const std::vector<u8> lone_high{0xED, 0xA0, 0xBD};
    const auto            result = decode_modified_utf8({lone_high.data(), lone_high.size()});
    REQUIRE(result.has_value());
    REQUIRE(*result == "\xEF\xBF\xBD");
}

TEST_CASE("length is computed without encoding", "[io][utf8]") {
    // Writing an NBT string needs the byte length before the bytes, so this
    // must agree with the encoder exactly or the length prefix lies.
    for (std::string_view text :
         {"stone", "café", "日本語", "\xF0\x9F\x98\x80", "mixed \xF0\x9F\x98\x80 text"}) {
        REQUIRE(modified_utf8_length(text) == encode_modified_utf8(text).size());
    }
    const std::string with_null{'a', '\0'};
    REQUIRE(modified_utf8_length(with_null) == encode_modified_utf8(with_null).size());
}
