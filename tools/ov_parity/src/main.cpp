// Terrain parity against the real game.
//
// The whole point of seed-exact generation is that a claim about it can be
// checked, and only one thing can check it: a world the game itself generated
// for the same seed. scripts/reference_world.sh makes one; this reads it back
// and compares, cell by cell, against what our generator says.
//
// It reports a count and a handful of disagreements rather than a verdict. A
// number that is not 100 % is information — which biomes, and where — and a
// tool that answered yes or no would throw that away.
#define OV_LOG_CATEGORY "parity"

#include "ov/base/log.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/density.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <filesystem>
#include <map>
#include <string>

using namespace ov;

namespace {

struct Options {
    std::filesystem::path world{"run/reference-1234567890/world"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    i64                   seed{1234567890};
    /// How many chunks to compare. The whole reference world is thousands;
    /// a few hundred settles the question and keeps the loop short.
    i32 chunks{200};
    /// Try small distortions of one axis and report the agreement each gives.
    ///
    /// This is measurement rather than theory: if a scale of 1.05 on one axis
    /// takes agreement from 80 % to 99 %, the bug is a factor and not a
    /// formula, and that is a far shorter road than reasoning about which of
    /// twenty constants is a percent out.
    std::string sweep;
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        const auto        value = [&](std::string_view prefix) {
            return argument.substr(prefix.size());
        };
        if (argument.starts_with("--world=")) {
            options.world = value("--world=");
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=").c_str());
        } else if (argument.starts_with("--chunks=")) {
            options.chunks = std::atoi(value("--chunks=").c_str());
        } else if (argument.starts_with("--data=")) {
            options.data = value("--data=");
        } else if (argument.starts_with("--reports=")) {
            options.reports = value("--reports=");
        } else if (argument.starts_with("--sweep=")) {
            options.sweep = value("--sweep=");
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);

    if (!std::filesystem::is_directory(options.world / "region")) {
        OV_LOG_ERROR("{} has no region/. Generate one with scripts/reference_world.sh.",
                     options.world.string());
        return 1;
    }

    auto router = worldgen::NoiseRouter::load(options.data, "overworld", options.seed);
    if (!router) {
        OV_LOG_ERROR("router: {}", worldgen::to_string(router.error()));
        return 1;
    }
    auto biomes = worldgen::BiomeSource::load(options.reports, "overworld");
    if (!biomes) {
        OV_LOG_ERROR("biome table: {}", worldgen::to_string(biomes.error()));
        return 1;
    }

    std::vector<std::pair<worldgen::ClimatePoint, std::string>> recorded;
    usize cells    = 0;
    usize agreed   = 0;
    usize chunks   = 0;
    // What we said where the game said something else, most common first. The
    // shape of the disagreement is what says which axis is wrong.
    std::map<std::string, usize> confusions;
    /// What each side said overall. A biome we never produce, or one we
    /// produce far too often, points at an axis rather than at a box.
    std::map<std::string, usize> theirs;
    std::map<std::string, usize> mine;
    /// The climate of the first few disagreements, which is what says which
    /// axis is wrong.
    std::vector<std::string> samples;
    /// The range each axis actually covers. An axis that is scaled wrongly
    /// shows here as a span that is too narrow or too wide, which no single
    /// disagreement would reveal.
    /// Where the humidity sat, per biome the game named. A bias in one axis
    /// shows as two distributions that overlap where the game's do not.
    std::map<std::string, std::array<i64, 3>> humidity_census;
    /// How many disagreements each axis was outside the box on.
    std::array<i64, 7> blame{};
    /// Disagreements where both biomes were exactly as near. Those are decided
    /// by the order of the table, not by the climate.
    usize ties = 0;
    std::array<i64, 7>                        lowest{};
    std::array<i64, 7> highest{};
    lowest.fill(std::numeric_limits<i64>::max());
    highest.fill(std::numeric_limits<i64>::min());

    for (const auto& entry : std::filesystem::directory_iterator(options.world / "region")) {
        if (entry.path().extension() != ".mca") {
            continue;
        }
        auto region = nbt::RegionFile::open(entry.path());
        if (!region) {
            continue;
        }
        for (u32 index = 0; index < 1024 && chunks < static_cast<usize>(options.chunks); ++index) {
            const u32 local_x = index % 32;
            const u32 local_z = index / 32;
            if (!region->has_chunk(local_x, local_z)) {
                continue;
            }
            auto document = region->read_chunk(local_x, local_z);
            if (!document) {
                continue;
            }

            // Only finished chunks. A force-loaded region contains chunks that
            // stopped at an earlier status, and their biome array is filled
            // with the default — plains — which is not a generation result at
            // all. Reading them made every disagreement say "the game chose
            // plains", which was the clue.
            const nbt::Tag* status = document->root.find("Status");
            if (status == nullptr || status->as_string() != "minecraft:full") {
                continue;
            }

            const nbt::Tag* x_pos = document->root.find("xPos");
            const nbt::Tag* z_pos = document->root.find("zPos");
            const nbt::Tag* list  = document->root.find("sections");
            if (x_pos == nullptr || z_pos == nullptr || list == nullptr ||
                list->type() != nbt::TagType::List) {
                continue;
            }
            const auto chunk_x = static_cast<i32>(x_pos->as_i64());
            const auto chunk_z = static_cast<i32>(z_pos->as_i64());

            for (const nbt::Tag& section : *list->list()) {
                const nbt::Tag* y_tag  = section.find("Y");
                const nbt::Tag* biomes_tag = section.find("biomes");
                if (y_tag == nullptr || biomes_tag == nullptr) {
                    continue;
                }
                const auto      section_y = static_cast<i32>(y_tag->as_i64());
                const nbt::Tag* palette   = biomes_tag->find("palette");
                if (palette == nullptr || palette->type() != nbt::TagType::List) {
                    continue;
                }
                std::vector<std::string> names;
                for (const nbt::Tag& name : *palette->list()) {
                    names.push_back(std::string(name.as_string()));
                }
                if (names.empty()) {
                    continue;
                }
                const nbt::Tag* data = biomes_tag->find("data");

                // A single-entry palette carries no data at all: every cell in
                // the section is that biome.
                const usize bits =
                    names.size() <= 1
                        ? 0
                        : static_cast<usize>(std::bit_width(names.size() - 1));
                std::vector<i64> words;
                if (data != nullptr) {
                    if (const auto* longs = data->get_if<nbt::Tag::LongArray>()) {
                        words.assign(longs->begin(), longs->end());
                    }
                }

                for (u32 cell = 0; cell < 64; ++cell) {
                    usize palette_index = 0;
                    if (bits != 0) {
                        if (words.empty()) {
                            continue;
                        }
                        const usize per_word = 64 / bits;
                        const usize word     = cell / per_word;
                        const usize offset   = (cell % per_word) * bits;
                        if (word >= words.size()) {
                            continue;
                        }
                        palette_index = static_cast<usize>(
                            (static_cast<u64>(words[word]) >> offset) & ((1ULL << bits) - 1));
                    }
                    if (palette_index >= names.size()) {
                        continue;
                    }

                    // Cells are indexed y, z, x over the 4x4x4 grid.
                    const i32 cx = static_cast<i32>(cell % 4);
                    const i32 cz = static_cast<i32>((cell / 4) % 4);
                    const i32 cy = static_cast<i32>(cell / 16);

                    const i32 quart_x = chunk_x * 4 + cx;
                    const i32 quart_z = chunk_z * 4 + cz;
                    const i32 quart_y = section_y * 4 + cy;

                    const auto climate = biomes->sample(*router, quart_x, quart_y, quart_z);
                    if (!options.sweep.empty()) {
                        // Keep the reading and the game's answer; the sweep
                        // re-scores them without resampling the noise, which is
                        // what makes trying thirty distortions cheap.
                        recorded.emplace_back(climate, std::string(names[palette_index]));
                        ++cells;
                        continue;
                    }
                    const auto ours = biomes->biome_at(climate);

                    ++cells;
                    for (usize axis = 0; axis < 7; ++axis) {
                        lowest[axis]  = std::min(lowest[axis], climate.coordinates[axis]);
                        highest[axis] = std::max(highest[axis], climate.coordinates[axis]);
                    }
                    theirs[names[palette_index]] += 1;
                    {
                        auto& row = humidity_census[std::string(names[palette_index])];
                        row[0] += 1;
                        row[1] += climate.coordinates[1];
                        row[2] = std::max(row[2], climate.coordinates[1]);
                    }
                    mine[std::string(ours)] += 1;
                    if (ours == names[palette_index]) {
                        ++agreed;
                    } else {
                        confusions[fmt::format("{} -> {}", names[palette_index], ours)] += 1;
                        // Which axis keeps us out of the box the game chose.
                        // Everything else is noise; this is the number that
                        // names the bug.
                        // Not "which axis is outside its box" — depth almost
                        // never sits exactly on 0.0 or 1.0, so that counts
                        // everything and names nothing. What matters is which
                        // axis costs us MORE against the game's biome than
                        // against the one we chose: that is the axis that
                        // decided the disagreement.
                        const auto gaps  = biomes->gap_to(climate, names[palette_index]);
                        const auto mine_ = biomes->gap_to(climate, ours);
                        for (usize axis = 0; axis < 7; ++axis) {
                            blame[axis] += gaps[axis] * gaps[axis] - mine_[axis] * mine_[axis];
                        }
                        if (biomes->distance_to(climate, names[palette_index]) ==
                            biomes->distance_to(climate, ours)) {
                            ++ties;
                        }
                        if (samples.size() < 8 && (confusions.size() + cells) % 1499 == 0) {
                            samples.push_back(fmt::format(
                                "  ({:>6},{:>5},{:>6})  gap to {}: t{} h{} c{} e{} d{} w{} o{}",
                                quart_x * 4, quart_y * 4, quart_z * 4, names[palette_index],
                                gaps[0], gaps[1], gaps[2], gaps[3], gaps[4], gaps[5], gaps[6]));
                        }
                    }
                }
            }
            ++chunks;
        }
        if (chunks >= static_cast<usize>(options.chunks)) {
            break;
        }
    }

    if (cells == 0) {
        OV_LOG_ERROR("no biome cells were read");
        return 1;
    }

    if (!options.sweep.empty()) {
        constexpr std::array<const char*, 6> kNames{"temperature", "humidity", "continentalness",
                                                    "erosion",     "depth",    "weirdness"};
        usize axis = 0;
        for (usize i = 0; i < kNames.size(); ++i) {
            if (options.sweep == kNames[i]) {
                axis = i;
            }
        }
        fmt::print("\nsweeping {} over {} cells\n", kNames[axis], recorded.size());
        fmt::print("{:>8} {:>8}   {:>10}\n", "scale", "offset", "agreed");
        for (const f64 scale : {1.00, 1.25, 1.50, 1.75, 2.00, 2.50, 3.00}) {
            for (const i64 offset : {-200, 0, 200}) {
                usize hits = 0;
                for (const auto& [climate, expected] : recorded) {
                    worldgen::ClimatePoint distorted = climate;
                    distorted.coordinates[axis] =
                        static_cast<i64>(static_cast<f64>(climate.coordinates[axis]) * scale) +
                        offset;
                    if (biomes->biome_at(distorted) == expected) {
                        ++hits;
                    }
                }
                fmt::print("{:>8.2f} {:>8}   {:>7} ({:>6.2f} %)\n", scale, offset, hits,
                           100.0 * static_cast<f64>(hits) / static_cast<f64>(recorded.size()));
            }
        }
        return 0;
    }

    fmt::print("\nseed {}, {} chunks, {} biome cells\n", options.seed, chunks, cells);
    fmt::print("agreed: {} / {}  ({:.3f} %)\n", agreed, cells,
               100.0 * static_cast<f64>(agreed) / static_cast<f64>(cells));

    {
        constexpr std::array<const char*, 7> kAxes{"temperature", "humidity", "continentalness",
                                                   "erosion",     "depth",    "weirdness",
                                                   "offset"};
        fmt::print("\nthe range each axis covered:\n");
        for (usize axis = 0; axis < kAxes.size(); ++axis) {
            fmt::print("  {:<16} {:>8} .. {:>8}\n", kAxes[axis], lowest[axis], highest[axis]);
        }
    }

    if (agreed != cells) {
        constexpr std::array<const char*, 7> kNames{"temperature", "humidity", "continentalness",
                                                    "erosion",     "depth",    "weirdness",
                                                    "offset"};
        fmt::print("\n{} of {} disagreements were exact ties\n", ties, cells - agreed);
        fmt::print("\nwhich axis decided the disagreements (higher = more to blame):\n");
        for (usize axis = 0; axis < kNames.size(); ++axis) {
            fmt::print("  {:<16} {:>16}\n", kNames[axis], blame[axis]);
        }
    }

    fmt::print("\nour humidity, grouped by the biome the game chose:\n");
    for (const auto& [name, row] : humidity_census) {
        if (row[0] < 200) {
            continue;
        }
        fmt::print("  {:<34} n={:>6}  mean {:>7}  max {:>7}\n", name, row[0], row[1] / row[0],
                   row[2]);
    }

    if (agreed != cells) {
        fmt::print("\nwhat each side said (game / ours):\n");
        std::vector<std::pair<std::string, usize>> census(theirs.begin(), theirs.end());
        std::sort(census.begin(), census.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (usize i = 0; i < census.size() && i < 12; ++i) {
            const auto found = mine.find(census[i].first);
            fmt::print("  {:<34} {:>7} / {:>7}\n", census[i].first, census[i].second,
                       found == mine.end() ? 0 : found->second);
        }
        fmt::print("\nthe first few, with their climate (x, y, z):\n");
        for (const auto& line : samples) {
            fmt::print("{}\n", line);
        }

        std::vector<std::pair<std::string, usize>> sorted(confusions.begin(), confusions.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        fmt::print("\nthe disagreements, most common first (game -> ours):\n");
        for (usize i = 0; i < sorted.size() && i < 15; ++i) {
            fmt::print("  {:>7}  {}\n", sorted[i].second, sorted[i].first);
        }
        if (sorted.size() > 15) {
            fmt::print("  … and {} more kinds\n", sorted.size() - 15);
        }
    }
    return agreed == cells ? 0 : 2;
}
