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
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/chunk_generator.hpp"
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
    /// Print one representative surface position per biome instead of
    /// comparing. What the renderer needs to be pointed at.
    bool locate{false};
    /// Compare blocks rather than biomes.
    bool terrain{false};
    /// Dump one column: what the game has, and every term we compute.
    std::string column;
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
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
        } else if (argument.starts_with("--column=")) {
            options.column = value("--column=");
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=");
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
                // Only the columns, not every block: a chunk is 98304 blocks
                // and the density graph is not cheap. Every fourth column in
                // each direction is 1024 blocks a chunk, which settles the
                // question without taking an hour.
                for (i32 sample_z = 0; sample_z < 16; sample_z += 4) {
                    for (i32 sample_x = 0; sample_x < 16; sample_x += 4) {
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
                        if (their_top != -65 && our_top != -65) {
                            const auto slot = static_cast<usize>(
                                std::clamp(our_top - their_top + 8, 0, 16));
                            ++height_delta[slot];
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
                                generator->is_solid(world_x, height, world_z);
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
                    const auto ours = biomes->biome_at(climate);

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
