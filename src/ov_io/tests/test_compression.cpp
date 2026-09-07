#include "ov/io/compression.hpp"

#include <catch2/catch_test_macros.hpp>
#include <numeric>
#include <string>
#include <vector>

using namespace ov;
using namespace ov::io;

namespace {

std::vector<u8> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

std::span<const u8> span_of(const std::vector<u8>& v) { return {v.data(), v.size()}; }

}  // namespace

TEST_CASE("gzip round-trips", "[io][compression]") {
    const auto original = bytes_of(
        "Minecraft level.dat is gzip-compressed NBT, and this text is long "
        "enough and repetitive enough that it actually compresses. "
        "Minecraft level.dat is gzip-compressed NBT.");

    const auto compressed = gzip_compress(span_of(original));
    REQUIRE(compressed.has_value());
    REQUIRE(looks_like_gzip(span_of(*compressed)));
    REQUIRE(compressed->size() < original.size());

    const auto restored = gzip_decompress(span_of(*compressed));
    REQUIRE(restored.has_value());
    REQUIRE(*restored == original);
}

TEST_CASE("zlib round-trips", "[io][compression]") {
    // Scheme 2 in a region file, and the network compression format.
    const auto original = bytes_of(
        "Region file chunks use zlib by default. Region file chunks use zlib "
        "by default. Region file chunks use zlib by default.");

    const auto compressed = zlib_compress(span_of(original));
    REQUIRE(compressed.has_value());
    REQUIRE(looks_like_zlib(span_of(*compressed)));

    const auto restored = zlib_decompress(span_of(*compressed));
    REQUIRE(restored.has_value());
    REQUIRE(*restored == original);
}

TEST_CASE("empty input round-trips", "[io][compression]") {
    const std::vector<u8> empty;

    const auto gz = gzip_compress(span_of(empty));
    REQUIRE(gz.has_value());
    const auto gz_back = gzip_decompress(span_of(*gz));
    REQUIRE(gz_back.has_value());
    REQUIRE(gz_back->empty());

    const auto zl = zlib_compress(span_of(empty));
    REQUIRE(zl.has_value());
    const auto zl_back = zlib_decompress(span_of(*zl));
    REQUIRE(zl_back.has_value());
    REQUIRE(zl_back->empty());
}

TEST_CASE("large incompressible data round-trips", "[io][compression]") {
    // Chunk palettes are close to incompressible, so the output can be larger
    // than the input. The bound has to account for that rather than assume
    // compression always shrinks.
    std::vector<u8> original(300'000);
    u32 state = 12345;
    for (auto& byte : original) {
        state = state * 1664525u + 1013904223u;
        byte  = static_cast<u8>(state >> 24);
    }

    const auto compressed = zlib_compress(span_of(original));
    REQUIRE(compressed.has_value());
    const auto restored = zlib_decompress(span_of(*compressed));
    REQUIRE(restored.has_value());
    REQUIRE(*restored == original);
}

TEST_CASE("container sniffing picks the right decoder", "[io][compression]") {
    const auto original = bytes_of("sniff me");

    const auto gz = gzip_compress(span_of(original));
    const auto zl = zlib_compress(span_of(original));
    REQUIRE(gz.has_value());
    REQUIRE(zl.has_value());

    REQUIRE(decompress(span_of(*gz)).value() == original);
    REQUIRE(decompress(span_of(*zl)).value() == original);

    REQUIRE(looks_like_gzip(span_of(*gz)));
    REQUIRE_FALSE(looks_like_zlib(span_of(*gz)));
    REQUIRE(looks_like_zlib(span_of(*zl)));
    REQUIRE_FALSE(looks_like_gzip(span_of(*zl)));
}

TEST_CASE("corrupt input is rejected", "[io][compression][malformed]") {
    const auto original   = bytes_of("something worth compressing, at length");
    auto       compressed = zlib_compress(span_of(original)).value();

    // Flip a byte in the middle of the stream.
    compressed[compressed.size() / 2] ^= 0xFF;
    const auto result = zlib_decompress(span_of(compressed));
    // Either it fails, or the checksum catches it — never a read past the end,
    // which is what the sanitizer build is watching for.
    if (result.has_value()) {
        REQUIRE(*result != original);
    } else {
        REQUIRE(result.error() == CompressionError::Corrupt);
    }
}

TEST_CASE("truncated input is rejected", "[io][compression][malformed]") {
    const auto original   = bytes_of("truncate me, please, at some length");
    const auto compressed = gzip_compress(span_of(original)).value();

    for (usize length = 1; length < compressed.size(); length += 3) {
        const std::vector<u8> partial{compressed.begin(),
                                      compressed.begin() + static_cast<isize>(length)};
        const auto result = gzip_decompress(span_of(partial));
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("garbage is not mistaken for a compressed stream",
          "[io][compression][malformed]") {
    const std::vector<u8> garbage{0x00, 0x01, 0x02, 0x03, 0x04};
    REQUIRE(decompress(span_of(garbage)).error() == CompressionError::Corrupt);
    REQUIRE(gzip_decompress(span_of(garbage)).error() == CompressionError::Corrupt);

    const std::vector<u8> empty;
    REQUIRE(decompress(span_of(empty)).error() == CompressionError::Corrupt);
}

TEST_CASE("a decompression bomb is refused, not allocated",
          "[io][compression][malformed]") {
    // 8 MiB of zeros compresses to a few kilobytes. A region file could carry
    // one that claims to expand to gigabytes; without the cap, reading someone
    // else's world would be enough to take the server down.
    const std::vector<u8> zeros(8ull * 1024 * 1024, 0);
    const auto            bomb = zlib_compress(span_of(zeros)).value();
    REQUIRE(bomb.size() < 64 * 1024);

    const auto refused = zlib_decompress(span_of(bomb), 1024 * 1024);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error() == CompressionError::TooLarge);

    // With a cap that fits, the same stream decodes normally.
    const auto allowed = zlib_decompress(span_of(bomb), 16ull * 1024 * 1024);
    REQUIRE(allowed.has_value());
    REQUIRE(allowed->size() == zeros.size());
}

TEST_CASE("a lying gzip size hint does not bypass the cap",
          "[io][compression][malformed]") {
    // gzip stores its uncompressed size in the last four bytes. It is
    // attacker-controlled, so it may size the first allocation but must never
    // be trusted as permission to exceed the caller's limit.
    const std::vector<u8> zeros(4ull * 1024 * 1024, 0);
    auto                  bomb = gzip_compress(span_of(zeros)).value();

    // Claim the output is tiny.
    bomb[bomb.size() - 4] = 0x10;
    bomb[bomb.size() - 3] = 0x00;
    bomb[bomb.size() - 2] = 0x00;
    bomb[bomb.size() - 1] = 0x00;

    const auto result = gzip_decompress(span_of(bomb), 64 * 1024);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error() == CompressionError::TooLarge);
}

TEST_CASE("errors have readable names", "[io][compression]") {
    REQUIRE(to_string(CompressionError::Corrupt) == "corrupt compressed stream");
    REQUIRE(to_string(CompressionError::TooLarge) == "decompressed size exceeds limit");
}
