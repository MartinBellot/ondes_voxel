// The parsing side of the feature layer, kept out of the public headers.
//
// simdjson is a template-heavy header and this module's rule is the project's:
// it appears in .cpp files and never in an installed one. Everything that
// turns a JSON node into a placement modifier, a provider or a predicate is
// declared here and implemented across placement.cpp, feature.cpp and
// ore_feature.cpp.
#pragma once

#include "ov/worldgen/placement.hpp"

#include <simdjson.h>

#include <expected>
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

}  // namespace ov::worldgen
