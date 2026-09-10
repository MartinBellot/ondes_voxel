// ── mobs-2 ── A zombie that drowns becomes a drowned; a husk, a zombie.
//
// minecraft.wiki *Zombie* § Drowned conversion: a zombie whose eyes stay in
// water for 30 seconds starts to convert, shakes for 15 seconds, and becomes a
// drowned; a husk does the same and becomes a zombie. The counter restarts
// when the eyes leave the water before the 30 seconds are up; once started,
// the conversion finishes wherever the mob goes. The timing is measured
// against a real server (docs/provenance/mobs-2.md § 6.1).
#pragma once

#include "ov/base/types.hpp"

namespace ov::gameplay {

inline constexpr i32 kDrownStartTicks   = 600;
inline constexpr i32 kDrownConvertTicks = 300;

struct DrowningState {
    /// Ticks with the eyes in water; -1 when they are out.
    i32 in_water{-1};
    /// Ticks left of the conversion; -1 when not converting.
    i32 converting{-1};
};

enum class DrowningEvent : u8 { None, Started, Converted };

/// One tick. `Started` on the tick the shaking begins, `Converted` on the
/// tick the mob must be replaced.
[[nodiscard]] DrowningEvent drowning_tick(DrowningState& state, bool eyes_in_water) noexcept;

}  // namespace ov::gameplay
