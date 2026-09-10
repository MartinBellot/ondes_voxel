// Cave edges: the blocks that line a cave, ours against the game's.
//
// This exists because the order of the generation stages is invisible
// everywhere else. `ov_carveparity` compares the carving *mask*, which does not
// depend on the order at all. `ov_surfparity` hands the surface rules the
// game's own terrain, so it never sees a carver. `ov_parity --terrain` asks
// solid-or-not, and a cave wall is solid whichever block it is made of. Three
// harnesses, and not one of them can tell "surface then carvers" from "carvers
// then surface".
//
// The place where the order does show is the skin of a cave. Carve first and
// the surface rules count their depth from the cave's ceiling, so the ground
// above a cave gets the wrong layers; carve second and the carver cuts through
// grass, dirt, sand and gravel, and the grass left hanging over a cut becomes
// dirt. So: take every cell next to a carved cell, and ask how often our block
// is the game's block.
//
// The absolute number is not the point and cannot be, because our noise puts
// its surface one to eight blocks off the game's in most columns and that error
// is underneath everything here. The number that means something is the
// **paired** one: over the cells where the two orders disagree with each other,
// which of them agrees with the game more often. That comparison cancels
// everything both arms share, the height error included, and it is the last
// block of output.
//
// Both orders are generated in one process, from one binary, on one sample —
// the same discipline `ov_parity --carvers` uses, and for the same reason.
#define OV_LOG_CATEGORY "caveedge"

#include "ov/base/log.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
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
    /// The biome table lives in the data generator's *reports*, not in the
    /// datapack tree the density functions come from. Two roots, and they are
    /// genuinely different directories.
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    std::filesystem::path registries{"data/vanilla/1.20.1/registry.ovpack"};
    i64                   seed{1234567890};
    i32                   chunks{60};
    /// How many chunks to take from any one region file. A region is often one
    /// biome; taking them all from the first file measures one landscape and
    /// calls it a world.
    i32 per_region{2};
    /// How many of the most common disagreements to print per arm.
    i32 show{10};
    /// Spare a carved cell that holds a fluid, instead of emptying it.
    ///
    /// The generator empties them by default, because the tag lists water and
    /// because it measures better. This puts it back, which is how that was
    /// settled and how it can be re-settled once an aquifer exists. Passed to
    /// the generator through the environment variable it already documents.
    bool keep_fluids{false};
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
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=").c_str());
        } else if (argument == "--keep-fluids") {
            options.keep_fluids = true;
        }
    }
    return options;
}

/// Vanilla's three airs are one block as far as any comparison goes. Comparing
/// the raw names instead makes a cave disagree with itself about a block that
/// is not there.
[[nodiscard]] std::string_view normalise(std::string_view name) {
    if (name == "minecraft:cave_air" || name == "minecraft:void_air") {
        return "minecraft:air";
    }
    return name;
}

/// Blocks a stage *after* the carvers puts down, where we correctly left what
/// the stage before it decided.
///
/// The ore stage scatters granite, andesite, diorite and the ores through
/// stone, and `ore_dirt` / `ore_gravel` scatter dirt and gravel through it too.
/// A cave wall of granite where we have stone is that stage's, not an ordering
/// error, and counting it as one would bury the signal this harness is looking
/// for. Named and counted apart rather than folded into either total — the same
/// treatment `ov_surfparity` gives them, and for the same reason: it is an
/// attribution, not a proof, and a real bug can hide behind each one.
[[nodiscard]] bool later_stage_owns(std::string_view theirs, std::string_view ours) {
    if (ours != "minecraft:stone" && ours != "minecraft:deepslate") {
        return false;
    }
    static constexpr std::array<std::string_view, 20> kOreStage{
        "minecraft:granite",         "minecraft:andesite",
        "minecraft:diorite",         "minecraft:tuff",
        "minecraft:coal_ore",        "minecraft:iron_ore",
        "minecraft:copper_ore",      "minecraft:gold_ore",
        "minecraft:redstone_ore",    "minecraft:lapis_ore",
        "minecraft:diamond_ore",     "minecraft:emerald_ore",
        "minecraft:deepslate_coal_ore", "minecraft:deepslate_iron_ore",
        "minecraft:deepslate_copper_ore", "minecraft:deepslate_gold_ore",
        "minecraft:deepslate_redstone_ore", "minecraft:deepslate_lapis_ore",
        "minecraft:deepslate_diamond_ore", "minecraft:deepslate_emerald_ore"};
    return std::ranges::find(kOreStage, theirs) != kOreStage.end();
}

struct ReferenceChunk {
    i32 chunk_x{0};
    i32 chunk_z{0};
    /// Block names, indexed [(y - kMinY) * 256 + z * 16 + x].
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

/// Unpack a section's packed palette indices. Vanilla never lets an entry
/// straddle a long, which is why this is a shift and not a bit cursor.
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
        // Blocks have a floor of four bits a cell. That is in the format, not a
        // rounding choice.
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

/// One arm of the comparison: one stage order, counted over one sample.
struct Arm {
    std::string name;
    /// Cells the mask cut.
    usize interior_seen{0};
    usize interior_agreed{0};
    /// Cells the mask did not cut but that touch one it did — the cave's skin,
    /// and the only place the stage order is visible.
    usize wall_seen{0};
    usize wall_agreed{0};
    /// Wall cells a later stage owns, held out of both totals above.
    usize wall_attributed{0};
    std::map<std::string, usize> wall_confusions;
};

void tally(Arm& arm, bool carved, std::string_view theirs, std::string_view ours) {
    if (carved) {
        ++arm.interior_seen;
        if (theirs == ours) {
            ++arm.interior_agreed;
        }
        return;
    }
    if (later_stage_owns(theirs, ours)) {
        ++arm.wall_attributed;
        return;
    }
    ++arm.wall_seen;
    if (theirs == ours) {
        ++arm.wall_agreed;
    } else {
        ++arm.wall_confusions[fmt::format("{} -> {}", theirs, ours)];
    }
}

void report(const Arm& arm, i32 show) {
    const auto percent = [](usize part, usize whole) {
        return whole == 0 ? 0.0 : 100.0 * static_cast<f64>(part) / static_cast<f64>(whole);
    };
    fmt::print("\n{}\n", arm.name);
    fmt::print("  cave walls   {:>9} / {:>9}  ({:6.3f} %)\n", arm.wall_agreed, arm.wall_seen,
               percent(arm.wall_agreed, arm.wall_seen));
    fmt::print("  cave interior{:>9} / {:>9}  ({:6.3f} %)\n", arm.interior_agreed,
               arm.interior_seen, percent(arm.interior_agreed, arm.interior_seen));
    fmt::print("  a later stage owns, held apart: {}\n", arm.wall_attributed);
    if (show <= 0 || arm.wall_confusions.empty()) {
        return;
    }
    std::vector<std::pair<std::string, usize>> sorted(arm.wall_confusions.begin(),
                                                      arm.wall_confusions.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
    fmt::print("  most common wall disagreements (the game -> ours):\n");
    for (usize i = 0; i < sorted.size() && i < static_cast<usize>(show); ++i) {
        fmt::print("    {:>8}  {}\n", sorted[i].second, sorted[i].first);
    }
}

// ── the aquifer arm ─────────────────────────────────────────────────────────
//
// Every cell of every generated chunk, sorted into four classes — air, water,
// lava, solid — ours against the game's, with the aquifer on and off. The
// class and not the block, because what the aquifer decides is exactly the
// class: it never chooses between stone and dirt.

/// The four classes, in this order: air, water, lava, solid.
///
/// Ice and the plants that grow in water are water for this purpose: they are
/// what a later stage made of a water cell (the freeze, the seagrass), and
/// counting them as solid is how an ice sheet becomes a terrain error (see
/// docs/provenance — "la glace n'est pas du terrain"). Snow layers are air for
/// the same reason on the other side.
[[nodiscard]] usize fluid_class(std::string_view name) {
    if (name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air" ||
        name == "minecraft:snow") {
        return 0;
    }
    if (name == "minecraft:water" || name == "minecraft:ice" ||
        name == "minecraft:bubble_column" || name == "minecraft:seagrass" ||
        name == "minecraft:tall_seagrass" || name == "minecraft:kelp" ||
        name == "minecraft:kelp_plant") {
        return 1;
    }
    if (name == "minecraft:lava") {
        return 2;
    }
    return 3;
}

struct ClassArm {
    std::string                                 name;
    std::array<std::array<usize, 4>, 4>         table{};
    /// The same, below the sea level only, where the aquifer has its say.
    std::array<std::array<usize, 4>, 4>         below_sea{};
};

void report(const ClassArm& arm) {
    static constexpr std::array<std::string_view, 4> kNames{"air", "water", "lava", "solid"};
    const auto print = [](const std::array<std::array<usize, 4>, 4>& table, std::string_view what) {
        usize total = 0;
        usize agree = 0;
        fmt::print("  {}\n    {:>12} {:>10} {:>10} {:>10} {:>10}\n", what, "game \\ ours",
                   kNames[0], kNames[1], kNames[2], kNames[3]);
        for (usize t = 0; t < 4; ++t) {
            fmt::print("    {:>12} {:>10} {:>10} {:>10} {:>10}\n", kNames[t], table[t][0],
                       table[t][1], table[t][2], table[t][3]);
            for (usize o = 0; o < 4; ++o) {
                total += table[t][o];
                agree += t == o ? table[t][o] : 0;
            }
        }
        fmt::print("    agreement {} / {} = {:.3f} %\n", agree, total,
                   total == 0 ? 0.0 : 100.0 * static_cast<f64>(agree) / static_cast<f64>(total));
    };
    fmt::print("\n{}\n", arm.name);
    print(arm.table, "every cell of every generated chunk");
    print(arm.below_sea, "cells below the sea level (y < 63)");
}

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
    auto surface =
        worldgen::SurfaceSystem::load(options.data, "overworld", options.seed, *blocks);
    if (!surface) {
        OV_LOG_ERROR("surface rules: {}", worldgen::to_string(surface.error()));
        return 1;
    }

    if (options.keep_fluids) {
        // Read by the generator when the carvers are attached. Set here rather
        // than exposed as a setter because it is the same switch the generator
        // already documents, and two ways to say one thing is one too many.
        ::setenv("OV_CARVE_FLUIDS", "0", 1);
    }

    const worldgen::CarvingContext carving_context{router->min_y(), router->height()};
    const worldgen::CarverStage    carvers{options.seed, carving_context};

    // Two generators, one process, one sample. The only difference between them
    // is when the carvers run.
    worldgen::ChunkGenerator after{*router, *biomes, *blocks};
    worldgen::ChunkGenerator before{*router, *biomes, *blocks};
    for (auto* generator : {&after, &before}) {
        generator->set_surface_system(&*surface);
        if (auto attached = generator->set_carvers(&carvers, *registries); !attached) {
            OV_LOG_ERROR("carvers: {}", worldgen::to_string(attached.error()));
            return 1;
        }
    }
    after.set_carve_before_surface(false);
    before.set_carve_before_surface(true);

    // ── the aquifer arm: the game's order, with the aquifer switched off ──
    worldgen::ChunkGenerator dry{*router, *biomes, *blocks};
    dry.set_surface_system(&*surface);
    if (auto attached = dry.set_carvers(&carvers, *registries); !attached) {
        OV_LOG_ERROR("carvers: {}", worldgen::to_string(attached.error()));
        return 1;
    }
    dry.set_carve_before_surface(false);
    dry.set_aquifer_enabled(false);
    ClassArm class_on;
    class_on.name = "classes, aquifer on  (the generator as it ships)";
    ClassArm class_off;
    class_off.name = "classes, aquifer off (global fluid rule, carvers cut to air)";
    usize class_paired      = 0;
    usize class_only_on     = 0;
    usize class_only_off    = 0;
    usize class_neither     = 0;
    std::map<std::string, usize> class_changes;

    Arm arm_before;
    arm_before.name = "before — noise + carvers, then biomes, then surface (stone cut only)";
    Arm arm_after;
    arm_after.name = "after  — noise, biomes, surface, then carvers (the tag cut)";

    /// The paired comparison, and the one that answers the question. Counted
    /// only over wall cells where the two orders produced different blocks:
    /// everything the two arms share — the noise's height error above all —
    /// cancels, and what is left is the ordering and nothing else.
    usize paired_cells      = 0;
    usize paired_only_after = 0;
    usize paired_only_before = 0;
    usize paired_neither    = 0;

    const auto our_name = [&](registry::BlockStateId state) {
        return state == registry::kAirState
                   ? std::string_view("minecraft:air")
                   : normalise(blocks->block_name(blocks->block_of(state)));
    };

    const world::AirStates air = world::AirStates::from(*blocks);
    const auto             shape = world::WorldShape::overworld();

    ReferenceChunk    reference;
    usize             chunks_read = 0;
    usize             chunks_with_caves = 0;
    worldgen::CarvingMask mask{carving_context.min_y, carving_context.height};

    for (const auto& entry : std::filesystem::directory_iterator(options.world / "region")) {
        if (chunks_read >= static_cast<usize>(options.chunks)) {
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
        for (u32 index = 0; index < 1024 && chunks_read < static_cast<usize>(options.chunks) &&
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
            // Only finished chunks: a partial one is a chunk the game never
            // completed, and its blocks are not a generation result. The
            // carving mask is the one thing that inverts this rule — a full
            // chunk has thrown it away — but we compute the mask ourselves, so
            // that does not apply here.
            const nbt::Tag* status = document->root.find("Status");
            if (status == nullptr || status->as_string() != "minecraft:full") {
                continue;
            }
            if (!decode(*document, reference)) {
                continue;
            }
            ++chunks_read;
            ++from_this_region;

            carvers.carve_into(reference.chunk_x, reference.chunk_z, mask);
            if (mask.empty()) {
                continue;
            }
            ++chunks_with_caves;

            const ov::ChunkPos position{reference.chunk_x, reference.chunk_z};
            world::Chunk         chunk_after{position, shape, air, &*blocks};
            world::Chunk         chunk_before{position, shape, air, &*blocks};
            after.generate(chunk_after);
            before.generate(chunk_before);

            // ── the aquifer arm ──
            world::Chunk chunk_dry{position, shape, air, &*blocks};
            dry.generate(chunk_dry);
            for (i32 cz = 0; cz < 16; ++cz) {
                for (i32 cx = 0; cx < 16; ++cx) {
                    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                        const std::string_view theirs = normalise(reference.block(cx, y, cz));
                        if (theirs.empty()) {
                            continue;
                        }
                        const auto  ax     = static_cast<usize>(cx);
                        const auto  az     = static_cast<usize>(cz);
                        const usize game   = fluid_class(theirs);
                        const usize on     = fluid_class(our_name(chunk_after.get_block(ax, y, az)));
                        const usize off    = fluid_class(our_name(chunk_dry.get_block(ax, y, az)));
                        ++class_on.table[game][on];
                        ++class_off.table[game][off];
                        if (y < 63) {
                            ++class_on.below_sea[game][on];
                            ++class_off.below_sea[game][off];
                        }
                        if (on != off) {
                            ++class_paired;
                            static constexpr std::array<std::string_view, 4> kClass{
                                "air", "water", "lava", "solid"};
                            ++class_changes[fmt::format("game {:<5}  off {:<5} -> on {:<5}",
                                                        kClass[game], kClass[off], kClass[on])];
                            if (on == game && off != game) {
                                ++class_only_on;
                            } else if (off == game && on != game) {
                                ++class_only_off;
                            } else if (on != game && off != game) {
                                ++class_neither;
                            }
                        }
                    }
                }
            }

            for (i32 cz = 0; cz < 16; ++cz) {
                for (i32 cx = 0; cx < 16; ++cx) {
                    for (i32 y = shape.min_y; y <= shape.max_y(); ++y) {
                        const bool carved = mask.get(cx, y, cz);
                        // A wall cell is one the carvers did not cut but that
                        // touches one they did. Six-connexity, and the x and z
                        // neighbours outside the chunk are simply not asked
                        // about: the mask is per chunk, so a cell on the border
                        // would be judged against a mask we do not have here.
                        bool touches = false;
                        if (!carved) {
                            static constexpr std::array<std::array<i32, 3>, 6> kNeighbours{
                                std::array<i32, 3>{1, 0, 0},  std::array<i32, 3>{-1, 0, 0},
                                std::array<i32, 3>{0, 1, 0},  std::array<i32, 3>{0, -1, 0},
                                std::array<i32, 3>{0, 0, 1},  std::array<i32, 3>{0, 0, -1}};
                            for (const auto& step : kNeighbours) {
                                const i32 nx = cx + step[0];
                                const i32 ny = y + step[1];
                                const i32 nz = cz + step[2];
                                if (nx < 0 || nx > 15 || nz < 0 || nz > 15 ||
                                    ny < shape.min_y || ny > shape.max_y()) {
                                    continue;
                                }
                                if (mask.get(nx, ny, nz)) {
                                    touches = true;
                                    break;
                                }
                            }
                        }
                        if (!carved && !touches) {
                            continue;
                        }

                        const std::string_view theirs = normalise(reference.block(cx, y, cz));
                        if (theirs.empty()) {
                            continue;
                        }
                        const auto ax = static_cast<usize>(cx);
                        const auto az = static_cast<usize>(cz);
                        const std::string_view mine_after =
                            our_name(chunk_after.get_block(ax, y, az));
                        const std::string_view mine_before =
                            our_name(chunk_before.get_block(ax, y, az));

                        tally(arm_after, carved, theirs, mine_after);
                        tally(arm_before, carved, theirs, mine_before);

                        if (!carved && mine_after != mine_before) {
                            ++paired_cells;
                            const bool right_after  = mine_after == theirs;
                            const bool right_before = mine_before == theirs;
                            if (right_after && !right_before) {
                                ++paired_only_after;
                            } else if (right_before && !right_after) {
                                ++paired_only_before;
                            } else if (!right_after && !right_before) {
                                ++paired_neither;
                            }
                        }
                    }
                }
            }
        }
    }

    fmt::print("\nseed {}, {} full chunks read, {} of them carved\n", options.seed, chunks_read,
               chunks_with_caves);
    fmt::print("cells adjacent to a carved cell, and the carved cells themselves\n");
    report(arm_before, options.show);
    report(arm_after, options.show);

    fmt::print("\npaired: wall cells where the two orders disagree with each other\n");
    if (paired_cells == 0) {
        fmt::print("  none — the two orders produced identical chunks on this sample\n");
    } else {
        const auto percent = [&](usize part) {
            return 100.0 * static_cast<f64>(part) / static_cast<f64>(paired_cells);
        };
        fmt::print("  cells the reordering changed:      {:>9}\n", paired_cells);
        fmt::print("  only the new order matches:        {:>9}  ({:6.3f} %)\n", paired_only_after,
                   percent(paired_only_after));
        fmt::print("  only the old order matches:        {:>9}  ({:6.3f} %)\n",
                   paired_only_before, percent(paired_only_before));
        fmt::print("  neither matches:                   {:>9}  ({:6.3f} %)\n", paired_neither,
                   percent(paired_neither));
    }

    // ── the aquifer arm, reported ──
    report(class_off);
    report(class_on);
    fmt::print("\npaired: cells whose class the aquifer changed\n");
    if (class_paired == 0) {
        fmt::print("  none — switching the aquifer off changed nothing on this sample\n");
    } else {
        const auto percent = [&](usize part) {
            return 100.0 * static_cast<f64>(part) / static_cast<f64>(class_paired);
        };
        fmt::print("  cells the aquifer changed:         {:>9}\n", class_paired);
        fmt::print("  only the aquifer matches the game: {:>9}  ({:6.3f} %)\n", class_only_on,
                   percent(class_only_on));
        fmt::print("  only the global rule matches:      {:>9}  ({:6.3f} %)\n", class_only_off,
                   percent(class_only_off));
        fmt::print("  neither matches:                   {:>9}  ({:6.3f} %)\n", class_neither,
                   percent(class_neither));
        std::vector<std::pair<std::string, usize>> sorted(class_changes.begin(),
                                                          class_changes.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
        for (usize i = 0; i < sorted.size() && i < 12; ++i) {
            fmt::print("    {:>9}  {}\n", sorted[i].second, sorted[i].first);
        }
    }
    return 0;
}
