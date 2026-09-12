// End parity: our End against an End the real 1.20.1 server generated.
//
// Four questions, each against the artefact the game keeps for it:
//
//   * **biomes**, cell by cell on the 4x4x4 grid, in every chunk the game took
//     at least to `minecraft:biomes` (a chunk below that holds the default
//     array and lies — pitfall 4 of the briefing). The End's biome is a rule
//     over the island noise, so this is also a reading of that noise at every
//     chunk centre beyond 1024 blocks;
//   * **blocks**, name by name, in chunks the game stopped before the
//     features (`noise`, `surface`, `carvers` — the End has no carver). Such a
//     chunk is exactly what `ChunkGenerator::generate()` produces, and the
//     comparison goes through `generate()` itself (pitfall 13);
//   * **finished chunks**, through the pipeline and the End's decorator: the
//     spikes, the small islands, the chorus, the gateways;
//   * **the spikes** one by one: where the game's obsidian pillar stands, how
//     high, whether it is caged, against `end_spikes(seed)` — and the End
//     crystals the game saved in `entities/`, which our worldgen cannot place.
//
// See docs/provenance/end.md for the numbers.
#define OV_LOG_CATEGORY "endparity"

#include "ov/base/log.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/decoration.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/end.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/pipeline.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ov;

namespace {

constexpr i32 kMinY   = 0;
constexpr i32 kHeight = 256;

struct Options {
    std::filesystem::path world{".scratch/reference-end-1234567890/world/DIM1"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    i64                   seed{1234567890};
    /// Pre-feature chunks to run through generate().
    i32 chunks{200};
    /// Chunks of any finished-enough status to compare biomes on.
    i32 biome_chunks{100000};
    /// `minecraft:full` chunks to run through the pipeline with the decorator.
    i32 full_chunks{0};
    /// Only chunks whose centre is at least this far from the origin, in
    /// blocks — the outer islands on their own.
    i32 min_distance{0};
    i32 show{8};
    /// Only finished chunks where the game has something besides air and end
    /// stone: the spikes, the chorus, the gateways, the islands' features. A
    /// sample of the first chunks in file order is mostly empty void.
    bool interesting{false};
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        const auto value = [&](std::string_view prefix) { return argument.substr(prefix.size()); };
        if (argument.starts_with("--world=")) {
            options.world = value("--world=");
        } else if (argument.starts_with("--data=")) {
            options.data = value("--data=");
        } else if (argument.starts_with("--reports=")) {
            options.reports = value("--reports=");
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=");
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=").c_str());
        } else if (argument.starts_with("--chunks=")) {
            options.chunks = std::atoi(value("--chunks=").c_str());
        } else if (argument.starts_with("--biome-chunks=")) {
            options.biome_chunks = std::atoi(value("--biome-chunks=").c_str());
        } else if (argument.starts_with("--full=")) {
            options.full_chunks = std::atoi(value("--full=").c_str());
        } else if (argument.starts_with("--min-distance=")) {
            options.min_distance = std::atoi(value("--min-distance=").c_str());
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=").c_str());
        } else if (argument == "--interesting") {
            options.interesting = true;
        }
    }
    return options;
}

/// One chunk of the reference End, decoded.
struct ReferenceChunk {
    i32                      chunk_x{0};
    i32                      chunk_z{0};
    std::string              status;
    std::vector<std::string> names;       ///< [(y - kMinY) * 256 + z * 16 + x]
    std::vector<std::string> properties;  ///< the same cells: "k=v,k=v" or ""
    std::vector<std::string> biomes;      ///< [(qy * 4 + qz) * 4 + qx]

    [[nodiscard]] usize cell(i32 x, i32 y, i32 z) const {
        return static_cast<usize>(y - kMinY) * 256 + static_cast<usize>(z) * 16 +
               static_cast<usize>(x);
    }
    [[nodiscard]] const std::string& block(i32 x, i32 y, i32 z) const { return names[cell(x, y, z)]; }
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
        out[cell] = static_cast<usize>((static_cast<u64>((*longs)[word]) >>
                                        ((cell % per_word) * bits)) &
                                       ((1ULL << bits) - 1));
    }
}

[[nodiscard]] bool decode(const nbt::Document& document, ReferenceChunk& chunk, bool blocks) {
    const nbt::Tag* x_pos  = document.root.find("xPos");
    const nbt::Tag* z_pos  = document.root.find("zPos");
    const nbt::Tag* status = document.root.find("Status");
    const nbt::Tag* list   = document.root.find("sections");
    if (x_pos == nullptr || z_pos == nullptr || status == nullptr || list == nullptr ||
        list->type() != nbt::TagType::List) {
        return false;
    }
    chunk.chunk_x = static_cast<i32>(x_pos->as_i64());
    chunk.chunk_z = static_cast<i32>(z_pos->as_i64());
    chunk.status  = std::string(status->as_string());
    chunk.biomes.assign(static_cast<usize>(kHeight / 4) * 16, "");
    chunk.names.clear();
    chunk.properties.clear();
    if (blocks) {
        chunk.names.assign(static_cast<usize>(kHeight) * 256, "minecraft:air");
        chunk.properties.assign(static_cast<usize>(kHeight) * 256, "");
    }

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
        if (const nbt::Tag* states = section.find("block_states"); states != nullptr && blocks) {
            const nbt::Tag* palette = states->find("palette");
            if (palette != nullptr && palette->type() == nbt::TagType::List &&
                !palette->list()->empty()) {
                std::vector<std::string> names;
                std::vector<std::string> props;
                for (const nbt::Tag& entry : *palette->list()) {
                    const nbt::Tag* name = entry.find("Name");
                    names.emplace_back(name == nullptr ? "minecraft:air" : name->as_string());
                    std::string text;
                    if (const nbt::Tag* p = entry.find("Properties");
                        p != nullptr && p->type() == nbt::TagType::Compound) {
                        std::map<std::string, std::string> sorted;
                        for (const auto& [key, value] : *p->compound()) {
                            sorted.emplace(key, std::string(value.as_string()));
                        }
                        for (const auto& [key, value] : sorted) {
                            text += (text.empty() ? "" : ",") + key + "=" + value;
                        }
                    }
                    props.push_back(std::move(text));
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
                    const usize at      = chunk.cell(local_x, y, local_z);
                    chunk.names[at]      = names[which];
                    chunk.properties[at] = props[which];
                }
            }
        }
        if (const nbt::Tag* biomes = section.find("biomes")) {
            const nbt::Tag* palette = biomes->find("palette");
            if (palette != nullptr && palette->type() == nbt::TagType::List &&
                !palette->list()->empty()) {
                std::vector<std::string> names;
                for (const nbt::Tag& entry : *palette->list()) {
                    names.emplace_back(entry.as_string());
                }
                const usize bits =
                    names.size() <= 1 ? 0 : static_cast<usize>(std::bit_width(names.size() - 1));
                unpack(biomes->find("data"), bits, 64, indices);
                for (usize cell = 0; cell < 64; ++cell) {
                    const usize which = indices[cell] < names.size() ? indices[cell] : 0;
                    const i32   qx    = static_cast<i32>(cell % 4);
                    const i32   qz    = static_cast<i32>((cell / 4) % 4);
                    const i32   qy    = section_y * 4 + static_cast<i32>(cell / 16) - (kMinY >> 2);
                    chunk.biomes[(static_cast<usize>(qy) * 4 + static_cast<usize>(qz)) * 4 +
                                 static_cast<usize>(qx)] = names[which];
                }
            }
        }
    }
    return true;
}

[[nodiscard]] std::string_view canonical(std::string_view name) {
    return name == "minecraft:void_air" || name == "minecraft:cave_air" ? "minecraft:air" : name;
}

/// Statuses at or past `minecraft:biomes`: the biome array is the game's.
[[nodiscard]] bool biomes_final(std::string_view status) {
    return status != "minecraft:empty" && status != "minecraft:structure_starts" &&
           status != "minecraft:structure_references";
}

/// Before the features: what `generate()` produces, no more.
[[nodiscard]] bool pre_features(std::string_view status) {
    return status == "minecraft:noise" || status == "minecraft:surface" ||
           status == "minecraft:carvers";
}

struct Count {
    usize compared{0};
    usize agreed{0};
};

[[nodiscard]] f64 percent(usize part, usize whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<f64>(part) / static_cast<f64>(whole);
}

[[nodiscard]] std::string state_properties(const registry::BlockRegistry& blocks,
                                           registry::BlockStateId          state) {
    std::map<std::string, std::string> sorted;
    for (const auto& property : blocks.properties(blocks.block_of(state))) {
        sorted.emplace(std::string(property.name),
                       std::string(blocks.property_value(state, property)));
    }
    std::string text;
    for (const auto& [key, value] : sorted) {
        text += (text.empty() ? "" : ",") + key + "=" + value;
    }
    return text;
}

void print_ranked(const std::map<std::pair<std::string, std::string>, usize>& confusion,
                  usize limit) {
    std::vector<std::pair<usize, std::pair<std::string, std::string>>> ranked;
    for (const auto& [pair, n] : confusion) {
        ranked.emplace_back(n, pair);
    }
    std::ranges::sort(ranked, std::greater{});
    for (usize i = 0; i < std::min(ranked.size(), limit); ++i) {
        fmt::print("    game {:<34} ours {:<34} {}\n", ranked[i].second.first,
                   ranked[i].second.second, ranked[i].first);
    }
}

/// The crystals the game saved: `entities/` region files, one entry per
/// End crystal with its position.
[[nodiscard]] std::vector<std::array<f64, 3>> read_crystals(const std::filesystem::path& world) {
    std::vector<std::array<f64, 3>> crystals;
    const auto                      directory = world / "entities";
    if (!std::filesystem::is_directory(directory)) {
        return crystals;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        auto region = nbt::RegionFile::open(entry.path());
        if (!region) {
            continue;
        }
        for (u32 index = 0; index < 1024; ++index) {
            if (!region->has_chunk(index % 32, index / 32)) {
                continue;
            }
            auto document = region->read_chunk(index % 32, index / 32);
            if (!document) {
                continue;
            }
            const nbt::Tag* list = document->root.find("Entities");
            if (list == nullptr || list->type() != nbt::TagType::List) {
                continue;
            }
            for (const nbt::Tag& entity : *list->list()) {
                const nbt::Tag* id  = entity.find("id");
                const nbt::Tag* pos = entity.find("Pos");
                if (id == nullptr || pos == nullptr || id->as_string() != "minecraft:end_crystal" ||
                    pos->type() != nbt::TagType::List || pos->list()->size() != 3) {
                    continue;
                }
                crystals.push_back({(*pos->list())[0].as_f64(), (*pos->list())[1].as_f64(),
                                    (*pos->list())[2].as_f64()});
            }
        }
    }
    std::ranges::sort(crystals);
    return crystals;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    const auto    regions = options.world / "region";
    if (!std::filesystem::is_directory(regions)) {
        OV_LOG_ERROR("{} has no region/. Generate one with scripts/reference_end.sh.",
                     options.world.string());
        return 1;
    }

    auto blocks = registry::BlockRegistry::load(options.pack);
    if (!blocks) {
        OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
        return 1;
    }
    auto router = worldgen::NoiseRouter::load(options.data, "end", options.seed);
    if (!router) {
        OV_LOG_ERROR("router: {}", worldgen::to_string(router.error()));
        return 1;
    }
    auto source = worldgen::BiomeSource::load(options.reports, "end");
    if (!source) {
        OV_LOG_ERROR("biome source: {}", worldgen::to_string(source.error()));
        return 1;
    }
    auto surface = worldgen::SurfaceSystem::load(options.data, "end", options.seed, *blocks);
    if (!surface) {
        OV_LOG_ERROR("surface rules: {}", worldgen::to_string(surface.error()));
        return 1;
    }
    worldgen::ChunkGenerator generator{*router, *source, *blocks};
    generator.set_surface_system(&*surface);
    const auto air = world::AirStates::from(*blocks);

    const auto name_of = [&](registry::BlockStateId state) -> std::string_view {
        return state == registry::kAirState ? std::string_view("minecraft:air")
                                            : blocks->block_name(blocks->block_of(state));
    };
    const auto far_enough = [&](i32 chunk_x, i32 chunk_z) {
        const i64 x = static_cast<i64>(chunk_x) * 16 + 8;
        const i64 z = static_cast<i64>(chunk_z) * 16 + 8;
        return x * x + z * z >= static_cast<i64>(options.min_distance) * options.min_distance;
    };

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(regions)) {
        if (entry.path().extension() == ".mca") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);

    Count                                                biome_cells;
    usize                                                biome_chunks = 0;
    std::map<std::pair<std::string, std::string>, usize> biome_confusion;
    std::map<std::string, Count>                         biome_by_name;
    usize                                                block_chunks = 0;
    Count                                                block_cells;
    std::map<std::string, Count>                         by_game_block;
    std::map<std::pair<std::string, std::string>, usize> confusion;
    std::map<std::string, usize>                         statuses;
    std::vector<std::string>                             reports;
    std::vector<std::pair<i32, i32>>                     full_positions;

    ReferenceChunk reference;
    for (const auto& file : files) {
        auto region = nbt::RegionFile::open(file);
        if (!region) {
            continue;
        }
        for (u32 index = 0; index < 1024; ++index) {
            const u32 local_x = index % 32;
            const u32 local_z = index / 32;
            if (!region->has_chunk(local_x, local_z)) {
                continue;
            }
            auto document = region->read_chunk(local_x, local_z);
            if (!document) {
                continue;
            }
            const nbt::Tag* status_tag = document->root.find("Status");
            const nbt::Tag* x_tag      = document->root.find("xPos");
            const nbt::Tag* z_tag      = document->root.find("zPos");
            if (status_tag == nullptr || x_tag == nullptr || z_tag == nullptr) {
                continue;
            }
            const std::string status{status_tag->as_string()};
            ++statuses[status];
            const auto cx = static_cast<i32>(x_tag->as_i64());
            const auto cz = static_cast<i32>(z_tag->as_i64());
            if (!far_enough(cx, cz)) {
                continue;
            }
            if (status == "minecraft:full") {
                full_positions.emplace_back(cx, cz);
            }
            const bool want_blocks =
                pre_features(status) && block_chunks < static_cast<usize>(options.chunks);
            const bool want_biomes =
                biomes_final(status) && biome_chunks < static_cast<usize>(options.biome_chunks);
            if (!want_blocks && !want_biomes) {
                continue;
            }
            if (!decode(*document, reference, want_blocks)) {
                continue;
            }

            world::Chunk ours{ChunkPos{reference.chunk_x, reference.chunk_z},
                              world::WorldShape::the_end(), air, &*blocks};
            if (want_blocks) {
                generator.generate(ours);
            } else {
                generator.generate_biomes(ours);
            }

            if (want_biomes) {
                ++biome_chunks;
                for (i32 qy = 0; qy < kHeight / 4; ++qy) {
                    for (i32 qz = 0; qz < 4; ++qz) {
                        for (i32 qx = 0; qx < 4; ++qx) {
                            const std::string& game =
                                reference.biomes[(static_cast<usize>(qy) * 4 +
                                                  static_cast<usize>(qz)) * 4 +
                                                 static_cast<usize>(qx)];
                            if (game.empty()) {
                                continue;
                            }
                            const std::string_view mine = blocks->biome_name(ours.get_biome(
                                static_cast<usize>(qx * 4), qy * 4, static_cast<usize>(qz * 4)));
                            ++biome_cells.compared;
                            ++biome_by_name[game].compared;
                            if (mine == game) {
                                ++biome_cells.agreed;
                                ++biome_by_name[game].agreed;
                            } else {
                                ++biome_confusion[{game, std::string(mine)}];
                            }
                        }
                    }
                }
            }

            if (!want_blocks) {
                continue;
            }
            ++block_chunks;
            usize chunk_wrong = 0;
            for (i32 y = kMinY; y < kMinY + kHeight; ++y) {
                for (i32 z = 0; z < 16; ++z) {
                    for (i32 x = 0; x < 16; ++x) {
                        const std::string_view game = canonical(reference.block(x, y, z));
                        const std::string_view mine = name_of(
                            ours.get_block(static_cast<usize>(x), y, static_cast<usize>(z)));
                        ++block_cells.compared;
                        auto& per = by_game_block[std::string(game)];
                        ++per.compared;
                        if (game == mine) {
                            ++block_cells.agreed;
                            ++per.agreed;
                        } else {
                            ++confusion[{std::string(game), std::string(mine)}];
                            ++chunk_wrong;
                        }
                    }
                }
            }
            if (chunk_wrong != 0 && reports.size() < static_cast<usize>(options.show)) {
                reports.push_back(fmt::format("  chunk ({:>5},{:>5}) {}  {} blocks differ",
                                              reference.chunk_x, reference.chunk_z,
                                              reference.status, chunk_wrong));
            }
        }
    }

    fmt::print("seed {}, reference {}\n", options.seed, options.world.string());
    fmt::print("chunk statuses on disk:");
    for (const auto& [status, n] : statuses) {
        fmt::print("  {} {}", status, n);
    }
    fmt::print("\n");

    fmt::print("\nbiomes: {} chunks, {} / {} cells agree ({:.4f} %)\n", biome_chunks,
               biome_cells.agreed, biome_cells.compared,
               percent(biome_cells.agreed, biome_cells.compared));
    for (const auto& [name, count] : biome_by_name) {
        fmt::print("  {:<28} {:>9} / {:<9} {:.4f} %\n", name, count.agreed, count.compared,
                   percent(count.agreed, count.compared));
    }
    print_ranked(biome_confusion, 8);

    fmt::print("\nblocks through generate(): {} pre-feature chunks, {} / {} blocks agree "
               "({:.4f} %)\n",
               block_chunks, block_cells.agreed, block_cells.compared,
               percent(block_cells.agreed, block_cells.compared));
    for (const auto& [name, count] : by_game_block) {
        fmt::print("    {:<32} {:>9} / {:<9} {:.4f} %\n", name, count.agreed, count.compared,
                   percent(count.agreed, count.compared));
    }
    print_ranked(confusion, 10);
    for (const auto& line : reports) {
        fmt::print("{}\n", line);
    }

    // ── Finished chunks, through the pipeline ────────────────────────────────
    auto features = worldgen::FeatureRegistry::load(options.data, *blocks);
    if (!features) {
        OV_LOG_ERROR("features: {}", worldgen::to_string(features.error()));
        return 1;
    }
    auto decorator = worldgen::Decorator::load(options.data, *blocks, *features, *source);
    if (!decorator) {
        OV_LOG_ERROR("decorator: {}", worldgen::to_string(decorator.error()));
        return 1;
    }
    fmt::print("\nEnd decorator: {} biomes, {} placed features named but not built", decorator->biome_count(),
               decorator->missing_count());
    for (const auto& name : decorator->missing()) {
        fmt::print("  {}", name);
    }
    fmt::print("\n");

    worldgen::ChunkPipeline pipeline{generator, &*decorator, *blocks, world::WorldShape::the_end(),
                                     options.seed};
    if (options.full_chunks > 0) {
        usize                                                compared_chunks = 0;
        Count                                                cells;
        Count                                                states;
        std::map<std::string, Count>                         by_block;
        std::map<std::pair<std::string, std::string>, usize> full_confusion;
        std::map<std::pair<std::string, std::string>, usize> state_confusion;
        std::vector<std::string>                             gateway_diffs;
        std::ranges::sort(full_positions);
        for (const auto& [cx, cz] : full_positions) {
            if (compared_chunks >= static_cast<usize>(options.full_chunks)) {
                break;
            }
            auto region = nbt::RegionFile::open(regions / fmt::format("r.{}.{}.mca", cx >> 5, cz >> 5));
            if (!region) {
                continue;
            }
            auto document = region->read_chunk(static_cast<u32>(cx & 31), static_cast<u32>(cz & 31));
            if (!document || !decode(*document, reference, true)) {
                continue;
            }
            if (options.interesting &&
                std::ranges::all_of(reference.names, [](const std::string& name) {
                    return name == "minecraft:air" || name == "minecraft:end_stone";
                })) {
                continue;
            }
            const world::Chunk& ours = pipeline.promote(cx, cz, worldgen::ChunkStatus::Full);
            ++compared_chunks;
            for (i32 y = kMinY; y < kMinY + kHeight; ++y) {
                for (i32 z = 0; z < 16; ++z) {
                    for (i32 x = 0; x < 16; ++x) {
                        const std::string_view game  = canonical(reference.block(x, y, z));
                        const auto             state = ours.get_block(static_cast<usize>(x), y,
                                                                      static_cast<usize>(z));
                        const std::string_view mine  = name_of(state);
                        ++cells.compared;
                        auto& per = by_block[std::string(game)];
                        ++per.compared;
                        if (game != mine) {
                            ++full_confusion[{std::string(game), std::string(mine)}];
                            // ── worldgen-3 ── where a gateway disagrees, by position
                            if (game == "minecraft:end_gateway" || mine == "minecraft:end_gateway") {
                                gateway_diffs.push_back(fmt::format(
                                    "    gateway ({}, {}, {}) game {} ours {}", cx * 16 + x, y,
                                    cz * 16 + z, game, mine));
                            }
                            continue;
                        }
                        ++cells.agreed;
                        ++per.agreed;
                        if (game == "minecraft:air" || game == "minecraft:end_stone") {
                            continue;
                        }
                        // Same block: are the properties the same too?
                        ++states.compared;
                        const std::string& theirs = reference.properties[reference.cell(x, y, z)];
                        const std::string  mine_p = state_properties(*blocks, state);
                        if (theirs == mine_p) {
                            ++states.agreed;
                        } else {
                            ++state_confusion[{std::string(game) + "[" + theirs + "]",
                                               "[" + mine_p + "]"}];
                        }
                    }
                }
            }
            pipeline.trim(cx, cz, 3);
        }
        fmt::print("\nfull chunks through the pipeline: {} chunks, {} / {} blocks agree ({:.4f} %)\n",
                   compared_chunks, cells.agreed, cells.compared, percent(cells.agreed, cells.compared));
        for (const auto& [name, count] : by_block) {
            fmt::print("    {:<32} {:>9} / {:<9} {:.4f} %\n", name, count.agreed, count.compared,
                       percent(count.agreed, count.compared));
        }
        print_ranked(full_confusion, 20);
        fmt::print("  same block, same properties (neither air nor end stone): {} / {} ({:.4f} %)\n",
                   states.agreed, states.compared, percent(states.agreed, states.compared));
        print_ranked(state_confusion, 10);
        for (const auto& line : gateway_diffs) {
            fmt::print("{}\n", line);
        }
    }

    // ── The spikes ───────────────────────────────────────────────────────────
    //
    // Read from the game's own blocks: the top of the obsidian column at each
    // centre, the block above it, whether iron bars stand round it — against
    // the ten `end_spikes(seed)` computes.
    const auto spikes   = worldgen::end_spikes(options.seed);
    const auto crystals = read_crystals(options.world);
    std::map<std::pair<i32, i32>, ReferenceChunk> cache;
    const auto game_block = [&](i32 x, i32 y, i32 z) -> std::string {
        const auto key = std::pair{x >> 4, z >> 4};
        auto       hit = cache.find(key);
        if (hit == cache.end()) {
            ReferenceChunk decoded;
            bool           ok = false;
            if (auto region = nbt::RegionFile::open(
                    regions / fmt::format("r.{}.{}.mca", key.first >> 5, key.second >> 5))) {
                if (auto document = region->read_chunk(static_cast<u32>(key.first & 31),
                                                       static_cast<u32>(key.second & 31))) {
                    ok = decode(*document, decoded, true);
                }
            }
            if (!ok) {
                decoded.names.assign(static_cast<usize>(kHeight) * 256, "?");
            }
            hit = cache.emplace(key, std::move(decoded)).first;
        }
        return hit->second.block(x & 15, y, z & 15) +
               (hit->second.status == "minecraft:full" ? "" : " (" + hit->second.status + ")");
    };
    fmt::print("\nspikes (ours: centre, radius, height, cage) against the game's blocks:\n");
    usize spikes_right = 0;
    for (const auto& spike : spikes) {
        i32 top = -1;
        for (i32 y = kHeight - 1; y >= 0; --y) {
            if (game_block(spike.centre_x, y, spike.centre_z).starts_with("minecraft:obsidian")) {
                top = y;
                break;
            }
        }
        const std::string cap   = game_block(spike.centre_x, spike.height, spike.centre_z);
        const std::string above = game_block(spike.centre_x, spike.height + 1, spike.centre_z);
        const std::string cage  = game_block(spike.centre_x + 2, spike.height + 1, spike.centre_z);
        // Radius: the widest obsidian at the spike's mid height along +x.
        i32 radius = 0;
        while (radius < 8 &&
               game_block(spike.centre_x + radius + 1, spike.height - 5, spike.centre_z)
                   .starts_with("minecraft:obsidian")) {
            ++radius;
        }
        const bool ok = top == spike.height - 1 && cap.starts_with("minecraft:bedrock") &&
                        radius == spike.radius &&
                        cage.starts_with("minecraft:iron_bars") == spike.guarded;
        spikes_right += ok ? 1 : 0;
        fmt::print("  ({:>4},{:>4}) r {} h {:>3} {:<5}  game: top obsidian {:>3}, at h {}, above {}, "
                   "cage side {}, radius +x {}  {}\n",
                   spike.centre_x, spike.centre_z, spike.radius, spike.height,
                   spike.guarded ? "caged" : "open", top, cap, above, cage, radius,
                   ok ? "OK" : "DIFFERS");
    }
    fmt::print("  {} / 10 spikes as the game built them\n", spikes_right);
    fmt::print("crystals saved by the game: {}\n", crystals.size());
    usize crystals_right = 0;
    for (const auto& crystal : crystals) {
        bool matched = false;
        for (const auto& spike : spikes) {
            if (crystal[0] == spike.centre_x + 0.5 && crystal[1] == spike.height + 1.0 &&
                crystal[2] == spike.centre_z + 0.5) {
                matched = true;
            }
        }
        crystals_right += matched ? 1 : 0;
        fmt::print("  ({}, {}, {}) {}\n", crystal[0], crystal[1], crystal[2],
                   matched ? "= a spike's (x + 0.5, height + 1, z + 0.5)" : "matches no spike");
    }
    fmt::print("  {} / {} at a spike's top\n", crystals_right, crystals.size());

    // ── The column at the origin ─────────────────────────────────────────────
    //
    // Where the dragon fight builds the exit portal: the top of the column at
    // (0, 0). The game's reference End never started a fight, so its column is
    // terrain — the same question asked of both.
    {
        const world::Chunk& ours = pipeline.promote(0, 0, worldgen::ChunkStatus::Full);
        const i32 our_top =
            ours.heightmap(world::HeightmapType::MotionBlockingNoLeaves).first_free(0, 0);
        i32 game_top = -1;
        for (i32 y = kHeight - 1; y >= 0; --y) {
            const std::string name = game_block(0, y, 0);
            if (!name.starts_with("minecraft:air") && !name.starts_with("?")) {
                game_top = y + 1;
                break;
            }
        }
        fmt::print("\ncolumn (0, 0): first free block ours {}, game {} — the exit portal's ring "
                   "one below\n",
                   our_top, game_top);
    }
    return 0;
}
