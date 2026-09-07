#include "ov/io/byte_writer.hpp"
#include "ov/io/compression.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace ov;
using namespace ov::nbt;

namespace {

/// Build a region file in memory.
///
/// Real region files cannot be committed — they are Mojang world data — so the
/// unit tests construct their own. The evidence that this matches reality comes
/// from `ov-inspect region --verify` run against an actual save; see
/// docs/PROVENANCE.md.
class RegionBuilder {
public:
    RegionBuilder() : data_(kRegionHeaderSize, 0) {}

    void add_chunk(u32 local_x, u32 local_z, const Document& document,
                   ChunkCompression scheme = ChunkCompression::Zlib) {
        const auto raw = write(document);

        std::vector<u8> payload;
        switch (scheme) {
            case ChunkCompression::Gzip: payload = io::gzip_compress(raw).value(); break;
            case ChunkCompression::Zlib: payload = io::zlib_compress(raw).value(); break;
            case ChunkCompression::None: payload = raw; break;
        }

        // Chunk data starts on a sector boundary.
        pad_to_sector();
        const usize sector_offset = data_.size() / kSectorSize;

        io::ByteWriter writer;
        writer.write_u32(static_cast<u32>(payload.size() + 1));  // length includes the scheme
        writer.write_u8(static_cast<u8>(scheme));
        writer.write_bytes(std::span<const u8>{payload});
        const auto block = writer.take();
        data_.insert(data_.end(), block.begin(), block.end());
        pad_to_sector();

        const usize sector_count = (data_.size() / kSectorSize) - sector_offset;
        const usize index        = static_cast<usize>(local_z) * kRegionSideChunks + local_x;
        const u32   entry = (static_cast<u32>(sector_offset) << 8) | static_cast<u32>(sector_count);

        data_[index * 4 + 0] = static_cast<u8>(entry >> 24);
        data_[index * 4 + 1] = static_cast<u8>(entry >> 16);
        data_[index * 4 + 2] = static_cast<u8>(entry >> 8);
        data_[index * 4 + 3] = static_cast<u8>(entry);

        const usize ts = kRegionChunkCount * 4 + index * 4;
        data_[ts + 3]  = 0x2A;  // arbitrary non-zero timestamp
    }

    /// Chop the trailing bytes of the final sector, the way real files are.
    void truncate_last_sector(usize bytes) {
        if (data_.size() > bytes) {
            data_.resize(data_.size() - bytes);
        }
    }

    [[nodiscard]] std::vector<u8> take() { return std::move(data_); }

private:
    void pad_to_sector() {
        const usize remainder = data_.size() % kSectorSize;
        if (remainder != 0) {
            data_.resize(data_.size() + (kSectorSize - remainder), 0);
        }
    }

    std::vector<u8> data_;
};

Document sample_chunk(i32 x, i32 z) {
    Tag root = Tag::make_compound();
    root.put("DataVersion", Tag{static_cast<i32>(3465)});  // 1.20.1
    root.put("xPos", Tag{x});
    root.put("zPos", Tag{z});
    root.put("Status", Tag{std::string{"minecraft:full"}});

    Tag sections = Tag::make_list(TagType::Compound);
    for (i32 y = -4; y < 20; ++y) {
        Tag section = Tag::make_compound();
        section.put("Y", Tag{static_cast<i8>(y)});
        sections.push(std::move(section));
    }
    root.put("sections", std::move(sections));
    return Document{"", std::move(root)};
}

}  // namespace

TEST_CASE("an empty region file has no chunks", "[nbt][region]") {
    // Vanilla creates the file before writing anything into it.
    const auto region = RegionFile::open(std::vector<u8>{});
    REQUIRE(region.has_value());
    REQUIRE(region->chunk_count() == 0);
    REQUIRE_FALSE(region->has_chunk(0, 0));
    REQUIRE(region->read_chunk(0, 0).error() == RegionError::ChunkAbsent);
}

TEST_CASE("a header-only region file has no chunks", "[nbt][region]") {
    const auto region = RegionFile::open(std::vector<u8>(kRegionHeaderSize, 0));
    REQUIRE(region.has_value());
    REQUIRE(region->chunk_count() == 0);
}

TEST_CASE("a truncated header is rejected", "[nbt][region][malformed]") {
    const auto region = RegionFile::open(std::vector<u8>(100, 0));
    REQUIRE_FALSE(region.has_value());
    REQUIRE(region.error() == RegionError::TruncatedHeader);
}

TEST_CASE("chunks round-trip through a region file", "[nbt][region]") {
    RegionBuilder builder;
    builder.add_chunk(0, 0, sample_chunk(0, 0));
    builder.add_chunk(5, 7, sample_chunk(5, 7));
    builder.add_chunk(31, 31, sample_chunk(31, 31));

    const auto region = RegionFile::open(builder.take());
    REQUIRE(region.has_value());
    REQUIRE(region->chunk_count() == 3);

    REQUIRE(region->has_chunk(0, 0));
    REQUIRE(region->has_chunk(5, 7));
    REQUIRE(region->has_chunk(31, 31));
    REQUIRE_FALSE(region->has_chunk(1, 1));

    const auto chunk = region->read_chunk(5, 7);
    REQUIRE(chunk.has_value());
    REQUIRE(chunk->root.find("xPos")->as_i64() == 5);
    REQUIRE(chunk->root.find("zPos")->as_i64() == 7);
    REQUIRE(chunk->root.find("DataVersion")->as_i64() == 3465);
    REQUIRE(chunk->root.find("sections")->size() == 24);

    REQUIRE(region->timestamp(5, 7) != 0);
    REQUIRE(region->timestamp(1, 1) == 0);
}

TEST_CASE("all three 1.20.1 compression schemes are readable", "[nbt][region]") {
    RegionBuilder builder;
    builder.add_chunk(0, 0, sample_chunk(0, 0), ChunkCompression::Zlib);
    builder.add_chunk(1, 0, sample_chunk(1, 0), ChunkCompression::Gzip);
    builder.add_chunk(2, 0, sample_chunk(2, 0), ChunkCompression::None);

    const auto region = RegionFile::open(builder.take());
    REQUIRE(region.has_value());

    REQUIRE(region->chunk_compression(0, 0) == ChunkCompression::Zlib);
    REQUIRE(region->chunk_compression(1, 0) == ChunkCompression::Gzip);
    REQUIRE(region->chunk_compression(2, 0) == ChunkCompression::None);

    for (u32 x = 0; x < 3; ++x) {
        const auto chunk = region->read_chunk(x, 0);
        REQUIRE(chunk.has_value());
        REQUIRE(chunk->root.find("xPos")->as_i64() == x);
    }
}

TEST_CASE("a file not padded to a sector boundary is still readable", "[nbt][region]") {
    // Real region files are not padded out to their last chunk's allocation. A
    // save from an actual world measured 1819235 bytes — 444.15 sectors — with
    // its final chunk allocated through sector 445. An implementation that
    // requires offset + sector_count to fit inside the file rejects perfectly
    // good worlds, which is exactly the bug this test locks out.
    RegionBuilder builder;
    builder.add_chunk(0, 0, sample_chunk(0, 0));
    builder.add_chunk(1, 0, sample_chunk(1, 0));
    builder.truncate_last_sector(3000);

    const auto region = RegionFile::open(builder.take());
    REQUIRE(region.has_value());
    REQUIRE(region->chunk_count() == 2);

    // Both chunks still decode: the payload is intact even though the file
    // stops short of the sector it was allocated.
    REQUIRE(region->read_chunk(0, 0).has_value());
    REQUIRE(region->read_chunk(1, 0).has_value());
}

TEST_CASE("an offset past the end of the file is rejected", "[nbt][region][malformed]") {
    // The cheapest way to make a parser read memory it does not own.
    std::vector<u8> data(kRegionHeaderSize + kSectorSize, 0);
    const u32       entry = (9999u << 8) | 1u;
    data[0]               = static_cast<u8>(entry >> 24);
    data[1]               = static_cast<u8>(entry >> 16);
    data[2]               = static_cast<u8>(entry >> 8);
    data[3]               = static_cast<u8>(entry);

    const auto region = RegionFile::open(std::move(data));
    REQUIRE_FALSE(region.has_value());
    REQUIRE(region.error() == RegionError::OffsetOutOfRange);
}

TEST_CASE("an offset inside the header is rejected", "[nbt][region][malformed]") {
    // Sectors 0 and 1 are the header itself. A chunk claiming to live there
    // would have the parser reading the location table as chunk data.
    std::vector<u8> data(kRegionHeaderSize + kSectorSize, 0);
    const u32       entry = (1u << 8) | 1u;
    data[0]               = static_cast<u8>(entry >> 24);
    data[1]               = static_cast<u8>(entry >> 16);
    data[2]               = static_cast<u8>(entry >> 8);
    data[3]               = static_cast<u8>(entry);

    const auto region = RegionFile::open(std::move(data));
    REQUIRE_FALSE(region.has_value());
    REQUIRE(region.error() == RegionError::OffsetOutOfRange);
}

TEST_CASE("a declared length larger than the data is rejected", "[nbt][region][malformed]") {
    RegionBuilder builder;
    builder.add_chunk(0, 0, sample_chunk(0, 0));
    auto data = builder.take();

    // Overwrite the chunk's length prefix with something enormous.
    const usize base = 2 * kSectorSize;
    data[base + 0]   = 0x7F;
    data[base + 1]   = 0xFF;
    data[base + 2]   = 0xFF;
    data[base + 3]   = 0xFF;

    const auto region = RegionFile::open(std::move(data));
    REQUIRE(region.has_value());
    const auto chunk = region->read_chunk(0, 0);
    REQUIRE_FALSE(chunk.has_value());
    REQUIRE(chunk.error() == RegionError::BadChunkLength);
}

TEST_CASE("a zero length is rejected", "[nbt][region][malformed]") {
    RegionBuilder builder;
    builder.add_chunk(0, 0, sample_chunk(0, 0));
    auto        data = builder.take();
    const usize base = 2 * kSectorSize;
    data[base + 0] = data[base + 1] = data[base + 2] = data[base + 3] = 0;

    const auto region = RegionFile::open(std::move(data));
    REQUIRE(region.has_value());
    REQUIRE(region->read_chunk(0, 0).error() == RegionError::BadChunkLength);
}

TEST_CASE("compression schemes newer than 1.20.1 are refused by name", "[nbt][region][malformed]") {
    // LZ4 (4) arrived in 24w04a and custom (127) in 24w05a. Encountering one
    // means the file came from a newer version, which is worth saying rather
    // than failing somewhere deeper.
    for (const u8 scheme : {u8{4}, u8{127}, u8{0}, u8{99}}) {
        RegionBuilder builder;
        builder.add_chunk(0, 0, sample_chunk(0, 0));
        auto data                 = builder.take();
        data[2 * kSectorSize + 4] = scheme;

        const auto region = RegionFile::open(std::move(data));
        REQUIRE(region.has_value());
        const auto chunk = region->read_chunk(0, 0);
        REQUIRE_FALSE(chunk.has_value());
        REQUIRE(chunk.error() == RegionError::UnsupportedScheme);
    }
}

TEST_CASE("corrupt chunk data fails cleanly", "[nbt][region][malformed]") {
    RegionBuilder builder;
    builder.add_chunk(0, 0, sample_chunk(0, 0));
    auto data = builder.take();

    // Scramble the compressed payload.
    for (usize i = 2 * kSectorSize + 5; i < 2 * kSectorSize + 60 && i < data.size(); ++i) {
        data[i] ^= 0xAA;
    }

    const auto region = RegionFile::open(std::move(data));
    REQUIRE(region.has_value());
    const auto chunk = region->read_chunk(0, 0);
    // Either decompression fails or the NBT does; neither may crash.
    if (!chunk.has_value()) {
        REQUIRE((chunk.error() == RegionError::DecompressionFailed ||
                 chunk.error() == RegionError::InvalidNbt));
    }
}

TEST_CASE("out-of-range coordinates are handled", "[nbt][region]") {
    RegionBuilder builder;
    builder.add_chunk(0, 0, sample_chunk(0, 0));
    const auto region = RegionFile::open(builder.take());
    REQUIRE(region.has_value());

    REQUIRE_FALSE(region->has_chunk(32, 0));
    REQUIRE_FALSE(region->has_chunk(0, 32));
    REQUIRE(region->timestamp(99, 99) == 0);
    REQUIRE(region->read_chunk(32, 0).error() == RegionError::ChunkAbsent);
}

TEST_CASE("world coordinates map onto region-local slots", "[nbt][region]") {
    RegionBuilder builder;
    builder.add_chunk(31, 31, sample_chunk(-1, -1));
    const auto region = RegionFile::open(builder.take());
    REQUIRE(region.has_value());

    // Chunk (-1, -1) lives in region (-1, -1) at local slot (31, 31).
    REQUIRE(region->has_chunk(ChunkPos{-1, -1}));
    REQUIRE(region->read_chunk(ChunkPos{-1, -1}).has_value());
}
