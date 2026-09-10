#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/pipeline.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace ov::worldgen {

namespace {

[[nodiscard]] constexpr i64 key_of(i32 chunk_x, i32 chunk_z) noexcept {
    return (static_cast<i64>(chunk_x) << 32) | static_cast<i64>(static_cast<u32>(chunk_z));
}

[[nodiscard]] constexpr i32 floor_div_16(i32 value) noexcept { return value >> 4; }

[[nodiscard]] constexpr usize local_16(i32 value) noexcept {
    return static_cast<usize>(value & 15);
}

/// The heightmap a feature's question really means.
///
/// `Chunk` keeps four maps and folds both `_WG` names onto slot zero, which is
/// WORLD_SURFACE — right for `WORLD_SURFACE_WG` and wrong for `OCEAN_FLOOR_WG`,
/// where it would answer with the top of the water instead of the sea bed. The
/// fold is harmless where it lives, because nothing stores a `_WG` map; it is
/// not harmless here, because `height_range`'s `heightmap` anchor asks for
/// `OCEAN_FLOOR_WG` by name. Resolved explicitly rather than inherited.
[[nodiscard]] constexpr world::HeightmapType stored_kind(world::HeightmapType type) noexcept {
    switch (type) {
        case world::HeightmapType::WorldSurfaceWG:
            return world::HeightmapType::WorldSurface;
        case world::HeightmapType::OceanFloorWG:
            return world::HeightmapType::OceanFloor;
        default:
            return type;
    }
}

}  // namespace

std::string_view to_string(ChunkStatus status) noexcept {
    switch (status) {
        case ChunkStatus::Empty: return "empty";
        case ChunkStatus::StructureStarts: return "structure_starts";
        case ChunkStatus::Biomes: return "biomes";
        case ChunkStatus::Noise: return "noise";
        case ChunkStatus::Surface: return "surface";
        case ChunkStatus::Carvers: return "carvers";
        case ChunkStatus::Features: return "features";
        case ChunkStatus::Full: return "full";
    }
    return "unknown";
}

/// The three-by-three of real chunks, presented as the level a feature writes
/// into.
///
/// This class is the whole reason the pipeline exists. Every method that could
/// silently swallow a write instead counts it: `border` for a write that left
/// the middle chunk and landed in a neighbour that is being kept, `dropped` for
/// one that left the neighbourhood altogether. The second is not an error — the
/// game drops those too, and generating that chunk's own neighbourhood is what
/// puts them back — but it is a number, not a shrug.
class PipelineLevel final : public FeatureLevel {
public:
    PipelineLevel(const registry::BlockRegistry& blocks, world::WorldShape shape, i32 sea_level,
                  i32 centre_x, i32 centre_z)
        : blocks_(&blocks),
          shape_(shape),
          sea_level_(sea_level),
          centre_x_(centre_x),
          centre_z_(centre_z) {}

    void install(i32 chunk_x, i32 chunk_z, world::Chunk* chunk) {
        const i32 dx = chunk_x - centre_x_ + 1;
        const i32 dz = chunk_z - centre_z_ + 1;
        if (dx < 0 || dx > 2 || dz < 0 || dz > 2) {
            return;
        }
        chunks_[static_cast<usize>(dz * 3 + dx)] = chunk;
    }

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        const world::Chunk* chunk = find(x, z);
        if (chunk == nullptr || outside_build_height(y)) {
            return registry::kAirState;
        }
        return chunk->get_block(local_16(x), y, local_16(z));
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        ++writes_;
        world::Chunk* chunk = find_mutable(x, z);
        if (chunk == nullptr || outside_build_height(y)) {
            ++dropped_;
            return false;
        }
        if (floor_div_16(x) != centre_x_ || floor_div_16(z) != centre_z_) {
            ++border_;
        }
        chunk->set_block(local_16(x), y, local_16(z), state);
        return true;
    }

    [[nodiscard]] i32 height(world::HeightmapType type, i32 x, i32 z) const override {
        const world::Chunk* chunk = find(x, z);
        if (chunk == nullptr) {
            return shape_.min_y;
        }
        return chunk->heightmap(stored_kind(type)).first_free(local_16(x), local_16(z));
    }

    [[nodiscard]] std::string_view biome_at(i32 x, i32 y, i32 z) const override {
        const world::Chunk* chunk = find(x, z);
        if (chunk == nullptr) {
            return "minecraft:plains";
        }
        const i32 clamped = std::clamp(y, shape_.min_y, shape_.max_y());
        return blocks_->biome_name(chunk->get_biome(local_16(x), clamped, local_16(z)));
    }

    [[nodiscard]] i32 min_y() const override { return shape_.min_y; }
    [[nodiscard]] i32 world_height() const override { return static_cast<i32>(shape_.height); }
    [[nodiscard]] i32 sea_level() const override { return sea_level_; }

    [[nodiscard]] u64 writes() const noexcept { return writes_; }
    [[nodiscard]] u64 border() const noexcept { return border_; }
    [[nodiscard]] u64 dropped() const noexcept { return dropped_; }

private:
    [[nodiscard]] const world::Chunk* find(i32 x, i32 z) const {
        const i32 dx = floor_div_16(x) - centre_x_ + 1;
        const i32 dz = floor_div_16(z) - centre_z_ + 1;
        if (dx < 0 || dx > 2 || dz < 0 || dz > 2) {
            return nullptr;
        }
        return chunks_[static_cast<usize>(dz * 3 + dx)];
    }

    [[nodiscard]] world::Chunk* find_mutable(i32 x, i32 z) {
        return const_cast<world::Chunk*>(find(x, z));
    }

    const registry::BlockRegistry*   blocks_;
    world::WorldShape                shape_;
    i32                              sea_level_;
    i32                              centre_x_;
    i32                              centre_z_;
    std::array<world::Chunk*, 9>     chunks_{};
    u64                              writes_{0};
    u64                              border_{0};
    u64                              dropped_{0};
};

struct ChunkPipeline::Impl {
    struct Entry {
        world::Chunk chunk;
        ChunkStatus  status{ChunkStatus::Empty};
        /// The structures that start here, decided at `StructureStarts`.
        ///
        /// Owned by the chunk's entry rather than by a side table, because a
        /// chunk that is taken out of the cache takes its answer with it and a
        /// side table would leak one row per chunk generated for the life of
        /// the world.
        std::vector<std::string_view> starts;
    };

    const ChunkGenerator*          generator;
    const Decorator*               decorator;
    const registry::BlockRegistry* blocks;
    world::WorldShape              shape;
    world::AirStates               air;
    i64                            level_seed;
    i32                            sea_level;

    /// Both borrowed, both optional, and independently so. See
    /// `ChunkPipeline::set_structures`.
    const StructurePlacer*       placer{nullptr};
    const StructureWorldSampler* sampler{nullptr};

    /// Node-based on purpose: `promote()` holds a reference to one entry while
    /// driving its neighbours, and `unordered_map` keeps references valid
    /// across a rehash where a vector would not.
    std::unordered_map<i64, Entry> cache;
    PipelineStats                  stats;

    Entry& entry_for(i32 chunk_x, i32 chunk_z) {
        const i64 key = key_of(chunk_x, chunk_z);
        if (const auto it = cache.find(key); it != cache.end()) {
            return it->second;
        }
        auto& entry = cache
                          .emplace(key, Entry{world::Chunk{ChunkPos{chunk_x, chunk_z}, shape, air,
                                                           blocks},
                                              ChunkStatus::Empty,
                                              {}})
                          .first->second;
        stats.reached[static_cast<usize>(ChunkStatus::Empty)] += 1;
        stats.resident   = cache.size();
        stats.peak_resident = std::max(stats.peak_resident, stats.resident);
        return entry;
    }

    void note(ChunkStatus status) { stats.reached[static_cast<usize>(status)] += 1; }

    /// Drive one chunk to a status. Recursive through the neighbours at
    /// `Features` and `Full`, and the recursion terminates because those are
    /// the only two steps with a radius: a neighbour is only ever asked for a
    /// *lower* status than the one being reached.
    Entry& advance(i32 chunk_x, i32 chunk_z, ChunkStatus target) {  // NOLINT(misc-no-recursion)
        Entry& entry = entry_for(chunk_x, chunk_z);
        while (entry.status < target) {
            const auto next = static_cast<ChunkStatus>(static_cast<u8>(entry.status) + 1);
            step(chunk_x, chunk_z, entry, next);
            entry.status = next;
            note(next);
        }
        return entry;
    }

    void step(i32 chunk_x, i32 chunk_z, Entry& entry,  // NOLINT(misc-no-recursion)
              ChunkStatus next) {
        switch (next) {
            case ChunkStatus::Empty:
                break;
            case ChunkStatus::StructureStarts:
                // Before the noise, and reading the generator rather than the
                // chunk — the chunk is still empty here and must stay that way.
                // A structure decision that needed generated blocks could not
                // answer for a chunk the world has not made, and the game
                // answers for those constantly (`/locate`, a village's own
                // jigsaw reaching four chunks away).
                if (placer != nullptr) {
                    for (const StructurePlacementResult& result :
                         placer->decide(level_seed, chunk_x, chunk_z, sampler)) {
                        if (result.decision == PlacementDecision::PlacedByPlacement) {
                            entry.starts.push_back(result.structure);
                        }
                    }
                    stats.structure_starts += entry.starts.size();
                }
                break;
            case ChunkStatus::Biomes:
                generator->generate_biomes(entry.chunk);
                break;
            case ChunkStatus::Noise:
                generator->generate_noise(entry.chunk);
                break;
            case ChunkStatus::Surface:
                generator->generate_surface(entry.chunk);
                break;
            case ChunkStatus::Carvers:
                generator->generate_carvers(entry.chunk);
                // Rebuilt here rather than at the end of everything: a carved
                // cell can be the block a heightmap was pointing at, and the
                // decoration that comes next asks this chunk how high its
                // columns are on nearly every placement.
                entry.chunk.recompute_heightmaps();
                break;
            case ChunkStatus::Features:
                decorate(chunk_x, chunk_z, entry);
                break;
            case ChunkStatus::Full:
                // Every neighbour runs its own features, which is the moment
                // the last write that can land in this chunk lands. Until then
                // the chunk is finished-looking and not finished.
                for (i32 dz = -1; dz <= 1; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dz == 0) {
                            continue;
                        }
                        (void)advance(chunk_x + dx, chunk_z + dz, ChunkStatus::Features);
                    }
                }
                // After all nine decorations, not only this chunk's own. The
                // maps are maintained incrementally by every `set_block`, so
                // this is a check as much as a computation — and it is cheap
                // next to the stage that precedes it.
                entry.chunk.recompute_heightmaps();
                break;
        }
    }

    void decorate(i32 chunk_x, i32 chunk_z, Entry& entry) {  // NOLINT(misc-no-recursion)
        if (decorator == nullptr) {
            return;
        }

        // The rule, enforced rather than assumed: the eight neighbours must
        // have finished their carvers before this chunk may place a feature.
        // A vein reaching into a neighbour that has not been carved yet would
        // be cut away afterwards; one reaching into a neighbour that does not
        // exist yet would be dropped.
        PipelineLevel level{*blocks, shape, sea_level, chunk_x, chunk_z};
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                Entry& neighbour = advance(chunk_x + dx, chunk_z + dz, ChunkStatus::Carvers);
                level.install(chunk_x + dx, chunk_z + dz, &neighbour.chunk);
            }
        }
        level.install(chunk_x, chunk_z, &entry.chunk);

        (void)decorator->decorate(level, chunk_x, chunk_z, level_seed);

        stats.decorations += 1;
        stats.feature_writes += level.writes();
        stats.border_writes += level.border();
        stats.dropped_writes += level.dropped();

        // The centre's own maps, rebuilt straight after its decoration: the
        // eight neighbours read them when their turn comes.
        entry.chunk.recompute_heightmaps();
    }
};

ChunkPipeline::ChunkPipeline(const ChunkGenerator& generator, const Decorator* decorator,
                             const registry::BlockRegistry& blocks, world::WorldShape shape,
                             i64 level_seed)
    : impl_(std::make_unique<Impl>()) {
    impl_->generator  = &generator;
    impl_->decorator  = decorator;
    impl_->blocks     = &blocks;
    impl_->shape      = shape;
    impl_->air        = world::AirStates::from(blocks);
    impl_->level_seed = level_seed;
    impl_->sea_level  = generator.sea_level();
    if (decorator == nullptr) {
        OV_LOG_INFO(
            "pipeline: no decorator attached; chunks stop at the carvers and carry no features");
    }
}

ChunkPipeline::~ChunkPipeline() = default;

const world::Chunk& ChunkPipeline::promote(i32 chunk_x, i32 chunk_z, ChunkStatus status) {
    return impl_->advance(chunk_x, chunk_z, status).chunk;
}

world::Chunk ChunkPipeline::take(i32 chunk_x, i32 chunk_z) {
    (void)impl_->advance(chunk_x, chunk_z, ChunkStatus::Full);
    const auto  it    = impl_->cache.find(key_of(chunk_x, chunk_z));
    world::Chunk chunk = std::move(it->second.chunk);
    impl_->cache.erase(it);
    impl_->stats.resident = impl_->cache.size();
    return chunk;
}

void ChunkPipeline::trim(i32 centre_x, i32 centre_z, i32 keep) {
    for (auto it = impl_->cache.begin(); it != impl_->cache.end();) {
        const auto chunk_x = static_cast<i32>(it->first >> 32);
        const auto chunk_z = static_cast<i32>(static_cast<u32>(it->first));
        if (std::abs(chunk_x - centre_x) > keep || std::abs(chunk_z - centre_z) > keep) {
            it = impl_->cache.erase(it);
        } else {
            ++it;
        }
    }
    impl_->stats.resident = impl_->cache.size();
}

void ChunkPipeline::clear() {
    impl_->cache.clear();
    impl_->stats.resident = 0;
}

void ChunkPipeline::set_structures(const StructurePlacer*       placer,
                                   const StructureWorldSampler* sampler) noexcept {
    impl_->placer  = placer;
    impl_->sampler = sampler;
}

std::vector<std::string_view> ChunkPipeline::structure_starts(i32 chunk_x, i32 chunk_z) {
    return impl_->advance(chunk_x, chunk_z, ChunkStatus::StructureStarts).starts;
}

const PipelineStats& ChunkPipeline::stats() const noexcept { return impl_->stats; }

ChunkStatus ChunkPipeline::status_of(i32 chunk_x, i32 chunk_z) const {
    const auto it = impl_->cache.find(key_of(chunk_x, chunk_z));
    return it == impl_->cache.end() ? ChunkStatus::Empty : it->second.status;
}

usize ChunkPipeline::footprint_bytes() const {
    usize bytes = 0;
    for (const auto& [key, entry] : impl_->cache) {
        for (const auto& section : entry.chunk.sections()) {
            bytes += section.blocks().data().size() * sizeof(u64);
            bytes += section.blocks().palette().size() * sizeof(u16);
            bytes += section.biomes().data().size() * sizeof(u64);
            bytes += section.biomes().palette().size() * sizeof(u16);
        }
        // The four heightmaps: 37 longs each in a 384-block world.
        bytes += 4 * 37 * sizeof(u64);
    }
    return bytes;
}

}  // namespace ov::worldgen
