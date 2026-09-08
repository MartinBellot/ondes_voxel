#include "ov/nbt/region_writer.hpp"

#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"

#include <algorithm>

namespace ov::nbt {
namespace {

/// Sectors the 8 KiB header occupies: 1024 offsets and 1024 timestamps.
constexpr usize kHeaderSectors = 2;

/// The region slot a chunk occupies. RegionFile keeps its own copy private, and
/// duplicating three lines beats widening that class's surface for a writer.
[[nodiscard]] constexpr usize slot_of(u32 local_x, u32 local_z) noexcept {
    return static_cast<usize>(local_z) * kRegionSideChunks + local_x;
}

void write_u32_be(std::vector<u8>& out, usize at, u32 value) {
    out[at + 0] = static_cast<u8>(value >> 24);
    out[at + 1] = static_cast<u8>(value >> 16);
    out[at + 2] = static_cast<u8>(value >> 8);
    out[at + 3] = static_cast<u8>(value);
}

}  // namespace

RegionWriter RegionWriter::open_or_empty(const std::filesystem::path& path) {
    RegionWriter writer;

    const auto existing = RegionFile::open(path);
    if (!existing) {
        // No file yet, or one we cannot read. Starting empty is right for the
        // first case; for the second it means a damaged region is replaced
        // rather than propagated, which is the safer of two bad options.
        return writer;
    }

    for (u32 z = 0; z < kRegionSideChunks; ++z) {
        for (u32 x = 0; x < kRegionSideChunks; ++x) {
            if (!existing->has_chunk(x, z)) {
                continue;
            }
            // Carried across still compressed: re-encoding a chunk nobody
            // touched would risk changing it, and the point of keeping it is
            // that it is unchanged.
            const auto bytes = existing->read_chunk_bytes(x, z);
            if (!bytes) {
                continue;
            }
            auto compressed = io::zlib_compress(*bytes);
            if (!compressed) {
                continue;
            }
            writer.chunks_[slot_of(x, z)] =
                StoredChunk{std::move(*compressed), existing->timestamp(x, z)};
        }
    }
    return writer;
}

void RegionWriter::set_chunk(u32 local_x, u32 local_z, const Document& document, u32 timestamp) {
    if (local_x >= kRegionSideChunks || local_z >= kRegionSideChunks) {
        return;
    }
    // Qualified: an unqualified `write` here finds this class's own write(path)
    // overload, not the NBT one, and the error it produces names neither.
    auto compressed = io::zlib_compress(nbt::write(document));
    if (!compressed) {
        return;
    }
    chunks_[slot_of(local_x, local_z)] = StoredChunk{std::move(*compressed), timestamp};
}

std::vector<u8> RegionWriter::build() const {
    std::vector<u8> out(kHeaderSectors * kSectorSize, 0);
    usize           next_sector = kHeaderSectors;

    for (const auto& [slot, chunk] : chunks_) {
        // Length counts the compression byte. One short truncates the last byte
        // of every chunk in the file, which reads as a corrupt stream rather
        // than a short one.
        const u32   payload_length = static_cast<u32>(chunk.compressed.size()) + 1;
        const usize total          = 4 + 1 + chunk.compressed.size();
        const usize sectors        = (total + kSectorSize - 1) / kSectorSize;

        // The offset table holds a sector number in three bytes, so a region
        // cannot address past 2^24 sectors, and a chunk cannot span more than
        // 255. Both are far beyond anything real; refusing is still better than
        // writing a truncated field.
        if (sectors > 255 || next_sector > 0xFFFFFF) {
            continue;
        }

        const usize start = out.size();
        out.resize(start + sectors * kSectorSize, 0);
        write_u32_be(out, start, payload_length);
        out[start + 4] = static_cast<u8>(ChunkCompression::Zlib);
        std::copy(chunk.compressed.begin(), chunk.compressed.end(),
                  out.begin() + static_cast<std::ptrdiff_t>(start + 5));

        const usize offset_at = slot * 4;
        out[offset_at + 0]    = static_cast<u8>(next_sector >> 16);
        out[offset_at + 1]    = static_cast<u8>(next_sector >> 8);
        out[offset_at + 2]    = static_cast<u8>(next_sector);
        out[offset_at + 3]    = static_cast<u8>(sectors);

        write_u32_be(out, kSectorSize + slot * 4, chunk.timestamp);
        next_sector += sectors;
    }

    return out;
}

bool RegionWriter::write(const std::filesystem::path& path) const {
    // Atomic, through io's existing temporary-and-rename. A region half-written
    // over its predecessor is worse than no save at all: the old header would
    // point at sectors the new contents have moved, and every chunk in the file
    // would read as garbage.
    return io::write_file_atomic(path, build()).has_value();
}

}  // namespace ov::nbt
