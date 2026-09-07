#include "ov/io/compression.hpp"

#include <libdeflate.h>

#include <algorithm>
#include <memory>

namespace ov::io {
namespace {

struct DecompressorDeleter {
    void operator()(libdeflate_decompressor* p) const noexcept { libdeflate_free_decompressor(p); }
};

struct CompressorDeleter {
    void operator()(libdeflate_compressor* p) const noexcept { libdeflate_free_compressor(p); }
};

using DecompressorPtr = std::unique_ptr<libdeflate_decompressor, DecompressorDeleter>;
using CompressorPtr   = std::unique_ptr<libdeflate_compressor, CompressorDeleter>;

/// gzip stores the uncompressed size, modulo 2^32, in its last four bytes.
///
/// It is a hint, not a guarantee — it is attacker-controlled and wraps for
/// inputs above 4 GiB — so it is used to size the first attempt and never to
/// bypass the cap.
[[nodiscard]] usize gzip_size_hint(std::span<const u8> input) noexcept {
    if (input.size() < 4) {
        return 0;
    }
    const auto tail = input.subspan(input.size() - 4);
    return static_cast<usize>(tail[0]) | (static_cast<usize>(tail[1]) << 8) |
           (static_cast<usize>(tail[2]) << 16) | (static_cast<usize>(tail[3]) << 24);
}

enum class Container { Gzip, Zlib, Deflate };

[[nodiscard]] CompressionResult<std::vector<u8>> decompress_with(std::span<const u8> input,
                                                                 usize limit, Container container) {
    if (input.empty()) {
        return std::unexpected{CompressionError::Corrupt};
    }

    DecompressorPtr decompressor{libdeflate_alloc_decompressor()};
    if (!decompressor) {
        return std::unexpected{CompressionError::OutOfMemory};
    }

    // Start from the gzip hint when it is plausible, otherwise from a multiple
    // of the input size. Both are only starting points: the loop grows until it
    // fits or hits the cap.
    usize capacity = container == Container::Gzip ? gzip_size_hint(input) : 0;
    if (capacity == 0 || capacity > limit) {
        capacity = std::min(limit, std::max<usize>(input.size() * 4, 1024));
    }

    std::vector<u8> output;
    for (;;) {
        try {
            output.resize(capacity);
        } catch (const std::bad_alloc&) {
            return std::unexpected{CompressionError::OutOfMemory};
        }

        usize      produced = 0;
        const auto result =
            container == Container::Gzip
                ? libdeflate_gzip_decompress(decompressor.get(), input.data(), input.size(),
                                             output.data(), output.size(), &produced)
                : libdeflate_zlib_decompress(decompressor.get(), input.data(), input.size(),
                                             output.data(), output.size(), &produced);

        if (result == LIBDEFLATE_SUCCESS) {
            output.resize(produced);
            return output;
        }
        if (result != LIBDEFLATE_INSUFFICIENT_SPACE) {
            return std::unexpected{CompressionError::Corrupt};
        }
        if (capacity >= limit) {
            // The stream really does expand past what the caller allows. This
            // is the decompression-bomb case, and it must be an error rather
            // than an ever-growing allocation.
            return std::unexpected{CompressionError::TooLarge};
        }
        capacity = std::min(limit, capacity * 2);
    }
}

}  // namespace

std::string_view to_string(CompressionError error) noexcept {
    switch (error) {
        case CompressionError::Corrupt: return "corrupt compressed stream";
        case CompressionError::TooLarge: return "decompressed size exceeds limit";
        case CompressionError::OutOfMemory: return "out of memory";
    }
    return "unknown compression error";
}

bool looks_like_gzip(std::span<const u8> data) noexcept {
    return data.size() >= 2 && data[0] == 0x1F && data[1] == 0x8B;
}

bool looks_like_zlib(std::span<const u8> data) noexcept {
    if (data.size() < 2) {
        return false;
    }
    // CMF low nibble is the compression method: 8 is deflate. The two header
    // bytes together are a multiple of 31.
    const bool deflate_method = (data[0] & 0x0F) == 0x08;
    const u32  header         = (static_cast<u32>(data[0]) << 8) | data[1];
    return deflate_method && (header % 31) == 0;
}

CompressionResult<std::vector<u8>> gzip_decompress(std::span<const u8> input, usize limit) {
    return decompress_with(input, limit, Container::Gzip);
}

CompressionResult<std::vector<u8>> zlib_decompress(std::span<const u8> input, usize limit) {
    return decompress_with(input, limit, Container::Zlib);
}

CompressionResult<std::vector<u8>> decompress(std::span<const u8> input, usize limit) {
    if (looks_like_gzip(input)) {
        return gzip_decompress(input, limit);
    }
    if (looks_like_zlib(input)) {
        return zlib_decompress(input, limit);
    }
    return std::unexpected{CompressionError::Corrupt};
}

namespace {

[[nodiscard]] CompressionResult<std::vector<u8>> compress_with(std::span<const u8> input, int level,
                                                               Container container) {
    CompressorPtr compressor{libdeflate_alloc_compressor(std::clamp(level, 1, 12))};
    if (!compressor) {
        return std::unexpected{CompressionError::OutOfMemory};
    }

    const usize bound = [&] {
        switch (container) {
            case Container::Gzip:
                return libdeflate_gzip_compress_bound(compressor.get(), input.size());
            case Container::Zlib:
                return libdeflate_zlib_compress_bound(compressor.get(), input.size());
            case Container::Deflate:
                return libdeflate_deflate_compress_bound(compressor.get(), input.size());
        }
        return usize{0};
    }();

    std::vector<u8> output;
    try {
        output.resize(bound);
    } catch (const std::bad_alloc&) {
        return std::unexpected{CompressionError::OutOfMemory};
    }

    const usize produced = [&] {
        switch (container) {
            case Container::Gzip:
                return libdeflate_gzip_compress(compressor.get(), input.data(), input.size(),
                                                output.data(), output.size());
            case Container::Zlib:
                return libdeflate_zlib_compress(compressor.get(), input.data(), input.size(),
                                                output.data(), output.size());
            case Container::Deflate:
                return libdeflate_deflate_compress(compressor.get(), input.data(), input.size(),
                                                   output.data(), output.size());
        }
        return usize{0};
    }();

    if (produced == 0) {
        return std::unexpected{CompressionError::Corrupt};
    }
    output.resize(produced);
    return output;
}

}  // namespace

CompressionResult<std::vector<u8>> deflate_decompress(std::span<const u8> input,
                                                      usize               uncompressed_size) {
    if (input.empty() && uncompressed_size != 0) {
        return std::unexpected{CompressionError::Corrupt};
    }

    DecompressorPtr decompressor{libdeflate_alloc_decompressor()};
    if (!decompressor) {
        return std::unexpected{CompressionError::OutOfMemory};
    }

    std::vector<u8> output;
    try {
        output.resize(uncompressed_size);
    } catch (const std::bad_alloc&) {
        return std::unexpected{CompressionError::OutOfMemory};
    }

    usize      produced = 0;
    const auto result   = libdeflate_deflate_decompress(
        decompressor.get(), input.data(), input.size(), output.data(), output.size(), &produced);
    if (result != LIBDEFLATE_SUCCESS || produced != uncompressed_size) {
        return std::unexpected{CompressionError::Corrupt};
    }
    return output;
}

CompressionResult<std::vector<u8>> deflate_compress(std::span<const u8> input, int level) {
    return compress_with(input, level, Container::Deflate);
}

CompressionResult<std::vector<u8>> gzip_compress(std::span<const u8> input, int level) {
    return compress_with(input, level, Container::Gzip);
}

CompressionResult<std::vector<u8>> zlib_compress(std::span<const u8> input, int level) {
    return compress_with(input, level, Container::Zlib);
}

}  // namespace ov::io
