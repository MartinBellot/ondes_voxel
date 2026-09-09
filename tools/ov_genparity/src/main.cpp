// The ores in a world *we* generated, against the ores the game generated.
//
// This asks a different question from `ov_features`, and the difference is the
// whole reason it exists.
//
// `ov_features` takes the game's own terrain as given: it reads a chunk the
// real server wrote, puts every ore back into the stone it replaced, runs our
// decoration over that, and compares. That isolates the placement from the
// terrain, and it is the right way to measure the placement — 99,509 % at the
// block, docs/provenance/features.md.
//
// It does not answer "does the world our pipeline produces contain the game's
// ores". A vein placed at exactly the right coordinate lands on nothing if our
// noise put air there, and `ov_features` cannot see that because it never uses
// our noise. So this harness generates the chunk end to end — noise, biomes,
// surface, carvers, features — through `ChunkPipeline`, and compares that
// world's ore blocks with the reference world's, position by position.
//
// The number it prints is **lower than `ov_features`' by construction**, and
// the gap between the two is not noise: it is what our terrain is still worth.
// Our surface sits one to eight blocks off the game's in nine columns out of
// ten (`ov_parity --terrain`: 15,9 % of columns at the right height), so an ore
// that our placement puts at exactly the game's coordinate frequently lands in
// air, in water, or in stone the carvers took away. Both numbers are reported
// side by side for that reason, and neither is the other's substitute.
//
// It also prints the count this whole layer was built for: how many blocks the
// decoration wrote outside the chunk being decorated. A single-chunk adaptor
// would report zero and look healthy.
#define OV_LOG_CATEGORY "genparity"

#include "ov/base/log.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/decoration.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/pipeline.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace ov;

namespace {

constexpr i32 kMinY   = -64;
constexpr i32 kHeight = 384;

struct Options {
    std::filesystem::path world{"run/reference-1234567890/world"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    std::filesystem::path registries{"data/vanilla/1.20.1/registry.ovpack"};
    i64                   seed{1234567890};
    i32                   chunks{24};
    i32                   per_region{4};
    /// Generate the terrain and stop before the features. The control arm: it
    /// says how many of the game's ores our terrain happens to hold *without*
    /// any decoration at all, which is the floor every other number sits on.
    bool no_features{false};
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
        } else if (argument.starts_with("--data=")) {
            options.data = value("--data=");
        } else if (argument.starts_with("--reports=")) {
            options.reports = value("--reports=");
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=");
        } else if (argument.starts_with("--registries=")) {
            options.registries = value("--registries=");
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=").c_str());
        } else if (argument.starts_with("--chunks=")) {
            options.chunks = std::atoi(value("--chunks=").c_str());
        } else if (argument.starts_with("--per-region=")) {
            options.per_region = std::atoi(value("--per-region=").c_str());
        } else if (argument == "--no-features") {
            options.no_features = true;
        }
    }
    return options;
}

/// The ores this comparison counts, and the stone each one replaced.
///
/// Only the ore blocks: the decorative stones (granite, diorite, andesite,
/// tuff) are placed by the same step and would triple the sample, but they are
/// also what `ore_dirt` and `ore_gravel` overwrite, and mixing them in would
/// make the number say something else. The list matches `ov_features`' so that
/// the two numbers are comparable, which is the point of printing both.
[[nodiscard]] bool is_ore(std::string_view name) {
    return name.ends_with("_ore");
}

struct ReferenceChunk {
    i32                      chunk_x{0};
    i32                      chunk_z{0};
    std::vector<std::string> names;

    [[nodiscard]] std::string_view block(i32 local_x, i32 y, i32 local_z) const {
        if (y < kMinY || y >= kMinY + kHeight) {
            return {};
        }
        const usize index = static_cast<usize>(y - kMinY) * 256 +
                            static_cast<usize>(local_z) * 16 + static_cast<usize>(local_x);
        return index < names.size() ? std::string_view(names[index]) : std::string_view();
    }
};

void unpack(const nbt::Tag* data, usize bits, usize cells, std::vector<usize>& out) {
    out.assign(cells, 0);
    if (bits == 0 || data == nullptr) {
        return;
    }
    const auto* longs = data->get_if<nbt::Tag::LongArray>();
    if (longs == nullptr) {
        return;
    }
    const usize per_word = 64 / bits;
    for (usize cell = 0; cell < cells; ++cell) {
        const usize word = cell / per_word;
        if (word >= longs->size()) {
            return;
        }
        out[cell] = static_cast<usize>(
            (static_cast<u64>((*longs)[word]) >> ((cell % per_word) * bits)) &
            ((1ULL << bits) - 1ULL));
    }
}

[[nodiscard]] bool decode(const nbt::Document& document, ReferenceChunk& chunk) {
    const nbt::Tag* x_pos = document.root.find("xPos");
    const nbt::Tag* z_pos = document.root.find("zPos");
    const nbt::Tag* list  = document.root.find("sections");
    if (x_pos == nullptr || z_pos == nullptr || list == nullptr ||
        list->type() != nbt::TagType::List) {
        return false;
    }
    chunk.chunk_x = static_cast<i32>(x_pos->as_i64());
    chunk.chunk_z = static_cast<i32>(z_pos->as_i64());
    chunk.names.assign(static_cast<usize>(kHeight) * 256, "minecraft:air");

    std::vector<usize> indices;
    for (const nbt::Tag& section : *list->list()) {
        const nbt::Tag* y_tag = section.find("Y");
        if (y_tag == nullptr) {
            continue;
        }
        const auto section_y = static_cast<i32>(y_tag->as_i64());
        if (section_y < (kMinY >> 4) || section_y >= ((kMinY + kHeight) >> 4)) {
            continue;
        }
        const nbt::Tag* states = section.find("block_states");
        if (states == nullptr) {
            continue;
        }
        const nbt::Tag* palette = states->find("palette");
        if (palette == nullptr || palette->type() != nbt::TagType::List ||
            palette->list()->empty()) {
            continue;
        }
        std::vector<std::string> names;
        for (const nbt::Tag& entry : *palette->list()) {
            const nbt::Tag* name = entry.find("Name");
            names.emplace_back(name == nullptr ? "minecraft:air" : name->as_string());
        }
        const usize bits =
            names.size() <= 1
                ? 0
                : std::max<usize>(4, static_cast<usize>(std::bit_width(names.size() - 1)));
        unpack(states->find("data"), bits, 4096, indices);
        for (usize cell = 0; cell < 4096; ++cell) {
            const usize which   = indices[cell] < names.size() ? indices[cell] : 0;
            const i32   local_x = static_cast<i32>(cell % 16);
            const i32   local_z = static_cast<i32>((cell / 16) % 16);
            const i32   y       = section_y * 16 + static_cast<i32>(cell / 256);
            chunk.names[static_cast<usize>(y - kMinY) * 256 + static_cast<usize>(local_z) * 16 +
                        static_cast<usize>(local_x)] = names[which];
        }
    }
    return true;
}

struct Tally {
    i64 theirs{0};
    i64 ours{0};
    i64 agreed{0};
};

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);

    if (!std::filesystem::is_directory(options.world / "region")) {
        OV_LOG_ERROR("{} has no region/. Generate one with scripts/reference_world.sh.",
                     options.world.string());
        return 1;
    }
    auto blocks = registry::BlockRegistry::load(options.pack);
    if (!blocks) {
        OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
        return 1;
    }
    auto registries = registry::Registries::load(options.registries);
    if (!registries) {
        OV_LOG_ERROR("registries {}: run tools/ov_datagen first", options.registries.string());
        return 1;
    }
    auto router = worldgen::NoiseRouter::load(options.data, "overworld", options.seed);
    if (!router) {
        OV_LOG_ERROR("router: {}", worldgen::to_string(router.error()));
        return 1;
    }
    auto biomes = worldgen::BiomeSource::load(options.reports, "overworld");
    if (!biomes) {
        OV_LOG_ERROR("biome source: {}", worldgen::to_string(biomes.error()));
        return 1;
    }
    auto surface = worldgen::SurfaceSystem::load(options.data, "overworld", options.seed, *blocks);
    if (!surface) {
        OV_LOG_ERROR("surface rules: {}", worldgen::to_string(surface.error()));
        return 1;
    }
    auto features = worldgen::FeatureRegistry::load(options.data, *blocks);
    if (!features) {
        OV_LOG_ERROR("features: {}", worldgen::to_string(features.error()));
        return 1;
    }
    auto decorator = worldgen::Decorator::load(options.data, *blocks, *features, *biomes);
    if (!decorator) {
        OV_LOG_ERROR("decorator: {}", worldgen::to_string(decorator.error()));
        return 1;
    }

    const worldgen::CarvingContext carving{router->min_y(), router->height()};
    const worldgen::CarverStage    carvers{options.seed, carving};

    worldgen::ChunkGenerator generator{*router, *biomes, *blocks};
    generator.set_surface_system(&*surface);
    if (auto attached = generator.set_carvers(&carvers, *registries); !attached) {
        OV_LOG_ERROR("carvers: {}", worldgen::to_string(attached.error()));
        return 1;
    }

    worldgen::ChunkPipeline pipeline{generator,
                                     options.no_features ? nullptr : &*decorator, *blocks,
                                     world::WorldShape::overworld(), options.seed};

    std::map<std::string, Tally> tallies;
    /// For every ore of theirs we do not have, what our generated world holds
    /// there. This is the diagnostic that separates "the vein went elsewhere"
    /// from "the vein is right and our terrain is not there".
    std::map<std::string, i64> instead;

    usize chunks_done = 0;
    usize skipped     = 0;

    // Wall-clock, and only for the cost line: nothing in the generation reads
    // it, which is the rule (CLAUDE.md principle 5).
    const auto started = std::chrono::steady_clock::now();

    for (const auto& entry : std::filesystem::directory_iterator(options.world / "region")) {
        if (chunks_done >= static_cast<usize>(options.chunks)) {
            break;
        }
        if (entry.path().extension() != ".mca") {
            continue;
        }
        auto region = nbt::RegionFile::open(entry.path());
        if (!region) {
            continue;
        }
        i32 from_this_region = 0;
        for (u32 index = 0; index < 1024 && chunks_done < static_cast<usize>(options.chunks) &&
                            from_this_region < options.per_region;
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
            // Only finished chunks. A chunk the game stopped partway has a
            // biome array full of `plains` and features that never ran, and it
            // lies in both directions.
            const nbt::Tag* status = document->root.find("Status");
            if (status == nullptr || status->as_string() != "minecraft:full") {
                continue;
            }
            ReferenceChunk theirs;
            if (!decode(*document, theirs)) {
                ++skipped;
                continue;
            }

            // Driven to `Full`, which means every one of the eight neighbours
            // has run its own features: the last write that can land in this
            // chunk has landed. Read by reference rather than taken, so the
            // cache keeps it for the neighbours still to come.
            const world::Chunk& ours =
                pipeline.promote(theirs.chunk_x, theirs.chunk_z, worldgen::ChunkStatus::Full);

            for (i32 y = kMinY; y < kMinY + kHeight; ++y) {
                for (i32 z = 0; z < 16; ++z) {
                    for (i32 x = 0; x < 16; ++x) {
                        const std::string_view their_name = theirs.block(x, y, z);
                        const auto             our_state =
                            ours.get_block(static_cast<usize>(x), y, static_cast<usize>(z));
                        const std::string_view our_name =
                            blocks->block_name(blocks->block_of(our_state));

                        const bool their_ore = is_ore(their_name);
                        const bool our_ore   = is_ore(our_name);
                        if (!their_ore && !our_ore) {
                            continue;
                        }
                        if (their_ore) {
                            Tally& tally = tallies[std::string(their_name)];
                            ++tally.theirs;
                            if (their_name == our_name) {
                                ++tally.agreed;
                            } else {
                                instead[std::string(our_name)] += 1;
                            }
                        }
                        if (our_ore) {
                            ++tallies[std::string(our_name)].ours;
                        }
                    }
                }
            }
            ++chunks_done;
            ++from_this_region;
            // The chunk is finished and compared; nothing else needs it, and
            // its 5x5 support can go with it once we move on.
            pipeline.trim(theirs.chunk_x, theirs.chunk_z, 3);
        }
    }

    const auto elapsed = std::chrono::duration<f64>(std::chrono::steady_clock::now() - started);

    fmt::print("\nseed {}, {} full chunks generated end to end ({} unreadable)\n", options.seed,
               chunks_done, skipped);
    fmt::print("terrain: noise, biomes, surface, carvers{}\n",
               options.no_features ? "  (features OFF — control arm)" : ", features");

    i64 all_theirs = 0;
    i64 all_ours   = 0;
    i64 all_agreed = 0;
    fmt::print("\n{:<34} {:>9} {:>9} {:>9} {:>9}\n", "ore", "theirs", "ours", "same block", "");
    for (const auto& [name, tally] : tallies) {
        all_theirs += tally.theirs;
        all_ours += tally.ours;
        all_agreed += tally.agreed;
        const f64 rate = tally.theirs == 0 ? 0.0
                                           : 100.0 * static_cast<f64>(tally.agreed) /
                                                 static_cast<f64>(tally.theirs);
        fmt::print("{:<34} {:>9} {:>9} {:>9} {:>7.3f} %\n", name, tally.theirs, tally.ours,
                   tally.agreed, rate);
    }
    fmt::print("{:<34} {:>9} {:>9} {:>9} {:>7.3f} %\n", "all", all_theirs, all_ours, all_agreed,
               all_theirs == 0 ? 0.0
                               : 100.0 * static_cast<f64>(all_agreed) /
                                     static_cast<f64>(all_theirs));

    if (!instead.empty()) {
        fmt::print("\nwhat our generated world holds where one of their ores is:\n");
        std::vector<std::pair<std::string, i64>> ranked(instead.begin(), instead.end());
        std::ranges::sort(ranked, [](const auto& a, const auto& b) { return a.second > b.second; });
        for (usize i = 0; i < ranked.size() && i < 12; ++i) {
            fmt::print("  {:<38} {:>8}\n", ranked[i].first, ranked[i].second);
        }
    }

    const auto& stats = pipeline.stats();
    fmt::print("\nthe pipeline\n");
    fmt::print("  chunks driven to each status: biomes {}, noise {}, surface {}, carvers {},"
               " features {}, full {}\n",
               stats.reached[static_cast<usize>(worldgen::ChunkStatus::Biomes)],
               stats.reached[static_cast<usize>(worldgen::ChunkStatus::Noise)],
               stats.reached[static_cast<usize>(worldgen::ChunkStatus::Surface)],
               stats.reached[static_cast<usize>(worldgen::ChunkStatus::Carvers)],
               stats.reached[static_cast<usize>(worldgen::ChunkStatus::Features)],
               stats.reached[static_cast<usize>(worldgen::ChunkStatus::Full)]);
    fmt::print("  decorations run: {} (one per chunk, not nine)\n", stats.decorations);
    fmt::print("  feature writes: {}\n", stats.feature_writes);
    fmt::print("  of those, outside the chunk being decorated and KEPT: {} ({:.3f} %)\n",
               stats.border_writes,
               stats.feature_writes == 0 ? 0.0
                                         : 100.0 * static_cast<f64>(stats.border_writes) /
                                               static_cast<f64>(stats.feature_writes));
    fmt::print("  writes that left the 3x3 entirely and were dropped: {}\n", stats.dropped_writes);
    fmt::print("  peak chunks resident: {}, cache now {} chunks / {:.1f} MiB\n",
               stats.peak_resident, stats.resident,
               static_cast<f64>(pipeline.footprint_bytes()) / (1024.0 * 1024.0));
    fmt::print("  {:.3f} s for {} compared chunks — {:.3f} s per compared chunk\n",
               elapsed.count(), chunks_done,
               chunks_done == 0 ? 0.0 : elapsed.count() / static_cast<f64>(chunks_done));

    return 0;
}
