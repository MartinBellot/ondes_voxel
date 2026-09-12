// ── mobs-4 ── What a mob's body makes of an effect, and what an effect makes
// of its walk.
//
// Two rules the status effects already had for a player and a cow, now
// needed for every mob this server brings to life:
//
//   * **the body.** The undead take instant health as harm and instant
//     damage as healing and refuse regeneration and poison; arthropods refuse
//     poison (effects.hpp, measured on a zombie, a skeleton and a spider). The
//     list of which species are which is the wiki's *Undead* and *Arthropod*
//     mob categories; the three measured species are in it.
//
//   * **the walk.** The game's walk law is `v = 2.15859 · s²` on ordinary
//     ground, `s` the `movement_speed` attribute times the running goal's
//     modifier (walk_speed.hpp, mobs-2.md § 1). Speed and Slowness act on the
//     attribute (a `multiply` modifier, effets.md § 3), so a mob whose
//     attribute is `k` times its base walks `k²` times as fast under any goal.
#pragma once

#include "ov/base/types.hpp"
#include "ov/gameplay/effects.hpp"

#include <string_view>

namespace ov::gameplay {

/// The body of a species, by registry name. Ordinary for anything the two
/// lists do not name.
[[nodiscard]] EffectTarget::Body effect_body(std::string_view type_name) noexcept;

/// How much faster than its base a mob walks when its `movement_speed` is
/// `value` for a base of `base`: `(value / base)²`, and 1 without a base.
[[nodiscard]] constexpr f64 walk_factor(f64 value, f64 base) noexcept {
    if (base <= 0.0) {
        return 1.0;
    }
    const f64 k = value / base;
    return k * k;
}

}  // namespace ov::gameplay
