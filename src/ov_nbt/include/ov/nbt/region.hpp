// Anvil region files (.mca).
//
// A region holds a 32x32 grid of chunks in one file:
//
//   offset 0      1024 x u32   location: 3 bytes sector offset, 1 byte sector count
//   offset 4096   1024 x u32   last-modified timestamps, seconds since the epoch
//   offset 8192   chunk data, aligned to 4096-byte sectors
//
// Each chunk payload is:
//
//   length(u32, big-endian)   number of bytes that follow, including the scheme
//   scheme(u8)                1 = gzip, 2 = zlib (what vanilla writes), 3 = none
//   data                      length - 1 bytes of compressed NBT
//
// Scheme 4 (LZ4) and 127 (custom) were added in 24w04a and 24w05a, after 1.20.1,
// so they are rejected here rather than silently mishandled.
//
// A location entry of all zeros means the chunk is absent, which is normal:
// most region files are sparse.
//
// Everything in this file is read from untrusted input. A region file from
// someone else's world can claim a chunk lives past the end of the file, or
// that it decompresses to gigabytes. Neither may do anything worse than return
// an error.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/block_pos.hpp"
#include "ov/nbt/binary.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ov::nbt {

enum class RegionError {
    /// File is shorter than the 8 KiB header.
    TruncatedHeader,
    /// A location entry points past the end of the file.
    OffsetOutOfRange,
    /// Declared payload length does not fit in the sectors allocated to it.
    BadChunkLength,
    /// Compression scheme is not 1, 2 or 3.
    UnsupportedScheme,
    DecompressionFailed,
    InvalidNbt,
    /// Chunk is not present in this region.
    ChunkAbsent,
};

[[nodiscard]] std::string_view to_string(RegionError error) noexcept;

template<typename T>
using RegionResult = std::expected<T, RegionError>;

/// Compression schemes, as stored in the byte before each chunk's payload.
enum class ChunkCompression : u8 {
    Gzip = 1,
    Zlib = 2,  // what vanilla writes
    None = 3,
};

inline constexpr usize kRegionSideChunks = 32;
inline constexpr usize kRegionChunkCount = kRegionSideChunks * kRegionSideChunks;
inline constexpr usize kSectorSize       = 4096;
inline constexpr usize kRegionHeaderSize = 2 * kRegionChunkCount * sizeof(u32);

/// A region file held in memory.
///
/// Region files are a few megabytes, so this reads the whole file rather than
/// mapping it. Chunks are decompressed on demand: loading all 1024 up front
/// would cost far more than any caller needs.
class RegionFile {
public:
    /// Parse the header of an in-memory region file. Chunk payloads are not
    /// touched until requested.
    [[nodiscard]] static RegionResult<RegionFile> open(std::vector<u8> data);

    /// Read a whole region file from disk.
    [[nodiscard]] static RegionResult<RegionFile> open(const std::filesystem::path& path);

    /// True if the region stores this chunk. Coordinates are region-relative,
    /// each in [0, 32).
    [[nodiscard]] bool has_chunk(u32 local_x, u32 local_z) const noexcept;
    [[nodiscard]] bool has_chunk(ChunkPos pos) const noexcept;

    /// Last-modified time, in seconds since the Unix epoch. 0 when absent.
    [[nodiscard]] u32 timestamp(u32 local_x, u32 local_z) const noexcept;

    /// Decompress and parse one chunk.
    [[nodiscard]] RegionResult<Document> read_chunk(u32 local_x, u32 local_z) const;
    [[nodiscard]] RegionResult<Document> read_chunk(ChunkPos pos) const;

    /// The raw decompressed bytes, before NBT parsing. Used by the round-trip
    /// check, which has to compare against exactly what was on disk.
    [[nodiscard]] RegionResult<std::vector<u8>> read_chunk_bytes(u32 local_x, u32 local_z) const;

    /// Which scheme a stored chunk uses.
    [[nodiscard]] std::optional<ChunkCompression> chunk_compression(u32 local_x, u32 local_z) const;

    /// Number of chunks actually present, out of 1024.
    [[nodiscard]] usize chunk_count() const noexcept;

    [[nodiscard]] usize byte_size() const noexcept { return data_.size(); }

private:
    struct Location {
        u32 sector_offset{0};
        u8  sector_count{0};

        [[nodiscard]] bool present() const noexcept {
            return sector_offset != 0 && sector_count != 0;
        }
    };

    [[nodiscard]] static usize index_of(u32 local_x, u32 local_z) noexcept {
        return static_cast<usize>(local_z) * kRegionSideChunks + local_x;
    }

    std::vector<u8>                         data_;
    std::array<Location, kRegionChunkCount> locations_{};
    std::array<u32, kRegionChunkCount>      timestamps_{};
};

}  // namespace ov::nbt
