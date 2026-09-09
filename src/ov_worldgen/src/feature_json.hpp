// The parsing side of the feature layer, kept out of the public headers.
//
// simdjson is a template-heavy header and this module's rule is the project's:
// it appears in .cpp files and never in an installed one. Everything that
// turns a JSON node into a placement modifier, a provider or a predicate is
// declared here and implemented across placement.cpp, feature.cpp and
// ore_feature.cpp.
#pragma once

#include "ov/worldgen/feature.hpp"

#include <simdjson.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ov::worldgen {

using Json = simdjson::dom::element;

/// "minecraft:ore_iron" -> "ore_iron". The namespace is always minecraft here;
/// it is stripped rather than checked because a datapack may add its own and
/// the path is built from the rest either way.
[[nodiscard]] std::string strip_namespace(std::string_view name);

/// "ore_iron" -> "minecraft:ore_iron", and a name that already has a namespace
/// is left alone. Feature lists in biomes and the keys of the registries have
/// to agree on one spelling or the lookups miss silently.
[[nodiscard]] std::string qualify(std::string_view name);

[[nodiscard]] std::expected<VerticalAnchor, FeatureError> parse_anchor(Json node);
[[nodiscard]] std::expected<IntProviderRef, FeatureError> parse_int_provider(Json node);
[[nodiscard]] std::expected<HeightProviderRef, FeatureError> parse_height_provider(Json node);
[[nodiscard]] std::expected<BlockPredicateRef, FeatureError> parse_block_predicate(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags);
[[nodiscard]] std::expected<PlacementModifierRef, FeatureError> parse_placement_modifier(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags);

/// A block state written the way the datapack writes one: a name and an
/// optional map of properties. Missing properties keep the block's defaults,
/// which is why this cannot be a plain name lookup.
[[nodiscard]] std::expected<registry::BlockStateId, FeatureError> parse_block_state(
    Json node, const registry::BlockRegistry& blocks);

/// Which state to write, as `simple_state_provider` and the weighted and
/// randomised forms spell one out. Shared: a tree's five providers and a
/// patch's `to_place` are the same grammar.
[[nodiscard]] std::expected<StateProviderRef, FeatureError> parse_state_provider(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags);

/// How a nested feature *name* is turned into a built feature.
///
/// The selectors do not always hold their children: `trees_plains` names
/// `minecraft:oak_bees_005`, and that file may not have been read yet. So the
/// registry hands the parser a resolver that reads and builds a configured
/// feature on demand, which turns the load into a depth-first walk of the
/// reference graph rather than one pass over a sorted directory.
struct FeatureResolver {
    /// A configured feature by name, built on demand.
    std::function<std::expected<FeatureRef, FeatureError>(std::string_view)> configured;
    /// A *placed* feature by name, likewise. `trees_taiga` names
    /// `minecraft:spruce_checked`, which is a placed feature with its own
    /// placement, so the two registries have to be walked together.
    std::function<std::expected<std::shared_ptr<const PlacedFeature>, FeatureError>(
        std::string_view)>
        placed;
};

/// One configured feature, by its `type` and `config`. Recursive: a selector
/// holds or names features, and a patch holds a whole placed feature inline.
[[nodiscard]] std::expected<FeatureRef, FeatureError> parse_feature(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags,
    const FeatureResolver& resolve);

/// A placed feature written out where a feature is expected — the shape
/// `random_patch` and the selectors use. Its `feature` may be held inline or
/// named, and a name goes through `resolve`.
[[nodiscard]] std::expected<std::shared_ptr<const PlacedFeature>, FeatureError>
parse_inline_placed_feature(Json node, const registry::BlockRegistry& blocks,
                            const BlockTags& tags, const FeatureResolver& resolve);

/// `would_survive`, from vegetation_feature.cpp.
///
/// It asks whether a block could stand at a position, which is a gameplay
/// question — so the answer comes from the named survival table there rather
/// than from ov_gameplay, which is a layer above this one. Every state vanilla
/// asks about is a sapling, and a sapling's rule is one line: the block below
/// is in `#minecraft:dirt`, or is farmland. A state whose rule is not in the
/// table is refused by name.
[[nodiscard]] std::expected<BlockPredicateRef, FeatureError> parse_survival_predicate(
    Json node, const registry::BlockRegistry& blocks, const BlockTags& tags);

/// `tree`, from tree_feature.cpp. Takes the `config` object, not the wrapper.
[[nodiscard]] std::expected<FeatureRef, FeatureError> parse_tree_feature(
    Json config, const registry::BlockRegistry& blocks, const BlockTags& tags);

/// Whether `kind` is one this layer claims. Separates "we do not know this
/// type" from "we know it and something inside it was refused", which are
/// different facts and used to be logged as the same one.
[[nodiscard]] bool is_vegetation_feature(std::string_view kind) noexcept;

/// The vegetation family, from vegetation_feature.cpp: `simple_block`,
/// `random_patch`, `flower`, `no_bonemeal_flower`, `block_pile`,
/// `block_column`, and the three selectors. `kind` is the type with its
/// namespace stripped; an unknown one is `Unsupported` and nothing else is.
[[nodiscard]] std::expected<FeatureRef, FeatureError> parse_vegetation_feature(
    std::string_view kind, Json config, const registry::BlockRegistry& blocks,
    const BlockTags& tags, const FeatureResolver& resolve);

}  // namespace ov::worldgen
