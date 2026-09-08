// ov-inspect — read Minecraft's binary formats and say what is in them.
//
// The point is not pretty output. It is that every format Ondes VOXEL claims to
// support can be pointed at a real file from a real world and checked, by hand,
// today. `nbt --verify` in particular decodes a file, re-encodes it, and
// compares the bytes: that is the whole of "our NBT support is correct",
// reduced to one command someone can run on their own save.

#define OV_LOG_CATEGORY "inspect"

#include "ov/base/log.hpp"
#include "ov/gameplay/loot.hpp"
#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"
#include "ov/io/zip.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/nbt/region_writer.hpp"
#include "ov/nbt/tag.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/heightmap.hpp"
#include "ov/world/light_array.hpp"
#include "ov/world/paletted_container.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ov;

constexpr usize kMaxPreviewElements = 8;

std::string indent_of(int depth) {
    return std::string(static_cast<usize>(depth) * 2, ' ');
}

void print_tag(const nbt::Tag& tag, std::string_view name, int depth, int max_depth) {
    const std::string pad   = indent_of(depth);
    const std::string label = name.empty() ? std::string{} : fmt::format("{}: ", name);

    switch (tag.type()) {
        case nbt::TagType::Compound: {
            const auto& entries = *tag.compound();
            fmt::print("{}{}{{{} entries}}\n", pad, label, entries.size());
            if (depth >= max_depth) {
                return;
            }
            for (const auto& entry : entries) {
                print_tag(entry.value, entry.name, depth + 1, max_depth);
            }
            break;
        }

        case nbt::TagType::List: {
            const auto& items = *tag.list();
            fmt::print("{}{}[{} x {}]\n", pad, label, items.size(),
                       nbt::to_string(tag.list_element_type()));
            if (depth >= max_depth) {
                return;
            }
            for (usize i = 0; i < items.size() && i < kMaxPreviewElements; ++i) {
                print_tag(items[i], fmt::format("[{}]", i), depth + 1, max_depth);
            }
            if (items.size() > kMaxPreviewElements) {
                fmt::print("{}  ... {} more\n", pad, items.size() - kMaxPreviewElements);
            }
            break;
        }

        case nbt::TagType::String: fmt::print("{}{}\"{}\"\n", pad, label, tag.as_string()); break;

        case nbt::TagType::ByteArray: fmt::print("{}{}byte[{}]\n", pad, label, tag.size()); break;
        case nbt::TagType::IntArray: fmt::print("{}{}int[{}]\n", pad, label, tag.size()); break;
        case nbt::TagType::LongArray: fmt::print("{}{}long[{}]\n", pad, label, tag.size()); break;

        case nbt::TagType::Float:
        case nbt::TagType::Double: fmt::print("{}{}{}\n", pad, label, tag.as_f64()); break;

        case nbt::TagType::End: fmt::print("{}{}<end>\n", pad, label); break;

        default: fmt::print("{}{}{}\n", pad, label, tag.as_i64()); break;
    }
}

struct Stats {
    usize tags{0};
    usize compounds{0};
    usize lists{0};
    usize strings{0};
    usize max_depth{0};
};

void collect(const nbt::Tag& tag, usize depth, Stats& stats) {
    ++stats.tags;
    stats.max_depth = std::max(stats.max_depth, depth);

    switch (tag.type()) {
        case nbt::TagType::Compound:
            ++stats.compounds;
            for (const auto& entry : *tag.compound()) {
                collect(entry.value, depth + 1, stats);
            }
            break;
        case nbt::TagType::List:
            ++stats.lists;
            for (const auto& item : *tag.list()) {
                collect(item, depth + 1, stats);
            }
            break;
        case nbt::TagType::String: ++stats.strings; break;
        default: break;
    }
}

int inspect_nbt(const std::filesystem::path& path, bool tree, bool verify, int max_depth) {
    const auto raw = io::read_file(path);
    if (!raw) {
        fmt::print(stderr, "cannot read {}: {}\n", path.string(), io::to_string(raw.error()));
        return 1;
    }

    // level.dat is gzip; region chunks are zlib; structure files are gzip.
    // Sniff rather than trust the extension.
    std::vector<u8> data;
    std::string     container = "none";
    if (io::looks_like_gzip(*raw)) {
        container = "gzip";
    } else if (io::looks_like_zlib(*raw)) {
        container = "zlib";
    }

    if (container == "none") {
        data = *raw;
    } else {
        auto decompressed = io::decompress(*raw);
        if (!decompressed) {
            fmt::print(stderr, "{}: {}\n", path.string(), io::to_string(decompressed.error()));
            return 1;
        }
        data = std::move(*decompressed);
    }

    const auto document = nbt::read(data);
    if (!document) {
        fmt::print(stderr, "{}: {}\n", path.string(), nbt::to_string(document.error()));
        return 1;
    }

    Stats stats;
    collect(document->root, 0, stats);

    fmt::print("{}\n", path.filename().string());
    fmt::print("  container ...... {}\n", container);
    fmt::print("  on disk ........ {} bytes\n", raw->size());
    fmt::print("  decompressed ... {} bytes\n", data.size());
    fmt::print("  root name ...... \"{}\"\n", document->name);
    fmt::print("  tags ........... {} ({} compounds, {} lists, {} strings)\n", stats.tags,
               stats.compounds, stats.lists, stats.strings);
    fmt::print("  max depth ...... {}\n", stats.max_depth);

    // Chunks carry DataVersion at the root; level.dat nests it under "Data".
    // 3465 is 1.20.1 — worth printing, since a file from another version is the
    // most likely explanation for anything surprising below.
    const nbt::Tag* version = document->root.find("DataVersion");
    if (version == nullptr) {
        if (const auto* level_data = document->root.find("Data")) {
            version = level_data->find("DataVersion");
        }
    }
    if (version != nullptr) {
        fmt::print("  DataVersion .... {}{}\n", version->as_i64(),
                   version->as_i64() == 3465 ? "  (1.20.1)" : "  (not 1.20.1)");
    }

    if (verify) {
        // The whole claim, in one comparison: decode then re-encode has to
        // reproduce the input byte for byte. Anything less and "our save opens
        // in vanilla" is a hope rather than a fact.
        const auto re_encoded = nbt::write(*document);
        if (re_encoded == data) {
            fmt::print("  round-trip ..... \033[0;32mbyte-identical\033[0m ({} bytes)\n",
                       re_encoded.size());
        } else {
            fmt::print("  round-trip ..... \033[0;31mMISMATCH\033[0m in {} bytes vs {} out\n",
                       data.size(), re_encoded.size());
            usize first_diff = 0;
            while (first_diff < std::min(data.size(), re_encoded.size()) &&
                   data[first_diff] == re_encoded[first_diff]) {
                ++first_diff;
            }
            fmt::print("                   first difference at offset {}\n", first_diff);
            return 1;
        }
    }

    if (tree) {
        fmt::print("\n");
        print_tag(document->root, document->name, 0, max_depth);
    }

    return 0;
}

/// Rebuild a region file from its own chunks and compare.
///
/// Reading a region proves we can parse one; this proves we can produce one.
/// Every chunk of the rebuilt file has to decompress to the same bytes as the
/// original, or a save is quietly lossy — and a lossy save is discovered by a
/// player, weeks later, on a world they cannot get back.
[[nodiscard]] int verify_region_rewrite(const std::filesystem::path& path,
                                        const nbt::RegionFile&       original) {
    auto writer = nbt::RegionWriter::open_or_empty(path);

    const auto rebuilt = nbt::RegionFile::open(writer.build());
    if (!rebuilt) {
        fmt::print("  rewrite ........ \033[0;31mrebuilt file does not parse\033[0m\n");
        return 1;
    }

    usize compared  = 0;
    usize identical = 0;
    for (u32 z = 0; z < nbt::kRegionSideChunks; ++z) {
        for (u32 x = 0; x < nbt::kRegionSideChunks; ++x) {
            if (!original.has_chunk(x, z)) {
                continue;
            }
            ++compared;
            const auto before = original.read_chunk_bytes(x, z);
            const auto after  = rebuilt->read_chunk_bytes(x, z);
            if (before && after && *before == *after) {
                ++identical;
            }
        }
    }

    fmt::print("  rewrite ........ {}{}/{} chunks byte-identical\033[0m\n",
               identical == compared ? "\033[0;32m" : "\033[0;31m", identical, compared);
    return identical == compared ? 0 : 1;
}

int inspect_region(const std::filesystem::path& path, bool verify) {
    const auto region = nbt::RegionFile::open(path);
    if (!region) {
        fmt::print(stderr, "{}: {}\n", path.string(), nbt::to_string(region.error()));
        return 1;
    }

    fmt::print("{}\n", path.filename().string());
    fmt::print("  size ........... {} bytes ({} sectors)\n", region->byte_size(),
               region->byte_size() / nbt::kSectorSize);
    fmt::print("  chunks present . {} / {}\n", region->chunk_count(), nbt::kRegionChunkCount);

    usize gzip = 0;
    usize zlib = 0;
    usize none = 0;
    for (u32 z = 0; z < nbt::kRegionSideChunks; ++z) {
        for (u32 x = 0; x < nbt::kRegionSideChunks; ++x) {
            if (const auto scheme = region->chunk_compression(x, z)) {
                switch (*scheme) {
                    case nbt::ChunkCompression::Gzip: ++gzip; break;
                    case nbt::ChunkCompression::Zlib: ++zlib; break;
                    case nbt::ChunkCompression::None: ++none; break;
                }
            }
        }
    }
    fmt::print("  compression .... zlib {}, gzip {}, none {}\n", zlib, gzip, none);

    if (!verify) {
        return 0;
    }

    // Decode every chunk in the region, re-encode it, and compare the bytes.
    // Doing it across a whole region rather than one hand-picked chunk is what
    // makes this evidence rather than an anecdote.
    usize checked    = 0;
    usize identical  = 0;
    usize failed     = 0;
    usize total_tags = 0;

    for (u32 z = 0; z < nbt::kRegionSideChunks; ++z) {
        for (u32 x = 0; x < nbt::kRegionSideChunks; ++x) {
            if (!region->has_chunk(x, z)) {
                continue;
            }
            ++checked;

            const auto bytes = region->read_chunk_bytes(x, z);
            if (!bytes) {
                ++failed;
                fmt::print("    chunk ({},{}) unreadable: {}\n", x, z,
                           nbt::to_string(bytes.error()));
                continue;
            }

            const auto document = nbt::read(*bytes);
            if (!document) {
                ++failed;
                fmt::print("    chunk ({},{}) bad NBT: {}\n", x, z,
                           nbt::to_string(document.error()));
                continue;
            }

            Stats stats;
            collect(document->root, 0, stats);
            total_tags += stats.tags;

            if (nbt::write(*document) == *bytes) {
                ++identical;
            } else {
                ++failed;
                fmt::print("    chunk ({},{}) round-trip MISMATCH\n", x, z);
            }
        }
    }

    fmt::print("  round-trip ..... {}{}/{} chunks byte-identical\033[0m ({} tags)\n",
               failed == 0 ? "\033[0;32m" : "\033[0;31m", identical, checked, total_tags);

    // Reading proves we can parse a region. This proves we can produce one,
    // which is the half a save depends on.
    const int rewrite = verify_region_rewrite(path, *region);
    return failed == 0 && rewrite == 0 ? 0 : 1;
}

int inspect_zip(const std::filesystem::path& path, bool verify) {
    const auto archive = io::ZipArchive::open(path);
    if (!archive) {
        fmt::print(stderr, "{}: {}\n", path.string(), io::to_string(archive.error()));
        return 1;
    }

    usize stored             = 0;
    usize deflated           = 0;
    usize directories        = 0;
    u64   uncompressed_total = 0;
    for (const auto& entry : archive->entries()) {
        if (entry.is_directory()) {
            ++directories;
            continue;
        }
        (entry.method == 8 ? deflated : stored)++;
        uncompressed_total += entry.uncompressed_size;
    }

    fmt::print("{}\n", path.filename().string());
    fmt::print("  size ........... {} bytes\n", archive->byte_size());
    fmt::print("  entries ........ {} ({} deflated, {} stored, {} directories)\n",
               archive->entry_count(), deflated, stored, directories);
    fmt::print("  uncompressed ... {} bytes\n", uncompressed_total);

    // The subtrees that matter to ov-assetimport. A resource pack carries only
    // textures; models, blockstates, fonts and language files come from the jar.
    for (const char* prefix :
         {"assets/minecraft/blockstates/", "assets/minecraft/models/block/",
          "assets/minecraft/models/item/", "assets/minecraft/textures/block/",
          "assets/minecraft/font/", "assets/minecraft/lang/", "data/minecraft/"}) {
        const auto found = archive->list(prefix);
        if (!found.empty()) {
            fmt::print("    {:38s} {}\n", prefix, found.size());
        }
    }

    if (!verify) {
        return 0;
    }

    // Extract everything. On a 23 MB jar that is some 10 000 entries, which is
    // a far better exercise of the reader than any archive we could build by
    // hand in a test.
    usize extracted = 0;
    usize failed    = 0;
    u64   bytes     = 0;
    for (const auto& entry : archive->entries()) {
        if (entry.is_directory()) {
            continue;
        }
        const auto content = archive->read(entry.name);
        if (!content) {
            ++failed;
            if (failed <= 5) {
                fmt::print("    {} failed: {}\n", entry.name, io::to_string(content.error()));
            }
            continue;
        }
        if (content->size() != entry.uncompressed_size) {
            ++failed;
            continue;
        }
        ++extracted;
        bytes += content->size();
    }

    fmt::print("  extraction ..... {}{}/{} entries, {} bytes\033[0m\n",
               failed == 0 ? "\033[0;32m" : "\033[0;31m", extracted, extracted + failed, bytes);
    return failed == 0 ? 0 : 1;
}

/// Verify the bit-packing against sections Mojang actually wrote.
///
/// Unpacking every entry of a real section and packing it back has to reproduce
/// the original longs exactly. This is the strongest available check on the
/// "an entry never spans two longs" rule, because the input was produced by the
/// game rather than by us — a round trip through our own encoder alone would
/// pass even with the rule inverted.
///
/// It does not resolve block names, so it is independent of the world's version.
int inspect_chunk_packing(const std::filesystem::path& path) {
    const auto region = nbt::RegionFile::open(path);
    if (!region) {
        fmt::print(stderr, "{}: {}\n", path.string(), nbt::to_string(region.error()));
        return 1;
    }

    usize               sections  = 0;
    usize               identical = 0;
    usize               skipped   = 0;
    std::map<u8, usize> by_width;

    // Light nibble order, settled by measurement rather than by argument.
    //
    // 2048 bytes hold 4096 cells, and the format says which half of a byte the
    // even cell lives in. Reading it backwards swaps every pair of neighbours
    // along x. No round trip detects that — the bytes still come back identical
    // — and the result is a world lit in a fine checkerboard, which reads as a
    // shader bug rather than a storage one.
    //
    // Real lighting varies smoothly, so the correct reading is the one giving
    // the smaller step between neighbouring cells. This measures both on arrays
    // the game itself wrote.
    // Heightmap semantics, checked rather than read off a wiki page.
    //
    // WORLD_SURFACE is documented as "the highest non-air block", but the value
    // stored is an offset, and whether it is that block's y or the free space
    // above it is exactly the kind of off-by-one that produces a world where
    // rain falls one block into the ground.
    //
    // So it is recomputed here from the block palettes — by NAME, so the check
    // is independent of which version's ids the world uses — and compared
    // against what the game wrote.
    usize columns_checked       = 0;
    usize columns_matched       = 0;
    usize columns_off_by_one    = 0;
    usize columns_reported      = 0;
    usize columns_replay_differ = 0;

    usize light_arrays        = 0;
    usize low_nibble_smoother = 0;
    f64   low_total           = 0.0;
    f64   high_total          = 0.0;

    for (u32 z = 0; z < nbt::kRegionSideChunks; ++z) {
        for (u32 x = 0; x < nbt::kRegionSideChunks; ++x) {
            if (!region->has_chunk(x, z)) {
                continue;
            }
            const auto chunk = region->read_chunk(x, z);
            if (!chunk) {
                continue;
            }
            const nbt::Tag* section_list = chunk->root.find("sections");
            if (section_list == nullptr || section_list->list() == nullptr) {
                continue;
            }

            // ── Recompute WORLD_SURFACE from the blocks ─────────────────────
            const nbt::Tag* heightmaps = chunk->root.find("Heightmaps");
            const nbt::Tag* surface =
                heightmaps == nullptr ? nullptr : heightmaps->find("WORLD_SURFACE");
            const auto* surface_longs =
                surface == nullptr ? nullptr : surface->get_if<nbt::Tag::LongArray>();

            if (surface_longs != nullptr && surface_longs->size() == 37) {
                constexpr std::string_view kAirNames[] = {"minecraft:air", "minecraft:cave_air",
                                                          "minecraft:void_air"};
                std::array<i32, 256>       top{};
                top.fill(std::numeric_limits<i32>::min());
                std::array<std::string_view, 256> top_name{};

                // Replayed through the real Chunk type, one set_block at a time,
                // so what is checked is the incremental heightmap maintenance
                // rather than a formula written out twice. Only air-ness matters
                // here, so every solid block becomes the same placeholder state.
                constexpr registry::BlockStateId kAirId{0};
                constexpr registry::BlockStateId kSolidId{1};
                world::Chunk replay{ChunkPos{static_cast<i32>(x), static_cast<i32>(z)},
                                    world::WorldShape::overworld(),
                                    world::AirStates{kAirId, kAirId, kAirId},
                                    // WORLD_SURFACE only: this replay compares
                                    // against the stored WORLD_SURFACE alone.
                                    nullptr};

                // The heightmap's origin is the *dimension's* floor, not the
                // lowest section the file happens to list. Measured: two chunks
                // in this world list 25 sections starting at -5 rather than 24
                // starting at -4, because vanilla writes an extra section below
                // the world for lighting. Deriving the origin from the section
                // list shifts every column in those chunks by 16.
                constexpr i32 kOverworldMinY = -64;

                for (const nbt::Tag& section : *section_list->list()) {
                    const nbt::Tag* y_tag = section.find("Y");
                    if (y_tag == nullptr) {
                        continue;
                    }
                    const auto section_y = static_cast<i32>(y_tag->as_i64());

                    const nbt::Tag* states = section.find("block_states");
                    if (states == nullptr) {
                        continue;
                    }
                    const nbt::Tag* palette = states->find("palette");
                    if (palette == nullptr || palette->list() == nullptr) {
                        continue;
                    }

                    std::vector<bool>             palette_is_air;
                    std::vector<std::string_view> palette_names;
                    palette_is_air.reserve(palette->list()->size());
                    for (const nbt::Tag& entry : *palette->list()) {
                        const nbt::Tag*        name = entry.find("Name");
                        const std::string_view text =
                            name == nullptr ? std::string_view{} : name->as_string();
                        palette_names.push_back(text);
                        palette_is_air.push_back(std::ranges::find(kAirNames, text) !=
                                                 std::end(kAirNames));
                    }

                    const i32       base = section_y * 16;
                    const nbt::Tag* data = states->find("data");
                    const auto*     longs =
                        data == nullptr ? nullptr : data->get_if<nbt::Tag::LongArray>();

                    if (longs == nullptr || longs->empty()) {
                        // Single-valued: either the whole section is air, or
                        // all of it is solid up to its top.
                        if (!palette_is_air.empty() && !palette_is_air[0]) {
                            for (usize c = 0; c < 256; ++c) {
                                if (base + 15 > top[c]) {
                                    top[c]      = base + 15;
                                    top_name[c] = palette_names[0];
                                }
                            }
                            for (i32 dy = 0; dy < 16; ++dy) {
                                for (usize c = 0; c < 256; ++c) {
                                    replay.set_block(c % 16, base + dy, c / 16, kSolidId);
                                }
                            }
                        }
                        continue;
                    }

                    const u8  bits = world::bits_for_palette(palette_is_air.size());
                    const u32 per  = world::entries_per_long(bits);
                    const u64 mask = (u64{1} << bits) - 1;
                    for (usize index = 0; index < 4096; ++index) {
                        const usize word = index / per;
                        if (word >= longs->size()) {
                            break;
                        }
                        const auto slot = static_cast<usize>(
                            (static_cast<u64>((*longs)[word]) >> ((index % per) * bits)) & mask);
                        if (slot >= palette_is_air.size() || palette_is_air[slot]) {
                            continue;
                        }
                        const i32 y = base + static_cast<i32>(index / 256);
                        if (y > top[index % 256]) {
                            top[index % 256]      = y;
                            top_name[index % 256] = palette_names[slot];
                        }
                        replay.set_block(index % 16, y, (index % 256) / 16, kSolidId);
                    }
                }

                const i32 min_y           = kOverworldMinY;
                usize     mismatched_here = 0;
                for (usize column = 0; column < 256; ++column) {
                    // 256 values of 9 bits, seven to a long — the same
                    // never-span rule the block palettes use.
                    const usize word   = column / 7;
                    const auto  stored = static_cast<i32>(
                        (static_cast<u64>((*surface_longs)[word]) >> ((column % 7) * 9)) & 0x1FF);

                    const i32 expected = top[column] == std::numeric_limits<i32>::min()
                                             ? 0
                                             : top[column] + 1 - min_y;
                    // The Chunk's own maintained heightmap has to agree with
                    // the recomputation as well; if it does not, the
                    // incremental path has drifted.
                    const i32 replayed = replay.heightmap(world::HeightmapType::WorldSurface)
                                             .first_free(column % 16, column / 16) -
                                         world::WorldShape::overworld().min_y;
                    if (replayed != expected) {
                        ++columns_replay_differ;
                    }

                    ++columns_checked;
                    if (stored == expected) {
                        ++columns_matched;
                    } else {
                        ++mismatched_here;
                        if (stored == expected - 1) {
                            ++columns_off_by_one;
                        }
                        // Capped: a world that disagrees everywhere would
                        // otherwise bury the summary under its own noise.
                        if (columns_reported < 8) {
                            ++columns_reported;
                            fmt::print(
                                "      column ({},{}) of chunk ({},{}): stored {}, "
                                "recomputed {} (top block y {} is {})\n",
                                column % 16, column / 16, x, z, stored, expected,
                                top[column] == std::numeric_limits<i32>::min() ? -9999
                                                                               : top[column],
                                top_name[column]);
                        }
                    }
                }
                if (mismatched_here > 0) {
                    fmt::print("    chunk ({},{}): {} columns differ, sections listed {}\n", x, z,
                               mismatched_here, section_list->list()->size());
                }
            }

            for (const nbt::Tag& section : *section_list->list()) {
                for (const char* which : {"BlockLight", "SkyLight"}) {
                    const nbt::Tag* tag = section.find(which);
                    if (tag == nullptr) {
                        continue;
                    }
                    const auto* bytes = tag->get_if<nbt::Tag::ByteArray>();
                    if (bytes == nullptr || bytes->size() != world::kLightByteCount) {
                        continue;
                    }
                    ++light_arrays;

                    // Total step between cells adjacent along x, under each
                    // reading. Row ends are skipped: those neighbours are not
                    // adjacent in the world.
                    f64   low   = 0.0;
                    f64   high  = 0.0;
                    usize pairs = 0;
                    for (usize i = 0; i + 1 < world::kLightCellCount; ++i) {
                        if ((i % 16) == 15) {
                            continue;
                        }
                        auto cell = [&](usize k, bool swapped) {
                            const auto byte = static_cast<u8>((*bytes)[k >> 1]);
                            const u8   lo   = byte & 0xF;
                            const u8   hi   = static_cast<u8>(byte >> 4);
                            const bool even = (k & 1) == 0;
                            return swapped ? (even ? hi : lo) : (even ? lo : hi);
                        };
                        low += std::abs(static_cast<f64>(cell(i, false)) -
                                        static_cast<f64>(cell(i + 1, false)));
                        high += std::abs(static_cast<f64>(cell(i, true)) -
                                         static_cast<f64>(cell(i + 1, true)));
                        ++pairs;
                    }
                    if (pairs == 0) {
                        continue;
                    }
                    low /= static_cast<f64>(pairs);
                    high /= static_cast<f64>(pairs);
                    low_total += low;
                    high_total += high;
                    if (low <= high) {
                        ++low_nibble_smoother;
                    }
                }

                const nbt::Tag* states = section.find("block_states");
                if (states == nullptr) {
                    continue;
                }
                const nbt::Tag* palette = states->find("palette");
                const nbt::Tag* data    = states->find("data");
                if (palette == nullptr || palette->list() == nullptr) {
                    continue;
                }
                ++sections;

                const usize palette_size = palette->list()->size();
                if (data == nullptr) {
                    // Single-valued: vanilla omits the data entirely, which is
                    // the case an implementation forgets.
                    if (palette_size == 1) {
                        ++identical;
                    } else {
                        ++skipped;
                    }
                    continue;
                }

                const auto* longs = data->get_if<nbt::Tag::LongArray>();
                if (longs == nullptr || longs->empty()) {
                    ++skipped;
                    continue;
                }

                // Derive the width the way the format defines it, from the
                // palette size, and check the long count agrees.
                const u8 bits = world::bits_for_palette(palette_size);
                if (world::packed_length(4096, bits) != longs->size()) {
                    fmt::print("    chunk ({},{}): {} longs, expected {} at {} bits\n", x, z,
                               longs->size(), world::packed_length(4096, bits), bits);
                    ++skipped;
                    continue;
                }
                ++by_width[bits];

                std::vector<u64> as_unsigned(longs->begin(), longs->end());
                std::vector<u16> indices(palette_size);
                for (u16 i = 0; i < palette_size; ++i) {
                    indices[i] = i;
                }

                auto container = world::PalettedContainer::blocks(0);
                if (!container.load_packed(bits, indices, as_unsigned)) {
                    ++skipped;
                    continue;
                }

                // Re-pack from scratch and compare against what was on disk.
                std::vector<u16> values(4096);
                for (usize i = 0; i < 4096; ++i) {
                    values[i] = container.get(i);
                }
                auto rebuilt = world::PalettedContainer::blocks(0);
                rebuilt.assign(values);

                const bool same = rebuilt.bits() == bits &&
                                  std::equal(rebuilt.data().begin(), rebuilt.data().end(),
                                             as_unsigned.begin(), as_unsigned.end());
                if (same) {
                    ++identical;
                } else if (rebuilt.bits() != bits) {
                    // Legitimate: vanilla may keep a wider palette than the
                    // contents now need, since it never shrinks either.
                    ++skipped;
                } else {
                    fmt::print("    chunk ({},{}): repacked longs differ at {} bits\n", x, z, bits);
                }
            }
        }
    }

    fmt::print("{}\n", path.filename().string());
    fmt::print("  sections ....... {}\n", sections);
    fmt::print("  widths ......... ");
    for (const auto& [bits, count] : by_width) {
        fmt::print("{}b:{} ", bits, count);
    }
    fmt::print("\n");
    if (columns_checked > 0) {
        fmt::print(
            "  WORLD_SURFACE .. {}{}/{} columns\033[0m recomputed from the blocks"
            " ({} off by one)\n",
            columns_matched == columns_checked ? "\033[0;32m" : "\033[0;31m", columns_matched,
            columns_checked, columns_off_by_one);
        fmt::print("  Chunk replay ... {}{}/{} columns\033[0m maintained incrementally\n",
                   columns_replay_differ == 0 ? "\033[0;32m" : "\033[0;31m",
                   columns_checked - columns_replay_differ, columns_checked);
    }
    if (light_arrays > 0) {
        const f64  low       = low_total / static_cast<f64>(light_arrays);
        const f64  high      = high_total / static_cast<f64>(light_arrays);
        const bool ours_wins = low_nibble_smoother * 2 > light_arrays;
        fmt::print("  light arrays ... {}\n", light_arrays);
        fmt::print(
            "  nibble order ... {}even cell = low nibble\033[0m on {}/{} "
            "(step {:.3f} vs {:.3f})\n",
            ours_wins ? "\033[0;32m" : "\033[0;31m", low_nibble_smoother, light_arrays, low, high);
    }
    fmt::print("  re-packed ...... {}{}/{} identical\033[0m ({} not comparable)\n",
               identical + skipped == sections ? "\033[0;32m" : "\033[0;31m", identical,
               sections - skipped, skipped);

    return identical + skipped == sections ? 0 : 1;
}

/// Report the block and its stored light at given positions.
///
/// Reads "x y z" lines from standard input and prints one line per position.
/// It exists so that measuring the game's own behaviour — place a block on a
/// real server, save, read back what it decided — can use the NBT reader that
/// has been checked against 17879 chunks, rather than a throwaway parser
/// written for the occasion. I wrote three of those before admitting they were
/// the least reliable part of the experiment.
int inspect_light(const std::filesystem::path& directory) {
    std::map<std::pair<i32, i32>, std::optional<nbt::RegionFile>> regions;

    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    while (std::cin >> x >> y >> z) {
        const i32  chunk_x    = x >> 4;
        const i32  chunk_z    = z >> 4;
        const auto region_key = std::pair{chunk_x >> 5, chunk_z >> 5};

        if (!regions.contains(region_key)) {
            const auto path =
                directory / fmt::format("r.{}.{}.mca", region_key.first, region_key.second);
            auto opened = nbt::RegionFile::open(path);
            regions.emplace(region_key, opened ? std::optional{std::move(*opened)} : std::nullopt);
        }
        const auto& region = regions.at(region_key);
        if (!region) {
            fmt::print("{} {} {} - -\n", x, y, z);
            continue;
        }

        const auto chunk =
            region->read_chunk(static_cast<u32>(chunk_x & 31), static_cast<u32>(chunk_z & 31));
        if (!chunk) {
            fmt::print("{} {} {} - -\n", x, y, z);
            continue;
        }

        const nbt::Tag* sections = chunk->root.find("sections");
        if (sections == nullptr || sections->list() == nullptr) {
            fmt::print("{} {} {} - -\n", x, y, z);
            continue;
        }

        std::string_view name  = "-";
        int              light = -1;
        int              sky   = -1;
        for (const nbt::Tag& section : *sections->list()) {
            const nbt::Tag* section_y = section.find("Y");
            if (section_y == nullptr || section_y->as_i64() != (y >> 4)) {
                continue;
            }
            const usize index =
                ((static_cast<usize>(y & 15) * 16) + static_cast<usize>(z & 15)) * 16 +
                static_cast<usize>(x & 15);

            // Both arrays: block light is what a block emits, sky light is what
            // reaches it, and the two answer different questions. An absent
            // array means uniformly zero — a value, not a gap.
            const auto nibble = [&](const char* key, int& out) {
                const nbt::Tag* tag = section.find(key);
                if (tag == nullptr) {
                    out = 0;
                    return;
                }
                if (const auto* bytes = tag->get_if<nbt::Tag::ByteArray>();
                    bytes != nullptr && bytes->size() == world::kLightByteCount) {
                    const auto byte = static_cast<u8>((*bytes)[index >> 1]);
                    out             = (index & 1) == 0 ? (byte & 0xF) : (byte >> 4);
                }
            };
            nibble("BlockLight", light);
            nibble("SkyLight", sky);

            if (const nbt::Tag* states = section.find("block_states")) {
                const nbt::Tag* palette = states->find("palette");
                if (palette != nullptr && palette->list() != nullptr && !palette->list()->empty()) {
                    usize           slot = 0;
                    const nbt::Tag* data = states->find("data");
                    const auto*     longs =
                        data == nullptr ? nullptr : data->get_if<nbt::Tag::LongArray>();
                    if (longs != nullptr && !longs->empty()) {
                        const u8   bits = world::bits_for_palette(palette->list()->size());
                        const u32  per  = world::entries_per_long(bits);
                        const auto word = static_cast<u64>((*longs)[index / per]);
                        slot            = static_cast<usize>((word >> ((index % per) * bits)) &
                                                             ((u64{1} << bits) - 1));
                    }
                    if (slot < palette->list()->size()) {
                        if (const nbt::Tag* tag = (*palette->list())[slot].find("Name")) {
                            name = tag->as_string();
                        }
                    }
                }
            }
            break;
        }
        fmt::print("{} {} {} {} {} {}\n", x, y, z, name, light, sky);
    }
    return 0;
}

/// Report the full block state at given positions: name and every property.
///
/// `column` prints the name alone, which is enough to tell whether a block is
/// there and useless for telling *which* stair it is. Placement conventions —
/// which way a slab faces, what shape a stair takes, which sides a fence
/// connects on — live entirely in the properties.
int inspect_states(const std::filesystem::path& directory) {
    std::map<std::pair<i32, i32>, std::optional<nbt::RegionFile>> regions;

    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    while (std::cin >> x >> y >> z) {
        const i32  chunk_x    = x >> 4;
        const i32  chunk_z    = z >> 4;
        const auto region_key = std::pair{chunk_x >> 5, chunk_z >> 5};

        if (!regions.contains(region_key)) {
            const auto path =
                directory / fmt::format("r.{}.{}.mca", region_key.first, region_key.second);
            auto opened = nbt::RegionFile::open(path);
            regions.emplace(region_key, opened ? std::optional{std::move(*opened)} : std::nullopt);
        }
        const auto& region = regions.at(region_key);
        const auto  chunk  = region ? region->read_chunk(static_cast<u32>(chunk_x & 31),
                                                         static_cast<u32>(chunk_z & 31))
                                    : decltype(region->read_chunk(0, 0)){};
        if (!region || !chunk) {
            fmt::print("{} {} {} -\n", x, y, z);
            continue;
        }

        const nbt::Tag* sections = chunk->root.find("sections");
        if (sections == nullptr || sections->list() == nullptr) {
            fmt::print("{} {} {} -\n", x, y, z);
            continue;
        }

        std::string description = "-";
        for (const nbt::Tag& section : *sections->list()) {
            const nbt::Tag* section_y = section.find("Y");
            if (section_y == nullptr || section_y->as_i64() != (y >> 4)) {
                continue;
            }
            const nbt::Tag* states = section.find("block_states");
            if (states == nullptr) {
                break;
            }
            const nbt::Tag* palette = states->find("palette");
            if (palette == nullptr || palette->list() == nullptr || palette->list()->empty()) {
                break;
            }
            const usize index =
                ((static_cast<usize>(y & 15) * 16) + static_cast<usize>(z & 15)) * 16 +
                static_cast<usize>(x & 15);
            usize           slot  = 0;
            const nbt::Tag* data  = states->find("data");
            const auto*     longs = data == nullptr ? nullptr : data->get_if<nbt::Tag::LongArray>();
            if (longs != nullptr && !longs->empty()) {
                const u8   bits = world::bits_for_palette(palette->list()->size());
                const u32  per  = world::entries_per_long(bits);
                const auto word = static_cast<u64>((*longs)[index / per]);
                slot =
                    static_cast<usize>((word >> ((index % per) * bits)) & ((u64{1} << bits) - 1));
            }
            if (slot >= palette->list()->size()) {
                break;
            }
            const nbt::Tag& entry = (*palette->list())[slot];
            const nbt::Tag* name  = entry.find("Name");
            if (name == nullptr) {
                break;
            }
            description = std::string{name->as_string()};
            if (const nbt::Tag* properties = entry.find("Properties");
                properties != nullptr && properties->compound() != nullptr) {
                // Sorted, because the palette's order is whatever the writer
                // used and two identical states must compare equal as text.
                std::vector<std::string> pairs;
                for (const nbt::CompoundEntry& property : *properties->compound()) {
                    pairs.push_back(
                        fmt::format("{}={}", property.name, property.value.as_string()));
                }
                std::ranges::sort(pairs);
                description += "[";
                for (usize i = 0; i < pairs.size(); ++i) {
                    description += (i == 0 ? "" : ",") + pairs[i];
                }
                description += "]";
            }
            break;
        }
        fmt::print("{} {} {} {}\n", x, y, z, description);
    }
    return 0;
}

/// Recompute the four heightmaps from the blocks and compare with what the game
/// wrote.
///
/// This is the check that the measured motion flags are right. Vanilla stored
/// its own answer in every chunk it ever saved; ours is derived from a table
/// built by putting each block on a column and reading what the game decided.
/// If the table is wrong anywhere, a real world will disagree here.
///
/// Only names and properties are needed, so the registry is used for its flags
/// rather than for state ids: `waterlogged` comes straight out of the palette
/// entry, which is what makes MOTION_BLOCKING separable from OCEAN_FLOOR.
int inspect_heightmaps(const std::filesystem::path& directory,
                       const std::filesystem::path& pack_path) {
    auto pack = registry::BlockRegistry::load(pack_path);
    if (!pack) {
        fmt::print(stderr, "cannot read {}: {}\n", pack_path.string(),
                   registry::to_string(pack.error()));
        return 1;
    }
    const registry::BlockRegistry& blocks = *pack;

    // What each palette entry contributes, resolved once per section rather
    // than per block: a section has up to a few dozen entries and 4096 cells.
    struct Contribution {
        bool surface{false};
        bool motion{false};
        bool no_leaves{false};
        bool floor{false};
        /// A block this version does not have — a modded one. Its column is
        /// skipped rather than counted wrong: we have no answer for it, and
        /// pretending it contributes nothing would be an answer.
        bool unknown{false};
    };

    constexpr i32        kMinY   = -64;
    constexpr u32        kHeight = 384;
    constexpr i32        kMaxY   = kMinY + static_cast<i32>(kHeight) - 1;
    constexpr std::array kNames  = std::to_array<std::string_view>(
        {"WORLD_SURFACE", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES", "OCEAN_FLOOR"});

    usize                      chunks         = 0;
    usize                      unknown_blocks = 0;
    usize                      skipped        = 0;
    std::array<usize, 4>       compared{};
    std::array<usize, 4>       matched{};
    std::array<std::string, 4> first_mismatch{};

    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (entry.path().extension() != ".mca") {
            continue;
        }
        auto region = nbt::RegionFile::open(entry.path());
        if (!region) {
            continue;
        }
        for (u32 local_z = 0; local_z < 32; ++local_z) {
            for (u32 local_x = 0; local_x < 32; ++local_x) {
                const auto chunk = region->read_chunk(local_x, local_z);
                if (!chunk) {
                    continue;
                }
                const nbt::Tag* sections = chunk->root.find("sections");
                const nbt::Tag* maps     = chunk->root.find("Heightmaps");
                if (sections == nullptr || sections->list() == nullptr || maps == nullptr) {
                    continue;
                }
                ++chunks;

                // Highest y in each column that counts, per heightmap.
                std::array<std::array<i32, world::kColumnCount>, 4> top{};
                for (auto& map : top) {
                    map.fill(kMinY - 1);
                }
                std::array<bool, world::kColumnCount> tainted{};

                for (const nbt::Tag& section : *sections->list()) {
                    const nbt::Tag* section_y = section.find("Y");
                    const nbt::Tag* states    = section.find("block_states");
                    if (section_y == nullptr || states == nullptr) {
                        continue;
                    }
                    const nbt::Tag* palette = states->find("palette");
                    if (palette == nullptr || palette->list() == nullptr ||
                        palette->list()->empty()) {
                        continue;
                    }
                    const i32 base_y = static_cast<i32>(section_y->as_i64()) * 16;
                    if (base_y < kMinY || base_y > kMaxY) {
                        continue;  // the extra section vanilla writes for lighting
                    }

                    std::vector<Contribution> contributions;
                    contributions.reserve(palette->list()->size());
                    for (const nbt::Tag& item : *palette->list()) {
                        const nbt::Tag* name_tag = item.find("Name");
                        Contribution    c;
                        if (name_tag != nullptr) {
                            const std::string_view name = name_tag->as_string();
                            const auto             id   = blocks.find_block(name);
                            if (!id) {
                                ++unknown_blocks;
                                c.unknown = true;
                            } else {
                                bool waterlogged = false;
                                if (const nbt::Tag* props = item.find("Properties")) {
                                    if (const nbt::Tag* w = props->find("waterlogged")) {
                                        waterlogged = w->as_string() == "true";
                                    }
                                }
                                const bool solid = blocks.blocks_motion(*id);
                                const bool fluid =
                                    waterlogged || blocks.holds_fluid(blocks.default_state(*id));
                                c.surface   = !blocks.is_air(*id);
                                c.floor     = solid;
                                c.motion    = solid || fluid;
                                c.no_leaves = c.motion && !blocks.is_leaves(*id);
                            }
                        }
                        contributions.push_back(c);
                    }

                    const nbt::Tag* data = states->find("data");
                    const auto*     longs =
                        data == nullptr ? nullptr : data->get_if<nbt::Tag::LongArray>();
                    const u8  bits = world::bits_for_palette(palette->list()->size());
                    const u32 per  = world::entries_per_long(bits);

                    for (usize index = 0; index < 4096; ++index) {
                        usize slot = 0;
                        if (longs != nullptr && !longs->empty()) {
                            const auto word = static_cast<u64>((*longs)[index / per]);
                            slot            = static_cast<usize>((word >> ((index % per) * bits)) &
                                                                 ((u64{1} << bits) - 1));
                        }
                        if (slot >= contributions.size()) {
                            continue;
                        }
                        const Contribution& c         = contributions[slot];
                        const usize         column_of = index % 256;
                        if (c.unknown) {
                            tainted[column_of] = true;
                            continue;
                        }
                        if (!c.surface) {
                            continue;  // air contributes to nothing
                        }
                        const i32                 y      = base_y + static_cast<i32>(index / 256);
                        const usize               column = index % 256;
                        const std::array<bool, 4> counts{c.surface, c.motion, c.no_leaves, c.floor};
                        for (usize which = 0; which < 4; ++which) {
                            if (counts[which] && y > top[which][column]) {
                                top[which][column] = y;
                            }
                        }
                    }
                }

                for (usize which = 0; which < 4; ++which) {
                    const nbt::Tag* tag = maps->find(kNames[which]);
                    const auto*     longs =
                        tag == nullptr ? nullptr : tag->get_if<nbt::Tag::LongArray>();
                    if (longs == nullptr) {
                        continue;
                    }
                    world::Heightmap stored{kMinY, kHeight};
                    std::vector<u64> words(longs->begin(), longs->end());
                    if (!stored.load(words)) {
                        continue;
                    }
                    for (usize column = 0; column < world::kColumnCount; ++column) {
                        if (tainted[column]) {
                            ++skipped;
                            continue;
                        }
                        const i32 ours   = top[which][column] + 1;
                        const i32 theirs = stored.first_free(column % 16, column / 16);
                        ++compared[which];
                        if (ours == theirs) {
                            ++matched[which];
                        } else if (first_mismatch[which].empty()) {
                            first_mismatch[which] = fmt::format(
                                "chunk {},{} column {},{}: ours {} theirs {}",
                                chunk->root.find("xPos") ? chunk->root.find("xPos")->as_i64() : 0,
                                chunk->root.find("zPos") ? chunk->root.find("zPos")->as_i64() : 0,
                                column % 16, column / 16, ours, theirs);
                        }
                    }
                }
            }
        }
    }

    fmt::print("  chunks ......... {}\n", chunks);
    if (unknown_blocks > 0) {
        fmt::print(
            "  unknown blocks . {} palette entries this version does not have\n"
            "  skipped ........ {} column comparisons touching one\n",
            unknown_blocks, skipped);
    }
    bool all_ok = true;
    for (usize which = 0; which < 4; ++which) {
        if (compared[which] == 0) {
            continue;
        }
        const bool ok = matched[which] == compared[which];
        all_ok        = all_ok && ok;
        fmt::print("  {}{:<26}{} {}/{}\n", ok ? "\033[0;32m" : "\033[0;31m", kNames[which],
                   "\033[0m", matched[which], compared[which]);
        if (!ok) {
            fmt::print("      first: {}\n", first_mismatch[which]);
        }
    }
    return all_ok ? 0 : 1;
}

/// Report a column's four stored heightmaps, and the block at a given y.
///
/// Reads "x y z" lines and prints one line each. This is the measuring end of
/// the MOTION_BLOCKING question: place a block on a real 1.20.1 server, let it
/// save, and read back which heightmaps the game itself decided the block
/// belongs to. The block's name is printed alongside because a block that
/// refused to stay — a torch with nothing to hang on — must be discarded rather
/// than recorded as "does not block motion".
int inspect_columns(const std::filesystem::path& directory) {
    std::map<std::pair<i32, i32>, std::optional<nbt::RegionFile>> regions;

    // The overworld's shape. The heightmap origin is the dimension's floor, not
    // the lowest section the file lists — a distinction that costs sixteen
    // blocks in the chunks where vanilla writes an extra section for lighting.
    constexpr i32 kMinY   = -64;
    constexpr u32 kHeight = 384;

    constexpr std::array kNames = std::to_array<std::string_view>(
        {"WORLD_SURFACE", "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES", "OCEAN_FLOOR"});

    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    while (std::cin >> x >> y >> z) {
        const i32  chunk_x    = x >> 4;
        const i32  chunk_z    = z >> 4;
        const auto region_key = std::pair{chunk_x >> 5, chunk_z >> 5};

        if (!regions.contains(region_key)) {
            const auto path =
                directory / fmt::format("r.{}.{}.mca", region_key.first, region_key.second);
            auto opened = nbt::RegionFile::open(path);
            regions.emplace(region_key, opened ? std::optional{std::move(*opened)} : std::nullopt);
        }
        const auto& region = regions.at(region_key);
        if (!region) {
            fmt::print("{} {} {} - - - - -\n", x, y, z);
            continue;
        }
        const auto chunk =
            region->read_chunk(static_cast<u32>(chunk_x & 31), static_cast<u32>(chunk_z & 31));
        if (!chunk) {
            fmt::print("{} {} {} - - - - -\n", x, y, z);
            continue;
        }

        std::string_view name = "-";
        if (const nbt::Tag* sections = chunk->root.find("sections");
            sections != nullptr && sections->list() != nullptr) {
            for (const nbt::Tag& section : *sections->list()) {
                const nbt::Tag* section_y = section.find("Y");
                if (section_y == nullptr || section_y->as_i64() != (y >> 4)) {
                    continue;
                }
                const nbt::Tag* states = section.find("block_states");
                if (states == nullptr) {
                    break;
                }
                const nbt::Tag* palette = states->find("palette");
                if (palette == nullptr || palette->list() == nullptr || palette->list()->empty()) {
                    break;
                }
                const usize index =
                    ((static_cast<usize>(y & 15) * 16) + static_cast<usize>(z & 15)) * 16 +
                    static_cast<usize>(x & 15);
                usize           slot = 0;
                const nbt::Tag* data = states->find("data");
                const auto* longs = data == nullptr ? nullptr : data->get_if<nbt::Tag::LongArray>();
                if (longs != nullptr && !longs->empty()) {
                    const u8   bits = world::bits_for_palette(palette->list()->size());
                    const u32  per  = world::entries_per_long(bits);
                    const auto word = static_cast<u64>((*longs)[index / per]);
                    slot            = static_cast<usize>((word >> ((index % per) * bits)) &
                                                         ((u64{1} << bits) - 1));
                }
                if (slot < palette->list()->size()) {
                    if (const nbt::Tag* tag = (*palette->list())[slot].find("Name")) {
                        name = tag->as_string();
                    }
                }
                break;
            }
        }

        fmt::print("{} {} {} {}", x, y, z, name);

        const nbt::Tag* maps = chunk->root.find("Heightmaps");
        for (const std::string_view key : kNames) {
            const nbt::Tag* tag   = maps == nullptr ? nullptr : maps->find(key);
            const auto*     longs = tag == nullptr ? nullptr : tag->get_if<nbt::Tag::LongArray>();
            if (longs == nullptr) {
                // Absent is not zero here: vanilla omits a heightmap it has not
                // computed, and reporting 0 would read as "the column is empty".
                fmt::print(" -");
                continue;
            }
            world::Heightmap map{kMinY, kHeight};
            std::vector<u64> words(longs->begin(), longs->end());
            if (!map.load(words)) {
                fmt::print(" ?");
                continue;
            }
            fmt::print(" {}",
                       map.first_free(static_cast<usize>(x & 15), static_cast<usize>(z & 15)));
        }
        fmt::print("\n");
    }
    return 0;
}

/// Roll a block's loot table many times and report what came out.
///
/// The point is comparison: the same cases are rolled on a real 1.20.1 server
/// through its own `/loot` command, and the two distributions are compared.
/// Reads "block silk fortune tool samples" lines, one case per line, and prints
/// one line per case.
int inspect_loot(const std::filesystem::path& pack_path) {
    auto blocks     = registry::BlockRegistry::load(pack_path);
    auto registries = registry::Registries::load(pack_path);
    if (!blocks || !registries) {
        fmt::print(stderr, "cannot read {}\n", pack_path.string());
        return 1;
    }
    const gameplay::LootTables tables{*blocks, *registries};
    const auto                 items = registries->find("minecraft:item");

    // A fixed seed: the run has to be repeatable, and the comparison is between
    // distributions rather than between individual rolls.
    math::XoroshiroRandomSource random{0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL};

    std::string block_name;
    int         silk    = 0;
    int         fortune = 0;
    std::string tool_name;
    int         samples = 0;

    while (std::cin >> block_name >> silk >> fortune >> tool_name >> samples) {
        const auto block = blocks->find_block(block_name);
        if (!block) {
            fmt::print("{} ?\n", block_name);
            continue;
        }
        gameplay::Held held;
        held.silk_touch = static_cast<u8>(silk);
        held.fortune    = static_cast<u8>(fortune);
        if (tool_name != "-" && items) {
            held.item = registries->protocol_id(*items, tool_name);
        }

        std::map<i32, std::pair<i64, i64>> totals;  // item -> (count, occurrences)
        std::vector<gameplay::Drop>        out;
        for (int i = 0; i < samples; ++i) {
            out.clear();
            tables.drops(blocks->default_state(*block), held, random, out);
            for (const gameplay::Drop& drop : out) {
                auto& entry = totals[drop.item];
                entry.first += drop.count;
                entry.second += 1;
            }
        }

        fmt::print("{}", block_name);
        for (const auto& [item, tally] : totals) {
            fmt::print(" {}:{}:{}",
                       items ? registries->entry_of(*items, item) : std::string_view{"?"},
                       tally.first, tally.second);
        }
        fmt::print("\n");
    }
    return 0;
}

void print_usage() {
    fmt::print(
        "ov-inspect — read Minecraft's binary formats\n"
        "\n"
        "  ov-inspect nbt    <file> [--tree] [--verify] [--depth=N]\n"
        "  ov-inspect region <file.mca> [--verify]\n"
        "  ov-inspect zip    <file.jar|.zip> [--verify]\n"
        "  ov-inspect chunk  <file.mca>          verify section bit-packing\n"
        "  ov-inspect light  <region-dir>        block, block light and sky light per 'x y z'\n"
        "  ov-inspect column <region-dir>        block and the four heightmaps per 'x y z'\n"
        "  ov-inspect heightmaps <region-dir> [--pack=P]  recompute them and compare\n"
        "  ov-inspect loot   <pack>              roll loot tables per 'block silk fortune tool n'\n"
        "  ov-inspect state  <region-dir>        full block state per 'x y z'\n"
        "\n"
        "  --tree      print the tag tree\n"
        "  --verify    decode, re-encode, and compare the bytes\n"
        "  --depth=N   limit tree depth (default 4)\n"
        "\n"
        "Handles gzip, zlib and uncompressed input; the container is sniffed,\n"
        "not taken from the file extension.\n"
        "\n"
        "Not an official Minecraft product. Not approved by or associated with Mojang.\n");
}

}  // namespace

int main(int argc, char** argv) {
    ov::set_log_level(ov::LogLevel::Warn);

    if (argc < 3) {
        print_usage();
        return argc < 2 ? 1 : 0;
    }

    const std::string_view command{argv[1]};
    if (command != "nbt" && command != "region" && command != "zip" && command != "chunk" &&
        command != "light" && command != "column" && command != "heightmaps" && command != "loot" &&
        command != "state") {
        fmt::print(stderr, "unknown command '{}'\n", command);
        print_usage();
        return 1;
    }

    bool        tree      = false;
    bool        verify    = false;
    int         max_depth = 4;
    std::string pack      = "data/vanilla/1.20.1/registry.ovpack";

    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--tree") {
            tree = true;
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg.starts_with("--pack=")) {
            pack = std::string{arg.substr(7)};
        } else if (arg.starts_with("--depth=")) {
            max_depth = std::atoi(std::string{arg.substr(8)}.c_str());
        } else {
            fmt::print(stderr, "unknown option '{}'\n", arg);
            return 1;
        }
    }

    if (command == "region") {
        return inspect_region(argv[2], verify);
    }
    if (command == "zip") {
        return inspect_zip(argv[2], verify);
    }
    if (command == "chunk") {
        return inspect_chunk_packing(argv[2]);
    }
    if (command == "light") {
        return inspect_light(argv[2]);
    }
    if (command == "column") {
        return inspect_columns(argv[2]);
    }
    if (command == "heightmaps") {
        return inspect_heightmaps(argv[2], pack);
    }
    if (command == "loot") {
        return inspect_loot(argv[2]);
    }
    if (command == "state") {
        return inspect_states(argv[2]);
    }
    return inspect_nbt(argv[2], tree, verify, max_depth);
}
