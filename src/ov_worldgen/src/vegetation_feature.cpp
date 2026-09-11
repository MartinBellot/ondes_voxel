#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/vegetation_feature.hpp"

#include "feature_json.hpp"
#include "overworld_feature.hpp"

#include "ov/base/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace ov::worldgen {

// ── The survival table ──────────────────────────────────────────────────────

namespace {

struct SurvivalEntry {
    std::string_view  name;
    PlantSurvivalRule rule;
};

/// One line per block vanilla's vegetation features place, grouped by the
/// `canSurvive` they inherit. Sorted, so the lookup is a binary search and so
/// that a missing block is obvious when reading.
constexpr std::array kSurvival{
    SurvivalEntry{"minecraft:acacia_sapling", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:allium", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:azalea", PlantSurvivalRule::DirtOrClay},
    SurvivalEntry{"minecraft:azure_bluet", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:birch_sapling", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:blue_orchid", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:cactus", PlantSurvivalRule::Cactus},
    SurvivalEntry{"minecraft:cave_vines", PlantSurvivalRule::Always},
    SurvivalEntry{"minecraft:cave_vines_plant", PlantSurvivalRule::Always},
    SurvivalEntry{"minecraft:cherry_sapling", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:cornflower", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:crimson_roots", PlantSurvivalRule::NyliumOrSoulSoil},
    SurvivalEntry{"minecraft:dandelion", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:dark_oak_sapling", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:dead_bush", PlantSurvivalRule::DeadBush},
    SurvivalEntry{"minecraft:fern", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:flowering_azalea", PlantSurvivalRule::DirtOrClay},
    SurvivalEntry{"minecraft:grass", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:jack_o_lantern", PlantSurvivalRule::Always},
    SurvivalEntry{"minecraft:jungle_sapling", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:large_fern", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:lilac", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:lily_of_the_valley", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:lily_pad", PlantSurvivalRule::Waterlily},
    SurvivalEntry{"minecraft:mangrove_propagule", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:melon", PlantSurvivalRule::Always},
    SurvivalEntry{"minecraft:moss_carpet", PlantSurvivalRule::NotAirBelow},
    SurvivalEntry{"minecraft:nether_sprouts", PlantSurvivalRule::NyliumOrSoulSoil},
    SurvivalEntry{"minecraft:oak_sapling", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:orange_tulip", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:oxeye_daisy", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:peony", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:pink_petals", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:pink_tulip", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:poppy", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:pumpkin", PlantSurvivalRule::Always},
    SurvivalEntry{"minecraft:red_tulip", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:rose_bush", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:seagrass", PlantSurvivalRule::Seagrass},
    SurvivalEntry{"minecraft:small_dripleaf", PlantSurvivalRule::SmallDripleaf},
    SurvivalEntry{"minecraft:spore_blossom", PlantSurvivalRule::HangingFromAbove},
    SurvivalEntry{"minecraft:spruce_sapling", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:sugar_cane", PlantSurvivalRule::SugarCane},
    SurvivalEntry{"minecraft:sunflower", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:sweet_berry_bush", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:tall_grass", PlantSurvivalRule::DirtOrFarmland},
    SurvivalEntry{"minecraft:warped_roots", PlantSurvivalRule::NyliumOrSoulSoil},
    SurvivalEntry{"minecraft:white_tulip", PlantSurvivalRule::DirtOrFarmland},
};

/// The two-tall plants. `simple_block` writes both halves of one of these and
/// refuses when the block above is not empty.
constexpr std::array kDoublePlants{
    std::string_view{"minecraft:large_fern"},   std::string_view{"minecraft:lilac"},
    std::string_view{"minecraft:peony"},        std::string_view{"minecraft:pitcher_plant"},
    std::string_view{"minecraft:rose_bush"},    std::string_view{"minecraft:small_dripleaf"},
    std::string_view{"minecraft:sunflower"},    std::string_view{"minecraft:tall_grass"},
};

}  // namespace

std::optional<PlantSurvivalRule> plant_survival_rule(std::string_view block_name) noexcept {
    const auto found = std::ranges::lower_bound(
        kSurvival, block_name, {}, [](const SurvivalEntry& entry) { return entry.name; });
    if (found == kSurvival.end() || found->name != block_name) {
        return std::nullopt;
    }
    return found->rule;
}

bool is_double_plant(std::string_view block_name) noexcept {
    return std::ranges::binary_search(kDoublePlants, block_name);
}

namespace {

// ── What the survival rules need to ask the world ───────────────────────────

/// The block sets and single blocks the survival rules name, resolved once.
struct GroundTags {
    std::vector<u16> dirt;
    std::vector<u16> dead_bush_may_place_on;
    std::vector<u16> nylium;
    std::vector<u16> sand;
    std::vector<u16> small_dripleaf_placeable;

    registry::BlockId farmland{0};
    registry::BlockId clay{0};
    registry::BlockId soul_soil{0};
    registry::BlockId water{0};
    registry::BlockId ice{0};
    registry::BlockId frosted_ice{0};
    registry::BlockId cactus{0};
    registry::BlockId sugar_cane{0};
    registry::BlockId magma_block{0};
};

using GroundTagsRef = std::shared_ptr<const GroundTags>;

[[nodiscard]] std::expected<GroundTagsRef, FeatureError> load_ground_tags(
    const registry::BlockRegistry& blocks, const BlockTags& tags) {
    auto result = std::make_shared<GroundTags>();

    const auto collect = [&](std::string_view tag,
                             std::vector<u16>& into) -> std::expected<void, FeatureError> {
        if (!tags.known(tag)) {
            OV_LOG_ERROR("worldgen: a vegetation feature needs tag {}, which was not exported",
                         tag);
            return std::unexpected(FeatureError::Missing);
        }
        for (usize index = 0; index < blocks.block_count(); ++index) {
            const registry::BlockId block{static_cast<u16>(index)};
            if (tags.contains(tag, block)) {
                into.push_back(block.value());
            }
        }
        std::ranges::sort(into);
        return {};
    };

    if (auto ok = collect("minecraft:dirt", result->dirt); !ok) return std::unexpected(ok.error());
    if (auto ok = collect("minecraft:dead_bush_may_place_on", result->dead_bush_may_place_on);
        !ok) {
        return std::unexpected(ok.error());
    }
    if (auto ok = collect("minecraft:nylium", result->nylium); !ok)
        return std::unexpected(ok.error());
    if (auto ok = collect("minecraft:sand", result->sand); !ok) return std::unexpected(ok.error());
    if (auto ok = collect("minecraft:small_dripleaf_placeable", result->small_dripleaf_placeable);
        !ok) {
        return std::unexpected(ok.error());
    }

    const auto named = [&](std::string_view name,
                           registry::BlockId& into) -> std::expected<void, FeatureError> {
        const auto block = blocks.find_block(name);
        if (!block) {
            OV_LOG_ERROR("worldgen: block {} is missing from the registry", name);
            return std::unexpected(FeatureError::Missing);
        }
        into = *block;
        return {};
    };
    if (auto ok = named("minecraft:farmland", result->farmland); !ok)
        return std::unexpected(ok.error());
    if (auto ok = named("minecraft:clay", result->clay); !ok) return std::unexpected(ok.error());
    if (auto ok = named("minecraft:soul_soil", result->soul_soil); !ok)
        return std::unexpected(ok.error());
    if (auto ok = named("minecraft:water", result->water); !ok) return std::unexpected(ok.error());
    if (auto ok = named("minecraft:ice", result->ice); !ok) return std::unexpected(ok.error());
    if (auto ok = named("minecraft:frosted_ice", result->frosted_ice); !ok)
        return std::unexpected(ok.error());
    if (auto ok = named("minecraft:cactus", result->cactus); !ok)
        return std::unexpected(ok.error());
    if (auto ok = named("minecraft:sugar_cane", result->sugar_cane); !ok)
        return std::unexpected(ok.error());
    if (auto ok = named("minecraft:magma_block", result->magma_block); !ok)
        return std::unexpected(ok.error());
    return std::static_pointer_cast<const GroundTags>(result);
}

[[nodiscard]] bool in_set(const std::vector<u16>& set, registry::BlockId block) {
    return std::ranges::binary_search(set, block.value());
}

[[nodiscard]] bool is_water_at(const registry::BlockRegistry& blocks, const GroundTags& ground,
                               const FeatureLevel& level, BlockPos at) {
    const auto state = level.block_at(at.x, at.y, at.z);
    const auto block = blocks.block_of(state);
    if (block == ground.water) {
        return true;
    }
    const auto waterlogged = blocks.find_property(block, "waterlogged");
    return waterlogged && blocks.property_value(state, *waterlogged) == "true";
}

/// The survival test, for one block at one place.
///
/// Never draws. Every caller in this file has already made every draw the
/// feature owes, which is what makes an error here cost blocks and not the
/// seed.
[[nodiscard]] bool can_survive(PlantSurvivalRule rule, const registry::BlockRegistry& blocks,
                               const GroundTags& ground, const FeatureLevel& level, BlockPos at) {
    const auto below_state = level.block_at(at.x, at.y - 1, at.z);
    const auto below       = blocks.block_of(below_state);
    switch (rule) {
        case PlantSurvivalRule::Always:
            return true;
        case PlantSurvivalRule::DirtOrFarmland:
            return in_set(ground.dirt, below) || below == ground.farmland;
        case PlantSurvivalRule::DirtOrClay:
            return in_set(ground.dirt, below) || below == ground.clay;
        case PlantSurvivalRule::DeadBush:
            return in_set(ground.dead_bush_may_place_on, below);
        case PlantSurvivalRule::NyliumOrSoulSoil:
            return in_set(ground.nylium, below) || below == ground.soul_soil ||
                   in_set(ground.dirt, below);
        case PlantSurvivalRule::Cactus: {
            constexpr std::array<std::array<i32, 2>, 4> sides{
                {{0, -1}, {0, 1}, {-1, 0}, {1, 0}}};
            for (const auto& side : sides) {
                const auto beside = blocks.block_of(
                    level.block_at(at.x + side[0], at.y, at.z + side[1]));
                if (blocks.blocks_motion(beside)) {
                    return false;
                }
            }
            if (below != ground.cactus && !in_set(ground.sand, below)) {
                return false;
            }
            const auto above = blocks.block_of(level.block_at(at.x, at.y + 1, at.z));
            return !(above == ground.water);
        }
        case PlantSurvivalRule::SugarCane: {
            if (below == ground.sugar_cane) {
                return true;
            }
            if (!in_set(ground.dirt, below) && !in_set(ground.sand, below)) {
                return false;
            }
            constexpr std::array<std::array<i32, 2>, 4> sides{
                {{0, -1}, {0, 1}, {-1, 0}, {1, 0}}};
            for (const auto& side : sides) {
                const BlockPos beside{at.x + side[0], at.y - 1, at.z + side[1]};
                const auto     block = blocks.block_of(level.block_at(beside.x, beside.y, beside.z));
                if (block == ground.frosted_ice ||
                    is_water_at(blocks, ground, level, beside)) {
                    return true;
                }
            }
            return false;
        }
        case PlantSurvivalRule::Waterlily:
            return below == ground.water || below == ground.ice;
        case PlantSurvivalRule::Seagrass: {
            if (below == ground.magma_block) {
                return false;
            }
            if (!blocks.face_is_sturdy(below_state, registry::BlockRegistry::Face::Up)) {
                return false;
            }
            return is_water_at(blocks, ground, level, at);
        }
        case PlantSurvivalRule::NotAirBelow:
            return !blocks.is_air(below);
        case PlantSurvivalRule::HangingFromAbove: {
            const auto above_state = level.block_at(at.x, at.y + 1, at.z);
            if (!blocks.face_is_sturdy(above_state, registry::BlockRegistry::Face::Down)) {
                return false;
            }
            return !is_water_at(blocks, ground, level, at);
        }
        case PlantSurvivalRule::SmallDripleaf: {
            if (in_set(ground.small_dripleaf_placeable, below)) {
                return true;
            }
            // A water *source* here: still water at level 0, or waterlogged.
            const auto here_state = level.block_at(at.x, at.y, at.z);
            const auto here       = blocks.block_of(here_state);
            bool       source     = false;
            if (here == ground.water) {
                const auto level_property = blocks.find_property(here, "level");
                source = !level_property || blocks.property_value(here_state, *level_property) == "0";
            } else if (const auto waterlogged = blocks.find_property(here, "waterlogged")) {
                source = blocks.property_value(here_state, *waterlogged) == "true";
            }
            return source && (in_set(ground.dirt, below) || below == ground.farmland);
        }
    }
    return false;
}

// ── simple_block ────────────────────────────────────────────────────────────

class SimpleBlockFeature final : public Feature {
public:
    SimpleBlockFeature(StateProviderRef provider, GroundTagsRef ground,
                       std::vector<std::pair<u16, PlantSurvivalRule>> rules,
                       std::vector<u16>                               double_plants)
        : provider_(std::move(provider)),
          ground_(std::move(ground)),
          rules_(std::move(rules)),
          double_plants_(std::move(double_plants)) {}

    [[nodiscard]] std::string_view type_name() const override { return "simple_block"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        // The provider is asked first and unconditionally. It may draw, and it
        // draws whether or not anything can live here.
        const auto state = provider_->state(level, random, at);
        const auto block = context.blocks->block_of(state);

        const auto rule = std::ranges::find_if(
            rules_, [&](const auto& entry) { return entry.first == block.value(); });
        if (rule == rules_.end()) {
            return false;
        }
        if (!can_survive(rule->second, *context.blocks, *ground_, level, at)) {
            return false;
        }
        if (std::ranges::binary_search(double_plants_, block.value())) {
            const auto above = context.blocks->block_of(level.block_at(at.x, at.y + 1, at.z));
            if (!context.blocks->is_air(above)) {
                return false;
            }
            const auto half = context.blocks->find_property(block, "half");
            if (!half) {
                return false;
            }
            const auto lower = with_value(*context.blocks, state, *half, "lower");
            const auto upper = with_value(*context.blocks, state, *half, "upper");
            (void)level.set_block(at.x, at.y, at.z, lower);
            (void)level.set_block(at.x, at.y + 1, at.z, upper);
            return true;
        }
        return level.set_block(at.x, at.y, at.z, state);
    }

private:
    [[nodiscard]] static registry::BlockStateId with_value(const registry::BlockRegistry& blocks,
                                                           registry::BlockStateId         state,
                                                           const registry::PropertyView& property,
                                                           std::string_view value) {
        for (usize index = 0; index < property.values.size(); ++index) {
            if (property.values[index] == value) {
                return blocks.with_property(state, property, static_cast<u16>(index));
            }
        }
        return state;
    }

    StateProviderRef                              provider_;
    GroundTagsRef                                 ground_;
    std::vector<std::pair<u16, PlantSurvivalRule>> rules_;
    std::vector<u16>                              double_plants_;
};

// ── random_patch, flower, no_bonemeal_flower ────────────────────────────────

/// Sixty-four tries around a point, six draws each, every time.
///
/// The three offsets are `nextInt(n) - nextInt(n)`, and Java fixes the order of
/// the two draws while C++ does not. Split into named locals, x then y then z,
/// which is the order the argument list is evaluated in.
class RandomPatchFeature final : public Feature {
public:
    RandomPatchFeature(std::shared_ptr<const PlacedFeature> inner, i32 tries, i32 xz_spread,
                       i32 y_spread, std::string_view name)
        : inner_(std::move(inner)),
          tries_(tries),
          xz_spread_(xz_spread),
          y_spread_(y_spread),
          name_(name) {}

    [[nodiscard]] std::string_view type_name() const override { return name_; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const i32 xz    = xz_spread_ + 1;
        const i32 y     = y_spread_ + 1;
        i32       hits  = 0;
        for (i32 attempt = 0; attempt < tries_; ++attempt) {
            const i32 x_hi = random.next_int(xz);
            const i32 x_lo = random.next_int(xz);
            const i32 y_hi = random.next_int(y);
            const i32 y_lo = random.next_int(y);
            const i32 z_hi = random.next_int(xz);
            const i32 z_lo = random.next_int(xz);
            const BlockPos here = at.offset(x_hi - x_lo, y_hi - y_lo, z_hi - z_lo);
            bool           wrote = false;
            expand(inner_->placement, context, level, random, here,
                   [&](BlockPos where) {
                       wrote |= inner_->feature->place(context, level, random, where);
                   });
            if (wrote) {
                ++hits;
            }
        }
        return hits > 0;
    }

private:
    std::shared_ptr<const PlacedFeature> inner_;
    i32                                  tries_;
    i32                                  xz_spread_;
    i32                                  y_spread_;
    std::string_view                     name_;
};

// ── block_pile ──────────────────────────────────────────────────────────────

class BlockPileFeature final : public Feature {
public:
    BlockPileFeature(StateProviderRef provider, GroundTagsRef ground, registry::BlockId grass)
        : provider_(std::move(provider)), ground_(std::move(ground)), grass_(grass) {}

    [[nodiscard]] std::string_view type_name() const override { return "block_pile"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        if (at.y < level.min_y() + 5) {
            return false;
        }
        const i32 half_x_draw = random.next_int(2);
        const i32 half_x      = 2 + half_x_draw;
        const i32 half_z_draw = random.next_int(2);
        const i32 half_z      = 2 + half_z_draw;

        // `BlockPos.betweenClosed` walks x fastest, then y, then z. Any other
        // order draws the same numbers at different places.
        for (i32 z = at.z - half_z; z <= at.z + half_z; ++z) {
            for (i32 y = at.y; y <= at.y + 1; ++y) {
                for (i32 x = at.x - half_x; x <= at.x + half_x; ++x) {
                    const i32 dx = at.x - x;
                    const i32 dz = at.z - z;
                    const f32 wide = random.next_float();
                    const f32 narrow = random.next_float();
                    if (static_cast<f32>(dx * dx + dz * dz) <= wide * 10.0F - narrow * 6.0F) {
                        try_place(context, level, random, {x, y, z});
                        continue;
                    }
                    const f32 stray = random.next_float();
                    if (static_cast<f64>(stray) < 0.031) {
                        try_place(context, level, random, {x, y, z});
                    }
                }
            }
        }
        return true;
    }

private:
    void try_place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
                   BlockPos at) const {
        const auto here = context.blocks->block_of(level.block_at(at.x, at.y, at.z));
        if (!context.blocks->is_air(here)) {
            return;
        }
        const auto below_state = level.block_at(at.x, at.y - 1, at.z);
        const auto below       = context.blocks->block_of(below_state);
        bool       may         = false;
        if (below == grass_) {
            // Grass gets a coin toss, and the toss happens only for grass.
            may = random.next_boolean();
        } else {
            may = context.blocks->face_is_sturdy(below_state,
                                                 registry::BlockRegistry::Face::Up);
        }
        if (!may) {
            return;
        }
        (void)level.set_block(at.x, at.y, at.z, provider_->state(level, random, at));
    }

    StateProviderRef  provider_;
    GroundTagsRef     ground_;
    registry::BlockId grass_;
};

// ── block_column ────────────────────────────────────────────────────────────

class BlockColumnFeature final : public Feature {
public:
    struct Layer {
        IntProviderRef   height;
        StateProviderRef state;
    };

    BlockColumnFeature(std::vector<Layer> layers, std::array<i32, 3> direction,
                       BlockPredicateRef allowed, bool prioritize_tip)
        : layers_(std::move(layers)),
          direction_(direction),
          allowed_(std::move(allowed)),
          prioritize_tip_(prioritize_tip) {}

    [[nodiscard]] std::string_view type_name() const override { return "block_column"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        // Every layer's height is drawn, in order, before anything is written.
        std::vector<i32> heights;
        heights.reserve(layers_.size());
        i32 total = 0;
        for (const Layer& layer : layers_) {
            const i32 height = layer.height->sample(random);
            heights.push_back(height);
            total += height;
        }
        if (total == 0) {
            return false;
        }

        BlockPos probe = at.offset(direction_[0], direction_[1], direction_[2]);
        for (i32 step = 0; step < total; ++step) {
            if (!allowed_->test(level, probe)) {
                truncate(heights, total, step);
                break;
            }
            probe = probe.offset(direction_[0], direction_[1], direction_[2]);
        }

        BlockPos cursor = at;
        for (usize index = 0; index < layers_.size(); ++index) {
            for (i32 step = 0; step < heights[index]; ++step) {
                (void)level.set_block(cursor.x, cursor.y, cursor.z,
                                      layers_[index].state->state(level, random, cursor));
                cursor = cursor.offset(direction_[0], direction_[1], direction_[2]);
            }
        }
        return true;
    }

private:
    void truncate(std::vector<i32>& heights, i32 total, i32 allowed) const {
        i32       excess = total - allowed;
        const i32 step   = prioritize_tip_ ? 1 : -1;
        const i32 from   = prioritize_tip_ ? 0 : static_cast<i32>(heights.size()) - 1;
        const i32 to     = prioritize_tip_ ? static_cast<i32>(heights.size()) : -1;
        for (i32 index = from; index != to && excess > 0; index += step) {
            const i32 taken = std::min(heights[static_cast<usize>(index)], excess);
            excess -= taken;
            heights[static_cast<usize>(index)] -= taken;
        }
    }

    std::vector<Layer> layers_;
    std::array<i32, 3> direction_;
    BlockPredicateRef  allowed_;
    bool               prioritize_tip_;
};

// ── The three selectors ─────────────────────────────────────────────────────

class SimpleRandomSelectorFeature final : public Feature {
public:
    explicit SimpleRandomSelectorFeature(
        std::vector<std::shared_ptr<const PlacedFeature>> features)
        : features_(std::move(features)) {}

    [[nodiscard]] std::string_view type_name() const override {
        return "simple_random_selector";
    }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const auto index = static_cast<usize>(
            random.next_int(static_cast<i32>(features_.size())));
        return run(features_[index], context, level, random, at);
    }

    static bool run(const std::shared_ptr<const PlacedFeature>& placed,
                    const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
                    BlockPos at) {
        bool wrote = false;
        expand(placed->placement, context, level, random, at, [&](BlockPos where) {
            wrote |= placed->feature->place(context, level, random, where);
        });
        return wrote;
    }

private:
    std::vector<std::shared_ptr<const PlacedFeature>> features_;
};

class RandomSelectorFeature final : public Feature {
public:
    struct Weighted {
        std::shared_ptr<const PlacedFeature> feature;
        f32                                  chance{0.0F};
    };

    RandomSelectorFeature(std::vector<Weighted> features,
                          std::shared_ptr<const PlacedFeature> fallback)
        : features_(std::move(features)), fallback_(std::move(fallback)) {}

    [[nodiscard]] std::string_view type_name() const override { return "random_selector"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        // One draw per candidate until one takes it: the first hit wins and the
        // rest are never rolled. This is what makes the order in the file part
        // of the seed.
        for (const Weighted& candidate : features_) {
            const f32 roll = random.next_float();
            if (roll < candidate.chance) {
                return SimpleRandomSelectorFeature::run(candidate.feature, context, level, random,
                                                        at);
            }
        }
        return SimpleRandomSelectorFeature::run(fallback_, context, level, random, at);
    }

private:
    std::vector<Weighted>                features_;
    std::shared_ptr<const PlacedFeature> fallback_;
};

class RandomBooleanSelectorFeature final : public Feature {
public:
    RandomBooleanSelectorFeature(std::shared_ptr<const PlacedFeature> when_true,
                                 std::shared_ptr<const PlacedFeature> when_false)
        : when_true_(std::move(when_true)), when_false_(std::move(when_false)) {}

    [[nodiscard]] std::string_view type_name() const override {
        return "random_boolean_selector";
    }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const bool heads = random.next_boolean();
        return SimpleRandomSelectorFeature::run(heads ? when_true_ : when_false_, context, level,
                                                random, at);
    }

private:
    std::shared_ptr<const PlacedFeature> when_true_;
    std::shared_ptr<const PlacedFeature> when_false_;
};

// ── Reading them out of the datapack ────────────────────────────────────────

[[nodiscard]] std::expected<std::shared_ptr<const PlacedFeature>, FeatureError> inner_feature(
    Json node, std::string_view key, const registry::BlockRegistry& blocks,
    const BlockTags& tags, const FeatureResolver& resolve) {
    auto field = node.at_key(key);
    if (field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    return parse_inline_placed_feature(field.value(), blocks, tags, resolve);
}

/// An int provider that may be a `weighted_list` of int providers.
///
/// `parse_int_provider` in placement.cpp reads a weighted list of plain
/// numbers, which is what the placement modifiers use. The cave vines' column
/// heights are a weighted list of *uniform providers*, which is two draws — one
/// for the entry, one inside it — and needs its own reader.
class WeightedListInt final : public IntProvider {
public:
    explicit WeightedListInt(std::vector<std::pair<IntProviderRef, i32>> entries)
        : entries_(std::move(entries)) {
        for (const auto& [provider, weight] : entries_) {
            (void)provider;
            total_ += weight;
        }
    }

    [[nodiscard]] i32 sample(FeatureRandom& random) const override {
        i32 roll = random.next_int(total_);
        for (const auto& [provider, weight] : entries_) {
            roll -= weight;
            if (roll < 0) {
                return provider->sample(random);
            }
        }
        return entries_.back().first->sample(random);
    }

private:
    std::vector<std::pair<IntProviderRef, i32>> entries_;
    i32                                         total_{0};
};

[[nodiscard]] std::expected<IntProviderRef, FeatureError> parse_nested_int_provider(Json node) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS ||
        strip_namespace(type) != "weighted_list") {
        return parse_int_provider(node);
    }
    simdjson::dom::array list;
    if (node.at_key("distribution").get(list) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    std::vector<std::pair<IntProviderRef, i32>> entries;
    for (auto entry : list) {
        auto data   = entry.at_key("data");
        i64  weight = 1;
        if (data.error() != simdjson::SUCCESS ||
            entry.at_key("weight").get(weight) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto inner = parse_nested_int_provider(data.value());
        if (!inner) {
            return inner;
        }
        entries.emplace_back(*inner, static_cast<i32>(weight));
    }
    if (entries.empty()) {
        return std::unexpected(FeatureError::Malformed);
    }
    return std::static_pointer_cast<const IntProvider>(
        std::make_shared<const WeightedListInt>(std::move(entries)));
}

}  // namespace

bool is_vegetation_feature(std::string_view kind) noexcept {
    static constexpr std::array<std::string_view, 9> kKinds{
        "block_column",   "block_pile",        "flower",
        "no_bonemeal_flower", "random_boolean_selector", "random_patch",
        "random_selector", "simple_block",     "simple_random_selector",
    };
    return std::ranges::binary_search(kKinds, kind);
}

std::expected<FeatureRef, FeatureError> parse_vegetation_feature(
    std::string_view kind, Json config, const registry::BlockRegistry& blocks,
    const BlockTags& tags, const FeatureResolver& resolve) {
    if (kind == "simple_block") {
        auto field = config.at_key("to_place");
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto provider = parse_state_provider(field.value(), blocks, tags);
        if (!provider) {
            return std::unexpected(provider.error());
        }
        auto ground = load_ground_tags(blocks, tags);
        if (!ground) {
            return std::unexpected(ground.error());
        }

        // Every state the provider can produce must have a survival rule, and
        // the check happens here rather than in the world: a feature that would
        // silently place nothing is worse than one that refuses to load.
        const auto states = (*provider)->possible_states();
        if (states.empty()) {
            OV_LOG_ERROR(
                "worldgen: a simple_block's provider cannot be enumerated, so its blocks' "
                "survival rules cannot be checked");
            return std::unexpected(FeatureError::Unsupported);
        }
        std::vector<std::pair<u16, PlantSurvivalRule>> rules;
        std::vector<u16>                              double_plants;
        for (const auto state : states) {
            const auto block = blocks.block_of(state);
            const auto name  = blocks.block_name(block);
            const auto rule  = plant_survival_rule(name);
            if (!rule) {
                OV_LOG_ERROR("worldgen: no survival rule for {}, so simple_block is refused",
                             name);
                return std::unexpected(FeatureError::Unsupported);
            }
            if (std::ranges::none_of(rules, [&](const auto& entry) {
                    return entry.first == block.value();
                })) {
                rules.emplace_back(block.value(), *rule);
            }
            if (is_double_plant(name)) {
                double_plants.push_back(block.value());
            }
        }
        std::ranges::sort(double_plants);
        const auto unique = std::ranges::unique(double_plants);
        double_plants.erase(unique.begin(), unique.end());
        return std::static_pointer_cast<const Feature>(std::make_shared<const SimpleBlockFeature>(
            *provider, *ground, std::move(rules), std::move(double_plants)));
    }

    if (kind == "random_patch" || kind == "flower" || kind == "no_bonemeal_flower") {
        auto inner = inner_feature(config, "feature", blocks, tags, resolve);
        if (!inner) {
            return std::unexpected(inner.error());
        }
        i64 tries     = 96;
        i64 xz_spread = 7;
        i64 y_spread  = 3;
        (void)config.at_key("tries").get(tries);
        (void)config.at_key("xz_spread").get(xz_spread);
        (void)config.at_key("y_spread").get(y_spread);
        // The three names are three registry entries over one algorithm; the
        // difference is what bonemeal may trigger, which worldgen never asks.
        const std::string_view name = kind == "random_patch"        ? "random_patch"
                                      : kind == "flower"           ? "flower"
                                                                   : "no_bonemeal_flower";
        return std::static_pointer_cast<const Feature>(std::make_shared<const RandomPatchFeature>(
            *inner, static_cast<i32>(tries), static_cast<i32>(xz_spread),
            static_cast<i32>(y_spread), name));
    }

    if (kind == "block_pile") {
        auto field = config.at_key("state_provider");
        if (field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto provider = parse_state_provider(field.value(), blocks, tags);
        if (!provider) {
            return std::unexpected(provider.error());
        }
        auto ground = load_ground_tags(blocks, tags);
        if (!ground) {
            return std::unexpected(ground.error());
        }
        const auto grass = blocks.find_block("minecraft:grass_block");
        if (!grass) {
            return std::unexpected(FeatureError::Missing);
        }
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const BlockPileFeature>(*provider, *ground, *grass));
    }

    if (kind == "block_column") {
        simdjson::dom::array layers;
        if (config.at_key("layers").get(layers) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        std::vector<BlockColumnFeature::Layer> built;
        for (auto entry : layers) {
            auto height_field = entry.at_key("height");
            auto state_field  = entry.at_key("provider");
            if (height_field.error() != simdjson::SUCCESS ||
                state_field.error() != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            auto height = parse_nested_int_provider(height_field.value());
            if (!height) return std::unexpected(height.error());
            auto state = parse_state_provider(state_field.value(), blocks, tags);
            if (!state) return std::unexpected(state.error());
            built.push_back({*height, *state});
        }
        std::array<i32, 3> direction{0, 1, 0};
        std::string_view   name;
        if (config.at_key("direction").get(name) == simdjson::SUCCESS) {
            if (name == "down") direction = {0, -1, 0};
            else if (name == "up") direction = {0, 1, 0};
            else if (name == "north") direction = {0, 0, -1};
            else if (name == "south") direction = {0, 0, 1};
            else if (name == "west") direction = {-1, 0, 0};
            else if (name == "east") direction = {1, 0, 0};
            else return std::unexpected(FeatureError::Malformed);
        }
        auto allowed_field = config.at_key("allowed_placement");
        if (allowed_field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto allowed = parse_block_predicate(allowed_field.value(), blocks, tags);
        if (!allowed) return std::unexpected(allowed.error());
        bool prioritize_tip = false;
        (void)config.at_key("prioritize_tip").get(prioritize_tip);
        return std::static_pointer_cast<const Feature>(std::make_shared<const BlockColumnFeature>(
            std::move(built), direction, *allowed, prioritize_tip));
    }

    if (kind == "simple_random_selector") {
        simdjson::dom::array list;
        if (config.at_key("features").get(list) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        std::vector<std::shared_ptr<const PlacedFeature>> features;
        for (auto entry : list) {
            auto parsed = parse_inline_placed_feature(entry, blocks, tags, resolve);
            if (!parsed) return std::unexpected(parsed.error());
            features.push_back(*parsed);
        }
        if (features.empty()) {
            return std::unexpected(FeatureError::Malformed);
        }
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const SimpleRandomSelectorFeature>(std::move(features)));
    }

    if (kind == "random_selector") {
        simdjson::dom::array list;
        if (config.at_key("features").get(list) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        std::vector<RandomSelectorFeature::Weighted> features;
        for (auto entry : list) {
            auto parsed = inner_feature(entry, "feature", blocks, tags, resolve);
            if (!parsed) return std::unexpected(parsed.error());
            f64 chance = 0.0;
            if (entry.at_key("chance").get(chance) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            features.push_back({*parsed, static_cast<f32>(chance)});
        }
        auto fallback = inner_feature(config, "default", blocks, tags, resolve);
        if (!fallback) return std::unexpected(fallback.error());
        return std::static_pointer_cast<const Feature>(std::make_shared<const RandomSelectorFeature>(
            std::move(features), *fallback));
    }

    if (kind == "random_boolean_selector") {
        auto when_true = inner_feature(config, "feature_true", blocks, tags, resolve);
        if (!when_true) return std::unexpected(when_true.error());
        auto when_false = inner_feature(config, "feature_false", blocks, tags, resolve);
        if (!when_false) return std::unexpected(when_false.error());
        return std::static_pointer_cast<const Feature>(
            std::make_shared<const RandomBooleanSelectorFeature>(*when_true, *when_false));
    }

    return std::unexpected(FeatureError::Unsupported);
}


// ── would_survive ───────────────────────────────────────────────────────────

namespace {

/// `state.canSurvive(level, pos + offset)`, for the states worldgen asks about.
///
/// The rule is looked up when the datapack is read, so a state with no rule is
/// a refusal at load and never a silent "yes" in the world. Every state vanilla
/// uses here is a sapling; the offset is in the format and vanilla always
/// leaves it at zero.
class SurvivalPredicate final : public BlockPredicate {
public:
    SurvivalPredicate(PlantSurvivalRule rule, BlockPos offset, GroundTagsRef ground,
                      const registry::BlockRegistry& blocks)
        : rule_(rule), offset_(offset), ground_(std::move(ground)), blocks_(&blocks) {}

    [[nodiscard]] bool test(const FeatureLevel& level, BlockPos at) const override {
        return can_survive(rule_, *blocks_, *ground_, level,
                           at.offset(offset_.x, offset_.y, offset_.z));
    }

private:
    PlantSurvivalRule              rule_;
    BlockPos                       offset_;
    GroundTagsRef                  ground_;
    const registry::BlockRegistry* blocks_;
};

}  // namespace

std::expected<BlockPredicateRef, FeatureError> parse_survival_predicate(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    auto state_field = node.at_key("state");
    if (state_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto state = parse_block_state(state_field.value(), blocks);
    if (!state) {
        return std::unexpected(state.error());
    }
    const auto name = blocks.block_name(blocks.block_of(*state));
    const auto rule = plant_survival_rule(name);
    if (!rule) {
        OV_LOG_ERROR("worldgen: no survival rule for {}, so would_survive is refused", name);
        return std::unexpected(FeatureError::Unsupported);
    }

    BlockPos             offset{0, 0, 0};
    simdjson::dom::array raw;
    if (node.at_key("offset").get(raw) == simdjson::SUCCESS) {
        std::array<i32, 3> parts{0, 0, 0};
        usize              index = 0;
        for (auto value : raw) {
            i64 component = 0;
            if (index >= parts.size() || value.get(component) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            parts[index++] = static_cast<i32>(component);
        }
        offset = BlockPos{parts[0], parts[1], parts[2]};
    }

    auto ground = load_ground_tags(blocks, tags);
    if (!ground) {
        return std::unexpected(ground.error());
    }
    return std::static_pointer_cast<const BlockPredicate>(
        std::make_shared<const SurvivalPredicate>(*rule, offset, *ground, blocks));
}

}  // namespace ov::worldgen
