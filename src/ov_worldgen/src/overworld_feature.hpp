// The overworld's feature types that are not trees, ores or plant patches.
//
// Each family lives in its own file — huge_mushroom_feature.cpp,
// ocean_feature.cpp, ice_feature.cpp, cave_feature.cpp, terrain_feature.cpp —
// and is claimed through one dispatcher, so that feature.cpp gains a single
// call and the nether work, which writes its own families in parallel, never
// has to touch the same lines.
//
// Private to the module, like feature_json.hpp: everything here takes simdjson
// nodes.
#pragma once

#include "feature_json.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace ov::worldgen {

/// The result of offering a feature type to a family: nothing when the family
/// does not claim the type, otherwise the built feature or the reason it was
/// refused.
using ClaimedFeature = std::optional<std::expected<FeatureRef, FeatureError>>;

/// Every family below, tried in turn.
[[nodiscard]] ClaimedFeature parse_overworld_feature(std::string_view kind, Json config,
                                                     const registry::BlockRegistry& blocks,
                                                     const BlockTags&               tags,
                                                     const FeatureResolver&         resolve);

// ── The families ────────────────────────────────────────────────────────────

/// `huge_brown_mushroom`, `huge_red_mushroom`.
[[nodiscard]] ClaimedFeature parse_huge_mushroom_feature(std::string_view kind, Json config,
                                                         const registry::BlockRegistry& blocks,
                                                         const BlockTags&               tags);

/// `seagrass`, `kelp`, `sea_pickle`, `coral_tree`, `coral_claw`,
/// `coral_mushroom`, `underwater_magma`.
[[nodiscard]] ClaimedFeature parse_ocean_feature(std::string_view kind, Json config,
                                                 const registry::BlockRegistry& blocks,
                                                 const BlockTags&               tags);

/// `iceberg`, `blue_ice`, `ice_spike`.
[[nodiscard]] ClaimedFeature parse_ice_feature(std::string_view kind, Json config,
                                               const registry::BlockRegistry& blocks,
                                               const BlockTags&               tags);

/// The caves: `geode`, `pointed_dripstone`, `dripstone_cluster`,
/// `large_dripstone`, `vegetation_patch`, `waterlogged_vegetation_patch`,
/// `root_system`, `multiface_growth`, `sculk_patch`.
[[nodiscard]] ClaimedFeature parse_cave_feature(std::string_view kind, Json config,
                                                const registry::BlockRegistry& blocks,
                                                const BlockTags&               tags,
                                                const FeatureResolver&         resolve);

/// `vegetation_patch`, `waterlogged_vegetation_patch`, `multiface_growth` —
/// called from the cave family, in lush_feature.cpp.
[[nodiscard]] ClaimedFeature parse_lush_feature(std::string_view kind, Json config,
                                                const registry::BlockRegistry& blocks,
                                                const BlockTags&               tags,
                                                const FeatureResolver&         resolve);

/// `root_system` — called from the cave family, in root_system_feature.cpp.
[[nodiscard]] ClaimedFeature parse_root_system_feature(std::string_view kind, Json config,
                                                       const registry::BlockRegistry& blocks,
                                                       const BlockTags&               tags,
                                                       const FeatureResolver&         resolve);

/// `pointed_dripstone`, `dripstone_cluster`, `large_dripstone` — called from the
/// cave family, in dripstone_feature.cpp.
[[nodiscard]] ClaimedFeature parse_dripstone_feature(std::string_view kind, Json config,
                                                     const registry::BlockRegistry& blocks,
                                                     const BlockTags&               tags);

/// The rest of the surface: `lake`, `forest_rock`, `bamboo`, `vines`,
/// `desert_well`, `monster_room`, `freeze_top_layer`, `bonus_chest`.
[[nodiscard]] ClaimedFeature parse_terrain_feature(std::string_view kind, Json config,
                                                   const registry::BlockRegistry& blocks,
                                                   const BlockTags&               tags);

/// The block predicates `solid` and `replaceable`, or nothing for another
/// type. Called from the one hook in placement.cpp.
///
///   * `replaceable` is `BlockState.canBeReplaced()`, read from the vanilla
///     `#minecraft:replaceable` tag, which lists exactly the blocks that carry
///     the property;
///   * `solid` is `BlockState.isSolid()`: a collision shape whose bounds
///     average at least 0.7291666 of a block, or are a full block tall. The
///     per-block overrides the game applies on top of the shape
///     (`forceSolidOn` / `forceSolidOff`) are not in any report and are not
///     modelled — named here, measured in docs/provenance/features.md.
[[nodiscard]] std::optional<std::expected<BlockPredicateRef, FeatureError>>
parse_shape_predicate(std::string_view kind, Json node, const registry::BlockRegistry& blocks,
                      const BlockTags& tags);

/// ── worldgen-3 ── `fossil`, in fossil_feature.cpp: two structure templates
/// from the server jar, placed through their processor lists.
[[nodiscard]] ClaimedFeature parse_fossil_feature(std::string_view kind, Json config,
                                                  const registry::BlockRegistry& blocks,
                                                  const BlockTags&               tags,
                                                  const FeatureResolver&         resolve);

/// ── worldgen-3 ── `freeze_top_layer`, in freeze_feature.cpp — called from
/// the terrain family.
[[nodiscard]] ClaimedFeature parse_freeze_feature(std::string_view kind,
                                                  const registry::BlockRegistry& blocks,
                                                  const BlockTags&               tags);

/// `bamboo` — called from the terrain family, in bamboo_feature.cpp.
[[nodiscard]] ClaimedFeature parse_bamboo_feature(std::string_view kind, Json config,
                                                  const registry::BlockRegistry& blocks,
                                                  const BlockTags&               tags);

/// `vines`, `forest_rock`, `lake`, `ice_spike` — called from the terrain
/// family, in surface_feature.cpp.
[[nodiscard]] ClaimedFeature parse_surface_feature(std::string_view kind, Json config,
                                                   const registry::BlockRegistry& blocks,
                                                   const BlockTags&               tags);

/// `noise_threshold_count` and `noise_based_count`, which read
/// `Biome.BIOME_INFO_NOISE`; nothing for another type. In biome_info_noise.cpp,
/// called from the one hook in placement.cpp.
[[nodiscard]] std::optional<std::expected<PlacementModifierRef, FeatureError>>
parse_noise_placement(std::string_view kind, Json node);

/// `noise_provider`, `noise_threshold_provider`, `dual_noise_provider`; nothing
/// for another type. In noise_state_provider.cpp, called from the one hook in
/// feature.cpp's `parse_state_provider`.
[[nodiscard]] std::optional<std::expected<StateProviderRef, FeatureError>>
parse_noise_state_provider(std::string_view kind, Json node, const registry::BlockRegistry& blocks);

// ── Shared questions about a block ──────────────────────────────────────────

/// `BlockState.isSolid()`, by the shape rule described at
/// `parse_shape_predicate`.
[[nodiscard]] bool is_solid(const registry::BlockRegistry& blocks,
                            registry::BlockStateId          state) noexcept;

/// `BlockState.isSolidRender`: a full, opaque cube.
///
/// Answered from two things the registry measured — sky light stops, and every
/// face is sturdy — rather than from a shape the game computes. Leaves and
/// glass have sturdy faces but let light through, so both come out false,
/// which is the game's answer for them.
[[nodiscard]] bool is_solid_render(const registry::BlockRegistry& blocks,
                                   registry::BlockStateId          state) noexcept;

/// All the blocks of a tag, sorted, or a refusal naming the tag.
[[nodiscard]] std::expected<std::vector<u16>, FeatureError> tag_members(
    const registry::BlockRegistry& blocks, const BlockTags& tags, std::string_view tag);

/// One block by name, or a refusal naming it.
[[nodiscard]] std::expected<registry::BlockId, FeatureError> named_block(
    const registry::BlockRegistry& blocks, std::string_view name);

/// Set one named property on a state; a block without it is left alone, which
/// is `BlockState.trySetValue`.
[[nodiscard]] registry::BlockStateId with_property_value(const registry::BlockRegistry& blocks,
                                                         registry::BlockStateId          state,
                                                         std::string_view                name,
                                                         std::string_view value) noexcept;

[[nodiscard]] inline bool holds(const std::vector<u16>& set, registry::BlockId block) noexcept {
    return std::binary_search(set.begin(), set.end(), block.value());
}

}  // namespace ov::worldgen
