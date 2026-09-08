#include "png.hpp"

#include <spng.h>

#include <memory>
#include <new>

namespace ov::render::png {

namespace {

struct ContextDeleter {
    void operator()(spng_ctx* context) const noexcept { spng_ctx_free(context); }
};

using ContextPtr = std::unique_ptr<spng_ctx, ContextDeleter>;

/// Everything that is not a size limit or an allocation failure reads the same
/// way to a caller: this file is not a texture we can use.
DecodeError classify(int code) noexcept {
    switch (code) {
        case SPNG_EMEM: return DecodeError::OutOfMemory;
        case SPNG_EUSER_WIDTH:
        case SPNG_EUSER_HEIGHT:
        case SPNG_EWIDTH:
        case SPNG_EHEIGHT: return DecodeError::TooLarge;
        case SPNG_EFMT: return DecodeError::Unsupported;
        default: return DecodeError::Malformed;
    }
}

/// A pack is untrusted input, so a chunk that claims to be enormous must be
/// refused before it is allocated. 16 MiB is far past anything a texture needs
/// and far below anything that hurts.
constexpr usize kMaxChunkBytes = 16u * 1024u * 1024u;

}  // namespace

std::string_view to_string(DecodeError error) noexcept {
    switch (error) {
        case DecodeError::Malformed: return "malformed PNG";
        case DecodeError::Unsupported: return "unsupported PNG variant";
        case DecodeError::TooLarge: return "PNG larger than the decoder's limit";
        case DecodeError::OutOfMemory: return "out of memory decoding PNG";
    }
    return "unknown PNG error";
}

std::expected<Image, DecodeError> decode_rgba8(std::span<const u8> bytes) {
    if (bytes.empty()) {
        return std::unexpected(DecodeError::Malformed);
    }

    ContextPtr context(spng_ctx_new(0));
    if (!context) {
        return std::unexpected(DecodeError::OutOfMemory);
    }

    spng_set_image_limits(context.get(), kMaxDimension, kMaxDimension);
    spng_set_chunk_limits(context.get(), kMaxChunkBytes, kMaxChunkBytes);

    // Borrows the span; the context must not outlive it, and it does not — the
    // whole decode happens before this function returns.
    if (const int code = spng_set_png_buffer(context.get(), bytes.data(), bytes.size());
        code != 0) {
        return std::unexpected(classify(code));
    }

    spng_ihdr header{};
    if (const int code = spng_get_ihdr(context.get(), &header); code != 0) {
        return std::unexpected(classify(code));
    }

    usize byte_count = 0;
    if (const int code = spng_decoded_image_size(context.get(), SPNG_FMT_RGBA8, &byte_count);
        code != 0) {
        return std::unexpected(classify(code));
    }

    Image image;
    image.width  = header.width;
    image.height = header.height;
    try {
        image.rgba.resize(byte_count);
    } catch (const std::bad_alloc&) {
        // Not the hot path, and the alternative is propagating an exception out
        // of a function whose whole contract is that a bad pack cannot crash us.
        return std::unexpected(DecodeError::OutOfMemory);
    }

    // SPNG_DECODE_TRNS is what makes a palette or greyscale image with a tRNS
    // chunk come out with the right alpha instead of opaque. Vanilla ships
    // several of those, and without the flag they lose their transparency in a
    // way that only shows up as a black box on glass.
    if (const int code = spng_decode_image(context.get(), image.rgba.data(), image.rgba.size(),
                                           SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
        code != 0) {
        return std::unexpected(classify(code));
    }

    return image;
}

}  // namespace ov::render::png
