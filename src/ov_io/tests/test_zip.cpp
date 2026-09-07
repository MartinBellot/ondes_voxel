#include "ov/io/byte_writer.hpp"
#include "ov/io/compression.hpp"
#include "ov/io/zip.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace ov;
using namespace ov::io;

namespace {

/// Build a ZIP archive in memory.
///
/// Real jars and resource packs cannot be committed — they are Mojang assets or
/// third-party packs — so the tests construct their own archives. The evidence
/// that this matches reality comes from pointing ov-assetimport at an actual
/// client jar; see docs/PROVENANCE.md.
class ZipBuilder {
public:
    void add(std::string name, std::string_view content, bool deflate = true) {
        const std::vector<u8> raw{content.begin(), content.end()};

        std::vector<u8> stored = raw;
        u16             method = 0;
        if (deflate) {
            auto compressed = deflate_compress(raw);
            REQUIRE(compressed.has_value());
            // Only worth it if it actually shrank; a tiny string often does not.
            if (compressed->size() < raw.size()) {
                stored = std::move(*compressed);
                method = 8;
            }
        }

        Record record;
        record.name              = std::move(name);
        record.method            = method;
        record.uncompressed_size = static_cast<u32>(raw.size());
        record.compressed_size   = static_cast<u32>(stored.size());
        record.local_offset      = static_cast<u32>(body_.size());

        io::ByteWriter local;
        write_u32(local, 0x04034b50);
        write_u16(local, 20);  // version needed
        write_u16(local, 0);   // flags
        write_u16(local, record.method);
        write_u16(local, 0);  // mod time
        write_u16(local, 0);  // mod date
        write_u32(local, 0);  // crc32 (unchecked here)
        write_u32(local, record.compressed_size);
        write_u32(local, record.uncompressed_size);
        write_u16(local, static_cast<u16>(record.name.size()));
        write_u16(local, 0);  // extra length
        local.write_bytes(record.name);
        local.write_bytes(std::span<const u8>{stored});

        const auto block = local.take();
        body_.insert(body_.end(), block.begin(), block.end());
        records_.push_back(std::move(record));
    }

    /// Insert an entry whose name is taken verbatim, bypassing any checks the
    /// builder would otherwise apply. Used to construct hostile archives.
    void add_raw_name(std::string name) { add(std::move(name), "x", false); }

    [[nodiscard]] std::vector<u8> take(std::string_view comment = {}) {
        std::vector<u8> out       = body_;
        const usize     cd_offset = out.size();

        io::ByteWriter central;
        for (const auto& record : records_) {
            write_u32(central, 0x02014b50);
            write_u16(central, 20);  // version made by
            write_u16(central, 20);  // version needed
            write_u16(central, 0);   // flags
            write_u16(central, record.method);
            write_u16(central, 0);
            write_u16(central, 0);
            write_u32(central, 0);  // crc32
            write_u32(central, record.compressed_size);
            write_u32(central, record.uncompressed_size);
            write_u16(central, static_cast<u16>(record.name.size()));
            write_u16(central, 0);  // extra
            write_u16(central, 0);  // comment
            write_u16(central, 0);  // disk
            write_u16(central, 0);  // internal attrs
            write_u32(central, 0);  // external attrs
            write_u32(central, record.local_offset);
            central.write_bytes(record.name);
        }
        const auto cd = central.take();
        out.insert(out.end(), cd.begin(), cd.end());

        io::ByteWriter eocd;
        write_u32(eocd, 0x06054b50);
        write_u16(eocd, 0);  // disk
        write_u16(eocd, 0);  // cd start disk
        write_u16(eocd, static_cast<u16>(records_.size()));
        write_u16(eocd, static_cast<u16>(records_.size()));
        write_u32(eocd, static_cast<u32>(cd.size()));
        write_u32(eocd, static_cast<u32>(cd_offset));
        write_u16(eocd, static_cast<u16>(comment.size()));
        eocd.write_bytes(comment);

        const auto tail = eocd.take();
        out.insert(out.end(), tail.begin(), tail.end());
        return out;
    }

private:
    struct Record {
        std::string name;
        u16         method{0};
        u32         uncompressed_size{0};
        u32         compressed_size{0};
        u32         local_offset{0};
    };

    // ZIP is little-endian; ByteWriter is big-endian because everything else in
    // Minecraft is. Writing the bytes out by hand keeps the two from being
    // confused for one another.
    static void write_u16(io::ByteWriter& w, u16 value) {
        w.write_u8(static_cast<u8>(value));
        w.write_u8(static_cast<u8>(value >> 8));
    }

    static void write_u32(io::ByteWriter& w, u32 value) {
        w.write_u8(static_cast<u8>(value));
        w.write_u8(static_cast<u8>(value >> 8));
        w.write_u8(static_cast<u8>(value >> 16));
        w.write_u8(static_cast<u8>(value >> 24));
    }

    std::vector<u8>     body_;
    std::vector<Record> records_;
};

std::string as_text(const std::vector<u8>& data) {
    return std::string{data.begin(), data.end()};
}

}  // namespace

TEST_CASE("a stored entry round-trips", "[io][zip]") {
    ZipBuilder builder;
    builder.add("pack.mcmeta", R"({"pack":{"pack_format":15}})", /*deflate=*/false);

    const auto archive = ZipArchive::open(builder.take());
    REQUIRE(archive.has_value());
    REQUIRE(archive->entry_count() == 1);
    REQUIRE(archive->contains("pack.mcmeta"));

    const auto content = archive->read("pack.mcmeta");
    REQUIRE(content.has_value());
    REQUIRE(as_text(*content) == R"({"pack":{"pack_format":15}})");
}

TEST_CASE("a deflated entry round-trips", "[io][zip]") {
    // Almost every entry in the client jar is deflated.
    const std::string model =
        R"({"parent":"minecraft:block/cube_all","textures":{"all":"minecraft:block/stone"}})"
        R"({"parent":"minecraft:block/cube_all","textures":{"all":"minecraft:block/stone"}})";

    ZipBuilder builder;
    builder.add("assets/minecraft/models/block/stone.json", model);

    const auto archive = ZipArchive::open(builder.take());
    REQUIRE(archive.has_value());

    const auto* entry = archive->find("assets/minecraft/models/block/stone.json");
    REQUIRE(entry != nullptr);
    REQUIRE(entry->method == 8);
    REQUIRE(entry->compressed_size < entry->uncompressed_size);

    const auto content = archive->read(entry->name);
    REQUIRE(content.has_value());
    REQUIRE(as_text(*content) == model);
}

TEST_CASE("many entries are all retrievable", "[io][zip]") {
    // The client jar holds around 10 000 entries; the index has to be exact,
    // not approximately right.
    ZipBuilder builder;
    for (int i = 0; i < 200; ++i) {
        builder.add("assets/minecraft/blockstates/block_" + std::to_string(i) + ".json",
                    "content number " + std::to_string(i), false);
    }

    const auto archive = ZipArchive::open(builder.take());
    REQUIRE(archive.has_value());
    REQUIRE(archive->entry_count() == 200);

    for (int i = 0; i < 200; ++i) {
        const auto name    = "assets/minecraft/blockstates/block_" + std::to_string(i) + ".json";
        const auto content = archive->read(name);
        REQUIRE(content.has_value());
        REQUIRE(as_text(*content) == "content number " + std::to_string(i));
    }
}

TEST_CASE("listing by prefix finds a subtree", "[io][zip]") {
    ZipBuilder builder;
    builder.add("assets/minecraft/blockstates/stone.json", "a", false);
    builder.add("assets/minecraft/blockstates/dirt.json", "b", false);
    builder.add("assets/minecraft/models/block/stone.json", "c", false);
    builder.add("pack.mcmeta", "d", false);

    const auto archive = ZipArchive::open(builder.take());
    REQUIRE(archive.has_value());

    REQUIRE(archive->list("assets/minecraft/blockstates/").size() == 2);
    REQUIRE(archive->list("assets/minecraft/").size() == 3);
    REQUIRE(archive->list("").size() == 4);
    REQUIRE(archive->list("nothing/").empty());
}

TEST_CASE("a missing entry is reported, not invented", "[io][zip]") {
    ZipBuilder builder;
    builder.add("a.txt", "a", false);
    const auto archive = ZipArchive::open(builder.take());
    REQUIRE(archive.has_value());

    REQUIRE_FALSE(archive->contains("b.txt"));
    REQUIRE(archive->find("b.txt") == nullptr);
    REQUIRE(archive->read("b.txt").error() == ZipError::NotFound);
}

TEST_CASE("an archive comment does not hide the trailer", "[io][zip]") {
    // The end-of-central-directory record has no fixed position: a comment of
    // up to 64 KiB may follow it, so it has to be found by scanning backwards.
    ZipBuilder builder;
    builder.add("a.txt", "hello", false);

    const auto archive = ZipArchive::open(builder.take("a comment that follows the trailer"));
    REQUIRE(archive.has_value());
    REQUIRE(as_text(archive->read("a.txt").value()) == "hello");
}

// ── Hostile archives ────────────────────────────────────────────────────────
// A resource pack is a file a player hands us. The interesting part of a ZIP
// reader is what it refuses.

TEST_CASE("path traversal is refused outright", "[io][zip][malformed]") {
    // Zip Slip: an entry named "../../.ssh/authorized_keys" writes wherever the
    // attacker likes. The archive is rejected as a whole rather than the entry
    // skipped — an archive carrying one is hostile, and the safest thing to do
    // with the rest of it is nothing.
    for (const char* hostile : {"../escape.txt", "a/../../escape.txt", "/etc/passwd",
                                "..\\windows\\system32", "C:/windows/system32"}) {
        ZipBuilder builder;
        builder.add_raw_name(hostile);
        const auto archive = ZipArchive::open(builder.take());
        REQUIRE_FALSE(archive.has_value());
        REQUIRE(archive.error() == ZipError::UnsafePath);
    }
}

TEST_CASE("safe paths are not rejected by the traversal check", "[io][zip]") {
    // A check that rejects legitimate names is as broken as one that accepts
    // hostile ones: "a..b" contains "..", and is perfectly safe.
    REQUIRE(is_safe_archive_path("assets/minecraft/models/block/stone.json"));
    REQUIRE(is_safe_archive_path("pack.mcmeta"));
    REQUIRE(is_safe_archive_path("a..b/file.json"));
    REQUIRE(is_safe_archive_path("...hidden"));

    REQUIRE_FALSE(is_safe_archive_path(".."));
    REQUIRE_FALSE(is_safe_archive_path("../x"));
    REQUIRE_FALSE(is_safe_archive_path("a/../../x"));
    REQUIRE_FALSE(is_safe_archive_path("/absolute"));
    REQUIRE_FALSE(is_safe_archive_path(""));
}

TEST_CASE("a non-archive is rejected", "[io][zip][malformed]") {
    REQUIRE(ZipArchive::open(std::vector<u8>{}).error() == ZipError::NotAnArchive);
    REQUIRE(ZipArchive::open(std::vector<u8>{1, 2, 3}).error() == ZipError::NotAnArchive);

    const std::string text = "this is a perfectly ordinary text file, not an archive at all";
    REQUIRE(ZipArchive::open(std::vector<u8>{text.begin(), text.end()}).error() ==
            ZipError::NotAnArchive);
}

TEST_CASE("a truncated archive is rejected", "[io][zip][malformed]") {
    ZipBuilder builder;
    builder.add("a.txt", "some content here", false);
    const auto full = builder.take();

    for (usize length = 1; length < full.size(); length += 5) {
        const std::vector<u8> partial{full.begin(), full.begin() + static_cast<isize>(length)};
        const auto            archive = ZipArchive::open(partial);
        // Whether it is refused at open or at read, it must not crash or read
        // past the buffer — which is what the sanitizer build is watching.
        if (archive.has_value()) {
            (void)archive->read("a.txt");
        }
    }
}

TEST_CASE("an entry size limit is enforced", "[io][zip][malformed]") {
    // A pack could declare a single 4 GiB texture. The cap has to bite before
    // the allocation, not after.
    ZipBuilder builder;
    builder.add("big.bin", std::string(200'000, 'x'));
    const auto archive = ZipArchive::open(builder.take());
    REQUIRE(archive.has_value());

    REQUIRE(archive->read("big.bin", 1024).error() == ZipError::TooLarge);
    REQUIRE(archive->read("big.bin", 1024 * 1024).has_value());
}

TEST_CASE("a corrupt central directory is rejected", "[io][zip][malformed]") {
    ZipBuilder builder;
    builder.add("a.txt", "content", false);
    auto data = builder.take();

    // Break the first central directory signature.
    const usize cd_start = data.size() - 22 - 46 - 5;
    data[cd_start] ^= 0xFF;

    const auto archive = ZipArchive::open(std::move(data));
    REQUIRE_FALSE(archive.has_value());
    REQUIRE(archive.error() == ZipError::Corrupt);
}

TEST_CASE("errors have readable names", "[io][zip]") {
    REQUIRE(to_string(ZipError::NotAnArchive) == "not a ZIP archive");
    REQUIRE(to_string(ZipError::UnsafePath) == "unsafe entry path");
    REQUIRE(to_string(ZipError::Unsupported) == "unsupported ZIP feature");
}
