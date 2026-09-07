#include "ov/nbt/region.hpp"

#include "ov/io/byte_reader.hpp"
#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"

#include <algorithm>

namespace ov::nbt {

std::string_view to_string(RegionError error) noexcept {
    switch (error) {
        case RegionError::TruncatedHeader: return "region header is truncated";
        case RegionError::OffsetOutOfRange: return "chunk offset points past end of file";
        case RegionError::BadChunkLength: return "chunk length does not fit its sectors";
        case RegionError::UnsupportedScheme: return "unsupported compression scheme";
        case RegionError::DecompressionFailed: return "chunk decompression failed";
        case RegionError::InvalidNbt: return "chunk NBT is invalid";
        case RegionError::ChunkAbsent: return "chunk is not present in this region";
    }
    return "unknown region error";
}

RegionResult<RegionFile> RegionFile::open(std::vector<u8> data) {
    if (data.size() < kRegionHeaderSize) {
        // An entirely empty file is legal — vanilla creates one before writing
        // any chunk — but a partial header is corruption.
        if (data.empty()) {
            RegionFile region;
            region.data_ = std::move(data);
            return region;
        }
        return std::unexpected{RegionError::TruncatedHeader};
    }

    RegionFile region;
    region.data_ = std::move(data);

    io::ByteReader reader{region.data_};

    for (usize i = 0; i < kRegionChunkCount; ++i) {
        const auto entry = reader.read_u32();
        if (!entry) {
            return std::unexpected{RegionError::TruncatedHeader};
        }
        const u32 offset = *entry >> 8;
        const u8  count  = static_cast<u8>(*entry & 0xFF);

        if (offset != 0 && count != 0) {
            // What is validated here is only that the chunk's own header is
            // readable. The sector count is an *allocation*, and real region
            // files do not pad the file out to it: a save from a real world was
            // 1819235 bytes — 444.15 sectors — with its last chunk allocated
            // through sector 445. Requiring offset + count to fit inside the
            // file rejects perfectly good worlds. The payload length is checked
            // against what actually exists at read time instead.
            //
            // Sector 0 and 1 are the header, so chunk data starts at 2.
            const usize base = static_cast<usize>(offset) * kSectorSize;
            if (offset < 2 || base + 5 > region.data_.size()) {
                return std::unexpected{RegionError::OffsetOutOfRange};
            }
        }
        region.locations_[i] = Location{offset, count};
    }

    for (usize i = 0; i < kRegionChunkCount; ++i) {
        const auto timestamp = reader.read_u32();
        if (!timestamp) {
            return std::unexpected{RegionError::TruncatedHeader};
        }
        region.timestamps_[i] = *timestamp;
    }

    return region;
}

RegionResult<RegionFile> RegionFile::open(const std::filesystem::path& path) {
    auto data = io::read_file(path);
    if (!data) {
        return std::unexpected{RegionError::TruncatedHeader};
    }
    return open(std::move(*data));
}

bool RegionFile::has_chunk(u32 local_x, u32 local_z) const noexcept {
    if (local_x >= kRegionSideChunks || local_z >= kRegionSideChunks) {
        return false;
    }
    return locations_[index_of(local_x, local_z)].present();
}

bool RegionFile::has_chunk(ChunkPos pos) const noexcept {
    return has_chunk(static_cast<u32>(floor_mod(pos.x, 32)),
                     static_cast<u32>(floor_mod(pos.z, 32)));
}

u32 RegionFile::timestamp(u32 local_x, u32 local_z) const noexcept {
    if (local_x >= kRegionSideChunks || local_z >= kRegionSideChunks) {
        return 0;
    }
    return timestamps_[index_of(local_x, local_z)];
}

usize RegionFile::chunk_count() const noexcept {
    usize count = 0;
    for (const auto& location : locations_) {
        count += location.present() ? usize{1} : usize{0};
    }
    return count;
}

std::optional<ChunkCompression> RegionFile::chunk_compression(u32 local_x, u32 local_z) const {
    if (!has_chunk(local_x, local_z)) {
        return std::nullopt;
    }
    const auto& location = locations_[index_of(local_x, local_z)];
    const usize base     = static_cast<usize>(location.sector_offset) * kSectorSize;
    if (base + 5 > data_.size()) {
        return std::nullopt;
    }
    return static_cast<ChunkCompression>(data_[base + 4]);
}

RegionResult<std::vector<u8>> RegionFile::read_chunk_bytes(u32 local_x, u32 local_z) const {
    if (!has_chunk(local_x, local_z)) {
        return std::unexpected{RegionError::ChunkAbsent};
    }

    const auto& location = locations_[index_of(local_x, local_z)];
    const usize base     = static_cast<usize>(location.sector_offset) * kSectorSize;

    // The chunk may not use its whole allocation, and the file may stop short
    // of it — real region files are not padded to a sector boundary. Bound by
    // whichever is smaller: what was allocated, and what is actually there.
    const usize allocated = static_cast<usize>(location.sector_count) * kSectorSize;
    const usize available = data_.size() - base;
    const usize capacity  = std::min(allocated, available);

    io::ByteReader reader{std::span<const u8>{data_}};
    reader.seek(base);

    const auto declared = reader.read_u32();
    if (!declared) {
        return std::unexpected{RegionError::BadChunkLength};
    }
    // The length counts the scheme byte, so it must be at least 1, and the
    // payload it promises has to exist.
    if (*declared == 0 || capacity < sizeof(u32) || *declared > capacity - sizeof(u32)) {
        return std::unexpected{RegionError::BadChunkLength};
    }

    const auto scheme_byte = reader.read_u8();
    if (!scheme_byte) {
        return std::unexpected{RegionError::BadChunkLength};
    }

    const auto payload = reader.read_bytes(*declared - 1);
    if (!payload) {
        return std::unexpected{RegionError::BadChunkLength};
    }

    switch (static_cast<ChunkCompression>(*scheme_byte)) {
        case ChunkCompression::Gzip: {
            auto result = io::gzip_decompress(*payload);
            if (!result)
                return std::unexpected{RegionError::DecompressionFailed};
            return std::move(*result);
        }
        case ChunkCompression::Zlib: {
            auto result = io::zlib_decompress(*payload);
            if (!result)
                return std::unexpected{RegionError::DecompressionFailed};
            return std::move(*result);
        }
        case ChunkCompression::None: return std::vector<u8>{payload->begin(), payload->end()};
    }

    // Schemes 4 (LZ4) and 127 (custom) postdate 1.20.1. Reading one would mean
    // the file came from a newer version, which is worth saying out loud rather
    // than failing somewhere deeper.
    return std::unexpected{RegionError::UnsupportedScheme};
}

RegionResult<Document> RegionFile::read_chunk(u32 local_x, u32 local_z) const {
    auto bytes = read_chunk_bytes(local_x, local_z);
    if (!bytes) {
        return std::unexpected{bytes.error()};
    }
    auto document = read(*bytes);
    if (!document) {
        return std::unexpected{RegionError::InvalidNbt};
    }
    return std::move(*document);
}

RegionResult<Document> RegionFile::read_chunk(ChunkPos pos) const {
    return read_chunk(static_cast<u32>(floor_mod(pos.x, 32)),
                      static_cast<u32>(floor_mod(pos.z, 32)));
}

}  // namespace ov::nbt
