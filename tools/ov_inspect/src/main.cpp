// ov-inspect — read Minecraft's binary formats and say what is in them.
//
// The point is not pretty output. It is that every format Ondes VOXEL claims to
// support can be pointed at a real file from a real world and checked, by hand,
// today. `nbt --verify` in particular decodes a file, re-encodes it, and
// compares the bytes: that is the whole of "our NBT support is correct",
// reduced to one command someone can run on their own save.

#define OV_LOG_CATEGORY "inspect"

#include "ov/base/log.hpp"
#include "ov/io/compression.hpp"
#include "ov/io/file.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/nbt/tag.hpp"

#include <fmt/format.h>

#include <filesystem>
#include <string>
#include <string_view>
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
    return failed == 0 ? 0 : 1;
}

void print_usage() {
    fmt::print(
        "ov-inspect — read Minecraft's binary formats\n"
        "\n"
        "  ov-inspect nbt    <file> [--tree] [--verify] [--depth=N]\n"
        "  ov-inspect region <file.mca> [--verify]\n"
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
    if (command != "nbt" && command != "region") {
        fmt::print(stderr, "unknown command '{}'\n", command);
        print_usage();
        return 1;
    }

    bool tree      = false;
    bool verify    = false;
    int  max_depth = 4;

    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--tree") {
            tree = true;
        } else if (arg == "--verify") {
            verify = true;
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
    return inspect_nbt(argv[2], tree, verify, max_depth);
}
