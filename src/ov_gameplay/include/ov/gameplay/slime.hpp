// ── mobs-2 ── A slime's size, and what a dying slime leaves behind.
//
// Sources: minecraft.wiki *Slime* — a natural slime is size 1, 2 or 4 (the
// `Size` tag is one less); its box is 0.52 × size on each side, its health
// size², its attack size and its speed 0.2 + 0.1 × size; a slime bigger than 1
// splits on death into 2 to 4 slimes of half its size. The division count is
// measured against a real server (docs/provenance/mobs-2.md § 5); the box of
// size 1 is the measured 0.5202 of normalized/entities.json.
#pragma once

#include "ov/base/types.hpp"
#include "ov/math/random.hpp"
#include "ov/math/vec.hpp"

#include <vector>

namespace ov::gameplay {

/// The side of a slime's box: 0.5202 at size 1 (measured) and proportional.
[[nodiscard]] constexpr f32 slime_side(i32 size) noexcept {
    return 0.5202F * static_cast<f32>(size);
}

/// Health at spawn: size².
[[nodiscard]] constexpr f32 slime_health(i32 size) noexcept {
    return static_cast<f32>(size * size);
}

/// The `movement_speed` attribute: 0.2 + 0.1 × size (0.3 at size 1, measured).
[[nodiscard]] constexpr f64 slime_speed(i32 size) noexcept {
    return 0.2 + 0.1 * static_cast<f64>(size);
}

/// A natural slime's size: one draw in three for the exponent, and a second
/// chance to grow scaled by the difficulty's special multiplier (0 on easy
/// and normal). Sizes 1, 2 or 4.
[[nodiscard]] i32 draw_slime_size(math::LegacyRandomSource& random, f32 special_multiplier);

struct SlimeChild {
    Vec3d offset{};
    i32   size{1};
};

/// What a dying slime of `size` leaves: nothing at size 1, otherwise
/// `2 + next_int(3)` slimes of half the size, on a two-by-two grid a quarter
/// of the parent's size apart.
void slime_children(i32 size, math::LegacyRandomSource& random, std::vector<SlimeChild>& out);

}  // namespace ov::gameplay
