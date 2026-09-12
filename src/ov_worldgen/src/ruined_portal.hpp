#pragma once

// ── portals ── The ruined portal's height and its cold test.
//
// The height is decided when the start is generated, from the noise alone: the
// game stores a portal's real y already at the `structure_starts` status, before
// any block exists. The ranges are minecraft.wiki's *Ruined Portal* page; the
// rest — the column the surface is read in, "first occupied" rather than "first
// free", the floor 15 blocks above the world's bottom rather than at y 15, and
// the settling down the four corners — is what the 25 overworld starts of the
// reference worlds agree on, all 25 (docs/provenance/structures.md, § portals).

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/worldgen/structure.hpp"
#include "ov/worldgen/structure_template.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace ov::worldgen {

/// Below this, a portal whose setup can be cold is cold: the snow threshold.
inline constexpr f32 kPortalColdTemperature = 0.15F;

/// The lowest a portal settles, above the world's bottom: -49 in the overworld.
inline constexpr i32 kPortalFloorAboveBottom = 15;

/// Where a ruined portal stands: the y of its box's bottom.
///
/// `box` is the template's box as placed at y 0 (rotation and mirror applied).
/// `min_y` is the dimension's bottom: -64 in the overworld, 0 in the Nether.
/// Draws from `random` right after the mirror, and only what its placement
/// needs — nothing for a portal on the surface or the sea bed.
///
/// Then, from that y down to `min_y + 15`, the first y where at least three of
/// the box's four corner columns hold something: anything but air, or, on the
/// sea bed, something solid.
[[nodiscard]] std::expected<i32, std::string> ruined_portal_height(
    const StructureWorldSampler& sampler, std::string_view placement, bool air_pocket,
    const BoundingBox& box, i32 min_y, math::LegacyRandomSource& random);

/// Whether the portal whose template origin is `origin` (settled) is cold: the
/// temperature there below 0.15. At the origin and not the box's middle — an
/// underground portal of reference-1234567890 (chunk 3817, 2053) is cold in the
/// snowy taiga at its origin and would not be in the cave biome at its middle.
/// An error when the sampler cannot give a temperature.
[[nodiscard]] std::expected<bool, std::string> ruined_portal_cold(
    const StructureWorldSampler& sampler, BlockPos origin);

}  // namespace ov::worldgen
