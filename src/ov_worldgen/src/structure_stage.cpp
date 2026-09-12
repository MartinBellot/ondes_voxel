#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/structure_stage.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/decoration.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace ov::worldgen {

namespace {

[[nodiscard]] constexpr i64 key_of(i32 chunk_x, i32 chunk_z) noexcept {
    return (static_cast<i64>(chunk_x) << 32) | static_cast<i64>(static_cast<u32>(chunk_z));
}

/// ── jigsaw ── `OV_STRUCT_REACH=<n>` scans n chunks around a decorated chunk
/// instead of `kReach`: a measuring instrument for the cost of the reach
/// (docs/provenance/jigsaw.md), read once. Not a supported setting — below the
/// jigsaw reach it drops the parts of villages that start farther away.
[[nodiscard]] i32 scan_reach() {
    static const i32 reach = [] {
        const char* value = std::getenv("OV_STRUCT_REACH");
        const int   parsed = value != nullptr ? std::atoi(value) : 0;
        return parsed > 0 ? static_cast<i32>(parsed) : StructureStage::kReach;
    }();
    return reach;
}

[[nodiscard]] constexpr usize local_16(i32 value) noexcept {
    return static_cast<usize>(value & 15);
}

/// The kinds the builder makes. Anything else the placer starts is counted as
/// refused, by kind, and not asked.
[[nodiscard]] bool buildable(StructureKind kind) noexcept {
    switch (kind) {
        case StructureKind::Igloo:
        case StructureKind::Shipwreck:
        case StructureKind::OceanRuin:
        case StructureKind::RuinedPortal:
        case StructureKind::BuriedTreasure:
        case StructureKind::NetherFossil:  // ── nether-2 ──
        case StructureKind::Jigsaw: return true;  // ── jigsaw ──
        default: return false;
    }
}

/// The three-by-three as a structure level. Writes outside the middle column
/// are refused: a structure is written chunk by chunk, each chunk its own part.
class StageLevel final : public StructureLevel {
public:
    StageLevel(std::span<world::Chunk* const, 9> chunks, i32 centre_x, i32 centre_z,
               const registry::BlockRegistry& blocks, const registry::Registries* registries,
               const StructureWorldSampler* sampler, i32 sea_level, bool write_anywhere)
        : blocks_(&blocks),
          registries_(registries),
          sampler_(sampler),
          centre_x_(centre_x),
          centre_z_(centre_z),
          sea_level_(sea_level),
          write_anywhere_(write_anywhere) {
        std::copy(chunks.begin(), chunks.end(), chunks_.begin());
        if (registries_ != nullptr) {
            block_entity_registry_ = registries_->find("minecraft:block_entity_type");
        }
    }

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        const world::Chunk* chunk = find(x, z);
        if (chunk == nullptr || outside_build_height(y)) {
            return registry::kAirState;
        }
        return chunk->get_block(local_16(x), y, local_16(z));
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        world::Chunk* chunk = find(x, z);
        if (chunk == nullptr || outside_build_height(y) || (!write_anywhere_ && !centre(x, z))) {
            return false;
        }
        chunk->set_block(local_16(x), y, local_16(z), state);
        ++written_;
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType type, i32 x, i32 z) const override {
        const world::Chunk* chunk = find(x, z);
        const bool          floor =
            type == world::HeightmapType::OceanFloorWG || type == world::HeightmapType::OceanFloor;
        if (chunk == nullptr) {
            // Outside the neighbourhood the generator answers, from the noise.
            if (sampler_ != nullptr) {
                return floor ? sampler_->ocean_floor_height(x, z) : sampler_->surface_height(x, z);
            }
            return min_y();
        }
        // The `_WG` maps are the stored ones at this point of generation: the
        // same fold `pipeline.cpp` makes, and for the same reason.
        const auto stored =
            type == world::HeightmapType::OceanFloorWG     ? world::HeightmapType::OceanFloor
            : type == world::HeightmapType::WorldSurfaceWG ? world::HeightmapType::WorldSurface
                                                           : type;
        return chunk->heightmap(stored).first_free(local_16(x), local_16(z));
    }

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        const world::Chunk* chunk = find(x, z);
        if (chunk == nullptr) {
            return sampler_ != nullptr ? sampler_->biome_at(x, y, z) : std::string_view{};
        }
        const i32 clamped = std::clamp(y, min_y(), max_y());
        return blocks_->biome_name(chunk->get_biome(local_16(x), clamped, local_16(z)));
    }

    [[nodiscard]] i32 min_y() const override {
        return chunks_[4] != nullptr ? chunks_[4]->shape().min_y : -64;
    }

    [[nodiscard]] i32 world_height() const override {
        return chunks_[4] != nullptr ? static_cast<i32>(chunks_[4]->shape().height) : 384;
    }

    [[nodiscard]] i32 sea_level() const override { return sea_level_; }

    void set_block_entity(i32 x, i32 y, i32 z, nbt::Tag data) override {
        world::Chunk* chunk = find(x, z);
        if (chunk == nullptr || outside_build_height(y) || (!write_anywhere_ && !centre(x, z))) {
            return;
        }
        world::BlockEntity entity;
        entity.x = static_cast<u8>(local_16(x));
        entity.y = y;
        entity.z = static_cast<u8>(local_16(z));
        if (const nbt::Tag* id = data.find("id")) {
            entity.type = std::string{id->as_string()};
        }
        if (registries_ != nullptr && block_entity_registry_) {
            entity.type_id =
                registries_->protocol_id(*block_entity_registry_, entity.type).value_or(0);
        }
        entity.data = std::move(data);
        chunk->set_block_entity(std::move(entity));
    }

    [[nodiscard]] const nbt::Tag* block_entity(i32 x, i32 y, i32 z) const override {
        const world::Chunk* chunk = find(x, z);
        if (chunk == nullptr) {
            return nullptr;
        }
        const auto* entity = chunk->block_entity_at(local_16(x), y, local_16(z));
        return entity != nullptr ? &entity->data : nullptr;
    }

    [[nodiscard]] u64 written() const noexcept { return written_; }

private:
    [[nodiscard]] bool centre(i32 x, i32 z) const noexcept {
        return (x >> 4) == centre_x_ && (z >> 4) == centre_z_;
    }

    [[nodiscard]] world::Chunk* find(i32 x, i32 z) const {
        const i32 dx = (x >> 4) - centre_x_ + 1;
        const i32 dz = (z >> 4) - centre_z_ + 1;
        if (dx < 0 || dx > 2 || dz < 0 || dz > 2) {
            return nullptr;
        }
        return chunks_[static_cast<usize>(dz * 3 + dx)];
    }

    const registry::BlockRegistry*      blocks_;
    const registry::Registries*         registries_;
    const StructureWorldSampler*        sampler_;
    std::optional<registry::RegistryId> block_entity_registry_;
    std::array<world::Chunk*, 9>        chunks_{};
    i32                                 centre_x_;
    i32                                 centre_z_;
    i32                                 sea_level_;
    bool                                write_anywhere_;
    u64                                 written_{0};
};

}  // namespace

namespace {

/// ── jigsaw ── `OV_STRUCT_STAGE=scan`: the stage as it was before each set
/// had its own reach — a square of `scan_reach()` chunks, every set asked in
/// every chunk, everything forgotten at `clear`. A measuring instrument, read
/// once: the before and the after of docs/provenance/jigsaw.md § 6 come out of
/// one binary.
[[nodiscard]] bool scan_mode() {
    static const bool scan = [] {
        const char* value = std::getenv("OV_STRUCT_STAGE");
        return value != nullptr && std::string_view{value} == "scan";
    }();
    return scan;
}

/// Division rounding towards negative infinity: the grid cell of a chunk.
[[nodiscard]] constexpr i32 cell_of(i32 value, i32 spacing) noexcept {
    const i32 quotient = value / spacing;
    return (value % spacing != 0 && value < 0) ? quotient - 1 : quotient;
}

/// A start whose pieces were all settled when it was grown — every jigsaw
/// start, heights from the noise alone. Nothing the stage does changes it
/// afterwards, so it is the same whenever and by whichever square it is asked.
/// ── jigsaw ── The stage's sampler, counting what it is asked and answering
/// with the sampler it wraps, unchanged. One per stage, and a stage belongs to
/// one generation stack and its one thread: the mutable counters are never
/// shared (trap 17 is a cache filled from two threads, which this is not).
class CountingSampler final : public StructureWorldSampler {
public:
    explicit CountingSampler(const StructureWorldSampler& inner) : inner_(&inner) {}

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        ++biomes_;
        return inner_->biome_at(x, y, z);
    }
    [[nodiscard]] i32 surface_height(i32 x, i32 z) const override {
        ++heights_;
        return inner_->surface_height(x, z);
    }
    [[nodiscard]] i32 ocean_floor_height(i32 x, i32 z) const override {
        ++heights_;
        return inner_->ocean_floor_height(x, z);
    }
    [[nodiscard]] std::optional<bool> base_solid(i32 x, i32 y, i32 z) const override {
        return inner_->base_solid(x, y, z);
    }

    [[nodiscard]] u64 heights() const noexcept { return heights_; }
    [[nodiscard]] u64 biomes() const noexcept { return biomes_; }

private:
    const StructureWorldSampler* inner_;
    mutable u64                  heights_{0};
    mutable u64                  biomes_{0};
};

[[nodiscard]] bool settled_at_birth(const StructureStart& start) noexcept {
    return std::ranges::all_of(start.pieces, [](const StructurePiece& piece) {
        return piece.kind == PieceKind::Jigsaw;
    });
}

}  // namespace

struct StructureStage::Impl {
    const StructurePlacer*         placer{nullptr};
    const StructureBuilder*        builder{nullptr};
    const StructureWorldSampler*   sampler{nullptr};
    const registry::BlockRegistry* blocks{nullptr};
    const registry::Registries*    registries{nullptr};
    i64                            level_seed{0};
    FeatureRandom::Kind            random_kind{FeatureRandom::Kind::Xoroshiro};

    /// ── jigsaw ── One structure set, and how far its starts reach.
    struct SetInfo {
        const StructureSet* set{nullptr};
        i32                 reach{kTemplateReach};
    };
    std::vector<SetInfo> sets;
    /// Per set, by start chunk: the start there, or nothing. Node-based: a
    /// pointer into one survives the insertion of another.
    std::vector<std::unordered_map<i64, std::optional<StructureStart>>> by_set;
    /// Starts put in by hand, replacing whatever their chunk had.
    std::unordered_map<i64, std::vector<StructureStart>> manual;
    /// `starts_at`'s answers, rebuilt on every call.
    std::unordered_map<i64, std::vector<StructureStart>> answers;
    /// Positions whose shape follows neighbours, by the chunk they are in.
    std::unordered_map<i64, std::vector<BlockPos>> shaped;
    StructureStageStats                            stats;
    /// ── structures ── Kinds refused by the caller, with the reason.
    std::map<StructureKind, std::string> refused_kinds;
    /// ── jigsaw ── `sampler` points here when there is one to wrap.
    std::unique_ptr<CountingSampler> counting;

    [[nodiscard]] bool is_candidate(usize set_index, i32 chunk_x, i32 chunk_z) const {
        const StructureSet& set = *sets[set_index].set;
        return set.spread && set.spread->is_candidate_chunk(level_seed, chunk_x, chunk_z);
    }

    /// The start one set has in one chunk, decided and built on first use.
    StructureStart* start_of(usize set_index, i32 chunk_x, i32 chunk_z) {
        auto&     cache = by_set[set_index];
        const i64 key   = key_of(chunk_x, chunk_z);
        if (const auto it = cache.find(key); it != cache.end()) {
            return it->second ? &*it->second : nullptr;
        }
        auto& slot = cache[key];
        ++stats.decisions;
        const auto result =
            placer->decide_set(*sets[set_index].set, level_seed, chunk_x, chunk_z, sampler);
        if (result.decision != PlacementDecision::PlacedByPlacement) {
            return nullptr;
        }
        const StructureDefinition* definition = placer->find(result.structure);
        if (definition == nullptr) {
            return nullptr;
        }
        // ── structures ── Refusals carry the structure's name: "jigsaw"
        // alone does not say whether a village or a bastion is missing.
        if (!buildable(definition->kind)) {
            ++stats.refused[definition->name + ": " + std::string{to_string(definition->kind)} +
                            " is not built here"];
            return nullptr;
        }
        if (const auto refused = refused_kinds.find(definition->kind);
            refused != refused_kinds.end()) {
            ++stats.refused[definition->name + ": " + refused->second];
            return nullptr;
        }
        auto start = builder->generate(*definition, level_seed, chunk_x, chunk_z, sampler);
        if (!start) {
            ++stats.refused[definition->name + ": " + start.error()];
            return nullptr;
        }
        if (!start->incomplete.empty()) {
            ++stats.incomplete[start->incomplete];
        }
        ++stats.starts_built;
        slot = std::move(*start);
        return &*slot;
    }

    /// Every start with pieces that may cross this chunk: for each set, the
    /// grid candidates within the set's reach plus `extra` — one chunk per
    /// grid cell, not every chunk of the square.
    void reaching(i32 chunk_x, i32 chunk_z, i32 extra, std::vector<StructureStart*>& out) {
        const auto take = [&](usize set_index, i32 x, i32 z) {
            if (manual.contains(key_of(x, z))) {
                return;
            }
            if (StructureStart* start = start_of(set_index, x, z);
                start != nullptr && !start->pieces.empty()) {
                out.push_back(start);
            }
        };
        if (scan_mode()) {
            const i32 reach = scan_reach() + extra;
            for (i32 dz = -reach; dz <= reach; ++dz) {
                for (i32 dx = -reach; dx <= reach; ++dx) {
                    for (usize index = 0; index < sets.size(); ++index) {
                        if (is_candidate(index, chunk_x + dx, chunk_z + dz)) {
                            take(index, chunk_x + dx, chunk_z + dz);
                        }
                    }
                }
            }
        } else {
            for (usize index = 0; index < sets.size(); ++index) {
                const auto& spread = sets[index].set->spread;
                if (!spread) {
                    continue;  // the strongholds' rings: never started here
                }
                const i32 reach   = sets[index].reach + extra;
                const i32 spacing = std::max(spread->spacing, 1);
                for (i32 gz = cell_of(chunk_z - reach, spacing);
                     gz <= cell_of(chunk_z + reach, spacing); ++gz) {
                    for (i32 gx = cell_of(chunk_x - reach, spacing);
                         gx <= cell_of(chunk_x + reach, spacing); ++gx) {
                        const ChunkPos candidate = spread->candidate(level_seed, gx, gz);
                        if (std::abs(candidate.x - chunk_x) <= reach &&
                            std::abs(candidate.z - chunk_z) <= reach) {
                            take(index, candidate.x, candidate.z);
                        }
                    }
                }
            }
        }
        const i32 bound = kReach + extra;
        for (auto& [key, list] : manual) {
            const auto x = static_cast<i32>(key >> 32);
            const auto z = static_cast<i32>(static_cast<u32>(key));
            if (std::abs(x - chunk_x) > bound || std::abs(z - chunk_z) > bound) {
                continue;
            }
            for (StructureStart& start : list) {
                if (!start.pieces.empty()) {
                    out.push_back(&start);
                }
            }
        }
    }
};

StructureStage::StructureStage(const StructurePlacer& placer, const StructureBuilder& builder,
                               const StructureWorldSampler*   sampler,
                               const registry::BlockRegistry& blocks,
                               const registry::Registries* registries, i64 level_seed)
    : impl_(std::make_unique<Impl>()) {
    impl_->placer      = &placer;
    impl_->builder     = &builder;
    impl_->sampler     = sampler;
    impl_->blocks      = &blocks;
    impl_->registries  = registries;
    impl_->level_seed  = level_seed;
    impl_->random_kind = configured_feature_random();
    if (sampler != nullptr) {  // ── jigsaw ── counted, answered unchanged
        impl_->counting = std::make_unique<CountingSampler>(*sampler);
        impl_->sampler  = impl_->counting.get();
    }

    // ── jigsaw ── Each set's own reach: a jigsaw set reaches as far as its
    // largest structure grows, every other set as far as a template does.
    for (const StructureSet& set : placer.sets().sets()) {
        Impl::SetInfo info;
        info.set  = &set;
        i32 reach = 0;
        for (const StructureSetEntry& entry : set.entries) {
            const StructureDefinition* definition = placer.find(entry.structure);
            const JigsawConfig*        config =
                definition != nullptr && definition->kind == StructureKind::Jigsaw &&
                        builder.jigsaw() != nullptr
                    ? builder.jigsaw()->config(definition->name)
                    : nullptr;
            reach = std::max(reach, config != nullptr ? config->reach_chunks : kTemplateReach);
        }
        if (reach > kReach) {
            OV_LOG_ERROR("structures: set {} reaches {} chunks, more than the stage's bound {}; "
                         "its farthest pieces are not placed",
                         set.name, reach, kReach);
            reach = kReach;
        }
        info.reach = reach;
        impl_->sets.push_back(info);
    }
    impl_->by_set.resize(impl_->sets.size());
}

StructureStage::~StructureStage() {
    // ── jigsaw ── The stage's work, once, for the cost measurement.
    if (impl_ && impl_->stats.decisions > 0) {
        const StructureStageStats& work = stats();
        OV_LOG_INFO("structures: stage work — {} decisions, {} starts grown, {} height queries, "
                    "{} biome queries",
                    work.decisions, work.starts_built, work.height_queries, work.biome_queries);
    }
}

const std::vector<StructureStart>& StructureStage::starts_at(i32 chunk_x, i32 chunk_z) {
    const i64 key = key_of(chunk_x, chunk_z);
    if (const auto it = impl_->manual.find(key); it != impl_->manual.end()) {
        return it->second;
    }
    // Rebuilt on every call and in set order, as the placer answers: a
    // template start still settles its height as its chunks are placed.
    auto& answer = impl_->answers[key];
    answer.clear();
    for (usize index = 0; index < impl_->sets.size(); ++index) {
        if (!impl_->is_candidate(index, chunk_x, chunk_z)) {
            continue;
        }
        if (const StructureStart* start = impl_->start_of(index, chunk_x, chunk_z)) {
            answer.push_back(*start);
        }
    }
    return answer;
}

std::vector<StructureStart*> StructureStage::starts_reaching(i32 chunk_x, i32 chunk_z, i32 extra) {
    std::vector<StructureStart*> out;
    impl_->reaching(chunk_x, chunk_z, extra, out);
    return out;
}

void StructureStage::add_start(StructureStart start) {
    auto& list = impl_->manual[key_of(start.chunk_x, start.chunk_z)];
    list.clear();
    list.push_back(std::move(start));
}

void StructureStage::place(std::span<world::Chunk* const, 9> neighbourhood, i32 chunk_x,
                           i32 chunk_z, i32 sea_level) {
    const BoundingBox column = BoundingBox::chunk_column(chunk_x, chunk_z, -2048, 2047);

    // Every start that crosses this chunk, in the game's order: by step, then
    // by the structure's rank in its step, then by start chunk.
    struct Crossing {
        i32             step;
        i32             index;
        StructureStart* start;
    };

    std::vector<Crossing> crossing;
    // ── jigsaw ── Each set within its own reach (Impl::reaching). The order
    // is the one the square scan gave: by step and rank, then by start chunk,
    // z before x — two starts of one structure share its random, in that order.
    std::vector<StructureStart*> reaching;
    impl_->reaching(chunk_x, chunk_z, 0, reaching);
    for (StructureStart* start : reaching) {
        if (!start->box.intersects(column)) {
            continue;
        }
        const StructureDefinition* definition = impl_->placer->find(start->structure);
        const i32 step  = definition != nullptr ? step_ordinal(definition->step) : 4;
        const i32 index = structure_step_index(*impl_->placer, start->structure);
        crossing.push_back({step, index, start});
    }
    std::stable_sort(crossing.begin(), crossing.end(), [](const Crossing& a, const Crossing& b) {
        return std::tie(a.step, a.index, a.start->chunk_z, a.start->chunk_x) <
               std::tie(b.step, b.index, b.start->chunk_z, b.start->chunk_x);
    });

    StageLevel level{neighbourhood,     chunk_x,        chunk_z,   *impl_->blocks,
                     impl_->registries, impl_->sampler, sea_level, false};
    const i64  decoration =
        decoration_seed(impl_->level_seed, chunk_x * 16, chunk_z * 16, impl_->random_kind);

    auto& shaped_here = impl_->shaped[key_of(chunk_x, chunk_z)];
    usize cursor      = 0;
    while (cursor < crossing.size()) {
        // One random per structure per chunk, shared by its starts and pieces.
        const i32     step  = crossing[cursor].step;
        const i32     index = crossing[cursor].index;
        FeatureRandom random{impl_->random_kind, feature_seed(decoration, index, step)};
        for (; cursor < crossing.size() && crossing[cursor].step == step &&
               crossing[cursor].index == index;
             ++cursor) {
            StructureStart& start = *crossing[cursor].start;
            for (StructurePiece& piece : start.pieces) {
                bool drawn = false;
                if (!piece.height_settled) {
                    // Settled where it is first placed, with this chunk's
                    // random, so a beached ship's extra sink is the draw the
                    // placement below would have made.
                    drawn = piece.box.intersects(column);
                    impl_->builder->settle_height(level, piece, drawn ? &random : nullptr);
                    start.box = start.pieces.front().box;
                    for (const StructurePiece& other : start.pieces) {
                        start.box.encapsulate(other.box);
                    }
                }
                if (!piece.box.intersects(column)) {
                    continue;
                }
                const auto result = impl_->builder->place(level, piece, column, random, drawn);
                ++impl_->stats.placements;
                shaped_here.insert(shaped_here.end(), result.shaped.begin(), result.shaped.end());
            }
        }
    }

    // The neighbour-shape update, again, for every recorded position in the
    // neighbourhood: this chunk's blocks may be the neighbours those were
    // waiting for. Writes land in the neighbours too, which is what the
    // pipeline's three-by-three is for.
    StageLevel wide{neighbourhood,     chunk_x,        chunk_z,   *impl_->blocks,
                    impl_->registries, impl_->sampler, sea_level, true};
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            if (neighbourhood[static_cast<usize>((dz + 1) * 3 + (dx + 1))] == nullptr) {
                continue;
            }
            const auto it = impl_->shaped.find(key_of(chunk_x + dx, chunk_z + dz));
            if (it != impl_->shaped.end() && !it->second.empty()) {
                update_shapes(wide, *impl_->blocks, it->second);
            }
        }
    }
    impl_->stats.blocks_written += level.written();
}

void StructureStage::trim(i32 centre_x, i32 centre_z, i32 keep) {
    const auto far = [&](i64 key) {
        const auto x = static_cast<i32>(key >> 32);
        const auto z = static_cast<i32>(static_cast<u32>(key));
        return std::abs(x - centre_x) > keep || std::abs(z - centre_z) > keep;
    };
    for (auto& cache : impl_->by_set) {
        std::erase_if(cache, [&](const auto& entry) { return far(entry.first); });
    }
    std::erase_if(impl_->manual, [&](const auto& entry) { return far(entry.first); });
    std::erase_if(impl_->answers, [&](const auto& entry) { return far(entry.first); });
    std::erase_if(impl_->shaped, [&](const auto& entry) { return far(entry.first); });
}

// ── structures ──
void StructureStage::clear() {
    // ── jigsaw ── What is kept is a pure function of the seed and the chunk:
    // the decisions that started nothing, and the starts settled when they
    // were grown (every jigsaw start). Keeping them across squares changes no
    // block — ov_gendet's serial and parallel worlds stay identical — and
    // spares a village being grown again by every square it crosses. A
    // template start settles its height on the terrain it is first placed on,
    // so it goes, as it always did. The scan instrument forgets everything.
    for (auto& cache : impl_->by_set) {
        if (scan_mode()) {
            cache.clear();
            continue;
        }
        std::erase_if(cache, [](const auto& entry) {
            return entry.second && !settled_at_birth(*entry.second);
        });
    }
    impl_->manual.clear();
    impl_->answers.clear();
    impl_->shaped.clear();
}

void StructureStage::refuse(StructureKind kind, std::string reason) {
    impl_->refused_kinds[kind] = std::move(reason);
}

const StructureStageStats& StructureStage::stats() const noexcept {
    if (impl_->counting) {  // ── jigsaw ── the sampler's counts, brought up to date
        impl_->stats.height_queries = impl_->counting->heights();
        impl_->stats.biome_queries  = impl_->counting->biomes();
    }
    return impl_->stats;
}

}  // namespace ov::worldgen
