// Carver parity: our carving masks against the ones the game saved.
//
// The claim a carver has to answer is not "are there caves" and not even "is
// there the right amount of cave". It is "is this cave the one the game cut,
// in the same chunk, at the same block". A carver whose random stream is one
// draw out produces caves that are indistinguishable from vanilla's in every
// aggregate — count, size, depth, branching — and shares no cell with them at
// all, which makes terrain parity *worse* than carving nothing.
//
// That question has an exact answer, and it is in the reference world. A chunk
// that the game generated but has not brought all the way to `minecraft:full`
// still carries `CarvingMasks: {AIR: [L;…]}`: the precise set of cells its
// carvers marked, in a long array whose bit layout is
//
//     index = (x & 15) | ((z & 15) << 4) | ((y - min_y) << 8)
//
// packed low bit first. So this tool reads those chunks, runs our carvers on
// the same coordinates and the same seed, and reports the intersection.
//
// Full chunks have had their masks dropped, which is why the comparison uses
// the unfinished ones — the opposite of the rule for block parity, and for the
// same underlying reason: read the chunks that still hold the evidence for the
// question being asked.
#define OV_LOG_CATEGORY "carveparity"

#include "ov/base/log.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/carving_mask.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace ov;

namespace {

struct Options {
    std::filesystem::path world{"run/reference-1234567890/world"};
    i64                   seed{1234567890};
    /// How many chunks with a saved mask to compare. A few hundred is enough
    /// to separate "exact" from "close" — either every chunk matches to the
    /// bit or the stream is wrong, there is no middle.
    i32 chunks{400};
    /// Print the first few disagreeing chunks in detail.
    i32 show{6};
    i32 min_y{-64};
    i32 height{384};
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        const auto value = [&](std::string_view prefix) { return argument.substr(prefix.size()); };
        if (argument.starts_with("--world=")) {
            options.world = value("--world=");
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=").c_str());
        } else if (argument.starts_with("--chunks=")) {
            options.chunks = std::atoi(value("--chunks=").c_str());
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=").c_str());
        }
    }
    return options;
}

/// The cells one mask has, as (x, y, z), for the handful of chunks printed in
/// full. Only used on the reporting path.
struct Cell {
    i32 x;
    i32 y;
    i32 z;
};

[[nodiscard]] std::vector<Cell> cells_of(const worldgen::CarvingMask& mask) {
    std::vector<Cell> out;
    const auto        words = mask.words();
    for (usize word = 0; word < words.size(); ++word) {
        u64 bits = words[word];
        while (bits != 0) {
            const auto  bit   = static_cast<usize>(std::countr_zero(bits));
            const usize index = word * 64 + bit;
            out.push_back(Cell{static_cast<i32>(index & 15U),
                               static_cast<i32>(index >> 8U) + mask.min_y(),
                               static_cast<i32>((index >> 4U) & 15U)});
            bits &= bits - 1;
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);

    const auto regions = options.world / "region";
    if (!std::filesystem::is_directory(regions)) {
        OV_LOG_ERROR("{} has no region/. Generate one with scripts/reference_world.sh.",
                     regions.string());
        return 1;
    }

    const worldgen::CarvingContext context{options.min_y, options.height};
    const worldgen::CarverStage    stage{options.seed, context};
    worldgen::CarvingMask          ours{context.min_y, context.height};

    usize compared     = 0;
    usize exact        = 0;
    usize theirs_total = 0;
    usize ours_total   = 0;
    usize both         = 0;
    usize ours_only    = 0;
    usize theirs_only  = 0;
    /// Where the two sides disagree, by height band. A carver that is right in
    /// one band and wrong in another is a different bug from one that is wrong
    /// everywhere.
    std::array<usize, 12>    both_by_band{};
    std::array<usize, 12>    ours_only_by_band{};
    std::array<usize, 12>    theirs_only_by_band{};
    std::vector<std::string> reports;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(regions)) {
        if (entry.path().extension() == ".mca") {
            files.push_back(entry.path());
        }
    }
    // Sorted, so that two runs of this tool compare the same chunks and a
    // number can be put next to a previous number.
    std::ranges::sort(files);

    for (const auto& file : files) {
        if (compared >= static_cast<usize>(options.chunks)) {
            break;
        }
        auto region = nbt::RegionFile::open(file);
        if (!region) {
            continue;
        }
        for (u32 index = 0; index < 1024 && compared < static_cast<usize>(options.chunks);
             ++index) {
            const u32 local_x = index % 32;
            const u32 local_z = index / 32;
            if (!region->has_chunk(local_x, local_z)) {
                continue;
            }
            auto document = region->read_chunk(local_x, local_z);
            if (!document) {
                continue;
            }

            const nbt::Tag* masks = document->root.find("CarvingMasks");
            if (masks == nullptr) {
                continue;
            }
            const nbt::Tag* air = masks->find("AIR");
            if (air == nullptr) {
                continue;
            }
            const auto* words = air->get_if<nbt::Tag::LongArray>();
            if (words == nullptr || words->empty()) {
                continue;
            }
            const nbt::Tag* x_pos = document->root.find("xPos");
            const nbt::Tag* z_pos = document->root.find("zPos");
            if (x_pos == nullptr || z_pos == nullptr) {
                continue;
            }
            const auto chunk_x = static_cast<i32>(x_pos->as_i64());
            const auto chunk_z = static_cast<i32>(z_pos->as_i64());

            const auto reference =
                worldgen::CarvingMask::from_long_array(*words, context.min_y, context.height);
            stage.carve_into(chunk_x, chunk_z, ours);

            ++compared;
            const auto reference_words = reference.words();
            const auto our_words       = ours.words();
            usize      chunk_both      = 0;
            usize      chunk_ours      = 0;
            usize      chunk_theirs    = 0;
            for (usize word = 0; word < our_words.size(); ++word) {
                const u64 mine  = our_words[word];
                const u64 other = word < reference_words.size() ? reference_words[word] : 0ULL;
                chunk_both += static_cast<usize>(std::popcount(mine & other));
                chunk_ours += static_cast<usize>(std::popcount(mine & ~other));
                chunk_theirs += static_cast<usize>(std::popcount(other & ~mine));
            }
            both += chunk_both;
            ours_only += chunk_ours;
            theirs_only += chunk_theirs;
            ours_total += ours.count();
            theirs_total += reference.count();
            if (chunk_ours == 0 && chunk_theirs == 0) {
                ++exact;
            } else if (reports.size() < static_cast<usize>(options.show)) {
                reports.push_back(
                    fmt::format("  chunk ({:>5},{:>5})  game {:>6}  ours {:>6}  shared {:>6}",
                                chunk_x, chunk_z, reference.count(), ours.count(), chunk_both));
            }

            // The per-band split needs the cells, which is only worth doing
            // while the numbers are still being read by a person.
            for (const Cell& cell : cells_of(ours)) {
                const auto band =
                    static_cast<usize>(std::clamp((cell.y - context.min_y) / 32, 0, 11));
                if (reference.get(cell.x, cell.y, cell.z)) {
                    ++both_by_band[band];
                } else {
                    ++ours_only_by_band[band];
                }
            }
            for (const Cell& cell : cells_of(reference)) {
                if (!ours.get(cell.x, cell.y, cell.z)) {
                    const auto band =
                        static_cast<usize>(std::clamp((cell.y - context.min_y) / 32, 0, 11));
                    ++theirs_only_by_band[band];
                }
            }
        }
    }

    if (compared == 0) {
        OV_LOG_ERROR(
            "no chunk in {} carries a saved carving mask. Only chunks that stopped "
            "before minecraft:full keep one.",
            regions.string());
        return 1;
    }

    fmt::print("seed {}, {} chunks with a saved AIR mask\n", options.seed, compared);
    fmt::print("chunks matching the game bit for bit: {} / {}  ({:.3f} %)\n", exact, compared,
               100.0 * static_cast<f64>(exact) / static_cast<f64>(compared));
    fmt::print("cells: game {}, ours {}\n", theirs_total, ours_total);
    const auto union_size = both + ours_only + theirs_only;
    fmt::print(
        "  in both:              {:>9}  ({:.3f} % of the game's)\n", both,
        theirs_total == 0 ? 0.0 : 100.0 * static_cast<f64>(both) / static_cast<f64>(theirs_total));
    fmt::print("  ours only:            {:>9}\n", ours_only);
    fmt::print("  the game's only:      {:>9}\n", theirs_only);
    fmt::print(
        "  intersection / union: {:.3f} %\n",
        union_size == 0 ? 0.0 : 100.0 * static_cast<f64>(both) / static_cast<f64>(union_size));

    fmt::print("\nby height:\n");
    fmt::print("  {:>14} {:>10} {:>10} {:>10}\n", "y", "both", "ours only", "game only");
    for (usize band = 0; band < both_by_band.size(); ++band) {
        if (both_by_band[band] == 0 && ours_only_by_band[band] == 0 &&
            theirs_only_by_band[band] == 0) {
            continue;
        }
        const i32 low = context.min_y + static_cast<i32>(band) * 32;
        fmt::print("  {:>6} .. {:>4} {:>10} {:>10} {:>10}\n", low, low + 31, both_by_band[band],
                   ours_only_by_band[band], theirs_only_by_band[band]);
    }

    if (!reports.empty()) {
        fmt::print("\nfirst chunks that differ:\n");
        for (const auto& line : reports) {
            fmt::print("{}\n", line);
        }
    }
    return 0;
}
