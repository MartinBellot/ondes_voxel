#include "../src/png.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <span>
#include <vector>

using namespace ov;
using namespace ov::render;

namespace {

u32 crc32_of(std::span<const u8> data) {
    u32 crc = 0xFFFFFFFFU;
    for (const u8 byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = ((crc & 1U) != 0U) ? (crc >> 1) ^ 0xEDB88320U : crc >> 1;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

void put_u32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value >> 24));
    out.push_back(static_cast<u8>(value >> 16));
    out.push_back(static_cast<u8>(value >> 8));
    out.push_back(static_cast<u8>(value));
}

void put_chunk(std::vector<u8>& out, std::string_view type, std::span<const u8> data) {
    put_u32(out, static_cast<u32>(data.size()));
    std::vector<u8> body;
    for (const char character : type) {
        body.push_back(static_cast<u8>(character));
    }
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_u32(out, crc32_of(body));
}

/// A zlib stream of stored (uncompressed) DEFLATE blocks.
///
/// Every PNG the tests need is a handful of texels, so there is nothing to
/// compress; stored blocks keep the fixtures readable and the test binary free
/// of a compressor it would otherwise have to trust to be correct.
std::vector<u8> zlib_stored(std::span<const u8> raw) {
    std::vector<u8> out{0x78, 0x01};

    usize offset = 0;
    do {
        const usize length = std::min<usize>(65535, raw.size() - offset);
        const bool  last   = offset + length >= raw.size();
        out.push_back(last ? 1U : 0U);
        out.push_back(static_cast<u8>(length & 0xFFU));
        out.push_back(static_cast<u8>(length >> 8));
        const auto complement = static_cast<u16>(~static_cast<u16>(length));
        out.push_back(static_cast<u8>(complement & 0xFFU));
        out.push_back(static_cast<u8>(complement >> 8));
        out.insert(out.end(), raw.begin() + static_cast<isize>(offset),
                   raw.begin() + static_cast<isize>(offset + length));
        offset += length;
    } while (offset < raw.size());

    u32 low  = 1;
    u32 high = 0;
    for (const u8 byte : raw) {
        low  = (low + byte) % 65521U;
        high = (high + low) % 65521U;
    }
    put_u32(out, (high << 16) | low);
    return out;
}

}  // namespace

namespace ov::render {

/// Shared with test_atlas.cpp, which needs real PNG bytes to feed a
/// MemoryAssetSource; it declares this prototype rather than pulling in a
/// header of its own, since two call sites do not justify a third file.
///
/// `channels` selects the colour type — 1 greyscale, 3 RGB, 4 RGBA — so that
/// the decoder's normalisation to RGBA8 is exercised rather than assumed.
std::vector<u8> encode_test_png(u32 width, u32 height, u32 channels, std::span<const u8> pixels) {
    const usize stride = static_cast<usize>(width) * channels;

    std::vector<u8> raw;
    raw.reserve(static_cast<usize>(height) * (stride + 1));
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);  // Filter type 0: none.
        const usize row = static_cast<usize>(y) * stride;
        raw.insert(raw.end(), pixels.begin() + static_cast<isize>(row),
                   pixels.begin() + static_cast<isize>(row + stride));
    }

    u8 colour_type = 6;
    if (channels == 1) {
        colour_type = 0;
    } else if (channels == 3) {
        colour_type = 2;
    }

    std::vector<u8> header;
    put_u32(header, width);
    put_u32(header, height);
    header.push_back(8);            // Bit depth.
    header.push_back(colour_type);  // Colour type.
    header.push_back(0);            // Compression: DEFLATE.
    header.push_back(0);            // Filter method.
    header.push_back(0);            // Interlace: none.

    std::vector<u8> out{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    put_chunk(out, "IHDR", header);
    put_chunk(out, "IDAT", zlib_stored(raw));
    put_chunk(out, "IEND", {});
    return out;
}

}  // namespace ov::render

TEST_CASE("an RGBA image decodes to exactly the bytes it was built from", "[png]") {
    const std::vector<u8> pixels{
        255, 0,   0,   255,  //
        0,   255, 0,   128,  //
        0,   0,   255, 0,    //
        1,   2,   3,   4,    //
    };
    const auto bytes = encode_test_png(2, 2, 4, pixels);

    const auto image = png::decode_rgba8(bytes);
    REQUIRE(image.has_value());
    CHECK(image->width == 2);
    CHECK(image->height == 2);
    CHECK(image->rgba == pixels);
}

TEST_CASE("a three-channel image comes back opaque", "[png]") {
    // The atlas never asks what colour type a file was. Everything downstream
    // indexes by 4, so a texture saved without an alpha channel has to arrive
    // with one.
    const std::vector<u8> pixels{
        10, 20, 30,  //
        40, 50, 60,  //
    };
    const auto bytes = encode_test_png(2, 1, 3, pixels);

    const auto image = png::decode_rgba8(bytes);
    REQUIRE(image.has_value());
    REQUIRE(image->rgba.size() == 8);
    CHECK(image->rgba[3] == 255);
    CHECK(image->rgba[7] == 255);
    CHECK(image->rgba[0] == 10);
    CHECK(image->rgba[4] == 40);
}

TEST_CASE("greyscale expands across the colour channels", "[png]") {
    const std::vector<u8> pixels{0, 128, 255, 64};
    const auto            bytes = encode_test_png(4, 1, 1, pixels);

    const auto image = png::decode_rgba8(bytes);
    REQUIRE(image.has_value());
    REQUIRE(image->rgba.size() == 16);
    CHECK(image->rgba[4] == 128);
    CHECK(image->rgba[5] == 128);
    CHECK(image->rgba[6] == 128);
    CHECK(image->rgba[7] == 255);
}

TEST_CASE("index() addresses rows the way the decoder writes them", "[png]") {
    const std::vector<u8> pixels{
        1, 1, 1, 255, 2, 2, 2, 255,  //
        3, 3, 3, 255, 4, 4, 4, 255,  //
    };
    const auto image = png::decode_rgba8(encode_test_png(2, 2, 4, pixels));
    REQUIRE(image.has_value());

    // Row-major from the top-left. A decoder that flipped the image vertically
    // would pass every other test in this file and put every sprite upside
    // down in the atlas.
    CHECK(image->rgba[image->index(0, 0)] == 1);
    CHECK(image->rgba[image->index(1, 0)] == 2);
    CHECK(image->rgba[image->index(0, 1)] == 3);
    CHECK(image->rgba[image->index(1, 1)] == 4);
}

TEST_CASE("a file that is not a PNG is an error, not a crash", "[png]") {
    // Every one of these is something a resource pack has actually contained:
    // an empty file, a JPEG someone renamed, and a PNG truncated by a failed
    // download.
    CHECK_FALSE(png::decode_rgba8({}).has_value());

    const std::vector<u8> not_a_png{0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F'};
    CHECK_FALSE(png::decode_rgba8(not_a_png).has_value());

    // Cut mid-IDAT. Losing only the trailing IEND is *not* an error to
    // libspng, which is a defensible reading of the format and not worth
    // fighting: the image data is all there.
    auto truncated = encode_test_png(2, 2, 4, std::vector<u8>(16, 255));
    truncated.resize(truncated.size() / 2);
    CHECK_FALSE(png::decode_rgba8(truncated).has_value());
}

TEST_CASE("a corrupt chunk checksum is refused", "[png]") {
    auto bytes = encode_test_png(2, 2, 4, std::vector<u8>(16, 255));
    // Flip a bit in the middle of the image data. A decoder that ignores CRCs
    // would hand back a plausible-looking texture built from garbage.
    bytes[bytes.size() / 2] = static_cast<u8>(bytes[bytes.size() / 2] ^ 0xFFU);
    CHECK_FALSE(png::decode_rgba8(bytes).has_value());
}

TEST_CASE("every decode error has a message", "[png]") {
    for (const auto error : {png::DecodeError::Malformed, png::DecodeError::Unsupported,
                             png::DecodeError::TooLarge, png::DecodeError::OutOfMemory}) {
        CHECK_FALSE(png::to_string(error).empty());
    }
}
