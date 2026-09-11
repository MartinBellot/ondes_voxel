// ── nether-2 ── The Nether's own feature types.
//
// Nine configured-feature types that only the Nether's biomes name:
// `glowstone_blob`, `weeping_vines`, `twisting_vines`,
// `nether_forest_vegetation`, `huge_fungus`, `basalt_columns`,
// `basalt_pillar`, `delta_feature` and `netherrack_replace_blobs`. Claimed
// through one call in feature.cpp, like the End's and the overworld's
// families, so that the three never touch the same lines.
//
// Private to the module, like feature_json.hpp: it takes simdjson nodes.
#pragma once

#include "overworld_feature.hpp"

#include "ov/worldgen/nether_feature.hpp"

namespace ov::worldgen {

/// Nothing when `kind` is not one of the nine; otherwise the built feature or
/// the reason it was refused.
[[nodiscard]] ClaimedFeature parse_nether_feature(std::string_view kind, Json config,
                                                  const registry::BlockRegistry& blocks,
                                                  const BlockTags&               tags);

}  // namespace ov::worldgen
