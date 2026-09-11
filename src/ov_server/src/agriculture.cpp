#define OV_LOG_CATEGORY "server"

#include "agriculture.hpp"

#include "ov/base/log.hpp"
#include "ov/world/chunk.hpp"
#include "ov/worldgen/feature.hpp"
#include "ov/worldgen/placement.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace ov::server {

namespace {

/// The level a tree is written into: the server's own, seen as worldgen sees
/// a region under construction.
class LevelFeatureView final : public worldgen::FeatureLevel {
public:
    explicit LevelFeatureView(world::LevelWriter& level) : level_{level} {}

    [[nodiscard]] registry::BlockStateId block_at(i32 x, i32 y, i32 z) const override {
        return level_.block_at({x, y, z});
    }

    bool set_block(i32 x, i32 y, i32 z, registry::BlockStateId state) override {
        // A branch reaching into a chunk that is not resident is dropped, the
        // same answer a worldgen region gives past its edge — and the only
        // one that does not generate a chunk on the tick thread.
        if (outside_build_height(y) || !level_.is_loaded({x, y, z})) {
            return false;
        }
        level_.set_block({x, y, z}, state);
        return true;
    }

    /// A `LevelView` carries no heightmap. The tree feature and its
    /// decorators never ask for one; if a future one does, it gets the real
    /// answer by scanning rather than a made-up constant.
    [[nodiscard]] i32 height(world::HeightmapType, i32 x, i32 z) const override {
        const registry::BlockRegistry& blocks = level_.blocks();
        for (i32 y = max_y(); y >= min_y(); --y) {
            if (!blocks.is_air(blocks.block_of(level_.block_at({x, y, z})))) {
                return y + 1;
            }
        }
        return min_y();
    }

    /// Trees do not read the biome. An empty name rather than a guessed one:
    /// anything that did read it would find no biome, not the wrong biome.
    [[nodiscard]] std::string_view biome_at(i32, i32, i32) const override { return {}; }

    [[nodiscard]] i32 min_y() const override { return level_.shape().min_y; }
    [[nodiscard]] i32 world_height() const override {
        return static_cast<i32>(level_.shape().height);
    }
    [[nodiscard]] i32 sea_level() const override { return 63; }

private:
    world::LevelWriter& level_;
};

/// What each sapling grows, from the wiki's Sapling and Tree pages.
struct Growth {
    std::string_view sapling;
    /// Grown by a single sapling. Empty when one alone grows nothing.
    std::string_view single;
    /// The chance the single one is `rare` instead. Oak: one in ten is fancy.
    f32              rare_chance;
    std::string_view rare;
    /// Grown by a two-by-two of the sapling. Empty when there is none.
    std::string_view mega;
    /// A second two-by-two tree, chosen half the time — the giant spruce's
    /// two shapes.
    std::string_view mega_alt;
};

constexpr Growth kGrowth[] = {
    {"minecraft:oak_sapling", "minecraft:oak", 0.1F, "minecraft:fancy_oak", "", ""},
    {"minecraft:birch_sapling", "minecraft:birch", 0.0F, "", "", ""},
    {"minecraft:spruce_sapling", "minecraft:spruce", 0.0F, "", "minecraft:mega_spruce",
     "minecraft:mega_pine"},
    {"minecraft:jungle_sapling", "minecraft:jungle_tree_no_vine", 0.0F, "",
     "minecraft:mega_jungle_tree", ""},
    {"minecraft:acacia_sapling", "minecraft:acacia", 0.0F, "", "", ""},
    {"minecraft:cherry_sapling", "minecraft:cherry", 0.0F, "", "", ""},
    {"minecraft:dark_oak_sapling", "", 0.0F, "", "minecraft:dark_oak", ""},
    {"minecraft:mangrove_propagule", "minecraft:mangrove", 0.15F, "minecraft:tall_mangrove", "",
     ""},
    {"minecraft:azalea", "minecraft:azalea_tree", 0.0F, "", "", ""},
    {"minecraft:flowering_azalea", "minecraft:azalea_tree", 0.0F, "", "", ""},
};

}  // namespace

// ── Trees ───────────────────────────────────────────────────────────────────

struct TreeGrower::Impl {
    const registry::BlockRegistry*          blocks{nullptr};
    std::optional<worldgen::FeatureRegistry> features;

    [[nodiscard]] bool place(world::LevelWriter& level, std::string_view feature_name,
                             BlockPos at, gameplay::PlantRandom& random) const {
        const worldgen::Feature* feature = features->configured(feature_name);
        if (feature == nullptr) {
            OV_LOG_WARN("tree feature '{}' is not available; the sapling stays", feature_name);
            return false;
        }
        worldgen::FeatureContext context;
        context.blocks       = blocks;
        context.feature_name = feature_name;
        worldgen::FeatureRandom tree_random{worldgen::FeatureRandom::Kind::Xoroshiro,
                                            random.next_long()};
        LevelFeatureView view{level};
        return feature->place(context, view, tree_random, at);
    }
};

TreeGrower::TreeGrower(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
TreeGrower::~TreeGrower() = default;

std::unique_ptr<TreeGrower> TreeGrower::load(const std::filesystem::path&   data_root,
                                             const registry::BlockRegistry& blocks) {
    auto features = worldgen::FeatureRegistry::load(
        data_root / "vanilla" / "1.20.1" / "generated" / "data" / "minecraft", blocks);
    if (!features) {
        return nullptr;
    }
    auto impl      = std::make_unique<Impl>();
    impl->blocks   = &blocks;
    impl->features = std::move(*features);
    return std::unique_ptr<TreeGrower>{new TreeGrower{std::move(impl)}};
}

bool TreeGrower::grow(world::LevelWriter& level, BlockPos pos, registry::BlockStateId sapling,
                      gameplay::PlantRandom& random) const {
    const registry::BlockRegistry& blocks = *impl_->blocks;
    const registry::BlockId        block  = blocks.block_of(sapling);
    const std::string_view         name   = blocks.block_name(block);
    const auto growth = std::ranges::find_if(kGrowth, [&](const Growth& g) { return g.sapling == name; });
    if (growth == std::end(kGrowth)) {
        return false;
    }
    const auto same = [&](BlockPos at) { return blocks.block_of(level.block_at(at)) == block; };

    // A two-by-two first, as the game looks for one: the four corners the
    // sapling could be the member of, north-west first.
    if (!growth->mega.empty()) {
        for (i32 i = 0; i >= -1; --i) {
            for (i32 j = 0; j >= -1; --j) {
                const BlockPos corner = pos.offset(i, 0, j);
                if (!same(corner) || !same(corner.offset(1, 0, 0)) ||
                    !same(corner.offset(0, 0, 1)) || !same(corner.offset(1, 0, 1))) {
                    continue;
                }
                const std::string_view feature =
                    !growth->mega_alt.empty() && random.next_boolean() ? growth->mega_alt
                                                                        : growth->mega;
                const std::array<BlockPos, 4> four{corner, corner.offset(1, 0, 0),
                                                   corner.offset(0, 0, 1), corner.offset(1, 0, 1)};
                std::array<registry::BlockStateId, 4> kept{};
                for (usize k = 0; k < 4; ++k) {
                    kept[k] = level.block_at(four[k]);
                    level.set_block(four[k], registry::kAirState);
                }
                if (impl_->place(level, feature, corner, random)) {
                    return true;
                }
                for (usize k = 0; k < 4; ++k) {
                    level.set_block(four[k], kept[k]);
                }
                return false;
            }
        }
    }
    if (growth->single.empty()) {
        // A lone dark oak sapling grows nothing, ever.
        return false;
    }
    std::string_view feature = growth->single;
    if (growth->rare_chance > 0.0F && random.next_float() < growth->rare_chance) {
        feature = growth->rare;
    }
    level.set_block(pos, registry::kAirState);
    if (impl_->place(level, feature, pos, random)) {
        return true;
    }
    level.set_block(pos, sapling);
    return false;
}

// ── The environment ─────────────────────────────────────────────────────────

ServerPlantEnvironment::ServerPlantEnvironment(PlantHooks hooks, const TreeGrower* trees)
    : hooks_{std::move(hooks)}, trees_{trees} {}

u8 ServerPlantEnvironment::block_light(BlockPos pos) const {
    return hooks_.block_light ? hooks_.block_light(pos) : u8{0};
}

u8 ServerPlantEnvironment::sky_light(BlockPos pos) const {
    return hooks_.sky_light ? hooks_.sky_light(pos) : u8{0};
}

u8 ServerPlantEnvironment::sky_darken() const {
    return hooks_.sky_darken ? hooks_.sky_darken() : u8{0};
}

void ServerPlantEnvironment::drop_block(BlockPos pos, registry::BlockStateId state) {
    if (hooks_.drop_block) {
        hooks_.drop_block(pos, state);
    }
}

bool ServerPlantEnvironment::grow_tree(world::LevelWriter& level, BlockPos pos,
                                       registry::BlockStateId sapling,
                                       gameplay::PlantRandom& random) {
    if (trees_ == nullptr) {
        // Refused and counted: a sapling that silently never grows looks like
        // one that is merely unlucky.
        ++trees_refused_;
        return false;
    }
    return trees_->grow(level, pos, sapling, random);
}

// ── The picking ─────────────────────────────────────────────────────────────

RandomTicks::RandomTicks(u64 seed) : random_{static_cast<i64>(seed)} { chunks_.reserve(1024); }

void RandomTicks::select(const world::ChunkMap& chunks, std::span<const Vec3d> players) {
    chunks_.clear();
    constexpr i32 kReach = static_cast<i32>(kPlayerRange) / 16 + 1;
    for (const Vec3d& who : players) {
        const i32 centre_x = static_cast<i32>(std::floor(who.x)) >> 4;
        const i32 centre_z = static_cast<i32>(std::floor(who.z)) >> 4;
        for (i32 dz = -kReach; dz <= kReach; ++dz) {
            for (i32 dx = -kReach; dx <= kReach; ++dx) {
                const ChunkPos pos{centre_x + dx, centre_z + dz};
                // The chunk's centre, horizontally, against the player: the
                // game's "close enough for spawning", which is also its gate
                // for random ticks.
                const f64 off_x = static_cast<f64>(pos.x) * 16.0 + 8.0 - who.x;
                const f64 off_z = static_cast<f64>(pos.z) * 16.0 + 8.0 - who.z;
                if (off_x * off_x + off_z * off_z >= kPlayerRange * kPlayerRange) {
                    continue;
                }
                if (!chunks.is_ticking(pos) ||
                    std::ranges::find(chunks_, pos) != chunks_.end()) {
                    continue;
                }
                chunks_.push_back(pos);
            }
        }
    }
    std::ranges::sort(chunks_, [](ChunkPos a, ChunkPos b) {
        return a.z != b.z ? a.z < b.z : a.x < b.x;
    });
}

void RandomTicks::tick_section(world::LevelWriter& level, const world::ChunkSection& section,
                               BlockPos origin, const gameplay::Plants& plants,
                               gameplay::PlantEnvironment& env, RandomTickStats& stats) {
    if (section.is_empty()) {
        return;
    }
    ++stats.sections;
    for (i32 k = 0; k < speed_; ++k) {
        // Three draws, in this order, as named locals: the operands of one
        // expression would be sequenced however the compiler liked.
        const i32 x = random_.next_int(16);
        const i32 y = random_.next_int(16);
        const i32 z = random_.next_int(16);
        ++stats.picks;
        const registry::BlockStateId state =
            section.get_block(static_cast<usize>(x), static_cast<usize>(y), static_cast<usize>(z));
        if (!plants.ticks_randomly(state)) {
            // ── fire ── lava's random tick, which lights fire
            if (extension_ != nullptr && extension_->ticks_randomly(state)) {
                ++stats.ticked;
                extension_->random_tick(level, origin.offset(x, y, z), state);
            }
            continue;
        }
        ++stats.ticked;
        plants.random_tick(level, env, origin.offset(x, y, z), state, random_);
    }
}

RandomTickStats RandomTicks::run(world::LevelWriter& level, world::ChunkMap& chunks,
                                 const gameplay::Plants& plants, gameplay::PlantEnvironment& env) {
    RandomTickStats stats;
    if (speed_ <= 0) {
        return stats;
    }
    const i32 min_section = level.shape().min_section();
    for (const ChunkPos pos : chunks_) {
        const world::Chunk* chunk = chunks.find(pos);
        if (chunk == nullptr) {
            continue;
        }
        bool surrounded = true;
        for (i32 dz = -1; dz <= 1 && surrounded; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (!chunks.contains(ChunkPos{pos.x + dx, pos.z + dz})) {
                    surrounded = false;
                    break;
                }
            }
        }
        if (!surrounded) {
            continue;
        }
        ++stats.chunks;
        const auto sections = chunk->sections();
        for (usize i = 0; i < sections.size(); ++i) {
            const BlockPos origin{pos.x * 16, (min_section + static_cast<i32>(i)) * 16, pos.z * 16};
            tick_section(level, sections[i], origin, plants, env, stats);
        }
    }
    return stats;
}

}  // namespace ov::server
