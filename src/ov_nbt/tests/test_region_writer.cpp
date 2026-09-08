#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/nbt/region_writer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>

using namespace ov;
using namespace ov::nbt;

namespace {

[[nodiscard]] std::filesystem::path temp_region(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("ov_test_" + name + ".mca");
}

[[nodiscard]] Document simple_chunk(i32 value) {
    Document document;
    document.name = "";
    document.root = Tag::make_compound();
    document.root.compound()->push_back(CompoundEntry{"DataVersion", Tag{i32{3465}}});
    document.root.compound()->push_back(CompoundEntry{"Marker", Tag{value}});
    return document;
}

}  // namespace

TEST_CASE("an empty region writes a header and nothing else", "[nbt][region][write]") {
    const RegionWriter writer;
    REQUIRE(writer.empty());

    // Two sectors of zeroes: 1024 offsets then 1024 timestamps. A reader takes
    // an all-zero offset to mean "no chunk here", which is what we want.
    const auto bytes = writer.build();
    REQUIRE(bytes.size() == 8192);
    REQUIRE(std::ranges::all_of(bytes, [](u8 b) { return b == 0; }));
}

TEST_CASE("a written chunk reads back", "[nbt][region][write]") {
    const auto path = temp_region("roundtrip");
    std::filesystem::remove(path);

    RegionWriter writer;
    writer.set_chunk(0, 0, simple_chunk(1), 42);
    writer.set_chunk(31, 31, simple_chunk(2), 43);
    writer.set_chunk(5, 7, simple_chunk(3), 44);
    REQUIRE(writer.chunk_count() == 3);
    REQUIRE(writer.write(path));

    const auto region = RegionFile::open(path);
    REQUIRE(region.has_value());
    REQUIRE(region->chunk_count() == 3);

    // The corners matter: slot arithmetic that transposes x and z still works
    // for (0,0) and fails for everything else.
    REQUIRE(region->has_chunk(0, 0));
    REQUIRE(region->has_chunk(31, 31));
    REQUIRE(region->has_chunk(5, 7));
    REQUIRE_FALSE(region->has_chunk(7, 5));

    const auto read = region->read_chunk(5, 7);
    REQUIRE(read.has_value());
    REQUIRE(read->root.find("Marker")->as_i64() == 3);
    REQUIRE(region->timestamp(5, 7) == 44);

    std::filesystem::remove(path);
}

TEST_CASE("chunks start on sector boundaries", "[nbt][region][write]") {
    // The offset table addresses sectors, not bytes: there is no way to say
    // "starts partway through one", so the payload has to be padded.
    RegionWriter writer;
    writer.set_chunk(0, 0, simple_chunk(1), 0);
    writer.set_chunk(1, 0, simple_chunk(2), 0);

    const auto bytes = writer.build();
    REQUIRE(bytes.size() % 4096 == 0);

    // Second chunk's declared offset must be past the first's sectors.
    const u32 first_offset  = (u32{bytes[0]} << 16) | (u32{bytes[1]} << 8) | bytes[2];
    const u32 first_sectors = bytes[3];
    const u32 second_offset = (u32{bytes[4]} << 16) | (u32{bytes[5]} << 8) | bytes[6];
    REQUIRE(first_offset == 2);
    REQUIRE(second_offset == first_offset + first_sectors);
}

TEST_CASE("the length field counts the compression byte", "[nbt][region][write]") {
    // One short truncates the last byte of every chunk in the file, which reads
    // as a corrupt stream rather than a short one.
    RegionWriter writer;
    writer.set_chunk(0, 0, simple_chunk(7), 0);
    const auto bytes = writer.build();

    const u32 offset = ((u32{bytes[0]} << 16) | (u32{bytes[1]} << 8) | bytes[2]) * 4096;
    const u32 length = (u32{bytes[offset]} << 24) | (u32{bytes[offset + 1]} << 16) |
                       (u32{bytes[offset + 2]} << 8) | bytes[offset + 3];
    REQUIRE(bytes[offset + 4] == 2);  // zlib

    // Everything from the compression byte to the end of the payload.
    const auto compressed_size = length - 1;
    REQUIRE(compressed_size > 0);
    REQUIRE(offset + 5 + compressed_size <= bytes.size());
}

TEST_CASE("opening a missing region starts empty rather than failing", "[nbt][region][write]") {
    // The first save of a new region has nothing to carry across, and treating
    // that as an error would make a fresh world unsavable.
    const auto writer = RegionWriter::open_or_empty(temp_region("does_not_exist"));
    REQUIRE(writer.empty());
}

TEST_CASE("editing one chunk keeps the others", "[nbt][region][write]") {
    const auto path = temp_region("preserve");
    std::filesystem::remove(path);

    RegionWriter first;
    first.set_chunk(1, 1, simple_chunk(10), 1);
    first.set_chunk(2, 2, simple_chunk(20), 2);
    REQUIRE(first.write(path));

    auto second = RegionWriter::open_or_empty(path);
    REQUIRE(second.chunk_count() == 2);
    second.set_chunk(1, 1, simple_chunk(99), 3);
    REQUIRE(second.write(path));

    const auto region = RegionFile::open(path);
    REQUIRE(region.has_value());
    REQUIRE(region->chunk_count() == 2);
    REQUIRE(region->read_chunk(1, 1)->root.find("Marker")->as_i64() == 99);
    REQUIRE(region->read_chunk(2, 2)->root.find("Marker")->as_i64() == 20);

    std::filesystem::remove(path);
}

TEST_CASE("coordinates outside the region are ignored", "[nbt][region][write][malformed]") {
    RegionWriter writer;
    writer.set_chunk(32, 0, simple_chunk(1), 0);
    writer.set_chunk(0, 99, simple_chunk(1), 0);
    REQUIRE(writer.empty());
}
