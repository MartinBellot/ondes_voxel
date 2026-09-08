// PNG decoding, and the one place libspng is allowed.
//
// The same rule that keeps simdjson inside json.cpp applies here, for a second
// reason on top of build time: libspng is a C library whose header defines a
// pile of unprefixed macros and enum constants, and a public header that drags
// it in would put SPNG_FMT_RGBA8 in the vocabulary of every file that wants to
// know how wide a sprite is. So it appears in png.cpp and nowhere else, and the
// rest of the module sees a width, a height and a byte vector.
//
// Decoding only. Nothing in the client writes a PNG: screenshots belong to the
// window layer, and the atlas is uploaded, never saved.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ov::render::png {

enum class DecodeError : u8 {
    /// Not a PNG, or a corrupt one.
    Malformed,
    /// Valid PNG, but a variant the decoder was not built to handle.
    Unsupported,
    /// Larger than kMaxDimension on a side.
    TooLarge,
    /// Allocation failed. A 16384² sprite sheet is a quarter of a gigabyte.
    OutOfMemory,
};

[[nodiscard]] std::string_view to_string(DecodeError error) noexcept;

/// Refused above this on either axis.
///
/// PNG itself allows 2³¹−1, which as RGBA8 is more memory than exists. A
/// resource pack is a file a player downloaded from a stranger, so the size is
/// checked from the header before a single row is decoded rather than
/// discovered when the allocation throws.
inline constexpr u32 kMaxDimension = 16384;

/// A decoded image, always RGBA8, always row-major from the top-left, always
/// width * height * 4 bytes.
///
/// Everything the atlas handles is normalised to this one layout: greyscale,
/// palette, 16-bit and tRNS transparency are all resolved by the decoder, so
/// nothing downstream has to ask what a texture's colour type was.
struct Image {
    u32             width{0};
    u32             height{0};
    std::vector<u8> rgba;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }

    /// Byte offset of a pixel. No bounds check: callers loop over their own
    /// dimensions, and a check here would be in the inner loop of stitching.
    [[nodiscard]] usize index(u32 x, u32 y) const noexcept {
        return (static_cast<usize>(y) * width + x) * 4;
    }
};

[[nodiscard]] std::expected<Image, DecodeError> decode_rgba8(std::span<const u8> bytes);

}  // namespace ov::render::png
