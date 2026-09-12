// Nether parity: our Nether against a Nether the real 1.20.1 server generated.
//
// Three questions, each against the artefact the game keeps for it:
//
//   * **biomes**, cell by cell on the 4x4x4 grid, in every chunk the game took
//     at least to `minecraft:biomes` (a chunk below that holds the default
//     array and lies — pitfall 4 of the briefing);
//   * **carving masks**, bit by bit, in chunks the game stopped at
//     `minecraft:carvers`: those still carry `CarvingMasks.AIR`, the exact set
//     of cells the Nether's carver considered;
//   * **blocks**, name by name, in the same `carvers`-status chunks. Such a
//     chunk is exactly noise + surface rules + carvers and nothing more — no
//     feature has run — which is precisely what `ChunkGenerator::generate()`
//     produces. The block comparison goes through `generate()` itself, so the
//     probe exercises the code path the server runs (pitfall 13).
//
// The Nether is seeded from the legacy random source; before
// `PositionalRandomFactory` existed this comparison could only be made as a
// distribution (docs/provenance/amplitude-old-blended-noise.md). See
// docs/provenance/nether.md for the numbers.
#define OV_LOG_CATEGORY "netherparity"

#include "ov/base/log.hpp"
#include "ov/gameplay/nether_portal.hpp"
#include "ov/io/file.hpp"
#include "ov/nbt/binary.hpp"
#include "ov/nbt/region.hpp"
#include "ov/protocol/play.hpp"
#include "ov/registry/block_states.hpp"
#include "ov/registry/registries.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/biome_source.hpp"
#include "ov/worldgen/carver.hpp"
#include "ov/worldgen/carving_mask.hpp"
#include "ov/worldgen/chunk_generator.hpp"
#include "ov/worldgen/decoration.hpp"
#include "ov/worldgen/density.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/pipeline.hpp"
#include "ov/worldgen/structure_pieces.hpp"  // ── nether-2 ── the fossils
#include "ov/worldgen/structure_set.hpp"     // ── nether-2 ──
#include "ov/worldgen/structure_stage.hpp"   // ── nether-2 ──

#include <memory>
#include <optional>
#include "ov/worldgen/surface_system.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace ov;

namespace {

constexpr i32 kMinY   = 0;
constexpr i32 kHeight = 256;

struct Options {
    std::filesystem::path world{"run/reference-nether-1234567890/world/DIM-1"};
    std::filesystem::path data{"data/vanilla/1.20.1/generated/data/minecraft"};
    std::filesystem::path reports{"data/vanilla/1.20.1/generated"};
    std::filesystem::path pack{"data/vanilla/1.20.1/registry.ovpack"};
    i64                   seed{1234567890};
    /// `carvers`-status chunks to run through generate(). Each costs a full
    /// chunk of noise, so this is the knob that sets the run time.
    i32 chunks{200};
    /// Chunks of any finished-enough status to compare biomes on.
    i32 biome_chunks{3000};
    i32 show{6};
    /// `minecraft:full` chunks to run through the pipeline with the Nether's
    /// decorator: the features' parity, and the list of what is not built.
    i32 full_chunks{0};
    /// ── nether-2 ── With `--full`: take one `full` chunk in this many, in
    /// file order, so that a sample spans every patch (and every biome) of
    /// the reference world rather than the first region file's.
    i32 full_stride{1};
    /// ── nether-2 ── Compare every Nether fossil start the game stored with
    /// the one our builder draws from the seed alone (template, rotation,
    /// origin), over our own base column.
    bool                  fossils{false};
    std::filesystem::path jar{"tools/vanilla/server.jar"};
    /// ── nether-2 ── `--full` without the structure stage: the "before" arm.
    bool no_structures{false};
    /// Print this many surface-block disagreements in full: position, biome,
    /// and the column around it on both sides. Only pairs of blocks the
    /// surface rules place, so a feature's neighbour write does not crowd out
    /// the rules' own errors.
    i32 pairs{0};
    /// Restrict `--pairs` to one game block and/or one of ours.
    std::string pair_game;
    std::string pair_ours;
    /// `x,y,z,axis;...`: build a portal at each target on our own finished
    /// Nether terrain and print where it went — the placement algorithm on
    /// its own, against the positions scripts/measure_nether_portal.py read
    /// from the real server.
    std::string portals;
    /// With `--portal`: also run the algorithm on this world's own terrain.
    std::filesystem::path portal_world;
    /// With `--portal` (one target): the Chunk Data packets the real server
    /// sent a player arriving there — the terrain the game built its portal in,
    /// before anything ticked — and the portal it built (`x,y,z,axis`), which
    /// is taken back out: its frame's floor row to netherrack, the rest to air.
    std::filesystem::path portal_packets;
    std::string           portal_unbuild;
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
        } else if (argument.starts_with("--show=")) {
            options.show = std::atoi(value("--show=").c_str());
        } else if (argument.starts_with("--full=")) {
            options.full_chunks = std::atoi(value("--full=").c_str());
        } else if (argument == "--fossils") {  // ── nether-2 ──
            options.fossils = true;
        } else if (argument == "--no-structures") {  // ── nether-2 ──
            options.no_structures = true;
        } else if (argument.starts_with("--jar=")) {  // ── nether-2 ──
            options.jar = value("--jar=");
        } else if (argument.starts_with("--full-stride=")) {  // ── nether-2 ──
            options.full_stride = std::max(1, std::atoi(value("--full-stride=").c_str()));
        } else if (argument.starts_with("--pairs=")) {
            options.pairs = std::atoi(value("--pairs=").c_str());
        } else if (argument.starts_with("--portal=")) {
            options.portals = value("--portal=");
        } else if (argument.starts_with("--portal-world=")) {
            options.portal_world = value("--portal-world=");
        } else if (argument.starts_with("--portal-packets=")) {
            options.portal_packets = value("--portal-packets=");
        } else if (argument.starts_with("--portal-unbuild=")) {
            options.portal_unbuild = value("--portal-unbuild=");
        } else if (argument.starts_with("--pair-game=")) {
            options.pair_game = value("--pair-game=");
        } else if (argument.starts_with("--pair-ours=")) {
            options.pair_ours = value("--pair-ours=");
        }
    }
    return options;
}

/// One chunk of the reference Nether, decoded.
struct ReferenceChunk {
    i32                      chunk_x{0};
    i32                      chunk_z{0};
    std::string              status;
    std::vector<std::string> names;   ///< [(y - kMinY) * 256 + z * 16 + x]
    std::vector<std::string> biomes;  ///< [(qy * 4 + qz) * 4 + qx]
    std::vector<i64>         mask;    ///< CarvingMasks.AIR, empty if absent

    [[nodiscard]] const std::string& block(i32 x, i32 y, i32 z) const {
        return names[static_cast<usize>(y - kMinY) * 256 + static_cast<usize>(z) * 16 +
                     static_cast<usize>(x)];
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
    if (blocks) {
        chunk.names.assign(static_cast<usize>(kHeight) * 256, "minecraft:air");
    }
    chunk.mask.clear();
    if (const nbt::Tag* masks = document.root.find("CarvingMasks")) {
        if (const nbt::Tag* air = masks->find("AIR")) {
            if (const auto* words = air->get_if<nbt::Tag::LongArray>()) {
                chunk.mask.assign(words->begin(), words->end());
            }
        }
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
                    chunk.names[static_cast<usize>(y - kMinY) * 256 +
                                static_cast<usize>(local_z) * 16 + static_cast<usize>(local_x)] =
                        names[which];
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
    if (name == "minecraft:void_air") {
        return "minecraft:air";
    }
    return name;
}

[[nodiscard]] bool is_empty(std::string_view name) {
    return name == "minecraft:air" || name == "minecraft:cave_air" ||
           name == "minecraft:void_air" || name == "minecraft:lava";
}

/// The blocks the Nether's surface rules — and nothing else before the
/// features — put down.
[[nodiscard]] bool is_surface_block(std::string_view name) {
    constexpr std::array<std::string_view, 8> kSurface{
        "minecraft:netherrack",     "minecraft:soul_sand",      "minecraft:soul_soil",
        "minecraft:gravel",         "minecraft:basalt",         "minecraft:blackstone",
        "minecraft:crimson_nylium", "minecraft:warped_nylium"};
    return std::ranges::find(kSurface, name) != kSurface.end();
}

/// Statuses at or past `minecraft:biomes`: the biome array is the game's.
[[nodiscard]] bool biomes_final(std::string_view status) {
    return status != "minecraft:empty" && status != "minecraft:structure_starts" &&
           status != "minecraft:structure_references";
}

struct Count {
    usize compared{0};
    usize agreed{0};
};

[[nodiscard]] f64 percent(usize part, usize whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<f64>(part) / static_cast<f64>(whole);
}

/// Our finished Nether, read through the pipeline; writes kept on the side.
class PipelineOverlay final : public world::LevelWriter {
public:
    PipelineOverlay(worldgen::ChunkPipeline& pipeline, const registry::BlockRegistry& blocks)
        : pipeline_(&pipeline), blocks_(&blocks) {}

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        if (pos.y < 0 || pos.y > 255) {
            return registry::kAirState;
        }
        if (const auto found = writes_.find(key(pos)); found != writes_.end()) {
            return found->second;
        }
        const world::Chunk& chunk =
            pipeline_->promote(pos.x >> 4, pos.z >> 4, worldgen::ChunkStatus::Full);
        return chunk.get_block(static_cast<usize>(pos.x & 15), pos.y,
                               static_cast<usize>(pos.z & 15));
    }
    [[nodiscard]] bool              is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::nether(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {true, false}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *blocks_; }
    void set_block(BlockPos pos, registry::BlockStateId state) override { writes_[key(pos)] = state; }
    void schedule_tick(BlockPos, std::string_view, i64, world::TickQueue,
                       world::TickPriority) override {}
    [[nodiscard]] bool has_scheduled_tick(BlockPos, std::string_view,
                                          world::TickQueue) const override {
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

private:
    [[nodiscard]] static std::tuple<i32, i32, i32> key(BlockPos p) { return {p.x, p.y, p.z}; }
    worldgen::ChunkPipeline*                               pipeline_;
    const registry::BlockRegistry*                         blocks_;
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> writes_;
};

/// The game's finished Nether, read from its region files; writes on the side.
/// What lets the placement algorithm be judged apart from our terrain.
class ReferenceOverlay final : public world::LevelWriter {
public:
    ReferenceOverlay(const std::filesystem::path& regions, const registry::BlockRegistry& blocks)
        : regions_(regions), blocks_(&blocks) {}

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        if (pos.y < 0 || pos.y > 255) {
            return registry::kAirState;
        }
        if (const auto found = writes_.find(key(pos)); found != writes_.end()) {
            return found->second;
        }
        const auto chunk_key = std::pair{pos.x >> 4, pos.z >> 4};
        auto       cached    = chunks_.find(chunk_key);
        if (cached == chunks_.end()) {
            ReferenceChunk decoded;
            const auto     path = regions_ / fmt::format("r.{}.{}.mca", chunk_key.first >> 5,
                                                         chunk_key.second >> 5);
            bool ok = false;
            if (auto region = nbt::RegionFile::open(path)) {
                const auto lx = static_cast<u32>(chunk_key.first & 31);
                const auto lz = static_cast<u32>(chunk_key.second & 31);
                if (region->has_chunk(lx, lz)) {
                    if (auto document = region->read_chunk(lx, lz)) {
                        ok = decode(*document, decoded, true);
                    }
                }
            }
            if (!ok) {
                ++missing_;
                decoded.names.assign(static_cast<usize>(kHeight) * 256, "minecraft:air");
            }
            cached = chunks_.emplace(chunk_key, std::move(decoded)).first;
        }
        const std::string& name = cached->second.block(pos.x & 15, pos.y, pos.z & 15);
        if (name == "minecraft:air" || name == "minecraft:void_air") {
            return registry::kAirState;
        }
        const auto block = blocks_->find_block(name);
        return block ? blocks_->default_state(*block) : registry::kAirState;
    }
    [[nodiscard]] bool              is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::nether(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {true, false}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *blocks_; }
    void set_block(BlockPos pos, registry::BlockStateId state) override { writes_[key(pos)] = state; }
    void schedule_tick(BlockPos, std::string_view, i64, world::TickQueue,
                       world::TickPriority) override {}
    [[nodiscard]] bool has_scheduled_tick(BlockPos, std::string_view,
                                          world::TickQueue) const override {
        return false;
    }
    [[nodiscard]] i64   game_time() const override { return 0; }
    [[nodiscard]] usize missing() const noexcept { return missing_; }

private:
    [[nodiscard]] static std::tuple<i32, i32, i32> key(BlockPos p) { return {p.x, p.y, p.z}; }
    std::filesystem::path                                        regions_;
    const registry::BlockRegistry*                               blocks_;
    mutable std::map<std::pair<i32, i32>, ReferenceChunk>        chunks_;
    mutable usize                                                missing_{0};
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> writes_;
};

/// Chunks decoded from the game's own packets; writes on the side.
class PacketOverlay final : public world::LevelWriter {
public:
    PacketOverlay(const std::map<std::pair<i32, i32>, world::Chunk>& chunks,
                  const registry::BlockRegistry&                      blocks)
        : chunks_(&chunks), blocks_(&blocks) {}

    [[nodiscard]] registry::BlockStateId block_at(BlockPos pos) const override {
        if (pos.y < 0 || pos.y > 255) {
            return registry::kAirState;
        }
        if (const auto found = writes_.find(key(pos)); found != writes_.end()) {
            return found->second;
        }
        const auto chunk = chunks_->find(std::pair{pos.x >> 4, pos.z >> 4});
        if (chunk == chunks_->end()) {
            return registry::kAirState;
        }
        return chunk->second.get_block(static_cast<usize>(pos.x & 15), pos.y,
                                       static_cast<usize>(pos.z & 15));
    }
    [[nodiscard]] bool              is_loaded(BlockPos) const override { return true; }
    [[nodiscard]] world::WorldShape shape() const override { return world::WorldShape::nether(); }
    [[nodiscard]] world::DimensionTraits traits() const override { return {true, false}; }
    [[nodiscard]] const registry::BlockRegistry& blocks() const override { return *blocks_; }
    void set_block(BlockPos pos, registry::BlockStateId state) override { writes_[key(pos)] = state; }
    void schedule_tick(BlockPos, std::string_view, i64, world::TickQueue,
                       world::TickPriority) override {}
    [[nodiscard]] bool has_scheduled_tick(BlockPos, std::string_view,
                                          world::TickQueue) const override {
        return false;
    }
    [[nodiscard]] i64 game_time() const override { return 0; }

private:
    [[nodiscard]] static std::tuple<i32, i32, i32> key(BlockPos p) { return {p.x, p.y, p.z}; }
    const std::map<std::pair<i32, i32>, world::Chunk>*          chunks_;
    const registry::BlockRegistry*                              blocks_;
    std::map<std::tuple<i32, i32, i32>, registry::BlockStateId> writes_;
};

/// Where our placement algorithm puts a new portal, on our own terrain.
int place_portals(const Options& options, const registry::BlockRegistry& blocks,
                  const registry::Registries& registries,
                  const worldgen::ChunkGenerator& generator, const worldgen::BiomeSource& source) {
    auto features = worldgen::FeatureRegistry::load(options.data, blocks);
    if (!features) {
        return 1;
    }
    auto decorator = worldgen::Decorator::load(options.data, blocks, *features, source);
    if (!decorator) {
        return 1;
    }
    const gameplay::PortalRules rules{blocks, registries};
    std::string_view            list = options.portals;
    while (!list.empty()) {
        const usize            semicolon = list.find(';');
        const std::string      item{list.substr(0, semicolon)};
        list = semicolon == std::string_view::npos ? std::string_view{} : list.substr(semicolon + 1);
        i32  x = 0, y = 0, z = 0;
        char axis = 'x';
        if (std::sscanf(item.c_str(), "%d,%d,%d,%c", &x, &y, &z, &axis) != 4) {
            continue;
        }
        const auto portal_axis = axis == 'z' ? gameplay::PortalAxis::Z : gameplay::PortalAxis::X;
        worldgen::ChunkPipeline pipeline{generator, &*decorator, blocks,
                                         world::WorldShape::nether(), options.seed};
        PipelineOverlay         level{pipeline, blocks};
        const auto made = rules.create(level, BlockPos{x, y, z}, portal_axis, 127);
        fmt::print("target ({}, {}, {}) axis {} -> portal min corner ({}, {}, {})", x, y, z,
                   axis, made.min_corner.x, made.min_corner.y, made.min_corner.z);
        if (!options.portal_packets.empty()) {
            // The terrain the game built its portal in, as it sent it.
            const auto bytes = io::read_file(options.portal_packets);
            if (!bytes) {
                OV_LOG_ERROR("cannot read {}", options.portal_packets.string());
                return 1;
            }
            const auto air = world::AirStates::from(blocks);
            std::map<std::pair<i32, i32>, world::Chunk> sent;
            usize                                       at = 0;
            while (at + 4 <= bytes->size()) {
                const usize length = (static_cast<usize>((*bytes)[at]) << 24) |
                                     (static_cast<usize>((*bytes)[at + 1]) << 16) |
                                     (static_cast<usize>((*bytes)[at + 2]) << 8) |
                                     static_cast<usize>((*bytes)[at + 3]);
                at += 4;
                if (at + length > bytes->size()) {
                    break;
                }
                if (auto chunk = net::parse_chunk_data(
                        std::span<const u8>{bytes->data() + at, length},
                        world::WorldShape::nether(), air, &blocks)) {
                    const auto pos = chunk->position();
                    sent.emplace(std::pair{pos.x, pos.z}, std::move(*chunk));
                }
                at += length;
            }
            PacketOverlay packets{sent, blocks};
            // Take the game's portal back out.
            i32  ux = 0, uy = 0, uz = 0;
            char uaxis = 'x';
            if (std::sscanf(options.portal_unbuild.c_str(), "%d,%d,%d,%c", &ux, &uy, &uz, &uaxis) ==
                4) {
                const auto netherrack =
                    blocks.default_state(*blocks.find_block("minecraft:netherrack"));
                for (i32 i = -1; i < 3; ++i) {
                    for (i32 j = -1; j < 4; ++j) {
                        const BlockPos p = uaxis == 'z' ? BlockPos{ux, uy + j, uz + i}
                                                        : BlockPos{ux + i, uy + j, uz};
                        packets.set_block(p, j < 0 ? netherrack : registry::kAirState);
                    }
                }
            }
            const auto on_sent = rules.create(packets, BlockPos{x, y, z}, portal_axis, 127);
            fmt::print("   on the terrain the game sent ({}, {}, {}) [{} chunks]",
                       on_sent.min_corner.x, on_sent.min_corner.y, on_sent.min_corner.z,
                       sent.size());
        }
        if (!options.portal_world.empty()) {
            // The same algorithm on the game's own terrain.
            ReferenceOverlay reference{options.portal_world / "region", blocks};
            const auto on_game = rules.create(reference, BlockPos{x, y, z}, portal_axis, 127);
            fmt::print("   on the game's terrain ({}, {}, {}){}", on_game.min_corner.x,
                       on_game.min_corner.y, on_game.min_corner.z,
                       reference.missing() != 0
                           ? fmt::format(" [{} chunks missing]", reference.missing())
                           : std::string{});
        }
        fmt::print("\n");
    }
    return 0;
}

// ── nether-2 ── The fossils ─────────────────────────────────────────────────

/// Our generator as a structure sees it: the base column (noise alone) and the
/// biome at a block. The two heights are never asked by a fossil.
class FossilSampler final : public worldgen::StructureWorldSampler {
public:
    explicit FossilSampler(const worldgen::ChunkGenerator& generator) : generator_(generator) {}
    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        return generator_.biome_name_at(x, y, z);
    }
    [[nodiscard]] i32 surface_height(i32, i32) const override { return 128; }
    [[nodiscard]] i32 ocean_floor_height(i32, i32) const override { return 128; }
    [[nodiscard]] std::optional<bool> base_solid(i32 x, i32 y, i32 z) const override {
        return generator_.is_solid(x, y, z);
    }

private:
    const worldgen::ChunkGenerator& generator_;
};

/// Every `minecraft:nether_fossil` start the game stored, against ours from
/// the seed: template, rotation, origin. The draws are all in the start, so a
/// wrong order shows on the first one.
int compare_fossils(const Options& options, const registry::BlockRegistry& blocks,
                    const worldgen::ChunkGenerator&           generator,
                    const std::vector<std::filesystem::path>& files) {
    auto tags = worldgen::BlockTags::load(options.data, blocks);
    if (!tags) {
        OV_LOG_ERROR("block tags did not load");
        return 1;
    }
    std::string detail;
    auto builder = worldgen::StructureBuilder::load(options.jar, options.data, blocks, *tags, &detail);
    if (!builder) {
        OV_LOG_ERROR("templates: {}", detail);
        return 1;
    }
    worldgen::StructureDefinition fossil;
    fossil.name = "minecraft:nether_fossil";
    fossil.kind = worldgen::StructureKind::NetherFossil;
    const FossilSampler sampler{generator};

    usize starts = 0, exact = 0, same_xz = 0, same_template = 0, same_rotation = 0;
    usize refused = 0;
    std::map<i32, usize> dy_histogram;
    std::map<std::string, usize> reasons;
    std::vector<std::string> shown;
    for (const auto& file : files) {
        auto region = nbt::RegionFile::open(file);
        if (!region) {
            continue;
        }
        for (u32 index = 0; index < 1024; ++index) {
            const u32 lx = index % 32;
            const u32 lz = index / 32;
            if (!region->has_chunk(lx, lz)) {
                continue;
            }
            auto document = region->read_chunk(lx, lz);
            if (!document) {
                continue;
            }
            const nbt::Tag* structures = document->root.find("structures");
            const nbt::Tag* all        = structures != nullptr ? structures->find("starts") : nullptr;
            const nbt::Tag* start      = all != nullptr ? all->find("minecraft:nether_fossil") : nullptr;
            if (start == nullptr || start->find("id") == nullptr ||
                start->find("id")->as_string() == "INVALID") {
                continue;
            }
            const nbt::Tag* children = start->find("Children");
            const auto*     list     = children != nullptr ? children->list() : nullptr;
            if (list == nullptr || list->empty()) {
                continue;
            }
            const nbt::Tag& piece = list->front();
            const i32 cx = static_cast<i32>(start->find("ChunkX")->as_i64());
            const i32 cz = static_cast<i32>(start->find("ChunkZ")->as_i64());
            const std::string_view tpl = piece.find("Template")->as_string();
            const std::string_view rot = piece.find("Rot")->as_string();
            const BlockPos         origin{static_cast<i32>(piece.find("TPX")->as_i64()),
                                  static_cast<i32>(piece.find("TPY")->as_i64()),
                                  static_cast<i32>(piece.find("TPZ")->as_i64())};
            ++starts;
            const auto ours = builder->generate(fossil, options.seed, cx, cz, &sampler);
            if (!ours || ours->pieces.empty()) {
                ++refused;
                ++reasons[ours ? std::string{"no piece"} : ours.error()];
                continue;
            }
            const worldgen::StructurePiece& mine = ours->pieces.front();
            const std::string_view rotation_names[] = {"NONE", "CLOCKWISE_90", "CLOCKWISE_180",
                                                       "COUNTERCLOCKWISE_90"};
            const std::string_view my_rot = rotation_names[static_cast<usize>(mine.rotation)];
            const bool t_ok  = mine.template_name == tpl;
            const bool r_ok  = my_rot == rot;
            const bool xz_ok = mine.origin.x == origin.x && mine.origin.z == origin.z;
            same_template += t_ok ? 1 : 0;
            same_rotation += r_ok ? 1 : 0;
            same_xz += xz_ok ? 1 : 0;
            if (xz_ok) {
                ++dy_histogram[mine.origin.y - origin.y];
            }
            if (t_ok && r_ok && xz_ok && mine.origin.y == origin.y) {
                ++exact;
            } else if (shown.size() < static_cast<usize>(options.show)) {
                shown.push_back(fmt::format("  chunk ({}, {}): game {} {} ({}, {}, {}); ours {} {} "
                                            "({}, {}, {})",
                                            cx, cz, tpl, rot, origin.x, origin.y, origin.z,
                                            mine.template_name, my_rot, mine.origin.x,
                                            mine.origin.y, mine.origin.z));
            }
        }
    }
    fmt::print("\nnether fossils: {} starts stored by the game\n", starts);
    fmt::print("  template {} / {}, rotation {} / {}, origin x,z {} / {}\n", same_template, starts,
               same_rotation, starts, same_xz, starts);
    fmt::print("  **whole start identical (template, rotation, x, y, z): {} / {}**\n", exact, starts);
    fmt::print("  our y minus the game's, where x and z agree:\n");
    for (const auto& [dy, n] : dy_histogram) {
        fmt::print("    {:+d}: {}\n", dy, n);
    }
    fmt::print("  refused by us: {}\n", refused);
    for (const auto& [why, n] : reasons) {
        fmt::print("    {} × {}\n", n, why);
    }
    for (const auto& line : shown) {
        fmt::print("{}\n", line);
    }
    return 0;
}

/// Finished chunks, through the pipeline and the Nether's decorator.
///
/// What a `full` chunk has that a `carvers` one has not is the features — and
/// the structures, which this server does not place at all (fortresses,
/// bastions, fossils). The report says per game block how much we reproduce,
/// and lists the placed features a Nether biome names that are not built.
int compare_full(const Options& options, const registry::BlockRegistry& blocks,
                 const worldgen::ChunkGenerator& generator, const worldgen::BiomeSource& source,
                 const std::vector<std::filesystem::path>& files) {
    auto features = worldgen::FeatureRegistry::load(options.data, blocks);
    if (!features) {
        OV_LOG_ERROR("features: {}", worldgen::to_string(features.error()));
        return 1;
    }
    auto decorator = worldgen::Decorator::load(options.data, blocks, *features, source);
    if (!decorator) {
        OV_LOG_ERROR("decorator: {}", worldgen::to_string(decorator.error()));
        return 1;
    }
    fmt::print("\nNether decorator: {} biomes, {} placed features named but not built:\n",
               decorator->biome_count(), decorator->missing_count());
    for (const auto& name : decorator->missing()) {
        fmt::print("  {}\n", name);
    }

    worldgen::ChunkPipeline pipeline{generator, &*decorator, blocks, world::WorldShape::nether(),
                                     options.seed};

    // ── nether-2 ── The structures this generator builds in the Nether — the
    // fossils — placed by the pipeline's structure stage, as in the overworld.
    // The placer only asks for the five Nether biomes' sets.
    auto tags = worldgen::BlockTags::load(options.data, blocks);
    auto sets = worldgen::StructureSetRegistry::load(options.data);
    std::optional<worldgen::StructurePlacer>  placer;
    std::optional<worldgen::StructureBuilder> builder;
    std::unique_ptr<worldgen::StructureStage> stage;
    const FossilSampler                       sampler{generator};
    if (tags && sets) {
        if (auto loaded = worldgen::StructurePlacer::load(options.data, *sets)) {
            placer.emplace(std::move(*loaded));
            placer->restrict_to_biomes({"minecraft:nether_wastes", "minecraft:crimson_forest",
                                        "minecraft:warped_forest", "minecraft:soul_sand_valley",
                                        "minecraft:basalt_deltas"});
        }
        std::string detail;
        if (auto loaded = worldgen::StructureBuilder::load(options.jar, options.data, blocks, *tags,
                                                           &detail)) {
            builder.emplace(std::move(*loaded));
        } else {
            OV_LOG_WARN("templates: {} — no structures", detail);
        }
    }
    if (placer && builder && !options.no_structures) {
        // ── great pyramid ── a parity instrument: vanilla structures only.
        stage = std::make_unique<worldgen::StructureStage>(
            *placer, *builder, &sampler, blocks, nullptr, options.seed,
            worldgen::OriginalStructures::vanilla_parity());
        pipeline.set_structures(&*placer, &sampler);
        pipeline.set_structure_stage(stage.get());
    }
    const auto name_of = [&](registry::BlockStateId state) -> std::string_view {
        return state == registry::kAirState ? std::string_view("minecraft:air")
                                            : blocks.block_name(blocks.block_of(state));
    };

    usize                        compared_chunks = 0;
    usize                        full_seen       = 0;  // ── nether-2 ──
    std::map<std::string, usize> ours_total;           // ── nether-2 ── how many we placed
    Count                        cells;
    std::map<std::string, Count> by_game_block;
    std::map<std::pair<std::string, std::string>, usize> confusion;
    ReferenceChunk               reference;
    for (const auto& file : files) {
        if (compared_chunks >= static_cast<usize>(options.full_chunks)) {
            break;
        }
        auto region = nbt::RegionFile::open(file);
        if (!region) {
            continue;
        }
        for (u32 index = 0; index < 1024 && compared_chunks < static_cast<usize>(options.full_chunks);
             ++index) {
            const u32 local_x = index % 32;
            const u32 local_z = index / 32;
            if (!region->has_chunk(local_x, local_z)) {
                continue;
            }
            auto document = region->read_chunk(local_x, local_z);
            if (!document || !decode(*document, reference, true) ||
                reference.status != "minecraft:full") {
                continue;
            }
            if (full_seen++ % static_cast<usize>(options.full_stride) != 0) {  // ── nether-2 ──
                continue;
            }
            const world::Chunk& ours =
                pipeline.promote(reference.chunk_x, reference.chunk_z, worldgen::ChunkStatus::Full);
            ++compared_chunks;
            for (i32 y = kMinY; y < kMinY + kHeight; ++y) {
                for (i32 z = 0; z < 16; ++z) {
                    for (i32 x = 0; x < 16; ++x) {
                        const std::string_view game = canonical(reference.block(x, y, z));
                        const std::string_view mine = name_of(
                            ours.get_block(static_cast<usize>(x), y, static_cast<usize>(z)));
                        ++cells.compared;
                        auto& per = by_game_block[std::string(game)];
                        ++per.compared;
                        ++ours_total[std::string(mine)];  // ── nether-2 ──
                        if (game == mine) {
                            ++cells.agreed;
                            ++per.agreed;
                        } else {
                            ++confusion[{std::string(game), std::string(mine)}];
                        }
                    }
                }
            }
            pipeline.trim(reference.chunk_x, reference.chunk_z, 3);
            if (stage) {  // ── nether-2 ──
                stage->trim(reference.chunk_x, reference.chunk_z, 3 + worldgen::StructureStage::kReach);
            }
        }
    }
    if (stage) {  // ── nether-2 ── what the structure stage did, and refused
        const auto& stats = stage->stats();
        fmt::print("\nstructure stage: {} starts built, {} placements, {} blocks written\n",
                   stats.starts_built, stats.placements, stats.blocks_written);
        for (const auto& [why, n] : stats.refused) {
            fmt::print("  refused {} × {}\n", n, why);
        }
    }

    fmt::print("\nfull chunks through the pipeline: {} chunks, {} / {} blocks agree ({:.3f} %)\n",
               compared_chunks, cells.agreed, cells.compared, percent(cells.agreed, cells.compared));
    for (const auto& [name, count] : by_game_block) {
        // ── nether-2 ── and how many of that block we placed in all
        fmt::print("    {:<36} {:>9} / {:<9} {:.3f} %   ours {}\n", name, count.agreed,
                   count.compared, percent(count.agreed, count.compared), ours_total[name]);
    }
    std::vector<std::pair<usize, std::pair<std::string, std::string>>> ranked;
    for (const auto& [pair, n] : confusion) {
        ranked.emplace_back(n, pair);
    }
    std::ranges::sort(ranked, std::greater{});
    fmt::print("  largest disagreements:\n");
    for (usize i = 0; i < std::min<usize>(ranked.size(), 25); ++i) {
        fmt::print("    game {:<34} ours {:<30} {}\n", ranked[i].second.first,
                   ranked[i].second.second, ranked[i].first);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Options options = parse(argc, argv);
    const auto    regions = options.world / "region";
    // `--portal` builds portals on terrain and needs no reference world.
    if (options.portals.empty() && !std::filesystem::is_directory(regions)) {
        OV_LOG_ERROR("{} has no region/. Generate one with scripts/reference_nether.sh.",
                     options.world.string());
        return 1;
    }

    auto blocks = registry::BlockRegistry::load(options.pack);
    auto packs  = registry::Registries::load(options.pack);
    if (!blocks || !packs) {
        OV_LOG_ERROR("registry {}: run tools/ov_datagen first", options.pack.string());
        return 1;
    }
    auto router = worldgen::NoiseRouter::load(options.data, "nether", options.seed);
    if (!router) {
        OV_LOG_ERROR("router: {}", worldgen::to_string(router.error()));
        return 1;
    }
    auto source = worldgen::BiomeSource::load(options.reports, "nether");
    if (!source) {
        OV_LOG_ERROR("biome source: {}", worldgen::to_string(source.error()));
        return 1;
    }
    auto surface = worldgen::SurfaceSystem::load(options.data, "nether", options.seed, *blocks);
    if (!surface) {
        OV_LOG_ERROR("surface rules: {}", worldgen::to_string(surface.error()));
        return 1;
    }
    const worldgen::CarverStage carvers = worldgen::CarverStage::nether(options.seed);
    worldgen::ChunkGenerator    generator{*router, *source, *blocks};
    generator.set_surface_system(&*surface);
    if (auto attached = generator.set_carvers(&carvers, *packs); !attached) {
        OV_LOG_ERROR("carvers: {}", worldgen::to_string(attached.error()));
        return 1;
    }
    const auto air = world::AirStates::from(*blocks);

    if (!options.portals.empty()) {
        return place_portals(options, *blocks, *packs, generator, *source);
    }

    const auto name_of = [&](registry::BlockStateId state) -> std::string_view {
        return state == registry::kAirState ? std::string_view("minecraft:air")
                                            : blocks->block_name(blocks->block_of(state));
    };

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(regions)) {
        if (entry.path().extension() == ".mca") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);

    // Biomes.
    Count                                    biome_cells;
    usize                                    biome_chunks = 0;
    std::map<std::pair<std::string, std::string>, usize> biome_confusion;
    std::map<std::string, Count>             biome_by_name;
    // Masks.
    usize mask_chunks = 0, mask_exact = 0, mask_both = 0, mask_ours = 0, mask_theirs = 0;
    // Blocks.
    usize                        block_chunks = 0;
    Count                        block_cells;
    Count                        solid_cells;
    std::map<std::string, Count> by_game_block;
    std::map<std::pair<std::string, std::string>, usize> confusion;
    std::array<Count, 16>        by_band{};
    // The lava level of the carvers: what the game left in its own carved
    // cells, by height.
    std::map<i32, std::array<usize, 3>> carved_by_y;  // [lava, cave_air, other]
    std::vector<std::string>            reports;
    i32                                 pairs_shown = 0;
    std::vector<std::string>            pair_lines;

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
            if (status_tag == nullptr) {
                continue;
            }
            const std::string status{status_tag->as_string()};
            const bool        carvers_status = status == "minecraft:carvers";
            const bool        want_blocks =
                carvers_status && block_chunks < static_cast<usize>(options.chunks);
            const bool want_biomes =
                biomes_final(status) && biome_chunks < static_cast<usize>(options.biome_chunks);
            if (!want_blocks && !want_biomes) {
                continue;
            }
            if (!decode(*document, reference, want_blocks)) {
                continue;
            }

            world::Chunk ours{ChunkPos{reference.chunk_x, reference.chunk_z},
                              world::WorldShape::nether(), air, &*blocks};
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
                                                  static_cast<usize>(qz)) *
                                                     4 +
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

            // The mask.
            if (!reference.mask.empty()) {
                const auto theirs = worldgen::CarvingMask::from_long_array(
                    reference.mask, carvers.context().min_y, carvers.context().height);
                const auto mine  = carvers.carve(reference.chunk_x, reference.chunk_z);
                const auto a     = theirs.words();
                const auto b     = mine.words();
                usize      both  = 0;
                usize      only1 = 0;
                usize      only2 = 0;
                for (usize word = 0; word < b.size(); ++word) {
                    const u64 other = word < a.size() ? a[word] : 0ULL;
                    both += static_cast<usize>(std::popcount(b[word] & other));
                    only1 += static_cast<usize>(std::popcount(b[word] & ~other));
                    only2 += static_cast<usize>(std::popcount(other & ~b[word]));
                }
                ++mask_chunks;
                mask_both += both;
                mask_ours += only1;
                mask_theirs += only2;
                if (only1 == 0 && only2 == 0) {
                    ++mask_exact;
                }
                for (i32 y = kMinY; y < kMinY + kHeight; ++y) {
                    for (i32 z = 0; z < 16; ++z) {
                        for (i32 x = 0; x < 16; ++x) {
                            if (!theirs.get(x, y, z)) {
                                continue;
                            }
                            const std::string& game = reference.block(x, y, z);
                            auto&              row  = carved_by_y[y];
                            if (game == "minecraft:lava") {
                                ++row[0];
                            } else if (game == "minecraft:cave_air") {
                                ++row[1];
                            } else {
                                ++row[2];
                            }
                        }
                    }
                }
            }

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
                        auto& band = by_band[static_cast<usize>((y - kMinY) / 16)];
                        ++band.compared;
                        if (game == mine) {
                            ++block_cells.agreed;
                            ++per.agreed;
                            ++band.agreed;
                        } else {
                            ++confusion[{std::string(game), std::string(mine)}];
                            ++chunk_wrong;
                            if (pairs_shown < options.pairs && is_surface_block(game) &&
                                is_surface_block(mine) &&
                                (options.pair_game.empty() || game == options.pair_game) &&
                                (options.pair_ours.empty() || mine == options.pair_ours)) {
                                ++pairs_shown;
                                std::string line = fmt::format(
                                    "  ({}, {}, {}) {}  game {} ours {}  surface depth {}\n    "
                                    "column y-4..y+4:",
                                    reference.chunk_x * 16 + x, y, reference.chunk_z * 16 + z,
                                    reference.biomes[(static_cast<usize>(y >> 2) * 4 +
                                                      static_cast<usize>(z >> 2)) *
                                                         4 +
                                                     static_cast<usize>(x >> 2)],
                                    game, mine,
                                    surface->surface_depth(reference.chunk_x * 16 + x,
                                                           reference.chunk_z * 16 + z));
                                for (i32 dy = 4; dy >= -4; --dy) {
                                    const i32 yy = y + dy;
                                    if (yy < kMinY || yy >= kMinY + kHeight) {
                                        continue;
                                    }
                                    const auto short_name = [](std::string_view n) {
                                        return n.substr(n.find(':') + 1);
                                    };
                                    line += fmt::format(
                                        "\n      {:>3}  {:<16} {:<16}", yy,
                                        short_name(canonical(reference.block(x, yy, z))),
                                        short_name(name_of(ours.get_block(
                                            static_cast<usize>(x), yy, static_cast<usize>(z)))));
                                }
                                pair_lines.push_back(std::move(line));
                            }
                        }
                        ++solid_cells.compared;
                        if (is_empty(game) == is_empty(mine)) {
                            ++solid_cells.agreed;
                        }
                    }
                }
            }
            if (chunk_wrong != 0 && reports.size() < static_cast<usize>(options.show)) {
                reports.push_back(fmt::format("  chunk ({:>5},{:>5})  {} blocks differ",
                                              reference.chunk_x, reference.chunk_z, chunk_wrong));
            }
        }
    }

    fmt::print("seed {}, reference {}\n", options.seed, options.world.string());

    fmt::print("\nbiomes: {} chunks, {} / {} cells agree ({:.3f} %)\n", biome_chunks,
               biome_cells.agreed, biome_cells.compared,
               percent(biome_cells.agreed, biome_cells.compared));
    for (const auto& [name, count] : biome_by_name) {
        fmt::print("  {:<28} {:>9} / {:<9} {:.3f} %\n", name, count.agreed, count.compared,
                   percent(count.agreed, count.compared));
    }
    {
        std::vector<std::pair<usize, std::pair<std::string, std::string>>> ranked;
        for (const auto& [pair, n] : biome_confusion) {
            ranked.emplace_back(n, pair);
        }
        std::ranges::sort(ranked, std::greater{});
        for (usize i = 0; i < std::min<usize>(ranked.size(), 8); ++i) {
            fmt::print("  game {:<26} ours {:<26} {}\n", ranked[i].second.first,
                       ranked[i].second.second, ranked[i].first);
        }
    }

    fmt::print("\ncarving masks: {} chunks, {} bit-exact ({:.3f} %); cells both {}, ours only {}, "
               "game only {}\n",
               mask_chunks, mask_exact, percent(mask_exact, mask_chunks), mask_both, mask_ours,
               mask_theirs);
    fmt::print("what the game left in its carved cells, by height (lava / cave_air / other):\n");
    for (const auto& [y, row] : carved_by_y) {
        if (y > 40 && y % 16 != 0) {
            continue;
        }
        fmt::print("  y {:>3}: {:>6} {:>6} {:>6}\n", y, row[0], row[1], row[2]);
    }

    fmt::print("\nblocks through generate(): {} carvers-status chunks, {} / {} blocks agree "
               "({:.3f} %)\n",
               block_chunks, block_cells.agreed, block_cells.compared,
               percent(block_cells.agreed, block_cells.compared));
    fmt::print("  solid / empty (air, cave air, lava) agreement: {:.3f} %\n",
               percent(solid_cells.agreed, solid_cells.compared));
    fmt::print("  by game block, recall:\n");
    for (const auto& [name, count] : by_game_block) {
        fmt::print("    {:<32} {:>9} / {:<9} {:.3f} %\n", name, count.agreed, count.compared,
                   percent(count.agreed, count.compared));
    }
    fmt::print("  by height:\n");
    for (usize band = 0; band < by_band.size(); ++band) {
        if (by_band[band].compared == 0) {
            continue;
        }
        fmt::print("    y {:>3} .. {:>3}  {:.3f} %\n", kMinY + static_cast<i32>(band) * 16,
                   kMinY + static_cast<i32>(band) * 16 + 15,
                   percent(by_band[band].agreed, by_band[band].compared));
    }
    {
        std::vector<std::pair<usize, std::pair<std::string, std::string>>> ranked;
        for (const auto& [pair, n] : confusion) {
            ranked.emplace_back(n, pair);
        }
        std::ranges::sort(ranked, std::greater{});
        fmt::print("  largest disagreements:\n");
        for (usize i = 0; i < std::min<usize>(ranked.size(), 15); ++i) {
            fmt::print("    game {:<30} ours {:<30} {}\n", ranked[i].second.first,
                       ranked[i].second.second, ranked[i].first);
        }
    }
    for (const auto& line : reports) {
        fmt::print("{}\n", line);
    }
    if (!pair_lines.empty()) {
        fmt::print("\nsurface disagreements, in full (game left, ours right):\n");
        for (const auto& line : pair_lines) {
            fmt::print("{}\n", line);
        }
    }

    if (options.fossils) {  // ── nether-2 ──
        if (const int status = compare_fossils(options, *blocks, generator, files); status != 0) {
            return status;
        }
    }
    if (options.full_chunks > 0) {
        return compare_full(options, *blocks, generator, *source, files);
    }
    return 0;
}
