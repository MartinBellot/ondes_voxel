// Structure blocks: our template layer against the blocks the game wrote.
//
// Level A — the game's own pieces, our code. Every finished chunk of a
// reference world that starts a template structure stores the pieces it chose:
// template, origin, rotation, mirror, integrity. Handing exactly those to our
// `StructureBuilder::place` and comparing what it writes with the blocks the
// game wrote isolates the template layer — the reader, the transforms, the
// processors, the data markers — from everything that decides *which* pieces.
// The piece is placed chunk column by chunk column, as the game does.
//
// Level B — our pieces, the game's pieces. `StructureBuilder::generate` from the
// seed alone, against the stored children, in every chunk that has a start —
// finished or not, since the pieces are decided long before the blocks.
//
// `--witness` places every piece a quarter-turn off. A comparison that still
// passes with the wrong rotation measures nothing (trap 14 of the project).
//
// One caveat is structural and named rather than hidden: level A reads the
// finished world as the "world before the structure" wherever the placement
// asks the world something (a water source to keep, a protected block). The
// structure is already in that world. It matters for waterlogging only, and it
// can only make a waterlogged block agree with itself.
#define OV_LOG_CATEGORY "structblocks"

#include "ov/base/log.hpp"
#include "ov/gameplay/weather.hpp"
#include "ov/nbt/region.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/world/chunk_storage.hpp"
#include "ov/worldgen/aquifer.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/decoration.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/pipeline.hpp"
#include "ov/worldgen/placement.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_pieces.hpp"
#include "ov/worldgen/structure_set.hpp"
#include "ov/worldgen/structure_stage.hpp"
#include "ov/worldgen/structure_template.hpp"
#include "ov/worldgen/surface_system.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace ov;

namespace {

struct Options {
    std::filesystem::path world{"run/struct-locate-1234567890/world"};
    std::filesystem::path jar{"tools/vanilla/server.jar"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    std::filesystem::path registries{"data/vanilla/1.20.1/registry.ovpack"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    i64                   seed{1234567890};
    char                  level{'a'};
    bool                  witness{false};
    std::string           only;
    i32                   show{6};
    /// The structure's index at its generation step, for the loot seeds.
    i32 structure_index{-1};
    /// Search the index the loot seeds imply, instead of comparing.
    bool find_index{false};
    /// Level C: run the decoration too (the real pipeline), or terrain and
    /// structures only.
    bool                  features{true};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    /// ── portals ── The dimension levels B and H generate: `overworld` or
    /// `the_nether` (the world is then the `DIM-1` directory).
    std::string dimension{"overworld"};
};

[[nodiscard]] Options parse(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto             value = [&](std::string_view flag) {
            return std::string{argument.substr(flag.size())};
        };
        if (argument.starts_with("--world=")) {
            options.world = value("--world=");
        } else if (argument.starts_with("--jar=")) {
            options.jar = value("--jar=");
        } else if (argument.starts_with("--pack=")) {
            options.pack = value("--pack=");
        } else if (argument.starts_with("--data=")) {
            options.data = value("--data=");
        } else if (argument.starts_with("--seed=")) {
            options.seed = std::atoll(value("--seed=").c_str());
        } else if (argument.starts_with("--dimension=")) {
            options.dimension = value("--dimension=");
        } else if (argument.starts_with("--level=")) {
            options.level = value("--level=").front();
        } else if (argument == "--witness") {
            options.witness = true;
        } else if (argument == "--find-index") {
            options.find_index = true;
        } else if (argument == "--no-features") {
            options.features = false;
        } else if (argument.starts_with("--only=")) {
            options.only = value("--only=");
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=").c_str());
        } else if (argument.starts_with("--structure-index=")) {
            options.structure_index = std::atoi(value("--structure-index=").c_str());
        }
    }
    return options;
}

/// The structures whose pieces the builder knows.
[[nodiscard]] bool in_scope(std::string_view name) {
    return name == "minecraft:igloo" || name.starts_with("minecraft:shipwreck") ||
           name.starts_with("minecraft:ocean_ruin") ||
           name.starts_with("minecraft:ruined_portal") || name == "minecraft:buried_treasure" ||
           // ── temples ── the scattered pieces
           name == "minecraft:swamp_hut" || name == "minecraft:desert_pyramid" ||
           name == "minecraft:jungle_pyramid" ||
           // ── jigsaw ──
           name.starts_with("minecraft:village_") || name == "minecraft:pillager_outpost" ||
           name == "minecraft:bastion_remnant" || name == "minecraft:ancient_city" ||
           name == "minecraft:trail_ruins";
}

[[nodiscard]] std::string label(const registry::BlockRegistry& blocks,
                                registry::BlockStateId         state) {
    const auto  block = blocks.block_of(state);
    std::string text{blocks.block_name(block)};
    const auto  properties = blocks.properties(block);
    if (!properties.empty()) {
        text += '[';
        bool first = true;
        for (const auto& property : properties) {
            if (!first) {
                text += ',';
            }
            first = false;
            text += fmt::format("{}={}", property.name, blocks.property_value(state, property));
        }
        text += ']';
    }
    return text;
}

struct StoredStart {
    std::string name;
    i32         chunk_x{0};
    i32         chunk_z{0};
    bool        full{false};
    nbt::Tag    children;
};

/// The reference world, read lazily chunk by chunk.
class ReferenceWorld {
public:
    ReferenceWorld(std::filesystem::path region_dir, const registry::BlockRegistry& blocks,
                   const registry::Registries* registries)
        : dir_(std::move(region_dir)), blocks_(&blocks), registries_(registries) {
        for (u32 index = 0; index < blocks.biome_count(); ++index) {
            biome_names_.push_back(blocks.biome_name(index));
        }
    }

    /// The chunk, if the game finished it.
    [[nodiscard]] const world::Chunk* full_chunk(i32 chunk_x, i32 chunk_z) {
        const i64 key = (static_cast<i64>(chunk_x) << 32) | static_cast<u32>(chunk_z);
        if (const auto it = chunks_.find(key); it != chunks_.end()) {
            return it->second ? &*it->second : nullptr;
        }
        auto&      slot   = chunks_[key];
        const auto region = region_for(chunk_x >> 5, chunk_z >> 5);
        if (region == nullptr) {
            return nullptr;
        }
        const auto local_x = static_cast<u32>(chunk_x & 31);
        const auto local_z = static_cast<u32>(chunk_z & 31);
        if (!region->has_chunk(local_x, local_z)) {
            return nullptr;
        }
        auto document = region->read_chunk(local_x, local_z);
        if (!document) {
            return nullptr;
        }
        const nbt::Tag* status = document->root.find("Status");
        if (status == nullptr || status->as_string() != "minecraft:full") {
            return nullptr;
        }
        const world::ChunkCodecContext context{blocks_, biome_names_, registries_,
                                               world::AirStates::from(*blocks_)};
        slot = world::from_nbt(*document, context);
        return slot ? &*slot : nullptr;
    }

private:
    [[nodiscard]] const nbt::RegionFile* region_for(i32 region_x, i32 region_z) {
        const auto key = std::pair{region_x, region_z};
        if (const auto it = regions_.find(key); it != regions_.end()) {
            return it->second ? &*it->second : nullptr;
        }
        auto&      slot = regions_[key];
        const auto path = dir_ / fmt::format("r.{}.{}.mca", region_x, region_z);
        if (std::filesystem::exists(path)) {
            if (auto opened = nbt::RegionFile::open(path)) {
                slot = std::move(*opened);
            }
        }
        return slot ? &*slot : nullptr;
    }

    std::filesystem::path                                         dir_;
    const registry::BlockRegistry*                                blocks_;
    const registry::Registries*                                   registries_;
    std::vector<std::string_view>                                 biome_names_;
    std::map<std::pair<i32, i32>, std::optional<nbt::RegionFile>> regions_;
    std::unordered_map<i64, std::optional<world::Chunk>>          chunks_;
};

[[nodiscard]] i64 key3(i32 x, i32 y, i32 z) {
    return (static_cast<i64>(x & 0x3FFFFFF) << 38) | (static_cast<i64>(z & 0x3FFFFFF) << 12) |
           static_cast<i64>(y & 0xFFF);
}

/// The reference world as a level our placement writes into. Writes go to an
/// overlay and are what gets compared; reads see the overlay first.
class OverlayLevel final : public worldgen::StructureLevel {
public:
    OverlayLevel(ReferenceWorld& world, const registry::BlockRegistry& blocks)
        : world_(&world), blocks_(&blocks) {}

    struct Write {
        i32                    x;
        i32                    y;
        i32                    z;
        registry::BlockStateId state;
    };

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        if (const auto it = writes_.find(key3(x, y, z)); it != writes_.end()) {
            return it->second.state;
        }
        const auto reference = reference_block(x, y, z).value_or(registry::kAirState);
        // The finished world already holds the structure's own containers,
        // and `protected_blocks` would refuse to overwrite them. Before the
        // structure there was water or nothing there; which, the chest's own
        // waterlogging says.
        const std::string_view name = blocks_->block_name(blocks_->block_of(reference));
        if (name == "minecraft:chest" || name == "minecraft:trapped_chest" ||
            name == "minecraft:barrel") {
            if (worldgen::holds_water_source(*blocks_, reference)) {
                if (const auto water = blocks_->find_block("minecraft:water")) {
                    return blocks_->default_state(*water);
                }
            }
            return registry::kAirState;
        }
        return reference;
    }

    [[nodiscard]] std::optional<registry::BlockStateId> reference_block(i32 x, i32 y, i32 z) const {
        const world::Chunk* chunk = world_->full_chunk(x >> 4, z >> 4);
        if (chunk == nullptr || y < -64 || y > 319) {
            return std::nullopt;
        }
        return chunk->get_block(static_cast<usize>(x & 15), y, static_cast<usize>(z & 15));
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        writes_[key3(x, y, z)] = Write{x, y, z, state};
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType type, i32 x, i32 z) const override {
        const world::Chunk* chunk = world_->full_chunk(x >> 4, z >> 4);
        if (chunk == nullptr) {
            return -64;
        }
        const auto stored =
            type == world::HeightmapType::OceanFloorWG     ? world::HeightmapType::OceanFloor
            : type == world::HeightmapType::WorldSurfaceWG ? world::HeightmapType::WorldSurface
                                                           : type;
        return chunk->heightmap(stored).first_free(static_cast<usize>(x & 15),
                                                   static_cast<usize>(z & 15));
    }

    [[nodiscard]] std::string_view biome_at(i32 /*x*/, i32 /*y*/, i32 /*z*/) const override {
        return "minecraft:plains";
    }

    [[nodiscard]] i32 min_y() const override { return -64; }

    [[nodiscard]] i32 world_height() const override { return 384; }

    [[nodiscard]] i32 sea_level() const override { return 63; }

    void set_block_entity(i32 x, i32 y, i32 z, nbt::Tag data) override {
        entities_[key3(x, y, z)] = std::move(data);
    }

    [[nodiscard]] const nbt::Tag* block_entity(i32 x, i32 y, i32 z) const override {
        if (const auto it = entities_.find(key3(x, y, z)); it != entities_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] const std::unordered_map<i64, Write>& writes() const noexcept { return writes_; }

    [[nodiscard]] const std::unordered_map<i64, nbt::Tag>& entities() const noexcept {
        return entities_;
    }

    [[nodiscard]] const nbt::Tag* reference_entity(i32 x, i32 y, i32 z) const {
        const world::Chunk* chunk = world_->full_chunk(x >> 4, z >> 4);
        if (chunk == nullptr) {
            return nullptr;
        }
        const auto* entity =
            chunk->block_entity_at(static_cast<usize>(x & 15), y, static_cast<usize>(z & 15));
        return entity != nullptr ? &entity->data : nullptr;
    }

private:
    ReferenceWorld*                   world_;
    const registry::BlockRegistry*    blocks_;
    std::unordered_map<i64, Write>    writes_;
    std::unordered_map<i64, nbt::Tag> entities_;
};

struct Tally {
    i32                        starts{0};
    i32                        pieces{0};
    i64                        compared{0};
    i64                        same{0};
    i64                        other_block{0};
    i64                        other_state{0};
    i64                        unread{0};
    i32                        chests{0};
    i32                        chests_table_ok{0};
    i32                        chests_seed_ok{0};
    std::map<std::string, i32> mismatches;
    std::set<std::string>      unknown_markers;
};

[[nodiscard]] std::vector<StoredStart> read_starts(const std::filesystem::path& world,
                                                   std::string_view             only) {
    std::vector<StoredStart>           out;
    std::vector<std::filesystem::path> regions;
    for (const auto& entry : std::filesystem::directory_iterator(world / "region")) {
        if (entry.path().extension() == ".mca") {
            regions.push_back(entry.path());
        }
    }
    std::sort(regions.begin(), regions.end());
    for (const auto& path : regions) {
        i32 region_x = 0;
        i32 region_z = 0;
        if (std::sscanf(path.stem().string().c_str(), "r.%d.%d", &region_x, &region_z) != 2) {
            continue;
        }
        auto region = nbt::RegionFile::open(path);
        if (!region) {
            continue;
        }
        for (u32 local_z = 0; local_z < 32; ++local_z) {
            for (u32 local_x = 0; local_x < 32; ++local_x) {
                if (!region->has_chunk(local_x, local_z)) {
                    continue;
                }
                auto document = region->read_chunk(local_x, local_z);
                if (!document) {
                    continue;
                }
                const nbt::Tag* structures = document->root.find("structures");
                const nbt::Tag* starts =
                    structures != nullptr ? structures->find("starts") : nullptr;
                if (starts == nullptr || starts->compound() == nullptr) {
                    continue;
                }
                const nbt::Tag* status = document->root.find("Status");
                for (const nbt::CompoundEntry& entry : *starts->compound()) {
                    const nbt::Tag* id = entry.value.find("id");
                    if (id != nullptr && id->as_string() == "INVALID") {
                        continue;
                    }
                    if (!in_scope(entry.name) ||
                        (!only.empty() && entry.name.find(only) == std::string::npos)) {
                        continue;
                    }
                    const nbt::Tag* children = entry.value.find("Children");
                    if (children == nullptr) {
                        continue;
                    }
                    out.push_back({entry.name, region_x * 32 + static_cast<i32>(local_x),
                                   region_z * 32 + static_cast<i32>(local_z),
                                   status != nullptr && status->as_string() == "minecraft:full",
                                   *children});
                }
            }
        }
    }
    return out;
}

/// Place every finished start's pieces and compare. `structure_index` is the
/// index the loot-seed random is built with.
[[nodiscard]] std::map<std::string, Tally> measure_level_a(
    const Options& options, i32 forced_index, const registry::BlockRegistry& blocks,
    const worldgen::StructureBuilder& builder, const worldgen::StructurePlacer& placer,
    ReferenceWorld& world, const std::vector<StoredStart>& starts) {
    std::map<std::string, Tally> tallies;
    const auto                   kind = worldgen::configured_feature_random();

    for (const StoredStart& start : starts) {
        if (!start.full) {
            continue;
        }
        Tally&                                tally = tallies[start.name];
        std::vector<worldgen::StructurePiece> pieces;
        bool                                  refused = false;
        for (const nbt::Tag& child : *start.children.list()) {
            auto piece = builder.piece_from_nbt(child);
            if (!piece) {
                tally.unknown_markers.insert(piece.error());
                refused = true;
                break;
            }
            if (options.witness) {
                piece->rotation =
                    worldgen::compose(piece->rotation, worldgen::Rotation::Clockwise90);
                if (const auto* tpl = builder.templates().find(piece->template_name)) {
                    const i32 y = piece->box.min_y;
                    piece->box  = tpl->bounding_box(piece->origin, piece->mirror, piece->rotation,
                                                    piece->pivot);
                    piece->box.move(0, y - piece->box.min_y, 0);
                }
            }
            pieces.push_back(std::move(*piece));
        }
        if (refused) {
            continue;
        }
        ++tally.starts;

        OverlayLevel          level{world, blocks};
        worldgen::BoundingBox box = pieces.front().box;
        for (const worldgen::StructurePiece& piece : pieces) {
            box.encapsulate(piece.box);
        }
        tally.pieces += static_cast<i32>(pieces.size());
        const auto* definition = placer.find(start.name);
        const i32   step = definition != nullptr ? worldgen::step_ordinal(definition->step) : 4;
        const i32   structure_index =
            forced_index >= 0 ? forced_index : worldgen::structure_step_index(placer, start.name);
        std::vector<BlockPos> shaped;
        // Chunk column by chunk column, as the game places it: one random per
        // chunk for the structure, shared by its pieces in their order.
        for (i32 chunk_x = box.min_x >> 4; chunk_x <= box.max_x >> 4; ++chunk_x) {
            for (i32 chunk_z = box.min_z >> 4; chunk_z <= box.max_z >> 4; ++chunk_z) {
                const auto clip = worldgen::BoundingBox::chunk_column(chunk_x, chunk_z, -64, 319);
                const i64  decoration =
                    worldgen::decoration_seed(options.seed, chunk_x * 16, chunk_z * 16, kind);
                worldgen::FeatureRandom random{
                    kind, worldgen::feature_seed(decoration, structure_index, step)};
                for (const worldgen::StructurePiece& piece : pieces) {
                    if (!piece.box.intersects(clip)) {
                        continue;
                    }
                    const auto result = builder.place(level, piece, clip, random);
                    for (const std::string& marker : result.unknown_markers) {
                        tally.unknown_markers.insert(marker);
                    }
                    shaped.insert(shaped.end(), result.shaped.begin(), result.shaped.end());
                }
            }
        }
        // The game's post-processing, once every chunk the structure touches
        // is down: a fence at a chunk border sees its neighbour at last.
        worldgen::update_shapes(level, blocks, shaped);

        for (const auto& [key, write] : level.writes()) {
            const auto reference = level.reference_block(write.x, write.y, write.z);
            if (!reference) {
                ++tally.unread;
                continue;
            }
            ++tally.compared;
            if (*reference == write.state) {
                ++tally.same;
                continue;
            }
            if (blocks.block_of(*reference) == blocks.block_of(write.state)) {
                ++tally.other_state;
            } else {
                ++tally.other_block;
            }
            ++tally.mismatches[label(blocks, write.state) + "  ->  " + label(blocks, *reference)];
        }
        for (const auto& [key, entity] : level.entities()) {
            const nbt::Tag* table = entity.find("LootTable");
            if (table == nullptr) {
                continue;
            }
            // Recover the position from the overlay key's write.
            const auto write = level.writes().find(key);
            if (write == level.writes().end()) {
                continue;
            }
            ++tally.chests;
            const nbt::Tag* theirs =
                level.reference_entity(write->second.x, write->second.y, write->second.z);
            if (theirs == nullptr) {
                continue;
            }
            const nbt::Tag* their_table = theirs->find("LootTable");
            if (their_table != nullptr && their_table->as_string() == table->as_string()) {
                ++tally.chests_table_ok;
                const nbt::Tag* seed       = entity.find("LootTableSeed");
                const nbt::Tag* their_seed = theirs->find("LootTableSeed");
                if (seed != nullptr && their_seed != nullptr &&
                    seed->as_i64() == their_seed->as_i64()) {
                    ++tally.chests_seed_ok;
                }
            }
        }
    }
    return tallies;
}

[[nodiscard]] i32 run_level_a(const Options& options, const registry::BlockRegistry& blocks,
                              const registry::Registries*       registries,
                              const worldgen::StructureBuilder& builder) {
    ReferenceWorld world{options.world / "region", blocks, registries};
    const auto     starts = read_starts(options.world, options.only);
    auto           sets   = worldgen::StructureSetRegistry::load(options.data);
    if (!sets) {
        OV_LOG_ERROR("structure sets: {}", worldgen::to_string(sets.error()));
        return 1;
    }
    auto placer = worldgen::StructurePlacer::load(options.data, *sets);
    if (!placer) {
        OV_LOG_ERROR("structures: {}", worldgen::to_string(placer.error()));
        return 1;
    }

    if (options.find_index) {
        // Which index at its step the loot seeds say each structure has: one
        // pass per candidate, and a 64-bit seed does not match by chance.
        fmt::print("loot-seed index search, world {}\n", options.world.string());
        for (i32 index = 0; index < 48; ++index) {
            for (const auto& [name, tally] :
                 measure_level_a(options, index, blocks, builder, *placer, world, starts)) {
                if (tally.chests_seed_ok > 0) {
                    fmt::print("  index {:>2}: {:<32} {} of {} chest seeds\n", index, name,
                               tally.chests_seed_ok, tally.chests);
                }
            }
        }
        return 0;
    }
    const auto tallies =
        measure_level_a(options, options.structure_index, blocks, builder, *placer, world, starts);

    fmt::print("level A — the game's pieces placed by our code{}\nworld {}\n\n",
               options.witness ? "  [WITNESS: every piece a quarter-turn off]" : "",
               options.world.string());
    fmt::print("{:<32} {:>6} {:>6} {:>8} {:>8} {:>8} {:>7} {:>7} {:>12}\n", "structure", "starts",
               "pieces", "compared", "same", "%", "blk≠", "state≠", "chests t/s");
    for (const auto& [name, tally] : tallies) {
        fmt::print("{:<32} {:>6} {:>6} {:>8} {:>8} {:>7.3f}% {:>7} {:>7} {:>5}/{}/{}\n", name,
                   tally.starts, tally.pieces, tally.compared, tally.same,
                   tally.compared > 0
                       ? 100.0 * static_cast<f64>(tally.same) / static_cast<f64>(tally.compared)
                       : 0.0,
                   tally.other_block, tally.other_state, tally.chests, tally.chests_table_ok,
                   tally.chests_seed_ok);
    }
    for (const auto& [name, tally] : tallies) {
        if (tally.mismatches.empty() && tally.unknown_markers.empty()) {
            continue;
        }
        fmt::print("\n{}\n", name);
        std::vector<std::pair<i32, std::string>> sorted;
        for (const auto& [text, count] : tally.mismatches) {
            sorted.emplace_back(count, text);
        }
        std::sort(sorted.rbegin(), sorted.rend());
        for (usize index = 0; index < sorted.size() && index < static_cast<usize>(options.show);
             ++index) {
            fmt::print("  {:>5}  {}\n", sorted[index].first, sorted[index].second);
        }
        for (const std::string& marker : tally.unknown_markers) {
            fmt::print("  refused/unknown: {}\n", marker);
        }
    }
    return 0;
}

/// The world as the placer's biome filter sees it, from our own noise — the
/// same two heightmap rules as `ov_structparity`, duplicated rather than shared
/// so that a change there cannot quietly change this measurement.
///
/// ── portals ── With the block registry it also answers the base column's
/// substance and the temperature, which a ruined portal's height and cold test
/// read — the same rules as the server's sampler.
class NoiseSampler final : public worldgen::StructureWorldSampler {
public:
    explicit NoiseSampler(const worldgen::ChunkGenerator& generator,
                          const registry::BlockRegistry*  blocks = nullptr)
        : generator_{&generator},
          blocks_{blocks},
          low_{generator.gen_min_y()},
          high_{generator.gen_min_y() + generator.gen_depth() - 1},
          lava_{fluid_state(blocks, "minecraft:lava")} {}

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        return generator_->biome_name_at(x, y, z);
    }

    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        const i32 sea = generator_->sea_level();
        for (i32 y = high_; y >= low_; --y) {
            if (generator_->is_solid(x, y, z) || y < sea) {
                return y + 1;
            }
        }
        return low_;
    }

    [[nodiscard]] i32 ocean_floor_height(i32 x, i32 z) const override {
        for (i32 y = high_; y >= low_; --y) {
            if (generator_->is_solid(x, y, z)) {
                return y + 1;
            }
        }
        return low_;
    }

    [[nodiscard]] std::optional<bool> base_solid(i32 x, i32 y, i32 z) const override {
        return generator_->is_solid(x, y, z);
    }

    [[nodiscard]] std::optional<worldgen::Substance> base_substance(i32 x, i32 y,
                                                                    i32 z) const override {
        const f64 density = generator_->density_at(x, y, z);
        if (generator_->aquifer_active()) {
            worldgen::AquiferSampler aquifer{*generator_->aquifer()};
            return aquifer.compute(x, y, z, density).substance;
        }
        if (density > 0.0) {
            return worldgen::Substance::Solid;
        }
        const auto fluid = generator_->fluid_at(y);
        if (fluid == registry::kAirState) {
            return worldgen::Substance::Air;
        }
        return fluid == lava_ && lava_ != registry::kAirState ? worldgen::Substance::Lava
                                                              : worldgen::Substance::Water;
    }

    [[nodiscard]] std::optional<f32> temperature_at(i32 x, i32 y, i32 z) const override {
        if (blocks_ == nullptr) {
            return std::nullopt;
        }
        const auto biome = blocks_->find_biome(generator_->biome_name_at(x, y, z));
        if (!biome) {
            return std::nullopt;
        }
        return climate_.temperature_at(gameplay::climate_of(blocks_->biome(*biome)), {x, y, z});
    }

private:
    /// A fluid's default state, or air when there is no registry to ask — then
    /// every fluid reads as water, which is all the portal's settling tells
    /// apart (air or not).
    [[nodiscard]] static registry::BlockStateId fluid_state(const registry::BlockRegistry* blocks,
                                                            std::string_view               name) {
        if (blocks == nullptr) {
            return registry::kAirState;
        }
        const auto block = blocks->find_block(name);
        return block ? blocks->default_state(*block) : registry::kAirState;
    }

    const worldgen::ChunkGenerator* generator_;
    const registry::BlockRegistry*  blocks_;
    i32                             low_;
    i32                             high_;
    registry::BlockStateId          lava_;
    gameplay::ClimateNoise          climate_;
};

[[nodiscard]] i32 run_level_b(const Options& options, const registry::BlockRegistry& blocks,
                              const worldgen::StructureBuilder& builder) {
    auto sets = worldgen::StructureSetRegistry::load(options.data);
    if (!sets) {
        OV_LOG_ERROR("structure sets: {}", worldgen::to_string(sets.error()));
        return 1;
    }
    auto placer = worldgen::StructurePlacer::load(options.data, *sets);
    if (!placer) {
        OV_LOG_ERROR("structures: {}", worldgen::to_string(placer.error()));
        return 1;
    }
    // ── portals ── A real sampler: a portal's height and its cold test are
    // decided at generation, from the noise, and compared below.
    auto router = worldgen::NoiseRouter::load(options.data, options.dimension, options.seed);
    auto biomes = worldgen::BiomeSource::load(options.reports, options.dimension);
    if (!router || !biomes) {
        OV_LOG_ERROR("worldgen stack could not be loaded for {}", options.dimension);
        return 1;
    }
    worldgen::ChunkGenerator generator{*router, *biomes, blocks};
    const NoiseSampler       sampler{generator, &blocks};

    struct Row {
        i32                        starts{0};
        i32                        exact{0};
        i32                        refused{0};
        std::map<std::string, i32> reasons;
        std::vector<std::string>   first_mismatch;
    };

    std::map<std::string, Row> rows;
    for (const StoredStart& start : read_starts(options.world, options.only)) {
        Row& row = rows[start.name];
        ++row.starts;
        const auto* definition = placer->find(start.name);
        if (definition == nullptr) {
            ++row.reasons["no definition"];
            continue;
        }
        auto ours =
            builder.generate(*definition, options.seed, start.chunk_x, start.chunk_z, &sampler);
        if (!ours) {
            ++row.refused;
            ++row.reasons[ours.error()];
            continue;
        }
        std::vector<worldgen::StructurePiece> theirs;
        for (const nbt::Tag& child : *start.children.list()) {
            if (auto piece = builder.piece_from_nbt(child)) {
                theirs.push_back(std::move(*piece));
            }
        }
        std::string why;
        if (!ours->incomplete.empty()) {
            // Compare what we do make, and count the start as incomplete.
            ++row.reasons["incomplete: " + ours->incomplete];
            theirs.resize(std::min(theirs.size(), ours->pieces.size()));
        }
        if (theirs.size() != ours->pieces.size()) {
            why = fmt::format("piece count {} vs {}", ours->pieces.size(), theirs.size());
        } else {
            for (usize index = 0; index < theirs.size() && why.empty(); ++index) {
                const auto& a = ours->pieces[index];
                const auto& b = theirs[index];
                if (a.template_name != b.template_name) {
                    why = "template " + a.template_name + " vs " + b.template_name;
                } else if (a.rotation != b.rotation) {
                    why = fmt::format("rotation {} vs {}", worldgen::to_string(a.rotation),
                                      worldgen::to_string(b.rotation));
                } else if (a.mirror != b.mirror) {
                    why = "mirror";
                } else if (a.origin.x != b.origin.x || a.origin.z != b.origin.z) {
                    why = fmt::format("origin {},{} vs {},{}", a.origin.x, a.origin.z, b.origin.x,
                                      b.origin.z);
                } else if (a.kind == worldgen::PieceKind::RuinedPortal &&
                           a.origin.y != b.origin.y) {
                    // ── portals ── Settled at generation: the stored y is the
                    // game's even in a start whose chunks were never finished.
                    why = fmt::format("height {} vs {}", a.origin.y, b.origin.y);
                } else if (a.kind == worldgen::PieceKind::RuinedPortal &&
                           a.portal.cold != b.portal.cold) {
                    why = fmt::format("cold {} vs {}", a.portal.cold, b.portal.cold);
                } else if (a.kind == worldgen::PieceKind::Scattered &&
                           (a.scattered.kind != b.scattered.kind ||
                            a.scattered.orientation != b.scattered.orientation ||
                            a.scattered.width != b.scattered.width ||
                            a.scattered.height != b.scattered.height ||
                            a.scattered.depth != b.scattered.depth ||
                            a.box.max_x != b.box.max_x || a.box.max_z != b.box.max_z)) {
                    // ── temples ── The stored y is the placeholder until the
                    // piece is placed; the facing and the footprint are the start's.
                    why = fmt::format("scattered: facing {} vs {}, box x {} vs {}, z {} vs {}",
                                      a.scattered.orientation, b.scattered.orientation, a.box.max_x,
                                      b.box.max_x, a.box.max_z, b.box.max_z);
                } else if (std::abs(a.integrity - b.integrity) > 1e-6F) {
                    why = "integrity";
                } else if (a.kind == worldgen::PieceKind::RuinedPortal &&
                           (a.portal.placement != b.portal.placement ||
                            a.portal.air_pocket != b.portal.air_pocket ||
                            std::abs(a.portal.mossiness - b.portal.mossiness) > 1e-6F ||
                            a.portal.vines != b.portal.vines ||
                            a.portal.overgrown != b.portal.overgrown)) {
                    why = fmt::format("portal properties: placement {} vs {}, air pocket {} vs {}",
                                      a.portal.placement, b.portal.placement, a.portal.air_pocket,
                                      b.portal.air_pocket);
                }
            }
        }
        if (why.empty()) {
            ++row.exact;
        } else {
            ++row.reasons["mismatch"];
            if (row.first_mismatch.size() < static_cast<usize>(options.show)) {
                row.first_mismatch.push_back(
                    fmt::format("chunk {} {}: {}", start.chunk_x, start.chunk_z, why));
            }
        }
    }
    fmt::print(
        "level B — our pieces from the seed against the game's stored pieces\nworld {}  seed "
        "{}\n\n",
        options.world.string(), options.seed);
    fmt::print("{:<32} {:>6} {:>6} {:>8}\n", "structure", "starts", "exact", "refused");
    for (const auto& [name, row] : rows) {
        fmt::print("{:<32} {:>6} {:>6} {:>8}\n", name, row.starts, row.exact, row.refused);
    }
    for (const auto& [name, row] : rows) {
        if (row.reasons.empty()) {
            continue;
        }
        fmt::print("\n{}\n", name);
        for (const auto& [reason, count] : row.reasons) {
            fmt::print("  {:>4} × {}\n", count, reason);
        }
        for (const std::string& line : row.first_mismatch) {
            fmt::print("         {}\n", line);
        }
    }
    return 0;
}

/// Level C — our world. The whole pipeline, noise to features with the
/// structure stage attached, generates the chunks a start of the game's
/// covers; the blocks the game's structure is made of (the positions its own
/// pieces write) are then read in our world and compared with the game's.
///
/// This is the only level whose number includes our terrain: a piece settles on
/// the ground it finds, and our ground is not everywhere the game's.
[[nodiscard]] i32 run_level_c(const Options& options, const registry::BlockRegistry& blocks,
                              const registry::Registries& registries,
                              const worldgen::BlockTags& /*tags*/,
                              const worldgen::StructureBuilder& builder) {
    auto sets = worldgen::StructureSetRegistry::load(options.data);
    if (!sets) {
        OV_LOG_ERROR("structure sets: {}", worldgen::to_string(sets.error()));
        return 1;
    }
    auto placer = worldgen::StructurePlacer::load(options.data, *sets);
    if (!placer) {
        OV_LOG_ERROR("structures: {}", worldgen::to_string(placer.error()));
        return 1;
    }
    auto router   = worldgen::NoiseRouter::load(options.data, "overworld", options.seed);
    auto biomes   = worldgen::BiomeSource::load(options.reports, "overworld");
    auto surface  = worldgen::SurfaceSystem::load(options.data, "overworld", options.seed, blocks);
    auto features = worldgen::FeatureRegistry::load(options.data, blocks);
    if (!router || !biomes || !surface || !features) {
        OV_LOG_ERROR("worldgen stack could not be loaded");
        return 1;
    }
    auto decorator = worldgen::Decorator::load(options.data, blocks, *features, *biomes);
    if (!decorator) {
        OV_LOG_ERROR("decorator: {}", worldgen::to_string(decorator.error()));
        return 1;
    }
    placer->restrict_to_biomes(biomes->biomes());

    worldgen::ChunkGenerator       generator{*router, *biomes, blocks};
    const worldgen::CarvingContext carving{router->min_y(), router->height()};
    const worldgen::CarverStage    carvers{options.seed, carving};
    generator.set_surface_system(&*surface);
    if (!generator.set_carvers(&carvers, registries)) {
        OV_LOG_ERROR("carvers could not be attached");
        return 1;
    }
    const NoiseSampler sampler{generator};

    ReferenceWorld world{options.world / "region", blocks, &registries};
    const auto     starts = read_starts(options.world, options.only);

    struct Row {
        i32                        starts{0};
        i32                        found{0};
        i32                        pieces{0};
        i32                        heights_exact{0};
        i64                        compared{0};
        i64                        same{0};
        std::map<std::string, i32> mismatches;
        std::vector<std::string>   heights;
    };

    std::map<std::string, Row> rows;

    for (const StoredStart& start : starts) {
        if (!start.full) {
            continue;
        }
        std::vector<worldgen::StructurePiece> theirs;
        for (const nbt::Tag& child : *start.children.list()) {
            if (auto piece = builder.piece_from_nbt(child)) {
                theirs.push_back(std::move(*piece));
            }
        }
        if (theirs.empty()) {
            continue;
        }
        Row& row = rows[start.name];
        ++row.starts;

        // Where the game's structure is: the positions its own pieces write,
        // placed by our code on the finished reference world (level A).
        OverlayLevel          reference{world, blocks};
        worldgen::BoundingBox box = theirs.front().box;
        for (const auto& piece : theirs) {
            box.encapsulate(piece.box);
            worldgen::FeatureRandom unused{worldgen::FeatureRandom::Kind::Xoroshiro, 0};
            (void)builder.place(reference, piece, piece.box, unused);
        }

        // Our world, generated around it.
        worldgen::ChunkPipeline  pipeline{generator, options.features ? &*decorator : nullptr,
                                          blocks, world::WorldShape::overworld(), options.seed};
        worldgen::StructureStage stage{*placer, builder,     &sampler,
                                       blocks,  &registries, options.seed};
        pipeline.set_structure_stage(&stage);
        for (i32 chunk_x = box.min_x >> 4; chunk_x <= box.max_x >> 4; ++chunk_x) {
            for (i32 chunk_z = box.min_z >> 4; chunk_z <= box.max_z >> 4; ++chunk_z) {
                (void)pipeline.promote(chunk_x, chunk_z, worldgen::ChunkStatus::Full);
            }
        }

        // Did we build the same start, and where did its pieces settle?
        for (const worldgen::StructureStart& ours : stage.starts_at(start.chunk_x, start.chunk_z)) {
            if (ours.structure != start.name) {
                continue;
            }
            ++row.found;
            const usize count = std::min(ours.pieces.size(), theirs.size());
            for (usize index = 0; index < count; ++index) {
                ++row.pieces;
                const i32 our_y   = ours.pieces[index].box.min_y;
                const i32 their_y = theirs[index].box.min_y;
                if (our_y == their_y) {
                    ++row.heights_exact;
                } else if (row.heights.size() < static_cast<usize>(options.show)) {
                    row.heights.push_back(fmt::format("chunk {} {} piece {}: ours y{} theirs y{}",
                                                      start.chunk_x, start.chunk_z, index, our_y,
                                                      their_y));
                }
            }
        }

        for (const auto& [key, write] : reference.writes()) {
            const auto theirs_state = reference.reference_block(write.x, write.y, write.z);
            if (!theirs_state) {
                continue;
            }
            const world::Chunk& chunk =
                pipeline.promote(write.x >> 4, write.z >> 4, worldgen::ChunkStatus::Full);
            const auto ours_state = chunk.get_block(static_cast<usize>(write.x & 15), write.y,
                                                    static_cast<usize>(write.z & 15));
            ++row.compared;
            if (ours_state == *theirs_state) {
                ++row.same;
            } else {
                ++row.mismatches[label(blocks, ours_state) + "  vs game  " +
                                 label(blocks, *theirs_state)];
            }
        }
        fmt::print(stderr, "  {} chunk {} {} done\n", start.name, start.chunk_x, start.chunk_z);
    }

    fmt::print("level C — our pipeline (noise → structures{}), the game's structure blocks\n",
               options.features ? " → features" : "");
    fmt::print("world {}  seed {}\n\n", options.world.string(), options.seed);
    fmt::print("{:<32} {:>6} {:>6} {:>12} {:>9} {:>9} {:>9}\n", "structure", "starts", "found",
               "heights", "compared", "same", "%");
    for (const auto& [name, row] : rows) {
        fmt::print("{:<32} {:>6} {:>6} {:>6}/{:<5} {:>9} {:>9} {:>8.3f}%\n", name, row.starts,
                   row.found, row.heights_exact, row.pieces, row.compared, row.same,
                   row.compared > 0
                       ? 100.0 * static_cast<f64>(row.same) / static_cast<f64>(row.compared)
                       : 0.0);
    }
    for (const auto& [name, row] : rows) {
        fmt::print("\n{}\n", name);
        for (const std::string& line : row.heights) {
            fmt::print("  height  {}\n", line);
        }
        std::vector<std::pair<i32, std::string>> sorted;
        for (const auto& [text, count] : row.mismatches) {
            sorted.emplace_back(count, text);
        }
        std::sort(sorted.rbegin(), sorted.rend());
        for (usize index = 0; index < sorted.size() && index < static_cast<usize>(options.show);
             ++index) {
            fmt::print("  {:>5}  {}\n", sorted[index].first, sorted[index].second);
        }
    }
    return 0;
}

/// Level H — the heights the starts' own generation reads, dumped for analysis:
/// for every ruined portal and buried treasure start of the game, the start's
/// large-feature seed, its box from our pieces, the game's settled box, and our
/// noise's heights over the box and base columns at its corners. One JSON
/// object per line; the rule is fitted outside, then written here in C++.
[[nodiscard]] i32 run_level_h(const Options& options, const registry::BlockRegistry& blocks,
                              const worldgen::StructureBuilder& builder) {
    auto sets = worldgen::StructureSetRegistry::load(options.data);
    if (!sets) {
        return 1;
    }
    auto placer = worldgen::StructurePlacer::load(options.data, *sets);
    auto router = worldgen::NoiseRouter::load(options.data, "overworld", options.seed);
    auto biomes = worldgen::BiomeSource::load(options.reports, "overworld");
    if (!placer || !router || !biomes) {
        OV_LOG_ERROR("worldgen stack could not be loaded");
        return 1;
    }
    worldgen::ChunkGenerator generator{*router, *biomes, blocks};
    const NoiseSampler       sampler{generator};
    for (const StoredStart& start : read_starts(options.world, options.only)) {
        const auto* definition = placer->find(start.name);
        if (definition == nullptr) {
            continue;
        }
        auto ours = builder.generate(*definition, options.seed, start.chunk_x, start.chunk_z,
                                     &sampler);
        if (!ours || ours->pieces.empty()) {
            continue;
        }
        const nbt::Tag& child = start.children.list()->front();
        const auto*     bb    = child.find("BB")->get_if<nbt::Tag::IntArray>();
        const auto&     piece = ours->pieces.front();
        const auto&     box   = piece.box;
        std::string     out   = fmt::format(
            R"({{"name":"{}","cx":{},"cz":{},"full":{},"seed":{},"placement":"{}","template":"{}",)"
                  R"("box":[{},{},{},{},{},{}],"game":[{},{},{},{},{},{}],"heights":[)",
            start.name, start.chunk_x, start.chunk_z, start.full ? 1 : 0,
            worldgen::large_feature_seed(options.seed, start.chunk_x, start.chunk_z),
            piece.portal.placement, piece.template_name, box.min_x, box.min_y, box.min_z,
            box.max_x, box.max_y, box.max_z, (*bb)[0], (*bb)[1], (*bb)[2], (*bb)[3], (*bb)[4],
            (*bb)[5]);
        bool first = true;
        for (i32 z = box.min_z; z <= box.max_z; ++z) {
            for (i32 x = box.min_x; x <= box.max_x; ++x) {
                out += fmt::format("{}[{},{},{},{}]", first ? "" : ",", x, z,
                                   sampler.surface_height(x, z), sampler.ocean_floor_height(x, z));
                first = false;
            }
        }
        out += R"(],"columns":{)";
        first = true;
        for (const auto& [x, z] : {std::pair{box.min_x, box.min_z}, std::pair{box.max_x, box.min_z},
                                   std::pair{box.min_x, box.max_z}, std::pair{box.max_x, box.max_z}}) {
            std::string column;
            for (i32 y = -64; y < 320; ++y) {
                column += generator.is_solid(x, y, z) ? '#' : '.';
            }
            out += fmt::format(R"({}"{},{}":"{}")", first ? "" : ",", x, z, column);
            first = false;
        }
        out += R"(},"biomes":{)";
        first = true;
        for (i32 y = -64; y < 320; y += 4) {
            out += fmt::format(R"({}"{}":"{}")", first ? "" : ",", y,
                               generator.biome_name_at(box.min_x, y, box.min_z));
            first = false;
        }
        out += "}}";
        fmt::print("{}\n", out);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    if (!std::filesystem::is_directory(options.world / "region")) {
        OV_LOG_ERROR("{} has no region/", options.world.string());
        return 1;
    }
    auto blocks = registry::BlockRegistry::load(options.pack);
    if (!blocks) {
        OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
        return 1;
    }
    auto registries = registry::Registries::load(options.registries);
    auto tags       = worldgen::BlockTags::load(options.data, *blocks);
    if (!tags) {
        OV_LOG_ERROR("block tags: {}", worldgen::to_string(tags.error()));
        return 1;
    }
    std::string detail;
    auto        builder =
        worldgen::StructureBuilder::load(options.jar, options.data, *blocks, *tags, &detail);
    if (!builder) {
        OV_LOG_ERROR("templates: {} ({})", worldgen::to_string(builder.error()), detail);
        return 1;
    }
    if (options.level == 'b') {
        return run_level_b(options, *blocks, *builder);
    }
    if (options.level == 'h') {
        return run_level_h(options, *blocks, *builder);
    }
    if (options.level == 'c') {
        if (!registries) {
            OV_LOG_ERROR("level C needs the registries pack");
            return 1;
        }
        return run_level_c(options, *blocks, *registries, *tags, *builder);
    }
    return run_level_a(options, *blocks, registries ? &*registries : nullptr, *builder);
}
