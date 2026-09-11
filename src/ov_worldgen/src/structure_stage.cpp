#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/structure_stage.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/decoration.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace ov::worldgen {

namespace {

[[nodiscard]] constexpr i64 key_of(i32 chunk_x, i32 chunk_z) noexcept {
    return (static_cast<i64>(chunk_x) << 32) | static_cast<i64>(static_cast<u32>(chunk_z));
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
        case StructureKind::NetherFossil: return true;  // ── nether-2 ──
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

struct StructureStage::Impl {
    const StructurePlacer*         placer{nullptr};
    const StructureBuilder*        builder{nullptr};
    const StructureWorldSampler*   sampler{nullptr};
    const registry::BlockRegistry* blocks{nullptr};
    const registry::Registries*    registries{nullptr};
    i64                            level_seed{0};
    FeatureRandom::Kind            random_kind{FeatureRandom::Kind::Xoroshiro};

    /// Starts by start chunk. Node-based: a reference into one survives the
    /// insertion of another.
    std::unordered_map<i64, std::vector<StructureStart>> starts;
    /// Positions whose shape follows neighbours, by the chunk they are in.
    std::unordered_map<i64, std::vector<BlockPos>> shaped;
    StructureStageStats                            stats;
    /// ── structures ── Kinds refused by the caller, with the reason.
    std::map<StructureKind, std::string> refused_kinds;

    std::vector<StructureStart>& starts_at(i32 chunk_x, i32 chunk_z) {
        const i64 key = key_of(chunk_x, chunk_z);
        if (const auto it = starts.find(key); it != starts.end()) {
            return it->second;
        }
        auto& out = starts[key];
        for (const StructurePlacementResult& result :
             placer->decide(level_seed, chunk_x, chunk_z, sampler)) {
            if (result.decision != PlacementDecision::PlacedByPlacement) {
                continue;
            }
            const StructureDefinition* definition = placer->find(result.structure);
            if (definition == nullptr) {
                continue;
            }
            // ── structures ── Refusals carry the structure's name: "jigsaw"
            // alone does not say whether a village or a bastion is missing.
            if (!buildable(definition->kind)) {
                ++stats.refused[definition->name + ": " + std::string{to_string(definition->kind)} +
                                " is not built here"];
                continue;
            }
            if (const auto refused = refused_kinds.find(definition->kind);
                refused != refused_kinds.end()) {
                ++stats.refused[definition->name + ": " + refused->second];
                continue;
            }
            auto start = builder->generate(*definition, level_seed, chunk_x, chunk_z, sampler);
            if (!start) {
                ++stats.refused[definition->name + ": " + start.error()];
                continue;
            }
            if (!start->incomplete.empty()) {
                ++stats.incomplete[start->incomplete];
            }
            ++stats.starts_built;
            out.push_back(std::move(*start));
        }
        return out;
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
}

StructureStage::~StructureStage() = default;

const std::vector<StructureStart>& StructureStage::starts_at(i32 chunk_x, i32 chunk_z) {
    return impl_->starts_at(chunk_x, chunk_z);
}

void StructureStage::add_start(StructureStart start) {
    auto& list = impl_->starts[key_of(start.chunk_x, start.chunk_z)];
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
    for (i32 dz = -kReach; dz <= kReach; ++dz) {
        for (i32 dx = -kReach; dx <= kReach; ++dx) {
            for (StructureStart& start : impl_->starts_at(chunk_x + dx, chunk_z + dz)) {
                if (!start.pieces.empty() && start.box.intersects(column)) {
                    const StructureDefinition* definition = impl_->placer->find(start.structure);
                    const i32 step  = definition != nullptr ? step_ordinal(definition->step) : 4;
                    const i32 index = structure_step_index(*impl_->placer, start.structure);
                    crossing.push_back({step, index, &start});
                }
            }
        }
    }
    std::stable_sort(crossing.begin(), crossing.end(), [](const Crossing& a, const Crossing& b) {
        return std::tie(a.step, a.index) < std::tie(b.step, b.index);
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
    std::erase_if(impl_->starts, [&](const auto& entry) { return far(entry.first); });
    std::erase_if(impl_->shaped, [&](const auto& entry) { return far(entry.first); });
}

// ── structures ──
void StructureStage::clear() {
    impl_->starts.clear();
    impl_->shaped.clear();
}

void StructureStage::refuse(StructureKind kind, std::string reason) {
    impl_->refused_kinds[kind] = std::move(reason);
}

const StructureStageStats& StructureStage::stats() const noexcept {
    return impl_->stats;
}

}  // namespace ov::worldgen
