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
#include "ov/worldgen/carver.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/noise.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>

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
    /// Which environment variable `--nether --sweep=` moves. The Nether
    /// comparison is a distribution oracle for whatever changes the shape of
    /// `base_3d_noise`, and the amplitude is not the only candidate: the blend
    /// selector is another, and asking the same oracle about it costs one
    /// string rather than a second tool.
    std::string sweep_var{"OV_BASE3D_GAIN"};
    /// Report the raw distribution of the blend selector, and how often it is
    /// saturated. No reference world involved.
    bool selector{false};
    /// Print one representative surface position per biome instead of
    /// comparing. What the renderer needs to be pointed at.
    bool locate{false};
    /// Compare blocks rather than biomes.
    bool terrain{false};
    /// Run the carvers before comparing, and subtract what they cut. Off by
    /// default so that the same binary gives the before and the after figure
    /// without rebuilding — the only honest way to say what a stage is worth.
    bool carvers{false};
    /// Report the distribution of each named term instead of comparing.
    ///
    /// No reference world is involved: this asks whether a term is the shape
    /// it is supposed to be. `base_3d_noise` is a sum of Perlin octaves
    /// blended between two symmetric stacks, so its mean over a large sample
    /// must be zero; a mean that is not says the octave weighting is wrong,
    /// and a term biased by a few hundredths is exactly what a surface one
    /// block low looks like.
    bool stats{false};
    /// Compare the *shape* of the surface, not only its height.
    ///
    /// A percentage cannot tell a field that is too large from a field that is
    /// the wrong field: both lower the score by roughly as much. The surface
    /// height read as a map can. Its standard deviation answers scale, its
    /// structure function answers which wavelengths are present, and its
    /// correlation with the game's own map answers whether our noise is the
    /// same noise at all — an octave stack seeded differently correlates at
    /// chance however well its amplitude is tuned.
    bool surface{false};
    /// Where `--surface` writes its raw per-column map, one line per column.
    ///
    /// The aggregate figures above can rank two candidates but they cannot
    /// say *why* one wins, because a term that is simply absent scores almost
    /// as well as a term that is right. Two runs dumped and regressed against
    /// each other can: the residual the game leaves when a term is removed is
    /// a field, and asking whether a candidate reproduces that field — and at
    /// what slope — is a different question from asking whether it lowers an
    /// average.
    std::string dump;
    /// Read the Nether instead, and report the fraction of stone at each
    /// height on both sides.
    ///
    /// This is the only place old_blended_noise can be seen on its own. The
    /// Nether's noise_settings name no depth, no factor, no jaggedness and no
    /// aquifers: between y = 24 and y = 104 its final_density collapses to
    /// squeeze(0.64 x base_3d_noise), so a block is stone exactly where that
    /// noise is positive. Above y = 104 a clamped gradient walks the threshold
    /// away from zero, about four hundredths a block, so the fraction of stone
    /// at each height *is* that noise's cumulative distribution read off the
    /// game's own blocks. Nothing here needs the seeds to match, which is what
    /// makes it usable: the Nether is seeded from the legacy random source and
    /// this router is not.
    bool nether{false};
    /// Cross the game's own carving mask with the game's own blocks.
    ///
    /// The aquifer is the one stage of the noise with no published algorithm,
    /// so it cannot be read; it has to be measured, and this is the oracle
    /// that measures it. A chunk the game stopped at `minecraft:carvers`
    /// carries both halves of the evidence at once: `CarvingMasks/AIR` says
    /// which cells its carvers *considered*, and the block array says what
    /// they hold afterwards. A considered cell that ends up as water is a cell
    /// where the aquifer said "fluid"; one that ends up as stone is the
    /// barrier refusing the cut. Neither answer is visible anywhere else, and
    /// neither needs our generator to be right first.
    ///
    /// `--dump=` writes the rows; without it only the summary is printed.
    bool aquifer{false};
    /// Dump one column: what the game has, and every term we compute.
    std::string column;
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    /// Carry the climate search's one-entry cache across the biome cells of a
    /// chunk, in the given order. The cache decides ties, so the order the
    /// questions are asked in is part of the answer; "none" asks each cell
    /// independently. See BiomeSearchCache.
    /// "xyz" is what the game does and what measures 100 %; "yzx" is the
    /// order the cells are *stored* in, which is not the order they were asked
    /// about; "none" asks each cell independently and measures 99.972 %.
    std::string cache{"xyz"};
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
        } else if (argument == "--locate") {
            options.locate = true;
        } else if (argument == "--terrain") {
            options.terrain = true;
        } else if (argument == "--carvers") {
            options.carvers = true;
        } else if (argument == "--stats") {
            options.stats = true;
        } else if (argument == "--surface") {
            options.surface = true;
        } else if (argument.starts_with("--sweep-var=")) {
            options.sweep_var = value("--sweep-var=");
        } else if (argument == "--selector") {
            options.selector = true;
        } else if (argument == "--aquifer") {
            options.aquifer = true;
        } else if (argument == "--nether") {
            options.nether = true;
        } else if (argument.starts_with("--dump=")) {
            options.dump = value("--dump=");
        } else if (argument.starts_with("--column=")) {
            options.column = value("--column=");
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=");
        } else if (argument.starts_with("--cache=")) {
            options.cache = value("--cache=");
        }
    }
    return options;
}

/// The block name at a position inside a chunk's NBT, or nullptr.
///
/// Read straight out of the palette rather than through a Chunk, because the
/// comparison is about names and a Chunk would resolve them through our own
/// registry — which would quietly turn a block we do not know into air.
[[nodiscard]] const std::string_view* block_at(const nbt::Document& document, i32 local_x, i32 y,
                                               i32 local_z) {
    static std::string_view result;
    const nbt::Tag*         list = document.root.find("sections");
    if (list == nullptr || list->type() != nbt::TagType::List) {
        return nullptr;
    }
    for (const nbt::Tag& section : *list->list()) {
        const nbt::Tag* y_tag = section.find("Y");
        if (y_tag == nullptr || static_cast<i32>(y_tag->as_i64()) != (y >> 4)) {
            continue;
        }
        const nbt::Tag* states = section.find("block_states");
        if (states == nullptr) {
            return nullptr;
        }
        const nbt::Tag* palette = states->find("palette");
        if (palette == nullptr || palette->type() != nbt::TagType::List ||
            palette->list()->empty()) {
            return nullptr;
        }
        const auto& entries = *palette->list();
        usize       index   = 0;
        if (entries.size() > 1) {
            const nbt::Tag* data = states->find("data");
            if (data == nullptr) {
                return nullptr;
            }
            const auto* longs = data->get_if<nbt::Tag::LongArray>();
            if (longs == nullptr) {
                return nullptr;
            }
            const usize bits =
                std::max<usize>(4, static_cast<usize>(std::bit_width(entries.size() - 1)));
            const usize cell =
                static_cast<usize>(((y & 15) * 16 + local_z) * 16 + local_x);
            const usize per_word = 64 / bits;
            const usize word     = cell / per_word;
            if (word >= longs->size()) {
                return nullptr;
            }
            index = static_cast<usize>((static_cast<u64>((*longs)[word]) >>
                                        ((cell % per_word) * bits)) &
                                       ((1ULL << bits) - 1));
        }
        if (index >= entries.size()) {
            return nullptr;
        }
        const nbt::Tag* name = entries[index].find("Name");
        if (name == nullptr) {
            return nullptr;
        }
        result = name->as_string();
        return &result;
    }
    return nullptr;
}

/// Does this block count as terrain?
///
/// Stone and its relatives only. Grass, dirt and sand are put there by the
/// surface rules, which run after the noise; counting them would measure a
/// stage this comparison is not about.
[[nodiscard]] bool is_solid_name(std::string_view name) {
    if (name == "minecraft:air" || name == "minecraft:cave_air" ||
        name == "minecraft:void_air" || name == "minecraft:water" ||
        name == "minecraft:lava") {
        return false;
    }
    // Ice is not terrain. On a frozen ocean the surface rules freeze the top
    // of the water, so the game's topmost "solid" block sits at the sea level
    // while the noise put the sea bed twenty blocks lower. Counting it made
    // every frozen column report a surface eight or more blocks too low and
    // buried the real offset under an artefact of a later stage.
    if (name == "minecraft:ice" || name == "minecraft:packed_ice" ||
        name == "minecraft:blue_ice" || name == "minecraft:frosted_ice") {
        return false;
    }
    // Trees are not terrain. They are placed by the feature stage, which runs
    // after the noise and is not implemented, so counting their trunks and
    // leaves as solid measures a stage this comparison is not about — and it
    // is most of what sits above the surface, which is exactly where the
    // "missing solid" figure piled up before this list existed.
    constexpr std::array<std::string_view, 8> kFeatureParts{
        "_log", "_leaves", "_wood", "vine", "_stem", "mushroom", "coral", "sapling"};
    for (const std::string_view part : kFeatureParts) {
        if (name.find(part) != std::string_view::npos) {
            return false;
        }
    }
    // The single block of vegetation that sits on almost every grass block.
    // Matched exactly rather than by substring: "grass_block" is terrain and
    // "short_grass" is not, and a substring test cannot tell them apart.
    constexpr std::array<std::string_view, 16> kPlants{
        "minecraft:short_grass", "minecraft:grass",     "minecraft:tall_grass",
        "minecraft:fern",        "minecraft:large_fern", "minecraft:dead_bush",
        "minecraft:snow",        "minecraft:sugar_cane", "minecraft:cactus",
        "minecraft:bamboo",      "minecraft:seagrass",   "minecraft:tall_seagrass",
        "minecraft:kelp",        "minecraft:kelp_plant", "minecraft:lily_pad",
        "minecraft:moss_carpet"};
    for (const std::string_view plant : kPlants) {
        if (name == plant) {
            return false;
        }
    }
    // Flowers, of which there are about twenty and all of which end the same
    // way in practice.
    constexpr std::array<std::string_view, 12> kFlowers{
        "minecraft:dandelion",  "minecraft:poppy",       "minecraft:blue_orchid",
        "minecraft:allium",     "minecraft:azure_bluet", "minecraft:oxeye_daisy",
        "minecraft:cornflower", "minecraft:lily_of_the_valley", "minecraft:sunflower",
        "minecraft:lilac",      "minecraft:rose_bush",   "minecraft:peony"};
    for (const std::string_view flower : kFlowers) {
        if (name == flower) {
            return false;
        }
    }
    if (name.find("_tulip") != std::string_view::npos) {
        return false;
    }
    return true;
}

}  // namespace

/// What the Nether's noise said at this block: stone, empty, or unreadable.
///
/// Three answers rather than two, because most of the wrong figures this tool
/// has produced came from forcing a third case into one of the first two.
/// Bedrock is five layers thick at the roof and the floor and is placed with no
/// reference to the density at all; a bastion's nether bricks replaced whatever
/// was there; a fungus grew after the fact. None of those is evidence either
/// way, and counting them as empty is what made the roof look forty per cent
/// too open.
///
/// `cave_air` is counted as stone, and that is not a slip: it is precisely the
/// mark the carvers leave where the noise *had* put stone. Plain air above the
/// lava sea is the noise's own emptiness.
enum class NetherBlock { Stone, Empty, Unknown };

[[nodiscard]] NetherBlock classify_nether(std::string_view name) {
    constexpr std::array<std::string_view, 13> kStone{
        "minecraft:netherrack",        "minecraft:basalt",
        "minecraft:blackstone",        "minecraft:soul_sand",
        "minecraft:soul_soil",         "minecraft:gravel",
        "minecraft:magma_block",       "minecraft:crimson_nylium",
        "minecraft:warped_nylium",     "minecraft:nether_gold_ore",
        "minecraft:nether_quartz_ore", "minecraft:ancient_debris",
        "minecraft:cave_air"};
    for (const std::string_view stone : kStone) {
        if (name == stone) {
            return NetherBlock::Stone;
        }
    }
    if (name == "minecraft:air" || name == "minecraft:lava" || name == "minecraft:water" ||
        name == "minecraft:void_air") {
        return NetherBlock::Empty;
    }
    return NetherBlock::Unknown;
}

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);

    if (options.nether) {
        // The Nether's own defaults, so that --nether needs no other argument.
        const std::filesystem::path world =
            options.world == std::filesystem::path("run/reference-1234567890/world")
                ? std::filesystem::path(
                      fmt::format("run/reference-nether-{}/world/DIM-1", options.seed))
                : options.world;
        if (!std::filesystem::is_directory(world / "region")) {
            OV_LOG_ERROR("{} has no region/. Generate one with scripts/reference_nether.sh.",
                         world.string());
            return 1;
        }
        // Counted per height, on both sides, over the same columns.
        std::array<i64, 128> their_stone{};
        std::array<i64, 128> their_total{};
        usize                chunks_read = 0;
        /// One sampled column, and which of its heights the game gave a
        /// readable answer for. Kept so that our own side is evaluated over
        /// exactly the same blocks, once per candidate amplitude, without
        /// reading the region files again.
        struct NetherColumn {
            i32                x{0};
            i32                z{0};
            std::array<u64, 2> readable{};
        };
        std::vector<NetherColumn> columns;

        for (const auto& entry : std::filesystem::directory_iterator(world / "region")) {
            if (entry.path().extension() != ".mca") {
                continue;
            }
            auto region = nbt::RegionFile::open(entry.path());
            if (!region) {
                continue;
            }
            // `--chunks` is per region file here, not per run. One region is
            // one patch of Nether a few hundred blocks across, and a lava sea
            // can fill all of it: the first version of this read sixty chunks
            // from whichever region came first and reported zero stone across
            // eleven heights because that region happened to be an ocean.
            usize from_region = 0;
            for (u32 index = 0;
                 index < 1024 && from_region < static_cast<usize>(options.chunks); ++index) {
                if (!region->has_chunk(index % 32, index / 32)) {
                    continue;
                }
                auto document = region->read_chunk(index % 32, index / 32);
                if (!document) {
                    continue;
                }
                const nbt::Tag* status = document->root.find("Status");
                if (status == nullptr || status->as_string() != "minecraft:full") {
                    continue;
                }
                const nbt::Tag* x_pos = document->root.find("xPos");
                const nbt::Tag* z_pos = document->root.find("zPos");
                if (x_pos == nullptr || z_pos == nullptr) {
                    continue;
                }
                const auto chunk_x = static_cast<i32>(x_pos->as_i64());
                const auto chunk_z = static_cast<i32>(z_pos->as_i64());

                // Every third column again, for the same reason: the cell is
                // four wide and a stride of four would only ever see one
                // residue.
                for (i32 local_z = 0; local_z < 16; local_z += 3) {
                    for (i32 local_x = 0; local_x < 16; local_x += 3) {
                        NetherColumn column{chunk_x * 16 + local_x, chunk_z * 16 + local_z, {}};
                        for (i32 y = 0; y < 128; ++y) {
                            const auto* named = block_at(*document, local_x, y, local_z);
                            if (named == nullptr) {
                                continue;
                            }
                            const NetherBlock what = classify_nether(*named);
                            if (what == NetherBlock::Unknown) {
                                continue;
                            }
                            their_stone[static_cast<usize>(y)] +=
                                what == NetherBlock::Stone ? 1 : 0;
                            ++their_total[static_cast<usize>(y)];
                            column.readable[static_cast<usize>(y) / 64] |=
                                1ULL << (static_cast<usize>(y) % 64);
                        }
                        columns.push_back(column);
                    }
                }
                ++from_region;
                ++chunks_read;
            }
        }

        // The threshold the noise is compared against at each height, read off
        // the Nether's own final_density. Between the two gradients it is
        // exactly zero; outside them it slides, and that slide is what turns a
        // column of blocks into a distribution.
        const auto threshold = [](i32 y) -> f64 {
            const f64 low  = std::clamp((static_cast<f64>(y) + 8.0) / 32.0, 0.0, 1.0);
            const f64 high = std::clamp((128.0 - static_cast<f64>(y)) / 24.0, 0.0, 1.0);
            // 2.5 + low * (-2.5 + 0.9375 + high * (-0.9375 + N)) > 0
            if (low <= 0.0 || high <= 0.0) {
                return std::numeric_limits<f64>::quiet_NaN();
            }
            return -(2.5 * (1.0 - low) / low + 0.9375 * (1.0 - high)) / high;
        };

        // The two bands where the threshold moves. Everywhere between them it
        // is exactly zero and the curve says nothing about amplitude, and
        // outside them the constant 2.5 swamps any noise there could be, so
        // evaluating our side there would cost time and buy nothing.
        const auto interesting = [](i32 y) { return (y >= 8 && y <= 40) || (y >= 92 && y <= 124); };

        // One sweep of the amplitude rather than one build per candidate. The
        // gain multiplies base_3d_noise and nothing else, so the curve it moves
        // is exactly the curve this comparison reads, and the gain whose curve
        // lands on the game's is the factor the arithmetic has to produce.
        std::vector<f64> gains{0.25, 0.35, 0.5, 0.7, 1.0, 1.4, 2.0, 2.8};
        if (!options.sweep.empty()) {
            gains.clear();
            for (usize start = 0; start < options.sweep.size();) {
                const usize comma = options.sweep.find(',', start);
                gains.push_back(std::strtod(options.sweep.substr(start, comma - start).c_str(),
                                            nullptr));
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        }
        const auto& kGains = gains;
        std::vector<std::array<i64, 128>> our_stone(kGains.size());
        std::vector<std::array<i64, 128>> our_total(kGains.size());

        for (usize g = 0; g < kGains.size(); ++g) {
            // The node reads the gain from the environment when it is built, so
            // the router is rebuilt per gain. It is three noises here, not the
            // overworld's thirty-five, and it costs nothing.
            const std::string text = fmt::format("{}", kGains[g]);
            ::setenv(options.sweep_var.c_str(), text.c_str(), 1);
            auto nether = worldgen::NoiseRouter::load(options.data, "nether", options.seed);
            if (!nether) {
                OV_LOG_ERROR("nether router: {}", worldgen::to_string(nether.error()));
                return 1;
            }
            const auto* density = nether->entry("final_density");
            if (density == nullptr) {
                OV_LOG_ERROR("the nether router has no final_density");
                return 1;
            }
            for (const NetherColumn& column : columns) {
                for (i32 y = 0; y < 128; ++y) {
                    if (!interesting(y) ||
                        (column.readable[static_cast<usize>(y) / 64] &
                         (1ULL << (static_cast<usize>(y) % 64))) == 0) {
                        continue;
                    }
                    our_stone[g][static_cast<usize>(y)] +=
                        density->compute({column.x, y, column.z}) > 0.0 ? 1 : 0;
                    ++our_total[g][static_cast<usize>(y)];
                }
            }
        }
        ::unsetenv(options.sweep_var.c_str());

        fmt::print("\nseed {}, {} nether chunks, {} columns, sweeping {}\n", options.seed,
                   chunks_read, columns.size(), options.sweep_var);
        fmt::print(
            "\nfraction of stone at each height. Between y = 24 and y = 104 the threshold is\n"
            "zero and the column says only where the noise's median is; in the two bands where\n"
            "it slides, the column is that noise's survival function read off the game's own\n"
            "blocks.\n\n");
        fmt::print("  {:>4} {:>9} {:>8}", "y", "threshold", "game");
        for (const f64 gain : kGains) {
            fmt::print(" {:>7.2f}", gain);
        }
        fmt::print(" {:>8}\n", "columns");
        for (i32 y = 0; y < 128; ++y) {
            const auto slot = static_cast<usize>(y);
            if (their_total[slot] < 64 || !interesting(y)) {
                continue;
            }
            const f64 t = threshold(y);
            fmt::print("  {:>4} {:>9} {:>8.4f}", y,
                       std::isnan(t) ? std::string("  -") : fmt::format("{:+.4f}", t),
                       static_cast<f64>(their_stone[slot]) / static_cast<f64>(their_total[slot]));
            for (usize g = 0; g < kGains.size(); ++g) {
                fmt::print(" {:>7.4f}", our_total[g][slot] == 0
                                            ? 0.0
                                            : static_cast<f64>(our_stone[g][slot]) /
                                                  static_cast<f64>(our_total[g][slot]));
            }
            fmt::print(" {:>8}\n", their_total[slot]);
        }

        // The distance between the two curves, over the sliding bands only.
        // A single number per gain, so the minimum can be pointed at rather
        // than argued about.
        fmt::print("\nmean absolute distance to the game's curve, over the sliding bands\n");
        fmt::print("  {:>7} {:>10} {:>10} {:>10}\n", "gain", "roof", "floor", "both");
        for (usize g = 0; g < kGains.size(); ++g) {
            f64   roof = 0.0, floor_sum = 0.0;
            usize roof_n = 0, floor_n = 0;
            for (i32 y = 0; y < 128; ++y) {
                const auto slot = static_cast<usize>(y);
                if (their_total[slot] < 64 || our_total[g][slot] == 0 || std::isnan(threshold(y))) {
                    continue;
                }
                const f64 gap = std::abs(static_cast<f64>(their_stone[slot]) /
                                             static_cast<f64>(their_total[slot]) -
                                         static_cast<f64>(our_stone[g][slot]) /
                                             static_cast<f64>(our_total[g][slot]));
                if (y > 104 && y <= 122) {
                    roof += gap;
                    ++roof_n;
                } else if (y >= 12 && y < 24) {
                    floor_sum += gap;
                    ++floor_n;
                }
            }
            fmt::print("  {:>7.2f} {:>10.4f} {:>10.4f} {:>10.4f}\n", kGains[g],
                       roof_n == 0 ? 0.0 : roof / static_cast<f64>(roof_n),
                       floor_n == 0 ? 0.0 : floor_sum / static_cast<f64>(floor_n),
                       roof_n + floor_n == 0
                           ? 0.0
                           : (roof + floor_sum) / static_cast<f64>(roof_n + floor_n));
        }

        // How far the threshold has to travel for the curve to climb from
        // three quarters to ninety-nine hundredths.
        //
        // This is the amplitude on its own, with the level the curve starts
        // from divided out. It matters because the two curves do not start
        // from the same place — our noise is positive rather more often than
        // the game's — and a distance that compares them point by point mixes
        // that offset into the answer. A width does not: it is measured in the
        // threshold's units, which are the noise's own units.
        const auto width = [&](auto value_at) {
            const auto crossing = [&](f64 level) {
                f64 previous_t = 0.0;
                f64 previous_v = value_at(105);
                for (i32 y = 106; y <= 122; ++y) {
                    const f64 t = threshold(y);
                    const f64 v = value_at(y);
                    if (previous_v < level && v >= level && v > previous_v) {
                        return previous_t + (t - previous_t) * (level - previous_v) /
                                                (v - previous_v);
                    }
                    previous_t = t;
                    previous_v = v;
                }
                return std::numeric_limits<f64>::quiet_NaN();
            };
            return crossing(0.99) - crossing(0.75);
        };

        const f64 their_width = width([&](i32 y) {
            const auto slot = static_cast<usize>(y);
            return their_total[slot] == 0 ? 0.0
                                          : static_cast<f64>(their_stone[slot]) /
                                                static_cast<f64>(their_total[slot]);
        });
        fmt::print(
            "\nhow far the threshold travels while the roof curve climbs from 0.75 to 0.99\n");
        fmt::print("  {:>7} {:>10} {:>10}\n", "gain", "width", "vs game");
        fmt::print("  {:>7} {:>10.4f} {:>10}\n", "game", std::abs(their_width), "1.000");
        for (usize g = 0; g < kGains.size(); ++g) {
            const f64 ours = width([&](i32 y) {
                const auto slot = static_cast<usize>(y);
                return our_total[g][slot] == 0 ? 0.0
                                               : static_cast<f64>(our_stone[g][slot]) /
                                                     static_cast<f64>(our_total[g][slot]);
            });
            fmt::print("  {:>7.2f} {:>10.4f} {:>10.3f}\n", kGains[g], std::abs(ours),
                       std::abs(ours / their_width));
        }
        return 0;
    }

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

    if (options.selector) {
        // ── how often is the blend a blend at all? ───────────────────────────
        //
        // `old_blended_noise` interpolates between two sixteen-octave stacks
        // with `blend = (selector / 10 + 1) / 2`, clamped. The selector is
        // eight octaves whose weights double, so it can reach several hundred
        // and the clamp would then be doing all the work — a switch rather than
        // a crossfade. That was written down as a suspected bug and never
        // measured. This measures it. No reference world is involved: it is a
        // property of the field.
        const auto report = [](std::string_view label, const worldgen::BlendedNoise& noise) {
            f64   sum = 0.0, sum2 = 0.0;
            f64   low = std::numeric_limits<f64>::max();
            f64   high = std::numeric_limits<f64>::lowest();
            usize count = 0, saturated_high = 0, saturated_low = 0;
            for (i32 x = -2048; x <= 2048; x += 37) {
                for (i32 z = -2048; z <= 2048; z += 41) {
                    for (i32 y = -32; y <= 160; y += 17) {
                        const f64 value = noise.selector(x, y, z);
                        sum += value;
                        sum2 += value * value;
                        low  = std::min(low, value);
                        high = std::max(high, value);
                        if (value >= 10.0) {
                            ++saturated_high;
                        } else if (value <= -10.0) {
                            ++saturated_low;
                        }
                        ++count;
                    }
                }
            }
            const f64 n    = static_cast<f64>(count);
            const f64 mean = sum / n;
            fmt::print("\n{}: {} samples, divisor {:.4g}\n", label, count,
                       noise.selector_divisor());
            fmt::print("  selector  mean {:+.4f}  sd {:.4f}  min {:+.3f}  max {:+.3f}\n", mean,
                       std::sqrt(std::max(0.0, sum2 / n - mean * mean)), low, high);
            fmt::print("  clamped to max_limit {:.3f} %   to min_limit {:.3f} %   "
                       "genuinely blended {:.3f} %\n",
                       100.0 * static_cast<f64>(saturated_high) / n,
                       100.0 * static_cast<f64>(saturated_low) / n,
                       100.0 * static_cast<f64>(count - saturated_high - saturated_low) / n);
        };

        if (const auto* overworld = router->blended_noise()) {
            report("overworld", *overworld);
        } else {
            fmt::print("the overworld router built no old_blended_noise\n");
        }
        auto nether = worldgen::NoiseRouter::load(options.data, "nether", options.seed);
        if (nether) {
            if (const auto* noise = nether->blended_noise()) {
                report("nether", *noise);
            }
        }
        return 0;
    }

    if (options.aquifer) {
        // ── the aquifer oracle ──────────────────────────────────────────────
        //
        // Two things the game wrote to disk, crossed against each other. The
        // mask says which cells its carvers considered; the blocks say what is
        // there afterwards. Our generator is not consulted for either, which is
        // the point: the answer does not depend on our terrain being right.
        //
        // Only chunks stopped at `minecraft:carvers` are read. A `full` chunk
        // has thrown its mask away, and one stopped earlier has no carving in
        // its blocks; either way the two halves would not be about the same
        // stage. Cells at or above the column's topmost solid block are
        // dropped: a carver that considered open sky says nothing about an
        // aquifer, and counting that air as "above the fluid level" is exactly
        // what made the first pass of this measurement contradict itself.
        const auto* term_flood   = router->entry("fluid_level_floodedness");
        const auto* term_spread  = router->entry("fluid_level_spread");
        const auto* term_lava    = router->entry("lava");
        const auto* term_barrier = router->entry("barrier");
        const auto* term_density = router->entry("final_density");
        if (term_flood == nullptr || term_spread == nullptr || term_lava == nullptr ||
            term_barrier == nullptr || term_density == nullptr) {
            OV_LOG_ERROR(
                "the router is missing one of fluid_level_floodedness, fluid_level_spread, "
                "lava, barrier, final_density; the aquifer oracle needs all five");
            return 1;
        }

        const auto regions = options.world / "region";
        if (!std::filesystem::is_directory(regions)) {
            OV_LOG_ERROR("{} has no region/.", regions.string());
            return 1;
        }
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(regions)) {
            if (entry.path().extension() == ".mca") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);

        std::ofstream out;
        if (!options.dump.empty()) {
            out.open(options.dump);
            if (!out) {
                OV_LOG_ERROR("cannot write {}", options.dump);
                return 1;
            }
            out << "# B x y z kind density   kind: w water, l lava, a air, s solid\n";
            out << "# C gx gy gz flood spread lava barrier  (16 x 12 x 16 cell, "
                   "sampled at the cell's block centre)\n";
            out << "# I gx gy gz flood spread lava barrier  (the same cell, "
                   "sampled at its grid index)\n";
        }

        usize                        chunks = 0;
        usize                        rows   = 0;
        std::map<char, usize>        kinds;
        std::map<std::string, usize> solid_names;
        // Water, lava, air and kept-solid by height band of 16. Where the
        // aquifer acts at all is the first thing to know, and it is not
        // everywhere: above the surface it never fires.
        std::map<i32, std::array<usize, 4>> bands;
        std::set<std::array<i32, 3>>        cells;

        for (const auto& file : files) {
            if (chunks >= static_cast<usize>(options.chunks)) {
                break;
            }
            auto region = nbt::RegionFile::open(file);
            if (!region) {
                continue;
            }
            for (u32 index = 0; index < 1024 && chunks < static_cast<usize>(options.chunks);
                 ++index) {
                auto document = region->read_chunk(index % 32, index / 32);
                if (!document) {
                    continue;
                }
                const nbt::Tag* status = document->root.find("Status");
                if (status == nullptr || status->as_string() != "minecraft:carvers") {
                    continue;
                }
                const nbt::Tag* masks = document->root.find("CarvingMasks");
                const nbt::Tag* air   = masks == nullptr ? nullptr : masks->find("AIR");
                const auto*     words = air == nullptr ? nullptr
                                                       : air->get_if<nbt::Tag::LongArray>();
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
                ++chunks;

                const i32 min_y = router->min_y();
                const i32 max_y = min_y + router->height() - 1;

                // The topmost block of each column that is neither air nor
                // fluid. Everything at or above it is sky.
                std::array<i32, 256> top{};
                for (i32 local_z = 0; local_z < 16; ++local_z) {
                    for (i32 local_x = 0; local_x < 16; ++local_x) {
                        i32 found = min_y - 1;
                        for (i32 y = max_y; y >= min_y; --y) {
                            const std::string_view* name =
                                block_at(*document, local_x, y, local_z);
                            if (name == nullptr) {
                                continue;
                            }
                            if (*name == "minecraft:air" || *name == "minecraft:cave_air" ||
                                *name == "minecraft:void_air" || *name == "minecraft:water" ||
                                *name == "minecraft:lava") {
                                continue;
                            }
                            found = y;
                            break;
                        }
                        top[static_cast<usize>(local_z * 16 + local_x)] = found;
                    }
                }

                for (usize word = 0; word < words->size(); ++word) {
                    u64 bits = static_cast<u64>((*words)[word]);
                    while (bits != 0) {
                        const auto  bit   = static_cast<usize>(std::countr_zero(bits));
                        const usize cell  = word * 64 + bit;
                        bits &= bits - 1;
                        const auto local_x = static_cast<i32>(cell & 15U);
                        const auto local_z = static_cast<i32>((cell >> 4U) & 15U);
                        const auto y = static_cast<i32>(cell >> 8U) + min_y;
                        if (y > max_y || y >= top[static_cast<usize>(local_z * 16 + local_x)]) {
                            continue;
                        }
                        const std::string_view* name =
                            block_at(*document, local_x, y, local_z);
                        if (name == nullptr) {
                            continue;
                        }
                        char kind = 's';
                        if (*name == "minecraft:water") {
                            kind = 'w';
                        } else if (*name == "minecraft:lava") {
                            kind = 'l';
                        } else if (*name == "minecraft:air" || *name == "minecraft:cave_air" ||
                                   *name == "minecraft:void_air") {
                            kind = 'a';
                        } else {
                            solid_names[std::string(*name)] += 1;
                        }
                        ++kinds[kind];
                        ++rows;
                        auto& band = bands[(y >> 4) << 4];
                        band[kind == 'w' ? 0 : kind == 'l' ? 1 : kind == 'a' ? 2 : 3] += 1;

                        const i32 world_x = chunk_x * 16 + local_x;
                        const i32 world_z = chunk_z * 16 + local_z;
                        // The grid the measurement points at: sixteen wide,
                        // forty tall, anchored so that cell k covers
                        // [40k, 40k + 40) and its middle is 40k + 20. Every
                        // fluid level read out of the reference world sits on
                        // `40k + 20 + 3j`, which is what named this grid.
                        cells.insert({world_x >> 4,
                                      y / 40 - (y < 0 && y % 40 != 0 ? 1 : 0),
                                      world_z >> 4});
                        if (out) {
                            out << fmt::format("B {} {} {} {} {:.6f}\n", world_x, y, world_z,
                                               kind,
                                               term_density->compute({world_x, y, world_z}));
                        }
                    }
                }
            }
        }

        if (out) {
            for (const auto& cell : cells) {
                const i32 bx = cell[0] * 16 + 8;
                const i32 by = cell[1] * 40 + 20;
                const i32 bz = cell[2] * 16 + 8;
                out << fmt::format("C {} {} {} {:.6f} {:.6f} {:.6f} {:.6f}\n", cell[0], cell[1],
                                   cell[2], term_flood->compute({bx, by, bz}),
                                   term_spread->compute({bx, by, bz}),
                                   term_lava->compute({bx, by, bz}),
                                   term_barrier->compute({bx, by, bz}));
                out << fmt::format("I {} {} {} {:.6f} {:.6f} {:.6f} {:.6f}\n", cell[0], cell[1],
                                   cell[2],
                                   term_flood->compute({cell[0], cell[1], cell[2]}),
                                   term_spread->compute({cell[0], cell[1], cell[2]}),
                                   term_lava->compute({cell[0], cell[1], cell[2]}),
                                   term_barrier->compute({cell[0], cell[1], cell[2]}));
                // The cell's lowest corner, in blocks. Which of the three a
                // formula reads is not documented anywhere this project may
                // look, so all three are written and the data chooses.
                const i32 ox = cell[0] * 16;
                const i32 oy = cell[1] * 40;
                const i32 oz = cell[2] * 16;
                out << fmt::format("O {} {} {} {:.6f} {:.6f} {:.6f} {:.6f}\n", cell[0], cell[1],
                                   cell[2], term_flood->compute({ox, oy, oz}),
                                   term_spread->compute({ox, oy, oz}),
                                   term_lava->compute({ox, oy, oz}),
                                   term_barrier->compute({ox, oy, oz}));
            }
        }

        fmt::print("\naquifer oracle: {} chunks stopped at minecraft:carvers, {} underground "
                   "considered cells\n",
                   chunks, rows);
        const auto share = [&](usize n) {
            return rows == 0 ? 0.0 : 100.0 * static_cast<f64>(n) / static_cast<f64>(rows);
        };
        fmt::print("  air   {:>8}  {:>7.3f} %\n", kinds['a'], share(kinds['a']));
        fmt::print("  water {:>8}  {:>7.3f} %\n", kinds['w'], share(kinds['w']));
        fmt::print("  lava  {:>8}  {:>7.3f} %\n", kinds['l'], share(kinds['l']));
        fmt::print("  solid {:>8}  {:>7.3f} %   <- the barrier, plus what is not replaceable\n",
                   kinds['s'], share(kinds['s']));
        fmt::print("\n  {:>12} {:>9} {:>9} {:>9} {:>9}\n", "band", "air", "water", "lava",
                   "solid");
        for (const auto& [low, counts] : bands) {
            fmt::print("  {:>5} ..{:>5} {:>9} {:>9} {:>9} {:>9}\n", low, low + 15, counts[2],
                       counts[0], counts[1], counts[3]);
        }
        fmt::print("\n  what a kept cell is made of:\n");
        std::vector<std::pair<std::string, usize>> ordered(solid_names.begin(),
                                                           solid_names.end());
        std::ranges::sort(ordered, [](const auto& a, const auto& b) { return a.second > b.second; });
        for (usize i = 0; i < ordered.size() && i < 12; ++i) {
            fmt::print("    {:<34} {:>8}\n", ordered[i].first, ordered[i].second);
        }
        if (!options.dump.empty()) {
            fmt::print("\n  rows written to {}\n", options.dump);
        }
        return 0;
    }

    if (options.stats) {
        // A grid wide enough to cross continents and deep enough to leave the
        // slides alone, so what is reported is the term's own shape and not
        // the shape of the world's floor and ceiling.
        struct Term {
            std::string_view              name;
            const worldgen::DensityFunction* function;
        };
        const std::array<Term, 8> terms{
            Term{"overworld/base_3d_noise",
                 router->function("minecraft:overworld/base_3d_noise")},
            Term{"overworld/sloped_cheese",
                 router->function("minecraft:overworld/sloped_cheese")},
            Term{"overworld/depth", router->function("minecraft:overworld/depth")},
            Term{"overworld/offset", router->function("minecraft:overworld/offset")},
            Term{"overworld/factor", router->function("minecraft:overworld/factor")},
            Term{"overworld/jaggedness", router->function("minecraft:overworld/jaggedness")},
            Term{"router:final_density", router->entry("final_density")},
            Term{"router:initial_density_without_jaggedness",
                 router->entry("initial_density_without_jaggedness")},
        };

        fmt::print("\n{:>44} {:>10} {:>10} {:>10} {:>10} {:>8}\n", "term", "mean", "sd", "min",
                   "max", "n");
        for (const auto& term : terms) {
            if (term.function == nullptr) {
                fmt::print("{:>44}   not in the router\n", term.name);
                continue;
            }
            f64   sum   = 0.0;
            f64   sum2  = 0.0;
            f64   low   = std::numeric_limits<f64>::max();
            f64   high  = std::numeric_limits<f64>::lowest();
            usize count = 0;
            for (i32 x = -2048; x <= 2048; x += 71) {
                for (i32 z = -2048; z <= 2048; z += 67) {
                    for (i32 y = 0; y <= 128; y += 13) {
                        const f64 value = term.function->compute({x, y, z});
                        sum += value;
                        sum2 += value * value;
                        low  = std::min(low, value);
                        high = std::max(high, value);
                        ++count;
                    }
                }
            }
            const f64 mean = sum / static_cast<f64>(count);
            const f64 sd = std::sqrt(std::max(0.0, sum2 / static_cast<f64>(count) - mean * mean));
            fmt::print("{:>44} {:>+10.5f} {:>10.5f} {:>+10.4f} {:>+10.4f} {:>8}\n", term.name,
                       mean, sd, low, high, count);
        }
        return 0;
    }

    if (!options.column.empty()) {
        const auto comma = options.column.find(',');
        const i32  cx    = std::atoi(options.column.substr(0, comma).c_str());
        const i32  cz    = std::atoi(options.column.substr(comma + 1).c_str());

        auto pack = registry::BlockRegistry::load(options.pack);
        if (!pack) {
            OV_LOG_ERROR("registry {} missing", options.pack.string());
            return 1;
        }
        const worldgen::ChunkGenerator generator{*router, *biomes, *pack};

        // Every term the terrain is built from, so a wrong one can be seen
        // rather than deduced.
        const auto* term_offset  = router->function("minecraft:overworld/offset");
        const auto* term_factor  = router->function("minecraft:overworld/factor");
        const auto* term_jagged  = router->function("minecraft:overworld/jaggedness");
        const auto* term_depth   = router->entry("depth");
        const auto* term_initial = router->entry("initial_density_without_jaggedness");

        fmt::print("column ({}, {})\n", cx, cz);
        fmt::print("  offset {:+.6f}   factor {:+.6f}   jaggedness {:+.6f}\n",
                   term_offset ? term_offset->compute({cx, 0, cz}) : 0.0,
                   term_factor ? term_factor->compute({cx, 0, cz}) : 0.0,
                   term_jagged ? term_jagged->compute({cx, 0, cz}) : 0.0);
        fmt::print("  {:>5} {:>12} {:>12} {:>12}\n", "y", "depth", "initial", "final");
        for (i32 y = 100; y >= 50; --y) {
            fmt::print("  {:>5} {:>12.6f} {:>12.6f} {:>12.6f}   {}\n", y,
                       term_depth ? term_depth->compute({cx, y, cz}) : 0.0,
                       term_initial ? term_initial->compute({cx, y, cz}) : 0.0,
                       generator.density_at(cx, y, cz),
                       generator.is_solid(cx, y, cz) ? "solid" : "");
        }
        return 0;
    }

    if (options.surface) {
        auto loaded = registry::BlockRegistry::load(options.pack);
        if (!loaded) {
            OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
            return 1;
        }
        registry::BlockRegistry        blocks = std::move(*loaded);
        const worldgen::ChunkGenerator generator{*router, *biomes, blocks};

        /// One column: what the game put on top, what we put on top, and
        /// whether the game left it dry. Every column of every chunk rather
        /// than a stride, because the statistics below are about *neighbours*
        /// and a stride of three has no neighbour at distance one.
        struct Column {
            i32  theirs{0};
            i32  ours{0};
            bool dry{false};
        };
        std::map<i64, Column> map;
        const auto            key = [](i32 x, i32 z) {
            return (static_cast<i64>(x) << 32) | static_cast<i64>(static_cast<u32>(z));
        };

        usize read_chunks = 0;
        for (const auto& entry : std::filesystem::directory_iterator(options.world / "region")) {
            if (entry.path().extension() != ".mca") {
                continue;
            }
            auto region = nbt::RegionFile::open(entry.path());
            if (!region) {
                continue;
            }
            for (u32 index = 0;
                 index < 1024 && read_chunks < static_cast<usize>(options.chunks); ++index) {
                if (!region->has_chunk(index % 32, index / 32)) {
                    continue;
                }
                auto document = region->read_chunk(index % 32, index / 32);
                if (!document) {
                    continue;
                }
                const nbt::Tag* status = document->root.find("Status");
                if (status == nullptr || status->as_string() != "minecraft:full") {
                    continue;
                }
                const nbt::Tag* x_pos = document->root.find("xPos");
                const nbt::Tag* z_pos = document->root.find("zPos");
                if (x_pos == nullptr || z_pos == nullptr) {
                    continue;
                }
                const auto chunk_x = static_cast<i32>(x_pos->as_i64());
                const auto chunk_z = static_cast<i32>(z_pos->as_i64());

                for (i32 local_z = 0; local_z < 16; ++local_z) {
                    for (i32 local_x = 0; local_x < 16; ++local_x) {
                        const i32 world_x = chunk_x * 16 + local_x;
                        const i32 world_z = chunk_z * 16 + local_z;
                        i32       theirs  = -65;
                        i32       ours    = -65;
                        for (i32 probe = 200; probe > -64; --probe) {
                            if (theirs == -65) {
                                const auto* at = block_at(*document, local_x, probe, local_z);
                                if (at != nullptr && is_solid_name(*at)) {
                                    theirs = probe;
                                }
                            }
                            if (ours == -65 && generator.is_solid(world_x, probe, world_z)) {
                                ours = probe;
                            }
                            if (theirs != -65 && ours != -65) {
                                break;
                            }
                        }
                        if (theirs == -65 || ours == -65) {
                            continue;
                        }
                        bool wet = false;
                        for (i32 above = 1; above <= 8 && !wet; ++above) {
                            const auto* at = block_at(*document, local_x, theirs + above, local_z);
                            wet = at != nullptr &&
                                  (*at == "minecraft:water" || *at == "minecraft:ice" ||
                                   *at == "minecraft:seagrass" || *at == "minecraft:kelp_plant");
                        }
                        map[key(world_x, world_z)] = Column{theirs, ours, !wet};
                    }
                }
                ++read_chunks;
            }
        }

        f64   sum_theirs = 0.0;
        f64   sum_ours   = 0.0;
        f64   sum_tt     = 0.0;
        f64   sum_oo     = 0.0;
        f64   sum_to     = 0.0;
        f64   sum_error  = 0.0;
        f64   sum_error2 = 0.0;
        usize dry        = 0;
        for (const auto& [at, column] : map) {
            if (!column.dry) {
                continue;
            }
            const auto t = static_cast<f64>(column.theirs);
            const auto o = static_cast<f64>(column.ours);
            sum_theirs += t;
            sum_ours += o;
            sum_tt += t * t;
            sum_oo += o * o;
            sum_to += t * o;
            sum_error += o - t;
            sum_error2 += (o - t) * (o - t);
            ++dry;
        }
        if (dry < 16) {
            OV_LOG_ERROR("only {} dry columns; raise --chunks", dry);
            return 1;
        }

        if (!options.dump.empty()) {
            std::ofstream out(options.dump);
            if (!out) {
                OV_LOG_ERROR("cannot write {}", options.dump);
                return 1;
            }
            out << "# x z dry theirs ours\n";
            for (const auto& [at, column] : map) {
                out << static_cast<i32>(at >> 32) << ' '
                    << static_cast<i32>(static_cast<u32>(at & 0xFFFFFFFF)) << ' '
                    << (column.dry ? 1 : 0) << ' ' << column.theirs << ' ' << column.ours << '\n';
            }
            OV_LOG_INFO("wrote {} columns to {}", map.size(), options.dump);
        }
        const auto n         = static_cast<f64>(dry);
        const f64  mean_t    = sum_theirs / n;
        const f64  mean_o    = sum_ours / n;
        const f64  var_t     = std::max(0.0, sum_tt / n - mean_t * mean_t);
        const f64  var_o     = std::max(0.0, sum_oo / n - mean_o * mean_o);
        const f64  covariance = sum_to / n - mean_t * mean_o;
        const f64  correlation =
            var_t > 0.0 && var_o > 0.0 ? covariance / std::sqrt(var_t * var_o) : 0.0;

        fmt::print("\nseed {}, {} chunks, {} columns, {} of them dry\n", options.seed,
                   read_chunks, map.size(), dry);
        fmt::print("\nsurface height over dry land\n");
        fmt::print("  {:>24} {:>10} {:>10}\n", "", "the game", "ours");
        fmt::print("  {:>24} {:>10.3f} {:>10.3f}\n", "mean", mean_t, mean_o);
        fmt::print("  {:>24} {:>10.3f} {:>10.3f}\n", "standard deviation", std::sqrt(var_t),
                   std::sqrt(var_o));
        fmt::print("  {:>24} {:>10.3f}\n", "mean error (ours-theirs)", sum_error / n);
        fmt::print("  {:>24} {:>10.3f}\n", "rms error", std::sqrt(sum_error2 / n));
        fmt::print("  {:>24} {:>10.4f}\n", "correlation", correlation);

        // The structure function: the mean squared difference between two
        // columns d apart. It is a spectrum read in the space domain, and it
        // is what separates a field that is merely too large from a field
        // missing its longest wavelengths — the first raises every d by the
        // same factor, the second only the large ones.
        constexpr std::array<i32, 12> kDistances{1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
        fmt::print("\nstructure function of the surface, rms height difference at distance d\n");
        fmt::print("  {:>4} {:>10} {:>10} {:>10} {:>10}\n", "d", "the game", "ours", "ratio",
                   "pairs");
        for (const i32 distance : kDistances) {
            f64   theirs_sum = 0.0;
            f64   ours_sum   = 0.0;
            usize pairs      = 0;
            for (const auto& [at, column] : map) {
                if (!column.dry) {
                    continue;
                }
                const auto x = static_cast<i32>(at >> 32);
                const auto z = static_cast<i32>(static_cast<u32>(at & 0xFFFFFFFF));
                for (const auto& [dx, dz] :
                     std::array<std::array<i32, 2>, 2>{{{distance, 0}, {0, distance}}}) {
                    const auto found = map.find(key(x + dx, z + dz));
                    if (found == map.end() || !found->second.dry) {
                        continue;
                    }
                    const auto dt =
                        static_cast<f64>(found->second.theirs - column.theirs);
                    const auto doo = static_cast<f64>(found->second.ours - column.ours);
                    theirs_sum += dt * dt;
                    ours_sum += doo * doo;
                    ++pairs;
                }
            }
            if (pairs == 0) {
                continue;
            }
            const f64 their_rms = std::sqrt(theirs_sum / static_cast<f64>(pairs));
            const f64 our_rms   = std::sqrt(ours_sum / static_cast<f64>(pairs));
            fmt::print("  {:>4} {:>10.3f} {:>10.3f} {:>10.3f} {:>10}\n", distance, their_rms,
                       our_rms, their_rms > 0.0 ? our_rms / their_rms : 0.0, pairs);
        }

        // And the same reading of the noise itself, with no reference world
        // involved. It says which wavelengths this build's base_3d_noise
        // actually carries, so that the surface figures above can be read as a
        // consequence rather than a coincidence.
        if (const auto* base = router->function("minecraft:overworld/base_3d_noise")) {
            fmt::print("\nbase_3d_noise itself, rms difference at distance d along x (y = 64)\n");
            fmt::print("  {:>4} {:>12}\n", "d", "rms");
            f64   noise_sum2 = 0.0;
            usize noise_n    = 0;
            for (i32 x = -1024; x <= 1024; x += 7) {
                for (i32 z = -1024; z <= 1024; z += 11) {
                    const f64 value = base->compute({x, 64, z});
                    noise_sum2 += value * value;
                    ++noise_n;
                }
            }
            fmt::print("  {:>4} {:>12.6f}   (rms of the field itself)\n", 0,
                       std::sqrt(noise_sum2 / static_cast<f64>(noise_n)));
            for (const i32 distance : kDistances) {
                f64   sum2  = 0.0;
                usize count = 0;
                for (i32 x = -1024; x <= 1024; x += 7) {
                    for (i32 z = -1024; z <= 1024; z += 11) {
                        const f64 difference =
                            base->compute({x + distance, 64, z}) - base->compute({x, 64, z});
                        sum2 += difference * difference;
                        ++count;
                    }
                }
                fmt::print("  {:>4} {:>12.6f}\n", distance,
                           std::sqrt(sum2 / static_cast<f64>(count)));
            }
        }
        return 0;
    }

    std::map<std::string, std::array<i32, 4>>                   located;
    std::vector<std::pair<worldgen::ClimatePoint, std::string>> recorded;
    // The terrain comparison needs the block registry, which the biome one
    // does not.
    std::optional<registry::BlockRegistry> pack;
    std::optional<worldgen::ChunkGenerator> generator;
    if (options.terrain) {
        auto loaded = registry::BlockRegistry::load(options.pack);
        if (!loaded) {
            OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
            return 1;
        }
        pack.emplace(std::move(*loaded));
        generator.emplace(*router, *biomes, *pack);
    }
    // ── carvers ─────────────────────────────────────────────────────────────
    // The mask is a pure function of the seed and the chunk, so it is computed
    // here rather than through the generator: nothing else about the chunk has
    // to exist first, and the comparison stays a comparison of the noise plus
    // exactly one more stage.
    const worldgen::CarvingContext carving_context{router->min_y(), router->height()};
    const worldgen::CarverStage    carver_stage{options.seed, carving_context};
    worldgen::CarvingMask carved{carving_context.min_y, carving_context.height};
    // ── end carvers ─────────────────────────────────────────────────────────
    /// Solid where the game is solid, and the two ways of being wrong. They
    /// mean different things: stone where the game has air is a cave nobody
    /// carved, and air where the game has stone would be the noise itself
    /// being wrong.
    usize blocks_seen = 0;
    usize blocks_agreed = 0;
    usize extra_solid = 0;
    usize missing_solid = 0;
    /// Disagreements by height, in bands of 32. Where they sit says what they
    /// are: near the surface they are trees and the surface rules, deep down
    /// they are caves nobody carved.
    /// What the blocks actually are. Guessing at them cost three rounds of
    /// excluding the wrong things.
    std::map<std::string, usize> extra_names;
    std::map<std::string, usize> missing_names;
    /// Our surface height minus theirs, from eight below to eight above.
    std::vector<std::string> probes;
    std::array<usize, 17>    height_delta{};
    std::array<usize, 10> extra_by_band{};
    std::array<usize, 10> missing_by_band{};
    /// The same surface offset, counted only over columns the game left dry.
    /// An ocean column's offset is dominated by the sea bed, which the
    /// aquifer and not the noise decides; mixing the two hides whatever the
    /// land is doing.
    std::array<usize, 17> land_delta{};
    usize                 land_columns = 0;
    /// Agreement if our density were read `kShift` blocks higher or lower.
    /// This is the test that tells a *translated* field from a merely wrong
    /// one: a genuine two-block offset shows as a peak at +2 here, and a
    /// field of the wrong shape shows as a flat curve with its maximum at 0.
    constexpr i32                 kShiftRange = 4;
    std::array<usize, 2 * kShiftRange + 1> shift_agreed{};
    /// The mean surface error against the position within an interpolation
    /// cell, which is eight blocks tall and four wide.
    ///
    /// This is the test for a misaligned cell grid, and it is a sharp one: if
    /// our corners sat half a cell away from the game's, the error would rise
    /// and fall with the height's position inside the cell. A flat row here
    /// says the grid is aligned and the error is in the values, not in where
    /// they are sampled.
    std::array<i64, 8> delta_by_cell_y{};
    std::array<i64, 8> count_by_cell_y{};
    std::array<i64, 4> delta_by_cell_x{};
    std::array<i64, 4> count_by_cell_x{};

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
    /// Cells and agreements per biome, as the game named it.
    std::map<std::string, std::array<i64, 2>> per_biome;
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

            // The surface height, for --locate. Without it a camera placed by
            // "highest non-air near the middle" ends up inside a tree, or on
            // the sea bed with the whole frame under water.
            std::array<i32, 256> surface{};
            surface.fill(64);
            if (const nbt::Tag* maps = document->root.find("Heightmaps")) {
                if (const nbt::Tag* world_surface = maps->find("WORLD_SURFACE")) {
                    if (const auto* longs = world_surface->get_if<nbt::Tag::LongArray>()) {
                        // Nine bits an entry, seven to a long, and the value is
                        // measured from the bottom of the world.
                        for (usize column = 0; column < 256; ++column) {
                            const usize word = column / 7;
                            const usize slot = (column % 7) * 9;
                            if (word < longs->size()) {
                                surface[column] =
                                    static_cast<i32>((static_cast<u64>((*longs)[word]) >> slot) &
                                                     0x1FFU) -
                                    64;
                            }
                        }
                    }
                }
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

            if (options.terrain) {
                if (options.carvers) {
                    carver_stage.carve_into(chunk_x, chunk_z, carved);
                }
                // Only the columns, not every block: a chunk is 98304 blocks
                // and the density graph is not cheap. Every fourth column in
                // each direction is 1024 blocks a chunk, which settles the
                // question without taking an hour.
                // Every third column rather than every fourth, and the three
                // is the point: the interpolation cell is four wide, so a
                // stride of four only ever lands on x % 4 == 0 and the test
                // for a misaligned horizontal grid has nothing to compare.
                // Three visits all four residues, and samples more besides.
                for (i32 sample_z = 0; sample_z < 16; sample_z += 3) {
                    for (i32 sample_x = 0; sample_x < 16; sample_x += 3) {
                        const i32 world_x = chunk_x * 16 + sample_x;
                        const i32 world_z = chunk_z * 16 + sample_z;

                        // The surface height on each side, block by block. A
                        // constant offset here is a bug in the density; a
                        // scattered one is roughness the interpolation is
                        // meant to smooth. The two look identical in a
                        // percentage.
                        i32 their_top = -65;
                        i32 our_top   = -65;
                        for (i32 probe = 200; probe > -64; --probe) {
                            if (their_top == -65) {
                                const auto* at = block_at(*document, sample_x, probe, sample_z);
                                if (at != nullptr && is_solid_name(*at)) {
                                    their_top = probe;
                                }
                            }
                            if (our_top == -65 &&
                                generator->is_solid(world_x, probe, world_z)) {
                                our_top = probe;
                            }
                            if (their_top != -65 && our_top != -65) {
                                break;
                            }
                        }
                        // Dry or not: whether the game put water anywhere in
                        // the eight blocks above what it called the surface.
                        bool wet = false;
                        for (i32 above = 1; above <= 8 && !wet; ++above) {
                            const auto* at =
                                block_at(*document, sample_x, their_top + above, sample_z);
                            wet = at != nullptr && (*at == "minecraft:water" ||
                                                    *at == "minecraft:ice" ||
                                                    *at == "minecraft:seagrass" ||
                                                    *at == "minecraft:kelp_plant");
                        }
                        if (their_top != -65 && our_top != -65) {
                            const auto slot = static_cast<usize>(
                                std::clamp(our_top - their_top + 8, 0, 16));
                            ++height_delta[slot];
                            if (!wet) {
                                ++land_delta[slot];
                                ++land_columns;
                                // Measured from the bottom of the world, which
                                // is where the cell grid is anchored.
                                const auto cell_y =
                                    static_cast<usize>(((their_top + 64) % 8 + 8) % 8);
                                delta_by_cell_y[cell_y] += our_top - their_top;
                                ++count_by_cell_y[cell_y];
                                const auto cell_x = static_cast<usize>((world_x % 4 + 4) % 4);
                                delta_by_cell_x[cell_x] += our_top - their_top;
                                ++count_by_cell_x[cell_x];
                            }
                            // How far off the density actually is at the block
                            // the game called the surface. A magnitude tells
                            // apart "a constant is missing" from "the field is
                            // the wrong shape".
                            if (probes.size() < 12 && our_top < their_top) {
                                probes.push_back(fmt::format(
                                    "  ({:>7},{:>7}) game {:>4}, ours {:>4}, density there "
                                    "{:+.5f}, one below {:+.5f}",
                                    world_x, world_z, their_top, our_top,
                                    generator->density_at(world_x, their_top, world_z),
                                    generator->density_at(world_x, their_top - 1, world_z)));
                            }
                        }
                        for (i32 height = -64; height < 256; height += 2) {
                            const auto* named =
                                block_at(*document, sample_x, height, sample_z);
                            if (named == nullptr) {
                                continue;
                            }
                            const bool their_solid = is_solid_name(*named);
                            const bool our_solid =
                                generator->is_solid(world_x, height, world_z) &&
                                !(options.carvers && carved.get(sample_x, height, sample_z));
                            for (i32 shift = -kShiftRange; shift <= kShiftRange; ++shift) {
                                const bool shifted =
                                    generator->is_solid(world_x, height + shift, world_z) &&
                                    !(options.carvers &&
                                      carved.get(sample_x, height + shift, sample_z));
                                if (shifted == their_solid) {
                                    ++shift_agreed[static_cast<usize>(shift + kShiftRange)];
                                }
                            }
                            ++blocks_seen;
                            if (their_solid == our_solid) {
                                ++blocks_agreed;
                            } else {
                                const auto band = static_cast<usize>(
                                    std::clamp((height + 64) / 32, 0, 9));
                                if (our_solid) {
                                    ++extra_solid;
                                    ++extra_by_band[band];
                                    extra_names[std::string(*named)] += 1;
                                } else {
                                    ++missing_solid;
                                    ++missing_by_band[band];
                                    missing_names[std::string(*named)] += 1;
                                }
                            }
                        }
                    }
                }
                ++chunks;
                continue;
            }

            // One cache per chunk. Vanilla's lives in a thread local and so
            // carries between chunks in whatever order the generation pool
            // ran them; within a chunk the order is fixed, and after the first
            // few cells the earlier state has washed out.
            worldgen::BiomeSearchCache cache;
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

                for (u32 slot = 0; slot < 64; ++slot) {
                    // The order the cells are *asked about*, which is not the
                    // order they are stored in. Storage is y, z, x; the fill
                    // loop that produced them may not be, and the cache makes
                    // the difference visible.
                    // slot = x * 16 + y * 4 + z when the loop nests x, y, z;
                    // storage is cell = y * 16 + z * 4 + x.
                    const u32 cell = options.cache == "xyz"
                                         ? (((slot / 4) % 4) * 16 + (slot % 4) * 4 + (slot / 16))
                                         : slot;
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


    if (options.locate) {
                        // Both heights are reported: the cell's own, and the
                        // surface above it. A surface biome wants the second; a
                        // cave biome only exists at the first, and pointing a
                        // camera at the sky above a lush cave frames a forest.
                        if (quart_y * 4 > 200) {
                            continue;
                        }
                        const std::string name(names[palette_index]);
                        if (!located.contains(name)) {
                            const usize column =
                                static_cast<usize>((cz * 4) * 16 + (cx * 4));
                            located.emplace(name,
                                            std::array<i32, 4>{quart_x * 4, quart_z * 4,
                                                               surface[column & 255], quart_y * 4});
                        }
                        ++cells;
                        continue;
                    }

                    const auto climate = biomes->sample(*router, quart_x, quart_y, quart_z);

    if (!options.sweep.empty()) {
                        // Keep the reading and the game's answer; the sweep
                        // re-scores them without resampling the noise, which is
                        // what makes trying thirty distortions cheap.
                        recorded.emplace_back(climate, std::string(names[palette_index]));
                        ++cells;
                        continue;
                    }
                    const auto ours = options.cache == "none"
                                          ? biomes->biome_at(climate)
                                          : biomes->biome_at(climate, cache);

                    ++cells;
                    for (usize axis = 0; axis < 7; ++axis) {
                        lowest[axis]  = std::min(lowest[axis], climate.coordinates[axis]);
                        highest[axis] = std::max(highest[axis], climate.coordinates[axis]);
                    }
                    theirs[names[palette_index]] += 1;
                    {
                        auto& row = per_biome[std::string(names[palette_index])];
                        row[0] += 1;
                        row[1] += (ours == names[palette_index]) ? 1 : 0;
                    }
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

                    if (options.terrain) {
        fmt::print("\nseed {}, {} chunks, {} blocks sampled\n", options.seed, chunks,
                   blocks_seen);
        fmt::print("solid/not agreed: {} / {}  ({:.3f} %)\n", blocks_agreed, blocks_seen,
                   100.0 * static_cast<f64>(blocks_agreed) / static_cast<f64>(blocks_seen));
        fmt::print("  stone where the game has none: {:>9}  ({:.3f} %)   <- caves and aquifers\n",
                   extra_solid, 100.0 * static_cast<f64>(extra_solid) /
                                    static_cast<f64>(blocks_seen));
        fmt::print("  none where the game has stone: {:>9}  ({:.3f} %)   <- the noise itself\n",
                   missing_solid, 100.0 * static_cast<f64>(missing_solid) /
                                      static_cast<f64>(blocks_seen));
        fmt::print("\nby height:\n  {:>12} {:>12} {:>12}\n", "y", "extra", "missing");
        for (usize band = 0; band < extra_by_band.size(); ++band) {
            if (extra_by_band[band] == 0 && missing_by_band[band] == 0) {
                continue;
            }
            fmt::print("  {:>5} .. {:>4} {:>12} {:>12}\n", static_cast<i32>(band) * 32 - 64,
                       static_cast<i32>(band) * 32 - 33, extra_by_band[band],
                       missing_by_band[band]);
        }
        const auto census = [](std::string_view title, const std::map<std::string, usize>& names) {
            std::vector<std::pair<std::string, usize>> sorted(names.begin(), names.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            fmt::print("\n{}\n", title);
            for (usize i = 0; i < sorted.size() && i < 10; ++i) {
                fmt::print("  {:>8}  {}\n", sorted[i].second, sorted[i].first);
            }
        };
        fmt::print("\nour surface height minus the game's:\n");
        usize columns = 0;
        for (const usize count : height_delta) {
            columns += count;
        }
        for (usize slot = 0; slot < height_delta.size(); ++slot) {
            if (height_delta[slot] == 0) {
                continue;
            }
            fmt::print("  {:>+3} {:>8}  ({:>6.2f} %)\n", static_cast<i32>(slot) - 8,
                       height_delta[slot],
                       100.0 * static_cast<f64>(height_delta[slot]) /
                           static_cast<f64>(columns));
        }
        fmt::print("\nthe same, over the {} columns the game left dry:\n", land_columns);
        for (usize slot = 0; slot < land_delta.size(); ++slot) {
            if (land_delta[slot] == 0) {
                continue;
            }
            fmt::print("  {:>+3} {:>8}  ({:>6.2f} %)\n", static_cast<i32>(slot) - 8,
                       land_delta[slot],
                       100.0 * static_cast<f64>(land_delta[slot]) /
                           static_cast<f64>(land_columns));
        }
        fmt::print("\nmean surface error against the height's place in the 8-block cell:\n");
        for (usize slot = 0; slot < delta_by_cell_y.size(); ++slot) {
            if (count_by_cell_y[slot] == 0) {
                continue;
            }
            fmt::print("  y%8 == {} {:>+8.3f}  ({} columns)\n", slot,
                       static_cast<f64>(delta_by_cell_y[slot]) /
                           static_cast<f64>(count_by_cell_y[slot]),
                       count_by_cell_y[slot]);
        }
        fmt::print("\nthe same against x within the 4-block cell:\n");
        for (usize slot = 0; slot < delta_by_cell_x.size(); ++slot) {
            if (count_by_cell_x[slot] == 0) {
                continue;
            }
            fmt::print("  x%4 == {} {:>+8.3f}  ({} columns)\n", slot,
                       static_cast<f64>(delta_by_cell_x[slot]) /
                           static_cast<f64>(count_by_cell_x[slot]),
                       count_by_cell_x[slot]);
        }
        fmt::print("\nagreement if our density were read n blocks off:\n");
        for (usize slot = 0; slot < shift_agreed.size(); ++slot) {
            fmt::print("  {:>+3} {:>8}  ({:>7.3f} %)\n",
                       static_cast<i32>(slot) - kShiftRange, shift_agreed[slot],
                       100.0 * static_cast<f64>(shift_agreed[slot]) /
                           static_cast<f64>(blocks_seen));
        }
        fmt::print("\nthe density where the game's surface is:\n");
        for (const auto& line : probes) {
            fmt::print("{}\n", line);
        }
        census("what the game had where we had none:", missing_names);
        census("what the game had where we had stone:", extra_names);
        return 0;
    }

    if (options.locate) {
        for (const auto& [name, where] : located) {
            fmt::print("{} {} {} {} {}\n", name, where[0], where[1], where[2], where[3]);
        }
        return 0;
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

    // Per biome, because an average hides everything that matters: one biome
    // at 60 % inside a world that is 99 % right is a real bug, and the total
    // would never show it.
    fmt::print("\nper biome, as the game named them:\n");
    fmt::print("  {:<36} {:>9} {:>9} {:>8}\n", "biome", "cells", "agreed", "");
    std::vector<std::pair<std::string, std::array<i64, 2>>> rows(per_biome.begin(),
                                                                 per_biome.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second[0] > b.second[0]; });
    for (const auto& [name, row] : rows) {
        const f64 rate = 100.0 * static_cast<f64>(row[1]) / static_cast<f64>(row[0]);
        fmt::print("  {:<36} {:>9} {:>9} {:>7.3f} %{}\n", name, row[0], row[1], rate,
                   row[1] == row[0] ? "" : "   <-");
    }
    fmt::print("\n{} of the {} biomes in the table were seen\n", rows.size(),
               biomes->biome_count());

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
