// How fast a mob walks, from its `movement_speed` attribute and its goal.
//
// ── The law ─────────────────────────────────────────────────────────────────
//
// A mob does not walk *at* its speed. Its move control asks for a speed `s` —
// the attribute times the running goal's modifier — and uses that one number
// twice: as the forward input the body is given, and as the acceleration that
// input is scaled by. The input is then damped by 0.98 like every living
// entity's, and the ground's friction does the rest. On a floor of
// slipperiness `f` (0.6 for grass, stone, dirt — nearly everything):
//
//     a = 0.98 · s · s · (0.6³ / f³)      acceleration per tick
//     v = a / (1 − 0.91 · f)              displacement per tick, at cruise
//
// which is `v = 2.15859 · s²` on ordinary ground. The documentation gives the
// pieces — the 0.91 and 0.6 friction, the 0.98 input damping, the cubed ratio
// (minecraft.wiki, *Movement* and *Ice*) — and the measurement gives the
// quadratic: seven speeds in docs/provenance/elevage.md § 8 fitted 2.1586 to
// 0.5 %, and the campaign in docs/provenance/mobs-2.md measures every goal of
// every species we ship against it.
//
// The halving mobs.md used (`v = s / 2`) is this curve's tangent near 0.23,
// which is why it was exact for the zombie it was fitted on and 16 % fast for a
// strolling cow.
#pragma once

#include "ov/base/types.hpp"

namespace ov::gameplay {

/// Slipperiness of an ordinary block: grass, dirt, stone. Documented.
inline constexpr f64 kDefaultSlipperiness = 0.6;

/// The input damping every living entity applies to its forward input, each
/// tick. Documented; it is what turns `1 / 0.454 = 2.2026` into the measured
/// 2.1586.
inline constexpr f64 kInputDamping = 0.98;

/// Blocks per tick at cruise, for a speed `s` (attribute × goal modifier) on a
/// floor of slipperiness `slipperiness`.
[[nodiscard]] constexpr f64 walk_blocks_per_tick_on(f64 s, f64 slipperiness) noexcept {
    const f64 ratio = kDefaultSlipperiness / slipperiness;
    const f64 accel = kInputDamping * s * s * ratio * ratio * ratio;
    return accel / (1.0 - 0.91 * slipperiness);
}

/// The coefficient of the law on ordinary ground: 0.98 / (1 − 0.546).
inline constexpr f64 kWalkLaw = kInputDamping / (1.0 - 0.91 * kDefaultSlipperiness);

/// Blocks per tick at cruise on ordinary ground: `2.15859 · s²`.
[[nodiscard]] constexpr f64 walk_blocks_per_tick(f64 attribute_times_modifier) noexcept {
    return kWalkLaw * attribute_times_modifier * attribute_times_modifier;
}

}  // namespace ov::gameplay
