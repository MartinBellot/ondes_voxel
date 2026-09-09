#define OV_LOG_CATEGORY "worldgen"

#include "ov/worldgen/feature.hpp"

#include "feature_json.hpp"

#include "ov/base/log.hpp"
#include "ov/worldgen/ore_feature.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <unordered_map>

namespace ov::worldgen {

Feature::~Feature() = default;

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

/// Which state to write at a position. Only the two forms the features
/// implemented here use; the rest are refused by name.
class StateProvider {
public:
    StateProvider()                                = default;
    StateProvider(const StateProvider&)            = delete;
    StateProvider& operator=(const StateProvider&) = delete;
    virtual ~StateProvider()                       = default;

    [[nodiscard]] virtual registry::BlockStateId state(const FeatureLevel& level,
                                                       FeatureRandom& random,
                                                       BlockPos at) const = 0;
};

using StateProviderRef = std::shared_ptr<const StateProvider>;

class SimpleState final : public StateProvider {
public:
    explicit SimpleState(registry::BlockStateId state) : state_(state) {}
    [[nodiscard]] registry::BlockStateId state(const FeatureLevel&,
                                               FeatureRandom&,
                                               BlockPos) const override {
        return state_;
    }

private:
    registry::BlockStateId state_;
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

private:
    std::vector<std::pair<BlockPredicateRef, StateProviderRef>> rules_;
    StateProviderRef                                            fallback_;
};

[[nodiscard]] std::expected<StateProviderRef, FeatureError> parse_state_provider(
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
        // `weighted_state_provider`, `noise_provider`, `randomized_int_state_provider`
        // and the rest belong to the vegetation features, which are not built
        // here. Named rather than approximated.
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

[[nodiscard]] std::expected<FeatureRef, FeatureError> parse_feature(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags) {
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
        simdjson::dom::array valid_blocks;
        if (config.at_key("valid_blocks").get(valid_blocks) != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        std::vector<u16> valid;
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

    // Everything else. Trees, vegetation, lakes, geodes, the end's islands:
    // 49 more types, each with its own algorithm and its own draws. Refused by
    // name, so that a world built on this framework is visibly missing them
    // rather than quietly containing an approximation of them.
    return std::unexpected(FeatureError::Unsupported);
}

}  // namespace

// ── The registry ────────────────────────────────────────────────────────────

struct FeatureRegistry::Impl {
    BlockTags                                             tags;
    std::unordered_map<std::string, FeatureRef>           configured;
    std::unordered_map<std::string, PlacedFeature>        placed;
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
    // was read from, so the string has to outlive every use of the document.
    // One parser is reused — each document is consumed before the next parse —
    // but the strings are kept until the whole load is over.
    simdjson::dom::parser                                 parser;
    std::vector<std::unique_ptr<simdjson::padded_string>> sources;
    const auto read = [&](const std::filesystem::path& path)
        -> std::expected<Json, FeatureError> {
        auto text = simdjson::padded_string::load(path.string());
        if (text.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Missing);
        }
        sources.push_back(std::make_unique<simdjson::padded_string>(std::move(text.value())));
        auto document = parser.parse(*sources.back());
        if (document.error() != simdjson::SUCCESS) {
            return std::unexpected(FeatureError::Malformed);
        }
        return document.value();
    };

    for (const auto& path : sorted_files(configured_dir)) {
        const std::string name = "minecraft:" + path.stem().string();
        ++impl.configured_seen;
        auto document = read(path);
        if (!document) {
            impl.unavailable.emplace(name, document.error());
            continue;
        }
        auto feature = parse_feature(*document, blocks, impl.tags);
        if (!feature) {
            impl.unavailable.emplace(name, feature.error());
            continue;
        }
        impl.configured.emplace(name, *feature);
    }

    for (const auto& path : sorted_files(placed_dir)) {
        const std::string name = "minecraft:" + path.stem().string();
        auto              document = read(path);
        if (!document) {
            impl.unavailable.emplace(name, document.error());
            continue;
        }
        std::string_view feature_name;
        if (document->at_key("feature").get(feature_name) != simdjson::SUCCESS) {
            // An inline configured feature is legal in the format and does not
            // occur in vanilla's own data; refused rather than half-read.
            impl.unavailable.emplace(name, FeatureError::Unsupported);
            continue;
        }
        const auto found = impl.configured.find(qualify(feature_name));
        if (found == impl.configured.end()) {
            // Its configured feature did not load. Not an error of its own —
            // it is the same gap counted once more — but it must not become a
            // placed feature that places nothing.
            impl.unavailable.emplace(name, FeatureError::Unsupported);
            continue;
        }

        PlacedFeature placed;
        placed.name    = name;
        placed.feature = found->second;
        simdjson::dom::array modifiers;
        if (document->at_key("placement").get(modifiers) != simdjson::SUCCESS) {
            impl.unavailable.emplace(name, FeatureError::Malformed);
            continue;
        }
        std::optional<FeatureError> failure;
        for (auto modifier : modifiers) {
            auto parsed = parse_placement_modifier(modifier, blocks, impl.tags);
            if (!parsed) {
                failure = parsed.error();
                break;
            }
            placed.placement.push_back(*parsed);
        }
        if (failure) {
            impl.unavailable.emplace(name, *failure);
            continue;
        }
        impl.placed.emplace(name, std::move(placed));
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
    return found == impl_->placed.end() ? nullptr : &found->second;
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
