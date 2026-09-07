// gzip and zlib, as Minecraft uses them.
//
// Where each one appears:
//   level.dat, player data, structure files   gzip
//   region file chunks, scheme 2 (the default) zlib
//   region file chunks, scheme 1 (rare)        gzip
//   network packets above the compression threshold  zlib
//
// LZ4 (scheme 4) and custom (scheme 127) exist in later versions but not in
// 1.20.1, so they are deliberately absent here rather than stubbed.
//
// Every input to this module is untrusted: a region file on disk, a packet from
// the network. A decompressor that trusts its input is a memory-exhaustion
// crash — a few kilobytes of crafted data can claim to expand to gigabytes —
// so every entry point takes an explicit output cap.
#pragma once

#include "ov/base/types.hpp"

#include <expected>
#include <span>
#include <vector>

namespace ov::io {

enum class CompressionError {
    /// The stream is not valid gzip or zlib.
    Corrupt,
    /// Output would exceed the caller's cap. Usually a decompression bomb.
    TooLarge,
    /// Allocation failed.
    OutOfMemory,
};

[[nodiscard]] std::string_view to_string(CompressionError error) noexcept;

template<typename T>
using CompressionResult = std::expected<T, CompressionError>;

/// Default ceiling for decompressed output.
///
/// A 1.20.1 chunk is a few hundred kilobytes at most; 64 MiB leaves a wide
/// margin for a densely populated one while still bounding a hostile file to
/// something a server survives.
inline constexpr usize kDefaultDecompressLimit = 64ull * 1024 * 1024;

[[nodiscard]] CompressionResult<std::vector<u8>> gzip_decompress(
    std::span<const u8> input, usize limit = kDefaultDecompressLimit);

[[nodiscard]] CompressionResult<std::vector<u8>> zlib_decompress(
    std::span<const u8> input, usize limit = kDefaultDecompressLimit);

/// Decompress by sniffing the container. gzip starts with 1F 8B; zlib's first
/// byte has a low nibble of 8 and the first two bytes are a multiple of 31.
[[nodiscard]] CompressionResult<std::vector<u8>> decompress(std::span<const u8> input,
                                                            usize limit = kDefaultDecompressLimit);

/// 1 is fastest, 12 is smallest. 6 matches what vanilla writes.
inline constexpr int kDefaultCompressionLevel = 6;

[[nodiscard]] CompressionResult<std::vector<u8>> gzip_compress(
    std::span<const u8> input, int level = kDefaultCompressionLevel);

[[nodiscard]] CompressionResult<std::vector<u8>> zlib_compress(
    std::span<const u8> input, int level = kDefaultCompressionLevel);

/// Raw DEFLATE, with no zlib or gzip wrapper.
///
/// This is what a ZIP entry stores, so it is what jars, resource packs and
/// datapacks need. Unlike the wrapped formats it carries neither a length nor a
/// checksum, so the exact output size must come from elsewhere — in a ZIP, from
/// the central directory. A mismatch is reported rather than grown into: it
/// means the archive contradicts itself, and guessing would hide that.
[[nodiscard]] CompressionResult<std::vector<u8>> deflate_decompress(std::span<const u8> input,
                                                                    usize uncompressed_size);

[[nodiscard]] CompressionResult<std::vector<u8>> deflate_compress(
    std::span<const u8> input, int level = kDefaultCompressionLevel);

/// True if the data begins with the gzip magic number.
[[nodiscard]] bool looks_like_gzip(std::span<const u8> data) noexcept;

/// True if the data begins with a plausible zlib header.
[[nodiscard]] bool looks_like_zlib(std::span<const u8> data) noexcept;

}  // namespace ov::io
