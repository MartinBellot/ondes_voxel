#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/feature.hpp"

#include "feature_json.hpp"
#include "overworld_feature.hpp"
#include "nether_feature.hpp"  // ── nether-2 ──

#include "ov/base/log.hpp"
#include "ov/worldgen/ore_feature.hpp"
#include "ov/worldgen/tree_feature.hpp"
#include "ov/worldgen/vegetation_feature.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace ov::worldgen {

Feature::~Feature() = default;

StateProvider::~StateProvider() = default;

std::vector<registry::BlockStateId> StateProvider::possible_states() const {
    return {};
}

std::expected<registry::BlockStateId, FeatureError> parse_block_state(
    Json node, const registry::BlockRegistry& blocks) {
    std::string_view name;
    if (node.at_key("Name").get(name) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const auto block = blocks.find_block(qualify(name));
    if (!block) {
        OV_LOG_ERROR("worldgen: a feature names unknown block {}", name);
        return std::unexpected(FeatureError::Malformed);
    }

    simdjson::dom::object properties;
    if (node.at_key("Properties").get(properties) != simdjson::SUCCESS) {
        // No properties means the block's *default* state, which is not always
        // its first: 484 of the 1003 blocks differ, and taking the first would
        // put upside-down slabs and unlit redstone ore into the world.
        return blocks.default_state(*block);
    }
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    for (auto [key, value] : properties) {
        std::string_view text;
        if (value.get(text) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        // A spring's `state` is a *fluid* state, not a block state, and fluid
        // states carry one property blocks do not: `falling`. The game converts
        // the fluid to its legacy block, and for a source fluid — which is what
        // every spring names — that conversion gives level 0 whether it is
        // falling or not. So the property is dropped here, and only this one:
        // anything else unknown is a datapack naming a state that does not
        // exist, and is refused.
        if (key == "falling" && !blocks.find_property(*block, key)) {
            continue;
        }
        pairs.emplace_back(key, text);
    }
    if (const auto state = blocks.state_for(*block, pairs)) {
        return *state;
    }
    OV_LOG_ERROR("worldgen: {} has no state matching the properties a feature asked for", name);
    return std::unexpected(FeatureError::Malformed);
}

namespace {

// ── Block state providers ───────────────────────────────────────────────────

class SimpleState final : public StateProvider {
public:
    explicit SimpleState(registry::BlockStateId state) : state_(state) {}
    [[nodiscard]] registry::BlockStateId state(const FeatureLevel&,
                                               FeatureRandom&,
                                               BlockPos) const override {
        return state_;
    }

    [[nodiscard]] std::vector<registry::BlockStateId> possible_states() const override {
        return {state_};
    }

private:
    registry::BlockStateId state_;
};

/// One of a weighted list, by a single `nextInt(total weight)`.
///
/// The draw happens whatever the world looks like, and it happens before any
/// survival test: a flower patch that lands on stone still spends it.
class WeightedState final : public StateProvider {
public:
    explicit WeightedState(std::vector<std::pair<registry::BlockStateId, i32>> entries)
        : entries_(std::move(entries)) {
        for (const auto& [state, weight] : entries_) {
            (void)state;
            total_ += weight;
        }
    }

    [[nodiscard]] registry::BlockStateId state(const FeatureLevel&, FeatureRandom& random,
                                               BlockPos) const override {
        i32 roll = random.next_int(total_);
        for (const auto& [state, weight] : entries_) {
            roll -= weight;
            if (roll < 0) {
                return state;
            }
        }
        return entries_.back().first;
    }

    [[nodiscard]] std::vector<registry::BlockStateId> possible_states() const override {
        std::vector<registry::BlockStateId> out;
        out.reserve(entries_.size());
        for (const auto& [state, weight] : entries_) {
            (void)weight;
            out.push_back(state);
        }
        return out;
    }

private:
    std::vector<std::pair<registry::BlockStateId, i32>> entries_;
    i32                                                 total_{0};
};

/// A source provider with one integer property re-rolled.
///
/// Two draws, and in this order: the source first, then the value. Reversing
/// them is invisible in the block that comes out and wrong in everything the
/// feature does afterwards.
class RandomizedIntState final : public StateProvider {
public:
    RandomizedIntState(StateProviderRef source, std::string property, IntProviderRef values,
                       const registry::BlockRegistry& blocks)
        : source_(std::move(source)),
          property_(std::move(property)),
          values_(std::move(values)),
          blocks_(&blocks) {}

    [[nodiscard]] registry::BlockStateId state(const FeatureLevel& level, FeatureRandom& random,
                                               BlockPos at) const override {
        const auto base  = source_->state(level, random, at);
        const auto value = values_->sample(random);
        const auto block = blocks_->block_of(base);
        const auto found = blocks_->find_property(block, property_);
        if (!found) {
            return base;
        }
        const auto text = std::to_string(value);
        for (usize index = 0; index < found->values.size(); ++index) {
            if (found->values[index] == text) {
                return blocks_->with_property(base, *found, static_cast<u16>(index));
            }
        }
        return base;
    }

    [[nodiscard]] std::vector<registry::BlockStateId> possible_states() const override {
        return source_->possible_states();
    }

private:
    StateProviderRef               source_;
    std::string                    property_;
    IntProviderRef                 values_;
    const registry::BlockRegistry* blocks_;
};

/// A block with its pillar axis drawn: `nextInt(3)` into x, y, z.
///
/// The order of the axes is the seed. `Direction.Axis.VALUES` is x, y, z, and
/// listing them any other way turns two thirds of a hay pile sideways.
class RotatedBlockState final : public StateProvider {
public:
    RotatedBlockState(registry::BlockStateId state, const registry::BlockRegistry& blocks)
        : state_(state), blocks_(&blocks) {}

    [[nodiscard]] registry::BlockStateId state(const FeatureLevel&, FeatureRandom& random,
                                               BlockPos) const override {
        static constexpr std::array<std::string_view, 3> kAxes{"x", "y", "z"};
        const auto axis  = kAxes[static_cast<usize>(random.next_int(3))];
        const auto block = blocks_->block_of(state_);
        const auto found = blocks_->find_property(block, "axis");
        if (!found) {
            return state_;
        }
        for (usize index = 0; index < found->values.size(); ++index) {
            if (found->values[index] == axis) {
                return blocks_->with_property(state_, *found, static_cast<u16>(index));
            }
        }
        return state_;
    }

    [[nodiscard]] std::vector<registry::BlockStateId> possible_states() const override {
        return {state_};
    }

private:
    registry::BlockStateId         state_;
    const registry::BlockRegistry* blocks_;
};

/// The first rule whose predicate holds, else the fallback.
///
/// This is what puts sandstone under a sand disk that would otherwise hang in
/// the air, and it is why a disk cannot be one block type.
class RuleBasedState final : public StateProvider {
public:
    RuleBasedState(std::vector<std::pair<BlockPredicateRef, StateProviderRef>> rules,
                   StateProviderRef fallback)
        : rules_(std::move(rules)), fallback_(std::move(fallback)) {}

    [[nodiscard]] registry::BlockStateId state(const FeatureLevel& level,
                                               FeatureRandom& random,
                                               BlockPos at) const override {
        for (const auto& [test, provider] : rules_) {
            if (test->test(level, at)) {
                return provider->state(level, random, at);
            }
        }
        return fallback_->state(level, random, at);
    }

    [[nodiscard]] std::vector<registry::BlockStateId> possible_states() const override {
        std::vector<registry::BlockStateId> out = fallback_->possible_states();
        for (const auto& [test, provider] : rules_) {
            (void)test;
            for (const auto state : provider->possible_states()) {
                out.push_back(state);
            }
        }
        return out;
    }

private:
    std::vector<std::pair<BlockPredicateRef, StateProviderRef>> rules_;
    StateProviderRef                                            fallback_;
};

}  // namespace

std::expected<StateProviderRef, FeatureError> parse_state_provider(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    std::string_view type;
    if (node.at_key("type").get(type) == simdjson::SUCCESS) {
        const std::string kind = strip_namespace(type);
        if (kind == "simple_state_provider") {
            auto field = node.at_key("state");
            if (field.error() != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            auto state = parse_block_state(field.value(), blocks);
            if (!state) {
                return std::unexpected(state.error());
            }
            return std::static_pointer_cast<const StateProvider>(
                std::make_shared<const SimpleState>(*state));
        }
        if (kind == "weighted_state_provider") {
            simdjson::dom::array entries;
            if (node.at_key("entries").get(entries) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            std::vector<std::pair<registry::BlockStateId, i32>> parsed;
            for (auto entry : entries) {
                auto data = entry.at_key("data");
                i64  weight = 1;
                if (data.error() != simdjson::SUCCESS ||
                    entry.at_key("weight").get(weight) != simdjson::SUCCESS) {
                    return std::unexpected(FeatureError::Malformed);
                }
                auto state = parse_block_state(data.value(), blocks);
                if (!state) {
                    return std::unexpected(state.error());
                }
                parsed.emplace_back(*state, static_cast<i32>(weight));
            }
            if (parsed.empty()) {
                return std::unexpected(FeatureError::Malformed);
            }
            return std::static_pointer_cast<const StateProvider>(
                std::make_shared<const WeightedState>(std::move(parsed)));
        }
        if (kind == "randomized_int_state_provider") {
            auto             source_field = node.at_key("source");
            auto             values_field = node.at_key("values");
            std::string_view property;
            if (source_field.error() != simdjson::SUCCESS ||
                values_field.error() != simdjson::SUCCESS ||
                node.at_key("property").get(property) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            auto source = parse_state_provider(source_field.value(), blocks, tags);
            if (!source) {
                return source;
            }
            auto values = parse_int_provider(values_field.value());
            if (!values) {
                return std::unexpected(values.error());
            }
            return std::static_pointer_cast<const StateProvider>(
                std::make_shared<const RandomizedIntState>(*source, std::string(property),
                                                           *values, blocks));
        }
        if (kind == "rotated_block_provider") {
            auto field = node.at_key("state");
            if (field.error() != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            auto state = parse_block_state(field.value(), blocks);
            if (!state) {
                return std::unexpected(state.error());
            }
            return std::static_pointer_cast<const StateProvider>(
                std::make_shared<const RotatedBlockState>(*state, blocks));
        }
        // The three noise-driven providers read a `NormalNoise` built over a
        // **legacy** random source, and noise.cpp only knows how to build one
        // over Xoroshiro. Implementing the legacy positional factory belongs to
        // that file rather than to this one, so they are refused by name; it
        // costs the three flower patches that vary with a noise field.
        //
        // `rotated_block_provider` belongs to the huge fungi, which are not
        // built here either.
        if (auto noise = parse_noise_state_provider(kind, node, blocks)) {  // features-2
            return std::move(*noise);
        }
        OV_LOG_ERROR("worldgen: block state provider '{}' is not implemented", kind);
        return std::unexpected(FeatureError::Unsupported);
    }

    // The rule-based form has no "type": it is a fallback and a list of rules.
    auto fallback_field = node.at_key("fallback");
    if (fallback_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto fallback = parse_state_provider(fallback_field.value(), blocks, tags);
    if (!fallback) {
        return fallback;
    }
    std::vector<std::pair<BlockPredicateRef, StateProviderRef>> rules;
    simdjson::dom::array                                        list;
    if (node.at_key("rules").get(list) == simdjson::SUCCESS) {
        for (auto rule : list) {
            auto when = rule.at_key("if_true");
            auto then = rule.at_key("then");
            if (when.error() != simdjson::SUCCESS || then.error() != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            auto test = parse_block_predicate(when.value(), blocks, tags);
            if (!test) {
                return std::unexpected(test.error());
            }
            auto provider = parse_state_provider(then.value(), blocks, tags);
            if (!provider) {
                return provider;
            }
            rules.emplace_back(*test, *provider);
        }
    }
    return std::static_pointer_cast<const StateProvider>(
        std::make_shared<const RuleBasedState>(std::move(rules), *fallback));
}

namespace {

// ── spring_feature ──────────────────────────────────────────────────────────

/// A single source block in a wall, where the shape of the rock around it is
/// exactly right.
///
/// The counts are equalities and not minima: `rock_count: 4` and
/// `hole_count: 1` means four solid sides and exactly one open one, so a
/// spring only ever appears where a cave meets stone on the other three sides.
/// Reading them as "at least" would put water down every cave wall.
class SpringFeature final : public Feature {
public:
    SpringFeature(registry::BlockStateId fluid, std::vector<u16> valid, bool requires_below,
                  i32 rock_count, i32 hole_count)
        : fluid_(fluid),
          valid_(std::move(valid)),
          requires_below_(requires_below),
          rock_count_(rock_count),
          hole_count_(hole_count) {}

    [[nodiscard]] std::string_view type_name() const override { return "spring_feature"; }

    bool place(const FeatureContext& context, FeatureLevel& level, FeatureRandom&,
               BlockPos at) const override {
        const auto& blocks = *context.blocks;
        const auto  valid  = [&](i32 x, i32 y, i32 z) {
            return std::ranges::binary_search(valid_, blocks.block_of(level.block_at(x, y, z)).value());
        };
        const auto air = [&](i32 x, i32 y, i32 z) {
            return blocks.is_air(blocks.block_of(level.block_at(x, y, z)));
        };

        if (!valid(at.x, at.y + 1, at.z)) {
            return false;
        }
        if (requires_below_ && !valid(at.x, at.y - 1, at.z)) {
            return false;
        }
        if (!air(at.x, at.y, at.z) && !valid(at.x, at.y, at.z)) {
            return false;
        }

        constexpr std::array<std::array<i32, 2>, 4> kSides{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
        i32                                         rock = 0;
        i32                                         hole = 0;
        for (const auto& side : kSides) {
            if (valid(at.x + side[0], at.y, at.z + side[1])) {
                ++rock;
            }
            if (air(at.x + side[0], at.y, at.z + side[1])) {
                ++hole;
            }
        }
        if (rock != rock_count_ || hole != hole_count_) {
            return false;
        }
        return level.set_block(at.x, at.y, at.z, fluid_);
    }

private:
    registry::BlockStateId fluid_;
    std::vector<u16>       valid_;
    bool                   requires_below_;
    i32                    rock_count_;
    i32                    hole_count_;
};

// ── disk ────────────────────────────────────────────────────────────────────

/// A flat circle of one block replacing another, a few blocks thick.
///
/// The radius is drawn once and the circle tested against its *square*, so the
/// disk is a proper circle rather than the diamond a Manhattan distance would
/// give. The columns run from the top down, which is what lets the rules see
/// what is under a block before they choose it.
class DiskFeature final : public Feature {
public:
    DiskFeature(IntProviderRef radius, i32 half_height, BlockPredicateRef target,
                StateProviderRef provider)
        : radius_(std::move(radius)),
          half_height_(half_height),
          target_(std::move(target)),
          provider_(std::move(provider)) {}

    [[nodiscard]] std::string_view type_name() const override { return "disk"; }

    bool place(const FeatureContext&, FeatureLevel& level, FeatureRandom& random,
               BlockPos at) const override {
        const i32 top    = at.y + half_height_;
        const i32 bottom = at.y - half_height_ - 1;
        const i32 radius = radius_->sample(random);
        bool      placed = false;
        for (i32 z = at.z - radius; z <= at.z + radius; ++z) {
            for (i32 x = at.x - radius; x <= at.x + radius; ++x) {
                const i32 dx = x - at.x;
                const i32 dz = z - at.z;
                if (dx * dx + dz * dz > radius * radius) {
                    continue;
                }
                // Exclusive at the bottom, so `half_height: 2` is five blocks
                // and not six.
                for (i32 y = top; y > bottom; --y) {
                    const BlockPos here{x, y, z};
                    if (!target_->test(level, here)) {
                        continue;
                    }
                    const auto state = provider_->state(level, random, here);
                    placed |= level.set_block(x, y, z, state);
                }
            }
        }
        return placed;
    }

private:
    IntProviderRef    radius_;
    i32               half_height_;
    BlockPredicateRef target_;
    StateProviderRef  provider_;
};

// ── Reading a configured feature ────────────────────────────────────────────

[[nodiscard]] std::expected<OreConfig, FeatureError> parse_ore_config(
    Json config, const registry::BlockRegistry& blocks, const BlockTags& tags) {
    OreConfig result;
    i64       size = 0;
    if (config.at_key("size").get(size) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    result.size = static_cast<i32>(size);
    f64 discard = 0.0;
    if (config.at_key("discard_chance_on_air_exposure").get(discard) == simdjson::SUCCESS) {
        result.discard_chance_on_air_exposure = static_cast<f32>(discard);
    }

    simdjson::dom::array targets;
    if (config.at_key("targets").get(targets) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    for (auto entry : targets) {
        OreTarget target;
        auto      state = entry.at_key("state");
        if (state.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto parsed = parse_block_state(state.value(), blocks);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        target.state = *parsed;

        auto             rule = entry.at_key("target");
        std::string_view rule_type;
        if (rule.error() != simdjson::SUCCESS ||
            rule.at_key("predicate_type").get(rule_type) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        const std::string kind = strip_namespace(rule_type);
        if (kind == "tag_match") {
            std::string_view tag;
            if (rule.at_key("tag").get(tag) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            if (!tags.known(tag)) {
                OV_LOG_ERROR("worldgen: an ore names tag {}, which was not exported", tag);
                return std::unexpected(FeatureError::Malformed);
            }
            for (usize index = 0; index < blocks.block_count(); ++index) {
                const auto block = static_cast<u16>(index);
                if (tags.contains(tag, registry::BlockId{block})) {
                    target.replaceable.push_back(block);
                }
            }
        } else if (kind == "block_match") {
            std::string_view name;
            if (rule.at_key("block").get(name) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            const auto block = blocks.find_block(qualify(name));
            if (!block) {
                return std::unexpected(FeatureError::Malformed);
            }
            target.replaceable.push_back(block->value());
        } else {
            // `blockstate_match` compares whole states and the two
            // `random_*_match` rules consume a draw. None appears in vanilla's
            // ore configurations, and guessing at the draw would move every
            // later feature in the chunk.
            OV_LOG_ERROR("worldgen: ore rule test '{}' is not implemented", kind);
            return std::unexpected(FeatureError::Unsupported);
        }
        std::ranges::sort(target.replaceable);
        result.targets.push_back(std::move(target));
    }
    return result;
}

}  // namespace

std::expected<FeatureRef, FeatureError> parse_feature(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags,
    const FeatureResolver& resolve) {
    std::string_view type;
    if (node.at_key("type").get(type) != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    const std::string kind = strip_namespace(type);
    auto              config = node.at_key("config");
    if (config.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }

    if (kind == "ore" || kind == "scattered_ore") {
        auto parsed = parse_ore_config(config.value(), blocks, tags);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        return kind == "ore" ? make_ore_feature(std::move(*parsed))
                             : make_scattered_ore_feature(std::move(*parsed));
    }
    if (kind == "spring_feature") {
        auto state = config.at_key("state");
        if (state.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto fluid = parse_block_state(state.value(), blocks);
        if (!fluid) {
            return std::unexpected(fluid.error());
        }
        std::vector<u16> valid;
        // A holder set: a list of names, or — the Nether's springs — one bare
        // name, which is the same set with a single member.
        std::string_view single_block;
        if (config.at_key("valid_blocks").get(single_block) == simdjson::SUCCESS) {
            const auto block = blocks.find_block(qualify(single_block));
            if (!block) {
                return std::unexpected(FeatureError::Malformed);
            }
            valid.push_back(block->value());
        } else {
            simdjson::dom::array valid_blocks;
            if (config.at_key("valid_blocks").get(valid_blocks) != simdjson::SUCCESS) {
                return std::unexpected(FeatureError::Malformed);
            }
            for (auto value : valid_blocks) {
                std::string_view name;
                if (value.get(name) != simdjson::SUCCESS) {
                    return std::unexpected(FeatureError::Malformed);
                }
                const auto block = blocks.find_block(qualify(name));
                if (!block) {
                    return std::unexpected(FeatureError::Malformed);
                }
                valid.push_back(block->value());
            }
        }
        std::ranges::sort(valid);
        bool requires_below = false;
        (void)config.at_key("requires_block_below").get(requires_below);
        i64 rock = 0;
        i64 hole = 0;
        if (config.at_key("rock_count").get(rock) != simdjson::SUCCESS ||
            config.at_key("hole_count").get(hole) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        return std::static_pointer_cast<const Feature>(std::make_shared<const SpringFeature>(
            *fluid, std::move(valid), requires_below, static_cast<i32>(rock),
            static_cast<i32>(hole)));
    }
    if (kind == "disk") {
        auto radius_field = config.at_key("radius");
        auto target_field = config.at_key("target");
        auto state_field  = config.at_key("state_provider");
        if (radius_field.error() != simdjson::SUCCESS ||
            target_field.error() != simdjson::SUCCESS ||
            state_field.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        auto radius = parse_int_provider(radius_field.value());
        if (!radius) return std::unexpected(radius.error());
        auto target = parse_block_predicate(target_field.value(), blocks, tags);
        if (!target) return std::unexpected(target.error());
        auto provider = parse_state_provider(state_field.value(), blocks, tags);
        if (!provider) return std::unexpected(provider.error());
        i64 half_height = 0;
        (void)config.at_key("half_height").get(half_height);
        return std::static_pointer_cast<const Feature>(std::make_shared<const DiskFeature>(
            *radius, static_cast<i32>(half_height), *target, *provider));
    }

    if (kind == "tree") {
        return parse_tree_feature(config.value(), blocks, tags);
    }
    if (is_vegetation_feature(kind)) {
        // Its own failures are already named by whatever refused them; saying
        // "random_patch is not implemented" on top of "no survival rule for
        // minecraft:fire" would be a second, false reason.
        return parse_vegetation_feature(kind, config.value(), blocks, tags, resolve);
    }
    // ── overworld features (features-2): one call, the families live elsewhere ──
    if (auto claimed = parse_overworld_feature(kind, config.value(), blocks, tags, resolve)) {
        return std::move(*claimed);
    }
    // ── end overworld features ──

    // ── end ── end_spike, end_island, chorus_plant, end_gateway.
    if (is_end_feature(kind)) {
        return parse_end_feature(kind, config.value(), blocks);
    }
    // ── nether-2 ── the Nether's nine types, in nether_feature.cpp
    if (auto claimed = parse_nether_feature(kind, config.value(), blocks, tags)) {
        return std::move(*claimed);
    }

    // Everything else. Lakes, geodes, the nether's vegetation: each with its
    // own algorithm and its own draws. Refused by name,
    // so that a world built on this framework is visibly missing them rather
    // than quietly containing an approximation of them.
    OV_LOG_ERROR("worldgen: feature type '{}' is not implemented", kind);
    return std::unexpected(FeatureError::Unsupported);
}

std::expected<std::shared_ptr<const PlacedFeature>, FeatureError> parse_inline_placed_feature(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags,
    const FeatureResolver& resolve) {
    // The whole thing may be one name: `trees_taiga`'s default is the string
    // "minecraft:spruce_checked", which is a placed feature in its own file.
    std::string_view whole;
    if (node.get(whole) == simdjson::SUCCESS) {
        return resolve.placed(whole);
    }
    auto feature_field = node.at_key("feature");
    if (feature_field.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    // The child may be held inline or named. `trees_plains` names both of its
    // two candidates, so a parser that only understood the inline form would
    // refuse every tree selector in the game.
    std::expected<FeatureRef, FeatureError> feature =
        std::unexpected(FeatureError::Malformed);
    std::string_view named;
    if (feature_field.get(named) == simdjson::SUCCESS) {
        feature = resolve.configured(named);
    } else {
        feature = parse_feature(feature_field.value(), blocks, tags, resolve);
    }
    if (!feature) {
        return std::unexpected(feature.error());
    }

    auto placed     = std::make_shared<PlacedFeature>();
    placed->feature = *feature;
    simdjson::dom::array modifiers;
    if (node.at_key("placement").get(modifiers) == simdjson::SUCCESS) {
        for (auto modifier : modifiers) {
            auto parsed = parse_placement_modifier(modifier, blocks, tags);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            placed->placement.push_back(*parsed);
        }
    }
    return std::static_pointer_cast<const PlacedFeature>(placed);
}

// ── The registry ────────────────────────────────────────────────────────────

struct FeatureRegistry::Impl {
    BlockTags                                             tags;
    std::unordered_map<std::string, FeatureRef>           configured;
    std::unordered_map<std::string, std::shared_ptr<const PlacedFeature>> placed;
    std::map<std::string, FeatureError>                   unavailable;
    usize                                                 configured_seen{0};
};

FeatureRegistry::FeatureRegistry() : impl_(std::make_unique<Impl>()) {}
FeatureRegistry::FeatureRegistry(FeatureRegistry&&) noexcept            = default;
FeatureRegistry& FeatureRegistry::operator=(FeatureRegistry&&) noexcept = default;
FeatureRegistry::~FeatureRegistry()                                     = default;

std::expected<FeatureRegistry, FeatureError> FeatureRegistry::load(
    const std::filesystem::path& data_root, const registry::BlockRegistry& blocks) {
    auto tags = BlockTags::load(data_root, blocks);
    if (!tags) {
        return std::unexpected(tags.error());
    }

    FeatureRegistry registry;
    Impl&           impl = *registry.impl_;
    impl.tags            = std::move(*tags);

    const auto configured_dir = data_root / "worldgen" / "configured_feature";
    const auto placed_dir     = data_root / "worldgen" / "placed_feature";
    if (!std::filesystem::is_directory(configured_dir) ||
        !std::filesystem::is_directory(placed_dir)) {
        OV_LOG_ERROR("worldgen: {} or {} is missing", configured_dir.string(),
                     placed_dir.string());
        return std::unexpected(FeatureError::Missing);
    }

    // Sorted, so that two runs read the same files in the same order and a
    // diagnostic list can be compared between them.
    const auto sorted_files = [](const std::filesystem::path& directory) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::ranges::sort(files);
        return files;
    };

    // simdjson parses in place: a document points into the padded string it
    // was read from, so both have to outlive every use of the document. One
    // parser *per file* rather than one reused, because the load is no longer a
    // single pass: a selector names a feature whose file has not been read yet,
    // so a document is opened while another is still being walked.
    struct Source {
        std::unique_ptr<simdjson::padded_string> text;
        std::unique_ptr<simdjson::dom::parser>   parser;
    };
    std::vector<Source> sources;
    const auto          read =
        [&](const std::filesystem::path& path) -> std::expected<Json, FeatureError> {
        auto text = simdjson::padded_string::load(path.string());
        if (text.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Missing);
        }
        Source source;
        source.text   = std::make_unique<simdjson::padded_string>(std::move(text.value()));
        source.parser = std::make_unique<simdjson::dom::parser>();
        auto document = source.parser->parse(*source.text);
        if (document.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        const Json result = document.value();
        sources.push_back(std::move(source));
        return result;
    };

    // The configured features, by name, read but not yet built.
    std::map<std::string, std::filesystem::path> configured_files;
    for (const auto& path : sorted_files(configured_dir)) {
        configured_files.emplace("minecraft:" + path.stem().string(), path);
    }
    impl.configured_seen = configured_files.size();

    // Building one may need another: `trees_plains` names `oak_bees_005` and
    // `fancy_oak_bees_005`, and neither is guaranteed to have been built yet.
    // So the walk is depth-first over the reference graph, with a set of names
    // currently on the stack so that a cycle in a datapack is a refusal rather
    // than a stack overflow.
    std::map<std::string, std::filesystem::path> placed_files;
    for (const auto& path : sorted_files(placed_dir)) {
        placed_files.emplace("minecraft:" + path.stem().string(), path);
    }

    // Two stacks, not one. `birch_tall` names `super_birch_bees_0002`, and
    // there is both a configured feature and a placed feature by that name:
    // resolving the placed one resolves the configured one, and a shared stack
    // reads that as a cycle and refuses the whole selector.
    std::set<std::string>               building_configured;
    std::set<std::string>               building_placed;
    std::map<std::string, FeatureError> failed_configured;
    std::map<std::string, FeatureError> failed_placed;
    FeatureResolver                     resolve;
    resolve.data_root = data_root;  // ── worldgen-3 ── the fossils' processor lists

    resolve.configured = [&](std::string_view raw) -> std::expected<FeatureRef, FeatureError> {
        const std::string name = qualify(raw);
        if (const auto found = impl.configured.find(name); found != impl.configured.end()) {
            return found->second;
        }
        if (const auto bad = failed_configured.find(name); bad != failed_configured.end()) {
            return std::unexpected(bad->second);
        }
        const auto file = configured_files.find(name);
        if (file == configured_files.end()) {
            OV_LOG_ERROR("worldgen: {} is named by a feature and has no file", name);
            failed_configured.emplace(name, FeatureError::Missing);
            return std::unexpected(FeatureError::Missing);
        }
        if (!building_configured.insert(name).second) {
            OV_LOG_ERROR("worldgen: {} takes part in a cycle of feature references", name);
            failed_configured.emplace(name, FeatureError::Malformed);
            return std::unexpected(FeatureError::Malformed);
        }
        auto                                    document = read(file->second);
        std::expected<FeatureRef, FeatureError> built =
            document ? parse_feature(*document, blocks, impl.tags, resolve)
                     : std::unexpected(document.error());
        building_configured.erase(name);
        if (!built) {
            // Named here, not only counted: the reason is logged by whatever
            // refused, and this line is what ties it to a file.
            OV_LOG_INFO("worldgen: configured feature {} did not load ({})", name,
                        to_string(built.error()));
            failed_configured.emplace(name, built.error());
            return built;
        }
        impl.configured.emplace(name, *built);
        return built;
    };

    resolve.placed = [&](std::string_view raw)
        -> std::expected<std::shared_ptr<const PlacedFeature>, FeatureError> {
        const std::string name = qualify(raw);
        if (const auto found = impl.placed.find(name); found != impl.placed.end()) {
            return found->second;
        }
        if (const auto bad = failed_placed.find(name); bad != failed_placed.end()) {
            return std::unexpected(bad->second);
        }
        const auto file = placed_files.find(name);
        if (file == placed_files.end()) {
            OV_LOG_ERROR("worldgen: placed feature {} is named and has no file", name);
            failed_placed.emplace(name, FeatureError::Missing);
            return std::unexpected(FeatureError::Missing);
        }
        if (!building_placed.insert(name).second) {
            OV_LOG_ERROR("worldgen: {} takes part in a cycle of placed references", name);
            failed_placed.emplace(name, FeatureError::Malformed);
            return std::unexpected(FeatureError::Malformed);
        }
        auto document = read(file->second);
        std::expected<std::shared_ptr<const PlacedFeature>, FeatureError> built =
            document ? parse_inline_placed_feature(*document, blocks, impl.tags, resolve)
                     : std::unexpected(document.error());
        building_placed.erase(name);
        if (!built) {
            failed_placed.emplace(name, built.error());
            return built;
        }
        // The name is what the `biome` modifier compares against, so it is set
        // here and not by the parser, which does not know it.
        const_cast<PlacedFeature&>(**built).name = name;
        impl.placed.emplace(name, *built);
        return built;
    };

    for (const auto& [name, path] : configured_files) {
        (void)path;
        (void)resolve.configured(name);
    }
    for (const auto& [name, path] : placed_files) {
        (void)path;
        (void)resolve.placed(name);
    }
    for (const auto& [name, error] : failed_configured) {
        impl.unavailable.emplace(name, error);
    }
    for (const auto& [name, error] : failed_placed) {
        impl.unavailable.emplace(name, error);
    }

    OV_LOG_INFO(
        "worldgen: {} of {} configured features, {} placed features, {} block tags; {} could not "
        "be built",
        impl.configured.size(), impl.configured_seen, impl.placed.size(), impl.tags.tag_count(),
        impl.unavailable.size());
    return registry;
}

const PlacedFeature* FeatureRegistry::placed(std::string_view name) const {
    const auto found = impl_->placed.find(qualify(name));
    return found == impl_->placed.end() ? nullptr : found->second.get();
}

const Feature* FeatureRegistry::configured(std::string_view name) const {
    const auto found = impl_->configured.find(qualify(name));
    return found == impl_->configured.end() ? nullptr : found->second.get();
}

std::expected<std::shared_ptr<const PlacedFeature>, FeatureError> FeatureRegistry::parse_placed(
    std::string_view json, std::string name, const registry::BlockRegistry& blocks) const {
    // ── worldgen-3 ── Resolution only looks up what `load` built: a probe's body
    // never names a file the registry has not read.
    FeatureResolver resolve;
    resolve.configured = [&](std::string_view raw) -> std::expected<FeatureRef, FeatureError> {
        const auto found = impl_->configured.find(qualify(raw));
        if (found == impl_->configured.end()) {
            return std::unexpected(FeatureError::Missing);
        }
        return found->second;
    };
    resolve.placed = [&](std::string_view raw)
        -> std::expected<std::shared_ptr<const PlacedFeature>, FeatureError> {
        const auto found = impl_->placed.find(qualify(raw));
        if (found == impl_->placed.end()) {
            return std::unexpected(FeatureError::Missing);
        }
        return found->second;
    };
    const simdjson::padded_string text{json};
    simdjson::dom::parser         parser;
    auto                          document = parser.parse(text);
    if (document.error() != simdjson::SUCCESS) {
        return std::unexpected(FeatureError::Malformed);
    }
    auto built = parse_inline_placed_feature(document.value(), blocks, impl_->tags, resolve);
    if (!built) {
        return built;
    }
    // A fresh object, so the name can be set without touching a shared one.
    auto named  = std::make_shared<PlacedFeature>(**built);
    named->name = std::move(name);
    return std::static_pointer_cast<const PlacedFeature>(named);
}

usize FeatureRegistry::placed_count() const noexcept {
    return impl_->placed.size();
}

usize FeatureRegistry::configured_count() const noexcept {
    return impl_->configured.size();
}

std::vector<std::pair<std::string, FeatureError>> FeatureRegistry::unavailable() const {
    return {impl_->unavailable.begin(), impl_->unavailable.end()};
}

usize FeatureRegistry::unsupported_count() const noexcept {
    return static_cast<usize>(std::ranges::count_if(impl_->unavailable, [](const auto& entry) {
        return entry.second == FeatureError::Unsupported;
    }));
}

const BlockTags& FeatureRegistry::tags() const noexcept {
    return impl_->tags;
}

}  // namespace ov::worldgen
