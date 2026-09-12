// Surface parity: our surface rules against a world the real game wrote.
//
// The measurement has to be designed before it means anything. Running our
// whole generator and comparing block for block would answer a question nobody
// asked: the noise stage puts its surface one to eight blocks below the game's
// in nine columns out of ten, so every column would disagree and the number
// would say nothing at all about the rules.
//
// So this hands the rules the game's *own* terrain. Each column of the
// reference world is stripped back to what the noise stage would have left —
// stone where the game has anything solid, water where it has water, air where
// it has air or a tree — and the rules are run over that. What comes back is
// this stage and nothing else.
//
// Two traps are already paid for. The first: only `minecraft:full` chunks. A
// force-loaded region holds chunks that stopped earlier, and their biome array
// is the default, so reading them makes every disagreement say "plains" — that
// one already cost 67 % where the truth was 99.9 %. The second: the ore stage
// runs *after* the surface, so the game's granite and coal ore sit where our
// rules correctly left stone. Those are counted apart rather than as errors.
#define OV_LOG_CATEGORY "surfparity"

#include "ov/base/log.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/worldgen/biome_source.hpp"
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
#include <vector>

using namespace ov;

namespace {

struct Options {
    std::filesystem::path world{"run/reference-1234567890/world"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    i64                   seed{1234567890};
    i32                   chunks{40};
    /// How many blocks below each column's surface to compare. Eight is the
    /// mandate's figure and it is the right one: the surface rules reach about
    /// six blocks down in the deepest case — badlands terracotta — and nothing
    /// below that is theirs.
    i32 depth{8};
    /// Print the first few disagreeing columns in full.
    i32 show{0};
    /// How many chunks to take from any one region file.
    ///
    /// A region is 512 blocks across, which is often one biome. Taking forty
    /// chunks from the first file measured three biomes and called it a world;
    /// spreading the same forty over forty regions measured twenty-two.
    i32 per_region{2};
    /// Take the preliminary surface from the reference world's own terrain
    /// rather than from our density graph. Answers, by measurement, how much
    /// of the residual belongs to the noise stage below this one.
    bool prelim_reference{false};
    /// Skip the rules entirely and compare the bare noise-stage column.
    ///
    /// The "before" number, and it has to be measured rather than asserted: a
    /// world of nothing but stone still agrees with the game wherever the game
    /// left stone, and how often that is, is not obvious.
    bool no_rules{false};
    /// Compare only columns in this biome.
    std::string biome;
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
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=");
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=").c_str());
        } else if (argument.starts_with("--chunks=")) {
            options.chunks = std::atoi(value("--chunks=").c_str());
        } else if (argument.starts_with("--depth=")) {
            options.depth = std::atoi(value("--depth=").c_str());
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=").c_str());
        } else if (argument.starts_with("--per-region=")) {
            options.per_region = std::atoi(value("--per-region=").c_str());
        } else if (argument == "--no-rules") {
            options.no_rules = true;
        } else if (argument == "--prelim-reference") {
            options.prelim_reference = true;
        } else if (argument.starts_with("--biome=")) {
            options.biome = value("--biome=");
        }
    }
    return options;
}

constexpr i32 kMinY   = -64;
constexpr i32 kHeight = 384;

/// What a block in the finished world was, as far as the surface stage is
/// concerned.
enum class Kind : u8 {
    /// Air, or something a later stage put in the air: a trunk, a leaf, a
    /// flower, a layer of snow.
    Air,
    /// Water or lava. Left where it is; the rules read it and rarely write it.
    Fluid,
    /// Stone, or something a later stage made out of stone.
    Terrain,
};

/// Is this block one the *feature* stage put there rather than the surface?
///
/// Everything here was arrived at by looking at what actually turned up in the
/// disagreements, not by reasoning about what ought to. Trees are the bulk of
/// it; the rest are the plants that sit on almost every grass block.
[[nodiscard]] bool is_feature_block(std::string_view name) {
    constexpr std::array<std::string_view, 10> kParts{"_log",  "_leaves", "_wood",  "vine",
                                                      "_stem", "mushroom", "coral", "sapling",
                                                      "_tulip", "bush"};
    for (const std::string_view part : kParts) {
        if (name.find(part) != std::string_view::npos) {
            return true;
        }
    }
    constexpr std::array<std::string_view, 24> kExact{
        "minecraft:short_grass",  "minecraft:grass",        "minecraft:tall_grass",
        "minecraft:fern",         "minecraft:large_fern",   "minecraft:snow",
        "minecraft:sugar_cane",   "minecraft:cactus",       "minecraft:bamboo",
        "minecraft:seagrass",     "minecraft:tall_seagrass", "minecraft:kelp",
        "minecraft:kelp_plant",   "minecraft:lily_pad",     "minecraft:moss_carpet",
        "minecraft:dandelion",    "minecraft:poppy",        "minecraft:blue_orchid",
        "minecraft:allium",       "minecraft:azure_bluet",  "minecraft:oxeye_daisy",
        "minecraft:cornflower",   "minecraft:lily_of_the_valley", "minecraft:torch"};
    for (const std::string_view exact : kExact) {
        if (name == exact) {
            return true;
        }
    }
    return false;
}

/// Blocks the *ore* stage makes out of stone, after the surface rules have run.
///
/// Our rules leaving stone where the game has granite is not a surface bug, and
/// counting it as one would bury the real ones. Named exactly rather than by
/// substring: "deepslate" is a surface rule's output and
/// "deepslate_coal_ore" is not.
[[nodiscard]] bool is_ore_stage_block(std::string_view name) {
    constexpr std::array<std::string_view, 8> kParts{"_ore",     "granite",  "diorite",
                                                     "andesite", "tuff",     "dripstone",
                                                     "amethyst", "smooth_basalt"};
    for (const std::string_view part : kParts) {
        if (name.find(part) != std::string_view::npos) {
            return true;
        }
    }
    return name == "minecraft:clay" || name == "minecraft:raw_iron_block" ||
           name == "minecraft:raw_copper_block";
}

/// Three names for the same nothing.
///
/// `cave_air` is what a carver leaves and `void_air` what a chunk boundary
/// reads back as; both are air to everything except the debug screen. Comparing
/// the names raw made 117 cells out of 16016 disagree about a block that is not
/// there, which is the sort of thing that quietly costs a percent.
[[nodiscard]] std::string_view canonical(std::string_view name) {
    if (name == "minecraft:cave_air" || name == "minecraft:void_air") {
        return "minecraft:air";
    }
    return name;
}

/// Vegetation that grows *inside* the water column.
///
/// Stripping a seagrass back to air rather than to water is a trap with teeth:
/// walking down the column then meets air where the game met water, which
/// resets `water_height` to "no water at all", and the sea bed below takes the
/// land branch and comes back as a grass block. That alone was most of what
/// separated lukewarm_ocean at 81 % from the rest of the world at 99 %.
[[nodiscard]] bool is_underwater_plant(std::string_view name) {
    constexpr std::array<std::string_view, 6> kExact{
        "minecraft:seagrass", "minecraft:tall_seagrass", "minecraft:kelp",
        "minecraft:kelp_plant", "minecraft:sea_pickle", "minecraft:bubble_column"};
    for (const std::string_view exact : kExact) {
        if (name == exact) {
            return true;
        }
    }
    // Coral in every form: blocks, fans, wall fans and the dead versions of
    // all of them. They sit in water, so removing one leaves water.
    return name.find("coral") != std::string_view::npos;
}

[[nodiscard]] Kind classify(std::string_view name) {
    if (name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air") {
        return Kind::Air;
    }
    if (is_underwater_plant(name)) {
        return Kind::Fluid;
    }
    if (name == "minecraft:water" || name == "minecraft:lava" ||
        name == "minecraft:bubble_column") {
        return Kind::Fluid;
    }
    // The lid the `freeze_top_layer` feature puts on cold water, long after
    // this stage. In the world the rules saw it was still water — and reading
    // it as stone made every frozen river a column whose surface was in the
    // wrong place, which was worth 605 disagreements on its own.
    if (name == "minecraft:ice") {
        return Kind::Fluid;
    }
    if (is_feature_block(name)) {
        return Kind::Air;
    }
    return Kind::Terrain;
}

/// ── worldgen-3 ── The ice the frozen-ocean pass and the iceberg feature stack
/// over the sea: packed ice, blue ice, and the snow blocks capping them.
[[nodiscard]] bool is_berg_ice(std::string_view name) {
    return name == "minecraft:packed_ice" || name == "minecraft:blue_ice" ||
           name == "minecraft:snow_block";
}

/// `classify`, in a frozen ocean column: the berg ice was put there after the
/// rules (by the pass that follows them, or by the iceberg feature), into what
/// was water under the sea and air over it. Anywhere else packed ice and snow
/// blocks are the rules' own output — the frozen peaks — and stay terrain.
[[nodiscard]] Kind classify_in(std::string_view name, i32 y, bool frozen_column) {
    if (frozen_column && is_berg_ice(name)) {
        return y < 63 ? Kind::Fluid : Kind::Air;
    }
    return classify(name);
}

/// One chunk of the reference world, decoded.
struct ReferenceChunk {
    i32 chunk_x{0};
    i32 chunk_z{0};
    /// Block names, indexed [(y - kMinY) * 256 + z * 16 + x].
    std::vector<std::string> names;
    /// Biome names on the quart grid, indexed [(qy * 4 + qz) * 4 + qx] with qy
    /// counted from the bottom of the world.
    std::vector<std::string> biomes;

    [[nodiscard]] std::string_view block(i32 local_x, i32 y, i32 local_z) const {
        const usize index = static_cast<usize>(y - kMinY) * 256 +
                            static_cast<usize>(local_z) * 16 + static_cast<usize>(local_x);
        return index < names.size() ? std::string_view(names[index]) : std::string_view();
    }

    [[nodiscard]] std::string_view biome(i32 local_x, i32 y, i32 local_z) const {
        const i32   qy    = (y - kMinY) >> 2;
        const usize index = (static_cast<usize>(qy) * 4 + static_cast<usize>(local_z >> 2)) * 4 +
                            static_cast<usize>(local_x >> 2);
        return index < biomes.size() ? std::string_view(biomes[index]) : std::string_view();
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
                                       ((1ULL << bits) - 1));
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
    chunk.biomes.assign(static_cast<usize>(kHeight / 4) * 16, "minecraft:plains");

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

        if (const nbt::Tag* states = section.find("block_states")) {
            const nbt::Tag* palette = states->find("palette");
            if (palette != nullptr && palette->type() == nbt::TagType::List &&
                !palette->list()->empty()) {
                std::vector<std::string> names;
                for (const nbt::Tag& entry : *palette->list()) {
                    const nbt::Tag* name = entry.find("Name");
                    names.emplace_back(name == nullptr ? "minecraft:air" : name->as_string());
                }
                // Blocks have a floor of four bits a cell; biomes do not. That
                // asymmetry is in the format, not a rounding choice.
                const usize bits =
                    names.size() <= 1
                        ? 0
                        : std::max<usize>(4, static_cast<usize>(std::bit_width(names.size() - 1)));
                unpack(states->find("data"), bits, 4096, indices);
                for (usize cell = 0; cell < 4096; ++cell) {
                    const usize which = indices[cell] < names.size() ? indices[cell] : 0;
                    const i32   local_x = static_cast<i32>(cell % 16);
                    const i32   local_z = static_cast<i32>((cell / 16) % 16);
                    const i32   y       = section_y * 16 + static_cast<i32>(cell / 256);
                    chunk.names[static_cast<usize>(y - kMinY) * 256 +
                                static_cast<usize>(local_z) * 16 +
                                static_cast<usize>(local_x)] = names[which];
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

/// ── worldgen-3 ── A frozen ocean column, by the game's biome at the sea's
/// surface.
[[nodiscard]] bool frozen_column(const ReferenceChunk& chunk, i32 local_x, i32 local_z) {
    const std::string_view biome = chunk.biome(local_x, 62, local_z);
    return biome == "minecraft:frozen_ocean" || biome == "minecraft:deep_frozen_ocean";
}

/// ── worldgen-3 ── A bare column in the eroded badlands: stone to y 63, air
/// above. Run through the surface, it comes back with the pillar our noise
/// raises there, if any — the prediction the game's terrain top is held to.
class PillarQueries final : public worldgen::SurfaceQueries {
public:
    explicit PillarQueries(const worldgen::SurfaceQueries& inner) : inner_(&inner) {}
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override {
        return "minecraft:eroded_badlands";
    }
    [[nodiscard]] f64 temperature_at(i32, i32, i32) const override { return 2.0; }
    [[nodiscard]] i32 surface_height(i32, i32) const override { return 63; }
    [[nodiscard]] i32 preliminary_surface(i32 x, i32 z) const override {
        return inner_->preliminary_surface(x, z);
    }

private:
    const worldgen::SurfaceQueries* inner_;
};

/// What the rules are told about the *game's* chunk.
class ReferenceQueries final : public worldgen::SurfaceQueries {
public:
    ReferenceQueries(const ReferenceChunk& chunk, const registry::BlockRegistry& blocks,
                     const worldgen::NoiseRouter& router, const std::array<i32, 256>& heights,
                     const std::array<i32, 256>& terrain_tops, bool prelim_from_reference)
        : chunk_(&chunk),
          blocks_(&blocks),
          initial_(router.entry("initial_density_without_jaggedness")),
          heights_(&heights),
          terrain_tops_(&terrain_tops),
          cell_height_(router.cell_height()),
          prelim_from_reference_(prelim_from_reference) {}

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        return chunk_->biome(x - chunk_->chunk_x * 16, y, z - chunk_->chunk_z * 16);
    }

    [[nodiscard]] f64 temperature_at(i32 x, i32 y, i32 z) const override {
        const auto index = blocks_->find_biome(biome_at(x, y, z));
        return index ? blocks_->biome(*index).temperature : 0.8;
    }

    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        const usize local_x = static_cast<usize>(x - chunk_->chunk_x * 16) & 15U;
        const usize local_z = static_cast<usize>(z - chunk_->chunk_z * 16) & 15U;
        return (*heights_)[local_z * 16 + local_x];
    }

    [[nodiscard]] i32 preliminary_surface(i32 x, i32 z) const override {
        if (prelim_from_reference_) {
            // The game's own coarse surface, standing in for the density
            // graph's. Vanilla scans the cell grid downward from the top of
            // the world and stops at the first cell the density calls solid,
            // so the answer is always a multiple of the cell height and always
            // at or just below the real surface. Rounding the game's terrain
            // top down to the same grid reproduces that closely enough to
            // answer the only question this mode exists for: how much of what
            // is left is our noise stage's two-block error rather than these
            // rules.
            const usize local_x = static_cast<usize>(x - chunk_->chunk_x * 16) & 15U;
            const usize local_z = static_cast<usize>(z - chunk_->chunk_z * 16) & 15U;
            const i32   top     = (*terrain_tops_)[local_z * 16 + local_x];
            const i32   from    = top - kMinY;
            return kMinY + (from / cell_height_) * cell_height_;
        }
        if (initial_ == nullptr) {
            return worldgen::kNoSurface;
        }
        const i32 quart_x = (x >> 2) << 2;
        const i32 quart_z = (z >> 2) << 2;
        const u64 key     = (static_cast<u64>(static_cast<u32>(quart_x)) << 32) |
                        static_cast<u64>(static_cast<u32>(quart_z));
        if (const auto found = cache_.find(key); found != cache_.end()) {
            return found->second;
        }
        i32 level = worldgen::kNoSurface;
        for (i32 y = kMinY + kHeight; y >= kMinY; y -= cell_height_) {
            if (initial_->compute(worldgen::FunctionContext{quart_x, y, quart_z}) > 0.390625) {
                level = y;
                break;
            }
        }
        cache_.emplace(key, level);
        return level;
    }

private:
    const ReferenceChunk*             chunk_;
    const registry::BlockRegistry*    blocks_;
    const worldgen::DensityFunction*  initial_;
    const std::array<i32, 256>*       heights_;
    const std::array<i32, 256>*       terrain_tops_;
    i32                               cell_height_;
    bool                              prelim_from_reference_;
    mutable std::map<u64, i32>        cache_;
};

struct Tally {
    usize compared{0};
    usize agreed{0};
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
    auto router = worldgen::NoiseRouter::load(options.data, "overworld", options.seed);
    if (!router) {
        OV_LOG_ERROR("router: {}", worldgen::to_string(router.error()));
        return 1;
    }
    auto surface = worldgen::SurfaceSystem::load(options.data, "overworld", options.seed, *blocks);
    if (!surface) {
        OV_LOG_ERROR("surface rules: {}", worldgen::to_string(surface.error()));
        return 1;
    }

    const auto air_name = [&](registry::BlockStateId state) {
        return state == registry::kAirState ? std::string_view("minecraft:air")
                                            : blocks->block_name(blocks->block_of(state));
    };
    const auto stone_block = blocks->find_block("minecraft:stone");
    const auto water_block = blocks->find_block("minecraft:water");
    const auto lava_block  = blocks->find_block("minecraft:lava");
    if (!stone_block || !water_block || !lava_block) {
        OV_LOG_ERROR("the registry is missing stone, water or lava");
        return 1;
    }
    const auto stone = blocks->default_state(*stone_block);
    const auto water = blocks->default_state(*water_block);
    const auto lava  = blocks->default_state(*lava_block);

    usize                        chunks_read = 0;
    Tally                        cells;
    Tally                        columns;
    Tally                        bedrock;
    std::array<Tally, 16>        by_layer{};
    std::array<Tally, 5>         bedrock_by_layer{};
    std::map<std::string, Tally> by_biome;
    std::map<std::string, usize> confusions;
    /// Cells where the game's block came from a stage that runs after this one.
    /// Reported rather than counted, because counting them either way would be
    /// a lie in one direction or the other.
    usize                        after_the_fact = 0;
    // ── worldgen-3 ── The berg ice over the sea bed of frozen ocean columns,
    // the game's against ours.
    struct IceTally {
        usize columns{0};
        usize theirs{0};
        usize ours{0};
        usize both{0};
        usize same{0};
        usize ours_only_columns{0};
        usize theirs_only_columns{0};
        std::map<std::string, usize> ours_only;
        std::map<i32, usize>         top_delta;
        std::map<i32, usize>         low_delta;
    } ice;
    // ── worldgen-3 ── Eroded badlands columns: where our noise raises a
    // pillar, is the game's terrain top that pillar's top?
    struct PillarTally {
        usize                columns{0};
        usize                raised{0};
        usize                exact{0};
        usize                hidden{0};
        usize                missing{0};
        std::map<i32, usize> delta;
    } pillars;
    std::map<std::string, usize> later_stage;
    std::vector<std::string>     shown;

    /// The surface depth, measured rather than argued about.
    ///
    /// A plains column the game finished says its own depth out loud: grass on
    /// top and then exactly `surface_depth` blocks of dirt, because the soil
    /// rule is `stone_depth(floor, add_surface_depth)` and that is what
    /// `depth <= 1 + surface_depth` means. So counting the dirt gives the
    /// number the game computed, and the difference from ours is a direct
    /// reading of whether the formula and its random draw are right — with no
    /// other rule in the way.
    std::array<usize, 17> depth_delta{};
    usize                 depth_samples = 0;

    /// Where the disagreements sit relative to a biome cell's edge.
    ///
    /// Biomes are stored per 4x4x4 cell, and this harness reads that array
    /// straight. Vanilla does not: `BiomeManager` fuzzes the lookup by a
    /// seeded offset of up to about half a cell, so a block near an edge can
    /// resolve to the neighbouring cell's biome and take a different branch of
    /// the rules entirely. Splitting the disagreements this way says how much
    /// of what is left is that and not the rules — a desert column with three
    /// bands of terracotta in it is not a surface bug, it is a badlands cell
    /// half a cell away.
    usize wrong_at_edge     = 0;
    usize wrong_in_interior = 0;

    std::vector<registry::BlockStateId> column(static_cast<usize>(kHeight));
    ReferenceChunk                      reference;

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
            // Only finished chunks. A partial one has the default biome array
            // and would make every disagreement say "plains".
            const nbt::Tag* status = document->root.find("Status");
            if (status == nullptr || status->as_string() != "minecraft:full") {
                continue;
            }
            if (!decode(*document, reference)) {
                continue;
            }
            ++chunks_read;
            ++from_this_region;

            const i32 origin_x = reference.chunk_x * 16;
            const i32 origin_z = reference.chunk_z * 16;

            // Two different tops, and conflating them cost an afternoon.
            //
            // WORLD_SURFACE_WG — where the rules start walking down, and what
            // `steep` compares — is the highest block that is not *air*, so in
            // an ocean it is the top of the water, sixteen blocks above the sea
            // bed. Starting the walk at the sea bed instead means the water is
            // never seen, `water_height` stays at "no water", every ocean floor
            // takes the land branch, and the game's gravel comes back as grass.
            // That single mistake was worth 30 % in cold_ocean.
            //
            // The comparison, on the other hand, wants the highest *terrain*
            // block: that is where the surface rules' output actually is.
            std::array<i32, 256> heights{};
            std::array<i32, 256> terrain_tops{};
            for (i32 cz = 0; cz < 16; ++cz) {
                for (i32 cx = 0; cx < 16; ++cx) {
                    const usize slot = static_cast<usize>(cz) * 16 + static_cast<usize>(cx);
                    i32         top  = kMinY;
                    i32         solid = kMinY;
                    const bool frozen_here = frozen_column(reference, cx, cz);
                    for (i32 y = kMinY + kHeight - 1; y >= kMinY; --y) {
                        const Kind kind = classify_in(reference.block(cx, y, cz), y, frozen_here);
                        if (top == kMinY && kind != Kind::Air) {
                            top = y;
                        }
                        if (kind == Kind::Terrain) {
                            solid = y;
                            break;
                        }
                    }
                    heights[slot]      = top;
                    terrain_tops[slot] = solid;
                }
            }
            const ReferenceQueries queries{reference,     *blocks,      *router,
                                           heights,       terrain_tops, options.prelim_reference};

            for (i32 cz = 0; cz < 16; ++cz) {
                for (i32 cx = 0; cx < 16; ++cx) {
                    const i32 world_x = origin_x + cx;
                    const i32 world_z = origin_z + cz;
                    const i32 top     = terrain_tops[static_cast<usize>(cz) * 16 +
                                                 static_cast<usize>(cx)];
                    if (top <= kMinY) {
                        continue;
                    }
                    const std::string biome_here{reference.biome(cx, top, cz)};
                    if (!options.biome.empty() && biome_here != options.biome) {
                        continue;
                    }

                    // ── worldgen-3 ── The pillar our noise raises here, against
                    // the game's terrain top.
                    if (biome_here == "minecraft:eroded_badlands" && !options.no_rules) {
                        std::vector<registry::BlockStateId> bare(column.size(), registry::kAirState);
                        for (i32 y = kMinY; y <= 63; ++y) {
                            bare[static_cast<usize>(y - kMinY)] = stone;
                        }
                        const PillarQueries pillar_queries{queries};
                        surface->build_column(bare, world_x, world_z, pillar_queries, *blocks);
                        i32 peak = 63;
                        for (i32 y = kMinY + kHeight - 1; y > 63; --y) {
                            if (bare[static_cast<usize>(y - kMinY)] != registry::kAirState) {
                                peak = y;
                                break;
                            }
                        }
                        ++pillars.columns;
                        if (peak > 63) {
                            ++pillars.raised;
                            if (top == peak) {
                                ++pillars.exact;
                            } else if (top > peak) {
                                ++pillars.hidden;
                            } else {
                                ++pillars.missing;
                            }
                            ++pillars.delta[std::clamp(top - peak, -20, 20)];
                        }
                    }

                    // Strip the column back to what the noise stage left.
                    const bool frozen = frozen_column(reference, cx, cz);
                    for (i32 y = kMinY; y < kMinY + kHeight; ++y) {
                        const std::string_view name = reference.block(cx, y, cz);
                        const auto             slot = static_cast<usize>(y - kMinY);
                        switch (classify_in(name, y, frozen)) {
                            case Kind::Air:
                                column[slot] = registry::kAirState;
                                break;
                            case Kind::Fluid:
                                column[slot] = name == "minecraft:lava" ? lava : water;
                                break;
                            case Kind::Terrain:
                                column[slot] = stone;
                                break;
                        }
                    }

                    if (!options.no_rules) {
                        surface->build_column(column, world_x, world_z, queries, *blocks);
                    }

                    // ── worldgen-3 ── Every cell over the sea bed: the ice the
                    // pass stacked against the game's. The game's also holds the
                    // iceberg *feature*'s ice, which this pass does not place.
                    if (frozen) {
                        ++ice.columns;
                        i32 their_top = kMinY - 1, our_top = kMinY - 1;
                        i32 their_low = kMinY + kHeight, our_low = kMinY + kHeight;
                        for (i32 y = top + 1; y < kMinY + kHeight; ++y) {
                            const std::string_view theirs = reference.block(cx, y, cz);
                            const std::string_view ours =
                                air_name(column[static_cast<usize>(y - kMinY)]);
                            const bool their_ice = is_berg_ice(theirs);
                            const bool our_ice   = is_berg_ice(ours);
                            ice.theirs += their_ice ? 1 : 0;
                            ice.ours += our_ice ? 1 : 0;
                            ice.both += (their_ice && our_ice) ? 1 : 0;
                            ice.same += (their_ice && theirs == ours) ? 1 : 0;
                            if (our_ice && !their_ice) {
                                ++ice.ours_only[fmt::format("{} {}", y >= 63 ? "above" : "below",
                                                            canonical(theirs))];
                            }
                            if (their_ice) {
                                their_top = std::max(their_top, y);
                                their_low = std::min(their_low, y);
                            }
                            if (our_ice) {
                                our_top = std::max(our_top, y);
                                our_low = std::min(our_low, y);
                            }
                        }
                        if (their_top >= kMinY && our_top >= kMinY) {
                            ++ice.top_delta[our_top - their_top];
                            ++ice.low_delta[our_low - their_low];
                        } else if (our_top >= kMinY) {
                            ++ice.ours_only_columns;
                        } else if (their_top >= kMinY) {
                            ++ice.theirs_only_columns;
                        }
                    }

                    if (reference.block(cx, top, cz) == "minecraft:grass_block") {
                        i32 their_depth = 0;
                        while (their_depth < 8 &&
                               reference.block(cx, top - their_depth - 1, cz) ==
                                   "minecraft:dirt") {
                            ++their_depth;
                        }
                        const i32 ours_depth = surface->surface_depth(world_x, world_z);
                        ++depth_delta[static_cast<usize>(
                            std::clamp(ours_depth - their_depth + 8, 0, 16))];
                        ++depth_samples;
                    }

                    const bool under_water =
                        classify_in(reference.block(cx, top + 1, cz), top + 1, frozen) ==
                        Kind::Fluid;

                    bool whole_column = true;
                    for (i32 step = 0; step < options.depth; ++step) {
                        const i32 y = top - step;
                        if (y < kMinY) {
                            break;
                        }
                        const std::string_view theirs = canonical(reference.block(cx, y, cz));
                        const std::string_view ours =
                            air_name(column[static_cast<usize>(y - kMinY)]);
                        // Three kinds of block that a stage *after* this one
                        // puts exactly where our rules correctly left something
                        // else. They are attributions, not proofs — a real bug
                        // could hide behind any of them — so they are named,
                        // counted and reported apart rather than folded in.
                        //
                        //  * the ore stage turns stone into granite, andesite,
                        //    coal ore and the rest;
                        //  * `ore_dirt` and `ore_gravel` are ore features too,
                        //    and put blobs of dirt and gravel in deep stone;
                        //  * `disk_sand`, `disk_gravel` and `disk_clay` lay a
                        //    patch over the top of a river bed that our rules
                        //    correctly filled with dirt.
                        const bool ours_is_stone =
                            ours == "minecraft:stone" || ours == "minecraft:deepslate";
                        const bool blob = (theirs == "minecraft:dirt" ||
                                           theirs == "minecraft:gravel") &&
                                          ours_is_stone && step >= 2;
                        const bool disk = (theirs == "minecraft:sand" ||
                                           theirs == "minecraft:gravel" ||
                                           theirs == "minecraft:clay") &&
                                          ours == "minecraft:dirt" && under_water;
                        // Podzol is a surface rule's output in exactly two
                        // biomes — the two old-growth taigas name it — and a
                        // *feature*'s output everywhere else: the bamboo
                        // vegetation lays discs of it under a bamboo jungle,
                        // long after this stage. Read from the rule file rather
                        // than assumed, which is why the two names are here.
                        const bool podzol_feature =
                            theirs == "minecraft:podzol" &&
                            biome_here != "minecraft:old_growth_pine_taiga" &&
                            biome_here != "minecraft:old_growth_spruce_taiga";
                        if (is_feature_block(theirs) || is_underwater_plant(theirs) ||
                            podzol_feature || (is_ore_stage_block(theirs) && ours_is_stone) ||
                            blob || disk) {
                            ++after_the_fact;
                            ++later_stage[fmt::format("{:>22} -> {}", theirs, ours)];
                            continue;
                        }
                        ++cells.compared;
                        ++by_layer[static_cast<usize>(std::min(step, 15))].compared;
                        ++by_biome[biome_here].compared;
                        if (theirs == ours) {
                            ++cells.agreed;
                            ++by_layer[static_cast<usize>(std::min(step, 15))].agreed;
                            ++by_biome[biome_here].agreed;
                        } else {
                            whole_column = false;
                            ++confusions[fmt::format("{:>34} -> {}", theirs, ours)];
                            // Is a different biome within reach of the fuzz?
                            // Only the six face neighbours of the cell: the
                            // offset is under half a cell, so a diagonal cell
                            // cannot be reached.
                            const std::string_view mine = reference.biome(cx, y, cz);
                            bool                   edge = false;
                            for (const auto& step2 : std::array<std::array<i32, 3>, 6>{
                                     std::array<i32, 3>{4, 0, 0}, std::array<i32, 3>{-4, 0, 0},
                                     std::array<i32, 3>{0, 4, 0}, std::array<i32, 3>{0, -4, 0},
                                     std::array<i32, 3>{0, 0, 4}, std::array<i32, 3>{0, 0, -4}}) {
                                const i32 nx = cx + step2[0];
                                const i32 ny = y + step2[1];
                                const i32 nz = cz + step2[2];
                                if (nx < 0 || nx > 15 || nz < 0 || nz > 15 || ny < kMinY ||
                                    ny >= kMinY + kHeight) {
                                    continue;
                                }
                                if (reference.biome(nx, ny, nz) != mine) {
                                    edge = true;
                                    break;
                                }
                            }
                            if (edge) {
                                ++wrong_at_edge;
                            } else {
                                ++wrong_in_interior;
                            }
                        }
                    }
                    ++columns.compared;
                    if (whole_column) {
                        ++columns.agreed;
                    } else if (static_cast<i32>(shown.size()) < options.show) {
                        std::string line = fmt::format(
                            "  ({:>7},{:>7}) top {:>4} surface_height {:>4} depth {} {:<28}",
                            world_x, world_z, top,
                            heights[static_cast<usize>(cz) * 16 + static_cast<usize>(cx)],
                            surface->surface_depth(world_x, world_z), biome_here);
                        for (i32 step = -4; step < options.depth; ++step) {
                            const i32 y = top - step;
                            line += fmt::format(
                                "\n      y{:>5}  game {:<28} ours {:<22} biome {}", y,
                                reference.block(cx, y, cz),
                                air_name(column[static_cast<usize>(y - kMinY)]),
                                reference.biome(cx, y, cz));
                        }
                        shown.push_back(std::move(line));
                    }

                    // The bedrock floor, on its own. It is decided by a
                    // positional draw and nothing else, so it is the one part
                    // of this stage that can be wrong in isolation — and the
                    // one part where a percentage means exactly what it says.
                    for (i32 y = kMinY; y < kMinY + 5; ++y) {
                        const std::string_view theirs = canonical(reference.block(cx, y, cz));
                        const std::string_view ours =
                            air_name(column[static_cast<usize>(y - kMinY)]);
                        if (theirs.empty()) {
                            continue;
                        }
                        const bool their_bedrock = theirs == "minecraft:bedrock";
                        const bool our_bedrock   = ours == "minecraft:bedrock";
                        ++bedrock.compared;
                        ++bedrock_by_layer[static_cast<usize>(y - kMinY)].compared;
                        if (their_bedrock == our_bedrock) {
                            ++bedrock.agreed;
                            ++bedrock_by_layer[static_cast<usize>(y - kMinY)].agreed;
                        }
                    }
                }
            }
        }
    }

    const auto percent = [](const Tally& tally) {
        return tally.compared == 0
                   ? 0.0
                   : 100.0 * static_cast<f64>(tally.agreed) / static_cast<f64>(tally.compared);
    };

    fmt::print("\nseed {}, {} full chunks, {} blocks below each surface\n", options.seed,
               chunks_read, options.depth);
    fmt::print("columns whose {} layers all match: {} / {}  ({:.3f} %)\n", options.depth,
               columns.agreed, columns.compared, percent(columns));
    fmt::print("individual blocks:                 {} / {}  ({:.3f} %)\n", cells.agreed,
               cells.compared, percent(cells));
    fmt::print("blocks a later stage owns, counted apart: {}\n", after_the_fact);
    {
        std::vector<std::pair<std::string, usize>> ranked{later_stage.begin(), later_stage.end()};
        std::ranges::sort(ranked, [](const auto& a, const auto& b) { return a.second > b.second; });
        for (usize i = 0; i < std::min<usize>(ranked.size(), 8); ++i) {
            fmt::print("    {:>8}  {}\n", ranked[i].second, ranked[i].first);
        }
    }

    fmt::print("\nby depth below the surface:\n");
    for (i32 step = 0; step < std::min(options.depth, 16); ++step) {
        const Tally& tally = by_layer[static_cast<usize>(step)];
        if (tally.compared == 0) {
            continue;
        }
        fmt::print("  -{:<3} {:>8} / {:<8} ({:6.2f} %)\n", step, tally.agreed, tally.compared,
                   percent(tally));
    }

    fmt::print("\nof the {} blocks we got wrong: {} sit next to a different biome cell, {} do "
               "not\n",
               wrong_at_edge + wrong_in_interior, wrong_at_edge, wrong_in_interior);

    fmt::print("\nour surface depth minus the game's, read off {} grass columns:\n",
               depth_samples);
    for (usize slot = 0; slot < depth_delta.size(); ++slot) {
        if (depth_delta[slot] == 0) {
            continue;
        }
        fmt::print("  {:+3}  {:>8}  ({:6.2f} %)\n", static_cast<i32>(slot) - 8, depth_delta[slot],
                   depth_samples == 0 ? 0.0
                                      : 100.0 * static_cast<f64>(depth_delta[slot]) /
                                            static_cast<f64>(depth_samples));
    }

    if (ice.columns > 0) {
        const auto share = [](usize part, usize whole) {
            return whole == 0 ? 0.0 : 100.0 * static_cast<f64>(part) / static_cast<f64>(whole);
        };
        fmt::print("\nberg ice over the sea bed, {} frozen ocean columns:\n", ice.columns);
        fmt::print("  game {}  ours {}  both ice {}  same block {}\n", ice.theirs, ice.ours,
                   ice.both, ice.same);
        fmt::print("  of ours, also the game's: {:.3f} %   of the game's, also ours: {:.3f} %\n",
                   share(ice.both, ice.ours), share(ice.both, ice.theirs));
        fmt::print("  columns with ice on our side only {}, on the game's only {}\n",
                   ice.ours_only_columns, ice.theirs_only_columns);
        fmt::print("  our cells the game left without ice:\n");
        for (const auto& [what, count] : ice.ours_only) {
            fmt::print("    {:>6}  {}\n", count, what);
        }
        fmt::print("  our top minus the game's, per column with ice on both sides:\n");
        for (const auto& [delta, count] : ice.top_delta) {
            fmt::print("    {:+4}  {:>6}\n", delta, count);
        }
        fmt::print("  our lowest minus the game's:\n");
        for (const auto& [delta, count] : ice.low_delta) {
            fmt::print("    {:+4}  {:>6}\n", delta, count);
        }
    }

    if (pillars.columns > 0) {
        fmt::print("\neroded badlands pillars, {} columns: our noise raises one in {}\n",
                   pillars.columns, pillars.raised);
        fmt::print("  the game's top is our pillar's top: {}   higher (terrain hides it): {}   "
                   "lower (no pillar in the game): {}\n",
                   pillars.exact, pillars.hidden, pillars.missing);
        for (const auto& [delta, count] : pillars.delta) {
            fmt::print("    game top minus ours {:+3}  {:>6}\n", delta, count);
        }
    }

    fmt::print("\nthe bedrock floor: {} / {} ({:.3f} %)\n", bedrock.agreed, bedrock.compared,
               percent(bedrock));
    for (i32 y = kMinY; y < kMinY + 5; ++y) {
        const Tally& tally = bedrock_by_layer[static_cast<usize>(y - kMinY)];
        if (tally.compared == 0) {
            continue;
        }
        fmt::print("  y {:>4}  {:>8} / {:<8} ({:6.2f} %)\n", y, tally.agreed, tally.compared,
                   percent(tally));
    }

    fmt::print("\nby biome, worst first:\n");
    std::vector<std::pair<std::string, Tally>> ranked{by_biome.begin(), by_biome.end()};
    std::ranges::sort(ranked, [&](const auto& a, const auto& b) {
        return percent(a.second) < percent(b.second);
    });
    for (const auto& [name, tally] : ranked) {
        fmt::print("  {:<34} {:>8} / {:<8} ({:6.2f} %)\n", name, tally.agreed, tally.compared,
                   percent(tally));
    }

    fmt::print("\nwhat the game had where we said something else, most common first:\n");
    std::vector<std::pair<std::string, usize>> pairs{confusions.begin(), confusions.end()};
    std::ranges::sort(pairs, [](const auto& a, const auto& b) { return a.second > b.second; });
    for (usize i = 0; i < std::min<usize>(pairs.size(), 25); ++i) {
        fmt::print("  {:>8}  {}\n", pairs[i].second, pairs[i].first);
    }

    for (const std::string& line : shown) {
        fmt::print("\n{}\n", line);
    }
    return 0;
}
