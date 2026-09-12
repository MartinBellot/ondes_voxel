// How hard a current pushes whatever is in it.
//
// The direction was already known — FluidRules::flow_vector, measured against
// the real server (docs/provenance/fluides.md). This file adds the magnitude:
// the flows of every block the box is in are made unit length, a block the
// entity is barely in counts for its depth, the sum is averaged, and the result
// is scaled by the fluid's strength (0.014 a tick for water). Something nearly
// still is never pushed by less than a floor, so a current always starts it
// moving. The figures are fitted on armor stands and items in a flowing channel
// on a real 1.20.1 server (scripts/measure_block_motion.py,
// docs/provenance/physique-blocs.md).
#pragma once

#include "ov/gameplay/fluid.hpp"
#include "ov/math/aabb.hpp"
#include "ov/math/vec.hpp"

namespace ov::gameplay {

struct CurrentConstants {
    /// Strength, blocks per tick per tick, before the entity's own drag.
    f64 water{0.014};
    f64 lava{0.0023333333333333335};
    f64 lava_ultrawarm{0.007};
    /// A push weaker than this on an entity slower than `still` on both
    /// horizontal axes is raised to it.
    f64 min_push{0.0045};
    f64 still{0.003};
    /// Below this depth a block's flow counts for the depth itself.
    f64 shallow{0.4};
};

/// The velocity a current adds this tick to an entity with `box`, in `kind`.
///
/// A player keeps the averaged vector; anything else is pushed at full
/// strength in its direction. Zero when no block of that fluid reaches the box
/// or the flows cancel out.
[[nodiscard]] Vec3d current_push(const FluidRules& rules, const world::LevelView& level,
                                 const AABB& box, FluidKind kind, const Vec3d& velocity,
                                 bool player, f64 strength, const CurrentConstants& constants = {});

}  // namespace ov::gameplay
